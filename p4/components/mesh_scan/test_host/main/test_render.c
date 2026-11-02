#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "unity.h"
#include "unity_test_runner.h"
#include "render.h"
#include "sample_result.h"

#define CHUNK_MAX 2048

typedef struct {
    char *data;
    size_t len;
    size_t flushes;
    size_t longest;
    size_t fail_at; /**< flush number that fails with ESP_FAIL, 0 = never */
} sink_t;

static esp_err_t sink_flush(void *ctx, const char *data, size_t len)
{
    sink_t *s = ctx;

    s->flushes++;
    if (s->fail_at && s->flushes == s->fail_at) {
        return ESP_FAIL;
    }
    if (len > s->longest) {
        s->longest = len;
    }
    s->data = realloc(s->data, s->len + len + 1);
    memcpy(s->data + s->len, data, len);
    s->len += len;
    s->data[s->len] = '\0';
    return ESP_OK;
}

static void sink_free(sink_t *s)
{
    free(s->data);
    memset(s, 0, sizeof(*s));
}

static mesh_scan_view_t done_view(const mesh_scan_result_t *result)
{
    return (mesh_scan_view_t) {
        .state = MESH_SCAN_DONE, .age_s = 42, .result = result
    };
}

/** Render into `s` and return the text; the caller frees it with sink_free. */
static const char *render(const mesh_scan_view_t *view, sink_t *s)
{
    memset(s, 0, sizeof(*s));
    TEST_ASSERT_EQUAL(ESP_OK, mesh_scan_render_view(view, sink_flush, s));
    TEST_ASSERT_NOT_NULL(s->data);
    return s->data;
}

static void assert_contains(const char *body, const char *part)
{
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(body, part), part);
}

static void assert_json(const char *body)
{
    cJSON *root = cJSON_Parse(body);

    TEST_ASSERT_NOT_NULL_MESSAGE(root, body);
    cJSON_Delete(root);
}

static const char *const LEADER_HEAD =
    "{\"rloc16\":9216,\"router_id\":9,\"ext_address\":\"d61a7c03f4e2b918\",\"version\":5,\"leader\":true,"
    "\"this_device\":false,\"border_router\":false,"
    "\"links\":[{\"router_id\":3,\"lq_in\":2},{\"router_id\":25,\"lq_in\":1},{\"router_id\":34,\"lq_in\":3}],"
    "\"addresses\":[\"fdde:ad00:beef::ff:fe00:2400\",\"fdde:ad00:beef:0:1c2d:3e4f:5a6b:7c8d\"],"
    "\"responded\":true,\"children_detail\":true,\"children\":[";

TEST_CASE("On idle: writes {\"state\":\"idle\"} only", "[mesh_scan]")
{
    const mesh_scan_view_t view = { .state = MESH_SCAN_IDLE, .age_s = 7, .routers = 3 };
    sink_t s;

    TEST_ASSERT_EQUAL_STRING("{\"state\":\"idle\"}", render(&view, &s));
    sink_free(&s);
}

TEST_CASE("On running before discover ends: writes progress {\"routers\":0,\"queried\":0}", "[mesh_scan]")
{
    const mesh_scan_view_t view = { .state = MESH_SCAN_RUNNING }; // result stays NULL: it is never read
    sink_t s;

    TEST_ASSERT_EQUAL_STRING("{\"state\":\"running\",\"progress\":{\"routers\":0,\"queried\":0}}", render(&view, &s));
    sink_free(&s);
}

TEST_CASE("On running during queries: writes progress with routers = discovered count and queried = routers finished",
          "[mesh_scan]")
{
    const mesh_scan_view_t view = { .state = MESH_SCAN_RUNNING, .routers = 5, .queried = 3 };
    sink_t s;

    TEST_ASSERT_EQUAL_STRING("{\"state\":\"running\",\"progress\":{\"routers\":5,\"queried\":3}}", render(&view, &s));
    sink_free(&s);
}

