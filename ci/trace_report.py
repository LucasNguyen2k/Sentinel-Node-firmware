#!/usr/bin/env python3
"""
trace_report.py - requirement traceability matrix FW-01..FW-24 from all test layers.

Reads the requirement-tagged results of every layer:
    build/firmware/results.json          L1 build matrix
    build/fw_host/results/*.json         L2 firmware host unit tests
    build/server/results.json            L3 Edge Server tests
    build/hil/results.json               L4 HIL dry run (emulator)
    build/hil_hw/results.json            L4 HIL on hardware (when the bench ran)
and writes build/trace/traceability.{md,html,json}.

Status per requirement:
    FAIL      at least one tagged test failed                      -> pipeline fails
    PASS      tests passed on real firmware code (L1/L2/L4 hardware) and none failed
    PARTIAL   only server or emulator evidence (no test touched the firmware itself)
    WAIVED    no tests, reason recorded in ci/fw_requirements.json
    GAP       no tests and no waiver                               -> pipeline fails
"""
import glob
import html
import json
import os
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def load(build):
    rows = []
    sources = [os.path.join(build, "firmware", "results.json"), os.path.join(build, "server", "results.json"),
               os.path.join(build, "hil", "results.json"), os.path.join(build, "hil_hw", "results.json")]
    sources += sorted(glob.glob(os.path.join(build, "fw_host", "results", "*.json")))
    for p in sources:
        if not os.path.exists(p):
            continue
        for r in json.load(open(p)):
            if p.endswith(os.path.join("hil_hw", "results.json")):
                r["target"] = "hardware"
            rows.append(r)
    return rows


def main():
    # --no-gate: report only (used by ci/run_all.sh when a layer was skipped on purpose, e.g. SKIP_L1=1)
    no_gate = "--no-gate" in sys.argv[1:]
    build = os.path.join(REPO, "build")
    reqs = json.load(open(os.path.join(REPO, "ci", "fw_requirements.json")))["requirements"]
    rows = load(build)
    matrix = []
    for q in reqs:
        tests = [r for r in rows if q["id"] in [x.strip() for x in r.get("reqs", "").split(",")]]
        passed = [t for t in tests if t["outcome"] == "passed"]
        failed = [t for t in tests if t["outcome"] == "failed"]
        on_firmware = [t for t in passed if t["layer"] in ("build", "fw_host") or t.get("target") == "hardware"]
        layers = sorted(set(t["layer"] + ("(hw)" if t.get("target") == "hardware" else
                                          "(emu)" if t["layer"] == "hil" else "") for t in passed))
        if failed:
            status = "FAIL"
        elif on_firmware:
            status = "PASS"
        elif passed:
            status = "PARTIAL"
        elif q.get("waiver"):
            status = "WAIVED"
        else:
            status = "GAP"
        matrix.append({"id": q["id"], "title": q["title"], "status": status, "passed": len(passed), "failed": len(failed),
                       "layers": layers, "note": q.get("waiver") or q.get("note", ""),
                       "failed_tests": ["%s/%s" % (t["suite"], t["test"]) for t in failed]})
    out = os.path.join(build, "trace")
    os.makedirs(out, exist_ok=True)
    json.dump(matrix, open(os.path.join(out, "traceability.json"), "w"), indent=1)
    md = ["# Requirement traceability FW-01..FW-24", "",
          "| Req | Title | Status | Passed | Failed | Evidence layers | Note |", "|---|---|---|---|---|---|---|"]
    for m in matrix:
        md.append("| %s | %s | %s | %d | %d | %s | %s |" % (m["id"], m["title"], m["status"], m["passed"], m["failed"],
                                                          ", ".join(m["layers"]), m["note"]))
    open(os.path.join(out, "traceability.md"), "w").write("\n".join(md) + "\n")
    colour = {"PASS": "#d9f2d9", "PARTIAL": "#fff4cc", "WAIVED": "#e8e8e8", "FAIL": "#f8d0d0", "GAP": "#f8d0d0"}
    rows_html = "".join("<tr style='background:%s'><td>%s</td><td>%s</td><td><b>%s</b></td><td>%d</td><td>%d</td><td>%s</td>"
                        "<td>%s</td></tr>" % (colour[m["status"]], m["id"], html.escape(m["title"]), m["status"], m["passed"],
                                              m["failed"], ", ".join(m["layers"]),
                                              html.escape(m["note"] + ("; failed: " + ", ".join(m["failed_tests"])
                                                                       if m["failed_tests"] else "")))
                        for m in matrix)
    open(os.path.join(out, "traceability.html"), "w").write(
        "<html><head><meta charset='utf-8'><title>Traceability</title><style>body{font-family:sans-serif}"
        "td,th{border:1px solid #bbb;padding:4px 8px}table{border-collapse:collapse}</style></head><body>"
        "<h2>Sentinel Node - requirement traceability FW-01..FW-24</h2><table><tr><th>Req</th><th>Title</th><th>Status</th>"
        "<th>Passed</th><th>Failed</th><th>Evidence layers</th><th>Note</th></tr>%s</table></body></html>" % rows_html)
    for m in matrix:
        print("%-6s %-8s %3d passed %2d failed  %-38s %s" % (m["id"], m["status"], m["passed"], m["failed"],
                                                         ",".join(m["layers"]), m["note"]))
    bad = [m["id"] for m in matrix if m["status"] in ("FAIL", "GAP")]
    print("\ntraceability: %d requirements, %d PASS, %d PARTIAL, %d WAIVED, %d FAIL/GAP %s" % (
        len(matrix), sum(m["status"] == "PASS" for m in matrix), sum(m["status"] == "PARTIAL" for m in matrix),
        sum(m["status"] == "WAIVED" for m in matrix), len(bad), bad if bad else ""))
    if bad and no_gate:
        print("(--no-gate: a test layer was skipped, so missing evidence is reported but does not fail this run)")
        return 0
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
