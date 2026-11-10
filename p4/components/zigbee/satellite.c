// The ZLOE satellite's bootstrap (zigbee_satellite.h). Pure.
#include <stdio.h>
#include <string.h>

#include "zigbee_satellite.h"

// The control protocol: a 3-byte header (command, then frame type in the low 2 bits and the request id above them,
// little-endian), then the payload. Requests carry the payload directly, responses a status byte first.
#define HEADER_LEN 3
#define FRAME_REQUEST 0
#define FRAME_RESPONSE 1
#define FRAME_NOTIFICATION 3
#define CMD_GET_HW_ADDRESS 0x02
#define CMD_CONFIGURE 0x10
#define CMD_START_NETWORK 0x16
#define CMD_SEND_UNICAST 0x30
#define CMD_SEND_BROADCAST 0x31
#define CMD_NONE 0xFF
#define NOTIFY_RECEIVED_APS 0x10

// Offsets in NetworkState.
#define ST_EPAN 4
#define ST_NWK 12
#define ST_IEEE 14
#define ST_NWK_KEY_COUNTER 39
#define ST_HAS_TCLK_SEED 59
#define ST_APS_COUNTER 78
// NetworkInfoPayload: NetworkState, key_count:u16, then a byte whose low bit is `started`.
#define INFO_STARTED ZIGBEE_SATELLITE_STATE_LEN + 2
#define INFO_LEN (INFO_STARTED + 1)

#define NODE_ROLE_ROUTER 1
#define RADIUS 30
#define BROADCAST_ROUTERS 0xFFFD
#define ZDP_DEVICE_ANNCE 0x0013
#define ZDP_NODE_DESC_REQ 0x0002
#define ZDP_ACTIVE_EP_REQ 0x0005
#define ZDP_RSP 0x8000
// Router, mains powered, receiver on when idle, allocate address.
#define ROUTER_CAPABILITY 0x8E
#define STACK_COMPLIANCE (22u << 9)
#define EPOCH_2026 1767225600

// ReceivedApsPayload.
#define APS_PROFILE 7
#define APS_CLUSTER 9
#define APS_SOURCE 0
#define APS_DST_EP 12
#define APS_DATA_LEN 15
#define APS_DATA 17

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = v & 0xFF;
    p[1] = v >> 8;
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v & 0xFFFF);
    put16(p + 2, v >> 16);
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static size_t put_header(zigbee_satellite_t *s, uint8_t *out, uint8_t command)
{
    s->request_id = (s->request_id + 1) & 0x3FFF;
    out[0] = command;
    put16(&out[1], (uint16_t)(FRAME_REQUEST | (s->request_id << 2)));
    return HEADER_LEN;
}

static void fail(zigbee_satellite_t *s, const char *reason)
{
    s->state = ZIGBEE_SATELLITE_FAILED;
    s->pending = CMD_NONE;
    strlcpy(s->reason, reason, sizeof(s->reason));
}

// A request whose response moves the bootstrap on.
static void request(zigbee_satellite_t *s, uint8_t command, const uint8_t *payload, size_t len)
{
    uint8_t frame[HEADER_LEN + 2 + ZIGBEE_SATELLITE_STATE_LEN];
    const size_t header = put_header(s, frame, command);

    if (len > 0) {
        memcpy(&frame[header], payload, len);
    }
    s->pending = command;
    s->deadline_ms = s->ops->now_ms(s->ctx) + ZIGBEE_SATELLITE_REQUEST_TIMEOUT_MS;
    s->ops->host_frame(s->ctx, frame, header + len);
}

void zigbee_satellite_init(zigbee_satellite_t *s, const zigbee_satellite_ops_t *ops, void *ctx)
{
    memset(s, 0, sizeof(*s));
    s->ops = ops;
    s->ctx = ctx;
    s->pending = CMD_NONE;
}

void zigbee_satellite_start(zigbee_satellite_t *s)
{
    s->state = ZIGBEE_SATELLITE_STARTING;
    request(s, CMD_GET_HW_ADDRESS, NULL, 0);
}

uint16_t zigbee_satellite_derive_nwk(const uint8_t eui64[8])
{
    uint32_t hash = 2166136261u;

    for (int i = 0; i < 8; i++) {
        hash = (hash ^ eui64[i]) * 16777619u;
    }
    return (uint16_t)((uint16_t)(hash ^ (hash >> 16)) % 0xFFF7 + 1);
}

uint32_t zigbee_satellite_clock_counter(int64_t unix_seconds)
{
    return unix_seconds > EPOCH_2026 ? (uint32_t)((unix_seconds - EPOCH_2026) * 10) : 0;
}

