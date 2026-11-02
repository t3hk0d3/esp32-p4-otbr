#include <stdio.h>
#include <stdlib.h>
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
                           "# TYPE br_heap_free_bytes ", "# TYPE br_ot_lock_acquisitions_total ", "# TYPE br_storage_ok ",
                           "# TYPE br_scrape_partial ", "# TYPE br_thread_role ", "# TYPE br_mac_tx_frames_total ",
                           "# TYPE br_border_routing_state ", "# TYPE br_srp_hosts ", "# TYPE br_thread_collect_age_seconds ",
                           "# TYPE br_thread_collect_failures_total "
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

TEST_CASE("Device family: br_mode has exactly one series at 1", "[metrics]")
{
    with_sample();
    char *out = render();

    TEST_ASSERT_EQUAL(4, count_prefix(out, "br_mode{"));
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
                              "br_srp_hosts", "br_srp_services"
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

TEST_CASE("Sample for the format check: written to METRICS_SAMPLE_PATH", "[metrics]")
{
    with_sample();
    char *out = render();
    FILE *f = fopen(METRICS_SAMPLE_PATH, "w");

    TEST_ASSERT_NOT_NULL(f);
    fputs(out, f);
    fclose(f);
    free(out);
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = (int)UNITY_END();
    exit(failures == 0 ? 0 : 1);
}
