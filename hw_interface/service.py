"""HW Interface service.

Follows the SN Device Driver (devices, events, sensor records, status, media folders) and produces the
Phase 1 datasets on the Edge HAL interface: files in the data area, messages on MQTT sentinel/v1.
"""
import glob
import json
import logging
import os
import queue
import re
import threading
import time
from collections import deque
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime

import paho.mqtt.client as mqtt

from .config import CONFIG_PRIVATE, CONFIG_SET_ALLOWED, Settings
from .driver import DriverClient
from .media import AudioIndex, driver_stamp_to_epoch, jpeg_size, wav_to_pcm16
from .store import DataArea, Ledger, utc_iso

log = logging.getLogger("hw_interface")
TOPIC = "sentinel/v1/%s/%s"
ORDER = {"NORMAL": 0, "SUSPECT": 1, "CONFIRMED": 2}
LOG_LINE = re.compile(r"^(\d+),([EM]\d+_\d+),(SUSPECT|CONFIRMED|NORMAL)\b")


def parse_event_log(text):
    """Event-log lines from the node's LOG reply: time,event_id,state[,...] -> [(t, event_id, state)].
    Other lines (headers, messages) are ignored."""
    out = []
    for line in str(text or "").splitlines():
        m = LOG_LINE.match(line.strip())
        if m:
            out.append((int(m.group(1)), m.group(2), m.group(3)))
    return out


def mac_colons(mac):
    m = re.sub(r"[^0-9A-Fa-f]", "", mac or "").upper()
    return ":".join(m[i:i + 2] for i in range(0, 12, 2)) if len(m) == 12 else None


def iso_to_epoch(s):
    if not s:
        return None
    try:
        d = datetime.fromisoformat(str(s).replace("Z", "+00:00"))
        return (d if d.tzinfo else d.astimezone()).timestamp()
    except ValueError:
        return None


class AudioJob:
    """A recording delivered as parts (ACO-02) or one window (ACO-01), cut from the driver's audio."""

    def __init__(self, dev, dataset, start, end, part_s, event=None, command_id=None, trigger="event"):
        self.dev, self.dataset, self.start, self.end, self.part_s = dev, dataset, start, end, part_s
        self.event, self.command_id, self.trigger = event, command_id, trigger
        self.next_start, self.part_no, self.files, self.done = start, 0, [], False
        self.missing = 0


class EventState:
    def __init__(self, dev, node_event_id, start_t, start_device_ms):
        self.dev = dev
        self.node_event_id = node_event_id
        self.event_id = node_event_id
        self.start_t = start_t
        self.chem_part_start = start_device_ms
        self.chem_rows, self.chem_parts = [], 0
        self.chem_end_ms, self.chem_end_deadline, self.chem_done = None, None, False
        self.audio = None
        self.img_parts, self.last_img_t, self.img_pending = 0, start_t, 0
        self.img_inflight = 0               # pictures counted but not yet announced: IMG-03 end waits for them
        self.img_end_sent, self.ended, self.end_t, self.end_late = False, False, None, False


class Device:
    def __init__(self, ip):
        self.ip, self.device_id, self.mac, self.name, self.fw = ip, None, None, None, None
        self.boot_count, self.status, self.status_rx, self.online = None, {}, None, False
        self.config, self.birth_sig = {}, None
        self.rec_cursor, self.last_record = None, None
        self.ms_sec, self.ms_idx = None, 0
        self.last_ms = None
        self.last_chem01_ms, self.chem01_stale_warned = None, False
        self.event = None
        self.last_chem01 = self.last_aco01 = self.last_img01 = 0.0
        self.last_img01_file = None
        self.event_img_parts = {}          # node event id -> pictures published (late pictures too)
        self.late_rows = deque(maxlen=20000)  # records that arrived late (replayed after an outage)
        self.recover_until = self.recover_next = 0.0
        self.recovering = False
        self.last_late_t = 0.0

    @property
    def boot_id(self):
        return "b%d" % (self.boot_count or 0)


