#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "unity.h"
#include "unity_test_runner.h"
#include "render.h"
#include "srp.h"
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

    TEST_ASSERT_EQUAL_STRING("{\"state\":\"running\",\"progress\":{\"routers\":0,\"queried\":0,"
                             "\"children\":{\"total\":0,\"queried\":0},"
                             "\"info\":{\"total\":0,\"answered\":0,\"sleepy_waiting\":0,\"wait_s\":0}}}",
                             render(&view, &s));
    sink_free(&s);
}

TEST_CASE("On running during queries: writes progress with routers = discovered count and queried = routers finished",
          "[mesh_scan]")
{
    const mesh_scan_view_t view = { .state = MESH_SCAN_RUNNING, .routers = 5, .queried = 3 };
    sink_t s;

    TEST_ASSERT_EQUAL_STRING("{\"state\":\"running\",\"progress\":{\"routers\":5,\"queried\":3,"
                             "\"children\":{\"total\":0,\"queried\":0},"
                             "\"info\":{\"total\":0,\"answered\":0,\"sleepy_waiting\":0,\"wait_s\":0}}}",
                             render(&view, &s));
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
                    "\"addresses\":[\"2001:db8:4a2:1:2be4:6c19:a0f7:3381\"],\"info\":{\"state\":\"not_asked\"}}");
    assert_contains(out, "],\"srp_hosts\":[{\"name\":\"kitchen-plug\","
                    "\"addresses\":[\"2001:db8:4a2:1:2be4:6c19:a0f7:3381\"],"
                    "\"services\":[{\"instance\":\"kitchen-plug\",\"type\":\"_matter._tcp\",");
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

/* The sample with transports on: this board (router 34) reaches the leader (9) over 15.4 and TREL, router 50 over
 * 15.4 only. */
static mesh_scan_result_t *transport_result(void)
{
    mesh_scan_result_t *r = sample_result();

    r->has_transport = true;
    r->this_links[9] = (mesh_scan_transport_t) {
        .valid = true, .pref_154 = 230, .pref_trel = 55
    };
    r->this_links[50] = (mesh_scan_transport_t) {
        .valid = true, .pref_154 = 255, .pref_trel = MESH_SCAN_PREF_NONE
    };
    r->this_links[3] = (mesh_scan_transport_t) {
        .valid = true, .pref_154 = 200, .pref_trel = 200
    }; // no link from 34
    return r;
}

TEST_CASE("On this board's router with TREL built: each link has transport with both preferences", "[mesh_scan]")
{
    const mesh_scan_view_t view = done_view(transport_result());
    sink_t s;
    const char *out = render(&view, &s);

    assert_contains(out, "\"links\":[{\"router_id\":9,\"lq_in\":3,\"transport\":{\"ieee802154\":230,\"trel\":55}},");
    assert_json(out);
    sink_free(&s);
}

TEST_CASE("On this board's router with TREL built: a radio the neighbour does not use is null", "[mesh_scan]")
{
    const mesh_scan_view_t view = done_view(transport_result());
    sink_t s;
    const char *out = render(&view, &s);

    assert_contains(out, "{\"router_id\":50,\"lq_in\":1,\"transport\":{\"ieee802154\":255,\"trel\":null}}]");
    sink_free(&s);
}

TEST_CASE("On this board's link missing from its diagnostic answer: written from the snapshot", "[mesh_scan]")
{
    mesh_scan_result_t *r = transport_result();

    r->this_links[3].lq_in = 2; // linked by the time of the snapshot, after this board answered
    const mesh_scan_view_t view = done_view(r);
    sink_t s;
    const char *out = render(&view, &s);

    assert_contains(out, "\"links\":[{\"router_id\":3,\"lq_in\":2,\"transport\":{\"ieee802154\":200,\"trel\":200}},");
    assert_json(out);
    sink_free(&s);
}

