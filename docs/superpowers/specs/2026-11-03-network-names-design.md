# Network names (mDNS and DHCP) section: design

Date: 2026-11-03. Status: draft for review.

## 1. Purpose and scope

The board shows up on the network as `esp-otbr-xxxxxx` (mDNS), as the same string in Home Assistant, and as lwIP's
default `espressif` in the router's DHCP client list. Users want to give it their own names. This adds a "Names" section
to the Device → Ethernet page that sets those names, plus an optional mDNS advertisement of the web UI.

In scope:

- mDNS hostname (`<name>.local`);
- the name Home Assistant shows (the `_meshcop._udp` instance name), following the hostname by default;
- an optional `_http._tcp` advertisement of the web UI, off by default;
- the hostname the board sends in DHCP requests, a separate setting with the default `esp-otbr`;
- showing the hostname actually in use when the mDNS library renamed it after a conflict;
- a new API endpoint, storage, the page section, tests, docs.

Non-goals:

- an mDNS responder on the management VLAN in split mode (judged not worth the effort; see section 7);
- turning mDNS off, choosing interfaces, IPv4/IPv6 answer toggles;
- showing conflict renames of the Home Assistant name or the web UI service (hostname only);
- anything that needs a reboot or the Ethernet trial/confirm flow.

Anti-goals (failure even if it works):

- changing the name breaks discovery by Home Assistant, the SRP advertising proxy or the Border Agent;
- the section makes the board unreachable, or drops its IPv4 lease, when saved;
- the management VLAN starts carrying mDNS in split mode (breaks spec 5.13 isolation).

## 2. Decisions

