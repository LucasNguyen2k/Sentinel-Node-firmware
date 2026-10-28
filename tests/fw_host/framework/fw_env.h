// fw_env.h - common host environment for extracted firmware code:
// Arduino mocks, FreeRTOS primitives, the REAL ezsn_hal.h interface, and a controllable fake HAL.
#pragma once
#include "Arduino.h"
#include "IPAddress.h"
#include <map>
#include <deque>
#include <string>
#include <vector>

uint32_t g_fake_millis = 1000;
FakeSerial Serial;

// ---------------------------------------------------------------- FreeRTOS
typedef void *SemaphoreHandle_t;
#define portMAX_DELAY 0xFFFFFFFFu
static int g_lock_depth = 0, g_lock_max = 0;
inline SemaphoreHandle_t xSemaphoreCreateMutex() { static int m; return &m; }
inline int xSemaphoreTake(SemaphoreHandle_t, uint32_t) { if (++g_lock_depth > g_lock_max) g_lock_max = g_lock_depth; return 1; }
inline int xSemaphoreGive(SemaphoreHandle_t) { g_lock_depth--; return 1; }
static int g_critical = 0;
#define taskENTER_CRITICAL() (g_critical++)
#define taskEXIT_CRITICAL() (g_critical--)

// ---------------------------------------------------------------- real HAL interface
#include "ezsn_hal.h"

// ---------------------------------------------------------------- fake HAL (state is public for tests)
struct FakeHal {
    // sensors
    int gas_raw = 400; HalStatus gas_status = HS_OK; bool gas_present = true;
    float env_t = 21.5f, env_rh = 55.0f; HalStatus env_status = HS_OK; bool env_present = true;
    bool us_present = false; float us_level = -1;
    bool power_present = false; HalPowerStatus power{};
    bool tamper_present = false; HalTamperStatus tamper{};
    int32_t thermal_raw = 105; HalStatus thermal_status = HS_OK;
    bool mic_present = true;
    // camera
    bool cam_running[4] = {false, false, false, false}; int cam_starts = 0, cam_stops = 0;
    HalStatus snapshot_status = HS_OK; std::vector<uint8_t> jpeg = std::vector<uint8_t>(5000, 0xAB);
    // SD card: path -> content
    bool sd_present = true; std::map<std::string, std::string> files; int ota_starts = 0; HalStatus ota_start_status = HS_OK;
    bool sd_fail_writes = false;
    // flash: sector address -> 4 KB content (erased = 0xFF)
    std::map<uint32_t, std::vector<uint8_t>> flash; int flash_writes = 0; bool flash_verify_fail = false;
    // NOR flash model: program can only clear bits; power-cut injection; wear accounting
    uint32_t flash_size = 0x1000000;   // 16 MB board unless a test says otherwise
    long flash_budget = -1;            // bytes (program) / 256-byte chunks (erase) before a power cut; -1 = never
    int nor_violations = 0;            // program attempts that needed a 0 -> 1 change (would need an erase)
    std::map<uint32_t, int> erase_counts;
    // radio
    bool connected = true; int32_t rssi = -40; uint8_t mac[6] = {0x00, 0xE0, 0x4C, 0x0F, 0x5C, 0x9D};
    IPAddress ip = IPAddress(192, 168, 100, 44); bool resolve_ok = true; int radio_begins = 0;
    // tcp (scripted)
    int next_sock = 7; int connect_result = 7; std::deque<std::string> rx; std::deque<int> recv_codes;
    std::vector<std::string> sent; int send_fail_after = -1; int closed = 0; int last_closed = -1; int so_error = 0;
    uint32_t ntp_epoch = 0;
    // system
    int wdt_refresh = 0; bool reset_called = false; uint32_t heap = 72800000, heap_min = 72790000;
};
FakeHal fakehal;

static std::vector<uint8_t> &flash_sector(uint32_t a)
{
    auto &v = fakehal.flash[a];
    if (v.empty()) v.assign(HAL_FLASH_SECTOR, 0xFF);
    return v;
}

