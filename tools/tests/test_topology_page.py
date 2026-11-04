"""Topology page (BDD: TopologyLogic, TopologyPage). Runs static/topology.js under node."""

import base64

import pytest
from device_js import logic, needs_node, page, scenario

# The sketch mesh as a JS literal. R: this board 0x8800 is a router; 0xd400 is another border router and the REED
# 0x5c01 didn't report its addresses. B: the same mesh with this board as a child. M: the real-mesh geometry of the
# 2026-11-03 hardware pass (made-up identifiers): this board 0x3400 between the other border router 0x1000 and the
# leader 0xe400, 0x3000 hanging off 0x1000 on a weak link, and a REED child of the board that didn't report.
FIXTURE = r"""
const mode = (rx) => ({rx_on_when_idle: rx, full_thread_device: false, full_network_data: false});
const detail = {timeout_s: 240, age_s: 12, connected_s: 86400, rssi_avg: -61, rssi_last: -63, link_margin_db: 39,
                frame_error_pct: 0.4, message_error_pct: 0.0, version: 4, csl: false};
const NET = {name: 'MyThread', channel: 15, partition_id: 1243556871, leader_router_id: 9,
             mesh_local_prefix: 'fdde:ad00:beef::/64', this_rloc16: 0x8800,
             omr_prefixes: ['2001:db8:4a2:1::/64', 'fd7a:91c3:22e0:1::/64']};
const SRP = [{name: 'kitchen-plug', addresses: ['2001:db8:4a2:1:2be4:6c19:a0f7:3381'],
              services: [{instance: 'kitchen-plug', type: '_matter._tcp'}]},
             {name: '<b>"x"</b>', addresses: ['fd7a:91c3:22e0:1::77'], services: []}];
const routers = () => [
  {rloc16: 0x2400, router_id: 9, ext_address: 'd61a7c03f4e2b918', version: 5, leader: true, this_device: false,
   border_router: false, links: [{router_id: 34, lq_in: 3}, {router_id: 23, lq_in: 2}, {router_id: 40, lq_in: 1}],
   addresses: ['fdde:ad00:beef::ff:fe00:2400', 'fdde:ad00:beef::ff:fe00:fc00', 'fe80::d41a:7c03:f4e2:b918'],
   responded: true, children_detail: true,
   children: [{rloc16: 0x2401, ext_address: '1c88f02ab4d6e5a3', mode: mode(false), lq_in: 3, ...detail,
               addresses: ['fdde:ad00:beef::1c88:f02a:b4d6']}]},
  {rloc16: 0x5c00, router_id: 23, ext_address: 'aa00000000005c00', version: 5, leader: false, this_device: false,
   border_router: false, links: [{router_id: 9, lq_in: 2}, {router_id: 34, lq_in: 3}, {router_id: 99, lq_in: 1}],
   addresses: ['fdde:ad00:beef::ff:fe00:5c00'], responded: true, children_detail: false,
   children: [{rloc16: 0x5c02, mode: mode(false), lq_in: 2, addresses: []},
              {rloc16: 0x5c01, mode: {...mode(true), full_thread_device: true}, lq_in: 3, addresses: [],
               addresses_reported: false}]},
  {rloc16: 0x8800, router_id: 34, ext_address: 'bb00000000008800', version: 5, leader: false, this_device: true,
   border_router: true, links: [{router_id: 9, lq_in: 3}, {router_id: 40, lq_in: 2}],
   addresses: ['fdde:ad00:beef::ff:fe00:8800', '2001:db8:4a2:1::1'], responded: true, children_detail: true,
   children: [{rloc16: 0x8801, ext_address: 'c100000000008801', mode: mode(true), lq_in: 3, ...detail,
               addresses: ['2001:db8:4a2:1::8801']},
              {rloc16: 0x8802, ext_address: 'c100000000008802', mode: mode(false), lq_in: 2, ...detail,
               addresses: ['fd7a:91c3:22e0:1::77']},
              {rloc16: 0x8803, ext_address: 'c100000000008803', mode: mode(false), lq_in: 1, ...detail,
               addresses: ['fdde:ad00:beef::8803:1', 'fe80::1']}]},
  {rloc16: 0xa000, router_id: 40, ext_address: 'dd0000000000a000', version: 5, leader: false, this_device: false,
   border_router: false, links: [{router_id: 9, lq_in: 3}, {router_id: 34, lq_in: 2}],
   addresses: ['fdde:ad00:beef::ff:fe00:a000'], responded: true, children_detail: true,
   children: [{rloc16: 0xa001, ext_address: 'e1000000000a0001', mode: mode(false), lq_in: 3, ...detail,
               addresses: ['2001:db8:4a2:1:2be4:6c19:a0f7:3381']}]},
  {rloc16: 0xd400, router_id: 53, ext_address: 'ee0000000000d400', leader: false, this_device: false,
   border_router: true, links: [], addresses: ['fdde:ad00:beef::ff:fe00:d400'], responded: true,
   children_detail: true, children: []},
  {rloc16: 0xdc00, router_id: 55, responded: false}
];
const R = {partial: true, network: NET, routers: routers(), srp_hosts: SRP};
const B = (() => {
  const r = routers();
  r[2].this_device = false; r[2].border_router = false;
  r[3].children.push({rloc16: 0xa002, ext_address: 'e1000000000a0002', mode: mode(true), lq_in: 3, ...detail,
                      addresses: ['2001:db8:4a2:1::a002']});
  return {partial: false, network: {...NET, this_rloc16: 0xa002}, routers: r, srp_hosts: SRP};
})();
const M = {partial: false, srp_hosts: [],
  network: {...NET, leader_router_id: 57, this_rloc16: 0x3400},
  routers: [
    {rloc16: 0x1000, router_id: 4, ext_address: 'a100000000001000', version: 5, leader: false, this_device: false,
     border_router: true, links: [{router_id: 13, lq_in: 3}, {router_id: 57, lq_in: 1}, {router_id: 12, lq_in: 1}],
     addresses: ['fdde:ad00:beef::ff:fe00:1000'], responded: true, children_detail: true, children: []},
    {rloc16: 0x3000, router_id: 12, ext_address: 'a100000000003000', version: 5, leader: false, this_device: false,
     border_router: false, links: [{router_id: 4, lq_in: 1}],
     addresses: ['fdde:ad00:beef::ff:fe00:3000'], responded: true, children_detail: true, children: []},
    {rloc16: 0x3400, router_id: 13, ext_address: 'a100000000003400', version: 5, leader: false, this_device: true,
     border_router: true, links: [{router_id: 4, lq_in: 3}, {router_id: 57, lq_in: 3}],
     addresses: ['fdde:ad00:beef::ff:fe00:3400'], responded: true, children_detail: true,
     children: [{rloc16: 0x3401, ext_address: 'a100000000003401',
                 mode: {rx_on_when_idle: true, full_thread_device: true, full_network_data: true}, lq_in: 3,
                 ...detail, addresses: [], addresses_reported: false}]},
    {rloc16: 0xe400, router_id: 57, ext_address: 'a10000000000e400', version: 5, leader: true, this_device: false,
     border_router: false, links: [{router_id: 13, lq_in: 3}, {router_id: 4, lq_in: 2}],
     addresses: ['fdde:ad00:beef::ff:fe00:e400'], responded: true, children_detail: true, children: []}]};
// SH: the mesh of the 2026-11-03 screenshots (the old layout put the Home box on the board's links and LQI badges):
// this board 0x8800 linked to all three other routers, each with children.
const kid = (rloc16) => ({rloc16, ext_address: 'f' + rloc16.toString(16).padStart(15, '0'), mode: mode(false), lq_in: 3,
                          ...detail, addresses: []});
const srouter = (rloc16, router_id, links, kids, extra) => ({rloc16, router_id, ext_address: rloc16.toString(16).padStart(16, '0'),
  version: 5, leader: false, this_device: false, border_router: false, addresses: [], responded: true,
  children_detail: true, links: links.map(([id, lq]) => ({router_id: id, lq_in: lq})), children: kids.map(kid), ...extra});
const SH = {partial: false, srp_hosts: [], network: {...NET, leader_router_id: 9, this_rloc16: 0x8800}, routers: [
  srouter(0x2400, 9, [[34, 3], [23, 2], [40, 3]], [0x2401], {leader: true}),
  srouter(0x5c00, 23, [[9, 2], [34, 3]], [0x5c01, 0x5c02]),
  srouter(0x8800, 34, [[9, 3], [23, 3], [40, 2]], [0x8801, 0x8802, 0x8803], {this_device: true, border_router: true}),
  srouter(0xa000, 40, [[9, 3], [34, 2]], [0xa001, 0xa002])]};
// TR: R with this board's radios: it reaches the leader (9) over 15.4 and TREL (TREL losing), router 40 over TREL only.
const TR = (() => {
  const r = routers();
  r[2].links = [{router_id: 9, lq_in: 3, transport: {ieee802154: 230, trel: 55}},
                {router_id: 40, lq_in: 2, transport: {ieee802154: null, trel: 255}}];
  return {...R, partial: false, routers: r};
})();
const D = L.devices(R), DB = L.devices(B), LK = L.routerLinks(R);
const byId = (ds, id) => ds.find((d) => d.id === id);
const names = (ds, list) => list.map((d) => d.id);
"""


# The page's map labels for a device list: "This border router" for the board, else the display name.
LABELS = "((ds) => Object.fromEntries(ds.map((d) => [d.id, d.thisBoard ? 'This border router' : L.displayName(d, {}, SRP)])))"


# Node and label footprints as the layout models them (node shape, then the label under it, two lines for a stub),
# relative to the node centre. (ds, m, labels) -> [[id, x0, x1, y0, y1]].
FOOTPRINTS = """((ds, m, labels) => ds.filter((d) => m.pos[d.id]).map((d) => {
  const half = L.nodeHalf(d), p = m.pos[d.id];
  const w = Math.max(2 * half, L.mapLabel(labels[d.id]).length * (d.parentId === null ? 7 : 6.5));
  return [d.id, p.x - w / 2, p.x + w / 2, p.y - half, p.y + half + 4 + 14 * (d.kind === 'stub' ? 2 : 1)];
}))"""

# (ds, lk) -> {hits: ids whose footprint the Home box overlaps, inside: the box lies inside the map}.
HOME_HITS = f"""((ds, lk) => {{
  const labels = {LABELS}(ds), m = L.layout(ds, lk, labels), h = m.home;
  const x0 = h.x - h.w / 2, x1 = h.x + h.w / 2, y0 = h.y - h.h / 2, y1 = h.y + h.h / 2;
  const hits = {FOOTPRINTS}(ds, m, labels).filter((b) => x0 < b[2] && b[1] < x1 && y0 < b[4] && b[3] < y1)
                   .map((b) => b[0]);
  const fps = {FOOTPRINTS}(ds, m, labels), lo = (i) => Math.min(...fps.map((b) => b[i])), hi = (i) => Math.max(...fps.map((b) => b[i]));
  const nodes = {{x0: lo(1), x1: hi(2), y0: lo(3), y1: hi(4)}};
  return {{hits, inside: x0 >= 0 && y0 >= 0 && x1 <= m.width && y1 <= m.height, box: {{x0, x1, y0, y1}}, nodes,
          width: m.width, height: m.height}};
}})"""

# Every line the page draws, as [a, b] node centres: router links, then parent lines. (ds, lk, m) -> [[a, b]].
LINES = """((ds, lk, m) => lk.filter((l) => m.pos[l.a] && m.pos[l.b]).map((l) => [m.pos[l.a], m.pos[l.b]])
  .concat(ds.filter((d) => d.parentId !== null && m.pos[d.id] && m.pos[d.parentId]).map((d) => [m.pos[d.parentId], m.pos[d.id]])))"""

# The LQI badges as the page draws them: 10 + 7 px per character wide, 16 px high, at the link middle. -> [[x0, x1, y0, y1]].
BADGES = """((lk, m) => lk.filter((l) => m.pos[l.a] && m.pos[l.b]).map((l) => {
  const w = 10 + 7 * L.linkText(l).length, x = (m.pos[l.a].x + m.pos[l.b].x) / 2, y = (m.pos[l.a].y + m.pos[l.b].y) / 2;
  return [x - w / 2, x + w / 2, y - 8, y + 8];
}))"""

# (ds, lk) -> {lines: drawn lines through the Home box, badges: LQI badges it overlaps}.
HOME_ON_LINKS = f"""((ds, lk) => {{
  const m = L.layout(ds, lk, {LABELS}(ds)), h = m.home;
  const box = {{x0: h.x - h.w / 2, x1: h.x + h.w / 2, y0: h.y - h.h / 2, y1: h.y + h.h / 2}};
  return {{lines: {LINES}(ds, lk, m).filter(([a, b]) => L.crosses(a, b, box)).length,
          badges: {BADGES}(lk, m).filter((b) => box.x0 < b[1] && b[0] < box.x1 && box.y0 < b[3] && b[2] < box.y1).length}};
}})"""

# (ds, lk) -> {closer: a free box centre more than 12 px nearer the board than the box, or null}. Grid step 4 px.
HOME_NEAREST = f"""((ds, lk) => {{
  const labels = {LABELS}(ds), m = L.layout(ds, lk, labels), h = m.home;
  const board = ds.find((d) => d.thisBoard), b = m.pos[board.id];
  const fps = {FOOTPRINTS}(ds, m, labels).map((f) => f.slice(1)).concat({BADGES}(lk, m)), lines = {LINES}(ds, lk, m);
  const taken = Math.hypot(h.x - b.x, h.y - b.y);
  const free = (x, y) => fps.every((f) => !(x - h.w / 2 < f[1] + 6 && f[0] < x + h.w / 2 + 6 &&
                                            y - h.h / 2 < f[3] + 6 && f[2] < y + h.h / 2 + 6)) &&
      lines.every(([p, q]) => !L.crosses(p, q, {{x0: x - h.w / 2, x1: x + h.w / 2, y0: y - h.h / 2, y1: y + h.h / 2}}));
  for (let dx = -taken; dx <= taken; dx += 4)
    for (let dy = -taken; dy <= taken; dy += 4)
      if (Math.hypot(dx, dy) < taken - 12 && free(b.x + dx, b.y + dy))
        return {{closer: [dx, dy], taken}};
  return {{closer: null, taken}};
}})"""


def js(expression):
    """Evaluate `expression` with the fixture (R, B, D, DB, LK, NET, SRP) in scope."""
    return logic("topology", f"(() => {{ {FIXTURE} return ({expression}); }})()")


def plain(expression):
    return logic("topology", expression)


def txt(*entries):
    """Base64 of TXT RDATA (length-prefixed strings) built from `key=value` entries (str or bytes)."""
    raw = b""
    for e in entries:
        b = e.encode() if isinstance(e, str) else e
        raw += bytes([len(b)]) + b
    return base64.b64encode(raw).decode()


# TXT records rebuilt from the keys and values recorded on 2026-11-04 on ha-thread-bbc1 (spec section 2 and the
# mockup): the ESPHome test device (this board's SRP server), a Matter device on two fabrics and the HA border router's
# meshcop record (the HA server; xp/xa as in the srp.c host tests, the meshcop entry order is not the recorded one).
ESPHOME_TXT = txt(
    "friendly_name=C6 Thread Test", "version=2026.7.4", "platform=ESP32", "board=esp32-c6-devkitc-1",
    "network=thread", "mac=acebe60bffa4", "project_name=esp32otbr.srp-probe", "project_version=0.1",
    "config_hash=e89f8cd1", "api_encryption=Noise_NNpsk0_25519_ChaChaPoly_SHA256",
)  # fmt: skip
MATTER_TXT = txt("SII=800", "SAI=2000", "SAT=4000")
MATTER_T_TXT = txt("SII=800", "SAI=2000", "SAT=4000", "T=0")
MESHCOP_TXT = txt(
    "rv=1", "nn=ha-thread-bbc1", b"xp=" + bytes.fromhex("5965b57e30c0def4"), "tv=1.4.0",
    b"xa=" + bytes.fromhex("7e287c8b8b140ee6"), b"sb=" + bytes.fromhex("000009b1"), "vn=Home Assistant",
    "mn=OpenThread Border Router",
)  # fmt: skip
# Synthetic (built from the HAP and Matter specs, never seen on the test network): a HomeKit outlet, a Matter
# device in pairing mode, an ICD, key-only and odd entries.
HOMEKIT_TXT = txt(
    "c#=3",
    "ff=0",
    "id=AA:BB:CC:DD:EE:FF",
    "md=Eve Energy 20EBO8301",
    "pv=1.1",
    "s#=1",
    "sf=0",
    "ci=7",
)
MATTERC_TXT = txt(
    "D=3840", "CM=1", "VP=4447+40961", "DT=266", "DN=Smart Plug", "SII=5000", "SAI=300"
)
ICD_TXT = txt("SII=5000", "SAI=300", "SAT=4000", "ICD=1")
ODD_TXT = txt("at", "=nokey", "empty=", "name=<b>x</b>")

# Hosts as the scan reports them (spec section 4) and devices as L.devices builds them, for the SRP decoders.
# 0x1000 is the HA border router: its meshcop host (LAN address, from its discovery proxy) and its own SRP host
# registered with this board. 0x3000 is a Matter device registered with the HA server (host name = ext address).
SRP_FIXTURE = f"""
const svc = (instance, type, port, source, txt) => ({{instance, type, port, source, subtypes: [], txt,
                                                     txt_truncated: false}});
const HOSTS = [
  {{name: 'homeassistant', addresses: ['fdaa:20::69'], source: 4096, ttl_s: 10,
   services: [svc('Home Assistant OpenThread Border Router #0EE6', '_meshcop._udp', 49154, 4096, '{MESHCOP_TXT}')]}},
  {{name: 'c6-thread-test', addresses: ['fd87:9b49:8264:1:c158:979b:6560:a15f'], source: 26624, lease_s: 7012,
   key_lease_s: 1209412, services: [svc('c6-thread-test', '_esphomelib._tcp', 6053, 26624, '{ESPHOME_TXT}'),
                                    svc('c6-thread-test', '_http._tcp', 80, 26624, '')]}},
  {{name: 'FEF6E5E01729FF7F', addresses: ['fd87:9b49:8264:1:69d5:55aa:6eff:e014'], source: 4096, ttl_s: 2940,
   services: [svc('CAC23A5DA6E7644C-000000000F4B0B5F', '_matter._tcp', 5540, 4096, '{MATTER_TXT}'),
              svc('3E5D425037C4F971-0000000000000003', '_matter._tcp', 5540, 4096, '{MATTER_T_TXT}')]}},
  {{name: 'homeassistant-otbr', addresses: ['fd87:9b49:8264:1:aaaa::1000'], source: 26624, lease_s: 5000,
   key_lease_s: 1200000, services: []}},
  {{name: 'eve-energy-1a2b', addresses: ['fd87:9b49:8264:1::4c01'], source: 26624, lease_s: 5000,
   key_lease_s: 1200000, services: [svc('Eve Energy 1A2B', '_hap._udp', 5001, 26624, '{HOMEKIT_TXT}')]}},
  {{name: '0E1F2A3B4C5D6E7F', addresses: ['fd87:9b49:8264:1::5001'], source: 26624, lease_s: 100, key_lease_s: 100,
   services: [svc('5F3A1B2C3D4E5F60', '_matterc._udp', 5540, 26624, '{MATTERC_TXT}')]}},
  {{name: 'A1B2C3D4E5F60718', addresses: ['fd87:9b49:8264:1::5002'], source: 26624, lease_s: 100, key_lease_s: 100,
   services: [svc('CAC23A5DA6E7644C-0000000000000009', '_matter._tcp', 5540, 26624, '{ICD_TXT}')]}},
  {{name: 'printer', addresses: ['fd87:9b49:8264:1::7001'], source: 26624, lease_s: 100, key_lease_s: 100,
   services: [svc('Printer', '_ipp._tcp', 631, 26624, '{txt("rp=ipp/print")}')]}},
  {{name: 'esp-otbr', addresses: ['fd87:9b49:8264:1::6800'], source: 26624, lease_s: 100, key_lease_s: 100,
   services: []}}];
const dev = (id, rloc16, ext, addrs, extra) => ({{id, rloc16, ext, addrs, thisBoard: false, parentId: null, ...extra}});
const DEVS = [
  dev('r1', 0x6400, '4eeb2e6a619150e3', ['fd87:9b49:8264:1:c158:979b:6560:a15f']),
  dev('r2', 0x3000, 'fef6e5e01729ff7f', ['fdde:ad00:beef::ff:fe00:3000']),
  dev('r3', 0x1000, '7e287c8b8b140ee6', ['fd87:9b49:8264:1:aaaa::1000']),
  dev('r4', 0x4c00, 'c100000000004c00', ['fd87:9b49:8264:1::4c01']),
  dev('r5', 0x5c00, 'aa00000000005c00', []),
  dev('r6', 0x6800, 'bb00000000006800', ['fd87:9b49:8264:1::6800'], {{thisBoard: true}}),
  dev('r7', 0x7000, 'cc00000000007000', ['fd87:9b49:8264:1::7001'])];
const host = (name) => HOSTS.find((h) => h.name === name);
const dv = (id) => DEVS.find((d) => d.id === id);
"""


