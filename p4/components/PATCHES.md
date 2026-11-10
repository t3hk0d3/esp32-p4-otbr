# Patches to vendored components

Vendored from:

- Repository: https://github.com/espressif/esp-thread-br
- Commit: 1f5af46d0ccb22fe01465ba6d16a33b276c20c1b
- Components: `components/esp_ot_br_server` -> `p4/components/esp_ot_br_server`,
  `components/esp_br_http_ota` -> `p4/components/esp_br_http_ota` (copied unchanged with
  `git archive`, not from a working tree)

Vendored code keeps upstream formatting and is excluded from pre-commit. Every change to it is
listed here. Numbering follows the spec (`docs/superpowers/specs/2026-10-30-esp32-p4-otbr-design.md`
section 5.8); the task that applies a patch adds its entry. Entry format: title, files touched,
reason, spec reference.

## Patches

1. Applied (Task 14). `esp_ot_br_server`: `httpd_handle_t esp_br_web_get_server(void)` (`include/esp_br_web.h`, which now
   includes `esp_http_server.h`; `CMakeLists.txt` gets `REQUIRES esp_http_server`) returns the running server's handle
   (NULL before the start), and `start_esp_br_http_server` sets `config.core_id = 1` so the HTTP server task runs on the
   management core (spec 5.11). It applies to the early start from `app_main` (patch 8). (spec 5.8)
2. Applied (Task 14). `esp_ot_br_server`: mode guard for everything that touches OpenThread. New files
   `private_include/esp_br_guard.h`, `src/esp_br_guard.c` (pure decision: `esp_br_guard_status(class, mode, ot_ready)`
   gives 0, 409 or 503; host test in `test_host/`). Call-site edits: `src/esp_br_web.c` (dispatcher
   `esp_otbr_thread_rest_handler`: lock-free pre-check replaces the patch 9 ready check; new glue `guard_now`,
   `guard_lock`, `guard_check_thread_op`, `guard_refuse_gui`, `guard_refuse_resource` and the macros `GUARD_PRECHECK`,
   `GUARD_CHECKED_CALL` after `httpd_send_packet`; one or two lines at the top of every handler in `s_web_gui_handlers`
   and `s_resource_handlers` that touches OpenThread), `src/esp_br_web_api.c` (`esp_br_thread_rest_request` checks again
   right after taking the OpenThread lock; `handle_openthread_join_network_request` re-checks every time it holds the lock
   again), `CMakeLists.txt` (`br_core` in `PRIV_REQUIRES`: a cycle with `br_core`'s own `PRIV_REQUIRES esp_ot_br_server`,
   which IDF accepts; a `linux` branch that only builds the guard for the host test), `tests/` (stubs for the new
   calls, five guard scenarios, `run_host_tests.py` adds `src/esp_br_guard.c` and two include paths). Reason: Thread
   changes and radio use are refused in SNIFFER (409) and RADIO_FAULT/STARTING (503), atomically with respect to mode
   changes: `br_core` changes the mode while holding the OpenThread lock, so a check made while holding the same lock
   cannot interleave. Error formats are the family's own: Thread REST `{"status","title","detail"}`
   (`thread_rest_send`), Web GUI `{"error","result","message"}` (`pack_response`, `error` = `OT_ERROR_INVALID_STATE`),
   resources `{"ErrorCode","ErrorMessage"}` (`resource_status`). Details: 409 `sniffer active`, 503
   `radio not available`. (spec 5.7, 5.8)
   `GUARD_CHECKED_CALL` (form, add/delete prefix, commission, add/delete ipaddr) checks the mode under the
   OpenThread lock and releases it before the `handle_*` call, which takes the lock itself (2026-11-02; it was
   `GUARD_LOCKED_CALL` and held the lock around the call). Holding it there held it twice: ESP-IDF's `otPlatUdpSocket`
   gives the task switching lock back once while it waits for the lwIP thread, which needs that lock to send into the
   Thread interface, so Form network (Thread up opens the DNS-SD socket while the multicast leaves of the interface down
   are queued) deadlocked the web server, lwIP, OpenThread and the event loop without a watchdog reset. A capture started
   between the check and the call is interrupted by it; the window is the time to take the lock again. Test:
   `tools/tests/test_web_gui_locking.py`.
3. Applied (Task 20; split into pages 2026-11). `esp_ot_br_server`: device pages. Rewritten upstream file
   `frontend/index.html` (the Dashboard becomes the status page: our status cards plus the upstream Thread network,
   identifiers and address table, read from `/get_properties` and `/ipaddr` only in BR_RUNNING; the PSKc is masked until
   "Show"; the upstream inline script, Refresh button and copyright footer are gone). New files `frontend/ethernet.html`,
   `frontend/firmware.html`, `frontend/capture.html`, `frontend/diagnostics.html`, and their scripts `frontend/static/status.js`, `ethernet.js`, `firmware.js`, `capture.js`, `diagnostics.js` on top of the
   shared `frontend/static/device_common.js` (pure logic exported for node, page code guarded by `typeof document`). Edits:
   `frontend/static/api.js`, a "Device" dropdown with the four device pages in the `pages` array that builds the navigation bar of
   every page (no other page needs a change); `frontend/static/style.css`, new classes appended after the upstream rules
   (no upstream rule changed) and `:focus-within` on the nav dropdowns so they open from the keyboard. The pages only use
   `/device/*`, so they render in every mode, also where the Thread REST reads answer 503; they poll only while the tab is
   shown and the watched section is on screen, one request at a time. They are served by the `/*` static handler: the
   device routes are exact `/device/...` paths. Their footer links the source code
   (https://github.com/t3hk0d3/esp32-p4-otbr), which the AGPL asks a network-facing modified version to offer. (spec 5.7,
   5.8; docs/superpowers/specs/2026-11-02-device-pages-design.md)
   2026-11-03: `frontend/ethernet.html` gets the "Thread over Ethernet" (TREL) card, `static/ethernet.js` its logic
   (`trelOnly`, `trelView`, `setTrel`); it also polls `/device/status` for the TREL state. No CSS change.
   (docs/superpowers/specs/2026-11-03-trel-design.md)
   2026-11-06: `frontend/index.html` and `static/status.js` get a "Chip temperature" row (`t-chip-temp`) in the "Load and memory"
   card, read from `chip_temperature_c` of `/device/diag`. No CSS change.
