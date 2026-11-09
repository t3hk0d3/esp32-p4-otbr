/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP Thread Border Router - Radios page (GET /device/channels, PUT and DELETE /device/channels/{id}, /device/status)
 *
 * RadiosLogic is pure and runs under node too (tools/tests/test_radios_page.py). RadiosPage is the page. Everything
 * that comes from the device is inserted with textContent, never as markup.
 */
'use strict';

var RadiosLogic = (function() {
var C = typeof DeviceCommon !== 'undefined' ? DeviceCommon : require('./device_common.js');
var DASH = '—';
var BAUDS = [ 115200, 230400, 460800, 921600 ];
var PORT_DEFAULT = 6638;

/* The mode cards, in order. `needs`: the firmware type the mode runs on. */
var MODES = [
  {id : 'off', icon : 'i-off', name : 'Off', desc : 'Listed, not used.'}, {
    id : 'otbr',
    icon : 'i-otbr',
    name : 'Thread border router',
    desc : 'This board\'s own Thread network runs on it.',
    needs : 'spinel'
  },
  {id : 'rcp-proxy', icon : 'i-proxy', name : 'RCP proxy', desc : 'Its serial line goes to another computer.'},
  {id : 'ziggurat', icon : 'i-zig', name : 'Zigbee (Ziggurat)', desc : 'The on-board Zigbee coordinator.'}
];
var FW_NAMES = {spinel : 'an OpenThread RCP', znp : 'Z-Stack (a Zigbee coprocessor)', ezsp : 'EZSP (a Zigbee coprocessor)'};
var FW_SHORT = {spinel : 'an OpenThread RCP', znp : 'Z-Stack', ezsp : 'EZSP'};

function modeOf(id) {
  for (var i = 0; i < MODES.length; i++)
    if (MODES[i].id === id)
      return MODES[i];
  return {id : id, icon : 'i-off', name : String(id), desc : ''};
}

/*
 * THE one place that decides whether this build can run a mode. The snapshot lists them in `available_modes`; a
 * snapshot without the list (an older board) can run off, otbr and rcp-proxy, and not ziggurat.
 */
function modeAvailable(mode, snapshot) {
  if (snapshot && Array.isArray(snapshot.available_modes))
    return snapshot.available_modes.indexOf(mode) >= 0;
  return mode !== 'ziggurat';
}

function present(ch) { return ch.mode !== 'unavailable'; }

/* The mode a channel is set to: the stored one while it is unplugged. */
function currentMode(ch) { return present(ch) ? ch.mode : ch.stored_mode; }

function findChannel(snapshot, id) {
  var list = snapshot && snapshot.channels ? snapshot.channels : [];
  for (var i = 0; i < list.length; i++)
    if (list[i].id === id)
      return list[i];
  return null;
}

function labelOf(snapshot, id) {
  var ch = findChannel(snapshot, id);
  return ch ? ch.label : String(id);
}

/*
 * The other channel Thread runs on (the one a move takes Thread from), or null. The board's `otbr_channel` of
 * /device/status says it when it is not null; only without it (null, or no status yet) the stored modes are looked at.
 */
function threadElsewhere(snapshot, ch, status) {
  var running = status ? status.otbr_channel : null;
  if (typeof running === 'string')
    return running === ch.id ? null : (findChannel(snapshot, running) || {id : running, label : running});
  var list = snapshot && snapshot.channels ? snapshot.channels : [];
  for (var i = 0; i < list.length; i++)
    if (list[i].id !== ch.id && list[i].stored_mode === 'otbr')
      return list[i];
  return null;
}

/* Why a mode card of this channel can't be picked ('' when it can). */
function cardWhy(mode, ch, snapshot) {
  if (!modeAvailable(mode, snapshot))
    return 'Not in this build';
  if (!present(ch)) {
    return mode === ch.stored_mode ? '' : 'Plug it in to change the mode.';
  }
  var m = modeOf(mode);
  var type = ch.firmware && ch.firmware.type;
  if (m.needs && type && FW_NAMES[type] && type !== m.needs)
    return 'Needs ' + FW_NAMES[m.needs] + '; this stick runs ' + FW_SHORT[type] + '.';
  if (mode === 'rcp-proxy' && ch.rcp_proxy === null)
    return 'No driver for this adapter.';
  return '';
}

function capitalize(s) { return s ? s.charAt(0).toUpperCase() + s.slice(1) : s; }

function stateView(ch) {
  if (!present(ch))
    return {text : 'Unplugged', tone : ''};
  if (ch.state === 'fault')
    return {text : 'Fault' + (ch.error ? ': ' + ch.error : ''), tone : 'chip-bad'};
  if (ch.state === 'starting')
    return {text : 'Starting', tone : 'chip-warn'};
  if (ch.state === 'busy')
    return {text : 'Busy', tone : 'chip-warn'};
  if (ch.state === 'running') {
    var ps = ch.mode === 'rcp-proxy' && ch.rcp_proxy && ch.rcp_proxy.state;
    if (ps === 'backoff') // the listener or the connection failed: it is tried again, the reason says why
      return {text : 'Backoff' + (ch.rcp_proxy.last_error ? ': ' + ch.rcp_proxy.last_error : ''), tone : 'chip-warn'};
    return {text : ps ? capitalize(ps) : 'Running', tone : 'chip-ok'};
  }
  return {text : capitalize(ch.state || 'idle'), tone : ''};
}

function activityText(ch) {
  if (!present(ch))
    return 'Settings are kept';
  var p = ch.rcp_proxy;
  if (ch.mode === 'rcp-proxy' && p && ch.state === 'running') {
    if (p.peer)
      return 'to ' + p.peer;
    return p.direction === 'connect' ? 'dialing ' + p.host + ':' + p.port : 'waiting on port ' + p.port;
  }
  if (ch.mode === 'off' || !ch.counters)
    return '';
  return ch.counters.frames_in + ' frames in · ' + ch.counters.drops + ' drops';
}

/* Everything a table row shows: {gone, icon, modeText, modeSub, fwText, fwSub, state, activity}. */
function rowView(ch) {
  var gone = !present(ch);
  var m = modeOf(ch.mode);
  var v = {gone : gone, state : stateView(ch), activity : activityText(ch), modeSub : ''};
  if (gone) {
    v.icon = 'i-gone';
    v.modeText = modeOf(ch.stored_mode).name + ' (when back)';
  } else if (ch.mode === 'off' && ch.stored_mode !== 'off') {
    v.icon = 'i-off';
    v.modeText = 'Off, stored: ' + modeOf(ch.stored_mode).name;
    v.modeSub = ch.error || '';
  } else {
    v.icon = m.icon;
    v.modeText = m.name;
  }
  var fw = ch.firmware;
  v.fwText = fw ? (fw.type || DASH) + ' ' + (fw.version || DASH) : DASH;
  v.fwSub = fw ? (fw.chip || DASH) : '';
  return v;
}

/* Subtitle of the dialog: id and what runs on the radio. */
function subtitle(ch) {
  var fw = ch.firmware;
  return ch.id + (fw ? ' · ' + (fw.type || DASH) + ' ' + (fw.version || DASH) + ' · ' + (fw.chip || DASH) : '');
}

function proxyOf(ch) {
  var p = ch.rcp_proxy;
  return {
    direction : p ? p.direction : 'listen',
    adapter : p ? p.adapter : 'tcp',
    port : p ? p.port : PORT_DEFAULT,
    host : p ? p.host : '',
    framing : p ? p.framing : 'hdlc'
  };
}

/*
 * What Save sends for `draft` = {pick, touched, direction, adapter, framing, host, port, baud, flow}: {error} or
 * {requests: [body, ...], move}. The settings go first and the mode alone after them: the API refuses a start, move
 * or stop of Thread that carries settings. Thread onto a radio that runs rcp-proxy sets it off first (the API refuses otbr while
 * the proxy runs on it).
 */
function plan(ch, snapshot, draft, status) {
  var settings = {};
  var plugged = present(ch);
  if (ch.usb) {
    var baud = C.wholeNumber(draft.baud, 1, 4000000);
    if (baud !== null && baud !== ch.line.baud)
      settings.baud = baud;
    if (ch.caps && ch.caps.flow_control && !!draft.flow !== !!ch.line.flow_control)
      settings.flow_control = !!draft.flow;
  }
  if (draft.pick === 'rcp-proxy') {
    var old = proxyOf(ch);
    var port = C.wholeNumber(draft.port, 1, 65535);
    if (port === null)
      return {error : 'The port must be a number from 1 to 65535.'};
    var host = String(draft.host).trim();
    if (draft.direction === 'connect' && host === '')
      return {error : 'Connect needs the host to dial.'};
    if (host.length > 63)
      return {error : 'The host is at most 63 characters.'};
    var px = {};
    if (draft.direction !== old.direction)
      px.direction = draft.direction;
    if (draft.adapter !== old.adapter)
      px.adapter = draft.adapter;
    if (port !== old.port)
      px.port = port;
    if (host !== old.host)
      px.host = host;
    if (draft.framing !== old.framing)
      px.framing = draft.framing;
    if (Object.keys(px).length)
      settings.rcp_proxy = px;
  }
  var requests = [];
  if (Object.keys(settings).length)
    requests.push(settings);
  var move = false;
  if (plugged && draft.touched && draft.pick !== ch.stored_mode) {
    if (draft.pick === 'otbr' && ch.mode === 'rcp-proxy')
      requests.push({mode : 'off'});
    requests.push({mode : draft.pick});
    move = draft.pick === 'otbr' && !!threadElsewhere(snapshot, ch, status);
  }
  return {requests : requests, move : move};
}

var CODE_TEXT = {
  mode_taken : 'Another radio already runs this mode.',
  incompatible : 'The radio\'s firmware does not fit this mode.',
  unavailable : 'The radio is not plugged in or has no driver.',
  not_available : 'This mode is not in this build.',
  hybrid_not_supported : 'Thread and Zigbee cannot run at once.',
  c6_busy : 'The on-board radio is busy: a firmware job runs, or a client is connected to its proxy.',
  drain_timeout : 'The previous radio did not stop in time, so Thread did not move.',
  port_taken : 'Another radio\'s proxy already listens on this port.',
  unknown_channel : 'This radio is no longer known to the board.',
  in_use : 'This radio is in use. Only an unplugged radio can be forgotten.',
  not_started : 'The radio service is not running yet. Try again in a moment.'
};

/* Text for a refused PUT or DELETE (status and parsed body, null when it had none). */
function errorText(status, body) {
  var code = body && typeof body.error === 'string' ? body.error : '';
  var detail = body && typeof body.detail === 'string' ? body.detail : '';
  if (code === 'probe_failed')
    return 'The test run failed' + (detail ? ': ' + detail : '') + '. Thread stays where it is.';
  if (code === 'busy')
    return 'The board is busy' + (detail ? ': ' + detail : '') + '. Try again in a moment.';
  if (code === 'bad_value')
    return 'Refused' + (detail ? ': ' + detail : ' (a value is not accepted)') + '.';
  if (code === 'port_taken')
    return CODE_TEXT.port_taken + (detail ? ' (' + detail + ')' : '');
  if (code === 'failed')
    return 'The change failed on the board' + (detail ? ' (' + detail + ')' : '') + '.';
  if (CODE_TEXT[code])
    return CODE_TEXT[code];
  return C.errorMessage(status, body);
}

/*
 * What the dialog says when Save's PUTs stopped at a refusal: `requests` as plan() made them, `replies` the answers in
 * order (the last one refused, or {status: 0, message} for a lost connection). What the PUTs before it already did comes
 * first: settings stored, and the radio switched off on its way to Thread (its proxy stopped, its peer dropped).
 */
function saveFailureText(requests, replies) {
  var last = replies[replies.length - 1];
  var why = last.status === 0 ? last.message : errorText(last.status, last.body);
  var settings = false, off = false;
  for (var i = 0; i < replies.length - 1; i++) {
    if (requests[i].mode === 'off')
      off = true;
    else if (requests[i].mode === undefined)
      settings = true;
  }
  if (off)
    return (settings ? 'The settings were saved and the radio was switched off' : 'The radio was switched off') +
           ' (its proxy stopped), but Thread did not start on it. ' + why;
  if (settings)
    return 'The settings were saved, but the mode change failed: ' + why;
  return why;
}

return {
  MODES : MODES,
  BAUDS : BAUDS,
  DASH : DASH,
  modeOf : modeOf,
  modeAvailable : modeAvailable,
  present : present,
  currentMode : currentMode,
  findChannel : findChannel,
  labelOf : labelOf,
  threadElsewhere : threadElsewhere,
  cardWhy : cardWhy,
  rowView : rowView,
  subtitle : subtitle,
  proxyOf : proxyOf,
  plan : plan,
  errorText : errorText,
  saveFailureText : saveFailureText
};
})();

