/*
 * ESP Thread Border Router - Names section of the Ethernet page (GET/PUT /device/names)
 *
 * NamesLogic is pure and runs under node too (tools/tests/test_names_page.py). NamesPage is the section: an always-open
 * form with its own Save, applied live by the board (no review, no reboot). The form is
 * {hostname, follows, ha_name, http_advert, dhcp_name} with the strings as typed.
 */
'use strict';

var NamesLogic = (function() {
var NAME_MAX = 63;
var DHCP_NAME_MAX = 32; // esp_netif refuses longer DHCP client names

/* The name without a pasted ".local" (or ".local."), case kept: the board stores it in lower case. */
function normaliseHostname(text) {
  return String(text).trim().replace(/\.local\.?$/i, '');
}

function utf8Bytes(text) {
  var n = 0;
  for (var i = 0; i < text.length; i++) {
    var c = text.charCodeAt(i);
    if (c < 0x80)
      n += 1;
    else if (c < 0x800)
      n += 2;
    else if (c >= 0xd800 && c <= 0xdbff) {
      n += 4; // a surrogate pair is one 4-byte character
      i++;
    } else
      n += 3;
  }
  return n;
}

/* The board's rules (netnames_rules_check): an empty name or a DNS label of at most `max`. null when valid. */
function labelError(name, max) {
  if (name === '')
    return null;
  if (name.length > max)
    return 'Longer than ' + max + ' characters.';
  if (!/^[A-Za-z0-9-]+$/.test(name))
    return 'Only letters, digits and hyphens are allowed.';
  if (name[0] === '-' || name[name.length - 1] === '-')
    return 'Must not start or end with a hyphen.';
  return null;
}

function haNameError(name) {
  if (name === '')
    return 'Enter a name, or tick Same as hostname.';
  if (utf8Bytes(name) > NAME_MAX)
    return 'Longer than 63 bytes (letters outside English take 2 to 4 bytes each).';
  if (name[0] === ' ' || name[name.length - 1] === ' ')
    return 'Must not start or end with a space.';
  if (/[\u0000-\u001f\u007f]/.test(name))
    return 'Must not contain control characters.';
  return null;
}

function fromApi(body) {
  return {
    hostname : body.hostname,
    follows : !!body.ha_name_follows_hostname,
    ha_name : body.ha_name,
    http_advert : !!body.http_advert,
    dhcp_name : body.dhcp_name
  };
}

/* {count, errors: {field: message}} with the API's field names. */
function validate(form) {
  var errors = {};
  var host = labelError(normaliseHostname(form.hostname), NAME_MAX);
  var dhcp = labelError(form.dhcp_name.trim(), DHCP_NAME_MAX);
  var ha = form.follows ? null : haNameError(form.ha_name);
  if (host)
    errors.hostname = host;
  if (ha)
    errors.ha_name = ha;
  if (dhcp)
    errors.dhcp_name = dhcp;
  return {count : Object.keys(errors).length, errors : errors};
}

function dirty(a, b) {
  return a.hostname !== b.hostname || a.follows !== b.follows || a.ha_name !== b.ha_name ||
         a.http_advert !== b.http_advert || a.dhcp_name !== b.dhcp_name;
}

function toBody(form) {
  return {
    hostname : normaliseHostname(form.hostname),
    ha_name_follows_hostname : form.follows,
    ha_name : form.ha_name,
    http_advert : form.http_advert,
    dhcp_name : form.dhcp_name.trim()
  };
}

/* The hostname the board means to use: the set one, or the default. */
function effectiveHostname(body) { return body.hostname || body.hostname_default; }

/* The mDNS library renamed the board after a conflict. */
function conflict(body) { return body.hostname_live !== effectiveHostname(body); }

/*
 * Where to go after a save: the same page on the new live name when this page was opened by the old live name; null
 * otherwise (opened by address, or the live name did not change).
 */
function redirectTarget(loc, before, after) {
  var host = String(loc.hostname || '').toLowerCase();
  if (host !== before.hostname_live + '.local' || after.hostname_live === before.hostname_live)
    return null;
  return loc.protocol + '//' + after.hostname_live + '.local' + (loc.port ? ':' + loc.port : '') + loc.pathname +
         (loc.search || '');
}

return {
  normaliseHostname : normaliseHostname,
  utf8Bytes : utf8Bytes,
  fromApi : fromApi,
  validate : validate,
  dirty : dirty,
  toBody : toBody,
  effectiveHostname : effectiveHostname,
  conflict : conflict,
  redirectTarget : redirectTarget
};
})();

