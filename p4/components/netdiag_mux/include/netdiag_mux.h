#pragma once

// One network-diagnostics callback for every sender (spec 2026-11-02, section 3.5). OpenThread's netdiag client keeps
// a single callback slot and every otThreadSendDiagnosticGet replaces it, so senders took each other's answers. Every
// sender calls netdiag_mux_send_get() instead; the mux owns the slot and hands each answer to the listeners it
// belongs to.

#include <stdbool.h>
#include <stdint.h>

#include "openthread/netdiag.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NETDIAG_MUX_MAX_LISTENERS 8

/**
 * Send a Network Diagnostic Get to `dest` and register `cb` as a listener for its answers.
 *
 * Run it on the OpenThread task or with the OpenThread lock held; answers arrive on the OpenThread task.
 *
 * The mux keeps one listener per callback function, with the `ctx` of its latest successful send. With `filter_peer`
 * and a unicast `dest` (not multicast, not an anycast locator) the listener only gets answers whose peer address is
 * `dest`; otherwise it gets every answer. Answers with an error (timeouts) or without a message are dropped.
 *
 * Known limits: a listener holds one filtered unicast destination at a time (its latest send), so two sends from the
 * same callback function to different peers in flight together only keep the later peer. The peer filter also cannot
 * tell senders apart: a REST diagnostic to a peer (or an anycast locator) that the scan or /topology queries at the
 * same moment can receive their answer.
 *
 * @return the result of otThreadSendDiagnosticGet; OT_ERROR_NO_BUFS when `cb` is new and NETDIAG_MUX_MAX_LISTENERS
 *         callbacks are registered already; OT_ERROR_INVALID_ARGS for a NULL `dest` or `cb`. The listener is only
 *         registered or updated when the send succeeds.
 */
otError netdiag_mux_send_get(const otIp6Address *dest, const uint8_t *tlvs, uint8_t count,
                             otReceiveDiagnosticGetCallback cb, void *ctx, bool filter_peer);

/**
 * Remove the listener of `cb`, so that it gets no further answers (late answers to a multicast Get keep coming for
 * as long as routers answer). Call it when a collection ends. Nothing happens for an unknown or NULL `cb`.
 *
 * Run it on the OpenThread task or with the OpenThread lock held.
 */
void netdiag_mux_release(otReceiveDiagnosticGetCallback cb);

#ifdef __cplusplus
}
#endif
