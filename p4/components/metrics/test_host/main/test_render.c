#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "render.h"
#include "sink.h"

static sample_input_t s_in;

static char *render(void)
{
    return render_to_string(&s_in.in, 2048);
}

static bool has_line(const char *body, const char *line)
{
    const size_t n = strlen(line);

    for (const char *p = strstr(body, line); p; p = strstr(p + 1, line)) {
        if ((p == body || p[-1] == '\n') && p[n] == '\n') {
            return true;
        }
    }
    return false;
}

// Lines that start with `prefix` (a metric name followed by "{" or " " to avoid matching longer names).
static int count_prefix(const char *body, const char *prefix)
{
    int n = 0;
    const size_t len = strlen(prefix);

    for (const char *p = body; *p;) {
        if (strncmp(p, prefix, len) == 0) {
            n++;
        }
        const char *nl = strchr(p, '\n');
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
    return n;
}

// Series of a state set whose value is 1.
static int ones(const char *body, const char *name)
{
    int n = 0;
    char prefix[96];

    snprintf(prefix, sizeof(prefix), "%s{", name);
    for (const char *p = strstr(body, prefix); p; p = strstr(p + 1, prefix)) {
        if (p != body && p[-1] != '\n') {
            continue;
        }
        const char *nl = strchr(p, '\n');
        if (nl && nl - p >= 2 && nl[-1] == '1' && nl[-2] == ' ') {
            n++;
        }
    }
    return n;
}

static void with_sample(void)
{
    sample_input(&s_in);
}

TEST_CASE("On any input: HELP and TYPE once per family, before its samples", "[metrics]")
{
    with_sample();
    char *out = render();

    TEST_ASSERT_EQUAL(1, count_prefix(out, "# HELP br_mode "));
    TEST_ASSERT_EQUAL(1, count_prefix(out, "# TYPE br_mode gauge"));
    TEST_ASSERT_TRUE(strstr(out, "# TYPE br_mode gauge\n") < strstr(out, "br_mode{"));
    TEST_ASSERT_TRUE(has_line(out, "# TYPE br_mac_tx_frames_total counter"));
    free(out);
}

TEST_CASE("On any input: families in the catalogue's order", "[metrics]")
{
    with_sample();
    char *out = render();
    const char *order[] = {"# TYPE br_info ", "# TYPE br_uptime_seconds ", "# TYPE br_mode ", "# TYPE br_cpu_seconds_total ",
                           "# TYPE br_heap_free_bytes ", "# TYPE br_chip_temperature_celsius ", "# TYPE br_ot_lock_acquisitions_total ", "# TYPE br_storage_ok ",
                           "# TYPE br_scrape_partial ", "# TYPE br_thread_role ", "# TYPE br_mac_tx_frames_total ",
                           "# TYPE br_border_routing_state ", "# TYPE br_srp_hosts ", "# TYPE br_trel_enabled ",
                           "# TYPE br_trel_tx_failures_total ", "# TYPE br_thread_collect_age_seconds ",
                           "# TYPE br_thread_collect_failures_total ", "# TYPE br_routing_method ",
                           "# TYPE br_routing_announced_prefixes ", "# TYPE br_routing_omr_custom ",
                           "# TYPE br_bgp_peer_state ", "# TYPE br_bgp_peer_uptime_seconds ",
                           "# TYPE br_bgp_received_dropped_total ", "# TYPE br_bgp_session_resets_total "
                          };
    const char *prev = out;

    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        const char *at = strstr(out, order[i]);

        TEST_ASSERT_NOT_NULL_MESSAGE(at, order[i]);
        TEST_ASSERT_TRUE_MESSAGE(at >= prev, order[i]);
        prev = at;
    }
    free(out);
}

TEST_CASE("Device family: br_info carries version, slot, rcp_version and rcp_compatible with value 1", "[metrics]")
{
    with_sample();
    char *out = render();

    TEST_ASSERT_TRUE(has_line(out, "br_info{version=\"v0.2.0\",slot=\"1\",rcp_version=\"rcp-1.2\",rcp_compatible=\"true\"} 1"));
    free(out);
}

TEST_CASE("Device family: br_info's rcp_compatible is \"false\" for a rejected RCP", "[metrics]")
{
    with_sample();
    s_in.device.rcp_compatible = false;
    char *out = render();

    TEST_ASSERT_NOT_NULL(strstr(out, "rcp_compatible=\"false\"} 1"));
    free(out);
}

