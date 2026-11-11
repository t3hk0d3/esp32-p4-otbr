'use strict';
/*
 * Test harness for docs/assets/flasher.js (used by test_flash_page.py, run with node): fakes for the serial port,
 * esptool-js and fetch that write what happens to one ordered event list.
 */
const path = require('path');

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

/*
 * options: targets (the published list, null for no targets.json), revision, chipName, mac, noSerial, noEsptool, noMd5,
 * cancelPicker, failConnect, failErase, failWrite (Error text, thrown half way through the second part),
 * failDownload (a file name), failDisconnect.
 */
function makeFlow(options) {
  const o = Object.assign({targets: TARGETS, revision: 103, chipName: 'ESP32-P4', mac: 'aa:bb:cc:dd:ee:ff'}, options);
  const t = {events: [], written: null, progress: [], port: {open: false}, warned: null};
  const log = (e) => t.events.push(e);

  class Transport {
    constructor(port) { this.port = port; log('transport'); }
    async setRTS(on) { log('rts:' + on); }
    async disconnect() {
      log('disconnect');
      this.port.open = false;
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
        getChipRevision: async () => { log('revision'); return o.revision; },
        readMac: async () => o.mac,
      };
    }
    async main() {
      log('main');
      this.transport.port.open = true;
      this.terminal.writeLine('Chip is ' + o.chipName);
      if (o.failConnect)
        throw new Error('Failed to connect with the device');
      return o.chipName;
    }
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

  async function fakeFetch(url) {
    log('fetch:' + url);
    const route = routes[url];
    if (!route || (o.failDownload && route.file === o.failDownload))
      return {ok: false, status: 404};
    return {
      ok: true, status: 200,
      json: async () => route.json,
      arrayBuffer: async () => new ArrayBuffer(route.bytes),
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
    sleep: async (ms) => log('sleep:' + ms),
    addUnloadListener: (fn) => { listeners.beforeunload = fn; },
  });
  t.unloadWarns = () => {
    const event = {prevented: false, preventDefault() { this.prevented = true; }};
    listeners.beforeunload(event);
    return event.prevented;
  };
  t.result = () => ({view: t.flow.view(), events: t.events, portOpen: t.port.open, written: t.written, progress: t.progress,
                     warned: t.warned, viewWhileErasing: t.viewWhileErasing, viewWhilePicking: t.viewWhilePicking});
  return t;
}

module.exports = {Flasher, makeFlow, TARGETS, SEVERAL_BOARDS, BOARD, OTHER_BOARD, V1, V3};
