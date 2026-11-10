#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <netinet/in.h>
#include "esp_log.h"
#include "unity.h"
#include "logging.h"
#include "logging_core.h"
#include "logging_priv.h"

/* ---- Fake USB-Serial-JTAG, resolver and UDP socket ---- */

#define FAKE_FD 7

static struct {
    bool connected;
    bool jtag_full; /**< TX buffer full: writes take nothing. */
    char jtag[1024];
    size_t jtag_len;
    int jtag_writes;
    esp_err_t resolve_result;
    int resolves;
    char resolved_host[LOGGING_TARGET_MAX];
    uint16_t resolved_port;
    int opens;
    int open_family;
    int sends;
    int send_result; /**< -1 = error. */
    char datagram[1024];
    size_t datagram_len;
    uint16_t sent_port;
    uint8_t sent_octet; /**< Last byte of the address the last datagram went to. */
    uint8_t addr_octet; /**< Last byte of the address the resolver answers (192.168.1.x). */
    int connected_calls;
    int closes;
    int closed_fd;
    bool clock_valid;
} s_fake;

static bool fake_jtag_connected(void)
{
    s_fake.connected_calls++;
    return s_fake.connected;
}

static int fake_jtag_write(const void *buf, size_t len)
{
    s_fake.jtag_writes++;
    if (s_fake.jtag_full || s_fake.jtag_len + len > sizeof(s_fake.jtag)) {
        return 0;
    }
    memcpy(s_fake.jtag + s_fake.jtag_len, buf, len);
    s_fake.jtag_len += len;
    return (int) len;
}

static esp_err_t fake_resolve(const char *host, uint16_t port, struct sockaddr_storage *addr, socklen_t *addr_len)
{
    struct sockaddr_in *in = (struct sockaddr_in *) addr;

    s_fake.resolves++;
    strlcpy(s_fake.resolved_host, host, sizeof(s_fake.resolved_host));
    s_fake.resolved_port = port;
    if (s_fake.resolve_result != ESP_OK) {
        return s_fake.resolve_result;
    }
    memset(addr, 0, sizeof(*addr));
    in->sin_family = AF_INET;
    in->sin_port = htons(port);
    in->sin_addr.s_addr = htonl(0xc0a80100 | s_fake.addr_octet);
    *addr_len = sizeof(*in);
    return ESP_OK;
}

static int fake_udp_open(int family)
{
    s_fake.opens++;
    s_fake.open_family = family;
    return FAKE_FD;
}

static int fake_udp_send(int fd, const void *buf, size_t len, const struct sockaddr *to, socklen_t to_len)
{
    TEST_ASSERT_EQUAL(FAKE_FD, fd);
    TEST_ASSERT_EQUAL(sizeof(struct sockaddr_in), to_len);
    s_fake.sends++;
    if (s_fake.send_result < 0) {
        return -1;
    }
    s_fake.datagram_len = len < sizeof(s_fake.datagram) - 1 ? len : sizeof(s_fake.datagram) - 1;
    memcpy(s_fake.datagram, buf, s_fake.datagram_len);
    s_fake.datagram[s_fake.datagram_len] = '\0';
    s_fake.sent_port = ntohs(((const struct sockaddr_in *) to)->sin_port);
    s_fake.sent_octet = (uint8_t)(ntohl(((const struct sockaddr_in *) to)->sin_addr.s_addr) & 0xff);
    return (int) len;
}

static void fake_udp_close(int fd)
{
    s_fake.closes++;
    s_fake.closed_fd = fd;
}

static const char *fake_hostname(void)
{
    return "esp-otbr-d2cafc";
}

static bool fake_clock(struct timeval *out)
{
    out->tv_sec = 1794312000; // 2026-11-10T12:00:00Z
    out->tv_usec = 123000;
    return s_fake.clock_valid;
}

static const logging_drain_ops_t FAKE_OPS = {
    .jtag_connected = fake_jtag_connected,
    .jtag_write = fake_jtag_write,
    .resolve = fake_resolve,
    .udp_open = fake_udp_open,
    .udp_send = fake_udp_send,
    .udp_close = fake_udp_close,
    .hostname = fake_hostname,
};