def srp(expression):
    """Evaluate `expression` with the SRP fixture (HOSTS, DEVS, host(name), dv(id)) in scope."""
    return logic("topology", f"(() => {{ {SRP_FIXTURE} return ({expression}); }})()")


NETWORK = "{mesh_local_prefix: 'fdde:ad00:beef:0::/64', omr_prefixes: ['2001:db8:4a2:1::/64', 'fd7a:91c3:22e0:1::/64']}"
STORE = """(() => { const m = {}; return {m, getItem: (k) => (k in m ? m[k] : null),
  setItem: (k, v) => { m[k] = String(v); }, removeItem: (k) => { delete m[k]; }}; })()"""
BROKEN = "{getItem() { throw new Error('denied'); }, setItem() { throw new Error('denied'); }, removeItem() { throw new Error('denied'); }}"

STACK_SL = "SL-OPENTHREAD/2.4.2.0_GitHub-7074a43e4; EFR32; Oct 30 2024 14:04"
STACK_ESP = "openthread-esp32/fff9895c-b678a4f; esp32c6;  2026-11-03 21:47:25"
STACK_POSIX = "OPENTHREAD/cb18e0f; POSIX; Oct  6 2026 20:28:48"


@needs_node
class TestTopologyLogic:
    """TopologyLogic pure functions"""

    # addressClass

    def test_address_under_a_global_omr_prefix_is_global(self):
        # On an address under a 2000::/3 OMR prefix: "global".
        assert (
            plain(f"L.addressClass('2001:db8:4a2:1:2be4:6c19:a0f7:3381', {NETWORK})")
            == "global"
        )

    def test_address_under_a_ula_omr_prefix_is_ula(self):
        # On an address under an fc00::/7 OMR prefix: "ula".
        assert plain(f"L.addressClass('fd7a:91c3:22e0:1::77', {NETWORK})") == "ula"

    def test_mesh_local_rloc_address_is_rloc(self):
        # On mesh-local with IID 0:ff:fe00:xxxx, xxxx < 0xfc00: "rloc".
        assert (
            plain(f"L.addressClass('fdde:ad00:beef::ff:fe00:5c00', {NETWORK})")
            == "rloc"
        )

    def test_mesh_local_fc_address_is_aloc(self):
        # On mesh-local with IID 0:ff:fe00:fcxx: "aloc".
        assert (
            plain(f"L.addressClass('fdde:ad00:beef:0:0:ff:fe00:fc00', {NETWORK})")
            == "aloc"
        )

    def test_other_mesh_local_address_is_mleid(self):
        # On other mesh-local: "mleid".
        assert (
            plain(f"L.addressClass('fdde:ad00:beef::1c88:f02a:b4d6', {NETWORK})")
            == "mleid"
        )

    def test_fe80_address_is_linklocal(self):
        # On fe80::/10: "linklocal".
        assert (
            plain(f"L.addressClass('fe80::d41a:7c03:f4e2:b918', {NETWORK})")
            == "linklocal"
        )

    def test_ff00_address_is_multicast(self):
        # On ff00::/8: "multicast".
        assert plain(f"L.addressClass('ff03::1', {NETWORK})") == "multicast"

    def test_unknown_address_is_other(self):
        # Otherwise: "other" (also for text that is not an address).
        assert plain(
            f"[L.addressClass('2a00:1::1', {NETWORK}), L.addressClass('nonsense', {NETWORK})]"
        ) == [
            "other",
            "other",
        ]

    # devices

    def test_devices_lists_every_router_stub_and_child_with_kind_and_parent(self):
        # On a result: one device per router, stub and child, each with kind, parent id and display fields.
        got = js("D.map((d) => [d.id, d.kind, d.parentId, d.rloc16])")
        assert got == [
            ["r9", "leader", None, 0x2400],
            ["c9217", "sed", "r9", 0x2401],
            ["r23", "router", None, 0x5C00],
            ["c23553", "med", "r23", 0x5C01],
            ["c23554", "sed", "r23", 0x5C02],
            ["r34", "br", None, 0x8800],
            ["c34817", "med", "r34", 0x8801],
            ["c34818", "sed", "r34", 0x8802],
            ["c34819", "sed", "r34", 0x8803],
            ["r40", "router", None, 0xA000],
            ["c40961", "sed", "r40", 0xA001],
            ["r53", "router", None, 0xD400],
            ["r55", "stub", None, 0xDC00],
        ]

    def test_devices_flags_this_board_and_keeps_stub_and_missing_fields_unknown(self):
        # On a result: only the board router is flagged; a stub has no ext/addresses/version; detail follows the data.
        got = js(
            "[D.filter((d) => d.thisBoard).map((d) => d.id), byId(D, 'r55').ext, byId(D, 'r55').responded,"
            " byId(D, 'r55').addrs, byId(D, 'r53').version, byId(D, 'r9').version,"
            " byId(D, 'r23').detail, byId(D, 'r34').detail, byId(D, 'c23554').ext, byId(D, 'c23554').detail,"
            " byId(D, 'c34818').detail, byId(D, 'c34818').child.rssi_avg, byId(D, 'c34818').sleepy]"
        )
        assert got == [
            ["r34"],
            None,
            False,
            [],
            None,
            5,
            False,
            True,
            None,
            False,
            True,
            -61,
            True,
        ]

    def test_devices_marks_the_child_matching_this_rloc16_when_no_router_is_this_device(
        self,
    ):
        # On this board being a child (no this_device router): marks the child with rloc16 == this_rloc16 as this board.
        got = js(
            "[DB.filter((d) => d.thisBoard).map((d) => [d.id, d.kind, d.parentId]), DB.filter((d) => d.kind === 'br').length]"
        )
        assert got == [[["c40962", "br", "r40"]], 1]

    def test_devices_flags_a_child_that_didnt_report_its_addresses(self):
        # On a child with addresses_reported false: addrsReported false; true for every other device.
        got = js(
            "[byId(D, 'c23553').addrsReported, byId(D, 'c23554').addrsReported, byId(D, 'c34817').addrsReported,"
            " byId(D, 'r23').addrsReported, byId(L.devices(M), 'c13313').addrsReported]"
        )
        assert got == [False, True, True, True, False]

    def test_devices_marks_another_border_router_and_keeps_its_kind(self):
        # On a router with border_router and not this_device: kind stays router/leader, otherBr true; this board,
        # plain routers, stubs and children are not other border routers.
        got = js(
            "(() => { const X = {...R, routers: routers()}; X.routers[0].border_router = true; const DX = L.devices(X);"
            " const DM = L.devices(M);"
            " return [[byId(D, 'r53').kind, byId(D, 'r53').otherBr], [byId(DX, 'r9').kind, byId(DX, 'r9').otherBr],"
            " [byId(DM, 'r4').kind, byId(DM, 'r4').otherBr],"
            " ['r13', 'r12', 'r57', 'c13313'].map((id) => byId(DM, id).otherBr),"
            " [byId(D, 'r55').otherBr, byId(D, 'r34').otherBr, byId(D, 'r9').otherBr]]; })()"
        )
        assert got == [
            ["router", True],
            ["leader", True],
            ["router", True],
            [False, False, False, False],
            [False, False, False],
        ]

    # routerLinks

    def test_routerlinks_pairs_two_sides_with_the_lower_id_first(self):
        # On two routers that list each other: one link, a = lower id, lqAB = b's lq_in for a, lqBA = a's lq_in for b.
        got = js("LK.filter((l) => l.a === 'r9' && l.b === 'r34')")
        # r34 lists r9 with 3 (lqAB); r9 lists r34 with 3 (lqBA)
        assert got == [{"a": "r9", "b": "r34", "lqAB": 3, "lqBA": 3}]
        got = js("LK.filter((l) => l.a === 'r9' && l.b === 'r40')")
        assert got == [{"a": "r9", "b": "r40", "lqAB": 3, "lqBA": 1}]

    def test_routerlinks_fills_a_missing_side_with_zero(self):
        # On a link listed by one side only: one link with the missing side 0.
        got = js("LK.filter((l) => l.a === 'r23' && l.b === 'r34')")
        assert got == [{"a": "r23", "b": "r34", "lqAB": 0, "lqBA": 3}]

    def test_routerlinks_leaves_a_stub_routers_side_unknown(self):
        # On a link to a router that didn't answer (a stub): its side is null (unknown), not 0.
        got = plain(
            "L.routerLinks({routers: [{router_id: 9, responded: true, links: [{router_id: 55, lq_in: 2}]},"
            " {router_id: 55, responded: false}, {router_id: 3, responded: true, links: [{router_id: 55, lq_in: 1}]}]})"
        )
        assert got == [
            {"a": "r3", "b": "r55", "lqAB": None, "lqBA": 1},
            {"a": "r9", "b": "r55", "lqAB": None, "lqBA": 2},
        ]

    def test_routerlinks_drops_a_link_to_an_unknown_router(self):
        # On a link to a router id that is in no router entry: no link.
        got = js("[LK.some((l) => l.a === 'r99' || l.b === 'r99'), LK.length]")
        assert got == [False, 5]

    # linkText

    def test_linktext_is_one_number_on_equal_sides(self):
        # On equal sides: "3".
        assert plain("L.linkText({a: 'r1', b: 'r2', lqAB: 3, lqBA: 3})") == "3"

    def test_linktext_shows_in_slash_out_in_as_view_on_different_sides(self):
        # On different sides: "in/out" in a's view, i.e. lqBA (into a) then lqAB (out of a).
        assert plain("L.linkText({a: 'r1', b: 'r2', lqAB: 3, lqBA: 2})") == "2/3"

    def test_linktext_shows_only_the_known_side_of_a_stub_link(self):
        # On one side unknown (null): only the known number.
        assert plain(
            "[L.linkText({a: 'r1', b: 'r2', lqAB: null, lqBA: 2}), L.linkText({a: 'r1', b: 'r2', lqAB: 1, lqBA: null})]"
        ) == ["2", "1"]

    # linkQuality

    def test_linkquality_is_the_lower_side(self):
        # On two known sides: the lower one.
        assert plain("L.linkQuality({a: 'r1', b: 'r2', lqAB: 3, lqBA: 1})") == 1

    def test_linkquality_uses_the_known_side_of_a_stub_link(self):
        # On one side unknown (null): the known side, never 0.
        assert plain(
            "[L.linkQuality({a: 'r1', b: 'r2', lqAB: null, lqBA: 3}), L.linkQuality({a: 'r1', b: 'r2', lqAB: 2, lqBA: null})]"
        ) == [3, 2]

    # displayName

    def test_displayname_prefers_the_nickname_for_the_ext_address(self):
        # On a nickname for the ext address: the nickname.
        got = js(
            "L.displayName(byId(D, 'c40961'), {e1000000000a0001: 'Hall light'}, SRP)"
        )
        assert got == "Hall light"

    def test_displayname_uses_the_srp_host_sharing_an_address(self):
        # On an SRP host sharing an address: the host name.
        assert js("L.displayName(byId(D, 'c40961'), {}, SRP)") == "kitchen-plug"

    def test_displayname_falls_back_to_unknown_rloc16(self):
        # Otherwise: "Unknown 0xNNNN".
        assert js("L.displayName(byId(D, 'c23553'), {}, SRP)") == "Unknown 0x5c01"

    def test_displayname_of_this_board_without_nickname_or_srp_host_is_this_border_router(
        self,
    ):
        # On this board with no nickname and no SRP host: "This border router" (spec 4.6), never "Unknown".
        assert js("L.displayName(byId(D, 'r34'), {}, SRP)") == "This border router"
        assert js("L.displayName(byId(DB, 'c40962'), {}, SRP)") == "This border router"

    def test_displayname_of_this_board_with_a_board_name_is_that_name(self):
        # On this board with a board name and no nickname: the board name; a nickname still wins.
        assert (
            js("L.displayName(byId(D, 'r34'), {}, SRP, 'otbr-attic')") == "otbr-attic"
        )
        assert (
            js(
                "L.displayName(byId(D, 'r34'), {bb00000000008800: 'Attic BR'}, SRP, 'otbr-attic')"
            )
            == "Attic BR"
        )

    def test_boardname_prefers_the_live_hostname(self):
        # On a /device/names body: hostname_live, else hostname; blank or missing: null.
        assert js(
            "[L.boardName({hostname: 'otbr', hostname_live: 'otbr-2'}), L.boardName({hostname: 'otbr'}),"
            " L.boardName({hostname: ' ', hostname_live: ''}), L.boardName(null), L.boardName({hostname: 7})]"
        ) == ["otbr-2", "otbr", None, None, None]

    def test_displayname_of_this_board_prefers_its_nickname(self):
        # On this board with a nickname: the nickname.
        assert (
            js("L.displayName(byId(D, 'r34'), {bb00000000008800: 'Attic BR'}, SRP)")
            == "Attic BR"
        )

    # rows

    def test_rows_all_lists_the_board_its_children_then_routers_with_children(self):
        # On "all": this board, its children, then each router by router id followed by its children.
        got = js("L.rows(D, 'all', '', {}, SRP).map((d) => d.id)")
        assert got == [
            "r34", "c34817", "c34818", "c34819",
            "r9", "c9217",
            "r23", "c23553", "c23554",
            "r40", "c40961",
            "r53", "r55",
        ]  # fmt: skip

    def test_rows_routers_keeps_only_routers_in_the_same_order(self):
        # On "routers": only routers, stubs, the leader and the board, same order.
        assert js("L.rows(D, 'routers', '', {}, SRP).map((d) => d.id)") == [
            "r34",
            "r9",
            "r23",
            "r40",
            "r53",
            "r55",
        ]

    def test_rows_end_keeps_only_children_in_the_same_order(self):
        # On "end": only children, same order.
        assert js("L.rows(D, 'end', '', {}, SRP).map((d) => d.id)") == [
            "c34817", "c34818", "c34819", "c9217", "c23553", "c23554", "c40961",
        ]  # fmt: skip

    def test_rows_sleepy_keeps_only_sleepy_children(self):
        # On "sleepy": only sleepy devices, same order.
        assert js("L.rows(D, 'sleepy', '', {}, SRP).map((d) => d.id)") == [
            "c34818", "c34819", "c9217", "c23554", "c40961",
        ]  # fmt: skip

    def test_rows_puts_a_board_that_is_a_child_under_its_parent(self):
        # On this board being a child: it sits under its parent, in RLOC16 order, with no leading board row.
        got = js("L.rows(DB, 'all', '', {}, SRP).map((d) => d.id)")
        assert got[0] == "r9"
        assert got[got.index("r40") : got.index("r40") + 4] == [
            "r40",
            "c40961",
            "c40962",
            "r53",
        ]

    def test_rows_query_matches_name_rloc_ext_srp_and_addresses_ignoring_case(self):
        # On a query: keeps rows whose name, RLOC16, ext address, SRP host name or any address contains it.
        got = js(
            "['KITCHEN', '0x5C01', '5c01', 'E1000000000A0001', '2001:db8:4a2:1::8801', 'hall', 'zzz']"
            ".map((q) => L.rows(D, 'all', q, {c100000000008801: 'Hall light'}, SRP).map((d) => d.id))"
        )
        assert got == [
            ["c40961"],
            ["c23553"],
            ["c23553"],
            ["c40961"],
            ["c34817"],
            ["c34817"],
            [],
        ]

    def test_rows_query_matches_a_name_that_is_markup_literally(self):
        # On a query: the SRP host name is compared as plain text.
        assert js("L.rows(D, 'all', '<b>', {}, SRP).map((d) => d.id)") == ["c34818"]

    # layout

    def test_layout_gives_identical_output_twice(self):
        # On the same input twice: the same coordinates.
        got = js("JSON.stringify(L.layout(D, LK)) === JSON.stringify(L.layout(D, LK))")
        assert got is True

    def test_layout_puts_this_board_at_the_centre(self):
        # On any input: this board is the centre the other routers surround.
        others = js(
            "(() => { const m = L.layout(D, LK); const c = m.pos.r34;"
            " return ['r9', 'r23', 'r40', 'r53', 'r55'].map((id) => Math.hypot(m.pos[id].x - c.x, m.pos[id].y - c.y)); })()"
        )
        assert all(d > 60 for d in others)

    def test_layout_centres_the_leader_when_this_board_is_a_child(self):
        # On this board being a child: the leader is the centre (nearest to the middle of the routers).
        got = js(
            "(() => { const m = L.layout(DB, L.routerLinks(B)); const ids = ['r23', 'r34', 'r40', 'r53', 'r55'];"
            " const c = m.pos.r9; const xs = Object.keys(m.pos).map((k) => m.pos[k].x);"
            " const cx = (Math.min(...xs) + Math.max(...xs)) / 2;"
            " return [ids.every((id) => Math.hypot(m.pos[id].x - c.x, m.pos[id].y - c.y) > 60), m.pos.c40962 !== undefined]; })()"
        )
        assert got == [True, True]

    def test_layout_keeps_every_child_within_its_parents_fan(self):
        # On any input: every child is within the fan radius of its parent.
        got = js(
            "(() => { const m = L.layout(D, LK); return D.filter((d) => d.parentId)"
            ".map((d) => Math.hypot(m.pos[d.id].x - m.pos[d.parentId].x, m.pos[d.id].y - m.pos[d.parentId].y)); })()"
        )
        assert len(got) == 7
        assert all(40 <= d <= 160 for d in got)

    def test_layout_never_goes_below_the_minimum_size_and_grows_with_the_node_count(
        self,
    ):
        # On any input: at least 320x200, positions inside the map, and a bigger mesh gives a bigger map.
        got = js(
            "(() => { const small = L.layout(D.slice(0, 1), []); const m = L.layout(D, LK);"
            " const many = []; for (let i = 0; i < 16; i++) { many.push({id: 'r' + i, kind: 'router', rloc16: i << 10,"
            " routerId: i, parentId: null, thisBoard: i === 0, leader: false, addrs: []}); }"
            " const big = L.layout(many, []);"
            " const inside = Object.keys(m.pos).every((k) => m.pos[k].x >= 0 && m.pos[k].x <= m.width && m.pos[k].y >= 0 && m.pos[k].y <= m.height);"
            " return [small.width, small.height, m.width >= 320, m.height >= 200, inside, big.width > m.width && big.height > m.height]; })()"
        )
        assert got == [320, 200, True, True, True, True]

    def test_layout_of_no_devices_is_the_minimum_empty_map(self):
        # On no devices: 320x200, no positions and no Home-network box.
        assert plain("L.layout([], [])") == {
            "width": 320,
            "height": 200,
            "pos": {},
            "home": None,
            "ring": 0,
        }

    def test_layout_of_the_fixture_fits_800_by_600(self):
        # On the fixture (13 nodes), with the board a router or a child: the map is at most 800x600.
        got = js(
            f"[[D, LK], [DB, L.routerLinks(B)]].map(([ds, lk]) => {{ const m = L.layout(ds, lk, {LABELS}(ds));"
            " return [m.width, m.height]; })"
        )
        assert all(w <= 800 and h <= 600 for w, h in got), got

    def test_layout_puts_the_home_box_next_to_the_board(self):
        # On a board that is a router or a child: the Home-network box centre is within 120 px of the board.
        got = js(
            f"[[D, LK, 'r34'], [DB, L.routerLinks(B), 'c40962']].map(([ds, lk, id]) => {{"
            f" const m = L.layout(ds, lk, {LABELS}(ds)); return Math.hypot(m.home.x - m.pos[id].x, m.home.y - m.pos[id].y); }})"
        )
        assert all(d <= 120 for d in got), got

    def test_layout_keeps_a_linkless_stub_on_the_ring(self):
        # On a router without links (the stub 0xdc00): no farther from the centre than the ring.
        got = js(
            f"(() => {{ const m = L.layout(D, LK, {LABELS}(D));"
            " return [Math.hypot(m.pos.r55.x - m.pos.r34.x, m.pos.r55.y - m.pos.r34.y), m.ring]; })()"
        )
        assert got[0] <= got[1] + 1, got

    def test_layout_label_boxes_never_overlap(self):
        # On the fixture: no two label boxes (estimated width x 14 px, under each node) overlap.
        got = js(
            f"[[D, LK], [DB, L.routerLinks(B)]].map(([ds, lk]) => {{ const labels = {LABELS}(ds);"
            " const m = L.layout(ds, lk, labels);"
            " const box = (d) => { const w = L.mapLabel(labels[d.id]).length * (d.parentId === null ? 7 : 6.5);"
            "   const p = m.pos[d.id], y = p.y + L.nodeHalf(d) + 4; return [d.id, p.x - w / 2, p.x + w / 2, y, y + 14]; };"
            " const b = ds.map(box), hits = [];"
            " for (let i = 0; i < b.length; i++) for (let j = i + 1; j < b.length; j++)"
            "   if (b[i][1] < b[j][2] && b[j][1] < b[i][2] && b[i][3] < b[j][4] && b[j][3] < b[i][4])"
            "     hits.push(b[i][0] + '/' + b[j][0]);"
            " return hits; })"
        )
        assert got == [[], []]

    def test_nodehalf_counts_the_outer_ring_of_another_border_router(self):
        # On another border router: its half height reaches the orange ring outside the disc (and the leader ring).
        got = plain(
            "[{kind: 'router', parentId: null, leader: false}, {kind: 'router', parentId: null, leader: false, otherBr: true},"
            " {kind: 'leader', parentId: null, leader: true}, {kind: 'leader', parentId: null, leader: true, otherBr: true}]"
            ".map(L.nodeHalf)"
        )
        assert got == [16, 21, 21, 26]

    def test_home_box_clears_every_node_on_the_real_mesh(self):
        # On the real-mesh geometry (the old layout put the box over 0x3000 and its label): the Home box overlaps no
        # node or label footprint, and sits inside the map.
        got = js(f"{HOME_HITS}(L.devices(M), L.routerLinks(M))")
        assert got["hits"] == [], got
        assert got["inside"] is True, got

    def test_home_box_stays_off_every_link_parent_line_and_lqi_badge(self):
        # On the screenshot mesh (the old layout put the box across the board's links and two LQI badges) and the
        # other fixtures: no drawn line passes through the box and no LQI badge overlaps it.
        got = js(
            "[[L.devices(SH), L.routerLinks(SH)], [L.devices(M), L.routerLinks(M)], [D, LK], [DB, L.routerLinks(B)]]"
            f".map(([ds, lk]) => {HOME_ON_LINKS}(ds, lk))"
        )
        assert all(g == {"lines": 0, "badges": 0} for g in got), got

    def test_crosses_detects_a_segment_through_a_box_and_not_one_beside_it(self):
        # On a 100x20 box: a line across it, one ending inside, one passing 20 px above, one parallel 10 px beside.
        got = plain(
            "[[{x: -10, y: 10}, {x: 110, y: 10}], [{x: 50, y: -40}, {x: 50, y: 5}], [{x: -10, y: -20}, {x: 110, y: -20}],"
            " [{x: 120, y: -40}, {x: 120, y: 60}]].map(([a, b]) => L.crosses(a, b, {x0: 0, x1: 100, y0: 0, y1: 20}))"
        )
        assert got == [True, True, False, False]

    def test_home_box_is_the_nearest_free_spot_around_the_board(self):
        # On the fixtures: no free spot for the box (clear of every footprint and LQI badge by the layout's 6 px gap,
        # and off every line) is more than one ring step (12 px) closer to the board than the one taken.
        got = js(
            "[[L.devices(SH), L.routerLinks(SH)], [L.devices(M), L.routerLinks(M)], [D, LK], [DB, L.routerLinks(B)]]"
            f".map(([ds, lk]) => {HOME_NEAREST}(ds, lk))"
        )
        assert all(g["closer"] is None for g in got), got

    def test_home_box_crowded_on_every_side_still_finds_a_clear_spot_and_the_map_grows(
        self,
    ):
        # On a board ringed by fully linked routers and many children: the box overlaps nothing, lies inside the map,
        # and the map grew for it (see below).
        got = js(
            "(() => { const ds = [{id: 'r0', kind: 'br', rloc16: 0, routerId: 0, parentId: null, thisBoard: true,"
            " leader: false, addrs: []}];"
            " const lk = [];"
            " for (let i = 1; i <= 8; i++) { ds.push({id: 'r' + i, kind: 'router', rloc16: i << 10, routerId: i,"
            "   parentId: null, thisBoard: false, leader: i === 1, addrs: []}); lk.push({a: 'r0', b: 'r' + i, lqAB: 3, lqBA: 3}); }"
            " for (let i = 1; i <= 8; i++) for (let j = i + 1; j <= 8; j++) lk.push({a: 'r' + i, b: 'r' + j, lqAB: 3, lqBA: 3});"
            " for (let k = 1; k <= 14; k++) ds.push({id: 'c' + k, kind: 'med', rloc16: k, routerId: 0, parentId: 'r0',"
            "   thisBoard: false, leader: false, addrs: []});"
            f" return {HOME_HITS}(ds, lk); }})()"
        )
        assert got["hits"] == [], got
        assert got["inside"] is True, got
        # The map grew for the box: it sticks out of the footprint box of every node and label, and the map
        # encloses that grown box.
        box, nodes = got["box"], got["nodes"]
        grown_w = max(nodes["x1"], box["x1"]) - min(nodes["x0"], box["x0"])
        grown_h = max(nodes["y1"], box["y1"]) - min(nodes["y0"], box["y0"])
        assert got["width"] >= grown_w and got["height"] >= grown_h, got
        assert (
            grown_w > nodes["x1"] - nodes["x0"] or grown_h > nodes["y1"] - nodes["y0"]
        ), got

    def test_maplabel_cuts_long_names_with_an_ellipsis(self):
        # On a name longer than 18 characters: the first 17 and "…"; shorter names unchanged.
        assert plain(
            "[L.mapLabel('a-very-long-device-name'), L.mapLabel('This border router')]"
        ) == ["a-very-long-devic…", "This border router"]

    def test_board_map_label_is_the_logic_boards_label(self):
        # On the map: the board's node label is TopologyLogic's BOARD_LABEL, "This border router".
        r = run(
            "await loaded(); return [L.BOARD_LABEL,"
            " node('r34').findAll((e) => e.tag === 'text').map((e) => e.textContent)];"
        )
        assert r[0] == "This border router"
        assert r[0] in r[1], r

    def test_board_is_named_by_its_hostname_once_names_are_read(self):
        # On GET /device/names answering a hostname: the map label, the table row and the dialog title use it.
        r = run(
            "S.names = {status: 200, body: {hostname: 'otbr-attic', hostname_live: 'otbr-attic'}};"
            " await loaded(); const a = [node('r34').attrs['aria-label'], row('r34').allText()];"
            " await pick('r34'); a.push(txt('t-d-name')); return a;"
        )
        assert r[0] == "otbr-attic, Border router"
        assert "otbr-attic" in r[1] and "This border router" not in r[1], r
        assert r[2] == "otbr-attic"

    def test_board_keeps_the_generic_label_when_names_fail(self):
        # On GET /device/names failing: "This border router".
        r = run(
            "S.names = {status: 500, body: {}}; await loaded(); return node('r34').attrs['aria-label'];"
        )
        assert r == "This border router, Border router"

    def test_source_has_no_random_numbers(self):
        # On any input: the layout does not use Math.random.
        from device_js import STATIC

        assert "Math.random" not in (STATIC / "topology.js").read_text()

    # savedResult / saveResult

    def test_savedresult_returns_the_stored_result_and_time(self):
        # On a valid topology:last: {savedAt, result}.
        got = js(
            f"(() => {{ const s = {STORE}; L.saveResult(s, R, 1700000000000); const v = L.savedResult(s);"
            " return [v.savedAt, v.result.routers.length, JSON.parse(s.m['topology:last']).saved_at]; })()"
        )
        assert got == [1700000000000, 6, 1700000000000]

    def test_savedresult_is_null_for_missing_garbage_old_shape_or_throwing_storage(
        self,
    ):
        # On missing, unparsable, wrong-shaped data or a throwing storage: null.
        got = js(
            f"[{STORE}, ...['not json', '{{}}', '[]', 'null', '42', '{{\"saved_at\": 1, \"result\": 5}}',"
            ' \'{"saved_at": "x", "result": {"routers": [], "network": {}}}\','
            ' \'{"savedAt": 1, "result": {"routers": [], "network": {}}}\','
            " JSON.stringify({routers: [], network: {}}), 'null'].map((raw) => ({getItem: () => raw}))]"
            ".map((s) => L.savedResult(s))"
        )
        assert got == [None] * 11
        assert js(f"L.savedResult({BROKEN})") is None

    def test_saveresult_returns_false_when_storage_throws(self):
        # On a throwing storage: false, no exception.
        assert js(f"[L.saveResult({BROKEN}, R, 1), L.saveResult({STORE}, R, 1)]") == [
            False,
            True,
        ]

    # names

    def test_savename_stores_and_loadnames_reads_the_nickname(self):
        # On a nickname: stored under the ext address, returns true.
        got = js(
            f"(() => {{ const s = {STORE}; return [L.saveName(s, 'aa11', ' Hall light '), L.loadNames(s)]; }})()"
        )
        assert got == [True, {"aa11": "Hall light"}]

    def test_savename_with_an_empty_name_removes_the_nickname(self):
        # On an empty name: the nickname is removed; the others stay.
        got = js(
            f"(() => {{ const s = {STORE}; L.saveName(s, 'aa11', 'A'); L.saveName(s, 'bb22', 'B');"
            " const r = L.saveName(s, 'aa11', '  '); return [r, L.loadNames(s)]; })()"
        )
        assert got == [True, {"bb22": "B"}]

    def test_loadnames_is_empty_for_missing_garbage_or_throwing_storage(self):
        # On missing, unparsable or throwing storage: {}.
        got = js(
            f"[L.loadNames({STORE}), L.loadNames({{getItem: () => 'oops'}}), L.loadNames({{getItem: () => '[1]'}}),"
            f" L.loadNames({BROKEN})]"
        )
        assert got == [{}, {}, {}, {}]

    def test_savename_returns_false_when_storage_throws(self):
        # On a throwing storage: false, no exception.
        assert js(f"L.saveName({BROKEN}, 'aa11', 'x')") is False

    # pickLast

    def test_picklast_prefers_a_newer_board_result(self):
        # On a board result newer than the saved one: the board result, saved at nowMs - age_s*1000.
        got = js(
            "(() => { const v = L.pickLast({savedAt: 1000000, result: B}, {state: 'done', age_s: 60, result: R}, 2000000);"
            " return [v.savedAt, v.result === R]; })()"
        )
        assert got == [1940000, True]

    def test_picklast_keeps_the_saved_result_otherwise(self):
        # Otherwise: the saved one (or null).
        got = js(
            "[L.pickLast({savedAt: 1990000, result: B}, {state: 'done', age_s: 60, result: R}, 2000000).result === B,"
            " L.pickLast({savedAt: 5, result: B}, {state: 'running'}, 2000000).savedAt,"
            " L.pickLast({savedAt: 5, result: B}, {state: 'failed', age_s: 1}, 2000000).savedAt,"
            " L.pickLast(null, {state: 'idle'}, 2000000),"
            " L.pickLast(null, {state: 'done', age_s: 0, result: R}, 2000000).savedAt]"
        )
        assert got == [True, 5, 5, None, 2000000]

    # conflictText

    def test_conflicttext_names_the_mode_when_the_network_is_not_running(self):
        # On 409 with detail "mode X": "The Thread network isn't running (mode X)".
        assert plain(
            "L.conflictText(409, {error: 'busy', detail: 'mode DISABLED'})"
        ) == ("The Thread network isn't running (mode DISABLED)")

    def test_conflicttext_explains_diagnostics_in_use(self):
        # On 409 with detail "mesh diagnostics are in use": ask to try again.
        assert plain(
            "L.conflictText(409, {error: 'busy', detail: 'mesh diagnostics are in use'})"
        ) == ("Another mesh scan is running, try again in a few seconds")

    def test_conflicttext_passes_another_detail_through(self):
        # On 409 with another detail: that detail.
        assert plain(
            "L.conflictText(409, {error: 'busy', detail: 'firmware update in progress'})"
        ) == ("firmware update in progress")

    # pingSummary

    def test_pingsummary_reports_replies_loss_and_round_trip_times(self):
        # On replies: "R of N replies", loss %, min/avg/max ms.
        got = plain(
            "L.pingSummary({replies: [{rtt: 5}, {rtt: 7}, {rtt: 9}], statistics: {sent: 4, received: 3,"
            " loss_percent: 25, rtt_min: 5, rtt_avg: 7, rtt_max: 9}}, false)"
        )
        assert got == ["3 of 4 replies", "Loss 25%", "min/avg/max 5/7/9 ms"]

    def test_pingsummary_adds_the_sleepy_line_for_sleepy_devices(self):
        # On a sleepy device: the sleepy line comes last.
        got = plain(
            "L.pingSummary({replies: [], statistics: {sent: 4, received: 0, loss_percent: 100}}, true)"
        )
        assert got == [
            "0 of 4 replies",
            "Loss 100%",
            "Sleepy devices answer when they next wake up.",
        ]

    # clockText / agoText

    def test_clocktext_is_local_time_zero_padded(self):
        # On a time: local HH:MM, zero-padded.
        assert plain(
            "[L.clockText(new Date(2026, 10, 3, 9, 5).getTime()), L.clockText(new Date(2026, 10, 3, 23, 41).getTime())]"
        ) == [
            "09:05",
            "23:41",
        ]

    def test_agotext_picks_the_unit_by_age(self):
        # On ages: "just now" under 1 min, "N min ago" under 60 min, "N h ago" under 24 h, else "N d ago".
        got = plain(
            "[0, 59000, 60000, 38 * 60000, 59 * 60000, 60 * 60000, 5 * 3600000, 24 * 3600000, 50 * 3600000, -5000]"
            ".map((age) => L.agoText(1000000000000 - age, 1000000000000))"
        )
        assert got == [
            "just now",
            "just now",
            "1 min ago",
            "38 min ago",
            "59 min ago",
            "1 h ago",
            "5 h ago",
            "1 d ago",
            "2 d ago",
            "just now",
        ]

    # lqColor

    def test_lqcolor_maps_three_two_one_to_green_amber_red(self):
        # On LQI 3 / 2 / 1 / 0: green #1e7e34, amber #a35f00, red #c62828, grey #5f5f5f.
        assert plain("[3, 2, 1, 0].map(L.lqColor)") == [
            "#1e7e34",
            "#a35f00",
            "#c62828",
            "#5f5f5f",
        ]

    # txtEntries

    def test_txtentries_decodes_the_recorded_esphome_txt_in_order(self):
        # On the ESPHome TXT recorded 2026-11-04: every entry in order, friendly_name "C6 Thread Test".
        got = srp(f"L.txtEntries('{ESPHOME_TXT}')")
        assert [e["key"] for e in got] == [
            "friendly_name", "version", "platform", "board", "network", "mac", "project_name", "project_version",
            "config_hash", "api_encryption",
        ]  # fmt: skip
        assert got[0]["value"] == "C6 Thread Test"
        assert got[9]["value"] == "Noise_NNpsk0_25519_ChaChaPoly_SHA256"

    def test_txtentries_gives_a_key_only_entry_a_null_value(self):
        # On a key-only entry ("at"): {key, value: null}.
        assert srp(f"L.txtEntries('{ODD_TXT}')[0]") == {
            "key": "at",
            "value": None,
            "bytes": None,
        }

    def test_txtentries_keeps_an_empty_value_and_skips_an_entry_without_key(self):
        # On "empty=" and "=nokey": "" kept as a value, the entry without a key dropped; markup stays plain text.
        got = srp(f"L.txtEntries('{ODD_TXT}').map((e) => [e.key, e.value])")
        assert got == [["at", None], ["empty", ""], ["name", "<b>x</b>"]]

    def test_txtentries_gives_a_binary_value_as_bytes_with_hex_text(self):
        # On meshcop xa (binary): value as a byte array, L.hexText of it "7e287c8b8b140ee6".
        got = srp(
            f"((e) => [Array.isArray(e.value), e.value.length, L.hexText(e.value), L.hexText(e.bytes)])"
            f"(L.txtEntries('{MESHCOP_TXT}').find((e) => e.key === 'xa'))"
        )
        assert got == [True, 8, "7e287c8b8b140ee6", "7e287c8b8b140ee6"]

    def test_txtentries_keeps_the_raw_bytes_of_a_text_value(self):
        # On a text value: bytes holds its raw bytes too.
        assert srp(f"L.hexText(L.txtEntries('{MESHCOP_TXT}')[0].bytes)") == "31"

    def test_txtentries_of_empty_or_invalid_base64_is_empty(self):
        # On "", undefined, "AA=A", "@@@@" or a single zero-length string: [].
        got = plain(
            "['', undefined, null, 'AA=A', '@@@@', 'AA==', 'Zm9v!'].map((b) => L.txtEntries(b))"
        )
        assert got == [[], [], [], [], [], [], []]

    def test_txtentries_stops_at_a_length_running_past_the_end(self):
        # On a string length past the end: the entries before it, nothing read beyond.
        cut = base64.b64encode(b"\x03a=1\x09b=2").decode()
        assert srp(f"L.txtEntries('{cut}').map((e) => e.key)") == ["a"]

    # describe

    def test_describe_esphome_host(self):
        # On the ESPHome host: kind "ESPHome", name, version, board, platform, project, encrypted API, ports.
        got = srp("L.describe(host('c6-thread-test'))")
        assert {k: v for k, v in got.items() if k != "services"} == {
            "kind": "ESPHome", "name": "C6 Thread Test", "version": "2026.7.4", "board": "esp32-c6-devkitc-1",
            "platform": "ESP32", "project": "esp32otbr.srp-probe", "projectVersion": "0.1", "encrypted": True,
            "apiPort": 6053, "webPort": 80,
        }  # fmt: skip

    def test_describe_esphome_without_api_encryption_is_not_encrypted(self):
        # On an ESPHome TXT without api_encryption: encrypted false, webPort null without _http._tcp.
        plain_txt = txt("friendly_name=Plain", "version=2025.1.0")
        got = plain(
            f"L.describe({{name: 'p', services: [{{instance: 'p', type: '_esphomelib._tcp', port: 6053,"
            f" txt: '{plain_txt}'}}]}})"
        )
        assert [got["encrypted"], got["webPort"], got["board"]] == [False, None, None]

    def test_describe_matter_host_on_two_fabrics(self):
        # On a Matter host with two instances on two fabrics: kind "Matter", one {fabric, node} per instance,
        # SII/SAI/SAT in ms, ICD null, T from the record that has it.
        got = srp("L.describe(host('FEF6E5E01729FF7F'))")
        assert {k: v for k, v in got.items() if k != "services"} == {
            "kind": "Matter",
            "fabrics": [{"fabric": "CAC23A5DA6E7644C", "node": "000000000F4B0B5F"},
                        {"fabric": "3E5D425037C4F971", "node": "0000000000000003"}],
            "sii": 800, "sai": 2000, "sat": 4000, "icd": None, "tcp": 0, "commissionable": None,
        }  # fmt: skip

    def test_describe_matter_icd_when_present(self):
        # On a Matter TXT with ICD=1 (synthetic): icd 1.
        assert srp("L.describe(host('A1B2C3D4E5F60718')).icd") == 1

    def test_describe_matter_commissionable(self):
        # On a _matterc._udp record (synthetic): vendor id, product id, device type and device name.
        got = srp("L.describe(host('0E1F2A3B4C5D6E7F'))")
        assert [got["kind"], got["fabrics"], got["commissionable"]] == [
            "Matter",
            [],
            {
                "vendorId": 4447,
                "productId": 40961,
                "deviceType": 266,
                "deviceName": "Smart Plug",
            },
        ]

    def test_describe_homekit_host(self):
        # On a _hap._udp record (synthetic): model, category name from ci, paired from sf.
        got = srp("L.describe(host('eve-energy-1a2b'))")
        assert {k: v for k, v in got.items() if k != "services"} == {
            "kind": "HomeKit", "name": "Eve Energy 1A2B", "model": "Eve Energy 20EBO8301", "category": "Outlet",
            "paired": True,
        }  # fmt: skip

    def test_describe_homekit_category_names_cover_1_to_36(self):
        # On ci 1, 2, 36, 37 and sf=1: "Other", "Bridge", "TV Streaming Stick", null; sf=1 means not paired.
        got = plain(
            "[1, 2, 36, 37].map((ci) => L.describe({name: 'h', services: [{instance: 'h', type: '_hap._udp',"
            " txt: Buffer.from([3 + String(ci).length, ...Buffer.from('ci=' + ci), 4, ...Buffer.from('sf=1')])"
            ".toString('base64')}]})).map((d) => [d.category, d.paired])"
        )
        assert got == [
            ["Other", False],
            ["Bridge", False],
            ["TV Streaming Stick", False],
            [None, False],
        ]

    def test_describe_homekit_paired_is_null_without_sf(self):
        # On a _hap._udp record without sf: paired null (unknown), not false.
        got = plain(
            "L.describe({name: 'h', services: [{instance: 'h', type: '_hap._udp', txt: ''}]}).paired"
        )
        assert got is None

    def test_describe_meshcop_host(self):
        # On the recorded meshcop record: vendor, model, Thread version, extended address from xa.
        got = srp("L.describe(host('homeassistant'))")
        assert {k: v for k, v in got.items() if k != "services"} == {
            "kind": "Border router", "name": "Home Assistant OpenThread Border Router #0EE6",
            "vendor": "Home Assistant", "model": "OpenThread Border Router", "threadVersion": "1.4.0",
            "ext": "7e287c8b8b140ee6",
        }  # fmt: skip

    def test_describe_unknown_type_keeps_the_raw_entries(self):
        # On an unknown service type: kind null, each service with its port, source and decoded entries.
        got = srp("L.describe(host('printer'))")
        assert got == {
            "kind": None,
            "services": [{"instance": "Printer", "type": "_ipp._tcp", "port": 631, "source": 26624,
                          "txtTruncated": False, "entries": [{"key": "rp", "value": "ipp/print",
                                                              "bytes": list(b"ipp/print")}]}],
        }  # fmt: skip

    def test_describe_lists_every_service_with_its_entries(self):
        # On the ESPHome host: both services, the _http._tcp one with no entries.
        got = srp(
            "L.describe(host('c6-thread-test')).services.map((s) => [s.type, s.port, s.entries.length])"
        )
        assert got == [["_esphomelib._tcp", 6053, 10], ["_http._tcp", 80, 0]]

    def test_describe_of_a_host_from_older_firmware(self):
        # On a host without txt or port fields: kind from the type alone, nothing decoded, no error.
        got = plain(
            "L.describe({name: 'kitchen-plug', services: [{instance: 'kitchen-plug', type: '_matter._tcp'}]})"
        )
        assert [
            got["kind"],
            got["fabrics"],
            got["sii"],
            got["services"][0]["port"],
        ] == ["Matter", [], None, None]

    # srpHostsFor

    def test_srphostsfor_matches_by_address(self):
        # On a host sharing an address: that host.
        assert srp("L.srpHostsFor(dv('r1'), HOSTS).map((h) => h.name)") == [
            "c6-thread-test"
        ]

    def test_srphostsfor_matches_a_host_named_after_the_ext_address(self):
        # On a host name equal to the ext address in any case: that host.
        got = srp(
            "[L.srpHostsFor(dv('r2'), HOSTS), L.srpHostsFor({...dv('r2'), ext: 'FEF6E5E01729FF7F'}, HOSTS)]"
            ".map((l) => l.map((h) => h.name))"
        )
        assert got == [["FEF6E5E01729FF7F"], ["FEF6E5E01729FF7F"]]

    def test_srphostsfor_matches_meshcop_or_trel_xa(self):
        # On a _meshcop._udp or _trel._udp service whose xa is the ext address: that host.
        trel = txt(b"xa=" + bytes.fromhex("7e287c8b8b140ee6"))
        got = srp(
            f"[L.srpHostsFor({{...dv('r3'), addrs: []}}, HOSTS),"
            f" L.srpHostsFor({{...dv('r3'), addrs: []}}, [{{name: 't', addresses: [], services: [{{instance: 't',"
            f" type: '_trel._udp', txt: '{trel}'}}]}}]),"
            f" L.srpHostsFor({{...dv('r3'), addrs: []}}, [{{name: 'h', addresses: [], services: [{{instance: 'h',"
            f" type: '_http._tcp', txt: '{trel}'}}]}}])].map((l) => l.map((h) => h.name))"
        )
        assert got == [["homeassistant"], ["t"], []]

    def test_srphostsfor_returns_every_match_this_boards_record_first(self):
        # On a border router with its own SRP host on this board and a meshcop host from its server: both, this board's
        # record (with a lease) first.
        assert srp("L.srpHostsFor(dv('r3'), HOSTS).map((h) => h.name)") == [
            "homeassistant-otbr",
            "homeassistant",
        ]

    def test_srphostsfor_without_a_match_is_empty(self):
        # On no match (or no hosts): [].
        assert srp(
            "[L.srpHostsFor(dv('r5'), HOSTS), L.srpHostsFor(dv('r5'), undefined)]"
        ) == [[], []]

    # displayName with service records

    def test_displayname_prefers_the_nickname_over_the_records(self):
        # On a nickname: the nickname.
        assert (
            srp("L.displayName(dv('r1'), {'4eeb2e6a619150e3': 'Test C6'}, HOSTS)")
            == "Test C6"
        )

    def test_displayname_uses_the_esphome_friendly_name(self):
        # On an ESPHome record: its friendly_name.
        assert srp("L.displayName(dv('r1'), {}, HOSTS)") == "C6 Thread Test"

    def test_displayname_uses_the_homekit_instance_name(self):
        # On a HomeKit record: its instance name.
        assert srp("L.displayName(dv('r4'), {}, HOSTS)") == "Eve Energy 1A2B"

    def test_displayname_uses_the_meshcop_instance_name(self):
        # On a meshcop record: its instance name, before the router's own SRP host name.
        assert (
            srp("L.displayName(dv('r3'), {}, HOSTS)")
            == "Home Assistant OpenThread Border Router #0EE6"
        )

    def test_displayname_of_a_host_named_after_the_ext_address_is_matter_device(self):
        # On only a host named after the ext address: "Matter device 0xNNNN".
        assert srp("L.displayName(dv('r2'), {}, HOSTS)") == "Matter device 0x3000"

    def test_displayname_of_a_host_named_after_the_ext_address_without_matter_records_is_unknown(
        self,
    ):
        # On only a host named after the ext address whose records are not Matter (none, or _http._tcp): "Unknown 0xNNNN".
        got = srp(
            "[[], [svc('x', '_http._tcp', 80, 1, '')]].map((services) => L.displayName(dv('r2'), {},"
            " [{name: 'FEF6E5E01729FF7F', addresses: [], services}]))"
        )
        assert got == ["Unknown 0x3000", "Unknown 0x3000"]

    def test_displayname_falls_back_to_the_srp_host_name(self):
        # On a host with no record that names the device: the host name.
        assert srp("L.displayName(dv('r7'), {}, HOSTS)") == "printer"

    def test_displayname_without_records_is_unknown_and_this_board_keeps_its_label(
        self,
    ):
        # On no match: "Unknown 0xNNNN"; this board stays "This border router" even with an SRP host of its own.
        assert srp(
            "[L.displayName(dv('r5'), {}, HOSTS), L.displayName(dv('r6'), {}, HOSTS)]"
        ) == [
            "Unknown 0x5c00",
            "This border router",
        ]

    def test_displayname_reads_name_sources_on_the_hosts_other_services(self):
        # On a host with _matter._tcp first and _hap._udp second: the HomeKit instance name, not the host name.
        got = srp(
            "L.displayName(dv('r3'), {}, [{name: 'bridge', addresses: dv('r3').addrs, services: ["
            f"svc('AAAAAAAAAAAAAAAA-0000000000000001', '_matter._tcp', 5540, 1, '{MATTER_TXT}'),"
            f" svc('Hue Bridge', '_hap._udp', 5001, 1, '{HOMEKIT_TXT}')]}}])"
        )
        assert got == "Hue Bridge"

    def test_displayname_prefers_esphome_over_homekit_across_hosts(self):
        # On two matched hosts, HomeKit first and ESPHome second: the ESPHome friendly_name wins (spec order).
        got = srp(
            "L.displayName(dv('r3'), {}, [{name: 'a', addresses: dv('r3').addrs, services: ["
            f"svc('Hue Bridge', '_hap._udp', 5001, 1, '{HOMEKIT_TXT}')]}},"
            " {name: 'b', addresses: dv('r3').addrs, services: ["
            f"svc('b', '_esphomelib._tcp', 6053, 1, '{ESPHOME_TXT}')]}}])"
        )
        assert got == "C6 Thread Test"

    # isUnnamed

    def test_isunnamed_is_true_for_the_generic_matter_name(self):
        # On a device shown as "Matter device 0xNNNN" or "Unknown 0xNNNN": unnamed.
        assert srp(
            "[L.isUnnamed(dv('r2'), {}, HOSTS), L.isUnnamed(dv('r5'), {}, HOSTS)]"
        ) == [True, True]

    def test_isunnamed_is_false_for_a_device_with_a_real_name(self):
        # On an ESPHome friendly_name, a meshcop name, a plain SRP host name: named.
        assert srp(
            "['r1', 'r3', 'r7'].map((id) => L.isUnnamed(dv(id), {}, HOSTS))"
        ) == [False, False, False]

    def test_isunnamed_is_false_with_a_nickname_and_for_this_board(self):
        # On a nickname on a generic device, and on this board: named.
        assert srp(
            "[L.isUnnamed(dv('r2'), {'fef6e5e01729ff7f': 'Plug'}, HOSTS), L.isUnnamed(dv('r6'), {}, HOSTS)]"
        ) == [False, False]

    # srpIndex

    def test_srpindex_decodes_each_host_once_for_many_lookups(self):
        # On an index built once, then displayName, rows and srpHostsFor repeatedly: the hosts' services are read once.
        got = srp(
            "(() => { let reads = 0; const counted = HOSTS.map((h) => ({...h, get services() { reads++;"
            " return h.services; }})); const idx = L.srpIndex(counted); const built = reads;"
            " for (let i = 0; i < 5; i++) { DEVS.forEach((d) => L.displayName(d, {}, idx));"
            " L.rows(DEVS, 'all', 'thread', {}, idx); DEVS.forEach((d) => L.srpHostsFor(d, idx)); }"
            " return [built, reads - built, built <= HOSTS.length * 2]; })()"
        )
        assert got[1] == 0 and got[2] is True

    def test_srpindex_of_an_index_is_the_index(self):
        # On an index passed to srpIndex: itself.
        assert (
            srp(
                "(() => { const i = L.srpIndex(HOSTS); return L.srpIndex(i) === i; })()"
            )
            is True
        )

    # kindLine

    def test_kindline_per_kind(self):
        # On each kind: the table's kind line (mockup section 1).
        got = srp(
            "['c6-thread-test', 'FEF6E5E01729FF7F', 'eve-energy-1a2b', 'homeassistant', 'printer']"
            ".map((n) => L.kindLine(L.describe(host(n))))"
        )
        assert got == [
            "ESPHome 2026.7.4 · esp32-c6-devkitc-1",
            "Matter · 2 fabrics",
            "HomeKit · Outlet · paired",
            "Border router · Home Assistant · Thread 1.4.0",
            "",
        ]

    def test_kindline_of_a_homekit_host_without_sf_says_nothing_about_pairing(self):
        # On a HomeKit record without sf: paired is null and the line leaves pairing out.
        got = srp(
            "(() => { const s = L.describe({name: 'h', services: [svc('Lamp', '_hap._udp', 1, 1, '')]});"
            " return [s.paired, L.kindLine(s)]; })()"
        )
        assert got == [None, "HomeKit"]

    # device info (stackInfo, infoName, appUrl, nameInfo, kindLine, matches)

    def test_stackinfo_names_the_vendor_chip_sdk_and_version_of_a_known_stack(self):
        # On the three recorded stack strings: vendor, chip, sdk and version (null when the string has none).
        got = plain(
            "[STACKS.sl, STACKS.esp, STACKS.posix].map((s) => L.stackInfo(s))".replace(
                "STACKS.sl", repr(STACK_SL)
            )
            .replace("STACKS.esp", repr(STACK_ESP))
            .replace("STACKS.posix", repr(STACK_POSIX))
        )
        assert got == [
            {
                "vendor": "Silicon Labs",
                "chip": "EFR32",
                "sdk": "SL-OpenThread",
                "version": "2.4.2.0",
            },
            {
                "vendor": "Espressif",
                "chip": "esp32c6",
                "sdk": "OpenThread",
                "version": None,
            },
            {
                "vendor": "OpenThread",
                "chip": "POSIX",
                "sdk": "OpenThread",
                "version": None,
            },
        ]

    def test_stackinfo_of_an_unknown_empty_or_absent_string_is_null(self):
        got = plain(
            "['Vendor/1.0; x; y', '', null, undefined, 5].map((s) => L.stackInfo(s) === null)"
        )
        assert got == [True] * 5

    def test_infoname_joins_vendor_and_model(self):
        got = plain(
            "[{vendor_name: 'Home Assistant', vendor_model: 'OpenThread Border Router'},"
            " {vendor_name: 'RD:Acme', vendor_model: 'Lamp'},"
            " {vendor_name: 'Acme'}, {vendor_name: 'RD:', vendor_model: 'Lamp'}, {vendor_model: 'Lamp'}, null,"
            " {state: 'no_answer'}].map((i) => L.infoName(i))"
        )
        assert got == [
            "Home Assistant OpenThread Border Router",
            "Acme Lamp",
            None,
            None,
            None,
            None,
            None,
        ]

    def test_nameinfo_uses_the_info_name_but_keeps_the_boards_hostname(self):
        got = plain(
            "(() => {"
            "const d = (info, board) => ({id: 'r1', kind: 'router', rloc16: 0x1000, ext: null, thisBoard: board, info});"
            "const info = {state: 'answered', vendor_name: 'Home Assistant', vendor_model: 'OTBR'};"
            "return [L.displayName(d(info, false), {}, [], null), L.displayName(d(null, false), {}, [], null),"
            " L.isUnnamed(d(info, false), {}, [], null), L.displayName(d(info, true), {}, [], 'myhost')]; })()"
        )
        assert got == ["Home Assistant OTBR", "Unknown 0x1000", False, "myhost"]

    def test_nameinfo_prefers_an_srp_name_over_the_info_name(self):
        got = srp(
            "(() => { const d = DEVS.find((x) => !x.thisBoard && !L.isUnnamed(x, {}, HOSTS, null));"
            " const w = {...d, info: {state: 'answered', vendor_name: 'Acme', vendor_model: 'Lamp'}};"
            " return [L.displayName(w, {}, HOSTS, null) === L.displayName(d, {}, HOSTS, null),"
            " L.displayName(w, {}, HOSTS, null)]; })()"
        )
        assert got[0] is True
        assert got[1] != "Acme Lamp"

    def test_nameinfo_info_name_beats_matter_device_fallback(self):
        got = srp(
            "(() => { const m = DEVS.find((d) => L.displayName(d, {}, HOSTS, null).startsWith('Matter device'));"
            " const w = {...m, info: {state: 'answered', vendor_name: 'Acme', vendor_model: 'Lamp'}};"
            " return [L.displayName(w, {}, HOSTS, null), L.isUnnamed(w, {}, HOSTS, null), L.isUnnamed(m, {}, HOSTS, null)]; })()"
        )
        assert got == ["Acme Lamp", False, True]

    def test_kindline_falls_back_to_the_stack_summary_without_an_srp_kind(self):
        got = plain(
            "(() => { const dev = (s) => ({info: {state: 'answered', thread_stack_version: s}});"
            f"return [L.kindLine(null, dev({STACK_SL!r})), L.kindLine({{kind: null}}, dev({STACK_ESP!r})),"
            " L.kindLine({kind: 'Matter', fabrics: [1], commissionable: false}, dev("
            f"{STACK_SL!r})), L.kindLine(null, dev('x')), L.kindLine(null, {{}}), L.kindLine(null)]; }})()"
        )
        assert got == [
            "Silicon Labs EFR32",
            "Espressif esp32c6",
            "Matter · 1 fabric",
            "",
            "",
            "",
        ]

    def test_matches_finds_a_device_by_vendor_name_model_or_stack_string(self):
        got = plain(
            "(() => { const d = {id: 'r1', rloc16: 0x1000, ext: null, addrs: [], parentId: null, info: {state: 'answered',"
            " vendor_name: 'Acme', vendor_model: 'Lamp', thread_stack_version: 'SL-OPENTHREAD/2.4; EFR32; x'}};"
            "return ['acme', 'lamp', 'efr32', 'nope'].map((q) => L.rows([d], 'all', q, {}, [], null).length); })()"
        )
        assert got == [1, 1, 1, 0]

    def test_appurl_accepts_only_http_and_https(self):
        got = plain(
            "['https://example.com/a', 'HTTP://example.com', 'javascript:alert(1)', 'data:text/html,x',"
            " 'example.com', '', null, ' https://x.y'].map((u) => L.appUrl(u))"
        )
        assert got == [
            "https://example.com/a",
            "HTTP://example.com",
            None,
            None,
            None,
            None,
            None,
            None,
        ]

    def test_devices_carry_info_through_and_default_it_to_null(self):
        got = js(
            "(() => { const X = {...R, routers: routers()}; X.routers[0].info = {state: 'answered', eui64: 'ab'};"
            " X.routers[0].children[0].info = {state: 'no_answer'};"
            " const d = L.devices(X); const e = L.devices(R);"
            " return [d[0].info, d[1].info, e[0].info]; })()"
        )
        assert got == [
            {"state": "answered", "eui64": "ab"},
            {"state": "no_answer"},
            None,
        ]

    # omrAddress

    def test_omraddress_is_the_first_address_under_an_omr_prefix(self):
        # On addresses with a mesh-local one first: the first under an OMR prefix.
        got = plain(
            "L.omrAddress(['fdde:ad00:beef::1', 'fe80::1', 'fd87:9b49:8264:1::7', 'fd87:9b49:8264:1::8'],"
            " {mesh_local_prefix: 'fdde:ad00:beef::/64', omr_prefixes: ['fd87:9b49:8264:1::/64']})"
        )
        assert got == "fd87:9b49:8264:1::7"

    def test_omraddress_without_a_match_is_null(self):
        # On no address under an OMR prefix, or no prefixes: null.
        got = plain(
            "[L.omrAddress(['fdde:ad00:beef::1'], {omr_prefixes: ['fd87:9b49:8264:1::/64']}),"
            " L.omrAddress(['fd87:9b49:8264:1::7'], {}), L.omrAddress(undefined, {})]"
        )
        assert got == [None, None, None]

    # rows search with service records

    def test_rows_query_matches_the_esphome_friendly_name(self):
        # On a query in the ESPHome friendly_name, also when a nickname hides it: that device.
        got = srp(
            "[L.rows(DEVS, 'all', 'thread test', {}, HOSTS), L.rows(DEVS, 'all', 'C6 THREAD',"
            " {'4eeb2e6a619150e3': 'Lab'}, HOSTS)].map((l) => l.map((d) => d.id))"
        )
        assert got == [["r1"], ["r1"]]

    def test_rows_query_matches_any_matched_host_name(self):
        # On a query in the name of a second matched host: that device.
        assert srp(
            "L.rows(DEVS, 'all', 'homeassistant-otbr', {}, HOSTS).map((d) => d.id)"
        ) == ["r3"]