const char *zigbee_satellite_state_name(zigbee_satellite_state_t state)
{
    switch (state) {
    case ZIGBEE_SATELLITE_STARTING:
        return "starting";
    case ZIGBEE_SATELLITE_WAITING_CLOCK:
        return "waiting_clock";
    case ZIGBEE_SATELLITE_WAITING_COORDINATOR:
        return "waiting_coordinator";
    case ZIGBEE_SATELLITE_CONFIGURING:
        return "configuring";
    case ZIGBEE_SATELLITE_RUNNING:
        return "running";
    case ZIGBEE_SATELLITE_FAILED:
        return "failed";
    default:
        return "idle";
    }
}

void zigbee_satellite_on_tick(zigbee_satellite_t *s)
{
    const int64_t now = s->ops->now_ms(s->ctx);
    int64_t unix_seconds;

    switch (s->state) {
    case ZIGBEE_SATELLITE_WAITING_CLOCK:
        if (s->ops->clock_unix(s->ctx, &unix_seconds)) {
            s->state = ZIGBEE_SATELLITE_WAITING_COORDINATOR;
            s->deadline_ms = now + ZIGBEE_SATELLITE_COORDINATOR_TIMEOUT_MS;
            s->next_netinfo_ms = now;
            zigbee_satellite_on_tick(s);
        } else if (now >= s->deadline_ms) {
            fail(s, "no clock");
        }
        break;
    case ZIGBEE_SATELLITE_WAITING_COORDINATOR:
        if (now >= s->deadline_ms) {
            fail(s, "no coordinator");
        } else if (now >= s->next_netinfo_ms) {
            s->next_netinfo_ms = now + ZIGBEE_SATELLITE_NETINFO_PERIOD_MS;
            s->ops->request_netinfo(s->ctx);
        }
        break;
    case ZIGBEE_SATELLITE_STARTING:
        if (now >= s->deadline_ms) {
            fail(s, "no answer to GetHwAddress");
        }
        break;
    case ZIGBEE_SATELLITE_CONFIGURING:
        if (now >= s->deadline_ms) {
            fail(s, s->pending == CMD_CONFIGURE ? "no answer to Configure" : "no answer to StartNetwork");
        }
        break;
    default:
        break;
    }
}

void zigbee_satellite_on_netinfo(zigbee_satellite_t *s, const uint8_t *data, size_t len)
{
    int64_t unix_seconds;

    if (s->state != ZIGBEE_SATELLITE_WAITING_COORDINATOR || len < INFO_LEN || !(data[INFO_STARTED] & 1) ||
            !s->ops->clock_unix(s->ctx, &unix_seconds)) {
        return;
    }
    uint8_t payload[2 + ZIGBEE_SATELLITE_STATE_LEN];
    uint8_t *state = &payload[2];
    const uint32_t counter = zigbee_satellite_clock_counter(unix_seconds);

    payload[0] = NODE_ROLE_ROUTER;
    payload[1] = 0; // no source routing
    memcpy(state, data, ZIGBEE_SATELLITE_STATE_LEN);
    s->nwk = zigbee_satellite_derive_nwk(s->eui64);
    memcpy(s->epan, &state[ST_EPAN], sizeof(s->epan));
    put16(&state[ST_NWK], s->nwk);
    memcpy(&state[ST_IEEE], s->eui64, sizeof(s->eui64));
    put32(&state[ST_NWK_KEY_COUNTER], counter);
    state[ST_HAS_TCLK_SEED] = 0;
    put32(&state[ST_APS_COUNTER], counter);
    s->ops->set_epan(s->ctx, s->epan);
    s->state = ZIGBEE_SATELLITE_CONFIGURING;
    request(s, CMD_CONFIGURE, payload, sizeof(payload));
}

// ZDP Device_annce, as any router does after (re)joining.
static void announce(zigbee_satellite_t *s)
{
    uint8_t frame[HEADER_LEN + 14 + 12];
    size_t n = put_header(s, frame, CMD_SEND_BROADCAST);

    frame[n++] = 0; // flags
    put16(&frame[n], BROADCAST_ROUTERS);
    n += 2;
    put16(&frame[n], 0); // ZDP profile
    n += 2;
    put16(&frame[n], ZDP_DEVICE_ANNCE);
    n += 2;
    frame[n++] = 0; // source endpoint
    frame[n++] = 0; // destination endpoint
    frame[n++] = 0; // APS counter: the stack's
    frame[n++] = RADIUS;
    frame[n++] = 0; // priority: normal
    put16(&frame[n], 12);
    n += 2;
    frame[n++] = 0; // ZDP sequence
    put16(&frame[n], s->nwk);
    n += 2;
    memcpy(&frame[n], s->eui64, 8);
    n += 8;
    frame[n++] = ROUTER_CAPABILITY;
    s->ops->host_frame(s->ctx, frame, n);
}

