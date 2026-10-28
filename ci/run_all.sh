#!/usr/bin/env bash
# run_all.sh - run the same stages as the Jenkins pipeline on a developer machine.
#   ci/run_all.sh            all stages (L1 needs arduino-cli + the Realtek core)
#   SKIP_L1=1 ci/run_all.sh  skip the firmware build matrix
#   MUTATION=1 ci/run_all.sh also run the mutation check
set -uo pipefail
cd "$(dirname "$0")/.."
rc=0
step() { echo; echo "=== $1"; shift; "$@" || rc=1; }
step "static checks" python3 -m py_compile server/sentinel_node_api.py ci/build_matrix.py ci/trace_report.py ci/mutation_check.py tests/common/node_emulator.py
[ "${SKIP_L1:-0}" = 1 ] || step "L1 firmware build matrix" python3 ci/build_matrix.py
step "L2 firmware host tests" python3 tests/fw_host/run_fw_host_tests.py
step "L3 Edge Server tests" python3 -m pytest tests/server -q -p no:cacheprovider --timeout 120 --junitxml build/server/junit.xml --req-json build/server/results.json
step "L4 HIL dry run (emulator)" python3 -m pytest tests/hil --target emulator --destructive -q -p no:cacheprovider --timeout 300 --junitxml build/hil/junit.xml --req-json build/hil/results.json
[ "${MUTATION:-0}" = 1 ] && step "mutation check" python3 ci/mutation_check.py --junit build/mutation/junit.xml
if [ "${SKIP_L1:-0}" = 1 ]; then step "traceability (advisory: L1 skipped)" python3 ci/trace_report.py --no-gate
else step "traceability" python3 ci/trace_report.py; fi
echo; [ $rc = 0 ] && echo "ALL STAGES PASSED" || echo "SOME STAGES FAILED"
exit $rc
