/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP Thread Border Router - Routing page (GET/PUT /device/routing, PD state from GET /device/status)
 *
 * RoutingLogic is pure and runs under node too (tools/tests/test_routing_page.py). RoutingPage is the page.
 * The draft holds the form strings as typed; the board validates, a 400 names the field.
 */
'use strict';

var RoutingLogic = (function() {
var C = typeof DeviceCommon !== 'undefined' ? DeviceCommon : require('./device_common.js');
var METHODS = [ 'pd', 'ripng', 'bgp', 'off' ];
var OMR_MODES = [ 'auto', 'custom' ];
var MAX_PEERS = 2;
var PD_OFF = ' DHCPv6 PD is off.';

/** Draft of a GET answer. */
function draftFromState(state) {
  var bgp = state.bgp || {};
  return {
    method : state.method,
    omr_mode : state.omr && state.omr.mode === 'custom' ? 'custom' : 'auto',
    omr_prefix : state.omr && state.omr.prefix ? state.omr.prefix : '',
    announce_ula : !!state.announce_ula,
    local_asn : bgp.local_asn === undefined || bgp.local_asn === null ? '' : String(bgp.local_asn),
    router_id : bgp.router_id || '',
    hold_time : bgp.hold_time === undefined || bgp.hold_time === null ? '' : String(bgp.hold_time),
    peers : (bgp.peers || []).map(function(p) { return {address : p.address, asn : String(p.asn)}; })
  };
}

/* Whole numbers as numbers, the rest as typed (the board's 400 names it). */
function number(text) {
  var t = String(text).trim();
  return /^\d+$/.test(t) ? Number(t) : t;
}

/** PUT body of a draft: always the whole bgp object, never the status. */
function bodyFromDraft(d) {
  var custom = d.omr_mode === 'custom';
  return {
    method : d.method,
    omr : {mode : d.omr_mode, prefix : custom ? String(d.omr_prefix).trim() : null},
    announce_ula : !!d.announce_ula,
    bgp : {
      local_asn : number(d.local_asn),
      router_id : String(d.router_id).trim() || null,
      hold_time : number(d.hold_time),
      peers : d.peers.map(function(p) { return {address : String(p.address).trim(), asn : number(p.asn)}; })
    }
  };
}

/** Which parts of the page the draft shows: {bgp, ripng, customPrefix}. */
function visibleSections(d) {
  return {bgp : d.method === 'bgp', ripng : d.method === 'ripng', customPrefix : d.omr_mode === 'custom'};
}

function same(a, b) { return String(a).trim() === String(b).trim(); }

/** Number of settings the draft changes against the saved draft (each peer row counts once). */
function dirtyCount(saved, d) {
  var n = 0;
  [ 'method', 'omr_mode', 'local_asn', 'router_id', 'hold_time' ].forEach(function(k) {
    if (!same(saved[k], d[k]))
      n++;
  });
  if (d.omr_mode === 'custom' && !same(saved.omr_prefix, d.omr_prefix))
    n++;
  if (saved.announce_ula !== d.announce_ula)
    n++;
  for (var i = 0; i < Math.max(saved.peers.length, d.peers.length); i++) {
    var a = saved.peers[i], b = d.peers[i];
    if (!a || !b || !same(a.address, b.address) || !same(a.asn, b.asn))
      n++;
  }
  return n;
}

var PD_TEXT = {
  disabled : 'DHCPv6 PD: disabled.',
  stopped : 'DHCPv6 PD: stopped while the border router is not running.',
  idle : 'DHCPv6 PD: idle, another border router asks for the prefix.',
  uninitialized : 'DHCPv6 PD: the border router is not up yet.'
};

/** Line on the method in use; `pd` is the pd object of /device/status. */
function methodHint(state, pd) {
  var st = state.status || {};
  if (state.method === 'pd') {
    if (!pd || !pd.state)
      return 'DHCPv6 PD: state unknown.';
    if (pd.state === 'running')
      return pd.prefix ? 'DHCPv6 PD: delegated ' + pd.prefix + '.' : 'DHCPv6 PD: soliciting, nothing delegated yet.';
    return PD_TEXT[pd.state] || 'DHCPv6 PD: ' + pd.state + '.';
  }
  if (state.method === 'ripng') {
    var r = st.ripng;
    if (!r || r.last_update_s_ago === null || r.last_update_s_ago === undefined)
      return 'RIPng: no update sent yet.' + PD_OFF;
    return 'RIPng: last update ' + r.last_update_s_ago + ' s ago.' + PD_OFF;
  }
  if (state.method === 'bgp') {
    var b = st.bgp;
    if (!b)
      return 'BGP: starting.' + PD_OFF;
    if (b.blocked)
      return 'BGP: sessions do not run (' + b.blocked + ').' + PD_OFF;
    var up = b.peers.filter(function(p) { return p.state === 'established'; }).length;
    return 'BGP: ' + up + ' of ' + b.peers.length + ' sessions established.' + PD_OFF;
  }
  return 'Nothing is announced.' + PD_OFF;
}

/* A 400's detail (bad_field) in words. */
var FIELD_TEXT = {
  'method' : 'The board does not accept this method.',
  'omr.prefix' : 'Enter a /64 prefix, for example 2001:db8:1:2::/64: not link-local, not multicast and not a prefix ' +
                 'that is already on your network.',
  'bgp.local_asn' : 'Enter an ASN from 1 to 4294967295, not 23456.',
  'bgp.router_id' : 'Enter an IPv4 address other than 0.0.0.0, or leave it empty.',
  'bgp.hold_time' : 'Enter 0, or a number of seconds from 3 to 65535.',
  'bgp.peers' : 'Check the peers: BGP needs at least one, each with its own unicast address (not one of the ' +
                'board\'s) and an ASN from 1 to 4294967295, not 23456.'
};

/** Text of a 400 naming `field`, or null when the page has no input for it. */
function fieldError(field) { return FIELD_TEXT.hasOwnProperty(field) ? FIELD_TEXT[field] : null; }

/** "1284" -> "1 284". */
function countText(n) { return String(n).replace(/\B(?=(\d{3})+(?!\d))/g, ' '); }

function stateName(s) { return s.charAt(0).toUpperCase() + s.slice(1); }

/** {text, cls} of a BGP session state chip. */
function stateChip(s) {
  return {text : stateName(String(s)), cls : s === 'established' ? 'chip-ok' : s === 'idle' ? 'chip-bad' : 'chip-warn'};
}

return {
  METHODS : METHODS,
  OMR_MODES : OMR_MODES,
  MAX_PEERS : MAX_PEERS,
  draftFromState : draftFromState,
  bodyFromDraft : bodyFromDraft,
  visibleSections : visibleSections,
  dirtyCount : dirtyCount,
  same : same,
  methodHint : methodHint,
  fieldError : fieldError,
  countText : countText,
  stateChip : stateChip,
  durationText : C.durationText
};
})();

