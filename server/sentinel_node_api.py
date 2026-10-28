#!/usr/bin/env python3
"""
Sentinel Node API: collects sensor data, JPEG images and WAV audio from
Sentinel Nodes (AMB82 boards) and serves them to other agents over REST.

Two servers run side by side:
  * Device side  http://192.168.100.1:8900
        GET  /add-device?ip=<ip>&name=<name>[&id=<mac>&fw=<ver>]   registration
        GET  /node-status?<k=v...>                                  heartbeat / health   (FW >= 1.1.0)
        GET  /node-event?<k=v...>                                   leak events          (FW >= 1.1.0)
        POST /                                                      event JPEG upload    (FW >= 1.1.0)
        GET  /up-data?data=<records>                                HTTP upload mode
    Successful replies are "<DONE|OK> T=<unix epoch>": nodes (FW >= 1.1.3) set their clock from it.
    Python's built-in (lenient) HTTP server, same as the original script. Firmware 1.0
    (3-field records, registration only) keeps working unchanged.
  * Agent side   http://<host>:8080/api/v1/...   docs: /docs   schema: /openapi.json

Sensor socket (per node, port 4000 + last IP octet):
    node -> server   records "f1,f2,..._f1,f2,...$"   and command replies "#RSP <text>$"
    server -> node   commands "CMD#<id> <command>\n" (FW >= 1.1.1, reply "#RSP#<id> ...$")
                     or "CMD <command>\n" (FW 1.1.0, replies matched in order); see POST /devices/{id}/command
    Records from FW >= 1.1.0 carry 11 fields, see SENSOR_FIELDS.

    pip install fastapi uvicorn requests
    AMB82_API_KEY=change-me python3 sentinel_node_api.py
"""
from __future__ import annotations

import asyncio
import codecs
import ipaddress
import json
import logging
import os
import re
import queue
import secrets
import select
import selectors
import shutil
import signal
import socket
import subprocess
import threading
import time
from collections import deque
from datetime import datetime
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Optional
from urllib.parse import parse_qs, urlparse

import requests
import uvicorn
from fastapi import APIRouter, Depends, FastAPI, Header, HTTPException, Query, Request
from fastapi.responses import FileResponse, StreamingResponse
from pydantic import BaseModel, Field

# ---------------------------------------------------------------- configuration
DEVICE_NET_IP = os.getenv("AMB82_DEVICE_NET_IP", "192.168.100.1")   # host IP on the device network
DEVICE_SUBNET = ipaddress.ip_network(os.getenv("AMB82_DEVICE_SUBNET", "192.168.100.0/24"))
REG_PORT = int(os.getenv("AMB82_REG_PORT", "8900"))                 # port the boards already call
API_HOST = os.getenv("AMB82_API_HOST", "0.0.0.0")                   # where agents connect
API_PORT = int(os.getenv("AMB82_API_PORT", "8080"))
DEBUG = os.getenv("AMB82_DEBUG") == "1"                             # log every raw sensor chunk
API_KEY = os.getenv("AMB82_API_KEY")                                # unset = no auth (not recommended)
DATA_DIR = Path(os.getenv("AMB82_DATA_DIR", "data"))

CAMERA_API = "/get-picture"
RTSP_PORT = 554
SOCKET_BASE_PORT = 4000
CAPTURE_GAP_TIME = 60          # seconds between automatic pictures
AUDIO_SEGMENT_SECONDS = 60     # length of each finished WAV file
SENSOR_IDLE_WARN = 30          # warn (but keep the connection) after this many silent seconds
# A node that loses power never closes its TCP connection. These keepalive settings make
# the kernel probe a silent connection after 30 s and drop it after 3 unanswered probes
# (about 60 s in total) instead of the Linux default of about 2 h 11 min.
KEEPALIVE_IDLE, KEEPALIVE_INTERVAL, KEEPALIVE_COUNT = 30, 10, 3
# A node counts as "online" if its last data arrived within this many seconds.
ONLINE_WINDOW = int(os.getenv("AMB82_ONLINE_WINDOW", "30"))
AUDIO_RECORDER = os.getenv("AMB82_AUDIO_RECORDER", "auto")   # auto | gst | ffmpeg
SENSOR_BUFFER_SIZE = 10_000    # sensor records kept in memory for the API
MAX_IMAGES = 1440              # retention (1 day at 60 s)
MAX_AUDIO_SEGMENTS = 1440      # retention (1 day at 60 s)
MAX_EVENT_IMAGES = int(os.getenv("AMB82_MAX_EVENT_IMAGES", "5000"))   # event pictures kept longer
MAX_EVENT_AUDIO = int(os.getenv("AMB82_MAX_EVENT_AUDIO", "2000"))     # event sound clips (firmware >= 1.6.0)
ECO_CAPTURE_GAP_TIME = int(os.getenv("AMB82_ECO_CAPTURE_GAP", "600"))  # periodic picture gap when node is in ECO mode
NODE_TOKEN = os.getenv("AMB82_NODE_TOKEN")      # node API token (firmware "token"); needed if node secure_all=1
EVENT_WEBHOOK = os.getenv("AMB82_EVENT_WEBHOOK")  # optional URL: every event is POSTed here as JSON
EVENT_BUFFER_SIZE = 10_000     # events kept in memory for the API
STATUS_STALE = int(os.getenv("AMB82_STATUS_STALE", "180"))   # heartbeat older than this -> "stale"
TIME_SYNC_INTERVAL = 3600      # push server time to nodes (CMD TIME) on connect and then hourly
COMMAND_TIMEOUT = 15           # default seconds to wait for a node command reply
MAX_UPLOAD_BYTES = 8 * 1024 * 1024

# Sensor wire format, as inferred from the original record_data():
# packets separated by "$", records inside a packet separated by "_".
PACKET_SEP = "$"
RECORD_SEP = "_"
MIN_RECORD_LEN = 6
MAX_PENDING = 64 * 1024

REPLY_PREFIX = "#RSP"          # command replies: "#RSP <text>" (1.1.0) or "#RSP#<id> <text>" (>= 1.1.1)

# Record layout. FW 1.0 sends the first 3 fields; FW >= 1.1.0 (EXTENDED_SENSOR_DATA) sends all 11.
SENSOR_FIELDS = ["temp_c", "rh_pct", "gas_raw",                      # legacy fields
                 "device_ts", "device_seq", "gas_ppm", "ac_db", "ac_hf_db",
                 "us", "event_state", "valid"]
EVENT_STATES = {0: "NORMAL", 1: "SUSPECT", 2: "CONFIRMED"}
VALID_BITS = ["aht_ok", "gas_ok", "gas_warm", "gas_calibrated", "mic_ok",
              "us_present", "time_synced", "maintenance", "simulated"]
INVALID_VALUE = -999           # firmware marker for "sensor read failed"

FILE_RE = re.compile(r"\d{8}_\d{6}_\d{6}\.(jpg|wav)")
MEDIA = {"image": ("images", "jpg", "image/jpeg"), "audio": ("audio", "wav", "audio/wav"),
         "event_image": ("event_images", "jpg", "image/jpeg"), "event_audio": ("event_audio", "wav", "audio/wav")}
MEDIA_ROUTE = {"image": "images", "audio": "audio", "event_image": "event-images", "event_audio": "event-audio"}
EVENT_ID_RE = re.compile(r"[A-Za-z0-9_-]{1,32}")

logging.basicConfig(level=logging.DEBUG if DEBUG else logging.INFO,
                    format="%(asctime)s [%(threadName)s] %(levelname)s %(message)s")
log = logging.getLogger("amb82")


def _stamp(dt: datetime) -> str:
    return dt.strftime("%Y%m%d_%H%M%S_%f")


def _stamp_to_iso(filename: str) -> Optional[str]:
    try:
        return datetime.strptime(filename[:22], "%Y%m%d_%H%M%S_%f").isoformat(timespec="milliseconds")
    except ValueError:
        return None


def _parse_value(v: str):
    v = v.strip()
    try:
        return float(v)
    except ValueError:
        return v


