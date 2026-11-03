/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP Thread Border Router - Ethernet page (GET/PUT /device/network, POST /device/network/confirm)
 *
 * EthernetLogic is pure and runs under node too (tools/tests/test_ethernet_page.py). EthernetPage is the page.
 *
 * The page edits a draft: {mode: 'single'|'split', single|mgmt|thread_bb: {tagged, vlan, dhcp, cidr, gw, dns}} with the
 * form strings as typed. It is filled once from the running configuration (the trial when one runs, else the active
 * one), compared with it for the review, and turned into the PUT body of the API only on Apply.
 */
'use strict';

var EthernetLogic = (function() {
var C = typeof DeviceCommon !== 'undefined' ? DeviceCommon : require('./device_common.js');
var VLAN_MAX = 4094;
var IDS = [ 'single', 'mgmt', 'thread_bb' ];
var NAMES = {single : 'Ethernet port', mgmt : 'Management', thread_bb : 'Thread backbone'};
var API_NAMES = {single : 'eth', mgmt : 'mgmt', thread_bb : 'thread_bb'};
var MODE_NAMES = {single : 'Single port', split : 'Split management and Thread'};

function ipv4Valid(text) {
  var parts = String(text).trim().split('.');
  if (parts.length !== 4)
    return false;
  return parts.every(function(p) { return /^(0|[1-9]\d{0,2})$/.test(p) && Number(p) <= 255; });
}

function ipv4ToNumber(text) {
  return String(text).trim().split('.').reduce(function(acc, p) { return acc * 256 + Number(p); }, 0);
}

/** Not 0.0.0.0, not loopback, not multicast or above: what the board accepts as an address (usable_unicast). */
function usableUnicast(n) {
  var first = n >>> 24;
  return n !== 0 && first < 224 && first !== 127;
}

/** A netmask is valid when it is dotted-quad, not 0.0.0.0, and its one bits are contiguous. */
function maskValid(text) {
  if (!ipv4Valid(text))
    return false;
  var m = ipv4ToNumber(text);
  if (m === 0)
    return false;
  var inverted = (~m) >>> 0;
  return (inverted & (inverted + 1)) === 0;
}

/** Dotted netmask of a prefix length 1..32, or null. */
function prefixToMask(prefix) {
  if (!(prefix >= 1 && prefix <= 32) || Math.floor(prefix) !== prefix)
    return null;
  var m = prefix === 32 ? 0xffffffff : (0xffffffff << (32 - prefix)) >>> 0;
  return [ m >>> 24, (m >>> 16) & 255, (m >>> 8) & 255, m & 255 ].join('.');
}

/** Prefix length of a valid netmask, or null. */
function maskToPrefix(mask) {
  if (!maskValid(mask))
    return null;
  var m = ipv4ToNumber(mask), n = 0;
  while (m & 0x80000000) {
    n++;
    m = (m << 1) >>> 0;
  }
  return n;
}

/** {addr, mask} of "a.b.c.d/nn", or null. */
function parseCidr(text) {
  var m = /^([^\/]+)\/(\d{1,2})$/.exec(String(text).trim());
  if (!m || !ipv4Valid(m[1]))
    return null;
  var mask = prefixToMask(Number(m[2]));
  return mask ? {addr : m[1].trim(), mask : mask} : null;
}

/** Interface object for the API from form strings; throws Error with a message the user can act on. */
function ifaceConfig(name, f) {
  var vlan = C.wholeNumber(f.vlan, 0, VLAN_MAX);
  if (vlan === null)
    throw new Error(name + ': VLAN must be a whole number from 0 (untagged) to ' + VLAN_MAX + '.');
  if (f.dhcp)
    return {vlan : vlan, ipv4 : {dhcp : true}};
  var addr = String(f.addr || '').trim(), mask = String(f.mask || '').trim();
  if (!addr || !mask)
    throw new Error(name + ': a static address needs an address and a netmask.');
  if (!ipv4Valid(addr))
    throw new Error(name + ': the address is not a dotted-quad IPv4 address.');
  if (!maskValid(mask))
    throw new Error(name + ': the netmask is not valid (for example 255.255.255.0).');
  var ipv4 = {dhcp : false, addr : addr, mask : mask};
  var gw = String(f.gw || '').trim(), dns = String(f.dns || '').trim();
  if (gw && !ipv4Valid(gw))
    throw new Error(name + ': the gateway is not a dotted-quad IPv4 address.');
  if (dns && !ipv4Valid(dns))
    throw new Error(name + ': the DNS server is not a dotted-quad IPv4 address.');
  if (gw)
    ipv4.gw = gw;
  if (dns)
    ipv4.dns = dns;
  return {vlan : vlan, ipv4 : ipv4};
}

/** Body of PUT /device/network from {mode, single|mgmt|thread_bb: {vlan, dhcp, addr, mask, gw, dns}}. */
function networkConfig(form) {
  if (form.mode === 'single')
    return {mode : 'single', single : ifaceConfig(NAMES.single, form.single || {})};
  if (form.mode === 'split') {
    var mgmt = ifaceConfig(NAMES.mgmt, form.mgmt || {});
    var bb = ifaceConfig(NAMES.thread_bb, form.thread_bb || {});
    if (mgmt.vlan === bb.vlan)
      throw new Error('Management and Thread backbone need different VLAN IDs (two untagged interfaces ' +
                      'are not possible either).');
    return {mode : 'split', mgmt : mgmt, thread_bb : bb};
  }
  throw new Error('Choose a setup.');
}

function ifaceIds(mode) { return mode === 'split' ? [ 'mgmt', 'thread_bb' ] : [ 'single' ]; }

function defaultIface(id) {
  return {tagged : id !== 'single', vlan : '', dhcp : true, cidr : '', gw : '', dns : ''};
}

/** Draft of an API configuration ({mode, single|mgmt|thread_bb: {vlan, ipv4}}), or of the default when null. */
function draftFromConfig(cfg) {
  var d = {mode : cfg && cfg.mode === 'split' ? 'split' : 'single'};
  IDS.forEach(function(id) {
    var c = cfg && cfg[id];
    if (!c) {
      d[id] = defaultIface(id);
      return;
    }
    var v4 = c.ipv4 || {dhcp : true};
    var prefix = v4.dhcp ? null : maskToPrefix(v4.mask);
    d[id] = {
      tagged : c.vlan !== 0,
      vlan : c.vlan ? String(c.vlan) : '',
      dhcp : !!v4.dhcp,
      cidr : v4.dhcp ? '' : v4.addr + (prefix === null ? '' : '/' + prefix),
      gw : v4.gw && v4.gw !== '0.0.0.0' ? v4.gw : '',
      dns : v4.dns && v4.dns !== '0.0.0.0' ? v4.dns : ''
    };
  });
  return d;
}

function vlanText(i) {
  if (!i.tagged)
    return 'Untagged';
  var n = C.wholeNumber(i.vlan, 1, VLAN_MAX); // "010" and "10" are the same VLAN
  return 'VLAN ' + (n === null ? (String(i.vlan).trim() || '?') : n);
}

/* The board stores an unset gateway or DNS server as 0.0.0.0. */
function unset(text) {
  var t = String(text).trim();
  return t === '0.0.0.0' ? '' : t;
}

function ipText(i) {
  if (i.dhcp)
    return 'Automatic (DHCP)';
  var gw = String(i.gw).trim(), dns = String(i.dns).trim();
  return 'Static ' + (String(i.cidr).trim() || '?') + (gw ? ', gateway ' + gw : '') + (dns ? ', DNS ' + dns : '');
}

/* The board's checks of a static interface (validate_iface): address, prefix, gateway and DNS server. */
function staticProblems(i, e) {
  var p = parseCidr(i.cidr), a = 0, m = 0, host = 0;
  if (!p) {
    e.push('Enter the address with its prefix length, for example 192.168.1.10/24.');
  } else {
    a = ipv4ToNumber(p.addr);
    m = ipv4ToNumber(p.mask);
    host = (~m) >>> 0;
    if (!usableUnicast(a))
      e.push('Use a normal unicast address (not 0.0.0.0, 127.x or multicast).');
    else if (host <= 1)
      e.push('Use a prefix length of 30 or shorter.');
    else if ((a & host) >>> 0 === 0 || (a & host) >>> 0 === host)
      e.push('This is the network or broadcast address of the subnet: pick another host address.');
  }
  var gw = unset(i.gw), dns = unset(i.dns);
  if (gw && !ipv4Valid(gw)) {
    e.push('The gateway is not an IPv4 address.');
  } else if (gw) {
    var g = ipv4ToNumber(gw);
    if (!usableUnicast(g) || (p && (g === a || ((g & m) >>> 0) !== ((a & m) >>> 0) || (g & host) >>> 0 === 0 ||
                                    (g & host) >>> 0 === host)))
      e.push('The gateway must be another host address in the same subnet.');
  }
  if (dns && !ipv4Valid(dns))
    e.push('The DNS server is not an IPv4 address.');
  else if (dns && !usableUnicast(ipv4ToNumber(dns)))
    e.push('The DNS server is not a usable address.');
}

/*
 * Problems of the interfaces of the draft's mode: {ifaces: {id: [text]}, global: [text], count}. The rules are the
 * board's own (netcfg_validate, p4/components/netcfg/config.c), so Review is only offered for what the board accepts.
 */
function validate(d) {
  var out = {ifaces : {single : [], mgmt : [], thread_bb : []}, global : [], count : 0};
  ifaceIds(d.mode).forEach(function(id) {
    var i = d[id], e = out.ifaces[id];
    if (i.tagged && C.wholeNumber(i.vlan, 1, VLAN_MAX) === null)
      e.push('Enter a VLAN ID from 1 to 4094.');
    if (!i.dhcp)
      staticProblems(i, e);
    out.count += e.length;
  });
  if (d.mode === 'split') {
    var a1 = d.mgmt, b1 = d.thread_bb;
    var va = C.wholeNumber(a1.vlan, 1, VLAN_MAX), vb = C.wholeNumber(b1.vlan, 1, VLAN_MAX);
    if (!a1.tagged && !b1.tagged)
      out.global.push('Only one of the two interfaces can be untagged.');
    else if (a1.tagged && b1.tagged && va !== null && va === vb)
      out.global.push('Management and Thread backbone need different VLAN IDs.');
    var pa = !a1.dhcp && parseCidr(a1.cidr), pb = !b1.dhcp && parseCidr(b1.cidr);
    if (pa && pb) {
      var mask = (ipv4ToNumber(pa.mask) & ipv4ToNumber(pb.mask)) >>> 0; // the shorter prefix
      if (((ipv4ToNumber(pa.addr) & mask) >>> 0) === ((ipv4ToNumber(pb.addr) & mask) >>> 0))
        out.global.push('Management and Thread backbone cannot be in the same subnet.');
    }
  }
  out.count += out.global.length;
  return out;
}

/** Rows {what, from, to} of what Apply changes, from the running draft `cur` to `d`. */
function changes(cur, d) {
  var rows = [];
  if (cur.mode !== d.mode)
    rows.push({what : 'Setup', from : MODE_NAMES[cur.mode], to : MODE_NAMES[d.mode]});
  var running = ifaceIds(cur.mode);
  ifaceIds(d.mode).forEach(function(id) {
    var inUse = running.indexOf(id) >= 0;
    [ [ ' VLAN', vlanText ], [ ' IPv4', ipText ] ].forEach(function(f) {
      var from = inUse ? f[1](cur[id]) : 'Not in use', to = f[1](d[id]);
      if (from !== to)
        rows.push({what : NAMES[id] + f[0], from : from, to : to});
    });
  });
  return rows;
}

/** PUT /device/network body of a draft (validate it first; throws like networkConfig). */
function requestBody(d) {
  var form = {mode : d.mode};
  ifaceIds(d.mode).forEach(function(id) {
    var i = d[id], p = i.dhcp ? null : parseCidr(i.cidr);
    form[id] = {
      vlan : i.tagged ? i.vlan : '0',
      dhcp : i.dhcp,
      addr : p ? p.addr : '',
      mask : p ? p.mask : '',
      gw : i.gw,
      dns : i.dns
    };
  });
  return networkConfig(form);
}

/** Where the page answers after Apply: the management interface of the draft. */
function reachText(d) {
  var split = d.mode === 'split', i = d[split ? 'mgmt' : 'single'];
  if (!i.dhcp) {
    var p = parseCidr(i.cidr);
    return 'http://' + (p ? p.addr : '?');
  }
  if (!split)
    return 'the address your DHCP server gives the board';
  return 'the address the DHCP server ' + (i.tagged ? 'on VLAN ' + String(i.vlan).trim() : 'of the untagged network') +
         ' gives the board';
}

/** VLAN IDs of the tagged interfaces of the draft's mode: the switch port must carry them. */
function taggedVlans(d) {
  return ifaceIds(d.mode).filter(function(id) { return d[id].tagged; }).map(function(id) {
    return String(d[id].vlan).trim();
  });
}

return {
  IDS : IDS,
  API_NAMES : API_NAMES,
  ipv4Valid : ipv4Valid,
  maskValid : maskValid,
  prefixToMask : prefixToMask,
  maskToPrefix : maskToPrefix,
  parseCidr : parseCidr,
  networkConfig : networkConfig,
  ifaceIds : ifaceIds,
  draftFromConfig : draftFromConfig,
  vlanText : vlanText,
  ipText : ipText,
  validate : validate,
  changes : changes,
  requestBody : requestBody,
  reachText : reachText,
  taggedVlans : taggedVlans
};
})();

