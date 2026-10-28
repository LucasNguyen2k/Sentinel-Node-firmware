// Step 3 on 32 MB boards: offline buffer, event pictures, event log on the file system (FW-07, FW-08, FW-20, FW-18)
// @extract firmware/sentinel_node/sentinel_node.ino: DATA_STORE_FLASH FLASH_BUF_INTERVAL_MS SD_BUFFER_MAX_BYTES TaskIdx taskBeat BootInfo bootInfo sdMutex sdOk storeOk lastFlashBufMs thinnedRecords bufferedRecords droppedRecords bufferReplayOffset otaInProgress fsAvailable fsDetail FS_AUD_PEND_DIR CLIP_RATE CLIP_RING_BYTES clipRing fsMediaName fsSaveFile FsDirStats FS_IMG_DIR FS_PEND_DIR FS_BUF_DIR FS_BUF_CURSOR FS_LOG_FILE FS_LOG_OLD FS_IMG_KEEP_BYTES FS_PEND_KEEP_BYTES FS_BUF_MAX_BYTES FS_BUF_CHUNK_BYTES FS_LOG_ROTATE_BYTES fsImgSeq fsBufPending fsBufDropped fsBufWriteSeq fsBufReadSeq fsBufReadOff lockM unlockM beat fsDirStats fsTrimDir fsImageName fsImageEventId fsSaveImage fsBufChunkPath fsCountLines fsBufSaveCursor fsBufInit fsBufAppend fsBufReplay fsLogAppend fsTailLines fsLogTail fsRetryPendingImage initStorage sdUsable storeEventLine sdAppendLine bufferToStorage replayBuffered readEventLogTail
// @extract firmware/sentinel_node/ezsn_hal.cpp: EZREC_MAGIC EZLOG_MAGIC EZLOG_HDR EZLOG_RHDR EZLOG_F_COMMIT EZLOG_F_DONE EZLOG_MAX_SECTORS EzRecHeader EzLogSectorHdr EzLogRecHdr EzLogRegion EzLogState ezhal_flog_regions ezhal_flog_state ezhal_crc32 ezlog_rec_size ezhal_flog_addr ezhal_flog_rec_at ezhal_flog_next_used ezhal_flog_init ezhal_flog_new_sector ezhal_flog_append ezhal_flog_peek ezhal_flog_consume ezhal_flog_pending ezhal_flog_read_recent ezhal_flog_stats ezhal_flog_format g_lfs g_lfscfg g_fsMounted g_fsLock g_fsOp ezfs_bd_read ezfs_bd_prog ezfs_bd_erase ezfs_bd_sync ezfs_lock ezfs_unlock ezfs_config ezfs_blank ezfs_locks ezhal_fs_mount_region ezhal_fs_unmount ezhal_fs_mounted ezhal_fs_format ezfs_write ezfs_mkparents ezhal_fs_write_file ezhal_fs_append ezhal_fs_read ezhal_fs_size ezhal_fs_exists ezhal_fs_remove ezhal_fs_rename ezhal_fs_mkdir ezhal_fs_list ezhal_fs_usage
// @csource tests/fw_host/third_party/littlefs/lfs.c
// @csource tests/fw_host/third_party/littlefs/lfs_util.c
// @cflags -DLFS_THREADSAFE -DLFS_NO_DEBUG -fexceptions -DEZ_REAL_FS -DFS_BUF_MAX_BYTES=65536 -DFS_BUF_CHUNK_BYTES=8192 -DFS_IMG_KEEP_BYTES=102400 -DFS_PEND_KEEP_BYTES=81920 -DFS_LOG_ROTATE_BYTES=2048
#include "minitest.h"
#include "fw_env.h"
#include "lfs.h"
std::vector<std::string> g_sent; int g_ok_budget = -1;
bool submitDataWithSocket(String d)
{
    if (g_ok_budget == 0) return false;
    if (g_ok_budget > 0) g_ok_budget--;
    g_sent.push_back(d.s);
    return true;
}
bool g_post_ok = true; std::vector<std::string> g_posted;
bool postImage(const String &evId, const uint8_t *, uint32_t len)
{
    if (!g_post_ok) return false;
    g_posted.push_back(evId.s + ":" + std::to_string(len));
    return true;
}
void LOG(String m) { Serial.println(m); }
#include "extracted.inc"
#include <set>

