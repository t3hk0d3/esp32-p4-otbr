# ZLOE satellite on the ESP32-P4: plan (spike)

Status: awaiting approval. Part of zigpy/ziggurat#84. Design source: `crates/ziggurat-phy-trel/MCU.md` on
`t3hk0d3/ziggurat` branch `spike/trel-backbone`.

**Goal.** A P4 in the new radio mode `ziggurat-satellite` runs Ziggurat as a router on the C6 or a dongle, reaches the
ZLOE backbone over its own Ethernet, and interoperates with the host implementation (ZLOE v2, `sec=none`).

**Spike rigor.** Only what proves the hypothesis: one light review at the end, tests for the logic below, no fix rounds
for minor findings, no user guide (a short bench report instead).

## Styleguide

- This repo: `STYLEGUIDE.md`. C is astyle via `make lint`, public symbols `zigbee_`, one `TAG` per file, `esp_err_t`
  with `ESP_RETURN_ON_ERROR`, host tests `TEST_CASE("<scenario>: <effect>", "[zigbee]")`, ops structs only as test seams.
  The zigbee task never calls lwIP.
- Ziggurat: `MCU.md`'s rules. `cargo fmt`, clippy `-D warnings -Dclippy::all -Dclippy::nursery`, `no_std` + `alloc` for
  MCU code.

## Decisions to confirm

1. **The bootstrap lives in C, as a local client of the stack's existing control protocol.** It sends `GetHwAddress`,
   `Configure`, `StartNetwork` and `SendBroadcast`/`SendUnicast` through `ziggurat_host_frame` and reads responses and
   `ReceivedAps` where the WebSocket would. The payloads are byte-aligned and small. This needs no new stack API and can
   be tested in `test_host`. The cost is a small second copy of `derive_nwk`, `clock_counter` and the ZDP answers from
   `satellite.rs`, pinned by test vectors taken from the Rust code.
2. **`ZloeCore` is sans-I/O.** A plain state struct with no locks, clocks or sockets: time comes in as milliseconds, and
   addresses are `core::net::SocketAddr`. The host keeps it under its mutex, the MCU under a blocking mutex.
3. **Two sockets on the board (IPv4 and IPv6) instead of one dual-stack socket.** The board announces both address
   families and the host prefers IPv6 link-local. Cost: 2 sockets and 2 UDP PCBs from the lwIP budget.
4. **A satellite with no coordinator or no clock keeps waiting** and shows why in its state, instead of giving up after
   60 s as the host does. Nothing is stored on the board: settings are fetched on every boot.
5. **`ziggurat` and `ziggurat-satellite` count as one singleton** (one stack per board). Thread on the other radio is
   allowed, as in hybrid mode. Changing into or out of the mode reboots.

## Structure

