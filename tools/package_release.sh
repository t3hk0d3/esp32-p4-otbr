#!/usr/bin/env bash
# Collect the release artifacts of a finished release build (make build-release, never make build-test) into one directory.
# Usage: tools/package_release.sh <out dir> (or make package)
# Requires an activated ESP-IDF environment (IDF_PATH, for the third-party license texts).
# Writes:
#   <board_id>_<chip_id>.ota     P4 OTA bundle (web UI and P4 app) of each firmware target, POST /device/ota
#   full_<board_id>_<chip_id>.zip full P4 USB flash of each target: images and flash_args (esptool write-flash @flash_args)
#   otbr.bundle                  copy of the default target's .ota: released firmware downloads it by this name
#   c6_rcp.bin                   C6 radio firmware, POST /device/radio/firmware
#   c6_installer.bin             one-time C6 installer for a factory C6 (ESP-Hosted), POST /device/radio/firmware before
#                                c6_rcp.bin; its version is the c6 base version
#   p4-flash.zip                 copy of the default target's full_*.zip
#   c6-updater-flash.zip         C6 base images for the USB-UART first install (docs/first-install.md, Path 3)
#   third-party-licenses.zip     license texts of the code linked into the images
#   versions.txt, SHA256SUMS     versions.txt starts with "release: <tag>" when RELEASE_TAG is set (CI sets it on a v* tag)
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ $# -ne 1 ]]; then
    echo "usage: $0 <out dir>" >&2
    exit 2
fi
: "${IDF_PATH:?activate the ESP-IDF environment first}"
# The board downloads the release's files by this tag: 1 to 32 characters from [A-Za-z0-9._-], checked before any output.
release_tag="${RELEASE_TAG:-}"
if [[ -n "$release_tag" && ! "$release_tag" =~ ^[A-Za-z0-9._-]{1,32}$ ]]; then
    echo "RELEASE_TAG '$release_tag' is not 1 to 32 characters of [A-Za-z0-9._-]" >&2
    exit 1
fi