def _safe_name(name: str) -> str:
    """Accept whatever the firmware sends, but make it safe for file paths."""
    return re.sub(r"[^A-Za-z0-9_-]", "_", name.strip())[:32] or "device"


def _num(v):
    """float/int from a string, None for missing, invalid or the firmware's -999 marker."""
    try:
        f = float(v)
    except (TypeError, ValueError):
        return None
    if f == INVALID_VALUE or f != f:
        return None
    return int(f) if f.is_integer() else f


def _parse_fields(values: list) -> Optional[dict]:
    """Named view of a record (FW 1.0: 3 fields, FW >= 1.1.0: 11 fields)."""
    if len(values) not in (3, len(SENSOR_FIELDS)):
        return None
    out = {k: _num(v) for k, v in zip(SENSOR_FIELDS, values)}
    if "us" in out and out["us"] is not None and out["us"] < 0:
        out["us"] = None                               # ultrasonic not fitted yet (OP-01)
    if out.get("device_ts") == 0:
        out["device_ts"] = None                        # node clock not synchronised yet
    if out.get("event_state") is not None:
        out["event_state"] = EVENT_STATES.get(int(out["event_state"]), str(out["event_state"]))
    if out.get("valid") is not None:
        mask = int(out["valid"])
        out["flags"] = {name: bool(mask >> i & 1) for i, name in enumerate(VALID_BITS)}
    return out


def _qs_flat(query: str) -> dict:
    return {k: v[0] for k, v in parse_qs(query, keep_blank_values=True).items()}


def _status_values(qs: dict) -> dict:
    """Convert the heartbeat query string into typed values."""
    out = {}
    for k, v in qs.items():
        if v == "null":
            out[k] = None
        else:
            n = _num(v) if re.fullmatch(r"-?\d+(\.\d+)?", v or "") else None
            out[k] = n if n is not None or v in ("-999", "-999.0", "-999.00") else v
    return out


def _extract_wav(body: bytes) -> Optional[bytes]:
    """Event sound clip: cut the RIFF/WAVE file out of the firmware's multipart envelope by its RIFF size."""
    i = body.find(b"RIFF")
    if i < 0 or body[i + 8:i + 12] != b"WAVE":
        return None
    size = int.from_bytes(body[i + 4:i + 8], "little") + 8
    if size < 44 or i + size > len(body):
        return None
    return body[i:i + size]


def _extract_jpeg(body: bytes) -> Optional[bytes]:
    """The firmware wraps the JPEG in a minimal multipart envelope; cut out SOI..EOI."""
    start = body.find(b"\xff\xd8\xff")
    end = body.rfind(b"\xff\xd9")
    if start < 0 or end < start:
        return None
    return body[start:end + 2]


def _in_device_subnet(host: Optional[str]) -> bool:
    try:
        return host is not None and ipaddress.ip_address(host) in DEVICE_SUBNET
    except ValueError:
        return False


# ---------------------------------------------------------------- events
class EventBus:
    """All node and server-generated events, with a global sequence for polling / SSE / webhook."""

    def __init__(self):
        self._lock = threading.Lock()
        self._events: deque = deque(maxlen=EVENT_BUFFER_SIZE)
        self._seq = 0
        self._hook_q: "queue.Queue[dict]" = queue.Queue(maxsize=1000)
        if EVENT_WEBHOOK:
            threading.Thread(target=self._webhook_loop, name="event-webhook", daemon=True).start()

    def publish(self, dev: "Device", etype: str, data: dict) -> dict:
        now = time.time()
        with self._lock:
            self._seq += 1
            ev = {"seq": self._seq, "type": etype, "device_id": dev.id, "device_name": dev.name,
                  "ts": round(now, 3), "time": datetime.fromtimestamp(now).isoformat(timespec="milliseconds"),
                  **data}
            self._events.append(ev)
        line = json.dumps(ev)
        with open(dev.dir / "events.jsonl", "a", encoding="utf-8") as f:
            f.write(line + "\n")
        lvl = logging.WARNING if data.get("state") == "CONFIRMED" or etype == "node_offline" else logging.INFO
        log.log(lvl, "EVENT %s %s %s", dev.name, etype, {k: v for k, v in data.items() if k != "images"})
        if EVENT_WEBHOOK:
            try:
                self._hook_q.put_nowait(ev)
            except queue.Full:
                log.warning("Event webhook queue full; event %d not forwarded", ev["seq"])
        return ev

    @property
    def current_seq(self) -> int:
        with self._lock:
            return self._seq

    def since(self, after_seq: Optional[int] = None, limit: int = 100,
              device_id: Optional[str] = None) -> list[dict]:
        with self._lock:
            evs = [e for e in self._events if device_id is None or e["device_id"] == device_id]
        if after_seq is None:
            return evs[-limit:]
        return [e for e in evs if e["seq"] > after_seq][:limit]

    def _webhook_loop(self) -> None:
        while True:
            ev = self._hook_q.get()
            for attempt in range(3):
                try:
                    requests.post(EVENT_WEBHOOK, json=ev, timeout=5).raise_for_status()
                    break
                except requests.RequestException as e:
                    if attempt == 2:
                        log.warning("Event webhook failed for event %d: %s", ev["seq"], e)
                    time.sleep(2)


events = EventBus()


class CommandTicket:
    def __init__(self, command: str):
        self.command = command
        self.done = threading.Event()
        self.reply: Optional[str] = None
        self.error: Optional[str] = None


