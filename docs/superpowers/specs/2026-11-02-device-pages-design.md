# Device pages redesign: design

Approved sketches: https://claude.ai/artifact/YM6s8R1bUGZmnGWcxwQiu2 (canvas "Device pages redesign": Status, Ethernet in three
states, Firmware, Packet capture, Diagnostics).

## 1. Goal

`device.html` today holds six unrelated sections on one page. Split it up: the device status becomes the Dashboard
(`index.html`, merged with the upstream Dashboard; amendment of 2026-11-02, `device.html` is removed), four device pages
take the rest, and the network form is replaced with a setup-first editor that shows what will change before the board
reboots.

Frontend only: no firmware or API change. The device pages use only `/device/*`, so they work in `RADIO_FAULT` and while
sniffing. The Dashboard also reads the Thread web GUI's `/get_properties` and `/ipaddr`, but only while the status says
`BR_RUNNING`; in the other modes its Thread cards say they show while the border router runs.

## 2. Pages and navigation

| Page | File | Script | Endpoints |
| --- | --- | --- | --- |
| Dashboard (status) | `index.html` | `static/status.js` | `/device/status`, `/device/diag`, `/device/reboot`, `/get_properties`, `/ipaddr` |
| Ethernet | `ethernet.html` | `static/ethernet.js` | `/device/network`, `/device/network/confirm` |
| Firmware | `firmware.html` | `static/firmware.js` | `/device/status`, `/device/ota`, `/device/ota/url`, `/device/radio/firmware`, `/device/radio/recover`, `/device/radio/log` |
| Packet capture | `capture.html` | `static/capture.js` | `/device/status`, `/device/sniffer.pcap`, `/device/sniffer` |
| Diagnostics | `diagnostics.html` | `static/diagnostics.js` | `/device/diag` |

- The Dashboard stays the landing page at `/`; `device.html` is removed and the docs point at the Dashboard.
- The nav entry "Device" in `static/api.js` becomes a dropdown like "Management", with the four device pages in the table
  order.
  It is highlighted on any of them.
- Shared code goes in `static/device_common.js`, loaded before the page script. It holds the pure helpers (error messages,
  backoff, last-seen text, boot detection, remaining time, whole-number parsing), the request wrapper, the poller (visibility
  and in-flight gating, backoff), the busy lock over the page's action buttons, `waitForBoard`, the offline line, and the
  trial banner.
- Each page has the trial banner (`/device/network` polled every 10 s for it): "These settings are on trial ... goes back
  to the previous settings in m:ss unless you keep them", with **Keep new settings** (`POST /device/network/confirm`).
  The button name replaces "Confirm" everywhere; the API is unchanged.
- New look in `static/style.css`: segmented control, chips, mode cards, tiles, bars, drop zone, sticky action bar. The
  palette, cards and fonts stay those of the current UI.

## 3. Dashboard (`index.html`, the status page)

Amendment: besides the content below, the Dashboard shows the Thread network (name, role, RLOC16, channel, TX power,
PAN ID, extended PAN ID, partition ID), identifiers (EUI-64, Border Agent ID, RLOC, OpenThread version, PSKc hidden
until Show) and the Thread address table, from `/get_properties` and `/ipaddr` in `BR_RUNNING` only.


- **Hero:** mode in words, with a colored top border and dot.
  - `BR_RUNNING`: "Border router running" (green).
  - `SNIFFER`: "Capturing packets", with channel and frames, and a link to Packet capture (blue).
  - `RADIO_FAULT`: "Radio fault", the text of `faultHint(fault_reason)`, and a link to Firmware (red).
  - `STARTING`: "Starting" (grey).
  - Beside it: raw mode, uptime and last reset (from `/device/diag`; a dash when diag fails).
- **Reboot** sits in the page header and asks for confirmation, as today.
- **Tiles**, each linking to its page:
  - Board firmware: version, slot, settings storage.
  - Radio: RCP version, match yes/no, base image installed vs expected, jumper.
  - Ethernet: IPv4 and IPv6 from `eth` in the status.
  - IPv6 prefix delegation: state and prefix, "none received" when null.
  - Packet capture: idle, active (channel, frames/dropped), or ended by a fault.
  - Load and memory: 10 s CPU per core, internal RAM free and lowest, PSRAM free.
- **Offline line:** when status fails, "last answer N s ago", as today.

## 4. Ethernet (`ethernet.html`)

**State.** The page reads `GET /device/network`:
- `mode`, plus `active` with `single|mgmt|thread_bb` = `{vlan, ipv4:{dhcp, addr, mask, gw, dns}}`;
- `trial` with `remaining_s`;
- `interfaces[]` (`name` = `eth`|`mgmt`|`thread_bb`, `vlan`, `link`, `ipv4`, `ipv6[]`);
- `vlan_rx_dropped`, `fallback` and `error`.

The editor is filled once from `active` and is not overwritten by later polls. Live interface state refreshes every 10 s.

**Setup cards.** "Single port" and "Split management and Thread" are buttons with `aria-pressed`, each with a small
cable diagram. The running mode carries "In use now". Picking a card changes the draft only.

