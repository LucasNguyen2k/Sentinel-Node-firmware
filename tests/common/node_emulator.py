"""
node_emulator.py - protocol-level emulator of a Sentinel Node running firmware 1.3.x.

It speaks exactly the wire protocol of the firmware (design doc section 6) so the Edge Server can
be tested without hardware, and so the hardware-in-the-loop suite can be dry-run in CI:

  * GET /add-device, /node-status, /node-event; POST / with an event JPEG (multipart envelope)
  * data socket to <server>:4000 + last IP octet: 11-field records "a,b,..._a,b,...$",
    commands "CMD#<id> <cmd>\\n" answered "#RSP#<id> <reply>$" (untagged for fw 1.1.0)
  * leak-event state machine for SIM GAS / SIM AC (SUSPECT -> NORMAL after the hold time)
  * commands PING STATUS CONFIG SET SAVE TIME SIM MAINT POWER LOG SELFTEST CAPTURE OTA_SD REBOOT HELP

Several emulators can run at once: each binds its own loopback address (127.0.0.x), so the server
sees different node IPs and opens one sensor port per node.
"""
import http.client
import json
import socket
import threading
import time
import urllib.parse

HELP = ("PING STATUS CONFIG SET <k> <v> SAVE RESET_CONFIG CAPTURE CAL_GAS CAL_AC SELFTEST MAINT <0|1> "
        "POWER <0|1> SIM <GAS|AC|CLEAR> [s] TIME <epoch> LOG [n] OTA_SD [FORCE] REBOOT")


