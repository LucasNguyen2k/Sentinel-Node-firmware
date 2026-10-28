// HAL flash data storage: range guard, A/B record store, append-only logs, power-cut safety, wear (FW-08, FW-20, FW-21, FW-02)
// @extract firmware/sentinel_node/ezsn_hal.cpp: EZREC_MAGIC EZLOG_MAGIC EZLOG_HDR EZLOG_RHDR EZLOG_F_COMMIT EZLOG_F_DONE EZLOG_MAX_SECTORS EzRecHeader EzLogSectorHdr EzLogRecHdr EzLogRegion EzLogState ezhal_flog_regions ezhal_flog_state ezhal_crc32 ezhal_flash_range_ok ezhal_frec_slot_valid ezhal_frec_read ezhal_frec_write ezlog_rec_size ezhal_flog_addr ezhal_flog_rec_at ezhal_flog_next_used ezhal_flog_init ezhal_flog_new_sector ezhal_flog_append ezhal_flog_peek ezhal_flog_consume ezhal_flog_pending ezhal_flog_read_recent ezhal_flog_stats ezhal_flog_format ezhal_flash_selftest
#include "minitest.h"
#include "fw_env.h"
#include "extracted.inc"
#include <algorithm>

static void fresh()
{
    fake_reset_all();
    for (int i = 0; i < EZFLOG_COUNT; i++) ezhal_flog_init(i);
}
static std::string mk(int i, size_t len = 40)
{
    std::string s = "rec" + std::to_string(i) + ":";
    while (s.size() < len) s += (char)('a' + (i + s.size()) % 26);
    return s;
}
static bool append(int log, const std::string &s) { return ezhal_flog_append(log, (const uint8_t *)s.data(), (uint16_t)s.size()) == HS_OK; }
static std::string peek(int log)
{
    uint8_t buf[4096]; uint16_t len = 0;
    if (ezhal_flog_peek(log, buf, sizeof(buf), &len) != HS_OK) return "<none>";
    return std::string((const char *)buf, len);
}
static std::vector<std::string> drain(int log, size_t max = 100000)
{
    std::vector<std::string> out;
    while (out.size() < max) {
        std::string p = peek(log);
        if (p == "<none>") break;
        out.push_back(p);
        ezhal_flog_consume(log);
    }
    return out;
}
static void no_nor_violation() { CHECK_EQ(fakehal.nor_violations, 0); }

// ---------------------------------------------------------------- guard
TEST(range_guard_protects_firmware_area, "FW-02,FW-20")
{
    fresh();
    CHECK(ezhal_flash_range_ok(0xFD0000, 4096));
    CHECK(ezhal_flash_range_ok(0xFFF000, 4096));
    CHECK(!ezhal_flash_range_ok(0x000000, 1));
    CHECK(!ezhal_flash_range_ok(0xFCF000, 4096));            // last sector before the user area
    CHECK(!ezhal_flash_range_ok(0xFCFFFF, 2));               // straddles the boundary
    CHECK(!ezhal_flash_range_ok(0xFFF000, 4097));            // runs past the end of flash
    CHECK(!ezhal_flash_range_ok(0xFFFFFFFFu, 2));            // wrap-around attempt
}

TEST(layout_fits_user_area, "FW-08")
{
    CHECK_EQ(EZHAL_FLASH_LOG_EVENTS_BASE + EZHAL_FLASH_LOG_EVENTS_SECTORS * HAL_FLASH_SECTOR, EZHAL_FLASH_LOG_BUFFER_BASE);
    CHECK_EQ(EZHAL_FLASH_LOG_BUFFER_BASE + EZHAL_FLASH_LOG_BUFFER_SECTORS * HAL_FLASH_SECTOR, EZHAL_FLASH_USER_END);
    CHECK(EZHAL_FLASH_LOG_BUFFER_SECTORS <= EZLOG_MAX_SECTORS);
    std::vector<uint32_t> fixed = {EZHAL_FLASH_CFG_A, EZHAL_FLASH_BOOTINFO, EZHAL_FLASH_CFG_B, EZHAL_FLASH_SCRATCH};
    for (auto a : fixed) CHECK(a < EZHAL_FLASH_LOG_EVENTS_BASE && a >= EZHAL_FLASH_USER_BASE);
}

