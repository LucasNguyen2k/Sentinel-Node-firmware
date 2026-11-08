"""HW Interface on the Edge HAL interface, checked by the node-gateway emulator (schemas, files, completeness)."""
import json
import os
import re
import time

import pytest

from conftest import DEVICE_ID, NODE_IP

ISO = lambda s: __import__("datetime").datetime.fromisoformat(s.replace("Z", "+00:00")).timestamp()


def sim(bench, text):
    r = bench.srv.post("/devices/%s/command" % NODE_IP, json={"command": text, "timeout": 10})
    assert r.status_code == 200 and str(r.json().get("reply", "")).startswith("OK"), r.text


# ---------------------------------------------------------------------- identity and health
def test_birth_retained_with_identity(bench):
    gw = bench.gw
    b = gw.wait_for(lambda: gw.by_topic("birth", DEVICE_ID), 20, "birth")[-1]
    assert b["board_name"] == "EZ_SN_044" and b["serial_number"] == "SN-00E04C0F5C9D" and b["ip"] == NODE_IP
    assert b["mac_address"] == "00:E0:4C:0F:5C:9D" and b["firmware_version"] == "1.6.1"
    assert b["capabilities"] == {"chemical": True, "acoustic_1": True, "acoustic_2_ultrasonic": False, "image": True}
    assert b["chemical_adc"]["bits"] == 10 and b["chemical_adc"]["resolution_mv"] == pytest.approx(3.226, abs=0.001)
    assert not {"pass", "token", "ssid", "server"} & set(b["config"])          # never credentials or network
    # a consumer that starts later still gets it at once (retained)
    from node_gateway_emu import NodeGatewayEmu
    late = NodeGatewayEmu("127.0.0.1", bench.port, bench.gw.data_root)
    try:
        late.wait_for(lambda: late.retained.get("sentinel/v1/%s/birth" % DEVICE_ID), 5, "retained birth")
    finally:
        late.close()


def test_heartbeat(bench):
    gw = bench.gw
    hbs = gw.wait_for(lambda: len(gw.by_topic("heartbeat", DEVICE_ID)) >= 2 and gw.by_topic("heartbeat", DEVICE_ID), 15, "heartbeats")
    h = hbs[-1]
    assert h["link"]["online"] is True and h["boot_id"].startswith("b")
    assert abs(ISO(h["last_seen"]) - time.time()) < 10


def test_chem01_baseline(bench):
    gw = bench.gw
    msgs = gw.wait_for(lambda: len(gw.by_topic("data/CHEM-01", DEVICE_ID)) >= 2 and gw.by_topic("data/CHEM-01", DEVICE_ID), 15, "CHEM-01")
    m = msgs[-1]
    t_ms, temp, rh, mos = m["sensor_values"].split(",")
    assert int(t_ms) == m["fields"]["time_ms"] and abs(int(t_ms) / 1000 - time.time()) < 10
    assert float(temp) == pytest.approx(24.5) and float(rh) == pytest.approx(60.1)
    assert m["fields"]["mos_mv"] == pytest.approx(161 / 1023 * 3300, abs=0.1)      # emulator gas reading 161
    q = m["quality"]
    assert set(q) == {"warmed_up", "calibrated", "sensor_ok", "temp_rh_ok", "time_synced", "maintenance", "simulated",
                      "mos_resolution_mv"}
    assert q["sensor_ok"] and q["temp_rh_ok"] and q["time_synced"] and not q["simulated"]
    assert "gas_ppm" not in m                         # emulator is not calibrated: no concentration
    gap = ISO(msgs[-1]["timestamp"]) - ISO(msgs[-2]["timestamp"])
    assert 1.0 <= gap <= 4.0                          # every chem01_s (2 s in this bench)


def test_baseline_audio_and_picture(bench):
    gw = bench.gw
    a = gw.wait_for(lambda: gw.by_topic("file/ACO-01", DEVICE_ID), 25, "ACO-01")[-1]
    assert ISO(a["recorded_to"]) - ISO(a["recorded_from"]) == pytest.approx(1.0, abs=0.01)
    assert "event_id" not in a and "part" not in a and a["time_accuracy_ms"] == 1000
    assert re.match(r"^SN-00E04C0F5C9D/ACO-01/\d{8}/\d{8}_\d{6}_\d{6}\.wav$", a["path"])
    i = gw.wait_for(lambda: gw.by_topic("file/IMG-01", DEVICE_ID), 15, "IMG-01")[-1]
    assert (i["width"], i["height"]) == (1280, 720)


