// The ZLOE backbone's board side (spike): one raw lwIP UDP PCB (IPv6 only; a raw PCB takes no socket from the budget),
// ESP-IDF mDNS (`_zloe._udp`, browse and announce) on the backbone netif, and the satellite's board operations.
//
// Tasks: the task zigbee_zloe (MGMT_CORE) owns discovery and sends what the zigbee task queues, through the lwIP thread.
// The PCB's receive callback runs on the lwIP thread and only posts to the zigbee task's mailbox. The zigbee task makes
// every ziggurat_zloe_* call and never touches lwIP.
#include <stdatomic.h>
#include <string.h>
#include <sys/time.h>

#include "board_cores.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"
#include "mdns.h"
#include "netcfg.h"
#include "timesync.h"
#include "ziggurat.h"
#include "zigbee_priv.h"
#include "zigbee_zloe_txt.h"

static const char *TAG = "zigbee";

#define ZLOE_PORT 19790
#define ZLOE_TASK_STACK 6144
#define ZLOE_TASK_PRIORITY 6
#define OUT_DEPTH 32
#define POLL_MS 100              // how often the task looks at a running mDNS query
#define QUERY_PERIOD_US (5 * 1000 * 1000)
#define QUERY_TIMEOUT_MS 2000
#define QUERY_MAX_RESULTS 8
#define PEER_MAX_ADDRS 4
#define PEER_MAX_TXT 10
#define STATUS_TICKS 4 // the status is refreshed every 4 ticks of the zigbee task

typedef struct {
    ziggurat_zloe_addr_t to;
    zigbee_frame_t *frame;
} out_t;

// A ZIGBEE_MSG_ZLOE_DATAGRAM's frame: the sender, then the datagram.
typedef struct {
    ziggurat_zloe_addr_t from;
    uint8_t data[];
} datagram_msg_t;

// A ZIGBEE_MSG_ZLOE_PEER's frame.
typedef struct {
    zigbee_zloe_record_t record;
    size_t addr_count;
    ziggurat_zloe_addr_t addrs[PEER_MAX_ADDRS];
} peer_msg_t;

static QueueHandle_t s_out;
static struct udp_pcb *s_pcb;   // lwIP thread after open_pcb
static struct netif *s_netif;   // set once, before the task sends
static atomic_bool s_announce_due;
static zigbee_zloe_record_t s_self;

/* ---- the lwIP thread ---- */

static void on_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    if (IP_IS_V6(addr)) {
        zigbee_frame_t *frame = zigbee_frame_alloc(sizeof(datagram_msg_t) + p->tot_len);

        if (frame != NULL) {
            datagram_msg_t *msg = (datagram_msg_t *)frame->data;
            const zigbee_msg_t m = {.kind = ZIGBEE_MSG_ZLOE_DATAGRAM, .frame = frame};

            memcpy(msg->from.addr, ip_2_ip6(addr)->addr, sizeof(msg->from.addr));
            msg->from.port = port;
            msg->from.scope = netif_get_index(s_netif);
            pbuf_copy_partial(p, msg->data, p->tot_len, 0);
            if (!zigbee_post_msg(&m, false)) {
                zigbee_frame_free(frame);
            }
        }
    }
    pbuf_free(p);
}

static esp_err_t open_pcb(void *ctx)
{
    s_netif = esp_netif_get_netif_impl(ctx);
    s_pcb = udp_new_ip_type(IPADDR_TYPE_V6);
    ESP_RETURN_ON_FALSE(s_pcb, ESP_ERR_NO_MEM, TAG, "ZLOE PCB");
    if (udp_bind(s_pcb, IP6_ADDR_ANY, ZLOE_PORT) != ERR_OK) {
        udp_remove(s_pcb);
        s_pcb = NULL;
        return ESP_FAIL;
    }
    udp_recv(s_pcb, on_recv, NULL);
    return ESP_OK;
}

static esp_err_t send_datagram(void *ctx)
{
    const out_t *out = ctx;
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)out->frame->len, PBUF_RAM);

    if (p == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ip_addr_t dst = {.type = IPADDR_TYPE_V6};

    memcpy(ip_2_ip6(&dst)->addr, out->to.addr, sizeof(out->to.addr));
    ip6_addr_assign_zone(ip_2_ip6(&dst), IP6_UNICAST, s_netif);
    pbuf_take(p, out->frame->data, out->frame->len);
    const err_t err = udp_sendto_if(s_pcb, p, &dst, out->to.port, s_netif);

    pbuf_free(p);
    return err == ERR_OK ? ESP_OK : ESP_FAIL;
}

/* ---- discovery (task zigbee_zloe) ---- */

