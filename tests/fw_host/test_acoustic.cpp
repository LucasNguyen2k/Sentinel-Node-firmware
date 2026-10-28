// Acoustic leak detection: PCM tap, band energy, adaptive noise floor, confirmation (FW-03, FW-04, FW-05)
// @extract firmware/sentinel_node/sentinel_node.ino: ACOUSTIC_DETECTION DEVICE_NAME SERVER_ADDRESS SERVER_PORT SOCKET_BASE_PORT API_TOKEN_DEFAULT CONFIG_MAGIC CONFIG_VERSION QCS6490 NodeConfig ssid pass cfg acAccSum acAccHf acAccN acFrames acLastFrameMs acPrevSample acDb acHfDb acFloorDb acFloorInit acAboveCnt acAlarm setDefaultConfig audioTapHandler processAcoustic micOk
#include "minitest.h"
#include "fw_env.h"
void clipFeed(const int16_t *, uint32_t) {}
#include "extracted.inc"
#include <vector>

static const double FS = 16000.0;   // audio preset 1: 16 kHz mono

static void fresh()
{
    fake_reset_all();
    setDefaultConfig();
    acAccSum = acAccHf = 0; acAccN = 0; acFrames = 0; acLastFrameMs = 0; acPrevSample = 0;
    acDb = acHfDb = acFloorDb = -120; acFloorInit = false; acAboveCnt = 0; acAlarm = false;
}

// one 50 ms window (800 samples) of a sine, delivered as 20 ms frames like the audio pipeline
static void window_sine(double amp, double freq, int samples = 800)
{
    static double phase = 0;
    std::vector<int16_t> frame;
    for (int i = 0; i < samples; i++) {
        frame.push_back((int16_t)lrint(amp * sin(phase)));
        phase += 2 * M_PI * freq / FS;
        if (frame.size() == 320 || i == samples - 1) {
            audioTapHandler(frame.data(), (uint32_t)frame.size());
            frame.clear();
        }
    }
    processAcoustic();
}

static double db_of_sine(double amp) { return 20 * log10(amp / sqrt(2.0) / 32768.0); }

TEST(full_band_level_matches_theory, "FW-04")
{
    fresh();
    window_sine(16384, 1000);
    CHECK_NEAR(acDb, db_of_sine(16384), 0.2);       // about -9 dBFS
    window_sine(1638, 1000);
    CHECK_NEAR(acDb, db_of_sine(1638), 0.2);        // 20 dB lower
}

TEST(first_difference_emphasises_hiss_band, "FW-04")
{
    fresh();
    // |H(f)| of x[n]-x[n-1] = 2 sin(pi f / fs): +3 dB at 4 kHz, about -28 dB at 100 Hz
    // (the first window after a frequency change contains one large step where the old and new
    //  signals meet, so each frequency is settled for one window before it is measured)
    window_sine(8000, 4000);
    window_sine(8000, 4000);
    CHECK_NEAR(acHfDb - acDb, 20 * log10(2 * sin(M_PI * 4000 / FS)), 0.3);
    window_sine(8000, 100);
    window_sine(8000, 100);
    CHECK_NEAR(acHfDb - acDb, 20 * log10(2 * sin(M_PI * 100 / FS)), 0.5);
}

TEST(no_audio_leaves_levels_unchanged, "FW-04")
{
    fresh();
    window_sine(8000, 3000);
    float before = acHfDb;
    processAcoustic();                               // no frames since last window
    CHECK_EQ(acHfDb, before);
}

TEST(alarm_needs_sustained_level_above_floor, "FW-04,FW-05")
{
    fresh();
    window_sine(200, 3000);                          // quiet background sets the floor
    float floor = acFloorDb;
    CHECK(acFloorInit);
    for (uint32_t i = 0; i + 1 < cfg.acConfirmWindows; i++) window_sine(4000, 3000);   // +26 dB
    CHECK(!acAlarm);                                 // one window short
    window_sine(4000, 3000);
    CHECK(acAlarm);
    CHECK_NEAR(acFloorDb, floor, 1e-3);              // floor frozen while loud
}

TEST(short_burst_resets_confirmation_counter, "FW-04")
{
    fresh();
    window_sine(200, 3000);
    for (int i = 0; i < 10; i++) window_sine(4000, 3000);
    window_sine(200, 3000);                          // back to quiet resets the count
    for (uint32_t i = 0; i + 1 < cfg.acConfirmWindows; i++) window_sine(4000, 3000);
    CHECK(!acAlarm);
}

TEST(alarm_clears_below_clear_level, "FW-04")
{
    fresh();
    window_sine(200, 3000);
    for (uint32_t i = 0; i < cfg.acConfirmWindows; i++) window_sine(4000, 3000);
    CHECK(acAlarm);
    window_sine(200 * 3.0, 3000);                    // +9.5 dB: above clear level (6 dB) -> still alarmed
    CHECK(acAlarm);
    window_sine(200 * 1.5, 3000);                    // +3.5 dB: below clear level
    CHECK(!acAlarm);
}

TEST(noise_floor_adapts_only_when_quiet, "FW-04")
{
    fresh();
    window_sine(1000, 3000);
    float f0 = acFloorDb;
    for (int i = 0; i < 200; i++) window_sine(500, 3000);    // background 6 dB quieter
    CHECK(acFloorDb < f0 - 3.0);                             // floor followed it down
    CHECK(acFloorDb > f0 - 6.1);
}

TEST(frames_are_continuous_across_boundaries, "FW-03")
{
    fresh();
    std::vector<int16_t> a(320, 1000), b(320, 1000);
    audioTapHandler(a.data(), 320);
    audioTapHandler(b.data(), 320);
    processAcoustic();
    // a constant signal has no first difference except the very first step from 0 to 1000
    CHECK(acHfDb < acDb - 25);
}

TEST(mic_ok_requires_recent_frames, "FW-13")
{
    fresh();
    CHECK(!micOk());
    window_sine(1000, 1000);
    CHECK(micOk());
    g_fake_millis += 3500;
    CHECK(!micOk());
    fakehal.mic_present = false;
    window_sine(1000, 1000);
    CHECK(!micOk());
}

MINITEST_MAIN
