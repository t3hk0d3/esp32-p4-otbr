/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP Thread Border Router - join an existing Thread network by its active dataset TLVs (Network page).
 * The pure logic is exported for node (tools/tests/test_network_page.py); the page code runs only in a browser.
 */
'use strict';

var DatasetJoin = (function() {
  var MAX_BYTES = 254; /* OT_OPERATIONAL_DATASET_MAX_LENGTH */
  /* TLVs of a complete active dataset with their allowed value lengths [type, name, min, max]; the board merges what it
   * gets into what it has stored, so a partial set would keep parts of the old network. */
  var REQUIRED = [
    [ 14, 'Active Timestamp', 8, 8 ], [ 0, 'Channel', 3, 3 ], [ 53, 'Channel Mask', 2, 255 ], [ 2, 'Ext PAN ID', 8, 8 ],
    [ 7, 'Mesh-Local Prefix', 8, 8 ], [ 5, 'Network Key', 16, 16 ], [ 3, 'Network Name', 1, 16 ], [ 1, 'PAN ID', 2, 2 ],
    [ 4, 'PSKc', 16, 16 ], [ 12, 'Security Policy', 3, 255 ]
  ];

  function hex(bytes) {
    var out = '';
    for (var i = 0; i < bytes.length; i++)
      out += (bytes[i] < 16 ? '0' : '') + bytes[i].toString(16);
    return out;
  }

  function utf8(bytes) {
    try {
      return decodeURIComponent(bytes.map(function(b) { return '%' + (b < 16 ? '0' : '') + b.toString(16); }).join(''));
    } catch (e) {
      return String.fromCharCode.apply(null, bytes);
    }
  }

  function panText(value) {
    var text = value.toString(16);
    while (text.length < 4)
      text = '0' + text;
    return '0x' + text;
  }

  function parseDatasetTlvs(text) {
    var compact = String(text === undefined || text === null ? '' : text).replace(/\s+/g, '').toLowerCase();
    if (!compact)
      return {ok : false, error : 'Paste the dataset TLVs (hex)'};
    if (!/^[0-9a-f]+$/.test(compact) || compact.length % 2)
      return {ok : false, error : 'Not a hex string'};
    var bytes = [];
    for (var i = 0; i < compact.length; i += 2)
      bytes.push(parseInt(compact.substr(i, 2), 16));
    if (bytes.length > MAX_BYTES)
      return {ok : false, error : 'Dataset too long (max ' + MAX_BYTES + ' bytes)'};
    var tlvs = {};
    for (var at = 0; at < bytes.length;) {
      if (at + 2 > bytes.length || at + 2 + bytes[at + 1] > bytes.length)
        return {ok : false, error : 'Malformed TLVs'};
      tlvs[bytes[at]] = bytes.slice(at + 2, at + 2 + bytes[at + 1]);
      at += 2 + bytes[at + 1];
    }
    var missing = REQUIRED.filter(function(r) { return !tlvs[r[0]]; }).map(function(r) { return r[1]; });
    if (missing.length)
      return {ok : false, error : 'Not a complete active dataset: missing ' + missing.join(', ')};
    var wrong = REQUIRED.filter(function(r) { return tlvs[r[0]].length < r[2] || tlvs[r[0]].length > r[3]; })
                    .map(function(r) { return r[1]; });
    if (wrong.length)
      return {ok : false, error : 'Malformed TLVs: wrong length of ' + wrong.join(', ')};
    var channel = tlvs[0], pan = tlvs[1];
    return {
      ok : true,
      hex : compact,
      network : {
        name : utf8(tlvs[3]),
        channel : (channel[1] << 8) | channel[2],
        panId : panText((pan[0] << 8) | pan[1]),
        extPanId : hex(tlvs[2])
      }
    };
  }

  function describeNetwork(network) {
    return network.name + ' (channel ' + network.channel + ', PAN ' + network.panId + ')';
  }

  function readCurrentNetwork(fetchFn) {
    return fetchFn('/node/dataset/active', {headers : {'Accept' : 'application/json'}})
        .then(function(r) {
          if (r.status !== 200)
            return null;
          return r.json().then(function(d) {
            return {
              name : d.networkName,
              channel : d.channel,
              panId : typeof d.panId === 'number' ? panText(d.panId) : d.panId
            };
          });
        })
        .catch(function() { return null; });
  }

  function statusOf(promise) {
    return promise.then(function(r) { return r; }, function() { return {status : 0, ok : false}; });
  }

  /* PUT the TLVs (the board restarts Thread itself when it runs); start Thread only when it is off, because enabling a
   * running Thread starts a fresh attach. */
  function joinWithDataset(hexTlvs, fetchFn) {
    var step = 'dataset';
    return statusOf(fetchFn('/node/dataset/active',
                            {method : 'PUT', headers : {'Content-Type' : 'text/plain'}, body : hexTlvs}))
        .then(function(put) {
          if (!put.ok)
            return {ok : false, step : 'dataset', status : put.status};
          step = 'state';
          return statusOf(fetchFn('/node/state')).then(function(state) {
            if (state.status !== 200)
              return {ok : false, step : 'state', status : state.status};
            return state.text().then(function(body) {
              var role = body;
              try {
                role = JSON.parse(body);
              } catch (e) {
              }
              if (role !== 'disabled')
                return {ok : true, started : false};
              step = 'enable';
              return statusOf(fetchFn('/node/state', {
                       method : 'PUT',
                       headers : {'Content-Type' : 'application/json'},
                       body : JSON.stringify('enable')
                     }))
                  .then(function(enable) {
                    return enable.ok ? {ok : true, started : true}
                                     : {ok : false, step : 'enable', status : enable.status};
                  });
            });
          });
        })
        .catch(function() { return {ok : false, step : step, status : 0}; });
  }

  function joinErrorMessage(step, status) {
    if (step === 'enable')
      return 'The dataset is stored, but Thread could not start (HTTP ' + status + '). Retry, or reboot the board.';
    if (status === 403)
      return 'Writes are refused on the management address. Open this page on the Thread backbone address.';
    if (status === 409)
      return 'The board is busy (sniffer running). Stop the capture and retry.';
    if (status === 503)
      return 'The radio is not available. See the Device page.';
    if (step === 'dataset' && status === 400)
      return 'The board rejected the dataset.';
    if (step === 'dataset' && status === 500)
      return 'The board could not apply the dataset (HTTP 500). Check that it is a valid active dataset; if the board ' +
             'left its network, see the Device page or reboot it.';
    if (status === 0)
      return 'Connection lost at step "' + step + '". Reload the page and check the board state.';
    return 'Join failed at step "' + step + '" (HTTP ' + status + ').';
  }

  return {
    parseDatasetTlvs : parseDatasetTlvs,
    describeNetwork : describeNetwork,
    readCurrentNetwork : readCurrentNetwork,
    joinWithDataset : joinWithDataset,
    joinErrorMessage : joinErrorMessage
  };
})();

