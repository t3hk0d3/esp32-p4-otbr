#!/usr/bin/env bash
# Build ziggurat's MCU staticlib (crates/ziggurat-ot of the submodule, --features esp-idf) for the ESP32-P4 and fix it up
# for linking into the firmware. The CMake step in build_ziggurat.cmake runs this.
# Usage: tools/build_staticlib.sh <out dir>
# Writes <out dir>/libziggurat_ot.a and <out dir>/ziggurat.h (cbindgen's header); the scratch (source copy, cargo cache,
# target dir) stays in <out dir> too, so the submodule is never modified.
# Cargo runs on the host when rustup has the target installed for the pinned toolchain (rust-toolchain.toml), else in the
# rust:<channel>-slim Docker image. Needs the riscv32-esp-elf binutils (on PATH with ESP-IDF activated, else from
# $IDF_TOOLS_PATH or ~/.espressif).
# Fixes applied to the archive, each checked:
#   - radium 0.7 (via bitvec) lists the RISC-V targets without 64-bit atomics by name and misses riscv32imafc: a copy
#     with the P4's target added replaces it. The build fails if the edit did not apply.
#   - The target's prebuilt compiler-rt C objects (__bswapsi2, __popcountsi2) are soft-float ABI, which ld refuses next to
#     ESP-IDF's single-float code; libgcc has the same functions, so those members are deleted.
#   - The esp-idf feature allocates through heap_caps_aligned_alloc / heap_caps_realloc / heap_caps_free; they are
#     renamed to zigbee_heap_* (implemented by the zigbee component, which owns the placement policy).
#   - The build fails if the archive still imports any other heap_caps_* or malloc, calloc, realloc, free.
set -euo pipefail

component="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
src="$component/ziggurat"
target=riscv32imafc-unknown-none-elf # the P4's ISA and float ABI (ilp32f), matching ESP-IDF's -mabi

die() {
    echo "build_staticlib.sh: $*" >&2
    exit 1
}