static void post_peer(const mdns_result_t *r, uint32_t scope)
{
    zigbee_zloe_txt_t items[PEER_MAX_TXT];
    const size_t count = r->txt_count < PEER_MAX_TXT ? r->txt_count : PEER_MAX_TXT;
    zigbee_zloe_record_t record;

    for (size_t i = 0; i < count; i++) {
        items[i] = (zigbee_zloe_txt_t) {
            r->txt[i].key, r->txt[i].value
        };
    }
    if (!zigbee_zloe_txt_parse(items, count, &record)) {
        return;
    }
    uint8_t addrs[PEER_MAX_ADDRS][16];
    size_t n = 0;

    for (const mdns_ip_addr_t *a = r->addr; a != NULL && n < PEER_MAX_ADDRS; a = a->next) {
        if (a->addr.type == ESP_IPADDR_TYPE_V6) {
            memcpy(addrs[n++], a->addr.u_addr.ip6.addr, 16);
        }
    }
    n = zigbee_zloe_keep_link_local(addrs, n);
    if (n == 0) {
        return;
    }
    zigbee_frame_t *frame = zigbee_frame_alloc(sizeof(peer_msg_t));

    if (frame == NULL) {
        return;
    }
    peer_msg_t *msg = (peer_msg_t *)frame->data;
    const zigbee_msg_t m = {.kind = ZIGBEE_MSG_ZLOE_PEER, .frame = frame};

    msg->record = record;
    msg->addr_count = n;
    for (size_t i = 0; i < n; i++) {
        memcpy(msg->addrs[i].addr, addrs[i], 16);
        msg->addrs[i].port = r->port;
        msg->addrs[i].scope = scope;
    }
    if (!zigbee_post_msg(&m, false)) {
        zigbee_frame_free(frame);
    }
}

static void poll_discovery(mdns_search_once_t **search, int64_t *next_query_us, esp_netif_t *netif)
{
    const int64_t now = esp_timer_get_time();

    if (*search == NULL) {
        if (now >= *next_query_us) {
            *next_query_us = now + QUERY_PERIOD_US;
            *search = mdns_query_async_new(NULL, "_zloe", "_udp", MDNS_TYPE_PTR, QUERY_TIMEOUT_MS, QUERY_MAX_RESULTS, NULL);
        }
        return;
    }
    mdns_result_t *results = NULL;
    uint8_t count = 0;

    if (!mdns_query_async_get_results(*search, 0, &results, &count)) {
        return;
    }
    for (const mdns_result_t *r = results; r != NULL; r = r->next) {
        if (r->esp_netif == netif) {
            post_peer(r, netif_get_index(s_netif));
        }
    }
    mdns_query_results_free(results);
    mdns_query_async_delete(*search);
    *search = NULL;
}

static void announce(void)
{
    zigbee_zloe_txt_t items[ZIGBEE_ZLOE_TXT_ITEMS];
    zigbee_zloe_txt_buf_t buf;
    mdns_txt_item_t txt[ZIGBEE_ZLOE_TXT_ITEMS];

    zigbee_zloe_txt_build(&s_self, items, &buf);
    for (size_t i = 0; i < ZIGBEE_ZLOE_TXT_ITEMS; i++) {
        txt[i] = (mdns_txt_item_t) {
            items[i].key, items[i].value
        };
    }
    const esp_err_t err = mdns_service_add(buf.xa, "_zloe", "_udp", ZLOE_PORT, txt, ZIGBEE_ZLOE_TXT_ITEMS);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ZLOE announce: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "ZLOE announced as %s, nwk %s", buf.xa, buf.nwk);
    }
}

static void zloe_task(void *arg)
{
    esp_netif_t *netif;

    while ((netif = netcfg_backbone_netif()) == NULL) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
    const esp_err_t err = esp_netif_tcpip_exec(open_pcb, netif);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ZLOE PCB on port %d: %s", ZLOE_PORT, esp_err_to_name(err));
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "ZLOE listening on UDP %d (IPv6)", ZLOE_PORT);
    mdns_search_once_t *search = NULL;
    int64_t next_query_us = 0;

    for (;;) {
        out_t out;

        if (xQueueReceive(s_out, &out, pdMS_TO_TICKS(POLL_MS)) == pdTRUE) {
            const esp_err_t sent = esp_netif_tcpip_exec(send_datagram, &out);

            if (sent != ESP_OK) {
                ESP_LOGD(TAG, "ZLOE send: %s", esp_err_to_name(sent));
            }
            zigbee_frame_free(out.frame);
        }
        if (atomic_exchange(&s_announce_due, false)) {
            announce();
        }
        poll_discovery(&search, &next_query_us, netif);
    }
}

