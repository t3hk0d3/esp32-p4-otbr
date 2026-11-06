"""The rig's Zigbee test device: the aux C6 devkit running tools/zigbee_test_device (an on/off light router).

The device joins a Zigbee network on command, so zigpy tests (the board's coordinator, ziggurat) have a real node to
pair. It is driven over its console (UART0 behind the devkit's CH343 bridge): the firmware prints `light: on|off` at
boot and on every change of the On/Off attribute, `joined: yes|no` on every change of the join state, and answers
`state` with `app: <project> <version>`, `light: ...` and `joined: ...`.

    with ZigbeeTestDevice() as device:
        device.factory_reset()
        ...  # open permit-join on the coordinator
        device.wait_joined()
        assert device.light_state() == "off"

The aux C6 is shared with other projects. Opening it refuses while another process holds the port (`fuser -v`), and it
is flashed (esptool, from tools/zigbee_test_device/build) only when its app is not this project's. Build the firmware
first: `make zigbee-test-device`.

Opening the port is meant not to reset the C6 (to be confirmed on the board): the CH343 auto-download circuit uses DTR
and RTS for GPIO9 and EN, the tty open raises both, and the order used here is (DTR, RTS) = (1,1), (1,0), (0,0), which
never asserts EN alone. Only esptool resets it, when it flashes.
"""

import errno
import os
import re
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

import serial

PORT = os.environ.get(
    "ZIGBEE_DEVICE_PORT",
    "/dev/serial/by-id/usb-1a86_USB_Single_Serial_5B61094735-if00",
)
BUILD_DIR = (
    Path(__file__).resolve().parent.parent.parent / "tools/zigbee_test_device/build"
)
PROJECT_NAME = "zigbee_test_device"
BAUD = 115200
FLASH_BAUD = 460800
APP_DESC_FLASH_OFFSET = (
    0x10020  # the factory app at 0x10000: image header 24, first segment header 8
)
APP_DESC_LEN = 80  # esp_app_desc_t up to and including project_name
PROJECT_NAME_OFFSET = 48
PROJECT_NAME_LEN = 32
PROBE_TIMEOUT_S = 3
BOOT_TIMEOUT_S = 30
ANSWER_TIMEOUT_S = 5
LEAVE_TIMEOUT_S = 10
JOIN_TIMEOUT_S = 60
POLL_S = 1
ESPTOOL_TIMEOUT_S = 180
ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")
LIGHT = re.compile(r"light: (on|off)\s*$")
JOINED = re.compile(r"joined: (yes|no)\s*$")
APP = re.compile(r"app: (\S+) (\S+)\s*$")


class DeviceBusy(RuntimeError):
    """Another process holds the serial port of the shared aux C6."""


def check_port_free(port):
    """Raise DeviceBusy when any process has the port open (`fuser -v`, run on the resolved device node)."""
    node = os.path.realpath(port)
    result = subprocess.run(
        ["fuser", "-v", node], capture_output=True, text=True, check=False
    )
    if result.returncode == 0:
        raise DeviceBusy(
            f"{port} ({node}) is in use, the aux C6 is shared:\n"
            f"{(result.stdout + result.stderr).strip()}"
        )


def open_console(port):
    """Open the console without an EN pulse on the way to DTR and RTS low.

    The tty open raises both lines (1,1) and pyserial applies DTR before RTS, so RTS goes low before open() applies
    anything (DTR stays at its default), and DTR goes low after it: (1,1) -> (1,0) -> (0,0). Setting DTR low first would
    pass through (0,1), which asserts EN in the esptool auto-reset circuit."""
    console = serial.Serial()
    console.port = port
    console.baudrate = BAUD
    console.timeout = 0.1
    console.exclusive = True
    console.rts = False
    console.open()
    try:
        console.dtr = False
    except OSError as error:
        if (
            error.errno != errno.ENOTTY
        ):  # a pty (the tests of this module) has no modem lines
            raise
    return console


