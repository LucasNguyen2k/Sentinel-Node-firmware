/*
 * ezsn_hal.h - Hardware Abstraction Layer of the EyeZense Sentinel Node firmware (FW-02)
 *
 * The application (sentinel_node.ino) talks to hardware ONLY through the functions declared here.
 * Each hardware group has one section with the same shape:
 *     hal_<group>_init()      bring the device up, returns false if absent or failed
 *     hal_<group>_present()   device fitted and initialised
 *     hal_<group>_read(...)   one measurement / status, returns a HalStatus
 * Groups whose hardware is not fitted yet (ultrasonic OP-01, power OP-04, tamper OP-08) are complete
 * stubs that return HS_NOT_PRESENT; adding the hardware means filling in their section in ezsn_hal.cpp only.
 *
 * Platform: Realtek AMB82-MINI (RTL8735B), Ameba Pro2 Arduino core 4.1.0.
 * Threading: HAL functions are NOT internally locked. The application serialises access per device
 * (camMutex, sdMutex, flashMutex, sockMutex in the sketch). The microphone callback runs in the
 * multimedia task and must not block.
 */
#ifndef EZSN_HAL_H
#define EZSN_HAL_H

#include <Arduino.h>
#include <IPAddress.h>
#include "VideoStream.h"         // MMFModule (microphone tap is a multimedia sink module)

#define EZSN_HAL_VERSION "1.0.0"

enum HalStatus {
    HS_OK = 0,
    HS_ERROR = -1,              // device present but the operation failed
    HS_NOT_PRESENT = -2,        // hardware not fitted / not supported in this build
};

// ---------------------------------------------------------------------------------------------
// Pin map (AMB82-MINI)
// ---------------------------------------------------------------------------------------------
#define HAL_PIN_CHEMICAL      A0     // AMB_D11 / PF_0, 10-bit ADC, 0-3.3 V
//#define HAL_PIN_ULTRASONIC  A1     // OP-01: envelope-detector output (define when fitted)
// AHT20 on the default I2C bus: SDA = AMB_D12, SCL = AMB_D13

// ---------------------------------------------------------------------------------------------
// Chemical (gas) sensor - analog MOS sensor on HAL_PIN_CHEMICAL
// ---------------------------------------------------------------------------------------------
#define HAL_GAS_ADC_MAX   1023
#define HAL_GAS_VREF      3.3f
bool      ezhal_gas_init();
bool      ezhal_gas_present();
HalStatus ezhal_gas_read_raw(int *raw);          // 0..HAL_GAS_ADC_MAX
float     ezhal_gas_raw_to_volts(float raw);

// ---------------------------------------------------------------------------------------------
// Ambient temperature / humidity - AHT20 (I2C)
// ---------------------------------------------------------------------------------------------
// AHT20 (I2C 0x38), own driver: 7-byte frame with CRC-8, busy and calibration bits checked, so a missing
// sensor (I2C reads of all 0x00 / 0xFF) can no longer be mistaken for a reading of -50 C / 0 %.
enum EzAhtResult { AHT_OK = 0, AHT_BUSY = 1, AHT_CRC = 2, AHT_BLANK = 3, AHT_RANGE = 4 };
uint8_t   ezhal_aht_crc8(const uint8_t *data, int len);
int       ezhal_aht_decode(const uint8_t frame[7], float *temp_c, float *rh_pct);   // EzAhtResult
bool      ezhal_env_init();
bool      ezhal_env_present();
HalStatus ezhal_env_read(float *temp_c, float *rh_pct);    // cached reading only, never touches I2C; rh in %RH
void      ezhal_env_service(uint32_t now_ms);        // AHT20 I2C work: call every ~100 ms from its own thread
bool      ezhal_aht_trigger();                       // start a measurement (false = no ACK)
void      ezhal_aht_read_frame(uint8_t frame[7]);

// ---------------------------------------------------------------------------------------------
// Ultrasonic leak sensor - OP-01 (stub until hardware is fitted)
// ---------------------------------------------------------------------------------------------
bool      ezhal_us_init();
bool      ezhal_us_present();
HalStatus ezhal_us_read(float *level);           // raw envelope level

// ---------------------------------------------------------------------------------------------
// Power system: battery, charger, solar - OP-03/OP-04 (stub until hardware is fitted)
// ---------------------------------------------------------------------------------------------
struct HalPowerStatus {
    bool  present;
    float battery_v;
    float soc_pct;
    float charge_ma;
    float solar_v;
    bool  charging;
    bool  fault;
};
bool      ezhal_power_init();
bool      ezhal_power_present();
HalStatus ezhal_power_read(HalPowerStatus *st);