TEST_CASE("Device family: br_info's rcp_compatible is \"unknown\" while the RCP is not judged", "[metrics]")
{
    with_sample();
    s_in.device.rcp_compatible_known = false;
    s_in.device.rcp_compatible = false;
    char *out = render();

    TEST_ASSERT_NOT_NULL(strstr(out, "rcp_compatible=\"unknown\"} 1"));
    TEST_ASSERT_NULL(strstr(out, "rcp_compatible=\"false\""));
    free(out);
}

TEST_CASE("Device family: br_mode has exactly one series at 1", "[metrics]")
{
    with_sample();
    char *out = render();

    TEST_ASSERT_EQUAL(5, count_prefix(out, "br_mode{"));
    TEST_ASSERT_EQUAL(1, ones(out, "br_mode"));
    TEST_ASSERT_TRUE(has_line(out, "br_mode{mode=\"BR_RUNNING\"} 1"));
    free(out);
}

TEST_CASE("Device family: br_cpu_seconds_total per core, busy = wall - idle", "[metrics]")
{
    with_sample();
    char *out = render();

    TEST_ASSERT_TRUE(has_line(out, "br_cpu_seconds_total{core=\"0\",mode=\"busy\"} 600.000000"));
    TEST_ASSERT_TRUE(has_line(out, "br_cpu_seconds_total{core=\"0\",mode=\"idle\"} 3000.000000"));
    TEST_ASSERT_TRUE(has_line(out, "br_cpu_seconds_total{core=\"1\",mode=\"busy\"} 599.999999"));
    TEST_ASSERT_TRUE(has_line(out, "br_cpu_seconds_total{core=\"1\",mode=\"idle\"} 3000.000001"));
    free(out);
}

TEST_CASE("Device family: lock and heap figures with their labels", "[metrics]")
{
    with_sample();
    char *out = render();

    TEST_ASSERT_TRUE(has_line(out, "br_ot_lock_acquisitions_total 12345"));
    TEST_ASSERT_TRUE(has_line(out, "br_ot_lock_hold_seconds_total 2.500000"));
    TEST_ASSERT_TRUE(has_line(out, "br_ot_lock_wait_seconds_total 0.001500"));
    TEST_ASSERT_TRUE(has_line(out, "br_ot_lock_hold_max_seconds{window=\"boot\"} 0.040000"));
    TEST_ASSERT_TRUE(has_line(out, "br_ot_lock_wait_max_seconds{window=\"60s\"} 0.000300"));
    TEST_ASSERT_TRUE(has_line(out, "br_heap_free_bytes{region=\"internal\"} 200000"));
    TEST_ASSERT_TRUE(has_line(out, "br_heap_largest_free_block_bytes{region=\"psram\"} 28000000"));
    TEST_ASSERT_TRUE(has_line(out, "br_reset_reason{reason=\"power-on\"} 1"));
    free(out);
}

TEST_CASE("On a valid chip temperature: br_chip_temperature_celsius with HELP, TYPE gauge and one decimal", "[metrics]")
{
    with_sample();
    char *out = render();

    TEST_ASSERT_TRUE(has_line(out, "# HELP br_chip_temperature_celsius Chip (die) temperature."));
    TEST_ASSERT_TRUE(has_line(out, "# TYPE br_chip_temperature_celsius gauge"));
    TEST_ASSERT_TRUE(has_line(out, "br_chip_temperature_celsius 47.3"));
    TEST_ASSERT_TRUE(strstr(out, "br_heap_largest_free_block_bytes{") < strstr(out, "# TYPE br_chip_temperature_celsius"));
    free(out);
}

TEST_CASE("On a chip temperature below zero or rounding to zero: the sign is written once, never -0.0", "[metrics]")
{
    with_sample();
    s_in.diag.chip_temperature_c = -3.04f;
    char *out = render();

    TEST_ASSERT_TRUE(has_line(out, "br_chip_temperature_celsius -3.0"));
    free(out);
    s_in.diag.chip_temperature_c = -0.04f;
    out = render();
    TEST_ASSERT_TRUE(has_line(out, "br_chip_temperature_celsius 0.0"));
    free(out);
}

TEST_CASE("On an invalid chip temperature: no family, br_scrape_partial 0", "[metrics]")
{
    with_sample();
    s_in.diag.chip_temperature_valid = false;
    char *out = render();

    TEST_ASSERT_NULL(strstr(out, "br_chip_temperature_celsius"));
    TEST_ASSERT_TRUE(has_line(out, "br_scrape_partial 0"));
    free(out);
}

TEST_CASE("On a non-finite chip temperature: no family", "[metrics]")
{
    with_sample();
    s_in.diag.chip_temperature_c = NAN;
    char *out = render();

    TEST_ASSERT_NULL(strstr(out, "br_chip_temperature_celsius"));
    free(out);
}