function RoutingPage(env) {
var C = typeof DeviceCommon !== 'undefined' ? DeviceCommon : require('./device_common.js');
var L = RoutingLogic;
var state = null;   // last answer of GET /device/routing
var pd = null;      // pd object of /device/status while the saved method is pd
var saved = null;   // draft of the stored settings
var draft = null;   // what the person edits
var errors = {};    // error element id -> text, from the last 400
var kit = C.DeviceKit(env, {actionIds : [ 'saveBtn', 'discardBtn' ], onBusy : function() { renderDraft(); }});
var $ = kit.$, el = kit.el, setText = kit.setText, show = kit.show, note = kit.note;

/* The error element next to each field a 400 can name. */
var ERROR_IDS = {
  'method' : 'err-method',
  'omr.prefix' : 'err-omr',
  'bgp.local_asn' : 'err-asn',
  'bgp.router_id' : 'err-routerid',
  'bgp.hold_time' : 'err-hold',
  'bgp.peers' : 'err-peers'
};

function clone(d) { return JSON.parse(JSON.stringify(d)); }

function pressed(id, on) { $(id).setAttribute('aria-pressed', on ? 'true' : 'false'); }

function setValue(e, v) {
  // Only on change: rewriting the same value would move the caret while someone types.
  if (e.value !== v)
    e.value = v;
}

function renderErrors() {
  Object.keys(ERROR_IDS).forEach(function(f) {
    var id = ERROR_IDS[f];
    setText(id, errors[id] || '');
    show(id, !!errors[id]);
  });
}

function peerInput(i, field, label, value) {
  var lab = el('label', label, 'field'), input = el('input');
  input.setAttribute('type', 'text');
  input.id = 'peer-' + i + '-' + field;
  lab.setAttribute('for', input.id);
  if (field === 'asn')
    input.setAttribute('inputmode', 'numeric');
  else
    input.className = 'mono';
  input.value = value;
  input.addEventListener('input', edit(function(ev) { draft.peers[i][field] = ev.target.value; }, 'err-peers'));
  lab.appendChild(input);
  return lab;
}

/* Peer rows are rebuilt only when their number changes, so typing never loses the focus. */
function renderPeers() {
  var box = $('peerRows');
  if (box.children.length !== draft.peers.length) {
    kit.clear(box);
    draft.peers.forEach(function(p, i) {
      var row = el('div', null, 'peer-row');
      row.appendChild(peerInput(i, 'address', 'Peer address', p.address));
      row.appendChild(peerInput(i, 'asn', 'Peer ASN', p.asn));
      var rm = el('button', 'Remove', 'btn btn-outline-danger btn-sm');
      rm.setAttribute('type', 'button');
      rm.id = 'peer-' + i + '-remove';
      rm.addEventListener('click', edit(function() { draft.peers.splice(i, 1); }, 'err-peers'));
      row.appendChild(rm);
      box.appendChild(row);
    });
  }
  draft.peers.forEach(function(p, i) {
    setValue($('peer-' + i + '-address'), p.address);
    setValue($('peer-' + i + '-asn'), p.asn);
  });
  $('addPeerBtn').disabled = draft.peers.length >= L.MAX_PEERS || kit.isBusy();
}

/* Locked while a Save runs: its reply rewrites the inputs. */
var INPUT_IDS = [ 'customPrefix', 'ulaBox', 'localAsn', 'routerId', 'holdTime' ];

function lockInputs(on) {
  var ids = INPUT_IDS.concat(L.METHODS.map(function(m) { return 'method-' + m; }),
                             L.OMR_MODES.map(function(m) { return 'omr-' + m; }));
  draft.peers.forEach(function(p, i) {
    ids.push('peer-' + i + '-address', 'peer-' + i + '-asn', 'peer-' + i + '-remove');
  });
  ids.forEach(function(id) { $(id).disabled = on; });
  if (on)
    $('addPeerBtn').disabled = true;
}

/* Everything the draft decides; inputs are only written when they differ, so a refresh keeps what is typed. */
function renderDraft() {
  if (!draft)
    return;
  var v = L.visibleSections(draft);
  L.METHODS.forEach(function(m) {
    pressed('method-' + m, draft.method === m);
    $('method-' + m + '-now').className = 'chip now' + (state.method === m ? ' chip-ok' : ' off');
  });
  L.OMR_MODES.forEach(function(m) {
    pressed('omr-' + m, draft.omr_mode === m);
    show('omr-' + m + '-now', saved.omr_mode === m);
  });
  show('customField', v.customPrefix);
  setValue($('customPrefix'), draft.omr_prefix);
  $('ulaBox').checked = draft.announce_ula;
  setText('ulaHint', v.customPrefix ? 'With a custom prefix, devices get an address from both.'
                                    : 'In automatic mode the ULA is already the OMR prefix; this only decides ' +
                                          'whether it is announced.');
  show('bgpArea', v.bgp);
  show('ripngArea', v.ripng);
  setValue($('localAsn'), draft.local_asn);
  setValue($('routerId'), draft.router_id);
  setValue($('holdTime'), draft.hold_time);
  renderPeers();
  renderErrors();

  var n = L.dirtyCount(saved, draft);
  show('actionBar', n > 0);
  setText('barText', n + (n === 1 ? ' unsaved change' : ' unsaved changes'));
  $('saveBtn').disabled = kit.isBusy();
  lockInputs(kit.isBusy());
}

function sessionRow(p) {
  var tr = el('tr'), who = el('td', null, 'mono');
  var cfg = saved.peers.filter(function(c) { return c.address === p.address; })[0];
  who.appendChild(env.document.createTextNode(p.address));
  if (cfg) {
    who.appendChild(el('br'));
    who.appendChild(el('span', 'AS' + cfg.asn + ' · ' + (String(cfg.asn) === String(state.bgp.local_asn) ? 'iBGP' : 'eBGP'),
                       'text-muted'));
  }
  tr.appendChild(who);
  var chip = L.stateChip(p.state), st = el('td');
  st.appendChild(el('span', chip.text, 'chip ' + chip.cls));
  tr.appendChild(st);
  tr.appendChild(el('td', p.state === 'established' ? L.durationText(p.uptime_s) : '-'));
  tr.appendChild(el('td', L.countText(p.received_dropped), 'num'));
  tr.appendChild(el('td', p.last_error || 'none', p.last_error ? '' : 'text-muted'));
  return tr;
}

/* The status parts: what the board reports, rebuilt on every answer of GET. */
function renderStatus() {
  var st = state.status || {};
  setText('methodHint', L.methodHint(state, pd));
  setText('omrInUse', st.omr_prefix || 'unknown');
  setText('ulaPrefix', st.ula_prefix || 'unknown');
  setText('ulaInline', st.ula_prefix || 'not known yet');
  var announces = state.method === 'ripng' || state.method === 'bgp';
  var ann = st.announced || [];
  setText('announced', ann.length ? ann.join('\n') : announces ? 'nothing yet' : 'nothing (only RIPng and BGP announce)');
  $('routerId').setAttribute('placeholder', st.bgp && st.bgp.router_id ? st.bgp.router_id + ' (from IPv4)' : 'from IPv4');

  var b = st.bgp, body = $('sessionsBody');
  kit.clear(body);
  (b ? b.peers : []).forEach(function(p) { body.appendChild(sessionRow(p)); });
  show('sessionsWrap', !!(b && b.peers.length));
  var bgpNote = !b ? 'Sessions start after you save with BGP picked.'
                   : b.blocked ? 'Sessions do not run: ' + b.blocked + '.'
                               : b.peers.length ? '' : 'No peers configured.';
  setText('bgpNote', bgpNote);
  show('bgpNote', !!bgpNote);

  var r = st.ripng;
  show('ripngFacts', !!r);
  show('ripngNote', !r);
  setText('ripngNote', 'RIPng starts after you save with RIPng picked.');
  if (r) {
    setText('ripLast', r.last_update_s_ago === null || r.last_update_s_ago === undefined
                           ? 'none sent yet'
                           : r.last_update_s_ago + ' s ago, to ff02::9 on the Thread backbone');
    setText('ripSent', L.countText(r.updates_sent));
    setText('ripRequests', L.countText(r.requests_answered));
    setText('ripDropped', L.countText(r.responses_dropped));
  }
}

function stale(text) {
  setText('r-stale', text);
  show('r-stale', !!text);
}

/* Takes a GET (or PUT) answer as the stored state; a clean draft follows it, unsaved edits stay. */
function adopt(next) {
  var fresh = L.draftFromState(next);
  if (!draft || L.dirtyCount(saved, draft) === 0) {
    // Keep a prefix typed behind the Automatic card.
    var typed = draft && String(draft.omr_prefix).trim() && !L.same(draft.omr_prefix, saved.omr_prefix)
                    ? draft.omr_prefix
                    : null;
    draft = clone(fresh);
    if (typed !== null && draft.omr_mode === 'auto')
      draft.omr_prefix = typed;
  }
  state = next;
  saved = fresh;
}

function refreshPd() {
  if (state.method !== 'pd') {
    pd = null;
    return Promise.resolve();
  }
  return kit.request('GET', '/device/status', {timeoutMs : 5000}).then(function(r) {
    pd = r.status === 200 && r.body ? r.body.pd : null;
  }, function() { pd = null; });
}

function refresh() {
  return kit.request('GET', '/device/routing', {timeoutMs : 5000}).then(function(r) {
    if (r.status !== 200 || !r.body) {
      stale('The routing state could not be read: ' + kit.failure(r));
      return false;
    }
    adopt(r.body);
    return refreshPd().then(function() {
      stale('');
      renderDraft();
      renderStatus();
      return true;
    });
  }, function() {
    stale('The routing state could not be read: no answer from the device.');
    return false;
  });
}

/* Every change of the draft goes through here; it clears the error shown for that field. */
function edit(fn, errorId) {
  return function(ev) {
    if (!draft || kit.isBusy())
      return;
    fn(ev);
    if (errorId)
      delete errors[errorId];
    renderDraft();
  };
}

function discard() {
  if (!saved)
    return;
  draft = clone(saved);
  errors = {};
  note('saveMsg', '', false);
  renderDraft();
}

function save() {
  if (!draft || kit.isBusy())
    return Promise.resolve(null);
  var body = L.bodyFromDraft(draft);
  errors = {};
  note('saveMsg', '', false);
  return kit.runOp(function() {
    return kit.request('PUT', '/device/routing', {headers : {'Content-Type' : 'application/json'}, body : JSON.stringify(body)})
        .then(function(r) {
          if (r.status === 200 && r.body) {
            draft = null;
            adopt(r.body);
            note('saveMsg', 'Saved. The board uses the new settings now.', false);
            return refreshPd().then(function() {
              renderStatus();
              return r;
            });
          }
          var field = r.status === 400 && r.body ? r.body.detail : null;
          if (ERROR_IDS[field] && L.fieldError(field)) {
            errors[ERROR_IDS[field]] = L.fieldError(field);
            // bgp is checked whatever the method, but shown only for BGP.
            if (field.indexOf('bgp.') === 0 && draft.method !== 'bgp')
              note('saveMsg', 'BGP settings: ' + L.fieldError(field) + ' Pick BGP to fix them.', true);
          } else
            note('saveMsg', kit.failure(r), true);
          return r;
        }, function(e) { note('saveMsg', e.message, true); });
  }).then(function(r) {
    renderDraft();
    return r;
  });
}

function init() {
  L.METHODS.forEach(function(m) {
    $('method-' + m).addEventListener('click', edit(function() { draft.method = m; }, 'err-method'));
  });
  L.OMR_MODES.forEach(function(m) {
    $('omr-' + m).addEventListener('click', edit(function() { draft.omr_mode = m; }, 'err-omr'));
  });
  $('customPrefix').addEventListener('input', edit(function(ev) { draft.omr_prefix = ev.target.value; }, 'err-omr'));
  $('ulaBox').addEventListener('change', edit(function(ev) { draft.announce_ula = ev.target.checked; }));
  $('localAsn').addEventListener('input', edit(function(ev) { draft.local_asn = ev.target.value; }, 'err-asn'));
  $('routerId').addEventListener('input', edit(function(ev) { draft.router_id = ev.target.value; }, 'err-routerid'));
  $('holdTime').addEventListener('input', edit(function(ev) { draft.hold_time = ev.target.value; }, 'err-hold'));
  $('addPeerBtn').addEventListener('click', edit(function() {
    if (draft.peers.length < L.MAX_PEERS)
      draft.peers.push({address : '', asn : ''});
  }, 'err-peers'));
  $('discardBtn').addEventListener('click', discard);
  $('saveBtn').addEventListener('click', save);
  kit.start();
  kit.poller(refresh, 5000, 'routingSection');
}

return {init : init, refresh : refresh, save : save, kit : kit};
}

if (typeof module !== 'undefined' && module.exports) {
  module.exports = RoutingLogic;
  module.exports.Page = RoutingPage;
}

if (typeof document !== 'undefined')
  RoutingPage(DeviceCommon.DeviceEnv()).init();
