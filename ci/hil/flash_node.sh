#!/usr/bin/env bash
# flash_node.sh BUILD_DIR SERIAL_PORT - flash the CI-built firmware onto the bench node.
# Uses the board's "Auto Flash Mode" so no button press is needed. If the bench board does not enter
# download mode automatically, wire its UART-DOWNLOAD and RESET buttons to a USB relay (see the setup guide).
set -euo pipefail
BUILD_DIR=$1
PORT=$2
FQBN="realtek:AmebaPro2:Ameba_AMB82-MINI:01_AutoUploadMode=Enable"
source /etc/profile.d/sentinel-ci.sh 2>/dev/null || true
for attempt in 1 2 3; do
  if arduino-cli upload -b "$FQBN" -p "$PORT" --input-dir "$BUILD_DIR" firmware/sentinel_node; then
    echo "flashed on attempt $attempt"; exit 0
  fi
  sleep 5
done
echo "flashing failed" >&2
exit 1
