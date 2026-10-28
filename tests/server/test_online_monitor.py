"""L3 integration: connectivity monitoring and recovery (node_online / node_offline, reconnect)."""
import time

import pytest


@pytest.mark.req("FW-13", "FW-20")
def test_offline_and_back_online(edge, make_node):
    n = make_node(90).start(threads=("reader", "uploader"))
    edge.wait_for(lambda: any(e["type"] == "node_online" for e in edge.get("/events?device_id=127.0.0.90").json()["events"]),
                  10, what="node_online")
    n.send_records = False                                 # node stops sending (e.g. Wi-Fi lost)
    edge.wait_for(lambda: any(e["type"] == "node_offline" for e in edge.get("/events?device_id=127.0.0.90").json()["events"]),
                  15, what="node_offline")
    n.send_records = True
    edge.wait_for(lambda: [e["type"] for e in edge.get("/events?device_id=127.0.0.90").json()["events"]][-1] == "node_online",
                  15, what="node_online again")


@pytest.mark.req("FW-20", "FW-10")
def test_reconnect_replaces_old_connection(edge, make_node):
    n = make_node(91).start(threads=("reader", "uploader"))
    edge.wait_for(lambda: edge.get("/devices/127.0.0.91").json()["sensor"]["connected"], what="connected")
    n._close_socket()
    n.connect_socket()
    time.sleep(1.5)
    assert edge.get("/devices/127.0.0.91").json()["sensor"]["connected"] is True
    assert "Traceback" not in edge.log()