4. Not applied. Not needed: Phase 0 showed `esp_ot_br_server` links without Wi-Fi. (spec 5.8)
5. Applied (part a Task 11B, part b Task 13). `esp_br_http_ota`: (a) minimal RCP strip: removed the
   `esp_rcp_ota_*` / `esp_rcp_firmware.h` use and the `esp_rcp_update` entry in `REQUIRES`. Reason: the RCP firmware
   is no longer bundled with the P4 image (R14). (b) Paired bundle OTA: the download loop of
   `src/esp_br_http_ota.c` now runs on a source-agnostic streaming core. Files: new `src/ota_bundle.c`,
   `include/ota_bundle.h` (44-byte header parser, no IDF flash API), new `src/ota_stream.c`, `include/ota_stream.h`
   (chunk state machine: header, web image, app image; flash backend behind an ops struct, so the host tests cover
   arbitrary chunking, truncation at every offset and the commit ordering), new `src/esp_br_ota.c` (flash backend and
   single-update guard: `esp_br_ota_begin/write/end/abort`; erases the inactive `web_M`, writes it raw, reads it back
   and compares SHA-256 before the app image is opened, `esp_ota_begin(OTA_WITH_SEQUENTIAL_WRITES)` (progressive erase, no multi-second stall), `esp_ota_end` image
   validation, `esp_ota_set_boot_partition(ota_M)` as the only commit; refuses a second update with
   `ESP_ERR_INVALID_STATE`, also while a committed update waits for its reboot), new `src/esp_br_ota_upload.c`
   (`esp_br_ota_from_httpd`, 4 KB chunks), `include/esp_br_http_ota.h` (new API; test-build-only
   `esp_br_ota_test_set_commit_delay_ms`), `CMakeLists.txt` (host-test branch for the pure files, `esp_http_server`,
   `esp_partition`, `mbedtls`). Changes to upstream `src/esp_br_http_ota.c`: only `download_ota_image` (calls the
   core instead of `esp_ota_*`, the single-update claim is taken before the HTTP connect; a read that returns 0 before the data is complete now fails instead of looping).
   Not applied: the RCP-mutual-exclusion with `br_core_push_radio_firmware` is Task 15 (`device_api` serialises both).
   Task 24 (final review I3): `esp_br_ota_begin` also refuses, before anything is erased, while the running image is
   pending verification (first 60 s after an update; `web_M` is then the rollback target's web slot). The decision is
   the pure `ota_stream_start_check` (`src/ota_stream.c`); the refusal is the new code `ESP_BR_OTA_ERR_NOT_CONFIRMED`
   (`include/ota_bundle.h`), which `device_api` answers with 409 `busy` and a retry hint.
   (spec 5.6, 5.8)
6. Not applied, not needed (V3 decision, measured). Intended: copy data under the OpenThread lock and build JSON after
   release, if the REST handlers hold the lock over 20 ms. Measured on the board: during repeated `/topology` the
   longest hold in the last 60 s is 61 ms and always by `ot_main` (OpenThread's own task, 45-80 ms also without any
   load), never by a REST handler; the topology handler holds the lock only to send two Diagnostic Get messages and
   builds its JSON outside it (under its own semaphore); a tight loop of `GET /node` (30 000 lock acquisitions) adds
   nothing to the p99 of pings through the board (61 ms vs 74 ms idle). The topology request does add 100-125 ms to
   the p99 (the Diagnostic Get traffic and its processing in OpenThread), which patch 6 would not change. (spec 5.11, 5.8)
7. Applied. build: drop web_storage partition image creation. Files: `esp_ot_br_server/CMakeLists.txt`
   (removed the `spiffs_create_partition_image(web_storage ...)` line). Reason: the P4 partition
   table (spec 5.1) has `web_0`/`web_1` only; Task 6 packs the vendored `frontend/` directory into
   `web_0` from `p4/CMakeLists.txt`. (spec 5.1, 5.8)
8. Applied. `esp_ot_br_server/src/esp_br_web.c`: `esp_br_web_start` also calls `start_web_server`
   immediately, besides registering the IP event handlers, and a failing
   `mdns_register_hostname_changed_callback` is a warning instead of `ESP_ERROR_CHECK`. Reason: the HTTP server
   must listen in every mode, without an Ethernet link and without mDNS, because app validity (`validity.c`) is
   gated on `esp_br_web_is_server_started()`. Known limitation: `start_web_server` tests and sets its started flag
   non-atomically (check, start, store). `app_main` calls it before the network exists, so the later IP-event
   calls only see the flag already set; two simultaneous first calls could still start two servers. (spec 5.3 steps 6 and 7)
9. Applied. `esp_ot_br_server`: Thread REST API guard at its single choke point. New
   `esp_br_web_set_openthread_ready()` / `esp_br_web_openthread_ready()` (`include/esp_br_web.h`, flag set by
   `br_core` only after OpenThread is fully initialised and cleared on a radio fault). `esp_otbr_thread_rest_handler`
   answers 503 `{"error": ..., "detail": "radio not available"}` while the flag is false and creates the REST worker
   (`esp_br_thread_rest_init`) lazily on the first request once it is true; `esp_otbr_thread_rest_register` no longer
   calls it at server start (the server starts before OpenThread exists). (spec 5.3 step 6, 5.4)
10. Applied (Task 15). `esp_ot_br_server`: `void esp_br_web_set_extra_handlers(esp_err_t (*cb)(httpd_handle_t))`
   (`include/esp_br_web.h`, `src/esp_br_web.c`); `start_esp_br_http_server` calls the callback after the Thread REST
   and GUI handlers and before the catch-all `/*` GET handler. Reason: `/*` is registered with the wildcard matcher and
   httpd refuses (and would shadow) any GET path registered after it, so `GET /device/status` and
   `GET /device/sniffer.pcap` could not be added from `app_main` after `esp_br_web_start`. `app_main` sets
   `device_api_register` as the callback before starting the server. (spec 5.7)
   `EXTRA_HANDLER_SLOTS` (the handlers the callback and the catch-all may add to `max_uri_handlers`) was 20 since the
   network names (`GET`/`PUT /device/names`, spec 2026-11-03-network-names-design.md) and is 24 since mesh routing
   (patch 19). The leader weight (`GET`/`PUT /device/thread`) uses 2 of them: 22 routes plus the catch-all. It is 27
   since the Thread console (patch 24): 25 routes plus the catch-all. It is 30 since self-update (patch 26): 27 routes
   plus the catch-all.
11. Applied (Task 18). `esp_ot_br_server`: the Thread REST background worker `thread_rest` (`rest_process_task`,
   `src/esp_br_web_api.c`) is created with `xTaskCreatePinnedToCore(..., 1)` instead of `xTaskCreate`. Reason: it takes
   the OpenThread lock every 100 ms and was unpinned, so it could run on core 0 next to the network path; it belongs to
   the management core (spec 5.11). The literal 1 is `MGMT_CORE` (`common/board_cores`), which the vendored component
   does not include. Patch 6 (lock hold times) is a separate, conditional decision, see V3 notes in the task 18 report.
   Task 24 (final review I1): one worker iteration is now `rest_process_once()`, which returns before taking the
   OpenThread lock while `esp_br_web_openthread_ready()` is false. `br_core` clears the flag before it tears OpenThread
   down (which deletes the lock) for a radio push or recovery; the worker used to block on the freed lock every 100 ms.
   The guard is still re-checked under the lock in `rest_poll_guarded()` (patch 2). Host test: `tests/test_rest.c`
   `test_worker_does_not_touch_the_lock_when_not_ready` (counts lock acquisitions in `tests/host_platform.c`).
12. Applied (Task 19). `esp_ot_br_server`: per-interface policy (spec 5.13). `src/esp_br_web.c` includes `netcfg.h` and defines
   `NETCFG_GATE(req, svc)`, which calls `netcfg_request_allowed(req, svc)` and fails the handler (`ESP_FAIL`: the server closes the
   connection after the 403 that netcfg sent) when the service is not served on the interface the request arrived on. First line of:
   the Thread REST dispatcher `esp_otbr_thread_rest_handler` (before anything of the request is read; `GET`/`HEAD` ->
   `NETCFG_SVC_REST_READ`, every other method -> `NETCFG_SVC_REST_WRITE`), and with `NETCFG_SVC_GUI` the static file handler
   `default_urls_get_handler` (also serves `/favicon.ico` and the web pages), all 13 handlers of `s_web_gui_handlers` and the two of
   `s_resource_handlers` (`esp_otbr_network_diagnostics_get_handler`, `esp_otbr_well_known_br_rest_get_handler`). `CMakeLists.txt`:
   `netcfg` in `requires`. Single mode (the default) allows everything without looking at the socket, so an untagged default boot
   serves exactly as before. `device_api` does its own gate (`NETCFG_SVC_DEVICE_API`) in its own table. Reason: GUI and `/device/*`
   only on `mgmt`, the full Thread REST API on `thread_bb`, read-only `GET /node/*` and `/api/*` on both (the upstream GUI calls
   `/node/state`). `GET /node/*` reads including the dataset TLVs are READ class and allowed on `mgmt` (ruling: the method mapping stands; Task 21/V3 checks
   whether that exposure is acceptable). `tests/run_host_tests.py` (`tools/run_vendored_rest_tests.sh`) takes the route-slot defines and
   the `NETCFG_GATE` macro from the source and stubs `xTaskCreatePinnedToCore` and `netcfg_request_allowed`; `test_rest.c` checks the
   gate (method -> service class, a refusal never reaches the dispatcher). (spec 5.8, 5.13)
13. Applied (Task 19B). `espressif/mdns` 1.14.1 (managed component, not vendored, **no source change**): link-time interposition
   instead of a patch. The library has no hook for received packets and lwIP has no IPv4 input hook, so `netcfg` links with
   `-Wl,--wrap=udp_recv` (`-Wl,-u,__wrap_udp_recv` keeps the archive member) and `p4/components/netcfg/mdns_filter.c` puts a
   filter in front of the receive callback the library installs on its port-5353 PCB: a datagram whose source is an address of
   another interface of the board (and not of the interface it arrived on) is our own announcement coming back through a
   reflector and is freed before the library can read it as a name conflict. The filter does not drop link-local sources: both
   VLAN netifs have the identical EUI-64 link-local address, so it is an address of the input netif too; such a datagram is
   suppressed by the library's own-source check (the input netif's link-local, IPv4 address), which is why
   `CONFIG_MDNS_SKIP_SUPPRESSING_OWN_QUERIES` must stay unset (`tools/tests/test_mdns_config.py`). Every other `udp_recv` call passes through. The
   decision is `netcfg_mdns_source_is_foreign_own` (`mdns_rules.c`, host-tested). Reason: the V3 rig showed a rename storm
   (`esp-otbr-xxxxxx-N`, about 5 per second) behind a gateway that reflects mDNS between VLAN 10 and 20. Companion rule without
   patch: in split mode `mgmt` is never registered with mDNS (`mdns_register_netif`), so it neither announces, answers nor joins
   the multicast groups; the library drops datagrams that arrive on an unregistered netif. (spec 5.13)
