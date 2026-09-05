#include "controller.h"

#include <string.h>

static const char *read_value(const brightness_io *io, brightness_value *out) {
    uint8_t reply[BR_REPLY_SIZE];
    memset(reply, 0xa5, sizeof(reply));
    const char *error = io->read(io->context, reply);
    if (error) return error;
    return brightness_reply(reply, sizeof(reply), out);
}

const char *brightness_get(const brightness_io *io, brightness_value *out) {
    const char *error = io->check_identity(io->context);
    return error ? error : read_value(io, out);
}

const char *brightness_set(const brightness_io *io, unsigned requested,
                           brightness_change *result) {
    memset(result, 0, sizeof(*result));
    if (requested > BR_MAXIMUM) return "brightness must be an integer from 0 to 400";
    const char *error = brightness_get(io, &result->before);
    if (error) return error;
    if (result->before.maximum != BR_MAXIMUM)
        return "unexpected monitor range; expected raw maximum 400, refusing setting change";
    if (result->before.current == requested) {
        result->after = result->before;
        result->verified = true;
        return NULL;
    }
    error = io->check_identity(io->context);
    if (error) return error;
    result->write_attempted = true;
    error = io->write(io->context, requested);
    if (error) return error;
    /* This monitor returned a malformed reply immediately after a successful
       setting change. Allow settling and at most 3 readback queries. Never
       resend the setting, and stop immediately if identity/range changes. */
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        io->settle(io->context);
        error = io->check_identity(io->context);
        if (error) return error;
        error = read_value(io, &result->after);
        if (error) continue;
        if (result->after.maximum != BR_MAXIMUM)
            return "monitor range changed after the setting write";
        if (result->after.current == requested) {
            result->verified = true;
            return NULL;
        }
        error = "readback did not confirm the requested brightness";
    }
    return error;
}