TEST_CASE("On this board's link in both its diagnostic answer and the snapshot: the answer's link quality", "[mesh_scan]")
{
    mesh_scan_result_t *r = transport_result();

    r->this_links[9].lq_in = 1; // the snapshot disagrees with the answer's 3
    const mesh_scan_view_t view = done_view(r);
    sink_t s;
    const char *out = render(&view, &s);

    assert_contains(out, "{\"router_id\":9,\"lq_in\":3,\"transport\":{\"ieee802154\":230,\"trel\":55}}");
    sink_free(&s);
}

TEST_CASE("On another router's links: no transport key", "[mesh_scan]")
{
    const mesh_scan_view_t view = done_view(transport_result());
    sink_t s;
    const char *out = render(&view, &s);

    int keys = 0;

    for (const char *at = strstr(out, "\"transport\""); at; at = strstr(at + 1, "\"transport\"")) {
        keys++;
    }
    assert_contains(out, LEADER_HEAD); // the leader's links are written as before
    TEST_ASSERT_EQUAL(2, keys);        // this board's two links only
    sink_free(&s);
}

TEST_CASE("On a build without TREL (has_transport false): no transport key anywhere", "[mesh_scan]")
{
    mesh_scan_result_t *r = transport_result();

    r->has_transport = false;
    const mesh_scan_view_t view = done_view(r);
    sink_t s;
    const char *out = render(&view, &s);

    TEST_ASSERT_NULL(strstr(out, "\"transport\""));
    sink_free(&s);
}

TEST_CASE("On done: writes a stub router as router_id, rloc16, responded:false and info not_asked", "[mesh_scan]")
{
    const mesh_scan_view_t view = done_view(sample_result());
    sink_t s;
    const char *out = render(&view, &s);

    assert_contains(out, "},{\"router_id\":25,\"rloc16\":25600,\"responded\":false,\"info\":{\"state\":\"not_asked\"}},{");
    sink_free(&s);
}

TEST_CASE("On done: omits the child-table fields of children under children_detail:false", "[mesh_scan]")
{
    const mesh_scan_view_t view = done_view(sample_result());
    sink_t s;
    const char *out = render(&view, &s);

    assert_contains(out, "\"responded\":true,\"children_detail\":false,\"children\":[{\"rloc16\":17409,"
                    "\"mode\":{\"rx_on_when_idle\":false,\"full_thread_device\":false,\"full_network_data\":false},"
                    "\"lq_in\":3,\"addresses\":[\"2001:db8:4a2:1::4401\"],\"info\":{\"state\":\"not_asked\"}}]");
    sink_free(&s);
}