// ---------------------------------------------------------------- A/B record store
TEST(record_store_round_trip_and_alternation, "FW-08,FW-21")
{
    fresh();
    char buf[64] = {0};
    uint32_t seq = 0;
    CHECK_EQ(ezhal_frec_read(EZHAL_FLASH_CFG_A, EZHAL_FLASH_CFG_B, buf, 64, &seq), HS_NOT_PRESENT);
    char v1[64] = "version-1", v2[64] = "version-2", v3[64] = "version-3";
    CHECK_EQ(ezhal_frec_write(EZHAL_FLASH_CFG_A, EZHAL_FLASH_CFG_B, v1, 64), HS_OK);
    CHECK_EQ(fakehal.erase_counts[EZHAL_FLASH_CFG_B], 1);     // first write goes to B (A may hold a legacy record)
    CHECK_EQ(fakehal.erase_counts[EZHAL_FLASH_CFG_A], 0);
    ezhal_frec_write(EZHAL_FLASH_CFG_A, EZHAL_FLASH_CFG_B, v2, 64);
    ezhal_frec_write(EZHAL_FLASH_CFG_A, EZHAL_FLASH_CFG_B, v3, 64);
    CHECK_EQ(fakehal.erase_counts[EZHAL_FLASH_CFG_A], 1);
    CHECK_EQ(fakehal.erase_counts[EZHAL_FLASH_CFG_B], 2);
    CHECK_EQ(ezhal_frec_read(EZHAL_FLASH_CFG_A, EZHAL_FLASH_CFG_B, buf, 64, &seq), HS_OK);
    CHECK_EQ(std::string(buf), std::string("version-3"));
    CHECK_EQ(seq, 3u);
    no_nor_violation();
}

TEST(record_store_survives_power_cut_at_every_byte, "FW-08,FW-20")
{
    char v1[100], v2[100];
    memset(v1, '1', 100); memset(v2, '2', 100);
    int cuts = 0;
    for (long budget = 0; budget < 16 + 116 + 8; budget++) {   // erase chunks + payload + header
        fresh();
        ezhal_frec_write(EZHAL_FLASH_CFG_A, EZHAL_FLASH_CFG_B, v1, 100);
        fakehal.flash_budget = budget;
        try { ezhal_frec_write(EZHAL_FLASH_CFG_A, EZHAL_FLASH_CFG_B, v2, 100); } catch (PowerLoss &) { cuts++; }
        fakehal.flash_budget = -1;
        char got[100];
        CHECK_EQ(ezhal_frec_read(EZHAL_FLASH_CFG_A, EZHAL_FLASH_CFG_B, got, 100, NULL), HS_OK);
        bool old_or_new = memcmp(got, v1, 100) == 0 || memcmp(got, v2, 100) == 0;
        CHECK(old_or_new);
        CHECK_EQ(ezhal_frec_write(EZHAL_FLASH_CFG_A, EZHAL_FLASH_CFG_B, v2, 100), HS_OK);   // device recovers
    }
    CHECK(cuts > 100);
    no_nor_violation();
}

TEST(record_store_falls_back_to_older_copy, "FW-08,FW-20")
{
    fresh();
    char a[32] = "older", b[32] = "newer", got[32];
    ezhal_frec_write(EZHAL_FLASH_CFG_A, EZHAL_FLASH_CFG_B, a, 32);     // -> B
    ezhal_frec_write(EZHAL_FLASH_CFG_A, EZHAL_FLASH_CFG_B, b, 32);     // -> A
    fakehal.flash[EZHAL_FLASH_CFG_A][20] &= 0x01;                       // bit rot in the newest copy (payload byte)
    CHECK_EQ(ezhal_frec_read(EZHAL_FLASH_CFG_A, EZHAL_FLASH_CFG_B, got, 32, NULL), HS_OK);
    CHECK_EQ(std::string(got), std::string("older"));
    fakehal.flash[EZHAL_FLASH_CFG_B][20] &= 0x01;
    CHECK_EQ(ezhal_frec_read(EZHAL_FLASH_CFG_A, EZHAL_FLASH_CFG_B, got, 32, NULL), HS_NOT_PRESENT);
    CHECK_EQ(ezhal_frec_write(EZHAL_FLASH_CFG_A, EZHAL_FLASH_CFG_B, a, 4093), HS_ERROR);   // too large for a sector
}

// ---------------------------------------------------------------- logs
TEST(log_is_fifo_and_persistent_across_reboot, "FW-08")
{
    fresh();
    for (int i = 0; i < 300; i++) CHECK(append(EZFLOG_BUFFER, mk(i, 20 + i % 60)));
    CHECK_EQ(ezhal_flog_pending(EZFLOG_BUFFER), 300u);
    auto first = drain(EZFLOG_BUFFER, 120);
    CHECK_EQ(first.size(), (size_t)120);
    for (int i = 0; i < 120 && i < (int)first.size(); i++) CHECK_EQ(first[i], mk(i, 20 + i % 60));
    ezhal_flog_init(EZFLOG_BUFFER);                                    // reboot
    CHECK_EQ(ezhal_flog_pending(EZFLOG_BUFFER), 180u);
    auto rest = drain(EZFLOG_BUFFER);
    CHECK_EQ(rest.size(), (size_t)180);
    if (!rest.empty()) CHECK_EQ(rest[0], mk(120, 20 + 120 % 60));       // no record sent twice
    CHECK_EQ(peek(EZFLOG_BUFFER), std::string("<none>"));
    no_nor_violation();
}

