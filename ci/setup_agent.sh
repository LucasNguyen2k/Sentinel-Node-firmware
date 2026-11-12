#!/usr/bin/env bash
# setup_agent.sh - prepare a bare-metal Jenkins agent for the Sentinel Node pipeline. Idempotent.
#
# Two roles, picked from the CPU type unless SENTINEL_AGENT_ROLE is set:
#   build  x86-64 only. Firmware toolchain (arduino-cli + Realtek AmebaPro2 core + aWOT) and the Python
#          test environment. Jenkins labels: docker, x86_64, and sentinel-flash if the bench board's USB
#          is attached to this machine.
#   edge   Any CPU (for example the ARM64 Rubik Pi 3 Edge Server). Python test environment, ffmpeg for the
#          server's audio recordings, and the Edge Server as systemd unit "sentinel-edge" with the sudo
#          rights ci/hil/deploy_edge_server.sh needs. Jenkins label: sentinel-hil.
#          It cannot build or flash firmware: Realtek publishes the AmebaPro2 compiler and upload tools
#          for x86-64 hosts only.
#
#   sudo ci/setup_agent.sh                              (role from the CPU type)
#   sudo SENTINEL_AGENT_ROLE=edge ci/setup_agent.sh     (force a role)
#   DRY_RUN=1 ci/setup_agent.sh                         (print what would be done, change nothing)
set -euo pipefail
ARDUINO_CLI_VERSION=1.3.1
AMEBA_CORE_VERSION=4.1.0
AWOT_COMMIT=7c508105d2bc9ad390e5ceea6f852f9457b28924
JENKINS_USER=${JENKINS_USER:-jenkins}
EDGE_USER=${EDGE_USER:-sentinel}
REPO=$(cd "$(dirname "$0")/.." && pwd)
ARCH=${SENTINEL_ARCH:-$(uname -m)}        # SENTINEL_ARCH only for testing this script
ROLE=${SENTINEL_AGENT_ROLE:-auto}
DRY_RUN=${DRY_RUN:-0}

run() {                                    # run a command, or only print it with DRY_RUN=1
  if [ "$DRY_RUN" = 1 ]; then echo "  would run: $*"; else "$@"; fi
}

case "$ARCH" in
  x86_64|amd64) IS_X86=1 ;;
  *)            IS_X86=0 ;;
esac
if [ "$ROLE" = auto ]; then
  if [ "$IS_X86" = 1 ]; then ROLE=build; else ROLE=edge; fi
fi
case "$ROLE" in
  build|edge) ;;
  *) echo "ERROR: SENTINEL_AGENT_ROLE must be build, edge or auto (got '$ROLE')" >&2; exit 2 ;;
esac
if [ "$ROLE" = build ] && [ "$IS_X86" != 1 ]; then
  cat >&2 << MSG
ERROR: this machine is $ARCH. The build role needs x86-64: Realtek publishes the AmebaPro2 compiler
       and upload tools for x86-64 hosts only, so firmware cannot be built or flashed here.
       Run this script without SENTINEL_AGENT_ROLE (or with SENTINEL_AGENT_ROLE=edge) to set this
       machine up as Edge Server / hardware-test agent, and build on an x86-64 machine.
MSG
  exit 1
fi
if [ "$DRY_RUN" = 1 ]; then MODE=" (dry run: nothing is changed)"; else MODE=""; fi
echo "Sentinel CI agent setup: role=$ROLE, cpu=$ARCH$MODE"

# ---- common: Python test environment ------------------------------------------------------------
run apt-get update
run apt-get install -y python3 python3-pip python3-venv git curl
run python3 -m venv /opt/sentinel-venv
run /opt/sentinel-venv/bin/pip install -q -r "$REPO/server/requirements.txt" -r "$REPO/tests/requirements.txt"