TEST_CASE("On two tasks with the same name: the second is labelled <name>#2", "[metrics]")
{
    with_sample();
    char *out = render();

    TEST_ASSERT_TRUE(has_line(out, "br_task_cpu_seconds_total{task=\"worker\"} 5.000001"));
    TEST_ASSERT_TRUE(has_line(out, "br_task_cpu_seconds_total{task=\"worker#2\"} 6.000001"));
    TEST_ASSERT_TRUE(has_line(out, "br_task_stack_free_min_bytes{task=\"worker#2\"} 517"));
    free(out);
}

TEST_CASE("On a task literally named like a generated label: every task label is unique", "[metrics]")
{
    with_sample();
    const char *names[] = {"worker", "worker", "worker#2"};

    s_in.diag.task_count = 3;
    for (size_t i = 0; i < 3; i++) {
        memset(s_in.diag.tasks[i].name, 0, sizeof(s_in.diag.tasks[i].name));
        strncpy(s_in.diag.tasks[i].name, names[i], DIAG_NAME_MAX - 1);
        s_in.diag.tasks[i].cpu_us = 1000000ull * (i + 1);
    }
    char *out = render();

    TEST_ASSERT_EQUAL(1, count_prefix(out, "br_task_cpu_seconds_total{task=\"worker\"}"));
    TEST_ASSERT_EQUAL(1, count_prefix(out, "br_task_cpu_seconds_total{task=\"worker#2\"}"));
    TEST_ASSERT_EQUAL(1, count_prefix(out, "br_task_cpu_seconds_total{task=\"worker#3\"}"));
    TEST_ASSERT_TRUE(has_line(out, "br_task_cpu_seconds_total{task=\"worker#2\"} 3.000000"));
    TEST_ASSERT_TRUE(has_line(out, "br_task_cpu_seconds_total{task=\"worker#3\"} 2.000000"));
    free(out);
}

TEST_CASE("On a NULL diag view: diag families left out, br_scrape_partial 1", "[metrics]")
{
    with_sample();
    s_in.in.diag = NULL;
    char *out = render();

    TEST_ASSERT_NULL(strstr(out, "br_uptime_seconds"));
    TEST_ASSERT_NULL(strstr(out, "br_cpu_seconds_total"));
    TEST_ASSERT_NULL(strstr(out, "br_heap_"));
    TEST_ASSERT_NULL(strstr(out, "br_chip_temperature_celsius"));
    TEST_ASSERT_NULL(strstr(out, "br_ot_lock_"));
    TEST_ASSERT_TRUE(has_line(out, "br_mode{mode=\"BR_RUNNING\"} 1"));
    TEST_ASSERT_TRUE(has_line(out, "br_scrape_partial 1"));
    TEST_ASSERT_TRUE(has_line(out, "br_thread_role{role=\"leader\"} 1"));
    free(out);
}

TEST_CASE("On a NULL collector view: Thread and collector families left out, br_scrape_partial 1", "[metrics]")
{
    with_sample();
    s_in.in.collector = NULL;
    char *out = render();

    TEST_ASSERT_NULL(strstr(out, "br_thread_"));
    TEST_ASSERT_NULL(strstr(out, "br_mac_"));
    TEST_ASSERT_TRUE(has_line(out, "br_scrape_partial 1"));
    free(out);
}

TEST_CASE("On a collector that is not started: Thread and collector families left out, br_scrape_partial 1", "[metrics]")
{
    with_sample();
    s_in.collector.started = false;
    char *out = render();

    TEST_ASSERT_NULL(strstr(out, "br_thread_"));
    TEST_ASSERT_TRUE(has_line(out, "br_scrape_partial 1"));
    free(out);
}

