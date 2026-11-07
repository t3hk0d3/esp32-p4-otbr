#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "update_json.h"

#define BASE "https://github.com/t3hk0d3/esp32-p4-otbr/releases"
#define RUNNING "20261105T120000Z-abc1234"
#define NEWER "20261106T120000Z-def5678"
#define RCP_OLD "20261105T120000Z-abc1234"
#define RCP_NEW "20261106T120000Z-def5678"

typedef struct {
    device_api_error_t error;
    bool app;
    bool rcp;
} decision_t;

static decision_t decide(self_update_cmp_t cmp, bool rcp_differs, bool has_rcp, bool force)
{
    decision_t d = {.app = true, .rcp = true}; // overwritten on every path

    d.error = device_api_update_decide(cmp, rcp_differs, has_rcp, force, &d.app, &d.rcp);
    return d;
}

static self_update_snapshot_t snapshot(self_update_state_t state, const char *tag, const char *app, const char *rcp)
{
    self_update_snapshot_t s;

    memset(&s, 0, sizeof(s));
    s.state = state;
    if (app) {
        s.has_release = true;
        strlcpy(s.release.tag, tag, sizeof(s.release.tag));
        strlcpy(s.release.app, app, sizeof(s.release.app));
        strlcpy(s.release.rcp, rcp, sizeof(s.release.rcp));
    }
    return s;
}

static const char *render(char *buf, size_t cap, const self_update_snapshot_t *s, bool rcp_known, const char *rcp)
{
    TEST_ASSERT_GREATER_THAN(0, device_api_update_json(buf, cap, s, BASE, RUNNING, rcp_known, rcp));
    TEST_ASSERT_EQUAL(strlen(buf), (size_t)device_api_update_json(buf, cap, s, BASE, RUNNING, rcp_known, rcp));
    return buf;
}

static void assert_has(const char *json, const char *part)
{
    if (!strstr(json, part)) {
        char msg[512];

        snprintf(msg, sizeof(msg), "\"%s\" not in %s", part, json);
        TEST_FAIL_MESSAGE(msg);
    }
}

/* ---- device_api_update_decide ---- */

TEST_CASE("NEWER, rcp differs: app and rcp; no error", "[device_api]")
{
    const decision_t d = decide(SELF_UPDATE_NEWER, true, true, false);

    TEST_ASSERT_EQUAL(0, d.error.http_status);
    TEST_ASSERT_TRUE(d.app);
    TEST_ASSERT_TRUE(d.rcp);
}

TEST_CASE("NEWER, rcp equal: app only", "[device_api]")
{
    const decision_t d = decide(SELF_UPDATE_NEWER, false, true, false);

    TEST_ASSERT_EQUAL(0, d.error.http_status);
    TEST_ASSERT_TRUE(d.app);
    TEST_ASSERT_FALSE(d.rcp);
}

TEST_CASE("SAME, rcp differs: rcp only", "[device_api]")
{
    const decision_t d = decide(SELF_UPDATE_SAME, true, true, false);

    TEST_ASSERT_EQUAL(0, d.error.http_status);
    TEST_ASSERT_FALSE(d.app);
    TEST_ASSERT_TRUE(d.rcp);
}

TEST_CASE("UNKNOWN: app", "[device_api]")
{
    const decision_t d = decide(SELF_UPDATE_UNKNOWN, false, true, false);

    TEST_ASSERT_EQUAL(0, d.error.http_status);
    TEST_ASSERT_TRUE(d.app);
    TEST_ASSERT_FALSE(d.rcp);
}

TEST_CASE("SAME or OLDER, rcp equal, no force: 409 up_to_date", "[device_api]")
{
    const self_update_cmp_t cmps[] = {SELF_UPDATE_SAME, SELF_UPDATE_OLDER};

    for (size_t i = 0; i < 2; i++) {
        const decision_t d = decide(cmps[i], false, true, false);

        TEST_ASSERT_EQUAL(409, d.error.http_status);
        TEST_ASSERT_EQUAL_STRING("up_to_date", d.error.code);
        TEST_ASSERT_NOT_NULL(d.error.detail);
        TEST_ASSERT_FALSE(d.app);
        TEST_ASSERT_FALSE(d.rcp);
    }
}

TEST_CASE("OLDER, force: app; rcp when has_rcp", "[device_api]")
{
    decision_t d = decide(SELF_UPDATE_OLDER, false, true, true);

    TEST_ASSERT_EQUAL(0, d.error.http_status);
    TEST_ASSERT_TRUE(d.app);
    TEST_ASSERT_TRUE(d.rcp);
    d = decide(SELF_UPDATE_OLDER, false, false, true);
    TEST_ASSERT_EQUAL(0, d.error.http_status);
    TEST_ASSERT_TRUE(d.app);
    TEST_ASSERT_FALSE(d.rcp);
}

