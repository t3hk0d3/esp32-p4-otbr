// Shared state of the zigbee component's board files (zigbee.c, platform.c, ws.c, status.c).
//
// Tasks:
//  - the zigbee task (ZIGBEE_TASK_CORE, ZIGBEE_TASK_PRIORITY) makes every ziggurat_* call and every zigbee_spinel_*
//    and zigbee_link_* call. It never calls lwIP.
//  - the sender task (zigbee_tx, MGMT_CORE) takes frames off the outbound queue and has the HTTP server task send them.
//  - the HTTP server task runs the WebSocket handlers; the esp_timer task runs the timer and tick callbacks; the mux's
//    owner task delivers the orchestrator's inputs (kept as the latest, then a KICK). These only post to the zigbee
//    task.
//
// The zigbee task's queue (`q`) carries radio_mux_ev_t items: the mux's events (it is the queue the mux is bound to),
// plus ZIGBEE_EV_ATTACH (posted by the zigbee task when it binds the queue) and ZIGBEE_EV_KICK (at most one: "the mailbox has work"). Everything else
// (inbound frames, connects and disconnects, timer, wake, tick, frames the sender took) waits in the mailbox, so that a
// property request's wait (zigbee.c op_wait) can take the mux's events out of `q` while the mailbox keeps its order,
// and so that `q` holds only what the mux sized it for (RADIO_MUX_POOL_SLOTS frames + RESET + GONE).
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_http_server.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "radio_mux.h"
#include "zigbee.h"
#include "zigbee_heap_stats.h"
#include "zigbee_kick.h"
#include "zigbee_link.h"
#include "zigbee_satellite.h"
#include "zigbee_spinel.h"

// radio_mux_ev_t types of our own in the zigbee task's queue (outside radio_mux_ev_type_t's values).
#define ZIGBEE_EV_ATTACH ((radio_mux_ev_type_t)100)
#define ZIGBEE_EV_KICK ((radio_mux_ev_type_t)101)

// The queue the mux is bound to: its frames (one per pool slot), RESET and GONE, plus our ATTACH and KICK, plus room
// for an attach / GONE / attach sequence the zigbee task has not taken yet.
#define ZIGBEE_EVENT_DEPTH (RADIO_MUX_POOL_SLOTS + 8)

// Mailbox: messages in order, plus flags. Frames may fill it up to ZIGBEE_MAILBOX_DEPTH - ZIGBEE_MAILBOX_RESERVE;
// connects and disconnects use the reserve, so a flood of frames never loses a disconnect.
#define ZIGBEE_MAILBOX_DEPTH 48
#define ZIGBEE_MAILBOX_RESERVE 16
#define ZIGBEE_FLAG_TIMER (1u << 0)
#define ZIGBEE_FLAG_WAKE (1u << 1)
#define ZIGBEE_FLAG_TICK (1u << 2)
#define ZIGBEE_FLAG_LOGLEVEL (1u << 3)

// Outbound queue to the sender: ZIGBEE_LINK_QUEUE_DEPTH counted frames plus hellos and closes.
#define ZIGBEE_OUT_DEPTH (ZIGBEE_LINK_QUEUE_DEPTH + 8)

typedef struct {
    size_t len;
    uint8_t data[];
} zigbee_frame_t;

typedef enum {
    ZIGBEE_MSG_CONNECT,    // a WebSocket client finished its handshake; frame: its address (text)
    ZIGBEE_MSG_DISCONNECT, // the socket of a session closed
    ZIGBEE_MSG_FRAME,      // a binary frame from a socket
    ZIGBEE_MSG_ZLOE_DATAGRAM, // a datagram from the backbone (zloe.c)
    ZIGBEE_MSG_ZLOE_PEER,     // a node discovery resolved (zloe.c)
} zigbee_msg_kind_t;

typedef struct {
    zigbee_msg_kind_t kind;
    int fd;
    uint32_t sid; // the session (ws.c); never 0
    zigbee_frame_t *frame;
} zigbee_msg_t;

typedef enum {
    ZIGBEE_OUT_FRAME, // send `frame` to (fd, sid) if `gen` is still the link's generation
    ZIGBEE_OUT_CLOSE, // close the socket (fd, sid): a newer client took over
} zigbee_out_kind_t;

typedef struct {
    zigbee_out_kind_t kind;
    bool counted; // a host_send frame: the sender reports it taken (zigbee_link_sent); the hello is not counted
    int fd;
    uint32_t sid;
    uint32_t gen;
    zigbee_frame_t *frame;
} zigbee_out_t;

