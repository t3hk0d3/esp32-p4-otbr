# Hybrid mode: Thread and Zigbee at once, on separate radios: design

Status: approved by the user, 2026-11-10, with the three decisions of section 9 as recommended. The user asked for hybrid mode on 2026-11-09 and for this design after
PR #42 (channel/mode orchestrator) merged. A spike on 2026-11-10 (section 2) ran both stacks together on the board.
Branch `feat/hybrid-mode`, one PR. It changes one rule of `2026-11-08-channel-mode-orchestrator-design.md` (section
4.3: "Thread and Zigbee never both bound") and the "No hybrid" limit of `docs/zigbee.md`. Everything else in those
designs stands.

## 1. Intent

The board has two radios: the built-in ESP32-C6 and one USB dongle. Today Thread (OTBR) and Zigbee (Ziggurat) exclude
each other, even on separate radios: a PUT that would bind both answers 409 `hybrid_not_supported`. Hybrid mode lifts
that exclusion, so one board serves a Thread network and a Zigbee network at the same time, each on its own radio.

What success looks like:

- A user with a C6 and a dongle sets one to `otbr` and the other to `ziggurat`, and both run, across reboots.
- Neither stack degrades the other in normal use: no Thread packet loss under Zigbee load, Zigbee latency as on its own.
- A loss of one radio (unplug, fault, maintenance) leaves the other stack running.

## 2. Evidence: the spike (2026-11-10)

Branch `spike/hybrid` 47462a5 (local): master 73bc984 with only the `hybrid_not_supported` condition removed from
`radio_mux/orch_core.c`. Thread on the C6 (router of the home network, channel 15), Zigbee on the OllyZig CC2652P dongle
(channel 20), test light on an ESP32-C6.

| Check | Thread only | Hybrid |
| --- | --- | --- |
| Internal heap free / lowest | 213.6 / 169.0 KB | 195.3 / 153.7 KB |
| Zigbee, 6 min on/off load | (alone: p50 91, p95 100 ms) | 3768 round trips, 0 failures, p50 94, p95 110 ms |
| Thread, pings to 4 routers from the board | 80, 0% loss | 720 during the load, 0% loss, round trips unchanged |
| Reboot | | both back within 16 s |
| Dongle unplug and replug | | Thread untouched; Zigbee `no_radio` "not detected", `running` 21 s after the replug |

Nothing else in the firmware depended on the exclusion: br_core has no reference to Zigbee, and the Zigbee component
none to OpenThread.

## 3. Goals, non-goals, anti-goals

Goals:

- Allow one `otbr` binding and one `ziggurat` binding at once, on different channels, in either assignment (Thread on
  the C6 and Zigbee on the dongle, or the reverse).
- Keep every other binding rule: one mode per channel, one Thread, one Zigbee, the firmware fit, the proxy's port rule.
- Keep each stack's behaviour on loss, reset and maintenance of its own radio as it is today.
- Tell the user when both networks share an 802.15.4 channel (section 5.3; the form is open decision 1).
- Tests and docs that match.

Non-goals:

- Thread and Zigbee on the same radio (one RCP serving both: needs a multi-PAN RCP).
- A Ziggurat stop. Entering or leaving Zigbee still reboots the board (section 5.2).
- More than one dongle, or more than one Thread or Zigbee instance.
- Choosing the Zigbee channel on the board: ZHA (the client) chooses it when it forms the network.

Anti-goals (a failure even if it works):

- A fault, unplug or maintenance of one radio that stops, restarts or reconfigures the other stack.
- A change to the board mode (`/device/status` `mode`) or its reasons in any scenario that does not involve Thread.
- Breaking stored settings: a board updated from master comes up with the same bindings, and a downgrade to a build
  with the exclusion must not boot into an inconsistent state (section 6).

## 4. Constraints

- Style: `STYLEGUIDE.md` and pre-commit; vendored components keep upstream formatting (`PATCHES.md` for any change).
- Internal heap: hybrid measured 195.3 KB free (lowest 153.7 KB) against 213.6 KB for Thread only. No new task; no
  further internal RAM is to be spent by this work.
- RF rule for tests: never form or join a test network on 802.15.4 channels 11 (home Zigbee), 15 (home Thread) or 25
  (spinel2quic testbed). Zigbee tests use 20, Thread test networks 26. Hybrid tests that keep the home Thread network do
  not form one.
- Hardware budget: tasks run host tests and builds only; one batched on-board pass after the review.

## 5. The model

### 5.1 The rule

The orchestrator's binding rule "Thread and Zigbee never both bound" (`radio_mux_orch_check_bind`, refusal
`RADIO_MUX_REFUSE_HYBRID`) goes. The rules that remain already give hybrid its shape:

