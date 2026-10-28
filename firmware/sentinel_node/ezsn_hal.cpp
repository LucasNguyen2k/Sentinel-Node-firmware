/*
 * ezsn_hal.cpp - Hardware Abstraction Layer implementation (see ezsn_hal.h, FW-02)
 *
 * Everything that touches a Realtek SDK driver or an Arduino library for a device lives here.
 * Stub sections (ultrasonic OP-01, power OP-03/OP-04, tamper OP-08) are where the drivers go when the
 * hardware is fitted; nothing else in the firmware needs to change for that.
 *
 * Raw lwIP code is at the END of this file: lwip/sockets.h defines read/write/close/... macros that
 * must not reach any other code.
 */
#include "ezsn_hal.h"
#include <WiFi.h>
#include "VideoStream.h"
#include <Wire.h>
#include "lfs.h"                  // LittleFS 2.4.1 from the Ameba core (LFS_THREADSAFE)
#include "WDT.h"
#include "AmebaFatFS.h"
#include "sys_api.h"

extern "C" {
#include "flash_api.h"
#include "hal_flash.h"
#include "device_lock.h"
#include "module_filesaver.h"
int rltk_wlan_get_thermal(uint32_t *thermal);
size_t xPortGetMinimumEverFreeHeapSize(void);
}

// =============================================================================================
// Chemical (gas) sensor
// =============================================================================================
static bool g_gasPresent = false;

bool ezhal_gas_init()
{
    // Analog input needs no set-up; "present" means the input is not stuck at a rail.
    int raw = analogRead(HAL_PIN_CHEMICAL);
    g_gasPresent = true;
    return raw > 3 && raw < HAL_GAS_ADC_MAX - 3;
}

bool ezhal_gas_present()
{
    return g_gasPresent;
}

HalStatus ezhal_gas_read_raw(int *raw)
{
    if (!g_gasPresent) return HS_NOT_PRESENT;
    *raw = analogRead(HAL_PIN_CHEMICAL);
    return HS_OK;
}

float ezhal_gas_raw_to_volts(float raw)
{
    return raw * HAL_GAS_VREF / (float)HAL_GAS_ADC_MAX;
}

// =============================================================================================
// Ambient temperature / humidity - AHT20
// =============================================================================================
static bool g_envPresent = false;

#define AHT_ADDR 0x38

// last valid reading (filled at boot and by the non-blocking ezhal_env_read)
static uint32_t g_ahtTrigMs = 0;
static bool g_ahtPending = false;
static int g_ahtLast = AHT_BLANK;
static uint32_t g_ahtValidMs = 0;
static float g_ahtT = 0, g_ahtH = 0;
// 1.6.1: absent-sensor handling. On this core an I2C transfer to a missing device waits for a timeout;
// in 1.5.1-1.6.0 that ran in the 50 ms sampling path and slowed sampling to one sample per ~6 s.
#ifndef EZHAL_AHT_PROBE_MS
#define EZHAL_AHT_PROBE_MS 60000UL                 // missing sensor: look for it again once a minute
#endif
static bool g_ahtAbsent = false;
static uint8_t g_ahtFails = 0;
static uint32_t g_ahtProbeMs = 0;

// CRC-8 of the AHT20 frame: polynomial 0x31, initial value 0xFF (datasheet)
uint8_t ezhal_aht_crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0xFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
    return crc;
}

// frame = status, 5 data bytes, CRC
int ezhal_aht_decode(const uint8_t frame[7], float *temp_c, float *rh_pct)
{
    bool zero = true, ones = true;
    for (int i = 0; i < 7; i++) {
        if (frame[i] != 0x00) zero = false;
        if (frame[i] != 0xFF) ones = false;
    }
    if (zero || ones) return AHT_BLANK;                    // nobody answered on the bus
    if (ezhal_aht_crc8(frame, 6) != frame[6]) return AHT_CRC;
    if (frame[0] & 0x80) return AHT_BUSY;
    uint32_t hraw = ((uint32_t)frame[1] << 12) | ((uint32_t)frame[2] << 4) | (frame[3] >> 4);
    uint32_t traw = (((uint32_t)frame[3] & 0x0F) << 16) | ((uint32_t)frame[4] << 8) | frame[5];
    float rh = hraw * 100.0f / 1048576.0f;
    float t = traw * 200.0f / 1048576.0f - 50.0f;
    if (t < -40.0f || t > 85.0f || rh < 0.0f || rh > 100.0f) return AHT_RANGE;   // sensor operating range
    *temp_c = t;
    *rh_pct = rh;
    return AHT_OK;
}

void ezhal_aht_read_frame(uint8_t frame[7])
{
    for (int i = 0; i < 7; i++) frame[i] = 0;
    Wire.requestFrom(AHT_ADDR, 7);
    for (int i = 0; i < 7 && Wire.available(); i++) frame[i] = (uint8_t)Wire.read();
    while (Wire.available()) Wire.read();
}

// start a measurement; false when nobody acknowledges the address
bool ezhal_aht_trigger()
{
    Wire.beginTransmission(AHT_ADDR);
    Wire.write(0xAC);
    Wire.write(0x33);
    Wire.write(0x00);
    return Wire.endTransmission() == 0;
}

static int ezhal_aht_measure(float *t, float *rh)
{
    Wire.beginTransmission(AHT_ADDR);
    Wire.write(0xAC);
    Wire.write(0x33);
    Wire.write(0x00);
    Wire.endTransmission();
    uint8_t frame[7];
    int r = AHT_BUSY;
    for (int tries = 0; tries < 10 && r == AHT_BUSY; tries++) {   // conversion takes ~80 ms
        delay(tries == 0 ? 80 : 20);
        ezhal_aht_read_frame(frame);
        r = ezhal_aht_decode(frame, t, rh);
    }
    return r;
}

bool ezhal_env_init()
{
    Wire.begin();
    delay(40);                                             // power-up time
    uint8_t st = 0;
    Wire.requestFrom(AHT_ADDR, 1);
    if (Wire.available()) st = (uint8_t)Wire.read();
    if ((st & 0x08) == 0) {                                // not calibrated: send the init command
        Wire.beginTransmission(AHT_ADDR);
        Wire.write(0xBE);
        Wire.write(0x08);
        Wire.write(0x00);
        Wire.endTransmission();
        delay(10);
    }
    float t = 0, h = 0;
    g_envPresent = ezhal_aht_measure(&t, &h) == AHT_OK;    // blocking once at boot (~80 ms)
    if (g_envPresent) {
        g_ahtT = t;
        g_ahtH = h;
        g_ahtValidMs = millis();
        g_ahtLast = AHT_OK;
    } else {
        g_ahtAbsent = true;                                // no I2C traffic until the next probe
        g_ahtProbeMs = millis();
    }
    return g_envPresent;
}


bool ezhal_env_present()
{
    return g_envPresent;
}

// The only code that talks to the AHT20 after boot; runs in its own low-priority thread (thread_env)
// every ~100 ms, so a slow or missing sensor can never delay sampling. One measurement per second:
// trigger, then read the frame >= 80 ms later. After 3 failures in a row the sensor counts as absent
// and is only probed once every EZHAL_AHT_PROBE_MS.
void ezhal_env_service(uint32_t now)
{
    if (g_ahtAbsent) {
        if (now - g_ahtProbeMs < EZHAL_AHT_PROBE_MS) return;
        g_ahtProbeMs = now;
        if (!ezhal_aht_trigger()) return;                  // still missing
        g_ahtAbsent = false;                               // fitted (again): measurement already started
        g_ahtFails = 0;
        g_ahtTrigMs = now ? now : 1;
        g_ahtPending = true;
        return;
    }
    if (g_ahtPending) {
        if (now - g_ahtTrigMs < 80) return;
        uint8_t frame[7];
        ezhal_aht_read_frame(frame);
        float t = 0, h = 0;
        int r = ezhal_aht_decode(frame, &t, &h);
        if (r == AHT_BUSY && now - g_ahtTrigMs <= 300) return;   // still converting
        g_ahtPending = false;
        g_ahtLast = r;
        if (r == AHT_OK) {
            taskENTER_CRITICAL();
            g_ahtT = t;
            g_ahtH = h;
            g_ahtValidMs = now ? now : 1;
            taskEXIT_CRITICAL();
            g_envPresent = true;
            g_ahtFails = 0;
        } else if (++g_ahtFails >= 3) {
            g_ahtAbsent = true;
            g_ahtProbeMs = now;
        }
        return;
    }
    if (g_ahtTrigMs == 0 || now - g_ahtTrigMs >= 1000) {
        g_ahtTrigMs = now ? now : 1;
        if (ezhal_aht_trigger()) {
            g_ahtPending = true;
        } else if (++g_ahtFails >= 3) {
            g_ahtAbsent = true;
            g_ahtProbeMs = now;
        }
    }
}

// Called every sample (50 ms): returns the last valid reading while it is less than 3 s old.
// Never touches the I2C bus.
HalStatus ezhal_env_read(float *temp_c, float *rh_pct)
{
    uint32_t now = millis();
    taskENTER_CRITICAL();
    uint32_t v = g_ahtValidMs;
    float t = g_ahtT, h = g_ahtH;
    taskEXIT_CRITICAL();
    if (v != 0 && now - v < 3000) {
        *temp_c = t;
        *rh_pct = h;
        return HS_OK;
    }
    return HS_ERROR;                                       // missing, faulty or stale
}

