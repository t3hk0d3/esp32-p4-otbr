#include "br_core.h"
#include "board_cores.h"

#include <assert.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_br_web.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_border_router.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_spinel.h"
#include "esp_openthread_types.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs_eventfd.h"
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
#include "openthread/platform/radio.h"
#include "openthread/thread.h"
#include "openthread/thread_ftd.h"
#include "openthread/trel.h"
#include "mode_fsm.h"
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
// Spinel SPI endpoint, an eventfd each for the netif, the task queue and the border router, and one for TREL reception.
#if CONFIG_OPENTHREAD_RADIO_TREL
#define EVENTFD_COUNT 5
#else
#define EVENTFD_COUNT 4
#endif
#define TREL_SET_LOCK_MS 1000
#define TREL_QUERY_MS 3000 // how long a peer query collects answers (esp mdns sends it about once a second)
#define TREL_QUERY_DELAY_MS 100 // after an address event: a burst of them gets one query

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
    set_compatible(false);
    // loop_running, not ot_ready: a push clears ot_ready while OpenThread still runs.
    fault_and_park(s_core.loop_running ? "RCP did not answer its reset" : FAULT_INCOMPATIBLE);
}

static void on_compatibility_error(void)
{
    set_compatible(false);
    fault_and_park(FAULT_INCOMPATIBLE);
}

