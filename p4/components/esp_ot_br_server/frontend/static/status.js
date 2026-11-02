/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP Thread Border Router - Device status page, device.html (/device/status, /device/diag, /device/reboot)
 *
 * StatusLogic is pure and runs under node too (tools/tests/test_status_page.py). StatusPage is the page.
 */
'use strict';

var StatusLogic = (function() {

function faultText(reason) {
  if (reason === 'no rcp firmware')
    return 'The radio has no firmware, so the Thread network cannot start. Install c6_rcp.bin on the Firmware page.';
  if (reason === 'rcp incompatible')
    return 'The radio firmware does not match this build, so the Thread network cannot start. Install the ' +
           'c6_rcp.bin of the same release.';
  return 'The radio stopped answering' + (reason ? ' (' + reason + ')' : '') +
         '. Updating or recovering the radio firmware may help.';
}

/** The banner of the page: {tone: ok|info|fault|idle, title, text, link: {href, label} | null}. */
function heroFor(s) {
  if (s.mode === 'BR_RUNNING')
    return {
      tone : 'ok',
      title : 'Border router running',
      text : 'The Thread stack is up and the radio firmware matches this build.',
      link : null
    };
  if (s.mode === 'SNIFFER')
    return {
      tone : 'info',
      title : 'Capturing packets',
      text : 'Channel ' + s.sniffer.channel + ', ' + s.sniffer.frames +
                 ' frames so far. The Thread network is stopped until the capture ends.',
      link : {href : '/capture.html', label : 'Open packet capture'}
    };
  if (s.mode === 'RADIO_FAULT')
    return {
      tone : 'fault',
      title : 'Radio fault',
      text : faultText(s.fault_reason),
      link : {href : '/firmware.html', label : 'Open firmware'}
    };
  if (s.mode === 'STARTING')
    return {
      tone : 'idle',
      title : 'Starting',
      text : 'The Thread stack is starting. This takes a few seconds after a boot or a radio update.',
      link : null
    };
  return {tone : 'idle', title : String(s.mode), text : '', link : null};
}

function snifferText(sn) {
  if (sn.active)
    return {state : 'Capturing on channel ' + sn.channel, detail : sn.frames + ' frames, ' + sn.drops + ' dropped'};
  if (sn.ended_by_fault)
    return {state : 'Idle', detail : 'The last capture was ended by a radio fault'};
  if (sn.frames)
    return {state : 'Idle', detail : 'Last capture: ' + sn.frames + ' frames, ' + sn.drops + ' dropped'};
  return {state : 'Idle', detail : 'No capture since boot'};
}

function pdText(pd) {
  return {
    state : pd.state === null || pd.state === undefined ? 'unknown (OpenThread was busy)' : String(pd.state),
    prefix : pd.prefix || 'None received'
  };
}

return {heroFor : heroFor, snifferText : snifferText, pdText : pdText};
})();

function StatusPage(env) {
var C = typeof DeviceCommon !== 'undefined' ? DeviceCommon : require('./device_common.js');
var L = StatusLogic;
var kit = C.DeviceKit(env, {actionIds : [ 'rebootBtn' ]});
var $ = kit.$, setText = kit.setText, show = kit.show, note = kit.note;
var lastSeen = 0;

function rcpText(installed) {
  if (installed === null || installed === undefined)
    return 'unknown (probe failed)';
  return installed === '' ? 'none (no valid RCP firmware)' : String(installed);
}

function render(s) {
  lastSeen = env.now();
  show('s-offline', false);
  var h = L.heroFor(s);
  $('hero').className = 'card hero hero-' + h.tone;
  setText('h-title', h.title);
  setText('h-text', h.text);
  show('h-link', !!h.link);
  if (h.link) {
    $('h-link').setAttribute('href', h.link.href);
    setText('h-link', h.link.label);
  }
  setText('h-mode', s.mode);
  setText('t-app', s.app.version);
  setText('t-slot', 'Slot ' + s.app.slot);
  setText('t-storage', s.storage_ok === undefined ? '-' : s.storage_ok ? 'OK' : 'Failed: settings are not saved');
  setText('t-rcp', rcpText(s.rcp.installed));
  setText('t-compat', s.rcp.compatible ? 'Yes' : 'No');
  var j = s.jumper;
  setText('t-jumper', j.present ? (j.recovery_failed ? 'Fitted, last recovery failed' : 'Fitted') : 'Not fitted');
  setText('t-base', !j.present ? '-'
                    : !j.base_installed ? 'unknown (expected ' + j.base_expected + ')'
                    : j.base_installed === j.base_expected ? j.base_installed + ', as expected'
                    : j.base_installed + ' (expected ' + j.base_expected + ')');
  setText('t-ipv4', s.eth.ipv4 || '-');
  setText('t-ipv6', s.eth.ipv6.length ? s.eth.ipv6.join('\n') : '-');
  var pd = L.pdText(s.pd);
  setText('t-pd-state', pd.state);
  setText('t-pd-prefix', pd.prefix);
  var sn = L.snifferText(s.sniffer);
  setText('t-sniffer-state', sn.state);
  setText('t-sniffer-detail', sn.detail);
}

function showOffline() {
  setText('s-offline', C.lastSeenText(lastSeen, env.now()));
  show('s-offline', true);
}

function refreshStatus() {
  return kit.request('GET', '/device/status', {timeoutMs : 5000}).then(function(r) {
    if (r.status === 200 && r.body) {
      render(r.body);
      return true;
    }
    showOffline();
    return false;
  }, function() {
    showOffline();
    return false;
  });
}

function refreshDiag() {
  return kit.request('GET', '/device/diag', {timeoutMs : 5000}).then(function(r) {
    if (r.status !== 200 || !r.body)
      return false;
    var d = r.body;
    kit.noteUptime(d.uptime_s);
    setText('h-uptime', C.durationText(d.uptime_s));
    setText('h-reset', d.reset_reason);
    setText('t-cpu', d.cpu.map(function(c, i) {
      return (i ? 'core ' : 'Core ') + c.core + ' ' + C.pct(c.load_pct['10s']);
    }).join(', '));
    var mi = d.memory.internal;
    setText('t-mem-int', C.bytesText(mi.free) + ' (lowest ' + C.bytesText(mi.min_free) + ')');
    setText('t-mem-psram', C.bytesText(d.memory.psram.free));
    return true;
  }, function() { return false; });
}

function reboot() {
  if (kit.isBusy() || !env.confirm('Reboot the board now?'))
    return Promise.resolve(null);
  return kit.runOp(function() {
    return kit.request('POST', '/device/reboot').then(function(r) {
      if (r.status !== 200) {
        note('rebootMsg', kit.failure(r), true);
        return r;
      }
      note('rebootMsg', 'Rebooting; this page reloads when the board is back.', false);
      kit.waitForBoard('rebootMsg');
      return {handoff : true};
    }, function(e) { note('rebootMsg', e.message, true); });
  });
}

function init() {
  $('rebootBtn').addEventListener('click', reboot);
  env.setInterval(function() {
    if (!$('s-offline').classList.contains('hidden'))
      showOffline();
  }, 1000);
  kit.start();
  kit.poller(refreshStatus, 3000, 'statusSection');
  kit.poller(refreshDiag, 10000, 'statusSection');
}

return {init : init, refreshStatus : refreshStatus, refreshDiag : refreshDiag, reboot : reboot, kit : kit};
}

if (typeof module !== 'undefined' && module.exports) {
  module.exports = StatusLogic;
  module.exports.Page = StatusPage;
}

if (typeof document !== 'undefined')
  StatusPage(DeviceCommon.DeviceEnv()).init();