static void boot()                                   // power-on: RAM lost, flash kept
{
    fakehal.flash_budget = -1;
    g_fsMounted = false; memset(&g_lfs, 0, sizeof(g_lfs)); g_lock_depth = 0;
    fsAvailable = false; storeOk = false; sdOk = false;
    fsBufPending = fsBufDropped = fsBufWriteSeq = fsBufReadSeq = fsBufReadOff = 0;
    bufferedRecords = droppedRecords = thinnedRecords = 0; lastFlashBufMs = 0;
    initStorage();
}
static void fresh()
{
    fake_reset_all();
    fakehal.flash_size = 0x2000000;
    g_sent.clear(); g_ok_budget = -1; g_post_ok = true; g_posted.clear(); otaInProgress = false;
    bootInfo.bootCount = 7; fsImgSeq = 0;
    boot();
}
static std::string rec(int i) { return "21.0,55.0,400,17915" + std::to_string(10000 + i) + "," + std::to_string(i) + ",-1.0,-43.0,-57.0,-1,0,83"; }
static void offline(int first, int n)                // n upload cycles while the server is unreachable
{
    for (int k = first; k < first + n; k++) {
        bufferToStorage(String(rec(k) + "$"));
        g_fake_millis += 900;
    }
}
static void replay_all() { for (int i = 0; i < 400 && replayBuffered() && bufferedRecords > 0; i++) {} replayBuffered(); }

TEST(file_system_used_on_32mb_board, "FW-08")
{
    fresh();
    CHECK(fsAvailable);
    CHECK(ezhal_fs_exists("/img"));
    CHECK(ezhal_fs_exists("/pend"));
    offline(0, 10);
    CHECK_EQ(fsBufPending, 10u);
    CHECK_EQ(bufferedRecords, 10u);
    CHECK_EQ(ezhal_flog_pending(EZFLOG_BUFFER), 0u);  // internal 144 KB buffer not used
}

TEST(replay_in_order_and_chunks_removed, "FW-08")
{
    fresh();
    offline(0, 400);                                   // ~3 chunks of 8 KB
    CHECK(fsDirStats(FS_BUF_DIR, ".txt").files >= 3);
    replay_all();
    CHECK_EQ(g_sent.size(), (size_t)400);
    for (int i = 0; i < 400 && i < (int)g_sent.size(); i++) CHECK_EQ(g_sent[i], rec(i) + "$");
    CHECK_EQ(fsBufPending, 0u);
    CHECK_EQ(fsDirStats(FS_BUF_DIR, ".txt").files, 1u); // only the chunk still being written
}

TEST(failed_send_and_reboot_resume_without_loss, "FW-08,FW-20")
{
    fresh();
    offline(0, 120);
    g_ok_budget = 30;
    CHECK(!replayBuffered());                         // server stops answering after 30
    boot();                                           // power cycle
    CHECK_EQ(fsBufPending, 90u);
    g_ok_budget = -1;
    replay_all();
    CHECK_EQ(g_sent.size(), (size_t)120);             // cursor saved: nothing lost, nothing twice
    for (int i = 0; i < 120 && i < (int)g_sent.size(); i++) CHECK_EQ(g_sent[i], rec(i) + "$");
}

TEST(power_cut_during_replay_resends_at_most_one_batch, "FW-08,FW-20")
{
    fresh();
    offline(0, 120);
    fakehal.flash_budget = 40;                        // power fails while the read position is saved
    try { replayBuffered(); } catch (PowerLoss &) {}
    boot();
    replay_all();
    std::set<std::string> uniq(g_sent.begin(), g_sent.end());
    CHECK_EQ(uniq.size(), (size_t)120);               // every record delivered
    CHECK(g_sent.size() <= 120 + 50);                 // at most one batch twice
}

TEST(budget_drops_oldest_and_counts, "FW-08")
{
    fresh();
    offline(0, 2000);                                 // 64 KB budget holds about 1100 records
    CHECK(fsBufDropped > 0);
    CHECK_EQ(fsBufPending + fsBufDropped, 2000u);
    uint32_t kept = fsBufPending;
    replay_all();
    CHECK_EQ(g_sent.size(), (size_t)kept);            // every kept record is delivered
    CHECK_EQ(g_sent.front(), rec((int)fsBufDropped) + "$");   // oldest kept record first
    CHECK_EQ(g_sent.back(), rec(1999) + "$");
}

TEST(older_internal_flash_records_sent_first, "FW-08")
{
    fresh();
    fsAvailable = false;                              // records buffered before the file system existed
    offline(0, 5);
    fsAvailable = true;
    offline(5, 5);
    replay_all();
    CHECK_EQ(g_sent.size(), (size_t)10);
    for (int i = 0; i < 10 && i < (int)g_sent.size(); i++) CHECK_EQ(g_sent[i], rec(i) + "$");
}

