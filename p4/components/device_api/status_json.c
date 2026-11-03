#include "br_core_mode.h"
#include "device_api_priv.h"
#include "routing_types.h"

const char *device_api_mode_name(int mode)
{
    switch (mode) {
    case BR_MODE_STARTING:
        return "STARTING";
    case BR_MODE_BR_RUNNING:
        return "BR_RUNNING";
    case BR_MODE_SNIFFER:
        return "SNIFFER";
    case BR_MODE_RADIO_FAULT:
        return "RADIO_FAULT";
    default:
        return "UNKNOWN";
    }
}

// otBorderRoutingDhcp6PdState: DISABLED, STOPPED, RUNNING, IDLE; -1 before the border router exists.
static const char *pd_state_name(int state)
{
    static const char *const names[] = {"disabled", "stopped", "running", "idle"};

    if (state == -1) {
        return "uninitialized";
    }
    return state >= 0 && state < 4 ? names[state] : "unknown";
}

// The peer figures only mean something with method bgp: null otherwise.
static void routing_peer_figure(json_out_t *w, const device_api_status_t *s, uint8_t value)
{
    if (s->routing_method == ROUTING_METHOD_BGP) {
        json_out_uint(w, value);
    } else {
        json_out_raw(w, "null");
    }
}

static void write_routing(json_out_t *w, const device_api_status_t *s)
{
    if (!s->routing_started) {
        json_out_raw(w, "null");
        return;
    }
    json_out_raw(w, "{\"method\":");
    json_out_str(w, routing_method_name(s->routing_method));
    json_out_raw(w, ",\"omr_mode\":");
    json_out_str(w, s->routing_omr_mode == ROUTING_OMR_CUSTOM ? "custom" : "auto");
    json_out_raw(w, ",\"announced_count\":");
    json_out_uint(w, s->routing_announced_count);
    json_out_raw(w, ",\"peers_established\":");
    routing_peer_figure(w, s, s->routing_peers_established);
    json_out_raw(w, ",\"peers_configured\":");
    routing_peer_figure(w, s, s->routing_peers_configured);
    json_out_raw(w, "}");
}

// "" is written as null: no value.
static void str_or_null(json_out_t *w, const char *text)
{
    json_out_str(w, text && text[0] ? text : NULL);
}

static void trel_json(json_out_t *w, const device_api_trel_t *t)
{
    if (!t->built) {
        json_out_raw(w, "null");
        return;
    }
    json_out_raw(w, "{\"enabled\":");
    json_out_bool(w, t->enabled);
    if (!t->state_valid) {
        json_out_raw(w, ",\"active\":null,\"peers\":null,\"tx_packets\":null,\"tx_failures\":null,\"rx_packets\":null,\"peer_list\":[]}");
        return;
    }
    json_out_raw(w, ",\"active\":");
    json_out_bool(w, t->active);
    json_out_raw(w, ",\"peers\":");
    json_out_uint(w, t->peers);
    json_out_raw(w, ",\"tx_packets\":");
    json_out_u64(w, t->tx_packets);
    json_out_raw(w, ",\"tx_failures\":");
    json_out_u64(w, t->tx_failures);
    json_out_raw(w, ",\"rx_packets\":");
    json_out_u64(w, t->rx_packets);
    json_out_raw(w, ",\"peer_list\":[");
    for (size_t i = 0; i < t->peer_count && i < DEVICE_API_TREL_PEERS_MAX; i++) {
        json_out_raw(w, i > 0 ? ",{\"ext_address\":" : "{\"ext_address\":");
        json_out_str(w, t->peer_list[i].ext_address);
        json_out_raw(w, ",\"address\":");
        json_out_str(w, t->peer_list[i].address);
        json_out_raw(w, "}");
    }
    json_out_raw(w, "]}");
}

int device_api_status_json(char *buf, size_t cap, const device_api_status_t *s)
{
    json_out_t w;

    json_out_init(&w, buf, cap);
    json_out_raw(&w, "{\"mode\":");
    json_out_str(&w, device_api_mode_name(s->mode));
    json_out_raw(&w, ",\"app\":{\"version\":");
    json_out_str(&w, s->app_version);
    json_out_raw(&w, ",\"slot\":");
    json_out_int(&w, s->app_slot);
    json_out_raw(&w, "},\"rcp\":{\"installed\":");
    json_out_str(&w, s->rcp_installed_known ? s->rcp_installed : NULL);
    json_out_raw(&w, ",\"compatible\":");
    json_out_bool(&w, s->rcp_compatible);
    json_out_raw(&w, "},\"jumper\":{\"present\":");
    json_out_bool(&w, s->jumper_present);
    json_out_raw(&w, ",\"base_expected\":");
    json_out_str(&w, s->base_expected);
    json_out_raw(&w, ",\"base_installed\":");
    str_or_null(&w, s->base_installed);
    json_out_raw(&w, ",\"recovery_failed\":");
    json_out_bool(&w, s->jumper_recovery_failed);
    json_out_raw(&w, "},\"eth\":{\"ipv4\":");
    str_or_null(&w, s->ipv4);
    json_out_raw(&w, ",\"ipv6\":[");
    for (size_t i = 0; i < s->ipv6_count && i < DEVICE_API_MAX_IPV6; i++) {
        if (i > 0) {
            json_out_raw(&w, ",");
        }
        json_out_str(&w, s->ipv6[i]);
    }
    json_out_raw(&w, "]},\"pd\":{\"state\":");
    json_out_str(&w, s->pd_state == DEVICE_API_PD_STATE_UNKNOWN ? NULL : pd_state_name(s->pd_state));
    json_out_raw(&w, ",\"prefix\":");
    str_or_null(&w, s->pd_prefix);
    json_out_raw(&w, "},\"routing\":");
    write_routing(&w, s);
    json_out_raw(&w, ",\"sniffer\":{\"active\":");
    json_out_bool(&w, s->sniffer_active);
    json_out_raw(&w, ",\"ended_by_fault\":");
    json_out_bool(&w, s->sniffer_ended_by_fault);
    json_out_raw(&w, ",\"channel\":");
    json_out_uint(&w, s->sniffer_channel);
    json_out_raw(&w, ",\"frames\":");
    json_out_uint(&w, s->sniffer_frames);
    json_out_raw(&w, ",\"drops\":");
    json_out_uint(&w, s->sniffer_drops);
    json_out_raw(&w, ",\"elapsed_s\":");
    json_out_uint(&w, s->sniffer_elapsed_s);
    json_out_raw(&w, ",\"duration_s\":");
    json_out_uint(&w, s->sniffer_duration_s);
    json_out_raw(&w, "},\"fault_reason\":");
    str_or_null(&w, s->fault_reason);
    json_out_raw(&w, ",\"storage_ok\":");
    json_out_bool(&w, s->storage_ok);
    json_out_raw(&w, ",\"trel\":");
    trel_json(&w, &s->trel);
    json_out_raw(&w, "}");
    return w.overflow ? -1 : (int)w.len;
}
