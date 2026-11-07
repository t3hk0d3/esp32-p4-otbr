/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP Thread Border Router - Shared API utilities
 */
'use strict';

/**
 * Escape HTML special characters to prevent XSS when inserting
 * device-supplied strings into innerHTML.
 */
function escapeHtml(str) {
  if (typeof str !== 'string')
    str = String(str);
  return str.replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;')
      .replace(/'/g, '&#39;');
}

/*
 * Reply of the Thread web GUI as an object. A refusal (4xx/5xx) resolves to {error: <HTTP status>, message} with the
 * reason the board gave, so callers that check `error` and show `message` explain it; it is never cached.
 */
function apiFailureText(status, body) {
  var reason = body && typeof body === 'object' ? (body.message || body.ErrorMessage || body.detail || '') : '';
  if (status === 403)
    return 'Refused on the management address: open the web UI on the Thread backbone address for Thread changes.';
  if (status === 409)
    return 'The board is busy' + (reason ? ' (' + reason + ')' : '') + '.';
  if (status === 503)
    return 'The Thread network is not running' + (reason ? ' (' + reason + ')' : '') + '. See the Dashboard.';
  return reason || ('Request failed (HTTP ' + status + ')');
}

function apiReply(r) {
  return r.text().then(function(text) {
    var body = null;
    try {
      body = JSON.parse(text);
    } catch (e) {
      if (r.ok)
        throw e;
    }
    if (!r.ok)
      return {error : r.status, message : apiFailureText(r.status, body)};
    return body;
  });
}

function apiGet(url) {
  return fetch(url).then(function(r) {
    return apiReply(r).then(function(data) {
      if (r.ok) {
        try {
          sessionStorage.setItem('cache:' + url, JSON.stringify(data));
        } catch (e) {
        }
      }
      return data;
    });
  });
}

function apiPost(url, data) {
  return fetch(url, {
           method : 'POST',
           headers : {'Content-Type' : 'application/json'},
           body : JSON.stringify(data)
         })
      .then(apiReply);
}

/**
 * Get cached response for a GET endpoint, or null if not available.
 * Use this to render immediately on page load before the live fetch completes.
 */
function getCached(url) {
  try {
    var raw = sessionStorage.getItem('cache:' + url);
    return raw ? JSON.parse(raw) : null;
  } catch (e) {
    return null;
  }
}

function showToast(msg, type) {
  var t = document.getElementById('_toast');
  if (!t) {
    t = document.createElement('div');
    t.id = '_toast';
    document.body.appendChild(t);
  }
  t.textContent = msg;
  t.className = 'toast toast-' + (type || 'info') + ' show';
  clearTimeout(t._tid);
  t._tid = setTimeout(function() { t.classList.remove('show'); }, 3500);
}

function showLoading(show) {
  var o = document.getElementById('_loading');
  if (!o) {
    o = document.createElement('div');
    o.id = '_loading';
    o.className = 'loading';
    o.innerHTML =
        '<div class="spinner" style="width:40px;height:40px"></div><div>Loading...</div>';
    document.body.appendChild(o);
  }
  if (show)
    o.classList.add('active');
  else
    o.classList.remove('active');
}

/*
 * Build navigation bar and highlight current page. Top bar: Dashboard, Thread, [Zigbee slot], Device, About. The slot
 * is an empty hidden <li id="nav-slot-zigbee"> (radio-mux, patch 29) that stays empty: Zigbee status is a section of
 * the Dashboard. The Thread menu is hidden while no radio is in Thread border router mode (GET /device/status
 * otbr_channel null); the last answer is remembered in sessionStorage so the menu does not flash on the next page.
 */
(function() {
var pages = [
  {href : '/index.html', label : 'Dashboard'}, {
    id : 'nav-thread',
    label : 'Thread',
    children : [
      {href : '/network.html', label : 'Network'},
      {href : '/topology.html', label : 'Topology'},
      {href : '/routing.html', label : 'Routing'},
      {href : '/commission.html', label : 'Commissioner'},
      {href : '/addresses.html', label : 'Addresses'},
      {href : '/tools.html', label : 'Tools'},
      {href : '/console.html', label : 'Console'}
    ]
  },
  {slot : 'nav-slot-zigbee'}, {
    label : 'Device',
    children : [
      {href : '/radios.html', label : 'Radios'},
      {href : '/ethernet.html', label : 'Ethernet'},
      {href : '/firmware.html', label : 'Firmware'},
      {href : '/diagnostics.html', label : 'Diagnostics'},
      {href : '/capture.html', label : 'Packet capture'}
    ]
  },
  {href : '/about.html', label : 'About'}
];
var HIDE_KEY = 'navThreadHidden';

/* Shows or hides the Thread menu (also called by the Radios page after a change). */
function setThreadMenuHidden(hidden) {
  var li = document.getElementById('nav-thread');
  if (li)
    li.hidden = hidden;
  try {
    sessionStorage.setItem(HIDE_KEY, hidden ? '1' : '0');
  } catch (e) {
  }
}
if (typeof window !== 'undefined')
  window.navSetThreadMenuHidden = setThreadMenuHidden;

/* Asks the board once; no answer, or a board without otbr_channel, leaves the Thread menu as it is. */
function watchThreadMenu() {
  if (typeof fetch !== 'function')
    return;
  fetch('/device/status')
      .then(function(r) { return r.ok ? r.text() : null; })
      .then(function(text) {
        var s = text ? JSON.parse(text) : null;
        if (s && s.otbr_channel !== undefined)
          setThreadMenuHidden(s.otbr_channel === null);
      })
      .catch(function() {});
}
var nav = document.getElementById('nav');
if (!nav)
  return;
var p = location.pathname;

var threadHidden = false;
try {
  threadHidden = sessionStorage.getItem(HIDE_KEY) === '1';
} catch (e) {
}

var html =
    '<a href="/" class="nav-brand"><img src="/favicon.ico" width="22" height="22" alt=""> Thread Border Router</a><ul class="nav-links">';
for (var i = 0; i < pages.length; i++) {
  var pg = pages[i];
  if (pg.slot) {
    html += '<li id="' + pg.slot + '" hidden></li>';
  } else if (pg.children) {
    /* Check if any child is active */
    var childActive = false;
    for (var c = 0; c < pg.children.length; c++) {
      if (p === pg.children[c].href) {
        childActive = true;
        break;
      }
    }
    html += '<li class="nav-dropdown"' + (pg.id ? ' id="' + pg.id + '"' : '') +
            (pg.id && threadHidden ? ' hidden' : '') + '>';
    html += '<a href="#" aria-haspopup="true" aria-expanded="false" class="nav-dropdown-toggle' +
            (childActive ? ' active' : '') + '">' + pg.label + '</a>';
    html += '<ul class="nav-dropdown-menu">';
    for (var j = 0; j < pg.children.length; j++) {
      var ch = pg.children[j];
      var active = (p === ch.href) ? ' class="active"' : '';
      html += '<li><a href="' + ch.href + '"' + active + '>' + ch.label +
              '</a></li>';
    }
    html += '</ul></li>';
  } else {
    var active = (p === pg.href || (p === '/' && pg.href === '/index.html'))
                     ? ' class="active"'
                     : '';
    html +=
        '<li><a href="' + pg.href + '"' + active + '>' + pg.label + '</a></li>';
  }
}
html += '</ul>';
nav.innerHTML = html;
watchThreadMenu();

/* A tap opens a menu too (touch screens have no hover); a tap elsewhere closes it. */
if (!nav.querySelectorAll)
  return; // the nav tests run this file against a bare object
function closeMenus(except) {
  var open = nav.querySelectorAll('.nav-dropdown.open');
  for (var k = 0; k < open.length; k++) {
    if (open[k] !== except) {
      open[k].classList.remove('open');
      open[k].firstChild.setAttribute('aria-expanded', 'false');
    }
  }
}
var toggles = nav.querySelectorAll('.nav-dropdown-toggle');
for (var t = 0; t < toggles.length; t++) {
  toggles[t].addEventListener('click', function(ev) {
    ev.preventDefault();
    var li = this.parentNode;
    closeMenus(li);
    var on = li.classList.toggle('open');
    this.setAttribute('aria-expanded', on ? 'true' : 'false');
  });
}
document.addEventListener('click', function(ev) {
  if (!nav.contains(ev.target))
    closeMenus(null);
});
/* Hover and keyboard focus open a menu by CSS: keep aria-expanded in step. */
var drops = nav.querySelectorAll('.nav-dropdown');
function syncExpanded(li, on) {
  li.firstChild.setAttribute('aria-expanded', on || li.classList.contains('open') ? 'true' : 'false');
}
for (var d = 0; d < drops.length; d++) {
  drops[d].addEventListener('mouseenter', function() { syncExpanded(this, true); });
  drops[d].addEventListener('mouseleave', function() { syncExpanded(this, false); });
  drops[d].addEventListener('focusin', function() { syncExpanded(this, true); });
  drops[d].addEventListener('focusout', function(ev) {
    if (!this.contains(ev.relatedTarget))
      syncExpanded(this, false);
  });
}
})();
