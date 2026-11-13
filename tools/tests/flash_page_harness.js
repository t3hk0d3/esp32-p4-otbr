'use strict';
/*
 * Test harness for docs/assets/flasher.js (used by test_flash_page.py, run with node): fakes for the serial port,
 * esptool-js and fetch that write what happens to one ordered event list.
 */
const path = require('path');
const zlib = require('zlib');

const Flasher = require(path.join(__dirname, '..', '..', 'docs', 'assets', 'flasher.js'));

const BOARD = 'ESP32-P4-WIFI6-POE-ETH';
const OTHER_BOARD = 'Second-Board';
const V1 = 'ESP32-P4-rev1.3';
const V3 = 'ESP32-P4-rev3.0';

function target(board, chip) {
  const v1 = chip === V1;
  return {
    board, board_name: board === BOARD ? 'Waveshare ' + BOARD : 'Second board',
    chip, chip_name: v1 ? 'ESP32-P4 revision v1.x' : 'ESP32-P4 revision v3.1 and later',
    min_rev: v1 ? 0 : 301, max_rev: v1 ? 199 : 399,
    manifest: board + '/' + chip + '/manifest.json', version: 'v9.9.9',
  };
}

const TARGETS = [target(BOARD, V1), target(BOARD, V3)];
const SEVERAL_BOARDS = TARGETS.concat([target(OTHER_BOARD, V1)]);
const PARTS = [
  {path: 'bootloader.bin', offset: 0x2000, size: 100},
  {path: 'p4.bin', offset: 0x100000, size: 300},
];

const RCP_VERSION = '20261113T000000Z-abcdef0';
const BASE_VERSION = 'base-1a2b3c4d';
const GOT_IP = 'I (4210) eth: got ip 192.168.1.50\r\n';

/* An ESP-IDF app image of `size` bytes with an app description (project, version). */
function appImage(project, version, size) {
  const data = new Uint8Array(size);
  data[0] = 0xE9;
  new DataView(data.buffer).setUint32(32, 0xABCD5432, true);
  data.set(new TextEncoder().encode(version), 48);
  data.set(new TextEncoder().encode(project), 80);
  for (let i = 112; i < size; i++)
    data[i] = (i * 7) & 0xff;
  return data;
}

/*
 * A P4 running the radio-fw console command (docs/reference/radio-console.md), scripted by `b`:
 *   state ('esp_hosted', 'updater', 'fault'), installed ('-'), base (BASE_VERSION), probing (status answers "probing"
 *   this many times first), statusSilent, loadError (text after "error " for every load), nakAt / dropAckAt (the
 *   first frame at this offset is answered nak crc / not at all), silentFrames, lose ('checking': at the status,
 *   'sending': after the first frame, 'write': the first frame's write throws, 'installing': after received),
 *   silentAfterReceived, busy (busy lines, default 2),
 *   readyMax (the max in its ready line), installerResult / rcpResult (the line after the busy lines; default done), afterReconnect (lines printed when the
 *   port opens again), noLogNoise.
 * Records t.radio: {commands, frames: [{offset, len}], aborts, images: [{project, size, crcOk}]}.
 */
