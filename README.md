# EyeZense Sentinel Node — firmware, Edge Server and CI/CD

| Path | Content |
|---|---|
| `firmware/sentinel_node/` | Firmware 1.3.2 (Arduino sketch + HAL). Build with Ameba Pro2 core 4.1.0. |
| `server/` | Edge Server API 1.1.0 |
| `Jenkinsfile`, `ci/` | Jenkins pipeline, build matrix, mutation check, traceability, agent setup, HIL bench scripts |
| `tests/` | L2 firmware host tests, L3 Edge Server tests, L4 hardware-in-the-loop suite |

Run every stage locally (the same as Jenkins):

```
pip install -r server/requirements.txt -r tests/requirements.txt
ci/run_all.sh                 # SKIP_L1=1 without arduino-cli, MUTATION=1 adds the mutation check
```

Setup and test specification: document "Sentinel Node CI/CD Verification System – Jenkins Setup Guide & Test Specification".
Design: document "EyeZense Sentinel Node – Architecture & Detailed Design".
