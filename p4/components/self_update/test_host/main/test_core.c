#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "self_update_priv.h"

#define APP "20261103T185311Z-e41c14c"
#define RCP "20261103T185115Z-e41c14c"
#define BASE "https://example.org/releases"

static esp_err_t parse(const char *text, self_update_release_t *out)
{
    return self_update_parse_versions(text, strlen(text), out);
}

/* ---- self_update_parse_versions ---- */

TEST_CASE("v0.4.0-style file: ESP_OK, tag, app and rcp set, unknown lines ignored", "[self_update]")
{
    self_update_release_t r;
    memset(&r, 0xaa, sizeof(r));
    const char *text = "release:     v0.4.0\n"
                       "p4 app:      " APP "\n"
                       "c6_rcp:      " RCP "\n"
                       "c6 base:     base-d0c56989\n"
                       "ESP-IDF:     v6.1\n";
    TEST_ASSERT_EQUAL(ESP_OK, parse(text, &r));
    TEST_ASSERT_EQUAL_STRING("v0.4.0", r.tag);
    TEST_ASSERT_EQUAL_STRING(APP, r.app);
    TEST_ASSERT_EQUAL_STRING(RCP, r.rcp);
}

TEST_CASE("File without a release line: ESP_OK, tag is empty", "[self_update]")
{
    self_update_release_t r;
    memset(&r, 0xaa, sizeof(r));
    const char *text = "p4 app:      " APP "\nc6_rcp:      " RCP "\nc6 base:     base-d0c56989\nESP-IDF:     v6.1\n";
    TEST_ASSERT_EQUAL(ESP_OK, parse(text, &r));
    TEST_ASSERT_EQUAL_STRING("", r.tag);
    TEST_ASSERT_EQUAL_STRING(APP, r.app);
}

TEST_CASE("File without a c6_rcp line: ESP_OK, rcp is empty", "[self_update]")
{
    self_update_release_t r;
    TEST_ASSERT_EQUAL(ESP_OK, parse("release: v1\np4 app: " APP "\n", &r));
    TEST_ASSERT_EQUAL_STRING("", r.rcp);
}

TEST_CASE("CRLF line ends, trailing spaces: values without \\r or spaces", "[self_update]")
{
    self_update_release_t r;
    TEST_ASSERT_EQUAL(ESP_OK, parse("release:  v0.4.0  \r\np4 app: " APP "  \r\nc6_rcp: " RCP "\r\n", &r));
    TEST_ASSERT_EQUAL_STRING("v0.4.0", r.tag);
    TEST_ASSERT_EQUAL_STRING(APP, r.app);
    TEST_ASSERT_EQUAL_STRING(RCP, r.rcp);
}

TEST_CASE("Last line without a newline: the value is read", "[self_update]")
{
    self_update_release_t r;
    TEST_ASSERT_EQUAL(ESP_OK, parse("p4 app: " APP, &r));
    TEST_ASSERT_EQUAL_STRING(APP, r.app);
}

TEST_CASE("No p4 app line: ESP_ERR_NOT_FOUND", "[self_update]")
{
    self_update_release_t r;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, parse("release: v1\nc6_rcp: " RCP "\n", &r));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, parse("", &r));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, parse("<html><body>Not Found</body></html>", &r));
}

TEST_CASE("Empty p4 app value: ESP_ERR_NOT_FOUND", "[self_update]")
{
    self_update_release_t r;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, parse("p4 app:   \nc6_rcp: " RCP "\n", &r));
}

TEST_CASE("Value longer than 31 characters: ESP_ERR_INVALID_SIZE", "[self_update]")
{
    self_update_release_t r;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, parse("p4 app: 0123456789012345678901234567890x\n", &r));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, parse("p4 app: " APP "\nc6_rcp: 0123456789012345678901234567890x\n", &r));
}

TEST_CASE("Value of exactly 31 characters: accepted", "[self_update]")
{
    self_update_release_t r;
    TEST_ASSERT_EQUAL(ESP_OK, parse("p4 app: 0123456789012345678901234567890\n", &r));
    TEST_ASSERT_EQUAL_STRING("0123456789012345678901234567890", r.app);
}

TEST_CASE("Tag with a path, a query or 33 characters: ESP_ERR_INVALID_ARG, out untouched", "[self_update]")
{
    const char *bad[] = {
        "release: ../x\np4 app: " APP "\n",
        "release: v1?a\np4 app: " APP "\n",
        "release: v1/2\np4 app: " APP "\n",
        "release: 012345678901234567890123456789012\np4 app: " APP "\n",
        "release:\np4 app: " APP "\n",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        self_update_release_t r;
        memset(&r, 0xaa, sizeof(r));
        TEST_ASSERT_EQUAL_MESSAGE(ESP_ERR_INVALID_ARG, parse(bad[i], &r), bad[i]);
        TEST_ASSERT_EACH_EQUAL_UINT8(0xaa, (uint8_t *)&r, sizeof(r));
    }
}