# ---- TopologyPage ----

# Fake routes: GET /device/topology answers S.gets in turn (the last one repeats; 'drop' drops the connection), POST
# answers S.post, /ping S.ping, /device/names S.names.
PAGE_ROUTES = """(u, o) => {
  if (u === '/device/topology') {
    if (o.method === 'POST')
      return S.post;
    const g = S.gets.length > 1 ? S.gets.shift() : S.gets[0];
    if (g === 'drop')
      throw new Error('connection dropped');
    return {status: 200, body: g};
  }
  if (u === '/ping')
    return {status: 200, body: S.ping};
  if (u === '/device/names')
    return S.names;
  return {status: 200, body: H.NETWORK};
}"""

PRELUDE = (
    "const L = t.Logic;"
    + FIXTURE
    + """
const RF = {...R, partial: false};
const RUNNING = (queried, routers, cq, ct) =>
  ({state: 'running', progress: {routers, queried, ...(ct === undefined ? {} : {children: {total: ct, queried: cq}})}});
const DONE = (result, age) => ({state: 'done', age_s: age || 0, result});
const S = {gets: [{state: 'idle'}], post: {status: 202, body: RUNNING(0, 0)}, names: {status: 404, body: {}},
           ping: {error: 0, message: 'OK', result: {replies: [{rtt: 5}, {rtt: 7}, {rtt: 9}],
                  statistics: {sent: 4, received: 3, loss_percent: 25, rtt_min: 5, rtt_avg: 7, rtt_max: 9}}}};
const calls = (method, url) => t.calls.filter((c) => c.method === method && c.url === url);
const vis = (id) => !t.el(id).hidden();
const txt = (id) => t.el(id).allText();
const node = (id) => t.el('t-map').findAll((e) => e.tag === 'g' && e.attrs['data-id'] === id)[0];
const ring = (id) => node(id).findAll((e) => e.classes.has('t-ring'))[0];
const row = (id) => t.el('t-rows').children.find((r) => r.attrs['data-id'] === id);
const buttons = (root, label) => root.findAll((e) => e.tag === 'button' && e.textContent === label);
const save = (ago, result) => {
  t.storage.m['topology:last'] = JSON.stringify({saved_at: t.env.now() - ago, result: result || R});
};
const start = async () => { t.page.init(); await t.advance(0); };
const loaded = async () => { save(60000); await start(); t.el('t-show-last').click(); await t.flush(); };
const pick = async (id) => { row(id).children[0].children[0].click(); await t.flush(); };
"""
)


