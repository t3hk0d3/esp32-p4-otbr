/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP Thread Border Router - Topology page (GET/POST /device/topology)
 *
 * TopologyLogic is pure and runs under node too (tools/tests/test_topology_page.py). TopologyPage is the page.
 * Device ids: routers are "r<router id>", children are "c<rloc16>".
 */
'use strict';

var TopologyLogic = (function() {

var LAST_KEY = 'topology:last';
var NAMES_KEY = 'topology:names';
var SLEEPY_NOTE = 'Sleepy devices answer when they next wake up.';

/* ---- IPv6 ---- */

/** The eight 16-bit groups of an IPv6 address, or null when `text` is not one. */
function parseV6(text) {
  if (typeof text !== 'string')
    return null;
  var halves = text.split('::');
  if (halves.length > 2)
    return null;
  function groups(part) {
    if (part === '')
      return [];
    var out = [];
    var items = part.split(':');
    for (var i = 0; i < items.length; i++) {
      if (!/^[0-9a-fA-F]{1,4}$/.test(items[i]))
        return null;
      out.push(parseInt(items[i], 16));
    }
    return out;
  }
  var head = groups(halves[0]);
  var tail = halves.length === 2 ? groups(halves[1]) : [];
  if (head === null || tail === null)
    return null;
  if (halves.length === 1)
    return head.length === 8 ? head : null;
  var fill = 8 - head.length - tail.length;
  if (fill < 1)
    return null;
  var mid = [];
  for (var k = 0; k < fill; k++)
    mid.push(0);
  return head.concat(mid, tail);
}

/** {g, len} of a prefix such as "fdde:ad00:beef::/64", or null. */
function parsePrefix(text) {
  if (typeof text !== 'string')
    return null;
  var at = text.indexOf('/');
  if (at < 0)
    return null;
  var g = parseV6(text.slice(0, at));
  var len = parseInt(text.slice(at + 1), 10);
  if (g === null || !(len >= 0 && len <= 128))
    return null;
  return {g : g, len : len};
}

function underPrefix(g, prefix) {
  var bits = prefix.len;
  for (var i = 0; i < 8 && bits > 0; i++, bits -= 16) {
    var n = Math.min(16, bits);
    var mask = (0xffff << (16 - n)) & 0xffff;
    if ((g[i] & mask) !== (prefix.g[i] & mask))
      return false;
  }
  return true;
}

/** "global", "ula", "rloc", "aloc", "mleid", "linklocal", "multicast" or "other". */
function addressClass(addr, network) {
  var g = parseV6(addr);
  if (g === null)
    return 'other';
  network = network || {};
  var omr = network.omr_prefixes || [];
  for (var i = 0; i < omr.length; i++) {
    var p = parsePrefix(omr[i]);
    if (p !== null && underPrefix(g, p)) {
      if ((p.g[0] & 0xe000) === 0x2000)
        return 'global';
      if ((p.g[0] & 0xfe00) === 0xfc00)
        return 'ula';
    }
  }
  var ml = parsePrefix(network.mesh_local_prefix);
  if (ml !== null && underPrefix(g, ml)) {
    if (g[4] === 0 && g[5] === 0x00ff && g[6] === 0xfe00)
      return g[7] >= 0xfc00 ? 'aloc' : 'rloc';
    return 'mleid';
  }
  if ((g[0] & 0xffc0) === 0xfe80)
    return 'linklocal';
  if ((g[0] & 0xff00) === 0xff00)
    return 'multicast';
  return 'other';
}

/* ---- devices and links ---- */

function sortedBy(list, key) {
  return list.slice().sort(function(a, b) { return a[key] - b[key]; });
}

function hex4(n) {
  var s = Number(n).toString(16);
  while (s.length < 4)
    s = '0' + s;
  return s;
}

/**
 * One entry per router, stub and child:
 * {id, kind (br|leader|router|stub|med|sed), rloc16, ext, routerId (the router itself, or a child's parent), parentId,
 *  thisBoard, leader, borderRouter, sleepy, responded, detail, child (the raw child object, or null), addrs, version,
 *  mode}. Routers come in router-id order, each followed by its children in RLOC16 order.
 */
function devices(result) {
  var out = [];
  var routers = sortedBy((result && result.routers) || [], 'router_id');
  var net = (result && result.network) || {};
  var routerThis = routers.some(function(r) { return r.this_device === true; });
  routers.forEach(function(r) {
    var responded = r.responded !== false;
    var isLeader = r.leader === true || r.router_id === net.leader_router_id;
    var kind = r.this_device === true ? 'br' : !responded ? 'stub' : isLeader ? 'leader' : 'router';
    var id = 'r' + r.router_id;
    out.push({
      id : id,
      kind : kind,
      rloc16 : r.rloc16,
      ext : r.ext_address || null,
      routerId : r.router_id,
      parentId : null,
      thisBoard : r.this_device === true,
      leader : isLeader,
      borderRouter : r.border_router === true,
      sleepy : false,
      responded : responded,
      detail : responded && r.children_detail !== false,
      child : null,
      addrs : r.addresses || [],
      version : r.version === undefined ? null : r.version,
      mode : null
    });
    sortedBy(r.children || [], 'rloc16').forEach(function(c) {
      var mode = c.mode || null;
      var sleepy = mode !== null && mode.rx_on_when_idle === false;
      var board = !routerThis && c.rloc16 === net.this_rloc16;
      out.push({
        id : 'c' + c.rloc16,
        kind : board ? 'br' : sleepy ? 'sed' : 'med',
        rloc16 : c.rloc16,
        ext : c.ext_address || null,
        routerId : r.router_id,
        parentId : id,
        thisBoard : board,
        leader : false,
        borderRouter : board,
        sleepy : sleepy,
        responded : true,
        detail : c.timeout_s !== undefined,
        child : c,
        addrs : c.addresses || [],
        version : c.version === undefined ? null : c.version,
        mode : mode
      });
    });
  });
  return out;
}

/** [{a, b, lqAB, lqBA}] with a the lower router id; lqAB is b's lq_in for a. Links to unknown routers are dropped. */
function routerLinks(result) {
  var routers = (result && result.routers) || [];
  var known = {};
  routers.forEach(function(r) { known[r.router_id] = true; });
  var byKey = {};
  routers.forEach(function(r) {
    (r.links || []).forEach(function(l) {
      var other = l.router_id;
      if (!known[other] || other === r.router_id)
        return;
      var lo = Math.min(r.router_id, other), hi = Math.max(r.router_id, other);
      var key = lo + ':' + hi;
      var link = byKey[key] || (byKey[key] = {a : 'r' + lo, b : 'r' + hi, lqAB : 0, lqBA : 0, lo : lo, hi : hi});
      if (r.router_id === lo)
        link.lqBA = l.lq_in;
      else
        link.lqAB = l.lq_in;
    });
  });
  return Object.keys(byKey).map(function(k) { return byKey[k]; }).sort(function(x, y) {
    return x.lo - y.lo || x.hi - y.hi;
  }).map(function(l) { return {a : l.a, b : l.b, lqAB : l.lqAB, lqBA : l.lqBA}; });
}

/** "3" when both sides agree, else "3/2" in a's view first. */
function linkText(link) {
  return link.lqAB === link.lqBA ? String(link.lqAB) : link.lqAB + '/' + link.lqBA;
}

/** Colour of an LQI: 3 green, 2 amber, 1 red, anything else grey. */
function lqColor(q) {
  return q >= 3 ? '#1e7e34' : q === 2 ? '#a35f00' : q === 1 ? '#c62828' : '#5f5f5f';
}

/* ---- names ---- */

function has(obj, key) { return Object.prototype.hasOwnProperty.call(obj, key); }

/** The first SRP host that shares an address with the device, or null. */
function srpHostFor(device, srpHosts) {
  var mine = {};
  (device.addrs || []).forEach(function(a) { mine[String(a).toLowerCase()] = true; });
  var hosts = srpHosts || [];
  for (var i = 0; i < hosts.length; i++) {
    var addrs = hosts[i].addresses || [];
    for (var j = 0; j < addrs.length; j++) {
      if (mine[String(addrs[j]).toLowerCase()] === true)
        return hosts[i];
    }
  }
  return null;
}

/** Nickname, else SRP host name, else "Unnamed 0xNNNN". */
function displayName(device, names, srpHosts) {
  if (device.ext && names && has(names, device.ext) && names[device.ext])
    return names[device.ext];
  var host = srpHostFor(device, srpHosts);
  if (host && host.name)
    return host.name;
  return 'Unnamed 0x' + hex4(device.rloc16);
}

/* ---- table rows ---- */

function matches(device, query, names, srpHosts) {
  var q = query.toLowerCase();
  var host = srpHostFor(device, srpHosts);
  var fields = [ displayName(device, names, srpHosts), '0x' + hex4(device.rloc16), device.ext || '', host ? host.name : '' ]
                   .concat(device.addrs || []);
  return fields.some(function(f) { return String(f).toLowerCase().indexOf(q) >= 0; });
}

/** Table order: this board (when a router) and its children, then routers by id with their children. */
function rows(devs, filter, query, names, srpHosts) {
  var board = devs.filter(function(d) { return d.thisBoard && d.parentId === null; });
  var routers = devs.filter(function(d) { return d.parentId === null && !d.thisBoard; });
  var ordered = [];
  board.concat(routers).forEach(function(r) {
    ordered.push(r);
    devs.forEach(function(d) {
      if (d.parentId === r.id)
        ordered.push(d);
    });
  });
  var q = typeof query === 'string' ? query.trim() : '';
  return ordered.filter(function(d) {
    if (filter === 'routers' && d.parentId !== null)
      return false;
    if (filter === 'end' && d.parentId === null)
      return false;
    if (filter === 'sleepy' && !d.sleepy)
      return false;
    return q === '' || matches(d, q, names, srpHosts);
  });
}

/* ---- map layout ---- */

var MIN_WIDTH = 640, MIN_HEIGHT = 420, MARGIN = 90;
var RING_MIN = 150, RING_PER_ROUTER = 34;
var STEPS = 60, STEP_MAX = 20, REPEL = 2000000, ATTRACT = 250;
var CHILD_R = 80, CHILD_R_ALT = 15, ARC_PER_CHILD = 25, ARC_MAX = 270;

/** {width, height, pos: {id: {x, y}}}. Deterministic: no randomness anywhere. */
function layout(devs, links) {
  var routers = sortedBy(devs.filter(function(d) { return d.parentId === null; }), 'routerId');
  var pos = {};
  if (routers.length === 0)
    return {width : MIN_WIDTH, height : MIN_HEIGHT, pos : pos};

  var centre = routers.filter(function(d) { return d.thisBoard; })[0] ||
               routers.filter(function(d) { return d.leader; })[0] || routers[0];
  var others = routers.filter(function(d) { return d !== centre; });
  var ring = Math.max(RING_MIN, others.length * RING_PER_ROUTER);
  pos[centre.id] = {x : 0, y : 0};
  others.forEach(function(d, i) {
    var angle = -Math.PI / 2 + 2 * Math.PI * i / others.length;
    pos[d.id] = {x : ring * Math.cos(angle), y : ring * Math.sin(angle)};
  });

  var all = [ centre ].concat(others);
  var usable = (links || []).filter(function(l) { return pos[l.a] && pos[l.b]; });
  var force = {};
  function push(a, b, amount) { /* amount > 0 pushes a away from b */
    var fx = pos[a].x - pos[b].x, fy = pos[a].y - pos[b].y;
    var dist = Math.sqrt(fx * fx + fy * fy);
    if (dist < 1) {
      fx = 1; fy = 0; dist = 1;
    }
    force[a].x += amount * fx / dist;
    force[a].y += amount * fy / dist;
  }
  for (var step = 0; step < STEPS; step++) {
    all.forEach(function(d) { force[d.id] = {x : 0, y : 0}; });
    for (var i = 0; i < all.length; i++) {
      for (var j = i + 1; j < all.length; j++) {
        var dx = pos[all[i].id].x - pos[all[j].id].x, dy = pos[all[i].id].y - pos[all[j].id].y;
        var d2 = Math.max(1, dx * dx + dy * dy);
        push(all[i].id, all[j].id, REPEL / d2);
        push(all[j].id, all[i].id, REPEL / d2);
      }
    }
    usable.forEach(function(l) {
      var dx = pos[l.a].x - pos[l.b].x, dy = pos[l.a].y - pos[l.b].y;
      var pull = (dx * dx + dy * dy) / ATTRACT;
      push(l.a, l.b, -pull);
      push(l.b, l.a, -pull);
    });
    var limit = STEP_MAX * (1 - step / STEPS);
    others.forEach(function(d) {
      var f = force[d.id];
      var mag = Math.sqrt(f.x * f.x + f.y * f.y);
      var scale = mag > limit ? limit / mag : 1;
      pos[d.id].x += f.x * scale;
      pos[d.id].y += f.y * scale;
    });
  }

  routers.forEach(function(p) {
    var kids = sortedBy(devs.filter(function(d) { return d.parentId === p.id; }), 'rloc16');
    if (kids.length === 0)
      return;
    var away = p === centre ? Math.PI / 2 : Math.atan2(pos[p.id].y - pos[centre.id].y, pos[p.id].x - pos[centre.id].x);
    var arc = Math.min(ARC_MAX, kids.length * ARC_PER_CHILD) * Math.PI / 180;
    kids.forEach(function(c, i) {
      var angle = away - arc / 2 + arc * (i + 0.5) / kids.length;
      var r = CHILD_R + (kids.length > 6 && i % 2 === 1 ? CHILD_R_ALT : 0);
      pos[c.id] = {x : pos[p.id].x + r * Math.cos(angle), y : pos[p.id].y + r * Math.sin(angle)};
    });
  });

  var ids = Object.keys(pos);
  var minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity;
  ids.forEach(function(id) {
    minX = Math.min(minX, pos[id].x); maxX = Math.max(maxX, pos[id].x);
    minY = Math.min(minY, pos[id].y); maxY = Math.max(maxY, pos[id].y);
  });
  var width = Math.max(MIN_WIDTH, Math.ceil(maxX - minX + 2 * MARGIN));
  var height = Math.max(MIN_HEIGHT, Math.ceil(maxY - minY + 2 * MARGIN));
  var dx = (width - (maxX - minX)) / 2 - minX, dy = (height - (maxY - minY)) / 2 - minY;
  ids.forEach(function(id) {
    pos[id] = {x : Math.round(pos[id].x + dx), y : Math.round(pos[id].y + dy)};
  });
  return {width : width, height : height, pos : pos};
}

/* ---- browser storage (never throws) ---- */

function isResult(r) {
  return r !== null && typeof r === 'object' && Array.isArray(r.routers) && r.network !== null &&
         typeof r.network === 'object';
}

/** {savedAt, result} from `topology:last`, or null when missing, unreadable or of another shape. */
function savedResult(storage) {
  try {
    var raw = storage.getItem(LAST_KEY);
    if (typeof raw !== 'string')
      return null;
    var v = JSON.parse(raw);
    if (v === null || typeof v !== 'object' || typeof v.saved_at !== 'number' || !isResult(v.result))
      return null;
    return {savedAt : v.saved_at, result : v.result};
  } catch (e) {
    return null;
  }
}

function saveResult(storage, result, nowMs) {
  try {
    storage.setItem(LAST_KEY, JSON.stringify({saved_at : nowMs, result : result}));
    return true;
  } catch (e) {
    return false;
  }
}

/** {ext: nickname} from `topology:names`; {} when missing or unreadable. */
function loadNames(storage) {
  var out = {};
  try {
    var v = JSON.parse(storage.getItem(NAMES_KEY));
    if (v !== null && typeof v === 'object' && !Array.isArray(v)) {
      Object.keys(v).forEach(function(k) {
        if (typeof v[k] === 'string' && v[k] !== '')
          out[k] = v[k];
      });
    }
  } catch (e) {
    return {};
  }
  return out;
}

/** Stores the nickname of `ext`; an empty (or blank) name removes it. False when it could not be stored. */
function saveName(storage, ext, name) {
  try {
    if (typeof ext !== 'string' || ext === '')
      return false;
    var names = loadNames(storage);
    var clean = typeof name === 'string' ? name.trim() : '';
    if (clean === '')
      delete names[ext];
    else
      names[ext] = clean;
    if (Object.keys(names).length === 0)
      storage.removeItem(NAMES_KEY);
    else
      storage.setItem(NAMES_KEY, JSON.stringify(names));
    return true;
  } catch (e) {
    return false;
  }
}

/** The "last result": the board's done result when newer than the saved one, else the saved one (or null). */
function pickLast(saved, boardState, nowMs) {
  if (boardState && boardState.state === 'done' && isResult(boardState.result)) {
    var at = nowMs - (Number(boardState.age_s) || 0) * 1000;
    if (!saved || at > saved.savedAt)
      return {savedAt : at, result : boardState.result};
  }
  return saved || null;
}

/* ---- texts ---- */

function conflictText(status, body) {
  var detail = body && typeof body.detail === 'string' ? body.detail : '';
  if (status === 409 && /^mode /.test(detail))
    return "The Thread network isn't running (" + detail + ')';
  if (status === 409 && detail === 'mesh diagnostics are in use')
    return 'Another mesh scan is running, try again in a few seconds';
  return detail || (body && body.error) || 'Request failed (' + status + ')';
}

/** Lines for a /ping `result`: replies, loss, min/avg/max, and the sleepy note for sleepy devices. */
function pingSummary(ping, sleepy) {
  ping = ping || {};
  var stats = ping.statistics || {};
  var replies = ping.replies || [];
  var got = typeof stats.received === 'number' ? stats.received : replies.length;
  var sent = typeof stats.sent === 'number' ? stats.sent : replies.length;
  var lines = [ got + ' of ' + sent + ' replies' ];
  if (typeof stats.loss_percent === 'number')
    lines.push('Loss ' + Math.round(stats.loss_percent * 10) / 10 + '%');
  if (got > 0 && stats.rtt_min !== undefined)
    lines.push('min/avg/max ' + stats.rtt_min + '/' + stats.rtt_avg + '/' + stats.rtt_max + ' ms');
  if (sleepy)
    lines.push(SLEEPY_NOTE);
  return lines;
}

function pad2(n) { return n < 10 ? '0' + n : String(n); }

/** Local "HH:MM". */
function clockText(ms) {
  var d = new Date(ms);
  return pad2(d.getHours()) + ':' + pad2(d.getMinutes());
}

/** "just now", "38 min ago", "3 h ago", "2 d ago". */
function agoText(ms, nowMs) {
  var min = Math.floor(Math.max(0, nowMs - ms) / 60000);
  if (min < 1)
    return 'just now';
  if (min < 60)
    return min + ' min ago';
  if (min < 1440)
    return Math.floor(min / 60) + ' h ago';
  return Math.floor(min / 1440) + ' d ago';
}

return {
  addressClass : addressClass,
  devices : devices,
  routerLinks : routerLinks,
  linkText : linkText,
  lqColor : lqColor,
  displayName : displayName,
  srpHostFor : srpHostFor,
  rows : rows,
  layout : layout,
  savedResult : savedResult,
  saveResult : saveResult,
  loadNames : loadNames,
  saveName : saveName,
  pickLast : pickLast,
  conflictText : conflictText,
  pingSummary : pingSummary,
  clockText : clockText,
  agoText : agoText
};
})();

