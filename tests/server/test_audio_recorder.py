"""SN Device Driver audio recorder: one continuous ffmpeg recording split into segments that join without gaps."""
import glob
import os
import shutil
import sys
import time
import wave
from datetime import datetime

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "common"))
from edge_server import EdgeServer        # noqa: E402
from node_emulator import NodeEmulator    # noqa: E402

pytestmark = pytest.mark.skipif(not shutil.which("ffmpeg"), reason="ffmpeg not installed")


def test_segments_join_without_gaps(tmp_path):
    srv = EdgeServer(str(tmp_path), AMB82_AUDIO_RECORDER="ffmpeg", AMB82_AUDIO_SEGMENT_SECONDS="2",
                     AMB82_AUDIO_INPUT="-re -f lavfi -i sine=frequency=440:sample_rate=16000").start()
    node = NodeEmulator(ip="127.0.0.91", reg_port=srv.reg_port, name="EZ_SN_091", mac="00E04C000091",
                        fw="1.6.1", hb_s=1).start()
    try:
        t0 = time.time()
        deadline = t0 + 20
        files = []
        while time.time() < deadline:
            files = sorted(glob.glob(os.path.join(str(tmp_path), "*_127.0.0.91", "audio", "*.wav")))
            if len(files) >= 3:
                break
            time.sleep(0.5)
        assert len(files) == 3, files                                   # newest 3 kept (MAX_AUDIO_SEGMENTS)
        prev = None
        for f in files:
            start = datetime.strptime(os.path.basename(f)[:22], "%Y%m%d_%H%M%S_%f").timestamp()
            with wave.open(f) as w:
                assert w.getframerate() == 16000 and w.getnchannels() == 1 and w.getsampwidth() == 2
                dur = w.getnframes() / 16000.0
            assert 1.9 < dur < 2.1, dur
            if prev is not None:
                assert abs(start - prev) < 0.005, (f, start - prev)       # no gap, no overlap
            prev = start + dur
            assert t0 < start < time.time()                              # wall-clock stamps
        assert not [n for n in os.listdir(os.path.dirname(files[0])) if n.endswith(".part")]
    finally:
        node.stop()
        srv.stop()
