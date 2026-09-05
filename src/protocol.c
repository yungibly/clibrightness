#include "protocol.h"

#include <string.h>

void brightness_request(uint8_t out[BR_REQUEST_SIZE]) {
    /* IOAVService supplies source address 0x51 separately. Checksum includes it:
       0x6e ^ 0x51 ^ 0x82 ^ 0x01 ^ 0x10 == 0xac.
       Confirmed in the supplied ASUS ARM64 library; see INVESTIGATION.md. */
    static const uint8_t request[BR_REQUEST_SIZE] = {0x82, 0x01, 0x10, 0xac};
    memcpy(out, request, sizeof(request));
}

bool brightness_parse_value(const char *text, unsigned *out) {
    if (!text || !*text || !out) return false;
    unsigned value = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        /* value was <= 400 on the previous iteration: no integer overflow. */
        value = value * 10 + (unsigned)(*p - '0');
        if (value > BR_MAXIMUM) return false;
    }
    *out = value;
    return true;
}

bool brightness_set_request(unsigned value, uint8_t out[BR_SET_SIZE]) {
    if (value > BR_MAXIMUM || !out) return false;
    uint8_t request[BR_SET_SIZE] =
        {0x84, 0x03, 0x10, (uint8_t)(value >> 8), (uint8_t)(value & 0xff), 0};
    request[5] = 0x6e ^ 0x51;
    for (size_t i = 0; i < BR_SET_SIZE - 1; ++i) request[5] ^= request[i];
    memcpy(out, request, sizeof(request));
    return true;
}

const char *brightness_reply(const uint8_t *b, size_t size,
                             brightness_value *out) {
    if (!b || !out || size != BR_REPLY_SIZE)
        return "brightness reply must contain exactly 11 bytes";
    if (b[0] != 0x6e)
        return "invalid reply source; no brightness value obtained";
    if (b[1] == 0x80)
        return "monitor returned a null reply; no brightness value obtained";
    if (b[1] != 0x88)
        return "invalid brightness reply length/header";
    uint8_t checksum = 0x50;
    for (size_t i = 0; i < size; ++i)
        checksum ^= b[i];
    if (checksum != 0)
        return "invalid brightness reply checksum";
    if (b[2] != 0x02 || b[4] != 0x10)
        return "reply is not for the brightness query";
    if (b[3] == 0x01)
        return "monitor reports brightness VCP 0x10 is unsupported";
    if (b[3] != 0)
        return "monitor returned an unknown result code";
    if (b[5] != 0)
        return "monitor did not report brightness as a continuous control";
    uint16_t maximum = (uint16_t)((uint16_t)b[6] << 8 | b[7]);
    uint16_t current = (uint16_t)((uint16_t)b[8] << 8 | b[9]);
    if (maximum == 0 || current > maximum)
        return "monitor reported inconsistent brightness current/maximum values";
    *out = (brightness_value){current, maximum};
    return NULL;
}