TEST_CASE("On done: omits the child-table fields of a child the child table did not list, keeps its sibling's",
          "[mesh_scan]")
{
    mesh_scan_result_t *result = sample_result();
    mesh_scan_child_t *missing = &result->routers[3].children[1]; // 0x0c02

    memset(missing->ext, 0, sizeof(missing->ext));
    missing->has_ext = false;
    const mesh_scan_view_t view = done_view(result);
    sink_t s;
    const char *out = render(&view, &s);

    assert_contains(out, "{\"rloc16\":3074,\"mode\":{\"rx_on_when_idle\":false,\"full_thread_device\":false,"
                    "\"full_network_data\":false},\"lq_in\":3,\"addresses\":[\"2001:db8:4a2:1::c02\"],\"info\":{\"state\":\"not_asked\"}}");
    assert_contains(out, "{\"rloc16\":3073,\"ext_address\":\"3000000000000001\",");
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

TEST_CASE("On done: omits the version key of a router whose version is unknown", "[mesh_scan]")
{
    mesh_scan_result_t *result = sample_result();
    const mesh_scan_view_t view = done_view(result);
    sink_t s;

    result->routers[9].has_version = false;
    const char *out = render(&view, &s);

    assert_contains(out, "\"router_id\":9,\"ext_address\":\"d61a7c03f4e2b918\",\"leader\":true,");
    assert_contains(out, "\"router_id\":3,\"ext_address\":\"00000000000000a1\",\"version\":5,\"leader\":");
    assert_json(out);
    sink_free(&s);
}

TEST_CASE("On done: omits frame_error_pct and message_error_pct of a child that does not track error rates",
          "[mesh_scan]")
{
    mesh_scan_result_t *result = sample_result();
    const mesh_scan_view_t view = done_view(result);
    sink_t s;

    result->routers[9].children[0].has_err_rate = false;
    const char *out = render(&view, &s);

    assert_contains(out, "{\"rloc16\":9217,\"ext_address\":\"1c88f02ab4d6e5a3\",\"mode\":{\"rx_on_when_idle\":false,"
                    "\"full_thread_device\":false,\"full_network_data\":false},\"lq_in\":3,\"timeout_s\":240,"
                    "\"age_s\":12,\"connected_s\":86400,\"rssi_avg\":-61,\"rssi_last\":-63,\"link_margin_db\":39,"
                    "\"version\":4,\"csl\":false,");
    assert_json(out);
    sink_free(&s);
}

TEST_CASE("On running in the child address step: writes progress {\"routers\",\"queried\",\"children\":"
          "{\"total\",\"queried\"}}", "[mesh_scan]")
{
    const mesh_scan_view_t view = {
        .state = MESH_SCAN_RUNNING, .routers = 5, .queried = 5, .children_total = 7, .children_queried = 2
    };
    sink_t s;

    TEST_ASSERT_EQUAL_STRING("{\"state\":\"running\",\"progress\":{\"routers\":5,\"queried\":5,"
                             "\"children\":{\"total\":7,\"queried\":2},"
                             "\"info\":{\"total\":0,\"answered\":0,\"sleepy_waiting\":0,\"wait_s\":0}}}",
                             render(&view, &s));
    sink_free(&s);
}

TEST_CASE("On running in the info phase: progress gains info with total, answered, sleepy_waiting and wait_s",
          "[mesh_scan]")
{
    const mesh_scan_view_t view = {
        .state = MESH_SCAN_RUNNING, .routers = 5, .queried = 5, .children_total = 7, .children_queried = 7,
        .info_total = 12, .info_answered = 9, .sleepy_waiting = 2, .sleepy_wait_s = 231
    };
    sink_t s;

    TEST_ASSERT_EQUAL_STRING("{\"state\":\"running\",\"progress\":{\"routers\":5,\"queried\":5,"
                             "\"children\":{\"total\":7,\"queried\":7},"
                             "\"info\":{\"total\":12,\"answered\":9,\"sleepy_waiting\":2,\"wait_s\":231}}}",
                             render(&view, &s));
    sink_free(&s);
}

static void set_answered(mesh_scan_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->state = MESH_SCAN_INFO_ANSWERED;
    info->has_eui64 = true;
    memcpy(info->eui64, "\x8c\x73\xda\xff\xfe\x95\x2c\xfb", 8);
    info->has_oui = true;
    info->oui = 0x0a1b2c;
    strcpy(info->vendor_name, "Home Assistant");
    strcpy(info->vendor_model, "OpenThread Border Router");
    strcpy(info->vendor_sw_version, "20261104-bb169ca");
    strcpy(info->thread_stack_version, "OPENTHREAD/cb18e0f; POSIX; Oct  6 2026 20:28:48");
    strcpy(info->vendor_app_url, "https://example.com/otbr");
}

/** The info object of router `id`. */
static cJSON *router_info_of(cJSON *root, int id)
{
    cJSON *routers = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "result"), "routers");
    cJSON *router = NULL;

    cJSON_ArrayForEach(router, routers) {
        if (cJSON_GetObjectItem(router, "router_id")->valueint == id) {
            return cJSON_GetObjectItem(router, "info");
        }
    }
    TEST_FAIL_MESSAGE("router not in the output");
    return NULL;
}

