#include "br_core.h"
#include "board_cores.h"

#include <assert.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

#include "board.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_br_web.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_border_router.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_mux.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_spinel.h"
#include "esp_openthread_types.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs.h"
#include "openthread/border_routing.h"
#include "openthread/dataset.h"
#include "openthread/dataset_ftd.h"
#include "openthread/ip6.h"
#include "openthread/link.h"
#include "openthread/link_raw.h"
#include "openthread/multi_radio.h"
#include "openthread/netdiag.h"
#include "openthread/platform/radio.h"
#include "openthread/thread.h"
#include "openthread/thread_ftd.h"
#include "openthread/trel.h"
#include "mode_fsm.h"
#include "otbr_probe.h"
#include "radio_mux.h"
#include "vendor_info.h"
#include "rcp_sync.h"
#include "sdkconfig.h"
#ifdef OTBR_TEST_BUILD
#include "driver/gpio.h"
#endif

static const char *TAG = "br_core";

#define OT_TASK_NAME "ot_main"
#define OT_TASK_STACK 12288
#define OT_TASK_PRIORITY CONFIG_OPENTHREAD_TASK_PRIORITY
#define BRINGUP_TASK_STACK 6144
#define BRINGUP_TASK_PRIORITY 4
#define BOOT_WAIT_MS 180000
#define BRINGUP_WAIT_MS 60000
#define LOOP_STOP_WAIT_MS 5000
// Longest a push waits for the open OpenThread sessions. The web server's callback waits end within ~0.25 s of the
// REST flag going off, so this only bounds a session held by slow HTTP I/O or a stuck OpenThread call.
#define SESSION_DRAIN_MS 45000
#define STATUS_LOCK_MS 100
#define LEADER_LOCK_MS 1000 // the PUT /device/thread wait for the OpenThread lock
#define LEADER_NVS_NAMESPACE "thread"
#define LEADER_NVS_KEY "leader_adj"
#define REBOOT_DELAY_MS 1500 // lets the caller of a push report its result before the reboot
#define FAULT_NO_RCP "no rcp firmware"
#define FAULT_INCOMPATIBLE "rcp incompatible"
#define FAULT_ROM_RECOVERY "rom recovery failed"
#define TREL_SET_LOCK_MS 1000
#define TREL_QUERY_MS 3000 // how long a peer query collects answers (esp mdns sends it about once a second)
#define TREL_QUERY_DELAY_MS 100 // after an address event: a burst of them gets one query
// The radio task (br_radio): OpenThread's start and stop on the OTBR channel, moves, the channel's GONE and return.
#define RADIO_TASK_NAME "br_radio"
#define RADIO_TASK_STACK 6144
#define RADIO_TASK_PRIORITY 5
#define MOVE_PROBE_WAIT_MS 10000 // the PUT that moves Thread waits this long for the target's probe
#define PROBE_RESET_WAIT_MS 2000 // the RCP's reset notification after a spinel reset
#define PROBE_ANSWER_WAIT_MS 500 // each property answer
#define PROBE_POLL_SLICE_MS 50
#define FAULT_NO_CHANNEL_START "OpenThread did not start on its channel"
#define FAULT_C6_HELD "rcp incompatible" // the test build's c6_hold: the C6 stays in reset, as an RCP that never answers

_Static_assert(BR_CORE_CHANNEL_ID_MAX == RADIO_MUX_ID_MAX, "br_core_status_t.otbr_channel holds a channel id");
_Static_assert(OTBR_RADIO_CAP_ACK_TIMEOUT == OT_RADIO_CAPS_ACK_TIMEOUT &&
               OTBR_RADIO_CAP_TRANSMIT_RETRIES == OT_RADIO_CAPS_TRANSMIT_RETRIES &&
               OTBR_RADIO_CAP_CSMA_BACKOFF == OT_RADIO_CAPS_CSMA_BACKOFF &&
               OTBR_RADIO_CAP_TRANSMIT_SEC == OT_RADIO_CAPS_TRANSMIT_SEC &&
               OTBR_RADIO_CAP_TRANSMIT_TIMING == OT_RADIO_CAPS_TRANSMIT_TIMING, "otbr_probe.h radio caps");

typedef struct {
    StaticSemaphore_t mutex_storage;
    SemaphoreHandle_t mutex;     // guards the fields below
    SemaphoreHandle_t boot_done; // given once OpenThread is initialised or the fault mode was entered
    SemaphoreHandle_t loop_done; // given by the OpenThread task when it left the main loop for a firmware push
    SemaphoreHandle_t bringup_done; // given when the border router task ended
    SemaphoreHandle_t sessions_done; // given when the last open OpenThread session closed
    esp_netif_t *backbone;
    esp_netif_t *ot_netif;
    TaskHandle_t ot_task;
    char fault_reason[64];
    char rcp_installed[32];    // version from the last probe, "" = none
    bool rcp_installed_known;  // the last probe reached the updater
    bool rcp_compatible;       // OpenThread accepted the RCP at the last start
    bool rom_recovery_failed;  // a ROM recovery failed and no later one worked
    bool ot_ready;             // OpenThread runs and takes calls; cleared before a push tears it down
    bool ot_initialised;       // esp_openthread_init succeeded and not yet torn down
    bool init_abandoned;       // OpenThread's start was abandoned half-way (its SPI driver stays registered): only a reboot restarts it
    bool address_seen;
    bool bringup_started;
    bool border_router_up;
    bool loop_running;
    bool push_pending;         // a firmware push asked the OpenThread task to leave its main loop
    uint32_t ot_sessions;      // open br_core_ot_session_begin sessions; teardown waits for 0
    atomic_bool push_busy;     // a firmware push is running (one long operation at a time)
    bool started;
    bool mdns_started;
    char mdns_hostname[64]; // as given to br_core_mdns_start, for the retry in br_core_start
    char mdns_instance[64];
} core_state_t;

static core_state_t s_core;
static bool s_storage_degraded; // set by br_core_note_storage_init before br_core_start
// Stored leader weight adjustment (NVS thread/leader_adj), loaded by br_core_note_storage_init; 0 when nothing is stored
// or storage is degraded. Read without any lock (bring-up, the HTTP handlers); written only by the setter, under
// s_leader_mutex.
static atomic_int s_leader_adj;
static StaticSemaphore_t s_leader_mutex_buf;
static SemaphoreHandle_t s_leader_mutex; // serialises compare, NVS write and store in br_core_set_leader_adjustment
static const mode_fsm_ops_t s_ops;
// The only owner of the mode (spec 5.4). Its operations run under the OpenThread lock, except the push.
static mode_fsm_t s_fsm = {.ops = &s_ops, .mode = BR_MODE_STARTING};
// What the sniffer must put back: Thread and netif state and the active dataset.
static struct {
    bool thread_enabled;
    bool ip6_enabled;
    bool has_dataset;
    otOperationalDatasetTlvs dataset;
} s_saved;
// Runs once the border router is up, with the OpenThread lock held (br_core_set_border_router_up_cb).
static void (*s_border_router_up_cb)(otInstance *instance);
// The platform config must outlive esp_openthread_init: OpenThread keeps pointers into it.
static esp_openthread_platform_config_t s_platform_config;

// The channel OpenThread runs on (the context of the mux operations). `gone`: the channel was detached or posted GONE;
// from then on the interface refuses requests until OpenThread stops. Set on every start.
typedef struct {
    radio_mux_chan_t *_Atomic ch;
    atomic_bool gone;
} otbr_link_t;
static otbr_link_t s_link;
// The otbr consumer's attached channel (set by the mux owner task in attach and detach) and the count of attaches.
static radio_mux_chan_t *_Atomic s_attached;
static atomic_uint s_attach_gen;
static TaskHandle_t _Atomic s_radio_task;
static const esp_openthread_mux_ops_t s_mux_ops;
static bool s_move_probe;   // br_radio: the next probe is a move's (its verdict answers the waiting PUT)
static bool s_start_failed; // br_radio: the last start on the attached channel failed (RADIO_FAULT from it)
static esp_err_t op_probe_channel(const char *id, char *err, size_t cap);
static esp_err_t op_ot_start(const char *id);
static esp_err_t op_ot_stop(void);
static esp_err_t op_store_otbr(const char *from, const char *to);
static void boot_otbr(void);
static void stop_openthread(void);
// Owned by the task that starts and stops OpenThread (br_radio, or a radio job while push_busy):
static radio_mux_chan_t *s_ot_chan; // the channel OpenThread was started on, NULL when it is stopped
static unsigned s_ot_gen;           // the attach it was started on: another attach since means the channel went
static unsigned s_tried_gen;        // the last attach a start was tried on (a failed start waits for the next one)
static radio_mux_chan_t *s_lease;   // a move's target, leased from the mux until the move ends
static radio_mux_fw_record_t s_probe_fw; // what the last probe read
static char s_probe_err[64];        // why the last probe failed
// A move of OpenThread: the caller of br_core_move_otbr waits for the probe's verdict, br_radio runs the move.
enum { MOVE_IDLE, MOVE_WAITING, MOVE_SIGNALLED, MOVE_ABANDONED };
static struct {
    atomic_bool running;   // from the request until br_radio finished it
    atomic_bool queued;    // for br_radio: run it
    atomic_int state;
    SemaphoreHandle_t probed;
    char from[RADIO_MUX_ID_MAX], to[RADIO_MUX_ID_MAX];
    esp_err_t result;
    char reason[RADIO_MUX_REASON_MAX];
} s_move;

static bool ot_ready_now(void);
// TREL setting (br_core_set_trel); applied after every esp_openthread_init.
static atomic_bool s_trel_wanted = true;
// br_core_set_ot_cli_hook; called after every OpenThread start and before every stop.
static void (*_Atomic s_ot_cli_hook)(bool running);
#if CONFIG_OPENTHREAD_RADIO_TREL
static esp_timer_handle_t s_trel_query_timer; // runs send_trel_query
static mdns_search_once_t *s_trel_query;      // the last peer query; used by the esp_timer task only
#endif

static void set_reason(const char *reason)
{
    xSemaphoreTake(s_core.mutex, portMAX_DELAY);
    strlcpy(s_core.fault_reason, reason, sizeof(s_core.fault_reason));
    xSemaphoreGive(s_core.mutex);
}

// Radio fault: the thread REST API is switched off first, then the mode changes.
static void enter_fault(const char *reason)
{
    esp_br_web_set_openthread_ready(false);
    set_reason(reason);
    ESP_LOGE(TAG, "radio fault: %s", reason);
    mode_fsm_set_fault(&s_fsm);
    xSemaphoreGive(s_core.boot_done);
}

// OpenThread aborts the process when its radio callbacks return: park this task instead. The fault
// state is what the rest of the firmware sees; the HTTP server and the management path keep running.
//
// OpenThread lock: during esp_openthread_init (s_core.ot_ready is still false) the callbacks run before the
// lock is taken. After init they run from the main loop, which holds the lock exactly once around
// esp_openthread_platform_process, so one release lets other tasks take the lock again after this task is gone.
//
// OpenThread runs the hook in whichever task used the radio (httpd, thread_rest, pcap_stream, bring-up too). Such a task
// must not be deleted (without httpd the board has no HTTP until a power cycle): the device reboots instead. OpenThread
// would abort if the hook returned, and sniffer state is never persisted, so the reboot loses nothing.
static void fault_and_park(const char *reason)
{
    // Not ot_ready: a push clears that while the main loop (and its hold of the lock) is still running.
    const bool in_mainloop = s_core.loop_running;

    if (mode_fsm_fault_action(xTaskGetCurrentTaskHandle() == s_core.ot_task) == MODE_FSM_FAULT_REBOOT) {
        enter_fault(reason);
        ESP_LOGE(TAG, "radio failure reported in task %s: rebooting", pcTaskGetName(NULL));
        esp_restart();
    }

    if (!in_mainloop && !s_core.ot_initialised) {
        s_core.init_abandoned = true; // parked inside esp_openthread_init: nothing of it can be released from outside
    }
    enter_fault(reason);
    if (in_mainloop) {
        esp_openthread_lock_release();
    }
    s_core.loop_running = false;
    s_core.ot_task = NULL;
    vTaskDelete(NULL);
}

static void set_compatible(bool compatible)
{
    xSemaphoreTake(s_core.mutex, portMAX_DELAY);
    s_core.rcp_compatible = compatible;
    xSemaphoreGive(s_core.mutex);
}

// A dead or mismatching RCP while OpenThread starts is reported as "rcp incompatible": the cure is the same, a push.
static void on_coprocessor_reset_failure(void)
{
    if (atomic_load(&s_link.gone)) {
        // The channel went away during a recovery: br_radio tears OpenThread down and ends in NO_RADIO.
        fault_and_park("the radio channel is gone");
    }
    set_compatible(false);
    // loop_running, not ot_ready: a push clears ot_ready while OpenThread still runs.
    fault_and_park(s_core.loop_running ? "RCP did not answer its reset" : FAULT_INCOMPATIBLE);
}

static void on_compatibility_error(void)
{
    set_compatible(false);
    fault_and_park(FAULT_INCOMPATIBLE);
}