function RadiosPage(env) {
var C = typeof DeviceCommon !== 'undefined' ? DeviceCommon : require('./device_common.js');
var L = RadiosLogic;
var kit = C.DeviceKit(env, {actionIds : [ 'mSave', 'mForget' ]});
var $ = kit.$, el = kit.el, clear = kit.clear, setText = kit.setText, show = kit.show, note = kit.note;
var doc = env.document;
var SVG_NS = 'http://www.w3.org/2000/svg';
var FAST_MS = 2000;
var MOVE_GIVE_UP_MS = 90000;

var snap = null;       // last GET /device/channels
var status = null;     // last GET /device/status
var openId = null;     // the channel the dialog shows
var draft = null;
var moving = null;     // {from, to}: a move of Thread that was accepted and has not settled
var poll = null;
var fastTimer = null;

function icon(id, cls) {
  var svg = doc.createElementNS(SVG_NS, 'svg');
  if (cls)
    svg.setAttribute('class', cls);
  svg.setAttribute('aria-hidden', 'true');
  var use = doc.createElementNS(SVG_NS, 'use');
  use.setAttribute('href', '#' + id);
  svg.appendChild(use);
  return svg;
}

function cell(children) {
  var td = doc.createElement('td');
  children.forEach(function(c) { td.appendChild(c); });
  return td;
}

function sub(text, cls) { return el('span', text, 'sub' + (cls ? ' ' + cls : '')); }

/* ---- the table ---- */

function renderRow(ch) {
  var v = L.rowView(ch);
  var tr = doc.createElement('tr');
  tr.className = v.gone ? 'gone' : '';
  var modeCell = el('span', null, 'mode-cell');
  modeCell.appendChild(icon(v.icon));
  modeCell.appendChild(el('span', v.modeText));
  var modeTd = cell([ modeCell ]);
  if (v.modeSub)
    modeTd.appendChild(sub(v.modeSub));
  var fwTd = cell([ el('span', v.fwText) ]);
  if (v.fwSub)
    fwTd.appendChild(sub(v.fwSub));
  var cfg = el('button', 'Configure', 'btn btn-outline btn-sm');
  cfg.setAttribute('type', 'button');
  cfg.addEventListener('click', function(ev) {
    if (ev && ev.stopPropagation)
      ev.stopPropagation();
    openDialog(ch.id);
  });
  tr.appendChild(cell([ el('b', ch.label), sub(ch.id, 'mono') ]));
  tr.appendChild(fwTd);
  tr.appendChild(modeTd);
  tr.appendChild(cell([ el('span', v.state.text, 'chip' + (v.state.tone ? ' ' + v.state.tone : '')) ]));
  tr.appendChild(cell([ sub(v.activity) ]));
  tr.appendChild(cell([ cfg ]));
  tr.addEventListener('click', function() { openDialog(ch.id); });
  return tr;
}

/* Where Thread runs: DeviceCommon.threadChip ("Thread runs on" only while it runs, else a warning with the state). */
function renderChip() {
  var chip = $('radios-chip');
  var id = status ? status.otbr_channel : undefined;
  var c = status ? C.threadChip(status, id ? L.labelOf(snap, id) : '') : null;
  if (!c) {
    chip.classList.add('hidden');
    return;
  }
  chip.classList.remove('hidden');
  chip.textContent = c.text;
  chip.className = 'chip ' + c.tone;
}

function anyStarting() {
  return !!snap && snap.channels.some(function(c) { return c.state === 'starting'; });
}

/*
 * After a stop of Thread was accepted (202 for another mode on its radio), says how it ended: stopped once the radio
 * stores the new mode; refused when it stays in otbr with a reason (drain_timeout: Thread runs on); else after 90 s.
 */
function settleStop() {
  var ch = L.findChannel(snap, moving.to);
  var name = L.labelOf(snap, moving.to);
  if (ch && ch.stored_mode === moving.mode) {
    note('rMsg', 'Thread stopped on ' + name + '.', false);
  } else if ((ch && ch.stored_mode === 'otbr' && ch.error) || env.now() - moving.at > MOVE_GIVE_UP_MS) {
    note('rMsg', 'Thread did not stop on ' + name + (ch && ch.error ? ' (' + ch.error + ')' : '') +
                     ': it keeps running there.', true);
  } else {
    return;
  }
  moving = null;
}

/*
 * After a move (or a start) was accepted, says how it ended. It failed only when the target was seen starting (or
 * faulted) and then settled without Thread running on it, or when 90 s passed without either: the first answers after
 * the 202 may still show the old picture.
 */
function settleMove() {
  if (!moving)
    return;
  if (moving.stop) {
    settleStop();
    return;
  }
  var to = L.findChannel(snap, moving.to);
  var name = L.labelOf(snap, moving.to);
  if (to && (to.state === 'starting' || to.state === 'fault'))
    moving.seen = true;
  // A move commits before it starts (otbr_channel names the target from then on): it worked once Thread runs there.
  var ok = !!status && status.otbr_channel === moving.to &&
           (status.mode === 'BR_RUNNING' || status.mode === 'SNIFFER');
  // Not committed (open sessions did not end in time): Thread runs on where it was. The channel list decides: the target
  // is stored otbr once the move committed, whatever a status read (failed, or late) still says.
  var kept = !!to && to.state === 'fault' && to.stored_mode !== 'otbr' && !!status && !!moving.from &&
             status.otbr_channel === moving.from;
  if (ok && !anyStarting()) {
    note('rMsg', 'Thread now runs on ' + name + '.', false);
  } else if (kept && !anyStarting()) {
    note('rMsg', 'Thread did not move' + (to.error ? ' (' + to.error + ')' : '') + ': it still runs on ' +
                     L.labelOf(snap, moving.from) + '. Try the move again.',
         true);
  } else if ((moving.seen && !anyStarting()) || env.now() - moving.at > MOVE_GIVE_UP_MS) {
    note('rMsg', 'Thread did not start on ' + name + (to && to.error ? ' (' + to.error + ')' : '') +
                     '. Thread stays on it, in fault: save otbr on it again to retry, or move Thread to another radio.',
         true);
  } else {
    return;
  }
  moving = null;
}

function render() {
  var body = $('rows');
  clear(body);
  snap.channels.forEach(function(ch) { body.appendChild(renderRow(ch)); });
  renderChip();
  if (typeof navSetThreadMenuHidden === 'function' && status && status.otbr_channel !== undefined)
    navSetThreadMenuHidden(status.otbr_channel === null);
  if (openId && !L.findChannel(snap, openId))
    closeDialog();
  settleMove();
  if (anyStarting() && fastTimer === null) {
    fastTimer = env.setTimeout(function() {
      fastTimer = null;
      if (poll)
        poll.wake();
    }, FAST_MS);
  }
}

function refresh() {
  return kit.request('GET', '/device/channels', {timeoutMs : 5000}).then(function(r) {
    if (r.status !== 200 || !r.body || !Array.isArray(r.body.channels)) {
      setText('radios-stale', 'The radio list could not be read: ' + kit.failure(r));
      show('radios-stale', true);
      return false;
    }
    snap = r.body;
    show('radios-stale', false);
    return kit.request('GET', '/device/status', {timeoutMs : 5000}).then(function(s) {
      if (s.status === 200 && s.body)
        status = s.body;
      render();
      return true;
    }, function() {
      render();
      return true;
    });
  }, function(e) {
    setText('radios-stale', e.message);
    show('radios-stale', true);
    return false;
  });
}

/* ---- the dialog ---- */

function current() { return openId ? L.findChannel(snap, openId) : null; }

function setSeg(prefix, values, picked) {
  values.forEach(function(v) { $(prefix + '-' + v).setAttribute('aria-pressed', v === picked ? 'true' : 'false'); });
}

function renderCards(ch) {
  var track = $('track');
  clear(track);
  var dots = $('dots');
  clear(dots);
  L.MODES.forEach(function(m) {
    var why = L.cardWhy(m.id, ch, snap);
    var b = el('button', null, 'rd-card');
    b.setAttribute('type', 'button');
    b.setAttribute('data-m', m.id);
    b.setAttribute('aria-pressed', m.id === draft.pick ? 'true' : 'false');
    b.disabled = !!why;
    b.appendChild(icon(m.icon));
    b.appendChild(el('b', m.name));
    b.appendChild(el('span', m.desc));
    if (why)
      b.appendChild(el('span', why, 'why'));
    b.addEventListener('click', function() {
      if (b.disabled)
        return;
      draft.pick = m.id;
      draft.touched = true;
      renderDialog();
    });
    track.appendChild(b);
    dots.appendChild(el('i', null, m.id === draft.pick ? 'on' : ''));
  });
}

function moveText(ch) {
  var from = L.threadElsewhere(snap, ch, status);
  if (!from)
    return 'Saving starts the Thread border router on this radio.';
  return 'Saving moves Thread from ' + from.label + ' to this radio. Thread devices lose the border router for a ' +
         'few seconds; the network and its settings stay the same. If this radio does not start, Thread stays on ' +
         'this radio in fault (it does not switch back): save otbr here again to retry, or move it to another radio.';
}

function renderDialog() {
  var ch = current();
  if (!ch)
    return;
  renderCards(ch);
  var moveHere = draft.pick === 'otbr' && ch.mode !== 'otbr';
  show('panelOtbr', moveHere);
  if (moveHere)
    setText('otbrNote', moveText(ch));
  show('panelProxy', draft.pick === 'rcp-proxy');
  setSeg('dir', [ 'listen', 'connect' ], draft.direction);
  setSeg('ad', [ 'tcp', 'udp' ], draft.adapter);
  setSeg('fr', [ 'hdlc', 'raw' ], draft.framing);
  show('hostField', draft.direction === 'connect');
  setText('portLabel', draft.direction === 'connect' ? 'Remote port' : 'Local port');
  show('framingField', draft.adapter === 'udp');
  setText('proxyHint', draft.direction === 'connect'
                           ? 'The board dials out and retries every 1 to 30 s while the host is away.'
                           : (draft.adapter === 'udp'
                                  ? 'The first address that sends becomes the peer; others are dropped until it has been silent for 30 s.'
                                  : 'The first client to connect keeps the radio until it disconnects.'));
  setText('mSave', moveHere && L.threadElsewhere(snap, ch, status) ? 'Move Thread here' : 'Save');
}

function fillBauds(ch) {
  var sel = $('baudSel');
  clear(sel);
  var bauds = L.BAUDS.slice();
  if (bauds.indexOf(ch.line.baud) < 0)
    bauds.push(ch.line.baud);
  bauds.forEach(function(b) {
    var o = el('option', b);
    o.value = String(b);
    sel.appendChild(o);
  });
  sel.value = String(ch.line.baud);
}

function openDialog(id) {
  var ch = L.findChannel(snap, id);
  if (!ch)
    return;
  openId = id;
  var p = L.proxyOf(ch);
  draft = {pick : L.currentMode(ch), touched : false, direction : p.direction, adapter : p.adapter, framing : p.framing};
  setText('mTitle', ch.label);
  setText('mSub', L.subtitle(ch));
  var gone = !L.present(ch);
  show('mUnavailable', gone);
  setText('mUnavailable', gone ? 'Unplugged. Changes are stored and apply when it is plugged in again; the mode can ' +
                                     'only be changed while it is plugged in.'
                               : '');
  show('mForget', gone);
  show('panelLine', !!ch.usb);
  if (ch.usb) {
    fillBauds(ch);
    var canFlow = !!(ch.caps && ch.caps.flow_control);
    $('flowIn').disabled = !canFlow;
    $('flowIn').checked = canFlow && !!ch.line.flow_control;
    setText('flowHint', canFlow ? 'CP210x bridges only; needed by EFR32 radios at 460800.' : 'Not supported by this USB bridge.');
  }
  $('hostIn').value = p.host;
  $('portIn').value = String(p.port);
  note('mMsg', '', false);
  renderDialog();
  var dlg = $('dlg');
  if (!dlg.open)
    dlg.showModal();
}

function closeDialog() {
  openId = null;
  var dlg = $('dlg');
  if (dlg.open)
    dlg.close();
}

/* The dialog's fields, read at the moment of Save. */
function readDraft(ch) {
  draft.host = $('hostIn').value;
  draft.port = $('portIn').value;
  if (ch.usb) {
    draft.baud = $('baudSel').value;
    draft.flow = $('flowIn').checked;
  }
  return draft;
}

/* PUTs the bodies one after the other; resolves to the replies, in order. A refusal or a lost connection ends it. */
function putAll(id, bodies) {
  var replies = [];
  var chain = Promise.resolve();
  bodies.forEach(function(body) {
    chain = chain.then(function() {
      if (replies.length && replies[replies.length - 1].status !== 200 && replies[replies.length - 1].status !== 202)
        return;
      return kit.request('PUT', '/device/channels/' + encodeURIComponent(id),
                         {headers : {'Content-Type' : 'application/json'}, body : JSON.stringify(body), timeoutMs : 20000})
          .then(function(r) { replies.push(r); }, function(e) { replies.push({status : 0, body : null, message : e.message}); });
    });
  });
  return chain.then(function() { return replies; });
}

function save() {
  var ch = current();
  if (!ch || kit.isBusy())
    return Promise.resolve(null);
  var p = L.plan(ch, snap, readDraft(ch), status);
  if (p.error) {
    note('mMsg', p.error, true);
    return Promise.resolve(null);
  }
  if (!p.requests.length) {
    closeDialog();
    return Promise.resolve(null);
  }
  var from = L.threadElsewhere(snap, ch, status);
  return kit.runOp(function() {
    note('mMsg', 'Saving...', false);
    return putAll(ch.id, p.requests).then(function(replies) {
      var last = replies[replies.length - 1];
      var failed = last.status !== 200 && last.status !== 202;
      var rebooting = replies.some(function(r) { return r.body && r.body.reboot; });
      if (failed) {
        // What went through before the refusal is said first (settings stored, the radio switched off).
        note('mMsg', L.saveFailureText(p.requests, replies), true);
      } else {
        closeDialog();
        if (last.status === 202) {
          var mode = p.requests[p.requests.length - 1].mode;
          var stop = mode !== 'otbr'; // another mode on the radio that runs Thread
          moving = {from : stop ? ch.id : (last.body && last.body.moved_from ? last.body.moved_from : (from && from.id)),
                    to : ch.id, at : env.now(), seen : false, stop : stop, mode : mode};
          if (stop)
            note('rMsg', 'Stopping Thread on ' + ch.label + ': its sessions end first, then the radio changes mode.',
                 false);
          else if (moving.from)
            note('rMsg', 'Moving Thread from ' + L.labelOf(snap, moving.from) + ' to ' + ch.label +
                             ': Thread devices lose the border router for a few seconds.', false);
          else
            note('rMsg', 'Starting Thread on ' + ch.label + '.', false);
        } else if (!rebooting) {
          note('rMsg', 'Saved ' + ch.label + '.', false);
        }
      }
      return refresh().then(function() {
        if (rebooting && !failed) {
          note('rMsg', 'Saved. The board is rebooting to apply the change; this page reloads when it is back.', false);
          kit.waitForBoard('rMsg');
          return {handoff : true};
        }
        return last;
      });
    });
  });
}

function forget() {
  var ch = current();
  if (!ch || kit.isBusy())
    return Promise.resolve(null);
  if (!env.confirm('Forget ' + ch.label + '? Its stored settings are deleted.'))
    return Promise.resolve(null);
  return kit.runOp(function() {
    return kit.request('DELETE', '/device/channels/' + encodeURIComponent(ch.id), {timeoutMs : 10000}).then(function(r) {
      if (r.status !== 204 && r.status !== 200) {
        note('mMsg', L.errorText(r.status, r.body), true);
        return r;
      }
      closeDialog();
      note('rMsg', 'Forgot ' + ch.label + '.', false);
      return refresh().then(function() { return r; });
    }, function(e) { note('mMsg', e.message, true); });
  });
}

function wireSeg(prefix, key, values) {
  values.forEach(function(v) {
    $(prefix + '-' + v).addEventListener('click', function() {
      draft[key] = v;
      renderDialog();
    });
  });
}

function init() {
  wireSeg('dir', 'direction', [ 'listen', 'connect' ]);
  wireSeg('ad', 'adapter', [ 'tcp', 'udp' ]);
  wireSeg('fr', 'framing', [ 'hdlc', 'raw' ]);
  $('mSave').addEventListener('click', save);
  $('mForget').addEventListener('click', forget);
  $('mCancel').addEventListener('click', closeDialog);
  $('mClose').addEventListener('click', closeDialog);
  $('dlg').addEventListener('close', function() { openId = null; });
  $('dlg').addEventListener('click', function(ev) {
    if (ev.target === $('dlg'))
      closeDialog();
  });
  [ [ 'carPrev', -180 ], [ 'carNext', 180 ] ].forEach(function(b) {
    $(b[0]).addEventListener('click', function() {
      if ($('track').scrollBy)
        $('track').scrollBy({left : b[1], behavior : 'smooth'});
    });
  });
  kit.start();
  poll = kit.poller(refresh, 5000, 'radiosSection');
}

return {init : init, refresh : refresh, save : save, forget : forget, kit : kit};
}

if (typeof module !== 'undefined' && module.exports) {
  module.exports = RadiosLogic;
  module.exports.Page = RadiosPage;
}

if (typeof document !== 'undefined')
  RadiosPage(DeviceCommon.DeviceEnv()).init();
