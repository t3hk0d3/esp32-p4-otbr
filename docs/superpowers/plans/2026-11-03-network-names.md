# Network names (mDNS and DHCP): implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** a "Names" section on the Device → Ethernet page that sets the mDNS hostname, the name Home Assistant shows,
an optional `_http._tcp` web UI advertisement and the DHCP client name, all applied live and stored in NVS.

**Architecture:** a new component `netnames` owns validation (pure, host-tested), storage and live apply. It drives the
mDNS library, ESP-IDF's meshcop publisher, and netcfg's netifs. `device_api` exposes `GET`/`PUT /device/names` with a
pure JSON layer. The page gets its own `static/names.js` (pure `NamesLogic` plus `NamesPage`), tested under node.

**Tech stack:** ESP-IDF v6.1, `espressif/mdns` 1.14.1, prebuilt `libopenthread_br.a`, lwIP, cJSON, Unity host tests,
node page tests, pytest-embedded.

**Spec:** `docs/superpowers/specs/2026-11-03-network-names-design.md` (approved, `cd0aa1f`). Read it with this plan.

**Styleguide:** `STYLEGUIDE.md`. In short:
- astyle through `make lint`, `snake_case`, and the new component prefix `netnames_`;
- `esp_err_t` with `ESP_RETURN_ON_ERROR`, and one `TAG` per file;
- host tests in `<component>/test_host/`, named `"<scenario>: <effect>"`;
- target tests in `p4/pytest/test_<feature>.py`;
- routine tasks go through the Makefile;
- every change to vendored code gets a `PATCHES.md` entry.

## Global constraints

- Hostname and DHCP name: empty, or 1–63 of `a-z0-9-`, not starting or ending with `-`. Upper case is accepted and
  stored lower case.
- HA name: 1–63 bytes UTF-8, no control characters, no leading or trailing space. It is required when
  `ha_name_follows_hostname` is false.
- Defaults:
  - hostname `esp-otbr-<mac[3]mac[4]mac[5] lowercase hex>` (`ESP_MAC_ETH`);
  - DHCP name `esp-otbr`;
  - HA name = the effective hostname.
- Web UI advert: `_http._tcp`, port 80, TXT `path=/`, instance = effective HA name. Off by default. Published only in
  single mode.
