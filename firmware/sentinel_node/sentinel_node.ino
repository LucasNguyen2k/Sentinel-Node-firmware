/*
sentinel_node.ino - EyeZense Pipeline Sentinel Node firmware (sketch folder: sentinel_node/, with ezsn_hal.h and ezsn_hal.cpp)

https://github.com/Ameba-AIoT/ameba-arduino-pro2/raw/main/Arduino_package/package_realtek_amebapro2_index.json
BOARD: Realtek Ameba Boards (v4.1.0)
Lib: aWOT (the AHT20 driver is in the HAL since 1.5.0)
AHT20: SDA: 12, SCL: 13
Chemical: 11

I2S (INMP441):
https://github.com/Ameba-AIoT/ameba-arduino-pro2/tree/dev/Arduino_package/hardware (dev branch d3a860b4cfd9d207119ef2ca99c660bae5c74507 (Fri Apr 17 17:26:44 2026 +0800) or build20260320)
https://ameba-doc-arduino-sdk.readthedocs-hosted.com/en/latest/ameba_pro2/amb82-mini/Example_Guides/Multimedia/Audio/I2S%20Audio.html
VDD: 3v3, SCK: 20, L/R: 19, WS: 17, SD: 16

I2S-43434
VDD: 3v3, BCLK: 20, SEL: 19, LRCL: 17, DOUT: 16

gst-launch-1.0 rtspsrc location=rtsp://10.0.0.24:554 latency=100 ! rtppcmadepay ! alawdec ! audioconvert ! audioresample ! wavenc ! filesink buffer-size=2000 location=AMB82_10.0.0.24.wav

==================================================================================================
 HIGH-LEVEL FIRMWARE REQUIREMENTS FW-01..FW-24 - IMPLEMENTATION STATUS (firmware 1.6.1)
 (design: "EyeZense Sentinel Node - Architecture & Detailed Design", section 2.1)
==================================================================================================
 Done
   FW-01 Boot & Initialization: staged boot with [BOOT] markers, offline start if Wi-Fi is absent.
   FW-03 Sensor Data Acquisition: configurable sample_ms / upload_ms, ECO rates.
   FW-04 Sensor Preprocessing: gas moving average, range checks, Rs/R0 -> ppm & %LEL, T/RH
         compensation, alarm hysteresis; acoustic band energy with adaptive noise floor.
   FW-05 Lightweight AI / Sensor Fusion: rule-based chemical + acoustic fusion on the node;
         no image AI on the node; final confirmation stays on the Edge AI Server.
   FW-06 Event Detection & Management: NORMAL/SUSPECT/CONFIRMED, event ids, timestamps, SD log.
   FW-07 Camera Event Control: capture on escalation and on CAPTURE; MP4 clip behind EVENT_VIDEO_CLIP.
   FW-08 Local Data Storage & Buffering: internal flash only (no SD card needed): A/B config, event log
         ring (32 KB), store-and-forward ring (144 KB, ~40 min offline at 1 record/s); SD card optional.
   FW-13 Device Health Monitoring: heap, RSSI, reconnects, boot count, reset reason, raw thermal,
         per-sensor status, self-test mask.
   FW-15 Time & Synchronization: server time in every reply, CMD TIME, optional NTP.
   FW-18 Diagnostics & Logging: reset cause, event log, communication counters, [BOOT] markers.
   FW-20 Fault Recovery / Watchdog: task-aware WDT, network watchdog, Wi-Fi/socket reconnection.
   FW-21 Configuration Management: thresholds, intervals, power mode, network, fusion in flash.
   FW-23 Data Compression & Encoding: JPEG pictures, G.711 A-law audio, H.264 clips (SDK/hardware).
 Partial
   FW-02 Drivers + HAL (ezsn_hal.h / ezsn_hal.cpp): chemical, acoustic, camera, T/RH, storage, flash, Wi-Fi,
         sockets, watchdog done; ultrasonic, power, tamper are HAL stubs (OP-01, OP-04, OP-08);
         HaLow replaces the radio section of the HAL (OP-02).
   FW-09 / FW-10 / FW-24: Wi-Fi + node-initiated socket/HTTP done; HaLow open (OP-02); pictures
         and audio still pulled from node-hosted HTTP/RTSP servers (OD-05).
   FW-11 Power modes: ACTIVE, ECO, event-active, maintenance; sleep and radio power states (OP-05).
   FW-12 Power & Battery Monitoring: raw internal temperature only (OP-04, OP-09).
   FW-16 Cybersecurity: MAC identity, API token, credentials in flash, CLI login, optional TLS-PSK;
         secure boot / firmware integrity open (OD-02).
   FW-17 Firmware Update / OTA: SD-card OTA with guards (OTA-mode build, size, event state, SD lock,
         result after reboot); remote delivery, signing and rollback open (OD-02, OD-08).
   FW-19 Manufacturing & Service Mode: CLI, calibration, diagnostics; no dedicated mfg-test mode yet.
   FW-22 Verification & Testability: self-test + SIM; CI/CD with Jenkins: build matrix, 127 host unit tests,
         server and HIL suites, mutation check, traceability report (repository ci/, tests/).
 Open
   FW-14 Tamper & Movement Monitoring (OP-08).

 OPEN POINTS - DEPEND ON HARDWARE NOT YET AVAILABLE (to be implemented later)
   OP-01  FW-02, FW-05  Ultrasonic channel: needs ultrasonic transducer / high-rate MEMS mic + envelope
                        detector on a free ADC pin. Placeholder: ULTRASONIC_PINOUT, field "us" = -1.
   OP-02  FW-09         Wi-Fi HaLow: needs HaLow module or transparent HaLow bridge. Current link: Wi-Fi.
   OP-03  FW-12         Solar + rechargeable battery system (hardware) to monitor.
   OP-04  FW-12, FW-13  Battery V / SoC / charging / solar V / charger fault: needs fuel gauge or
                        ADC dividers + charger status pins. Low-battery load shedding depends on it.
   OP-05  FW-01, FW-11  Sleep/standby with wake-on-event, radio/camera power states, power budget
                        validation: needs a wake signal on an AON GPIO and the final power system.
   OP-06  FW-02         Magnetic release actuator + attach (Hall) sensor driver. Release command must be
                        authenticated + confirmed when implemented.
   OP-07  none          Enclosure & environmental protection (mechanical).
   OP-08  FW-14         Tamper/movement/orientation/enclosure-open: needs accelerometer + enclosure switch.
   OP-09  FW-12, FW-13  Internal temperature in degC: SDK exposes only the raw RF thermal meter; needs an
                        on-board thermistor or calibration against a reference (itemp_scale/itemp_offset).
   OP-10  FW-02         Robot deployment: external beacon (LED/BLE) + magnet hardware.
   OP-11  none          Hazardous-area compliance (hardware design / certification).

 OTHER DEFERRED ITEMS (not hardware, need data / design work / other teams)
   OD-01  FW-04                Gas curve (gas_a, gas_b, gas_clean_ratio) and compensation (gas_kt, gas_kh)
                               defaults are generic MOS-methane values: set them from the sensor datasheet
                               + calibration gas test.
   OD-02  FW-16, FW-17         Secure boot, signed firmware, update rollback, server-side TLS: key
                               provisioning; not available via the Arduino build flow.
   OD-03  FW-10                Done in sentinel_node_api.py 1.1.0 (/node-status, /node-event, event image
                               POST, '#RSP' replies, extended CSV fields, "CMD#<id>" command correlation).
   OD-04  FW-07                EVENT_VIDEO_CLIP is disabled by default until bench-tested with RTSP/audio.
   OD-05  FW-09, FW-10, FW-24  Remove node-hosted servers from the production data path: node pushes
                               pictures and audio; node HTTP becomes service-mode only.
   OD-06  FW-22                Unit and integration test suite traced to FW-01..FW-24.
==================================================================================================
*/

// All hardware access goes through the Hardware Abstraction Layer (FW-02): ezsn_hal.h / ezsn_hal.cpp in this folder.
#include "ezsn_hal.h"
#include <HttpClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
// NOTE: do not use WiFiUDP / NTPClient in this firmware. In core 4.1.0 WiFiUDP sends on and then
// closes socket 0 (the RTSP listener). NTP is ezhal_ntp_query() on a private lwIP socket (ezsn_hal.cpp).
#include "StreamIO.h"
#include "VideoStream.h"

#include "AudioStream.h"
#include "AudioEncoder.h"
#include "RTSP.h"
#include "aWOT.h"
#include "sys_api.h"
//#include "rtc.h"
#define DEVICE_NAME "EZ_SYS_INTE01"         // Lucas
#define AON_WDT_Enable 0
#define WDT_TIMEOUT 8000
#define CHANNEL 0
//#define STREAMING_VIDEO
#define PRINT_DEBUG false
//#define TESTING
 
#define SERVER_ADDRESS "192.168.100.1"       // Lucas
//#define SERVER_ADDRESS "10.0.0.1"       // Lucas
#define QCS6490 true
#define SERVER_PORT 8900
#define STREAMING_SOCKET true
//#define STREAMING_ONLY true
//#define AUDIO_I2S true                                            // Lucas: using embedded Audio Mic only.

// ---------------- New feature switches (FW-01..FW-24) ----------------
#define FW_VERSION              "1.6.1"
#define EXTENDED_SENSOR_DATA    true          // append ts,seq,gas_ppm,ac_db,ac_hf_db,us,evt,valid to each record
#define ACOUSTIC_DETECTION                    // FW-04/FW-05: on-device acoustic analysis (audio tap)
//#define EVENT_VIDEO_CLIP                    // FW-07: MP4 clip to SD on CONFIRMED event (bench-test first)
//#define USE_TLS_SOCKET                      // FW-16: TLS-PSK on the data socket (server must support it)
//#define ENABLE_NET_OTA                      // FW-17: Realtek network OTA (Tools > OTA Mode: Enable)
#define DATA_STORE_FLASH                      // FW-08: event log + offline buffer in internal flash (no SD card)
#define FLASH_BUF_INTERVAL_MS   800           // offline: keep about one record per upload cycle in flash
//#define ENABLE_SD_STORAGE                   // optional SD card: event pictures, MP4 clips, SD-card OTA
#define PRODUCTION_BUILD        false         // true: serial CLI needs LOGIN <token>
#define API_TOKEN_DEFAULT       "ezsn-change-me"
#define SOCKET_BASE_PORT        4000
#define NET_OTA_PORT            3000
#define TLS_PSK_IDENTITY        "ezsn"
#define TLS_PSK_KEY             "000102030405060708090a0b0c0d0e0f"
#define CONFIG_FLASH_ADDR       0xFD0000      // user flash area (FlashMemory app base)
#define BOOTINFO_FLASH_ADDR     0xFD1000
#define TASK_STALL_MS           120000
#define SD_BUFFER_MAX_BYTES     (8UL * 1024UL * 1024UL)
#define CLIP_CHANNEL            1
// SD-card OTA (FW-17, design doc 4.14)
#define OTA_FILE                "ota.bin"
#define OTA_SLOT_BYTES          0x400000UL         // PT_FW2 in the OTA partition table (4 MB)
#define OTA_MIN_BYTES           (256UL * 1024UL)   // anything smaller cannot be a firmware image
#define OTA_TIMEOUT_MS          (10UL * 60UL * 1000UL)  // success always reboots well before this

// Default preset configurations for each video channel:
// Channel 0 : 1920 x 1080 30FPS H264
// Channel 1 : 1280 x 720  30FPS H264
// Channel 2 : 1280 x 720  30FPS MJPEG

// Default audio preset configurations:
// 0 :  8kHz Mono Analog Mic
// 1 : 16kHz Mono Analog Mic
// 2 :  8kHz Mono Digital PDM Mic
// 3 : 16kHz Mono Digital PDM Mic
VideoSetting configV(CHANNEL);
VideoSetting configPic(VIDEO_HD, CAM_NN_FPS, VIDEO_JPEG, 1);
AudioSetting configA(1);
Audio audio;
AAC aac;
G711E pcm;
RTSP rtsp;

extern "C" {
#include "FreeRTOS.h"
#include "semphr.h"
}

#ifdef ACOUSTIC_DETECTION
// Microphone tap (ezhal_mic_tap_module) is the second output: PCM encoder + acoustic analysis
StreamIO audioStreamer(1, 2);    // 1 Input Audio -> PCM encoder + acoustic analysis tap
#else
StreamIO audioStreamer(1, 1);    // 1 Input Audio -> 1 Output AAC
#endif
#ifdef STREAMING_VIDEO
    StreamIO avMixStreamer(2, 1);    // 2 Input Video + Audio -> 1 Output RTSP
#else
    StreamIO avMixStreamer(1, 1);    // 1 Audio -> 1 Output RTSP
#endif

#ifdef EVENT_VIDEO_CLIP
#include "MP4Recording.h"
VideoSetting configClip(VIDEO_VGA, 15, VIDEO_H264, 0);
MP4Recording mp4;
StreamIO clipStreamer(1, 1);     // Camera channel 1 (H264) -> MP4 on SD
#endif

#if defined(ENABLE_NET_OTA) && !defined(ALLOW_REALTEK_NET_OTA)
#error "ENABLE_NET_OTA: Realtek network OTA opens an unauthenticated port 5000 and its keep-alive WiFiClient closes socket 4 (the data socket). Use OTA_SD (design doc 4.14). Define ALLOW_REALTEK_NET_OTA only for bench tests."
#endif
#ifdef ENABLE_NET_OTA
#include "OTA.h"
OTA ota;
#endif

#ifdef AUDIO_I2S
#include "I2S.h"
    I2SSetting configI2S(16000);
    I2S i2s;
#endif

uint32_t thread_server_id, thread_wd_id, stack_size_server, stack_size_wd;
int priority_server;

#ifdef QCS6490
char ssid[] = "camera_demo";//"TONI";//    //Angel
char pass[] = "12345678";//"Tony@!123";//        //Matthew2043
#else
char ssid[] = "TONI";//"camera_demo";//"TONI";//    //Angel
char pass[] = "Tony@!123";//"12345678";//"Tony@!123";//        //Matthew2043
#endif
int status = WL_IDLE_STATUS;
//WiFiServer server(80, TCP_MODE, NON_BLOCKING_MODE);
WiFiServer server(80);
WiFiClient wifiClient;
#ifdef USE_TLS_SOCKET
WiFiSSLClient clientSocket;     // note: subject to the SDK errno issue described above
#else
DataSocket clientSocket;
#endif
int clientSocketPort = SOCKET_BASE_PORT;
// SDK note: a WiFiClient that never connected holds _sock = MAX_SOCK_NUM (4) and its stop()/destructor
// call close(4), which closes an unrelated live socket (e.g. the RTSP listener -> endless
// "[RTSP Err]rtsp_ctx->client_socket accept error"). Only stop clients that really connected.
bool clientSocketOpened = false;
Application app;
uint32_t img_addr = 0;
uint32_t img_len = 0;

#define SENSOR_DATA_LENGTH 20
String sensorData[SENSOR_DATA_LENGTH]={""};
int sensorDataIdx = 0;
bool bSensorReading = false;
bool bSensorGetting = false;

// ====================================================================================
// Types (declared before any function so Arduino prototype generation works)
// ====================================================================================
#define CONFIG_MAGIC   0x455A534EUL   // "EZSN"
#define CONFIG_VERSION 3

struct NodeConfig {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    char deviceName[32];
    char ssid[33];
    char pass[65];
    char serverAddr[40];
    uint32_t serverPort;
    uint32_t socketBasePort;
    char apiToken[33];
    char ntpServer[40];
    uint8_t secureAll;          // 1: token required on every HTTP endpoint (incl. legacy)
    uint8_t powerMode;          // 0: ACTIVE, 1: ECO
    uint8_t maintenance;        // 1: maintenance mode (alarms suspended)
    uint8_t reserved0;
    uint32_t sampleIntervalMs;
    uint32_t uploadIntervalMs;
    uint32_t heartbeatIntervalS;
    uint32_t netRebootMin;      // reboot after this many minutes without server connectivity
    // Gas (FW-04)
    float gasR0;                // 0 = not calibrated
    float gasRL;                // load resistor (kOhm)
    float gasVc;                // circuit voltage (V)
    float gasCleanAirRatio;     // Rs/R0 in clean air
    float gasCurveA;            // ppm = A * (Rs/R0)^B
    float gasCurveB;
    float gasTempCoef;          // ratio correction per degC from 20 degC
    float gasHumCoef;           // ratio correction per %RH from 65 %RH
    float gasAlarmPpm;
    float gasClearPpm;
    float gasConfirmPpm;        // gas alone above this -> CONFIRMED (0 = disabled)
    uint32_t gasAlarmRaw;       // raw ADC alarm while uncalibrated (0 = disabled)
    uint32_t gasWarmupS;
    // Acoustic (FW-04)
    float acThresholdDb;        // HF level above noise floor to raise alarm
    float acClearDb;
    uint32_t acConfirmWindows;
    // Fusion / events (FW-05, FW-06)
    uint32_t fusionWindowS;
    uint32_t eventHoldS;
    uint32_t clipSeconds;
    // Thermal (FW-13)
    float thermalLimitC;
    float intTempScale;         // degC = raw * scale + offset (scale 0 = not calibrated, OP-09)
    float intTempOffset;
    uint32_t crc;
};

struct BootInfo {
    uint32_t magic;
    uint32_t bootCount;
    uint32_t pendingReason;     // reason written right before an intentional reset
    char pendingDetail[24];
    uint32_t crc;
};

enum ResetReasonCode {
    RR_UNEXPECTED = 0,          // power-on / brown-out / watchdog / crash (not recorded by firmware)
    RR_CMD_REBOOT,
    RR_SOCKET_FAIL,
    RR_REGISTER_FAIL,
    RR_NETWORK_LOST,
    RR_UPLOAD_FAIL,
    RR_TASK_STALL,
    RR_OTA,
    RR_CONFIG
};

enum EventState { EVT_NORMAL = 0, EVT_SUSPECT = 1, EVT_CONFIRMED = 2 };

enum TaskIdx { T_GET = 0, T_UP, T_SOCK, T_EVENT, T_HB, T_CAP, T_CLI, T_COUNT };
const char *taskNames[T_COUNT] = {"get", "up", "sock", "event", "hb", "cap", "cli"};