# ---------------------------------------------------------------------- a node-triggered event
def test_event_delivers_complete_evidence(bench):
    gw = bench.gw
    before = {b["event_id"] for b in gw.by_topic("event/CHEM-03")}
    sim(bench, "SIM GAS 5")
    start = gw.wait_for(lambda: [b for b in gw.by_topic("event/CHEM-03", DEVICE_ID)
                                 if b["notification"] == "start" and b["event_id"] not in before], 15, "CHEM-03 start")[-1]
    eid = start["event_id"]
    assert re.match(r"^E\d+_\d+$", eid) and start["triggered_by"]["chemical"]     # the node's own event id
    end = gw.wait_for(lambda: [b for b in gw.by_topic("event/CHEM-03") if b["notification"] == "end" and b["event_id"] == eid], 40, "CHEM-03 end")[-1]
    assert not end.get("late") and end["source"] == "live", end                  # a live event ends live, not via recovery
    for notif in ("CHEM-03", "ACO-03", "IMG-03"):
        res = gw.wait_for(lambda: gw.evidence_complete(eid, notif), 40, notif + " end")
        assert res["complete"], (notif, res)
        assert res["expected"] >= (2 if notif == "IMG-03" else 1), (notif, res)   # at least 2 pictures per event
    parts = sorted(b["part"] for b in gw.by_topic("file/ACO-02") if b["event_id"] == eid)
    assert parts == list(range(1, len(parts) + 1))
    for b in (b for b in gw.by_topic("file/CHEM-02") if b["event_id"] == eid):
        assert b["rows"] > 0 and ISO(b["recorded_to"]) > ISO(b["recorded_from"])


def test_short_event_still_gets_two_pictures(bench):
    """An event shorter than the picture interval: only the minimum-pictures rule can supply the second one."""
    gw = bench.gw
    bench.hwi.s.event_img_every_s = 600
    try:
        before = {b["event_id"] for b in gw.by_topic("event/CHEM-03")}
        sim(bench, "SIM GAS 1")
        eid = gw.wait_for(lambda: [b["event_id"] for b in gw.by_topic("event/CHEM-03", DEVICE_ID)
                                   if b["notification"] == "start" and b["event_id"] not in before], 15, "start")[-1]
        res = gw.wait_for(lambda: gw.evidence_complete(eid, "IMG-03"), 40, "IMG-03 end")
        assert res["complete"] and res["expected"] == 2, res
    finally:
        bench.hwi.s.event_img_every_s = 3


# ---------------------------------------------------------------------- commands
def states(results):
    return [r["state"] for r in results]


def test_capture_image_command(bench):
    cid, res = bench.gw.command(DEVICE_ID, "capture_image", count=2, timeout=30)
    assert states(res)[0] == "accepted" and states(res)[-1] == "done" and len(res[-1]["files"]) == 2
    imgs = [b for b in bench.gw.by_topic("file/IMG-02") if b["command_id"] == cid]
    assert len(imgs) == 2 and all(b["trigger"] == "command" for b in imgs)


def test_record_audio_command(bench):
    gw = bench.gw
    cid, res = gw.command(DEVICE_ID, "record_audio", duration_s=4, timeout=40)
    assert states(res) == ["accepted", "done"] and len(res[-1]["files"]) == 2       # 2 parts of 2 s
    parts = [b for b in gw.by_topic("file/ACO-02") if b["command_id"] == cid]
    assert sorted(p["part"] for p in parts) == [1, 2] and all(p["trigger"] == "command" for p in parts)
    end = gw.wait_for(lambda: gw.events.get((DEVICE_ID, "cmd:" + cid, "ACO-03"), {}).get("end"), 10, "ACO-03 end")
    assert end["parts_total"] == 2 and end["command_id"] == cid


def test_stop_recording_command(bench):
    gw = bench.gw
    import json as _j
    import uuid
    rec_id = "c-" + uuid.uuid4().hex[:8]
    gw.mq.publish("sentinel/v1/%s/cmd/record_audio" % DEVICE_ID, _j.dumps({"command_id": rec_id, "duration_s": 60}), qos=1)
    gw.wait_for(lambda: "accepted" in states(gw.results.get(rec_id, [])), 10, "recording accepted")
    time.sleep(3)
    _, res = gw.command(DEVICE_ID, "stop_recording", target_command_id=rec_id, timeout=20)
    assert states(res)[-1] == "done"
    final = gw.wait_for(lambda: [r for r in gw.results.get(rec_id, []) if r["state"] == "done"], 30, "recording done")[-1]
    assert 1 <= len(final["files"]) <= 3                                           # stopped long before 60 s


