#!/usr/bin/env bash
# Build the projects that exist, in order c6_updater, c6_rcp, c6_migrator, p4; stop at the first failure.
# Usage: tools/build_all.sh [--test]
#   --test  enable test-only console commands (macro OTBR_TEST_BUILD=1, via tools/test_build.cmake).
#           Every run passes OTBR_TEST_BUILD=0|1 explicitly, so it never sticks in the CMake cache.
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

for project in c6_updater c6_rcp c6_migrator p4; do
    if [[ ! -f "$root/$project/CMakeLists.txt" ]]; then
        echo "skipping $project (not present)"
        continue
    fi
    echo "building $project"
    idf.py -C "$root/$project" -DOTBR_TEST_BUILD=$test_build build
done
