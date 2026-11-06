/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP Thread Border Router - Dashboard, index.html: the status page (/device/status, /device/diag, /device/reboot,
 * and the Thread web GUI's /get_properties and /ipaddr while the border router runs)
 *
 * StatusLogic is pure and runs under node too (tools/tests/test_status_page.py). StatusPage is the page.
 */
'use strict';

var StatusLogic = (function() {

function faultText(reason) {
  if (reason === 'no rcp firmware')
    return 'The radio has no firmware, so the Thread network cannot start. Install c6_rcp.bin on the Firmware page.';
  if (reason === 'rcp incompatible')
    return 'The radio firmware does not match this build, so the Thread network cannot start. Install the ' +
           'c6_rcp.bin of the same release.';
  if (reason === 'esp-hosted firmware')
    return 'The radio still runs its factory ESP-Hosted firmware. On the Firmware page, install c6_installer.bin ' +
           'once, then c6_rcp.bin of the same release.';
  if (reason === 'rom recovery failed')
    return 'Rewriting the radio over the maintenance jumper failed. Reboot the board, or use Recover radio on the ' +
           'Firmware page.';
  if (reason === 'storage unavailable')
    return 'The settings storage did not start, so the Thread network cannot start. The rest of the board works; ' +
           'see Troubleshooting in the documentation.';
  return 'The radio stopped answering' + (reason ? ' (' + reason + ')' : '') +
         '. Updating or recovering the radio firmware may help.';
}

/** The banner of the page: {tone: ok|info|fault|idle, title, text, link: {href, label} | null}. */
function heroFor(s) {
  if (s.mode === 'BR_RUNNING')
    return {
      tone : 'ok',
      title : 'Border router running',
      text : 'The Thread stack is up and the radio firmware matches this build.',
      link : null
    };
  if (s.mode === 'SNIFFER')
    return {
      tone : 'info',
      title : 'Capturing packets',
      text : 'Channel ' + s.sniffer.channel + ', ' + s.sniffer.frames +
                 ' frames so far. The Thread network is stopped until the capture ends.',
      link : {href : '/capture.html', label : 'Open packet capture'}
    };
  var dongle = !!s.otbr_channel && s.otbr_channel !== 'c6'; // Thread's radio is a USB stick: nothing to update here
  if (s.mode === 'RADIO_FAULT')
    return {
      tone : 'fault',
      title : 'Radio fault',
      text : faultText(s.fault_reason),
      link : dongle ? {href : '/radios.html', label : 'Open Radios'} : {href : '/firmware.html', label : 'Open firmware'}
    };
  if (s.mode === 'NO_RADIO')
    return {
      tone : 'idle',
      title : 'No Thread radio',
      text : 'No radio runs the Thread border router' + (s.fault_reason ? ' (' + s.fault_reason + ')' : '') +
                 '. Choose or plug in its radio on the Radios page.',
      link : {href : '/radios.html', label : 'Open Radios'}
    };
  if (s.mode === 'STARTING')
    return {
      tone : 'idle',
      title : 'Starting',
      text : 'The Thread stack is starting. This takes a few seconds after a boot or a radio update.',
      link : null
    };
  return {tone : 'idle', title : String(s.mode), text : '', link : null};
}

/** The Dashboard card's state line for a /device/zigbee body (null when it could not be read). */
function zigbeeStateText(z) {
  if (!z)
    return 'unknown';
  if (z.state === 'running')
    return z.client ? 'Running, ZHA connected' : 'Waiting for Home Assistant';
  if (z.state === 'no_radio')
    return 'No radio';
  if (z.state === 'starting')
    return 'Starting';
  if (z.state === 'incompatible')
    return 'Radio not compatible';
  return 'Not running';
}

function snifferText(sn) {
  if (sn.active)
    return {state : 'Capturing on channel ' + sn.channel, detail : sn.frames + ' frames, ' + sn.drops + ' dropped'};
  if (sn.ended_by_fault)
    return {state : 'Idle', detail : 'The last capture was ended by a radio fault'};
  if (sn.frames)
    return {state : 'Idle', detail : 'Last capture: ' + sn.frames + ' frames, ' + sn.drops + ' dropped'};
  return {state : 'Idle', detail : 'No capture since boot'};
}

function pdText(pd) {
  return {
    state : pd.state === null || pd.state === undefined ? 'unknown (OpenThread was busy)' : String(pd.state),
    prefix : pd.prefix || 'None received'
  };
}

var NOT_JOINED = {disabled : true, detached : true, '' : true};

/** RLOC16 of an RLOC address: its last 16 bits as "0x7800"; "-" without one ("::" while detached). */
function rloc16Of(address) {
  if (!address || address === '-' || address === '::')
    return '-';
  var last = String(address).split(':').pop();
  if (!/^[0-9a-fA-F]{1,4}$/.test(last))
    return '-';
  return '0x' + ('000' + last.toLowerCase()).slice(-4);
}

/** The Thread cards from the result of /get_properties; "-" for a field the answer lacks. */
function threadView(r) {
  function f(k) { return r[k] === undefined || r[k] === null || r[k] === '' ? '-' : String(r[k]); }
  var role = r['Thread:Role'] === undefined ? '' : String(r['Thread:Role']);
  return {
    name : f('Network:Name'),
    role : f('Thread:Role'),
    channel : f('RCP:Channel'),
    txpower : f('RCP:TxPower'),
    panid : f('Network:PANID'),
    xpanid : f('Network:XPANID'),
    partition : f('Network:PartitionID'),
    otver : f('OpenThread:Version'),
    eui64 : f('RCP:EUI64'),
    baid : f('Network:BorderAgentID'),
    pskc : f('OpenThread:PSKc'),
    rloc : f('IPv6:RoutingLocalAddress'),
    rloc16 : rloc16Of(r['IPv6:RoutingLocalAddress']),
    joined : !NOT_JOINED[role]
  };
}

/** The banner's Thread line: "Leader of MyHome on channel 15", or "" outside a network. */
function roleLine(v) {
  if (!v.joined)
    return '';
  return v.role.charAt(0).toUpperCase() + v.role.slice(1) + ' of ' + v.name + ' on channel ' + v.channel;
}

/** A secret shown as 8 dots, whatever its length ("-" stays). */
function maskSecret(s) { return s === '-' ? s : '\u2022\u2022\u2022\u2022\u2022\u2022\u2022\u2022'; }

/*
 * The Home Assistant tile from /device/status, /device/network and /device/names (null when not read):
 * {url, mdns, haName, iface, note}. Home Assistant must use the Thread backbone address: in split mode the management
 * interface refuses Thread changes and is never announced over mDNS.
 */
function haView(s, net, names) {
  var split = !!(net && net.mode === 'split');
  var name = split ? 'thread_bb' : 'eth';
  var iface = (net && net.interfaces || []).filter(function(i) { return i.name === name; })[0] || null;
  var addr = iface ? iface.ipv4 : null; // no network snapshot: no address rather than a stale or wrong one
  var host = names && names.hostname_live ? names.hostname_live + '.local' : '';
  var cfg = net && net.active && net.active[split ? 'thread_bb' : 'single'];
  var dhcp = !!(cfg && cfg.ipv4 && cfg.ipv4.dhcp);
  var notes = [];
  if (s.mode !== 'BR_RUNNING')
    notes.push('Home Assistant can use the board once the border router runs.');
  if (split)
    notes.push('Home Assistant must reach the Thread backbone address; the management address refuses Thread changes.');
  else
    notes.push('Home Assistant finds the board by mDNS when both are on the same network segment.');
  if (addr && dhcp)
    notes.push('The address comes from DHCP: give it a reservation so the URL stays valid.');
  return {
    url : addr ? 'http://' + addr : host ? 'http://' + host : '-',
    mdns : host || '-',
    haName : names && names.ha_name_effective ? names.ha_name_effective : '-',
    iface : split ? 'Thread backbone (VLAN ' + (iface ? iface.vlan : '?') + ')' : 'Ethernet port',
    note : notes.join(' ')
  };
}

/** Rows of the Thread address table from the result of /ipaddr. */
function addressRows(list) {
  return list.map(function(a) {
    return {address : a.address + '/' + a.prefixLength, origin : String(a.origin), state : a.preferred ? 'preferred' : 'deprecated'};
  });
}

return {
  heroFor : heroFor,
  snifferText : snifferText,
  zigbeeStateText : zigbeeStateText,
  pdText : pdText,
  threadView : threadView,
  roleLine : roleLine,
  maskSecret : maskSecret,
  haView : haView,
  addressRows : addressRows
};
})();

function StatusPage(env) {
var C = typeof DeviceCommon !== 'undefined' ? DeviceCommon : require('./device_common.js');
var L = StatusLogic;
var kit = C.DeviceKit(env, {
  actionIds : [ 'rebootBtn' ],
  trial : false,
  refreshNetwork : function() { return refreshNetwork(); }
});
var $ = kit.$, setText = kit.setText, show = kit.show, note = kit.note;
var lastSeen = 0;
var lastMode = null;   // mode of the last status answer: the Thread data is only asked for in BR_RUNNING
var threadPoll = null;
var pskc = '-';
var pskcShown = false;
var lastStatus = null; // for the Home Assistant tile, which also needs /device/network
var lastNet = null;
var lastNames = null;
var lastZigbee; // undefined until the first answer
var zigbeePoll = null;

function rcpText(installed) {
  if (installed === null || installed === undefined)
    return 'unknown (probe failed)';
  return installed === '' ? 'none (no valid RCP firmware)' : String(installed);
}

/* "Thread runs on: <label>": the label comes from /device/channels, asked once per radio the status names. */
var channelLabels = {};
var labelAsked = null;

function renderThreadOn(s) {
  var chip = $('h-thread-on');
  var id = s.otbr_channel;
  if (id === undefined) {
    chip.classList.add('hidden');
    return;
  }
  chip.classList.remove('hidden');
  if (id === null) {
    chip.textContent = 'Thread runs on: no radio';
    chip.className = 'chip chip-warn';
    return;
  }
  chip.textContent = 'Thread runs on: ' + (channelLabels[id] || id);
  chip.className = 'chip chip-ok';
  if (!channelLabels[id] && labelAsked !== id) {
    labelAsked = id;
    kit.request('GET', '/device/channels', {timeoutMs : 5000}).then(function(r) {
      if (r.status !== 200 || !r.body || !r.body.channels)
        return;
      r.body.channels.forEach(function(c) { channelLabels[c.id] = c.label; });
      if (lastStatus)
        renderThreadOn(lastStatus);
    }, function() {});
  }
}

function render(s) {
  var wasRunning = lastMode === 'BR_RUNNING';
  lastMode = s.mode;
  if (s.mode === 'BR_RUNNING' && !wasRunning && threadPoll)
    threadPoll.wake();
  if (s.mode !== 'BR_RUNNING')
    threadOff();
  lastSeen = env.now();
  show('s-offline', false);
  kit.renderThreadNotice(s, null, true);
  renderThreadOn(s);
  var h = L.heroFor(s);
  $('hero').className = 'card hero hero-' + h.tone;
  setText('h-title', h.title);
  setText('h-text', h.text);
  show('h-link', !!h.link);
  if (h.link) {
    $('h-link').setAttribute('href', h.link.href);
    setText('h-link', h.link.label);
  }
  setText('h-mode', s.mode);
  setText('t-app', s.app.version);
  setText('t-slot', 'Slot ' + s.app.slot);
  setText('t-storage', s.storage_ok === undefined ? '-' : s.storage_ok ? 'OK' : 'Failed: settings are not saved');
  setText('t-rcp', rcpText(s.rcp.installed));
  setText('t-compat', C.compatText(s.rcp));
  var j = s.jumper;
  setText('t-jumper', j.present ? (j.recovery_failed ? 'Fitted, last recovery failed' : 'Fitted') : 'Not fitted');
  setText('t-base', !j.present ? '-'
                    : !j.base_installed ? 'unknown (expected ' + j.base_expected + ')'
                    : j.base_installed === j.base_expected ? j.base_installed + ', as expected'
                    : j.base_installed + ' (expected ' + j.base_expected + ')');
  setText('t-ipv4', s.eth.ipv4 || '-');
  setText('t-ipv6', s.eth.ipv6.length ? s.eth.ipv6.join('\n') : '-');
  var pd = L.pdText(s.pd);
  setText('t-pd-state', pd.state);
  setText('t-pd-prefix', pd.prefix);
  var sn = L.snifferText(s.sniffer);
  setText('t-sniffer-state', sn.state);
  setText('t-sniffer-detail', sn.detail);
  lastStatus = s;
  renderZigbee(s);
  renderHa();
}

function renderHa() {
  if (!lastStatus)
    return;
  var v = L.haView(lastStatus, lastNet, lastNames);
  setText('t-ha-url', v.url);
  setText('t-ha-mdns', v.mdns);
  setText('t-ha-name', v.haName);
  setText('t-ha-iface', v.iface);
  setText('t-ha-hint', v.note);
}

/* /device/network (the trial banner) and /device/names: the Home Assistant tile. */
function refreshNetwork() {
  return kit.request('GET', '/device/network', {timeoutMs : 5000}).then(function(r) {
    if (r.status !== 200 || !r.body) {
      lastNet = null;
      renderHa();
      return false;
    }
    lastNet = r.body;
    kit.renderTrial(r.body);
    renderHa();
    return kit.request('GET', '/device/names', {timeoutMs : 5000}).then(function(n) {
      if (n.status === 200 && n.body)
        lastNames = n.body;
      renderHa();
      return true;
    }, function() { return true; });
  }, function() {
    lastNet = null;
    renderHa();
    return false;
  });
}

/* ---- Zigbee card: only while a radio channel runs the Zigbee coordinator (zigbee_channel in /device/status) ---- */

function renderZigbee(s) {
  show('t-zg-card', !!s.zigbee_channel);
  if (!s.zigbee_channel)
    return;
  setText('t-zg-radio', s.zigbee_channel === 'c6' ? 'on-board C6' : 'USB radio ' + s.zigbee_channel);
  if (lastZigbee === undefined && zigbeePoll)
    zigbeePoll.wake();
}

function renderZigbeeBody(z) {
  lastZigbee = z;
  setText('t-zg-state', L.zigbeeStateText(z));
  setText('t-zg-channel', z && z.radio ? String(z.radio.channel) : '-');
  setText('t-zg-client', z && z.client ? z.client.address : '-');
}

function refreshZigbee() {
  if (!lastStatus || !lastStatus.zigbee_channel)
    return Promise.resolve(true);
  return kit.request('GET', '/device/zigbee', {timeoutMs : 5000}).then(function(r) {
    var ok = r.status === 200 && !!r.body;
    renderZigbeeBody(ok ? r.body : null);
    return ok;
  }, function() {
    renderZigbeeBody(null);
    return false;
  });
}

function showOffline() {
  setText('s-offline', C.lastSeenText(lastSeen, env.now()));
  show('s-offline', true);
}

function refreshStatus() {
  return kit.request('GET', '/device/status', {timeoutMs : 5000}).then(function(r) {
    if (r.status === 200 && r.body) {
      render(r.body);
      return true;
    }
    showOffline();
    return false;
  }, function() {
    showOffline();
    return false;
  });
}

function diagDashes() {
  [ 'h-uptime', 'h-reset', 't-cpu', 't-mem-int', 't-mem-psram' ].forEach(function(id) { setText(id, '-'); });
  return false;
}

function refreshDiag() {
  return kit.request('GET', '/device/diag', {timeoutMs : 5000}).then(function(r) {
    if (r.status !== 200 || !r.body)
      return diagDashes();
    var d = r.body;
    kit.noteUptime(d.uptime_s);
    setText('h-uptime', C.durationText(d.uptime_s));
    setText('h-reset', d.reset_reason);
    setText('t-cpu', d.cpu.map(function(c, i) {
      return (i ? 'core ' : 'Core ') + c.core + ' ' + C.pct(c.load_pct['10s']);
    }).join(', '));
    var mi = d.memory.internal;
    setText('t-mem-int', C.bytesText(mi.free) + ' (lowest ' + C.bytesText(mi.min_free) + ')');
    setText('t-mem-psram', C.bytesText(d.memory.psram.free));
    return true;
  }, diagDashes);
}

/* ---- Thread network (the Thread web GUI answers {error, result, message}; 503 outside BR_RUNNING) ---- */

var THREAD_FIELDS = [
  't-net-name', 't-net-role', 't-net-rloc16', 't-net-channel', 't-net-txpower', 't-net-panid', 't-net-xpanid',
  't-net-partition', 't-id-eui64', 't-id-baid', 't-id-rloc', 't-id-otver'
];

/* Outside BR_RUNNING there is no Thread data: nothing from the last network stays on screen. */
function threadOff() {
  show('t-net-off', true);
  show('t-net-none', false);
  setText('h-role', '');
  THREAD_FIELDS.forEach(function(id) { setText(id, '-'); });
  pskc = '-';
  showPskc();
  kit.clear($('addr-body'));
}

function showPskc() {
  setText('t-id-pskc', pskcShown ? pskc : L.maskSecret(pskc));
  setText('pskcBtn', pskcShown ? 'Hide' : 'Show');
  $('pskcBtn').setAttribute('aria-pressed', pskcShown ? 'true' : 'false');
}

function renderThread(r) {
  var v = L.threadView(r);
  show('t-net-off', false);
  show('t-net-none', !v.joined);
  setText('h-role', L.roleLine(v));
  setText('t-net-name', v.name);
  setText('t-net-role', v.role);
  setText('t-net-rloc16', v.rloc16);
  setText('t-id-rloc', v.rloc);
  setText('t-net-channel', v.channel);
  setText('t-net-txpower', v.txpower);
  setText('t-net-panid', v.panid);
  setText('t-net-xpanid', v.xpanid);
  setText('t-net-partition', v.partition);
  setText('t-id-eui64', v.eui64);
  setText('t-id-baid', v.baid);
  setText('t-id-otver', v.otver);
  pskc = v.pskc;
  showPskc();
}

function renderAddresses(list) {
  var body = $('addr-body');
  kit.clear(body);
  L.addressRows(list).forEach(function(a) {
    var tr = kit.el('tr');
    var th = kit.el('th', a.address, 'mono');
    th.setAttribute('scope', 'row');
    tr.appendChild(th);
    var origin = kit.el('td');
    origin.appendChild(kit.el('span', a.origin, 'chip'));
    tr.appendChild(origin);
    var state = kit.el('td');
    state.appendChild(kit.el('span', a.state, a.state === 'preferred' ? 'chip chip-ok' : 'chip chip-bad'));
    tr.appendChild(state);
    body.appendChild(tr);
  });
}

function guiResult(url) {
  return kit.request('GET', url, {timeoutMs : 5000}).then(function(r) {
    return r.status === 200 && r.body && !r.body.error && r.body.result ? r.body.result : null;
  }, function() { return null; });
}

function refreshThread() {
  if (lastMode !== 'BR_RUNNING')
    return Promise.resolve(true); // asked for again when the status says it runs
  // The mode can change while a request runs: a late answer from a stopped network is dropped.
  return guiResult('/get_properties').then(function(props) {
    if (lastMode !== 'BR_RUNNING')
      return true;
    if (props)
      renderThread(props);
    return guiResult('/ipaddr').then(function(list) {
      if (lastMode !== 'BR_RUNNING')
        return true;
      if (list)
        renderAddresses(list);
      else
        kit.clear($('addr-body')); // no stale addresses from an earlier network
      return !!(props && list);
    });
  });
}

function reboot() {
  if (kit.isBusy() || !env.confirm('Reboot the board now?'))
    return Promise.resolve(null);
  return kit.runOp(function() {
    return kit.request('POST', '/device/reboot').then(function(r) {
      if (r.status !== 200) {
        note('rebootMsg', kit.failure(r), true);
        return r;
      }
      note('rebootMsg', 'Rebooting; this page reloads when the board is back.', false);
      kit.waitForBoard('rebootMsg');
      return {handoff : true};
    }, function(e) { note('rebootMsg', e.message, true); });
  });
}

function init() {
  $('rebootBtn').addEventListener('click', reboot);
  $('pskcBtn').addEventListener('click', function() {
    pskcShown = !pskcShown;
    showPskc();
  });
  env.setInterval(function() {
    if (!$('s-offline').classList.contains('hidden'))
      showOffline();
  }, 1000);
  kit.start();
  kit.poller(refreshStatus, 3000, 'statusSection');
  kit.poller(refreshDiag, 10000, 'statusSection');
  threadPoll = kit.poller(refreshThread, 10000, 'statusSection');
  kit.poller(refreshNetwork, 10000, null);
  zigbeePoll = kit.poller(refreshZigbee, 5000, 'statusSection');
}

return {
  init : init,
  refreshStatus : refreshStatus,
  refreshDiag : refreshDiag,
  refreshThread : refreshThread,
  refreshNetwork : refreshNetwork,
  refreshZigbee : refreshZigbee,
  reboot : reboot,
  kit : kit
};
}

if (typeof module !== 'undefined' && module.exports) {
  module.exports = StatusLogic;
  module.exports.Page = StatusPage;
}

if (typeof document !== 'undefined')
  StatusPage(DeviceCommon.DeviceEnv()).init();
