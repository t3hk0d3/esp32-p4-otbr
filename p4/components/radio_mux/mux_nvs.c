// The channel registry in NVS: namespace radio_mux, an index key "chans" with the ids of every remembered channel,
// and one blob (version 1) per channel under radio_mux_nvs_key(id).
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "radio_mux_priv.h"

static const char *TAG = "radio_mux";

#define NVS_NAMESPACE "radio_mux"
#define NVS_INDEX "chans"
#define BLOB_VERSION 1

// Version 1 of a channel's NVS blob. Read back by the same firmware layout; another version is ignored (defaults).
typedef struct {
    uint8_t version;
    uint8_t kind;
    uint8_t mode;
    uint8_t flow;
    uint32_t baud;
    radio_mux_proxy_settings_t proxy;
    radio_mux_fw_record_t fw;
    uint8_t fw_source;
    int64_t fw_at;
    int64_t first_seen, last_seen;
    uint16_t vid, pid;
    uint8_t driver;
    char serial[RADIO_MUX_SERIAL_MAX];
    char label[RADIO_MUX_LABEL_MAX];
    char id[RADIO_MUX_ID_MAX];
} chan_blob_t;

static void blob_from_chan(const struct radio_mux_chan *ch, chan_blob_t *b)
{
    memset(b, 0, sizeof(*b));
    b->version = BLOB_VERSION;
    b->kind = (uint8_t)ch->kind;
    b->mode = (uint8_t)ch->mode;
    b->flow = ch->flow ? 1 : 0;
    b->baud = ch->baud;
    b->proxy = ch->proxy;
    b->fw = ch->fw.rec;
    b->fw_source = (uint8_t)ch->fw.source;
    b->fw_at = ch->fw.at;
    b->first_seen = ch->first_seen;
    b->last_seen = ch->last_seen;
    b->vid = ch->usb.vid;
    b->pid = ch->usb.pid;
    b->driver = (uint8_t)ch->usb.driver;
    memcpy(b->serial, ch->usb.serial, sizeof(b->serial));
    memcpy(b->label, ch->label, sizeof(b->label));
    memcpy(b->id, ch->id, sizeof(b->id));
}

// A blob that is malformed or out of range keeps the channel's defaults for that part.
static void chan_from_blob(struct radio_mux_chan *ch, const chan_blob_t *b)
{
    const radio_mux_mode_t mode = (radio_mux_mode_t)b->mode;

    if (mode == RADIO_MUX_MODE_OFF || mode == RADIO_MUX_MODE_OTBR || mode == RADIO_MUX_MODE_ZIGGURAT ||
            mode == RADIO_MUX_MODE_RCP_PROXY) {
        ch->mode = mode;
    }
    ch->baud = radio_mux_baud_ok(b->baud) ? b->baud : 0;
    ch->flow = b->flow != 0;
    if (radio_mux_proxy_ok(&b->proxy)) {
        ch->proxy = b->proxy;
    }
    if (b->fw_source == RADIO_MUX_FW_SRC_PROBE || b->fw_source == RADIO_MUX_FW_SRC_PASSIVE) {
        ch->fw.rec = b->fw;
        ch->fw.rec.version[sizeof(ch->fw.rec.version) - 1] = '\0';
        ch->fw.rec.chip[sizeof(ch->fw.rec.chip) - 1] = '\0';
        ch->fw.source = (radio_mux_fw_source_t)b->fw_source;
        ch->fw.at = b->fw_at;
    }
    ch->first_seen = b->first_seen;
    ch->last_seen = b->last_seen;
    ch->usb.vid = b->vid;
    ch->usb.pid = b->pid;
    ch->usb.driver = b->driver <= RADIO_MUX_DRV_CDC_ACM ? (radio_mux_driver_t)b->driver : RADIO_MUX_DRV_NONE;
    memcpy(ch->usb.serial, b->serial, sizeof(ch->usb.serial));
    ch->usb.serial[sizeof(ch->usb.serial) - 1] = '\0';
    memcpy(ch->label, b->label, sizeof(ch->label));
    ch->label[sizeof(ch->label) - 1] = '\0';
}

// The index: the ids of every remembered channel, RADIO_MUX_ID_MAX bytes each.
static esp_err_t store_index(nvs_handle_t nvs)
{
    char ids[RADIO_MUX_CHANNELS_MAX][RADIO_MUX_ID_MAX];
    size_t n = 0;

    memset(ids, 0, sizeof(ids));
    for (size_t i = 0; i < RADIO_MUX_CHANNELS_MAX; i++) {
        if (g_mux.chans[i].used && g_mux.chans[i].remembered) {
            memcpy(ids[n++], g_mux.chans[i].id, RADIO_MUX_ID_MAX);
        }
    }
    return n ? nvs_set_blob(nvs, NVS_INDEX, ids, n * RADIO_MUX_ID_MAX) : nvs_erase_key(nvs, NVS_INDEX);
}

