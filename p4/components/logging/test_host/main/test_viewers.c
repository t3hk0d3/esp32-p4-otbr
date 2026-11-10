#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "logging.h"
#include "logging_core.h"
#include "logging_priv.h"

/* ---- Fake NVS and esp_log_level_set: the gate the settings code hands to ESP-IDF ---- */

static esp_log_level_t s_star_level; /**< Last level given for "*". */

static esp_err_t fake_blob_get(const char *ns, const char *key, void *buf, size_t *len)
{
    (void) ns;
    (void) key;
    (void) buf;
    (void) len;
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t fake_blob_set(const char *ns, const char *key, const void *buf, size_t len)
{
    (void) ns;
    (void) key;
    (void) buf;
    (void) len;
    return ESP_OK;
}

static void fake_level_set(const char *tag, esp_log_level_t level)
{
    if (strcmp(tag, "*") == 0) {
        s_star_level = level;
    }
}

static const logging_ops_t FAKE_OPS = {
    .blob_get = fake_blob_get,
    .blob_set = fake_blob_set,
    .level_set = fake_level_set,
};

/* ---- Fake drain sinks: JTAG and syslog stay quiet ---- */

static bool no_host(void)
{
    return false;
}

static const logging_drain_ops_t QUIET_DRAIN = {
    .jtag_connected = no_host,
};

/* ---- Wake counters, one per viewer context ---- */

static int s_wakes[LOGGING_VIEWERS_MAX + 1];

static void wake(void *ctx)
{
    s_wakes[(int)(intptr_t) ctx]++;
}

/** Settings initialised (defaults, component default `def`), sinks quiet, no viewer, counters cleared. */
static void setup_with_default(esp_log_level_t def)
{
    logging_viewers_reset();
    logging_reset();
    logging_set_ops(&FAKE_OPS);
    TEST_ASSERT_EQUAL(ESP_OK, logging_init());
    logging_settings_t s;

    logging_core_defaults(&s);
    s.default_level = def;
    TEST_ASSERT_EQUAL(ESP_OK, logging_apply(&s, NULL));
    logging_drain_set_ops(&QUIET_DRAIN);
    logging_drain_reset();
    memset(s_wakes, 0, sizeof(s_wakes));
}

static void setup(void)
{
    setup_with_default(ESP_LOG_VERBOSE);
}

static void teardown(void)
{
    logging_viewers_reset();
}

/** A drained line through the drain's own entry point, with the levels in force. */
static void drain(esp_log_level_t level, const char *tag, uint32_t uptime_ms, const char *text)
{
    logging_line_t l = {.uptime_ms = uptime_ms, .level = level, .len = (uint16_t) strlen(text), .text = text};

    strlcpy(l.tag, tag, sizeof(l.tag));
    logging_drain_line(&l, logging_levels());
}

static logging_status_t status(void)
{
    logging_status_t st;

    memset(&st, 0, sizeof(st));
    logging_viewers_status(&st);
    return st;
}

/** Read one line into `text` (NUL-terminated); false when the queue is empty. */
static bool read_text(logging_viewer_t *v, char *text, size_t cap, uint32_t *dropped)
{
    logging_line_t l;

    if (!logging_viewer_read(v, &l, dropped)) {
        return false;
    }
    snprintf(text, cap, "%u %u %s %.*s", (unsigned) l.level, (unsigned) l.uptime_ms, l.tag, (int) l.len, l.text);
    return true;
}

/* ---- viewer fan-out ---- */

TEST_CASE("record and two viewers at debug and info: debug record reaches only the debug viewer", "[logging]")
{
    setup();
    logging_viewer_t *dbg = logging_viewer_open(ESP_LOG_DEBUG, wake, (void *) 1);
    logging_viewer_t *inf = logging_viewer_open(ESP_LOG_INFO, wake, (void *) 2);
    char text[128];

    TEST_ASSERT_NOT_NULL(dbg);
    TEST_ASSERT_NOT_NULL(inf);
    drain(ESP_LOG_DEBUG, "ziggurat", 812, "frame in");
    TEST_ASSERT_TRUE(read_text(dbg, text, sizeof(text), NULL));
    TEST_ASSERT_EQUAL_STRING("4 812 ziggurat frame in", text);
    TEST_ASSERT_FALSE(read_text(inf, text, sizeof(text), NULL));
    TEST_ASSERT_EQUAL(1, s_wakes[1]);
    TEST_ASSERT_EQUAL(0, s_wakes[2]);
    teardown();
}

TEST_CASE("record and two viewers at debug and info: info record reaches both, in order", "[logging]")
{
    setup();
    logging_viewer_t *dbg = logging_viewer_open(ESP_LOG_DEBUG, wake, (void *) 1);
    logging_viewer_t *inf = logging_viewer_open(ESP_LOG_INFO, wake, (void *) 2);
    char text[128];

    drain(ESP_LOG_INFO, "radio_mux", 1, "chan up");
    drain(ESP_LOG_WARN, "eth", 2, "link down");
    TEST_ASSERT_TRUE(read_text(inf, text, sizeof(text), NULL));
    TEST_ASSERT_EQUAL_STRING("3 1 radio_mux chan up", text);
    TEST_ASSERT_TRUE(read_text(inf, text, sizeof(text), NULL));
    TEST_ASSERT_EQUAL_STRING("2 2 eth link down", text);
    TEST_ASSERT_FALSE(read_text(inf, text, sizeof(text), NULL));
    TEST_ASSERT_TRUE(read_text(dbg, text, sizeof(text), NULL));
    TEST_ASSERT_TRUE(read_text(dbg, text, sizeof(text), NULL));
    TEST_ASSERT_EQUAL_UINT32(2, status().live_sent);
    teardown();
}

TEST_CASE("no viewer: nothing queued, nothing counted", "[logging]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_LOG_NONE, logging_levels().viewers);
    drain(ESP_LOG_ERROR, "eth", 1, "link down");
    TEST_ASSERT_EQUAL_UINT32(0, status().live_sent);
    TEST_ASSERT_EQUAL_UINT32(0, status().live_dropped);
    TEST_ASSERT_EQUAL(0, status().viewers);
    teardown();
}

TEST_CASE("closed viewer: gets no more lines, others keep theirs", "[logging]")
{
    setup();
    logging_viewer_t *a = logging_viewer_open(ESP_LOG_INFO, wake, (void *) 1);
    logging_viewer_t *b = logging_viewer_open(ESP_LOG_INFO, wake, (void *) 2);
    char text[128];

    logging_viewer_close(a);
    drain(ESP_LOG_INFO, "eth", 1, "link up");
    TEST_ASSERT_EQUAL(0, s_wakes[1]);
    TEST_ASSERT_TRUE(read_text(b, text, sizeof(text), NULL));
    TEST_ASSERT_EQUAL(1, status().viewers);
    teardown();
}

/* ---- per-viewer drop ---- */

TEST_CASE("full viewer queue: that viewer's dropped + 1; the other viewer unaffected", "[logging]")
{
    setup();
    logging_viewer_t *slow = logging_viewer_open(ESP_LOG_INFO, wake, (void *) 1);
    logging_viewer_t *fast = logging_viewer_open(ESP_LOG_INFO, wake, (void *) 2);
    char body[LOGGING_LINE_MAX + 1];
    char text[LOGGING_LINE_MAX + 64];
    uint32_t dropped = 0;
    int sent = 0;

    memset(body, 'x', LOGGING_LINE_MAX);
    body[LOGGING_LINE_MAX] = '\0';
    // Far more than 8 KB of long lines: the slow viewer reads nothing, the fast one reads each at once.
    for (int i = 0; i < 40; i++) {
        drain(ESP_LOG_INFO, "flood", (uint32_t) i, body);
        TEST_ASSERT_TRUE(read_text(fast, text, sizeof(text), &dropped));
        TEST_ASSERT_EQUAL_UINT32(0, dropped);
    }
    while (read_text(slow, text, sizeof(text), &dropped) && sent < 40) {
        if (sent == 0) {
            TEST_ASSERT_GREATER_THAN_UINT32(0, dropped); // reported with the first read after the drops
        } else {
            TEST_ASSERT_EQUAL_UINT32(0, dropped);
        }
        sent++;
    }
    TEST_ASSERT_GREATER_THAN(0, sent);
    TEST_ASSERT_LESS_THAN(40, sent);
    TEST_ASSERT_EQUAL_UINT32(40, status().live_sent);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(40 - sent), status().live_dropped);
    teardown();
}

