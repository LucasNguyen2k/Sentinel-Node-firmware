"""L3 integration: data socket records, timestamps, store-and-forward replay and the command channel."""
import time

import pytest

pytestmark = pytest.mark.req("FW-10")


def wait_records(edge, ip, n, timeout=8):
    return edge.wait_for(lambda: (lambda r: r if len(r) >= n else None)(
        edge.get("/devices/%s/sensor?limit=200" % ip).json()["records"]), timeout, what="%d records" % n)


@pytest.mark.req("FW-03", "FW-15")
def test_records_stored_with_fields_and_device_time(edge, make_node):
    make_node(60, hb_s=60).start(threads=("reader", "uploader"))
    recs = wait_records(edge, "127.0.0.60", 8)
    r = recs[-1]
    assert r["fields"]["temp_c"] == 24.5 and r["fields"]["gas_raw"] == 161
    assert r["ts_source"] == "device"                     # clock came from the registration reply
    assert abs(r["ts"] - time.time()) < 5
    seqs = [x["fields"]["device_seq"] for x in recs]
    assert seqs == sorted(seqs)


@pytest.mark.req("FW-15")
def test_server_pushes_time_on_connect(edge, make_node):
    n = make_node(61).start(threads=("reader",))
    cmds = edge.wait_for(lambda: [c for c in n.received_commands if c.startswith("TIME ")], what="TIME push")
    assert abs(int(cmds[0].split()[1]) - time.time()) <= 2


@pytest.mark.req("FW-08", "FW-15")
def test_replayed_records_keep_original_time(edge, make_node):
    n = make_node(62).start(threads=("reader",))
    old = int(time.time()) - 3600
    n.send("24.0,55.0,150,%d,1,-1.0,-40.0,-55.0,-1,0,83_24.0,55.0,150,0,2,-1.0,-40.0,-55.0,-1,0,30$" % old)
    recs = wait_records(edge, "127.0.0.62", 2)
    assert recs[0]["ts_source"] == "device" and int(recs[0]["ts"]) == old      # buffered an hour ago
    assert recs[1]["ts_source"] == "server"                                      # node clock not set


def test_command_replies_are_not_stored_as_records(edge, make_node):
    make_node(63).start(threads=("reader",))
    r = edge.post("/devices/127.0.0.63/command", json={"command": "STATUS"}).json()
    assert r["ok"] and r["json"]["fw"] == "1.3.1" and r["via"] == "socket"
    time.sleep(0.5)
    recs = edge.get("/devices/127.0.0.63/sensor?limit=50").json()["records"]
    assert all(not x["raw"].startswith("#") and "uptime_s" not in x["raw"] for x in recs)


@pytest.mark.req("FW-19", "FW-21")
def test_command_round_trips(edge, make_node):
    make_node(64).start(threads=("reader",))
    def c(x):
        return edge.post("/devices/127.0.0.64/command", json={"command": x}).json()
    assert c("PING")["reply"] == "PONG EZ_EMU 1.3.1"
    cfg = c("CONFIG")["reply"]
    assert "pass=****" in cfg and "secret" not in cfg
    assert c("SET sample_ms 100")["ok"] is True
    assert c("SET bogus 1")["ok"] is False
    assert "OTA_SD [FORCE]" in c("HELP")["reply"]


@pytest.mark.req("FW-10", "FW-20")
def test_lost_reply_does_not_shift_later_replies(edge, make_node):
    n = make_node(65).start(threads=("reader",))
    orig = n.execute
    n.execute = lambda line: (time.sleep(3) or orig(line)) if line.startswith("SLOW") else orig(line)
    r = edge.post("/devices/127.0.0.65/command", json={"command": "SLOW", "timeout": 1})
    assert r.status_code == 504
    assert edge.post("/devices/127.0.0.65/command", json={"command": "PING"}).json()["reply"].startswith("PONG")
    time.sleep(3)                                          # the late SLOW reply arrives now and is dropped
    assert edge.post("/devices/127.0.0.65/command", json={"command": "PING"}).json()["reply"].startswith("PONG")


@pytest.mark.req("FW-10")
def test_firmware_1_1_0_untagged_commands(edge, make_node):
    n = make_node(66, fw="1.1.0", tagged=False).start(threads=("reader",))
    edge.wait_for(lambda: edge.get("/devices/127.0.0.66").json()["sensor"]["connected"], 15, what="socket connected")
    r = edge.post("/devices/127.0.0.66/command", json={"command": "PING"}).json()
    assert r["reply"] == "PONG EZ_EMU 1.1.0"
    assert any(c == "PING" for c in n.received_commands)


def test_command_validation_and_availability(edge, make_node):
    assert edge.post("/devices/127.0.0.67/command", json={"command": "PING"}).status_code == 404
    n = make_node(67)
    n.register()                                           # registered, no socket yet
    assert edge.post("/devices/127.0.0.67/command", json={"command": "PING"}).status_code == 503
    n.connect_socket()
    n.start(threads=("reader",))
    assert edge.post("/devices/127.0.0.67/command", json={"command": "BAD$CMD"}).status_code == 422
    old = make_node(68, fw="1.0")
    old.register()
    assert edge.post("/devices/127.0.0.68/command", json={"command": "PING"}).status_code == 409


@pytest.mark.req("FW-17")
def test_ota_command_reports_build_capability(edge, make_node):
    make_node(69, ota_capable=False).start(threads=("reader",))
    r = edge.post("/devices/127.0.0.69/command", json={"command": "OTA_SD"}).json()
    assert r["ok"] is False and "built without OTA Mode" in r["reply"]
