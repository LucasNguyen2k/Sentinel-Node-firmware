"""L3 integration: node -> server HTTP protocol on port 8900 (registration, heartbeat, events, images)."""
import time
import urllib.parse

import pytest

pytestmark = pytest.mark.req("FW-10")


@pytest.mark.req("FW-15")
def test_registration_reply_carries_server_clock(edge, make_node):
    n = make_node(44)
    st, body = n.http("GET", "/add-device?ip=127.0.0.44&name=EZ_T&id=00E04C00002C&fw=1.3.1")
    assert st == 200 and body.startswith("DONE T=")
    assert abs(int(body.split("T=")[1]) - time.time()) <= 2
    dev = edge.get("/devices/127.0.0.44").json()
    assert dev["node"]["node_id"] == "00E04C00002C" and dev["node"]["firmware"] == "1.3.1"
    assert dev["node"]["commands_supported"] is True


@pytest.mark.req("FW-13", "FW-20")
def test_heartbeat_registers_unknown_node_after_server_restart(edge, make_node):
    n = make_node(45)                       # never called /add-device
    st, body = n.heartbeat()
    assert st == 200 and body.startswith("OK T=")
    status = edge.get("/devices/127.0.0.45/status").json()
    assert status["status"]["fw"] == "1.3.1" and status["status"]["ota_capable"] == 0
    assert status["stale"] is False


@pytest.mark.req("FW-06", "FW-07", "FW-23")
def test_event_message_and_event_image(edge, make_node):
    n = make_node(46)
    n.register()
    q = urllib.parse.urlencode({"name": "EZ_EMU", "id": n.mac, "event": "E3_1", "state": "SUSPECT", "prev": "NORMAL",
                                "gas_ppm": "1200.5", "gas_raw": 315, "ac_db": "2.1", "g": 1, "a": 0, "u": 0, "maint": 0,
                                "ts": int(time.time()), "valid": 95})
    assert n.http("GET", "/node-event?" + q)[0] == 200
    jpeg = b"\xff\xd8\xff\xe0" + b"IMG" * 500 + b"\xff\xd9"
    assert n.post_image("E3_1", jpeg)[0] == 200
    evs = edge.get("/devices/127.0.0.46/events").json()["events"]
    leak = [e for e in evs if e["type"] == "leak_event"][0]
    assert leak["state"] == "SUSPECT" and leak["sensors"]["gas"] is True and leak["gas_ppm"] == 1200.5
    imgs = edge.get("/devices/127.0.0.46/event-images").json()
    assert imgs[0]["event_id"] == "E3_1"
    got = edge.get("/devices/127.0.0.46/event-images/" + imgs[0]["file"]).content
    assert got == jpeg                                       # envelope removed, bytes intact


@pytest.mark.req("FW-07")
def test_image_upload_rejections(edge, make_node):
    n = make_node(47)
    n.register()
    assert n.http("POST", "/", body=b"no jpeg here", headers={"Content-Length": "12"})[0] == 400
    assert n.http("POST", "/", body=b"", headers={"Content-Length": "0"})[0] == 411
    big = str(9 * 1024 * 1024)
    assert n.http("POST", "/", body=b"x", headers={"Content-Length": big})[0] == 413


@pytest.mark.req("FW-08")
def test_http_upload_mode(edge, make_node):
    n = make_node(48)
    n.register()
    n._set_clock(int(time.time()))
    data = n.record() + "_" + n.record() + "$"
    assert n.http("GET", "/up-data?data=" + urllib.parse.quote(data))[0] == 200
    recs = edge.get("/devices/127.0.0.48/sensor?limit=10").json()["records"]
    assert len(recs) == 2 and recs[0]["fields"]["device_seq"] == 1


def test_unknown_path_and_unknown_device(edge, make_node):
    n = make_node(49)
    assert n.http("GET", "/nope")[0] == 404
    assert n.http("GET", "/node-event?state=SUSPECT")[0] == 404      # unknown node without name


@pytest.mark.req("FW-16")
def test_requests_from_outside_device_subnet_rejected(tmp_path):
    from edge_server import EdgeServer
    from node_emulator import NodeEmulator
    srv = EdgeServer(tmp_path, AMB82_DEVICE_SUBNET="10.0.0.0/8").start()
    try:
        n = NodeEmulator(ip="127.0.0.50", reg_port=srv.reg_port)
        assert n.register() == 403
        assert n.heartbeat()[0] == 403
        assert srv.get("/devices").json() == []
    finally:
        srv.stop()
