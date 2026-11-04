# Service details in the topology scan: design

Date: 2026-11-04. Status: draft.

## 1. Purpose and scope

The Topology page names a device only when it registered with this board's SRP server, and it shows only the service
types. On a network shared with another border router (the Home Assistant OTBR on `ha-thread-bbc1`), most devices
register with the other server, so most nodes stay "Unknown 0xNNNN". The records also carry more than a name: ESPHome
friendly name, version and board, Matter fabrics and sleep timings, HomeKit model and category, border router vendor
and model.

This change makes the scan collect the service records from **every** SRP server on the network, keep their **raw TXT
data**, port, subtypes and lease, and lets the page decode them in JavaScript.

In scope: the scan step that asks the other servers, the extra per-service fields in the scan result, decoding and
display on the Topology page, docs.

Out of scope:
- Asking devices themselves: they run no mDNS responder on the mesh, and their APIs need credentials (Matter fabric
  keys, ESPHome API key). Checked on 2026-11-04, see 2.
- The Home Assistant API (fabric names, Matter device names).
- LAN mDNS as a source.
- Caching records between scans, including commissionable Matter records seen during a pairing window.
- Metrics.

## 2. What we found (2026-11-04, `ha-thread-bbc1`)

- The network data lists two DNS/SRP unicast servers: this board (`0x6800`) and the HA OTBR (`0x1000`). Devices pick
  one each. The HA server held both Matter devices; this board held the ESPHome test device.
- Both servers answer DNS-SD browses and resolves from the mesh (port 53 at the address in the network data).
- The HA server is also a discovery proxy: its browse answers mix in LAN mDNS results (an AirPlay speaker, its own
  meshcop record), with LAN addresses and TTL 10.
- Matter host names (`FEF6E5E01729FF7F`) equal the node's Thread extended address. ESPHome's `mac` TXT is the chip's
  base MAC, not the extended address; ESPHome matches by its OMR address.
- No device answers mDNS on the mesh, unicast or `ff03::fb`; a port-53 control query got answers.
- Raw records seen:
  - ESPHome `_esphomelib._tcp` (port 6053): `friendly_name`, `version`, `platform`, `board`, `network=thread`, `mac`,
    `project_name`, `project_version`, `config_hash`, `api_encryption`. Plus `_http._tcp` port 80 with empty TXT.
  - Matter `_matter._tcp` (port 5540): instance `<compressed fabric>-<node id>`, TXT `SII`, `SAI`, `SAT`, sometimes `T`.
  - Border router `_meshcop._udp`: `vn`, `mn`, `tv`, `nn`, and binary `xa`, `xp`, `omr`, `sb`, `at`, `pt`, `id`.

## 3. Decisions

