"""L4: storage behaviour of a running node - offline buffer, file system, pictures, event log (OD-09).

Outages are simulated on the node with SIM OFFLINE (firmware >= 1.5.2): it buffers its records and
treats picture uploads as failed, while the command channel and heartbeats keep working, so the test
can drive and observe everything through the Edge Server.
"""
import collections

import pytest

pytestmark = pytest.mark.hil


def node_status(bench):
    return bench.cmd("STATUS")["json"]


def pending_key(st):
    return "fs_buf_pending" if st.get("flash_mb", 16) >= 32 else "flash_buf_pending"


def node_config(bench):
    """CONFIG reply 'key=value' lines -> dict"""
    out = {}
    for line in bench.cmd("CONFIG")["reply"].splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out


@pytest.mark.req("FW-03", "FW-13")
def test_sampling_rate_nominal(bench):
    """The node samples at its configured rate (1.6.0 sampled at 0.15 Hz on a board without AHT20)."""
    cfg = node_config(bench)
    want = 1000.0 / float(cfg.get("sample_ms", "50"))
    st = node_status(bench)
    if "sample_rate_hz" not in st:
        pytest.skip("firmware before 1.6.1 does not report sample_rate_hz")
    rate = float(bench.wait_for(lambda: node_status(bench)["sample_rate_hz"] or None, 30, "measured sample rate"))
    assert rate >= 0.8 * want, "sampling at %.2f Hz, configured %.1f Hz" % (rate, want)


@pytest.mark.req("FW-08", "FW-20", "FW-15")
def test_offline_records_buffered_and_replayed_without_loss(bench):
    st0 = node_status(bench)
    assert st0.get("time", 0) > 1600000000, "node clock not set"
    key = pending_key(st0)
    t0 = st0["time"]
    assert bench.cmd("SIM OFFLINE 20")["reply"].startswith("OK sim OFFLINE")
    bench.wait_for(lambda: node_status(bench).get(key, 0) > 0, 15, "records kept in the offline buffer")
    assert node_status(bench)["sim_offline"] == 1
    t1 = t0 + 20
    bench.wait_for(lambda: node_status(bench)["sim_offline"] == 0 and node_status(bench).get(key, 1) == 0, 20 + 120,
                   "offline buffer replayed")
    recs = bench.get("/devices/%s/sensor?limit=10000" % bench.ip).json()["records"]
    inside = [r for r in recs if (r.get("fields") or {}).get("device_ts") and t0 + 2 <= r["fields"]["device_ts"] <= t1 - 2]
    cfg = node_config(bench)
    cycle_s = max(float(cfg.get("upload_ms", "900")), 800.0) / 1000.0     # one record kept per upload cycle
    expected = int(0.6 * (t1 - t0 - 4) / cycle_s)
    st1 = node_status(bench)
    assert len(inside) >= expected, ("only %d records from inside the outage, expected >= %d "
                                     "(sample_rate_hz=%s, up_cycle_ms=%s, upload_ms=%s, power_mode=%s)" % (
                                         len(inside), expected, st1.get("sample_rate_hz"), st1.get("up_cycle_ms"),
                                         cfg.get("upload_ms"), st1.get("power_mode")))
    dup = [raw for raw, n in collections.Counter(r["raw"] for r in inside).items() if n > 1]
    assert not dup, "records delivered twice: %s" % dup[:3]
    assert all(r["ts_source"] == "device" for r in inside)          # original time kept


@pytest.mark.req("FW-08", "FW-02")
def test_file_system_round_trip(bench):
    st = node_status(bench)
    if st.get("flash_mb", 16) < 32:
        pytest.skip("16 MB flash: no file system on this board")
    assert st["fs_ok"] == 1 and st["fs_total_kb"] > 15000
    r = bench.cmd("FS_TEST", timeout=40)["reply"]
    assert r.startswith("OK 64 KB written"), r
    assert bench.cmd("FS_DF")["reply"].startswith("OK used")


@pytest.mark.req("FW-07", "FW-08", "FW-20")
def test_picture_kept_during_outage_and_delivered_after(bench):
    st = node_status(bench)
    if st.get("flash_mb", 16) < 32 or st.get("cam_ok") != 1:
        pytest.skip("needs a 32 MB board with a working camera")
    seq = bench.seq()
    bench.cmd("SIM OFFLINE 15")
    r = bench.cmd("CAPTURE")["reply"]
    assert r.startswith("OK capture "), r
    eid = r.split()[-1]
    bench.wait_for(lambda: node_status(bench).get("fs_pend_img", 0) >= 1, 25, "picture kept as pending")
    bench.wait_for(lambda: [e for e in bench.events(seq) if e["type"] == "event_image" and e["event_id"] == eid],
                   15 + 90, "pending picture delivered after the outage")
    bench.wait_for(lambda: node_status(bench).get("fs_pend_img", 1) == 0, 60, "no pictures left pending")


@pytest.mark.destructive
@pytest.mark.req("FW-08", "FW-18")
def test_event_log_survives_reboot(bench):
    seq = bench.seq()
    bench.cmd("SIM GAS 3")
    leak = bench.wait_for(lambda: [e for e in bench.events(seq) if e["type"] == "leak_event" and e["state"] == "SUSPECT"],
                          15, "SUSPECT event")[0]
    eid = leak["event_id"]
    assert eid in bench.cmd("LOG 5")["reply"]
    before = bench.status()["boot_count"]
    bench.cmd("REBOOT")
    bench.wait_for(lambda: bench.status()["boot_count"] == before + 1, 120, "node back after reboot")
    bench.wait_for(lambda: bench.device()["sensor"]["connected"], 60, "data socket after reboot")
    assert eid in bench.cmd("LOG 20")["reply"], "event line lost across the reboot"


@pytest.mark.req("FW-07", "FW-08", "FW-20", "FW-03")
def test_event_sound_clip_recorded_during_outage(bench):
    st = node_status(bench)
    if st.get("flash_mb", 16) < 32 or st.get("mic_ok") != 1:
        pytest.skip("needs a 32 MB board with a working microphone")
    bench.wait_for(lambda: node_status(bench)["event_state"] == "NORMAL", 90, "node back to NORMAL before the test")
    seq = bench.seq()
    bench.cmd("SIM OFFLINE 25")
    bench.cmd("SIM GAS 3")
    leak = bench.wait_for(lambda: [e for e in bench.events(seq) if e["type"] == "leak_event" and e["state"] == "SUSPECT"],
                          15, "SUSPECT event during the outage")[0]
    eid = leak["event_id"]
    bench.wait_for(lambda: node_status(bench).get("fs_aud_pend", 0) >= 1, 20, "sound clip kept on the node")
    clip = bench.wait_for(lambda: [e for e in bench.events(seq) if e["type"] == "event_audio" and e["event_id"] == eid],
                          25 + 90, "sound clip delivered after the outage")[0]
    assert clip["bytes"] == 44 + 5 * 16000, "clip is not 5 s of 16 kHz A-law"
    wav = bench.get(clip["url"].replace("/api/v1", "", 1)).content
    assert wav[:4] == b"RIFF" and wav[8:12] == b"WAVE" and wav[20] == 6
    bench.wait_for(lambda: node_status(bench).get("fs_aud_pend", 1) == 0, 60, "no sound clips left pending")