14. Applied. `esp_ot_br_server/src/esp_br_web_api.c` `rest_dataset`: `PUT /node/dataset/active` is accepted while Thread runs
   (ot-br-posix and upstream answer 409 unless the role is `disabled`). With Thread running, a request that is not a whole
   network (Active Timestamp, Network Key, Network Name, Ext PAN ID, Mesh-Local Prefix, PAN ID and Channel all present in
   the request itself) still answers 409, also when it would change nothing: a partial change such as only the channel
   would move this node alone and split it from the mesh; it belongs in the pending dataset. A whole network is taken as
   the request's own TLVs: as sent for `text/plain` (TLVs the parser does not map are kept), converted from the request for
   JSON; not merged with the old dataset, which kept e.g. the old network's Wake-up Channel TLV on hardware. If they equal
   the stored TLVs the answer is 200 with no OpenThread write (no detach). Otherwise, under the OpenThread lock the handler
   already holds, Thread is stopped, the stored pending dataset is deleted, the request's TLVs are stored and Thread is
   started on them (200, one `ESP_LOGI` "active dataset replaced while attached, Thread restarted"). A failed delete or store starts
   Thread again on the old dataset (500); a failed start leaves the new dataset stored (500). With Thread disabled the
   dataset is merged and stored as before and Thread stays off (201/200); only the pending dataset is deleted first when the
   stored active dataset changes. Why the pending dataset goes: it belongs to the old network, and OpenThread
   (`PendingDatasetManager::HandleDelayTimer`) applies it when its delay expires if its network key differs from the
   active one, which took the board back to the old network on hardware; with no local pending dataset the timer finds
   nothing and clears itself. `If-None-Match: *`, `PUT /node/dataset/pending` and the guard statuses are unchanged.
   Reason: a 409 was reported from Home Assistant's Thread panel moving a running board onto the preferred network. HA's
   `otbr/set_network` disables Thread first (`set_enabled(False)`, `set_active_dataset_tlvs`, `set_enabled(True)`), and
   `PUT /node/state "disable"` is synchronous here, so the exact cause is not confirmed; the hardware pass captures the
   board log of that flow. The patch also lets any client move a running board to another whole network in one call (the
   Network page "Join with Dataset" card, item 15). Host tests: `tests/test_rest.c`
   `test_dataset_active_while_running` (new mock seams `mock_active_tlvs`, `mock_update_tlvs`, `mock_convert_tlvs`,
   `mock_set_active_error`, `mock_thread_enable_error`, `mock_clear_pending_error`, `mock_calls` in `tests/openthread_mock.inc`, `tests/mock_functions.json`,
   `tests/host_platform.h`, which also stubs `ESP_LOGI`); `tools/otbr_compat.py` expects success and an unchanged role. Target tests: `p4/pytest/test_network_page.py`.
15. Applied. `esp_ot_br_server` frontend: "Join with Dataset" card on the Network page (`frontend/network.html`, between
   Scan and Form) and new `frontend/static/dataset_join.js` (pure logic exported for node, page code guarded by
   `typeof document`, text only through `textContent`). The pasted hex must be a complete active dataset (Active
   Timestamp, Channel, Channel Mask, Ext PAN ID, Mesh-Local Prefix, Network Key, Network Name, PAN ID, PSKc, Security
   Policy, each with a valid length; ≤ 254 bytes), because the board merges what it gets into the stored dataset. A confirm names the network the board is on (if any; when it cannot be read, the confirm says so and still warns that
   it is replaced) and the new one; Join stays disabled while a join runs, and role replies that arrive after the poll
   ended are ignored;
   then the page uses only existing REST endpoints: `PUT /node/dataset/active` (text/plain; item 14 restarts Thread when
   it runs), `GET /node/state`, and `PUT /node/state "enable"` only when Thread is `disabled` (enabling a running Thread
   starts a fresh attach), then polls the role for up to 60 s like the Form flow. 403 (management address), 409
   (sniffer) and 503 (radio) are shown as readable messages. Host tests: `tools/tests/test_network_page.py`.