TEST_CASE("On a router with info answered: writes info with state, eui64, the strings and vendor_oui", "[mesh_scan]")
{
    mesh_scan_result_t *result = sample_result();

    set_answered(&result->routers[3].info);
    const mesh_scan_view_t view = done_view(result);
    sink_t s;
    const char *out = render(&view, &s);

    assert_contains(out, "],\"info\":{\"state\":\"answered\",\"eui64\":\"8c73dafffe952cfb\","
                    "\"vendor_name\":\"Home Assistant\",\"vendor_model\":\"OpenThread Border Router\","
                    "\"vendor_sw_version\":\"20261104-bb169ca\","
                    "\"thread_stack_version\":\"OPENTHREAD/cb18e0f; POSIX; Oct  6 2026 20:28:48\","
                    "\"vendor_app_url\":\"https://example.com/otbr\",\"vendor_oui\":\"0a1b2c\"}}");
    assert_json(out);
    sink_free(&s);
}

TEST_CASE("On a child with info answered: writes info after addresses", "[mesh_scan]")
{
    mesh_scan_result_t *result = sample_result();

    set_answered(&result->routers[3].children[0].info);
    const mesh_scan_view_t view = done_view(result);
    sink_t s;
    const char *out = render(&view, &s);

    assert_contains(out, "\"addresses\":[\"2001:db8:4a2:1::c01\"],\"info\":{\"state\":\"answered\",");
    assert_json(out);
    sink_free(&s);
}

TEST_CASE("On info answered with fields absent: leaves the absent fields out", "[mesh_scan]")
{
    mesh_scan_result_t *result = sample_result();

    memset(&result->routers[3].info, 0, sizeof(result->routers[3].info));
    result->routers[3].info.state = MESH_SCAN_INFO_ANSWERED;
    strcpy(result->routers[3].info.thread_stack_version, "SL-OPENTHREAD/2.4.2.0_GitHub-7074a43e4; EFR32; Oct 30 2024");
    const mesh_scan_view_t view = done_view(result);
    sink_t s;
    const char *out = render(&view, &s);

    assert_contains(out, "\"info\":{\"state\":\"answered\",\"thread_stack_version\":"
                    "\"SL-OPENTHREAD/2.4.2.0_GitHub-7074a43e4; EFR32; Oct 30 2024\"}}");
    sink_free(&s);
}

TEST_CASE("On info no_answer, cut_short or not_asked: writes the state only", "[mesh_scan]")
{
    mesh_scan_result_t *result = sample_result();
    const struct {
        mesh_scan_info_state_t state;
        const char *name;
    } cases[] = {
        { MESH_SCAN_INFO_NO_ANSWER, "no_answer" },
        { MESH_SCAN_INFO_CUT_SHORT, "cut_short" },
        { MESH_SCAN_INFO_NOT_ASKED, "not_asked" },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        set_answered(&result->routers[3].info); // fields left over from an earlier state must not show
        result->routers[3].info.state = cases[i].state;
        const mesh_scan_view_t view = done_view(result);
        sink_t s;
        const char *out = render(&view, &s);
        char want[64];

        snprintf(want, sizeof(want), "],\"info\":{\"state\":\"%s\"}}", cases[i].name);
        assert_contains(out, want);
        assert_json(out);
        sink_free(&s);
    }
}

TEST_CASE("On this board's router in the sample: writes its local info answered", "[mesh_scan]")
{
    const mesh_scan_view_t view = done_view(sample_result());
    sink_t s;
    const char *out = render(&view, &s);
    cJSON *root = cJSON_Parse(out);

    TEST_ASSERT_NOT_NULL_MESSAGE(root, out);
    cJSON *got = router_info_of(root, 34);

    TEST_ASSERT_EQUAL_STRING("answered", cJSON_GetObjectItem(got, "state")->valuestring);
    TEST_ASSERT_EQUAL_STRING("0a1b2c3d4e5f6071", cJSON_GetObjectItem(got, "eui64")->valuestring);
    TEST_ASSERT_EQUAL_STRING("ESP32-P4-WIFI6-POE-ETH", cJSON_GetObjectItem(got, "vendor_name")->valuestring);
    cJSON_Delete(root);
    sink_free(&s);
}

