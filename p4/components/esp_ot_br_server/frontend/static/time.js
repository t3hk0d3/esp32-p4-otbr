/*
 * ESP Thread Border Router - Time section of the Ethernet page (GET/PUT /device/time)
 *
 * TimeLogic is pure and runs under node too (tools/tests/test_time_page.py). TimePage is the section: the board time,
 * the sync state and servers, and an always-open form {enabled, mode, server} with its own Save, applied live by the
 * board. It also fills the "Time server" row of the interface card whose DHCP lease named the servers.
 * Spec: docs/superpowers/specs/2026-11-10-time-sync-design.md, section 4.3.
 */
'use strict';

var TimeLogic = (function() {
var SERVER_MAX = 63;
var IFACE_NAMES = {mgmt : 'Management', thread_bb : 'Thread backbone'};

/* {text, kind} of the section's chip; kind '' (neutral), 'ok' or 'warn'. */
function chip(st) {
  if (!st.enabled)
    return {text : 'Off', kind : ''};
  if (st.now === null)
    return {text : 'Clock not set', kind : 'warn'};
  if (st.state === 'synced')
    return {text : 'Synced', kind : 'ok'};
  if (st.state === 'unreachable')
    return {text : 'Server not answering', kind : 'warn'};
  return {text : 'Waiting for server', kind : ''};
}

function plural(n, unit) { return n + ' ' + unit + (n === 1 ? '' : 's') + ' ago'; }

function lastSynced(st) {
  var s = st.synced_age_s;
  if (s === null || s === undefined)
    return st.now === null ? 'Never since power-up' : 'Before the last restart';
  if (s < 60)
    return 'Just now';
  if (s < 3600)
    return plural(Math.floor(s / 60), 'minute');
  if (s < 86400)
    return plural(Math.floor(s / 3600), 'hour');
  return plural(Math.floor(s / 86400), 'day');
}

function sourceLine(st) {
  if (st.source === 'dhcp')
    return IFACE_NAMES[st.dhcp_iface] ? 'from your router (DHCP on ' + IFACE_NAMES[st.dhcp_iface] + ')'
                                      : 'from your router (DHCP)';
  if (st.source === 'default')
    return 'default; your router names none';
  if (st.source === 'custom')
    return 'set here';
  return '';
}

/* Under Custom: what Automatic would use instead; '' when the router offers nothing. */
function dhcpHint(st) {
  var list = st.dhcp_servers || [];
  if (!list.length)
    return '';
  return 'Your router offers ' + list.join(' and ') + '. Automatic would use ' + (list.length > 1 ? 'them' : 'it') + '.';
}

/* {iface, text} for the interface card whose lease named the servers; null when none or not known. */
function ifaceRow(st) {
  var list = st.dhcp_servers || [];
  if (!st.dhcp_iface || !list.length)
    return null;
  return {iface : st.dhcp_iface, text : list.join(', ')};
}

function fromApi(body) { return {enabled : !!body.enabled, mode : body.mode, server : body.server || ''}; }

/* The board's rule (timesync_rules_check): an IPv4 or IPv6 literal or a hostname. null when valid. */
function serverError(server) {
  if (server === '')
    return 'Enter a server, or choose Automatic.';
  if (server.length > SERVER_MAX)
    return 'Longer than ' + SERVER_MAX + ' characters.';
  if (/^[0-9a-fA-F:]+$/.test(server) && (server.match(/:/g) || []).length >= 2)
    return null; // an IPv6 address; the board checks it further
  var labels = server.split('.');
  for (var i = 0; i < labels.length; i++) {
    var l = labels[i];
    if (!/^[A-Za-z0-9-]+$/.test(l))
      return 'A hostname or an IP address: letters, digits, hyphens and dots.';
    if (l[0] === '-' || l[l.length - 1] === '-')
      return 'No part of the name may start or end with a hyphen.';
  }
  return null;
}

/* {field: message}; the server is checked only under Custom (Automatic hides the field). */
function validate(form) {
  var errors = {};
  if (form.mode === 'custom') {
    var e = serverError(form.server.trim());
    if (e)
      errors.server = e;
  }
  return errors;
}

function dirty(a, b) { return a.enabled !== b.enabled || a.mode !== b.mode || a.server.trim() !== b.server.trim(); }

/* The PUT body. Under Automatic a server that is not valid is not sent: the board keeps the stored one for Custom. */
function toBody(form) {
  var server = form.server.trim();
  if (form.mode === 'auto' && server !== '' && serverError(server))
    server = '';
  return {enabled : !!form.enabled, mode : form.mode, server : server};
}

return {
  chip : chip,
  lastSynced : lastSynced,
  sourceLine : sourceLine,
  dhcpHint : dhcpHint,
  ifaceRow : ifaceRow,
  fromApi : fromApi,
  validate : validate,
  dirty : dirty,
  toBody : toBody
};
})();

