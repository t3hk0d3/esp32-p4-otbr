"""mDNS library configuration the netcfg design relies on (spec 5.13)."""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
OPTION = "CONFIG_MDNS_SKIP_SUPPRESSING_OWN_QUERIES"
# Split mode: both VLAN netifs have the same EUI-64 link-local address, a reflected packet from the other
# netif's link-local source is only dropped by the library's own-source check, which this option removes.


def enabled(path):
    return re.search(rf"^{OPTION}=y", path.read_text(), re.MULTILINE) is not None


def test_own_source_check_is_not_disabled_in_the_defaults():
    assert not enabled(ROOT / "p4/sdkconfig.defaults")


def test_own_source_check_is_not_disabled_in_the_generated_sdkconfig():
    generated = ROOT / "p4/sdkconfig"
    if not generated.exists():
        return  # not configured yet: the defaults test covers the source
    assert not enabled(generated)


PINNED_VERSION = (
    "1.14.1"  # patch 13 (--wrap=udp_recv) and R31 depend on this version's internals
)


def test_mdns_is_pinned_to_the_exact_version_in_the_manifest():
    manifest = (ROOT / "p4/main/idf_component.yml").read_text()
    assert re.search(
        rf'^\s*espressif/mdns:\s*"==\s*{re.escape(PINNED_VERSION)}"',
        manifest,
        re.MULTILINE,
    )


def test_mdns_is_locked_to_the_pinned_version():
    lock = (ROOT / "p4/dependencies.lock").read_text()
    entry = re.search(r"^  espressif/mdns:\n(?:    .*\n)+", lock, re.MULTILINE)
    assert entry is not None
    assert f"version: {PINNED_VERSION}" in entry.group(0)
