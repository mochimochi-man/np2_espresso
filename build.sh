#!/usr/bin/env bash
# Build the np2 espresso PC-98 emulator for ESP32-S3.
#
# REQUIRES ESP-IDF v5.5. This project depends on arduino-esp32 3.3.x, which
# requires "idf <6.2", so it will NOT build on ESP-IDF v6.x.
#
# Usage:
#   . $HOME/esp/esp-idf-v5.5/export.sh   # (optional; this script auto-sources if needed)
#   ./build.sh
set -e
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Auto-activate ESP-IDF if idf.py is not already on PATH.
if ! command -v idf.py >/dev/null 2>&1; then
  for e in "$IDF_PATH/export.sh" "$HOME/esp/esp-idf-v5.5/export.sh" "$HOME/esp/esp-idf/export.sh"; do
    if [ -f "$e" ]; then . "$e" >/dev/null; break; fi
  done
fi
if ! command -v idf.py >/dev/null 2>&1; then
  echo "ERROR: ESP-IDF not found. Install ESP-IDF v5.5, then run:" >&2
  echo "       . \$HOME/esp/esp-idf-v5.5/export.sh" >&2
  exit 1
fi

# Warn (don't hard-fail) if an incompatible IDF major is active.
ver="$(idf.py --version 2>/dev/null || true)"
case "$ver" in
  *v6.*|*v7.*)
    echo "WARNING: '$ver' active, but this project needs ESP-IDF v5.5" >&2
    echo "         (arduino-esp32 3.3.x requires idf <6.2). Build will likely fail." >&2 ;;
esac

idf.py build