TEST_CASE("full viewer queue: drops reported once, then 0 on the following reads", "[logging]")
{
    setup();
    logging_viewer_t *v = logging_viewer_open(ESP_LOG_INFO, wake, (void *) 1);
    char body[LOGGING_LINE_MAX + 1];
    char text[LOGGING_LINE_MAX + 64];
    uint32_t first = 0, again = 7;
    int queued = 0;

    memset(body, 'y', LOGGING_LINE_MAX);
    body[LOGGING_LINE_MAX] = '\0';
    for (int i = 0; i < 40; i++) {
        drain(ESP_LOG_INFO, "flood", (uint32_t) i, body);
    }
    while (read_text(v, text, sizeof(text), queued == 0 ? &first : &again)) {
        queued++;
    }
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(40 - queued), first);
    TEST_ASSERT_EQUAL_UINT32(0, again);
    teardown();
}

TEST_CASE("full viewer queue: the queue holds about LOGGING_VIEWER_QUEUE bytes of lines", "[logging]")
{
    setup();
    logging_viewer_t *v = logging_viewer_open(ESP_LOG_INFO, wake, (void *) 1);
    char body[257];
    char text[LOGGING_LINE_MAX + 64];
    int queued = 0;

    memset(body, 'z', 256);
    body[256] = '\0';
    for (int i = 0; i < 100; i++) {
        drain(ESP_LOG_INFO, "flood", (uint32_t) i, body);
    }
    while (read_text(v, text, sizeof(text), NULL)) {
        queued++;
    }
    // 256-byte bodies plus a record header each: 8 KB holds a little under 32 of them.
    TEST_ASSERT_GREATER_OR_EQUAL(24, queued);
    TEST_ASSERT_LESS_OR_EQUAL(32, queued);
    teardown();
}

