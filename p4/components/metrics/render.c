#include "render.h"

#include <string.h>

#include "diag.h"

static const char *const mode_names[] = {"STARTING", "BR_RUNNING", "SNIFFER", "RADIO_FAULT"};
static const char *const role_names[METRICS_ROLE_COUNT] = {"disabled", "detached", "child", "router", "leader"};
static const char *const br_state_names[] = {"uninitialized", "disabled", "stopped", "running"};
static const char *const pd_state_names[] = {"disabled", "stopped", "running", "idle"};
static const char *const srp_state_names[] = {"disabled", "running", "stopped"};
static const char *const directions[] = {"inbound", "outbound"};
static const char *const casts[] = {"unicast", "multicast"};
static const char *const events[] = {"rx", "tx_success", "tx_failure"};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

// ---- sample syntax ----

static void family(metrics_writer_t *w, const char *name, const char *type, const char *help)
{
    metrics_writer_raw(w, "# HELP ");
    metrics_writer_raw(w, name);
    metrics_writer_raw(w, " ");
    metrics_writer_raw(w, help);
    metrics_writer_raw(w, "\n# TYPE ");
    metrics_writer_raw(w, name);
    metrics_writer_raw(w, " ");
    metrics_writer_raw(w, type);
    metrics_writer_raw(w, "\n");
}

// A sample is begin, any number of label calls, then one end call.
typedef struct {
    metrics_writer_t *w;
    bool labelled;
} sample_t;

static sample_t begin(metrics_writer_t *w, const char *name)
{
    metrics_writer_raw(w, name);
    return (sample_t) {
        .w = w
    };
}

static void label(sample_t *s, const char *key, const char *value)
{
    metrics_writer_raw(s->w, s->labelled ? "," : "{");
    metrics_writer_raw(s->w, key);
    metrics_writer_raw(s->w, "=\"");
    metrics_writer_label_value(s->w, value);
    metrics_writer_raw(s->w, "\"");
    s->labelled = true;
}

static void close_labels(sample_t *s)
{
    metrics_writer_raw(s->w, s->labelled ? "} " : " ");
}

static void end_u64(sample_t *s, uint64_t value)
{
    close_labels(s);
    metrics_writer_u64(s->w, value);
    metrics_writer_raw(s->w, "\n");
}

static void end_usec(sample_t *s, uint64_t usec)
{
    close_labels(s);
    metrics_writer_usec(s->w, usec);
    metrics_writer_raw(s->w, "\n");
}

// A family with one unlabelled sample.
static void single(metrics_writer_t *w, const char *name, const char *type, const char *help, uint64_t value)
{
    family(w, name, type, help);
    sample_t s = begin(w, name);

    end_u64(&s, value);
}

// One series per state, 1 for `current`, 0 for the others.
static void state_set(metrics_writer_t *w, const char *name, const char *help, const char *key, const char *const *names,
                      size_t count, int current)
{
    family(w, name, "gauge", help);
    for (size_t i = 0; i < count; i++) {
        sample_t s = begin(w, name);

        label(&s, key, names[i]);
        end_u64(&s, (int)i == current ? 1 : 0);
    }
}

static void u32_text(char *out, size_t cap, uint32_t value)
{
    char digits[11];
    size_t n = 0;

    do {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value > 0 && n < sizeof(digits));
    size_t i = 0;

    while (n > 0 && i + 1 < cap) {
        out[i++] = digits[--n];
    }
    out[i] = '\0';
}

// ---- device ----

static void render_info(metrics_writer_t *w, const metrics_device_t *d)
{
    char slot[12] = "";

    if (d->slot >= 0) {
        u32_text(slot, sizeof(slot), (uint32_t)d->slot);
    }
    family(w, "br_info", "gauge", "Firmware and radio versions (value 1).");
    sample_t s = begin(w, "br_info");

    label(&s, "version", d->version ? d->version : "");
    label(&s, "slot", slot);
    label(&s, "rcp_version", d->rcp_version ? d->rcp_version : "");
    label(&s, "rcp_compatible", d->rcp_compatible ? "true" : "false");
    end_u64(&s, 1);
}

