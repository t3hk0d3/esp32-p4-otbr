"""Zigbee mode: ziggurat on a radio channel, driven by zigpy-ziggurat (the library ZHA uses) over ws://<board>/zigbee.
Hardware-gated: written for the batched on-board pass, never run where it was written.

Rig: the board on the LAN, the C6 as the radio (`c6`); with `-m usb_dongle` also a CC2652P/EFR32 dongle on the USB-A
port. The tests with the marker `zigbee_device` need the aux C6 running tools/zigbee_test_device
(`make zigbee-test-device`; env ZIGBEE_DEVICE_PORT; select with `-m zigbee_device`).

They target the radio channel mux's device API (docs/superpowers/specs/2026-11-06-radio-channel-mux-design.md, "API"):
`GET /device/channels`, `PUT /device/channels/{id}` with `{"mode": ...}` (a change into or out of `ziggurat` answers
`"reboot": true` and the board reboots). That API is not on feat/zigbee-mode until the mux is merged.

Every test that switches the mode puts the channel back afterwards (fixture `zigbee_mode`): the C6 to `otbr`, a dongle
to `off`.
"""

import asyncio
import contextlib
import statistics
import time

import pytest
import requests
from conftest import (
    BOOT_TIMEOUT_S,
    CONSOLE_READY,
    ROM_BANNER,
    wait_reachable,
)

zigpy_types = pytest.importorskip("zigpy.types")
from zigpy_ziggurat.zigbee.application import (
    ControllerApplication,
)

C6 = "c6"
OTBR_MODE = "otbr"
RESTING_MODE = {C6: OTBR_MODE}  # a dongle rests in `off`
HELLO_S = 5  # zigpy waits this long for the hello on connect
HELLO_MARGIN_S = 1
DROPPED_S = 10
RUNNING_TIMEOUT_S = 60
FORM_TIMEOUT_S = 60
SCAN_TIMEOUT_S = 60
RECOVERY_S = 60
TAKEOVER_S = 10
PERMIT_S = 60
INTERVIEW_TIMEOUT_S = 60
REPORT_TIMEOUT_S = 10
LATENCY_ROUNDS = 30
LATENCY_BUDGET_S = 0.5  # recorded, loose: the figure matters more than the bound
# A connected client keeps its socket and WebSocket buffers (about 0.7 KB on the board, 2026-11-07); the margin allows
# that and still catches a lost frame buffer or a growing queue.
HEAP_MARGIN_BYTES = 4096
HEAP_SETTLE_S = 5
ON_OFF_CLUSTER_ID = 0x0006
ATTR_ON_OFF = 0x0000
ZIGBEE_MODE = "ziggurat"
ENERGY_CHANNELS = 16  # 802.15.4 channels 11..26
USB_CHANNEL_KIND = "usb"
EXPECTED_STATE = "running"


def radio_params():
    """The radios a test runs on: the C6 always, a dongle only with `-m usb_dongle`."""
    return [
        pytest.param(C6, id="c6"),
        pytest.param(USB_CHANNEL_KIND, id="usb", marks=pytest.mark.usb_dongle),
    ]


@pytest.fixture
def base_url(board_ip):
    return f"http://{board_ip}"


def get_json(base_url, path):
    response = requests.get(f"{base_url}{path}", timeout=10)
    assert response.status_code == requests.codes.ok, response.text
    return response.json()


def zigbee_status(base_url):
    return get_json(base_url, "/device/zigbee")


def wait_state(base_url, state, timeout):
    """Poll /device/zigbee until its `state` is `state`; fail with the last body."""
    deadline = time.monotonic() + timeout
    body = None
    while time.monotonic() < deadline:
        try:
            response = requests.get(f"{base_url}/device/zigbee", timeout=5)
            body = response.json()
            if response.status_code == requests.codes.ok and body["state"] == state:
                return body
        except (requests.RequestException, ValueError):
            pass
        time.sleep(1)
    pytest.fail(f"/device/zigbee never reached {state!r}; last body: {body}")


def channel_id(base_url, radio):
    """The mux id of a radio: `c6`, or the first USB channel that is present."""
    if radio == C6:
        return C6
    channels = get_json(base_url, "/device/channels")
    channels = (
        channels.get("channels", channels) if isinstance(channels, dict) else channels
    )
    for channel in channels:
        if channel["kind"] == USB_CHANNEL_KIND and channel["mode"] != "unavailable":
            return channel["id"]
    pytest.skip("no USB radio dongle on the board")


def channel_mode(base_url, channel):
    channels = get_json(base_url, "/device/channels")
    channels = (
        channels.get("channels", channels) if isinstance(channels, dict) else channels
    )
    return next(c["mode"] for c in channels if c["id"] == channel)