TEST_CASE("On a stub router: writes info not_asked after responded", "[mesh_scan]")
{
    const mesh_scan_view_t view = done_view(sample_result());
    sink_t s;
    const char *out = render(&view, &s);

    assert_contains(out, "{\"router_id\":25,\"rloc16\":25600,\"responded\":false,"
                    "\"info\":{\"state\":\"not_asked\"}}");
    sink_free(&s);
}

TEST_CASE("On info strings with a quote, a control byte or invalid UTF-8: escapes them so the output parses",
          "[mesh_scan]")
{
    mesh_scan_result_t *result = sample_result();
    mesh_scan_info_t *info = &result->routers[3].info;

    set_answered(info);
    strcpy(info->vendor_name, "<b>x\"y\\z</b>");
    strcpy(info->vendor_model, "a\x01\x1f" "b");
    strcpy(info->vendor_sw_version, "v\xff\xc3");
    const mesh_scan_view_t view = done_view(result);
    sink_t s;
    const char *out = render(&view, &s);
    cJSON *root = cJSON_Parse(out);

    TEST_ASSERT_NOT_NULL_MESSAGE(root, out);
    cJSON *got = router_info_of(root, 3);

    TEST_ASSERT_EQUAL_STRING("<b>x\"y\\z</b>", cJSON_GetObjectItem(got, "vendor_name")->valuestring);
    TEST_ASSERT_EQUAL_STRING("a\x01\x1f" "b", cJSON_GetObjectItem(got, "vendor_model")->valuestring);
    TEST_ASSERT_EQUAL_STRING("v\xef\xbf\xbd\xef\xbf\xbd", cJSON_GetObjectItem(got, "vendor_sw_version")->valuestring);
    cJSON_Delete(root);
    sink_free(&s);
}

TEST_CASE("On info strings filling their arrays: writes them whole", "[mesh_scan]")
{
    mesh_scan_result_t *result = sample_result();
    mesh_scan_info_t *info = &result->routers[3].info;

    set_answered(info);
    memset(info->vendor_name, 'n', sizeof(info->vendor_name) - 1);
    memset(info->thread_stack_version, 's', sizeof(info->thread_stack_version) - 1);
    memset(info->vendor_app_url, 'u', sizeof(info->vendor_app_url) - 1);
    const mesh_scan_view_t view = done_view(result);
    sink_t s;
    const char *out = render(&view, &s);
    cJSON *root = cJSON_Parse(out);

    TEST_ASSERT_NOT_NULL_MESSAGE(root, out);
    cJSON *got = router_info_of(root, 3);

    TEST_ASSERT_EQUAL(32, strlen(cJSON_GetObjectItem(got, "vendor_name")->valuestring));
    TEST_ASSERT_EQUAL(64, strlen(cJSON_GetObjectItem(got, "thread_stack_version")->valuestring));
    TEST_ASSERT_EQUAL(96, strlen(cJSON_GetObjectItem(got, "vendor_app_url")->valuestring));
    cJSON_Delete(root);
    sink_free(&s);
}