// Reset of the OTBR channel's radio, from the recovery ladder (OpenThread's hardware-reset hook): the C6's reset GPIO
// or a dongle's close and reopen, through the mux. The interface waits for the RESET (mux_spinel_interface.cpp).
static esp_err_t reset_rcp(void)
{
    radio_mux_chan_t *ch = atomic_load(&s_link.ch);

    return ch ? radio_mux_reset(ch) : ESP_ERR_INVALID_STATE;
}

static esp_err_t op_reset_rcp(void)
{
    esp_err_t err = reset_rcp();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "resetting the RCP failed: %s", esp_err_to_name(err));
    }
    return err;
}

// OpenThread's hardware-reset hook, called from the main loop (lock held). Before OpenThread is up (init, boot
// wait) it only resets the C6.
static void on_rcp_failure(void)
{
    if (atomic_load(&s_core.push_busy) && !s_core.loop_running) {
        return; // the push stopped OpenThread: it owns the C6 and resets it itself
    }
    // While a push waits for the OpenThread sessions to end (the mode is STARTING) the C6 is only reset, below.
    if (!s_core.ot_ready || br_core_get_mode() == BR_MODE_STARTING) {
        op_reset_rcp();
        return;
    }
    esp_br_web_set_openthread_ready(false);
    mode_fsm_on_rcp_failure(&s_fsm);
    esp_br_web_set_openthread_ready(br_core_get_mode() == BR_MODE_BR_RUNNING);
}

// The radio is the mux channel (esp_openthread_mux_set); of the SPI settings only the bus speed is read (by
// otPlatRadioGetBusSpeed, for the frame transfer time): SPI2's clock for the C6, the line's baud for a dongle.
static esp_openthread_platform_config_t platform_config(uint32_t bus_speed)
{
    const esp_openthread_platform_config_t config = {
        .radio_config = {
            .radio_mode = RADIO_MODE_SPI_RCP,
            .radio_spi_config = {
                .host_device = SPI2_HOST,
                .spi_device = {.clock_speed_hz = (int)bus_speed},
                .intr_pin = -1,
            },
        },
        .host_config = {.host_connection_mode = HOST_CONNECTION_MODE_NONE},
        .port_config = {.storage_partition_name = "nvs", .netif_queue_size = 10, .task_queue_size = 10},
    };
    return config;
}

// Bits per second between the P4 and the RCP of channel `id`.
static uint32_t channel_bus_speed(const char *id)
{
    uint32_t speed = P4_RCP_SPI_HZ;

    if (strcmp(id, RADIO_MUX_C6_ID) == 0) {
        return speed;
    }
    radio_mux_snapshot_t *snap = malloc(sizeof(*snap));

    if (snap) {
        radio_mux_get_snapshot(snap);
        for (size_t i = 0; i < snap->n; i++) {
            if (strcmp(snap->chans[i].id, id) == 0) {
                speed = snap->chans[i].line.baud;
            }
        }
        free(snap);
    }
    return speed;
}

// The stored leader weight adjustment into OpenThread's device properties (which recompute the local leader weight).
// Called with the OpenThread lock held. OpenThread keeps the properties in the host instance only: they last until
// esp_openthread_deinit, so every (re)start applies them again in bringup_task.
static void apply_leader_adjustment(otInstance *instance, int8_t adj)
{
    otDeviceProperties props = *otThreadGetDeviceProperties(instance);

    props.mLeaderWeightAdjustment = adj;
    otThreadSetDeviceProperties(instance, &props);
}

// Border router start: the border-router-up hook (DHCPv6-PD, OMR), Thread on when a dataset is stored. Spec 5.3 step 5.
static void bringup_task(void *arg)
{
    otInstance *instance = esp_openthread_get_instance();

    ESP_LOGI(TAG, "starting the border router");
    // The session keeps a push from tearing OpenThread down under the border router start.
    if (!br_core_ot_session_begin()) {
        ESP_LOGW(TAG, "OpenThread is being stopped: no border router start");
        xSemaphoreGive(s_core.bringup_done);
        vTaskDelete(NULL);
        return;
    }
    esp_openthread_lock_acquire(portMAX_DELAY);
    esp_err_t err = esp_openthread_border_router_init();
    if (err != ESP_OK) {
        // The radio works, the border router does not: not a radio fault, the mode stays STARTING.
        esp_openthread_lock_release();
        br_core_ot_session_end();
        ESP_LOGE(TAG, "border router init: %s", esp_err_to_name(err));
        set_reason("border router init failed");
        xSemaphoreGive(s_core.bringup_done);
        vTaskDelete(NULL);
        return;
    }
    s_core.border_router_up = true;
    if (s_border_router_up_cb) {
        s_border_router_up_cb(instance);
    } else {
        ESP_LOGW(TAG, "no routing hook registered, DHCPv6-PD stays off");
    }

    // Before Thread starts, so that the first leader weight it advertises already has the adjustment.
    const int8_t adj = (int8_t)atomic_load(&s_leader_adj);
    apply_leader_adjustment(instance, adj);
    if (adj != 0) {
        ESP_LOGI(TAG, "leader weight adjustment %+d (weight %u)", adj, otThreadGetLocalLeaderWeight(instance));
    }

    if (otDatasetIsCommissioned(instance)) {
        otError ip6 = otIp6SetEnabled(instance, true);
        otError thread = ip6 == OT_ERROR_NONE ? otThreadSetEnabled(instance, true) : ip6;
        if (thread == OT_ERROR_NONE) {
            ESP_LOGI(TAG, "Thread started with the stored dataset");
        } else {
            ESP_LOGE(TAG, "starting Thread failed: %s", otThreadErrorToString(thread));
            set_reason("starting Thread failed");
        }
    } else {
        ESP_LOGI(TAG, "no stored dataset, Thread stays idle");
    }
    // A radio fault entered meanwhile (e.g. the boot wait timed out) stays. Under the lock, where a push changes the
    // mode: a push that began meanwhile (mode STARTING, ot_ready cleared) keeps its STARTING.
    if (ot_ready_now()) {
        mode_fsm_set_started(&s_fsm);
    }
    esp_openthread_lock_release();
    br_core_ot_session_end();
    xSemaphoreGive(s_core.bringup_done);
    vTaskDelete(NULL);
}

// Called with the mutex held: start the border router once both OpenThread and an address exist.
static void maybe_start_bringup(void)
{
    if (!s_core.ot_ready || !s_core.address_seen || s_core.bringup_started) {
        return;
    }
    s_core.bringup_started = true;
    if (xTaskCreatePinnedToCore(bringup_task, "br_bringup", BRINGUP_TASK_STACK, NULL, BRINGUP_TASK_PRIORITY, NULL,
                                NET_CORE) != pdPASS) {
        ESP_LOGE(TAG, "cannot create the border router task");
        s_core.bringup_started = false;
    }
}

static void note_address(void)
{
    xSemaphoreTake(s_core.mutex, portMAX_DELAY);
    s_core.address_seen = true;
    maybe_start_bringup();
    xSemaphoreGive(s_core.mutex);
}

// The IDF port browses for TREL peers with a single query, sent when TREL is enabled. That is before the backbone has an
// address, or before the switch port forwards, so the query is lost; esp mdns repeats browses on a new address only
// for its predefined netifs, which netcfg does not use. Peers already on the link would never be found. A PTR query
// once an address shows the link works makes them answer, and the browse takes the answers.
#if CONFIG_OPENTHREAD_RADIO_TREL
// A query still running may have sent its last packet before the address came: start a new one once it has ended.
static void send_trel_query(void *arg)
{
    if (!atomic_load(&s_trel_wanted)) {
        return;
    }
    if (s_trel_query) {
        mdns_result_t *results = NULL;

        if (!mdns_query_async_get_results(s_trel_query, 0, &results, NULL)) {
            esp_timer_start_once(s_trel_query_timer, TREL_QUERY_MS * 1000ULL);
            return;
        }
        mdns_query_results_free(results);
        mdns_query_async_delete(s_trel_query);
    }
    s_trel_query = mdns_query_async_new(NULL, "_trel", "_udp", MDNS_TYPE_PTR, TREL_QUERY_MS, 1, NULL);
}
#endif

static void query_trel_peers(void)
{
#if CONFIG_OPENTHREAD_RADIO_TREL
    if (s_trel_query_timer) {
        esp_timer_stop(s_trel_query_timer); // not armed: nothing to stop
        esp_timer_start_once(s_trel_query_timer, TREL_QUERY_DELAY_MS * 1000ULL);
    }
#endif
}

static void on_got_ip6(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const ip_event_got_ip6_t *event = data;

    if (event->esp_netif == s_core.backbone) {
        note_address();
        // A link-local address exists before the switch port forwards; a SLAAC address needed a router advertisement.
        if (esp_netif_ip6_get_addr_type(&event->ip6_info.ip) != ESP_IP6_ADDR_IS_LINK_LOCAL) {
            query_trel_peers();
        }
    }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const ip_event_got_ip_t *event = data;

    if (event->esp_netif == s_core.backbone) {
        query_trel_peers();
    }
}

static bool backbone_has_ip6(void)
{
    esp_ip6_addr_t addresses[CONFIG_LWIP_IPV6_NUM_ADDRESSES];

    return esp_netif_get_all_ip6(s_core.backbone, addresses) > 0;
}

/** The SW version in Network Diagnostic answers: "YYYYMMDD-<7 hex>" from the build stamp. A failure is only logged. */
static void set_vendor_sw_version(void)
{
    char version[BR_CORE_VENDOR_SW_VERSION_SIZE];

    br_core_vendor_sw_version(esp_app_get_description()->version, version);
    esp_openthread_lock_acquire(portMAX_DELAY);
    otError err = otThreadSetVendorSwVersion(esp_openthread_get_instance(), version);
    esp_openthread_lock_release();
    if (err != OT_ERROR_NONE) {
        ESP_LOGW(TAG, "vendor SW version \"%s\": %s", version, otThreadErrorToString(err));
    }
}

static esp_err_t init_openthread(void)
{
    // The eventfd VFS is registered once in app_main (BR_CORE_EVENTFD_COUNT plus the mux's), before radio_mux_start.
    esp_openthread_register_rcp_failure_handler(on_rcp_failure);
    esp_openthread_set_coprocessor_reset_failure_callback(on_coprocessor_reset_failure);
    esp_openthread_set_compatibility_error_callback(on_compatibility_error);
    esp_openthread_set_backbone_netif(s_core.backbone);

    s_platform_config = platform_config(channel_bus_speed(radio_mux_chan_id(atomic_load(&s_link.ch))));
    esp_openthread_mux_set(&s_mux_ops, &s_link); // before every esp_openthread_init: the channel it runs on
    ESP_RETURN_ON_ERROR(esp_openthread_init(&s_platform_config), TAG, "esp_openthread_init");
    s_core.ot_initialised = true;
    set_vendor_sw_version();
#if CONFIG_OPENTHREAD_RADIO_TREL
    esp_openthread_lock_acquire(portMAX_DELAY);
    otTrelSetEnabled(esp_openthread_get_instance(), atomic_load(&s_trel_wanted));
    esp_openthread_lock_release();
#endif

    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_OPENTHREAD();
    esp_netif_t *netif = esp_netif_new(&netif_config);
    ESP_RETURN_ON_FALSE(netif, ESP_ERR_NO_MEM, TAG, "creating the OpenThread netif");
    s_core.ot_netif = netif;
    ESP_RETURN_ON_ERROR(esp_netif_attach(netif, esp_openthread_netif_glue_init(&s_platform_config)), TAG,
                        "attaching the OpenThread netif");
    void (*cli_hook)(bool) = atomic_load(&s_ot_cli_hook);

    if (cli_hook) {
        esp_openthread_lock_acquire(portMAX_DELAY);
        cli_hook(true);
        esp_openthread_lock_release();
    }
    return ESP_OK;
}

// The boot-time probe, best effort. The test build can force the result of the first probe of a boot; the probe
// after a firmware push (which restarts the radio through here) is always real.
static esp_err_t run_probe(rcp_sync_probe_result_t *probe)
{
#ifdef OTBR_TEST_BUILD
    static bool override_used;
    const br_core_test_probe_t forced = override_used ? BR_CORE_TEST_PROBE_REAL : br_core_test_probe_override();

    override_used = true;
    if (forced != BR_CORE_TEST_PROBE_REAL) {
        ESP_LOGW(TAG, "test build: probe forced to %s", forced == BR_CORE_TEST_PROBE_EMPTY ? "empty" : "fail");
        memset(probe, 0, sizeof(*probe));
        probe->updater_reachable = forced == BR_CORE_TEST_PROBE_EMPTY;
        strlcpy(probe->last_error, "forced by the test build", sizeof(probe->last_error));
        return ESP_OK;
    }
#endif
    return rcp_sync_probe(probe);
}