TEST(full_ring_drops_oldest_and_counts_it, "FW-08")
{
    fresh();
    EzFlashLogStats st;
    const int N = 3000;                                                // ~1.3x the ring's capacity
    for (int i = 0; i < N; i++) append(EZFLOG_BUFFER, mk(i, 56));
    ezhal_flog_stats(EZFLOG_BUFFER, &st);
    CHECK(st.dropped > 0);
    CHECK_EQ(st.pending + st.dropped, (uint32_t)N);
    std::string oldest = peek(EZFLOG_BUFFER);
    CHECK_EQ(oldest, mk((int)st.dropped, 56));                          // oldest surviving is next after the dropped ones
    ezhal_flog_init(EZFLOG_BUFFER);
    CHECK_EQ(ezhal_flog_pending(EZFLOG_BUFFER), st.pending);
    auto all = drain(EZFLOG_BUFFER);
    CHECK_EQ(all.size(), (size_t)st.pending);
    if (!all.empty()) CHECK_EQ(all.back(), mk(N - 1, 56));
    no_nor_violation();
}

TEST(capacity_matches_design, "FW-08")
{
    fresh();
    EzFlashLogStats ev, bf;
    ezhal_flog_stats(EZFLOG_EVENTS, &ev);
    ezhal_flog_stats(EZFLOG_BUFFER, &bf);
    CHECK_EQ(ev.capacity, 8u * 4080u);
    CHECK_EQ(bf.capacity, 36u * 4080u);
    int n = 0;
    while (true) {                                                      // 56-byte records = one offline sample
        EzFlashLogStats s;
        append(EZFLOG_BUFFER, mk(n, 56));
        ezhal_flog_stats(EZFLOG_BUFFER, &s);
        if (s.dropped) break;
        n++;
    }
    CHECK(n >= 2200);                                                   // ~37 min at one record per second
}

TEST(wear_is_spread_evenly, "FW-08")
{
    fresh();
    for (int i = 0; i < 36 * 63 * 10; i++) append(EZFLOG_BUFFER, mk(i, 56));   // about ten trips round the ring
    int mn = 1 << 30, mx = 0;
    for (int k = 0; k < EZHAL_FLASH_LOG_BUFFER_SECTORS; k++) {
        int c = fakehal.erase_counts[EZHAL_FLASH_LOG_BUFFER_BASE + k * HAL_FLASH_SECTOR];
        mn = std::min(mn, c); mx = std::max(mx, c);
    }
    CHECK(mx - mn <= 1);
    EzFlashLogStats st;
    ezhal_flog_stats(EZFLOG_BUFFER, &st);
    CHECK_EQ(st.max_erase, (uint32_t)mx);
}

TEST(power_cut_during_append_never_corrupts, "FW-08,FW-20")
{
    std::string x = mk(999, 50);
    int cuts = 0;
    for (long budget = 0; budget < 8 + 52 + 1 + 2; budget++) {        // header + data + commit flag
        fresh();
        for (int i = 0; i < 5; i++) append(EZFLOG_BUFFER, mk(i));
        fakehal.flash_budget = budget;
        bool done = false;
        try { done = append(EZFLOG_BUFFER, x); } catch (PowerLoss &) { cuts++; }
        fakehal.flash_budget = -1;
        ezhal_flog_init(EZFLOG_BUFFER);                                // reboot
        auto got = drain(EZFLOG_BUFFER);
        CHECK(got.size() == 5 || got.size() == 6);
        for (int i = 0; i < 5 && i < (int)got.size(); i++) CHECK_EQ(got[i], mk(i));
        if (got.size() == 6) CHECK_EQ(got[5], x);                      // all of it, or nothing
        if (done) CHECK_EQ(got.size(), (size_t)6);
        CHECK(append(EZFLOG_BUFFER, mk(7)));                           // log keeps working
        CHECK_EQ(peek(EZFLOG_BUFFER), mk(7));
    }
    CHECK(cuts > 50);
    no_nor_violation();
}

