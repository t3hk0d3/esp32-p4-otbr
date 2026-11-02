'use strict';
/*
 * Test harness for the device pages (static/device_common.js and the page scripts, used by the test_*_page.py files,
 * run with node): a small fake DOM built from the ids of a page, a fake fetch that records requests, and a manual clock
 * with timers.
 */
const fs = require('fs');
const path = require('path');

const FRONTEND = path.join(__dirname, '..', '..', 'p4', 'components', 'esp_ot_br_server', 'frontend');
const Common = require(path.join(FRONTEND, 'static', 'device_common.js'));
const SCRIPTS = {device: 'status', ethernet: 'ethernet', firmware: 'firmware', capture: 'capture',
                 diagnostics: 'diagnostics'};

class El {
  constructor(doc, tag, id, classes) {
    this.doc = doc;
    this.tag = tag;
    this.children = [];
    this.classes = new Set(classes || []);
    this.style = {};
    this.attrs = {};
    this.listeners = {};
    this.textContent = '';
    this.value = '';
    this.checked = false;
    this.disabled = false;
    this.files = [];
    this._id = '';
    if (id)
      this.id = id;
    const self = this;
    this.classList = {
      toggle(c, force) {
        const on = force === undefined ? !self.classes.has(c) : !!force;
        on ? self.classes.add(c) : self.classes.delete(c);
        return on;
      },
      contains(c) { return self.classes.has(c); },
      add(c) { self.classes.add(c); },
      remove(c) { self.classes.delete(c); }
    };
  }
  get id() { return this._id; }
  set id(v) {
    this._id = v;
    this.doc.byId[v] = this;
  }
  set className(v) { this.classes = new Set(String(v).split(/\s+/).filter(Boolean)); }
  get className() { return Array.from(this.classes).join(' '); }
  get firstChild() { return this.children[0] || null; }
  appendChild(c) { this.children.push(c); return c; }
  removeChild(c) { this.children = this.children.filter((x) => x !== c); return c; }
  setAttribute(k, v) { this.attrs[k] = String(v); }
  getAttribute(k) { return this.attrs[k]; }
  addEventListener(t, f) { (this.listeners[t] = this.listeners[t] || []).push(f); }
  fire(type, extra) {
    const ev = Object.assign({type, target: this, defaultPrevented: false,
                              preventDefault() { this.defaultPrevented = true; }}, extra || {});
    (this.listeners[type] || []).forEach((f) => f(ev));
    return ev;
  }
  click() { return this.fire('click'); }
  /* Sets the value (or checked) and fires the event a person typing or ticking would fire. */
  type(value) { this.value = value; return this.fire('input'); }
  tick(on) { this.checked = on; return this.fire('change'); }
  hidden() { return this.classes.has('hidden'); }
  /* Text of the element and all children, for rows built by the script. */
  allText() { return [this.textContent].concat(this.children.map((c) => c.allText())).filter(Boolean).join(' '); }
}

class Doc {
  constructor(html) {
    this.byId = {};
    this.hidden = false;
    this.listeners = {};
    const tagRe = /<(\w+)([^>]*)>/g;
    let m;
    while ((m = tagRe.exec(html))) {
      const attrs = m[2];
      const id = /\bid="([^"]+)"/.exec(attrs);
      const cls = /\bclass="([^"]*)"/.exec(attrs);
      if (id) {
        const e = new El(this, m[1], id[1], cls ? cls[1].split(/\s+/) : []);
        const re = /\b([\w-]+)="([^"]*)"/g;
        let a;
        while ((a = re.exec(attrs)))
          e.attrs[a[1]] = a[2];
        if (/\bchecked\b/.test(attrs))
          e.checked = true;
        if (e.attrs.value !== undefined)
          e.value = e.attrs.value;
      }
    }
  }
  getElementById(id) {
    if (!this.byId[id])
      throw new Error('no element with id ' + id);
    return this.byId[id];
  }
  createElement(tag) { return new El(this, tag); }
  createTextNode(t) { const e = new El(this, '#text'); e.textContent = t; return e; }
  addEventListener(t, f) { (this.listeners[t] = this.listeners[t] || []).push(f); }
  fire(t) {
    const ev = {type: t, defaultPrevented: false, preventDefault() { this.defaultPrevented = true; }};
    (this.listeners[t] || []).forEach((f) => f(ev));
    return ev;
  }
}

/*
 * `page`: a page name (device, ethernet, firmware, capture, diagnostics) or {html, kit} to run DeviceKit alone on the
 * given markup with the given options. routes: function(url, opts) -> {status, body} | throws to simulate a dropped
 * connection.
 */
