/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP Thread Border Router - shared code of the Thread management pages (Network, Commissioner, Addresses, Tools):
 * the mode banner from /device/status, random network parameters, form validation.
 * MgmtLogic is pure and runs under node too (tools/tests/test_mgmt_pages.py); MgmtPage runs only in a browser.
 */
'use strict';

var MgmtLogic = (function() {

/*
 * Why Thread management does not work right now, from /device/status (null: no answer): {text, href, label}, or null
 * in BR_RUNNING.
 */
function modeBanner(s) {
  if (!s)
    return {text : 'No answer from the device.', href : '/', label : 'Open the Dashboard'};
  var noRadio = typeof DeviceCommon !== 'undefined' ? DeviceCommon : require('./device_common.js');
  var n = noRadio.threadNotice(s, null);
  if (n && !n.fault)
    return {text : n.text, href : n.href, label : n.label};
  if (s.mode === 'BR_RUNNING')
    return null;
  if (s.mode === 'SNIFFER')
    return {
      text : 'A packet capture is running: the Thread network is stopped until it ends.',
      href : '/capture.html',
      label : 'Open packet capture'
    };
  if (s.mode === 'RADIO_FAULT')
    return {
      text : 'The radio is not working' + (s.fault_reason ? ' (' + s.fault_reason + ')' : '') +
                 ', so the Thread network cannot run.',
      href : '/firmware.html',
      label : 'Open firmware'
    };
  if (s.mode === 'STARTING')
    return {text : 'The Thread stack is starting. Reload this page in a few seconds.', href : '/', label : 'Open the Dashboard'};
  return {text : 'The border router is in mode ' + String(s.mode) + '.', href : '/', label : 'Open the Dashboard'};
}

/* `rnd(n)` returns n random bytes (crypto.getRandomValues in a browser). */
function hexOf(bytes) {
  var out = '';
  for (var i = 0; i < bytes.length; i++)
    out += (bytes[i] < 16 ? '0' : '') + bytes[i].toString(16);
  return out;
}

var PASSPHRASE_CHARS = 'abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789';

/* Fresh parameters for a new network: a random key, PAN ID, Ext PAN ID, passphrase and ULA on-mesh prefix. */
function randomNetwork(rnd) {
  var pan = rnd(2);
  if (pan[0] === 0xff && pan[1] === 0xff)
    pan[1] = 0xfe; // 0xffff is the broadcast PAN ID
  var ula = rnd(5); // fd00::/8 plus a 40-bit global ID (RFC 4193)
  var tag = rnd(2);
  var phrase = '';
  var pr = rnd(12);
  for (var i = 0; i < pr.length; i++)
    phrase += PASSPHRASE_CHARS.charAt(pr[i] % PASSPHRASE_CHARS.length);
  var h = hexOf(ula);
  return {
    networkName : 'Thread-' + hexOf(tag).toUpperCase(),
    networkKey : hexOf(rnd(16)),
    panId : '0x' + hexOf(pan),
    extPanId : hexOf(rnd(8)),
    passphrase : phrase,
    prefix : 'fd' + h.slice(0, 2) + ':' + h.slice(2, 6) + ':' + h.slice(6, 10) + '::'
  };
}

function utf8Length(text) {
  try {
    return unescape(encodeURIComponent(text)).length;
  } catch (e) {
    return text.length;
  }
}

/* Problems of the Form Network fields: [{field, text}] (empty when it can be sent). */
function validateForm(d) {
  var out = [];
  function bad(field, text) { out.push({field : field, text : text}); }
  var name = String(d.networkName || '');
  if (!name.trim() || utf8Length(name) > 16)
    bad('networkName', 'Network name: 1 to 16 bytes.');
  if (!/^[0-9a-fA-F]{32}$/.test(String(d.networkKey || '')))
    bad('networkKey', 'Network key: 32 hex characters.');
  var pan = String(d.panId || '');
  if (!/^0x[0-9a-fA-F]{1,4}$/.test(pan) || parseInt(pan, 16) === 0xffff)
    bad('panId', 'PAN ID: 0x0000 to 0xfffe, with the 0x prefix.');
  var ch = String(d.channel === undefined ? '' : d.channel).trim();
  if (!/^\d+$/.test(ch) || Number(ch) < 11 || Number(ch) > 26)
    bad('channel', 'Channel: 11 to 26.');
  if (!/^[0-9a-fA-F]{16}$/.test(String(d.extPanId || '')))
    bad('extPanId', 'Extended PAN ID: 16 hex characters.');
  var phrase = String(d.passphrase || '');
  if (phrase.length < 6 || phrase.length > 255)
    bad('passphrase', 'Passphrase: 6 to 255 characters.');
  var prefix = String(d.prefix || '');
  if (!/^[0-9a-fA-F]{1,4}(:[0-9a-fA-F]{1,4}){0,3}::$/.test(prefix))
    bad('prefix', 'On-mesh prefix: up to four groups of a /64 prefix ending in "::", e.g. fd12:3456:789a::.');
  return out;
}

/* Joiner credential (PSKd) per the Thread spec: 6 to 32 of 0-9 and A-Z without I, O, Q and Z. Message or null. */
function checkPskd(text) {
  var p = String(text || '').trim();
  if (!p)
    return 'Enter the PSKd printed on the device (or its QR code).';
  if (p.length < 6 || p.length > 32)
    return 'The PSKd has 6 to 32 characters.';
  if (!/^[0-9A-HJ-NPR-Y]+$/.test(p))
    return 'The PSKd uses only digits and capital letters, without I, O, Q and Z.';
  return null;
}

/* Confirm text before a form or a scan-join replaces the network: `current` as DatasetJoin.readCurrentNetwork. */
function replaceQuestion(action, current) {
  if (!current)
    return action + '?';
  var what = current.unknown ? 'The network this board is on could not be read; it'
                             : 'This board is on the network ' + current.name + ' (channel ' + current.channel + '). It';
  return what + ' is replaced, and every device on it has to be joined again. ' + action + '?';
}

return {
  modeBanner : modeBanner,
  randomNetwork : randomNetwork,
  validateForm : validateForm,
  checkPskd : checkPskd,
  replaceQuestion : replaceQuestion
};
})();

