// Application data store on internal flash: offline buffer, replay, event log (FW-08, FW-20, FW-18)
// @extract firmware/sentinel_node/sentinel_node.ino: DATA_STORE_FLASH FLASH_BUF_INTERVAL_MS SD_BUFFER_MAX_BYTES TaskIdx taskBeat sdMutex sdOk storeOk fsAvailable fsDetail FS_AUD_PEND_DIR CLIP_RATE CLIP_RING_BYTES clipRing FsDirStats fsImgSeq fsBufPending fsBufDropped fsBufWriteSeq fsBufReadSeq fsBufReadOff FS_BUF_DIR FS_BUF_CURSOR FS_LOG_FILE FS_LOG_OLD FS_IMG_DIR FS_PEND_DIR FS_BUF_MAX_BYTES FS_BUF_CHUNK_BYTES FS_LOG_ROTATE_BYTES fsDirStats fsBufChunkPath fsCountLines fsBufSaveCursor fsBufInit fsBufAppend fsBufReplay fsLogAppend fsTailLines fsLogTail lastFlashBufMs thinnedRecords bufferedRecords droppedRecords bufferReplayOffset otaInProgress lockM unlockM beat initStorage sdUsable storeEventLine sdAppendLine bufferToStorage replayBuffered readEventLogTail
// @extract firmware/sentinel_node/ezsn_hal.cpp: EZREC_MAGIC EZLOG_MAGIC EZLOG_HDR EZLOG_RHDR EZLOG_F_COMMIT EZLOG_F_DONE EZLOG_MAX_SECTORS EzRecHeader EzLogSectorHdr EzLogRecHdr EzLogRegion EzLogState ezhal_flog_regions ezhal_flog_state ezhal_crc32 ezhal_frec_slot_valid ezhal_frec_read ezhal_frec_write ezlog_rec_size ezhal_flog_addr ezhal_flog_rec_at ezhal_flog_next_used ezhal_flog_init ezhal_flog_new_sector ezhal_flog_append ezhal_flog_peek ezhal_flog_consume ezhal_flog_pending ezhal_flog_read_recent ezhal_flog_stats ezhal_flog_format
#include "minitest.h"
#include "fw_env.h"
std::vector<std::string> g_sent; int g_ok_budget = -1;
bool submitDataWithSocket(String d)
{
    if (g_ok_budget == 0) return false;
    if (g_ok_budget > 0) g_ok_budget--;
    g_sent.push_back(d.s);
    return true;
}
void LOG(String m) { Serial.println(m); }
#include "extracted.inc"

static void boot()                                   // power-on: RAM lost, flash kept
{
    storeOk = false; bufferedRecords = droppedRecords = thinnedRecords = 0; lastFlashBufMs = 0;
    initStorage();
}
static void fresh() { fake_reset_all(); g_sent.clear(); g_ok_budget = -1; otaInProgress = false; sdOk = false; boot(); }
static std::string rec(int i) { return "21.0,55.0,400,1791500" + std::to_string(100 + i) + "," + std::to_string(i) + ",-1.0,-43.0,-57.0,-1,0,83"; }
static String packet(int first, int n)               // what the upload thread hands over: rec_rec_..._rec$
{
    std::string p;
    for (int i = first; i < first + n; i++) p += (p.empty() ? "" : "_") + rec(i);
    return String(p + "$");
}

TEST(store_ready_without_sd_card, "FW-08")
{
    fresh();
    CHECK(storeOk);
    CHECK(!sdOk);
    CHECK_EQ(bufferedRecords, 0u);
}

TEST(offline_keeps_newest_record_per_upload_cycle, "FW-08")
{
    fresh();
    for (int k = 0; k < 10; k++) {                    // ten upload cycles, 900 ms apart, 18 records each
        bufferToStorage(packet(k * 18, 18));
        g_fake_millis += 900;
    }
    CHECK_EQ(bufferedRecords, 10u);
    replayBuffered();
    CHECK_EQ(g_sent.size(), (size_t)10);
    for (int k = 0; k < 10 && k < (int)g_sent.size(); k++) CHECK_EQ(g_sent[k], rec(k * 18 + 17) + "$");   // last of each packet
}

