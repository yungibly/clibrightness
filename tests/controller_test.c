#include "controller.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    unsigned identity_checks, reads, writes, settles;
    unsigned fail_identity_at, fail_read_at, fail_read_from, incomplete_read_at;
    bool corrupt_before, corrupt_after, fail_write, stale_after, change_range;
    unsigned current, maximum, written_value;
} fake_monitor;

static const char *fake_identity(void *context) {
    fake_monitor *m = context;
    ++m->identity_checks;
    return m->identity_checks == m->fail_identity_at ? "identity mismatch" : NULL;
}

static const char *fake_read(void *context, uint8_t reply[BR_REPLY_SIZE]) {
    fake_monitor *m = context;
    ++m->reads;
    if (m->reads == m->fail_read_at || (m->fail_read_from && m->reads >= m->fail_read_from))
        return "read failed";
    if (m->reads == m->incomplete_read_at) {
        /* Actual malformed reply observed immediately after the first write. */
        const uint8_t incomplete[BR_REPLY_SIZE] = {0x01, 0x10, 0xac, 0, 0, 0, 0, 0, 0, 0, 0};
        memcpy(reply, incomplete, sizeof(incomplete));
        return NULL;
    }
    uint8_t b[BR_REPLY_SIZE] = {
        0x6e, 0x88, 0x02, 0x00, 0x10, 0x00,
        (uint8_t)(m->maximum >> 8), (uint8_t)(m->maximum & 0xff),
        (uint8_t)(m->current >> 8), (uint8_t)(m->current & 0xff), 0x50
    };
    for (size_t i = 0; i < 10; ++i) b[10] ^= b[i];
    if ((m->reads == 1 && m->corrupt_before) || (m->reads >= 2 && m->corrupt_after))
        b[10] ^= 1;
    memcpy(reply, b, sizeof(b));
    return NULL;
}

static const char *fake_write(void *context, unsigned value) {
    fake_monitor *m = context;
    ++m->writes;
    assert(m->identity_checks == 2 && m->reads == 1);
    assert(value <= 400 && m->maximum == 400);
    m->written_value = value;
    if (!m->stale_after) m->current = value;
    if (m->change_range) m->maximum = 200;
    /* Simulate even the ambiguous case: setting applied but transport errored. */
    return m->fail_write ? "write failed" : NULL;
}

static void fake_settle(void *context) {
    fake_monitor *m = context;
    ++m->settles;
    assert(m->writes == 1);
}

static brightness_io io_for(fake_monitor *m) {
    return (brightness_io){m, fake_identity, fake_read, fake_write, fake_settle};
}

static fake_monitor fresh(void) {
    return (fake_monitor){.current = 20, .maximum = 400};
}

static void fails_without_setting(fake_monitor *m, unsigned request) {
    brightness_io io = io_for(m);
    brightness_change change;
    assert(brightness_set(&io, request, &change) != NULL);
    assert(!change.write_attempted && !change.verified && m->writes == 0);
}

int main(void) {
    fake_monitor m = fresh();
    fails_without_setting(&m, 401);
    assert(m.identity_checks == 0 && m.reads == 0);
    fails_without_setting(&m, UINT_MAX);
    assert(m.identity_checks == 0 && m.reads == 0);

    m = fresh(); m.fail_identity_at = 1;
    fails_without_setting(&m, 21);
    assert(m.reads == 0);
    m = fresh(); m.fail_identity_at = 2;
    fails_without_setting(&m, 21);
    assert(m.reads == 1);
    m = fresh(); m.fail_read_at = 1;
    fails_without_setting(&m, 21);
    m = fresh(); m.corrupt_before = true;
    fails_without_setting(&m, 21);
    const unsigned bad_maxima[] = {0, 100, 200, 399, 401, 65535};
    for (size_t i = 0; i < sizeof(bad_maxima) / sizeof(bad_maxima[0]); ++i) {
        m = fresh(); m.maximum = bad_maxima[i];
        fails_without_setting(&m, 21);
    }
    m = fresh(); m.current = 401;
    fails_without_setting(&m, 21);

    for (unsigned request = 0; request <= 400; ++request) {
        m = fresh();
        brightness_io io = io_for(&m);
        brightness_change change;
        assert(brightness_set(&io, request, &change) == NULL);
        assert(change.verified && change.before.current == 20);
        assert(change.after.current == request && change.after.maximum == 400);
        assert(m.writes == (request == 20 ? 0u : 1u));
        assert(change.write_attempted == (request != 20));
        assert(m.reads == (request == 20 ? 1u : 2u));
        assert(m.settles == (request == 20 ? 0u : 1u));
        if (request != 20) assert(m.written_value == request);
    }

    /* Failures after an attempted write never cause retry or rollback. */
    for (unsigned failure = 0; failure < 6; ++failure) {
        m = fresh();
        switch (failure) {
            case 0: m.fail_write = true; break;
            case 1: m.fail_read_from = 2; break;
            case 2: m.corrupt_after = true; break;
            case 3: m.stale_after = true; break;
            case 4: m.change_range = true; break;
            case 5: m.fail_identity_at = 3; break;
        }
        brightness_io io = io_for(&m);
        brightness_change change;
        assert(brightness_set(&io, 21, &change) != NULL);
        assert(change.write_attempted && !change.verified && m.writes == 1);
        assert(m.reads <= 4 && m.identity_checks <= 5 && m.settles <= 3);
        if (failure == 1 || failure == 2 || failure == 3)
            assert(m.reads == 4 && m.settles == 3);
        if (failure == 4) assert(m.reads == 2 && m.settles == 1);
        if (failure == 5) assert(m.reads == 1 && m.settles == 1);
    }

    /* Recover from transient read errors or the observed malformed response,
       while retaining exactly one setting write. */
    for (unsigned failure = 0; failure < 2; ++failure) {
        m = fresh();
        if (failure == 0) m.fail_read_at = 2;
        else m.incomplete_read_at = 2;
        brightness_io io = io_for(&m);
        brightness_change change;
        assert(brightness_set(&io, 21, &change) == NULL);
        assert(change.verified && change.after.current == 21);
        assert(m.writes == 1 && m.reads == 3 && m.settles == 2);
    }

    /* Plain get has no setting callback, including on malformed replies. */
    m = fresh();
    brightness_io io = io_for(&m);
    brightness_value value = {123, 456};
    assert(brightness_get(&io, &value) == NULL);
    assert(value.current == 20 && value.maximum == 400 && m.writes == 0);
    m = fresh(); m.corrupt_before = true;
    value = (brightness_value){123, 456};
    assert(brightness_get(&io, &value) != NULL);
    assert(value.current == 123 && value.maximum == 456 && m.writes == 0);
    puts("controller tests passed (offline fake monitor; all 401 values and failure paths)");
    return 0;
}