if [[ $# -ne 1 ]]; then
    echo "usage: $0 <out dir>" >&2
    exit 2
fi
out="$(mkdir -p "$1" && cd "$1" && pwd)"
[[ -f "$src/crates/ziggurat-ot/Cargo.toml" ]] || die "the ziggurat submodule is empty: git submodule update --init"

channel="$(sed -n 's/^channel *= *"\(.*\)"/\1/p' "$component/rust-toolchain.toml")"
[[ -n "$channel" ]] || die "no channel in rust-toolchain.toml"

# The riscv32-esp-elf binutils; the host's cannot read RISC-V objects.
tool() {
    local name="riscv32-esp-elf-$1" found="" cand
    if command -v "$name" >/dev/null; then
        command -v "$name"
        return
    fi
    for cand in "${IDF_TOOLS_PATH:-$HOME/.espressif}"/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin/"$name"; do
        [[ -x "$cand" ]] && found="$cand" # the last, i.e. highest version
    done
    [[ -n "$found" ]] || die "$name not found: activate ESP-IDF or install its toolchain"
    echo "$found"
}
ar="$(tool ar)" objcopy="$(tool objcopy)" nm="$(tool nm)" readelf="$(tool readelf)" ranlib="$(tool ranlib)"

# Runs inside the container or on the host, with WORK, CARGO_HOME and CARGO_TARGET_DIR set (and CARGO_TOOLCHAIN, e.g.
# +1.99, on the host).
build_crate() {
    local radium="$CARGO_HOME/radium-0.7.0-p4" original
    cd "$WORK/crates/ziggurat-ot"
    cargo ${CARGO_TOOLCHAIN:+"$CARGO_TOOLCHAIN"} fetch --locked
    set -- "$CARGO_HOME"/registry/src/*/radium-0.7.0
    [[ $# -eq 1 && -d "$1" ]] || { echo "radium-0.7.0 is not in the cargo registry" >&2; return 1; }
    original="$1"
    rm -rf "$radium"
    cp -R "$original" "$radium"
    sed -i 's/| "riscv32imac" |/| "riscv32imac" | "riscv32imafc" |/' "$radium/build.rs"
    grep -q '"riscv32imafc"' "$radium/build.rs" || { echo "the radium patch did not apply" >&2; return 1; }
    # The patch makes cargo rewrite that one Cargo.lock entry (of the copy), hence no --locked on the build.
    cargo ${CARGO_TOOLCHAIN:+"$CARGO_TOOLCHAIN"} build --release --target "$TARGET" --features esp-idf \
        --config "patch.crates-io.radium.path=\"$radium\""
}

# A pristine copy of the source: the build rewrites Cargo.lock and the generated header, which would dirty the submodule.
work="$out/work"
rm -rf "$work"
mkdir -p "$work" "$out/cargo-home"
tar -C "$src" --exclude=.git --exclude=target -cf - . | tar -C "$work" -xf -

cd "$component" # rust-toolchain.toml applies here
if command -v rustup >/dev/null && rustup target list --installed 2>/dev/null | grep -qx "$target"; then
    echo "cargo on the host (toolchain $channel)"
    WORK="$work" CARGO_HOME="$out/cargo-home" CARGO_TARGET_DIR="$out/target" TARGET="$target" CARGO_TOOLCHAIN="+$channel" \
        bash -c "set -euo pipefail; $(declare -f build_crate); build_crate"
else
    echo "cargo in Docker (rust:$channel-slim)"
    command -v docker >/dev/null || die "neither rustup with the $target target nor docker is available"
    docker run --rm -v "$out:/out" -e WORK=/out/work -e CARGO_HOME=/out/cargo-home -e CARGO_TARGET_DIR=/out/target \
        -e TARGET="$target" "rust:$channel-slim" \
        bash -c "set -euo pipefail; trap 'chown -R $(id -u):$(id -g) /out' EXIT; rustup target add $target >/dev/null; $(declare -f build_crate); build_crate"
fi

lib="$out/libziggurat_ot.a"
cp "$out/target/$target/release/libziggurat_ot.a" "$lib"
cp "$work/crates/ziggurat-ot/include/ziggurat.h" "$out/ziggurat.h"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# Deletes the soft-float members of the archive $1, then scans every member again and dies if one is left. Members are
# found by name (ar p, ar d), so a duplicate name would hide one from both: it is fatal. A member readelf cannot read is
# fatal too, as is anything that is not an ELF object.
drop_soft_float() {
    local lib="$1" member names
    names="$("$ar" t "$lib")"
    [[ -z "$(sort <<<"$names" | uniq -d)" ]] || die "archive members share a name: $(sort <<<"$names" | uniq -d | tr '\n' ' ')"
    while read -r member; do
        if is_soft_float "$lib" "$member"; then
            "$ar" d "$lib" "$member"
            echo "dropped soft-float $member"
        fi
    done <<<"$names"
    while read -r member; do
        ! is_soft_float "$lib" "$member" || die "soft-float member left in the archive: $member"
    done < <("$ar" t "$lib")
}

# Whether member $2 of archive $1 is compiled for the soft-float ABI; dies when its ELF header cannot be read.
is_soft_float() {
    local header
    "$ar" p "$1" "$2" >"$tmp/m.o" || die "cannot extract archive member $2"
    header="$("$readelf" -h "$tmp/m.o")" || die "readelf cannot read archive member $2"
    grep -q "soft-float ABI" <<<"$header"
}

drop_soft_float "$lib"

"$objcopy" --redefine-sym heap_caps_aligned_alloc=zigbee_heap_aligned_alloc \
    --redefine-sym heap_caps_realloc=zigbee_heap_realloc \
    --redefine-sym heap_caps_free=zigbee_heap_free "$lib"
"$ranlib" "$lib"

# The imports: symbols some member needs and no member defines. Archive listings carry "member:" header lines.
symbols() { "$nm" --format=just-symbols "$@" "$lib" | grep -v ':$' | sort -u; }
imports="$(comm -23 <(symbols --undefined-only) <(symbols --defined-only))"
echo "imports of $lib:"
echo "$imports" | sed 's/^/  /'
forbidden="$(grep -E '^(heap_caps_.*|malloc|calloc|realloc|free)$' <<<"$imports" || true)"
[[ -z "$forbidden" ]] || die "the archive imports allocator symbols the IDF heap rename leaves out: $(tr '\n' ' ' <<<"$forbidden")"
grep -qx zigbee_heap_aligned_alloc <<<"$imports" || die "the heap rename did not apply (no zigbee_heap_aligned_alloc import)"
echo "built $lib"
