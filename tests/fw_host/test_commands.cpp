// Command interpreter shared by data socket, HTTP /cmd and serial CLI (FW-10, FW-11, FW-17, FW-19, FW-21, FW-22)
// @extract firmware/sentinel_node/sentinel_node.ino: FW_VERSION DEVICE_NAME SERVER_ADDRESS SERVER_PORT SOCKET_BASE_PORT API_TOKEN_DEFAULT CONFIG_MAGIC CONFIG_VERSION QCS6490 OTA_FILE OTA_SLOT_BYTES OTA_MIN_BYTES NodeConfig BootInfo ResetReasonCode EventState CfgType CfgItem ssid pass cfg bootInfo cfgTable cfgTableLen sdMutex camMutex flashMutex sdOk eventState epochAtSync millisAtSync timeSynced acHfDb acFloorDb acFloorInit acAboveCnt acAlarm eventCounter simGasUntil simOfflineUntil bCapturePicture captureEventId otaInProgress otaStartMs otaResult bootDone bootStageName fsAvailable fsDetail rebootAtMs rebootReason rebootDetail lockM unlockM setDefaultConfig setClock fnum findCfgItem cfgItemValue setCfgItem configDump eventStateName simOffline otaPrecheck scheduleReboot rebootCheck executeCommand sampleCount sampleRateHz sampleWinStartMs sampleWinCount sampleRateUpdate samplingOk effectiveSampleMs
// @variant normal:
// @variant otamode: -DArduino_OTA_MODE
#include "minitest.h"
#include "fw_env.h"
// collaborators replaced by test doubles
int g_saves = 0, g_cam_on = 0, g_cam_idle = 0; uint32_t g_reboot = 0, g_pending = 0; std::string g_reboot_detail, g_pending_detail; int g_log_n = -1;
bool saveConfig() { g_saves++; return true; }
String buildStatus(bool json) { return json ? String("{\"status\":1}") : String("status=1"); }
String runSelfTest() { return "AHT20:PASS\nRESULT:PASS"; }
String calibrateGas() { return "OK gas_r0=12.000 saved"; }
void cameraOn() { g_cam_on++; }
void cameraIdle() { g_cam_idle++; }
String readEventLogTail(int n) { g_log_n = n; return "log"; }
void rebootWithReason(uint32_t r, String d) { g_reboot = r; g_reboot_detail = d.s; }
void recordResetReason(uint32_t r, String d) { g_pending = r; g_pending_detail = d.s; }
void LOG(String m) { Serial.println(m); }
#include "extracted.inc"

static void fresh()
{
    fake_reset_all();
    sdMutex = xSemaphoreCreateMutex();               // real handles, so lockM()/unlockM() really lock
    flashMutex = xSemaphoreCreateMutex();
    camMutex = xSemaphoreCreateMutex();
    setDefaultConfig();
    bootDone = true; bootStageName = "running";
    bootInfo.bootCount = 3; eventCounter = 0; eventState = EVT_NORMAL; sdOk = true;
    timeSynced = false; simGasUntil = 0; bCapturePicture = false; otaInProgress = false; otaResult = "none";
    g_saves = g_cam_on = g_cam_idle = 0; g_reboot = g_pending = 0; g_log_n = -1;
}
static String cmd(const char *c) { return executeCommand(String(c)); }

TEST(ping_reports_name_and_version, "FW-10,FW-19")
{
    fresh();
    CHECK_EQ(cmd("PING"), String("PONG ") + DEVICE_NAME + " " + FW_VERSION);
    CHECK_EQ(cmd("  ping  "), String("PONG ") + DEVICE_NAME + " " + FW_VERSION);     // trimmed, case-insensitive
}

TEST(help_lists_every_command, "FW-19")
{
    fresh();
    String h = cmd("HELP");
    for (auto c : {"PING", "STATUS", "CONFIG", "SET", "SAVE", "RESET_CONFIG", "CAPTURE", "CAL_GAS", "CAL_AC", "SELFTEST",
                   "MAINT", "POWER", "SIM", "TIME", "LOG", "OTA_SD [FORCE]", "REBOOT"})
        CHECK_CONTAINS(h, c);
}

TEST(status_and_config_and_selftest, "FW-13,FW-19,FW-22")
{
    fresh();
    CHECK_EQ(cmd("STATUS"), String("{\"status\":1}"));
    CHECK_CONTAINS(cmd("CONFIG"), "sample_ms=50");
    CHECK_CONTAINS(cmd("SELFTEST"), "RESULT:PASS");
    CHECK_CONTAINS(cmd("CAL_GAS"), "OK gas_r0=");
}

