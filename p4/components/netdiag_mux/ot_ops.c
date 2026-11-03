// OpenThread glue of the network-diagnostics mux: the real send, which installs the mux's own callback in
// OpenThread's single netdiag slot, and the public netdiag_mux_send_get().
#include "esp_openthread.h"
#include "mux.h"
#include "openthread/netdiag.h"

/** OpenThread task: every netdiag answer and error lands here; the context is unused. */
static void on_answer(otError error, otMessage *msg, const otMessageInfo *info, void *ctx)
{
    (void)ctx;
    netdiag_mux_core_dispatch(error, msg, info);
}

static otError op_send(const otIp6Address *dest, const uint8_t *tlvs, uint8_t count)
{
    return otThreadSendDiagnosticGet(esp_openthread_get_instance(), dest, tlvs, count, on_answer, NULL);
}

static const netdiag_mux_ops_t real_ops = {
    .send = op_send,
};

otError netdiag_mux_send_get(const otIp6Address *dest, const uint8_t *tlvs, uint8_t count,
                             otReceiveDiagnosticGetCallback cb, void *ctx, bool filter_peer)
{
    return netdiag_mux_core_send(&real_ops, dest, tlvs, count, cb, ctx, filter_peer);
}