/** The JTAG driver failed to install: no write op. */
static const logging_drain_ops_t NO_JTAG_OPS = {
    .jtag_connected = fake_jtag_connected,
    .jtag_write = NULL,
    .resolve = fake_resolve,
    .udp_open = fake_udp_open,
    .udp_send = fake_udp_send,
    .udp_close = fake_udp_close,
    .hostname = fake_hostname,
};

/* ---- Log output capture: the sinks must never log ---- */

static int s_log_lines;

static int count_lines(const char *fmt, va_list ap)
{
    (void) fmt;
    (void) ap;
    s_log_lines++;
    return 0;
}

static vprintf_like_t s_prev_vprintf;

static void setup(void)
{
    logging_drain_set_ops(&FAKE_OPS);
    logging_drain_reset(); // closes the last test's socket: before the fakes are cleared
    memset(&s_fake, 0, sizeof(s_fake));
    s_fake.connected = true;
    s_fake.addr_octet = 10;
    logging_set_clock(NULL);
    s_log_lines = 0;
    s_prev_vprintf = esp_log_set_vprintf(count_lines);
}

static void teardown(void)
{
    esp_log_set_vprintf(s_prev_vprintf);
}

static logging_levels_t levels(esp_log_level_t jtag, esp_log_level_t syslog)
{
    logging_settings_t s;

    logging_core_defaults(&s);
    s.channel[LOGGING_CH_JTAG] = jtag;
    s.channel[LOGGING_CH_SYSLOG] = syslog;
    return logging_core_levels(&s, ESP_LOG_NONE);
}

static logging_line_t line(esp_log_level_t level, const char *tag, uint32_t uptime_ms, const char *text)
{
    logging_line_t l = {.uptime_ms = uptime_ms, .level = level, .len = (uint16_t) strlen(text), .text = text};

    strlcpy(l.tag, tag, sizeof(l.tag));
    return l;
}

static logging_status_t status(void)
{
    logging_status_t st;

    memset(&st, 0xa5, sizeof(st));
    logging_drain_status(&st);
    return st;
}

/** Syslog on at `level` towards `target`, resolved at uptime 0. */
static void syslog_on(esp_log_level_t level, const char *target)
{
    logging_drain_configure(level, target);
    logging_drain_tick(0);
}

/* ---- JTAG sink ---- */

TEST_CASE("JTAG record at or below the JTAG level with a host reading: written, sent[JTAG] + 1", "[logging]")
{
    setup();
    logging_line_t l = line(ESP_LOG_INFO, "radio_mux", 1234, "chan up");

    logging_drain_line(&l, levels(ESP_LOG_INFO, ESP_LOG_NONE));
    s_fake.jtag[s_fake.jtag_len] = '\0';
    TEST_ASSERT_EQUAL_STRING("I (1234) radio_mux: chan up\n", s_fake.jtag);
    TEST_ASSERT_EQUAL_UINT32(1, status().sent[LOGGING_CH_JTAG]);
    TEST_ASSERT_EQUAL_UINT32(0, status().dropped[LOGGING_CH_JTAG]);
    TEST_ASSERT_TRUE(status().jtag_host);
    teardown();
}

TEST_CASE("JTAG record above the JTAG level: not written, nothing counted", "[logging]")
{
    setup();
    logging_line_t l = line(ESP_LOG_DEBUG, "radio_mux", 1, "detail");

    logging_drain_line(&l, levels(ESP_LOG_INFO, ESP_LOG_NONE));
    TEST_ASSERT_EQUAL(0, s_fake.jtag_writes);
    TEST_ASSERT_EQUAL_UINT32(0, status().sent[LOGGING_CH_JTAG]);
    TEST_ASSERT_EQUAL_UINT32(0, status().dropped[LOGGING_CH_JTAG]);
    teardown();
}

