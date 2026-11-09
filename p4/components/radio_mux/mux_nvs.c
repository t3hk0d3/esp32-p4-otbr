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
// A change of the layout above (or of a type in it) is a new blob version: the old blobs would be read wrongly.
_Static_assert(sizeof(chan_blob_t) == 336, "chan_blob_t changed: bump BLOB_VERSION (and read the old one) first");

#define C6_LABEL "ESP32-C6 (built-in)" // without a built-in RCP described (radio_mux_set_builtin)

// The built-in channel's label: its descriptor's.
static const char *builtin_label(void)
{
    return g_mux.builtin && g_mux.builtin->label ? g_mux.builtin->label : C6_LABEL;
}

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
    snprintf(b->id, sizeof(b->id), "%s", radio_mux_nvs_id(ch->id)); // the built-in: its stored id, as older builds read
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
    if (radio_mux_proxy_check(&b->proxy) == RADIO_MUX_PROXY_CHECK_OK) {
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
            snprintf(ids[n++], RADIO_MUX_ID_MAX, "%s", radio_mux_nvs_id(g_mux.chans[i].id));
        }
    }
    return n ? nvs_set_blob(nvs, NVS_INDEX, ids, n * RADIO_MUX_ID_MAX) : nvs_erase_key(nvs, NVS_INDEX);
}

// Writes the channel's blob into the open handle; *index set when the channel is new to NVS (marked remembered).
static esp_err_t set_chan(nvs_handle_t nvs, struct radio_mux_chan *ch, bool *index)
{
    char key[16];
    chan_blob_t b;

    radio_mux_nvs_key(ch->id, key);
    blob_from_chan(ch, &b);
    const esp_err_t err = nvs_set_blob(nvs, key, &b, sizeof(b));

    if (err == ESP_OK && !ch->remembered) {
        ch->remembered = true;
        *index = true;
    }
    return err;
}

// Owner. Writes the blobs of a and b (b may be NULL or a), and the index when one of them is new to NVS, in one NVS
// handle with one commit. A failure leaves the remembered flags as they were.
esp_err_t mux_store_chans(struct radio_mux_chan *a, struct radio_mux_chan *b)
{
    nvs_handle_t nvs;
    const bool a_known = a->remembered, b_known = b ? b->remembered : true;
    bool index = false;

    if (b == a) {
        b = NULL;
    }
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "nvs_open");
    esp_err_t err = set_chan(nvs, a, &index);

    if (err == ESP_OK && b) {
        err = set_chan(nvs, b, &index);
    }
    if (err == ESP_OK && index) {
        err = store_index(nvs);
    }
    err = err == ESP_OK ? nvs_commit(nvs) : err;
    nvs_close(nvs);
    if (err != ESP_OK) {
        a->remembered = a_known;
        if (b) {
            b->remembered = b_known;
        }
    }
    ESP_RETURN_ON_ERROR(err, TAG, "storing %s%s%s", a->id, b ? " and " : "", b ? b->id : "");
    return ESP_OK;
}

// Owner. Writes the channel's blob, and the index when the channel is new to NVS.
esp_err_t mux_store_chan(struct radio_mux_chan *ch)
{
    return mux_store_chans(ch, NULL);
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
    ch->port = kind == RADIO_MUX_KIND_BUILTIN ? &g_mux.c6 : &g_mux.usb;
    ch->mode = kind == RADIO_MUX_KIND_BUILTIN ? RADIO_MUX_MODE_OTBR : RADIO_MUX_MODE_OFF;
    radio_mux_proxy_defaults(&ch->proxy);
    ch->eff_mode = RADIO_MUX_MODE_UNAVAILABLE;
    ch->supported = true;
    snprintf(ch->reason, sizeof(ch->reason), "not detected");
}

// At load: a singleton (otbr, ziggurat) stored on more than one channel (a move's stores cut apart) keeps the first in
// slot order (the C6 is slot 0); the others are stored off.
static void dedupe_singletons(void)
{
    radio_mux_mode_t modes[RADIO_MUX_CHANNELS_MAX];
    bool changed[RADIO_MUX_CHANNELS_MAX];

    for (size_t i = 0; i < RADIO_MUX_CHANNELS_MAX; i++) {
        modes[i] = g_mux.chans[i].used ? g_mux.chans[i].mode : RADIO_MUX_MODE_OFF;
    }
    if (radio_mux_singleton_dedupe(modes, changed, RADIO_MUX_CHANNELS_MAX) == 0) {
        return;
    }
    for (size_t i = 0; i < RADIO_MUX_CHANNELS_MAX; i++) {
        struct radio_mux_chan *ch = &g_mux.chans[i];

        if (changed[i]) {
            ch->mode = RADIO_MUX_MODE_OFF;
            ESP_LOGW(TAG, "%s: stored in a mode another channel has (an interrupted move): stored off", ch->id);
            if (mux_store_chan(ch) != ESP_OK) {
                ESP_LOGW(TAG, "%s: storing off failed: off until the next boot's repair", ch->id);
            }
        }
    }
}

void mux_load_chans(void)
{
    char ids[RADIO_MUX_CHANNELS_MAX][RADIO_MUX_ID_MAX];
    size_t len = sizeof(ids), slot = 1; // slot 0 is the C6
    nvs_handle_t nvs;
    chan_blob_t b;

    mux_chan_defaults(&g_mux.chans[0], RADIO_MUX_BUILTIN_ID, RADIO_MUX_KIND_BUILTIN); // listed by the owner task (c6_init)
    snprintf(g_mux.chans[0].label, sizeof(g_mux.chans[0].label), "%s", builtin_label());
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
        const bool c6 = radio_mux_is_builtin_id(ids[i]); // stored as RADIO_MUX_BUILTIN_NVS_ID
        struct radio_mux_chan *ch = c6 ? &g_mux.chans[0] : (slot < RADIO_MUX_CHANNELS_MAX ? &g_mux.chans[slot++] : NULL);

        if (ch == NULL) {
            break;
        }
        if (!c6) {
            mux_chan_defaults(ch, ids[i], RADIO_MUX_KIND_USB);
        }
        chan_from_blob(ch, &b);
        if (c6) { // the built-in's label is the firmware's: one stored by an older build never replaces it
            snprintf(ch->label, sizeof(ch->label), "%s", builtin_label());
        }
        ch->remembered = true;
    }
    nvs_close(nvs);
    dedupe_singletons();
}
