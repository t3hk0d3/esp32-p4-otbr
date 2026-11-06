# Zigbee Mode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A radio channel set to `ziggurat` runs zigpy/ziggurat on the P4 as a Zigbee coordinator that ZHA reaches at
`ws://<board>/zigbee`, on the C6 or a USB RCP dongle, without OpenThread.

**Architecture:** A new component `zigbee` registers the `ziggurat` mode consumer with `radio_mux`, owns one `zigbee`
task that runs every `ziggurat_*` call, drives the channel's RCP through its own small C spinel client on the mux's
frames and events, and serves ziggurat's binary protocol over a WebSocket. Ziggurat itself is the upstream
`crates/ziggurat-ot` staticlib, built for the P4 by cargo from a pinned submodule, with its heap renamed into PSRAM.

**Tech Stack:** ESP-IDF 6.1, `radio_mux` (feat/radio-mux), OpenThread's plain-C `lib/spinel/spinel.c` (encode/decode
only), Rust 1.99 for `riscv32imafc-unknown-none-elf` (host rustup or the `rust:1.99-slim` image), esp_http_server
WebSocket, hardware AES (`aes/esp_aes.h`), Unity host tests on the `linux` target, pytest-embedded with zigpy-ziggurat
1.2.1, esp-zigbee-sdk for the test device.

**Spec:** `docs/superpowers/specs/2026-11-06-zigbee-mode-design.md` (page sketch:
`docs/superpowers/specs/2026-11-06-zigbee-page-mockup.html`). It builds on
`docs/superpowers/specs/2026-11-06-radio-channel-mux-design.md` and its plan
`docs/superpowers/plans/2026-11-06-radio-channel-mux.md`, both on branch `feat/radio-mux`.

## Styleguide

`STYLEGUIDE.md`, enforced by `make lint`, as for every plan in this repo:
- astyle for C, ruff for Python, `snake_case`. Public symbols prefixed `zigbee_` (add it to the styleguide's prefix
  list in Task 4). The staticlib owns `ziggurat_*`; nothing of ours uses that prefix except the platform functions the
  staticlib imports (`ziggurat_platform_*`) and the renamed heap imports (`zigbee_heap_*`).
- One `static const char *TAG = "zigbee";` per file.
- `esp_err_t` returns with `ESP_RETURN_ON_ERROR` / `ESP_GOTO_ON_ERROR`; `ESP_ERROR_CHECK` only in `app_main` init.
- Pure logic in its own files with no ESP-IDF calls, built alone on the `linux` target (like `radio_mux/core.c`).
  Test seams (ops structs) only where a host test needs them.
- Host tests in `p4/components/zigbee/test_host/`, named `TEST_CASE("<scenario>: <effect>", "[zigbee]")`. Target
  tests in `p4/pytest/test_zigbee.py`. Page tests in `tools/tests/test_zigbee_page.py`.
- Nothing on the OpenThread task or the esp_timer task calls lwIP (STYLEGUIDE, tasks and locks); the same holds for the
  `zigbee` task: frames to the client go through the sender task.
- Vendored `esp_ot_br_server` changes (the frontend and the web server) go into `p4/components/PATCHES.md`.
- Commands are run as `set -o pipefail; <command> 2>&1 | tee "$LOG_DIR/<name>.log"` with `$LOG_DIR` your scratch
  directory; read the log instead of rerunning.
- No board access inside tasks: host tests and builds only. On-board checks are written in Task 9 and run in the
  hardware pass (Task 10), after the final review and its fixes.

## Global Constraints

- **Mode name** `ziggurat`; firmware need `spinel`; channel limit 1; `reboot_on_change` true; `frames` true. The mux
  enforces these from the registration; 409 `mode_taken`, `not_available`, `incompatible`, `hybrid_not_supported` are
  the mux's.
- **Ziggurat upstream:** `https://github.com/zigpy/ziggurat.git`, submodule at
  `p4/components/zigbee/ziggurat`, pinned to `f917f9898a1c7da2e5e5aa9add7c6d9d6a48ee76` (dev, 2026-11-04); built
  crate `crates/ziggurat-ot` with `--features esp-idf`, target `riscv32imafc-unknown-none-elf`, Rust 1.99.
- **Build fixes, applied by the build script and checked:** radium 0.7 copy with `"riscv32imafc"` added to its
  no-64-bit-atomics arm, through `--config 'patch.crates-io.radium.path=...'`; every soft-float archive member deleted;
  `heap_caps_aligned_alloc` / `heap_caps_realloc` / `heap_caps_free` renamed to `zigbee_heap_aligned_alloc` /
  `zigbee_heap_realloc` / `zigbee_heap_free` inside `libziggurat_ot.a`, and the build fails if any other `heap_caps_*`
  or `malloc`-family import remains.
