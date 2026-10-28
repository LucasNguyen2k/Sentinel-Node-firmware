// Gas sensor processing: filtering, Rs/R0 conversion, compensation, warm-up, alarms (FW-03, FW-04, FW-21)
// @extract firmware/sentinel_node/sentinel_node.ino: GAS_AVG_N DEVICE_NAME SERVER_ADDRESS SERVER_PORT SOCKET_BASE_PORT API_TOKEN_DEFAULT CONFIG_MAGIC CONFIG_VERSION QCS6490 NodeConfig ssid pass cfg gasFiltered gasHist gasHistIdx gasHistCnt gasRs gasPpm gasLel gasAlarm gasOk bootMs setDefaultConfig processGas
#include "minitest.h"
#include "fw_env.h"
#include "extracted.inc"

static void fresh()
{
    fake_reset_all();
    setDefaultConfig();
    for (int i = 0; i < GAS_AVG_N; i++) gasHist[i] = 0;
    gasHistIdx = gasHistCnt = 0;
    gasFiltered = 0; gasRs = gasPpm = gasLel = -1; gasAlarm = false; gasOk = false;
    bootMs = millis();
}

static void warm() { g_fake_millis = bootMs + (cfg.gasWarmupS + 1) * 1000UL; }

// Feed the same raw value until the moving average is full
static void feed(int raw, int n = GAS_AVG_N, float t = 20, float rh = 65, bool env = true)
{
    for (int i = 0; i < n; i++) processGas(raw, t, rh, env);
}

// Reference implementation of the documented formula (design doc 4.2)
static double rs_of(double raw) { double v = raw * 3.3 / 1023.0; return cfg.gasRL * (cfg.gasVc - v) / v; }
static double ppm_of(double raw, double r0, double k = 1.0) { return cfg.gasCurveA * pow(rs_of(raw) / r0 / k, cfg.gasCurveB); }
// R0 that makes `raw` read `ppm`
static double r0_for(double raw, double ppm) { return rs_of(raw) / pow(ppm / cfg.gasCurveA, 1.0 / cfg.gasCurveB); }

TEST(defaults_match_design, "FW-21")
{
    fresh();
    CHECK_EQ(cfg.gasR0, 0.0f);
    CHECK_EQ(cfg.gasWarmupS, 180u);
    CHECK_EQ(cfg.gasAlarmPpm, 1000.0f);
    CHECK_EQ(cfg.gasClearPpm, 700.0f);
    CHECK_EQ(cfg.gasConfirmPpm, 5000.0f);
    CHECK_EQ(cfg.gasAlarmRaw, 0u);
    CHECK_EQ(cfg.sampleIntervalMs, 50u);
    CHECK_EQ(cfg.magic, (uint32_t)CONFIG_MAGIC);
}

TEST(uncalibrated_reports_no_ppm_and_no_alarm, "FW-04")
{
    fresh(); warm();
    feed(900);
    CHECK_EQ(gasPpm, -1.0f);
    CHECK_EQ(gasLel, -1.0f);
    CHECK(!gasAlarm);
    CHECK(gasOk);
}

TEST(ppm_and_lel_follow_documented_curve, "FW-04")
{
    fresh(); warm();
    cfg.gasR0 = 12.0f;
    feed(500);
    CHECK_NEAR(gasFiltered, 500, 1e-3);
    CHECK_NEAR(gasRs, rs_of(500), 1e-3);
    CHECK_NEAR(gasPpm, ppm_of(500, 12.0), ppm_of(500, 12.0) * 1e-4);
    CHECK_NEAR(gasLel, gasPpm / 500.0, 1e-3);   // methane LEL = 50,000 ppm
}

TEST(alarm_has_hysteresis, "FW-04")
{
    fresh(); warm();
    cfg.gasR0 = (float)r0_for(500, 1500);          // 500 raw -> 1500 ppm
    feed(500);
    CHECK(gasAlarm);
    // drift down to 800 ppm: between clear (700) and alarm (1000) -> stays latched
    cfg.gasR0 = (float)r0_for(500, 800);
    feed(500);
    CHECK_NEAR(gasPpm, 800, 2);
    CHECK(gasAlarm);
    cfg.gasR0 = (float)r0_for(500, 650);           // below clear level
    feed(500);
    CHECK(!gasAlarm);
    cfg.gasR0 = (float)r0_for(500, 900);           // rising again, still below alarm level
    feed(500);
    CHECK(!gasAlarm);
}

TEST(no_alarm_during_warm_up, "FW-04")
{
    fresh();
    cfg.gasR0 = (float)r0_for(500, 5000);
    g_fake_millis = bootMs + (cfg.gasWarmupS - 1) * 1000UL;
    feed(500);
    CHECK(gasPpm > 4000);
    CHECK(!gasAlarm);
    warm();
    feed(500, 1);
    CHECK(gasAlarm);
}

TEST(rail_values_mark_sensor_faulty_and_suppress_alarm, "FW-04,FW-13")
{
    fresh(); warm();
    cfg.gasR0 = (float)r0_for(500, 5000);
    feed(0);
    CHECK(!gasOk);
    CHECK(!gasAlarm);
    feed(1023);
    CHECK(!gasOk);
    CHECK(!gasAlarm);
    feed(4); CHECK(gasOk);
    feed(1019); CHECK(gasOk);
}

TEST(moving_average_over_ten_samples, "FW-04")
{
    fresh(); warm();
    feed(100);
    processGas(1100, 20, 65, true);                 // one spike among ten samples
    CHECK_NEAR(gasFiltered, (9 * 100 + 1100) / 10.0, 1e-3);
    processGas(100, 20, 65, true);
    CHECK_NEAR(gasFiltered, (9 * 100 + 1100) / 10.0, 1e-3);   // spike still inside the window
    feed(100, 9);
    CHECK_NEAR(gasFiltered, 100, 1e-3);                        // spike has left the window
}

TEST(temperature_humidity_compensation, "FW-04")
{
    fresh(); warm();
    cfg.gasR0 = 12.0f;
    cfg.gasTempCoef = 0.02f;                        // +2 % per degC above 20
    cfg.gasHumCoef = 0.0f;
    feed(500, GAS_AVG_N, 30, 65, true);             // k = 1 + 0.02 * 10 = 1.2
    CHECK_NEAR(gasPpm, ppm_of(500, 12.0, 1.2), ppm_of(500, 12.0, 1.2) * 1e-4);
    feed(500, GAS_AVG_N, 30, 65, false);            // AHT20 invalid -> no compensation
    CHECK_NEAR(gasPpm, ppm_of(500, 12.0, 1.0), ppm_of(500, 12.0) * 1e-4);
    cfg.gasTempCoef = 0; cfg.gasHumCoef = 0.01f;
    feed(500, GAS_AVG_N, 20, 85, true);             // k = 1 + 0.01 * 20 = 1.2
    CHECK_NEAR(gasPpm, ppm_of(500, 12.0, 1.2), ppm_of(500, 12.0, 1.2) * 1e-4);
}

TEST(raw_fallback_alarm_while_uncalibrated, "FW-04")
{
    fresh(); warm();
    cfg.gasAlarmRaw = 600;
    feed(599); CHECK(!gasAlarm);
    feed(600); CHECK(gasAlarm);
    feed(560); CHECK(gasAlarm);                     // above 0.9 * 600 = 540: latched
    feed(530); CHECK(!gasAlarm);
}

MINITEST_MAIN