enum CfgType { CT_STR, CT_U32, CT_U8, CT_F32 };
struct CfgItem {
    const char *key;
    CfgType type;
    void *ptr;
    uint16_t size;
    bool secret;
};

// ====================================================================================
// Globals for new features
// ====================================================================================
NodeConfig cfg;
BootInfo bootInfo;
uint32_t lastResetReason = RR_UNEXPECTED;
String lastResetDetail = "";
volatile bool otaInProgress = false;     // SDK OTA task owns the SD card: application SD access paused
uint32_t otaStartMs = 0;
volatile bool bootDone = false;          // setup() finished (all threads running)
const char *bootStageName = "start";
uint32_t rebootAtMs = 0;                 // deferred reboot: 0 = none (REBOOT replies first, resets 1 s later)
uint32_t rebootReason = 0;
String rebootDetail = "";
String otaResult = "none";               // last OTA outcome, reported in heartbeat / STATUS as "ota"
String deviceId = "";

SemaphoreHandle_t dataMutex = NULL;
SemaphoreHandle_t sockMutex = NULL;
SemaphoreHandle_t sdMutex = NULL;
SemaphoreHandle_t camMutex = NULL;
SemaphoreHandle_t flashMutex = NULL;

volatile uint32_t taskBeat[T_COUNT] = {0};
bool stallRecorded = false;

// time (sources: Edge Server reply "T=<epoch>", socket "CMD TIME", optional NTP server)
uint32_t epochAtSync = 0;
uint32_t millisAtSync = 0;
bool timeSynced = false;
uint32_t lastNtpMs = 0;

// network
uint32_t wifiReconnects = 0;
uint32_t sockReconnects = 0;
uint32_t lastWifiAttemptMs = 0;
uint32_t lastSockAttemptMs = 0;
uint32_t sockBackoffMs = 2000;
uint32_t lastServerOkMs = 0;
bool deviceRegistered = false;

// sensors
float lastTemp = -999, lastHum = -999;
bool ahtOk = false;
int lastChemRaw = -1;
float gasFiltered = 0;
#define GAS_AVG_N 10
int gasHist[GAS_AVG_N] = {0};
int gasHistIdx = 0, gasHistCnt = 0;
float gasRs = -1, gasPpm = -1, gasLel = -1;
volatile bool gasAlarm = false;
bool gasOk = false;
uint32_t bootMs = 0;
uint32_t recordSeq = 0;
float lastUs = -1;                 // OP-01 placeholder
volatile bool usAlarm = false;     // OP-01 placeholder

// acoustic
volatile int64_t acAccSum = 0, acAccHf = 0;
volatile uint32_t acAccN = 0;
volatile uint32_t acFrames = 0;
volatile uint32_t acLastFrameMs = 0;
volatile int16_t acPrevSample = 0;
float acDb = -120, acHfDb = -120, acFloorDb = -120;
bool acFloorInit = false;
uint32_t acAboveCnt = 0;
volatile bool acAlarm = false;

// events
volatile int eventState = EVT_NORMAL;
String eventId = "";
uint32_t eventCounter = 0;
uint32_t lastGasAlarmMs = 0, lastAcAlarmMs = 0, lastUsAlarmMs = 0, lastAnyAlarmMs = 0;
bool everGas = false, everAc = false, everUs = false;
uint32_t simGasUntil = 0, simAcUntil = 0;
uint32_t simOfflineUntil = 0;            // SIM OFFLINE: behave as if the Edge Server were unreachable (tests)
volatile bool serverLinkUp = false;
// sampling health (1.6.1): measured sample rate and upload cycle, so a slow sampling path is visible
volatile uint32_t sampleCount = 0;
float sampleRateHz = 0;                  // 0 = not measured yet (first 10 s after the threads start)
uint32_t sampleWinStartMs = 0, sampleWinCount = 0;
uint32_t upCycleMs = 0;      // last record upload reached the Edge Server (set by the upload thread)

// Event sound clips (32 MB boards, 1.6.0): while the Edge Server is unreachable, every leak event that starts
// or escalates saves CLIP_PRE_MS + CLIP_POST_MS of microphone audio (G.711 A-law WAV, 16 kHz) to the file
// system; the clips are delivered when the server is back.
#define CLIP_RATE        16000
#define CLIP_PRE_MS      2000
#define CLIP_POST_MS     3000
#define CLIP_RING_BYTES  (10UL * CLIP_RATE)        // 10 s of A-law audio: room for a slow writer
uint8_t *clipRing = NULL;
volatile uint32_t clipWritten = 0;       // samples written into the ring since boot
volatile bool clipArmed = false;
uint32_t clipTrigger = 0;                // clipWritten at the trigger
String clipEventId = "";
uint32_t clipsSaved = 0, clipsSkipped = 0;

bool thermalDerate = false;

// camera / capture
volatile bool bCapturePicture = false;
volatile bool bCaptureClip = false;
String captureEventId = "";
bool lastCaptureOk = false;
uint32_t captureCount = 0;
bool clipRecording = false;

// storage
bool sdOk = false;
String sdRoot = "";
uint32_t bufferedRecords = 0;
uint32_t droppedRecords = 0;
uint32_t bufferReplayOffset = 0;
bool storeOk = false;
bool fsAvailable = false;                // LittleFS on the upper 16 MB (32 MB flash boards only)
String fsDetail = "not mounted";
// directory summary used by the file-system data functions (declared early for the Arduino builder)
struct FsDirStats {
    uint32_t files;
    uint32_t bytes;
    String oldest;               // names sort chronologically (zero-padded counters)
    String newest;
};
                    // flash data store (event log + offline buffer) ready
uint32_t lastFlashBufMs = 0;
uint32_t thinnedRecords = 0;             // offline records not kept because of FLASH_BUF_INTERVAL_MS
uint32_t selfTestMask = 0;       // bit set = FAIL
String selfTestReport = "";

// serial CLI
bool cliLoggedIn = !PRODUCTION_BUILD;

CfgItem cfgTable[] = {
    {"name", CT_STR, cfg.deviceName, sizeof(cfg.deviceName), false},
    {"ssid", CT_STR, cfg.ssid, sizeof(cfg.ssid), false},
    {"pass", CT_STR, cfg.pass, sizeof(cfg.pass), true},
    {"server", CT_STR, cfg.serverAddr, sizeof(cfg.serverAddr), false},
    {"port", CT_U32, &cfg.serverPort, 0, false},
    {"sock_base", CT_U32, &cfg.socketBasePort, 0, false},
    {"token", CT_STR, cfg.apiToken, sizeof(cfg.apiToken), true},
    {"ntp", CT_STR, cfg.ntpServer, sizeof(cfg.ntpServer), false},
    {"secure_all", CT_U8, &cfg.secureAll, 0, false},
    {"power", CT_U8, &cfg.powerMode, 0, false},
    {"maint", CT_U8, &cfg.maintenance, 0, false},
    {"sample_ms", CT_U32, &cfg.sampleIntervalMs, 0, false},
    {"upload_ms", CT_U32, &cfg.uploadIntervalMs, 0, false},
    {"hb_s", CT_U32, &cfg.heartbeatIntervalS, 0, false},
    {"net_reboot_min", CT_U32, &cfg.netRebootMin, 0, false},
    {"gas_r0", CT_F32, &cfg.gasR0, 0, false},
    {"gas_rl", CT_F32, &cfg.gasRL, 0, false},
    {"gas_vc", CT_F32, &cfg.gasVc, 0, false},
    {"gas_clean_ratio", CT_F32, &cfg.gasCleanAirRatio, 0, false},
    {"gas_a", CT_F32, &cfg.gasCurveA, 0, false},
    {"gas_b", CT_F32, &cfg.gasCurveB, 0, false},
    {"gas_kt", CT_F32, &cfg.gasTempCoef, 0, false},
    {"gas_kh", CT_F32, &cfg.gasHumCoef, 0, false},
    {"gas_alarm_ppm", CT_F32, &cfg.gasAlarmPpm, 0, false},
    {"gas_clear_ppm", CT_F32, &cfg.gasClearPpm, 0, false},
    {"gas_confirm_ppm", CT_F32, &cfg.gasConfirmPpm, 0, false},
    {"gas_alarm_raw", CT_U32, &cfg.gasAlarmRaw, 0, false},
    {"gas_warmup_s", CT_U32, &cfg.gasWarmupS, 0, false},
    {"ac_th_db", CT_F32, &cfg.acThresholdDb, 0, false},
    {"ac_clear_db", CT_F32, &cfg.acClearDb, 0, false},
    {"ac_windows", CT_U32, &cfg.acConfirmWindows, 0, false},
    {"fusion_s", CT_U32, &cfg.fusionWindowS, 0, false},
    {"event_hold_s", CT_U32, &cfg.eventHoldS, 0, false},
    {"clip_s", CT_U32, &cfg.clipSeconds, 0, false},
    {"thermal_c", CT_F32, &cfg.thermalLimitC, 0, false},
    {"itemp_scale", CT_F32, &cfg.intTempScale, 0, false},
    {"itemp_offset", CT_F32, &cfg.intTempOffset, 0, false},
};
const int cfgTableLen = sizeof(cfgTable) / sizeof(cfgTable[0]);

void setup()
{
    Serial.begin(115200);
    bootMs = millis();

    // FW-08/FW-16/FW-21: persistent configuration, boot information, identity
    dataMutex = xSemaphoreCreateMutex();
    sockMutex = xSemaphoreCreateMutex();
    sdMutex = xSemaphoreCreateMutex();
    camMutex = xSemaphoreCreateMutex();
    flashMutex = xSemaphoreCreateMutex();
    bootStage("config");
    loadConfig();
    loadBootInfo();
    startCliThread();
    Serial.println("EyeZense Sentinel Node FW " + String(FW_VERSION) + " boot #" + String(bootInfo.bootCount) + " last reset: " + resetReasonName(lastResetReason) + " " + lastResetDetail);

    bootStage("wifi");
    ezhal_radio_begin(cfg.ssid, cfg.pass);
    Serial.print("Attempting to connect to WPA SSID: ");
    Serial.println(cfg.ssid);
    uint32_t wifiStart = millis();
    while (!ezhal_radio_connected()) {
        delay(500);
        Serial.print(".");
        if (millis() - wifiStart > 60000) {          // do not block forever: continue offline, reconnect later
            Serial.println("\nWiFi not available, continue offline (data will be buffered)");
            break;
        }
        if ((millis() - wifiStart) % 15000 < 500) {
            ezhal_radio_begin(cfg.ssid, cfg.pass);
        }
    }
    Serial.println(ezhal_radio_connected() ? "WiFi connected, IP " + String(ezhal_radio_local_ip_str())
                                                 : String("WiFi NOT connected"));
    bootStage("multimedia");
    uint8_t mac[6] = {0};
    ezhal_radio_mac(mac);
    char macStr[18];
    sprintf(macStr, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    deviceId = String(macStr);

    // Configure camera video channel with video format information
    // Adjust the bitrate based on your WiFi network quality
    // configV.setBitrate(2 * 1024 * 1024);     // Recommend to use 2Mbps for RTSP streaming to prevent network congestion
#ifdef STREAMING_VIDEO
    Camera.configVideoChannel(CHANNEL, configV);
#else
    Camera.configVideoChannel(CHANNEL, configPic);
#endif
#ifdef EVENT_VIDEO_CLIP
    Camera.configVideoChannel(CLIP_CHANNEL, configClip);
#endif
    Camera.videoInit();

#ifdef AUDIO_I2S
    // Configure I2S
    i2s.configI2S(configI2S);
    i2s.begin();
#else
    // Configure audio peripheral for audio data output
    audio.configAudio(configA);
    audio.begin();
#endif

    // Configure AAC audio encoder
    //aac.configAudio(configA);
    //aac.begin();
    pcm.configAudio(configA);
    pcm.configCodec(CODEC_G711_PCMA);
    pcm.begin();

    // Configure RTSP with identical video format information and enable audio streaming
#ifdef STREAMING_VIDEO
    rtsp.configVideo(configV);
#endif
    rtsp.configAudio(configA, CODEC_G711_PCMA);
    //rtsp.configAudio(configA, CODEC_AAC);
    rtsp.begin();

    // Configure StreamIO object to stream data from audio channel to AAC encoder
#ifdef AUDIO_I2S
    audioStreamer.registerInput(i2s);
#else
    audioStreamer.registerInput(audio);
#endif
#ifdef ACOUSTIC_DETECTION
    bool micTapOk = ezhal_mic_tap_init(audioTapHandler);
    audioStreamer.registerOutput1(pcm);
    if (micTapOk) {
        audioStreamer.registerOutput2(ezhal_mic_tap_module());
    } else {
        Serial.println("Audio tap open failed - acoustic detection disabled");
    }
#else
    audioStreamer.registerOutput(pcm);
#endif
    if (audioStreamer.begin() != 0) {
        Serial.println("StreamIO link start failed");
    }

    // Configure StreamIO object to stream data from video channel and AAC encoder to rtsp output
#ifdef STREAMING_VIDEO
    avMixStreamer.registerInput2(Camera.getStream(CHANNEL));
    avMixStreamer.registerInput1(pcm);
#else
    avMixStreamer.registerInput(pcm);
#endif
    avMixStreamer.registerOutput(rtsp);
    if (avMixStreamer.begin() != 0) {
        Serial.println("StreamIO link start failed");
    }

#ifdef EVENT_VIDEO_CLIP
    mp4.configVideo(configClip);
    mp4.setRecordingDataType(STORAGE_VIDEO);
    mp4.setRecordingFileCount(1);
    clipStreamer.registerInput(Camera.getStream(CLIP_CHANNEL));
    clipStreamer.registerOutput(mp4);
    if (clipStreamer.begin() != 0) {
        Serial.println("Clip StreamIO link start failed");
    }
#endif
//#ifdef STREAMING_VIDEO
    // Start data stream from video channel
    if (cfg.powerMode == 0 || isVideoStreaming()) {
        ezhal_cam_start(CHANNEL, 0);
    }
//#endif
    delay(1000);
    // server.setBlocking();
    server.begin();
    printInfo();
    initPageListening();
    //Serial.println("RTC Start");
    //rtc.Init();
    //long long epochTime = rtc.SetEpoch(1970, 1, 1, 1, 1, 1);
    //rtc.Write(epochTime);

#ifndef STREAMING_ONLY
    Serial.println("AHT Start");
    bootStage("hal");
    bool envOk = ezhal_env_init();
    bool gasOk0 = ezhal_gas_init();
    ezhal_us_init();
    ezhal_power_init();
    ezhal_tamper_init();
    Serial.println(String("HAL ") + EZSN_HAL_VERSION + ": env=" + (envOk ? "ok" : "FAIL") + " gas=" + (gasOk0 ? "ok" : "rail") +
                   " mic=" + (ezhal_mic_present() ? "ok" : "absent") + " ultrasonic=" + (ezhal_us_present() ? "ok" : "absent(OP-01)") +
                   " power=" + (ezhal_power_present() ? "ok" : "absent(OP-04)") + " tamper=" + (ezhal_tamper_present() ? "ok" : "absent(OP-08)"));
    delay(900);

    bootStage("storage");
    initStorage();
    otaCheckOutcome();
    bootStage("time");
    Serial.println(syncTime(true) ? "Time synced: " + String(getEpoch()) : String("Time not synced (server will push it)"));
    bootStage("self-test");
    runSelfTest();

    bootStage("register");
    addDevice(5);
    // Socket
    if (STREAMING_SOCKET == true) {
        delay(2000);
        startWebSocket(false);
    }
#ifdef ENABLE_NET_OTA
    ota.start_OTA_threads(NET_OTA_PORT, cfg.serverAddr);
#endif
    bootStage("threads");
    start_multithreading();
    // Watchdog
    ezhal_wdt_start(WDT_TIMEOUT);
    bootStage("running");
#endif
}

void loop()
{
#ifndef STREAMING_ONLY
    //Serial.println("==========loop===========");
    WiFiClient client = server.available();
    if (client.connected()) {
        if (PRINT_DEBUG) Serial.println("==========loop have query===========");
        app.process(&client);
        //Serial.println("==========loop 22222===========");
        client.stop();
    }
    delay(400);
#endif
}

void LOG(String message) {
	Serial.println(message);
}

// SDK String(float, n) right-pads to width n+2 (0 -> " 0"); trim so URLs/CSV carry clean numbers
String fnum(float v, int decimals)
{
    String out = String(v, (unsigned char)decimals);
    out.trim();
    return out;
}

void bootStage(const char *stage)
{
    bootStageName = stage;
    Serial.println("[BOOT] " + String(millis()) + " ms: " + stage);
    if (strcmp(stage, "running") == 0) bootDone = true;
}

// The serial CLI starts right after the config is loaded, so service commands (Wi-Fi settings,
// flash diagnostics, REBOOT) work even if a later boot stage is slow or stuck.
void startCliThread()
{
    if (os_thread_create_arduino(thread_cli_task, NULL, osPriorityNormal, 1024 * 10)) {
        Serial.println("Thread CLI created successfully");
    } else {
        Serial.println("Failed to create thread CLI");
    }
}

bool isVideoStreaming()
{
#ifdef STREAMING_VIDEO
    return true;
#else
    return false;
#endif
}

// ====================================================================================
// Locks / task heartbeat
// ====================================================================================
void lockM(SemaphoreHandle_t m)
{
    if (m) xSemaphoreTake(m, portMAX_DELAY);
}

void unlockM(SemaphoreHandle_t m)
{
    if (m) xSemaphoreGive(m);
}

void beat(int t)
{
    taskBeat[t] = millis() | 1;     // never 0 once registered
}

// ====================================================================================
// Persistent configuration (FW-08, FW-18, FW-21)
// ====================================================================================
uint32_t crc32Calc(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320UL & (0 - (crc & 1)));
        }
    }
    return ~crc;
}

