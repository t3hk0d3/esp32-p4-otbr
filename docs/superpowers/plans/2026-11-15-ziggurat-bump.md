# Bump Ziggurat to upstream main (6c35405)

Branch: `feat/zigbee-latest-upgrade-1f833e`. Styleguide: `STYLEGUIDE.md` at the repo root (C side), upstream rustfmt for the
local patch. Status: approved, implemented, on-board pass done 2026-11-15.

## What changes upstream

Submodule `p4/components/zigbee/ziggurat` goes from `f917f98` to `6c35405` (13 commits, 80 files). Notable:

| PR | Change | Effect on us |
|----|--------|--------------|
| #93 | `SendGpdf`: Green Power frames sent at a set time after the GPDF they answer | **C API break** (see below) |
| #86 | Git info in `GetFirmwareInfo` version, from a new `build.rs` | Needs an env var from our build (see below) |
| #97, #99 | `no_std` crates, most heap allocations removed | Lower heap use; our archive checks must still pass |
| #101, #102, #96, #95, #92, #89, #90 | Routing/broadcast/leave fixes, router ageing 4 periods | Behaviour only |
| #103 | Less log noise, more context | Behaviour only |
| #98 | RCP reconnect with backoff | ziggurat-server only, not the MCU staticlib |

Checked already: `patches/ziggurat-ot-log.patch` applies cleanly; the set of `ziggurat_platform_*` imports is unchanged;
CCM is now called in place (`input == output`), which `zigbee_ccm_crypt` already handles (MAC over plaintext first on
encrypt, CTR is byte-wise); radium 0.7 is still in the lock file, so the archive fix-up still applies; MSRV 1.98 ≤ our
pinned 1.99.

## Breaking points and fixes

1. **`ziggurat_platform_radio_transmit` signature.** Gains `rx_channel` (receiver channel after the TX) and
   `tx_delay` / `tx_delay_base_time` (timed TX, both 0 = send now). Our spinel `STREAM_RAW` set already carries these
   fields (`txDelay`, `txDelayBaseTime`, `rxChannelAfterTxDone`); today we send 0, 0 and `channel`.
   - `platform.c`: new signature, passes all three through.
   - `spinel_client.c` / its header: `zigbee_spinel_transmit` takes the three values and puts them in the frame.