function EthernetPage(env) {
var C = typeof DeviceCommon !== 'undefined' ? DeviceCommon : require('./device_common.js');
var L = EthernetLogic;
var net = null;     // last answer of GET /device/network
var cur = null;     // draft of the running configuration
var draft = null;   // what the person edits
var editing = {};
var review = false;
var ack = false;
var kit = C.DeviceKit(env, {
  actionIds : [ 'discardBtn', 'backBtn' ],
  trial : false,
  refreshNetwork : function() { return refreshNetwork(); },
  onBusy : function() { render(); }
});
var $ = kit.$, el = kit.el, setText = kit.setText, show = kit.show, note = kit.note;

function clone(d) { return JSON.parse(JSON.stringify(d)); }

function running(id) { return !!net && L.ifaceIds(net.mode).indexOf(id) >= 0; }

function live(id) {
  var name = L.API_NAMES[id];
  return (net && net.interfaces || []).filter(function(i) { return i.name === name; })[0] || null;
}

function pressed(id, on) { $(id).setAttribute('aria-pressed', on ? 'true' : 'false'); }

function setValue(id, v) {
  // Only on change: rewriting the same value would move the caret while someone types.
  if ($(id).value !== v)
    $(id).value = v;
}

/* Error lines of a box; rebuilt only when they change, so screen readers announce them once. */
function messages(id, list) {
  var box = $(id), key = list.join('\n');
  if (box.getAttribute('data-shown') === key)
    return;
  box.setAttribute('data-shown', key);
  kit.clear(box);
  list.forEach(function(t) { box.appendChild(el('p', t, 'error-text')); });
}

function renderIface(id, errors) {
  var i = draft[id], p = 'if-' + id + '-', inUse = running(id), lv = inUse ? live(id) : null, ed = !!editing[id];
  setText(p + 'vlanchip', L.vlanText(i));
  setText(p + 'linkchip', inUse ? (lv && lv.link ? 'Link up' : 'Link down') : 'Not in use yet');
  $(p + 'linkchip').className = 'chip ' + (inUse ? (lv && lv.link ? 'chip-ok' : 'chip-bad') : '');
  setText(p + 'edit', ed ? 'Done' : 'Edit');
  $(p + 'edit').setAttribute('aria-expanded', ed ? 'true' : 'false');

  show(p + 'live', inUse && !ed);
  if (inUse) {
    var c = cur[id];
    setText(p + 'ipv4', lv && lv.ipv4 ? lv.ipv4 : 'none yet');
    setText(p + 'src', c.dhcp ? 'from DHCP' : 'static');
    show(p + 'confrow', !c.dhcp);
    var conf = [];
    if (c.gw)
      conf.push('gateway ' + c.gw);
    if (c.dns)
      conf.push('DNS ' + c.dns);
    setText(p + 'conf', conf.length ? conf.join(', ') : 'no gateway or DNS server');
    setText(p + 'ipv6', lv && lv.ipv6 && lv.ipv6.length ? lv.ipv6.join('\n') : 'none');
  }
  show(p + 'summary', !inUse && !ed);
  setText(p + 'summary', 'Not in use yet. After you apply: ' + L.vlanText(i) + ', IPv4 ' + L.ipText(i) + '.');

  show(p + 'editor', ed);
  pressed(p + 'untagged', !i.tagged);
  pressed(p + 'tagged', i.tagged);
  show(p + 'vlanfield', i.tagged);
  pressed(p + 'dhcp', i.dhcp);
  pressed(p + 'staticbtn', !i.dhcp);
  show(p + 'staticfields', !i.dhcp);
  setValue(p + 'vlan', i.vlan);
  setValue(p + 'cidr', i.cidr);
  setValue(p + 'gw', i.gw);
  setValue(p + 'dns', i.dns);
  messages(p + 'errors', errors);
}

function render() {
  if (!draft)
    return;
  [ 'single', 'split' ].forEach(function(m) {
    pressed('mode-' + m, draft.mode === m);
    show('mode-' + m + '-now', net.mode === m);
  });
  var v = L.validate(draft), rows = L.changes(cur, draft), shown = L.ifaceIds(draft.mode);
  L.IDS.forEach(function(id) {
    show('if-' + id, shown.indexOf(id) >= 0);
    if (shown.indexOf(id) >= 0)
      renderIface(id, v.ifaces[id]);
  });
  messages('global-errors', v.global);

  var n = rows.length, canReview = n > 0 && v.count === 0;
  if (review && !canReview)
    review = false;
  show('actionBar', (n > 0 || v.count > 0) && !review);
  setText('barText', v.count ? 'Fix ' + v.count + (v.count === 1 ? ' problem' : ' problems') +
                                   ' before you review the changes.'
                             : n + (n === 1 ? ' change' : ' changes') + ' not applied yet.');
  $('reviewBtn').disabled = !canReview || kit.isBusy();

  show('reviewSection', review);
  var body = $('diffBody');
  kit.clear(body);
  rows.forEach(function(r) {
    var tr = el('tr');
    var th = el('th', r.what);
    th.setAttribute('scope', 'row');
    tr.appendChild(th);
    tr.appendChild(el('td', r.from, 'diff-from'));
    tr.appendChild(el('td', r.to, 'diff-to'));
    body.appendChild(tr);
  });
  setText('reachText', L.reachText(draft));
  var tagged = L.taggedVlans(draft);
  show('ackRow', tagged.length > 0);
  setText('vlanList', tagged.map(function(t) { return 'VLAN ' + t; }).join(' and '));
  $('ackBox').checked = ack;
  $('applyBtn').disabled = kit.isBusy() || !canApply();
}

function canApply() {
  return review && L.validate(draft).count === 0 && L.changes(cur, draft).length > 0 &&
         (L.taggedVlans(draft).length === 0 || ack);
}

function refreshNetwork() {
  return kit.request('GET', '/device/network', {timeoutMs : 5000}).then(function(r) {
    if (r.status !== 200 || !r.body) {
      setText('n-stale', 'The network state could not be read: ' + kit.failure(r));
      show('n-stale', true);
      return false;
    }
    net = r.body;
    var running = L.draftFromConfig(net.trial || net.active);
    if (cur && review && JSON.stringify(running) !== JSON.stringify(cur)) {
      // Someone changed the running settings: the reviewed diff no longer holds.
      review = false;
      ack = false;
    }
    cur = running;
    if (!draft)
      draft = clone(cur);
    kit.renderTrial(net);
    show('n-stale', false);
    setText('n-dropped', String(net.vlan_rx_dropped));
    setText('n-fallback', net.fallback ? 'The stored settings could not be applied (' + (net.error || 'unknown reason') +
                                             '); the defaults are running.'
                                       : '');
    show('n-fallback', !!net.fallback);
    render();
    return true;
  }, function() {
    setText('n-stale', 'The network state could not be read: no answer from the device.');
    show('n-stale', true);
    return false;
  });
}

/* Every change of the draft goes through here: it ends the review and asks for the VLAN tick again. */
function edit(fn) {
  return function(ev) {
    // While the board reboots into what was applied, the review and its instructions stay as they are.
    if (!draft || kit.isBusy())
      return;
    fn(ev);
    review = false;
    ack = false;
    render();
  };
}

function pickMode(m) {
  draft.mode = m;
  L.ifaceIds(m).forEach(function(id) {
    if (!running(id))
      editing[id] = true;
  });
}

function discard() {
  if (!cur)
    return;
  draft = clone(cur);
  editing = {};
  review = false;
  ack = false;
  note('netMsg', '', false);
  render();
}

function openReview() {
  var v = L.validate(draft);
  if (v.count || !L.changes(cur, draft).length)
    return;
  review = true;
  editing = {};
  render();
}

function apply() {
  if (!draft || kit.isBusy() || !canApply())
    return Promise.resolve(null);
  var body, reach = L.reachText(draft);
  try {
    body = L.requestBody(draft);
  } catch (e) {
    note('netMsg', e.message, true);
    return Promise.resolve(null);
  }
  return kit.runOp(function() {
    return kit.request('PUT', '/device/network', {headers : {'Content-Type' : 'application/json'}, body : JSON.stringify(body)})
        .then(function(r) {
          if (r.status !== 200) {
            note('netMsg', kit.failure(r), true);
            return r;
          }
          note('netMsg', 'Saved as a trial. The board is rebooting: open ' + reach + ' and press Keep new settings ' +
                             'within 5 minutes.', false);
          kit.waitForBoard('netMsg', 'The board did not come back at this address. Open ' + reach + ' and press ' +
                                         'Keep new settings within 5 minutes of its reboot, or it goes back to the ' +
                                         'previous settings by itself.');
          return {handoff : true};
        }, function(e) {
          // The board may have saved the trial before the connection dropped.
          note('netMsg', e.message + ' If the board saved the new settings, it is rebooting: open ' + reach +
                             ' and press Keep new settings within 5 minutes.', true);
        });
  });
}

function wireIface(id) {
  var p = 'if-' + id + '-';
  $(p + 'edit').addEventListener('click', edit(function() { editing[id] = !editing[id]; }));
  $(p + 'untagged').addEventListener('click', edit(function() { draft[id].tagged = false; }));
  $(p + 'tagged').addEventListener('click', edit(function() { draft[id].tagged = true; }));
  $(p + 'dhcp').addEventListener('click', edit(function() { draft[id].dhcp = true; }));
  $(p + 'staticbtn').addEventListener('click', edit(function() { draft[id].dhcp = false; }));
  [ 'vlan', 'cidr', 'gw', 'dns' ].forEach(function(f) {
    $(p + f).addEventListener('input', edit(function(ev) { draft[id][f] = ev.target.value; }));
  });
}

function init() {
  $('mode-single').addEventListener('click', edit(function() { pickMode('single'); }));
  $('mode-split').addEventListener('click', edit(function() { pickMode('split'); }));
  L.IDS.forEach(wireIface);
  $('discardBtn').addEventListener('click', discard);
  $('reviewBtn').addEventListener('click', openReview);
  $('backBtn').addEventListener('click', function() {
    review = false;
    render();
  });
  $('ackBox').addEventListener('change', function() {
    ack = $('ackBox').checked;
    render();
  });
  $('applyBtn').addEventListener('click', apply);
  kit.start();
  kit.poller(refreshNetwork, 10000, 'ethSection');
}

/* The names section stays still during a trial and while the board reboots into what was applied. */
function isBlocked() { return kit.isBusy() || !!(net && net.trial); }

return {init : init, refreshNetwork : refreshNetwork, apply : apply, kit : kit, isBlocked : isBlocked};
}

if (typeof module !== 'undefined' && module.exports) {
  module.exports = EthernetLogic;
  module.exports.Page = EthernetPage;
}

// A global: names.js, loaded after this script, asks it whether the page is busy.
var ethernetPage = typeof document !== 'undefined' ? EthernetPage(DeviceCommon.DeviceEnv()) : null;
if (ethernetPage)
  ethernetPage.init();