// ---------------------------------------------------------------------------------------------
// Tamper / movement: accelerometer + enclosure switch - OP-08 (stub until hardware is fitted)
// ---------------------------------------------------------------------------------------------
struct HalTamperStatus {
    bool  present;
    bool  moved;
    bool  enclosure_open;
    bool  detached;
    float tilt_deg;
};
bool      ezhal_tamper_init();
bool      ezhal_tamper_present();
HalStatus ezhal_tamper_read(HalTamperStatus *st);

// ---------------------------------------------------------------------------------------------
// Internal temperature - RTL8735B RF thermal meter (raw index; degC needs calibration, OP-09)
// ---------------------------------------------------------------------------------------------
HalStatus ezhal_temp_internal_raw(int32_t *raw);   // needs the radio to be up

// ---------------------------------------------------------------------------------------------
// Microphone tap - raw 16-bit PCM frames from the audio pipeline (16 kHz mono)
// The sketch builds the media pipeline (Audio -> encoder -> RTSP) and registers ezhal_mic_tap_module()
// as an extra StreamIO output; every frame is then passed to the callback.
// ---------------------------------------------------------------------------------------------
typedef void (*HalMicFrameCallback)(const int16_t *samples, uint32_t count);
bool       ezhal_mic_tap_init(HalMicFrameCallback cb);
bool       ezhal_mic_present();                    // tap initialised
MMFModule &ezhal_mic_tap_module();

// ---------------------------------------------------------------------------------------------
// Camera - channel control and JPEG snapshot (channel configuration stays in the media pipeline)
// ---------------------------------------------------------------------------------------------
HalStatus ezhal_cam_start(int channel, uint32_t settle_ms);
HalStatus ezhal_cam_stop(int channel);
bool      ezhal_cam_running(int channel);
#define HAL_CAM_SNAPSHOT_TIMEOUT_MS 5000
// JPEG snapshot. The SDK's Camera.getImage() waits forever when the video engine is not running ("VOE not
// init": camera missing, wrong sensor option); here it runs in a worker thread and the caller gives up after
// HAL_CAM_SNAPSHOT_TIMEOUT_MS. While a call is stuck in the SDK, further snapshots fail at once.
HalStatus ezhal_cam_snapshot(int channel, uint32_t *addr, uint32_t *len);
uint32_t  ezhal_cam_timeouts();       // snapshots that got no frame in time since boot
bool      ezhal_cam_stuck();          // a snapshot is still waiting inside the SDK   // buffer owned by the driver

// ---------------------------------------------------------------------------------------------
// SD card (FatFS) - paths are relative to the card root, e.g. "ezsn/events.csv"
// ---------------------------------------------------------------------------------------------
bool      ezhal_sd_mount();
bool      ezhal_sd_present();
HalStatus ezhal_sd_mkdir(const String &rel);
bool      ezhal_sd_exists(const String &rel);
HalStatus ezhal_sd_remove(const String &rel);
HalStatus ezhal_sd_rename(const String &rel_from, const String &rel_to);
int32_t   ezhal_sd_size(const String &rel);                            // -1 if missing
HalStatus ezhal_sd_append(const String &rel, const uint8_t *data, uint32_t len);
HalStatus ezhal_sd_write(const String &rel, const uint8_t *data, uint32_t len);   // replace file
int32_t   ezhal_sd_read_at(const String &rel, uint32_t offset, uint8_t *buf, uint32_t len);
HalStatus ezhal_sd_read_line(const String &rel, uint32_t offset, String *line, uint32_t *next_offset);
HalStatus ezhal_sd_start_ota(const String &rel);                       // reboots when done

// ---------------------------------------------------------------------------------------------
// Internal flash (NOR). Only the user area 0xFD0000..0xFFFFFF (48 x 4 KB sectors, 192 KB) may be
// written or erased; every write/erase API refuses addresses outside it, so a bug can never touch
// the firmware, bootloader or calibration partitions. Layout used by the firmware:
// ---------------------------------------------------------------------------------------------
#define HAL_FLASH_SECTOR                 4096
#define EZHAL_FLASH_USER_BASE            0xFD0000u
#define EZHAL_FLASH_USER_END             0x1000000u
#define EZHAL_FLASH_CFG_A                0xFD0000u   // config slot A (also the single-sector format of <= 1.3.2)
#define EZHAL_FLASH_BOOTINFO             0xFD1000u   // boot counter, pending reset reason
#define EZHAL_FLASH_CFG_B                0xFD2000u   // config slot B
#define EZHAL_FLASH_SCRATCH              0xFD3000u   // self-test scratch sector (FLASH_TEST)
#define EZHAL_FLASH_LOG_EVENTS_BASE      0xFD4000u   // event log ring: 8 sectors = 32 KB
#define EZHAL_FLASH_LOG_EVENTS_SECTORS   8
#define EZHAL_FLASH_LOG_BUFFER_BASE      0xFDC000u   // store-and-forward ring: 36 sectors = 144 KB
#define EZHAL_FLASH_LOG_BUFFER_SECTORS   36

