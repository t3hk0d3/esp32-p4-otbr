#include "br_core.h"
#include "board_cores.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_br_web.h"
#include "esp_log.h"
#include "esp_mac.h"
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
#include "openthread/border_routing.h"
#include "openthread/dataset.h"
#include "openthread/dataset_ftd.h"
#include "openthread/ip6.h"
#include "openthread/link.h"
#include "openthread/link_raw.h"
#include "openthread/platform/radio.h"
#include "openthread/thread.h"
#include "mode_fsm.h"
#include "rcp_sync.h"
#include "sdkconfig.h"
#ifdef OTBR_TEST_BUILD
#include "driver/gpio.h"
#include "esp_openthread_cli.h"
#endif

static const char *TAG = "br_core";

#define HOSTNAME_PREFIX "esp-otbr-"
#define HOSTNAME_LEN (sizeof(HOSTNAME_PREFIX) + 6)
#define OT_TASK_NAME "ot_main"
#define OT_TASK_STACK 12288
#define OT_TASK_PRIORITY CONFIG_OPENTHREAD_TASK_PRIORITY
#define BRINGUP_TASK_STACK 6144
#define BRINGUP_TASK_PRIORITY 4
#define BOOT_WAIT_MS 180000
#define BRINGUP_WAIT_MS 60000
#define LOOP_STOP_WAIT_MS 5000
#define STATUS_LOCK_MS 100
#define REBOOT_DELAY_MS 1500 // lets the caller of a push report its result before the reboot
#define FAULT_NO_RCP "no rcp firmware"
#define FAULT_INCOMPATIBLE "rcp incompatible"
#define FAULT_ROM_RECOVERY "rom recovery failed"
// Spinel SPI endpoint, an eventfd each for the netif, the task queue and the border router.
#define EVENTFD_COUNT 4

typedef struct {
    StaticSemaphore_t mutex_storage;
    SemaphoreHandle_t mutex;     // guards the fields below
    SemaphoreHandle_t boot_done; // given once OpenThread is initialised or the fault mode was entered
    SemaphoreHandle_t loop_done; // given by the OpenThread task when it left the main loop for a firmware push
    SemaphoreHandle_t bringup_done; // given when the border router task ended
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
    atomic_bool push_busy;     // a firmware push is running (one long operation at a time)
    bool started;
    bool mdns_started;
} core_state_t;

static core_state_t s_core;
static bool s_storage_degraded; // set by br_core_note_storage_init before br_core_start
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
// The platform config must outlive esp_openthread_init: OpenThread keeps pointers into it.
static esp_openthread_platform_config_t s_platform_config;

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
    const bool in_mainloop = s_core.ot_ready;

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
    fault_and_park(s_core.ot_ready ? "RCP did not answer its reset" : FAULT_INCOMPATIBLE);
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
    if (atomic_load(&s_core.push_busy)) {
        return; // the push owns the C6 and resets it itself
    }
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