void setDefaultConfig()
{
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic = CONFIG_MAGIC;
    cfg.version = CONFIG_VERSION;
    cfg.size = sizeof(cfg);
    strncpy(cfg.deviceName, DEVICE_NAME, sizeof(cfg.deviceName) - 1);
    strncpy(cfg.ssid, ssid, sizeof(cfg.ssid) - 1);
    strncpy(cfg.pass, pass, sizeof(cfg.pass) - 1);
    strncpy(cfg.serverAddr, SERVER_ADDRESS, sizeof(cfg.serverAddr) - 1);
    cfg.serverPort = SERVER_PORT;
    cfg.socketBasePort = SOCKET_BASE_PORT;
    strncpy(cfg.apiToken, API_TOKEN_DEFAULT, sizeof(cfg.apiToken) - 1);
    cfg.ntpServer[0] = 0;          // empty: time comes from the Edge Server (set "ntp" to use an NTP server)
    cfg.secureAll = 0;
    cfg.powerMode = 0;
    cfg.maintenance = 0;
    cfg.sampleIntervalMs = 50;
    cfg.uploadIntervalMs = 900;
    cfg.heartbeatIntervalS = 60;
    cfg.netRebootMin = 30;
    cfg.gasR0 = 0;                 // not calibrated -> run CAL_GAS in clean air
    cfg.gasRL = 10.0;
    cfg.gasVc = 3.3;
    cfg.gasCleanAirRatio = 4.4;    // OD-01: generic MOS methane values, confirm with datasheet
    cfg.gasCurveA = 1012.7;
    cfg.gasCurveB = -2.786;
    cfg.gasTempCoef = 0.0;
    cfg.gasHumCoef = 0.0;
    cfg.gasAlarmPpm = 1000;        // 2 %LEL methane
    cfg.gasClearPpm = 700;
    cfg.gasConfirmPpm = 5000;      // 10 %LEL
    cfg.gasAlarmRaw = 0;
    cfg.gasWarmupS = 180;
    cfg.acThresholdDb = 12.0;
    cfg.acClearDb = 6.0;
    cfg.acConfirmWindows = 20;     // 20 x 50 ms = 1 s sustained
    cfg.fusionWindowS = 60;
    cfg.eventHoldS = 30;
    cfg.clipSeconds = 10;
    cfg.thermalLimitC = 70.0;
    cfg.intTempScale = 0.0;
    cfg.intTempOffset = 0.0;
}

bool configValid()
{
    return cfg.magic == CONFIG_MAGIC && cfg.version == CONFIG_VERSION && cfg.size == sizeof(cfg) &&
           cfg.crc == crc32Calc((uint8_t *)&cfg, sizeof(cfg) - sizeof(uint32_t));
}

// Config lives in an A/B record store (EZHAL_FLASH_CFG_A / _B): a power cut during SAVE keeps the
// previous copy. Firmware <= 1.3.2 kept a raw copy in one sector at 0xFD0000; it is migrated once.
void loadConfig()
{
    lockM(flashMutex);
    HalStatus st = ezhal_frec_read(EZHAL_FLASH_CFG_A, EZHAL_FLASH_CFG_B, &cfg, sizeof(cfg), NULL);
    unlockM(flashMutex);
    if (st == HS_OK && configValid()) {
        Serial.println("Config loaded from flash (A/B)");
        return;
    }
    lockM(flashMutex);
    ezhal_flash_read(CONFIG_FLASH_ADDR, &cfg, sizeof(cfg));          // single-sector format of <= 1.3.2
    unlockM(flashMutex);
    if (configValid()) {
        Serial.println("Config migrated from the single-sector format");
        saveConfig();
        return;
    }
    Serial.println("Config invalid/absent -> defaults");
    setDefaultConfig();
    saveConfig();
}

bool saveConfig()
{
    cfg.crc = crc32Calc((uint8_t *)&cfg, sizeof(cfg) - sizeof(uint32_t));
    lockM(flashMutex);
    HalStatus st = ezhal_frec_write(EZHAL_FLASH_CFG_A, EZHAL_FLASH_CFG_B, &cfg, sizeof(cfg));   // other slot, verified
    unlockM(flashMutex);
    return st == HS_OK;
}

void writeBootInfo()
{
    bootInfo.crc = crc32Calc((uint8_t *)&bootInfo, sizeof(bootInfo) - sizeof(uint32_t));
    lockM(flashMutex);
    ezhal_flash_write_sector(BOOTINFO_FLASH_ADDR, &bootInfo, sizeof(bootInfo));
    unlockM(flashMutex);
}

void loadBootInfo()
{
    lockM(flashMutex);
    ezhal_flash_read(BOOTINFO_FLASH_ADDR, &bootInfo, sizeof(bootInfo));
    unlockM(flashMutex);
    uint32_t crc = crc32Calc((uint8_t *)&bootInfo, sizeof(bootInfo) - sizeof(uint32_t));
    if (bootInfo.magic != CONFIG_MAGIC || bootInfo.crc != crc) {
        memset(&bootInfo, 0, sizeof(bootInfo));
        bootInfo.magic = CONFIG_MAGIC;
    }
    lastResetReason = bootInfo.pendingReason;
    bootInfo.pendingDetail[sizeof(bootInfo.pendingDetail) - 1] = 0;
    lastResetDetail = String(bootInfo.pendingDetail);
    bootInfo.bootCount++;
    bootInfo.pendingReason = RR_UNEXPECTED;     // anything not recorded before next reset = unexpected
    bootInfo.pendingDetail[0] = 0;
    writeBootInfo();
}

void recordResetReason(uint32_t reason, String detail)
{
    bootInfo.pendingReason = reason;
    strncpy(bootInfo.pendingDetail, detail.c_str(), sizeof(bootInfo.pendingDetail) - 1);
    bootInfo.pendingDetail[sizeof(bootInfo.pendingDetail) - 1] = 0;
    writeBootInfo();
}

// Reboot after the reply to the current command has gone out (REBOOT used to reset before replying,
// so the Edge Server waited 15 s for an answer that never came).
void scheduleReboot(uint32_t reason, String detail, uint32_t delayMs)
{
    rebootReason = reason;
    rebootDetail = detail;
    rebootAtMs = millis() + delayMs;
    if (rebootAtMs == 0) rebootAtMs = 1;
}

// Called every second by the heartbeat thread
void rebootCheck()
{
    if (rebootAtMs != 0 && (int32_t)(millis() - rebootAtMs) >= 0) {
        rebootAtMs = 0;
        rebootWithReason(rebootReason, rebootDetail);
    }
}

void rebootWithReason(uint32_t reason, String detail)
{
    LOG("Reboot: " + resetReasonName(reason) + " " + detail);
    recordResetReason(reason, detail);
    delay(200);
    ezhal_sys_reset();
}

String resetReasonName(uint32_t r)
{
    switch (r) {
        case RR_CMD_REBOOT: return "cmd_reboot";
        case RR_SOCKET_FAIL: return "socket_fail";
        case RR_REGISTER_FAIL: return "register_fail";
        case RR_NETWORK_LOST: return "network_lost";
        case RR_UPLOAD_FAIL: return "upload_fail";
        case RR_TASK_STALL: return "task_stall";
        case RR_OTA: return "ota";
        case RR_CONFIG: return "config";
        default: return "unexpected(power/wdt/brownout)";
    }
}

int findCfgItem(String key)
{
    for (int i = 0; i < cfgTableLen; i++) {
        if (key.equalsIgnoreCase(cfgTable[i].key)) return i;
    }
    return -1;
}

String cfgItemValue(int i, bool mask)
{
    CfgItem &it = cfgTable[i];
    if (mask && it.secret) return "****";
    switch (it.type) {
        case CT_STR: return String((char *)it.ptr);
        case CT_U32: return String(*(uint32_t *)it.ptr);
        case CT_U8: return String(*(uint8_t *)it.ptr);
        case CT_F32: return fnum(*(float *)it.ptr, 4);
    }
    return "";
}

bool setCfgItem(String key, String value)
{
    int i = findCfgItem(key);
    if (i < 0) return false;
    CfgItem &it = cfgTable[i];
    switch (it.type) {
        case CT_STR:
            if (value.length() >= it.size) return false;
            memset(it.ptr, 0, it.size);
            strncpy((char *)it.ptr, value.c_str(), it.size - 1);
            break;
        case CT_U32: *(uint32_t *)it.ptr = (uint32_t)value.toInt(); break;
        case CT_U8: *(uint8_t *)it.ptr = (uint8_t)value.toInt(); break;
        case CT_F32: *(float *)it.ptr = value.toFloat(); break;
    }
    return true;
}

String configDump(bool json)
{
    String out = json ? "{" : "";
    for (int i = 0; i < cfgTableLen; i++) {
        if (json) {
            out += (i ? "," : "");
            out += "\"" + String(cfgTable[i].key) + "\":\"" + cfgItemValue(i, true) + "\"";
        } else {
            out += String(cfgTable[i].key) + "=" + cfgItemValue(i, true) + "\n";
        }
    }
    if (json) out += "}";
    return out;
}

// ====================================================================================
// Time (FW-15: timestamps for fusion)
// ====================================================================================
void setClock(uint32_t epoch)
{
    if (epoch < 1600000000UL) return;
    epochAtSync = epoch;
    millisAtSync = millis();
    timeSynced = true;
}

// Optional NTP (cfg.ntpServer). The Edge Server also provides time in its HTTP replies and via
// "CMD TIME", so an NTP server is not required on the device network.
bool syncTime(bool force)
{
    if (!ezhal_radio_connected()) return timeSynced;
    if (cfg.ntpServer[0] == 0) return timeSynced;
    if (!force && timeSynced && millis() - lastNtpMs < 3600000UL) return true;
    lastNtpMs = millis();
    IPAddress ip;
    if (!ezhal_radio_resolve(cfg.ntpServer, &ip)) return timeSynced;
    uint32_t e = ezhal_ntp_query((uint32_t)ip, 1000);
    if (e == 0) return timeSynced;
    setClock(e);
    return true;
}

uint32_t getEpoch()
{
    if (!timeSynced) return 0;
    return epochAtSync + (millis() - millisAtSync) / 1000;
}

// ====================================================================================
// Storage on SD (FW-08)
// ====================================================================================
// ====================================================================================
// Data on the file system (32 MB flash boards): event pictures, offline buffer, event log (FW-07, FW-08)
// ====================================================================================
#define FS_IMG_DIR          "/img"                      // pictures delivered to the Edge Server (history)
#define FS_PEND_DIR         "/pend"                     // pictures not yet delivered
#define FS_BUF_DIR          "/buf"                      // offline sensor records, 32 KB chunk files
#define FS_BUF_CURSOR       "/buf/cursor"               // "<chunk> <offset>" of the next unsent record
#define FS_LOG_FILE         "/log/events.csv"
#define FS_LOG_OLD          "/log/events.1.csv"
#ifndef FS_IMG_KEEP_BYTES                       // budgets can be overridden (host tests use small ones)
#define FS_IMG_KEEP_BYTES   (4UL * 1024UL * 1024UL)
#endif
#ifndef FS_PEND_KEEP_BYTES                       // budgets can be overridden (host tests use small ones)
#define FS_PEND_KEEP_BYTES  (3UL * 1024UL * 1024UL)
#endif
#ifndef FS_BUF_MAX_BYTES                       // budgets can be overridden (host tests use small ones)
#define FS_BUF_MAX_BYTES    (8UL * 1024UL * 1024UL)
#endif
#ifndef FS_BUF_CHUNK_BYTES                       // budgets can be overridden (host tests use small ones)
#define FS_BUF_CHUNK_BYTES  (32UL * 1024UL)
#endif
#define FS_AUD_DIR          "/audio"                    // event sound clips delivered (history)
#define FS_AUD_PEND_DIR     "/audio/pend"               // event sound clips not yet delivered
#ifndef FS_AUD_KEEP_BYTES
#define FS_AUD_KEEP_BYTES   (2UL * 1024UL * 1024UL)
#endif
#ifndef FS_AUD_PEND_KEEP_BYTES
#define FS_AUD_PEND_KEEP_BYTES (3UL * 1024UL * 1024UL)
#endif
#ifndef FS_LOG_ROTATE_BYTES                       // budgets can be overridden (host tests use small ones)
#define FS_LOG_ROTATE_BYTES (128UL * 1024UL)
#endif

uint32_t fsImgSeq = 0;
uint32_t fsBufPending = 0;
uint32_t fsBufDropped = 0;
uint32_t fsBufWriteSeq = 0;      // chunk being appended to (0 = not initialised)
uint32_t fsBufReadSeq = 0;       // oldest chunk with unsent records
uint32_t fsBufReadOff = 0;       // byte offset of the next unsent record in that chunk

FsDirStats fsDirStats(const char *dir, const char *suffix)
{
    FsDirStats st;
    st.files = 0;
    st.bytes = 0;
    String ls;
    if (ezhal_fs_list(dir, &ls) != HS_OK) return st;
    int pos = 0;
    while (pos < (int)ls.length()) {
        int nl = ls.indexOf('\n', pos);
        if (nl < 0) nl = ls.length();
        String line = ls.substring(pos, nl);
        pos = nl + 1;
        int sp = line.indexOf(' ');
        if (sp <= 0) continue;
        String name = line.substring(0, sp);
        if (!name.endsWith(suffix)) continue;              // skips directories, cursor and .tmp files
        st.files++;
        st.bytes += (uint32_t)line.substring(sp + 1).toInt();
        if (st.oldest.length() == 0 || strcmp(name.c_str(), st.oldest.c_str()) < 0) st.oldest = name;
        if (strcmp(name.c_str(), st.newest.c_str()) > 0) st.newest = name;
    }
    return st;
}

// delete the oldest files until the directory holds at most maxBytes
void fsTrimDir(const char *dir, uint32_t maxBytes, const char *suffix)
{
    for (int guard = 0; guard < 1000; guard++) {
        FsDirStats st = fsDirStats(dir, suffix);
        if (st.bytes <= maxBytes || st.files <= 1) return;
        String p = String(dir) + "/" + st.oldest;
        if (ezhal_fs_remove(p.c_str()) != HS_OK) return;
    }
}

// "<boot>_<counter>_<event id><ext>": sorts chronologically, keeps the event id for a later upload
String fsMediaName(const String &evId, const char *ext)
{
    char b[24];
    snprintf(b, sizeof(b), "%05lu_%05lu_", (unsigned long)(bootInfo.bootCount % 100000UL),
             (unsigned long)(++fsImgSeq % 100000UL));
    return String(b) + evId + ext;
}

String fsImageName(const String &evId)
{
    return fsMediaName(evId, ".jpg");
}

String fsImageEventId(const String &name)
{
    int a = name.indexOf('_');
    int b = a < 0 ? -1 : name.indexOf('_', a + 1);
    if (b < 0 || !(name.endsWith(".jpg") || name.endsWith(".wav"))) return name;
    return name.substring(b + 1, name.length() - 4);
}

bool fsSaveFile(const char *dir, const String &name, const uint8_t *buf, uint32_t len, uint32_t keepBytes,
                const char *suffix)
{
    String path = String(dir) + "/" + name;
    bool ok = ezhal_fs_write_file(path.c_str(), buf, len) == HS_OK;
    fsTrimDir(dir, keepBytes, suffix);
    return ok;
}

bool fsSaveImage(const char *dir, const String &name, const uint8_t *buf, uint32_t len, uint32_t keepBytes)
{
    return fsSaveFile(dir, name, buf, len, keepBytes, ".jpg");
}

// ---- offline buffer ----------------------------------------------------------------
String fsBufChunkPath(uint32_t seq)
{
    char b[32];
    snprintf(b, sizeof(b), FS_BUF_DIR "/%010lu.txt", (unsigned long)seq);
    return String(b);
}

uint32_t fsCountLines(const char *path, uint32_t from)
{
    static uint8_t b[512];
    uint32_t n = 0, off = from;
    int32_t r;
    while ((r = ezhal_fs_read(path, off, b, sizeof(b))) > 0) {
        for (int32_t i = 0; i < r; i++) if (b[i] == '\n') n++;
        off += (uint32_t)r;
    }
    return n;
}

void fsBufSaveCursor()
{
    String c = String(fsBufReadSeq) + " " + String(fsBufReadOff);
    ezhal_fs_write_file(FS_BUF_CURSOR, (const uint8_t *)c.c_str(), c.length());
}

void fsBufInit()
{
    fsBufPending = 0;
    fsBufReadOff = 0;
    fsBufWriteSeq = fsBufReadSeq = 1;
    if (!fsAvailable) return;
    ezhal_fs_mkdir(FS_BUF_DIR);
    FsDirStats st = fsDirStats(FS_BUF_DIR, ".txt");
    if (st.files == 0) return;
    fsBufReadSeq = (uint32_t)st.oldest.toInt();
    fsBufWriteSeq = (uint32_t)st.newest.toInt();
    char cb[40];
    int32_t n = ezhal_fs_read(FS_BUF_CURSOR, 0, (uint8_t *)cb, sizeof(cb) - 1);
    if (n > 0) {
        cb[n] = 0;
        char *e = NULL;
        uint32_t cs = strtoul(cb, &e, 10);
        uint32_t co = strtoul(e, NULL, 10);
        if (cs == fsBufReadSeq) fsBufReadOff = co;        // a cursor for an already deleted chunk is stale
    }
    for (uint32_t s = fsBufReadSeq; s <= fsBufWriteSeq; s++) {
        String p = fsBufChunkPath(s);
        if (ezhal_fs_size(p.c_str()) >= 0) fsBufPending += fsCountLines(p.c_str(), s == fsBufReadSeq ? fsBufReadOff : 0);
    }
}

bool fsBufAppend(const String &rec)
{
    if (!fsAvailable) return false;
    if (fsBufWriteSeq == 0) fsBufInit();
    String path = fsBufChunkPath(fsBufWriteSeq);
    if (ezhal_fs_size(path.c_str()) >= (int32_t)FS_BUF_CHUNK_BYTES) {
        fsBufWriteSeq++;
        path = fsBufChunkPath(fsBufWriteSeq);
    }
    String line = rec + "\n";
    if (ezhal_fs_append(path.c_str(), (const uint8_t *)line.c_str(), line.length()) != HS_OK) return false;
    fsBufPending++;
    // budget: when the chunks exceed FS_BUF_MAX_BYTES, the oldest chunk is dropped (counted)
    if ((fsBufWriteSeq - fsBufReadSeq + 1) * FS_BUF_CHUNK_BYTES > FS_BUF_MAX_BYTES) {
        String old = fsBufChunkPath(fsBufReadSeq);
        uint32_t lost = fsCountLines(old.c_str(), fsBufReadOff);
        ezhal_fs_remove(old.c_str());
        fsBufDropped += lost;
        fsBufPending -= (lost < fsBufPending ? lost : fsBufPending);
        fsBufReadSeq++;
        fsBufReadOff = 0;
        fsBufSaveCursor();
    }
    return true;
}

