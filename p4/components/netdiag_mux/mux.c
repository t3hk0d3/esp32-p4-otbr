// Listener table and answer dispatch of the network-diagnostics mux (spec 2026-11-02, section 3.5).
//
// Peer filter. A unicast Get is a CoAP request (kUriDiagnosticGetRequest) and its answer is the CoAP response. OpenThread
// (IDF v6.1, openthread/src/core/coap/coap.cpp:1595, CoapBase::PendingRequests::FindRelatedRequest) matches a
// response to its request only when the response's peer address and port equal the request's destination
// (HasSamePeerAddrAndPort, coap.cpp:1506), unless the destination is multicast or an anycast locator. The server side
// answers with the request's own message info (thread/net_diag.cpp:708), so its source is the address the request
// was sent to. So the peer address of a unicast answer is `dest`, except for anycast locators (the leader ALOC, say),
// whose answer comes from the responder's own address: those are never filtered, like multicast. A multicast Get is
// a non-confirmable query (kUriDiagnosticGetQuery) whose answers come from each responder (net_diag.cpp:816-819).
#include "mux.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "netdiag_mux";

typedef struct {
    otReceiveDiagnosticGetCallback cb; /**< NULL: free slot */
    void *ctx;                         /**< of the latest successful send */
    bool filtered;                     /**< only answers from `peer` */
    otIp6Address peer;
} listener_t;

static listener_t s_listeners[NETDIAG_MUX_MAX_LISTENERS];

/** Multicast, or an anycast locator (IID 0000:00ff:fe00:fcxx). */
static bool answers_from_anywhere(const otIp6Address *dest)
{
    static const uint8_t locator_iid[] = { 0x00, 0x00, 0x00, 0xff, 0xfe, 0x00 };
    const uint8_t *a = dest->mFields.m8;

    return a[0] == 0xff || (!memcmp(a + 8, locator_iid, sizeof(locator_iid)) && a[14] == 0xfc);
}

static listener_t *find(otReceiveDiagnosticGetCallback cb)
{
    listener_t *free_slot = NULL;

    for (size_t i = 0; i < NETDIAG_MUX_MAX_LISTENERS; i++) {
        if (s_listeners[i].cb == cb) {
            return &s_listeners[i];
        }
        if (!s_listeners[i].cb && !free_slot) {
            free_slot = &s_listeners[i];
        }
    }
    return free_slot;
}

otError netdiag_mux_core_send(const netdiag_mux_ops_t *ops, const otIp6Address *dest, const uint8_t *tlvs,
                              uint8_t count, otReceiveDiagnosticGetCallback cb, void *ctx, bool filter_peer)
{
    if (!dest || !cb) {
        return OT_ERROR_INVALID_ARGS;
    }
    listener_t *l = find(cb);
    if (!l) {
        ESP_LOGE(TAG, "all %d listener slots are taken, send refused", NETDIAG_MUX_MAX_LISTENERS);
        return OT_ERROR_NO_BUFS;
    }
    // The answers arrive on the OpenThread task, which the caller is on or holds the lock of: none can come in before
    // the listener is updated below.
    otError err = ops->send(dest, tlvs, count);
    if (err != OT_ERROR_NONE) {
        return err;
    }
    l->cb = cb;
    l->ctx = ctx;
    l->filtered = filter_peer && !answers_from_anywhere(dest);
    l->peer = *dest;
    return OT_ERROR_NONE;
}

void netdiag_mux_core_dispatch(otError error, otMessage *msg, const otMessageInfo *info)
{
    if (error != OT_ERROR_NONE || !msg || !info) {
        return; // timeouts and failures: every listener ignores them
    }
    // A snapshot of the table: a callback may send again, which can register a listener or change one; neither
    // changes who gets this answer.
    listener_t snapshot[NETDIAG_MUX_MAX_LISTENERS];
    memcpy(snapshot, s_listeners, sizeof(snapshot));
    for (size_t i = 0; i < NETDIAG_MUX_MAX_LISTENERS; i++) {
        const listener_t l = snapshot[i];
        if (!l.cb || (l.filtered && memcmp(l.peer.mFields.m8, info->mPeerAddr.mFields.m8, sizeof(l.peer)))) {
            continue;
        }
        l.cb(error, msg, info, l.ctx);
    }
}

void netdiag_mux_core_release(otReceiveDiagnosticGetCallback cb)
{
    if (!cb) {
        return;
    }
    for (size_t i = 0; i < NETDIAG_MUX_MAX_LISTENERS; i++) {
        if (s_listeners[i].cb == cb) {
            memset(&s_listeners[i], 0, sizeof(s_listeners[i]));
            return;
        }
    }
}

void netdiag_mux_core_reset(void)
{
    memset(s_listeners, 0, sizeof(s_listeners));
}
