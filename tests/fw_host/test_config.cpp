// Configuration and boot information in flash, reset reasons (FW-08, FW-16, FW-18, FW-20, FW-21)
// @extract firmware/sentinel_node/sentinel_node.ino: DEVICE_NAME SERVER_ADDRESS SERVER_PORT SOCKET_BASE_PORT API_TOKEN_DEFAULT CONFIG_MAGIC CONFIG_VERSION CONFIG_FLASH_ADDR BOOTINFO_FLASH_ADDR QCS6490 NodeConfig BootInfo ResetReasonCode CfgType CfgItem ssid pass cfg bootInfo lastResetReason lastResetDetail flashMutex cfgTable cfgTableLen lockM unlockM crc32Calc setDefaultConfig configValid loadConfig saveConfig writeBootInfo loadBootInfo recordResetReason rebootWithReason resetReasonName findCfgItem fnum cfgItemValue setCfgItem configDump
// @extract firmware/sentinel_node/ezsn_hal.cpp: EZREC_MAGIC EzRecHeader ezhal_crc32 ezhal_frec_slot_valid ezhal_frec_read ezhal_frec_write
#include "minitest.h"
#include "fw_env.h"
void LOG(String m) { Serial.println(m); }
#include "extracted.inc"
#include <set>

static void fresh()
{
    fake_reset_all();
    memset(&cfg, 0, sizeof(cfg));
    memset(&bootInfo, 0, sizeof(bootInfo));
}

// simulate a reboot: RAM is lost, flash is kept
static void reboot_and_load()
{
    memset(&cfg, 0, sizeof(cfg));
    memset(&bootInfo, 0, sizeof(bootInfo));
    loadConfig();
    loadBootInfo();
}

TEST(crc32_standard_check_value, "FW-08")
{
    const char *v = "123456789";
    CHECK_EQ(crc32Calc((const uint8_t *)v, 9), 0xCBF43926u);
}

TEST(config_fits_one_flash_sector, "FW-08,FW-21")
{
    CHECK(sizeof(NodeConfig) + 16 <= HAL_FLASH_SECTOR);     // record-store header + payload
    CHECK(sizeof(BootInfo) <= HAL_FLASH_SECTOR);
    CHECK_EQ(CONFIG_FLASH_ADDR % HAL_FLASH_SECTOR, 0);
    CHECK_EQ(BOOTINFO_FLASH_ADDR % HAL_FLASH_SECTOR, 0);
    CHECK(CONFIG_FLASH_ADDR >= 0xF00000);            // outside every partition (design doc 4.14)
}

TEST(blank_flash_gets_defaults_written, "FW-08,FW-21")
{
    fresh();
    loadConfig();
    CHECK_EQ(cfg.magic, (uint32_t)CONFIG_MAGIC);
    NodeConfig stored;
    CHECK_EQ(ezhal_frec_read(EZHAL_FLASH_CFG_A, EZHAL_FLASH_CFG_B, &stored, sizeof(stored), NULL), HS_OK);
    CHECK_EQ(String(stored.serverAddr), String(SERVER_ADDRESS));
    CHECK_EQ(String(cfg.apiToken), String(API_TOKEN_DEFAULT));
}

TEST(saved_config_survives_reboot, "FW-08,FW-21")
{
    fresh();
    loadConfig();
    cfg.sampleIntervalMs = 123;
    cfg.gasR0 = 4.5f;
    strcpy(cfg.deviceName, "NODE_A");
    CHECK(saveConfig());
    reboot_and_load();
    CHECK_EQ(cfg.sampleIntervalMs, 123u);
    CHECK_EQ(cfg.gasR0, 4.5f);
    CHECK_EQ(String(cfg.deviceName), String("NODE_A"));
}

TEST(corrupted_newest_config_falls_back_to_previous, "FW-08,FW-20")
{
    fresh();
    loadConfig();                                    // defaults -> slot B
    cfg.sampleIntervalMs = 123; saveConfig();        // -> slot A
    cfg.sampleIntervalMs = 999; saveConfig();        // -> slot B (newest)
    fakehal.flash[EZHAL_FLASH_CFG_B][25] ^= 0xFF;    // bit rot in the newest copy (device name byte)
    reboot_and_load();
    CHECK_EQ(cfg.sampleIntervalMs, 123u);            // previous copy, not defaults
}

