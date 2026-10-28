#!/usr/bin/env bash
# setup_agent.sh - prepare a bare-metal Jenkins agent (the HIL bench = the Edge Server machine).
# Installs the same toolchain as ci/docker/Dockerfile plus the Edge Server runtime. Idempotent.
#   sudo ci/setup_agent.sh            (run once; re-run after changing pinned versions)
set -euo pipefail
ARDUINO_CLI_VERSION=1.3.1
AMEBA_CORE_VERSION=4.1.0
AWOT_COMMIT=7c508105d2bc9ad390e5ceea6f852f9457b28924
JENKINS_USER=${JENKINS_USER:-jenkins}
export ARDUINO_DIRECTORIES_DATA=/opt/arduino15 ARDUINO_DIRECTORIES_USER=/opt/arduino
export ARDUINO_BOARD_MANAGER_ADDITIONAL_URLS=https://github.com/Ameba-AIoT/ameba-arduino-pro2/raw/main/Arduino_package/package_realtek_amebapro2_index.json

apt-get update
apt-get install -y python3 python3-pip python3-venv g++ git curl bzip2 xz-utils cppcheck
id "$JENKINS_USER" >/dev/null 2>&1 && usermod -aG dialout "$JENKINS_USER"      # USB serial access for flashing

if ! arduino-cli version 2>/dev/null | grep -q "$ARDUINO_CLI_VERSION"; then
  curl -fsSL "https://github.com/arduino/arduino-cli/releases/download/v${ARDUINO_CLI_VERSION}/arduino-cli_${ARDUINO_CLI_VERSION}_Linux_64bit.tar.gz" \
    | tar -xz -C /usr/local/bin arduino-cli
fi
arduino-cli core update-index
arduino-cli core install "realtek:AmebaPro2@${AMEBA_CORE_VERSION}"
mkdir -p /opt/arduino/libraries
cd /opt/arduino/libraries
[ -d aWOT ] || git clone https://github.com/lasselukkari/aWOT
git -C aWOT fetch -q && git -C aWOT checkout -q "$AWOT_COMMIT"

python3 -m venv /opt/sentinel-venv
/opt/sentinel-venv/bin/pip install -q -r "$(dirname "$0")/../server/requirements.txt" -r "$(dirname "$0")/../tests/requirements.txt"
chown -R "$JENKINS_USER" /opt/arduino15 /opt/arduino /opt/sentinel-venv 2>/dev/null || true

cat > /etc/profile.d/sentinel-ci.sh << PROFILE
export ARDUINO_DIRECTORIES_DATA=/opt/arduino15 ARDUINO_DIRECTORIES_USER=/opt/arduino
export ARDUINO_BOARD_MANAGER_ADDITIONAL_URLS=$ARDUINO_BOARD_MANAGER_ADDITIONAL_URLS
export PATH=/opt/sentinel-venv/bin:\$PATH
PROFILE
echo "Agent ready: $(arduino-cli version), core realtek:AmebaPro2@${AMEBA_CORE_VERSION}"
