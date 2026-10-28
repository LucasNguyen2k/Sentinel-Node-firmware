"""L4: system tests of a running Sentinel Node through the Edge Server (hardware or emulator)."""
import time

import pytest

pytestmark = pytest.mark.hil


@pytest.mark.req("FW-01", "FW-10", "FW-16")
def test_node_registered_with_expected_firmware(bench):
    d = bench.device()
    assert d["node"]["node_id"].startswith("00E04C"), "MAC must carry the Realtek OUI"
    if bench.fw:
        assert d["node"]["firmware"] == bench.fw
    assert d["node"]["commands_supported"] is True
    assert d["sensor"]["online"] is True


@pytest.mark.req("FW-13", "FW-12", "FW-18")
def test_health_report_complete_and_healthy(bench):
    s = bench.status()
    for k in ("uptime_s", "boot_count", "last_reset", "heap", "heap_min", "rssi", "sock_connected", "int_temp_raw",
              "aht_ok", "gas_ok", "mic_ok", "cam_ok", "sd_ok", "selftest_fail_mask", "ota", "ota_capable", "valid"):
        assert k in s, "heartbeat field %s missing" % k
    assert s["sock_connected"] == 1 and s["registered"] == 1
    assert s["aht_ok"] == 1 and s["mic_ok"] == 1 and s["cam_ok"] == 1
    assert s["int_temp_raw"] is not None and s["int_temp_raw"] > 0          # FW-12: internal temperature source
    assert (s["selftest_fail_mask"] & ~bench.allowed_mask) == 0, "self-test failures: mask %d" % s["selftest_fail_mask"]
    assert s["heap_min"] > 1000000


@pytest.mark.req("FW-15")
def test_node_clock_synchronised(bench):
    st = bench.cmd("STATUS")["json"]
    assert st["time"] > 1600000000, "node clock not set"
    assert abs(st["time"] - time.time()) <= 3


@pytest.mark.req("FW-19", "FW-16", "FW-21")
def test_service_commands(bench):
    assert bench.cmd("PING")["reply"].startswith("PONG ")
    cfg = bench.cmd("CONFIG")["reply"]
    assert "pass=****" in cfg and "token=****" in cfg
    assert "OTA_SD [FORCE]" in bench.cmd("HELP")["reply"]
    assert bench.cmd("FLY")["ok"] is False


@pytest.mark.req("FW-22", "FW-13")
def test_self_test_passes(bench):
    r = bench.cmd("SELFTEST", timeout=40)["reply"]
    assert "RESULT:" in r
    if bench.allowed_mask == 0:
        assert "RESULT:PASS" in r, r


@pytest.mark.req("FW-05", "FW-06", "FW-07", "FW-22", "FW-23")
def test_simulated_gas_leak_end_to_end(bench):
    seq = bench.seq()
    assert bench.cmd("SIM GAS 5")["ok"]
    leak = bench.wait_for(lambda: [e for e in bench.events(seq) if e["type"] == "leak_event" and e["state"] == "SUSPECT"],
                          15, "SUSPECT event")[0]
    assert leak["sensors"]["gas"] is True                      # 1.1.9 regression
    img = bench.wait_for(lambda: [e for e in bench.events(seq) if e["type"] == "event_image"], 20, "event picture")[0]
    assert img["event_id"] == leak["event_id"] and img["bytes"] > 1000
    bench.wait_for(lambda: [e for e in bench.events(seq) if e.get("state") == "NORMAL"], 5 + bench.hold_s + 20,
                   "return to NORMAL")


@pytest.mark.req("FW-17")
def test_ota_guard_reports_build_capability(bench):
    # precondition on hardware: no ota.bin on the node's SD card
    r = bench.cmd("OTA_SD")["reply"]
    if bench.status()["ota_capable"] == 1:
        assert r == "ERR ota.bin not found on SD"
    else:
        assert "built without OTA Mode" in r


@pytest.mark.soak
@pytest.mark.req("FW-09", "FW-10", "FW-20", "FW-03")
def test_data_socket_stable_and_records_flowing(bench):
    s0, seq0 = bench.status(), bench.seq()
    rec0 = bench.get("/devices/%s/sensor/latest" % bench.ip).json()
    time.sleep(bench.soak_s)
    s1 = bench.status()
    rec1 = bench.get("/devices/%s/sensor/latest" % bench.ip).json()
    assert s1["sock_reconnects"] == s0["sock_reconnects"], "data socket reconnected during the soak window"
    assert not [e for e in bench.events(seq0) if e["type"] == "node_offline"]
    assert rec1["seq"] > rec0["seq"], "no new sensor records"
    assert s1["heap_min"] >= s0["heap_min"] - 200000, "heap minimum dropped (possible leak)"


@pytest.mark.destructive
@pytest.mark.req("FW-01", "FW-18", "FW-20")
def test_reboot_recovers_and_reports_reason(bench):
    before = bench.status()["boot_count"]
    bench.cmd("REBOOT")
    bench.wait_for(lambda: bench.status()["boot_count"] == before + 1, 120, "node back after reboot")
    s = bench.status()
    assert s["last_reset"].startswith("cmd_reboot")
    bench.wait_for(lambda: bench.device()["sensor"]["connected"], 60, "data socket after reboot")


@pytest.mark.destructive
@pytest.mark.req("FW-11", "FW-21")
def test_power_mode_round_trip(bench):
    assert bench.cmd("POWER 1")["reply"] == "OK power ECO"
    bench.wait_for(lambda: bench.status()["power_mode"] == "ECO", 90, "ECO in heartbeat")
    assert bench.cmd("POWER 0")["reply"] == "OK power ACTIVE"
    bench.wait_for(lambda: bench.status()["power_mode"] == "ACTIVE", 90, "ACTIVE in heartbeat")


@pytest.mark.serial
@pytest.mark.destructive
@pytest.mark.req("FW-01", "FW-18")
def test_boot_log_stages_in_order(bench):
    import serial  # pyserial
    stages = ["config", "wifi", "multimedia", "hal", "storage", "time", "self-test", "register", "threads", "running"]
    with serial.Serial(bench.serial_port, 115200, timeout=1) as port:
        port.reset_input_buffer()
        bench.cmd("REBOOT")
        seen, bad, t0 = [], [], time.time()
        while time.time() - t0 < 90 and "running" not in seen:
            line = port.readline().decode(errors="replace").strip()
            if "[BOOT]" in line:
                seen.append(line.split(":")[-1].strip())
            if "RTSP Err" in line or "accept error" in line:
                bad.append(line)
    assert seen == stages, "boot stages: %s" % seen
    assert not bad, "RTSP errors during boot: %s" % bad[:3]


@pytest.mark.req("FW-08", "FW-02", "FW-13")
def test_flash_data_store_healthy(bench):
    s = bench.status()
    assert s.get("storage") == "flash" and s.get("flash_ok") == 1
    assert s.get("flash_corrupt", 0) == 0
    r = bench.cmd("FLASH_TEST")["reply"]
    assert r.startswith("OK") and "nor_and=yes" in r and "guard=ok" in r, r
    stats = bench.cmd("FLASH")["reply"]
    assert "events:" in stats and "buffer:" in stats and "store_ok=1" in stats