TEST(thinning_limits_rate, "FW-08")
{
    fresh();
    for (int k = 0; k < 30; k++) { bufferToStorage(packet(k, 1)); g_fake_millis += 300; }   // faster than 800 ms
    CHECK(bufferedRecords >= 10 && bufferedRecords <= 12);
    CHECK_EQ(thinnedRecords, 30u - bufferedRecords);
}

TEST(replay_only_consumes_after_server_accepts, "FW-08,FW-20")
{
    fresh();
    for (int k = 0; k < 5; k++) { bufferToStorage(packet(k, 1)); g_fake_millis += 900; }
    g_ok_budget = 2;
    CHECK(!replayBuffered());                         // third send fails
    CHECK_EQ(bufferedRecords, 3u);
    g_ok_budget = -1;
    CHECK(replayBuffered());
    CHECK_EQ(g_sent.size(), (size_t)5);
    for (int k = 0; k < 5 && k < (int)g_sent.size(); k++) CHECK_EQ(g_sent[k], rec(k) + "$");   // record 3 not lost, nothing twice
}

TEST(buffer_survives_reboot_without_duplicates, "FW-08,FW-20")
{
    fresh();
    for (int k = 0; k < 6; k++) { bufferToStorage(packet(k, 1)); g_fake_millis += 900; }
    g_ok_budget = 2;
    replayBuffered();
    boot();                                           // power cycle before the rest is sent
    CHECK_EQ(bufferedRecords, 4u);
    g_ok_budget = -1;
    replayBuffered();
    CHECK_EQ(g_sent.size(), (size_t)6);
    for (int k = 0; k < 6 && k < (int)g_sent.size(); k++) CHECK_EQ(g_sent[k], rec(k) + "$");
}

TEST(replay_sends_at_most_fifty_per_cycle, "FW-08")
{
    fresh();
    for (int k = 0; k < 120; k++) { bufferToStorage(packet(k, 1)); g_fake_millis += 900; }
    replayBuffered();
    CHECK_EQ(g_sent.size(), (size_t)50);
    replayBuffered(); replayBuffered();
    CHECK_EQ(g_sent.size(), (size_t)120);
    CHECK_EQ(bufferedRecords, 0u);
}

TEST(long_outage_drops_oldest_beyond_capacity, "FW-08")
{
    fresh();
    for (int k = 0; k < 3000; k++) { bufferToStorage(packet(k, 1)); g_fake_millis += 900; }   // 45 minutes offline
    EzFlashLogStats st;
    ezhal_flog_stats(EZFLOG_BUFFER, &st);
    CHECK(st.dropped > 0);
    CHECK(bufferedRecords >= 2200);                   // ~37 minutes kept
    CHECK_EQ(bufferedRecords + st.dropped, 3000u);
    replayBuffered();
    CHECK_EQ(g_sent[0], rec((int)st.dropped) + "$");   // oldest kept record comes first
}

TEST(event_log_lines_and_tail, "FW-08,FW-18")
{
    fresh();
    for (int i = 0; i < 25; i++) CHECK(storeEventLine(String("1791500000,E3_" + std::to_string(i) + ",SUSPECT,-1.0,400,9.0,010,0,95")));
    String t = readEventLogTail(3);
    CHECK_EQ(t, String("1791500000,E3_22,SUSPECT,-1.0,400,9.0,010,0,95\n1791500000,E3_23,SUSPECT,-1.0,400,9.0,010,0,95\n"
                       "1791500000,E3_24,SUSPECT,-1.0,400,9.0,010,0,95"));
    boot();
    CHECK(readEventLogTail(1).startsWith("1791500000,E3_24"));
}

TEST(flash_store_independent_of_sd_ota, "FW-08,FW-17")
{
    fresh();
    otaInProgress = true;                             // SD OTA pause applies to the SD card only
    bufferToStorage(packet(0, 1));
    CHECK_EQ(bufferedRecords, 1u);
    CHECK(storeEventLine("x"));
}

TEST(store_failure_counts_dropped, "FW-08,FW-13")
{
    fresh();
    storeOk = false;
    bufferToStorage(packet(0, 1));
    CHECK_EQ(droppedRecords, 1u);
    CHECK(!storeEventLine("x"));
    CHECK_EQ(readEventLogTail(3), String("flash data store not available"));
}

MINITEST_MAIN
