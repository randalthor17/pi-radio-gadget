#!/usr/bin/env bash
# Cross-builds the daemon for the Pi (static aarch64, musl: runs on any Pi OS, no glibc version issues).
#
#   scripts/cross-build.sh [OUTPUT_DIR]        default: build-aarch64/pi-radiogadget
#
# With nix this needs nothing else. Without nix, put an aarch64 musl cross compiler in CC, e.g.
#   CC=aarch64-linux-musl-gcc scripts/cross-build.sh
# (any aarch64 compiler works with -static; the daemon uses only POSIX and Linux system calls).
# Push the result to a running Pi with:  pi-radiogadget-flash build-aarch64/pi-radiogadget
set -euo pipefail
cd "$(dirname "$0")/.."
out="${1:-build-aarch64}"
mkdir -p "$out"
sources=(src/*.c)
cflags=(-std=gnu11 -O2 -pthread -static -Isrc)

if [ -n "${CC:-}" ]; then
    "$CC" "${cflags[@]}" "${sources[@]}" -o "$out/pi-radiogadget"
else
    command -v nix-shell >/dev/null || { echo "need nix-shell, or set CC to an aarch64 cross compiler" >&2; exit 1; }
    nix-shell -p pkgsCross.aarch64-multiplatform.pkgsStatic.stdenv.cc --run \
        "aarch64-unknown-linux-musl-gcc ${cflags[*]} ${sources[*]} -o $out/pi-radiogadget"
fi
file "$out/pi-radiogadget" 2>/dev/null || ls -l "$out/pi-radiogadget"