TEST_CASE("On done: writes age_s and the full result in the spec 3.3 field names and order", "[mesh_scan]")
{
    const mesh_scan_view_t view = done_view(sample_result());
    sink_t s;
    const char *out = render(&view, &s);

    TEST_ASSERT_EQUAL_INT(0, strncmp(out, "{\"state\":\"done\",\"age_s\":42,\"result\":{\"partial\":true,\"network\":{",
                                     strlen("{\"state\":\"done\",\"age_s\":42,\"result\":{\"partial\":true,\"network\":{")));
    assert_contains(out, "\"network\":{\"name\":\"MyThread\",\"channel\":15,\"partition_id\":4026531841,"
                    "\"leader_router_id\":9,\"mesh_local_prefix\":\"fdde:ad00:beef::/64\",\"this_rloc16\":34816,"
                    "\"omr_prefixes\":[\"2001:db8:4a2:1::/64\",\"fd7a:91c3:22e0:1::/64\"]},\"routers\":[");
    assert_contains(out, LEADER_HEAD);
    assert_contains(out, "{\"rloc16\":9217,\"ext_address\":\"1c88f02ab4d6e5a3\",\"mode\":{\"rx_on_when_idle\":false,"
                    "\"full_thread_device\":false,\"full_network_data\":false},\"lq_in\":3,\"timeout_s\":240,"
                    "\"age_s\":12,\"connected_s\":86400,\"rssi_avg\":-61,\"rssi_last\":-63,\"link_margin_db\":39,"
                    "\"frame_error_pct\":0.4,\"message_error_pct\":0.0,\"version\":4,\"csl\":false,"
                    "\"addresses\":[\"2001:db8:4a2:1:2be4:6c19:a0f7:3381\"]}");
    assert_contains(out, "],\"srp_hosts\":[{\"name\":\"kitchen-plug\","
                    "\"addresses\":[\"2001:db8:4a2:1:2be4:6c19:a0f7:3381\"],"
                    "\"services\":[{\"instance\":\"kitchen-plug\",\"type\":\"_matter._tcp\"}]},");
    // Routers come in router ID order: 3, 9, 17, 25, 34, 50.
    const char *ids[] = { "\"router_id\":3,\"ext", "\"router_id\":9,\"ext", "\"router_id\":17,\"ext", "{\"router_id\":25,\"rloc16\"",
                          "\"router_id\":34,\"ext", "\"router_id\":50,\"ext"
                        };
    const char *prev = out;

    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        const char *at = strstr(out, ids[i]);

        TEST_ASSERT_NOT_NULL_MESSAGE(at, ids[i]);
        TEST_ASSERT_TRUE_MESSAGE(at > prev, ids[i]);
        prev = at;
    }
    assert_json(out);
    sink_free(&s);
}

TEST_CASE("On done: writes links only for lq_in > 0", "[mesh_scan]")
{
    const mesh_scan_view_t view = done_view(sample_result());
    sink_t s;
    const char *out = render(&view, &s);

    // The leader has lq_in 0 from router 17, so exactly the three links of LEADER_HEAD.
    assert_contains(out, LEADER_HEAD);
    TEST_ASSERT_NULL(strstr(out, "\"lq_in\":0"));
    sink_free(&s);
}

TEST_CASE("On done: writes a stub router as router_id, rloc16, responded:false and nothing else", "[mesh_scan]")
{
    const mesh_scan_view_t view = done_view(sample_result());
    sink_t s;
    const char *out = render(&view, &s);

    assert_contains(out, "},{\"router_id\":25,\"rloc16\":25600,\"responded\":false},{");
    sink_free(&s);
}

