/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP Thread Border Router - shared code of the device pages (/device/* API)
 *
 * DeviceCommon is pure logic and runs under node too (tools/tests/test_device_common.py). DeviceKit is what every device
 * page needs: requests, polling, the busy lock, the wait for a reboot and the trial banner. Everything that comes from
 * the device is inserted with textContent, never as markup.
 */
'use strict';

var DeviceCommon = (function() {

function wholeNumber(value, min, max) {
  var text = String(value).trim();
  if (!/^\d+$/.test(text))
    return null;
  var n = Number(text);
  return (n >= min && n <= max) ? n : null;
}

/*
 * OpenThread's per-radio preference of a Thread neighbour, as /device/status (TREL peers) and /device/topology (this
 * board's links) give it: {ieee802154, trel}, each 0..255 or null when the neighbour does not use that radio.
 */
function radioPref(preference, radio) {
  return preference && typeof preference[radio] === 'number' ? preference[radio] : null;
}

/** TREL's preference is below 15.4's: the tunnel is losing to the radio (shown red). */
function trelLow(preference) {
  var trel = radioPref(preference, 'trel'), radio = radioPref(preference, 'ieee802154');
  return trel !== null && radio !== null && trel < radio;
}

/** [{text, low}] of "15.4 255 · TREL 55", low marking the TREL figure when trelLow; [] when neither radio is used. */
function preferenceParts(preference) {
  var radio = radioPref(preference, 'ieee802154'), trel = radioPref(preference, 'trel');
  var parts = [];
  if (radio !== null)
    parts.push({text : '15.4 ' + radio, low : false});
  if (radio !== null && trel !== null)
    parts.push({text : ' \u00b7 ', low : false});
  if (trel !== null) {
    parts.push({text : 'TREL ', low : false});
    parts.push({text : String(trel), low : trelLow(preference)});
  }
  return parts;
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

function formatRemaining(seconds) {
  var s = Math.max(0, Math.floor(seconds));
  var rest = s % 60;
  return Math.floor(s / 60) + ':' + (rest < 10 ? '0' : '') + rest;
}

/** Share of the trial window left, whole percent: the board reverts after NETCFG_TRIAL_TIMEOUT_S (300 s). */
var TRIAL_WINDOW_S = 300;
function trialLeftPct(seconds) {
  var s = Math.min(TRIAL_WINDOW_S, Math.max(0, seconds));
  return Math.round(s * 100 / TRIAL_WINDOW_S);
}

/** Uptime in words: "2 d 3 h", "7 h 35 min", "12 min", "40 s". */
function durationText(seconds) {
  var s = Math.max(0, Math.floor(seconds));
  var d = Math.floor(s / 86400), h = Math.floor(s / 3600) % 24, m = Math.floor(s / 60) % 60;
  if (d)
    return d + ' d ' + h + ' h';
  if (h)
    return h + ' h ' + m + ' min';
  if (m)
    return m + ' min';
  return s + ' s';
}

/*
 * Upload limits, the same as the device enforces (413 otherwise): an RCP image must fit the C6 OTA slot
 * (C6_OTA_SLOT_SIZE, common/c6_board/c6_board.h); a bundle is the 44-byte header plus at most the web partition (2 MB)
 * plus the app partition (8 MB), partitions.csv. tools/tests/test_firmware_page.py compares these with the sources.
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

/** "Matches this build" of a status rcp object: Yes/No, "unknown" when the probe failed, "-" without firmware. */
function compatText(rcp) {
  if (!rcp || rcp.installed === null || rcp.installed === undefined)
    return 'unknown';
  if (rcp.installed === '')
    return '-';
  return rcp.compatible ? 'Yes' : 'No';
}

function bytesText(bytes) {
  if (bytes >= 1048576)
    return (bytes / 1048576).toFixed(1) + ' MB';
  return (bytes / 1024).toFixed(1) + ' KB';
}

return {
  wholeNumber : wholeNumber,
  compatText : compatText,
  errorMessage : errorMessage,
  formatRemaining : formatRemaining,
  trialLeftPct : trialLeftPct,
  durationText : durationText,
  shouldPoll : shouldPoll,
  checkUploadSize : checkUploadSize,
  nextDelay : nextDelay,
  lastSeenText : lastSeenText,
  bootObserved : bootObserved,
  RCP_MAX_BYTES : RCP_MAX_BYTES,
  BUNDLE_MAX_BYTES : BUNDLE_MAX_BYTES,
  coreLabel : coreLabel,
  pct : pct,
  loadText : loadText,
  bytesText : bytesText,
  radioPref : radioPref,
  trelLow : trelLow,
  preferenceParts : preferenceParts
};
})();

/*
 * The shared page machinery. `env` carries everything it touches (document, fetch, timers, clock, dialogs), so the
 * tests can run it against stubs. `opts`:
 *   actionIds    ids of the buttons a long operation disables (the trial banner's confirmBtn is added)
 *   trial        false when the page renders the trial banner itself (renderTrial) instead of polling for it
 *   onBusy(on)   called after the busy state changed
 */
function DeviceKit(env, opts) {
var L = DeviceCommon;
var doc = env.document;
opts = opts || {};
var actionIds = (opts.actionIds || []).concat([ 'confirmBtn' ]);
var busy = false;
var waiting = false;
var paused = false;
var pollers = [];
var lastUptime = null;
var trialEnd = 0;
var WAIT_STEP_MS = 2000;
var WAIT_MAX_MS = 180000;
var GIVE_UP_TEXT = 'The board did not answer within 3 minutes. After a network change its address or VLAN may have ' +
                   'changed: open the new address. Unless the new settings are kept within 5 minutes from the ' +
                   'management interface, the board goes back to the previous ones.';

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
  actionIds.forEach(function(id) {
    var e = $(id);
    e.disabled = on;
    e.setAttribute('aria-disabled', on ? 'true' : 'false');
    e.classList.toggle('disabled', on);
  });
  setPaused(on);
  if (opts.onBusy)
    opts.onBusy(on);
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
function request(method, url, ropts) {
  ropts = ropts || {};
  var ctl = new env.AbortController();
  var timer = ropts.timeoutMs ? env.setTimeout(function() { ctl.abort(); }, ropts.timeoutMs) : null;
  var lost = ropts.upload ? 'The board closed the connection (likely rejected the upload: check size and busy state).'
                          : 'No answer from the device (the connection was closed or it is rebooting).';
  return env.fetch(url, {method : method, headers : ropts.headers, body : ropts.body, signal : ctl.signal})
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

/*
 * Polling: one request at a time, backoff on failure, quiet while hidden, off screen (when `sectionId` names an
 * element to watch) or busy. `task` resolves to false on failure.
 */
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
  if (env.IntersectionObserver && sectionId) {
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

/*
 * Waits for the board to reboot and come back (rule: L.bootObserved), probing /device/diag every 2 s for at most
 * 3 minutes, then reloads the page. One waiter at a time. Polling stays off meanwhile (busy). `giveUpText` replaces
 * the generic message shown when the board does not come back here (for example, it moved to another address).
 */
function waitForBoard(msgId, giveUpText) {
  if (waiting)
    return;
  waiting = true;
  var started = env.now();
  var state = {outage : false, baseline : lastUptime};
  function giveUp() {
    waiting = false;
    note(msgId, giveUpText || GIVE_UP_TEXT, true);
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

/* ---- trial banner: the network settings run on trial until they are kept ---- */

function renderTrial(n) {
  if (n && n.trial) {
    // The server's remaining_s is the truth: every answer re-syncs the countdown.
    trialEnd = env.now() + n.trial.remaining_s * 1000;
    show('trialBanner', true);
    tickTrial();
  } else {
    trialEnd = 0;
    show('trialBanner', false);
  }
}

function refreshTrial() {
  return request('GET', '/device/network', {timeoutMs : 5000}).then(function(r) {
    if (r.status !== 200 || !r.body)
      return false;
    renderTrial(r.body);
    return true;
  }, function() { return false; });
}

function tickTrial() {
  if (!trialEnd)
    return;
  var left = Math.max(0, Math.round((trialEnd - env.now()) / 1000));
  setText('trialRemaining', L.formatRemaining(left));
  // A page cached from before the bar has no #trialFill; the countdown must still run out.
  var fill = $('trialFill');
  if (fill)
    fill.style.width = L.trialLeftPct(left) + '%';
  if (left === 0) {
    // The board reverts and reboots now: the banner and its button are stale until the next answer.
    trialEnd = 0;
    show('trialBanner', false);
    env.setTimeout(opts.trial === false ? opts.refreshNetwork : refreshTrial, 3000);
  }
}

function confirmTrial() {
  return runOp(function() {
    return request('POST', '/device/network/confirm').then(function(r) {
      note('confirmMsg', r.status === 200 ? 'New settings kept.' : failure(r), r.status !== 200);
      return opts.trial === false ? opts.refreshNetwork() : refreshTrial();
    }, function(e) { note('confirmMsg', e.message, true); });
  });
}

function start() {
  $('confirmBtn').addEventListener('click', confirmTrial);
  env.setInterval(tickTrial, 1000);
  if (opts.trial !== false)
    poller(refreshTrial, 10000, null);
}

return {
  $ : $,
  el : el,
  clear : clear,
  setText : setText,
  show : show,
  note : note,
  request : request,
  failure : failure,
  poller : poller,
  setBusy : setBusy,
  runOp : runOp,
  waitForBoard : waitForBoard,
  renderTrial : renderTrial,
  refreshTrial : refreshTrial,
  tickTrial : tickTrial,
  confirmTrial : confirmTrial,
  noteUptime : function(u) { lastUptime = u; },
  isBusy : function() { return busy; },
  isWaiting : function() { return waiting; },
  start : start
};
}

/* The browser's environment for DeviceKit and the pages; the tests pass their own. */
function DeviceEnv() {
  return {
    document : document,
    location : location,
    fetch : function(url, opts) { return fetch(url, opts); },
    setTimeout : function(f, ms) { return setTimeout(f, ms); },
    clearTimeout : function(t) { clearTimeout(t); },
    setInterval : function(f, ms) { return setInterval(f, ms); },
    now : function() { return Date.now(); },
    confirm : function(t) { return confirm(t); },
    storage : (function() {
      try {
        return window.sessionStorage;
      } catch (e) {
        return null;
      }
    })(),
    reload : function() { location.reload(); },
    navigate : function(url) { location.assign(url); },
    AbortController : AbortController,
    IntersectionObserver : typeof IntersectionObserver !== 'undefined' ? IntersectionObserver : null
  };
}

DeviceCommon.DeviceKit = DeviceKit;
DeviceCommon.DeviceEnv = DeviceEnv;

if (typeof module !== 'undefined' && module.exports)
  module.exports = DeviceCommon;