TEST_CASE("JTAG with no host: dropped silently, dropped[JTAG] + 1, jtag_host false, no write", "[logging]")
{
    setup();
    s_fake.connected = false;
    logging_line_t l = line(ESP_LOG_WARN, "eth", 5, "link down");

    logging_drain_line(&l, levels(ESP_LOG_INFO, ESP_LOG_NONE));
    TEST_ASSERT_EQUAL(0, s_fake.jtag_writes);
    TEST_ASSERT_EQUAL_UINT32(1, status().dropped[LOGGING_CH_JTAG]);
    TEST_ASSERT_EQUAL_UINT32(0, status().sent[LOGGING_CH_JTAG]);
    TEST_ASSERT_FALSE(status().jtag_host);
    TEST_ASSERT_EQUAL(0, s_log_lines);
    teardown();
}

TEST_CASE("JTAG with a host that does not read (TX buffer full): dropped[JTAG] + 1, no partial line", "[logging]")
{
    setup();
    s_fake.jtag_full = true;
    logging_line_t l = line(ESP_LOG_INFO, "eth", 5, "link up");

    logging_drain_line(&l, levels(ESP_LOG_INFO, ESP_LOG_NONE));
    TEST_ASSERT_EQUAL(1, s_fake.jtag_writes);
    TEST_ASSERT_EQUAL(0, s_fake.jtag_len);
    TEST_ASSERT_EQUAL_UINT32(1, status().dropped[LOGGING_CH_JTAG]);
    TEST_ASSERT_EQUAL(0, s_log_lines);
    teardown();
}

TEST_CASE("JTAG without a driver: dropped[JTAG] + 1, nothing formatted or written, host not asked", "[logging]")
{
    setup();
    logging_drain_set_ops(&NO_JTAG_OPS);
    s_fake.connected_calls = 0;
    logging_line_t l = line(ESP_LOG_INFO, "eth", 5, "link up");

    logging_drain_line(&l, levels(ESP_LOG_INFO, ESP_LOG_NONE));
    TEST_ASSERT_EQUAL(0, s_fake.jtag_writes);
    TEST_ASSERT_EQUAL(0, s_fake.connected_calls);
    TEST_ASSERT_EQUAL_UINT32(1, status().dropped[LOGGING_CH_JTAG]);
    TEST_ASSERT_FALSE(status().jtag_host);
    teardown();
}

/* ---- syslog sink ---- */

TEST_CASE("syslog new target: state resolving, then sending once resolved", "[logging]")
{
    setup();
    logging_drain_configure(ESP_LOG_WARN, "logs.lan:1514");
    TEST_ASSERT_EQUAL(LOGGING_SYSLOG_RESOLVING, status().syslog);
    TEST_ASSERT_EQUAL(0, s_fake.resolves);

    logging_drain_tick(0);
    TEST_ASSERT_EQUAL(1, s_fake.resolves);
    TEST_ASSERT_EQUAL_STRING("logs.lan", s_fake.resolved_host);
    TEST_ASSERT_EQUAL(1514, s_fake.resolved_port);
    TEST_ASSERT_EQUAL(1, s_fake.opens);
    TEST_ASSERT_EQUAL(AF_INET, s_fake.open_family);
    TEST_ASSERT_EQUAL(LOGGING_SYSLOG_SENDING, status().syslog);
    teardown();
}

TEST_CASE("syslog sending: one RFC 5424 datagram per line, sent[SYSLOG] + 1", "[logging]")
{
    setup();
    syslog_on(ESP_LOG_WARN, "192.168.1.10");
    logging_line_t l = line(ESP_LOG_WARN, "zigbee", 813120, "request timeout");

    logging_drain_line(&l, levels(ESP_LOG_NONE, ESP_LOG_WARN));
    TEST_ASSERT_EQUAL(1, s_fake.sends);
    TEST_ASSERT_EQUAL_STRING("<132>1 - esp-otbr-d2cafc zigbee - - - 813120 request timeout", s_fake.datagram);
    TEST_ASSERT_EQUAL(514, s_fake.sent_port);
    TEST_ASSERT_EQUAL_UINT32(1, status().sent[LOGGING_CH_SYSLOG]);
    teardown();
}

