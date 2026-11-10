# Logging

The board has one log stream. Every part of the firmware (the ESP-IDF components, OpenThread and the
[Ziggurat](glossary.md) Zigbee stack) writes into it, and it goes out on four channels, each with its own level: the
**UART**, **USB-JTAG**, **[syslog](glossary.md)** over UDP and a **live log** in the browser. Turn up the part you are
chasing on its own, without flooding the rest. The board keeps no log history: for the full history, send it to a
syslog server.

Settings are stored and survive a reboot. Change them on the **Device → Logs** page, with the console command `log`
(for when the network is down), or over the API (`GET`/`PUT /device/logging`, see [HTTP API](api.md#logging)).

## Levels

Levels, from quiet to chatty: `off`, `error`, `warning`, `info`, `debug`, `verbose`.

A line goes out on a channel only if it passes **both** its component's level and that channel's level. A component is a
log tag (`wifi`, `esp_netif`, ...) plus `OPENTHREAD` and `ziggurat`; every tag without a level of its own follows the
default (`*`). Example: `ziggurat` at `debug` and syslog at `warning` sends ziggurat's warnings and errors to syslog,
while a live viewer at `debug` sees all of ziggurat's debug lines.

| Setting | On a fresh board |
| --- | --- |
| UART level | `info` (the output is what it was before logging had settings) |
| USB-JTAG level | `info` |
| Syslog level and target | `off`, no target |
| Components | `*` = `info`, nothing else |

- Up to 32 components can have their own level. A tag is up to 23 characters, without spaces or quotes. The Logs page
  suggests the tags the board has seen since it started.
- The live log has no setting of its own: each viewer picks its own level (`info` unless it asks for another).
- `debug` and `verbose` exist only in a test build (`make build-test`). A release build compiles log calls in up to
  `info`, so a level above that is refused (the Logs page doesn't offer it, `GET /device/logging` reports `max_level`).
  Levels a test build stored are lowered to `info` when a release build starts.

## Channels

### UART

The console on the board's USB-UART bridge (header pins 8/10), as before. It gets every line that passes its level,
complete, and writes it at once, so the lines before a crash still appear.

### USB-JTAG

The P4's own USB port on header pins 27/28. It needs a USB breakout on the 40-pin header; the USB dongles use the other
USB port, so the two don't conflict. It carries log lines only: the console stays on the UART. While no computer is
connected, the board doesn't prepare lines for it at all; the Logs page and `jtag.host` in the API say whether one is.

### Syslog

UDP to one target `host[:port]` (port 514 when you leave it out): an IPv4 address, `[IPv6]`, `[IPv6]:port` or a
hostname. The level is `off` by default and can't be raised until a target is set.

- Each line is one RFC 5424 message, facility `local0`. The APP-NAME is the tag, so you can filter by component. The
  timestamp is empty (`-`) until the board's clock is set, and the message starts with the board's uptime in
  milliseconds:

  ```
  <132>1 - esp-otbr-d2cafc zigbee - - - 813120 request timeout
  ```

- A hostname is looked up again every 10 minutes and whenever you save the settings. If the lookup fails, the board
  tries again after 5 s, then 10, 20, 40 and every 60 s.
- The Logs page shows the state: `sending`, `resolving` (looking the name up), `unresolved` (the name does not resolve)
  or `no socket` (the board has no network socket to spare; it tries again like a failed lookup).

To receive it with rsyslog, put this in `/etc/rsyslog.d/esp-otbr.conf` and open UDP port 514 in the host's firewall:

```
module(load="imudp")
input(type="imudp" port="514")
if $fromhost-ip == '192.168.1.50' then {
    action(type="omfile" file="/var/log/esp-otbr/board.log")
    stop
}
```

Then set the target and a level on the Logs page, or on the console with `log syslog <host>` and
`log level syslog info`. `$programname` is the tag, so `$programname == 'OPENTHREAD'` picks OpenThread's lines.

### Live log

The Logs page shows the board's log as it happens. It starts **disconnected** and costs the board nothing until you
press **Connect**; **Disconnect** stops it again. Received lines stay until **Clear**. Pick the viewer's level, Pause
(lines keep arriving and are kept, at most 5000), filter, Copy all, or go Fullscreen (Esc leaves).

Lines look like the UART's:

```
I (813120) radio_mux: channel up
```

- `# dropped <n> lines` means this viewer lost lines because they came faster than it read them. A slow viewer is never
  disconnected for being slow.
- At most **2 viewers** at once; a third is refused.
- Debug and verbose lines of the board's own network stack (the web server, Ethernet, lwIP) never reach a viewer or
  syslog: sending them would make more of them, without end. Use the UART or USB-JTAG for those.

From a terminal, with `websocat`:

```
websocat 'ws://<board>/device/logging/ws?level=debug'
```

(`debug` on a test build; a release build refuses a level above `info`.)

## Console command

The UART console has `log`, for when the network is down:

| Command | Effect |
| --- | --- |
| `log` | the channels with levels and counters, the syslog target and state, the default and the overrides |
| `log level <uart\|jtag\|syslog> <level>` | a channel's level |
| `log tag <tag\|*> <level\|default>` | a component's level; `*` is the default for every other tag; `default` removes the tag's own level |
| `log syslog <host[:port]\|off>` | the syslog target (`off` clears it and turns syslog off); the level is set on its own |

Changes are applied and stored like a save on the Logs page. Bad input prints a usage line and changes nothing.

## Limits

- A line is cut at 512 bytes (256 for OpenThread), marked `…[cut]`.
- When lines come faster than the board can send them, the extra ones are dropped and counted (the counters are on the
  Logs page and in the [metrics](metrics.md#logging-metrics)); the UART always gets its lines.
- Lines from the first moments of boot, and from the rare places that can't wait (interrupts, flash writes), go to the
  UART only.
- No logs from the C6 radio or the USB dongles.
- The Thread Console's `loglevel` command changes a level behind the board's back: the board's settings win again at the
  next save. Use the Logs page or `log tag` instead.

How it works inside, and why these limits exist: [Internals](reference/internals.md#logging).