16. Applied. `esp_ot_br_server` frontend: topology page redesign (spec 2026-11-02,
   docs/superpowers/specs/2026-11-02-topology-page-design.md). `frontend/topology.html` rewritten and
   `frontend/static/topology.js` added (pure `TopologyLogic` exported for node, page code guarded by `typeof document`,
   text only through `textContent`, the map an inline SVG built with `createElementNS`, the device details a native
   `<dialog>`); new classes appended to `frontend/static/style.css`. The page reads `GET`/`POST /device/topology`
   (device_api) and pings through the existing `POST /ping`; `/topology` is no longer used by the page. The last result
   and nicknames live in `localStorage` (`topology:last`, `topology:names`). A `done` answer is a fresh load (green
   pill) when it is the POST's own reply with `age_s` of at most 5 s, or when polling ended a scan this page followed;
   any other `done` shows as a saved result (amber). Host tests:
   `tools/tests/test_topology_page.py`.
   Follow-up (2026-11-04, service details, docs/superpowers/specs/2026-11-04-srp-details-design.md): `TopologyLogic`
   decodes the scan's SRP records: `txtEntries` (base64 TXT RDATA to `{key, value, bytes}`, key-only entries with a
   null value, binary values as bytes, `hexText`), `describe` (ESPHome, Matter operational and commissionable,
   HomeKit, border router meshcop/TREL), `srpHostsFor` replacing `srpHostFor` (address, host name = extended address,
   meshcop/TREL `xa` = extended address; every match, this board's records first). `displayName`: nickname, this
   board, ESPHome `friendly_name`, HomeKit and meshcop instance names, the SRP host name, "Matter device 0xNNNN",
   "Unknown 0xNNNN"; the table search also matches the friendly name and every matched host name.
   This board is named by its hostname (`hostname_live`, else `hostname`) from `GET /device/names` (device_api), read
   once at page start (`boardName`); "This border router" when that read fails.
   Follow-up (2026-11-04, Topology page UI): the page shows the records. A kind line under each device name
   (`kindLine`), a hint "Service records from N SRP servers: …" and an amber note for a server whose `srp_servers` state
   is `no_answer` or `cut_short` (new `t-srp-hint`/`t-srp-warn` in `topology.html`); the dialog gains Device, Project,
   Node name, Fabrics, Retry timing, SRP server, TREL, Registered with (lease from `lease_s`, or the server's name and
   RLOC16 with `ttl_s`), services with ports, an ESPHome `_http._tcp` Open link to the first OMR address (`target=_blank`,
   `rel=noopener`; none without an OMR address), a pairing-mode note for Matter, and a "Show raw records (N)" toggle
   (`t-d-raw`, `t-d-note`; TXT values as hex when binary). All text still goes in through `textContent`. The hosts are
   decoded once per loaded result (`srpIndex`), `displayName` reads every service of every matched host (ESPHome
   `friendly_name`, then HomeKit, then meshcop instance), the generic "Matter device 0xNNNN" counts as unnamed
   (`isUnnamed`), and HomeKit `paired` is null without `sf`. A result saved by older firmware (no `srp_servers`, no
   `txt`) renders as before. New `t-` classes at the end of `static/style.css`.
   Follow-up (2026-11-04, device info): the page shows each device's `info` from the scan (spec 2026-11-04). `TopologyLogic`
   gains `stackInfo` (known Thread stack strings: Silicon Labs, Espressif, OpenThread POSIX), `infoName` (vendor + model;
   the "RD:" prefix stripped; null without a model) and `appUrl` (http/https only). `displayName` uses the info name
   after the SRP names and before "Matter device 0xNNNN"; `kindLine(sum, device)` falls back to the stack summary;
   the table search matches vendor name, model and stack string. The dialog gains Hardware, Stack, Software, EUI-64,
   App URL (a link only for http/https, else text) and "Device info" (no answer, sleepy or not; not asked under the scan
   time limit). The loading view says "Asking devices for details: D of T" (D: answered or given up) and, once
   `progress.info.awake_left` is 0 with sleepy devices waiting, "Waiting for N sleepy devices (…, up to M min)";
   without `awake_left` and `done` (older firmware) it goes by `answered`. Results and progress without `info` render
   as before. No new `t-` classes.
17. Applied. `esp_ot_br_server`: every Network Diagnostic Get goes through the new `netdiag_mux` component (spec
   2026-11-02 section 3.5). OpenThread's netdiag client keeps one callback slot and every `otThreadSendDiagnosticGet`
   replaces it, so `/topology`, the REST diagnostics and discovery, and the `mesh_scan` child step took each other's
   answers. `src/esp_br_web_api.c` includes `netdiag_mux.h`; its four `otThreadSendDiagnosticGet` calls
   (`build_thread_network_topology`: own RLOC and all-routers multicast; `rest_poll`: device discovery to the
   realm-local all-nodes multicast and `getNetworkDiagnosticTask`) call `netdiag_mux_send_get(..., filter_peer = true)`,
   so a unicast Get only gets answers from its destination (multicast and anycast-locator Gets are unfiltered in the mux).
   The discovery callback `rest_discover_devices_callback` already drops answers without the Ext Address TLV (TLV 0), so
   the scan's TLV-8-only answers that reach it create no device; no further guard. The `/topology` collector's answer
   callback (`get_diagnosticTlv_information`, now returning `bool`) skips an answer without the Address16 TLV (TLV 1)
   instead of storing it under "0xffee"; `diagnosticTlv_result_handler` counts only answers it stored, so a foreign
   answer cannot end the collection early. Known limits of the mux (also in `netdiag_mux.h`): a listener keeps one
   filtered unicast destination at a time, and a REST diagnostic to a peer (or an anycast locator) that the scan or
   `/topology` queries at the same moment can receive their answer. `CMakeLists.txt`: `netdiag_mux` in
   `PRIV_REQUIRES`. Host tests: `tests/host_platform.c`/`.h` stub `netdiag_mux_send_get` (forwards to the mocked
   `otThreadSendDiagnosticGet`, so `mock_deliver_diagnostic` works as before, and counts the sends and the filtered ones
   in `mock_netdiag_sends`/`mock_netdiag_filtered_sends`, reset in `tests/openthread_mock.inc`); `tests/run_host_tests.py`
   also compiles the `/topology` collector and the diagnostic list helpers of `esp_br_web_base.c` (with a scripted
   `otThreadGetNextDiagnosticTlv` and a tick clock that `vTaskDelay` advances) for `test_topology_collector`, and scans `tests/host_platform.c` for the OpenThread functions to mock; `tests/test_rest.c` `test_actions` asserts
   the filtered sends and that a discovery answer without TLV 0 adds no device. The mux itself is tested in
   `p4/components/netdiag_mux/test_host/`.
18. Applied (2026-11). `esp_ot_br_server` frontend, upstream pages: new
   `frontend/static/mgmt.js` (pure logic exported for node: mode banner, random network parameters, Form Network and
   PSKd validation, the replace-network confirm; page code guarded by `typeof document`), loaded after `api.js` by
   `network.html`, `commission.html`, `addresses.html`, `tools.html`, whose `checkThreadState`
   /`disableManagementPage` guard (removed from `api.js`) is replaced by `MgmtPage.guard`, which reads `/device/status`
   first. `api.js`: `apiGet`/`apiPost` resolve a 4xx/5xx to `{error: <status>, message}` (never cached); the nav brand is
   "Thread Border Router"; dropdowns also open on a tap (`.nav-dropdown.open` in `style.css`, which also gets
   `[aria-invalid]` and `#t-ha-url` rules appended). `network.html`: random defaults instead of the example key,
   IDs, passphrase and prefix; labelled and validated fields; confirms for Form and scan Join (via
   `DatasetJoin.readCurrentNetwork`); a shorter scan table; "This board's dataset" (`GET /node/dataset/active`,
   text/plain). `commission.html`: no default PSKd. `addresses.html`: confirms for remove/delete, no default prefix.
   `about.html`: project texts and links, `/device/status` fields. Every upstream page gets the project footer instead
   of the Espressif copyright line. Host tests: `tools/tests/test_mgmt_pages.py`.
19. Applied (mesh routing, Task 8). `esp_ot_br_server/src/esp_br_web.c`: `EXTRA_HANDLER_SLOTS` 20 -> 24, the slots
   patch 10 reserves for the `device_api` routes and the catch-all `/*` GET handler. With `GET`/`PUT /device/routing`
   `device_api` registers 20 routes (18 before: the names and topology routes), plus the catch-all: 21, which 20 slots
   no longer cover (only the `+30` slack of `max_uri_handlers` hides the shortfall; running out silently drops the later
   registrations, the catch-all first). 24 leaves 3 slots of headroom.
   (docs/superpowers/specs/2026-11-03-mesh-routing-design.md sections 5, 6)
20. Applied (mesh routing, Task 10). `esp_ot_br_server` frontend: Routing page. New files `frontend/routing.html` and
   `frontend/static/routing.js` (pure logic exported for node, page code guarded by `typeof document`, text only through
   `textContent`), built from the approved mockup `docs/superpowers/specs/2026-11-03-routing-page-mockup.html`. It reads
   and writes `GET`/`PUT /device/routing` (no reboot, no trial), reads the PD state from `GET /device/status` while the
   saved method is `pd`, refreshes every 5 s keeping unsaved edits, and shows a 400's field next to its input. Edits:
   `frontend/static/api.js`, "Routing" after "Ethernet" in the Device dropdown; `frontend/static/style.css`, new rules
   appended (`.mode-cards.four`, `.now.off`, `.peer-row`; no existing rule changed). Host tests:
   `tools/tests/test_routing_page.py`. (docs/superpowers/specs/2026-11-03-mesh-routing-design.md section 8)
21. Applied (mesh routing, review fix). `esp_ot_br_server`: `include/esp_br_web.h` defines
   `ESP_BR_WEB_MAX_OPEN_SOCKETS` (7, the upstream value), and `src/esp_br_web.c` sets `config.max_open_sockets` from it.
   Reason: the routing task's compile-time lwIP socket budget (`p4/components/routing/task.c`, `SOCKETS_NEEDED` against
   `CONFIG_LWIP_MAX_SOCKETS`) counts the web server's sockets; with one definition, raising it trips that check
   instead of silently overrunning the budget. (docs/superpowers/specs/2026-11-03-mesh-routing-design.md section 5)

22. Applied. OpenThread sessions around every OpenThread user outside the OpenThread task, so that a radio push or ROM
   recovery (which deletes the OpenThread lock and instance in `esp_openthread_deinit`) cannot run under them.
   `esp_ot_br_server/src/esp_br_web.c`: every Web GUI handler (`s_resource_handlers`, `s_web_gui_handlers`) is registered
   behind `ot_session_handler`, which opens a `br_core_ot_session_begin` session for the whole handler; `guard_now` sees
   OpenThread as ready only inside one (`ot_ready_for_request`). The Thread REST dispatcher opens its own session around
   the guard and `esp_br_thread_rest_request`. `esp_br_web_api.c`: `rest_process_once` opens one per poll. The waits for
   OpenThread callbacks (scan, join, ping, pending dataset: `ot_wait_callback`; the topology collection loop) end early
   once the REST flag is off, so a push waits for them about 0.25 s instead of up to 30 s, and a scan waits at most
   30 s (was `portMAX_DELAY`: a radio fault that stopped the main loop mid-scan hung the only HTTP task). The scan list
   is not replaced, and the network-key join does not read it, while `otThreadIsDiscoverInProgress` says a scan whose
   wait timed out may still write it. Before: the handlers checked the ready flag, then waited for the lock, and the
   pending-dataset write (30 s) and ping (14 s) released the lock and took it again without a re-check, so a push started
   meanwhile deleted the lock under them (use-after-free). Now `br_core` puts the mode to STARTING and clears its ready
   state under the OpenThread lock, waits up to 45 s for open sessions to close (the main loop keeps running, an RCP
   failure meanwhile only resets the C6) and refuses the push as busy (409) if they do not, putting the mode back. A pending-dataset
   PUT whose wait ends that way answers 503 and clears its busy flag (its callback dies with the instance). Known
   limit: a Web GUI session also covers the request's socket I/O, so a very slow client can hold it. Host tests:
   `tests/test_rest.c` `test_no_openthread_without_a_session`, `test_callback_wait_ends_when_not_ready` (mocks
   `br_core_ot_session_begin/end` in `tests/host_platform.c`). (Before the numbering was fixed this entry was 18 and
   comments in `src/` said "patch 16".)

23. Applied. Architecture review follow-ups (2026-11). `src/esp_br_web_api.c`: the `/topology` and diagnostics
   collection (`handle_ot_resource_network_diagnostics_request`) releases its `netdiag_mux` listener when it ends
   (`netdiag_mux_release`), so late answers to its multicast Get no longer reach `diagnosticTlv_result_handler`; the
   callback returns at once while no collection runs instead of waiting up to 1 s for `s_diagnostic_semaphore` on the
   OpenThread task, and the result set is taken out under the semaphore and converted after it. `src/esp_br_web.c`: the
   Thread REST reset (`DELETE /node`) reboots through `reboot_gate_restart`, so a running update or radio push is not
   cut off (`CMakeLists.txt`: `reboot_gate` in `PRIV_REQUIRES`). Host tests: `tests/test_rest.c`
   `test_topology_collector` (release count, late answer dropped), stubs in `tests/host_platform.c`.
