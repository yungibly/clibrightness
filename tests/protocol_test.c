#include "protocol.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static void checksum(uint8_t b[BR_REPLY_SIZE]) {
    b[10] = 0x50;
    for (size_t i = 0; i < 10; ++i)
        b[10] ^= b[i];
}

static void rejects(const uint8_t *b, size_t size) {
    brightness_value value = {123, 456};
    assert(brightness_reply(b, size, &value) != NULL);
    assert(value.current == 123 && value.maximum == 456);
}

int main(void) {
    uint8_t request[BR_REQUEST_SIZE];
    brightness_request(request);
    const uint8_t expected[] = {0x82, 0x01, 0x10, 0xac};
    assert(memcmp(request, expected, sizeof(expected)) == 0);

    const struct { unsigned value; uint8_t packet[BR_SET_SIZE]; } golden[] = {
        {0,   {0x84, 0x03, 0x10, 0x00, 0x00, 0xa8}},
        {21,  {0x84, 0x03, 0x10, 0x00, 0x15, 0xbd}},
        {255, {0x84, 0x03, 0x10, 0x00, 0xff, 0x57}},
        {256, {0x84, 0x03, 0x10, 0x01, 0x00, 0xa9}},
        {400, {0x84, 0x03, 0x10, 0x01, 0x90, 0x39}}
    };
    uint8_t set_packet[BR_SET_SIZE];
    for (size_t i = 0; i < sizeof(golden) / sizeof(golden[0]); ++i) {
        assert(brightness_set_request(golden[i].value, set_packet));
        assert(memcmp(set_packet, golden[i].packet, BR_SET_SIZE) == 0);
    }
    for (unsigned v = 0; v <= 400; ++v) {
        assert(brightness_set_request(v, set_packet));
        assert(set_packet[0] == 0x84 && set_packet[1] == 0x03 && set_packet[2] == 0x10);
        assert(((unsigned)set_packet[3] * 256 + set_packet[4]) == v);
        uint8_t total = 0x6e ^ 0x51;
        for (size_t i = 0; i < BR_SET_SIZE; ++i) total ^= set_packet[i];
        assert(total == 0);
        char text[16];
        snprintf(text, sizeof(text), "%u", v);
        unsigned parsed = UINT_MAX;
        assert(brightness_parse_value(text, &parsed) && parsed == v);
    }
    uint8_t unchanged[BR_SET_SIZE];
    memset(unchanged, 0x55, sizeof(unchanged));
    memcpy(set_packet, unchanged, sizeof(set_packet));
    assert(!brightness_set_request(401, set_packet));
    assert(!brightness_set_request(UINT_MAX, set_packet));
    assert(!brightness_set_request(10, NULL));
    assert(memcmp(set_packet, unchanged, sizeof(set_packet)) == 0);
    const char *invalid_input[] = {
        "", "-1", "+1", "401", "4294967296", "99999999999999999999999999",
        "10x", "10.0", "0x10", "4e2", " 10", "10 ", "10\n", "\t10"
    };
    for (size_t i = 0; i < sizeof(invalid_input) / sizeof(invalid_input[0]); ++i) {
        unsigned parsed = 12345;
        assert(!brightness_parse_value(invalid_input[i], &parsed));
        assert(parsed == 12345);
    }
    unsigned parsed = 12345;
    assert(!brightness_parse_value(NULL, &parsed));
    assert(!brightness_parse_value("10", NULL));
    assert(brightness_parse_value("020", &parsed) && parsed == 20);

    /* Independently specified full reply: raw 10 of 200, checksum 0x66. */
    const uint8_t fixture[BR_REPLY_SIZE] =
        {0x6e, 0x88, 0x02, 0x00, 0x10, 0x00, 0x00, 0xc8, 0x00, 0x0a, 0x66};
    brightness_value value = {0};
    assert(brightness_reply(fixture, sizeof(fixture), &value) == NULL);
    assert(value.current == 10 && value.maximum == 200);

    /* Actual validated reply from the user's PA249CGV: raw 10, maximum 400. */
    const uint8_t hardware_fixture[BR_REPLY_SIZE] =
        {0x6e, 0x88, 0x02, 0x00, 0x10, 0x00, 0x01, 0x90, 0x00, 0x0a, 0x3f};
    assert(brightness_reply(hardware_fixture, sizeof(hardware_fixture), &value) == NULL);
    assert(value.current == 10 && value.maximum == 400);

    uint8_t b[BR_REPLY_SIZE + 1] = {0};
    rejects(b, BR_REPLY_SIZE); /* An all-zero buffer is not brightness zero. */
    memset(b, 0xff, sizeof(b));
    rejects(b, BR_REPLY_SIZE);
    rejects(NULL, BR_REPLY_SIZE);
    assert(brightness_reply(fixture, BR_REPLY_SIZE, NULL) != NULL);
    memcpy(b, fixture, BR_REPLY_SIZE);
    for (size_t size = 0; size <= BR_REPLY_SIZE + 1; ++size)
        if (size != BR_REPLY_SIZE) rejects(b, size);

    /* Any single-bit corruption anywhere in a valid reply must be rejected. */
    for (size_t byte = 0; byte < BR_REPLY_SIZE; ++byte) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            memcpy(b, fixture, BR_REPLY_SIZE);
            b[byte] ^= (uint8_t)(1u << bit);
            rejects(b, BR_REPLY_SIZE);
        }
    }

    /* Well-checksummed but semantically wrong replies must also be rejected. */
    const struct { size_t index; uint8_t byte; } invalid[] = {
        {0, 0x6f}, {1, 0x80}, {1, 0x87}, {2, 0x03}, {3, 0x01},
        {3, 0x02}, {4, 0x12}, {5, 0x01}, {7, 0}, {9, 201}
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        memcpy(b, fixture, BR_REPLY_SIZE);
        b[invalid[i].index] = invalid[i].byte;
        checksum(b);
        rejects(b, BR_REPLY_SIZE);
    }

    /* Cover the entire requested range without clamping or percent scaling. */
    for (unsigned current = 0; current <= 400; ++current) {
        memcpy(b, hardware_fixture, BR_REPLY_SIZE);
        b[8] = (uint8_t)(current >> 8);
        b[9] = (uint8_t)(current & 0xff);
        checksum(b);
        assert(brightness_reply(b, BR_REPLY_SIZE, &value) == NULL);
        assert(value.current == current && value.maximum == 400);
    }
    /* Diagnostics must faithfully expose other raw ranges too. */
    memcpy(b, fixture, BR_REPLY_SIZE);
    b[6] = 0x03; b[7] = 0xe8; /* 1000 */
    b[8] = 0x01; b[9] = 0x2c; /* 300 */
    checksum(b);
    assert(brightness_reply(b, BR_REPLY_SIZE, &value) == NULL);
    assert(value.current == 300 && value.maximum == 1000);
    puts("protocol tests passed (offline; no hardware APIs linked)");
    return 0;
}
