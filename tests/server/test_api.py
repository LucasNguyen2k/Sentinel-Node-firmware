"""L3 integration: REST API for agents (auth, devices, status, events, streams)."""
import json
import time

import pytest
import requests

pytestmark = pytest.mark.req("FW-10")


@pytest.mark.req("FW-16")
def test_api_key_required(edge):
    assert requests.get(edge.api + "/devices", timeout=5).status_code == 401
    assert requests.get(edge.api + "/devices", headers={"X-API-Key": "wrong"}, timeout=5).status_code == 401


@pytest.mark.req("FW-06", "FW-07", "FW-13")
def test_full_event_flow_through_api(edge, make_node):
    make_node(80, event_hold_s=2).start()
    edge.wait_for(lambda: edge.get("/devices/127.0.0.80/status").status_code == 200, what="first heartbeat")
    seq0 = edge.get("/events").json()["next_after_seq"]
    assert edge.post("/devices/127.0.0.80/command", json={"command": "SIM GAS 2"}).json()["ok"]
    evs = edge.wait_for(lambda: (lambda e: e if any(x["type"] == "event_image" for x in e) else None)(
        edge.get("/events?after_seq=%d&device_id=127.0.0.80" % seq0).json()["events"]), 25, what="event image")
    leak = [e for e in evs if e["type"] == "leak_event"][0]
    assert leak["state"] == "SUSPECT" and leak["sensors"]["gas"] is True
    img = [e for e in evs if e["type"] == "event_image"][0]
    assert img["event_id"] == leak["event_id"]
    edge.wait_for(lambda: any(e.get("state") == "NORMAL" for e in edge.get("/events?after_seq=%d" % seq0).json()["events"]),
                  30, what="return to NORMAL")
    info = edge.get("/devices/127.0.0.80").json()
    assert info["event"]["state"] == "NORMAL" and info["event"]["event_images"] >= 1


@pytest.mark.req("FW-13")
def test_status_staleness(edge, make_node):
    n = make_node(81, hb_s=60)
    n.register()
    n.heartbeat()
    assert edge.get("/devices/127.0.0.81/status").json()["stale"] is False
    time.sleep(5)                                          # AMB82_STATUS_STALE=4 in tests
    assert edge.get("/devices/127.0.0.81/status").json()["stale"] is True


def test_event_paging_and_filters(edge, make_node):
    n = make_node(82)
    n.register()
    for i in range(5):
        n.http("GET", "/node-event?name=EZ_EMU&event=E1_%d&state=SUSPECT&prev=NORMAL&g=1&a=0&u=0" % i)
    allev = edge.get("/events?device_id=127.0.0.82&type=leak_event").json()
    assert allev["count"] == 5
    first = allev["events"][1]["seq"]
    rest = edge.get("/events?device_id=127.0.0.82&after_seq=%d" % first).json()["events"]
    assert [e["event_id"] for e in rest if e["type"] == "leak_event"] == ["E1_2", "E1_3", "E1_4"]


def test_sse_event_stream(edge, make_node):
    n = make_node(83)
    n.register()
    got = []
    # resume from a known sequence number (documented client usage): an event published between the
    # request and the start of the stream is then still delivered
    seq = edge.get("/events").json()["next_after_seq"]
    with requests.get(edge.api + "/events/stream?device_id=127.0.0.83&after_seq=%d" % seq, headers=edge.h,
                      stream=True, timeout=10) as r:
        assert r.headers["content-type"].startswith("text/event-stream")
        n.http("GET", "/node-event?name=EZ_EMU&event=E9_1&state=SUSPECT&prev=NORMAL&g=1&a=0&u=0")
        t0 = time.time()
        for line in r.iter_lines(decode_unicode=True):
            if line and line.startswith("data: "):
                got.append(json.loads(line[6:]))
                break
            if time.time() - t0 > 8:
                break
    assert got and got[0]["event_id"] == "E9_1"


@pytest.mark.req("FW-17", "FW-19")
def test_command_catalogue(edge):
    cmds = edge.get("/commands").json()
    for k in ("PING", "STATUS", "SET <key> <value>", "OTA_SD [FORCE]", "SIM <GAS|AC|CLEAR> [s]", "REBOOT"):
        assert k in cmds


@pytest.mark.req("FW-03")
def test_sensor_csv_and_latest(edge, make_node):
    make_node(84).start(threads=("reader", "uploader"))
    edge.wait_for(lambda: edge.get("/devices/127.0.0.84/sensor/latest").status_code == 200, what="latest record")
    csv = edge.get("/devices/127.0.0.84/sensor.csv").text
    assert csv.count("\n") >= 1 and "161" in csv
