/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP Thread Border Router - the Dashboard's Zigbee section (GET /device/zigbee; 409 when no radio is a Zigbee
 * coordinator)
 *
 * ZigbeeLogic is pure and runs under node too (tools/tests/test_zigbee_page.py). ZigbeeSection renders the section
 * inside the Dashboard (index.html); StatusPage (status.js) owns its poll and shows it only while a radio runs Zigbee.
 */
'use strict';

var ZigbeeLogic = (function() {

var COPY_RESET_MS = 2000;

/** Poll period: quick while the coordinator starts, calm otherwise. */
function pollMs(state) { return state === 'starting' ? 2000 : 5000; }

/** The radio behind a mux channel id: "c6" is the on-board one, "usb-..." a stick. */
function radioName(channel) {
  if (channel === 'c6')
    return 'on-board C6';
  return channel ? 'USB radio ' + channel : '-';
}

/** Epoch seconds as a local date and time; 0 means the clock was not set when it happened. */
function epochText(t) {
  if (t === null || t === undefined)
    return '-';
  if (!t)
    return 'clock not set';
  return new Date(t * 1000).toLocaleString();
}

function zhaPath(host) { return 'ws://' + host + '/zigbee'; }

function hex16(n) {
  var h = Number(n).toString(16).toUpperCase();
  while (h.length < 4)
    h = '0' + h;
  return '0x' + h;
}

/** "d885acfffef9cb4c" as "d8:85:ac:ff:fe:f9:cb:4c". */
function extendedText(hex) { return String(hex).replace(/(..)(?!$)/g, '$1:'); }

/** 48913 as "48 913". */
function count(n) { return String(n).replace(/\B(?=(\d{3})+(?!\d))/g, ' '); }

/** The banner: {tone: ok|info|fault|idle, title, text}. */
function heroFor(z) {
  var radio = radioName(z.channel);
  if (z.state === 'starting')
    return {tone : 'idle', title : 'Starting', text : 'The coordinator is starting on the ' + radio + '.'};
  if (z.state === 'idle')
    return {tone : 'info', title : 'Coordinator not running', text : z.reason || 'The coordinator is not running.'};
  if (z.state === 'incompatible')
    return {tone : 'info', title : 'Radio not compatible', text : z.reason || 'The radio firmware does not fit this build.'};
  if (z.state === 'no_radio')
    return {
      tone : 'fault',
      title : 'No radio',
      text : 'The ' + radio + ' is not detected. The coordinator keeps the network in memory and ZHA stays connected, ' +
             'but nothing is sent or received. Once the radio is back it carries on: no restart, no re-pairing.'
    };
  if (z.client)
    return {
      tone : 'ok',
      title : 'Coordinator running',
      text : (z.radio ? 'Channel ' + z.radio.channel + ' on the ' : 'Running on the ') + radio + '. ZHA is connected.'
    };
  return {
    tone : 'info',
    title : 'Waiting for Home Assistant',
    text : 'The coordinator runs on the ' + radio + ' and keeps the network going (routing, sleepy devices), but ' +
           'nothing controls it until ZHA connects.'
  };
}

return {
  pollMs : pollMs,
  radioName : radioName,
  epochText : epochText,
  zhaPath : zhaPath,
  hex16 : hex16,
  extendedText : extendedText,
  count : count,
  heroFor : heroFor,
  COPY_RESET_MS : COPY_RESET_MS
};
})();

