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
               ['# Busy: ', 'busy'], ['# Line too long', 'too-long']];
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
var C = typeof DeviceCommon !== 'undefined' ? DeviceCommon : require('./device_common.js');
var L = ConsoleLogic;
var MAX_LINES = 2000;
var SETTLE_MS = 1500; // a closing socket's session is gone by then
var COPY_RESET_MS = 2000;
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
var checking = false; // a reconnect is asking the board whether the console is free
var settle = null; // the load's second look at a session (a timer id, then true while its GET runs)
var openedAt = null; // when the last socket opened (env.now)
var sockOpened = false; // the current socket got past the handshake
var NO_RADIO_RETRY_MS = 5000;
var waiting = null; // the line sent last, until its Done or Error
var before = null; // what waited before it, back in place if the board refuses the line
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
  $('con-toggle').textContent = on || retrying() ? 'Disconnect' : 'Connect';
}

function retrying() { return retryTimer !== null || checking; }

function get(url) {
  return env.fetch(url, {method : 'GET'}).then(function(r) {
    return r.text().then(function(t) { return r.status === 200 ? JSON.parse(t) : null; });
  });
}

function inUse(session) {
  pill('in-use');
  $('con-where').textContent = 'In use by ' + session.peer;
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
    if (end) {
      waiting = null;
      before = null; // finished: a later refusal brings back nothing
    }
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
  if (state === 'busy' || state === 'too-long' || state === 'not-running') {
    waiting = before; // refused: it never ran
    before = null;
  }
  if (state === 'connected' || state === 'not-running') {
    attempts = 0;
    pill(state);
  } else if (state === 'taken-over' || state === 'off') {
    userClosed = true; // the board closed it on purpose: no reconnect
    pill(state);
  }
}

/*
 * Asks the board whether Thread runs: with no radio in OTBR mode it shows the notice (the console WebSocket answers
 * 503 then) and resolves true.
 */
function threadDown() {
  return get('/device/status').then(function(s) { return !!C.showThreadNotice(env.document, C.threadNotice(s, null)); },
                                    function() { return false; });
}

function waitForRadio() {
  pill('not-running');
  setOnline(false);
  retryTimer = env.setTimeout(function() {
    retryTimer = null;
    load();
  }, NO_RADIO_RETRY_MS);
}

function onClose(ev) {
  var wasOpen = sockOpened;
  ws = null;
  if (partial) {
    print(partial, 'line');
    partial = '';
  }
  if (waiting !== null) {
    print('# "' + waiting + '" got no answer before the connection dropped; it was not sent again', 'note');
    waiting = null;
  }
  if (ev.code === 4001 || ev.code === 4003)
    userClosed = true; // the board closed it on purpose, even if its notice was lost
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
  if (wasOpen) {
    retry();
    return;
  }
  // The handshake failed (the console answers 503 while no radio runs Thread): say why instead of retrying blindly.
  threadDown().then(function(down) {
    if (userClosed || ws)
      return; // the user acted meanwhile
    if (down)
      waitForRadio();
    else
      retry();
  });
}

function retry() {
  attempts++;
  pill('reconnecting');
  retryTimer = env.setTimeout(function() {
    retryTimer = null;
    reconnect();
  }, L.backoff(attempts));
  setOnline(false);
}

// Reconnect only into a free console, or one still held by this page's own dropped socket: a session that began
// after that socket opened is someone else's, and taking it over would cut them off.
function reconnect() {
  checking = true;
  var elapsed = openedAt === null ? Infinity : (env.now() - openedAt) / 1000;
  get('/device/console').then(function(c) {
    if (!checking)
      return; // disconnected meanwhile
    checking = false;
    if (!c)
      return retry();
    if (!c.enabled) {
      $('con-off').classList.remove('hidden');
      pill('off');
    } else if (c.session && c.session.since_s < elapsed - 2) {
      inUse(c.session);
    } else {
      connect();
      return;
    }
    attempts = 0;
    setOnline(false);
  }, function() {
    if (checking) {
      checking = false;
      retry();
    }
  });
}

function stopRetry() {
  checking = false;
  if (typeof settle === 'number')
    env.clearTimeout(settle);
  settle = null;
  if (retryTimer !== null) {
    env.clearTimeout(retryTimer);
    retryTimer = null;
  }
}

function connect() {
  userClosed = false;
  stopRetry();
  pill(attempts ? 'reconnecting' : 'connecting');
  // Events of a socket this page already let go of (a close that arrives late) change nothing.
  var sock = new env.WebSocket('ws://' + env.location.host + '/device/console/ws');
  ws = sock;
  sockOpened = false;
  sock.onopen = function() {
    if (sock === ws) {
      sockOpened = true;
      openedAt = env.now();
      setOnline(true);
    }
  };
  sock.onmessage = function(ev) {
    if (sock === ws)
      onMessage(ev);
  };
  sock.onclose = function(ev) {
    if (sock === ws)
      onClose(ev);
  };
  $('con-where').textContent = '';
  $('con-toggle').textContent = 'Disconnect';
}

function disconnect() {
  userClosed = true;
  stopRetry();
  var sock = ws;
  if (sock) {
    onClose({code : 1000});
    sock.close(1000);
  }
  pill('disconnected');
  setOnline(false);
}

function send(line) {
  ws.send(line);
  print('> ' + line, 'cmd');
  if (history[history.length - 1] !== line)
    history.push(line);
  hpos = -1;
  before = waiting;
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
  return threadDown().then(function(down) {
    if (down) {
      waitForRadio();
      return;
    }
    return loadConsole();
  }, loadConsole);
}

function loadConsole() {
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
    if (!c.session)
      return connect();
    // After a reload the old page's socket may still be closing: ask once more before calling it someone else's.
    pill('connecting');
    settle = env.setTimeout(function() {
      settle = true;
      get('/device/console').then(function(again) {
        if (settle !== true)
          return; // Connect or Disconnect meanwhile
        settle = null;
        if (again && again.enabled && again.session)
          inUse(again.session);
        else if (again && again.enabled)
          connect();
        else if (again) {
          $('con-off').classList.remove('hidden');
          pill('off');
        } else
          pill('disconnected');
      }, function() {
        if (settle === true) {
          settle = null;
          pill('disconnected');
        }
      });
    }, SETTLE_MS);
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
  $('con-toggle').addEventListener('click', function() { ws !== null || retrying() ? disconnect() : connect(); });
  $('con-enable').addEventListener('click', enable);
  $('con-clear').addEventListener('click', function() {
    var out = $('con-out');
    while (out.firstChild)
      out.removeChild(out.firstChild);
  });
  $('con-copy').addEventListener('click', function() {
    var b = $('con-copy');
    var label = b.textContent;
    var text = $('con-out').children.map ? $('con-out').children.map(function(c) { return c.textContent; }).join('\n')
                                         : $('con-out').innerText;
    C.copyText(env, text, env.document.body)
        .then(function() { b.textContent = 'Copied'; }, function() { b.textContent = 'Copy failed'; })
        .then(function() { env.setTimeout(function() { b.textContent = label; }, COPY_RESET_MS); });
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