esp_err_t zigbee_zloe_start(void)
{
    static TaskHandle_t task;

    if (s_out == NULL) {
        s_out = xQueueCreate(OUT_DEPTH, sizeof(out_t));
        ESP_RETURN_ON_FALSE(s_out, ESP_ERR_NO_MEM, TAG, "ZLOE queue");
    }
    if (task == NULL) {
        ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(zloe_task, "zigbee_zloe", ZLOE_TASK_STACK, NULL, ZLOE_TASK_PRIORITY, &task,
                                                    MGMT_CORE) == pdPASS,
                            ESP_ERR_NO_MEM, TAG, "ZLOE task");
    }
    return ESP_OK;
}

/* ---- the zigbee task ---- */

void zigbee_zloe_on_msg(const zigbee_msg_t *m)
{
    if (m->kind == ZIGBEE_MSG_ZLOE_DATAGRAM) {
        const datagram_msg_t *msg = (const datagram_msg_t *)m->frame->data;

        ziggurat_zloe_datagram(msg->data, m->frame->len - sizeof(*msg), &msg->from);
    } else {
        const peer_msg_t *msg = (const peer_msg_t *)m->frame->data;

        ziggurat_zloe_peer_resolved(msg->record.eui64, msg->record.epan, msg->record.nwk, msg->record.coordinator, msg->addrs,
                                    msg->addr_count);
    }
}

bool ziggurat_platform_zloe_send(const ziggurat_zloe_addr_t *to, const uint8_t *data, size_t len)
{
    out_t out = {.to = *to, .frame = zigbee_frame_copy(data, len)};

    if (out.frame == NULL) {
        return false;
    }
    if (xQueueSend(s_out, &out, 0) != pdTRUE) {
        zigbee_frame_free(out.frame);
        return false;
    }
    return true;
}

void ziggurat_platform_zloe_netinfo(const uint8_t *data, size_t len)
{
    zigbee_satellite_on_netinfo(&g_zigbee.satellite, data, len);
}

static int64_t op_now_ms(void *ctx)
{
    return esp_timer_get_time() / 1000;
}

static bool op_clock_unix(void *ctx, int64_t *out)
{
    struct timeval tv;

    if (!timesync_clock_valid() || gettimeofday(&tv, NULL) != 0) {
        return false;
    }
    *out = tv.tv_sec;
    return true;
}

static void op_host_frame(void *ctx, const uint8_t *frame, size_t len)
{
    ziggurat_host_frame(frame, len);
}

static bool op_request_netinfo(void *ctx)
{
    return ziggurat_zloe_request_netinfo();
}

static void op_set_epan(void *ctx, const uint8_t epan[8])
{
    ziggurat_zloe_set_epan(epan);
}

static void op_announce(void *ctx, const uint8_t eui64[8], const uint8_t epan[8], uint16_t nwk)
{
    memcpy(s_self.eui64, eui64, sizeof(s_self.eui64));
    memcpy(s_self.epan, epan, sizeof(s_self.epan));
    s_self.nwk = nwk;
    atomic_store(&s_announce_due, true);
}

const zigbee_satellite_ops_t g_zigbee_satellite_ops = {
    .now_ms = op_now_ms,
    .clock_unix = op_clock_unix,
    .host_frame = op_host_frame,
    .request_netinfo = op_request_netinfo,
    .set_epan = op_set_epan,
    .announce = op_announce,
};

void zigbee_zloe_publish_status(void)
{
    static unsigned ticks;

    if (++ticks < STATUS_TICKS) {
        return;
    }
    ticks = 0;
    const zigbee_satellite_t *s = &g_zigbee.satellite;
    ziggurat_zloe_peer_t peers[ZIGBEE_ZLOE_PEERS_MAX];
    const size_t count = ziggurat_zloe_peers(peers, ZIGBEE_ZLOE_PEERS_MAX);

    taskENTER_CRITICAL(&g_zigbee_status_lock);
    zigbee_satellite_status_t *out = &g_zigbee_status.satellite;

    strlcpy(out->state, zigbee_satellite_state_name(s->state), sizeof(out->state));
    strlcpy(out->reason, s->reason, sizeof(out->reason));
    out->nwk = s->nwk;
    out->peer_count = (uint8_t)count;
    for (size_t i = 0; i < count; i++) {
        memcpy(out->peers[i].eui64, peers[i].eui64, sizeof(out->peers[i].eui64));
        out->peers[i].nwk = peers[i].nwk;
        out->peers[i].live = peers[i].live;
        out->peers[i].lqi = peers[i].lqi;
        out->peers[i].last_rx_age_ms = peers[i].last_rx_age_ms;
    }
    taskEXIT_CRITICAL(&g_zigbee_status_lock);
}