static void render_cpu(metrics_writer_t *w, const diag_metrics_view_t *g)
{
    static const char *const core_names[DIAG_METRICS_CORES] = {"0", "1"};

    family(w, "br_cpu_seconds_total", "counter", "CPU time per core, busy and idle.");
    for (int c = 0; c < DIAG_METRICS_CORES; c++) {
        const uint64_t idle = g->cores[c].idle_us < g->cores[c].wall_us ? g->cores[c].idle_us : g->cores[c].wall_us;
        sample_t busy = begin(w, "br_cpu_seconds_total");

        label(&busy, "core", core_names[c]);
        label(&busy, "mode", "busy");
        end_usec(&busy, g->cores[c].wall_us - idle);
        sample_t idle_sample = begin(w, "br_cpu_seconds_total");

        label(&idle_sample, "core", core_names[c]);
        label(&idle_sample, "mode", "idle");
        end_usec(&idle_sample, idle);
    }
}

// Task names are not unique in FreeRTOS: the second task of a name is "<name>#2", the third "<name>#3".
static void task_label(const diag_metrics_view_t *g, size_t i, char *out, size_t cap)
{
    uint32_t same = 0;

    for (size_t k = 0; k < i; k++) {
        if (strncmp(g->tasks[k].name, g->tasks[i].name, DIAG_METRICS_NAME_MAX) == 0) {
            same++;
        }
    }
    strncpy(out, g->tasks[i].name, DIAG_METRICS_NAME_MAX);
    out[DIAG_METRICS_NAME_MAX] = '\0';
    if (same > 0) {
        const size_t len = strlen(out);

        out[len] = '#';
        u32_text(out + len + 1, cap - len - 1, same + 1);
    }
}

#define TASK_LABEL_MAX (DIAG_METRICS_NAME_MAX + 12)

static void render_tasks(metrics_writer_t *w, const diag_metrics_view_t *g)
{
    char name[TASK_LABEL_MAX];

    if (g->task_count == 0) {
        return;
    }
    family(w, "br_task_cpu_seconds_total", "counter", "CPU time per task since it was first seen.");
    for (size_t i = 0; i < g->task_count; i++) {
        task_label(g, i, name, sizeof(name));
        sample_t s = begin(w, "br_task_cpu_seconds_total");

        label(&s, "task", name);
        end_usec(&s, g->tasks[i].cpu_us);
    }
    family(w, "br_task_stack_free_min_bytes", "gauge", "Least free stack per task since it started.");
    for (size_t i = 0; i < g->task_count; i++) {
        task_label(g, i, name, sizeof(name));
        sample_t s = begin(w, "br_task_stack_free_min_bytes");

        label(&s, "task", name);
        end_u64(&s, g->tasks[i].stack_free_min);
    }
}

static void heap_family(metrics_writer_t *w, const char *name, const char *help, uint32_t internal, uint32_t psram)
{
    family(w, name, "gauge", help);
    sample_t a = begin(w, name);

    label(&a, "region", "internal");
    end_u64(&a, internal);
    sample_t b = begin(w, name);

    label(&b, "region", "psram");
    end_u64(&b, psram);
}

static void window_family(metrics_writer_t *w, const char *name, const char *help, uint32_t boot_us, uint32_t last_60s_us)
{
    family(w, name, "gauge", help);
    sample_t a = begin(w, name);

    label(&a, "window", "boot");
    end_usec(&a, boot_us);
    sample_t b = begin(w, name);

    label(&b, "window", "60s");
    end_usec(&b, last_60s_us);
}

static void render_diag(metrics_writer_t *w, const diag_metrics_view_t *g)
{
    render_cpu(w, g);
    render_tasks(w, g);
    single(w, "br_tasks_untracked", "gauge", "Tasks beyond the ones the sampler tracks.", g->untracked);
    heap_family(w, "br_heap_free_bytes", "Free heap.", g->internal.free, g->psram.free);
    heap_family(w, "br_heap_min_free_bytes", "Least free heap since boot.", g->internal.min_free, g->psram.min_free);
    heap_family(w, "br_heap_largest_free_block_bytes", "Largest free heap block.", g->internal.largest_free_block,
                g->psram.largest_free_block);
    single(w, "br_ot_lock_acquisitions_total", "counter", "OpenThread lock acquisitions.", g->lock.acquisitions);
    family(w, "br_ot_lock_hold_seconds_total", "counter", "Time the OpenThread lock was held.");
    sample_t hold = begin(w, "br_ot_lock_hold_seconds_total");

    end_usec(&hold, g->lock.hold_us_total);
    family(w, "br_ot_lock_wait_seconds_total", "counter", "Time spent waiting for the OpenThread lock.");
    sample_t wait = begin(w, "br_ot_lock_wait_seconds_total");

    end_usec(&wait, g->lock.wait_us_total);
    window_family(w, "br_ot_lock_hold_max_seconds", "Longest OpenThread lock hold.", g->lock.max_hold_us,
                  g->lock.max_hold_60s_us);
    window_family(w, "br_ot_lock_wait_max_seconds", "Longest wait for the OpenThread lock.", g->lock.max_wait_us,
                  g->lock.max_wait_60s_us);
}