// Owner. Writes the channel's blob, and the index when the channel is new to NVS.
esp_err_t mux_store_chan(struct radio_mux_chan *ch)
{
    nvs_handle_t nvs;
    char key[16];
    chan_blob_t b;

    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "nvs_open");
    radio_mux_nvs_key(ch->id, key);
    blob_from_chan(ch, &b);
    esp_err_t err = nvs_set_blob(nvs, key, &b, sizeof(b));

    if (err == ESP_OK && !ch->remembered) {
        ch->remembered = true;
        err = store_index(nvs);
        if (err != ESP_OK) {
            ch->remembered = false;
        }
    }
    err = err == ESP_OK ? nvs_commit(nvs) : err;
    nvs_close(nvs);
    ESP_RETURN_ON_ERROR(err, TAG, "storing %s", ch->id);
    return ESP_OK;
}

// Owner. Erases the channel's blob and drops it from the index.
esp_err_t mux_erase_chan(struct radio_mux_chan *ch)
{
    nvs_handle_t nvs;
    char key[16];

    if (!ch->remembered) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "nvs_open");
    radio_mux_nvs_key(ch->id, key);
    esp_err_t err = nvs_erase_key(nvs, key);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    ch->remembered = false;
    err = err == ESP_OK ? store_index(nvs) : err;
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK; // erasing an index that was already gone
    }
    err = err == ESP_OK ? nvs_commit(nvs) : err;
    nvs_close(nvs);
    ESP_RETURN_ON_ERROR(err, TAG, "erasing %s", ch->id);
    return ESP_OK;
}

void mux_chan_defaults(struct radio_mux_chan *ch, const char *id, radio_mux_kind_t kind)
{
    memset(ch, 0, sizeof(*ch));
    ch->used = true;
    snprintf(ch->id, sizeof(ch->id), "%s", id);
    ch->kind = kind;
    ch->port = kind == RADIO_MUX_KIND_C6 ? &g_mux.c6 : &g_mux.usb;
    ch->mode = kind == RADIO_MUX_KIND_C6 ? RADIO_MUX_MODE_OTBR : RADIO_MUX_MODE_OFF;
    radio_mux_proxy_defaults(&ch->proxy);
    ch->eff_mode = RADIO_MUX_MODE_UNAVAILABLE;
    ch->supported = true;
    snprintf(ch->reason, sizeof(ch->reason), "not detected");
}

void mux_load_chans(void)
{
    char ids[RADIO_MUX_CHANNELS_MAX][RADIO_MUX_ID_MAX];
    size_t len = sizeof(ids), slot = 1; // slot 0 is the C6
    nvs_handle_t nvs;
    chan_blob_t b;

    mux_chan_defaults(&g_mux.chans[0], RADIO_MUX_C6_ID, RADIO_MUX_KIND_C6);
    // Until the C6 transport exists (Task 6) the mux lists c6 without opening it; br_core runs its own SPI path.
    snprintf(g_mux.chans[0].reason, sizeof(g_mux.chans[0].reason), "C6 transport not in this build yet");
    snprintf(g_mux.chans[0].label, sizeof(g_mux.chans[0].label), "ESP32-C6 (on board)");
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);

    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) { // not found: nothing stored yet
            ESP_LOGE(TAG, "reading the channels failed (%s): defaults", esp_err_to_name(err));
        }
        return;
    }
    if (nvs_get_blob(nvs, NVS_INDEX, ids, &len) != ESP_OK) {
        len = 0;
    }
    for (size_t i = 0; i < len / RADIO_MUX_ID_MAX; i++) {
        char key[16];
        size_t blen = sizeof(b);

        ids[i][RADIO_MUX_ID_MAX - 1] = '\0';
        radio_mux_nvs_key(ids[i], key);
        if (nvs_get_blob(nvs, key, &b, &blen) != ESP_OK || blen != sizeof(b) || b.version != BLOB_VERSION ||
                strncmp(b.id, ids[i], RADIO_MUX_ID_MAX) != 0) {
            ESP_LOGW(TAG, "stored channel %s unreadable: skipped", ids[i]);
            continue;
        }
        const bool c6 = strcmp(ids[i], RADIO_MUX_C6_ID) == 0;
        struct radio_mux_chan *ch = c6 ? &g_mux.chans[0] : (slot < RADIO_MUX_CHANNELS_MAX ? &g_mux.chans[slot++] : NULL);

        if (ch == NULL) {
            break;
        }
        if (!c6) {
            mux_chan_defaults(ch, ids[i], RADIO_MUX_KIND_USB);
        }
        chan_from_blob(ch, &b);
        ch->remembered = true;
    }
    nvs_close(nvs);
}
