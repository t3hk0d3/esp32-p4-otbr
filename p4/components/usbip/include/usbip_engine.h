// USB/IP session engine (spec sections 5, 6.3-6.5, 8.1). A pure, single-threaded state machine: parsed CMD_SUBMIT and
// CMD_UNLINK go in, USB transfers and endpoint commands go out through an ops seam (esp-usb on the target, a fake in the
// host tests), completions come back in and become RET_SUBMIT / RET_UNLINK replies.
//
// Threading: only the network task calls the engine (spec 8.2); there are no locks inside. The ops must not call back
// into the engine: completions are queued by the USB side and handed to usbip_engine_on_complete by the network task.
//
// Transfer ownership:
// - The engine allocates every usbip_xfer_t (inside a private wrapper) and its buffers through ops->alloc, and frees
//   them through ops->free. A transfer handed to ops->submit belongs to the USB side until it comes back through
//   usbip_engine_on_complete (exactly once per submit); inside that call the engine answers it, frees it, or submits
//   it again (after a flush).
// - native is the ops' own handle (the esp-usb transfer). The engine sets it to NULL when it creates the transfer and
//   never reads it. The ops release what it refers to before handing the completion over (or reuse it on the next
//   submit of the same transfer); the engine may free x inside usbip_engine_on_complete.
// - On session end (usbip_engine_stop) every waiting transfer and every flushed transfer already back is freed at once,
//   and the budget reads 0. A transfer still with the USB side is orphaned: it no longer counts anywhere, and when its
//   completion arrives (canceled by the flush, at any time later, also after a new session started) the engine frees
//   its buffer and the transfer and sends nothing. The engine struct and the ops must outlive such completions.
//
// Unlink of a transfer in flight on a bulk or interrupt endpoint (spec 6.5): the endpoint is halted, flushed and
// cleared at once; the RET_UNLINK goes out when the unlinked transfer comes back: -ECONNRESET and no RET_SUBMIT when
// it was canceled, or its RET_SUBMIT then RET_UNLINK 0 when it had completed before the flush took it. The endpoint's
// other canceled IN transfers are submitted again in their original order once all of them are back, ahead of its
// FIFO. A canceled OUT transfer is never submitted again (a flush reports 0 bytes even when data went out, spec 6.3):
// as collateral of any cancel (CLEAR_FEATURE(ENDPOINT_HALT), SET_INTERFACE) it is answered -ECONNRESET, actual 0.
//
// Endpoint commands: ep_halt / ep_flush / ep_clear are never called for EP0 (esp-usb refuses them). A flush completes
// every transfer in flight on the endpoint as USBIP_XS_CANCELED, asynchronously, through usbip_engine_on_complete.
//
// Buffers: a waiting transfer keeps its OUT data in alloc(n, dma = false) (PSRAM on the target). An in-flight transfer
// gets alloc(n, dma = true) of at least len bytes.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "usbip_budget.h"
#include "usbip_core.h"
#include "usbip_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

// The size of one reply header, as passed to ops->emit.
#define USBIP_ENGINE_HDR_LEN USBIP_CMD_HDR_LEN

// One USB transfer as the ops see it.
// - ep: endpoint address with the direction bit (0x81); 0 for EP0 in both directions.
// - in: data stage direction.
// - type: USB transfer type (0 control, 2 bulk, 3 interrupt).
// - flags: the URB transfer flags of the CMD_SUBMIT (USBIP_URB_ZERO_PACKET maps to USB_TRANSFER_FLAG_ZERO_PACK).
// - len: the number of bytes to transfer (esp-usb num_bytes). EP0: the 8-byte setup packet plus the data stage, with an
//   IN data stage rounded up to a multiple of 64. Bulk/interrupt IN: rounded up to the endpoint's MPS.
// - buf: len bytes or more, DMA capable; EP0 holds the setup packet first, then the OUT data.
// The completion's actual byte count is esp-usb's actual_num_bytes: for EP0 it includes the 8 setup bytes.
typedef struct {
    uint32_t seqnum;
    uint8_t ep;
    bool in;
    uint8_t type;
    uint32_t flags;
    size_t len;
    uint8_t *buf;
    void *native;
} usbip_xfer_t;