- Applying never reboots, never touches the Ethernet trial, and never drops the IPv4 lease (no `dhcpc_stop/start`).
- Names live in their own NVS namespace (`otbr_names`), which `net reset` does not touch.
- No flashing inside tasks. Target tests run in one batched hardware pass after review and fixes.
- Commits end with only `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review focus

Inputs the spec implies but no task's tests would otherwise exercise. Each one has a test added in its owning task.

1. **The user types or pastes `kitchen.local` into the hostname field.** The page strips a trailing `.local` (any case)
   before validating, so the user doesn't get a confusing "invalid character" error. Owner: Task 5.
2. **A hostname change runs into a conflict, so the board answers as `name-2`.** The redirect after Save goes to the
   live name from a fresh GET, not to the typed one. Owner: Task 5.
3. **An HA name with multi-byte characters** (Cyrillic, emoji) near the limit. Both the page and the board count UTF-8
   bytes, not characters: 63 bytes passes, 64 is refused with the field named. Owners: Tasks 1 and 5.
4. **The hostname is cleared to go back to the default while the HA name follows it.** Both return to
   `esp-otbr-xxxxxx` live, and the meshcop service is republished once. Owner: Task 3 (step selection in Task 1).
5. **Saving the same hostname again after a conflict rename** (stored `x`, live `x-2`). It counts as a change and
   re-runs `mdns_hostname_set`, so the user can retry once the other device has gone. Owners: Task 1 (selection) and
   Task 3.

## Design notes

**New and changed interfaces (contract):**

- `p4/components/netnames/include/netnames.h`
  - `netnames_settings_t { char hostname[64]; bool ha_name_follows_hostname; char ha_name[64]; bool http_advert;
    char dhcp_name[64]; }`
  - `netnames_t { netnames_settings_t set; char hostname[64]; char hostname_default[64]; char hostname_live[64];
    char ha_name[64]; char dhcp_name[64]; bool http_advert_available; }`. The unprefixed fields are the effective
    values.
  - `esp_err_t netnames_init(void)`, `void netnames_get(netnames_t *out)`,
    `esp_err_t netnames_set(const netnames_settings_t *in, netnames_result_t *res)`,
    `void netnames_set_mode(netcfg_mode_t mode)`.
  - Effective-value getters for the boot path: `const char *netnames_hostname(void)`,
    `const char *netnames_ha_name(void)`, `const char *netnames_dhcp_name(void)`.
  - `netnames_result_t { const char *field; const char *reason; bool applied; char detail[64]; }`
- `p4/components/netnames/netnames_rules.h` (pure, private include)
  - `bool netnames_rules_check(const netnames_settings_t *in, const char **field, const char **reason)`
  - `void netnames_rules_normalise(netnames_settings_t *s)`, which lower-cases the hostname and DHCP name
  - `void netnames_rules_default_hostname(const uint8_t mac[6], char out[64])`
  - `void netnames_rules_effective(const netnames_settings_t *s, const char *default_hostname,
    char hostname[64], char ha_name[64], char dhcp_name[64])`
  - `netnames_steps_t netnames_rules_steps(const netnames_settings_t *old, const netnames_settings_t *new,
    const char *default_hostname, const char *live_hostname)`, which returns flags `hostname`, `ha_name`,
    `http_advert` and `dhcp_name`
- `netcfg.h`: `esp_err_t netcfg_set_dhcp_hostname(const char *name)`. It stores the name for netifs created later and
  applies it to existing DHCP netifs with a lease renewal. Also `netcfg_mode_t netcfg_running_mode(void)`.
- `br_core.h`: `esp_err_t br_core_mdns_start(const char *hostname, const char *instance)`. The signature changes and
  `br_core` no longer builds the name. Its `HOSTNAME_PREFIX` code moves into `netnames_rules_default_hostname`.
- `device_api_priv.h` (pure, host-tested):
  - `char *device_api_names_json(const netnames_t *n, netcfg_mode_t mode, const netnames_result_t *res)`, where `res`
    may be NULL for GET;
  - `esp_err_t device_api_names_parse(const char *body, netnames_settings_t *out, const char **field)`.

**Wiring and order (`p4/main/app_main.c`):**
- after `storage_init`: `netnames_init()`, which loads the names and calls `esp_openthread_set_meshcop_instance_name`
  with the effective HA name;
- `br_core_mdns_start(netnames_hostname(), netnames_ha_name())`;
- in `net_init_task`: `netcfg_set_dhcp_hostname(netnames_dhcp_name())` before `netcfg_start`, then
  `netnames_set_mode(netcfg_running_mode())` after it, which publishes `_http._tcp` if it is on and the mode is single.

**Live apply (`netnames_set`), run under the `netnames` mutex.** Validate, normalise, store. Then, for each step flag:

- **hostname:** `mdns_hostname_set`.
- **ha_name:**
  - `mdns_instance_name_set`;
  - under the OpenThread lock, `esp_openthread_set_meshcop_instance_name`;
  - if the role is child, router or leader, remove and then publish the meshcop service.
- **http_advert:** add or remove the service, or update its instance name. Publish only in single mode.
- **dhcp_name:** `netcfg_set_dhcp_hostname`.

A step that fails sets `applied = false` and a `detail`, logs it, and the remaining steps still run.

**Live name.** `netnames` registers its own `mdns_register_hostname_changed_callback`. It keeps `hostname_live` under
the mutex and seeds it from `mdns_hostname_get` at init.

**HTTP handler buffer.** The PUT body is at most 512 bytes; anything larger gets 400 `too_large`, as in other handlers.

## BDD skeleton

```
netnames_rules (rules.c, host)
    netnames_rules_check
        All fields empty, follows hostname
            - true
        Hostname "Kitchen-OTBR"
            - true (case is fixed by normalise)
        Hostname "-a", "a-", "a.b", "a_b", 64 chars
            - false, field "hostname"
        Hostname 63 chars of a-z0-9-
            - true
        DHCP name invalid like hostname
            - false, field "dhcp_name"
        Follows off, HA name empty
            - false, field "ha_name"
        Follows off, HA name with a control character, or leading/trailing space
            - false, field "ha_name"
        Follows off, HA name of 63 bytes of UTF-8 (21 Cyrillic letters)
            - true
        Follows off, HA name of 64 bytes of UTF-8
            - false, field "ha_name"
        Follows on, HA name holds junk
            - true (ignored)
    netnames_rules_normalise
        Upper-case hostname and DHCP name
            - stored lower case; HA name untouched
    netnames_rules_default_hostname
        MAC ..:a1:b2:c3
            - "esp-otbr-a1b2c3"
    netnames_rules_effective
        Nothing set
            - hostname = default, HA name = default hostname, DHCP name "esp-otbr"
        Hostname set, follows on
            - HA name = hostname
        Follows off
            - HA name = the set HA name
    netnames_rules_steps
        Nothing changed, live = effective hostname
            - no flags
        Only DHCP name changed
            - dhcp_name only
        Hostname changed, follows on
            - hostname and ha_name and http_advert (instance follows)
        Hostname changed, follows off
            - hostname only
        Hostname cleared to default, follows on
            - hostname and ha_name and http_advert
        Same hostname, live differs (conflict rename)
            - hostname
        http_advert toggled
            - http_advert only

