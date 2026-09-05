#include "controller.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

/* Private macOS APIs, also used by the ASUS reference library. Weak imports
   allow an explicit error if a future OS removes them. No ASUS code is linked. */
extern CFTypeRef IOAVServiceCreateWithService(CFAllocatorRef, io_service_t)
    __attribute__((weak_import));
extern IOReturn IOAVServiceWriteI2C(CFTypeRef, uint32_t, uint32_t, void *, uint32_t)
    __attribute__((weak_import));
extern IOReturn IOAVServiceReadI2C(CFTypeRef, uint32_t, uint32_t, void *, uint32_t)
    __attribute__((weak_import));

typedef struct {
    io_service_t framebuffer;
    io_service_t proxy;
    uint64_t framebuffer_id;
    uint64_t proxy_id;
    char serial[128];
    char route[128];
} target;

static bool string_is(CFTypeRef value, CFStringRef expected) {
    return value && CFGetTypeID(value) == CFStringGetTypeID() &&
           CFEqual(value, expected);
}

static bool number_is(CFTypeRef value, int64_t expected) {
    int64_t actual = 0;
    return value && CFGetTypeID(value) == CFNumberGetTypeID() &&
           CFNumberGetValue(value, kCFNumberSInt64Type, &actual) && actual == expected;
}

static bool property_is(io_service_t entry, CFStringRef key, CFStringRef expected) {
    CFTypeRef value = IORegistryEntryCreateCFProperty(entry, key, NULL, 0);
    bool equal = string_is(value, expected);
    if (value) CFRelease(value);
    return equal;
}

static bool zero_property(io_service_t entry, CFStringRef key) {
    CFTypeRef value = IORegistryEntryCreateCFProperty(entry, key, NULL, 0);
    bool equal = number_is(value, 0);
    if (value) CFRelease(value);
    return equal;
}

static void release_target(target *t) {
    if (t->framebuffer) IOObjectRelease(t->framebuffer);
    if (t->proxy) IOObjectRelease(t->proxy);
    memset(t, 0, sizeof(*t));
}

/* 0: another display; 1: expected identity; -1: incomplete/unexpected identity. */
static int framebuffer_identity(io_service_t entry, char serial[128]) {
    CFTypeRef attrs = IORegistryEntryCreateCFProperty(entry, CFSTR("DisplayAttributes"), NULL, 0);
    if (!attrs) return 0;
    int result = 0;
    if (CFGetTypeID(attrs) != CFDictionaryGetTypeID()) goto done;
    CFTypeRef product = CFDictionaryGetValue(attrs, CFSTR("ProductAttributes"));
    if (!product || CFGetTypeID(product) != CFDictionaryGetTypeID()) goto done;
    if (!string_is(CFDictionaryGetValue(product, CFSTR("ProductName")), CFSTR("PA249CGV")))
        goto done;
    result = -1;
    /* Vendor/product IDs observed in macOS's cached identity for this model. */
    if (!string_is(CFDictionaryGetValue(product, CFSTR("ManufacturerID")), CFSTR("AUS")) ||
        !number_is(CFDictionaryGetValue(product, CFSTR("LegacyManufacturerID")), 0x06b3) ||
        !number_is(CFDictionaryGetValue(product, CFSTR("ProductID")), 0xaa36)) goto done;
    CFTypeRef sn = CFDictionaryGetValue(product, CFSTR("AlphanumericSerialNumber"));
    if (!sn || CFGetTypeID(sn) != CFStringGetTypeID() ||
        !CFStringGetCString(sn, serial, 128, kCFStringEncodingASCII) || !serial[0]) goto done;
    for (const unsigned char *p = (const unsigned char *)serial; *p; ++p)
        if (*p < 33 || *p > 126) goto done;
    result = 1;
done:
    CFRelease(attrs);
    return result;
}