- **zigpy-ziggurat** pinned to 1.2.1 in the target test environment; bumped only together with the submodule.
- **Endpoint:** WebSocket `/zigbee` on the existing HTTP server, one binary frame per message, maximum inbound frame
  2048 bytes (larger closes the socket), one client, the newest takes over. The stack's first outbound frame is its
  `hello`, kept and sent first to every later client. zigpy waits 5 s for it.
- **Threads:** every `ziggurat_*` call on the `zigbee` task (core `NET_CORE`, priority equal to the OpenThread task's;
  add `ZIGBEE_TASK_CORE` / `ZIGBEE_TASK_PRIORITY` to `common/board_cores/include/board_cores.h` and
  `p4/pytest/core_map.py`). Mux events, inbound frames and the timer post to its queue.
- **Memory:** ziggurat's heap from `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT` only; current and peak bytes counted.
- **Radio loss:** RESET or an unsolicited spinel reset → attach sequence and replay of the stored configuration and
  source-match table; GONE → "no radio" (transmits and energy scans fail at once, configuration edits stored, no
  reboot); a later attach of the same channel → replay, never a second `ziggurat_start`.
- **API:** `GET /device/zigbee` (409 when no channel runs `ziggurat`); `/device/status` gains `zigbee_channel` (id or
  null); `/metrics` gains `zigbee_*` series only while a channel runs `ziggurat`.
- **UI:** a top-level `Zigbee` link in the slot the mux's Task 9 leaves in `api.js`, shown when a channel runs
  `ziggurat`; one page `zigbee.html`, status only, no settings.
- **No OpenThread** in the `zigbee` component: no `otLink*`, no `esp_openthread_*` calls.

## Coordination with the mux implementer

The user's requirement: the mux is built in parallel by the session **"RCP USB dongle support"**
(`local_696308eb-deec-45c5-9e0c-2e162ecfd592`, branch `feat/radio-mux`, worktree
`.claude/worktrees/feat-radio-mux`). This plan depends on its Tasks 4 (consumer API), 7 (OTBR on any channel,
`NO_RADIO`, the `c6` consumer), 8 (device API) and 9 (Radios page and the regrouped navigation). Rules for the whole
plan:

- **Never edit the mux's files** (`p4/components/radio_mux/`, `rcp_proxy/`, `p4/ot_port/`, the mux's parts of
  `br_core`, `radios.html`, `radios.js`). A change they need is a request sent to that session with `SendMessage`
  (`to`: the session's name as `ListAgents` shows it), stating the file, the exact interface change and why. Then wait
  for its answer; do not work around a missing interface locally.
- **Sync points** are steps in the tasks below, marked **[sync]**. At each one: read `git log feat/radio-mux` for the
  task it names, read the mux's `radio_mux.h` (or the file named), and message the mux session with what this branch
  is about to rely on. Record the mux commit you built against in the task's commit message (`mux: <hash>`).
- **Shared files** both branches touch: `p4/main/app_main.c`, `p4/main/CMakeLists.txt`,
  `common/board_cores/include/board_cores.h`, `p4/pytest/core_map.py`, `p4/components/device_api/status_json.c`,
  `device_api.c`, `frontend/static/api.js`, `status.js`, `p4/components/PATCHES.md`, `STYLEGUIDE.md`, `docs/api.md`,
  `CHANGELOG.md`, `mkdocs.yml`. After a commit that touches one, send the mux session one line naming the file and the
  lines changed, so its next merge or rebase expects it.
- **Merging the mux in:** only with the user's go-ahead in chat. Bring `refs/heads/feat/radio-mux` into
  `feat/zigbee-mode` at the sync points that need it (Task 4 and later); never push or rewrite `feat/radio-mux`.
- **The board is shared.** Before any on-board run (Task 10), ask the mux session whether its hardware pass is running,
  and read `/device/status` `app.version` in its own step (the rig rule). Never flash over its build without the user's
  explicit OK.
- **Merge order:** the mux PR merges to `master` first; this branch is then rebased on `master` and its PR opened.
  Tell the mux session when this PR is opened and when it merges.

## Review Focus

1. **Two clients racing** (ZHA restarting while an old socket still sends): the newest client gets the kept `hello`
   first and every later frame; the old socket's late frames are discarded, never interleaved (Task 5 host test
   `takeover: late frames from the old client are dropped`).
