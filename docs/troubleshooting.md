# Troubleshooting and limitations

## Fault reasons

`GET /device/status` (`make status BOARD=<board>`) shows `mode` and `fault_reason`. In `RADIO_FAULT` the Thread API
answers 503; `/device/*`, the device page and OTA keep working.

| `fault_reason` | Meaning and fix |
| --- | --- |
| `no rcp firmware` | The C6 has no valid RCP image (new board, erased slots). Upload `c6_rcp.bin`. Not retried automatically. |
| `rcp incompatible` | OpenThread rejected the RCP, or it did not answer its reset. Upload the `c6_rcp.bin` of the same release. |
| `storage unavailable` | NVS did not initialise (for example no free pages), `storage_ok` is `false`. The board booted degraded instead of boot-looping: HTTP, OTA and `/device/*` work, OpenThread does not start. NVS is never erased automatically because it holds the Thread credentials. Update to a firmware that fixes it, or erase NVS by hand (`esptool --chip esp32p4 -p /dev/ttyACM0 erase-region 0x9000 0x10000`; the Thread network has to be set up again) and reboot. |
| `rom recovery failed` | The updater was silent and the jumper recovery failed. Check the jumper wires and `GET /device/radio/log`; try `make recover`; without a jumper use the no-jumper path of [First install](first-install.md). |
| `resetting the RCP failed`, `radio boot timed out`, `radio restart failed`, `OpenThread main loop ended` | Radio did not come up. The board retries (at most three automatic recoveries per 10 minutes), then waits: reboot, or upload firmware. Check the antenna and the C6 console log. |

## Other problems

- Board unreachable after a network change: wait 5 minutes for the revert, or use the console `net reset` (`make monitor`).
- No IP address: check the cable and the switch port (VLAN setup), `net show` on the console.
- `GET /api/node` / `/api/devices` show `state` `disabled` (name `OpenThread`, rloc16 `0xfffe`) although `GET /node` says
  `leader`: upstream esp-thread-br builds that collection entry once and never refreshes it (see
  `p4/components/PATCHES.md`, "Known upstream behaviour"). Use `/node`, `/node/state` or `/node/dataset/active` (what Home
  Assistant uses) for the live state.
- Thread REST answers 503 `radio not available`: see `mode` above; 409 `sniffer active`: a capture is running
  (`DELETE /device/sniffer`).
- The board boots in `RADIO_FAULT` after an on-board test: see [Development](development.md#testing) (`c6_hold 0`).
- Recovery from a bad P4 flash: reflash over USB (`make erase flash`). See [First install](first-install.md#recovery-procedures)
  for the C6.

## Known limitations and non-goals

- No authentication or TLS anywhere: anybody on the network can reconfigure the board, read the Thread dataset with
  `GET /node/dataset/active` (also from the `mgmt` VLAN: read-only `GET /node/*` is served there by design) or update its
  firmware. Put it on a trusted network. `https://` URLs are not supported for downloads.
- DHCPv6-PD makes Thread devices globally addressable; rely on the upstream router's firewall.
- Requests for the mesh topology (`/topology`, `/api/diagnostics`) add roughly 100 ms to the latency of pings through the
  border router while they run (Diagnostic Get traffic).
- Flash writes (OTA) use XIP from PSRAM so traffic keeps flowing, but the P4 reboots after an update.
- The `-dirty` flag of the app version stamp is as of the last configure (see [Building](building.md#versions)); a new commit
  refreshes the stamp.
- Untested on hardware: the no-jumper first install ([First install](first-install.md), Path 2, esptool over H7).
- Untested on hardware: recovery of a truly blank C6 (bootloader, partition table and otadata erased) and overwriting the
  factory ESP-Hosted firmware.
- A mDNS reflector between the two VLANs is tolerated, but a host that claims the board's mDNS name with different addresses
  can rename it (standard mDNS behaviour).
- Radio firmware updates interrupt the border router for a few seconds; seamless updates are not implemented.
- Not verified on hardware: the `br_core` give-up path after repeated radio failures, and the fall-back to default network
  config when the stored config cannot be applied (host-tested only).
