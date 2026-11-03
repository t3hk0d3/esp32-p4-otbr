#pragma once

// Listener table and answer dispatch (private), over an ops table so the host tests can drive them without
// OpenThread. ot_ops.c supplies the real send and feeds OpenThread's answers to netdiag_mux_core_dispatch(). Everything
// runs on the OpenThread task or with the OpenThread lock held, so the table needs no lock of its own.

#include <stdbool.h>
#include <stdint.h>

#include "netdiag_mux.h"

#ifdef __cplusplus
extern "C" {
#endif

/** What the mux needs from OpenThread. */
typedef struct {
    /** Send the Get with the mux's own callback, whose answers reach netdiag_mux_core_dispatch(). */
    otError(*send)(const otIp6Address *dest, const uint8_t *tlvs, uint8_t count);
} netdiag_mux_ops_t;

/** netdiag_mux_send_get() over `ops`. */
otError netdiag_mux_core_send(const netdiag_mux_ops_t *ops, const otIp6Address *dest, const uint8_t *tlvs,
                              uint8_t count, otReceiveDiagnosticGetCallback cb, void *ctx, bool filter_peer);

/** One call of OpenThread's netdiag callback: hand an answer to every listener it belongs to. */
void netdiag_mux_core_dispatch(otError error, otMessage *msg, const otMessageInfo *info);

/** Forget every listener (host tests). */
void netdiag_mux_core_reset(void);

#ifdef __cplusplus
}
#endif