if (typeof module !== 'undefined' && module.exports)
  module.exports = MgmtLogic;

/*
 * MgmtPage.guard(needsNetwork): reads /device/status and, outside BR_RUNNING, shows why at the top of the page and
 * disables its controls. With needsNetwork, also when the board is not on a Thread network. Resolves to
 * {running, attached}.
 */
var MgmtPage = typeof document === 'undefined' ? null : (function() {
  function banner(text, href, label) {
    var container = document.querySelector('.container');
    if (!container)
      return;
    var box = document.getElementById('mgmtBanner');
    if (!box) {
      box = document.createElement('div');
      box.id = 'mgmtBanner';
      box.className = 'disabled-banner';
      box.setAttribute('role', 'status');
      container.insertBefore(box, container.firstChild);
    }
    while (box.firstChild)
      box.removeChild(box.firstChild);
    box.appendChild(document.createTextNode(text + ' '));
    var a = document.createElement('a');
    a.href = href;
    a.textContent = label;
    box.appendChild(a);
  }

  var disabledByGuard = []; // only what the guard disabled, so a restore leaves the page's own disabled controls alone
  var RETRY_MS = 5000;

  /* A part of the page that works in every mode (the Leader card): data-guard-exempt on the element. */
  function exempt(el) { return !!el.closest && !!el.closest('[data-guard-exempt]'); }

  /* Disables the page's controls, the dialogs too (they sit outside .container), and closes an open dialog. */
  function disable() {
    var els = document.querySelectorAll(
        '.container button, .container input, .container select, .container textarea, ' +
        '.modal button, .modal input, .modal select, .modal textarea');
    for (var i = 0; i < els.length; i++) {
      if (!els[i].disabled && !exempt(els[i])) {
        els[i].disabled = true;
        disabledByGuard.push(els[i]);
      }
    }
    var cards = document.querySelectorAll('.card');
    for (var j = 0; j < cards.length; j++) {
      if (!exempt(cards[j]))
        cards[j].classList.add('card-disabled');
    }
    var open = document.querySelectorAll('.modal-overlay.active');
    for (var k = 0; k < open.length; k++)
      open[k].classList.remove('active');
  }

  function restore() {
    disabledByGuard.forEach(function(el) { el.disabled = false; });
    disabledByGuard = [];
    var cards = document.querySelectorAll('.card-disabled');
    for (var j = 0; j < cards.length; j++)
      cards[j].classList.remove('card-disabled');
    var box = document.getElementById('mgmtBanner');
    if (box)
      box.parentNode.removeChild(box);
  }

  function json(url) {
    return fetch(url).then(function(r) {
      return r.json().then(function(body) { return {status : r.status, body : body}; });
    });
  }

  /* One check: {running, attached, blocked}. */
  function check(needsNetwork) {
    return json('/device/status').then(function(r) { return r.status === 200 ? r.body : null; }, function() { return null; })
        .then(function(s) {
          var b = MgmtLogic.modeBanner(s);
          if (b)
            return {running : false, attached : false, blocked : b};
          return json('/node/state').then(function(r) { return r.status === 200 ? r.body : ''; }, function() { return ''; })
              .then(function(role) {
                var attached = role === 'child' || role === 'router' || role === 'leader';
                var blocked = needsNetwork && !attached ? {
                  text : 'This border router is not on a Thread network yet.',
                  href : '/network.html',
                  label : 'Form or join one'
                }
                                                        : null;
                return {running : true, attached : attached, blocked : blocked};
              });
        });
  }

  /*
   * Checks now; while the page is blocked (no answer, a radio fault, a capture, no network) it checks again every
   * 5 s and gives the controls back as soon as the reason is gone.
   */
  function guard(needsNetwork) {
    return check(needsNetwork).then(function(r) {
      if (r.blocked) {
        banner(r.blocked.text, r.blocked.href, r.blocked.label);
        disable();
        setTimeout(function() { guard(needsNetwork); }, RETRY_MS);
      } else if (disabledByGuard.length || document.getElementById('mgmtBanner')) {
        restore();
      }
      return {running : r.running, attached : r.attached};
    });
  }

  /* Random bytes for MgmtLogic.randomNetwork. */
  function rnd(n) {
    var a = new Uint8Array(n);
    window.crypto.getRandomValues(a);
    return Array.prototype.slice.call(a);
  }

  return {guard : guard, rnd : rnd};
})();

if (typeof module !== 'undefined' && module.exports && MgmtPage)
  module.exports.Page = MgmtPage;
