"""common/c6_board/c6_board.h flash constants agree with partitions_c6.csv (ROM recovery derives its regions from them)."""

import csv
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
HEADER = ROOT / "common/c6_board/c6_board.h"
TABLE = ROOT / "common/c6_board/partitions_c6.csv"


def constants():
    return {
        name: int(value, 16)
        for name, value in re.findall(
            r"#define\s+(C6_FLASH_\w+)\s+(0x[0-9a-fA-F]+)", HEADER.read_text()
        )
    }


def partitions():
    rows = csv.reader(
        line for line in TABLE.read_text().splitlines() if not line.startswith("#")
    )
    return {r[0].strip(): (int(r[3], 16), int(r[4], 16)) for r in rows if len(r) >= 5}


def test_otadata_region_matches_the_partition_table():
    assert partitions()["otadata"] == (
        constants()["C6_FLASH_OTADATA_OFFSET"],
        constants()["C6_FLASH_OTADATA_SIZE"],
    )


def test_test_region_matches_the_partition_table():
    assert partitions()["test"] == (
        constants()["C6_FLASH_TEST_OFFSET"],
        constants()["C6_FLASH_TEST_SIZE"],
    )


def test_nvs_and_ota_0_offsets_match_the_partition_table():
    assert partitions()["nvs"][0] == constants()["C6_FLASH_NVS_OFFSET"]
    assert partitions()["ota_0"][0] == constants()["C6_FLASH_OTA0_OFFSET"]


# ESP32-C6 constants (ESP-IDF components/bootloader/Kconfig.projbuild: CONFIG_BOOTLOADER_OFFSET_IN_FLASH is 0x0 on the
# C6, unlike the 0x1000 of older chips). The partition table offset is stated in c6_updater/sdkconfig.defaults. No build
# artifact is read, so the test needs no prior build.
C6_BOOTLOADER_OFFSET = 0x0


def test_bootloader_offset_is_the_c6_rom_constant():
    assert constants()["C6_FLASH_BOOTLOADER_OFFSET"] == C6_BOOTLOADER_OFFSET


def test_partition_table_offset_matches_the_updater_defaults():
    defaults = (ROOT / "c6_updater/sdkconfig.defaults").read_text()
    table = int(
        re.search(
            r"^CONFIG_PARTITION_TABLE_OFFSET=(0x[0-9a-fA-F]+)", defaults, re.MULTILINE
        ).group(1),
        16,
    )
    assert constants()["C6_FLASH_PTABLE_OFFSET"] == table