# Every firmware target of the registry, one per line: <board_id>/<chip_id>, build dir, sdkconfig, revision range (the
# ESP-IDF numbers the flasher picks an image by) and whether it is the default target. Paths are relative to the root.
mapfile -t targets < <(python3 -I - "$root" <<'PY'
import sys
sys.path.insert(0, sys.argv[1] + "/tools")
import targets

data = targets.load()
for pair in targets.pairs(data):
    board, chip = pair.split("/")
    files = targets.resolve(data, board, chip)
    default = int(data["default"] == {"board": board, "chip": chip})
    print(pair, files["build_dir"], files["sdkconfig"], data["chips"][chip]["min_rev"], data["chips"][chip]["max_rev"],
          default, sep="\t")
PY
)
[[ ${#targets[@]} -gt 0 ]] || { echo "no firmware targets in the registry" >&2; exit 1; }

project_version() { # <build dir>
    python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["project_version"])' \
        "$1/project_description.json"
}

# All checks run before any output is written. A test build must never be released (build_all.sh records the choice as
# the OTBR_TEST_BUILD cache entry).
build_dirs=("c6_rcp/build" "c6_updater/build" "c6_installer/build")
for entry in "${targets[@]}"; do
    IFS=$'\t' read -r pair build_dir _ <<<"$entry"
    if [[ ! -f "$root/$build_dir/project_description.json" ]]; then
        echo "$pair has no build in $build_dir; run make build-release" >&2
        exit 1
    fi
    build_dirs+=("$build_dir")
done
for build_dir in "${build_dirs[@]}"; do
    [[ -f "$root/$build_dir/CMakeCache.txt" ]] || { echo "$build_dir has no build; run make build-release" >&2; exit 1; }
    if grep -q '^OTBR_TEST_BUILD:[A-Z]*=1$' "$root/$build_dir/CMakeCache.txt"; then
        echo "$build_dir is a --test build; rebuild with make build-release" >&2
        exit 1
    fi
done

# The flasher picks an image by the chip's revision range in the registry: the sdkconfig must agree. Every P4 build of
# a release carries one version, else a board would always find the release older or newer than itself.
p4_version=""
for entry in "${targets[@]}"; do
    IFS=$'\t' read -r pair build_dir sdkconfig min_rev max_rev _ <<<"$entry"
    have_min="$(sed -n 's/^CONFIG_ESP32P4_REV_MIN_FULL=//p' "$root/$sdkconfig" 2>/dev/null)"
    have_max="$(sed -n 's/^CONFIG_ESP32P4_REV_MAX_FULL=//p' "$root/$sdkconfig" 2>/dev/null)"
    if [[ "$have_min" != "$min_rev" || "$have_max" != "$max_rev" ]]; then
        echo "$pair: $sdkconfig has revisions ${have_min:-?} to ${have_max:-?}, the registry says $min_rev to $max_rev" >&2
        exit 1
    fi
    version="$(project_version "$root/$build_dir")"
    p4_version="${p4_version:-$version}"
    if [[ "$version" != "$p4_version" ]]; then
        echo "$pair is version $version, the targets before it $p4_version; rebuild with make build-release" >&2
        exit 1
    fi
done

out="$(mkdir -p "$1" && cd "$1" && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
# zip adds to an existing archive: the zips are built in the empty $tmp (no stale files on a re-run) and moved to $out
# together once all of them exist, so a failed run leaves the previous ones in place.

# Zip the images named in a build directory's flash_args, flattened, with a flash_args that uses the flat names.
flash_zip() { # <build dir> <zip name>
    local build="$1" zip_name="$2" dir="$tmp/$2"
    mkdir -p "$dir"
    {
        head -n 1 "$build/flash_args"
        tail -n +2 "$build/flash_args" | while read -r offset image; do
            cp "$build/$image" "$dir/"
            echo "$offset $(basename "$image")"
        done
    } >"$dir/flash_args"
    cp "$root/LICENSE" "$root/NOTICE" "$dir/"
    (cd "$dir" && zip -q -r "$tmp/$zip_name.zip" .)
}

# The C6 base images for the USB-UART install, laid out as the P4's ROM recovery writes them: the updater goes into the
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
    (cd "$dir" && zip -q -r "$tmp/c6-updater-flash.zip" .)
}

# Each target's bundle goes into $tmp with its zip and moves into $out with them; otbr.bundle and p4-flash.zip are
# copies of the default target's.
files=()
for entry in "${targets[@]}"; do
    IFS=$'\t' read -r pair build_dir _ _ _ default <<<"$entry"
    name="${pair/\//_}"
    python3 "$root/tools/make_bundle.py" --web "$root/$build_dir/web_0.bin" --app "$root/$build_dir/p4.bin" \
        --out "$tmp/$name.ota"
    flash_zip "$root/$build_dir" "full_$name"
    files+=("$name.ota" "full_$name.zip")
    if [[ "$default" == 1 ]]; then
        cp "$tmp/$name.ota" "$tmp/otbr.bundle"
        cp "$tmp/full_$name.zip" "$tmp/p4-flash.zip"
    fi
done
[[ -f "$tmp/otbr.bundle" ]] || { echo "the registry's default target is not a listed target" >&2; exit 1; }
cp "$root/c6_rcp/build/c6_rcp.bin" "$root/c6_installer/build/c6_installer.bin" "$out/"
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
cp "$root/p4/components/zigbee/ziggurat/LICENSE.md" "$licenses/ziggurat.txt" # the submodule linked into the P4 image
for component in "$root"/p4/managed_components/*/; do
    license="$(find "$component" -maxdepth 1 \( -iname 'LICENSE*' -o -iname 'COPYING*' \) | head -n 1)"
    [[ -n "$license" ]] && cp "$license" "$licenses/$(basename "$component").txt"
done
(cd "$licenses" && zip -q -r "$tmp/third-party-licenses.zip" .)
for file in "${files[@]}"; do mv -f "$tmp/$file" "$out/"; done
mv -f "$tmp/otbr.bundle" "$tmp/p4-flash.zip" "$tmp/c6-updater-flash.zip" "$tmp/third-party-licenses.zip" "$out/"

{
    [[ -z "$release_tag" ]] || echo "release:     $release_tag"
    echo "p4 app:      $p4_version"
    echo "c6_rcp:      $(project_version "$root/c6_rcp/build")"
    echo "c6 base:     $(project_version "$root/c6_updater/build")"
    echo "ESP-IDF:     $(cd "$IDF_PATH" && git describe --tags --always 2>/dev/null || echo unknown)"
} >"$out/versions.txt"

(cd "$out" && sha256sum "${files[@]}" otbr.bundle c6_rcp.bin c6_installer.bin p4-flash.zip c6-updater-flash.zip \
    third-party-licenses.zip versions.txt >SHA256SUMS)
cat "$out/versions.txt"