- one instance per channel: a channel runs `otbr` or `ziggurat`, never both;
- one Thread binding and one Zigbee binding;
- the firmware fit: both need spinel RCP firmware, which the C6 and the supported dongles have.

So a PUT of `ziggurat` on the dongle while Thread runs on the C6 binds Zigbee next to Thread, and the reverse. A PUT of
`otbr` on the channel that runs Zigbee is a move of Thread onto that channel, and unbinds Zigbee from it (below).

### 5.2 Reboots

Entering or leaving Zigbee reboots the board, as today (the Zigbee instance declares `reboot_on_change`). In hybrid this
takes Thread down for the reboot, about 16 s in the spike. The PUT's reply keeps `"reboot": true`, and the Radios page
says before the save that Thread will restart. Changes that do not bind or unbind Zigbee (a Thread move between the
other radios, a proxy change) behave as today and do not reboot.

### 5.3 Same 802.15.4 channel

Thread's channel is the board's active dataset; Zigbee's is the one ZHA formed on (`/device/zigbee` `radio.channel`).
When both are equal, both networks share the air: it works, but each slows the other under load. The board shows this
(decision 1). It never changes either channel itself.

### 5.4 Loss and maintenance

Unchanged per stack, and independent:

- An unplug, fault or reset of Zigbee's radio: Zigbee `no_radio`, then back; Thread untouched (shown in the spike).
- The same for Thread's radio: board mode as today; Zigbee untouched.
- C6 maintenance (firmware push, ROM recovery) asks only the instance bound to the C6. With Zigbee on the dongle and
  Thread on the C6, Thread drains and pauses as today; Zigbee keeps running.

### 5.5 Status and UI

- `/device/status`: `mode` stays Thread's status; `zigbee_channel` names Zigbee's channel, as today. No new fields.
- Dashboard: already shows the Thread and Zigbee sections side by side; both are filled at once. The same-channel
  notice (5.3) goes in the Zigbee section, linking the Radios page.
- Radios page: the save text for a change that enters or leaves Zigbee says that the board reboots and Thread restarts.
  The `hybrid_not_supported` text goes.
- Docs: `zigbee.md` (the "No hybrid" limit becomes a short "Thread and Zigbee together" section), `radios.md`, `api.md`
  (409 list), CHANGELOG.

## 6. Storage

No new record: the per-channel stored modes already hold both bindings. A downgrade to a build that still has the
exclusion: its load rules keep duplicates of one mode only, not the hybrid pair, so it would boot with both bound,
which that build never tested. Builder's call: either accept it (documented in CHANGELOG as "downgrade with Zigbee
off"), or have the old build's load path unbind Zigbee when Thread is also stored, which needs a change released before
hybrid. Decided in section 9, item 3.

## 7. API

- `PUT /device/channels/{id}`: no longer answers 409 `hybrid_not_supported`. Everything else as today.
- Reads: unchanged.
- Clients in this repo (Radios page, tests) drop the refusal. External clients only lose an error they could no longer
  get.

## 8. Testing

Host:

- Orchestrator rules: `otbr` and `ziggurat` on different channels bind in both orders; one mode per channel, one
  Thread, one Zigbee, the firmware fit and the proxy rule still refuse as today; a PUT of `otbr` on Zigbee's channel is
  a move that unbinds Zigbee with a reboot.
- Page tests: the save texts for entering and leaving Zigbee next to Thread; the same-channel notice on the Dashboard.

On the board (batched pass, RF rule):

- Thread on the C6 and Zigbee on the dongle, and the reverse: both run; heap against section 2.
- 6-minute Zigbee load with Thread pings from the board (as the spike); then Thread traffic with Zigbee idle.
- Reboot: both come back.
- Dongle unplug and replug in both assignments: the other stack untouched.
- A C6 firmware push with Thread on the C6 and Zigbee on the dongle: Zigbee keeps running.
- `test_zigbee.py` and `test_channels.py` with hybrid cases added; the existing suites as regression.

## 9. Decisions (the user's, 2026-11-10)

1. Same-channel notice (5.3): a warning in the Zigbee section of the Dashboard, linking the Radios page. No refusal: ZHA
   picks the Zigbee channel after the bind.
2. Both assignments are supported and tested: Thread on the C6 with Zigbee on the dongle, and the reverse.
3. Downgrade (section 6): documented in CHANGELOG and  as "turn Zigbee off before going back to an older
   firmware"; no load-path guard.

## 10. Left to the builder

- Names, log texts, the exact notice wording.
- Whether `RADIO_MUX_REFUSE_HYBRID` is removed from the enum or left unused.
- The staging inside the branch.