netnames (netnames.c, target)
    netnames_init
        Nothing stored
            - defaults; meshcop instance name set to default hostname
        Stored values
            - loaded; effective values resolved
        NVS fails to open
            - logs, defaults, ESP_OK (boot continues)
    netnames_set
        Invalid input
            - ESP_ERR_INVALID_ARG; res.field and res.reason set; nothing stored or applied
        Storage write fails
            - ESP_FAIL; nothing applied
        Valid input
            - stored; the selected steps applied; netnames_get returns new values
        OpenThread not running (no role)
            - meshcop not republished; instance name still set for the next role change
        A step fails
            - ESP_OK, res.applied false, res.detail names the step; other steps still run
    netnames_set_mode
        Single and advert on
            - _http._tcp published
        Split
            - not published; http_advert_available false
    hostname changed callback
        Library renames to x-2
            - hostname_live "x-2"

netcfg_set_dhcp_hostname (netcfg, target)
    Before netcfg_start
        - stored; add_iface sets it on each new netif
    After start, DHCP netif
        - hostname set; lease renewed; IPv4 address unchanged
    After start, static netif
        - hostname set; no DHCP action
    Name longer than 63
        - ESP_ERR_INVALID_ARG
netcfg_running_mode
    - the running mode; NETCFG_MODE_SINGLE before start

device_api names JSON (host)
    device_api_names_json
        GET (res NULL)
            - every spec section 6 field; mode "single"/"split"; no "applied"
        PUT result
            - adds "applied"; adds "detail" only when not empty
    device_api_names_parse
        Valid body
            - fills every field
        Missing field, wrong type, not an object
            - ESP_ERR_INVALID_ARG; field names the key ("body" when not an object)
        String longer than 63 bytes
            - ESP_ERR_INVALID_ARG; field names the key
