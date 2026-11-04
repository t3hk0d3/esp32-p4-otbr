// OpenThread glue of the mesh scan: the real ops of scan.c, the mesh-diag callbacks that feed its entry points, and
// the public mesh_scan_init/start/cancel. Lock order everywhere: OpenThread lock, then the scan mutex. The ops never
// take the OpenThread lock: scan.c calls them from mesh_scan_start/cancel (which hold it) or from the OpenThread task.
//
// otMeshDiagCancel (OpenThread mesh_diag.cpp) is synchronous: it aborts the discover CoAP transactions, returns the
// module to idle and stops its timer, and every response path checks that state first. So no callback of a cancelled
// operation runs afterwards, and the callbacks below need no guard of their own. The paths that fail a scan without
// cancelling (the mode watchdog, a callback that finds the mode lost, a cancel without the OpenThread lock) rely on
// OpenThread's single mesh-diag slot instead: a new discover gets OT_ERROR_BUSY until the abandoned operation has ended.
//
// The child address step (spec 3.4, ruling R14) sends Network Diagnostic Gets through netdiag_mux (spec 3.5), which
// otMeshDiagCancel does not cover. The mux owns OpenThread's single netdiag callback slot, so the vendored GUI/REST
// API (esp_br_web_api.c) no longer takes our answers or replaces our callback; on_child_diag gets every answer
// (filter_peer false) with the context of the scan's LATEST send, not of the query being answered. An answer is
// matched to its child by the peer address (the child's RLOC), and the scan's gen/phase check is a second guard. An
// answer without an IPv6 Address List TLV is dropped; one to another sender's query that carries the list describes
// its peer correctly, so using it is harmless. Errors never reach us: a scan-owned deadline per query gives up on
// children that don't answer.
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "br_core.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "mesh_scan.h"
#include "netdiag_mux.h"
#include "openthread/error.h"
#include "openthread/link.h"
#include "openthread/mesh_diag.h"
#include "openthread/multi_radio.h"
#include "openthread/netdiag.h"
#include "openthread/netdata.h"
#include "openthread/srp_server.h"
#include "openthread/thread.h"
#include "openthread/thread_ftd.h"
#include "scan.h"
#include "sdkconfig.h"
#include "srp.h"

static const char *TAG = "mesh_scan";

#define LOCK_WAIT_MS 100
#define CAP_POST_RETRY_MS 100 // the OpenThread task queue was full
#define WATCHDOG_PERIOD_MS 1000

_Static_assert(MESH_SCAN_OT_ERROR_NONE == OT_ERROR_NONE, "OT error code");
_Static_assert(MESH_SCAN_OT_ERROR_NO_BUFS == OT_ERROR_NO_BUFS, "OT error code");
_Static_assert(MESH_SCAN_OT_ERROR_BUSY == OT_ERROR_BUSY, "OT error code");
_Static_assert(MESH_SCAN_OT_ERROR_INVALID_STATE == OT_ERROR_INVALID_STATE, "OT error code");
_Static_assert(MESH_SCAN_OT_ERROR_RESPONSE_TIMEOUT == OT_ERROR_RESPONSE_TIMEOUT, "OT error code");
_Static_assert(MESH_SCAN_MAX_ROUTERS == OT_NETWORK_MAX_ROUTER_ID + 1, "router IDs");
_Static_assert(sizeof(((mesh_scan_router_t *)0)->lq_in) == sizeof(((otMeshDiagRouterInfo *)0)->mLinkQualities),
               "link qualities");
_Static_assert(sizeof(mesh_scan_ip6_t) == sizeof(otIp6Address), "IPv6 address");
_Static_assert(sizeof(mesh_scan_prefix_t) == sizeof(otMeshLocalPrefix), "mesh-local prefix");
_Static_assert(sizeof(((mesh_scan_child_t *)0)->ext) == sizeof(otExtAddress), "ext address");

// OpenThread's task queue runs a function on the OpenThread task with the lock held. Its header is private to the
// component, the function itself is exported (br_core.c declares it the same way).
typedef void (*ot_task_fn_t)(void *);
// Must match components/openthread/private_include/esp_openthread_task_queue.h (IDF v6.1); revisit on an IDF update.
extern esp_err_t esp_openthread_task_queue_post(ot_task_fn_t task, void *arg);

