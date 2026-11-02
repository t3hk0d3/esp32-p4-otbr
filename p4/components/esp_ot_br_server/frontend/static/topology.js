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
  return q >= 3 ? '#1e7e34' : q === 2 ? '#b35c00' : q === 1 ? '#dc3545' : '#999999';
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

function TopologyPage(env) { return {}; }

if (typeof module !== 'undefined' && module.exports) {
  module.exports = TopologyLogic;
  module.exports.Page = TopologyPage;
}
