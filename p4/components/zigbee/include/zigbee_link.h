#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Outbound queue depth, in frames. */
#define ZIGBEE_LINK_QUEUE_DEPTH 32
/** The largest binary frame accepted from a client; a bigger one closes the socket. */
#define ZIGBEE_LINK_MAX_FRAME 2048

/**
 * The rules of the /zigbee WebSocket as plain state: which socket is the client, the kept hello, and the outbound
 * queue's occupancy. The link owns no frames and no sockets. Not thread-safe: the caller serialises access.
 */
typedef struct {
    size_t depth;
    size_t queued;
    int client_fd;     // -1 with no client
    bool have_hello;   // the stack's first outbound frame was kept by the caller
    bool hello_due;    // the current client has not been sent the hello yet
    bool refused;      // a host_send was refused since the queue last had room
    uint32_t generation; // bumped on every connect and disconnect
} zigbee_link_t;

/** Resets the link; `queue_depth` frames fit in the outbound queue. */
void zigbee_link_init(zigbee_link_t *link, size_t queue_depth);

/**
 * The stack wants to send a frame of `len` bytes. true: the caller enqueues it (or drops it when there is no client:
 * then nothing is counted and true is returned too). false: the queue is full; the stack must retry after
 * zigbee_link_drained() reports true.
 */
bool zigbee_link_host_send(zigbee_link_t *link, size_t len);

/** The sender took one queued frame off the queue (sent, or discarded because zigbee_link_frame_current() said stale). */
void zigbee_link_sent(zigbee_link_t *link);

/**
 * Offers an outbound frame as the hello. true the first time with a non-empty frame: the caller keeps a copy.
 * false afterwards.
 */
bool zigbee_link_take_hello(zigbee_link_t *link, const uint8_t *frame, size_t len);

/**
 * `fd` becomes the client. Returns the previous client's fd (to close), or -1 if there was none or it is the same
 * socket. When a hello is kept, zigbee_link_take_send_hello() returns true next, before any other frame.
 */
int zigbee_link_connect(zigbee_link_t *link, int fd);

/** The client `fd` went away. true if it was the current client; a stale fd changes nothing. */
bool zigbee_link_disconnect(zigbee_link_t *link, int fd);

/** true once per connect when the kept hello must be sent to the client before anything else. */
bool zigbee_link_take_send_hello(zigbee_link_t *link);

/**
 * The current generation. It changes on every connect (a reused fd included) and every disconnect. The sender tags
 * each frame it queues with the generation read when it queued the frame, and the hello it sends to a new client.
 */
uint32_t zigbee_link_generation(const zigbee_link_t *link);

/**
 * false: the frame was queued under an older generation, so its client is gone or replaced; the sender discards it
 * (and still calls zigbee_link_sent() so it frees its slot). true: send it.
 */
bool zigbee_link_frame_current(const zigbee_link_t *link, uint32_t generation);

/** The client of the link, or -1. */
int zigbee_link_client(const zigbee_link_t *link);

/**
 * Whether a frame received on socket `fd` goes to the stack. false (drop): not the current client (late frames from a
 * replaced client), not binary, or empty or over ZIGBEE_LINK_MAX_FRAME bytes. The caller closes the socket when
 * zigbee_link_must_close() is true for the length.
 */
bool zigbee_link_accept_inbound(const zigbee_link_t *link, int fd, size_t len, bool binary);

/** true when a frame of `len` bytes is over the maximum, so its socket must be closed. */
bool zigbee_link_must_close(size_t len);

/** true once when a refused host_send may be retried: the queue has room again. */
bool zigbee_link_drained(zigbee_link_t *link);

#ifdef __cplusplus
}
#endif