static bool s_initialised;
static esp_timer_handle_t s_cap_timer;
static esp_timer_handle_t s_watchdog_timer;
static esp_timer_handle_t s_child_timer;
/** Children of the router in a discover callback (OpenThread task only); PSRAM, MESH_SCAN_MAX_CHILDREN entries. */
static mesh_scan_child_t *s_children;

// Cap delivery. A cap event posted for an old scan must not cut a newer one short: arm_cap records the scan's
// generation and the deadline (both under the OpenThread lock); the timer callback captures the generation armed at
// that moment and posts it to the OpenThread task, where the cap counts only if the generation still matches and its
// deadline has passed (the deadline catches a re-arm that raced with the capture).
//
// Mode watchdog (ruling R6). While a scan runs, a 1 s periodic timer checks the mode; once it is not BR_RUNNING it
// fails the scan through mesh_scan_on_mode_lost(gen) from the esp_timer task (scan mutex only, no OpenThread call:
// OpenThread may be torn down). The cap does the same when it finds the mode lost. disarm_cap stops both timers.
static uint32_t s_cap_gen;          // OpenThread lock
static int64_t s_cap_deadline_us;   // OpenThread lock
static atomic_uint_fast32_t s_cap_armed_gen;
static atomic_uint_fast32_t s_child_armed_gen; // child deadline: the generation it was armed for

static otInstance *instance(void)
{
    return esp_openthread_get_instance();
}

// Conversions (OpenThread task)

static void child_from_info(mesh_scan_child_t *c, const otMeshDiagChildInfo *info)
{
    memset(c, 0, sizeof(*c));
    c->rloc16 = info->mRloc16;
    c->rx_on_when_idle = info->mMode.mRxOnWhenIdle;
    c->full_thread_device = info->mMode.mDeviceType;
    c->full_network_data = info->mMode.mNetworkData;
    c->lq_in = info->mLinkQuality;
}

static void router_from_info(mesh_scan_router_t *r, otMeshDiagRouterInfo *info)
{
    memset(r, 0, sizeof(*r));
    r->rloc16 = info->mRloc16;
    r->router_id = info->mRouterId;
    memcpy(r->ext, info->mExtAddress.m8, sizeof(r->ext));
    r->has_version = info->mVersion != OT_MESH_DIAG_VERSION_UNKNOWN;
    r->version = r->has_version ? info->mVersion : 0;
    r->leader = info->mIsLeader;
    r->this_device = info->mIsThisDevice;
    r->border_router = info->mIsBorderRouter;
    memcpy(r->lq_in, info->mLinkQualities, sizeof(r->lq_in));

    otIp6Address addr;
    while (info->mIp6AddrIterator && r->n_addrs < MESH_SCAN_MAX_ROUTER_ADDRS &&
            otMeshDiagGetNextIp6Address(info->mIp6AddrIterator, &addr) == OT_ERROR_NONE) {
        memcpy(r->addrs[r->n_addrs++], addr.mFields.m8, sizeof(r->addrs[0]));
    }

    // The true count goes on: scan.c sees a router with more children than it keeps and marks it partial.
    otMeshDiagChildInfo child;
    uint32_t n = 0;
    while (info->mChildIterator && s_children && n < UINT16_MAX &&
            otMeshDiagGetNextChildInfo(info->mChildIterator, &child) == OT_ERROR_NONE) {
        if (n < MESH_SCAN_MAX_CHILDREN) {
            child_from_info(&s_children[n], &child);
        }
        n++;
    }
    r->n_children = (uint16_t)n;
    r->children = n ? s_children : NULL;
}

static void child_from_entry(mesh_scan_child_t *c, const otMeshDiagChildEntry *e)
{
    memset(c, 0, sizeof(*c));
    c->rloc16 = e->mRloc16;
    memcpy(c->ext, e->mExtAddress.m8, sizeof(c->ext));
    c->has_ext = true;
    c->rx_on_when_idle = e->mRxOnWhenIdle;
    c->full_thread_device = e->mDeviceTypeFtd;
    c->full_network_data = e->mFullNetData;
    c->timeout_s = e->mTimeout;
    c->age_s = e->mAge;
    c->connected_s = e->mConnectionTime;
    c->rssi_avg = e->mAverageRssi;
    c->rssi_last = e->mLastRssi;
    c->link_margin_db = e->mLinkMargin;
    c->has_err_rate = e->mSupportsErrRate;
    c->frame_err = e->mSupportsErrRate ? e->mFrameErrorRate : 0;
    c->msg_err = e->mSupportsErrRate ? e->mMessageErrorRate : 0;
    c->version = e->mVersion;
    c->csl = e->mCslSynchronized;
}

