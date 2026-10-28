// File system on the upper 16 MB of a 32 MB flash: LittleFS 2.4.1 through the HAL (FW-08, FW-20, FW-02)
// @extract firmware/sentinel_node/ezsn_hal.cpp: g_lfs g_lfscfg g_fsMounted g_fsLock g_fsOp ezfs_bd_read ezfs_bd_prog ezfs_bd_erase ezfs_bd_sync ezfs_lock ezfs_unlock ezfs_config ezfs_blank ezfs_locks ezhal_fs_mount_region ezhal_fs_unmount ezhal_fs_mounted ezhal_fs_format ezfs_write ezfs_mkparents ezhal_fs_write_file ezhal_fs_append ezhal_fs_read ezhal_fs_size ezhal_fs_exists ezhal_fs_remove ezhal_fs_rename ezhal_fs_mkdir ezhal_fs_list ezhal_fs_usage
// @csource tests/fw_host/third_party/littlefs/lfs.c
// @csource tests/fw_host/third_party/littlefs/lfs_util.c
// @cflags -DLFS_THREADSAFE -DLFS_NO_DEBUG -fexceptions -DEZ_REAL_FS
#include "minitest.h"
#include "fw_env.h"
#include "lfs.h"
#include "extracted.inc"
#include <algorithm>

static void fresh32()
{
    fake_reset_all();
    fakehal.flash_size = 0x2000000;
    g_fsMounted = false;
    memset(&g_lfs, 0, sizeof(g_lfs));
}

// power-on after a cut: RAM state is gone, flash is kept
static void reboot()
{
    fakehal.flash_budget = -1;
    g_fsMounted = false;
    memset(&g_lfs, 0, sizeof(g_lfs));
    g_lock_depth = 0;
}

static String mount(bool fmt = true)
{
    String d;
    ezhal_fs_mount_region(fmt, &d);
    return d;
}

static std::string blob(int seed, size_t n)
{
    std::string s(n, '\0');
    for (size_t i = 0; i < n; i++) s[i] = (char)((i * 131 + seed * 7 + (i >> 8)) & 0xFF);
    return s;
}

static std::string readall(const char *path)
{
    int32_t n = ezhal_fs_size(path);
    if (n < 0) return "<missing>";
    std::string s(n, '\0');
    if (n && ezhal_fs_read(path, 0, (uint8_t *)&s[0], n) != n) return "<read error>";
    return s;
}

static bool put(const char *path, const std::string &d) { return ezhal_fs_write_file(path, (const uint8_t *)d.data(), d.size()) == HS_OK; }

TEST(no_file_system_without_32mb, "FW-08,FW-02")
{
    fresh32();
    fakehal.flash_size = 0x1000000;                 // 16 MB board: region not accessible
    String d;
    CHECK_EQ(ezhal_fs_mount_region(true, &d), HS_ERROR);
    CHECK(!ezhal_fs_mounted());
    CHECK(fakehal.erase_counts.empty());            // nothing was written anywhere
}

TEST(blank_region_is_formatted_once, "FW-08")
{
    fresh32();
    String d = mount();
    CHECK(ezhal_fs_mounted());
    CHECK_CONTAINS(d, "formatted new file system");
    CHECK_CONTAINS(d, "mounted 16192 KB");
    CHECK(put("/a.txt", "hello"));
    ezhal_fs_unmount();
    d = mount();
    CHECK(d.indexOf("formatted") < 0);             // existing file system is mounted, not reformatted
    CHECK_EQ(readall("/a.txt"), std::string("hello"));
}

TEST(file_operations, "FW-08")
{
    fresh32();
    mount();
    CHECK(put("/img/2026/E1_1.jpg", blob(1, 30000)));          // parent directories created
    CHECK_EQ(ezhal_fs_size("/img/2026/E1_1.jpg"), 30000);
    CHECK(ezhal_fs_exists("/img/2026"));
    CHECK_EQ(ezhal_fs_size("/img/2026"), -1);                  // a directory has no file size
    CHECK_EQ(ezhal_fs_append("/log/events.csv", (const uint8_t *)"l1\n", 3), HS_OK);
    CHECK_EQ(ezhal_fs_append("/log/events.csv", (const uint8_t *)"l2\n", 3), HS_OK);
    CHECK_EQ(readall("/log/events.csv"), std::string("l1\nl2\n"));
    uint8_t b[4];
    CHECK_EQ(ezhal_fs_read("/log/events.csv", 3, b, 4), 3);   // read from an offset, short at the end
    CHECK(memcmp(b, "l2\n", 3) == 0);
    String ls;
    CHECK_EQ(ezhal_fs_list("/", &ls), HS_OK);
    CHECK_CONTAINS(ls, "img/ -");
    CHECK_CONTAINS(ls, "log/ -");
    CHECK_EQ(ezhal_fs_rename("/log/events.csv", "/log/old.csv"), HS_OK);
    CHECK(!ezhal_fs_exists("/log/events.csv"));
    CHECK_EQ(ezhal_fs_remove("/log"), HS_ERROR);               // not empty
    CHECK_EQ(ezhal_fs_remove("/log/old.csv"), HS_OK);
    CHECK_EQ(ezhal_fs_remove("/log"), HS_OK);
    CHECK_EQ(ezhal_fs_read("/nope", 0, b, 4), -1);
    CHECK_EQ(ezhal_fs_size("/nope"), -1);
    CHECK_EQ(fakehal.nor_violations, 0);
}