function makeEnv(page, routes) {
  const html = typeof page === 'string' ? fs.readFileSync(path.join(FRONTEND, page + '.html'), 'utf8') : page.html;
  const doc = new Doc(html);
  const calls = [];
  const timers = [];
  let now = 1000000;
  let seq = 0;
  let reloads = 0;
  let inFlight = 0;
  let maxInFlight = 0;
  let confirms = [];
  const env = {
    document: doc,
    location: {host: 'board.lan'},
    fetch(url, opts) {
      calls.push({url, method: opts.method, headers: opts.headers, body: opts.body});
      inFlight++;
      maxInFlight = Math.max(maxInFlight, inFlight);
      return new Promise((resolve, reject) => {
        setImmediate(() => {
          inFlight--;
          let r;
          try {
            r = routes(url, opts);
          } catch (e) {
            reject(e);
            return;
          }
          const text = typeof r.body === 'string' ? r.body : JSON.stringify(r.body);
          resolve({status: r.status, text: () => Promise.resolve(text)});
        });
      });
    },
    setTimeout(f, ms) { const id = ++seq; timers.push({id, at: now + ms, f}); return id; },
    clearTimeout(id) { const i = timers.findIndex((t) => t.id === id); if (i >= 0) timers.splice(i, 1); },
    setInterval(f, ms) { const id = ++seq; timers.push({id, at: now + ms, f, every: ms}); return id; },
    now() { return now; },
    confirm(t) { confirms.push(t); return true; },
    reload() { reloads++; },
    AbortController: function() { this.signal = {}; this.abort = function() {}; },
    IntersectionObserver: null
  };
  let page_ = null, kit = null, Logic = null;
  if (typeof page === 'string') {
    Logic = require(path.join(FRONTEND, 'static', SCRIPTS[page] + '.js'));
    page_ = Logic.Page(env);
  } else {
    kit = Common.DeviceKit(env, page.kit || {});
  }
  const flush = async () => { for (let i = 0; i < 8; i++) await new Promise((r) => setImmediate(r)); };
  const advance = async (ms) => {
    const end = now + ms;
    for (;;) {
      await flush();
      timers.sort((a, b) => a.at - b.at);
      const t = timers.find((x) => x.at <= end);
      if (!t)
        break;
      now = Math.max(now, t.at);
      if (t.every)
        t.at += t.every;
      else
        timers.splice(timers.indexOf(t), 1);
      t.f();
    }
    now = end;
    await flush();
  };
  return {env, page: page_, kit, Logic, Common, doc, calls, flush, advance, el: (id) => doc.getElementById(id),
          reloads: () => reloads, maxInFlight: () => maxInFlight, confirms: () => confirms};
}

const STATUS = {
  mode: 'BR_RUNNING', app: {version: '1.0', slot: 0}, storage_ok: true, rcp: {installed: '2.1', compatible: true},
  jumper: {present: true, base_expected: 'b1', base_installed: 'b1', recovery_failed: false},
  eth: {ipv4: '10.0.0.5', ipv6: ['fe80::1']}, pd: {state: 'running', prefix: null},
  sniffer: {active: false, ended_by_fault: false, channel: 0, frames: 0, drops: 0, elapsed_s: 0, duration_s: 0},
  fault_reason: null
};

const DIAG = {
  uptime_s: 27303, reset_reason: 'software',
  cpu: [{core: 0, load_pct: {'1s': 0.4, '10s': 0.5, '60s': 0.4}}, {core: 1, load_pct: {'1s': 0.6, '10s': 0.6, '60s': 0.5}}],
  memory: {internal: {free: 418713, min_free: 359116, largest_free_block: 249856},
           psram: {free: 31666995, min_free: 31352422, largest_free_block: 31457280}},
  ot_lock: {acquisitions: 416546, max_hold_us: 263196, longest_holder: 'ot_main', max_wait_us: 177285,
            last_60s: {acquisitions: 910, max_hold_us: 57854, longest_holder: 'ot_main', max_wait_us: 1758}},
  tasks: [
    {name: 'IDLE0', core: 0, priority: 0, state: 'ready', cpu_pct: {'1s': 99.6, '10s': 99.5, '60s': 99.6}, stack_free_min_bytes: 1164},
    {name: 'httpd', core: 1, priority: 5, state: 'blocked', cpu_pct: {'1s': 0.4, '10s': 0.5, '60s': 0.3}, stack_free_min_bytes: 2052},
    {name: 'ipc0', core: 0, priority: 24, state: 'suspended', cpu_pct: {'1s': 0, '10s': 0, '60s': 0}, stack_free_min_bytes: 508}
  ],
  tasks_untracked: 0
};

/* GET /device/network with a running single untagged DHCP configuration and no trial. */
const NETWORK = {
  mode: 'single',
  active: {mode: 'single', single: {vlan: 0, ipv4: {dhcp: true}}},
  trial: null,
  interfaces: [{name: 'eth', vlan: 0, link: true, ipv4: '192.168.1.117', ipv6: ['fe80::1', 'fdaa:1::1']}],
  vlan_rx_dropped: 0,
  fallback: false
};

/* Deep copy of a fixture, with `patch` merged at the top level. */
function fixture(obj, patch) { return Object.assign(JSON.parse(JSON.stringify(obj)), patch || {}); }

module.exports = {makeEnv, STATUS, DIAG, NETWORK, fixture, Common};
