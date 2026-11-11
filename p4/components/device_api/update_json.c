#include "update_json.h"

#include "self_update_priv.h"

#define PAGE_URL_MAX 192

device_api_error_t device_api_update_decide(self_update_cmp_t cmp, bool rcp_differs, bool has_rcp, bool force,
                                            bool *install_app, bool *install_rcp)
{
    static const device_api_error_t ok = {0, NULL, NULL};
    static const device_api_error_t up_to_date = {409, "up_to_date", "the board runs the latest release"};

    *install_app = force || cmp == SELF_UPDATE_NEWER || cmp == SELF_UPDATE_UNKNOWN;
    *install_rcp = has_rcp && (force || rcp_differs);
    return *install_app || *install_rcp ? ok : up_to_date;
}

static const char *state_name(self_update_state_t state)
{
    switch (state) {
    case SELF_UPDATE_CHECKING:
        return "checking";
    case SELF_UPDATE_DONE:
        return "done";
    case SELF_UPDATE_FAILED:
        return "failed";
    default:
        return "never";
    }
}

static const char *cmp_name(self_update_cmp_t cmp)
{
    switch (cmp) {
    case SELF_UPDATE_NEWER:
        return "newer";
    case SELF_UPDATE_SAME:
        return "same";
    case SELF_UPDATE_OLDER:
        return "older";
    default:
        return "unknown";
    }
}

// "" -> NULL, written as null.
static const char *or_null(const char *text)
{
    return text && text[0] ? text : NULL;
}

int device_api_update_json(char *buf, size_t cap, const self_update_snapshot_t *snap, const char *base,
                           const char *running_app, bool rcp_known, const char *rcp_installed,
                           const char *board_id, const char *chip_id)
{
    const self_update_release_t *rel = &snap->release;
    const char *running_rcp = rcp_known ? or_null(rcp_installed) : NULL;
    const self_update_cmp_t cmp = snap->has_release ? self_update_compare(rel->app, running_app) : SELF_UPDATE_UNKNOWN;
    const bool rcp_differs = snap->has_release && self_update_rcp_differs(rel->rcp, rcp_installed, rcp_known);
    char page_url[PAGE_URL_MAX];
    char bundle[DEVICE_API_BUNDLE_NAME_MAX];
    json_out_t w;

    if (snap->has_release && self_update_release_page_url(base, rel->tag, page_url, sizeof(page_url)) != ESP_OK) {
        return -1;
    }
    if (self_update_bundle_name(board_id, chip_id, bundle, sizeof(bundle)) != ESP_OK) {
        return -1;
    }
    json_out_init(&w, buf, cap);
    json_out_raw(&w, "{\"state\":");
    json_out_str(&w, state_name(snap->state));
    json_out_raw(&w, ",\"checked_age_s\":");
    if (snap->has_age) {
        json_out_uint(&w, snap->age_s);
    } else {
        json_out_raw(&w, "null");
    }
    json_out_raw(&w, ",\"error\":");
    json_out_str(&w, or_null(snap->error));
    json_out_raw(&w, ",\"release\":");
    if (snap->has_release) {
        json_out_raw(&w, "{\"tag\":");
        json_out_str(&w, or_null(rel->tag));
        json_out_raw(&w, ",\"app\":");
        json_out_str(&w, rel->app);
        json_out_raw(&w, ",\"rcp\":");
        json_out_str(&w, or_null(rel->rcp));
        json_out_raw(&w, ",\"url\":");
        json_out_str(&w, page_url);
        json_out_raw(&w, "}");
    } else {
        json_out_raw(&w, "null");
    }
    json_out_raw(&w, ",\"repo_url\":");
    json_out_str(&w, SELF_UPDATE_REPO_URL);
    json_out_raw(&w, ",\"board_id\":");
    json_out_str(&w, board_id);
    json_out_raw(&w, ",\"chip_id\":");
    json_out_str(&w, chip_id);
    json_out_raw(&w, ",\"bundle\":");
    json_out_str(&w, bundle);
    json_out_raw(&w, ",\"running\":{\"app\":");
    json_out_str(&w, running_app);
    json_out_raw(&w, ",\"rcp\":");
    json_out_str(&w, running_rcp);
    json_out_raw(&w, "},\"app\":");
    json_out_str(&w, snap->has_release ? cmp_name(cmp) : NULL);
    json_out_raw(&w, ",\"rcp_differs\":");
    json_out_bool(&w, rcp_differs);
    json_out_raw(&w, ",\"available\":");
    json_out_bool(&w, snap->has_release && (cmp == SELF_UPDATE_NEWER || cmp == SELF_UPDATE_UNKNOWN || rcp_differs));
    json_out_raw(&w, "}");
    return w.overflow ? -1 : (int)w.len;
}

int device_api_update_reply_json(char *buf, size_t cap, bool app_installed, const char *rcp_result, const char *detail,
                                 bool rebooting)
{
    json_out_t w;

    json_out_init(&w, buf, cap);
    json_out_raw(&w, "{\"status\":\"ok\",\"app\":");
    json_out_str(&w, app_installed ? "installed" : "unchanged");
    json_out_raw(&w, ",\"rcp\":");
    json_out_str(&w, rcp_result);
    json_out_raw(&w, ",\"detail\":");
    json_out_str(&w, detail ? detail : "");
    json_out_raw(&w, ",\"rebooting\":");
    json_out_bool(&w, rebooting);
    json_out_raw(&w, "}");
    return w.overflow ? -1 : (int)w.len;
}