24. Applied (Thread console, Task 3). `esp_ot_br_server/src/esp_br_web.c`: `EXTRA_HANDLER_SLOTS` 24 -> 27. `device_api`
   adds `GET`/`PUT /device/console` and the WebSocket `GET /device/console/ws`: 25 routes plus the catch-all, one slot
   of headroom. (docs/superpowers/specs/2026-11-04-thread-console-design.md)
25. Applied (self-update, Task 1). `esp_ot_br_server` frontend, `frontend/static/firmware.js` and `frontend/firmware.html`:
   the device downloads over https now (certificate bundle, `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN=y`).
   `checkUpdateUrl` accepts `https://` as well as `http://` (message: "The URL must start with http:// or https://"),
   both URL labels read "Download address (http or https)", and the placeholders are `https://host/...`. Tests:
   `tools/tests/test_firmware_page.py` (`test_https_update_url_is_accepted`).
26. Applied (self-update, Task 5). `esp_ot_br_server/src/esp_br_web.c`: `EXTRA_HANDLER_SLOTS` 27 -> 30. `device_api`
   adds `GET`/`POST /device/update`: 27 routes plus the catch-all (28), which 27 slots no longer cover. 30 leaves 2
   slots of headroom. (docs/superpowers/plans/2026-11-05-self-update.md, Task 5)
27. Applied (self-update, Task 6). `esp_ot_br_server` frontend, `frontend/firmware.html`, `frontend/static/firmware.js` and
   `frontend/static/style.css`: the Updates card (`#updCard`, above `#firmwareSection`) shows the latest GitHub release from
   `GET /device/update`, checks (`?check=true`, polled with the page's refresh timer) and installs (`POST /device/update`,
   `?force=1` for a build newer than the release). `FirmwareLogic` gains `updateNeedsCheck`, `updateChip`, `versionRow`,
   `checkedText`, `updateAction`, `updateHint`, `updateQuestion`, `checkFailedText`, `updateReplyText`, `repoLabel` (and
   helpers); the page keeps `update:after` (`{rcpFailed}`) next to `ota:before` across the reload and reports through
   `updMsg`. The manual Install buttons (`otaInstall`, `rcpInstall`) are `btn-outline`. `style.css`: `.facts .was`.
   Tests: `tools/tests/test_firmware_page.py` (`TestUpdateLogic`, `TestUpdatesCard`).
28. Applied (USB serial proxy, Task 4). `esp_ot_br_server/src/esp_br_web.c`: `EXTRA_HANDLER_SLOTS` 27 -> 29 (before the self-update merge).
   `device_api` adds `GET`/`PUT /device/usb_serial`: 27 routes plus the catch-all, one slot of headroom (those two routes
   are gone again in patch 29).
   (docs/superpowers/specs/2026-11-05-usb-serial-proxy-design.md)
29. Applied (radio channel mux, Task 8). `esp_ot_br_server`: the board mode `NO_RADIO` (no radio channel in otbr is
   present) in the REST guard. `src/esp_br_guard.c`: `READ` and `THREAD_OP` answer 503 in `NO_RADIO` whatever the
   ready flag says (it may lag while OpenThread is torn down after an unplug); `esp_br_guard_detail(status, mode)`
   takes the mode and says `no radio in OTBR mode` for 503 in `NO_RADIO` (`radio not available` otherwise, as before).
   Call sites: `src/esp_br_web.c` (the Thread REST dispatcher reads the mode once for the check and the detail;
   `guard_refuse_gui` and `guard_refuse_resource` use the guard's detail, the resource format no longer hard-codes
   it) and `src/esp_br_web_api.c` (`esp_br_thread_rest_request`). `EXTRA_HANDLER_SLOTS` 29 -> 30: `device_api` drops
   `GET`/`PUT /device/usb_serial` (patch 28) and adds `GET /device/channels`, `PUT` and `DELETE /device/channels/*`:
   28 routes (the console WebSocket included) plus the catch-all, one slot of headroom. The console WebSocket
   (`device_api`, not vendored) answers its handshake with 503 `no radio in OTBR mode` in `NO_RADIO` as well. Host
   tests: `test_host/` (NO_RADIO cases) and `tests/test_rest.c` `test_guard_no_radio`.
   Task 9 (web UI): new `frontend/radios.html` and `frontend/static/radios.js` (the Radios page: channel table,
   configure dialog, mode cards, move of Thread); `frontend/static/api.js` builds the regrouped navigation (Dashboard,
   Thread, an empty hidden `<li id="nav-slot-zigbee">`, Device with Radios first, About) and hides the Thread menu
   while `/device/status` has `otbr_channel` null; `frontend/static/device_common.js` gains `threadState`,
   `threadNotice` and the kit's `renderThreadNotice`/`watchThread`; `status.js` (chip "Thread runs on", notice),
   `mgmt.js` (mode banner), `routing.js`, `topology.js`, `capture.js` and their pages (`#threadNotice`) show the
   "no radio in OTBR mode" notice; `index.html` text and chip; `static/style.css` appends the Radios classes.
   Final review fixes: `radios.js` shows a proxy in backoff as a warning with its `last_error`, names `port_taken`,
   sets a radio that runs `rcp-proxy` off before it takes Thread, and follows a 202 for a start ("Starting Thread on")
   and a stop of Thread ("Stopping Thread on", then stopped or refused with the reason); `status.js` has a
   `NO_RADIO` hero linking the Radios page, and the `RADIO_FAULT` hero links Radios instead of Firmware while Thread's
   radio is a dongle.
   Patch step (2026-11-08): `device_common.js` gains `threadChip`, which `status.js` and `radios.js` use for the chip:
   "Thread runs on" only in `BR_RUNNING` or `SNIFFER`, otherwise a warning "Thread on <label>: <state or reason>" or
   "No radio in Thread mode"; `radios.js` `saveFailureText` says the radio was switched off when Thread's start after
   the PUT off is refused; `console.js` counts the wait for the radio as a retry (the toggle reads Disconnect).
   (docs/superpowers/specs/2026-11-06-radio-channel-mux-design.md, sections Board mode and API)
   Merged with self-update (patch 26, which took `EXTRA_HANDLER_SLOTS` to 30 for `/device/update`): `device_api` now has
   31 routes (the console WebSocket included) plus the catch-all, so `EXTRA_HANDLER_SLOTS` is 34, two slots of headroom.
34. Applied (Zigbee mode, Task 5). `esp_ot_br_server/src/esp_br_web.c`: `EXTRA_HANDLER_SLOTS` 34 -> 36 (on top of patch
   29 and self-update: `GET /device/zigbee` and `GET /zigbee`, two slots of headroom kept). The extra-handlers callback (`app_main.c`
   `register_extra_handlers`) also registers the WebSocket `GET /zigbee` (`zigbee_register_ws`) after the device API,
   before the catch-all. Frontend: `index.html` (the Dashboard) is restructured into three sections, Device and
   network, Thread and Zigbee, each stack's section showing a one-line placeholder while it does not run; the board
   line takes the board-wide faults and Reboot, a two-row Radios card replaces the "Thread runs on" chip, and the
   shared `#threadNotice` banner and the Mode row are gone. `static/status.js` gains `boardHealthFor`,
   `threadHeroFor` (was `heroFor`), `isThreadActive`, `radiosView` and polls `/device/channels`;
   `static/zigbee.js` is the Dashboard's Zigbee section (`ZigbeeSection`, loaded before `status.js`); there is no
   Zigbee page (no `zigbee.html`) and `static/api.js` leaves patch 29's `<li id="nav-slot-zigbee">` empty;
   `static/device_common.js` `poller` accepts a function as its period; `static/style.css` adds `.zg-*` and
   `.dash-*` classes and styles `.tile-head h3`. Page tests: `tools/tests/test_status_page.py`,
   `tools/tests/test_zigbee_page.py`. (docs/superpowers/specs/2026-11-06-zigbee-mode-design.md,
   docs/superpowers/specs/2026-11-07-unified-dashboard-design.md)
35. Applied (time sync, Task 4). `esp_ot_br_server/src/esp_br_web.c`: `EXTRA_HANDLER_SLOTS` 36 -> 38 (`GET` and `PUT
   /device/time`, two slots of headroom kept). (docs/superpowers/specs/2026-11-10-time-sync-design.md)
