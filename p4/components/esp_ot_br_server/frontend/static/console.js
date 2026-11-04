/*
 * ESP Thread Border Router - Thread Console page (GET/PUT /device/console, WebSocket /device/console/ws, GET /node)
 *
 * ConsoleLogic is pure and runs under node too (tools/tests/test_console_page.py). ConsolePage is the page: the board's
 * OpenThread CLI over one WebSocket, newest session wins. Commands that can take the network down ask first; a line
 * that was waiting when the connection dropped is never sent again.
 */
'use strict';

var ConsoleLogic = (function() {
var BACKOFF_MS = [1000, 2000, 5000];
var DATASET_REASON = 'Changes the network settings. A wrong dataset moves the board to another network.';

/* "ot state", "state\n" -> "state"; nothing left -> null. */
function normalise(line) {
  var s = String(line).trim();
  if (s === 'ot' || /^ot\s/.test(s))
    s = s.slice(2).trim();
  return s ? s : null;
}

/* Why a command needs a confirmation, or null. `network` is the Thread network name, null when unknown. */
function riskyReason(cmd, network) {
  var words = (normalise(cmd) || '').split(/\s+/);
  var net = network || 'the Thread network';
  var first = words[0];
  var line = words.join(' ');
  if (first === 'factoryreset')
    return 'Erases the Thread network settings on this board and restarts OpenThread. The board leaves ' + net +
           ' until it is joined again.';
  if (first === 'reset')
    return 'Restarts the OpenThread stack. The board leaves the mesh for a few seconds.';
  if (line === 'thread stop')
    return 'Stops Thread on this board. It stops being a border router for ' + net + ' until Thread is started again.';
  if (line === 'ifconfig down')
    return 'Takes the Thread interface down. The board leaves the mesh until it is brought up again.';
  if (first === 'dataset' && words.length > 1 && words[1] !== 'active' && words[1] !== 'pending')
    return DATASET_REASON;
  return null;
}

/* The pill state a board notice leads to, null for CLI output. */
function noticeState(text) {
  var rules = [['# Connected to ', 'connected'], ['# Thread is not running', 'not-running'],
               ['# Closed: a new console session', 'taken-over'], ['# Closed: the console was turned off', 'off'],
               ['# Busy: ', 'busy']];
  for (var i = 0; i < rules.length; i++) {
    if (String(text).indexOf(rules[i][0]) === 0)
      return rules[i][1];
  }
  return null;
}

/* Wait before reconnect attempt n (1-based): 1 s, 2 s, then 5 s. */
function backoff(attempt) { return BACKOFF_MS[Math.min(Math.max(attempt, 1), BACKOFF_MS.length) - 1]; }

/* One step through the history (oldest first). pos -1 is the new input below the newest entry. */
function historyStep(history, pos, dir) {
  if (dir === 'up') {
    if (!history.length)
      return {pos : -1, value : ''};
    var up = pos === -1 ? history.length - 1 : Math.max(0, pos - 1);
    return {pos : up, value : history[up]};
  }
  if (pos === -1 || pos + 1 >= history.length)
    return {pos : -1, value : ''};
  return {pos : pos + 1, value : history[pos + 1]};
}

return {normalise : normalise, riskyReason : riskyReason, noticeState : noticeState, backoff : backoff,
        historyStep : historyStep};
})();

