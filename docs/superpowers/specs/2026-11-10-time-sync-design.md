# Time sync (NTP): design

Date: 2026-11-10. Status: draft, waiting for approval. Branch `feat/rtc-ntp-sync-401f02`, on `master` (v0.5.0).
Mockup: `2026-11-10-time-sync-mockup.html` (states 1 to 5, 1b to 1d).

## 1. Purpose and scope

Nothing sets the P4's clock. The code that wants wall-clock time already exists and reads "clock not set":
- Zigbee status: `client_since`, `last_client_left`, `started` (`p4/components/zigbee`, shown on the Zigbee page);
- the radio mux: the firmware install time `fw.at` (`p4/components/radio_mux`, shown on the Radios page);
- the logging design (`2026-11-10-logging-design.md`, branch `feat/logging-implementation-e64924`): syslog lines carry
  an RFC 5424 timestamp only when the clock is valid.

This change makes the board set its clock from an NTP server, lets the user choose the server in the web UI, and
makes the existing timestamps show real dates, including for things that happened before the first sync.

Goals:
- The clock is right within a few seconds of the uplink getting an address, with no setup.
- The server is: the one set in the web UI; else the ones the DHCP lease names; else `pool.ntp.org`.
- The Ethernet page shows the board time, whether it is synced, which server is used and where it came from.
- Other components can ask "is the clock valid" cheaply.

Non-goals:
- A battery-backed clock. The board has no RTC chip. After a power loss the clock is unset until the first sync.
- Time zones on the board. The board keeps UTC. The browser shows local time (as `zigbee.js` does already).
- Wall-clock time in log lines. They stay uptime ms; `CONFIG_LOG_TIMESTAMP_SOURCE` isn't changed (agreed with the
  logging session). Only syslog uses the clock, and that is the logging branch's work.
- Serving time to others: no NTP server on the board, no Thread network time, no Zigbee Time cluster.
- NTP servers from DHCPv6 or router advertisements. Only DHCPv4 option 42 is read.
- Authenticated time (NTS). Plain SNTP.
- A "Sync now" button. Saving the settings restarts the sync.
- A metric in `/metrics`.

Anti-goals:
- NTP traffic when the user turned sync off.
- A UI that claims "Synced" when no server has answered for hours.
- A blocking or lock-taking "is the clock valid" query (it is called from the log path).

## 2. What exists (found 2026-11-10)

- ESP-IDF v6.1. `esp_netif_sntp` wraps lwIP's SNTP client. It uses a raw UDP PCB, not a socket: `CONFIG_LWIP_MAX_SOCKETS`
  and its comment in `p4/sdkconfig.defaults` don't change.
- `CONFIG_LWIP_DHCP_GET_NTP_SRV` is off today. With it on, lwIP asks for option 42 and hands the answer to the SNTP
  module through `dhcp_set_ntp_servers(count, addrs)`. That call has **no interface argument** and is global: with two
  DHCP interfaces (split mode) the last lease wins, and stock IDF doesn't say which lease it was.
- IDF's default `CONFIG_LIBC_TIME_SYSCALL_USE_RTC_HRT` should keep the clock across a soft restart (OTA, reboot from
  the UI). Not verified on the P4: it's on the hardware checklist (8).
- Zigbee and the mux each test `time(NULL) >= 2020-01-01` and record `0` for "clock not set". Zigbee records `started`
  at boot, before any sync can have happened, so with NTP alone it would read "clock not set" until the next restart.
  Section 5 fixes that.
- Settings of this kind (`netnames`) are a component with pure rules (`rules.c`, host-tested), NVS storage, a
  `GET`/`PUT /device/...` pair in `device_api`, and a section on the Ethernet page with Discard / Save.

## 3. Decisions

"User" marks what the user decided; the rest are the designer's calls, open to change.

| Topic | Decision | By |
| --- | --- | --- |
| Why | Real dates in status timestamps (Zigbee, radios) and in syslog | User |
| Server choice | Configurable in the web UI | User |
| Default | DHCP's server when the lease names one, otherwise `pool.ntp.org` | User |
| UI | A Time section on the Ethernet page, as in the mockup | User (sketch accepted) |
| Order | A custom server overrides DHCP | Designer |
| Custom server | Used alone: no fallback to DHCP's servers or the pool | Designer |
| DHCP servers | Up to two are used; more are ignored. While the lease names any, the pool is not used, even if they don't answer | Designer |
| Off switch | One toggle; off means no NTP traffic at all | Designer |
| Time zone | None on the board; UTC only | Designer |
| Clock step | The clock is set at once on each answer (no slewing) | Designer |
| Poll interval | One hour (IDF default); first request after IDF's default random startup delay (up to 5 s) | Designer |
| "Valid" | `time(NULL) >= 2020-01-01 00:00:00 UTC`, the same threshold Zigbee and the mux use | Designer |
| Early events | Zigbee records uptime and converts to a date when read, so events before the first sync get real dates | Designer |
| Mux `fw.at` | Unchanged. It is stored in NVS as epoch seconds; an install made before the first sync still records "clock not set" | Designer |
| Split mode | Servers from either lease are used (lwIP: last lease wins). The UI names the lease only if the builder finds it without patching IDF (6.3) | Designer |
| Apply | Live, no reboot, no trial | Designer |

