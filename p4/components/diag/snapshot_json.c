#include "snapshot_json.h"

#include "device_api_priv.h"

static const char *const state_names[] = {"running", "ready", "blocked", "suspended", "deleted"};
static const char *const window_keys[DIAG_WINDOW_COUNT] = {"1s", "10s", "60s"};

// One decimal, rounded: the writer has no floats.
static void put_pct(json_out_t *w, float pct)
{
    const uint32_t tenths = (uint32_t)(pct * 10.0f + 0.5f);

    json_out_uint(w, tenths / 10);
    json_out_raw(w, ".");
    json_out_uint(w, tenths % 10);
}

static void put_pct_object(json_out_t *w, const float pct[DIAG_WINDOW_COUNT])
{
    json_out_raw(w, "{");
    for (int i = 0; i < DIAG_WINDOW_COUNT; i++) {
        json_out_raw(w, i ? ",\"" : "\"");
        json_out_raw(w, window_keys[i]);
        json_out_raw(w, "\":");
        put_pct(w, pct[i]);
    }
    json_out_raw(w, "}");
}

static void put_uint_field(json_out_t *w, const char *key, uint32_t value, bool first)
{
    json_out_raw(w, first ? "\"" : ",\"");
    json_out_raw(w, key);
    json_out_raw(w, "\":");
    json_out_uint(w, value);
}

static void put_heap(json_out_t *w, const char *key, const diag_heap_info_t *h)
{
    json_out_raw(w, "\"");
    json_out_raw(w, key);
    json_out_raw(w, "\":{");
    put_uint_field(w, "free", h->free_bytes, true);
    put_uint_field(w, "min_free", h->min_free_bytes, false);
    put_uint_field(w, "largest_free_block", h->largest_free_block, false);
    json_out_raw(w, "}");
}

static void put_lock_totals(json_out_t *w, const diag_lock_totals_t *t)
{
    put_uint_field(w, "acquisitions", t->acquisitions, true);
    put_uint_field(w, "max_hold_us", t->max_hold_us, false);
    put_uint_field(w, "max_wait_us", t->max_wait_us, false);
    json_out_raw(w, ",\"longest_holder\":");
    json_out_str(w, t->hold_task);
}

int diag_snapshot_render(char *buf, size_t cap, const diag_snapshot_input_t *in)
{
    json_out_t w;
    const diag_view_t *view = in->view;

    json_out_init(&w, buf, cap);
    json_out_raw(&w, "{");
    put_uint_field(&w, "uptime_s", in->uptime_s, true);
    json_out_raw(&w, ",\"reset_reason\":");
    json_out_str(&w, in->reset_reason);

    json_out_raw(&w, ",\"cpu\":[");
    for (int c = 0; c < DIAG_CORES; c++) {
        json_out_raw(&w, c ? ",{" : "{");
        put_uint_field(&w, "core", (uint32_t)c, true);
        json_out_raw(&w, ",\"load_pct\":");
        put_pct_object(&w, view->load_pct[c]);
        json_out_raw(&w, "}");
    }

    json_out_raw(&w, "],\"tasks\":[");
    for (size_t i = 0; i < view->task_count; i++) {
        const diag_task_view_t *t = &view->tasks[i];

        json_out_raw(&w, i ? ",{\"name\":" : "{\"name\":");
        json_out_str(&w, t->task.name);
        json_out_raw(&w, ",\"core\":");
        if (t->task.core == DIAG_CORE_ANY) {
            json_out_raw(&w, "\"any\"");
        } else {
            json_out_int(&w, t->task.core);
        }
        put_uint_field(&w, "priority", t->task.prio, false);
        json_out_raw(&w, ",\"state\":");
        json_out_str(&w, t->task.state < sizeof(state_names) / sizeof(state_names[0]) ? state_names[t->task.state] : "unknown");
        json_out_raw(&w, ",\"cpu_pct\":");
        put_pct_object(&w, t->cpu_pct);
        put_uint_field(&w, "stack_free_min_bytes", t->task.stack_hwm, false);
        json_out_raw(&w, "}");
    }
    json_out_raw(&w, "]");
    put_uint_field(&w, "tasks_untracked", view->untracked, false);

    json_out_raw(&w, ",\"memory\":{");
    put_heap(&w, "internal", &in->internal);
    json_out_raw(&w, ",");
    put_heap(&w, "psram", &in->psram);

    json_out_raw(&w, "},\"ot_lock\":{");
    put_lock_totals(&w, &in->lock->since_boot);
    json_out_raw(&w, ",\"last_60s\":{");
    put_lock_totals(&w, &in->lock->last_60s);
    json_out_raw(&w, "}}}");
    return w.overflow ? -1 : (int)w.len;
}