/* ---- open beyond max ---- */

TEST_CASE("open beyond LOGGING_VIEWERS_MAX: the third open returns NULL", "[logging]")
{
    setup();
    TEST_ASSERT_EQUAL(2, LOGGING_VIEWERS_MAX); // Ruling R11
    logging_viewer_t *v[LOGGING_VIEWERS_MAX];

    for (int i = 0; i < LOGGING_VIEWERS_MAX; i++) {
        v[i] = logging_viewer_open(ESP_LOG_INFO, wake, (void *)(intptr_t) i);
        TEST_ASSERT_NOT_NULL(v[i]);
    }
    TEST_ASSERT_NULL(logging_viewer_open(ESP_LOG_INFO, wake, NULL));
    TEST_ASSERT_EQUAL(LOGGING_VIEWERS_MAX, status().viewers);
    teardown();
}

TEST_CASE("open after a close at the maximum: succeeds", "[logging]")
{
    setup();
    logging_viewer_t *v[LOGGING_VIEWERS_MAX];

    for (int i = 0; i < LOGGING_VIEWERS_MAX; i++) {
        v[i] = logging_viewer_open(ESP_LOG_INFO, wake, NULL);
    }
    logging_viewer_close(v[LOGGING_VIEWERS_MAX - 1]);
    TEST_ASSERT_NOT_NULL(logging_viewer_open(ESP_LOG_DEBUG, wake, NULL));
    teardown();
}

TEST_CASE("open with a level outside off..verbose: NULL, nothing opened", "[logging]")
{
    setup();
    TEST_ASSERT_NULL(logging_viewer_open((esp_log_level_t) 6, wake, NULL));
    TEST_ASSERT_EQUAL(0, status().viewers);
    teardown();
}

TEST_CASE("open and close many times: each close frees the slot and the queue", "[logging]")
{
    setup();
    for (int i = 0; i < 100; i++) {
        logging_viewer_t *v = logging_viewer_open(ESP_LOG_INFO, NULL, NULL);

        TEST_ASSERT_NOT_NULL(v);
        drain(ESP_LOG_INFO, "eth", 1, "link up"); // a NULL wake is allowed
        logging_viewer_close(v);
    }
    TEST_ASSERT_EQUAL(0, status().viewers);
    logging_viewer_close(NULL); // ignored
    teardown();
}

/* ---- gate recomputed on open, close and level change ---- */

TEST_CASE("first debug viewer opens: viewers level debug published, gate raised to debug", "[logging]")
{
    setup_with_default(ESP_LOG_VERBOSE);
    TEST_ASSERT_EQUAL(ESP_LOG_INFO, s_star_level); // UART and JTAG info: nothing wants more
    logging_viewer_t *v = logging_viewer_open(ESP_LOG_DEBUG, wake, NULL);

    TEST_ASSERT_EQUAL(ESP_LOG_DEBUG, logging_levels().viewers);
    TEST_ASSERT_EQUAL(ESP_LOG_DEBUG, s_star_level);
    logging_viewer_close(v);
    teardown();
}