bool      ezhal_flash_range_ok(uint32_t addr, uint32_t len);            // inside the user area
HalStatus ezhal_flash_read(uint32_t addr, void *buf, uint32_t len);
HalStatus ezhal_flash_write_sector(uint32_t addr, const void *buf, uint32_t len);   // erase + write + verify
HalStatus ezhal_flash_erase_sector(uint32_t addr);                      // sector-aligned, sets bytes to 0xFF
HalStatus ezhal_flash_program(uint32_t addr, const void *buf, uint32_t len);  // NOR program: bits 1->0, no erase
HalStatus ezhal_flash_selftest(String *detail);                         // erase/program/verify on the scratch sector
// Bench probe for boards with more than 16 MB of flash: JEDEC ID, extended-address register, whether
// addresses above 16 MB wrap to the lower half, and (write_test) whether the upper half is writable via
// the extended-address register. All writes go to offset EZHAL_FLASH_SCRATCH, in the lower half or the
// same offset in the upper half, so even a misrouted write only lands in our own scratch sector.
HalStatus ezhal_flash_probe(bool write_test, String *report);
uint32_t  ezhal_crc32(const uint8_t *data, uint32_t len);

// ---------------------------------------------------------------------------------------------
// File system (LittleFS 2.4.1, compiled into the Ameba core) on the upper 16 MB of a 32 MB flash.
// Only boards whose flash reports 32 MB (JEDEC capacity 0x19) get a file system. The region excludes
// the mirror of the lower reserved area, so 0x1FD3000 stays free for the write check: before the first
// mount after boot, a plain-address write there must land in the upper half and leave the lower scratch
// sector (its 16 MB alias) untouched; otherwise the file system is not mounted.
// ---------------------------------------------------------------------------------------------
#define EZHAL_FS_BASE        0x1000000u
#define EZHAL_FS_BLOCK       4096u
#define EZHAL_FS_BLOCKS      4048u                      // 0x1000000..0x1FCFFFF (15.8 MB)
#define EZHAL_FS_CHECK_ADDR  0x1FD3000u                 // write-check sector (alias of EZHAL_FLASH_SCRATCH)
uint32_t  ezhal_flash_size();                           // bytes, from the JEDEC ID (0 = unknown)
bool      ezhal_fsflash_range_ok(uint32_t addr, uint32_t len);
HalStatus ezhal_fsflash_read(uint32_t addr, void *buf, uint32_t len);
HalStatus ezhal_fsflash_program(uint32_t addr, const void *buf, uint32_t len);
HalStatus ezhal_fsflash_erase(uint32_t addr);
HalStatus ezhal_fs_upper_write_check(String *detail);

HalStatus ezhal_fs_mount(bool format_if_blank, String *detail);   // size + write check, then mount (portable part below)
HalStatus ezhal_fs_mount_region(bool format_if_blank, String *detail);
void      ezhal_fs_unmount();
bool      ezhal_fs_mounted();
HalStatus ezhal_fs_format();
HalStatus ezhal_fs_write_file(const char *path, const uint8_t *data, uint32_t len);   // create/replace atomically
HalStatus ezhal_fs_append(const char *path, const uint8_t *data, uint32_t len);
int32_t   ezhal_fs_read(const char *path, uint32_t offset, uint8_t *buf, uint32_t len); // bytes read, -1 = error
int32_t   ezhal_fs_size(const char *path);                        // -1 = missing
bool      ezhal_fs_exists(const char *path);
HalStatus ezhal_fs_remove(const char *path);                      // file or empty directory
HalStatus ezhal_fs_rename(const char *from, const char *to);      // atomic, replaces `to`
HalStatus ezhal_fs_mkdir(const char *path);                       // creates parents; ok if it exists
HalStatus ezhal_fs_list(const char *dir, String *out);            // "name size\n", directories as "name/ -"
HalStatus ezhal_fs_usage(uint32_t *used_bytes, uint32_t *total_bytes);