36. Applied (time sync, Task 5). Frontend: `ethernet.html` gains the Time section after Hostname and a hidden
   "Time server" row (`if-<iface>-ntprow`) on each interface card; new `static/time.js` (`TimeLogic`, `TimePage`) fills
   both from `GET /device/time` and saves with `PUT /device/time`. Page tests: `tools/tests/test_time_page.py`.
   (docs/superpowers/specs/2026-11-10-time-sync-design.md)
37. Applied (hybrid mode, Tasks 3 and 4). Frontend: `radios.html` gains `#zigbeeNote` under the otbr panel;
   `static/radios.js` gains `RadiosLogic.rebootNote` (a save that enters or leaves Zigbee reboots the board and Thread
   with it), shown in `#zigbeeNote`, or in `#otbrNote` when otbr is picked on the radio that runs Zigbee; the
   `hybrid_not_supported` text is gone. `index.html` gains `#zg-shared` (with a link to the Radios page) above
   `#zg-note`; `static/zigbee.js` gains `ZigbeeLogic.sharedChannelText` and the section's `setThreadChannel`, which
   `static/status.js` calls with the Thread cards' channel (null while Thread is off), so the Zigbee section warns when
   Thread and Zigbee share an 802.15.4 channel. Page tests: `tools/tests/test_radios_page.py`,
   `tools/tests/test_zigbee_page.py`. (docs/superpowers/specs/2026-11-10-hybrid-mode-design.md)
38. Applied (logging, Task 7). `esp_ot_br_server/src/esp_br_web.c`: `EXTRA_HANDLER_SLOTS` 38 -> 41 (on top of patch
   35). `device_api` adds `GET`/`PUT /device/logging` and the live log WebSocket `GET /device/logging/ws`: 37 routes
   (both WebSockets included) plus `/zigbee` and the catch-all, two slots of headroom kept. Live log clients use the web
   server's sockets (`ESP_BR_WEB_MAX_OPEN_SOCKETS`, 7, unchanged), so the lwIP socket sum does not change.
   (docs/superpowers/specs/2026-11-10-logging-design.md)
39. Applied (logging, Task 9). Frontend, the Device -> Logs page: new `frontend/logs.html` and `static/logs.js`
   (`LogsLogic` pure, `LogsPage`; settings cards for channels and components, a live card that opens its WebSocket
   only on Connect); `static/api.js` adds `{href : '/logs.html', label : 'Logs'}` to the Device menu after Firmware;
   `static/style.css` adds the `.lg-*` rules (foldable cards, level colours, fullscreen). Page tests:
   `tools/tests/test_logs_page.py`, `tools/tests/test_device_pages.py`. (docs/superpowers/specs/2026-11-10-logging-design.md,
   Web UI)
40. Applied (USB/IP, Task 10). `esp_ot_br_server/src/esp_br_web.c`: `EXTRA_HANDLER_SLOTS` 41 -> 43 (on top of patch
   38). `device_api` adds `GET`/`PUT /device/usbip`: 39 routes (both WebSockets included) plus `/zigbee` and the
   catch-all, two slots of headroom kept. (docs/superpowers/specs/2026-11-10-usbip-design.md)
41. Applied (USB/IP, Task 11). `esp_ot_br_server` frontend: new page `frontend/usbip.html` with `static/usbip.js` (on/off
   switch that stages the change and a Save and Reboot button, device and client cards, counters, attach commands); `static/api.js` adds
   `USB/IP` to the Device menu after Radios; `static/style.css` appends the `u-*` classes; `frontend/radios.html` gets
   the `mUsbipLink` line and `static/radios.js` shows the USB channels as exported while `/device/channels` says
   `"usbip": true` (reason with a link to the USB/IP page, mode cards disabled). `pack_web.py` packs the new files
   without a change.
   (docs/superpowers/specs/2026-11-10-usbip-design.md)

### REST guard classification (patches 2 and 9)

Classes: `NONE` no OpenThread access, always served. `READ` reads OpenThread state: 200 while OpenThread runs (patch 9
ready flag), 503 otherwise (RADIO_FAULT/STARTING before OpenThread is up, SNIFFER: `br_core` switches the flag off while
Thread is stopped, so Thread state reads answer 503 there). `THREAD_OP` changes Thread/dataset state or needs the radio:
only in BR_RUNNING with OpenThread ready; SNIFFER 409, RADIO_FAULT/STARTING 503. NO_RADIO (patch 29): every `READ`
and `THREAD_OP` endpoint below answers 503 `no radio in OTBR mode`, as in the RADIO_FAULT / STARTING column. THREAD_OP is checked twice: lock-free
first (no lock exists before OpenThread is up), then while holding the OpenThread lock where the OpenThread call
happens.

| Endpoint | Class | SNIFFER | RADIO_FAULT / STARTING | Atomic with the call |
| --- | --- | --- | --- | --- |
| `GET /.well-known/thread/br-rest` (discovery), static files, `/.well-known/thread/esp-br-rest` | NONE | 200 | 200 | n/a |
| Thread REST `GET`/`HEAD` (`/node`, `/node/state`, `/node/dataset/*`, `/api/*` ...) | READ | 503 | 503 | n/a |
| Thread REST `PUT`/`POST`/`DELETE` (`/node/state`, `/node/dataset/*`, commissioner, ePSKc, `/api/actions`, `DELETE /node` ...) | THREAD_OP | 409 | 503 | yes, under the lock in `esp_br_thread_rest_request` |
| `GET /get_properties`, `/node_information`, `/ipaddr` | READ | 503 | 503 | n/a |
| `POST /form_network`, `/add_prefix`, `/delete_prefix`, `/commission`, `/add_ipaddr`, `/delete_ipaddr` | THREAD_OP | 409 | 503 | no: `GUARD_CHECKED_CALL` checks under the lock and releases it before the `handle_*` call, which takes the lock itself (holding it across the call deadlocked Form network); a capture started in between is interrupted |
| `POST /join_network` | THREAD_OP | 409 | 503 | yes for every state change: `handle_openthread_join_network_request` re-checks each time it takes the lock (a race ends in the generic failure reply, nothing changed) |
| `GET /available_network` (scan), `/topology`, `/diagnostics` (mesh queries), `POST /ping` | THREAD_OP | 409 | 503 | check under the lock, then released: they wait with the lock released, so a mode change during the wait cannot be excluded; they change no Thread state |
| Background poll task `rest_process_task` (starts queued `/api/actions`, cancels expired ones) | THREAD_OP | not run | not run | yes, `rest_poll_guarded` checks under the lock each cycle; outside BR_RUNNING the queue is left untouched and polling resumes on return (actions past their deadline are then cancelled) |

The status of 409/503 comes before request validation (a malformed body in SNIFFER is 409, not 400). `read-only status
endpoint, any mode: 200` (spec) is met by the NONE class and by the `br_core` status endpoint of Task 15; Thread state
reads need a running OpenThread and answer 503 while it is off (a deliberate choice: raw link on in SNIFFER, OpenThread
API behaviour there is not verified). Known gaps: the REST dataset-pending write releases the lock for up to 30 s while
waiting for the leader (upstream); a sniffer started in that window completes the request normally. The default
`/*` static file handler and the OPTIONS/404/405 replies of the dispatcher do not touch OpenThread and are not guarded.
Host tests: `test_host/` (decision table) and `tests/test_rest.c` (dispatcher, run with `tools/run_vendored_rest_tests.sh`);
target tests: `p4/pytest/test_rest_guard.py`, `test_br_boot.py::test_radio_fault_web_gui_api_answers_503`.

## Generated ESP-IDF copies

`tools/ot_radio_port.cmake` (radio channel mux, Task 7) generates `p4/build/ot_port_components/openthread` at every
configure, and that component overrides ESP-IDF's `openthread` (`EXTRA_COMPONENT_DIRS`). It holds symlinks to IDF's
`components/openthread` (v6.1), a real `src/port` and `include` with symlinks to IDF's files plus the project's
`p4/ot_port/mux_spinel_interface.cpp`, `mux_spinel_interface.hpp`, `mux_gone_answer.{c,h}` (the answers after GONE,
host-tested in `p4/ot_port/test_host`), `esp_openthread_mux.h` and radio_mux's header-only
`radio_mux_spinel_uint.h` (the spinel packed-uint codec `mux_gone_answer.c` includes), and two patched copies.
Each replaced text is looked up with `string(FIND)` first; a text that is not there any more (an IDF update) stops the
configure with a `FATAL_ERROR` naming the IDF file. Kconfig stays `CONFIG_OPENTHREAD_RADIO_SPINEL_SPI`; the IDF tree is
not touched. Upstream path: a runtime "custom spinel interface" radio mode with per-interface caps (spec section 5).

