# Zigbee test device

Firmware for the rig's Zigbee test device: an on/off light **router** for the aux ESP32-C6 devkit (Waveshare
ESP32-C6-DEV-KIT-N8, 8 MB flash). The board's Zigbee coordinator (ziggurat on the P4) is tested end to end with zigpy; this
device is the real Zigbee node that joins on command. `p4/pytest/zigbee_device.py` drives it.

Derived from the on/off light of esp-zigbee-sdk (`examples/home_automation_devices/on_off_light`, v2.1.0). The example is a
coordinator, so this one steers into a network instead of forming one, and its external `light_driver` and `example_common`
packages are replaced by `main/light.c` and a retry timer in `main/zigbee_node.c`.

## Build

```sh
make zigbee-test-device
```

ESP-IDF v6.1 (`IDF_VERSION`) builds it; no Docker image is needed. Components (pinned in `main/idf_component.yml`,
locked in `dependencies.lock`): `espressif/esp-zigbee-lib` 2.1.0 and `espressif/led_strip` 3.1.1. esp-zigbee-lib 2.x carries the
ZBOSS stack in its own libraries, so there is no `espressif/esp-zboss-lib` dependency (that component belongs to the 1.x line).

## Behaviour

- The light is the devkit's on-board WS2812B (GPIO8, net `RGB_CTRL`): On/Off cluster on is white, off is dark.
- Console: UART0 at 115200 baud, which is the CH343 bridge of the devkit (by-id `usb-1a86_USB_Single_Serial_*-if00`).
- Console lines the tests read: `light: on` / `light: off` at boot and on every change of the On/Off attribute,
  `joined: yes` / `joined: no` on every change of the join state.
- Commands: `state` prints `app: <project name> <version>`, `light: ...` and `joined: ...`; `factory_reset` leaves the
  network, clears the Zigbee data and starts steering again.
- The device scans channels 11..26 and retries steering every second until a network with permit-join open admits it.

## Flash

Only the helper flashes it (and only when the app on the board is not this project's); to do it by hand, from the build
directory: `esptool --chip esp32c6 -p <port> write-flash @flash_args`. The aux C6 is shared with other projects: check that
nothing else is using it first.