# ---------------------------------------------------------------- device
class Device:
    def __init__(self, ip: str, name: str):
        self.ip = ip
        self.name = name
        self.id = ip
        self.registered_at = datetime.now()
        self.dir = DATA_DIR / f"{name}_{ip}"
        self.csv_path = self.dir / "sensor-data.csv"
        for kind in MEDIA:
            self._media_dir(kind).mkdir(parents=True, exist_ok=True)
        self.socket_port = SOCKET_BASE_PORT + int(ip.split(".")[3])

        self._stop_event = threading.Event()
        self._lock = threading.Lock()
        self._capture_lock = threading.Lock()
        self._records: deque = deque(maxlen=SENSOR_BUFFER_SIZE)
        self._seq = 0
        self._last_rx_ts: Optional[float] = None
        self.sensor_connected = False
        self.last_data_at: Optional[float] = None    # wall-clock time of the last sensor bytes
        self._audio_warned = False
        self._threads: list[threading.Thread] = []
        # FW >= 1.1.0: identity, health, events, command channel
        self.node_id: Optional[str] = None         # MAC address reported by the node
        self.fw: Optional[str] = None
        self.status: dict = {}                     # last /node-status heartbeat
        self.status_at: Optional[float] = None
        self.event_state = "NORMAL"
        self.event_id: Optional[str] = None
        self.last_event: Optional[dict] = None
        self._conn: Optional[socket.socket] = None
        self._send_lock = threading.Lock()
        self._tickets: deque = deque()             # FW 1.1.0: untagged replies, matched in order
        self._tagged: dict[str, CommandTicket] = {} # FW >= 1.1.1: replies carry the request id
        self._cmd_counter = 0
        self._last_time_sync = 0.0
        self.was_online = False
        self.boot_count: Optional[int] = None     # node boot counter (registration / heartbeat)
        self.rebooting_until = 0.0                # set by a REBOOT command, cleared by re-registration

    # ---- lifecycle
    def start(self) -> None:
        for fn, label in ((self._sensor_socket_loop, "sensor"),
                          (self._image_loop, "image"),
                          (self._audio_loop, "audio")):
            t = threading.Thread(target=self._supervise(fn), name=f"{label}-{self.ip}", daemon=True)
            t.start()
            self._threads.append(t)
        log.info("Started device %s (%s), sensor port %d", self.name, self.ip, self.socket_port)

    def update_identity(self, node_id: Optional[str], fw: Optional[str]) -> None:
        if node_id:
            self.node_id = re.sub(r"[^0-9A-Fa-f]", "", node_id)[:12].upper() or None
        if fw:
            self.fw = re.sub(r"[^0-9A-Za-z._-]", "", fw)[:16] or None

    def _fw_tuple(self) -> tuple:
        try:
            return tuple(int(x) for x in (self.fw or "").split(".")[:3])
        except ValueError:
            return ()

    def reset_connection(self, reason: str) -> None:
        """Drop the sensor socket now (the node rebooted, so the connection is dead even if TCP has not
        noticed yet) and fail commands still waiting for a reply."""
        with self._send_lock:
            conn = self._conn
            self._fail_tickets(reason)
        if conn is not None:
            try:
                conn.shutdown(socket.SHUT_RDWR)       # the socket thread then closes it
            except OSError:
                pass
            log.info("Sensor connection of %s dropped: %s", self.ip, reason)

    def mark_rebooting(self) -> None:
        self.rebooting_until = time.time() + 120
        self.reset_connection("node is rebooting")

    @property
    def rebooting(self) -> bool:
        return self.rebooting_until > time.time()

    @property
    def supports_tagged_commands(self) -> bool:
        return self._fw_tuple() >= (1, 1, 1)

    @property
    def supports_commands(self) -> bool:
        """Command channel / extended protocol exists from FW 1.1.0 on."""
        try:
            return self.fw is not None and tuple(int(x) for x in self.fw.split(".")[:2]) >= (1, 1)
        except ValueError:
            return False

    def shutdown(self) -> None:
        self._stop_event.set()
        for t in self._threads:
            t.join(timeout=10)
        log.info("Stopped device %s (%s)", self.name, self.ip)

    def _supervise(self, fn):
        """Restart a worker if it crashes instead of letting the thread die silently."""
        def runner():
            while not self._stop_event.is_set():
                try:
                    fn()
                except Exception:
                    log.exception("%s crashed; restarting in 5 s", fn.__name__)
                self._stop_event.wait(5)
        return runner

    # ---- sensor data (raw TCP stream from the board)
    def _sensor_socket_loop(self) -> None:
        """Accept the board's TCP stream. Idle connections are kept open (the firmware
        re-registers when we close them); a new connection from the board replaces the old one."""
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as srv, selectors.DefaultSelector() as sel:
            srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            srv.bind((DEVICE_NET_IP, self.socket_port))
            srv.listen(2)
            srv.setblocking(False)
            sel.register(srv, selectors.EVENT_READ)
            log.info("Sensor socket listening on %s:%d", DEVICE_NET_IP, self.socket_port)
            conn: Optional[socket.socket] = None
            decoder = None
            pending = ""
            last_rx = time.monotonic()
            warned = False

            def close_conn(reason: str) -> None:
                nonlocal conn, pending
                if conn is None:
                    return
                if pending:
                    self._ingest([pending])
                    pending = ""
                sel.unregister(conn)
                with self._send_lock:
                    self._conn = None
                    self._fail_tickets("connection closed")
                conn.close()
                conn = None
                self.sensor_connected = False
                log.info("Sensor stream closed (%s)", reason)

            try:
                while not self._stop_event.is_set():
                    for key, _ in sel.select(timeout=1.0):
                        if key.fileobj is srv:
                            new, addr = srv.accept()
                            close_conn("replaced by a new connection")
                            new.setblocking(False)
                            new.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
                            for opt, val in (("TCP_KEEPIDLE", KEEPALIVE_IDLE),
                                             ("TCP_KEEPINTVL", KEEPALIVE_INTERVAL),
                                             ("TCP_KEEPCNT", KEEPALIVE_COUNT)):
                                if hasattr(socket, opt):
                                    new.setsockopt(socket.IPPROTO_TCP, getattr(socket, opt), val)
                            sel.register(new, selectors.EVENT_READ)
                            conn = new
                            with self._send_lock:
                                self._conn = new
                                self._tickets.clear()
                            self._last_time_sync = 0.0           # push our clock to the node right away
                            decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")
                            self._last_rx_ts = None
                            self.sensor_connected = True
                            last_rx, warned = time.monotonic(), False
                            log.info("Sensor stream connected from %s", addr)
                            continue
                        if key.fileobj is not conn:
                            continue
                        try:
                            chunk = conn.recv(4096)
                        except (BlockingIOError, InterruptedError):
                            continue
                        except OSError as e:
                            close_conn(f"socket error: {e}")
                            continue
                        if not chunk:
                            close_conn("closed by device")
                            continue
                        last_rx, warned = time.monotonic(), False
                        self.last_data_at = time.time()
                        log.debug("Sensor bytes from %s: %r", self.ip, chunk)
                        pending += decoder.decode(chunk)
                        *packets, pending = pending.split(PACKET_SEP)
                        if len(pending) > MAX_PENDING:
                            packets.append(pending)
                            pending = ""
                        if packets:
                            self._ingest(packets)
                    if conn is not None and self.supports_commands and \
                            time.time() - self._last_time_sync > TIME_SYNC_INTERVAL:
                        self._last_time_sync = time.time()
                        # Node timestamps (record field device_ts) become usable even without NTP access
                        self.send_command(f"TIME {int(time.time())}", wait=False)
                    if conn is not None and not warned and time.monotonic() - last_rx > SENSOR_IDLE_WARN:
                        log.warning("No sensor data for %d s from %s (connection kept open)",
                                    SENSOR_IDLE_WARN, self.ip)
                        warned = True
            finally:
                close_conn("shutting down")

    def _ingest(self, packets: list[str]) -> None:
        data_packets = []
        for pkt in packets:
            stripped = pkt.strip()
            if stripped.startswith("#"):          # command reply, not sensor data (FW >= 1.1.0)
                self._on_reply(stripped[len(REPLY_PREFIX):] if stripped.startswith(REPLY_PREFIX) else " " + stripped[1:])
            else:
                data_packets.append(pkt)
        texts = [p.strip() for pkt in data_packets for p in pkt.split(RECORD_SEP)
                 if len(p.strip()) >= MIN_RECORD_LEN]
        if not texts:
            return
        now = time.time()
        n = len(texts)
        # Records arrived in a batch: spread them evenly over the time since the last batch,
        # ending at "now" (the original spread them into the future).
        step = (now - self._last_rx_ts) / n if self._last_rx_ts else 0.0
        self._last_rx_ts = now
        lines = []
        with self._lock:
            for i, text in enumerate(texts):
                ts = now - (n - 1 - i) * step
                parts = text.split(",")
                fields = _parse_fields(parts)
                ts_source = "server"
                dev_ts = fields.get("device_ts") if fields else None
                # Node clock (synchronised by CMD TIME / NTP): exact time, also for records the node
                # buffered on SD while offline and replays later.
                if dev_ts is not None and 1_600_000_000 < dev_ts <= now + 60:
                    ts, ts_source = float(dev_ts), "device"
                self._seq += 1
                rec = {
                    "seq": self._seq,
                    "ts": round(ts, 3),
                    "time": datetime.fromtimestamp(ts).isoformat(timespec="milliseconds"),
                    "ts_source": ts_source,
                    "raw": text,
                    "values": [_parse_value(v) for v in parts],
                }
                if fields is not None:
                    rec["fields"] = fields
                self._records.append(rec)
                lines.append(f"{rec['time']},{rec['seq']},{text}\n")
        with open(self.csv_path, "a", encoding="utf-8") as f:
            f.writelines(lines)

    @property
    def current_seq(self) -> int:
        with self._lock:
            return self._seq

    def latest_record(self) -> Optional[dict]:
        with self._lock:
            return self._records[-1] if self._records else None

    def records(self, after_seq: Optional[int] = None, limit: int = 100) -> list[dict]:
        with self._lock:
            if after_seq is None:
                return list(self._records)[-limit:]
            return [r for r in self._records if r["seq"] > after_seq][:limit]

    # ---- command channel (FW >= 1.1.0): "CMD <command>\n" out, "#RSP <reply>$" back, in order
    def _fail_tickets(self, reason: str) -> None:
        pending = [t for t in self._tickets if t is not None] + list(self._tagged.values())
        self._tickets.clear()
        self._tagged.clear()
        for t in pending:
            t.error = reason
            t.done.set()

    def _on_reply(self, payload: str) -> None:
        """payload is what follows '#RSP': '#<id> <text>' (tagged) or ' <text>' (untagged)."""
        with self._send_lock:
            if payload.startswith("#"):
                tag, _, text = payload[1:].partition(" ")
                ticket = self._tagged.pop(tag, None)
            else:
                text = payload[1:] if payload.startswith(" ") else payload
                ticket = self._tickets.popleft() if self._tickets else None
        if ticket is None:
            log.debug("Reply from %s (no waiter): %s", self.ip, text[:200])
            return
        ticket.reply = text
        ticket.done.set()

    @staticmethod
    def _send_nonblocking(sock: socket.socket, data: bytes, timeout: float) -> None:
        """The socket stays non-blocking (the reader thread relies on it); wait for writability."""
        deadline = time.monotonic() + timeout
        view = memoryview(data)
        while view:
            try:
                sent = sock.send(view)
                view = view[sent:]
            except (BlockingIOError, InterruptedError):
                left = deadline - time.monotonic()
                if left <= 0:
                    raise TimeoutError("send timed out")
                select.select([], [sock], [], left)

    def send_command(self, command: str, wait: bool = True,
                     timeout: float = COMMAND_TIMEOUT) -> Optional[str]:
        """Send a firmware command over the sensor socket. Returns the reply text (wait=True).
        Raises RuntimeError when the node is not connected or does not answer."""
        command = command.strip()
        if not command or len(command) > 150 or any(c in command for c in "\r\n$"):
            raise ValueError("command must be 1..150 characters without newlines or '$'")
        ticket = CommandTicket(command) if wait else None
        with self._send_lock:
            if self._conn is None:
                raise RuntimeError("sensor socket not connected")
            tagged = self.supports_tagged_commands
            if tagged:
                self._cmd_counter += 1
                tag = f"c{self._cmd_counter}"
                if ticket is not None:
                    self._tagged[tag] = ticket
                data = f"CMD#{tag} {command}\n".encode()
            else:
                self._tickets.append(ticket)        # a None ticket absorbs the reply of a fire-and-forget command
                data = f"CMD {command}\n".encode()
            try:
                self._send_nonblocking(self._conn, data, timeout=5)
            except OSError as e:
                if tagged:
                    self._tagged.pop(tag, None)
                else:
                    self._tickets.pop()
                raise RuntimeError(f"send failed: {e}") from e
        shown = command if not command.upper().startswith("SET ") else " ".join(command.split(" ")[:2]) + " ***"
        log.info("Command to %s: %s", self.ip, shown)
        if ticket is None:
            return None
        if not ticket.done.wait(timeout):
            if tagged:
                with self._send_lock:
                    self._tagged.pop(tag, None)      # a late reply is simply ignored
            # untagged (FW 1.1.0): keep the ticket queued so it absorbs the late reply
            raise RuntimeError(f"no reply within {timeout:.0f} s")
        if ticket.error:
            raise RuntimeError(ticket.error)
        return ticket.reply

    def command_via_http(self, command: str, timeout: float = COMMAND_TIMEOUT) -> str:
        """Fallback when the socket is down: node HTTP /cmd endpoint (needs AMB82_NODE_TOKEN)."""
        if not NODE_TOKEN:
            raise RuntimeError("sensor socket not connected and AMB82_NODE_TOKEN not set for HTTP fallback")
        r = requests.get(f"http://{self.ip}/cmd", params={"c": command, "token": NODE_TOKEN}, timeout=timeout)
        r.raise_for_status()
        return r.text

    # ---- node heartbeat / events (FW >= 1.1.0)
    def on_status(self, values: dict) -> None:
        self.status = values
        if isinstance(values.get("boot_count"), int) and self.boot_count is None:
            self.boot_count = values["boot_count"]
        self.status_at = time.time()
        if values.get("event_state"):
            self.event_state = str(values["event_state"])
        if values.get("event_id"):
            self.event_id = str(values["event_id"])

    def on_event(self, values: dict) -> dict:
        state = str(values.get("state", "")).upper()
        event_id = str(values.get("event", ""))
        data = {
            "event_id": event_id if EVENT_ID_RE.fullmatch(event_id) else None,
            "state": state,
            "previous_state": str(values.get("prev", "")).upper(),
            "sensors": {"gas": values.get("g") == 1, "acoustic": values.get("a") == 1,
                        "ultrasonic": values.get("u") == 1},
            "gas_ppm": None if values.get("gas_ppm") in (None, -1) else values.get("gas_ppm"),
            "gas_raw": values.get("gas_raw"),
            "acoustic_above_floor_db": values.get("ac_db"),
            "maintenance": values.get("maint") == 1,
            "device_ts": values.get("ts") or None,
            "valid": values.get("valid"),
        }
        self.event_state = state or self.event_state
        self.event_id = data["event_id"] or self.event_id
        self.last_event = events.publish(self, "leak_event", data)
        return self.last_event

    def save_event_image(self, jpeg: bytes, event_id: Optional[str]) -> Path:
        eid = event_id if event_id and EVENT_ID_RE.fullmatch(event_id) else None
        path = self._media_dir("event_image") / f"{_stamp(datetime.now())}.jpg"
        tmp = path.with_name(f".{path.name}.part")
        tmp.write_bytes(jpeg)
        tmp.replace(path)
        if eid:   # side-car so the event <-> picture link survives restarts
            path.with_suffix(".event").write_text(eid, encoding="utf-8")
        self._prune("event_image", MAX_EVENT_IMAGES)
        events.publish(self, "event_image", {"event_id": eid, "file": path.name, "bytes": len(jpeg),
                                             "url": f"/api/v1/devices/{self.id}/event-images/{path.name}"})
        return path

    def save_event_audio(self, wav: bytes, event_id: Optional[str]) -> Path:
        """Sound clip a node recorded while this server was unreachable (firmware >= 1.6.0)."""
        eid = event_id if event_id and EVENT_ID_RE.fullmatch(event_id) else None
        path = self._media_dir("event_audio") / f"{_stamp(datetime.now())}.wav"
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp = path.with_name(f".{path.name}.part")
        tmp.write_bytes(wav)
        tmp.replace(path)
        if eid:
            path.with_suffix(".event").write_text(eid, encoding="utf-8")
        self._prune("event_audio", MAX_EVENT_AUDIO)
        events.publish(self, "event_audio", {"event_id": eid, "file": path.name, "bytes": len(wav),
                                             "url": f"/api/v1/devices/{self.id}/event-audio/{path.name}"})
        return path

    @property
    def status_age(self) -> Optional[int]:
        return None if self.status_at is None else int(time.time() - self.status_at)

    @property
    def power_mode(self) -> Optional[str]:
        return self.status.get("power_mode")

    # ---- images
    def _image_loop(self) -> None:
        while not self._stop_event.is_set():     # loop, not recursion (original hit the recursion limit after ~16 h)
            self.capture_image()
            # ECO nodes switch the camera off between pictures: poll them much less often (Req 12)
            self._stop_event.wait(ECO_CAPTURE_GAP_TIME if self.power_mode == "ECO" else CAPTURE_GAP_TIME)

    def capture_image(self) -> Optional[Path]:
        with self._capture_lock:
            try:
                params = {"token": NODE_TOKEN} if NODE_TOKEN else None
                r = requests.get(f"http://{self.ip}{CAMERA_API}", params=params, timeout=10)
                r.raise_for_status()
            except requests.RequestException as e:
                log.warning("Picture from %s failed: %s", self.ip, e)
                return None
            path = self._media_dir("image") / f"{_stamp(datetime.now())}.jpg"
            tmp = path.with_name(f".{path.name}.part")
            tmp.write_bytes(r.content)
            tmp.replace(path)                     # atomic: the API never serves a half-written JPEG
            self._prune("image", MAX_IMAGES)
            return path

    # ---- audio (finished WAV segments)
    def _audio_command(self, tmp: Path) -> tuple[Optional[str], list[str]]:
        url = f"rtsp://{self.ip}:{RTSP_PORT}"
        if AUDIO_RECORDER in ("auto", "gst") and shutil.which("gst-launch-1.0"):
            # -e: SIGINT sends EOS so wavenc writes a valid header
            return "gst", ["gst-launch-1.0", "-e", "rtspsrc", f"location={url}", "latency=100", "!",
                           "rtppcmadepay", "!", "alawdec", "!", "audioconvert", "!", "audioresample", "!",
                           "wavenc", "!", "filesink", f"location={tmp}"]
        if AUDIO_RECORDER in ("auto", "ffmpeg") and shutil.which("ffmpeg"):
            return "ffmpeg", ["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
                              "-i", url, "-vn", "-t", str(AUDIO_SEGMENT_SECONDS),
                              "-acodec", "pcm_s16le", "-f", "wav", "-y", str(tmp)]
        return None, []

    def _audio_loop(self) -> None:
        while not self._stop_event.is_set():
            final = self._media_dir("audio") / f"{_stamp(datetime.now())}.wav"
            tmp = final.with_name(f".{final.name}.part")
            tool, cmd = self._audio_command(tmp)
            if tool is None:
                if not self._audio_warned:
                    log.error("Audio disabled: neither gst-launch-1.0 nor ffmpeg is installed "
                              "(AMB82_AUDIO_RECORDER=%s). Install one; it is picked up automatically.",
                              AUDIO_RECORDER)
                    self._audio_warned = True
                self._stop_event.wait(60)
                continue
            if self._audio_warned:
                log.info("Audio recorder found: %s", tool)
                self._audio_warned = False
            try:                                  # argument list, no shell: device name can't inject commands
                proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL)
            except OSError as e:
                log.error("Cannot start %s: %s", tool, e)
                self._stop_event.wait(30)
                continue
            # ffmpeg stops itself after -t seconds; gst runs until we interrupt it
            deadline = time.monotonic() + AUDIO_SEGMENT_SECONDS + (15 if tool == "ffmpeg" else 0)
            while proc.poll() is None and time.monotonic() < deadline and not self._stop_event.is_set():
                time.sleep(0.5)
            interrupted = proc.poll() is None
            if interrupted:
                proc.send_signal(signal.SIGINT)   # both tools finalise the WAV header on SIGINT
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
            ok = tmp.exists() and tmp.stat().st_size > 44    # more than a bare WAV header
            if ok:
                tmp.replace(final)
                self._prune("audio", MAX_AUDIO_SEGMENTS)
            else:
                tmp.unlink(missing_ok=True)
            failed = not ok or (not interrupted and (tool == "gst" or proc.returncode != 0))
            if failed and not self._stop_event.is_set():
                log.warning("%s exited with %s for %s (no usable audio); retrying in 5 s",
                            tool, proc.returncode, self.ip)
                self._stop_event.wait(5)

    # ---- media file helpers
    def _media_dir(self, kind: str) -> Path:
        return self.dir / MEDIA[kind][0]

    def list_files(self, kind: str) -> list[Path]:
        ext = MEDIA[kind][1]
        return sorted(p for p in self._media_dir(kind).glob(f"*.{ext}") if FILE_RE.fullmatch(p.name))

    def media_file(self, kind: str, filename: str) -> Optional[Path]:
        if not FILE_RE.fullmatch(filename) or not filename.endswith("." + MEDIA[kind][1]):
            return None                           # also blocks path traversal
        p = self._media_dir(kind) / filename
        return p if p.is_file() else None

    def _prune(self, kind: str, keep: int) -> None:
        files = self.list_files(kind)
        for f in files[:-keep] if keep > 0 else []:
            f.unlink(missing_ok=True)
            f.with_suffix(".event").unlink(missing_ok=True)

    def event_of_image(self, path: Path) -> Optional[str]:
        side = path.with_suffix(".event")
        try:
            return side.read_text(encoding="utf-8").strip() or None
        except OSError:
            return None

    # ---- liveness
    @property
    def last_data_age(self) -> Optional[int]:
        """Seconds since the last sensor bytes arrived, or None if never."""
        return None if self.last_data_at is None else int(time.time() - self.last_data_at)

    @property
    def online(self) -> bool:
        age = self.last_data_age
        return self.sensor_connected and age is not None and age <= ONLINE_WINDOW

    # ---- summary for the API
    def info(self) -> dict:
        latest = self.latest_record()
        images, audio = self.list_files("image"), self.list_files("audio")
        event_images = self.list_files("event_image")
        base = f"/api/v1/devices/{self.id}"
        return {
            "id": self.id,
            "name": self.name,
            "ip": self.ip,
            "registered_at": self.registered_at.isoformat(timespec="seconds"),
            "rtsp_url": f"rtsp://{self.ip}:{RTSP_PORT}",
            "sensor": {"online": self.online,
                       "connected": self.sensor_connected,
                       "last_data_age_s": self.last_data_age,
                       "records_received": self.current_seq,
                       "last_record_at": latest["time"] if latest else None},
            "images": {"count": len(images), "latest": images[-1].name if images else None,
                       "interval_seconds": CAPTURE_GAP_TIME},
            "audio": {"segments": len(audio), "latest": audio[-1].name if audio else None,
                      "segment_seconds": AUDIO_SEGMENT_SECONDS},
            "node": {"node_id": self.node_id,
                     "firmware": self.fw,
                     "commands_supported": self.supports_commands,
                     "status_age_s": self.status_age,
                     "status_stale": self.status_age is None or self.status_age > STATUS_STALE,
                     "power_mode": self.power_mode,
                     "maintenance": bool(self.status.get("maint")) if self.status else None,
                     "last_reset": self.status.get("last_reset"),
                     "selftest_fail_mask": self.status.get("selftest_fail_mask"),
                     "ota": self.status.get("ota"),
                     "ota_capable": self.status.get("ota_capable")},
            "event": {"state": self.event_state, "event_id": self.event_id,
                      "last_event_at": self.last_event["time"] if self.last_event else None,
                      "event_images": len(event_images),
                      "event_audio": len(self.list_files("event_audio"))},
            "links": {
                "sensor_latest": f"{base}/sensor/latest",
                "sensor_history": f"{base}/sensor",
                "sensor_stream": f"{base}/sensor/stream",
                "sensor_csv": f"{base}/sensor.csv",
                "image_latest": f"{base}/images/latest",
                "image_list": f"{base}/images",
                "image_capture": f"{base}/images/capture",
                "audio_latest": f"{base}/audio/latest",
                "audio_list": f"{base}/audio",
                "node_status": f"{base}/status",
                "events": f"{base}/events",
                "event_images": f"{base}/event-images",
                "event_audio": f"{base}/event-audio",
                "command": f"{base}/command",
            },
        }