// Border router start: DHCPv6-PD on, Thread on when a dataset is stored. Spec 5.3 step 5.
static void bringup_task(void *arg)
{
    otInstance *instance = esp_openthread_get_instance();

    ESP_LOGI(TAG, "starting the border router");
    esp_openthread_lock_acquire(portMAX_DELAY);
    esp_err_t err = esp_openthread_border_router_init();
    if (err != ESP_OK) {
        // The radio works, the border router does not: not a radio fault, the mode stays STARTING.
        esp_openthread_lock_release();
        ESP_LOGE(TAG, "border router init: %s", esp_err_to_name(err));
        set_reason("border router init failed");
        xSemaphoreGive(s_core.bringup_done);
        vTaskDelete(NULL);
        return;
    }
    s_core.border_router_up = true;
    otBorderRoutingDhcp6PdSetEnabled(instance, true);
    ESP_LOGI(TAG, "DHCPv6-PD enabled");

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
    esp_openthread_lock_release();

    // A radio fault entered meanwhile (e.g. the boot wait timed out) stays.
    mode_fsm_set_started(&s_fsm);
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

static void on_got_ip6(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const ip_event_got_ip6_t *event = data;

    if (event->esp_netif == s_core.backbone) {
        note_address();
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

    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_OPENTHREAD();
    esp_netif_t *netif = esp_netif_new(&netif_config);
    ESP_RETURN_ON_FALSE(netif, ESP_ERR_NO_MEM, TAG, "creating the OpenThread netif");
    s_core.ot_netif = netif;
    ESP_RETURN_ON_ERROR(esp_netif_attach(netif, esp_openthread_netif_glue_init(&s_platform_config)), TAG,
                        "attaching the OpenThread netif");
#ifdef OTBR_TEST_BUILD
    esp_openthread_lock_acquire(portMAX_DELAY);
    esp_openthread_cli_init();
    esp_openthread_lock_release();
#endif
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
        // During a push the mode is transiently RADIO_FAULT; br_core_push_radio_firmware sets the flag from the final state.
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

esp_err_t br_core_mdns_start(void)
{
    uint8_t mac[6];
    char hostname[HOSTNAME_LEN];

    if (s_core.mdns_started) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_ETH), TAG, "reading the MAC");
    snprintf(hostname, sizeof(hostname), HOSTNAME_PREFIX "%02x%02x%02x", mac[3], mac[4], mac[5]);
    ESP_RETURN_ON_ERROR(mdns_init(), TAG, "mdns_init");
    ESP_RETURN_ON_ERROR(mdns_hostname_set(hostname), TAG, "mdns_hostname_set");
    ESP_RETURN_ON_ERROR(mdns_instance_name_set(hostname), TAG, "mdns_instance_name_set");
    s_core.mdns_started = true;
    ESP_LOGI(TAG, "mDNS hostname %s.local", hostname);
    return ESP_OK;
}

void br_core_note_storage_init(esp_err_t storage_init_err)
{
    s_storage_degraded = mode_fsm_storage_degraded(storage_init_err);
    if (s_storage_degraded) {
        ESP_LOGE(TAG, "storage init failed (%s): booting DEGRADED, OpenThread will not start; NVS is NOT erased "
                 "(it holds the Thread credentials)", esp_err_to_name(storage_init_err));
    }
}

esp_err_t br_core_start(esp_netif_t *backbone)
{
    ESP_RETURN_ON_FALSE(backbone, ESP_ERR_INVALID_ARG, TAG, "no backbone netif");
    ESP_RETURN_ON_FALSE(!s_core.started, ESP_ERR_INVALID_STATE, TAG, "already started");
    s_core.started = true;
    s_core.backbone = backbone;
    s_core.mutex = xSemaphoreCreateMutexStatic(&s_core.mutex_storage);
    s_core.boot_done = xSemaphoreCreateBinary();
    s_core.loop_done = xSemaphoreCreateBinary();
    s_core.bringup_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_core.boot_done && s_core.loop_done && s_core.bringup_done, ESP_ERR_NO_MEM, TAG, "no memory");

    // mDNS first (normally started earlier by app_main): it answers while the radio boots.
    // Not essential for the radio: without mDNS the board is reachable by its address, and an early return here would leave
    // the mode STARTING with no reason and no OpenThread task.
    const esp_err_t mdns_err = br_core_mdns_start();

    if (mdns_err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS did not start (%s), continuing without it", esp_err_to_name(mdns_err));
    }
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_GOT_IP6, on_got_ip6, NULL), TAG, "event handler");

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
    if (!ot_ready || !esp_openthread_lock_acquire(pdMS_TO_TICKS(STATUS_LOCK_MS))) {
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
static esp_err_t run_c6_job(const rcp_fw_source_t *source, char *err_detail, size_t err_cap)
{
    if (source) {
        return rcp_sync_push_firmware(source, err_detail, err_cap);
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
        const esp_err_t abandoned_err = run_c6_job(source, err_detail, err_cap);
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

    const esp_err_t push_err = run_c6_job(source, err_detail, err_cap);
    if (push_err != ESP_OK) {
        ESP_LOGE(TAG, "%s failed: %s", what, esp_err_to_name(push_err));
    }

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
        if (source) {
            ESP_LOGE(TAG, "restarting OpenThread after the %s failed: %s", what, esp_err_to_name(err));
        } else { // the normal outcome of a ROM recovery on a C6 without RCP firmware
            ESP_LOGW(TAG, "restarting OpenThread after the %s failed: %s (the status shows why)", what, esp_err_to_name(err));
        }
        if (s_core.fault_reason[0] == '\0') {
            enter_fault("radio restart failed");
        }
        return mode_fsm_radio_job_result(source == NULL, push_err, err);
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

esp_err_t br_core_enter_sniffer(uint8_t channel, otLinkRawReceiveDone receive_cb)
{
    ESP_RETURN_ON_FALSE(receive_cb != NULL, ESP_ERR_INVALID_ARG, TAG, "no receive callback");
    ESP_RETURN_ON_FALSE(ot_ready_now(), ESP_ERR_INVALID_STATE, TAG, "OpenThread is not running");
    esp_openthread_lock_acquire(portMAX_DELAY);
    if (!ot_ready_now()) { // a push took OpenThread over while this task waited for the lock
        esp_openthread_lock_release();
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
    return err;
}

void br_core_set_sniffer_end_cb(void (*cb)(void))
{
    mode_fsm_set_sniffer_end_cb(&s_fsm, cb);
}

esp_err_t br_core_leave_sniffer(void)
{
    ESP_RETURN_ON_FALSE(ot_ready_now(), ESP_ERR_INVALID_STATE, TAG, "OpenThread is not running");
    esp_openthread_lock_acquire(portMAX_DELAY);
    if (!ot_ready_now()) {
        esp_openthread_lock_release();
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = mode_fsm_leave_sniffer(&s_fsm);
    if (err == ESP_OK) {
        esp_br_web_set_openthread_ready(true);
    }
    esp_openthread_lock_release();
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
    bool expected = false;
    ESP_RETURN_ON_FALSE(atomic_compare_exchange_strong(&s_core.push_busy, &expected, true), ESP_ERR_INVALID_STATE, TAG,
                        "a firmware push or ROM recovery is already running");

    // Guard: under the OpenThread lock, re-check the mode and make ot_ready false, so that the sniffer entry points
    // (which re-check it under the same lock) cannot start while OpenThread is torn down. The lock only exists while
    // OpenThread is initialised.
    const bool had_openthread = ot_ready_now();
    if (had_openthread) {
        esp_openthread_lock_acquire(portMAX_DELAY);
    }
    esp_err_t err = mode_fsm_push_allowed(br_core_get_mode(), false);
    if (err == ESP_OK) {
        esp_br_web_set_openthread_ready(false); // Thread REST off before Thread stops
        xSemaphoreTake(s_core.mutex, portMAX_DELAY);
        s_core.ot_ready = false;
        xSemaphoreGive(s_core.mutex);
    }
    if (had_openthread) {
        esp_openthread_lock_release();
    }

    if (err == ESP_OK) {
        // No OpenThread lock: the operation deletes and re-creates it.
        err = source ? mode_fsm_push_firmware(&s_fsm, source, err_detail, err_cap)
              : mode_fsm_rom_recover(&s_fsm, err_detail, err_cap);
        if (err == ESP_ERR_INVALID_STATE && had_openthread) {
            // Refused after the guard cleared the state: nothing was stopped, put it back.
            xSemaphoreTake(s_core.mutex, portMAX_DELAY);
            s_core.ot_ready = true;
            xSemaphoreGive(s_core.mutex);
        }
        // The flag follows the final state on every path, never the transient RADIO_FAULT of the job.
        esp_br_web_set_openthread_ready(mode_fsm_rest_ready_after_push(br_core_get_mode(), ot_ready_now()));
    }
    atomic_store(&s_core.push_busy, false);
    return err;
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