// =============================================================================================
// Ultrasonic leak sensor - OP-01
// =============================================================================================
bool ezhal_us_init()
{
#ifdef HAL_PIN_ULTRASONIC
    return true;
#else
    return false;
#endif
}

bool ezhal_us_present()
{
#ifdef HAL_PIN_ULTRASONIC
    return true;
#else
    return false;
#endif
}

HalStatus ezhal_us_read(float *level)
{
#ifdef HAL_PIN_ULTRASONIC
    *level = (float)analogRead(HAL_PIN_ULTRASONIC);   // OP-01: envelope level; scaling with the hardware
    return HS_OK;
#else
    *level = -1;
    return HS_NOT_PRESENT;
#endif
}

// =============================================================================================
// Power system - OP-03 / OP-04 (fuel gauge or ADC dividers + charger status pins)
// =============================================================================================
bool ezhal_power_init()
{
    return false;                              // OP-04: initialise fuel gauge / ADC dividers here
}

bool ezhal_power_present()
{
    return false;
}

HalStatus ezhal_power_read(HalPowerStatus *st)
{
    memset(st, 0, sizeof(*st));
    st->present = false;                       // OP-04: fill battery_v, soc_pct, charge_ma, solar_v, ...
    return HS_NOT_PRESENT;
}

// =============================================================================================
// Tamper / movement - OP-08 (accelerometer + enclosure switch)
// =============================================================================================
bool ezhal_tamper_init()
{
    return false;                              // OP-08: initialise accelerometer (I2C) and switch input here
}

bool ezhal_tamper_present()
{
    return false;
}

HalStatus ezhal_tamper_read(HalTamperStatus *st)
{
    memset(st, 0, sizeof(*st));
    st->present = false;                       // OP-08: fill moved, enclosure_open, detached, tilt_deg
    return HS_NOT_PRESENT;
}

// =============================================================================================
// Internal temperature - RF thermal meter of the RTL8735B
// =============================================================================================
HalStatus ezhal_temp_internal_raw(int32_t *raw)
{
    uint32_t th = 0;
    if (WiFi.status() != WL_CONNECTED) return HS_ERROR;   // meter is read through the Wi-Fi driver
    if (rltk_wlan_get_thermal(&th) != 0) return HS_ERROR;
    *raw = (int32_t)(th & 0xFF);
    return HS_OK;
}

// =============================================================================================
// Microphone tap - filesaver sink module that hands every raw PCM frame to a callback
// =============================================================================================
static HalMicFrameCallback g_micCb = NULL;
static bool g_micPresent = false;

static void micTapHandler(char *name, uint32_t data_addr, uint32_t data_size)
{
    (void)name;
    if (g_micCb != NULL && data_addr != 0 && data_size >= 2) {
        g_micCb((const int16_t *)data_addr, data_size / 2);
    }
}

class HalMicTap: public MMFModule {
public:
    bool begin()
    {
        _p_mmf_context = mm_module_open(&filesaver_module);
        if (_p_mmf_context == NULL) {
            return false;
        }
        mm_module_ctrl(_p_mmf_context, CMD_FILESAVER_SET_TYPE_HANDLER, (int)micTapHandler);
        mm_module_ctrl(_p_mmf_context, CMD_FILESAVER_APPLY, 0);
        return true;
    }
};
static HalMicTap g_micTap;

bool ezhal_mic_tap_init(HalMicFrameCallback cb)
{
    g_micCb = cb;
    g_micPresent = g_micTap.begin();
    return g_micPresent;
}

bool ezhal_mic_present()
{
    return g_micPresent;
}

MMFModule &ezhal_mic_tap_module()
{
    return g_micTap;
}

// =============================================================================================
// Camera
// =============================================================================================
#define HAL_CAM_CHANNELS 4
static bool g_camRunning[HAL_CAM_CHANNELS] = {false};

HalStatus ezhal_cam_start(int channel, uint32_t settle_ms)
{
    if (channel < 0 || channel >= HAL_CAM_CHANNELS) return HS_ERROR;
    if (g_camRunning[channel]) return HS_OK;
    Camera.channelBegin(channel);
    g_camRunning[channel] = true;
    if (settle_ms) delay(settle_ms);            // auto exposure
    return HS_OK;
}

HalStatus ezhal_cam_stop(int channel)
{
    if (channel < 0 || channel >= HAL_CAM_CHANNELS) return HS_ERROR;
    if (!g_camRunning[channel]) return HS_OK;
    Camera.channelEnd(channel);
    g_camRunning[channel] = false;
    return HS_OK;
}

bool ezhal_cam_running(int channel)
{
    return channel >= 0 && channel < HAL_CAM_CHANNELS && g_camRunning[channel];
}

static SemaphoreHandle_t g_snapReq = NULL, g_snapDone = NULL;
static volatile int g_snapCh = 0;
static volatile uint32_t g_snapAddr = 0, g_snapLen = 0;
static volatile bool g_snapBusy = false;
static volatile uint32_t g_snapTimeouts = 0;

static void ezhal_cam_snap_task(const void *argument)
{
    (void)argument;
    while (1) {
        if (xSemaphoreTake(g_snapReq, portMAX_DELAY) == pdTRUE) {
            uint32_t a = 0, l = 0;
            g_snapBusy = true;
            Camera.getImage(g_snapCh, &a, &l);          // no timeout inside the SDK
            g_snapAddr = a;
            g_snapLen = l;
            g_snapBusy = false;
            xSemaphoreGive(g_snapDone);
        }
    }
}

HalStatus ezhal_cam_snapshot(int channel, uint32_t *addr, uint32_t *len)
{
    *addr = 0;
    *len = 0;
    if (!ezhal_cam_running(channel)) return HS_ERROR;
    if (g_snapReq == NULL) {
        g_snapReq = xSemaphoreCreateBinary();
        g_snapDone = xSemaphoreCreateBinary();
        if (g_snapReq == NULL || g_snapDone == NULL ||
            !os_thread_create_arduino(ezhal_cam_snap_task, NULL, osPriorityNormal, 1024 * 4)) {
            return HS_ERROR;
        }
    }
    if (g_snapBusy) return HS_ERROR;                    // an earlier snapshot is still stuck in the SDK
    xSemaphoreTake(g_snapDone, 0);                      // discard a late completion of an earlier call
    g_snapCh = channel;
    xSemaphoreGive(g_snapReq);
    if (xSemaphoreTake(g_snapDone, HAL_CAM_SNAPSHOT_TIMEOUT_MS / portTICK_PERIOD_MS) != pdTRUE) {
        g_snapTimeouts++;
        return HS_ERROR;
    }
    *addr = g_snapAddr;
    *len = g_snapLen;
    return (*addr != 0 && *len > 0) ? HS_OK : HS_ERROR;
}

uint32_t ezhal_cam_timeouts()
{
    return g_snapTimeouts;
}

bool ezhal_cam_stuck()
{
    return g_snapBusy;
}

// =============================================================================================
// SD card (FatFS)
// =============================================================================================
static AmebaFatFS g_fs;
static bool g_sdPresent = false;
static String g_sdRoot = "";

static String sdPath(const String &rel)
{
    return g_sdRoot + rel;
}

bool ezhal_sd_mount()
{
    g_sdPresent = g_fs.begin();
    if (g_sdPresent) g_sdRoot = String(g_fs.getRootPath());
    return g_sdPresent;
}

bool ezhal_sd_present()
{
    return g_sdPresent;
}

HalStatus ezhal_sd_mkdir(const String &rel)
{
    if (!g_sdPresent) return HS_NOT_PRESENT;
    g_fs.mkdir(sdPath(rel));
    return HS_OK;
}

bool ezhal_sd_exists(const String &rel)
{
    return g_sdPresent && g_fs.exists(sdPath(rel));
}

HalStatus ezhal_sd_remove(const String &rel)
{
    if (!g_sdPresent) return HS_NOT_PRESENT;
    return g_fs.remove(sdPath(rel)) ? HS_OK : HS_ERROR;
}

HalStatus ezhal_sd_rename(const String &rel_from, const String &rel_to)
{
    if (!g_sdPresent) return HS_NOT_PRESENT;
    return g_fs.rename(sdPath(rel_from), sdPath(rel_to)) ? HS_OK : HS_ERROR;
}

int32_t ezhal_sd_size(const String &rel)
{
    if (!ezhal_sd_exists(rel)) return -1;
    File f = g_fs.open(sdPath(rel));
    if (!f) return -1;
    int32_t sz = (int32_t)f.size();
    f.close();
    return sz;
}

HalStatus ezhal_sd_append(const String &rel, const uint8_t *data, uint32_t len)
{
    if (!g_sdPresent) return HS_NOT_PRESENT;
    File f = g_fs.open(sdPath(rel));
    if (!f) return HS_ERROR;
    f.seek(f.size());
    bool ok = f.write(data, len) == len;
    f.close();
    return ok ? HS_OK : HS_ERROR;
}

