#include "sniffer.h"

#include <stdatomic.h>
#include <string.h>

#include "br_core.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#ifdef CONFIG_IDF_TARGET_LINUX
#include <time.h>
#else
#include "esp_timer.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "openthread/link_raw.h"

static const char *TAG = "sniffer";

// The PSDU the raw link delivers ends in two bytes that are not an FCS (see SNIFFER_RX_PSDU_HAS_FCS). The driver treats
// lengths below 3 as invalid, so those never enter the ring.
#define MIN_STORED_PSDU_LEN 3

// Ring entry: this header, then the PSDU. Entries are written and read as bytes, wrapping at the end of the ring.
#define ENTRY_HEADER_LEN 12
#define RING_MASK (SNIFFER_RING_BYTES - 1)
_Static_assert((SNIFFER_RING_BYTES & (SNIFFER_RING_BYTES - 1)) == 0, "the ring size must be a power of two");

static struct {
    SemaphoreHandle_t lock; // lifecycle and the reader's use of the ring; never taken by the receive callback
    StaticSemaphore_t lock_storage;
    SemaphoreHandle_t wake; // binary: a frame arrived or the capture ended
    StaticSemaphore_t wake_storage;
    uint8_t *ring;          // NULL when there is no capture whose data is unread (changes only under `lock`)
    _Atomic uint32_t head;  // bytes written, only the receive callback advances it
    _Atomic uint32_t tail;  // bytes read, only the reader advances it
    _Atomic uint32_t frames;
    _Atomic uint32_t drops;
    _Atomic bool active;    // frames are being captured; the receive callback checks it
    _Atomic bool ended_by_fault;
    _Atomic int64_t end_us; // when `active` went false
    int64_t start_us;
    uint32_t duration_s;
    uint8_t channel;
    bool header_sent;
    bool end_cb_registered;
    bool leave_error_logged; // once per capture
    _Atomic int init_state;  // 0 = not initialised, 1 = initialising, 2 = done
} s;

// Race-free one-time creation of the lock and the wake-up semaphore; every public entry point calls it.
static void ensure_init(void)
{
    int expected = 0;

    if (atomic_load(&s.init_state) == 2) {
        return;
    }
    if (atomic_compare_exchange_strong(&s.init_state, &expected, 1)) {
        s.lock = xSemaphoreCreateMutexStatic(&s.lock_storage);
        s.wake = xSemaphoreCreateBinaryStatic(&s.wake_storage);
        atomic_store(&s.init_state, 2);
        return;
    }
    while (atomic_load(&s.init_state) != 2) {
        vTaskDelay(1);
    }
}

static int64_t now_us(void)
{
#ifdef CONFIG_IDF_TARGET_LINUX
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
#else
    return esp_timer_get_time();
#endif
}

static void ring_write(uint32_t pos, const void *src, size_t len)
{
    const size_t first = SNIFFER_RING_BYTES - (pos & RING_MASK);
    const size_t n = len < first ? len : first;

    memcpy(s.ring + (pos & RING_MASK), src, n);
    memcpy(s.ring, (const uint8_t *)src + n, len - n);
}

static void ring_read(uint32_t pos, void *dst, size_t len)
{
    const size_t first = SNIFFER_RING_BYTES - (pos & RING_MASK);
    const size_t n = len < first ? len : first;

    memcpy(dst, s.ring + (pos & RING_MASK), n);
    memcpy((uint8_t *)dst + n, s.ring, len - n);
}

