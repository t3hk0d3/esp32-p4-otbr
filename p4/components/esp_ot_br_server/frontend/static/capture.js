/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP Thread Border Router - Packet capture page (/device/sniffer.pcap, DELETE /device/sniffer, /device/status)
 *
 * CaptureLogic is pure and runs under node too (tools/tests/test_capture_page.py). CapturePage is the page.
 */
'use strict';

var CaptureLogic = (function() {
var C = typeof DeviceCommon !== 'undefined' ? DeviceCommon : require('./device_common.js');
var CHANNEL_MIN = 11, CHANNEL_MAX = 26, DURATION_MAX = 3600;

/**
 * URL of the capture stream, or null when the channel or the duration is out of range. An empty channel is left out,
 * so the board captures on its Thread network's channel.
 */
function captureUrl(channel, duration) {
  var d = C.wholeNumber(duration, 1, DURATION_MAX);
  if (d === null)
    return null;
  if (String(channel).trim() === '')
    return '/device/sniffer.pcap?duration=' + d;
  var c = C.wholeNumber(channel, CHANNEL_MIN, CHANNEL_MAX);
  if (c === null)
    return null;
  return '/device/sniffer.pcap?channel=' + c + '&duration=' + d;
}

function liveCommand(host, url) { return "curl -N 'http://" + host + url + "' | wireshark -k -i -"; }

/** What the running card shows for an active sniffer status. */
function activeView(sn) {
  var total = Math.max(1, sn.duration_s);
  return {
    title : 'Capturing on channel ' + sn.channel,
    left : C.formatRemaining(sn.duration_s - sn.elapsed_s),
    frames : String(sn.frames),
    drops : String(sn.drops),
    pct : Math.min(100, Math.round(sn.elapsed_s * 100 / total))
  };
}

return {captureUrl : captureUrl, liveCommand : liveCommand, activeView : activeView, LENGTHS : [ 30, 60, 600, 3600 ]};
})();

function CapturePage(env) {
var C = typeof DeviceCommon !== 'undefined' ? DeviceCommon : require('./device_common.js');
var L = CaptureLogic;
var kit = C.DeviceKit(env, {actionIds : [ 'capStart', 'capStop' ]});
var $ = kit.$, setText = kit.setText, show = kit.show, note = kit.note;
var duration = 60;
var lastStatus = null;
var statusPoll = null;
var startedAt = 0;   // time of the last Start click until the board is seen capturing (or not)
var START_CHECK_MS = 5000;

function captureHref() { return L.captureUrl($('capChannel').value, duration); }

/* The link and the command follow the fields; an invalid channel keeps the command of the last valid one. */
function updateLink() {
  L.LENGTHS.forEach(function(s) { $('len-' + s).setAttribute('aria-pressed', s === duration ? 'true' : 'false'); });
  var url = captureHref();
  if (!url) {
    setText('cap-cmd', 'Enter a channel from 11 to 26, or leave it empty, to get the command.');
    return;
  }
  $('capStart').setAttribute('href', url);
  setText('cap-cmd', L.liveCommand(env.location.host, url));
}

/*
 * The capture is a plain download, so the page never sees the board's answer to it. After Start it watches the
 * status instead: capturing means it started; still idle after a few seconds means the board refused it.
 */
function checkStart(sn) {
  if (!startedAt)
    return;
  if (sn.active) {
    startedAt = 0;
    note('capMsg', 'Capture running: your browser saves sniffer.pcap as the frames arrive.', false);
  } else if (env.now() - startedAt >= START_CHECK_MS) {
    startedAt = 0;
    note('capMsg', 'The board did not start the capture (it may be busy, or it has no Thread network to take the ' +
                       'channel from). The downloaded file holds its answer.', true);
  }
}

function render(s) {
  lastStatus = s;
  show('cap-stale', false);
  var sn = s.sniffer;
  show('cap-idle', !sn.active);
  show('cap-active', !!sn.active);
  show('cap-fault', !sn.active && !!sn.ended_by_fault);
  checkStart(sn);
  if (sn.active) {
    var v = L.activeView(sn);
    setText('cap-a-title', v.title);
    setText('cap-a-left', v.left);
    setText('cap-a-frames', v.frames);
    setText('cap-a-drops', v.drops);
    $('cap-a-fill').style.width = v.pct + '%';
  }
  updateLink();
}

function refreshStatus() {
  return kit.request('GET', '/device/status', {timeoutMs : 5000}).then(function(r) {
    if (r.status === 200 && r.body) {
      render(r.body);
      return true;
    }
    setText('cap-stale', 'The capture state could not be read: ' + kit.failure(r));
    show('cap-stale', true);
    return false;
  }, function(e) {
    setText('cap-stale', e.message);
    show('cap-stale', true);
    return false;
  });
}

function onStart(ev) {
  var url = captureHref();
  if (kit.isBusy()) {
    ev.preventDefault();
    return;
  }
  if (!url) {
    ev.preventDefault();
    note('capMsg', 'The channel must be empty or 11 to 26.', true);
    return;
  }
  if (lastStatus && lastStatus.mode !== 'BR_RUNNING') {
    ev.preventDefault();
    note('capMsg', lastStatus.mode === 'SNIFFER' ? 'A capture is already running.'
                                                  : 'The border router is not running (' + lastStatus.mode + ').', true);
    return;
  }
  $('capStart').setAttribute('href', url);
  note('capMsg', 'Starting the capture: your browser saves sniffer.pcap as the frames arrive.', false);
  startedAt = env.now();
  [ 1500, START_CHECK_MS + 500 ].forEach(function(ms) {
    env.setTimeout(function() {
      if (statusPoll)
        statusPoll.wake();
    }, ms);
  });
}

function stopCapture() {
  if (kit.isBusy())
    return Promise.resolve(null);
  return kit.request('DELETE', '/device/sniffer').then(function(r) {
    note('capMsg', r.status === 200 ? 'Capture stopped.' : kit.failure(r), r.status !== 200);
    return refreshStatus();
  }, function(e) { note('capMsg', e.message, true); });
}

function init() {
  L.LENGTHS.forEach(function(s) {
    $('len-' + s).addEventListener('click', function() {
      duration = s;
      updateLink();
    });
  });
  $('capChannel').addEventListener('input', updateLink);
  $('capStart').addEventListener('click', onStart);
  $('capStop').addEventListener('click', stopCapture);
  updateLink();
  kit.start();
  statusPoll = kit.poller(refreshStatus, 3000, 'captureSection');
}

return {init : init, refreshStatus : refreshStatus, stopCapture : stopCapture, kit : kit};
}

if (typeof module !== 'undefined' && module.exports) {
  module.exports = CaptureLogic;
  module.exports.Page = CapturePage;
}

if (typeof document !== 'undefined')
  CapturePage(DeviceCommon.DeviceEnv()).init();