HalStatus ezhal_sd_write(const String &rel, const uint8_t *data, uint32_t len)
{
    if (!g_sdPresent) return HS_NOT_PRESENT;
    String p = sdPath(rel);
    if (g_fs.exists(p)) g_fs.remove(p);
    File f = g_fs.open(p);
    if (!f) return HS_ERROR;
    bool ok = f.write(data, len) == len;
    f.close();
    return ok ? HS_OK : HS_ERROR;
}

int32_t ezhal_sd_read_at(const String &rel, uint32_t offset, uint8_t *buf, uint32_t len)
{
    if (!g_sdPresent) return -1;
    File f = g_fs.open(sdPath(rel));
    if (!f) return -1;
    f.seek(offset);
    uint32_t n = 0;
    int c;
    while (n < len && (c = f.read()) >= 0) {
        buf[n++] = (uint8_t)c;
    }
    f.close();
    return (int32_t)n;
}

HalStatus ezhal_sd_read_line(const String &rel, uint32_t offset, String *line, uint32_t *next_offset)
{
    if (!g_sdPresent) return HS_NOT_PRESENT;
    File f = g_fs.open(sdPath(rel));
    if (!f) return HS_ERROR;
    if (offset >= f.size()) {
        f.close();
        return HS_ERROR;                       // end of file
    }
    f.seek(offset);
    *line = "";
    int c;
    while ((c = f.read()) >= 0 && c != '\n') {
        *line += (char)c;
    }
    *next_offset = f.position();
    f.close();
    return HS_OK;
}

HalStatus ezhal_sd_start_ota(const String &rel)
{
    if (!ezhal_sd_exists(rel)) return HS_NOT_PRESENT;
    File f;
    f.sdstartota(sdPath(rel).c_str());
    return HS_OK;
}

// =============================================================================================
// Internal flash
// =============================================================================================
static flash_t g_flash;

bool ezhal_flash_range_ok(uint32_t addr, uint32_t len)
{
    return addr >= EZHAL_FLASH_USER_BASE && addr <= EZHAL_FLASH_USER_END && len <= EZHAL_FLASH_USER_END - addr;
}

HalStatus ezhal_flash_read(uint32_t addr, void *buf, uint32_t len)
{
    flash_stream_read(&g_flash, addr, len, (uint8_t *)buf);
    return HS_OK;
}

HalStatus ezhal_flash_erase_sector(uint32_t addr)
{
    if ((addr % HAL_FLASH_SECTOR) != 0 || !ezhal_flash_range_ok(addr, HAL_FLASH_SECTOR)) return HS_ERROR;
    flash_erase_sector(&g_flash, addr);
    return HS_OK;
}

HalStatus ezhal_flash_program(uint32_t addr, const void *buf, uint32_t len)
{
    if (!ezhal_flash_range_ok(addr, len)) return HS_ERROR;
    if (len == 0) return HS_OK;
    flash_stream_write(&g_flash, addr, len, (uint8_t *)buf);
    return HS_OK;
}

static const char *ezhal_flash_vendor(uint8_t m)
{
    switch (m) {
        case 0xEF: return "Winbond";
        case 0xC2: return "Macronix";
        case 0xC8: return "GigaDevice";
        case 0x20: return "XMC/Micron";
        case 0x0B: return "XTX";
        case 0x68: return "Boya";
        case 0x1C: return "EON";
        case 0x9D: return "ISSI";
        case 0x5E: return "Zbit";
        default: return "unknown";
    }
}

static String ezhal_hex(const uint8_t *b, int n)
{
    String s = "";
    const char *d = "0123456789ABCDEF";
    for (int i = 0; i < n; i++) {
        if (i) s += " ";
        s += d[b[i] >> 4];
        s += d[b[i] & 15];
    }
    return s;
}

HalStatus ezhal_flash_probe(bool write_test, String *report)
{
    const uint32_t SCR_OFF = EZHAL_FLASH_SCRATCH;               // 0xFD3000 in either 16 MB half
    const uint32_t UPPER = 0x1000000u + SCR_OFF;                // 0x1FD3000
    uint8_t id[3] = {0, 0, 0};
    uint8_t p[32], q[32], buf[32];
    String r = "";

    flash_read_id(&g_flash, id, 3);
    uint32_t size = (id[2] >= 0x10 && id[2] <= 0x20) ? (1u << id[2]) : 0;
    r += "jedec=" + ezhal_hex(id, 3) + " (" + ezhal_flash_vendor(id[0]) + ", " +
         (size ? String(size / (1024u * 1024u)) + " MB" : String("size unknown")) + ")\n";
    int ear0 = flash_get_extend_addr(&g_flash);
    r += "extended_addr_register=" + String(ear0) + "\n";
    if (ear0 != 0) {
        *report = r + "verdict: ABORT - extended address register is not 0 at start; nothing written\n";
        return HS_ERROR;
    }

    // unique pattern in the lower scratch sector
    uint32_t tag = millis();
    for (int i = 0; i < 32; i++) p[i] = (uint8_t)("EZLOWER-PROBE-PATTERN-0123456789"[i] ^ (tag >> (i % 4 * 8)));
    if (ezhal_flash_erase_sector(SCR_OFF) != HS_OK || ezhal_flash_program(SCR_OFF, p, 32) != HS_OK) {
        *report = r + "verdict: ABORT - could not prepare the scratch sector\n";
        return HS_ERROR;
    }
    flash_stream_read(&g_flash, SCR_OFF, 32, buf);
    bool lower_ok = memcmp(buf, p, 32) == 0;
    r += String("lower_scratch_write=") + (lower_ok ? "ok" : "FAIL") + "\n";

    // 1) plain read 16 MB higher: does it return the lower data (3-byte address wrap)?
    uint8_t direct[32];
    flash_stream_read(&g_flash, UPPER, 32, direct);
    bool wraps = memcmp(direct, p, 32) == 0;
    r += "direct_read_0x1FD3000=" + ezhal_hex(direct, 8) + "... wraps_to_lower=" + (wraps ? "YES" : "no") + "\n";

    // 2) read the same offset with the extended address register set to 1 (upper half)
    uint8_t viaear[32];
    flash_set_extend_addr(&g_flash, 1);
    flash_stream_read(&g_flash, SCR_OFF, 32, viaear);
    flash_set_extend_addr(&g_flash, 0);
    int ear_back = flash_get_extend_addr(&g_flash);
    bool ear_same_as_lower = memcmp(viaear, p, 32) == 0;
    bool ear_same_as_direct = memcmp(viaear, direct, 32) == 0;
    r += "ear1_read_0xFD3000=" + ezhal_hex(viaear, 8) + "... same_as_lower=" + (ear_same_as_lower ? "YES" : "no") +
         " same_as_direct=" + (ear_same_as_direct ? "yes" : "no") + " ear_restored=" + String(ear_back) + "\n";

    // 3) optional write test in the upper half (via the register); a misrouted write hits our scratch
    bool w_done = false, upper_ok = false, lower_untouched = false;
    if (write_test && ear_back == 0) {
        for (int i = 0; i < 32; i++) q[i] = (uint8_t)("EZUPPER-PROBE-PATTERN-9876543210"[i] ^ (tag >> (i % 4 * 8)));
        flash_set_extend_addr(&g_flash, 1);
        HalStatus e = ezhal_flash_erase_sector(SCR_OFF);
        flash_set_extend_addr(&g_flash, 0);
        flash_set_extend_addr(&g_flash, 1);
        HalStatus w = (e == HS_OK) ? ezhal_flash_program(SCR_OFF, q, 32) : HS_ERROR;
        flash_set_extend_addr(&g_flash, 0);
        flash_set_extend_addr(&g_flash, 1);
        flash_stream_read(&g_flash, SCR_OFF, 32, buf);
        flash_set_extend_addr(&g_flash, 0);
        upper_ok = (w == HS_OK) && memcmp(buf, q, 32) == 0;
        flash_stream_read(&g_flash, SCR_OFF, 32, buf);
        lower_untouched = memcmp(buf, p, 32) == 0;
        uint8_t again[32];
        flash_stream_read(&g_flash, UPPER, 32, again);
        bool direct_sees_upper = memcmp(again, q, 32) == 0;
        flash_set_extend_addr(&g_flash, 1);                  // clean the upper test sector again
        ezhal_flash_erase_sector(SCR_OFF);
        flash_set_extend_addr(&g_flash, 0);
        w_done = true;
        r += String("upper_write_via_ear=") + (upper_ok ? "ok" : "FAIL") + " lower_untouched=" +
             (lower_untouched ? "yes" : "NO") + " direct_read_sees_upper=" + (direct_sees_upper ? "yes" : "no") + "\n";
    }
    ezhal_flash_erase_sector(SCR_OFF);                            // leave the lower scratch erased
    int ear_end = flash_get_extend_addr(&g_flash);
    r += "extended_addr_register_at_end=" + String(ear_end) + "\n";

    String verdict;
    if (ear_end != 0) verdict = "ERROR - extended address register not restored: power-cycle the board";
    else if (!lower_ok) verdict = "ERROR - lower scratch sector not writable";
    else if (size && size <= 0x1000000u) verdict = "chip reports 16 MB or less: no upper half";
    else if (ear_same_as_lower && (!w_done || !upper_ok)) verdict = "upper half NOT reachable: the extended address register has no effect";
    else if (wraps && (!w_done || (upper_ok && lower_untouched)))
        verdict = String("upper half reachable via the extended address register; plain addresses >16 MB WRAP to the lower half") +
                  (w_done ? " (write test passed)" : " (run FLASH_PROBE WRITE to confirm writes)");
    else if (!wraps && ear_same_as_direct)
        verdict = String("plain addresses >16 MB reach the upper half (no wrap)") + (w_done ? (upper_ok && lower_untouched ? "; write test passed" : "; write test FAILED") : "; writes not tested");
    else verdict = "inconclusive - send this report";
    *report = r + "verdict: " + verdict + "\n";
    return (ear_end == 0 && lower_ok) ? HS_OK : HS_ERROR;
}

