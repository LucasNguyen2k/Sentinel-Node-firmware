// Event sound clips while the Edge Server is unreachable (32 MB boards, firmware 1.6.0) (FW-03, FW-07, FW-08, FW-20)
// @extract firmware/sentinel_node/sentinel_node.ino: BootInfo bootInfo fsAvailable FsDirStats fsImgSeq FS_AUD_DIR FS_AUD_PEND_DIR FS_AUD_KEEP_BYTES FS_AUD_PEND_KEEP_BYTES CLIP_RATE CLIP_PRE_MS CLIP_POST_MS CLIP_RING_BYTES clipRing clipWritten clipArmed clipTrigger clipEventId clipsSaved clipsSkipped serverLinkUp simOfflineUntil simOffline fsDirStats fsTrimDir fsMediaName fsImageEventId fsSaveFile alawEncode clipFeed serverReachable clipRequestIfOffline clipWavHeader clipService fsRetryPendingAudio
// @extract firmware/sentinel_node/ezsn_hal.cpp: g_lfs g_lfscfg g_fsMounted g_fsLock g_fsOp ezfs_bd_read ezfs_bd_prog ezfs_bd_erase ezfs_bd_sync ezfs_lock ezfs_unlock ezfs_config ezfs_blank ezfs_locks ezhal_fs_mount_region ezhal_fs_unmount ezhal_fs_mounted ezhal_fs_format ezfs_write ezfs_mkparents ezhal_fs_write_file ezhal_fs_append ezhal_fs_read ezhal_fs_size ezhal_fs_exists ezhal_fs_remove ezhal_fs_rename ezhal_fs_mkdir ezhal_fs_list ezhal_fs_usage
// @csource tests/fw_host/third_party/littlefs/lfs.c
// @csource tests/fw_host/third_party/littlefs/lfs_util.c
// @cflags -DLFS_THREADSAFE -DLFS_NO_DEBUG -fexceptions -DEZ_REAL_FS -DFS_AUD_PEND_KEEP_BYTES=200000 -DFS_AUD_KEEP_BYTES=200000
#include "minitest.h"
#include "fw_env.h"
#include "lfs.h"
void LOG(String m) { Serial.println(m); }
bool g_post_ok = true; std::vector<std::string> g_posted;
bool postMedia(const String &evId, const uint8_t *, uint32_t len, const char *type)
{
    if (!g_post_ok) return false;
    g_posted.push_back(evId.s + ":" + std::to_string(len) + ":" + type);
    return true;
}
#include "extracted.inc"

static int16_t sample(uint32_t i) { return (int16_t)((int32_t)((i * 37u) % 60000u) - 30000); }
static uint32_t fed = 0;
static void feed_seconds(double s)                    // audio tap frames of 20 ms
{
    uint32_t n = (uint32_t)(s * CLIP_RATE);
    int16_t f[320];
    for (uint32_t done = 0; done < n; done += 320) {
        for (int i = 0; i < 320; i++) f[i] = sample(fed + i);
        clipFeed(f, 320);
        fed += 320;
    }
}
static int alaw_decode(uint8_t a)                      // ITU-T G.711 reference decoder
{
    a ^= 0x55;
    int t = (a & 0x0F) << 4, seg = (a & 0x70) >> 4;
    if (seg == 0) t += 8; else if (seg == 1) t += 0x108; else t = (t + 0x108) << (seg - 1);
    return (a & 0x80) ? t : -t;
}
static std::vector<uint8_t> g_ring;
static void fresh()
{
    fake_reset_all();
    fakehal.flash_size = 0x2000000;
    g_fsMounted = false; memset(&g_lfs, 0, sizeof(g_lfs)); g_lock_depth = 0;
    String d; ezhal_fs_mount_region(true, &d);
    ezhal_fs_mkdir(FS_AUD_PEND_DIR);
    fsAvailable = true;
    g_ring.assign(CLIP_RING_BYTES, 0); clipRing = g_ring.data();
    clipWritten = 0; clipArmed = false; clipTrigger = 0; clipEventId = ""; clipsSaved = clipsSkipped = 0;
    serverLinkUp = false; simOfflineUntil = 0; fed = 0;
    bootInfo.bootCount = 9; fsImgSeq = 0;
    g_post_ok = true; g_posted.clear();
}
static std::vector<uint8_t> read_file(const std::string &p)
{
    int32_t n = ezhal_fs_size(p.c_str());
    std::vector<uint8_t> b(n > 0 ? n : 0);
    if (n > 0) ezhal_fs_read(p.c_str(), 0, b.data(), n);
    return b;
}
static uint32_t le32(const std::vector<uint8_t> &b, int o) { return b[o] | b[o + 1] << 8 | b[o + 2] << 16 | (uint32_t)b[o + 3] << 24; }