GET /device/names                  - 200 with device_api_names_json
PUT /device/names
    Body too large                 - 400 too_large
    Parse or validation error      - 400 {"error":"invalid_config","field":..,"detail":..}
    Storage failure                - 500
    Success                        - 200 with the result JSON
    Split mode, Thread side        - 403 like every /device/* route

NamesLogic (names.js, node)
    fromApi
        - form state from the GET body; follows checkbox from ha_name_follows_hostname
    normaliseHostname
        "kitchen.local", "Kitchen.LOCAL"
            - "kitchen" (lower-cased by the server; field shows what was typed minus .local)
    validate
        - same cases as netnames_rules_check, with the same field keys and UTF-8 byte counting
    dirty
        Unchanged form
            - false
        Any field changed
            - true
    toBody
        - the five PUT fields
    redirectTarget(location, before, after)
        Page host is "<before.hostname_live>.local", live hostname changed
            - "<after.hostname_live>.local" with the same path
        Page opened by IP
            - null
        Live hostname unchanged
            - null
NamesPage (harness)
    Load
        - fields filled; Save and Discard disabled
    Edit
        - Save and Discard enabled; inline error per invalid field; Save disabled while invalid
    Follows checked
        - HA input disabled and shows the effective hostname
    Split mode
        - advert checkbox disabled with the split-mode hint; hostname split-mode note shown
    Conflict (hostname_live differs)
        - "Renamed" chip and banner naming both names
    Save success
        - PUT sent; then GET after ~2 s; redirect when redirectTarget says so, else reload section
    Save 400
        - error shown under the named field
    Save applied false
        - detail shown as a warning; values reloaded
    Ethernet trial or reboot pending
        - section disabled

On-board (p4/pytest/test_names.py, batched hardware pass)
    Fresh board
        - GET shows defaults; esp-otbr-xxxxxx.local resolves
    PUT hostname
        - new name resolves; old stops answering; IPv4 unchanged
    HA name
        - _meshcop._udp browse shows the new instance
    Web UI advert
        - _http._tcp present only while on
    DHCP name
        - same IPv4 answers before, during and after the save
    Conflict (mdns_spoof.py)
        - hostname_live ends in "-2"
    Invalid input
        - 400
    Reboot
        - values kept
    Split mode (VLAN rig)
        - no _http._tcp; nothing answered on mgmt
Manual (hardware pass)
    - router lists the new DHCP name; a fresh board shows esp-otbr
```

## Tasks

### Task 1: `netnames` rules (pure)

**Files:**
- Create: `p4/components/netnames/{CMakeLists.txt, rules.c, netnames_rules.h, include/netnames.h}` (the types only so
  far).
- Create: `p4/components/netnames/test_host/{CMakeLists.txt, main/CMakeLists.txt, main/test_rules.c}`, laid out like
  `netcfg/test_host`.

**Interfaces:** produces `netnames_settings_t`, `netnames_rules_*` and `netnames_steps_t` (see Design notes).

- [ ] Write `test_rules.c` from the `netnames_rules` branch of the skeleton, one `TEST_CASE` per effect, tagged
  `[netnames]`. It includes the 63/64-byte UTF-8 cases (Review focus 3) and the conflict-retry and clear-to-default step
  cases (Review focus 4 and 5).
- [ ] Run `logrun make test-host`. Expected: the netnames suite fails to build (no `rules.c`).
- [ ] Implement `rules.c`. The linux-target `CMakeLists.txt` branch builds `rules.c` only, like netcfg's.
- [ ] Run `logrun make test-host`. Expected: all suites green.
- [ ] Commit `feat(netnames): name rules`.

### Task 2: netcfg DHCP name and running mode

**Files:** modify `p4/components/netcfg/include/netcfg.h`, `netcfg.c`, `ifaces.c` (`add_iface`).

**Interfaces:** produces `netcfg_set_dhcp_hostname` and `netcfg_running_mode`.

- [ ] Implement `netcfg_set_dhcp_hostname`:
  - keep a 64-byte copy under `s.lock`;
  - in `add_iface`, call `esp_netif_set_hostname` before DHCP starts;
  - after start, for each DHCP netif, set the hostname and ask lwIP for a renewal on the TCP/IP thread (e.g.
    `dhcp_renew` on `esp_netif_get_netif_impl`, through `esp_netif_tcpip_exec` or `tcpip_callback`). No stop/start.
- [ ] Implement `netcfg_running_mode`: `s.running.mode` under the lock, or single before start.
- [ ] Run `logrun make build`. Expected: OK. Run `logrun make test-host`: netcfg suite still green.
- [ ] Commit `feat(netcfg): settable DHCP hostname, running mode accessor`.

### Task 3: `netnames` runtime and boot wiring

**Files:**
- Create: `p4/components/netnames/netnames.c`.
- Modify: `netnames/CMakeLists.txt` (target branch: `PRIV_REQUIRES mdns nvs_flash openthread netcfg esp_netif log`).
- Modify: `p4/components/br_core/br_core.c` and `include/br_core.h` (`br_core_mdns_start` signature; drop
  `HOSTNAME_PREFIX`).
- Modify: `p4/main/app_main.c` and `p4/main/CMakeLists.txt`.

**Interfaces:** consumes Tasks 1 and 2. Produces `netnames_init`, `netnames_get`, `netnames_set`, `netnames_set_mode`
and the getters.

- [ ] Implement `netnames.c` against the `netnames` branch of the skeleton and the "Live apply" note.
  - Static buffers that live for the board's lifetime hold the effective names.
  - The steps come from `netnames_rules_steps`, with the live hostname passed in so that Review focus 5 holds.
  - The meshcop remove-then-publish runs only when `otThreadGetDeviceRole` is child, router or leader, all under
    `esp_openthread_lock_acquire`. Skip it while `br_core` reports OpenThread not running.
- [ ] Change `br_core_mdns_start` to take the hostname and instance. Its boot log line keeps `mDNS hostname %s.local`
  (pytest greps it).
- [ ] Wire `app_main.c` in the order given in Design notes.
- [ ] Run `logrun make build` and `logrun make test-host`. Expected: both OK.
- [ ] Commit `feat(netnames): stored names applied at boot and live`.

### Task 4: `/device/names` API

**Files:**
- Create: `p4/components/device_api/names_json.c` (pure).
- Create: `p4/components/device_api/test_host/main/test_names.c`.
- Modify: `device_api_priv.h`, `device_api.c` (two routes and handlers), `device_api/CMakeLists.txt` (host branch adds
  `names_json.c`; target `PRIV_REQUIRES netnames`).
- Modify: `p4/components/esp_ot_br_server/src/esp_br_web.c` (`EXTRA_HANDLER_SLOTS` 16 → 20) and
  `p4/components/PATCHES.md` (patch entry).
- Modify: `docs/api.md`.

**Interfaces:** consumes `netnames_get` and `netnames_set`. Produces the HTTP contract in spec section 6.

- [ ] Write `test_names.c` from the `device_api names JSON` branch of the skeleton. The host build gets `netnames.h`
  types through the include path only (no `netnames` runtime).
- [ ] Run `logrun make test-host`. Expected: the device_api suite fails to build.
- [ ] Implement `names_json.c` with cJSON, like `status_json.c`.
- [ ] Run `logrun make test-host`. Expected: green.
- [ ] Add the handlers:
  - `read_small_body` with a 512-byte cap, then `device_api_names_parse`, then `netnames_set`;
  - map errors as in the skeleton; `send_error` gains no new shape, and the `field` key is added by building that
    body with `names_json.c`.
- [ ] Add the two `/device/names` rows to `docs/api.md`.
- [ ] Run `logrun make build`. Expected: OK.
- [ ] Commit `feat(device_api): GET/PUT /device/names`.

### Task 5: Names section on the Ethernet page

**Files:**
- Create: `p4/components/esp_ot_br_server/frontend/static/names.js`, exporting `NamesLogic` with
  `module.exports.Page = NamesPage`, like `ethernet.js`.
- Modify: `frontend/ethernet.html` (section after Interfaces, the script tag, intro text) and `frontend/static/style.css`
  (only if an existing class doesn't fit: the `.local` suffix label and the conflict banner reuse `disabled-banner`).
- Create: `tools/tests/test_names_page.py`.
- Modify: `tools/tests/device_pages_harness.js` (a `/device/names` mock and a location override, if missing).

**Interfaces:** consumes the spec section 6 JSON. The page reuses `DeviceCommon` helpers and the Ethernet page's busy
state (`EthernetPage.kit.isBusy()`) to disable itself during a trial or reboot.

- [ ] Write `test_names_page.py` from the `NamesLogic` and `NamesPage` branches of the skeleton. It includes the
  `.local` paste (Review focus 1), redirect to the *live* name after a conflict (Review focus 2), and UTF-8 byte
  counting (Review focus 3).
- [ ] Run `logrun make test-tools`. Expected: the new tests fail (no `names.js`).
- [ ] Implement `names.js` and the HTML to the approved mockup B and spec section 7. The copy is in spec section 7.
- [ ] Run `logrun make test-tools`. Expected: all green, `test_ethernet_page.py` included.
- [ ] Commit `feat(web): names section on the Ethernet page`.

### Task 6: On-board tests and docs

**Files:**
- Create: `p4/pytest/test_names.py`, which reuses `mdns_query.py` and `mdns_spoof.py`; the split case is marked
  `vlan_switch` like `test_vlan.py`.
- Modify: `docs/networking.md` ("Names" section), `docs/home-assistant.md` (how the HA name is set, replacing the "listed
  by its mDNS name" line), and `CHANGELOG.md` (Unreleased).

- [ ] Write `test_names.py` from the on-board branch of the skeleton. It restores the default names in a fixture
  teardown. Don't run it: hardware pass.
- [ ] Write the docs. This step needs no BDD, since it's docs only.
- [ ] Run `logrun make lint` and `logrun make test`. Expected: both green.
- [ ] Commit `test(names): on-board tests; docs: network names`.

### Task 7: Review, then the hardware pass

- [ ] Whole-branch review on the most capable model. Fix the Critical and Important findings test-first, and commit
  each fix.
- [ ] Batched hardware pass (with the user):
  - `make test-target TEST=p4/pytest/test_names.py`;
  - the split case on the VLAN rig;
  - you check the DHCP name on your router.

## Verification

- `logrun make test-host`: the netnames and device_api suites are green.
- `logrun make test-tools`: the names page tests and the existing page tests are green.
- `logrun make build` and `logrun make lint`: OK.
- Hardware (deferred, batched): `make test-target TEST=p4/pytest/test_names.py`, the VLAN rig split case, and the
  manual router check.
