# Leader weight and "Become leader": design

Date: 2026-11-03. Status: approved 2026-11-03; plan `docs/superpowers/plans/2026-11-03-leader-weight.md`. Mockup: https://claude.ai/artifact/DHVssN3ZEPZXetCP8KTECj (Leader card on the Network
page, and its four states).

## 1. Purpose and scope

On a Thread network shared with a Linux border router (ot-br-posix), the Linux one always ends up as leader. Both report
the same leader weight (65), so nothing in Thread prefers this board, and a healthy leader is never replaced anyway.
The user wants to make this board the leader and keep it there through later partition merges.

In scope: a stored leader weight adjustment, applied whenever Thread starts and live; a "Become leader" action; an API
for both; a Leader card on the Network page showing this board's and the current leader's weight; docs; tests.

Out of scope: an automatic take-over (the board never takes over on its own), the other device properties (power
supply, CCM), metrics, a Dashboard change, changing the weight of other border routers.

## 2. Facts this design rests on (checked in IDF 6.1's OpenThread)

- Thread version 1.4 is the default, so `OPENTHREAD_CONFIG_MLE_DEVICE_PROPERTY_LEADER_WEIGHT_ENABLE` is on and
  `otThreadSetDeviceProperties` exists. The weight is `DeviceProperties::CalculateLeaderWeight`: base 64, +1 for a border
  router (`mIsBorderRouter` is preset when border routing is built in), +0 for the default power supply
  (`OPENTHREAD_CONFIG_DEVICE_POWER_SUPPLY` = external), plus `mLeaderWeightAdjustment`, clamped to −16..+16. So this
  board is 65 today, and 49..81 with the adjustment.
- `Mle::ComparePartitions` orders partitions by leader weight first, then non-singleton, then partition ID. The weight
  only matters when partitions meet (after a leader loss or a split); a router that joins a partition never displaces
  its leader.
- `otThreadBecomeLeader` starts a new partition with this device as leader. While attached it refuses with
  `OT_ERROR_NOT_CAPABLE` unless the local weight is strictly greater than the current leader's
  (`otThreadGetLeaderWeight`), or when the device may not be a router. Other errors: `OT_ERROR_INVALID_STATE` when
  Thread is disabled or the dataset is incomplete.
- `otThreadSetDeviceProperties` updates the local weight at once. The partition's advertised weight is fixed when its
  leader takes the role, so a change made while this board is leader shows in the partition only after the next
  partition this board forms.
- OpenThread does not store device properties; the firmware has to apply them at every start.

## 3. Decisions

| Topic | Decision |
| --- | --- |
| Setting | one integer, the weight adjustment, −16..+16, default 0. Nothing else of the device properties is exposed |
| Storage | NVS, its own namespace `thread`, key `leader_adj` (i8), written only when the value changes; survives reboot and firmware updates; not touched by `net reset` |
| Apply | at every Thread bring-up before Thread is enabled; live under the OpenThread lock in `BR_RUNNING` |
| API | one small endpoint, `/device/thread`, for what no existing API has: the stored adjustment, this board's weight, and the take-over (`become_leader`). Everything about the leader and the role comes from the existing `GET /node` |
| API, read | `GET /device/thread` (below) |
| API, write | `PUT /device/thread` with either or both keys: `{"leader_weight_adjustment": n, "become_leader": true}`. At least one key; unknown keys are ignored; `"become_leader": false` does nothing. Order: validate the whole body first, then store and apply the adjustment, then try the take-over |
| PUT reply | `200` with the GET body plus `"applied": true`, or `false` (with a `detail` when a live step failed) when the adjustment was stored but not applied live (not `BR_RUNNING`, or the OpenThread lock not free within 1 s: applied at the next start). With `become_leader`, the reply adds `"become_leader": "started"` |
| PUT errors | in the style of `/device/names`: 400 `invalid_config` with `field` `leader_weight_adjustment` (not an integer, outside −16..16), `become_leader` (not a boolean) or `body` (no JSON object, or neither key); `length_required`, `invalid_request`; 413 `payload_too_large`; 500 `storage_failed`. Take-over refusals: 409 `cannot_take_over` with `detail` `weight not higher` (the leader's weight is equal or higher), `already leader`, `not attached` (detached or disabled), `not router eligible`; 409 `busy` `mode <MODE>` when not `BR_RUNNING`; 503 `busy` `OpenThread is busy, retry` when the lock was not free within 1 s. An adjustment in the same body is stored even when the take-over is refused (the error's `detail` stays the reason; a GET shows the stored value) |
| Take-over trigger | only the button (`become_leader` in a PUT). Never automatic, never at boot |
| Take-over persistence | none: only the weight is stored |
| Modes | the setting can be saved in every mode; take-over only in `BR_RUNNING` |
| Access | the same as `/device/names`: routes registered in `device_api.c`'s table next to it, served and gated the same way |
| Router eligible | `otThreadIsRouterEligible` (input to the take-over decision) |
| Live apply fails | the value stays stored; PUT answers 200 with `"applied": false` and a `detail` naming the failure (as `/device/names` does) |
| UI placement | a "Leader" card at the top of the Network page (`network.html`) |
| Slider | applies on Save, not while dragging; the label shows the resulting weight and "not saved" while it differs |
| Button | greyed out unless the saved weight is strictly greater than the leader's (`leaderData.weighting` of `GET /node`), this board is not the leader, and there is no unsaved slider change; the hint says why. The page decides this from its two reads; the PUT checks again under the OpenThread lock, so a stale page can only get a 409 |
| Confirm | a dialog before the take-over PUT, listing the effects. The page sends the adjustment (Save) and the take-over (button) as separate PUTs |
| Leader name | "this board" when this board is leader, else `router 0x<rloc16>` (`leaderRouterId` × 1024, in hex). No Topology nickname: `GET /node` does not give the leader's extended address |