// Mesh-diag callbacks (OpenThread task, OpenThread lock held). OT_ERROR_PENDING means more calls follow; any other
// code is the last call of the operation.

static void on_discover(otError error, otMeshDiagRouterInfo *info, void *ctx)
{
    (void)ctx;
    if (info) {
        mesh_scan_router_t r;
        router_from_info(&r, info);
        mesh_scan_on_router(&r);
    }
    if (error != OT_ERROR_PENDING) {
        mesh_scan_on_discover_done(error);
    }
}

static void on_child_table(otError error, const otMeshDiagChildEntry *entry, void *ctx)
{
    (void)ctx;
    if (entry) {
        mesh_scan_child_t c;
        child_from_entry(&c, entry);
        mesh_scan_on_child_entry(&c);
    }
    if (error != OT_ERROR_PENDING) {
        mesh_scan_on_child_table_done(error);
    }
}

static void on_child_addrs(otError error, uint16_t child_rloc16, otMeshDiagIp6AddrIterator *it, void *ctx)
{
    (void)ctx;
    if (it) {
        uint8_t addrs[MESH_SCAN_MAX_CHILD_ADDRS][16];
        uint8_t n = 0;
        otIp6Address addr;
        while (n < MESH_SCAN_MAX_CHILD_ADDRS && otMeshDiagGetNextIp6Address(it, &addr) == OT_ERROR_NONE) {
            memcpy(addrs[n++], addr.mFields.m8, sizeof(addrs[0]));
        }
        if (n) {
            mesh_scan_on_child_addrs(child_rloc16, (const uint8_t (*)[16])addrs, n);
        }
    }
    if (error != OT_ERROR_PENDING) {
        mesh_scan_on_child_addrs_done(error);
    }
}

/** Network Diagnostic Get answer (any sender's, via netdiag_mux); `ctx` is the scan generation of our latest send. */
static void on_child_diag(otError error, otMessage *msg, const otMessageInfo *info, void *ctx)
{
    static otNetworkDiagTlv tlv; // OpenThread task only; a few hundred bytes, kept off its stack
    uint8_t addrs[MESH_SCAN_MAX_CHILD_ADDRS][16];
    uint8_t n = 0;
    uint16_t rloc16;

    if (error != OT_ERROR_NONE || !msg || !info ||
            !mesh_scan_rloc16_from_peer(info->mPeerAddr.mFields.m8, otThreadGetMeshLocalPrefix(instance())->m8,
                                        &rloc16)) {
        return;
    }
    otNetworkDiagIterator it = OT_NETWORK_DIAGNOSTIC_ITERATOR_INIT;
    while (otThreadGetNextDiagnosticTlv(msg, &it, &tlv) == OT_ERROR_NONE) {
        if (tlv.mType != OT_NETWORK_DIAGNOSTIC_TLV_IP6_ADDR_LIST) {
            continue;
        }
        for (uint8_t i = 0; i < tlv.mData.mIp6AddrList.mCount && n < MESH_SCAN_MAX_CHILD_ADDRS; i++) {
            memcpy(addrs[n++], tlv.mData.mIp6AddrList.mList[i].mFields.m8, sizeof(addrs[0]));
        }
    }
    mesh_scan_on_child_diag((uint32_t)(uintptr_t)ctx, rloc16, (const uint8_t (*)[16])addrs, n);
}

// Cap timer

static void cap_on_ot_task(void *arg)
{
    const uint32_t gen = (uint32_t)(uintptr_t)arg;

    if (gen != s_cap_gen || esp_timer_get_time() < s_cap_deadline_us) {
        return; // armed for an older scan
    }
    mesh_scan_on_cap();
}

/** Start a one-shot; a concurrent re-arm from the timer callback (retry) makes the first start fail once. */
static void one_shot_start(esp_timer_handle_t timer, uint64_t us)
{
    esp_timer_stop(timer);
    esp_err_t err = esp_timer_start_once(timer, us);
    if (err == ESP_ERR_INVALID_STATE) {
        esp_timer_stop(timer);
        err = esp_timer_start_once(timer, us);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "one-shot timer: %s", esp_err_to_name(err));
    }
}

/**
 * esp_timer task: hand the cap to the OpenThread task. Outside BR_RUNNING the scan fails here instead (OpenThread and
 * its task queue may be gone).
 */