# ---- build role: firmware toolchain (x86-64) ----------------------------------------------------
if [ "$ROLE" = build ]; then
  export ARDUINO_DIRECTORIES_DATA=/opt/arduino15 ARDUINO_DIRECTORIES_USER=/opt/arduino
  export ARDUINO_BOARD_MANAGER_ADDITIONAL_URLS=https://github.com/Ameba-AIoT/ameba-arduino-pro2/raw/main/Arduino_package/package_realtek_amebapro2_index.json
  run apt-get install -y g++ bzip2 xz-utils cppcheck
  if id "$JENKINS_USER" >/dev/null 2>&1; then run usermod -aG dialout "$JENKINS_USER"; fi   # USB serial for flashing
  if ! arduino-cli version 2>/dev/null | grep -q "$ARDUINO_CLI_VERSION"; then
    if [ "$DRY_RUN" = 1 ]; then
      echo "  would install arduino-cli $ARDUINO_CLI_VERSION (Linux_64bit) to /usr/local/bin"
    else
      curl -fsSL "https://github.com/arduino/arduino-cli/releases/download/v${ARDUINO_CLI_VERSION}/arduino-cli_${ARDUINO_CLI_VERSION}_Linux_64bit.tar.gz" \
        | tar -xz -C /usr/local/bin arduino-cli
    fi
  fi
  run arduino-cli core update-index
  run arduino-cli core install "realtek:AmebaPro2@${AMEBA_CORE_VERSION}"
  run mkdir -p /opt/arduino/libraries
  if [ "$DRY_RUN" = 1 ]; then
    echo "  would clone aWOT at $AWOT_COMMIT into /opt/arduino/libraries"
  else
    cd /opt/arduino/libraries
    [ -d aWOT ] || git clone https://github.com/lasselukkari/aWOT
    git -C aWOT fetch -q && git -C aWOT checkout -q "$AWOT_COMMIT"
    cd "$REPO"
  fi
  PROFILE_EXTRA="export ARDUINO_DIRECTORIES_DATA=/opt/arduino15 ARDUINO_DIRECTORIES_USER=/opt/arduino
export ARDUINO_BOARD_MANAGER_ADDITIONAL_URLS=$ARDUINO_BOARD_MANAGER_ADDITIONAL_URLS"
  OWNED="/opt/arduino15 /opt/arduino /opt/sentinel-venv"
fi