2. **Frame-pending table full** (more sleepy children than the RCP's source-match table holds): the add returns
   nonzero to ziggurat, the stored table stays consistent, and a later replay sends only what the RCP accepted before
   (Task 3 host test `source match: an add the RCP refuses is not replayed`).
3. **Unplug during a transmit or an energy scan:** the pending operation completes as failed at once, and the next
   transmit after the radio returns is not refused as "one in flight" (Task 3 host tests `gone: a transmit in flight
   fails` and `gone: an energy scan in progress fails`, and `reattach: a transmit after replay is accepted`).
4. **A reset storm** (RESET arriving while the attach sequence still runs): the sequence restarts from the top, the
   replay runs once, no request waits on a reply that will never come (Task 3 host test `reset during attach: the
   sequence restarts and replays once`).
5. **A client that stops reading:** the sender task blocks on the socket, the queue fills, `host_send` returns false,
   the `zigbee` task never blocks, and frames resume after the ready signal once the client reads again or a new client
   takes over (Task 5 host test `backpressure: a full queue refuses and signals ready once drained`).

## File map

- **Create `p4/components/zigbee/`:**
  - `CMakeLists.txt` (linux branch: pure files only), `idf_component.yml` (none needed unless the build adds one),
    `ziggurat/` (git submodule), `build_ziggurat.cmake` (the cargo step), `tools/build_staticlib.sh` (cargo, the three
    archive fixes, the import check), `rust-toolchain.toml`.
  - Pure: `include/zigbee_spinel.h` + `spinel_client.c` (the spinel client state machine over an ops seam),
    `include/zigbee_ccm.h` + `ccm.c` (CCM* over a block-encrypt callback), `include/zigbee_link.h` + `link.c` (hello
    cache, client takeover, outbound queue accounting), `include/zigbee_heap_stats.h` + `heap_stats.c`.
  - Board: `include/zigbee.h` (public API), `zigbee_priv.h`, `zigbee.c` (registration, attach/detach, the task),
    `platform.c` (the 20 `ziggurat_platform_*` functions and the heap shim), `ws.c` (handler and sender task),
    `status.c` (counters, last configuration, snapshot).
  - `test_host/` (Unity project like `radio_mux/test_host`), with recorded spinel frames in `test_host/main/frames.h`.
- **Create** `tools/zigbee_test_device/` (an esp-zigbee-sdk on/off light for the aux C6, its own IDF project),
  `p4/pytest/test_zigbee.py`, `p4/pytest/zigbee_device.py` (aux C6 helper), `docs/zigbee.md`,
  `p4/components/esp_ot_br_server/frontend/zigbee.html`, `frontend/static/zigbee.js`, `tools/tests/test_zigbee_page.py`.
- **Modify** `p4/main/app_main.c`, `p4/main/CMakeLists.txt`, `p4/components/device_api/` (`device_api.c`,
  `status_json.c`, `device_api_priv.h`, a new `zigbee_json.c/.h`, `test_host/main/test_zigbee_json.c`,
  `CMakeLists.txt`), `p4/components/metrics/` (Zigbee series), `frontend/static/api.js` (the Zigbee link),
  `frontend/static/status.js` (Dashboard card), `p4/components/PATCHES.md`, `common/board_cores/include/board_cores.h`,
  `p4/pytest/core_map.py`, `p4/pytest/pytest.ini` (marker `zigbee_device`), `p4/pytest/requirements.txt`,
  `.github/workflows/build.yml` (Rust target), `.gitmodules`, `NOTICE`, `STYLEGUIDE.md`, `docs/building.md`,
  `docs/api.md`, `docs/README.md`, `mkdocs.yml`, `CHANGELOG.md`.

## BDD skeleton

The contract is the spec's section 5. Restated per module with the plan's names; every effect is one test.

```
zigbee_spinel (pure client)
    zigbee_spinel_attach
        On a fresh channel
            - sends CMD_RESET, then reads PROTOCOL_VERSION, NCP_VERSION, CAPS, RADIO_CAPS, HWADDR
            - sets PHY_ENABLED and MAC_RAW_STREAM_ENABLED, then replays any stored configuration and source-match table
            - reports ready to the caller
        With a protocol major version other than 4 or a missing required radio cap
            - reports incompatible with the reason, sends nothing more
        On RESET (or an unsolicited LAST_STATUS reset) while attaching
            - restarts the sequence from CMD_RESET and replays once
    zigbee_spinel_configure
        - sends PHY_CHAN, PHY_TX_POWER, MAC_15_4_PANID, MAC_15_4_SADDR, MAC_15_4_LADDR, MAC_PROMISCUOUS_MODE,
          MAC_SRC_MATCH_ENABLED, MAC_RX_ON_WHEN_IDLE_MODE where supported
        - stores the configuration for replay
        - returns nonzero on an RCP error status
        With no radio
            - stores the configuration, sends nothing, returns 0
    zigbee_spinel_src_match_clear / _add_short / _add_ext
        - edits the RCP table and the stored copy
        On an add the RCP refuses
            - returns nonzero and keeps it out of the stored copy
    zigbee_spinel_transmit
        - sends one STREAM_RAW frame with channel, CSMA, max retries and max backoffs metadata
        On the RCP's answer
            - reports acked, no ack, channel access failure, aborted or failed
        With a frame in flight, or longer than 127 bytes with FCS
            - returns nonzero without sending
        On an RCP without rx-on-when-idle
            - puts the radio back to receive after the answer
        With no radio
            - returns nonzero at once
    zigbee_spinel_energy_scan
        - scans one channel for the duration and reports the peak RSSI
        With no radio
            - returns nonzero at once
    received STREAM_RAW frame
        - delivers the PSDU without FCS, with channel, RSSI, LQI and the mux's receive time
    request timeout (one per request, 2000 ms)
        - asks for a channel reset
    On GONE
        - fails a transmit in flight and an energy scan in progress at once
        - enters no radio; later operations fail or are stored as above
    On attach after GONE
        - runs the attach sequence and replays; reports ready

zigbee_ccm
    Encrypt with the reference vectors
        - writes the expected ciphertext and MIC
    Decrypt with the right MIC
        - writes the plaintext and returns 0
    Decrypt with a wrong MIC
        - returns nonzero
    With a zero-length payload, or a MIC length of 0
        - follows CCM* exactly (MIC only, or encryption only)

zigbee_link (pure)
    host send
        With a client and room
            - queues the frame, returns true
        With a full queue
            - returns false; signals ready once the queue drains
        With no client
            - drops the frame, returns true
    first outbound frame
        - is kept as the hello
    client connect
        With a kept hello
            - queues the hello first, then makes this the client
        With another client connected
            - replaces it; late frames from the old client are dropped
    inbound frame
        Over 2048 bytes
            - closes the socket
        Not binary
            - ignored

zigbee_heap (shim)
    alloc, realloc, free
        - serve from PSRAM only, honour alignment, count current and peak bytes
    On PSRAM exhausted
        - return NULL

zigbee (board)
    zigbee_register
        - registers the ziggurat consumer: needs spinel, limit 1, reboot on change, frames
    attach (first time)
        - binds the queue, runs the spinel attach, then starts ziggurat on the zigbee task
    attach (after GONE)
        - binds the queue, runs the attach and replay, never starts ziggurat again
    channel unavailable or incompatible
        - ziggurat not started; status reports the reason
    WebSocket handshake on /zigbee
        With no channel running ziggurat
            - refused (the socket is closed after the handshake)
        With the stack running
            - becomes the client per zigbee_link (hello first)
    panic
        - logs, keeps the message in RTC no-init memory for the next boot's last_reset, aborts

device_api
    GET /device/zigbee
        With a ziggurat channel
            - returns channel, firmware, state, client, last configuration, counters, PSRAM use
        Otherwise
            - 409
    GET /device/status
        - zigbee_channel: the channel id or null

web UI
    nav
        With a ziggurat channel
            - shows the top-level Zigbee link
        Otherwise
            - no Zigbee link
    zigbee.html
        - running / waiting for Home Assistant / no radio / no Zigbee radio, as the mockup
```

## Tasks

### Task 1: Build ziggurat for the P4

**Files:** Create `.gitmodules` entry and submodule `p4/components/zigbee/ziggurat`,
`p4/components/zigbee/tools/build_staticlib.sh`, `p4/components/zigbee/build_ziggurat.cmake`,
`p4/components/zigbee/rust-toolchain.toml`, a minimal `p4/components/zigbee/CMakeLists.txt` that runs the cargo step and
adds the prebuilt library. Modify `.github/workflows/build.yml`, `NOTICE`, `docs/building.md`. Reference:
`spike/ziggurat-p4` `tools/build_ziggurat.sh`.

**Interfaces:**
- Produces: `${CMAKE_BINARY_DIR}/ziggurat/libziggurat_ot.a` and `.../ziggurat.h` (generated by the crate's cbindgen),
  linked into the `zigbee` component; the staticlib's imports are exactly the 20 `ziggurat_platform_*` functions,
  `zigbee_heap_aligned_alloc/realloc/free`, and libc/libgcc symbols.

- [ ] **Step 1:** Add the submodule at the pinned commit; `rust-toolchain.toml` pins `1.99` with the target.
- [ ] **Step 2:** `build_staticlib.sh <out_dir>`: runs cargo on the host when `rustup target list --installed`
  has the target, else in `rust:1.99-slim` (as the spike); applies the radium patch (fails if the edit did not apply);
  deletes soft-float members (as the spike); renames the three heap imports with `objcopy --redefine-sym` on every
  member; then fails unless `nm -u` lists no `heap_caps_*`, `malloc`, `calloc`, `realloc` or `free`.
- [ ] **Step 3:** `build_ziggurat.cmake`: an `add_custom_command` producing the archive and header, depending on the
  submodule's `Cargo.lock` and the script; `add_prebuilt_library` from it.
- [ ] **Step 4:** CI: install the target with `rustup target add riscv32imafc-unknown-none-elf` in the build job
  (the IDF container has no Rust: install rustup with the pinned toolchain first). `NOTICE`: ziggurat, Apache-2.0.
  `docs/building.md`: the Rust prerequisite and the Docker fallback.
- [ ] **Step 5: Verify:** the full image cannot link until Task 5 provides the platform functions, so build the
  archive alone: `p4/components/zigbee/tools/build_staticlib.sh "$LOG_DIR/zig"` (`t1-lib.log`) prints the import list
  and passes its own check. Keep the component out of `main`'s requirements until Task 5.
- [ ] **Step 6: Commit:** `build(zigbee): ziggurat staticlib for the P4 from a pinned submodule`.

### Task 2: Spinel client: attach, configuration and operations (pure)

**Files:** Create `include/zigbee_spinel.h`, `spinel_client.c`, `test_host/` (project, `main/test_spinel.c`,
`main/frames.h` with frames recorded from OpenThread's `RadioSpinel` talking to the C6 and to the CC2652P; record them
from the spike board or build them with `spinel.c` and document which). The linux build compiles IDF's
`openthread/src/lib/spinel/spinel.c` by path.

**[sync] Step 0:** mux Task 4 committed on `feat/radio-mux`: read `radio_mux.h`; confirm with the mux session that
`radio_mux_ev_t {type, slot, len, rx_us}`, `radio_mux_tx`, `radio_mux_release` and `radio_mux_reset` are final, and
that a slot holds one complete spinel frame (header byte first, no HDLC, no SPI header). Record the mux commit.

**Interfaces (produces, `zigbee_spinel.h`, no ESP-IDF types):**
- `typedef struct { int (*send)(void *ctx, const uint8_t *frame, size_t len); void (*request_reset)(void *ctx);
  int64_t (*now_us)(void *ctx); void *ctx; } zigbee_spinel_ops_t;`
- `typedef struct { void (*ready)(void *ctx, bool compatible, const char *reason); void (*rx)(void *ctx, const uint8_t
  *psdu, size_t len, uint8_t channel, int8_t rssi, uint8_t lqi, uint64_t time_us); void (*tx_done)(void *ctx,
  zigbee_tx_result_t r); void (*scan_done)(void *ctx, int8_t max_rssi); void *ctx; } zigbee_spinel_sink_t;`
  with `typedef enum { ZIGBEE_TX_ACKED, ZIGBEE_TX_NO_ACK, ZIGBEE_TX_CCA, ZIGBEE_TX_ABORTED, ZIGBEE_TX_FAILED }`.
- `typedef struct { uint8_t channel; int8_t tx_power_dbm; uint16_t pan_id, short_addr; uint8_t ext_addr[8]; bool
  promiscuous; } zigbee_radio_config_t;` (`ext_addr` in ziggurat's order; the client sends it as `RadioSpinel` does).
- `zigbee_spinel_t` (state struct, caller-allocated), `void zigbee_spinel_init(zigbee_spinel_t *, const
  zigbee_spinel_ops_t *, const zigbee_spinel_sink_t *);`, `void zigbee_spinel_attach(zigbee_spinel_t *);`,
  `int zigbee_spinel_configure(zigbee_spinel_t *, const zigbee_radio_config_t *);`, `int
  zigbee_spinel_set_channel(zigbee_spinel_t *, uint8_t)`, `int zigbee_spinel_set_promiscuous(zigbee_spinel_t *, bool)`,
  `void zigbee_spinel_src_match_clear(zigbee_spinel_t *)`, `int zigbee_spinel_src_match_add_short(zigbee_spinel_t *,
  uint16_t)`, `int zigbee_spinel_src_match_add_ext(zigbee_spinel_t *, const uint8_t ext[8])`, `int
  zigbee_spinel_transmit(zigbee_spinel_t *, const uint8_t *psdu, size_t len, uint8_t channel, bool csma, uint8_t
  retries, uint8_t backoffs)`, `int zigbee_spinel_energy_scan(zigbee_spinel_t *, uint8_t channel, uint16_t ms)`,
  `void zigbee_spinel_on_frame(zigbee_spinel_t *, const uint8_t *frame, size_t len, int64_t rx_us)`, `void
  zigbee_spinel_on_tick(zigbee_spinel_t *)` (timeouts), `bool zigbee_spinel_eui64(const zigbee_spinel_t *, uint8_t
  out[8])`. RESET/GONE handling is Task 3.

Requests are serialized: one outstanding transaction id (1–15) per request, the next sent on its reply; the source-match
table capacity is not assumed (the RCP's refusal is the limit). Confirm each property's encoding against
`radio_spinel.cpp` in IDF 6.1 before writing its test; where the CC2652P lacks `MAC_RX_ON_WHEN_IDLE_MODE`, re-arm
receive after each transmit answer.

- [ ] **Step 1: Failing host tests**, one per effect of the skeleton's `attach`, `configure`, `src_match`, `transmit`,
  `energy_scan`, received frame and request-timeout scenarios (those without "no radio"), plus Review Focus 2.
- [ ] **Step 2: Run, expect failure:** `make test-host` (`t2-red.log`).
- [ ] **Step 3: Implement** `spinel_client.c` to the interface above.
- [ ] **Step 4: Run, expect pass** (`t2-green.log`).
- [ ] **Step 5: Commit:** `feat(zigbee): spinel client for the RCP (pure), mux: <hash>`.

### Task 3: Spinel client: resets, radio loss and replay (pure)

**Files:** Modify `spinel_client.c`, `zigbee_spinel.h`, `test_host/main/test_spinel.c`.

**Interfaces (adds):** `void zigbee_spinel_on_reset(zigbee_spinel_t *)` (mux RESET; the client also calls it itself on
an unsolicited `LAST_STATUS` in the reset range), `void zigbee_spinel_on_gone(zigbee_spinel_t *)`, `bool
zigbee_spinel_has_radio(const zigbee_spinel_t *)`, `uint32_t zigbee_spinel_resets(const zigbee_spinel_t *)`.

- [ ] **Step 1: Failing host tests** for the skeleton's GONE, "with no radio" and attach-after-GONE effects, the
  unsolicited reset, and Review Focus 3 and 4.
- [ ] **Step 2–4:** red, implement, green (`t3-*.log`).
- [ ] **Step 5: Commit:** `feat(zigbee): replay on RCP reset and a no-radio state on loss`.

### Task 4: CCM*, heap accounting and the hello/client link (pure)

**Files:** Create `include/zigbee_ccm.h` + `ccm.c`, `include/zigbee_heap_stats.h` + `heap_stats.c`,
`include/zigbee_link.h` + `link.c`, `test_host/main/{test_ccm.c,test_heap_stats.c,test_link.c}`. Modify
`STYLEGUIDE.md` (prefix `zigbee_`); tell the mux session (shared file).

**Interfaces:**
- `int zigbee_ccm_crypt(void (*encrypt_block)(void *ctx, uint8_t block[16]), void *ctx, bool encrypt, const uint8_t
  nonce[13], const uint8_t *auth, size_t auth_len, const uint8_t *in, uint8_t *out, size_t len, uint8_t *tag, size_t
  tag_len);` (0 or nonzero; `in`/`out` may alias; `tag_len` 0 or 4..16 even). The spike's `zigbee.c` CCM is the
  reference; test vectors: RFC 3610 packet vectors 1–3 (truncated to the tag lengths used) and IEEE 802.15.4-2006
  Annex C.2 (MIC-32).
- `zigbee_heap_stats_t {size_t current, peak; uint32_t failures;}` with `zigbee_heap_stats_alloc(st, size)`,
  `_free(st, size)`, `_realloc(st, old, new)`.
- `zigbee_link_t`: `void zigbee_link_init(zigbee_link_t *, size_t queue_depth)`, `bool zigbee_link_host_send(
  zigbee_link_t *, size_t len)` (accounting only: true = enqueue it), `bool zigbee_link_take_hello(zigbee_link_t *,
  const uint8_t *frame, size_t len)` (true the first time), `int zigbee_link_connect(zigbee_link_t *, int fd)` (returns
  the previous fd or -1; marks "send hello first"), `bool zigbee_link_accept_inbound(const zigbee_link_t *, int fd,
  size_t len, bool binary)` (false = drop; `len > 2048` sets "close"), `bool zigbee_link_drained(zigbee_link_t *)`
  (true once when a refused send may retry). Queue depth 32.

- [ ] **Step 1: Failing host tests** per the skeleton's `zigbee_ccm`, `zigbee_link`, heap accounting, and Review
  Focus 1 and 5.
- [ ] **Step 2–4:** red, implement, green (`t4-*.log`).
- [ ] **Step 5: Commit:** `feat(zigbee): CCM*, heap accounting and the WebSocket link rules (pure)`.

### Task 5: The `zigbee` component on the board

**[sync] Step 0:** mux Tasks 4 and 7 committed. Ask the user for the go-ahead to merge `refs/heads/feat/radio-mux` into
this branch; after the merge, tell the mux session the commit you merged. Confirm with it that a consumer registered
in `app_main` before `radio_mux_start()` is attached on the `c6` channel when its stored mode is `ziggurat` (Task 7 of
the mux registers OTBR's consumer the same way).

**Files:** Create `include/zigbee.h`, `zigbee_priv.h`, `zigbee.c`, `platform.c`, `ws.c`, `status.c`; complete
`CMakeLists.txt` (target branch: `REQUIRES esp_http_server`, `PRIV_REQUIRES radio_mux esp_timer esp_hw_support
freertos heap log mbedtls esp_system`). Modify `p4/main/app_main.c`, `p4/main/CMakeLists.txt`,
`common/board_cores/include/board_cores.h`, `p4/pytest/core_map.py`, the web server's extra-handler registration
(`app_main.c`'s extra-handlers callback, as the spike did), `p4/components/esp_ot_br_server` `EXTRA_HANDLER_SLOTS`
+1 with a `PATCHES.md` entry. Tell the mux session (shared files).

**Interfaces:**
- `zigbee.h`: `esp_err_t zigbee_register(void);` (registers the consumer; call before `radio_mux_start`),
  `esp_err_t zigbee_register_ws(httpd_handle_t server);`, `typedef struct { ... } zigbee_status_t;` (channel id,
  firmware text, state `ZIGBEE_STATE_{IDLE, STARTING, RUNNING, NO_RADIO, INCOMPATIBLE}`, reason, client address and
  connect time, last client and leave time, `zigbee_radio_config_t` last config, counters `rx, rx_dropped, tx, tx_no_ack,
  tx_cca, tx_failed, energy_scans, rcp_resets, frames_in, frames_out`, heap current/peak, started time),
  `void zigbee_get_status(zigbee_status_t *out);` (any task; copies under a spinlock), `const char
  *zigbee_channel(void);` (id or NULL).
- `platform.c`: the 20 `ziggurat_platform_*` functions over Tasks 2–4 and IDF (esp_timer one-shot posting to the
  queue; wake = a coalesced queue post; `esp_fill_random`; AES block through `esp_aes`; CCM through `zigbee_ccm_crypt`;
  `host_send` through `zigbee_link` and the sender queue; radio functions through `zigbee_spinel_*`; EUI-64 from the
  client; `esp_restart`; panic text into an `RTC_NOINIT_ATTR` buffer then `abort()`; `last_reset` returns it once) and
  `zigbee_heap_aligned_alloc/realloc/free` on `heap_caps_*` with `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT` and
  `zigbee_heap_stats`.
- The task loop: one queue of tagged items (mux event, inbound frame, timer, wake, tick, host-ready); mux RX slots are
  passed to `zigbee_spinel_on_frame` and released before the next item.

- [ ] **Step 1:** Write the board code to these interfaces, including the handshake rule of the skeleton's
  "WebSocket handshake" scenario (no ziggurat channel: refuse).
- [ ] **Step 2: Verify:** `make build` (`t5-build.log`) passes; `riscv32-esp-elf-nm p4/build/p4.elf` shows no
  unresolved `ziggurat_platform_*`; `make test-host` still passes (`t5-host.log`).
- [ ] **Step 3: Commit:** `feat(zigbee): ziggurat on a radio_mux channel with its own task and WebSocket, mux: <hash>`.

### Task 6: Device API and metrics

**[sync] Step 0:** mux Task 8 committed (its `status_json.c` and `/device/channels` changes). Tell the mux session the
`status_json.c` lines this task adds.

**Files:** Create `p4/components/device_api/zigbee_json.c/.h`, `test_host/main/test_zigbee_json.c`. Modify
`device_api.c` (route `{"/device/zigbee", HTTP_GET, ...}`), `status_json.c` (`zigbee_channel`),
`device_api_priv.h`, `CMakeLists.txt` (`PRIV_REQUIRES zigbee`), `test_host/main/{CMakeLists.txt,test_status.c}`,
`p4/components/metrics/` (series `zigbee_frames_total{dir="rx|tx"}`, `zigbee_tx_results_total{result=...}`,
`zigbee_rcp_resets_total`, `zigbee_heap_bytes{kind="current|peak"}`, `zigbee_client_connected`, only while a channel
runs `ziggurat`), its host test.

**Interfaces:** `esp_err_t zigbee_json_status(const zigbee_status_t *st, char *buf, size_t cap, size_t *len);` with the
field names: `channel`, `firmware`, `state` (`idle|starting|running|no_radio|incompatible`), `reason`, `client`
(`{address, since}` or null), `last_client` (`{address, left}` or null), `radio` (`{channel, pan_id, short_address,
extended_address, tx_power_dbm}` or null), `counters` (the status counters by name), `heap` (`{current, peak}`),
`started`.

- [ ] **Step 1: Failing host tests** for the skeleton's `device_api` effects and each JSON field.
- [ ] **Step 2–4:** red, implement, green (`t6-*.log`); `make build`.
- [ ] **Step 5: Commit:** `feat(device_api): /device/zigbee, zigbee_channel and Zigbee metrics`.

### Task 7: Zigbee page and the navigation link

**[sync] Step 0:** mux Task 9 committed: read its `api.js` slot for the Zigbee link and how a page learns the channel
modes (`/device/status` `zigbee_channel`). Agree with the mux session that this task only fills the slot and adds a
Dashboard card in `status.js`, and tell it the lines.

**Files:** Create `frontend/zigbee.html`, `frontend/static/zigbee.js`, `tools/tests/test_zigbee_page.py`. Modify
`frontend/static/api.js` (the slot), `status.js` (a Zigbee card when `zigbee_channel` is set), `style.css` only if the
mockup needs classes the stylesheet lacks, `p4/components/PATCHES.md` (frontend entry).

**Interfaces:** the page polls `GET /device/zigbee` every 5 s (2 s while `starting`); copy, layout and states from the
mockup (`2026-11-06-zigbee-page-mockup.html`): Running, Waiting for Home Assistant, No radio, No Zigbee radio (409).

- [ ] **Step 1: Failing page tests** with `device_pages_harness.js` on recorded `/device/zigbee` bodies for the four
  states, the Copy button's text, and the nav link present only with `zigbee_channel` set.
- [ ] **Step 2–4:** red, implement, green: `make test-tools` (`t7-*.log`), then `make build`.
- [ ] **Step 5: Commit:** `feat(web): Zigbee page and navigation link`.

### Task 8: Test device for the rig

**Files:** Create `tools/zigbee_test_device/` (IDF project for `esp32c6`: esp-zigbee-sdk's on/off light as a router,
`idf_component.yml` pinning `espressif/esp-zigbee-lib` and `esp-zboss-lib`; a console command `factory_reset` that
leaves the network and starts joining; on/off state printed on change), `p4/pytest/zigbee_device.py` (open the aux C6
by its by-id path `usb-1a86_USB_Single_Serial_5B61094735-if00` after `fuser -v` shows it free; flash with esptool when
its app description is not this project's; `factory_reset()`, `wait_joined()`, `light_state()`). Modify `Makefile`
(`zigbee-test-device` target building it), `p4/pytest/requirements.txt` (zigpy-ziggurat 1.2.1, pyserial),
`p4/pytest/pytest.ini` (marker `zigbee_device`).

- [ ] **Step 1:** Build the test device with IDF 6.1: `make zigbee-test-device` (`t8-build.log`). If esp-zigbee-sdk
  does not build on 6.1, build it in the `espressif/idf:v5.5` image from the same target and note the reason in the
  project's README.
- [ ] **Step 2:** Write `zigbee_device.py` (no board run here).
- [ ] **Step 3: Commit:** `test(zigbee): aux C6 on/off light test device and its helper`.

### Task 9: On-board tests and docs

**Files:** Create `p4/pytest/test_zigbee.py`, `docs/zigbee.md`. Modify `docs/api.md`, `docs/README.md`, `mkdocs.yml`,
`CHANGELOG.md`; tell the mux session (shared files).

- [ ] **Step 1: Write the tests** (not run here), parametrized over the radio (`c6`, and the CC2652P channel when
  present, marker `usb_dongle` from the mux plan):
  `test_switch_to_ziggurat_and_back` (PUT mode, reboot, `/device/zigbee` running, Thread menu hidden; back to `otbr`,
  same dataset), `test_form_and_energy_scan`, `test_device_joins_and_toggles` (marker `zigbee_device`: join, interview,
  on/off from zigpy changes `light_state()`, a report reaches zigpy), `test_reconnect_gets_hello_within_5s`,
  `test_second_client_takes_over`, `test_radio_reset_survived` (C6 `rcp_glitch`; dongle: the mux's consumer reset;
  `rcp_resets` +1, client still connected, device answers), `test_radio_gone_and_back` (C6 `rcp_kill` then `c6_hold
  0` and the mux's re-attach; dongle: manual unplug prompt; `no_radio`, `uptime_s` keeps growing, device answers after
  return), `test_internal_heap_not_lower_than_idle`, `test_latency_p50` (zigpy on/off round trip, record the median).
- [ ] **Step 2: Docs:** `docs/zigbee.md` (what Zigbee mode is, choosing it on Radios, connecting ZHA, the page,
  radio loss and resets, the no-login warning, limits), `/device/zigbee` and `zigbee_channel` in `docs/api.md`, index,
  nav and CHANGELOG entries.
- [ ] **Step 3:** `make lint` on the changed files and `make docs` (`t9-*.log`).
- [ ] **Step 4: Commit:** `test, docs(zigbee): on-board tests and the Zigbee documentation`.

### Task 10: Hardware pass (after the final review and its fixes)

**[sync] Step 0:** ask the mux session whether its hardware pass (its Task 11) is running or planned; agree an order so
the two never share the board at once. Get the user's OK for the flash budget: one P4 app flash over the network, the
aux C6 flashed once with the test device.

- [ ] Read `/device/status` `app.version` in its own step; compare with the last build put on the board; stop and ask
  if it differs.
- [ ] `make build-test && make ota BOARD=192.168.1.117`; wait about 70 s.
- [ ] `make test-target TEST=p4/pytest/test_zigbee.py` (with `TARGET_PYTEST` per the rig notes), then with the
  CC2652P: `MARK=usb_dongle`.
- [ ] Review Focus checks on the board: ZHA restart during traffic (two clients), more sleepy children than the table
  holds (if the rig has enough devices; else note it), unplug during a transmit, `rcp_glitch` repeated 5 times in 2 s,
  a client that stops reading for 60 s.
- [ ] Manual acceptance with the user: Home Assistant's ZHA against the board, a real device paired from ZHA and
  controlled, a ZHA restart, a board reboot.
- [ ] Put the C6 back to `otbr` (the ha-thread-bbc1 network rejoined) and restore `ot_cli` on the aux C6 if a Thread
  suite needs it next. Record results in the session's memory notes and the PR description; tell the mux session the
  board is free.
