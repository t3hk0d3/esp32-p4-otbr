#include "fake_io.h"

#include <string.h>

#include "unity.h"

fake_io_state_t g_io;

static fake_io_call_t *record(fake_io_op_t op)
{
    TEST_ASSERT_LESS_THAN_MESSAGE(FAKE_IO_MAX_CALLS, g_io.call_count, "fake io call log full");
    fake_io_call_t *c = &g_io.calls[g_io.call_count++];

    memset(c, 0, sizeof(*c));
    c->op = op;
    c->h = -1;
    return c;
}

static void copy_data(fake_io_call_t *c, const uint8_t *buf, size_t len)
{
    c->len = len;
    memcpy(c->data, buf, len < FAKE_IO_MAX_DATA ? len : FAKE_IO_MAX_DATA);
}

static int io_tcp_connect(void *ctx, const uint8_t addr[16], const char **why)
{
    fake_io_call_t *c = record(FIO_CONNECT);

    c->has_addr = true;
    memcpy(c->addr, addr, 16);
    if (g_io.connect_fail) {
        *why = g_io.connect_fail;
        return -1;
    }
    c->h = g_io.next_handle++;
    return c->h;
}

static int io_tcp_send(void *ctx, int h, const uint8_t *buf, size_t len)
{
    fake_io_call_t *c = record(FIO_SEND);

    c->h = h;
    copy_data(c, buf, len);
    return h == g_io.send_fail_h ? -1 : 0;
}

static void io_tcp_close(void *ctx, int h)
{
    record(FIO_CLOSE)->h = h;
}

static bool io_bgp_listen(void *ctx, bool on)
{
    record(FIO_LISTEN)->on = on;
    return !on || g_io.listen_ok;
}

static bool io_ripng_socket(void *ctx, bool on)
{
    record(FIO_RIPNG_SOCKET)->on = on;
    return !on || g_io.ripng_ok;
}

static int io_ripng_send(void *ctx, const uint8_t *dst, uint16_t port, const uint8_t *buf, size_t len)
{
    fake_io_call_t *c = record(FIO_RIPNG_SEND);

    if (dst) {
        c->has_addr = true;
        memcpy(c->addr, dst, 16);
        c->port = port;
    }
    copy_data(c, buf, len);
    return 0;
}

static bool io_publisher_apply(void *ctx, const routing_settings_t *s)
{
    record(FIO_PUBLISHER_APPLY)->settings = *s;
    return g_io.publisher_ok;
}

static uint32_t io_rand32(void)
{
    return g_io.rand_value;
}

const routing_engine_io_t fake_io = {
    .ctx = NULL,
    .tcp_connect = io_tcp_connect,
    .tcp_send = io_tcp_send,
    .tcp_close = io_tcp_close,
    .bgp_listen = io_bgp_listen,
    .ripng_socket = io_ripng_socket,
    .ripng_send = io_ripng_send,
    .publisher_apply = io_publisher_apply,
    .rand32 = io_rand32,
};

void fake_io_reset(void)
{
    memset(&g_io, 0, sizeof(g_io));
    g_io.next_handle = 100;
    g_io.send_fail_h = -1;
    g_io.listen_ok = true;
    g_io.ripng_ok = true;
    g_io.publisher_ok = true;
}

void fake_io_clear_calls(void)
{
    g_io.call_count = 0;
}

int fake_io_count(fake_io_op_t op)
{
    int n = 0;

    for (int i = 0; i < g_io.call_count; i++) {
        n += g_io.calls[i].op == op;
    }
    return n;
}

int fake_io_index(fake_io_op_t op, int nth)
{
    for (int i = 0; i < g_io.call_count; i++) {
        if (g_io.calls[i].op == op && nth-- == 0) {
            return i;
        }
    }
    return -1;
}

const fake_io_call_t *fake_io_find(fake_io_op_t op, int nth)
{
    int i = fake_io_index(op, nth);

    return i < 0 ? NULL : &g_io.calls[i];
}

int fake_io_sends_on(int h)
{
    int n = 0;

    for (int i = 0; i < g_io.call_count; i++) {
        n += g_io.calls[i].op == FIO_SEND && g_io.calls[i].h == h;
    }
    return n;
}

const fake_io_call_t *fake_io_send_on(int h, int nth)
{
    for (int i = 0; i < g_io.call_count; i++) {
        if (g_io.calls[i].op == FIO_SEND && g_io.calls[i].h == h && nth-- == 0) {
            return &g_io.calls[i];
        }
    }
    return NULL;
}

int fake_io_bgp_sends(int h, uint8_t type)
{
    int n = 0;

    for (int i = 0; i < g_io.call_count; i++) {
        const fake_io_call_t *c = &g_io.calls[i];
        n += c->op == FIO_SEND && c->h == h && c->len >= ROUTING_BGP_HDR_LEN && c->data[18] == type;
    }
    return n;
}

int fake_io_closes(int h)
{
    int n = 0;

    for (int i = 0; i < g_io.call_count; i++) {
        n += g_io.calls[i].op == FIO_CLOSE && g_io.calls[i].h == h;
    }
    return n;
}