typedef struct {
    // Made once, by the first bind (the mux's owner task), and never freed.
    QueueHandle_t q;
    QueueHandle_t out;
    SemaphoreHandle_t job_done; // the sender's server-task work finished
    esp_timer_handle_t timer;   // ziggurat's one-shot timer
    esp_timer_handle_t tick;    // periodic: the spinel client's deadlines
    zigbee_spinel_t *spinel;
    zigbee_satellite_t satellite; // the zigbee task's
    atomic_bool up; // all of the above exist and both tasks run

    _Atomic(radio_mux_chan_t *) ch; // the attached channel (kept after GONE: slots are still released through it)
    _Atomic(httpd_handle_t) server;
    atomic_bool started;      // ziggurat_start ran (or is running): the WebSocket accepts clients
    zigbee_kick_t kick;       // at most one KICK in `q` (zigbee_kick.h)
    portMUX_TYPE in_lock;     // the orchestrator's latest input (on_input), taken by the zigbee task
    radio_mux_input_t in;
    atomic_uint sent;         // counted frames the sender took off `out` since the zigbee task last looked
    atomic_uint gen;          // the link's generation, published by the zigbee task (the sender drops older frames)
    atomic_int client_fd;     // the link's client, published by the zigbee task (the sender's LRU refresh)
    atomic_uint client_sid;

    // Mailbox, under mb_lock.
    portMUX_TYPE mb_lock;
    uint32_t mb_flags;
    zigbee_msg_t mb[ZIGBEE_MAILBOX_DEPTH];
    size_t mb_head, mb_count;

    // The zigbee task's own.
    zigbee_link_t link;
    uint32_t link_sid;           // the session of the link's client (0: none)
    char link_addr[ZIGBEE_ADDRESS_MAX];
    zigbee_frame_t *hello;       // the stack's first outbound frame
    uint64_t timer_deadline;     // UINT64_MAX: not armed
    bool start_due;              // the first ready came: ziggurat_start runs at the top of the loop
    bool start_ran;
    uint8_t scan_channel;        // the channel of the last energy scan asked for (its failure is logged with it)
    uint32_t in_seen;            // the seq of the last input handled (0: none)
    bool kick_swallowed;         // op_wait took a KICK out of `q`: the loop looks at the mailbox without blocking
    bool mailbox_more;           // the last mailbox round left messages behind
} zigbee_t;

extern zigbee_t g_zigbee;

// Status (status.c), under g_zigbee_status_lock. g_zigbee_heap is the staticlib's heap accounting (platform.c).
extern portMUX_TYPE g_zigbee_status_lock;
extern zigbee_status_t g_zigbee_status;
// When the status's moments happened, as esp_timer_get_time() (-1: never), under g_zigbee_status_lock. zigbee_get_status
// turns them into dates (timesync_epoch_at), so a moment before the clock was first set still gets its real date.
typedef struct {
    int64_t client_since_us;
    int64_t last_client_left_us;
    int64_t started_us;
} zigbee_status_times_t;
extern zigbee_status_times_t g_zigbee_status_times;
extern zigbee_heap_stats_t g_zigbee_heap;
// One short statement on the status under its lock (several: take the lock around them).
#define ZIGBEE_STATUS(...)                           \
    do {                                             \
        taskENTER_CRITICAL(&g_zigbee_status_lock);   \
        __VA_ARGS__;                                 \
        taskEXIT_CRITICAL(&g_zigbee_status_lock);    \
    } while (0)

/* ---- zigbee.c ---- */

// Any task: queue a message (control: may use the reserve) / set flags, then kick the zigbee task. false: full.
bool zigbee_post_msg(const zigbee_msg_t *m, bool control);
void zigbee_post_flags(uint32_t flags);
void zigbee_kick(void);
// A frame buffer of `len` bytes (PSRAM first), and one holding a copy of `data`. NULL when out of memory.
zigbee_frame_t *zigbee_frame_alloc(size_t len);
zigbee_frame_t *zigbee_frame_copy(const uint8_t *data, size_t len);
void zigbee_frame_free(zigbee_frame_t *f);
// Zigbee task: queue a frame for the link's client. false (frame freed) when the outbound queue is full.
bool zigbee_out_post(const zigbee_out_t *item);

/* ---- platform.c ---- */

// At registration (every boot): takes the panic message the previous boot recorded, for ziggurat_platform_last_reset.
void zigbee_platform_boot(void);
// The esp_timer callback of ziggurat's timer (esp_timer task).
void zigbee_platform_timer_cb(void *arg);
// Zigbee task: the timer flag came; calls ziggurat_timer_fired unless the timer was re-armed for later or cancelled.
void zigbee_platform_timer_due(void);

/* ---- ws.c ---- */

esp_err_t zigbee_ws_start_sender(void);

/* ---- status.c ---- */

void zigbee_status_set_state(zigbee_state_t state, const char *reason);
// The channel id (attach): zigbee_channel() answers it from then on.
void zigbee_status_note_channel(const char *id);

/* ---- zloe.c ---- */

extern const zigbee_satellite_ops_t g_zigbee_satellite_ops;
// The first bind: the task that owns the backbone's PCB and mDNS.
esp_err_t zigbee_zloe_start(void);
// Zigbee task: a ZIGBEE_MSG_ZLOE_* message.
void zigbee_zloe_on_msg(const zigbee_msg_t *m);
// Zigbee task, each tick: the satellite's state and peers into the status.
void zigbee_zloe_publish_status(void);

/* ---- zlog.c ---- */

// app_main (zigbee_register): follows the log level of the tag "ziggurat". Not before logging_init.
esp_err_t zigbee_zlog_init(void);
// The first bind: the task that prints ziggurat's lines.
esp_err_t zigbee_zlog_start_drain(void);
// Zigbee task: gives ziggurat the stored level (after ziggurat_start, and on ZIGBEE_FLAG_LOGLEVEL).
void zigbee_zlog_apply(void);
