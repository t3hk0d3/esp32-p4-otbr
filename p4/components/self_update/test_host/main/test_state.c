#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "self_update_priv.h"

#define APP "20261103T185311Z-e41c14c"
#define RCP "20261103T185115Z-e41c14c"
#define APP2 "20261104T101010Z-abcdef1"
#define VERSIONS_URL SELF_UPDATE_RELEASES_URL "/latest/download/versions.txt"

static struct {
    esp_err_t result;
    const char *body;
    const char *detail;
    char url[256];
    int calls;
    int64_t now_us;
} s_fake;

static esp_err_t fake_fetch(const char *url, char *buf, size_t cap, size_t *len, char *detail, size_t detail_cap)
{
    s_fake.calls++;
    snprintf(s_fake.url, sizeof(s_fake.url), "%s", url);
    if (s_fake.result != ESP_OK) {
        snprintf(detail, detail_cap, "%s", s_fake.detail);
        return s_fake.result;
    }
    const size_t n = strlen(s_fake.body);
    TEST_ASSERT_TRUE(n <= cap);
    memcpy(buf, s_fake.body, n);
    *len = n;
    return ESP_OK;
}

static int64_t fake_now(void)
{
    return s_fake.now_us;
}

static void setup(void)
{
    static const self_update_ops_t ops = {.fetch = fake_fetch, .now_us = fake_now};
    memset(&s_fake, 0, sizeof(s_fake));
    self_update_set_ops(&ops);
    self_update_reset();
}

static void serve_valid(void)
{
    s_fake.result = ESP_OK;
    s_fake.body = "release:     v0.4.0\np4 app:      " APP "\nc6_rcp:      " RCP "\n";
}

static void run_check(void)
{
    TEST_ASSERT_TRUE(self_update_check_begin());
    self_update_check_run();
}

/* ---- self_update_check_begin ---- */

TEST_CASE("Idle: begin returns true and the state is checking", "[self_update]")
{
    setup();
    self_update_snapshot_t s;
    TEST_ASSERT_TRUE(self_update_check_begin());
    self_update_get(&s);
    TEST_ASSERT_EQUAL(SELF_UPDATE_CHECKING, s.state);
}

TEST_CASE("Idle after done or failed: begin returns true", "[self_update]")
{
    setup();
    self_update_snapshot_t s;
    serve_valid();
    run_check();
    TEST_ASSERT_TRUE(self_update_check_begin());
    self_update_get(&s);
    TEST_ASSERT_EQUAL(SELF_UPDATE_CHECKING, s.state);
    self_update_check_run();
    s_fake.result = ESP_FAIL;
    s_fake.detail = "boom";
    run_check();
    TEST_ASSERT_TRUE(self_update_check_begin());
    self_update_get(&s);
    TEST_ASSERT_EQUAL(SELF_UPDATE_CHECKING, s.state);
}

TEST_CASE("Already checking: begin returns false and the state is unchanged", "[self_update]")
{
    setup();
    self_update_snapshot_t s;
    TEST_ASSERT_TRUE(self_update_check_begin());
    TEST_ASSERT_FALSE(self_update_check_begin());
    self_update_get(&s);
    TEST_ASSERT_EQUAL(SELF_UPDATE_CHECKING, s.state);
}

/* ---- self_update_check_run ---- */

TEST_CASE("Fetch returns a valid file: state done, release stored, error cleared, age 0", "[self_update]")
{
    setup();
    self_update_snapshot_t s;
    s_fake.result = ESP_FAIL;
    s_fake.detail = "earlier failure";
    run_check();
    serve_valid();
    s_fake.now_us = 5000000;
    run_check();
    self_update_get(&s);
    TEST_ASSERT_EQUAL(SELF_UPDATE_DONE, s.state);
    TEST_ASSERT_TRUE(s.has_release);
    TEST_ASSERT_EQUAL_STRING("v0.4.0", s.release.tag);
    TEST_ASSERT_EQUAL_STRING(APP, s.release.app);
    TEST_ASSERT_EQUAL_STRING(RCP, s.release.rcp);
    TEST_ASSERT_EQUAL_STRING("", s.error);
    TEST_ASSERT_TRUE(s.has_age);
    TEST_ASSERT_EQUAL_UINT32(0, s.age_s);
}

