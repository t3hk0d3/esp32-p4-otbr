#!/usr/bin/env bash
# Flash the ESP32-C6 test devkit with a stock ESP-IDF OpenThread example:
#   rcp  ot_rcp: UART RCP (UART0, 460800 baud) for the TREL test peer border router (tools/trel_peer/up.sh)
#   cli  ot_cli: the Thread test peer of the on-board tests (console on UART0, 115200, `ot` prefix)
# Usage: tools/trel_peer/flash_c6.sh rcp|cli [port]   (port: $PEER_PORT, default /dev/ttyACM1, the UART bridge)
# Needs the ESP-IDF environment (. ~/esp/esp-idf-<version>/export.sh). The example is built in a temporary copy under
# $TMPDIR.
set -euo pipefail

case "${1:-}" in
rcp) example=ot_rcp ;;
cli) example=ot_cli ;;
*)
    echo "usage: $0 rcp|cli [port]" >&2
    exit 2
    ;;
esac
port="${2:-${PEER_PORT:-/dev/ttyACM1}}"
: "${IDF_PATH:?ESP-IDF environment not active: . ~/esp/esp-idf-<version>/export.sh}"

# A directory of its own per run (two runs never share a tree), removed afterwards.
work="$(mktemp -d "${TMPDIR:-/tmp}/c6-${example}.XXXXXX")"
trap 'rm -rf "${work}"' EXIT
cp -r "${IDF_PATH}/examples/openthread/${example}/." "${work}"
cd "${work}"
# Leftovers of a build inside the IDF tree: the lock's relative component paths do not resolve from the copy.
rm -rf build sdkconfig dependencies.lock managed_components
idf.py set-target esp32c6
idf.py -p "${port}" build flash
echo "${example} flashed to ${port}"
