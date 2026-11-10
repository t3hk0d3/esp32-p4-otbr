// The functions libziggurat_ot.a imports: the 19 ziggurat_platform_* (ziggurat.h) and the renamed heap
// (zigbee_heap_aligned_alloc / _realloc / _free, build_ziggurat.cmake). Every one of them is called from inside a
// ziggurat_* call, so on the zigbee task, except the heap functions' accounting, which other tasks read.
#include <stdlib.h>
#include <string.h>

#include "aes/esp_aes.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "reboot_gate.h"
#include "ziggurat.h"
#include "zigbee_ccm.h"
#include "zigbee_priv.h"

static const char *TAG = "zigbee";

// The stack's heap lives in PSRAM (spec, Memory in PSRAM); the staticlib asks for internal RAM, which is ignored.
#define HEAP_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

#define PANIC_MAGIC 0x7a199e01u
#define PANIC_MAX 256 // the staticlib's panic handler formats at most 256 bytes

zigbee_heap_stats_t g_zigbee_heap;

/* ---- time, wake, entropy ---- */

uint64_t ziggurat_platform_time_now_us(void)
{
    return (uint64_t)esp_timer_get_time();
}

void ziggurat_platform_timer_arm(uint64_t deadline_us)
{
    zigbee_t *z = &g_zigbee;

    esp_timer_stop(z->timer); // ESP_ERR_INVALID_STATE when it is not running
    z->timer_deadline = deadline_us;
    if (deadline_us == UINT64_MAX) {
        return;
    }
    const uint64_t now = ziggurat_platform_time_now_us();

    esp_timer_start_once(z->timer, deadline_us > now ? deadline_us - now : 1);
}

// esp_timer task: only flags the zigbee task.
void zigbee_platform_timer_cb(void *arg)
{
    zigbee_post_flags(ZIGBEE_FLAG_TIMER);
}

void zigbee_platform_timer_due(void)
{
    zigbee_t *z = &g_zigbee;

    // A flag from a firing that a later arm (or a cancel) superseded is not this deadline's.
    if (z->timer_deadline == UINT64_MAX || ziggurat_platform_time_now_us() < z->timer_deadline) {
        return;
    }
    z->timer_deadline = UINT64_MAX;
    ziggurat_timer_fired();
}

// From inside any ziggurat call (the executor's pender): a coalesced post; the loop calls ziggurat_process.
void ziggurat_platform_wake(void)
{
    zigbee_post_flags(ZIGBEE_FLAG_WAKE);
}

void ziggurat_platform_entropy(uint8_t *buf, size_t len)
{
    esp_fill_random(buf, len);
}

/* ---- AES and CCM* on the AES engine ---- */

static void aes_block(void *ctx, uint8_t block[16])
{
    esp_aes_crypt_ecb(ctx, ESP_AES_ENCRYPT, block, block);
}

void ziggurat_platform_aes128_encrypt_block(const uint8_t *key, uint8_t *block)
{
    esp_aes_context ctx;

    esp_aes_init(&ctx);
    esp_aes_setkey(&ctx, key, 128);
    aes_block(&ctx, block);
    esp_aes_free(&ctx);
}

int32_t ziggurat_platform_ccm_crypt(bool encrypt, const uint8_t *key, const uint8_t *nonce, const uint8_t *auth,
                                    size_t auth_len, const uint8_t *input, uint8_t *output, size_t len, uint8_t *tag,
                                    size_t tag_len)
{
    esp_aes_context ctx;

    esp_aes_init(&ctx);
    esp_aes_setkey(&ctx, key, 128);
    const int rc = zigbee_ccm_crypt(aes_block, &ctx, encrypt, nonce, auth, auth_len, input, output, len, tag, tag_len);

    esp_aes_free(&ctx);
    return rc == 0 ? 0 : -1;
}

/* ---- the host link ---- */

// The satellite's bootstrap is the stack's only client.
bool ziggurat_platform_host_send(const uint8_t *data, size_t len)
{
    zigbee_satellite_on_frame(&g_zigbee.satellite, data, len);
    return true;
}

/* ---- the radio, through the spinel client ---- */

int32_t ziggurat_platform_radio_configure(const struct ziggurat_radio_config_t *config)
{
    // rx_on_when_idle is not passed on: the client always asks for it (a coordinator always receives).
    zigbee_radio_config_t c = {
        .channel = config->channel,
        .tx_power_dbm = config->tx_power_dbm,
        .pan_id = config->pan_id,
        .short_addr = config->short_address,
        .promiscuous = config->promiscuous,
    };

    memcpy(c.ext_addr, config->extended_address, sizeof(c.ext_addr));
    ZIGBEE_STATUS(g_zigbee_status.has_radio_config = true, g_zigbee_status.radio = c);
    const int rc = zigbee_spinel_configure(g_zigbee.spinel, &c);

    ESP_LOGI(TAG, "radio: channel %u, PAN 0x%04x, short 0x%04x, %d dBm: %d", c.channel, c.pan_id, c.short_addr,
             c.tx_power_dbm, rc);
    return rc;
}

int32_t ziggurat_platform_radio_set_channel(uint8_t channel)
{
    ZIGBEE_STATUS(g_zigbee_status.radio.channel = channel);
    return zigbee_spinel_set_channel(g_zigbee.spinel, channel);
}

int32_t ziggurat_platform_radio_set_promiscuous(bool promiscuous)
{
    ZIGBEE_STATUS(g_zigbee_status.radio.promiscuous = promiscuous);
    return zigbee_spinel_set_promiscuous(g_zigbee.spinel, promiscuous);
}

void ziggurat_platform_radio_src_match_clear(void)
{
    zigbee_spinel_src_match_clear(g_zigbee.spinel);
}

