"""End-to-end fixtures: Mosquitto + SN Device Driver + emulated node + media feeder + HW Interface + node-gateway emulator."""
import math
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import wave
from datetime import datetime

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path[:0] = [REPO, os.path.join(REPO, "tests", "common")]

from edge_server import EdgeServer            # noqa: E402
from node_emulator import NodeEmulator        # noqa: E402
from node_gateway_emu import NodeGatewayEmu   # noqa: E402

NODE_IP, NODE_MAC, NODE_NAME = "127.0.0.44", "00E04C0F5C9D", "EZ_SN_044"
DEVICE_ID = "SN-" + NODE_MAC
SEG_S = 2.0                                       # length of the synthetic driver audio segments


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def tiny_jpeg(w=1280, h=720):
    sof = b"\xff\xc0" + struct.pack(">HBHHB", 17, 8, h, w, 3) + b"\x01\x22\x00\x02\x11\x01\x03\x11\x01"
    return b"\xff\xd8\xff\xe0" + struct.pack(">H", 16) + b"JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00" + sof + b"\xff\xd9"


class MediaFeeder(threading.Thread):
    """Plays the SN Device Driver's recorder and picture fetch: writes finished audio segments and periodic
    pictures into the driver's folder of the node, named by local start time as the driver does."""

    def __init__(self, folder, keep_audio=None):
        super().__init__(daemon=True)
        self.folder = folder
        self.stop = threading.Event()
        # like the driver's MAX_AUDIO_SEGMENTS: keep only the newest N finished segments
        self.keep_audio = keep_audio if keep_audio is not None else int(os.getenv("HAL_AUDIO_KEEP", "3"))
        self.gap_s = float(os.getenv("HAL_AUDIO_GAP", "0"))     # missing audio between segments, like a restart

    @staticmethod
    def stamp(t):
        return datetime.fromtimestamp(t).strftime("%Y%m%d_%H%M%S_%f")

    def run(self):
        os.makedirs(os.path.join(self.folder, "audio"), exist_ok=True)
        os.makedirs(os.path.join(self.folder, "images"), exist_ok=True)
        seg_start = math.floor(time.time())
        last_img = 0
        while not self.stop.is_set():
            now = time.time()
            if now >= seg_start + SEG_S:                       # segment finished: write it like the recorder
                n = int(16000 * SEG_S)
                frames = b"".join(struct.pack("<h", int(3000 * math.sin(2 * math.pi * 440 * i / 16000))) for i in range(n))
                p = os.path.join(self.folder, "audio", self.stamp(seg_start) + ".wav")
                with wave.open(p + ".tmp", "wb") as w:
                    w.setnchannels(1)
                    w.setsampwidth(2)
                    w.setframerate(16000)
                    w.writeframes(frames)
                os.replace(p + ".tmp", p)
                seg_start += SEG_S + self.gap_s
                segs = sorted(f for f in os.listdir(os.path.join(self.folder, "audio")) if f.endswith(".wav"))
                for old in segs[:-self.keep_audio]:
                    os.remove(os.path.join(self.folder, "audio", old))
            if now - last_img >= 1.0:
                last_img = now
                with open(os.path.join(self.folder, "images", self.stamp(now) + ".jpg"), "wb") as f:
                    f.write(tiny_jpeg())
            self.stop.wait(0.1)


@pytest.fixture(scope="module")
def bench():
    if not shutil.which("mosquitto"):
        pytest.skip("mosquitto not installed")
    tmp = tempfile.mkdtemp(prefix="hal-")
    port = free_port()
    conf = os.path.join(tmp, "mosquitto.conf")
    open(conf, "w").write("listener %d 127.0.0.1\nallow_anonymous true\npersistence false\n" % port)
    broker = subprocess.Popen(["mosquitto", "-c", conf], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    srv = EdgeServer(os.path.join(tmp, "driver"), api_key="test-key").start()
    node = NodeEmulator(ip=NODE_IP, reg_port=srv.reg_port, name=NODE_NAME, mac=NODE_MAC, fw="1.6.1",
                        hb_s=1.0, event_hold_s=2.0, flash_mb=16).start()
    folder = os.path.join(tmp, "driver", "%s_%s" % (NODE_NAME, NODE_IP))
    deadline = time.time() + 15
    while not os.path.isdir(folder) and time.time() < deadline:
        time.sleep(0.2)
    feeder = MediaFeeder(folder)
    feeder.start()

    from hw_interface.config import Settings
    from hw_interface.service import HwInterface

    def settings():
        return Settings(driver_api=srv.api, driver_key="test-key", driver_data_dir=os.path.join(tmp, "driver"),
                        data_root=os.path.join(tmp, "data-area"), state_db=os.path.join(tmp, "hwi.sqlite"),
                        mqtt_host="127.0.0.1", mqtt_port=port, chem01_s=2, aco01_every_s=3, aco01_len_s=1,
                        img01_every_s=2, part_s=2, event_img_every_s=3, min_event_images=2, audio_giveup_s=10,
                        capture_timeout_s=8, poll_events_s=0.3, poll_records_s=0.5, poll_status_s=0.5,
                        poll_devices_s=1.0, chem_end_grace_s=1.5, recovery_window_s=90, recovery_every_s=1.5,
                        live_lag_s=5, recovery_quiet_s=3)
    gw = NodeGatewayEmu("127.0.0.1", port, os.path.join(tmp, "data-area"))
    hwi = HwInterface(settings()).start()
    b = type("Bench", (), {})()
    b.tmp, b.port, b.srv, b.node, b.gw, b.hwi, b.settings, b.feeder = tmp, port, srv, node, gw, hwi, settings, feeder
    yield b
    feeder.stop.set()
    b.hwi.close()
    gw.close()
    node.stop()
    srv.stop()
    broker.terminate()
    shutil.rmtree(tmp, ignore_errors=True)
