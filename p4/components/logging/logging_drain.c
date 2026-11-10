// Drain of the logging component: the sinks behind the transit ring. Each record goes to USB-Serial-JTAG (when its
// driver is installed, a host is there and its TX buffer has room, else it is dropped) and to syslog over UDP (when the
// target is resolved, else it is dropped), counted per channel, and to the queue of each live viewer whose level it
// passes (logging_viewers.c). A syslog name target is looked up again every 10 min
// and on every settings apply; a failed lookup is retried after 5 s, doubling up to 60 s. Nothing here logs: the drain task is a sink (logging_sink_enter), and the sinks
// count their errors instead. The sink logic is host-tested through the ops seam; the task, the ESP-IDF ops and
// logging_drain_start are target-only.

#include <arpa/inet.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "logging_core.h"
#include "logging_priv.h"
#include "logging_text.h"

/** "X (<uptime>) <tag>: <body>\n", as ESP-IDF writes it to the UART without the colour. */
#define JTAG_LINE_MAX (LOGGING_LINE_MAX + LOGGING_TAG_MAX + sizeof("X (4294967295) : \n"))
/** RFC 5424 header (PRI, version, TIMESTAMP, a 63-byte HOSTNAME, a 48-byte APP-NAME, NILVALUEs, uptime) plus body. */
#define SYSLOG_DGRAM_MAX (LOGGING_LINE_MAX + 192)

static const char LEVEL_LETTERS[] = "NEWIDV"; // indexed by esp_log_level_t

typedef struct {
    uint32_t state; /**< logging_syslog_state_t; atomic: read by logging_drain_status on any task. */
    char target[LOGGING_TARGET_MAX];
    char host[LOGGING_TARGET_MAX]; /**< `target` parsed; IPv6 without brackets. */
    uint16_t port;
    bool literal; /**< `host` is an IP address: looked up once, never again. */
    int fd;
    int family;
    struct sockaddr_storage addr;
    socklen_t addr_len;
    bool due;              /**< Look up at the next tick, whatever the time. */
    uint32_t last_try_ms;  /**< When the last lookup was made. */
    uint32_t interval_ms;  /**< Time from `last_try_ms` to the next lookup. */
    uint32_t backoff_ms;   /**< Interval after the next failed lookup. */
} syslog_sink_t;

static const logging_drain_ops_t *s_ops;
static syslog_sink_t s_syslog = {.fd = -1};
static uint32_t s_sent[LOGGING_CH_COUNT];    // atomic: written by the drain task, read by any task
static uint32_t s_dropped[LOGGING_CH_COUNT]; // idem
static bool (*s_clock)(struct timeval *out); // atomic pointer
static logging_tagset_t s_tags;              // grown by the drain task only; read lock-free (logging_tagset_add)
// Drain task only: kept off its stack.
static char s_jtag_line[JTAG_LINE_MAX];
static char s_dgram[SYSLOG_DGRAM_MAX];

static void count(uint32_t *counter)
{
    __atomic_fetch_add(counter, 1, __ATOMIC_RELAXED);
}

static void syslog_set_state(logging_syslog_state_t state)
{
    __atomic_store_n(&s_syslog.state, (uint32_t) state, __ATOMIC_RELAXED);
}

static void syslog_close(void)
{
    if (s_syslog.fd >= 0) {
        s_ops->udp_close(s_syslog.fd);
        s_syslog.fd = -1;
    }
}

void logging_drain_set_ops(const logging_drain_ops_t *ops)
{
    s_ops = ops;
}

void logging_drain_reset(void)
{
    if (s_ops) {
        syslog_close();
    }
    memset(&s_syslog, 0, sizeof(s_syslog));
    s_syslog.fd = -1;
    memset(s_sent, 0, sizeof(s_sent));
    memset(s_dropped, 0, sizeof(s_dropped));
    memset(&s_tags, 0, sizeof(s_tags));
}

void logging_set_clock(bool (*now)(struct timeval *out))
{
    __atomic_store_n(&s_clock, now, __ATOMIC_RELEASE);
}

static bool is_ip_literal(const char *host)
{
    struct in6_addr a6;
    struct in_addr a4;

    return inet_pton(AF_INET, host, &a4) == 1 || inet_pton(AF_INET6, host, &a6) == 1;
}