// The C6's own boot steps (spec 5.3 steps 3 and 4), inside a C6 job (radio_mux_c6_job_begin/_end): probe the installed
// RCP over the updater (rcp_sync, SPI3), the ROM recovery it may run, then reset the C6 into its RCP. Nothing is pushed.
// The status fields follow the probe. `affects_mode`: the C6 is the OTBR channel, so what stops it from running
// OpenThread is a radio fault (as before the mux); otherwise only the status changes. Returns ESP_OK when the C6 should
// run its RCP; ESP_ERR_INVALID_STATE for the test build's c6_hold (the C6 stays in reset), ESP_ERR_NOT_FOUND otherwise.
static esp_err_t c6_boot_steps(bool affects_mode)
{
    rcp_sync_probe_result_t probe;
    esp_err_t err = run_probe(&probe);
    const char *fault = NULL;
    bool held = false;

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RCP probe could not run: %s", esp_err_to_name(err));
        memset(&probe, 0, sizeof(probe));
    }
    xSemaphoreTake(s_core.mutex, portMAX_DELAY);
    s_core.rcp_installed_known = probe.updater_reachable;
    strlcpy(s_core.rcp_installed, probe.installed_version, sizeof(s_core.rcp_installed));
    if (affects_mode) {
        s_core.rcp_compatible = false;
    }
    s_core.rom_recovery_failed = mode_fsm_next_recovery_failed(s_core.rom_recovery_failed,
                                                               probe.rom_recovery_failed || probe.rom_recovered, probe.rom_recovered);
    xSemaphoreGive(s_core.mutex);
    if (mode_fsm_rom_failure_is_fault(&probe)) {
        // Spec 7: the updater was silent and the recovery failed; no further automatic attempt this boot.
        ESP_LOGE(TAG, "the ROM recovery of the C6 base image failed");
        fault = FAULT_ROM_RECOVERY;
    } else if (probe.esp_hosted) {
        // A factory C6: OpenThread cannot run on it. The device page asks for c6_installer.bin.
        ESP_LOGE(TAG, "the C6 runs its factory ESP-Hosted firmware: upload c6_installer.bin, then c6_rcp.bin");
        fault = RCP_SYNC_ESP_HOSTED_ERROR;
    } else if (probe.updater_reachable && !probe.installed_valid) {
        ESP_LOGE(TAG, "the C6 holds no RCP firmware");
        fault = FAULT_NO_RCP;
    }
    if (probe.rom_recovery_failed && !fault) {
        // The updater answered (base version mismatch): an RCP may be installed, so the boot goes on with it.
        ESP_LOGE(TAG, "the ROM recovery of the C6 base image failed; continuing with the installed RCP (see status jumper.recovery_failed)");
    }
    if (!fault && !probe.updater_reachable) {
        ESP_LOGW(TAG, "RCP probe failed (%s), trying the installed firmware", probe.last_error);
    } else if (!fault) {
        ESP_LOGI(TAG, "installed RCP firmware %s", probe.installed_version);
    }
#ifdef OTBR_TEST_BUILD
    if (!fault && br_core_test_c6_hold()) {
        ESP_LOGW(TAG, "test build: holding the C6 in reset");
        gpio_set_level(P4_PIN_C6_RESET, 0);
        fault = FAULT_C6_HELD;
        held = true;
    }
#endif
    if (!fault && rcp_sync_boot_rcp() != ESP_OK) {
        fault = "resetting the RCP failed";
    }
    if (fault && affects_mode) {
        enter_fault(fault);
    }
    return held ? ESP_ERR_INVALID_STATE : fault ? ESP_ERR_NOT_FOUND : ESP_OK;
}

// The OpenThread task: initialises OpenThread on the channel in s_link (br_core_ot_start), then runs its main loop.
static void ot_task(void *arg)
{
    const esp_err_t init_err = init_openthread();

    if (init_err != ESP_OK) {
        if (!s_core.ot_initialised) {
            s_core.init_abandoned = true;
        }
        enter_fault(FAULT_INCOMPATIBLE);
        s_core.ot_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    set_compatible(true);
    xSemaphoreTake(s_core.mutex, portMAX_DELAY);
    s_core.ot_ready = true;
    if (!atomic_load(&s_core.push_busy)) {
        // During a push the mode is transiently STARTING; br_core_push_radio_firmware sets the flag from the final state.
        esp_br_web_set_openthread_ready(br_core_get_mode() != BR_MODE_RADIO_FAULT);
    }
    if (backbone_has_ip6()) {
        s_core.address_seen = true;
    }
    maybe_start_bringup();
    xSemaphoreGive(s_core.mutex);
    ESP_LOGI(TAG, "OpenThread initialised on %s", radio_mux_chan_id(atomic_load(&s_link.ch)));
    s_core.loop_running = true;
    xSemaphoreGive(s_core.boot_done);

    esp_err_t err = esp_openthread_launch_mainloop();
    s_core.loop_running = false;
    s_core.ot_task = NULL;
    if (s_core.push_pending) {
        // A stop (push, move, the channel gone) asked for the exit and tears OpenThread down now.
        xSemaphoreGive(s_core.loop_done);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGE(TAG, "OpenThread main loop ended: %s", esp_err_to_name(err));
    enter_fault("OpenThread main loop ended");
    vTaskDelete(NULL);
}

static esp_err_t start_ot_task(void)
{
    return xTaskCreatePinnedToCore(ot_task, OT_TASK_NAME, OT_TASK_STACK, NULL, OT_TASK_PRIORITY, &s_core.ot_task,
                                   NET_CORE) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t br_core_mdns_start(const char *hostname, const char *instance)
{
    ESP_RETURN_ON_FALSE(hostname && hostname[0] && instance, ESP_ERR_INVALID_ARG, TAG, "no mDNS names");
    if (s_core.mdns_started) {
        return ESP_OK;
    }
    if (hostname != s_core.mdns_hostname) {
        strlcpy(s_core.mdns_hostname, hostname, sizeof(s_core.mdns_hostname));
        strlcpy(s_core.mdns_instance, instance, sizeof(s_core.mdns_instance));
    }
    ESP_RETURN_ON_ERROR(mdns_init(), TAG, "mdns_init");
    ESP_RETURN_ON_ERROR(mdns_hostname_set(s_core.mdns_hostname), TAG, "mdns_hostname_set");
    ESP_RETURN_ON_ERROR(mdns_instance_name_set(s_core.mdns_instance), TAG, "mdns_instance_name_set");
    s_core.mdns_started = true;
    ESP_LOGI(TAG, "mDNS hostname %s.local", s_core.mdns_hostname);
    return ESP_OK;
}

// The stored leader weight adjustment, once NVS is usable. Nothing stored (or a value out of range): 0.
static void load_leader_adjustment(void)
{
    nvs_handle_t handle;
    int8_t value = 0;

    const esp_err_t open_err = nvs_open(LEADER_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (open_err != ESP_OK) {
        if (open_err != ESP_ERR_NVS_NOT_FOUND) { // the namespace does not exist before the first write
            ESP_LOGW(TAG, "reading the leader weight adjustment: nvs_open: %s, using 0", esp_err_to_name(open_err));
        }
        return;
    }
    const esp_err_t err = nvs_get_i8(handle, LEADER_NVS_KEY, &value);
    nvs_close(handle);
    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "reading the leader weight adjustment: %s, using 0", esp_err_to_name(err));
        }
        return;
    }
    if (!br_core_leader_adjustment_valid(value)) {
        ESP_LOGW(TAG, "stored leader weight adjustment %d is out of range, using 0", value);
        return;
    }
    atomic_store(&s_leader_adj, value);
}

void br_core_note_storage_init(esp_err_t storage_init_err)
{
    if (!s_leader_mutex) { // once, before any HTTP handler can run
        s_leader_mutex = xSemaphoreCreateMutexStatic(&s_leader_mutex_buf);
    }
    s_storage_degraded = mode_fsm_storage_degraded(storage_init_err);
    if (s_storage_degraded) {
        ESP_LOGE(TAG, "storage init failed (%s): booting DEGRADED, OpenThread will not start; NVS is NOT erased "
                 "(it holds the Thread credentials)", esp_err_to_name(storage_init_err));
        return;
    }
    load_leader_adjustment();
}

esp_err_t br_core_start(esp_netif_t *backbone)
{
    ESP_RETURN_ON_FALSE(backbone, ESP_ERR_INVALID_ARG, TAG, "no backbone netif");
    ESP_RETURN_ON_FALSE(!s_core.started, ESP_ERR_INVALID_STATE, TAG, "already started");
    s_core.backbone = backbone;
    s_core.mutex = xSemaphoreCreateMutexStatic(&s_core.mutex_storage);
    s_core.boot_done = xSemaphoreCreateBinary();
    s_core.loop_done = xSemaphoreCreateBinary();
    s_core.bringup_done = xSemaphoreCreateBinary();
    s_core.sessions_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_core.boot_done && s_core.loop_done && s_core.bringup_done && s_core.sessions_done, ESP_ERR_NO_MEM,
                        TAG, "no memory");
    // Only now: the status and session calls (the HTTP server already runs) use the objects above once this is set.
    s_core.started = true;

    // mDNS first (normally started earlier by app_main): it answers while the radio boots.
    // Not essential for the radio: without mDNS the board is reachable by its address, and an early return here would leave
    // the mode STARTING with no reason and no OpenThread task.
    const esp_err_t mdns_err = br_core_mdns_start(s_core.mdns_hostname, s_core.mdns_instance);

    if (mdns_err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS did not start (%s), continuing without it", esp_err_to_name(mdns_err));
    }
#if CONFIG_OPENTHREAD_RADIO_TREL
    const esp_timer_create_args_t trel_query_timer = {.callback = send_trel_query, .name = "trel_query"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&trel_query_timer, &s_trel_query_timer), TAG, "TREL query timer");
#endif
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_GOT_IP6, on_got_ip6, NULL), TAG, "event handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, NULL), TAG, "event handler");

    boot_otbr(); // NO_RADIO, a fault of the C6's boot steps, or OpenThread starting on br_radio once the channel attaches
    if (xSemaphoreTake(s_core.boot_done, pdMS_TO_TICKS(BOOT_WAIT_MS)) != pdTRUE) {
        enter_fault("radio boot timed out");
    }
    return ESP_OK;
}

br_core_mode_t br_core_get_mode(void)
{
    return mode_fsm_get_mode(&s_fsm);
}

bool br_core_thread_ops_allowed(void)
{
    return mode_fsm_thread_ops_allowed(&s_fsm);
}

// Everything br_core_get_status reports without OpenThread. Sets *ot_ready and *border_router for the caller.
static void get_status_brief(br_core_status_t *out, bool *ot_ready, bool *border_router)
{
    memset(out, 0, sizeof(*out));
    out->mode = br_core_get_mode();
    out->pd_state = -1;
    out->storage_degraded = s_storage_degraded;
    *ot_ready = false;
    *border_router = false;
    if (!s_core.started) {
        return;
    }
    const char *otbr = radio_mux_otbr_channel(); // not under br_core's mutex: it takes the mux's lock

    strlcpy(out->otbr_channel, otbr ? otbr : "", sizeof(out->otbr_channel));
    xSemaphoreTake(s_core.mutex, portMAX_DELAY);
    strlcpy(out->rcp_installed, s_core.rcp_installed, sizeof(out->rcp_installed));
    out->rcp_installed_known = s_core.rcp_installed_known;
    out->rcp_compatible = s_core.rcp_compatible;
    out->rom_recovery_failed = s_core.rom_recovery_failed;
    strlcpy(out->fault_reason, s_core.fault_reason, sizeof(out->fault_reason));
    *ot_ready = s_core.ot_ready && out->mode != BR_MODE_RADIO_FAULT;
    *border_router = s_core.bringup_started;
    xSemaphoreGive(s_core.mutex);
}

void br_core_get_status_brief(br_core_status_t *out)
{
    bool ot_ready, border_router;

    get_status_brief(out, &ot_ready, &border_router);
}

void br_core_get_status(br_core_status_t *out)
{
    bool ot_ready, border_router;

    get_status_brief(out, &ot_ready, &border_router);
    if (!ot_ready || !br_core_ot_session_begin()) {
        return;
    }
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(STATUS_LOCK_MS))) {
        br_core_ot_session_end();
        return;
    }
    out->ot_state_valid = true;
    otInstance *instance = esp_openthread_get_instance();
    otOperationalDataset dataset;
    out->has_dataset = otDatasetIsCommissioned(instance);
    if (otDatasetGetActive(instance, &dataset) == OT_ERROR_NONE && dataset.mComponents.mIsChannelPresent) {
        out->channel = (uint8_t)dataset.mChannel;
    }
    if (border_router && out->mode == BR_MODE_BR_RUNNING) {
        otBorderRoutingPrefixTableEntry entry;
        out->pd_state = (int)otBorderRoutingDhcp6PdGetState(instance);
        if (otBorderRoutingGetPdOmrPrefix(instance, &entry) == OT_ERROR_NONE) {
            otIp6PrefixToString(&entry.mPrefix, out->pd_prefix, sizeof(out->pd_prefix));
        }
    }
    esp_openthread_lock_release();
    br_core_ot_session_end();
}

