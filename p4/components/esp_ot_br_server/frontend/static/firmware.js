/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP Thread Border Router - Firmware page (/device/ota, /device/radio/*, /device/status)
 *
 * FirmwareLogic is pure and runs under node too (tools/tests/test_firmware_page.py). FirmwarePage is the page.
 */
'use strict';

var FirmwareLogic = (function() {

/** The single radio firmware endpoint: a file upload without source, or an empty body with ?from_url=. */
function radioFirmwareUrl(fromUrl) {
  var base = '/device/radio/firmware';
  return fromUrl ? base + '?from_url=' + encodeURIComponent(fromUrl) : base;
}

/** Message for a source URL the device cannot use, or null. */
function checkUpdateUrl(url) {
  var u = String(url).trim();
  if (u === '')
    return 'Enter a URL.';
  if (!/^https?:\/\//i.test(u))
    return 'The URL must start with http:// or https://';
  return null;
}

/** Hint for the radio card from status.fault_reason. */
function faultHint(reason) {
  if (reason === 'no rcp firmware')
    return {highlight : true, text : 'The radio co-processor has no valid firmware. Install c6_rcp.bin below.'};
  if (reason === 'esp-hosted firmware')
    return {
      highlight : true,
      text : 'The radio co-processor still runs its factory ESP-Hosted firmware. Install c6_installer.bin below first (once), then c6_rcp.bin.'
    };
  if (reason === 'rcp incompatible')
    return {
      highlight : true,
      text : 'The radio firmware does not match this border router build. Install the c6_rcp.bin of this release below.'
    };
  return {highlight : false, text : ''};
}

function jumperPresent(status) {
  return !!(status && status.jumper && status.jumper.present === true);
}

/*
 * Confirm text before an install: `card` 'ota' or 'rcp', `what` the file name or URL, `running` the Border Router
 * version, `faultReason` status.fault_reason (the installer step must not lose power).
 */
function installQuestion(card, what, running, faultReason) {
  if (card === 'ota')
    return 'Install ' + what + ' and reboot? The board now runs ' + (running || 'an unknown version') +
           '; Thread and the web UI are gone for about a minute.';
  if (faultReason === 'esp-hosted firmware')
    return 'Install the radio firmware ' + what + '? This replaces the factory ESP-Hosted firmware and takes ' +
           'about a minute. Do not power off or reset the board until it is done, or the radio may need the ' +
           'maintenance jumper to recover.';
  return 'Install the radio firmware ' + what + '? The Thread network stops for about half a minute.';
}

/** Note after the reboot of an update: `before` the version before it (null: no update was waited for). */
function afterUpdateText(before, now) {
  if (!before)
    return null;
  if (before === now)
    return 'The board is back and still runs ' + now + ' (the bundle had the same version, or the update was rolled back).';
  return 'Update done: the board now runs ' + now + ' (before: ' + before + ').';
}

function rcpInstalledText(installed) {
  if (installed === null || installed === undefined)
    return 'unknown (probe failed)';
  return installed === '' ? 'none (no valid RCP firmware)' : String(installed);
}

return {
  radioFirmwareUrl : radioFirmwareUrl,
  checkUpdateUrl : checkUpdateUrl,
  faultHint : faultHint,
  jumperPresent : jumperPresent,
  installQuestion : installQuestion,
  afterUpdateText : afterUpdateText,
  rcpInstalledText : rcpInstalledText
};
})();

function FirmwarePage(env) {
var C = typeof DeviceCommon !== 'undefined' ? DeviceCommon : require('./device_common.js');
var L = FirmwareLogic;
var kit = C.DeviceKit(env, {actionIds : [ 'otaInstall', 'rcpInstall', 'recoverBtn', 'logBtn' ]});
var $ = kit.$, setText = kit.setText, show = kit.show, note = kit.note;
var source = {ota : 'file', rcp : 'file'};
var running = null; // Border Router version from the last status
var faultReason = null; // fault_reason from the last status
var OTA_KEY = 'ota:before'; // version before an update, kept across the reload that follows it

function stored(key, value) {
  try {
    var s = env.storage || null;
    if (!s)
      return null;
    if (value === undefined)
      return s.getItem(key);
    if (value === null)
      s.removeItem(key);
    else
      s.setItem(key, value);
  } catch (e) {
  }
  return null;
}

function render(s) {
  show('fw-stale', false);
  setText('p4-version', s.app.version);
  running = s.app.version;
  faultReason = s.fault_reason || null;
  // While an update is waited for, the status is still the old image's: keep the note for the board that comes back.
  var after = kit.isWaiting() ? null : L.afterUpdateText(stored(OTA_KEY), s.app.version);
  if (after) {
    note('otaMsg', after, false);
    stored(OTA_KEY, null);
  }
  setText('p4-slot', 'Slot ' + s.app.slot);
  setText('rcp-version', L.rcpInstalledText(s.rcp.installed));
  setText('rcp-compat', C.compatText(s.rcp));
  var hint = L.faultHint(s.fault_reason);
  setText('radioHint', hint.text);
  show('radioHint', hint.highlight);
  $('rcpCard').style.outline = hint.highlight ? '2px solid var(--primary)' : '';
  var j = s.jumper, present = L.jumperPresent(s);
  setText('jumperChip', present ? (j.recovery_failed ? 'Jumper fitted, last recovery failed' : 'Jumper fitted')
                                : 'Jumper not fitted');
  $('jumperChip').className = present ? 'chip chip-warn' : 'chip';
  show('noJumper', !present);
  show('jumperControls', present);
}

function refreshStatus() {
  return kit.request('GET', '/device/status', {timeoutMs : 5000}).then(function(r) {
    if (r.status === 200 && r.body) {
      render(r.body);
      return true;
    }
    setText('fw-stale', 'The firmware state could not be read: ' + kit.failure(r));
    show('fw-stale', true);
    return false;
  }, function(e) {
    setText('fw-stale', e.message);
    show('fw-stale', true);
    return false;
  });
}

/* ---- source per card: a file or a URL ---- */

function setSource(card, which) {
  source[card] = which;
  $(card + '-src-file').setAttribute('aria-pressed', which === 'file' ? 'true' : 'false');
  $(card + '-src-url').setAttribute('aria-pressed', which === 'url' ? 'true' : 'false');
  show(card + '-file-pane', which === 'file');
  show(card + '-url-pane', which === 'url');
}

function showFileName(card) {
  var f = $(card + 'File').files[0];
  setText(card + 'DropName', f ? f.name : '');
}

function wireDrop(card) {
  var zone = $(card + 'Drop');
  zone.addEventListener('dragover', function(ev) {
    ev.preventDefault();
    zone.classList.add('dragging');
  });
  zone.addEventListener('dragleave', function() { zone.classList.remove('dragging'); });
  zone.addEventListener('drop', function(ev) {
    ev.preventDefault();
    zone.classList.remove('dragging');
    if (ev.dataTransfer && ev.dataTransfer.files && ev.dataTransfer.files.length) {
      $(card + 'File').files = ev.dataTransfer.files;
      showFileName(card);
    }
  });
  $(card + 'File').addEventListener('change', function() { showFileName(card); });
}

/* ---- uploads and installs ---- */

function sendFile(url, file, msgId, label) {
  if (!file) {
    note(msgId, 'Choose a file first.', true);
    return Promise.resolve(null);
  }
  var tooBig = C.checkUploadSize(label, file.size);
  if (tooBig) {
    note(msgId, tooBig, true);
    return Promise.resolve(null);
  }
  if (!env.confirm(L.installQuestion(label === 'bundle' ? 'ota' : 'rcp', file.name, running, faultReason)))
    return Promise.resolve(null);
  return kit.runOp(function() {
    note(msgId, 'Uploading ' + file.name + ' (' + file.size + ' bytes)...', false);
    // A File body makes the browser send Content-Length, which the device requires.
    return kit.request('POST', url, {headers : {'Content-Type' : 'application/octet-stream'}, body : file, upload : true})
        .then(function(r) { return finishInstall(r, msgId, label); }, function(e) {
          note(msgId, e.message, true);
          return null;
        });
  });
}

function sendUrl(url, src, msgId, label, jsonBody) {
  var problem = L.checkUpdateUrl(src);
  if (problem) {
    note(msgId, problem, true);
    return Promise.resolve(null);
  }
  if (!env.confirm(L.installQuestion(label === 'bundle' ? 'ota' : 'rcp', src.trim(), running, faultReason)))
    return Promise.resolve(null);
  return kit.runOp(function() {
    note(msgId, 'The device is downloading ' + src.trim() + ' ...', false);
    var opts = jsonBody ? {headers : {'Content-Type' : 'application/json'}, body : JSON.stringify({url : src.trim()})}
                        : {};
    return kit.request('POST', url, opts).then(function(r) { return finishInstall(r, msgId, label); }, function(e) {
      note(msgId, e.message, true);
      return null;
    });
  });
}

function finishInstall(r, msgId, label) {
  if (r.status !== 200) {
    note(msgId, kit.failure(r), true);
    return null;
  }
  if (label === 'bundle') {
    stored(OTA_KEY, running || '');
    note(msgId, 'Update installed. The board is rebooting; this page reloads when it is back.', false);
    kit.waitForBoard(msgId);
    return {handoff : true};
  }
  var v = r.body && r.body.installed;
  note(msgId, 'Radio firmware installed' + (v ? ': ' + v : '') + '.', false);
  refreshStatus();
  return r;
}

/** Install button of a card ('ota' or 'rcp'), with the source chosen on that card. */
function install(card) {
  if (kit.isBusy())
    return Promise.resolve(null);
  if (card === 'ota') {
    if (source.ota === 'url')
      return sendUrl('/device/ota/url', $('otaUrl').value, 'otaMsg', 'bundle', true);
    return sendFile('/device/ota', $('otaFile').files[0], 'otaMsg', 'bundle');
  }
  if (source.rcp === 'url') {
    var u = String($('rcpUrl').value).trim();
    return sendUrl(L.radioFirmwareUrl(u), u, 'rcpMsg', 'rcp', false);
  }
  return sendFile(L.radioFirmwareUrl(''), $('rcpFile').files[0], 'rcpMsg', 'rcp');
}

/* ---- radio recovery ---- */

function recover() {
  if (kit.isBusy() || !env.confirm('Rewrite the radio co-processor base image over the maintenance jumper? The Thread network stops ' +
                                   'while it runs; the radio firmware may have to be installed again afterwards.'))
    return Promise.resolve(null);
  return kit.runOp(function() {
    note('recoverMsg', 'Rewriting the radio co-processor base image through the ROM loader (this takes a while)...', false);
    return kit.request('POST', '/device/radio/recover').then(function(r) {
      note('recoverMsg', r.status === 200 ? (r.body && r.body.detail) || 'Done.' : kit.failure(r), r.status !== 200);
      refreshStatus();
      return r;
    }, function(e) { note('recoverMsg', e.message, true); });
  });
}

function showLog() {
  return kit.request('GET', '/device/radio/log').then(function(r) {
    if (r.status === 200) {
      setText('c6Log', r.text || '(empty)');
      show('c6Log', true);
    }
    else
      note('recoverMsg', kit.failure(r), true);
  }, function(e) { note('recoverMsg', e.message, true); });
}

function init() {
  [ 'ota', 'rcp' ].forEach(function(card) {
    $(card + '-src-file').addEventListener('click', function() { setSource(card, 'file'); });
    $(card + '-src-url').addEventListener('click', function() { setSource(card, 'url'); });
    $(card + 'Install').addEventListener('click', function() { install(card); });
    wireDrop(card);
  });
  // A file dropped beside a drop zone would make the browser open it and leave the page.
  [ 'dragover', 'drop' ].forEach(function(t) { env.document.addEventListener(t, function(ev) { ev.preventDefault(); }); });
  $('recoverBtn').addEventListener('click', recover);
  $('logBtn').addEventListener('click', showLog);
  kit.start();
  kit.poller(refreshStatus, 5000, 'firmwareSection');
}

return {init : init, refreshStatus : refreshStatus, install : install, recover : recover, showLog : showLog, kit : kit};
}

if (typeof module !== 'undefined' && module.exports) {
  module.exports = FirmwareLogic;
  module.exports.Page = FirmwarePage;
}

if (typeof document !== 'undefined')
  FirmwarePage(DeviceCommon.DeviceEnv()).init();