// Send up to 50 buffered records, oldest first. The read position is saved after each batch, so a
// reboot in the middle of a replay resends at most that batch.
bool fsBufReplay()
{
    if (!fsAvailable || fsBufPending == 0) return true;
    static uint8_t rb[1024];
    int sent = 0;
    bool ok = true;
    while (sent < 50) {
        String path = fsBufChunkPath(fsBufReadSeq);
        int32_t n = ezhal_fs_read(path.c_str(), fsBufReadOff, rb, sizeof(rb) - 1);
        char *nl = NULL;
        if (n > 0) {
            rb[n] = 0;
            nl = strchr((char *)rb, '\n');
        }
        if (nl == NULL) {                                  // end of this chunk
            if (fsBufReadSeq >= fsBufWriteSeq) break;      // the chunk still being written
            ezhal_fs_remove(path.c_str());
            fsBufReadSeq++;
            fsBufReadOff = 0;
            fsBufSaveCursor();
            continue;
        }
        *nl = 0;
        uint32_t lineLen = (uint32_t)(nl - (char *)rb) + 1;
        if (lineLen > 1 && !submitDataWithSocket(String((const char *)rb))) {
            ok = false;
            break;
        }
        fsBufReadOff += lineLen;
        if (fsBufPending) fsBufPending--;
        sent++;
        beat(T_UP);
    }
    if (sent > 0) fsBufSaveCursor();
    if (ok && sent > 0 && fsBufPending == 0) LOG("Offline buffer fully replayed (file system)");
    return ok;
}

// ---- event log -----------------------------------------------------------------------
bool fsLogAppend(const String &line)
{
    if (ezhal_fs_size(FS_LOG_FILE) >= (int32_t)FS_LOG_ROTATE_BYTES) {
        ezhal_fs_remove(FS_LOG_OLD);
        ezhal_fs_rename(FS_LOG_FILE, FS_LOG_OLD);
    }
    String l = line + "\n";
    return ezhal_fs_append(FS_LOG_FILE, (const uint8_t *)l.c_str(), l.length()) == HS_OK;
}

// last n lines of a file (oldest first, '\n'-joined, no trailing newline); *got = lines returned
String fsTailLines(const char *path, int n, int *got)
{
    *got = 0;
    int32_t sz = ezhal_fs_size(path);
    if (sz <= 0 || n <= 0) return "";
    uint32_t want = (uint32_t)n * 160UL + 1;
    if (want > (uint32_t)sz) want = (uint32_t)sz;
    char *b = (char *)malloc(want + 1);
    if (b == NULL) return "";
    int32_t r = ezhal_fs_read(path, (uint32_t)sz - want, (uint8_t *)b, want);
    if (r < 0) r = 0;
    b[r] = 0;
    int end = r;
    while (end > 0 && b[end - 1] == '\n') end--;           // ignore the final newline
    int start = end, lines = 0;
    while (start > 0 && lines < n) {
        int i = start - 1;
        while (i >= 0 && b[i] != '\n') i--;
        if (i < 0 && want < (uint32_t)sz) break;           // partial first line of the window
        start = i + 1;
        lines++;
        if (i < 0) break;
        start = i;                                         // points at the '\n' before this line
    }
    if (start < end && b[start] == '\n') start++;
    b[end] = 0;
    String out = String(b + start);
    free(b);
    *got = lines;
    return out;
}

String fsLogTail(int n)
{
    int got = 0, got2 = 0;
    String a = fsTailLines(FS_LOG_FILE, n, &got);
    if (got >= n) return a;
    String b = fsTailLines(FS_LOG_OLD, n - got, &got2);
    if (b.length() == 0) return a;
    return a.length() ? b + "\n" + a : b;
}

// ---- pictures: upload, keep, retry ---------------------------------------------------
// POST one JPEG to the Edge Server (same request format as always); true only when the server
// answered "200", i.e. it received the whole picture.
// SIM OFFLINE active (defined here, after the types the Arduino builder's prototypes need)
bool simOffline()
{
    return simOfflineUntil != 0 && (int32_t)(millis() - simOfflineUntil) < 0;
}

bool postImage(const String &evId, const uint8_t *buf, uint32_t len)
{
    return postMedia(evId, buf, len, "image/jpeg");
}

// POST a picture or sound clip; the Edge Server stores it by its Type header
bool postMedia(const String &evId, const uint8_t *buf, uint32_t len, const char *type)
{
    if (simOffline()) return false;                        // SIM OFFLINE: the upload counts as undelivered
    if (!ezhal_radio_connected() || !wifiClient.connect(cfg.serverAddr, cfg.serverPort)) return false;
    const char *boundary = "----AMB82_Boundary";
    String head = "--" + (String)boundary + "\r\n" + "Content-Disposition: form-data\r\n\r\n";
    String tail = "\r\n--" + (String)boundary + "--\r\n";
    uint32_t contentLength = head.length() + len + tail.length();
    wifiClient.print("POST / HTTP/1.1\r\n");
    wifiClient.print("IP: " + ezhal_radio_local_ip_str() + "\r\n");
    wifiClient.print("Device: " + String(cfg.deviceName) + "\r\n");
    wifiClient.print("Event: " + evId + "\r\n");
    wifiClient.print("Content-Length: " + String(contentLength) + "\r\n");
    wifiClient.print(String("Type: ") + type + "\r\n\r\n");
    wifiClient.print(head);
    wifiClient.write(buf, len);
    wifiClient.print(tail);
    String resp = "";
    uint32_t t0 = millis();
    while (millis() - t0 < 5000 && resp.length() < 12) {   // status line "HTTP/1.x 200"
        if (wifiClient.available()) resp += (char)wifiClient.read();
        else delay(10);
    }
    wifiClient.stop();
    return resp.indexOf(" 200") > 0;
}

// upload the oldest picture that could not be delivered earlier (every 10 s at most)
void fsRetryPendingImage()
{
    static uint32_t lastTry = 0;
    if (!fsAvailable || !ezhal_radio_connected() || millis() - lastTry < 10000) return;
    lastTry = millis();
    FsDirStats st = fsDirStats(FS_PEND_DIR, ".jpg");
    if (st.files == 0) return;
    String path = String(FS_PEND_DIR) + "/" + st.oldest;
    int32_t sz = ezhal_fs_size(path.c_str());
    if (sz <= 0) {
        ezhal_fs_remove(path.c_str());
        return;
    }
    uint8_t *b = (uint8_t *)malloc((size_t)sz);
    if (b == NULL) return;
    if (ezhal_fs_read(path.c_str(), 0, b, (uint32_t)sz) == sz && postImage(fsImageEventId(st.oldest), b, (uint32_t)sz)) {
        String hist = String(FS_IMG_DIR) + "/" + st.oldest;
        ezhal_fs_mkdir(FS_IMG_DIR);
        ezhal_fs_rename(path.c_str(), hist.c_str());
        fsTrimDir(FS_IMG_DIR, FS_IMG_KEEP_BYTES, ".jpg");
        LOG("Pending picture uploaded: " + st.oldest);
    }
    free(b);
}

// ---- event sound clips ----------------------------------------------------------------
// G.711 A-law encoder (ITU-T G.711, as in the reference g711.c)
uint8_t alawEncode(int16_t pcm)
{
    static const int16_t segEnd[8] = {0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF};
    int32_t v = pcm >> 3;
    uint8_t mask;
    if (v >= 0) {
        mask = 0xD5;
    } else {
        mask = 0x55;
        v = -v - 1;
    }
    int seg = 0;
    while (seg < 8 && v > segEnd[seg]) seg++;
    if (seg >= 8) return (uint8_t)(0x7F ^ mask);
    uint8_t a = (uint8_t)(seg << 4);
    a |= (seg < 2) ? ((v >> 1) & 0x0F) : ((v >> seg) & 0x0F);
    return (uint8_t)(a ^ mask);
}

// called by the audio tap for every PCM frame (MMF task): keep the last 10 s as A-law
void clipFeed(const int16_t *s, uint32_t n)
{
    if (clipRing == NULL) return;
    uint32_t w = clipWritten;
    for (uint32_t i = 0; i < n; i++) clipRing[(w + i) % CLIP_RING_BYTES] = alawEncode(s[i]);
    clipWritten = w + n;
}

bool serverReachable()
{
    return serverLinkUp && !simOffline();
}

// start a clip for this event if the server is unreachable (one clip at a time)
void clipRequestIfOffline(const String &evId)
{
    if (!fsAvailable || clipRing == NULL || serverReachable()) return;
    if (clipArmed) {
        if (evId != clipEventId) clipsSkipped++;          // the same event escalating keeps its one clip
        return;
    }
    clipEventId = evId;
    clipTrigger = clipWritten;
    clipArmed = true;
}

// 44-byte WAV header for 8-bit A-law mono (format tag 6)
void clipWavHeader(uint8_t *h, uint32_t dataBytes)
{
    const uint32_t rate = CLIP_RATE;
    uint32_t v[11] = {0x46464952u, 36 + dataBytes, 0x45564157u, 0x20746D66u, 16, 0, rate, rate, 0, 0x61746164u, dataBytes};
    memcpy(h, v, 44);
    h[20] = 6; h[21] = 0;                                   // format: A-law
    h[22] = 1; h[23] = 0;                                   // mono
    h[32] = 1; h[33] = 0;                                   // block align
    h[34] = 8; h[35] = 0;                                   // bits per sample
}

// called by the picture thread: once CLIP_POST_MS of audio after the trigger has arrived, save the clip
void clipService()
{
    if (!clipArmed) return;
    const uint32_t pre = CLIP_RATE / 1000 * CLIP_PRE_MS, post = CLIP_RATE / 1000 * CLIP_POST_MS;
    uint32_t w = clipWritten;
    if (w - clipTrigger < post) return;
    uint32_t start = clipTrigger > pre ? clipTrigger - pre : 0;         // shorter right after boot
    uint32_t end = clipTrigger + post;
    if (w - start > CLIP_RING_BYTES - CLIP_RATE / 10) {                 // writer was late: keep what is still there
        start = w - (CLIP_RING_BYTES - CLIP_RATE / 10);
        if (start > end) start = end;
    }
    uint32_t n = end - start;
    uint8_t *buf = (uint8_t *)malloc(44 + n);
    clipArmed = false;
    if (buf == NULL) {
        clipsSkipped++;
        return;
    }
    clipWavHeader(buf, n);
    for (uint32_t i = 0; i < n; i++) buf[44 + i] = clipRing[(start + i) % CLIP_RING_BYTES];
    String name = fsMediaName(clipEventId, ".wav");
    if (fsSaveFile(FS_AUD_PEND_DIR, name, buf, 44 + n, FS_AUD_PEND_KEEP_BYTES, ".wav")) {
        clipsSaved++;
        LOG("Sound clip kept for a later upload: " + name + " (" + String(n / (CLIP_RATE / 1000)) + " ms)");
    }
    free(buf);
}

// upload the oldest clip that could not be delivered (every 10 s at most, server reachable)
void fsRetryPendingAudio()
{
    static uint32_t lastTry = 0;
    if (!fsAvailable || !serverReachable() || millis() - lastTry < 10000) return;
    lastTry = millis();
    FsDirStats st = fsDirStats(FS_AUD_PEND_DIR, ".wav");
    if (st.files == 0) return;
    String path = String(FS_AUD_PEND_DIR) + "/" + st.oldest;
    int32_t sz = ezhal_fs_size(path.c_str());
    if (sz <= 0) {
        ezhal_fs_remove(path.c_str());
        return;
    }
    uint8_t *b = (uint8_t *)malloc((size_t)sz);
    if (b == NULL) return;
    if (ezhal_fs_read(path.c_str(), 0, b, (uint32_t)sz) == sz &&
        postMedia(fsImageEventId(st.oldest), b, (uint32_t)sz, "audio/wav")) {
        String hist = String(FS_AUD_DIR) + "/" + st.oldest;
        ezhal_fs_rename(path.c_str(), hist.c_str());
        fsTrimDir(FS_AUD_DIR, FS_AUD_KEEP_BYTES, ".wav");
        LOG("Pending sound clip uploaded: " + st.oldest);
    }
    free(b);
}

void initStorage()
{
    {
        HalStatus fst = ezhal_fs_mount(true, &fsDetail);
        fsAvailable = fst == HS_OK;
        Serial.println(String("File system: ") + (fst == HS_OK ? "" : (fst == HS_NOT_PRESENT ? "none - " : "FAILED - ")) + fsDetail);
        if (fsAvailable) {
            fsBufInit();
            ezhal_fs_mkdir(FS_IMG_DIR);
            ezhal_fs_mkdir(FS_PEND_DIR);
            ezhal_fs_mkdir(FS_AUD_PEND_DIR);
            if (clipRing == NULL) clipRing = (uint8_t *)malloc(CLIP_RING_BYTES);   // 160 KB, 32 MB boards only
            Serial.println("File system data: offline records=" + String(fsBufPending) + " pictures=" +
                           String(fsDirStats(FS_IMG_DIR, ".jpg").files) + " pending pictures=" +
                           String(fsDirStats(FS_PEND_DIR, ".jpg").files));
        }
    }
#ifdef DATA_STORE_FLASH
    lockM(sdMutex);
    bool evOk = ezhal_flog_init(EZFLOG_EVENTS) == HS_OK;
    bool bufOk = ezhal_flog_init(EZFLOG_BUFFER) == HS_OK;
    storeOk = evOk && bufOk;
    bufferedRecords = ezhal_flog_pending(EZFLOG_BUFFER);
    EzFlashLogStats ev, bf;
    ezhal_flog_stats(EZFLOG_EVENTS, &ev);
    ezhal_flog_stats(EZFLOG_BUFFER, &bf);
    unlockM(sdMutex);
    Serial.println(String("Flash data store ") + (storeOk ? "ready" : "FAILED") + ": events=" + String(ev.records) +
                   " buffered=" + String(bf.pending) + " max_erase=" + String(bf.max_erase > ev.max_erase ? bf.max_erase : ev.max_erase));
#endif
#ifdef ENABLE_SD_STORAGE
    lockM(sdMutex);
    sdOk = ezhal_sd_mount();
    if (sdOk) {
        sdRoot = "0:/";                          // display only; HAL paths are relative to the card root
        ezhal_sd_mkdir("ezsn");
        ezhal_sd_mkdir("ezsn/img");
#ifndef DATA_STORE_FLASH
        bufferedRecords = ezhal_sd_size("ezsn/buffer.txt") > 0 ? 1 : 0;   // exact count unknown after reboot
#endif
    }
    unlockM(sdMutex);
    Serial.println(sdOk ? "SD card ready: " + sdRoot
                        : String("SD card not available (insert FAT32 microSD for event log, event images and offline buffering)"));
#endif
}

// SD usable by the application: card present and no OTA task reading it (the SDK OTA task mounts
// the card through its own VFS, so concurrent application writes could corrupt the file system)
bool sdUsable()
{
    return sdOk && !otaInProgress;
}

// One line of the leak-event log (ts,id,state,...): flash ring, or ezsn/events.csv on the SD card
bool storeEventLine(String line)
{
    if (fsAvailable) return fsLogAppend(line);
#ifdef DATA_STORE_FLASH
    if (!storeOk) return false;
    lockM(sdMutex);
    HalStatus st = ezhal_flog_append(EZFLOG_EVENTS, (const uint8_t *)line.c_str(), (uint16_t)line.length());
    unlockM(sdMutex);
    return st == HS_OK;
#else
    return sdAppendLine("ezsn/events.csv", line);
#endif
}

bool sdAppendLine(String relPath, String line)
{
    if (!sdUsable()) return false;
    line += "\n";
    lockM(sdMutex);
    HalStatus st = ezhal_sd_append(relPath, (const uint8_t *)line.c_str(), line.length());
    unlockM(sdMutex);
    return st == HS_OK;
}

bool sdWriteFile(String relPath, const uint8_t *data, uint32_t len)
{
    if (!sdUsable()) return false;
    lockM(sdMutex);
    HalStatus st = ezhal_sd_write(relPath, data, len);
    unlockM(sdMutex);
    return st == HS_OK;
}

void bufferToStorage(String record)
{
    if (record.length() == 0) return;
#ifdef DATA_STORE_FLASH
    if (!storeOk) {
        droppedRecords++;
        return;
    }
    // Flash holds 144 KB: while offline keep the newest record of each upload cycle (about 1 per second,
    // ~40 minutes) rather than all 20 per second (~2 minutes).
    if (lastFlashBufMs != 0 && millis() - lastFlashBufMs < FLASH_BUF_INTERVAL_MS) {
        thinnedRecords++;
        return;
    }
    if (record.endsWith("$")) record = record.substring(0, record.length() - 1);
    int cut = -1;
    for (int i = (int)record.length() - 1; i >= 0; i--) {     // last '_' (String::lastIndexOf is buggy in core 4.1.0)
        if (record[i] == '_') { cut = i; break; }
    }
    String last = record.substring(cut + 1) + "$";
    if (fsAvailable) {                         // 32 MB boards: 8 MB on the file system (~40 h)
        if (fsBufAppend(last)) lastFlashBufMs = millis();
        else droppedRecords++;
        bufferedRecords = ezhal_flog_pending(EZFLOG_BUFFER) + fsBufPending;
        return;
    }
    lockM(sdMutex);
    HalStatus st = ezhal_flog_append(EZFLOG_BUFFER, (const uint8_t *)last.c_str(), (uint16_t)last.length());
    bufferedRecords = ezhal_flog_pending(EZFLOG_BUFFER);
    unlockM(sdMutex);
    if (st == HS_OK) lastFlashBufMs = millis();
    else droppedRecords++;
    return;
#endif
    if (!sdUsable()) {
        droppedRecords++;
        return;
    }
    record += "\n";
    lockM(sdMutex);
    int32_t size = ezhal_sd_size("ezsn/buffer.txt");
    if (size < 0) size = 0;
    if ((uint32_t)size < SD_BUFFER_MAX_BYTES &&
        ezhal_sd_append("ezsn/buffer.txt", (const uint8_t *)record.c_str(), record.length()) == HS_OK) {
        bufferedRecords++;
    } else {
        droppedRecords++;
    }
    unlockM(sdMutex);
}

