#ifndef CLIBRIGHTNESS_CONTROLLER_H
#define CLIBRIGHTNESS_CONTROLLER_H

#include "protocol.h"

/* Callbacks return NULL on success. The only write callback takes a brightness
   value, never an arbitrary feature code, address, or packet. */
typedef struct {
    void *context;
    const char *(*check_identity)(void *context);
    const char *(*read)(void *context, uint8_t reply[BR_REPLY_SIZE]);
    const char *(*write)(void *context, unsigned brightness);
    void (*settle)(void *context);
} brightness_io;

typedef struct {
    brightness_value before;
    brightness_value after;
    bool write_attempted;
    bool verified;
} brightness_change;

const char *brightness_get(const brightness_io *io, brightness_value *out);
const char *brightness_set(const brightness_io *io, unsigned requested,
                           brightness_change *result);

#endif