class Registry:
    def __init__(self):
        self._lock = threading.Lock()
        self._devices: dict[str, Device] = {}

    def register(self, ip: str, name: str, node_id: Optional[str] = None,
                 fw: Optional[str] = None) -> tuple[Device, bool]:
        with self._lock:
            dev = self._devices.get(ip)
            if dev:
                dev.update_identity(node_id, fw)
                return dev, False
            dev = Device(ip, name)
            dev.update_identity(node_id, fw)
            self._devices[ip] = dev
        dev.start()
        return dev, True

    def monitor(self) -> None:
        """Publish node_online / node_offline events (Req 18 connectivity monitoring)."""
        while True:
            for dev in self.all():
                online = dev.online
                if online != dev.was_online:
                    dev.was_online = online
                    events.publish(dev, "node_online" if online else "node_offline",
                                   {"last_data_age_s": dev.last_data_age})
            time.sleep(5)

    def get(self, device_id: str) -> Optional[Device]:
        with self._lock:
            return self._devices.get(device_id)

    def all(self) -> list[Device]:
        with self._lock:
            return list(self._devices.values())


registry = Registry()

# ---------------------------------------------------------------- HTTP API
app = FastAPI(
    title="Sentinel Node API",
    version="1.1.0",
    description="Sensor readings, leak events, node health, JPEG snapshots and WAV audio segments "
                "from Sentinel Nodes, plus a command channel to the nodes. "
                "Send the key in the X-API-Key header.",
)


