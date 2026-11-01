#!/usr/bin/env bash
# Run the upstream host tests of esp_ot_br_server (tests/run_host_tests.py). The runner expects the upstream
# repository layout (components/esp_ot_br_server next to examples/basic_thread_border_router/managed_components),
# so the component is copied into a temporary tree of that shape. Needs an activated ESP-IDF environment and a
# populated p4/managed_components (run a p4 build first).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cjson="$root/p4/managed_components/espressif__cjson"
[[ -d "$cjson" ]] || { echo "missing $cjson: build p4 first" >&2; exit 1; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/components/br_core" "$tmp/examples/basic_thread_border_router/managed_components"
cp -r "$root/p4/components/esp_ot_br_server" "$tmp/components/"
# the guard includes br_core_mode.h (a plain enum header)
cp -r "$root/p4/components/br_core/include" "$tmp/components/br_core/"
ln -s "$cjson" "$tmp/examples/basic_thread_border_router/managed_components/espressif__cjson"
python3 "$tmp/components/esp_ot_br_server/tests/run_host_tests.py"