static void on_response(zigbee_satellite_t *s, uint8_t command, uint16_t id, const uint8_t *body, size_t len)
{
    char why[ZIGBEE_SATELLITE_REASON_MAX];

    if (command != s->pending || id != s->request_id || len < 1) {
        return; // the response to a send of ours, or a stale one
    }
    if (body[0] != 0) {
        snprintf(why, sizeof(why), "command 0x%02x refused: status 0x%02x", command, body[0]);
        fail(s, why);
        return;
    }
    switch (command) {
    case CMD_GET_HW_ADDRESS:
        if (len < 9) {
            fail(s, "malformed GetHwAddress response");
            return;
        }
        memcpy(s->eui64, &body[1], sizeof(s->eui64));
        s->pending = CMD_NONE;
        s->state = ZIGBEE_SATELLITE_WAITING_CLOCK;
        s->deadline_ms = s->ops->now_ms(s->ctx) + ZIGBEE_SATELLITE_CLOCK_TIMEOUT_MS;
        break;
    case CMD_CONFIGURE:
        request(s, CMD_START_NETWORK, NULL, 0);
        break;
    case CMD_START_NETWORK:
        s->pending = CMD_NONE;
        s->state = ZIGBEE_SATELLITE_RUNNING;
        s->ops->announce(s->ctx, s->eui64, s->epan, s->nwk);
        announce(s);
        break;
    default:
        break;
    }
}

// A unicast ZDP answer to `dest`.
static void zdp_reply(zigbee_satellite_t *s, uint16_t dest, uint16_t cluster, const uint8_t *asdu, size_t len)
{
    uint8_t frame[HEADER_LEN + 24 + 20];
    size_t n = put_header(s, frame, CMD_SEND_UNICAST);

    frame[n++] = 0; // flags: no EUI64, no APS ack, no APS encryption
    put16(&frame[n], dest);
    n += 2;
    memset(&frame[n], 0, 8); // destination EUI64: unused
    n += 8;
    put16(&frame[n], 0); // ZDP profile
    n += 2;
    put16(&frame[n], cluster);
    n += 2;
    frame[n++] = 0; // source endpoint
    frame[n++] = 0; // destination endpoint
    frame[n++] = 0; // APS counter: the stack's
    frame[n++] = RADIUS;
    frame[n++] = 0; // priority: normal
    frame[n++] = 0; // route: the stack decides
    put16(&frame[n], (uint16_t)len);
    n += 2;
    memcpy(&frame[n], asdu, len);
    n += len;
    s->ops->host_frame(s->ctx, frame, n);
}

// Answers the requests of zigpy's interview for our own address; it gives up on a device otherwise.
static void on_received_aps(zigbee_satellite_t *s, const uint8_t *p, size_t len)
{
    if (s->state != ZIGBEE_SATELLITE_RUNNING || len < APS_DATA || get16(&p[APS_PROFILE]) != 0 || p[APS_DST_EP] != 0) {
        return;
    }
    const size_t data_len = get16(&p[APS_DATA_LEN]);
    const uint8_t *data = &p[APS_DATA];

    if (data_len < 3 || len < APS_DATA + data_len || get16(&data[1]) != s->nwk) {
        return;
    }
    const uint16_t cluster = get16(&p[APS_CLUSTER]);
    const uint16_t source = get16(&p[APS_SOURCE]);

    if (cluster == ZDP_NODE_DESC_REQ) {
        // Node descriptor: router, 2.4 GHz, buffer 82, transfer sizes 128, stack compliance revision 22.
        uint8_t rsp[17] = {data[0], 0x00, data[1], data[2], 0x01, 0x40, ROUTER_CAPABILITY, 0x00, 0x00, 0x52};

        put16(&rsp[10], 128);
        put16(&rsp[12], STACK_COMPLIANCE);
        put16(&rsp[14], 128);
        rsp[16] = 0x00;
        zdp_reply(s, source, ZDP_RSP | ZDP_NODE_DESC_REQ, rsp, sizeof(rsp));
    } else if (cluster == ZDP_ACTIVE_EP_REQ) {
        const uint8_t rsp[5] = {data[0], 0x00, data[1], data[2], 0x00}; // no endpoints

        zdp_reply(s, source, ZDP_RSP | ZDP_ACTIVE_EP_REQ, rsp, sizeof(rsp));
    }
}

void zigbee_satellite_on_frame(zigbee_satellite_t *s, const uint8_t *frame, size_t len)
{
    if (len < HEADER_LEN) {
        return;
    }
    const uint16_t type_id = get16(&frame[1]);
    const unsigned type = type_id & 3;

    if (type == FRAME_RESPONSE) {
        on_response(s, frame[0], type_id >> 2, &frame[HEADER_LEN], len - HEADER_LEN);
    } else if (type == FRAME_NOTIFICATION && frame[0] == NOTIFY_RECEIVED_APS) {
        on_received_aps(s, &frame[HEADER_LEN], len - HEADER_LEN);
    }
}