TEST(both_copies_corrupted_gives_defaults, "FW-08,FW-20")
{
    fresh();
    loadConfig();
    cfg.sampleIntervalMs = 999; saveConfig();
    fakehal.flash[EZHAL_FLASH_CFG_A][25] ^= 0xFF;
    fakehal.flash[EZHAL_FLASH_CFG_B][25] ^= 0xFF;
    reboot_and_load();
    CHECK_EQ(cfg.sampleIntervalMs, 50u);
    CHECK_EQ(cfg.magic, (uint32_t)CONFIG_MAGIC);
}

TEST(migrates_single_sector_config_of_1_3_2, "FW-08,FW-21")
{
    fresh();
    setDefaultConfig();                              // what firmware 1.3.2 left in flash: a raw copy at 0xFD0000
    cfg.sampleIntervalMs = 777; cfg.gasR0 = 4.5f;
    cfg.crc = crc32Calc((uint8_t *)&cfg, sizeof(cfg) - sizeof(uint32_t));
    ezhal_flash_write_sector(CONFIG_FLASH_ADDR, &cfg, sizeof(cfg));
    reboot_and_load();
    CHECK_EQ(cfg.sampleIntervalMs, 777u);            // calibration kept across the upgrade
    CHECK_EQ(cfg.gasR0, 4.5f);
    NodeConfig b;
    CHECK_EQ(ezhal_frec_read(EZHAL_FLASH_CFG_A, EZHAL_FLASH_CFG_B, &b, sizeof(b), NULL), HS_OK);   // now in slot B
    CHECK_EQ(b.sampleIntervalMs, 777u);
    reboot_and_load();
    CHECK_EQ(cfg.gasR0, 4.5f);
}

TEST(power_cut_during_save_keeps_a_valid_config, "FW-08,FW-20")
{
    for (long budget = 0; budget < 16 + (long)sizeof(NodeConfig) + 20; budget += 7) {
        fresh();
        loadConfig();
        cfg.sampleIntervalMs = 111; saveConfig();
        cfg.sampleIntervalMs = 222;
        fakehal.flash_budget = budget;
        try { saveConfig(); } catch (PowerLoss &) {}
        fakehal.flash_budget = -1;
        reboot_and_load();
        CHECK(cfg.sampleIntervalMs == 111 || cfg.sampleIntervalMs == 222);
    }
}

TEST(older_config_version_replaced, "FW-21")
{
    fresh();
    loadConfig();
    cfg.version = CONFIG_VERSION - 1;
    cfg.sampleIntervalMs = 999;
    cfg.crc = crc32Calc((uint8_t *)&cfg, sizeof(cfg) - sizeof(uint32_t));
    ezhal_flash_write_sector(CONFIG_FLASH_ADDR, &cfg, sizeof(cfg));
    reboot_and_load();
    CHECK_EQ(cfg.version, (uint16_t)CONFIG_VERSION);
    CHECK_EQ(cfg.sampleIntervalMs, 50u);
}

TEST(save_reports_flash_verify_failure, "FW-08")
{
    fresh();
    loadConfig();
    fakehal.flash_verify_fail = true;
    CHECK(!saveConfig());
}

TEST(config_keys_match_design_document, "FW-21")
{
    // design doc section 7.3
    const char *documented[] = {"name", "ssid", "pass", "server", "port", "sock_base", "token", "ntp", "secure_all", "power",
                                "maint", "sample_ms", "upload_ms", "hb_s", "net_reboot_min", "gas_r0", "gas_rl", "gas_vc",
                                "gas_clean_ratio", "gas_a", "gas_b", "gas_kt", "gas_kh", "gas_alarm_ppm", "gas_clear_ppm",
                                "gas_confirm_ppm", "gas_alarm_raw", "gas_warmup_s", "ac_th_db", "ac_clear_db", "ac_windows",
                                "fusion_s", "event_hold_s", "clip_s", "thermal_c", "itemp_scale", "itemp_offset"};
    std::set<std::string> keys;
    for (int i = 0; i < cfgTableLen; i++) keys.insert(cfgTable[i].key);
    CHECK_EQ((int)keys.size(), cfgTableLen);                  // no duplicates
    CHECK_EQ(cfgTableLen, (int)(sizeof(documented) / sizeof(documented[0])));
    for (auto k : documented) {
        if (!keys.count(k)) minitest::fail(__FILE__, __LINE__, std::string("documented key missing: ") + k);
    }
}