TEST_CASE("On a child with addresses_reported false: writes \"addresses_reported\":false after \"addresses\"",
          "[mesh_scan]")
{
    mesh_scan_result_t *result = sample_result();
    mesh_scan_child_t *c = &result->routers[9].children[1]; // 0x2402, rx-on

    c->n_addrs = 0;
    c->addresses_reported = false;
    const mesh_scan_view_t view = done_view(result);
    sink_t s;
    const char *out = render(&view, &s);

    assert_contains(out, "\"csl\":false,\"addresses\":[],\"addresses_reported\":false,\"info\":{\"state\":\"not_asked\"}},{\"rloc16\":9219,");
    const char *first = strstr(out, "addresses_reported");
    TEST_ASSERT_NULL(strstr(first + 1, "addresses_reported")); // the other children leave the key out
    assert_json(out);
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

// SRP servers, hosts and services

static const char *const LOCAL_HOST =
    "{\"name\":\"kitchen-plug\",\"addresses\":[\"2001:db8:4a2:1:2be4:6c19:a0f7:3381\"],"
    "\"services\":[{\"instance\":\"kitchen-plug\",\"type\":\"_matter._tcp\",\"port\":5540,\"source\":34816,"
    "\"subtypes\":[\"_i4f2\",\"_S3\"],\"txt\":\"A2E9MQ==\",\"txt_truncated\":false}],"
    "\"source\":34816,\"lease_s\":7200,\"key_lease_s\":1209600}";

TEST_CASE("On srp_servers: each server has rloc16, address, port, this_board, state string and hosts, after srp_hosts",
          "[mesh_scan]")
{
    const mesh_scan_view_t view = done_view(sample_result());
    sink_t s;
    const char *out = render(&view, &s);

    assert_json(out);
    assert_contains(out, "\"srp_servers\":["
                    "{\"rloc16\":34816,\"address\":\"fdde:ad00:beef::ff:fe00:8800\",\"port\":53554,\"this_board\":true,"
                    "\"state\":\"answered\",\"hosts\":1},"
                    "{\"rloc16\":9216,\"address\":\"fd7a:91c3:22e0:1::53\",\"port\":53,\"this_board\":false,"
                    "\"state\":\"no_answer\",\"hosts\":0},"
                    "{\"rloc16\":51200,\"address\":\"2001:db8:4a2:1::53\",\"port\":5353,\"this_board\":false,"
                    "\"state\":\"cut_short\",\"hosts\":2}]}}");
    TEST_ASSERT_TRUE(strstr(out, "\"srp_hosts\":[") < strstr(out, "\"srp_servers\":["));
    sink_free(&s);
}

TEST_CASE("On a host from this board: writes source, lease_s and key_lease_s after services, no ttl_s", "[mesh_scan]")
{
    const mesh_scan_view_t view = done_view(sample_result());
    sink_t s;
    const char *out = render(&view, &s);

    assert_contains(out, LOCAL_HOST);
    sink_free(&s);
}

TEST_CASE("On a host from another server: writes source and ttl_s after services, no lease fields", "[mesh_scan]")
{
    const mesh_scan_view_t view = done_view(sample_result());
    sink_t s;
    const char *out = render(&view, &s);

    assert_contains(out, "\"services\":[],\"source\":9216,\"ttl_s\":1800}");
    assert_contains(out, "\"txt_truncated\":true}],\"source\":51200,\"ttl_s\":600}");
    const char *remote = strstr(out, "\"name\":\"ha-sensor\"");
    TEST_ASSERT_NOT_NULL(remote);
    TEST_ASSERT_NULL(strstr(remote, "lease_s"));
    sink_free(&s);
}

TEST_CASE("On a service: writes port, source, subtypes array, txt base64 and txt_truncated after type", "[mesh_scan]")
{
    const mesh_scan_view_t view = done_view(sample_result());
    sink_t s;
    const char *out = render(&view, &s);

    assert_contains(out, "\"type\":\"_matter._tcp\",\"port\":5540,\"source\":34816,\"subtypes\":[\"_i4f2\",\"_S3\"],"
                    "\"txt\":\"A2E9MQ==\",\"txt_truncated\":false}");
    sink_free(&s);
}

TEST_CASE("On a service without TXT: writes \"txt\":\"\" and an empty subtypes array", "[mesh_scan]")
{
    const mesh_scan_view_t view = done_view(sample_result());
    sink_t s;
    const char *out = render(&view, &s);

    assert_contains(out, "\"type\":\"_http._tcp\",\"port\":80,\"source\":51200,\"subtypes\":[],\"txt\":\"\","
                    "\"txt_truncated\":true}");
    sink_free(&s);
}

TEST_CASE("On a service with has_txt and zero length: writes \"txt\":\"\"", "[mesh_scan]")
{
    mesh_scan_result_t *result = sample_result();
    const mesh_scan_view_t view = done_view(result);
    sink_t s;

    result->srp[0].services[0].txt_len = 0;
    const char *out = render(&view, &s);

    assert_contains(out, "\"subtypes\":[\"_i4f2\",\"_S3\"],\"txt\":\"\",\"txt_truncated\":false}");
    sink_free(&s);
}

/** Render the first service of host 0 with these TXT bytes in the pool and return its "txt" value in `out`. */
static void txt_of(const uint8_t *bytes, uint16_t len, char *out, size_t out_size)
{
    mesh_scan_result_t *result = sample_result();
    const mesh_scan_view_t view = done_view(result);
    sink_t s;

    memcpy(result->pool + result->srp[0].services[0].txt_off, bytes, len);
    result->srp[0].services[0].txt_len = len;
    const char *body = render(&view, &s);
    const char *at = strstr(body, "\"txt\":\"");

    TEST_ASSERT_NOT_NULL(at);
    at += strlen("\"txt\":\"");
    const char *end = strchr(at, '"');

    TEST_ASSERT_NOT_NULL(end);
    TEST_ASSERT_LESS_THAN(out_size, (size_t)(end - at));
    memcpy(out, at, (size_t)(end - at));
    out[end - at] = '\0';
    sink_free(&s);
}

TEST_CASE("On a TXT of 1, 2 and 3 trailing bytes: writes standard base64 with == / = / no padding", "[mesh_scan]")
{
    const uint8_t bytes[] = { 0xff, 0xfe, 0xfd, 0xfc, 0xfb };
    char out[32];

    txt_of(bytes, 1, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("/w==", out);
    txt_of(bytes, 2, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("//4=", out);
    txt_of(bytes, 3, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("//79", out);
    txt_of(bytes, 4, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("//79/A==", out);
    txt_of(bytes, 5, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("//79/Ps=", out);
}

TEST_CASE("On a TXT long enough to cross chunks: writes it whole as valid JSON", "[mesh_scan]")
{
    mesh_scan_result_t *result = sample_result();
    const mesh_scan_view_t view = done_view(result);
    sink_t s;

    for (int i = 0; i < MESH_SCAN_TXT_MAX; i++) {
        result->pool[result->srp[0].services[0].txt_off + i] = (uint8_t)i;
    }
    result->srp[0].services[0].txt_len = MESH_SCAN_TXT_MAX;
    result->srp[0].services[0].n_sub = 0; // the labels sit behind the sample's TXT in the pool: overwritten here
    result->pool_used = MESH_SCAN_TXT_MAX;
    const char *out = render(&view, &s);

    assert_json(out);
    assert_contains(out, "\"txt\":\"AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8gISIjJCUmJygpKissLS4v");
    assert_contains(out, "9fb3+Pn6+/z9/v8=\",\"txt_truncated\"");
    sink_free(&s);
}

TEST_CASE("On a result without SRP data: writes \"srp_servers\":[] and the new host fields empty", "[mesh_scan]")
{
    static mesh_scan_result_t result; // ~100 KB: not on the test stack
    sink_t s;

    memset(&result, 0, sizeof(result));
    strcpy(result.srp[0].name, "x");
    strcpy(result.srp[0].services[0].instance, "i");
    strcpy(result.srp[0].services[0].type, "t");
    result.srp[0].n_services = 1;
    result.n_srp = 1;
    const mesh_scan_view_t view = done_view(&result);
    const char *out = render(&view, &s);

    assert_json(out);
    assert_contains(out, "\"srp_hosts\":[{\"name\":\"x\",\"addresses\":[],\"services\":[{\"instance\":\"i\","
                    "\"type\":\"t\",\"port\":0,\"source\":0,\"subtypes\":[],\"txt\":\"\",\"txt_truncated\":false}],"
                    "\"source\":0,\"ttl_s\":0}],\"srp_servers\":[]}}");
    sink_free(&s);
}
