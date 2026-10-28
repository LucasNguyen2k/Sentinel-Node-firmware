// AHT20 frame validation: CRC-8, missing sensor, busy, range (FW-03, FW-13, FW-02)
// @extract firmware/sentinel_node/ezsn_hal.cpp: ezhal_aht_crc8 ezhal_aht_decode g_envPresent g_ahtTrigMs g_ahtPending g_ahtLast g_ahtValidMs g_ahtT g_ahtH EZHAL_AHT_PROBE_MS g_ahtAbsent g_ahtFails g_ahtProbeMs ezhal_env_service ezhal_env_read
// @cflags -DEZ_REAL_ENV
#include "minitest.h"
#include "fw_env.h"
// fake bus: counts every I2C transaction the driver makes
static bool g_ack = false;            // sensor fitted and answering
static int g_triggers = 0, g_reads = 0;
static uint8_t g_frame[7];
bool ezhal_aht_trigger() { g_triggers++; return g_ack; }
void ezhal_aht_read_frame(uint8_t f[7]) { g_reads++; for (int i = 0; i < 7; i++) f[i] = g_ack ? g_frame[i] : 0; }
#include "extracted.inc"

// build a valid frame for given raw values (status 0x1C = calibrated, idle)
static void frame_for(uint32_t hraw, uint32_t traw, uint8_t status, uint8_t f[7])
{
    f[0] = status;
    f[1] = (uint8_t)(hraw >> 12);
    f[2] = (uint8_t)(hraw >> 4);
    f[3] = (uint8_t)(((hraw & 0x0F) << 4) | ((traw >> 16) & 0x0F));
    f[4] = (uint8_t)(traw >> 8);
    f[5] = (uint8_t)traw;
    f[6] = ezhal_aht_crc8(f, 6);
}

TEST(crc8_standard_check_value, "FW-03")
{
    const char *v = "123456789";                     // CRC-8 (poly 0x31, init 0xFF) check value
    CHECK_EQ((int)ezhal_aht_crc8((const uint8_t *)v, 9), 0xF7);
}

TEST(valid_frame_decodes, "FW-03")
{
    uint8_t f[7];
    frame_for(524288, 393216, 0x1C, f);              // 50 %RH, 25 C
    float t = 0, rh = 0;
    CHECK_EQ(ezhal_aht_decode(f, &t, &rh), (int)AHT_OK);
    CHECK_NEAR(t, 25.0, 0.01);
    CHECK_NEAR(rh, 50.0, 0.01);
}

TEST(missing_sensor_frames_rejected, "FW-13,FW-03")
{
    // regression: the Seeed library turned an all-zero I2C read into -50 C / 0 %RH "success"
    uint8_t zero[7] = {0, 0, 0, 0, 0, 0, 0}, ones[7] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    float t = 99, rh = 99;
    CHECK_EQ(ezhal_aht_decode(zero, &t, &rh), (int)AHT_BLANK);
    CHECK_EQ(ezhal_aht_decode(ones, &t, &rh), (int)AHT_BLANK);
    uint8_t zero_but_status[7] = {0x1C, 0, 0, 0, 0, 0, 0};          // CRC byte missing
    CHECK_EQ(ezhal_aht_decode(zero_but_status, &t, &rh), (int)AHT_CRC);
    CHECK_EQ(t, 99.0f);                                // outputs untouched on failure
}

TEST(corrupted_frame_rejected, "FW-03")
{
    uint8_t f[7];
    frame_for(524288, 393216, 0x1C, f);
    float t, rh;
    for (int byte = 0; byte < 6; byte++) {
        for (int bit = 0; bit < 8; bit++) {
            uint8_t g[7];
            memcpy(g, f, 7);
            g[byte] ^= (uint8_t)(1 << bit);
            CHECK_EQ(ezhal_aht_decode(g, &t, &rh), (int)AHT_CRC);   // every single-bit error is caught
        }
    }
}