def run(body):
    """Run `body` (async JS) on the topology page with the fixture, the fake routes and the helpers in scope."""
    return scenario(page("topology", PAGE_ROUTES) + PRELUDE + body)


FAILED_STOPPED = "The scan stopped: the Thread network went down"
NAMES_NOTE = "This browser can't save names. They last until the page is reloaded."
NO_ROUTE = (
    "No routable address yet. Hosts on your home network can't reach this device."
)
CLOSE_BY = {
    "x": "t.el('t-d-x').click();",
    "close": "t.el('t-d-close').click();",
    "esc": "t.el('t-dialog').fire('cancel'); t.el('t-dialog').close();",
    "backdrop": "t.el('t-dialog').click();",
}


# ---- TopologyPage with service records ----

# The 2026-11-04 mesh in SRP_FIXTURE's terms: this board 0x6800 (SRP server of the ESPHome device 0x6400), the HA border
# router 0x1000 (SRP server of the Matter device 0x3000's proxied records), and the page helpers fact(label) and
# openSV(id). SERVERS is the scan's srp_servers.
SV_FIXTURE = r"""
const SERVERS = [
  {rloc16: 0x6800, address: 'fd87:9b49:8264:1::6800', port: 53, this_board: true, state: 'answered', hosts: 4},
  {rloc16: 0x1000, address: 'fd87:9b49:8264:1:aaaa::1000', port: 53, this_board: false, state: 'answered', hosts: 2}];
const sr = (id, rloc16, ext, addrs, extra) =>
  srouter(rloc16, id, [[26, 3]], [], {ext_address: ext, addresses: addrs, ...extra});
const SV = {partial: false, srp_hosts: HOSTS, srp_servers: SERVERS,
  network: {...NET, mesh_local_prefix: 'fdde:ad00:beef::/64', omr_prefixes: ['fd87:9b49:8264:1::/64'],
            leader_router_id: 4, this_rloc16: 0x6800},
  routers: [
    sr(4, 0x1000, '7e287c8b8b140ee6', ['fd87:9b49:8264:1:aaaa::1000'], {leader: true, border_router: true}),
    sr(12, 0x3000, 'fef6e5e01729ff7f', []),
    sr(25, 0x6400, '4eeb2e6a619150e3', ['fd87:9b49:8264:1:c158:979b:6560:a15f']),
    srouter(0x6800, 26, [[4, 3], [12, 3], [25, 3]], [],
            {ext_address: 'bb00000000006800', addresses: ['fd87:9b49:8264:1::6800'], this_device: true,
             border_router: true})]};
const showSV = async (result) => { save(60000, result || SV); await start(); t.el('t-show-last').click(); await t.flush(); };
const openSV = async (id, result) => { await showSV(result); await pick(id); };
const fact = (label) => {
  const k = t.el('t-d-facts').children[0].children;
  const i = k.findIndex((e) => e.tag === 'dt' && e.textContent === label);
  return i < 0 ? null : k[i + 1].allText();
};
const kind = (id) => row(id).children[0].findAll((e) => e.classes.has('t-kind')).map((e) => e.textContent);
const rawBtn = () => t.el('t-d-raw').findAll((e) => e.tag === 'button')[0];
const rawTables = () => t.el('t-d-raw').findAll((e) => e.tag === 'table');
const rawRows = (tbl) => tbl.findAll((e) => e.tag === 'tr').map((r) => r.children.map((c) => c.textContent));
"""