TEST_CASE("Tag of exactly 32 characters: accepted", "[self_update]")
{
    self_update_release_t r;
    TEST_ASSERT_EQUAL(ESP_OK, parse("release: 01234567890123456789012345678901\np4 app: " APP "\n", &r));
    TEST_ASSERT_EQUAL_STRING("01234567890123456789012345678901", r.tag);
}

TEST_CASE("Text of 1025 bytes: ESP_ERR_INVALID_SIZE", "[self_update]")
{
    char *big = malloc(2000);
    TEST_ASSERT_NOT_NULL(big);
    memset(big, 'x', 2000);
    self_update_release_t r;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, self_update_parse_versions(big, SELF_UPDATE_VERSIONS_MAX + 1, &r));
    free(big);
}

TEST_CASE("Text of exactly 1024 bytes: parsed", "[self_update]")
{
    char *big = malloc(SELF_UPDATE_VERSIONS_MAX);
    TEST_ASSERT_NOT_NULL(big);
    memset(big, '#', SELF_UPDATE_VERSIONS_MAX);
    const char *line = "p4 app: " APP "\n";
    memcpy(big, line, strlen(line));
    self_update_release_t r;
    TEST_ASSERT_EQUAL(ESP_OK, self_update_parse_versions(big, SELF_UPDATE_VERSIONS_MAX, &r));
    TEST_ASSERT_EQUAL_STRING(APP, r.app);
    free(big);
}

TEST_CASE("A NUL inside the text: ESP_ERR_INVALID_ARG", "[self_update]")
{
    self_update_release_t r;
    const char text[] = "p4 app: " APP "\n\0release: v1\n";
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, self_update_parse_versions(text, sizeof(text) - 1, &r));
}

TEST_CASE("Text not NUL-terminated: nothing past len is read", "[self_update]")
{
    char buf[64];
    const char *line = "p4 app: " APP;
    size_t n = strlen(line);
    memcpy(buf, line, n);
    memset(buf + n, '9', sizeof(buf) - n); /* digits would extend the value if read */
    self_update_release_t r;
    TEST_ASSERT_EQUAL(ESP_OK, self_update_parse_versions(buf, n, &r));
    TEST_ASSERT_EQUAL_STRING(APP, r.app);
}

TEST_CASE("Key repeated: the first value is kept", "[self_update]")
{
    self_update_release_t r;
    const char *text = "release: v1\np4 app: " APP "\nc6_rcp: " RCP "\nrelease: v2\np4 app: 20270101T000000Z-aaaaaaa\n"
                       "c6_rcp: 20270101T000000Z-aaaaaaa\n";
    TEST_ASSERT_EQUAL(ESP_OK, parse(text, &r));
    TEST_ASSERT_EQUAL_STRING("v1", r.tag);
    TEST_ASSERT_EQUAL_STRING(APP, r.app);
    TEST_ASSERT_EQUAL_STRING(RCP, r.rcp);
}

TEST_CASE("Key as a prefix of another word: not matched", "[self_update]")
{
    self_update_release_t r;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, parse("p4 app2: " APP "\nreleased: v1\n", &r));
}

/* ---- self_update_compare ---- */

TEST_CASE("Release stamp later: SELF_UPDATE_NEWER", "[self_update]")
{
    TEST_ASSERT_EQUAL(SELF_UPDATE_NEWER, self_update_compare("20261104T000000Z-aaaaaaa", APP));
}

TEST_CASE("Release stamp earlier: SELF_UPDATE_OLDER", "[self_update]")
{
    TEST_ASSERT_EQUAL(SELF_UPDATE_OLDER, self_update_compare("20261102T000000Z-aaaaaaa", APP));
}

TEST_CASE("Same stamp and hash, running has -dirty: SELF_UPDATE_SAME", "[self_update]")
{
    TEST_ASSERT_EQUAL(SELF_UPDATE_SAME, self_update_compare(APP, APP "-dirty"));
    TEST_ASSERT_EQUAL(SELF_UPDATE_SAME, self_update_compare(APP, APP));
}

TEST_CASE("Same stamp, different hash: SELF_UPDATE_UNKNOWN", "[self_update]")
{
    TEST_ASSERT_EQUAL(SELF_UPDATE_UNKNOWN, self_update_compare("20261103T185311Z-0000000", APP));
}