TEST_CASE("On done: omits the child-table fields of children under children_detail:false", "[mesh_scan]")
{
    const mesh_scan_view_t view = done_view(sample_result());
    sink_t s;
    const char *out = render(&view, &s);

    assert_contains(out, "\"responded\":true,\"children_detail\":false,\"children\":[{\"rloc16\":17409,"
                    "\"mode\":{\"rx_on_when_idle\":false,\"full_thread_device\":false,\"full_network_data\":false},"
                    "\"lq_in\":3,\"addresses\":[\"2001:db8:4a2:1::4401\"]}]");
    sink_free(&s);
}

TEST_CASE("On done: writes rloc16 and partition_id as unsigned numbers, ext addresses as 16 lower-case hex digits",
          "[mesh_scan]")
{
    const mesh_scan_view_t view = done_view(sample_result());
    sink_t s;
    const char *out = render(&view, &s);

    assert_contains(out, "\"this_rloc16\":34816");             // 0x8800 is negative as int16_t
    assert_contains(out, "\"partition_id\":4026531841");       // 0xf0000001 is negative as int32_t
    assert_contains(out, "\"rloc16\":51200,\"router_id\":50"); // 0xc800
    assert_contains(out, "\"ext_address\":\"00000000000000a1\"");
    assert_contains(out, "\"ext_address\":\"fedcba9876543210\"");
    TEST_ASSERT_NULL(strstr(out, "\"rloc16\":-"));
    sink_free(&s);
}

TEST_CASE("On done: writes IPv6 addresses in RFC 5952 short form and prefixes as \"<addr>/64\"", "[mesh_scan]")
{
    mesh_scan_result_t *result = sample_result();
    const mesh_scan_view_t view = done_view(result);
    sink_t s;
    const char *out = render(&view, &s);

    assert_contains(out, "\"fdde:ad00:beef::ff:fe00:2400\"");             // the longest run of zero groups
    assert_contains(out, "\"fdde:ad00:beef:0:1c2d:3e4f:5a6b:7c8d\"");     // a single zero group stays
    assert_contains(out, "\"fdde:ad00:beef::ff:fe00:c00\"");              // no leading zeros, lower case
    assert_contains(out, "\"2001:db8:4a2:1::c01\"");
    assert_contains(out, "\"mesh_local_prefix\":\"fdde:ad00:beef::/64\"");
    assert_contains(out, "\"2001:db8:4a2:1::/64\"");
    assert_contains(out, "\"::\"");                                       // all zero
    sink_free(&s);

    // Two runs of the same length: the first one is compressed.
    const uint8_t tie[16] = { 0, 1, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 3, 0, 4 };

    memcpy(result->srp[0].addrs[0], tie, sizeof(tie));
    out = render(&view, &s);
    assert_contains(out, "\"1::2:0:0:3:4\"");
    sink_free(&s);
}

TEST_CASE("On done: writes error rates as percent with one decimal", "[mesh_scan]")
{
    mesh_scan_result_t *result = sample_result();
    const mesh_scan_view_t view = done_view(result);
    mesh_scan_child_t *c = &result->routers[9].children[0];
    sink_t s;

    c->frame_err = 0xffff;
    c->msg_err = 0x0666; // 2.4994 %
    const char *out = render(&view, &s);

    assert_contains(out, "\"frame_error_pct\":100.0,\"message_error_pct\":2.5,");
    assert_contains(out, "\"frame_error_pct\":0.4,\"message_error_pct\":0.0,");
    sink_free(&s);
}

TEST_CASE("On failed: writes error (\"busy\" | \"stopped\" | \"no_memory\") and age_s, no result", "[mesh_scan]")
{
    const struct {
        mesh_scan_error_t error;
        const char *json;
    } cases[] = {
        { MESH_SCAN_ERR_BUSY, "{\"state\":\"failed\",\"error\":\"busy\",\"age_s\":9}" },
        { MESH_SCAN_ERR_STOPPED, "{\"state\":\"failed\",\"error\":\"stopped\",\"age_s\":9}" },
        { MESH_SCAN_ERR_NO_MEMORY, "{\"state\":\"failed\",\"error\":\"no_memory\",\"age_s\":9}" },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const mesh_scan_view_t view = { .state = MESH_SCAN_FAILED, .error = cases[i].error, .age_s = 9 };
        sink_t s;

        TEST_ASSERT_EQUAL_STRING(cases[i].json, render(&view, &s));
        sink_free(&s);
    }
}