def run_esptool(port, *args, cwd=None, before="default-reset", after="hard-reset"):
    command = [
        sys.executable,
        "-m",
        "esptool",
        "--chip",
        "esp32c6",
        "-p",
        port,
        "-b",
        str(FLASH_BAUD),
        "--before",
        before,
        "--after",
        after,
        *args,
    ]
    result = subprocess.run(
        command,
        cwd=cwd,
        capture_output=True,
        text=True,
        timeout=ESPTOOL_TIMEOUT_S,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(f"esptool {args[0]} failed:\n{result.stdout}{result.stderr}")


def flashed_project_name(port):
    """Project name in the app description of the factory app on the board (esptool read-flash; resets the C6)."""
    with tempfile.TemporaryDirectory() as directory:
        target = Path(directory) / "app_desc.bin"
        run_esptool(
            port,
            "read-flash",
            hex(APP_DESC_FLASH_OFFSET),
            str(APP_DESC_LEN),
            str(target),
        )
        raw = target.read_bytes()
    name = raw[PROJECT_NAME_OFFSET : PROJECT_NAME_OFFSET + PROJECT_NAME_LEN]
    return name.split(b"\0", 1)[0].decode(errors="replace")


def flash_firmware(port, build_dir=BUILD_DIR):
    if not (build_dir / "flash_args").is_file():
        raise RuntimeError(f"no firmware in {build_dir}: run `make zigbee-test-device`")
    run_esptool(port, "write-flash", "@flash_args", cwd=build_dir)


class ZigbeeTestDevice:
    """Console driver of the Zigbee test device. Use as a context manager, or call open() and close()."""

    def __init__(self, port=PORT, build_dir=BUILD_DIR):
        self.port = port
        self.build_dir = build_dir
        self.serial = None
        self._lines = []
        self._changed = threading.Condition()
        self._running = False
        self._reader = None

    def __enter__(self):
        self.open()
        return self

    def __exit__(self, *exc):
        self.close()

    def open(self):
        """Open the console (after `fuser` shows the port free) and flash the firmware if it is not this project's."""
        check_port_free(self.port)
        self._start_console()
        if self._answers_as_this_project():
            return
        self._stop_console()
        if flashed_project_name(self.port) != PROJECT_NAME:
            flash_firmware(self.port, self.build_dir)
        self._start_console()
        if not self._answers_as_this_project(BOOT_TIMEOUT_S):
            self.close()
            raise TimeoutError(f"{self.port}: no `{PROJECT_NAME}` console after boot")

    def close(self):
        self._stop_console()

    def factory_reset(self):
        """Leave the network and start joining again; returns once the device has left (or was not joined)."""
        was_joined = self._query()["joined"]
        mark = self._mark()
        self._send("factory_reset")
        self._wait_line(re.compile(r"factory_reset: (done|failed)"), mark)
        if was_joined:
            self._wait_line(JOINED, mark, LEAVE_TIMEOUT_S, expect="no")

    def wait_joined(self, timeout=JOIN_TIMEOUT_S):
        """Block until the device is on a network (it retries steering every second while permit-join is open)."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self._query()["joined"]:
                return
            time.sleep(POLL_S)
        raise TimeoutError(f"Zigbee test device did not join within {timeout} s")

    def light_state(self):
        """'on' or 'off': the On/Off attribute as the device reports it (the `light:` lines)."""
        return self._query()["light"]

    def wait_light(self, expected, timeout=10):
        """Block until a `light: <expected>` line arrives (or the state already is `expected`)."""
        mark = self._mark()
        if self.light_state() == expected:
            return
        self._wait_line(LIGHT, mark, timeout, expect=expected)

    def lines(self):
        """Every console line since open, ANSI escapes removed."""
        with self._changed:
            return list(self._lines)

    # --- console plumbing ---

    def _start_console(self):
        self.serial = open_console(self.port)
        self._running = True
        self._reader = threading.Thread(target=self._read, daemon=True)
        self._reader.start()

    def _stop_console(self):
        self._running = False
        if self._reader is not None:
            self._reader.join(timeout=2)
            self._reader = None
        if self.serial is not None:
            self.serial.close()
            self.serial = None

    def _read(self):
        pending = b""
        while self._running:
            try:
                pending += self.serial.read(4096)
            except (serial.SerialException, OSError, TypeError):
                return
            *complete, pending = pending.split(b"\n")
            if complete:
                with self._changed:
                    for raw in complete:
                        text = ANSI.sub("", raw.decode(errors="replace")).strip()
                        if text:
                            self._lines.append(text)
                    self._changed.notify_all()

    def _send(self, command):
        self.serial.write(f"{command}\n".encode())

    def _mark(self):
        with self._changed:
            return len(self._lines)

    def _wait_line(self, pattern, since, timeout=ANSWER_TIMEOUT_S, expect=None):
        """The first line after index `since` that matches `pattern` (and whose first group is `expect`)."""
        deadline = time.monotonic() + timeout
        seen = since
        with self._changed:
            while True:
                while seen < len(self._lines):
                    found = pattern.search(self._lines[seen])
                    seen += 1
                    if found and (expect is None or found.group(1) == expect):
                        return found
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError(
                        f"no console line {pattern.pattern!r} within {timeout} s; "
                        f"last lines: {self._lines[-5:]}"
                    )
                self._changed.wait(remaining)

    def _query(self, timeout=ANSWER_TIMEOUT_S):
        """The device's answer to `state`: app, version, light ('on'/'off') and joined (bool)."""
        mark = self._mark()
        self._send("state")
        app = self._wait_line(APP, mark, timeout)
        mark = self._mark_of(APP, mark)
        joined = self._wait_line(JOINED, mark, timeout)
        light = self._last_light(mark)
        return {
            "app": app.group(1),
            "version": app.group(2),
            "light": light,
            "joined": joined.group(1) == "yes",
        }

    def _mark_of(self, pattern, since):
        """Index of the first line after `since` that matches `pattern`."""
        with self._changed:
            for index in range(since, len(self._lines)):
                if pattern.search(self._lines[index]):
                    return index
        return since

    def _last_light(self, since):
        with self._changed:
            for line in reversed(self._lines[since:]):
                found = LIGHT.search(line)
                if found:
                    return found.group(1)
        raise RuntimeError("the `state` answer has no `light:` line")

    def _answers_as_this_project(self, timeout=PROBE_TIMEOUT_S):
        """True when the console answers `state` with this project's name; tries again until `timeout`."""
        deadline = time.monotonic() + timeout
        while True:
            mark = self._mark()
            self._send("state")
            try:
                found = self._wait_line(APP, mark, min(1, max(0.1, timeout)))
                return found.group(1) == PROJECT_NAME
            except TimeoutError:
                if time.monotonic() >= deadline:
                    return False
