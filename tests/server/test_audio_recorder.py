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


def _log(srv):
    try:
        return open(srv.log_path).read()
    except OSError:
        return ""


def test_recorder_restarts_when_no_audio_arrives(tmp_path):
    """A stream that never delivers audio: ffmpeg would wait forever; the watchdog restarts the recording."""
    fifo = str(tmp_path / "stream.raw")
    os.mkfifo(fifo)                                   # nobody ever writes to it
    srv = EdgeServer(str(tmp_path / "d"), AMB82_AUDIO_RECORDER="ffmpeg", AMB82_AUDIO_SEGMENT_SECONDS="1",
                     AMB82_AUDIO_START_TIMEOUT_S="2", AMB82_AUDIO_INPUT="-f s16le -ar 16000 -ac 1 -i " + fifo).start()
    node = NodeEmulator(ip="127.0.0.92", reg_port=srv.reg_port, name="EZ_SN_092", mac="00E04C000092",
                        fw="1.6.1", hb_s=1).start()
    try:
        deadline = time.time() + 40
        while time.time() < deadline and _log(srv).count("No audio from 127.0.0.92") < 2:
            time.sleep(0.5)
        assert _log(srv).count("No audio from 127.0.0.92") >= 2, _log(srv)[-2000:]
    finally:
        node.stop()
        srv.stop()


def test_recorder_restarts_when_the_stream_stalls(tmp_path):
    """Audio flows, then stops while the stream stays open (a stuck stream): detected as a stall."""
    import threading
    fifo = str(tmp_path / "stream.raw")
    os.mkfifo(fifo)
    stop = threading.Event()

    def feed():
        try:
            with open(fifo, "wb") as f:               # blocks until ffmpeg opens the stream
                for _ in range(40):                   # 4 s of audio at real-time pace
                    f.write(b"\x00\x00" * 1600)
                    f.flush()
                    time.sleep(0.1)
                stop.wait(30)                         # then nothing more, stream still open
        except (BrokenPipeError, OSError):
            pass
    t = threading.Thread(target=feed, daemon=True)
    t.start()
    srv = EdgeServer(str(tmp_path / "d"), AMB82_AUDIO_RECORDER="ffmpeg", AMB82_AUDIO_SEGMENT_SECONDS="1",
                     AMB82_AUDIO_STALL_S="2", AMB82_AUDIO_START_TIMEOUT_S="20",
                     AMB82_AUDIO_INPUT="-f s16le -ar 16000 -ac 1 -i " + fifo).start()
    node = NodeEmulator(ip="127.0.0.93", reg_port=srv.reg_port, name="EZ_SN_093", mac="00E04C000093",
                        fw="1.6.1", hb_s=1).start()
    try:
        deadline = time.time() + 40
        while time.time() < deadline and "Audio from 127.0.0.93 stalled" not in _log(srv):
            time.sleep(0.5)
        assert "Audio from 127.0.0.93 stalled" in _log(srv), _log(srv)[-2000:]
        files = glob.glob(os.path.join(str(tmp_path / "d"), "*_127.0.0.93", "audio", "*.wav"))
        assert files, "segments recorded before the stall are kept"
    finally:
        stop.set()
        node.stop()
        srv.stop()


def test_lost_packets_are_filled_with_silence(tmp_path):
    """Over UDP the node's stream loses packets; the segments must still hold their full length (the holes are
    filled with silence at the right place), so the audio stays aligned with the clock."""
    lossy = '-re -f lavfi -i "sine=frequency=440:sample_rate=16000,aselect=lt(mod(t\\,1)\\,0.9)"'   # loses 10 %
    srv = EdgeServer(str(tmp_path), AMB82_AUDIO_RECORDER="ffmpeg", AMB82_AUDIO_SEGMENT_SECONDS="2",
                     AMB82_AUDIO_INPUT=lossy).start()
    node = NodeEmulator(ip="127.0.0.94", reg_port=srv.reg_port, name="EZ_SN_094", mac="00E04C000094",
                        fw="1.6.1", hb_s=1).start()
    try:
        deadline = time.time() + 25
        files = []
        while time.time() < deadline:
            files = sorted(glob.glob(os.path.join(str(tmp_path), "*_127.0.0.94", "audio", "*.wav")))
            if len(files) >= 3:
                break
            time.sleep(0.5)
        assert len(files) == 3, files
        prev, total = None, 0.0
        for f in files:
            start = datetime.strptime(os.path.basename(f)[:22], "%Y%m%d_%H%M%S_%f").timestamp()
            with wave.open(f) as w:
                dur = w.getnframes() / 16000.0
            assert 1.85 < dur < 2.2, (f, dur)                             # one 64 ms source block either way
            if prev is not None:
                assert abs(start - prev) < 0.1, (f, start - prev)
            prev, total = start + dur, total + dur
        assert abs(total - 6.0) < 0.15, total                             # without the filling: about 5.4 s
    finally:
        node.stop()
        srv.stop()