BOLD_TXT = txt("friendly_name=<b>Hi</b>", "at", b"bin=\x00\xff", "empty=")


def sv(body):
    return run(SRP_FIXTURE + SV_FIXTURE + body)


@needs_node
class TestTopologyPage:
    """topology.js run against a fake DOM, fetch, clock, localStorage and clipboard"""

    # init

    def test_open_without_saved_result_shows_only_load_topology(self):
        # On open with no saved result and board idle: the start view with only Load topology and the
        # "No saved result" line; one GET, no POST.
        r = run(
            "await start(); return [vis('t-start'), vis('t-loading'), vis('t-loaded'), vis('t-show-last'),"
            " vis('t-load'), txt('t-last-line'), calls('GET', '/device/topology').length,"
            " calls('POST', '/device/topology').length];"
        )
        assert r == [
            True,
            False,
            False,
            False,
            True,
            "No saved result in this browser yet.",
            1,
            0,
        ]

    def test_open_with_saved_result_offers_show_last_result(self):
        # On open with a saved result: Show last result and Load topology with the "Last result" line.
        r = run(
            "save(38 * 60000); await start(); return [vis('t-show-last'), vis('t-load'), txt('t-last-line'),"
            " L.clockText(t.env.now() - 38 * 60000)];"
        )
        assert r[:2] == [True, True]
        assert r[2] == f"Last result: {r[3]} (38 min ago), 6 routers and 7 end devices"

    def test_open_while_running_shows_loading_and_polls_get(self):
        # On open while the board is running: the loading view, then a GET every second.
        r = run(
            "S.gets = [RUNNING(0, 0), RUNNING(2, 5), RUNNING(2, 5)]; await start();"
            " const a = [vis('t-loading'), vis('t-start'), txt('t-progress')]; await t.advance(1000);"
            " a.push(txt('t-progress'), calls('GET', '/device/topology').length); await t.advance(1000);"
            " a.push(calls('GET', '/device/topology').length); return a;"
        )
        assert r == [
            True,
            False,
            "Finding routers…",
            "Asking each router about its children: 2 of 5",
            2,
            3,
        ]

    def test_open_while_running_sends_no_post(self):
        # On open while the board is running (RF4): no POST, only GETs, however long the scan runs.
        r = run(
            "S.gets = [RUNNING(1, 5)]; await start(); await t.advance(10000);"
            " return [calls('POST', '/device/topology').length, calls('GET', '/device/topology').length];"
        )
        assert r[0] == 0
        assert r[1] == 11

    # Load topology

    def test_load_posts_shows_loading_and_polls_every_second(self):
        # On click: POST, the loading view, then a GET every second while running.
        r = run(
            "await start(); S.gets = [RUNNING(1, 5), RUNNING(3, 5)]; t.el('t-load').click(); await t.flush();"
            " const gets = () => calls('GET', '/device/topology').length;"
            " const a = [calls('POST', '/device/topology').length, vis('t-loading'), gets()];"
            " await t.advance(999); a.push(gets()); await t.advance(1); a.push(gets(), txt('t-progress'));"
            " await t.advance(1000); a.push(gets(), txt('t-progress')); return a;"
        )
        assert r == [
            1,
            True,
            1,
            1,
            2,
            "Asking each router about its children: 1 of 5",
            3,
            "Asking each router about its children: 3 of 5",
        ]

    def test_done_stores_the_result_and_shows_the_green_pill(self):
        # On done: topology:last stored, the loaded view with the green pill "Loaded at HH:MM", polling stops.
        r = run(
            "await start(); S.gets = [DONE(RF, 0)]; t.el('t-load').click(); await t.advance(1000);"
            " const s = JSON.parse(t.storage.m['topology:last']); const n = calls('GET', '/device/topology').length;"
            " await t.advance(5000);"
            " return [s.saved_at === t.env.now() - 5000, s.result.network.name, vis('t-loaded'), vis('t-loading'),"
            " txt('t-pill'), t.el('t-pill').classes.has('t-pill-live'), txt('t-reload'),"
            " calls('GET', '/device/topology').length - n, L.clockText(t.env.now() - 5000)];"
        )
        assert r[:4] == [True, "MyThread", True, False]
        assert r[4] == f"Loaded at {r[8]}"
        assert r[5:8] == [True, "Load again", 0]

    def test_done_post_reply_up_to_five_seconds_old_counts_as_a_fresh_load(self):
        # On a POST answered with done and age_s 5: the green pill "Loaded at HH:MM" at the board's end time.
        r = run(
            "await start(); S.post = {status: 202, body: DONE(RF, 5)}; t.el('t-load').click(); await t.flush();"
            " return [vis('t-loaded'), txt('t-pill'), t.el('t-pill').classes.has('t-pill-live'),"
            " t.el('t-pill').classes.has('t-pill-saved'), txt('t-reload'), L.clockText(t.env.now() - 5000)];"
        )
        assert r[0] is True
        assert r[1] == f"Loaded at {r[5]}"
        assert r[2:5] == [True, False, "Load again"]

    def test_done_post_reply_older_than_five_seconds_shows_as_saved_at_its_real_time(
        self,
    ):
        # On a POST answered with done and age_s 6 or more: the amber pill "Saved result from HH:MM, N min ago" at the
        # board's end time, not the green "Loaded at".
        r = run(
            "await start(); S.post = {status: 202, body: DONE(RF, 600)}; t.el('t-load').click(); await t.flush();"
            " const a = [vis('t-loaded'), txt('t-pill'), t.el('t-pill').classes.has('t-pill-saved'),"
            " t.el('t-pill').classes.has('t-pill-live'), txt('t-reload'), L.clockText(t.env.now() - 600000)];"
            " S.post = {status: 202, body: DONE(RF, 6)}; t.el('t-reload').click(); await t.flush();"
            " a.push(t.el('t-pill').classes.has('t-pill-saved')); return a;"
        )
        assert r[0] is True
        assert r[1] == f"Saved result from {r[5]}, 10 min ago"
        assert r[2:5] == [True, False, "Load fresh"]
        assert r[6] is True

    def test_followed_scan_done_by_polling_is_live_whatever_its_age(self):
        # On a scan the page followed while running whose done poll arrives with age_s 20: the green pill (the age
        # rule is for the POST's own reply only).
        r = run(
            "await start(); S.post = {status: 202, body: RUNNING(0, 0)};"
            " S.gets = [RUNNING(1, 2), DONE(RF, 20)]; t.el('t-load').click(); await t.advance(3000);"
            " return [vis('t-loaded'), t.el('t-pill').classes.has('t-pill-live'),"
            " t.el('t-pill').classes.has('t-pill-saved')];"
        )
        assert r == [True, True, False], r

    def test_failed_scan_shows_its_error_and_keeps_the_last_result(self):
        # On failed: the error in the start view; the last result stays.
        r = run(
            "save(60000); await start(); const before = t.storage.m['topology:last'];"
            " S.gets = [{state: 'failed', error: 'stopped', age_s: 0}]; t.el('t-load').click(); await t.advance(1000);"
            " return [vis('t-start'), txt('t-start-note'), vis('t-show-last'),"
            " t.storage.m['topology:last'] === before];"
        )
        assert r == [True, FAILED_STOPPED, True, True]

    def test_conflict_shows_conflicttext_in_the_start_view(self):
        # On 409: conflictText in the start view, no polling.
        r = run(
            "await start(); S.post = {status: 409, body: {error: 'busy', detail: 'mode SNIFFER'}};"
            " t.el('t-load').click(); await t.advance(3000);"
            " return [vis('t-start'), vis('t-loading'), txt('t-start-note'), calls('GET', '/device/topology').length];"
        )
        assert r == [True, False, "The Thread network isn't running (mode SNIFFER)", 1]

    def test_five_failed_polls_in_a_row_give_up_on_the_scan(self):
        # On 5 failed GETs in a row (with backoff): the start view says the board stopped answering, no more GETs,
        # and Load topology works again.
        r = run(
            "await start(); S.gets = ['drop']; t.el('t-load').click(); await t.advance(40000);"
            " const gets = () => calls('GET', '/device/topology').length;"
            " const a = [vis('t-start'), vis('t-loading'), txt('t-start-note'), gets()]; await t.advance(60000);"
            " a.push(gets()); t.el('t-load').click(); await t.flush(); a.push(calls('POST', '/device/topology').length);"
            " return a;"
        )
        assert r == [
            True,
            False,
            "The board stopped answering. Load again when it's back.",
            6,
            6,
            2,
        ]

    def test_board_restart_mid_scan_says_the_scan_was_interrupted(self):
        # On a GET answering idle while the page follows a scan (the board restarted): the start view says the scan
        # was interrupted, polling stops, and Load topology works again.
        r = run(
            "await start(); S.gets = [RUNNING(1, 5), {state: 'idle'}]; t.el('t-load').click(); await t.advance(2000);"
            " const gets = () => calls('GET', '/device/topology').length;"
            " const a = [vis('t-start'), vis('t-loading'), txt('t-start-note'), gets()]; await t.advance(5000);"
            " a.push(gets()); t.el('t-load').click(); await t.flush(); a.push(calls('POST', '/device/topology').length);"
            " return a;"
        )
        assert r == [
            True,
            False,
            "The scan was interrupted (the board restarted). Load again.",
            3,
            3,
            2,
        ]

    def test_a_success_after_four_failed_polls_carries_on(self):
        # On 4 failed GETs then answers: polling goes on and the scan ends in the loaded view.
        r = run(
            "await start(); S.gets = ['drop', 'drop', 'drop', 'drop', RUNNING(2, 5), DONE(RF, 0)];"
            " t.el('t-load').click(); await t.advance(31000);"
            " const a = [vis('t-loading'), txt('t-progress')]; await t.advance(1000);"
            " a.push(vis('t-loaded'), calls('GET', '/device/topology').length); return a;"
        )
        assert r == [
            True,
            "Asking each router about its children: 2 of 5",
            True,
            7,
        ]

    # Show last result

    def test_show_last_result_shows_the_amber_pill_without_a_request(self):
        # On click: the loaded view with the amber pill "Saved result from HH:MM, N min ago"; no request.
        r = run(
            "save(38 * 60000, RF); await start(); const n = t.calls.length; t.el('t-show-last').click();"
            " await t.flush(); return [vis('t-loaded'), txt('t-pill'), t.el('t-pill').classes.has('t-pill-saved'),"
            " txt('t-reload'), t.calls.length - n, L.clockText(t.env.now() - 38 * 60000)];"
        )
        assert r[0] is True
        assert r[1] == f"Saved result from {r[5]}, 38 min ago"
        assert r[2:5] == [True, "Load fresh", 0]

    # loaded view

    def test_partial_result_says_some_routers_didnt_answer(self):
        # On partial: the pill adds "some routers didn't answer".
        assert run("await loaded(); return txt('t-pill');").endswith(
            ", some routers didn't answer"
        )

    def test_summary_tiles_show_network_devices_leader_and_prefixes(self):
        # On a result: network, device counts, the leader and the OMR prefixes with their kind.
        r = run(
            "await loaded(); return ['t-tile-network', 't-tile-devices', 't-tile-leader', 't-tile-prefixes']"
            ".map(txt);"
        )
        assert r[0] == "Network MyThread Channel 15 · partition 0x4a1f2c07"
        assert r[1] == "Devices 13 6 routers · 7 end devices, 5 of them sleepy"
        assert r[2] == "Leader Unknown 0x2400 0x2400"
        assert (
            r[3]
            == "Routable prefixes 2001:db8:4a2:1::/64 global fd7a:91c3:22e0:1::/64 ULA"
        )

    def test_map_nodes_are_focusable_named_buttons(self):
        # On the map: each node is a <g tabindex=0 role=button> with an accessible name; the board is
        # "This border router".
        r = run(
            "await loaded(); return [node('r9').attrs, node('r34').attrs['aria-label'],"
            " t.el('t-map').findAll((e) => e.tag === 'g' && e.attrs.role === 'button').length];"
        )
        assert r[0]["tabindex"] == "0" and r[0]["role"] == "button"
        assert r[0]["aria-label"] == "Unknown 0x2400, Router"
        assert r[1] == "This border router, Border router"
        assert r[2] == 13

    def test_map_click_opens_the_dialog_for_that_device(self):
        # On a device clicked on the map: the dialog opens for it.
        r = run(
            "await loaded(); node('r23').click(); return [t.el('t-dialog').open, txt('t-d-name')];"
        )
        assert r == [True, "Unknown 0x5c00"]

    def test_map_node_opens_with_enter_and_space(self):
        # On Enter or Space on a focused node: the dialog opens; the key's default is prevented.
        r = run(
            "await loaded(); const a = []; for (const key of ['Enter', ' ']) { t.el('t-dialog').close();"
            " const ev = node('r9').fire('keydown', {key}); a.push(t.el('t-dialog').open, ev.defaultPrevented); }"
            " t.el('t-dialog').close(); node('r9').fire('keydown', {key: 'a'}); a.push(t.el('t-dialog').open);"
            " return a;"
        )
        assert r == [True, True, True, True, False]

    def test_table_name_opens_the_dialog_for_that_device(self):
        # On its name clicked in the table: the dialog opens for it.
        r = run(
            "await loaded(); await pick('c34817'); return [t.el('t-dialog').open, txt('t-d-name')];"
        )
        assert r == [True, "Unknown 0x8801"]

    def test_open_dialog_dims_everything_outside_the_neighbourhood(self):
        # On a device picked: everything but the device, its neighbours, parent and children is dimmed.
        r = run(
            "await loaded(); node('r23').click(); const dim = (id) => node(id).classes.has('t-dim');"
            " return ['r23', 'r9', 'r34', 'c23553', 'c23554', 'r40', 'r53', 'c34817'].map(dim);"
        )
        assert r == [False, False, False, False, False, True, True, True]

    def test_picked_device_row_is_tinted(self):
        # On a device picked: its row is tinted, no other.
        r = run(
            "await loaded(); node('r23').click(); return t.el('t-rows').children"
            ".filter((r) => r.classes.has('t-row-sel')).map((r) => r.attrs['data-id']);"
        )
        assert r == ["r23"]

    def test_filter_chip_rerenders_the_rows(self):
        # On a filter chip: only that kind is listed and the chip is pressed.
        r = run(
            "await loaded(); const chip = t.el('t-chips').children.find((c) => c.attrs['data-filter'] === 'sleepy');"
            " chip.click(); const now = t.el('t-chips').children.find((c) => c.attrs['data-filter'] === 'sleepy');"
            " return [t.el('t-rows').children.map((r) => r.attrs['data-id']), now.attrs['aria-pressed'],"
            " t.el('t-chips').children.map((c) => c.allText())];"
        )
        assert r[0] == ["c34818", "c34819", "c9217", "c23554", "c40961"]
        assert r[1] == "true"
        assert r[2] == ["All 13", "Routers 6", "End devices 7", "Sleepy 5"]

    def test_search_rerenders_the_rows_and_says_when_nothing_matches(self):
        # On search input: the matching rows; with no match "No device matches '<q>'".
        r = run(
            "await loaded(); t.el('t-search').type('kitchen');"
            " const a = [t.el('t-rows').children.map((r) => r.attrs['data-id']), vis('t-empty')];"
            " t.el('t-search').type('zzz'); a.push(t.el('t-rows').children.length, vis('t-empty'), txt('t-empty'));"
            " return a;"
        )
        assert r == [["c40961"], False, 0, True, "No device matches 'zzz'"]

    def test_copy_writes_the_address_and_reads_copied(self):
        # On Copy: the address goes to the clipboard and the button reads "Copied".
        r = run(
            "await loaded(); const b = buttons(row('c40961'), 'Copy')[0]; b.click(); await t.flush();"
            " return [t.clipboard.written, b.textContent];"
        )
        assert r == [["2001:db8:4a2:1:2be4:6c19:a0f7:3381"], "Copied"]

    def test_copy_failure_reads_copy_failed(self):
        # On a clipboard that refuses: the button reads "Copy failed".
        r = run(
            "await loaded(); t.clipboard.fail = true; const b = buttons(row('c40961'), 'Copy')[0]; b.click();"
            " await t.flush(); return b.textContent;"
        )
        assert r == "Copy failed"

    # dialog

    def test_neighbour_child_and_parent_links_switch_the_device(self):
        # On a neighbour, child or parent link: the dialog switches to that device and the map follows.
        r = run(
            "await loaded(); node('r40').click(); const a = [];"
            " buttons(t.el('t-d-neighbors'), 'Unknown 0x2400')[0].click(); a.push(txt('t-d-name'));"
            " buttons(t.el('t-d-children'), 'Unknown 0x2401')[0].click();"
            " a.push(txt('t-d-name'), node('r40').classes.has('t-dim'));"
            " buttons(t.el('t-d-facts'), 'Unknown 0x2400')[0].click();"
            " a.push(txt('t-d-name'), node('r40').classes.has('t-dim'), t.el('t-dialog').open); return a;"
        )
        assert r == [
            "Unknown 0x2400",
            "Unknown 0x2401",
            True,
            "Unknown 0x2400",
            False,
            True,
        ]

    def test_router_neighbours_show_both_link_directions(self):
        # On a router: neighbours with "LQI 3" or "LQI <in> in / <out> out" in its own view.
        r = run(
            "await loaded(); node('r9').click(); return t.el('t-d-neighbors').children.map((c) => c.allText());"
        )
        assert r == [
            "Unknown 0x5c00 LQI 2",
            "This border router LQI 3",
            "Unknown 0xa000 LQI 1 in / 3 out",
        ]

    def test_map_badges_agree_with_the_dialog_in_the_first_routers_view(self):
        # On differing sides: the badge reads in/out in the lower router's view, the same as its dialog
        # (r9 hears r40 at 1, r40 hears r9 at 3: badge "1/3", dialog "LQI 1 in / 3 out").
        r = run(
            "await loaded(); const badges = t.el('t-map').findAll((e) => e.classes.has('t-badge-text'))"
            ".map((e) => e.textContent); node('r9').click();"
            " return [badges, t.el('t-d-neighbors').children.map((c) => c.allText())[2]];"
        )
        assert r == [["2", "3", "1/3", "3/0", "2"], "Unknown 0xa000 LQI 1 in / 3 out"]

    def test_stub_link_shows_the_known_side_on_the_map_and_a_dash_in_the_dialog(self):
        # On a link to a router that didn't answer: the badge shows only the known number in its colour, and the
        # dialog shows "–" for the unknown side.
        r = run(
            "const X = {...R, routers: routers()}; X.routers[0].links.push({router_id: 55, lq_in: 3});"
            " save(60000, X); await start(); t.el('t-show-last').click(); await t.flush();"
            " const edge = t.el('t-map').findAll((e) => e.classes.has('t-link-edge'))"
            ".find((g) => g.findAll((e) => e.classes.has('t-badge-text'))[0].textContent === '3'"
            " && g.findAll((e) => e.tag === 'line')[0].attrs['stroke-dasharray'] === undefined"
            " && g.findAll((e) => e.tag === 'line')[0].attrs.stroke === '#1e7e34');"
            " const badges = t.el('t-map').findAll((e) => e.classes.has('t-badge-text')).map((e) => e.textContent);"
            " node('r9').click(); const a = [badges, !!edge, t.el('t-d-neighbors').children.map((c) => c.allText())];"
            " node('r55').click(); a.push(t.el('t-d-neighbors').children.map((c) => c.allText())); return a;"
        )
        assert r[0] == ["2", "3", "1/3", "3", "3/0", "2"]
        assert r[1] is True
        assert r[2][3] == "Unknown 0xdc00 LQI 3 in / – out"
        assert r[3] == ["Unknown 0x2400 LQI – in / 3 out"]

    def test_ping_posts_to_ping_and_shows_the_summary(self):
        # On Ping: POST /ping {address, count: 4, size: 32}, then the pingSummary lines.
        r = run(
            "await loaded(); await pick('c40961'); buttons(t.el('t-d-addrs'), 'Ping')[0].click(); await t.flush();"
            " const p = calls('POST', '/ping');"
            " return [p.length, JSON.parse(p[0].body), p[0].headers['Content-Type'], txt('t-d-ping')];"
        )
        assert r[:3] == [
            1,
            {"address": "2001:db8:4a2:1:2be4:6c19:a0f7:3381", "count": 4, "size": 32},
            "application/json",
        ]
        assert r[3] == (
            "ping 2001:db8:4a2:1:2be4:6c19:a0f7:3381 3 of 4 replies Loss 25% min/avg/max 5/7/9 ms"
            " Sleepy devices answer when they next wake up."
        )

    def test_rename_save_stores_the_nickname_and_updates_names_everywhere(self):
        # On Rename then Save: the nickname is stored under the ext address; dialog, table and map show it.
        r = run(
            "await loaded(); await pick('r40'); t.el('t-d-rename').click(); t.el('t-d-name-input').value = ' Hall light ';"
            " t.el('t-d-save').click(); return [JSON.parse(t.storage.m['topology:names']), txt('t-d-name'),"
            " row('r40').children[0].children[0].textContent, node('r40').attrs['aria-label'],"
            " row('c40961').children[2].textContent];"
        )
        assert r == [
            {"dd0000000000a000": "Hall light"},
            "Hall light",
            "Hall light",
            "Hall light, Router",
            "Hall light",
        ]

    def test_rename_with_an_empty_name_removes_the_nickname(self):
        # On Save with an empty name: the nickname is removed.
        r = run(
            "t.storage.m['topology:names'] = JSON.stringify({dd0000000000a000: 'Hall light'}); await loaded();"
            " await pick('r40'); const a = [txt('t-d-name')]; t.el('t-d-rename').click();"
            " a.push(t.el('t-d-name-input').value); t.el('t-d-name-input').value = ''; t.el('t-d-save').click();"
            " a.push('topology:names' in t.storage.m, txt('t-d-name')); return a;"
        )
        assert r == ["Hall light", "Hall light", False, "Unknown 0xa000"]

    @pytest.mark.parametrize("how", sorted(CLOSE_BY))
    def test_close_keeps_the_selection_ring_and_row_tint(self, how):
        # On close (×, Close, Esc, backdrop): closed, nothing dimmed, the ring and the row tint stay.
        r = run(
            "await loaded(); node('r23').click();"
            + CLOSE_BY[how]
            + " return [t.el('t-dialog').open, t.el('t-map').findAll((e) => e.classes.has('t-dim')).length,"
            " ring('r23').hidden(), ring('r9').hidden(), row('r23').classes.has('t-row-sel')];"
        )
        assert r == [False, 0, False, True, True]

    def test_show_on_map_closes_and_scrolls_to_the_map(self):
        # On Show on map: the dialog closes and the map card is scrolled into view.
        r = run(
            "await loaded(); node('r9').click(); t.el('t-d-showmap').click();"
            " return [t.el('t-dialog').open, t.el('t-map-card').scrolled];"
        )
        assert r == [False, 1]

    def test_child_without_detail_says_its_parent_didnt_answer(self):
        # On a child whose parent gave no detail (children_detail false): "Details unavailable: its parent didn't
        # answer", no detail rows.
        r = run("await loaded(); await pick('c23553'); return txt('t-d-facts');")
        assert "Details unavailable: its parent didn't answer" in r
        assert "RSSI" not in r

    def test_child_left_out_of_an_answering_parents_table_says_it_wasnt_reported(self):
        # On a child without detail under a router that did give detail (the child table left it out):
        # "Details unavailable: its parent didn't report it", no detail rows.
        r = run(
            "const X = {...R, routers: routers()};"
            " X.routers[4].children = [{rloc16: 0xd401, mode: mode(true), lq_in: 3, addresses: []}];"
            " save(60000, X); await start(); t.el('t-show-last').click(); await t.flush(); await pick('c54273');"
            " return txt('t-d-facts');"
        )
        assert "Details unavailable: its parent didn't report it" in r
        assert "didn't answer" not in r
        assert "RSSI" not in r

    def test_child_with_detail_lists_its_link_figures(self):
        # On a child with detail: parent, LQI, RSSI, link margin, error rates and attach time.
        r = run("await loaded(); await pick('c40961'); return txt('t-d-facts');")
        assert "Parent Unknown 0xa000" in r and "LQI 3" in r
        assert "RSSI avg -61 dBm, last -63 dBm" in r and "Link margin 39 dB" in r
        assert "Frame errors 0.4%" in r and "Message errors 0.0%" in r
        assert "Attached for 1 d 0 h" in r
        assert "Radio Off between polls, child timeout 240 s" in r
        assert "SRP host kitchen-plug.default.service.arpa" in r
        assert "Details unavailable" not in r

    def test_missing_values_show_a_dash(self):
        # On values the board left out (spec 3.3): "–".
        r = run(
            "await loaded(); await pick('r53'); const a = [txt('t-d-facts')]; await pick('r55');"
            " a.push(txt('t-d-facts')); return a;"
        )
        assert "Thread version –" in r[0]
        assert "Ext. address –" in r[1]

    def test_no_routable_address_shows_the_amber_note(self):
        # On no routable address: the amber "No routable address yet" note; hidden for a device with one.
        r = run(
            "await loaded(); await pick('c23554'); const a = [vis('t-d-noroute'), txt('t-d-noroute')];"
            " node('c40961').click(); a.push(vis('t-d-noroute')); return a;"
        )
        assert r == [True, NO_ROUTE, False]

    def test_internal_addresses_are_behind_the_toggle(self):
        # On the toggle: mesh-local, RLOC and link-local addresses join the list; ALOC and multicast never do.
        r = run(
            "await loaded(); await pick('r9'); const list = () => t.el('t-d-addrs').children.map((c) => c.allText());"
            " const a = [list(), txt('t-d-more')]; t.el('t-d-more').click(); a.push(list(), txt('t-d-more'));"
            " return a;"
        )
        assert r[0] == []
        assert r[1] == "Show internal addresses (mesh-local, RLOC, link-local)"
        assert r[2] == [
            "RLOC fdde:ad00:beef::ff:fe00:2400 Copy Ping",
            "Link-local fe80::d41a:7c03:f4e2:b918 Copy Ping",
        ]
        assert r[3] == "Hide internal addresses"

    # unreported addresses and other border routers (spec 3.4)

    def test_table_says_not_reported_in_grey_for_a_child_that_didnt_report(self):
        # On a child with addrsReported false: the routable cell reads "Not reported" in grey, not the amber "None yet".
        r = run(
            "await loaded(); const cell = (id) => row(id).children[4];"
            " return [cell('c23553').allText(), cell('c23553').children[0].classes.has('t-unreported'),"
            " cell('c23553').children[0].classes.has('t-none'), cell('c23554').allText()];"
        )
        assert r == ["Not reported", True, False, "None yet"]

    def test_dialog_says_the_device_didnt_report_its_addresses(self):
        # On a child with addrsReported false: "This device didn't report its addresses." instead of the amber
        # "No routable address yet" note.
        r = run(
            "await loaded(); await pick('c23553'); const a = [vis('t-d-noroute'), txt('t-d-noroute'),"
            " t.el('t-d-noroute').classes.has('t-warn')]; await pick('c23554');"
            " a.push(txt('t-d-noroute'), t.el('t-d-noroute').classes.has('t-warn')); return a;"
        )
        assert r == [
            True,
            "This device didn't report its addresses.",
            False,
            NO_ROUTE,
            True,
        ]

    def test_table_types_another_border_router_as_border_router(self):
        # On another border router: type "Border router"; a plain router stays "Router".
        r = run(
            "await loaded(); return ['r53', 'r9', 'r23'].map((id) => row(id).children[1].textContent);"
        )
        assert r == ["Border router", "Router", "Router"]

    def test_dialog_shows_a_border_router_chip_for_another_border_router(self):
        # On another border router: a "Border router" chip in the dialog header.
        r = run(
            "await loaded(); await pick('r53'); const a = [t.el('t-d-chips').children.map((c) => c.textContent)];"
            " await pick('r23'); a.push(t.el('t-d-chips').children.map((c) => c.textContent)); return a;"
        )
        assert r == [["Border router"], ["Router"]]

    def test_map_rings_another_border_router_in_orange_and_the_legend_names_it(self):
        # On another border router: an orange outer ring on its node (not on a plain router), its accessible name
        # says "Border router", and the legend has "Other border router".
        r = run(
            "await loaded(); const orange = (id) => node(id).findAll((e) => e.tag === 'circle'"
            " && e.classes.has('t-br-ring') && e.attrs.stroke === '#b35c00').length;"
            " return [orange('r53'), orange('r23'), orange('r34'), node('r53').attrs['aria-label']];"
        )
        assert r == [1, 0, 0, "Unknown 0xd400, Border router"]
        from device_js import FRONTEND

        html = (FRONTEND / "topology.html").read_text()
        legend = html[
            html.index('id="t-legend"') : html.index(
                "</div>", html.index('id="t-legend"')
            )
        ]
        assert (
            '<span class="t-lg t-lg-otherbr" aria-hidden="true"></span>Other border router'
            in legend
        )

    def test_map_keeps_both_rings_on_a_leader_that_is_a_border_router(self):
        # On a leader that is also another border router: the blue leader ring and the orange ring outside it.
        r = run(
            "const X = {...R, routers: routers()}; X.routers[0].border_router = true;"
            " save(60000, X); await start(); t.el('t-show-last').click(); await t.flush();"
            " const circles = node('r9').findAll((e) => e.tag === 'circle' && !e.classes.has('t-ring'));"
            " return circles.map((c) => [c.attrs.r, c.attrs.stroke, c.attrs.fill]);"
        )
        assert r == [
            ["16", "#174a77", "#1f5f99"],
            ["21", "#1f5f99", "none"],
            ["26", "#b35c00", "none"],
        ]

    def test_loading_view_shows_the_child_address_step(self):
        # On progress.children.total > 0: "Asking devices for their addresses: M of N" with the bar; with total 0 the
        # router step text.
        r = run(
            "S.gets = [RUNNING(2, 5, 0, 0), RUNNING(5, 5, 1, 4), RUNNING(5, 5, 1, 4)]; await start();"
            " const a = [txt('t-progress')]; await t.advance(1000);"
            " a.push(txt('t-progress'), vis('t-progress-track'), t.el('t-progress-bar').style.width); return a;"
        )
        assert r == [
            "Asking each router about its children: 2 of 5",
            "Asking devices for their addresses: 1 of 4",
            True,
            "25%",
        ]

    # device info

    def test_loading_view_says_it_is_asking_devices_for_details(self):
        r = run(
            "const I = (total, answered, sleepy, wait) => ({state: 'running', progress: {routers: 4, queried: 4,"
            " children: {total: 3, queried: 3}, info: {total, answered, sleepy_waiting: sleepy, wait_s: wait}}});"
            "S.gets = [I(6, 2, 1, 120), I(6, 4, 2, 200), I(6, 5, 1, 30), I(6, 4, 2, 0), I(0, 0, 0, 0)]; await start();"
            " const a = [txt('t-progress'), t.el('t-progress-bar').style.width];"
            " for (let i = 0; i < 4; i++) { await t.advance(1000); a.push(txt('t-progress')); } return a;"
        )
        assert r == [
            "Asking devices for details: 2 of 6",
            "33%",
            "Waiting for 2 sleepy devices (they answer when they next wake, up to 4 min)",
            "Waiting for 1 sleepy device (they answer when they next wake, up to 1 min)",
            "Waiting for 2 sleepy devices (they answer when they next wake)",
            "Asking devices for their addresses: 3 of 3",
        ]

    def test_loading_view_waits_for_sleepy_devices_once_no_awake_device_is_left(self):
        # On awake_left and done (newer firmware): the waiting text once awake_left is 0 with sleepy devices waiting,
        # even when an awake device gave up; otherwise "Asking devices for details: D of T" with the bar on done.
        r = run(
            "const I = (total, answered, sleepy, wait, awake, done) => ({state: 'running', progress: {routers: 4,"
            " queried: 4, children: {total: 3, queried: 3}, info: {total, answered, sleepy_waiting: sleepy,"
            " wait_s: wait, awake_left: awake, done}}});"
            "S.gets = [I(6, 2, 2, 200, 2, 3), I(6, 3, 2, 200, 0, 4), I(6, 3, 2, 0, 0, 4), I(6, 3, 0, 0, 1, 5),"
            " I(6, 3, 0, 0, 1, 5)]; await start();"
            " const a = [txt('t-progress'), t.el('t-progress-bar').style.width];"
            " for (let i = 0; i < 3; i++) { await t.advance(1000); a.push(txt('t-progress'),"
            " t.el('t-progress-bar').style.width); } return a;"
        )
        assert r == [
            "Asking devices for details: 3 of 6",
            "50%",
            "Waiting for 2 sleepy devices (they answer when they next wake, up to 4 min)",
            "67%",
            "Waiting for 2 sleepy devices (they answer when they next wake)",
            "67%",
            "Asking devices for details: 5 of 6",
            "83%",
        ]

    def test_loading_view_without_awake_left_keeps_the_answered_rule(self):
        # On older firmware (no awake_left or done): the waiting text only when the sleepy devices are all that is
        # left by the answered count, the bar on answered.
        r = run(
            "const I = (total, answered, sleepy, wait) => ({state: 'running', progress: {routers: 4, queried: 4,"
            " children: {total: 3, queried: 3}, info: {total, answered, sleepy_waiting: sleepy, wait_s: wait}}});"
            "S.gets = [I(6, 3, 2, 120), I(6, 4, 2, 120), I(6, 4, 2, 120)]; await start();"
            " const a = [txt('t-progress'), t.el('t-progress-bar').style.width]; await t.advance(1000);"
            " a.push(txt('t-progress'), t.el('t-progress-bar').style.width); return a;"
        )
        assert r == [
            "Asking devices for details: 3 of 6",
            "50%",
            "Waiting for 2 sleepy devices (they answer when they next wake, up to 2 min)",
            "67%",
        ]

    def test_loading_view_without_info_keeps_todays_texts(self):
        r = run(
            "S.gets = [RUNNING(5, 5, 1, 4), RUNNING(2, 5)]; await start(); const a = [txt('t-progress')];"
            " await t.advance(1000); a.push(txt('t-progress')); return a;"
        )
        assert r == [
            "Asking devices for their addresses: 1 of 4",
            "Asking each router about its children: 2 of 5",
        ]

    INFO_RESULT = (
        f"const IR_STACKS = {{posix: {STACK_POSIX!r}, sl: {STACK_SL!r}, esp: {STACK_ESP!r}}};"
        + r"""
const IR = structuredClone(SV);
IR.routers[0].info = {state: 'answered', eui64: '8c73dafffe952cfb', vendor_name: 'Home Assistant',
  vendor_model: 'OpenThread Border Router', thread_stack_version: IR_STACKS.posix,
  vendor_app_url: 'https://example.com/ha'};
IR.routers[1].info = {state: 'answered', thread_stack_version: IR_STACKS.sl};
IR.routers[2].info = {state: 'answered', vendor_name: 'RD:', thread_stack_version: IR_STACKS.esp,
  vendor_app_url: 'javascript:alert(1)'};
IR.routers[3].info = {state: 'answered', vendor_name: '<b>x</b>', vendor_model: '<i>y</i>'};
IR.routers[2].children = [{rloc16: 0x6401, ext_address: 'acebe6fffe0bffa4', mode: {rx_on_when_idle: false},
  lq_in: 3, addresses: [], info: {state: 'no_answer'}},
  {rloc16: 0x6402, ext_address: 'acebe6fffe0bffa5', mode: {rx_on_when_idle: true}, lq_in: 3, addresses: [],
   info: {state: 'no_answer'}},
  {rloc16: 0x6403, ext_address: 'acebe6fffe0bffa6', mode: {rx_on_when_idle: true}, lq_in: 3, addresses: [],
   info: {state: 'cut_short'}},
  {rloc16: 0x6404, ext_address: 'acebe6fffe0bffa7', mode: {rx_on_when_idle: true}, lq_in: 3, addresses: [],
   info: {state: 'not_asked'}}];
"""
    )

    def test_dialog_shows_hardware_stack_software_and_eui64(self):
        r = sv(
            self.INFO_RESULT
            + "IR.routers[0].info.vendor_sw_version = '20261104-bb169ca';"
            " await openSV('r4', IR); return ['Hardware', 'Stack', 'Software', 'EUI-64'].map(fact);"
        )
        assert r == [
            "Home Assistant OpenThread Border Router",
            STACK_POSIX,
            "20261104-bb169ca",
            "8c73dafffe952cfb",
        ]

    def test_dialog_summarises_the_stack_when_there_are_no_vendor_strings(self):
        r = sv(
            self.INFO_RESULT
            + "await openSV('r12', IR); return ['Hardware', 'Stack', 'Software', 'EUI-64', 'App URL'].map(fact);"
        )
        assert r == [
            "Silicon Labs EFR32 · SL-OpenThread 2.4.2.0",
            STACK_SL,
            None,
            None,
            None,
        ]

    def test_dialog_summary_omits_missing_parts(self):
        r = sv(self.INFO_RESULT + "await openSV('r25', IR); return fact('Hardware');")
        assert r == "Espressif esp32c6"

    def test_dialog_links_an_http_app_url(self):
        r = sv(
            self.INFO_RESULT
            + "await openSV('r4', IR); const a = t.el('t-d-facts').findAll((e) => e.tag === 'a');"
            " return [fact('App URL'), a.map((e) => [e.textContent, e.attrs.href, e.attrs.rel])];"
        )
        assert r[0] == "https://example.com/ha"
        assert r[1] == [
            ["https://example.com/ha", "https://example.com/ha", "noopener"]
        ]

    def test_dialog_shows_a_javascript_app_url_as_plain_text_without_a_link(self):
        r = sv(
            self.INFO_RESULT + "await openSV('r25', IR); return [fact('App URL'),"
            " t.el('t-d-facts').findAll((e) => e.tag === 'a' && /^javascript/i.test(e.attrs.href || '')).length];"
        )
        assert r == ["javascript:alert(1)", 0]

    def test_dialog_shows_markup_in_vendor_strings_as_text(self):
        r = sv(
            self.INFO_RESULT + "await openSV('r26', IR); return [fact('Hardware'),"
            " t.el('t-d-facts').findAll((e) => e.tag === 'b' || e.tag === 'i').length, txt('t-d-name')];"
        )
        assert r[0] == "<b>x</b> <i>y</i>"
        assert r[1] == 0

    def test_dialog_of_a_sleepy_child_that_did_not_answer_says_so(self):
        r = sv(
            self.INFO_RESULT + "await openSV('c25601', IR); return fact('Device info');"
        )
        assert r == "no answer (sleepy device)"

    def test_dialog_of_an_awake_device_that_did_not_answer_says_so(self):
        r = sv(
            self.INFO_RESULT + "await openSV('c25602', IR); return fact('Device info');"
        )
        assert r == "no answer"

    def test_dialog_of_a_device_cut_short_by_the_time_limit_says_so(self):
        r = sv(
            self.INFO_RESULT + "await openSV('c25603', IR); return fact('Device info');"
        )
        assert r == "not asked (scan time limit)"

    def test_dialog_of_a_device_that_was_not_asked_shows_no_info_facts(self):
        r = sv(
            self.INFO_RESULT + "await openSV('c25604', IR);"
            " return ['Device info', 'Hardware', 'Stack', 'Software', 'EUI-64', 'App URL'].map(fact);"
        )
        assert r == [None] * 6

    def test_dialog_of_a_result_without_info_has_no_info_facts(self):
        r = sv(
            "await openSV('r4');"
            " return ['Device info', 'Hardware', 'Stack', 'Software', 'EUI-64', 'App URL'].map(fact);"
        )
        assert r == [None] * 6

    def test_table_names_a_device_by_its_info_and_shows_the_stack_kind(self):
        r = sv(
            self.INFO_RESULT
            + "IR.routers[2].children[1].info = {state: 'answered', thread_stack_version: IR_STACKS.sl};"
            " await showSV(IR); return [row('r4').children[0].allText(), kind('c25602'), kind('r12')];"
        )
        assert "Home Assistant OpenThread Border Router" in r[0]
        assert r[1] == ["Silicon Labs EFR32"]
        assert r[2] == ["Matter \u00b7 2 fabrics"]  # an SRP kind wins over the stack

    # storage unavailable

    def test_throwing_storage_works_without_saved_result_and_names(self):
        # On a throwing localStorage: no saved result, a load still works, names work for the page's life and the
        # dialog says names can't be saved.
        r = run(
            "t.storage.fail = true; await start(); const a = [vis('t-start'), vis('t-show-last'), txt('t-last-line')];"
            " S.gets = [DONE(RF, 0)]; t.el('t-load').click(); await t.advance(1000); a.push(vis('t-loaded'));"
            " await pick('r40'); t.el('t-d-rename').click(); a.push(txt('t-names-note'));"
            " t.el('t-d-name-input').value = 'Hall'; t.el('t-d-save').click(); a.push(txt('t-d-name')); return a;"
        )
        assert r == [
            True,
            False,
            "No saved result in this browser yet.",
            True,
            NAMES_NOTE,
            "Hall",
        ]

    # review focus

    def test_srp_name_with_markup_appears_only_as_text(self):
        # On an SRP name <b>"x"</b> (RF1): shown verbatim as text in the table, dialog and map; no element made of it.
        r = run(
            "await loaded(); await pick('c34818');"
            " const label = node('c34818').findAll((e) => e.tag === 'text').map((e) => e.textContent);"
            " const made = ['t-map', 't-rows', 't-dialog'].map((id) => t.el(id).findAll((e) => e.tag === 'b').length);"
            " return [row('c34818').children[0].children[0].textContent, txt('t-d-name'), label, made];"
        )
        assert r[0] == '↳ <b>"x"</b>'
        assert r[1] == '<b>"x"</b>'
        assert '<b>"x"</b>' in r[2]
        assert r[3] == [0, 0, 0]

    # service records: device table

    def test_table_row_shows_a_kind_line_under_the_name(self):
        # On devices with a description: the kind line under the name (ESPHome, Matter, border router).
        r = sv("await showSV(); return [kind('r25'), kind('r12'), kind('r4')];")
        assert r == [
            ["ESPHome 2026.7.4 · esp32-c6-devkitc-1"],
            ["Matter · 2 fabrics"],
            ["Border router · Home Assistant · Thread 1.4.0"],
        ]

    def test_table_row_without_a_description_has_no_kind_line(self):
        # On this board, whose only record has no services: no kind line.
        assert sv("await showSV(); return kind('r26');") == []

    def test_table_names_come_from_the_records(self):
        # On the records of the sample mesh: the ESPHome friendly name and the meshcop name replace "Unknown".
        r = sv(
            "await showSV(); return ['r25', 'r4', 'r12'].map((id) => row(id).children[0].children[0].textContent);"
        )
        assert r == [
            "C6 Thread Test",
            "Home Assistant OpenThread Border Router #0EE6",
            "Matter device 0x3000",
        ]

    def test_table_hint_names_every_srp_server_with_its_device_count(self):
        # On srp_servers all answered: the hint, this board by "this board", the other by display name.
        r = sv(
            "await showSV(); return [vis('t-srp-hint'), txt('t-srp-hint'), vis('t-srp-warn')];"
        )
        assert r == [
            True,
            (
                "Service records from 2 SRP servers: this board (4 devices) and"
                " Home Assistant OpenThread Border Router #0EE6 (2 devices)."
            ),
            False,
        ]

    def test_table_hint_counts_a_single_server_in_the_singular(self):
        # On one server with one host: "1 SRP server" and "1 device".
        r = sv(
            "await showSV({...SV, srp_servers: [{...SERVERS[0], hosts: 1}]}); return txt('t-srp-hint');"
        )
        assert r == "Service records from 1 SRP server: this board (1 device)."

    def test_table_warns_about_a_server_that_did_not_answer(self):
        # On a server in state no_answer: the amber note naming it.
        r = sv(
            "await showSV({...SV, srp_servers: [SERVERS[0], {...SERVERS[1], state: 'no_answer'}]});"
            " return [vis('t-srp-warn'), t.el('t-srp-warn').classes.has('t-warn'), txt('t-srp-warn')];"
        )
        assert r == [
            True,
            True,
            (
                "Home Assistant OpenThread Border Router #0EE6's SRP server didn't answer, so the devices"
                " registered there show without names."
            ),
        ]

    def test_table_warns_about_a_server_whose_answer_was_cut_short(self):
        # On a server in state cut_short that no device in the scan matches: the note names it by RLOC16.
        r = sv(
            "await showSV({...SV, srp_servers: [SERVERS[0], {...SERVERS[1], rloc16: 0x7400, state: 'cut_short'}]});"
            " return txt('t-srp-warn');"
        )
        assert r == (
            "Router 0x7400's SRP server's answer was cut short, so some devices registered there may be missing."
        )

    # service records: device dialog

    def test_dialog_of_an_esphome_device_lists_device_project_and_node_name(self):
        # On the ESPHome device: Device, Project and Node name from its records.
        r = sv(
            "await openSV('r25'); return [fact('Device'), fact('Project'), fact('Node name')];"
        )
        assert r == [
            "ESPHome 2026.7.4 on esp32-c6-devkitc-1 (ESP32)",
            "esp32otbr.srp-probe 0.1",
            "c6-thread-test",
        ]

    def test_dialog_says_registered_with_this_board_and_the_lease(self):
        # On a record with lease_s 7012 (this board's server): "This board · lease ends in 1 h 56 min".
        r = sv("await openSV('r25'); return fact('Registered with');")
        assert r == "This board · lease ends in 1 h 56 min"

    def test_dialog_lists_the_services_with_their_ports(self):
        # On the ESPHome device: both services with their ports.
        r = sv("await openSV('r25'); return fact('Services');")
        assert "_esphomelib._tcp port 6053" in r
        assert "_http._tcp port 80" in r

    def test_dialog_offers_an_open_link_to_the_web_server_on_the_omr_address(self):
        # On the ESPHome _http._tcp service and an OMR address: Open, to http://[address]:80/ in a new tab.
        r = sv(
            "await openSV('r25'); const a = t.el('t-d-facts').findAll((e) => e.tag === 'a');"
            " return a.map((e) => [e.textContent, e.attrs.href, e.attrs.target, e.attrs.rel]);"
        )
        assert r == [
            [
                "Open",
                "http://[fd87:9b49:8264:1:c158:979b:6560:a15f]:80/",
                "_blank",
                "noopener",
            ]
        ]

    def test_dialog_has_no_open_link_without_an_omr_address(self):
        # On a network without OMR prefixes: no Open link.
        r = sv(
            "await openSV('r25', {...SV, network: {...SV.network, omr_prefixes: []}});"
            " return t.el('t-d-facts').findAll((e) => e.tag === 'a').length;"
        )
        assert r == 0

    def test_dialog_of_a_matter_device_lists_fabrics_and_retry_timing(self):
        # On the Matter device: Device, a Fabrics list and the retry timing from SII, SAI and SAT.
        r = sv(
            "await openSV('r12'); return [fact('Device'), fact('Fabrics'), fact('Retry timing')];"
        )
        assert r[0] == "Matter, paired with 2 controllers"
        assert "CAC23A5DA6E7644C" in r[1] and "000000000F4B0B5F" in r[1]
        assert "3E5D425037C4F971" in r[1] and "0000000000000003" in r[1]
        assert r[2] == "800 ms idle, 2 s active, active for 4 s after traffic"

    def test_dialog_names_the_server_a_proxied_record_came_from(self):
        # On a record with ttl_s 2940 from the server 0x1000: its display name, RLOC16 and "record valid 49 min".
        r = sv("await openSV('r12'); return fact('Registered with');")
        assert r == (
            "Home Assistant OpenThread Border Router #0EE6 (0x1000) · record valid 49 min"
        )

    def test_dialog_links_the_registering_server_to_its_own_details(self):
        # On a click on the server's name: that router's dialog opens.
        r = sv(
            "await openSV('r12'); const b = buttons(t.el('t-d-facts'), 'Home Assistant OpenThread Border Router #0EE6');"
            " b[0].click(); await t.flush(); return txt('t-d-name');"
        )
        assert r == "Home Assistant OpenThread Border Router #0EE6"

    def test_dialog_of_a_matter_device_explains_the_missing_vendor_and_product(self):
        # On a Matter device without a commissionable record: the pairing-mode note; the ESPHome device has none.
        r = sv(
            "await openSV('r12'); const a = [vis('t-d-note'), txt('t-d-note')]; await pick('r25');"
            " a.push(vis('t-d-note')); return a;"
        )
        assert r == [
            True,
            (
                "Vendor, product and device type are not in these records: Matter publishes them only while the"
                " device is in pairing mode."
            ),
            False,
        ]

    def test_dialog_of_the_other_border_router_lists_vendor_model_and_version(self):
        # On the HA border router: vendor, model and Thread version from its meshcop record.
        r = sv("await openSV('r4'); return fact('Device');")
        assert r == "Home Assistant · OpenThread Border Router · Thread 1.4.0"

    def test_dialog_of_a_border_router_shows_its_srp_server_line(self):
        # On a router with an srp_servers entry: "Yes, N devices registered · state".
        r = sv(
            "await openSV('r4'); const a = [fact('SRP server')];"
            " await showSV({...SV, srp_servers: [SERVERS[0], {...SERVERS[1], state: 'no_answer', hosts: 0}]});"
            " await pick('r4'); a.push(fact('SRP server')); await pick('r25'); a.push(fact('SRP server'));"
            " return a;"
        )
        assert r == [
            "Yes, 2 devices registered · answered",
            "Yes, 0 devices registered · didn't answer",
            None,
        ]

    def test_dialog_of_the_other_border_router_prefers_its_srp_host_over_its_meshcop_host(
        self,
    ):
        # On a border router matching its meshcop host first and its own SRP host (from another server) second: SRP host
        # and Registered with from the SRP host.
        r = sv(
            "const own = (({lease_s, key_lease_s, ...h}) => ({...h, source: 4096, ttl_s: 60}))("
            "HOSTS.find((h) => h.name === 'homeassistant-otbr'));"
            " const hosts = HOSTS.filter((h) => h !== HOSTS.find((x) => x.name === 'homeassistant-otbr')).concat([own]);"
            " await openSV('r4', {...SV, srp_hosts: hosts}); return [fact('SRP host'), fact('Registered with')];"
        )
        assert r[0] == "homeassistant-otbr.default.service.arpa"
        assert r[1].endswith("· record valid 1 min")

    def test_dialog_of_the_other_border_router_with_only_its_meshcop_host_says_advertised_on_the_lan(
        self,
    ):
        # On a border router whose only matched host is its meshcop host: "– (advertised on the LAN as <host>)", no
        # Registered with.
        r = sv(
            "await openSV('r4', {...SV, srp_hosts: HOSTS.filter((h) => h.name !== 'homeassistant-otbr')});"
            " return [fact('SRP host'), fact('Registered with')];"
        )
        assert r == ["– (advertised on the LAN as homeassistant)", None]

    # service records: raw records

    def test_dialog_hides_the_raw_records_until_asked(self):
        # On a device with services: "Show raw records (2)" and no tables.
        r = sv(
            "await openSV('r12'); return [vis('t-d-raw'), rawBtn().textContent, rawTables().length];"
        )
        assert r == [True, "Show raw records (2)", 0]

    def test_dialog_toggles_the_raw_records_open_and_closed(self):
        # On two clicks: "Hide raw records" with a table per service, then back to "Show raw records (2)".
        r = sv(
            "await openSV('r12'); rawBtn().click(); await t.flush(); const a = [rawBtn().textContent, rawTables().length];"
            " rawBtn().click(); await t.flush(); a.push(rawBtn().textContent, rawTables().length); return a;"
        )
        assert r == [
            "Hide raw records",
            2,
            "Show raw records (2)",
            0,
        ]

    def test_raw_records_list_every_txt_entry_of_a_service(self):
        # On the ESPHome device: each TXT entry as key and value rows; a service without TXT says so.
        r = sv(
            "await openSV('r25'); rawBtn().click(); await t.flush(); const [api, web] = rawTables();"
            " return [rawRows(api).length, rawRows(api)[0], rawRows(api)[9], web.allText()];"
        )
        assert r[0] == 10
        assert r[1] == ["friendly_name", "C6 Thread Test"]
        assert r[2] == ["api_encryption", "Noise_NNpsk0_25519_ChaChaPoly_SHA256"]
        assert "No TXT entries" in r[3]

    def test_raw_records_show_binary_values_as_hex(self):
        # On the border router's meshcop record: xa and sb (binary) in hex.
        r = sv(
            "await openSV('r4'); rawBtn().click(); await t.flush();"
            " return rawTables().flatMap(rawRows).filter((x) => ['xa', 'sb', 'xp'].includes(x[0]));"
        )
        assert r == [
            ["xp", "5965b57e30c0def4"],
            ["xa", "7e287c8b8b140ee6"],
            ["sb", "000009b1"],
        ]

    def test_raw_records_caption_names_the_instance_port_and_source(self):
        # On a service from this board and one from the HA server: instance.type, port and where it came from.
        r = sv(
            "await openSV('r25'); rawBtn().click(); await t.flush(); const a = [rawTables()[0].allText()];"
            " await pick('r12'); rawBtn().click(); await t.flush(); a.push(rawTables()[0].allText()); return a;"
        )
        assert (
            "c6-thread-test._esphomelib._tcp" in r[0]
            and "port 6053" in r[0]
            and "from this board" in r[0]
        )
        assert "_matter._tcp" in r[1] and "port 5540" in r[1]
        assert "from Home Assistant OpenThread Border Router #0EE6" in r[1]

    def test_raw_records_start_closed_for_the_next_device(self):
        # On opening another device after opening raw records: closed again.
        r = sv(
            "await openSV('r25'); rawBtn().click(); await t.flush(); t.el('t-d-close').click(); await pick('r12');"
            " return rawTables().length;"
        )
        assert r == 0

    # service records: markup and odd values

    def test_markup_in_a_friendly_name_stays_text(self):
        # On friendly_name=<b>Hi</b> (RF3): shown as text in the table, dialog, raw records and map; no <b> made.
        r = sv(
            f"const X = {{...SV, srp_hosts: HOSTS.map((h) => h.name !== 'c6-thread-test' ? h : {{...h, services: ["
            f"svc('c6-thread-test', '_esphomelib._tcp', 6053, 26624, '{BOLD_TXT}')]}})}};"
            " await openSV('r25', X); rawBtn().click(); await t.flush();"
            " const made = ['t-rows', 't-dialog', 't-map'].map((id) => t.el(id).findAll((e) => e.tag === 'b').length);"
            " return [row('r25').children[0].children[0].textContent, txt('t-d-name'), made,"
            " rawRows(rawTables()[0])];"
        )
        assert r[0] == "<b>Hi</b>"
        assert r[1] == "<b>Hi</b>"
        assert r[2] == [0, 0, 0]

    def test_raw_records_show_key_only_empty_and_binary_entries(self):
        # On key-only, empty and binary TXT entries (RF3): the key alone for a flag, an empty value, hex for binary.
        r = sv(
            f"const X = {{...SV, srp_hosts: HOSTS.map((h) => h.name !== 'c6-thread-test' ? h : {{...h, services: ["
            f"svc('c6-thread-test', '_esphomelib._tcp', 6053, 26624, '{BOLD_TXT}')]}})}};"
            " await openSV('r25', X); rawBtn().click(); await t.flush(); return rawRows(rawTables()[0]);"
        )
        assert r == [
            ["friendly_name", "<b>Hi</b>"],
            ["at", "no value"],
            ["bin", "00ff"],
            ["empty", ""],
        ]

    # service records: nicknames and generic names

    def test_generic_matter_name_is_styled_as_unnamed(self):
        # On a device shown as "Matter device 0x3000": the unnamed style, in the table and the dialog title.
        r = sv(
            "await openSV('r12'); return [row('r12').children[0].children[0].classes.has('t-unnamed'),"
            " t.el('t-d-name').classes.has('t-unnamed'), row('r25').children[0].children[0].classes.has('t-unnamed')];"
        )
        assert r == [True, True, False]

    def test_a_nickname_on_a_generic_matter_device_is_not_unnamed(self):
        # On a nickname given to "Matter device 0x3000": named.
        r = sv(
            "await openSV('r12'); t.el('t-d-rename').click(); t.el('t-d-name-input').value = 'Plug';"
            " t.el('t-d-save').click(); await t.flush();"
            " return [row('r12').children[0].children[0].textContent,"
            " row('r12').children[0].children[0].classes.has('t-unnamed')];"
        )
        assert r == ["Plug", False]

    # service records: results from older firmware

    def test_saved_result_from_older_firmware_renders_as_before(self):
        # On a result with no srp_servers and no txt (RF5): no kind lines, no servers hint or note, no Device facts.
        r = run(
            "await loaded(); const a = [row('c40961').children[0].findAll((e) => e.classes.has('t-kind')).length,"
            " vis('t-srp-hint'), vis('t-srp-warn')]; await pick('c40961');"
            " const dts = t.el('t-d-facts').findAll((e) => e.tag === 'dt').map((e) => e.textContent);"
            " a.push(dts.includes('Device'), dts.includes('Registered with'), vis('t-d-raw'), vis('t-d-note'),"
            " txt('t-d-facts')); return a;"
        )
        assert r[:7] == [0, False, False, False, False, False, False]
        assert "SRP host kitchen-plug.default.service.arpa" in r[7]
        assert "kitchen-plug" in r[7] and "_matter._tcp" in r[7]