uint32_t ezhal_flash_size()
{
    static uint32_t size = 0xFFFFFFFFu;
    if (size == 0xFFFFFFFFu) {
        uint8_t id[3] = {0, 0, 0};
        flash_read_id(&g_flash, id, 3);
        size = (id[2] >= 0x10 && id[2] <= 0x20) ? (1u << id[2]) : 0;
    }
    return size;
}

bool ezhal_fsflash_range_ok(uint32_t addr, uint32_t len)
{
    const uint32_t end = EZHAL_FS_BASE + EZHAL_FS_BLOCKS * EZHAL_FS_BLOCK;
    return ezhal_flash_size() >= 0x2000000u && addr >= EZHAL_FS_BASE && addr <= end && len <= end - addr;
}

HalStatus ezhal_fsflash_read(uint32_t addr, void *buf, uint32_t len)
{
    if (!ezhal_fsflash_range_ok(addr, len)) return HS_ERROR;
    flash_stream_read(&g_flash, addr, len, (uint8_t *)buf);
    return HS_OK;
}

HalStatus ezhal_fsflash_program(uint32_t addr, const void *buf, uint32_t len)
{
    if (!ezhal_fsflash_range_ok(addr, len)) return HS_ERROR;
    if (len) flash_stream_write(&g_flash, addr, len, (uint8_t *)buf);
    return HS_OK;
}

HalStatus ezhal_fsflash_erase(uint32_t addr)
{
    if ((addr % EZHAL_FS_BLOCK) != 0 || !ezhal_fsflash_range_ok(addr, EZHAL_FS_BLOCK)) return HS_ERROR;
    flash_erase_sector(&g_flash, addr);
    return HS_OK;
}

// A plain-address write at 0x1FD3000 must reach the upper half: if the address wrapped, it would land in
// our own scratch sector 0xFD3000 (harmless), and the file system is not mounted.
HalStatus ezhal_fs_upper_write_check(String *detail)
{
    if (ezhal_flash_size() < 0x2000000u) {
        *detail = "flash is " + String(ezhal_flash_size() / 1048576u) + " MB: no file system";
        return HS_NOT_PRESENT;
    }
    uint8_t p[16], q[16], b[16];
    uint32_t tag = millis();
    for (int i = 0; i < 16; i++) {
        p[i] = (uint8_t)(0x5A ^ i ^ (tag >> (i % 4 * 8)));
        q[i] = (uint8_t)(0xA5 ^ i ^ (tag >> (i % 4 * 8)));
    }
    ezhal_flash_erase_sector(EZHAL_FLASH_SCRATCH);
    ezhal_flash_program(EZHAL_FLASH_SCRATCH, p, 16);
    flash_erase_sector(&g_flash, EZHAL_FS_CHECK_ADDR);
    flash_stream_write(&g_flash, EZHAL_FS_CHECK_ADDR, 16, q);
    flash_stream_read(&g_flash, EZHAL_FS_CHECK_ADDR, 16, b);
    bool upper = memcmp(b, q, 16) == 0;
    flash_stream_read(&g_flash, EZHAL_FLASH_SCRATCH, 16, b);
    bool lower = memcmp(b, p, 16) == 0;
    flash_erase_sector(&g_flash, EZHAL_FS_CHECK_ADDR);
    ezhal_flash_erase_sector(EZHAL_FLASH_SCRATCH);
    *detail = String("upper_write=") + (upper ? "ok" : "FAIL") + " lower_untouched=" + (lower ? "yes" : "NO");
    return (upper && lower) ? HS_OK : HS_ERROR;
}

HalStatus ezhal_fs_mount(bool format_if_blank, String *detail)
{
    static int checked = 0;                                // 0 = not yet, 1 = passed, -1 = failed
    if (checked == 0) {
        String d;
        HalStatus c = ezhal_fs_upper_write_check(&d);
        checked = (c == HS_OK) ? 1 : -1;
        if (c != HS_OK) {
            *detail = d;
            return c;
        }
    } else if (checked < 0) {
        *detail = "upper-half write check failed at boot";
        return HS_ERROR;
    }
    return ezhal_fs_mount_region(format_if_blank, detail);
}

HalStatus ezhal_flash_write_sector(uint32_t addr, const void *buf, uint32_t len)
{
    if (len > HAL_FLASH_SECTOR || (addr % HAL_FLASH_SECTOR) != 0) return HS_ERROR;
    if (!ezhal_flash_range_ok(addr, HAL_FLASH_SECTOR)) return HS_ERROR;
    flash_erase_sector(&g_flash, addr);
    flash_stream_write(&g_flash, addr, len, (uint8_t *)buf);
    uint8_t chk[256];                            // read back and verify
    for (uint32_t off = 0; off < len; off += sizeof(chk)) {
        uint32_t n = (len - off) < sizeof(chk) ? (len - off) : sizeof(chk);
        flash_stream_read(&g_flash, addr + off, n, chk);
        if (memcmp(chk, (const uint8_t *)buf + off, n) != 0) return HS_ERROR;
    }
    return HS_OK;
}

// =============================================================================================
// Flash data storage: CRC, A/B record store, append-only logs.
// Portable: uses only ezhal_flash_read / ezhal_flash_program / ezhal_flash_erase_sector, so the host
// unit tests run this exact code against a simulated NOR flash with power-cut injection.
// =============================================================================================
#define EZREC_MAGIC     0x43525A45u      // "EZRC"
#define EZLOG_MAGIC     0x474C5A45u      // "EZLG"
#define EZLOG_HDR       16u              // sector header size
#define EZLOG_RHDR      8u               // record header size
#define EZLOG_F_COMMIT  0xFEu            // flag byte: 0xFF written, 0xFE committed, 0xFC consumed
#define EZLOG_F_DONE    0xFCu
#define EZLOG_MAX_SECTORS 36

struct EzRecHeader {
    uint32_t magic, seq, len, crc;
};
struct EzLogSectorHdr {
    uint32_t magic, seq, erase_count, rsv;
};
struct EzLogRecHdr {
    uint16_t len;
    uint8_t flags;
    uint8_t rsv;
    uint32_t crc;
};
struct EzLogRegion {
    uint32_t base;
    uint16_t sectors;
};
struct EzLogState {
    bool ready;
    int16_t head;                        // physical index of the newest sector, -1 = empty ring
    uint32_t head_off;                   // next write offset in the head sector
    uint32_t next_seq;                   // sequence number for the next sector
    int16_t cur;                         // replay cursor: sector ...
    uint32_t cur_off;                    // ... and offset of the next record to examine
    uint32_t pending, records, dropped, corrupt, max_erase;
    uint32_t sect_seq[EZLOG_MAX_SECTORS];   // 0 = free sector
};

const EzLogRegion ezhal_flog_regions[EZFLOG_COUNT] = {
    {EZHAL_FLASH_LOG_EVENTS_BASE, EZHAL_FLASH_LOG_EVENTS_SECTORS},
    {EZHAL_FLASH_LOG_BUFFER_BASE, EZHAL_FLASH_LOG_BUFFER_SECTORS},
};
EzLogState ezhal_flog_state[EZFLOG_COUNT];

