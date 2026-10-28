"""edge_server.py - start a real Edge Server process for tests and talk to its API."""
import os
import socket
import subprocess
import sys
import time

import requests

SERVER = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "server", "sentinel_node_api.py"))


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class EdgeServer:
    """A sentinel_node_api.py subprocess on loopback with its own ports and data directory."""

    def __init__(self, data_dir, api_key="test-key", **env):
        self.reg_port, self.api_port = free_port(), free_port()
        self.api_key = api_key
        self.data_dir = str(data_dir)
        self.log_path = os.path.join(self.data_dir, "server.log")
        e = dict(os.environ)
        e.update({"AMB82_DEVICE_NET_IP": "127.0.0.1", "AMB82_DEVICE_SUBNET": "127.0.0.0/8",
                  "AMB82_REG_PORT": str(self.reg_port), "AMB82_API_PORT": str(self.api_port), "AMB82_API_HOST": "127.0.0.1",
                  "AMB82_API_KEY": api_key, "AMB82_DATA_DIR": self.data_dir, "AMB82_AUDIO_RECORDER": "none",
                  "AMB82_ONLINE_WINDOW": "3", "AMB82_STATUS_STALE": "4"})
        e.update({k: str(v) for k, v in env.items()})
        self.env = e
        self.proc = None

    @property
    def api(self):
        return "http://127.0.0.1:%d/api/v1" % self.api_port

    def start(self, timeout=20):
        os.makedirs(self.data_dir, exist_ok=True)
        self._log = open(self.log_path, "w")
        self.proc = subprocess.Popen([sys.executable, SERVER], env=self.env, stdout=self._log, stderr=subprocess.STDOUT)
        t0 = time.time()
        while time.time() - t0 < timeout:
            if self.proc.poll() is not None:
                raise RuntimeError("Edge Server exited:\n" + self.log())
            try:
                if requests.get(self.api + "/devices", headers=self.h, timeout=1).status_code == 200:
                    return self
            except requests.RequestException:
                pass
            time.sleep(0.2)
        raise RuntimeError("Edge Server did not start:\n" + self.log())

    @property
    def h(self):
        return {"X-API-Key": self.api_key}

    def get(self, path, **kw):
        return requests.get(self.api + path, headers=self.h, timeout=kw.pop("timeout", 10), **kw)

    def post(self, path, **kw):
        return requests.post(self.api + path, headers=self.h, timeout=kw.pop("timeout", 30), **kw)

    def log(self):
        try:
            return open(self.log_path).read()
        except OSError:
            return ""

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        if getattr(self, "_log", None):
            self._log.close()

    def wait_for(self, fn, timeout=10.0, interval=0.2, what="condition"):
        t0 = time.time()
        last = None
        while time.time() - t0 < timeout:
            try:
                last = fn()
                if last:
                    return last
            except (requests.RequestException, KeyError, IndexError, ValueError):
                pass
            time.sleep(interval)
        raise AssertionError("timed out after %.0f s waiting for %s (last value: %r)" % (timeout, what, last))
