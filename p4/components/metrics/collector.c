// Thread collector: every 10 s, in BR_RUNNING, reads the OpenThread figures of GET /metrics under the OpenThread lock
// (10 ms at most) into a cache that scrapes copy. A scrape never takes the OpenThread lock.
#include "collector.h"

#include <string.h>

#include "board_cores.h"
#include "br_core.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "metrics.h"
#include "openthread/border_routing.h"
#include "openthread/ip6.h"
#include "openthread/link.h"
#include "openthread/srp_server.h"
#include "openthread/thread.h"
#include "openthread/thread_ftd.h"
#include "openthread/trel.h"
#include "sdkconfig.h"

static const char *TAG = "metrics";

#define COLLECTOR_CORE MGMT_CORE
#define COLLECTOR_PRIORITY (tskIDLE_PRIORITY + 1) // lowest application priority
#define COLLECTOR_STACK 3072
#define MUTEX_WAIT_MS 100

static SemaphoreHandle_t s_mutex;
static bool s_started;
// Guarded by s_mutex:
static metrics_thread_t s_cache;
static bool s_valid;
static bool s_ever_collected;
static int64_t s_collected_us;
static uint32_t s_failures;

static void read_mac(otInstance *ins, metrics_thread_t *t)
{
    const otMacCounters *m = otLinkGetCounters(ins);

    t->mac.tx_total = m->mTxTotal;
    t->mac.tx_retry = m->mTxRetry;
    t->mac.tx_err_cca = m->mTxErrCca;
    t->mac.tx_err_abort = m->mTxErrAbort;
    t->mac.tx_err_busy_channel = m->mTxErrBusyChannel;
    t->mac.rx_total = m->mRxTotal;
    t->mac.rx_duplicated = m->mRxDuplicated;
    t->mac.rx_err_fcs = m->mRxErrFcs;
    t->mac.rx_err_sec = m->mRxErrSec;
    t->mac.rx_err_no_frame = m->mRxErrNoFrame;
    t->mac.rx_err_unknown_neighbor = m->mRxErrUnknownNeighbor;
    t->mac.rx_err_invalid_src = m->mRxErrInvalidSrcAddr;
    t->mac.rx_err_other = m->mRxErrOther;
}

static void read_mesh(otInstance *ins, metrics_thread_t *t)
{
    otRouterInfo router;
    otChildInfo child;
    otNeighborInfo neighbor;
    otNeighborInfoIterator it = OT_NEIGHBOR_INFO_ITERATOR_INIT;
    const uint8_t max_router_id = otThreadGetMaxRouterId(ins);
    const uint16_t max_children = otThreadGetMaxAllowedChildren(ins);

    for (uint16_t id = 0; id <= max_router_id; id++) {
        if (otThreadGetRouterInfo(ins, id, &router) == OT_ERROR_NONE && router.mAllocated) {
            t->routers++;
        }
    }
    for (uint16_t i = 0; i < max_children; i++) {
        if (otThreadGetChildInfoByIndex(ins, i, &child) == OT_ERROR_NONE) {
            t->children++;
        }
    }
    while (otThreadGetNextNeighborInfo(ins, &it, &neighbor) == OT_ERROR_NONE) {
        t->neighbors++;
    }
}

static void read_border_routing(otInstance *ins, metrics_thread_t *t)
{
    const otBorderRoutingCounters *c = otIp6GetBorderRoutingCounters(ins);
    const otPacketsAndBytes *traffic[2][2] = {
        {&c->mInboundUnicast, &c->mInboundMulticast},
        {&c->mOutboundUnicast, &c->mOutboundMulticast},
    };

    t->br_state = (int)otBorderRoutingGetState(ins);
    t->pd_state = (int)otBorderRoutingDhcp6PdGetState(ins);
    for (int d = 0; d < 2; d++) {
        for (int k = 0; k < 2; k++) {
            t->br_packets[d][k] = traffic[d][k]->mPackets;
            t->br_bytes[d][k] = traffic[d][k]->mBytes;
        }
    }
    t->ra[0] = c->mRaRx;
    t->ra[1] = c->mRaTxSuccess;
    t->ra[2] = c->mRaTxFailure;
    t->rs[0] = c->mRsRx;
    t->rs[1] = c->mRsTxSuccess;
    t->rs[2] = c->mRsTxFailure;
}

static void read_srp(otInstance *ins, metrics_thread_t *t)
{
    t->srp_state = (int)otSrpServerGetState(ins);
    for (const otSrpServerHost *h = otSrpServerGetNextHost(ins, NULL); h; h = otSrpServerGetNextHost(ins, h)) {
        if (otSrpServerHostIsDeleted(h)) {
            continue;
        }
        t->srp_hosts++;
        for (const otSrpServerService *s = otSrpServerHostGetNextService(h, NULL); s;
                s = otSrpServerHostGetNextService(h, s)) {
            if (!otSrpServerServiceIsDeleted(s)) {
                t->srp_services++;
            }
        }
    }
}