TEST(alaw_reference_values, "FW-03")
{
    CHECK_EQ((int)alawEncode(0), 0xD5);
    CHECK_EQ((int)alawEncode(-1), 0x55);
    CHECK_EQ((int)alawEncode(32767), 0xAA);
    CHECK_EQ((int)alawEncode(-32768), 0x2A);
    int worst = 0;
    for (int x = -32768; x <= 32767; x += 7) {
        int err = std::abs(alaw_decode(alawEncode((int16_t)x)) - x) - (std::abs(x) / 32 + 16);
        if (err > worst) worst = err;
    }
    CHECK_EQ(worst, 0);                               // within the A-law quantisation step everywhere
}

TEST(clip_only_while_server_unreachable, "FW-07,FW-08")
{
    fresh();
    feed_seconds(3);
    serverLinkUp = true;
    clipRequestIfOffline("E9_1");
    CHECK(!clipArmed);                                // server reachable: no clip
    serverLinkUp = false;
    clipRequestIfOffline("E9_1");
    CHECK(clipArmed);
    clipArmed = false;
    serverLinkUp = true; simOfflineUntil = millis() + 10000;
    clipRequestIfOffline("E9_2");
    CHECK(clipArmed);                                 // SIM OFFLINE counts as unreachable
    fsAvailable = false; clipArmed = false; simOfflineUntil = 0; serverLinkUp = false;
    clipRequestIfOffline("E9_3");
    CHECK(!clipArmed);                                // 16 MB boards: no clips
}

TEST(clip_is_two_seconds_before_and_three_after, "FW-07,FW-08")
{
    fresh();
    feed_seconds(4);
    uint32_t trig = clipWritten;
    clipRequestIfOffline("E9_4");
    feed_seconds(2);
    clipService();
    CHECK(clipArmed);                                 // waits for the 3 s after the trigger
    CHECK_EQ(fsDirStats(FS_AUD_PEND_DIR, ".wav").files, 0u);
    feed_seconds(1);
    clipService();
    CHECK(!clipArmed);
    FsDirStats st = fsDirStats(FS_AUD_PEND_DIR, ".wav");
    CHECK_EQ(st.files, 1u);
    CHECK_EQ(st.oldest, String("00009_00001_E9_4.wav"));
    std::vector<uint8_t> w = read_file(std::string(FS_AUD_PEND_DIR "/") + st.oldest.s);
    CHECK_EQ(w.size(), (size_t)(44 + 5 * CLIP_RATE));
    if (w.size() != 44 + 5 * CLIP_RATE) return;
    CHECK(memcmp(w.data(), "RIFF", 4) == 0 && memcmp(w.data() + 8, "WAVEfmt ", 8) == 0 && memcmp(w.data() + 36, "data", 4) == 0);
    CHECK_EQ(le32(w, 4), 36u + 5 * CLIP_RATE);
    CHECK_EQ((int)w[20], 6);                          // A-law
    CHECK_EQ((int)w[22], 1);                          // mono
    CHECK_EQ(le32(w, 24), (uint32_t)CLIP_RATE);
    CHECK_EQ((int)w[34], 8);
    CHECK_EQ(le32(w, 40), 5u * CLIP_RATE);
    CHECK_EQ((int)w[44], (int)alawEncode(sample(trig - 2 * CLIP_RATE)));
    CHECK_EQ((int)w[44 + 2 * CLIP_RATE], (int)alawEncode(sample(trig)));
    CHECK_EQ((int)w.back(), (int)alawEncode(sample(trig + 3 * CLIP_RATE - 1)));
    CHECK_EQ(clipsSaved, 1u);
}