// Send buffered records (oldest first). Returns false if sending failed.
bool replayBuffered()
{
#ifdef DATA_STORE_FLASH
    if (!storeOk || ezhal_flog_pending(EZFLOG_BUFFER) == 0) {
        bool fsok = fsBufReplay();             // nothing (left) in the internal flash buffer
        bufferedRecords = fsBufPending;
        return fsok;
    }
    static uint8_t rbuf[512];
    int sent = 0;
    while (sent < 50) {                         // limit per call so live data is not starved
        uint16_t len = 0;
        lockM(sdMutex);
        HalStatus st = ezhal_flog_peek(EZFLOG_BUFFER, rbuf, sizeof(rbuf) - 1, &len);
        if (st == HS_ERROR) ezhal_flog_consume(EZFLOG_BUFFER);   // unusable record: skip it
        unlockM(sdMutex);
        if (st == HS_NOT_PRESENT) break;
        if (st != HS_OK) continue;
        rbuf[len] = 0;
        if (!submitDataWithSocket(String((const char *)rbuf))) return false;
        lockM(sdMutex);
        ezhal_flog_consume(EZFLOG_BUFFER);      // only after the server accepted it
        bufferedRecords = ezhal_flog_pending(EZFLOG_BUFFER) + fsBufPending;
        unlockM(sdMutex);
        sent++;
        beat(T_UP);
    }
    if (sent > 0 && bufferedRecords == 0) LOG("Store-and-forward buffer fully replayed (flash)");
    return true;
#endif
    if (!sdUsable() || bufferedRecords == 0) return true;
    const String p = "ezsn/buffer.txt";
    int sentNow = 0;
    while (sentNow < 50) {                      // limit per call so live data is not starved
        lockM(sdMutex);
        int32_t total = ezhal_sd_size(p);
        if (total < 0 || bufferReplayOffset >= (uint32_t)total) {
            if (total >= 0) ezhal_sd_remove(p);
            unlockM(sdMutex);
            bufferedRecords = 0;
            bufferReplayOffset = 0;
            if (total >= 0) LOG("Store-and-forward buffer fully replayed");
            return true;
        }
        String line = "";
        uint32_t next = bufferReplayOffset;
        HalStatus st = ezhal_sd_read_line(p, bufferReplayOffset, &line, &next);
        unlockM(sdMutex);
        if (st != HS_OK) return true;
        if (line.length() > 0) {
            if (!submitDataWithSocket(line)) return false;
        }
        bufferReplayOffset = next;
        sentNow++;
        beat(T_UP);
    }
    return true;
}

String readEventLogTail(int lines)
{
    if (fsAvailable) return fsLogTail(lines);
#ifdef DATA_STORE_FLASH
    String fout = "";
    lockM(sdMutex);
    ezhal_flog_read_recent(EZFLOG_EVENTS, (uint32_t)lines, &fout);
    unlockM(sdMutex);
    return storeOk ? fout : String("flash data store not available");
#endif
    if (!sdOk) return "SD not available";
    if (otaInProgress) return "SD busy: OTA in progress";
    const String p = "ezsn/events.csv";
    String out = "";
    lockM(sdMutex);
    int32_t sz = ezhal_sd_size(p);
    if (sz > 0) {
        uint32_t want = (uint32_t)lines * 120;
        uint32_t start = (uint32_t)sz > want ? (uint32_t)sz - want : 0;
        uint32_t len = (uint32_t)sz - start;
        uint8_t *buf = (uint8_t *)malloc(len + 1);
        if (buf != NULL) {
            int32_t n = ezhal_sd_read_at(p, start, buf, len);
            if (n > 0) {
                buf[n] = 0;
                out = String((char *)buf);
            }
            free(buf);
        }
        if (start > 0) {
            int nl = out.indexOf('\n');
            if (nl >= 0) out = out.substring(nl + 1);
        }
    }
    unlockM(sdMutex);
    return out;
}

// ====================================================================================
// Network helpers (FW-09, FW-10, FW-20)
// ====================================================================================
bool ensureWiFi()
{
    if (ezhal_radio_connected()) return true;
    if (millis() - lastWifiAttemptMs < 15000) return false;
    lastWifiAttemptMs = millis();
    LOG("WiFi lost, reconnecting...");
    ezhal_radio_disconnect();
    ezhal_radio_begin(cfg.ssid, cfg.pass);
    wifiReconnects++;
    if (ezhal_radio_connected()) {
        LOG("WiFi reconnected");
        return true;
    }
    return false;
}

bool ensureSocket()
{
    lockM(sockMutex);
    bool isUp = clientSocket.connected();
    unlockM(sockMutex);
    if (isUp) return true;
    if (millis() - lastSockAttemptMs < sockBackoffMs) return false;
    lastSockAttemptMs = millis();
    if (startWebSocket(false)) {
        sockBackoffMs = 2000;
        return true;
    }
    sockBackoffMs = sockBackoffMs * 2 > 60000 ? 60000 : sockBackoffMs * 2;
    return false;
}

void checkNetworkWatchdog()
{
    if (lastServerOkMs == 0) lastServerOkMs = millis();
    if (cfg.netRebootMin > 0 && millis() - lastServerOkMs > cfg.netRebootMin * 60000UL) {
        rebootWithReason(RR_NETWORK_LOST, String(cfg.netRebootMin) + "min");
    }
}

String urlEncode(String s)
{
    String o = "";
    const char *hex = "0123456789ABCDEF";
    for (unsigned int i = 0; i < s.length(); i++) {
        char c = s[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            o += c;
        } else {
            o += '%';
            o += hex[(c >> 4) & 0xF];
            o += hex[c & 0xF];
        }
    }
    return o;
}

bool startWebSocket(bool rebootOnFail)
{
	int countFailed = 0;

    if (!ezhal_radio_connected()) return false;
    IPAddress localIp = ezhal_radio_local_ip();
    String _localIP = (String)localIp.get_address();
    // fixed: SDK String::lastIndexOf(char) uses strchr() and returns the FIRST match, so the old
    // substring() gave "168.100.57" -> port 4168. Use the last address byte directly.
    String lastNum = String(localIp[3]);
    clientSocketPort = cfg.socketBasePort + localIp[3];                          // fixed: no accumulation on reconnect
    String kHostname = cfg.serverAddr;

    Serial.println("Start localIP: " + _localIP + " last num:" + lastNum);
    Serial.println("Start WebSocket: " + kHostname + ":" + String(clientSocketPort));

    lockM(sockMutex);
    if (clientSocketOpened) {               // never stop() a client that never connected (closes fd 4)
        clientSocket.stop();
        clientSocketOpened = false;
    }
    bool connected = false;
    int maxTry = rebootOnFail ? 15 : 3;
    while (countFailed < maxTry) {
#ifdef USE_TLS_SOCKET
        connected = clientSocket.connect(kHostname.c_str(), clientSocketPort, (unsigned char *)TLS_PSK_IDENTITY, (unsigned char *)TLS_PSK_KEY);
#else
        connected = clientSocket.connect(kHostname.c_str(), clientSocketPort);
#endif
        if (connected) {
            clientSocketOpened = true;
            break;
        }
        Serial.println("connection socket failed: " + String(clientSocketPort));            // Lucas
        countFailed++;
        delay(1000);                                                                     // fixed: retry delay
    }
    unlockM(sockMutex);
    if (!connected) {
        if (rebootOnFail) {
            rebootWithReason(RR_SOCKET_FAIL, String(clientSocketPort));
        }
        return false;
    }
    sockReconnects++;
    lastServerOkMs = millis();
    Serial.println("connected to socket server");
    return true;
}

void socketReply(String msg, String tag)
{
    // Replies are prefixed with '#' so the server can separate them from sensor records.
    // A command sent as "CMD#<tag> <command>" is answered as "#RSP#<tag> <reply>$" so the server
    // can match replies to requests even if one reply is lost.
    String out = (tag.length() ? "#RSP#" + tag + " " : String("#RSP ")) + msg + "$";
    lockM(sockMutex);
    if (clientSocket.connected()) {
        clientSocket.write((const uint8_t *)out.c_str(), out.length());
    }
    unlockM(sockMutex);
}

void thread_socket_server_task(const void *argument)
{
    String line = "";
    uint8_t chunk[256];
    while (1) {
        beat(T_SOCK);
        // Drain received bytes while holding the socket lock (stop()/connect() run under it too),
        // then process outside the lock: executeCommand() may take seconds and socketReply() locks.
        lockM(sockMutex);
        int got = 0;
        if (clientSocket.connected()) {
            got = clientSocket.read(chunk, sizeof(chunk));
        }
        unlockM(sockMutex);
        if (got > 0) {
            // Commands are "CMD <command>" / "CMD#<tag> <command>" terminated by \n, \r or $
            for (int i = 0; i < got; i++) {
                uint8_t c = chunk[i];
                if (c == '\n' || c == '\r' || c == '$') {
                    line.trim();
                    if (line.length() > 0) {
                        Serial.print("Socket server data: ");
                        Serial.println(line);
                        if (line.startsWith("CMD ")) {
                            String r = executeCommand(line.substring(4));
                            socketReply(r, "");
                        } else if (line.startsWith("CMD#")) {
                            int sp = line.indexOf(' ');
                            if (sp > 4 && sp <= 20) {
                                String tag = line.substring(4, sp);
                                String r = executeCommand(line.substring(sp + 1));
                                socketReply(r, tag);
                            }
                        }
                    }
                    line = "";
                } else if (line.length() < 256) {
                    line += (char)c;
                }
            }
            continue;                           // more data may be waiting
        }
        delay(200);
    }
}

void thread_wd_task(const void *argument)
{
    Serial.println("Starting Watch Dog ...");
    uint32_t lastRun = millis();
    while (1) {
        // The hardware WDT resets after WDT_TIMEOUT (8 s). Refreshing every 1 s (was every 6 s, i.e. only
        // 2 s of slack) so CPU-heavy moments (JPEG serve + RTSP restart + SD write) cannot starve it into a
        // false reset. Real hangs are still caught: a stalled worker stops the refreshes (TASK_STALL_MS).
        uint32_t gap = millis() - lastRun;
        lastRun = millis();
        if (gap > 3000) {
            LOG("WDT: watchdog thread was delayed " + String(gap) + " ms");
        }
        // FW-20: refresh only if every registered worker task is alive
        String stalled = "";
        uint32_t now = millis();
        for (int i = 0; i < T_COUNT; i++) {
            if (taskBeat[i] != 0 && now - taskBeat[i] > TASK_STALL_MS) {
                stalled = taskNames[i];
                break;
            }
        }
        if (stalled == "") {
            ezhal_wdt_refresh();
        } else if (!stallRecorded) {
            stallRecorded = true;
            LOG("Task stalled: " + stalled + " -> watchdog reset");
            recordResetReason(RR_TASK_STALL, stalled);
        }
        //Serial.println("Watch Dog checking...");
        delay(1000);
    }
}

// ====================================================================================
// Camera (FW-07)
// ====================================================================================
void cameraOn()
{
    ezhal_cam_start(CHANNEL, 1500);           // 1.5 s for auto exposure when the channel was stopped
}

void cameraIdle()
{
    if (cfg.powerMode == 1 && !isVideoStreaming()) {
        ezhal_cam_stop(CHANNEL);
    }
}

bool grabImage()
{
    cameraOn();
    lastCaptureOk = ezhal_cam_snapshot(CHANNEL, &img_addr, &img_len) == HS_OK;
    return lastCaptureOk;
}

/// sample POST data
void thread_capture_picture_task(const void *argument)
{
    while(1)
    {
        beat(T_CAP);
#ifdef EVENT_VIDEO_CLIP
        if (bCaptureClip) {
            bCaptureClip = false;
            recordClip(captureEventId);
        }
#endif
        if (!bCapturePicture)
        {
            delay(100);
            clipService();
            fsRetryPendingImage();
            fsRetryPendingAudio();
            continue;
        }
        Serial.println("===========================Starting Capture picture ...");
        //delay(5000);
        bCapturePicture = false;
        String evId = captureEventId;
        lockM(camMutex);
        bool ok = grabImage();
        if (ok) {
            captureCount++;
            sdWriteFile("ezsn/img/" + evId + ".jpg", (uint8_t *)img_addr, img_len);
        }
        Serial.println("===========================DONE ...");
        if (!ok) {
            unlockM(camMutex);
            Serial.println("Capture failed");
            continue;
        }
        // (postImage connects without setRecvTimeout(): on a not-yet-connected WiFiClient it calls
        //  connected() -> stop() -> close(4), which killed the data socket.)
        bool sent = postImage(evId, (const uint8_t *)img_addr, img_len);
        Serial.println(sent ? "Upload finished" : "Upload failed");
        if (fsAvailable) {
            String name = fsImageName(evId);
            if (sent) {
                fsSaveImage(FS_IMG_DIR, name, (const uint8_t *)img_addr, img_len, FS_IMG_KEEP_BYTES);
            } else {
                fsSaveImage(FS_PEND_DIR, name, (const uint8_t *)img_addr, img_len, FS_PEND_KEEP_BYTES);
                LOG("Picture kept for a later upload: " + name);
            }
        }
        cameraIdle();
        unlockM(camMutex);
        Serial.println("DONE.");
    }
}

#ifdef EVENT_VIDEO_CLIP
void recordClip(String evId)
{
    if (!sdUsable() || thermalDerate || clipRecording) return;
    clipRecording = true;
    LOG("Recording clip for event " + evId);
    mp4.setRecordingFileName("ezsn_" + evId);
    mp4.setRecordingDuration(cfg.clipSeconds);
    ezhal_cam_start(CLIP_CHANNEL, 0);
    mp4.begin();
    for (uint32_t i = 0; i < cfg.clipSeconds + 2; i++) {
        beat(T_CAP);
        delay(1000);
    }
    mp4.end();
    ezhal_cam_stop(CLIP_CHANNEL);
    clipRecording = false;
    LOG("Clip done");
}
#endif

void start_multithreading()
{
    Serial.println("Starting multithreading");
    priority_server = osPriorityNormal;
    stack_size_server = 10240;
    thread_server_id = os_thread_create_arduino(thread_socket_server_task, NULL, priority_server, stack_size_server);

    if (thread_server_id) {
        Serial.println("Thread Server created successfully");
    } else {
        Serial.println("Failed to create thread Server");
    }

    stack_size_wd = 2048;
    stack_size_wd = 4096;               // String + LOG use in the stall path
    // above the worker threads, so busy workers cannot starve the watchdog refresh
    thread_wd_id = os_thread_create_arduino(thread_wd_task, NULL, osPriorityAboveNormal, stack_size_wd);

    if (thread_wd_id) {
        Serial.println("Thread WD created successfully");
    } else {
        Serial.println("Failed to create thread WD");
    }

    uint32_t stack_size_capture_picture = 1024*50;
    uint32_t thread_id_capture_picture = os_thread_create_arduino(thread_capture_picture_task, NULL, osPriorityNormal, stack_size_capture_picture);

    if (thread_id_capture_picture) {
        Serial.println("Thread thread_id_capture_picture created successfully");
    } else {
        Serial.println("Failed to create thread thread_capture_picture_task");
    }

    uint32_t stack_size_get_data = 1024*20;
    if (os_thread_create_arduino(thread_env_task, NULL, osPriorityBelowNormal, 1024 * 4)) {
        Serial.println("Thread env created successfully");
    } else {
        Serial.println("Thread env creation failed");
    }
    uint32_t thread_id_get_data = os_thread_create_arduino(thread_get_data_task, NULL, osPriorityNormal, stack_size_get_data);

    if (thread_id_get_data) {
        Serial.println("Thread thread_id_get_data created successfully");
    } else {
        Serial.println("Failed to create thread thread_id_get_data");
    }

    uint32_t stack_size_up_data = 1024*12;
    uint32_t thread_id_up_data = os_thread_create_arduino(thread_up_data_task, NULL, osPriorityNormal, stack_size_up_data);

    if (thread_id_up_data) {
        Serial.println("Thread thread_id_up_data created successfully");
    } else {
        Serial.println("Failed to create thread thread_id_up_data");
    }

    // New: event fusion, heartbeat/health, serial service CLI
    if (os_thread_create_arduino(thread_event_task, NULL, osPriorityNormal, 1024 * 12)) {
        Serial.println("Thread event created successfully");
    } else {
        Serial.println("Failed to create thread event");
    }
    if (os_thread_create_arduino(thread_heartbeat_task, NULL, osPriorityNormal, 1024 * 12)) {
        Serial.println("Thread heartbeat created successfully");
    } else {
        Serial.println("Failed to create thread heartbeat");
    }
}

uint32_t effectiveSampleMs()
{
    // FW-11: ECO mode samples slower, but returns to full rate while an event is active
    if (cfg.powerMode == 1 && eventState == EVT_NORMAL) {
        return cfg.sampleIntervalMs * 10;
    }
    return cfg.sampleIntervalMs;
}

uint32_t effectiveUploadMs()
{
    if (cfg.powerMode == 1 && eventState == EVT_NORMAL) {
        return cfg.uploadIntervalMs * 5;
    }
    return cfg.uploadIntervalMs;
}

// measured sample rate over windows of >= 10 s (FW-03, FW-13)
void sampleRateUpdate(uint32_t now)
{
    if (sampleWinStartMs == 0) {
        sampleWinStartMs = now;
        sampleWinCount = sampleCount;
        return;
    }
    uint32_t el = now - sampleWinStartMs;
    if (el < 10000) return;
    sampleRateHz = (float)(sampleCount - sampleWinCount) * 1000.0f / (float)el;
    sampleWinStartMs = now;
    sampleWinCount = sampleCount;
}

// SAMPLING self-test line: below 80 % of the configured rate fails (bit 7)
bool samplingOk(String *line)
{
    float want = 1000.0f / (float)effectiveSampleMs();
    if (sampleRateHz <= 0) {
        *line = "SAMPLING:N/A (measured 10 s after start)";
        return true;
    }
    bool ok = sampleRateHz >= 0.8f * want;
    *line = String("SAMPLING:") + (ok ? "PASS " : "FAIL ") + fnum(sampleRateHz, 1) + " Hz (configured " + fnum(want, 1) + " Hz)";
    return ok;
}

// AHT20 bus work in its own low-priority thread: a missing or slow sensor never delays sampling
void thread_env_task(const void *argument)
{
    (void)argument;
    while (1) {
        ezhal_env_service(millis());
        delay(100);
    }
}

