#!/usr/bin/env bash
# Download the vendor 3D model of the ESP32-P4-WIFI6-POE-ETH board, check its hash and extract the STEP file.
# Usage: tools/fetch_board_model.sh (or make case-board)
# Writes .artifacts/board/board.zip and .artifacts/board/board.stp (both git-ignored). A cached zip with the right hash
# is reused. Overrides for tests: BOARD_MODEL_URL, BOARD_MODEL_SHA256, BOARD_MODEL_DIR.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
url="${BOARD_MODEL_URL:-https://files.waveshare.com/wiki/ESP32-P4-WIFI6-POE-ETH/ESP32-P4-WIFI6-POE-ETH-3D.zip}"
expected="${BOARD_MODEL_SHA256:-916430173b5a375567025b9757aa6bb19cf380d75236a5d6fbf0c66650d78e11}"
dir="${BOARD_MODEL_DIR:-$root/.artifacts/board}"
zip="$dir/board.zip"

mkdir -p "$dir"

sha_of() { sha256sum "$1" | cut -d' ' -f1; }

if [[ -f "$zip" && "$(sha_of "$zip")" == "$expected" ]]; then
    echo "cached: $zip"
else
    echo "downloading $url"
    curl -fsSL -o "$zip.part" "$url"
    mv "$zip.part" "$zip"
fi

actual="$(sha_of "$zip")"
if [[ "$actual" != "$expected" ]]; then
    rm -f "$zip"
    echo "sha256 mismatch for $url" >&2
    echo "  expected: $expected" >&2
    echo "  actual:   $actual" >&2
    exit 1
fi

tmp="$(mktemp -d "$dir/unzip.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT
unzip -q -o "$zip" -d "$tmp"
step="$(find "$tmp" -type f -iname '*.stp' -o -type f -iname '*.step' | head -n 1)"
if [[ -z "$step" ]]; then
    echo "no STEP file in $zip" >&2
    exit 1
fi
mv "$step" "$dir/board.stp"
echo "board model: $dir/board.stp"