/* opts.isBlocked(): true while the Ethernet page runs a trial or waits for its reboot. */
function NamesPage(env, opts) {
var C = typeof DeviceCommon !== 'undefined' ? DeviceCommon : require('./device_common.js');
var L = NamesLogic;
var SETTLE_MS = 2000; // the mDNS library probes a new name for about a second before it answers to it
var FIELDS = {hostname : 'nm-hostname', ha_name : 'nm-ha', dhcp_name : 'nm-dhcp'};
var api = null;   // last GET /device/names
var saved = null; // form of `api`
var form = null;  // what the person edits
var serverErrors = {};
var saving = false;
// Its own kit only for its helpers: the Ethernet page owns the trial banner and the busy state.
var kit = C.DeviceKit(env, {trial : false});
var $ = kit.$, el = kit.el, setText = kit.setText, show = kit.show, note = kit.note;
opts = opts || {};

function blocked() { return saving || !!(opts.isBlocked && opts.isBlocked()); }

function setValue(id, v) {
  if ($(id).value !== v)
    $(id).value = v;
}

function messages(id, text) {
  var box = $(id);
  if (box.getAttribute('data-shown') === (text || ''))
    return;
  box.setAttribute('data-shown', text || '');
  kit.clear(box);
  if (text)
    box.appendChild(el('p', text, 'error-text'));
}

function render() {
  if (!form)
    return;
  var off = blocked(), v = L.validate(form), split = api.mode === 'split';
  var haShown = form.follows ? L.normaliseHostname(form.hostname).toLowerCase() || api.hostname_default : form.ha_name;
  setValue('nm-hostname', form.hostname);
  $('nm-follows').checked = form.follows;
  setValue('nm-ha', haShown);
  $('nm-http').checked = form.http_advert;
  setValue('nm-dhcp', form.dhcp_name);
  [ 'nm-hostname', 'nm-follows', 'nm-dhcp' ].forEach(function(id) { $(id).disabled = off; });
  $('nm-ha').disabled = off || form.follows;
  $('nm-http').disabled = off || !api.http_advert_available;
  show('nm-http-split', !api.http_advert_available);
  show('nm-hostname-split', split);
  setText('nm-hostname-hint', 'Letters, digits and hyphens, up to 63. Leave empty for the default ' +
                                  api.hostname_default + '.');
  setText('nm-dhcp-hint', 'What your router lists for this board: letters, digits and hyphens, up to 32. Leave empty ' +
                              'for the default ' +
                              api.dhcp_name_default + '. The router sees it at the lease renewal the board asks for on ' +
                              'Save; it only matters with an automatic (DHCP) address.');
  Object.keys(FIELDS).forEach(function(f) { messages(FIELDS[f] + '-err', v.errors[f] || serverErrors[f]); });
  var renamed = L.conflict(api);
  show('nm-announced', !renamed);
  show('nm-renamed', renamed);
  setText('nm-conflict', renamed ? 'Another device on the network already uses ' + L.effectiveHostname(api) +
                                       '.local, so the board answers as ' + api.hostname_live +
                                       '.local. Pick a different name, or rename the other device.'
                                 : '');
  show('nm-conflict', renamed);
  var changed = L.dirty(saved, form);
  // After a conflict rename, saving the same name again asks the board to try it again.
  $('nm-save').disabled = off || !(changed || renamed) || v.count > 0;
  $('nm-discard').disabled = off || !changed;
}

function load(body) {
  var editing = form && saved && L.dirty(saved, form);
  api = body;
  saved = L.fromApi(body);
  // A poll never overwrites what someone is typing.
  if (!editing)
    form = JSON.parse(JSON.stringify(saved));
}

function refresh() {
  return kit.request('GET', '/device/names', {timeoutMs : 5000}).then(function(r) {
    if (r.status !== 200 || !r.body) {
      note('nm-msg', 'The names could not be read: ' + kit.failure(r), true);
      return false;
    }
    load(r.body);
    render();
    return true;
  }, function() {
    note('nm-msg', 'The names could not be read: no answer from the device.', true);
    return false;
  });
}

function edit(fn) {
  return function(ev) {
    if (!form || blocked())
      return;
    fn(ev);
    serverErrors = {};
    note('nm-msg', '', false);
    render();
  };
}

function discard() {
  if (!saved || blocked())
    return;
  form = JSON.parse(JSON.stringify(saved));
  serverErrors = {};
  note('nm-msg', '', false);
  render();
}

function settleThenReload(before, reply) {
  return new Promise(function(resolve) { env.setTimeout(resolve, SETTLE_MS); }).then(function() {
    return kit.request('GET', '/device/names', {timeoutMs : 5000});
  }).then(function(r) { return r.status === 200 && r.body ? r.body : reply; }, function() {
    // The old name may already be gone (the board says goodbye for it): the PUT reply has the live name too.
    return reply;
  }).then(function(after) {
    var target = L.redirectTarget(env.location || {}, before, after);
    if (target) {
      note('nm-msg', 'Saved. Moving to ' + target + ' ...', false);
      env.navigate(target);
      return;
    }
    form = null;
    load(after);
  });
}

function save() {
  if (!form || blocked() || L.validate(form).count || !(L.dirty(saved, form) || L.conflict(api)))
    return Promise.resolve(null);
  var before = api;
  saving = true;
  render();
  return kit.request('PUT', '/device/names', {headers : {'Content-Type' : 'application/json'},
                                               body : JSON.stringify(L.toBody(form)), timeoutMs : 10000})
      .then(function(r) {
        if (r.status === 400 && r.body && FIELDS[r.body.field]) {
          serverErrors = {};
          serverErrors[r.body.field] = r.body.detail;
          note('nm-msg', 'The board refused the names: see the field above.', true);
          return null;
        }
        if (r.status !== 200 || !r.body) {
          note('nm-msg', kit.failure(r), true);
          return null;
        }
        if (r.body.applied === false)
          note('nm-msg', 'Saved, but not everything took effect yet: ' + r.body.detail, true);
        else
          note('nm-msg', 'Saved.', false);
        return settleThenReload(before, r.body);
      }, function(e) { note('nm-msg', e.message, true); })
      .then(function() {
        saving = false;
        render();
      });
}

function init() {
  $('nm-hostname').addEventListener('input', edit(function(ev) { form.hostname = ev.target.value; }));
  $('nm-follows').addEventListener('change', edit(function(ev) {
    form.follows = ev.target.checked;
    if (!form.follows && form.ha_name === '')
      form.ha_name = api.ha_name_effective;
  }));
  $('nm-ha').addEventListener('input', edit(function(ev) { form.ha_name = ev.target.value; }));
  $('nm-http').addEventListener('change', edit(function(ev) { form.http_advert = ev.target.checked; }));
  $('nm-dhcp').addEventListener('input', edit(function(ev) { form.dhcp_name = ev.target.value; }));
  $('nm-discard').addEventListener('click', discard);
  $('nm-save').addEventListener('click', save);
  kit.poller(function() { return saving ? Promise.resolve(true) : refresh(); }, 10000, 'namesSection');
}

return {init : init, refresh : refresh, save : save, render : render, kit : kit};
}

if (typeof module !== 'undefined' && module.exports) {
  module.exports = NamesLogic;
  module.exports.Page = NamesPage;
}

if (typeof document !== 'undefined')
  NamesPage(DeviceCommon.DeviceEnv(), {
    isBlocked : function() { return typeof ethernetPage !== 'undefined' && ethernetPage.isBlocked(); }
  }).init();
