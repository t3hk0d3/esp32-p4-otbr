#!/usr/bin/env bash
# Build the projects that exist, in order c6_updater, c6_rcp, c6_installer, p4; stop at the first failure.
# Usage: tools/build_all.sh [--test] [--target <board_id>/<chip_id> | --all-targets]
#   --test         enable test-only console commands (macro OTBR_TEST_BUILD=1, via tools/test_build.cmake).
#                  Every run passes OTBR_TEST_BUILD=0|1 explicitly, so it never sticks in the CMake cache.
#                  A project with a sdkconfig.defaults.test (p4: Debug and Verbose logging compiled in) is configured from
#                  its defaults plus that file into its own sdkconfig.test; a release build uses the defaults into
#                  sdkconfig. Both are passed on every run (SDKCONFIG, SDKCONFIG_DEFAULTS), so neither sticks in the cache;
#                  switching rebuilds everything, as OTBR_TEST_BUILD alone already does.
#   --target       build p4 for that firmware target (tools/targets.py list) instead of the default one: its own build
#                  dir and sdkconfig (p4/build_<board_id>_<chip_id>, p4/sdkconfig_<board_id>_<chip_id>), the defaults
#                  of its board and chip, and the ids as -DOTBR_BOARD_ID and -DOTBR_CHIP_ID. The C6 projects do not
#                  depend on the target: they are built once.
#   --all-targets  build p4 for every target, all with the same OTBR_BUILD_STAMP (tools/version.cmake), so that their
#                  versions are equal. A run for one target passes an empty stamp, so a stamp never sticks in the cache.
# A project's sdkconfig (or sdkconfig.test) older than one of the defaults files it is made from is deleted first, so
# that ESP-IDF generates it again: it applies the defaults only to options the existing file does not set, so a changed
# default would not reach an old checkout. Local menuconfig changes are lost then; keep them in the defaults.
# Requires an activated ESP-IDF environment (see IDF_VERSION) and python3 (tools/targets.py).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
usage="usage: $0 [--test] [--target <board_id>/<chip_id> | --all-targets]"
test_build=0
target=""
all_targets=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --test) test_build=1 ;;
        --all-targets) all_targets=1 ;;
        --target)
            [[ $# -ge 2 ]] || { echo "$usage" >&2; exit 2; }
            target="$2"
            shift
            ;;
        *)
            echo "$usage" >&2
            exit 2
            ;;
    esac
    shift
done
if [[ $all_targets == 1 && -n "$target" ]]; then
    echo "$usage" >&2
    exit 2
fi

# Build p4 once for board_id/chip_id ("" is the default target) with the given OTBR_BUILD_STAMP ("" is the configure time).
build_p4() {
    local info key value board_id chip_id build_dir sdkconfig defaults_list stamp="$2"
    # shellcheck disable=SC2086
    info="$("$root/tools/targets.py" resolve ${1/\// })"
    while IFS='=' read -r key value; do
        case "$key" in
            board_id) board_id="$value" ;;
            chip_id) chip_id="$value" ;;
            build_dir) build_dir="$root/$value" ;;
            sdkconfig) sdkconfig="$root/$value" ;;
            defaults) defaults_list="$value" ;;
        esac
    done <<<"$info"
    local defaults=() file
    IFS=';' read -r -a defaults <<<"$defaults_list"
    for i in "${!defaults[@]}"; do
        defaults[i]="$root/${defaults[i]}"
    done
    if [[ $test_build == 1 ]]; then
        sdkconfig="$sdkconfig.test"
        defaults+=("$root/p4/sdkconfig.defaults.test")
    fi
    for file in "${defaults[@]}"; do
        if [[ -f "$sdkconfig" && "$file" -nt "$sdkconfig" ]]; then
            echo "p4: ${file##*/} changed, regenerating ${sdkconfig##*/}"
            rm "$sdkconfig"
        fi
    done
    echo "building p4 $board_id/$chip_id (${sdkconfig##*/})"
    idf.py -C "$root/p4" -B "$build_dir" -DOTBR_TEST_BUILD=$test_build -DOTBR_BUILD_STAMP="$stamp" \
        -DOTBR_BOARD_ID="$board_id" -DOTBR_CHIP_ID="$chip_id" -DSDKCONFIG="$sdkconfig" \
        -DSDKCONFIG_DEFAULTS="$(IFS=';'; echo "${defaults[*]}")" build
}

for project in c6_updater c6_rcp c6_installer; do
    if [[ ! -f "$root/$project/CMakeLists.txt" ]]; then
        echo "skipping $project (not present)"
        continue
    fi
    sdkconfig="$root/$project/sdkconfig"
    defaults=("$root/$project/sdkconfig.defaults")
    if [[ $test_build == 1 && -f "$root/$project/sdkconfig.defaults.test" ]]; then
        sdkconfig="$root/$project/sdkconfig.test"
        defaults+=("$root/$project/sdkconfig.defaults.test")
    fi
    for file in "${defaults[@]}"; do
        if [[ -f "$sdkconfig" && "$file" -nt "$sdkconfig" ]]; then
            echo "$project: ${file##*/} changed, regenerating ${sdkconfig##*/}"
            rm "$sdkconfig"
        fi
    done
    echo "building $project (${sdkconfig##*/})"
    idf.py -C "$root/$project" -DOTBR_TEST_BUILD=$test_build -DSDKCONFIG="$sdkconfig" \
        -DSDKCONFIG_DEFAULTS="$(IFS=';'; echo "${defaults[*]}")" build
done

if [[ ! -f "$root/p4/CMakeLists.txt" ]]; then
    echo "skipping p4 (not present)"
elif [[ $all_targets == 1 ]]; then
    stamp="$(date -u +%Y%m%dT%H%M%SZ)"
    while read -r pair; do
        build_p4 "$pair" "$stamp"
    done < <("$root/tools/targets.py" list)
else
    build_p4 "$target" ""
fi
