'use strict';
/*
 * Test harness for static/device.js (used by test_device_page.py, run with node): a small fake DOM built from the ids
 * of device.html, a fake fetch that records requests, and a manual clock with timers.
 */
const fs = require('fs');
const path = require('path');

const FRONTEND = path.join(__dirname, '..', '..', 'p4', 'components', 'esp_ot_br_server', 'frontend');
const Device = require(path.join(FRONTEND, 'static', 'device.js'));

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
  get firstChild() { return this.children[0] || null; }
  appendChild(c) { this.children.push(c); return c; }
  removeChild(c) { this.children = this.children.filter((x) => x !== c); return c; }
  setAttribute(k, v) { this.attrs[k] = String(v); }
  getAttribute(k) { return this.attrs[k]; }
  addEventListener(t, f) { (this.listeners[t] = this.listeners[t] || []).push(f); }
  click() {
    const ev = {defaultPrevented: false, preventDefault() { this.defaultPrevented = true; }};
    (this.listeners.click || []).forEach((f) => f(ev));
    return ev;
  }
  hidden() { return this.classes.has('hidden'); }
}

class Doc {
  constructor(html) {
    this.byId = {};
    this.hidden = false;
    this.listeners = {};
    this.fieldsets = [];
    const tagRe = /<(\w+)([^>]*)>/g;
    let m;
    while ((m = tagRe.exec(html))) {
      const attrs = m[2];
      const id = /\bid="([^"]+)"/.exec(attrs);
      const cls = /\bclass="([^"]*)"/.exec(attrs);
      const iface = /data-iface="([^"]+)"/.exec(attrs);
      if (id || iface) {
        const e = new El(this, m[1], id && id[1], cls ? cls[1].split(/\s+/) : []);
        if (iface) {
          e.attrs['data-iface'] = iface[1];
          this.fieldsets.push(e);
        }
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
  querySelectorAll(sel) {
    if (sel !== 'fieldset[data-iface]')
      throw new Error('unsupported selector ' + sel);
    return this.fieldsets;
  }
  addEventListener(t, f) { (this.listeners[t] = this.listeners[t] || []).push(f); }
  fire(t) { (this.listeners[t] || []).forEach((f) => f()); }
}

/* routes: function(url, opts) -> {status, body} | throws to simulate a dropped connection */
function makeEnv(routes) {
  const html = fs.readFileSync(path.join(FRONTEND, 'device.html'), 'utf8');
  const doc = new Doc(html);
  const calls = [];
  const timers = [];
  let now = 1000000;
  let seq = 0;
  let reloads = 0;
  let inFlight = 0;
  let maxInFlight = 0;
  const env = {
    document: doc,
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
    confirm() { return true; },
    reload() { reloads++; },
    AbortController: function() { this.signal = {}; this.abort = function() {}; },
    IntersectionObserver: null
  };
  const page = Device.DevicePage(env);
  const flush = async () => { for (let i = 0; i < 5; i++) await new Promise((r) => setImmediate(r)); };
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
  return {env, page, doc, calls, flush, advance, el: (id) => doc.getElementById(id),
          reloads: () => reloads, maxInFlight: () => maxInFlight, Device};
}

const STATUS = {
  mode: 'BR_RUNNING', app: {version: '1.0', slot: 0}, rcp: {installed: '2.1', compatible: true},
  jumper: {present: true, base_expected: 'b1', base_installed: 'b1', recovery_failed: false},
  eth: {ipv4: '10.0.0.5', ipv6: ['fe80::1']}, pd: {state: 'running', prefix: null},
  sniffer: {active: false, ended_by_fault: false, channel: 0, frames: 0, drops: 0, elapsed_s: 0, duration_s: 0},
  fault_reason: null
};

module.exports = {makeEnv, STATUS, Device};