| Topic | Decision |
| --- | --- |
| Placement | a "Names" section on the Ethernet page, after "Interfaces" |
| Layout | always-open form (option B of the mockups): fields are always editable; Discard and Save enable on a change |
| Apply | its own Save, applied live: no review, no trial, no reboot. Independent of the Ethernet draft and action bar |
| Hostname | empty means the default `esp-otbr-<last 3 MAC bytes, lowercase hex>` (today's name) |
| HA name | "Same as hostname" checkbox, on by default; when off, a free-text name |
| Web UI advert | `_http._tcp`, port 80, off by default; greyed out and never published in split mode |
| DHCP name | separate field; empty means the default `esp-otbr`; replaces lwIP's `espressif` on every interface |
| Live name | the page shows the hostname the mDNS library actually uses and warns when it differs from the set one |
| Persistence | NVS, survives reboot and firmware updates; not touched by `net reset` |
| Upgrade | a board with nothing stored keeps its mDNS hostname and HA name; only the DHCP name changes (`espressif` → `esp-otbr`) |

## 3. Facts this design rests on (checked in the tree)

- `br_core_mdns_start()` (`p4/components/br_core/br_core.c`) sets the hostname and the mDNS instance name to
  `esp-otbr-xxxxxx` at boot. The library is `espressif/mdns` pinned at 1.14.1 (`tools/tests/test_mdns_config.py`).
- On a probe conflict the library renames the host (`mangle_name` in `mdns_receive.c`, `name` → `name-2`) and calls the
  callbacks registered with `mdns_register_hostname_changed_callback`. `esp_br_web.c` already registers one (logging
  only). `mdns_hostname_get()` returns the name in use.
- Home Assistant's Thread panel labels each border router by its `_meshcop._udp` instance name, falling back to the
  `mn` TXT value and then the SRV host (`thread-config-panel.ts` in home-assistant/frontend: `instance_name ||
  model_name || server`). The server host is shown as secondary text.
- That instance name comes from ESP-IDF's own `_meshcop._udp` publisher, not from OpenThread's
  `otBorderAgentSetMeshCoPServiceBaseName`. `esp_openthread_state.c` republishes with
  `esp_openthread_get_meshcop_instance_name()` on every role change. The firmware never calls
  `esp_openthread_set_meshcop_instance_name()`, so the name is NULL and the publisher uses the hostname. The header says
  the setter may only be called before `esp_openthread_border_router_init`, but the disassembly of the prebuilt
  `libopenthread_br.a` (esp32p4) shows no such guard. It frees the old string and stores `strndup(name, 63)`, so it
  works after init and copies. `esp_openthread_publish_meshcop_mdns(name)` returns early while the service is already
  published, so a rename needs `esp_openthread_remove_meshcop_mdns()` first.
- Netifs are created in `netcfg/ifaces.c` (`add_iface`) from `ESP_NETIF_INHERENT_DEFAULT_ETH()`, so DHCP sends
  `CONFIG_LWIP_LOCAL_HOSTNAME` (`espressif`).
- In split mode mDNS runs only on `thread_bb` (`netcfg_mdns_allowed`), and the web UI is served only on `mgmt`
  (`netcfg_policy`). An `_http._tcp` record would therefore point at an interface that refuses the web UI.
- `/device/*` routes are registered in `device_api_register()` and gated by `netcfg_request_allowed(…DEVICE_API)`.

## 4. Architecture

- New component `p4/components/netnames/` (prefix `netnames_`), following `STYLEGUIDE.md`:
  - `rules.c`: pure validation, normalisation and default resolution (no ESP-IDF calls); host-tested in
    `netnames/test_host/`;
  - `netnames.c`: NVS storage (own namespace, e.g. `otbr_names`), the effective values, and the glue that applies them
    to the mDNS library, the meshcop publisher and the netifs;
  - `include/netnames.h`: the public API below.
- `br_core_mdns_start()` takes the hostname and the instance name from `netnames` instead of building them itself.
  `br_core` boot also calls `esp_openthread_set_meshcop_instance_name()` with the effective HA name before
  `esp_openthread_border_router_init`.
- `netcfg/ifaces.c` sets each netif's DHCP hostname (`esp_netif_set_hostname`) to the effective DHCP name before DHCP
  starts.
- `device_api` gets `GET /device/names` and `PUT /device/names` in its route table, behind the same policy gate as the
  other `/device/*` routes. The handler budget in `esp_br_web.c` (`EXTRA_HANDLER_SLOTS 16`
  plus 30 spare in `max_uri_handlers`) already takes 16 device routes plus the catch-all. Raise `EXTRA_HANDLER_SLOTS`
  to cover the new two. That is a change to vendored code and gets an entry in `PATCHES.md`.
- Frontend: a new section in `ethernet.html`, logic in a new `static/names.js` (pure part `NamesLogic`, runnable under
  node like `EthernetLogic`, page part `NamesPage`), styles reused from `style.css`.

### Public API (`netnames.h`)

- `esp_err_t netnames_init(void)`: loads stored values (missing keys = defaults). Called in `app_main` right after
  `storage_init`, before `br_core_mdns_start` and netcfg. A storage failure is logged and the defaults are used.
- `void netnames_get(netnames_t *out)`: the stored settings plus the effective values (defaults resolved).
- `esp_err_t netnames_set(const netnames_settings_t *in)`: validates, stores, then applies live (section 5). Returns
  `ESP_ERR_INVALID_ARG` on a validation error, with a field name and reason available for the API reply.
- `const char *netnames_dhcp_name(void)`, `const char *netnames_hostname(void)`, `const char *netnames_ha_name(void)`:
  effective values for the boot path.

Data shape (contract):

- `netnames_settings_t`: `hostname` (0–63 chars, empty = default), `ha_name_follows_hostname` (bool), `ha_name`
  (0–63 bytes UTF-8), `http_advert` (bool), `dhcp_name` (0–32 chars, empty = default; amended after review:
  `esp_netif_set_hostname` refuses longer names).

## 5. Behaviour

### Validation (rules.c)

- Hostname and DHCP name: empty, or 1–63 (DHCP name: 1–32, the `esp_netif` limit) characters of `a-z`, `0-9`, `-`,
  not starting or ending with `-`. Upper case
  is accepted and stored in lower case. Anything else is rejected with the field name.
- HA name (used only when it doesn't follow the hostname): 1–63 bytes of UTF-8, no control characters, no leading or
  trailing spaces. Empty with "follows hostname" off is rejected.
- Defaults: hostname `esp-otbr-<mac[3..5] hex>`, DHCP name `esp-otbr`, HA name = effective hostname.

### Applying on Save (live, in this order)

1. Store all fields in NVS. If storage fails, nothing is applied and the API returns 500.
2. Hostname changed: `mdns_hostname_set(new)`. The library probes and announces. If it is taken, the library renames
   and the live name differs (see "Live name").
3. HA name changed (including through a hostname change while it follows): `mdns_instance_name_set(new)`, then, if the
   meshcop service is currently published, remove and republish it with the new name under the OpenThread lock. Also
   call `esp_openthread_set_meshcop_instance_name(new)` so the next role change republishes with it. All three
   calls run under the OpenThread lock, because the role-change handler reads the stored name on the OpenThread task.
4. Web UI advert: published (`mdns_service_add`, `_http._tcp`, port 80, instance = HA name, TXT `path=/`) only when it
   is on and the mode is single. Removed when it's turned off. An instance-name change updates it. The setting is
   stored in either mode. Switching modes needs a reboot, so it is applied at boot.
5. DHCP name changed: `esp_netif_set_hostname` on every netif that uses DHCP, then a lease renewal so the router learns
   the name. The renewal must not drop the current address: no `dhcpc_stop`/`dhcpc_start`. Only an interface that
   holds a lease renews; one still looking for a lease sends the name with its next DISCOVER or REQUEST. The builder picks the
   mechanism, for example lwIP `dhcp_renew` through the TCP/IP context. Interfaces with static IPv4 just keep the
   value. The page explains that it only matters with DHCP.

Each step is skipped when its value did not change. A failure in steps 2–5 is logged and reported in the reply (`applied:
false` with a detail). The stored value still stands and applies at the next boot.

### Live name

- `netnames` registers its own hostname-changed callback and keeps the name in use (`mdns_hostname_get` at start).
- When the library renames the host and the HA name follows the hostname, the HA name stays the *set* one (no cascade).
  The page shows the live hostname and the warning.

### Boot

- `storage_init` → `netnames_init` → `br_core_mdns_start` with the stored hostname and instance name (today
  `app_main` starts mDNS before `net_init_task` runs `netcfg_start`; that order stays) → netcfg brings up netifs with
  the DHCP name → meshcop instance name set before `esp_openthread_border_router_init` → `_http._tcp` published if on
  and single mode.

## 6. HTTP API

`GET /device/names` → 200:

```json
{
  "hostname": "kitchen-otbr",
  "hostname_default": "esp-otbr-a1b2c3",
  "hostname_live": "kitchen-otbr",
  "ha_name_follows_hostname": true,
  "ha_name": "",
  "ha_name_effective": "kitchen-otbr",
  "http_advert": false,
  "http_advert_available": true,
  "dhcp_name": "",
  "dhcp_name_default": "esp-otbr",
  "mode": "single"
}
```

`PUT /device/names` with body `{hostname, ha_name_follows_hostname, ha_name, http_advert, dhcp_name}` (all required)
→ 200 with the GET body plus `"applied": true|false` and an optional `"detail"`. Errors follow the existing `/device/*`
error shape (`{"error":code,"detail":…}`) plus a `field` key for validation errors (builder's call, not in the existing
shape): 400 `{"error":"invalid_config","field":"hostname","detail":"…"}`, 500 on storage failure, 403 from the policy gate.

`docs/api.md` documents both.

## 7. Page

Section "Names" on the Ethernet page, after "Interfaces". One card with:

- **Hostname**: text input with a `.local` suffix label. Hint: allowed characters, and that empty means
  `esp-otbr-xxxxxx` (the real default from the API).
- **Name in Home Assistant**: "Same as hostname" checkbox, on by default. When on, the text input is disabled and shows
  the effective hostname. Hint: what HA shows.
- **Web UI**: checkbox "Advertise the web UI (`_http._tcp`)", off by default, with the hint "Lets network browsers and
  discovery apps find this page."
- **Name shown to your router (DHCP)**: text input. Hint: empty means `esp-otbr`, and the router sees it at the next
  lease renewal, which the board requests on Save.
- **Discard / Save**: disabled until the form differs from the loaded settings. Validation errors appear inline per
  field, using the same rules as the server (shared in `NamesLogic`).
- **Conflict state**: when `hostname_live` ≠ the effective hostname, show a "Renamed" chip and a banner: another
  device already uses `<name>.local`, the board answers as `<live>.local`, pick a different name.
- **Split mode**: the web UI checkbox is disabled with "Not available in split mode: the web UI is on the management
  VLAN, and mDNS runs only on the Thread backbone." Under the hostname: "In split mode this name resolves on the Thread
  backbone only. Open this page by its management IP address."
- **After Save**: if the page was opened via `<old live hostname>.local` and the hostname changed, the page moves to
  the new `.local` name on the same path after a short delay (about 2 s, so the library can probe). Otherwise it reloads
  the section from the API.
- The section loads and saves independently of the Ethernet draft. During an Ethernet trial or the reboot after Apply
  it is disabled, like the rest of the page.

The page's intro text gets a short mention of names.

## 8. Testing

- Host tests (`netnames/test_host/`, Unity): validation, normalisation, defaults, and the "what changed" step
  selection. Pure parts only.
- Node tests (`tools/tests/test_names_page.py` via the existing harness): form state, validation messages, dirty
  tracking, the split-mode and conflict rendering, the redirect decision.
- Target tests (`p4/pytest/test_names.py`): GET defaults on a fresh board. PUT a hostname and `mdns_query.py` resolves it
  while the old name stops answering. HA name visible in the `_meshcop._udp` browse. `_http._tcp` present only while on.
  Saving a new DHCP name keeps the board's IPv4 address and connectivity (the same address answers before, during and
  right after the save; the API reply reports the renewal requested). Conflict via `mdns_spoof.py` gives a live name
  `-2`. Invalid input gets 400. Values survive a reboot. Split mode on the VLAN rig: no `_http._tcp`, nothing on mgmt.
- Manual check during the hardware pass (no rig work): the user confirms on their router that the board's client name
  changes to the new DHCP name after Save, and to `esp-otbr` on a board with nothing stored.
- Hardware checks run as one batched pass after review and fixes, not inside each task (project rule).

## 9. Docs

- `docs/networking.md`: a "Names" section covering what each name is for, the defaults and the split-mode limits.
- `docs/api.md`: the endpoint.
- `docs/home-assistant.md`: a line on how the HA name is set.
- A screenshot of the section in `docs/images/` if the Ethernet page docs carry one.

## 10. Left to the builder

- The mechanism for a lease renewal that keeps the address (step 5).
- Exact NVS key names and the namespace name.
- Hint wording beyond what section 7 fixes.
- Whether the meshcop republish needs a short delay after the hostname change so the SRV target is the new host.