/* opts.isBlocked(): true while the Ethernet page runs a trial or waits for its reboot. Ids tm-* and if-<iface>-ntp*. */
function TimePage(env, opts) {
var C = typeof DeviceCommon !== 'undefined' ? DeviceCommon : require('./device_common.js');
var L = TimeLogic;
var IFACES = [ 'single', 'mgmt', 'thread_bb' ];
var api = null;    // last GET /device/time
var readAt = 0;    // env.now() when `api` arrived: the board time ticks from there
var saved = null;  // form of `api`
var form = null;   // what the person edits
var serverError = '';
var saving = false;
var kit = C.DeviceKit(env, {trial : false});
var $ = kit.$, el = kit.el, setText = kit.setText, show = kit.show, note = kit.note;
opts = opts || {};

function blocked() { return saving || !!(opts.isBlocked && opts.isBlocked()); }

function boardTime() {
  if (!api || api.now === null)
    return 'Not set';
  var ms = api.now * 1000 + (env.now() - readAt);
  return new Date(ms).toLocaleString();
}

function renderServers() {
  var box = $('tm-servers');
  kit.clear(box);
  if (!api.servers.length) {
    box.appendChild(el('span', 'None'));
    return;
  }
  api.servers.forEach(function(s, i) {
    if (i)
      box.appendChild(el('br'));
    box.appendChild(el('span', s.address, 'mono'));
    if (s.answered) {
      box.appendChild(env.document.createTextNode(' '));
      box.appendChild(el('span', 'answered', 'chip chip-ok'));
    }
  });
}

function renderIfaceRows() {
  var row = L.ifaceRow(api);
  IFACES.forEach(function(name) {
    var on = !!row && row.iface === name;
    show('if-' + name + '-ntprow', on);
    setText('if-' + name + '-ntp', on ? row.text : '');
  });
}

function render() {
  if (!api)
    return;
  var c = L.chip(api);
  setText('tm-chip', c.text);
  $('tm-chip').classList.toggle('chip-ok', c.kind === 'ok');
  $('tm-chip').classList.toggle('chip-warn', c.kind === 'warn');
  setText('tm-now', boardTime());
  setText('tm-last', L.lastSynced(api));
  renderServers();
  setText('tm-source', L.sourceLine(api));
  renderIfaceRows();
  if (!form)
    return;
  var off = blocked(), v = L.validate(form), custom = form.mode === 'custom';
  $('tm-enabled').checked = form.enabled;
  $('tm-auto').setAttribute('aria-pressed', String(!custom));
  $('tm-custom').setAttribute('aria-pressed', String(custom));
  if ($('tm-server').value !== form.server)
    $('tm-server').value = form.server;
  show('tm-mode', form.enabled);
  show('tm-server-field', form.enabled && custom);
  show('tm-hint-auto', form.enabled && !custom);
  show('tm-hint-off', !form.enabled);
  setText('tm-dhcp-hint', custom ? L.dhcpHint(api) : '');
  setText('tm-server-err', v.server || serverError || '');
  [ 'tm-enabled', 'tm-auto', 'tm-custom', 'tm-server' ].forEach(function(id) { $(id).disabled = off; });
  $('tm-save').disabled = off || !L.dirty(saved, form) || Object.keys(v).length > 0;
  $('tm-discard').disabled = off || !L.dirty(saved, form);
}

function load(body) {
  var editing = form && saved && L.dirty(saved, form);
  api = body;
  readAt = env.now();
  saved = L.fromApi(body);
  // A poll never overwrites what someone is editing.
  if (!editing)
    form = L.fromApi(body);
}

function refresh() {
  return kit.request('GET', '/device/time', {timeoutMs : 5000}).then(function(r) {
    if (r.status !== 200 || !r.body) {
      note('tm-msg', 'The time settings could not be read: ' + kit.failure(r), true);
      return false;
    }
    load(r.body);
    render();
    return true;
  }, function() {
    note('tm-msg', 'The time settings could not be read: no answer from the device.', true);
    return false;
  });
}

function edit(fn) {
  return function(ev) {
    if (!form || blocked())
      return;
    fn(ev);
    serverError = '';
    note('tm-msg', '', false);
    render();
  };
}

function save() {
  if (!form || blocked() || !L.dirty(saved, form) || Object.keys(L.validate(form)).length)
    return Promise.resolve(null);
  saving = true;
  render();
  return kit.request('PUT', '/device/time', {headers : {'Content-Type' : 'application/json'},
                                              body : JSON.stringify(L.toBody(form)), timeoutMs : 10000})
      .then(function(r) {
        if (r.status === 400 && r.body && r.body.field === 'server') {
          serverError = r.body.detail || 'The board refused the server.';
          note('tm-msg', 'The board refused the server: see the field above.', true);
          return;
        }
        if (r.status !== 200 || !r.body) {
          note('tm-msg', kit.failure(r), true);
          return;
        }
        var keep = form.server; // the typed server stays for a later switch back to Custom
        form = null;
        load(r.body);
        if (form.mode === 'auto' && form.server === '')
          form.server = saved.server = keep;
        note('tm-msg', 'Saved.', false);
        // The reply predates the change: the board applies it on its own task, so read the result again.
        env.setTimeout(refresh, 500);
      }, function(e) { note('tm-msg', e.message, true); })
      .then(function() {
        saving = false;
        render();
      });
}

function init() {
  $('tm-enabled').addEventListener('change', edit(function(ev) { form.enabled = ev.target.checked; }));
  $('tm-auto').addEventListener('click', edit(function() { form.mode = 'auto'; }));
  $('tm-custom').addEventListener('click', edit(function() { form.mode = 'custom'; }));
  $('tm-server').addEventListener('input', edit(function(ev) { form.server = ev.target.value; }));
  $('tm-discard').addEventListener('click', function() {
    if (!saved || blocked())
      return;
    form = JSON.parse(JSON.stringify(saved));
    serverError = '';
    note('tm-msg', '', false);
    render();
  });
  $('tm-save').addEventListener('click', function() { save(); });
  env.setInterval(function() {
    if (api)
      setText('tm-now', boardTime());
  }, 1000);
  kit.poller(function() { return saving ? Promise.resolve(true) : refresh(); }, 10000, 'timeSection');
}

return {init : init, refresh : refresh, save : save, render : render, kit : kit};
}

if (typeof module !== 'undefined' && module.exports) {
  module.exports = TimeLogic;
  module.exports.Page = TimePage;
}

if (typeof document !== 'undefined')
  TimePage(DeviceCommon.DeviceEnv(), {
    isBlocked : function() { return typeof ethernetPage !== 'undefined' && ethernetPage.isBlocked(); }
  }).init();