void thread_get_data_task(const void *argument)
{
    Serial.println("Starting Get Sensor ...");
    while (1) {
        beat(T_GET);
        if (bSensorGetting) {
            delay(100);
            continue;
        }
        //bSensorReading = true;
        String _data = getSensorData();
        sampleCount++;
        sampleRateUpdate(millis());
        //bSensorReading = false;
        lockM(dataMutex);                       // fixed: shared with upload thread and HTTP handler
        sensorData[sensorDataIdx] = _data;
        sensorDataIdx++;
        if(sensorDataIdx >= SENSOR_DATA_LENGTH)
            sensorDataIdx=0;
        unlockM(dataMutex);

        delay(effectiveSampleMs());
    }
}

void printInfo(void)
{
    Serial.println("------------------------------");
    Serial.println("- Summary of Streaming -");
    Serial.println("------------------------------");
    Camera.printInfo();

    IPAddress ip = ezhal_radio_local_ip();

    Serial.println("- RTSP -");
    Serial.print("rtsp://");
    Serial.print(ip);
    Serial.print(":");
    rtsp.printInfo();

    Serial.println("- Audio -");
    audio.printInfo();
}

// ====================================================================================
// Sensor processing (FW-03, FW-04)
// ====================================================================================
void processGas(int raw, float tempC, float rhPct, bool envValid)
{
    gasHist[gasHistIdx] = raw;
    gasHistIdx = (gasHistIdx + 1) % GAS_AVG_N;
    if (gasHistCnt < GAS_AVG_N) gasHistCnt++;
    long s = 0;
    for (int i = 0; i < gasHistCnt; i++) s += gasHist[i];
    gasFiltered = (float)s / gasHistCnt;

    gasOk = (raw > 3 && raw < 1020);          // rail values indicate open/short circuit
    float vout = gasFiltered * 3.3f / 1023.0f;
    if (vout < 0.01f) vout = 0.01f;
    gasRs = cfg.gasRL * (cfg.gasVc - vout) / vout;
    if (gasRs < 0) gasRs = 0;

    bool warm = (millis() - bootMs) / 1000 >= cfg.gasWarmupS;
    if (cfg.gasR0 > 0) {
        float ratio = gasRs / cfg.gasR0;
        if (envValid) {
            float k = 1.0f + cfg.gasTempCoef * (tempC - 20.0f) + cfg.gasHumCoef * (rhPct - 65.0f);
            if (k > 0.1f) ratio = ratio / k;
        }
        gasPpm = (ratio > 0) ? cfg.gasCurveA * powf(ratio, cfg.gasCurveB) : -1;
        gasLel = gasPpm >= 0 ? gasPpm / 500.0f : -1;     // methane LEL = 5 %vol = 50000 ppm
        if (warm && gasOk) {
            if (!gasAlarm && gasPpm >= cfg.gasAlarmPpm) gasAlarm = true;
            else if (gasAlarm && gasPpm < cfg.gasClearPpm) gasAlarm = false;
        } else {
            gasAlarm = false;
        }
    } else {
        gasPpm = -1;
        gasLel = -1;
        // not calibrated: optional raw threshold fallback
        if (warm && gasOk && cfg.gasAlarmRaw > 0) {
            if (!gasAlarm && gasFiltered >= cfg.gasAlarmRaw) gasAlarm = true;
            else if (gasAlarm && gasFiltered < cfg.gasAlarmRaw * 0.9f) gasAlarm = false;
        } else {
            gasAlarm = false;
        }
    }
}

#ifdef ACOUSTIC_DETECTION
// Called by the audio pipeline for every raw PCM frame (16-bit mono). Must stay fast.
void audioTapHandler(const int16_t *s, uint32_t n)
{
    if (s == NULL || n == 0) return;
    int64_t sum = 0, sumHf = 0;
    int32_t prev = acPrevSample;
    for (uint32_t i = 0; i < n; i++) {
        int32_t x = s[i];
        int32_t d = x - prev;              // first difference ~ high-pass (emphasises hiss/leak band)
        prev = x;
        sum += (int64_t)x * x;
        sumHf += (int64_t)d * d;
    }
    acPrevSample = (int16_t)prev;
    taskENTER_CRITICAL();
    acAccSum += sum;
    acAccHf += sumHf;
    acAccN += n;
    taskEXIT_CRITICAL();
    acFrames++;
    acLastFrameMs = millis();
    clipFeed(s, n);
}
#endif

void processAcoustic()
{
    taskENTER_CRITICAL();
    int64_t sum = acAccSum, sumHf = acAccHf;
    uint32_t n = acAccN;
    acAccSum = 0;
    acAccHf = 0;
    acAccN = 0;
    taskEXIT_CRITICAL();
    if (n == 0) return;                         // no new audio in this window
    const float fs2 = 32768.0f * 32768.0f;
    float p = (float)sum / n / fs2;
    float ph = (float)sumHf / n / fs2;
    acDb = 10.0f * log10f(p > 1e-12f ? p : 1e-12f);
    acHfDb = 10.0f * log10f(ph > 1e-12f ? ph : 1e-12f);
    if (!acFloorInit) {
        acFloorDb = acHfDb;
        acFloorInit = true;
    }
    float above = acHfDb - acFloorDb;
    if (!acAlarm) {
        if (above >= cfg.acThresholdDb) {
            acAboveCnt++;
            if (acAboveCnt >= cfg.acConfirmWindows) acAlarm = true;
        } else {
            acAboveCnt = 0;
            acFloorDb = acFloorDb * 0.995f + acHfDb * 0.005f;      // adapt noise floor only when quiet
        }
    } else if (above < cfg.acClearDb) {
        acAlarm = false;
        acAboveCnt = 0;
    }
}

bool micOk()
{
#ifdef ACOUSTIC_DETECTION
    // acFrames > 0: right after boot acLastFrameMs is 0, which looked "recent" for the first 3 s
    return ezhal_mic_present() && acFrames > 0 && (millis() - acLastFrameMs < 3000);
#else
    return false;
#endif
}

uint32_t validMask()
{
    uint32_t m = 0;
    if (ahtOk) m |= 1 << 0;
    if (gasOk) m |= 1 << 1;
    if ((millis() - bootMs) / 1000 >= cfg.gasWarmupS) m |= 1 << 2;
    if (cfg.gasR0 > 0) m |= 1 << 3;
    if (micOk()) m |= 1 << 4;
    if (ezhal_us_present()) m |= 1 << 5;
    if (timeSynced) m |= 1 << 6;
    if (cfg.maintenance) m |= 1 << 7;
    if (millis() < simGasUntil || millis() < simAcUntil) m |= 1 << 8;
    return m;
}

String getSensorData(){
  float temp = -999;
  float humidity = -999;
  int chem = -1;
  #ifdef TESTING
      temp = random(100);
      humidity = random(100) / 100.0;
      chem = random(1024);
      ahtOk = true;
  #else
      float t = 0, rh = 0;
      static bool ahtReported = true;               // log changes only, not every 50 ms sample
      if (ezhal_env_read(&t, &rh) != HS_OK)
      {
          ahtOk = false;
          if (ahtReported) {
              Serial.println("AHT20: no valid reading (sensor missing or faulty)");
              ahtReported = false;
          }
      } else {
          if (!ahtReported) {
              Serial.println("AHT20: readings valid again");
              ahtReported = true;
          }
          ahtOk = true;
          temp = t;
          humidity = rh / 100.0f;              // fraction, as before
      }
      // Chemical sensor
      if (ezhal_gas_read_raw(&chem) != HS_OK) chem = -1;
  #endif
  lastTemp = temp;
  lastHum = ahtOk ? humidity * 100 : -999;
  lastChemRaw = chem;
  thermalDerate = ahtOk && temp >= cfg.thermalLimitC;
  processGas(chem, temp, humidity * 100, ahtOk);
  processAcoustic();
  if (ezhal_us_read(&lastUs) != HS_OK) lastUs = -1;     // OP-01: threshold logic pending hardware
  //unsigned long ts = rtc.Read();//String(ts) + "," +
  String _data = (String)temp + "," + (String)(ahtOk ? humidity*100 : -999) + "," + (String)chem;
  if (EXTENDED_SENSOR_DATA) {
      recordSeq++;
      _data += "," + String(getEpoch()) + "," + String(recordSeq) + "," + fnum(gasPpm, 1) + "," + fnum(acDb, 1) + "," +
               fnum(acHfDb, 1) + "," + fnum(lastUs, 0) + "," + String((int)eventState) + "," + String(validMask());
  }
  //Serial.println("Data: " + _data);
  return _data;
}

// ====================================================================================
// HTTP interface (legacy + new service endpoints)
// ====================================================================================
void initPageListening(){
  app.get("/reboot", &rebootDevice);
  app.get("/index", &index);
  app.get("/get-sensor-data", &getSensorData);
  app.get("/get-picture", &getCameraPicture);
  // New service / integration endpoints (token protected)
  app.get("/status", &httpStatus);
  app.get("/config", &httpConfig);
  app.get("/cmd", &httpCmd);
  app.get("/calibrate", &httpCalibrate);
  app.get("/self-test", &httpSelfTest);
}

bool authorized(Request &req, bool legacy)
{
  if (legacy && !cfg.secureAll) return true;
  if (cfg.apiToken[0] == 0) return true;
  char tok[40] = {0};
  if (req.query("token", tok, sizeof(tok)) && strcmp(tok, cfg.apiToken) == 0) return true;
  return false;
}

void rebootDevice(Request &req, Response &res){
  Serial.println("- rebootDevice -");
  if (!authorized(req, false)) {
    res.sendStatus(401);
    return;
  }
  res.print("Rebooting");
  res.flush();
  //submitData();
  rebootWithReason(RR_CMD_REBOOT, "http");
}

void getCameraPicture(Request &req, Response &res){
	Serial.println("- getCameraPicture -");
	if (!authorized(req, true)) {
		res.sendStatus(401);
		return;
	}
	lockM(camMutex);
	bool ok = grabImage();
	if (ok) {
		res.set("Content-Type", "image/jpeg");
		res.write((uint8_t*)img_addr, img_len);   // fixed: no trailing "DONE" (corrupted the JPEG)
	} else {
		res.sendStatus(503);
	}
	cameraIdle();
	unlockM(camMutex);
}

void getSensorData(Request &req, Response &res){
  //Serial.println("- getSensorData -");
  if (!authorized(req, true)) {
    res.sendStatus(401);
    return;
  }
  String _data = collectSensorData();
  res.print(_data);
}

String collectSensorData(){
  String _data = "";
  lockM(dataMutex);
  // sensorData is not full
  if(sensorData[SENSOR_DATA_LENGTH - 1] == "") {
    //Serial.println("- 1 -");
    for(int i=0; i<sensorDataIdx;i++) {
        if (sensorData[i] != "")
            _data = _data + "_" + sensorData[i];
    }
  } else {
    for(int i=sensorDataIdx; i<SENSOR_DATA_LENGTH;i++) {
        if (sensorData[i] != "")
            _data = _data + "_" + sensorData[i];
    }
    for(int i=0; i<sensorDataIdx;i++) {
        if (sensorData[i] != "")
            _data = _data + "_" + sensorData[i];
    }
  }
  // Empty Data Array
  for (int i = 0; i < SENSOR_DATA_LENGTH; i++) {
    sensorData[i] = "";
  }
  sensorDataIdx = 0;
  unlockM(dataMutex);

  if (_data.length() == 0) return "";
  _data = _data.substring(1);
  _data += "$";
  return _data;
}

void index(Request &req, Response &res){
  Serial.println("- webserverHeaalth -");
  //server.sendHeader("Access-Control-Allow-Origin","*");
  //server.send(200, "text/html", "I'm Fine!");
  res.set("Access-Control-Allow-Origin","*");
  res.print("I'm Fine!");
}

void httpStatus(Request &req, Response &res)
{
  if (!authorized(req, false)) {
    res.sendStatus(401);
    return;
  }
  res.set("Content-Type", "application/json");
  res.set("Access-Control-Allow-Origin", "*");
  res.print(buildStatus(true));
}

void httpConfig(Request &req, Response &res)
{
  if (!authorized(req, false)) {
    res.sendStatus(401);
    return;
  }
  char key[24] = {0};
  char val[72] = {0};
  if (req.query("key", key, sizeof(key)) && req.query("value", val, sizeof(val))) {
    String r = executeCommand("SET " + String(key) + " " + String(val));
    res.print(r);
    return;
  }
  res.set("Content-Type", "application/json");
  res.print(configDump(true));
}

void httpCmd(Request &req, Response &res)
{
  if (!authorized(req, false)) {
    res.sendStatus(401);
    return;
  }
  char c[160] = {0};
  if (!req.query("c", c, sizeof(c))) {
    res.print(executeCommand("HELP"));
    return;
  }
  for (char *p = c; *p; p++) if (*p == '+') *p = ' ';
  res.print(executeCommand(String(c)));
}

void httpCalibrate(Request &req, Response &res)
{
  if (!authorized(req, false)) {
    res.sendStatus(401);
    return;
  }
  char s[16] = {0};
  req.query("sensor", s, sizeof(s));
  String sensor = String(s);
  if (sensor.equalsIgnoreCase("acoustic")) res.print(executeCommand("CAL_AC"));
  else res.print(executeCommand("CAL_GAS"));
}

void httpSelfTest(Request &req, Response &res)
{
  if (!authorized(req, false)) {
    res.sendStatus(401);
    return;
  }
  res.print(executeCommand("SELFTEST"));
}

int submitData(String path) {
  int responseCode = 400;
  // Check first: destroying a never-connected WiFiClient closes fd 4 (see clientSocketOpened note)
  if (!ezhal_radio_connected()) return responseCode;
  IPAddress resolved;
  if (!ezhal_radio_resolve(cfg.serverAddr, &resolved)) return responseCode;   // failed lookup leaves _sock = 4
  WiFiClient c;
  // fixed: no c.setRecvTimeout() before connecting. In core 4.1.0 it calls connected() on the fresh
  // client, which calls stop() -> close(4): every heartbeat/event closed the sensor data socket and the
  // HTTP reply then arrived on it. connect() applies a 3 s receive timeout by itself.
  c.setBlockingMode();
  HttpClient http(c);
  String kHostname = cfg.serverAddr;
  uint16_t kPort = cfg.serverPort;
  Serial.println(path);
  int err = http.get(kHostname.c_str(), kPort, path.c_str());
  if (err == 0) {
    //Serial.println("StartedRequest ok");
    //Serial.print("Response status code: ");
    //Serial.println(http.responseStatusCode());
    responseCode = http.responseStatusCode();
    if (responseCode == 200 && http.skipResponseHeaders() >= 0) {
      // Edge Server >= 1.1.0 appends its clock: "DONE T=<epoch>" / "OK T=<epoch>"
      String body = "";
      uint32_t t0 = millis();
      while (body.length() < 64 && millis() - t0 < 500) {
        int ch = http.read();
        if (ch < 0) {
          if (!c.connected()) break;
          delay(10);
          continue;
        }
        body += (char)ch;
      }
      int tp = body.indexOf("T=");
      if (tp >= 0) setClock((uint32_t)body.substring(tp + 2).toInt());
    }
  }
  c.stop();
  return responseCode;
}

bool submitDataWithSocket(String data) {
    lockM(sockMutex);
    bool isUp = clientSocket.connected();
    unlockM(sockMutex);
    if (!isUp) {
        LOG("Socket disconnected, try to connect again !");
        //sys_reset();
        if (!ensureSocket()) return false;
    }
    lockM(sockMutex);
    size_t n = clientSocket.connected() ? clientSocket.write((uint8_t*)data.c_str(), data.length()) : 0;
    unlockM(sockMutex);
    bool ok = (n == data.length());
    if (ok) lastServerOkMs = millis();
    return ok;
}

void addDevice(int attempts) {
  Serial.println("- Add Sentinel Node to EdgeServer -");
  //http://192.168.56.102:8900/add-device?ip=192.168.101.184&name=DEMO_1                                    // Lucas
  //unsigned long ts = rtc.Read(); //+ "&ts=" + String(ts);
  // fixed: iterative retry (old recursive version reset its counter on each call and never gave up)
  for (int countFailed = 0; countFailed < attempts; countFailed++) {
    String kPath = "/add-device?ip=" + ezhal_radio_local_ip_str() + "&name=" + urlEncode(String(cfg.deviceName)) +
                   "&id=" + deviceId + "&fw=" + String(FW_VERSION) + "&boot=" + String(bootInfo.bootCount);
    if (ezhal_radio_connected() && ezhal_radio_local_ip_str() != "0.0.0.0" && submitData(kPath) == 200) {
      Serial.println("Added Successfully !");
      deviceRegistered = true;
      lastServerOkMs = millis();
      lastSockAttemptMs = 0;          // the server opens our socket port on registration: connect now
      sockBackoffMs = 2000;
      return;
    }
    Serial.println("Added Failed, retry after 2s");
    if (countFailed + 1 < attempts) delay(2000);
  }
  // Server not reachable: keep running (data is buffered), registration retried by heartbeat thread
  LOG("Add new device failed, will retry in background");
}

void thread_up_data_task(const void *argument) {
  String kPath = "";
  int queryServerFailedCount = 0;
  while(1) {
	  beat(T_UP);
	  //bSensorGetting = true;
	  if (bSensorReading) {
		delay(100);
		continue;
	  }
	  if (sensorData[0] == "") {
        //bSensorGetting = false;
		delay(900);
		continue;
	  }
      if (STREAMING_SOCKET == true) {
        static uint32_t lastCycleMs = 0;
        uint32_t cyc = millis();
        if (lastCycleMs != 0) upCycleMs = cyc - lastCycleMs;
        lastCycleMs = cyc;
        String _data = collectSensorData();
        bool sent = false;
        if (!simOffline() && ensureWiFi() && ensureSocket()) {
          if (replayBuffered()) {                 // FW-08: send stored records first (oldest first)
            sent = submitDataWithSocket(_data);
          }
        }
        serverLinkUp = sent;
        if (!sent) {
          bufferToStorage(_data);
          if (!simOffline()) checkNetworkWatchdog();   // a simulated outage never triggers the network reboot
        }
        if (PRINT_DEBUG) Serial.println(sent ? "submitDataWithSocket DONE" : "submitDataWithSocket buffered");
        //bSensorGetting = false;
        delay(effectiveUploadMs());
        continue;
      }

	  if (kPath == "") {
		kPath = "/up-data?data=" + collectSensorData();
      }
	  if (submitData(kPath) == 200) {
		Serial.println("Up Data Successfully !");
		kPath = "";
		//bSensorGetting = false;
		queryServerFailedCount = 0;
		lastServerOkMs = millis();
		delay(effectiveUploadMs());
	  } else {
		Serial.println("Update Data Failed, retry after 2s");
		delay(200);
		if (queryServerFailedCount == 25)
			rebootWithReason(RR_UPLOAD_FAIL, "http");
		queryServerFailedCount++;
	  }
  }
}