void logging_drain_configure(esp_log_level_t syslog_level, const char *target)
{
    logging_syslog_state_t state = (logging_syslog_state_t) __atomic_load_n(&s_syslog.state, __ATOMIC_RELAXED);

    if (syslog_level == ESP_LOG_NONE || !target || target[0] == '\0') {
        syslog_close();
        s_syslog.target[0] = '\0';
        syslog_set_state(LOGGING_SYSLOG_OFF);
        return;
    }
    if (state != LOGGING_SYSLOG_OFF && strcmp(target, s_syslog.target) == 0) {
        // Same target: keep the socket. A name is looked up again (an apply is how a user says "the address moved").
        if (!s_syslog.literal) {
            s_syslog.due = true;
            s_syslog.backoff_ms = LOGGING_SYSLOG_RETRY_MIN_MS;
        }
        return;
    }
    syslog_close();
    strlcpy(s_syslog.target, target, sizeof(s_syslog.target));
    if (logging_target_parse(target, s_syslog.host, sizeof(s_syslog.host), &s_syslog.port) != ESP_OK) {
        s_syslog.host[0] = '\0'; // validated settings never get here; the lookup then fails and is retried
    }
    s_syslog.literal = s_syslog.host[0] != '\0' && is_ip_literal(s_syslog.host);
    s_syslog.due = true;
    s_syslog.backoff_ms = LOGGING_SYSLOG_RETRY_MIN_MS;
    syslog_set_state(LOGGING_SYSLOG_RESOLVING);
}

/**
 * Look the target up. Success: (re)open a socket when the family changed, state sending, next lookup in 10 min (a
 * name) or never (an IP address). Failure: unresolved, or still sending to the last address when there is one; the
 * next try follows the back-off.
 */
static void syslog_resolve(void)
{
    struct sockaddr_storage addr;
    socklen_t addr_len = 0;
    bool sending = __atomic_load_n(&s_syslog.state, __ATOMIC_RELAXED) == LOGGING_SYSLOG_SENDING;

    if (s_syslog.host[0] == '\0' || s_ops->resolve(s_syslog.host, s_syslog.port, &addr, &addr_len) != ESP_OK) {
        s_syslog.interval_ms = s_syslog.backoff_ms;
        s_syslog.backoff_ms = s_syslog.backoff_ms * 2 < LOGGING_SYSLOG_RETRY_MAX_MS ? s_syslog.backoff_ms * 2 :
                              LOGGING_SYSLOG_RETRY_MAX_MS;
        if (!sending) {
            syslog_set_state(LOGGING_SYSLOG_UNRESOLVED);
        }
        return;
    }
    s_syslog.addr = addr;
    s_syslog.addr_len = addr_len;
    if (s_syslog.fd < 0 || s_syslog.family != addr.ss_family) {
        syslog_close();
        s_syslog.family = addr.ss_family;
        s_syslog.fd = s_ops->udp_open(s_syslog.family);
    }
    if (s_syslog.fd < 0) {
        s_syslog.interval_ms = s_syslog.backoff_ms;
        s_syslog.backoff_ms = s_syslog.backoff_ms * 2 < LOGGING_SYSLOG_RETRY_MAX_MS ? s_syslog.backoff_ms * 2 :
                              LOGGING_SYSLOG_RETRY_MAX_MS;
        syslog_set_state(LOGGING_SYSLOG_UNRESOLVED);
        return;
    }
    s_syslog.backoff_ms = LOGGING_SYSLOG_RETRY_MIN_MS;
    s_syslog.interval_ms = LOGGING_SYSLOG_REFRESH_MS;
    syslog_set_state(LOGGING_SYSLOG_SENDING);
}

void logging_drain_tick(uint32_t now_ms)
{
    logging_syslog_state_t state = (logging_syslog_state_t) __atomic_load_n(&s_syslog.state, __ATOMIC_RELAXED);

    if (state == LOGGING_SYSLOG_OFF || (state == LOGGING_SYSLOG_SENDING && s_syslog.literal && !s_syslog.due)) {
        return;
    }
    // Unsigned difference: still right when the millisecond uptime wraps.
    if (!s_syslog.due && now_ms - s_syslog.last_try_ms < s_syslog.interval_ms) {
        return;
    }
    s_syslog.due = false;
    s_syslog.last_try_ms = now_ms;
    syslog_resolve();
}