# ---- edge role: Edge Server service + rights for the pipeline -----------------------------------
if [ "$ROLE" = edge ]; then
  run apt-get install -y ffmpeg                       # the server records the nodes' audio with ffmpeg
  if ! id "$EDGE_USER" >/dev/null 2>&1; then run useradd --system --create-home --shell /usr/sbin/nologin "$EDGE_USER"; fi
  run install -d -o "$EDGE_USER" -m 0755 /opt/sentinel-edge
  run install -m 0644 "$REPO/server/sentinel_node_api.py" /opt/sentinel-edge/sentinel_node_api.py
  if [ ! -f /etc/sentinel-edge.env ]; then
    if [ "$DRY_RUN" = 1 ]; then
      echo "  would create /etc/sentinel-edge.env (API key placeholder - edit it)"
    else
      printf '%s\n' "# Edge Server settings (see sentinel_node_api.py for all AMB82_* options)" \
        "AMB82_API_KEY=change-me" "AMB82_DATA_DIR=/opt/sentinel-edge/data" > /etc/sentinel-edge.env
      chmod 0640 /etc/sentinel-edge.env && chgrp "$EDGE_USER" /etc/sentinel-edge.env
    fi
  fi
  run install -m 0644 "$REPO/ci/hil/sentinel-edge.service" /etc/systemd/system/sentinel-edge.service
  # HW Interface (Edge HAL): data area + MQTT sentinel/v1. Mosquitto is the bench broker; in production the
  # software team's broker is used instead (set HWI_MQTT_HOST/PORT/USER/PASS in the env file).
  run apt-get install -y mosquitto
  run /opt/sentinel-venv/bin/pip install -q -r "$REPO/hw_interface/requirements.txt"
  run rm -rf /opt/sentinel-edge/hw_interface
  run cp -r "$REPO/hw_interface" /opt/sentinel-edge/hw_interface
  run install -d -o "$EDGE_USER" -m 0755 /var/lib/sentinel /var/lib/sentinel/data-area
  if [ ! -f /etc/sentinel-hw-interface.env ]; then
    if [ "$DRY_RUN" = 1 ]; then
      echo "  would create /etc/sentinel-hw-interface.env (driver key and MQTT placeholders - edit it)"
    else
      printf '%s\n' "# HW Interface settings (all options: hw_interface/config.py)" \
        "HWI_DRIVER_API=http://127.0.0.1:8080/api/v1" "HWI_DRIVER_KEY=change-me" \
        "HWI_DRIVER_DATA_DIR=/opt/sentinel-edge/data" "HWI_DATA_ROOT=/var/lib/sentinel/data-area" \
        "HWI_STATE_DB=/var/lib/sentinel/hw_interface.sqlite" "HWI_MQTT_HOST=127.0.0.1" "HWI_MQTT_PORT=1883" \
        "# chemical ADC of the boards: today internal 10-bit over 3.3 V" "HWI_ADC_BITS=10" "HWI_ADC_VREF_V=3.3" \
        > /etc/sentinel-hw-interface.env
      chmod 0640 /etc/sentinel-hw-interface.env && chgrp "$EDGE_USER" /etc/sentinel-hw-interface.env
    fi
  fi
  run install -m 0644 "$REPO/ci/hil/sentinel-hw-interface.service" /etc/systemd/system/sentinel-hw-interface.service
  # sentinel-status: one-screen health check (Wi-Fi, services, settings, nodes, data, disk, clock)
  run install -d -m 0755 /opt/sentinel-edge/tools
  run install -m 0755 "$REPO/tools/sentinel_status.py" /opt/sentinel-edge/tools/sentinel_status.py
  if [ "$DRY_RUN" = 1 ]; then
    echo "  would install /usr/local/bin/sentinel-status"
  else
    printf '#!/bin/sh\nexec /opt/sentinel-venv/bin/python /opt/sentinel-edge/tools/sentinel_status.py "$@"\n' > /usr/local/bin/sentinel-status
    chmod 0755 /usr/local/bin/sentinel-status
  fi
  run systemctl daemon-reload
  run systemctl enable sentinel-edge sentinel-hw-interface
  if id "$JENKINS_USER" >/dev/null 2>&1; then
    run usermod -aG dialout "$JENKINS_USER"                 # only used if a board's serial port is attached here
    run install -m 0755 -o root -g root "$REPO/ci/hil/sentinel-deploy.sh" /usr/local/sbin/sentinel-deploy
    SUDOERS="$JENKINS_USER ALL=(root) NOPASSWD: /usr/local/sbin/sentinel-deploy, /usr/bin/journalctl -u sentinel-edge *, /usr/bin/journalctl -u sentinel-hw-interface *"
    if [ "$DRY_RUN" = 1 ]; then
      echo "  would write /etc/sudoers.d/sentinel-edge-ci: $SUDOERS"
    else
      # validate a copy first: an invalid file in /etc/sudoers.d would break sudo on the whole machine
      tmp=$(mktemp) && echo "$SUDOERS" > "$tmp" && chmod 0440 "$tmp"
      if visudo -cf "$tmp" >/dev/null; then
        install -m 0440 -o root -g root "$tmp" /etc/sudoers.d/sentinel-edge-ci
      else
        echo "ERROR: the sudoers rule did not validate; /etc/sudoers.d left unchanged" >&2; rm -f "$tmp"; exit 1
      fi
      rm -f "$tmp"
    fi
  fi
  PROFILE_EXTRA=""
  OWNED="/opt/sentinel-venv"
fi

# ---- common: ownership and shell profile --------------------------------------------------------
if [ "$DRY_RUN" = 1 ]; then
  echo "  would give $JENKINS_USER ownership of: $OWNED"
  echo "  would write /etc/profile.d/sentinel-ci.sh (PATH=/opt/sentinel-venv/bin:...)"
else
  # shellcheck disable=SC2086
  chown -R "$JENKINS_USER" $OWNED 2>/dev/null || true
  cat > /etc/profile.d/sentinel-ci.sh << PROFILE
${PROFILE_EXTRA}
export PATH=/opt/sentinel-venv/bin:\$PATH
PROFILE
fi

echo
if [ "$ROLE" = build ]; then
  echo "Build agent ready ($ARCH). Jenkins labels: docker x86_64 (+ sentinel-flash if the bench board's USB is attached here)."
else
  echo "Edge Server / hardware-test agent ready ($ARCH). Jenkins label: sentinel-hil."
  echo "Next: set AMB82_API_KEY in /etc/sentinel-edge.env and the same key as HWI_DRIVER_KEY in /etc/sentinel-hw-interface.env,"
  echo "      then: sudo systemctl restart sentinel-edge sentinel-hw-interface"
  echo "Note: stop any other Edge Server started by hand (for example run_sna.sh): both use ports 8080 and 8900."
fi
