#!/usr/bin/env python3
"""node-gateway emulator: plays the software team's side of the Edge HAL interface.

It subscribes to sentinel/v1/#, validates every message against the JSON Schemas, checks every announced
file in the data area (exists, size, SHA-256, content), checks seq order and event completeness
(parts_total), finds unannounced files (reconciliation scan), and sends commands.

As a tool:  python3 tests/common/node_gateway_emu.py --mqtt-host 127.0.0.1 --data-root /var/lib/sentinel/data-area
"""
import argparse
import csv
import hashlib
import io
import json
import os
import re
import threading
import time
import uuid
import wave

import paho.mqtt.client as mqtt
from jsonschema import Draft202012Validator

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SCHEMA_DIR = os.path.join(REPO, "interface", "schemas", "sentinel-v1")
EVIDENCE = {"CHEM-03": "CHEM-02", "ACO-03": "ACO-02", "ACO-06": "ACO-05", "IMG-03": "IMG-02"}
FINAL = ("done", "refused", "expired")


def _topic_re(pattern):
    return re.compile("^" + re.escape(pattern).replace(r"\+", "[^/]+") + "$")


class NodeGatewayEmu:
    def __init__(self, host, port, data_root, client_id=None):
        self.data_root = os.path.abspath(data_root)
        self.started = time.time()
        index = json.load(open(os.path.join(SCHEMA_DIR, "index.json")))
        self.validators = [(_topic_re(t["topic"]), t["schema"],
                            Draft202012Validator(json.load(open(os.path.join(SCHEMA_DIR, t["schema"] + ".schema.json")))))
                           for t in index["topics"]]
        self.lock = threading.Lock()
        self.messages = []              # (topic, body)
        self.schema_errors = []         # (topic, message)
        self.file_errors = []           # (path, problem)
        self.files = {}                 # path -> body of its "file ready" message
        self.duplicates = 0
        self.seq_errors = []
        self._last_seq = {}
        self.events = {}                # (device_id, event_id, notification dataset) -> {"start":…, "records": set(), "end": body}
        self.results = {}               # command_id -> [states]
        self.retained = {}              # topic -> body (retained messages seen)
        self.mq = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id or "gw-emu-" + uuid.uuid4().hex[:6])
        self.mq.on_message = self._on_message
        self._connected = threading.Event()
        self.mq.on_connect = lambda c, u, f, rc, p: (c.subscribe("sentinel/v1/#", qos=1), self._connected.set())
        self.mq.connect(host, port, keepalive=30)
        self.mq.loop_start()
        if not self._connected.wait(10):
            raise RuntimeError("broker not reachable")

    def close(self):
        self.mq.loop_stop()
        self.mq.disconnect()

    # ------------------------------------------------------------------ intake
    def _on_message(self, client, userdata, msg):
        if "/cmd/" in msg.topic:
            return                               # our own commands
        try:
            body = json.loads(msg.payload)
        except ValueError:
            with self.lock:
                self.schema_errors.append((msg.topic, "not JSON"))
            return
        v = next(((name, val) for rx, name, val in self.validators if rx.match(msg.topic)), None)
        errs = ["no schema for topic"] if v is None else [e.message for e in v[1].iter_errors(body)]
        with self.lock:
            self.messages.append((msg.topic, body))
            if msg.retain:
                self.retained[msg.topic] = body
            for e in errs:
                self.schema_errors.append((msg.topic, e))
            key = (body.get("device_id"), body.get("dataset_id"))
            seq = body.get("seq")
            if isinstance(seq, int):
                last = self._last_seq.get(key)
                if last is not None and seq <= last and not msg.retain:
                    self.seq_errors.append((msg.topic, last, seq))
                self._last_seq[key] = max(seq, last or 0)
        kind = msg.topic.split("/")[3] if msg.topic.count("/") >= 3 else ""
        if kind == "file":
            self._intake_file(body)
        elif kind == "event":
            self._track_event(body)
        elif kind == "cmd_result":
            with self.lock:
                self.results.setdefault(body.get("command_id"), []).append(body)

    def _intake_file(self, body):
        rel = body.get("path", "")
        if rel in self.files and self.files[rel].get("sha256") == body.get("sha256"):
            with self.lock:
                self.duplicates += 1             # a repeat: the ledger ignores it
            return
        problem = self.check_file(body)
        with self.lock:
            self.files[rel] = body
            if problem:
                self.file_errors.append((rel, problem))

    def check_file(self, body):
        p = os.path.join(self.data_root, body.get("path", ""))
        if not os.path.isfile(p):
            return "announced file does not exist"
        data = open(p, "rb").read()
        if len(data) != body.get("size"):
            return "size %d != announced %s" % (len(data), body.get("size"))
        if hashlib.sha256(data).hexdigest() != body.get("sha256"):
            return "SHA-256 mismatch"
        ds, ct = body.get("dataset_id"), body.get("content_type")
        if ct == "text/csv":
            rows = list(csv.reader(io.StringIO(data.decode())))
            if rows[0] != ["time_ms", "temp_c", "rh_pct", "mos_mv"]:
                return "CSV header %s" % rows[0]
            if len(rows) - 1 != body.get("rows"):
                return "CSV has %d rows, announced %s" % (len(rows) - 1, body.get("rows"))
            times = [int(r[0]) for r in rows[1:]]
            if len(set(times)) != len(times):
                return "CSV time_ms not unique"
        elif ct == "audio/wav":
            try:
                with wave.open(io.BytesIO(data)) as w:
                    dur = w.getnframes() / float(w.getframerate())
            except wave.Error as e:
                return "not a WAV: %s" % e
            if body.get("recorded_by", "edge") == "edge":
                want = self.iso(body["recorded_to"]) - self.iso(body["recorded_from"])
                if abs(dur - want) > 0.06:
                    return "WAV lasts %.3f s, window is %.3f s" % (dur, want)
        elif ct == "image/jpeg" and data[:2] != b"\xff\xd8":
            return "not a JPEG"
        if not os.path.basename(p).startswith(body["path"].split("/")[2]):
            return "file name does not start with its date folder (%s)" % ds
        return None

    @staticmethod
    def iso(s):
        from datetime import datetime
        return datetime.fromisoformat(s.replace("Z", "+00:00")).timestamp()

    def _track_event(self, body):
        key = (body.get("device_id"), body.get("event_id") or "cmd:%s" % body.get("command_id"), body.get("dataset_id"))
        with self.lock:
            e = self.events.setdefault(key, {"start": None, "records": set(), "end": None, "states": []})
            n = body.get("notification")
            if n == "start":
                e["start"] = body
            elif n == "record":
                e["records"].add(body.get("part"))
            elif n == "end":
                e["end"] = body
            if body.get("dataset_id") == "CHEM-03":
                e["states"].append(body.get("state"))

    # ------------------------------------------------------------------ checks used by tests
    def wait_for(self, pred, timeout, what="condition", step=0.2):
        end = time.time() + timeout
        while time.time() < end:
            with self.lock:
                v = pred()
            if v:
                return v
            time.sleep(step)
        raise AssertionError("timed out after %.0f s waiting for %s" % (timeout, what))

    def by_topic(self, suffix, device_id=None):
        return [b for t, b in self.messages if t.endswith("/" + suffix) and (device_id is None or b.get("device_id") == device_id)]

    def evidence_complete(self, event_id, notif_dataset, device_id=None):
        """Compare the end notification's parts_total with the files received for that event.
        event_id is the node's own id (E31_4): unique only together with device_id."""
        keys = [k for k in self.events if k[1] == event_id and k[2] == notif_dataset and device_id in (None, k[0])]
        if len(keys) != 1:
            return None
        dev = keys[0][0]
        e = self.events[keys[0]]
        if not e["end"]:
            return None
        file_ds = EVIDENCE.get(notif_dataset)
        parts = {b.get("part") for b in self.files.values() if b.get("event_id") == event_id and b.get("dataset_id") == file_ds
                 and b.get("device_id") == dev and b.get("recorded_by", "edge") == "edge"}
        total = e["end"].get("parts_total")
        return {"expected": total, "received": len(parts), "complete": len(parts) == total and parts == set(range(1, total + 1))}

    def reconcile(self):
        """Files in the data area that were never announced (the software spec's reconciliation scan)."""
        found = []
        for d, _, names in os.walk(self.data_root):
            for n in names:
                if n.endswith(".part"):
                    continue
                full = os.path.join(d, n)
                rel = os.path.relpath(full, self.data_root).replace(os.sep, "/")
                try:
                    older = os.path.getmtime(full) < self.started - 5     # announced before the monitor started
                except OSError:
                    continue
                if rel not in self.files and not older:
                    found.append(rel)
        return found

    def command(self, device_id, name, timeout=60, **payload):
        cid = "c-" + uuid.uuid4().hex[:8]
        payload["command_id"] = cid
        self.mq.publish("sentinel/v1/%s/cmd/%s" % (device_id, name), json.dumps(payload), qos=1)
        self.wait_for(lambda: any(r["state"] in FINAL for r in self.results.get(cid, [])), timeout, "result of " + name)
        with self.lock:
            return cid, list(self.results[cid])

    def summary(self):
        with self.lock:
            return {"messages": len(self.messages), "files": len(self.files), "schema_errors": len(self.schema_errors),
                    "file_errors": len(self.file_errors), "seq_errors": len(self.seq_errors), "duplicates": self.duplicates,
                    "events": len({k[:2] for k in self.events})}