static void sink_jtag(const logging_line_t *line)
{
    int n;

    // No driver (its install failed, logged once at boot) or no host: dropped, nothing formatted.
    if (!s_ops->jtag_write || !s_ops->jtag_connected()) {
        count(&s_dropped[LOGGING_CH_JTAG]);
        return;
    }
    n = snprintf(s_jtag_line, sizeof(s_jtag_line), "%c (%" PRIu32 ") %s: %.*s\n", LEVEL_LETTERS[line->level],
                 line->uptime_ms, line->tag, (int) line->len, line->text);
    if (n < 0 || (size_t) n >= sizeof(s_jtag_line)) {
        count(&s_dropped[LOGGING_CH_JTAG]); // cannot happen with the sizes above; never send a cut line
        return;
    }
    if (s_ops->jtag_write(s_jtag_line, (size_t) n) == n) {
        count(&s_sent[LOGGING_CH_JTAG]);
    } else {
        count(&s_dropped[LOGGING_CH_JTAG]);
    }
}

static void sink_syslog(const logging_line_t *line)
{
    bool (*clock)(struct timeval * out) = __atomic_load_n(&s_clock, __ATOMIC_ACQUIRE);
    struct timeval wall;
    const char *hostname = s_ops->hostname ? s_ops->hostname() : NULL;
    int n;

    if (__atomic_load_n(&s_syslog.state, __ATOMIC_RELAXED) != LOGGING_SYSLOG_SENDING) {
        count(&s_dropped[LOGGING_CH_SYSLOG]);
        return;
    }
    n = logging_syslog_format(s_dgram, sizeof(s_dgram), line->level, hostname ? hostname : "-", line->tag,
                              line->uptime_ms, clock && clock(&wall) ? &wall : NULL, line->text, line->len);
    if (n < 0 || s_ops->udp_send(s_syslog.fd, s_dgram, (size_t) n, (const struct sockaddr *) &s_syslog.addr,
                                 s_syslog.addr_len) < 0) {
        count(&s_dropped[LOGGING_CH_SYSLOG]);
        return;
    }
    count(&s_sent[LOGGING_CH_SYSLOG]);
}

void logging_drain_line(const logging_line_t *line, logging_levels_t lv)
{
    if (line->level <= ESP_LOG_NONE || line->level > ESP_LOG_VERBOSE) {
        return;
    }
    logging_tagset_add(&s_tags, line->tag); // no lock: the drain is the only writer, readers need none
    if (line->level <= lv.channel[LOGGING_CH_JTAG]) {
        sink_jtag(line);
    }
    if (line->level <= lv.channel[LOGGING_CH_SYSLOG]) {
        sink_syslog(line);
    }
    if (line->level <= lv.viewers) {
        logging_viewers_fanout(line); // no viewer: lv.viewers is off, nothing to do
    }
}

void logging_drain_status(logging_status_t *out)
{
    out->jtag_host = s_ops && s_ops->jtag_write && s_ops->jtag_connected();
    out->syslog = (logging_syslog_state_t) __atomic_load_n(&s_syslog.state, __ATOMIC_RELAXED);
    for (int ch = 0; ch < LOGGING_CH_COUNT; ch++) {
        if (ch != LOGGING_CH_UART) {
            out->sent[ch] = __atomic_load_n(&s_sent[ch], __ATOMIC_RELAXED);
            out->dropped[ch] = __atomic_load_n(&s_dropped[ch], __ATOMIC_RELAXED);
        }
    }
}

size_t logging_known_tags(const char **out, size_t cap)
{
    // Lock-free: the set only grows, an entry is written before the count that publishes it (logging_tagset_add) and
    // never changes after, so the pointers stay valid.
    return logging_tagset_list(&s_tags, out, cap);
}

#if !CONFIG_IDF_TARGET_LINUX

#include <netdb.h>
#include <unistd.h>
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "board_cores.h"
#include "netnames.h"

static const char *TAG = "logging";

#define JTAG_TX_BUFFER 1024 /**< Two long lines; a host that does not read makes the rest drop. */
#define JTAG_RX_BUFFER 128  /**< Unused (nothing reads), but the driver wants one above its 64-byte endpoint. */
/**
 * The drain task's stack: the sinks' buffers are static, the deepest paths are logging_syslog_format (~400 B),
 * snprintf, getaddrinfo, and the UART write of a line the task itself logs (ESP-IDF's vprintf). Check
 * stack_free_min_bytes of "log_out" in /device/diag on the board and trim.
 */
