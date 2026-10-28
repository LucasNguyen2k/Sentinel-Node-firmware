// Event fusion state machine, event messages and capture triggers (FW-05, FW-06, FW-07, FW-15)
// @extract firmware/sentinel_node/sentinel_node.ino: ACOUSTIC_DETECTION DEVICE_NAME SERVER_ADDRESS SERVER_PORT SOCKET_BASE_PORT API_TOKEN_DEFAULT CONFIG_MAGIC CONFIG_VERSION QCS6490 NodeConfig BootInfo EventState ssid pass cfg bootInfo deviceId epochAtSync millisAtSync timeSynced ahtOk gasFiltered gasPpm gasAlarm gasOk bootMs usAlarm acFrames acLastFrameMs acHfDb acFloorDb acAlarm eventState eventId eventCounter lastGasAlarmMs everGas simGasUntil thermalDerate bCapturePicture bCaptureClip captureEventId setDefaultConfig getEpoch fnum urlEncode micOk validMask eventStateName onEventChange eventStep
#include "minitest.h"
#include "fw_env.h"

// ---- collaborators of the extracted code (outside the unit under test)
std::vector<std::string> g_submits;
int submitData(String path) { g_submits.push_back(path.s); return 200; }
std::vector<std::string> g_sdlines;
bool storeEventLine(String line) { g_sdlines.push_back("ezsn/events.csv|" + line.s); return true; }
void LOG(String m) { Serial.println(m); }

std::vector<std::string> g_clip_req;
void clipRequestIfOffline(const String &e) { g_clip_req.push_back(e.s); }
#include "extracted.inc"

static uint32_t now;
static void fresh()
{
    fake_reset_all();
    setDefaultConfig();
    bootInfo.bootCount = 7;
    deviceId = "00E04C0F5C9D";
    epochAtSync = 1791500000; millisAtSync = 1000; timeSynced = true;
    gasAlarm = acAlarm = usAlarm = false; gasPpm = -1; gasFiltered = 300;
    simGasUntil = simAcUntil = 0;
    lastGasAlarmMs = lastAcAlarmMs = lastUsAlarmMs = lastAnyAlarmMs = 0;
    everGas = everAc = everUs = false;
    eventState = EVT_NORMAL; eventId = ""; eventCounter = 0;
    thermalDerate = false; bCapturePicture = false; captureEventId = "";
    g_submits.clear(); g_sdlines.clear();
    now = 10000; g_fake_millis = now;
}

// run the 200 ms event loop for `ms`
static void run(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 200) {
        now += 200;
        g_fake_millis = now;
        eventStep(now);
    }
}

static std::string last_submit() { return g_submits.empty() ? "" : g_submits.back(); }
static bool has(const std::string &h, const std::string &n) { return h.find(n) != std::string::npos; }

TEST(single_gas_alarm_raises_suspect, "FW-05,FW-06")
{
    fresh();
    gasAlarm = true;
    run(200);
    CHECK_EQ((int)eventState, (int)EVT_SUSPECT);
    CHECK_EQ(eventId, String("E7_1"));
    CHECK_EQ(g_submits.size(), (size_t)1);
    CHECK(has(last_submit(), "/node-event?"));
    CHECK(has(last_submit(), "&event=E7_1&state=SUSPECT&prev=NORMAL"));
    CHECK(has(last_submit(), "&g=1&a=0&u=0"));
    CHECK(has(last_submit(), "&id=00E04C0F5C9D"));
}

TEST(escalation_requests_event_picture, "FW-07")
{
    fresh();
    gasAlarm = true;
    run(200);
    CHECK(bCapturePicture);
    CHECK_EQ(captureEventId, String("E7_1"));
}

TEST(escalations_request_sound_clip, "FW-07,FW-08")
{
    fresh();
    g_clip_req.clear();
    gasAlarm = true;
    run(200);                                               // NORMAL -> SUSPECT
    acAlarm = true;
    run(200);                                               // SUSPECT -> CONFIRMED
    CHECK_EQ((int)eventState, (int)EVT_CONFIRMED);
    gasAlarm = acAlarm = false;
    run(cfg.eventHoldS * 1000 + 1000);                      // back to NORMAL: no clip
    CHECK_EQ((int)eventState, (int)EVT_NORMAL);
    CHECK_EQ(g_clip_req.size(), (size_t)2);
    if (g_clip_req.size() == 2) CHECK(g_clip_req[0] == "E7_1" && g_clip_req[1] == "E7_1");
    g_clip_req.clear();
    cfg.maintenance = 1;
    gasAlarm = true;
    run(200);
    CHECK(g_clip_req.empty());                              // maintenance: no clip, like no picture
}