TEST(set_changes_ram_only_until_save, "FW-21")
{
    fresh();
    CHECK_CONTAINS(cmd("SET sample_ms 200"), "OK sample_ms");
    CHECK_EQ(cfg.sampleIntervalMs, 200u);
    CHECK_EQ(g_saves, 0);
    CHECK_CONTAINS(cmd("SET hb_s=30"), "OK hb_s");                   // key=value form
    CHECK_EQ(cfg.heartbeatIntervalS, 30u);
    CHECK_EQ(cmd("SAVE"), String("OK saved"));
    CHECK_EQ(g_saves, 1);
}

TEST(set_errors, "FW-21")
{
    fresh();
    CHECK_CONTAINS(cmd("SET sample_ms"), "ERR usage");
    CHECK_CONTAINS(cmd("SET bogus 1"), "ERR unknown key");
    String tooLong = String("SET name ") + std::string(60, 'n').c_str();   // name holds 31 characters
    CHECK_CONTAINS(executeCommand(tooLong), "ERR unknown key or bad value");
}

TEST(reset_config_restores_defaults, "FW-21")
{
    fresh();
    cfg.sampleIntervalMs = 999;
    CHECK_CONTAINS(cmd("RESET_CONFIG"), "OK defaults saved");
    CHECK_EQ(cfg.sampleIntervalMs, 50u);
    CHECK_EQ(g_saves, 1);
}

TEST(capture_requests_manual_picture, "FW-07")
{
    fresh();
    CHECK_EQ(cmd("CAPTURE"), String("OK capture M3_1"));
    CHECK(bCapturePicture);
    CHECK_EQ(captureEventId, String("M3_1"));
}

TEST(acoustic_floor_recalibration, "FW-19")
{
    fresh();
    acHfDb = -47.5f; acFloorDb = -60; acAlarm = true; acAboveCnt = 9;
    CHECK_CONTAINS(cmd("CAL_AC"), "OK acoustic floor=-47.5");
    CHECK_EQ(acFloorDb, -47.5f);
    CHECK(!acAlarm);
    CHECK_EQ(acAboveCnt, 0u);
}

TEST(maintenance_mode_persists, "FW-11,FW-19")
{
    fresh();
    CHECK_EQ(cmd("MAINT 1"), String("OK maintenance ON"));
    CHECK_EQ(cfg.maintenance, (uint8_t)1);
    CHECK_EQ(cmd("MAINT 0"), String("OK maintenance OFF"));
    CHECK_EQ(g_saves, 2);
}

TEST(power_modes_switch_camera, "FW-11")
{
    fresh();
    CHECK_EQ(cmd("POWER 1"), String("OK power ECO"));
    CHECK_EQ(cfg.powerMode, (uint8_t)1);
    CHECK_EQ(g_cam_idle, 1);
    CHECK_EQ(cmd("POWER 0"), String("OK power ACTIVE"));
    CHECK_EQ(g_cam_on, 1);
}

TEST(simulation_commands, "FW-22")
{
    fresh();
    CHECK_EQ(cmd("SIM GAS 30"), String("OK sim GAS 30 s"));
    CHECK_EQ(simGasUntil, millis() + 30000);
    CHECK_EQ(cmd("sim ac"), String("OK sim AC 60 s"));             // default 60 s
    CHECK_EQ(cmd("SIM CLEAR"), String("OK sim CLEAR 60 s"));
    CHECK_EQ(simGasUntil, 0u);
    CHECK_CONTAINS(cmd("SIM FIRE"), "ERR");
    CHECK_EQ(cmd("SIM OFFLINE 20"), String("OK sim OFFLINE 20 s"));
    CHECK(simOffline());
    g_fake_millis += 20001;
    CHECK(!simOffline());                            // ends by itself
    cmd("SIM OFFLINE 99999");
    CHECK_EQ(simOfflineUntil, millis() + 3600000u);  // capped at one hour
    cmd("SIM CLEAR");
    CHECK(!simOffline());
}

TEST(time_command_sets_clock, "FW-15")
{
    fresh();
    CHECK_EQ(cmd("TIME 1791500000"), String("OK time 1791500000"));
    CHECK(timeSynced);
    CHECK_EQ(epochAtSync, 1791500000u);
    fresh();
    CHECK_CONTAINS(cmd("TIME 12345"), "ERR bad epoch");
    CHECK(!timeSynced);
}

TEST(log_command, "FW-18")
{
    fresh();
    cmd("LOG 5"); CHECK_EQ(g_log_n, 5);
    cmd("LOG");   CHECK_EQ(g_log_n, 20);
}

TEST(reboot_replies_first_then_resets, "FW-18,FW-20,FW-10")
{
    // regression test for 1.4.1: REBOOT used to reset before replying (server waited 15 s)
    fresh();
    rebootAtMs = 0;
    CHECK_EQ(cmd("REBOOT"), String("OK rebooting in 1 s"));
    CHECK_EQ(g_reboot, 0u);                          // nothing yet: the reply goes out first
    g_fake_millis += 999; rebootCheck();
    CHECK_EQ(g_reboot, 0u);
    g_fake_millis += 1; rebootCheck();
    CHECK_EQ(g_reboot, (uint32_t)RR_CMD_REBOOT);
    CHECK_EQ(g_reboot_detail, std::string("cmd"));
    g_reboot = 0; rebootCheck();
    CHECK_EQ(g_reboot, 0u);                          // only once
}