static const char *find_target(target *t) {
    memset(t, 0, sizeof(*t));
    io_iterator_t iterator = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault,
            IOServiceMatching("IOMobileFramebuffer"), &iterator) != KERN_SUCCESS)
        return "cannot enumerate display framebuffers";
    const char *error = NULL;
    io_service_t entry;
    while ((entry = IOIteratorNext(iterator))) {
        char serial[128] = {0};
        int match = framebuffer_identity(entry, serial);
        if (match < 0 || (match && t->framebuffer)) {
            error = match < 0 ? "PA249CGV identity is incomplete or unexpected"
                              : "multiple PA249CGV displays found; refusing ambiguous target";
            IOObjectRelease(entry);
            break;
        }
        if (match) {
            t->framebuffer = entry;
            memcpy(t->serial, serial, sizeof(t->serial));
        } else IOObjectRelease(entry);
    }
    if (!IOIteratorIsValid(iterator)) error = "display registry changed during enumeration";
    IOObjectRelease(iterator);
    if (error) goto fail;
    if (!t->framebuffer) { error = "no uniquely identified ASUS PA249CGV found"; goto fail; }

    io_registry_entry_t parent = IO_OBJECT_NULL;
    if (IORegistryEntryGetParentEntry(t->framebuffer, kIOServicePlane, &parent) != KERN_SUCCESS) {
        error = "cannot identify the display's transport route"; goto fail;
    }
    io_name_t name = {0};
    kern_return_t status = IORegistryEntryGetName(parent, name);
    IOObjectRelease(parent);
    char *address = strchr(name, '@');
    if (address) *address = '\0';
    /* Deliberately limited to the observed native USB-C DisplayPort topology. */
    if (status != KERN_SUCCESS || strncmp(name, "dispext", 7) || !name[7]) {
        error = "unrecognized USB-C display route; no fallback will be attempted"; goto fail;
    }
    for (size_t i = 7; name[i]; ++i) {
        if (name[i] < '0' || name[i] > '9') {
            error = "invalid display route identifier"; goto fail;
        }
    }
    int n = snprintf(t->route, sizeof(t->route), "%s:dcpav-service-epic:0", name);
    if (n < 0 || (size_t)n >= sizeof(t->route)) { error = "display route too long"; goto fail; }
    if (IOServiceGetMatchingServices(kIOMainPortDefault,
            IOServiceMatching("DCPAVServiceProxy"), &iterator) != KERN_SUCCESS) {
        error = "cannot enumerate display transport services"; goto fail;
    }
    while ((entry = IOIteratorNext(iterator))) {
        parent = IO_OBJECT_NULL;
        bool match = false;
        if (IORegistryEntryGetParentEntry(entry, kIOServicePlane, &parent) == KERN_SUCCESS) {
            io_name_t endpoint = {0};
            match = IORegistryEntryGetName(parent, endpoint) == KERN_SUCCESS &&
                    !strcmp(endpoint, t->route) &&
                    property_is(parent, CFSTR("EPICName"), CFSTR("dcpav-service-epic")) &&
                    property_is(parent, CFSTR("EPICProviderClass"), CFSTR("DCPDP13Service")) &&
                    property_is(parent, CFSTR("EPICLocation"), CFSTR("External")) &&
                    zero_property(parent, CFSTR("EPICUnit")) &&
                    property_is(entry, CFSTR("Location"), CFSTR("External")) &&
                    zero_property(entry, CFSTR("Unit"));
            IOObjectRelease(parent);
        }
        if (match && t->proxy) {
            error = "multiple transports match the target; refusing ambiguous route";
            IOObjectRelease(entry);
            break;
        }
        if (match) t->proxy = entry;
        else IOObjectRelease(entry);
    }
    if (!IOIteratorIsValid(iterator)) error = "transport registry changed during enumeration";
    IOObjectRelease(iterator);
    if (error) goto fail;
    if (!t->proxy) { error = "no uniquely matched native USB-C transport found"; goto fail; }
    if (IORegistryEntryGetRegistryEntryID(t->framebuffer, &t->framebuffer_id) != KERN_SUCCESS ||
        IORegistryEntryGetRegistryEntryID(t->proxy, &t->proxy_id) != KERN_SUCCESS) {
        error = "cannot obtain stable registry entry identities"; goto fail;
    }
    return NULL;
fail:
    release_target(t);
    return error;
}

static int transaction_lock(void) {
    char directory[1024], path[1200];
    size_t size = confstr(_CS_DARWIN_USER_TEMP_DIR, directory, sizeof(directory));
    if (!size || size > sizeof(directory)) return -1;
    int n = snprintf(path, sizeof(path), "%sclibrightness-%u.lock", directory, (unsigned)getuid());
    if (n < 0 || (size_t)n >= sizeof(path)) return -1;
    int fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != getuid() ||
        st.st_nlink != 1 || (st.st_mode & 0777) != 0600 || flock(fd, LOCK_EX | LOCK_NB)) {
        close(fd); return -1;
    }
    return fd;
}

static void print_bytes(const char *label, const uint8_t *bytes, size_t size) {
    fprintf(stderr, "%s", label);
    for (size_t i = 0; i < size; ++i) fprintf(stderr, " %02x", (unsigned)bytes[i]);
    fputc('\n', stderr);
}

static void identify(const target *t) {
    fprintf(stderr, "Target: ASUS PA249CGV, serial %s\n", t->serial);
    fprintf(stderr, "Route: %s (framebuffer 0x%" PRIx64 ", proxy 0x%" PRIx64 ")\n",
            t->route, t->framebuffer_id, t->proxy_id);
}

typedef struct {
    target *target;
    CFTypeRef service;
    bool diagnostic;
} native_context;

static const char *check_identity(void *opaque) {
    native_context *context = opaque;
    target current;
    const char *error = find_target(&current);
    target *original = context->target;
    if (!error && (original->framebuffer_id != current.framebuffer_id ||
                   original->proxy_id != current.proxy_id ||
                   strcmp(original->serial, current.serial)))
        error = "display identity changed during the operation";
    release_target(&current);
    return error;
}

