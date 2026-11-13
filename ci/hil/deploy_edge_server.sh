#!/usr/bin/env bash
# deploy_edge_server.sh - install this build's Edge Server (SN Device Driver, HW Interface, sentinel-status) on
# the bench and restart both services. The Jenkins user runs the root-owned /usr/local/sbin/sentinel-deploy
# through sudo (set up by ci/setup_agent.sh); the settings in /etc/sentinel-*.env stay as they are.
set -euo pipefail
since=$(date '+%Y-%m-%d %H:%M:%S')
sudo /usr/local/sbin/sentinel-deploy "$PWD"
for i in $(seq 1 30); do
  code=$(curl -s -o /dev/null -w "%{http_code}" -H "X-API-Key: ${AMB82_API_KEY}" http://127.0.0.1:8080/api/v1/devices || true)
  if [ "$code" = 200 ]; then echo "SN Device Driver up"; break; fi
  if [ "$code" = 401 ] || [ "$code" = 403 ]; then
    echo "SN Device Driver is up but rejects the API key ($code): the Jenkins credential 'sentinel-api-key'" \
         "differs from AMB82_API_KEY in /etc/sentinel-edge.env on this machine (enter it without quotes)" >&2
    exit 1
  fi
  [ "$i" = 30 ] && { echo "SN Device Driver did not come up (last answer: ${code:-none})" >&2; sudo journalctl -u sentinel-edge -n 50 --no-pager || true; exit 1; }
  sleep 1
done
for i in $(seq 1 30); do
  if sudo journalctl -u sentinel-hw-interface --since "$since" --no-pager | grep -q "MQTT connected"; then
    echo "HW Interface up (MQTT connected)"; exit 0
  fi
  sleep 1
done
echo "HW Interface did not connect to MQTT" >&2
sudo journalctl -u sentinel-hw-interface -n 50 --no-pager || true
exit 1