class NodeEmulator:
    def __init__(self, ip="127.0.0.44", server="127.0.0.1", reg_port=8900, name="EZ_EMU", mac="00E04C000044",
                 fw="1.3.1", hb_s=2.0, upload_s=0.5, event_hold_s=3.0, ota_capable=False, tagged=True,
                 send_records=True, flash_mb=16):
        self.ip, self.server, self.reg_port = ip, server, reg_port
        self.name, self.mac, self.fw = name, mac, fw
        self.hb_s, self.upload_s, self.event_hold_s = hb_s, upload_s, event_hold_s
        self.ota_capable, self.tagged, self.send_records = ota_capable, tagged, send_records
        self.sock_port = 4000 + int(ip.split(".")[3])
        self.cfg = {"name": name, "sample_ms": "50", "upload_ms": "900", "hb_s": str(int(hb_s)), "maint": "0",
                    "power": "0", "pass": "secret-wifi", "token": "secret-token", "server": server}
        self.boot_count = 1
        self.last_reset = "unexpected(power/wdt/brownout)"
        self.clock_offset = None          # epoch - monotonic, set by T= / CMD TIME
        self.seq = 0
        self.event_state = "NORMAL"
        self.event_id = ""
        self.event_counter = 0
        self.sim_gas_until = 0.0
        self.sim_ac_until = 0.0
        self.last_alarm = 0.0
        self.sock = None
        self.sock_lock = threading.Lock()
        self.sock_reconnects = 0
        self.registered = False
        self.received_commands = []
        self.http_log = []
        self.event_log = []
        self.ota_result = "none"
        self._stop = threading.Event()
        self._threads = []
        self._t0 = time.monotonic()
        self.flash_mb = flash_mb
        self.sim_offline_until = 0.0      # SIM OFFLINE (firmware >= 1.5.2)
        self.buffer = []                  # records kept while "offline"
        self.pending_pics = []            # pictures not yet delivered
        self.pending_clips = []           # event sound clips recorded while "offline" (firmware >= 1.6.0)
        self.clips_kept = 0
        self.clips_saved = 0
        self.pics_kept = 0
        self._abandoned = []              # sockets left open by an emulated hard reset
        self.boot_s = 1.5                 # emulated boot time after a reset

    # ------------------------------------------------------------------ time
    def epoch(self):
        return 0 if self.clock_offset is None else int(time.monotonic() + self.clock_offset)

    def _set_clock(self, epoch):
        if epoch >= 1600000000:
            self.clock_offset = epoch - time.monotonic()

    # ------------------------------------------------------------------ HTTP to the server (port 8900)
    def http(self, method, path, body=None, headers=None):
        c = http.client.HTTPConnection(self.server, self.reg_port, timeout=5, source_address=(self.ip, 0))
        try:
            c.request(method, path, body=body, headers=headers or {})
            r = c.getresponse()
            data = r.read().decode(errors="replace")
        finally:
            c.close()
        self.http_log.append((method, path.split("?")[0], r.status))
        if r.status == 200 and "T=" in data:
            try:
                self._set_clock(int(data.split("T=")[1].split()[0]))
            except ValueError:
                pass
        return r.status, data

    def register(self):
        q = urllib.parse.urlencode({"ip": self.ip, "name": self.name, "id": self.mac, "fw": self.fw,
                                    "boot": self.boot_count})
        st, _ = self.http("GET", "/add-device?" + q)
        self.registered = st == 200
        return st

    def offline(self):
        return time.monotonic() < self.sim_offline_until

    def status_fields(self):
        f = {"name": self.name, "id": self.mac, "fw": self.fw, "ip": self.ip, "uptime_s": int(time.monotonic() - self._t0),
                "time": self.epoch(), "boot_count": self.boot_count, "last_reset": self.last_reset,
                "heap": 72800000, "heap_min": 72790000, "rssi": -35, "wifi_reconnects": 0,
                "sock_connected": 1 if self.sock else 0, "sock_reconnects": self.sock_reconnects,
                "registered": 1 if self.registered else 0, "power_mode": "ECO" if self.cfg["power"] == "1" else "ACTIVE",
                "maint": int(self.cfg["maint"]), "int_temp_raw": 105, "int_temp_c": "null", "amb_temp_c": 24.5,
                "rh_pct": 60.1, "aht_ok": 1, "gas_raw": 161, "gas_rs_kohm": 53.4, "gas_ppm": -1.0, "gas_lel_pct": -1.0,
                "gas_ok": 1, "gas_calibrated": 0, "gas_warm": 1, "mic_ok": 1, "ac_db": -43.0, "ac_hf_db": -57.0,
                "ac_floor_db": -56.0, "us": "null", "battery": "null", "tamper": "null", "cam_ok": 1, "captures": 0,
                "storage": "flash", "flash_ok": 1, "flash_buf_pending": 0, "flash_buf_used_pct": 0,
                "flash_buf_dropped": 0, "flash_buf_thinned": 0, "flash_evt_records": len(self.event_log),
                "flash_corrupt": 0, "flash_max_erase": 3, "sd_ok": 0, "sd_buffered": 0, "sd_dropped": 0, "event_state": self.event_state, "event_id": self.event_id,
                "thermal_derate": 0, "selftest_fail_mask": 0, "ota": self.ota_result,
                "ota_capable": 1 if self.ota_capable else 0, "valid": 83,
                "sim_offline": 1 if self.offline() else 0, "flash_mb": self.flash_mb,
                "sample_rate_hz": 20.0, "up_cycle_ms": int(self.upload_s * 1000)}
        f["storage"] = "flash"
        if self.flash_mb >= 32:
            f.update({"fs_ok": 1, "fs_used_kb": 8, "fs_total_kb": 16192, "fs_img": self.pics_kept,
                      "fs_pend_img": len(self.pending_pics), "fs_buf_pending": len(self.buffer), "fs_buf_dropped": 0,
                      "fs_aud": self.clips_kept, "fs_aud_pend": len(self.pending_clips),
                      "clips_saved": self.clips_saved, "clips_skipped": 0})
        else:
            f["flash_buf_pending"] = len(self.buffer)
        return f

    def heartbeat(self):
        return self.http("GET", "/node-status?" + urllib.parse.urlencode(self.status_fields()))

    def post_image(self, event_id, jpeg=None):
        if self.offline():
            self.pending_pics.append(event_id)                # like the firmware: kept for a later upload
            return 0, ""
        st = self._post_image(event_id, jpeg)
        if st[0] == 200 and self.flash_mb >= 32:
            self.pics_kept += 1
        return st

    @staticmethod
    def clip_wav(seconds=5.0, rate=16000):
        """A 5 s 8-bit A-law mono WAV, like the firmware's event sound clips."""
        import struct
        n = int(seconds * rate)
        data = bytes((0xD5 ^ (i & 0x0F)) for i in range(n))
        return (b"RIFF" + struct.pack("<I", 36 + n) + b"WAVEfmt " +
                struct.pack("<IHHIIHH", 16, 6, 1, rate, rate, 1, 8) + b"data" + struct.pack("<I", n) + data)

    def post_clip(self, event_id):
        if self.offline():
            return 0, ""
        b = "----AMB82_Boundary"
        body = ("--" + b + "\r\nContent-Disposition: form-data\r\n\r\n").encode() + self.clip_wav() + ("\r\n--" + b + "--\r\n").encode()
        st = self.http("POST", "/", body=body, headers={"IP": self.ip, "Device": self.name, "Event": event_id,
                                                        "Content-Length": str(len(body)), "Type": "audio/wav"})
        if st[0] == 200:
            self.clips_kept += 1
        return st

    def _post_image(self, event_id, jpeg=None):
        jpeg = jpeg or (b"\xff\xd8\xff\xe0" + b"EMU" * 2000 + b"\xff\xd9")
        b = "----AMB82_Boundary"
        body = ("--" + b + "\r\nContent-Disposition: form-data\r\n\r\n").encode() + jpeg + ("\r\n--" + b + "--\r\n").encode()
        return self.http("POST", "/", body=body, headers={"IP": self.ip, "Device": self.name, "Event": event_id,
                                                          "Content-Length": str(len(body)), "Type": "image/jpeg"})

    # ------------------------------------------------------------------ data socket
    def connect_socket(self):
        s = socket.create_connection((self.server, self.sock_port), timeout=5, source_address=(self.ip, 0))
        s.settimeout(0.2)
        with self.sock_lock:
            self.sock = s
        self.sock_reconnects += 1

    def send(self, data: str):
        with self.sock_lock:
            if self.sock:
                self.sock.sendall(data.encode())

    def record(self, temp=24.5, rh=60.1, gas=161):
        self.seq += 1
        state = {"NORMAL": 0, "SUSPECT": 1, "CONFIRMED": 2}[self.event_state]
        return "%.2f,%.2f,%d,%d,%d,-1.0,-43.0,-57.0,-1,%d,83" % (temp, rh, gas, self.epoch(), self.seq, state)

    def _reply(self, tag, text):
        self.send((("#RSP#%s " % tag) if tag else "#RSP ") + text + "$")

    # ------------------------------------------------------------------ commands (firmware semantics)
    def execute(self, line):
        parts = line.strip().split(" ", 1)
        cmd, arg = parts[0].upper(), (parts[1].strip() if len(parts) > 1 else "")
        self.received_commands.append(line.strip())
        if cmd == "HELP":
            return HELP
        if cmd == "PING":
            return "PONG %s %s" % (self.name, self.fw)
        if cmd == "STATUS":
            f = self.status_fields()
            return json.dumps({k: (None if v == "null" else v) for k, v in f.items()})
        if cmd == "CONFIG":
            return "\n".join("%s=%s" % (k, "****" if k in ("pass", "token") else v) for k, v in self.cfg.items())
        if cmd == "SET":
            kv = arg.replace("=", " ", 1).split(" ", 1)
            if len(kv) < 2:
                return "ERR usage: SET <key> <value>"
            if kv[0] not in self.cfg:
                return "ERR unknown key or bad value: " + kv[0]
            self.cfg[kv[0]] = kv[1]
            return "OK %s (use SAVE to persist; network keys need REBOOT)" % kv[0]
        if cmd == "SAVE":
            return "OK saved"
        if cmd == "TIME":
            try:
                e = int(arg)
            except ValueError:
                e = 0
            if e < 1600000000:
                return "ERR bad epoch"
            self._set_clock(e)
            return "OK time %d" % e
        if cmd == "SIM":
            what, _, secs = arg.partition(" ")
            secs = int(secs) if secs.strip().isdigit() else 60
            what = what.upper()
            if what == "OFFLINE":
                self.sim_offline_until = time.monotonic() + min(secs, 3600)
            elif what == "GAS":
                self.sim_gas_until = time.monotonic() + secs
            elif what == "AC":
                self.sim_ac_until = time.monotonic() + secs
            elif what == "CLEAR":
                self.sim_gas_until = self.sim_ac_until = self.sim_offline_until = 0
            else:
                return "ERR SIM <GAS|AC|OFFLINE|CLEAR> [seconds]"
            return "OK sim %s %d s" % (what, secs)
        if cmd == "MAINT":
            self.cfg["maint"] = "1" if arg.strip() == "1" else "0"
            return "OK maintenance " + ("ON" if self.cfg["maint"] == "1" else "OFF")
        if cmd == "POWER":
            self.cfg["power"] = "1" if arg.strip() == "1" else "0"
            return "OK power " + ("ECO" if self.cfg["power"] == "1" else "ACTIVE")
        if cmd == "LOG":
            return "\n".join(self.event_log[-(int(arg) if arg.isdigit() else 20):])
        if cmd == "SELFTEST":
            return ("AHT20:PASS\nGAS_ADC:PASS raw=161\nMIC:PASS\nCAMERA:PASS bytes=48000\nSD:PASS\nFLASH_CFG:PASS\n"
                    "WIFI:PASS rssi=-35\nULTRASONIC:N/A (OP-01)\nPOWER:N/A (OP-04)\nTAMPER:N/A (OP-08)\nRESULT:PASS")
        if cmd == "CAPTURE":
            self.event_counter += 1
            eid = "M%d_%d" % (self.boot_count, self.event_counter)
            threading.Thread(target=self.post_image, args=(eid,), daemon=True).start()
            return "OK capture " + eid
        if cmd == "FS_TEST" and self.flash_mb >= 32:
            return "OK 64 KB written in 1500 ms, read+verified in 40 ms"
        if cmd == "FS_DF" and self.flash_mb >= 32:
            return "OK used 8 KB of 16192 KB (0%)"
        if cmd.startswith("FS_"):
            return "ERR file system not available: flash is 16 MB: no file system"
        if cmd == "FLASH_TEST":
            return "OK erase=ok program=ok nor_and=yes guard=ok"
        if cmd == "FLASH":
            return ("events: sectors=8 capacity=32640 used=%d records=%d pending=%d dropped=0 corrupt=0 max_erase=3\n"
                    "buffer: sectors=36 capacity=146880 used=0 records=0 pending=0 dropped=0 corrupt=0 max_erase=3\n"
                    "thinned=0 store_ok=1" % (60 * len(self.event_log), len(self.event_log), len(self.event_log)))
        if cmd == "OTA_SD":
            if not self.ota_capable:
                return ("ERR firmware built without OTA Mode: build with Tools > OTA Mode = Enable and NN Model Load "
                        "From = SD Card, flash it over USB once, then OTA_SD works")
            return "ERR ota.bin not found on SD"
        if cmd == "REBOOT":
            threading.Thread(target=self._reboot, daemon=True).start()
            return "OK rebooting in 1 s"
        return "ERR unknown command (HELP)"

    def _reboot(self):
        # like the real chip: a hard reset sends no FIN; the old TCP connection is simply abandoned
        time.sleep(1.0)
        with self.sock_lock:
            if self.sock is not None:
                self._abandoned.append(self.sock)
            self.sock = None
        time.sleep(self.boot_s)
        self.boot_count += 1
        self.last_reset = "cmd_reboot:cmd"
        self.seq = 0
        self.event_state = "NORMAL"                     # RAM state: the firmware restarts without an event
        self.event_counter = 0
        self.sim_gas_until = self.sim_ac_until = self.sim_offline_until = 0
        self.clock_offset = None
        self._t0 = time.monotonic()
        self._boot()

    # ------------------------------------------------------------------ threads
    def _reader(self):
        buf = b""
        while not self._stop.is_set():
            s = self.sock
            if s is None:
                time.sleep(0.1)
                continue
            try:
                d = s.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                self._close_socket(s)          # only the socket this thread was using
                continue
            if not d:
                self._close_socket(s)
                continue
            buf += d
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode(errors="replace").strip()
                if line.startswith("CMD#"):
                    head, _, body = line.partition(" ")
                    tag = head[4:]
                    self._reply(tag if self.tagged else "", self.execute(body))
                elif line.startswith("CMD "):
                    self._reply("", self.execute(line[4:]))

    def _uploader(self):
        while not self._stop.is_set():
            s = self.sock
            if self.send_records and self.offline():
                self.buffer.append(self.record() + "$")       # one record per cycle, as the firmware keeps
            elif s and self.send_records:
                try:
                    if self.buffer:                             # oldest first, then live data
                        batch, self.buffer = self.buffer[:50], self.buffer[50:]
                        for r in batch:
                            self.send(r)
                    self.send("_".join(self.record() for _ in range(4)) + "$")
                    if self.pending_pics:
                        self.post_image(self.pending_pics.pop(0))
                    if self.pending_clips and self.post_clip(self.pending_clips[0])[0] == 200:
                        self.pending_clips.pop(0)
                except OSError:
                    self._close_socket(s)
            self._stop.wait(self.upload_s)

    def _heartbeat_loop(self):
        while not self._stop.is_set():
            try:
                self.heartbeat()
            except OSError:
                pass
            self._stop.wait(self.hb_s)

    def _event_loop(self):
        while not self._stop.is_set():
            now = time.monotonic()
            g = now < self.sim_gas_until
            a = now < self.sim_ac_until
            if g or a:
                self.last_alarm = now
            target = "CONFIRMED" if (g and a) else ("SUSPECT" if (g or a) else None)
            order = {"NORMAL": 0, "SUSPECT": 1, "CONFIRMED": 2}
            if target and order[target] > order[self.event_state]:
                if self.event_state == "NORMAL":
                    self.event_counter += 1
                    self.event_id = "E%d_%d" % (self.boot_count, self.event_counter)
                self._transition(target, g, a)
            elif self.event_state != "NORMAL" and not (g or a) and now - self.last_alarm > self.event_hold_s:
                self._transition("NORMAL", g, a)
            self._stop.wait(0.2)

    def _transition(self, new, g, a):
        prev = self.event_state
        order = {"NORMAL": 0, "SUSPECT": 1, "CONFIRMED": 2}
        if order[new] > order[prev] and self.offline() and self.flash_mb >= 32 \
                and self.event_id not in self.pending_clips:
            self.pending_clips.append(self.event_id)     # one 5 s clip per event while the server is unreachable
            self.clips_saved += 1
        self.event_state = new
        self.event_log.append("%d,%s,%s" % (self.epoch(), self.event_id, new))
        q = urllib.parse.urlencode({"name": self.name, "id": self.mac, "event": self.event_id, "state": new, "prev": prev,
                                    "gas_ppm": "-1.0", "gas_raw": 161, "ac_db": "9.0" if a else "0.5", "g": int(g),
                                    "a": int(a), "u": 0, "maint": self.cfg["maint"], "ts": self.epoch(), "valid": 83})
        try:
            self.http("GET", "/node-event?" + q)
            if {"NORMAL": 0, "SUSPECT": 1, "CONFIRMED": 2}[new] > {"NORMAL": 0, "SUSPECT": 1, "CONFIRMED": 2}[prev] \
                    and self.cfg["maint"] != "1":
                self.post_image(self.event_id)
        except OSError:
            pass

    def _close_socket(self, only=None):
        """Close the current socket; with `only`, close it only if it is still that socket (a reader that
        failed on an old socket must not close a newer one)."""
        with self.sock_lock:
            if only is not None and self.sock is not only:
                return
            if self.sock:
                try:
                    self.sock.close()
                except OSError:
                    pass
            self.sock = None

    def _boot(self):
        for _ in range(50):
            if self._stop.is_set():
                return
            try:
                if self.register() == 200:
                    break
            except OSError:
                pass
            time.sleep(0.2)
        for _ in range(50):
            try:
                self.connect_socket()
                break
            except OSError:
                time.sleep(0.2)

    # ------------------------------------------------------------------ lifecycle
    def start(self, threads=("reader", "uploader", "heartbeat", "events")):
        self._t0 = time.monotonic()
        self._boot()
        targets = {"reader": self._reader, "uploader": self._uploader, "heartbeat": self._heartbeat_loop,
                   "events": self._event_loop}
        for t in threads:
            th = threading.Thread(target=targets[t], name="emu-%s-%s" % (self.ip, t), daemon=True)
            th.start()
            self._threads.append(th)
        return self

    def stop(self):
        self._stop.set()
        self._close_socket()
        for t in self._threads:
            t.join(timeout=2)
