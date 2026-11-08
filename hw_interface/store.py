"""Ledger (SQLite: seq numbers, file register) and the data area (atomic writes, retention)."""
import hashlib
import os
import sqlite3
import threading
import time
from datetime import datetime, timezone


def utc_iso(t: float) -> str:
    """Epoch seconds -> 2026-10-22T14:15:37.120Z"""
    return datetime.fromtimestamp(t, tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.") + "%03dZ" % int(round((t % 1) * 1000) % 1000)


def file_stamp(t: float) -> str:
    """Epoch seconds -> 20261022_141537_120000 (UTC, microseconds)"""
    d = datetime.fromtimestamp(t, tz=timezone.utc)
    return d.strftime("%Y%m%d_%H%M%S_") + "%06d" % d.microsecond


class Ledger:
    def __init__(self, path):
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        self.db = sqlite3.connect(path, check_same_thread=False)
        self.lock = threading.Lock()
        with self.lock, self.db:
            self.db.execute("CREATE TABLE IF NOT EXISTS seq (device_id TEXT, dataset_id TEXT, value INTEGER, "
                            "PRIMARY KEY (device_id, dataset_id))")
            self.db.execute("CREATE TABLE IF NOT EXISTS files (path TEXT PRIMARY KEY, device_id TEXT, dataset_id TEXT, "
                            "size INTEGER, sha256 TEXT, created REAL)")
            self.db.execute("CREATE TABLE IF NOT EXISTS events (device_id TEXT, node_event_id TEXT, "
                            "PRIMARY KEY (device_id, node_event_id))")
            self.db.execute("CREATE TABLE IF NOT EXISTS devices (device_id TEXT PRIMARY KEY, first_seen REAL)")
            self.db.execute("CREATE TABLE IF NOT EXISTS kv (key TEXT PRIMARY KEY, value TEXT)")

    def get(self, key, default=None):
        with self.lock:
            row = self.db.execute("SELECT value FROM kv WHERE key=?", (key,)).fetchone()
            return row[0] if row else default

    def put(self, key, value):
        with self.lock, self.db:
            self.db.execute("INSERT OR REPLACE INTO kv VALUES (?, ?)", (key, str(value)))

    def first_seen(self, device_id) -> float:
        """When this HW Interface first saw the node (recorded on the first call, kept across restarts)."""
        with self.lock, self.db:
            row = self.db.execute("SELECT first_seen FROM devices WHERE device_id=?", (device_id,)).fetchone()
            if row:
                return row[0]
            now = time.time()
            self.db.execute("INSERT INTO devices VALUES (?, ?)", (device_id, now))
            return now

    def event_known(self, device_id, node_event_id) -> bool:
        with self.lock:
            return self.db.execute("SELECT 1 FROM events WHERE device_id=? AND node_event_id=?",
                                   (device_id, node_event_id)).fetchone() is not None

    def mark_event(self, device_id, node_event_id):
        with self.lock, self.db:
            self.db.execute("INSERT OR IGNORE INTO events VALUES (?, ?)", (device_id, node_event_id))

    def next_seq(self, device_id, dataset_id) -> int:
        with self.lock, self.db:
            row = self.db.execute("SELECT value FROM seq WHERE device_id=? AND dataset_id=?", (device_id, dataset_id)).fetchone()
            v = (row[0] if row else 0) + 1
            self.db.execute("INSERT OR REPLACE INTO seq VALUES (?, ?, ?)", (device_id, dataset_id, v))
            return v

    def add_file(self, path, device_id, dataset_id, size, sha):
        with self.lock, self.db:
            self.db.execute("INSERT OR REPLACE INTO files VALUES (?, ?, ?, ?, ?, ?)",
                            (path, device_id, dataset_id, size, sha, time.time()))

    def files_older_than(self, cutoff):
        with self.lock:
            return [r[0] for r in self.db.execute("SELECT path FROM files WHERE created < ?", (cutoff,))]

    def forget_file(self, path):
        with self.lock, self.db:
            self.db.execute("DELETE FROM files WHERE path=?", (path,))


class DataArea:
    def __init__(self, root, ledger: Ledger):
        self.root = os.path.abspath(root)
        self.ledger = ledger
        os.makedirs(self.root, exist_ok=True)

    def rel_path(self, device_id, dataset_id, start_t, ext) -> str:
        stamp = file_stamp(start_t)
        return "%s/%s/%s/%s.%s" % (device_id, dataset_id, stamp[:8], stamp, ext)

    def write(self, rel, data: bytes, device_id, dataset_id):
        """Write as <name>.part, flush to disk, then rename: a file is complete before it is announced."""
        final = os.path.join(self.root, rel)
        os.makedirs(os.path.dirname(final), exist_ok=True)
        part = final + ".part"
        with open(part, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.replace(part, final)
        sha = hashlib.sha256(data).hexdigest()
        self.ledger.add_file(rel, device_id, dataset_id, len(data), sha)
        return len(data), sha

    def cleanup(self, retention_s) -> int:
        removed = 0
        for rel in self.ledger.files_older_than(time.time() - retention_s):
            try:
                os.remove(os.path.join(self.root, rel))
            except FileNotFoundError:
                pass
            self.ledger.forget_file(rel)
            removed += 1
            d = os.path.dirname(os.path.join(self.root, rel))
            while d.startswith(self.root) and d != self.root:
                try:
                    os.rmdir(d)                  # only removes empty folders
                except OSError:
                    break
                d = os.path.dirname(d)
        return removed