// OpenThread can be called: initialised and not in the radio fault mode (where the init may be parked).
static bool ot_callable(void)
{
    if (!s_core.started) {
        return false;
    }
    xSemaphoreTake(s_core.mutex, portMAX_DELAY);
    const bool callable = s_core.ot_ready && br_core_get_mode() != BR_MODE_RADIO_FAULT;

    xSemaphoreGive(s_core.mutex);
    return callable;
}

#if CONFIG_OPENTHREAD_RADIO_TREL
// A session around the OpenThread access: a radio push otherwise deletes the lock and the instance while this task
// waits for the lock (patch 22 in PATCHES.md, br_core.h). False, with nothing open, when OpenThread is not callable.
static bool trel_session_begin(void)
{
    if (!ot_callable()) {
        return false;
    }
    if (!br_core_ot_session_begin()) {
        return false;
    }
    if (!ot_callable()) { // a radio fault came in between
        br_core_ot_session_end();
        return false;
    }
    return true;
}
#endif

esp_err_t br_core_set_trel(bool enabled)
{
#if CONFIG_OPENTHREAD_RADIO_TREL
    atomic_store(&s_trel_wanted, enabled);
    if (!trel_session_begin()) {
        return ESP_ERR_INVALID_STATE; // stored, applied by the next esp_openthread_init
    }
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(TREL_SET_LOCK_MS))) {
        br_core_ot_session_end();
        ESP_LOGW(TAG, "TREL %s: OpenThread busy, applied at the next start", enabled ? "on" : "off");
        return ESP_ERR_TIMEOUT;
    }
    if (!ot_callable()) { // a radio fault while this task waited for the lock
        esp_openthread_lock_release();
        br_core_ot_session_end();
        return ESP_ERR_INVALID_STATE;
    }
    otTrelSetEnabled(esp_openthread_get_instance(), enabled);
    esp_openthread_lock_release();
    br_core_ot_session_end();
    ESP_LOGI(TAG, "TREL %s", enabled ? "on" : "off");
    return ESP_OK;
#else
    (void)enabled;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

bool br_core_trel_wanted(void)
{
    return atomic_load(&s_trel_wanted);
}

#if CONFIG_OPENTHREAD_RADIO_TREL
// Serialised by the OpenThread lock, which every caller holds: one sort buffer (about 1.5 KB) for all of them.
static br_core_trel_sort_t s_trel_sort;
#endif

void br_core_trel_read_locked(otInstance *instance, br_core_trel_state_t *out, br_core_trel_list_t *list)
{
#if CONFIG_OPENTHREAD_RADIO_TREL
    const otTrelCounters *counters = otTrelGetCounters(instance);
    otTrelPeerIterator iterator;
    const otTrelPeer *peer;

    out->state_valid = true;
    out->active = otTrelIsEnabled(instance) && otThreadGetDeviceRole(instance) != OT_DEVICE_ROLE_DISABLED;
    if (counters) {
        out->tx_packets = counters->mTxPackets;
        out->tx_bytes = counters->mTxBytes;
        out->tx_failures = counters->mTxFailure;
        out->rx_packets = counters->mRxPackets;
        out->rx_bytes = counters->mRxBytes;
    }
    br_core_trel_sort_init(&s_trel_sort, otThreadGetExtendedPanId(instance)->m8, otDatasetIsCommissioned(instance),
                           list == NULL);
    otTrelInitPeerIterator(instance, &iterator);
    while ((peer = otTrelGetNextPeer(instance, &iterator)) != NULL) {
        char sock_address[OT_IP6_SOCK_ADDR_STRING_SIZE] = "";
        br_core_trel_raw_peer_t raw = {.sock_address = sock_address};

        memcpy(raw.ext_address, peer->mExtAddress.m8, sizeof(raw.ext_address));
        memcpy(raw.ext_pan_id, peer->mExtPanId.m8, sizeof(raw.ext_pan_id));
        if (br_core_trel_sort_keeps(&s_trel_sort, raw.ext_pan_id)) { // a peer only counted needs neither
            otMultiRadioNeighborInfo info;
            br_core_trel_radios_t radios = {
                .found = otMultiRadioGetNeighborInfo(instance, &peer->mExtAddress, &info) == OT_ERROR_NONE,
            };

            if (radios.found) {
                radios.has_154 = info.mSupportsIeee802154;
                radios.pref_154 = info.mIeee802154Info.mPreference;
                radios.has_trel = info.mSupportsTrelUdp6;
                radios.pref_trel = info.mTrelUdp6Info.mPreference;
            }
            otIp6SockAddrToString(&peer->mSockAddr, sock_address, sizeof(sock_address));
            br_core_trel_raw_set_radios(&raw, &radios);
        }
        br_core_trel_sort_add(&s_trel_sort, &raw);
    }
    br_core_trel_sort_finish(&s_trel_sort, out, list);
#else
    (void)instance;
    (void)out;
    (void)list;
#endif
}

void br_core_get_trel(br_core_trel_t *out)
{
    memset(out, 0, sizeof(*out));
#if CONFIG_OPENTHREAD_RADIO_TREL
    out->built = true;
    out->enabled = br_core_trel_wanted();
    if (!trel_session_begin()) {
        return;
    }
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(STATUS_LOCK_MS))) {
        br_core_ot_session_end();
        return;
    }
    if (!ot_callable()) { // a radio fault while this task waited for the lock
        esp_openthread_lock_release();
        br_core_ot_session_end();
        return;
    }
    br_core_trel_read_locked(esp_openthread_get_instance(), &out->state, &out->list);
    esp_openthread_lock_release();
    br_core_ot_session_end();
#endif
}

// ---- mode state machine: real operations (run under the OpenThread lock) ----

// Private API of the openthread component. The border router library registers a main loop workflow named
// "dns_upstream_query"; the registry keeps only 15 characters but compares 16 when unregistering, so
// esp_openthread_border_router_deinit leaves a stale entry whose process function then fails the next main loop.
// Unregistering with the stored name removes it.
void esp_openthread_platform_workflow_unregister(const char *name);
#define DNS_UPSTREAM_WORKFLOW "dns_upstream_qu"

static esp_err_t ot_result(otError error, const char *what)
{
    if (error != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "%s: %s", what, otThreadErrorToString(error));
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t op_thread_stop(void)
{
    otInstance *instance = esp_openthread_get_instance();

    s_saved.thread_enabled = otThreadGetDeviceRole(instance) != OT_DEVICE_ROLE_DISABLED;
    s_saved.ip6_enabled = otIp6IsEnabled(instance);
    s_saved.has_dataset = otDatasetGetActiveTlvs(instance, &s_saved.dataset) == OT_ERROR_NONE;
    if (!s_saved.thread_enabled) {
        return ESP_OK;
    }
    return ot_result(otThreadSetEnabled(instance, false), "stopping Thread");
}

static esp_err_t op_netif_down(void)
{
    return s_saved.ip6_enabled ? ot_result(otIp6SetEnabled(esp_openthread_get_instance(), false), "netif down") : ESP_OK;
}

// Raw link on first, then channel and promiscuous mode: with the raw link off otLinkSetChannel would invalidate
// the datasets (spec 5.4).
static esp_err_t op_raw_enable(uint8_t channel, otLinkRawReceiveDone cb)
{
    otInstance *instance = esp_openthread_get_instance();

    ESP_RETURN_ON_ERROR(ot_result(otLinkRawSetReceiveDone(instance, cb), "raw link enable"), TAG, "raw link");
    // Enabling the raw link switches the MAC layer off, and with it the radio; the RCP refuses promiscuous mode
    // and receive on a disabled radio (and OpenThread aborts on that), so enable the radio again.
    esp_err_t err = ot_result(otPlatRadioEnable(instance), "radio enable");
    if (err == ESP_OK) {
        // Without the MAC layer nobody keeps the radio receiving between frames: the RCP would stop after the first.
        otPlatRadioSetRxOnWhenIdle(instance, true);
        err = ot_result(otLinkSetChannel(instance, channel), "raw link channel");
    }
    if (err == ESP_OK) {
        err = ot_result(otLinkRawSetPromiscuous(instance, true), "raw link promiscuous");
    }
    if (err == ESP_OK) {
        err = ot_result(otLinkRawReceive(instance), "raw link receive");
    }
    if (err != ESP_OK) {
        otLinkRawSetPromiscuous(instance, false);
        otLinkRawSleep(instance);
        otLinkRawSetReceiveDone(instance, NULL);
    }
    return err;
}

static esp_err_t op_raw_disable(void)
{
    otInstance *instance = esp_openthread_get_instance();

    if (!otLinkRawIsEnabled(instance)) {
        return ESP_OK;
    }
    otLinkRawSetPromiscuous(instance, false);
    otLinkRawSleep(instance);
    return ot_result(otLinkRawSetReceiveDone(instance, NULL), "raw link disable");
}

// The RCP failed while capturing: only OpenThread's own flag changes (no spinel request, see mode_fsm_ops_t). This
// relies on SubMac being in the Receive state, so that SubMac::Enable after the callback is cleared is a no-op and sends
// nothing to the RCP.
static esp_err_t op_raw_abort(void)
{
    otInstance *instance = esp_openthread_get_instance();

    return otLinkRawIsEnabled(instance) ? ot_result(otLinkRawSetReceiveDone(instance, NULL), "raw link abort") : ESP_OK;
}

// After the RCP reset: OpenThread restored the RCP to the capture's state (promiscuous, receiving); undo that.
static esp_err_t op_quiet_radio(void)
{
    otInstance *instance = esp_openthread_get_instance();

    otPlatRadioSetPromiscuous(instance, false);
    return ot_result(otPlatRadioSleep(instance), "radio sleep");
}

// OpenThread's task queue runs a function on the OpenThread task with the lock held. Its header is private to the
// component, the function itself is exported.
typedef void (*ot_task_fn_t)(void *);
// Must match components/openthread/private_include/esp_openthread_task_queue.h (IDF v6.1); revisit on an IDF update.
extern esp_err_t esp_openthread_task_queue_post(ot_task_fn_t task, void *arg);

typedef struct {
    void (*fn)(void *ctx);
    void *ctx;
} deferred_t;
// One slot is enough: the only caller (mode_fsm) always passes the same fn and ctx.
static deferred_t s_deferred;
static atomic_bool s_keepalive_pending;

// Runs once the failure hook has returned and OpenThread restored the RCP's state. The Thread REST flag follows the
// mode the step ended in.
static void deferred_trampoline(void *arg)
{
    const deferred_t job = *(const deferred_t *)arg;

    job.fn(job.ctx);
    esp_br_web_set_openthread_ready(br_core_get_mode() == BR_MODE_BR_RUNNING);
}

static esp_err_t op_defer(void (*fn)(void *ctx), void *ctx)
{
    s_deferred = (deferred_t) {
        fn, ctx
    };
    return esp_openthread_task_queue_post(deferred_trampoline, &s_deferred);
}

// The dataset (and with it the channel Thread starts on) is untouched by the raw link; check it, and put the saved
// one back if it is not. otLinkSetChannel is never called here: the raw link is off.
static esp_err_t op_restore_channel(void)
{
    otInstance *instance = esp_openthread_get_instance();
    otOperationalDatasetTlvs now;

    if (!s_saved.has_dataset) {
        return ESP_OK;
    }
    if (otDatasetGetActiveTlvs(instance, &now) == OT_ERROR_NONE && now.mLength == s_saved.dataset.mLength &&
            memcmp(now.mTlvs, s_saved.dataset.mTlvs, now.mLength) == 0) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "active dataset changed while sniffing, putting the saved one back");
    return ot_result(otDatasetSetActiveTlvs(instance, &s_saved.dataset), "restoring the dataset");
}

static esp_err_t op_netif_up(void)
{
    return s_saved.ip6_enabled ? ot_result(otIp6SetEnabled(esp_openthread_get_instance(), true), "netif up") : ESP_OK;
}

static esp_err_t op_thread_start(void)
{
    return s_saved.thread_enabled ? ot_result(otThreadSetEnabled(esp_openthread_get_instance(), true), "starting Thread")
           : ESP_OK;
}