static void cap_timer_cb(void *arg)
{
    (void)arg;
    const uint32_t gen = (uint32_t)atomic_load(&s_cap_armed_gen);

    // The session keeps a radio push from deleting the task queue between the mode check and the post.
    if (br_core_get_mode() != BR_MODE_BR_RUNNING || !br_core_ot_session_begin()) {
        mesh_scan_on_mode_lost(gen);
        return;
    }
    if (esp_openthread_task_queue_post(cap_on_ot_task, (void *)(uintptr_t)gen) != ESP_OK) {
        // ESP_ERR_INVALID_STATE: a new scan armed the timer meanwhile, which supersedes this cap.
        esp_timer_start_once(s_cap_timer, (uint64_t)CAP_POST_RETRY_MS * 1000);
    }
    br_core_ot_session_end();
}

// Child deadline: the same hand-over as the cap. scan.c checks the generation and which queries are overdue, so an
// early or stale delivery does no harm.

static void child_deadline_on_ot_task(void *arg)
{
    const uint32_t gen = (uint32_t)(uintptr_t)arg;

    if (gen == s_cap_gen) {
        mesh_scan_on_child_deadline(gen);
    }
}

/** esp_timer task: hand the child deadline to the OpenThread task (or fail the scan outside BR_RUNNING). */
static void child_timer_cb(void *arg)
{
    (void)arg;
    const uint32_t gen = (uint32_t)atomic_load(&s_child_armed_gen);

    if (br_core_get_mode() != BR_MODE_BR_RUNNING || !br_core_ot_session_begin()) {
        mesh_scan_on_mode_lost(gen);
        return;
    }
    if (esp_openthread_task_queue_post(child_deadline_on_ot_task, (void *)(uintptr_t)gen) != ESP_OK) {
        esp_timer_start_once(s_child_timer, (uint64_t)CAP_POST_RETRY_MS * 1000);
    }
    br_core_ot_session_end();
}

/** esp_timer task, every second while a scan runs. */
static void watchdog_cb(void *arg)
{
    (void)arg;
    if (br_core_get_mode() != BR_MODE_BR_RUNNING) {
        mesh_scan_on_mode_lost((uint32_t)atomic_load(&s_cap_armed_gen));
    }
}

// Ops (scan mutex held; OpenThread lock held by the caller)

static int op_discover(void)
{
    const otMeshDiagDiscoverConfig config = {
        .mDiscoverIp6Addresses = true,
        .mDiscoverChildTable = true,
    };
    return otMeshDiagDiscoverTopology(instance(), &config, on_discover, NULL);
}

static int op_query_child_table(uint16_t rloc16)
{
    return otMeshDiagQueryChildTable(instance(), rloc16, on_child_table, NULL);
}

static int op_query_child_addrs(uint16_t rloc16)
{
    return otMeshDiagQueryChildrenIp6Addrs(instance(), rloc16, on_child_addrs, NULL);
}

static int op_query_child_diag(uint16_t rloc16, uint32_t gen)
{
    static const uint8_t tlvs[] = { OT_NETWORK_DIAGNOSTIC_TLV_IP6_ADDR_LIST };
    otIp6Address dest = { 0 };

    // The child's RLOC: mesh-local prefix + 0:ff:fe00:<rloc16>.
    memcpy(dest.mFields.m8, otThreadGetMeshLocalPrefix(instance())->m8, OT_MESH_LOCAL_PREFIX_SIZE);
    dest.mFields.m8[11] = 0xff;
    dest.mFields.m8[12] = 0xfe;
    dest.mFields.m8[14] = (uint8_t)(rloc16 >> 8);
    dest.mFields.m8[15] = (uint8_t)rloc16;
    // Unfiltered: on_child_diag matches the peer to the child itself.
    return netdiag_mux_send_get(&dest, tlvs, sizeof(tlvs), on_child_diag, (void *)(uintptr_t)gen, false);
}

static void op_arm_child_deadline(uint32_t ms, uint32_t gen)
{
    atomic_store(&s_child_armed_gen, gen);
    one_shot_start(s_child_timer, (uint64_t)ms * 1000);
}

static void op_cancel(void)
{
    otMeshDiagCancel(instance());
}

