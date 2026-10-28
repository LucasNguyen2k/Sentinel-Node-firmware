// SD-card OTA guards: pre-checks, outcome after reboot, timeout (FW-17, FW-08, FW-18)
// @extract firmware/sentinel_node/sentinel_node.ino: FW_VERSION OTA_FILE OTA_SLOT_BYTES OTA_MIN_BYTES OTA_TIMEOUT_MS ResetReasonCode EventState sdMutex sdOk eventState lastResetReason lastResetDetail otaInProgress otaStartMs otaResult lockM unlockM sdUsable eventStateName otaPrecheck otaCheckOutcome otaWatch
// @variant normal:
// @variant otamode: -DArduino_OTA_MODE
#include "minitest.h"
#include "fw_env.h"
uint32_t g_pending = 0; std::string g_pending_detail;
void recordResetReason(uint32_t r, String d) { g_pending = r; g_pending_detail = d.s; }
void LOG(String m) { Serial.println(m); }
#include "extracted.inc"

static void fresh()
{
    fake_reset_all();
    sdOk = true; eventState = EVT_NORMAL; otaInProgress = false; otaResult = "none";
    lastResetReason = RR_UNEXPECTED; lastResetDetail = "";
    g_pending = 0; g_pending_detail.clear();
}
static void card_with(uint32_t size) { fakehal.files["ota.bin"] = std::string(size, 'F'); }

#ifndef Arduino_OTA_MODE
TEST(normal_build_refuses_every_update, "FW-17")
{
    fresh(); card_with(3297284);
    int32_t sz;
    CHECK_CONTAINS(otaPrecheck(false, &sz), "built without OTA Mode");
    CHECK_CONTAINS(otaPrecheck(true, &sz), "built without OTA Mode");
}
#else
TEST(accepts_valid_image, "FW-17")
{
    fresh(); card_with(3297284);
    int32_t sz;
    CHECK_EQ(otaPrecheck(false, &sz), String(""));
    CHECK_EQ(sz, 3297284);
}

TEST(refusal_matrix, "FW-17")
{
    int32_t sz;
    fresh(); sdOk = false; card_with(3297284);
    CHECK_EQ(otaPrecheck(false, &sz), String("ERR SD not available"));
    fresh();
    CHECK_EQ(otaPrecheck(false, &sz), String("ERR ota.bin not found on SD"));
    fresh(); card_with(OTA_SLOT_BYTES + 1);
    CHECK_CONTAINS(otaPrecheck(false, &sz), "larger than the 4 MB firmware slot");
    fresh(); card_with(OTA_SLOT_BYTES);
    CHECK_EQ(otaPrecheck(false, &sz), String(""));                         // exactly 4 MB fits
    fresh(); card_with(OTA_MIN_BYTES - 1);
    CHECK_CONTAINS(otaPrecheck(false, &sz), "not a firmware image");
    fresh(); card_with(3297284); otaInProgress = true;
    CHECK_EQ(otaPrecheck(false, &sz), String("ERR OTA already in progress"));
}

TEST(leak_event_blocks_unless_forced, "FW-17,FW-06")
{
    int32_t sz;
    fresh(); card_with(3297284); eventState = EVT_SUSPECT;
    CHECK_CONTAINS(otaPrecheck(false, &sz), "leak event SUSPECT active");
    CHECK_EQ(otaPrecheck(true, &sz), String(""));
    eventState = EVT_CONFIRMED;
    CHECK_CONTAINS(otaPrecheck(false, &sz), "CONFIRMED");
}

TEST(timeout_marks_failure_and_restores_sd, "FW-17,FW-20")
{
    fresh();
    otaInProgress = true; otaStartMs = millis(); otaResult = "running";
    g_fake_millis += OTA_TIMEOUT_MS - 1; otaWatch();
    CHECK(otaInProgress);
    g_fake_millis += 2; otaWatch();
    CHECK(!otaInProgress);
    CHECK_CONTAINS(otaResult, "failed: no reboot within 10 min");
    CHECK_EQ(g_pending, (uint32_t)RR_UNEXPECTED);                          // pending "ota" reason cleared
    CHECK(sdUsable());
}
#endif

TEST(outcome_new_version_is_success_and_renames_image, "FW-17")
{
    fresh(); card_with(3297284);
    lastResetReason = RR_OTA; lastResetDetail = "sd:1.3.0";
    otaCheckOutcome();
    CHECK_CONTAINS(otaResult, std::string("ok: 1.3.0 -> ") + FW_VERSION);
    CHECK(!fakehal.files.count("ota.bin"));
    CHECK(fakehal.files.count(std::string("ota_") + FW_VERSION + ".bin"));
}

TEST(outcome_replaces_previous_renamed_image, "FW-17")
{
    fresh(); card_with(3297284);
    std::string done = std::string("ota_") + FW_VERSION + ".bin";
    fakehal.files[done] = "old";
    lastResetReason = RR_OTA; lastResetDetail = "sd:1.0.0";
    otaCheckOutcome();
    CHECK_EQ(fakehal.files[done].size(), (size_t)3297284);
}

TEST(outcome_same_version_is_unchanged_and_keeps_image, "FW-17")
{
    fresh(); card_with(3297284);
    lastResetReason = RR_OTA; lastResetDetail = std::string("sd:") + FW_VERSION;
    otaCheckOutcome();
    CHECK_CONTAINS(otaResult, "unchanged");
    CHECK(fakehal.files.count("ota.bin"));
}

TEST(outcome_handles_1_3_0_detail_format, "FW-17")
{
    fresh();
    lastResetReason = RR_OTA; lastResetDetail = "sd";                     // written by firmware 1.3.0
    otaCheckOutcome();
    CHECK_CONTAINS(otaResult, "unchanged");
}

TEST(outcome_ignores_other_resets, "FW-17")
{
    fresh(); lastResetReason = RR_CMD_REBOOT; otaCheckOutcome();
    CHECK_EQ(otaResult, String("none"));
}

TEST(outcome_without_card_still_reported, "FW-17")
{
    fresh(); sdOk = false;
    lastResetReason = RR_OTA; lastResetDetail = "sd:1.2.0";
    otaCheckOutcome();
    CHECK_CONTAINS(otaResult, "ok: 1.2.0");
    CHECK(otaResult.indexOf("renamed") < 0);
}

MINITEST_MAIN