static void read_trel(otInstance *ins, metrics_thread_t *t)
{
#if CONFIG_OPENTHREAD_RADIO_TREL
    br_core_trel_state_t trel = {0};

    br_core_trel_read_locked(ins, &trel, NULL); // the same walk as /device/status, counts only: both report alike
    t->trel.built = true;
    t->trel.enabled = br_core_trel_wanted();
    t->trel.active = trel.active;
    t->trel.peers = trel.peers;
    t->trel.peers_other = trel.peers_other;
    t->trel.tx_packets = trel.tx_packets;
    t->trel.tx_bytes = trel.tx_bytes;
    t->trel.tx_failures = trel.tx_failures;
    t->trel.rx_packets = trel.rx_packets;
    t->trel.rx_bytes = trel.rx_bytes;
#else
    (void)ins;
    (void)t;
#endif
}

// Under the OpenThread lock: reads only, no allocation, no logging, no waiting.
static void read_thread(otInstance *ins, metrics_thread_t *t)
{
    const otMleCounters *mle = otThreadGetMleCounters(ins);
    const otIpCounters *ip = otThreadGetIp6Counters(ins);

    memset(t, 0, sizeof(*t));
    t->role = (int)otThreadGetDeviceRole(ins);
    t->role_entered[OT_DEVICE_ROLE_DISABLED] = mle->mDisabledRole;
    t->role_entered[OT_DEVICE_ROLE_DETACHED] = mle->mDetachedRole;
    t->role_entered[OT_DEVICE_ROLE_CHILD] = mle->mChildRole;
    t->role_entered[OT_DEVICE_ROLE_ROUTER] = mle->mRouterRole;
    t->role_entered[OT_DEVICE_ROLE_LEADER] = mle->mLeaderRole;
    t->partition_changes = mle->mPartitionIdChanges;
    t->attach_attempts = mle->mAttachAttempts;
    read_mesh(ins, t);
    read_mac(ins, t);
    t->ip6.tx_success = ip->mTxSuccess;
    t->ip6.tx_failure = ip->mTxFailure;
    t->ip6.rx_success = ip->mRxSuccess;
    t->ip6.rx_failure = ip->mRxFailure;
    read_border_routing(ins, t);
    read_srp(ins, t);
    read_trel(ins, t);
}

static void collect_once(void)
{
    metrics_thread_t fresh;

    // The session keeps a radio push from deleting the OpenThread lock while this task waits for it.
    if (br_core_get_mode() != BR_MODE_BR_RUNNING || !br_core_ot_session_begin()) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_valid = false;
        xSemaphoreGive(s_mutex);
        return;
    }
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(METRICS_COLLECT_LOCK_MS))) {
        br_core_ot_session_end();
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_failures++;
        xSemaphoreGive(s_mutex);
        return;
    }
    // The mode may have changed while waiting: the stack is only read in BR_RUNNING.
    otInstance *ins = esp_openthread_get_instance();
    const bool running = ins && br_core_get_mode() == BR_MODE_BR_RUNNING;

    if (running) {
        read_thread(ins, &fresh);
    }
    esp_openthread_lock_release();
    br_core_ot_session_end();

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (running) {
        s_cache = fresh;
        s_collected_us = esp_timer_get_time();
        s_ever_collected = true;
    }
    s_valid = running;
    xSemaphoreGive(s_mutex);
}

static void collector_task(void *arg)
{
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        collect_once();
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(METRICS_COLLECT_PERIOD_MS));
    }
}

esp_err_t metrics_start(void)
{
    ESP_RETURN_ON_FALSE(!s_started, ESP_ERR_INVALID_STATE, TAG, "already started");
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "no memory for the collector");
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(collector_task, "metrics", COLLECTOR_STACK, NULL, COLLECTOR_PRIORITY, NULL,
                                COLLECTOR_CORE) != pdPASS) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        ESP_LOGE(TAG, "cannot create the collector task");
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}

esp_err_t metrics_collector_copy(metrics_collector_view_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_started) {
        return ESP_OK;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_WAIT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    out->started = true;
    out->ever_collected = s_ever_collected;
    // A collection is at most 10 s old: drop the Thread series at once when the mode has left BR_RUNNING since.
    out->valid = s_valid && br_core_get_mode() == BR_MODE_BR_RUNNING;
    out->failures = s_failures;
    out->thread = s_cache;
    const int64_t collected_us = s_collected_us;

    xSemaphoreGive(s_mutex);
    if (out->ever_collected) {
        const int64_t age_ms = (esp_timer_get_time() - collected_us) / 1000;

        out->age_ms = age_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)age_ms;
    }
    return ESP_OK;
}