bool ezhal_gas_init() { return fakehal.gas_present; }
bool ezhal_gas_present() { return fakehal.gas_present; }
HalStatus ezhal_gas_read_raw(int *raw) { if (!fakehal.gas_present) return HS_NOT_PRESENT; *raw = fakehal.gas_raw; return fakehal.gas_status; }
float ezhal_gas_raw_to_volts(float raw) { return raw * HAL_GAS_VREF / (float)HAL_GAS_ADC_MAX; }
bool ezhal_env_init() { return fakehal.env_present; }
bool ezhal_env_present() { return fakehal.env_present; }
#ifndef EZ_REAL_ENV
HalStatus ezhal_env_read(float *t, float *rh) { if (fakehal.env_status != HS_OK) return fakehal.env_status; *t = fakehal.env_t; *rh = fakehal.env_rh; return HS_OK; }
#endif
bool ezhal_us_init() { return fakehal.us_present; }
bool ezhal_us_present() { return fakehal.us_present; }
HalStatus ezhal_us_read(float *l) { if (!fakehal.us_present) { *l = -1; return HS_NOT_PRESENT; } *l = fakehal.us_level; return HS_OK; }
bool ezhal_power_init() { return fakehal.power_present; }
bool ezhal_power_present() { return fakehal.power_present; }
HalStatus ezhal_power_read(HalPowerStatus *s) { *s = fakehal.power; s->present = fakehal.power_present; return fakehal.power_present ? HS_OK : HS_NOT_PRESENT; }
bool ezhal_tamper_init() { return fakehal.tamper_present; }
bool ezhal_tamper_present() { return fakehal.tamper_present; }
HalStatus ezhal_tamper_read(HalTamperStatus *s) { *s = fakehal.tamper; s->present = fakehal.tamper_present; return fakehal.tamper_present ? HS_OK : HS_NOT_PRESENT; }
HalStatus ezhal_temp_internal_raw(int32_t *raw) { if (fakehal.thermal_status == HS_OK) *raw = fakehal.thermal_raw; return fakehal.thermal_status; }
bool ezhal_mic_tap_init(HalMicFrameCallback) { return fakehal.mic_present; }
bool ezhal_mic_present() { return fakehal.mic_present; }
HalStatus ezhal_cam_start(int ch, uint32_t) { if (!fakehal.cam_running[ch]) { fakehal.cam_running[ch] = true; fakehal.cam_starts++; } return HS_OK; }
HalStatus ezhal_cam_stop(int ch) { if (fakehal.cam_running[ch]) { fakehal.cam_running[ch] = false; fakehal.cam_stops++; } return HS_OK; }
bool ezhal_cam_running(int ch) { return fakehal.cam_running[ch]; }
uint32_t ezhal_cam_timeouts() { return 0; }
bool ezhal_cam_stuck() { return false; }
HalStatus ezhal_cam_snapshot(int ch, uint32_t *addr, uint32_t *len)
{
    *addr = 0; *len = 0;
    if (!fakehal.cam_running[ch] || fakehal.snapshot_status != HS_OK) return HS_ERROR;
    *addr = (uint32_t)(uintptr_t)1;   // non-zero marker; tests never dereference it
    *len = (uint32_t)fakehal.jpeg.size();
    return HS_OK;
}