// Hardware reset of the C6, used by OpenThread when the spinel software reset fails. Phase 0: without a
// registered handler a failed RCP reset asserts.
static esp_err_t reset_rcp(void)
{
#ifdef OTBR_TEST_BUILD
    if (br_core_test_c6_hold()) {
        ESP_LOGW(TAG, "test build: holding the C6 in reset");
        gpio_set_level(P4_PIN_C6_RESET, 0);
        return ESP_OK;
    }
#endif
    return rcp_sync_boot_rcp();
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

static esp_openthread_platform_config_t platform_config(void)
{
    const esp_openthread_platform_config_t config = {
        .radio_config = {
            .radio_mode = RADIO_MODE_SPI_RCP,
            .radio_spi_config = {
                .host_device = SPI2_HOST,
                .dma_channel = 2,
                .spi_interface = {
                    .mosi_io_num = P4_PIN_RCP_MOSI,
                    .miso_io_num = P4_PIN_RCP_MISO,
                    .sclk_io_num = P4_PIN_RCP_SCLK,
                    .quadwp_io_num = -1,
                    .quadhd_io_num = -1,
                },
                .spi_device = {
                    .cs_ena_pretrans = 2,
                    .input_delay_ns = 100,
                    .mode = 0,
                    .clock_speed_hz = P4_RCP_SPI_HZ,
                    .spics_io_num = P4_PIN_RCP_CS,
                    .queue_size = 5,
                },
                .intr_pin = P4_PIN_RCP_INT,
            },
        },
        .host_config = {.host_connection_mode = HOST_CONNECTION_MODE_NONE},
        .port_config = {.storage_partition_name = "nvs", .netif_queue_size = 10, .task_queue_size = 10},
    };
    return config;
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

static esp_err_t init_openthread(void)
{
    esp_vfs_eventfd_config_t eventfd_config = {.max_fds = EVENTFD_COUNT};
    esp_err_t err = esp_vfs_eventfd_register(&eventfd_config);
    ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_INVALID_STATE, err, TAG, "eventfd register: %s", esp_err_to_name(err));

    esp_openthread_register_rcp_failure_handler(on_rcp_failure);
    esp_openthread_set_coprocessor_reset_failure_callback(on_coprocessor_reset_failure);
    esp_openthread_set_compatibility_error_callback(on_compatibility_error);
    esp_openthread_set_backbone_netif(s_core.backbone);

    s_platform_config = platform_config();
    ESP_RETURN_ON_ERROR(esp_openthread_init(&s_platform_config), TAG, "esp_openthread_init");
    s_core.ot_initialised = true;
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

// Spec 5.3 steps 3 and 4: probe the installed RCP, reset the C6 into it, start OpenThread. Nothing is pushed here.
// Returns ESP_OK when OpenThread runs; otherwise the fault mode was entered.
static esp_err_t boot_radio(void)
{
    rcp_sync_probe_result_t probe;
    esp_err_t err = run_probe(&probe);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RCP probe could not run: %s", esp_err_to_name(err));
        memset(&probe, 0, sizeof(probe));
    }
    xSemaphoreTake(s_core.mutex, portMAX_DELAY);
    s_core.rcp_installed_known = probe.updater_reachable;
    strlcpy(s_core.rcp_installed, probe.installed_version, sizeof(s_core.rcp_installed));
    s_core.rcp_compatible = false;
    xSemaphoreGive(s_core.mutex);

    xSemaphoreTake(s_core.mutex, portMAX_DELAY);
    s_core.rom_recovery_failed = mode_fsm_next_recovery_failed(s_core.rom_recovery_failed,
                                                               probe.rom_recovery_failed || probe.rom_recovered, probe.rom_recovered);
    xSemaphoreGive(s_core.mutex);
    if (mode_fsm_rom_failure_is_fault(&probe)) {
        // Spec 7: the updater was silent and the recovery failed; no further automatic attempt this boot.
        ESP_LOGE(TAG, "the ROM recovery of the C6 base image failed");
        enter_fault(FAULT_ROM_RECOVERY);
        return ESP_ERR_INVALID_STATE;
    }
    if (probe.rom_recovery_failed) {
        // The updater answered (base version mismatch): an RCP may be installed, so the boot goes on with it.
        ESP_LOGE(TAG, "the ROM recovery of the C6 base image failed; continuing with the installed RCP (see status jumper.recovery_failed)");
    }
    if (probe.esp_hosted) {
        // A factory C6: OpenThread cannot run on it. The device page asks for c6_installer.bin.
        ESP_LOGE(TAG, "the C6 runs its factory ESP-Hosted firmware: upload c6_installer.bin, then c6_rcp.bin");
        enter_fault(RCP_SYNC_ESP_HOSTED_ERROR);
        return ESP_ERR_NOT_FOUND;
    }
    if (!probe.updater_reachable) {
        ESP_LOGW(TAG, "RCP probe failed (%s), trying the installed firmware", probe.last_error);
    } else if (!probe.installed_valid) {
        ESP_LOGE(TAG, "the C6 holds no RCP firmware");
        enter_fault(FAULT_NO_RCP);
        return ESP_ERR_NOT_FOUND;
    } else {
        ESP_LOGI(TAG, "installed RCP firmware %s", probe.installed_version);
    }

    if (!mode_fsm_radio_start_allowed(s_storage_degraded)) {
        ESP_LOGE(TAG, "storage is unavailable (NVS): OpenThread is not started");
        enter_fault(MODE_FSM_FAULT_STORAGE);
        return ESP_ERR_INVALID_STATE;
    }
    err = reset_rcp();
    if (err != ESP_OK) {
        enter_fault("resetting the RCP failed");
        return err;
    }
    err = init_openthread();
    if (err != ESP_OK) {
        if (!s_core.ot_initialised) {
            s_core.init_abandoned = true;
        }
        enter_fault(FAULT_INCOMPATIBLE);
        return err;
    }
    set_compatible(true);
    return ESP_OK;
}

static void ot_task(void *arg)
{
    if (boot_radio() != ESP_OK) {
        s_core.ot_task = NULL;
        vTaskDelete(NULL);
        return;
    }
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
    ESP_LOGI(TAG, "OpenThread initialised");
    s_core.loop_running = true;
    xSemaphoreGive(s_core.boot_done);

    esp_err_t err = esp_openthread_launch_mainloop();
    s_core.loop_running = false;
    s_core.ot_task = NULL;
    if (s_core.push_pending) {
        // br_core_push_radio_firmware asked for the exit and tears OpenThread down now.
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

    ESP_RETURN_ON_ERROR(start_ot_task(), TAG, "cannot create the OpenThread task");
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
    otInstance *instance = esp_openthread_get_instance();
    const otTrelCounters *counters = otTrelGetCounters(instance);
    otTrelPeerIterator iterator;
    const otTrelPeer *peer;

    out->state_valid = true;
    out->active = otTrelIsEnabled(instance) && otThreadGetDeviceRole(instance) != OT_DEVICE_ROLE_DISABLED;
    out->peers = otTrelGetNumberOfPeers(instance);
    if (counters) {
        out->tx_packets = counters->mTxPackets;
        out->tx_bytes = counters->mTxBytes;
        out->tx_failures = counters->mTxFailure;
        out->rx_packets = counters->mRxPackets;
        out->rx_bytes = counters->mRxBytes;
    }
    otTrelInitPeerIterator(instance, &iterator);
    while (out->peer_count < BR_CORE_TREL_PEERS_MAX && (peer = otTrelGetNextPeer(instance, &iterator)) != NULL) {
        br_core_trel_peer_t *entry = &out->peer_list[out->peer_count++];

        for (size_t i = 0; i < sizeof(peer->mExtAddress.m8); i++) {
            snprintf(&entry->ext_address[2 * i], 3, "%02x", peer->mExtAddress.m8[i]);
        }
        otIp6SockAddrToString(&peer->mSockAddr, entry->sock_address, sizeof(entry->sock_address));
    }
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

// Stop OpenThread, run the C6 job (push the image, or ROM recovery when `source` is NULL), start OpenThread again
// (its boot probes, resets the C6 and initialises). Needs a task that is not the OpenThread task (it cannot stop its
// own main loop). The caller (run_radio_job) has cleared ot_ready and the Thread REST flag. Blocks until the border
// router is up again or the restart failed.
static esp_err_t op_radio_job(const rcp_fw_source_t *source, char *err_detail, size_t err_cap, bool *restarted)
{
    const char *what = source ? "RCP firmware push" : "ROM recovery";

    *restarted = false;
    if (xTaskGetCurrentTaskHandle() == s_core.ot_task) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (s_core.init_abandoned) {
        // OpenThread's start was abandoned half-way (RCP incompatible): its SPI driver still holds SPI2 and its state
        // cannot be torn down, so it cannot be restarted in place. The link uses another SPI host (link_spi.c) for the
        // push, and a successful push is followed by a reboot, which starts OpenThread on a clean system. The fault
        // state and reason stay until then.
        ESP_LOGW(TAG, "%s without stopping OpenThread (abandoned start), reboot follows", what);
        bool abandoned_base_only = false;
        const esp_err_t abandoned_err = run_c6_job(source, err_detail, err_cap, &abandoned_base_only);
        if (abandoned_err == ESP_OK) {
            // The C6 is still in its updater (after a ROM recovery: its normal boot, the probe enters the updater again):
            // read back the version so that the HTTP reply and the status show it.
            rcp_sync_probe_result_t probe = {0};
            const bool probe_ok = rcp_sync_probe(&probe) == ESP_OK && probe.updater_reachable;
            xSemaphoreTake(s_core.mutex, portMAX_DELAY);
            mode_fsm_installed_after_push(probe_ok, probe.installed_version, &s_core.rcp_installed_known, s_core.rcp_installed,
                                          sizeof(s_core.rcp_installed));
            xSemaphoreGive(s_core.mutex);
            xTaskCreatePinnedToCore(reboot_task, "br_reboot", 2048, NULL, BRINGUP_TASK_PRIORITY, NULL, MGMT_CORE);
        } else {
            ESP_LOGE(TAG, "%s failed: %s", what, esp_err_to_name(abandoned_err));
        }
        return abandoned_err;
    }

    ESP_LOGI(TAG, "%s: stopping OpenThread", what);
    teardown_openthread();
    xSemaphoreTake(s_core.mutex, portMAX_DELAY);
    s_core.ot_ready = false;
    s_core.bringup_started = false;
    s_core.fault_reason[0] = '\0';
    xSemaphoreGive(s_core.mutex);
    xSemaphoreTake(s_core.boot_done, 0);
    xSemaphoreTake(s_core.bringup_done, 0);

    bool base_only = false;
    const esp_err_t push_err = run_c6_job(source, err_detail, err_cap, &base_only);
    if (push_err != ESP_OK) {
        ESP_LOGE(TAG, "%s failed: %s", what, esp_err_to_name(push_err));
    }
    // base_only: the C6 has its updater and no RCP, so the restart below ends in RADIO_FAULT "no rcp firmware" by design.

    esp_err_t err = start_ot_task();
    if (err == ESP_OK) {
        err = xSemaphoreTake(s_core.boot_done, pdMS_TO_TICKS(BOOT_WAIT_MS)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
    }
    if (err == ESP_OK && !s_core.ot_ready) {
        err = ESP_FAIL; // the fault mode was entered during the boot
    }
    if (err == ESP_OK) {
        err = xSemaphoreTake(s_core.bringup_done, pdMS_TO_TICKS(BRINGUP_WAIT_MS)) == pdTRUE && s_core.border_router_up
              ? ESP_OK : ESP_FAIL;
    }
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
    // After a boot timeout ("radio boot timed out") the OpenThread task may still be in boot_radio, driving the C6 link.
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