TEST(clip_right_after_boot_is_shorter, "FW-07")
{
    fresh();
    feed_seconds(1);
    clipRequestIfOffline("E9_1");
    feed_seconds(3);
    clipService();
    FsDirStats st = fsDirStats(FS_AUD_PEND_DIR, ".wav");
    CHECK_EQ(st.bytes, 44u + 4 * CLIP_RATE);          // 1 s before (all there is) + 3 s after
}

TEST(one_clip_per_event, "FW-07")
{
    fresh();
    feed_seconds(3);
    clipRequestIfOffline("E9_5");
    clipRequestIfOffline("E9_5");                     // SUSPECT -> CONFIRMED of the same event
    CHECK_EQ(clipsSkipped, 0u);
    clipRequestIfOffline("E9_6");                     // another event while recording
    CHECK_EQ(clipsSkipped, 1u);
    CHECK_EQ(clipEventId, String("E9_5"));
}

TEST(late_writer_keeps_audio_still_in_ring, "FW-07,FW-20")
{
    fresh();
    feed_seconds(4);
    uint32_t trig = clipWritten;
    clipRequestIfOffline("E9_7");
    feed_seconds(12);                                 // picture thread busy for a long time
    clipService();
    FsDirStats st = fsDirStats(FS_AUD_PEND_DIR, ".wav");
    CHECK_EQ(st.files, 1u);
    std::vector<uint8_t> w = read_file(std::string(FS_AUD_PEND_DIR "/") + st.oldest.s);
    CHECK(w.size() > 44 && w.size() < 44 + 5 * CLIP_RATE);
    if (w.size() > 44) CHECK_EQ((int)w.back(), (int)alawEncode(sample(trig + 3 * CLIP_RATE - 1)));
}

TEST(pending_clip_delivered_when_server_back, "FW-07,FW-08,FW-20")
{
    fresh();
    feed_seconds(4);
    clipRequestIfOffline("E9_8");
    feed_seconds(3);
    clipService();
    g_fake_millis += 20000;
    fsRetryPendingAudio();
    CHECK(g_posted.empty());                          // still unreachable
    serverLinkUp = true; g_post_ok = false;
    g_fake_millis += 20000;
    fsRetryPendingAudio();
    CHECK_EQ(fsDirStats(FS_AUD_PEND_DIR, ".wav").files, 1u);
    g_post_ok = true;
    g_fake_millis += 5000;
    fsRetryPendingAudio();
    CHECK(g_posted.empty());                          // at most one try per 10 s
    g_fake_millis += 6000;
    fsRetryPendingAudio();
    CHECK_EQ(g_posted.size(), (size_t)1);
    if (!g_posted.empty()) CHECK_EQ(g_posted[0], "E9_8:" + std::to_string(44 + 5 * CLIP_RATE) + ":audio/wav");
    CHECK_EQ(fsDirStats(FS_AUD_PEND_DIR, ".wav").files, 0u);
    CHECK(ezhal_fs_exists("/audio/00009_00001_E9_8.wav"));
}

TEST(pending_clips_budget_keeps_newest, "FW-08")
{
    fresh();
    feed_seconds(3);
    for (int k = 0; k < 4; k++) {
        clipRequestIfOffline(String("E9_" + std::to_string(10 + k)));
        feed_seconds(3);
        clipService();
    }
    FsDirStats st = fsDirStats(FS_AUD_PEND_DIR, ".wav");
    CHECK(st.bytes <= 200000u);
    CHECK_EQ(st.files, 2u);
    CHECK(st.newest.endsWith("E9_13.wav"));
}

MINITEST_MAIN