- `src/port/esp_openthread_radio_spinel.cpp`:
  1. After `#include "esp_spi_spinel_interface.hpp"`: `#include "mux_spinel_interface.hpp"`.
  2. `using esp::openthread::SpiSpinelInterface;` and `static SpinelInterfaceAdapter<SpiSpinelInterface>
     s_spinel_interface;` become the same with `MuxSpinelInterface`. The concrete-only calls of the port (`Enable` with
     the SPI config, `AfterRadioInit`, `Disable`, `RegisterRcpFailureHandler`) exist on `MuxSpinelInterface` with the
     same signatures (the SPI config is ignored: the channel comes from `esp_openthread_mux_set`), so no call site
     changes.
  3. The `s_radio_caps` initializer (energy scan, rx-on-when-idle under `CONFIG_OPENTHREAD_RX_ON_WHEN_IDLE`, tx
     security, rx timing, tx timing, ack timeout, sleep-to-tx, CSMA backoff, tx retries) becomes the caps required from
     the RCP by the Global Constraints: ack timeout, tx retries, CSMA backoff, tx security, tx timing (the ot-br-posix
     set for Thread 1.2+). The C6 RCP passes as before; a CC2652P RCP (Koenkk 2025.3.1, caps 0x7f: no rx timing, no
     rx-on-when-idle) now passes. OpenThread still uses every cap the RCP reports (`otPlatRadioGetCaps`).
  4. `otPlatRadioGetCaps`: `s_radio_caps = s_radio.GetRadioCaps(); return s_radio_caps;` becomes `return
     s_radio.GetRadioCaps();`. Upstream assigns the running RCP's caps to the static, so the next `esp_openthread_init`
     (OpenThread started again on another channel after a move) would require the previous RCP's caps: a move from the
     C6 to a CC2652P would fail `CheckRadioCapabilities`.