**Interface cards.** One per interface of the draft mode (`single` → "Ethernet port"; `mgmt` → "Management";
`thread_bb` → "Thread backbone"). Each card has:
- the role text, a VLAN chip, and a link chip ("Link up", "Link down", or "Not in use yet" when the running mode doesn't have it);
- the live view when the running mode has the interface: IPv4 (live) with "from DHCP" or "static", plus the configured gateway and DNS when static, and the IPv6 addresses;
- otherwise the line "Not in use yet. After you apply: …";
- an **Edit/Done** toggle that opens the editor. Interfaces not in use yet open in edit mode.

**Editor.**
- VLAN: segmented Untagged/Tagged; Tagged shows "VLAN ID (1–4094)".
- IPv4: segmented Automatic (DHCP)/Static. Static shows "Address and prefix length" (`192.168.1.10/24`), "Gateway
  (optional)" and "DNS server (optional)".
- The fixed line: "IPv6 addresses come from your router's advertisements".
- Validation runs on every input and shows next to the interface. Errors that span both interfaces (two untagged, equal VLAN
  IDs) show under the cards.
- The prefix is converted to and from the API's `mask`.

**Action bar** (sticky, shown when the draft differs from `active` or has errors):
- the text "N changes not applied yet" or "Fix N problems before you review the changes";
- **Discard changes** refills the draft from `active`;
- **Review changes** is disabled while there are errors.

**Review.**
- A table with one row per change: Setting / Now / After apply. Mode, and VLAN and IPv4 per interface; an interface the
  running mode lacks shows "Not in use".
- Three steps: save as trial and reboot; open the page at the management address (static: `http://<addr>`; DHCP: "the
  address the DHCP server [on VLAN n] gives the board"); keep within 5 minutes or it goes back.
- When any interface of the draft is tagged, a checkbox: "The switch port ... carries VLAN a and VLAN b as tagged VLANs".
  **Apply and reboot** stays disabled until it is ticked.
- The `net reset` hint.
- **Back to editing** and **Apply and reboot**. Apply sends `PUT /device/network` with the body built from the draft (same
  shape as today), then shows "Rebooting into the new settings" and runs `waitForBoard`.
- Server errors (400 `invalid_config` with detail, 409) show in the review section.

**Extras.**
- "Frames dropped for an unknown VLAN since boot: N".
- The fallback banner, as today.
- A stale-state line when the poll fails.

## 5. Firmware (`firmware.html`)

- **Two cards:** Border router (ESP32-P4) and Radio (ESP32-C6).
  - P4 shows the installed version and "Slot n".
  - C6 shows the installed RCP version (`rcpInstalledText`) and "Matches this build" yes/no.
- **Each card's source picker:** a segmented "From a file / From a URL".
  - File: a drop zone (a label wrapping the file input; drop also sets it) naming `otbr.bundle` or `c6_rcp.bin` and the size limit (10 MB / 1.5 MB).
  - URL: an input that accepts http only.
  - Each card has one action button ("Install and reboot" / "Install radio firmware") that uses the chosen source.
- **Effect lines:** "The board writes the bundle to the other slot and reboots into it." / "The Thread network stops for about
  half a minute while the radio is rewritten. Not possible during a packet capture."
- **Fault highlight:** when `faultHint` highlights (no RCP firmware, incompatible), the radio card gets the hint banner and the outline.
- **Radio recovery card:**
  - A chip says "Jumper fitted" or "Jumper not fitted".
  - Without the jumper: an explanation and a link to the jumper doc (on GitHub, as a plain `<a>`).
  - With the jumper: **Recover C6**, **Show C6 console** and the log box.
- Uploads keep today's behavior: size check before sending, busy lock, then `waitForBoard`, plus the messages.
- No byte-level progress bar: today's fetch upload has no progress events (see 9).

## 6. Packet capture (`capture.html`)

- **Idle:**
  - The trial note "The Thread network stops while a capture runs".
  - Channel: an input, empty by default = "the Thread network's channel". The API picks it when no channel is sent and answers 503 when it can't read it.
  - Length: segmented 30 s, 1 min, 10 min, 1 h.
  - **Start capture:** an `<a download>` to the capture URL, as today.
- **Active** (from `/device/status` `sniffer`, polled every 3 s): channel, time left, frames, dropped, a progress bar, and **Stop capture** (`DELETE /device/sniffer`).
- **Ended by a fault:** a line saying so.
- **Watch live in Wireshark:** a `curl -N 'http://<location.host>/device/sniffer.pcap?...' | wireshark -k -i -` command built from the current fields.
- `captureUrl` accepts an empty channel (no `channel` parameter).

## 7. Diagnostics (`diagnostics.html`)

- **CPU card:** per core, "1 s / 10 s / 60 s" plus a bar at the 10 s value; uptime and last reset.
- **Memory card:** internal and PSRAM, each with free, lowest since boot, and largest block. There is no bar, because `/device/diag` reports no total (deviation from the sketch).
- **OpenThread lock:** a table with rows since boot and last 60 s; columns acquisitions, longest hold, held by, longest wait.
- **Tasks:** a table sorted by 10 s CPU, descending. Stack free under 1024 B is shown in red and bold.
- A link to `/metrics`. Polls every 2 s while visible.

## 8. Tests

- `tools/tests/device_page_harness.js` takes the page name, loads `device_common.js` and that page's script, and fakes the
  DOM, fetch and clock as today.
- The existing device page tests move into per-page files under `tools/tests/`, all in `make test-tools`:
  - `test_device_pages.py`: files, nav, routing, budget, no markup insertion, ids exist, live regions;
  - `test_device_common.py`;
  - `test_status_page.py`;
  - `test_ethernet_page.py`;
  - `test_firmware_page.py`;
  - `test_capture_page.py`;
  - `test_diagnostics_page.py`.
- Behaviour that survives keeps its test; behaviour that changes gets its test changed in the same step.
- `p4/pytest/test_device_page.py` serves the Dashboard, the four device pages and their scripts. It runs on the board only in the next batched hardware pass.

## 9. Out of scope

- No firmware or API change.
- Live gateway and DNS for DHCP interfaces: the API does not report them.
- Upload progress bars: these would need XHR.
- "Your network uses channel N" on Packet capture: it would need `/node`, which is not available in every mode.
- New screenshots (`docs/images/device.png`, `diagnostics.png`): taken in the next hardware pass.
