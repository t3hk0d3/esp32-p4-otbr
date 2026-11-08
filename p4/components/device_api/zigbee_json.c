// /device/zigbee body (spec docs/superpowers/specs/2026-11-06-zigbee-mode-design.md). Pure.
#include <stdio.h>

#include "device_api_priv.h"
#include "zigbee_json.h"

static const char *state_name(zigbee_state_t state)
{
    switch (state) {
    case ZIGBEE_STATE_IDLE:
        return "idle";
    case ZIGBEE_STATE_STARTING:
        return "starting";
    case ZIGBEE_STATE_RUNNING:
        return "running";
    case ZIGBEE_STATE_NO_RADIO:
        return "no_radio";
    case ZIGBEE_STATE_INCOMPATIBLE:
        return "incompatible";
    default:
        return "idle";
    }
}

static void key(json_out_t *w, const char *name)
{
    json_out_raw(w, ",\"");
    json_out_raw(w, name);
    json_out_raw(w, "\":");
}

static void counter(json_out_t *w, const char *name, uint32_t value)
{
    key(w, name);
    json_out_uint(w, value);
}

// Epoch seconds are written as unsigned: a negative value (never produced) would otherwise be a negative time.
static void epoch(json_out_t *w, int64_t value)
{
    json_out_u64(w, value > 0 ? (uint64_t)value : 0);
}

static void write_client(json_out_t *w, bool present, const char *address, const char *time_key, int64_t time)
{
    if (!present) {
        json_out_raw(w, "null");
        return;
    }
    json_out_raw(w, "{\"address\":");
    json_out_str(w, address);
    key(w, time_key);
    epoch(w, time);
    json_out_raw(w, "}");
}

static void write_radio(json_out_t *w, const zigbee_status_t *st)
{
    char hex[17];

    if (!st->has_radio_config) {
        json_out_raw(w, "null");
        return;
    }
    for (int i = 0; i < 8; i++) {
        snprintf(&hex[i * 2], 3, "%02x", st->radio.ext_addr[i]);
    }
    json_out_raw(w, "{\"channel\":");
    json_out_uint(w, st->radio.channel);
    key(w, "pan_id");
    json_out_uint(w, st->radio.pan_id);
    key(w, "short_address");
    json_out_uint(w, st->radio.short_addr);
    key(w, "extended_address");
    json_out_str(w, hex);
    key(w, "tx_power_dbm");
    json_out_int(w, st->radio.tx_power_dbm);
    json_out_raw(w, "}");
}

esp_err_t zigbee_json_status(const zigbee_status_t *st, char *buf, size_t cap, size_t *len)
{
    json_out_t w;

    json_out_init(&w, buf, cap);
    json_out_raw(&w, "{\"channel\":");
    json_out_str(&w, st->channel);
    key(&w, "firmware");
    json_out_str(&w, st->firmware);
    key(&w, "state");
    json_out_str(&w, state_name(st->state));
    key(&w, "reason");
    json_out_str(&w, st->reason);
    key(&w, "client");
    write_client(&w, st->has_client, st->client_address, "since", st->client_since);
    key(&w, "last_client");
    write_client(&w, st->has_last_client, st->last_client_address, "left", st->last_client_left);
    key(&w, "radio");
    write_radio(&w, st);
    json_out_raw(&w, ",\"counters\":{\"rx\":");
    json_out_uint(&w, st->counters.rx);
    counter(&w, "rx_dropped", st->counters.rx_dropped);
    counter(&w, "tx", st->counters.tx);
    counter(&w, "tx_no_ack", st->counters.tx_no_ack);
    counter(&w, "tx_cca", st->counters.tx_cca);
    counter(&w, "tx_failed", st->counters.tx_failed);
    counter(&w, "energy_scans", st->counters.energy_scans);
    counter(&w, "rcp_resets", st->counters.rcp_resets);
    counter(&w, "frames_in", st->counters.frames_in);
    counter(&w, "frames_out", st->counters.frames_out);
    json_out_raw(&w, "},\"heap\":{\"current\":");
    json_out_u64(&w, st->heap.current);
    key(&w, "peak");
    json_out_u64(&w, st->heap.peak);
    json_out_raw(&w, "}");
    key(&w, "started");
    if (st->stack_started) {
        epoch(&w, st->started);
    } else {
        json_out_raw(&w, "null");
    }
    json_out_raw(&w, "}");
    *len = w.len;
    return w.overflow ? ESP_ERR_INVALID_SIZE : ESP_OK;
}
