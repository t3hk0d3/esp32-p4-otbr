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
   (patch 19).
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

### REST guard classification (patches 2 and 9)

Classes: `NONE` no OpenThread access, always served. `READ` reads OpenThread state: 200 while OpenThread runs (patch 9
ready flag), 503 otherwise (RADIO_FAULT/STARTING before OpenThread is up, SNIFFER: `br_core` switches the flag off while
Thread is stopped, so Thread state reads answer 503 there). `THREAD_OP` changes Thread/dataset state or needs the radio:
only in BR_RUNNING with OpenThread ready; SNIFFER 409, RADIO_FAULT/STARTING 503. THREAD_OP is checked twice: lock-free
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

- `CONFIG_OPENTHREAD_BR_START_WEB` is not set: it is an esp-thread-br example Kconfig option we do
  not vendor; `app_main` calls `esp_br_web_start` itself.
- Task 19 (netcfg, spec 5.13): `CONFIG_LWIP_IP_FORWARD=n` explicit; `CONFIG_MDNS_PREDEF_NETIF_{STA,AP,ETH}=n` and
  `CONFIG_MDNS_MAX_INTERFACES=2` (netcfg registers its Ethernet netifs with mDNS itself, `mdns_register_netif`; the predefined
  interfaces would use all slots). `-Wl,--wrap=lwip_hook_ip6_input` / `--wrap=lwip_hook_ip6_route` are INTERFACE link options of
  the `netcfg` component. lwIP options that change `struct netif` stay untouched (`CONFIG_LWIP_PPP_SUPPORT=y`,
  `CONFIG_LWIP_NUM_NETIF_CLIENT_DATA=0`, no VLAN hook).