/*
 * The page. `env` is DeviceEnv plus `localStorage` (may be null or throw) and `navigator` (for the clipboard).
 * Views: start, loading and loaded. Everything from the board goes in through textContent or attribute setters.
 */
function TopologyPage(env) {
var C = typeof DeviceCommon !== 'undefined' ? DeviceCommon : require('./device_common.js');
var L = TopologyLogic;
var kit = C.DeviceKit(env, {});
var $ = kit.$, el = kit.el, clear = kit.clear, setText = kit.setText, show = kit.show;
var doc = env.document;

var SVG_NS = 'http://www.w3.org/2000/svg';
var POLL_MS = 1000;
var COPY_RESET_MS = 2000;
var LABEL_MAX = 22;
var BOARD_LABEL = 'This border router';
var NO_SAVED = 'No saved result in this browser yet.';
var NAMES_NOTE = "This browser can't save names. They last until the page is reloaded.";
var NO_DETAIL = "Details unavailable: its parent didn't answer";
var NO_ROUTE = "No routable address yet. Hosts on your home network can't reach this device.";
var MORE_TEXT = 'Show internal addresses (mesh-local, RLOC, link-local)';
var LESS_TEXT = 'Hide internal addresses';
var TYPE_TEXT = {
  br : 'Border router',
  leader : 'Router',
  router : 'Router',
  stub : 'Router (no answer)',
  med : 'End device',
  sed : 'Sleepy end device'
};
var FAILED_TEXT = {
  busy : 'Another mesh scan is running, try again in a few seconds',
  stopped : 'The scan stopped: the Thread network went down',
  no_memory : 'The board ran out of memory for the scan'
};
var ADDR_TAG = {global : 'Global', ula : 'ULA', mleid : 'Mesh-local', rloc : 'RLOC', linklocal : 'Link-local', other : 'Other'};
var THREAD_VERSION = {2 : '1.1', 3 : '1.2', 4 : '1.3', 5 : '1.4'};
var FILTERS = [ [ 'all', 'All' ], [ 'routers', 'Routers' ], [ 'end', 'End devices' ], [ 'sleepy', 'Sleepy' ] ];

var storage = null;
var storageOk = false;
var names = {};
var last = null;         /* the "last result": {savedAt, result} */
var model = null;        /* the result on screen: {result, net, srp, devs, byId, links, lay} */
var filter = 'all';
var selected = null;     /* device id with the selection ring */
var dialogOpen = false;
var showInternal = false;
var renaming = false;
var pingSeq = 0;
var scanning = false;    /* from Load (or joining a running scan) until the scan leaves `running` */
var pollTimer = null;
var pollFailures = 0;
var scanStarted = 0;
var dimmables = [];      /* map elements: {el, lit(focusId, near)} */
var rings = {};          /* device id -> its selection ring on the map */
var rowEls = {};         /* device id -> its table row */

/* ---- small helpers ---- */

function rlocText(n) { return '0x' + ('000' + Number(n).toString(16)).slice(-4); }

function plural(n, word) { return n + ' ' + word + (n === 1 ? '' : 's'); }

function orDash(v, unit) { return typeof v === 'number' ? v + (unit || '') : '–'; }

function pctText(v) { return typeof v === 'number' ? v.toFixed(1) + '%' : '–'; }

function button(text, cls, onClick) {
  var b = el('button', text, cls);
  b.setAttribute('type', 'button');
  b.addEventListener('click', onClick);
  return b;
}

function svg(tag, attrs, parent) {
  var e = doc.createElementNS(SVG_NS, tag);
  Object.keys(attrs).forEach(function(k) { e.setAttribute(k, String(attrs[k])); });
  if (parent)
    parent.appendChild(e);
  return e;
}

function nameOf(d) { return L.displayName(d, names, model.srp); }

function mapName(d) { return d.thisBoard ? BOARD_LABEL : nameOf(d); }

function unnamed(d) { return !(d.ext && names[d.ext]) && !L.srpHostFor(d, model.srp); }

function badgeText(d) { return d.thisBoard ? 'This board' : d.leader && d.parentId === null ? 'Leader' : ''; }

function counts(devs) {
  var routers = devs.filter(function(d) { return d.parentId === null; }).length;
  return {
    total : devs.length,
    routers : routers,
    end : devs.length - routers,
    sleepy : devs.filter(function(d) { return d.sleepy; }).length
  };
}

/* [{addr, cls}] without multicast and ALOC; routable (under an OMR prefix) or internal. */
function addresses(d) {
  return d.addrs.map(function(a) { return {addr : a, cls : L.addressClass(a, model.net)}; }).filter(function(x) {
    return x.cls !== 'multicast' && x.cls !== 'aloc';
  });
}

function isRoutable(x) { return x.cls === 'global' || x.cls === 'ula'; }

/* ---- storage ---- */

function probeStorage(s) {
  try {
    s.setItem('topology:probe', '1');
    s.removeItem('topology:probe');
    return true;
  } catch (e) {
    return false;
  }
}

/* ---- clipboard: the Clipboard API needs a secure context; the board serves plain http, so fall back ---- */

function legacyCopy(text) {
  try {
    var host = dialogOpen ? $('t-dialog') : doc.body;
    if (!doc.execCommand || !host)
      return false;
    var area = el('textarea', null, 't-offscreen');
    area.value = text;
    area.setAttribute('readonly', '');
    host.appendChild(area);
    area.select();
    var ok = doc.execCommand('copy');
    host.removeChild(area);
    return ok;
  } catch (e) {
    return false;
  }
}

function copyText(text) {
  var clip = env.navigator && env.navigator.clipboard;
  if (clip && clip.writeText)
    return clip.writeText(text);
  return legacyCopy(text) ? Promise.resolve() : Promise.reject(new Error('no clipboard'));
}

function copyButton(addr, cls) {
  var b = button('Copy', cls, function() {
    copyText(addr).then(function() { b.textContent = 'Copied'; }, function() { b.textContent = 'Copy failed'; })
        .then(function() { env.setTimeout(function() { b.textContent = 'Copy'; }, COPY_RESET_MS); });
  });
  return b;
}

/* ---- views ---- */

function view(name) {
  show('t-start', name === 'start');
  show('t-loading', name === 'loading');
  show('t-loaded', name === 'loaded');
  show('t-status', name === 'loaded');
}

function renderStart(noteText) {
  if (last) {
    var c = counts(L.devices(last.result));
    setText('t-last-line', 'Last result: ' + L.clockText(last.savedAt) + ' (' + L.agoText(last.savedAt, env.now()) +
                               '), ' + plural(c.routers, 'router') + ' and ' + plural(c.end, 'end device'));
  } else {
    setText('t-last-line', NO_SAVED);
  }
  show('t-show-last', last !== null);
  kit.note('t-start-note', noteText || '', !!noteText);
  view('start');
}

function renderProgress(state) {
  var p = (state && state.progress) || {};
  if (p.routers > 0) {
    setText('t-progress', 'Asking each router about its children: ' + p.queried + ' of ' + p.routers);
    $('t-progress-bar').style.width = Math.min(100, Math.round(100 * p.queried / p.routers)) + '%';
  } else {
    setText('t-progress', 'Finding routers…');
  }
  show('t-progress-track', p.routers > 0);
  setText('t-elapsed', 'Elapsed: ' + Math.max(0, Math.round((env.now() - scanStarted) / 1000)) + ' s');
  view('loading');
}

/* ---- scan: POST once, then GET every second while running ---- */

function failedText(error) { return FAILED_TEXT[error] || 'The scan failed (' + (error || 'unknown') + ')'; }

function follow(state) {
  if (state && state.state === 'running') {
    renderProgress(state);
    schedulePoll(POLL_MS);
    return;
  }
  scanning = false;
  if (state && state.state === 'done' && state.result) {
    var at = env.now() - (Number(state.age_s) || 0) * 1000;
    last = {savedAt : at, result : state.result};
    L.saveResult(storage, state.result, at);
    showResult(last, true);
  } else {
    renderStart(state && state.state === 'failed' ? failedText(state.error) : '');
  }
}

function schedulePoll(ms) {
  if (pollTimer === null)
    pollTimer = env.setTimeout(poll, ms);
}

function poll() {
  pollTimer = null;
  kit.request('GET', '/device/topology', {timeoutMs : 5000}).then(function(r) {
    if (r.status === 200 && r.body) {
      pollFailures = 0;
      follow(r.body);
    } else {
      retry();
    }
  }, retry);
}

function retry() {
  pollFailures++;
  schedulePoll(C.nextDelay(POLL_MS, pollFailures));
}

function joinScan(state) {
  scanning = true;
  pollFailures = 0;
  scanStarted = env.now();
  follow(state);
}

function load() {
  if (scanning)
    return;
  scanning = true;
  scanStarted = env.now();
  renderProgress(null);
  kit.request('POST', '/device/topology', {timeoutMs : 5000}).then(function(r) {
    if ((r.status === 202 || r.status === 200) && r.body) {
      follow(r.body);
    } else {
      scanning = false;
      renderStart(L.conflictText(r.status, r.body));
    }
  }, function(e) {
    scanning = false;
    renderStart(e.message);
  });
}

/* ---- loaded view ---- */

function showResult(rec, live) {
  var devs = L.devices(rec.result);
  var links = L.routerLinks(rec.result);
  var byId = {};
  devs.forEach(function(d) { byId[d.id] = d; });
  model = {
    rec : rec,
    live : live,
    result : rec.result,
    net : rec.result.network || {},
    srp : rec.result.srp_hosts || [],
    devs : devs,
    byId : byId,
    links : links,
    lay : L.layout(devs, links)
  };
  if (selected !== null && !byId[selected])
    selected = null;
  renderPill();
  renderAll();
  view('loaded');
}

function renderPill() {
  var text = (model.live ? 'Loaded at ' + L.clockText(model.rec.savedAt)
                         : 'Saved result from ' + L.clockText(model.rec.savedAt) + ', ' +
                               L.agoText(model.rec.savedAt, env.now()));
  if (model.result.partial)
    text += ", some routers didn't answer";
  setText('t-pill', text);
  $('t-pill').classList.toggle('t-pill-live', model.live);
  $('t-pill').classList.toggle('t-pill-saved', !model.live);
  setText('t-reload', model.live ? 'Load again' : 'Load fresh');
}

/* Everything that shows names: after a load and after a rename. */
function renderAll() {
  renderTiles();
  renderMap();
  renderChips();
  renderRows();
  if (dialogOpen)
    renderDialog();
  applyFocus();
}

function tile(id, label, value, sub) {
  var t = $(id);
  clear(t);
  t.appendChild(el('div', label, 't-tile-label'));
  if (value !== null)
    t.appendChild(el('div', value, 't-tile-value'));
  (sub || []).forEach(function(n) { t.appendChild(n); });
}

function renderTiles() {
  var net = model.net;
  var c = counts(model.devs);
  var partition = typeof net.partition_id === 'number'
                      ? '0x' + ('0000000' + (net.partition_id >>> 0).toString(16)).slice(-8)
                      : '–';
  var networkSub = 'Channel ' + orDash(net.channel) + ' · partition ' + partition;
  tile('t-tile-network', 'Network', net.name || '–', [ el('div', networkSub, 't-tile-sub') ]);
  var devicesSub = plural(c.routers, 'router') + ' · ' + plural(c.end, 'end device') + ', ' + c.sleepy +
                   ' of them sleepy';
  tile('t-tile-devices', 'Devices', String(c.total), [ el('div', devicesSub, 't-tile-sub') ]);
  var leader = model.devs.filter(function(d) { return d.leader && d.parentId === null; })[0];
  tile('t-tile-leader', 'Leader', leader ? nameOf(leader) : '–',
       leader ? [ el('div', rlocText(leader.rloc16), 't-tile-sub t-mono') ] : []);
  var prefixes = (net.omr_prefixes || []).map(function(p) {
    var line = el('div', null, 't-tile-prefix t-mono');
    line.appendChild(el('span', p));
    var cls = L.addressClass(String(p).split('/')[0], net);
    if (cls === 'global' || cls === 'ula')
      line.appendChild(el('span', cls === 'global' ? 'global' : 'ULA', 't-tile-kind'));
    return line;
  });
  tile('t-tile-prefixes', 'Routable prefixes', prefixes.length ? null : 'None', prefixes);
}

/* ---- map ---- */

function neighbourhood(id) {
  var near = {};
  if (id === null)
    return near;
  near[id] = true;
  model.links.forEach(function(l) {
    if (l.a === id)
      near[l.b] = true;
    if (l.b === id)
      near[l.a] = true;
  });
  model.devs.forEach(function(d) {
    if (d.parentId === id)
      near[d.id] = true;
  });
  var dev = model.byId[id];
  if (dev && dev.parentId)
    near[dev.parentId] = true;
  return near;
}

function shortLabel(text) { return text.length > LABEL_MAX ? text.slice(0, LABEL_MAX - 1) + '…' : text; }

/* Draws the node shape at the origin of `g`; returns its half height. */
function drawShape(g, d) {
  if (d.kind === 'br' && d.parentId === null) {
    svg('rect', {x : -20, y : -20, width : 40, height : 40, rx : 10, fill : '#b35c00', stroke : '#8a4700', 'stroke-width' : 2}, g);
    svg('text', {'class' : 't-node-br', 'text-anchor' : 'middle', 'dominant-baseline' : 'central'}, g).textContent = 'BR';
    return 20;
  }
  if (d.kind === 'br') {
    svg('rect', {x : -9, y : -9, width : 18, height : 18, rx : 4, fill : '#b35c00', stroke : '#8a4700', 'stroke-width' : 2}, g);
    return 9;
  }
  if (d.parentId !== null) {
    var ring = {r : 9, fill : '#fff', stroke : '#1f5f99', 'stroke-width' : 2.5};
    if (d.sleepy)
      ring['stroke-dasharray'] = '3 2';
    svg('circle', ring, g);
    return 9;
  }
  var stub = d.kind === 'stub';
  svg('circle', {r : 16, fill : stub ? '#9a9a9a' : '#1f5f99', stroke : stub ? '#7a7a7a' : '#174a77', 'stroke-width' : 2}, g);
  if (d.leader) {
    svg('circle', {r : 21, fill : 'none', stroke : stub ? '#9a9a9a' : '#1f5f99', 'stroke-width' : 2}, g);
    return 21;
  }
  return 16;
}

function drawNode(layer, d, p) {
  var g = svg('g', {'class' : 't-node', tabindex : 0, role : 'button', 'data-id' : d.id,
                    transform : 'translate(' + p.x + ',' + p.y + ')'},
              layer);
  g.setAttribute('aria-label', mapName(d) + ', ' + TYPE_TEXT[d.kind]);
  var half = drawShape(g, d);
  var ring = d.kind === 'br' && d.parentId === null
                 ? svg('rect', {x : -26, y : -26, width : 52, height : 52, rx : 14, 'class' : 't-ring hidden'}, g)
                 : svg('circle', {r : half + 6, 'class' : 't-ring hidden'}, g);
  rings[d.id] = ring;
  var label = svg('text', {'class' : d.parentId === null ? 't-label t-label-router' : 't-label', y : half + 16,
                           'text-anchor' : 'middle'},
                  g);
  label.textContent = shortLabel(mapName(d));
  if (d.kind === 'stub') {
    svg('text', {'class' : 't-label t-label-note', y : half + 30, 'text-anchor' : 'middle'}, g).textContent =
        "didn't answer";
  }
  g.addEventListener('click', function() { openDevice(d.id); });
  g.addEventListener('keydown', function(ev) {
    if (ev.key === 'Enter' || ev.key === ' ' || ev.key === 'Spacebar') {
      ev.preventDefault();
      openDevice(d.id);
    }
  });
  dimmables.push({el : g, lit : function(f, near) { return near[d.id] === true; }});
}

function drawLink(layer, l, pos) {
  var a = pos[l.a], b = pos[l.b];
  var q = Math.min(l.lqAB, l.lqBA);
  var color = L.lqColor(q);
  var g = svg('g', {'class' : 't-link-edge'}, layer);
  var line = {x1 : a.x, y1 : a.y, x2 : b.x, y2 : b.y, stroke : color, 'stroke-width' : q >= 3 ? 3 : 2};
  if (q <= 1)
    line['stroke-dasharray'] = '6 4';
  svg('line', line, g);
  var text = L.linkText(l);
  var w = 10 + 7 * text.length;
  var mx = Math.round((a.x + b.x) / 2), my = Math.round((a.y + b.y) / 2);
  svg('rect', {x : mx - w / 2, y : my - 8, width : w, height : 16, rx : 8, fill : '#fff', stroke : '#e0e0e0'}, g);
  svg('text', {'class' : 't-badge-text', x : mx, y : my, fill : color, 'text-anchor' : 'middle',
               'dominant-baseline' : 'central'},
      g)
      .textContent = text;
  dimmables.push({el : g, lit : function(f) { return f === l.a || f === l.b; }});
}

/* The dashed line from this board to the "Home network · Ethernet" box below it. */
function drawUplink(layer, board, p, lay) {
  var g = svg('g', {'class' : 't-uplink'}, layer);
  var cx = Math.min(Math.max(p.x, 92), lay.width - 92);
  var cy = board.parentId === null ? lay.height - 28 : Math.min(lay.height - 16, p.y + 60);
  svg('line', {x1 : p.x, y1 : p.y, x2 : cx, y2 : cy - 13, stroke : '#b35c00', 'stroke-width' : 2, 'stroke-dasharray' : '6 4'}, g);
  svg('rect', {x : cx - 90, y : cy - 13, width : 180, height : 26, rx : 6, fill : '#fff8f0', stroke : '#b35c00',
               'stroke-dasharray' : '4 3'},
      g);
  svg('text', {'class' : 't-uplink-text', x : cx, y : cy, 'text-anchor' : 'middle', 'dominant-baseline' : 'central'}, g)
      .textContent = 'Home network · Ethernet';
  dimmables.push({el : g, lit : function(f) { return f === board.id; }});
}

function renderMap() {
  var map = $('t-map');
  var lay = model.lay, pos = lay.pos;
  clear(map);
  dimmables = [];
  rings = {};
  map.setAttribute('width', lay.width);
  map.setAttribute('height', lay.height);
  map.setAttribute('viewBox', '0 0 ' + lay.width + ' ' + lay.height);
  var edges = svg('g', {}, map), nodes = svg('g', {}, map);
  var board = model.devs.filter(function(d) { return d.thisBoard; })[0];
  if (board && pos[board.id])
    drawUplink(edges, board, pos[board.id], lay);
  model.devs.forEach(function(d) {
    if (d.parentId === null || !pos[d.id] || !pos[d.parentId])
      return;
    var line = svg('line', {x1 : pos[d.parentId].x, y1 : pos[d.parentId].y, x2 : pos[d.id].x, y2 : pos[d.id].y,
                            stroke : '#9a9a9a', 'stroke-width' : 1.5},
                   edges);
    dimmables.push({el : line, lit : function(f) { return f === d.id || f === d.parentId; }});
  });
  model.links.forEach(function(l) {
    if (pos[l.a] && pos[l.b])
      drawLink(edges, l, pos);
  });
  model.devs.forEach(function(d) {
    if (pos[d.id])
      drawNode(nodes, d, pos[d.id]);
  });
}

/* Dims what lies outside the open device's neighbourhood; the ring and the row tint follow the selection. */
function applyFocus() {
  if (model === null)
    return;
  var focus = dialogOpen ? selected : null;
  var near = neighbourhood(focus);
  dimmables.forEach(function(x) { x.el.classList.toggle('t-dim', focus !== null && !x.lit(focus, near)); });
  Object.keys(rings).forEach(function(id) { rings[id].classList.toggle('hidden', id !== selected); });
  Object.keys(rowEls).forEach(function(id) { rowEls[id].classList.toggle('t-row-sel', id === selected); });
}

/* ---- table ---- */

function renderChips() {
  var box = $('t-chips');
  clear(box);
  FILTERS.forEach(function(f) {
    var n = L.rows(model.devs, f[0], '', names, model.srp).length;
    var chip = button(null, 't-chip', function() {
      filter = f[0];
      renderChips();
      renderRows();
    });
    chip.setAttribute('aria-pressed', filter === f[0] ? 'true' : 'false');
    chip.setAttribute('data-filter', f[0]);
    chip.appendChild(el('span', f[1]));
    chip.appendChild(el('span', String(n), 't-chip-count'));
    box.appendChild(chip);
  });
}

function viaText(d) {
  if (d.parentId !== null)
    return nameOf(model.byId[d.parentId]);
  var n = model.links.filter(function(l) { return l.a === d.id || l.b === d.id; }).length;
  return plural(n, 'router neighbor');
}

function addressCell(d) {
  var td = el('td', null, 't-cell-addrs');
  var routable = addresses(d).filter(isRoutable);
  routable.forEach(function(x) {
    var line = el('div', null, 't-addr');
    line.appendChild(el('span', ADDR_TAG[x.cls], 't-tag'));
    line.appendChild(el('span', x.addr, 't-mono t-addr-text'));
    line.appendChild(copyButton(x.addr, 't-small-btn'));
    td.appendChild(line);
  });
  if (routable.length === 0)
    td.appendChild(el('span', 'None yet', 't-none'));
  return td;
}

function rowFor(d) {
  var tr = el('tr');
  tr.setAttribute('data-id', d.id);
  var dev = el('td', null, d.parentId !== null ? 't-cell-dev t-child' : 't-cell-dev');
  dev.appendChild(button((d.parentId !== null ? '↳ ' : '') + nameOf(d), unnamed(d) ? 't-name t-unnamed' : 't-name',
                         function() { openDevice(d.id); }));
  var badge = badgeText(d);
  if (badge)
    dev.appendChild(el('span', badge, 'chip chip-warn t-badge'));
  dev.appendChild(el('div', rlocText(d.rloc16) + ' · ' + (d.ext || '–'), 't-mono t-sub'));
  tr.appendChild(dev);
  tr.appendChild(el('td', TYPE_TEXT[d.kind]));
  tr.appendChild(el('td', viaText(d)));
  var lq = d.child && typeof d.child.lq_in === 'number' ? d.child.lq_in : null;
  var lqCell = el('td', lq === null ? '–' : String(lq), 't-lq');
  if (lq !== null)
    lqCell.style.color = L.lqColor(lq);
  tr.appendChild(lqCell);
  tr.appendChild(addressCell(d));
  rowEls[d.id] = tr;
  return tr;
}

function renderRows() {
  var body = $('t-rows');
  var query = String($('t-search').value || '').trim();
  clear(body);
  rowEls = {};
  var list = L.rows(model.devs, filter, query, names, model.srp);
  list.forEach(function(d) { body.appendChild(rowFor(d)); });
  setText('t-empty', query ? "No device matches '" + query + "'" : 'No devices of this kind.');
  show('t-empty', list.length === 0);
  applyFocus();
}

/* ---- dialog ---- */

function openDevice(id) {
  if (!model.byId[id])
    return;
  if (selected !== id || !dialogOpen) {
    selected = id;
    showInternal = false;
    renaming = false;
    clearPing();
  }
  dialogOpen = true;
  renderDialog();
  var dlg = $('t-dialog');
  if (!dlg.open)
    dlg.showModal();
  applyFocus();
}

function onDialogClosed() {
  dialogOpen = false;
  renaming = false;
  clearPing();
  applyFocus();
}

function linkButton(d) {
  return button(nameOf(d), 't-link', function() { openDevice(d.id); });
}

function radioText(d) {
  if (d.parentId === null)
    return 'Full Thread device, always on';
  if (!d.mode)
    return '–';
  if (!d.sleepy)
    return 'Always on, child of a router';
  var timeout = d.child && d.child.timeout_s;
  return typeof timeout === 'number' ? 'Off between polls, child timeout ' + timeout + ' s' : 'Off between polls';
}

function versionText(v) {
  if (typeof v !== 'number')
    return '–';
  return THREAD_VERSION[v] || String(v);
}

function fact(dl, label, value, cls) {
  dl.appendChild(el('dt', label));
  var dd = el('dd', typeof value === 'string' ? value : null, cls);
  if (typeof value !== 'string')
    dd.appendChild(value);
  dl.appendChild(dd);
}

function renderFacts(d) {
  var box = $('t-d-facts');
  clear(box);
  var dl = el('dl', null, 'facts');
  fact(dl, 'RLOC16', rlocText(d.rloc16), 't-mono');
  fact(dl, 'Ext. address', d.ext || '–', 't-mono');
  var host = L.srpHostFor(d, model.srp);
  fact(dl, 'SRP host', host ? host.name + '.default.service.arpa' : 'Not registered', host ? 't-mono' : '');
  if (host && host.services && host.services.length) {
    fact(dl, 'Services', host.services.map(function(s) { return s.instance + ' (' + s.type + ')'; }).join(', '));
  }
  fact(dl, 'Radio', radioText(d));
  fact(dl, 'Thread version', versionText(d.version));
  box.appendChild(dl);
  if (d.parentId === null)
    return;
  fact(dl, 'Parent', linkButton(model.byId[d.parentId]));
  var lq = d.child && typeof d.child.lq_in === 'number' ? d.child.lq_in : null;
  var lqText = el('span', lq === null ? '–' : String(lq), 't-lq');
  if (lq !== null)
    lqText.style.color = L.lqColor(lq);
  fact(dl, 'LQI', lqText);
  if (!d.detail) {
    box.appendChild(el('p', NO_DETAIL, 't-warn'));
    return;
  }
  var c = d.child;
  fact(dl, 'RSSI', 'avg ' + orDash(c.rssi_avg, ' dBm') + ', last ' + orDash(c.rssi_last, ' dBm'));
  fact(dl, 'Link margin', orDash(c.link_margin_db, ' dB'));
  fact(dl, 'Frame errors', pctText(c.frame_error_pct));
  fact(dl, 'Message errors', pctText(c.message_error_pct));
  fact(dl, 'Attached for', typeof c.connected_s === 'number' ? C.durationText(c.connected_s) : '–');
}

function renderRouterColumn(d) {
  show('t-d-router', d.parentId === null);
  var nbBox = $('t-d-neighbors'), kidBox = $('t-d-children');
  clear(nbBox);
  clear(kidBox);
  if (d.parentId !== null)
    return;
  model.links.forEach(function(l) {
    if (l.a !== d.id && l.b !== d.id)
      return;
    var mine = l.a === d.id;
    var other = model.byId[mine ? l.b : l.a];
    var qin = mine ? l.lqBA : l.lqAB, qout = mine ? l.lqAB : l.lqBA;
    var line = el('div', null, 't-d-item');
    line.appendChild(linkButton(other));
    var q = el('span', qin === qout ? 'LQI ' + qin : 'LQI ' + qin + ' in / ' + qout + ' out');
    q.style.color = L.lqColor(Math.min(qin, qout));
    line.appendChild(q);
    nbBox.appendChild(line);
  });
  model.devs.forEach(function(c) {
    if (c.parentId !== d.id)
      return;
    var line = el('div', null, 't-d-item');
    line.appendChild(linkButton(c));
    line.appendChild(el('span', c.sleepy ? 'Sleepy' : 'Always on', 't-muted'));
    kidBox.appendChild(line);
  });
  if (!nbBox.firstChild)
    nbBox.appendChild(el('span', 'None', 't-muted'));
  if (!kidBox.firstChild)
    kidBox.appendChild(el('span', 'None', 't-muted'));
}

function renderAddresses(d) {
  var all = addresses(d);
  var routable = all.filter(isRoutable);
  var internal = all.filter(function(x) { return !isRoutable(x); });
  var box = $('t-d-addrs');
  clear(box);
  routable.concat(showInternal ? internal : []).forEach(function(x) {
    var line = el('div', null, 't-d-addr');
    line.appendChild(el('span', ADDR_TAG[x.cls], isRoutable(x) ? 't-tag' : 't-tag t-tag-internal'));
    line.appendChild(el('span', x.addr, 't-mono t-addr-text'));
    line.appendChild(copyButton(x.addr, 't-small-btn'));
    line.appendChild(button('Ping', 't-small-btn', function() { ping(x.addr, d); }));
    box.appendChild(line);
  });
  setText('t-d-noroute', routable.length === 0 ? NO_ROUTE : '');
  show('t-d-noroute', routable.length === 0);
  show('t-d-more', internal.length > 0);
  setText('t-d-more', showInternal ? LESS_TEXT : MORE_TEXT);
}

function renderDialog() {
  var d = model.byId[selected];
  setText('t-d-name', nameOf(d));
  $('t-d-name').classList.toggle('t-unnamed', unnamed(d));
  show('t-d-rename', !!d.ext && !renaming);
  show('t-d-rename-form', renaming);
  setText('t-names-note', storageOk ? '' : NAMES_NOTE);
  var chips = $('t-d-chips');
  clear(chips);
  chips.appendChild(el('span', TYPE_TEXT[d.kind], 'chip t-chip-type'));
  if (badgeText(d))
    chips.appendChild(el('span', badgeText(d), 'chip chip-warn'));
  renderFacts(d);
  renderRouterColumn(d);
  renderAddresses(d);
}

function startRename() {
  var d = model.byId[selected];
  renaming = true;
  $('t-d-name-input').value = names[d.ext] || '';
  renderDialog();
  $('t-d-name-input').focus();
}

function saveRename() {
  var d = model.byId[selected];
  var clean = String($('t-d-name-input').value || '').trim();
  L.saveName(storage, d.ext, clean);
  /* Kept in memory too, so a rename works for the page's life when the browser can't store it. */
  if (clean === '')
    delete names[d.ext];
  else
    names[d.ext] = clean;
  renaming = false;
  renderAll();
}

/* ---- ping ---- */

function clearPing() {
  pingSeq++;
  show('t-d-ping', false);
  clear($('t-d-ping'));
}

function ping(addr, d) {
  var seq = ++pingSeq;
  var box = $('t-d-ping');
  clear(box);
  box.appendChild(el('div', 'ping ' + addr, 't-mono t-ping-addr'));
  var out = el('div', null, 't-ping-out');
  out.appendChild(el('div', 'Pinging…'));
  box.appendChild(out);
  show('t-d-ping', true);
  kit.request('POST', '/ping', {
       headers : {'Content-Type' : 'application/json'},
       body : JSON.stringify({address : addr, count : 4, size : 32}),
       timeoutMs : 20000
     })
      .then(function(r) {
        var b = r.body;
        if (r.status === 200 && b && b.error === 0 && b.result)
          return L.pingSummary(b.result, d.sleepy);
        return [ (b && b.message) || 'Ping failed (' + r.status + ')' ];
      }, function(e) { return [ e.message ]; })
      .then(function(lines) {
        if (seq !== pingSeq)
          return;
        clear(out);
        lines.forEach(function(t) { out.appendChild(el('div', t)); });
      });
}

/* ---- start ---- */

function wire() {
  $('t-load').addEventListener('click', load);
  $('t-reload').addEventListener('click', load);
  $('t-show-last').addEventListener('click', function() {
    if (last)
      showResult(last, false);
  });
  $('t-search').addEventListener('input', renderRows);
  var dlg = $('t-dialog');
  dlg.addEventListener('close', onDialogClosed);
  /* A click on the dialog element itself is a click on the backdrop: the content sits in .t-d-body. */
  dlg.addEventListener('click', function(ev) {
    if (ev.target === dlg)
      dlg.close();
  });
  $('t-d-x').addEventListener('click', function() { dlg.close(); });
  $('t-d-close').addEventListener('click', function() { dlg.close(); });
  $('t-d-showmap').addEventListener('click', function() {
    dlg.close();
    $('t-map-card').scrollIntoView({behavior : 'smooth', block : 'start'});
  });
  $('t-d-rename').addEventListener('click', startRename);
  $('t-d-save').addEventListener('click', saveRename);
  $('t-d-name-input').addEventListener('keydown', function(ev) {
    if (ev.key === 'Enter') {
      ev.preventDefault();
      saveRename();
    }
  });
  $('t-d-more').addEventListener('click', function() {
    showInternal = !showInternal;
    renderAddresses(model.byId[selected]);
  });
}

function init() {
  kit.start();
  storage = env.localStorage || null;
  storageOk = probeStorage(storage);
  names = L.loadNames(storage);
  var saved = L.savedResult(storage);
  wire();
  /* One GET: it never starts mesh traffic. A running scan is joined, never restarted. */
  kit.request('GET', '/device/topology', {timeoutMs : 5000}).then(function(r) {
    var board = r.status === 200 ? r.body : null;
    last = L.pickLast(saved, board, env.now());
    if (board && board.state === 'running')
      joinScan(board);
    else
      renderStart('');
  }, function() {
    last = saved;
    renderStart('');
  });
}

return {init : init, kit : kit};
}

if (typeof module !== 'undefined' && module.exports) {
  module.exports = TopologyLogic;
  module.exports.Page = TopologyPage;
}

if (typeof document !== 'undefined') {
  var topologyEnv = DeviceCommon.DeviceEnv();
  try {
    topologyEnv.localStorage = window.localStorage;
  } catch (e) {
    topologyEnv.localStorage = null;
  }
  topologyEnv.navigator = navigator;
  TopologyPage(topologyEnv).init();
}