#define DRAIN_STACK 5120
#define DRAIN_PRIORITY (tskIDLE_PRIORITY + 2) // below the HTTP server (tskIDLE_PRIORITY + 5)
#define DRAIN_IDLE_MS 1000                    // wake-up without lines: syslog retries, settings changes

static bool s_jtag_installed;
static logging_settings_t s_settings; // drain task only: too big for its stack
static char s_text[LOGGING_LINE_MAX];

static bool jtag_connected(void)
{
    return s_jtag_installed && usb_serial_jtag_is_connected();
}

static int jtag_write(const void *buf, size_t len)
{
    return usb_serial_jtag_write_bytes(buf, len, 0);
}

static esp_err_t resolve(const char *host, uint16_t port, struct sockaddr_storage *addr, socklen_t *addr_len)
{
    const struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM};
    struct addrinfo *res = NULL;
    esp_err_t err = ESP_FAIL;

    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
        return ESP_FAIL;
    }
    if (res->ai_family == AF_INET && res->ai_addrlen <= sizeof(*addr)) {
        memcpy(addr, res->ai_addr, res->ai_addrlen);
        ((struct sockaddr_in *) addr)->sin_port = htons(port);
        *addr_len = res->ai_addrlen;
        err = ESP_OK;
    } else if (res->ai_family == AF_INET6 && res->ai_addrlen <= sizeof(*addr)) {
        memcpy(addr, res->ai_addr, res->ai_addrlen);
        ((struct sockaddr_in6 *) addr)->sin6_port = htons(port);
        *addr_len = res->ai_addrlen;
        err = ESP_OK;
    }
    freeaddrinfo(res);
    return err;
}

static int udp_open(int family)
{
    return socket(family, SOCK_DGRAM, IPPROTO_UDP);
}

static int udp_send(int fd, const void *buf, size_t len, const struct sockaddr *to, socklen_t to_len)
{
    return sendto(fd, buf, len, MSG_DONTWAIT, to, to_len);
}

static void udp_close(int fd)
{
    close(fd);
}

static logging_drain_ops_t s_target_ops = {
    .jtag_connected = jtag_connected,
    .jtag_write = jtag_write,
    .resolve = resolve,
    .udp_open = udp_open,
    .udp_send = udp_send,
    .udp_close = udp_close,
    .hostname = netnames_hostname,
};

static uint32_t uptime_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void drain_task(void *arg)
{
    logging_ring_t *ring = arg;
    uint32_t seen = 0;
    bool configured = false;
    logging_line_t line;

    logging_sink_enter(); // for good: whatever the sinks log (lwIP, the JTAG driver) goes to the UART only
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(DRAIN_IDLE_MS));
        uint32_t generation = logging_settings_generation();

        if (!configured || generation != seen) {
            seen = generation;
            configured = logging_get_settings(&s_settings) == ESP_OK;
            if (configured) {
                logging_drain_configure(s_settings.channel[LOGGING_CH_SYSLOG], s_settings.syslog_target);
            }
        }
        logging_drain_tick(uptime_ms());
        while (logging_ring_pop(ring, &line, s_text, sizeof(s_text))) {
            logging_drain_line(&line, logging_levels());
        }
    }
}

esp_err_t logging_drain_start(logging_ring_t *ring, TaskHandle_t *task)
{
    usb_serial_jtag_driver_config_t jtag = {
        .tx_buffer_size = JTAG_TX_BUFFER,
        .rx_buffer_size = JTAG_RX_BUFFER,
    };
    esp_err_t err = usb_serial_jtag_driver_install(&jtag);

    s_jtag_installed = err == ESP_OK;
    if (!s_jtag_installed) {
        // Once, at boot, before the capture starts (UART only). The JTAG channel then counts every line as dropped
        // and reports no host; this line is what tells the two apart.
        ESP_LOGE(TAG, "USB-Serial-JTAG driver install failed (%s): USB-JTAG gets no lines this boot",
                 esp_err_to_name(err));
        s_target_ops.jtag_write = NULL;
    }
    logging_drain_set_ops(&s_target_ops);
    if (xTaskCreatePinnedToCore(drain_task, "log_out", DRAIN_STACK, ring, DRAIN_PRIORITY, task, MGMT_CORE) !=
            pdPASS) {
        if (s_jtag_installed) {
            usb_serial_jtag_driver_uninstall();
            s_jtag_installed = false;
        }
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

#endif // !CONFIG_IDF_TARGET_LINUX
