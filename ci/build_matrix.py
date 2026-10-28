#!/usr/bin/env python3
"""
build_matrix.py - compile every firmware configuration with the real Realtek toolchain (layer L1).

Variants are defined in ci/build_variants.json: board options (e.g. OTA Mode), source edits that
switch feature #defines on or off, the expected result (pass, or fail with a given #error) and the
artifacts to keep. Warnings reported in our own source files fail a variant marked no_warnings.

Environment:
    ARDUINO_CLI          arduino-cli executable (default: arduino-cli)
    ARDUINO_EXTRA_PROPS  extra --build-property values, separated by ';' (optional)
Outputs (under --out, default build/firmware):
    <variant>/...                     build folders, compiler logs
    artifacts/<variant>/...           flash_ntz.bin, ota.bin
    junit.xml, results.json           for Jenkins and ci/trace_report.py
"""
import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import time
from xml.sax.saxutils import escape

OWN_FILES = r"(sentinel_node\.ino|ezsn_hal\.cpp|ezsn_hal\.h):\d+:\d+: warning"


def arduino_data_dir(cli):
    try:
        out = subprocess.run([cli, "config", "get", "directories.data"], capture_output=True, text=True).stdout.strip()
        if out:
            return out
    except OSError:
        pass
    return os.path.expanduser("~/.arduino15")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
    ap.add_argument("--out", default=None)
    ap.add_argument("--only", default="", help="comma-separated variant names")
    args = ap.parse_args()
    repo = args.repo
    out = args.out or os.path.join(repo, "build", "firmware")
    os.makedirs(out, exist_ok=True)
    cfg = json.load(open(os.path.join(repo, "ci", "build_variants.json")))
    cli = os.environ.get("ARDUINO_CLI", "arduino-cli")
    extra = [p for p in os.environ.get("ARDUINO_EXTRA_PROPS", "").split(";") if p.strip()]
    sketch_src = os.path.join(repo, "firmware", "sentinel_node")
    results = []
    for v in cfg["variants"]:
        if args.only and v["name"] not in args.only.split(","):
            continue
        name = v["name"]
        work = os.path.join(out, name)
        shutil.rmtree(work, ignore_errors=True)
        sketch = os.path.join(work, "sentinel_node")
        shutil.copytree(sketch_src, sketch)
        status, msg = "passed", ""
        for fname, old, new in v.get("edits", []):
            p = os.path.join(sketch, fname)
            s = open(p, encoding="utf-8").read()
            if s.count(old) != 1:
                status, msg = "error", "edit pattern %r found %d times in %s" % (old, s.count(old), fname)
                break
            open(p, "w", encoding="utf-8").write(s.replace(old, new))
        log, warnings, size = "", [], ""
        if status == "passed":
            fqbn = cfg["fqbn"] + ((":" + v["options"]) if v.get("options") else "")
            cmd = [cli, "compile", "-b", fqbn, "--warnings", "all", "--build-path", os.path.join(work, "build")]
            for p in extra:
                cmd += ["--build-property", p]
            cmd.append(sketch)
            t0 = time.time()
            r = subprocess.run(cmd, capture_output=True, text=True)
            log = r.stdout + r.stderr
            open(os.path.join(work, "compile.log"), "w").write(log)
            warnings = sorted(set(re.findall(OWN_FILES + r".*", log)))
            m = re.search(r"Sketch uses (\d+) bytes", log)
            size = m.group(1) if m else ""
            built = r.returncode == 0
            if v["expect"] == "pass":
                if not built:
                    status, msg = "failed", "compile failed:\n" + log[-3000:]
                elif v.get("no_warnings") and warnings:
                    status, msg = "failed", "%d warning(s) in our files:\n%s" % (len(warnings), "\n".join(warnings))
            else:
                if built:
                    status, msg = "failed", "expected the build to fail but it succeeded"
                elif v.get("expect_error") and v["expect_error"] not in log:
                    status, msg = "failed", "build failed, but not with the expected error %r" % v["expect_error"]
            # artifacts
            if status == "passed" and built:
                adir = os.path.join(out, "artifacts", name)
                os.makedirs(adir, exist_ok=True)
                for a in v.get("artifacts", []):
                    if a == "ota.bin":
                        cands = [c for c in glob.glob(os.path.join(arduino_data_dir(cli), "packages", "realtek", "tools",
                                                                   "ameba_pro2_tools", "*", "ota.bin"))
                                 if os.path.getmtime(c) >= t0 - 1]
                        if not cands:
                            status, msg = "failed", "ota.bin was not produced by the OTA-mode build"
                        else:
                            shutil.copy(max(cands, key=os.path.getmtime), adir)
                    else:
                        src = os.path.join(work, "build", a)
                        if os.path.exists(src):
                            shutil.copy(src, adir)
                        else:
                            status, msg = "failed", "artifact %s missing" % a
        results.append({"layer": "build", "suite": "firmware_build", "test": name, "reqs": v.get("reqs", ""),
                        "outcome": status if status in ("passed", "failed") else "failed",
                        "message": msg, "size": size, "warnings": len(warnings)})
        print("[%s] %-18s %s %s" % ("  OK  " if status == "passed" else " FAIL ", name,
                                     ("size=" + size) if size else "", msg.splitlines()[0] if msg else ""))
    with open(os.path.join(out, "results.json"), "w") as f:
        json.dump(results, f, indent=1)
    bad = [r for r in results if r["outcome"] != "passed"]
    with open(os.path.join(out, "junit.xml"), "w") as f:
        f.write('<?xml version="1.0" encoding="UTF-8"?>\n<testsuite name="firmware_build" tests="%d" failures="%d">\n'
                % (len(results), len(bad)))
        for r in results:
            f.write('  <testcase classname="build.firmware" name="%s [%s]">' % (escape(r["test"]), escape(r["reqs"])))
            if r["outcome"] != "passed":
                f.write('<failure message="%s">%s</failure>' % (escape(r["message"].splitlines()[0] if r["message"] else "failed"),
                                                                escape(r["message"])))
            f.write("</testcase>\n")
        f.write("</testsuite>\n")
    print("\nbuild matrix: %d variants, %d failed" % (len(results), len(bad)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