TEST_CASE("On a valid cache younger than 30 s: every Thread family, state sets with one series at 1", "[metrics]")
{
    with_sample();
    s_in.collector.age_ms = 29999;
    char *out = render();
    const char *families[] = {"br_thread_role_entered_total", "br_thread_partition_changes_total",
                              "br_thread_attach_attempts_total", "br_thread_routers", "br_thread_children",
                              "br_thread_neighbors", "br_mac_tx_frames_total", "br_mac_tx_retries_total",
                              "br_mac_tx_errors_total", "br_mac_rx_frames_total", "br_mac_rx_duplicated_total",
                              "br_mac_rx_errors_total", "br_thread_ip6_packets_total", "br_border_routing_packets_total",
                              "br_border_routing_bytes_total", "br_border_routing_ra_total", "br_border_routing_rs_total",
                              "br_srp_hosts", "br_srp_services", "br_trel_enabled", "br_trel_active", "br_trel_peers",
                              "br_trel_peers_other", "br_trel_packets_total", "br_trel_bytes_total", "br_trel_tx_failures_total"
                             };

    for (size_t i = 0; i < sizeof(families) / sizeof(families[0]); i++) {
        char type[96];

        snprintf(type, sizeof(type), "# TYPE %s ", families[i]);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, type), families[i]);
    }
    const char *sets[] = {"br_thread_role", "br_border_routing_state", "br_dhcp6_pd_state", "br_srp_server_state"};

    for (size_t i = 0; i < sizeof(sets) / sizeof(sets[0]); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(1, ones(out, sets[i]), sets[i]);
    }
    TEST_ASSERT_TRUE(has_line(out, "br_border_routing_bytes_total{direction=\"outbound\",cast=\"multicast\"} 10000000003"));
    TEST_ASSERT_TRUE(has_line(out, "br_mac_rx_errors_total{reason=\"invalid_src\"} 9"));
    TEST_ASSERT_TRUE(has_line(out, "br_thread_ip6_packets_total{direction=\"rx\",result=\"failure\"} 2"));
    TEST_ASSERT_TRUE(has_line(out, "br_border_routing_ra_total{event=\"tx_failure\"} 22"));
    TEST_ASSERT_TRUE(has_line(out, "br_trel_peers 2"));
    TEST_ASSERT_TRUE(has_line(out, "br_trel_packets_total{direction=\"rx\"} 17951"));
    TEST_ASSERT_TRUE(has_line(out, "br_trel_bytes_total{direction=\"tx\"} 5000000000"));
    TEST_ASSERT_TRUE(has_line(out, "br_trel_tx_failures_total 3"));
    TEST_ASSERT_TRUE(has_line(out, "br_scrape_partial 0"));
    free(out);
}

TEST_CASE("On a cache aged 30 s or more: Thread families left out, age and failures still written, partial 0", "[metrics]")
{
    with_sample();
    s_in.collector.age_ms = 30000;
    char *out = render();

    TEST_ASSERT_NULL(strstr(out, "br_thread_role"));
    TEST_ASSERT_NULL(strstr(out, "br_mac_"));
    TEST_ASSERT_TRUE(has_line(out, "br_thread_collect_age_seconds 30.000000"));
    TEST_ASSERT_TRUE(has_line(out, "br_thread_collect_failures_total 1"));
    TEST_ASSERT_TRUE(has_line(out, "br_scrape_partial 0"));
    free(out);
}

TEST_CASE("On an invalid cache: Thread families left out", "[metrics]")
{
    with_sample();
    s_in.collector.valid = false;
    char *out = render();

    TEST_ASSERT_NULL(strstr(out, "br_thread_role"));
    TEST_ASSERT_TRUE(has_line(out, "br_thread_collect_failures_total 1"));
    free(out);
}

TEST_CASE("Before the first successful collection: no age, failures written", "[metrics]")
{
    with_sample();
    s_in.collector.ever_collected = false;
    s_in.collector.valid = false;
    char *out = render();

    TEST_ASSERT_NULL(strstr(out, "br_thread_collect_age_seconds"));
    TEST_ASSERT_TRUE(has_line(out, "br_thread_collect_failures_total 1"));
    free(out);
}

TEST_CASE("On pd_state -1: br_dhcp6_pd_state left out", "[metrics]")
{
    with_sample();
    s_in.collector.thread.pd_state = -1;
    char *out = render();

    TEST_ASSERT_NULL(strstr(out, "br_dhcp6_pd_state"));
    TEST_ASSERT_NOT_NULL(strstr(out, "br_border_routing_state{"));
    free(out);
}

TEST_CASE("On a label value with quotes and backslashes: escaped in the output", "[metrics]")
{
    with_sample();
    s_in.device.version = "v\"1\\x";
    char *out = render();

    TEST_ASSERT_NOT_NULL(strstr(out, "br_info{version=\"v\\\"1\\\\x\","));
    free(out);
}

TEST_CASE("On a small buffer: output identical to one large buffer", "[metrics]")
{
    with_sample();
    char *small = render_to_string(&s_in.in, 64);
    char *large = render_to_string(&s_in.in, 65536);

    TEST_ASSERT_EQUAL_STRING(large, small);
    free(small);
    free(large);
}

