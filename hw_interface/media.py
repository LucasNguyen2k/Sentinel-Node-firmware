"""Audio cutting from the SN Device Driver's recordings, and picture size from JPEG headers."""
import glob
import io
import os
import struct
import wave
from datetime import datetime


def driver_stamp_to_epoch(name: str):
    """The SN Device Driver names files by local time: 20261022_141830_002113.wav -> epoch seconds."""
    try:
        return datetime.strptime(os.path.basename(name)[:22], "%Y%m%d_%H%M%S_%f").astimezone().timestamp()
    except ValueError:
        return None


class AudioIndex:
    """The driver's audio folder of one node: consecutive WAV segments, each named by its start time."""

    def __init__(self, folder):
        self.folder = folder
        self._cache = {}

    def segments(self):
        out = []
        for p in sorted(glob.glob(os.path.join(self.folder, "*.wav"))):
            start = driver_stamp_to_epoch(p)
            if start is None:
                continue
            info = self._cache.get(p)
            if info is None:
                try:
                    with wave.open(p, "rb") as w:
                        info = (w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes())
                except (wave.Error, EOFError, OSError):
                    continue
                self._cache[p] = info
            ch, sw, rate, n = info
            out.append((start, start + n / float(rate), p, info))
        return out

    def covers(self, t0, t1, max_gap=0.05) -> bool:
        """True if the recordings contain [t0, t1) with no gap longer than max_gap seconds."""
        t = t0
        for s, e, _, _ in self.segments():
            if e <= t:
                continue
            if s > t + max_gap:
                return False
            t = e
            if t >= t1:
                return True
        return False

    def cut(self, t0, t1):
        """(WAV bytes of [t0, t1) in the recordings' own format, milliseconds of silence inserted), or (None, 0).
        Gaps between recordings are filled with silence so the file covers exactly [t0, t1); audio that two
        recordings both contain is taken once."""
        frames, params, cursor, gap_frames = [], None, t0, 0
        for s, e, p, (ch, sw, rate, n) in self.segments():
            if e <= cursor or s >= t1:
                continue
            params = params or (ch, sw, rate)
            frame = ch * sw
            if s > cursor:                                   # a gap before this recording
                k = int(round((s - cursor) * rate))
                frames.append(b"\x00" * (k * frame))
                gap_frames += k
                cursor = s
            a = max(0, int(round((cursor - s) * rate)))
            b = min(n, int(round((t1 - s) * rate)))
            if b <= a:
                continue
            with wave.open(p, "rb") as w:
                w.setpos(a)
                frames.append(w.readframes(b - a))
            cursor = s + b / float(rate)
        if not params:
            return None, 0
        ch, sw, rate = params
        want = int(round((t1 - t0) * rate))
        have = sum(len(f) for f in frames) // (ch * sw)
        if have < want:                                      # a gap at the end
            frames.append(b"\x00" * ((want - have) * ch * sw))
            gap_frames += want - have
        data = b"".join(frames)[: want * ch * sw]
        frames = [data]
        buf = io.BytesIO()
        with wave.open(buf, "wb") as w:
            w.setnchannels(ch)
            w.setsampwidth(sw)
            w.setframerate(rate)
            w.writeframes(data)
        return buf.getvalue(), int(round(gap_frames * 1000.0 / rate))


def jpeg_size(data: bytes):
    """(width, height) from a JPEG's SOF marker, or (None, None)."""
    if data[:2] != b"\xff\xd8":
        return None, None
    i = 2
    while i + 9 < len(data):
        if data[i] != 0xFF:
            i += 1
            continue
        marker = data[i + 1]
        if marker in (0xD8, 0x01) or 0xD0 <= marker <= 0xD7:
            i += 2
            continue
        seg_len = struct.unpack(">H", data[i + 2:i + 4])[0]
        if marker in (0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7, 0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF):
            h, w = struct.unpack(">HH", data[i + 5:i + 9])
            return w, h
        i += 2 + seg_len
    return None, None


def _alaw_to_linear(a):
    """G.711 A-law byte -> 16-bit sample (ITU-T G.711, as in the reference implementation g711.c)."""
    a ^= 0x55
    t = (a & 0x0F) << 4
    seg = (a & 0x70) >> 4
    if seg == 0:
        t += 8
    elif seg == 1:
        t += 0x108
    else:
        t = (t + 0x108) << (seg - 1)
    return t if a & 0x80 else -t


def _ulaw_to_linear(u):
    """G.711 mu-law byte -> 16-bit sample."""
    u = ~u & 0xFF
    t = ((u & 0x0F) << 3) + 0x84
    t <<= (u & 0x70) >> 4
    return (0x84 - t) if u & 0x80 else (t - 0x84)


_ALAW = [_alaw_to_linear(i) for i in range(256)]
_ULAW = [_ulaw_to_linear(i) for i in range(256)]


def wav_to_pcm16(data: bytes) -> bytes:
    """A WAV in G.711 A-law (format 6) or mu-law (7), as the node stores its outage clips, converted to 16-bit
    PCM, the format of every ACO dataset. PCM input (format 1) is returned unchanged."""
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        return data
    pos, fmt, ch, rate, payload = 12, None, 1, 16000, None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            fmt, ch, rate = struct.unpack("<HHI", body[:8])
        elif cid == b"data":
            payload = body
        pos += 8 + size + (size & 1)
    if fmt not in (6, 7) or payload is None:
        return data
    table = _ALAW if fmt == 6 else _ULAW
    pcm = struct.pack("<%dh" % len(payload), *(table[b] for b in payload))
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(ch)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm)
    return buf.getvalue()