@needs_node
class TestTrelTransport:
    """TopologyLogic: routerLinks transport, sendsOver, trelLow, transportText"""

    def test_board_links_carry_their_transport(self):
        r = js(
            "L.routerLinks(TR).filter((l) => l.transport).map((l) => [l.a, l.b, l.transport])"
        )
        assert r == [
            ["r9", "r34", {"ieee802154": 230, "trel": 55}],
            ["r34", "r40", {"ieee802154": None, "trel": 255}],
        ]

    def test_links_between_other_routers_have_no_transport(self):
        r = js(
            "L.routerLinks(TR).filter((l) => l.a !== 'r34' && l.b !== 'r34').some((l) => 'transport' in l)"
        )
        assert r is False

    @pytest.mark.parametrize(
        ("transport", "radio"),
        [
            ("{ieee802154: 255, trel: 255}", "trel"),
            ("{ieee802154: 255, trel: 230}", "trel"),
            ("{ieee802154: 210, trel: 200}", "ieee802154"),
            ("{ieee802154: 200, trel: 200}", "trel"),
            ("{ieee802154: 200, trel: null}", "ieee802154"),
        ],
    )
    def test_sends_over_mirrors_the_openthread_radio_selector(self, transport, radio):
        assert plain(f"L.sendsOver({transport})") == radio

    def test_trel_below_the_radio_is_low(self):
        assert plain("L.trelLow({ieee802154: 230, trel: 55})") is True

    def test_trel_alone_is_never_low(self):
        assert plain("L.trelLow({ieee802154: null, trel: 255})") is False

    @pytest.mark.parametrize(
        ("transport", "text"),
        [
            ("{ieee802154: 255, trel: null}", "15.4 100%"),
            ("{ieee802154: null, trel: 255}", "TREL 100%"),
            (
                "{ieee802154: 255, trel: 55}",
                "15.4 100% \u00b7 TREL 22%, sends over 15.4",
            ),
            (
                "{ieee802154: 255, trel: 255}",
                "15.4 100% \u00b7 TREL 100%, sends over TREL",
            ),
        ],
    )
    def test_transport_text_names_both_preferences_and_the_radio_in_use(
        self, transport, text
    ):
        assert plain(f"L.transportText({transport})") == text