TEST_CASE("On a writer error: metrics_render returns it", "[metrics]")
{
    with_sample();
    char buf[64];
    sink_t sink = {.fail_at = 3};
    metrics_writer_t w;

    metrics_writer_init(&w, buf, sizeof(buf), sink_flush, &sink);
    TEST_ASSERT_EQUAL(ESP_FAIL, metrics_render(&w, &s_in.in));
    sink_free(&sink);
}

// ---- routing ----

static const char *const bgp_families[] = {"br_bgp_peer_state", "br_bgp_peer_uptime_seconds",
                                           "br_bgp_received_dropped_total", "br_bgp_session_resets_total"
                                          };
static const char *const ripng_families[] = {"br_ripng_updates_sent_total", "br_ripng_requests_answered_total",
                                             "br_ripng_responses_dropped_total"
                                            };

#define ASSERT_NO_FAMILIES(out, names) \
    for (size_t i_ = 0; i_ < sizeof(names) / sizeof((names)[0]); i_++) { \
        TEST_ASSERT_NULL_MESSAGE(strstr(out, (names)[i_]), (names)[i_]); \
    }

static void with_method(routing_method_t method)
{
    with_sample();
    s_in.routing.method = method;
    if (method != ROUTING_METHOD_BGP) {
        s_in.routing.peer_count = 0;
    }
}

TEST_CASE("Routing bgp: br_routing_method has one series per method, bgp at 1", "[metrics]")
{
    with_method(ROUTING_METHOD_BGP);
    char *out = render();

    TEST_ASSERT_TRUE(has_line(out, "# TYPE br_routing_method gauge"));
    TEST_ASSERT_EQUAL(4, count_prefix(out, "br_routing_method{"));
    TEST_ASSERT_EQUAL(1, ones(out, "br_routing_method"));
    TEST_ASSERT_TRUE(has_line(out, "br_routing_method{method=\"pd\"} 0"));
    TEST_ASSERT_TRUE(has_line(out, "br_routing_method{method=\"ripng\"} 0"));
    TEST_ASSERT_TRUE(has_line(out, "br_routing_method{method=\"bgp\"} 1"));
    TEST_ASSERT_TRUE(has_line(out, "br_routing_method{method=\"off\"} 0"));
    free(out);
}

TEST_CASE("Routing bgp: announced prefix count and custom OMR flag as gauges", "[metrics]")
{
    with_method(ROUTING_METHOD_BGP);
    char *out = render();

    TEST_ASSERT_TRUE(has_line(out, "# TYPE br_routing_announced_prefixes gauge"));
    TEST_ASSERT_TRUE(has_line(out, "br_routing_announced_prefixes 2"));
    TEST_ASSERT_TRUE(has_line(out, "# TYPE br_routing_omr_custom gauge"));
    TEST_ASSERT_TRUE(has_line(out, "br_routing_omr_custom 1"));
    free(out);
}

TEST_CASE("Routing bgp: one state series per state and peer, one at 1 per peer", "[metrics]")
{
    with_method(ROUTING_METHOD_BGP);
    char *out = render();
    const char *states[] = {"idle", "connect", "active", "opensent", "openconfirm", "established"};

    TEST_ASSERT_TRUE(has_line(out, "# TYPE br_bgp_peer_state gauge"));
    TEST_ASSERT_EQUAL(12, count_prefix(out, "br_bgp_peer_state{"));
    TEST_ASSERT_EQUAL(2, ones(out, "br_bgp_peer_state"));
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        char line[128];

        snprintf(line, sizeof(line), "br_bgp_peer_state{peer=\"2001:db8::1\",state=\"%s\"} %d", states[i],
                 i == ROUTING_BGP_ESTABLISHED);
        TEST_ASSERT_TRUE_MESSAGE(has_line(out, line), line);
        snprintf(line, sizeof(line), "br_bgp_peer_state{peer=\"fe80::211:22ff:fe33:4455\",state=\"%s\"} %d",
                 states[i], i == ROUTING_BGP_ACTIVE);
        TEST_ASSERT_TRUE_MESSAGE(has_line(out, line), line);
    }
    free(out);
}

