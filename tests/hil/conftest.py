"""
L4 hardware-in-the-loop suite.

  --target hardware   a real Sentinel Node registered with a running Edge Server (nightly / release)
  --target emulator   a local Edge Server + firmware emulator (every commit: proves the HIL tests work)

Hardware example (on the HIL bench = the Edge Server machine):
  pytest tests/hil --target hardware --api-url http://127.0.0.1:8080/api/v1 --api-key $AMB82_API_KEY \
         --node-ip 192.168.100.44 --fw-version 1.3.2 --serial-port /dev/ttyUSB0 --destructive
"""
import os
import time

import pytest
import requests


def pytest_addoption(parser):
    g = parser.getgroup("hil")
    g.addoption("--target", choices=["hardware", "emulator"], default=None)
    g.addoption("--api-url", default="http://127.0.0.1:8080/api/v1")
    g.addoption("--api-key", default=os.environ.get("AMB82_API_KEY", ""))
    g.addoption("--node-ip", default=None)
    g.addoption("--fw-version", default=None, help="firmware version the node must report")
    g.addoption("--serial-port", default=None, help="node USB serial port for boot-log checks (optional)")
    g.addoption("--soak-minutes", type=float, default=None, help="socket stability window")
    g.addoption("--allowed-selftest-mask", type=int, default=0, help="self-test failures to tolerate (bit mask)")
    g.addoption("--event-hold-s", type=float, default=None, help="node event_hold_s (default 30 on hardware)")
    g.addoption("--destructive", action="store_true", help="allow reboot / config-changing tests")


def pytest_collection_modifyitems(config, items):
    target = config.getoption("--target")
    for it in items:
        if "hil" + os.sep not in str(it.fspath):
            continue
        if target is None:
            it.add_marker(pytest.mark.skip(reason="HIL suite: pass --target hardware|emulator"))
        elif "destructive" in it.keywords and not config.getoption("--destructive") and target == "hardware":
            it.add_marker(pytest.mark.skip(reason="destructive test: pass --destructive"))
        elif "serial" in it.keywords and (target != "hardware" or not config.getoption("--serial-port")):
            it.add_marker(pytest.mark.skip(reason="needs --target hardware and --serial-port"))


class Bench:
    def __init__(self, api, key, node_ip, fw, emulator=None, hold_s=30.0, soak_s=60.0, allowed_mask=0, serial_port=None):
        self.api, self.key, self.ip, self.fw = api.rstrip("/"), key, node_ip, fw
        self.emulator, self.hold_s, self.soak_s, self.allowed_mask = emulator, hold_s, soak_s, allowed_mask
        self.serial_port = serial_port

    @property
    def h(self):
        return {"X-API-Key": self.key}

    def get(self, path, **kw):
        return requests.get(self.api + path, headers=self.h, timeout=15, **kw)

    def cmd(self, command, timeout=20):
        r = requests.post(self.api + "/devices/%s/command" % self.ip, headers=self.h,
                          json={"command": command, "timeout": timeout}, timeout=timeout + 10)
        assert r.status_code == 200, "command %s failed: %s %s" % (command, r.status_code, r.text)
        return r.json()

    def status(self):
        return self.get("/devices/%s/status" % self.ip).json()["status"]

    def device(self):
        return self.get("/devices/%s" % self.ip).json()

    def seq(self):
        return self.get("/events").json()["next_after_seq"]

    def events(self, after):
        return self.get("/events?device_id=%s&after_seq=%d&limit=1000" % (self.ip, after)).json()["events"]

    def wait_for(self, fn, timeout, what):
        t0, last = time.time(), None
        while time.time() - t0 < timeout:
            try:
                last = fn()
                if last:
                    return last
            except (requests.RequestException, KeyError, IndexError, ValueError, AssertionError):
                pass
            time.sleep(0.5)
        raise AssertionError("timed out after %.0f s waiting for %s (last: %r)" % (timeout, what, last))


@pytest.fixture(scope="session")
def bench(request, tmp_path_factory):
    c = request.config
    target = c.getoption("--target")
    if target == "emulator":
        from edge_server import EdgeServer
        from node_emulator import NodeEmulator
        srv = EdgeServer(tmp_path_factory.mktemp("hil_edge")).start()
        node = NodeEmulator(ip="127.0.0.77", reg_port=srv.reg_port, mac="00E04C0F5C77", fw="1.5.2",
                            hb_s=2, event_hold_s=3, flash_mb=32).start()
        b = Bench(srv.api, srv.api_key, "127.0.0.77", "1.5.2", emulator=node, hold_s=3,
                  soak_s=(c.getoption("--soak-minutes") or 0.25) * 60)
        b.wait_for(lambda: b.get("/devices/127.0.0.77/status").status_code == 200, 15, "emulated node heartbeat")
        yield b
        node.stop()
        srv.stop()
        assert "Traceback" not in srv.log(), srv.log()[-3000:]
        return
    if not c.getoption("--node-ip"):
        pytest.skip("--node-ip is required for --target hardware")
    b = Bench(c.getoption("--api-url"), c.getoption("--api-key"), c.getoption("--node-ip"), c.getoption("--fw-version"),
              hold_s=c.getoption("--event-hold-s") or 30, soak_s=(c.getoption("--soak-minutes") or 5) * 60,
              allowed_mask=c.getoption("--allowed-selftest-mask"), serial_port=c.getoption("--serial-port"))
    b.wait_for(lambda: b.device()["sensor"]["connected"], 120, "node %s connected to the Edge Server" % b.ip)
    yield b
