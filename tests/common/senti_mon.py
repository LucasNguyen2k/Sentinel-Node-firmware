#!/usr/bin/env python3
"""senti_mon.py - live view of one Sentinel Node for manual tests (1 line per second, Ctrl+C to stop).

  python3 senti_mon.py ac      acoustic: level, background, margin above background, event
  python3 senti_mon.py gas     gas: raw ADC, Rs, ppm, %LEL, warm-up, event
  python3 senti_mon.py all     acoustic + gas
  python3 senti_mon.py time    clock: node time vs this computer, sync flag, timestamp of the latest record
  python3 senti_mon.py store   flash data store: offline buffer, drops, event log, wear, server link

Settings come from environment variables (defaults in brackets):
  SENTI_API [http://192.168.100.1:8080/api/v1]  SENTI_KEY [sentinel-key]  SENTI_NODE [192.168.100.44]
"""
import json
import os
import sys
import time
import urllib.error
import urllib.request

API = os.environ.get("SENTI_API", "http://192.168.100.1:8080/api/v1")
KEY = os.environ.get("SENTI_KEY", "sentinel-key")
NODE = os.environ.get("SENTI_NODE", "192.168.100.44")


def status():
    """Ask the node for STATUS through the Edge Server command API; returns a dict or raises."""
    req = urllib.request.Request(
        "%s/devices/%s/command" % (API, NODE),
        data=json.dumps({"command": "STATUS", "timeout": 5}).encode(),
        headers={"X-API-Key": KEY, "Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=10) as r:
        body = json.load(r)
    if body.get("json") is None:
        raise RuntimeError(body.get("reply") or body.get("detail") or "no STATUS reply")
    return body["json"]


def latest_record():
    req = urllib.request.Request("%s/devices/%s/sensor/latest" % (API, NODE), headers={"X-API-Key": KEY})
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.load(r)


def num(v, fmt):
    return "  n/a" if v is None else format(v, fmt)


def line(s, mode):
    t = time.strftime("%H:%M:%S")
    ev = "%s %s" % (s.get("event_state"), s.get("event_id") or "")
    parts = [t, "rssi=%s" % s.get("rssi")]
    if mode in ("ac", "all"):
        hf, fl = s.get("ac_hf_db"), s.get("ac_floor_db")
        above = None if hf is None or fl is None else hf - fl
        parts += ["hf=%s" % num(hf, "6.1f"), "floor=%s" % num(fl, "6.1f"), "above=%s dB" % num(above, "5.1f")]
    if mode in ("gas", "all"):
        parts += ["raw=%s" % num(s.get("gas_raw"), "5"), "Rs=%sk" % num(s.get("gas_rs_kohm"), "7.2f"),
                  "ppm=%s" % num(s.get("gas_ppm"), "8.1f"), "LEL=%s%%" % num(s.get("gas_lel_pct"), "5.2f"),
                  "warm=%s" % s.get("gas_warm")]
    if mode == "store":
        parts += ["link=%s" % ("up" if s.get("sock_connected") else "DOWN"), "storage=%s" % s.get("storage"),
                  "flash_ok=%s" % s.get("flash_ok"), "buf_pending=%s" % s.get("flash_buf_pending"),
                  "used=%s%%" % s.get("flash_buf_used_pct"), "dropped=%s" % s.get("flash_buf_dropped"),
                  "thinned=%s" % s.get("flash_buf_thinned"), "events=%s" % s.get("flash_evt_records"),
                  "corrupt=%s" % s.get("flash_corrupt"), "max_erase=%s" % s.get("flash_max_erase")]
        return "  ".join(parts)
    if mode == "time":
        now = time.time()
        node_t = s.get("time") or 0
        synced = bool((s.get("valid") or 0) & 64)          # validity bit 6 = time_synced
        diff = "%+.1f s" % (node_t - now) if node_t else "n/a (node clock not set)"
        parts += ["node=%s" % (time.strftime("%H:%M:%S", time.localtime(node_t)) if node_t else "0"),
                  "diff=%s" % diff, "synced=%s" % int(synced), "uptime=%ss" % s.get("uptime_s")]
        try:
            rec = latest_record()
            f = rec.get("fields") or {}
            dts = f.get("device_ts")
            parts += ["record: ts_source=%s" % rec.get("ts_source"),
                      "device_ts=%s" % (time.strftime("%H:%M:%S", time.localtime(dts)) if dts else "0"),
                      "age=%.0fs" % (now - rec.get("ts", now)), "seq=%s" % f.get("device_seq")]
        except (urllib.error.URLError, OSError, ValueError) as e:
            parts.append("record: n/a (%s)" % e)
        return "  ".join(parts)
    parts.append("event=" + ev)
    return "  ".join(parts)


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "all"
    if mode not in ("ac", "gas", "all", "time", "store"):
        print(__doc__)
        return 2
    while True:
        try:
            print(line(status(), mode), flush=True)
        except (urllib.error.URLError, OSError, RuntimeError, ValueError) as e:
            print(time.strftime("%H:%M:%S"), "(no status: %s)" % e, flush=True)
        time.sleep(1)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        pass