static void render_device(metrics_writer_t *w, const metrics_input_t *in, bool partial)
{
    const metrics_device_t *d = in->device;

    render_info(w, d);
    if (in->diag) {
        single(w, "br_uptime_seconds", "gauge", "Seconds since boot.", in->diag->uptime_s);
        family(w, "br_reset_reason", "gauge", "Reason of the last reset (value 1).");
        sample_t s = begin(w, "br_reset_reason");

        label(&s, "reason", in->diag->reset_reason ? in->diag->reset_reason : "unknown");
        end_u64(&s, 1);
    }
    state_set(w, "br_mode", "Operating mode.", "mode", mode_names, COUNT(mode_names), d->mode);
    if (in->diag) {
        render_diag(w, in->diag);
    }
    single(w, "br_storage_ok", "gauge", "NVS storage initialised.", d->storage_ok);
    single(w, "br_sniffer_active", "gauge", "A sniffer capture is running.", d->sniffer_active);
    single(w, "br_sniffer_frames_total", "counter", "Frames captured by the current or last capture.", d->sniffer_frames);
    single(w, "br_sniffer_drops_total", "counter", "Frames dropped by the current or last capture.", d->sniffer_drops);
    single(w, "br_eth_up", "gauge", "The management interface is up.", d->eth_up);
    single(w, "br_network_trial_pending", "gauge", "A network configuration waits for confirmation.", d->trial_pending);
    single(w, "br_scrape_partial", "gauge", "A source was unavailable and its series are missing.", partial);
}

// ---- thread ----

static void labelled_u64(metrics_writer_t *w, const char *name, const char *key, const char *value, uint64_t v)
{
    sample_t s = begin(w, name);

    label(&s, key, value);
    end_u64(&s, v);
}

static void render_mac(metrics_writer_t *w, const metrics_thread_t *t)
{
    single(w, "br_mac_tx_frames_total", "counter", "802.15.4 frames sent.", t->mac.tx_total);
    single(w, "br_mac_tx_retries_total", "counter", "802.15.4 retransmissions.", t->mac.tx_retry);
    family(w, "br_mac_tx_errors_total", "counter", "802.15.4 send errors.");
    labelled_u64(w, "br_mac_tx_errors_total", "reason", "cca", t->mac.tx_err_cca);
    labelled_u64(w, "br_mac_tx_errors_total", "reason", "abort", t->mac.tx_err_abort);
    labelled_u64(w, "br_mac_tx_errors_total", "reason", "busy_channel", t->mac.tx_err_busy_channel);
    single(w, "br_mac_rx_frames_total", "counter", "802.15.4 frames received.", t->mac.rx_total);
    single(w, "br_mac_rx_duplicated_total", "counter", "Duplicated 802.15.4 frames received.", t->mac.rx_duplicated);
    family(w, "br_mac_rx_errors_total", "counter", "802.15.4 receive errors.");
    labelled_u64(w, "br_mac_rx_errors_total", "reason", "fcs", t->mac.rx_err_fcs);
    labelled_u64(w, "br_mac_rx_errors_total", "reason", "security", t->mac.rx_err_sec);
    labelled_u64(w, "br_mac_rx_errors_total", "reason", "no_frame", t->mac.rx_err_no_frame);
    labelled_u64(w, "br_mac_rx_errors_total", "reason", "unknown_neighbor", t->mac.rx_err_unknown_neighbor);
    labelled_u64(w, "br_mac_rx_errors_total", "reason", "invalid_src", t->mac.rx_err_invalid_src);
    labelled_u64(w, "br_mac_rx_errors_total", "reason", "other", t->mac.rx_err_other);
}