| Topic | Decision |
| --- | --- |
| Servers asked | every DNS/SRP unicast entry in the network data (`otNetDataGetNextDnsSrpUnicastInfo`, server data and service data forms), except this board's own. Anycast entries are not asked (my call: no server seen uses it; the anycast server's unicast entry, if any, is asked) |
| This board's server | read locally, as today, with the new fields |
| Service types browsed on other servers | fixed list: `_matter._tcp`, `_matterc._udp`, `_esphomelib._tcp`, `_hap._udp`, `_http._tcp`, `_meshcop._udp`, `_trel._udp` (enumerating types with `_services._dns-sd._udp` got no answer from either server) |
| Which server is this board's | the entry whose address is one of this board's own Thread addresses (not by RLOC16) (my call) |
| Query method | OpenThread DNS client (already built: `CONFIG_OPENTHREAD_DNS_CLIENT=y`, service discovery on by default): `otDnsClientBrowse`, then `otDnsClientResolveService` for each instance the browse answer did not describe. "Described" means the answer carried SRV, TXT and at least one AAAA for that instance; anything less gets a resolve (my call). The query config names the server address and port 53 |
| Timing | per query 3 s response timeout, 2 attempts; at most 4 queries in flight, enforced by the scan. The step runs **in parallel** with discover and the child phases from scan start; it needs only the network data, so it never loses time to a slow child phase. Running it first would make `POST /device/topology` answer before discover starts and lose today's synchronous 409 `busy`. It has its own 15 s budget inside the existing 60 s cap, and `finish` waits for both; at the budget, queries still in flight are given up (my call) |
| Server outcome | `srp_servers[].state`: `answered` (at least one query answered), `no_answer` (every query timed out or failed), `cut_short` (the budget or the cap ended it with queries in flight). None of them sets `partial`, which keeps its meaning (routers and children) |
| Which remote hosts are kept | hosts with at least one address under an OMR prefix of the network data or the mesh-local prefix. Others (LAN results from a discovery proxy) are dropped on the board, with one exception below |
| Border router records (`_meshcop._udp`, `_trel._udp`) | these come from a discovery proxy with LAN addresses, so the address rule would drop them. They are kept when their TXT `xp` (extended PAN ID, 8 bytes) equals this network's extended PAN ID; that needs only the active dataset, so the check works during the SRP step, before discover. The page matches them to a router by TXT `xa` = the router's extended address. Records of this board itself (`xa` = its own extended address) are dropped |
| Duplicates | one entry per host name (case-insensitive). The host's fields (`source`, lease or TTL, addresses) come from this board's record, else the first server that answered. Services from all sources merge by instance and type; each service has its own `source` (my call) |
| Raw TXT | the TXT RDATA exactly as received (length-prefixed strings), base64 in JSON. The page decodes it |
| TXT limits | at most 512 bytes per service (longer: cut at a string boundary, `txt_truncated: true`). TXT and subtype labels of a scan live in one 24 KiB pool, a separate `ops.alloc` block owned by the result; the host struct keeps an offset and length per service instead of inline arrays. When the pool is full, later services have no `txt` and `txt_truncated: true` (my call; PSRAM growth is small next to the ~100 KB result) |
| Other limits | existing: 64 hosts, 8 addresses and 8 services per host. New: 6 subtypes per service, 48 resolves per scan |
| Subtypes | from this board's server only (`otSrpServerServiceGetSubTypeServiceNameAt`, label only, e.g. `_I3E5D425037C4F971`). Not asked from other servers |
| Lease | this board's server: key-lease and lease remaining in seconds. Other servers: the TTL of the answer |
| Server status | a new `srp_servers` list says which servers were asked and whether each answered. A server that didn't answer does not make the scan `partial` (`partial` keeps its meaning: routers and children) |
| Device matching on the page | as today (first host sharing an address), plus a host whose name equals the device's extended address (case-insensitive), plus a host with a `_meshcop._udp` or `_trel._udp` service whose TXT `xa` equals the device's extended address. A device can match several hosts (a border router: its meshcop host and its own SRP host); the dialog shows all of them |
| Display name | 1. nickname; 2. ESPHome `friendly_name`; 3. HomeKit instance name; 4. meshcop instance name (e.g. "Home Assistant OpenThread Border Router #0EE6"); 5. SRP host name, unless it is the extended address (Matter), then "Matter device 0xNNNN"; 6. "Unknown 0xNNNN" (my call). This board keeps "This border router" |
| API | no new endpoint; additive fields in `GET /device/topology` |
| Mockup | `docs/superpowers/specs/2026-11-04-srp-details-mockup.html` (the table, and the ESPHome, Matter and border router dialogs) |

## 4. Scan result

Additions only; existing fields keep their meaning.

```json
"srp_servers": [
  {"rloc16": 26624, "address": "fd8f:...:a75c", "port": 53536, "this_board": true, "state": "answered", "hosts": 1},
  {"rloc16": 4096, "address": "fd8f:...:bff7", "port": 53554, "this_board": false, "state": "answered", "hosts": 2}
],
"srp_hosts": [{
  "name": "c6-thread-test",
  "addresses": ["fd87:9b49:8264:1:c158:979b:6560:a15f"],
  "source": 26624,
  "lease_s": 7012, "key_lease_s": 1209412,
  "services": [{
    "instance": "c6-thread-test", "type": "_esphomelib._tcp", "port": 6053, "source": 26624,
    "subtypes": [],
    "txt": "<base64 of the TXT RDATA>", "txt_truncated": false
  }]
}]
```

