/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP Thread Border Router - USB/IP page (GET/PUT /device/usbip)
 *
 * UsbipLogic is pure and runs under node too (tools/tests/test_usbip_page.py). UsbipPage is the page. Everything that
 * comes from the device is inserted with textContent, never as markup.
 */
'use strict';

var UsbipLogic = (function() {
var C = typeof DeviceCommon !== 'undefined' ? DeviceCommon : require('./device_common.js');
var BUSID = '1-1';
var INFLIGHT_SHARED_BYTES = 32768; // USBIP_INFLIGHT_BYTES (usbip_core.h): what IN reads share
var INFLIGHT_HARD_BYTES = 81920;   // USBIP_INFLIGHT_HARD (usbip_core.h): the cap on all buffers in flight
var SPEEDS = {low : 'low (1.5 Mbit/s)', full : 'full (12 Mbit/s)', high : 'high (480 Mbit/s)'};

/* The chip of the page head, by state (spec section 5); `live`: the boxes about the port show. */
var STATES = {
  no_device : {text : 'No device', cls : 'chip-warn', live : true},
  unsupported : {text : 'Cannot export', cls : 'chip-bad', live : true},
  exportable : {text : 'Ready to attach', cls : 'chip-ok', live : true},
  attached : {text : 'Attached', cls : 'chip-ok', live : true},
  blocked : {text : 'Not running', cls : 'chip-bad', live : false}
};

/* The address without a port: "board.lan:80" -> "board.lan", "[fd00::1]:80" -> "fd00::1". Bare IPv6 stays. */
function hostOnly(text) {
  var t = String(text || '');
  var v6 = /^\[([^\]]*)\](?::\d+)?$/.exec(t);
  if (v6)
    return v6[1];
  if (t.indexOf(':') !== t.lastIndexOf(':'))
    return t;
  return t.replace(/:\d+$/, '');
}

function linuxCommands(host, busid) {
  return [
    '# once per boot of that computer', 'sudo modprobe vhci-hcd', '# see what the board offers, then attach it',
    'usbip list -r ' + host, 'sudo usbip attach -r ' + host + ' -b ' + busid, '# done with it', 'sudo usbip port',
    'sudo usbip detach -p 0'
  ].join('\n');
}

function windowsCommands(host, busid) {
  return [
    'REM with usbip-win2 installed, in an admin prompt', 'usbip list -r ' + host,
    'usbip attach -r ' + host + ' -b ' + busid
  ].join('\n');
}

/* On, but its start failed at boot: state off with the failure as the reason and no reboot pending. */
function startFailed(s) { return !!s && !!s.enabled && s.state === 'off' && !s.pending_reboot && !!s.reason; }

/*
 * What the page shows for a /device/usbip body and the address the page was opened at:
 * {chip: {text, cls}, show: {device, client, stats, attach}, commands: {linux, windows}}.
 */
function view(status, host) {
  var s = status || {};
  var st = STATES[s.state];
  var chip;
  if (!status)
    chip = {text : '-', cls : ''};
  else if (st)
    chip = {text : st.text, cls : st.cls};
  else if (startFailed(s))
    chip = {text : 'Not running', cls : 'chip-bad'};
  else if (s.enabled)
    chip = {text : 'Reboot pending', cls : 'chip-warn'}; // stored on, the port is not owned yet
  else
    chip = {text : 'Off', cls : ''};
  var live = !!st && st.live;
  var addr = hostOnly(host);
  var busid = s.device && s.device.busid ? s.device.busid : BUSID;
  return {
    chip : chip,
    show : {device : live, client : live, stats : live, attach : live},
    commands : {linux : linuxCommands(addr, busid), windows : windowsCommands(addr, busid)}
  };
}

/* "2 h 14 min" since the epoch second `since`; "unknown" while the board's clock is unset (0). */
function sinceText(since, nowMs) {
  if (typeof since !== 'number' || since <= 0)
    return 'unknown';
  return C.durationText(nowMs / 1000 - since);
}

function sizeText(bytes) {
  var units = [ 'B', 'KB', 'MB', 'GB' ];
  var v = Number(bytes) || 0, i = 0;
  while (v >= 1024 && i < units.length - 1) {
    v /= 1024;
    i++;
  }
  return (v >= 100 || i === 0 ? Math.round(v) : Math.round(v * 10) / 10) + ' ' + units[i];
}

/* 11950 -> "11 950" (no-break space). */
function groupText(n) { return String(Math.round(Number(n) || 0)).replace(/\B(?=(\d{3})+$)/g, ' '); }

function plural(n, one, many) { return n + ' ' + (n === 1 ? one : many); }

/* The text next to the switch. */
function switchText(s) {
  if (s && !s.enabled && s.active)
    return 'Turns off at the next reboot. Until then the port stays exported.';
  if (!s || !s.enabled)
    return 'Off. The USB port belongs to the Radios page.';
  if (STATES[s.state] && STATES[s.state].live)
    return 'On. Whatever you plug into the USB-A port is offered on TCP ' + (s.port || 3240) +
           '. Radios on the USB port are not used meanwhile.';
  return 'On, but not running: see below.';
}

/* What Save and Reboot does, shown next to the button while a change waits. */
function rebootText(turningOn) {
  return turningOn ? 'Not saved yet. Save and Reboot restarts the board (about 30 s); afterwards the USB port is shared ' +
                         'over the network, and radios on it are not used until you turn USB/IP off; their settings are kept.'
                   : 'Not saved yet. Save and Reboot restarts the board (about 30 s); a computer that has the device ' +
                         'attached loses it, and radios on the USB port get their settings back.';
}

/* Rows of the device card: [[key, value], ...]. */
function deviceFacts(d) {
  var rows = [ [ 'IDs', d.vid + ':' + d.pid ] ];
  if (d.serial)
    rows.push([ 'Serial', d.serial ]);
  rows.push([ 'Speed', SPEEDS[d.speed] || String(d.speed) ]);
  rows.push([ 'Bus id', d.busid ]);
  rows.push([ 'Interfaces', plural(d.interfaces, 'interface', 'interfaces') + ' · ' +
                                plural(d.endpoints, 'endpoint', 'endpoints') ]);
  return rows;
}

function interfaceName(s) { return s && s.interface === 'single' ? 'Ethernet network' : 'management network'; }

/* The four figures of the counters card: [{value, sub}] in order. */
function statFigures(s) {
  var c = s.counters || {}, m = s.memory || {};
  return [
    {value : groupText(c.sessions), sub : c.refused + ' refused'}, {
      value : groupText(c.completed),
      sub : plural(c.errors, 'error', 'errors') + ' · ' + c.unlinks + ' cancelled'
    },
    {value : sizeText(c.bytes_in), sub : 'to device ' + sizeText(c.bytes_out)}, {
      value : sizeText(m.inflight_bytes),
      sub : 'peak ' + sizeText(m.inflight_peak) + ' of ' + sizeText(INFLIGHT_HARD_BYTES) + ' (reads ' +
                sizeText(INFLIGHT_SHARED_BYTES) + ') · queue peak ' + sizeText(m.waiting_peak)
    }
  ];
}

return {
  view : view,
  startFailed : startFailed,
  hostOnly : hostOnly,
  sinceText : sinceText,
  sizeText : sizeText,
  switchText : switchText,
  rebootText : rebootText,
  deviceFacts : deviceFacts,
  interfaceName : interfaceName,
  statFigures : statFigures,
  BUSID : BUSID
};
})();