@needs_node
class TestTrelBadgeSpot:
    """TopologyLogic.trelBadgeSpot: the TREL badge on the far half of the curve, clear of other badges"""

    # Far half of a curve: from the home box (0, 0) through (100, 0) to the neighbour (100, 100).
    CURVE = "{x: 0, y: 0}, {x: 100, y: 0}, {x: 100, y: 100}"

    def test_free_curve_puts_the_badge_in_the_middle(self):
        assert js(
            f"(({{x, y}}) => ({{x, y}}))(L.trelBadgeSpot({self.CURVE}, 60, []))"
        ) == {"x": 75, "y": 25}

    def test_badge_moves_off_a_badge_at_the_middle(self):
        # An LQI badge at (75, 25): the middle, 0.35, 0.65 and 0.2 come within the 6 px gap of it; 0.8 (96, 64) is clear.
        r = js(
            f"(({{x, y, box}}) => [x, y, box])(L.trelBadgeSpot({self.CURVE}, 60, [{{x0: 55, x1: 95, y0: 17, y1: 33}}]))"
        )
        assert r == [96, 64, {"x0": 66, "x1": 126, "y0": 56, "y1": 72}]

    def test_badge_stays_in_the_middle_when_no_spot_is_clear(self):
        r = js(
            f"(({{x, y}}) => ({{x, y}}))(L.trelBadgeSpot({self.CURVE}, 60, [{{x0: -50, x1: 150, y0: -50, y1: 150}}]))"
        )
        assert r == {"x": 75, "y": 25}


