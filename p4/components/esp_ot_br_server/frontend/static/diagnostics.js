/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP Thread Border Router - Diagnostics page (GET /device/diag)
 *
 * DiagnosticsLogic is pure and runs under node too (tools/tests/test_diagnostics_page.py). DiagnosticsPage is the page.
 */
'use strict';

var DiagnosticsLogic = (function() {

/** Copy of `tasks`, busiest first by 10 s load; equal loads by name. */
function sortedTasks(tasks) {
  return tasks.slice().sort(function(a, b) {
    var d = b.cpu_pct['10s'] - a.cpu_pct['10s'];
    return d !== 0 ? d : (a.name < b.name ? -1 : a.name > b.name ? 1 : 0);
  });
}

function stackLow(bytes) { return bytes < 1024; }

/** Microseconds in the largest unit that fits: "500 µs", "1.8 ms", "2.5 s". */
function usText(us) {
  if (us < 1000)
    return us + ' µs';
  if (us < 1000000)
    return (us / 1000).toFixed(1) + ' ms';
  return (us / 1000000).toFixed(1) + ' s';
}

return {sortedTasks : sortedTasks, stackLow : stackLow, usText : usText};
})();

function DiagnosticsPage(env) {
var C = typeof DeviceCommon !== 'undefined' ? DeviceCommon : require('./device_common.js');
var L = DiagnosticsLogic;
var kit = C.DeviceKit(env, {});
var $ = kit.$, el = kit.el, setText = kit.setText;

function meter(label, value, widthPct) {
  var m = el('div', null, 'meter');
  var head = el('div', null, 'meter-head');
  head.appendChild(el('span', label));
  head.appendChild(el('span', value));
  m.appendChild(head);
  if (widthPct !== null) {
    var bar = el('span', null, 'bar');
    var fill = el('span', null, 'bar-fill');
    fill.style.width = Math.min(100, Math.max(0, widthPct)) + '%';
    bar.appendChild(fill);
    m.appendChild(bar);
  }
  return m;
}

function row(cells, classes) {
  var tr = el('tr');
  cells.forEach(function(v, i) { tr.appendChild(el(i === 0 ? 'th' : 'td', v, classes[i] || '')); });
  tr.children[0].setAttribute('scope', 'row');
  return tr;
}

function heapLine(name, h) {
  var m = el('div', null, 'meter');
  var head = el('div', null, 'meter-head');
  head.appendChild(el('span', name));
  head.appendChild(el('span', C.bytesText(h.free) + ' free'));
  m.appendChild(head);
  m.appendChild(el('div', 'Lowest since boot ' + C.bytesText(h.min_free) + ', largest free block ' +
                              C.bytesText(h.largest_free_block), 'form-hint'));
  return m;
}

function lockRow(period, t) {
  return row([ period, String(t.acquisitions), L.usText(t.max_hold_us), t.longest_holder || '-', L.usText(t.max_wait_us) ],
             [ '', 'num', 'num', '', 'num' ]);
}

function render(d) {
  kit.noteUptime(d.uptime_s);
  setText('d-uptime', C.durationText(d.uptime_s));
  setText('d-reset', d.reset_reason);
  var cpu = $('d-cpu');
  kit.clear(cpu);
  d.cpu.forEach(function(c) { cpu.appendChild(meter('Core ' + c.core, C.loadText(c.load_pct), c.load_pct['10s'])); });
  var mem = $('d-mem');
  kit.clear(mem);
  mem.appendChild(heapLine('Internal RAM', d.memory.internal));
  mem.appendChild(heapLine('PSRAM', d.memory.psram));
  var lock = $('d-lock');
  kit.clear(lock);
  lock.appendChild(lockRow('Since boot', d.ot_lock));
  lock.appendChild(lockRow('Last 60 s', d.ot_lock.last_60s));
  var body = $('d-tasks');
  kit.clear(body);
  L.sortedTasks(d.tasks).forEach(function(t) {
    body.appendChild(row([ t.name, t.state, C.coreLabel(t.core), String(t.priority), C.loadText(t.cpu_pct),
                           t.stack_free_min_bytes + ' B' ],
                         [ '', '', 'num', 'num', 'num', L.stackLow(t.stack_free_min_bytes) ? 'num stack-low' : 'num' ]));
  });
  setText('d-note', d.tasks_untracked ? d.tasks_untracked + ' more tasks are not listed.' : '');
}

function refresh() {
  return kit.request('GET', '/device/diag', {timeoutMs : 5000}).then(function(r) {
    if (r.status === 200 && r.body) {
      render(r.body);
      return true;
    }
    setText('d-note', kit.failure(r));
    return false;
  }, function(e) {
    setText('d-note', e.message);
    return false;
  });
}

function init() {
  kit.start();
  kit.poller(refresh, 2000, 'diagSection');
}

return {init : init, refresh : refresh, render : render, kit : kit};
}

if (typeof module !== 'undefined' && module.exports) {
  module.exports = DiagnosticsLogic;
  module.exports.Page = DiagnosticsPage;
}

if (typeof document !== 'undefined')
  DiagnosticsPage(DeviceCommon.DeviceEnv()).init();