function UsbipPage(env) {
var C = typeof DeviceCommon !== 'undefined' ? DeviceCommon : require('./device_common.js');
var L = UsbipLogic;
var kit = C.DeviceKit(env, {actionIds : [ 'toggle', 'kickBtn', 'saveBtn' ]});
var $ = kit.$, el = kit.el, clear = kit.clear, setText = kit.setText, show = kit.show, note = kit.note;
var POLL_MS = 3000;

var status = null; // last GET /device/usbip
var staged = null; // the switch's unsaved value; null when it shows the stored one

function hostName() { return env.location && env.location.host ? env.location.host : ''; }

/* Hides `id` when `visible` is false; also marks the control hidden for the layout. */
function showIf(id, visible) { show(id, !!visible); }

function renderDevice(s) {
  var d = s.device;
  showIf('devInfo', !!d);
  showIf('devEmpty', !d);
  setText('devEmpty', 'Nothing is plugged into the USB-A port.');
  $('devIcon').classList.toggle('dim', s.state === 'unsupported');
  if (!d)
    return;
  setText('devName', d.product || (d.vid + ':' + d.pid));
  setText('devMaker', d.manufacturer || '');
  var why = s.state === 'unsupported' ? (s.reason || 'This device cannot be exported.') : '';
  showIf('devWhy', !!why);
  setText('devWhy', why);
  var kv = $('devKv');
  clear(kv);
  L.deviceFacts(d).forEach(function(row) {
    kv.appendChild(el('dt', row[0]));
    kv.appendChild(el('dd', row[1]));
  });
}

function renderClient(s) {
  var c = s.client;
  showIf('clientInfo', !!c);
  showIf('kickBtn', !!c);
  showIf('clientEmpty', !c);
  if (c) {
    setText('clientAddr', L.hostOnly(c.addr));
    setText('clientSince', L.sinceText(c.since, env.now()));
  } else {
    setText('clientEmpty', s.state === 'exportable' ? 'No computer has attached it yet.' : 'No client.');
  }
}

function renderStats(s) {
  var ids = [ [ 'stSessions', 'stSessionsSub' ], [ 'stTransfers', 'stTransfersSub' ], [ 'stFrom', 'stFromSub' ],
              [ 'stBuf', 'stBufSub' ] ];
  L.statFigures(s).forEach(function(f, i) {
    setText(ids[i][0], f.value);
    setText(ids[i][1], f.sub);
  });
}

/* Fills a command box: comment lines (# or REM) get their own style. */
function renderCommands(id, text) {
  var box = $(id);
  clear(box);
  text.split('\n').forEach(function(line) {
    var comment = /^(#|REM )/.test(line);
    var row = el('span', line + '\n', comment ? 'c' : '');
    box.appendChild(row);
  });
}

function render() {
  var s = status;
  if (!s)
    return;
  var v = L.view(s, hostName());
  var chip = $('headChip');
  chip.className = 'chip' + (v.chip.cls ? ' ' + v.chip.cls : '');
  chip.textContent = v.chip.text;
  if (staged === !!s.enabled)
    staged = null; // stored meanwhile (another tab)
  renderSave();
  setText('switchText', L.switchText(s));
  showIf('blockedNote', s.state === 'blocked');
  if (s.state === 'blocked')
    setText('blockedText', 'USB/IP is on but did not start at boot: ' + (s.reason || 'a radio on the USB port holds it') +
                               '. Thread and Zigbee win over USB/IP: move that radio off the USB port or switch it off on the ' +
                               'Radios page, then reboot.');
  showIf('failedNote', L.startFailed(s));
  if (L.startFailed(s))
    setText('failedText', s.reason + '. Reboot the board to try again.');
  // Blocked says what to do already; a pending change applies only once the block is gone.
  var pending = !!s.pending_reboot && s.state !== 'blocked';
  showIf('pendingNote', pending);
  if (pending)
    setText('pendingText', 'USB/IP was turned ' + (s.enabled ? 'on' : 'off') +
                               ' but the board has not rebooted yet: the change applies after the next reboot.');
  setText('ifaceName', L.interfaceName(s));
  showIf('deviceCard', v.show.device);
  showIf('clientCard', v.show.client);
  showIf('statsCard', v.show.stats);
  showIf('howCard', v.show.attach);
  if (v.show.device) {
    renderDevice(s);
    renderClient(s);
    renderStats(s);
    renderCommands('cmdLinux', v.commands.linux);
    renderCommands('cmdWin', v.commands.windows);
  }
}

function refresh() {
  return kit.request('GET', '/device/usbip', {timeoutMs : 5000}).then(function(r) {
    if (r.status !== 200 || !r.body) {
      setText('usbipStale', 'The USB/IP status could not be read: ' + kit.failure(r));
      showIf('usbipStale', true);
      return false;
    }
    status = r.body;
    showIf('usbipStale', false);
    render();
    return true;
  }, function(e) {
    setText('usbipStale', e.message);
    showIf('usbipStale', true);
    return false;
  });
}

function put(body) {
  return kit.request('PUT', '/device/usbip',
                     {headers : {'Content-Type' : 'application/json'}, body : JSON.stringify(body), timeoutMs : 10000});
}

/* ---- the switch ---- */

function shownValue() { return staged !== null ? staged : !!(status && status.enabled); }

/* The switch shows the staged value; Save and Reboot shows while it differs from the stored one. */
function renderSave() {
  $('toggle').setAttribute('aria-checked', String(shownValue()));
  showIf('saveRow', staged !== null);
  if (staged !== null)
    setText('saveText', L.rebootText(staged));
}

/* A click only stages the change; nothing is sent until Save and Reboot. Flipping back unstages it. */
function stage() {
  if (!status || kit.isBusy())
    return;
  var next = !shownValue();
  staged = next === !!status.enabled ? null : next;
  showIf('err409', false);
  note('toggleMsg', '', false);
  renderSave();
}

/* Back to the stored value, after the board answered without rebooting. */
function unstage() {
  staged = null;
  renderSave();
}

function showRefused(detail) {
  setText('err409Text', detail || 'A radio on the USB port runs Thread or Zigbee.');
  showIf('err409', true);
}

function apply() {
  var value = staged;
  if (value === null || kit.isBusy())
    return Promise.resolve(null);
  return kit.runOp(function() {
    showIf('err409', false);
    note('toggleMsg', '', false);
    return put({enabled : value}).then(function(r) {
      if (r.status === 200 && r.body && r.body.reboot) {
        showIf('saveRow', false); // saved: "Not saved yet" no longer holds
        note('toggleMsg', 'Saved. The board is rebooting (about 30 s); this page reloads when it is back.', false);
        kit.waitForBoard('toggleMsg');
        return {handoff : true};
      }
      unstage();
      if (r.status === 200)
        return refresh().then(function() { return r; });
      if (r.status === 409 && r.body && r.body.error === 'usb_channel_in_use') {
        showRefused(r.body.detail);
        return r;
      }
      note('toggleMsg', kit.failure(r), true);
      return r;
    }, function(e) {
      unstage();
      note('toggleMsg', e.message + ' If the board accepted the change it is rebooting: reload this page in a minute.',
           true);
    });
  });
}

/* ---- the client ---- */

function kick() {
  if (kit.isBusy())
    return Promise.resolve(null);
  return kit.runOp(function() {
    note('toggleMsg', '', false);
    return put({kick : true}).then(function(r) {
      if (r.status !== 200) {
        note('toggleMsg', kit.failure(r), true);
        return r;
      }
      return refresh().then(function() { return r; });
    }, function(e) { note('toggleMsg', e.message, true); });
  });
}

/* ---- the attach commands ---- */

function pickOs(linux) {
  showIf('cmdLinux', linux);
  showIf('cmdWin', !linux);
  $('osLinux').className = 'btn btn-sm ' + (linux ? 'btn-primary' : 'btn-secondary');
  $('osWin').className = 'btn btn-sm ' + (linux ? 'btn-secondary' : 'btn-primary');
}

function init() {
  $('toggle').addEventListener('click', stage);
  $('saveBtn').addEventListener('click', apply);
  $('kickBtn').addEventListener('click', kick);
  $('osLinux').addEventListener('click', function() { pickOs(true); });
  $('osWin').addEventListener('click', function() { pickOs(false); });
  kit.start();
  kit.poller(refresh, POLL_MS, null);
}

return {init : init, refresh : refresh, apply : apply, kick : kick, kit : kit};
}

if (typeof module !== 'undefined' && module.exports) {
  module.exports = UsbipLogic;
  module.exports.Page = UsbipPage;
}

if (typeof document !== 'undefined')
  UsbipPage(DeviceCommon.DeviceEnv()).init();