// ====================================================================================
// Event fusion state machine (FW-05, FW-06; FW-07 capture trigger; FW-10 events)
// ====================================================================================
String eventStateName(int s)
{
    return s == EVT_CONFIRMED ? "CONFIRMED" : (s == EVT_SUSPECT ? "SUSPECT" : "NORMAL");
}

void onEventChange(int oldState, int newState)
{
    // effective alarm flags, including SIM (a simulated gas event used to report g=0)
    uint32_t nowMs = millis();
    bool gEff = gasAlarm || nowMs < simGasUntil;
    bool aEff = acAlarm || nowMs < simAcUntil;
    String line = String(getEpoch()) + "," + eventId + "," + eventStateName(newState) + "," + fnum(gasPpm, 1) + "," +
                  fnum(gasFiltered, 0) + "," + fnum(acHfDb - acFloorDb, 1) + "," + String(gEff ? 1 : 0) +
                  String(aEff ? 1 : 0) + String(usAlarm ? 1 : 0) + "," + String(cfg.maintenance) + "," + String(validMask());
    LOG("EVENT " + line);
    storeEventLine(line);

    String path = "/node-event?name=" + urlEncode(String(cfg.deviceName)) + "&id=" + deviceId + "&event=" + eventId +
                  "&state=" + eventStateName(newState) + "&prev=" + eventStateName(oldState) + "&gas_ppm=" + fnum(gasPpm, 1) +
                  "&gas_raw=" + fnum(gasFiltered, 0) + "&ac_db=" + fnum(acHfDb - acFloorDb, 1) + "&g=" + String(gEff ? 1 : 0) +
                  "&a=" + String(aEff ? 1 : 0) + "&u=" + String(usAlarm ? 1 : 0) + "&maint=" + String(cfg.maintenance) +
                  "&ts=" + String(getEpoch()) + "&valid=" + String(validMask());
    submitData(path);

    if (newState > oldState && !cfg.maintenance) clipRequestIfOffline(eventId);   // sound clip only while the server is unreachable; none in maintenance (technician noise)
    if (newState > oldState && !cfg.maintenance && !thermalDerate) {
        captureEventId = eventId;
        bCapturePicture = true;                       // FW-07: event-triggered capture
#ifdef EVENT_VIDEO_CLIP
        if (newState == EVT_CONFIRMED) bCaptureClip = true;
#endif
    }
}

// One evaluation of the fusion state machine at time 'now' (ms). Called every 200 ms by
// thread_event_task; separate so it can be unit-tested (FW-22).
void eventStep(uint32_t now)
{
    bool g = gasAlarm || now < simGasUntil;
    bool a = acAlarm || now < simAcUntil;
    bool u = usAlarm;
    if (g) { lastGasAlarmMs = now; everGas = true; }
    if (a) { lastAcAlarmMs = now; everAc = true; }
    if (u) { lastUsAlarmMs = now; everUs = true; }
    if (g || a || u) lastAnyAlarmMs = now;

    uint32_t win = cfg.fusionWindowS * 1000UL;
    int recent = (everGas && now - lastGasAlarmMs < win) + (everAc && now - lastAcAlarmMs < win) +
                 (everUs && now - lastUsAlarmMs < win);
    int active = g + a + u;
    int target = EVT_NORMAL;
    if (recent >= 2 || (g && cfg.gasConfirmPpm > 0 && gasPpm >= cfg.gasConfirmPpm)) {
        target = EVT_CONFIRMED;
    } else if (active >= 1) {
        target = EVT_SUSPECT;
    }

    int cur = eventState;
    if (target > cur) {
        if (cur == EVT_NORMAL) {
            eventCounter++;
            eventId = "E" + String(bootInfo.bootCount) + "_" + String(eventCounter);
        }
        eventState = target;
        onEventChange(cur, target);
    } else if (cur != EVT_NORMAL && active == 0 && now - lastAnyAlarmMs > cfg.eventHoldS * 1000UL) {
        eventState = EVT_NORMAL;                  // latched until all sensors quiet for eventHoldS
        onEventChange(cur, EVT_NORMAL);
        everGas = everAc = everUs = false;
    }
}

void thread_event_task(const void *argument)
{
    while (1) {
        beat(T_EVENT);
        eventStep(millis());
        delay(200);
    }
}

// ====================================================================================
// Health / heartbeat (FW-13, FW-10)
// ====================================================================================
int32_t readInternalThermalRaw()
{
    int32_t raw = -1;
    return ezhal_temp_internal_raw(&raw) == HS_OK ? raw : -1;
}

void addKV(String &out, const char *k, String v, bool json, bool quote)
{
    if (json) {
        if (out.length() > 1) out += ",";
        out += "\"" + String(k) + "\":" + (quote ? "\"" + v + "\"" : v);
    } else {
        if (out.length() > 0) out += "&";
        out += String(k) + "=" + urlEncode(v);
    }
}

String buildStatus(bool json)
{
    String o = json ? "{" : "";
    int32_t itRaw = readInternalThermalRaw();
    addKV(o, "name", String(cfg.deviceName), json, true);
    addKV(o, "id", deviceId, json, true);
    addKV(o, "fw", FW_VERSION, json, true);
    addKV(o, "ip", ezhal_radio_local_ip_str(), json, true);
    addKV(o, "uptime_s", String(millis() / 1000), json, false);
    addKV(o, "time", String(getEpoch()), json, false);
    addKV(o, "boot_count", String(bootInfo.bootCount), json, false);
    addKV(o, "last_reset", resetReasonName(lastResetReason) + (lastResetDetail.length() ? ":" + lastResetDetail : ""), json, true);
    addKV(o, "heap", String(ezhal_heap_free()), json, false);
    addKV(o, "heap_min", String(ezhal_heap_min()), json, false);
    addKV(o, "rssi", String(ezhal_radio_rssi()), json, false);
    addKV(o, "wifi_reconnects", String(wifiReconnects), json, false);
    lockM(sockMutex);
    bool sockUp = clientSocket.connected();
    unlockM(sockMutex);
    addKV(o, "sock_connected", String(sockUp ? 1 : 0), json, false);
    addKV(o, "sock_reconnects", String(sockReconnects), json, false);
    addKV(o, "registered", String(deviceRegistered ? 1 : 0), json, false);
    addKV(o, "power_mode", cfg.powerMode ? "ECO" : "ACTIVE", json, true);
    addKV(o, "maint", String(cfg.maintenance), json, false);
    addKV(o, "int_temp_raw", String(itRaw), json, false);
    addKV(o, "int_temp_c", (cfg.intTempScale != 0 && itRaw >= 0) ? fnum(itRaw * cfg.intTempScale + cfg.intTempOffset, 1) : "null", json, false);
    addKV(o, "amb_temp_c", fnum(lastTemp, 2), json, false);
    addKV(o, "rh_pct", fnum(lastHum, 1), json, false);
    addKV(o, "aht_ok", String(ahtOk ? 1 : 0), json, false);
    addKV(o, "gas_raw", fnum(gasFiltered, 0), json, false);
    addKV(o, "gas_rs_kohm", fnum(gasRs, 2), json, false);
    addKV(o, "gas_ppm", fnum(gasPpm, 1), json, false);
    addKV(o, "gas_lel_pct", fnum(gasLel, 2), json, false);
    addKV(o, "gas_ok", String(gasOk ? 1 : 0), json, false);
    addKV(o, "gas_calibrated", String(cfg.gasR0 > 0 ? 1 : 0), json, false);
    addKV(o, "gas_warm", String((millis() - bootMs) / 1000 >= cfg.gasWarmupS ? 1 : 0), json, false);
    addKV(o, "mic_ok", String(micOk() ? 1 : 0), json, false);
    addKV(o, "ac_db", fnum(acDb, 1), json, false);
    addKV(o, "ac_hf_db", fnum(acHfDb, 1), json, false);
    addKV(o, "ac_floor_db", fnum(acFloorDb, 1), json, false);
    // Hardware still to be fitted reports null through its HAL stub (OP-01, OP-04, OP-08)
    HalPowerStatus pw;
    HalTamperStatus tp;
    bool pwOk = ezhal_power_read(&pw) == HS_OK;
    bool tpOk = ezhal_tamper_read(&tp) == HS_OK;
    addKV(o, "us", ezhal_us_present() ? fnum(lastUs, 0) : String("null"), json, false);
    addKV(o, "battery", pwOk ? fnum(pw.battery_v, 2) : String("null"), json, false);
    if (pwOk) {
        addKV(o, "soc_pct", fnum(pw.soc_pct, 0), json, false);
        addKV(o, "solar_v", fnum(pw.solar_v, 2), json, false);
        addKV(o, "charging", String(pw.charging ? 1 : 0), json, false);
        addKV(o, "power_fault", String(pw.fault ? 1 : 0), json, false);
    }
    addKV(o, "tamper", tpOk ? String((tp.moved || tp.enclosure_open || tp.detached) ? 1 : 0) : String("null"), json, false);
    addKV(o, "cam_ok", String(lastCaptureOk ? 1 : 0), json, false);
    addKV(o, "captures", String(captureCount), json, false);
#ifdef DATA_STORE_FLASH
    addKV(o, "storage", "flash", json, true);
    {
        EzFlashLogStats ev, bf;
        lockM(sdMutex);
        ezhal_flog_stats(EZFLOG_EVENTS, &ev);
        ezhal_flog_stats(EZFLOG_BUFFER, &bf);
        unlockM(sdMutex);
        addKV(o, "flash_ok", String(storeOk ? 1 : 0), json, false);
        addKV(o, "flash_buf_pending", String(bf.pending), json, false);
        addKV(o, "flash_buf_used_pct", String(bf.capacity ? bf.used * 100 / bf.capacity : 0), json, false);
        addKV(o, "flash_buf_dropped", String(bf.dropped), json, false);
        addKV(o, "flash_buf_thinned", String(thinnedRecords), json, false);
        addKV(o, "flash_evt_records", String(ev.records), json, false);
        addKV(o, "flash_corrupt", String(ev.corrupt + bf.corrupt), json, false);
        addKV(o, "flash_max_erase", String(bf.max_erase > ev.max_erase ? bf.max_erase : ev.max_erase), json, false);
    }
#else
    addKV(o, "storage", "sd", json, true);
#endif
    addKV(o, "sd_ok", String(sdOk ? 1 : 0), json, false);
    addKV(o, "sd_buffered", String(bufferedRecords), json, false);
    addKV(o, "sd_dropped", String(droppedRecords), json, false);
    addKV(o, "event_state", eventStateName(eventState), json, true);
    addKV(o, "event_id", eventId, json, true);
    addKV(o, "thermal_derate", String(thermalDerate ? 1 : 0), json, false);
    addKV(o, "selftest_fail_mask", String(selfTestMask), json, false);
    addKV(o, "ota", otaInProgress ? String("running") : otaResult, json, true);
    addKV(o, "boot_stage", bootStageName, json, true);
    addKV(o, "sim_offline", String(simOffline() ? 1 : 0), json, false);
    addKV(o, "server_link", String(serverLinkUp ? 1 : 0), json, false);
    addKV(o, "sample_rate_hz", fnum(sampleRateHz, 1), json, false);
    addKV(o, "up_cycle_ms", String(upCycleMs), json, false);
    addKV(o, "flash_mb", String(ezhal_flash_size() / 1048576u), json, false);
    addKV(o, "fs_ok", String(fsAvailable ? 1 : 0), json, false);
    if (fsAvailable) {
        uint32_t fsUsed = 0, fsTotal = 0;
        ezhal_fs_usage(&fsUsed, &fsTotal);
        addKV(o, "fs_used_kb", String(fsUsed / 1024u), json, false);
        addKV(o, "fs_total_kb", String(fsTotal / 1024u), json, false);
        addKV(o, "fs_img", String(fsDirStats(FS_IMG_DIR, ".jpg").files), json, false);
        addKV(o, "fs_pend_img", String(fsDirStats(FS_PEND_DIR, ".jpg").files), json, false);
        addKV(o, "fs_buf_pending", String(fsBufPending), json, false);
        addKV(o, "fs_buf_dropped", String(fsBufDropped), json, false);
        addKV(o, "fs_aud", String(fsDirStats(FS_AUD_DIR, ".wav").files), json, false);
        addKV(o, "fs_aud_pend", String(fsDirStats(FS_AUD_PEND_DIR, ".wav").files), json, false);
        addKV(o, "clips_saved", String(clipsSaved), json, false);
        addKV(o, "clips_skipped", String(clipsSkipped), json, false);
    }
    addKV(o, "cam_timeouts", String(ezhal_cam_timeouts()), json, false);
#ifdef Arduino_OTA_MODE
    addKV(o, "ota_capable", "1", json, false);
#else
    addKV(o, "ota_capable", "0", json, false);
#endif
    addKV(o, "valid", String(validMask()), json, false);
    if (json) o += "}";
    return o;
}

void thread_heartbeat_task(const void *argument)
{
    uint32_t lastHb = 0;
    uint32_t lastRegTry = 0;
    while (1) {
        beat(T_HB);
        otaWatch();
        rebootCheck();
        // Not registered yet (e.g. Edge Server started after the node): retry every 10 s, not every heartbeat
        if (!deviceRegistered && ezhal_radio_connected() && millis() - lastRegTry >= 10000) {
            lastRegTry = millis();
            addDevice(1);
        }
        if (lastHb == 0 && millis() - bootMs < 20000) {     // first heartbeat after sensors have real data
            delay(1000);
            continue;
        }
        if (millis() - lastHb >= cfg.heartbeatIntervalS * 1000UL || lastHb == 0) {
            lastHb = millis();
            if (ezhal_radio_connected()) {
                syncTime(false);
                if (submitData("/node-status?" + buildStatus(false)) == 200) {
                    lastServerOkMs = millis();
                }
            }
        }
        delay(1000);
    }
}

// ====================================================================================
// Self-test (FW-22) and calibration (FW-19)
// ====================================================================================
String runSelfTest()
{
    String r = "";
    uint32_t mask = 0;
    float t = 0, rh = 0;
    bool aht = ezhal_env_read(&t, &rh) == HS_OK;
    r += String("AHT20:") + (aht ? "PASS" : "FAIL") + "\n";
    if (!aht) mask |= 1 << 0;
    int raw = -1;
    bool gas = ezhal_gas_read_raw(&raw) == HS_OK && raw > 3 && raw < HAL_GAS_ADC_MAX - 3;
    r += "GAS_ADC:" + String(gas ? "PASS" : "FAIL") + " raw=" + String(raw) + "\n";
    if (!gas) mask |= 1 << 1;
#ifdef ACOUSTIC_DETECTION
    uint32_t f0 = acFrames;
    delay(1500);
    bool mic = ezhal_mic_present() && acFrames != f0;
    r += String("MIC:") + (mic ? "PASS" : "FAIL") + "\n";
    if (!mic) mask |= 1 << 2;
#endif
    lockM(camMutex);
    bool cam = grabImage();
    cameraIdle();
    unlockM(camMutex);
    r += String("CAMERA:") + (cam ? "PASS" : "FAIL") + " bytes=" + String(img_len) +
         (cam ? "" : String(" (no frame within ") + String(HAL_CAM_SNAPSHOT_TIMEOUT_MS / 1000) +
                     " s: check the camera module and Tools > Camera Options)") + "\n";
    if (!cam) mask |= 1 << 3;
#ifdef ENABLE_SD_STORAGE
    bool sd = sdOk;
    if (sd) {
        const char *probe = "ezsn-selftest";
        sd = sdWriteFile("ezsn/selftest.txt", (const uint8_t *)probe, strlen(probe));
    }
    r += String("SD:") + (sd ? "PASS" : "FAIL") + "\n";
    if (!sd) mask |= 1 << 4;
#endif
#ifdef DATA_STORE_FLASH
    String fdet;
    lockM(flashMutex);
    bool fl = ezhal_flash_selftest(&fdet) == HS_OK;
    unlockM(flashMutex);
    fl = fl && storeOk;
    r += String("FLASH_STORE:") + (fl ? "PASS " : "FAIL ") + fdet + "\n";
    if (!fl) mask |= 1 << 4;
#endif
    NodeConfig chk;
    lockM(flashMutex);
    HalStatus cst = ezhal_frec_read(EZHAL_FLASH_CFG_A, EZHAL_FLASH_CFG_B, &chk, sizeof(chk), NULL);
    unlockM(flashMutex);
    bool cfgOk = cst == HS_OK && chk.magic == CONFIG_MAGIC && chk.crc == crc32Calc((uint8_t *)&chk, sizeof(chk) - sizeof(uint32_t));
    r += String("FLASH_CFG:") + (cfgOk ? "PASS" : "FAIL") + "\n";
    if (!cfgOk) mask |= 1 << 5;
    bool wifi = ezhal_radio_connected();
    r += String("WIFI:") + (wifi ? "PASS" : "FAIL") + " rssi=" + String(ezhal_radio_rssi()) + "\n";
    if (!wifi) mask |= 1 << 6;
    String sline;
    if (!samplingOk(&sline)) mask |= 1 << 7;
    r += sline + "\n";
    float usl = -1;
    HalPowerStatus pws;
    HalTamperStatus tps;
    r += String("ULTRASONIC:") + (ezhal_us_present() ? (ezhal_us_read(&usl) == HS_OK ? "PASS" : "FAIL") : "N/A (OP-01)") + "\n";
    r += String("POWER:") + (ezhal_power_present() ? (ezhal_power_read(&pws) == HS_OK ? "PASS" : "FAIL") : "N/A (OP-04)") + "\n";
    r += String("TAMPER:") + (ezhal_tamper_present() ? (ezhal_tamper_read(&tps) == HS_OK ? "PASS" : "FAIL") : "N/A (OP-08)") + "\n";
    selfTestMask = mask;
    selfTestReport = r;
    r += mask == 0 ? "RESULT:PASS" : "RESULT:FAIL mask=" + String(mask);
    LOG("Self-test:\n" + r);
    return r;
}