TEST_CASE("SAME, force: app installed", "[device_api]")
{
    const decision_t d = decide(SELF_UPDATE_SAME, false, true, true);

    TEST_ASSERT_EQUAL(0, d.error.http_status);
    TEST_ASSERT_TRUE(d.app);
}

TEST_CASE("Release without rcp, rcp \"differs\": rcp not installed", "[device_api]")
{
    decision_t d = decide(SELF_UPDATE_NEWER, true, false, false);

    TEST_ASSERT_EQUAL(0, d.error.http_status);
    TEST_ASSERT_TRUE(d.app);
    TEST_ASSERT_FALSE(d.rcp);
    d = decide(SELF_UPDATE_SAME, true, false, false);
    TEST_ASSERT_EQUAL(409, d.error.http_status);
    TEST_ASSERT_EQUAL_STRING("up_to_date", d.error.code);
}

/* ---- device_api_update_json ---- */

TEST_CASE("Never checked: state never, release null, app null, available false", "[device_api]")
{
    char buf[1024];
    const self_update_snapshot_t s = snapshot(SELF_UPDATE_NEVER, NULL, NULL, NULL);

    render(buf, sizeof(buf), &s, true, RCP_OLD);
    assert_has(buf, "\"state\":\"never\"");
    assert_has(buf, "\"checked_age_s\":null");
    assert_has(buf, "\"error\":null");
    assert_has(buf, "\"release\":null");
    assert_has(buf, "\"app\":null");
    assert_has(buf, "\"rcp_differs\":false");
    assert_has(buf, "\"available\":false");
    assert_has(buf, "\"running\":{\"app\":\"" RUNNING "\",\"rcp\":\"" RCP_OLD "\"}");
}

TEST_CASE("Never checked: repo_url https://github.com/t3hk0d3/esp32-p4-otbr", "[device_api]")
{
    char buf[1024];
    const self_update_snapshot_t s = snapshot(SELF_UPDATE_NEVER, NULL, NULL, NULL);

    render(buf, sizeof(buf), &s, true, RCP_OLD);
    assert_has(buf, "\"repo_url\":\"https://github.com/t3hk0d3/esp32-p4-otbr\"");
}

TEST_CASE("Done, newer release with a tag: release.url <base>/tag/<tag>, app newer, available true", "[device_api]")
{
    char buf[1024];
    self_update_snapshot_t s = snapshot(SELF_UPDATE_DONE, "v0.4.0", NEWER, RCP_OLD);

    s.has_age = true;
    s.age_s = 42;
    render(buf, sizeof(buf), &s, true, RCP_OLD);
    TEST_ASSERT_EQUAL_STRING("{\"state\":\"done\",\"checked_age_s\":42,\"error\":null,"
                             "\"release\":{\"tag\":\"v0.4.0\",\"app\":\"" NEWER "\",\"rcp\":\"" RCP_OLD "\","
                             "\"url\":\"" BASE "/tag/v0.4.0\"},"
                             "\"repo_url\":\"https://github.com/t3hk0d3/esp32-p4-otbr\","
                             "\"running\":{\"app\":\"" RUNNING "\",\"rcp\":\"" RCP_OLD "\"},"
                             "\"app\":\"newer\",\"rcp_differs\":false,\"available\":true}", buf);
}

TEST_CASE("Done, same release, rcp differs: app same, rcp_differs true, available true", "[device_api]")
{
    char buf[1024];
    const self_update_snapshot_t s = snapshot(SELF_UPDATE_DONE, "v0.4.0", RUNNING, RCP_NEW);

    render(buf, sizeof(buf), &s, true, RCP_OLD);
    assert_has(buf, "\"app\":\"same\"");
    assert_has(buf, "\"rcp_differs\":true");
    assert_has(buf, "\"available\":true");
}

TEST_CASE("Done, same release, rcp equal: available false", "[device_api]")
{
    char buf[1024];
    const self_update_snapshot_t s = snapshot(SELF_UPDATE_DONE, "v0.4.0", RUNNING, RCP_OLD);

    render(buf, sizeof(buf), &s, true, RCP_OLD);
    assert_has(buf, "\"app\":\"same\"");
    assert_has(buf, "\"rcp_differs\":false");
    assert_has(buf, "\"available\":false");
}

