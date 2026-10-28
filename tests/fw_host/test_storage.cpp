// Optional SD-card storage (ENABLE_SD_STORAGE): store-and-forward buffer, replay, event log, OTA pause (FW-08, FW-20)
// @extract firmware/sentinel_node/sentinel_node.ino: SD_BUFFER_MAX_BYTES TaskIdx taskBeat sdMutex sdOk sdRoot fsAvailable fsDetail FS_AUD_PEND_DIR CLIP_RATE CLIP_RING_BYTES clipRing FsDirStats fsImgSeq fsBufPending fsBufDropped fsBufWriteSeq fsBufReadSeq fsBufReadOff FS_BUF_DIR FS_BUF_CURSOR FS_LOG_FILE FS_LOG_OLD FS_IMG_DIR FS_PEND_DIR FS_BUF_MAX_BYTES FS_BUF_CHUNK_BYTES FS_LOG_ROTATE_BYTES fsDirStats fsBufChunkPath fsCountLines fsBufSaveCursor fsBufInit fsBufAppend fsBufReplay fsLogAppend fsTailLines fsLogTail bufferedRecords droppedRecords bufferReplayOffset otaInProgress lockM unlockM beat initStorage sdUsable sdAppendLine sdWriteFile bufferToStorage replayBuffered readEventLogTail
// @variant sd: -DENABLE_SD_STORAGE
#include "minitest.h"
#include "fw_env.h"
std::vector<std::string> g_sent; int g_send_ok_budget = -1;   // -1 = unlimited
bool submitDataWithSocket(String d)
{
    if (g_send_ok_budget == 0) return false;
    if (g_send_ok_budget > 0) g_send_ok_budget--;
    g_sent.push_back(d.s);
    return true;
}
void LOG(String m) { Serial.println(m); }
#include "extracted.inc"

static const char *BUF = "ezsn/buffer.txt";
static void fresh()
{
    fake_reset_all();
    sdOk = true; otaInProgress = false;
    bufferedRecords = droppedRecords = bufferReplayOffset = 0;
    g_sent.clear(); g_send_ok_budget = -1;
}
static std::string rec(int i) { return "21.0,55.0,400,0," + std::to_string(i) + "$"; }

TEST(offline_records_are_buffered_one_per_line, "FW-08")
{
    fresh();
    for (int i = 1; i <= 3; i++) bufferToStorage(String(rec(i)));
    CHECK_EQ(bufferedRecords, 3u);
    CHECK_EQ(fakehal.files[BUF], rec(1) + "\n" + rec(2) + "\n" + rec(3) + "\n");
}

TEST(replay_is_oldest_first_and_removes_file, "FW-08")
{
    fresh();
    for (int i = 1; i <= 5; i++) bufferToStorage(String(rec(i)));
    CHECK(replayBuffered());
    CHECK(replayBuffered());                       // second call notices the end and cleans up
    CHECK_EQ(g_sent.size(), (size_t)5);
    for (int i = 0; i < 5 && i < (int)g_sent.size(); i++) CHECK_EQ(g_sent[i], rec(i + 1));
    CHECK(!fakehal.files.count(BUF));
    CHECK_EQ(bufferedRecords, 0u);
    CHECK_EQ(bufferReplayOffset, 0u);
}

TEST(failed_send_resumes_without_duplicates, "FW-08,FW-20")
{
    fresh();
    for (int i = 1; i <= 6; i++) bufferToStorage(String(rec(i)));
    g_send_ok_budget = 2;
    CHECK(!replayBuffered());                      // third send fails
    CHECK_EQ(g_sent.size(), (size_t)2);
    g_send_ok_budget = -1;
    CHECK(replayBuffered());
    CHECK(replayBuffered());
    CHECK_EQ(g_sent.size(), (size_t)6);            // records 3..6 once each, nothing repeated
    for (int i = 0; i < 6 && i < (int)g_sent.size(); i++) CHECK_EQ(g_sent[i], rec(i + 1));
}

TEST(replay_sends_at_most_fifty_per_cycle, "FW-08")
{
    fresh();
    for (int i = 1; i <= 120; i++) bufferToStorage(String(rec(i)));
    replayBuffered();
    CHECK_EQ(g_sent.size(), (size_t)50);           // live data is not starved
    replayBuffered(); replayBuffered(); replayBuffered();
    CHECK_EQ(g_sent.size(), (size_t)120);
    CHECK(!fakehal.files.count(BUF));
}

TEST(buffer_found_after_reboot_is_replayed, "FW-08")
{
    fresh();
    fakehal.files[BUF] = rec(1) + "\n" + rec(2) + "\n";
    initStorage();                                 // boot: card mounted, pending buffer detected
    CHECK(bufferedRecords > 0);
    replayBuffered(); replayBuffered();
    CHECK_EQ(g_sent.size(), (size_t)2);
}

TEST(buffer_capped_at_eight_megabytes, "FW-08")
{
    fresh();
    fakehal.files[BUF] = std::string(SD_BUFFER_MAX_BYTES, 'x');
    bufferToStorage(String(rec(1)));
    CHECK_EQ(droppedRecords, 1u);
    CHECK_EQ(fakehal.files[BUF].size(), (size_t)SD_BUFFER_MAX_BYTES);
}

TEST(no_card_or_write_error_counts_dropped, "FW-08,FW-13")
{
    fresh();
    sdOk = false;
    bufferToStorage(String(rec(1)));
    CHECK_EQ(droppedRecords, 1u);
    sdOk = true;
    fakehal.sd_fail_writes = true;
    bufferToStorage(String(rec(2)));
    CHECK_EQ(droppedRecords, 2u);
    bufferToStorage(String(""));                  // empty packets are ignored
    CHECK_EQ(droppedRecords, 2u);
}

TEST(sd_access_paused_during_ota, "FW-08,FW-17")
{
    fresh();
    bufferToStorage(String(rec(1)));
    otaInProgress = true;
    CHECK(!sdUsable());
    CHECK(!sdAppendLine("ezsn/events.csv", "x"));
    uint8_t b[3] = {1, 2, 3};
    CHECK(!sdWriteFile("ezsn/img/E1_1.jpg", b, 3));
    bufferToStorage(String(rec(2)));
    CHECK_EQ(droppedRecords, 1u);
    CHECK(replayBuffered());
    CHECK(g_sent.empty());                         // nothing read while the SDK task owns the card
    CHECK_EQ(readEventLogTail(5), String("SD busy: OTA in progress"));
    otaInProgress = false;
    CHECK(sdAppendLine("ezsn/events.csv", "x"));
}

TEST(event_log_tail_starts_on_line_boundary, "FW-08,FW-18")
{
    fresh();
    std::string log;
    for (int i = 0; i < 100; i++) log += "1791500000,E1_" + std::to_string(i) + ",SUSPECT,-1.0,400,0.0,100,0,83\n";
    fakehal.files["ezsn/events.csv"] = log;
    String t = readEventLogTail(3);
    CHECK(t.startsWith("1791500000,E1_"));
    CHECK(t.endsWith("E1_99,SUSPECT,-1.0,400,0.0,100,0,83\n"));
    CHECK(t.length() <= 3 * 120);
    sdOk = false;
    CHECK_EQ(readEventLogTail(3), String("SD not available"));
}

MINITEST_MAIN