TEST_CASE("Either version malformed: SELF_UPDATE_UNKNOWN", "[self_update]")
{
    TEST_ASSERT_EQUAL(SELF_UPDATE_UNKNOWN, self_update_compare("abc", APP));
    TEST_ASSERT_EQUAL(SELF_UPDATE_UNKNOWN, self_update_compare(APP, "abc"));
    TEST_ASSERT_EQUAL(SELF_UPDATE_UNKNOWN, self_update_compare("", APP));
    TEST_ASSERT_EQUAL(SELF_UPDATE_UNKNOWN, self_update_compare(APP, ""));
    TEST_ASSERT_EQUAL(SELF_UPDATE_UNKNOWN, self_update_compare("20261103 185311 -e41c14c", APP));
    TEST_ASSERT_EQUAL(SELF_UPDATE_UNKNOWN, self_update_compare("20261103T185311Z", APP));
    TEST_ASSERT_EQUAL(SELF_UPDATE_UNKNOWN, self_update_compare(APP "-junk", APP));
    TEST_ASSERT_EQUAL(SELF_UPDATE_UNKNOWN, self_update_compare(NULL, APP));
    TEST_ASSERT_EQUAL(SELF_UPDATE_UNKNOWN, self_update_compare(APP, NULL));
}

/* ---- self_update_rcp_differs ---- */

TEST_CASE("Release rcp empty: false", "[self_update]")
{
    TEST_ASSERT_FALSE(self_update_rcp_differs("", RCP, true));
    TEST_ASSERT_FALSE(self_update_rcp_differs("", "", false));
}

TEST_CASE("Equal rcp strings: false", "[self_update]")
{
    TEST_ASSERT_FALSE(self_update_rcp_differs(RCP, RCP, true));
}

TEST_CASE("Different rcp: true", "[self_update]")
{
    TEST_ASSERT_TRUE(self_update_rcp_differs(RCP, "20261001T000000Z-aaaaaaa", true));
}

TEST_CASE("Running rcp unknown or empty: true", "[self_update]")
{
    TEST_ASSERT_TRUE(self_update_rcp_differs(RCP, RCP, false));
    TEST_ASSERT_TRUE(self_update_rcp_differs(RCP, "", true));
    TEST_ASSERT_TRUE(self_update_rcp_differs(RCP, NULL, true));
}

/* ---- self_update_asset_url ---- */

TEST_CASE("Asset URL with a tag: <base>/download/<tag>/<file>", "[self_update]")
{
    char out[128];
    TEST_ASSERT_EQUAL(ESP_OK, self_update_asset_url(BASE, "v0.4.0", "otbr.bundle", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(BASE "/download/v0.4.0/otbr.bundle", out);
}

TEST_CASE("Asset URL with an empty tag: <base>/latest/download/<file>", "[self_update]")
{
    char out[128];
    TEST_ASSERT_EQUAL(ESP_OK, self_update_asset_url(BASE, "", "versions.txt", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(BASE "/latest/download/versions.txt", out);
}

TEST_CASE("Asset URL that does not fit: ESP_ERR_INVALID_SIZE, out is empty", "[self_update]")
{
    char out[16] = "junk";
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, self_update_asset_url(BASE, "v0.4.0", "otbr.bundle", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
}

TEST_CASE("Asset URL that fits exactly: ESP_OK; one byte less: ESP_ERR_INVALID_SIZE", "[self_update]")
{
    const char *want = BASE "/download/v1/c6_rcp.bin";
    char out[64];
    TEST_ASSERT_EQUAL(ESP_OK, self_update_asset_url(BASE, "v1", "c6_rcp.bin", out, strlen(want) + 1));
    TEST_ASSERT_EQUAL_STRING(want, out);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, self_update_asset_url(BASE, "v1", "c6_rcp.bin", out, strlen(want)));
    TEST_ASSERT_EQUAL_STRING("", out);
}

TEST_CASE("Asset URL with a bad tag: ESP_ERR_INVALID_ARG, out is empty", "[self_update]")
{
    char out[128] = "junk";
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, self_update_asset_url(BASE, "../x", "otbr.bundle", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, self_update_asset_url(BASE, "v1?a", "otbr.bundle", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
}

/* ---- self_update_release_page_url ---- */

TEST_CASE("Release page with a tag: <base>/tag/<tag>", "[self_update]")
{
    char out[128];
    TEST_ASSERT_EQUAL(ESP_OK, self_update_release_page_url(BASE, "v0.4.0", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(BASE "/tag/v0.4.0", out);
}

TEST_CASE("Release page with an empty tag: <base>/latest", "[self_update]")
{
    char out[128];
    TEST_ASSERT_EQUAL(ESP_OK, self_update_release_page_url(BASE, "", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(BASE "/latest", out);
}

TEST_CASE("Release page that does not fit: ESP_ERR_INVALID_SIZE, out is empty", "[self_update]")
{
    char out[8] = "junk";
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, self_update_release_page_url(BASE, "v0.4.0", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
}

TEST_CASE("Release page with a bad tag: ESP_ERR_INVALID_ARG, out is empty", "[self_update]")
{
    char out[128] = "junk";
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, self_update_release_page_url(BASE, "a b", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = (int)UNITY_END();
    exit(failures == 0 ? 0 : 1);
}
