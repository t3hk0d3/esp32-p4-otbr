/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP Thread Border Router - Device page (/device/* API)
 *
 * Part 1 is pure logic and runs under node too (tools/tests/test_device_page.py). Part 2 is the page. Everything that
 * comes from the device is inserted with textContent, never as markup.
 */
'use strict';

var DeviceLogic = (function() {
var CHANNEL_MIN = 11, CHANNEL_MAX = 26, DURATION_MAX = 3600, VLAN_MAX = 4094;

function wholeNumber(value, min, max) {
  var text = String(value).trim();
  if (!/^\d+$/.test(text))
    return null;
  var n = Number(text);
  return (n >= min && n <= max) ? n : null;
}

/** URL of the capture stream, or null when channel or duration is out of range. */
function captureUrl(channel, duration) {
  var c = wholeNumber(channel, CHANNEL_MIN, CHANNEL_MAX);
  var d = wholeNumber(duration, 1, DURATION_MAX);
  if (c === null || d === null)
    return null;
  return '/device/sniffer.pcap?channel=' + c + '&duration=' + d;
}

/** The single firmware endpoint: a file upload without source, or an empty body with ?from_url=. */
function radioFirmwareUrl(fromUrl) {
  var base = '/device/radio/firmware';
  return fromUrl ? base + '?from_url=' + encodeURIComponent(fromUrl) : base;
}

/** Message for a source URL the device cannot use, or null. */
function checkUpdateUrl(url) {
  var u = String(url).trim();
  if (u === '')
    return 'Enter a URL.';
  if (/^https:/i.test(u))
    return 'https is not supported: the device downloads over plain http:// only.';
  if (!/^http:\/\//i.test(u))
    return 'The URL must start with http://';
  return null;
}

var STATUS_TEXT = {
  409 : 'The device is busy',
  412 : 'Not possible without the maintenance jumper',
  413 : 'Too large',
  502 : 'Transfer failed',
  503 : 'Not available now'
};

/** Human-readable text for an error reply: status explanation plus the detail of {"error","detail"}. */
function errorMessage(status, body) {
  var detail = body && typeof body.detail === 'string' ? body.detail : '';
  var code = body && typeof body.error === 'string' ? body.error : '';
  var lead = STATUS_TEXT[status] || ('Request failed (HTTP ' + status + ')');
  var rest = detail || code;
  return rest ? lead + ': ' + rest : lead;
}

/** Hint for the radio card from status.fault_reason. */
function faultHint(reason) {
  if (reason === 'no rcp firmware')
    return {
      highlight : true,
      text : 'The C6 has no valid RCP firmware. Upload c6_rcp.bin below.'
    };
  if (reason === 'rcp incompatible')
    return {
      highlight : true,
      text :
          'The RCP firmware does not match this application. Upload the c6_rcp.bin of this release below.'
    };
  return {highlight : false, text : ''};
}

function jumperPresent(status) {
  return !!(status && status.jumper && status.jumper.present === true);
}

function rcpInstalledText(installed) {
  if (installed === null || installed === undefined)
    return 'unknown (probe failed)';
  return installed === '' ? 'none (no valid RCP firmware)' : String(installed);
}

function formatRemaining(seconds) {
  var s = Math.max(0, Math.floor(seconds));
  var rest = s % 60;
  return Math.floor(s / 60) + ':' + (rest < 10 ? '0' : '') + rest;
}

function ipv4Valid(text) {
  var parts = String(text).trim().split('.');
  if (parts.length !== 4)
    return false;
  return parts.every(function(p) { return /^\d{1,3}$/.test(p) && Number(p) <= 255; });
}

function ipv4ToNumber(text) {
  return String(text).trim().split('.').reduce(function(acc, p) { return acc * 256 + Number(p); }, 0);
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

/** Interface object for the API from form strings; throws Error with a message the user can act on. */
function ifaceConfig(name, f) {
  var vlan = wholeNumber(f.vlan, 0, VLAN_MAX);
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
    return {mode : 'single', single : ifaceConfig('Interface', form.single || {})};
  if (form.mode === 'split') {
    var mgmt = ifaceConfig('Management', form.mgmt || {});
    var bb = ifaceConfig('Thread backbone', form.thread_bb || {});
    if (mgmt.vlan === bb.vlan)
      throw new Error('Management and Thread backbone need different VLAN IDs (two untagged interfaces ' +
                      'are not possible either).');
    return {mode : 'split', mgmt : mgmt, thread_bb : bb};
  }
  throw new Error('Choose a mode.');
}

/*
 * Upload limits, the same as the device enforces (413 otherwise): an RCP image must fit the C6 OTA slot
 * (C6_OTA_SLOT_SIZE, common/c6_board/c6_board.h); a bundle is the 44-byte header plus at most the web partition (2 MB)
 * plus the app partition (8 MB), partitions.csv. tools/tests/test_device_page.py compares these with the sources.
 */
var RCP_MAX_BYTES = 0x180000;
var BUNDLE_MAX_BYTES = 44 + 0x200000 + 0x800000;

/** Message when a file cannot be sent (kind 'bundle' or 'rcp'), or null. */
function checkUploadSize(kind, size) {
  var max = kind === 'bundle' ? BUNDLE_MAX_BYTES : RCP_MAX_BYTES;
  if (!size)
    return 'The file is empty.';
  if (size > max)
    return 'Too large: ' + size + ' bytes, the device accepts at most ' + max + ' bytes.';
  return null;
}

var POLL_MAX_MS = 30000;

/** Delay before the next poll: the period, doubled per consecutive failure up to 30 s. */
function nextDelay(period, failures) {
  return failures <= 0 ? period : Math.min(POLL_MAX_MS, period * Math.pow(2, failures));
}

function lastSeenText(lastSeenMs, nowMs) {
  if (!lastSeenMs)
    return 'No answer from the device.';
  return 'No answer from the device (last seen ' + Math.max(0, Math.round((nowMs - lastSeenMs) / 1000)) +
         ' s ago).';
}

/*
 * Reboot detection while waiting for the board. A probe is {ok, uptime} from /device/diag. The board is back when a
 * probe succeeds after at least one failed probe (the outage was seen), or when the uptime is lower than the uptime
 * seen before the operation (it rebooted between two probes). Without an outage and without a baseline nothing is
 * accepted: the caller gives up after its time limit. `state` is {outage, baseline}; it is updated.
 */
function bootObserved(state, probe) {
  if (!probe.ok) {
    state.outage = true;
    return false;
  }
  if (state.outage)
    return true;
  if (state.baseline === null || state.baseline === undefined) {
    state.baseline = probe.uptime;
    return false;
  }
  return probe.uptime < state.baseline;
}

/** Poll only while the tab is shown, the section is on screen and the last answer has arrived. */
function shouldPoll(s) { return !s.hidden && s.sectionVisible && !s.inFlight && !s.paused; }

function coreLabel(core) { return String(core); }

function pct(value) { return Number(value).toFixed(1) + '%'; }

/** {"1s","10s","60s"} -> "a% / b% / c%". */
function loadText(load) {
  return [ pct(load['1s']), pct(load['10s']), pct(load['60s']) ].join(' / ');
}

function bytesText(bytes) {
  if (bytes >= 1048576)
    return (bytes / 1048576).toFixed(1) + ' MB';
  return (bytes / 1024).toFixed(1) + ' KB';
}

return {
  captureUrl : captureUrl,
  radioFirmwareUrl : radioFirmwareUrl,
  checkUpdateUrl : checkUpdateUrl,
  errorMessage : errorMessage,
  faultHint : faultHint,
  jumperPresent : jumperPresent,
  rcpInstalledText : rcpInstalledText,
  formatRemaining : formatRemaining,
  networkConfig : networkConfig,
  shouldPoll : shouldPoll,
  ipv4Valid : ipv4Valid,
  maskValid : maskValid,
  checkUploadSize : checkUploadSize,
  nextDelay : nextDelay,
  lastSeenText : lastSeenText,
  bootObserved : bootObserved,
  RCP_MAX_BYTES : RCP_MAX_BYTES,
  BUNDLE_MAX_BYTES : BUNDLE_MAX_BYTES,
  coreLabel : coreLabel,
  loadText : loadText,
  bytesText : bytesText
};
})();

if (typeof module !== 'undefined' && module.exports)
  module.exports = DeviceLogic;

/*
 * The page. `env` carries everything it touches (document, fetch, timers, clock, dialogs), so the tests can run it
 * against stubs. Returns the entry points the tests drive.
 */
function DevicePage(env) {
var L = DeviceLogic;
var doc = env.document;
var lastStatus = null;
var lastSeen = 0;
var lastUptime = null;
var trialEnd = 0;
var busy = false;
var waiting = false;
var paused = false;
var pollers = [];
var netFilled = false;
var NET_FIELDS = [ 'vlan', 'dhcp', 'addr', 'mask', 'gw', 'dns' ];
var ACTION_IDS = [
  'capStart', 'capStop', 'otaUpload', 'otaFetch', 'rcpUpload', 'rcpFetch', 'recoverBtn', 'logBtn', 'rebootBtn',
  'netApply', 'confirmBtn'
];
var WAIT_STEP_MS = 2000;
var WAIT_MAX_MS = 180000;
var GIVE_UP_TEXT = 'The board did not answer within 3 minutes. After a network change its address or VLAN may have ' +
                   'changed: open the new address. Unless the new configuration is confirmed within 5 minutes ' +
                   'from the management interface, the board reverts to the previous one.';

function $(id) { return doc.getElementById(id); }

function el(tag, text, cls) {
  var e = doc.createElement(tag);
  if (text !== undefined && text !== null)
    e.textContent = String(text);
  if (cls)
    e.className = cls;
  return e;
}

function clear(node) {
  while (node.firstChild)
    node.removeChild(node.firstChild);
}

function setText(id, text) { $(id).textContent = text; }

function show(id, visible) { $(id).classList.toggle('hidden', !visible); }

function note(id, text, isError) {
  var e = $(id);
  e.textContent = text;
  e.style.color = isError ? 'var(--danger)' : '';
}

/* ---- busy: one long operation at a time ---- */

function setBusy(on) {
  busy = on;
  ACTION_IDS.forEach(function(id) {
    var e = $(id);
    e.disabled = on;
    e.setAttribute('aria-disabled', on ? 'true' : 'false');
    e.classList.toggle('disabled', on);
  });
  setPaused(on);
}

/* Runs `fn` (resolves to a result, {handoff: true} when the wait for the reboot took over) if nothing else runs. */
function runOp(fn) {
  if (busy)
    return Promise.resolve(null);
  setBusy(true);
  return fn().then(function(r) {
    if (!(r && r.handoff))
      setBusy(false);
    return r;
  }, function(e) {
    setBusy(false);
    throw e;
  });
}

function setPaused(on) {
  paused = on;
  if (!on)
    pollers.forEach(function(p) { p.wake(); });
}

/* ---- requests ---- */

/*
 * Resolves to {status, body, text}; rejects with an Error whose message is shown as is. For an upload, a dropped
 * connection most likely means the device refused it (it closes the connection after an early error reply).
 */
function request(method, url, opts) {
  opts = opts || {};
  var ctl = new env.AbortController();
  var timer = opts.timeoutMs ? env.setTimeout(function() { ctl.abort(); }, opts.timeoutMs) : null;
  var lost = opts.upload ? 'The board closed the connection (likely rejected the upload: check size and busy state).'
                         : 'No answer from the device (the connection was closed or it is rebooting).';
  return env.fetch(url, {method : method, headers : opts.headers, body : opts.body, signal : ctl.signal})
      .then(function(r) {
        return r.text().then(function(text) {
          var body = null;
          try {
            body = JSON.parse(text);
          } catch (e) {
          }
          return {status : r.status, body : body, text : text};
        });
      })
      .catch(function() { throw new Error(lost); })
      .then(function(r) {
        env.clearTimeout(timer);
        return r;
      }, function(e) {
        env.clearTimeout(timer);
        throw e;
      });
}

function failure(r) { return L.errorMessage(r.status, r.body); }

/* ---- polling: one request at a time, backoff on failure, quiet while hidden, off screen or busy ---- */

function poller(task, period, sectionId) {
  var timer = null, inFlight = false, visible = true, failures = 0;
  function tick() {
    timer = null;
    if (!L.shouldPoll({hidden : doc.hidden, sectionVisible : visible, inFlight : inFlight, paused : paused}))
      return;
    inFlight = true;
    task().then(function(ok) { done(ok !== false); }, function() { done(false); });
  }
  function done(ok) {
    inFlight = false;
    failures = ok ? 0 : failures + 1;
    schedule(L.nextDelay(period, failures));
  }
  function schedule(ms) {
    if (timer === null)
      timer = env.setTimeout(tick, ms);
  }
  function wake() {
    if (timer !== null) {
      env.clearTimeout(timer);
      timer = null;
    }
    schedule(0);
  }
  if (env.IntersectionObserver) {
    new env.IntersectionObserver(function(entries) {
      visible = entries[entries.length - 1].isIntersecting;
      if (visible)
        wake();
    }).observe($(sectionId));
  }
  doc.addEventListener('visibilitychange', function() {
    if (!doc.hidden)
      wake();
  });
  var p = {wake : wake, failures : function() { return failures; }};
  pollers.push(p);
  schedule(0);
  return p;
}

/* ---- status ---- */

function renderStatus(s) {
  lastStatus = s;
  lastSeen = env.now();
  show('s-offline', false);
  setText('s-mode', s.mode);
  setText('s-fault', s.fault_reason || 'none');
  setText('s-app', s.app.version + ' (slot ' + s.app.slot + ')');
  setText('s-rcp', L.rcpInstalledText(s.rcp.installed));
  setText('s-compat', s.rcp.compatible ? 'yes' : 'no');
  var j = s.jumper;
  setText('s-jumper', j.present ? (j.recovery_failed ? 'fitted, last recovery failed' : 'fitted') : 'not fitted');
  setText('s-base', j.present ? ((j.base_installed || 'unknown') + ' (expected ' + j.base_expected + ')') : '-');
  setText('s-ipv4', s.eth.ipv4 || '-');
  setText('s-ipv6', s.eth.ipv6.length ? s.eth.ipv6.join(', ') : '-');
  setText('s-pd', s.pd.state + (s.pd.prefix ? ' ' + s.pd.prefix : ''));
  var sn = s.sniffer;
  setText('s-sniffer', sn.active ? ('capturing on channel ' + sn.channel + ': ' + sn.frames + ' frames, ' +
                                    sn.drops + ' dropped, ' + sn.elapsed_s + ' of ' + sn.duration_s + ' s')
                                 : (sn.ended_by_fault ? 'last capture ended by a radio fault' : 'idle'));
  show('jumperControls', L.jumperPresent(s));
  var hint = L.faultHint(s.fault_reason);
  setText('radioHint', hint.text);
  show('radioHint', hint.highlight);
  $('radioUpload').style.outline = hint.highlight ? '2px solid var(--primary)' : '';
}

function showOffline() {
  setText('s-offline', L.lastSeenText(lastSeen, env.now()));
  show('s-offline', true);
}

function refreshStatus() {
  return request('GET', '/device/status', {timeoutMs : 5000}).then(function(r) {
    if (r.status === 200 && r.body) {
      renderStatus(r.body);
      return true;
    }
    showOffline();
    return false;
  }).catch(function() {
    showOffline();
    return false;
  });
}

/* ---- diagnostics ---- */

function rows(container, pairs) {
  clear(container);
  pairs.forEach(function(p) {
    var row = el('div', null, 'info-row');
    row.appendChild(el('span', p[0], 'info-label'));
    row.appendChild(el('span', p[1], 'info-value'));
    container.appendChild(row);
  });
}

function heapText(h) {
  return L.bytesText(h.free) + ' free, minimum ' + L.bytesText(h.min_free) + ', largest block ' +
         L.bytesText(h.largest_free_block);
}

function lockText(t) {
  return t.acquisitions + ' acquisitions, longest hold ' + t.max_hold_us + ' us (' + (t.longest_holder || '-') +
         '), longest wait ' + t.max_wait_us + ' us';
}

function renderDiag(d) {
  lastUptime = d.uptime_s;
  setText('d-uptime', d.uptime_s + ' s');
  setText('d-reset', d.reset_reason);
  rows($('d-cpu'), d.cpu.map(function(c) { return [ 'Core ' + c.core, L.loadText(c.load_pct) ]; }));
  rows($('d-mem'), [ [ 'Internal', heapText(d.memory.internal) ], [ 'PSRAM', heapText(d.memory.psram) ] ]);
  rows($('d-lock'), [ [ 'Since boot', lockText(d.ot_lock) ], [ 'Last 60 s', lockText(d.ot_lock.last_60s) ] ]);
  var body = $('d-tasks');
  clear(body);
  d.tasks.forEach(function(t) {
    var tr = el('tr');
    [ t.name, L.coreLabel(t.core), t.priority, t.state, L.loadText(t.cpu_pct), t.stack_free_min_bytes + ' B' ]
        .forEach(function(v) { tr.appendChild(el('td', v)); });
    body.appendChild(tr);
  });
  setText('d-note', d.tasks_untracked ? d.tasks_untracked + ' more tasks are not listed.' : '');
}

function refreshDiag() {
  return request('GET', '/device/diag', {timeoutMs : 5000}).then(function(r) {
    if (r.status === 200 && r.body) {
      renderDiag(r.body);
      return true;
    }
    setText('d-note', failure(r));
    return false;
  }).catch(function(e) {
    setText('d-note', e.message);
    return false;
  });
}

/* ---- sniffer ---- */

function captureHref() { return L.captureUrl($('capChannel').value, $('capDuration').value); }

function onCaptureClick(ev) {
  var url = captureHref();
  if (busy) {
    ev.preventDefault();
    return;
  }
  if (!url) {
    ev.preventDefault();
    note('capMsg', 'Channel must be 11 to 26 and duration 1 to 3600 seconds.', true);
    return;
  }
  if (lastStatus && lastStatus.mode !== 'BR_RUNNING') {
    ev.preventDefault();
    note('capMsg', lastStatus.mode === 'SNIFFER' ? 'A capture is already running.'
                                                  : 'The border router is not running (' + lastStatus.mode + ').', true);
    return;
  }
  $('capStart').setAttribute('href', url);
  note('capMsg', 'Capture requested; the file is saved by the browser when it ends or is stopped.', false);
}

function stopCapture() {
  if (busy)
    return Promise.resolve(null);
  return request('DELETE', '/device/sniffer').then(function(r) {
    note('capMsg', r.status === 200 ? 'Capture stopped.' : failure(r), r.status !== 200);
    return refreshStatus();
  }, function(e) { note('capMsg', e.message, true); });
}

/* ---- uploads and installs ---- */

function sendFile(url, file, msgId, label) {
  if (!file) {
    note(msgId, 'Choose a file first.', true);
    return Promise.resolve(null);
  }
  var tooBig = L.checkUploadSize(label, file.size);
  if (tooBig) {
    note(msgId, tooBig, true);
    return Promise.resolve(null);
  }
  return runOp(function() {
    note(msgId, 'Uploading ' + file.name + ' (' + file.size + ' bytes)...', false);
    // A File body makes the browser send Content-Length, which the device requires.
    return request('POST', url, {headers : {'Content-Type' : 'application/octet-stream'}, body : file, upload : true})
        .then(function(r) { return finishInstall(r, msgId, label); }, function(e) {
          note(msgId, e.message, true);
          return null;
        });
  });
}

function sendUrl(url, source, msgId, label, jsonBody) {
  var problem = L.checkUpdateUrl(source);
  if (problem) {
    note(msgId, problem, true);
    return Promise.resolve(null);
  }
  return runOp(function() {
    note(msgId, 'The device is downloading ' + source + ' ...', false);
    var opts = jsonBody ? {headers : {'Content-Type' : 'application/json'}, body : JSON.stringify({url : source.trim()})}
                        : {};
    return request('POST', url, opts).then(function(r) { return finishInstall(r, msgId, label); }, function(e) {
      note(msgId, e.message, true);
      return null;
    });
  });
}

function finishInstall(r, msgId, label) {
  if (r.status !== 200) {
    note(msgId, failure(r), true);
    return null;
  }
  if (label === 'bundle') {
    note(msgId, 'Update installed. The board is rebooting; this page reloads when it is back.', false);
    waitForBoard(msgId);
    return {handoff : true};
  }
  var v = r.body && r.body.installed;
  note(msgId, 'RCP firmware installed' + (v ? ': ' + v : '') + '.', false);
  refreshStatus();
  return r;
}

/*
 * Waits for the board to reboot and come back (rule: L.bootObserved), probing /device/diag every 2 s for at most
 * 3 minutes, then reloads the page. One waiter at a time. Polling stays off meanwhile (busy).
 */
function waitForBoard(msgId) {
  if (waiting)
    return;
  waiting = true;
  var started = env.now();
  var state = {outage : false, baseline: lastUptime};
  function giveUp() {
    waiting = false;
    note(msgId, GIVE_UP_TEXT, true);
    setBusy(false);
  }
  function probe() {
    request('GET', '/device/diag', {timeoutMs : 3000}).then(function(r) {
      var ok = r.status === 200 && r.body && typeof r.body.uptime_s === 'number';
      return {ok : !!ok, uptime : ok ? r.body.uptime_s : 0};
    }, function() { return {ok : false, uptime : 0}; }).then(function(sample) {
      if (L.bootObserved(state, sample)) {
        waiting = false;
        env.reload();
      } else if (env.now() - started > WAIT_MAX_MS) {
        giveUp();
      } else {
        env.setTimeout(probe, WAIT_STEP_MS);
      }
    });
  }
  probe();
}

/* ---- radio recovery ---- */

function recover() {
  return runOp(function() {
    note('recoverMsg', 'Rewriting the C6 base image through the ROM loader (this takes a while)...', false);
    return request('POST', '/device/radio/recover').then(function(r) {
      note('recoverMsg', r.status === 200 ? (r.body && r.body.detail) || 'Done.' : failure(r), r.status !== 200);
      refreshStatus();
      return r;
    }, function(e) { note('recoverMsg', e.message, true); });
  });
}

function showLog() {
  return request('GET', '/device/radio/log').then(function(r) {
    if (r.status === 200)
      setText('c6Log', r.text || '(empty)');
    else
      note('recoverMsg', failure(r), true);
  }, function(e) { note('recoverMsg', e.message, true); });
}

/* ---- reboot ---- */

function reboot() {
  if (busy || !env.confirm('Reboot the board now?'))
    return Promise.resolve(null);
  return runOp(function() {
    return request('POST', '/device/reboot').then(function(r) {
      if (r.status !== 200) {
        note('rebootMsg', failure(r), true);
        return r;
      }
      note('rebootMsg', 'Rebooting; this page reloads when the board is back.', false);
      waitForBoard('rebootMsg');
      return {handoff : true};
    }, function(e) { note('rebootMsg', e.message, true); });
  });
}

/* ---- network ---- */

function buildNetForm() {
  doc.querySelectorAll('fieldset[data-iface]').forEach(function(fs) {
    var id = fs.getAttribute('data-iface');
    function field(name, label, type) {
      var wrap = el('div', null, 'form-group');
      var input = el('input', null, type === 'checkbox' ? '' : 'form-input');
      input.type = type;
      input.id = 'net-' + id + '-' + name;
      var lab = el('label', label, 'form-label');
      lab.setAttribute('for', input.id);
      if (type === 'checkbox') {
        wrap.appendChild(input);
        wrap.appendChild(doc.createTextNode(' '));
        wrap.appendChild(lab);
      } else {
        wrap.appendChild(lab);
        wrap.appendChild(input);
      }
      fs.appendChild(wrap);
      return input;
    }
    field('vlan', 'VLAN ID (0 = untagged)', 'number');
    field('dhcp', 'IPv4 from DHCP', 'checkbox').addEventListener('change', updateNetForm);
    field('addr', 'IPv4 address', 'text');
    field('mask', 'Netmask', 'text');
    field('gw', 'Gateway (optional)', 'text');
    field('dns', 'DNS server (optional)', 'text');
  });
}

function netInput(iface, name) { return $('net-' + iface + '-' + name); }

function updateNetForm() {
  var split = $('netMode').value === 'split';
  doc.querySelectorAll('fieldset[data-iface]').forEach(function(fs) {
    var id = fs.getAttribute('data-iface');
    fs.classList.toggle('hidden', split ? id === 'single' : id !== 'single');
    var dhcp = netInput(id, 'dhcp').checked;
    [ 'addr', 'mask', 'gw', 'dns' ].forEach(function(n) { netInput(id, n).disabled = dhcp; });
  });
}

function fillIface(id, cfg) {
  if (!cfg)
    return;
  netInput(id, 'vlan').value = cfg.vlan;
  netInput(id, 'dhcp').checked = !!cfg.ipv4.dhcp;
  [ 'addr', 'mask', 'gw', 'dns' ].forEach(function(n) { netInput(id, n).value = cfg.ipv4[n] || ''; });
}

function readIface(id) {
  var f = {};
  NET_FIELDS.forEach(function(n) {
    var input = netInput(id, n);
    f[n] = n === 'dhcp' ? input.checked : input.value;
  });
  return f;
}

function ifaceRow(i) {
  var tr = el('tr');
  [ i.name, i.vlan === 0 ? 'untagged' : i.vlan, i.link ? 'up' : 'down', i.ipv4 || '-',
    (i.ipv6 && i.ipv6.length) ? i.ipv6.join(', ') : '-' ].forEach(function(v) { tr.appendChild(el('td', v)); });
  return tr;
}

function renderNetwork(n, firstFill) {
  setText('n-mode', n.mode);
  setText('n-dropped', n.vlan_rx_dropped);
  setText('n-fallback', n.fallback ? 'The stored configuration could not be applied (' + (n.error || 'unknown reason') +
                                         '); the defaults are running.'
                                   : '');
  show('n-fallback', !!n.fallback);
  show('n-stale', false);
  var body = $('n-ifaces');
  clear(body);
  (n.interfaces || []).forEach(function(i) { body.appendChild(ifaceRow(i)); });
  var cfg = n.active || n;
  if (firstFill) {
    $('netMode').value = cfg.mode || n.mode;
    fillIface('single', cfg.single);
    fillIface('mgmt', cfg.mgmt);
    fillIface('thread_bb', cfg.thread_bb);
    updateNetForm();
  }
  if (n.trial) {
    // The server's remaining_s is the truth: every answer re-syncs the countdown.
    trialEnd = env.now() + n.trial.remaining_s * 1000;
    show('trialBanner', true);
    tickTrial();
  } else {
    trialEnd = 0;
    show('trialBanner', false);
  }
}

function refreshNetwork() {
  return request('GET', '/device/network', {timeoutMs : 5000}).then(function(r) {
    if (r.status === 200 && r.body) {
      renderNetwork(r.body, !netFilled);
      netFilled = true;
      return true;
    }
    setText('n-stale', 'The network state could not be read: ' + failure(r));
    show('n-stale', true);
    return false;
  }).catch(function() {
    setText('n-stale', 'The network state could not be read: no answer from the device.');
    show('n-stale', true);
    return false;
  });
}

function tickTrial() {
  if (!trialEnd)
    return;
  var left = Math.max(0, Math.round((trialEnd - env.now()) / 1000));
  setText('trialRemaining', L.formatRemaining(left));
  if (left === 0) {
    trialEnd = 0;
    env.setTimeout(refreshNetwork, 3000);
  }
}

function tick() {
  tickTrial();
  if (!$('s-offline').classList.contains('hidden'))
    showOffline();
}

function applyNetwork() {
  if (busy)
    return Promise.resolve(null);
  var form = {mode : $('netMode').value};
  [ 'single', 'mgmt', 'thread_bb' ].forEach(function(id) { form[id] = readIface(id); });
  var cfg;
  try {
    cfg = L.networkConfig(form);
  } catch (e) {
    note('netMsg', e.message, true);
    return Promise.resolve(null);
  }
  if (!env.confirm('Applying reboots the board into the new network configuration. If it is not confirmed ' +
                   'from the management interface within 5 minutes, the board reverts. Continue?'))
    return Promise.resolve(null);
  return runOp(function() {
    return request('PUT', '/device/network', {headers : {'Content-Type' : 'application/json'}, body : JSON.stringify(cfg)})
        .then(function(r) {
          if (r.status !== 200) {
            note('netMsg', failure(r), true);
            return r;
          }
          note('netMsg', 'Stored. The board is rebooting into the trial configuration; open the page at its new ' +
                         'address and press Confirm within 5 minutes.', false);
          waitForBoard('netMsg');
          return {handoff : true};
        }, function(e) { note('netMsg', e.message, true); });
  });
}

function confirmTrial() {
  return runOp(function() {
    return request('POST', '/device/network/confirm').then(function(r) {
      note('confirmMsg', r.status === 200 ? 'Configuration confirmed.' : failure(r), r.status !== 200);
      return refreshNetwork();
    }, function(e) { note('confirmMsg', e.message, true); });
  });
}

/* ---- wiring ---- */

function init() {
  buildNetForm();
  $('capStart').addEventListener('click', onCaptureClick);
  $('capStop').addEventListener('click', stopCapture);
  $('otaUpload').addEventListener('click', function() { sendFile('/device/ota', $('otaFile').files[0], 'otaMsg', 'bundle'); });
  $('otaFetch').addEventListener('click', function() { sendUrl('/device/ota/url', $('otaUrl').value, 'otaMsg', 'bundle', true); });
  $('rcpUpload').addEventListener('click', function() {
    sendFile(L.radioFirmwareUrl(''), $('rcpFile').files[0], 'rcpMsg', 'rcp');
  });
  $('rcpFetch').addEventListener('click', function() {
    var u = $('rcpUrl').value.trim();
    sendUrl(L.radioFirmwareUrl(u), u, 'rcpMsg', 'rcp', false);
  });
  $('recoverBtn').addEventListener('click', recover);
  $('logBtn').addEventListener('click', showLog);
  $('rebootBtn').addEventListener('click', reboot);
  $('netMode').addEventListener('change', updateNetForm);
  $('netApply').addEventListener('click', applyNetwork);
  $('confirmBtn').addEventListener('click', confirmTrial);
  env.setInterval(tick, 1000);
  poller(refreshStatus, 3000, 'statusSection');
  poller(refreshNetwork, 10000, 'networkSection');
  poller(refreshDiag, 2000, 'diagSection');
}

return {
  init : init,
  poller : poller,
  renderStatus : renderStatus,
  renderNetwork : renderNetwork,
  sendFile : sendFile,
  sendUrl : sendUrl,
  stopCapture : stopCapture,
  applyNetwork : applyNetwork,
  confirmTrial : confirmTrial,
  reboot : reboot,
  recover : recover,
  refreshStatus : refreshStatus,
  refreshNetwork : refreshNetwork,
  setBusy : setBusy,
  runOp : runOp,
  isBusy : function() { return busy; },
  isWaiting : function() { return waiting; },
  waitForBoard : waitForBoard,
  tick : tick,
  buildNetForm : buildNetForm
};
}

if (typeof module !== 'undefined' && module.exports)
  module.exports.DevicePage = DevicePage;

if (typeof document !== 'undefined') {
  DevicePage({
    document : document,
    fetch : function(url, opts) { return fetch(url, opts); },
    setTimeout : function(f, ms) { return setTimeout(f, ms); },
    clearTimeout : function(t) { clearTimeout(t); },
    setInterval : function(f, ms) { return setInterval(f, ms); },
    now : function() { return Date.now(); },
    confirm : function(t) { return confirm(t); },
    reload : function() { location.reload(); },
    AbortController : AbortController,
    IntersectionObserver : typeof IntersectionObserver !== 'undefined' ? IntersectionObserver : null
  }).init();
}