function ConsolePage(env) {
var L = ConsoleLogic;
var MAX_LINES = 2000;
var CHIPS = ['state', 'netdata show', 'router table', 'child table', 'leaderdata', 'srp server host'];
var PILLS = {
  connected : ['Connected', 'ok'], connecting : ['Connecting', 'warn'], reconnecting : ['Reconnecting', 'warn'],
  'not-running' : ['Thread not running', 'warn'], 'taken-over' : ['Taken over', 'bad'], off : ['Off', 'bad'],
  'in-use' : ['In use', 'warn'], disconnected : ['Disconnected', 'idle']
};
var ws = null;
var connected = false;
var userClosed = false;
var attempts = 0;
var retryTimer = null;
var waiting = null; // the line sent last, until its Done or Error
var partial = ''; // output after the last newline
var history = [];
var hpos = -1;
var network = null;
var pendingRisky = null;

function $(id) { return env.document.getElementById(id); }

function pill(state) {
  var p = PILLS[state];
  $('con-pill').textContent = p[0];
  $('con-pill').className = 'con-pill con-' + p[1];
}

function setOnline(on) {
  connected = on;
  $('con-cmd').disabled = !on;
  $('con-run').disabled = !on;
  $('con-toggle').textContent = on || retryTimer !== null ? 'Disconnect' : 'Connect';
}

function print(text, kind) {
  var out = $('con-out');
  var row = env.document.createElement('div');
  row.className = 'con-' + kind;
  row.textContent = text;
  out.appendChild(row);
  while (out.children.length > MAX_LINES)
    out.removeChild(out.firstChild);
  if (out.scrollTop !== undefined)
    out.scrollTop = out.scrollHeight;
}

function output(text) {
  var lines = (partial + text.replace(/\r/g, '')).split('\n');
  partial = lines.pop();
  lines.forEach(function(line) {
    var end = line === 'Done' || /^Error \d+:/.test(line);
    print(line, end ? (line === 'Done' ? 'done' : 'err') : 'line');
    if (end)
      waiting = null;
  });
}

function onMessage(ev) {
  var text = String(ev.data);
  var state = L.noticeState(text);
  if (text.indexOf('# ') !== 0) {
    output(text);
    return;
  }
  print(text, 'note');
  if (state === 'connected' || state === 'not-running') {
    attempts = 0;
    pill(state);
    if (state === 'not-running')
      waiting = null; // refused: it never ran
  } else if (state === 'taken-over' || state === 'off') {
    userClosed = true; // the board closed it on purpose: no reconnect
    pill(state);
  }
}

function onClose(ev) {
  ws = null;
  if (partial) {
    print(partial, 'line');
    partial = '';
  }
  if (waiting !== null) {
    print('# "' + waiting + '" got no answer before the connection dropped; it was not sent again', 'note');
    waiting = null;
  }
  if (userClosed) {
    if (ev.code === 4001)
      pill('taken-over');
    else if (ev.code === 4003)
      pill('off');
    else if ($('con-pill').textContent === 'Connected' || $('con-pill').textContent === 'Connecting')
      pill('disconnected');
    setOnline(false);
    return;
  }
  attempts++;
  pill('reconnecting');
  retryTimer = env.setTimeout(function() {
    retryTimer = null;
    connect();
  }, L.backoff(attempts));
  setOnline(false);
}

function connect() {
  userClosed = false;
  if (retryTimer !== null) {
    env.clearTimeout(retryTimer);
    retryTimer = null;
  }
  pill(attempts ? 'reconnecting' : 'connecting');
  ws = new env.WebSocket('ws://' + env.location.host + '/device/console/ws');
  ws.onopen = function() { setOnline(true); };
  ws.onmessage = onMessage;
  ws.onclose = onClose;
  $('con-where').textContent = '';
  $('con-toggle').textContent = 'Disconnect';
}

function disconnect() {
  userClosed = true;
  if (retryTimer !== null) {
    env.clearTimeout(retryTimer);
    retryTimer = null;
  }
  if (ws)
    ws.close(1000);
  pill('disconnected');
  setOnline(false);
}

function send(line) {
  ws.send(line);
  print('> ' + line, 'cmd');
  if (history[history.length - 1] !== line)
    history.push(line);
  hpos = -1;
  waiting = line;
  $('con-cmd').value = '';
}

function run(raw) {
  var line = L.normalise(raw === undefined ? $('con-cmd').value : raw);
  if (line === null || !connected)
    return;
  var why = L.riskyReason(line, network);
  if (why) {
    pendingRisky = line;
    $('con-dlg-cmd').textContent = line;
    $('con-dlg-why').textContent = why;
    $('con-dialog').showModal();
    return;
  }
  send(line);
}

function load() {
  var get = function(url) {
    return env.fetch(url, {method : 'GET'}).then(function(r) {
      return r.text().then(function(t) { return r.status === 200 ? JSON.parse(t) : null; });
    });
  };
  get('/node').then(function(n) { network = n && typeof n.networkName === 'string' ? n.networkName : null; },
                    function() {});
  return get('/device/console').then(function(c) {
    if (!c)
      return;
    $('con-off').classList.toggle('hidden', !!c.enabled);
    if (!c.enabled) {
      pill('off');
      return;
    }
    if (c.session) {
      pill('in-use');
      $('con-where').textContent = 'In use by ' + c.session.peer;
      return;
    }
    connect();
  }, function() { pill('disconnected'); });
}

function enable() {
  env.fetch('/device/console', {method : 'PUT', headers : {'Content-Type' : 'application/json'},
                                body : JSON.stringify({enabled : true})})
      .then(function(r) {
        if (r.status !== 200)
          return;
        $('con-off').classList.add('hidden');
        connect();
      });
}

function init() {
  setOnline(false);
  CHIPS.forEach(function(cmd) {
    var b = env.document.createElement('button');
    b.type = 'button';
    b.className = 'con-chip';
    b.textContent = cmd;
    b.addEventListener('click', function() { run(cmd); });
    $('con-chips').appendChild(b);
  });
  $('con-run').addEventListener('click', function() { run(); });
  $('con-cmd').addEventListener('keydown', function(ev) {
    if (ev.key === 'Enter') {
      ev.preventDefault();
      run();
    } else if (ev.key === 'ArrowUp' || ev.key === 'ArrowDown') {
      ev.preventDefault();
      var s = L.historyStep(history, hpos, ev.key === 'ArrowUp' ? 'up' : 'down');
      hpos = s.pos;
      $('con-cmd').value = s.value;
    }
  });
  $('con-toggle').addEventListener('click', function() { connected || retryTimer !== null ? disconnect() : connect(); });
  $('con-enable').addEventListener('click', enable);
  $('con-clear').addEventListener('click', function() {
    var out = $('con-out');
    while (out.firstChild)
      out.removeChild(out.firstChild);
  });
  $('con-copy').addEventListener('click', function() {
    var text = $('con-out').children.map ? $('con-out').children.map(function(c) { return c.textContent; }).join('\n')
                                         : $('con-out').innerText;
    if (env.navigator && env.navigator.clipboard)
      env.navigator.clipboard.writeText(text).catch(function() {});
  });
  $('con-dlg-cancel').addEventListener('click', function() {
    pendingRisky = null;
    $('con-dialog').close();
  });
  $('con-dlg-run').addEventListener('click', function() {
    var line = pendingRisky;
    pendingRisky = null;
    $('con-dialog').close();
    if (line !== null && connected)
      send(line);
  });
  return load();
}

return {init : init, run : run};
}

if (typeof module !== 'undefined' && module.exports) {
  module.exports = ConsoleLogic;
  module.exports.Page = ConsolePage;
}

if (typeof document !== 'undefined') {
  var consoleEnv = DeviceCommon.DeviceEnv();
  consoleEnv.WebSocket = WebSocket;
  consoleEnv.navigator = navigator;
  ConsolePage(consoleEnv).init();
}