TEST_CASE("Routing bgp: uptime gauge, dropped and resets counters per peer", "[metrics]")
{
    with_method(ROUTING_METHOD_BGP);
    char *out = render();

    TEST_ASSERT_TRUE(has_line(out, "# TYPE br_bgp_peer_uptime_seconds gauge"));
    TEST_ASSERT_TRUE(has_line(out, "br_bgp_peer_uptime_seconds{peer=\"2001:db8::1\"} 7200"));
    TEST_ASSERT_TRUE(has_line(out, "br_bgp_peer_uptime_seconds{peer=\"fe80::211:22ff:fe33:4455\"} 0"));
    TEST_ASSERT_TRUE(has_line(out, "# TYPE br_bgp_received_dropped_total counter"));
    TEST_ASSERT_TRUE(has_line(out, "br_bgp_received_dropped_total{peer=\"2001:db8::1\"} 40"));
    TEST_ASSERT_TRUE(has_line(out, "br_bgp_received_dropped_total{peer=\"fe80::211:22ff:fe33:4455\"} 41"));
    TEST_ASSERT_TRUE(has_line(out, "# TYPE br_bgp_session_resets_total counter"));
    TEST_ASSERT_TRUE(has_line(out, "br_bgp_session_resets_total{peer=\"2001:db8::1\"} 3"));
    TEST_ASSERT_TRUE(has_line(out, "br_bgp_session_resets_total{peer=\"fe80::211:22ff:fe33:4455\"} 4"));
    free(out);
}

TEST_CASE("Routing bgp: no RIPng series", "[metrics]")
{
    with_method(ROUTING_METHOD_BGP);
    char *out = render();

    ASSERT_NO_FAMILIES(out, ripng_families);
    free(out);
}

TEST_CASE("Routing bgp with no peers: no peer families, method still written", "[metrics]")
{
    with_method(ROUTING_METHOD_BGP);
    s_in.routing.peer_count = 0;
    char *out = render();

    ASSERT_NO_FAMILIES(out, bgp_families);
    TEST_ASSERT_TRUE(has_line(out, "br_routing_method{method=\"bgp\"} 1"));
    free(out);
}

TEST_CASE("Routing ripng: the three RIPng counters", "[metrics]")
{
    with_method(ROUTING_METHOD_RIPNG);
    char *out = render();

    TEST_ASSERT_TRUE(has_line(out, "br_routing_method{method=\"ripng\"} 1"));
    TEST_ASSERT_TRUE(has_line(out, "# TYPE br_ripng_updates_sent_total counter"));
    TEST_ASSERT_TRUE(has_line(out, "br_ripng_updates_sent_total 120"));
    TEST_ASSERT_TRUE(has_line(out, "# TYPE br_ripng_requests_answered_total counter"));
    TEST_ASSERT_TRUE(has_line(out, "br_ripng_requests_answered_total 5"));
    TEST_ASSERT_TRUE(has_line(out, "# TYPE br_ripng_responses_dropped_total counter"));
    TEST_ASSERT_TRUE(has_line(out, "br_ripng_responses_dropped_total 9"));
    free(out);
}

TEST_CASE("Routing ripng: no bgp series", "[metrics]")
{
    with_method(ROUTING_METHOD_RIPNG);
    char *out = render();

    ASSERT_NO_FAMILIES(out, bgp_families);
    free(out);
}

TEST_CASE("Routing pd or off: method, prefixes and OMR flag only, no BGP or RIPng series", "[metrics]")
{
    const routing_method_t methods[] = {ROUTING_METHOD_PD, ROUTING_METHOD_OFF};

    for (size_t m = 0; m < 2; m++) {
        with_method(methods[m]);
        char *out = render();

        TEST_ASSERT_EQUAL(1, ones(out, "br_routing_method"));
        TEST_ASSERT_NOT_NULL(strstr(out, "\nbr_routing_announced_prefixes "));
        TEST_ASSERT_NOT_NULL(strstr(out, "\nbr_routing_omr_custom "));
        ASSERT_NO_FAMILIES(out, bgp_families);
        ASSERT_NO_FAMILIES(out, ripng_families);
        free(out);
    }
}

TEST_CASE("Routing absent: no routing series", "[metrics]")
{
    with_sample();
    s_in.in.routing = NULL;
    char *out = render();

    TEST_ASSERT_NULL(strstr(out, "br_routing_"));
    TEST_ASSERT_NULL(strstr(out, "br_bgp_"));
    TEST_ASSERT_NULL(strstr(out, "br_ripng_"));
    free(out);
}

TEST_CASE("Routing absent: br_scrape_partial 1", "[metrics]")
{
    with_sample();
    s_in.in.routing = NULL;
    char *out = render();

    TEST_ASSERT_TRUE(has_line(out, "br_scrape_partial 1"));
    free(out);
}

static void write_sample(const char *path)
{
    char *out = render();
    FILE *f = fopen(path, "w");

    TEST_ASSERT_NOT_NULL(f);
    fputs(out, f);
    fclose(f);
    free(out);
}

