#include <stdlib.h>
#include <string.h>

#include "render.h"
#include "sink.h"

esp_err_t sink_flush(void *ctx, const char *data, size_t len)
{
    sink_t *s = ctx;

    s->flushes++;
    if (s->fail_at && s->flushes == s->fail_at) {
        return ESP_FAIL;
    }
    s->data = realloc(s->data, s->len + len + 1);
    memcpy(s->data + s->len, data, len);
    s->len += len;
    s->data[s->len] = '\0';
    return ESP_OK;
}

void sink_free(sink_t *s)
{
    free(s->data);
    memset(s, 0, sizeof(*s));
}

void sample_input(sample_input_t *s)
{
    memset(s, 0, sizeof(*s));
    s->device = (metrics_device_t) {
        .mode = 1, .version = "v0.2.0", .slot = 1, .rcp_version = "rcp-1.2", .rcp_compatible = true, .storage_ok = true,
        .sniffer_active = false, .sniffer_frames = 7, .sniffer_drops = 2, .eth_up = true, .trial_pending = false,
    };
    s->diag.uptime_s = 3600;
    s->diag.reset_reason = "power-on";
    for (int c = 0; c < DIAG_CORES; c++) {
        s->diag.cores[c].wall_us = 3600000000ull;
        s->diag.cores[c].idle_us = 3000000000ull + c;
    }
    const char *names[] = {"IDLE0", "IDLE1", "ot_main", "httpd", "worker", "worker"};

    s->diag.task_count = sizeof(names) / sizeof(names[0]);
    for (size_t i = 0; i < s->diag.task_count; i++) {
        strncpy(s->diag.tasks[i].name, names[i], DIAG_NAME_MAX - 1);
        s->diag.tasks[i].cpu_us = 1000000ull * (i + 1) + 1;
        s->diag.tasks[i].stack_free_min = 512 + (uint32_t)i;
    }
    s->diag.untracked = 0;
    s->diag.internal.free = 200000;
    s->diag.internal.min_free = 150000;
    s->diag.internal.largest_free_block = 100000;
    s->diag.psram.free = 30000000;
    s->diag.psram.min_free = 29000000;
    s->diag.psram.largest_free_block = 28000000;
    s->diag.chip_temperature_valid = true;
    s->diag.chip_temperature_c = 47.25f;
    s->diag.lock.acquisitions = 12345;
    s->diag.lock.hold_us_total = 2500000;
    s->diag.lock.wait_us_total = 1500;
    s->diag.lock.max_hold_us = 40000;
    s->diag.lock.max_wait_us = 9000;
    s->diag.lock.max_hold_60s_us = 1200;
    s->diag.lock.max_wait_60s_us = 300;

    metrics_thread_t *t = &s->collector.thread;

    t->role = 4;
    for (int i = 0; i < METRICS_ROLE_COUNT; i++) {
        t->role_entered[i] = (uint32_t)i + 1;
    }
    t->partition_changes = 2;
    t->attach_attempts = 3;
    t->routers = 4;
    t->children = 5;
    t->neighbors = 6;
    t->mac.tx_total = 1000;
    t->mac.tx_retry = 10;
    t->mac.tx_err_cca = 1;
    t->mac.tx_err_abort = 2;
    t->mac.tx_err_busy_channel = 3;
    t->mac.rx_total = 2000;
    t->mac.rx_duplicated = 4;
    t->mac.rx_err_fcs = 5;
    t->mac.rx_err_sec = 6;
    t->mac.rx_err_no_frame = 7;
    t->mac.rx_err_unknown_neighbor = 8;
    t->mac.rx_err_invalid_src = 9;
    t->mac.rx_err_other = 10;
    t->ip6.tx_success = 500;
    t->ip6.tx_failure = 1;
    t->ip6.rx_success = 600;
    t->ip6.rx_failure = 2;
    t->br_state = 3;
    t->pd_state = 2;
    for (int d = 0; d < 2; d++) {
        for (int c = 0; c < 2; c++) {
            t->br_packets[d][c] = 100 + d * 2 + c;
            t->br_bytes[d][c] = 10000000000ull + d * 2 + c;
        }
    }
    for (int i = 0; i < 3; i++) {
        t->ra[i] = 20 + i;
        t->rs[i] = 30 + i;
    }
    t->srp_state = 1;
    t->srp_hosts = 3;
    t->srp_services = 4;
    t->trel.built = true;
    t->trel.enabled = true;
    t->trel.active = true;
    t->trel.peers = 2;
    t->trel.peers_other = 1;
    t->trel.tx_packets = 18204;
    t->trel.tx_bytes = 5000000000ull;
    t->trel.tx_failures = 3;
    t->trel.rx_packets = 17951;
    t->trel.rx_bytes = 4000000000ull;
    s->collector.started = true;
    s->collector.ever_collected = true;
    s->collector.valid = true;
    s->collector.age_ms = 4200;
    s->collector.failures = 1;

    static const uint8_t peers[ROUTING_MAX_PEERS][16] = {
        {0x20, 0x01, 0x0d, 0xb8, [15] = 0x01},
        {0xfe, 0x80, [8] = 0x02, 0x11, 0x22, 0xff, 0xfe, 0x33, 0x44, 0x55},
    };
    routing_metrics_view_t *r = &s->routing;

    r->method = ROUTING_METHOD_BGP;
    r->announced_count = 2;
    r->omr_custom = true;
    r->peer_count = ROUTING_MAX_PEERS;
    for (int i = 0; i < ROUTING_MAX_PEERS; i++) {
        memcpy(r->peers[i].addr, peers[i], 16);
        r->peers[i].state = i == 0 ? ROUTING_BGP_ESTABLISHED : ROUTING_BGP_ACTIVE;
        r->peers[i].uptime_s = i == 0 ? 7200 : 0;
        r->peers[i].received_dropped = 40 + (uint32_t)i;
        r->peers[i].resets = 3 + (uint32_t)i;
    }
    r->ripng_updates_sent = 120;
    r->ripng_requests_answered = 5;
    r->ripng_responses_dropped = 9;
    s->in = (metrics_input_t) {
        .device = &s->device, .diag = &s->diag, .collector = &s->collector, .routing = &s->routing
    };
}

char *render_to_string(const metrics_input_t *in, size_t cap)
{
    char *buf = malloc(cap);
    sink_t sink = {0};
    metrics_writer_t w;

    metrics_writer_init(&w, buf, cap, sink_flush, &sink);
    metrics_render(&w, in);
    metrics_writer_finish(&w);
    free(buf);
    if (!sink.data) {
        sink.data = calloc(1, 1);
    }
    return sink.data;
}