TEST_CASE("Failed after an earlier success: state failed, error set, release still the earlier one", "[device_api]")
{
    char buf[1024];
    self_update_snapshot_t s = snapshot(SELF_UPDATE_FAILED, "v0.4.0", NEWER, RCP_OLD);

    s.has_age = true;
    s.age_s = 3;
    strlcpy(s.error, "connection refused (http://x/versions.txt)", sizeof(s.error));
    render(buf, sizeof(buf), &s, true, RCP_OLD);
    assert_has(buf, "\"state\":\"failed\"");
    assert_has(buf, "\"checked_age_s\":3");
    assert_has(buf, "\"error\":\"connection refused (http://x/versions.txt)\"");
    assert_has(buf, "\"release\":{\"tag\":\"v0.4.0\",\"app\":\"" NEWER "\"");
    assert_has(buf, "\"app\":\"newer\"");
}

TEST_CASE("Checking: state checking", "[device_api]")
{
    char buf[1024];
    const self_update_snapshot_t s = snapshot(SELF_UPDATE_CHECKING, NULL, NULL, NULL);

    render(buf, sizeof(buf), &s, true, RCP_OLD);
    assert_has(buf, "\"state\":\"checking\"");
}

TEST_CASE("Release without a tag: release.tag null, release.url <base>/latest", "[device_api]")
{
    char buf[1024];
    const self_update_snapshot_t s = snapshot(SELF_UPDATE_DONE, "", NEWER, "");

    render(buf, sizeof(buf), &s, true, RCP_OLD);
    assert_has(buf, "\"release\":{\"tag\":null,\"app\":\"" NEWER "\",\"rcp\":null,\"url\":\"" BASE "/latest\"}");
    assert_has(buf, "\"rcp_differs\":false"); // no c6_rcp line: nothing to compare
}

TEST_CASE("rcp unknown on the board: running.rcp null, rcp_differs true", "[device_api]")
{
    char buf[1024];
    const self_update_snapshot_t s = snapshot(SELF_UPDATE_DONE, "v0.4.0", RUNNING, RCP_OLD);

    render(buf, sizeof(buf), &s, false, RCP_OLD);
    assert_has(buf, "\"running\":{\"app\":\"" RUNNING "\",\"rcp\":null}");
    assert_has(buf, "\"rcp_differs\":true");
    assert_has(buf, "\"available\":true");
    render(buf, sizeof(buf), &s, true, "");
    assert_has(buf, "\"running\":{\"app\":\"" RUNNING "\",\"rcp\":null}");
    assert_has(buf, "\"rcp_differs\":true");
}

TEST_CASE("Older release: app older, available false", "[device_api]")
{
    char buf[1024];
    const self_update_snapshot_t s = snapshot(SELF_UPDATE_DONE, "v0.3.0", "20250101T000000Z-1111111", RCP_OLD);

    render(buf, sizeof(buf), &s, true, RCP_OLD);
    assert_has(buf, "\"app\":\"older\"");
    assert_has(buf, "\"available\":false");
}

TEST_CASE("Malformed release version: app unknown, available true", "[device_api]")
{
    char buf[1024];
    const self_update_snapshot_t s = snapshot(SELF_UPDATE_DONE, "v0.4.0", "garbage", RCP_OLD);

    render(buf, sizeof(buf), &s, true, RCP_OLD);
    assert_has(buf, "\"app\":\"unknown\"");
    assert_has(buf, "\"available\":true");
}

TEST_CASE("Buffer too small: -1", "[device_api]")
{
    char buf[64];
    const self_update_snapshot_t s = snapshot(SELF_UPDATE_DONE, "v0.4.0", NEWER, RCP_OLD);

    TEST_ASSERT_EQUAL(-1, device_api_update_json(buf, sizeof(buf), &s, BASE, RUNNING, true, RCP_OLD));
}

/* ---- device_api_update_reply_json ---- */

TEST_CASE("App installed, rcp failed: status ok, app installed, rcp failed, rebooting true", "[device_api]")
{
    char buf[256];
    const int len = device_api_update_reply_json(buf, sizeof(buf), true, "failed", "push failed", true);

    TEST_ASSERT_EQUAL(strlen(buf), (size_t)len);
    TEST_ASSERT_EQUAL_STRING("{\"status\":\"ok\",\"app\":\"installed\",\"rcp\":\"failed\",\"detail\":\"push failed\","
                             "\"rebooting\":true}", buf);
}

TEST_CASE("Rcp only: app unchanged, rebooting false", "[device_api]")
{
    char buf[256];

    TEST_ASSERT_GREATER_THAN(0, device_api_update_reply_json(buf, sizeof(buf), false, "installed", "x", false));
    TEST_ASSERT_EQUAL_STRING("{\"status\":\"ok\",\"app\":\"unchanged\",\"rcp\":\"installed\",\"detail\":\"x\","
                             "\"rebooting\":false}", buf);
}

TEST_CASE("Reply buffer too small: -1", "[device_api]")
{
    char buf[16];

    TEST_ASSERT_EQUAL(-1, device_api_update_reply_json(buf, sizeof(buf), true, "unchanged", "detail", true));
}