**Ziggurat** (new branch `spike/zloe-mcu` from `spike/trel-backbone`, in this repo's submodule, pushed to the fork only):

- New crate `crates/ziggurat-zloe-core`: wire header and kinds, MAC header parsing, identity, peer table (`BTreeMap`),
  coordinators, dedup, health to LQI, link liveness, resolved-peer handling, broadcast plan, and the generic async
  unicast retry loop.
- `BackboneTransport` trait: `send(to, datagram)` and `wait_ack(seq, timeout)`. The runtime supplies both, so the
  3 × 50 ms loop exists once.
- `ziggurat-phy-trel`: `TrelPhy` keeps sockets, mDNS, multicast joins, QUIC and the deny list, and calls the core for
  everything else.
- `ziggurat-ot`, feature `zloe` (default build unchanged): `ZloePhy<OtLinkPhy>` implementing `RadioPhy`, with a merged
  receive stream that needs no extra task. FFI:
  - in: `ziggurat_zloe_datagram`, `ziggurat_zloe_peer_resolved`, `ziggurat_zloe_peer_removed`, `ziggurat_zloe_set_epan`,
    `ziggurat_zloe_request_netinfo`, `ziggurat_zloe_peers`;
  - out: `ziggurat_platform_zloe_send`, `ziggurat_platform_zloe_netinfo`.

**This repo** (`p4/components/zigbee`, `radio_mux`, `device_api`):

- `satellite.c` (pure): the bootstrap state machine, frame encoding and decoding, NWK and counter derivation, ZDP answers.
- `zloe_txt.c` (pure): TXT build and parse, the coordinator filter, address ranking.
- `zloe.c` (board): task `zigbee_zloe` on `MGMT_CORE` owning the sockets and mDNS. Received datagrams go to the zigbee
  task's mailbox; datagrams to send come back through a queue.
- `radio_mux`: mode `RADIO_MUX_MODE_ZIGGURAT_SATELLITE`, wire name `ziggurat-satellite`, stored in NVS.
- `zigbee.c` / `platform.c`: role chosen at bind. In satellite mode the WebSocket refuses clients and `host_send` feeds
  `satellite.c`.
- `GET /device/zigbee`: adds `role`, the satellite's state and the peer list.
- Submodule pin moves to `spike/zloe-mcu`; `build_staticlib.sh` passes `--features esp-idf,zloe`; `sdkconfig.defaults`
  socket and PCB budgets grow by 2.

## Behaviour

```
ZloeCore
    on_datagram
        Data from a peer on our extended PAN ID
            - returns the frame once, with LQI from the peer's health
            - refreshes the peer's address, NWK and last-received time
            - returns an ack datagram when the ack flag is set
        Retransmit of a delivered frame
            - returns no frame
            - still returns the ack
        Datagram from another extended PAN ID, or from ourselves
            - returns nothing and learns no peer
        Malformed datagram (short, bad magic, wrong version)
            - returns nothing
        Ack for a pending unicast
            - completes that unicast
        NETINFO_RSP
            - returns the settings bytes
    on_air_frame
        Source is a live backbone peer
            - drops the frame
        Source is not a peer, or the peer's link has gone stale
            - keeps the frame
    plan_transmit
        Broadcast data frame
            - transmits on the radio
            - makes one datagram per peer, or per confirmed multicast group
        Broadcast MAC command
            - transmits on the radio only
        Unicast to a peer
            - sends over the backbone
        Unicast to an unknown destination
            - transmits on the radio only
    send_unicast
        Acked
            - returns Acked and raises the peer's health
        Three tries without an ack
            - falls back to the radio
            - lowers the peer's health
            - rotates the peer to its next address
    resolved
        Peer on our network, matching security mode
            - creates the peer
        Peer resolved before our identity is known
            - becomes a peer once the identity is set
        Coordinator
            - is kept for bootstrap even without an identity
TrelPhy
    existing suite
        After the rebuild on ZloeCore
            - cargo test -p ziggurat-phy-trel stays green
ZloePhy (ziggurat-ot, feature zloe)
    transmit
        No identity yet
            - passes the frame to the radio
    subscribe_rx
        Frames from radio and backbone
            - delivers both on one stream
zigbee_zloe_txt
    build
        Router with identity
            - writes v=2, xa, xp, nwk, role=router, sec=none and no mc
    parse
        Complete record
            - yields EUI64, extended PAN, NWK and role
        Missing or malformed xa
            - rejects the record
        sec other than none
            - rejects the record
    rank_addresses
        IPv6 link-local and IPv4 on our subnet
            - puts link-local first
        Address on another subnet, or loopback
            - leaves it out
zigbee_satellite
    derive_nwk
        Known EUI64s
            - matches the vectors from satellite.rs
            - never returns 0x0000 or 0xFFF8 and above
    clock_counter
        Known time
            - matches satellite.rs (seconds since 2026-01-01, times 10)
    run
        Clock not valid
            - stays in waiting_clock
            - requests no settings and sends no Configure
        No coordinator found
            - stays in waiting_coordinator and retries each second
        Settings received
            - sends Configure with role router, derived NWK, own EUI64, clock-seeded counters, no TCLK seed
            - then sends StartNetwork
            - then broadcasts Device_annce to 0xFFFD with capability 0x8E
            - ends in running
        Malformed settings, or coordinator not started
            - stays in waiting_coordinator
        Configure or StartNetwork refused
            - goes to failed with the status as the reason
    on_received_aps
        Node_Desc_req for our NWK
            - sends Node_Desc_rsp as a router
        Active_EP_req for our NWK
            - sends Active_EP_rsp with no endpoints
        ZDP request for another NWK, or another cluster
            - sends nothing
radio_mux
    mode names and storage
        ziggurat-satellite
            - round-trips through its wire name and NVS
    radio_mux_singleton_dedupe
        ziggurat and ziggurat-satellite both stored
            - keeps the first and turns the other off
zigbee WebSocket
    upgrade
        Satellite mode
            - refuses the client
```

Tests come from this tree: Rust in `ziggurat-zloe-core`, C in `zigbee/test_host` and `radio_mux/test_host`. The board
glue (`zloe.c`) is covered by the hardware pass.

## Steps

1. Ziggurat: branch, `ziggurat-zloe-core` with tests, `TrelPhy` on it. `cargo test -p ziggurat-phy-trel` green, built in
   `rust:1.99-slim` with the target dir outside the tree.
2. Ziggurat: `zloe` feature and FFI; the MCU staticlib builds with and without it. Push the branch to the fork.
3. Bump the submodule. Confirm `patches/ziggurat-ot-log.patch` still applies; the coordinator build picks up the spike's
   driver changes on this branch only.
4. `radio_mux` mode, pure C modules and their host tests.
5. Board glue, role handling, status. `make build-test`, host tests, `make lint`.
6. One light review.
7. Hardware pass (below), bench report in `crates/ziggurat-phy-trel/MCU.md`. Push to `t3hk0d3/esp32-p4-otbr`, no PR.

## Hardware pass

Budget: OTA only, no C6 flash, ideally one test build plus one restore.

1. `clerk acquire p4-otbr-boards/p4-otbr-a`. Record `app.version` (`20261110T140400Z-3fe051f`), the radio modes and the
   network mode.
2. Ask the bench's owner session to switch to `sec=none`. The network is on channel 20, PAN `0xAD61`; the board never
   uses channels 11, 15 or 25.
3. OTA, set a radio to `ziggurat-satellite`. Check: the clock gate, the host's `[diag]` listing the board as a peer at
   cost 1/1, ZHA's topology scan showing it, and an On/Off to a device through it.
4. Record internal heap (free and lowest), PSRAM, app size against the slot, and static RAM.
5. Restore: rebuild `3fe051f` (branch `feat/usbip`) if no image is left, OTA, put the radio modes back, release the
   lease. A rebuilt image carries a new version stamp; the report will say so.

## Known limits of this version

- The network key travels in clear over `NETINFO` (`sec=none`, accepted for the spike).
- No joins through the satellite and no children (as on the host). No multicast (`mc` not announced). No QUIC.
- A coordinator that changes network settings needs a satellite reboot.
