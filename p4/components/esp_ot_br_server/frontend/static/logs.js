/*
 * ESP Thread Border Router - Logs page (GET/PUT /device/logging, WebSocket /device/logging/ws)
 *
 * LogsLogic is pure and runs under node too (tools/tests/test_logs_page.py). LogsPage is the page: where the log goes
 * (channels, components) and a live view over one WebSocket that the user opens with Connect. Changes apply at once,
 * as a PUT carrying only the changed field.
 */
'use strict';

var LogsLogic = (function() {
var LEVELS = [ 'off', 'error', 'warning', 'info', 'debug', 'verbose' ];
var NAMES = {off : 'Off', error : 'Error', warning : 'Warning', info : 'Info', debug : 'Debug', verbose : 'Verbose'};
var LETTERS = {E : 'error', W : 'warning', I : 'info', D : 'debug', V : 'verbose'};
var CHANNELS = [ [ 'uart', 'UART' ], [ 'jtag', 'USB-JTAG' ], [ 'syslog', 'Syslog' ] ];
var DEFAULT_PORT = 514;
var TAG_MAX = 23; // the board keeps tags below 24 bytes
var LINE = /^([EWIDV]) \((\d+)\) ([^ :]+): (.*)$/;
var TARGET_HELP = 'Not a host[:port], e.g. 192.168.1.10:514, logs.lan or [fd00::1]:514';
var STATES = {
  off : [ 'off', 'con-idle' ], resolving : [ 'resolving', 'con-warn' ], sending : [ 'sending', 'con-ok' ],
  unresolved : [ 'unresolved', 'con-warn' ], no_socket : [ 'no socket', 'con-warn' ]
};

function levelName(level) { return NAMES[level] || String(level); }

/* The levels from off up to `max` (the board's max_level: what its build compiles in); all of them for anything else. */
function levelsUpTo(max) {
  var i = LEVELS.indexOf(max);
  return i < 0 ? LEVELS.slice() : LEVELS.slice(0, i + 1);
}

/* The levels of a component: "*" (everything else) stops at debug, as on the board (LOGGING_DEFAULT_MAX). */
function componentLevels(tag, max) {
  var all = levelsUpTo(max);
  return tag === '*' ? all.slice(0, Math.min(all.length, LEVELS.indexOf('debug') + 1)) : all;
}

function parsePort(text) {
  if (!/^[0-9]+$/.test(text))
    return null;
  var n = parseInt(text, 10);
  return n >= 1 && n <= 65535 ? n : null;
}

/* {host, port} of "host[:port]" or "[v6][:port]" as the board's logging_target_parse reads it; null when it refuses. */
function targetParse(text) {
  var s = String(text);
  var host, tail = '', port = DEFAULT_PORT;
  if (s.charAt(0) === '[') {
    var close = s.indexOf(']');
    if (close < 0)
      return null;
    host = s.slice(1, close);
    tail = s.slice(close + 1);
    if (tail.charAt(0) === ':') {
      port = parsePort(tail.slice(1));
      if (port === null)
        return null;
    } else if (tail !== '') {
      return null;
    }
  } else {
    var colon = s.indexOf(':');
    host = colon < 0 ? s : s.slice(0, colon);
    if (colon >= 0) {
      if (s.indexOf(':', colon + 1) >= 0)
        return null; // a second colon is an unbracketed IPv6 address
      port = parsePort(s.slice(colon + 1));
      if (port === null)
        return null;
    }
  }
  if (host.length === 0 || host.length >= 128 || /[\x00-\x20\x7f\[\]]/.test(host))
    return null;
  return {host : host, port : port};
}

function targetValid(text) { return targetParse(text) !== null; }

function targetText(t) { return (t.host.indexOf(':') >= 0 ? '[' + t.host + ']' : t.host) + ':' + t.port; }

/* "UART Info · USB-JTAG Off · Syslog Warning → 192.168.1.10:514" from the `channels` object of GET /device/logging. */
function summaryChannels(channels) {
  var parts = CHANNELS.map(function(c) { return c[1] + ' ' + levelName(channels[c[0]].level); });
  var line = parts.join(' · ');
  var sys = channels.syslog;
  var target = sys.level !== 'off' && sys.target ? targetParse(sys.target) : null;
  return target ? line + ' → ' + targetText(target) : line;
}

/* "everything else Info · ziggurat Debug" from the `components` object. */
function summaryComponents(components) {
  var parts = [ 'everything else ' + levelName(components['*']) ];
  Object.keys(components).forEach(function(tag) {
    if (tag !== '*')
      parts.push(tag + ' ' + levelName(components[tag]));
  });
  return parts.join(' · ');
}

/* The overrides with `tag` set to `level` / without `tag`; "*" (everything else) stays. Copies. */
function withComponent(components, tag, level) {
  var out = Object.assign({}, components);
  out[tag] = level;
  return out;
}

function withoutComponent(components, tag) {
  var out = Object.assign({}, components);
  delete out[tag];
  return out;
}

/* As the board: printable ASCII without spaces, quotes or backslashes. */
function tagValid(tag) {
  return tag.length >= 1 && tag.length <= TAG_MAX && tag !== '*' && /^[\x21-\x7e]+$/.test(tag) && !/["'\\]/.test(tag);
}

/* One WebSocket frame: {notice, level, tag, text, body}. "# ..." frames are notices from the board. */
function classify(frame) {
  var text = String(frame);
  if (text.indexOf('# ') === 0)
    return {notice : true, level : null, tag : null, text : text, body : text};
  var m = LINE.exec(text);
  if (!m)
    return {notice : false, level : null, tag : null, text : text, body : text};
  return {notice : false, level : LETTERS[m[1]], tag : m[3], text : text, body : m[4], head : m[1] + ' (' + m[2] + ') '};
}

/* Notices always show; a line shows when its text holds `needle` (lower case; empty matches all). */
function matches(l, needle) { return !needle || l.notice === true || l.text.toLowerCase().indexOf(needle) >= 0; }

function filter(lines, text) {
  var needle = String(text).toLowerCase();
  if (!needle)
    return lines;
  return lines.filter(function(l) { return matches(l, needle); });
}

/* [text, pill class] of a channel's status pill from the GET reply. */
function channelStatus(name, reply) {
  var ch = reply.channels[name];
  var pill;
  if (name === 'uart')
    pill = ch.level === 'off' ? [ 'off', 'con-idle' ] : [ 'active', 'con-ok' ];
  else if (name === 'jtag')
    pill = ch.host ? [ 'host', 'con-ok' ] : [ 'no host', 'con-idle' ];
  else
    pill = STATES[ch.state] || STATES.off;
  var c = reply.counters && reply.counters[name];
  return c && c.dropped > 0 ? [ pill[0] + ' · ' + c.dropped + ' dropped', pill[1] ] : pill;
}

/* Text for a rejected PUT: the board names the field that it refused. */
function putError(status, body, errorMessage) {
  if (status === 400 && body && body.detail === 'channels.syslog.target')
    return 'Syslog needs a valid host[:port] before it can be turned on.';
  return errorMessage(status, body);
}

return {LEVELS : LEVELS, levelName : levelName, levelsUpTo : levelsUpTo, componentLevels : componentLevels, targetParse : targetParse, targetValid : targetValid,
        summaryChannels : summaryChannels, summaryComponents : summaryComponents, withComponent : withComponent,
        withoutComponent : withoutComponent, tagValid : tagValid, classify : classify, filter : filter, matches : matches,
        channelStatus : channelStatus, putError : putError, TARGET_HELP : TARGET_HELP};
})();

function LogsPage(env) {
var C = typeof DeviceCommon !== 'undefined' ? DeviceCommon : require('./device_common.js');
var L = LogsLogic;
var kit = C.DeviceKit(env, {});
var $ = kit.$, el = kit.el;
var MAX_LINES = 5000;
var MAX_VIEWERS = 2; // LOGGING_VIEWERS_MAX on the board
var RESOLVE_POLL_MS = 2000;
var RESOLVE_POLLS = 10;
var COPY_RESET_MS = 2000;
var FLUSH_MS = 50;
var FILTER_MS = 150;
var TARGET_HINT = 'UDP, port 514 if omitted. IPv6: [fd00::1]:514';
var current = null; // the last reply of GET/PUT /device/logging
var maxLevel = 'verbose'; // the board's max_level: a release build compiles in up to info
var queue = Promise.resolve(); // one PUT at a time, so a second change builds on the first one's reply
var polls = 0;
var ws = null;
var opened = false;
var lines = []; // everything received, oldest first, at most MAX_LINES
var paused = false;
var filterText = '';
var shown = 0; // lines that pass the filter
var fresh = []; // received since the last flush
var flushTimer = null;
var filterTimer = null;

function option(value, label) {
  var o = el('option', label);
  o.value = value;
  return o;
}

function fillLevels(sel, withOff) {
  L.levelsUpTo(maxLevel).forEach(function(l) {
    if (withOff || l !== 'off')
      sel.appendChild(option(l, L.levelName(l)));
  });
}

/* The static level menus again, for a new max_level; a choice above it becomes max_level. */
function limitLevels(max) {
  maxLevel = max;
  [ [ 'ch-uart', true ], [ 'ch-jtag', true ], [ 'ch-syslog', true ], [ 'cp-new-lv', true ], [ 'live-lv', false ] ]
      .forEach(function(p) {
        var sel = $(p[0]);
        var was = sel.value;
        kit.clear(sel);
        fillLevels(sel, p[1]);
        sel.value = L.levelsUpTo(max).indexOf(was) < 0 ? max : was;
      });
  var hint = $('lv-ceiling');
  hint.classList.toggle('hidden', max === 'verbose');
  hint.textContent = 'This firmware logs up to ' + L.levelName(max) + '; Debug and Verbose need a test build.';
}

function say(text, isError) { kit.note('lg-note', text, isError); }

/* ---- settings ---- */

function setPill(id, status) {
  $(id).textContent = status[0];
  $(id).className = 'con-pill ' + status[1];
}

function summaries(reply) {
  $('ch-sum').textContent = L.summaryChannels(reply.channels);
  $('cp-sum').textContent = L.summaryComponents(reply.components);
}

function componentRow(tag, level) {
  var tr = el('tr');
  var name = el('td');
  if (tag === '*')
    name.appendChild(el('i', 'everything else'));
  else
    name.appendChild(el('code', tag));
  var sel = el('select', null, 'form-select');
  sel.id = 'cp-lv-' + tag;
  sel.setAttribute('aria-label', tag === '*' ? 'Level of everything else' : 'Level of ' + tag);
  L.componentLevels(tag, maxLevel).forEach(function(l) { sel.appendChild(option(l, L.levelName(l))); });
  sel.value = level;
  sel.addEventListener('change', function() {
    var level = sel.value;
    putComponents(function(set) { return L.withComponent(set, tag, level); });
  });
  var lv = el('td');
  lv.appendChild(sel);
  var last = el('td');
  if (tag !== '*') {
    var del = el('button', '✕', 'lg-del');
    del.id = 'cp-del-' + tag;
    del.type = 'button';
    del.title = 'Back to everything else';
    del.setAttribute('aria-label', 'Remove ' + tag);
    del.addEventListener('click', function() { putComponents(function(set) { return L.withoutComponent(set, tag); }); });
    last.appendChild(del);
  }
  [ name, lv, last ].forEach(function(td) { tr.appendChild(td); });
  return tr;
}

function render(reply) {
  current = reply;
  var max = L.levelsUpTo(reply.max_level).length < L.LEVELS.length ? reply.max_level : 'verbose';
  if (max !== maxLevel)
    limitLevels(max);
  var ch = reply.channels;
  [ [ 'ch-uart', 'uart' ], [ 'ch-jtag', 'jtag' ], [ 'ch-syslog', 'syslog' ] ].forEach(function(p) {
    $(p[0]).value = ch[p[1]].level;
  });
  if (env.document.activeElement !== $('sys-target')) {
    $('sys-target').value = ch.syslog.target;
    checkTarget();
  }
  [ [ 'uart-pill', 'uart' ], [ 'jtag-pill', 'jtag' ], [ 'sys-pill', 'syslog' ] ].forEach(function(p) {
    setPill(p[0], L.channelStatus(p[1], reply));
  });
  var rows = $('cp-rows');
  kit.clear(rows);
  var comps = reply.components;
  rows.appendChild(componentRow('*', comps['*']));
  Object.keys(comps).forEach(function(tag) {
    if (tag !== '*')
      rows.appendChild(componentRow(tag, comps[tag]));
  });
  var tags = $('known-tags');
  kit.clear(tags);
  (reply.known_tags || []).forEach(function(tag) {
    if (!Object.prototype.hasOwnProperty.call(comps, tag))
      tags.appendChild(option(tag, ''));
  });
  summaries(reply);
  watchResolving(reply);
}

/* A syslog name being resolved turns into sending or unresolved within moments: look again a few times. */
function watchResolving(reply) {
  if (reply.channels.syslog.state !== 'resolving') {
    polls = 0;
    return;
  }
  if (polls++ < RESOLVE_POLLS)
    env.setTimeout(load, RESOLVE_POLL_MS);
}

function load() {
  return kit.request('GET', '/device/logging', {timeoutMs : 5000}).then(function(r) {
    if (r.status === 200 && r.body) {
      render(r.body);
      return true;
    }
    say(kit.failure(r) + (r.status === 403 ? ': this network interface does not serve the device API.' : ''), true);
    return false;
  }, function(e) {
    say(e.message, true);
    return false;
  });
}

/* The controls back to what the board has, once there is a reply to show (none while the first GET failed). */
function restore() {
  if (current)
    render(current);
}

/* `makeBody()` is called when the PUT is due, so it sees the reply of the one before. A step that fails, or throws,
 * says so and leaves the queue resolved: the next change still goes out. */
function put(makeBody) {
  queue = queue.then(function() {
    return kit.request('PUT', '/device/logging', {
                headers : {'Content-Type' : 'application/json'},
                body : JSON.stringify(makeBody()),
                timeoutMs : 5000
              })
        .then(function(r) {
          if (r.status === 200 && r.body) {
            say('', false);
            render(r.body);
            return;
          }
          say(L.putError(r.status, r.body, C.errorMessage), true);
          restore(); // the controls go back to what the board has
        }, function(e) {
          say(e.message, true);
          restore();
        });
  }).catch(function(e) {
    say(e.message, true);
    restore();
  });
  return queue;
}

function putChannel(name, field, value) {
  var f = {};
  f[field] = value;
  var ch = {};
  ch[name] = f;
  return put(function() { return {channels : ch}; });
}

/* `change(set)` gets the override set of the last reply when the PUT is due, so queued edits build on each other. */
/* An override set exists only once the board's settings are loaded; before that the edit is refused, not queued. */
function putComponents(change) {
  if (!current) {
    say('The settings are not loaded yet; reload the page.', true);
    return queue;
  }
  return put(function() { return {components : change(current.components)}; });
}

function checkTarget() {
  var input = $('sys-target');
  var text = input.value.trim();
  var ok = text === '' || L.targetValid(text);
  input.setAttribute('aria-invalid', ok ? 'false' : 'true');
  $('sys-hint').textContent = ok ? TARGET_HINT : L.TARGET_HELP;
  return ok;
}

function addComponent() {
  var tag = $('cp-new').value.trim();
  if (!L.tagValid(tag)) {
    say('A component is the log tag: up to 23 characters, no spaces or quotes.', true);
    return;
  }
  if (current && !Object.prototype.hasOwnProperty.call(current.components, tag) &&
      Object.keys(current.components).length > 32) {
    say('Up to 32 components can have their own level.', true);
    return;
  }
  say('', false);
  $('cp-new').value = '';
  var level = $('cp-new-lv').value;
  putComponents(function(set) { return L.withComponent(set, tag, level); });
}

function fold(cardId, buttonId) {
  var card = $(cardId);
  var open = !card.classList.contains('lg-open');
  card.classList.toggle('lg-open', open);
  $(buttonId).setAttribute('aria-expanded', open ? 'true' : 'false');
}

/* ---- live view ---- */

function lineRow(l) {
  var row = el('div');
  if (l.notice) {
    row.className = 'lg-note';
    row.textContent = l.text;
  } else if (l.tag) {
    row.className = 'lg-' + l.head.charAt(0);
    row.appendChild(env.document.createTextNode(l.head));
    row.appendChild(el('span', l.tag, 'lg-tag'));
    row.appendChild(env.document.createTextNode(': ' + l.body));
  } else {
    row.textContent = l.text;
  }
  return row;
}

/* Frames are collected and put on screen once per FLUSH_MS: one DOM pass, one count, one scroll per flush. */
function count() {
  $('live-count').textContent =
      (filterText ? shown + ' of ' + lines.length : String(lines.length)) + ' lines' + (paused ? ' \u00b7 paused' : '');
}

function scrollDown() {
  var out = $('live-out');
  out.scrollTop = out.scrollHeight;
}

function needle() { return filterText.toLowerCase(); }

/* The view again from `lines` (the filter changed, or Clear). */
function renderAll() {
  var out = $('live-out');
  var n = needle();
  fresh = [];
  kit.clear(out);
  shown = 0;
  lines.forEach(function(l) {
    l.row = null;
    l.match = L.matches(l, n);
    if (l.match) {
      shown++;
      l.row = lineRow(l);
      out.appendChild(l.row);
    }
  });
  count();
  if (!paused)
    scrollDown();
}

function flush() {
  if (flushTimer !== null) {
    env.clearTimeout(flushTimer);
    flushTimer = null;
  }
  var out = $('live-out');
  fresh.forEach(function(l) {
    if (!l.gone && l.match) {
      l.row = lineRow(l);
      out.appendChild(l.row);
    }
  });
  fresh = [];
  count();
  if (!paused)
    scrollDown();
}

function addLine(frame) {
  var l = L.classify(frame);
  l.match = L.matches(l, needle());
  lines.push(l);
  fresh.push(l);
  if (l.match)
    shown++;
  if (lines.length > MAX_LINES) {
    var old = lines.shift();
    old.gone = true;
    if (old.match)
      shown--;
    if (old.row)
      $('live-out').removeChild(old.row);
  }
  if (flushTimer === null)
    flushTimer = env.setTimeout(flush, FLUSH_MS);
}

function notice(text) { addLine('# ' + text); }

function livePill(text, cls) {
  $('live-pill').textContent = text;
  $('live-pill').className = 'con-pill ' + cls;
}

function liveClosed() {
  livePill('Disconnected', 'con-idle');
  $('live-toggle').textContent = 'Connect';
}

/* A refused handshake shows no status in a browser: ask the board why. */
function explainRefusal() {
  return kit.request('GET', '/device/logging', {timeoutMs : 5000}).then(function(r) {
    if (r.status === 403)
      notice('Refused: this network interface does not serve the device API.');
    else if (r.status === 200 && r.body && r.body.viewers >= MAX_VIEWERS)
      notice('Refused: too many viewers (' + r.body.viewers + ' of ' + MAX_VIEWERS +
             '). Close another live log, then connect again.');
    else
      notice('Could not connect to the live log.');
  }, function() { notice('Could not connect to the live log.'); });
}

function onClose(ev) {
  var wasOpen = opened;
  ws = null;
  opened = false;
  liveClosed();
  if (!wasOpen)
    explainRefusal();
  else if (ev.code === 1013)
    notice('Refused: too many viewers (' + MAX_VIEWERS + ' of ' + MAX_VIEWERS +
           '). Close another live log, then connect again.');
  else if (ev.code === 1001)
    notice('The board closed the connection (this page did not answer its pings).');
  else
    notice('Connection lost. Press Connect to continue.');
}

function connect() {
  var level = $('live-lv').value;
  var sock = new env.WebSocket('ws://' + env.location.host + '/device/logging/ws?level=' + level);
  ws = sock;
  opened = false;
  livePill('Connecting', 'con-warn');
  $('live-toggle').textContent = 'Disconnect';
  // Events of a socket this page already let go of change nothing.
  sock.onopen = function() {
    if (sock !== ws)
      return;
    opened = true;
    livePill('Connected', 'con-ok');
    var now = $('live-lv').value;
    if (now !== level)
      sock.send(JSON.stringify({level : now})); // changed while connecting
    notice('Connected to ' + env.location.host + ' at level ' + now + '. Lines from now on.');
  };
  sock.onmessage = function(ev) {
    if (sock === ws)
      addLine(ev.data);
  };
  sock.onclose = function(ev) {
    if (sock === ws)
      onClose(ev);
  };
}

function disconnect() {
  var sock = ws;
  ws = null;
  opened = false;
  liveClosed();
  sock.close(1000);
  notice('Disconnected. The board stops sending to this page.');
}

function fullscreen(on) {
  $('live').classList.toggle('lg-fs', on);
  $('live-fs').textContent = on ? 'Exit fullscreen' : '⛶ Fullscreen';
  if (!paused)
    scrollDown();
}

function init() {
  kit.start();
  [ 'ch-uart', 'ch-jtag', 'ch-syslog' ].forEach(function(id) {
    fillLevels($(id), true);
    var name = id.slice(3);
    $(id).addEventListener('change', function() { putChannel(name, 'level', $(id).value); });
  });
  fillLevels($('cp-new-lv'), true);
  $('cp-new-lv').value = 'debug';
  fillLevels($('live-lv'), false);
  $('live-lv').value = 'info';
  $('ch-fold').addEventListener('click', function() { fold('ch-card', 'ch-fold'); });
  $('cp-fold').addEventListener('click', function() { fold('cp-card', 'cp-fold'); });
  $('sys-target').addEventListener('input', checkTarget);
  $('sys-target').addEventListener('change', function() {
    if (checkTarget())
      putChannel('syslog', 'target', $('sys-target').value.trim());
  });
  $('cp-add').addEventListener('click', addComponent);
  $('live-toggle').addEventListener('click', function() { ws ? disconnect() : connect(); });
  $('live-lv').addEventListener('change', function() {
    if (ws && opened)
      ws.send(JSON.stringify({level : $('live-lv').value}));
  });
  $('live-pause').addEventListener('click', function() {
    paused = !paused;
    $('live-pause').textContent = paused ? 'Resume' : 'Pause';
    $('live-pause').classList.toggle('btn-primary', paused);
    count();
    if (!paused)
      scrollDown();
  });
  $('live-clear').addEventListener('click', function() {
    lines = [];
    renderAll();
  });
  $('live-copy').addEventListener('click', function() {
    var b = $('live-copy');
    var label = b.textContent;
    C.copyText(env, lines.map(function(l) { return l.text; }).join('\n'), env.document.body)
        .then(function() { b.textContent = 'Copied'; }, function() { b.textContent = 'Copy failed'; })
        .then(function() { env.setTimeout(function() { b.textContent = label; }, COPY_RESET_MS); });
  });
  $('live-filter').addEventListener('input', function() {
    if (filterTimer !== null)
      env.clearTimeout(filterTimer);
    filterTimer = env.setTimeout(function() {
      filterTimer = null;
      filterText = $('live-filter').value;
      renderAll();
    }, FILTER_MS);
  });
  $('live-fs').addEventListener('click', function() { fullscreen(!$('live').classList.contains('lg-fs')); });
  env.document.addEventListener('keydown', function(ev) {
    if (ev.key === 'Escape' && $('live').classList.contains('lg-fs'))
      fullscreen(false);
  });
  liveClosed();
  count();
  return load();
}

return {init : init, load : load, flush : flush};
}

if (typeof module !== 'undefined' && module.exports) {
  module.exports = LogsLogic;
  module.exports.Page = LogsPage;
}

if (typeof document !== 'undefined') {
  var logsEnv = DeviceCommon.DeviceEnv();
  logsEnv.WebSocket = WebSocket;
  logsEnv.navigator = navigator;
  LogsPage(logsEnv).init();
}
