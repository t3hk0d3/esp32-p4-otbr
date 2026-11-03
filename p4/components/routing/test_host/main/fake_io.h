#pragma once

// A routing_engine_io_t that records every call in order, with the bytes sent, and returns configurable results. One
// global instance; fake_io_reset() before each test. Recording past FAKE_IO_MAX_CALLS fails the test.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "engine.h"

#define FAKE_IO_MAX_CALLS 96
#define FAKE_IO_MAX_DATA 256

typedef enum {
    FIO_CONNECT,
    FIO_SEND,
    FIO_CLOSE,
    FIO_LISTEN,
    FIO_RIPNG_SOCKET,
    FIO_RIPNG_SEND,
    FIO_PUBLISHER_APPLY,
} fake_io_op_t;

typedef struct {
    fake_io_op_t op;
    int h;            /**< connect (the handle returned), send, close */
    bool on;          /**< listen, ripng_socket */
    bool has_addr;    /**< connect, ripng_send unicast */
    uint8_t addr[16];
    uint16_t port;    /**< ripng_send unicast */
    size_t len;       /**< send, ripng_send */
    uint8_t data[FAKE_IO_MAX_DATA];
    routing_settings_t settings; /**< publisher_apply */
} fake_io_call_t;

typedef struct {
    fake_io_call_t calls[FAKE_IO_MAX_CALLS];
    int call_count;
    int next_handle;          /**< handle the next successful connect returns (then incremented) */
    const char *connect_fail; /**< non-NULL: connect fails at once with this text */
    int send_fail_h;          /**< sends on this handle fail (-1: none) */
    bool listen_ok;
    bool ripng_ok;
    bool publisher_ok;
    uint32_t rand_value;
} fake_io_state_t;

extern fake_io_state_t g_io;
extern const routing_engine_io_t fake_io;

void fake_io_reset(void);
void fake_io_clear_calls(void);
int fake_io_count(fake_io_op_t op);
/** The `nth` (0-based) call of `op`, NULL when there is none. */
const fake_io_call_t *fake_io_find(fake_io_op_t op, int nth);
/** Index in g_io.calls of the `nth` call of `op`, -1 when there is none. */
int fake_io_index(fake_io_op_t op, int nth);
/** Sends on handle `h`. */
int fake_io_sends_on(int h);
/** The `nth` send on handle `h`, NULL when there is none. */
const fake_io_call_t *fake_io_send_on(int h, int nth);
/** Sends on `h` whose BGP message type is `type`. */
int fake_io_bgp_sends(int h, uint8_t type);
/** Closes of handle `h`. */
int fake_io_closes(int h);
