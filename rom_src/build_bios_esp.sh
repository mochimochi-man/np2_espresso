#!/usr/bin/env bash
# Assemble BIOS98C into a flat ROM image for the SD card.
#
# The result is dropped in the SD card's root next to the disk images and is
# loaded the same way a real BIOS.ROM is, so it can be swapped with one for an
# A/B comparison.
set -e
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if ! command -v nasm >/dev/null 2>&1; then
  echo "ERROR: nasm not found.  sudo apt install nasm" >&2
  exit 1
fi

nasm -f bin -o BIOS_ESP.ROM bios_esp.asm
ls -l BIOS_ESP.ROM