TEST_CASE("syslog with a valid clock: TIMESTAMP from the clock hook", "[logging]")
{
    setup();
    s_fake.clock_valid = true;
    logging_set_clock(fake_clock);
    syslog_on(ESP_LOG_INFO, "192.168.1.10");
    logging_line_t l = line(ESP_LOG_INFO, "zigbee", 1, "up");

    logging_drain_line(&l, levels(ESP_LOG_NONE, ESP_LOG_INFO));
    TEST_ASSERT_NOT_NULL(strstr(s_fake.datagram, " 2026-11-10T12:00:00.123Z esp-otbr-d2cafc "));
    teardown();
}

TEST_CASE("syslog with a clock hook that reports an invalid clock: TIMESTAMP is NILVALUE", "[logging]")
{
    setup();
    logging_set_clock(fake_clock);
    syslog_on(ESP_LOG_INFO, "192.168.1.10");
    logging_line_t l = line(ESP_LOG_INFO, "zigbee", 1, "up");

    logging_drain_line(&l, levels(ESP_LOG_NONE, ESP_LOG_INFO));
    TEST_ASSERT_EQUAL(0, strncmp(s_fake.datagram, "<134>1 - esp-otbr-d2cafc ", 25));
    teardown();
}

TEST_CASE("syslog record above the syslog level: not sent, nothing counted", "[logging]")
{
    setup();
    syslog_on(ESP_LOG_WARN, "192.168.1.10");
    logging_line_t l = line(ESP_LOG_INFO, "zigbee", 1, "up");

    logging_drain_line(&l, levels(ESP_LOG_NONE, ESP_LOG_WARN));
    TEST_ASSERT_EQUAL(0, s_fake.sends);
    TEST_ASSERT_EQUAL_UINT32(0, status().sent[LOGGING_CH_SYSLOG]);
    TEST_ASSERT_EQUAL_UINT32(0, status().dropped[LOGGING_CH_SYSLOG]);
    teardown();
}

TEST_CASE("syslog on an unresolvable name: state unresolved", "[logging]")
{
    setup();
    s_fake.resolve_result = ESP_FAIL;
    syslog_on(ESP_LOG_WARN, "nowhere.lan");
    TEST_ASSERT_EQUAL(LOGGING_SYSLOG_UNRESOLVED, status().syslog);
    TEST_ASSERT_EQUAL(0, s_fake.opens);
    teardown();
}

TEST_CASE("syslog on an unresolvable name: retried after 5, 10, 20, 40, then every 60 s", "[logging]")
{
    static const uint32_t tries_at[] = {5000, 15000, 35000, 75000, 135000, 195000};

    setup();
    s_fake.resolve_result = ESP_FAIL;
    syslog_on(ESP_LOG_WARN, "nowhere.lan");
    TEST_ASSERT_EQUAL(1, s_fake.resolves);
    for (int i = 0; i < 6; i++) {
        logging_drain_tick(tries_at[i] - 1);
        TEST_ASSERT_EQUAL(1 + i, s_fake.resolves);
        logging_drain_tick(tries_at[i]);
        TEST_ASSERT_EQUAL(2 + i, s_fake.resolves);
    }
    TEST_ASSERT_EQUAL(LOGGING_SYSLOG_UNRESOLVED, status().syslog);

    s_fake.resolve_result = ESP_OK;
    logging_drain_tick(255000);
    TEST_ASSERT_EQUAL(8, s_fake.resolves);
    TEST_ASSERT_EQUAL(LOGGING_SYSLOG_SENDING, status().syslog);
    teardown();
}

TEST_CASE("syslog retry across the uptime counter wrap: still after 5 s", "[logging]")
{
    setup();
    s_fake.resolve_result = ESP_FAIL;
    logging_drain_configure(ESP_LOG_WARN, "nowhere.lan");
    logging_drain_tick(UINT32_MAX - 1000);
    logging_drain_tick(3000);
    TEST_ASSERT_EQUAL(1, s_fake.resolves);
    logging_drain_tick(3999);
    TEST_ASSERT_EQUAL(2, s_fake.resolves);
    teardown();
}

