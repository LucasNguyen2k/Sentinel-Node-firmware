"""Event sound clips a node recorded while the Edge Server was unreachable (firmware >= 1.6.0)."""
import pytest

pytestmark = pytest.mark.req("FW-07", "FW-08", "FW-10")


def test_event_sound_clip_stored_and_published(edge, make_node):
    n = make_node(61)
    n.register()
    wav = n.clip_wav()
    assert n.post_clip("E5_2")[0] == 200
    evs = edge.get("/devices/127.0.0.61/events").json()["events"]
    ev = [e for e in evs if e["type"] == "event_audio"]
    assert ev and ev[0]["event_id"] == "E5_2" and ev[0]["bytes"] == len(wav)
    clips = edge.get("/devices/127.0.0.61/event-audio?event_id=E5_2").json()
    assert len(clips) == 1 and clips[0]["event_id"] == "E5_2" and clips[0]["file"].endswith(".wav")
    got = edge.get("/devices/127.0.0.61/event-audio/" + clips[0]["file"]).content
    assert got == wav                                          # envelope removed, bytes intact
    assert got[:4] == b"RIFF" and got[20] == 6                 # A-law WAV
    assert edge.get("/devices/127.0.0.61/event-audio?event_id=E5_9").json() == []
    assert edge.get("/devices/127.0.0.61/event-images").json() == []   # not mixed up with pictures


def test_event_audio_rejections(edge, make_node):
    n = make_node(62)
    n.register()
    hdr = {"Type": "audio/wav", "Event": "E1_1"}
    body = b"no wav in here"
    assert n.http("POST", "/", body=body, headers=dict(hdr, **{"Content-Length": str(len(body))}))[0] == 400
    cut = n.clip_wav()[:1000]                                   # RIFF size says 80 KB, only 1 KB arrived
    assert n.http("POST", "/", body=cut, headers=dict(hdr, **{"Content-Length": str(len(cut))}))[0] == 400
    assert edge.get("/devices/127.0.0.62/event-audio").json() == []