TEST_CASE("viewer level change: viewers level is the highest open level", "[logging]")
{
    setup();
    logging_viewer_t *a = logging_viewer_open(ESP_LOG_DEBUG, wake, NULL);
    logging_viewer_t *b = logging_viewer_open(ESP_LOG_INFO, wake, NULL);

    TEST_ASSERT_EQUAL(ESP_LOG_DEBUG, logging_levels().viewers);
    logging_viewer_set_level(a, ESP_LOG_WARN);
    TEST_ASSERT_EQUAL(ESP_LOG_INFO, logging_levels().viewers);
    TEST_ASSERT_EQUAL(ESP_LOG_INFO, s_star_level);
    logging_viewer_set_level(b, ESP_LOG_VERBOSE);
    TEST_ASSERT_EQUAL(ESP_LOG_VERBOSE, logging_levels().viewers);
    TEST_ASSERT_EQUAL(ESP_LOG_VERBOSE, s_star_level);
    logging_viewer_set_level(b, (esp_log_level_t) 9); // ignored
    TEST_ASSERT_EQUAL(ESP_LOG_VERBOSE, logging_levels().viewers);
    teardown();
}

TEST_CASE("viewer level change: lines follow the new level", "[logging]")
{
    setup();
    logging_viewer_t *v = logging_viewer_open(ESP_LOG_INFO, wake, NULL);
    char text[128];

    logging_viewer_set_level(v, ESP_LOG_WARN);
    drain(ESP_LOG_INFO, "eth", 1, "link up");
    TEST_ASSERT_FALSE(read_text(v, text, sizeof(text), NULL));
    drain(ESP_LOG_WARN, "eth", 2, "link down");
    TEST_ASSERT_TRUE(read_text(v, text, sizeof(text), NULL));
    teardown();
}

TEST_CASE("viewer closes: viewers level drops to the highest remaining one", "[logging]")
{
    setup();
    logging_viewer_t *a = logging_viewer_open(ESP_LOG_VERBOSE, wake, NULL);
    logging_viewer_t *b = logging_viewer_open(ESP_LOG_WARN, wake, NULL);

    logging_viewer_close(a);
    TEST_ASSERT_EQUAL(ESP_LOG_WARN, logging_levels().viewers);
    TEST_ASSERT_EQUAL(ESP_LOG_INFO, s_star_level); // UART info is now the highest
    (void) b;
    teardown();
}

TEST_CASE("last viewer closes: viewers level off, gate back to the channels", "[logging]")
{
    setup();
    logging_viewer_t *a = logging_viewer_open(ESP_LOG_VERBOSE, wake, NULL);

    TEST_ASSERT_EQUAL(ESP_LOG_VERBOSE, s_star_level);
    logging_viewer_close(a);
    TEST_ASSERT_EQUAL(ESP_LOG_NONE, logging_levels().viewers);
    TEST_ASSERT_EQUAL(ESP_LOG_INFO, s_star_level);
    teardown();
}

TEST_CASE("viewer at off: counts as no level, gets nothing", "[logging]")
{
    setup();
    logging_viewer_t *v = logging_viewer_open(ESP_LOG_NONE, wake, NULL);
    char text[128];

    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL(ESP_LOG_NONE, logging_levels().viewers);
    drain(ESP_LOG_ERROR, "eth", 1, "boom");
    TEST_ASSERT_FALSE(read_text(v, text, sizeof(text), NULL));
    TEST_ASSERT_EQUAL(1, status().viewers);
    teardown();
}

/* ---- the network path's own debug lines never reach a viewer (Rulings R13, R14) ---- */

TEST_CASE("debug viewer and debug lines from network-path tags: not queued, nothing woken", "[logging]")
{
    setup();
    logging_viewer_t *v = logging_viewer_open(ESP_LOG_VERBOSE, wake, (void *) 1);
    const char *tags[] = {"httpd", "httpd_sess", "httpd_txrx", "httpd_ws", "httpd_parse", "httpd_uri", "esp.emac",
                          "esp_eth", "esp_netif_lwip", "lwip"
                         };
    char text[128];

    for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++) {
        drain(ESP_LOG_DEBUG, tags[i], 1, "work");
        drain(ESP_LOG_VERBOSE, tags[i], 2, "doing select");
    }
    TEST_ASSERT_FALSE(read_text(v, text, sizeof(text), NULL));
    TEST_ASSERT_EQUAL(0, s_wakes[1]);
    TEST_ASSERT_EQUAL_UINT32(0, status().live_sent);
    TEST_ASSERT_EQUAL_UINT32(0, status().live_dropped);
    teardown();
}

