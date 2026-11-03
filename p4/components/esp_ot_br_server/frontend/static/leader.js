/*
 * ESP Thread Border Router - Leader card of the Network page (GET/PUT /device/thread, GET /node)
 *
 * LeaderLogic is pure and runs under node too (tools/tests/test_leader_page.py). LeaderPage is the card: the two weights,
 * the stored weight adjustment (slider with Save) and the Become leader button with its confirm dialog.
 */
'use strict';

var LeaderLogic = (function() {
var SCALE_MIN = 48;
var SCALE_MAX = 96;
var ADJ_MAX = 16; // OpenThread limit of the adjustment, both ways
var MINUS = '\u2212';
var APOS = '\u2019';

function isNum(v) { return typeof v === 'number' && isFinite(v); }

/* "+8", "0", or "\u22123" (a real minus sign). */
function sign(n) { return n > 0 ? '+' + n : n < 0 ? MINUS + Math.abs(n) : '0'; }

/* Position of weight `w` on the 48..96 scale in percent, clamped. */
function scalePos(w) { return Math.max(0, Math.min(100, (w - SCALE_MIN) / (SCALE_MAX - SCALE_MIN) * 100)); }

function hex4(n) {
  var h = Math.max(0, n).toString(16);
  while (h.length < 4)
    h = '0' + h;
  return h;
}

/* "this board" when it is the leader, else the leader's RLOC16 (router id << 10). */
function leaderName(node) {
  if (node && node.state === 'leader')
    return 'this board';
  if (node && node.leaderData && isNum(node.leaderData.leaderRouterId))
    return 'router 0x' + hex4(node.leaderData.leaderRouterId * 1024);
  return 'the current leader';
}

function attachedState(state) { return state === 'child' || state === 'router' || state === 'leader'; }

/*
 * What the card shows. `thread` and `node` are the parsed replies, null when the read failed; `pending` is the slider
 * value, null while it is untouched.
 */
function view(thread, node, pendingAdj) {
  var adjustment = thread && isNum(thread.leader_weight_adjustment) ? thread.leader_weight_adjustment : 0;
  var hasWeight = !!thread && isNum(thread.leader_weight);
  var ownWeight = hasWeight ? thread.leader_weight : null;
  var base = hasWeight ? (isNum(thread.leader_weight_base) ? thread.leader_weight_base : ownWeight - adjustment) : null;
  var state = node && typeof node.state === 'string' ? node.state : null;
  var isLeader = state === 'leader';
  var attached = attachedState(state);
  var reported = node && node.leaderData && isNum(node.leaderData.weighting) ? node.leaderData.weighting : null;
  var leaderWeight = isLeader && ownWeight !== null ? ownWeight : reported;
  var shown = hasWeight && attached && leaderWeight !== null;
  var dirty = !!thread && isNum(pendingAdj) && pendingAdj !== adjustment;
  var shownAdj = isNum(pendingAdj) ? pendingAdj : adjustment;
  var v = {
    ownWeight : ownWeight,
    base : base,
    adjustment : adjustment,
    leaderWeight : shown ? leaderWeight : null,
    leaderName : leaderName(node),
    role : state || '\u2014',
    verdict : '',
    verdictKind : '',
    scaleOwn : shown ? scalePos(ownWeight) : null,
    scaleLeader : shown ? scalePos(leaderWeight) : null,
    buttonEnabled : false,
    hint : '',
    dirty : dirty,
    saveEnabled : dirty,
    comparisonShown : shown,
    breakdown : hasWeight ? base + ' base \u00b7 ' + sign(adjustment) + ' adjustment' : '',
    label : 'Weight adjustment: ' + sign(shownAdj) +
            (dirty && base !== null ? '  (weight ' + (base + shownAdj) + ', not saved)' : dirty ? '  (not saved)' : '')
  };
  if (shown) {
    if (isLeader) {
      v.verdict = 'This board is the leader.';
      v.verdictKind = 'good';
    } else if (ownWeight > leaderWeight) {
      v.verdict = 'Higher than the leader: this board wins the next merge.';
      v.verdictKind = 'good';
    } else if (ownWeight === leaderWeight) {
      v.verdict = 'Equal: the next merge is decided by the random partition ID.';
      v.verdictKind = 'warn';
    } else {
      v.verdict = 'Lower than the leader: the leader keeps the role.';
      v.verdictKind = 'bad';
    }
  }
  if (!node)
    v.hint = 'Thread is not running.';
  else if (!attached)
    v.hint = 'This board is not attached to a Thread network.';
  else if (!shown)
    v.hint = 'The weights could not be read just now.';
  else if (isLeader)
    v.hint = 'Already the leader.';
  else if (dirty)
    v.hint = 'Save the adjustment first.';
  else if (leaderWeight > base + ADJ_MAX)
    v.hint = 'The leader' + APOS + 's weight ' + leaderWeight + ' is above the most this board can reach (' +
             (base + ADJ_MAX) + ').';
  else if (ownWeight <= leaderWeight)
    v.hint = 'Needs a weight above ' + leaderWeight + ' (the leader' + APOS + 's). Raise the adjustment to ' +
             sign(leaderWeight - base + 1) + ' or more.';
  else {
    v.hint = 'Takes over now. The mesh reattaches for a few seconds.';
    v.buttonEnabled = true;
  }
  return v;
}

return {view : view, leaderName : leaderName, sign : sign, scalePos : scalePos};
})();