static uint8_t op_router_table(uint16_t rloc16_out[MESH_SCAN_MAX_ROUTERS], uint8_t ids_out[MESH_SCAN_MAX_ROUTERS])
{
    otInstance *ins = instance();
    const uint8_t max_id = otThreadGetMaxRouterId(ins);
    uint8_t n = 0;

    for (uint16_t id = 0; id <= max_id && n < MESH_SCAN_MAX_ROUTERS; id++) {
        otRouterInfo info;
        if (otThreadGetRouterInfo(ins, id, &info) != OT_ERROR_NONE || !info.mAllocated) {
            continue;
        }
        rloc16_out[n] = info.mRloc16;
        ids_out[n] = info.mRouterId;
        n++;
    }
    return n;
}

/** Copy the first `labels` dot-separated labels of `src` (the rest is the domain), truncated to `cap`. */
static void copy_labels(char *dst, size_t cap, const char *src, int labels)
{
    size_t i = 0;

    for (; src && src[i] && i + 1 < cap; i++) {
        if (src[i] == '.' && --labels == 0) {
            break;
        }
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

static void snapshot_network(otInstance *ins, mesh_scan_network_t *net)
{
    otLeaderData leader;

    const char *name = otThreadGetNetworkName(ins);
    if (name) {
        strlcpy(net->name, name, sizeof(net->name));
    }
    net->channel = otLinkGetChannel(ins);
    if (otThreadGetLeaderData(ins, &leader) == OT_ERROR_NONE) {
        net->partition_id = leader.mPartitionId;
        net->leader_router_id = leader.mLeaderRouterId;
    }
    memcpy(net->ml_prefix, otThreadGetMeshLocalPrefix(ins)->m8, sizeof(net->ml_prefix));
    net->this_rloc16 = otThreadGetRloc16(ins);

    otNetworkDataIterator it = OT_NETWORK_DATA_ITERATOR_INIT;
    otBorderRouterConfig config;
    net->n_omr = 0;
    while (net->n_omr < MESH_SCAN_MAX_OMR && otNetDataGetNextOnMeshPrefix(ins, &it, &config) == OT_ERROR_NONE) {
        if (!config.mSlaac) {
            continue;
        }
        bool seen = false; // one entry per border router publishing the prefix
        for (uint8_t i = 0; i < net->n_omr && !seen; i++) {
            seen = !memcmp(net->omr[i], config.mPrefix.mPrefix.mFields.m8, sizeof(net->omr[i]));
        }
        if (!seen) {
            memcpy(net->omr[net->n_omr++], config.mPrefix.mPrefix.mFields.m8, sizeof(net->omr[0]));
        }
    }
}

/** Fill one service of a local host: port, TXT (cut at 512 bytes) and subtype labels go through the result's pool. */
static void snapshot_service(mesh_scan_result_t *r, const otSrpServerService *svc, uint16_t source,
                             __typeof__(((mesh_scan_srp_host_t *)0)->services[0]) *s)
{
    uint16_t txt_len = 0;
    const uint8_t *txt = otSrpServerServiceGetTxtData(svc, &txt_len);
    const uint16_t kept = txt ? mesh_scan_txt_cut(txt, txt_len, MESH_SCAN_TXT_MAX) : 0;

    strlcpy(s->instance, otSrpServerServiceGetInstanceLabel(svc), sizeof(s->instance));
    // "_matter._tcp.default.service.arpa." -> "_matter._tcp"
    copy_labels(s->type, sizeof(s->type), otSrpServerServiceGetServiceName(svc), 2);
    s->port = otSrpServerServiceGetPort(svc);
    s->source = source;
    s->txt_truncated = kept < txt_len;
    if (kept > 0 && mesh_scan_pool_put(r, txt, kept, &s->txt_off)) {
        s->has_txt = true;
        s->txt_len = kept;
    } else if (kept > 0) {
        s->txt_truncated = true; // pool full
    } else if (txt) {
        s->has_txt = true; // present and empty (or nothing fitting the cap: truncated above)
    }

    // Subtype labels, NUL-separated: "_i4f2._sub._matter._tcp.default.service.arpa." -> "_i4f2"
    const uint16_t n_sub = otSrpServerServiceGetNumberOfSubTypes(svc);
    uint16_t start = r->pool_used;

    for (uint16_t i = 0; i < n_sub && s->n_sub < MESH_SCAN_MAX_SUBTYPES; i++) {
        char label[MESH_SCAN_NAME_LEN];

        if (otSrpServerParseSubTypeServiceName(otSrpServerServiceGetSubTypeServiceNameAt(svc, i), label,
                                               sizeof(label)) != OT_ERROR_NONE ||
                !mesh_scan_pool_put(r, (const uint8_t *)label, (uint16_t)strlen(label) + 1, &start)) {
            continue;
        }
        if (s->n_sub++ == 0) {
            s->sub_off = start;
        }
    }
}

static void snapshot_srp_host(mesh_scan_result_t *r, const otSrpServerHost *host, mesh_scan_srp_host_t *out,
                              uint16_t source)
{
    uint8_t n_addrs = 0;
    const otIp6Address *addrs = otSrpServerHostGetAddresses(host, &n_addrs);
    otSrpServerLeaseInfo lease;

    memset(out, 0, sizeof(*out));
    copy_labels(out->name, sizeof(out->name), otSrpServerHostGetFullName(host), 1); // without .default.service.arpa.
    for (uint8_t i = 0; addrs && i < n_addrs && out->n_addrs < MESH_SCAN_MAX_SRP_ADDRS; i++) {
        memcpy(out->addrs[out->n_addrs++], addrs[i].mFields.m8, sizeof(out->addrs[0]));
    }
    for (const otSrpServerService *svc = otSrpServerHostGetNextService(host, NULL);
            svc && out->n_services < MESH_SCAN_MAX_SRP_SERVICES; svc = otSrpServerHostGetNextService(host, svc)) {
        if (!otSrpServerServiceIsDeleted(svc)) {
            snapshot_service(r, svc, source, &out->services[out->n_services++]);
        }
    }
    otSrpServerHostGetLeaseInfo(host, &lease);
    out->source = source;
    out->local = true;
    out->lease_s = lease.mRemainingLease / 1000;
    out->key_lease_s = lease.mRemainingKeyLease / 1000;
}

// Radios this board uses toward each router it has a link to. Local state only: no mesh traffic.
static void snapshot_transport(otInstance *ins, mesh_scan_result_t *r)
{
    memset(r->this_links, 0, sizeof(r->this_links));
    r->has_transport = false;
#if CONFIG_OPENTHREAD_RADIO_TREL
    r->has_transport = true;
    for (uint8_t id = 0; id < MESH_SCAN_MAX_ROUTERS; id++) {
        otRouterInfo router;
        otMultiRadioNeighborInfo radios;

        if (otThreadGetRouterInfo(ins, id, &router) != OT_ERROR_NONE || !router.mAllocated || !router.mLinkEstablished ||
                otMultiRadioGetNeighborInfo(ins, &router.mExtAddress, &radios) != OT_ERROR_NONE) {
            continue;
        }
        r->this_links[id] = (mesh_scan_transport_t) {
            .valid = true,
            .lq_in = router.mLinkQualityIn,
            .pref_154 = radios.mSupportsIeee802154 ? radios.mIeee802154Info.mPreference : MESH_SCAN_PREF_NONE,
            .pref_trel = radios.mSupportsTrelUdp6 ? radios.mTrelUdp6Info.mPreference : MESH_SCAN_PREF_NONE,
        };
    }
#else
    (void)ins;
#endif
}

static void op_snapshot(mesh_scan_result_t *r)
{
    otInstance *ins = instance();

    snapshot_network(ins, &r->network);
    snapshot_transport(ins, r);
    r->n_srp = 0;
    r->pool_used = 0;
    for (const otSrpServerHost *host = otSrpServerGetNextHost(ins, NULL);
            host && r->n_srp < MESH_SCAN_MAX_SRP_HOSTS; host = otSrpServerGetNextHost(ins, host)) {
        if (!otSrpServerHostIsDeleted(host)) {
            snapshot_srp_host(r, host, &r->srp[r->n_srp++], r->network.this_rloc16);
        }
    }
}

static bool op_br_running(void)
{
    return br_core_get_mode() == BR_MODE_BR_RUNNING;
}

static int64_t op_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void op_arm_cap(uint32_t ms, uint32_t gen)
{
    s_cap_gen = gen;
    s_cap_deadline_us = esp_timer_get_time() + (int64_t)ms * 1000;
    atomic_store(&s_cap_armed_gen, gen);
    one_shot_start(s_cap_timer, (uint64_t)ms * 1000);
    esp_timer_stop(s_watchdog_timer);
    esp_err_t err = esp_timer_start_periodic(s_watchdog_timer, (uint64_t)WATCHDOG_PERIOD_MS * 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mode watchdog: %s", esp_err_to_name(err));
    }
}

static void op_disarm_cap(void)
{
    // ESP_ERR_INVALID_STATE when not running (the cap already fired): the generation checks cover that.
    esp_timer_stop(s_cap_timer);
    esp_timer_stop(s_watchdog_timer);
    esp_timer_stop(s_child_timer);
}

static void *op_alloc(size_t size)
{
    return heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM);
}

static void op_release(void *p)
{
    heap_caps_free(p);
}

static const mesh_scan_ops_t real_ops = {
    .discover = op_discover,
    .query_child_table = op_query_child_table,
    .query_child_addrs = op_query_child_addrs,
    .query_child_diag = op_query_child_diag,
    .arm_child_deadline = op_arm_child_deadline,
    .cancel = op_cancel,
    .router_table = op_router_table,
    .snapshot = op_snapshot,
    .br_running = op_br_running,
    .now_ms = op_now_ms,
    .arm_cap = op_arm_cap,
    .disarm_cap = op_disarm_cap,
    .alloc = op_alloc,
    .release = op_release,
};

// Public entry points

esp_err_t mesh_scan_init(void)
{
    ESP_RETURN_ON_FALSE(!s_initialised, ESP_ERR_INVALID_STATE, TAG, "already initialised");

    s_children = heap_caps_calloc(MESH_SCAN_MAX_CHILDREN, sizeof(*s_children), MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_children, ESP_ERR_NO_MEM, TAG, "no memory for the children buffer");

    const esp_timer_create_args_t cap_args = {
        .callback = cap_timer_cb,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "mesh_scan_cap",
    };
    const esp_timer_create_args_t watchdog_args = {
        .callback = watchdog_cb,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "mesh_scan_wdog",
    };
    const esp_timer_create_args_t child_args = {
        .callback = child_timer_cb,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "mesh_scan_child",
    };
    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_ERROR(esp_timer_create(&cap_args, &s_cap_timer), fail, TAG, "cap timer");
    ESP_GOTO_ON_ERROR(esp_timer_create(&watchdog_args, &s_watchdog_timer), fail, TAG, "mode watchdog");
    ESP_GOTO_ON_ERROR(esp_timer_create(&child_args, &s_child_timer), fail, TAG, "child deadline");

    mesh_scan_set_ops(&real_ops);
    s_initialised = true;
    return ESP_OK;

fail:
    if (s_cap_timer) {
        esp_timer_delete(s_cap_timer);
        s_cap_timer = NULL;
    }
    if (s_watchdog_timer) {
        esp_timer_delete(s_watchdog_timer);
        s_watchdog_timer = NULL;
    }
    heap_caps_free(s_children);
    s_children = NULL;
    return ret;
}

esp_err_t mesh_scan_start(void)
{
    ESP_RETURN_ON_FALSE(s_initialised, ESP_ERR_NOT_SUPPORTED, TAG, "not initialised");
    // The session keeps a radio push from deleting the lock and the instance under this call.
    if (!br_core_ot_session_begin()) {
        return ESP_ERR_NOT_ALLOWED; // OpenThread is stopping or not up: the mode is no longer BR_RUNNING
    }
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(LOCK_WAIT_MS))) {
        br_core_ot_session_end();
        ESP_LOGW(TAG, "start: OpenThread lock not free within %d ms", LOCK_WAIT_MS);
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = mesh_scan_core_start();
    esp_openthread_lock_release();
    br_core_ot_session_end();
    return err;
}

esp_err_t mesh_scan_cancel(void)
{
    if (!s_initialised) {
        return ESP_OK;
    }
    // No session: OpenThread is stopping or not up, so there is no lock to take. Fail the scan (scan mutex only).
    if (!br_core_ot_session_begin()) {
        mesh_scan_core_fail_stopped();
        return ESP_OK;
    }
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(LOCK_WAIT_MS))) {
        br_core_ot_session_end();
        // Fail the scan anyway (scan mutex only), so the caller never runs alongside a live scan.
        ESP_LOGW(TAG, "cancel: OpenThread lock not free within %d ms; stopping without telling OpenThread",
                 LOCK_WAIT_MS);
        mesh_scan_core_fail_stopped();
        return ESP_ERR_TIMEOUT;
    }
    mesh_scan_core_cancel();
    esp_openthread_lock_release();
    br_core_ot_session_end();
    return ESP_OK;
}
