#!/usr/bin/env bash
# deploy_edge_server.sh - install this build's Edge Server on the bench and restart it.
# The bench runs the server as systemd unit "sentinel-edge" (ci/hil/sentinel-edge.service). The Jenkins user
# needs:  jenkins ALL=(root) NOPASSWD: /usr/bin/systemctl restart sentinel-edge, /usr/bin/install
set -euo pipefail
sudo /usr/bin/install -m 0644 server/sentinel_node_api.py /opt/sentinel-edge/sentinel_node_api.py
sudo /usr/bin/systemctl restart sentinel-edge
for i in $(seq 1 30); do
  if curl -fs -H "X-API-Key: ${AMB82_API_KEY}" http://127.0.0.1:8080/api/v1/devices >/dev/null; then
    echo "Edge Server up"; exit 0
  fi
  sleep 1
done
echo "Edge Server did not come up" >&2
sudo journalctl -u sentinel-edge -n 50 --no-pager || true
exit 1