// Stop everything OpenThread owns so that the SPI bus is free again. Called from a task other than the OpenThread
// task, without the lock (esp_openthread_deinit deletes it).
static void teardown_openthread(void)
{
    if (s_core.loop_running) {
        xSemaphoreTake(s_core.loop_done, 0);
        s_core.push_pending = true;
        esp_openthread_mainloop_exit();
        if (xSemaphoreTake(s_core.loop_done, pdMS_TO_TICKS(LOOP_STOP_WAIT_MS)) != pdTRUE) {
            // Deinitialising under a running main loop is undefined behaviour: fault and reboot instead.
            s_core.push_pending = false;
            enter_fault("openthread did not stop");
            ESP_LOGE(TAG, "OpenThread main loop did not stop: rebooting");
            esp_restart();
        }
        s_core.push_pending = false;
    }
    if (s_core.border_router_up) {
        esp_openthread_lock_acquire(portMAX_DELAY);
        esp_err_t err = esp_openthread_border_router_deinit();
        esp_openthread_lock_release();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "border router deinit: %s", esp_err_to_name(err));
        }
        esp_openthread_platform_workflow_unregister(DNS_UPSTREAM_WORKFLOW);
        s_core.border_router_up = false;
    }
    if (s_core.ot_netif) {
        esp_netif_destroy(s_core.ot_netif);
        s_core.ot_netif = NULL;
        esp_openthread_netif_glue_deinit();
    }
    // esp_openthread_deinit expects the lock to be held (it finalises the instance, which releases the lock in
    // its UDP callbacks) and deletes the lock itself, so there is no release afterwards. After an initialisation that
    // was abandoned (an incompatible RCP parks the task inside esp_openthread_init) there is nothing safe to deinit.
    if (s_core.ot_initialised) {
        void (*cli_hook)(bool) = atomic_load(&s_ot_cli_hook);

        if (cli_hook) {
            cli_hook(false); // the CLI's task queue goes with esp_openthread_deinit
        }
        esp_openthread_lock_acquire(portMAX_DELAY);
        esp_openthread_deinit();
        s_core.ot_initialised = false;
    }
}

static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(REBOOT_DELAY_MS));
    esp_restart();
}

// The C6 job between stopping and restarting OpenThread: push the image, or (source NULL) ROM recovery of the base image.
// *base_only: the job leaves the C6 with its updater and no RCP (a ROM recovery, or c6_installer.bin through ESP-Hosted).
static esp_err_t run_c6_job(const rcp_fw_source_t *source, char *err_detail, size_t err_cap, bool *base_only)
{
    *base_only = source == NULL;
    if (source) {
        return rcp_sync_push_firmware(source, err_detail, err_cap, base_only);
    }
    // A failed manual recovery still restarts OpenThread, whose boot probe may then run the one automatic recovery of
    // this boot (the probe sees the same trigger): a second attempt after a manual one is intended, not a loop.
    const esp_err_t err = rcp_sync_rom_recover();
    // Refusals (no jumper, busy, no image) are no attempt; anything else is, and decides the status flag.
    const bool attempted = err != ESP_ERR_NOT_SUPPORTED && err != ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_core.mutex, portMAX_DELAY);
    s_core.rom_recovery_failed = mode_fsm_next_recovery_failed(s_core.rom_recovery_failed, attempted, err == ESP_OK);
    xSemaphoreGive(s_core.mutex);

    if (err != ESP_OK && err_detail && err_cap > 0) {
        strlcpy(err_detail, err == ESP_ERR_NOT_SUPPORTED ? "no maintenance jumper" : "ROM recovery of the C6 base image failed", err_cap);
    }
    return err;
}

// A C6 job inside radio_mux_c6_job_begin/_end, then the C6's boot steps (the status reads the result).
// `affects_mode`: OpenThread runs on the C6 (c6_boot_steps). ESP_ERR_INVALID_STATE (c6_busy: rcp-proxy on the C6 has a
// peer) or ESP_ERR_TIMEOUT (the mux did not answer) when nothing ran (*ran false). *steps: what c6_boot_steps returned.
static esp_err_t c6_job_window(const rcp_fw_source_t *source, char *err_detail, size_t err_cap, bool affects_mode,
                               bool *base_only, esp_err_t *steps, bool *ran)
{
    const esp_err_t begin = radio_mux_c6_job_begin();

    *base_only = false;
    *steps = ESP_FAIL;
    *ran = begin == ESP_OK;
    if (begin != ESP_OK) {
        if (err_detail && err_cap > 0) {
            strlcpy(err_detail, begin == ESP_ERR_INVALID_STATE ? "c6_busy: rcp-proxy on the C6 has a peer" :
                    "the radio mux did not answer", err_cap);
        }
        return begin == ESP_ERR_INVALID_STATE ? ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT;
    }
    const esp_err_t err = run_c6_job(source, err_detail, err_cap, base_only);

    *steps = c6_boot_steps(affects_mode);
    radio_mux_c6_job_end(*steps != ESP_ERR_INVALID_STATE); // the test build's c6_hold keeps it unavailable
    return err;
}

// OpenThread on the C6 again after a job: the mux attached it at the job's end when it answers. Probe and start, as
// br_radio does for any attach (the radio job keeps br_radio out meanwhile).
static esp_err_t restart_on_c6(void)
{
    char err[64];
    radio_mux_chan_t *ch = atomic_load(&s_attached);

    if (!ch || strcmp(radio_mux_chan_id(ch), RADIO_MUX_C6_ID) != 0) {
        return ESP_ERR_NOT_FOUND; // unavailable after the job: the boot steps' fault says why
    }
    s_tried_gen = atomic_load(&s_attach_gen);
    if (op_probe_channel(RADIO_MUX_C6_ID, err, sizeof(err)) != ESP_OK) {
        set_reason(err);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return op_ot_start(RADIO_MUX_C6_ID);
}

// A C6 job while OpenThread runs on another channel (or none): OpenThread and the board mode are left alone.
static esp_err_t run_c6_job_alone(const rcp_fw_source_t *source, char *err_detail, size_t err_cap)
{
    bool base_only, ran;
    esp_err_t steps;

    return c6_job_window(source, err_detail, err_cap, false, &base_only, &steps, &ran);
}

// Stop OpenThread, run the C6 job (push the image, or ROM recovery when `source` is NULL) inside a C6 job of the mux,
// start OpenThread on the C6 again (its boot steps, the mux's attach, the probe through the channel, the init). Needs a
// task that is not the OpenThread task (it cannot stop its own main loop). The caller (run_radio_job) has cleared
// ot_ready and the Thread REST flag. Blocks until the border router is up again or the restart failed.
static esp_err_t op_radio_job(const rcp_fw_source_t *source, char *err_detail, size_t err_cap, bool *restarted)
{
    const char *what = source ? "RCP firmware push" : "ROM recovery";
    bool base_only = false, ran;
    esp_err_t steps;

    *restarted = false;
    if (xTaskGetCurrentTaskHandle() == s_core.ot_task) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (s_core.init_abandoned) {
        // OpenThread's start was abandoned half-way: its state cannot be torn down, so it cannot be restarted in place.
        // A successful job is followed by a reboot, which starts OpenThread on a clean system. The fault state and
        // reason stay until then.
        ESP_LOGW(TAG, "%s without stopping OpenThread (abandoned start), reboot follows", what);
        const esp_err_t abandoned_err = c6_job_window(source, err_detail, err_cap, false, &base_only, &steps, &ran);

        if (abandoned_err == ESP_OK) {
            xTaskCreatePinnedToCore(reboot_task, "br_reboot", 2048, NULL, BRINGUP_TASK_PRIORITY, NULL, MGMT_CORE);
        } else {
            ESP_LOGE(TAG, "%s failed: %s", what, esp_err_to_name(abandoned_err));
        }
        return abandoned_err;
    }

    ESP_LOGI(TAG, "%s: stopping OpenThread", what);
    stop_openthread();
    xSemaphoreTake(s_core.mutex, portMAX_DELAY);
    s_core.fault_reason[0] = '\0';
    xSemaphoreGive(s_core.mutex);

    const esp_err_t push_err = c6_job_window(source, err_detail, err_cap, true, &base_only, &steps, &ran);
    if (push_err != ESP_OK) {
        ESP_LOGE(TAG, "%s failed: %s", what, esp_err_to_name(push_err));
    }
    // base_only: the C6 has its updater and no RCP, so the restart below ends in RADIO_FAULT "no rcp firmware" by design.
    // A job that did not run (c6_busy, the mux silent) left the C6 attached: OpenThread starts on it again.
    esp_err_t err = !ran || steps == ESP_OK ? restart_on_c6() : ESP_ERR_NOT_FOUND;

    if (err != ESP_OK) {
        if (!base_only) {
            ESP_LOGE(TAG, "restarting OpenThread after the %s failed: %s", what, esp_err_to_name(err));
        } else { // the normal outcome on a C6 without RCP firmware
            ESP_LOGW(TAG, "restarting OpenThread after the %s failed: %s (the status shows why)", what, esp_err_to_name(err));
        }
        if (s_core.fault_reason[0] == '\0') {
            enter_fault("radio restart failed");
        }
        const esp_err_t result = mode_fsm_radio_job_result(base_only, push_err, err);
        if (result == RCP_SYNC_ERR_RESTART && err_detail && err_cap) {
            xSemaphoreTake(s_core.mutex, portMAX_DELAY);
            snprintf(err_detail, err_cap, "the image is installed, but OpenThread did not start on it: %s",
                     s_core.fault_reason);
            xSemaphoreGive(s_core.mutex);
        }
        return result;
    }
    *restarted = true;
    return push_err;
}

static esp_err_t op_push_firmware(const rcp_fw_source_t *source, char *err_detail, size_t err_cap, bool *restarted)
{
    return op_radio_job(source, err_detail, err_cap, restarted);
}

static esp_err_t op_rom_recover(char *err_detail, size_t err_cap, bool *restarted)
{
    return op_radio_job(NULL, err_detail, err_cap, restarted);
}

static void op_reboot(void)
{
    esp_restart();
}

static int64_t op_now_us(void)
{
    return esp_timer_get_time();
}

static const mode_fsm_ops_t s_ops = {
    .thread_stop = op_thread_stop,
    .netif_down = op_netif_down,
    .raw_enable = op_raw_enable,
    .raw_disable = op_raw_disable,
    .raw_abort = op_raw_abort,
    .quiet_radio = op_quiet_radio,
    .defer = op_defer,
    .restore_channel = op_restore_channel,
    .netif_up = op_netif_up,
    .thread_start = op_thread_start,
    .reset_rcp = op_reset_rcp,
    .push_firmware = op_push_firmware,
    .rom_recover = op_rom_recover,
    .reboot = op_reboot,
    .now_us = op_now_us,
    .probe_channel = op_probe_channel,
    .ot_start = op_ot_start,
    .ot_stop = op_ot_stop,
    .store_otbr = op_store_otbr,
};

// True while OpenThread takes calls. Read under the status mutex; callers that hold the OpenThread lock use it to
// re-check after taking it: a push clears the flag under that lock before it tears OpenThread down.
static bool ot_ready_now(void)
{
    if (!s_core.started) {
        return false;
    }
    xSemaphoreTake(s_core.mutex, portMAX_DELAY);
    const bool ready = s_core.ot_ready;
    xSemaphoreGive(s_core.mutex);
    return ready;
}

bool br_core_ot_session_begin(void)
{
    if (!s_core.started) {
        return false;
    }
    xSemaphoreTake(s_core.mutex, portMAX_DELAY);
    const bool ready = s_core.ot_ready;
    if (ready) {
        s_core.ot_sessions++;
    }
    xSemaphoreGive(s_core.mutex);
    return ready;
}

void br_core_ot_session_end(void)
{
    xSemaphoreTake(s_core.mutex, portMAX_DELAY);
    assert(s_core.ot_sessions > 0);
    if (--s_core.ot_sessions == 0) {
        xSemaphoreGive(s_core.sessions_done);
    }
    xSemaphoreGive(s_core.mutex);
}

// After ot_ready was cleared (no new session can open): wait until the open ones are closed. False after
// SESSION_DRAIN_MS with sessions still open.
static bool wait_sessions_closed(void)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(SESSION_DRAIN_MS);

    while (true) {
        xSemaphoreTake(s_core.mutex, portMAX_DELAY);
        const uint32_t open = s_core.ot_sessions;
        xSemaphoreGive(s_core.mutex);
        if (open == 0) {
            return true;
        }
        const TickType_t left = deadline - xTaskGetTickCount();
        if ((int32_t)left <= 0) {
            ESP_LOGW(TAG, "%" PRIu32 " OpenThread session(s) still open", open);
            return false;
        }
        xSemaphoreTake(s_core.sessions_done, left); // a stale give only makes the loop look again
    }
}

// ---- OpenThread on a radio mux channel: the otbr consumer, the probe, start, stop, moves (spec section 3) ----

static void radio_notify(void)
{
    TaskHandle_t task = atomic_load(&s_radio_task);

    if (task) {
        xTaskNotifyGive(task);
    }
}

// The mux operations of MuxSpinelInterface (esp_openthread_mux.h), on the OpenThread task under the OpenThread lock.
static esp_err_t mux_tx(void *ctx, const uint8_t *f, size_t n)
{
    radio_mux_chan_t *ch = atomic_load(&((otbr_link_t *)ctx)->ch);

    return ch ? radio_mux_tx(ch, f, n) : ESP_ERR_INVALID_STATE;
}

static bool mux_poll(void *ctx, uint8_t **slot, size_t *len)
{
    otbr_link_t *link = ctx;
    radio_mux_chan_t *ch = atomic_load(&link->ch);
    radio_mux_ev_t ev;

    while (ch && radio_mux_poll(ch, &ev)) {
        switch (ev.type) {
        case RADIO_MUX_EV_RX_FRAME:
            *slot = ev.slot;
            *len = ev.len;
            return true;
        case RADIO_MUX_EV_RESET:
            *slot = NULL;
            *len = 0;
            return true;
        case RADIO_MUX_EV_GONE:
            atomic_store(&link->gone, true);
            radio_notify();
            break;
        }
    }
    return false;
}

