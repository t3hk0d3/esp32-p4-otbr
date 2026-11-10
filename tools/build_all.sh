#!/usr/bin/env bash
# Build the projects that exist, in order c6_updater, c6_rcp, c6_installer, p4; stop at the first failure.
# Usage: tools/build_all.sh [--test]
#   --test  enable test-only console commands (macro OTBR_TEST_BUILD=1, via tools/test_build.cmake).
#           Every run passes OTBR_TEST_BUILD=0|1 explicitly, so it never sticks in the CMake cache.
#           A project with a sdkconfig.defaults.test (p4: Debug and Verbose logging compiled in) is configured from
#           sdkconfig.defaults plus that file into its own sdkconfig.test; a release build uses sdkconfig.defaults into
#           sdkconfig. Both are passed on every run (SDKCONFIG, SDKCONFIG_DEFAULTS), so neither sticks in the cache;
#           switching rebuilds everything, as OTBR_TEST_BUILD alone already does.
# A project's sdkconfig (or sdkconfig.test) older than one of the defaults files it is made from is deleted first, so
# that ESP-IDF generates it again: it applies the defaults only to options the existing file does not set, so a changed
# default would not reach an old checkout. Local menuconfig changes are lost then; keep them in the defaults.
# Requires an activated ESP-IDF environment (see IDF_VERSION).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
test_build=0

case "${1:-}" in
    "") ;;
    --test) test_build=1 ;;
    *)
        echo "usage: $0 [--test]" >&2
        exit 2
        ;;
esac

for project in c6_updater c6_rcp c6_installer p4; do
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