- `CMakeLists.txt`: the `git rev-parse` for `IDF_VERSION_FOR_OPENTHREAD_PACKAGE` runs in IDF's component directory
  instead of `${CMAKE_CURRENT_SOURCE_DIR}` (the generated directory lies in this repository, so the package version
  would show this repository's commit).

## usb_host_cdc_acm

Vendored from:

- Component: `espressif/usb_host_cdc_acm` 2.4.1 (ESP Component Registry; esp-usb commit
  `eb0f02163be96dfc753cf43d1207329e44f1ca60`, `host/class/cdc/usb_host_cdc_acm`), component hash
  `5bc1372b46de79ccae1dd17de48e30dfa1e8ab75269c2ebf54da01370f9e42fb`.
- Copied unchanged from the managed component the build resolved (every file matched the archive's `CHECKSUMS.json`;
  that file and `.component_hash` are left out) to `p4/components/usb_host_cdc_acm`.
- `override_path` in `p4/components/radio_mux/idf_component.yml` makes every user (radio_mux, `usb_host_ch34x_vcp`
  2.2.1, `usb_host_cp210x_vcp` 2.2.0, `esp-serial-flasher`) build this copy; `p4/dependencies.lock` lists it as a
  `local` source and no `espressif__usb_host_cdc_acm` is downloaded.

Same rules as above: upstream formatting, excluded from pre-commit, every change listed here. Patch numbers continue
the sequence above.

30. Applied (CH34x status packets, 2026-11-07). `usb_host_cdc_acm`: packets on the notification endpoint that are not
   CDC notifications. `cdc_acm_host.c` `notif_xfer_cb`: a packet whose first byte (bmRequestType) is not
   `CDC_NOTIF_BM_REQUEST_TYPE` (0xA1, new in `include/usb/usb_types_cdc.h`), or an empty one, is passed to the user's
   event callback as the new event `CDC_ACM_HOST_VENDOR_NOTIFICATION` (`include/usb/cdc_host_types.h`: appended to
   `cdc_acm_host_dev_event_t` after the suspend/resume values, so no upstream value moves; union member
   `vendor_notification {const uint8_t *data; size_t len;}`, valid only during the callback; `<stddef.h>` included for
   `size_t`), with no warning and no hex dump; the INTR IN poll is resubmitted as before. Reason: the CH34x driver
   (`usb_host_ch34x_vcp` 2.2.1) opens the CH340 through `cdc_acm_host_open`, so its 4-byte vendor status packets
   (`20 ff 0f aa` ...) were read as CDC notifications with code 0xFF and logged with a hex dump, 3-10 per second under
   traffic, on the CDC driver task that also runs the bulk IN callback: on the 115200 baud console that delayed the bulk
   IN resubmit while the dongle kept sending at 460800. radio_mux's `usb_transport.c` counts the CH34x's UART errors
   from these packets. Host test: none here (upstream's `host_test` needs the esp-usb tree's USB host mocks); the
   CH34x status decision is host-tested in radio_mux (`radio_mux_ch34x_uart_error`).
   (docs/superpowers/specs/2026-11-06-radio-channel-mux-design.md; docs/radios.md, counter `uart_errors`)

31. Applied (CH34x late answers, 2026-11-07). `usb_host_cdc_acm`: the bulk IN transfer keeps the length asked for at
   open. `include/esp_private/cdc_host_common.h`: new field `data.in_xfer_len`; `cdc_acm_host.c`
   `cdc_acm_transfers_allocate` sets it to the asked length rounded down to whole max packets (below one packet:
   upstream's length from the allocated buffer) and uses it for the first submit; `cdc_acm_reset_in_transfer` uses it
   instead of the allocated `data_buffer_size`. Reason: `urb_alloc` rounds transfer buffers up to the cache line (at
   least 64 bytes on the P4), so a one-packet (32-byte) CH340 IN transfer became two packets after the first
   completion. A bulk IN transfer completes only on a short packet or when full, and USB serial bridges send no ZLP:
   a received burst ending on a packet boundary stayed in the transfer until the device sent again, which showed as
   ~2 s late answers on the OllyZig (CH340) under Zigbee and rcp-proxy load. radio_mux's `usb_transport.c` asks for
   one max packet (`in_buffer_size` 0). Linux's usb-serial also reads one max packet per URB, but keeps two read URBs
   in flight, so the bridge always has a transfer to complete into; this driver has one IN transfer in flight, so
   every packet now waits for a software turnaround (completion, `data_cb`, resubmit) that the bridge's receive FIFO
   must cover. Not measured under a sustained stream yet (docs/radios.md, "Verifying on a board"); if `uart_errors`
   or `drops` move there, the fallback is a longer transfer only for devices known to send ZLPs, or a second IN
   transfer here. Below one packet (`0 < in_buf_len < MPS` with a buffer still under one MPS after the cache-line
   rounding, for example an HS device with MPS 512 asked for 100 bytes) `in_xfer_len` comes out 0 and a zero-length
   IN is submitted, as upstream also did; this project never asks for that (it passes 0). Upstream's `host_test` that
   opens with `in_buffer_size = 100` (`test_device_interaction.cpp`) would now expect a first submit of 64 bytes, not
   100: adjust it if that test is ever revived here. Host test: none (no USB host mocks, see patch 30).
   (docs/superpowers/specs/2026-11-06-radio-channel-mux-design.md)

32. Applied (CH34x UART overrun, 2026-11-07). `usb_host_cdc_acm`: `CDC_ACM_IN_XFERS` (8) bulk IN transfers in flight
   per device. `include/esp_private/cdc_host_common.h`: new `CDC_ACM_IN_XFERS`; `data.in_xfer`,
   `data.in_data_buffer_base` and `data.in_polling` become arrays of that size (`in_xfer_len` and `in_mps` stay shared:
   the transfers are allocated alike). `cdc_acm_host.c`: `cdc_acm_transfers_allocate` allocates each transfer (patch
   28's length for all; a failed allocation frees the ones before it); `cdc_acm_transfers_free` frees each;
   `cdc_acm_reset_in_transfer` takes the transfer's index; the new `cdc_acm_submit_in_polls` submits every transfer not
   already polling, in index order, from `cdc_acm_start` and `cdc_acm_resume`; `cdc_acm_suspend_polling` and
   `cdc_acm_host_close` clear every `in_polling` flag, and close halts, flushes and clears the shared endpoint once
   (that retires every transfer on it); `in_xfer_cb` finds the transfer's index, calls `data_cb`, resets that transfer
   and resubmits it, so it queues behind the other one. Order: the USB host queues a pipe's URBs FIFO
   (`hcd_dwc.c` `hcd_urb_enqueue` appends to `pending_urb_tailq`; the HCD's two DMA buffers execute and retire in
   order, the next buffer starting in the ISR as the previous completes, into `done_urb_tailq`), and
   `usb_host.c` `_handle_pending_ep` dequeues them in that order and runs each callback to its end on the CDC driver
   task (`usb_host_client_handle_events`), so `data_cb` sees the data in arrival order, one call at a time; the
   resubmit inside the callback goes behind the transfer still in flight. "Data not processed" (`data_cb` returning
   false): upstream appends the next data into the same buffer (not on the P4: `SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE`
   only warns there); with more than one transfer the next data already lands in another transfer, so it is treated
   as processed, with one `ESP_LOGW` per boot; with `CDC_ACM_IN_XFERS` 1 the upstream code is compiled unchanged.
   radio_mux's `data_cb` always returns true; the ch34x and cp210x VCP drivers pass the user's callback through.
   Memory: seven more transfers per open device (one max packet each, a 64-byte DMA buffer after the cache-line
   rounding: 448 bytes).
   Reason: on the board (CH340 OllyZig at 460800, sustained Zigbee load, 6 minutes) the single 32-byte IN transfer of
   patch 31 left the CH340 nothing to complete into between a completion and the resubmit (46 bytes/ms at 460800):
   the channel counted `uart_errors` 9 (the CH340's own UART receive-error/overrun bit, patch 30) and `rx_errors` 5
   (bad HDLC frames). Linux's usb-serial keeps two one-MPS read URBs in flight (generic driver, `bulk_in_urbs[2]`);
   two were not enough here. Measured on the board (rcp-proxy over TCP, frame mode, 2 minutes of pipelined spinel
   requests, 8 per burst, ~15-29 KB/s from the dongle): with 2 transfers 23453 of 23472 replies, 19 with a bad FCS,
   `uart_errors` 38; with 8 transfers 44808 of 44808, none bad, `uart_errors` 0, at twice the throughput. Linux
   completes URBs in interrupt context; here the callbacks run on the CDC driver task (priority 7, below lwIP's 18),
   so the queue must cover its scheduling delay: 8 one-packet transfers are ~5.5 ms at 460800 on a CH340. (Bursts of
   60 pipelined requests still lose replies, but on the dongle's side: the RCP drops requests it cannot take, with
   no flow control, and the board counts no error.) Errors: a transfer that fails (stall, error) is not resubmitted, as upstream; the endpoint flush
   then returns the other one as well (cancelled, no resubmit), and the user may see one `CDC_ACM_HOST_ERROR` per
   transfer. Upstream's `host_test` fixtures (`common_test_fixtures.cpp`) still use the single `data.in_xfer` and expect
   one IN submit: adjust them if that test is ever revived here. Host test: none (no USB host mocks, see patch 30).

33. Applied (close race, 2026-11-07). `usb_host_cdc_acm`: `cdc_acm_host_close` no longer aborts when an IN callback
   resubmits during the close. `include/esp_private/cdc_host_common.h`: new field `closing` (volatile bool) in
   `cdc_dev_t`. `cdc_acm_host.c`: close sets `closing` with the callbacks cleared; `in_xfer_cb` and `notif_xfer_cb`
   (both resubmit points) resubmit only while it is false; close retries the endpoint halt, flush and clear plus the
   interface release up to `CDC_ACM_CLOSE_ATTEMPTS` (10) rounds one tick apart (logged when it took more than one), the
   endpoint resets use `ESP_ERROR_CHECK_WITHOUT_ABORT`, and a release that still fails is logged and returned instead of
   aborting (the device stays listed; radio_mux's reopen then fails and the channel is treated as gone). Reason: close
   runs on radio_mux's owner task (MGMT_CORE), the callbacks on the CDC driver task (NET_CORE since 0071091); a callback
   that had passed the polling-flag check before close cleared it resubmitted its transfer, and
   `usb_host_interface_release` failed with ESP_ERR_INVALID_STATE under upstream's `ESP_ERROR_CHECK`: an abort (core
   dump: `cdc_acm_host_close` line 790/794, called from radio_mux's USB reset path) about once per 100 reopens under
   traffic on the board. With the patch: 300 reopens under traffic, 4 late transfers caught by a second round, no
   abort. The race existed upstream with both tasks on one core too, rarer (it needs a preemption inside the callback).
   Host test: none (no USB host mocks, see patch 30).
   Extended 2026-11-08 (code review): the notification interface, when it is a separate one (a CDC-ACM device opened
   at interface 0: COMM + DATA), is released inside the same retried rounds (its interrupt transfer can be in flight
   like a bulk IN one), and a close whose data OR notification release still fails returns the error without
   removing or freeing the device (before, a failed notification release was only logged and the device, its
   interrupt transfer included, was freed: a use-after-free where upstream aborted). `cdc_dev_t` gains
   `data_released` and `notif_released` (set when that interface's release succeeded, so a retried close of a device
   whose first close failed releases only what is still claimed and does not halt endpoints of a released interface).
   radio_mux keeps the handle of a failed close and closes it again (usb_transport.c, a few seconds of retries).
   The notification interface counts as separate only when open claimed it, i.e. with an interrupt transfer (a device
   whose notification element is EP0 has none and its interface is never claimed, so releasing it would fail forever).

## Known upstream behaviour (not patches)

- **`GET /api/node` and `/api/devices` can report a stale node** (`state: "disabled"`, `networkName: "OpenThread"`,
  `rloc16: 0xfffe`) while `GET /node` and `/node/state` say `leader` (found in Task 22). Cause, in upstream `1f5af46`
  as well: `rest_self` (called once from `esp_br_thread_rest_init`) builds the border router's own collection item
  from `rest_node()` once and state changes never refresh it (only a network-diagnostics action can merge attributes into it); `/node` calls `rest_node()` on every request. Upstream
  initialises the REST worker at web server start, before Thread is configured, so the snapshot is "disabled" there too;
  with patch 11's lazy worker (created on the first request after the ready flag) the snapshot is taken at the first
  Thread REST request instead. Not a regression of patches 2, 9 or 11, and nothing in our code reads that item. Home
  Assistant (python-otbr-api) uses only `/node` and `/node/dataset/active`, which are live. Left as it is; use
  `/node`, `/node/state` or `/node/dataset/active` for the current state.

## Configuration (not patches)

- `CONFIG_OPENTHREAD_CLI_OTA=n` in `p4/sdkconfig.defaults`: the Kconfig option of
  `espressif/esp_ot_cli_extension` that enables its OTA command (which uses `esp_br_http_ota` and
  would bypass the bundle format). Its default is already `n`; it is set explicitly.
- `p4/main/idf_component.yml` pins `esp_ot_cli_extension ~2.0.0`, `mdns ^1.0.0`,
  `esp-serial-flasher ==1.11.0` (the version proven in Phase 0), and `protocol_examples_common` from
  `${IDF_PATH}/examples/common_components` (`esp_br_web.c` includes `protocol_examples_common.h`).
- R14 (2026-10-31): the RCP firmware is distributed and updated separately from the P4 image. There is no
  `espressif/esp_rcp_update` dependency any more (historical: it was pinned `==1.7.0` and configured with
  `CONFIG_AUTO_UPDATE_RCP=n`), no `rcp_N` partitions and no bundled RCP image. `CONFIG_OPENTHREAD_RCP_COMMAND=n`
  in `p4/sdkconfig.defaults` keeps `esp_ot_cli_extension` from optionally requiring `esp_rcp_update`.
  The one-time effect on existing boards: `partitions.csv` changed, so flash the P4 once over USB
  (`make erase flash`); an OTA cannot move the partition table.

- Thread console: `CONFIG_HTTPD_WS_SUPPORT=y` with `CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT=y` (the per-interface policy
  check before the upgrade) and `CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y` (attach to the console right after it; the
  server does not call the URI handler for the handshake).
- `CONFIG_OPENTHREAD_BR_START_WEB` is not set: it is an esp-thread-br example Kconfig option we do
  not vendor; `app_main` calls `esp_br_web_start` itself.
- Task 19 (netcfg, spec 5.13): `CONFIG_LWIP_IP_FORWARD=n` explicit; `CONFIG_MDNS_PREDEF_NETIF_{STA,AP,ETH}=n` and
  `CONFIG_MDNS_MAX_INTERFACES=2` (netcfg registers its Ethernet netifs with mDNS itself, `mdns_register_netif`; the predefined
  interfaces would use all slots). `-Wl,--wrap=lwip_hook_ip6_input` / `--wrap=lwip_hook_ip6_route` are INTERFACE link options of
  the `netcfg` component. lwIP options that change `struct netif` stay untouched (`CONFIG_LWIP_PPP_SUPPORT=y`,
  `CONFIG_LWIP_NUM_NETIF_CLIENT_DATA=0`, no VLAN hook).