if (typeof module !== 'undefined' && module.exports)
  module.exports = DatasetJoin;

/* The page: the "Join with Dataset" card of network.html. Uses the overlay helpers of network.html. */
if (typeof document !== 'undefined') {
  (function() {
    var L = DatasetJoin;
    var box = document.getElementById('datasetTlvs');
    var preview = document.getElementById('datasetPreview');
    var button = document.getElementById('datasetJoinBtn');
    var status = document.getElementById('datasetStatus');
    if (!box || !preview || !button || !status)
      return;
    var parsed = null;
    var pollTimer = null;

    function refresh() {
      parsed = L.parseDatasetTlvs(box.value);
      preview.textContent = parsed.ok ? 'Network: ' + L.describeNetwork(parsed.network) : (box.value.trim() ? parsed.error : '');
      preview.style.color = parsed.ok ? 'var(--success)' : 'var(--danger)';
      button.disabled = !parsed.ok;
    }

    function setStatus(ok, text) {
      status.textContent = text;
      status.style.color = ok ? 'var(--success)' : 'var(--danger)';
    }

    function poll() {
      var count = 0;
      if (pollTimer)
        clearInterval(pollTimer);
      pollTimer = setInterval(function() {
        if (++count > 30) {
          clearInterval(pollTimer);
          pollTimer = null;
          finishBlocking(false, 'Timed out — check device status');
          setStatus(false, 'Timed out — check device status');
          return;
        }
        fetch('/node/state')
            .then(function(r) { return r.json(); })
            .then(function(role) {
              role = typeof role === 'string' ? role : '';
              updateBlockingStatus('Attaching... (' + (role || 'detached') + ')');
              if (role === 'leader' || role === 'router' || role === 'child') {
                clearInterval(pollTimer);
                pollTimer = null;
                finishBlocking(true, 'Joined — role: ' + role);
                setStatus(true, 'Joined — role: ' + role);
              }
            })
            .catch(function() { /* transient while Thread restarts */ });
      }, 2000);
    }

    function join() {
      refresh();
      if (!parsed.ok)
        return;
      var target = parsed;
      button.disabled = true;
      L.readCurrentNetwork(fetch.bind(window)).then(function(current) {
        var question = 'Join ' + L.describeNetwork(target.network) + '?';
        if (current)
          question = 'This board is on ' + L.describeNetwork(current) + '. That network is replaced by ' +
                     L.describeNetwork(target.network) + '. Continue?';
        if (!window.confirm(question)) {
          button.disabled = false;
          return;
        }
        showBlockingOverlay('Joining Thread Network', 'Storing the dataset...');
        L.joinWithDataset(target.hex, fetch.bind(window)).then(function(result) {
          button.disabled = false;
          if (!result.ok) {
            var message = L.joinErrorMessage(result.step, result.status);
            finishBlocking(false, message);
            setStatus(false, message);
            return;
          }
          updateBlockingStatus('Dataset stored — attaching...');
          poll();
        });
      }).catch(function() {
        button.disabled = false;
        showBlockingOverlay('Joining Thread Network', ''); /* finishBlocking needs the overlay to exist */
        finishBlocking(false, 'Unexpected error — reload the page and check the board state');
        setStatus(false, 'Unexpected error — reload the page and check the board state');
      });
    }

    box.addEventListener('input', refresh);
    button.addEventListener('click', join);
    refresh();
  })();
}