TEST(busy_and_out_of_range, "FW-03")
{
    uint8_t f[7];
    float t, rh;
    frame_for(524288, 393216, 0x9C, f);              // busy bit set
    CHECK_EQ(ezhal_aht_decode(f, &t, &rh), (int)AHT_BUSY);
    frame_for(524288, 0, 0x1C, f);                   // -50 C: below the -40 C operating range
    CHECK_EQ(ezhal_aht_decode(f, &t, &rh), (int)AHT_RANGE);
    frame_for(524288, 1048575, 0x1C, f);             // 150 C
    CHECK_EQ(ezhal_aht_decode(f, &t, &rh), (int)AHT_RANGE);
    frame_for(0, 393216, 0x1C, f);                   // 0 %RH is valid
    CHECK_EQ(ezhal_aht_decode(f, &t, &rh), (int)AHT_OK);
}

static void env_reset(bool fitted)
{
    fake_reset_all();
    g_ack = fitted; g_triggers = g_reads = 0;
    g_ahtTrigMs = 0; g_ahtPending = false; g_ahtValidMs = 0; g_ahtAbsent = false; g_ahtFails = 0; g_ahtProbeMs = 0;
    frame_for(0x80000, 0x66666, 0x1C, g_frame);             // about 50 %RH / 30 degC
    g_fake_millis = 1000;
}
static void run_service(uint32_t ms)                          // env thread: every 100 ms
{
    for (uint32_t t = 0; t < ms; t += 100) {
        g_fake_millis += 100;
        ezhal_env_service(millis());
    }
}

TEST(sampling_read_never_touches_i2c, "FW-03,FW-02")
{
    env_reset(true);
    run_service(2000);
    int tr = g_triggers, rd = g_reads;
    float t = 0, h = 0;
    for (int i = 0; i < 1000; i++) {                         // 50 s of 20 Hz sampling, clock running
        g_fake_millis += 50;
        if (i % 40 == 0) { g_ack = true; }                   // (the sensor keeps answering throughout)
        ezhal_env_read(&t, &h);
    }
    CHECK_EQ(g_triggers, tr);
    CHECK_EQ(g_reads, rd);                                   // no bus access from the sampling path
    run_service(500);
    CHECK(ezhal_env_read(&t, &h) == HS_OK);                  // the env thread refreshes the reading
}

TEST(fitted_sensor_measured_once_per_second, "FW-03")
{
    env_reset(true);
    run_service(10000);
    CHECK(g_triggers >= 9 && g_triggers <= 11);
    CHECK(g_reads >= 9 && g_reads <= 11);
    float t = 0, h = 0;
    CHECK(ezhal_env_read(&t, &h) == HS_OK);
    CHECK(t > 29.0f && t < 31.0f && h > 49.0f && h < 51.0f);
}

TEST(missing_sensor_probed_once_a_minute, "FW-03,FW-13")
{
    env_reset(false);
    run_service(5 * 60000);                                  // five minutes without a sensor
    CHECK(g_triggers <= 3 + 5);                              // 3 failures, then one probe per minute
    CHECK_EQ(g_reads, 0);
    CHECK(g_ahtAbsent);
    float t = 0, h = 0;
    CHECK(ezhal_env_read(&t, &h) == HS_ERROR);
}

TEST(sensor_fitted_later_found_within_a_minute, "FW-03,FW-20")
{
    env_reset(false);
    run_service(2 * 60000);
    g_ack = true;                                            // technician plugs the sensor in
    run_service(61000);
    float t = 0, h = 0;
    CHECK(ezhal_env_read(&t, &h) == HS_OK);
    CHECK(!g_ahtAbsent);
}

TEST(sensor_that_stops_answering_goes_stale, "FW-03,FW-13")
{
    env_reset(true);
    run_service(3000);
    float t = 0, h = 0;
    CHECK(ezhal_env_read(&t, &h) == HS_OK);
    g_ack = false;                                           // unplugged
    run_service(3500);
    CHECK(ezhal_env_read(&t, &h) == HS_ERROR);               // reading older than 3 s
    run_service(2000);
    CHECK(g_ahtAbsent);                                      // and treated as missing after 3 failures
}

TEST(busy_sensor_retried_but_not_forever, "FW-03")
{
    env_reset(true);
    frame_for(0x80000, 0x66666, 0x9C, g_frame);              // busy bit set
    run_service(250);
    CHECK(g_ahtPending);                                     // still waiting for the conversion
    run_service(200);
    CHECK(!g_ahtPending);                                    // gave up after 300 ms
    CHECK_EQ(g_ahtLast, (int)AHT_BUSY);
}

MINITEST_MAIN