def test_config_set_command(bench):
    gw = bench.gw
    n_birth = len(gw.by_topic("birth", DEVICE_ID))
    _, res = gw.command(DEVICE_ID, "config_set", key="sample_ms", value=100, timeout=20)
    assert states(res) == ["accepted", "done"]
    b = gw.wait_for(lambda: len(gw.by_topic("birth", DEVICE_ID)) > n_birth and gw.by_topic("birth", DEVICE_ID), 10, "new birth")[-1]
    assert b["config"].get("sample_ms") == 100                                     # birth follows the change
    _, res = gw.command(DEVICE_ID, "config_set", key="sample_ms", value=50, timeout=20)
    assert states(res)[-1] == "done"
    # allowed by the HW Interface, but the node does not know it: the node's answer is passed on
    _, res = gw.command(DEVICE_ID, "config_set", key="ac_th_db", value=10, timeout=20)
    assert states(res)[-1] == "refused" and "unknown key" in res[-1]["detail"]
    # never allowed: network and credentials
    _, res = gw.command(DEVICE_ID, "config_set", key="ssid", value="evil", timeout=20)
    assert states(res)[-1] == "refused" and "not allowed" in res[-1]["detail"]


def test_unknown_and_repeated_commands(bench):
    gw = bench.gw
    _, res = gw.command(DEVICE_ID, "self_destruct", timeout=10)
    assert states(res) == ["accepted", "refused"]
    cid, _ = gw.command(DEVICE_ID, "capture_image", timeout=30)
    gw.mq.publish("sentinel/v1/%s/cmd/capture_image" % DEVICE_ID, json.dumps({"command_id": cid}), qos=1)
    time.sleep(2)
    assert states(gw.results[cid]).count("accepted") == 1                         # a repeat is ignored


# ---------------------------------------------------------------------- 16 MB board, server outages
def test_birth_reports_storage_of_a_16mb_board(bench):
    gw = bench.gw
    b = gw.wait_for(lambda: [x for x in gw.by_topic("birth", DEVICE_ID) if x.get("storage")], 15, "birth with storage")[-1]
    assert b["storage"] == {"flash_mb": 16, "filesystem": False, "offline_records_minutes": 40, "event_log_entries": 350,
                            "keeps_event_pictures": False, "keeps_sound_clips": False}


def new_chem03(gw, before, notification, late=None):
    return [b for b in gw.by_topic("event/CHEM-03", DEVICE_ID) if b["event_id"] not in before
            and b["notification"] == notification and (late is None or bool(b.get("late")) is late)]


def test_event_during_outage_recovered_from_event_log(bench):
    """The node's event message is lost while the driver is down; the HW Interface recovers the event from the
    node's event log once it is back, and builds CHEM-02 from the replayed records."""
    gw = bench.gw
    before = {b["event_id"] for b in gw.by_topic("event/CHEM-03")}
    boot_before = gw.wait_for(lambda: gw.by_topic("birth", DEVICE_ID), 20, "first birth")[-1]["boot_id"]
    gw.wait_for(lambda: gw.by_topic("data/CHEM-01", DEVICE_ID), 20, "steady state")
    n_birth = len(gw.by_topic("birth", DEVICE_ID))
    bench.srv.stop()
    time.sleep(2)
    assert bench.node.execute("SIM GAS 3").startswith("OK")
    t_sim = time.time()
    time.sleep(9)                                          # SUSPECT, then NORMAL after the hold, all while offline
    assert [l for l in bench.node.event_log if l.split(",")[2] == "NORMAL" and int(l.split(",")[0]) >= t_sim - 2]
    bench.srv.start()
    end = gw.wait_for(lambda: new_chem03(gw, before, "end", late=True), 90, "recovered CHEM-03 end")[-1]
    births = gw.by_topic("birth", DEVICE_ID)[n_birth:]          # B13: birth on every connect, not only on change
    assert births and births[-1]["boot_id"] == boot_before
    eid = end["event_id"]
    assert end["source"] == "node_event_log"
    start = [b for b in gw.by_topic("event/CHEM-03") if b["event_id"] == eid and b["notification"] == "start"][-1]
    assert start["late"] is True and start["state"] == "SUSPECT"
    assert t_sim - 3 <= ISO(start["timestamp"]) <= ISO(end["timestamp"]) <= t_sim + 12      # the node's own times
    files = [b for b in gw.by_topic("file/CHEM-02") if b["event_id"] == eid]
    assert files and all(b.get("late") is True and b["rows"] > 0 for b in files)
    res = gw.evidence_complete(eid, "CHEM-03")
    assert res["complete"] and res["expected"] == end["parts_total"] >= 1, res


