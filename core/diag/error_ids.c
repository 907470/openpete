/* error_ids.c - the id and message reference of each player-facing error.
 * See openpete_error_ids.h. */

#include "openpete_error_ids.h"

#define ERROR_ROW(name, id, anchor) \
    [OP_E_##name] = { id, OPENPETE_TROUBLESHOOTING_URL "#" anchor,          \
                      "[" id ": " OPENPETE_TROUBLESHOOTING_URL "#" anchor "]" },

static const struct {
    const char* id;
    const char* url;
    const char* ref;
} k_errors[OP_E_COUNT] = {
    OPENPETE_ERROR_IDS(ERROR_ROW)
};

#undef ERROR_ROW

const char* openpete_error_id(openpete_error_t e) {
    return ((unsigned)e < OP_E_COUNT) ? k_errors[e].id : "OP-E-UNKNOWN";
}

const char* openpete_error_url(openpete_error_t e) {
    return ((unsigned)e < OP_E_COUNT) ? k_errors[e].url
                                      : OPENPETE_TROUBLESHOOTING_URL "#by-error-id";
}

const char* openpete_error_ref(openpete_error_t e) {
    return ((unsigned)e < OP_E_COUNT)
               ? k_errors[e].ref
               : "[OP-E-UNKNOWN: " OPENPETE_TROUBLESHOOTING_URL "#by-error-id]";
}