2. **RX timestamp clock.** The timed TX base is the GPDF's RX timestamp, and the RCP schedules it on its own radio clock.
   We pass the mux's host receive time to `ziggurat_radio_rx`, so a timed GPDF would land at the wrong time (in
   practice "late → sent at once"). Ziggurat uses this timestamp for nothing else (checked: only the GPDF trigger, and
   the host protocol's packet field). Upstream's own spinel PHY passes the RCP timestamp.
   - `handle_stream_raw` passes the RCP timestamp it already unpacks; the sink's `time_us` argument becomes radio time.
   - Anything else in the component that reads that argument as host time is switched to the mux time (to be checked
     while implementing; `zigbee.c` sink is the only caller seen).
3. **Timed TX support on the RCP.** OT reports it as `OT_RADIO_CAPS_TRANSMIT_TIMING` (bit 4). Not required for
   Zigbee in general, so attach does not fail without it; the cap is recorded and a delayed transmit on an RCP without
   it is refused locally (Ziggurat then reports the GPDF as not delivered). Whether our C6 RCP build sets it is checked
   during implementation (c6_rcp sdkconfig); if not, enabling it is a separate decision, raised then.
4. **Version string (#86).** `build_staticlib.sh` builds from a copy inside the build dir, so `build.rs` would run
   `git` against the **esp32-otbr** repo (wrong describe, plus `rerun-if-changed` on every tracked file of ours), or find
   no `git` in the Docker image. The script sets `ZIGGURAT_GIT_VERSION` from `git -C <submodule> describe --always`
   (with `-dirty` if the submodule tree is dirty) and `SOURCE_DATE_EPOCH` from the submodule's last commit, on host and
   in Docker.
5. **Hybrid mode / radio mux.** `rx_channel` other than the TX channel is new. Confirm the radio_mux passes
   `rxChannelAfterTxDone` through untouched for the Zigbee client (it owns the channel in Zigbee and hybrid mode).

Out of scope: `tools/zigbee_test_device` (esp-zigbee-lib 2.1.0) is a test peer, not the stack; left as is.

## BDD skeleton

```
zigbee_spinel_transmit(s, psdu, len, channel, rx_channel, csma, retries, backoffs, tx_delay, tx_delay_base_time)
    On an immediate frame (tx_delay 0)
        - sends one STREAM_RAW with txDelay 0, txDelayBaseTime 0
        - sends rxChannelAfterTxDone = rx_channel (not channel)
        - returns 0 and marks a transmit in flight
    On a timed frame (tx_delay > 0) and the RCP has TRANSMIT_TIMING
        - sends txDelay and txDelayBaseTime as given
        - returns 0
    On a timed frame and the RCP lacks TRANSMIT_TIMING
        - returns ZIGBEE_SPINEL_ERR_LOCAL
        - sends nothing, no transmit in flight
    On the existing failure cases (not attached, in flight, oversize)
        - unchanged: returns ZIGBEE_SPINEL_ERR_LOCAL

zigbee_spinel attach
    On an RCP with TRANSMIT_TIMING
        - records it in radio_caps; attach succeeds
    On an RCP without TRANSMIT_TIMING
        - attach still succeeds (cap not in CAPS_REQUIRED)

handle_stream_raw (via an RX notification)
    On a received frame
        - hands the sink the RCP timestamp from the frame, not the mux receive time
        - other fields unchanged (PSDU without FCS, channel, RSSI, LQI)

ziggurat_platform_radio_transmit
    On success
        - forwards channel, rx_channel, csma, retries, backoffs, tx_delay, tx_delay_base_time to the spinel client
        - counts tx
    On spinel refusal
        - returns the error and counts tx_failed

ziggurat_platform_ccm_crypt
    On in-place encrypt (input == output)
        - output equals the reference ciphertext and tag
    On in-place decrypt with a valid tag
        - output equals the plaintext, returns 0
    On in-place decrypt with a bad tag
        - returns nonzero

build_staticlib.sh
    On a clean submodule
        - built archive's version string is "<crate version>+<submodule describe>"
    On a dirty submodule
        - version string ends in "-dirty"
    On a second build with no changes
        - cargo does not rerun the ziggurat-protocol build script (no rerun-if-changed on esp32-otbr files)
    Existing archive checks
        - still pass (no stray malloc/heap_caps imports, ziggurat_platform_log imported, ziggurat_log_set_level exported)
```

## Steps

1. Bump the submodule to `6c35405`; build to confirm the expected compile/link break (signature only).
2. Spinel client: transmit parameters, TRANSMIT_TIMING cap, RCP timestamp to the sink. Tests in
   `test_host/main/test_spinel.c` first (from the skeleton), then code.
3. `platform.c` transmit wrapper; in-place CCM cases in `test_host/main/test_ccm.c`.
4. `build_staticlib.sh`: `ZIGGURAT_GIT_VERSION` and `SOURCE_DATE_EPOCH`, host and Docker paths; header comment updated.
5. Check c6_rcp for TRANSMIT_TIMING and radio_mux for `rxChannelAfterTxDone`; report, don't change the RCP here.
6. Host tests, full P4 build (`make`), `docs/zigbee.md` and CHANGELOG note.
7. Review, then one batched on-board pass (clerk lease first): form on ch 20, join a device, check firmware version
   string in ZHA/zigpy, routing after a router drop. Green Power only if a GPD is at hand.

## Verification commands

```
make -C p4/components/zigbee/test_host test 2>&1 | tee <scratch>/zigbee_host_tests.log
make build 2>&1 | tee <scratch>/p4_build.log
```