def test_live_event_ending_during_outage_is_closed(bench):
    gw = bench.gw
    before = {b["event_id"] for b in gw.by_topic("event/CHEM-03")}
    sim(bench, "SIM GAS 4")
    eid = gw.wait_for(lambda: new_chem03(gw, before, "start", late=False), 15, "live start")[-1]["event_id"]
    bench.srv.stop()
    time.sleep(10)                                         # the event ends while the driver is down
    bench.srv.start()
    end = gw.wait_for(lambda: [b for b in gw.by_topic("event/CHEM-03") if b["event_id"] == eid and b["notification"] == "end"],
                      90, "end of the live event")[-1]
    assert end.get("late") is True and end["source"] == "node_event_log"
    assert gw.evidence_complete(eid, "CHEM-03")["complete"]
    img = gw.wait_for(lambda: gw.evidence_complete(eid, "IMG-03"), 60, "IMG-03 end")
    assert img["complete"], img
    assert len([b for b in gw.by_topic("event/CHEM-03") if b["event_id"] == eid and b["notification"] == "end"]) == 1


def test_recovery_does_not_preempt_a_live_event_end(bench):
    """Race: while recovery is active after a connect, the node's log already shows NORMAL before the live
    event message has been processed. The live event must still end live, not be closed as 'late'."""
    gw, hw = bench.gw, bench.hwi
    gw.wait_for(lambda: gw.by_topic("data/CHEM-01", DEVICE_ID), 20, "steady state")
    old = hw.s.poll_events_s
    hw.s.poll_events_s = 3.0                               # live event messages arrive slowly ...
    hw.schedule_recovery(hw.devices[NODE_IP])              # ... while recovery reads the log every 1.5 s
    try:
        before = {b["event_id"] for b in gw.by_topic("event/CHEM-03")}
        sim(bench, "SIM GAS 1")
        end = gw.wait_for(lambda: [b for b in gw.by_topic("event/CHEM-03", DEVICE_ID)
                                   if b["notification"] == "end" and b["event_id"] not in before], 40, "end")[-1]
        assert not end.get("late") and end["source"] == "live", end
        eid = end["event_id"]
        starts = [b for b in gw.by_topic("event/CHEM-03") if b["event_id"] == eid and b["notification"] == "start"]
        assert len(starts) == 1, starts                    # never published twice (live and recovered)
    finally:
        hw.s.poll_events_s = old


def _event_with_gaps(bench, gap_s, max_gap_s):
    gw, hw = bench.gw, bench.hwi
    gw.wait_for(lambda: gw.by_topic("data/CHEM-01", DEVICE_ID), 20, "steady state")
    old = hw.s.audio_max_gap_s
    bench.feeder.gap_s, hw.s.audio_max_gap_s = gap_s, max_gap_s
    try:
        before = {b["event_id"] for b in gw.by_topic("event/CHEM-03")}
        time.sleep(3)
        sim(bench, "SIM GAS 4")
        eid = gw.wait_for(lambda: [b["event_id"] for b in gw.by_topic("event/CHEM-03", DEVICE_ID)
                                   if b["notification"] == "start" and b["event_id"] not in before], 15, "start")[-1]
        res = gw.wait_for(lambda: gw.evidence_complete(eid, "ACO-03"), 60, "ACO-03 end")
        parts = [b for b in gw.by_topic("file/ACO-02", DEVICE_ID) if b["event_id"] == eid]
        return res, parts
    finally:
        bench.feeder.gap_s, hw.s.audio_max_gap_s = 0, old


def test_short_audio_gaps_are_bridged(bench):
    """The recorder can leave short gaps (a restart): parts are still complete, the silence is reported."""
    res, parts = _event_with_gaps(bench, gap_s=0.5, max_gap_s=3.0)
    assert res["complete"], res
    assert any(p.get("gaps_ms", 0) >= 400 for p in parts), [p.get("gaps_ms") for p in parts]
    paths = {p["path"] for p in parts}
    assert not [e for e in bench.gw.file_errors if e[0] in paths], bench.gw.file_errors   # each WAV = its window


