'use strict';
/*
 * The browser flasher (docs/overrides/flash.html). createFlow holds the steps and takes everything from outside as
 * arguments, so tools/tests/test_flash_page.py runs it under node with fakes; view() turns its state into what the page
 * shows; start() binds that to the data-* attributes of the template.
 */
(function (root) {
  const P4 = 'ESP32-P4';
  const BAUD = 460800;
  const STEP_OF = {
    loading: 0, unsupported: 0, empty: 0, unavailable: 0, idle: 0, picking: 0, connect_failed: 0, manual: 0,
    reading: 1, no_firmware: 1, wrong_chip: 1,
    detected: 2, downloading: 2, download_failed: 2, erasing: 2, writing: 2, write_failed: 2,
    done: 3,
  };
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

  function distinct(values) {
    return Array.from(new Set(values));
  }

  function createFlow(deps) {
    const state = {
      stage: 'loading', targets: [], candidates: [], board: null, chip: null, erase: false, revision: null, chipName: '',
      log: [], percent: 0, part: '', file: '', host: '',
    };
    let esptool = null;
    let session = null; /* {transport, loader} while the port is open */
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

    /* Starts the firmware the way ESP Web Tools does, then lets go of the port. */
    async function resetAndRelease() {
      if (!session)
        return;
      try {
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
      session = {transport, loader};
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
        /* shown first: closing a port whose cable is gone may take a while */
        emit({stage: 'write_failed'});
        await release();
        return;
      }
      await resetAndRelease();
      emit({stage: 'done', percent: 100});
    }

    async function init() {
      deps.addUnloadListener((event) => {
        if (state.stage === 'erasing' || state.stage === 'writing') {
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
        await resetAndRelease();
        emit({stage: 'idle', revision: null, candidates: [], board: null, chip: null, erase: false, percent: 0, part: '', file: ''});
      },
    };
    return flow;
  }

  function view(state, chosen) {
    const stage = state.stage;
    const busy = stage === 'erasing' || stage === 'writing';
    const known = state.revision !== null && ['detected', 'downloading', 'erasing', 'writing', 'done', 'no_firmware'].includes(stage);
    const stepIndex = STEP_OF[stage];
    const stepClass = (i) => 'step' + (i === stepIndex ? ' on' : i < stepIndex ? ' done' : '');
    const failed = stage === 'connect_failed' || stage === 'write_failed';
    const boards = distinct(state.candidates.map((t) => t.board));
    const chips = distinct(state.candidates.filter((t) => !state.board || t.board === state.board).map((t) => t.chip));
    const byBoard = (id) => state.candidates.find((t) => t.board === id);
    const byChip = (id) => state.candidates.find((t) => t.chip === id);
    const anyVersion = chosen ? chosen.version : state.targets.length ? state.targets[0].version : '';

    let trace = 'trace';
    if (stage === 'reading' || stage === 'downloading' || busy)
      trace = 'trace live run';
    else if (stage === 'detected' || stage === 'manual' || stage === 'no_firmware')
      trace = 'trace live';
    else if (stage === 'done')
      trace = 'trace good';
    else if (failed)
      trace = 'trace bad';

    return {
      stage,
      step0: stepClass(0), step1: stepClass(1), step2: stepClass(2),
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
      hostUrl: state.host ? 'http://' + state.host + '/' : '',
      openLabel: 'Open ' + state.host,
      /* the live board */
      revLabel: known ? revisionLabel(state.revision) : '',
      traceClass: trace,
      chipClass: known || stage === 'reading' ? 'chip found' : 'chip',
      scanClass: stage === 'reading' ? 'scan on' : 'scan',
      flashClass: 'flash-part' + (busy ? ' on' : stage === 'done' ? ' good' : ''),
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

  const api = {targetsForRevision, hostFromMac, createFlow, view, start, STAGES};
  if (typeof module !== 'undefined' && module.exports)
    module.exports = api;
  else
    root.Flasher = api;
})(typeof window !== 'undefined' ? window : globalThis);