uint32_t ezhal_crc32(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

// ---- A/B record store -------------------------------------------------------------------------
bool ezhal_frec_slot_valid(uint32_t addr, uint32_t len, EzRecHeader *h, uint8_t *payload)
{
    if (ezhal_flash_read(addr, h, sizeof(*h)) != HS_OK) return false;
    if (h->magic != EZREC_MAGIC || h->len != len || len > HAL_FLASH_SECTOR - sizeof(*h)) return false;
    if (ezhal_flash_read(addr + sizeof(*h), payload, len) != HS_OK) return false;
    return ezhal_crc32(payload, len) == h->crc;
}

HalStatus ezhal_frec_read(uint32_t slot_a, uint32_t slot_b, void *buf, uint32_t len, uint32_t *seq)
{
    if (len == 0 || len > HAL_FLASH_SECTOR - sizeof(EzRecHeader)) return HS_ERROR;
    uint8_t *tmp = (uint8_t *)malloc(len);
    if (tmp == NULL) return HS_ERROR;
    EzRecHeader ha, hb;
    bool va = ezhal_frec_slot_valid(slot_a, len, &ha, (uint8_t *)buf);
    bool vb = ezhal_frec_slot_valid(slot_b, len, &hb, tmp);
    HalStatus r = HS_NOT_PRESENT;
    if (vb && (!va || hb.seq > ha.seq)) {
        memcpy(buf, tmp, len);
        if (seq) *seq = hb.seq;
        r = HS_OK;
    } else if (va) {
        if (seq) *seq = ha.seq;
        r = HS_OK;
    }
    free(tmp);
    return r;
}

HalStatus ezhal_frec_write(uint32_t slot_a, uint32_t slot_b, const void *buf, uint32_t len)
{
    if (len == 0 || len > HAL_FLASH_SECTOR - sizeof(EzRecHeader)) return HS_ERROR;
    uint8_t *tmp = (uint8_t *)malloc(len);
    if (tmp == NULL) return HS_ERROR;
    EzRecHeader ha, hb;
    bool va = ezhal_frec_slot_valid(slot_a, len, &ha, tmp);
    bool vb = ezhal_frec_slot_valid(slot_b, len, &hb, tmp);
    uint32_t target, seq;
    if (va && vb) {
        target = (ha.seq >= hb.seq) ? slot_b : slot_a;
        seq = (ha.seq >= hb.seq ? ha.seq : hb.seq) + 1;
    } else if (va) {
        target = slot_b; seq = ha.seq + 1;
    } else if (vb) {
        target = slot_a; seq = hb.seq + 1;
    } else {
        target = slot_b; seq = 1;          // B first: a legacy record in A stays intact until B is written
    }
    EzRecHeader h = {EZREC_MAGIC, seq, len, ezhal_crc32((const uint8_t *)buf, len)};
    HalStatus r = HS_ERROR;
    if (ezhal_flash_erase_sector(target) == HS_OK &&
        ezhal_flash_program(target + sizeof(h), buf, len) == HS_OK &&     // payload first ...
        ezhal_flash_program(target, &h, sizeof(h)) == HS_OK) {             // ... header last
        EzRecHeader hv;
        if (ezhal_frec_slot_valid(target, len, &hv, tmp) && hv.seq == seq && memcmp(tmp, buf, len) == 0) r = HS_OK;
    }
    free(tmp);
    return r;
}

// ---- flash logs -------------------------------------------------------------------------------
static inline uint32_t ezlog_rec_size(uint16_t len)
{
    return EZLOG_RHDR + (((uint32_t)len + 3u) & ~3u);
}

uint32_t ezhal_flog_addr(int log, int sect, uint32_t off)
{
    return ezhal_flog_regions[log].base + (uint32_t)sect * HAL_FLASH_SECTOR + off;
}

// Read the record header at (sect, off). false = no record here (free space or unusable bytes).
bool ezhal_flog_rec_at(int log, int sect, uint32_t off, EzLogRecHdr *r)
{
    if (off + EZLOG_RHDR > HAL_FLASH_SECTOR) return false;
    if (ezhal_flash_read(ezhal_flog_addr(log, sect, off), r, sizeof(*r)) != HS_OK) return false;
    if (r->len == 0xFFFF || r->len == 0) return false;
    return off + ezlog_rec_size(r->len) <= HAL_FLASH_SECTOR;
}

// Next sector in ring order after `sect` that holds data, or -1.
int ezhal_flog_next_used(int log, int sect)
{
    EzLogState &s = ezhal_flog_state[log];
    int n = ezhal_flog_regions[log].sectors;
    if (sect == s.head) return -1;
    for (int k = 1; k <= n; k++) {
        int i = (sect + k) % n;
        if (s.sect_seq[i] != 0) return i;
        if (i == s.head) return -1;
    }
    return -1;
}

HalStatus ezhal_flog_init(int log)
{
    if (log < 0 || log >= EZFLOG_COUNT) return HS_ERROR;
    EzLogState &s = ezhal_flog_state[log];
    int n = ezhal_flog_regions[log].sectors;
    memset(&s, 0, sizeof(s));
    s.head = -1;
    s.cur = -1;
    uint32_t maxseq = 0, minseq = 0xFFFFFFFFu;
    int tail = -1;
    for (int i = 0; i < n; i++) {
        EzLogSectorHdr h;
        ezhal_flash_read(ezhal_flog_addr(log, i, 0), &h, sizeof(h));
        if (h.magic == EZLOG_MAGIC && h.seq != 0 && h.seq != 0xFFFFFFFFu) {
            s.sect_seq[i] = h.seq;
            if (h.erase_count != 0xFFFFFFFFu && h.erase_count > s.max_erase) s.max_erase = h.erase_count;
            if (h.seq > maxseq) { maxseq = h.seq; s.head = i; }
            if (h.seq < minseq) { minseq = h.seq; tail = i; }
        }
    }
    s.next_seq = maxseq + 1;
    if (s.head >= 0) {
        // walk every used sector from the oldest: count records, find the end of the head sector
        int sect = tail;
        while (sect >= 0) {
            uint32_t off = EZLOG_HDR;
            EzLogRecHdr r;
            bool last_uncommitted = false;
            while (ezhal_flog_rec_at(log, sect, off, &r)) {
                last_uncommitted = (r.flags != EZLOG_F_COMMIT && r.flags != EZLOG_F_DONE);
                if (r.flags == EZLOG_F_COMMIT) { s.pending++; s.records++; }
                else if (r.flags == EZLOG_F_DONE) s.records++;
                off += ezlog_rec_size(r.len);
            }
            if (sect == s.head) {
                // a power cut can leave a half-written record at the end: never append after it
                bool clean_end = true;
                if (off + EZLOG_RHDR <= HAL_FLASH_SECTOR) {
                    EzLogRecHdr fr;
                    ezhal_flash_read(ezhal_flog_addr(log, sect, off), &fr, sizeof(fr));
                    clean_end = fr.len == 0xFFFF && fr.flags == 0xFF && fr.crc == 0xFFFFFFFFu;
                }
                s.head_off = (last_uncommitted || !clean_end) ? HAL_FLASH_SECTOR : off;
            }
            sect = ezhal_flog_next_used(log, sect);
        }
        s.cur = tail;
        s.cur_off = EZLOG_HDR;
    }
    s.ready = true;
    return HS_OK;
}

// Start a new head sector: the next sector in rotation (erasing the oldest data if the ring is full).
HalStatus ezhal_flog_new_sector(int log)
{
    EzLogState &s = ezhal_flog_state[log];
    int n = ezhal_flog_regions[log].sectors;
    int nxt = (s.head < 0) ? 0 : (s.head + 1) % n;
    if (s.sect_seq[nxt] != 0) {
        // ring full: this is the oldest sector; count what is lost
        uint32_t off = EZLOG_HDR;
        EzLogRecHdr r;
        while (ezhal_flog_rec_at(log, nxt, off, &r)) {
            if (r.flags == EZLOG_F_COMMIT) {
                bool before_cursor = (s.cur == nxt && off < s.cur_off);
                if (!before_cursor) { s.dropped++; if (s.pending) s.pending--; }
                if (s.records) s.records--;
            } else if (r.flags == EZLOG_F_DONE) {
                if (s.records) s.records--;
            }
            off += ezlog_rec_size(r.len);
        }
        if (s.cur == nxt) {
            int after = ezhal_flog_next_used(log, nxt);
            s.cur = (after >= 0) ? after : -1;
            s.cur_off = EZLOG_HDR;
        }
    }
    EzLogSectorHdr old;
    ezhal_flash_read(ezhal_flog_addr(log, nxt, 0), &old, sizeof(old));
    uint32_t ec = (old.magic == EZLOG_MAGIC && old.erase_count != 0xFFFFFFFFu) ? old.erase_count + 1 : 1;
    s.sect_seq[nxt] = 0;
    if (ezhal_flash_erase_sector(ezhal_flog_addr(log, nxt, 0)) != HS_OK) return HS_ERROR;
    EzLogSectorHdr h = {EZLOG_MAGIC, s.next_seq, ec, 0xFFFFFFFFu};
    if (ezhal_flash_program(ezhal_flog_addr(log, nxt, 0), &h, sizeof(h)) != HS_OK) return HS_ERROR;
    s.sect_seq[nxt] = s.next_seq++;
    if (ec > s.max_erase) s.max_erase = ec;
    s.head = nxt;
    s.head_off = EZLOG_HDR;
    if (s.cur < 0) {
        s.cur = nxt;
        s.cur_off = EZLOG_HDR;
    }
    return HS_OK;
}

HalStatus ezhal_flog_append(int log, const uint8_t *data, uint16_t len)
{
    if (log < 0 || log >= EZFLOG_COUNT || !ezhal_flog_state[log].ready) return HS_ERROR;
    if (len == 0 || ezlog_rec_size(len) > HAL_FLASH_SECTOR - EZLOG_HDR) return HS_ERROR;
    EzLogState &s = ezhal_flog_state[log];
    uint32_t size = ezlog_rec_size(len);
    if (s.head < 0 || s.head_off + size > HAL_FLASH_SECTOR) {
        if (ezhal_flog_new_sector(log) != HS_OK) return HS_ERROR;
    }
    uint32_t addr = ezhal_flog_addr(log, s.head, s.head_off);
    EzLogRecHdr r = {len, 0xFF, 0xFF, ezhal_crc32(data, len)};
    s.head_off += size;                                          // reserve first: a failed write is never reused
    if (ezhal_flash_program(addr, &r, sizeof(r)) != HS_OK) return HS_ERROR;            // 1. header (length)
    if (ezhal_flash_program(addr + EZLOG_RHDR, data, len) != HS_OK) return HS_ERROR;   // 2. data
    uint8_t f = EZLOG_F_COMMIT;
    if (ezhal_flash_program(addr + 2, &f, 1) != HS_OK) return HS_ERROR;                // 3. commit flag
    s.pending++;
    s.records++;
    return HS_OK;
}

HalStatus ezhal_flog_peek(int log, uint8_t *buf, uint16_t cap, uint16_t *len)
{
    if (log < 0 || log >= EZFLOG_COUNT || !ezhal_flog_state[log].ready) return HS_ERROR;
    EzLogState &s = ezhal_flog_state[log];
    while (s.cur >= 0) {
        EzLogRecHdr r;
        if (!ezhal_flog_rec_at(log, s.cur, s.cur_off, &r)) {
            int nxt = ezhal_flog_next_used(log, s.cur);
            if (nxt < 0) return HS_NOT_PRESENT;
            s.cur = nxt;
            s.cur_off = EZLOG_HDR;
            continue;
        }
        if (r.flags == EZLOG_F_COMMIT) {
            if (r.len > cap) return HS_ERROR;
            ezhal_flash_read(ezhal_flog_addr(log, s.cur, s.cur_off + EZLOG_RHDR), buf, r.len);
            if (ezhal_crc32(buf, r.len) == r.crc) {
                *len = r.len;
                return HS_OK;
            }
            s.corrupt++;                                         // unreadable: mark consumed and skip it
            uint8_t f = EZLOG_F_DONE;
            ezhal_flash_program(ezhal_flog_addr(log, s.cur, s.cur_off + 2), &f, 1);
            if (s.pending) s.pending--;
        }
        s.cur_off += ezlog_rec_size(r.len);                       // consumed or never committed
    }
    return HS_NOT_PRESENT;
}

HalStatus ezhal_flog_consume(int log)
{
    if (log < 0 || log >= EZFLOG_COUNT || !ezhal_flog_state[log].ready) return HS_ERROR;
    EzLogState &s = ezhal_flog_state[log];
    EzLogRecHdr r;
    if (s.cur < 0 || !ezhal_flog_rec_at(log, s.cur, s.cur_off, &r) || r.flags != EZLOG_F_COMMIT) return HS_ERROR;
    uint8_t f = EZLOG_F_DONE;
    if (ezhal_flash_program(ezhal_flog_addr(log, s.cur, s.cur_off + 2), &f, 1) != HS_OK) return HS_ERROR;
    s.cur_off += ezlog_rec_size(r.len);
    if (s.pending) s.pending--;
    return HS_OK;
}

uint32_t ezhal_flog_pending(int log)
{
    return (log >= 0 && log < EZFLOG_COUNT) ? ezhal_flog_state[log].pending : 0;
}

HalStatus ezhal_flog_read_recent(int log, uint32_t max_records, String *out)
{
    if (log < 0 || log >= EZFLOG_COUNT || !ezhal_flog_state[log].ready) return HS_ERROR;
    EzLogState &s = ezhal_flog_state[log];
    *out = "";
    if (s.head < 0 || max_records == 0) return HS_OK;
    if (max_records > 200) max_records = 200;
    int n = ezhal_flog_regions[log].sectors;
    uint32_t *pos = (uint32_t *)malloc(max_records * sizeof(uint32_t));   // ring of record addresses
    if (pos == NULL) return HS_ERROR;
    uint32_t cnt = 0;
    int sect = -1;
    for (int k = 1; k <= n; k++) {                                // oldest used sector = first after head
        int i = (s.head + k) % n;
        if (s.sect_seq[i] != 0) { sect = i; break; }
    }
    while (sect >= 0) {
        uint32_t off = EZLOG_HDR;
        EzLogRecHdr r;
        while (ezhal_flog_rec_at(log, sect, off, &r)) {
            if (r.flags == EZLOG_F_COMMIT || r.flags == EZLOG_F_DONE) pos[cnt++ % max_records] = ezhal_flog_addr(log, sect, off);
            off += ezlog_rec_size(r.len);
        }
        sect = ezhal_flog_next_used(log, sect);
    }
    uint32_t first = cnt > max_records ? cnt - max_records : 0;
    uint8_t buf[257];
    for (uint32_t k = first; k < cnt; k++) {
        uint32_t a = pos[k % max_records];
        EzLogRecHdr r;
        ezhal_flash_read(a, &r, sizeof(r));
        uint16_t l = r.len > 256 ? 256 : r.len;
        ezhal_flash_read(a + EZLOG_RHDR, buf, l);
        buf[l] = 0;
        if (out->length()) *out += "\n";
        *out += String((const char *)buf);
    }
    free(pos);
    return HS_OK;
}

void ezhal_flog_stats(int log, EzFlashLogStats *st)
{
    memset(st, 0, sizeof(*st));
    if (log < 0 || log >= EZFLOG_COUNT) return;
    EzLogState &s = ezhal_flog_state[log];
    uint32_t n = ezhal_flog_regions[log].sectors;
    uint32_t used_sectors = 0;
    for (uint32_t i = 0; i < n; i++) if (s.sect_seq[i] != 0) used_sectors++;
    st->sectors = n;
    st->capacity = n * (HAL_FLASH_SECTOR - EZLOG_HDR);
    st->used = used_sectors == 0 ? 0 : (used_sectors - 1) * (HAL_FLASH_SECTOR - EZLOG_HDR) +
                                       ((s.head_off > EZLOG_HDR ? s.head_off : EZLOG_HDR) - EZLOG_HDR);
    if (st->used > st->capacity) st->used = st->capacity;
    st->records = s.records;
    st->pending = s.pending;
    st->dropped = s.dropped;
    st->corrupt = s.corrupt;
    st->max_erase = s.max_erase;
}

HalStatus ezhal_flog_format(int log)
{
    if (log < 0 || log >= EZFLOG_COUNT) return HS_ERROR;
    for (int i = 0; i < ezhal_flog_regions[log].sectors; i++) {
        if (ezhal_flash_erase_sector(ezhal_flog_addr(log, i, 0)) != HS_OK) return HS_ERROR;
    }
    return ezhal_flog_init(log);
}

// Erase / program / read-back on the scratch sector; also proves NOR program semantics and the
// range guard. Destroys only the scratch sector.
HalStatus ezhal_flash_selftest(String *detail)
{
    static uint8_t buf[HAL_FLASH_SECTOR];
    const uint32_t a = EZHAL_FLASH_SCRATCH;
    bool erase_ok = ezhal_flash_erase_sector(a) == HS_OK;
    ezhal_flash_read(a, buf, HAL_FLASH_SECTOR);
    for (uint32_t i = 0; i < HAL_FLASH_SECTOR && erase_ok; i++) erase_ok = buf[i] == 0xFF;
    uint8_t pat[256];
    for (int i = 0; i < 256; i++) pat[i] = (uint8_t)(i * 7 + 3);
    bool prog_ok = ezhal_flash_program(a, pat, sizeof(pat)) == HS_OK;
    ezhal_flash_read(a, buf, sizeof(pat));
    prog_ok = prog_ok && memcmp(buf, pat, sizeof(pat)) == 0;
    uint8_t x = 0x0F, y = 0xF0, z = 0xAA;
    ezhal_flash_program(a + 512, &x, 1);
    ezhal_flash_program(a + 512, &y, 1);                     // NOR: 0x0F AND 0xF0 = 0x00 without erase
    ezhal_flash_read(a + 512, &z, 1);
    bool nor_and = (z == 0x00);
    bool guard_ok = ezhal_flash_program(0x000000, &x, 1) == HS_ERROR &&      // firmware area refused
                    ezhal_flash_erase_sector(EZHAL_FLASH_USER_BASE - HAL_FLASH_SECTOR) == HS_ERROR &&
                    ezhal_flash_program(EZHAL_FLASH_USER_END - 1, pat, 2) == HS_ERROR;
    ezhal_flash_erase_sector(a);
    *detail = String("erase=") + (erase_ok ? "ok" : "FAIL") + " program=" + (prog_ok ? "ok" : "FAIL") +
              " nor_and=" + (nor_and ? "yes" : "NO") + " guard=" + (guard_ok ? "ok" : "FAIL");
    return (erase_ok && prog_ok && nor_and && guard_ok) ? HS_OK : HS_ERROR;
}

// =============================================================================================
// File system: LittleFS on EZHAL_FS_* (portable: uses only ezhal_fsflash_*, host-tested with LittleFS 2.4.1)
// =============================================================================================
static lfs_t g_lfs;
static struct lfs_config g_lfscfg;
static bool g_fsMounted = false;
static SemaphoreHandle_t g_fsLock = NULL;              // LittleFS is built with LFS_THREADSAFE
static SemaphoreHandle_t g_fsOp = NULL;                // serialises our multi-call operations (write + rename)

static int ezfs_bd_read(const struct lfs_config *c, lfs_block_t b, lfs_off_t off, void *buf, lfs_size_t size)
{
    (void)c;
    return ezhal_fsflash_read(EZHAL_FS_BASE + b * EZHAL_FS_BLOCK + off, buf, size) == HS_OK ? 0 : LFS_ERR_IO;
}

static int ezfs_bd_prog(const struct lfs_config *c, lfs_block_t b, lfs_off_t off, const void *buf, lfs_size_t size)
{
    (void)c;
    return ezhal_fsflash_program(EZHAL_FS_BASE + b * EZHAL_FS_BLOCK + off, buf, size) == HS_OK ? 0 : LFS_ERR_IO;
}

static int ezfs_bd_erase(const struct lfs_config *c, lfs_block_t b)
{
    (void)c;
    return ezhal_fsflash_erase(EZHAL_FS_BASE + b * EZHAL_FS_BLOCK) == HS_OK ? 0 : LFS_ERR_IO;
}

static int ezfs_bd_sync(const struct lfs_config *c)
{
    (void)c;
    return 0;
}

static int ezfs_lock(const struct lfs_config *c)
{
    (void)c;
    xSemaphoreTake(g_fsLock, portMAX_DELAY);
    return 0;
}

static int ezfs_unlock(const struct lfs_config *c)
{
    (void)c;
    xSemaphoreGive(g_fsLock);
    return 0;
}

static void ezfs_config()
{
    memset(&g_lfscfg, 0, sizeof(g_lfscfg));
    g_lfscfg.read = ezfs_bd_read;
    g_lfscfg.prog = ezfs_bd_prog;
    g_lfscfg.erase = ezfs_bd_erase;
    g_lfscfg.sync = ezfs_bd_sync;
    g_lfscfg.lock = ezfs_lock;
    g_lfscfg.unlock = ezfs_unlock;
    g_lfscfg.read_size = 16;
    g_lfscfg.prog_size = 16;
    g_lfscfg.block_size = EZHAL_FS_BLOCK;
    g_lfscfg.block_count = EZHAL_FS_BLOCKS;
    g_lfscfg.block_cycles = 500;                       // wear levelling: relocate metadata after 500 erases
    g_lfscfg.cache_size = 256;
    g_lfscfg.lookahead_size = 128;                     // 1024 blocks per allocator scan
}

// never formatted: the first two blocks (superblock pair) are fully erased
static bool ezfs_blank()
{
    uint8_t buf[256];
    for (uint32_t a = 0; a < 2 * EZHAL_FS_BLOCK; a += sizeof(buf)) {
        if (ezhal_fsflash_read(EZHAL_FS_BASE + a, buf, sizeof(buf)) != HS_OK) return false;
        for (unsigned i = 0; i < sizeof(buf); i++) {
            if (buf[i] != 0xFF) return false;
        }
    }
    return true;
}

static void ezfs_locks()
{
    if (g_fsLock == NULL) g_fsLock = xSemaphoreCreateMutex();
    if (g_fsOp == NULL) g_fsOp = xSemaphoreCreateMutex();
}

HalStatus ezhal_fs_mount_region(bool format_if_blank, String *detail)
{
    if (g_fsMounted) {
        *detail = "already mounted";
        return HS_OK;
    }
    ezfs_locks();
    ezfs_config();
    int err = lfs_mount(&g_lfs, &g_lfscfg);
    String pre = "";
    if (err != 0) {
        // only an untouched region is formatted automatically; a damaged file system needs FS_FORMAT YES
        if (!(format_if_blank && ezfs_blank())) {
            *detail = "mount failed (" + String(err) + "): FS_FORMAT YES erases the file system";
            return HS_ERROR;
        }
        err = lfs_format(&g_lfs, &g_lfscfg);
        if (err == 0) err = lfs_mount(&g_lfs, &g_lfscfg);
        if (err != 0) {
            *detail = "format failed (" + String(err) + ")";
            return HS_ERROR;
        }
        pre = "formatted new file system, ";
    }
    g_fsMounted = true;
    uint32_t used = 0, total = 0;
    ezhal_fs_usage(&used, &total);
    *detail = pre + "mounted " + String(total / 1024u) + " KB, used " + String(used / 1024u) + " KB";
    return HS_OK;
}

void ezhal_fs_unmount()
{
    if (g_fsMounted) lfs_unmount(&g_lfs);
    g_fsMounted = false;
}

bool ezhal_fs_mounted()
{
    return g_fsMounted;
}

HalStatus ezhal_fs_format()
{
    ezfs_locks();
    ezhal_fs_unmount();
    ezfs_config();
    if (lfs_format(&g_lfs, &g_lfscfg) != 0 || lfs_mount(&g_lfs, &g_lfscfg) != 0) return HS_ERROR;
    g_fsMounted = true;
    return HS_OK;
}

static HalStatus ezfs_write(const char *path, const uint8_t *data, uint32_t len, int flags)
{
    lfs_file_t f;
    if (lfs_file_open(&g_lfs, &f, path, flags) < 0) return HS_ERROR;
    lfs_ssize_t n = len ? lfs_file_write(&g_lfs, &f, data, len) : 0;
    int cerr = lfs_file_close(&g_lfs, &f);
    return (n == (lfs_ssize_t)len && cerr == 0) ? HS_OK : HS_ERROR;
}

static void ezfs_mkparents(const char *path)
{
    char buf[LFS_NAME_MAX + 1];
    size_t n = strlen(path);
    if (n > LFS_NAME_MAX) return;
    for (size_t i = 1; i < n; i++) {
        if (path[i] == '/') {
            memcpy(buf, path, i);
            buf[i] = 0;
            lfs_mkdir(&g_lfs, buf);                     // LFS_ERR_EXIST is fine
        }
    }
}

HalStatus ezhal_fs_write_file(const char *path, const uint8_t *data, uint32_t len)
{
    if (!g_fsMounted || path == NULL || strlen(path) + 5 > LFS_NAME_MAX) return HS_ERROR;
    xSemaphoreTake(g_fsOp, portMAX_DELAY);
    ezfs_mkparents(path);
    String tmp = String(path) + ".tmp";               // write aside, then rename atomically over the old file
    HalStatus st = ezfs_write(tmp.c_str(), data, len, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (st == HS_OK && lfs_rename(&g_lfs, tmp.c_str(), path) != 0) st = HS_ERROR;
    if (st != HS_OK) lfs_remove(&g_lfs, tmp.c_str());
    xSemaphoreGive(g_fsOp);
    return st;
}

HalStatus ezhal_fs_append(const char *path, const uint8_t *data, uint32_t len)
{
    if (!g_fsMounted || path == NULL) return HS_ERROR;
    xSemaphoreTake(g_fsOp, portMAX_DELAY);
    ezfs_mkparents(path);
    HalStatus st = ezfs_write(path, data, len, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_APPEND);
    xSemaphoreGive(g_fsOp);
    return st;
}

int32_t ezhal_fs_read(const char *path, uint32_t offset, uint8_t *buf, uint32_t len)
{
    if (!g_fsMounted || path == NULL) return -1;
    lfs_file_t f;
    if (lfs_file_open(&g_lfs, &f, path, LFS_O_RDONLY) < 0) return -1;
    int32_t r = -1;
    if (lfs_file_seek(&g_lfs, &f, offset, LFS_SEEK_SET) >= 0) r = lfs_file_read(&g_lfs, &f, buf, len);
    lfs_file_close(&g_lfs, &f);
    return r < 0 ? -1 : r;
}

int32_t ezhal_fs_size(const char *path)
{
    struct lfs_info info;
    if (!g_fsMounted || lfs_stat(&g_lfs, path, &info) < 0 || info.type != LFS_TYPE_REG) return -1;
    return (int32_t)info.size;
}

bool ezhal_fs_exists(const char *path)
{
    struct lfs_info info;
    return g_fsMounted && lfs_stat(&g_lfs, path, &info) >= 0;
}

HalStatus ezhal_fs_remove(const char *path)
{
    return (g_fsMounted && lfs_remove(&g_lfs, path) >= 0) ? HS_OK : HS_ERROR;
}

HalStatus ezhal_fs_rename(const char *from, const char *to)
{
    return (g_fsMounted && lfs_rename(&g_lfs, from, to) >= 0) ? HS_OK : HS_ERROR;
}

HalStatus ezhal_fs_mkdir(const char *path)
{
    if (!g_fsMounted) return HS_ERROR;
    ezfs_mkparents(path);
    int e = lfs_mkdir(&g_lfs, path);
    return (e >= 0 || e == LFS_ERR_EXIST) ? HS_OK : HS_ERROR;
}

HalStatus ezhal_fs_list(const char *dir, String *out)
{
    *out = "";
    if (!g_fsMounted) return HS_ERROR;
    lfs_dir_t d;
    if (lfs_dir_open(&g_lfs, &d, dir) < 0) return HS_ERROR;
    struct lfs_info info;
    while (lfs_dir_read(&g_lfs, &d, &info) > 0) {
        if (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0) continue;
        if (info.type == LFS_TYPE_DIR) *out += String(info.name) + "/ -\n";
        else *out += String(info.name) + " " + String((uint32_t)info.size) + "\n";
    }
    lfs_dir_close(&g_lfs, &d);
    return HS_OK;
}

HalStatus ezhal_fs_usage(uint32_t *used_bytes, uint32_t *total_bytes)
{
    *total_bytes = EZHAL_FS_BLOCKS * EZHAL_FS_BLOCK;
    *used_bytes = 0;
    if (!g_fsMounted) return HS_ERROR;
    lfs_ssize_t blocks = lfs_fs_size(&g_lfs);
    if (blocks < 0) return HS_ERROR;
    *used_bytes = (uint32_t)blocks * EZHAL_FS_BLOCK;
    return HS_OK;
}

// =============================================================================================
// Radio (Wi-Fi). OP-02: a HaLow implementation replaces this section.
// =============================================================================================
HalStatus ezhal_radio_begin(const char *ssid, const char *pass)
{
    WiFi.begin((char *)ssid, pass);
    return WiFi.status() == WL_CONNECTED ? HS_OK : HS_ERROR;
}

void ezhal_radio_disconnect()
{
    WiFi.disconnect();
}

bool ezhal_radio_connected()
{
    return WiFi.status() == WL_CONNECTED;
}

int32_t ezhal_radio_rssi()
{
    return WiFi.RSSI();
}

void ezhal_radio_mac(uint8_t mac[6])
{
    WiFi.macAddress(mac);
}

IPAddress ezhal_radio_local_ip()
{
    return WiFi.localIP();
}

String ezhal_radio_local_ip_str()
{
    return (String)WiFi.localIP().get_address();
}

bool ezhal_radio_resolve(const char *host, IPAddress *ip)
{
    return WiFi.hostByName(host, *ip) != 0;
}

// =============================================================================================
// DataSocket - long-lived data/command socket (methods named like WiFiClient)
// Why not WiFiClient: in core 4.1.0 WiFiClient::available() decides "disconnected" from the GLOBAL errno,
// which other threads change; a healthy socket was then closed. Non-blocking recv + per-socket SO_ERROR.
// =============================================================================================
int DataSocket::connect(const char *host, uint16_t port)
{
    stop();
    IPAddress ip;
    if (!ezhal_radio_resolve(host, &ip)) return 0;
    _sock = ezhal_tcp_connect((uint32_t)ip, port);
    _rxLen = _rxPos = 0;
    return _sock >= 0 ? 1 : 0;
}

uint8_t DataSocket::connected()
{
    if (_sock < 0) return 0;
    if (_rxPos >= _rxLen) fill();               // also detects peer close / reset
    return _sock >= 0 ? 1 : 0;
}

int DataSocket::available()
{
    if (_sock < 0) return 0;
    if (_rxPos >= _rxLen) fill();
    return _rxLen - _rxPos;
}

int DataSocket::read(uint8_t *buf, size_t size)
{
    int n = 0;
    while ((size_t)n < size && available() > 0) {
        buf[n++] = _rx[_rxPos++];
    }
    return n;
}

size_t DataSocket::write(const uint8_t *buf, size_t size)
{
    if (_sock < 0) return 0;
    if (ezhal_tcp_send(_sock, buf, (int)size) != (int)size) {
        // only after 30 s without any progress (SO_SNDTIMEO) or a hard socket error
        Serial.println("DataSocket: send failed, so_error=" + String(ezhal_tcp_error(_sock)) + " -> reconnect");
        stop();
        return 0;
    }
    return size;
}

void DataSocket::stop()
{
    if (_sock >= 0) ezhal_tcp_close(_sock);
    _sock = -1;
    _rxLen = _rxPos = 0;
}

void DataSocket::fill()
{
    _rxLen = _rxPos = 0;
    int n = ezhal_tcp_recv(_sock, _rx, sizeof(_rx));
    if (n > 0) {
        _rxLen = n;
    } else if (n == -1) {
        Serial.println("DataSocket: closed by server -> reconnect");
        stop();
    } else if (n < -1) {
        Serial.println("DataSocket: socket error " + String(-n) + " -> reconnect");
        stop();
    }
}

// =============================================================================================
// Hardware watchdog
// =============================================================================================
static WDT g_wdt(0);                             // 0: system WDT (not the AON WDT)

void ezhal_wdt_start(uint32_t timeout_ms)
{
    g_wdt.init(timeout_ms);
    g_wdt.start();
}

void ezhal_wdt_refresh()
{
    g_wdt.refresh();
}

// =============================================================================================
// System
// =============================================================================================
// Clean software reset. REBOOT, watchdog-thread and network resets restart the CPU but not the flash chip;
// after 1.5.0 the boot ROM then reported "[SPIF Err]Invalid ID" on the 32 MB board before recovering.
// Wait until no flash operation is in progress, put the flash back into its power-on mode (3-byte
// addressing, plain SPI), then reset. The flash lock is never released: the reset follows at once.
#ifndef EZHAL_CLEAN_FLASH_RESET
#define EZHAL_CLEAN_FLASH_RESET 1
#endif
void ezhal_sys_reset()
{
#if EZHAL_CLEAN_FLASH_RESET
    uint32_t size = ezhal_flash_size();                     // cached; must be read before taking the lock
    device_mutex_lock(RT_DEV_LOCK_FLASH);
    if (g_flash.phal_spic_adaptor != NULL) {
        if (size >= 0x2000000u) hal_flash_exit_4byte_addr(g_flash.phal_spic_adaptor);
        hal_flash_reset_to_spi(g_flash.phal_spic_adaptor);
    }
#endif
    sys_reset();
}

uint32_t ezhal_heap_free()
{
    return (uint32_t)os_get_free_heap_size_arduino();
}

uint32_t ezhal_heap_min()
{
    return (uint32_t)xPortGetMinimumEverFreeHeapSize();
}

// =============================================================================================
// Raw lwIP: TCP helpers + SNTP.  KEEP THIS SECTION AT THE VERY END OF THE FILE.
// Why not WiFiUDP / NTPClient: in core 4.1.0 WiFiUDP::writeImmediately() sends on hard-coded socket 0
// and endPacket() then closes socket 0 - the RTSP listener.
// =============================================================================================
#include <lwip/sockets.h>

// server_ip: IPv4 address in network byte order. Returns Unix epoch seconds, or 0 on failure.
uint32_t ezhal_ntp_query(uint32_t server_ip, int timeout_ms)
{
    int s = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        return 0;
    }
    lwip_setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout_ms, sizeof(timeout_ms));

    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(123);
    to.sin_addr.s_addr = server_ip;

    uint8_t pkt[48];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x1B;                       // LI 0, version 3, mode 3 (client)

    uint32_t epoch = 0;
    if (lwip_sendto(s, pkt, sizeof(pkt), 0, (struct sockaddr *)&to, sizeof(to)) == (int)sizeof(pkt)) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int n = lwip_recvfrom(s, pkt, sizeof(pkt), 0, (struct sockaddr *)&from, &fromlen);
        if (n >= 44 && from.sin_addr.s_addr == server_ip) {
            uint32_t secs = ((uint32_t)pkt[40] << 24) | ((uint32_t)pkt[41] << 16) |
                            ((uint32_t)pkt[42] << 8) | (uint32_t)pkt[43];
            if (secs > 2208988800UL) {
                epoch = secs - 2208988800UL;      // NTP era 1900 -> Unix 1970
            }
        }
    }
    lwip_close(s);
    return epoch;
}