static const char *read_brightness(void *opaque, uint8_t reply[BR_REPLY_SIZE]) {
    native_context *context = opaque;
    uint8_t request[BR_REQUEST_SIZE];
    brightness_request(request);
    if (context->diagnostic) print_bytes("Get request:", request, sizeof(request));
    usleep(50000);
    IOReturn status = IOAVServiceWriteI2C(context->service, 0x37, 0x51, request, BR_REQUEST_SIZE);
    if (status != kIOReturnSuccess) {
        fprintf(stderr, "DDC query error: 0x%08x\n", (unsigned)status);
        return "brightness query request failed";
    }
    usleep(50000);
    status = IOAVServiceReadI2C(context->service, 0x37, 0, reply, BR_REPLY_SIZE);
    if (context->diagnostic) print_bytes("Get reply:", reply, BR_REPLY_SIZE);
    if (status != kIOReturnSuccess) {
        fprintf(stderr, "DDC read error: 0x%08x\n", (unsigned)status);
        return "brightness reply read failed";
    }
    return NULL;
}

static const char *write_brightness(void *opaque, unsigned value) {
    native_context *context = opaque;
    uint8_t request[BR_SET_SIZE];
    /* Defense at the actual write boundary as well as in argument parsing and
       the controller. The builder cannot address any feature except 0x10. */
    if (!brightness_set_request(value, request)) return "invalid brightness value";
    if (context->diagnostic) print_bytes("Set brightness request:", request, sizeof(request));
    usleep(50000);
    IOReturn status = IOAVServiceWriteI2C(context->service, 0x37, 0x51, request, BR_SET_SIZE);
    if (status != kIOReturnSuccess) {
        fprintf(stderr, "DDC setting error: 0x%08x\n", (unsigned)status);
        return "brightness setting request failed";
    }
    return NULL;
}

static void settle_brightness(void *opaque) {
    (void)opaque;
    usleep(200000);
}

int main(int argc, char **argv) {
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "--help"))) {
        puts("Usage: clibrightness identify | get [--diagnose] | set <0-400> [--diagnose]\n"
             "  identify       Show target using cached macOS metadata; no DDC request.\n"
             "  get            Print brightness as current/maximum in monitor units.\n"
             "  set <0-400>    Set brightness after validated read; verify with readback.\n"
             "  --diagnose     Also print identity and exact request/reply bytes.\n"
             "Only ASUS PA249CGV on the supported native USB-C route is accepted.");
        return 0;
    }
    bool metadata_only = argc == 2 && !strcmp(argv[1], "identify");
    bool setting = !strcmp(argv[1], "set");
    int base_count = setting ? 3 : 2;
    bool diagnostic = argc == base_count + 1 && !strcmp(argv[argc - 1], "--diagnose");
    bool valid_command = setting || !strcmp(argv[1], "get");
    unsigned requested = 0;
    if (!metadata_only && (!valid_command || (argc != base_count && !diagnostic))) {
        fputs("error: expected identify, get [--diagnose], or set <0-400> [--diagnose]\n", stderr);
        return 2;
    }
    if (setting && !brightness_parse_value(argv[2], &requested)) {
        fputs("error: brightness must be a complete decimal integer from 0 to 400\n", stderr);
        return 2;
    }
    target t;
    const char *error = find_target(&t);
    if (error) { fprintf(stderr, "error: %s\n", error); return 1; }
    if (metadata_only || diagnostic) identify(&t);
    if (metadata_only) { release_target(&t); return 0; }
    if (!IOAVServiceCreateWithService || !IOAVServiceWriteI2C || !IOAVServiceReadI2C) {
        fputs("error: required macOS display APIs are unavailable\n", stderr);
        release_target(&t); return 1;
    }
    int lock = transaction_lock();
    if (lock < 0) {
        fputs("error: cannot obtain exclusive transaction lock\n", stderr);
        release_target(&t); return 1;
    }
    CFTypeRef service = IOAVServiceCreateWithService(kCFAllocatorDefault, t.proxy);
    if (!service) {
        fputs("error: macOS refused access to the matched display service; no DDC request sent\n", stderr);
        close(lock); release_target(&t); return 1;
    }
    native_context context = {&t, service, diagnostic};
    brightness_io io = {&context, check_identity, read_brightness, write_brightness, settle_brightness};
    brightness_value value = {0};
    brightness_change change = {0};
    if (setting) {
        error = brightness_set(&io, requested, &change);
        value = change.after;
        if (!error && diagnostic)
            fprintf(stderr, "Verified: %u -> %u (maximum %u)%s\n",
                    (unsigned)change.before.current, (unsigned)value.current,
                    (unsigned)value.maximum, change.write_attempted ? "" : "; already at requested value, no setting write");
    } else error = brightness_get(&io, &value);
    if (error) {
        fprintf(stderr, "error: %s\n", error);
        if (change.write_attempted && !change.verified)
            fputs("The setting may have been applied, but is unverified. No setting retry or rollback was sent. Check the OSD.\n", stderr);
    } else printf("%u/%u\n", (unsigned)value.current, (unsigned)value.maximum);
    CFRelease(service);
    close(lock);
    release_target(&t);
    return error ? 1 : 0;
}