TEST(set_and_read_every_type, "FW-21")
{
    fresh();
    setDefaultConfig();
    CHECK(setCfgItem("sample_ms", "250"));     CHECK_EQ(cfg.sampleIntervalMs, 250u);
    CHECK(setCfgItem("POWER", "1"));           CHECK_EQ(cfg.powerMode, (uint8_t)1);       // keys are case-insensitive
    CHECK(setCfgItem("gas_r0", "3.25"));       CHECK_EQ(cfg.gasR0, 3.25f);
    CHECK(setCfgItem("server", "10.1.2.3"));   CHECK_EQ(String(cfg.serverAddr), String("10.1.2.3"));
    CHECK_EQ(cfgItemValue(findCfgItem("sample_ms"), false), String("250"));
    CHECK_EQ(cfgItemValue(findCfgItem("gas_r0"), false), String("3.2500"));
}

TEST(rejects_unknown_key_and_overlong_string, "FW-21")
{
    fresh();
    setDefaultConfig();
    CHECK(!setCfgItem("no_such_key", "1"));
    std::string longname(40, 'x');                              // deviceName holds 31 characters
    CHECK(!setCfgItem("name", longname.c_str()));
    CHECK_EQ(String(cfg.deviceName), String(DEVICE_NAME));     // unchanged
    CHECK(setCfgItem("name", std::string(31, 'y').c_str()));
}

TEST(secrets_are_masked, "FW-16")
{
    fresh();
    setDefaultConfig();
    strcpy(cfg.pass, "wifi-secret");
    strcpy(cfg.apiToken, "token-secret");
    String text = configDump(false), json = configDump(true);
    CHECK(text.indexOf("wifi-secret") < 0);
    CHECK(text.indexOf("token-secret") < 0);
    CHECK(json.indexOf("wifi-secret") < 0);
    CHECK_CONTAINS(text, "pass=****");
    CHECK_CONTAINS(json, "\"token\":\"****\"");
    CHECK(json.startsWith("{") && json.endsWith("}"));
}

TEST(boot_counter_increments, "FW-18")
{
    fresh();
    loadBootInfo(); CHECK_EQ(bootInfo.bootCount, 1u);
    loadBootInfo(); CHECK_EQ(bootInfo.bootCount, 2u);
    reboot_and_load(); CHECK_EQ(bootInfo.bootCount, 3u);
}

TEST(reset_reason_survives_reboot_once, "FW-18,FW-20")
{
    fresh();
    loadBootInfo();
    recordResetReason(RR_TASK_STALL, "up");
    reboot_and_load();
    CHECK_EQ(lastResetReason, (uint32_t)RR_TASK_STALL);
    CHECK_EQ(lastResetDetail, String("up"));
    CHECK_EQ(resetReasonName(lastResetReason), String("task_stall"));
    reboot_and_load();                                          // nothing recorded before this reset
    CHECK_EQ(lastResetReason, (uint32_t)RR_UNEXPECTED);
    CHECK_EQ(resetReasonName(lastResetReason), String("unexpected(power/wdt/brownout)"));
}

TEST(reboot_with_reason_records_then_resets, "FW-18,FW-20")
{
    fresh();
    loadBootInfo();
    rebootWithReason(RR_CMD_REBOOT, "cmd");
    CHECK(fakehal.reset_called);
    reboot_and_load();
    CHECK_EQ(resetReasonName(lastResetReason), String("cmd_reboot"));
    CHECK_EQ(lastResetDetail, String("cmd"));
}

TEST(long_reset_detail_is_truncated_safely, "FW-18")
{
    fresh();
    loadBootInfo();
    recordResetReason(RR_SOCKET_FAIL, std::string(100, 'z').c_str());
    reboot_and_load();
    CHECK_EQ((int)lastResetDetail.length(), (int)sizeof(bootInfo.pendingDetail) - 1);
}

TEST(every_reset_reason_has_a_name, "FW-18")
{
    for (uint32_t r = RR_CMD_REBOOT; r <= RR_CONFIG; r++) {
        CHECK(resetReasonName(r) != String("unexpected(power/wdt/brownout)"));
    }
    CHECK_EQ(resetReasonName(999), String("unexpected(power/wdt/brownout)"));
}

TEST(corrupted_boot_info_restarts_counting, "FW-18,FW-20")
{
    fresh();
    loadBootInfo(); loadBootInfo();
    fakehal.flash[BOOTINFO_FLASH_ADDR][5] ^= 0xFF;
    loadBootInfo();
    CHECK_EQ(bootInfo.bootCount, 1u);
}

MINITEST_MAIN