TEST_CASE("debug viewer and an info line from an httpd tag: queued", "[logging]")
{
    setup();
    logging_viewer_t *v = logging_viewer_open(ESP_LOG_DEBUG, wake, (void *) 1);
    char text[128];

    drain(ESP_LOG_INFO, "httpd_uri", 3, "no handler");
    TEST_ASSERT_TRUE(read_text(v, text, sizeof(text), NULL));
    TEST_ASSERT_EQUAL_STRING("3 3 httpd_uri no handler", text);
    drain(ESP_LOG_DEBUG, "radio_mux", 4, "detail"); // other tags keep the viewer's level
    TEST_ASSERT_TRUE(read_text(v, text, sizeof(text), NULL));
    teardown();
}

/* ---- lines the transit ring refused (Ruling R12) ---- */

TEST_CASE("ring drops while viewers are open: each open viewer's next read reports them", "[logging]")
{
    setup();
    logging_viewer_t *a = logging_viewer_open(ESP_LOG_INFO, wake, (void *) 1);
    logging_viewer_t *b = logging_viewer_open(ESP_LOG_DEBUG, wake, (void *) 2);
    char text[128];
    uint32_t dropped = 0;

    logging_viewers_add_dropped(5);
    drain(ESP_LOG_INFO, "eth", 1, "link up");
    TEST_ASSERT_TRUE(read_text(a, text, sizeof(text), &dropped));
    TEST_ASSERT_EQUAL_UINT32(5, dropped);
    TEST_ASSERT_FALSE(read_text(a, text, sizeof(text), &dropped));
    TEST_ASSERT_EQUAL_UINT32(0, dropped);
    TEST_ASSERT_TRUE(read_text(b, text, sizeof(text), &dropped));
    TEST_ASSERT_EQUAL_UINT32(5, dropped);
    teardown();
}

TEST_CASE("ring drops added to a full queue's own drops: one total", "[logging]")
{
    setup();
    logging_viewer_t *v = logging_viewer_open(ESP_LOG_INFO, wake, (void *) 1);
    char body[LOGGING_LINE_MAX + 1];
    char text[LOGGING_LINE_MAX + 64];
    uint32_t dropped = 0;
    int queued = 0;

    memset(body, 'q', LOGGING_LINE_MAX);
    body[LOGGING_LINE_MAX] = '\0';
    for (int i = 0; i < 40; i++) {
        drain(ESP_LOG_INFO, "flood", (uint32_t) i, body);
    }
    logging_viewers_add_dropped(3);
    TEST_ASSERT_TRUE(read_text(v, text, sizeof(text), &dropped));
    queued = 1;
    while (read_text(v, text, sizeof(text), NULL)) {
        queued++;
    }
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(40 - queued + 3), dropped);
    teardown();
}

TEST_CASE("ring drops with no viewer open, or before a viewer opens: not reported to it", "[logging]")
{
    setup();
    logging_viewers_add_dropped(7); // nobody open: ignored
    logging_viewer_t *v = logging_viewer_open(ESP_LOG_INFO, wake, (void *) 1);
    char text[128];
    uint32_t dropped = 9;

    drain(ESP_LOG_INFO, "eth", 1, "link up");
    TEST_ASSERT_TRUE(read_text(v, text, sizeof(text), &dropped));
    TEST_ASSERT_EQUAL_UINT32(0, dropped);
    teardown();
}

TEST_CASE("ring drops reported without a line: viewer woken, dropped_since set, read false", "[logging]")
{
    setup();
    logging_viewer_t *v = logging_viewer_open(ESP_LOG_INFO, wake, (void *) 1);
    char text[128];
    uint32_t dropped = 0;

    logging_viewers_add_dropped(2);
    TEST_ASSERT_EQUAL(1, s_wakes[1]); // woken, so the notice goes out without waiting for a line
    TEST_ASSERT_FALSE(read_text(v, text, sizeof(text), &dropped));
    TEST_ASSERT_EQUAL_UINT32(2, dropped);
    teardown();
}