TEST(flash_probe_command_locks_storage, "FW-02,FW-22")
{
    fresh();
    String r = cmd("FLASH_PROBE");
    CHECK(r.startsWith("OK\n"));
    CHECK_CONTAINS(r, "verdict: fake");
    CHECK_EQ(g_probe_write, false);
    CHECK_EQ(g_probe_lock_depth, 2);                 // storage and flash mutexes held during the probe
    CHECK_EQ(g_lock_depth, 0);                       // and released afterwards
    cmd("flash_probe write");
    CHECK_EQ(g_probe_write, true);
    CHECK_CONTAINS(cmd("HELP"), "FLASH_PROBE [WRITE]");
}

TEST(service_commands_work_during_boot, "FW-19,FW-20,FW-01")
{
    fresh();
    bootDone = false; bootStageName = "self-test";
    CHECK(cmd("PING").startsWith("PONG"));
    CHECK_EQ(cmd("SET hb_s 30"), String("OK hb_s (use SAVE to persist; network keys need REBOOT)"));
    CHECK(cmd("FLASH_PROBE").startsWith("OK"));
    CHECK_EQ(cmd("REBOOT"), String("OK rebooting in 1 s"));
    String r = cmd("CAPTURE");                       // needs camera + threads: refused until boot completes
    CHECK(r.startsWith("ERR still booting (stage self-test)"));
    CHECK_CONTAINS(r, "FLASH_PROBE");
    CHECK(cmd("SIM GAS 5").startsWith("ERR still booting"));
    CHECK(!bCapturePicture);
}

TEST(unknown_command, "FW-10")
{
    fresh();
    CHECK_EQ(cmd("FLY"), String("ERR unknown command (HELP)"));
}

#ifndef Arduino_OTA_MODE
TEST(ota_sd_refused_on_normal_build, "FW-17")
{
    fresh();
    fakehal.files["ota.bin"] = std::string(3297284, 'F');
    CHECK_CONTAINS(cmd("OTA_SD"), "built without OTA Mode");
    CHECK_EQ(fakehal.ota_starts, 0);
    CHECK(!otaInProgress);
    CHECK_EQ(g_pending, 0u);
}
#else
TEST(ota_sd_starts_and_records_state, "FW-17")
{
    fresh();
    fakehal.files["ota.bin"] = std::string(3297284, 'F');
    String r = cmd("OTA_SD");
    CHECK(r.startsWith(String("OK OTA started from SD (3297284 bytes, running ") + FW_VERSION + ")"));
    CHECK(otaInProgress);
    CHECK_EQ(otaResult, String("running"));
    CHECK_EQ(g_pending, (uint32_t)RR_OTA);
    CHECK_EQ(g_pending_detail, std::string("sd:") + FW_VERSION);
    CHECK_EQ(fakehal.ota_starts, 1);
    CHECK_EQ(g_lock_depth, 0);                                       // SD lock released
}

TEST(ota_sd_force_and_start_failure, "FW-17")
{
    fresh();
    fakehal.files["ota.bin"] = std::string(3297284, 'F');
    eventState = EVT_SUSPECT;
    CHECK_CONTAINS(cmd("OTA_SD"), "leak event");
    CHECK(cmd("OTA_SD force").startsWith("OK"));
    fresh();
    fakehal.files["ota.bin"] = std::string(3297284, 'F');
    fakehal.ota_start_status = HS_ERROR;
    CHECK_EQ(cmd("OTA_SD"), String("ERR could not start OTA"));
    CHECK(!otaInProgress);
    CHECK_EQ(g_pending, (uint32_t)RR_UNEXPECTED);
    CHECK_EQ(otaResult, String("failed: could not start"));
}
#endif

TEST(sampling_health_self_test_line, "FW-03,FW-13,FW-22")
{
    fresh();
    sampleCount = 0; sampleRateHz = 0; sampleWinStartMs = 0; sampleWinCount = 0;
    String line;
    CHECK(samplingOk(&line));
    CHECK(line.startsWith("SAMPLING:N/A"));                  // not measured yet
    sampleRateUpdate(1000);
    for (int i = 0; i < 200; i++) sampleCount++;             // 20 Hz for 10 s
    sampleRateUpdate(11000);
    CHECK(samplingOk(&line));
    CHECK(line.startsWith("SAMPLING:PASS 20.0 Hz"));
    sampleCount += 2;                                        // 0.15 Hz: the 1.6.0 fault on boards without AHT20
    sampleRateUpdate(24000);
    CHECK(!samplingOk(&line));
    CHECK(line.startsWith("SAMPLING:FAIL 0.2 Hz"));
}

MINITEST_MAIN