function makeBoard(b, t, log) {
  const board = Object.assign({state: 'esp_hosted', installed: '-', base: BASE_VERSION, probing: 0, busy: 2}, b);
  const queue = [];
  let waiting = null;
  let lost = false;
  let opened = 0;
  let line = '';
  let load = null; /* {size, crc, data, expected, buf} */
  const dropped = new Set();
  const naked = new Set();
  t.radio = {commands: [], frames: [], aborts: 0, images: [], strayFrames: 0};

  const push = (text) => {
    queue.push(new TextEncoder().encode(text));
    if (waiting) {
      const w = waiting;
      waiting = null;
      w.resolve({value: queue.shift(), done: false});
    }
  };
  const reply = (text) => push((board.noLogNoise ? '' : 'I (77) rcp_sync: log noise\r\n') + '@radio-fw ' + text + '\r\n');
  const lose = () => {
    log('port-lost');
    lost = true;
    if (waiting) {
      const w = waiting;
      waiting = null;
      w.reject(new Error('NetworkError: The device has been lost.'));
    }
  };

  function finishLoad() {
    const project = load.data[0] === 0xE9 ? new TextDecoder().decode(load.data.subarray(80, 112)).replace(/\0.*$/, '') : '';
    const crcOk = (zlib.crc32(load.data) >>> 0) === load.crc;
    t.radio.images.push({project, size: load.size, crcOk});
    load = null;
    if (!crcOk) {
      reply('error invalid_request image checksum mismatch');
      return;
    }
    reply('received');
    if (board.lose === 'installing') {
      lose();
      return;
    }
    if (board.silentAfterReceived)
      return;
    for (let i = 1; i <= board.busy; i++)
      reply('busy ' + i * 5);
    const installer = project === 'c6_installer';
    const result = (installer ? board.installerResult : board.rcpResult) || 'done';
    if (result === 'done') {
      board.state = 'updater';
      board.installed = installer ? '-' : RCP_VERSION;
      reply('done installed=' + board.installed);
    } else {
      reply(result);
    }
  }

  function takeFrame(bytes) {
    const view = new DataView(bytes.buffer, bytes.byteOffset);
    const offset = view.getUint32(2, true);
    const len = view.getUint16(6, true);
    const crc = view.getUint32(8 + len, true);
    const crcOk = (zlib.crc32(bytes.subarray(2, 8 + len)) >>> 0) === crc;
    if (len === 0) {
      t.radio.aborts++;
      load = null;
      reply('error aborted the sender stopped');
      return;
    }
    t.radio.frames.push({offset, len, crcOk});
    if (board.lose === 'sending' || (board.loseAfterFrames && t.radio.frames.length === board.loseAfterFrames)) {
      lose();
      return;
    }
    if (board.silentFrames)
      return;
    if (board.nakAt === offset && !naked.has(offset)) {
      naked.add(offset);
      reply('nak ' + load.expected + ' crc');
      return;
    }
    if (offset === load.expected && crcOk) {
      load.data.set(bytes.subarray(8, 8 + len), offset);
      load.expected += len;
    }
    /* a lost ack: the frame is taken, only its answer goes missing */
    if (!(board.dropAckAt === offset && !dropped.has(offset) && dropped.add(offset)))
      reply('ack ' + load.expected);
    if (load.expected === load.size)
      finishLoad();
  }

  function command(text) {
    t.radio.commands.push(text);
    if (text === 'radio-fw status') {
      if (board.lose === 'checking') {
        lose();
      } else if (!board.statusSilent) {
        const state = board.probing-- > 0 ? 'probing' : board.state;
        reply('status state=' + state + ' installed=' + board.installed + ' base=' + board.base + ' jumper=0');
      }
      return;
    }
    const m = /^radio-fw load (\d+) ([0-9a-f]{8})$/.exec(text);
    if (m) {
      if (board.loadError) {
        reply('error ' + board.loadError);
        return;
      }
      const size = Number(m[1]);
      load = {size, crc: parseInt(m[2], 16), data: new Uint8Array(size), expected: 0, buf: new Uint8Array(0)};
      reply('ready max=' + (board.readyMax || 1024));
    }
  }

  function write(bytes) {
    if (!load && bytes[0] === 0x52 && bytes[1] === 0x46) {
      t.radio.strayFrames = (t.radio.strayFrames || 0) + 1;
      return;
    }
    if (load) {
      const buf = new Uint8Array(load.buf.length + bytes.length);
      buf.set(load.buf);
      buf.set(bytes, load.buf.length);
      load.buf = buf;
      while (load && load.buf.length >= 8) {
        const len = new DataView(load.buf.buffer).getUint16(6, true);
        if (load.buf.length < 12 + len)
          break;
        const one = load.buf.slice(0, 12 + len);
        load.buf = load.buf.slice(12 + len);
        takeFrame(one);
      }
      return;
    }
    for (const ch of new TextDecoder().decode(bytes)) {
      if (ch === '\r') {
        push(line + '\r\n'); /* the console's echo */
        command(line.trim());
        line = '';
      } else {
        line += ch;
      }
    }
  }

  return {
    open() {
      lost = false;
      queue.length = 0;
      if (opened++ === 0)
        push(board.console === undefined ? GOT_IP : board.console);
      else
        for (const text of board.afterReconnect || [])
          push(text + '\r\n');
    },
    readable: {
      getReader: () => ({
        read() {
          /* what the board printed before the cable went still arrives */
          if (queue.length)
            return Promise.resolve({value: queue.shift(), done: false});
          if (lost)
            return Promise.reject(new Error('NetworkError: The device has been lost.'));
          return new Promise((resolve, reject) => { waiting = {resolve, reject}; });
        },
        cancel() {
          log('console-cancel');
          if (waiting) {
            const w = waiting;
            waiting = null;
            w.resolve({done: true});
          }
          return Promise.resolve();
        },
        releaseLock() {},
      }),
    },
    writable: {
      getWriter: () => ({
        write(bytes) {
          if (!lost && board.lose === 'write' && load)
            lose();
          if (lost)
            return Promise.reject(new Error('NetworkError: The device has been lost.'));
          write(bytes);
          return Promise.resolve();
        },
        releaseLock() {},
      }),
    },
  };
}

