#!/usr/bin/env bash
# Build and run every host test project (*/test_host/, */*/test_host/ and */*/*/test_host/) on the ESP-IDF linux
# target, HOST_TEST_JOBS projects at a time (default 4). Each project's output goes to a log that is printed only when
# that project fails; every project runs, and the exit status is 1 when any failed.
# idf.py set-target runs only when a project has no linux build yet or one of its sdkconfig.defaults* files is newer
# than its sdkconfig: set-target regenerates sdkconfig and reconfigures from scratch, which costs more than the build.
# Requires an activated ESP-IDF environment.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
jobs="${HOST_TEST_JOBS:-4}"
logs="$(mktemp -d)"
trap 'rm -rf "$logs"' EXIT

needs_set_target() {
    local dir="$1" defaults
    grep -qx 'IDF_TARGET:STRING=linux' "$dir/build/CMakeCache.txt" 2>/dev/null || return 0
    [[ -f "$dir/sdkconfig" ]] || return 0
    for defaults in "$dir"/sdkconfig.defaults*; do
        [[ -f "$defaults" && "$defaults" -nt "$dir/sdkconfig" ]] && return 0
    done
    return 1
}

# Called as `run_project ... || status=$?`, where set -e does not apply, so every step returns on failure itself.
run_project() {
    local dir="$1" elf
    if needs_set_target "$dir"; then
        idf.py -C "$dir" --preview set-target linux || return
    fi
    idf.py -C "$dir" build || return
    elf="$(find "$dir/build" -maxdepth 1 -name '*.elf' | head -n 1)"
    if [[ -z "$elf" ]]; then
        echo "no .elf produced in $dir/build" >&2
        return 1
    fi
    "$elf"
}

projects=()
while IFS= read -r dir; do
    [[ -f "$dir/CMakeLists.txt" ]] && projects+=("$dir")
done < <(find "$root" -mindepth 2 -maxdepth 4 -type d -name test_host \
    -not -path '*/build/*' -not -path '*/managed_components/*' | sort)

if [[ ${#projects[@]} -eq 0 ]]; then
    echo "no test projects found"
    exit 0
fi

# Each project writes its exit status next to its log, so the results do not depend on which job wait -n reaps.
for i in "${!projects[@]}"; do
    while [[ $(jobs -rp | wc -l) -ge $jobs ]]; do
        wait -n || true
    done
    (
        status=0
        run_project "${projects[$i]}" >"$logs/$i.log" 2>&1 || status=$?
        echo "$status" >"$logs/$i.status"
    ) &
done
wait

failed=()
for i in "${!projects[@]}"; do
    name="${projects[$i]#"$root"/}"
    if [[ "$(cat "$logs/$i.status")" == 0 ]]; then
        echo "pass: $name"
    else
        failed+=("$name")
        echo "FAIL: $name"
        sed 's/^/    /' "$logs/$i.log"
    fi
done

if [[ ${#failed[@]} -gt 0 ]]; then
    echo "${#failed[@]} of ${#projects[@]} host test projects failed: ${failed[*]}" >&2
    exit 1
fi
echo "all ${#projects[@]} host test projects passed"
