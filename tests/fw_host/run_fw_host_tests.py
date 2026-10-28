#!/usr/bin/env python3
"""
run_fw_host_tests.py - build and run the firmware host unit tests (layer L2 of the test strategy).

Each tests/fw_host/test_*.cpp declares in its header:
    // @extract firmware/sentinel_node/sentinel_node.ino: symbolA symbolB ...
    // @variant default:
    // @variant ota: -DArduino_OTA_MODE
The runner extracts the named symbols verbatim from the firmware (framework/extract.py), compiles
each variant with g++ and AddressSanitizer/UndefinedBehaviorSanitizer, runs it and writes
    build/fw_host/junit/<test>.<variant>.xml     (Jenkins junit step)
    build/fw_host/results/<test>.<variant>.json (ci/trace_report.py)

Usage:  python3 tests/fw_host/run_fw_host_tests.py [--repo DIR] [--out DIR] [--no-sanitize] [-k PATTERN]
Exit code 0 only if every variant of every test compiled and passed.
"""
import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
from xml.sax.saxutils import escape

HERE = os.path.dirname(os.path.abspath(__file__))


def parse_header(path):
    extracts, variants, csources, cflags = [], [], [], []
    for line in open(path, encoding="utf-8"):
        m = re.match(r"\s*//\s*@csource\s+(\S+)", line)
        if m:
            csources.append(m.group(1))
            continue
        m = re.match(r"\s*//\s*@cflags\s+(.*)", line)
        if m:
            cflags += m.group(1).split()
            continue
        m = re.match(r"\s*//\s*@extract\s+([^:]+):\s*(.*)", line)
        if m:
            extracts.append((m.group(1).strip(), m.group(2).split()))
            continue
        m = re.match(r"\s*//\s*@variant\s+(\w+):\s*(.*)", line)
        if m:
            variants.append((m.group(1), m.group(2).split()))
    return extracts, variants or [("default", [])], csources, cflags


def compile_error_junit(path, suite, log):
    with open(path, "w") as f:
        f.write('<?xml version="1.0" encoding="UTF-8"?>\n<testsuite name="%s" tests="1" errors="1">\n' % escape(suite))
        f.write('  <testcase classname="fw_host.%s" name="build"><error message="build or extraction failed">%s</error></testcase>\n'
                % (escape(suite), escape(log[-4000:])))
        f.write("</testsuite>\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=os.path.abspath(os.path.join(HERE, "..", "..")))
    ap.add_argument("--out", default=None)
    ap.add_argument("--no-sanitize", action="store_true")
    ap.add_argument("-k", default="", help="only tests whose file name contains this")
    args = ap.parse_args()
    repo = args.repo
    out = args.out or os.path.join(repo, "build", "fw_host")
    for d in ("junit", "results", "obj"):
        os.makedirs(os.path.join(out, d), exist_ok=True)
    cxx = os.environ.get("CXX", "g++")
    flags = ["-std=c++17", "-g", "-O0", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-unused-variable",
             "-Wno-unused-function", "-Wno-unused-but-set-variable", "-Wno-missing-field-initializers"]
    if not args.no_sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-sanitize-recover=undefined"]
    inc = ["-I", os.path.join(HERE, "mock_include"), "-I", os.path.join(HERE, "framework"),
           "-I", os.path.join(repo, "firmware", "sentinel_node"), "-I", os.path.join(HERE, "third_party", "littlefs")]
    cc = os.environ.get("CC", "gcc")
    cflags_base = ["-std=gnu99", "-g", "-O0"] + [f for f in flags if f.startswith("-fsanitize") or f.startswith("-fno-")]
    extractor = os.path.join(HERE, "framework", "extract.py")

    total_fail = 0
    ran = 0
    for test in sorted(glob.glob(os.path.join(HERE, "test_*.cpp"))):
        name = os.path.splitext(os.path.basename(test))[0]
        if args.k and args.k not in name:
            continue
        extracts, variants, csources, cflags = parse_header(test)
        for vname, vflags in variants:
            suite = "%s.%s" % (name, vname)
            work = os.path.join(out, "obj", suite)
            shutil.rmtree(work, ignore_errors=True)
            os.makedirs(work)
            # 1. extract firmware code
            parts, log = [], ""
            ok = True
            for k, (src, syms) in enumerate(extracts):
                part = os.path.join(work, "part%d.inc" % k)
                r = subprocess.run([sys.executable, extractor, os.path.join(repo, src), part] + syms,
                                   capture_output=True, text=True)
                log += r.stdout + r.stderr
                if r.returncode != 0:
                    ok = False
                    break
                parts.append(part)
            if ok:
                with open(os.path.join(work, "extracted.inc"), "w") as f:
                    for p in parts:
                        f.write(open(p).read() + "\n")
            # 2. compile (C sources such as LittleFS first, then the test)
            exe = os.path.join(work, name)
            objs = []
            for k, csrc in enumerate(csources):
                if not ok:
                    break
                obj = os.path.join(work, "c%d.o" % k)
                r = subprocess.run([cc] + cflags_base + cflags + vflags + inc + ["-c", os.path.join(repo, csrc), "-o", obj],
                                   capture_output=True, text=True)
                log += r.stdout + r.stderr
                ok = r.returncode == 0
                objs.append(obj)
            if ok:
                cmd = [cxx] + flags + cflags + vflags + inc + ["-I", work, test] + objs + ["-o", exe]
                r = subprocess.run(cmd, capture_output=True, text=True)
                log += r.stdout + r.stderr
                ok = r.returncode == 0
            junit = os.path.join(out, "junit", suite + ".xml")
            if not ok:
                print("[BUILD FAIL] %s\n%s" % (suite, log[-3000:]))
                compile_error_junit(junit, suite, log)
                total_fail += 1
                continue
            # 3. run
            env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0", UBSAN_OPTIONS="print_stacktrace=1")
            r = subprocess.run([exe, "--suite", suite, "--junit", junit,
                                "--json", os.path.join(out, "results", suite + ".json")],
                               capture_output=True, text=True, env=env, timeout=120)
            sys.stdout.write(r.stdout)
            if r.returncode != 0:
                sys.stdout.write(r.stderr[-3000:])
                total_fail += 1
                if not os.path.exists(junit) or "Sanitizer" in r.stderr:
                    compile_error_junit(junit, suite, r.stdout + r.stderr)
            ran += 1
    print("\nfw_host: %d suite variants run, %d failed" % (ran, total_fail))
    return 1 if total_fail else 0


if __name__ == "__main__":
    sys.exit(main())