def test_lost_audio_parts_show_as_incomplete(bench):
    """A part that cannot be cut keeps its number: the evidence shows incomplete instead of looking complete."""
    res, parts = _event_with_gaps(bench, gap_s=0.5, max_gap_s=0.05)
    assert not res["complete"] and res["received"] < res["expected"], res
    nums = sorted(p["part"] for p in parts)
    assert nums != list(range(1, len(nums) + 1)) or len(nums) < res["expected"], nums


# ---------------------------------------------------------------------- interface integrity
def test_interface_integrity(bench):
    gw = bench.gw
    time.sleep(1)
    assert gw.schema_errors == [], gw.schema_errors[:5]
    assert gw.file_errors == [], gw.file_errors[:5]
    assert gw.seq_errors == [], gw.seq_errors[:5]
    assert gw.reconcile() == []
    # a file being written right now is fine; one that stays .part is not
    gw.wait_for(lambda: not [n for d, _, ns in os.walk(gw.data_root) for n in ns if n.endswith(".part")], 5,
                "no .part files")


def test_seq_continues_after_restart(bench):
    gw = bench.gw
    last = max(b["seq"] for b in gw.by_topic("heartbeat", DEVICE_ID))
    bench.hwi.close()
    from hw_interface.service import HwInterface
    bench.hwi = HwInterface(bench.settings()).start()
    n_before = len(gw.messages)
    old_events = {b["event_id"] for b in gw.by_topic("event/CHEM-03", DEVICE_ID)}
    gw.wait_for(lambda: any(b["seq"] > last for b in gw.by_topic("heartbeat", DEVICE_ID)), 15, "heartbeat after restart")
    time.sleep(4)
    assert gw.seq_errors == []
    # nothing handled before the restart is published again (the driver still holds those events)
    replayed = [(t, b.get("event_id")) for t, b in gw.messages[n_before:]
                if ("/event/" in t or "/file/IMG-02" in t) and b.get("event_id") in old_events]
    assert replayed == [], replayed


def test_offline_node_command_expires(bench):
    bench.node.stop()
    time.sleep(6)                                     # driver marks it offline, HW Interface refreshes devices
    _, res = bench.gw.command(DEVICE_ID, "capture_image", timeout=20)
    assert states(res)[-1] == "expired" and res[-1]["detail"] == "node offline"
    off = [b for b in bench.gw.by_topic("heartbeat", DEVICE_ID) if not b["link"]["online"]]
    assert off, "an offline heartbeat is published"


# ---------------------------------------------------------------------- units
def test_adc_configurations():
    from hw_interface.config import Settings
    s = Settings(adc_bits=10, adc_vref_v=3.3)
    assert s.counts_to_mv(1023) == 3300.0 and s.mos_resolution_mv == pytest.approx(3.226, abs=0.001)
    s = Settings(adc_bits=12, adc_vref_v=5.0)
    assert s.counts_to_mv(4095) == 5000.0 and s.mos_resolution_mv == pytest.approx(1.221, abs=0.001)
    s = Settings(adc_bits=16, adc_vref_v=5.0)
    assert s.counts_to_mv(65535) == 5000.0 and s.mos_resolution_mv == pytest.approx(0.076, abs=0.001)
    s = Settings(adc_bits=10, adc_vref_v=3.3, adc_divider=1.515)
    assert s.counts_to_mv(1023) == pytest.approx(4999.5, abs=0.1)
    assert s.counts_to_mv(None) is None and s.counts_to_mv(-1) is None


def test_synthetic_milliseconds(tmp_path):
    from hw_interface.config import Settings
    from hw_interface.service import Device, HwInterface
    hw = HwInterface(Settings(data_root=str(tmp_path / "a"), state_db=str(tmp_path / "s.db")))
    dev = Device("x")
    got = []
    for ts in (100, 100, 100, 101, 101):
        hw.on_record(dev, {"fields": {"device_ts": ts, "gas_raw": 161}})
        got.append(dev.last_record["time_ms"])
    assert got == [100000, 100050, 100100, 101000, 101050]
    hw.on_record(dev, {"ts": 5.5, "fields": {"device_ts": None, "gas_raw": 161}})
    assert dev.last_record["time_ms"] == 5500 and dev.last_record["synced"] is False


