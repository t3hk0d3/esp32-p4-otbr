# Channel/mode orchestrator: channels, mode instances and one orchestrator: design

Status: draft for review, 2026-11-08. The model was agreed with the user in a brainstorm on 2026-11-08, after PR #39
(radio channel mux) and PR #40 (Zigbee mode); both are merged (master dd5da30). Order: the user reviews this design,
then the Zigbee session (it asked to, for section 4.7 and Zigbee's reinit answer), then the plan is written. Branch
`feat/channel-orchestrator`, one PR. It replaces parts of
`2026-11-06-radio-channel-mux-design.md` (section 3: "Modes and rules", "OTBR mode", "Board mode, faults and status")
and changes how `2026-11-06-zigbee-mode-design.md`'s consumer is driven. Everything else in those designs stands.

It is called the orchestrator, not the coordinator, so it is not confused with the Zigbee coordinator, the network role
that Ziggurat plays.

## 1. Intent

### Why

Which service runs on which radio is decided today in three places that each reconcile on their own:

- the mux's attach machine (radio_mux `mux.c`), which attaches a channel's stored mode's consumer;
- br_core's `br_radio` task (`br_core_radio.c`), which starts and stops OpenThread from about fifteen flags
  (`s_start_failed`, `s_booting`, `s_c6_boot_steps`, `s_lost`, `s_retry`, `push_busy`, the attach generations, ...);
- rcp_proxy's own reconcile of its attach and detach wishes.

The C6's maintenance (its boot steps, the firmware push, the ROM recovery) lives in Thread's code, so it reset the C6
behind Zigbee's or the proxy's back. The fix in #39 (the mux holds the C6 for its boot steps) works, but it is one more
special case of the same kind. Each new mode or maintenance step adds flags in three places.

### What success looks like

- One place decides which mode runs on which channel: the orchestrator. It owns the configured bindings.
- A channel is a connection to an RCP with its own lifecycle. It knows nothing about modes.
- Each mode is a service with its own state machine that runs on at most one channel and decides for itself whether it
  can let its channel go.
- Maintenance of a radio (boot steps, firmware push, ROM recovery) is a channel state, `reinit`, that every mode
  handles, so no mode's radio is reset behind its back.
- A failure changes only a state and its reason. Only the user changes the configuration.
- From outside, the board behaves as it does today, except for the changes listed in section 9.

## 2. Goals, non-goals, anti-goals

Goals:

- Channels and mode instances as two separate layers with an orchestrator between them.
- The `reinit` channel state, required in every mode's state machine.
- Moves of Thread as rebinds: probe the target, commit, start; no fallback to the old radio.
- An honest read API: the channel's lifecycle and the bound mode's state shown separately.
- The C6's maintenance moved out of Thread's code.
- The board's built-in RCP as a channel kind of its own (section 4.1), so a board with another built-in RCP needs a new
  descriptor and maintenance ops, not a new channel kind. The C6 channel's id becomes `builtin-c6`.
- Remove the special cases this replaces: the C6 boot hold, the C6 job flags in br_core, br_radio's reconcile and the
  proxy's own attach reconcile.

Non-goals:

- Hybrid (Thread and Zigbee at once). The exclusion stays.
- More than one Thread or one Zigbee instance, or new modes.
- A Radios page redesign. Only its texts that describe the rollback change, and it may show the new fields.
- Changes to the transports (USB host, CDC driver, C6 SPI), the frame path, firmware detection or the proxy's adapters.
- A Ziggurat stop. Leaving Zigbee still reboots (section 4.7).
- Another built-in RCP (an ESP32-H2 board) or another transport for it (UART). Only the seam is built, with the C6
  over SPI as its one implementation.

Anti-goals (a failure even if it works):

- The orchestrator changing a configured binding on its own: a fault, an unplug or a refusal never moves or clears it.
- The orchestrator retrying a refusal or a failure by itself.
- A mode's radio reset or taken without its state machine having been asked first.
- A change to what a client sees in the scenarios of section 5 that is not listed in section 9: board modes, REST
  guards, reply codes and their `detail` texts.
- Breaking stored settings: a board updated from master must come up with the same modes and proxy settings.

## 3. Constraints

- ESP-IDF v6.1, ESP32-P4. Cores per `board_cores.h`: NET_CORE 0 (network, OpenThread, the C6 transport, the USB host
  and CDC task), MGMT_CORE 1 (HTTP, the mux owner task, rcp_proxy, br_radio).
- Style: `STYLEGUIDE.md` and pre-commit (`... | xargs uvx pre-commit run --files`). Vendored components keep upstream
  formatting, and every change to them is recorded in `p4/components/PATCHES.md` (next free entry: 35).
- Memory: internal RAM is already tight (the mux took about 65 KB). No new FreeRTOS task for the orchestrator (my call:
  it runs on the mux owner task). Internal heap free at idle with no dongle must not drop by more than 4 KB from
  master dd5da30 (my call, measured on the board before and after).
- The API's write shape is kept: `PUT /device/channels/{id}` with `mode` binds or unbinds, with today's bodies, codes
  and `detail` texts. `DELETE` is unchanged. The exceptions are in section 9: the move change, and the built-in
  channel's id and kind.
- Storage stays readable both ways: a board updated from master keeps its modes; see section 7.
- The existing on-board suites (`test_channels`, `test_dongle`, `test_rcp_push`, `test_device_api`, `test_br_boot`,
  `test_jumper`, `test_rest_guard` and `test_sniffer` with their RCP kill cases, `test_radio_firmware_e2e`,
  `test_zigbee`) and the page tests (`tools/tests`) are the regression net. A test changes only where section 9
  changes the behavior it checks.

## 4. The model

### 4.1 Channels

A channel is a connection to an RCP. It keeps today's label, line settings, firmware record, counters and transport.
There are two kinds:

| | BuiltinRcpChannel | UsbSerialChannel |
| --- | --- | --- |
| What | the board's own RCP, soldered on | a USB dongle |
| id, kind | `builtin-<chip>`: `builtin-c6` on this board; kind `builtin` | `usb-<vid>-<pid>[-<serial>]`; kind `usb` (unchanged) |
| Presence | always there, never `absent` | hot-plug; up to 7 remembered, one open at a time |
| Data path | its transport: spinel over SPI (OpenThread's SPI framing) on this board | bytes over a serial bridge (CDC-ACM, CP210x, CH34x); HDLC framing by the mux for frame modes, raw for the proxy |
| `reinit` work | its maintenance ops: boot steps, firmware push, ROM recovery | firmware detection after plug-in |
| Reset | the board's reset pin | close and reopen over USB |
| Line settings | fixed | baud, flow control |
| Fault kinds | firmware (below) | link (below) |

**A built-in RCP has three parts:**

- **A board descriptor:** the chip, the id, the label ("ESP32-C6 (built-in)"), and the pins (reset, boot, INT, the bus).
- **A transport:** spinel over SPI here. It is generic and works for any SPI spinel RCP. A UART transport could come
  later.
- **Maintenance ops** behind one interface: probe the installed image, push an image, ROM recovery, and what the board
  checks at boot.

This board has one implementation: the C6 on the ESP32-P4 board, with its checks for the factory ESP-Hosted image and
the jumper. `rcp_sync` stays its flashing engine. A board with an ESP32-H2 would bring its own descriptor and ops; the
Espressif ROM bootloader path is mostly shared. In the rest of this document, "the C6" means this board's built-in RCP.

Its lifecycle state, `channel_state`, is one of:

| `channel_state` | Meaning |
| --- | --- |
| `absent` | no device: an unplugged dongle. The C6 is never `absent` |
| `reinit` | the channel is not usable by a mode while maintenance runs on it. Always with a reason naming the work: `boot steps`, `detecting firmware`, `firmware push`, `ROM recovery` |
| `ready` | open and usable: a mode may be bound and run on it |
| `fault` | unusable, with a reason and a kind (below) |

A `fault` has one of two kinds, because the board shows them differently today (section 6):

- **firmware:** what the C6's maintenance found. That is no RCP firmware, the factory ESP-Hosted image, or a failed
  ROM recovery: today's `radio_mux_c6_present` reasons.
- **link:** the connection failed: `USB close failed`, three fruitless resets, no driver.

A C6 whose installed RCP does not answer is not a channel fault. It is `ready`, and Thread's start probe fails on it
with "rcp incompatible", as today.

- Every channel starts in `reinit`. The C6 runs its boot steps there. A dongle runs its firmware detection there,
  after plug-in. A channel reaches `ready` only once that work is done, so no mode starts before it.
- `reinit` is runtime only. It is never stored, and a reboot returns to the configured binding.
- A reset or reopen that a bound mode asks for (`radio_mux_reset`), and the mux's own recovery after a USB error, stay
  inside `ready`: the mode gets RESET (or GONE when the device did not come back) as today. They are not maintenance.
- The channel layer never decides what runs on it. It reports `channel_state` changes to the orchestrator, and carries
  frames and events to whichever mode the orchestrator bound to it.

### 4.2 Mode instances

A mode instance is a service with its own state machine, bound to at most one channel:

| Instance | How many | Notes |
| --- | --- | --- |
| Thread | one (singleton) | today's `mode_fsm` in br_core is its state machine; the board mode is derived from it (section 6) |
| Zigbee | one (singleton) | today's ziggurat consumer in the `zigbee` component |
| RCP proxy | one per channel | the user rejected a single proxy that serves several channels. Its instance belongs to its channel: rcp-proxy on `builtin-c6` and on a dongle are two instances |
| off | none | a channel bound to nothing |

Every instance's state machine handles these inputs from the orchestrator:

- **bind(channel)**: the instance may start on the channel (it is `ready`). It answers with its own state changes.
- **unbind**: the configuration took the channel away (a mode change, a move). The instance stops and lets go.
- **reinit request(reason)**: maintenance wants the channel. The instance answers **accept** (it let go now),
  **delay** (it lets go once something finishes, then reports that it did, within a bound it states) or
  **refuse(reason)**. The decision belongs to the instance, not the orchestrator.
- **channel lost**: the channel went `absent` or `fault` while bound. The instance goes to its own "no radio" state
  with the channel's reason and waits. Thread is the exception for a `fault` of kind firmware: it goes to `fault`
  instead, which keeps today's `RADIO_FAULT` for those (section 6).
- **channel back**: the channel is `ready` again after `reinit`, `absent` or `fault`, with the binding unchanged. The
  instance starts again (a bind of the same channel).

An instance reports its state and reason to the orchestrator, which shows them in the read API. Each instance keeps its
own state names:

| Instance | States |
| --- | --- |
| Thread | `no_radio`, `starting`, `running`, `sniffer`, `releasing`, `fault` |
| Zigbee | today's `idle`, `starting`, `running`, `no_radio`, `incompatible` |
| RCP proxy | `off`, plus today's `listening`, `connecting`, `backoff`, `connected`, and `no_radio` |

How each instance answers a reinit request (decided here; today's behavior kept):

- **Thread**: accepts after its open sessions drain. That is a delay of up to 45 s (`SESSION_DRAIN_MS`), then
  `releasing` → `no_radio` with the reinit's reason. It refuses after the drain timeout ("OpenThread is busy, try
  again"), while it runs as a sniffer, and while it is starting. These are today's `mode_fsm_push_allowed` refusals.
  On channel back it starts OpenThread on the channel again. A firmware push or ROM recovery that changes the RCP makes
  it re-read `rcp_compatible`, as today.
- **Zigbee**: always accepts. It goes `no_radio` with the reason "radio reinit: <work>". Its client stays connected,
  and its radio settings are replayed on channel back. This uses its existing GONE/RESET handling (the Zigbee
  session's input).
- **RCP proxy**: refuses while a peer is connected (today's 409 `c6_busy`). Otherwise it accepts: its sessions close
  and its listener closes, and both reopen on channel back.

### 4.3 The orchestrator

The orchestrator owns the binding table: for each instance, the channel it is configured to run on (or none). It is the
only writer of that table, and the table changes only by a user action through the API.

It enforces the rules that are checked today in `radio_mux_check_set_mode`, now as binding rules:

- one instance per channel;
- one Thread binding and one Zigbee binding;
- Thread and Zigbee never both bound (`hybrid_not_supported`);
- the firmware fit (`radio_mux_fits`);
- the proxy's `port_taken`.

What it does, and nothing more:

- **Bind** an instance when its configured channel is `ready` and the instance is not running on it. This covers boot,
  plug-in, the end of `reinit` and a bind by the user.
- **Unbind** an instance when the user changes the configuration.
- **Run maintenance**: ask the bound instance to let go (reinit request), move the channel to `reinit` only once it
  accepted, run the work, then move the channel back to `ready` (or `fault`), which binds the instance again. A refusal
  goes back to the caller (409 with the instance's reason) and nothing runs.
- **Report**: each channel's lifecycle state and the bound instance's state and reason, for the API and the board
  mode.

It is policy-free:

- it never retries a refusal;
- it never re-binds an instance to another channel;
- it never clears a binding because of a fault.

A Thread fault is retried by the user's PUT of `otbr` on the same channel, as today.

The orchestrator runs on the mux owner task (my call, no new task). Instances run on their own tasks as today (Thread:
br_radio and the OpenThread task; Zigbee: its task; RCP proxy: the proxy task). The orchestrator talks to them through
non-blocking calls or posts, and they answer through posts. Neither side calls into the other while holding its own
lock.

### 4.4 Configuration and state

- Configuration is the binding table plus each channel's line and proxy settings. Only the API changes it.
- State is everything else: `channel_state` with its reason, and each instance's state with its reason.
- A failure (a start that fails, a probe that fails, an unplug, a refused reinit) changes state only. It never changes
  configuration.

### 4.5 Moves are rebinds

A PUT of `otbr` on channel B while Thread is bound to A (a move), while it is bound to nothing (a start), or a PUT of
another mode on A while Thread is bound to it (a stop):

1. Probe: Thread probes B through the channel while it keeps running on A. This uses the lease of today. If the probe
   fails, the PUT answers 409 `probe_failed` and nothing changed.
2. 202 `{"moving": true, "moved_from": ...}` as today.
3. Unbind from A: Thread drains its sessions (up to 45 s; a drain timeout refuses the move and leaves Thread running on
   A with `drain_timeout`, as today) and stops.
4. Commit: the binding table now says Thread → B, A → off (one atomic store, as today's two-blob commit).
5. Bind on B: Thread starts on B. If the start fails, Thread is `fault` on B with the reason, and the board is in
   `RADIO_FAULT`. **There is no rollback to A** (section 9). The user moves Thread back, or retries with a PUT of
   `otbr` on B.

A stop does steps 1 (accepted, no probe), 3 and 4: A → the requested mode, Thread → none, board `NO_RADIO`.

**An abandoned start** (`MODE_FSM_ERR_START_ABANDONED`: OpenThread's init stuck past its wait, an error or a park
inside `esp_openthread_init`, or the channel gone during the init). OpenThread's init cannot be abandoned from outside,
so no start can run again until a reboot.

- **Today:** a move's abandoned start reboots the board by itself, so it comes back on A (the stored modes had not
  changed yet). Every other path (boot, plug-in, the restart after a C6 push, the retry PUT) gives `RADIO_FAULT` "an
  earlier OpenThread start was abandoned: reboot", with no reboot.
- **Under the rebind:** the binding already says B, so a reboot comes back on B and gains nothing.
- **My call:** every path, the move included, ends the way the others do today: Thread `fault`, `RADIO_FAULT` with
  that text, no automatic reboot. No new persistent state, and no reboot loop on a radio that hangs every init.
- **Way out for the user:** a PUT of `otbr` on another channel. Its probe and commit work, and its start is refused
  with the same text. Then a reboot starts Thread on that channel.

### 4.6 Boot

Every channel starts in `reinit`:

- The C6 runs its boot steps there: the rcp_sync probe, the jumper detection, `rcp.installed`, the automatic ROM
  recovery.
- A present dongle runs its detection there.

When a channel becomes `ready`, the orchestrator binds its configured instance, so each instance is bound once per boot,
after its channel's work. This replaces the boot hold of #39 (`radio_mux_c6_hold_t`, `RADIO_MUX_C6_BOOT_HOLD_MS`) and
br_core's `s_booting` / `s_c6_boot_steps`.

The board mode at boot follows Thread's binding, with today's values (the full table is in section 6):

- `STARTING` while the C6 runs its boot steps with Thread bound to it;
- `NO_RADIO` while Thread's dongle is `absent` or detecting ("waiting for <id>"), or when Thread has no binding ("no
  channel in otbr");
- `RADIO_FAULT` with the reason when the C6 ends in a firmware `fault` ("no rcp firmware", ...) or Thread's start probe
  fails ("rcp incompatible");
- `NO_RADIO` with the reason when Thread's dongle ends in a link `fault`.

A boot job that never starts cannot happen any more, because the orchestrator runs the boot steps itself. A step that
hangs is bounded by the step's own timeouts (rcp_sync's), and the channel then ends in `fault`.

### 4.7 Zigbee under the orchestrator

- Zigbee is a singleton instance wrapping today's ziggurat consumer. It needs spinel firmware, and at most one channel
  carries it.
- Entering or leaving Zigbee reboots, as today, because ziggurat cannot be stopped once started. My call: this is a
  property each instance declares, "binding or unbinding me needs a reboot" (Zigbee: both true; Thread and the proxy:
  false). It is not an orchestrator rule. The orchestrator turns such a change into today's `APPLY_REBOOT`: it stores the
  new binding and the caller reboots. If a Ziggurat stop exists one day, Zigbee drops the property and nothing else
  changes. The Zigbee session asked for this decision; it is open to its review.
- A reinit while Zigbee is bound (a C6 push with Zigbee on the C6) is accepted as in 4.2. It needs no reboot: the
  channel is not unbound, only paused.

## 5. Scenarios: today's behavior and its new path

| Scenario | New path | Visible change |
| --- | --- | --- |
| Boot, C6 in otbr | C6 `reinit` (boot steps) → `ready` → bind Thread | none |
| Boot, C6 in rcp-proxy or ziggurat | C6 `reinit` → `ready` → bind the instance once | none (the #39 fix becomes the model) |
| Boot, Thread on an unplugged dongle | dongle `absent`, Thread bound and waiting | none (`NO_RADIO` "waiting for <id>") |
| Plug-in | `reinit` (detection) → `ready` → bind | none |
| Unplug of Thread's dongle | channel lost → Thread `no_radio`, board `NO_RADIO` | none |
| PUT mode on a channel | orchestrator checks the rules → unbind/bind (or `APPLY_REBOOT`) | none |
| Move, start, stop of Thread | section 4.5 | no rollback; an abandoned start no longer reboots |
| Firmware push / ROM recovery on the C6 | reinit request to the C6's instance → `reinit` → work → `ready` | `c6_busy` comes from the proxy instance's refusal; the reply texts stay |
| Push with Thread on a dongle | the C6's instance (proxy, Zigbee or none) is asked; Thread is not touched | none |
| Push with Zigbee on the C6 | Zigbee accepts, `no_radio` "radio reinit: firmware push", replays after | none (today: GONE then attach) |
| Proxy peer connected, push asked | proxy refuses → 409 `c6_busy` | none |
| Thread start fails on its channel | Thread `fault`, board `RADIO_FAULT`, binding kept; PUT otbr retries | none |
| Sniffer | inside Thread's state machine, as today | none |
| Fruitless resets (3) | channel link `fault` → instance gets channel lost with the reason (Thread: `no_radio`, board `NO_RADIO`) | none |
| C6 whose RCP does not answer (`c6_hold`) | C6 `ready`; Thread's start probe fails → Thread `fault` "rcp incompatible" | none (`RADIO_FAULT`) |
| C6 with no RCP firmware, ESP-Hosted, failed ROM recovery | C6 firmware `fault` → Thread `fault` | none (`RADIO_FAULT`) |

## 6. API

Writes keep today's shape (section 3).

Reads keep every existing field with today's meaning and add these (my call on the names):

- Per channel:
  - `channel_state` and `channel_reason` (section 4.1);
  - `mode_state` and `mode_reason`: the bound instance's state in its own names (section 4.2), `null` for `off`.
- Today's `state` and `error` stay, derived exactly as today from the two above, so the Radios page, the Dashboard and
  the tests keep working. The page may switch to the new fields later.
- At the top of `GET /device/channels`: `modes`, a read-only list of the instances. Each entry has `mode`, `channel`
  (the configured channel id or `null`), `state`, `reason` and `reboot_on_change`. Example:
  `{"mode": "otbr", "channel": "builtin-c6", "state": "running", "reason": null, "reboot_on_change": false}`. A proxy instance
  appears once per channel bound to rcp-proxy.
- `/device/status`'s board mode (`BR_RUNNING`, `NO_RADIO`, `RADIO_FAULT`, `STARTING`, `SNIFFER`) is derived from
  Thread's state and its channel, with today's values in every scenario of section 5:

  | Thread | Its channel | Board mode | Reason (today's texts) |
  | --- | --- | --- | --- |
  | `running` | `ready` | `BR_RUNNING` | |
  | `sniffer` | `ready` | `SNIFFER` | |
  | `starting` or `releasing` | any | `STARTING` | |
  | bound, waiting | `reinit` for `boot steps`, `firmware push` or `ROM recovery` (the C6) | `STARTING` | |
  | bound, waiting | `reinit` for `detecting firmware` (a dongle) | `NO_RADIO` | "waiting for <id>" |
  | bound, waiting | `absent` | `NO_RADIO` | "waiting for <id>" at boot, "radio unplugged (<id>)" after an unplug |
  | `no_radio` | `fault`, kind link | `NO_RADIO` | the channel's reason |
  | `fault` | `fault`, kind firmware | `RADIO_FAULT` | the channel's reason ("no rcp firmware", ...) |
  | `fault` | `ready` | `RADIO_FAULT` | the start's reason ("rcp incompatible", "an earlier OpenThread start was abandoned: reboot", ...) |
  | unbound | | `NO_RADIO` | "no channel in otbr" |

  `otbr_channel` stays the configured channel.

## 7. Storage

The binding table needs no new record (my call): it is the union of the per-channel stored modes that exist today
(namespace and keys of `mux_nvs.c`). For each channel, the stored mode names the instance bound to it, and the
singletons are found by scanning. So:

- A board updated from master reads its configuration unchanged, and a downgrade reads it unchanged too.
- The built-in channel keeps its NVS key `c6` (today `radio_mux_nvs_key` special-cases the C6's id to that key). Only
  the API id changes to `builtin-c6`, so its stored mode and settings carry over.
- A rebind stores both channels in one NVS commit (today's two-blob commit, W-2).
- The load rule for duplicates stays: two channels stored `otbr` keep the first in slot order, and the rest become
  `off`. The same rule applies to two `ziggurat`.
- There is no new persistent state.

## 8. Code layout and threading

What goes where. Names of new files and types are the builder's call within these lines:

- **radio_mux (channels):** the transports, the frame path, detection, the channel registry, `channel_state`, the
  lease. The attach machine is reduced to the lifecycle (absent, reinit work, ready, fault). "Which consumer to
  attach" leaves it. Gone: the C6 boot hold and the C6 job API as it is today (`radio_mux_c6_job_begin/_end`); the
  orchestrator's maintenance path replaces them.
- **The orchestrator:** a new module on the mux owner task. It holds:
  - the binding table and the binding rules (moved from `radio_mux_check_set_mode`);
  - the reinit protocol;
  - the maintenance runner;
  - the snapshot for the API.
  Its decisions are pure functions with host tests, in the style of `radio_mux` `core.c`.
- **C6 maintenance:** the boot steps, the push and the ROM recovery move out of br_core (`core_c6_boot_steps`,
  `run_radio_job`, `run_c6_job_alone`, `push_busy`) into a C6 maintenance piece the orchestrator runs. `rcp_sync` stays
  the flashing engine. `rcp.installed` and the firmware status move with it. `rcp_compatible` (Thread's verdict on the
  spinel version) stays in Thread.
- **Thread (br_core):** `mode_fsm` becomes the instance's state machine with the inputs of section 4.2. br_radio's
  reconcile and its flags go away; bind, unbind, reinit request, channel lost and channel back replace them. The
  move's probe, drain and start stay, but `start_back` is removed.
- **Zigbee (zigbee component):** its consumer becomes the instance. It gets the reinit request (always accept) and
  declares the reboot property.
- **RCP proxy:** an instance per channel. Its own attach reconcile (`s_want`) is replaced by bind and unbind from the
  orchestrator. Its reinit answer is the peer check.
- **device_api:** the new read fields; the PUT path calls the orchestrator instead of `radio_mux_set`'s mode branch.
  Line and proxy settings keep their path.

## 9. Changes users will see

1. **No rollback after a move.** A target that passes the probe but does not start leaves Thread in `fault` on the
   target, and the board in `RADIO_FAULT`. Today Thread starts again on the old radio. The docs (`radios.md`, "Moving
   Thread", steps 4 to 6) and the Radios page texts that promise the rollback change. Tests that check a rollback change
   to check the fault on the target.
2. **An abandoned start during a move no longer reboots the board.** Today it reboots back onto the old radio. Now it
   gives `RADIO_FAULT` "an earlier OpenThread start was abandoned: reboot", as every other path already does (section
   4.5). `radios.md` step 6 changes.
3. **The stored mode changes before the target runs.** Today `stored_mode` changes only once Thread runs on the target.
   Now it changes at the commit, step 4.
4. **New read fields** (section 6). They are additive.
5. **Zigbee during a C6 push** shows `no_radio` "radio reinit: firmware push" instead of a GONE and a new attach.
6. **The built-in channel's id and kind:** `c6` becomes `builtin-c6`, and kind `c6` becomes `builtin`.
   - It shows wherever a channel id shows: `GET/PUT /device/channels/{id}`, `/device/status` `otbr_channel`,
     `/device/zigbee` `channel`, the pages and the logs.
   - The old id is not accepted as an alias (my call): the pages and the tests in this repo are its only known
     clients, and they change with it.
   - In the repo: about 25 files use the id or kind: three pages (`firmware.js`, `status.js`, `zigbee.js`),
     `device_api`'s channel JSON, the host tests, `test_channels`, `test_dongle`, `test_zigbee`, the page tests, and
     `docs/api.md`.
   - The firmware record's `chip` (`esp32c6`) already names the chip.

## 10. Testing

Host:

- **Orchestrator:**
  - the binding rules (every refusal of today's `radio_mux_check_set_mode`, moved);
  - bind on ready;
  - unbind on a change;
  - the reinit protocol: accept, delay with its bound, refuse with its reason, and a delay that never reports back;
  - the derived board mode table;
  - the derived `state`/`error` of today for every scenario in section 5;
  - the storage load rules.
- **Each instance's reinit and bind decisions:** Thread (in `mode_fsm`'s host tests), the proxy (peer check), Zigbee
  (always accept, reboot property).

On the board, following the standing rule:

- Tasks run host tests and builds only. They never flash the board and never run on-board tests.
- There is one batched on-board pass, after the whole-branch review and its fixes.
- The stages of the plan are internal to this one branch and PR. Each stage keeps the build and the host tests green.

The pass, with a written flash budget:

- **Baseline:** one P4 app flash of a master dd5da30 build: the idle internal heap with no dongle.
- **The branch:** one P4 app flash.
- **C6 flash writes:** only for `test_rcp_push` and `test_radio_firmware_e2e`.
- **The regression suites of section 3.** This design asks for them because the refactor touches every mode. Section
  9's items change their expectations.
- **Targeted checks:**
  - boot with each mode on the C6: each instance bound once, and no resets beyond the instance's own;
  - a push with Zigbee on the C6;
  - a push refused while a proxy peer is connected;
  - a failed move: Thread `fault` on the target, no rollback;
  - an abandoned start during a move: `RADIO_FAULT`, no reboot, with a test-build hook that makes the start hang;
  - the board-mode table of section 6, row by row, where the rig can reach it (`c6_hold`, `rcp_probe_override`, an
    unplug, a link fault via `usb_stall`);
  - NVS compatibility: the branch boots on the records master wrote during the baseline;
  - heap against the baseline.

## 11. Risks

- **Size.** This touches four components and their tests. The plan should land it in stages inside the branch that
  each keep the build and the host tests green. For example:
  1. the orchestrator and the rules, with the attach machine driven through it;
  2. reinit and the C6 maintenance;
  3. Thread's flags;
  4. the read API.
  A regression on the board shows up only in the final pass, so each stage's host tests must cover the scenarios of
  section 5 that it touches.
- **Thread's state machine.** `mode_fsm` carries years of edge cases: the drain, sniffer, start abandoned, the REST
  guard. The new inputs must map onto it without losing any of them. The host tests of `mode_fsm` stay, and every
  existing case must pass.
- **Ordering between the owner task and br_radio.** Today's generation checks guard against a late detach. The bind
  and unbind messages need the same guard: an instance must ignore a bind or unbind for a binding it no longer has.
- **Radios page and docs texts** about the rollback (section 9).

## 12. Left to the builder

- File, type and function names.
- How instances receive orchestrator inputs (callbacks that post to their task, or a queue per instance), as long as
  nothing blocks the owner task.
- The staging of the plan.
- Log texts.
- Whether `delay` carries its own deadline or the orchestrator applies one per reason.
- How the Radios page shows `channel_state` / `mode_state` (optional in this work).

## 13. Calls made in this design (for review)

1. The binding table is stored as today's per-channel modes, with no new NVS record and no migration (section 7).
2. The orchestrator runs on the mux owner task, with no new task, and a 4 KB internal heap budget against master
   (section 3).
3. The read API adds `channel_state`, `channel_reason`, `mode_state`, `mode_reason` and a top-level `modes` list, and
   keeps `state`/`error` as derived fields (section 6).
4. Every abandoned start ends in `RADIO_FAULT` without an automatic reboot. This includes a move's, which today
   reboots back onto the old radio (section 4.5).
5. Channel faults have two kinds, firmware and link, so the board mode keeps today's values: `RADIO_FAULT` for the
   C6's firmware faults, `NO_RADIO` for a dongle's link faults. The C6 is never `absent` (sections 4.1 and 6).
6. Zigbee's reboot on entering or leaving is an instance property, not an orchestrator rule (section 4.7). Open to the
   Zigbee session's review.
7. The instances' reinit answers keep today's refusals: Thread's drain and sniffer, and the proxy's connected peer
   (section 4.2).
8. A reset that a mode asks for, and the USB error recovery, are not `reinit` (section 4.1).
9. The built-in RCP is a channel kind of its own, built from a board descriptor, a transport and maintenance ops,
   with only the C6 over SPI implemented (section 4.1).
10. The built-in channel's kind becomes `builtin` along with its id `builtin-c6`, and `c6` is not kept as an alias
    (section 9).
