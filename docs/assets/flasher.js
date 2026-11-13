'use strict';
/*
 * The browser flasher (docs/overrides/flash.html). createFlow holds the steps and takes everything from outside as
 * arguments, so tools/tests/test_flash_page.py runs it under node with fakes; view() turns its state into what the page
 * shows; start() binds that to the data-* attributes of the template.
 */
(function (root) {
  const P4 = 'ESP32-P4';
  const BAUD = 460800;
  const CONSOLE_BAUD = 115200;
  const CONSOLE_KEEP = 4096; /* of the console text: enough for a line split across reads */
  const ADDRESS_WAIT_MS = 60000; /* boot plus DHCP, with room for a slow server */
  const CLOSE_WAIT_MS = 2000; /* for the port to close before starting over */
  const STEP_OF = {
    loading: 0, unsupported: 0, empty: 0, unavailable: 0, idle: 0, picking: 0, connect_failed: 0, manual: 0,
    reading: 1, no_firmware: 1, wrong_chip: 1,
    detected: 2, downloading: 2, download_failed: 2, erasing: 2, writing: 2, write_failed: 2,
    done: 3,
    radio_checking: 3, radio_ready: 3, radio_no_installer: 3, radio_wrong_installer: 3, radio_fault: 3,
    radio_sending: 3, radio_installing: 3, radio_failed: 3, radio_port_lost: 3,
    radio_current: 4, radio_done: 4,
  };
  /* The radio step: the `radio-fw` console command of the P4 (docs/reference/radio-console.md). */
  const RADIO_CHUNK = 1024;
  const RADIO_REPLY = '@radio-fw ';
  const ACK_WAIT_MS = 2000; /* for each frame's ack; then the frame is sent again */
  const FRAME_TRIES = 5; /* sends of one frame before giving up */
  const REPLY_WAIT_MS = 5000; /* for status, ready and received */
  const STATUS_TRIES = 3;
  const STATUS_POLL_MS = 2000; /* between status questions while the board is still probing the radio */
  const STATUS_POLLS = 30;
  const BUSY_SILENCE_MS = 30000; /* the board prints busy every 5 s while it installs */
  const RECONNECT_LISTEN_MS = 10000;
  const INSTALL_CAP_S = {installer: 480, rcp: 120};
  const LOST = 'lost';
  const TIMEOUT = 'timeout';
  const STAGES = Object.keys(STEP_OF);
  const PART_NAMES = {
    'bootloader.bin': 'the bootloader',
    'partition-table.bin': 'the partition table',
    'ota_data_initial.bin': 'the OTA data',
    'p4.bin': 'the border router',
    'web_0.bin': 'the web UI',
  };

  function targetsForRevision(targets, revision) {
    return targets.filter((t) => t.min_rev <= revision && revision <= t.max_rev);
  }

  function revisionLabel(revision) {
    return 'v' + Math.floor(revision / 100) + '.' + (revision % 100);
  }

  /* The board's default name, as netnames_rules_default_hostname makes it. */
  function hostFromMac(mac) {
    return 'esp-otbr-' + mac.split(':').slice(3).join('').toLowerCase() + '.local';
  }

  /* The IPv4 address in the console text, from the line p4/main/eth.c logs; '' when there is none yet. */
  function addressIn(text) {
    const match = /eth: got ip (\d{1,3}(?:\.\d{1,3}){3})/.exec(text);
    return match ? match[1] : '';
  }

  let crcTable = null;

  /* CRC-32 (IEEE, as zlib's crc32); `crc` continues an earlier result. */
  function crc32(bytes, crc) {
    if (!crcTable) {
      crcTable = new Uint32Array(256);
      for (let n = 0; n < 256; n++) {
        let c = n;
        for (let k = 0; k < 8; k++)
          c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
        crcTable[n] = c >>> 0;
      }
    }
    let c = ~(crc || 0) >>> 0;
    for (let i = 0; i < bytes.length; i++)
      c = crcTable[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
    return ~c >>> 0;
  }

  /* One frame of `radio-fw load`: "RF", offset u32, length u16, payload, CRC-32 of offset, length and payload. */
  function frame(offset, bytes) {
    const out = new Uint8Array(8 + bytes.length + 4);
    const view = new DataView(out.buffer);
    out[0] = 0x52;
    out[1] = 0x46;
    view.setUint32(2, offset, true);
    view.setUint16(6, bytes.length, true);
    out.set(bytes, 8);
    view.setUint32(8 + bytes.length, crc32(out.subarray(2, 8 + bytes.length)), true);
    return out;
  }

  /* A reply line of the board's radio-fw command, wherever it starts in the line; null for any other line. */
  function parseReply(line) {
    const at = line.indexOf(RADIO_REPLY);
    if (at < 0)
      return null;
    const rest = line.slice(at + RADIO_REPLY.length).replace(/\r$/, '').trim();
    const space = rest.indexOf(' ');
    const kind = space < 0 ? rest : rest.slice(0, space);
    const text = space < 0 ? '' : rest.slice(space + 1);
    const words = text ? text.split(' ') : [];
    const fields = {};
    for (const word of words) {
      const eq = word.indexOf('=');
      if (eq > 0)
        fields[word.slice(0, eq)] = word.slice(eq + 1);
    }
    return {kind, text, words, fields};
  }

  /* Project and version from the app description of an ESP-IDF app image; null for anything else. */
  function appDesc(bytes) {
    const DESC = 32;
    if (!bytes || bytes.length < DESC + 80 || bytes[0] !== 0xE9)
      return null;
    if (new DataView(bytes.buffer, bytes.byteOffset).getUint32(DESC, true) !== 0xABCD5432)
      return null;
    const text = (from) => {
      const field = bytes.subarray(DESC + from, DESC + from + 32);
      const end = field.indexOf(0);
      return new TextDecoder().decode(end < 0 ? field : field.subarray(0, end));
    };
    return {project: text(48), version: text(16)};
  }

  /* The text the board gives when an installer is from another release (rcp_sync push_installer). */
  function wrongInstaller(version, base) {
    return 'wrong c6_installer.bin: ' + version + ', need ' + (base && base !== '-' ? base : 'none');
  }

  function distinct(values) {
    return Array.from(new Set(values));
  }

  function createFlow(deps) {
    const state = {
      stage: 'loading', targets: [], candidates: [], board: null, chip: null, erase: false, revision: null, chipName: '',
      log: [], percent: 0, part: '', file: '', host: '', ip: '', addressWaiting: false,
      radio: null, /* radio.json of the release: {version, rcp: {path, version}, installer?: {path, base_version}} */
      radioStatus: null, radioPlan: [], radioFile: '', radioPercent: 0, radioElapsed: 0, radioInstalled: '',
      radioCode: '', radioDetail: '', radioPhase: '',
    };
    let esptool = null;
    let session = null; /* {port, transport, loader} while the port is open */
    let search = null; /* {stopped, finish} while waiting for the address */
    let link = null; /* the console port after the install: see openLink */
    let images = {}; /* downloaded radio images by role: {name, data} */
    let epoch = 0; /* counts restarts: an action that finishes after one is stale */
    let listener = () => {};
    const emit = (patch) => { Object.assign(state, patch); listener(); };

    function target() {
      return state.candidates.find((t) => t.board === state.board && t.chip === state.chip) || null;
    }

    function only(values) {
      const unique = distinct(values);
      return unique.length === 1 ? unique[0] : null;
    }

    function offer(candidates) {
      emit({candidates, board: only(candidates.map((t) => t.board)), chip: only(candidates.map((t) => t.chip))});
    }

    async function release() {
      const open = session;
      session = null;
      if (open) {
        try {
          await open.transport.disconnect();
        } catch (e) {
          /* the port is already gone */
        }
      }
    }

    /*
     * Starts the firmware, then lets go of the port. DTR is cleared first: esptool-js's connect sequence leaves its
     * remembered DTR true, and setRTS resends that, so with both lines asserted the pulse would not reset the chip.
     */
    async function resetAndRelease() {
      if (!session)
        return;
      try {
        await session.transport.setDTR(false);
        await session.transport.setRTS(true);
        await deps.sleep(100);
        await session.loader.after();
      } catch (e) {
        /* the board or the cable is gone; nothing left to reset */
      }
      await release();
    }

    /* The picker must open straight from the click; null when it was cancelled. */
    async function pickPort() {
      const before = state.stage;
      emit({stage: 'picking'});
      try {
        return await deps.serial.requestPort();
      } catch (e) {
        emit({stage: before});
        return null;
      }
    }

    /* Opens the port and enters the bootloader; false after showing why not. */
    async function connect(port) {
      emit({stage: 'reading', log: []});
      const transport = new esptool.Transport(port);
      const terminal = {
        clean() {},
        write() {},
        writeLine(line) {
          if (state.stage === 'reading')
            emit({log: state.log.concat(line).slice(-3)});
        },
      };
      const loader = new esptool.ESPLoader({transport, baudrate: BAUD, terminal});
      /*
       * Workaround for an esptool-js bug (0.7.0 and 0.7.1): when the board resets on the switch to BAUD, the recovery
       * in ESPLoader.changeBaud reconnects and calls runStub() but skips chip.postConnect(). For the ESP32-P4 ECO7
       * (rev 3.01/3.02) that call holds powerOnFlash(), the fix for a ROM bug that fails the flash attach in a second
       * download session, so the stub never answers FLASH_DEFL_BEGIN and nothing is written. main() already runs
       * postConnect before the first upload; this runs it again before every later one. Remove once esptool-js runs
       * postConnect in that recovery path.
       */
      const runStub = loader.runStub.bind(loader);
      let stubUploads = 0;
      loader.runStub = async (...args) => {
        if (stubUploads++ > 0 && loader.chip && typeof loader.chip.postConnect === 'function')
          await loader.chip.postConnect(loader);
        return runStub(...args);
      };
      session = {port, transport, loader};
      try {
        await loader.main();
        const chipName = loader.chip.CHIP_NAME;
        if (chipName !== P4) {
          emit({stage: 'wrong_chip', chipName});
          await resetAndRelease();
          return false;
        }
        emit({chipName, host: hostFromMac(await loader.chip.readMac(loader))});
        return true;
      } catch (e) {
        console.error('flasher: connect failed', e);
        emit({stage: 'connect_failed'});
        await release();
        return false;
      }
    }

    async function detect() {
      const port = await pickPort();
      if (!port || !(await connect(port)))
        return;
      let revision;
      try {
        revision = await session.loader.chip.getChipRevision(session.loader);
      } catch (e) {
        console.error('flasher: reading the chip revision failed', e);
        emit({stage: 'connect_failed'});
        await release();
        return;
      }
      const candidates = targetsForRevision(state.targets, revision);
      emit({revision});
      offer(candidates);
      if (candidates.length) {
        emit({stage: 'detected'});
      } else {
        emit({stage: 'no_firmware'});
        await resetAndRelease();
      }
    }

    async function getOk(file, url) {
      let response;
      try {
        response = await deps.fetch(url);
      } catch (e) {
        response = null;
      }
      if (!response || !response.ok) {
        const error = new Error('download failed');
        error.file = file;
        throw error;
      }
      return response;
    }

    async function download(chosen) {
      const url = deps.base + chosen.manifest;
      const manifest = await (await getOk('manifest.json', url)).json();
      const dir = url.slice(0, url.lastIndexOf('/') + 1);
      const files = [];
      for (const part of manifest.parts) {
        const response = await getOk(part.path, dir + part.path);
        files.push({path: part.path, address: part.offset, data: new Uint8Array(await response.arrayBuffer())});
      }
      return files;
    }

    function reporter(files) {
      const sizes = files.map((f) => f.data.length);
      const total = sizes.reduce((a, b) => a + b, 0);
      return (index, written, partTotal) => {
        const before = sizes.slice(0, index).reduce((a, b) => a + b, 0);
        const done = before + (partTotal ? sizes[index] * written / partTotal : 0);
        emit({stage: 'writing', percent: Math.floor(100 * done / total), part: PART_NAMES[files[index].path] || files[index].path});
      };
    }

    async function write(files) {
      const {loader} = session;
      if (state.erase) {
        emit({stage: 'erasing'});
        await loader.eraseFlash();
      }
      emit({stage: 'writing', percent: 0, part: ''});
      await loader.writeFlash({
        fileArray: files.map((f) => ({data: f.data, address: f.address})),
        flashMode: 'keep', flashFreq: 'keep', flashSize: 'keep',
        eraseAll: false, compress: true,
        reportProgress: reporter(files),
        calculateMD5Hash: deps.md5,
      });
    }

    /*
     * The board's console after the install, at its own speed: the text it prints (for the address), the radio-fw reply
     * lines in it, and a writer. One reader for the whole time: the address search and the radio step share it.
     */
    async function openLink(port) {
      await port.open({baudRate: CONSOLE_BAUD});
      const l = {
        port, reader: port.readable.getReader(), writer: port.writable ? port.writable.getWriter() : null,
        text: '', partial: '', replies: [], waiters: new Set(), ended: false, lost: false, closing: false,
      };
      l.notify = () => { for (const w of Array.from(l.waiters)) w(); };
      l.pumped = pump(l);
      return l;
    }

    async function pump(l) {
      const decoder = new TextDecoder();
      try {
        for (;;) {
          const {value, done} = await l.reader.read();
          if (done)
            break;
          const chunk = decoder.decode(value, {stream: true});
          l.text = (l.text + chunk).slice(-CONSOLE_KEEP);
          const lines = (l.partial + chunk).split('\n');
          l.partial = lines.pop().slice(-CONSOLE_KEEP);
          for (const line of lines) {
            const reply = parseReply(line);
            if (reply)
              l.replies.push(reply);
          }
          l.notify();
        }
      } catch (e) {
        /* the cable is gone */
      }
      l.ended = true;
      l.lost = !l.closing;
      l.notify();
    }

    /* check()'s first truthy result; LOST once the port is gone, TIMEOUT after ms (none without ms). */
    function until(l, check, ms) {
      return new Promise((resolve) => {
        let settled = false;
        const finish = (value) => {
          if (!settled) {
            settled = true;
            l.waiters.delete(attempt);
            resolve(value);
          }
        };
        const attempt = () => {
          const value = check();
          if (value)
            finish(value);
          else if (l.ended)
            finish(LOST);
        };
        l.waiters.add(attempt);
        attempt();
        if (ms !== undefined)
          deps.sleep(ms).then(() => finish(TIMEOUT));
      });
    }

    /* The next reply of one of these kinds (others are dropped), LOST or TIMEOUT. */
    function nextReply(l, kinds, ms) {
      return until(l, () => {
        while (l.replies.length) {
          const reply = l.replies.shift();
          if (kinds.includes(reply.kind))
            return reply;
        }
        return null;
      }, ms);
    }

    async function send(l, bytes) {
      if (!l.writer || l.ended)
        throw radioLost();
      try {
        await l.writer.write(bytes);
      } catch (e) {
        l.ended = true;
        l.lost = true;
        l.notify();
        throw radioLost();
      }
    }

    function sendLine(l, text) {
      l.replies = [];
      return send(l, new TextEncoder().encode(text + '\r'));
    }

    /* Lets go of a console port (the current one by default); waits at most CLOSE_WAIT_MS for a port that hangs. */
    async function closeLink(l = link) {
      if (l === link)
        link = null;
      if (!l)
        return;
      l.closing = true;
      const closed = (async () => {
        await l.reader.cancel().catch(() => {});
        try {
          l.reader.releaseLock();
          if (l.writer)
            l.writer.releaseLock();
        } catch (e) {
          /* already released */
        }
        await l.port.close().catch(() => {});
      })();
      await Promise.race([closed, deps.sleep(CLOSE_WAIT_MS)]);
    }

    /* The board's address, or '' after ADDRESS_WAIT_MS whatever the port is doing. Leaves the console open in `link`. */
    async function findAddress(port) {
      const running = {stopped: false};
      const answer = new Promise((resolve) => { running.finish = resolve; });
      search = running;
      (async () => {
        let l;
        try {
          l = await openLink(port);
        } catch (e) {
          running.finish('');
          return;
        }
        if (running.stopped) {
          /* opened after the search gave up: let go at once */
          await closeLink(l);
          return;
        }
        link = l;
        const found = await until(l, () => addressIn(l.text));
        running.finish(found === LOST ? '' : found);
      })();
      deps.sleep(ADDRESS_WAIT_MS).then(() => running.finish(''));
      const ip = await answer;
      running.stopped = true;
      if (search === running)
        search = null;
      emit({ip, addressWaiting: false});
    }

    async function install() {
      const chosen = target();
      if (!chosen)
        return;
      if (!session) {
        const port = await pickPort();
        if (!port || !(await connect(port)))
          return;
      }
      emit({stage: 'downloading'});
      let files;
      try {
        files = await download(chosen);
      } catch (e) {
        emit({stage: 'download_failed', file: e.file});
        await resetAndRelease();
        return;
      }
      try {
        await write(files);
      } catch (e) {
        console.error('flasher: install failed', e);
        /* shown first: closing a port whose cable is gone may take a while */
        emit({stage: 'write_failed'});
        await release();
        return;
      }
      const {port} = session;
      await resetAndRelease();
      emit({stage: 'done', percent: 100, ip: '', addressWaiting: true});
      await findAddress(port);
      if (state.stage !== 'done')
        return; /* started over meanwhile */
      if (state.radio)
        await checkRadio(); /* without a usable port it offers Reconnect */
      else
        await closeLink();
    }

    /* ---- the radio step ---- */

    function radioLost() {
      return {lost: true};
    }

    function radioError(code, detail) {
      return {code, detail};
    }

    /* `radio-fw status`, asked again while the board is still probing; the parsed fields. */
    async function radioStatus(l) {
      for (let poll = 0; poll < STATUS_POLLS; poll++) {
        let reply = TIMEOUT;
        for (let attempt = 0; attempt < STATUS_TRIES && reply === TIMEOUT; attempt++) {
          await sendLine(l, 'radio-fw status');
          reply = await nextReply(l, ['status'], REPLY_WAIT_MS);
        }
        if (reply === LOST)
          throw radioLost();
        if (reply === TIMEOUT)
          throw radioError('no_answer', 'The board did not answer radio-fw status. Its firmware may be older than this page.');
        if (reply.fields.state !== 'probing' && reply.fields.state !== 'busy')
          return reply.fields;
        await deps.sleep(STATUS_POLL_MS);
      }
      throw radioError('no_answer', 'The board is still busy with the radio. Wait a minute, then check again.');
    }

    async function radioImage(role) {
      if (!images[role]) {
        const entry = state.radio[role];
        const name = entry.path.slice(entry.path.lastIndexOf('/') + 1);
        let data;
        try {
          const response = await getOk(name, deps.base + entry.path);
          data = new Uint8Array(await response.arrayBuffer());
        } catch (e) {
          throw radioError('download_failed', 'The file ' + name + ' could not be fetched. Check your connection.');
        }
        images[role] = {role, name, data};
      }
      return images[role];
    }

    function radioFailed(error) {
      if (error && error.lost && !link)
        return Promise.resolve(); /* the page closed the port itself: started over */
      if (error && error.lost) {
        emit({stage: 'radio_port_lost'});
        return closeLink();
      }
      if (error && error.code) {
        emit({stage: 'radio_failed', radioCode: error.code, radioDetail: error.detail || ''});
        return Promise.resolve();
      }
      throw error;
    }

    /* Reads the radio's state and shows what the page will do about it. */
    async function checkRadio() {
      if (!link || link.ended) {
        emit({stage: 'radio_port_lost', radioPhase: 'checking'});
        await closeLink();
        return;
      }
      emit({stage: 'radio_checking', radioPhase: 'checking', radioCode: '', radioDetail: ''});
      try {
        const status = await radioStatus(link);
        emit({radioStatus: status});
        if (status.state === 'esp_hosted') {
          if (!state.radio.installer) {
            emit({stage: 'radio_no_installer'});
            return;
          }
          const installer = await radioImage('installer');
          await radioImage('rcp');
          const desc = appDesc(installer.data);
          if (!desc || desc.version !== status.base) {
            emit({stage: 'radio_wrong_installer', radioDetail: wrongInstaller(desc ? desc.version : '?', status.base)});
            return;
          }
          emit({stage: 'radio_ready', radioPlan: ['installer', 'rcp']});
        } else if (status.state === 'updater') {
          if (status.installed === state.radio.rcp.version) {
            emit({stage: 'radio_current', radioInstalled: status.installed});
            return;
          }
          await radioImage('rcp');
          emit({stage: 'radio_ready', radioPlan: ['rcp']});
        } else {
          emit({stage: 'radio_fault'});
        }
      } catch (e) {
        await radioFailed(e);
      }
    }

    /* After `received`: busy lines until done or error. Resolves with the installed version. */
    async function waitForPush(l, role) {
      for (;;) {
        const reply = await nextReply(l, ['busy', 'done', 'error'], BUSY_SILENCE_MS);
        if (reply === LOST || reply === TIMEOUT)
          throw radioLost();
        if (reply.kind === 'error')
          throw radioError(reply.words[0] || 'push_failed', reply.words.slice(1).join(' '));
        if (reply.kind === 'done')
          return reply.fields.installed && reply.fields.installed !== '-' ? reply.fields.installed : '';
        const elapsed = Number(reply.words[0]) || 0;
        emit({radioElapsed: elapsed});
        if (elapsed > INSTALL_CAP_S[role])
          throw radioError('timeout', 'The board has been installing for ' + elapsed + ' s without an answer.');
      }
    }

    /* `radio-fw load`, the frames, then the board's push. Resolves with the installed version. */
    async function sendImage(l, image) {
      const {data} = image;
      emit({stage: 'radio_sending', radioPhase: 'sending', radioFile: image.name, radioPercent: 0, radioElapsed: 0});
      await sendLine(l, 'radio-fw load ' + data.length + ' ' + crc32(data).toString(16).padStart(8, '0'));
      let reply = await nextReply(l, ['ready', 'error'], REPLY_WAIT_MS);
      if (reply === LOST)
        throw radioLost();
      if (reply === TIMEOUT)
        throw radioError('no_answer', 'The board did not answer radio-fw load.');
      if (reply.kind === 'error')
        throw radioError(reply.words[0], reply.words.slice(1).join(' '));
      const chunk = Math.min(RADIO_CHUNK, Number(reply.fields.max) || RADIO_CHUNK);
      let offset = 0;
      let tries = 0;
      let received = false;
      while (offset < data.length) {
        if (tries >= FRAME_TRIES) {
          await send(l, frame(offset, new Uint8Array(0))).catch(() => {});
          throw radioError('transfer_failed', 'The board did not take the image at byte ' + offset + '.');
        }
        tries++;
        if (offset + chunk >= data.length)
          emit({radioPhase: 'finishing'}); /* once the board has the last frame it may start installing */
        await send(l, frame(offset, data.subarray(offset, offset + chunk)));
        /* `received` stands for the last frame's ack when that ack is lost */
        reply = await nextReply(l, ['ack', 'nak', 'error', 'received'], ACK_WAIT_MS);
        if (reply === LOST)
          throw radioLost();
        if (reply === TIMEOUT)
          continue;
        if (reply.kind === 'error')
          throw radioError(reply.words[0], reply.words.slice(1).join(' '));
        if (reply.kind === 'received') {
          received = true;
          break;
        }
        const next = Number(reply.words[0]);
        if (reply.kind === 'ack' && next > offset)
          tries = 0;
        offset = next;
        emit({radioPercent: Math.floor(100 * offset / data.length)});
      }
      if (!received)
        reply = await nextReply(l, ['received', 'error'], REPLY_WAIT_MS);
      if (reply === LOST)
        throw radioLost();
      if (reply === TIMEOUT)
        throw radioError('no_answer', 'The board did not confirm the image.');
      if (reply.kind === 'error')
        throw radioError(reply.words[0], reply.words.slice(1).join(' '));
      emit({stage: 'radio_installing', radioPhase: 'installing', radioPercent: 100});
      return waitForPush(l, image.role);
    }

    /* The planned images in order; after the installer, the updater must answer before the RCP goes. */
    async function installRadio() {
      if (!link || link.ended) {
        emit({stage: 'radio_port_lost', radioPhase: 'checking'});
        return;
      }
      try {
        for (const role of state.radioPlan) {
          if (role === 'rcp' && state.radioPlan[0] === 'installer') {
            emit({stage: 'radio_checking', radioPhase: 'checking'});
            const status = await radioStatus(link);
            emit({radioStatus: status});
            if (status.state !== 'updater')
              throw radioError('install_failed', 'The C6 did not come back with the updater after the installer.');
          }
          const installed = await sendImage(link, await radioImage(role));
          emit({radioInstalled: installed});
        }
        emit({stage: 'radio_done'});
      } catch (e) {
        await radioFailed(e);
      }
    }

    /* After a lost port: pick it again and listen; a push that started keeps running on the board. */
    async function reconnectRadio() {
      const port = await pickPort();
      if (!port)
        return;
      await closeLink();
      try {
        link = await openLink(port);
      } catch (e) {
        emit({stage: 'radio_port_lost'});
        return;
      }
      if (state.radioPhase !== 'installing' && state.radioPhase !== 'finishing') {
        await checkRadio();
        return;
      }
      emit({stage: 'radio_installing'});
      const heard = await nextReply(link, ['busy', 'done', 'error'], RECONNECT_LISTEN_MS);
      if (heard === TIMEOUT) {
        await checkRadio();
        return;
      }
      try {
        if (heard === LOST)
          throw radioLost();
        if (heard.kind === 'error')
          throw radioError(heard.words[0] || 'push_failed', heard.words.slice(1).join(' '));
        const installed = heard.kind === 'done' ? (heard.fields.installed || '').replace(/^-$/, '') :
          await waitForPush(link, state.radioFile === 'c6_installer.bin' ? 'installer' : 'rcp');
        emit({radioInstalled: installed});
        if (installed && installed === state.radio.rcp.version)
          emit({stage: 'radio_done'});
        else
          await checkRadio();
      } catch (e) {
        await radioFailed(e);
      }
    }

    async function init() {
      deps.addUnloadListener((event) => {
        if (['erasing', 'writing', 'radio_sending', 'radio_installing'].includes(state.stage)) {
          event.preventDefault();
          event.returnValue = '';
        }
      });
      if (!deps.serial) {
        emit({stage: 'unsupported'});
        return;
      }
      try {
        const response = await deps.fetch(deps.base + 'targets.json');
        if (!response.ok)
          throw new Error('no targets');
        state.targets = await response.json();
      } catch (e) {
        emit({stage: 'empty'});
        return;
      }
      try {
        const response = await deps.fetch(deps.base + 'radio.json');
        state.radio = response.ok ? await response.json() : null;
      } catch (e) {
        state.radio = null; /* the P4 can still be installed */
      }
      if (!deps.md5) {
        /* the checksum library failed to load: found out here, not half way through a write */
        emit({stage: 'unavailable'});
        return;
      }
      try {
        esptool = await deps.loadEsptool();
      } catch (e) {
        emit({stage: 'unavailable'});
        return;
      }
      emit({stage: 'idle'});
    }

    const flow = {
      state,
      init,
      detect,
      install,
      view: () => view(state, target()),
      subscribe(fn) { listener = fn; },
      setErase(on) { emit({erase: on}); },
      pickBoard(board) { emit({board}); },
      pickChip(chip) { emit({chip}); },
      chooseByHand() {
        /* reached from the first screen or a failure: the port is closed in both */
        emit({stage: 'manual', revision: null});
        offer(state.targets);
      },
      async restart() {
        if (search) {
          search.stopped = true;
          search.finish('');
        }
        epoch++;
        await closeLink();
        await resetAndRelease();
        images = {};
        emit({stage: 'idle', revision: null, candidates: [], board: null, chip: null, erase: false, percent: 0, part: '', file: '', ip: '', addressWaiting: false,
              radioStatus: null, radioPlan: [], radioFile: '', radioPercent: 0, radioElapsed: 0, radioInstalled: '', radioCode: '', radioDetail: '', radioPhase: ''});
      },
      checkRadio,
      installRadio,
      reconnectRadio,
      async installAnyway() {
        const started = epoch;
        try {
          await radioImage('rcp');
          if (started === epoch)
            emit({stage: 'radio_ready', radioPlan: ['rcp']});
        } catch (e) {
          if (started === epoch)
            await radioFailed(e);
        }
      },
    };
    return flow;
  }

  function view(state, chosen) {
    const stage = state.stage;
    const busy = stage === 'erasing' || stage === 'writing';
    const known = state.revision !== null && ['detected', 'downloading', 'erasing', 'writing', 'done', 'no_firmware'].includes(stage);
    /* without radio files the install is the last step */
    const stepIndex = stage === 'done' && !state.radio ? 4 : STEP_OF[stage];
    const radioStage = stage.startsWith('radio_');
    const radioBusy = stage === 'radio_sending' || stage === 'radio_installing';
    const plan = state.radioPlan || [];
    const installerPlanned = plan.includes('installer');
    const stepClass = (i) => 'step' + (i === stepIndex ? ' on' : i < stepIndex ? ' done' : '');
    const failed = stage === 'connect_failed' || stage === 'write_failed';
    const boards = distinct(state.candidates.map((t) => t.board));
    const chips = distinct(state.candidates.filter((t) => !state.board || t.board === state.board).map((t) => t.chip));
    const byBoard = (id) => state.candidates.find((t) => t.board === id);
    const byChip = (id) => state.candidates.find((t) => t.chip === id);
    const anyVersion = chosen ? chosen.version : state.targets.length ? state.targets[0].version : '';
    const address = state.ip || state.host;

    let trace = 'trace';
    if (stage === 'reading' || stage === 'downloading' || busy || radioBusy)
      trace = 'trace live run';
    else if (stage === 'detected' || stage === 'manual' || stage === 'no_firmware')
      trace = 'trace live';
    else if (stage === 'done' || stage === 'radio_done' || stage === 'radio_current')
      trace = 'trace good';
    else if (failed || stage === 'radio_failed' || stage === 'radio_port_lost')
      trace = 'trace bad';

    return {
      stage,
      step0: stepClass(0), step1: stepClass(1), step2: stepClass(2), step3: stepClass(3),
      rev: state.revision === null ? '' : revisionLabel(state.revision),
      chipName: state.chipName,
      log: state.log,
      showBoardChoices: boards.length > 1,
      boardChoices: boards.map((id) => ({value: id, label: byBoard(id).board_name, checked: id === state.board})),
      chipChoices: chips.map((id) => ({value: id, label: byChip(id).chip_name + ' (' + id + ')', checked: id === state.chip})),
      erase: state.erase,
      canInstall: !!chosen && (stage === 'detected' || stage === 'manual'),
      hasTarget: !!chosen,
      boardName: chosen ? chosen.board_name : '',
      chipId: chosen ? chosen.chip : '',
      release: anyVersion ? 'Release ' + anyVersion : '',
      version: anyVersion,
      percent: state.percent,
      percentText: state.percent + '%',
      part: state.part,
      writingLabel: state.part ? 'Writing ' + state.part : 'Starting to write',
      file: state.file,
      host: state.host,
      hostUrl: address ? 'http://' + address + '/' : '',
      openLabel: 'Open ' + address,
      addressWaiting: state.addressWaiting,
      canOpen: (stage === 'done' || radioStage) && !state.addressWaiting,
      /* the radio step */
      radioAvailable: !!state.radio,
      radioMissing: !state.radio,
      radioRelease: state.radio ? state.radio.rcp.version : '',
      radioPlanText: installerPlanned ? 'c6_installer.bin, then c6_rcp.bin' : 'c6_rcp.bin',
      radioWarnInstaller: installerPlanned,
      radioWarnRcp: !installerPlanned,
      radioFile: state.radioFile,
      radioPercentText: state.radioPercent + '%',
      radioSendingLabel: 'Sending ' + state.radioFile,
      radioElapsedText: state.radioElapsed + ' s',
      radioInstalling: state.radioFile === 'c6_installer.bin' ? 'The C6 is installing the installer, then starts its updater.' :
        'The C6 is writing the radio firmware.',
      radioInstallingInstaller: state.radioFile === 'c6_installer.bin',
      radioInstalled: state.radioInstalled || (state.radioStatus && state.radioStatus.installed !== '-' ? state.radioStatus.installed : ''),
      radioCode: state.radioCode,
      radioDetail: state.radioDetail,
      radioInstallerFailed: state.radioCode === 'install_failed' && state.radioFile === 'c6_installer.bin',
      lostWhileChecking: state.radioPhase === 'checking' || state.radioPhase === '',
      lostWhileSending: state.radioPhase === 'sending',
      lostWhileFinishing: state.radioPhase === 'finishing',
      lostWhileInstalling: state.radioPhase === 'installing',
      /* the live board */
      revLabel: known ? revisionLabel(state.revision) : '',
      traceClass: trace,
      chipClass: known || stage === 'reading' ? 'chip found' : 'chip',
      scanClass: stage === 'reading' ? 'scan on' : 'scan',
      flashClass: 'flash-part' + (busy ? ' on' : stage === 'done' || radioStage ? ' good' : ''),
      radioGroupClass: radioStage ? 'c6' : 'dim',
      radioChipClass: radioBusy || stage === 'radio_done' || stage === 'radio_current' ? 'chip found' : 'chip',
      portRing: stage === 'idle' ? 'ring on' : 'ring',
      portLabel: stage === 'idle' ? 'lab on' : 'lab',
      buttonRing: stage === 'connect_failed' ? 'ring on' : 'ring',
      buttonLabel: stage === 'connect_failed' ? 'lab on' : 'lab',
    };
  }

  /* One radio button per choice; rebuilt only when the choices change, so a pick keeps the keyboard focus. */
  function renderChoices(doc, box, choices, pick) {
    const drawn = choices.map((c) => c.value).join(' ');
    if (box.dataset.drawn !== drawn) {
      box.dataset.drawn = drawn;
      box.textContent = '';
      /* each list is its own radio group: the same list appears on two panels */
      const name = box.dataset.list + '-' + box.closest('[data-stage]').dataset.stage;
      for (const choice of choices) {
        const label = doc.createElement('label');
        label.className = 'choice';
        const input = doc.createElement('input');
        input.type = 'radio';
        input.name = name;
        input.value = choice.value;
        input.addEventListener('change', () => pick(choice.value));
        label.append(input, doc.createTextNode(choice.label));
        box.appendChild(label);
      }
    }
    box.querySelectorAll('input').forEach((input, i) => { input.checked = choices[i].checked; });
  }

  function renderLog(doc, box, lines) {
    box.textContent = '';
    for (const line of lines) {
      const row = doc.createElement('div');
      row.textContent = line;
      box.appendChild(row);
    }
  }

  function render(doc, flow) {
    const v = flow.view();
    doc.querySelectorAll('[data-stage]').forEach((el) => { el.hidden = !el.dataset.stage.split(' ').includes(v.stage); });
    doc.querySelectorAll('[data-text]').forEach((el) => { el.textContent = v[el.dataset.text]; });
    doc.querySelectorAll('[data-if]').forEach((el) => { el.hidden = !v[el.dataset.if]; });
    doc.querySelectorAll('[data-class]').forEach((el) => { el.setAttribute('class', v[el.dataset.class]); });
    doc.querySelectorAll('[data-href]').forEach((el) => { el.href = v[el.dataset.href]; });
    doc.querySelectorAll('[data-check]').forEach((el) => { el.checked = v[el.dataset.check]; });
    doc.querySelectorAll('[data-list="log"]').forEach((el) => renderLog(doc, el, v.log));
    doc.querySelectorAll('[data-list="boardChoices"]').forEach((el) => renderChoices(doc, el, v.boardChoices, flow.pickBoard));
    doc.querySelectorAll('[data-list="chipChoices"]').forEach((el) => renderChoices(doc, el, v.chipChoices, flow.pickChip));
  }

  /* The page: wires the flow to the browser. */
  function start(options) {
    const doc = root.document;
    const flow = createFlow({
      serial: root.navigator.serial,
      base: options.base,
      fetch: (url) => root.fetch(url),
      loadEsptool: () => import(options.esptool),
      md5: root.SparkMD5 ? (data) => root.SparkMD5.ArrayBuffer.hash(data) : null,
      sleep: (ms) => new Promise((resolve) => setTimeout(resolve, ms)),
      addUnloadListener: (fn) => root.addEventListener('beforeunload', fn),
    });
    flow.subscribe(() => render(doc, flow));
    doc.querySelectorAll('[data-action]').forEach((el) => el.addEventListener('click', () => flow[el.dataset.action]()));
    doc.querySelectorAll('[data-check="erase"]').forEach((el) => el.addEventListener('change', () => flow.setErase(el.checked)));
    render(doc, flow);
    return flow.init();
  }

  const api = {
    targetsForRevision, hostFromMac, addressIn, crc32, frame, parseReply, appDesc, wrongInstaller, createFlow, view, start,
    STAGES, ADDRESS_WAIT_MS, CLOSE_WAIT_MS, ACK_WAIT_MS, REPLY_WAIT_MS, STATUS_POLL_MS, BUSY_SILENCE_MS, RECONNECT_LISTEN_MS,
  };
  if (typeof module !== 'undefined' && module.exports)
    module.exports = api;
  else
    root.Flasher = api;
})(typeof window !== 'undefined' ? window : globalThis);