TEST_CASE("syslog name target sending: looked up again every 10 min, the new address used", "[logging]")
{
    setup();
    syslog_on(ESP_LOG_WARN, "logs.lan");
    logging_line_t l = line(ESP_LOG_ERROR, "zigbee", 1, "boom");

    logging_drain_tick(599999);
    TEST_ASSERT_EQUAL(1, s_fake.resolves);
    s_fake.addr_octet = 11;
    logging_drain_tick(600000);
    TEST_ASSERT_EQUAL(2, s_fake.resolves);
    TEST_ASSERT_EQUAL(LOGGING_SYSLOG_SENDING, status().syslog);
    logging_drain_line(&l, levels(ESP_LOG_NONE, ESP_LOG_WARN));
    TEST_ASSERT_EQUAL(11, s_fake.sent_octet);
    TEST_ASSERT_EQUAL(0, s_fake.closes); // same family: socket kept
    logging_drain_tick(1199999);
    TEST_ASSERT_EQUAL(2, s_fake.resolves);
    logging_drain_tick(1200000);
    TEST_ASSERT_EQUAL(3, s_fake.resolves);
    teardown();
}

TEST_CASE("syslog name target, failed refresh: keeps the last address, retried after 5 s", "[logging]")
{
    setup();
    syslog_on(ESP_LOG_WARN, "logs.lan");
    logging_line_t l = line(ESP_LOG_ERROR, "zigbee", 1, "boom");

    s_fake.resolve_result = ESP_FAIL;
    logging_drain_tick(600000);
    TEST_ASSERT_EQUAL(2, s_fake.resolves);
    TEST_ASSERT_EQUAL(LOGGING_SYSLOG_SENDING, status().syslog);
    logging_drain_line(&l, levels(ESP_LOG_NONE, ESP_LOG_WARN));
    TEST_ASSERT_EQUAL(10, s_fake.sent_octet);
    logging_drain_tick(604999);
    TEST_ASSERT_EQUAL(2, s_fake.resolves);
    s_fake.resolve_result = ESP_OK;
    s_fake.addr_octet = 12;
    logging_drain_tick(605000);
    TEST_ASSERT_EQUAL(3, s_fake.resolves);
    logging_drain_line(&l, levels(ESP_LOG_NONE, ESP_LOG_WARN));
    TEST_ASSERT_EQUAL(12, s_fake.sent_octet);
    teardown();
}

TEST_CASE("syslog IP literal target: never looked up again", "[logging]")
{
    setup();
    syslog_on(ESP_LOG_WARN, "192.168.1.10");
    logging_drain_tick(600000);
    logging_drain_tick(36000000);
    logging_drain_configure(ESP_LOG_WARN, "192.168.1.10");
    logging_drain_tick(36000001);
    TEST_ASSERT_EQUAL(1, s_fake.resolves);
    TEST_ASSERT_EQUAL(LOGGING_SYSLOG_SENDING, status().syslog);
    teardown();
}

TEST_CASE("syslog apply with the same name target: socket kept, looked up again at the next tick", "[logging]")
{
    setup();
    syslog_on(ESP_LOG_WARN, "logs.lan");
    logging_drain_configure(ESP_LOG_DEBUG, "logs.lan");
    TEST_ASSERT_EQUAL(LOGGING_SYSLOG_SENDING, status().syslog);
    logging_drain_tick(10);
    TEST_ASSERT_EQUAL(2, s_fake.resolves);
    TEST_ASSERT_EQUAL(0, s_fake.closes);
    TEST_ASSERT_EQUAL(LOGGING_SYSLOG_SENDING, status().syslog);
    teardown();
}

