// The zigbee status: counters, state, client and the last radio configuration, kept under a spinlock by the zigbee
// and sender tasks; a snapshot for any task, completed with the mux's view of the channel.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "radio_mux.h"
#include "zigbee_priv.h"

#define EPOCH_VALID 1577836800 // 2020-01-01: an earlier clock was never set (as the mux's)

_Static_assert(ZIGBEE_CHANNEL_ID_MAX == RADIO_MUX_ID_MAX, "channel ids are the mux's");

portMUX_TYPE g_zigbee_status_lock = portMUX_INITIALIZER_UNLOCKED;
zigbee_status_t g_zigbee_status = {.state = ZIGBEE_STATE_IDLE};

// The channel in mode ziggurat, found once per boot: entering or leaving the mode reboots (reboot_on_change), so it
// does not change while the board runs. 0: not looked up yet; 1: none; 2: s_chan holds it (never written again).
static portMUX_TYPE s_chan_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_chan[ZIGBEE_CHANNEL_ID_MAX];
static atomic_int s_chan_state;

int64_t zigbee_now_epoch(void)
{
    const time_t t = time(NULL);

    return t >= EPOCH_VALID ? (int64_t)t : 0;
}

void zigbee_status_set_state(zigbee_state_t state, const char *reason)
{
    taskENTER_CRITICAL(&g_zigbee_status_lock);
    g_zigbee_status.state = state;
    strlcpy(g_zigbee_status.reason, reason ? reason : "", sizeof(g_zigbee_status.reason));
    taskEXIT_CRITICAL(&g_zigbee_status_lock);
}

void zigbee_status_note_channel(const char *id)
{
    taskENTER_CRITICAL(&s_chan_lock);
    if (atomic_load(&s_chan_state) != 2) {
        strlcpy(s_chan, id, sizeof(s_chan));
        atomic_store(&s_chan_state, 2); // after the copy: readers take s_chan only in state 2
    }
    taskEXIT_CRITICAL(&s_chan_lock);
}

// The mux's channels (about 4 KB, from PSRAM). NULL when out of memory or the mux has not started (no channels yet).
static radio_mux_snapshot_t *snapshot(void)
{
    radio_mux_snapshot_t *snap = heap_caps_malloc(sizeof(*snap), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (snap == NULL) {
        return NULL;
    }
    radio_mux_get_snapshot(snap);
    if (snap->n == 0) {
        free(snap);
        return NULL;
    }
    return snap;
}

const char *zigbee_channel(void)
{
    int st = atomic_load(&s_chan_state);

    if (st == 0) {
        radio_mux_snapshot_t *snap = snapshot();

        if (snap == NULL) {
            return NULL; // try again later
        }
        const char *id = NULL;

        for (size_t i = 0; i < snap->n && !id; i++) {
            if (snap->chans[i].stored_mode == RADIO_MUX_MODE_ZIGGURAT) {
                id = snap->chans[i].id;
            }
        }
        if (id) {
            zigbee_status_note_channel(id);
        } else {
            int unknown = 0;

            atomic_compare_exchange_strong(&s_chan_state, &unknown, 1);
        }
        free(snap);
        st = atomic_load(&s_chan_state);
    }
    return st == 2 ? s_chan : NULL;
}

static void firmware_text(const radio_mux_firmware_t *fw, char *out, size_t cap)
{
    out[0] = '\0';
    if (fw->source == RADIO_MUX_FW_SRC_NONE) {
        return;
    }
    snprintf(out, cap, "%s%s%s%s%s", radio_mux_fw_name(fw->rec.type), fw->rec.version[0] ? " " : "", fw->rec.version,
             fw->rec.chip[0] ? " " : "", fw->rec.chip);
}

void zigbee_get_status(zigbee_status_t *out)
{
    taskENTER_CRITICAL(&g_zigbee_status_lock);
    *out = g_zigbee_status;
    out->heap.current = g_zigbee_heap.current;
    out->heap.peak = g_zigbee_heap.peak;
    taskEXIT_CRITICAL(&g_zigbee_status_lock);

    const char *id = zigbee_channel();

    if (id == NULL) {
        out->channel[0] = '\0';
        return;
    }
    strlcpy(out->channel, id, sizeof(out->channel));
    radio_mux_snapshot_t *snap = snapshot();

    if (snap == NULL) {
        return;
    }
    for (size_t i = 0; i < snap->n; i++) {
        const radio_mux_chan_info_t *c = &snap->chans[i];

        if (strcmp(c->id, id) != 0) {
            continue;
        }
        firmware_text(&c->firmware, out->firmware, sizeof(out->firmware));
        out->counters.rx_dropped = c->counters.drops;
        // Never attached (or gone): the mux knows why (not present, its firmware does not fit, a fault).
        if ((out->state == ZIGBEE_STATE_IDLE || out->state == ZIGBEE_STATE_NO_RADIO) && c->reason[0]) {
            strlcpy(out->reason, c->reason, sizeof(out->reason));
        } else if (out->state == ZIGBEE_STATE_IDLE && !c->present) {
            strlcpy(out->reason, "radio not present", sizeof(out->reason));
        }
        break;
    }
    free(snap);
}