// A/B record store: a fixed-size record kept in two sectors. A write always goes to the slot that does
// NOT hold the newest valid copy (payload first, header last), so a power cut during a write leaves the
// previous copy readable. Read returns the valid copy with the highest sequence number.
HalStatus ezhal_frec_read(uint32_t slot_a, uint32_t slot_b, void *buf, uint32_t len, uint32_t *seq);
HalStatus ezhal_frec_write(uint32_t slot_a, uint32_t slot_b, const void *buf, uint32_t len);

// Flash logs: append-only rings of variable-length records (max 4072 bytes) over a range of sectors.
// Each record is committed by programming a flag after its data, and marked consumed by programming the
// flag again - both without erasing - so a power cut can never return half a record, and replayed
// records are not sent twice after a reboot. When a ring is full the oldest sector is erased (its
// unconsumed records are counted as dropped). Sectors are used in rotation (even wear).
enum EzFlashLogId { EZFLOG_EVENTS = 0, EZFLOG_BUFFER = 1, EZFLOG_COUNT = 2 };
struct EzFlashLogStats {
    uint32_t sectors;      // sectors in the ring
    uint32_t capacity;     // payload bytes the ring can hold
    uint32_t used;         // bytes in use (approximate)
    uint32_t records;      // committed records present (pending + consumed)
    uint32_t pending;      // committed, not yet consumed
    uint32_t dropped;      // unconsumed records lost to ring overflow since init
    uint32_t corrupt;      // records skipped because of a bad CRC since init
    uint32_t max_erase;    // highest erase count of any sector (wear)
};
HalStatus ezhal_flog_init(int log);                                     // scan flash, rebuild state (call at boot)
HalStatus ezhal_flog_append(int log, const uint8_t *data, uint16_t len);
HalStatus ezhal_flog_peek(int log, uint8_t *buf, uint16_t cap, uint16_t *len);   // oldest pending record
HalStatus ezhal_flog_consume(int log);                                  // mark the peeked record as sent
uint32_t  ezhal_flog_pending(int log);
HalStatus ezhal_flog_read_recent(int log, uint32_t max_records, String *out);    // newest N, oldest first, '\n'-joined
void      ezhal_flog_stats(int log, EzFlashLogStats *st);
HalStatus ezhal_flog_format(int log);                                   // erase the whole ring

// ---------------------------------------------------------------------------------------------
// Radio - Wi-Fi today; a HaLow implementation (OP-02) replaces this section only
// ---------------------------------------------------------------------------------------------
HalStatus ezhal_radio_begin(const char *ssid, const char *pass);   // blocking attempt
void      ezhal_radio_disconnect();
bool      ezhal_radio_connected();
int32_t   ezhal_radio_rssi();
void      ezhal_radio_mac(uint8_t mac[6]);
IPAddress ezhal_radio_local_ip();
String    ezhal_radio_local_ip_str();
bool      ezhal_radio_resolve(const char *host, IPAddress *ip);

// ---------------------------------------------------------------------------------------------
// TCP / UDP on raw lwIP (avoids the WiFiClient / WiFiUDP defects of core 4.1.0, design doc 8.1)
// ---------------------------------------------------------------------------------------------
int       ezhal_tcp_connect(uint32_t server_ip, uint16_t port);       // socket or -1
int       ezhal_tcp_send(int sock, const uint8_t *buf, int len);       // len or -1
int       ezhal_tcp_recv(int sock, uint8_t *buf, int len);             // >0 bytes, 0 none, -1 closed, -errno
int       ezhal_tcp_error(int sock);                                   // SO_ERROR of this socket
void      ezhal_tcp_close(int sock);
uint32_t  ezhal_ntp_query(uint32_t server_ip, int timeout_ms);         // Unix epoch or 0

// Long-lived data/command socket with WiFiClient-compatible methods
class DataSocket {
public:
    int     connect(const char *host, uint16_t port);
    uint8_t connected();
    int     available();
    int     read(uint8_t *buf, size_t size);
    size_t  write(const uint8_t *buf, size_t size);
    void    stop();
private:
    void    fill();
    int     _sock = -1;
    uint8_t _rx[256];
    int     _rxLen = 0;
    int     _rxPos = 0;
};

// ---------------------------------------------------------------------------------------------
// Hardware watchdog
// ---------------------------------------------------------------------------------------------
void ezhal_wdt_start(uint32_t timeout_ms);
void ezhal_wdt_refresh();

// ---------------------------------------------------------------------------------------------
// System
// ---------------------------------------------------------------------------------------------
void     ezhal_sys_reset();
uint32_t ezhal_heap_free();
uint32_t ezhal_heap_min();

#endif  // EZSN_HAL_H