def require_api_key(x_api_key: Optional[str] = Header(default=None)) -> None:
    if API_KEY and not (x_api_key and secrets.compare_digest(x_api_key, API_KEY)):
        raise HTTPException(status_code=401, detail="Invalid or missing X-API-Key")


def _device(device_id: str) -> Device:
    dev = registry.get(device_id)
    if not dev:
        raise HTTPException(status_code=404, detail=f"Unknown device {device_id}")
    return dev


def _serve(dev: Device, kind: str, path: Optional[Path], no_cache: bool = False) -> FileResponse:
    if path is None:
        raise HTTPException(status_code=404, detail=f"No {kind} available")
    headers = {"X-Captured-At": _stamp_to_iso(path.name) or "", "X-Device-Id": dev.id}
    if no_cache:
        headers["Cache-Control"] = "no-store"
    return FileResponse(path, media_type=MEDIA[kind][2], filename=path.name, headers=headers)


def _file_list(dev: Device, kind: str, limit: int) -> list[dict]:
    route = MEDIA_ROUTE[kind]
    files = dev.list_files(kind)[-limit:]
    out = []
    for p in reversed(files):
        item = {"file": p.name, "captured_at": _stamp_to_iso(p.name), "bytes": p.stat().st_size,
                "url": f"/api/v1/devices/{dev.id}/{route}/{p.name}"}
        if kind in ("event_image", "event_audio"):
            item["event_id"] = dev.event_of_image(p)
        out.append(item)
    return out