def wait_rebooted(dut, base_url):
    """After a PUT that answered `reboot: true`: the board restarts, then serves HTTP again."""
    dut.expect(ROM_BANNER, timeout=BOOT_TIMEOUT_S)
    dut.expect(CONSOLE_READY, timeout=BOOT_TIMEOUT_S)
    wait_reachable(base_url.removeprefix("http://"))


def set_mode(dut, base_url, channel, mode):
    """PUT the channel's mode and, when the board says it reboots, wait for the reboot. Returns the answer body."""
    response = requests.put(
        f"{base_url}/device/channels/{channel}", json={"mode": mode}, timeout=30
    )
    assert response.status_code in (requests.codes.ok, requests.codes.accepted), (
        response.text
    )
    body = response.json()
    if body.get("reboot"):
        wait_rebooted(dut, base_url)
    return body


def resting_mode(channel):
    return RESTING_MODE.get(channel, "off")


def restore_channel(dut, base_url, channel):
    """Put the channel back to its resting mode and check the board really is back; raises when it is not."""
    if channel_mode(base_url, channel) != resting_mode(channel):
        set_mode(dut, base_url, channel, resting_mode(channel))
    assert channel_mode(base_url, channel) == resting_mode(channel)
    assert get_json(base_url, "/device/status")["zigbee_channel"] is None


@pytest.fixture
def guard_channel(dut, base_url):
    """guard_channel(channel): call BEFORE changing the channel's mode; the channel is restored when the test ends,
    on every path. A restore that fails fails the test loudly: a board left in Zigbee mode poisons later suites."""
    guarded = []
    yield guarded.append
    failures = []
    for channel in guarded:
        try:
            restore_channel(dut, base_url, channel)
        except Exception as error:  # noqa: BLE001  (collected, reported below)
            failures.append(f"{channel}: {error!r}")
    if failures:
        pytest.fail(
            f"could not restore the radio, the board may stay in Zigbee mode: {failures}"
        )


@pytest.fixture
def zigbee_mode(dut, base_url, guard_channel):
    """zigbee_mode(radio): switch the radio's channel to ziggurat (reboot) and return its id; restored afterwards."""

    def enter(radio):
        channel = channel_id(base_url, radio)
        guard_channel(channel)
        set_mode(dut, base_url, channel, ZIGBEE_MODE)
        wait_state(base_url, EXPECTED_STATE, RUNNING_TIMEOUT_S)
        return channel

    return enter


@contextlib.asynccontextmanager
async def zigpy_app(address, auto_form=True):
    """A zigpy coordinator connected to the board's WebSocket, shut down afterwards."""
    config = {"device": {"path": f"ws://{address}/zigbee"}, "database_path": None}
    app = await ControllerApplication.new(config, auto_form=auto_form, start_radio=True)
    try:
        yield app
    finally:
        with contextlib.suppress(
            Exception
        ):  # a dropped session may fail to close cleanly
            await app.shutdown()


def run(coroutine):
    return asyncio.run(coroutine)


async def pair_device(app, device, permit=PERMIT_S):
    """Open permit-join, let the test device join and be interviewed; return the zigpy device."""
    joined = asyncio.get_running_loop().create_future()

    class Listener:
        def raw_device_initialized(self, zigpy_device):
            if not joined.done():
                joined.set_result(zigpy_device)

    app.add_listener(Listener())
    device.set_channels([app.state.network_info.channel])
    device.factory_reset()
    await app.permit(permit)
    waiter = asyncio.get_running_loop().run_in_executor(
        None, device.wait_joined, permit
    )
    zigpy_device = await asyncio.wait_for(joined, INTERVIEW_TIMEOUT_S)
    await waiter
    return zigpy_device


def on_off_cluster(zigpy_device):
    for endpoint_id, endpoint in zigpy_device.endpoints.items():
        # Endpoint 0 is zigpy's ZDO object: no clusters, no endpoint_id.
        if endpoint_id != 0 and ON_OFF_CLUSTER_ID in endpoint.in_clusters:
            return endpoint.in_clusters[ON_OFF_CLUSTER_ID]
    pytest.fail("the joined device has no On/Off cluster")


@pytest.fixture
def zigbee_test_device():
    """The aux C6's Zigbee test device (flashed when it is not this project's), or a skip when the port is busy."""
    from zigbee_device import DeviceBusy, ZigbeeTestDevice

    try:
        device = ZigbeeTestDevice()
        device.open()
    except DeviceBusy as error:
        pytest.skip(str(error))
    yield device
    device.close()