typedef struct {
    esp_err_t (*submit)(void *ctx, usbip_xfer_t *x);
    esp_err_t (*ep_halt)(void *ctx, uint8_t ep);
    esp_err_t (*ep_flush)(void *ctx, uint8_t ep);
    esp_err_t (*ep_clear)(void *ctx, uint8_t ep);
    esp_err_t (*iface_claim)(void *ctx, uint8_t iface, uint8_t alt);
    esp_err_t (*iface_release)(void *ctx, uint8_t iface);
    // Turns the root port off and on again; while it is off a pending control transfer ends.
    void (*port_power_cycle)(void *ctx);
    void *(*alloc)(void *ctx, size_t n, bool dma);
    void (*free)(void *ctx, void *p);
    // Sends one reply: the 48-byte header, then n bytes of IN data (RET_SUBMIT only).
    void (*emit)(void *ctx, const uint8_t hdr[USBIP_ENGINE_HDR_LEN], const uint8_t *data, size_t n);
    // Asks the network side to close the session's connection. The engine runs usbip_engine_stop itself right after.
    void (*end_session)(void *ctx, const char *why);
    int64_t (*now_ms)(void *ctx);
} usbip_engine_ops_t;

typedef struct {
    uint32_t submits;        // CMD_SUBMIT received
    uint32_t completed;      // RET_SUBMIT sent with status 0
    uint32_t errors;         // RET_SUBMIT sent with a non-zero status
    uint32_t unlinks;        // CMD_UNLINK received
    uint32_t overload_ends;  // sessions ended because waiting data exceeded USBIP_WAIT_BYTES
    uint64_t bytes_in;       // IN data returned in RET_SUBMIT
    uint64_t bytes_out;      // OUT data handed to the device (a transfer submitted again is not counted twice)
} usbip_counters_t;

// Private state. Callers allocate the struct; only the functions below touch it.
typedef struct usbip_engine_xfer usbip_engine_xfer_t;

typedef struct {
    bool claimed;              // on a claimed interface at its current alt
    uint8_t addr, type, iface;
    uint16_t mps;
    uint16_t canceling;        // flushed transfers not back yet
    usbip_engine_xfer_t *inflight, *inflight_tail;  // in submission order
    usbip_engine_xfer_t *wait, *wait_tail;          // the endpoint's FIFO
} usbip_engine_slot_t;

typedef struct {
    uint8_t num, alt;
    bool claimed;
} usbip_engine_iface_t;

typedef struct {
    const usbip_engine_ops_t *ops;
    void *ctx;
    bool active;
    uint32_t devid;
    uint8_t config_value;
    const uint8_t *cfg;
    size_t cfg_len;
    uint8_t num_ifaces;
    usbip_engine_iface_t ifaces[USBIP_IFACES_MAX];
    usbip_engine_slot_t slots[USBIP_EP_MAX + 1];  // 0 = EP0, 1..15 = non-control endpoints (budget indices)
    usbip_budget_t budget;
    usbip_counters_t counters;
} usbip_engine_t;

// Starts a session for dev: claims every interface of cfg at alternate setting 0. cfg (the active configuration
// descriptor, already validated by usbip_core_dev_record) must stay valid until the session ends. Keeps the counters
// and memory peaks: they count since the struct was zeroed (boot, spec 9). On a claim failure the interfaces already claimed are released and the error is returned.
// e must be zeroed before its first start (static storage is); ESP_ERR_INVALID_STATE while a session runs.
esp_err_t usbip_engine_start(usbip_engine_t *e, const usbip_engine_ops_t *ops, void *ctx, const usbip_dev_rec_t *dev,
                             const uint8_t *cfg, size_t cfg_len);

// Handles one parsed command; out_data holds the OUT data of a CMD_SUBMIT (submit.length bytes) or is NULL. Ignored
// when no session runs.
void usbip_engine_on_cmd(usbip_engine_t *e, const usbip_cmd_t *c, const uint8_t *out_data);

// A transfer handed to ops->submit came back. actual is esp-usb's actual_num_bytes (EP0: setup included).
void usbip_engine_on_complete(usbip_engine_t *e, usbip_xfer_t *x, usbip_xfer_status_t st, size_t actual);

// Checks the EP0 unlink deadline (USBIP_EP0_UNLINK_MS); ends the session when it passed. Call it periodically.
void usbip_engine_tick(usbip_engine_t *e);

// Ends the session (spec section 5): halts, flushes and clears every non-control endpoint; turns the root port off
// first when a control transfer is still pending; releases the interfaces; power-cycles the root port (once); frees
// the session's buffers. Does not call ops->end_session. Idempotent.
void usbip_engine_stop(usbip_engine_t *e, const char *why);

bool usbip_engine_active(const usbip_engine_t *e);
void usbip_engine_counters(const usbip_engine_t *e, usbip_counters_t *out);
void usbip_engine_mem(const usbip_engine_t *e, usbip_mem_stats_t *out);

#ifdef __cplusplus
}
#endif