// OpenThread task: copy only. No allocation, no lock, no logging.
static void receive_done(otInstance *instance, otRadioFrame *frame, otError error)
{
    if (!atomic_load(&s.active) || error != OT_ERROR_NONE || frame == NULL || frame->mPsdu == NULL ||
            frame->mLength > SNIFFER_MAX_PSDU_LEN) {
        return;
    }
    if (frame->mLength < MIN_STORED_PSDU_LEN) { // hardware junk: counted, not stored
        atomic_fetch_add(&s.drops, 1);
        return;
    }
    const uint32_t head = atomic_load_explicit(&s.head, memory_order_relaxed);
    const uint32_t tail = atomic_load_explicit(&s.tail, memory_order_acquire);
    const uint32_t need = ENTRY_HEADER_LEN + frame->mLength;

    if (SNIFFER_RING_BYTES - (head - tail) < need) {
        atomic_fetch_add(&s.drops, 1);
        return;
    }
    uint8_t header[ENTRY_HEADER_LEN];
    const uint64_t timestamp = frame->mInfo.mRxInfo.mTimestamp;

    memcpy(header, &timestamp, sizeof(timestamp));
    header[8] = (uint8_t)frame->mInfo.mRxInfo.mRssi;
    header[9] = frame->mInfo.mRxInfo.mLqi;
    header[10] = frame->mChannel;
    header[11] = (uint8_t)frame->mLength;
    ring_write(head, header, sizeof(header));
    ring_write(head + ENTRY_HEADER_LEN, frame->mPsdu, frame->mLength);
    atomic_store_explicit(&s.head, head + need, memory_order_release);
    atomic_fetch_add(&s.frames, 1);
    xSemaphoreGive(s.wake);
}

static void mark_ended(void)
{
    if (atomic_exchange(&s.active, false)) {
        atomic_store(&s.end_us, now_us());
    }
}

// br_core: the mode left SNIFFER on its own (OpenThread task or fault task): flags and a wake-up only.
static void sniffer_end_cb(void)
{
    atomic_store(&s.ended_by_fault, true);
    mark_ended();
    xSemaphoreGive(s.wake);
}

static void free_ring(void)
{
    heap_caps_free(s.ring);
    s.ring = NULL;
}

// Leave the sniffer mode. Caller holds the lock. An end that raced in (br_core already left) counts as done.
static esp_err_t finish_capture(void)
{
    if (!atomic_load(&s.active)) {
        return ESP_OK;
    }
    const esp_err_t err = br_core_leave_sniffer();

    // Someone else already left the sniffer mode (not SNIFFER any more): the capture is over, whatever the error.
    if (err != ESP_OK && atomic_load(&s.active) && br_core_get_mode() == BR_MODE_SNIFFER) {
        if (!s.leave_error_logged) {
            s.leave_error_logged = true;
            ESP_LOGE(TAG, "leaving the sniffer mode failed: %s", esp_err_to_name(err));
        }
        return err;
    }
    mark_ended();
    xSemaphoreGive(s.wake);
    return ESP_OK;
}