// ---- Raw TCP helpers for DataSocket (no use of the shared errno) ----
int ezhal_tcp_connect(uint32_t server_ip, uint16_t port)
{
    int s = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) {
        return -1;
    }
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr.s_addr = server_ip;
    if (lwip_connect(s, (struct sockaddr *)&to, sizeof(to)) < 0) {
        lwip_close(s);
        return -1;
    }
    int one = 1;
    lwip_setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));      // detect dead peers
    lwip_setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));      // small records go out at once
    int sndTimeoutMs = 30000;     // same as the SDK's own sockets; JPEG uploads can starve TCP buffers for seconds
    lwip_setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &sndTimeoutMs, sizeof(sndTimeoutMs));
    return s;
}

// Returns len on success, -1 on error (connection unusable).
int ezhal_tcp_send(int sock, const uint8_t *buf, int len)
{
    int off = 0;
    while (off < len) {
        int n = lwip_send(sock, buf + off, len - off, 0);
        if (n <= 0) {
            return -1;
        }
        off += n;
    }
    return len;
}

// Pending error of THIS socket (SO_ERROR), never the shared errno. 0 = none, -1 = query failed.
int ezhal_tcp_error(int sock)
{
    int err = 0;
    socklen_t elen = sizeof(err);
    if (lwip_getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &elen) < 0) {
        return -1;
    }
    return err;
}

// Non-blocking. Returns >0 bytes read, 0 when no data is waiting (or anything transient),
// -1 when the server closed the connection, -<errno> for a fatal connection error.
int ezhal_tcp_recv(int sock, uint8_t *buf, int len)
{
    int n = lwip_recv(sock, buf, len, MSG_DONTWAIT);
    if (n > 0) {
        return n;
    }
    if (n == 0) {
        return -1;                               // orderly close (FIN) by the server
    }
    int err = ezhal_tcp_error(sock);
    switch (err) {
        case ECONNRESET:
        case ECONNABORTED:
        case ENOTCONN:
        case EPIPE:
        case ETIMEDOUT:                          // keepalive gave up
        case EHOSTUNREACH:
            return -err;
        default:
            return 0;                            // no data yet / transient (incl. failed query)
    }
}

void ezhal_tcp_close(int sock)
{
    lwip_close(sock);
}
