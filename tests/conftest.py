"""
Root pytest configuration shared by tests/server (L3) and tests/hil (L4).

* @pytest.mark.req("FW-10", "FW-15") tags a test with the firmware requirements it verifies.
* --req-json PATH writes one JSON record per test (layer, test, reqs, outcome) for ci/trace_report.py.
"""
import json
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "common"))
sys.path.insert(0, os.path.join(HERE, "..", "server"))

_records = {}


def pytest_addoption(parser):
    parser.addoption("--req-json", default=None, help="write requirement-tagged results to this JSON file")


def pytest_configure(config):
    config.addinivalue_line("markers", "req(*ids): firmware requirements verified by the test (FW-xx)")
    config.addinivalue_line("markers", "hil: needs a real Sentinel Node or the emulator target")
    config.addinivalue_line("markers", "destructive: reboots the node or changes persistent state")
    config.addinivalue_line("markers", "soak: long-running stability test")
    config.addinivalue_line("markers", "serial: needs the node USB serial port")


def _layer(item):
    p = str(item.fspath)
    return "hil" if os.sep + "hil" + os.sep in p else ("server" if os.sep + "server" + os.sep in p else "other")


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    rep = outcome.get_result()
    reqs = []
    for m in item.iter_markers("req"):
        reqs.extend(m.args)
    key = item.nodeid
    rec = _records.setdefault(key, {"layer": _layer(item), "suite": os.path.basename(str(item.fspath)),
                                    "test": item.name, "reqs": ",".join(sorted(set(reqs))), "outcome": "passed"})
    if rep.skipped and rec["outcome"] == "passed":
        rec["outcome"] = "skipped"
    elif rep.failed:
        rec["outcome"] = "failed"
    if hasattr(item, "config") and getattr(item.config.option, "target", None):
        rec["target"] = item.config.option.target


def pytest_sessionfinish(session, exitstatus):
    path = session.config.getoption("--req-json")
    if path:
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        with open(path, "w") as f:
            json.dump(list(_records.values()), f, indent=1)