TEST(event_log_rotates_and_tail_spans_files, "FW-08,FW-18")
{
    fresh();
    for (int i = 0; i < 100; i++) CHECK(storeEventLine(String("1791925230,E7_" + std::to_string(i) + ",SUSPECT,20.3,174,41.1,010,0,95")));
    CHECK(ezhal_fs_exists(FS_LOG_OLD));               // rotated at 2 KB
    String t = readEventLogTail(3);
    CHECK_EQ(t, String("1791925230,E7_97,SUSPECT,20.3,174,41.1,010,0,95\n1791925230,E7_98,SUSPECT,20.3,174,41.1,010,0,95\n"
                       "1791925230,E7_99,SUSPECT,20.3,174,41.1,010,0,95"));
    String all = readEventLogTail(45);                // reaches into events.1.csv
    int lines = 1;
    for (unsigned i = 0; i < all.length(); i++) if (all[i] == '\n') lines++;
    CHECK_EQ(lines, 45);
    CHECK(all.startsWith("1791925230,E7_55,"));
    boot();
    CHECK(readEventLogTail(1).startsWith("1791925230,E7_99,"));
}

TEST(picture_names_sort_and_keep_event_id, "FW-07")
{
    fresh();
    String a = fsImageName("E7_3"), b = fsImageName("M7_4");
    CHECK_EQ(a, String("00007_00001_E7_3.jpg"));
    CHECK(strcmp(a.c_str(), b.c_str()) < 0);
    CHECK_EQ(fsImageEventId(a), String("E7_3"));
    CHECK_EQ(fsImageEventId(b), String("M7_4"));
}

TEST(picture_history_keeps_newest_within_budget, "FW-07,FW-08")
{
    fresh();
    std::string jpg(10000, 'J');
    for (int i = 0; i < 30; i++) fsSaveImage(FS_IMG_DIR, fsImageName("E7_" + std::to_string(i)), (const uint8_t *)jpg.data(), jpg.size(), FS_IMG_KEEP_BYTES);
    FsDirStats st = fsDirStats(FS_IMG_DIR, ".jpg");
    CHECK(st.bytes <= FS_IMG_KEEP_BYTES);
    CHECK_EQ(st.files, 10u);
    CHECK_EQ(st.newest, String("00007_00030_E7_29.jpg"));
    CHECK_EQ(st.oldest, String("00007_00021_E7_20.jpg"));
}

TEST(undelivered_pictures_are_retried_and_moved, "FW-07,FW-20")
{
    fresh();
    std::string jpg(5000, 'P');
    for (int i = 0; i < 3; i++) fsSaveImage(FS_PEND_DIR, fsImageName("E7_" + std::to_string(i)), (const uint8_t *)jpg.data(), jpg.size(), FS_PEND_KEEP_BYTES);
    g_post_ok = false;
    g_fake_millis += 20000; fsRetryPendingImage();
    CHECK_EQ(fsDirStats(FS_PEND_DIR, ".jpg").files, 3u);       // still pending
    g_post_ok = true;
    g_fake_millis += 5000; fsRetryPendingImage();
    CHECK(g_posted.empty());                                   // at most one try per 10 s
    g_fake_millis += 6000; fsRetryPendingImage();
    CHECK_EQ(g_posted.size(), (size_t)1);
    if (!g_posted.empty()) CHECK_EQ(g_posted[0], std::string("E7_0:5000"));   // oldest first, original event id
    CHECK_EQ(fsDirStats(FS_PEND_DIR, ".jpg").files, 2u);
    CHECK(ezhal_fs_exists("/img/00007_00001_E7_0.jpg"));       // moved to the history
    g_fake_millis += 11000; fsRetryPendingImage();
    g_fake_millis += 11000; fsRetryPendingImage();
    CHECK_EQ(fsDirStats(FS_PEND_DIR, ".jpg").files, 0u);
    CHECK_EQ(fsDirStats(FS_IMG_DIR, ".jpg").files, 3u);
}

TEST(pending_pictures_budget, "FW-07,FW-08")
{
    fresh();
    std::string jpg(10000, 'Q');
    for (int i = 0; i < 20; i++) fsSaveImage(FS_PEND_DIR, fsImageName("E7_" + std::to_string(i)), (const uint8_t *)jpg.data(), jpg.size(), FS_PEND_KEEP_BYTES);
    FsDirStats st = fsDirStats(FS_PEND_DIR, ".jpg");
    CHECK(st.bytes <= FS_PEND_KEEP_BYTES);
    CHECK_EQ(st.newest, String("00007_00020_E7_19.jpg"));     // newest kept, oldest dropped
}

MINITEST_MAIN
