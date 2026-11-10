# HTTP API

Reference for scripts and integrations. For using the board, start with the [Web UI tour](web-ui.md).


Everything is on port 80, no authentication. Errors on `/device/*` are JSON `{"error": "<code>", "detail": "<text>"}`. Besides the errors listed
below, any of them can answer 500 `internal` or `no_memory`.

## Device API

| Method and path | Purpose | Notable errors |
| --- | --- | --- |
| `GET /device/status` | mode, app version and slot, `zigbee_channel` (id of the channel in Zigbee mode, `null` when none), `rcp`, `jumper`, `eth` addresses, `pd`, `routing`, `sniffer` stats, `fault_reason`, `storage_ok`, `trel` (`enabled`, `active`, `peers` (this Thread network), `peers_other` (other networks), `tx_packets`, `tx_failures`, `rx_packets`, `peer_list`: at most 8, this network's first, each `ext_address`, `ext_pan_id`, `same_network`, `preference` (`{"ieee802154": 0..255\|null, "trel": 0..255\|null}`, OpenThread's per-radio preference; `null` when the peer is not a Thread neighbour), `address`; `trel` is `null` in a build without TREL; `active`, both counts and the counters `null` when OpenThread's state could not be read) | |
| `GET /device/sniffer.pcap?channel=&duration=` | stream a pcap capture | 400 `invalid_channel`, `invalid_duration`, `no_channel`; 409 `busy` (not `BR_RUNNING`); 503 `busy` (no channel given and OpenThread's state could not be read within 100 ms: retry); 500 `sniffer_failed` |
| `DELETE /device/sniffer` | stop the running capture | 404 `not_found` |
| `GET /device/update[?check=true\|false]` | the cached state of the update check against the latest GitHub release; never waits for the network. `check=true` starts a check on a worker (the reply shows `state: "checking"`; a running check is joined) and the caller polls the plain `GET`. Fields: `state` (`never`, `checking`, `done`, `failed`), `checked_age_s` (seconds since the last finished check, `null` before the first), `error` (detail of the last failed check, else `null`), `release` (`null` until a check succeeded, then kept: `{"tag": <string>\|null, "app", "rcp": <string>\|null, "url": <release page>}`; `tag` and `rcp` are `null` when `versions.txt` has no such line), `repo_url`, `running` (`{"app", "rcp"}`; `rcp` `null` when unknown), `app` (`newer`, `same`, `older`, `unknown`; `null` without a release), `rcp_differs`, `available` (`app` is `newer` or `unknown`, or `rcp_differs`). See [Updates](updates.md) | 400 `invalid_request` (`check` is not `true` or `false`); 409 `busy` (`check=true` while OTA, a radio push or ROM recovery runs) |
| `POST /device/update[?force=1\|0]` | check again (the cached result is not used), then install the latest release: the bundle, then the radio firmware when it differs, then one reboot. The body is empty and the request is held until the install ends. 200 `{"status":"ok","app":"installed"\|"unchanged","rcp":"installed"\|"unchanged"\|"failed","detail":...,"rebooting":bool}`; `rcp` `failed` means the new app is committed and the board reboots, and the next update pushes only the radio firmware. `force=1` installs although nothing is newer | 400 `invalid_request` (a body, or `force` not `0` or `1`), `invalid_bundle`; 409 `up_to_date` (nothing to install, no `force`), `busy` (another long operation or a check runs; the radio firmware is to be installed and the board is sniffing or starting); 502 `check_failed` (`versions.txt` could not be fetched or read; nothing changed), `transfer_failed` (bundle download; nothing changed), radio-only update: the errors of `POST /device/radio/firmware?from_url=` |
| `POST /device/ota` | upload an `otbr.bundle`, then reboot | 400 `length_required`, `invalid_bundle`; 413 `payload_too_large`; 409 `busy` |
| `POST /device/ota/url` | body `{"url": "http(s)://..."}`, same via download (https is checked against the built-in certificate bundle; only 301, 302 and 307 redirects are followed) | 400 `invalid_request`, `invalid_bundle`; 409 `busy`; 502 `transfer_failed` |
| `POST /device/radio/firmware[?from_url=]` | (`from_url` is an `http://` or `https://` address; redirects are followed) push `c6_rcp.bin` to the C6, then wait (up to 180 s) until the mode on the C6 runs again (Thread, Zigbee or the RCP proxy); on a factory C6 (`fault_reason` `esp-hosted firmware`) only a `c6_installer.bin` with this firmware's C6 base version, sent through ESP-Hosted, then `{"installed":null}` and `no rcp firmware` | 400 `length_required`, `invalid_image` (also `c6_installer.bin` to our updater, anything else to ESP-Hosted, an installer with another base version), `image_too_large`; 413 `payload_too_large`; 409 `busy` (sniffing, starting, OpenThread being moved, update running); 502 `fetch_failed`, `push_failed`, `install_failed` (ESP-Hosted transfer of `c6_installer.bin`), `restart_failed` (the image is installed, but the mode on the C6 did not start on it, whatever the mode; the detail names it and why: "the image is installed, but OpenThread did not start on it: <reason>", or "...: still starting after 180 s") |
| `POST /device/radio/recover` | maintenance jumper: rewrite the C6 base image (no RCP firmware) and restart | 200 `{"recovered":true,"rcp":"<installed version>"\|"no rcp firmware"\|"unknown","mode":...}` (also when the C6 has no RCP firmware afterwards); 412 `no_jumper`; 409 `busy`; 502 `recovery_failed` (only when the recovery itself failed) |
| `GET /device/radio/log` | jumper: the newest 16 KB of the C6 console (`text/plain`) | 412 `no_jumper` |
| `GET /device/diag` | per-core load, per-task CPU/stack/affinity, memory, OpenThread lock stats, uptime, reset reason, chip temperature (`chip_temperature_c`, die temperature in °C with one decimal, `null` when unavailable) | |
| `GET /device/network` | active config (with `trel`), pending trial with remaining time, interface state | |
| `PUT /device/network` | store a trial network config and reboot into it; a body that changes only `trel` is applied at once without a trial (`{"status":"applied"}`; `{"status":"saved","detail":...}` when Thread is not running or busy and it applies at the next start; see [TREL](networking.md#trel-thread-over-ethernet)) | 400 `invalid_config` (`detail` names the field, `trel` when not a boolean), `length_required`, `invalid_request`; 409 `busy` (also a `trel`-only change while a trial runs); 413 `payload_too_large`; 503 `not_started`; 500 `store_failed` |
| `POST /device/network/confirm` | commit the trial config | 409 `no_trial`; 403 `wrong_interface`; 500 `store_failed` |
| `GET /device/names` | network names: `hostname`, `hostname_default`, `hostname_live` (differs after an mDNS conflict rename), `ha_name_follows_hostname`, `ha_name`, `ha_name_effective`, `http_advert`, `http_advert_available` (false in split mode), `dhcp_name`, `dhcp_name_default`, `mode` | |
| `PUT /device/names` | body `{hostname, ha_name_follows_hostname, ha_name, http_advert, dhcp_name}` (all required; empty hostname / DHCP name = default); applied live, no reboot; replies the GET body plus `applied` and, when a live step failed, `detail` | 400 `invalid_config` with `field` (`hostname`, `ha_name`, `dhcp_name`, or a missing / wrongly typed key, `body` for no JSON object), `length_required`, `invalid_request`; 413 `payload_too_large`; 500 `storage_failed` |
| `GET /device/time` | clock and NTP sync: `enabled`, `mode` (`auto` \| `custom`), `server` (the custom server; may be empty in `auto`), `state` (`off`, `waiting`: no answer yet and under a minute since the start or the last save; `synced`; `unreachable`: no answer for a minute, or every server in use stopped answering), `now` (board time, epoch seconds; `null` when the clock is not set, before 2020), `synced_age_s` (seconds since the last answer; `null` when none since boot), `source` (`custom`, `dhcp`, `default` = `pool.ntp.org`, `null` when off), `servers` (`[{address, answered}]`, the servers in use, `answered` on the one that answered last), `dhcp_servers` (what the DHCP lease names, up to two, whatever the mode), `dhcp_iface` (`single`, `mgmt`, `thread_bb` or `null` when not known) | |
| `PUT /device/time` | body `{enabled, mode, server}` (all required; `server` at most 63 bytes, a hostname, IPv4 or IPv6 address; required in `custom`; in `auto` an empty server keeps the stored custom one); applied live, no reboot; replies the GET body | 400 `invalid_config` with `field` (`enabled`, `mode`, `server`, `body` for no JSON object), `length_required`, `invalid_request`; 413 `payload_too_large` (over 256 bytes); 500 `storage_failed` |
| `GET /device/thread` | leader weight: `leader_weight_adjustment` (stored, -16..16), `leader_weight` (this board's local weight) and `leader_weight_base` (weight minus adjustment); only the adjustment while OpenThread is not running, plus `partial: true` when its lock was not free in time | |
| `PUT /device/thread` | body `{leader_weight_adjustment?, become_leader?}` (at least one; `8.0` counts as 8; `become_leader: false` does nothing). Order: validate the whole body, store and apply the adjustment, then take over as leader (only from a strictly lower weight; never automatic). Replies the GET body plus `applied` (and `detail` when false: `Thread is not running` or `OpenThread is busy, applied within seconds or at the next start`) and `"become_leader":"started"` (`"partial": true` can follow a started take-over when the weight read times out) | 400 `invalid_config` with `field` (`leader_weight_adjustment`, `become_leader`, `body`), `length_required`, `invalid_request`; 409 `cannot_take_over` with `detail` `weight not higher` / `already leader` / `not attached` / `not router eligible` (an adjustment sent in the same body stays stored); 409 `busy` (`mode <MODE>`); 413 `payload_too_large`; 500 `storage_failed`, `internal` (`taking over failed`); 503 `busy` (`OpenThread is busy, retry` or `OpenThread is restarting, retry`). When the body also had an adjustment, a 409/503/500 from the take-over step also carries `leader_weight_adjustment` (the stored value) and `applied` |
| `POST /device/topology` | start a mesh scan (or join the running one); 202 with the state object | 409 `busy` (`mode <MODE>`, an update or radio job running, `a capture stream is already running` while a capture stream holds the pcap slot, `mesh diagnostics are in use`); 503 `busy` (`OpenThread is busy, retry`, or `topology scan unavailable` when the scan component failed to start at boot); 500 `no_memory` |
| `GET /device/topology` | the scan state, progress and the last result (no mesh traffic); in a build with TREL, a link of this board's router carries `transport` when OpenThread knows that neighbour's radios: `{"ieee802154": 0..255\|null, "trel": 0..255\|null}` (OpenThread's preference per radio, `null` for a radio it does not use); no `transport` on this board's link means its radios are unknown, and other routers' links never have it; the result carries service records from every SRP server, see [Service records](#service-records-in-the-topology-scan) | |
| `GET /device/routing` | routing settings and status, see [Routing](#routing) | 503 `not_started` |
| `PUT /device/routing` | store the routing settings and apply them live (no reboot) | 400 `invalid_request` (`detail` names the field), `length_required`; 413 `payload_too_large` (over 2048 bytes); 500 `storage_failed` (nothing changed), `internal` (the body could not be checked, `detail` is the error; nothing changed), `no_memory` (the settings were stored and applied, the answer could not be built); 503 `not_started` |
| `GET /device/console` | Thread console setting and session: `{"enabled": bool, "session": null \| {"peer": "<address>", "since_s": <s>}}`, see [Thread console](#thread-console) | |
| `PUT /device/console` | body `{"enabled": bool}`; stored, off by default; turning it off closes the session (4003); replies like GET | 400 `invalid_request` (`enabled` missing or not a boolean), `length_required`; 413 `payload_too_large`; 500 `storage_failed`; 503 `not_started` (the console did not start at boot) |
| `GET /device/console/ws` | WebSocket: the board's OpenThread CLI, see [Thread console](#thread-console) | 403 on an interface that does not serve the device API |
| `GET /device/logging` | log settings and status: `max_level` (the highest level this build compiles in: `info` in a release build, `verbose` in a test build), `channels` (`uart`, `jtag` with `host`, `syslog` with `target` and `state`), `components` (`"*"` is the default level), `known_tags`, `viewers`, `counters` (`sent` and `dropped` for `uart`, `jtag`, `syslog`, `live`), see [Logging](logging.md) | 503 `not_started` |
| `PUT /device/logging` | any subset of `channels.<name>.level`, `channels.syslog.target` and `components`; applied at once and stored; `components` replaces the override set (`"*"` sets the default); a level above `max_level`, or `"*"` above `debug`, is refused; unknown keys are ignored, so a client may PUT back what GET gave it; replies like GET | 400 `invalid_request` (`detail` names the field, e.g. `channels.syslog.target`, `components.<tag>`; a tag listed twice is refused), `length_required`; 413 `payload_too_large` (over 4096 bytes); 500 `storage_failed` (the levels are in force but were not stored), `no_memory`; 503 `not_started` |
| `GET /device/logging/ws` | WebSocket: the live log, see [Live log](#live-log) | 400 `invalid_request` (`level` in the URL is not a level or is above `max_level`), 403 on an interface that does not serve the device API, 503 `too_many_viewers` (two viewers are open) |
| `GET /device/channels` | every radio channel and the modes the board offers, see [Radio channels](#radio-channels) | |
| `PUT /device/channels/{id}` | any of `mode`, `baud`, `flow_control` and an `rcp_proxy` object; replies with the channel (`"reboot": true` when the mode's consumer reboots), or 202 `{"moving": true, "moved_from": "<id>"}` when Thread moves to this channel (also for a start, `moved_from` null, and a stop of Thread, `moved_from` the channel itself); `otbr` on the channel that runs Zigbee is no 202 but the channel with `"reboot": true` (Thread moves or starts there through the reboot) | 400 `bad_value` (`detail` names the field), `length_required`; 404 `unknown_channel`; 409 `unavailable`, `not_available`, `incompatible`, `mode_taken`, `probe_failed`, `port_taken`, `busy`; 413 `payload_too_large` (over 512 bytes); 500 `failed`; 503 `mux_busy` (the radio mux took the change but did not answer in time: a mode, speed or proxy change may still be applied, a start, move or stop will not) |
| `DELETE /device/channels/{id}` | forget an unplugged radio and its settings; 204 | 404 `unknown_channel`; 409 `in_use` (the C6 or a plugged-in radio); 500 `failed`; 503 `not_started`, `mux_busy` (taken but not answered in time: the radio may still be forgotten) |
| `GET /device/zigbee` | Zigbee mode status, see [Zigbee coordinator](#zigbee-coordinator-experimental) | 409 `not_zigbee` (no channel runs `ziggurat`) |
| `POST /device/reboot` | reboot | 409 `busy` while an update or push runs (other reboots, such as the network trial revert, `net reset` and `DELETE /node`, wait until it ends) |
| `GET /metrics` | device health and Thread figures in the Prometheus text format, see [metrics](metrics.md) | 500 `no_memory` |

`/device/status` modes: `STARTING`, `BR_RUNNING`, `SNIFFER`, `RADIO_FAULT`, `NO_RADIO` (no present radio channel is in `otbr`: Thread is switched off on purpose or its dongle is unplugged; the Thread REST API and the console WebSocket answer 503 `no radio in OTBR mode`). `otbr_channel` is the id of the channel stored in mode `otbr` (present or not), or `null` when none is: it is not proof that Thread runs there, which only `mode` `BR_RUNNING` or `SNIFFER` says (in `NO_RADIO` it names the unplugged dongle, in `RADIO_FAULT` the faulted radio). The Dashboard and the Radios page show "Thread runs on" only then, otherwise a warning with the state. `POST /device/radio/firmware` and `POST /device/radio/recover` also answer 409 `c6_busy` while `rcp-proxy` on the C6 has a peer.

### Service records in the topology scan

The scan result lists the hosts and services registered with the network's SRP servers (`srp_hosts`) and says which
servers were asked (`srp_servers`). The SRP step runs next to the router and child phases from the start of the scan,
with its own 15 s budget inside the 60 s cap.

- Where records come from: this board's SRP server is read locally (with lease and key lease). Every other DNS/SRP
  server in the network data is asked over DNS-SD on port 53 for `_matter._tcp`, `_matterc._udp`, `_esphomelib._tcp`,
  `_hap._udp`, `_http._tcp`, `_meshcop._udp` and `_trel._udp`. An instance the browse answer did not describe (SRV,
  TXT and an address) is resolved for port, host address and TXT (with a follow-up AAAA query when the SRV answer has
  no address), unless the browse answer gave addresses and none passes the filter below; ESPHome and HomeKit resolves
  go first. At most 4 queries are in flight, 2 per server while several are asked; a server whose query times out
  before it answered anything is not asked again. A record whose TXT cannot be parsed is kept without TXT.
- Filter: a host is kept when it has an address under an OMR prefix or the mesh-local prefix. A border router record
  (`_meshcop._udp`, `_trel._udp`) is kept when its extended PAN ID (`xp`) is this network's and it has an extended
  address (`xa`) other than this board's.
- De-duplication: by host name; a host seen on several servers is listed once.
- Limits: 64 hosts, 8 services per host, 512 TXT bytes per service (`txt_truncated` is set when cut), 6 subtypes per
  service, 48 resolves per scan (16 of them kept for `_esphomelib._tcp` and `_hap._udp`), 15 s.
- A server that does not answer never sets `partial`. `srp_servers[].state` is `answered` (at least one query was
  answered, an empty answer counts, and all its queries ended), `no_answer` (no query it was asked got an answer:
  they all timed out or failed, or it was given up after a timeout), or `cut_short` (the budget or the cap ended the
  step while it still had queries in flight or waiting and had not been given up, or it answered only after its
  waiting queries were dropped).

```json
"srp_servers": [
  {"rloc16": 26624, "address": "fd8f:...:a75c", "port": 53536, "this_board": true, "state": "answered", "hosts": 1},
  {"rloc16": 4096, "address": "fd8f:...:bff7", "port": 53554, "this_board": false, "state": "answered", "hosts": 2}
],
"srp_hosts": [{
  "name": "c6-thread-test", "addresses": ["fd87:9b49:8264:1:c158:979b:6560:a15f"],
  "source": 26624, "lease_s": 7012, "key_lease_s": 1209412,
  "services": [{
    "instance": "c6-thread-test", "type": "_esphomelib._tcp", "port": 6053, "source": 26624,
    "subtypes": [], "txt": "<base64 of the TXT RDATA>", "txt_truncated": false
  }]
}]
```

- `srp_servers[]`: `rloc16` (number) of the server router, its `address`, the SRP `port` from the network data (the
  DNS query goes to port 53), `this_board`, `state`, and `hosts`, the hosts kept from it (after the address filter,
  before de-duplication).
- `srp_hosts[].source` and `services[].source`: the `rloc16` of the server the record came from. `lease_s` and
  `key_lease_s` only for hosts from this board's server; hosts from other servers have `ttl_s` instead.
- `services[].port` is the SRV port. `txt` is the base64 of the raw TXT RDATA, `""` when the record has none.
  `subtypes` is always empty for hosts from other servers.

### Device info in the topology scan

After the router and child tables, the scan asks every router and every child (except this board, which is read
locally) for its Network Diagnostic vendor TLVs. Each router and each child in the result carries an `info` object.
While the scan runs, `progress.info` counts the answers.

- `info.state`: `answered` (the device replied; the fields below follow), `no_answer` (asked, but the deadline passed
  or the send failed), `cut_short` (still queued when the cap hit), `not_asked` (never asked: a router that did not
  answer discover, or any device when the 60 s cap hit before the info step). This board's router entry is always
  `answered`, with its local values.
- Fields, present only after an answer and only when not empty:

| Field | Format |
| --- | --- |
| `eui64` | factory EUI-64, 16 hex digits |
| `vendor_name` | string, up to 32 bytes |
| `vendor_model` | string, up to 32 bytes |
| `vendor_sw_version` | string, up to 16 bytes |
| `thread_stack_version` | string, up to 64 bytes, e.g. `openthread-esp32/fff9895c-b678a4f; esp32c6;  2026-11-03 21:47:25` |
| `vendor_app_url` | string, up to 96 bytes |
| `vendor_oui` | 24-bit OUI, 6 hex digits |

- Strings are JSON-escaped; bytes that are not valid UTF-8 become U+FFFD. A device may answer with none of the asked
  TLVs: it is `answered` and has no fields.
- `progress.info` is `{total, answered, sleepy_waiting, wait_s, awake_left, done}`: devices to ask, devices that
  replied (`answered` counts replies only, not `no_answer`), sleepy devices still waiting, the seconds until the
  latest sleepy deadline (0 when none waits), awake devices (routers and rx-on children) queued or in flight, and
  devices answered or given up (`done` counts `no_answer` too). Once `awake_left` is 0 only sleepy devices are left.
- A sleepy child answers only after its next poll. The scan waits for sleepy children up to 300 s from the scan start,
  each for the child's own timeout from the child table (240 s when unknown), and asks again every 60 s while it
  waits (OpenThread drops an unanswered request after 62-93 s, so one request is always pending). Every other step stays bounded by the 60 s cap. At
  the sleepy cap the scan finishes with what it has. Missing info never sets `partial`.
- During the info step the scan holds no mesh diagnostics operation, so the REST `/topology` and diagnostics endpoints
  work; `POST /device/topology` joins the running scan. Both answers reach the scan, which ignores one that can't be
  its own: with a TLV the scan does not ask for, or with addresses it did not ask for (or none when it did). A REST
  Get for a subset of the scan's TLVs at the same moment can still be taken as its answer; it describes that device
  correctly.
- When this board is a child, its own child entry is read locally, like its router entry.

```json
"info": {"state": "answered", "eui64": "8c73dafffe952cfb", "vendor_name": "Home Assistant",
         "vendor_model": "OpenThread Border Router",
         "thread_stack_version": "OPENTHREAD/cb18e0f; POSIX; Oct  6 2026 20:28:48"}
```

### Routing

How the core router learns the route into the mesh (see [Routing to the mesh](routing.md)).
`GET /device/routing` returns this shape; `PUT /device/routing` takes the same body, without `status` (ignored if sent),
and answers 200 with the new GET shape:

```
{
  "method": "bgp",
  "omr": {"mode": "custom", "prefix": "2001:db8:42:100::/64"},
  "announce_ula": true,
  "bgp": {
    "local_asn": 4200000001,
    "router_id": null,
    "hold_time": 90,
    "peers": [ {"address": "2001:db8:42::1", "asn": 4200000001} ]
  },
  "status": {
    "omr_prefix": "2001:db8:42:100::/64",
    "own_omr_prefix": "2001:db8:42:100::/64",
    "ula_prefix": "fd98:2656:4740:1::/64",
    "announced": ["2001:db8:42:100::/64", "fd98:2656:4740:1::/64"],
    "bgp": {
      "router_id": "192.168.20.15",
      "blocked": null,
      "peers": [ {
        "address": "2001:db8:42::1",
        "state": "established",
        "uptime_s": 8123,
        "last_error": null,
        "received_dropped": 3
      } ]
    },
    "ripng": null
  }
}
```

Settings (defaults: `pd`, `auto`, `false`, local ASN 4200000001, router ID `null`, hold time 90, no peers):

- `method`: `pd`, `ripng`, `bgp` or `off`. Required.
- `omr`: required. `mode` `auto` or `custom`; `prefix` is required for `custom` and ignored for `auto` (GET shows
  `null`). The prefix is a /64 with the host bits zero, not `::/64`, not link-local (`fe80::/10`), not multicast, and
  not on-link on the backbone.
- `announce_ula`: `true` or `false`. Required.
- `bgp`: optional; left out, the stored BGP settings stay (their peers still get the address checks below, so a peer
  that has become one of the board's own addresses is refused with `bgp.peers`). When present it is checked whatever the method, so the
  stored BGP settings are always valid, and it must be complete (`null` is refused):
  - `local_asn` and each peer's `asn`: 1..4294967295, not 23456 (AS_TRANS);
  - `router_id`: `null` (use the backbone IPv4 address) or a dotted IPv4 address other than `0.0.0.0`; the backbone
    address wins when there is one;
  - `hold_time`: 0 or 3..65535;
  - `peers`: 0..2 entries with distinct addresses, not multicast, not `::`, not one of the board's own addresses. A
    link-local address is on the backbone interface.
- Method `bgp` needs at least one peer.

A body that fails a check answers 400 `{"error": "invalid_request", "detail": "<field>"}` and changes nothing. The
field is one of `body` (not a JSON object), `method`, `omr`, `omr.mode`, `omr.prefix`, `announce_ula`, `bgp`,
`bgp.local_asn`, `bgp.router_id`, `bgp.hold_time`, `bgp.peers`.

Status:

- `omr_prefix`: the OMR prefix the mesh uses, which OpenThread picks among all border routers (it can be another
  border router's); `own_omr_prefix`: this board's OMR prefix (custom, delegated or ULA), withdrawn while another one
  is preferred; `ula_prefix`: the self-generated ULA. Each is `null` while unknown.
- `announced`: what RIPng or BGP announces now; empty with `pd` and `off` and while the border router is not running.
- `bgp`: `null` unless the method is `bgp`. `router_id` is the one in use (`null`: none). `blocked` is `null` or why
  sessions do not run: `"no router id"`, `"no link-local address"`. One entry in `peers` per configured peer: `state`
  is `idle`, `connect`, `active`, `opensent`, `openconfirm` or `established`; `uptime_s` counts from Established (0
  when down); `received_dropped` counts the routes received and dropped; `last_error` is `null` or, for example,
  `"notification 6/2 received"`, `"notification 4/0 sent"`, `"connect: refused"`, `"connect: host unreachable"`,
  `"hold timer expired"`, `"connection lost"`.
- `ripng`: `null` unless the method is `ripng`, else `{"last_update_s_ago", "updates_sent", "requests_answered",
  "responses_dropped"}`. `last_update_s_ago` is `null` before the first update.

`/device/status` carries a summary, `null` while routing is not running:

```
"routing": {"method": "bgp", "omr_mode": "custom", "announced_count": 2, "peers_established": 1, "peers_configured": 1}
```

`peers_established` and `peers_configured` are `null` unless the method is `bgp`. The `pd` object stays as it is; with
any method but `pd` its `state` is `disabled`.

### Thread console

The board's own OpenThread CLI over a WebSocket, for debugging a mesh without a second Thread device. The board stays a
border router while it is used. It is off by default: turn it on with `PUT /device/console` or on the
**Management → Console** page. One session at a time; a new one takes over.

- Send one command per text frame (a leading `ot ` is dropped, the line may be up to 383 bytes). The CLI's output comes
  back as text frames, ending with `Done` or `Error <n>: <text>`. Async commands such as `ping` and `dns browse` print
  their result later, before their `Done`.
- Board notices are separate frames starting with `# `: `# Connected to <host> (OpenThread <version>, <role> <0xRLOC16>)`,
  `# Thread is not running (mode <MODE>)` (lines run only in `BR_RUNNING`), `# Busy: "<line>" is still running` (only
  `factoryreset` gets through while a command runs), `# Line too long (max <n> bytes)`, `# Output dropped (<n> bytes)`
  (more than 8 KB waiting), `# Closed: a new console session opened from <address>`,
  `# Closed: the console was turned off`.
- Close codes: 4001 taken over, 4003 the console is off (also right after the upgrade), 4009 output not read for 15 s,
  1001 `no answer` (the board pings every 10 s; 30 s without a pong), 1007 a line with a NUL byte, 1009 a frame over
  1 KB.
- Every command is logged at INFO by its first word and length only, as the rest can carry keys
  (`console <peer>: dataset (52 bytes)`). The page asks before `factoryreset`, `reset`, `thread stop`, `ifconfig down`
  and `dataset` changes; a raw WebSocket client does not.

```
websocat ws://<board>/device/console/ws
```

### Logging

Concepts, channels and levels are in [Logging](logging.md), the internals in
[Internals](reference/internals.md#logging). `GET /device/logging` answers

```json
{
  "max_level": "verbose",
  "channels": {
    "uart": {"level": "info"},
    "jtag": {"level": "info", "host": false},
    "syslog": {"level": "off", "target": "", "state": "off"}
  },
  "components": {"*": "info", "ziggurat": "debug"},
  "known_tags": ["esp_netif", "OPENTHREAD", "ziggurat"],
  "viewers": 0,
  "counters": {"uart": {"sent": 0, "dropped": 0}, "jtag": {"sent": 0, "dropped": 0},
               "syslog": {"sent": 0, "dropped": 0}, "live": {"sent": 0, "dropped": 0}}
}
```

Levels are `off`, `error`, `warning`, `info`, `debug`, `verbose`. `syslog.state` is `off`, `resolving`, `sending`,
`unresolved` (the name does not resolve) or `no_socket` (resolved, but no UDP socket was free; tried again like a
failed lookup). `jtag.host` is whether a USB host is connected; while none is, JTAG lines are neither sent nor counted.
`target` is `host[:port]` (port 514 when omitted; `[fd00::1]:514` for IPv6). `components` holds one level per tag plus
`"*"`; a tag is 1 to 23 bytes of printable ASCII without spaces, quotes or backslashes (else 400
`components.<tag>`). A `PUT` with `components` replaces the whole override set, so leaving a tag out removes its
override. The syslog level cannot be raised above `off` while the target is empty (400 `channels.syslog.target`).

### Live log

`GET /device/logging/ws` sends the log lines logged after the connection, one per text frame, as
`<L> (<uptime ms>) <tag>: <text>` with `L` one of `E W I D V`. Connect with `?level=<level>` (default `info`); a text
frame `{"level":"<level>"}` changes the level of this viewer only (anything else is answered with a `# ignored` notice).
Board notices start with `# `: `# dropped <n> lines` (this viewer lost lines; shown whatever its level). At most two
viewers: a third is refused with 503 before the upgrade. The board pings every 10 s and closes with 1001 `no answer` a
client silent for 30 s. Frames are valid UTF-8: ill-formed bytes in a line are replaced by U+FFFD. Viewers (and syslog)
get no `debug` or `verbose` lines of the network-path tags (`httpd*`, `esp.emac*`, `esp_eth*`, `esp_netif*`, `lwip*`, `vfs_calls*`).

```
websocat 'ws://<board>/device/logging/ws?level=debug'
```

### Radio channels

Every radio is a channel: the ESP32-C6 (built-in) (`builtin-c6`, kind `builtin`; its id was `c6` before, which is no
longer accepted) and a USB dongle (`usb-<vid>-<pid>`, plus `-<serial>` when the device
reports one). Concepts, modes and behaviour are in [Radios](radios.md). `GET /device/channels` answers

```
{"available_modes": ["off", "otbr", "rcp-proxy", "ziggurat"], "usbip": false,
 "channels": [
  {"id": "builtin-c6", "kind": "builtin", "label": "ESP32-C6 (built-in)", "mode": "otbr", "stored_mode": "otbr",
   "state": "running", "error": null, "line": {"baud": 460800, "flow_control": false}, "usb": null,
   "caps": {"firmware_update": true, "hw_reset": true, "flow_control": false},
   "rcp_proxy": {"direction": "listen", "adapter": "tcp", "port": 6638, "host": "", "framing": "hdlc",
                 "state": null, "peer": null, "last_error": null},
   "firmware": {"type": "spinel", "version": "1.4.0", "chip": "esp32c6", "baud": null, "source": "passive", "at": 1762430000},
   "counters": {"frames_in": 10, "frames_out": 9, "bytes_in": 1000, "bytes_out": 900, "drops": 1, "resets": 2, "rx_errors": 3,
               "uart_errors": 0},
   "channel_state": "ready", "channel_reason": null, "mode_state": "running", "mode_reason": null},
  {"id": "usb-10c4-ea60-abc123", "kind": "usb", "label": "Sonoff Zigbee 3.0 USB Dongle Plus", "mode": "rcp-proxy",
   "stored_mode": "rcp-proxy", "state": "running", "error": null, "line": {"baud": 115200, "flow_control": true},
   "usb": {"vid": "10c4", "pid": "ea60", "serial": "abc123", "driver": "cp210x"},
   "caps": {"firmware_update": false, "hw_reset": false, "flow_control": true},
   "rcp_proxy": {"direction": "connect", "adapter": "udp", "port": 9999, "host": "host.local", "framing": "raw",
                 "state": "connected", "peer": "192.168.1.20:9999", "last_error": null},
   "firmware": {"type": "znp", "version": "20240710", "chip": null, "baud": 115200, "source": "probe", "at": null},
   "counters": {"frames_in": 0, "frames_out": 0, "bytes_in": 0, "bytes_out": 0, "drops": 0, "resets": 0, "rx_errors": 0,
               "uart_errors": 0},
   "channel_state": "ready", "channel_reason": null, "mode_state": "connected", "mode_reason": null}
 ],
 "modes": [
  {"mode": "otbr", "channel": "builtin-c6", "state": "running", "reason": null, "reboot_on_change": false},
  {"mode": "rcp-proxy", "channel": "usb-10c4-ea60-abc123", "state": "connected", "reason": null,
   "reboot_on_change": false}
 ]}
```

- `usbip` is `true` while the [USB/IP server](usbip.md) is on: the USB channels are then `unavailable` with
  `channel_reason` "USB is exported over USB/IP".
- `available_modes` are the modes the build offers (those with a registered instance: `off`, `otbr`, `rcp-proxy` and
  `ziggurat`).
- `mode` is what the channel does now: `unavailable` (reported only: not detected, unplugged, no driver), `off`,
  `otbr`, `ziggurat` or `rcp-proxy`. `stored_mode` is the mode it returns to, also while unavailable or when
  the firmware does not fit.
- `state` is `idle`, `starting`, `running`, `fault` (with `error`) or `busy` (the C6's maintenance: its boot steps,
  `error` "the C6's boot steps run", or a firmware push or ROM recovery, "a C6 job holds the radio"). A failed start of
  OpenThread shows as `fault` on its channel, with the board's fault reason. `state` and `error` are derived from the
  four fields below, as before them.
- `channel_state` is the channel's own lifecycle: `absent` (an unplugged dongle), `reinit` (maintenance runs on it;
  `channel_reason` names it: `boot steps`, `detecting firmware`, `firmware push`, `ROM recovery`), `ready`, or `fault`
  (`channel_reason` says why). `mode_state` and `mode_reason` are the state of the mode bound to the channel in that
  mode's own words (Thread: `no_radio`, `starting`, `running`, `sniffer`, `releasing`, `fault`; Zigbee: `idle`,
  `starting`, `running`, `no_radio`, `incompatible`; the proxy: `off`, `listening`, `connecting`, `backoff`,
  `connected`, `no_radio`), `null` for a channel in `off` or before its mode reported one. At the end of a maintenance
  the orchestrator shows the bound mode as `starting`, whatever the mode, until it reports again. A reason is `null`
  when there is none.
- `modes` lists the mode instances: Thread and Zigbee once each, the proxy once per channel in `rcp-proxy`. `channel`
  is the channel stored in that mode (`null` for none: Thread is `no_radio` "no channel in otbr", Zigbee `idle`);
  `state` and `reason` are that channel's `mode_state` and `mode_reason`; `reboot_on_change` says whether entering or
  leaving the mode reboots the board.
- `usb` is `null` for the C6. `rcp_proxy` always carries the stored settings; `state` (`listening`, `connecting`,
  `backoff`, `connected`) and `peer` are set only while the channel runs `rcp-proxy`. `last_error` says why the
  proxy's last try to listen or connect failed (`"tcp port 6638: Address already in use"`, `"host.local does not
  resolve"`, `"connect timed out"`) and is `null` once a try worked; while the proxy is in `backoff` it is also the
  channel's `error`.
- `firmware` is `null` before a detection; `type` is `spinel`, `ziggurat`, `znp`, `ezsp` or `unknown`; `source` is
  `probe` or `passive`; `version`, `chip`, `baud` and `at` are `null` when unknown.
- `counters` count since boot: `drops` are frames (raw: chunks) lost to a full pool or queue, `resets` the radio's
  resets, `rx_errors` HDLC frames with a bad CRC or too long, `uart_errors` receive errors the USB bridge itself
  reported (a CH34x status packet with an overrun, parity, framing or receive error bit, as the ch34x driver's defines read it, not confirmed against a datasheet; 0 on other bridges and the
  C6). A dongle that sends them is also logged, at most once per 10 s.

`PUT /device/channels/{id}` takes a body of at most 512 bytes with any of: `mode` (`off`, `otbr`, `ziggurat`,
`rcp-proxy`), `baud` (38400, 57600, 115200, 230400, 460800, 921600, 1000000, 2000000; refused on the C6),
`flow_control` (bool), and `rcp_proxy` with any of `direction` (`listen`, `connect`), `adapter` (`tcp`, `udp`), `port`
(1-65535, not 80 or 179), `host` (up to 63 bytes, needed for `connect`), `framing` (`hdlc`, `raw`; UDP only) and
`kick` (`true` drops the current peer or releases the UDP lock; not stored). Unknown keys are refused. Line and proxy
settings are stored and applied at once, also for an unplugged radio.

| Reply | Meaning |
| --- | --- |
| 200 + the channel | applied; with `"reboot": true` when the mode's consumer reboots the board on a change. `otbr` on the channel that runs Zigbee is such a reboot, not a 202 move: no probe; both radios are stored at once (this one `otbr`, the radio Thread ran on `off`), and Thread starts here after the reboot |
| 202 `{"moving": true, "moved_from": "<id>"}` | `otbr` on a channel while another has it: Thread moves after the target passed a probe; the target shows `starting` until it settles. The stored modes change once OpenThread stopped on the old radio, before it starts on the target; a start that fails leaves Thread on the target in `fault` (`RADIO_FAULT`, no rollback), and an abandoned start is `RADIO_FAULT` "an earlier OpenThread start was abandoned: reboot" without a reboot ([Radios](reference/internals.md#moving-thread)). The same for a start (`otbr` while no channel has it; `moved_from` null) and a stop (another mode on the channel that has `otbr`; `moved_from` is that channel: its sessions drain, OpenThread stops, then the new mode is stored) |
| 400 `bad_value` | wrong type or value; `detail` names the field (`mode`, `baud`, `rcp_proxy.port`, `rcp_proxy.host`, ...); `unavailable` cannot be set; a start, move or stop of Thread may not carry `baud`, `flow_control` or `rcp_proxy` settings (send them in a request of their own; nothing is stored) |
| 404 `unknown_channel` | no such id |
| 409 `unavailable`, `incompatible`, `not_available` | the radio is not usable, its detected firmware does not fit the mode, or no consumer is registered for the mode (`ziggurat`) |
| 409 `mode_taken` | the mode is at its channel limit (one radio runs Thread, one runs Zigbee; both may run at once on different radios) |
| 409 `probe_failed` | the target of a move or start did not answer as a spinel RCP, or runs `rcp-proxy` ("set it off first"); nothing changed |
| 409 `port_taken` | the channel would listen in `rcp-proxy` on the adapter and port another channel stored in `rcp-proxy` listens on (also an unplugged one); `detail` names the port and that channel; nothing changed |
| 409 `usbip_enabled` | a mode change on a USB channel while USB/IP is on (line and proxy settings are still accepted) |
| 409 `busy` | an update, a firmware push, a recovery or another move runs, a mode is changed during a move, or the board is stopping or starting OpenThread on its radio (after an unplug, a plug-in) |
| 500 `failed` | the mux is not started, storage failed or a probe took too long |
| 503 `mux_busy` | the radio mux took the change but did not answer within 5 s; a mode, speed or proxy change may still be applied, a start, move or stop will not (read the channel again) |

A move can fail in two ways after the 202. When open Thread sessions do not end within 45 s, nothing is committed:
Thread keeps running on the old channel, `otbr_channel` still names it, and the target shows `"state": "fault"` with
`error` `drain_timeout` (send the move again). When the target does not start, the move is already committed: Thread
stays on the target in `fault` (`RADIO_FAULT`, the reason in `fault_reason` and the target's `error`); there is no
switch back, a PUT of `otbr` on the target retries and one on another channel moves it. After an abandoned start
(`fault_reason` "an earlier OpenThread start was abandoned: reboot") no retry or move starts OpenThread again until
the board reboots. A refused stop leaves the
channel `running` in `otbr` with `error` `drain_timeout`. Poll
`GET /device/channels` after a 202. While OpenThread starts on its channel, that channel shows `starting`.

### USB/IP

The board can export the device on its USB-A port with USB/IP (TCP 3240); see [USB/IP server](usbip.md).
`GET /device/usbip` answers:

```
{"enabled": true, "active": true, "pending_reboot": false,
 "state": "attached", "reason": null,
 "port": 3240, "interface": "mgmt",
 "device": {"busid": "1-1", "vid": "10c4", "pid": "ea60", "bcd_device": "0100", "class": "00",
            "speed": "full", "manufacturer": "Silicon Labs", "product": "CP2102N USB to UART Bridge Controller",
            "serial": "abc123", "interfaces": 1, "endpoints": 2},
 "client": {"addr": "192.168.10.20:51322", "since": 1762790000},
 "counters": {"sessions": 3, "refused": 1, "submits": 12000, "completed": 11950, "errors": 2, "unlinks": 48,
              "bytes_in": 1048576, "bytes_out": 65536, "sessions_ended_overload": 0},
 "memory": {"inflight_bytes": 4096, "inflight_peak": 16384, "waiting_bytes": 0, "waiting_peak": 2048},
 "last_error": null}
```

- `enabled` is the stored value, `active` whether USB/IP owns the port this boot, `pending_reboot` when they differ
  (except after a failed start, below).
- `state` is `off`, `blocked`, `no_device`, `unsupported`, `exportable` or `attached`; `reason` explains `blocked` and
  `unsupported` and is `null` otherwise. When USB/IP is on but failed to start at boot, `state` is `off`, `reason` and
  `last_error` are `"USB/IP failed to start: <error>"` and `pending_reboot` is `false` (`true` once it is turned off).
- `interface` is `single` or `mgmt`. `device` is `null` without a device, `client` without a session (`since` in
  epoch seconds, 0 when the clock is not set). `speed` is `low`, `full`, `high` or `unknown`.
- `counters` and `memory` (bytes) count since boot.

`PUT /device/usbip {"enabled": true|false}` answers `{"reboot": true}` when the board reboots, and `{"reboot": false}`
when the value is the stored one already. `PUT /device/usbip {"kick": true}` drops the current client and answers `{}`
(also without a session; not stored).

| Reply | Meaning |
| --- | --- |
| 200 | applied |
| 400 `bad_value` | the body is not one of the two forms |
| 409 `usb_channel_in_use` | switching on while a USB channel's stored mode is `otbr` or `ziggurat`; `detail` is "`<id>` runs `<mode>`: move it or switch it off first" |
| 409 `busy` | "an update or firmware push is running or waits for its reboot" |
| 500 `store_failed` | the setting could not be written to flash; nothing changed |

### Zigbee coordinator (experimental)

A radio in `ziggurat` mode serves ZHA over a WebSocket at `ws://<board>/zigbee` (binary protocol of zigpy-ziggurat, one
client, a second one takes over; 403 on an interface that does not serve the device API); see
[Zigbee coordinator](zigbee.md). `GET /device/zigbee` answers:

```
{"channel": "builtin-c6", "firmware": "spinel 1.2 efr32", "state": "running", "reason": "",
 "client": {"address": "192.168.1.20:51234", "since": 1790000000}, "last_client": null,
 "radio": {"channel": 15, "pan_id": 6754, "short_address": 0, "extended_address": "00124b00abcdef01", "tx_power_dbm": 8},
 "counters": {"rx": 10, "rx_dropped": 1, "tx": 9, "tx_no_ack": 2, "tx_cca": 3, "tx_failed": 4, "energy_scans": 5,
              "rcp_resets": 6, "frames_in": 7, "frames_out": 8},
 "heap": {"current": 1000, "peak": 2000}, "started": 1789999000}
```

`state` is `idle`, `starting`, `running`, `no_radio` or `incompatible`; `reason` says why when it is not `running`.
`client` and `last_client` (`{"address", "since", "left"}`) are `null` when absent, `radio` while the stack has no
network settings, and times are epoch seconds (0 when the board's clock is not set). `counters.tx` counts transmits the
radio accepted; `tx_failed` also counts those refused before reaching it. `rx_dropped` is the radio channel's counter: frames
received from the radio that the board dropped because its frame pool or event queue was full. `heap` is the memory the stack holds, in bytes. The radio's mode is set with
`PUT /device/channels/<id>`.

## Thread REST API

Upstream esp-thread-br, ot-br-posix 0.6.0 compatible: `/node`, `/node/state`, `/node/dataset/active|pending`,
`/node/ba-id`, `/node/ext-address`, `/node/coprocessor/version`, `/node/ba-epskc/*`, `/node/commissioner/*`, `/api/actions`,
`/api/devices`, `/api/diagnostics`, `/api/node`, `/.well-known/thread/br-rest`. In `SNIFFER` these answer 503 (reads) or 409
(changes); in `STARTING` and `RADIO_FAULT` they answer 503. `DELETE /node/dataset/...` is not supported. `PUT /node/dataset/active`
is accepted while Thread runs (ot-br-posix answers 409): the board stops Thread, stores the dataset and starts Thread on
it (the old network's pending dataset is deleted); the dataset it already has is accepted without a restart. A partial dataset (e.g. only `channel`) still answers 409
while Thread runs: use `/node/dataset/pending` to change a running network. The mode, fault reason
and version information are in `/device/status`, which works in every mode. Home Assistant's OpenThread Border Router integration
connects to `http://<board>`: `tools/otbr_compat.py` checks the `python-otbr-api` calls it makes against the board
(`delete_active_dataset`, its fallback for a router without factory reset, is not used here: the board supports
`DELETE /node`); see [Home Assistant](home-assistant.md).

## Web UI and guides

The web pages are described in the [Web UI tour](web-ui.md). Creating a network, adding border routers and choosing the
leader are in [Your Thread network](thread-network.md).
