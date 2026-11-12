#!/usr/bin/env python3
"""sentinel-status: one-screen health check of the Sentinel Edge Server (Rubik Pi 3).

    sudo sentinel-status            # full check (sudo: needed to read the settings files and the journal)
    sentinel-status --watch 5       # refresh every 5 s (Ctrl+C to stop)
    sentinel-status --json          # machine-readable
Exit code: 0 = all OK, 1 = warnings, 2 = problems.
"""
import argparse
import glob
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

OK, WARN, FAIL, INFO = "ok", "warn", "fail", "info"
EDGE_ENV, HWI_ENV = "/etc/sentinel-edge.env", "/etc/sentinel-hw-interface.env"
LEASES = ("/var/lib/misc/dnsmasq.leases", "/var/lib/dnsmasq/dnsmasq.leases")


# ---------------------------------------------------------------------------------------------- helpers
def sh(cmd, timeout=5):
    """Run a command; return (exit code, stdout). Never raises."""
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, p.stdout.strip()
    except (OSError, subprocess.TimeoutExpired):
        return 127, ""


def read_env(path):
    """KEY=VALUE file -> dict, or None if it cannot be read (no sudo)."""
    try:
        out = {}
        for line in open(path):
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                out[k.strip()] = v.strip().strip('"').strip("'")
        return out
    except OSError:
        return None


def http_json(url, key=None, timeout=4):
    req = urllib.request.Request(url, headers={"X-API-Key": key} if key else {})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode())