TEST_CASE("syslog on an unresolvable name: lines dropped and counted", "[logging]")
{
    setup();
    s_fake.resolve_result = ESP_FAIL;
    syslog_on(ESP_LOG_WARN, "nowhere.lan");
    logging_line_t l = line(ESP_LOG_ERROR, "zigbee", 1, "boom");

    logging_drain_line(&l, levels(ESP_LOG_NONE, ESP_LOG_WARN));
    logging_drain_line(&l, levels(ESP_LOG_NONE, ESP_LOG_WARN));
    TEST_ASSERT_EQUAL(0, s_fake.sends);
    TEST_ASSERT_EQUAL_UINT32(2, status().dropped[LOGGING_CH_SYSLOG]);
    teardown();
}

TEST_CASE("syslog on a send error: dropped[SYSLOG] + 1, no log line produced", "[logging]")
{
    setup();
    syslog_on(ESP_LOG_WARN, "192.168.1.10");
    s_fake.send_result = -1;
    logging_line_t l = line(ESP_LOG_ERROR, "zigbee", 1, "boom");

    logging_drain_line(&l, levels(ESP_LOG_NONE, ESP_LOG_WARN));
    TEST_ASSERT_EQUAL(1, s_fake.sends);
    TEST_ASSERT_EQUAL_UINT32(1, status().dropped[LOGGING_CH_SYSLOG]);
    TEST_ASSERT_EQUAL_UINT32(0, status().sent[LOGGING_CH_SYSLOG]);
    TEST_ASSERT_EQUAL(LOGGING_SYSLOG_SENDING, status().syslog);
    TEST_ASSERT_EQUAL(0, s_log_lines);
    teardown();
}

TEST_CASE("syslog off: socket closed, state off", "[logging]")
{
    setup();
    syslog_on(ESP_LOG_WARN, "192.168.1.10");
    logging_drain_configure(ESP_LOG_NONE, "192.168.1.10");
    TEST_ASSERT_EQUAL(1, s_fake.closes);
    TEST_ASSERT_EQUAL(FAKE_FD, s_fake.closed_fd);
    TEST_ASSERT_EQUAL(LOGGING_SYSLOG_OFF, status().syslog);

    logging_drain_tick(70000);
    TEST_ASSERT_EQUAL(1, s_fake.resolves);
    teardown();
}

TEST_CASE("syslog target changed: old socket closed, new target resolved", "[logging]")
{
    setup();
    syslog_on(ESP_LOG_WARN, "192.168.1.10");
    logging_drain_configure(ESP_LOG_WARN, "192.168.1.11:1514");
    TEST_ASSERT_EQUAL(1, s_fake.closes);
    TEST_ASSERT_EQUAL(LOGGING_SYSLOG_RESOLVING, status().syslog);
    logging_drain_tick(10);
    TEST_ASSERT_EQUAL_STRING("192.168.1.11", s_fake.resolved_host);
    TEST_ASSERT_EQUAL(1514, s_fake.resolved_port);
    TEST_ASSERT_EQUAL(LOGGING_SYSLOG_SENDING, status().syslog);
    teardown();
}

TEST_CASE("syslog level changed, same target: socket kept, not resolved again", "[logging]")
{
    setup();
    syslog_on(ESP_LOG_WARN, "192.168.1.10");
    logging_drain_configure(ESP_LOG_DEBUG, "192.168.1.10");
    logging_drain_tick(10);
    TEST_ASSERT_EQUAL(0, s_fake.closes);
    TEST_ASSERT_EQUAL(1, s_fake.resolves);
    TEST_ASSERT_EQUAL(LOGGING_SYSLOG_SENDING, status().syslog);
    teardown();
}

/* ---- known tags ---- */

TEST_CASE("drained record: its tag listed in the known tags once", "[logging]")
{
    const char *tags[4];

    setup();
    logging_line_t l = line(ESP_LOG_INFO, "radio_mux", 1, "x");

    logging_drain_line(&l, levels(ESP_LOG_INFO, ESP_LOG_NONE));
    logging_drain_line(&l, levels(ESP_LOG_INFO, ESP_LOG_NONE));
    TEST_ASSERT_EQUAL(1, logging_known_tags(tags, 4));
    TEST_ASSERT_EQUAL_STRING("radio_mux", tags[0]);
    teardown();
}
