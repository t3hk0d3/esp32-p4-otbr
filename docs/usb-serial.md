# USB serial proxy (experimental)

!!! warning "Experimental"
    This feature is a proof of concept. It has been built and host-tested, but not yet run with a dongle on a board.
    Expect rough edges, and do not rely on it for a network you cannot afford to lose.

The board can serve a USB serial dongle on its USB-A port to one TCP client, byte for byte. This lets a Zigbee or
Thread radio dongle (an SLZB-06, a CP210x, CH34x or CDC-ACM stick) stay plugged into the board while ZHA,
Zigbee2MQTT or otbr-agent runs somewhere else. It is off by default, and the board needs no dongle to start it.

## Turn it on

```
curl -X PUT http://<board>/device/usb_serial -d '{"enabled":true}'
```

The proxy then listens on TCP port 6638 (settable with `port`). Baud rate (default 115200) and hardware flow control
(default off) are set the same way; see [HTTP API and web UI](api.md#usb-serial-proxy-experimental). One client at a
time: the first one keeps the port, and later connections are closed at once.

!!! danger "Anyone who can reach the port controls the radio"
    The port has no login. Whoever connects can send any bytes to the dongle: reset it, re-flash it, or form and
    join networks with it. Use it only on a network you trust.

    - In split mode, both Ethernet VLANs (`mgmt` and `thread_bb`) reach the port.
    - The Thread mesh never does: a connection arriving over the mesh is refused.

## Clients

ZHA: add the integration with the serial path `socket://<board>:6638`.

Zigbee2MQTT:

```yaml
serial:
  port: tcp://<board>:6638
  adapter: zstack # an SLZB-06 on its Zigbee firmware
```

otbr-agent, through socat (a Thread RCP dongle):

```
socat -d pty,raw,echo=0,link=/tmp/ttyRCP tcp:<board>:6638
otbr-agent ... 'spinel+hdlc+uart:///tmp/ttyRCP'
```

## SLZB-06 notes

- Put the SLZB-06 in USB mode, so that its USB port is the serial link to the radio.
- Use 115200 baud and no hardware flow control (the defaults).
- Back up the dongle's Zigbee network with `zigpy-znp` before forming a new one through the proxy: a new network
  replaces the stored one.

## Status

`GET /device/usb_serial` shows the dongle (vendor and product ID, driver, errors), the connected client, byte counters
and `refused_connections`. Add `"kick": true` to a `PUT` to drop the current client.