/* env: the page environment (location for the ZHA path, navigator for the clipboard); kit: the Dashboard's DeviceKit. */
function ZigbeeSection(env, kit) {
var C = typeof DeviceCommon !== 'undefined' ? DeviceCommon : require('./device_common.js');
var L = ZigbeeLogic;
var $ = kit.$, el = kit.el, setText = kit.setText, show = kit.show;
var lastState = null;

function facts(id, rows) {
  var dl = $(id);
  kit.clear(dl);
  rows.forEach(function(r) {
    dl.appendChild(el('dt', r[0]));
    dl.appendChild(el('dd', r[1], r[2] || ''));
  });
}

function dash(v, fn) { return v === null || v === undefined ? '-' : fn ? fn(v) : String(v); }

function render(z) {
  lastState = z.state;
  var h = L.heroFor(z);
  show('zigbee-off', false);
  show('zigbee-on', true);
  $('zg-hero').className = 'card hero hero-' + h.tone;
  setText('zg-title', h.title);
  setText('zg-text', h.text);
  setText('zg-radio', L.radioName(z.channel));
  setText('zg-since', z.state === 'running' || z.state === 'no_radio' ? L.epochText(z.started) : '-');
  setText('zg-resets', L.count(z.counters.rcp_resets));
  setText('zg-path', L.zhaPath(env.location.host));

  var chip = $('zg-client-chip');
  chip.textContent = z.client ? 'connected' : 'not connected';
  chip.className = z.client ? 'chip chip-ok' : 'chip';
  if (z.client)
    facts('zg-client', [ [ 'Address', z.client.address, 'zg-mono' ], [ 'Connected since', L.epochText(z.client.since) ],
                         [ 'Frames in / out', L.count(z.counters.frames_in) + ' / ' + L.count(z.counters.frames_out) ] ]);
  else if (z.last_client)
    facts('zg-client', [ [ 'Last client', z.last_client.address, 'zg-mono' ], [ 'Left', L.epochText(z.last_client.left) ] ]);
  else
    facts('zg-client', [ [ 'Last client', 'none since start' ] ]);

  var r = z.radio;
  facts('zg-net', [ [ 'Channel', r ? String(r.channel) : '-' ], [ 'PAN ID', r ? L.hex16(r.pan_id) : '-', 'zg-mono' ],
                    [ 'Coordinator', r ? L.extendedText(r.extended_address) : '-', 'zg-mono' ],
                    [ 'Short address', r ? L.hex16(r.short_address) : '-', 'zg-mono' ],
                    [ 'TX power', r ? r.tx_power_dbm + ' dBm' : '-' ] ]);

  var c = z.counters;
  facts('zg-traffic', [ [ 'Received', L.count(c.rx) ], [ 'Dropped', L.count(c.rx_dropped) ], [ 'Sent', L.count(c.tx) ],
                        [ 'Not acknowledged', L.count(c.tx_no_ack) ], [ 'Channel busy', L.count(c.tx_cca) ],
                        [ 'Failed', L.count(c.tx_failed) ], [ 'Energy scans', L.count(c.energy_scans) ] ]);

  setText('zg-heap', C.bytesText(z.heap.current));
  setText('zg-heap-use', 'in use by the coordinator, peak ' + C.bytesText(z.heap.peak));
  facts('zg-fw', [ [ 'Radio', z.firmware || '-', 'zg-mono' ] ]);
  setText('zg-note', '');
}

/* No radio runs the coordinator: the placeholder instead of the content. */
function renderNone() {
  lastState = null;
  show('zigbee-off', true);
  show('zigbee-on', false);
  setText('zg-note', '');
}

/* Zigbee stopped: nothing from the last coordinator stays on screen. */
function clear() {
  renderNone();
  [ 'zg-title', 'zg-text', 'zg-radio', 'zg-since', 'zg-resets', 'zg-heap', 'zg-heap-use' ].forEach(function(id) {
    setText(id, id === 'zg-text' || id === 'zg-heap-use' ? '' : '-');
  });
  [ 'zg-client', 'zg-net', 'zg-traffic', 'zg-fw' ].forEach(function(id) { kit.clear($(id)); });
}

function refresh() {
  return kit.request('GET', '/device/zigbee', {timeoutMs : 5000}).then(function(r) {
    if (r.status === 200 && r.body) {
      render(r.body);
      return true;
    }
    if (r.status === 409) {
      renderNone();
      return true;
    }
    setText('zg-note', kit.failure(r));
    return false;
  }, function(e) {
    setText('zg-note', e.message);
    return false;
  });
}

function copyPath() {
  var b = $('zg-copy');
  C.copyText(env, $('zg-path').textContent, env.document.body)
      .then(function() { b.textContent = 'Copied'; }, function() { b.textContent = 'Copy failed'; })
      .then(function() { env.setTimeout(function() { b.textContent = 'Copy'; }, L.COPY_RESET_MS); });
}

function bindCopy() { $('zg-copy').addEventListener('click', copyPath); }

/* The poll period for StatusPage's poller: quick while the coordinator starts. */
function pollMs() { return L.pollMs(lastState); }

return {render : render, renderNone : renderNone, clear : clear, refresh : refresh, bindCopy : bindCopy, pollMs : pollMs};
}

if (typeof module !== 'undefined' && module.exports) {
  module.exports = ZigbeeLogic;
  module.exports.Section = ZigbeeSection;
}