class CommandRequest(BaseModel):
    command: str = Field(..., max_length=150, examples=["STATUS", "SELFTEST", "SIM GAS 60", "SET sample_ms 100"],
                         description="Firmware command, see GET /api/v1/commands")
    timeout: float = Field(COMMAND_TIMEOUT, ge=1, le=60)


FIRMWARE_COMMANDS = {
    "PING": "Liveness check", "STATUS": "Full node health (JSON)", "CONFIG": "Current configuration (secrets masked)",
    "SET <key> <value>": "Change a configuration value (RAM)", "SAVE": "Persist configuration to flash",
    "RESET_CONFIG": "Factory defaults", "CAPTURE": "Take a picture (uploaded as event image)",
    "CAL_GAS": "Gas baseline calibration in clean air (~5 s)", "CAL_AC": "Reset acoustic noise floor",
    "SELFTEST": "Run self-test", "MAINT <0|1>": "Maintenance mode", "POWER <0|1>": "0 ACTIVE, 1 ECO",
    "SIM <GAS|AC|CLEAR> [s]": "Simulate a sensor alarm", "TIME <epoch>": "Set node clock",
    "LOG [n]": "Tail of the node event log on SD", "OTA_SD [FORCE]": "Firmware update from ota.bin on the node's SD card (OTA-mode builds only; refused during a leak event unless FORCE); result appears as status field ota after the reboot",
    "REBOOT": "Reboot the node",
}


def _run_command(dev: Device, command: str, timeout: float = COMMAND_TIMEOUT) -> dict:
    if dev.rebooting:
        raise HTTPException(status_code=503, detail="Node is rebooting; it re-registers in about 20-40 s")
    if command.strip().upper() == "REBOOT":
        # firmware >= 1.4.1 answers "OK rebooting in 1 s"; older firmware resets before answering
        try:
            reply = dev.send_command("REBOOT", timeout=5)
        except RuntimeError as e:
            if "not connected" in str(e):
                raise HTTPException(status_code=503, detail="Node command failed: %s" % e)
            reply = "OK (no reply: node reset before answering)"
        dev.mark_rebooting()
        return {"device_id": dev.id, "command": "REBOOT", "via": "socket", "ok": True, "reply": reply, "json": None}
    if not dev.supports_commands and dev.fw is not None:
        raise HTTPException(status_code=409, detail=f"Firmware {dev.fw} has no command channel (needs >= 1.1.0)")
    try:
        try:
            reply = dev.send_command(command, timeout=timeout)
            via = "socket"
        except RuntimeError as e:
            if "not connected" not in str(e):
                raise
            reply = dev.command_via_http(command, timeout=timeout)
            via = "http"
    except ValueError as e:
        raise HTTPException(status_code=422, detail=str(e))
    except (RuntimeError, requests.RequestException) as e:
        raise HTTPException(status_code=504 if "no reply" in str(e) else 503, detail=f"Node command failed: {e}")
    parsed = None
    if reply is not None and reply.lstrip().startswith("{"):
        try:
            parsed = json.loads(reply)
        except ValueError:
            pass
    return {"device_id": dev.id, "command": command, "via": via, "ok": not (reply or "").startswith("ERR"),
            "reply": reply, "json": parsed}


@app.get("/health", tags=["meta"])
def health():
    return {"status": "ok", "devices": len(registry.all())}


api = APIRouter(prefix="/api/v1", dependencies=[Depends(require_api_key)])


@api.get("/events", tags=["events"])
def list_events(after_seq: Optional[int] = Query(None, description="Return events with seq greater than this"),
                limit: int = Query(100, ge=1, le=EVENT_BUFFER_SIZE),
                device_id: Optional[str] = None,
                type: Optional[str] = Query(None, description="leak_event | event_image | node_online | node_offline")):
    evs = events.since(after_seq, EVENT_BUFFER_SIZE if type else limit, device_id)
    if type:
        evs = [e for e in evs if e["type"] == type][-limit:] if after_seq is None else \
              [e for e in evs if e["type"] == type][:limit]
    return {"count": len(evs),
            "next_after_seq": evs[-1]["seq"] if evs else (events.current_seq if after_seq is None
                                                          else min(after_seq, events.current_seq)),
            "events": evs}


