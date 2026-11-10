# Logging

The firmware has one log stream. Every part of it (the ESP-IDF components, OpenThread and the Ziggurat Zigbee stack)
writes into it, and it goes out on four channels, each with its own level: the **UART**, **USB-JTAG**, **syslog** over
UDP and a **live log** in the browser. The part being chased can be turned up on its own without flooding the rest. The
board keeps no log history: for the full history, use a syslog server.

Settings are stored and survive a reboot. They are on the **Device → Logs** page (`/logs.html`), in the API
(`GET`/`PUT /device/logging`, see [HTTP API and web UI](api.md#logging)) and in the console command `log`.

## Levels and the AND rule

Levels, from quiet to chatty: `off`, `error`, `warning`, `info`, `debug`, `verbose`.

A line goes out on a channel only if it passes **both** its component's level and that channel's level. A component is a
log tag (`wifi`, `esp_netif`, ...) plus `OPENTHREAD` and `ziggurat`; every tag without an override of its own follows
the default (`*`). Example: `ziggurat` at `debug` and syslog at `warning` sends ziggurat's warnings and errors to
syslog, while a live viewer at `debug` sees all of ziggurat's debug lines.

| Setting | Default on a fresh board |
| --- | --- |
| UART level | `info` (the output is what it was before this feature) |
| USB-JTAG level | `info` |
| Syslog level and target | `off`, no target |
| Components | `*` = `info`, no overrides |

- At most 32 component overrides, tags shorter than 24 bytes. The board remembers the tags it has seen since boot (at
  most 64) so the page can suggest them.
- The live log has no setting of its own: each viewer picks its own level (default `info`).
- OpenThread's own log level follows the `OPENTHREAD` component, so OpenThread does not format lines nobody will see.
  The same goes for `ziggurat`.
- The firmware keeps `debug` and `verbose` calls, but nothing at those levels is produced unless a channel and a
  component both ask for it.

## Channels

### UART

The CH343 console on header pins 8/10 (GPIO37/38), as before, written synchronously: the lines before a crash still
appear. The UART gets every line that passes its level, complete.

### USB-JTAG

The P4's USB-Serial-JTAG on header pins 27/28 (GPIO24/25, the full-speed USB PHY). It needs a USB breakout on the 40-pin
header; the USB dongles use the other (high-speed) port, so the two do not conflict. The secondary console on this port
is off: the console stays on the UART, and this channel only carries log lines. With no host reading, its lines are
dropped and counted, never waited for (`jtag.host` in `GET /device/logging` says whether a host reads).

### Syslog

UDP, one target `host[:port]` (port 514 when omitted): an IPv4 address, `[IPv6]` or `[IPv6]:port`, or a hostname. The
level is `off` by default and cannot be raised while the target is empty.

- Format: RFC 5424, one line per datagram, facility `local0`, severity error 3, warning 4, info 6, debug and verbose 7.
  HOSTNAME is the board's hostname, APP-NAME is the tag (filter by component on it), the timestamp is empty (`-`) until
  the clock is set and UTC with milliseconds after, and the message starts with the board's uptime in milliseconds:

  ```
  <132>1 - esp-otbr-d2cafc zigbee - - - 813120 request timeout
  ```

- A hostname is looked up again every 10 minutes and after every apply. A failed lookup is retried after 5 s, then 10,
  20, 40 and 60 s, and the state is `unresolved` meanwhile. While DNS keeps failing, the JTAG and syslog output can
  stall for the length of each lookup; the UART keeps its lines.
- State in `GET /device/logging`: `off`, `resolving`, `sending`, `unresolved`. A send error drops the line and counts
  it; it never logs (that would send itself again).
- Lines logged by the network stack while it sends a line stay on the UART and are not sent again.

To receive it with rsyslog, in `/etc/rsyslog.d/esp-otbr.conf` (open UDP port 514 in the host's firewall):

```
module(load="imudp")
input(type="imudp" port="514")
if $fromhost-ip == '192.168.1.50' then {
    action(type="omfile" file="/var/log/esp-otbr/board.log")
    stop
}
```

Then `log syslog <host>` and `log level syslog info` on the console, or set both on the Logs page. `$programname` is the
tag, so `$programname == 'OPENTHREAD'` picks OpenThread's lines.

### Live log

Text frames over `ws://<board>/device/logging/ws`, one line per frame, only lines logged after the connection:

```
I (813120) radio_mux: channel up
```

- The page starts **disconnected**: it costs the board nothing until you press **Connect**, and **Disconnect** closes
  the socket again. Received lines stay until **Clear**. It also has the viewer's level, Pause (the lines keep arriving
  and are kept, at most 5000, the oldest dropped), Copy all, a filter and Fullscreen (Esc leaves).
- With `websocat`:

  ```
  websocat 'ws://<board>/device/logging/ws?level=debug'
  ```

  The level is the query parameter `level` (default `info`). A text frame `{"level":"warning"}` changes it for this
  viewer only; anything else is answered with a `# ignored` notice.
- Board notices start with `# `. `# dropped <n> lines` says that this viewer lost lines (its queue was full, or the
  transit ring was); it is shown whatever the viewer's level.
- At most **2 viewers** at once: a third gets 503 `too_many_viewers` ("too many viewers") before the upgrade. The board
  pings every 10 s and closes (1001, `no answer`) a viewer that has not answered for 30 s. A slow viewer is never closed
  for being slow: it loses lines and is told so.
- Viewers never get `debug` or `verbose` lines of the `httpd*` tags (the server's own dispatch would feed itself). Syslog,
  UART and JTAG still do.
- No viewer, no work: lines are neither formatted nor kept for the live log while nobody is connected.
- The API answers 403 on an interface that does not serve the device API (see [Networking](networking.md)).

## Limits

- Lines logged in a context that cannot take part (an interrupt, with the flash cache off, with the scheduler suspended
  or not running) and before logging has started go to the UART only.
- A full transit ring drops lines and counts them per channel; the UART is not affected.
- A line is cut at 256 bytes for OpenThread and 512 bytes for the rest, marked `…[cut]`.
- No logs from the C6 radio or the USB dongles.
- Counters are in `GET /device/logging` (`counters`, per channel `sent` and `dropped`, plus `live`) and as
  [metrics](metrics.md#logging-metrics). They restart at 0 on reboot.

## Console command

The UART console has `log`, for when the network is down:

| Command | Effect |
| --- | --- |
| `log` | the channels with levels and counters, the syslog target and state, the default and the overrides |
| `log level <uart\|jtag\|syslog> <level>` | a channel's level |
| `log tag <tag\|*> <level\|default>` | a component's level; `*` is the default for every other tag; `default` removes the override |
| `log syslog <host[:port]\|off>` | the syslog target (`off` clears it); the level is separate |

Changes are applied and stored like `PUT /device/logging`. Bad input prints a usage line and changes nothing.

## Tests

`p4/pytest/test_logging.py` (on a board; the syslog test needs the test host reachable from the board over UDP, the
`403` test the VLAN rig) and the host tests in `p4/components/logging/test_host/`.
