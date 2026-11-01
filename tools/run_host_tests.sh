#!/usr/bin/env bash
# Build and run every host test project (*/test_host/, */*/test_host/ and */*/*/test_host/) on the ESP-IDF linux
# target. Stops at the first failure. Requires an activated ESP-IDF environment.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
found=0

while IFS= read -r dir; do
    [[ -f "$dir/CMakeLists.txt" ]] || continue
    found=1
    echo "host test: ${dir#"$root"/}"
    idf.py -C "$dir" --preview set-target linux
    idf.py -C "$dir" build
    elf="$(find "$dir/build" -maxdepth 1 -name '*.elf' | head -n 1)"
    if [[ -z "$elf" ]]; then
        echo "no .elf produced in $dir/build" >&2
        exit 1
    fi
    "$elf"
done < <(find "$root" -mindepth 2 -maxdepth 4 -type d -name test_host \
    -not -path '*/build/*' -not -path '*/managed_components/*' | sort)

if [[ $found -eq 0 ]]; then
    echo "no test projects found"
fi
