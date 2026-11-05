# Self-update from GitHub releases: implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
> After approval this file goes onto a new branch `feat/self-update` cut from `master`.

## Context

The board can already install a bundle from a URL (`POST /device/ota/url`) and push radio firmware from a URL
(`POST /device/radio/firmware?from_url=`). Neither can reach GitHub, for two reasons:
- `device_api_check_url` rejects `https://`;
- the certificate bundle is enabled in sdkconfig but not linked, because nothing calls `esp_crt_bundle_attach`.

There is a third blocker: GitHub answers a release download with a redirect to `release-assets.githubusercontent.com`
whose `Location` header is about 915 bytes. The client's default buffer is 512 bytes and
`CONFIG_ESP_HTTP_CLIENT_STRICT_HEADER_BUFFER=y` is set, so the request fails.

The running app version is a build stamp (`20261103T185311Z-e41c14c`), not a tag. Every release already publishes
`versions.txt` with the same stamps.

**Goal:** the Firmware page shows whether a newer release exists on GitHub and installs it with one click: the P4 bundle,
then the matching C6 radio firmware when it differs, then a reboot. `/device/ota/url` and `from_url` accept `https://`.

**Architecture:**
- A new component `self_update` owns everything GitHub-specific:
  - the releases base URL and the asset URLs;
  - parsing `versions.txt` and comparing versions (pure, host-tested);
  - the blocking fetch of `versions.txt` and the cached result of the last check;
  - the shared HTTPS client config: certificate bundle, a 2 KB header buffer.
- `device_api` adds two endpoints (`GET /device/update[?check=true]`, `POST /device/update`) and runs both the check and the install on worker tasks, under its existing
  long-operation slot. The install reuses the OTA path and the radio push path that exist today.
- The Firmware page gains an Updates card.

**Tech stack:** ESP-IDF v6.1 (`esp_http_client`, `esp_crt_bundle`, mbedTLS), Unity host tests, node page tests,
pytest-embedded target tests.

**Mockups:** `docs/superpowers/specs/2026-11-05-self-update-mockup.html` (the Updates card in its eight states, with
the texts) and `docs/superpowers/specs/2026-11-05-firmware-page-mockup.html` (the whole page). Task 6 builds them.

**Spec:** none. This plan carries the design (section "Design decisions"); the investigation was done in the session
on 2026-11-05.