### `GET /device/thread`

```json
{"leader_weight_adjustment": 8, "leader_weight": 73, "leader_weight_base": 65}
```

- `leader_weight_adjustment`: the stored value; always present.
- `leader_weight`: `otThreadGetLocalLeaderWeight`; `leader_weight_base` = `leader_weight − leader_weight_adjustment`
  (what the UI shows as "base"). Both are omitted when OpenThread is not running, or when the OpenThread lock was not
  free within the bounded wait the other Thread reads use (then `"partial": true` is added).

### What the page reads from `GET /node` (existing, unchanged)

`state` (this board's role: `disabled`, `detached`, `child`, `router`, `leader`), `leaderData.weighting` (the leader's
weight), `leaderData.leaderRouterId` and `leaderData.partitionId`. `leaderData` is meaningless unless `state` is
`child`, `router` or `leader`.

## 4. Architecture

- `br_core` (owns OpenThread):
  - `esp_err_t br_core_set_leader_adjustment(int8_t adj)`: range check, NVS write when changed, live apply in
    `BR_RUNNING` (`otThreadGetDeviceProperties`, change `mLeaderWeightAdjustment`, `otThreadSetDeviceProperties`).
    Returns whether it was applied or only saved.
  - `int8_t br_core_leader_adjustment(void)`: the stored value; never takes a lock.
  - `esp_err_t br_core_leader_weight(uint8_t *out)`: `otThreadGetLocalLeaderWeight` with a bounded lock wait.
  - `esp_err_t br_core_become_leader(void)`: checks and `otThreadBecomeLeader` under the lock; maps the OpenThread errors
    to the reasons in section 3.
  - Bring-up applies the stored adjustment next to the TREL setting, before Thread is enabled.
  - A pure helper decides the take-over refusal reason (or none) from (mode, role, router eligible, local weight,
    leader weight), so it is host-tested.
- `device_api`: the GET and PUT handlers (the PUT parses both keys and calls `br_core_set_leader_adjustment`, then `br_core_become_leader`), JSON in and out, error replies through the existing `json_err` helpers.
- Web UI: the Leader card on `network.html` and a new `static/leader.js` (ES5 like the other pages). It polls
  `GET /device/thread` and `GET /node` together every 5 s while the page is visible, and every 1 s for 30 s after a
  take-over until `state` is `leader`. A pure part (verdict, button state and hint, leader name) takes both replies
  and is what the frontend tests drive. The card shows the role, both weights with this board's breakdown (base and adjustment), a 48..96 scale
  with both markers, a one-line verdict (lower / equal / higher / this board is the leader), the slider with Save, the
  Become leader button with its hint, and an open "What Become leader does" box:
  1. This board starts a new partition with itself as leader: same network, same devices.
  2. The other routers, the old leader included, join it; their end devices follow.
  3. Mesh traffic can drop for a few seconds; addresses and pairings don't change.
  4. The dataset doesn't change; the old leader keeps working as a border router.
  Afterwards: only the weight is stored. If this board is offline for more than about 2 minutes (a quick restart keeps the role), another router takes over and keeps it, even with a lower weight. Press Become leader again then. Why greyed out: OpenThread only takes over from a
  leader with a strictly lower weight.
  When OpenThread is not running the card shows the stored adjustment, keeps Save working, and hides the comparison.
- Docs: the two rows (`GET`, `PUT` with both keys) in the Device API table of `docs/api.md`; a subsection "Choosing the leader" under "Adding more
  border routers" in `docs/api.md` (why another border router stays leader, the adjustment, the button).

## 5. Testing

- Host tests: the take-over decision helper (every reason), adjustment range and JSON parsing, the GET JSON writer.
- Frontend tests in `tools/tests/` with the existing fake-DOM harness: button state and hint for lower / equal /
  higher / leader / unsaved / not attached / OpenThread not running, Save enablement, the leader name.
- The on-board test is written in the plan but run in the batched hardware pass after review and fixes, not inside a
  task. The hardware pass (2026-11-03) confirmed the "Afterwards" text (section 6).
- On the board, with the Thread peer (ot_cli on the C6 devkit) as the other router: the peer forms the network and is
  leader with weight 65 (`leaderweight`); the board joins; take-over is refused (`weight not higher`); adjustment +1;
  take-over succeeds and the board becomes leader while the peer stays attached as a router; the adjustment survives a
  reboot. With the peer at `leaderweight 81`, +16 still refuses.

## 6. Risks

- Measured on the board (2026-11-03): a quick restart (~10 s down, `POST /device/reboot`) kept the leader role in the same partition; with Thread off for ~3 minutes the rest of the mesh kept the old partition for about 125 s, then a router with weight 65 took over in a new partition, and when this board (weight 66) rejoined it did not take the role back, because weights only count when partitions merge. The "Afterwards" copy was corrected accordingly.
- ot-br-posix may set a higher weight on some builds (CCM border router: 72, stable power: +4). Up to 81 is reachable
  with +16; above that the button stays greyed out and the hint says so.
- A take-over interrupts mesh traffic for a few seconds; the confirm dialog says so.
- Two boards of this firmware with high adjustments and both clicking take over: OpenThread's strict comparison keeps
  them from flipping back and forth; equal weights refuse.