class HwInterface:
    def __init__(self, settings: Settings = None):
        self.s = settings or Settings()
        self.ledger = Ledger(self.s.state_db)
        self.area = DataArea(self.s.data_root, self.ledger)
        self.driver = DriverClient(self.s.driver_api, self.s.driver_key, self.s.driver_data_dir)
        self.devices = {}                  # ip -> Device
        self.by_id = {}                    # device_id -> Device
        self.lock = threading.RLock()
        self.jobs = []                     # AudioJob
        self.pending_caps = {}             # node capture id (M..) -> dict(command)
        self.commands = {}                 # command_id -> dict
        self.cmd_queue = queue.Queue()
        self.pool = ThreadPoolExecutor(max_workers=4)
        self.event_cursor = None
        # time of the newest driver event already handled, kept across restarts: after a restart of the HW Interface
        # (or of the driver) the driver's event list is read again from the start, and older events are skipped
        self.event_hwm = float(self.ledger.get("event_hwm", "0"))
        self.stop = threading.Event()
        self._last = {}
        self.mq = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=self.s.mqtt_client_id, clean_session=False,
                              transport="unix" if self.s.mqtt_host.startswith("/") else "tcp")
        if self.s.mqtt_user:
            self.mq.username_pw_set(self.s.mqtt_user, self.s.mqtt_pass)
        self.mq.on_connect = self._on_connect
        self.mq.on_message = self._on_message
        self.connected = threading.Event()

    # ------------------------------------------------------------------ MQTT
    def _on_connect(self, client, userdata, flags, reason_code, properties):
        if reason_code == 0:
            client.subscribe("sentinel/v1/+/cmd/+", qos=1)
            self.connected.set()
            log.info("MQTT connected")

    def _on_message(self, client, userdata, msg):
        parts = msg.topic.split("/")
        if len(parts) == 5 and parts[3] == "cmd":
            self.cmd_queue.put((parts[2], parts[4], msg.payload))

    def publish(self, dev, suffix, body, retain=False):
        self.mq.publish(TOPIC % (dev.device_id, suffix), json.dumps(body, separators=(",", ":")), qos=1, retain=retain)

    def common(self, dev, dataset_id, t, seq_key=None):
        return {"device_id": dev.device_id, "dataset_id": dataset_id, "timestamp": utc_iso(t),
                "seq": self.ledger.next_seq(dev.device_id, seq_key or dataset_id), "boot_id": dev.boot_id,
                "schema_version": self.s.schema_version}

    # ------------------------------------------------------------------ lifecycle
    def start(self):
        if self.s.mqtt_host.startswith("/"):
            self.mq.connect(self.s.mqtt_host, keepalive=30)
        else:
            self.mq.connect(self.s.mqtt_host, self.s.mqtt_port, keepalive=30)
        self.mq.loop_start()
        if not self.connected.wait(10):
            raise RuntimeError("MQTT broker not reachable")
        self.thread = threading.Thread(target=self._run, daemon=True, name="hw-interface")
        self.thread.start()
        return self

    def close(self):
        self.stop.set()
        if getattr(self, "thread", None):
            self.thread.join(10)
        self.pool.shutdown(wait=False, cancel_futures=True)
        self.mq.loop_stop()
        self.mq.disconnect()

    def _due(self, key, every):
        now = time.time()
        if now - self._last.get(key, 0) >= every:
            self._last[key] = now
            return True
        return False

    def _run(self):
        self._first_devices = True
        while not self.stop.is_set():
            try:
                self.tick()
            except Exception:
                log.exception("tick failed")
            self.stop.wait(0.1)

    def tick(self):
        s = self.s
        if self._due("devices", s.poll_devices_s):
            self.refresh_devices()
        if self._due("events", s.poll_events_s):
            self.poll_events()
        if self._due("records", s.poll_records_s):
            for dev in list(self.devices.values()):
                self.poll_records(dev)
        if self._due("status", s.poll_status_s):
            for dev in list(self.devices.values()):
                self.poll_status(dev)
        now = time.time()
        for dev in list(self.devices.values()):
            if dev.device_id is None:
                continue
            if now - dev.last_chem01 >= s.chem01_s and dev.last_record:
                dev.last_chem01 = now
                self.maybe_chem01(dev)
            if now - dev.last_aco01 >= s.aco01_every_s:
                dev.last_aco01 = now
                with self.lock:
                    self.jobs.append(AudioJob(dev, "ACO-01", now, now + s.aco01_len_s, s.aco01_len_s, trigger=None))
            if now - dev.last_img01 >= s.img01_every_s:
                if self.publish_img01(dev):
                    dev.last_img01 = now
            self.event_housekeeping(dev, now)
            if dev.recover_until and now >= dev.recover_next and not dev.recovering:
                dev.recover_next = now + self.s.recovery_every_s
                if now > dev.recover_until:
                    dev.recover_until = 0
                else:
                    dev.recovering = True
                    self.pool.submit(self.recover, dev)
        self.service_audio(now)
        while not self.cmd_queue.empty():
            device_id, name, payload = self.cmd_queue.get()
            self.pool.submit(self.handle_command, device_id, name, payload)
        self.expire_captures(now)
        if self._due("retention", 60 if s.retention_s > 600 else 1):
            n = self.area.cleanup(s.retention_s)
            if n:
                log.info("retention: removed %d files", n)

    # ------------------------------------------------------------------ devices, birth, heartbeat
    def refresh_devices(self):
        try:
            infos = self.driver.devices()
        except Exception as e:
            log.warning("driver not reachable: %s", e)
            for dev in self.devices.values():
                dev.online = False               # back online later = recovery of what was missed
            return
        for info in infos:
            ip = info.get("ip")
            node = info.get("node") or {}
            mac = mac_colons(node.get("node_id"))
            if not ip or not mac:
                continue
            with self.lock:
                dev = self.devices.get(ip)
                if dev is None:
                    dev = self.devices[ip] = Device(ip)
                old_id = dev.device_id
                dev.mac, dev.device_id = mac, "SN-" + mac.replace(":", "")
                dev.name, dev.fw = info.get("name"), node.get("firmware")
                was_online = dev.online
                dev.online = bool((info.get("sensor") or {}).get("online"))
                if dev.online and not was_online:
                    dev.birth_sig = None                 # birth on every connect
                    self.schedule_recovery(dev)
                if old_id and old_id != dev.device_id:
                    self.by_id.pop(old_id, None)
                self.by_id[dev.device_id] = dev
            self.ledger.first_seen(dev.device_id)        # recorded once: the baseline for event-log recovery
            if not dev.config:
                self.fetch_config(dev)
            self.maybe_birth(dev)

    def fetch_config(self, dev):
        r = self.driver.command(dev.ip, "CONFIG", timeout=10)
        if r.get("ok") and r.get("reply"):
            cfg = {}
            for line in str(r["reply"]).splitlines():
                if "=" in line:
                    k, v = line.split("=", 1)
                    cfg[k.strip()] = v.strip()
            dev.config = cfg

    def maybe_birth(self, dev, force=False):
        cfg = {k: self._num(v) for k, v in sorted(dev.config.items()) if k not in CONFIG_PRIVATE}
        storage = self.storage_info(dev)
        sig = json.dumps([dev.name, dev.fw, dev.boot_count, dev.mac, dev.ip, cfg, storage], sort_keys=True)
        if sig == dev.birth_sig and not force:
            return
        dev.birth_sig = sig
        body = self.common(dev, "NODE", time.time())
        body.update({
            "board_name": str(dev.config.get("name") or dev.name or ""),
            "serial_number": "SN-" + dev.mac.replace(":", ""),
            "ip": dev.ip,
            "firmware_version": dev.fw or "",
            "mac_address": dev.mac,
            "capabilities": {"chemical": True, "acoustic_1": True,
                             "acoustic_2_ultrasonic": bool(self.s.ultrasonic_fitted), "image": True},
            "chemical_adc": {"converter": self.s.adc_converter, "bits": self.s.adc_bits, "vref_v": self.s.adc_vref_v,
                             "divider": self.s.adc_divider, "circuit_supply_v": self.s.adc_circuit_supply_v,
                             "resolution_mv": self.s.mos_resolution_mv},
            "config": {k: v for k, v in cfg.items() if k != "name"},
        })
        if storage:
            body["storage"] = storage
        self.publish(dev, "birth", body, retain=True)
        log.info("birth %s (boot %s, fw %s)", dev.device_id, dev.boot_count, dev.fw)

    @staticmethod
    def storage_info(dev):
        """What the node keeps while the server is unreachable (from its status); None before the first status."""
        mb = dev.status.get("flash_mb")
        if mb is None:
            return None
        fs = mb >= 32 and dev.status.get("fs_ok") == 1
        return {"flash_mb": mb, "filesystem": fs, "offline_records_minutes": 2400 if fs else 40,
                "event_log_entries": 2800 if fs else 350, "keeps_event_pictures": fs, "keeps_sound_clips": fs}

    @staticmethod
    def _num(v):
        try:
            f = float(v)
            return int(f) if f.is_integer() and "." not in str(v) else f
        except (TypeError, ValueError):
            return v

    def poll_status(self, dev):
        try:
            st = self.driver.status(dev.ip)
        except Exception:
            return
        status = st.get("status") or {}
        rx = st.get("received_at")
        if not status or rx == dev.status_rx:
            return
        dev.status, dev.status_rx = status, rx
        if status.get("boot_count") is not None and status.get("boot_count") != dev.boot_count:
            dev.boot_count = status.get("boot_count")
            self.fetch_config(dev)
            self.maybe_birth(dev)
        self.maybe_birth(dev)                            # storage block, once the first status is known
        self.publish_heartbeat(dev, online=not st.get("stale", False))

    def publish_heartbeat(self, dev, online):
        st = dev.status
        t_rx = iso_to_epoch(dev.status_rx) or time.time()
        flag = lambda k: (st.get(k) == 1) if st.get(k) is not None else None
        body = self.common(dev, "HEARTBEAT", time.time())
        body.update({
            "link": {"online": bool(online), "rssi_dbm": st.get("rssi"), "wifi_reconnects": st.get("wifi_reconnects")},
            "last_seen": utc_iso(t_rx),
            "uptime_s": st.get("uptime_s"),
            "health": {"chemical_ok": flag("gas_ok"), "acoustic_1_ok": flag("mic_ok"), "image_ok": flag("cam_ok"),
                       "temp_rh_ok": flag("aht_ok"), "selftest_fail_mask": st.get("selftest_fail_mask"),
                       "sample_rate_hz": st.get("sample_rate_hz"), "event_state": st.get("event_state")},
        })
        self.publish(dev, "heartbeat", body)

    # ------------------------------------------------------------------ records, CHEM-01, CHEM-02
    def poll_records(self, dev):
        try:
            r = self.driver.records(dev.ip, dev.rec_cursor if dev.rec_cursor is not None else 0)
        except Exception:
            return
        nxt = r.get("next_after_seq")
        if dev.rec_cursor is not None and nxt is not None and nxt < dev.rec_cursor:
            dev.rec_cursor = 0                   # driver restarted: its seq numbers start again
            return
        for rec in r.get("records", []):
            self.on_record(dev, rec)
        if nxt is not None:
            dev.rec_cursor = nxt

    def on_record(self, dev, rec):
        f = rec.get("fields") or {}
        dts = f.get("device_ts")
        if dts:
            # synthetic milliseconds until the firmware sends real ones: +50 ms per record within a second
            if dts == dev.ms_sec:
                dev.ms_idx = min(dev.ms_idx + 1, 19)
            else:
                dev.ms_sec, dev.ms_idx = dts, 0
            time_ms, synced = int(dts) * 1000 + dev.ms_idx * 50, True
        else:
            time_ms, synced = int((rec.get("ts") or time.time()) * 1000), False
        # unique and increasing: a second with more than 20 records continues at +1 ms; a large jump back
        # (the node's clock was corrected) is taken as it is
        if dev.last_ms is not None and dev.last_ms - 5000 < time_ms <= dev.last_ms:
            time_ms = dev.last_ms + 1
        dev.last_ms = time_ms
        flags = f.get("flags") or {}
        row = {"time_ms": time_ms, "temp_c": f.get("temp_c"), "rh_pct": f.get("rh_pct"),
               "mos_mv": self.s.counts_to_mv(f.get("gas_raw")), "gas_ppm": f.get("gas_ppm"),
               "flags": flags, "synced": synced}
        dev.last_record = row
        if time_ms / 1000.0 < time.time() - self.s.live_lag_s:
            if dev.late_rows.maxlen != self.s.late_rows_max:
                dev.late_rows = deque(dev.late_rows, maxlen=self.s.late_rows_max)
            dev.late_rows.append(row)
            dev.last_late_t = time.time()
        ev = dev.event
        if ev and not ev.chem_done:
            if ev.chem_part_start is None:
                ev.chem_part_start = time_ms
            if time_ms < ev.chem_part_start:
                return                                       # taken before the event started
            if ev.chem_end_ms is not None and time_ms > ev.chem_end_ms:
                self.flush_chem(dev, ev, final=True)
                return
            while time_ms >= ev.chem_part_start + self.s.part_s * 1000:
                self.flush_chem(dev, ev)
            ev.chem_rows.append(row)

    @staticmethod
    def _csv_num(v):
        return "" if v is None else ("%.2f" % v if isinstance(v, float) else str(v))

    def flush_chem(self, dev, ev, final=False):
        start = ev.chem_part_start
        rows = [r for r in ev.chem_rows]
        ev.chem_rows = []
        ev.chem_part_start = start + self.s.part_s * 1000
        if rows:
            ev.chem_parts += 1
            t1 = (rows[-1]["time_ms"] + 50) / 1000.0 if final else (start + self.s.part_s * 1000) / 1000.0
            self.write_chem_part(dev, ev.event_id, ev.chem_parts, rows, start / 1000.0, t1, late=ev.end_late)
        if final:
            ev.chem_done = True
            self.publish_chem03(dev, ev.event_id, "end", "NORMAL", {}, None, False, parts_total=ev.chem_parts,
                                late=ev.end_late, t=ev.end_t)

    def write_chem_part(self, dev, event_id, part, rows, t0, t1, late=False):
        text = "time_ms,temp_c,rh_pct,mos_mv\n" + "".join(
            "%d,%s,%s,%s\n" % (r["time_ms"], self._csv_num(r["temp_c"]), self._csv_num(r["rh_pct"]),
                               "" if r["mos_mv"] is None else "%.1f" % r["mos_mv"]) for r in rows)
        rel = self.area.rel_path(dev.device_id, "CHEM-02", t0, "csv")
        size, sha = self.area.write(rel, text.encode(), dev.device_id, "CHEM-02")
        body = self.common(dev, "CHEM-02", t0)
        body.update({"path": rel, "content_type": "text/csv", "size": size, "sha256": sha, "rows": len(rows),
                     "event_id": event_id, "part": part, "recorded_from": utc_iso(t0), "recorded_to": utc_iso(t1)})
        if late:
            body["late"] = True
        self.publish(dev, "file/CHEM-02", body)

    def maybe_chem01(self, dev):
        """CHEM-01 only from a reading newer than the one published last time: when no new records arrive (the
        driver or the node is down), nothing is published rather than the same old reading again."""
        t = dev.last_record["time_ms"]
        if t == dev.last_chem01_ms:
            if not dev.chem01_stale_warned:
                log.warning("%s CHEM-01: no new reading since %s, not published", dev.device_id, utc_iso(t / 1000.0))
                dev.chem01_stale_warned = True
            return False
        dev.last_chem01_ms, dev.chem01_stale_warned = t, False
        self.publish_chem01(dev)
        return True

    def publish_chem01(self, dev):
        r, st = dev.last_record, dev.status
        flags = r["flags"]
        counts = st.get("gas_raw") if st.get("gas_raw") is not None else None
        mos = self.s.counts_to_mv(counts) if counts is not None else r["mos_mv"]
        sensor_ok = bool(flags.get("gas_ok", st.get("gas_ok") == 1))
        temp_ok = bool(flags.get("aht_ok", st.get("aht_ok") == 1))
        temp, rh = (r["temp_c"], r["rh_pct"]) if temp_ok else (None, None)
        mos = mos if sensor_ok else None
        fmt = lambda v, p: "" if v is None else ("%." + str(p) + "f") % v
        body = self.common(dev, "CHEM-01", r["time_ms"] / 1000.0)
        body.update({
            "sensor_values": "%d,%s,%s,%s" % (r["time_ms"], fmt(temp, 2), fmt(rh, 2), fmt(mos, 1)),
            "fields": {"time_ms": r["time_ms"], "temp_c": temp, "rh_pct": rh, "mos_mv": mos},
            "quality": {"warmed_up": bool(flags.get("gas_warm", st.get("gas_warm") == 1)),
                        "calibrated": bool(flags.get("gas_calibrated", st.get("gas_calibrated") == 1)),
                        "sensor_ok": sensor_ok, "temp_rh_ok": temp_ok,
                        "time_synced": bool(flags.get("time_synced", r["synced"])),
                        "maintenance": bool(flags.get("maintenance", False)),
                        "simulated": bool(flags.get("simulated", False)),
                        "mos_resolution_mv": self.s.mos_resolution_mv},
        })
        ppm = r.get("gas_ppm")
        if body["quality"]["calibrated"] and isinstance(ppm, (int, float)) and ppm >= 0:
            body["gas_ppm"] = ppm
        self.publish(dev, "data/CHEM-01", body)

    # ------------------------------------------------------------------ events from the driver
    def poll_events(self):
        try:
            r = self.driver.events(self.event_cursor if self.event_cursor is not None else 0)
        except Exception:
            return
        nxt = r.get("next_after_seq")
        if self.event_cursor is not None and nxt is not None and nxt < self.event_cursor:
            self.event_cursor = 0                # driver restarted: read its new events from the start
            return
        newest = self.event_hwm
        for e in r.get("events", []):
            ts = float(e.get("ts") or 0)
            if ts and ts <= self.event_hwm:
                continue                                 # handled before a restart
            try:
                self.on_event(e)
            except Exception:
                log.exception("event %s failed", e.get("type"))
            newest = max(newest, ts)
        if newest > self.event_hwm:
            self.event_hwm = newest
            self.ledger.put("event_hwm", repr(newest))
        if nxt is not None:
            self.event_cursor = nxt

    def on_event(self, e):
        dev = self.devices.get(e.get("device_id"))
        if dev is None or dev.device_id is None:
            return
        typ = e.get("type")
        if typ == "leak_event":
            self.on_leak_event(dev, e)
        elif typ == "event_image":
            self.pool.submit(self.on_event_image, dev, e)
        elif typ == "event_audio":
            self.pool.submit(self.on_event_audio, dev, e)
        elif typ in ("node_rebooted", "node_online"):
            self._last["devices"] = 0                    # refresh soon
            dev.config = {}
            dev.birth_sig = None                         # birth on every connect
            self.schedule_recovery(dev)
        elif typ == "node_offline":
            dev.online = False
            if dev.status:
                self.publish_heartbeat(dev, online=False)

    def publish_chem03(self, dev, event_id, notification, state, sensors, mos, maint, parts_total=None, late=False, t=None):
        body = self.common(dev, "CHEM-03", t or time.time())
        body.update({"event_id": event_id, "notification": notification, "state": state,
                     "triggered_by": {"chemical": bool(sensors.get("gas")), "acoustic_1": bool(sensors.get("acoustic")),
                                      "acoustic_2_ultrasonic": bool(sensors.get("ultrasonic"))},
                     "mos_mv": mos, "maintenance": bool(maint), "source": "node_event_log" if late else "live"})
        if late:
            body["late"] = True
        if notification == "end":
            body["parts_total"] = parts_total or 0
        self.publish(dev, "event/CHEM-03", body)

    def notify(self, dev, dataset, event_id, notification, command_id=None, part=None, path=None, size=None, total=None):
        body = self.common(dev, dataset, time.time())
        body.update({"event_id": event_id, "command_id": command_id, "notification": notification})
        if notification == "record":
            body.update({"part": part, "path": path, "size": size})
        if notification == "end":
            body["parts_total"] = total
        self.publish(dev, "event/" + dataset, body)

    def on_leak_event(self, dev, e):
        state, prev, node_id = e.get("state"), e.get("previous_state") or "NORMAL", e.get("event_id")
        t = e.get("ts") or time.time()
        mos = self.s.counts_to_mv(e.get("gas_raw"))
        sensors = e.get("sensors") or {}
        if node_id:
            self.ledger.mark_event(dev.device_id, node_id)
        with self.lock:
            ev = dev.event
            if state in ("SUSPECT", "CONFIRMED") and (ev is None or ev.ended or ev.node_event_id != node_id):
                if ev is not None:
                    self.force_finish(dev, ev, t)
                dts = e.get("device_ts")
                ev = dev.event = EventState(dev, node_id, t, int(dts) * 1000 if dts else None)
                ev.audio = AudioJob(dev, "ACO-02", t, None, self.s.part_s, event=ev, trigger="event")
                self.jobs.append(ev.audio)
                self.publish_chem03(dev, ev.event_id, "start", state, sensors, mos, e.get("maintenance"), t=t)
                self.notify(dev, "ACO-03", ev.event_id, "start")
                self.notify(dev, "IMG-03", ev.event_id, "start")
            elif state in ("SUSPECT", "CONFIRMED") and ORDER[state] > ORDER.get(prev, 0):
                self.publish_chem03(dev, ev.event_id, "record", state, sensors, mos, e.get("maintenance"), t=t)
            elif state == "NORMAL" and ev and not ev.ended and ev.node_event_id == node_id:
                ev.ended, ev.end_t = True, t
                dts = e.get("device_ts")
                ev.chem_end_ms = int(dts) * 1000 if dts else int(t * 1000)
                ev.chem_end_deadline = time.time() + self.s.chem_end_grace_s
                ev.audio.end = t

    def force_finish(self, dev, ev, t):
        """A new event starts before the previous one has delivered everything: close it now."""
        if not ev.ended:
            ev.ended, ev.end_t = True, t
            ev.audio.end = t
        if not ev.chem_done:
            self.flush_chem(dev, ev, final=True)
        if not ev.img_end_sent:
            ev.img_end_sent = True
            self.notify(dev, "IMG-03", ev.event_id, "end", total=ev.img_parts)

    def event_housekeeping(self, dev, now):
        ev = dev.event
        if ev is None:
            return
        with self.lock:
            if not ev.ended and ev.img_pending == 0 and now - ev.last_img_t >= self.s.event_img_every_s:
                ev.last_img_t = now
                self.request_capture(dev, {"kind": "event", "event": ev})
            if ev.ended and not ev.chem_done and now >= ev.chem_end_deadline:
                self.flush_chem(dev, ev, final=True)
            if ev.ended and not ev.img_end_sent and ev.img_pending == 0 and ev.img_inflight == 0:
                if ev.img_parts < self.s.min_event_images:
                    ev.last_img_t = now
                    self.request_capture(dev, {"kind": "event", "event": ev})
                else:
                    ev.img_end_sent = True
                    self.notify(dev, "IMG-03", ev.event_id, "end", total=ev.img_parts)

    def request_capture(self, dev, ctx):
        """Ask the node for one picture; ctx says what it belongs to (event or command)."""
        if ctx.get("kind") == "event":
            ctx["event"].img_pending += 1

        def go():
            try:
                r = self.driver.command(dev.ip, "CAPTURE", timeout=10)
            except Exception as e:                       # driver not reachable: counts as a failed capture
                r = {"ok": False, "reply": None, "detail": str(e)}
            m = re.search(r"OK capture (\S+)", str(r.get("reply") or ""))
            with self.lock:
                if m:
                    ctx["t"] = time.time()
                    self.pending_caps[m.group(1)] = ctx
                elif ctx.get("kind") == "event":
                    ctx["event"].img_pending -= 1
                else:
                    c = self.commands.get(ctx["command_id"])
                    if c and not c.get("finished"):
                        c["finished"] = True
                        self.cmd_result(dev, ctx["command_id"], "capture_image", "refused", c["files"],
                                        str(r.get("reply") or r.get("detail") or "no answer"))
        self.pool.submit(go)

    def expire_captures(self, now):
        with self.lock:
            for cap_id, ctx in list(self.pending_caps.items()):
                if now - ctx["t"] > self.s.capture_timeout_s:
                    self.pending_caps.pop(cap_id)
                    if ctx.get("kind") == "event":
                        ctx["event"].img_pending -= 1
            for cid, c in list(self.commands.items()):
                if c.get("deadline") and now > c["deadline"] and not c.get("finished"):
                    c["finished"] = True
                    self.cmd_result(c["dev"], cid, c["command"], "expired", c.get("files", []), "no result in time")

    def on_event_image(self, dev, e):
        node_id = e.get("event_id") or ""
        data = self.driver.download(e["url"])
        t = e.get("ts") or time.time()
        with self.lock:
            ctx = self.pending_caps.pop(node_id, None)
            ev = dev.event
            if ctx and ctx.get("kind") == "event":
                ev_ = ctx["event"]
                ev_.img_pending -= 1
                event, trigger, cmd_id = ev_, "event", None
            elif ctx and ctx.get("kind") == "command":
                event, trigger, cmd_id = None, "command", ctx["command_id"]
            elif ev and node_id == ev.node_event_id:
                event, trigger, cmd_id = ev, "event", None
            else:
                event, trigger, cmd_id = None, "event" if node_id.startswith("E") else "command", None
            if event is not None:
                event.img_parts += 1
                event.img_inflight += 1
                part, event_id = event.img_parts, event.event_id
            else:
                event_id = (ctx or {}).get("event_id") or (node_id if node_id.startswith("E") else None)
                key = event_id or ("cmd:" + str(cmd_id))
                dev.event_img_parts[key] = dev.event_img_parts.get(key, 0) + 1
                part = dev.event_img_parts[key]
        try:                                             # counted as in flight until announced (or failed)
            w, h = jpeg_size(data)
            rel = self.area.rel_path(dev.device_id, "IMG-02", t, "jpg")
            size, sha = self.area.write(rel, data, dev.device_id, "IMG-02")
            body = self.common(dev, "IMG-02", t)
            body.update({"path": rel, "content_type": "image/jpeg", "size": size, "sha256": sha, "event_id": event_id,
                         "part": part, "trigger": trigger, "command_id": cmd_id, "width": w, "height": h})
            if event is None and node_id.startswith("E") and not self.ledger.event_known(dev.device_id, node_id):
                body["late"] = True                      # a picture the node kept during an outage
            self.publish(dev, "file/IMG-02", body)
            self.notify(dev, "IMG-03", event_id, "record", command_id=cmd_id, part=part, path=rel, size=size)
        finally:
            if event is not None:
                with self.lock:
                    event.img_inflight -= 1
        if ctx and ctx.get("kind") == "command":
            self.command_progress(ctx["command_id"], rel)

    def on_event_audio(self, dev, e):
        """A clip the node recorded itself while the server was unreachable (32 MB boards)."""
        node_id = e.get("event_id") or ""
        data = wav_to_pcm16(self.driver.download(e["url"]))    # the node keeps clips as A-law: PCM like all ACO files
        event_id = node_id or None
        ev = dev.event if dev.event and dev.event.node_event_id == node_id else None
        t0 = (ev.start_t - 2) if ev else (e.get("ts") or time.time()) - 5
        with self.lock:
            key = (event_id or "") + ":node-audio"
            dev.event_img_parts[key] = dev.event_img_parts.get(key, 0) + 1
            part = 1000 + dev.event_img_parts[key]        # node clips are numbered apart from the edge parts
        rel = self.area.rel_path(dev.device_id, "ACO-02", t0, "wav")
        size, sha = self.area.write(rel, data, dev.device_id, "ACO-02")
        body = self.common(dev, "ACO-02", t0)
        body.update({"path": rel, "content_type": "audio/wav", "size": size, "sha256": sha, "event_id": event_id,
                     "part": part, "trigger": "event", "command_id": None, "recorded_from": utc_iso(t0),
                     "recorded_to": utc_iso(t0 + 5), "time_accuracy_ms": 60000, "recorded_by": "node",
                     "late": True})                       # kept by the node during an outage
        self.publish(dev, "file/ACO-02", body)

    # ------------------------------------------------------------------ event-log recovery
    def schedule_recovery(self, dev):
        now = time.time()
        dev.recover_until = now + self.s.recovery_window_s
        dev.recover_next = now + 1.0

    def recover(self, dev):
        """After a node comes back: publish events whose messages were lost while the server was unreachable,
        from the node's own event log, with CHEM-02 built from the replayed records."""
        try:
            last = dev.last_record
            if not last or last["time_ms"] / 1000.0 < time.time() - self.s.live_lag_s:
                return                                   # no live data yet: try again later
            if time.time() - dev.last_late_t < self.s.recovery_quiet_s:
                return                                   # replayed records still arriving: try again later
            try:
                r = self.driver.command(dev.ip, "LOG 50", timeout=15)
            except Exception as e:
                log.warning("recovery of %s: driver not reachable (%s), will retry", dev.device_id, e.__class__.__name__)
                return
            if not r.get("ok"):
                return
            entries = parse_event_log(r.get("reply"))
            first_seen = self.ledger.first_seen(dev.device_id)
            by_id = {}
            for t, eid, state in entries:
                by_id.setdefault(eid, []).append((t, state))
            # an event whose last log line is this recent may still have its live message on the way:
            # leave it to the live path (prevents a race between the log read and the event message)
            settle = time.time() - (self.s.live_lag_s + 2 * self.s.poll_events_s)
            for eid, lines in by_id.items():
                if not eid.startswith("E"):
                    continue
                if max(t for t, _ in lines) > settle:
                    continue                             # looked at again on the next recovery pass
                ends = [t for t, st in lines if st == "NORMAL"]
                ev = dev.event
                if ev and not ev.ended and ev.node_event_id == eid and ends:
                    with self.lock:                      # a live event that ended during the outage
                        ev.ended, ev.end_t, ev.end_late = True, float(ends[-1]), True
                        ev.chem_end_ms = ends[-1] * 1000
                        ev.chem_end_deadline = time.time()
                        ev.audio.end = float(ends[-1])
                    continue
                if self.ledger.event_known(dev.device_id, eid):
                    continue
                if max(t for t, _ in lines) < first_seen - 5:
                    self.ledger.mark_event(dev.device_id, eid)   # ended before this HW Interface knew the node: history
                    continue
                if not ends:
                    continue                             # still running: its next state change arrives live
                self.publish_recovered(dev, eid, lines)
                self.ledger.mark_event(dev.device_id, eid)
        except Exception:
            log.exception("recovery of %s failed", dev.device_id)
        finally:
            dev.recovering = False

    def publish_recovered(self, dev, node_event_id, lines):
        event_id = node_event_id
        starts = [(t, st) for t, st in lines if st != "NORMAL"]
        t_start = starts[0][0] if starts else lines[0][0]
        t_end = [t for t, st in lines if st == "NORMAL"][-1]
        prev = "NORMAL"
        for t, st in lines:
            if st == "NORMAL":
                continue
            self.publish_chem03(dev, event_id, "start" if prev == "NORMAL" else "record", st, {}, None, False,
                                late=True, t=float(t))
            prev = st
        rows = sorted((r for r in dev.late_rows if t_start * 1000 <= r["time_ms"] <= t_end * 1000 + 999),
                      key=lambda r: r["time_ms"])
        part, a = 0, t_start * 1000
        while rows:
            chunk = [r for r in rows if r["time_ms"] < a + self.s.part_s * 1000]
            rows = rows[len(chunk):]
            if chunk:
                part += 1
                t1 = (chunk[-1]["time_ms"] + 50) / 1000.0 if not rows else (a + self.s.part_s * 1000) / 1000.0
                self.write_chem_part(dev, event_id, part, chunk, a / 1000.0, t1, late=True)
            a += self.s.part_s * 1000
        self.publish_chem03(dev, event_id, "end", "NORMAL", {}, None, False, parts_total=part, late=True, t=float(t_end))
        log.info("recovered event %s from the node's event log (%d CHEM-02 parts)", event_id, part)

    # ------------------------------------------------------------------ audio (ACO-01, ACO-02)
    def service_audio(self, now):
        with self.lock:
            jobs = list(self.jobs)
        for job in jobs:
            folder = self.driver.node_folder(job.dev.ip)
            idx = AudioIndex(folder + "/audio") if folder else None
            while not job.done:
                end_known = job.end is not None
                if end_known and job.next_start >= job.end - 0.01:
                    self.finish_job(job)
                    break
                a = job.next_start
                b = a + job.part_s
                if end_known:
                    b = min(b, job.end)
                if b > now:
                    break                                   # the part is not over yet
                if idx and idx.covers(a, b, self.s.audio_max_gap_s):
                    data, gaps_ms = idx.cut(a, b)
                    self.write_audio(job, a, b, data, gaps_ms)
                    job.next_start = b
                elif now > b + self.s.audio_giveup_s:
                    log.warning("%s %s: no audio for %s..%s", job.dev.device_id, job.dataset, utc_iso(a), utc_iso(b))
                    job.next_start = b                       # give up on this part
                    if job.dataset == "ACO-01":
                        job.done = True
                    else:
                        job.part_no += 1                     # 3. the number stays used: the gap shows
                        job.missing += 1
                else:
                    break
        with self.lock:
            self.jobs = [j for j in self.jobs if not j.done]

    def write_audio(self, job, a, b, data, gaps_ms=0):
        dev = job.dev
        rel = self.area.rel_path(dev.device_id, job.dataset, a, "wav")
        size, sha = self.area.write(rel, data, dev.device_id, job.dataset)
        body = self.common(dev, job.dataset, a)
        body.update({"path": rel, "content_type": "audio/wav", "size": size, "sha256": sha,
                     "recorded_from": utc_iso(a), "recorded_to": utc_iso(b),
                     "time_accuracy_ms": self.s.aco_time_accuracy_ms})
        if gaps_ms:
            body["gaps_ms"] = gaps_ms                        # silence inserted where the recording had a gap
        if job.dataset == "ACO-01":
            job.done = True
        else:
            job.part_no += 1
            event_id = job.event.event_id if job.event else getattr(job, "event_id", None)
            body.update({"event_id": event_id, "part": job.part_no, "trigger": job.trigger,
                         "command_id": job.command_id, "recorded_by": "edge"})
        job.files.append(rel)
        self.publish(dev, "file/" + job.dataset, body)      # the file-ready message first (spec 6.2) ...
        if job.dataset != "ACO-01":                          # ... then the notification that refers to it
            self.notify(dev, "ACO-03", event_id, "record", command_id=job.command_id, part=job.part_no, path=rel, size=size)

    def finish_job(self, job):
        job.done = True
        if job.dataset != "ACO-02":
            return
        event_id = job.event.event_id if job.event else getattr(job, "event_id", None)
        self.notify(job.dev, "ACO-03", event_id, "end", command_id=job.command_id, total=job.part_no)
        if job.command_id:
            c = self.commands.get(job.command_id)
            if c and not c.get("finished"):
                c["finished"] = True
                if not job.files:                    # nothing recorded at all: the command could not be carried out
                    self.cmd_result(job.dev, job.command_id, "record_audio", "expired", [],
                                    "no audio recorded (%d parts)" % job.part_no)
                else:
                    self.cmd_result(job.dev, job.command_id, "record_audio", "done", job.files,
                                    "%d of %d parts without audio" % (job.missing, job.part_no) if job.missing else None)

    # ------------------------------------------------------------------ IMG-01
    def publish_img01(self, dev):
        folder = self.driver.node_folder(dev.ip)
        if not folder:
            return False
        files = sorted(glob.glob(os.path.join(folder, "images", "*.jpg")))
        if not files or files[-1] == dev.last_img01_file:
            return False
        p = files[-1]
        dev.last_img01_file = p
        with open(p, "rb") as f:
            data = f.read()
        t = driver_stamp_to_epoch(p) or os.path.getmtime(p)
        w, h = jpeg_size(data)
        rel = self.area.rel_path(dev.device_id, "IMG-01", t, "jpg")
        size, sha = self.area.write(rel, data, dev.device_id, "IMG-01")
        body = self.common(dev, "IMG-01", t)
        body.update({"path": rel, "content_type": "image/jpeg", "size": size, "sha256": sha, "width": w, "height": h})
        self.publish(dev, "file/IMG-01", body)
        return True

    # ------------------------------------------------------------------ commands
    def cmd_result(self, dev, command_id, command, state, files, detail):
        body = self.common(dev, "CMD", time.time())
        body.update({"command_id": command_id, "command": command, "state": state, "files": list(files),
                     "detail": detail})
        self.publish(dev, "cmd_result", body)

    def command_progress(self, command_id, rel):
        with self.lock:
            c = self.commands.get(command_id)
            if not c or c.get("finished"):
                return
            c["files"].append(rel)
            if len(c["files"]) >= c.get("count", 1):
                c["finished"] = True
                self.cmd_result(c["dev"], command_id, c["command"], "done", c["files"], None)

    def handle_command(self, device_id, name, payload):
        dev = self.by_id.get(device_id)
        try:
            p = json.loads(payload or b"{}")
        except ValueError:
            p = {}
        cid = str(p.get("command_id") or "")
        if dev is None:
            log.warning("command %s for unknown device %s", name, device_id)
            return
        if not cid:
            return self.cmd_result(dev, "", name, "refused", [], "command_id missing")
        if cid in self.commands:
            return                                           # repeated message: already handled
        c = self.commands[cid] = {"dev": dev, "command": name, "files": []}
        self.cmd_result(dev, cid, name, "accepted", [], None)
        if not dev.online:
            c["finished"] = True
            return self.cmd_result(dev, cid, name, "expired", [], "node offline")
        if name == "capture_image":
            n = int(p.get("count") or 1)
            if not 1 <= n <= 10:
                c["finished"] = True
                return self.cmd_result(dev, cid, name, "refused", [], "count must be 1..10")
            c.update(count=n, deadline=time.time() + self.s.capture_timeout_s * n + 10)
            for _ in range(n):
                self.request_capture(dev, {"kind": "command", "command_id": cid, "event_id": p.get("event_id")})
                time.sleep(0.5)
        elif name == "record_audio":
            dur = float(p.get("duration_s") or 0)
            if not 0 < dur <= 600:
                c["finished"] = True
                return self.cmd_result(dev, cid, name, "refused", [], "duration_s must be 0..600")
            now = time.time()
            job = AudioJob(dev, "ACO-02", now, now + dur, self.s.part_s, command_id=cid, trigger="command")
            job.event_id = p.get("event_id")
            c.update(job=job, deadline=now + dur + self.s.audio_giveup_s + 30)
            with self.lock:
                self.jobs.append(job)
            self.notify(dev, "ACO-03", job.event_id, "start", command_id=cid)
        elif name == "stop_recording":
            target = self.commands.get(str(p.get("target_command_id") or ""))
            c["finished"] = True
            if not target or not target.get("job") or target["job"].done:
                return self.cmd_result(dev, cid, name, "refused", [], "no running recording with that command_id")
            target["job"].end = min(target["job"].end, time.time())
            self.cmd_result(dev, cid, name, "done", [], None)
        elif name == "config_set":
            key, value = str(p.get("key") or ""), p.get("value")
            c["finished"] = True
            if key not in CONFIG_SET_ALLOWED:
                return self.cmd_result(dev, cid, name, "refused", [], "setting not allowed: %s" % key)
            r = self.driver.command(dev.ip, "SET %s %s" % (key, value))
            if not str(r.get("reply") or "").startswith("OK"):
                return self.cmd_result(dev, cid, name, "refused", [], str(r.get("reply") or r.get("detail")))
            r2 = self.driver.command(dev.ip, "SAVE")
            if not str(r2.get("reply") or "").startswith("OK"):
                return self.cmd_result(dev, cid, name, "refused", [], "SAVE failed: %s" % (r2.get("reply") or r2.get("detail")))
            self.fetch_config(dev)
            self.maybe_birth(dev)
            self.cmd_result(dev, cid, name, "done", [], None)
        else:
            c["finished"] = True
            self.cmd_result(dev, cid, name, "refused", [], "unknown command")