@api.get("/events/stream", tags=["events"])
async def events_stream(request: Request,
                        after_seq: Optional[int] = Query(None, description="Replay from this seq; default: only new"),
                        device_id: Optional[str] = None):
    """Server-Sent Events: one event per leak event / event image / online change, `id` = seq."""
    async def gen():
        last = events.current_seq if after_seq is None else after_seq
        while not await request.is_disconnected():
            for ev in events.since(last, 500):
                last = ev["seq"]
                if device_id is None or ev["device_id"] == device_id:
                    yield f"id: {ev['seq']}\nevent: {ev['type']}\ndata: {json.dumps(ev)}\n\n"
            yield ": keep-alive\n\n"
            await asyncio.sleep(0.5)

    return StreamingResponse(gen(), media_type="text/event-stream",
                             headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"})


@api.get("/commands", tags=["commands"])
def list_commands():
    return FIRMWARE_COMMANDS


@api.get("/devices", tags=["devices"])
def list_devices():
    return [d.info() for d in registry.all()]


@api.get("/devices/{device_id}", tags=["devices"])
def get_device(device_id: str):
    return _device(device_id).info()


@api.get("/devices/{device_id}/status", tags=["node"])
def node_status(device_id: str):
    """Last heartbeat reported by the node (FW >= 1.1.0, every hb_s seconds)."""
    dev = _device(device_id)
    if not dev.status:
        raise HTTPException(status_code=404, detail="No heartbeat received from this node yet")
    return {"device_id": dev.id, "received_at": datetime.fromtimestamp(dev.status_at).isoformat(timespec="seconds"),
            "age_s": dev.status_age, "stale": dev.status_age > STATUS_STALE, "status": dev.status}


@api.get("/devices/{device_id}/events", tags=["events"])
def device_events(device_id: str, after_seq: Optional[int] = None,
                  limit: int = Query(100, ge=1, le=EVENT_BUFFER_SIZE)):
    dev = _device(device_id)
    evs = events.since(after_seq, limit, dev.id)
    return {"device_id": dev.id, "state": dev.event_state, "event_id": dev.event_id,
            "count": len(evs), "events": evs}


@api.post("/devices/{device_id}/command", tags=["commands"])
def device_command(device_id: str, body: CommandRequest):
    """Send a firmware command (see GET /api/v1/commands) and return the node's reply."""
    return _run_command(_device(device_id), body.command, body.timeout)


@api.post("/devices/{device_id}/self-test", tags=["commands"])
def device_self_test(device_id: str):
    return _run_command(_device(device_id), "SELFTEST", 30)


@api.post("/devices/{device_id}/calibrate/{sensor}", tags=["commands"])
def device_calibrate(device_id: str, sensor: str):
    """sensor: gas (clean air, node must be warmed up) | acoustic (quiet background)."""
    cmds = {"gas": "CAL_GAS", "acoustic": "CAL_AC"}
    if sensor not in cmds:
        raise HTTPException(status_code=422, detail="sensor must be gas or acoustic")
    return _run_command(_device(device_id), cmds[sensor], 30)


@api.post("/devices/{device_id}/maintenance", tags=["commands"])
def device_maintenance(device_id: str, on: bool = Query(...)):
    return _run_command(_device(device_id), f"MAINT {1 if on else 0}")


@api.post("/devices/{device_id}/power-mode", tags=["commands"])
def device_power_mode(device_id: str, mode: str = Query(..., description="ACTIVE or ECO")):
    mode = mode.upper()
    if mode not in ("ACTIVE", "ECO"):
        raise HTTPException(status_code=422, detail="mode must be ACTIVE or ECO")
    return _run_command(_device(device_id), f"POWER {1 if mode == 'ECO' else 0}")


@api.post("/devices/{device_id}/reboot", tags=["commands"])
def device_reboot(device_id: str):
    dev = _device(device_id)
    if dev.rebooting:
        raise HTTPException(status_code=503, detail="Node is already rebooting")
    try:
        dev.send_command("REBOOT", wait=False)
        via = "socket"
    except RuntimeError:
        if not NODE_TOKEN:
            raise HTTPException(status_code=503, detail="Socket down and AMB82_NODE_TOKEN not set")
        try:
            requests.get(f"http://{dev.ip}/reboot", params={"token": NODE_TOKEN}, timeout=5)
        except requests.RequestException:
            pass                                    # the node drops the connection while rebooting
        via = "http"
    dev.mark_rebooting()
    return {"device_id": dev.id, "command": "REBOOT", "via": via, "ok": True}


@api.get("/devices/{device_id}/sensor/latest", tags=["sensor"])
def sensor_latest(device_id: str):
    rec = _device(device_id).latest_record()
    if rec is None:
        raise HTTPException(status_code=404, detail="No sensor data yet")
    return rec


@api.get("/devices/{device_id}/sensor", tags=["sensor"])
def sensor_history(device_id: str,
                   after_seq: Optional[int] = Query(None, description="Return records with seq greater than this (incremental polling)"),
                   limit: int = Query(100, ge=1, le=SENSOR_BUFFER_SIZE)):
    dev = _device(device_id)
    recs = dev.records(after_seq, limit)
    return {"device_id": dev.id, "count": len(recs),
            # lower than the caller's cursor after a controller restart (seq reset) -> caller resets to 0
            "next_after_seq": recs[-1]["seq"] if recs else (
                dev.current_seq if after_seq is None else min(after_seq, dev.current_seq)),
            "records": recs}


@api.get("/devices/{device_id}/sensor.csv", tags=["sensor"])
def sensor_csv(device_id: str):
    dev = _device(device_id)
    if not dev.csv_path.exists():
        raise HTTPException(status_code=404, detail="No sensor data yet")
    return FileResponse(dev.csv_path, media_type="text/csv", filename=f"{dev.name}_{dev.ip}_sensor.csv")


@api.get("/devices/{device_id}/sensor/stream", tags=["sensor"])
async def sensor_stream(device_id: str, request: Request,
                        after_seq: Optional[int] = Query(None, description="Replay from this seq; default: only new data")):
    """Server-Sent Events: one `sensor` event per record, `id` = seq."""
    dev = _device(device_id)

    async def events():
        last = dev.current_seq if after_seq is None else after_seq
        while not await request.is_disconnected():
            for rec in dev.records(last, 500):
                last = rec["seq"]
                yield f"id: {rec['seq']}\nevent: sensor\ndata: {json.dumps(rec)}\n\n"
            yield ": keep-alive\n\n"
            await asyncio.sleep(0.5)

    return StreamingResponse(events(), media_type="text/event-stream",
                             headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"})


@api.get("/devices/{device_id}/images", tags=["images"])
def list_images(device_id: str, limit: int = Query(50, ge=1, le=MAX_IMAGES)):
    return _file_list(_device(device_id), "image", limit)


@api.get("/devices/{device_id}/images/latest", tags=["images"], response_class=FileResponse)
def latest_image(device_id: str):
    dev = _device(device_id)
    files = dev.list_files("image")
    return _serve(dev, "image", files[-1] if files else None, no_cache=True)


@api.post("/devices/{device_id}/images/capture", tags=["images"], response_class=FileResponse)
def capture_image(device_id: str):
    dev = _device(device_id)
    path = dev.capture_image()
    if path is None:
        raise HTTPException(status_code=502, detail="Device did not return a picture")
    return _serve(dev, "image", path, no_cache=True)


@api.get("/devices/{device_id}/images/{filename}", tags=["images"], response_class=FileResponse)
def get_image(device_id: str, filename: str):
    dev = _device(device_id)
    return _serve(dev, "image", dev.media_file("image", filename))


@api.get("/devices/{device_id}/event-images", tags=["events"])
def list_event_images(device_id: str, limit: int = Query(50, ge=1, le=MAX_EVENT_IMAGES),
                      event_id: Optional[str] = None):
    items = _file_list(_device(device_id), "event_image", MAX_EVENT_IMAGES if event_id else limit)
    if event_id:
        items = [i for i in items if i["event_id"] == event_id][:limit]
    return items


@api.get("/devices/{device_id}/event-images/latest", tags=["events"], response_class=FileResponse)
def latest_event_image(device_id: str):
    dev = _device(device_id)
    files = dev.list_files("event_image")
    return _serve(dev, "event_image", files[-1] if files else None, no_cache=True)


@api.get("/devices/{device_id}/event-images/{filename}", tags=["events"], response_class=FileResponse)
def get_event_image(device_id: str, filename: str):
    dev = _device(device_id)
    return _serve(dev, "event_image", dev.media_file("event_image", filename))


@api.get("/devices/{device_id}/event-audio", tags=["events"])
def list_event_audio(device_id: str, limit: int = Query(50, ge=1, le=MAX_EVENT_AUDIO),
                     event_id: Optional[str] = None):
    """Sound clips the node recorded during leak events while the server was unreachable."""
    items = _file_list(_device(device_id), "event_audio", MAX_EVENT_AUDIO if event_id else limit)
    if event_id:
        items = [i for i in items if i["event_id"] == event_id][:limit]
    return items


@api.get("/devices/{device_id}/event-audio/latest", tags=["events"], response_class=FileResponse)
def latest_event_audio(device_id: str):
    dev = _device(device_id)
    files = dev.list_files("event_audio")
    return _serve(dev, "event_audio", files[-1] if files else None, no_cache=True)


@api.get("/devices/{device_id}/event-audio/{filename}", tags=["events"], response_class=FileResponse)
def get_event_audio(device_id: str, filename: str):
    dev = _device(device_id)
    return _serve(dev, "event_audio", dev.media_file("event_audio", filename))


@api.get("/devices/{device_id}/audio", tags=["audio"])
def list_audio(device_id: str, limit: int = Query(50, ge=1, le=MAX_AUDIO_SEGMENTS)):
    return _file_list(_device(device_id), "audio", limit)


@api.get("/devices/{device_id}/audio/latest", tags=["audio"], response_class=FileResponse)
def latest_audio(device_id: str):
    dev = _device(device_id)
    files = dev.list_files("audio")
    return _serve(dev, "audio", files[-1] if files else None, no_cache=True)


@api.get("/devices/{device_id}/audio/{filename}", tags=["audio"], response_class=FileResponse)
def get_audio(device_id: str, filename: str):
    dev = _device(device_id)
    return _serve(dev, "audio", dev.media_file("audio", filename))


app.include_router(api)


# ---------------------------------------------------------------- device registration server
def register_device(ip: str, name: str, client: str, node_id: Optional[str] = None,
                    fw: Optional[str] = None, boot: Optional[str] = None) -> tuple[int, str]:
    """Returns (http_status, body). Every rejection is logged with its reason."""
    try:
        dev_ip = ipaddress.ip_address(ip)
    except ValueError:
        log.warning("Registration rejected: invalid ip=%r from %s", ip, client)
        return 400, "INVALID IP"
    if dev_ip not in DEVICE_SUBNET or not _in_device_subnet(client):
        log.warning("Registration rejected: ip=%s / sender=%s not in AMB82_DEVICE_SUBNET=%s",
                    dev_ip, client, DEVICE_SUBNET)
        return 403, "FORBIDDEN"
    safe = _safe_name(name)
    if safe != name:
        log.info("Device name %r stored as %r", name, safe)
    dev, created = registry.register(str(dev_ip), safe, node_id, fw)
    log.info("%s device %s (%s) id=%s fw=%s boot=%s", "Registered" if created else "Re-registered",
             safe, dev_ip, dev.node_id, dev.fw, boot)
    # same node (MAC) seen at another IP: its DHCP address changed
    if dev.node_id:
        for other in registry.all():
            if other is not dev and other.node_id == dev.node_id:
                log.warning("Node %s moved from %s to %s (DHCP address changed?)", dev.node_id, other.ip, dev.ip)
                events.publish(dev, "node_ip_changed", {"node_id": dev.node_id, "old_ip": other.ip, "new_ip": dev.ip})
    boot_n = int(boot) if boot and boot.isdigit() else None
    was_rebooting = dev.rebooting
    dev.rebooting_until = 0.0
    if boot_n is not None:
        if dev.boot_count is not None and boot_n != dev.boot_count:
            # the node restarted (REBOOT, power cut, watchdog): its old socket is dead
            dev.reset_connection("node rebooted (boot %d -> %d)" % (dev.boot_count, boot_n))
            events.publish(dev, "node_rebooted", {"boot_count": boot_n, "requested": was_rebooting})
        dev.boot_count = boot_n
    return 200, "DONE"


def _device_for_client(client: str, qs: dict) -> Optional[Device]:
    """Node messages are matched by sender IP. A node that registered before this service
    (re)started is registered again from its heartbeat, so it never has to be reset."""
    dev = registry.get(client)
    if dev is None and qs.get("name"):
        code, _ = register_device(client, qs["name"], client, qs.get("id"), qs.get("fw"))
        dev = registry.get(client) if code == 200 else None
    return dev


class RegistrationHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        client = self.client_address[0]
        url = urlparse(self.path)
        if url.path in ("/node-status", "/up-data"):
            log.debug("Device request from %s: %s", client, self.path)    # periodic: keep the log readable
        else:
            log.info("Device request from %s: %s", client, self.path)
        if not _in_device_subnet(client):
            return self._reply(403, "FORBIDDEN")
        qs = _qs_flat(url.query)
        if url.path == "/add-device":
            return self._reply(*register_device(qs.get("ip", ""), qs.get("name", ""), client,
                                                qs.get("id"), qs.get("fw"), qs.get("boot")))
        if url.path == "/node-status":
            dev = _device_for_client(client, qs)
            if dev is None:
                return self._reply(404, "UNKNOWN DEVICE")
            dev.update_identity(qs.get("id"), qs.get("fw"))
            dev.on_status(_status_values(qs))
            return self._reply(200, "OK")
        if url.path == "/node-event":
            dev = _device_for_client(client, qs)
            if dev is None:
                return self._reply(404, "UNKNOWN DEVICE")
            dev.on_event(_status_values(qs))
            return self._reply(200, "OK")
        if url.path == "/up-data":                   # firmware HTTP mode (STREAMING_SOCKET false)
            dev = registry.get(client)
            if dev is None:
                return self._reply(404, "UNKNOWN DEVICE")
            dev.last_data_at = time.time()
            dev._ingest(qs.get("data", "").split(PACKET_SEP))
            return self._reply(200, "OK")
        return self._reply(404, "NOT FOUND")

    def do_POST(self):
        """Event upload: 'POST /' with headers Device / Event / IP / Type and a JPEG picture or
        (Type: audio/wav, firmware >= 1.6.0) a sound clip recorded while this server was unreachable."""
        client = self.client_address[0]
        if not _in_device_subnet(client) or urlparse(self.path).path != "/":
            return self._reply(403 if not _in_device_subnet(client) else 404, "REJECTED")
        dev = registry.get(client)
        if dev is None:
            return self._reply(404, "UNKNOWN DEVICE")
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = 0
        if length <= 0 or length > MAX_UPLOAD_BYTES:
            return self._reply(413 if length > 0 else 411, "BAD LENGTH")
        body = b""
        try:
            while len(body) < length:
                chunk = self.rfile.read(min(65536, length - len(body)))
                if not chunk:
                    break
                body += chunk
        except OSError as e:
            log.warning("Image upload from %s interrupted: %s", client, e)
            return
        event_id = (self.headers.get("Event") or "").strip()
        if (self.headers.get("Type") or "").strip().lower().startswith("audio/"):
            wav = _extract_wav(body)
            if wav is None:
                log.warning("Audio upload from %s (%d bytes) contains no WAV", client, len(body))
                return self._reply(400, "NO WAV")
            path = dev.save_event_audio(wav, event_id)
            log.info("Event sound clip from %s event=%s -> %s (%d bytes)", dev.name, event_id or "-", path.name, len(wav))
            return self._reply(200, "OK")
        jpeg = _extract_jpeg(body)
        if jpeg is None:
            log.warning("Upload from %s (%d bytes) contains no JPEG", client, len(body))
            return self._reply(400, "NO JPEG")
        event_id = (self.headers.get("Event") or "").strip()
        path = dev.save_event_image(jpeg, event_id)
        log.info("Event image from %s event=%s -> %s (%d bytes)", dev.name, event_id or "-", path.name, len(jpeg))
        self._reply(200, "OK")

    def _reply(self, code: int, body: str) -> None:
        if code == 200:
            # Node FW >= 1.1.3 sets its clock from this (no NTP server needed on the device network).
            # Older firmware only checks the status code, so the extra text is harmless.
            body = f"{body} T={int(time.time())}"
        data = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-type", "text/html")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        try:
            self.wfile.write(data)
        except OSError:
            pass                                  # the firmware often closes right after sending

    def log_message(self, fmt, *args):
        log.debug("reg-server: " + fmt, *args)


def start_registration_server() -> ThreadingHTTPServer:
    try:
        srv = ThreadingHTTPServer((DEVICE_NET_IP, REG_PORT), RegistrationHandler)
    except OSError as e:
        raise SystemExit(
            f"Cannot listen on {DEVICE_NET_IP}:{REG_PORT} ({e}).\n"
            f"  - Is another copy of the Sentinel Node API already running? Stop it first.\n"
            f"  - Is {DEVICE_NET_IP} really assigned to this host? Check with: ip addr")
    threading.Thread(target=srv.serve_forever, name="reg-server", daemon=True).start()
    log.info("Waiting for Sentinel Nodes at http://%s:%d/add-device "
             "(reset a node that booted before this service started)", DEVICE_NET_IP, REG_PORT)
    return srv


def main() -> None:
    DATA_DIR.mkdir(parents=True, exist_ok=True)   # old data is kept (the original deleted it on start)
    if not API_KEY:
        log.warning("AMB82_API_KEY is not set: /api/v1 is open to anyone who can reach %s:%d", API_HOST, API_PORT)
    reg_server = start_registration_server()
    threading.Thread(target=registry.monitor, name="online-monitor", daemon=True).start()
    if EVENT_WEBHOOK:
        log.info("Events are forwarded to %s", EVENT_WEBHOOK)
    log.info("Sentinel Node API on http://%s:%d/api/v1  (docs: /docs)", API_HOST, API_PORT)
    try:
        uvicorn.run(app, host=API_HOST, port=API_PORT, log_level="info")
    finally:
        reg_server.shutdown()
        for dev in registry.all():
            dev.shutdown()


if __name__ == "__main__":
    main()