TEST_CASE("Sample for the format check: written to METRICS_SAMPLE_PATH", "[metrics]")
{
    with_sample();
    write_sample(METRICS_SAMPLE_PATH);
}

TEST_CASE("Sample for the format check, routing ripng: written to METRICS_SAMPLE_RIPNG_PATH", "[metrics]")
{
    with_method(ROUTING_METHOD_RIPNG);
    write_sample(METRICS_SAMPLE_RIPNG_PATH);
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = (int)UNITY_END();
    exit(failures == 0 ? 0 : 1);
}

TEST_CASE("On a build without TREL: no TREL families", "[metrics]")
{
    with_sample();
    s_in.collector.thread.trel.built = false;
    char *out = render();

    TEST_ASSERT_NULL(strstr(out, "br_trel_"));
    free(out);
}

TEST_CASE("On TREL peers of both networks: br_trel_peers counts this network, br_trel_peers_other the rest",
          "[metrics]")
{
    with_sample();
    s_in.collector.thread.trel.peers = 2;
    s_in.collector.thread.trel.peers_other = 3;
    char *out = render();

    TEST_ASSERT_TRUE(has_line(out, "br_trel_peers 2"));
    TEST_ASSERT_TRUE(has_line(out, "br_trel_peers_other 3"));
    TEST_ASSERT_TRUE(strstr(out, "# TYPE br_trel_peers ") < strstr(out, "# TYPE br_trel_peers_other "));
    TEST_ASSERT_TRUE(strstr(out, "# TYPE br_trel_peers_other ") < strstr(out, "# TYPE br_trel_packets_total "));
    free(out);
}

TEST_CASE("On TREL switched off: enabled 0, active 0, families still written", "[metrics]")
{
    with_sample();
    s_in.collector.thread.trel.enabled = false;
    s_in.collector.thread.trel.active = false;
    s_in.collector.thread.trel.peers = 0;
    s_in.collector.thread.trel.peers_other = 0;
    char *out = render();

    TEST_ASSERT_TRUE(has_line(out, "br_trel_enabled 0"));
    TEST_ASSERT_TRUE(has_line(out, "br_trel_active 0"));
    TEST_ASSERT_TRUE(has_line(out, "br_trel_peers 0"));
    TEST_ASSERT_TRUE(has_line(out, "br_trel_peers_other 0"));
    free(out);
}

/* ---- Zigbee ---- */

static void with_zigbee(void)
{
    with_sample();
    s_in.device.zigbee = (metrics_zigbee_t) {
        .active = true, .frames_rx = 10, .frames_tx = 9, .tx_no_ack = 2, .tx_cca = 3, .tx_failed = 4, .rcp_resets = 6,
        .heap_current = 1000, .heap_peak = 2000, .client_connected = true,
    };
}

TEST_CASE("Zigbee series while a channel runs ziggurat: frames per direction", "[metrics]")
{
    with_zigbee();
    char *out = render();

    TEST_ASSERT_TRUE(has_line(out, "# TYPE zigbee_frames_total counter"));
    TEST_ASSERT_TRUE(has_line(out, "zigbee_frames_total{dir=\"rx\"} 10"));
    TEST_ASSERT_TRUE(has_line(out, "zigbee_frames_total{dir=\"tx\"} 9"));
    free(out);
}

TEST_CASE("Zigbee series: transmit results as no_ack, cca and failed", "[metrics]")
{
    with_zigbee();
    char *out = render();

    TEST_ASSERT_TRUE(has_line(out, "# TYPE zigbee_tx_results_total counter"));
    TEST_ASSERT_TRUE(has_line(out, "zigbee_tx_results_total{result=\"no_ack\"} 2"));
    TEST_ASSERT_TRUE(has_line(out, "zigbee_tx_results_total{result=\"cca\"} 3"));
    TEST_ASSERT_TRUE(has_line(out, "zigbee_tx_results_total{result=\"failed\"} 4"));
    TEST_ASSERT_EQUAL_INT(0, count_prefix(out, "zigbee_tx_results_total{result=\"acked\"}"));
    free(out);
}

TEST_CASE("Zigbee series: RCP resets, heap bytes by kind and the client flag", "[metrics]")
{
    with_zigbee();
    char *out = render();

    TEST_ASSERT_TRUE(has_line(out, "# TYPE zigbee_rcp_resets_total counter"));
    TEST_ASSERT_TRUE(has_line(out, "zigbee_rcp_resets_total 6"));
    TEST_ASSERT_TRUE(has_line(out, "# TYPE zigbee_heap_bytes gauge"));
    TEST_ASSERT_TRUE(has_line(out, "zigbee_heap_bytes{kind=\"current\"} 1000"));
    TEST_ASSERT_TRUE(has_line(out, "zigbee_heap_bytes{kind=\"peak\"} 2000"));
    TEST_ASSERT_TRUE(has_line(out, "# TYPE zigbee_client_connected gauge"));
    TEST_ASSERT_TRUE(has_line(out, "zigbee_client_connected 1"));
    free(out);
}

