"""TREL build options (docs/superpowers/specs/2026-11-03-trel-design.md).

The IDF menu "Thread Trel Radio Link" depends on EXAMPLE_CONNECT_ETHERNET (protocol_examples_common): if that
dependency ever goes away, Kconfig drops CONFIG_OPENTHREAD_RADIO_TREL from the generated sdkconfig without an error.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
OPTIONS = {
    "CONFIG_OPENTHREAD_RADIO_TREL": "y",
    "CONFIG_OPENTHREAD_TREL_BUFFER_SIZE": "20",
}


def value(text, option):
    match = re.search(rf"^{option}=(.*)$", text, re.MULTILINE)
    return match.group(1) if match else None


def test_trel_is_enabled_in_the_defaults():
    text = (ROOT / "p4/sdkconfig.defaults").read_text()
    for option, expected in OPTIONS.items():
        assert value(text, option) == expected, option


def test_trel_survives_into_the_generated_sdkconfig():
    generated = ROOT / "p4/sdkconfig"
    if not generated.exists():
        return  # not configured yet: the defaults test covers the source
    text = generated.read_text()
    for option, expected in OPTIONS.items():
        assert value(text, option) == expected, option