String calibrateGas()
{
    if ((millis() - bootMs) / 1000 < cfg.gasWarmupS) {
        return "ERR gas sensor warming up (" + String(cfg.gasWarmupS - (millis() - bootMs) / 1000) + " s left)";
    }
    float sum = 0;
    int n = 0;
    for (int i = 0; i < 50; i++) {
        if (gasRs > 0) {
            sum += gasRs;
            n++;
        }
        delay(100);
    }
    if (n < 10 || cfg.gasCleanAirRatio <= 0) return "ERR gas calibration failed";
    cfg.gasR0 = (sum / n) / cfg.gasCleanAirRatio;
    bool ok = saveConfig();
    return "OK gas_r0=" + fnum(cfg.gasR0, 3) + (ok ? " saved" : " SAVE FAILED");
}

// ====================================================================================
// SD-card OTA guards (FW-17)
// ====================================================================================
// Returns "" when an update may start, otherwise the refusal text. *size receives the file size.
String otaPrecheck(bool force, int32_t *size)
{
    *size = -1;
#ifndef Arduino_OTA_MODE
    // A normal build's partition table has only a 64 KB second firmware slot: an SD update cannot fit.
    (void)force;
    return "ERR firmware built without OTA Mode: build with Tools > OTA Mode = Enable and NN Model Load From = SD Card, flash it over USB once, then OTA_SD works";
#else
    if (!sdOk) return "ERR SD not available";
    if (otaInProgress) return "ERR OTA already in progress";
    lockM(sdMutex);
    *size = ezhal_sd_size(OTA_FILE);
    unlockM(sdMutex);
    if (*size < 0) return "ERR ota.bin not found on SD";
    if ((uint32_t)*size > OTA_SLOT_BYTES) {
        return "ERR ota.bin is " + String(*size) + " bytes, larger than the 4 MB firmware slot (" + String(OTA_SLOT_BYTES) + ")";
    }
    if ((uint32_t)*size < OTA_MIN_BYTES) return "ERR ota.bin is only " + String(*size) + " bytes: not a firmware image";
    if (!force && eventState != EVT_NORMAL) {
        return "ERR leak event " + eventStateName(eventState) + " active: retry when NORMAL, or OTA_SD FORCE";
    }
    return "";
#endif
}

// Called once at boot (after the SD card is mounted): did the last OTA succeed?
void otaCheckOutcome()
{
    if (lastResetReason != RR_OTA) return;
    String prev = lastResetDetail.startsWith("sd:") ? lastResetDetail.substring(3) : String("");
    if (prev.length() > 0 && prev != FW_VERSION) {
        otaResult = "ok: " + prev + " -> " + FW_VERSION;
        // Rename the image so a repeated OTA_SD cannot flash the same file again
        if (sdOk) {
            String done = String("ota_") + FW_VERSION + ".bin";
            lockM(sdMutex);
            if (ezhal_sd_exists(OTA_FILE)) {
                if (ezhal_sd_exists(done)) ezhal_sd_remove(done);
                if (ezhal_sd_rename(OTA_FILE, done) == HS_OK) {
                    otaResult += " (ota.bin renamed to " + done + ")";
                }
            }
            unlockM(sdMutex);
        }
    } else {
        // Reset during an OTA but the version did not change: update failed / power lost, or same image
        otaResult = "unchanged: still " + String(FW_VERSION) + " after the OTA reset (update failed or same version)";
    }
    LOG("OTA result: " + otaResult);
}

// Called every second by the heartbeat thread: a successful update always reboots, so an update that is
// still "running" after OTA_TIMEOUT_MS has failed. Give the SD card back and clear the pending reason.
void otaWatch()
{
    if (otaInProgress && millis() - otaStartMs > OTA_TIMEOUT_MS) {
        otaInProgress = false;
        otaResult = "failed: no reboot within " + String(OTA_TIMEOUT_MS / 60000) + " min";
        recordResetReason(RR_UNEXPECTED, "");
        LOG("OTA " + otaResult + "; SD access resumed, still running " + String(FW_VERSION));
    }
}

// ====================================================================================
// Command interpreter shared by socket, HTTP /cmd and serial CLI (FW-10, FW-19, FW-22)
// ====================================================================================
String executeCommand(String line)
{
    line.trim();
    int sp = line.indexOf(' ');
    String cmd = sp < 0 ? line : line.substring(0, sp);
    String arg = sp < 0 ? "" : line.substring(sp + 1);
    arg.trim();
    cmd.toUpperCase();
    if (!bootDone) {
        // during boot only commands that do not need the network, sensors or threads
        static const char *early[] = {"PING", "HELP", "STATUS", "CONFIG", "SET", "SAVE", "RESET_CONFIG", "LOG",
                                      "FLASH", "FLASH_TEST", "FLASH_FORMAT", "FLASH_PROBE", "REBOOT",
                                      "FS_DF", "FS_LS", "FS_CAT", "FS_RM", "FS_TEST", "FS_FORMAT"};
        bool allowed = false;
        for (unsigned i = 0; i < sizeof(early) / sizeof(early[0]); i++) {
            if (cmd == early[i]) allowed = true;
        }
        if (!allowed) {
            return String("ERR still booting (stage ") + bootStageName +
                   "); available now: PING HELP STATUS CONFIG SET SAVE RESET_CONFIG LOG FLASH FLASH_TEST FLASH_FORMAT FLASH_PROBE REBOOT FS_*";
        }
    }

    if (cmd == "HELP") {
        return "PING STATUS CONFIG SET <k> <v> SAVE RESET_CONFIG CAPTURE CAL_GAS CAL_AC SELFTEST MAINT <0|1> "
               "POWER <0|1> SIM <GAS|AC|OFFLINE|CLEAR> [s] TIME <epoch> LOG [n] FLASH FLASH_TEST FLASH_FORMAT <EVENTS|BUFFER> "
               "FLASH_PROBE [WRITE] FS_DF FS_LS [dir] FS_CAT <file> [offset] FS_RM <path> FS_TEST FS_FORMAT YES "
               "OTA_SD [FORCE] REBOOT";
    }
    if (cmd == "PING") return "PONG " + String(cfg.deviceName) + " " + String(FW_VERSION);
    if (cmd == "STATUS") return buildStatus(true);
    if (cmd == "CONFIG") return configDump(false);
    if (cmd == "SET") {
        int s2 = arg.indexOf(' ');
        if (s2 < 0) s2 = arg.indexOf('=');
        if (s2 < 0) return "ERR usage: SET <key> <value>";
        String k = arg.substring(0, s2);
        String v = arg.substring(s2 + 1);
        v.trim();
        if (!setCfgItem(k, v)) return "ERR unknown key or bad value: " + k;
        return "OK " + k + " (use SAVE to persist; network keys need REBOOT)";
    }
    if (cmd == "SAVE") return saveConfig() ? "OK saved" : "ERR flash write failed";
    if (cmd == "RESET_CONFIG") {
        setDefaultConfig();
        return saveConfig() ? "OK defaults saved, REBOOT to apply network settings" : "ERR flash write failed";
    }
    if (cmd == "CAPTURE") {
        eventCounter++;
        captureEventId = "M" + String(bootInfo.bootCount) + "_" + String(eventCounter);
        bCapturePicture = true;
        return "OK capture " + captureEventId;
    }
    if (cmd == "CAL_GAS") return calibrateGas();
    if (cmd == "CAL_AC") {
        acFloorDb = acHfDb;
        acFloorInit = true;
        acAlarm = false;
        acAboveCnt = 0;
        return "OK acoustic floor=" + fnum(acFloorDb, 1) + " dB";
    }
    if (cmd == "SELFTEST") return runSelfTest();
    if (cmd == "MAINT") {
        cfg.maintenance = arg.toInt() ? 1 : 0;
        saveConfig();
        return String("OK maintenance ") + (cfg.maintenance ? "ON" : "OFF");
    }
    if (cmd == "POWER") {
        cfg.powerMode = arg.toInt() ? 1 : 0;
        saveConfig();
        if (cfg.powerMode == 0) {
            lockM(camMutex);
            cameraOn();
            unlockM(camMutex);
        } else {
            lockM(camMutex);
            cameraIdle();
            unlockM(camMutex);
        }
        return String("OK power ") + (cfg.powerMode ? "ECO" : "ACTIVE");
    }
    if (cmd == "SIM") {
        String what = arg;
        int s2 = arg.indexOf(' ');
        uint32_t secs = 60;
        if (s2 > 0) {
            what = arg.substring(0, s2);
            secs = arg.substring(s2 + 1).toInt();
        }
        what.toUpperCase();
        if (what == "GAS") simGasUntil = millis() + secs * 1000;
        else if (what == "AC") simAcUntil = millis() + secs * 1000;
        else if (what == "OFFLINE") {
            if (secs > 3600) secs = 3600;                    // a test outage is never longer than an hour
            simOfflineUntil = millis() + secs * 1000;
            if (simOfflineUntil == 0) simOfflineUntil = 1;
        }
        else if (what == "CLEAR") simGasUntil = simAcUntil = simOfflineUntil = 0;
        else return "ERR SIM <GAS|AC|OFFLINE|CLEAR> [seconds]";
        return "OK sim " + what + " " + String(secs) + " s";
    }
    if (cmd == "TIME") {
        uint32_t e = (uint32_t)arg.toInt();
        if (e < 1600000000UL) return "ERR bad epoch";
        setClock(e);
        return "OK time " + String(e);
    }
    if (cmd == "LOG") {
        int n = arg.toInt();
        return readEventLogTail(n > 0 ? n : 20);
    }
    if (cmd.startsWith("FS_")) {
        if (cmd == "FS_FORMAT") {
            if (arg != "YES") return "ERR FS_FORMAT YES erases every file";
            if (ezhal_flash_size() < 0x2000000u) return "ERR no file system on this board (flash is not 32 MB)";
            HalStatus st = ezhal_fs_format();
            fsAvailable = st == HS_OK;
            fsDetail = fsAvailable ? "formatted" : "format failed";
            return fsAvailable ? "OK formatted" : "ERR format failed";
        }
        if (!fsAvailable) return "ERR file system not available: " + fsDetail;
        if (cmd == "FS_DF") {
            uint32_t used = 0, total = 0;
            ezhal_fs_usage(&used, &total);
            return "OK used " + String(used / 1024u) + " KB of " + String(total / 1024u) + " KB (" +
                   String(total ? (uint32_t)((uint64_t)used * 100 / total) : 0) + "%)";
        }
        if (cmd == "FS_LS") {
            String out;
            String dir = arg.length() ? arg : String("/");
            if (ezhal_fs_list(dir.c_str(), &out) != HS_OK) return "ERR no such directory: " + dir;
            return out.length() ? out : String("(empty)");
        }
        if (cmd == "FS_CAT") {
            int sp2 = arg.indexOf(' ');
            String path = sp2 < 0 ? arg : arg.substring(0, sp2);
            uint32_t off = sp2 < 0 ? 0 : (uint32_t)arg.substring(sp2 + 1).toInt();
            int32_t size = ezhal_fs_size(path.c_str());
            if (size < 0) return "ERR no such file: " + path;
            static uint8_t cbuf[513];
            int32_t n = ezhal_fs_read(path.c_str(), off, cbuf, 512);
            if (n < 0) return "ERR read failed";
            for (int32_t i = 0; i < n; i++) {
                if ((cbuf[i] < 32 || cbuf[i] > 126) && cbuf[i] != '\n') cbuf[i] = '.';   // binary bytes shown as '.'
            }
            cbuf[n] = 0;
            return "OK " + path + " size=" + String(size) + " offset=" + String(off) + " shown=" + String(n) + "\n" +
                   String((const char *)cbuf);
        }
        if (cmd == "FS_RM") {
            return ezhal_fs_remove(arg.c_str()) == HS_OK ? "OK removed " + arg : "ERR cannot remove " + arg + " (missing or non-empty dir)";
        }
        if (cmd == "FS_TEST") {
            // 64 KB round trip in a test file: write (atomic), append, read back, verify, remove
            static uint8_t tbuf[4096];
            const char *tp = "/test/fs_test.bin";
            uint32_t seed = millis();
            for (int i = 0; i < 4096; i++) tbuf[i] = (uint8_t)(i * 31 + seed);
            uint32_t t0 = millis();
            bool ok = ezhal_fs_write_file(tp, tbuf, 4096) == HS_OK;
            for (int k = 1; k < 16 && ok; k++) {
                tbuf[0] = (uint8_t)k;
                ok = ezhal_fs_append(tp, tbuf, 4096) == HS_OK;
            }
            uint32_t tw = millis() - t0;
            ok = ok && ezhal_fs_size(tp) == 65536;
            static uint8_t rbuf2[4096];
            t0 = millis();
            for (int k = 0; k < 16 && ok; k++) {
                ok = ezhal_fs_read(tp, k * 4096u, rbuf2, 4096) == 4096;
                tbuf[0] = (uint8_t)(k == 0 ? (uint8_t)(seed) : k);
                ok = ok && memcmp(tbuf, rbuf2, 4096) == 0;
            }
            uint32_t tr = millis() - t0;
            ezhal_fs_remove(tp);
            ezhal_fs_remove("/test");
            return String(ok ? "OK" : "ERR") + " 64 KB written in " + String(tw) + " ms, read+verified in " + String(tr) + " ms";
        }
        return "ERR unknown FS command (FS_DF FS_LS FS_CAT FS_RM FS_TEST FS_FORMAT)";
    }
    if (cmd == "FLASH_PROBE") {
        // bench diagnostic for >16 MB flash; all our flash users are blocked while it runs
        String opt = arg;
        opt.toUpperCase();
        String rep;
        lockM(sdMutex);
        lockM(flashMutex);
        HalStatus st = ezhal_flash_probe(opt == "WRITE", &rep);
        unlockM(flashMutex);
        unlockM(sdMutex);
        return String(st == HS_OK ? "OK\n" : "ERR\n") + rep;
    }
    if (cmd == "FLASH" || cmd == "FLASH_TEST" || cmd == "FLASH_FORMAT") {
#ifdef DATA_STORE_FLASH
        if (cmd == "FLASH_TEST") {
            String d;
            lockM(flashMutex);
            HalStatus st = ezhal_flash_selftest(&d);
            unlockM(flashMutex);
            return String(st == HS_OK ? "OK " : "ERR ") + d;
        }
        if (cmd == "FLASH_FORMAT") {
            String which = arg;
            which.toUpperCase();
            int log = which == "EVENTS" ? EZFLOG_EVENTS : (which == "BUFFER" ? EZFLOG_BUFFER : -1);
            if (log < 0) return "ERR usage: FLASH_FORMAT <EVENTS|BUFFER>";
            lockM(sdMutex);
            HalStatus st = ezhal_flog_format(log);
            bufferedRecords = ezhal_flog_pending(EZFLOG_BUFFER);
            unlockM(sdMutex);
            return st == HS_OK ? "OK formatted " + which : String("ERR format failed");
        }
        String out = "";
        const char *names[EZFLOG_COUNT] = {"events", "buffer"};
        lockM(sdMutex);
        for (int i = 0; i < EZFLOG_COUNT; i++) {
            EzFlashLogStats st;
            ezhal_flog_stats(i, &st);
            out += String(names[i]) + ": sectors=" + String(st.sectors) + " capacity=" + String(st.capacity) +
                   " used=" + String(st.used) + " records=" + String(st.records) + " pending=" + String(st.pending) +
                   " dropped=" + String(st.dropped) + " corrupt=" + String(st.corrupt) + " max_erase=" + String(st.max_erase) + "\n";
        }
        unlockM(sdMutex);
        return out + "thinned=" + String(thinnedRecords) + " store_ok=" + String(storeOk ? 1 : 0);
#else
        return "ERR flash data store not enabled (DATA_STORE_FLASH)";
#endif
    }
    if (cmd == "OTA_SD") {
        String opt = arg;
        opt.toUpperCase();
        int32_t size = -1;
        String refusal = otaPrecheck(opt == "FORCE", &size);
        if (refusal.length() > 0) return refusal;
        // Remember the running version: after the reboot, otaCheckOutcome() compares it with the new one
        recordResetReason(RR_OTA, String("sd:") + FW_VERSION);
        otaInProgress = true;                  // pause application SD access before the SDK task opens the card
        otaStartMs = millis();
        otaResult = "running";
        lockM(sdMutex);                        // wait until any application SD operation has finished
        HalStatus st = ezhal_sd_start_ota(OTA_FILE);
        unlockM(sdMutex);
        if (st != HS_OK) {
            otaInProgress = false;
            otaResult = "failed: could not start";
            recordResetReason(RR_UNEXPECTED, "");
            return "ERR could not start OTA";
        }
        LOG("OTA started: ota.bin " + String(size) + " bytes, running " + String(FW_VERSION));
        return "OK OTA started from SD (" + String(size) + " bytes, running " + String(FW_VERSION) +
               "); node reboots when done, then reports ota=ok: " + String(FW_VERSION) + " -> <new>";
    }
    if (cmd == "REBOOT") {
        scheduleReboot(RR_CMD_REBOOT, "cmd", 1000);
        return "OK rebooting in 1 s";
    }
    return "ERR unknown command (HELP)";
}

void thread_cli_task(const void *argument)
{
    String line = "";
    while (1) {
        beat(T_CLI);
        rebootCheck();                     // REBOOT also works during boot, before the heartbeat thread runs
        while (Serial.available() > 0) {
            char c = (char)Serial.read();
            if (c == '\n' || c == '\r') {
                line.trim();
                if (line.length() > 0) {
                    if (!cliLoggedIn) {
                        if (line == "LOGIN " + String(cfg.apiToken)) {
                            cliLoggedIn = true;
                            Serial.println("OK logged in");
                        } else {
                            Serial.println("ERR LOGIN <token> required");
                        }
                    } else {
                        Serial.println(executeCommand(line));
                    }
                }
                line = "";
            } else if (line.length() < 200) {
                line += c;
            }
        }
        delay(100);
    }
}
