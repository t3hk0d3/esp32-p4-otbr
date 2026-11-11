# Building and flashing

## Prerequisites

- ESP-IDF at the version in `IDF_VERSION` (v6.1). The Makefile activates it by itself from `~/esp/esp-idf-v6.1/export.sh`
  when `idf.py` is not on `PATH`; point `IDF_EXPORT` at another `export.sh` if yours lives elsewhere.
- The ziggurat submodule (`git clone --recurse-submodules`, or `git submodule update --init` in an existing clone): the P4
  build compiles its Zigbee stack from source.
- Rust for that build, either of:
    - `rustup` with the target of the toolchain pinned in `p4/components/zigbee/rust-toolchain.toml`
      (`cd p4/components/zigbee && rustup target add riscv32imafc-unknown-none-elf`); cargo then runs on the host.
    - Docker: without the target `p4/components/zigbee/tools/build_staticlib.sh` runs cargo in the `rust:1.99-slim`
      image instead, so Docker alone is enough (the image is pulled on first use).
- `uv` for the tools that are not installed globally (`make lint`, `make test-tools`).
- `node` (optional): runs the device page tests in `make test-tools`.
- `clang++` (optional): runs `make test-vendored`.
- For the on-board tests: a Python virtualenv with `p4/pytest/requirements.txt` (`uv venv .venv` and
  `uv pip install -r p4/pytest/requirements.txt`).

`make help` lists every target and the variables they take (`PORT`, `BOARD`, `C6_PORT`, `DIST`, `TEST`, `MARK`).

## Build

```
make build         # c6_updater, c6_rcp, c6_installer, p4, in that order; stops at the first failure
make build-test    # the same with the test-only console commands (OTBR_TEST_BUILD=1); never ship it
```

The P4 build embeds the C6 updater, bootloader and partition table (used by the maintenance jumper recovery). Outputs:

| File | What |
| --- | --- |
| `p4/build/p4.bin` | P4 application image |
| `p4/build/web_0.bin` | web UI image (the vendored `frontend/` directory) |
| `c6_rcp/build/c6_rcp.bin` | C6 radio firmware, a separate release artifact |
| `c6_updater/build/` | C6 bootloader, partition table and updater (only needed for the USB-UART first install) |
| `c6_installer/build/c6_installer.bin` | one-time C6 installer for a factory C6 (ESP-Hosted): carries the three `c6_updater` images; its version is the C6 base version |

`make build` builds the default firmware target, `ESP32-P4-WIFI6-POE-ETH` with `ESP32-P4-rev1.3` (chips below v3). For
another one, name both ids: `make build BOARD_ID=ESP32-P4-WIFI6-POE-ETH CHIP_ID=ESP32-P4-rev3.0` (chips v3.1 and later)
builds into its own directory, and `make flash`, `make bundle` and `make ota` take the same two variables.
`make build-release` builds every target.

`make bundle` packs the web UI and the P4 app into `dist/otbr.bundle` for an OTA update (`dist/<board_id>_<chip_id>.ota`
for another target); `make package` collects every release file into `dist/` (see [Development](development.md#ci-and-releases)).

## Versions

Version strings of the P4 app and of `c6_rcp.bin` are `<UTC build timestamp>-<git hash>[-dirty]`. They are taken at CMake
configure time, and a new commit, checkout or reset triggers a reconfigure by itself, so the next build carries a fresh
stamp. Only `-dirty` is as of the last configure: uncommitted edits made after it do not change the stamp
(`make reconfigure` refreshes it).

Releases consist of two independent artifacts with independent versions: the P4 OTA bundle `otbr.bundle` (web UI and P4
app) and `c6_rcp.bin`. The frozen C6 updater (the base image the P4 embeds) is the exception: its version is
content-addressed, `base-<8 hex>` from a hash of the sources that define it (`tools/base_version.cmake`), so it does not
change with the build time or with commits that leave those sources alone (an IDF patch-level upgrade or a local
menuconfig edit of the generated `sdkconfig` does not change it, deliberately).

## Flash

```
make flash      # PORT=/dev/ttyACM0 by default
make monitor
```

This writes bootloader, partition table, application and the web UI image. A board flashed with an older partition layout
(historical: early builds had `rcp_0`/`rcp_1` partitions) needs `make erase` and one full flash over USB; an OTA cannot
move the partition table. Erasing the flash also deletes the stored Thread dataset and network configuration. The C6 is
not touched by a P4 flash: see [First install](first-install.md) for a new board.

Without a checkout, use `full_<board_id>_<chip_id>.zip` of your chip from a release: unpack it and run
`esptool --chip esp32p4 -p /dev/ttyACM0 write-flash @flash_args` inside the directory.
