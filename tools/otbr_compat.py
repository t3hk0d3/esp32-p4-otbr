"""Home Assistant compatibility driver: every python-otbr-api call of HA's `otbr` integration against a board.

Usage: tools/otbr_compat.py --url http://<board> [--expect-writes-refused] [--no-factory-reset]
Needs `python-otbr-api` (p4/pytest/requirements.txt) and a board in BR_RUNNING with a stored dataset.

Exits non-zero at the first failing call, printing `FAIL <call>`. The run restores the board:
  - dataset: replaced by `create_active_dataset` while Thread is disabled and put back with
    `set_active_dataset_tlvs`; the channel is moved by `set_channel` and put back by `create_pending_dataset`;
  - Thread is enabled again and the ePSKc setting is restored.
`factory_reset` (DELETE /node) is destructive (erases the settings, the board restarts), so it runs LAST: afterwards
the original dataset is written back through the REST API and Thread is enabled. Skip it with --no-factory-reset.

--expect-writes-refused is for the management address of a board in split network mode (spec 5.13: REST writes are
allowed only on thread_bb): the read calls must work and the first write call must be refused with HTTP 403.
"""

import argparse
import asyncio
import sys
import time

import aiohttp
import python_otbr_api as api

ATTACHED = {"leader", "router", "child"}
ATTACH_TIMEOUT_S = 120
CHANNEL_TIMEOUT_S = 150
RESTART_TIMEOUT_S = 180
ALTERNATE_CHANNEL = 15
FALLBACK_CHANNEL = 20
SHORT_DELAY_MS = 20  # the leader raises it to its minimum delay timer


class CompatFailure(Exception):
    """A call did not behave like the Home Assistant integration needs."""


def say(line):
    print(line, flush=True)


async def call(name, coroutine):
    """Await one API call; print it; exit 1 on any exception."""
    try:
        result = await coroutine
    except Exception as error:  # noqa: BLE001  (any failure of a call ends the run)
        say(f"FAIL {name}: {type(error).__name__}: {error}")
        sys.exit(1)
    shown = result.hex() if isinstance(result, bytes) else result
    say(f"OK   {name}" + (f" -> {str(shown)[:100]}" if shown is not None else ""))
    return result


async def call_expecting(name, coroutine, error_type):
    """Await a call that must raise `error_type`."""
    try:
        await coroutine
    except error_type as error:
        say(f"OK   {name} -> {type(error).__name__} (expected)")
        return
    except Exception as error:  # noqa: BLE001
        say(
            f"FAIL {name}: expected {error_type.__name__}, got {type(error).__name__}: {error}"
        )
        sys.exit(1)
    say(f"FAIL {name}: expected {error_type.__name__}, the call succeeded")
    sys.exit(1)


def check(name, condition, detail=""):
    if not condition:
        say(f"FAIL {name}: {detail}")
        sys.exit(1)
    say(f"OK   {name}")


async def node_state(session, url):
    async with session.get(
        f"{url}/node/state", timeout=aiohttp.ClientTimeout(total=10)
    ) as response:
        return await response.json()


async def wait_until(what, probe, timeout, interval=2):
    """Poll `probe()` (a coroutine returning a truthy value) until it is truthy; exit 1 on timeout."""
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            last = await probe()
            if last:
                say(f"OK   {what}")
                return last
        except (aiohttp.ClientError, TimeoutError, api.OTBRError) as error:
            last = f"{type(error).__name__}: {error}"
        await asyncio.sleep(interval)
    say(f"FAIL {what}: not within {timeout} s (last: {last})")
    sys.exit(1)


async def wait_attached(session, url, otbr):
    async def probe():
        return await node_state(session, url) in ATTACHED

    await wait_until("Thread attached again", probe, ATTACH_TIMEOUT_S)


async def wait_channel(otbr, channel):
    async def probe():
        dataset = await otbr.get_active_dataset()
        return dataset is not None and dataset.channel == channel

    await wait_until(f"active dataset on channel {channel}", probe, CHANNEL_TIMEOUT_S)


async def read_calls(otbr):
    """The calls HA makes to show the integration and to identify the router (no writes)."""
    version = await call("get_api_version", otbr.get_api_version())
    check("get_api_version is a 2.x API", str(version).startswith("2"), version)
    tlvs = await call("get_active_dataset_tlvs", otbr.get_active_dataset_tlvs())
    check(
        "an active dataset is stored",
        bool(tlvs),
        "no active dataset: provision one first",
    )
    dataset = await call("get_active_dataset", otbr.get_active_dataset())
    await call("get_pending_dataset_tlvs", otbr.get_pending_dataset_tlvs())
    agent_id = await call("get_border_agent_id", otbr.get_border_agent_id())
    check("border agent id is 16 bytes", len(agent_id) == 16, len(agent_id))
    address = await call("get_extended_address", otbr.get_extended_address())
    check("extended address is 8 bytes", len(address) == 8, len(address))
    version_string = await call(
        "get_coprocessor_version", otbr.get_coprocessor_version()
    )
    check("coprocessor version is not empty", bool(version_string))
    enabled = await call("get_ephemeral_key_enabled", otbr.get_ephemeral_key_enabled())
    return tlvs, dataset, enabled


