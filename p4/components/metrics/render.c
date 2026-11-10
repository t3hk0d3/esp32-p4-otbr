#include "render.h"

#include <math.h>
#include <string.h>

#include "diag.h"
#include "ipv6_text.h"

static const char *const mode_names[] = {"STARTING", "BR_RUNNING", "SNIFFER", "RADIO_FAULT", "NO_RADIO"};
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

// A label is label_open, its value written through the writer, then label_close.
static void label_open(sample_t *s, const char *key)
{
    metrics_writer_raw(s->w, s->labelled ? "," : "{");
    metrics_writer_raw(s->w, key);
    metrics_writer_raw(s->w, "=\"");
    s->labelled = true;
}

static void label_close(sample_t *s)
{
    metrics_writer_raw(s->w, "\"");
}

static void label(sample_t *s, const char *key, const char *value)
{
    label_open(s, key);
    metrics_writer_label_value(s->w, value);
    label_close(s);
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

// A family with one unlabelled sample in seconds.
static void single_usec(metrics_writer_t *w, const char *name, const char *type, const char *help, uint64_t usec)
{
    family(w, name, type, help);
    sample_t s = begin(w, name);

    end_usec(&s, usec);
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

// ---- device ----

static void render_info(metrics_writer_t *w, const metrics_device_t *d)
{
    family(w, "br_info", "gauge", "Firmware and radio versions (value 1).");
    sample_t s = begin(w, "br_info");

    label(&s, "version", d->version ? d->version : "");
    label_open(&s, "slot");
    if (d->slot >= 0) {
        metrics_writer_u64(w, (uint64_t)d->slot);
    }
    label_close(&s);
    label(&s, "rcp_version", d->rcp_version ? d->rcp_version : "");
    label(&s, "rcp_compatible", !d->rcp_compatible_known ? "unknown" : d->rcp_compatible ? "true" : "false");
    end_u64(&s, 1);
}

static void render_cpu(metrics_writer_t *w, const diag_metrics_view_t *g)
{
    family(w, "br_cpu_seconds_total", "counter", "CPU time per core, busy and idle.");
    for (int c = 0; c < DIAG_CORES; c++) {
        const uint64_t idle = g->cores[c].idle_us < g->cores[c].wall_us ? g->cores[c].idle_us : g->cores[c].wall_us;
        const uint64_t usec[2] = {g->cores[c].wall_us - idle, idle};
        static const char *const modes[2] = {"busy", "idle"};

        for (int m = 0; m < 2; m++) {
            sample_t s = begin(w, "br_cpu_seconds_total");

            label_open(&s, "core");
            metrics_writer_u64(w, (uint64_t)c);
            label_close(&s);
            label(&s, "mode", modes[m]);
            end_usec(&s, usec[m]);
        }
    }
}

// Task names are not unique in FreeRTOS: a repeated name gets "#2", "#3"... nth[i] is the suffix of task i (1 = none),
// chosen so that the label equals no earlier label, and a suffixed one no task's own name (tasks "a", "a", "a#2" -> "a", "a#3",
// "a#2"). Counted once for both task families.
#define TASK_LABEL_MAX (DIAG_NAME_MAX + 4)

static void format_label(char out[TASK_LABEL_MAX], const char *name, uint8_t n)
{
    size_t len = strnlen(name, DIAG_NAME_MAX);
    char digits[3];
    size_t d = 0;

    memcpy(out, name, len);
    if (n > 1) {
        out[len++] = '#';
        for (; n > 0; n /= 10) {
            digits[d++] = (char)('0' + n % 10);
        }
        while (d > 0) {
            out[len++] = digits[--d];
        }
    }
    out[len] = '\0';
}

// A plain name only has to differ from the earlier labels; a suffixed one also from every other task's own name.
static bool label_taken(const diag_metrics_view_t *g, const uint8_t nth[DIAG_MAX_TASKS], size_t i, const char *label,
                        bool suffixed)
{
    char other[TASK_LABEL_MAX];

    for (size_t k = 0; k < g->task_count; k++) {
        if (k < i) {
            format_label(other, g->tasks[k].name, nth[k]);
            if (strcmp(other, label) == 0) {
                return true;
            }
        }
        if (suffixed && k != i) {
            format_label(other, g->tasks[k].name, 1);
            if (strcmp(other, label) == 0) {
                return true;
            }
        }
    }
    return false;
}

static void task_occurrences(const diag_metrics_view_t *g, uint8_t nth[DIAG_MAX_TASKS])
{
    char label[TASK_LABEL_MAX];

    for (size_t i = 0; i < g->task_count; i++) {
        uint8_t n = 1;

        format_label(label, g->tasks[i].name, n);
        while (n < UINT8_MAX && label_taken(g, nth, i, label, n > 1)) {
            format_label(label, g->tasks[i].name, ++n);
        }
        nth[i] = n;
    }
}

static void task_label(sample_t *s, const diag_metrics_view_t *g, const uint8_t nth[DIAG_MAX_TASKS], size_t i)
{
    char name[DIAG_NAME_MAX + 1];

    memcpy(name, g->tasks[i].name, DIAG_NAME_MAX);
    name[DIAG_NAME_MAX] = '\0';
    label_open(s, "task");
    metrics_writer_label_value(s->w, name);
    if (nth[i] > 1) {
        metrics_writer_raw(s->w, "#");
        metrics_writer_u64(s->w, nth[i]);
    }
    label_close(s);
}

static void render_tasks(metrics_writer_t *w, const diag_metrics_view_t *g)
{
    uint8_t nth[DIAG_MAX_TASKS];

    if (g->task_count == 0) {
        return;
    }
    task_occurrences(g, nth);
    family(w, "br_task_cpu_seconds_total", "counter", "CPU time per task since it was first seen.");
    for (size_t i = 0; i < g->task_count; i++) {
        sample_t s = begin(w, "br_task_cpu_seconds_total");

        task_label(&s, g, nth, i);
        end_usec(&s, g->tasks[i].cpu_us);
    }
    family(w, "br_task_stack_free_min_bytes", "gauge", "Least free stack per task since it started.");
    for (size_t i = 0; i < g->task_count; i++) {
        sample_t s = begin(w, "br_task_stack_free_min_bytes");

        task_label(&s, g, nth, i);
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

// One decimal, rounded half away from zero. A value that rounds to zero is written "0.0": the sign comes from the
// rounded tenths, so never "-0.0". A non-finite or out-of-range reading writes no series.
static void chip_temperature_family(metrics_writer_t *w, const diag_metrics_view_t *g)
{
    if (!g->chip_temperature_valid || !isfinite(g->chip_temperature_c) || fabsf(g->chip_temperature_c) > 1000.0f) {
        return;
    }
    const float scaled = g->chip_temperature_c * 10.0f;
    const int32_t tenths = (int32_t)(scaled < 0.0f ? scaled - 0.5f : scaled + 0.5f);

    family(w, "br_chip_temperature_celsius", "gauge", "Chip (die) temperature.");
    sample_t s = begin(w, "br_chip_temperature_celsius");

    close_labels(&s);
    metrics_writer_tenths(w, tenths);
    metrics_writer_raw(w, "\n");
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
    chip_temperature_family(w, g);
    single(w, "br_ot_lock_acquisitions_total", "counter", "OpenThread lock acquisitions.", g->lock.acquisitions);
    single_usec(w, "br_ot_lock_hold_seconds_total", "counter", "Time the OpenThread lock was held.",
                g->lock.hold_us_total);
    single_usec(w, "br_ot_lock_wait_seconds_total", "counter", "Time spent waiting for the OpenThread lock.",
                g->lock.wait_us_total);
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

static void render_trel(metrics_writer_t *w, const metrics_thread_t *t)
{
    single(w, "br_trel_enabled", "gauge", "TREL is switched on (1) or off (0).", t->trel.enabled);
    single(w, "br_trel_active", "gauge", "OpenThread runs TREL: switched on and Thread up.", t->trel.active);
    single(w, "br_trel_peers", "gauge", "TREL peers of this Thread network on the backbone link.", t->trel.peers);
    single(w, "br_trel_peers_other", "gauge", "TREL peers of other Thread networks on the backbone link.",
           t->trel.peers_other);
    family(w, "br_trel_packets_total", "counter", "TREL packets.");
    labelled_u64(w, "br_trel_packets_total", "direction", "tx", t->trel.tx_packets);
    labelled_u64(w, "br_trel_packets_total", "direction", "rx", t->trel.rx_packets);
    family(w, "br_trel_bytes_total", "counter", "TREL bytes.");
    labelled_u64(w, "br_trel_bytes_total", "direction", "tx", t->trel.tx_bytes);
    labelled_u64(w, "br_trel_bytes_total", "direction", "rx", t->trel.rx_bytes);
    single(w, "br_trel_tx_failures_total", "counter", "TREL packets that could not be sent.", t->trel.tx_failures);
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
    if (t->trel.built) {
        render_trel(w, t);
    }
}

// ---- routing ----

static void peer_label(sample_t *s, const routing_metrics_peer_t *p)
{
    char text[ROUTING_IPV6_TEXT_MAX];

    routing_ipv6_to_text(p->addr, text, sizeof(text));
    label(s, "peer", text);
}

// One family with a sample per peer; values[i] belongs to peers[i].
static void peer_family(metrics_writer_t *w, const routing_metrics_view_t *r, const char *name, const char *type,
                        const char *help, const uint32_t values[ROUTING_MAX_PEERS])
{
    family(w, name, type, help);
    for (uint8_t i = 0; i < r->peer_count; i++) {
        sample_t s = begin(w, name);

        peer_label(&s, &r->peers[i]);
        end_u64(&s, values[i]);
    }
}

// Only the configured peers; a family with no peers is left out.
static void render_bgp(metrics_writer_t *w, const routing_metrics_view_t *r)
{
    uint32_t uptime[ROUTING_MAX_PEERS], dropped[ROUTING_MAX_PEERS], resets[ROUTING_MAX_PEERS];

    if (r->peer_count == 0 || r->peer_count > ROUTING_MAX_PEERS) {
        return;
    }
    family(w, "br_bgp_peer_state", "gauge", "BGP session state per configured peer.");
    for (uint8_t i = 0; i < r->peer_count; i++) {
        for (int k = 0; k < ROUTING_BGP_STATE_COUNT; k++) {
            sample_t s = begin(w, "br_bgp_peer_state");

            peer_label(&s, &r->peers[i]);
            label(&s, "state", routing_bgp_state_name(k));
            end_u64(&s, k == (int)r->peers[i].state ? 1 : 0);
        }
        uptime[i] = r->peers[i].uptime_s;
        dropped[i] = r->peers[i].received_dropped;
        resets[i] = r->peers[i].resets;
    }
    peer_family(w, r, "br_bgp_peer_uptime_seconds", "gauge", "Seconds since the BGP session was established, 0 when down.",
                uptime);
    peer_family(w, r, "br_bgp_received_dropped_total", "counter", "Routes received from the peer and dropped.", dropped);
    peer_family(w, r, "br_bgp_session_resets_total", "counter", "Times the BGP session dropped from Established.", resets);
}

static void render_routing(metrics_writer_t *w, const routing_metrics_view_t *r)
{
    const char *methods[ROUTING_METHOD_COUNT];

    for (int i = 0; i < ROUTING_METHOD_COUNT; i++) {
        methods[i] = routing_method_name(i);
    }
    state_set(w, "br_routing_method", "How the route into the mesh is announced on the backbone.", "method", methods,
              ROUTING_METHOD_COUNT, (int)r->method);
    single(w, "br_routing_announced_prefixes", "gauge", "Prefixes announced on the backbone.", r->announced_count);
    single(w, "br_routing_omr_custom", "gauge", "A custom OMR prefix is configured.", r->omr_custom);
    if (r->method == ROUTING_METHOD_BGP) {
        render_bgp(w, r);
    } else if (r->method == ROUTING_METHOD_RIPNG) {
        single(w, "br_ripng_updates_sent_total", "counter", "RIPng update messages sent, regular and triggered.",
               r->ripng_updates_sent);
        single(w, "br_ripng_requests_answered_total", "counter", "RIPng requests answered.", r->ripng_requests_answered);
        single(w, "br_ripng_responses_dropped_total", "counter", "RIPng responses and invalid packets received and dropped.",
               r->ripng_responses_dropped);
    }
}

// ---- logging ----

static void render_logging(metrics_writer_t *w, const metrics_logging_t *l)
{
    static const char *const channels[METRICS_LOG_CHANNELS] = {"uart", "jtag", "syslog"};

    family(w, "br_log_lines_sent_total", "counter", "Log lines written per channel (live: queued for a viewer).");
    for (size_t i = 0; i < METRICS_LOG_CHANNELS; i++) {
        labelled_u64(w, "br_log_lines_sent_total", "channel", channels[i], l->sent[i]);
    }
    labelled_u64(w, "br_log_lines_sent_total", "channel", "live", l->live_sent);
    family(w, "br_log_lines_dropped_total", "counter", "Log lines a channel wanted but dropped (buffer or queue full, send failed).");
    for (size_t i = 0; i < METRICS_LOG_CHANNELS; i++) {
        labelled_u64(w, "br_log_lines_dropped_total", "channel", channels[i], l->dropped[i]);
    }
    labelled_u64(w, "br_log_lines_dropped_total", "channel", "live", l->live_dropped);
    single(w, "br_log_viewers", "gauge", "Live log clients connected.", l->viewers);
}

// ---- zigbee ----

static void render_zigbee(metrics_writer_t *w, const metrics_zigbee_t *z)
{
    family(w, "zigbee_frames_total", "counter", "Radio frames received from and transmitted to the RCP by the Zigbee stack.");
    labelled_u64(w, "zigbee_frames_total", "dir", "rx", z->frames_rx);
    labelled_u64(w, "zigbee_frames_total", "dir", "tx", z->frames_tx);
    family(w, "zigbee_tx_results_total", "counter", "Transmit results other than success.");
    labelled_u64(w, "zigbee_tx_results_total", "result", "no_ack", z->tx_no_ack);
    labelled_u64(w, "zigbee_tx_results_total", "result", "cca", z->tx_cca);
    labelled_u64(w, "zigbee_tx_results_total", "result", "failed", z->tx_failed);
    single(w, "zigbee_rcp_resets_total", "counter", "RCP resets seen by the Zigbee stack.", z->rcp_resets);
    family(w, "zigbee_heap_bytes", "gauge", "PSRAM heap of the Zigbee stack in use, now and at its peak.");
    labelled_u64(w, "zigbee_heap_bytes", "kind", "current", z->heap_current);
    labelled_u64(w, "zigbee_heap_bytes", "kind", "peak", z->heap_peak);
    single(w, "zigbee_client_connected", "gauge", "A client is connected to the Zigbee WebSocket.", z->client_connected);
}

esp_err_t metrics_render(metrics_writer_t *w, const metrics_input_t *in)
{
    const metrics_collector_view_t *c = in->collector;
    const bool collector_ok = c && c->started;
    const bool partial = !in->diag || !collector_ok || !in->routing;

    render_device(w, in, partial);
    if (collector_ok && c->valid && c->age_ms < METRICS_FRESH_MS) {
        render_thread(w, &c->thread);
    }
    if (collector_ok) {
        if (c->ever_collected) {
            single_usec(w, "br_thread_collect_age_seconds", "gauge", "Seconds since the last successful Thread collection.",
                        (uint64_t)c->age_ms * 1000);
        }
        single(w, "br_thread_collect_failures_total", "counter",
               "Thread collections skipped because the OpenThread lock was busy.", c->failures);
    }
    if (in->routing) {
        render_routing(w, in->routing);
    }
    render_logging(w, &in->device->logging);
    if (in->device->zigbee.active) {
        render_zigbee(w, &in->device->zigbee);
    }
    return w->err;
}