class TestTrelMap:
    """TREL links of this board on the map, in the legend, the dialog and the device table"""

    LOAD_TR = (
        "save(60000, TR); await start(); t.el('t-show-last').click(); await t.flush();"
    )
    TREL_EDGES = "t.el('t-map').findAll((e) => e.classes.has('t-trel-edge'))"

    def test_link_over_both_radios_gets_a_straight_line_and_a_dashed_curve_through_the_home_box(
        self,
    ):
        r = run(
            self.LOAD_TR + f" const g = {self.TREL_EDGES};"
            " const badge = (e) => e.findAll((x) => x.classes.has('t-badge-text'))[0].textContent;"
            " const path = g.map((e) => e.findAll((x) => x.tag === 'path')[0]);"
            " const home = t.el('t-map').findAll((e) => e.classes.has('t-uplink'))[0]"
            ".findAll((e) => e.tag === 'rect')[0].attrs;"
            " const cx = Number(home.x) + Number(home.width) / 2, cy = Number(home.y) + Number(home.height) / 2;"
            " const straight = t.el('t-map').findAll((e) => e.classes.has('t-link-edge') && !e.classes.has('t-trel-edge'))"
            ".map((e) => e.findAll((x) => x.classes.has('t-badge-text'))[0].textContent);"
            " return [g.map(badge), path.map((p) => [p.attrs['stroke-dasharray'], p.attrs.stroke,"
            " p.attrs.d.includes(' ' + cx + ' ' + cy + ' T ')]), straight];"
        )
        assert r[0] == ["TREL 22%", "TREL 100%"]
        assert r[1] == [["7 4", "#7b3fb5", True], ["7 4", "#7b3fb5", True]]
        assert "3" in r[2]  # the 15.4 line to the leader stays

    def test_home_box_is_drawn_over_the_trel_curves(self):
        r = run(
            self.LOAD_TR + " const all = t.el('t-map').findAll(() => true);"
            " const at = (cls) => all.map((e, i) => e.classes.has(cls) ? i : -1).filter((i) => i >= 0);"
            " return [Math.max(...at('t-trel-edge')) < Math.min(...at('t-uplink')), at('t-uplink').length];"
        )
        assert r == [True, 1]

    def test_radio_links_are_drawn_over_the_home_box(self):
        r = run(
            self.LOAD_TR + " const all = t.el('t-map').findAll(() => true);"
            " const at = (f) => all.map((e, i) => f(e) ? i : -1).filter((i) => i >= 0);"
            " const radio = at((e) => e.classes.has('t-link-edge') && !e.classes.has('t-trel-edge'));"
            " return Math.max(...at((e) => e.classes.has('t-uplink'))) < Math.min(...radio);"
        )
        assert r is True

    def test_trel_badges_overlap_no_lqi_badge(self):
        r = run(
            self.LOAD_TR
            + " const box = (e) => { const r = e.findAll((x) => x.tag === 'rect')[0].attrs;"
            " return {x0: Number(r.x), x1: Number(r.x) + Number(r.width), y0: Number(r.y), y1: Number(r.y) + Number(r.height)}; };"
            " const edges = t.el('t-map').findAll((e) => e.classes.has('t-link-edge'));"
            " const trel = edges.filter((e) => e.classes.has('t-trel-edge')).map(box);"
            " const lqi = edges.filter((e) => !e.classes.has('t-trel-edge')).map(box);"
            " const hit = (a, b) => a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;"
            " return [trel.length, trel.some((a) => lqi.some((b) => hit(a, b)))];"
        )
        assert r == [2, False]

    def test_trel_only_link_has_no_straight_line(self):
        # R's badges are ["2", "3", "1/3", "3/0", "2"]; the last one is the board's link to r40, TREL only in TR.
        r = run(
            self.LOAD_TR
            + " return t.el('t-map').findAll((e) => e.classes.has('t-link-edge') && !e.classes.has('t-trel-edge'))"
            ".map((e) => e.findAll((x) => x.classes.has('t-badge-text'))[0].textContent);"
        )
        assert r == ["2", "3", "1/3", "3/0"]

    def test_trel_below_the_radio_gets_a_red_badge(self):
        r = run(
            self.LOAD_TR
            + f" return {self.TREL_EDGES}.map((e) => e.findAll((x) => x.classes.has('t-badge-text'))[0].attrs.fill);"
        )
        assert r == ["#c62828", "#7b3fb5"]

    def test_legend_names_trel_only_when_a_trel_link_is_on_the_map(self):
        r = run(
            "await loaded(); const without = vis('t-lg-trel');"
            " save(60000, TR); await start(); t.el('t-show-last').click(); await t.flush();"
            " return [without, vis('t-lg-trel')];"
        )
        assert r == [False, True]

    def test_dialog_names_both_preferences_the_radio_in_use_and_the_lqi_caveat(self):
        r = run(
            self.LOAD_TR
            + " node('r9').click(); return t.el('t-d-neighbors').children.map((c) => c.allText().replace(/\\s+/g, ' ').replace(/ ,/g, ','));"
        )
        assert r[1] == (
            "This border router LQI 3 15.4 90% \u00b7 TREL 22%, sends over 15.4 link quality includes TREL frames"
        )

    def test_dialog_marks_a_low_trel_figure_red(self):
        r = run(
            self.LOAD_TR
            + " node('r9').click(); return t.el('t-d-neighbors').findAll((e) => e.classes.has('pref-low'))"
            ".map((e) => e.textContent);"
        )
        assert r == ["22%"]

    def test_connected_via_names_the_boards_transport_to_that_router(self):
        r = run(
            self.LOAD_TR
            + " return [row('r9').children[2].allText().replace(/\\s+/g, ' ').replace(/ ,/g, ','),"
            " row('r23').children[2].allText()];"
        )
        assert r[0] == "3 router neighbors 15.4 90% \u00b7 TREL 22%, sends over 15.4"
        assert "TREL" not in r[1]