def describe(topic, b):
    """One readable line per message, for --show."""
    ds = b.get("dataset_id", "")
    bits = []
    for k in ("notification", "state", "event_id", "part", "parts_total", "rows", "command", "command_id", "late"):
        if b.get(k) is not None:
            bits.append("%s=%s" % (k, b[k]))
    if ds == "CHEM-01":
        bits.append("values=%s" % b.get("sensor_values"))
        q = b.get("quality", {})
        bits.append("warmed_up=%s calibrated=%s" % (q.get("warmed_up"), q.get("calibrated")))
    if "path" in b:
        bits.append("%s (%s bytes)" % (b["path"].rsplit("/", 1)[-1], b.get("size")))
    if topic.endswith("/birth"):
        bits.append("board=%s fw=%s storage=%s" % (b.get("board_name"), b.get("firmware_version"), b.get("storage")))
    if topic.endswith("/heartbeat"):
        bits.append("online=%s" % b.get("link", {}).get("online"))
    return "%s seq=%s %s" % (topic.split("/", 3)[3], b.get("seq"), " ".join(bits))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--mqtt-host", default="127.0.0.1")
    ap.add_argument("--mqtt-port", type=int, default=1883)
    ap.add_argument("--data-root", required=True)
    ap.add_argument("--every", type=float, default=10, help="seconds between summaries")
    ap.add_argument("--show", action="store_true", help="print every message as it arrives")
    ap.add_argument("--send", metavar="COMMAND", help="send one command (for example capture_image), print its result, exit")
    ap.add_argument("--device", help="device_id for --send, for example SN-00E04C0F5C9D")
    ap.add_argument("--args", default="{}", help="command fields as JSON, for example '{\"count\": 2}'")
    a = ap.parse_args()
    gw = NodeGatewayEmu(a.mqtt_host, a.mqtt_port, a.data_root)
    if a.send:
        if not a.device:
            ap.error("--send needs --device")
        cid, results = gw.command(a.device, a.send, timeout=180, **json.loads(a.args))
        for r in results:
            print(r["state"], r.get("detail") or "", " ".join(r.get("files", [])))
        gw.close()
        return
    shown = 0
    try:
        next_summary = time.time() + a.every
        while True:
            time.sleep(0.5)
            if a.show:
                with gw.lock:
                    new = gw.messages[shown:]
                    shown = len(gw.messages)
                for t, b in new:
                    print(time.strftime("%H:%M:%S"), describe(t, b), flush=True)
            if time.time() >= next_summary:
                next_summary = time.time() + a.every
                print(time.strftime("%H:%M:%S"), "SUMMARY", gw.summary(), flush=True)
                for t, e in gw.schema_errors[-3:]:
                    print("  schema:", t, e)
                for p_, e in gw.file_errors[-3:]:
                    print("  file:", p_, e)
                for (dev, eid, ds), e in list(gw.events.items()):
                    if e["end"] and ds in EVIDENCE:
                        print("  evidence %s %s %s: %s" % (dev, eid, ds, gw.evidence_complete(eid, ds, dev)))
                unann = gw.reconcile()
                if unann:
                    print("  unannounced files:", unann[:5])
    except KeyboardInterrupt:
        gw.close()


if __name__ == "__main__":
    main()