**Styleguide:** `STYLEGUIDE.md`:
- astyle and ruff via `make lint`;
- `snake_case`, with the new prefix `self_update_` (Task 1 adds it to the styleguide's prefix list);
- `esp_err_t` plus `ESP_RETURN_ON_ERROR`;
- one `TAG` per file;
- host tests in `<component>/test_host/main/`, named `"<scenario>: <effect>"` and tagged `[self_update]` or
  `[device_api]`;
- target tests in `p4/pytest/test_self_update.py`;
- test-only code under `#ifdef OTBR_TEST_BUILD`;
- a new routine task gets a `Makefile` target;
- changes to the vendored `esp_ot_br_server` frontend are listed in `p4/components/PATCHES.md`;
- `firmware.js` stays ES5.

## Design decisions

1. **The source of truth is `versions.txt`, not the GitHub API.**
   - It is 121 bytes, served from `<releases>/latest/download/versions.txt`, with no JSON to parse and no API rate
     limit.
   - CI adds a `release:` line to it (Task 2). The board then downloads every asset by that tag, from
     `<releases>/download/<tag>/<file>`. That way the bundle and the RCP come from the same release as the
     `versions.txt` that was read, even if a new release appears in between.
   - A `versions.txt` without a `release:` line (v0.3.0 and older) falls back to the `latest/download/` URLs.
2. **Ordering by stamp.** The first 16 characters of a version (`YYYYMMDDTHHMMSSZ`) order versions as strings:
   - release later than running: `newer`;
   - earlier: `older` (a dev build newer than the release);
   - equal stamp and equal 7-hex hash: `same`; `-dirty` is ignored;
   - anything else, or a malformed version: `unknown`.
3. **RCP by equality.** The RCP is installed when the release's `c6_rcp` differs from `rcp.installed`, when
   `rcp.installed` is unknown or empty, or when the install is forced. Every release rebuilds the RCP with a new stamp,
   so an update usually pushes the RCP too, with a few seconds of Thread outage. This is accepted.
4. **Install order: bundle, then RCP, then one reboot.**
   - The bundle is the large download and the likeliest to fail. A failure there changes nothing.
   - An RCP push failure after a committed bundle still reboots into the new P4, and the reply says `rcp: "failed"`.
     The next update then sees `same` for the app and pushes only the RCP.
   - Without a P4 update (RCP only), there is no reboot and the slot is released.
5. **Nothing happens in the background.** There is no periodic check and no automatic install, because an update
   restarts a border router. The page starts a check when it opens and the last check is older than 1 h or never ran.
6. **One HTTP client at a time.** The socket budget in `sdkconfig.defaults` has room for one HTTP client. The check
   takes a new long-operation kind `DEVICE_API_OP_UPDATE_CHECK`, which excludes OTA, the radio push and ROM recovery
   (and the other way round). Both the check and the install refuse with 409 `busy` while another one runs.
7. **Certificate bundle:** `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN=y`. It contains the roots that `github.com`
   chains to (Sectigo, USERTrust) and the root of the asset host (ISRG). No pinning. Expiry dates stay unchecked
   (`MBEDTLS_HAVE_TIME_DATE` is off; there is no SNTP).
8. **TLS memory:** sdkconfig defaults are kept at first. The hardware pass records the minimum free internal heap
   during a download. If it drops below 32 KB, set `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` and re-run that pass item.
9. **The repository is fixed at build time.** It is a constant in `self_update.h`:
   `SELF_UPDATE_REPO_URL "https://github.com/t3hk0d3/esp32-p4-otbr"`, and the releases base is that plus
   `/releases`. Test builds can override the releases base (not the repo link)
   with the console command `update_source <http base>`, so the target test can use a local HTTP server.
10. **Bundles are not signed.** Trust rests on TLS to GitHub. Signed bundles are out of scope; `docs/updates.md`
    says so.

## Global constraints

- HTTP client config for every URL fetch (OTA URL, radio `from_url`, `versions.txt`):
  - `crt_bundle_attach = esp_crt_bundle_attach`;
  - `buffer_size = 2048`, `buffer_size_tx = 1024`;
  - the existing timeouts: `OTA_URL_TIMEOUT_MS` 30000, `HTTP_CLIENT_TIMEOUT_MS`;
  - `versions.txt` uses `SELF_UPDATE_FETCH_TIMEOUT_MS` = 15000.
- Accepted URL schemes: `http://` and `https://`. The error code `https_not_supported` is removed.
- `versions.txt`:
  - at most `SELF_UPDATE_VERSIONS_MAX` = 1024 bytes;
  - keys `release`, `p4 app`, `c6_rcp`, each followed by `:` and spaces; other lines are ignored;
  - the first occurrence of a key wins; `\r\n` is accepted.
- Tag: 1 to 32 characters from `[A-Za-z0-9._-]`; anything else rejects the whole file.
- Version values: at most 31 characters.
- Asset file names: `versions.txt`, `otbr.bundle`, `c6_rcp.bin`.
- Page auto-check: when `state` is `never`, or `checked_age_s` > 3600.
- Release page link:
  - with a tag: `<releases>/tag/<tag>`;
  - without one: `<releases>/latest`.
- No flashing inside tasks; one batched hardware pass after review and fixes.
- Commits end with only `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Contract: endpoints

`GET /device/update[?check=true]`: 200 with the cached state below. It never waits for the network.
- Without `check`, or with `check=false`: just the cached state.
- `check=true`: a check starts on a worker and the reply shows `state: "checking"`. When a check is already running,
  the request joins it (same reply, no second check). The page then polls the plain `GET` until `state` is no longer
  `checking`.
- `check=true` while OTA, a radio push or ROM recovery runs: 409 `busy`, and no check starts.
- Any other `check` value: 400 `invalid_request`.

```
{
  "state": "never" | "checking" | "done" | "failed",
  "checked_age_s": <uint> | null,        // since the last finished check (done or failed)
  "error": <string> | null,              // detail of the last failed check, null after a successful one
  "release": null | {                    // the last successful check; kept when a later check fails
    "tag": <string> | null, "app": <string>, "rcp": <string> | null, "url": <release page link>
  },
  "repo_url": <string>,                  // SELF_UPDATE_REPO_URL, for the card's source link
  "running": {"app": <string>, "rcp": <string> | null},
  "app": "newer" | "same" | "older" | "unknown" | null,   // null when release is null
  "rcp_differs": <bool>,
  "available": <bool>                    // app is "newer" or "unknown", or rcp_differs
}
```


`POST /device/update[?force=1]`: the body is empty. The request is held until the install finishes, as `/device/ota/url`
does today.
- It first runs a fresh check of its own; the cached result is never used to install.
- 200 `{"status":"ok","app":"installed"|"unchanged","rcp":"installed"|"unchanged"|"failed","detail":<text>,"rebooting":<bool>}`.
- 409 `up_to_date`: nothing to install and no `force`.
- 409 `busy`: another long operation or check is running.
- 502 `check_failed`: `versions.txt` could not be fetched or parsed; nothing was changed.
- The OTA errors as today (`invalid_bundle`, `transfer_failed`), when the bundle step fails; nothing was changed.
- The push errors as today (`device_api_map_push_error`), when only the RCP was to be installed and the push fails.

## Review focus

1. **A hostile or broken `versions.txt`:**
   - a tag `../x` or one with `?`;
   - a 5 KB file, an HTML error page, a missing `p4 app:` line;
   - no URL may ever be built from a bad tag, and the check fails with a clear `error`.

   Owner: Task 3 (parse), Task 4 (check state).
2. **A redirect larger than the buffer, and HTTPS failures.**
   - A `Location` header longer than 2048 bytes, or a TLS handshake failure, gives `check_failed` or
     `transfer_failed`, never a hang or a crash.
   - The URL in the error detail is the one asked for, not the signed redirect.

   Owner: Task 1 (config), hardware pass.
3. **Committed bundle, failed RCP.** The board still reboots into the new P4, the reply says `rcp: "failed"`, and the
   next `GET /device/update` (after the reboot) reports `app: "same"` and `rcp_differs: true`. Owner: Task 5, hardware
   pass.
4. **Overlapping requests:**
   - an install during a check;
   - a check during an install;
   - an upload to `/device/ota` during a check;
   - two installs at once.

   Each gets 409, and the check cache is never half written. Owner: Tasks 4 and 5.
5. **A dev build newer than the release.** The page says the running build is newer and offers "Install <tag> anyway"
   (`force=1`), and never offers a plain install. Owner: Task 6.

## BDD skeleton

```
self_update core (self_update_core.c, host)
    self_update_parse_versions(text, len, out)
        v0.4.0-style file ("release: v0.4.0", "p4 app:      2026…-abc1234", "c6_rcp:  2026…-abc1234", ESP-IDF line)
            - ESP_OK; tag "v0.4.0", app and rcp set; the unknown lines are ignored
        File without a release line (v0.3.0)
            - ESP_OK; tag ""
        CRLF line ends, trailing spaces
            - values without \r or spaces
        No "p4 app:" line, or an empty value
            - ESP_ERR_NOT_FOUND
        Value longer than 31 characters
            - ESP_ERR_INVALID_SIZE
        Tag "../x", "v1?a", a 33-character tag
            - ESP_ERR_INVALID_ARG; out left untouched
        len > 1024, or a NUL inside the text
            - ESP_ERR_INVALID_SIZE / ESP_ERR_INVALID_ARG
        Key repeated
            - the first value is kept
    self_update_compare(release_app, running_app)
        Release stamp later
            - SELF_UPDATE_NEWER
        Release stamp earlier
            - SELF_UPDATE_OLDER
        Same stamp and hash; running has "-dirty"
            - SELF_UPDATE_SAME
        Same stamp, different hash
            - SELF_UPDATE_UNKNOWN
        Either one malformed ("abc", "", no T/Z)
            - SELF_UPDATE_UNKNOWN
    self_update_rcp_differs(release_rcp, running_rcp, running_known)
        Release rcp empty
            - false
        Equal strings
            - false
        Different, running unknown, or running ""
            - true
    self_update_asset_url(base, tag, file, out, cap)
        Tag "v0.4.0"
            - "<base>/download/v0.4.0/otbr.bundle"
        Empty tag
            - "<base>/latest/download/otbr.bundle"
        Result does not fit cap
            - ESP_ERR_INVALID_SIZE, out is ""
    self_update_release_page_url(base, tag, out, cap)
        Tag set / empty
            - "<base>/tag/<tag>" / "<base>/latest"

self_update check state (self_update.c, host, fake fetch ops)
    self_update_check_begin()
        Idle (never, done or failed)
            - true; state checking
        Already checking
            - false; state unchanged
    self_update_check_run()
        Fetch returns a valid file
            - state done, release stored, error cleared, checked_age_s 0
        Fetch fails (transport detail "connection refused")
            - state failed, error "connection refused"; the previous release is kept
        Fetch succeeds but parse fails
            - state failed, error names the parse failure; the previous release is kept
        Fetch asks for the versions.txt URL of the latest release
            - the URL is "<base>/latest/download/versions.txt"
    self_update_get(snapshot)
        Never checked
            - state never, checked_age_s absent (has_age false), no release
        30 s after a finished check
            - checked_age_s 30

device_api limits (limits.c, host)
    device_api_check_url(url)
        "https://example.org/x"
            - acceptable
        "http://example.org/x"
            - acceptable
        "", "ftp://x", "https://"
            - 400 invalid_request

device_api update decision (update_json.c, host)
    device_api_update_decide(cmp, rcp_differs, has_rcp, force, &install_app, &install_rcp)
        NEWER, rcp differs
            - app and rcp; no error
        SAME, rcp differs
            - rcp only
        UNKNOWN
            - app
        SAME or OLDER, rcp equal, no force
            - 409 up_to_date
        OLDER, force
            - app; rcp when has_rcp
        Release without rcp, rcp "differs"
            - rcp not installed
    device_api_update_json(buf, cap, snapshot, running_app, rcp_known, rcp_installed)
        Never checked
            - state "never", release null, app null, available false
            - repo_url "https://github.com/t3hk0d3/esp32-p4-otbr"
        Done, newer release with a tag
            - release.url "<base>/tag/<tag>", app "newer", available true
        Failed after an earlier success
            - state "failed", error set, release still the earlier one
        Release without a tag
            - release.tag null, release.url "<base>/latest"
        rcp unknown on the board
            - running.rcp null, rcp_differs true
        Buffer too small
            - -1
    device_api_update_reply_json(buf, cap, app_installed, rcp_result, detail, rebooting)
        App installed, rcp failed
            - {"status":"ok","app":"installed","rcp":"failed",…,"rebooting":true}

device_api long operations (long_op.c, host)
    device_api_op_try_begin(DEVICE_API_OP_UPDATE_CHECK)
        Nothing running
            - true
        OTA, radio push or ROM recovery running
            - false
        Check running, then OTA, push or recover asked
            - false for each
        Check running, then a second check
            - false (the handler joins instead)
        Pcap running
            - true
    device_api_op_long_running()
        Only a check running
            - false (a check never defers a reboot)

device_api endpoints (device_api.c, target: p4/pytest/test_self_update.py, local HTTP server via update_source)
    GET /device/update
        Fresh boot
            - 200, state "never"; no request reaches the local server
    GET /device/update?check=true
        Server serves versions.txt
            - 200, state "checking"; within 10 s a plain GET shows state done, release as served
        Second check=true while the first runs
            - 200, state "checking"; the local server sees one versions.txt request
        Server down
            - a plain GET shows state failed, error set
        During an install
            - 409 busy
        check=yes
            - 400 invalid_request
    POST /device/update
        Release newer, RCP equal to the installed one
            - 200 app installed, rcp unchanged, rebooting; the board comes back on the new stamp
        Release same, RCP differs
            - 200 app unchanged, rcp installed, rebooting false; rcp.installed is the served stamp
        Nothing to do
            - 409 up_to_date
        Nothing to do, force=1
            - 200 app installed
        Bundle URL answers 404
            - 502 transfer_failed; slot and boot partition unchanged
        During a running check
            - 409 busy

firmware page (firmware.js, node: tools/tests/test_firmware_page.py; texts as in the mockups)
    FirmwareLogic.checkUpdateUrl(url)
        https URL
            - null (accepted)
    FirmwareLogic.updateNeedsCheck(u)
        state never; checked_age_s 3601
            - true
        checked_age_s 3600; state checking
            - false
    FirmwareLogic.updateChip(u)
        checking
            - {text "Checking", cls ""}
        failed
            - {text "Check failed", cls "chip-bad"}
        app newer or unknown, tag v0.4.0
            - {text "v0.4.0 available", cls "chip-warn"}; tag null: "Update available"
        app same, rcp differs
            - {text "Radio firmware differs", cls "chip-warn"}
        app older
            - {text "Newer than the release", cls ""}
        nothing differs
            - {text "Up to date", cls "chip-ok"}
        never
            - {text "Not checked", cls ""}
    FirmwareLogic.versionRow(release, installed, newer)
        Different
            - {value release, was "installed: <installed>"}; newer true appends " (newer)"
        Equal
            - {value release, was "same as installed"}
        Release null (no c6_rcp line)
            - {value "-", was ""}
        Installed null
            - was "installed: unknown"
    FirmwareLogic.checkedText(u)
        No age
            - "-"
        age < 60 / 720 / 10800 s
            - "just now" / "12 min ago" / "3 h ago"
        failed with an age
            - the age text plus " (the last check failed)"
    FirmwareLogic.updateAction(u)
        app newer or unknown
            - {label "Install v0.4.0 and reboot", primary true, force false}
        app same, rcp differs
            - {label "Install the radio firmware of v0.4.0", primary true, force false}
        app older
            - {label "Install v0.4.0 anyway", primary false, force true}
        up to date, no release, or checking
            - null (the Install button is hidden; Check again stays)
        tag null
            - "the latest release" in place of the tag
    FirmwareLogic.updateHint(u)
        app and rcp to install
            - names the bundle, the radio firmware, the half-minute Thread stop and the reboot
        app only / rcp only
            - no radio sentence / no reboot sentence
        app older
            - "This board runs a build that is newer than v0.4.0. Installing goes back to the release."
        up to date
            - "Nothing to install: the board runs the latest release."
    FirmwareLogic.updateQuestion(u, running)
        app and rcp
            - names the tag, the running stamp, the radio rewrite, the Thread stop and the reboot
        rcp only
            - names the tag and the Thread stop; no reboot
        app older
            - says the running build is newer than this release
    FirmwareLogic.checkFailedText(u)
        failed with error "TLS handshake failed (url)"
            - "The check failed: TLS handshake failed (url). The board needs internet access and DNS."
    FirmwareLogic.updateReplyText(r)
        200, rebooting true, rcp installed or unchanged
            - "Update installed. The board is rebooting; this page reloads when it is back."
        200, rebooting true, rcp failed
            - the same text; {rcpFailed: detail} to keep across the reload
        200, rebooting false
            - "Radio firmware installed: <stamp>."
        Not 200
            - kit.failure(r), shown red
    FirmwareLogic.repoLabel(url)
        "https://github.com/t3hk0d3/esp32-p4-otbr"
            - "t3hk0d3/esp32-p4-otbr"
    FirmwarePage Updates card
        Page opens, never checked
            - GET /device/update?check=true, then polls the plain GET until not checking
            - status line "Checking GitHub for the latest release..."; Install and Check again disabled meanwhile
        Check again clicked
            - GET /device/update?check=true; 409 busy shows the failure text in red
        Install clicked, confirm() cancelled
            - no request
        Install clicked and confirmed
            - POST /device/update (?force=1 when the action says so) inside runOp: every action button on the page is
              disabled; status line "The device is downloading v0.4.0 from GitHub and installing it ..."
        Reply rebooting true
            - ota:before stored, update:after stored with {rcpFailed}; waitForBoard('updMsg')
        Reload after the reboot with update:after stored
            - afterUpdateText goes to updMsg (not otaMsg); a rcpFailed detail adds the red line "The Border Router
              was updated, but the radio firmware was not: <detail>. Install again to update the radio firmware only."
            - both keys cleared
        Reply rebooting false
            - the radio text in updMsg; refreshStatus and a new check
        Release and repo links
            - an <a> only for https URLs; every text through textContent
        Manual cards
            - otaInstall and rcpInstall are btn-outline; updInstall is btn-primary unless the action says otherwise
```

## Tasks

### Task 1: HTTPS for URL downloads

**Files:**
- Create: `p4/components/self_update/CMakeLists.txt`
- Create: `p4/components/self_update/include/self_update.h`
- Create: `p4/components/self_update/self_update_http.c`
- Modify: `p4/sdkconfig.defaults`: certificate bundle CMN; a comment on the socket budget line saying the update check
  shares the one HTTP client
- Modify: `p4/components/device_api/limits.c`, `device_api_priv.h` (doc comment), `device_api.c` (the OTA config at
  line 700 and the radio config at line 1000 use the new config), `device_api/CMakeLists.txt` (`PRIV_REQUIRES self_update`)
- Modify: `p4/components/esp_ot_br_server/frontend/static/firmware.js` (`checkUpdateUrl`), `frontend/firmware.html`
  (both URL labels "Download address (http or https)", placeholders `https://host/...`), `p4/components/PATCHES.md`
- Modify: `docs/api.md` (rows for `/device/ota/url` and `from_url`), `docs/updates.md` (https accepted), `STYLEGUIDE.md`
  (prefix list)
- Test: `p4/components/device_api/test_host/main/test_limits.c`, `tools/tests/test_firmware_page.py`

**Interfaces:**
- Produces: `esp_http_client_config_t self_update_http_config(const char *url, int timeout_ms)` (certificate bundle,
  `buffer_size` 2048, `buffer_size_tx` 1024). In this task the CMakeLists registers only `self_update_http.c` (target).
  Task 3 adds the linux-target branch with the pure files, following `device_api/CMakeLists.txt`.

- [ ] Update the `device_api_check_url` tests in `test_limits.c` to the limits scenarios in the BDD skeleton; replace
  the https page test with "https URL accepted". Run `make test-host` and `make test-tools`: the new cases fail.
- [ ] Implement: `check_url` accepts both schemes; both URL clients in `device_api.c` use `self_update_http_config`;
  `checkUpdateUrl` accepts https; set the sdkconfig default. Add the PATCHES.md entry.
- [ ] `make test-host test-tools` pass. `logrun make build` passes. `make sizes` shows the P4 app growing by about
  70 KB at most.
- [ ] Docs: api.md, updates.md, styleguide prefix. `make lint`. Commit `feat(ota): https URLs with the certificate bundle`.

### Task 2: release tag in versions.txt

BDD skipped: a packaging change of one line. It is checked by the verification below.

**Files:**
- Modify: `tools/package_release.sh`: write `release:     $RELEASE_TAG` as the first line when `RELEASE_TAG` is set
  and matches the tag rule, with the value padded like the other keys; fail when it is set but malformed.
- Modify: `.github/workflows/build.yml`: the build job's Package step sets `RELEASE_TAG` to `github.ref_name` on
  `refs/tags/v*`, empty otherwise.
- Modify: `Makefile`: the `package` help comment mentions `RELEASE_TAG`.

- [ ] With the existing builds: `logrun bash -c 'RELEASE_TAG=v9.9.9 make package && head -n 2 dist/versions.txt'`
  prints the `release:` line, then `p4 app:`. Without `RELEASE_TAG` there is no release line. `RELEASE_TAG='v1/x'`
  exits with an error.
- [ ] Commit `build(release): tag line in versions.txt`.

### Task 3: self_update core

**Files:**
- Create: `p4/components/self_update/self_update_core.c`, `p4/components/self_update/self_update_priv.h` (pure
  declarations, the way `device_api_priv.h` works)
- Create: `p4/components/self_update/test_host/` (same layout as `device_api/test_host`), `test_host/main/test_core.c`

**Interfaces:**
- Produces, in `self_update.h`:
  - `typedef struct { char tag[33]; char app[32]; char rcp[32]; } self_update_release_t;` (an empty string means
    absent);
  - `typedef enum { SELF_UPDATE_NEWER, SELF_UPDATE_SAME, SELF_UPDATE_OLDER, SELF_UPDATE_UNKNOWN } self_update_cmp_t;`;
  - `#define SELF_UPDATE_REPO_URL`, `SELF_UPDATE_RELEASES_URL` (`SELF_UPDATE_REPO_URL "/releases"`),
    `SELF_UPDATE_VERSIONS_MAX 1024`, `SELF_UPDATE_FETCH_TIMEOUT_MS 15000`.
- Produces, in `self_update_priv.h`:
  - `esp_err_t self_update_parse_versions(const char *text, size_t len, self_update_release_t *out)`;
  - `self_update_cmp_t self_update_compare(const char *release_app, const char *running_app)`;
  - `bool self_update_rcp_differs(const char *release_rcp, const char *running_rcp, bool running_known)`;
  - `esp_err_t self_update_asset_url(const char *base, const char *tag, const char *file, char *out, size_t cap)`;
  - `esp_err_t self_update_release_page_url(const char *base, const char *tag, char *out, size_t cap)`.

- [ ] Write the host tests for the "self_update core" part of the skeleton, one `TEST_CASE` per effect, tag
  `[self_update]`. `make test-host`: they fail to link.
- [ ] Implement the five functions. The parser works on a bounded copy, never past `len`, and writes `out` only on
  `ESP_OK`.
- [ ] `make test-host` passes. `make lint`. Commit `feat(self_update): versions.txt parser, version order, release URLs`.

### Task 4: check state and fetch

**Files:**
- Create: `p4/components/self_update/self_update.c` (state, mutex, the fetch through `esp_http_client` on target, the
  fetch ops seam)
- Modify: `p4/components/self_update/include/self_update.h`, `self_update_priv.h`
- Modify: `p4/main/test_cmds.c`: test builds get `update_source <base>` (sets the base; empty restores the default)
- Test: `p4/components/self_update/test_host/main/test_state.c`

**Interfaces:**
- Consumes: Task 3's functions; Task 1's `self_update_http_config`.
- Produces, in `self_update.h`:
  - `typedef enum { SELF_UPDATE_NEVER, SELF_UPDATE_CHECKING, SELF_UPDATE_DONE, SELF_UPDATE_FAILED } self_update_state_t;`
  - `typedef struct { self_update_state_t state; bool has_age; uint32_t age_s; char error[96]; bool has_release; self_update_release_t release; } self_update_snapshot_t;`
  - `bool self_update_check_begin(void)`: true moves to checking;
  - `void self_update_check_run(void)`: blocking; call only after begin returned true;
  - `esp_err_t self_update_fetch_release(self_update_release_t *out, char *detail, size_t cap)`: a fresh fetch and
    parse that does not touch the cache (the install uses it);
  - `void self_update_get(self_update_snapshot_t *out)`;
  - `const char *self_update_base(void)`;
  - `#ifdef OTBR_TEST_BUILD void self_update_test_set_base(const char *base)`.
- Produces, in `self_update_priv.h` (host seam):
  - `typedef struct { esp_err_t (*fetch)(const char *url, char *buf, size_t cap, size_t *len, char *detail, size_t detail_cap); int64_t (*now_us)(void); } self_update_ops_t;`
  - `void self_update_set_ops(const self_update_ops_t *ops)`.

- [ ] Write the host tests for the "check state" part of the skeleton. `make test-host`: they fail.
- [ ] Implement:
  - the target fetch reads at most `SELF_UPDATE_VERSIONS_MAX` + 1 bytes and calls more an error ("versions.txt too
    large");
  - it expects 200;
  - the detail names the URL asked for, never the redirect.
- [ ] `make test-host` passes. `logrun make build` and `logrun make build-test` pass. `make lint`. Commit
  `feat(self_update): update check with a cached result`.

### Task 5: device_api endpoints

**Files:**
- Create: `p4/components/device_api/update_json.c`
- Modify: `p4/components/device_api/device_api_priv.h`, `long_op.c` (`DEVICE_API_OP_UPDATE_CHECK`), `device_api.c`:
  - the two handlers (the GET parses `check` with `device_api_query_value`);
  - a check worker;
  - an install worker that reuses `ota_succeeded`, the OTA error mapping, `url_source_*` and
    `br_core_push_radio_firmware` (it holds `DEVICE_API_OP_OTA` for both steps, as decision 4 describes);
  - URI handler registration.
- Modify: `p4/components/device_api/CMakeLists.txt` (`update_json.c` in both lists)
- Create: `p4/pytest/test_self_update.py` (the "device_api endpoints" scenarios; it runs in the hardware pass only)
- Test: `test_host/main/test_update.c` (new), `test_long_op.c`

**Interfaces:**
- Consumes: Task 4's `self_update_*`; Task 3's `self_update_compare`, `self_update_rcp_differs`,
  `self_update_asset_url`, `self_update_release_page_url`.
- Produces, in `device_api_priv.h`:
  - `device_api_error_t device_api_update_decide(self_update_cmp_t cmp, bool rcp_differs, bool has_rcp, bool force, bool *install_app, bool *install_rcp)`;
  - `int device_api_update_json(char *buf, size_t cap, const self_update_snapshot_t *snap, const char *base, const char *running_app, bool rcp_known, const char *rcp_installed)`;
  - `int device_api_update_reply_json(char *buf, size_t cap, bool app_installed, const char *rcp_result, const char *detail, bool rebooting)`.

- [ ] Write the host tests for "update decision" and "long operations". `make test-host`: they fail.
- [ ] Implement the pure functions and the op kind.
- [ ] Implement the handlers and workers to the endpoint contract:
  - the install worker calls `self_update_fetch_release`;
  - it decides, then runs the bundle step, then the RCP step;
  - it replies, then reboots only when the app was installed.
- [ ] Write `p4/pytest/test_self_update.py`. It uses a local `http.server` with a `releases/` tree built from
  `dist/` and points the board at it with `update_source`. Do not run it on the board.
- [ ] `make test-host` passes. `logrun make build` and `logrun make build-test` pass. `make lint`. Commit
  `feat(device_api): GET/POST /device/update with check=true`.

### Task 6: Firmware page Updates card

**Files:**
- Modify: `p4/components/esp_ot_br_server/frontend/firmware.html`:
  - a `section.card.mb-16#updCard` above `#firmwareSection`, laid out as in the mockups: a `tile-head` with
    `#updChip`, a `card-sub` with the repo link `#updRepo`, `dl.facts` (`#updRelease`, `#updApp`, `#updRcp`,
    `#updChecked`), and an `.editor` fieldset "Install the release" with `#updHint`, `#updInstall`, `#updCheck`
    and the status line `#updMsg`;
  - `otaInstall` and `rcpInstall` become `btn btn-outline`.
- Modify: `frontend/static/style.css`: one rule, `.facts .was` (block, `--text-muted`, 12px).
- Modify: `frontend/static/firmware.js`, `p4/components/PATCHES.md`.
- Test: `tools/tests/test_firmware_page.py` (new `TestFirmwareLogic` cases and `FirmwarePage` scenarios through the
  existing harness).

**Interfaces:**
- Consumes: the `GET /device/update` JSON and the `POST` replies from the contract above; `DeviceKit`'s `runOp`,
  `note`, `failure`, `waitForBoard` and `request`; the page's `stored`, `OTA_KEY` and `afterUpdateText`.
- Produces, on `FirmwareLogic`: `updateNeedsCheck`, `updateChip`, `versionRow`, `checkedText`, `updateAction`,
  `updateHint`, `updateQuestion`, `checkFailedText`, `updateReplyText`, `repoLabel`, with the shapes in the skeleton.
  A new storage key `UPDATE_KEY = 'update:after'` (JSON `{rcpFailed: <detail> | null}`). `updInstall` and `updCheck`
  join `actionIds`.

The card follows the page's existing patterns, not new ones:
- the confirm is `env.confirm`, as on the other cards;
- progress and errors go through `note('updMsg', ...)`;
- there is no spinner, banner or modal.

- [ ] Write the page tests for the "firmware page" part of the skeleton. `make test-tools`: they fail.
- [ ] Implement the pure functions, then the card's wiring, in ES5. The card renders from `GET /device/update` plus
  the running versions of the last status; polling while `checking` reuses the page's refresh timer.
- [ ] Markup and the style rule as listed. Add the PATCHES.md entry (firmware.html, firmware.js, style.css). The
  visual comparison with the mockups is hardware pass item 6 (the page needs a board to render).
- [ ] `make test-tools` passes. `logrun make build` passes (the web image is packed). `make lint`. Commit
  `feat(web): Updates card on the Firmware page`.

### Task 7: docs

BDD skipped: documentation only.

- [ ] `docs/updates.md`: a new first section "Update from GitHub":
  - what is checked and when;
  - the install order and its outage;
  - the dev-build and `force` case;
  - no signatures (trust is TLS to GitHub);
  - `update_source` in test builds.
- [ ] `docs/api.md`: rows for the two endpoints, the `check` parameter and the JSON fields. `CHANGELOG.md` `Unreleased`: an Added entry.
- [ ] `make lint`. Commit `docs: update from GitHub`.

## Hardware pass (after review and fixes, one batch)

1. `pytest p4/pytest/test_self_update.py` on the bench (local server).
2. Real GitHub, from a dev build:
   - Check shows `older`, then "Install anyway" installs the latest release. If that release is v0.3.0 (no tag line),
     the latest URLs are used.
   - Record the Location sizes seen and the minimum free internal heap during the download (decision 8).
3. `POST /device/ota/url` with the `https://github.com/.../releases/download/<tag>/otbr.bundle` URL.
4. `POST /device/radio/firmware?from_url=` with an https URL.
5. Failed RCP after a committed bundle: stop the local server's `c6_rcp.bin` in the middle of the transfer. The board
   reboots into the new P4, and a second update pushes only the RCP.
6. Firmware page in a browser, against both mockups: auto-check on open, chip and facts per state, the release and
   repo links, confirm text, install with every action button disabled, the wait for reboot, the "Update done" line
   in the Updates card, the outline manual buttons.