static void mux_release(void *ctx, uint8_t *slot)
{
    radio_mux_release(atomic_load(&((otbr_link_t *)ctx)->ch), slot);
}

static int mux_eventfd(void *ctx)
{
    radio_mux_chan_t *ch = atomic_load(&((otbr_link_t *)ctx)->ch);

    return ch ? radio_mux_eventfd(ch) : -1;
}

static esp_err_t mux_reset(void *ctx)
{
    radio_mux_chan_t *ch = atomic_load(&((otbr_link_t *)ctx)->ch);

    return ch ? radio_mux_reset(ch) : ESP_ERR_INVALID_STATE;
}

static bool mux_gone(void *ctx)
{
    const otbr_link_t *link = ctx;

    return atomic_load(&link->gone) || !atomic_load(&link->ch);
}

static const esp_openthread_mux_ops_t s_mux_ops = {
    .tx = mux_tx,
    .poll = mux_poll,
    .release = mux_release,
    .eventfd = mux_eventfd,
    .reset = mux_reset,
    .gone = mux_gone,
};

// The otbr consumer (radio mux owner task): note the channel and wake br_radio, which starts and stops OpenThread.
static esp_err_t otbr_attach(radio_mux_chan_t *ch, void *ctx)
{
    atomic_store(&s_attached, ch);
    atomic_fetch_add(&s_attach_gen, 1);
    ESP_LOGI(TAG, "%s: otbr attached", radio_mux_chan_id(ch));
    radio_notify();
    return ESP_OK;
}

static void otbr_detach(radio_mux_chan_t *ch, void *ctx)
{
    radio_mux_chan_t *expected = ch;

    atomic_compare_exchange_strong(&s_attached, &expected, NULL);
    if (atomic_load(&s_link.ch) == ch) {
        atomic_store(&s_link.gone, true); // the interface refuses requests from now on (no recovery, no DieNow)
    }
    ESP_LOGI(TAG, "%s: otbr detached", radio_mux_chan_id(ch));
    radio_notify();
}

static void wait_readable(int fd, int64_t us)
{
    if (fd < 0) {
        vTaskDelay(pdMS_TO_TICKS(PROBE_POLL_SLICE_MS));
        return;
    }
    fd_set read_fds;
    struct timeval timeout = {.tv_sec = (time_t)(us / 1000000), .tv_usec = (suseconds_t)(us % 1000000)};

    FD_ZERO(&read_fds);
    FD_SET(fd, &read_fds);
    select(fd + 1, &read_fds, NULL, NULL, &timeout);
}

// Waits up to `ms` for the RCP's reset notification (`tid` 0, `key` ignored) or the answer to request `tid` about `key`;
// other frames are given back. The answer's value goes to `value` (cut to *len, *len set). ESP_ERR_INVALID_STATE on
// GONE, ESP_ERR_TIMEOUT.
static esp_err_t probe_wait(radio_mux_chan_t *ch, uint8_t tid, uint32_t key, uint8_t *value, size_t *len, uint32_t ms)
{
    const int64_t deadline = esp_timer_get_time() + (int64_t)ms * 1000;
    radio_mux_ev_t ev;

    for (;;) {
        while (radio_mux_poll(ch, &ev)) {
            if (ev.type == RADIO_MUX_EV_GONE) {
                return ESP_ERR_INVALID_STATE;
            }
            if (ev.type != RADIO_MUX_EV_RX_FRAME) {
                continue;
            }
            otbr_spinel_msg_t m;
            const bool hit = otbr_probe_parse(ev.slot, ev.len, &m) &&
                             (tid == 0 ? otbr_probe_is_reset(&m) : otbr_probe_is_answer(&m, tid, key));

            if (hit && value) {
                *len = m.value_len < *len ? m.value_len : *len;
                memcpy(value, m.value, *len);
            }
            radio_mux_release(ch, ev.slot);
            if (hit) {
                return ESP_OK;
            }
        }
        const int64_t left = deadline - esp_timer_get_time();

        if (left <= 0) {
            return ESP_ERR_TIMEOUT;
        }
        wait_readable(radio_mux_eventfd(ch), left < PROBE_POLL_SLICE_MS * 1000 ? left : PROBE_POLL_SLICE_MS * 1000);
    }
}

// One property: GET with `tid`, its value into `value` (*len in: room, out: length).
static esp_err_t probe_get(radio_mux_chan_t *ch, uint8_t tid, uint32_t key, uint8_t *value, size_t *len)
{
    uint8_t frame[OTBR_PROBE_FRAME_MAX];
    const esp_err_t err = radio_mux_tx(ch, frame, otbr_probe_get_frame(tid, key, frame));

    return err == ESP_OK ? probe_wait(ch, tid, key, value, len, PROBE_ANSWER_WAIT_MS) : err;
}

// The probe before OpenThread starts on a channel (spec: Start): spinel reset, PROP_NCP_VERSION, the capabilities (an
// RCP, and the radio caps OpenThread requires). An incompatible or wrong-baud radio fails here, with the reason in
// `err`, never inside esp_openthread_init.
static esp_err_t probe_rcp(radio_mux_chan_t *ch, radio_mux_fw_record_t *fw, char *err, size_t cap)
{
    uint8_t frame[OTBR_PROBE_FRAME_MAX];
    uint8_t value[RADIO_MUX_DETECT_RX_MAX];
    size_t len;
    uint32_t radio_caps = 0;
    radio_mux_ev_t ev;
    esp_err_t ret;

    while (radio_mux_poll(ch, &ev)) { // what came before the probe
        if (ev.type == RADIO_MUX_EV_GONE) {
            snprintf(err, cap, "the radio channel went away");
            return ESP_ERR_INVALID_STATE;
        }
        if (ev.type == RADIO_MUX_EV_RX_FRAME) {
            radio_mux_release(ch, ev.slot);
        }
    }
    // A C6 that is still booting may not take the reset; it announces its own start then, which counts as well.
    ret = radio_mux_tx(ch, frame, otbr_probe_reset_frame(frame));
    if (ret != ESP_ERR_INVALID_STATE) {
        ret = probe_wait(ch, 0, 0, NULL, NULL, PROBE_RESET_WAIT_MS);
    }
    if (ret != ESP_OK) {
        snprintf(err, cap, "%s", ret == ESP_ERR_INVALID_STATE ? "the radio channel went away" :
                 "no spinel answer to a reset (no RCP firmware, or a wrong baud)");
        return ret;
    }
    len = sizeof(value) - 1;
    ret = probe_get(ch, 1, OTBR_SPINEL_PROP_NCP_VERSION, value, &len);
    if (ret != ESP_OK) {
        snprintf(err, cap, "no answer to PROP_NCP_VERSION");
        return ret;
    }
    value[len] = '\0';
    memset(fw, 0, sizeof(*fw));
    fw->type = RADIO_MUX_FW_SPINEL;
    radio_mux_detect_parse_spinel_version((const char *)value, fw->version, sizeof(fw->version), fw->chip,
                                          sizeof(fw->chip));
    len = sizeof(value);
    ret = probe_get(ch, 2, OTBR_SPINEL_PROP_CAPS, value, &len);
    if (ret != ESP_OK || !otbr_probe_has_cap(value, len, OTBR_SPINEL_CAP_CONFIG_RADIO)) {
        snprintf(err, cap, "%s", ret != ESP_OK ? "no answer to PROP_CAPS" : "not an RCP (no radio configuration cap)");
        return ret != ESP_OK ? ret : ESP_ERR_NOT_SUPPORTED;
    }
    len = sizeof(value);
    ret = probe_get(ch, 3, OTBR_SPINEL_PROP_RADIO_CAPS, value, &len);
    if (ret != ESP_OK || !otbr_probe_radio_caps(value, len, &radio_caps)) {
        snprintf(err, cap, "no answer to PROP_RADIO_CAPS");
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_RESPONSE;
    }
    if (otbr_probe_missing_caps(radio_caps, err, cap) != 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_LOGI(TAG, "%s: RCP %s (%s), radio caps 0x%" PRIx32, radio_mux_chan_id(ch), fw->version, fw->chip, radio_caps);
    return ESP_OK;
}

// The otbr channel `id` as br_radio holds it: the move's lease, or the attached channel.
static radio_mux_chan_t *channel_for(const char *id)
{
    radio_mux_chan_t *ch = atomic_load(&s_attached);

    if (s_lease && strcmp(radio_mux_chan_id(s_lease), id) == 0) {
        return s_lease;
    }
    return ch && strcmp(radio_mux_chan_id(ch), id) == 0 ? ch : NULL;
}

// The probe's verdict to the PUT that waits for it (br_core_move_otbr). false: that PUT gave up, the move stops.
static bool move_signal(esp_err_t result, const char *reason)
{
    int waiting = MOVE_WAITING;

    s_move.result = result;
    snprintf(s_move.reason, sizeof(s_move.reason), "%s", reason ? reason : "");
    if (atomic_compare_exchange_strong(&s_move.state, &waiting, MOVE_SIGNALLED)) {
        xSemaphoreGive(s_move.probed);
        return true;
    }
    return waiting != MOVE_ABANDONED;
}

static esp_err_t op_probe_channel(const char *id, char *err, size_t cap)
{
    radio_mux_chan_t *ch = channel_for(id);
    radio_mux_fw_record_t fw;
    esp_err_t ret = ESP_OK;

    if (!ch) { // not the attached channel: a move's target, taken from the mux until the move ends
        ret = radio_mux_lease(id, &s_lease, err, cap);
        ch = ret == ESP_OK ? s_lease : NULL;
    }
    if (ret == ESP_OK) {
        ret = probe_rcp(ch, &fw, err, cap);
    }
    if (ret == ESP_OK) {
        s_probe_fw = fw;
        if (ch != s_lease) {
            radio_mux_note_firmware(ch, &fw); // a lease's record is noted once the move stored it
        }
    } else {
        strlcpy(s_probe_err, err, sizeof(s_probe_err));
    }
    if (s_move_probe) {
        s_move_probe = false;
        if (!move_signal(ret == ESP_OK ? ESP_OK : ESP_ERR_INVALID_RESPONSE, err) && ret == ESP_OK) {
            snprintf(err, cap, "the request gave up waiting for the probe");
            return ESP_ERR_TIMEOUT;
        }
    }
    return ret;
}

// Stops OpenThread (the sessions are drained or nothing may wait any more) and forgets its channel.
static void stop_openthread(void)
{
    esp_br_web_set_openthread_ready(false);
    xSemaphoreTake(s_core.mutex, portMAX_DELAY);
    s_core.ot_ready = false;
    s_core.bringup_started = false;
    xSemaphoreGive(s_core.mutex);
    teardown_openthread();
    s_ot_chan = NULL;
    atomic_store(&s_link.ch, NULL);
}

static esp_err_t op_ot_start(const char *id)
{
    radio_mux_chan_t *ch = channel_for(id);

    if (!ch) {
        return ESP_ERR_NOT_FOUND;
    }
    if (s_core.init_abandoned) { // an earlier start parked inside esp_openthread_init: nothing of it can be released
        set_reason("an earlier OpenThread start was abandoned: reboot");
        return ESP_ERR_INVALID_STATE;
    }
    if (!mode_fsm_radio_start_allowed(s_storage_degraded)) {
        ESP_LOGE(TAG, "storage is unavailable (NVS): OpenThread is not started");
        set_reason(MODE_FSM_FAULT_STORAGE);
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store(&s_link.ch, ch);
    atomic_store(&s_link.gone, false);
    s_ot_chan = ch;
    s_ot_gen = atomic_load(&s_attach_gen);
    xSemaphoreTake(s_core.mutex, portMAX_DELAY);
    s_core.ot_ready = false;
    s_core.bringup_started = false;
    s_core.fault_reason[0] = '\0';
    xSemaphoreGive(s_core.mutex);
    xSemaphoreTake(s_core.boot_done, 0);
    xSemaphoreTake(s_core.bringup_done, 0);

    esp_err_t err = start_ot_task();

    if (err == ESP_OK && xSemaphoreTake(s_core.boot_done, pdMS_TO_TICKS(BOOT_WAIT_MS)) != pdTRUE) {
        s_core.init_abandoned = true; // still inside esp_openthread_init: never torn down under it
        return ESP_ERR_TIMEOUT;
    }
    if (err == ESP_OK && !ot_ready_now()) {
        err = ESP_FAIL; // the fault mode was entered during the init
    }
    if (err == ESP_OK && backbone_has_ip6()) { // without an address the border router starts once one comes
        err = xSemaphoreTake(s_core.bringup_done, pdMS_TO_TICKS(BRINGUP_WAIT_MS)) == pdTRUE && s_core.border_router_up
              ? ESP_OK : ESP_FAIL;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: OpenThread did not start: %s", id, esp_err_to_name(err));
        stop_openthread();
    }
    return err;
}

static esp_err_t op_ot_stop(void)
{
    if (!s_ot_chan && !s_core.ot_initialised) {
        atomic_store(&s_link.ch, NULL);
        return ESP_OK; // nothing runs
    }
    if (ot_ready_now()) {
        // As a push: no new session, Thread REST off, then wait for the open sessions (they end early once it is off).
        esp_openthread_lock_acquire(portMAX_DELAY);
        esp_br_web_set_openthread_ready(false);
        xSemaphoreTake(s_core.mutex, portMAX_DELAY);
        s_core.ot_ready = false;
        xSemaphoreGive(s_core.mutex);
        esp_openthread_lock_release();
        if (!wait_sessions_closed()) {
            xSemaphoreTake(s_core.mutex, portMAX_DELAY);
            s_core.ot_ready = true;
            xSemaphoreGive(s_core.mutex);
            return ESP_ERR_TIMEOUT; // nothing stopped; the caller puts the REST flag back
        }
    }
    ESP_LOGI(TAG, "stopping OpenThread on %s", s_ot_chan ? radio_mux_chan_id(s_ot_chan) : "its channel");
    stop_openthread();
    return ESP_OK;
}

static esp_err_t op_store_otbr(const char *from, const char *to)
{
    const esp_err_t err = radio_mux_commit_otbr(from, to);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "storing otbr on %s (off on %s): %s", to, from, esp_err_to_name(err));
        return err;
    }
    s_lease = NULL; // now the otbr consumer's attach
    s_ot_gen = atomic_load(&s_attach_gen);
    s_tried_gen = s_ot_gen;
    radio_mux_chan_t *ch = atomic_load(&s_attached);

    if (ch) {
        radio_mux_note_firmware(ch, &s_probe_fw);
    }
    return ESP_OK;
}

