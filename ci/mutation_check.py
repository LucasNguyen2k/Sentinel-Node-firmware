#!/usr/bin/env python3
"""
mutation_check.py - proves the firmware host tests can fail (nightly CI stage).

Each mutant injects one realistic bug - several are re-introductions of defects fixed during
development - into a scratch copy of the firmware and runs the relevant host-test suite against it.
The check passes only if every mutant is detected ("killed"). A surviving mutant means the tests
no longer protect that behaviour.

Usage:  python3 ci/mutation_check.py [--repo DIR] [--junit build/mutation/junit.xml]
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from xml.sax.saxutils import escape

INO = "firmware/sentinel_node/sentinel_node.ino"
HAL = "firmware/sentinel_node/ezsn_hal.cpp"

# (id, file, original text, mutated text, test suite filter, what it simulates)
MUTANTS = [
    ("M01", INO, "gasFiltered < cfg.gasAlarmRaw * 0.9f", "gasFiltered < cfg.gasAlarmRaw * 0.8f", "gas",
     "gas raw-fallback hysteresis changed"),
    ("M02", INO, "if (recent >= 2 ||", "if (recent >= 3 ||", "fusion",
     "fusion needs three sensors instead of two"),
    ("M03", INO, "now - lastAnyAlarmMs > cfg.eventHoldS * 1000UL", "now - lastAnyAlarmMs > cfg.eventHoldS * 100UL", "fusion",
     "event hold time 10x too short"),
    ("M04", INO, "if (newState > oldState && !cfg.maintenance && !thermalDerate)", "if (newState > oldState && !thermalDerate)",
     "fusion", "maintenance mode no longer suppresses pictures"),
    ("M05", INO, "bool gEff = gasAlarm || nowMs < simGasUntil;", "bool gEff = gasAlarm;", "fusion",
     "re-introduce the SIM GAS reporting bug fixed in 1.1.9"),
    ("M06", INO, "return ezhal_mic_present() && acFrames > 0 && (millis() - acLastFrameMs < 3000);",
     "return ezhal_mic_present() && (millis() - acLastFrameMs < 3000);", "acoustic",
     "re-introduce the mic_ok-at-boot bug found by these tests"),
    ("M07", INO, "if (mask && it.secret) return \"****\";", "", "config", "secrets no longer masked"),
    ("M08", INO, "0xEDB88320UL", "0xEDB88321UL", "config", "CRC polynomial wrong"),
    ("M09", INO, "if ((uint32_t)*size > OTA_SLOT_BYTES) {", "if ((uint32_t)*size > OTA_SLOT_BYTES + 4096) {", "ota",
     "OTA accepts an image larger than the slot"),
    ("M10", INO, "while (sentNow < 50) {", "while (sentNow < 500) {", "storage", "store-and-forward starves live data"),
    ("M11", INO, "return epochAtSync + (millis() - millisAtSync) / 1000;", "return epochAtSync + millis() / 1000;", "records",
     "clock ignores the sync point (breaks after rollover)"),
    ("M12", INO, "  String _data = (String)temp + \",\" + (String)(ahtOk ? humidity*100 : -999) + \",\" + (String)chem;",
     "  String _data = (String)temp + \",\" + (String)chem + \",\" + (String)(ahtOk ? humidity*100 : -999);", "records",
     "record fields swapped"),
    ("M13", HAL, "    } else if (n == -1) {", "    } else if (n <= 0) {", "datasocket",
     "re-introduce 'idle socket treated as closed' (1.1.7 bug)"),
    ("M14", INO, "if (!force && eventState != EVT_NORMAL) {", "if (eventState != EVT_NORMAL) {", "ota",
     "OTA_SD FORCE no longer overrides a leak event"),
    ("M15", HAL, "target = (ha.seq >= hb.seq) ? slot_b : slot_a;", "target = (ha.seq >= hb.seq) ? slot_a : slot_b;",
     "flashlog", "config save overwrites the newest copy (power cut loses config)"),
    ("M16", HAL, "    if (ezhal_flash_program(ezhal_flog_addr(log, s.cur, s.cur_off + 2), &f, 1) != HS_OK) return HS_ERROR;\n    s.cur_off",
     "    s.cur_off", "flashlog", "'sent' flag not stored: records resent after reboot"),
    ("M17", INO, "if (lastFlashBufMs != 0 && millis() - lastFlashBufMs < FLASH_BUF_INTERVAL_MS) {", "if (false) {",
     "flashstore", "offline thinning disabled (flash fills in ~2 minutes)"),
    ("M18", HAL, "    return addr >= EZHAL_FLASH_USER_BASE && addr <= EZHAL_FLASH_USER_END && len <= EZHAL_FLASH_USER_END - addr;",
     "    return true;", "flashlog", "flash range guard removed (firmware area writable)"),
    ("M19", INO, "        scheduleReboot(RR_CMD_REBOOT, \"cmd\", 1000);\n        return \"OK rebooting in 1 s\";",
     "        rebootWithReason(RR_CMD_REBOOT, \"cmd\");\n        return \"OK rebooting in 1 s\";", "commands",
     "re-introduce 'REBOOT resets before replying' (1.4.0 bug)"),
    ("M20", HAL, "    if (ezhal_aht_crc8(frame, 6) != frame[6]) return AHT_CRC;\n", "", "aht",
     "AHT20 CRC check removed (corrupted readings accepted)"),
    ("M21", HAL, "        if (!(format_if_blank && ezfs_blank())) {", "        if (!format_if_blank) {", "test_fs",
     "damaged file system formatted automatically (data loss)"),
    ("M22", INO, "    if (sent > 0) fsBufSaveCursor();\n", "", "fsdata",
     "offline-buffer read position not saved (records resent after reboot)"),
    ("M23", INO, "        if (st.bytes <= maxBytes || st.files <= 1) return;", "        return;", "fsdata",
     "picture retention disabled (file system fills up)"),
    ("M24", INO, "    if (!fsAvailable || clipRing == NULL || serverReachable()) return;",
     "    if (!fsAvailable || clipRing == NULL) return;", "audioclip",
     "sound clips recorded while the server is reachable (flash wear, needless uploads)"),
    ("M25", INO, "    uint32_t start = clipTrigger > pre ? clipTrigger - pre : 0;", "    uint32_t start = clipTrigger;", "audioclip",
     "sound clip loses the 2 s before the trigger (the sound that caused the alarm)"),
    ("M26", HAL, "        if (now - g_ahtProbeMs < EZHAL_AHT_PROBE_MS) return;", "", "aht",
     "missing AHT20 polled continuously instead of once a minute"),
    ("M27", HAL, "    uint32_t now = millis();\n    taskENTER_CRITICAL();\n    uint32_t v = g_ahtValidMs;",
     "    uint32_t now = millis();\n    ezhal_env_service(now);\n    taskENTER_CRITICAL();\n    uint32_t v = g_ahtValidMs;", "aht",
     "sampling read does I2C work again (the 1.6.0 regression: 0.15 Hz without a sensor)"),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
    ap.add_argument("--junit", default=None)
    args = ap.parse_args()
    runner = os.path.join(args.repo, "tests", "fw_host", "run_fw_host_tests.py")
    results = []
    for mid, rel, orig, mut, suite, why in MUTANTS:
        work = tempfile.mkdtemp(prefix="mut_%s_" % mid)
        try:
            shutil.copytree(os.path.join(args.repo, "firmware"), os.path.join(work, "firmware"))
            path = os.path.join(work, rel)
            src = open(path, encoding="utf-8").read()
            if src.count(orig) != 1:
                results.append((mid, why, "invalid", "pattern found %d times (update this mutant)" % src.count(orig)))
                continue
            open(path, "w", encoding="utf-8").write(src.replace(orig, mut))
            r = subprocess.run([sys.executable, runner, "--repo", work, "--out", os.path.join(work, "out"),
                                "--no-sanitize", "-k", suite], capture_output=True, text=True,
                               cwd=args.repo)
            # runner uses tests from the real repo but firmware from the mutated copy
            killed = r.returncode != 0
            results.append((mid, why, "killed" if killed else "SURVIVED",
                            "" if killed else "suite '%s' still passed" % suite))
        finally:
            shutil.rmtree(work, ignore_errors=True)
    bad = [r for r in results if r[2] != "killed"]
    for mid, why, status, note in results:
        print("%-4s %-9s %s %s" % (mid, status, why, ("- " + note) if note else ""))
    print("\nmutation check: %d mutants, %d killed, %d survived/invalid" % (len(results), len(results) - len(bad), len(bad)))
    if args.junit:
        os.makedirs(os.path.dirname(os.path.abspath(args.junit)), exist_ok=True)
        with open(args.junit, "w") as f:
            f.write('<?xml version="1.0" encoding="UTF-8"?>\n<testsuite name="mutation" tests="%d" failures="%d">\n' % (len(results), len(bad)))
            for mid, why, status, note in results:
                f.write('  <testcase classname="mutation" name="%s %s [FW-22]">' % (mid, escape(why)))
                if status != "killed":
                    f.write('<failure message="%s">%s</failure>' % (escape(status), escape(note)))
                f.write("</testcase>\n")
            f.write("</testsuite>\n")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