def port_open(host, port, timeout=1.5):
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def ago(seconds):
    if seconds is None:
        return "never"
    s = int(seconds)
    if s < 60:
        return "%d s ago" % s
    if s < 3600:
        return "%d min ago" % (s // 60)
    return "%d h %d min ago" % (s // 3600, (s % 3600) // 60)


def duration(seconds):
    s = int(seconds)
    if s < 3600:
        return "%d min" % (s // 60)
    if s < 86400:
        return "%d h %d min" % (s // 3600, (s % 3600) // 60)
    return "%d d %d h" % (s // 86400, (s % 86400) // 3600)


def service(name):
    """(state, seconds running or None) of a systemd service; ('n/a', None) without systemd."""
    if not shutil.which("systemctl"):
        return "n/a", None
    _, state = sh(["systemctl", "is-active", name])
    since = None
    if state == "active":
        _, ts = sh(["systemctl", "show", name, "--property=ActiveEnterTimestampMonotonic", "--value"])
        try:
            since = time.monotonic() - int(ts) / 1e6
        except ValueError:
            pass
    return state or "unknown", since


class Report:
    def __init__(self):
        self.sections = []

    def section(self, title):
        self.sections.append((title, []))

    def add(self, level, name, text):
        self.sections[-1][1].append((level, name, text))

    def counts(self):
        items = [i for _, rows in self.sections for i in rows]
        return sum(1 for i in items if i[0] == WARN), sum(1 for i in items if i[0] == FAIL)


# ---------------------------------------------------------------------------------------------- checks
def check_wifi(r):
    r.section("WI-FI ACCESS POINT")
    _, procs = sh(["pgrep", "-a", "hostapd"])
    conf_path = None
    for line in procs.splitlines():
        m = re.search(r"(\S+\.conf)\b", line)
        if m:
            conf_path = m.group(1)
    conf = {}
    for p in [conf_path] if conf_path else ["/etc/hostapd/hostapd.conf"]:
        try:
            for line in open(p):
                if "=" in line and not line.lstrip().startswith("#"):
                    k, v = line.strip().split("=", 1)
                    conf[k] = v
        except (OSError, TypeError):
            pass
    iface = conf.get("interface", "wlan1")
    if procs:
        ssid = conf.get("ssid")
        if not ssid:
            _, st = sh(["hostapd_cli", "-i", iface, "status"])
            m = re.search(r"^ssid\[0\]=(.*)$", st, re.M)
            ssid = m.group(1) if m else None
        detail = 'SSID "%s" · %s' % (ssid, iface) if ssid else "SSID unknown (run with sudo) · %s" % iface
        if conf.get("channel"):
            detail += " · channel " + conf["channel"]
        r.add(OK, "hostapd", "running · " + detail)
    else:
        r.add(FAIL, "hostapd", "not running: the nodes have no Wi-Fi (start your access-point script)")
    _, addr = sh(["ip", "-4", "-o", "addr", "show", "br0"])
    m = re.search(r"inet (\S+)", addr)
    if m and m.group(1).startswith("192.168.100.1/"):
        r.add(OK, "br0", m.group(1))
    elif m:
        r.add(WARN, "br0", "%s (expected 192.168.100.1/24)" % m.group(1))
    else:
        r.add(FAIL, "br0", "no address: the SN Device Driver cannot listen on 192.168.100.1")
    state, _ = service("dnsmasq")
    leases = []
    for path in LEASES:
        try:
            leases = [l.split() for l in open(path) if l.strip()]
            break
        except OSError:
            continue
    if state == "active" or sh(["pgrep", "dnsmasq"])[0] == 0:
        r.add(OK, "dnsmasq", "running · %d lease%s" % (len(leases), "" if len(leases) == 1 else "s"))
    else:
        r.add(FAIL, "dnsmasq", "not running: nodes get no IP address")
    if procs:
        code, sta = sh(["iw", "dev", iface, "station", "dump"])
        if code == 0:
            n = len(re.findall(r"^Station ", sta, re.M))
            r.add(OK if n else WARN, "stations", "%d connected" % n if n else "none connected")
    return leases


def check_services(r, args, edge_env, hwi_env):
    r.section("SERVICES")
    # Mosquitto
    state, since = service("mosquitto")
    host = (hwi_env or {}).get("HWI_MQTT_HOST", "127.0.0.1")
    port = int((hwi_env or {}).get("HWI_MQTT_PORT", "1883"))
    listening = port_open(host, port) if not host.startswith("/") else os.path.exists(host)
    births = mqtt_births(host, port, hwi_env) if listening else None
    if state not in ("active", "n/a") and not listening:
        r.add(FAIL, "mosquitto", "%s: sudo systemctl enable --now mosquitto" % state)
    elif not listening:
        r.add(FAIL, "mosquitto", "running but not listening on %s:%s (check /etc/mosquitto/conf.d/)" % (host, port))
    else:
        text = "running" + (" " + duration(since) if since else "") + " · listening %s:%s" % (host, port)
        if births is None:
            text += " · MQTT test skipped (no paho)"
        elif isinstance(births, str):
            r.add(FAIL, "mosquitto", text + " · MQTT connect failed: " + births)
            births = None
        else:
            text += " · %d node birth%s on sentinel/v1" % (births, "" if births == 1 else "s")
        if births is not None or "skipped" in text:
            r.add(OK, "mosquitto", text)
    # SN Device Driver
    state, since = service("sentinel-edge")
    api = args.api
    try:
        h = http_json(api.rsplit("/api/", 1)[0] + "/health")
        ok_api = True
    except (urllib.error.URLError, OSError, ValueError):
        h, ok_api = {}, False
    if ok_api and state not in ("active", "n/a"):
        r.add(WARN, "sentinel-edge", "API ok · %d device%s, but not running as the service (started by hand?)" % (
            h.get("devices", 0), "" if h.get("devices") == 1 else "s"))
    elif ok_api:
        r.add(OK, "sentinel-edge", "running%s · API ok · %d device%s" % (
            " " + duration(since) if since else "", h.get("devices", 0), "" if h.get("devices") == 1 else "s"))
    elif state == "active":
        r.add(FAIL, "sentinel-edge", "running but API not answering on %s (journalctl -u sentinel-edge)" % api)
    else:
        r.add(FAIL, "sentinel-edge", "%s: sudo systemctl restart sentinel-edge" % state)
    # HW Interface
    state, since = service("sentinel-hw-interface")
    if state == "active":
        jtext, jlevel = journal_summary("sentinel-hw-interface", since)
        r.add(jlevel, "hw-interface", "running " + (duration(since) if since else "") + " · " + jtext)
    elif sh(["pgrep", "-f", "[-]m hw_interface"])[0] == 0:
        r.add(WARN, "hw-interface", "running, but not as the service (started by hand?)")
    else:
        r.add(FAIL, "hw-interface", "%s: sudo systemctl restart sentinel-hw-interface" % state)
    # Settings (values never printed)
    if edge_env is None or hwi_env is None:
        r.add(INFO, "settings", "not checked: run with sudo to read %s and %s" % (EDGE_ENV, HWI_ENV))
    else:
        a, b = edge_env.get("AMB82_API_KEY", ""), hwi_env.get("HWI_DRIVER_KEY", "")
        problems = []
        if not a or a == "change-me":
            problems.append("AMB82_API_KEY not set")
        if not b or b == "change-me":
            problems.append("HWI_DRIVER_KEY not set")
        if a and b and a != b:
            problems.append("HWI_DRIVER_KEY differs from AMB82_API_KEY")
        if not edge_env.get("AMB82_NODE_TOKEN"):
            problems.append("AMB82_NODE_TOKEN not set (no HTTP command fallback)")
        if problems:
            r.add(FAIL if any("KEY" in p and "TOKEN" not in p for p in problems) else WARN, "settings", "; ".join(problems))
        else:
            r.add(OK, "settings", "API keys match · node token set")
    return ok_api


def mqtt_births(host, port, hwi_env):
    """Connect like node-gateway, count retained birth messages. None = paho not available, str = error."""
    try:
        import paho.mqtt.client as mqtt
    except ImportError:
        return None
    seen, done = set(), {"rc": None}
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="sentinel-status-%d" % os.getpid(),
                    transport="unix" if host.startswith("/") else "tcp")
    if hwi_env and hwi_env.get("HWI_MQTT_USER"):
        c.username_pw_set(hwi_env["HWI_MQTT_USER"], hwi_env.get("HWI_MQTT_PASS", ""))
    c.on_connect = lambda cl, u, f, rc, p: (done.__setitem__("rc", rc), cl.subscribe("sentinel/v1/+/birth", 1))
    c.on_message = lambda cl, u, m: seen.add(m.topic) if m.retain and m.payload else None
    try:
        c.connect(host, port, 10) if not host.startswith("/") else c.connect(host, 1883, 10)
        c.loop_start()
        time.sleep(1.5)
        c.loop_stop()
        c.disconnect()
    except OSError as e:
        return str(e)
    if done["rc"] is None or str(done["rc"]) != "Success":
        return "refused (%s)" % done["rc"]
    return len(seen)


def journal_summary(unit, since):
    """MQTT connection state and warnings of the last 10 minutes, from the journal."""
    if not shutil.which("journalctl"):
        return "journal not available", INFO
    code, out = sh(["journalctl", "-u", unit, "--since", "-10min", "--no-pager", "-o", "cat"])
    if code != 0:
        return "journal not readable (run with sudo)", INFO
    lines = out.splitlines()
    warns = [l for l in lines if " WARNING " in l or " ERROR " in l or "Traceback" in l]
    # the connection state = the last connect / disconnect line since the service started (not only recent lines:
    # after days of running the start-up "MQTT connected" is long gone from the tail of the log)
    span = ["--since", "-%ds" % (int(since) + 5)] if since else ["-n", "5000"]
    _, all_out = sh(["journalctl", "-u", unit, "--no-pager", "-o", "cat"] + span, timeout=15)
    states = re.findall(r"MQTT (connected|disconnected|connection refused)", all_out)
    if states:
        mqtt_ok = states[-1] == "connected"
    else:                                            # log rotated away: is there a live connection to the broker?
        _, socks = sh(["ss", "-tnpH", "state", "established", "( dport = :1883 )"])
        mqtt_ok = "python" in socks
    text = ("MQTT connected" if mqtt_ok else "MQTT not connected") + " · %d warning%s (10 min)" % (
        len(warns), "" if len(warns) == 1 else "s")
    if warns:
        last = re.sub(r"^\S+ \S+ (WARNING|ERROR) \S+: ", "", warns[-1])
        text += ' · last: "%s"' % (last[:70] + ("…" if len(last) > 70 else ""))
    return text, (FAIL if not mqtt_ok else WARN if warns else OK)


def check_nodes(r, args, edge_env, leases):
    r.section("NODES")
    key = (edge_env or {}).get("AMB82_API_KEY") or os.getenv("SENTINEL_API_KEY")
    if not key:
        r.add(INFO, "nodes", "not checked: run with sudo (API key in %s) or set SENTINEL_API_KEY" % EDGE_ENV)
        return []
    try:
        devices = http_json(args.api + "/devices", key)
    except urllib.error.HTTPError as e:
        r.add(FAIL, "nodes", "API answered %d (wrong API key?)" % e.code)
        return []
    except (urllib.error.URLError, OSError, ValueError):
        r.add(INFO, "nodes", "not checked: SN Device Driver API not answering")
        return []
    if not devices:
        r.add(WARN, "nodes", "no node registered yet")
    for d in devices:
        node, sensor = d.get("node") or {}, d.get("sensor") or {}
        mac = (node.get("node_id") or "").upper()
        dev_id = "SN-" + mac if mac else "(no MAC yet)"
        age, st_age = sensor.get("last_data_age_s"), node.get("status_age_s")
        name = d.get("ip", "?")
        ident = "%s · %s · fw %s · " % (dev_id, (d.get("name") or "?")[:20], node.get("firmware") or "?")
        if sensor.get("online"):
            level = WARN if (st_age is not None and st_age > 180) else OK
            text = ident + "online · data %s · status %s" % (ago(age), ago(st_age))
            try:                                         # the node's own health flags (0 = part not working)
                st = (http_json(args.api + "/devices/%s/status" % d.get("ip"), key) or {}).get("status") or {}
            except (urllib.error.URLError, OSError, ValueError):
                st = {}
            failed = [label for k, label in (("cam_ok", "camera"), ("mic_ok", "microphone"),
                                             ("aht_ok", "temperature/humidity"), ("gas_ok", "gas sensor"))
                      if st.get(k) == 0]
            if failed:
                level, text = WARN, text + " · not working: " + ", ".join(failed)
        else:
            level, text = FAIL, ident + "OFFLINE · last data %s" % ago(age)
        r.add(level, name, text)
    return devices


def check_data(r):
    r.section("DATA AND MACHINE")
    root = "/var/lib/sentinel/data-area"
    if os.path.isdir(root):
        newest, count, size = {}, 0, 0
        for path in glob.glob(os.path.join(root, "*", "*", "*", "*")):
            if path.endswith(".part"):
                continue
            try:
                st = os.stat(path)
            except OSError:
                continue
            ds = path.split(os.sep)[-3]
            newest[ds] = max(newest.get(ds, 0), st.st_mtime)
            count, size = count + 1, size + st.st_size
        now = time.time()
        if not newest:
            r.add(WARN, "data area", "empty: no files delivered yet")
        else:
            parts = ["%s %s" % (ds, ago(now - t)) for ds, t in sorted(newest.items()) if ds in ("ACO-01", "IMG-01")]
            stale = [ds for ds in ("ACO-01", "IMG-01") if ds in newest and now - newest[ds] > 420]
            missing = [ds for ds in ("ACO-01", "IMG-01") if ds not in newest]
            text = " · ".join(parts + ["%d files" % count, "%.1f MB" % (size / 1e6)])
            if missing:
                text += " · no %s yet" % "/".join(missing)
            r.add(WARN if stale or missing else OK, "data area", text + (" · %s overdue (every 5 min)" % "/".join(stale) if stale else ""))
    else:
        r.add(FAIL, "data area", "%s missing (run sudo ci/setup_agent.sh)" % root)
    for path in ("/var/lib/sentinel", "/opt/sentinel-edge"):
        if os.path.isdir(path):
            du = shutil.disk_usage(path)
            free_pct = du.free * 100 / du.total
            level = FAIL if free_pct < 5 else WARN if free_pct < 15 else OK
            r.add(level, "disk", "%s %.1f GB free (%d%%)" % (path, du.free / 1e9, free_pct))
            break
    code, out = sh(["timedatectl", "show", "--property=NTPSynchronized", "--value"])
    if code == 0 and out:
        r.add(OK if out == "yes" else WARN, "clock", "NTP synchronized" if out == "yes" else "NOT synchronized: event times may be wrong")


# ---------------------------------------------------------------------------------------------- output
SYMBOL = {OK: "✔", WARN: "!", FAIL: "✖", INFO: "·"}
COLOR = {OK: "\033[32m", WARN: "\033[33m", FAIL: "\033[31m", INFO: "\033[90m"}


def render(r, color):
    c = (lambda lvl, s: COLOR[lvl] + s + "\033[0m") if color else (lambda lvl, s: s)
    bold = (lambda s: "\033[1m" + s + "\033[0m") if color else (lambda s: s)
    out = [bold("Sentinel Edge Server status") + "   %s   (%s)" % (time.strftime("%Y-%m-%d %H:%M:%S"), socket.gethostname()), ""]
    width = min(15, max([len(n) for _, rows in r.sections for _, n, _ in rows] + [10]))
    for title, rows in r.sections:
        out.append(bold(title))
        for lvl, name, text in rows:
            out.append("  %s %s  %s" % (c(lvl, SYMBOL[lvl]), name.ljust(width), text))
        out.append("")
    warns, fails = r.counts()
    if fails:
        out.append(c(FAIL, "RESULT: %d problem%s, %d warning%s" % (fails, "" if fails == 1 else "s", warns, "" if warns == 1 else "s")))
    elif warns:
        out.append(c(WARN, "RESULT: %d warning%s" % (warns, "" if warns == 1 else "s")))
    else:
        out.append(c(OK, "RESULT: all OK"))
    return "\n".join(out)


def run_once(args):
    edge_env, hwi_env = read_env(EDGE_ENV), read_env(HWI_ENV)
    r = Report()
    leases = check_wifi(r)
    check_services(r, args, edge_env, hwi_env)
    check_nodes(r, args, edge_env, leases)
    check_data(r)
    return r


def main():
    ap = argparse.ArgumentParser(description="Health check of the Sentinel Edge Server.")
    ap.add_argument("--api", default="http://127.0.0.1:8080/api/v1", help="SN Device Driver API")
    ap.add_argument("--watch", type=float, metavar="SECONDS", help="refresh every SECONDS")
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    ap.add_argument("--no-color", action="store_true")
    args = ap.parse_args()
    color = sys.stdout.isatty() and not args.no_color and not args.json
    try:
        while True:
            r = run_once(args)
            warns, fails = r.counts()
            if args.json:
                print(json.dumps({"time": time.strftime("%Y-%m-%dT%H:%M:%S"), "warnings": warns, "problems": fails,
                                  "sections": [{"title": t, "items": [{"level": l, "name": n, "text": x} for l, n, x in rows]}
                                               for t, rows in r.sections]}, indent=2))
            else:
                if args.watch:
                    print("\033[2J\033[H", end="")
                print(render(r, color))
            if not args.watch:
                return 2 if fails else 1 if warns else 0
            time.sleep(args.watch)
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
