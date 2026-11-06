"""Client for the SN Device Driver (sentinel_node_api.py): REST API and its data folder."""
import glob
import os

import requests


class DriverClient:
    def __init__(self, api, key, data_dir, timeout=10):
        self.api = api.rstrip("/")
        self.root = self.api.rsplit("/api/", 1)[0]
        self.h = {"X-API-Key": key} if key else {}
        self.data_dir = data_dir
        self.timeout = timeout

    def _get(self, path, **params):
        r = requests.get(self.api + path, headers=self.h, params=params, timeout=self.timeout)
        r.raise_for_status()
        return r.json()

    def devices(self):
        return self._get("/devices")

    def status(self, ip):
        return self._get("/devices/%s/status" % ip)

    def events(self, after_seq):
        return self._get("/events", after_seq=after_seq, limit=1000)

    def records(self, ip, after_seq, limit=5000):
        return self._get("/devices/%s/sensor" % ip, after_seq=after_seq, limit=limit)

    def command(self, ip, text, timeout=15):
        r = requests.post(self.api + "/devices/%s/command" % ip, headers=self.h,
                          json={"command": text, "timeout": timeout}, timeout=timeout + 5)
        if r.status_code != 200:
            try:
                detail = r.json().get("detail")
            except ValueError:
                detail = r.text
            return {"ok": False, "reply": None, "detail": str(detail)}
        return r.json()

    def download(self, url):
        full = url if url.startswith("http") else self.root + url
        r = requests.get(full, headers=self.h, timeout=self.timeout)
        r.raise_for_status()
        return r.content

    def node_folder(self, ip):
        """The driver keeps each node's files in <data dir>/<name>_<ip>/; the name may change, the IP suffix not."""
        hits = sorted(glob.glob(os.path.join(self.data_dir, "*_%s" % ip)), key=os.path.getmtime)
        return hits[-1] if hits else None