esp_err_t sniffer_start(uint8_t channel, uint32_t duration_s)
{
    ensure_init();
    xSemaphoreTake(s.lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;

    if (atomic_load(&s.active)) {
        err = ESP_ERR_INVALID_STATE;
        goto out;
    }
    free_ring(); // an ended capture nobody drained
    s.ring = heap_caps_malloc(SNIFFER_RING_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s.ring == NULL) {
        err = ESP_ERR_NO_MEM;
        goto out;
    }
    atomic_store(&s.head, 0);
    atomic_store(&s.tail, 0);
    atomic_store(&s.frames, 0);
    atomic_store(&s.drops, 0);
    atomic_store(&s.ended_by_fault, false);
    xSemaphoreTake(s.wake, 0); // clear a stale wake-up
    s.channel = channel;
    s.duration_s = duration_s;
    s.header_sent = false;
    s.leave_error_logged = false;
    s.start_us = now_us();
    if (!s.end_cb_registered) {
        br_core_set_sniffer_end_cb(sniffer_end_cb);
        s.end_cb_registered = true;
    }
    atomic_store(&s.active, true); // frames can arrive as soon as the raw link is on
    err = br_core_enter_sniffer(channel, receive_done);
    if (err != ESP_OK) {
        atomic_store(&s.active, false);
        free_ring();
    }
out:
    xSemaphoreGive(s.lock);
    return err;
}

esp_err_t sniffer_stop(void)
{
    ensure_init();
    xSemaphoreTake(s.lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;

    if (s.ring != NULL) {
        err = finish_capture();
        if (err == ESP_OK) {
            free_ring();
        }
    }
    xSemaphoreGive(s.lock);
    return err;
}

// Append whole entries to `buf` while they fit. Caller holds the lock and s.ring is not NULL.
static size_t drain_ring(uint8_t *buf, size_t cap)
{
    size_t n = 0;
    uint32_t tail = atomic_load_explicit(&s.tail, memory_order_relaxed);
    const uint32_t head = atomic_load_explicit(&s.head, memory_order_acquire);

    while (tail != head) {
        uint8_t header[ENTRY_HEADER_LEN];
        sniffer_frame_t frame;

        ring_read(tail, header, sizeof(header));
        memcpy(&frame.timestamp_us, header, sizeof(frame.timestamp_us));
        frame.rssi = (int8_t)header[8];
        frame.lqi = header[9];
        frame.channel = header[10];
        frame.psdu_len = header[11];
        if (frame.psdu_len < MIN_STORED_PSDU_LEN || frame.psdu_len > SNIFFER_MAX_PSDU_LEN) {
            // Never stored by receive_done; skipping keeps one bad entry from blocking the ring for good.
            tail += ENTRY_HEADER_LEN + frame.psdu_len;
            atomic_store_explicit(&s.tail, tail, memory_order_release);
            continue;
        }
        ring_read(tail + ENTRY_HEADER_LEN, frame.psdu, frame.psdu_len);
        const size_t written = sniffer_pcap_record(&frame, SNIFFER_RX_PSDU_HAS_FCS, buf + n, cap - n);
        if (written == 0) {
            break; // does not fit
        }
        n += written;
        tail += ENTRY_HEADER_LEN + frame.psdu_len;
        atomic_store_explicit(&s.tail, tail, memory_order_release);
    }
    return n;
}

static bool duration_over(void)
{
    return s.duration_s != 0 && now_us() - s.start_us >= (int64_t)s.duration_s * 1000000;
}

size_t sniffer_read(uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    ensure_init();
    if (cap < SNIFFER_MIN_READ_CAP) {
        return 0;
    }
    const int64_t deadline_us = now_us() + (int64_t)timeout_ms * 1000;

    for (;;) {
        xSemaphoreTake(s.lock, portMAX_DELAY);
        if (s.ring == NULL) {
            xSemaphoreGive(s.lock);
            return 0;
        }
        if (atomic_load(&s.active) && duration_over()) {
            finish_capture();
        }
        size_t n = 0;

        if (!s.header_sent) {
            n = sniffer_pcap_global_header(buf, cap);
            s.header_sent = true;
        }
        n += drain_ring(buf + n, cap - n);
        const bool ended = !atomic_load(&s.active);

        if (n == 0 && ended) { // drained: the capture is over
            free_ring();
        }
        int64_t wait_us = deadline_us - now_us();

        if (n == 0 && !ended && s.duration_s != 0) { // wake up when the duration ends
            const int64_t left_us = s.start_us + (int64_t)s.duration_s * 1000000 - now_us();
            wait_us = wait_us < left_us ? wait_us : left_us + 1000;
        }
        xSemaphoreGive(s.lock);
        if (n > 0 || ended || wait_us <= 0) {
            return n;
        }
        const TickType_t ticks = pdMS_TO_TICKS((wait_us + 999) / 1000);
        xSemaphoreTake(s.wake, ticks > 0 ? ticks : 1);
    }
}

void sniffer_get_stats(sniffer_stats_t *out)
{
    ensure_init();
    const bool active = atomic_load(&s.active);
    const int64_t end_us = active ? now_us() : atomic_load(&s.end_us);

    memset(out, 0, sizeof(*out));
    out->active = active;
    out->ended_by_fault = atomic_load(&s.ended_by_fault);
    out->channel = s.channel;
    out->frames = atomic_load(&s.frames);
    out->drops = atomic_load(&s.drops);
    out->duration_s = s.duration_s;
    out->elapsed_s = end_us > s.start_us ? (uint32_t)((end_us - s.start_us) / 1000000) : 0;
}
