#!/bin/sh
# sentinel-deploy SRC_DIR - install the Edge Server software of a checked-out build and restart it.
# Installed as /usr/local/sbin/sentinel-deploy (root-owned) by ci/setup_agent.sh; the Jenkins user may run
# exactly this script with sudo (see /etc/sudoers.d/sentinel-edge-ci). Settings in /etc/*.env are not touched.
set -eu
SRC=${1:?usage: sentinel-deploy SRC_DIR}
for f in server/sentinel_node_api.py hw_interface/__main__.py tools/sentinel_status.py; do
  [ -f "$SRC/$f" ] || { echo "sentinel-deploy: $SRC/$f missing" >&2; exit 2; }
done
install -m 0644 "$SRC/server/sentinel_node_api.py" /opt/sentinel-edge/sentinel_node_api.py
rm -rf /opt/sentinel-edge/hw_interface.new
cp -r "$SRC/hw_interface" /opt/sentinel-edge/hw_interface.new
find /opt/sentinel-edge/hw_interface.new -name __pycache__ -type d -prune -exec rm -rf {} +
rm -rf /opt/sentinel-edge/hw_interface.old
[ -d /opt/sentinel-edge/hw_interface ] && mv /opt/sentinel-edge/hw_interface /opt/sentinel-edge/hw_interface.old
mv /opt/sentinel-edge/hw_interface.new /opt/sentinel-edge/hw_interface
install -m 0755 "$SRC/tools/sentinel_status.py" /opt/sentinel-edge/tools/sentinel_status.py
systemctl restart sentinel-edge sentinel-hw-interface
echo "sentinel-deploy: installed from $SRC and restarted sentinel-edge, sentinel-hw-interface"