TEST(large_file_and_usage, "FW-08")
{
    fresh32();
    mount();
    uint32_t u0, total;
    ezhal_fs_usage(&u0, &total);
    std::string big = blob(9, 2 * 1024 * 1024);                // 2 MB
    CHECK(put("/big.bin", big));
    uint32_t u1;
    ezhal_fs_usage(&u1, &total);
    CHECK(u1 >= u0 + 2 * 1024 * 1024);
    CHECK(readall("/big.bin") == big);
    ezhal_fs_unmount();
    mount();
    CHECK(readall("/big.bin") == big);                         // survives remount
    CHECK_EQ(ezhal_fs_remove("/big.bin"), HS_OK);
    uint32_t u2;
    ezhal_fs_usage(&u2, &total);
    CHECK(u2 < u1);
    CHECK_EQ(total, 4048u * 4096u);
}

TEST(replace_is_atomic_under_power_cuts, "FW-08,FW-20")
{
    std::string v1 = blob(1, 6000), v2 = blob(2, 7000);
    int cuts = 0;
    for (long budget = 0; budget < 9000; budget += 61) {
        fresh32();
        mount();
        put("/cfg/settings.bin", v1);
        fakehal.flash_budget = budget;
        try { put("/cfg/settings.bin", v2); } catch (PowerLoss &) { cuts++; }
        reboot();
        String d = mount(false);
        CHECK(ezhal_fs_mounted());
        std::string got = readall("/cfg/settings.bin");
        bool old_or_new = (got == v1) || (got == v2);
        if (!old_or_new) minitest::fail(__FILE__, __LINE__, "budget " + std::to_string(budget) + ": partial file, size " + std::to_string(got.size()));
        CHECK(put("/cfg/other.bin", "still writable"));
    }
    CHECK(cuts > 50);
    CHECK_EQ(fakehal.nor_violations, 0);
}

TEST(append_survives_power_cuts, "FW-08,FW-20")
{
    std::string base = "line-0\n", add = blob(3, 3000);
    for (long budget = 0; budget < 4000; budget += 53) {
        fresh32();
        mount();
        ezhal_fs_append("/log.csv", (const uint8_t *)base.data(), base.size());
        fakehal.flash_budget = budget;
        try { ezhal_fs_append("/log.csv", (const uint8_t *)add.data(), add.size()); } catch (PowerLoss &) {}
        reboot();
        mount(false);
        std::string got = readall("/log.csv");
        CHECK(got == base || got == base + add);               // old content never damaged
    }
}

TEST(full_file_system_fails_cleanly, "FW-08")
{
    fresh32();
    mount();
    std::string chunk = blob(5, 1024 * 1024);
    int n = 0;
    while (n < 40 && put(("/fill/" + std::to_string(n)).c_str(), chunk)) n++;
    CHECK(n >= 14 && n < 17);                                  // ~15.8 MB region
    CHECK(!put("/fill/more", chunk));                          // NOSPC reported, no crash
    CHECK(readall("/fill/0") == chunk);                        // existing files intact
    CHECK_EQ(ezhal_fs_remove("/fill/0"), HS_OK);
    CHECK(put("/fill/more", chunk));                           // space is reusable
}

TEST(damaged_file_system_is_not_auto_formatted, "FW-08,FW-20")
{
    fresh32();
    mount();
    put("/keep.bin", blob(4, 10000));
    ezhal_fs_unmount();
    for (uint32_t blk = 0; blk < 2; blk++) {                   // destroy both superblocks
        auto &v = fakehal.flash[EZHAL_FS_BASE + blk * 4096];
        for (int i = 0; i < 64; i++) v[i] = 0x00;
    }
    auto before = fakehal.erase_counts;
    String d;
    CHECK_EQ(ezhal_fs_mount_region(true, &d), HS_ERROR);
    CHECK_CONTAINS(d, "FS_FORMAT YES");
    CHECK(fakehal.erase_counts == before);                     // nothing erased: data left for recovery
    CHECK_EQ(ezhal_fs_format(), HS_OK);                        // explicit format works
    CHECK(ezhal_fs_mounted());
    CHECK_EQ(ezhal_fs_size("/keep.bin"), -1);
}

TEST(wear_is_spread, "FW-08")
{
    fresh32();
    mount();
    for (int i = 0; i < 3000; i++) put("/counter.bin", blob(i, 1000));
    int mx = 0, used = 0;
    for (auto &e : fakehal.erase_counts) {
        if (e.first < EZHAL_FS_BASE) continue;
        mx = std::max(mx, e.second);
        used++;
    }
    CHECK(used > 100);                                         // rewrites rotate over many blocks
    CHECK(mx <= 520);                                          // block_cycles = 500 bounds metadata wear
    CHECK_EQ(readall("/counter.bin"), blob(2999, 1000));
    CHECK_EQ(fakehal.nor_violations, 0);
}

TEST(invalid_paths_rejected, "FW-08")
{
    fresh32();
    mount();
    std::string longname = "/" + std::string(260, 'n');
    CHECK(!put(longname.c_str(), "x"));
    CHECK_EQ(ezhal_fs_write_file(NULL, (const uint8_t *)"x", 1), HS_ERROR);
    ezhal_fs_unmount();
    CHECK(!put("/a", "x"));                                    // not mounted
}

MINITEST_MAIN
