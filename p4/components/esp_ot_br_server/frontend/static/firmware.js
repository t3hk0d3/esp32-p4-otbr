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
  if (!/^https?:\/\//.test(u))
    return 'The URL must start with http:// or https://';
  return null;
}

/**
 * Hint for the radio card (the C6) from status.fault_reason. A fault while Thread runs on a USB radio
 * (status.otbr_channel other than 'c6') is that radio's: nothing to install here, no hint.
 */
function faultHint(reason, otbrChannel) {
  if (!!otbrChannel && otbrChannel !== 'c6')
    return {highlight : false, text : ''};
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

/* ---- the Updates card (GET /device/update) ---- */

var UPDATE_CHECK_MAX_AGE_S = 3600; // a check older than this is repeated when the page opens

/** The release as the card names it. */
function releaseName(u) {
  return u && u.release && u.release.tag ? u.release.tag : 'the latest release';
}

/** True when the page should start a check: never checked, or the last one is older than an hour (not while one runs). */
function updateNeedsCheck(u) {
  if (u.state === 'checking')
    return false;
  if (u.state === 'never')
    return true;
  return typeof u.checked_age_s === 'number' && u.checked_age_s > UPDATE_CHECK_MAX_AGE_S;
}

function appDiffers(u) { return u.app === 'newer' || u.app === 'unknown'; }

/** Chip of the card: {text, cls}. */
function updateChip(u) {
  if (u.state === 'checking')
    return {text : 'Checking', cls : ''};
  if (u.state === 'failed')
    return {text : 'Check failed', cls : 'chip-bad'};
  if (u.state === 'never' || !u.release)
    return {text : 'Not checked', cls : ''};
  if (u.app === 'older')
    return {text : 'Newer than the release', cls : ''};
  if (appDiffers(u))
    return {text : u.release.tag ? u.release.tag + ' available' : 'Update available', cls : 'chip-warn'};
  if (u.rcp_differs)
    return {text : 'Radio firmware differs', cls : 'chip-warn'};
  return {text : 'Up to date', cls : 'chip-ok'};
}

/** A version row: the release version and, under it, what is installed. `newer`: the installed build is the newer one. */
function versionRow(release, installed, newer) {
  if (release === null || release === undefined || release === '')
    return {value : '-', was : ''};
  if (installed === release)
    return {value : release, was : 'same as installed'};
  var was = installed === null || installed === undefined ? 'unknown' : installed === '' ? 'none' : installed;
  return {value : release, was : 'installed: ' + was + (newer ? ' (newer)' : '')};
}

/** Age of the last check as text. */
function checkedText(u) {
  var age = u.checked_age_s;
  if (typeof age !== 'number')
    return '-';
  var text = age < 60 ? 'just now' : age < 3600 ? Math.floor(age / 60) + ' min ago' : Math.floor(age / 3600) + ' h ago';
  return u.state === 'failed' ? text + ' (the last check failed)' : text;
}

/** What Install does: {label, primary, force}, or null (nothing to install, no result, or a check running). */
function updateAction(u) {
  if (u.state === 'checking' || !u.release)
    return null;
  var name = releaseName(u);
  if (u.app === 'older')
    return {label : 'Install ' + name + ' anyway', primary : false, force : true};
  if (appDiffers(u))
    return {label : 'Install ' + name + ' and reboot', primary : true, force : false};
  if (u.rcp_differs)
    return {label : 'Install the radio firmware of ' + name, primary : true, force : false};
  return null;
}

/** The release carries radio firmware: "Install anyway" (force) rewrites the radio even when it matches. */
function releaseHasRcp(u) { return !!(u.release && u.release.rcp); }

/** Description under the card's legend. */
function updateHint(u) {
  if (u.state === 'checking' || !u.release)
    return 'The board downloads the bundle and, when it differs, the radio firmware from the same release, then reboots.';
  var name = releaseName(u);
  if (u.app === 'older')
    return 'This board runs a build that is newer than ' + name + '. Installing goes back to the release' +
           (releaseHasRcp(u) ? ' and rewrites the radio firmware (the Thread network stops for about half a minute).'
                             : '.');
  var app = appDiffers(u);
  if (app && u.rcp_differs)
    return 'The board downloads the bundle and the radio firmware from ' + name + ', rewrites the radio (the Thread ' +
           'network stops for about half a minute), then reboots.';
  if (app)
    return 'The board downloads the bundle from ' + name + ', then reboots.';
  if (u.rcp_differs)
    return 'The board downloads the radio firmware from ' + name + ' and rewrites the radio. The Thread network stops ' +
           'for about half a minute.';
  return 'Nothing to install: the board runs the latest release.';
}

/** Confirm text before the install; `running` is the Border Router version of the last status. */
function updateQuestion(u, running) {
  var name = releaseName(u);
  var now = running || (u.running && u.running.app) || 'an unknown version';
  var radio = 'the radio firmware is rewritten too, so the Thread network stops for about half a minute';
  var gone = 'Thread and the web UI are gone for about a minute';
  if (u.app === 'older')
    return 'The board runs ' + now + ', which is a build that is newer than this release (' + name + '). Install ' +
           name + ' from GitHub anyway and reboot? ' + (releaseHasRcp(u) ? 'The radio firmware is rewritten too, so the ' +
           'Thread network stops for about half a minute. ' : '') + gone + ' during the reboot.';
  if (appDiffers(u))
    return 'Install ' + name + ' from GitHub and reboot? The board now runs ' + now + '; ' +
           (u.rcp_differs ? radio + ', and ' : '') + gone + ' during the reboot.';
  return 'Install the radio firmware of ' + name + ' from GitHub? The Thread network stops for about half a minute.';
}

/** Red status line of a failed check. */
function checkFailedText(u) {
  return 'The check failed' + (u.error ? ': ' + u.error : '') + '. The board needs internet access and DNS.';
}

var REBOOT_TEXT = 'Update installed. The board is rebooting; this page reloads when it is back.';

/*
 * Reading of the POST /device/update reply: {text, rebooting, rcpFailed} (`rcpFailed`: the detail to keep across the
 * reload, or null), or null when the reply is not a 200 (the page then shows the failure text). `stamp` is the radio
 * firmware version of the card's last check: the reply carries none.
 */
function updateReplyText(r, stamp) {
  if (r.status !== 200)
    return null;
  var b = r.body || {};
  if (b.rebooting)
    return {text : REBOOT_TEXT, rebooting : true, rcpFailed : b.rcp === 'failed' ? String(b.detail || 'unknown error') : null};
  return {text : 'Radio firmware installed' + (stamp ? ': ' + stamp : '') + '.', rebooting : false, rcpFailed : null};
}

/** Red line after the reboot of an update whose radio firmware push failed. */
/** `appChanged` false: the board came back on the old version (rolled back), so the Border Router was not updated. */
function rcpFailedText(detail, appChanged) {
  if (appChanged === false)
    return 'The radio firmware was not updated either: ' + detail + '.';
  return 'The Border Router was updated, but the radio firmware was not: ' + detail +
         '. Install again to update the radio firmware only.';
}

/** "owner/name" of a repository URL; the URL itself when it has no path. */
function repoLabel(url) {
  var m = /^[a-z]+:\/\/[^\/]+\/(.+?)\/*$/i.exec(String(url));
  return m ? m[1] : String(url);
}

/** The URL when the card may link to it (https only), else null. */
function linkUrl(url) { return /^https:\/\//i.test(String(url)) ? String(url) : null; }

return {
  radioFirmwareUrl : radioFirmwareUrl,
  checkUpdateUrl : checkUpdateUrl,
  faultHint : faultHint,
  jumperPresent : jumperPresent,
  installQuestion : installQuestion,
  afterUpdateText : afterUpdateText,
  rcpInstalledText : rcpInstalledText,
  releaseName : releaseName,
  updateNeedsCheck : updateNeedsCheck,
  updateChip : updateChip,
  versionRow : versionRow,
  checkedText : checkedText,
  updateAction : updateAction,
  updateHint : updateHint,
  updateQuestion : updateQuestion,
  checkFailedText : checkFailedText,
  updateReplyText : updateReplyText,
  rcpFailedText : rcpFailedText,
  releaseHasRcp : releaseHasRcp,
  repoLabel : repoLabel,
  linkUrl : linkUrl
};
})();

function FirmwarePage(env) {
var C = typeof DeviceCommon !== 'undefined' ? DeviceCommon : require('./device_common.js');
var L = FirmwareLogic;
var kit = C.DeviceKit(env, {
  actionIds : [ 'otaInstall', 'rcpInstall', 'updInstall', 'updCheck', 'recoverBtn', 'logBtn' ],
  onBusy : function() { lockUpdateButtons(); }
});
var $ = kit.$, setText = kit.setText, show = kit.show, note = kit.note;
var source = {ota : 'file', rcp : 'file'};
var running = null; // Border Router version from the last status
var faultReason = null; // fault_reason from the last status
var OTA_KEY = 'ota:before'; // version before an update, kept across the reload that follows it
var UPDATE_KEY = 'update:after'; // JSON {rcpFailed: detail | null}: the card's install, kept across the same reload
var upd = null; // last GET /device/update body
var rcpInstalled; // rcp.installed of the last status (undefined before the first one)
var sticky = false; // updMsg shows a result of the user's or the reload: the card's own lines leave it alone
var checkingShown = false; // updMsg shows the checking line
var updPoll = null; // follows a running check while the Updates card is on screen

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
  rcpInstalled = s.rcp.installed;
  var before = stored(OTA_KEY);
  var after = kit.isWaiting() ? null : L.afterUpdateText(before, s.app.version);
  if (!kit.isWaiting())
    reportAfterUpdate(after, !before || before !== s.app.version);
  setText('p4-slot', 'Slot ' + s.app.slot);
  setText('rcp-version', L.rcpInstalledText(s.rcp.installed));
  setText('rcp-compat', C.compatText(s.rcp));
  var hint = L.faultHint(s.fault_reason, s.otbr_channel);
  setText('radioHint', hint.text);
  show('radioHint', hint.highlight);
  $('rcpCard').style.outline = hint.highlight ? '2px solid var(--primary)' : '';
  var j = s.jumper, present = L.jumperPresent(s);
  setText('jumperChip', present ? (j.recovery_failed ? 'Jumper fitted, last recovery failed' : 'Jumper fitted')
                                : 'Jumper not fitted');
  $('jumperChip').className = present ? 'chip chip-warn' : 'chip';
  show('noJumper', !present);
  show('jumperControls', present);
  if (upd)
    renderUpdate();
}

/* After the reboot of an update: the note goes to the card that started it (update:after), else to the bundle card. */
function reportAfterUpdate(after, appChanged) {
  var pending = null;
  try {
    pending = JSON.parse(stored(UPDATE_KEY));
  } catch (e) {
  }
  if (pending && typeof pending === 'object') {
    var failed = pending.rcpFailed ? L.rcpFailedText(pending.rcpFailed, appChanged) : null;
    note('updMsg', [ after, failed ].filter(Boolean).join(' '), !!failed);
    sticky = true;
    checkingShown = false;
    stored(UPDATE_KEY, null);
    stored(OTA_KEY, null);
  }
  else if (after) {
    note('otaMsg', after, false);
    stored(OTA_KEY, null);
  }
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

/* ---- Updates from GitHub ---- */

function lockUpdateButtons() {
  var on = kit.isBusy() || !!(upd && upd.state === 'checking');
  [ 'updInstall', 'updCheck' ].forEach(function(id) {
    var e = $(id);
    e.disabled = on;
    e.setAttribute('aria-disabled', on ? 'true' : 'false');
    e.classList.toggle('disabled', on);
  });
}

function linkTo(url, text) {
  var a = kit.el('a', text);
  a.setAttribute('href', url);
  a.setAttribute('target', '_blank');
  a.setAttribute('rel', 'noopener');
  return a;
}

/* A version row: the release version, the installed one under it in muted text. */
function renderVersion(id, row) {
  var dd = $(id);
  kit.clear(dd);
  dd.appendChild(env.document.createTextNode(row.value));
  if (row.was)
    dd.appendChild(kit.el('span', row.was, 'was'));
}

function renderUpdate() {
  var u = upd, chip = L.updateChip(u), rel = u.release, run = u.running || {};
  setText('updChip', chip.text);
  $('updChip').className = chip.cls ? 'chip ' + chip.cls : 'chip';
  var repo = $('updRepo'), repoUrl = L.linkUrl(u.repo_url);
  setText('updRepo', u.repo_url ? L.repoLabel(u.repo_url) : 'the repository');
  if (repoUrl) {
    repo.setAttribute('href', repoUrl);
    repo.setAttribute('target', '_blank');
    repo.setAttribute('rel', 'noopener');
  }
  else
    repo.removeAttribute('href');
  var dd = $('updRelease');
  kit.clear(dd);
  if (!rel)
    dd.appendChild(env.document.createTextNode('-'));
  else {
    dd.appendChild(env.document.createTextNode(rel.tag || 'latest'));
    var relUrl = L.linkUrl(rel.url);
    if (relUrl) {
      dd.appendChild(env.document.createTextNode(' · '));
      dd.appendChild(linkTo(relUrl, 'release notes'));
    }
  }
  var runApp = running || run.app, runRcp = rcpInstalled !== undefined ? rcpInstalled : run.rcp;
  renderVersion('updApp', L.versionRow(rel ? rel.app : null, runApp === undefined ? null : runApp, u.app === 'older'));
  renderVersion('updRcp', L.versionRow(rel ? rel.rcp : null, runRcp === undefined ? null : runRcp, false));
  setText('updChecked', L.checkedText(u));
  setText('updHint', L.updateHint(u));
  var act = L.updateAction(u), btn = $('updInstall');
  show('updInstall', !!act);
  if (act) {
    setText('updInstall', act.label);
    btn.className = act.primary ? 'btn btn-primary' : 'btn btn-outline';
  }
  lockUpdateButtons();
}

/* A new GET /device/update body: the card, and the status line when the check starts or ends. */
function setUpdate(body) {
  upd = body;
  renderUpdate();
  if (sticky)
    return;
  if (body.state === 'checking') {
    note('updMsg', 'Checking GitHub for the latest release...', false);
    checkingShown = true;
  }
  else if (body.state === 'failed') {
    note('updMsg', L.checkFailedText(body), true);
    checkingShown = false;
  }
  else if (checkingShown) {
    note('updMsg', '', false);
    checkingShown = false;
  }
}

function loadUpdate() {
  return kit.request('GET', '/device/update', {timeoutMs : 5000}).then(function(r) {
    if (r.status === 200 && r.body && typeof r.body === 'object') {
      setUpdate(r.body);
      return r.body;
    }
    note('updMsg', kit.failure(r), true);
    return null;
  }, function(e) {
    note('updMsg', e.message, true);
    return null;
  });
}

/* Asks the board for a check; the poller follows it to the end. */
function startCheck() {
  return kit.request('GET', '/device/update?check=true', {timeoutMs : 5000}).then(function(r) {
    if (r.status === 200 && r.body && typeof r.body === 'object') {
      setUpdate(r.body);
      if (updPoll)
        updPoll.wake();
    }
    else
      note('updMsg', kit.failure(r), true);
    return r;
  }, function(e) {
    note('updMsg', e.message, true);
    return null;
  });
}

function checkUpdate() {
  if (kit.isBusy() || (upd && upd.state === 'checking'))
    return Promise.resolve(null);
  sticky = false;
  checkingShown = false;
  note('updMsg', '', false);
  return startCheck();
}

function finishUpdate(r) {
  var reply = L.updateReplyText(r, upd && upd.release ? upd.release.rcp : null);
  if (!reply) {
    note('updMsg', kit.failure(r), true);
    return null;
  }
  if (reply.rebooting) {
    stored(OTA_KEY, running || '');
    stored(UPDATE_KEY, JSON.stringify({rcpFailed : reply.rcpFailed}));
    note('updMsg', reply.text, false);
    kit.waitForBoard('updMsg');
    return {handoff : true};
  }
  note('updMsg', reply.text, false);
  refreshStatus();
  sticky = true;
  startCheck();
  return r;
}

function installUpdate() {
  var act = upd ? L.updateAction(upd) : null;
  if (kit.isBusy() || !act || !env.confirm(L.updateQuestion(upd, running)))
    return Promise.resolve(null);
  return kit.runOp(function() {
    sticky = true;
    note('updMsg', 'The device is downloading ' + L.releaseName(upd) + ' from GitHub and installing it ...', false);
    return kit.request('POST', '/device/update' + (act.force ? '?force=1' : '')).then(finishUpdate, function(e) {
      note('updMsg', e.message, true);
      return null;
    });
  });
}

/* One poll of the Updates card: the check while it runs, nothing otherwise. */
function followCheck() {
  if (!upd || upd.state !== 'checking')
    return Promise.resolve(true);
  return loadUpdate().then(function(u) { return u !== null; });
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
  $('updInstall').addEventListener('click', installUpdate);
  $('updCheck').addEventListener('click', checkUpdate);
  kit.start();
  kit.poller(refreshStatus, 5000, 'firmwareSection');
  updPoll = kit.poller(followCheck, 5000, 'updCard');
  loadUpdate().then(function(u) {
    if (u && L.updateNeedsCheck(u))
      startCheck();
  });
}

return {
  init : init,
  refreshStatus : refreshStatus,
  install : install,
  installUpdate : installUpdate,
  checkUpdate : checkUpdate,
  recover : recover,
  showLog : showLog,
  kit : kit
};
}

if (typeof module !== 'undefined' && module.exports) {
  module.exports = FirmwareLogic;
  module.exports.Page = FirmwarePage;
}

if (typeof document !== 'undefined')
  FirmwarePage(DeviceCommon.DeviceEnv()).init();