TEST(power_cut_while_starting_new_sector, "FW-08,FW-20")
{
    for (long budget = 0; budget < 16 + 16 + 2; budget++) {             // erase chunks + sector header
        fresh();
        int n = 0;
        EzFlashLogStats st;
        do { append(EZFLOG_BUFFER, mk(n++, 56)); ezhal_flog_stats(EZFLOG_BUFFER, &st); }
        while (fakehal.erase_counts.size() < 2);                       // second sector just started
        // fill the second sector, then cut power while the third one is prepared
        while (true) {
            EzLogState &s = ezhal_flog_state[EZFLOG_BUFFER];
            if (s.head_off + 64 > HAL_FLASH_SECTOR) break;
            append(EZFLOG_BUFFER, mk(n++, 56));
        }
        fakehal.flash_budget = budget;
        try { append(EZFLOG_BUFFER, mk(n, 56)); } catch (PowerLoss &) {}
        fakehal.flash_budget = -1;
        ezhal_flog_init(EZFLOG_BUFFER);
        auto got = drain(EZFLOG_BUFFER);
        CHECK(got.size() == (size_t)n || got.size() == (size_t)n + 1);
        for (int i = 0; i < n && i < (int)got.size(); i++) CHECK_EQ(got[i], mk(i, 56));
        CHECK(append(EZFLOG_BUFFER, mk(5000, 56)));
        CHECK_EQ(peek(EZFLOG_BUFFER), mk(5000, 56));
    }
    no_nor_violation();
}

TEST(power_cut_during_consume_is_at_least_once, "FW-08,FW-20")
{
    for (long budget = 0; budget < 2; budget++) {
        fresh();
        for (int i = 0; i < 3; i++) append(EZFLOG_BUFFER, mk(i));
        peek(EZFLOG_BUFFER);
        fakehal.flash_budget = budget;
        try { ezhal_flog_consume(EZFLOG_BUFFER); } catch (PowerLoss &) {}
        fakehal.flash_budget = -1;
        ezhal_flog_init(EZFLOG_BUFFER);
        auto got = drain(EZFLOG_BUFFER);
        CHECK(got.size() == 2 || got.size() == 3);                     // resent at most once, never lost
        if (!got.empty()) CHECK_EQ(got.back(), mk(2));
    }
}

TEST(corrupt_record_skipped_and_counted, "FW-08,FW-13")
{
    fresh();
    for (int i = 0; i < 3; i++) append(EZFLOG_BUFFER, mk(i));
    uint32_t data_of_second = EZHAL_FLASH_LOG_BUFFER_BASE + 16 + ezlog_rec_size(40) + 8 + 5;
    fakehal.flash[EZHAL_FLASH_LOG_BUFFER_BASE][data_of_second - EZHAL_FLASH_LOG_BUFFER_BASE] &= 0x01;   // bit rot
    auto got = drain(EZFLOG_BUFFER);
    CHECK_EQ(got.size(), (size_t)2);
    if (got.size() == 2) { CHECK_EQ(got[0], mk(0)); CHECK_EQ(got[1], mk(2)); }
    EzFlashLogStats st;
    ezhal_flog_stats(EZFLOG_BUFFER, &st);
    CHECK_EQ(st.corrupt, 1u);
}

TEST(read_recent_returns_newest_in_order, "FW-08,FW-18")
{
    fresh();
    for (int i = 0; i < 1000; i++) append(EZFLOG_EVENTS, "1791500000,E1_" + std::to_string(i) + ",SUSPECT");   // wraps the ring
    String out;
    CHECK_EQ(ezhal_flog_read_recent(EZFLOG_EVENTS, 3, &out), HS_OK);
    CHECK_EQ(out, String("1791500000,E1_997,SUSPECT\n1791500000,E1_998,SUSPECT\n1791500000,E1_999,SUSPECT"));
    ezhal_flog_init(EZFLOG_EVENTS);
    ezhal_flog_read_recent(EZFLOG_EVENTS, 1, &out);
    CHECK_EQ(out, String("1791500000,E1_999,SUSPECT"));
}

TEST(size_limits_and_format, "FW-08")
{
    fresh();
    std::string max(4072, 'm'), over(4073, 'o');
    CHECK_EQ(ezhal_flog_append(EZFLOG_BUFFER, (const uint8_t *)"", 0), HS_ERROR);
    CHECK_EQ(ezhal_flog_append(EZFLOG_BUFFER, (const uint8_t *)over.data(), 4073), HS_ERROR);
    CHECK(append(EZFLOG_BUFFER, max));
    CHECK_EQ(peek(EZFLOG_BUFFER), max);
    CHECK_EQ(ezhal_flog_format(EZFLOG_BUFFER), HS_OK);
    CHECK_EQ(ezhal_flog_pending(EZFLOG_BUFFER), 0u);
    CHECK_EQ(peek(EZFLOG_BUFFER), std::string("<none>"));
    CHECK(append(EZFLOG_BUFFER, mk(1)));
}

TEST(selftest_on_scratch_sector, "FW-22,FW-02")
{
    fresh();
    String d;
    CHECK_EQ(ezhal_flash_selftest(&d), HS_OK);
    CHECK_CONTAINS(d, "nor_and=yes");
    CHECK_CONTAINS(d, "guard=ok");
    CHECK(fakehal.erase_counts[EZHAL_FLASH_SCRATCH] >= 1);
    CHECK_EQ(fakehal.erase_counts.size(), (size_t)1);                  // only the scratch sector was touched
}

MINITEST_MAIN