int32_t ziggurat_platform_radio_src_match_add_short(uint16_t address)
{
    return zigbee_spinel_src_match_add_short(g_zigbee.spinel, address);
}

int32_t ziggurat_platform_radio_src_match_add_ext(const uint8_t *address)
{
    return zigbee_spinel_src_match_add_ext(g_zigbee.spinel, address);
}

int32_t ziggurat_platform_radio_transmit(const uint8_t *psdu, size_t len, uint8_t channel, bool csma_ca,
                                         uint8_t max_frame_retries, uint8_t max_csma_backoffs)
{
    const int rc = zigbee_spinel_transmit(g_zigbee.spinel, psdu, len, channel, csma_ca, max_frame_retries,
                                          max_csma_backoffs);

    if (rc == 0) {
        ZIGBEE_STATUS(g_zigbee_status.counters.tx++);
    } else {
        ZIGBEE_STATUS(g_zigbee_status.counters.tx_failed++);
    }
    return rc;
}

int32_t ziggurat_platform_radio_energy_scan(uint8_t channel, uint16_t duration_ms)
{
    const int rc = zigbee_spinel_energy_scan(g_zigbee.spinel, channel, duration_ms);

    g_zigbee.scan_channel = channel;
    if (rc == 0) {
        ZIGBEE_STATUS(g_zigbee_status.counters.energy_scans++);
    } else {
        ESP_LOGW(TAG, "energy scan on channel %u not started: %d", channel, rc);
    }
    return rc;
}

void ziggurat_platform_hw_eui64(uint8_t *out)
{
    // The stack starts after the first attach read HWADDR, so the EUI-64 is known by then.
    if (!zigbee_spinel_eui64(g_zigbee.spinel, out)) {
        memset(out, 0, 8);
    }
}

/* ---- reset, panic ---- */

// Through reboot_gate (reboot_gate.h): a running update or C6 firmware push finishes first. The staticlib spins
// forever after this call returns, so it never does: the zigbee task waits here for the reboot instead.
void ziggurat_platform_hard_reset(void)
{
    ESP_LOGW(TAG, "the client asked for a hard reset");
    reboot_gate_restart(0);
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

typedef struct {
    uint32_t magic;
    uint32_t len;
    uint32_t sum;
    char msg[PANIC_MAX];
} panic_record_t;

// The previous boot's panic message, kept over the reset (RTC memory is not cleared by a software reset).
static RTC_NOINIT_ATTR panic_record_t s_panic_rtc;
// This boot's copy of it, given to the stack once.
static char s_last_reset[PANIC_MAX];
static size_t s_last_reset_len;

static uint32_t record_sum(const panic_record_t *r)
{
    uint32_t sum = r->len ^ PANIC_MAGIC;

    for (size_t i = 0; i < r->len && i < sizeof(r->msg); i++) {
        sum = sum * 31 + (uint8_t)r->msg[i];
    }
    return sum;
}

void zigbee_platform_boot(void)
{
    panic_record_t *r = &s_panic_rtc;

    // Only a panic reset follows our abort(); after a power-on the memory holds noise, after a clean reset a record
    // from an earlier boot that was already reported or belongs to another crash.
    if (esp_reset_reason() == ESP_RST_PANIC && r->magic == PANIC_MAGIC && r->len <= sizeof(r->msg) &&
            r->sum == record_sum(r)) {
        memcpy(s_last_reset, r->msg, r->len);
        s_last_reset_len = r->len;
    }
    r->magic = 0;
}

void ziggurat_platform_panic(const uint8_t *msg, size_t len)
{
    panic_record_t *r = &s_panic_rtc;

    if (len > sizeof(r->msg)) {
        len = sizeof(r->msg);
    }
    ESP_LOGE(TAG, "ziggurat panic: %.*s", (int)len, (const char *)msg);
    memcpy(r->msg, msg, len);
    r->len = (uint32_t)len;
    r->sum = record_sum(r);
    r->magic = PANIC_MAGIC;
    abort();
}

size_t ziggurat_platform_last_reset(uint8_t *buf, size_t cap)
{
    size_t n = s_last_reset_len < cap ? s_last_reset_len : cap;

    memcpy(buf, s_last_reset, n);
    s_last_reset_len = 0; // once
    return n;
}

/* ---- the staticlib's heap ---- */

void *zigbee_heap_aligned_alloc(size_t alignment, size_t size, uint32_t caps)
{
    void *p = heap_caps_aligned_alloc(alignment, size, HEAP_CAPS);
    const size_t n = p ? heap_caps_get_allocated_size(p) : 0;

    if (p) {
        ZIGBEE_STATUS(zigbee_heap_stats_alloc(&g_zigbee_heap, n));
    } else {
        ZIGBEE_STATUS(zigbee_heap_stats_failed(&g_zigbee_heap));
    }
    return p;
}

void *zigbee_heap_realloc(void *ptr, size_t size, uint32_t caps)
{
    const size_t old = ptr ? heap_caps_get_allocated_size(ptr) : 0;
    void *p = heap_caps_realloc(ptr, size, HEAP_CAPS);

    if (p == NULL && size > 0) {
        ZIGBEE_STATUS(zigbee_heap_stats_failed(&g_zigbee_heap)); // ptr is untouched
    } else {
        const size_t n = p ? heap_caps_get_allocated_size(p) : 0;

        ZIGBEE_STATUS(zigbee_heap_stats_realloc(&g_zigbee_heap, old, n));
    }
    return p;
}

void zigbee_heap_free(void *ptr)
{
    if (ptr == NULL) {
        return;
    }
    const size_t n = heap_caps_get_allocated_size(ptr);

    heap_caps_free(ptr);
    ZIGBEE_STATUS(zigbee_heap_stats_free(&g_zigbee_heap, n));
}
