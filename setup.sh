#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Duke Nukem 3D 32X - idempotent environment bootstrap.
#
# Installs (only if missing):
#   * Chilly Willy's 32XDK 20220418 (sh-elf-gcc + m68k-elf-gcc, GCC 12.1)
#     into /opt/toolchains/sega
#   * PicoDrive libretro core -> tests/emu/picodrive_libretro.so
#     (headless 32X emulator used by the point-to-point test harness)
#
# The toolchain lives outside the persisted workspace, so re-run this after
# any cold start:   ./setup.sh
# ---------------------------------------------------------------------------
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
GENDEV="${GENDEV:-/opt/toolchains/sega}"

need_pkg() { command -v "$1" >/dev/null 2>&1; }

if ! need_pkg zstd; then
  echo "[setup] installing zstd"
  sudo apt-get install -y -qq zstd >/dev/null 2>&1 || \
    (sudo apt-get update -qq >/dev/null 2>&1 && sudo apt-get install -y -qq zstd >/dev/null 2>&1)
fi

if [ ! -x "$GENDEV/sh-elf/bin/sh-elf-gcc" ]; then
  echo "[setup] downloading 32XDK 20220418"
  tmp="$(mktemp -d)"
  curl -sSL -o "$tmp/32xdk.tar.zst" \
    https://github.com/viciious/32XDK/releases/download/20220418/chillys-sega-devkit-20220418-opt.tar.zst
  sudo tar --zstd -xf "$tmp/32xdk.tar.zst" -C /
  rm -rf "$tmp"
fi
"$GENDEV/sh-elf/bin/sh-elf-gcc" --version | head -1
"$GENDEV/m68k-elf/bin/m68k-elf-gcc" --version | head -1

EMU="$HERE/tests/emu/picodrive_libretro.so"
if [ ! -f "$EMU" ]; then
  echo "[setup] building PicoDrive libretro core (a few minutes)"
  src="${PICO_SRC:-/tmp/picodrive}"
  if [ ! -d "$src" ]; then
    git clone --depth 1 --recurse-submodules --shallow-submodules \
      https://github.com/libretro/picodrive.git "$src"
  fi
  make -C "$src" -f Makefile.libretro platform=unix -j"$(nproc)" >/tmp/picodrive_build.log 2>&1
  mkdir -p "$HERE/tests/emu"
  cp "$src/picodrive_libretro.so" "$EMU"
fi
echo "[setup] PicoDrive core: $EMU"
echo "[setup] done"