// After a mode_fsm channel operation: the reason and the Thread REST flag follow the mode it ended in, and
// br_core_start's wait for the radio ends.
static void after_channel_op(void)
{
    const br_core_mode_t mode = br_core_get_mode();

    xSemaphoreTake(s_core.mutex, portMAX_DELAY);
    if (mode == BR_MODE_RADIO_FAULT && s_core.fault_reason[0] == '\0') {
        strlcpy(s_core.fault_reason, s_probe_err[0] ? s_probe_err : FAULT_NO_CHANNEL_START,
                sizeof(s_core.fault_reason));
    } else if (mode != BR_MODE_RADIO_FAULT) {
        s_core.fault_reason[0] = '\0';
    }
    const bool ready = s_core.ot_ready;
    xSemaphoreGive(s_core.mutex);
    esp_br_web_set_openthread_ready(ready && mode != BR_MODE_RADIO_FAULT);
    xSemaphoreGive(s_core.boot_done);
}

// OpenThread follows the otbr channel: torn down when it went (unplug, mode change, a C6 job outside a push), started
// when one is attached (boot, plug-in, the end of a C6 job, a mode change); a failed start waits for the next attach.
static void reconcile(void)
{
    if (atomic_load(&s_core.push_busy)) {
        return; // the job stops and starts OpenThread itself
    }
    radio_mux_chan_t *att = atomic_load(&s_attached);
    const unsigned gen = atomic_load(&s_attach_gen);

    if ((s_ot_chan && (s_ot_chan != att || s_ot_gen != gen || atomic_load(&s_link.gone))) ||
            (!s_ot_chan && !att && s_start_failed)) {
        ESP_LOGW(TAG, "the OTBR channel is gone: OpenThread stops, NO_RADIO");
        s_start_failed = false;
        mode_fsm_on_channel_gone(&s_fsm);
        after_channel_op();
    }
    if (!s_ot_chan && att && gen != s_tried_gen) {
        s_tried_gen = gen;
        s_probe_err[0] = '\0';
        ESP_LOGI(TAG, "%s: starting OpenThread", radio_mux_chan_id(att));
        mode_fsm_on_channel_back(&s_fsm, radio_mux_chan_id(att));
        s_start_failed = br_core_get_mode() == BR_MODE_RADIO_FAULT;
        after_channel_op();
    }
}

static void run_move(void)
{
    char err[RADIO_MUX_REASON_MAX] = "";

    ESP_LOGI(TAG, "moving OpenThread from %s to %s", s_move.from[0] ? s_move.from : "(none)", s_move.to);
    s_move_probe = true;
    const esp_err_t ret = mode_fsm_move_otbr(&s_fsm, s_move.from, s_move.to, err, sizeof(err));

    if (s_move_probe) { // refused before the probe
        s_move_probe = false;
        move_signal(ret, err);
    }
    if (s_lease) {
        const bool nothing_shown = ret == MODE_FSM_ERR_PROBE_FAILED || ret == ESP_ERR_INVALID_STATE;

        if (nothing_shown || ret == MODE_FSM_ERR_DRAIN_TIMEOUT || ret == MODE_FSM_ERR_START_FAILED) {
            radio_mux_lease_end(s_lease, nothing_shown ? NULL : err);
            s_lease = NULL;
        } else { // OpenThread runs on the target, but the modes were not stored: it stays leased until a reboot
            ESP_LOGE(TAG, "%s: %s", s_move.to, err);
        }
    }
    if (ret == ESP_OK) {
        s_start_failed = false;
        ESP_LOGI(TAG, "OpenThread moved to %s", s_move.to);
    } else {
        ESP_LOGW(TAG, "moving OpenThread to %s: %s (%s)", s_move.to, esp_err_to_name(ret), err);
    }
    after_channel_op();
    atomic_store(&s_move.running, false);
}

static void radio_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (atomic_exchange(&s_move.queued, false)) {
            run_move();
        }
        reconcile();
    }
}