- `source`: RLOC16 of the server the host came from.
- `lease_s` / `key_lease_s`: only for hosts from this board's server. `ttl_s` instead for hosts from other servers.
- `port` (DNS SRV port of the service) and the TXT RDATA come from the record the server returned. A record without
  TXT has `"txt": ""`. `subtypes` is empty for hosts from other servers.
- `srp_servers[].port` is the SRP port from the network data (the DNS query goes to port 53). `hosts` counts the hosts
  kept from that server, after the address filter and before de-duplication.

## 5. Architecture

- `mesh_scan`:
  - A parallel SRP step (not a phase in the sequence), started with the scan and waited for by `finish`, driven by new ops (browse, resolve, list servers) so the host
    tests run it without OpenThread, like the other phases.
  - `ot_ops.c` reads the server list, starts the DNS client queries with the server's address, and copies the answers
    (instance, host, port, addresses, TTL, raw TXT) into the scan through new callbacks. The local snapshot also copies
    port, TXT, subtypes and lease info.
  - `scan.c` owns the address filter, the `xp` exception for border router records, de-duplication, the limits and
    the TXT pool. `ot_ops.c` supplies this network's extended PAN ID and this board's extended address with the server
    list.
  - `render.c` writes `srp_servers` and the new service fields, with base64 for `txt`.
- `topology.js` (pure part, `L`):
  - `txtEntries(base64)` → list of `{key, value}` where `value` is a string when printable, otherwise bytes (shown as
    hex).
  - `describe(host)` → a summary per known type: ESPHome (friendly name, version, board, project), Matter operational
    (fabric count from the instance names, node IDs, `SII`/`SAI`/`SAT`/`ICD`, `T`), Matter commissionable (`VP`, `DT`,
    `DN`), HomeKit (`md`, `ci` as a category name, `sf` paired or not), border router (`vn`, `mn`, `tv`), HTTP (port).
  - `srpHostFor` becomes `srpHostsFor` (all matching hosts) with the extended-address and `xa` matches.
- Topology page device dialog, left column, after "SRP host":
  - "Device": the summary from `describe` (e.g. "ESPHome 2026.7.4 · esp32-c6-devkitc-1 · project esp32otbr.srp-probe
    0.1", "Matter, on 2 fabrics · polls every 800 ms idle", "Border router: Home Assistant, OpenThread Border Router,
    Thread 1.4.0").
  - "Registered with": "this board", or the server router's display name and RLOC16 (e.g. "Home Assistant OpenThread
    Border Router #0EE6 (0x1000)" when its meshcop record names it), with the lease or TTL.
  - Services list now with ports. An ESPHome `_http._tcp` service gets an "Open" link to `http://[<address>]:<port>/`
    with the host's first address under an OMR prefix; no such address, no link.
  - A "Show raw records" toggle lists every TXT entry per service.
  - HomeKit `ci` names cover the standard HAP accessory categories (1-36).
- The table's search also matches the ESPHome friendly name.
- Docs: `docs/api.md` (topology fields), the topology spec section 3.3 gets a pointer here.

## 6. Risks and open points

- Scan time: two servers × 7 types plus resolves adds a few seconds; a dead server is bounded by the 15 s budget.
  The plan works from the numbers here; the timing is measured in the on-board pass after review, not inside tasks.
- Test data: the ESPHome, Matter operational and meshcop TXT fixtures are the bytes recorded on 2026-11-04 (section 2);
  Matter commissionable, HomeKit and `ICD` fixtures are built from their specs and marked as synthetic.
- OpenThread DNS client messages come from the shared message pool; 4 queries in flight must not starve the mesh
  diagnostics (they run in earlier phases, so they do not overlap).
- The HA server's browse answers for its own SRP entries gave names only, so most Matter devices need one resolve each;
  48 resolves cover about 20 devices on 2 fabrics. Larger networks lose details past that (not names from this board's
  server).
- A discovery proxy could return a mesh device's record taken from LAN mDNS (published by the other border router's
  advertising proxy) with an OMR address. De-duplication by host name covers it.
- ESPHome and HomeKit details on a real HomeKit device, Matter commissionable records, and `ICD` were not seen on the
  test network; their decoding is tested with recorded sample bytes only.