TEST(gas_and_acoustic_within_window_confirm, "FW-05,FW-06")
{
    fresh();
    gasAlarm = true;
    run(1000);
    acAlarm = true;
    run(200);
    CHECK_EQ((int)eventState, (int)EVT_CONFIRMED);
    CHECK_EQ(eventId, String("E7_1"));                      // same event, escalated
    CHECK(has(last_submit(), "state=CONFIRMED&prev=SUSPECT"));
    CHECK(has(last_submit(), "&g=1&a=1"));
}

TEST(second_sensor_outside_fusion_window_does_not_confirm, "FW-05")
{
    fresh();
    cfg.fusionWindowS = 10;
    gasAlarm = true;
    run(1000);
    gasAlarm = false;
    run(15000);                                             // 15 s quiet: gas older than the 10 s window
    acAlarm = true;
    run(200);
    CHECK_EQ((int)eventState, (int)EVT_SUSPECT);            // still within the 30 s hold, not confirmed
}

TEST(high_gas_alone_confirms_directly, "FW-05,FW-06")
{
    fresh();
    gasPpm = 6000;                                          // >= gas_confirm_ppm (5000)
    gasAlarm = true;
    run(200);
    CHECK_EQ((int)eventState, (int)EVT_CONFIRMED);
    CHECK_EQ(g_submits.size(), (size_t)1);
    CHECK(has(last_submit(), "state=CONFIRMED&prev=NORMAL"));
}

TEST(confirmed_is_latched_until_quiet, "FW-06")
{
    fresh();
    gasAlarm = acAlarm = true;
    run(200);
    CHECK_EQ((int)eventState, (int)EVT_CONFIRMED);
    acAlarm = false;
    run(120000);                                            // gas still alarming for 2 minutes
    CHECK_EQ((int)eventState, (int)EVT_CONFIRMED);          // never drops back to SUSPECT
}

TEST(returns_to_normal_after_hold_time, "FW-06")
{
    fresh();
    gasAlarm = true;
    run(200);
    gasAlarm = false;
    size_t n = g_submits.size();
    bCapturePicture = false;
    run(cfg.eventHoldS * 1000 - 400);
    CHECK_EQ((int)eventState, (int)EVT_SUSPECT);
    run(800);
    CHECK_EQ((int)eventState, (int)EVT_NORMAL);
    CHECK_EQ(g_submits.size(), n + 1);
    CHECK(has(last_submit(), "state=NORMAL&prev=SUSPECT"));
    CHECK(!bCapturePicture);                                // de-escalation takes no picture
}

TEST(new_event_gets_new_id, "FW-06")
{
    fresh();
    gasAlarm = true; run(200);
    gasAlarm = false; run(cfg.eventHoldS * 1000 + 1000);
    acAlarm = true; run(200);
    CHECK_EQ(eventId, String("E7_2"));
}

TEST(maintenance_mode_flags_events_and_takes_no_picture, "FW-06,FW-19")
{
    fresh();
    cfg.maintenance = 1;
    gasAlarm = true;
    run(200);
    CHECK_EQ((int)eventState, (int)EVT_SUSPECT);
    CHECK(has(last_submit(), "&maint=1"));
    CHECK(!bCapturePicture);
}

TEST(thermal_derate_suppresses_picture, "FW-07,FW-13")
{
    fresh();
    thermalDerate = true;
    gasAlarm = true;
    run(200);
    CHECK(!bCapturePicture);
}

TEST(simulated_gas_reported_as_gas, "FW-06,FW-22")
{
    // regression test for the 1.1.9 fix: SIM GAS events used to report g=0
    fresh();
    simGasUntil = now + 5000;
    run(200);
    CHECK_EQ((int)eventState, (int)EVT_SUSPECT);
    CHECK(has(last_submit(), "&g=1&a=0"));
}

TEST(simulation_expires, "FW-22")
{
    fresh();
    simGasUntil = now + 2000;
    run(200);
    run(2000 + cfg.eventHoldS * 1000 + 400);
    CHECK_EQ((int)eventState, (int)EVT_NORMAL);
}

TEST(event_log_line_and_timestamp, "FW-06,FW-08,FW-15")
{
    fresh();
    gasAlarm = true;
    run(200);
    CHECK_EQ(g_sdlines.size(), (size_t)1);
    std::string line = g_sdlines.back();
    CHECK(line.rfind("ezsn/events.csv|", 0) == 0);
    std::string csv = line.substr(line.find('|') + 1);
    CHECK_EQ((int)std::count(csv.begin(), csv.end(), ','), 8);       // 9 columns
    uint32_t expect = epochAtSync + (now - millisAtSync) / 1000;
    CHECK(csv.rfind(std::to_string(expect) + ",E7_1,SUSPECT,", 0) == 0);
    CHECK(has(last_submit(), "&ts=" + std::to_string(expect)));
}

TEST(no_event_without_alarm, "FW-05")
{
    fresh();
    run(60000);
    CHECK_EQ((int)eventState, (int)EVT_NORMAL);
    CHECK(g_submits.empty());
}

MINITEST_MAIN