def test_retention_and_atomic_write(tmp_path):
    from hw_interface.store import DataArea, Ledger
    led = Ledger(str(tmp_path / "s.db"))
    area = DataArea(str(tmp_path / "area"), led)
    rel = area.rel_path("SN-00E04C0F5C9D", "IMG-01", 1792678537.2, "jpg")
    assert rel == "SN-00E04C0F5C9D/IMG-01/20261022/20261022_141537_200000.jpg"
    size, sha = area.write(rel, b"\xff\xd8data", "SN-00E04C0F5C9D", "IMG-01")
    assert size == 6 and len(sha) == 64 and not os.path.exists(os.path.join(area.root, rel + ".part"))
    assert area.cleanup(3600) == 0 and os.path.exists(os.path.join(area.root, rel))
    with led.lock, led.db:
        led.db.execute("UPDATE files SET created = created - 7200")
    assert area.cleanup(3600) == 1 and not os.path.exists(os.path.join(area.root, rel))
    assert not os.path.exists(os.path.join(area.root, "SN-00E04C0F5C9D"))        # empty folders removed
    assert led.next_seq("SN-1", "X") == 1 and led.next_seq("SN-1", "X") == 2
    assert Ledger(str(tmp_path / "s.db")).next_seq("SN-1", "X") == 3              # kept across restarts


def test_parse_event_log():
    from hw_interface.service import parse_event_log
    text = ("1791925230,E7_0,SUSPECT,20.3,174,41.1,010,0,95\n"      # firmware line
            "1791925262,E7_0,NORMAL\n"                              # emulator line
            "flash data store not available\n,,\n1791925300,M7_1,NORMAL")
    assert parse_event_log(text) == [(1791925230, "E7_0", "SUSPECT"), (1791925262, "E7_0", "NORMAL"),
                                     (1791925300, "M7_1", "NORMAL")]
    assert parse_event_log(None) == []


def test_first_seen_is_kept(tmp_path):
    """Event-log recovery treats only events that ended before this moment as history."""
    from hw_interface.store import Ledger
    led = Ledger(str(tmp_path / "s.db"))
    t1 = led.first_seen("SN-00E04C0F5C9D")
    time.sleep(0.05)
    assert led.first_seen("SN-00E04C0F5C9D") == t1
    assert Ledger(str(tmp_path / "s.db")).first_seen("SN-00E04C0F5C9D") == t1          # kept across restarts


def test_synthetic_milliseconds_stay_unique_with_extra_records(tmp_path):
    """The real node sometimes sends 21-22 records in one second: times must stay unique and increasing."""
    from hw_interface.config import Settings
    from hw_interface.service import Device, HwInterface
    hw = HwInterface(Settings(data_root=str(tmp_path / "a"), state_db=str(tmp_path / "s.db")))
    dev = Device("x")
    got = []
    for ts in [200] * 22 + [201, 201]:
        hw.on_record(dev, {"fields": {"device_ts": ts, "gas_raw": 161}})
        got.append(dev.last_record["time_ms"])
    assert len(set(got)) == len(got) and got == sorted(got), got
    assert got[19:22] == [200950, 200951, 200952] and got[22] == 201000


def test_audio_cut_bridges_short_gaps(tmp_path):
    import math
    import struct
    import wave as wv
    from datetime import datetime
    from hw_interface.media import AudioIndex

    def seg(start, secs):
        name = datetime.fromtimestamp(start).strftime("%Y%m%d_%H%M%S_%f") + ".wav"
        with wv.open(str(tmp_path / name), "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000)
            w.writeframes(b"".join(struct.pack("<h", int(1000 * math.sin(i / 10))) for i in range(int(16000 * secs))))
    t = 1793000000.0
    seg(t, 2.0)
    seg(t + 3.5, 2.0)                                      # 1.5 s gap
    seg(t + 10.5, 2.0)                                     # 5 s gap
    idx = AudioIndex(str(tmp_path))
    assert not idx.covers(t + 1, t + 5)                    # default: no gap allowed
    assert idx.covers(t + 1, t + 5, max_gap=3.0)
    data, gaps_ms = idx.cut(t + 1, t + 5)
    import io
    with wv.open(io.BytesIO(data)) as w:
        assert w.getnframes() == 4 * 16000                 # exactly the window
    assert gaps_ms == 1500
    assert not idx.covers(t + 4, t + 12, max_gap=3.0)      # a 5 s gap is not bridged