def trigger_radio_reset(send, radio):
    """Make the radio reset under the running coordinator."""
    if radio == C6:
        send("rcp_glitch", rb"rcp_glitch: ok")
    else:
        pytest.skip(
            "TODO: the mux's consumer reset for a dongle has no REST trigger yet"
        )


@pytest.mark.parametrize("radio", radio_params())
class TestZigbeeMode:
    def test_switch_to_ziggurat_and_back(
        self, dut, base_url, http, guard_channel, radio
    ):
        """PUT mode, reboot, /device/zigbee running and Thread's API gone; back to otbr with the same dataset."""
        channel = channel_id(base_url, radio)
        guard_channel(
            channel
        )  # restored by the fixture on every path, before anything can fail
        dataset = None
        if radio == C6:
            dataset = http.get("/node/dataset/active")
            if dataset.status_code == requests.codes.ok:
                dataset = dataset.text
            else:
                dataset = None
        body = set_mode(dut, base_url, channel, ZIGBEE_MODE)
        assert body.get("reboot") is True
        status = wait_state(base_url, EXPECTED_STATE, RUNNING_TIMEOUT_S)
        assert status["channel"] == channel
        assert get_json(base_url, "/device/status")["zigbee_channel"] == channel
        assert channel_mode(base_url, channel) == ZIGBEE_MODE
        # Thread does not run: its REST API does not answer as a running border router.
        assert (
            requests.get(f"{base_url}/node/state", timeout=10).status_code
            != requests.codes.ok
        )
        # TODO(hardware pass): the web UI hides the Thread menu entries; look at it by eye.
        back = set_mode(dut, base_url, channel, resting_mode(channel))
        assert back.get("reboot") is True
        assert requests.get(f"{base_url}/device/zigbee", timeout=10).status_code == (
            requests.codes.conflict
        )
        assert get_json(base_url, "/device/status")["zigbee_channel"] is None
        if radio == C6 and dataset is not None:
            deadline = time.monotonic() + RUNNING_TIMEOUT_S
            again = None
            while time.monotonic() < deadline:
                try:  # the board is still rebooting into Thread: a dropped connection is a retry
                    again = http.get("/node/dataset/active")
                    if again.status_code == requests.codes.ok:
                        break
                except requests.RequestException:
                    pass
                time.sleep(2)
            assert again is not None
            assert again.text == dataset

    def test_form_and_energy_scan(self, base_url, zigbee_mode, radio):
        """Connecting forms a network (the stack keeps no state of its own) and an energy scan covers every channel."""
        zigbee_mode(radio)

        async def body():
            async with zigpy_app(base_url.removeprefix("http://")) as app:
                info, node = app.state.network_info, app.state.node_info
                assert 11 <= info.channel <= 26
                assert info.pan_id != 0
                assert node.nwk == 0
                scan = await asyncio.wait_for(
                    app.energy_scan(
                        channels=zigpy_types.Channels.ALL_CHANNELS,
                        duration_exp=2,
                        count=1,
                    ),
                    SCAN_TIMEOUT_S,
                )
                assert len(scan) == ENERGY_CHANNELS
                return info.channel, info.pan_id

        channel, pan_id = run(body())
        status = zigbee_status(base_url)
        assert status["radio"]["channel"] == channel
        assert status["radio"]["pan_id"] == pan_id
        assert status["counters"]["energy_scans"] >= 1
        assert status["client"] is None or status["state"] == EXPECTED_STATE

    @pytest.mark.zigbee_device
    def test_device_joins_and_toggles(
        self, base_url, zigbee_mode, zigbee_test_device, radio
    ):
        """Join and interview the test device; on/off from zigpy turns its LED on and off (read back from its console)."""
        zigbee_mode(radio)

        async def body():
            async with zigpy_app(base_url.removeprefix("http://")) as app:
                zigpy_device = await pair_device(app, zigbee_test_device)
                cluster = on_off_cluster(zigpy_device)
                await cluster.on()
                zigbee_test_device.wait_light("on")
                assert zigbee_test_device.light_state() == "on"
                await cluster.off()
                zigbee_test_device.wait_light("off")
                assert zigbee_test_device.light_state() == "off"

        run(body())

    @pytest.mark.zigbee_device
    def test_device_report_reaches_zigpy(
        self, base_url, zigbee_mode, zigbee_test_device, radio
    ):
        """Toggling the light from the device's console reaches zigpy as an On/Off attribute report."""
        if not hasattr(zigbee_test_device, "toggle"):
            pytest.skip(
                "the test device has no console command to toggle its light yet "
                "(tools/zigbee_test_device/main/repl.c, p4/pytest/zigbee_device.py)"
            )
        zigbee_mode(radio)

        async def body():
            async with zigpy_app(base_url.removeprefix("http://")) as app:
                zigpy_device = await pair_device(app, zigbee_test_device)
                cluster = on_off_cluster(zigpy_device)
                seen = asyncio.get_running_loop().create_future()

                class Listener:
                    def attribute_updated(self, attribute_id, value, timestamp=None):
                        if attribute_id == ATTR_ON_OFF and not seen.done():
                            seen.set_result(bool(value))

                cluster.add_listener(Listener())
                before = zigbee_test_device.light_state()
                zigbee_test_device.toggle()
                reported = await asyncio.wait_for(seen, REPORT_TIMEOUT_S)
                assert reported == (before == "off")

        run(body())

    def test_reconnect_gets_hello_within_5s(self, base_url, zigbee_mode, radio):
        """After a client leaves, the next connect gets the hello (zigpy waits 5 s for it) and the same network."""
        zigbee_mode(radio)
        address = base_url.removeprefix("http://")

        async def body():
            async with zigpy_app(address) as app:
                first = app.state.network_info.pan_id
            # The constructor validates the config itself (validating it twice breaks zigpy's OTA provider).
            app = ControllerApplication(
                {"device": {"path": f"ws://{address}/zigbee"}, "database_path": None}
            )
            try:
                started = time.monotonic()
                await (
                    app.connect()
                )  # returns once zigpy has the hello (its own wait is 5 s)
                hello_s = time.monotonic() - started
                assert hello_s < HELLO_S + HELLO_MARGIN_S, hello_s
                await app.initialize()
                assert app.state.network_info.pan_id == first
            finally:
                with contextlib.suppress(Exception):
                    await app.shutdown()

        run(body())
        status = zigbee_status(base_url)
        assert status["last_client"] is not None

    def test_second_client_takes_over(self, base_url, zigbee_mode, radio):
        """A second client connecting replaces the first: the first is dropped and /device/zigbee names it last_client.
        Both clients run on this host, so the address does not change, and `since` is 0 while the clock is unset."""
        zigbee_mode(radio)
        address = base_url.removeprefix("http://")

        async def scan(app):
            return await asyncio.wait_for(
                app.energy_scan(
                    channels=zigpy_types.Channels.ALL_CHANNELS, duration_exp=2, count=1
                ),
                DROPPED_S,
            )

        async def body():
            async with zigpy_app(address) as first:
                before = zigbee_status(base_url)
                assert before["last_client"] is None, (
                    before
                )  # a fresh boot: nobody left yet
                async with zigpy_app(address, auto_form=False) as second:
                    deadline = time.monotonic() + TAKEOVER_S
                    after = before
                    while time.monotonic() < deadline and after["last_client"] is None:
                        await asyncio.sleep(0.5)
                        after = zigbee_status(base_url)
                    assert after["last_client"] is not None, after
                    assert (
                        after["last_client"]["address"] == before["client"]["address"]
                    )
                    assert after["client"] is not None
                    # The first client lost its connection: a request on it fails; the second one works.
                    with pytest.raises(Exception):  # noqa: B017  (ConnectionError or a timeout)
                        await scan(first)
                    assert len(await scan(second)) == ENERGY_CHANNELS

        run(body())

    def test_radio_reset_survived(self, dut, send, base_url, zigbee_mode, radio):
        """A radio reset (C6 `rcp_glitch`): rcp_resets grows, the client stays connected, and the device still answers."""
        zigbee_mode(radio)
        address = base_url.removeprefix("http://")

        async def body():
            async with zigpy_app(address) as app:
                before = zigbee_status(base_url)
                trigger_radio_reset(send, radio)
                # At least one: on the C6 the mux's RESET and the RCP's own boot status may count as two.
                deadline = time.monotonic() + RECOVERY_S
                while time.monotonic() < deadline:
                    now = zigbee_status(base_url)
                    if (
                        now["counters"]["rcp_resets"]
                        >= before["counters"]["rcp_resets"] + 1
                        and now["state"] == EXPECTED_STATE
                    ):
                        break
                    await asyncio.sleep(1)
                assert (
                    now["counters"]["rcp_resets"]
                    >= before["counters"]["rcp_resets"] + 1
                )
                assert now["state"] == EXPECTED_STATE
                assert now["client"] is not None
                assert now["client"]["address"] == before["client"]["address"]
                scan = await asyncio.wait_for(
                    app.energy_scan(
                        channels=zigpy_types.Channels.ALL_CHANNELS,
                        duration_exp=2,
                        count=1,
                    ),
                    SCAN_TIMEOUT_S,
                )
                assert len(scan) == ENERGY_CHANNELS

        run(body())

    def test_radio_held_comes_back(self, dut, send, base_url, zigbee_mode, radio):
        """The C6 held in reset (`rcp_kill`) under a connected client: the client's refused sends ask for a channel
        reset, the reset brings the C6 back, rcp_resets grows, no reboot, and the stack answers again."""
        if radio != C6:
            pytest.skip("rcp_kill holds the C6 only")
        zigbee_mode(radio)
        address = base_url.removeprefix("http://")
        uptime_before = get_json(base_url, "/device/diag")["uptime_s"]

        async def body():
            async with zigpy_app(address) as app:
                before = zigbee_status(base_url)["counters"]["rcp_resets"]
                send("rcp_kill", rb"rcp_kill: ESP_OK")
                try:
                    deadline = time.monotonic() + RECOVERY_S
                    now = zigbee_status(base_url)
                    while time.monotonic() < deadline:
                        with contextlib.suppress(
                            Exception
                        ):  # sends fail until the reset brought the C6 back
                            await asyncio.wait_for(app.permit(0), 5)
                        now = zigbee_status(base_url)
                        if (
                            now["counters"]["rcp_resets"] > before
                            and now["state"] == EXPECTED_STATE
                        ):
                            break
                        await asyncio.sleep(1)
                    assert now["counters"]["rcp_resets"] > before, now
                    assert now["state"] == EXPECTED_STATE, now
                finally:
                    send(
                        "c6_hold 0", rb"c6_hold: ok"
                    )  # the flag persists: never leave it for the next boot
                scan = await asyncio.wait_for(
                    app.energy_scan(
                        channels=zigpy_types.Channels.ALL_CHANNELS,
                        duration_exp=2,
                        count=1,
                    ),
                    SCAN_TIMEOUT_S,
                )
                assert len(scan) == ENERGY_CHANNELS

        run(body())
        assert get_json(base_url, "/device/diag")["uptime_s"] > uptime_before

    def test_radio_gone_and_back(self, base_url, radio):
        """The radio disappears (GONE): state no_radio, no reboot; after its return the stack answers. On the C6 GONE
        comes only from a C6 firmware job: test_radio_held_comes_back covers the C6's loss."""
        if radio == C6:
            pytest.skip(
                "the C6 has no GONE without a firmware job; test_radio_held_comes_back covers its loss"
            )
        pytest.skip(
            "TODO: a dongle needs a person to unplug it (no manual opt-in in conftest yet)"
        )

    def test_internal_heap_not_lower_than_idle(
        self, record_property, base_url, zigbee_mode, radio
    ):
        """Internal free heap with a client and a formed network is not below the figure of the channel idle (Zigbee
        mode, the stack started, no client); the stack's PSRAM use is recorded."""
        channel = zigbee_mode(radio)
        time.sleep(HEAP_SETTLE_S)
        assert zigbee_status(base_url)["client"] is None
        idle = get_json(base_url, "/device/diag")["memory"]["internal"]["free"]

        async def body():
            async with zigpy_app(base_url.removeprefix("http://")):
                await asyncio.sleep(HEAP_SETTLE_S)
                return (
                    get_json(base_url, "/device/diag")["memory"]["internal"]["free"],
                    zigbee_status(base_url)["heap"],
                )

        busy, stack_heap = run(body())
        record_property("zigbee_internal_free_idle", idle)
        record_property("zigbee_internal_free_busy", busy)
        record_property("zigbee_stack_heap_current", stack_heap["current"])
        record_property("zigbee_stack_heap_peak", stack_heap["peak"])
        assert busy >= idle - HEAP_MARGIN_BYTES, (channel, idle, busy)

    @pytest.mark.zigbee_device
    def test_latency_p50(
        self, record_property, base_url, zigbee_mode, zigbee_test_device, radio
    ):
        """Median of an on/off round trip through zigpy to the test device; recorded, with a loose bound."""
        zigbee_mode(radio)

        async def body():
            async with zigpy_app(base_url.removeprefix("http://")) as app:
                cluster = on_off_cluster(await pair_device(app, zigbee_test_device))
                samples = []
                for index in range(LATENCY_ROUNDS):
                    started = time.monotonic()
                    await (cluster.on() if index % 2 == 0 else cluster.off())
                    samples.append(time.monotonic() - started)
                return statistics.median(samples)

        p50 = run(body())
        record_property("zigbee_onoff_p50_s", p50)
        print(f"zigbee on/off round trip p50: {p50 * 1000:.0f} ms")
        assert p50 < LATENCY_BUDGET_S