/*
 * options: targets (the published list, null for no targets.json), revision, chipName, mac, noSerial, noEsptool, noMd5,
 * cancelPicker, failConnect, failRevision, failErase, failWrite (Error text, thrown half way through the second part),
 * failDownload (a file name), failDisconnect, console (chunks the board prints after the install, then the stream
 * ends), holdConsole (the board stays silent until the read is cancelled), failConsoleOpen, hangConsoleOpen,
 * hangConsoleClose, baudRecovery (main() uploads the stub a second time, as esptool-js does when the board resets on
 * the baud change), noPostConnect (the chip has no postConnect), radio (publish radio.json and the C6 images; an
 * object overrides radio.json fields, {installer: null} leaves the installer out), installerVersion (of the published
 * c6_installer.bin), board (the console is a P4 running radio-fw, see makeBoard), addressTimesOut (with board: the
 * wait for the address runs out), failBody (a file name: its body fails after the headers).
 */
function makeFlow(options) {
  const o = Object.assign({targets: TARGETS, revision: 103, chipName: 'ESP32-P4', mac: 'aa:bb:cc:dd:ee:ff'}, options);
  const t = {events: [], written: null, progress: [], warned: null, errors: []};
  const log = (e) => t.events.push(e);
  console.error = (...args) => t.errors.push(args.map((a) => (a instanceof Error ? a.message : String(a))).join(' '));
  const chunks = (o.console || []).map((text) => new TextEncoder().encode(text));
  let cancelRead = null;
  t.port = {
    isOpen: false,
    readable: {
      getReader: () => ({
        read() {
          if (chunks.length)
            return Promise.resolve({value: chunks.shift(), done: false});
          if (o.holdConsole)
            return new Promise((resolve) => { cancelRead = () => resolve({done: true}); });
          return Promise.resolve({done: true});
        },
        cancel() {
          log('console-cancel');
          if (cancelRead)
            cancelRead();
          return Promise.resolve();
        },
        releaseLock() {},
      }),
    },
    async open(args) {
      log('console-open:' + args.baudRate);
      if (o.failConsoleOpen)
        throw new Error('port lost');
      if (o.hangConsoleOpen)
        await new Promise(() => {});
      t.port.isOpen = true;
    },
    async close() {
      log('console-close');
      if (o.hangConsoleClose)
        await new Promise(() => {});
      t.port.isOpen = false;
    },
  };

  if (o.board) {
    const board = makeBoard(o.board, t, log);
    const open = t.port.open;
    t.port.readable = board.readable;
    t.port.writable = board.writable;
    t.port.open = async (args) => {
      await open(args);
      board.open();
    };
  }

  class Transport {
    constructor(port) { this.port = port; log('transport'); }
    async setRTS(on) { log('rts:' + on); }
    async setDTR(on) { log('dtr:' + on); }
    async disconnect() {
      log('disconnect');
      this.port.isOpen = false;
      if (o.failDisconnect)
        throw new Error('port lost');
    }
  }

  class ESPLoader {
    constructor(args) {
      this.transport = args.transport;
      this.terminal = args.terminal;
      this.chip = {
        CHIP_NAME: o.chipName,
        getChipRevision: async () => {
          log('revision');
          if (o.failRevision)
            throw new Error('Read timeout');
          return o.revision;
        },
        readMac: async () => o.mac,
      };
      if (!o.noPostConnect)
        this.chip.postConnect = async (loader) => { log(loader === this ? 'postConnect' : 'postConnect:other-loader'); };
    }
    /* as esptool-js 0.7: connect, chip.postConnect, runStub, then changeBaud, whose recovery runs runStub alone */
    async main() {
      log('main');
      this.transport.port.isOpen = true;
      this.terminal.writeLine('Chip is ' + o.chipName);
      if (o.failConnect)
        throw new Error('Failed to connect with the device');
      if (this.chip.postConnect)
        await this.chip.postConnect(this);
      await this.runStub();
      if (o.baudRecovery)
        await this.runStub();
      return o.chipName;
    }
    async runStub() { log('stub'); }
    async eraseFlash() {
      log('erase');
      t.viewWhileErasing = t.flow.view();
      if (o.failErase)
        throw new Error('erase failed');
    }
    async writeFlash(args) {
      log('write');
      t.written = {
        files: args.fileArray.map((f) => ({address: f.address, size: f.data.length})),
        flashMode: args.flashMode, flashFreq: args.flashFreq, flashSize: args.flashSize,
        eraseAll: args.eraseAll, compress: args.compress,
        md5: args.fileArray.map((f) => args.calculateMD5Hash(f.data)),
      };
      for (let i = 0; i < args.fileArray.length; i++) {
        const total = args.fileArray[i].data.length;
        for (const written of [0, total / 2, total]) {
          if (o.failWrite && i === 1 && written === total)
            throw new Error(o.failWrite);
          args.reportProgress(i, written, total);
          const v = t.flow.view();
          t.progress.push({percent: v.percent, part: v.part, stage: v.stage});
          t.warned = t.warned === null ? t.unloadWarns() : t.warned;
        }
      }
    }
    async after() { log('after'); }
  }

  const routes = {};
  if (o.targets)
    routes['fw/targets.json'] = {json: o.targets};
  for (const tg of o.targets || []) {
    routes['fw/' + tg.manifest] = {json: {version: tg.version, parts: PARTS.map(({path: p, offset}) => ({path: p, offset}))}};
    const dir = 'fw/' + tg.manifest.replace(/manifest\.json$/, '');
    for (const part of PARTS)
      routes[dir + part.path] = {bytes: part.size, file: part.path};
  }

  if (o.radio) {
    const index = Object.assign({
      version: 'v9.9.9',
      rcp: {path: 'c6/c6_rcp.bin', version: RCP_VERSION},
      installer: {path: 'c6/c6_installer.bin', base_version: o.installerVersion || BASE_VERSION},
    }, o.radio === true ? {} : o.radio);
    if (!index.installer)
      delete index.installer;
    routes['fw/radio.json'] = {json: index};
    routes['fw/c6/c6_rcp.bin'] = {data: appImage('c6_rcp', RCP_VERSION, 2100), file: 'c6_rcp.bin'};
    routes['fw/c6/c6_installer.bin'] = {data: appImage('c6_installer', o.installerVersion || BASE_VERSION, 2500),
                                        file: 'c6_installer.bin'};
  }

  async function fakeFetch(url) {
    log('fetch:' + url);
    const route = routes[url];
    if (!route || (o.failDownload && route.file === o.failDownload))
      return {ok: false, status: 404};
    return {
      ok: true, status: 200,
      json: async () => route.json,
      arrayBuffer: async () => {
        if (o.failBody && route.file === o.failBody)
          throw new TypeError('network error');
        return route.data ? route.data.slice().buffer : new ArrayBuffer(route.bytes);
      },
    };
  }

  const serial = o.noSerial ? undefined : {
    requestPort() {
      log('requestPort');
      t.viewWhilePicking = t.flow.view();
      if (o.cancelPicker) {
        const error = new Error('No port selected by the user.');
        error.name = 'NotFoundError';
        return Promise.reject(error);
      }
      return Promise.resolve(t.port);
    },
  };

  const listeners = {};
  t.flow = Flasher.createFlow({
    serial,
    base: 'fw/',
    fetch: fakeFetch,
    loadEsptool: async () => {
      if (o.noEsptool)
        throw new Error('blocked');
      return {ESPLoader, Transport};
    },
    md5: o.noMd5 ? null : (data) => 'md5-of-' + data.length,
    sleep: async (ms) => {
      log('sleep:' + ms);
      /* the wait for the address: a board with something to say answers before it runs out */
      if ([Flasher.ADDRESS_WAIT_MS, Flasher.CLOSE_WAIT_MS].includes(ms) && (o.console || o.holdConsole))
        await new Promise(() => {});
      if (ms === Flasher.ADDRESS_WAIT_MS && o.board && !o.addressTimesOut)
        await new Promise(() => {});
      /* any other wait of a scripted board runs out only once everything it has queued is read */
      if (o.board)
        await new Promise((resolve) => setImmediate(resolve));
    },
    addUnloadListener: (fn) => { listeners.beforeunload = fn; },
  });
  t.unloadWarns = () => {
    const event = {prevented: false, preventDefault() { this.prevented = true; }};
    listeners.beforeunload(event);
    return event.prevented;
  };
  t.untilStage = async (...stages) => {
    for (let i = 0; i < 10000 && !stages.includes(t.flow.view().stage); i++)
      await new Promise((resolve) => setImmediate(resolve));
  };
  t.result = () => ({view: t.flow.view(), events: t.events, portOpen: t.port.isOpen, written: t.written, progress: t.progress,
                     warned: t.warned, errors: t.errors, viewWhileErasing: t.viewWhileErasing, viewWhilePicking: t.viewWhilePicking,
                     radio: t.radio || null, state: {radioPlan: t.flow.state.radioPlan, radioPhase: t.flow.state.radioPhase}});
  return t;
}

module.exports = {Flasher, makeFlow, appImage, TARGETS, SEVERAL_BOARDS, BOARD, OTHER_BOARD, V1, V3, RCP_VERSION, BASE_VERSION};