TEST_CASE("Zigbee series without a client: zigbee_client_connected is 0", "[metrics]")
{
    with_zigbee();
    s_in.device.zigbee.client_connected = false;
    char *out = render();

    TEST_ASSERT_TRUE(has_line(out, "zigbee_client_connected 0"));
    free(out);
}

TEST_CASE("Zigbee series without a ziggurat channel: no zigbee family", "[metrics]")
{
    with_sample();
    char *out = render();

    TEST_ASSERT_NULL(strstr(out, "zigbee_"));
    free(out);
}

TEST_CASE("Zigbee series: written after the routing families", "[metrics]")
{
    with_zigbee();
    char *out = render();
    const char *routing = strstr(out, "# TYPE br_bgp_session_resets_total ");
    const char *zigbee = strstr(out, "# TYPE zigbee_frames_total ");

    TEST_ASSERT_NOT_NULL(routing);
    TEST_ASSERT_NOT_NULL(zigbee);
    TEST_ASSERT_TRUE(zigbee > routing);
    free(out);
}

/* ---- Logging ---- */

TEST_CASE("Logging series: lines sent per channel, live from live_sent", "[metrics]")
{
    with_sample();
    char *out = render();

    TEST_ASSERT_TRUE(has_line(out, "# TYPE br_log_lines_sent_total counter"));
    TEST_ASSERT_TRUE(has_line(out, "br_log_lines_sent_total{channel=\"uart\"} 100"));
    TEST_ASSERT_TRUE(has_line(out, "br_log_lines_sent_total{channel=\"jtag\"} 40"));
    TEST_ASSERT_TRUE(has_line(out, "br_log_lines_sent_total{channel=\"syslog\"} 30"));
    TEST_ASSERT_TRUE(has_line(out, "br_log_lines_sent_total{channel=\"live\"} 20"));
    free(out);
}

TEST_CASE("Logging series: lines dropped per channel, live from live_dropped", "[metrics]")
{
    with_sample();
    char *out = render();

    TEST_ASSERT_TRUE(has_line(out, "# TYPE br_log_lines_dropped_total counter"));
    TEST_ASSERT_TRUE(has_line(out, "br_log_lines_dropped_total{channel=\"uart\"} 0"));
    TEST_ASSERT_TRUE(has_line(out, "br_log_lines_dropped_total{channel=\"jtag\"} 5"));
    TEST_ASSERT_TRUE(has_line(out, "br_log_lines_dropped_total{channel=\"syslog\"} 6"));
    TEST_ASSERT_TRUE(has_line(out, "br_log_lines_dropped_total{channel=\"live\"} 3"));
    free(out);
}

TEST_CASE("Logging series: br_log_viewers is a gauge of the connected live clients", "[metrics]")
{
    with_sample();
    char *out = render();

    TEST_ASSERT_TRUE(has_line(out, "# TYPE br_log_viewers gauge"));
    TEST_ASSERT_TRUE(has_line(out, "br_log_viewers 2"));
    free(out);
}

TEST_CASE("Logging series without a viewer or a line: written, all zero", "[metrics]")
{
    with_sample();
    memset(&s_in.device.logging, 0, sizeof(s_in.device.logging));
    char *out = render();

    TEST_ASSERT_TRUE(has_line(out, "br_log_viewers 0"));
    TEST_ASSERT_TRUE(has_line(out, "br_log_lines_sent_total{channel=\"live\"} 0"));
    TEST_ASSERT_TRUE(has_line(out, "br_log_lines_dropped_total{channel=\"live\"} 0"));
    free(out);
}

TEST_CASE("Logging series: written after the routing families, before Zigbee", "[metrics]")
{
    with_zigbee();
    char *out = render();
    const char *routing = strstr(out, "# TYPE br_bgp_session_resets_total ");
    const char *logging = strstr(out, "# TYPE br_log_lines_sent_total ");
    const char *zigbee = strstr(out, "# TYPE zigbee_frames_total ");

    TEST_ASSERT_NOT_NULL(logging);
    TEST_ASSERT_TRUE(logging > routing);
    TEST_ASSERT_TRUE(zigbee > logging);
    free(out);
}
