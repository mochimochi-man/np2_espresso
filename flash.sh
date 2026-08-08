#!/usr/bin/env bash
# Flash the firmware and open the serial monitor.
# Usage: ./flash.sh [PORT]     (default port: /dev/ttyACM0)
# Exit the monitor with Ctrl-].
set -e
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="${1:-/dev/ttyACM0}"

if ! command -v idf.py >/dev/null 2>&1; then
  for e in "$IDF_PATH/export.sh" "$HOME/esp/esp-idf-v5.5/export.sh" "$HOME/esp/esp-idf/export.sh"; do
    if [ -f "$e" ]; then . "$e" >/dev/null; break; fi
  done
fi
if ! command -v idf.py >/dev/null 2>&1; then
  echo "ERROR: ESP-IDF not found. Run: . \$HOME/esp/esp-idf-v5.5/export.sh" >&2
  exit 1
fi

idf.py -p "$PORT" flash monitor
