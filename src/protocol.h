#ifndef CLIBRIGHTNESS_PROTOCOL_H
#define CLIBRIGHTNESS_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

enum { BR_REQUEST_SIZE = 4, BR_SET_SIZE = 6, BR_REPLY_SIZE = 11, BR_MAXIMUM = 400 };

typedef struct {
    uint16_t current;
    uint16_t maximum;
} brightness_value;

/* Fixed Get VCP Feature request for brightness. */
void brightness_request(uint8_t out[BR_REQUEST_SIZE]);
/* Only brightness 0..400; leaves output unchanged on rejection. */
bool brightness_set_request(unsigned value, uint8_t out[BR_SET_SIZE]);
bool brightness_parse_value(const char *text, unsigned *out);

/* NULL means success. On error, *out is unchanged. Values are raw, not percent. */
const char *brightness_reply(const uint8_t *bytes, size_t size,
                             brightness_value *out);

#endif