async def ephemeral_key_calls(otbr, was_enabled):
    await call("set_ephemeral_key_enabled(True)", otbr.set_ephemeral_key_enabled(True))
    result = await call("activate_ephemeral_key", otbr.activate_ephemeral_key())
    check(
        "activate_ephemeral_key returned a passcode",
        bool(getattr(result, "tap", None)),
        result,
    )
    status = await call("get_ephemeral_key_status", otbr.get_ephemeral_key_status())
    check(
        "ePSKc session started", status.state != api.EphemeralKeyState.DISABLED, status
    )
    await call("deactivate_ephemeral_key", otbr.deactivate_ephemeral_key())
    await call(
        f"set_ephemeral_key_enabled({was_enabled})",
        otbr.set_ephemeral_key_enabled(was_enabled),
    )


async def dataset_calls(session, url, otbr, tlvs, dataset):
    """Disable Thread, replace the dataset the way HA does for a new network, restore it and enable Thread."""
    await call("set_enabled(False)", otbr.set_enabled(False))
    await wait_disabled(session, url)
    fresh = api.ActiveDataSet(network_name="compat-test", channel=dataset.channel)
    await call("create_active_dataset", otbr.create_active_dataset(fresh))
    created = await call("get_active_dataset (after create)", otbr.get_active_dataset())
    check(
        "created dataset has the requested name",
        created.network_name == "compat-test",
        created.network_name,
    )
    check("created dataset got a network key", bool(created.network_key))
    await call("set_active_dataset_tlvs (restore)", otbr.set_active_dataset_tlvs(tlvs))
    restored = await call(
        "get_active_dataset_tlvs (after restore)", otbr.get_active_dataset_tlvs()
    )
    check(
        "original dataset is back",
        restored == tlvs,
        f"{restored.hex()} != {tlvs.hex()}",
    )
    await call("set_enabled(True)", otbr.set_enabled(True))
    await wait_attached(session, url, otbr)


async def wait_disabled(session, url):
    async def probe():
        return await node_state(session, url) == "disabled"

    await wait_until("Thread disabled", probe, ATTACH_TIMEOUT_S)


async def channel_calls(session, url, otbr, dataset):
    """set_channel and create_pending_dataset (the second one moves the channel back)."""
    original = dataset.channel
    target = ALTERNATE_CHANNEL if original != ALTERNATE_CHANNEL else FALLBACK_CHANNEL
    await call(f"set_channel({target})", otbr.set_channel(target, delay=SHORT_DELAY_MS))
    await wait_channel(otbr, target)
    current = await otbr.get_active_dataset()
    if current.active_timestamp and current.active_timestamp.seconds is not None:
        current.active_timestamp.seconds += 1
    current.channel = original
    pending = api.PendingDataSet(active_dataset=current, delay=SHORT_DELAY_MS)
    await call(
        f"create_pending_dataset (channel {original})",
        otbr.create_pending_dataset(pending),
    )
    await wait_channel(otbr, original)
    await wait_attached(session, url, otbr)


async def factory_reset_and_reprovision(session, url, otbr, tlvs):
    await call("factory_reset", otbr.factory_reset())
    await asyncio.sleep(5)  # the board restarts after answering

    async def probe():
        return await otbr.get_api_version()

    await wait_until(
        "board answers again after factory_reset", probe, RESTART_TIMEOUT_S
    )
    stored = await call(
        "get_active_dataset_tlvs (after factory_reset)", otbr.get_active_dataset_tlvs()
    )
    check("factory_reset erased the dataset", not stored, stored)
    await call(
        "set_active_dataset_tlvs (re-provision)", otbr.set_active_dataset_tlvs(tlvs)
    )
    await call("set_enabled(True) (re-provision)", otbr.set_enabled(True))
    await wait_attached(session, url, otbr)
    again = await call(
        "get_active_dataset_tlvs (final)", otbr.get_active_dataset_tlvs()
    )
    check(
        "original dataset is back after the reset",
        again == tlvs,
        f"{again.hex()} != {tlvs.hex()}",
    )


async def refused_write(otbr):
    """Split mode, management address: reads work, the first write gets 403."""
    try:
        await otbr.set_enabled(False)
    except api.OTBRError as error:
        check("first write is refused with 403", "403" in str(error), error)
        return
    say("FAIL set_enabled(False): the write was accepted on the management address")
    sys.exit(1)


async def main(args):
    url = args.url.rstrip("/")
    async with aiohttp.ClientSession() as session:
        otbr = api.OTBR(url, session, timeout=20)
        tlvs, dataset, ephemeral_was_enabled = await read_calls(otbr)
        if args.expect_writes_refused:
            await refused_write(otbr)
            return
        await ephemeral_key_calls(otbr, ephemeral_was_enabled)
        await call_expecting(
            "set_active_dataset_tlvs while Thread is enabled",
            otbr.set_active_dataset_tlvs(tlvs),
            api.ThreadNetworkActiveError,
        )
        await dataset_calls(session, url, otbr, tlvs, dataset)
        await channel_calls(session, url, otbr, dataset)
        if not args.no_factory_reset:
            await factory_reset_and_reprovision(session, url, otbr, tlvs)
    say("ALL OK")


def parse_arguments(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument(
        "--url", required=True, help="base URL of the board, e.g. http://192.168.1.117"
    )
    parser.add_argument(
        "--expect-writes-refused",
        action="store_true",
        help="management address in split mode",
    )
    parser.add_argument(
        "--no-factory-reset", action="store_true", help="skip the destructive last call"
    )
    return parser.parse_args(argv)


if __name__ == "__main__":
    asyncio.run(main(parse_arguments(sys.argv[1:])))
