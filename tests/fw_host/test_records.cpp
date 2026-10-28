// Sensor acquisition record format, validity flags, time keeping, encoding helpers (FW-03, FW-13, FW-15, FW-10)
// @extract firmware/sentinel_node/sentinel_node.ino: ACOUSTIC_DETECTION EXTENDED_SENSOR_DATA GAS_AVG_N DEVICE_NAME SERVER_ADDRESS SERVER_PORT SOCKET_BASE_PORT API_TOKEN_DEFAULT CONFIG_MAGIC CONFIG_VERSION QCS6490 NodeConfig EventState ssid pass cfg epochAtSync millisAtSync timeSynced lastTemp ahtOk lastChemRaw gasFiltered gasHist gasHistIdx gasHistCnt gasRs gasAlarm gasOk bootMs recordSeq lastUs usAlarm acAccSum acAccN acFrames acLastFrameMs acPrevSample acDb acFloorDb acFloorInit acAboveCnt acAlarm eventState simGasUntil thermalDerate setDefaultConfig setClock getEpoch processGas micOk validMask processAcoustic getSensorData@getSensorData() fnum urlEncode
#include "minitest.h"
#include "fw_env.h"
#include "extracted.inc"
#include <sstream>

static std::vector<std::string> fields(const String &rec)
{
    std::vector<std::string> out;
    std::stringstream ss(rec.s);
    std::string f;
    while (std::getline(ss, f, ',')) out.push_back(f);
    return out;
}

static void fresh()
{
    fake_reset_all();
    setDefaultConfig();
    timeSynced = false; epochAtSync = 0; millisAtSync = 0;
    recordSeq = 0; eventState = EVT_NORMAL; thermalDerate = false; simGasUntil = 0;
    for (int i = 0; i < GAS_AVG_N; i++) gasHist[i] = 0;
    gasHistIdx = gasHistCnt = 0; gasAlarm = false; acAlarm = false; acFrames = 0; acAccN = 0;
    bootMs = millis();
}

TEST(record_has_eleven_fields_in_documented_order, "FW-03,FW-10")
{
    fresh();
    fakehal.env_t = 21.5f; fakehal.env_rh = 55.0f; fakehal.gas_raw = 412;
    setClock(1791500000);
    String r = getSensorData();
    auto f = fields(r);
    CHECK_EQ(f.size(), (size_t)11);
    if (f.size() != 11) return;
    CHECK_EQ(f[0], std::string("21.50"));          // temp_c
    CHECK_EQ(f[1], std::string("55.00"));          // rh_pct
    CHECK_EQ(f[2], std::string("412"));            // gas_raw
    CHECK_EQ(f[3], std::string("1791500000"));     // device_ts
    CHECK_EQ(f[4], std::string("1"));              // device_seq
    CHECK_EQ(f[5], std::string("-1.0"));           // gas_ppm (uncalibrated)
    CHECK_EQ(f[8], std::string("-1"));             // us (not fitted)
    CHECK_EQ(f[9], std::string("0"));              // event_state NORMAL
}

TEST(no_field_has_padding, "FW-03")
{
    // regression test for the SDK String(float) padding fixed in 1.1.6 (gas_raw=%200)
    fresh();
    fakehal.gas_raw = 5;
    for (auto &f : fields(getSensorData())) CHECK(!f.empty() && f[0] != ' ');
    CHECK_EQ(String(0.0f, 0), String(" 0"));       // the SDK behaviour being worked around
    CHECK_EQ(fnum(0.0f, 0), String("0"));
    CHECK_EQ(fnum(-1.0f, 1), String("-1.0"));
}

TEST(sequence_increments_per_record, "FW-03,FW-15")
{
    fresh();
    for (int i = 1; i <= 5; i++) CHECK_EQ(fields(getSensorData())[4], std::to_string(i));
}

TEST(failed_aht20_reports_invalid_marker, "FW-03,FW-13")
{
    fresh();
    fakehal.env_status = HS_ERROR;
    auto f = fields(getSensorData());
    CHECK_EQ(f[0], std::string("-999.00"));
    CHECK_EQ(f[1], std::string("-999.00"));
    CHECK(!ahtOk);
    CHECK_EQ(std::stoi(f[10]) & 1, 0);             // valid bit 0 (aht_ok) clear
    fakehal.env_status = HS_OK;
    f = fields(getSensorData());
    CHECK_EQ(std::stoi(f[10]) & 1, 1);
}

TEST(validity_bits_follow_sensor_state, "FW-13")
{
    fresh();
    int v = std::stoi(fields(getSensorData())[10]);
    CHECK(v & (1 << 1));                           // gas_ok (raw 400)
    CHECK(!(v & (1 << 2)));                        // not warm yet
    CHECK(!(v & (1 << 3)));                        // not calibrated
    CHECK(!(v & (1 << 4)));                        // no audio frames yet
    CHECK(!(v & (1 << 5)));                        // no ultrasonic sensor
    CHECK(!(v & (1 << 6)));                        // clock not set
    g_fake_millis = bootMs + (cfg.gasWarmupS + 1) * 1000UL;
    cfg.gasR0 = 10; cfg.maintenance = 1; setClock(1791500000); simGasUntil = millis() + 1000;
    fakehal.us_present = true; fakehal.us_level = 12;
    v = std::stoi(fields(getSensorData())[10]);
    for (int b : {1, 2, 3, 5, 6, 7, 8}) CHECK(v & (1 << b));
}

TEST(ultrasonic_value_reported_when_fitted, "FW-02,FW-03")
{
    fresh();
    fakehal.us_present = true; fakehal.us_level = 123;
    CHECK_EQ(fields(getSensorData())[8], std::string("123"));
}

TEST(thermal_derate_at_limit, "FW-13")
{
    fresh();
    fakehal.env_t = 69.9f; getSensorData(); CHECK(!thermalDerate);
    fakehal.env_t = 70.0f; getSensorData(); CHECK(thermalDerate);
    fakehal.env_status = HS_ERROR; getSensorData(); CHECK(!thermalDerate);   // unknown temperature: no derate
}

TEST(clock_unsynced_reports_zero, "FW-15")
{
    fresh();
    CHECK_EQ(getEpoch(), 0u);
    CHECK_EQ(fields(getSensorData())[3], std::string("0"));
}

TEST(clock_rejects_implausible_time, "FW-15")
{
    fresh();
    setClock(1599999999);
    CHECK(!timeSynced);
    setClock(0);
    CHECK(!timeSynced);
    setClock(1600000000);
    CHECK(timeSynced);
}

TEST(clock_advances_with_millis, "FW-15")
{
    fresh();
    setClock(1791500000);
    g_fake_millis += 2999;
    CHECK_EQ(getEpoch(), 1791500002u);
    g_fake_millis += 1;
    CHECK_EQ(getEpoch(), 1791500003u);
}

TEST(clock_survives_millis_rollover, "FW-15")
{
    fresh();
    g_fake_millis = 0xFFFFF000u;                   // 4.1 s before the 49.7-day wrap
    setClock(1791500000);
    g_fake_millis += 10000;                        // wraps through zero
    CHECK_EQ(getEpoch(), 1791500010u);
}

TEST(url_encoding_of_query_values, "FW-10")
{
    CHECK_EQ(urlEncode("abc-_.~123"), String("abc-_.~123"));
    CHECK_EQ(urlEncode("a b&c=d"), String("a%20b%26c%3Dd"));
    CHECK_EQ(urlEncode("unexpected(power/wdt)"), String("unexpected%28power%2Fwdt%29"));
}

MINITEST_MAIN
