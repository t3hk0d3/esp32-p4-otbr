#!/usr/bin/env bash
# Collect the release artifacts of a finished release build (make build, never make build-test) into one directory.
# Usage: tools/package_release.sh <out dir> (or make package)
# Requires an activated ESP-IDF environment (IDF_PATH, for the third-party license texts).
# Writes:
#   otbr.bundle                  P4 OTA bundle (web UI and P4 app), POST /device/ota
#   c6_rcp.bin                   C6 radio firmware, POST /device/radio/firmware
#   p4-flash.zip                 full P4 USB flash: images and flash_args (esptool --chip esp32p4 write-flash @flash_args)
#   c6-updater-flash.zip         C6 base images for the no-jumper first install (docs/first-install.md, Path 2)
#   third-party-licenses.zip     license texts of the code linked into the images
#   versions.txt, SHA256SUMS
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ $# -ne 1 ]]; then
    echo "usage: $0 <out dir>" >&2
    exit 2
fi
: "${IDF_PATH:?activate the ESP-IDF environment first}"
out="$(mkdir -p "$1" && cd "$1" && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

project_version() {
    python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["project_version"])' \
        "$root/$1/build/project_description.json"
}

# A test build must never be released (build_all.sh records the choice as the OTBR_TEST_BUILD cache entry).
for project in p4 c6_rcp c6_updater; do
    if grep -q '^OTBR_TEST_BUILD:[A-Z]*=1$' "$root/$project/build/CMakeCache.txt"; then
        echo "$project/build is a --test build; rebuild with make build" >&2
        exit 1
    fi
done

# Zip the images named in a project's flash_args, flattened, with a flash_args that uses the flat names.
flash_zip() {
    local project="$1" zip_name="$2" dir="$tmp/$2"
    mkdir -p "$dir"
    {
        head -n 1 "$root/$project/build/flash_args"
        tail -n +2 "$root/$project/build/flash_args" | while read -r offset image; do
            cp "$root/$project/build/$image" "$dir/"
            echo "$offset $(basename "$image")"
        done
    } >"$dir/flash_args"
    cp "$root/LICENSE" "$root/NOTICE" "$dir/"
    (cd "$dir" && zip -q -r "$out/$zip_name.zip" .)
}

# The C6 base images for the no-jumper install, laid out as the P4's ROM recovery writes them: the updater goes into the
# test partition. (The flash_args of the c6_updater build would put it into ota_0, the first RCP slot.)
c6_base_zip() {
    local dir="$tmp/c6-updater-flash" build="$root/c6_updater/build" test_offset
    test_offset="$(awk -F', *' '$1 == "test" {print $4}' "$root/common/c6_board/partitions_c6.csv")"
    [[ -n "$test_offset" ]] || { echo "no test partition in partitions_c6.csv" >&2; exit 1; }
    mkdir -p "$dir"
    cp "$build/bootloader/bootloader.bin" "$build/partition_table/partition-table.bin" "$build/c6_updater.bin" \
        "$root/LICENSE" "$root/NOTICE" "$dir/"
    {
        head -n 1 "$build/flash_args"
        echo "0x0 bootloader.bin"
        echo "0x8000 partition-table.bin"
        echo "$test_offset c6_updater.bin"
    } >"$dir/flash_args"
    (cd "$dir" && zip -q -r "$out/c6-updater-flash.zip" .)
}

python3 "$root/tools/make_bundle.py" --web "$root/p4/build/web_0.bin" --app "$root/p4/build/p4.bin" \
    --out "$out/otbr.bundle"
cp "$root/c6_rcp/build/c6_rcp.bin" "$out/"
flash_zip p4 p4-flash
c6_base_zip

licenses="$tmp/third-party-licenses"
mkdir -p "$licenses"
cp "$root/LICENSE" "$root/NOTICE" "$licenses/"
for entry in \
    esp-idf:LICENSE \
    openthread:components/openthread/openthread/LICENSE \
    lwip:components/lwip/lwip/COPYING \
    mbedtls:components/mbedtls/mbedtls/LICENSE \
    esp_phy-binary-libraries:components/esp_phy/lib/LICENSE \
    esp_coex-binary-libraries:components/esp_coex/lib/LICENSE \
    esp_wifi-binary-libraries:components/esp_wifi/lib/LICENSE; do
    cp "$IDF_PATH/${entry#*:}" "$licenses/${entry%%:*}.txt"
done
cp "$root/p4/components/esp_ot_br_server/LICENSE" "$licenses/esp-thread-br.txt"
for component in "$root"/p4/managed_components/*/; do
    license="$(find "$component" -maxdepth 1 \( -iname 'LICENSE*' -o -iname 'COPYING*' \) | head -n 1)"
    [[ -n "$license" ]] && cp "$license" "$licenses/$(basename "$component").txt"
done
(cd "$licenses" && zip -q -r "$out/third-party-licenses.zip" .)

{
    echo "p4 app:      $(project_version p4)"
    echo "c6_rcp:      $(project_version c6_rcp)"
    echo "c6 base:     $(project_version c6_updater)"
    echo "ESP-IDF:     $(cd "$IDF_PATH" && git describe --tags --always 2>/dev/null || echo unknown)"
} >"$out/versions.txt"

(cd "$out" && sha256sum otbr.bundle c6_rcp.bin p4-flash.zip c6-updater-flash.zip third-party-licenses.zip \
    versions.txt >SHA256SUMS)
cat "$out/versions.txt"