bool ezhal_sd_mount() { return fakehal.sd_present; }
bool ezhal_sd_present() { return fakehal.sd_present; }
HalStatus ezhal_sd_mkdir(const String &) { return fakehal.sd_present ? HS_OK : HS_NOT_PRESENT; }
bool ezhal_sd_exists(const String &r) { return fakehal.sd_present && fakehal.files.count(r.s); }
HalStatus ezhal_sd_remove(const String &r) { if (!fakehal.sd_present) return HS_NOT_PRESENT; return fakehal.files.erase(r.s) ? HS_OK : HS_ERROR; }
HalStatus ezhal_sd_rename(const String &a, const String &b)
{
    if (!fakehal.sd_present) return HS_NOT_PRESENT;
    auto it = fakehal.files.find(a.s);
    if (it == fakehal.files.end() || fakehal.files.count(b.s)) return HS_ERROR;
    fakehal.files[b.s] = it->second; fakehal.files.erase(it); return HS_OK;
}
int32_t ezhal_sd_size(const String &r) { if (!ezhal_sd_exists(r)) return -1; return (int32_t)fakehal.files[r.s].size(); }
HalStatus ezhal_sd_append(const String &r, const uint8_t *d, uint32_t n)
{
    if (!fakehal.sd_present) return HS_NOT_PRESENT;
    if (fakehal.sd_fail_writes) return HS_ERROR;
    fakehal.files[r.s].append((const char *)d, n); return HS_OK;
}
HalStatus ezhal_sd_write(const String &r, const uint8_t *d, uint32_t n)
{
    if (!fakehal.sd_present) return HS_NOT_PRESENT;
    if (fakehal.sd_fail_writes) return HS_ERROR;
    fakehal.files[r.s] = std::string((const char *)d, n); return HS_OK;
}
int32_t ezhal_sd_read_at(const String &r, uint32_t off, uint8_t *buf, uint32_t n)
{
    if (!ezhal_sd_exists(r)) return -1;
    auto &f = fakehal.files[r.s];
    if (off >= f.size()) return 0;
    uint32_t k = std::min<uint32_t>(n, (uint32_t)f.size() - off);
    memcpy(buf, f.data() + off, k); return (int32_t)k;
}
HalStatus ezhal_sd_read_line(const String &r, uint32_t off, String *line, uint32_t *next)
{
    if (!fakehal.sd_present) return HS_NOT_PRESENT;
    if (!fakehal.files.count(r.s)) return HS_ERROR;
    auto &f = fakehal.files[r.s];
    if (off >= f.size()) return HS_ERROR;
    size_t e = f.find('\n', off);
    *line = String(f.substr(off, (e == std::string::npos ? f.size() : e) - off));
    *next = (uint32_t)(e == std::string::npos ? f.size() : e + 1);
    return HS_OK;
}
HalStatus ezhal_sd_start_ota(const String &r) { fakehal.ota_starts++; if (!ezhal_sd_exists(r)) return HS_NOT_PRESENT; return fakehal.ota_start_status; }
HalStatus ezhal_flash_read(uint32_t addr, void *buf, uint32_t len)
{
    uint8_t *o = (uint8_t *)buf;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t a = addr + i, sec = a - (a % HAL_FLASH_SECTOR);
        o[i] = flash_sector(sec)[a - sec];
    }
    return HS_OK;
}
struct PowerLoss {};                       // thrown by the fake flash to simulate a power cut
static void flash_spend()
{
    if (fakehal.flash_budget < 0) return;
    if (fakehal.flash_budget == 0) throw PowerLoss();
    fakehal.flash_budget--;
}
static bool fake_user_area(uint32_t a, uint32_t len) { return a >= 0xFD0000u && a <= 0x1000000u && len <= 0x1000000u - a; }
HalStatus ezhal_flash_erase_sector(uint32_t addr)
{
    if (addr % HAL_FLASH_SECTOR || !fake_user_area(addr, HAL_FLASH_SECTOR)) return HS_ERROR;
    auto &v = flash_sector(addr);
    for (uint32_t c = 0; c < HAL_FLASH_SECTOR; c += 256) {     // erase proceeds in chunks; a cut leaves a mix
        flash_spend();
        memset(v.data() + c, 0xFF, 256);
    }
    fakehal.erase_counts[addr]++;
    return HS_OK;
}
HalStatus ezhal_flash_program(uint32_t addr, const void *buf, uint32_t len)
{
    if (!fake_user_area(addr, len)) return HS_ERROR;
    if (fakehal.flash_verify_fail) return HS_OK;               // simulate a write that silently does nothing
    const uint8_t *b = (const uint8_t *)buf;
    for (uint32_t i = 0; i < len; i++) {
        flash_spend();
        uint32_t a = addr + i, sec = a - (a % HAL_FLASH_SECTOR);
        uint8_t &cell = flash_sector(sec)[a - sec];
        if (b[i] & ~cell) fakehal.nor_violations++;
        cell &= b[i];
    }
    return HS_OK;
}
uint32_t ezhal_flash_size() { return fakehal.flash_size; }
#ifdef EZ_REAL_FS
HalStatus ezhal_fs_mount_region(bool format_if_blank, String *detail);
HalStatus ezhal_fs_mount(bool f, String *detail) { return ezhal_fs_mount_region(f, detail); }   // size/write check are hardware-only
#else
HalStatus ezhal_fs_mount(bool, String *detail) { *detail = "flash is 16 MB: no file system"; return HS_NOT_PRESENT; }
#endif
#ifndef EZ_REAL_FS
// stand-ins for suites that only need the file system to be absent (test_fs uses the real code)
HalStatus ezhal_fs_format() { return HS_ERROR; }
bool ezhal_fs_mounted() { return false; }
HalStatus ezhal_fs_usage(uint32_t *u, uint32_t *t) { *u = 0; *t = 0; return HS_ERROR; }
HalStatus ezhal_fs_list(const char *, String *o) { *o = ""; return HS_ERROR; }
int32_t ezhal_fs_size(const char *) { return -1; }
int32_t ezhal_fs_read(const char *, uint32_t, uint8_t *, uint32_t) { return -1; }
HalStatus ezhal_fs_remove(const char *) { return HS_ERROR; }
HalStatus ezhal_fs_write_file(const char *, const uint8_t *, uint32_t) { return HS_ERROR; }
HalStatus ezhal_fs_append(const char *, const uint8_t *, uint32_t) { return HS_ERROR; }
HalStatus ezhal_fs_mkdir(const char *) { return HS_ERROR; }
HalStatus ezhal_fs_rename(const char *, const char *) { return HS_ERROR; }
#endif
static bool fake_fs_area(uint32_t a, uint32_t len)
{
    const uint32_t end = EZHAL_FS_BASE + EZHAL_FS_BLOCKS * EZHAL_FS_BLOCK;
    return fakehal.flash_size >= 0x2000000u && a >= EZHAL_FS_BASE && a <= end && len <= end - a;
}
HalStatus ezhal_fsflash_read(uint32_t addr, void *buf, uint32_t len)
{
    if (!fake_fs_area(addr, len)) return HS_ERROR;
    return ezhal_flash_read(addr, buf, len);
}
HalStatus ezhal_fsflash_erase(uint32_t addr)
{
    if (addr % HAL_FLASH_SECTOR || !fake_fs_area(addr, HAL_FLASH_SECTOR)) return HS_ERROR;
    auto &v = flash_sector(addr);
    for (uint32_t c = 0; c < HAL_FLASH_SECTOR; c += 256) {
        flash_spend();
        memset(v.data() + c, 0xFF, 256);
    }
    fakehal.erase_counts[addr]++;
    return HS_OK;
}
HalStatus ezhal_fsflash_program(uint32_t addr, const void *buf, uint32_t len)
{
    if (!fake_fs_area(addr, len)) return HS_ERROR;
    const uint8_t *b = (const uint8_t *)buf;
    for (uint32_t i = 0; i < len; i++) {
        flash_spend();
        uint32_t a2 = addr + i, sec = a2 - (a2 % HAL_FLASH_SECTOR);
        uint8_t &cell = flash_sector(sec)[a2 - sec];
        if (b[i] & ~cell) fakehal.nor_violations++;
        cell &= b[i];
    }
    return HS_OK;
}
HalStatus ezhal_flash_write_sector(uint32_t addr, const void *buf, uint32_t len)
{
    if (len > HAL_FLASH_SECTOR || addr % HAL_FLASH_SECTOR) return HS_ERROR;
    auto &s = flash_sector(addr);
    s.assign(HAL_FLASH_SECTOR, 0xFF);
    memcpy(s.data(), buf, len);
    fakehal.flash_writes++;
    return fakehal.flash_verify_fail ? HS_ERROR : HS_OK;
}
HalStatus ezhal_radio_begin(const char *, const char *) { fakehal.radio_begins++; return fakehal.connected ? HS_OK : HS_ERROR; }
void ezhal_radio_disconnect() {}
bool ezhal_radio_connected() { return fakehal.connected; }
int32_t ezhal_radio_rssi() { return fakehal.rssi; }
void ezhal_radio_mac(uint8_t m[6]) { memcpy(m, fakehal.mac, 6); }
IPAddress ezhal_radio_local_ip() { return fakehal.ip; }
String ezhal_radio_local_ip_str() { return fakehal.ip.toStr(); }
bool ezhal_radio_resolve(const char *, IPAddress *ip) { if (fakehal.resolve_ok) *ip = IPAddress(192, 168, 100, 1); return fakehal.resolve_ok; }
int ezhal_tcp_connect(uint32_t, uint16_t) { return fakehal.connect_result; }
int ezhal_tcp_send(int, const uint8_t *b, int len)
{
    if (fakehal.send_fail_after == 0) return -1;
    if (fakehal.send_fail_after > 0) fakehal.send_fail_after--;
    fakehal.sent.push_back(std::string((const char *)b, len)); return len;
}
int ezhal_tcp_recv(int, uint8_t *buf, int len)
{
    if (!fakehal.recv_codes.empty()) { int c = fakehal.recv_codes.front(); fakehal.recv_codes.pop_front(); return c; }
    if (fakehal.rx.empty()) return 0;
    std::string chunk = fakehal.rx.front(); fakehal.rx.pop_front();
    int n = std::min<int>(len, (int)chunk.size());
    memcpy(buf, chunk.data(), n);
    if (n < (int)chunk.size()) fakehal.rx.push_front(chunk.substr(n));
    return n;
}
int ezhal_tcp_error(int) { return fakehal.so_error; }
void ezhal_tcp_close(int s) { fakehal.closed++; fakehal.last_closed = s; }
uint32_t ezhal_ntp_query(uint32_t, int) { return fakehal.ntp_epoch; }
int g_probe_calls = 0; bool g_probe_write = false; int g_probe_lock_depth = -1;
HalStatus ezhal_flash_probe(bool write_test, String *report)
{
    g_probe_calls++; g_probe_write = write_test; g_probe_lock_depth = g_lock_depth;
    *report = "jedec=EF 40 19 (Winbond, 32 MB)\nverdict: fake";
    return HS_OK;
}
void ezhal_wdt_start(uint32_t) {}
void ezhal_wdt_refresh() { fakehal.wdt_refresh++; }
void ezhal_sys_reset() { fakehal.reset_called = true; }
uint32_t ezhal_heap_free() { return fakehal.heap; }
uint32_t ezhal_heap_min() { return fakehal.heap_min; }

inline void fake_reset_all()
{
    fakehal = FakeHal();
    g_fake_millis = 1000;
    Serial.lines.clear();
    g_lock_depth = 0;
}
