// Shared between the owner task and registry (mux.c) and the USB transport (usb_transport.c). Unless a function says
// otherwise, only the owner task calls it.
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "radio_mux.h"
#include "radio_mux_detect.h"
#include "radio_mux_hdlc.h"
#include "radio_mux_pool.h"

// Room in front of a frame in its pool slot: the C6's 5-byte SPI header (R12). USB frames start at the same offset.
#define MUX_SLOT_HDR 5
#define MUX_CACHE_LINE 64 // CONFIG_CACHE_L1_CACHE_LINE_SIZE and CONFIG_CACHE_L2_CACHE_LINE_SIZE on the P4
#define MUX_ALIGN_UP(n, a) ((((n) + (a) - 1) / (a)) * (a))
// A full SPI transaction (header + largest frame) plus the HDLC decoder's two CRC bytes, rounded to the cache line.
#define MUX_SLOT_SIZE MUX_ALIGN_UP(MUX_SLOT_HDR + RADIO_MUX_FRAME_MAX + RADIO_MUX_HDLC_CRC_LEN, MUX_CACHE_LINE)

typedef enum {
    MUX_RX_DISCARD, // nobody reads: received bytes are dropped
    MUX_RX_DETECT,  // bytes into the ring for the detection (owner task), no eventfd
    MUX_RX_RAW,     // bytes into the ring for a raw consumer, eventfd poked
    MUX_RX_FRAMES,  // HDLC decoded into pool slots, RX_FRAME posted
} mux_rx_mode_t;

typedef struct {
    uint16_t vid, pid;
    uint8_t addr;
    radio_mux_driver_t driver;
    char serial[RADIO_MUX_SERIAL_MAX];
    char product[RADIO_MUX_LABEL_MAX];
} mux_usb_dev_t;

// One transport: the C6 or the USB port (one device open at a time). Runtime state only; the stored settings live
// in the channel records.
typedef struct mux_port {
    radio_mux_kind_t kind;
    radio_mux_attach_t att;                // owner
    struct radio_mux_chan *chan;           // owner: the channel on this transport, NULL while ABSENT
    const radio_mux_consumer_t *consumer;  // owner: attached consumer
    mux_usb_dev_t dev;                     // owner: the device on the port (USB)
    mux_usb_dev_t pending;                 // owner: the device to probe next (PLUG, or deferred while CLOSING)
    char fault[RADIO_MUX_REASON_MAX];      // owner: why the device is not open or not attached; "" when fine
    char warn[RADIO_MUX_REASON_MAX];       // owner: a warning of the last open (flow control unsupported, baud off)
    radio_mux_detect_t det;                // owner: detection while PROBING
    int64_t det_wait_ms;
    uint32_t det_baud;
    int64_t presence_check_us;

    // Delivery. Changed by the owner with rx_lock held, read by the transport with rx_lock held.
    QueueHandle_t evq;   // the channel's own queue (consumers without a bound queue)
    QueueHandle_t bound; // the consumer's queue, NULL = evq + eventfd
    int efd;

    // Receive path: the transport's callback holds rx_lock for its whole run, the owner while it changes the mode.
    SemaphoreHandle_t rx_lock;
    mux_rx_mode_t rx_mode;
    radio_mux_hdlc_dec_t dec;
    uint8_t *dec_slot;   // slot the decoder writes into (at MUX_SLOT_HDR); NULL = into `discard`
    uint8_t *discard;    // decoder target while the pool is empty: the frame is parsed and dropped
    uint32_t dec_errors; // crc_errors + overruns already added to the counters
    RingbufHandle_t ring; // raw and detection bytes

    // Pool and counters: under `lock` (taken by the transport, the consumers' release and tx, and the snapshot).
    portMUX_TYPE lock;
    radio_mux_pool_t pool;
    radio_mux_counters_t ctr;

    // Transmit: tx_lock serialises senders and the owner's close; tx_ok is false whenever a sender must fail at once.
    SemaphoreHandle_t tx_lock;
    atomic_bool tx_ok;
    bool tx_frames; // set at attach: the consumer sends frames (HDLC-encoded on USB)
    uint8_t *tx_buf;
    size_t tx_cap;
} mux_port_t;

typedef enum {
    MUX_MSG_NEW_DEV,  // new_dev_cb: a USB device appeared
    MUX_MSG_GONE,     // event_cb: the open device was disconnected
    MUX_MSG_SET,      // radio_mux_set
    MUX_MSG_FORGET,   // radio_mux_forget
    MUX_MSG_RESET,    // radio_mux_reset
    MUX_MSG_NOTE_FW,  // radio_mux_note_firmware
} mux_msg_type_t;

typedef struct mux_call mux_call_t;

typedef struct {
    mux_msg_type_t type;
    union {
        mux_usb_dev_t dev;        // NEW_DEV
        const void *hdl;          // GONE: the CDC handle that went away
        mux_call_t *call;         // SET, FORGET
        struct radio_mux_chan *ch; // RESET
        struct {
            struct radio_mux_chan *ch;
            radio_mux_fw_record_t fw;
        } note;                   // NOTE_FW
    };
} mux_msg_t;

/* ---- mux.c, for the transports ---- */

// Any task or callback, never blocks: false when the queue is full or the mux is not started.
bool mux_post(const mux_msg_t *m);
// Transport callback context, rx_lock held: bytes received from a byte-stream transport (USB), handled per rx_mode.
void mux_rx_bytes(mux_port_t *p, const uint8_t *data, size_t len);

/* ---- usb_transport.c ---- */

// From the owner task (so the USB interrupt lands on MGMT_CORE): usb_host_install, the usb_lib task, the CDC driver
// with its new-device callback.
esp_err_t usb_transport_start(void);
// Opens `dev` (callbacks bound to `p`), then line coding at baud 8N1 with read-back, flow control (CP210x only) and
// DTR+RTS asserted together once. On an error the device is closed and `err` says why; `err` may also hold a warning
// (flow control unsupported, baud read back close) with ESP_OK.
esp_err_t usb_transport_open(mux_port_t *p, const mux_usb_dev_t *dev, uint32_t baud, bool flow, char *err, size_t cap);
// New line settings on the open device. A failure (or a baud read back far off) closes the device.
esp_err_t usb_transport_line(mux_port_t *p, uint32_t baud, bool flow, char *err, size_t cap);
// The single closer: takes tx_lock so no sender is inside the driver, then cdc_acm_host_close once. No-op when closed.
void usb_transport_close(mux_port_t *p);
bool usb_transport_is_open(void);
bool usb_transport_is(const void *hdl);
// Any task, with p->tx_lock held. ESP_ERR_INVALID_STATE when no device is open.
esp_err_t usb_transport_write(const uint8_t *data, size_t n);
// Whether a device with this address is still enumerated (a device that is not open sends no disconnect event).
bool usb_transport_dev_attached(uint8_t addr);
