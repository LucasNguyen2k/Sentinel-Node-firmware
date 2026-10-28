"""L3 integration: node reboots - requested (REBOOT) and unexpected (power cut, watchdog)."""
import time

import pytest

pytestmark = pytest.mark.req("FW-10", "FW-20")


def connected(edge, ip):
    return edge.get("/devices/%s" % ip).json()["sensor"]["connected"]


@pytest.mark.req("FW-18")
def test_reboot_replies_and_commands_fail_fast_while_rebooting(edge, make_node):
    n = make_node(100, hb_s=60).start(threads=("reader",))
    edge.wait_for(lambda: connected(edge, "127.0.0.100"), 10, what="connected")
    seq = edge.get("/events").json()["next_after_seq"]
    t0 = time.time()
    r = edge.post("/devices/127.0.0.100/command", json={"command": "REBOOT"}).json()
    assert r["ok"] is True and r["reply"] == "OK rebooting in 1 s"
    assert time.time() - t0 < 6                                     # no 15 s wait
    t1 = time.time()
    r2 = edge.post("/devices/127.0.0.100/command", json={"command": "STATUS"})
    assert r2.status_code == 503 and "rebooting" in r2.json()["detail"]
    assert time.time() - t1 < 2                                     # fails fast, does not wait for a dead socket
    edge.wait_for(lambda: n.boot_count == 2 and connected(edge, "127.0.0.100"), 20, what="node back")
    st = edge.post("/devices/127.0.0.100/command", json={"command": "STATUS"}).json()
    assert st["ok"] and st["json"]["boot_count"] == 2
    ev = [e for e in edge.get("/events?after_seq=%d&device_id=127.0.0.100" % seq).json()["events"] if e["type"] == "node_rebooted"]
    assert ev and ev[0]["boot_count"] == 2 and ev[0]["requested"] is True


def test_unexpected_reset_drops_stale_socket(edge, make_node):
    n = make_node(101, hb_s=60).start(threads=("reader",))
    edge.wait_for(lambda: connected(edge, "127.0.0.101"), 10, what="connected")
    seq = edge.get("/events").json()["next_after_seq"]
    n._reboot()                                                      # hard reset: old TCP connection left open
    edge.wait_for(lambda: n.boot_count == 2 and connected(edge, "127.0.0.101"), 20, what="node back")
    t0 = time.time()
    st = edge.post("/devices/127.0.0.101/command", json={"command": "STATUS"}).json()
    assert st["ok"] and st["json"]["boot_count"] == 2 and time.time() - t0 < 3
    ev = [e for e in edge.get("/events?after_seq=%d&device_id=127.0.0.101" % seq).json()["events"] if e["type"] == "node_rebooted"]
    assert ev and ev[0]["requested"] is False
    assert "node rebooted (boot 1 -> 2)" in edge.log()


@pytest.mark.req("FW-13")
def test_node_ip_change_is_reported(edge, make_node):
    a = make_node(102)
    a.register()
    b = make_node(103)
    b.mac = a.mac                                                    # same node, new DHCP address
    b.register()
    ev = [e for e in edge.get("/events?type=node_ip_changed").json()["events"]]
    assert ev and ev[-1]["old_ip"] == "127.0.0.102" and ev[-1]["new_ip"] == "127.0.0.103"