TEST_CASE("Fetch fails: state failed, error is the transport detail, the previous release is kept", "[self_update]")
{
    setup();
    self_update_snapshot_t s;
    serve_valid();
    run_check();
    s_fake.result = ESP_ERR_NOT_FOUND;
    s_fake.detail = "connection refused";
    run_check();
    self_update_get(&s);
    TEST_ASSERT_EQUAL(SELF_UPDATE_FAILED, s.state);
    TEST_ASSERT_EQUAL_STRING("connection refused", s.error);
    TEST_ASSERT_TRUE(s.has_release);
    TEST_ASSERT_EQUAL_STRING(APP, s.release.app);
}

TEST_CASE("Fetch succeeds but parse fails: state failed, error names the parse failure, release kept", "[self_update]")
{
    setup();
    self_update_snapshot_t s;
    serve_valid();
    run_check();
    s_fake.body = "<html>404</html>\n";
    run_check();
    self_update_get(&s);
    TEST_ASSERT_EQUAL(SELF_UPDATE_FAILED, s.state);
    TEST_ASSERT_NOT_NULL(strstr(s.error, "versions.txt"));
    TEST_ASSERT_NOT_NULL(strstr(s.error, "ESP_ERR_NOT_FOUND"));
    TEST_ASSERT_TRUE(s.has_release);
    TEST_ASSERT_EQUAL_STRING(APP, s.release.app);
}

TEST_CASE("Fetch asks for the latest release: the URL is <base>/latest/download/versions.txt", "[self_update]")
{
    setup();
    serve_valid();
    run_check();
    TEST_ASSERT_EQUAL_STRING(VERSIONS_URL, s_fake.url);
    TEST_ASSERT_EQUAL_STRING(SELF_UPDATE_RELEASES_URL, self_update_base());
}

/* ---- self_update_fetch_release ---- */

TEST_CASE("fetch_release succeeds: release returned, the cache is not touched", "[self_update]")
{
    setup();
    self_update_snapshot_t s;
    self_update_release_t r;
    char detail[160] = "";
    serve_valid();
    TEST_ASSERT_EQUAL(ESP_OK, self_update_fetch_release(&r, detail, sizeof(detail)));
    TEST_ASSERT_EQUAL_STRING(APP, r.app);
    self_update_get(&s);
    TEST_ASSERT_EQUAL(SELF_UPDATE_NEVER, s.state);
    TEST_ASSERT_FALSE(s.has_release);
}

TEST_CASE("fetch_release fails: the error and the detail are returned", "[self_update]")
{
    setup();
    self_update_release_t r;
    char detail[160] = "";
    s_fake.result = ESP_ERR_TIMEOUT;
    s_fake.detail = "timed out";
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, self_update_fetch_release(&r, detail, sizeof(detail)));
    TEST_ASSERT_EQUAL_STRING("timed out", detail);
    s_fake.result = ESP_OK;
    s_fake.body = "nothing useful\n";
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, self_update_fetch_release(&r, detail, sizeof(detail)));
    TEST_ASSERT_NOT_NULL(strstr(detail, "versions.txt"));
}

/* ---- self_update_get ---- */

TEST_CASE("Never checked: state never, no age, no release", "[self_update]")
{
    setup();
    self_update_snapshot_t s;
    self_update_get(&s);
    TEST_ASSERT_EQUAL(SELF_UPDATE_NEVER, s.state);
    TEST_ASSERT_FALSE(s.has_age);
    TEST_ASSERT_FALSE(s.has_release);
    TEST_ASSERT_EQUAL_STRING("", s.error);
}

TEST_CASE("30 s after a finished check: checked_age_s is 30", "[self_update]")
{
    setup();
    self_update_snapshot_t s;
    serve_valid();
    s_fake.now_us = 1000000;
    run_check();
    s_fake.now_us = 31000000;
    self_update_get(&s);
    TEST_ASSERT_TRUE(s.has_age);
    TEST_ASSERT_EQUAL_UINT32(30, s.age_s);
}

TEST_CASE("A new release replaces the stored one", "[self_update]")
{
    setup();
    self_update_snapshot_t s;
    serve_valid();
    run_check();
    s_fake.body = "p4 app:      " APP2 "\n";
    run_check();
    self_update_get(&s);
    TEST_ASSERT_EQUAL_STRING(APP2, s.release.app);
    TEST_ASSERT_EQUAL_STRING("", s.release.tag);
}
