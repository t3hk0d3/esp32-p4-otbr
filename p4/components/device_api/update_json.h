#pragma once

// GET/POST /device/update bodies and the install decision (update_json.c). Pure: host-tested in test_host/. Apart from
// device_api_priv.h so that the components that include that header by path do not need self_update.

#include <stdbool.h>
#include <stddef.h>
#include "device_api_priv.h"
#include "self_update.h"

/** Room for a bundle file name, "<board_id>_<chip_id>.ota". */
#define DEVICE_API_BUNDLE_NAME_MAX 96

#ifdef __cplusplus
extern "C" {
#endif

/**
 * What POST /device/update installs. `cmp` orders the release's app against the running one, `rcp_differs` is
 * self_update_rcp_differs of the two, `has_rcp` says whether the release names a radio firmware at all.
 * The app is installed when it is newer or unknown, or with `force`; the radio firmware when the release has one and
 * it differs, or with `force`. http_status 0: go on (*install_app / *install_rcp say what); 409 "up_to_date" when
 * neither is to be installed (both outputs false then).
 */
device_api_error_t device_api_update_decide(self_update_cmp_t cmp, bool rcp_differs, bool has_rcp, bool force,
                                            bool *install_app, bool *install_rcp);

/**
 * The GET /device/update body (the endpoint contract) from the cached check `snap`. `base` is the releases base (for
 * the release page link), `running_app` the running P4 version, `rcp_known` / `rcp_installed` the radio firmware
 * installed in the C6 ("" or unknown: null). `board_id` and `chip_id` name the firmware target of this build; the body
 * carries them and the bundle file name they give. Returns the length, or -1 when it does not fit `cap` or the ids give
 * no bundle name.
 */
int device_api_update_json(char *buf, size_t cap, const self_update_snapshot_t *snap, const char *base,
                           const char *running_app, bool rcp_known, const char *rcp_installed,
                           const char *board_id, const char *chip_id);

/**
 * The 200 body of POST /device/update: `{"status":"ok","app":"installed"|"unchanged","rcp":<rcp_result>,
 * "detail":<detail>,"rebooting":<bool>}`. `rcp_result` is "installed", "unchanged" or "failed". Returns the length,
 * or -1 when it does not fit `cap`.
 */
int device_api_update_reply_json(char *buf, size_t cap, bool app_installed, const char *rcp_result, const char *detail,
                                 bool rebooting);

#ifdef __cplusplus
}
#endif