static esp_err_t start_radio_task(void)
{
    TaskHandle_t task;

    if (atomic_load(&s_radio_task)) {
        return ESP_OK;
    }
    if (xTaskCreatePinnedToCore(radio_task, RADIO_TASK_NAME, RADIO_TASK_STACK, NULL, RADIO_TASK_PRIORITY, &task,
                                MGMT_CORE) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    atomic_store(&s_radio_task, task);
    return ESP_OK;
}

// The board's radio at boot (spec: Board mode): no channel stored in otbr, or a dongle, is NO_RADIO until it attaches;
// the C6 runs its boot steps inside a C6 job (R29: rcp_sync drives SPI3 and the INT pin) and is attached again at its
// end, which starts OpenThread on br_radio.
static void boot_otbr(void)
{
    char id[RADIO_MUX_ID_MAX] = "";
    const char *stored = radio_mux_otbr_channel();

    if (stored) {
        strlcpy(id, stored, sizeof(id));
    }
    if (!mode_fsm_boot(&s_fsm, stored ? id : NULL)) {
        if (start_radio_task() != ESP_OK) {
            enter_fault("cannot create the radio task");
            return;
        }
        ESP_LOGI(TAG, "%s: NO_RADIO until it is present", stored ? id : "no channel in otbr");
        xSemaphoreGive(s_core.boot_done);
        radio_notify(); // a dongle attached before the start
        return;
    }
    const esp_err_t job = radio_mux_c6_job_begin();

    if (job != ESP_OK) {
        enter_fault(job == ESP_ERR_TIMEOUT ? "the radio mux did not answer" : "the C6 is busy");
        return;
    }
    if (start_radio_task() != ESP_OK) {
        radio_mux_c6_job_end(true);
        enter_fault("cannot create the radio task");
        return;
    }
    const esp_err_t steps = c6_boot_steps(true);

    // A C6 held in reset by the test build stays unavailable (nothing attaches and drives its reset GPIO high again).
    radio_mux_c6_job_end(steps != ESP_ERR_INVALID_STATE);
    radio_notify();
}

esp_err_t br_core_register_otbr(void)
{
    static StaticSemaphore_t probed_buf;
    const radio_mux_consumer_t consumer = {
        .mode = RADIO_MUX_MODE_OTBR,
        .needs = RADIO_MUX_FW_SPINEL,
        .limit = 1,
        .reboot_on_change = false,
        .frames = true,
        .attach = otbr_attach,
        .detach = otbr_detach,
    };

    if (!s_move.probed) {
        s_move.probed = xSemaphoreCreateBinaryStatic(&probed_buf);
    }
    ESP_RETURN_ON_ERROR(radio_mux_register_consumer(&consumer), TAG, "the otbr consumer");
    radio_mux_set_otbr_mover(br_core_move_otbr);
    return ESP_OK;
}

esp_err_t br_core_move_otbr(const char *from, const char *to, char *reason, size_t cap)
{
    bool idle = false;

    if (reason && cap > 0) {
        reason[0] = '\0';
    }
    if (!s_core.started || !atomic_load(&s_radio_task) || !to) {
        snprintf(reason, cap, "OpenThread is not started");
        return ESP_ERR_INVALID_STATE;
    }
    if (!atomic_compare_exchange_strong(&s_move.running, &idle, true)) {
        snprintf(reason, cap, "OpenThread is already being moved");
        return ESP_ERR_INVALID_STATE;
    }
    if (atomic_load(&s_core.push_busy)) { // a push checks s_move.running after it set push_busy: one of them refuses
        atomic_store(&s_move.running, false);
        snprintf(reason, cap, "a C6 firmware push or recovery runs");
        return ESP_ERR_INVALID_STATE;
    }
    strlcpy(s_move.from, from ? from : "", sizeof(s_move.from));
    strlcpy(s_move.to, to, sizeof(s_move.to));
    s_move.result = ESP_FAIL;
    s_move.reason[0] = '\0';
    xSemaphoreTake(s_move.probed, 0);
    atomic_store(&s_move.state, MOVE_WAITING);
    atomic_store(&s_move.queued, true);
    radio_notify();
    if (xSemaphoreTake(s_move.probed, pdMS_TO_TICKS(MOVE_PROBE_WAIT_MS)) != pdTRUE) {
        int waiting = MOVE_WAITING;

        if (atomic_compare_exchange_strong(&s_move.state, &waiting, MOVE_ABANDONED)) {
            snprintf(reason, cap, "the probe of %s did not finish in time", to);
            return ESP_ERR_TIMEOUT; // br_radio stops the move after its probe
        }
        xSemaphoreTake(s_move.probed, portMAX_DELAY); // signalled just now
    }
    snprintf(reason, cap, "%s", s_move.reason);
    return s_move.result;
}

bool br_core_move_running(void)
{
    return atomic_load(&s_move.running);
}

esp_err_t br_core_enter_sniffer(uint8_t channel, otLinkRawReceiveDone receive_cb)
{
    ESP_RETURN_ON_FALSE(receive_cb != NULL, ESP_ERR_INVALID_ARG, TAG, "no receive callback");
    // The session keeps a push from deleting the lock while this task waits for it.
    ESP_RETURN_ON_FALSE(br_core_ot_session_begin(), ESP_ERR_INVALID_STATE, TAG, "OpenThread is not running");
    esp_openthread_lock_acquire(portMAX_DELAY);
    if (!ot_ready_now()) { // a push is about to take OpenThread over: it cleared the flag while this task waited
        esp_openthread_lock_release();
        br_core_ot_session_end();
        return ESP_ERR_INVALID_STATE;
    }
    if (br_core_get_mode() == BR_MODE_BR_RUNNING) {
        esp_br_web_set_openthread_ready(false); // Thread REST off before Thread stops
    }
    esp_err_t err = mode_fsm_enter_sniffer(&s_fsm, channel, receive_cb);
    if (err != ESP_OK && br_core_get_mode() == BR_MODE_BR_RUNNING) {
        esp_br_web_set_openthread_ready(true);
    }
    esp_openthread_lock_release();
    br_core_ot_session_end();
    return err;
}

void br_core_set_sniffer_end_cb(void (*cb)(void))
{
    mode_fsm_set_sniffer_end_cb(&s_fsm, cb);
}

void br_core_set_border_router_up_cb(void (*cb)(otInstance *instance))
{
    s_border_router_up_cb = cb;
}

esp_err_t br_core_leave_sniffer(void)
{
    ESP_RETURN_ON_FALSE(br_core_ot_session_begin(), ESP_ERR_INVALID_STATE, TAG, "OpenThread is not running");
    esp_openthread_lock_acquire(portMAX_DELAY);
    if (!ot_ready_now()) {
        esp_openthread_lock_release();
        br_core_ot_session_end();
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = mode_fsm_leave_sniffer(&s_fsm);
    if (err == ESP_OK) {
        esp_br_web_set_openthread_ready(true);
    }
    esp_openthread_lock_release();
    br_core_ot_session_end();
    return err;
}

// On the OpenThread task: the failure path (hook, fault_and_park) assumes it runs there.
static void keepalive_task(void *arg)
{
    if (br_core_get_mode() == BR_MODE_SNIFFER) {
        otPlatRadioGetRssi(esp_openthread_get_instance()); // a spinel round trip; a timeout enters the failure path
    }
    atomic_store(&s_keepalive_pending, false);
}

void br_core_sniffer_keepalive(void)
{
    bool expected = false;

    if (br_core_get_mode() != BR_MODE_SNIFFER || !atomic_compare_exchange_strong(&s_keepalive_pending, &expected, true)) {
        return;
    }
    if (esp_openthread_task_queue_post(keepalive_task, NULL) != ESP_OK) {
        atomic_store(&s_keepalive_pending, false);
    }
}

// One C6 job at a time (push_busy), OpenThread stopped around it: a firmware push (source set) or a ROM recovery
// (source NULL). Callers have validated their arguments.
static esp_err_t run_radio_job(const rcp_fw_source_t *source, char *err_detail, size_t err_cap)
{
    ESP_RETURN_ON_FALSE(s_core.started, ESP_ERR_INVALID_STATE, TAG, "not started");
    // Up front, before any state is touched: the OpenThread task cannot stop its own main loop.
    ESP_RETURN_ON_ERROR(mode_fsm_push_allowed(br_core_get_mode(), xTaskGetCurrentTaskHandle() == s_core.ot_task), TAG,
                        "radio job not allowed in this mode or task");
    // After a boot timeout ("radio boot timed out") the OpenThread task may still be initialising OpenThread.
    if (s_core.ot_task && !s_core.loop_running) {
        if (err_detail && err_cap > 0) {
            strlcpy(err_detail, "the radio is still booting", err_cap);
        }
        ESP_LOGW(TAG, "radio job refused: the radio is still booting");
        return ESP_ERR_INVALID_STATE;
    }
    bool expected = false;
    ESP_RETURN_ON_FALSE(atomic_compare_exchange_strong(&s_core.push_busy, &expected, true), ESP_ERR_INVALID_STATE, TAG,
                        "a firmware push or ROM recovery is already running");
    if (atomic_load(&s_move.running)) { // a move checks push_busy after it set running: one of them refuses
        atomic_store(&s_core.push_busy, false);
        if (err_detail && err_cap > 0) {
            strlcpy(err_detail, "OpenThread is being moved to another radio", err_cap);
        }
        return ESP_ERR_INVALID_STATE;
    }
    const char *otbr = radio_mux_otbr_channel();

    if (!otbr || strcmp(otbr, RADIO_MUX_C6_ID) != 0) {
        // OpenThread does not run on the C6 (a dongle has it, or no channel): the job leaves it and the mode alone.
        const esp_err_t alone = run_c6_job_alone(source, err_detail, err_cap);

        atomic_store(&s_core.push_busy, false);
        radio_notify();
        return alone;
    }

    // Begin the job under the OpenThread lock: the mode becomes STARTING (the REST guard and the Web GUI refuse Thread
    // access, an RCP failure only resets the C6, a border router start does not complete) and ot_ready goes false, so
    // that no new OpenThread session opens and the sniffer entry points (which re-check it under the same lock) cannot
    // start. The lock only exists while OpenThread is initialised.
    const bool had_openthread = ot_ready_now();
    if (had_openthread) {
        esp_openthread_lock_acquire(portMAX_DELAY);
    }
    esp_err_t err = mode_fsm_begin_radio_job(&s_fsm);
    if (err == ESP_OK) {
        esp_br_web_set_openthread_ready(false); // Thread REST off before Thread stops
        xSemaphoreTake(s_core.mutex, portMAX_DELAY);
        s_core.ot_ready = false;
        xSemaphoreGive(s_core.mutex);
    }
    if (had_openthread) {
        esp_openthread_lock_release();
    }
    // Tasks inside an OpenThread session may still wait for the lock or for an OpenThread callback: teardown deletes
    // the lock and the instance, so it waits for them. The main loop keeps running meanwhile, and the long waits of the
    // web server end early once the REST flag is off.
    if (err == ESP_OK && had_openthread && !wait_sessions_closed()) {
        err = ESP_ERR_INVALID_STATE; // busy: nothing was stopped
        if (err_detail && err_cap > 0) {
            strlcpy(err_detail, "OpenThread is busy, try again", err_cap);
        }
        esp_openthread_lock_acquire(portMAX_DELAY);
        mode_fsm_cancel_radio_job(&s_fsm);
        xSemaphoreTake(s_core.mutex, portMAX_DELAY);
        s_core.ot_ready = true;
        xSemaphoreGive(s_core.mutex);
        esp_br_web_set_openthread_ready(mode_fsm_rest_ready_after_push(br_core_get_mode(), true));
        esp_openthread_lock_release();
    }

    if (err == ESP_OK) {
        // No OpenThread lock: the operation deletes and re-creates it.
        err = source ? mode_fsm_push_firmware(&s_fsm, source, err_detail, err_cap)
              : mode_fsm_rom_recover(&s_fsm, err_detail, err_cap);
        // The flag follows the final state on every path, never the transient STARTING of the job.
        esp_br_web_set_openthread_ready(mode_fsm_rest_ready_after_push(br_core_get_mode(), ot_ready_now()));
    }
    atomic_store(&s_core.push_busy, false);
    radio_notify(); // what the channel did meanwhile (br_radio stayed out)
    return err;
}

bool br_core_radio_job_running(void)
{
    return atomic_load(&s_core.push_busy);
}

esp_err_t br_core_push_radio_firmware(const rcp_fw_source_t *source, char *err_detail, size_t err_cap)
{
    if (err_detail && err_cap > 0) {
        err_detail[0] = '\0';
    }
    ESP_RETURN_ON_FALSE(source && source->read, ESP_ERR_INVALID_ARG, TAG, "no firmware source");
    return run_radio_job(source, err_detail, err_cap);
}

esp_err_t br_core_rom_recover(char *err_detail, size_t err_cap)
{
    if (err_detail && err_cap > 0) {
        err_detail[0] = '\0';
    }
    // Before anything is stopped: without the jumper there is nothing to recover with.
    ESP_RETURN_ON_FALSE(rcp_sync_jumper_present(), ESP_ERR_NOT_SUPPORTED, TAG, "no maintenance jumper");
    return run_radio_job(NULL, err_detail, err_cap);
}

// ---- leader weight and take-over ----

static esp_err_t store_leader_adjustment(int8_t adj)
{
    nvs_handle_t handle;

    ESP_RETURN_ON_ERROR(nvs_open(LEADER_NVS_NAMESPACE, NVS_READWRITE, &handle), TAG, "nvs_open");
    esp_err_t err = nvs_set_i8(handle, LEADER_NVS_KEY, adj);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

// OpenThread takes calls (like br_core_get_status: every mode but RADIO_FAULT). Device properties live in the host
// instance, so the leader weight adjustment can be read and set in STARTING and SNIFFER too.
static bool ot_usable(void)
{
    return ot_ready_now() && br_core_get_mode() != BR_MODE_RADIO_FAULT;
}

static bool br_running(void)
{
    return ot_ready_now() && br_core_get_mode() == BR_MODE_BR_RUNNING;
}

// Releases the OpenThread lock taken by ot_lock_while and ends its session.
static void ot_unlock(void)
{
    esp_openthread_lock_release();
    br_core_ot_session_end();
}

// Opens an OpenThread session and takes the OpenThread lock within `wait_ms` while `usable()` holds, checked before and
// again after the wait (a push clears ot_ready under the lock before it tears OpenThread down). ESP_ERR_INVALID_STATE
// when it does not hold or no session opens, ESP_ERR_TIMEOUT when the lock was not free in time; nothing stays open on
// either. On ESP_OK the caller ends with ot_unlock.
static esp_err_t ot_lock_while(bool (*usable)(void), uint32_t wait_ms)
{
    if (!usable() || !br_core_ot_session_begin()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(wait_ms))) {
        br_core_ot_session_end();
        return ESP_ERR_TIMEOUT;
    }
    if (!usable()) {
        ot_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

// With the OpenThread lock held: make OpenThread use the stored adjustment if it does not yet (a save that could not
// take the lock, or one between bring-up's apply and a later read).
static void sync_leader_adjustment(otInstance *instance)
{
    const int8_t adj = (int8_t)atomic_load(&s_leader_adj);

    if (otThreadGetDeviceProperties(instance)->mLeaderWeightAdjustment != adj) {
        apply_leader_adjustment(instance, adj);
        ESP_LOGI(TAG, "leader weight adjustment %+d (weight %u)", adj, otThreadGetLocalLeaderWeight(instance));
    }
}

esp_err_t br_core_set_leader_adjustment(int8_t adj, br_core_leader_apply_t *result)
{
    ESP_RETURN_ON_FALSE(result, ESP_ERR_INVALID_ARG, TAG, "no result output");
    *result = BR_CORE_LEADER_NOT_READY;
    ESP_RETURN_ON_FALSE(br_core_leader_adjustment_valid(adj), ESP_ERR_INVALID_ARG, TAG, "leader weight adjustment %d out of range", adj);
    ESP_RETURN_ON_FALSE(!s_storage_degraded, ESP_ERR_INVALID_STATE, TAG, "storage is unavailable");

    ESP_RETURN_ON_FALSE(s_leader_mutex, ESP_ERR_INVALID_STATE, TAG, "br_core_note_storage_init was not called");

    // The mutex covers the compare, the NVS write and the store, so concurrent callers cannot interleave them. It is not
    // held while waiting for the OpenThread lock.
    xSemaphoreTake(s_leader_mutex, portMAX_DELAY);
    const bool changed = atomic_load(&s_leader_adj) != adj;
    esp_err_t store_err = ESP_OK;
    if (changed) {
        store_err = store_leader_adjustment(adj);
        if (store_err == ESP_OK) {
            atomic_store(&s_leader_adj, adj);
        }
    }
    xSemaphoreGive(s_leader_mutex);
    ESP_RETURN_ON_ERROR(store_err, TAG, "storing the leader weight adjustment");

    const esp_err_t lock_err = ot_lock_while(ot_usable, LEADER_LOCK_MS);
    if (lock_err != ESP_OK) {
        if (changed) {
            ESP_LOGI(TAG, "leader weight adjustment %+d stored, applied at the next start or weight read", adj);
        }
        *result = lock_err == ESP_ERR_TIMEOUT ? BR_CORE_LEADER_BUSY : BR_CORE_LEADER_NOT_READY;
        return ESP_OK;
    }
    sync_leader_adjustment(esp_openthread_get_instance());
    ot_unlock();
    *result = BR_CORE_LEADER_APPLIED;
    return ESP_OK;
}

int8_t br_core_leader_adjustment(void)
{
    return (int8_t)atomic_load(&s_leader_adj);
}

esp_err_t br_core_leader_weight(uint8_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "no output");
    const esp_err_t err = ot_lock_while(ot_usable, STATUS_LOCK_MS);
    if (err != ESP_OK) {
        return err; // not logged: the GET polls this every few seconds
    }
    otInstance *instance = esp_openthread_get_instance();
    sync_leader_adjustment(instance);
    *out = otThreadGetLocalLeaderWeight(instance);
    ot_unlock();
    return ESP_OK;
}

esp_err_t br_core_become_leader(br_core_leader_refusal_t *refusal, uint8_t *weight)
{
    ESP_RETURN_ON_FALSE(refusal, ESP_ERR_INVALID_ARG, TAG, "no refusal output");
    *refusal = BR_CORE_LEADER_OK;

    const esp_err_t lock_err = ot_lock_while(br_running, LEADER_LOCK_MS);
    if (lock_err == ESP_ERR_TIMEOUT) {
        return ESP_ERR_TIMEOUT;
    }
    if (lock_err != ESP_OK) {
        if (br_core_get_mode() == BR_MODE_BR_RUNNING) {
            return ESP_ERR_NOT_FINISHED; // a radio push is about to stop OpenThread: ot_ready is cleared, the mode is not yet
        }
        *refusal = br_core_leader_check(false, false, false, false, 0, 0);
        return ESP_ERR_INVALID_STATE;
    }

    otInstance *instance = esp_openthread_get_instance();
    const otDeviceRole role = otThreadGetDeviceRole(instance);
    const bool attached = role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER || role == OT_DEVICE_ROLE_LEADER;
    const uint8_t own = otThreadGetLocalLeaderWeight(instance);
    const uint8_t leader = otThreadGetLeaderWeight(instance);

    *refusal = br_core_leader_check(true, attached, role == OT_DEVICE_ROLE_LEADER, otThreadIsRouterEligible(instance), own,
                                    leader);
    if (*refusal != BR_CORE_LEADER_OK) {
        ot_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    const otError error = otThreadBecomeLeader(instance);
    ot_unlock();

    switch (error) {
    case OT_ERROR_NONE:
        ESP_LOGI(TAG, "taking over as leader (weight %u > %u)", own, leader);
        if (weight) {
            *weight = own;
        }
        return ESP_OK;
    case OT_ERROR_NOT_CAPABLE:
        *refusal = BR_CORE_LEADER_WEIGHT_NOT_HIGHER;
        return ESP_ERR_INVALID_STATE;
    case OT_ERROR_INVALID_STATE:
        *refusal = BR_CORE_LEADER_NOT_ATTACHED;
        return ESP_ERR_INVALID_STATE;
    default:
        ESP_LOGE(TAG, "becoming leader: %s", otThreadErrorToString(error));
        return ESP_FAIL;
    }
}

void br_core_set_ot_cli_hook(void (*hook)(bool running))
{
    atomic_store(&s_ot_cli_hook, hook);
}