/* The card: reads both replies every 5 s, owns the slider, Save, the confirm dialog and the fast polling after a take-over. */
function LeaderPage(env, opts) {
var C = typeof DeviceCommon !== 'undefined' ? DeviceCommon : require('./device_common.js');
var L = LeaderLogic;
var POLL_MS = 5000;
var TAKEOVER_POLL_MS = 1000;
var TAKEOVER_MAX_MS = 30000;
var JSON_HEADERS = {'Content-Type' : 'application/json'};
var kit = C.DeviceKit(env, {trial : false});
var $ = kit.$, setText = kit.setText, show = kit.show, note = kit.note;
var thread = null;  // last GET /device/thread, null when it failed
var node = null;    // last GET /node, null when it failed
var pending = null; // slider value while it differs from the saved one
var busy = false;   // a PUT is in flight
var takingOver = false;
var seq = 0;        // only the newest read may fill the card
opts = opts || {};

function current() { return L.view(thread, node, pending); }

function render() {
  var v = current();
  setText('ld-role', v.role);
  show('ld-comparison', v.comparisonShown);
  setText('ld-own-weight', v.ownWeight === null ? '\u2014' : String(v.ownWeight));
  setText('ld-own-breakdown', v.breakdown);
  setText('ld-leader-weight', v.leaderWeight === null ? '\u2014' : String(v.leaderWeight));
  setText('ld-leader-who', v.leaderName);
  if (v.comparisonShown) {
    $('ld-scale-own').style.left = v.scaleOwn + '%';
    $('ld-scale-leader').style.left = v.scaleLeader + '%';
  }
  setText('ld-verdict', v.verdict);
  $('ld-verdict').className = 'ld-verdict' + (v.verdictKind ? ' ld-' + v.verdictKind : '');
  setText('ld-adj-label', v.label);
  var shownAdj = pending === null ? v.adjustment : pending;
  if (String($('ld-adj').value) !== String(shownAdj))
    $('ld-adj').value = String(shownAdj);
  $('ld-adj').disabled = thread === null || busy;
  $('ld-save').disabled = !v.saveEnabled || busy;
  setText('ld-become-hint', v.hint);
  $('ld-become').disabled = !v.buttonEnabled || busy || takingOver;
  for (var i = 1; i <= 2; i++)
    setText('ld-name-' + i, v.leaderName);
}

function readJson(url) {
  return kit.request('GET', url, {timeoutMs : 5000}).then(function(r) { return r.status === 200 && r.body ? r.body : null; },
                                                          function() { return null; });
}

/* Both reads at once. Resolves to true when /device/thread answered. */
function refresh() {
  var mine = ++seq;
  return Promise.all([ readJson('/device/thread'), readJson('/node') ]).then(function(r) {
    if (mine !== seq)
      return r[0] !== null;
    thread = r[0];
    node = r[1];
    render();
    return thread !== null;
  });
}

function refreshNode() {
  var mine = ++seq;
  return readJson('/node').then(function(n) {
    if (mine === seq) {
      node = n;
      render();
    }
    return n !== null;
  });
}

function errorText(r) { return r.body && r.body.detail ? r.body.detail : kit.failure(r); }

function save() {
  var v = current();
  if (busy || !v.saveEnabled)
    return Promise.resolve(null);
  var wanted = pending;
  busy = true;
  note('ld-save-msg', '', false);
  render();
  return kit.request('PUT', '/device/thread', {headers : JSON_HEADERS,
                                               body : JSON.stringify({leader_weight_adjustment : wanted}),
                                               timeoutMs : 5000})
      .then(function(r) {
        if (r.status !== 200 || !r.body) {
          note('ld-save-msg', errorText(r), true);
          return;
        }
        thread = r.body;
        pending = null;
        if (r.body.applied === false)
          note('ld-save-msg', 'Saved. ' + (r.body.detail || 'It is applied when Thread starts.'), false);
        else
          note('ld-save-msg', 'Saved.', false);
      }, function(e) { note('ld-save-msg', e.message, true); })
      .then(function() {
        busy = false;
        render();
        return refreshNode(); // the reply has the weights; the leader's may have changed with them
      });
}

function openDialog() {
  var v = current();
  if (busy || !v.buttonEnabled)
    return;
  setText('ld-modal-weight', String(v.ownWeight));
  setText('ld-modal-leader-weight', String(v.leaderWeight));
  setText('ld-modal-name', v.leaderName);
  setText('ld-modal-name-2', v.leaderName);
  $('ld-modal').classList.add('active');
  $('ld-cancel').focus();
}

function closeDialog() {
  $('ld-modal').classList.remove('active');
  $('ld-become').focus();
}

function pollTakeOver(started) {
  env.setTimeout(function() {
    refresh().then(function() {
      var done = node && node.state === 'leader';
      if (done) {
        takingOver = false;
        note('ld-become-msg', 'This board is the leader now.', false);
      } else if (env.now() - started >= TAKEOVER_MAX_MS) {
        takingOver = false;
        note('ld-become-msg', 'This board is not the leader after 30 s. Check the role again in a moment.', true);
      } else {
        pollTakeOver(started);
        return;
      }
      render();
    });
  }, TAKEOVER_POLL_MS);
}

function confirmTakeOver() {
  closeDialog();
  if (busy)
    return Promise.resolve(null);
  busy = true;
  note('ld-become-msg', '', false);
  render();
  return kit.request('PUT', '/device/thread', {headers : JSON_HEADERS, body : JSON.stringify({become_leader : true}),
                                               timeoutMs : 5000})
      .then(function(r) {
        busy = false;
        if (r.status === 200 && r.body && r.body.become_leader === 'started') {
          takingOver = true;
          note('ld-become-msg', 'Taking over: the mesh is reattaching ...', false);
          render();
          pollTakeOver(env.now());
          return;
        }
        note('ld-become-msg', r.status === 200 ? 'The board did not start the take-over.' : errorText(r), true);
        render();
        return refresh(); // the page may be stale: the leader changed meanwhile
      }, function(e) {
        busy = false;
        note('ld-become-msg', e.message, true);
        render();
      });
}

function init() {
  $('ld-adj').addEventListener('input', function(ev) {
    if (thread === null || busy)
      return;
    var n = parseInt(ev.target.value, 10);
    pending = isNaN(n) ? null : n;
    note('ld-save-msg', '', false);
    render();
  });
  $('ld-save').addEventListener('click', save);
  $('ld-become').addEventListener('click', openDialog);
  $('ld-cancel').addEventListener('click', closeDialog);
  $('ld-confirm').addEventListener('click', confirmTakeOver);
  $('ld-modal').addEventListener('keydown', function(ev) {
    if (ev.key === 'Escape')
      closeDialog();
  });
  kit.poller(function() { return takingOver ? Promise.resolve(true) : refresh(); }, POLL_MS, 'ld-card');
}

return {init : init, refresh : refresh, save : save, render : render, kit : kit};
}

if (typeof module !== 'undefined' && module.exports) {
  module.exports = LeaderLogic;
  module.exports.Page = LeaderPage;
}

if (typeof document !== 'undefined')
  LeaderPage(DeviceCommon.DeviceEnv()).init();