TEST_CASE("On a string with quotes, backslashes, control bytes or non-ASCII: escapes it so the output parses as JSON "
          "and round-trips the text", "[mesh_scan]")
{
    mesh_scan_result_t *result = sample_result();
    const mesh_scan_view_t view = done_view(result);
    sink_t s;
    const char *out = render(&view, &s);

    // The sample's second SRP host is named a"b\<c>.
    assert_contains(out, "\"name\":\"a\\\"b\\\\<c>\"");
    sink_free(&s);

    const char *tricky = "q\"b\\\x01\x1f\t\n/\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80";

    strcpy(result->srp[1].name, tricky);
    strcpy(result->network.name, tricky);
    out = render(&view, &s);
    cJSON *root = cJSON_Parse(out);

    TEST_ASSERT_NOT_NULL_MESSAGE(root, out);
    cJSON *res = cJSON_GetObjectItem(root, "result");
    cJSON *host = cJSON_GetArrayItem(cJSON_GetObjectItem(res, "srp_hosts"), 1);

    TEST_ASSERT_EQUAL_STRING(tricky, cJSON_GetObjectItem(host, "name")->valuestring);
    TEST_ASSERT_EQUAL_STRING(tricky, cJSON_GetObjectItem(cJSON_GetObjectItem(res, "network"), "name")->valuestring);
    cJSON_Delete(root);
    sink_free(&s);

    // Bytes that are not UTF-8 must not make the document invalid: they come out as U+FFFD.
    strcpy(result->srp[1].name, "x\xff\xc3y\xed\xa0\x80z");
    out = render(&view, &s);
    root = cJSON_Parse(out);
    TEST_ASSERT_NOT_NULL_MESSAGE(root, out);
    res = cJSON_GetObjectItem(root, "result");
    host = cJSON_GetArrayItem(cJSON_GetObjectItem(res, "srp_hosts"), 1);
    TEST_ASSERT_EQUAL_STRING("x\xef\xbf\xbd\xef\xbf\xbdy\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbdz",
                             cJSON_GetObjectItem(host, "name")->valuestring);
    cJSON_Delete(root);
    sink_free(&s);
}

TEST_CASE("On a result larger than one chunk: calls the sink with chunks of at most 2048 bytes whose concatenation is "
          "the whole document", "[mesh_scan]")
{
    const mesh_scan_view_t view = done_view(sample_result());
    sink_t s = { 0 };

    TEST_ASSERT_EQUAL(ESP_OK, mesh_scan_render_view(&view, sink_flush, &s));
    TEST_ASSERT_GREATER_THAN(2048, s.len);
    TEST_ASSERT_GREATER_THAN(1, s.flushes);
    TEST_ASSERT_LESS_OR_EQUAL(CHUNK_MAX, s.longest);
    TEST_ASSERT_EQUAL(strlen(s.data), s.len);
    assert_json(s.data);
    assert_contains(s.data, LEADER_HEAD);
    sink_free(&s);
}

TEST_CASE("On the sink failing: stops and returns its error", "[mesh_scan]")
{
    const mesh_scan_view_t view = done_view(sample_result());
    sink_t s = { .fail_at = 2 };

    TEST_ASSERT_EQUAL(ESP_FAIL, mesh_scan_render_view(&view, sink_flush, &s));
    TEST_ASSERT_EQUAL(2, s.flushes);
    sink_free(&s);
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = (int)UNITY_END();
    exit(failures == 0 ? 0 : 1);
}