static void ip6_sample(metrics_writer_t *w, const char *direction, const char *result, uint32_t value)
{
    sample_t s = begin(w, "br_thread_ip6_packets_total");

    label(&s, "direction", direction);
    label(&s, "result", result);
    end_u64(&s, value);
}

static void br_traffic(metrics_writer_t *w, const char *name, const char *help, const uint64_t v[2][2])
{
    family(w, name, "counter", help);
    for (int d = 0; d < 2; d++) {
        for (int c = 0; c < 2; c++) {
            sample_t s = begin(w, name);

            label(&s, "direction", directions[d]);
            label(&s, "cast", casts[c]);
            end_u64(&s, v[d][c]);
        }
    }
}

static void event_family(metrics_writer_t *w, const char *name, const char *help, const uint32_t v[3])
{
    family(w, name, "counter", help);
    for (size_t i = 0; i < COUNT(events); i++) {
        labelled_u64(w, name, "event", events[i], v[i]);
    }
}

static void render_thread(metrics_writer_t *w, const metrics_thread_t *t)
{
    state_set(w, "br_thread_role", "Thread device role.", "role", role_names, COUNT(role_names), t->role);
    family(w, "br_thread_role_entered_total", "counter", "Times each role was entered.");
    for (size_t i = 0; i < COUNT(role_names); i++) {
        labelled_u64(w, "br_thread_role_entered_total", "role", role_names[i], t->role_entered[i]);
    }
    single(w, "br_thread_partition_changes_total", "counter", "Thread partition id changes.", t->partition_changes);
    single(w, "br_thread_attach_attempts_total", "counter", "Thread attach attempts.", t->attach_attempts);
    single(w, "br_thread_routers", "gauge", "Routers in the Thread network.", t->routers);
    single(w, "br_thread_children", "gauge", "Children of this border router.", t->children);
    single(w, "br_thread_neighbors", "gauge", "Neighbours of this border router.", t->neighbors);
    render_mac(w, t);
    family(w, "br_thread_ip6_packets_total", "counter", "IPv6 packets over the Thread interface.");
    ip6_sample(w, "tx", "success", t->ip6.tx_success);
    ip6_sample(w, "tx", "failure", t->ip6.tx_failure);
    ip6_sample(w, "rx", "success", t->ip6.rx_success);
    ip6_sample(w, "rx", "failure", t->ip6.rx_failure);
    state_set(w, "br_border_routing_state", "Border routing manager state.", "state", br_state_names,
              COUNT(br_state_names), t->br_state);
    if (t->pd_state >= 0) {
        state_set(w, "br_dhcp6_pd_state", "DHCPv6 prefix delegation state.", "state", pd_state_names,
                  COUNT(pd_state_names), t->pd_state);
    }
    br_traffic(w, "br_border_routing_packets_total", "Packets routed between Thread and the backbone.", t->br_packets);
    br_traffic(w, "br_border_routing_bytes_total", "Bytes routed between Thread and the backbone.", t->br_bytes);
    event_family(w, "br_border_routing_ra_total", "Router advertisements.", t->ra);
    event_family(w, "br_border_routing_rs_total", "Router solicitations.", t->rs);
    state_set(w, "br_srp_server_state", "SRP server state.", "state", srp_state_names, COUNT(srp_state_names),
              t->srp_state);
    single(w, "br_srp_hosts", "gauge", "Hosts registered with the SRP server.", t->srp_hosts);
    single(w, "br_srp_services", "gauge", "Services registered with the SRP server.", t->srp_services);
}

esp_err_t metrics_render(metrics_writer_t *w, const metrics_input_t *in)
{
    const metrics_collector_view_t *c = in->collector;
    const bool collector_ok = c && c->started;
    const bool partial = !in->diag || !collector_ok;

    render_device(w, in, partial);
    if (collector_ok && c->valid && c->age_ms < METRICS_FRESH_MS) {
        render_thread(w, &c->thread);
    }
    if (collector_ok) {
        if (c->ever_collected) {
            family(w, "br_thread_collect_age_seconds", "gauge", "Seconds since the last successful Thread collection.");
            sample_t s = begin(w, "br_thread_collect_age_seconds");

            end_usec(&s, (uint64_t)c->age_ms * 1000);
        }
        single(w, "br_thread_collect_failures_total", "counter",
               "Thread collections skipped because the OpenThread lock was busy.", c->failures);
    }
    return w->err;
}