## 4. Behaviour

### 4.1 Which servers are used

| Enabled | Mode | Lease names servers | Servers in use | `source` |
| --- | --- | --- | --- | --- |
| no | any | any | none | `null` |
| yes | `custom` | any | the custom server | `custom` |
| yes | `auto` | yes | the lease's first two | `dhcp` |
| yes | `auto` | no (or every interface is static) | `pool.ntp.org` | `default` |

In `auto` the choice follows the lease: when a lease arrives, renews or is lost, the list is rebuilt.

### 4.2 Sync state

| `state` | Meaning |
| --- | --- |
| `off` | sync is disabled |
| `waiting` | enabled; no answer yet since boot or since the settings changed, and under a minute has passed |
| `synced` | a server in use answered, and at least one of them is still reachable |
| `unreachable` | enabled; no answer for a minute after start, or every server in use stopped answering (lwIP's reachability register is 0 for all of them) |

The clock's validity is a separate fact (`now` is `null` when not valid). After a soft restart the clock can be valid
while the state is `waiting`.

### 4.3 What the page shows (mockup)

| Chip | When |
| --- | --- |
| Synced (ok) | `synced` |
| Off (neutral) | `off` |
| Clock not set (warn) | enabled and `now` is `null` |
| Server not answering (warn) | `unreachable` and the clock is valid |
| Waiting for server (neutral) | `waiting` and the clock is valid |

- **Board time:** `now`, shown in the browser's time zone and ticking in the page between refreshes; "Not set" when `null`.
- **Last synced:** from `synced_age_s`: "4 minutes ago"; "Never since power-up" when `null` and the clock isn't valid;
  "Before the last restart" when `null` and the clock is valid.
- **Server(s):** each server in use, the one that answered last marked, and a source line: "from your router (DHCP)"
  (with the interface name when known), "default; your router names none", or "set here".
- **Form:** the toggle; Automatic / Custom server; the server field only under Custom; under Custom, a hint naming what
  the router offers when it offers any (1d). Discard / Save as in the other sections. The custom server is kept
  when the user switches to Automatic, so switching back restores it.
- **Interface card (1c):** a "Time server" row with the lease's servers, on the card of the interface they came from.
  If the interface isn't known (6.3) the row is left out and the Time section alone shows them.
- The section refreshes with the page's existing polling.

## 5. Parts

### 5.1 New component `p4/components/timesync` (prefix `timesync_`)

Layout as `netnames`: pure rules in their own file with host tests, IDF glue in another.

Public API (`include/timesync.h`):

| Function | Contract |
| --- | --- |
| `esp_err_t timesync_init(void)` | load the settings (defaults: enabled, `auto`, empty server) and start. Called from `app_main` after the network interfaces exist |
| `bool timesync_clock_valid(void)` | true when the clock is at or after 2020-01-01. Callable from any task (not an ISR); takes no lock that the OpenThread task holds; doesn't block |
| `int64_t timesync_epoch_at(int64_t uptime_us)` | the date of a past moment given as `esp_timer_get_time()`: epoch seconds, or 0 when the clock isn't valid. Same calling rules as above |
| `void timesync_get_config(timesync_config_t *out)` | the stored settings |
| `esp_err_t timesync_set_config(const timesync_config_t *in, const char **field, const char **reason)` | validate, store, apply live. `ESP_ERR_INVALID_ARG` with the field named; a storage error leaves the running state unchanged |
| `void timesync_get_status(timesync_status_t *out)` | a snapshot for the API (4.1, 4.2 and the fields of 5.2) |

`timesync_config_t`: `enabled` (bool), `mode` (`auto` or `custom`), `server` (string, up to 63 bytes).

Server validation: empty is allowed only in `auto`. Otherwise 1 to 63 bytes: an IPv4 literal, an IPv6 literal, or a
hostname (labels of letters, digits and hyphens, separated by dots, no leading or trailing hyphen in a label).

Storage: its own NVS namespace, one key per field, as `netnames`.

Rules to keep (STYLEGUIDE, "Tasks and locks"): the sync callback runs on lwIP's thread and only records the moment;
nothing here runs on the OpenThread task or the esp_timer task.

### 5.2 Device API (`p4/components/device_api`: `time_json.c`, `time_json.h`, handlers in `device_api.c`)

`GET /device/time`:

| Field | Type | Meaning |
| --- | --- | --- |
| `enabled` | bool | setting |
| `mode` | `"auto"` or `"custom"` | setting |
| `server` | string | the custom server; may be empty in `auto` |
| `state` | string | 4.2 |
| `now` | number or `null` | board time, epoch seconds; `null` when the clock isn't valid |
| `synced_age_s` | number or `null` | seconds since the last answer; `null` when none since boot |
| `source` | `"custom"`, `"dhcp"`, `"default"` or `null` | 4.1 |
| `servers` | array of `{address, answered}` | the servers in use; `answered` true for the one that answered last |
| `dhcp_servers` | array of strings | what the lease names (up to two), whatever the mode |
| `dhcp_iface` | `"single"`, `"mgmt"`, `"thread_bb"` or `null` | the interface whose lease named them; `null` when unknown or none |

`PUT /device/time`: body `{enabled, mode, server}`, all required. Applied live. Replies the `GET` body.
Errors, following `PUT /device/names`: 400 `invalid_config` with `field` (`enabled`, `mode`, `server`, or `body`),
`length_required`, `invalid_request`; 413 `payload_too_large` (over 256 bytes); 500 `storage_failed`.

Two more URI handlers: `EXTRA_HANDLER_SLOTS` in `esp_ot_br_server/src/esp_br_web.c` grows by 2, with a
`p4/components/PATCHES.md` entry.

### 5.3 Web UI

- `frontend/ethernet.html`: the Time section after Hostname, markup as the mockup (existing classes only).
- `frontend/static/time.js`: load, render (4.3), edit, save. Dates with `toLocaleString()` as `zigbee.js`.
- `frontend/static/ethernet.js`: the "Time server" row on the interface card, from `dhcp_servers` / `dhcp_iface`.

### 5.4 Zigbee: dates for events before the first sync

`zigbee_now_epoch()` call sites record `esp_timer_get_time()` instead of an epoch. The status snapshot converts with
`timesync_epoch_at()` when it is read. `zigbee_status_t` and `/device/zigbee` keep their fields and meaning (epoch
seconds, 0 for "clock not set"), so `zigbee_json.c`, its tests and `zigbee.js` don't change. The Zigbee component's
own `EPOCH_VALID` goes away.

### 5.5 Config (`p4/sdkconfig.defaults`)

`CONFIG_LWIP_DHCP_GET_NTP_SRV=y`, `CONFIG_LWIP_DHCP_MAX_NTP_SERVERS=2`, `CONFIG_LWIP_SNTP_MAX_SERVERS=2`. No socket
count change. An existing `p4/sdkconfig` keeps old values: `docs/development.md` already tells developers to delete it.

### 5.6 Docs

`docs/networking.md` (a Time section: sources, split mode, what leaves the board and to where), `docs/api.md` (the
two endpoints), `docs/web-ui.md`, `docs/troubleshooting.md` ("clock not set"), `CHANGELOG.md` under Unreleased.

## 6. Constraints and risks for the builder

1. **Styleguide:** `STYLEGUIDE.md` applies throughout (`timesync_` prefix, `esp_err_t`, one `TAG`, host tests named by
   scenario, no lwIP calls from the OpenThread or esp_timer task). The prefix is added to its list.
2. **Changing servers live:** `esp_netif_sntp` is set up once; changing mode or server means tearing it down and
   starting it again. Do it from the HTTP task, never from lwIP's thread.
3. **Which lease named the servers:** stock lwIP doesn't say. If the builder finds a way that needs no IDF patch,
   fill `dhcp_iface` and show the interface name and the card row. If not, `dhcp_iface` is always `null`, the card row
   is dropped, and the source line reads "from your router (DHCP)". Don't patch IDF for this.
4. **Lease events:** the server list must be rebuilt on this project's own interfaces' "got IP" and "lost IP"
   events (`netcfg` creates them, VLAN ones included). Check which IP event they post.
5. **`pool.ntp.org` needs DNS.** With a static address and no DNS server set, `auto` can't resolve it; the state is
   `unreachable` and the page says so. No special case.
6. **Overlap with the logging branch:** `p4/sdkconfig.defaults`, `PATCHES.md` numbering, `EXTRA_HANDLER_SLOTS`,
   `docs/api.md`, `CHANGELOG.md`, and `p4/components/zigbee`. Whoever merges second rebases. Logging calls
   `timesync_clock_valid()`; if logging merges first it needs a stub until this lands.

## 7. BDD skeleton

```
timesync (rules)
    validate config
        On mode auto with an empty server
            - accepts
        On mode custom with a hostname, an IPv4 literal or an IPv6 literal
            - accepts
        On mode custom with an empty server
            - rejects, naming field "server"
        On a server over 63 bytes, with a space, or with a label starting or ending in a hyphen
            - rejects, naming field "server"
        On an unknown mode
            - rejects, naming field "mode"
    choose servers
        On disabled
            - returns no servers and source null
        On custom, whatever the lease names
            - returns the custom server alone, source "custom"
        On auto with a lease naming one server
            - returns that server, source "dhcp"
        On auto with a lease naming three servers
            - returns the first two, source "dhcp"
        On auto with no server in the lease
            - returns pool.ntp.org, source "default"
    derive state
        On disabled
            - returns "off"
        On enabled, no answer, under a minute since start
            - returns "waiting"
        On enabled, no answer, a minute or more since start
            - returns "unreachable"
        On an answer and a reachable server
            - returns "synced"
        On an earlier answer and every server now unreachable
            - returns "unreachable"
    epoch at
        On a valid clock and a moment 90 s ago
            - returns now minus 90
        On a valid clock and a moment before the first sync
            - returns the real date of that moment
        On a clock that is not valid
            - returns 0
    clock valid
        On a clock before 2020-01-01
            - returns false
        On a clock at or after 2020-01-01
            - returns true

timesync
    timesync_init
        On no stored settings
            - starts enabled in auto
        On stored settings
            - starts with them
        On stored disabled
            - starts no NTP client
    timesync_set_config
        On a valid change
            - stores it
            - restarts the sync with the new servers
            - resets the state to "waiting"
        On switching to disabled
            - stops the NTP client
            - leaves the clock as it is
        On an invalid config
            - returns ESP_ERR_INVALID_ARG with the field
            - changes nothing
        On a storage failure
            - returns the error
            - keeps the running servers
    on a lease change
        In auto, on a lease that names servers
            - switches to them, source "dhcp"
        In auto, on losing the lease's servers
            - switches to pool.ntp.org, source "default"
        In custom
            - keeps the custom server
            - still reports dhcp_servers
    on an answer
        Always
            - sets the clock
            - records the moment and the server that answered

device_api
    GET /device/time
        On synced from DHCP
            - writes every field of 5.2, now and synced_age_s as numbers
        On a clock not set
            - writes now null and synced_age_s null
        On disabled
            - writes state "off", source null, servers empty
    PUT /device/time
        On a valid body
            - applies it and replies the GET body
        On a missing or wrongly typed key
            - 400 invalid_config with that field
        On no JSON object
            - 400 invalid_config with field "body"
        On a body over 256 bytes
            - 413 payload_too_large
        On a storage failure
            - 500 storage_failed

zigbee (status)
    snapshot
        On a client that connected before the first sync, read after it
            - client_since is the real date
        On the stack started at boot, read after the first sync
            - started is the real date
        On a clock that is not valid
            - client_since, last_client_left and started are 0

time.js (page)
    render
        On each chip condition of 4.3
            - shows that chip
        On synced_age_s null with a valid clock
            - shows "Before the last restart"
        On dhcp_servers present under Custom
            - shows the hint naming them
    save
        On Custom with an empty field
            - shows an error under the field and sends nothing
        On a 400 with a field
            - shows the error under that field
```

## 8. Verification

- Host tests for the rules, the JSON and the Zigbee snapshot (`make test`), `make lint`, a full build.
- One on-board pass after review and fixes, never inside a task (project rule):
  1. power-up on the home network: clock set within seconds; source matches what the router hands out;
  2. Zigbee `started` shows a real date although it started before the sync;
  3. soft restart: the clock stays valid, "Before the last restart" until the next answer;
  4. custom server that doesn't exist: "Server not answering", clock keeps running;
  5. off: no NTP packets on the wire (capture);
  6. split mode: which lease's servers win, and whether `dhcp_iface` is right.

## 9. Left to the builder

File split inside `timesync`, NVS key names, the exact reachability test behind `unreachable`, hint and error
wording beyond the mockup, and how the page orders its requests.
