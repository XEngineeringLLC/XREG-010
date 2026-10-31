// XREG-010
// Copyright (C) 2026 X Engineering LLC
// Contact: joe@xengineering.net

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 3 of the License.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

// Must be strict semver ^\d+\.\d+\.\d+$ — no suffixes, no leading v. Compared as an integer
// major*10000 + minor*100 + patch, so the ceiling is 999.99.99 (minor/patch cannot exceed 99).
const char *FIRMWARE_VERSION = "0.0.10";

// Stable proxy domain in front of the Supabase "ota" bucket, so hosting can move without a re-flash.
// Download uses setInsecure; integrity is the on-device RSA-4096 signature check (performOTAUpdate).
const char *OTA_BASE_URL = "https://ota.xengineering.net";
#include <OneWire.h>            // temp sensors
#include <DallasTemperature.h>  // temp sensors
//#include <SPI.h>                // display- removed to save connectors space
//#include <U8g2lib.h>            // display
#include <ADS1115_lite.h>  // measuring 4 analog inputs
ADS1115_lite adc(ADS1115_DEFAULT_ADDRESS);
#include <VeDirectFrameHandler_xeng.h>  // for victron communication — my fork: frameCounter new-frame gating + corrupted-stream bounds fixes (see its header)
#include "INA228.h"
INA228 INA(0x40);
//DONT MOVE THE NEXT 6 LINES AROUND, MUST STAY IN THIS ORDER
#define ESP32_CAN_RX_PIN GPIO_NUM_16                           //
#define ESP32_CAN_TX_PIN GPIO_NUM_17                           //
#include <NMEA2000_esp32_xeng.h>                               // _xeng fork of NMEA2000_twai: transmit never blocks (stock waits up to 100ms/frame on fast-packet frames when the TX queue is full, e.g. no bus)
tNMEA2000_esp32 NMEA2000(ESP32_CAN_TX_PIN, ESP32_CAN_RX_PIN);  // necessary for ESP32-S3 library
#include <N2kMessages.h>
#include <N2kMessagesEnumToStr.h>  // questionably needed
#include <WiFi.h>
#include <AsyncTCP.h>                    // for wifi stuff, IMPORTANT, don't ever update, use mathieucarbou github repository
#include <LittleFS.h>                    //
fs::LittleFSFS webFS;                    // Separate filesystem for web files
bool usingFactoryWebFiles = false;       // Track which web partition is mounted
#include <ESPAsyncWebServer_xeng.h>      // for wifi stuff, important, don't ever update, use mathieucarbou github repository — my fork of 3.11.0: queued SSE messages live in PSRAM, never internal RAM (see its AsyncEventSource.h)
#include <DNSServer.h>                   // For captive portal (Wifi Network Provisioning) functionality
#include <ESPmDNS.h>                     // helps with wifi provisioning (to save users trouble of looking up ESP32's IP address)
#include "esp_heap_caps.h"               // needed for tracking heap usage
#include "freertos/FreeRTOS.h"           // for stack usage
#include "freertos/task.h"               // for stack usage
#define configGENERATE_RUN_TIME_STATS 1  // for CPU use tracking
#include <mbedtls/md.h>                  // security
#include <mbedtls/sha256.h>              // settings-password hash (pwStore/pwVerify, 3_functions.ino)
#include <vector>                        // Console message queue system
#include <String>                        // Console message queue system
#include <memory>                        // shared_ptr lifetime for streamed binary endpoint buffers (/fastscope.bin)
#include "esp_task_wdt.h"                //Watch dog to prevent hung up code from wreaking havoc
#include "esp_log.h"                     // get rid of spam in serial monitor
#include <TinyGPSPlus.h>                 // for NMEA0183, not currently implemented
#include <math.h>                        // For pow() function needed by Peukert calculation
#include "nvs_flash.h"                   //for persistent storage that's better than Flash
#include "nvs.h"                         //for persistent storage that's better than Flash
#include <HTTPClient.h>                  // for weather reports
#include <ArduinoJson.h>                 // for weather reports (and secure OTA?)
#include <WiFiClientSecure.h>            // for weather reports
#include <PID_v1_xeng.h>                 // My fork to include tracking anti-windup methods       Thanks Brett!
#include <esp_ota_ops.h>                 // secure OTA
#include <mbedtls/pk.h>                  // secure OTA
#include <mbedtls/base64.h>              // secure OTA
#include <mbedtls/platform.h>            // mbedtls_platform_set_calloc_free — route TLS buffers to PSRAM
#include "esp_system.h"                  // secure OTA
#include <BMP388_DEV.h>                  //non blocking capability - don't upgrade, this was customized for better error handling by xengineering
#include "esp_partition.h"               // for esp_partition_find_first
#include "esp_image_format.h"            // esp_image_verify: whole-image digest for the ota_0 verified-install record
#include "esp_heap_caps.h"
#include <time.h>            // Supabase
#include "esp_sntp.h"        // sntp_get_sync_status — proof a real NTP packet landed, see syncTimeFromNTP
#include "esp_psram.h"       // for ESP32 health calculations
#include "LSM6DSOXSensor_xeng.h"  //accelerometer — my fork: the I2C transport reports bus failures (stock always returned OK, so every LSM6DSOX_OK test below was vacuous)
#include "esp_adc/adc_continuous.h"   // fast alternator-current channel (GPIO3/ADC1_CH2) — hardware-timed DMA sampling
#include "esp_adc/adc_cali.h"         // eFuse factory gain correction for the fast channel (nominal scaling, NOT user calibration)
#include "esp_adc/adc_cali_scheme.h"


// rom/rtc.h can't be included here (its SLEEP_MODE enum collides with BMP388_DEV.h), so declare
// the ROM function directly; it returns the RESET_REASON enum, ABI-compatible with int.
extern "C" int rtc_get_reset_reason(int cpu_no);

// Cold globals in PSRAM: `T &name = PsramPlace::make<T>();` keeps every use site unchanged. Safe at static init: PSRAM
// joins the heap in the IDF CORE init stage, before global constructors run. Zero-filled like .bss, then default-
// constructed; falls back to internal RAM only when PSRAM is missing (counted, printed at boot). Not for anything
// touched every control pass (claude_docs/firmware-internals.md, PSRAM move candidates).
#include <new>
#include <type_traits>
struct PsramPlace {
  static inline uint32_t psramBytes = 0;
  static inline uint16_t internalFallbacks = 0;
  template <typename T> static T &make() {
    constexpr size_t al = alignof(T) < 4 ? 4 : alignof(T);
    void *p = heap_caps_aligned_calloc(al, 1, sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) {
      psramBytes += sizeof(T);
    } else {
      p = heap_caps_aligned_calloc(al, 1, sizeof(T), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      internalFallbacks++;
    }
    using E = std::remove_all_extents_t<T>;
    if constexpr (!std::is_trivially_default_constructible_v<E>) {
      for (size_t i = 0; i < sizeof(T) / sizeof(E); i++) new (static_cast<E *>(p) + i) E();
    }
    return *static_cast<T *>(p);
  }
  // Plain malloc()/new go to PSRAM first while open; explicit-caps allocations (FreeRTOS objects, task stacks, drivers)
  // are unaffected. Every task's plain malloc moves meanwhile, so open it only around one-time setup code.
  static void mallocWindow(bool open) {
    heap_caps_malloc_extmem_enable(open ? 0 : CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
  }
};

// FrontStore::classify confidence states (engine in 7_functions.ino); numeric order mirrored in script.js
// labels. Defined here because globals below initialize from them.
#define FRONT_MEASURED      0
#define FRONT_ESTIMATED     1
#define FRONT_LEARNING_EDGE 2
#define FRONT_NO_REFERENCE  3

// Make the types visible to auto-generated prototypes, hack
struct UpdateInfo;
struct StreamingExtractor;
struct HttpsRequest;
struct SensorSnapshot;  // full definition below (PSRAM sensor ring)
struct SensorChan;      // its per-channel cell; snapChan() (2_functions.ino) returns one by value, so its auto-prototype needs the name
struct IgnWatermark;    // forward-declared so wmIgnUpdate's auto-prototype compiles
struct FaDetectResult;  // fast alt-current channel detector contract (full definition in 2_functions.ino)
struct FadJob;          // pulse-pattern detector (defined in 2_functions.ino, bodies in 8_functions.ino),
struct FadResult;       // forward-declared for their auto-prototypes
struct ZFitResult;      // zeroFitRegress() return type (full def near ZeroFitRecord) — same reason
struct SolarLedgerLive; // the sled*() hour helpers (4_functions.ino) take it by reference (full def near sledRing) — same reason
struct RipFit;          // forward-declared for the ripFitEncode/ripFitDecode auto-prototypes
struct LongTermRecord;  // buildLongTermRecord() takes it by reference (full def near longTermRing)
// Defined here so the cxStartPersistCancel() auto-prototype (2_functions.ino) can't precede the type.
enum CxStartCancel : uint8_t { CX_CANCEL_NONE = 0, CX_CANCEL_FRESH = 1, CX_CANCEL_RESUME = 2 };
// Auto-prototype generator fails on default-argument functions defined in later .ino files.
bool fieldCutSettled(uint32_t extraMs = 0);

// ===== DEMO MODE SEAL (9_demo.ino) — must stay below every #include and the forward declarations above =====
// While demoSeal is up nothing in the sketch can change NVS or the userdata filesystem: every NVS read-write open is
// downgraded to read-only (nvs_set/commit then fail), and every mutating LittleFS call is refused. Library code is
// untouched. Settings writes go to a RAM overlay instead (settingWrite, 2_functions.ino).
#define DEMO_OFF 0
#define DEMO_ENTERING 1
#define DEMO_ON 2
#define DEMO_EXITING 3
volatile bool demoSeal = false;
volatile uint8_t demoPhase = DEMO_OFF;
const char *DEMO_REFUSAL = "Not available in demo mode. Set Hardware Present to Yes to use it.";
esp_err_t demoNvsOpen(const char *ns, nvs_open_mode_t mode, nvs_handle_t *h) {
  return (nvs_open)(ns, (demoSeal && mode == NVS_READWRITE) ? NVS_READONLY : mode, h);
}
esp_err_t demoNvsFlashErase() { return demoSeal ? ESP_ERR_NOT_ALLOWED : (nvs_flash_erase)(); }
#define nvs_open(ns, mode, h) demoNvsOpen((ns), (mode), (h))
#define nvs_flash_erase() demoNvsFlashErase()
static fs::LittleFSFS &realLittleFS = LittleFS;
class DemoSealFS {
 public:
  bool begin(bool formatOnFail = false, const char *basePath = "/littlefs", uint8_t maxOpenFiles = 10, const char *label = "spiffs") {
    return realLittleFS.begin(formatOnFail, basePath, maxOpenFiles, label);
  }
  void end() { realLittleFS.end(); }
  bool format() { return demoSeal ? false : realLittleFS.format(); }
  size_t totalBytes() { return realLittleFS.totalBytes(); }
  size_t usedBytes() { return realLittleFS.usedBytes(); }
  File open(const char *p, const char *m = FILE_READ, const bool create = false) {
    if (demoSeal && (create || !m || m[0] != 'r' || strchr(m, '+'))) return File();
    return realLittleFS.open(p, m, create);
  }
  File open(const String &p, const char *m = FILE_READ, const bool create = false) { return open(p.c_str(), m, create); }
  bool exists(const char *p) { return realLittleFS.exists(p); }
  bool exists(const String &p) { return realLittleFS.exists(p); }
  bool remove(const char *p) { return demoSeal ? false : realLittleFS.remove(p); }
  bool remove(const String &p) { return remove(p.c_str()); }
  bool rename(const char *a, const char *b) { return demoSeal ? false : realLittleFS.rename(a, b); }
  bool rename(const String &a, const String &b) { return rename(a.c_str(), b.c_str()); }
  bool mkdir(const char *p) { return demoSeal ? false : realLittleFS.mkdir(p); }
  bool mkdir(const String &p) { return mkdir(p.c_str()); }
  bool rmdir(const char *p) { return demoSeal ? false : realLittleFS.rmdir(p); }
  bool rmdir(const String &p) { return rmdir(p.c_str()); }
  operator fs::FS &() { return realLittleFS; }   // read-side APIs (beginResponse, readFile); writeFile() checks demoSeal itself
};
DemoSealFS littleFsSeal;
#define LittleFS littleFsSeal

SET_LOOP_TASK_STACK_SIZE(20 * 1024);  // 20KB stack necessary for SSL/TLS operations
int hardwarePresent = 1;              // RAM only, boots 1, never saved or exported. 0 = demo mode: simulated boat, field drive stays real (9_demo.ino)
// Parse JSON update response - this struct definition cannot move down in file, leave it here.
struct UpdateInfo {
  bool hasUpdate;
  String version;
  String firmwareUrl;
  String signatureUrl;
  String changelog;
  size_t firmwareSize;
};

struct CachedAsset {
  uint8_t *data = nullptr;
  size_t size = 0;
  char etag[24] = {0};  // content hash of the gz bytes; lets a revalidating browser get a 304 instead of the body
};

CachedAsset loadFileToRAM(const char *path);  // defined in 7_functions.ino (explicit prototype: returns a custom struct)

CachedAsset cachedIndex, cachedCss, cachedJs, cachedUplotCss, cachedUplotJs;

// ============= HTTPS TASK SYSTEM =============
int lastHttpResponseCode = 0;  // Track last HTTP response for failure handling
volatile uint32_t lastCloudUploadOkMs = 0;  // millis() of last ack-confirmed payload upload; 0 = never. Written by Core-0 HTTPS task, read by CSV2.
QueueHandle_t httpsQueue;
TaskHandle_t httpsTaskHandle;

// Fault-detector Core-0 worker: notify faDetTaskHandle to arm; faResultSem returns the verdict.
TaskHandle_t faDetTaskHandle = NULL;
TaskHandle_t cpuLoadTaskHandle = NULL;   // low-prio Core-0 CPU-load sampler, plus the field-cut heap walks and task census kept off the Core-1 loop
SemaphoreHandle_t faResultSem = NULL;

SemaphoreHandle_t fsMutex = NULL;  // mutex protection for littlefs

enum HttpsRequestType {
  HTTPS_UPLOAD_PAYLOAD,
  HTTPS_UPLOAD_CONFIG,
  HTTPS_UPLOAD_BOATPERF,
  HTTPS_UPLOAD_ALTHEALTH,
  HTTPS_FETCH_WEATHER,
  HTTPS_UPDATE_FW_VERSION,
  HTTPS_CHECK_FORCED_UPDATE,
  HTTPS_CLEAR_FORCED_UPDATE,
  HTTPS_GET_PENDING_CONFIG,    // admin config push: fetch a queued config at boot
  HTTPS_CLEAR_PENDING_CONFIG,  // clear the queued config after applying it
  HTTPS_RESET_RPM_AXIS,        // tach rescaled: delete the device's RPM-indexed cloud data
  HTTPS_RESET_ALT_HEALTH,      // alt-health Start Over: delete the device's alt_points + alt_health cloud rows
  HTTPS_RESET_BOAT_PERF,       // boat-performance Clear All / records import: delete the device's boat_points + boat_performance rows
  HTTPS_UPLOAD_CX_LEDGER,      // commissioning-ledger event batch → log-commissioning-event
  HTTPS_UPLOAD_USAGE,          // daily app-usage analytics blob → track-behavior
  HTTPS_CLOUD_OP               // staged registration/profile op (cloudOp slot in 3_functions.ino); keeps doCloudPOST off async_tcp
};

struct HttpsRequest {
  HttpsRequestType type;
  char    *payload;     // PSRAM, OWNED: producer ps_malloc's, the httpsTask worker free's after use.
  uint32_t payloadCap;  // allocated capacity (0 / NULL payload for the no-body request types)
};

String currentUID;
#define FUNC_TIMING_WINDOW_MS 10000  // rolling window for per-function worst-case timing (ms)


// "excitation proxy" = temp-normalized field drive, NOT measured field current.

// Excitation-proxy temp normalization (copper seed; cloud refits α later).
#define ALT_ALPHA_PER_C 0.00393f
#define ALT_TREF_C      25.0f
#define ALT_MIN_BATT_V  8.0f
#define ALT_RPM_MAX     6000.0f   // axis full-scale (RPM)
#define ALT_FI_MAX      15.0f     // axis full-scale (excitation proxy)

// ===== ALTERNATOR HEALTH — Best-Ever Front =====
// Sparse best-ever output-amps surface over {RPM, excitation, Vbus, temp}; the device learns + evaluates it
// (engine instance in 7_functions.ino), the cloud prunes + retains raw history.

// Performance-vs-engine-hours TREND ring (the headline). One point per engine-hour bucket:
// average + worst output-% vs the best-ever front. Survives reboot; uploads for cloud history.
#define ALT_TREND_CAP 8760     // 1-yr rolling window of runtime (~52 KB PSRAM); display only, cloud keeps raw points
#define ALT_TREND_DROP 720     // buckets evicted per rollover (~30 days), so /alttrend.bin is rewritten ~monthly, not hourly
struct AltTrendPt {
  uint16_t engHour;            // engine-hours-since-baseline bucket index
  int16_t  worstPct;          // low-decile (P10) output-% in this bucket (×10), not raw min
  int16_t  overallPct;        // average output-% in this bucket (×10)
};
static AltTrendPt *altTrend = nullptr;
static int         altTrendCount = 0;
static uint32_t    altTrendFlushed = 0;     // trend records already in the /alttrend.bin append log
static bool        altTrendRewrite = true;  // force a full log rewrite (load-miss or ring eviction)
// Set by /debug/fillmax (no ?persist=yes) and /debug/clearmax: RAM rings are synthetic, so field-off
// persisters skip flash and keep the real learned blobs. RAM-only; cleared only by reboot.
volatile bool dbgRingsSynthetic = false;
// Current-bucket accumulators (committed when the engine-hour bucket advances).
static double altBucket_sum = 0, altBucket_n = 0;   // running average over the bucket
static float  altBucket_worst = 0;                  // raw min output-% this bucket (debug CSV only; the committed low line is P10)
static uint16_t (&altBucketHist)[60] = PsramPlace::make<uint16_t[60]>();             // bucket %-histogram (2% bins, 0..120) → committed P10
static int    altCurEngHour = -1;
double altTrendBaselineSec = 0.0;   // EngineRunTime_AllTime at last "Start Over" (trend X-axis origin)

// Live point for the dashboard gauge / trend dot, via AltLive SSE. The ~75 Hz fold writes only the
// EMA-filtered inputs; prediction + state + % are computed at 1 Hz in altHealth_tick.
static float   altLive_rpm = 0, altLive_exc = 0, altLive_amps = 0, altLive_pred = 0;
static float   altLive_vbus = 0, altLive_tF = 0, altLive_duty = 0;   // filtered fold outputs for the 1 Hz evaluator
static float   altLive_gAmps = 0;          // graded amps: episode boxcar average (same statistic the records store)
static float   altTempSlopeFMin = 0;       // case-temp slope (F/min, fast-slow EMA pair in the fold) — thermal-transient tag
static float   altLive_pct = 0;            // live output-% = graded amps ÷ LWLR prediction (NO clamp; may exceed 100)
static float   altLatchPct = 0;            // header Health chip: last graded (MEASURED/ESTIMATED) % this boot, held through ungraded ticks; 0 = none yet
static bool    altRefOk = false;           // state is MEASURED/ESTIMATED → the % is trustworthy
static float   altRefDist = 999.0f;        // normalized distance to nearest support point (diagnostics)
static bool    altLiveValid = false;
static bool    altSteady = false;          // FULL steady run (temp held STEADY_TEMP_SEC) — feeds surface+trend, draws the orange ring
static bool    altSessSteady = false;      // SESSION steady (temp held SESSION_TEMP_SEC = half) — gates a Session-plot dot
static uint8_t altRefSource = 0;           // active reference surface for grading: 0 = My History (learned), 1 = Uploaded File
static uint8_t altState = FRONT_NO_REFERENCE;   // confidence state (FrontStore::classify, 1 Hz)
static uint32_t altLastFoldMs = 0;         // last fold time — stale fold (field off) → live point not graded
static float   altWorstPctLive = 0;        // P10 output-% of the in-progress trend bucket
static float   altOverallPctLive = 0;      // current-bucket average output-%
static uint8_t altStatusCode = 0;          // 0 normal, 3 disabled (Ignore Temperature), 4 waiting for commissioning, 5 waiting for Charge Health Calibration (stage 9)
// Session statistics over the graded 1 Hz samples since boot/Start Over: mean + P10 histogram (60 × 2% bins).
static uint16_t (&altSessHist)[60] = PsramPlace::make<uint16_t[60]>();
static uint32_t altSessN = 0;
static double   altSessSum = 0;
static bool    altHiFieldAlert = false;    // high-field-low-output alert (independent of the record book)

// GUI-adjustable gate settings (Pattern B); steady-time/front/eval knobs live with the engine in 7_functions.ino.
// Bands act on the EMA-filtered inputs (altEmaSec); smear = band × dI/dx, axes add in quadrature (Alt_Health_Dev_Summary.md §3).
// RPM and field axes bound rms departure from their fitted line, so smooth ramps record (ALT_GATE_TUNING_CAPTURE_SPEC.md).
float altDutyLineRms   = 0.4f;   // % points rms from fitted line (filtered; raw CV dither ~3 p-p). 2.52 A/pt open-loop dominates
                                 // smear, but records store 2 s MEAN duty + amps, which co-move, so wander partly cancels.
                                 // Do NOT re-tighten: 0.3 starved emits. Also the emit-time trimmed boxcar range tolerance on field
float altDutySlewMax   = 1.0f;   // % points/s: a moving field means the regulator is chasing something, not one operating point
float altRpmLineRms    = 48.0f;  // RPM rms about the fitted line (filtered; raw idle jitter ~50 p-p); bounds wobble about a ramp,
                                 // not the ramp. 0.044 A/rpm at idle, 0.006-0.008 at cruise. Smear is bounded by altAmpsSlewPct,
                                 // not this; 12 was the largest sole-blocker. Do not re-tighten it without removing the output slope cap
float altRpmRateMax    = 250.0f; // rpm/s slope cap on that line; backstop only, the rms test already excludes what it would catch
float altVbusTol       = 0.20f;  // V, filtered max−min, not a line fit: |dI/dV| < 1.8 and sign-unstable, and a moving bus
                                 // is a load event rather than a ramp
// Admission floors:
float altMinAmps = 2.0f;
float altMinDuty = 5.0f;
// Mode:
float altPaused       = 0.0f;    // 1 = halt LEARNING into My History (sticky); independent of altRefSource.
                                 // Selecting Uploaded defaults this ON; live display + trend still update.
float altSimMode      = 0.0f;    // 1 only in demo mode (9_demo.ino sets and clears it): alt health grades the simulated boat, points stay local

// BOAT PERFORMANCE — sailing polar bins. Full design: BOAT_PERFORMANCE_MATRIX_PLAN.md.
#define PERF_WS_BINS    16         // wind-speed bands 0..40 kt
#define PERF_WS_MAX     40.0f
#define PERF_WA_BINS    36         // wind-angle bins 0..360° (symmetric fold uses 0..180 → first 18)
#define PERF_WA_MAX     360.0f
#define PERF_PITCH_BINS 6          // sea-state (pitch-std) bands for the 1-D derate
#define PERF_PITCH_MAX  12.0f      // pitch std (deg) full scale
#define PERF_SAIL_CELLS (PERF_WS_BINS * PERF_WA_BINS)   // 576
#define PERF_TOPK       4          // best-ever = K best window-means; reference = min(topK) (robust high pct)

// BOAT PERFORMANCE — Best-Ever Front: sparse best-ever boat-speed surfaces, SAIL over {AWS, AWA, pitch-std},
// MOTOR over {RPM, headwind, pitch-std}. Wind axes are APPARENT, stored both-sided; perfFoldSymmetric folds
// |AWA| at eval/display only. Speed = STW or SOG. Engine instances in 7_functions.ino.

// Live points for the dashboard (red dot + % gauge), via PerfLive / MotorLive SSE.
static float perfLive_ws = 0, perfLive_wa = 0, perfLive_spd = 0, perfLive_best = 0;
static float perfLive_pitch = 0, perfLive_pct = 0;
static bool  perfLiveValid = false;
static bool  perfSteady = false;    // currently inside a steady-run (sail OR motor episode accumulating) — live indicator
static float perfSettleSec = -1.0f; // dwell seconds remaining before the active episode banks; 0 = settled, -1 = not eligible
static uint8_t perfLiveSrc = 0;     // 1 STW, 2 SOG
static float motorLive_rpm = 0, motorLive_hw = 0, motorLive_spd = 0, motorLive_best = 0, motorLive_pct = 0, motorLive_pitch = 0;
static bool  motorLiveValid = false;
// Confidence states for the live % (FrontStore::classify at 1 Hz — same engine as the alternator's).
static uint8_t perfState = FRONT_NO_REFERENCE, motorState = FRONT_NO_REFERENCE;
static uint8_t motorLiveSrc = 0;

// GUI-adjustable settings (registry-driven); steady-time/sea-state/front/eval knobs live with the engine in 7_functions.ino.
float perfWsTol        = 2.0f;      // AWS deviation band (kt)
float perfWaTol        = 12.0f;     // AWA deviation band (deg)
float perfRpmTol       = 100.0f;    // motoring RPM deviation band
float perfMinBoatSpeed = 0.5f;      // admission floor (kt)
float perfMinWindSpeed = 2.0f;      // admission floor (kt, sailing)
float perfRpmFloor     = 50.0f;     // RPM ≤ this = engine off = sailing; above = motoring (motorsailing counts as motoring)
float perfSpeedSrc     = 1.0f;      // 1 = STW, 2 = SOG (switching source = Clear-All)
float perfFoldSymmetric = 1.0f;     // 1 = fold |AWA| at eval/display (default); 0 = keep both tacks
float perfPaused       = 0.0f;      // 1 = halt learning/persist (sticky across reboot); live display still updates
float perfSimMode      = 0.0f;      // 1 only in demo mode (9_demo.ino sets and clears it): boat performance reads the simulated boat, points stay local

// Supabase configuration......similar stuff is locally defined in a few functions that upload sensor data, config snapshots to cloud
const char *SUPABASE_URL = "https://qnbekuaoweuteylitzvo.supabase.co";
// PUBLIC anon key - safe to commit, protected by RLS policies
const char *SUPABASE_ANON_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InFuYmVrdWFvd2V1dGV5bGl0enZvIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NTE5NzY1MzUsImV4cCI6MjA2NzU1MjUzNX0.k2S_kzkdAyN1Azs_7enxLun9LouB1bA_q7Sw8x1Cp0o";


// Vessel Info
float BOAT_LENGTH_FT = 0;
float BOAT_DISPLACEMENT_LBS = 0;
// char arrays, not Strings: /saveVesselInfo writes on the web task while Core 1 reads (e.g. BATTERY_TYPE); a String
// reassignment frees the old buffer under the reader, an array can only tear. 50 chars + NUL = form maxlength.
char BOAT_TYPE[51] = "monohull";
// Monohull and catamaran are sailboats, every other type a motorboat; blank = monohull (form default).
bool boatIsSail() { return BOAT_TYPE[0] == '\0' || strcmp(BOAT_TYPE, "monohull") == 0 || strcmp(BOAT_TYPE, "catamaran") == 0; }
char BOAT_MAKE_MODEL[51] = "";
uint16_t BOAT_YEAR = 2025;
char ENGINE_MAKE[51] = "";
uint16_t ENGINE_HP = 0;
uint8_t SYSTEM_VOLTAGE_CLASS = 12;
// Field winding rated voltage (12/24/36/48, never above the class), set in Vessel Info. Many 24/36/48 V
// alternators carry a 12 V winding, which is why 12 is the default on every class.
uint8_t FieldWindingV = 12;
// The winding rating the STORED field-side duty knobs are scaled for (NVS). Equals fieldWindingEffective() except
// for the moment between a change and fieldWindingReconcile(), and at boot after an import wrote NVS raw.
uint8_t fieldWindApplied = 12;
// "W:R" (rating, ohms) the owner confirmed when the resistance check doubted a bus-rated winding; any other pair re-arms it.
char fieldWindAck[24] = "";
// Share of a winding's rated excitation one duty-% gives on this bus, relative to a 12 V winding on a 12 V bus
// (12/class for a 12 V winding, 1 for a bus-rated one). Every 12 V-referenced field-duty default, duty rate,
// duty threshold and current-loop gain is multiplied by it.
float fieldScale() {
  return (float)fieldWindApplied / (float)(SYSTEM_VOLTAGE_CLASS ? SYSTEM_VOLTAGE_CLASS : 12);
}
// BatteryCapacity_Ah already exists
char BATTERY_TYPE[51] = "lifepo4";
char BATTERY_MAKE_MODEL[51] = "";
char ALTERNATOR_BRAND_MODEL[51] = "";
// SolarWatts already exists
// imuMountOrientation already exists
float IMU_DIST_BOW_FT = 0;
float IMU_DIST_CL_FT = 0;
float IMU_HEIGHT_WL_FT = 0;
char HOME_PORT[51] = "";  // 50 chars + null terminator
// Per-unit label; empty = consumers fall back to XREG-<uid6>. Two regulators on one LAN must never present the same name.
char REGULATOR_NAME[33] = "";

// Bounded copy for every vessel-info text field above. A null src (absent JSON key) writes "".
static void vesselSetText(char *dst, size_t cap, const char *src) {
  if (!src) src = "";
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
}


// Accelerometer Stuff
// I²C address - ST-style 8-bit format (library shifts internally)
// LSM6DSOX_I2C_ADD_L = 0xD5 (SA0=0), LSM6DSOX_I2C_ADD_H = 0xD7 (SA0=1)
constexpr uint8_t LSM6DSOX_ADDR = LSM6DSOX_I2C_ADD_L;

// LSM6DSOX sensor object (shares existing Wire instance)
LSM6DSOXSensor imu(&Wire, LSM6DSOX_ADDR);

// FIFO tag_sensor values (after decoding: tag_sensor = raw_tag >> 3)
constexpr uint8_t TAG_SENSOR_GYRO = 1;
constexpr uint8_t TAG_SENSOR_ACCEL = 2;
constexpr uint8_t TAG_SENSOR_TEMP = 3;

// Axis remapping (compile-time constants, will move to settings later)
// Adjust these based on PCB mounting orientation relative to boat axes
constexpr int8_t ACCEL_X_SIGN = 1;  // +1 or -1
constexpr int8_t ACCEL_Y_SIGN = 1;
constexpr int8_t ACCEL_Z_SIGN = 1;
constexpr int8_t GYRO_X_SIGN = 1;
constexpr int8_t GYRO_Y_SIGN = 1;
constexpr int8_t GYRO_Z_SIGN = 1;

struct IMUSample {
  int16_t x, y, z;
  uint32_t timestamp_us;
};

static_assert(sizeof(IMUSample) == 10 || sizeof(IMUSample) == 12,
              "IMUSample unexpected size - check padding");

// Target ~32 KB total for both rings.
constexpr uint16_t ACCEL_RING_SIZE = (sizeof(IMUSample) == 10) ? 2000 : 1666;  // ~24 KB
constexpr uint16_t GYRO_RING_SIZE = (sizeof(IMUSample) == 10) ? 666 : 555;     // ~8 KB

volatile bool otaInProgress = false;
volatile uint32_t otaInProgressSinceMs = 0;  // stamped with otaInProgress; checkOtaWedge() retires a flag older than OTA_WEDGE_LIMIT_MS
volatile bool settingsDirty = false;  // set by /get handler; triggers immediate CSV3 send
unsigned long lastEventSourceSend = 0;


struct ImuRingBuffer {
  IMUSample accel[ACCEL_RING_SIZE];
  IMUSample gyro[GYRO_RING_SIZE];
  uint16_t accel_head = 0;
  uint16_t accel_tail = 0;
  uint16_t gyro_head = 0;
  uint16_t gyro_tail = 0;
  uint32_t accel_dropped = 0;
  uint32_t gyro_dropped = 0;
};
static ImuRingBuffer *imuRingBuffer = nullptr;

// IMU polling state
bool imuEnabled = false;
unsigned long lastIMUPoll = 0;
constexpr unsigned long IMU_POLL_INTERVAL = 10;  // ms

constexpr uint16_t MAX_FIFO_DRAIN_PER_POLL = 6;   // Conservative start, tune up if needed
uint8_t fifoBuffer[MAX_FIFO_DRAIN_PER_POLL * 7];  // 7 bytes per FIFO entry

// Error/diagnostic counters
uint32_t imu_fifo_overrun_count = 0;
uint32_t imu_i2c_error_count = 0;
uint64_t imu_total_samples_accel = 0;
uint64_t imu_total_samples_gyro = 0;
uint32_t imu_unknown_tag_count = 0;

constexpr int ANALOG_READ_COLLISION_THRESHOLD = 1200;  // µs - skip IMU if analog read was long


// ===== IMU DERIVED METRICS - GLOBAL VARIABLES =====

// --- RAW SIGNALS (Current values, updated at reporting rate) ---
// Note: Stored as int16 in ring buffers, converted to float only when needed
float imu_accel_x_raw = 0;  // g
float imu_accel_y_raw = 0;  // g
float imu_accel_z_raw = 0;  // g
float imu_gyro_x_raw = 0;   // dps
float imu_gyro_y_raw = 0;   // dps
float imu_gyro_z_raw = 0;   // dps

// --- CALCULATED METRICS (Real-time) ---
float imu_heel_deg = 0;
float imu_pitch_deg = 0;
float imu_yaw_rate_dps = 0;          // Current yaw rate (direct from gyro_z)
float imu_vertical_accel_g = 0;
float imu_total_accel_g = 0;         // Total acceleration magnitude
float imu_msi_score = 0;             // Motion Sickness Index (L&G 1987, freq-weighted vertical accel RMS; 100 = severe)
float imu_vomit_pct = 0;             // Estimated % of population vomiting after 2hrs (L&G 1987, power-law approx)
float imu_anchorage_comfort = 0;     // Heuristic comfort score 0-100 (100=calm); roll+MSI+slam weighted
float imu_heel_deviation_120s = 0;   // Peak roll deviation from 2-min mean (°)
float imu_pitch_deviation_120s = 0;  // Peak pitch deviation from 2-min mean (°)
float imu_heading_swing_120s = -1;   // Peak-to-peak heading swing over 2 min (°); -1 = no compass data

// --- RAW SIGNAL STATISTICS (Reset every reporting period) ---
float imu_accel_x_min = 999.0;
float imu_accel_x_max = -999.0;
float imu_accel_x_sum = 0;
uint32_t imu_accel_x_count = 0;

float imu_accel_y_min = 999.0;
float imu_accel_y_max = -999.0;
float imu_accel_y_sum = 0;
uint32_t imu_accel_y_count = 0;

float imu_accel_z_min = 999.0;
float imu_accel_z_max = -999.0;
float imu_accel_z_sum = 0;
uint32_t imu_accel_z_count = 0;

float imu_gyro_x_min = 999.0;
float imu_gyro_x_max = -999.0;
float imu_gyro_x_sum = 0;
uint32_t imu_gyro_x_count = 0;

float imu_gyro_y_min = 999.0;
float imu_gyro_y_max = -999.0;
float imu_gyro_y_sum = 0;
uint32_t imu_gyro_y_count = 0;

float imu_gyro_z_min = 999.0;
float imu_gyro_z_max = -999.0;
float imu_gyro_z_sum = 0;
uint32_t imu_gyro_z_count = 0;

// --- CALCULATED METRIC STATISTICS (Reset every reporting period) ---

// Event detection thresholds
float CAPSIZE_THRESHOLD_DEG = 120.0f;
float PITCHPOLE_THRESHOLD_DEG = 70.0f;
float SLAM_THRESHOLD_G = 2.5f;  // Vertical accel threshold for slam

float imu_heel_min = 999.0;
float imu_heel_max = -999.0;
float imu_heel_sum = 0;
uint32_t imu_heel_count = 0;

float imu_pitch_min = 999.0;
float imu_pitch_max = -999.0;
float imu_pitch_sum = 0;
uint32_t imu_pitch_count = 0;

float imu_vertical_accel_min = 999.0;
float imu_vertical_accel_max = -999.0;
float imu_vertical_accel_sum = 0;
uint32_t imu_vertical_accel_count = 0;

float imu_total_accel_min = 999.0;
float imu_total_accel_max = -999.0;
float imu_total_accel_sum = 0;
uint32_t imu_total_accel_count = 0;

// --- SUMMARY METRICS (Time-windowed, 60s rolling) ---
float imu_heel_change_60s = 0;     // max - min over 60s
float imu_heel_deviation_60s = 0;  // deviation from mean over 60s
float imu_pitch_change_60s = 0;
float imu_pitch_deviation_60s = 0;

// --- EVENT COUNTERS ---
uint32_t imu_capsize_count = 0;        // Lifetime, persistent to NVS
uint32_t imu_pitchpole_count = 0;      // Lifetime, persistent to NVS
uint32_t imu_slam_count = 0;           // Reset every reporting period
uint32_t imu_slam_count_lifetime = 0;  // Lifetime, persistent to NVS
float imu_slam_peak_max = 0;           // Reset every reporting period

// --- SEA STATE HOURS (stored as minutes, NVS-persisted) ---
// Moving = SOGNMEA >= 1.5 kts; buckets by MSI: gentle<10, moderate<30, rough<70, extreme>=70
uint32_t imu_min_moving_gentle = 0;
uint32_t imu_min_moving_moderate = 0;
uint32_t imu_min_moving_rough = 0;
uint32_t imu_min_moving_extreme = 0;
uint32_t imu_min_stat_gentle = 0;
uint32_t imu_min_stat_moderate = 0;
uint32_t imu_min_stat_rough = 0;
uint32_t imu_min_stat_extreme = 0;

// --- WAVE PERIOD ---
float imu_wave_period_sec = -1.0;  // -1 = no valid detection

// --- LIFETIME MAXIMUMS (Persistent to NVS) ---
float imu_heel_max_lifetime = 0;
float imu_pitch_max_lifetime = 0;
float imu_slam_peak_lifetime = 0;

// --- MOUNTING ORIENTATION ---
uint8_t imuMountOrientation = 0;  // 0-3, persisted in NVS with the rest of Vessel Info (NK_imuMountOrient)
constexpr uint8_t IMU_ORIENT_COUNT = 4;  // must equal the axisRemap[] row count — static_assert in 4_functions.ino
bool vesselInfoSaved = false;  // NK_vesselSaved exists — until then IMU metrics and the first-boot SoC seed hold off (orientation/chemistry unknown)

// --- COMPLEMENTARY FILTER STATE ---
float cf_heel = 0;
float cf_pitch = 0;
unsigned long cf_lastUpdate = 0;

// --- IMU ZERO / LEVEL CALIBRATION (captured at rest, persisted as JSON string in NVS key NK_imu_zero) ---
float imuHeelOffsetDeg = 0;   // accel-derived heel at rest; subtracted from filter
float imuPitchOffsetDeg = 0;  // accel-derived pitch at rest; subtracted from filter
float imuGxBias = 0;          // gyro pitch-rate bias at rest (dps, vessel frame)
float imuGyBias = 0;          // gyro heel-rate bias at rest (dps)
float imuGzBias = 0;          // gyro yaw-rate bias at rest (dps)

// --- IMU INSTALL VALIDATION ---
// Mount state is a latched FACT decided at Zero capture, cleared only by another Zero: az = -sensorY in every axisRemap
// row, so az ≈ +1 g at a level Zero iff the board is truly vertical. Judged from the CAPTURE, never a running window
// (34° heel gives az 0.83 g). The heel/pitch bias check is per-window: suspect data is flagged, never discarded.
enum : uint8_t { IMU_MOUNT_UNKNOWN = 0, IMU_MOUNT_OK = 1, IMU_MOUNT_BAD = 2 };
uint8_t imuMountState = IMU_MOUNT_UNKNOWN;
constexpr float IMU_MOUNT_VACC_MIN_G = 0.85f;   // az at Zero below this → up axis >32° off vertical (signed: catches upside-down)
constexpr float IMU_MOUNT_TACC_MIN_G = 0.85f;   // |a| at Zero outside [min,max] → sensor dead, saturated or mis-scaled
constexpr float IMU_MOUNT_TACC_MAX_G = 1.15f;
constexpr float IMU_BIAS_SUSPECT_DEG = 45.0f;   // per-window |avg heel| or |avg pitch| above this → flag the window
constexpr float IMU_ZERO_WARN_DEG    = 20.0f;   // captured offset above this → warn; a level boat at the dock is near 0
constexpr float IMU_ZERO_WARN_AMAG_G = 0.05f;   // mean |a| off 1 g by more than this → boat moved during the capture
constexpr uint64_t IMU_WINDOW_MIN_VALID_US = 60000000ULL;  // ≥60 s of samples before a window may judge its own bias
bool imuZeroCaptured = false;   // mirrors settingExists(NK_imu_zero) — keeps an NVS read out of the payload path

// Live install status for the dashboard warning. The per-window 45° bias is deliberately NOT here (a window property,
// not the install's). Code 4 (no IMU) has nothing to verify or tell the user.
inline uint8_t imuInstallCode() {   // 0 = OK, 1 = never zeroed, 2 = mount not vertical,
  if (!imuEnabled)                        return 4;   // 3 = zeroed before the mount check existed, 4 = no IMU
  if (!imuZeroCaptured)                   return 1;
  if (imuMountState == IMU_MOUNT_BAD)     return 2;
  if (imuMountState == IMU_MOUNT_UNKNOWN) return 3;
  return 0;
}

// Zero-capture state machine (averages ~2s of samples before storing)
volatile bool imuZeroInProgress = false;
double imuZeroAxSum = 0, imuZeroAySum = 0, imuZeroAzSum = 0;  // accel sums
double imuZeroGxSum = 0, imuZeroGySum = 0, imuZeroGzSum = 0;  // gyro sums
uint16_t imuZeroAccelN = 0, imuZeroGyroN = 0;
const uint16_t IMU_ZERO_TARGET = 200;  // ~2s of accel samples at 104 Hz (normal completion)
// Timeout guarantees the capture resolves (updateAccelMetrics runs every loop): finalize on what was gathered if >= floor, else abort.
const uint16_t IMU_ZERO_MIN = 80;                 // floor (~0.8s) below which the average is too noisy to trust → abort
const unsigned long IMU_ZERO_TIMEOUT_MS = 5000;   // capture window always closes within this
unsigned long imuZeroStartMs = 0;                 // millis() when the capture opened (timeout reference)

// --- WAVE PERIOD DETECTION STATE ---
float wave_decimated_buffer[100];  // Circular buffer for decimated samples
uint16_t wave_decim_head = 0;
float wave_last_dc_mean = 0;  // For DC removal
bool wave_last_crossing_positive = false;
unsigned long wave_last_crossing_time = 0;
float wave_period_ewma = -1.0;

// IMU NVS cache variables
uint32_t prev_imu_capsize_count = 0;
uint32_t prev_imu_pitchpole_count = 0;
uint32_t prev_imu_slam_count_lifetime = 0;
float prev_imu_heel_max_lifetime = 0;
float prev_imu_pitch_max_lifetime = 0;
float prev_imu_slam_peak_lifetime = 0;
uint32_t prev_imu_min_moving_gentle = 0;
uint32_t prev_imu_min_moving_moderate = 0;
uint32_t prev_imu_min_moving_rough = 0;
uint32_t prev_imu_min_moving_extreme = 0;
uint32_t prev_imu_min_stat_gentle = 0;
uint32_t prev_imu_min_stat_moderate = 0;
uint32_t prev_imu_min_stat_rough = 0;
uint32_t prev_imu_min_stat_extreme = 0;



unsigned long forcedUpdateDeadline = 0;
bool hasForcedUpdate = false;
int forcedFwVersionInt = 0;
volatile bool otaCheckDone = false;   // boot version report + forced-update check sent (loop(), core 1); registration reads it on core 0
bool pendingConfigCheckDone = false;  // boot admin-config pull sent; the field-on queue flush re-arms it

// Admin config push notices for the dashboard (the apply is boot-only, executeGetPendingConfig) so a change is never invisible.
// cfgPushPendingCount comes from the 6 h check-forced-update poll (nothing applied there) so a user can restart early;
// cfgPushAppliedCount is restored from NVS at boot (written by the push before its reboot), cleared by the dashboard ack.
uint8_t cfgPushPendingCount = 0;   // settings staged in the cloud; 0 = nothing queued
// Fixed storage, not String: written on the HTTPS task, read by /configPush on the webserver
// task with no lock — a race on a char array can tear text, never the heap.
char (&cfgPushPendingNote)[84] = PsramPlace::make<char[84]>();  // short admin description shown in the notice (firmware caps at 80)
uint16_t cfgPushAppliedCount = 0;  // settings the previous boot's push changed; 0 = nothing to report
String  cfgPushAppliedKeys  = "";  // comma-separated param names it changed (capped, see CFGPUSH_KEYS_CAP)

// Collector for param names applyImportConfig actually CHANGED; nullptr = don't collect. A global, not a defaulted parameter:
// the prototype generator re-emits the default (duplicate decl) and never forward-declares variables (4_functions.ino sets it).
String *cfgImportChangedNames = nullptr;
uint16_t cfgImportChangedCount = 0;  // names noted while collecting; exact past the list's CFGPUSH_KEYS_CAP

// WiFiClientSecure secureClient;  // Reusable SSL client to prevent stack overflow when we had individual ones for each upload   ABANDONED, THIS WAS NOT GOOD IN FLAKY WIFI
unsigned long lastHttpsOperationTime = 0;
const unsigned long HTTPS_MIN_INTERVAL = 500;            // OTA entry only (the three performOTA* paths) waits this long after the last HTTPS op; httpsTask queue items don't
const unsigned long CONFIG_SNAPSHOT_INTERVAL = 86400000;    // 24 hr — production (TEST value was 300000 = 5 min)
// SENSOR_UPLOAD_INTERVAL is firmware-only (no UI, no LittleFS) — edit + reflash to change.
const unsigned long SENSOR_UPLOAD_INTERVAL = 600000;          // 10 min — production (TEST value was 120000 = 2 min)
const unsigned long BUFFER_UPLOAD_INTERVAL = 13000;  // 13 seconds
const unsigned long BOATPERF_UPLOAD_INTERVAL = 900000;  // 15 min — field-off-gated upload + cloud re-fit cadence
unsigned long lastBoatPerfUploadTime = 0;
const unsigned long ALTHEALTH_UPLOAD_INTERVAL = 900000;  // 15 min — field-off-gated alt-health upload + cloud re-fit cadence
unsigned long lastAltHealthUploadTime = 0;
int64_t lastAltHealthSyncEpoch = 0;   // unix sec of last SUCCESSFUL alt-health cloud sync this boot (0 = none) — for "synced N ago"
int64_t lastBoatPerfSyncEpoch  = 0;   // unix sec of last SUCCESSFUL boat-perf cloud sync this boot
// Maintenance reboot ladder (checkAndRestart, 5_functions.ino); uptime-relative, nothing persists. 24 h: fully quiet window
// (engine+field off, no SSE client, battery OK, queue drained) held 5 min. 48 h: drops the no-client/SoC preferences.
// 72 h: regardless of engine (graceful field-down); only OTA / active test / brown-out voltage defer.
// 72 h must stay far below the 49.7-day millis() wrap — other timers aren't wrap-audited.
const unsigned long REBOOT_OPPORTUNISTIC_MS = 86400000UL;   // 24 h
const unsigned long REBOOT_RELAXED_MS       = 172800000UL;  // 48 h
const unsigned long REBOOT_HARD_MS          = 259200000UL;  // 72 h
const unsigned long REBOOT_QUIET_HOLD_MS    = 300000UL;     // quiet window must hold this long
const int   REBOOT_SOC_FLOOR_X100  = 3000;   // quiet-tier SoC preference (SOC_percent is %×100)
const float REBOOT_VBAT_FLOOR_V    = 11.8f;  // ×(SYSTEM_VOLTAGE_CLASS/12) — quiet/relaxed battery floor
const float REBOOT_VBAT_BROWNOUT_V = 11.5f;  // ×(SYSTEM_VOLTAGE_CLASS/12) — hard tier: WiFi startup spike into a near-dead
                                             // battery risks a brown-out boot-loop / NVS write corruption
const int   REBOOT_HEAP_FLOOR_KB       = 20;          // largest contiguous internal block below this = TLS effectively dead (HTTPS preflight skips <15 KB)
const unsigned long REBOOT_HEAP_LOW_HOLD_MS = 1800000UL;  // 30 min continuously low before requesting — handshake dips recover in seconds
const unsigned long FORCED_RECHECK_INTERVAL_MS = 21600000UL;  // 6 h — periodic forced-update/version re-check (boot check alone is too slow at 24-72 h uptimes)

//Configuration Snapshot Stuff
char *configPayloadBuffer;

unsigned long lastConfigSnapshotTime = 0;
// Starts true so the first field-off window after boot uploads; also set by saveVesselInfoToNvs() and /registerProfile so
// the user_profiles projection is fresh on Save. Cleared only when a payload is queued, so a failed gate retries.
bool configSnapshotRequested = true;
// Retry wait after no connection/response: 0 = none pending (gate's 60 s throttle applies), then 60 s doubling to 1 h.
// Cleared by success and by every fresh request.
unsigned long configSnapshotRetryMs = 0;
unsigned long queueDrainHoldStart = 0;  // millis() when we started waiting for queue to drain before low power; 0 = not waiting

// App-usage analytics: /track page-dwell / button counters in a fixed PSRAM pool of opaque keys ("p:<page>"/"b:<button>"),
// so new pages need no firmware change. Uploaded once per LOCAL day (client UTC offset, epoch math, immune to maintenance
// reboots); persisted to /usage.bin at shutdown flush points. Accumulates in every mode; upload needs Client mode + cloud.
struct UsageSlot {
  char key[44];  // 'p:'/'b:' + client key (client caps at 40 chars) + NUL
  uint32_t n;    // event count (page opens / button presses)
  uint32_t ms;   // visible dwell time, pages only
};
const int USAGE_SLOTS = 192;
const int USAGE_PAYLOAD_SIZE = 24576;
UsageSlot *usageSlots = nullptr;  // ps_malloc'd in setup()
uint16_t usageSlotCount = 0;
uint32_t usageOverflowN = 0;  // increments that arrived after all slots were taken
uint32_t usageOpens = 0;      // period app-open count (all clients)
uint32_t usageOpensApp = 0;   // subset from Capacitor clients
int64_t usagePeriodStartEpoch = 0;  // 0 = clock has never been valid this period
int32_t usageTzOffsetS = 0;         // last client-reported UTC offset (seconds east); persisted in /usage.bin
bool usageAppSeen = false;          // a Capacitor client posted this period
bool usageDirty = false;            // accumulator changed since last dumpUsageAccum()
unsigned long lastUsageUploadAttempt = 0;
SemaphoreHandle_t usageMutex = NULL;  // /track runs on async_tcp (Core 0 tree), build/reset on loop() — cross-core
// Serializes the deferred commissioning-start persist machine: worker (loop(), Core 1) vs
// Begin/Cancel (async_tcp, Core 0). See cxStartPersistService in 2_functions.ino.
SemaphoreHandle_t cxStartMutex = NULL;
// Lifetime totals (NVS "storage" namespace via saveNVSDataFull, same pattern as EngineRunTime_AllTime)
double UsageOpenTime_AllTime = 0.0;  // seconds of visible app time
uint32_t UsageOpens_AllTime = 0;
uint32_t UsageDays_AllTime = 0;  // DELIVERED usage uploads (incremented on HTTP 200 in executeUploadUsage)
int32_t prev_UsageOpenTime_AllTime = 0;
int32_t prev_UsageOpens_AllTime = 0;
int32_t prev_UsageDays_AllTime = 0;

uint64_t chipid;
int firmwareVersionInt = 0;
uint32_t deviceIdUpper = 0;
uint32_t deviceIdLower = 0;
char device_id_hex[17];

int currentPartitionType = 0;  // 0=factory, 1=ota_0

// Streaming tar extraction state (no gzip decompression)
struct StreamingExtractor {
  // Tar parsing state
  bool inTarHeader;
  uint8_t tarHeader[512];
  size_t tarHeaderPos;
  String currentFileName;
  size_t currentFileSize;
  size_t currentFilePos;
  bool isCurrentFileFirmware;

  //Padding state for proper streaming
  bool inPadding;
  size_t paddingRemaining;

  esp_ota_handle_t otaHandle;
  const esp_partition_t *otaPartition;
  bool otaStarted;

  // LittleFS state
  File currentWebFile;
  bool prodFSMounted;
  bool webWriteFailed;  // any web-file open/write/size failure — fails the whole update before the boot switch

  mbedtls_md_context_t hashCtx;
  bool hashStarted;
};

#define BMP3_ADDR 0x76
BMP388_DEV bmp388;

//WIFI STUFF
//these will be the custom network created by the user in AP mode (custom values persist in NVS as NK_apssid / NK_appass)
String esp32_ap_ssid = "ALTERNATOR_WIFI";  // Default SSID
// WiFi connection timeout when trying to avoid Access Point Mode (and connect to ship's wifi on reboot)
const unsigned long WIFI_TIMEOUT = 20000;  // 20 seconds
String esp32_ap_password = "alternator123";  // Default ESP32 AP password
// Reconnect engine state (checkWiFiConnection(), every loop). No backoff or give-up: cheap presence scans until the
// network returns (a hotspot can appear at any moment).
enum : uint8_t { WIFI_RECON_IDLE = 0,
                 WIFI_RECON_SEEKING = 1,
                 WIFI_RECON_JOINING = 2 };
struct WiFiReconnection {
  uint8_t state = WIFI_RECON_IDLE;
  unsigned long lastScanKickoff = 0;
  unsigned long joinStart = 0;
  int32_t channel = 0;       // channel of last sighting/connection; 0 = unknown → full sweep
  uint8_t scansSinceSweep = 0;
  int attemptCount = 0;      // failed joins since last connect (diagnostics)
  int lastSignalStrength = -999;
} wifiRecon;
const unsigned long WIFI_SCAN_CADENCE_MS = 2000;   // ~5-6 mA average while disconnected, worst-case ~2 s detect
const unsigned long WIFI_JOIN_TIMEOUT_MS = 15000;  // assoc+DHCP wall clock before returning to seek
const uint8_t WIFI_FULL_SWEEP_EVERY = 5;           // every Nth scan sweeps all channels (hotspot channel varies per session)

// Settings arm gate: an acknowledgment, not authentication (LAN clients are trusted). RAM-only, so a reboot disarms; while
// disarmed every mutating endpoint rejects, which kills replayed/stale settings URLs.
// True while the LM2907 tach is known to be lying (field-cut test, hard-cut tail). Set every tick by
// buildTickSnapshot; read by the engine start/stop console lines so they do not report a phantom.
bool g_rpmTachMasked = false;
// No timer: armed stays armed until Lock Controls, another client's lock, or a reboot. Do not re-add an auto-lock:
// it stranded a user mid-session and silently refused the next write.
bool settingsArmed = false;
// Optional password on the gate (Setup > System): arming needs it and returns a token every mutating request must carry
// (shared marina WiFi). Salt + hash only; tokens are RAM-only.
bool pwRequired = false;
char pwSaltHex[17] = "";
char (&pwHashHex)[65] = PsramPlace::make<char[65]>();
#define ARM_TOKENS 4   // one per unlocked client; a fifth unlock retires the oldest
char (&armTok)[ARM_TOKENS][17] = PsramPlace::make<char[ARM_TOKENS][17]>();
uint8_t armTokNext = 0;
uint8_t pwFailCount = 0;
uint32_t pwLockUntilMs = 0;   // five wrong passwords buy a 60 s refusal of every attempt

// ===== HEAP MONITORING =====
int rawFreeHeap = 0;      // in bytes
int FreeHeap = 0;         // in KB
int MinFreeHeap = 0;      // in KB
int FreeInternalRam = 0;  // in KB
int Heapfrag = 0;         // 0–100 %, integer only; computed with each internal-RAM walk (walkInternalHeap)
uint32_t FreePSRAM = 0;   // KB of free PSRAM
uint32_t LargestPSRAMBlock = 0;  // KB, largest contiguous free PSRAM block; written only by setup() and cpuLoadTask (Core 0)
volatile uint32_t largestPsramAtMs = 0;    // millis() of that walk, 0 = none yet
size_t LargestInternalBlock = 0;           // KB, largest contiguous free internal block; written only by walkInternalHeap() (setup, cpuLoadTask)
volatile uint32_t largestInternalAtMs = 0; // millis() of that walk, 0 = none yet
volatile bool heapWalkAllowed = false;     // Core 1 copy of fieldCutSettled(0) for cpuLoadTask (the function's state is Core-1 only)
size_t TotalInternalRam, TotalPSRAM;
int LittleFsFreeKb = -1;   // userdata LittleFS free, KB; -1 until boot seed. Writers set fsFreeDirty; refresh runs at the next 4 s health sample with the field cut settled (usedBytes() is a ~100ms full-flash traversal — never on a live control pass)
bool fsFreeDirty = false;  // writers: Core 1 loop + httpsTask ring-backup remove; a set lost to a concurrent Core 1 clear only delays the readout
int WebFsUsedKb = -1;              // web-asset partition in use, KB; -1 until the boot seed. Only an OTA changes it, so one read at boot covers the whole run
int WebFsTotalKb = -1;             // that partition's size, KB — 1024 for both prod_fs and factory_fs
const char *WebFsPartLabel = "?";  // which partition the two above describe: a failed OTA remounts webFS on factory_fs


// ===== TASK STACK MONITORING =====
const int MAX_TASKS = 50;  // Grossly big enough!
TaskStatus_t *taskArray;
// runUs = the task's ulRunTimeCounter at the census, esp_timer microseconds (32-bit, wraps at 71.6 min); /debug divides the
// delta against the previous census by atUs delta for each task's share of its core, IDLE0/IDLE1 included.
struct TaskCensusRow { char name[configMAX_TASK_NAME_LEN]; TaskHandle_t handle; uint32_t runUs; uint32_t hwm; int32_t core; uint8_t prio; };
struct TaskCensus { int64_t atUs; uint32_t atMs; uint16_t n; TaskCensusRow row[MAX_TASKS]; };
TaskCensus *taskCensus = nullptr;      // [2] in PSRAM: cpuLoadTask fills the slot /debug is not reading, then flips taskCensusPub
volatile uint8_t taskCensusPub = 0;
volatile uint32_t taskCensusAtMs = 0;  // millis() of the published census, 0 = none yet
volatile bool taskCensusWanted = false;  // set by /debug; each census freezes Core 1 ~4 ms, so only one per /debug view
// Optional: reuse these across calls if needed
int numTasks = 0;
int tasksCaptured = 0;
int stackBytes = 0;
int core = 0;

//CPU
// ===== CPU LOAD TRACKING =====
unsigned long lastIdle0Time = 0;  // Previous IDLE0 task runtime counter
unsigned long lastIdle1Time = 0;  // Previous IDLE1 task runtime counter
unsigned long lastCheckTime = 0;  // Last time CPU load was measured
int cpuLoadCore0 = 0;             // CPU load percentage for Core 0
int cpuLoadCore1 = 0;             // CPU load percentage for Core 1
int cpuLoadCore0Max = 0;
int cpuLoadCore1Max = 0;

//More health monitoring
// Session and health tracking
unsigned long sessionStartTime = 0;
unsigned long LastSessionDuration = 0;     // seconds, persistent
unsigned long CurrentSessionDuration = 0;  // seconds
int LastSessionMaxLoopTime = 0;            // microseconds, persistent (UI divides by 1e6 to show seconds)
int MaxLoopTime = 0;                       // not displayed on client, but available here
int lastSessionMinHeap = 999999;           // KB, persistent
int wifiReconnectsTotal = 0;               // persistent, the one i actually use
// Simple WiFi disconnect tracking
int wifiDisconnectCount = 0;
int LastResetReason;         // why the ESP32 restarted most recently
int ancientResetReason = 0;  // why the ESP32 restarted 2 sessions ago
int totalPowerCycles = 0;    // Total number of power cycles (persistent)

// Raw reset causes captured at boot by captureResetReason(): the esp-level enum plus the
// per-CPU ROM codes, which distinguish causes the esp level collapses (e.g. GLITCH_RTC_RST
// vs POWER_GLITCH_RESET). Shown in the BOOTED console line and serial banner.
int g_rawResetEsp = -1;
int g_rawResetRtc0 = -1;
int g_rawResetRtc1 = -1;

// Black box in RTC noinit RAM: survives every reset but a true power-down, so valid magic after an unexplained reset proves
// the 3.3V rail never died. Written every loop pass; captured + re-armed at boot in captureResetReason().
typedef struct {
  uint32_t magic;
  uint32_t upMillis;
  float ibv;
  float duty;
  float rpm;
  float measAmps;
  int32_t maxLoopUs;   // session-worst MaxLoopTime (µs) — feeds LastSessionMaxLoopTime on next boot
  int32_t minHeapKB;   // session MinFreeHeap — feeds lastSessionMinHeap on next boot
  int16_t altTempF;
  uint8_t sysMode;
  uint8_t chargeStage;
} BlackBoxSnap;
// sizeof folded into the magic so a layout change invalidates old snapshots.
#define BLACKBOX_MAGIC (0xB1ACB0C5UL ^ (uint32_t)sizeof(BlackBoxSnap))
#define BLACKBOX_DEMO_MAGIC (BLACKBOX_MAGIC ^ 0x00DE0000UL)
// Why a deferred reboot was asked for (REBOOT_INTENT_TAG | REBOOT_WHY_*), told to the next boot (captureResetReason): RTC
// noinit, so the reboot path writes no flash. Defined ahead of clockRealTag, so the linker places it last and clockRealTag,
// g_blackBox and g_ovTel keep their addresses.
RTC_NOINIT_ATTR uint32_t g_rebootIntent;
#define REBOOT_INTENT_TAG 0x1A904700UL
// Soft-clock origin (CLOCK_REAL_TAG, clockIsReal() below). Defined first so the linker places it LAST in RTC noinit:
// ahead of g_ovTel/g_blackBox it moved both, and a soft reset into the new layout fails their magic.
RTC_NOINIT_ATTR uint32_t clockRealTag;
RTC_NOINIT_ATTR BlackBoxSnap g_blackBox;
BlackBoxSnap g_blackBoxPrev;        // copy of last session's snapshot, taken at boot
bool g_blackBoxPrevValid = false;   // true if last session's magic matched (RTC RAM survived)

// Lifetime OV excursion telemetry in RTC noinit RAM (lost only on true power-down — accepted). Histogram of RAW bus V vs Bulk:
// 18 fine 0.2 V bins to Bulk+3.6, 12 coarse 1.0 V bins to Bulk+15.6, +1 overflow; widths scale ×(class/12). Separate from the
// session counters (g_voltSpikeCount etc.); zeroed only by a magic mismatch or /resetOvTelemetryLifetime.
#define OV_HIST_FINE_BINS 18
#define OV_HIST_COARSE_BINS 12
#define OV_HIST_BINS (OV_HIST_FINE_BINS + OV_HIST_COARSE_BINS + 1)  // 31
typedef struct {
  uint64_t timeMs[OV_HIST_BINS];  // cumulative ms with raw bus V in this band (first: 8-byte align; u32 would wrap at 49.7 days/bin)
  uint32_t events[OV_HIST_BINS];  // entries into this band (band-occupancy rising edge; low bins run large from CV ripple — expected, timeMs is the ripple-robust metric)
  uint32_t softExceedCount;       // rising edge: filtered V crossed target + OvMeasMarginV (Group-2 soft cap actually engaged)
  uint32_t swHardCutCount;        // rising edge: REASON_FAST_OVERVOLTAGE (SW hard cut, raw/per-tick)
  uint32_t inaCutCount;           // rising edge: REASON_INA_OVERVOLTAGE (INA228 averaged-compare latched cut — software-failed-to-protect backstop)
  uint32_t kdEventCount;          // rising edge: CV D-term engagement episodes (trim after ≥1s quiet) — pre-protection, counts saves not trips
  uint32_t tierLowCutCount;       // rising edge: REASON_OV_TIER_LOW (timed cut, LOW tier)
  uint32_t tierMidCutCount;       // rising edge: REASON_OV_TIER_MID (timed cut, MID tier)
  uint32_t magic;
} OvTelemetry;  // ~392 B; RTC slow RAM has ample room (black box is the only other user)
// Size + bin count in the magic: a layout change self-invalidates (no migration; old histograms aren't comparable).
#define OVTEL_MAGIC (0x0E7E1E77UL ^ (uint32_t)sizeof(OvTelemetry) ^ ((uint32_t)OV_HIST_BINS << 8))
RTC_NOINIT_ATTR OvTelemetry g_ovTel;
// Writers and /getOvTelemetry go through ovTel(): in demo it is a PSRAM copy, so RTC never carries demo data into a later boot.
OvTelemetry *demoOvTelPtr = nullptr;
#define ovTel() (demoOvTelPtr ? *demoOvTelPtr : g_ovTel)
// ovHistBin() + updateOvHistogram() live in 5_functions.ino (need BulkVoltage, declared below).

// These are health variables for the TempTask (digital temperature measurement)
unsigned long lastTempTaskHeartbeat = 0;
bool tempTaskHealthy = true;
bool tempTaskSuspended = false;                 // True while temp task is intentionally suspended (80MHz engine-off idle); gates checkTempTaskHealth()
bool tempTaskAlarm = false;                     // Set by checkTempTaskHealth() — raises alarm condition; physical buzzer (GPIO21) is still gated by AlarmActivate
const unsigned long TEMP_TASK_TIMEOUT = 20000;  // 20 seconds

//Cloud Upload Stuff
static const char *UPLOAD_HOST = "qnbekuaoweuteylitzvo.supabase.co";
static const uint16_t UPLOAD_PORT = 443;
//snapshot
const char *host = "qnbekuaoweuteylitzvo.supabase.co";
const int port = 443;
// Slow-cellular profile. CONNECT+HANDSHAKE share one blocking client.connect() with no WDT feed: their sum must stay < the 16 s
// panic WDT (DNS pre-resolved with a feed). READ_TIMEOUT is idle (any byte restarts it); GLOBAL_TIMEOUT is the per-attempt
// wall-clock cap and may exceed the WDT because the read loops feed it.
const uint32_t CONNECT_TIMEOUT = 7000;    // ms
const uint32_t HANDSHAKE_TIMEOUT = 7000;  // ms (setHandshakeTimeout() takes SECONDS — divide at call site)
const uint32_t READ_TIMEOUT = 10000;      // ms, idle
const uint32_t GLOBAL_TIMEOUT = 45000;    // ms


unsigned long lastConsoleMessageTime = 0;
const unsigned long CONSOLE_MESSAGE_INTERVAL = 700;  //
// 128 truncated the actionable tail of long diagnostics. The ring is ps_malloc'd (costs PSRAM); also caps queueConsoleMessageF's vsnprintf.
#define CONSOLE_MSG_LEN 256
static portMUX_TYPE consoleMux = portMUX_INITIALIZER_UNLOCKED;
// Console history ring (~55 KB PSRAM), /consolehist.txt: device-side source of truth for the Download Logs console file;
// live SSE drains from it (consoleSsePending), browser history is per-browser and lossy.
#define CONSOLE_HIST_SIZE 200
// Unsent live-console lines, capped well under the ring: an outage stops the drain and reconnect stamps the backlog with
// arrival time. 32 clears a wizard test's end-of-run burst (10 dropped SysID Bode lines).
#define CONSOLE_SSE_BACKLOG 32
struct ConsoleHistEntry {
  uint32_t ms;      // millis() at queue time
  time_t epoch;     // soft-clock epoch at queue time (pre-2020 value = clock not yet synced)
  char msg[CONSOLE_MSG_LEN];
};
ConsoleHistEntry *consoleHist = nullptr;
volatile int consoleHistHead = 0;
volatile int consoleHistCount = 0;
volatile int consoleSsePending = 0;   // newest ring entries not yet sent to SSE, capped at CONSOLE_SSE_BACKLOG


// DNS Server for captive portal
DNSServer dnsServer;
const byte DNS_PORT = 53;

enum DeviceMode {
  MODE_CONFIG,  // WiFi configuration (minimal - just DNS + config page)
  MODE_AP,      // Access Point with full functionality
  MODE_CLIENT   // Client mode connected to ship's WiFi
};
DeviceMode currentMode = MODE_CLIENT;  // gets overwritten immediately, exact mode here doesn't matter

bool littleFSMounted = false;

int INADisconnected = 0;
int BMP388Disconnected = 0;
int WifiHeartBeat = 0;
static uint32_t stateRevision = 0;  // used for UX in client interface improvement
// Boot identity in every SSE payload: a cached per-channel block with matching sessionId + sane age belongs to this boot,
// a mismatch proves it stale. 31 bits so it fits a signed int (SafeInt-shaped consumers).
static uint32_t g_sessionId = 0;
int VeTime = 0;                     // rolling
int VeTime2 = 0;                    //session

int SendWifiTime = 0;
//Needed for ALERT pin on INA228 to shut off Field and delay 10 seconds before retrying if fault condition is gone
bool inaOvervoltageLatched = false;
unsigned long inaOvervoltageTime = 0;
uint32_t inaOvervoltageClearedMs = 0;  // timestamp when INA OV latch last cleared; used to suppress disagreement check
// Covers field collapse + both sensors resettling; must exceed the INA228 averaged update period.
const uint32_t INA_OV_DISAGREE_SUPPRESS_MS = 10000;  // 10 seconds


// ── Input filter (disturbance rejection) ─────────────────────────────────
// Per-sample IIR EMA, α = dt / (TC + dt) from actual elapsed time, on CH0/CH1/CH2. Protections read the RAW originals
// (iExcess runs its own IExcessTau EMA). Each _filtered signal has its own TC; the thermistor (CH3) filters in tempPID_tick().
float OutputPIDFilterTC = 34.0f;   // ms — Output Current PID EMA TC, NVS-backed (τ/3 at commissioning plant τ≈103ms)
float VoltageFilterTC = 103.0f;    // ms — IBV EMA TC for CV voltage loop, NVS-backed (full plant τ at commissioning, τ≈103ms)
float g_pidI_filtered = 0.0f;        // Output Current PID EMA signal
float IBV_filtered = 0.0f;           // EMA of INA228 bus voltage — used by getFiltV()

// ── SystemID — plant delay measurement ───────────────────────────────────
// Step test: baseline → 3× (duty up / duty down) → post-process; PSRAM buffer allocated on first run, never freed.
// Legal in any SYS_MODE_AUTO state; Manual is banned (duty locked to ManualDutyTarget). voltageControlActive = !inIdleStage,
// so it is true even in bulk — do NOT gate on it. Enforced in /get startSystemID and the JS preflight.
float SystemIDStepAmplitude = 6.4f;  // % duty step; default from a commissioning field curve (12 V value, seeded x fieldScale())

// ── SystemID sine-sweep (open-loop plant Bode) — Stage 1 ──────────────────────
// Test type selects step (rise/fall delay) vs sine sweep (per-frequency gain/phase
// of measured current vs commanded duty, PID off = the plant's own frequency response).
uint8_t systemIDTestType   = 0;        // 0 = step delay, 1 = sine sweep
float   systemIDSineFreqStart = 0.5f;  // Hz, sweep low end
float   systemIDSineFreqEnd   = 20.0f; // Hz, sweep high end
uint8_t systemIDSineCycles    = 2;     // analysed cycles per frequency (1 settle cycle skipped); integer-cycle windowed, so 2 is clean
// Fitted plant τ (ms) from the Plant Delay sweep fit; 0 = not fitted. NVS-persisted so the Biggest Actionable Disturbance
// readout survives reboots; re-fit only when the plant changes (alternator, belt, field wiring).
uint16_t systemIDPlantTauMs   = 112;   // ms — default from a commissioning plant fit; overwritten by each fit
#define SYSID_SINE_NPOINTS 10          // log-spaced sweep points
#define SYSID_STEP_HOLD_MS 5000        // ms — fixed step-test per-phase dwell; sized to plant settling (several plant τ)
struct SysIDBodePoint { float freqHz; float gainApPct; float phaseDeg; };  // gain = A output per %duty
SysIDBodePoint (&systemIDBode)[SYSID_SINE_NPOINTS] = PsramPlace::make<SysIDBodePoint[SYSID_SINE_NPOINTS]>();
uint8_t systemIDBodeCount = 0;         // valid Bode points after a sine run (served by /sysidbode)
float   systemIDFsHz      = 0.0f;      // CH1 sample rate at the last plant sweep's start; 0 = not measured
// Engine speed across the last sweep's sine phase — advisory context served by /sysidbode
// (the wizard warns loosely on speed offset/wander; never gated or aborted on).
float   systemIDRpmMin = 0.0f, systemIDRpmMax = 0.0f, systemIDRpmAvg = 0.0f;

// SystemID open-loop sine-sweep history (50-record ring, /sysidsweeplog.bin).
struct SysIDSweepRecord {
  uint16_t runNumber;
  float    rolloffHz;      // -3dB plant roll-off; -1 if none in range
  float    dcGainApPct;    // low-frequency plateau gain (A per %duty)
  float    worstPhaseDeg;
  float    worstPhaseFreqHz;
  float    setupAmplitude; // % duty
  float    stabilizeAmps;  // Wave Floor, A
  float    sweepStartHz;
  float    sweepEndHz;
  uint8_t  cycles;
  uint8_t  nPoints;
  float    avgRPM;
  float    avgAltTempF;
  float    battV;          // bus voltage during the sweep
  uint8_t  chargeStage;    // getChargeStageDisplayCode() at commit
  uint32_t epoch;          // wall-clock Unix seconds at commit (0 = clock not synced)
  SysIDBodePoint curve[SYSID_SINE_NPOINTS];
};
SysIDSweepRecord *sysidSweepLog = nullptr;
uint8_t  sysidSweepLogCount = 0;
uint8_t  sysidSweepLogHead  = 0;
uint16_t sysidSweepRunCounter = 0;

volatile bool systemIDRequested = false;       // set true by UI handler to trigger a test run
volatile bool systemIDAbortRequested = false;  // set true by UI handler to abort in-progress test
uint8_t systemIDActive = 0;                    // 0=idle, 1-9=current phase (sent to UI for progress)
bool systemIDResultsReady = false;             // set true when post-processing is complete
uint32_t systemIDRunTok = 0;                   // run token, see fieldCurveRunTok
uint32_t systemIDLastEndMs = 0;                // millis() when last test ended (cooldown guard)
uint8_t systemIDAbortReason = 0;               // FieldEventReason code if protection aborted the test; 0 = no abort / clean exit
uint8_t systemIDAbortPhase = 0;                // phase (1-9) at moment of protection abort; 0 = no abort

// ── Stabilize-phase constants ────────────────────────────────────────────────
float SystemIDStabilizeAmps = 35.0f;      // target alternator output before the sweep (= sweep trough)
#define SYSID_STABILIZE_SAMPLES 5         // ring buffer size: 5 samples × 1Hz = 5-second window
#define SYSID_STABILIZE_BAND_A 3.0f       // 5-s rolling average must be within ±3A of target
#define SYSID_STABILIZE_TIMEOUT_MS 30000  // abort if can't stabilize within this window

float systemIDRiseDelay_ms[3] = { 0.0f, 0.0f, 0.0f };  // rising-step delays, ms
float systemIDFallDelay_ms[3] = { 0.0f, 0.0f, 0.0f };  // falling-step delays, ms
float systemIDRiseAvg_ms = 0.0f;
float systemIDFallAvg_ms = 0.0f;
float systemIDStepAmp_A[3] = { 0.0f, 0.0f, 0.0f };    // rise step amplitude per trial (upMean - quietMean), A
float systemIDQuietPP_A[3] = { 0.0f, 0.0f, 0.0f };    // quiet-phase peak-to-peak noise per trial (quietMax - quietMin), A

// ── SystemID ring buffer log (50 records, persisted to /systemidlog.bin) ──
// Committed on every completion (success or abort) so fleet snapshots ship a short history.
struct SystemIDRecord {
  uint16_t runNumber;
  float    score;              // = riseAvg_ms (lower = faster plant). -1 if aborted / no rises.
  float    riseDelays[3];      // ms per trial; -1 if not measured
  float    fallDelays[3];      // ms per trial; -1 if not measured
  float    riseAvg_ms;
  float    fallAvg_ms;
  float    stepAmps[3];        // A per trial (upMean - quietMean)
  float    quietPP[3];         // A peak-to-peak quiet-phase noise per trial
  uint8_t  abortReason;        // 0 = clean exit; non-zero = FieldEventReason
  uint8_t  abortPhase;         // phase number at abort; 0 = clean exit
  float    setupStepAmplitude; // SystemIDStepAmplitude at test time (% duty)
  float    avgRPM;             // RPM snapshot at commit
  float    avgAltTempF;        // AlternatorTemperatureF snapshot at commit
  float    battV;              // bus voltage snapshot at commit
  uint8_t  chargeStage;        // getChargeStageDisplayCode() at commit
  uint32_t epoch;              // wall-clock Unix seconds at commit (0 = clock not synced)
};

SystemIDRecord *systemIDLog = nullptr;  // ps_malloc(50 × sizeof(SystemIDRecord))
uint8_t  systemIDLogCount = 0;          // records currently in ring buffer (0–50)
uint8_t  systemIDLogHead  = 0;          // next write index
uint16_t systemIDRunCounter = 0;        // increments each commit; persists via loadSystemIDLog

struct SystemIDSample {
  uint32_t ts;     // millis() at sample time
  float duty_cmd;  // duty commanded this tick (%)
  float amps;      // MeasuredAmps at sample time (raw, unfiltered)
};
// 15000 × 12 bytes = 180 KB in PSRAM. Covers TC up to ~1.8 s at the ~13 ms CH1 cadence.
static const int SYSID_BUF_SIZE = 15000;
SystemIDSample *sysIDBuffer = nullptr;  // ps_malloc'd at test start, freed when systemID_tick is back at IDLE
int sysIDSampleCount = 0;

// ── SystemID pre-test state snapshot ─────────────────────────────────────────
// Captured at test start; restored on clean exit so the user returns to
// whatever mode they were in (AUTO, MANUAL, CV, bulk, etc.) without a jump.
struct SystemIDResumeState {
  bool valid = false;
  int sysMode = 0;
  float setpointLimited_snap = 0.0f;
  float lastAppliedDuty_snap = 0.0f;  // informational only; NOT replayed as a command
  float cv_I_snap = 0.0f;
  bool voltageControlActive_snap = false;
};
static SystemIDResumeState g_sysIDResume;

// ── Field-% curve (auto-commissioning Phase 1a) ───────────────────────────────
// Quasi-static duty→amps ramp on the SystemID duty-override path (exclusive with SystemID / tuning sweeps); each step dwells
// >5× any field L/R so lag doesn't bias the curve. Proposes SystemIDStabilizeAmps = 50% of limit-at-speed and
// SystemIDStepAmplitude = (duty@75% − duty@50%)/2, kept BELOW the knee (K near saturation reads low → over-aggressive Kp).
#define FIELDCURVE_MAX_PTS    192       // ramp samples (PSRAM); the scaled steps need 183 to reach 92% at the smallest fieldScale() (0.25)
#define FIELDCURVE_DUTY_START (5.0f * fieldScale())      // first step (%), 12 V value scaled to the winding
#define FIELDCURVE_DUTY_MAX   92.0f     // ramp ceiling (%) — stop here if amps never reach the limit
#define FIELDCURVE_DUTY_STEP  (2.0f * fieldScale())      // duty increment per step (%)
#define FIELDCURVE_ONSET_FINE_STEP (0.25f * fieldScale()) // onset sweep: fine step backing DOWN to refine the coarse knee (%)
#define FIELDCURVE_DWELL_MS   1500UL    // settle dwell per step (ms) — >5τ for any realistic field
#define FIELDCURVE_EASE_MS    1500.0f   // ease-in/out ramp between the operating duty and the sweep floor (ms)
struct FieldCurvePoint { float duty; float amps; };
FieldCurvePoint *fieldCurveBuf = nullptr;     // ps_malloc'd on first run, never freed
int   fieldCurveCount = 0;                     // points captured this run
volatile bool fieldCurveRequested = false;     // set by /get?startFieldCurve
volatile bool fieldCurveAbortRequested = false;// set by /get?cancelFieldCurve
uint8_t fieldCurveActive = 0;                  // 0=idle, 1=ramping, 2=processing (sent to UI)
bool  fieldCurveResultsReady = false;          // true when curve + proposals are ready to read
uint32_t fieldCurveLastEndMs = 0;              // cooldown guard
// Browser-minted run token; only an ACCEPTED start adopts it, so a refused start leaves the previous run's token and its
// latched results won't match the caller. 0 = caller sent none.
uint32_t fieldCurveRunTok = 0;
// Results (read via /fieldcurve.csv)
float fieldCurveKneeDuty   = -1.0f;            // duty at the saturation knee (%), -1 = none found
float fieldCurveKneeAmps   = -1.0f;
float fieldCurveTargetLimitA = 0.0f;           // table limit-at-speed used for the 50/75% targets (A)
float fieldCurvePropStabA  = 0.0f;             // proposed SystemIDStabilizeAmps (A)
float fieldCurvePropStepPct = 0.0f;            // proposed SystemIDStepAmplitude (% duty)
// Engine speed across the last ramp's settled dwells (/fieldcurve.json, advisory). avg = reference speed for downstream
// sweeps (loose ±500 RPM, never a gate). Reset at each start.
float fieldCurveRpmMin = 0.0f, fieldCurveRpmMax = 0.0f, fieldCurveRpmAvg = 0.0f;
bool  fieldCurveOk = false;                     // true if a usable linear region + proposals were found
bool  fieldCurveCeilingLimited = false;         // true if the ramp hit the field ceiling (ccDutyCeiling) BEFORE the amp target — curve truncated
uint8_t fieldCurveCeilBind = 0;                 // g_fieldCeilBind latched when the ramp stopped on the ceiling (/fieldcurve.json, /kneesweep.json "ceilBind")
                                                // Cleared at each sweep start.
// Field-curve / knee sweep abort reason (latched at the protection cut, cleared at each start; /fieldcurve.json & /kneesweep.json).
// FIRST cause wins: a hard cut corrupts the tach ~4.6 s, so a phantom RPM_TOO_LOW cut during teardown must not overwrite it.
volatile uint8_t fieldCurveAbortReason = 0;     // 0 = none / user-cancel; else FieldEventReason code
char (&fieldCurveAbortMsg)[96] = PsramPlace::make<char[96]>();             // human reason text at the cut ("" = none); must fit the 49-byte battery-temperature-wait reason
volatile uint8_t fieldCurveAbortFollowOn = 0;   // 2nd distinct cut reason in the same teardown (0 = none)
float fieldCurveAbortVolts = 0.0f;              // bus volts at the first cut
float fieldCurveAbortDuty  = 0.0f;              // commanded field % at the first cut

// ── Min% onset-knee sweep (commissioning) ────────────────────────────────────
// Field-curve ramp that STOPS when settled amps lift off baseline (onset knee), forcing almost no current. Run at 3 held RPMs
// DESCENDING, each warm-started; applyKneeCurve() fits onset = a + C/RPM into rpmMinDutyTable. Above the top anchor the floor is ZERO.
#define KNEE_ANCHOR_MAX 8
volatile bool kneeSweepRequested = false;       // set by /get?startKneeSweep
bool  fieldCurveOnsetMode = false;              // true = onset-stop sweep (Min%), false = saturation sweep (SystemID)
float kneeSweepKneeDuty = -1.0f;                // onset duty found this sweep (%), -1 = none/aborted
float kneeSweepRPM      = 0.0f;                 // RPM the sweep was held at
float kneeSweepTempF    = 0.0f;                 // alternator case temp at sweep (°F), for the kneeTempRefF normalization
bool  kneeSweepOk       = false;                // true = a clean onset was captured this sweep
float kneeAnchorRPM[KNEE_ANCHOR_MAX]  = {0};    // committed anchors (RPM, onset duty %, case temp °F)
float kneeAnchorDuty[KNEE_ANCHOR_MAX] = {0};
float kneeAnchorTempF[KNEE_ANCHOR_MAX] = {0};
int   kneeAnchorN = 0;                          // anchors committed so far
// Live preview of the onset = a + C/RPM fit over current anchors (/kneesweep.json) so review can flag a bad point before
// Apply. Valid once kneeAnchorN >= 2; -1 / -1 otherwise.
float kneeFitA         = 0.0f;                  // fitted intercept a (% duty, the RPM→∞ asymptote)
float kneeFitC         = 0.0f;                  // fitted slope C (% duty · RPM; onset = a + C/RPM)
float kneeFitResidPct  = -1.0f;                 // worst-anchor fit residual (% duty), -1 = not fitted
int   kneeFitWorstIdx  = -1;                    // anchor index (capture order) of the worst residual

// ── Gate-tuning capture: manual session logger + field sweeper ────────────────
// Sizes the admission gates from measured per-axis output sensitivity (ALT_GATE_TUNING_CAPTURE_SPEC.md). Code in
// 7_functions.ino; state declared here for the /get handlers (3_functions.ino) and duty-override chain (6_functions.ino).
volatile uint8_t altLogState   = 0;   // 0 IDLE, 1 RECORDING, 2 STOPPED, 3 FULL
uint32_t altLogRows            = 0;   // rows captured this session
float    altLogSecLeft         = 0;   // countdown: capacity remaining at the fixed 10 Hz rate (s)
// Buttons are REQUESTS consumed on the control loop (altLogService), never actions on the web task, so a Discard can
// never free the buffer under a row write.
volatile bool    altLogStartReq = false, altLogStopReq = false, altLogClearReq = false;
// Stimulus generator: duty ramp up and back down on the field-curve duty-override path (GOV_BYPASS_SLEW + MANUAL PID);
// up-and-down cancels field lag and decorrelates speed sag / case warming from the field axis.
uint8_t  altSweepActive        = 0;   // 0 idle, 1 ramping up, 2 ramping down, 3 easing out
// Only the constant-rate ramp: ease transitions move duty far faster, so their rows must not carry the sweep flag.
bool     altSweepMeasuredLeg   = false;
volatile bool altSweepRequested = false;      // set by /get?altSweepStart
volatile bool altSweepAbortRequested = false; // set by /get?altSweepCancel, and by the fast-OV cut
float    altSweepRatePctS      = 1.0f;        // ramp rate (% duty per second). 12 V value, x fieldScale() at boot and on a class or winding change, like the floor below
// Ramp turnaround margins (panel controls posted with the start); see altSweepLimitNow() in 7_functions.ino.
float    altSweepMarginA       = 10.0f;      // amps below every current limit
float    altSweepMarginV       = 0.20f;      // volts below every voltage limit (12V value; ×class/12 at boot and on class change).
                                             // Not 0.15: a >=1.2 V/s bank acceptance knee fired the hard cut ~1.5 s after turnaround
float    altSweepFromPct       = FIELDCURVE_DUTY_START;   // ramp floor (%)
float    altSweepToPct         = 0.0f;        // ramp ceiling (%); 0 = resolve to the live field ceiling at start
uint32_t altSweepLastEndMs     = 0;           // cooldown guard, same 2 s rule as the other field tests
uint32_t altSweepReqMs         = 0;           // millis() of the start request — it expires (see altSweep_tick)
bool     altSweepLimitRev      = false;       // this sweep turned around early, not at its commanded ceiling
uint8_t  altSweepRevWhy        = 0;           // 0 not turned early, 1 a control limit, 2 the Reverse now button
volatile bool altSweepReverseReq = false;     // set by /get?altSweepReverse — turn the up leg around now

// ── Field de-energize time-constant test (runs inside commissioning stage 7) ──
// Settles the REAL current loop at the stabilize level, freezes duty, steps the field to MinDuty (the floor a real OV clamp
// drives to) and fits the decay τ = L/R: the time OV protection's field drain takes. 20 kSPS fast channel (ADS CH1 fallback),
// calibrated against ADS at hold and tail. Drain is RPM-dependent, so the wizard cuts once per Min%-floor speed and fits
// drain-vs-RPM (fdDrain*). Ramp runs the normal AUTO path; hold/cut/ease share the SystemID override (OR'd into sysIDRunning).
#define FIELDCUT_BUF_SIZE   256      // ADS fallback decay samples (~10-30 ms CH1 cadence, PSRAM)
#define FIELDCUT_SETTLE_MS  3000UL   // measured current must sit in-band this long (after the setpoint slew arrives) before the hold starts
#define FIELDCUT_SETTLE_TIMEOUT_MS 30000UL  // never settles (e.g. target unreachable at idle) → proceed with what we have if it clears FIELDCUT_MIN_BASE_A
#define FIELDCUT_HOLD_MS    5000UL   // duty FROZEN this long before the cut — field at a constant, settled operating point; baseline + high calibration point averaged over the tail of this window
#define FIELDCUT_MEAS_MS    4000UL   // cut window: field at MinDuty. Decay is done in ~0.2 s (τ tens of ms); the window only needs to outlast it, then average the residual floor + low calibration point over the final 2.5 s.
#define FIELDCUT_SNAP_MS    400UL    // grab the 500 ms fast-channel ring this long after the cut → window spans ~[cut-100, cut+400] ms even with drain lag
#define FIELDCUT_EASE_MS    1500.0f  // ease duty back to the pre-test operating point on exit — gentle re-engagement can't trip a protection
#define FIELDCUT_MIN_BASE_A 5.0f     // baseline-above-floor must clear this or there is no decay to fit
#define FIELDCUT_RPM_GRACE_MS 6000UL // keep masking the RPM gates this long after a run: the cut starves the LM2907 pickup (~4.6 s of garbage while it re-biases). fieldCutRpm is latched during the HOLD, before the cut.
#define PROT_LOADDUMP_HOLD_MS   1500UL   // manual load-dump test: hold field collapsed at MinDuty this long before easing back
#define PROT_RECOVER_TIMEOUT_MS 30000UL  // manual ladder-walk test: abort if a cut never lands or its lockout never clears
#define FIELDCUT_FAST_N     10000    // one full fast-channel ring copy (500 ms @ 20 kSPS), int16 mV
#define FIELDCUT_PLOT_N     220      // decimated decay points served to the wizard plot
struct FieldCutSample { uint32_t tMs; float amps; };
FieldCutSample *fieldCutBuf = nullptr;          // ps_malloc'd on first run, never freed
int   fieldCutCount = 0;                          // ADS decay samples captured this run
volatile bool fieldCutRequested = false;          // set by /get?fieldCutStart
volatile bool fieldCutAbortRequested = false;     // set by /get?fieldCutCancel
uint8_t fieldCutActive = 0;                        // 0=idle 1=running 2=processing (sent to UI)
uint8_t fieldCutPhase = 0;                         // UI caption: 0=ramping 1=holding 2=cut/measuring 3=easing
bool  fieldCutCcActive = false;                    // ramp/settle phase: the normal AUTO path drives the real current PID at fieldCutCmdA
float fieldCutCmdA = 0.0f;                         // ramp-phase current target (= SystemIDStabilizeAmps at start)
bool  fieldCutResultsReady = false;
uint32_t fieldCutRunTok = 0;                       // run token, see fieldCurveRunTok
bool  fieldCutOk = false;                          // true = a usable τ was fit this run
float fieldCutTauMs   = -1.0f;                     // fitted decay τ this run (ms), -1 = none — QA cross-check, NOT the stored value
float fieldCutFallMs  = -1.0f;                     // measured 90%→10% fall time (ms), -1 = none
float fieldCutDrainMs = -1.0f;                     // measured command→10%-of-baseline drain time (ms) — model-free; the wizard fits drain-vs-RPM across the per-speed runs and stores the line (fdDrain*)
float fieldCutBaseA   = 0.0f;                      // steady output averaged over the frozen-duty hold (A) — the pre-cut amps reported per run
float fieldCutRpm     = 0.0f;                      // engine RPM averaged over the same hold tail — latched BEFORE the cut (the cut corrupts the tach ~4.6 s); the x-value of this run's drain point
float fieldCutFloorA  = 0.0f;                      // residual output at MinDuty, averaged over the tail (A) — the decay asymptote
float fieldCutResidPct = -1.0f;                    // log-fit RMS residual as % (quality; lower is better)
uint8_t fieldCutSrc = 0;                           // decay data source this run: 1 = 20 kSPS fast channel, 0 = ADS fallback
int16_t *fcFastBuf = nullptr;                      // PSRAM — decay ring copy (calibrated mV)
int      fcFastCount = 0;
float    fcSnapElapsedMs = 400.0f;                 // ms from the cut command to the fast-ring grab (FIELDCUT_SNAP_MS + one tick jitter); last decimated sample = the grab, so command instant = last-sample-time − this
int16_t *fcScratch = nullptr;                      // PSRAM — hold-end / tail-end ring copies for the calibration means
float *fcPlotMs = nullptr, *fcPlotA = nullptr;     // PSRAM — decimated decay served via /fieldcut.json (t relative to the cut command)
int    fcPlotN = 0;
uint32_t fieldCutLastEndMs = 0;                    // cooldown guard
char (&fieldCutAbortMsg)[96] = PsramPlace::make<char[96]>();                  // human reason text on a failed/aborted run; 48 truncated four of the fit-failure messages

// ── Charge health system calibration (commissioning stage 9) ──────────────────────────────
// Measures the alternator's speed lead (altLeadSec): with the field pinned, output answers where the engine is GOING, so up
// and down passes disagree; the delay that collapses the gap pairs output with speed in the health admission gate.
// Operator follows a clocked schedule (idle → CHC_TOP_FRAC × max working RPM → hold → idle) four times. Cycle 1 sizes the
// field AT THE TOP (fastest all run, so nothing later exceeds what sizing proved safe) and freezes duty; cycles 2-4 fit
// independently. Cycles chain on the clock with no button press: no phase pins the field while nobody is driving.
#define CHC_SAMP_MS        100u    // capture cadence. MUST equal EP_FEED_DT_MS — the answer is counted in these samples and applied as that many feed ticks
#define CHC_BUF_N          2600    // 260 s at CHC_SAMP_MS (PSRAM, 4 B/sample). Worst case: top 4000 (CHC_TOP_FRAC 0.50 x 8000 clamp) over idle 500 = 17.5 s ramps; a freeze at the start of the cycle-1 climb records climb + 8 s hold + descent + 3 x (2 ramps + 3 s) = 157 s
#define CHC_KMAX           9       // candidate delays searched, in CHC_SAMP_MS samples (0…0.9 s); altLeadSec clamps to 1.0 s
#define CHC_BIN_RPM        50.0f   // speed bin width for the up/down comparison
#define CHC_RATE_MIN       40.0f   // rpm/s below which a sample carries no direction information
#define CHC_MIN_SWING_RPM  300.0f  // a shorter pass never opens a gap worth fitting
#define CHC_MIN_BINS       3       // paired speed bins needed before a candidate delay is scored
#define CHC_TOP_FRAC       0.50f   // the test tops out at this share of maxWorkingRpm. MUST equal CHC_TOP_FRAC in script.js
#define CHC_PACE_RPM_S     200.0f  // schedule ramp pace (5 s per 1000 rpm). MUST equal CHC_PACE_RPM_S in script.js
#define CHC_SETTLE_IDLE_MS 10000u  // cycle 1 opens with this long at idle
#define CHC_SIZE_HOLD_MS   8000u   // cycle 1's top hold: the current loop settles and the duty is frozen inside this window
#define CHC_TOP_HOLD_MS    3000u   // top hold of the measured cycles — room to turn the throttle around
#define CHC_MIN_RAMP_MS    2000u
#define CHC_MEAS_CYCLES    3       // measured cycles after the sizing cycle, one independent fit each
#define CHC_TOL_FLOOR_RPM  250.0f  // "at the top" for the freeze = inside the chart's corridor cushion: max(floor, frac x top)…
#define CHC_TOL_FRAC       0.15f   // …MUST equal CHC_LANE_FLOOR_RPM / CHC_LANE_FRAC in script.js, or the screen says in range while the firmware waits
#define CHC_HOLD_ABOVE_IDLE 400.0f // the top must clear idle by this, or the passes have no range (checked at Start and on the setup screen)
#define CHC_SETTLE_MS      1200u   // current must sit inside the band this long before the duty is frozen
#define CHC_MIN_TEST_A     3.0f    // below this there is no output to measure a lead in
#define CHC_V_ABORT_MARGIN (0.02f * (float)SYSTEM_VOLTAGE_CLASS / 12.0f)   // take the field down entirely this far under the charge target (12 V value x class/12)
#define CHC_I_ABORT_FRAC   0.95f   // …and at this fraction of the HIGH column at the present speed
#define CHC_EASE_MS        1500.0f // ease the field between the frozen level and the pre-test point
#define CHC_LOHI_AH        201     // banks at or above this size are sized off the HIGH charge-rate column

int maxWorkingRpm = 0;                     // NVS: the engine's maximum working RPM as entered on the stage-9 setup screen; 0 = never entered
volatile bool chcRequested = false;        // /get?chcStart
volatile uint32_t chcReqMs = 0;            // when that press landed — the request expires after 30 s
volatile bool chcAbortRequested = false;   // /get?chcCancel, or a protection cut
uint8_t chcActive = 0;                     // 0 = idle, 1 = running
uint8_t chcPhase = 0;                      // reported to the wizard: 0 idle, 1 settle at idle, 2 sizing climb, 3 sizing hold,
                                           // 4 frozen (measured cycles), 6 easing out
uint8_t chcTestPct = 50;                   // share of the charge-rate column the test aims for (20…80, wizard-held)
bool  chcCcActive = false;                 // sizing cycle: the normal AUTO path drives the real current PID at chcCmdA
float chcCmdA = 0.0f;                      // …to this level — chcTestPct of the cap column at the present speed, never above the top's
float chcTargetA = 0.0f;                   // the level at the top (= chcSizeCapA × chcTestPct%)
float chcHoldDuty = 0.0f;                  // duty the current loop settled on, frozen for the measured cycles
float chcIdleRpm = 0.0f;                   // speed at Start — the bottom of every cycle
float chcTopRpm = 0.0f;                    // CHC_TOP_FRAC × maxWorkingRpm — the top of every cycle, and where the field is sized
uint32_t chcRampMs = 0;                    // one ramp of the schedule, (top − idle) at CHC_PACE_RPM_S
uint32_t chcRunMs = 0;                     // ms since the run's clock started — the browser draws its chart against this
uint8_t chcCycle = 0, chcSeg = 0;          // where the clock is: cycle 1…1+CHC_MEAS_CYCLES; segment 0 settle, 1 climb, 2 hold, 3 descent
bool  chcFrozen = false;                   // the duty has been captured — everything from here runs open-loop
float chcMinA = 0.0f, chcPeakA = 0.0f;     // output range actually seen across the measured cycles
float chcLeadSec = -1.0f;                  // the answer: mean of the closest pair of cycle fits (the browser recomputes and stores it)
float chcLead[3] = {-1.0f, -1.0f, -1.0f};  // one independent fit per measured cycle
float chcGapB[3] = {-1.0f, -1.0f, -1.0f};  // up/down disagreement per fit, uncorrected…
float chcGapA[3] = {-1.0f, -1.0f, -1.0f};  // …and at the fitted delay
// Throttle pace the operator delivered (|rate| >= CHC_RATE_MIN samples); reported for reference only.
float chcRateMin = -1.0f, chcRateMax = -1.0f, chcRateAvg = -1.0f;
bool  chcResultsReady = false, chcOk = false;
char (&chcAbortMsg)[128] = PsramPlace::make<char[128]>();               // 64 truncated four of its own messages mid-word
uint32_t chcLastEndMs = 0;
struct ChcSample { uint16_t rpm; int16_t amps10; };   // 4 B — amps ×10, so ±3200 A
static_assert(sizeof(ChcSample) == 4, "ChcSample lost its packing — the PSRAM budget assumes 4 B");
ChcSample *chcBuf = nullptr;               // PSRAM capture buffer, filled once per run (never wraps)
int chcCount = 0;
int chcCycStart[3] = {0,0,0}, chcCycEnd[3] = {0,0,0};   // sample span of each measured cycle — its fit window
float chcSizeCapA = 0.0f, chcGuardCapA = 0.0f;   // charge-rate column at the top (sizes the test) / HIGH column live (guards it)

// ── Manual protection-trigger test (Settings → Emergency & Troubleshooting) ────────────────
// Reproduces each field-collapse waveform for a controlled bench run: energizes via protTestCcActive to protTestCmdA on the
// real current loop, then fires the REAL executor. Set only from /get?protTestStart (gated AUTO / RPM / charging / no-other-test).
volatile bool protTestRequested = false;           // /get?protTestStart — pending start
volatile bool protTestAbortRequested = false;      // /get?protTestCancel
volatile bool protTestCutPending = false;          // mode 1/4: pre-gate fires applyImmediateCut(fast-OV) next tick
volatile bool protTestManualCutActive = false;     // mode 5: hold GPIO4 open (true hard cut) — gates the field-enable HIGH in AdjustField
uint8_t  protTestActive = 0;                        // 0=idle, 1=running (UI/guards)
uint8_t  protTestMode = 0;                          // 1=instant cut, 2=load-dump, 3=graceful ramp, 4=ladder walk, 5=manual bench cut
bool     protTestCcActive = false;                  // energize phase: real current loop drives field to protTestCmdA (modes 1-4 only)
float    protTestCmdA = 0.0f;                        // energize target alternator output current (A) — modes 1-4; 0 = auto-seed at fire
uint8_t  protTestReps = 1;                           // ladder-walk / manual repeat count
uint8_t  protTestRepDone = 0;                        // cuts fired so far (UI)
uint16_t protTestCutMs = 500;                        // mode 5: hard-cut hold time (ms)
uint16_t protTestGapMs = 1000;                       // mode 5: field-restored gap between repeated cuts (ms)
uint32_t protTestLastEndMs = 0;                      // cooldown guard
// Commissioned field DRAIN time (ms): command→10%-of-output (≈ dead-time + 2.3 L/R). Holds the WORST-CASE (longest) endpoint
// of the drain-vs-RPM line (fdDrain*), used when RPM is unknown and by the 3× post-protection backstops; a manual edit flattens
// the line. Name kept for NVS/CSV3 continuity (key `fieldDecayTau`, CSV3 slot 314). Safe placeholder default.
uint16_t fieldDecayTauMs = 60;
// Drain-vs-RPM line as its two TESTED endpoints (shifted to sit at/above every point). Lookup interpolates and CLAMPS outside
// the tested range — never extrapolates (an underestimate releases the OV clamp with the field still hot). Degenerate → flat
// fieldDecayTauMs (fdDrainMsAtRpm()).
uint16_t fdDrainLoMs  = 0;   // drain (ms) at fdDrainRpmLo
uint16_t fdDrainHiMs  = 0;   // drain (ms) at fdDrainRpmHi
uint16_t fdDrainRpmLo = 0;   // lowest tested RPM (idle point)
uint16_t fdDrainRpmHi = 0;   // highest tested RPM (max-working point)
float fdDrainMsAtRpm(float rpm);
// Fast-channel amps cal (NVS): ampsCal = faCalGain × nominal + faCalOffA. ESP32 ADC absolute accuracy is poor, so the
// field-decay test two-points it against the ADS1115 (hold ≈30 A, tail ≈0 A) each run. Defaults = pass-through.
float faCalGain = 1.0f;
float faCalOffA = 0.0f;

// ── Auto-commissioning state machine ──────────────────────────────────────────
// Persistent across reboots (NVS). 0=NOT_COMMISSIONED, 1=IN_PROGRESS, 2=COMMISSIONED.
// Phase 0 snapshots every setting the flow may write (positional CSV string in one
// NVS key) so an explicit abort reverts to the pre-commissioning tune.
uint8_t commissionState = 0;                    // mirrors NK_commissionState
uint8_t commissionPhase = 0;                    // mirrors NK_commissionPhase — current wizard phase (0=Prep…8=Stress test, 9=finished; moves backward on Back); drives the Commissioning tab checklist
// Per-stage completion bitmask: bit i = stage i done (0=Prep 1=Field curve 2=Plant fit 3=Verify 4=Disturbances 5=Thresholds
// 6=CV plant fit 7=Min% floor + Field decay 8=Stress test). COMMISSIONED needs only COMMISSION_REQUIRED_MASK = 0x0FF (8 optional).
uint16_t commissionDoneMask = 0;                // mirrors NK_commissionDoneMask
// Per-stage "set by hand" bitmask; with the done bit: measured (done,!manual), hand-marked (done,manual), skipped-outstanding
// (!done,manual), pending (!done,!manual). Cleared when a stage is properly measured or its done bit is cleared.
uint16_t commissionManualMask = 0;              // mirrors NK_commissionManualMask
volatile bool faCommissionGate = false;         // Phase 2: relax the matrix steadiness gate (RPM-in-bin, drift OK)

// Commissioning rest hold between steps: the open wizard pings every ~2 s; a stale ping (dialog closed / Wi-Fi dropped) drops
// the hold within COMMISSION_HEARTBEAT_TIMEOUT_MS. Floor and ramp scale × fieldScale().
volatile uint32_t lastCommissionHeartbeatMs = 0;        // millis() of the last dialog heartbeat (0 = stale/exited)
#define COMMISSION_REST_FLOOR_PCT 4.0f                  // 12 V-base rest hold floor (% duty); bench-tune knob
#define COMMISSION_REST_RAMP_PCT 5.0f                   // 12 V-base rest ramp rate (%/s)
#define COMMISSION_HEARTBEAT_TIMEOUT_MS 20000UL         // stale ping → drop the hold. Not 5 s: Download Logs blocks the browser main thread and hogs the async server, starving the 2 s ping, and the field ramped up mid-wizard

// Weather Mode Global Variables
float UVToday = 0.0;
float UVTomorrow = 0.0;
float UVDay2 = 0.0;
int SolarWatts = 660;                      // nominal solar array power in watts
float pKwHrToday;                          // predicted solar output in kw*hr for the rest of today, as of the last weather forecast download
float pKwHrTomorrow;                       // predicted solar output in kw*hr for all of tomorrow
float pKwHr2days;                          // predicted solar output in kw*hr for 2 days from now
const float MJ_TO_KWH_CONVERSION = 0.278;  // MJ/m²/day to kWh/m²/day
const float STC_IRRADIANCE = 1000.0f;      // Standard Test Conditions W/m²
float performanceRatio = .75;              // Real-world performance losses (typically 0.75)


unsigned long weatherLastUpdate = 0;
int weatherDataValid = 0;  // 0=false, 1=true
char (&weatherLastError)[64] = PsramPlace::make<char[64]>();

int weatherModeEnabled = 0;            // 0=disabled, 1=enabled
float UVThresholdHigh = 2.1;           // UV index above this = high solar expected (kWh)
int WeatherUpdateInterval = 21600000;  // Update every 6 hours (in milliseconds)
// A forecast this far past its refresh stops holding the field off (analyzeWeatherMode). Grace over the interval because the
// refetch is queued in the same tick, and releasing there blips the field on during every fetch.
const unsigned long WeatherStaleGraceMs = 1800000UL;  // 30 min
int WeatherTimeoutMs = 10000;          // HTTP timeout in milliseconds

int currentWeatherMode = 0;           // 0=normal, 1=high solar, 2=low solar
unsigned long nextWeatherUpdate = 0;  // When next update is due
time_t weatherFetchEpoch = 0;         // wall clock of the last successful forecast fetch (0 = none this boot) — tells the ledger which local day "tomorrow" meant
float   *wxHourWm2 = nullptr;         // ps_malloc'd in solarLedgerInit(): WX_HOURS hourly shortwave radiation, W/m² mean of the hour ENDING at wxHourT0 + k*3600 (Open-Meteo's convention)
time_t   wxHourT0  = 0;               // unix time of slot 0; 0 = no hourly forecast held. Refilled with the table under wxHourMux, so the ledger (Core 1) never reads a half-filled one
uint16_t wxHourN   = 0;               // slots filled
portMUX_TYPE wxHourMux = portMUX_INITIALIZER_UNLOCKED;   // the fetch (Core 0) writes the three above inside it; sledCaptureHourly (Core 1) reads inside it

// ── Solar energy ledger: predicted vs actual harvest + consumption, one record per LOCAL day ──
// Closes the loop on Defer to Solar: day X's forecast is frozen the evening before (what the overnight pause acted on); actual
// harvest and consumption are booked at midnight. actual/forecast drifts performanceRatio (solarLearnEnable); consumption
// history sizes the pause threshold (solarUseConsEnable). PSRAM ring + live day; flash only at field-cut-settled.
#define SLED_SIZE           90
#define SLED_PATH           "/solarledger.bin"
#define SLED_LIVE_PATH      "/solarlive.bin"
#define SLED_MAGIC          0x534C4547u   // 'SLEG'
#define SLED_VER            2u            // v2: hourly forecast + per-hour actual/qualification in the live day; learning-basis columns in the record
#define SLED_MIN_COVER_MIN  1200          // a day feeds the consumption prediction only if the device was awake >= 20 h of it (ratio learning is per hour, SLED_HOUR_MIN_SEEN)
#define SLED_RATIO_MIN      0.15f
#define SLED_RATIO_MAX      1.20f
#define SLED_LIVE_FLUSH_MS  1800000UL     // live-day state to flash at most every 30 min while the field is cut
#define SLED_PRED_WINDOW    30            // consumption prediction looks back at most this many ledger days
#define SLED_PRED_MEDIAN_N  7             // ...and takes the median of the newest N complete days (mean when fewer exist)
#define SLED_MIN_QUAL_WH    150           // the qualifying hours must promise at least this much (at ratio 1.0) before a day can ratio-learn
#define SLED_HOUR_MIN_SEEN  55            // minute samples an hour needs (of 60) to count as fully observed
#define WX_HOURS            72            // hourly forecast slots kept from a fetch: three local days
#define SLED_F_FORECAST 0x01   // predHarvKwh came from a real forecast
#define SLED_F_VEDIRECT 0x02   // VE.Direct frames were seen during the day
#define SLED_F_SHUNT    0x04   // battery shunt configured (consumption is measurable)
#define SLED_F_LEARNED  0x08   // this day moved performanceRatio
#define SLED_F_PARTIAL  0x10   // coverage below SLED_MIN_COVER_MIN — excluded from the consumption prediction (learning is per hour and judges each hour's own coverage)
#define SLED_F_SAMEDAY  0x20   // forecast only arrived after midnight (device booted late) — a full-day number, but not the one any overnight pause acted on
#define SLED_F_HOURLY   0x40   // an hourly forecast was captured for the day; learning needs it (per-hour actual vs forecast, battery-limited hours discarded)
#define SLED_F_LIVE     0x80   // CSV export only: the day still in progress
struct SolarLedgerRec {
  uint32_t dayIdx;        // local calendar day number: (epoch + sledTzOffsetS) / 86400, the ledger's latched offset (4_functions.ino)
  float predHarvKwh;      // forecast harvest at the ratio then in force (NAN = no forecast the evening before)
  float predIrrKwh;       // the same forecast at ratio 1.0 (irradiance x array watts) — the learning basis
  float actHarvKwh;       // VE.Direct panel-power integral over the day (NAN = no VE.Direct all day)
  float predConsKwh;      // consumption predicted for this day from ledger history (NAN = no history yet)
  float actConsKwh;       // house loads = alternator + solar + battery discharge - battery charge (NAN = not measurable)
  float altKwh;           // alternator share of the day's sources
  float ratioAfter;       // performanceRatio after this day's learning step (NAN = day did not learn)
  float qualPredKwh;      // forecast over the qualifying hours at ratio 1.0 = the learning basis (NAN = no hourly forecast)
  float qualActKwh;       // panel-power integral over those same hours
  uint32_t qualHours;     // bit h set = local hour h qualified: MPPT freely tracking all hour, device awake all hour, forecast held
  uint16_t coverageMin;   // minutes of the day the device was awake and accumulating
  uint8_t  flags;         // SLED_F_*
  uint8_t  pad;
};
struct SolarLedgerLive {  // the day in progress — persisted so a reboot resumes it instead of losing the day
  uint32_t dayIdx;        // 0 = no day open (clock never valid)
  double baseSolarWh, baseAltWh, baseChgWh, baseDischgWh;   // *_AllTime counters at day open
  float predHarvKwh, predIrrKwh, predConsKwh;
  uint16_t coverageMin;
  uint8_t  flags;
  int8_t   curHour;        // local hour being accumulated (-1 = none yet this day)
  double   hourBaseWh;     // SolarChargedEnergy_AllTime when curHour began
  uint32_t hourFcMask;     // bit h = hourFcWh[h] holds a real forecast
  uint16_t hourFcWh[24];   // forecast Wh per local hour at ratio 1.0 (W/m² x array watts / 1000)
  uint16_t hourActWh[24];  // panel-power integral per local hour, finalised when the hour ends
  uint8_t  hourSeen[24];   // minute samples taken in the hour
  uint8_t  hourBad[24];    // 1 Hz samples where the MPPT was not freely tracking or VE.Direct was silent; one disqualifies the hour
};
SolarLedgerRec *sledRing = nullptr;   // ps_malloc'd in solarLedgerInit()
uint16_t sledHead = 0;                // next write slot
uint16_t sledCount = 0;               // 0..SLED_SIZE
SolarLedgerLive &sledLive = PsramPlace::make<SolarLedgerLive>();
bool sledDirty = false;               // ring changed (day closed / cleared) — flush at field-cut
bool sledLiveDirty = false;           // live-day state changed — flush at field-cut, rate-limited
bool sledRatioDirty = false;          // learned performanceRatio awaiting its NVS write (field-cut only)
float sledNeedKwh = 0.0f;             // pause threshold actually in force (manual or consumption-sized)
int   sledNeedSource = 0;             // 0 = High Solar Threshold setting, 1 = predicted consumption + margin
int   solarLearnEnable = 1;           // learn performanceRatio from each complete day's actual/forecast harvest
int   solarUseConsEnable = 1;         // size the pause threshold from predicted consumption (falls back to the setting until history exists)
float solarConsMarginPct = 25.0f;     // headroom over predicted consumption before a day counts as "solar covers it"
float solarLearnRatePct = 10.0f;      // per-day blend weight of the day's ratio into performanceRatio (slow by design)

//Input Settings
float uTargetAmps = 3;                           // the one that gets used as the real target

// Charge-rate Hi->Lo ceiling slew (AdjustField): the ceiling glides down instead of stepping, so iExcess never sees a false
// measured-vs-command excess. Armed by applyChargeRateMode() on any Hi->Lo change; self-clears once the lower cap is reached.
float modeCapSlew = 0.0f;          // imposed ceiling during the down-glide (A)
bool modeCapSlewActive = false;    // true while a Hi->Lo glide is in progress
uint32_t modeCapSlewEndMs = 0;     // millis() the glide self-cleared; iExcess stays suppressed for
                                   // MODE_CAP_GLIDE_GRACE_MS after, to cover the field-current settling tail
#define MODE_CAP_GLIDE_SEC 2.5f      // full-scale Hi->Lo glide time (s); rate = MaxTableValue / this
#define MODE_CAP_GLIDE_GRACE_MS 1000 // iExcess suppression hold AFTER the glide ends (ms)

float FloatVoltage = 13.6;
float BulkVoltage = 13.9;                        // this could have been called Target Bulk Voltage to be more clear
float ChargingVoltageTarget = 0;                 // This becomes active target — the SLEWED value the CV loop AND the OV
                                                 // protections read (slewed from ChargingVoltageTargetReq in AdjustFieldLearnMode).
float ChargingVoltageTargetReq = 0;              // Instantaneous DESIRED target (set each tick by stage logic /
                                                 // TargetVoltageMode / MaintainMode). ChargingVoltageTarget ramps to it.
float VoltageHardwareLimit = 14.3f;  // boot placeholder; NVS setting loaded in InitSystemSettings, programs the INA228 ALERT comparator (updateINA228OvervoltageThreshold). Top OV rung: ABOVE the software instant cut (AlternatorHardShutdownV), 0.2 V x class/12 below the BMS charge-disconnect voltage.
bool inBulkStage = true;

// SYSTEM_VOLTAGE_CLASS (Vessel Info) is the sole class truth: CV gain normalization, volt-domain thresholds and the boot SoC OCV
// brackets scale by it, and a class change (applyNominalVoltageChange) rescales the charge profile + whole OV ladder (tier margins,
// software cut, INA228 limit). The field side scales by fieldScale().

enum ChargeStageDisplayCode : uint8_t {
  CHARGE_STAGE_NONE = 0,
  CHARGE_STAGE_BULK = 1,
  CHARGE_STAGE_ABSORPTION = 2,
  CHARGE_STAGE_FLOAT = 3,
  CHARGE_STAGE_MANUAL = 4,
  CHARGE_STAGE_MAINTAIN = 5,
  CHARGE_STAGE_TARGET_V = 6,
  CHARGE_STAGE_IDLE = 7,
  CHARGE_STAGE_COMMISSION = 8   // wizard dialog open (commissionHeartbeat fresh) — outranks all charge stages
};
uint8_t chargeStageDisplay = 0;
bool idleHoldActive = false;   // tick.idleHold, kept for getChargeStageDisplayCode(): the IDLE stage holds the field off, so "field active" never sees it

// ====== Charger algorithm add-ons (no safeties here) ======

// SoC availability (you'll decide when to assert this)
bool socInfoAvailable = false;

// Tail-current based bulk completion
float TailCurrent_A = 10.0f;
uint32_t bulkVoltageHoldTimer = 0;  // millis() timestamp, 0 = inactive

// Float->Bulk (rebulk) based on sag + debounce
float RebulkVoltage = 13.1f;
uint32_t rebulkDebounceTime = 10UL * 1000UL;  // ms
uint32_t rebulkTimer = 0;                     // millis() timestamp, 0 = inactive

// Optional: don't allow immediate rebulk right after entering float
uint32_t MinFloatTime = 5UL * 60UL * 1000UL;  // ms

// SoC-based refinements (same scaling as SOC_percent: percent*100)
int SOC_BlockRebulk_percent = 90;
int SOC_AllowRebulk_percent = 80;

uint32_t FLOAT_DURATION = 8 * 3600;  // 8 hours in seconds
uint32_t floatStartTime = 0;

float RebulkCurrent_A = 10.0f;  // net discharge current threshold to trigger rebulk
bool inIdleStage = false;      // true when UseFloat=0 and absorption complete, waiting for rebulk
int UseFloat = 0;              // post-absorption mode: 0 = idle until rebulk criteria met, 1 = hold FloatVoltage, 2 = zero-current float (alternator carries house loads, battery held at 0 A)
bool zeroFloatActive = false;  // live: in the float stage with UseFloat=2 — zero-current regulation (MaintainMode's control law, entered by the stage machine with rebulk criteria still armed)


volatile bool inAbsorptionStage = false;
uint32_t absorptionStartTime = 0;
float AbsorptionVoltage = 13.9f;
uint32_t AbsorptionTimeoutMs = 2700000UL;   //
uint32_t absorptionCompleteTime = 30000UL;  // tail current hold before float
uint32_t absorptionTailTimer = 0;
uint32_t bulkVoltageHoldMs = 250;  // time at bulk voltage before entering absorption
volatile bool restartChargeCycleRequested = false;  // web /get?RestartChargeCycle=1 sets it; consumed at the top of updateChargingStage() (control-loop task) so the stage flags are only mutated from one task

// No FieldAdjustmentInterval: the field loop is free-running (AdjustFieldLearnMode() every loop pass; inner PID on fresh ADS CH1
// samples ~13 ms apart, up to ~30 ms). Its NVS key NK_FieldAdjustmentInterval and CSV3 slot are retired — never reuse.
float TemperatureLimitF = 175;       // measured at the case probe; internal/metal temps run roughly +40 to +50 °F above this depending on sensor installation. Strategy rationale: docs Charging Strategy page
// Cold-charge lockout: acts on the measured battery temperature (batteryTempF(), sources 1..4) and
// fails open when none qualifies. Default ON (lithium); lead-acid can opt out.
bool  coldChargeLockoutEnable = true;    // master on/off for the cold-charge lockout (default ON)
float MinChargeTempF = 40.0f;            // battery-temperature floor below which charging is locked out (°F)
const float ColdChargeHysteresisF = 2.0f;// °F rise above MinChargeTempF before charging re-arms (anti-chatter)
// Hot-charge lockout, mirror of the cold one. Acts only on a measured battery temperature (source 1..4).
// Spec: Working Markdown Docs/BATTERY_TEMP_SENSORS_SPEC.md §5.
int   hotChargeLockoutEnable = 0;        // master on/off for the hot-charge lockout (default OFF)
float MaxChargeTempF = 122.0f;           // battery-temperature ceiling above which charging is locked out (°F)
const float HotChargeHysteresisF = 2.0f; // °F fall below MaxChargeTempF before charging re-arms (anti-chatter)
// Battery + extra temperature probes (multi-drop 1-Wire) and the battery-temperature source chain.
// Spec: Working Markdown Docs/BATTERY_TEMP_SENSORS_SPEC.md. batteryTempF() (6_functions.ino) picks the
// source; buildTickSnapshot mirrors its result into battTempActiveF/battTempActiveSrc once per tick.
int   battTempProbeEnable = 0;      // 1 = the BATT-role DS18B20 feeds BatteryTempProbeF + IDX_BATT_TEMP_PROBE
int   extraTempProbeEnable = 0;     // 1 = the EXTRA-role DS18B20 feeds ExtraTempF + IDX_EXTRA_TEMP
int   battTempSource = 0;           // 0 Auto (probe, NMEA 2000, VE.Direct, RV-C), 1 Probe, 2 NMEA 2000, 3 VE.Direct, 4 RV-C, 6 None. 5 (board) retired; a stored 5 loads as Auto
int   extraTempAlarmHiEnable = 0;   // EXTRA-probe high alarm on/off
float extraTempAlarmHiF = 150.0f;   // EXTRA-probe high alarm threshold (°F)
int   extraTempAlarmLoEnable = 0;   // EXTRA-probe low alarm on/off
float extraTempAlarmLoF = 32.0f;    // EXTRA-probe low alarm threshold (°F)
float BatteryTempProbeF = NAN;      // BATT-role probe (°F); written by TempTask (and demoReadSensors in demo mode), NAN until the first good read
float ExtraTempF = NAN;             // EXTRA-role probe (°F); written by TempTask only
float batteryTempF(uint8_t *srcOut);   // 6_functions.ino — callers earlier in the concatenation order (the SoC seed) need this
float battTempActiveF = NAN;        // batteryTempF() result of the current tick (°F); NAN = no source qualifies
uint8_t battTempActiveSrc = 0;      // 0 none, 1 probe, 2 NMEA 2000 (127508), 3 VE.Direct (T), 4 RV-C (DC_SOURCE_STATUS_2); 5 retired, never emitted
float VictronBattTempF = NAN;       // VE.Direct "T" field (°F), BMV/SmartShunt with a temperature sensor
float VictronSoc = NAN;             // VE.Direct "SOC" field (%), the battery monitor's own count; NAN while it reports "---" (unsynced)
float rvcRxBattTempF = NAN;         // RV-C DC_SOURCE_STATUS_2 source temperature (°F) at the dvccInst instance
bool  cvTempDerateInert = false;    // CommissionTempSrc is not a measured source (legacy 0, or the retired board stamp 5); derate returns 1.0 until the CV fit is re-run
int ManualFieldToggle = 0;           // 0 = Auto — fresh-flash default. Set to 1 for Manual (debugging).
int PhysicalPanelOverride = 0;       // 0 (default) = the app owns HiLow and MaintainMode and the Cable 3 panel wires only report; 1 = the panel wires own both modes and the app toggles go inert
int MaintainMode = 0;                // Set to 1 to target 0 amps at battery
int TargetVoltageMode = 0;
float TargetVoltageSetpoint = 13.5f;
int OnOff = 0;             // 0 is charger off, 1 is charger On (corresponds to Alternator Enable in Basic Settings)
int Ignition = 0;          // Digital Input      NEED THIS TO HAVE WIFI ON , FOR NOW
int IgnitionOverride = 2;  // Auto (any value != 1) = follow real GPIO1 ignition wire (default); 1 = force ON. Cannot force off.
int HiLow = 1;             // 0 will be a low setting, 1 a high setting
// ── Switch panel inputs (data Cable 3 / connector RJ3) ────────────────────────────────────────
// RJ3 pin 5 = HIGH/LOW -> R47 -> opto U10 -> GPIO39 (ON = High rate); pin 7 = FORCEFLOAT -> R76/R52 -> opto U17 -> GPIO40 (ON = Maintain).
// Inverting optos like ignition on GPIO1: asserted == !digitalRead(pin). Pin 3 (EXTRA_OPTICAL_GPIO1 -> GPIO8) is unused.
// Always read and reported; PhysicalPanelOverride alone decides whether they steer anything (resolve*Mode, 6_functions.ino).
const int panelHiRatePin = 39;
const int panelForceFloatPin = 40;
bool panelHiRateInput = false;      // debounced HIGH/LOW input: true = voltage present at Cable 3 pin 5, i.e. switch ON = High rate
bool panelForceFloatInput = false;  // debounced FORCEFLOAT input: true = voltage present at Cable 3 pin 7, i.e. switch ON = Maintain
// HiLow / MaintainMode are the EFFECTIVE modes; these hold the app toggle's last choice (also what NVS carries), so switching the
// panel override back off restores it exactly.
int HiLowUserSel = 1;
int MaintainModeUserSel = 0;
int AmpSensorRange = 1;    // 0=±200A, 1=±300A (default), 2=±500A — hall effect sensor range
int LimpHome = 0;          // 1 will set to limp home mode, whatever that gets set up to be
int resolution = 12;       // for OneWire temp sensor measurement
int VeData = 0;            // Set to 1 if VE serial data exists
int NMEA0183Data = 0;      // Master switch for the NMEA 0183 receiver (only 1 lets the timed loop call ReadNMEA0183Data); off clears any held 0183 heading, since nothing else would age it out
// ── NMEA 0183 receive (Serial2 / GPIO6, its own opto channel — nothing else on the board touches that pin) ──
int NMEA0183Baud = 19200;         // 4800 = standard 0183 talker, 9600, 19200 = YachtDevices combined out, 38400 = AIS/HS
volatile bool n183ReconfigPending = false;  // set by the /get handler on the web task; applyNMEA0183Serial() runs on Core 1 instead, because it deletes the UART driver the loop reader is inside
int NMEA0183Invert = 0;           // UART invert flag. 0 suits an RS-232-level talker: its mark idles negative, the opto LED
                                  // stays dark and the 13k pull-up holds the line high. 1 suits a TTL-level talker, which
                                  // idles high, lights the LED and pulls the open collector low — same case as VE.Direct.
float n183HeadingDeg = -1.0f;     // last heading decoded from HDT/THS/HDG/HDM; -1 = nothing decoded yet
uint8_t n183HdgRef = 0;           // reference frame of n183HeadingDeg: 0 none, 1 magnetic, 2 true. Never conflate the two.
uint32_t n183SentenceCount = 0;   // checksum-valid sentences of ANY type — proves the wiring even for talkers we don't decode
uint32_t n183ChecksumErrCount = 0;  // failed checksum: right baud but wrong polarity/noise usually shows up here first
void n183ClearHeading();          // defined in 5_functions; the master-switch handler in 3_functions calls it
// ── HARD OVER-CURRENT PROTECTION ─────────────────────────────
// "Group 0" in UI = hardware overcurrent trip; HardOCEnable is its group toggle (ignores the global protections flag)
float HardOCTripAmps = 160.0f;   // derived: MaxTableValue + 10A — recomputed at boot and on MaxTableValue change, not persisted
uint32_t HardOCDebounceMs = 40;  // user-adjustable, persisted in NVS (provisioned devices keep their stored value). 40 ms is free noise robustness: a BMS charge-overcurrent trip holds >= 0.5 s, so nothing races this debounce
bool HardOCEnable = true;        // Group 0 enable — gates the last-resort trip; survives the global protections toggle, so OFF removes the final over-current backstop
uint32_t hardOCStartMs = 0;
const int pwmPin = 14;  // field PWM pin
//const int pwmChannel = 0;      //0–7 available for high-speed PWM  (ESP32)
const int pwmResolution = 12;          // Error = +0.010%    PWM Resolution = ±0.024% (1/4096)
float SwitchingFrequency = 400;      // Field switching frequency (Hz). Not 19 kHz: field edges couple into the DS18B20 OneWire line (~5 CRC fails/min). The P-type bootstrap chop below ~5kHz is benign (FIELD_DRIVE_BOOTSTRAP_DROOP.md). Ceiling 19455Hz (LEDC 12-bit divider; higher kills boot attach). Efficiency flat 100Hz-19kHz.
const int MIN_SAFE_FREQUENCY = 50;     // Above most audible issues // set to 2000 later
const int MAX_SAFE_FREQUENCY = 25000;  // Below core loss and EMI concerns
// REAL per-bus field-duty cap, class-invariant (gate-driver bootstrap limit + user knob; 99 = full duty). Per-class field-current
// protection is MaxFieldVolts; setDutyPercent() clamps the open-loop paths (manual/limp/fault) to it too.
float MaxDuty = 99.0;
// Field ceiling in physical field volts, typed from the datasheet (24/48 V alternators often carry a 12 V winding). Default
// 1.25 × fieldWindApplied, NEVER class-scaled (the winding is the alternator's, not the bank's). ccDutyCeiling() enforces whichever
// of this and MaxDuty binds first; solved against the MEASURED bus each tick so duty returns as the bank sags. Thermal (I²R) limit
// only: the winding sees full bus volts each on-edge. Duty × battery V model; true field V runs above it by the cable drop.
float MaxFieldVolts = 15.0;
float MinDuty = 1.0;       // field-duty floor: inner-PID outMin + soft-clamp decay floor. Compile-time default only — overwritten from NVS (seeded x fieldScale()) at boot; per-RPM onset floor lives in rpmMinDutyTable.
float ManualDutyTarget = 4.0f;  // manual-mode field duty %, 0.01 resolution (open-loop plant tests need sub-1% steps). 12 V value, seeded x fieldScale()
int InvertAltAmps = 0;     // change sign of alternator amp reading
int InvertBattAmps = 0;    // change sign of battery amp reading
int BatteryShuntPresent = 1;  // 1 = INA228 battery shunt fitted (default). 0 = no battery-current sensor: Bcur meaningless, every battery-current consumer takes its safe/off branch and the UI hides those features.
int ShuntLead = 0;         // battery lead the shunt sits in: 0 not set, 1 negative (ground on its load side: IBV reads high by I*Rshunt, corrected at the INA228 read),
                           // 2 positive (no correction; correcting it would read low = overcharge)
uint32_t Freq = 0;         // ESP32 switching Frequency in case we want to report it for debugging

//Variables to store measurements
float ShuntVoltage_mV;                        // Battery shunt voltage from INA228
float Bcur;                                   // battery shunt current from INA228
float targetCurrent;                          // This is used in the field adjustment loop, gets set to the desired source of current info (ie battery shunt, alt hall sensor, victron, etc.)
float IBV;                                    // Ina 228 battery voltage
float IBVMax = 6;                             // used to track maximum battery voltage (NVS-persisted, shown on dashboard)
float dutyCycle;                              // Field outout %--- this is just what's transmitted over Wifi (case sensitive)
float FieldResistance = NAN;                  // Field resistance in ohms, usually 2 to 6, changes 10-20% with temp. NAN = never measured: the 10 A ceiling then holds the field off, unless FieldResDefault applies
float FieldResTrefC = NAN;                    // alternator temperature (°C) recorded when FieldResistance was last written; NAN = never recorded (the math then assumes 30 °C)
bool FieldResDefault = false;                 // the owner skipped the test for FD_SKIP_R_OHM (12 V with no battery shunt only); counts only while nothing is measured and the class is 12 V (fieldResDefaultInUse)
// Field resistance a 12 V owner with no battery shunt may take instead of the test. Used flat, with no temperature credit and no
// FD_MARGIN: then the 10 A ceiling stays above 124 % duty up to 16 V and the heat law trims only above a 90 C INA228 die.
#define FD_SKIP_R_OHM 2.0f

// Field resistance test (BATTERY_WIRING_AND_SHUNT_SPEC.md Addendum B1): engine off, field held at a few voltages, current from the
// battery-current step (shunt) or the owner's meter. Here because 6_, 5_ and 3_functions.ino all use it.
#define FRT_IDLE     0
#define FRT_RUNNING  1
#define FRT_DONE     2
#define FRT_FAILED   3
#define FRT_SHUNT    1
#define FRT_METER    2
#define FRT_H_PROBE  0   // holds, in run order
#define FRT_H_AIM    1   // between the probe and the top, so the top is aimed by a slope (skipped when there is no room)
#define FRT_H_TOP    2
#define FRT_H_MID    3
#define FRT_N_HOLDS  4
#define FRT_MP_HOLD  1   // meter phases: field on, counting down
#define FRT_MP_WAIT  2   //               field off, waiting for the owner's reading
#define FRT_PHASE_MS      3000UL   // shunt: one field-off or field-on phase
#define FRT_SETTLE_MS     1000UL   // discarded at the start of a phase: the coil's L/R is tenths of a second
#define FRT_METER_HOLD_MS 12000UL  // meter: field on per hold; long enough to read the meter
#define FRT_HB_MS         8000UL   // no request from the app this long ends the test. Not 3 s: on this link a lost reply is resent 2-2.5 s later, a second loss 5 s after that
#define FRT_MAX_A         6.0f     // shunt: implied field current, FRT_RING_N-read mean, that ends the test at once
#define FRT_FAST_MAX_A    9.0f     // shunt: the same on the last 8 reads (~50 ms), for a fault rather than a plan that lands high
#define FRT_RING_N        64       // ~0.4 s of INA228 fast-mode reads
#define FRT_TOP_AIM_A     4.0f
#define FRT_AIM_A         2.0f     // the aiming hold, planned by the probe's V / A
#define FRT_AIM_SKIP_A    3.0f     // a probe at or above this plans the top itself: there the contact drop moves it little
#define FRT_MID_FRAC      0.6f
#define FRT_PROBE_MIN_A   0.5f     // a probe under this moves up a rung
#define FRT_NO_STEP_A     0.020f   // shunt: a battery step under this at the 6 V probe means the shunt does not see the field
#define FRT_METER_MIN_A   0.05f    // every real winding draws more than this at 3 V
#define FRT_MAX_CYC       5        // shunt: cycles per hold, three plus up to two retakes of a dropped cycle
#define FRT_DRIFT_A       0.050f   // shunt: a cycle's two field-off readings may differ by this or FRT_DRIFT_FRAC of the step
#define FRT_DRIFT_FRAC    0.03f    // not 15 %: 0.19-0.28 A drift on a wandering solar charge passed at the top hold; a steady run stays under 0.04 A
#define FRT_CALM_MS       45000UL  // shunt: two opening field-off readings must agree within FRT_DRIFT_A by this, or the test stops
#define FRT_CHARGING_A    0.30f    // shunt: a field-off reading above this (charging, engine off) names a charger in the drift stop
struct FrtCycle {
  float a0, b, a1;   // shunt: battery amps (charging positive) of the field-off, field-on and field-off phases
  bool ok;           // the two field-off readings agree, so no load switched mid-cycle
  float f;           // field amps, step / duty: the screen shows this, so a hold and its median cycle read the same
};
struct FrtHold {
  float x;   // field volts by the firmware's model, d*V - (1-d)*FD_VF_EST, at the V measured during the hold
  float d;   // duty %
  float v;   // battery volts during the hold
  float i;   // field amps measured
  float tC;  // alternator temperature (C) when the hold's amps came in, NAN with no probe reading: shown per hold
  FrtCycle c[FRT_MAX_CYC];   // shunt: every cycle of the hold, kept or dropped
  uint8_t nc;
};
struct FieldResTestState {
  volatile uint8_t state;      // FRT_IDLE / RUNNING / DONE / FAILED
  uint8_t method;              // FRT_SHUNT / FRT_METER
  uint8_t hold;                // FRT_H_* in progress
  uint8_t rung;                // probe rung 0..2 (3 / 6 / 12 V)
  float rungX[2], rungI[2];    // a rung the probe stepped up from: its field volts and amps
  uint8_t phase;               // shunt 0..2 * FRT_MAX_CYC (even field off, odd field on); meter FRT_MP_*
  uint32_t phaseStartMs;
  uint32_t startMs;
  uint32_t endMs;              // last end of a run (0 = none since boot): the tach-mask tail runs from here
  volatile uint32_t lastPollMs;   // last /fieldResTest request: the app's heartbeat
  volatile uint32_t hbWorstMs;    // longest gap between requests this run, for the console
  float duty;                  // % applied this pass (0 in a field-off phase)
  float holdX, holdDuty;       // the hold in progress
  double sumI, sumV;           // sample window of the phase in progress (after the settle)
  uint32_t n;
  float phI[2 * FRT_MAX_CYC + 1], phV[2 * FRT_MAX_CYC + 1];   // shunt phase means: battery amps (charging positive) and volts
  bool calm;                   // shunt: two opening field-off readings agreed, so the field may come on
  float gateA;                 // shunt: the last opening field-off reading, for that agreement (NAN: none yet)
  float baseA;                 // last field-off phase mean: the live 6 A check's baseline
  float ring[FRT_RING_N];
  double ringSum;
  uint8_t ringN, ringK;
  bool overA;                  // the live check saw more than FRT_MAX_A (or FRT_FAST_MAX_A)
  float tripA, tripLim;        // the reading that tripped it, and the limit it passed
  float liveI;                 // latest implied field amps (shunt), FRT_RING_N-read mean, for the screen
  float lvLo, lvHi, fsLo, fsHi;   // this on-phase's range after the settle: liveI, and the 8-read mean; logged per cycle
  FrtHold h[FRT_N_HOLDS];
  float probeC;                // alternator temperature at the start (NAN: no reading), stored with R
  float r, vc, probeOffV;
  volatile bool saved;         // written by /fieldResTest save=1, the owner's Next or Save
  volatile bool reqStart, reqAbort, reqAgain, reqReading;
  volatile uint8_t reqMethod;
  volatile float reqAmps;
  char msg[320];               // failure reason, or the result line
};
FieldResTestState frt = {};
// The field PWM reaches the tach pickup through the stator, so with the engine stopped (a test precondition) anything
// the tach reads during a test and the LM2907 re-bias tail after it is coupling, not rotation.
inline bool frtTachMasked() {
  return frt.state == FRT_RUNNING || (frt.endMs != 0 && (uint32_t)(millis() - frt.endMs) < FIELDCUT_RPM_GRACE_MS);
}
void frtInaSample(float bcur, float v, bool shuntOk);
void fieldResRecord(float r, float probeC, bool typed);
int FieldWiring = 0;                          // field wiring the drive-heat limit checks: 0 not set (both checks), 1 P-type (D5 spreader), 2 N-type (J2 connector)
float vvout;                                  // Calculated field volts (approximate)
float iiout;                                  // Calculated field amps (approximate)
volatile float AlternatorTemperatureF = NAN;
float MaxAlternatorTemperatureF = 0;          // maximum alternator temperature
// === Thermistor Stuff
float R_fixed = 10000.0;               // Series resistor in ohms
float Beta = 3380.0;                   // Thermistor Beta constant — default matches Murata NXFT15XH103FA2B050 (β25/50 = 3380K)
float R0 = 10000.0;                    // Thermistor resistance at T0
float T0_C = 25.0;                     // Reference temp in Celsius
int TempSource = 0;                    // 0 for OneWire default, 1 for Thermistor
int temperatureThermistor = -99;       // thermistor reading
float MaxTemperatureThermistor = -99;  // maximum thermistor temperature (on alternator)
float TempToUse = NAN;                 // gets set to temperatureThermistor or AlternatorTemperatureF
TaskHandle_t tempTaskHandle = NULL;    // separate CPU task for temp reading (vestigial: reads are non-blocking)
float VictronVoltage = 0;              // battery V reading from VeDirect
float VictronCurrent = 0;              // battery Current (careful, can also be solar current if hooked up to solar charge controller not BMV712)
float HeadingNMEA = 0;                 // Just here to test NMEA functionality
int8_t HeadingRefNMEA = -1;            // PGN 127250 reference frame: 0 = true, 1 = magnetic, -1 = unknown
float HeadingVariationDeg = NAN;       // PGN 127250 variation, degrees east-positive; NAN = not provided
float COGNMEA = 0;                     // Course Over Ground from NMEA2K (degrees)
float SOGNMEA = 0;                     // Speed Over Ground from NMEA2K (knots)
float STWNMEA = NAN;                   // Speed Through Water (SOW) from NMEA2K PGN 128259 (knots); NAN = no log
double LatitudeNMEA = 0;               // from own ship AIS FIX LATER
double LongitudeNMEA = 0;              // from own ship AIS FIX LATER
float ApparentWindSpeedNMEA = NAN;     // Apparent wind speed from NMEA2K (knots); NAN = no fresh reading (calculateDerivedMetrics)
float ApparentWindAngleNMEA = NAN;     // Apparent wind angle from NMEA2K (degrees); NAN = no fresh reading
int NMEA2KData = 0;                    //
int NMEA2KVerbose = 0;                 // print stuff to serial monitor or not
int SatelliteCountNMEA = 0;            // Number of satellites
#define GPS_SMOOTHING_SAMPLES 5
double latBuffer[GPS_SMOOTHING_SAMPLES] = { 0 };
double lonBuffer[GPS_SMOOTHING_SAMPLES] = { 0 };
int gpsBufferIndex = 0;
int gpsBufferCount = 0;

// Calculated navigation metrics
float TrueWindSpeedNMEA = NAN;  // True wind speed (knots)
float TrueWindAngleNMEA = NAN;  // True wind angle (degrees)
float LeewayNMEA = NAN;         // Leeway angle (degrees, + = drift to starboard)
float VMGNMEA = NAN;            // VMG toward the manual target bearing (knots)
float VMGUpwind = NAN;          // VMG to windward = SOG·cos(TWA), knots — always computed (no mode toggle)
float VMGTargetBearing = -1;    // User-set target bearing for VMG (-1 = not set)
float sustainedTWS = NAN;       // 2-min running avg of TrueWindSpeedNMEA (knots); basis for Beaufort + gale (not instantaneous)
float currentGaleMinutes = 0;   // live: minutes continuously in a gale (sustainedTWS >= 34 kt), 0 when not
// Duty cycle tracking
//float IgnitionDutyCycle = 0;       // % of time ignition is on
//float EngineRunningDutyCycle = 0;  // % of time engine RPM > 100

// ADS1115
int16_t Raw = 0;
float Channel0V, Channel1V, Channel2V, Channel3V;
float BatteryV, MeasuredAmps, RPM;  //Readings from ADS1115
float MeasuredAmpsMax;              // used to track maximum alternator output
float RPMMax;                       // used to track maximum RPM
int ADS1115Disconnected = 0;
volatile bool battVFreshFlag = false;
bool ibvFreshFlag = false;  // set each INA228 read; cleared by fastOV dvdt block

// === Cloud Features Variables (and some others later added to LiveData) ===
// _AllTime totals are double: a float mantissa runs out around 16M Wh
double ChargedEnergy_AllTime = 0.0;            // Wh (lifetime total produced from all sources)
double DischargedEnergy_AllTime = 0.0;         // Wh (lifetime)
float SolarChargedEnergy = 0.0f;               // Solar Wh (session)
double SolarChargedEnergy_AllTime = 0.0;       // Solar Wh (lifetime)
double AlternatorChargedEnergy_AllTime = 0.0;  // Alternator Wh (lifetime)
float EngineFuelUsed = 0.0f;              // L (session)
double EngineFuelUsed_AllTime = 0.0;      // L (lifetime) — double: past 4096 L a float rounds every 0.01 L step to 0.00977
double AlternatorFuelUsed_AllTime = 0.0;  // L (lifetime) — double, same reason
// Engine fuel consumption table - RPM vs. gallons per hour
// Fuel table defaults
#define FUEL_TABLE_SIZE 10
float fuelTableRPM[FUEL_TABLE_SIZE] = { 0, 500, 1000, 1500, 2000, 2500, 3000, 3500, 4000, 4500 };
float fuelTableGPH[FUEL_TABLE_SIZE] = { 0.0, 0.3, 0.5, 0.8, 1.2, 1.8, 2.5, 3.5, 5.0, 7.0 };
float defaultFuelRPMValues[FUEL_TABLE_SIZE] = { 0, 500, 1000, 1500, 2000, 2500, 3000, 3500, 4000, 4500 };
float defaultFuelGPHValues[FUEL_TABLE_SIZE] = { 0.0, 0.3, 0.5, 0.8, 1.2, 1.8, 2.5, 3.5, 5.0, 7.0 };
float currentFuelGPH = 0.0f;  // live fuel flow, gallons/hr (interpolated from table by RPM; 0 = engine off)
float currentNMPG = 0.0f;     // live fuel economy, nautical miles per gallon (SOG / GPH; 0 = unavailable)
// Session fuel-economy curve: mpg binned by RPM, session only. Binned only once RPM and boat speed have BOTH held steady
// (mirrors boat-perf Episode banding). Bins span [0, top fuel-table RPM], so the width auto-fits the engine.
#define FUELCURVE_BINS 18           // number of RPM bins (fixed); width = currentFuelTopRPM / FUELCURVE_BINS
float fuelCurveNMPG[FUELCURVE_BINS] = { 0 };  // per-bin naut mi/gal; 0 = no qualified reading yet. Intentionally session-only (not persisted; UI labels it "this session").
float currentFuelTopRPM = 0.0f;     // top configured fuel-table RPM; sent to the chart for x-axis scaling
// Settle-then-measure gate: RPM+speed must hold within band for the settle time (boat speed lags throttle
// by tens of seconds), THEN mpg is averaged over the sample window and that average freezes the bin.
float fuelCurveRpmTol = 500.0f;     // RPM spread (max-min) still counted as steady
float fuelCurveSpdTol = 2.0f;       // boat-speed spread (max-min) still counted as steady (= +/-1 kt)
float fuelCurveSettleSec = 40.0f;   // hold steady this long before sampling starts (settling, discarded)
float fuelCurveSampleSec = 20.0f;   // then average mpg over this window; the average freezes the bin
// steady-gate running state + sample accumulators (engine-off / stopped / Reset all break the run):
float fcRpmMin = 0, fcRpmMax = 0, fcSpdMin = 0, fcSpdMax = 0;
uint32_t fcRunStartMs = 0;
bool fcRun = false;
double fcSampleMpgSum = 0, fcSampleRpmSum = 0;
uint32_t fcSampleCount = 0;
float PeakVoltage_AllTime = 0.0f;  // V (lifetime peak)
float MinVoltage = 99.0f;          // V (session min)
float MinVoltage_AllTime = 99.0f;  // V (lifetime min)
double EngineRunTime_AllTime = 0.0;  // seconds (lifetime) — divide by 3600 for hours; double for many-year precision
// Boat-speed data maturity — lifetime seconds spent moving in each mode (sailing = engine off, motoring = engine on).
// Shown next to "front points" in the Boat Speed plot. Reset by Clear All (resetBoatPerformance). Persisted in NVS.
double perfSailSeconds = 0.0;
double perfMotorSeconds = 0.0;
float ChargeCycles = 0;          // count (session)
float ChargeCycles_AllTime = 0;  // count (lifetime)
// Travel (nm, kts)
float TotalDistance = 0.0f;                      // nm (session)
float TotalDistance_AllTime = 0.0f;              // nm (lifetime)
float MaxSpeed = 0.0f;                           // kts, best 60-s average (session)
float MaxSpeed_AllTime = 0.0f;                   // kts, best 60-s average (lifetime)
float sogSust1m = 0.0f;                          // kts, SOG averaged over the last 60 s (0 = window not covered)
// Sustained-speed ring (updateSustainedSpeed in 5_functions.ino). Sized in samples:
// stores at most 1/s, so 96 slots always span more than the 60 s window.
#define SUST_WINDOW_MS    60000UL
#define SUST_MAX_GAP_MS    8000UL   // 4 missed COGSOG updates
#define SUST_STORE_MIN_MS    900UL
#define SUST_RING_SIZE        96
uint32_t *sustT = nullptr;                       // ps_malloc'd in setup(): sample millis()
float    *sustV = nullptr;                       // ps_malloc'd in setup(): sample knots
// Longest single trip (nm). Trip ends after 60 min continuous GPS-invalid OR continuous SOG < 1.5 kn.
float LongestSingleTrip_Nm_AllTime = 0.0f;       // nm (lifetime — winning trip)
float currentTripDistanceNm = 0.0f;              // nm (in-progress trip; NVS-persisted with epoch)
uint32_t currentTripLastUpdateEpoch = 0;         // Unix seconds of last advance (0 = unknown; only stamped when timeIsSynced)
bool tripActive = false;                         // true between motion start and 60-min stall finalize
bool tripPendingRecovery = false;                // set true on boot if NVS had a saved in-progress trip; resolved once time syncs
uint32_t timeSinceLastMotion = 0;                // ms accumulated below 1.5 kn while trip active
uint32_t timeSinceLastValidGps = 0;              // ms accumulated with invalid GPS while trip active
// Rolling 24-hour max distance (lb-max-24hr leaderboard). 24 hourly buckets; bucket[head] = in-progress hour.
// Sum of all 24 ≈ last 24 hours of motion (~1 hr undercount worst case). Ring is session-only; watermark is NVS-persisted.
float Max24hrDistance_AllTime = 0.0f;            // nm (lifetime max over any rolling 24-hr window)
float (&distHourBuckets)[24] = PsramPlace::make<float[24]>();                 // per-hour distance accumulators (nm)
uint8_t distHourHead = 0;                        // index of current in-progress hour bucket
uint32_t distHourStartMs = 0;                    // millis() at start of current hour bucket (0 = uninitialized)
// Water depth (NMEA2k PGN 128267). Stored in meters (NMEA native); displayed in feet (×3.28084).
float WaterDepth_m = 0.0f;                       // depth below transducer in meters
// Deepest anchorage leaderboard — sliding 5-hr window. Qualifying anchorage = boat stays within 100yd
// AND depth changes by 2–100 ft. Ring is allocated in PSRAM at boot (see setup()); watermark NVS-persisted.
float DeepestAnchorage_Ft_AllTime = 0.0f;        // ft (lifetime deepest qualifying anchorage avg depth)
// Sample tuple held in PSRAM ring; ~24 B × 300 slots = ~7.2 KB.
struct AnchorageSample {
  uint32_t sampleMs;   // millis() at sample
  double lat;
  double lon;
  float depth_ft;      // converted to feet for direct comparison with thresholds
};
AnchorageSample *anchorageRing = nullptr;        // PSRAM-allocated 300-slot ring
const uint16_t ANCHORAGE_RING_SIZE = 300;        // 300 samples = 299 one-minute intervals ≈ 5 hr (qualifier accepts one short)
uint16_t anchorageRingHead = 0;                  // next write slot
uint16_t anchorageRingCount = 0;                 // valid samples (up to ANCHORAGE_RING_SIZE)
uint32_t lastAnchorageSampleMs = 0;              // last sample push time
float AvgSOC = 0.0f;                             // % (session average state of charge, not currently used for anything)
float AvgSOC_AllTime = 0.0f;                     // % (lifetime average state of charge)
float AvgSpeed_AllTime = 0.0f;                   // kts (lifetime average speed)
float AvgSpeed = 0.0f;                           // Session average speed (kts) - random test value
volatile bool avgSpeedResetRequested = false;    // ResetAvgSpeed (web task): the speed calc zeroes its session accumulators on core 1
double AlternatorOnTime_AllTime = 0.0;           // seconds (lifetime); double like EngineRunTime_AllTime — a float stops taking 1 s steps at 2^24 s (4,660 h)
int EngineCycles_AllTime = 0;                    // Lifetime engine cycles - random test
float MaxAlternatorTemperatureF_AllTime = 0.0f;  // Lifetime max alternator temp (°F) - random test
float MaxTemperatureThermistor_AllTime = -99;    // Lifetime max thermistor temp (°C) - random test
float MeasuredAmpsMax_AllTime = 0.0f;            // Lifetime max alternator output (A) - random test
float RPMMax_AllTime = 0.0f;                     // Lifetime max RPM - random test
// SOC tracking accumulators (must be global for NVS save/load)
double socAccumulator_AllTime = 0.0;     // %·s (lifetime) — double; float loses integer exactness past 2^24, about 2-4 days of 2 s samples
double totalSocSampleTime_AllTime = 0.0;      // s (lifetime) — double so the denominator carries the same fractional seconds as the numerator; saved to NVS as u32
double totalVoltageSampleTime_AllTime = 0.0;  // s (lifetime) — double, same reason; saved to NVS as u32
double totalPoweredTime_AllTime = 0.0;        // s (lifetime), every pass whatever the sensors say; the sailing ratio divides by it; saved to NVS as u32
double totalSpeedSampleTime_AllTime = 0.0;    // s (lifetime) — double, same reason; saved to NVS as u32
double voltageAccumulator_AllTime = 0.0;  // V·s (lifetime) — double; float overflowed in ~14 days
double speedAccumulator_AllTime = 0.0;    // kt·s (lifetime) — double
float AvgVoltage_AllTime = 0.0f;


String installId = "";  // NK_InstallId; /installid serves it + "|" + FIRMWARE_VERSION — browsers auto-clear console history when either changes (NVS erase or reflash)

//supabase authentications stuff
String authToken = "";      // Stored auth token
bool isRegistered = false;  // Registration status


// CLOUD FEATURES - STATIC BUFFERS// For ESP32-S3 with 16GB and OPI PSRAM
const int PAYLOAD_BUFFER_SIZE = 6144;  // PSRAM. ~2.9 kB worst case at payload_v 6; headroom for the next additive block
const int CONFIG_PAYLOAD_SIZE = 32768;  // sized for the config picker spec (~190 fields)
const int ALT_UPLOAD_BUF_SIZE  = 256 * 1024;  // PSRAM sync block cap: a full alt batch (≤4096 pts) out, then the cloud's front read back into it (4096 rows ≤ 229 KB)
const int PERF_UPLOAD_BUF_SIZE = 512 * 1024;  // PSRAM sync block cap: sail+motor batches (≤4096 pts EACH) out, then both fronts back (2 × 4096 rows ≤ 393 KB)
#define SYNC_NOTE_BOAT_OUT  0  // syncAllocNote() slots: the upload block (Core 1) ...
#define SYNC_NOTE_ALT_OUT   1
#define SYNC_NOTE_BOAT_BACK 2  // ... and growing it for a reply bigger than the local fronts predicted (Core 0)
#define SYNC_NOTE_ALT_BACK  3

char *tempBuffer;

const int FILENAME_BUFFER_SIZE = 64;
const int TIMESTAMP_BUFFER_SIZE = 32;
const int MESSAGE_BUFFER_SIZE = 128;
char *payloadBuffer;
char *filenameBuffer;
char *timestampBuffer;
char *messageBuffer;
bool timeIsSynced = false;
time_t timeBase = 0;               // Unix timestamp at last sync
unsigned long timeBaseMillis = 0;  // millis() at last sync
unsigned long lastTimeSyncAttempt = 0;
const unsigned long TIME_SYNC_INTERVAL = 43200000;  // 12 hour
enum TimeSource { TIME_NONE,
                  TIME_GPS,
                  TIME_PHONE,
                  TIME_NTP,
                  TIME_MILLIS,
                  TIME_ESTIMATED };  // soft clock: RTC-retained (soft reboot) or NVS-restored (cold boot), pending a real source
TimeSource currentTimeSource = TIME_NONE;
// CLOCK_REAL_TAG once NTP, GPS or the phone set the clock; RTC RAM, so the clock a soft reset keeps is real only if it was
// real before. Cleared on a cold boot, whose NVS soft clock is an estimate (stamps from it are back-dated by the off time).
#define CLOCK_REAL_TAG 0x5245414Cu  // 'REAL'
inline bool clockIsReal() { return timeIsSynced && timeBase > 0 && clockRealTag == CLOCK_REAL_TAG; }

// Lower sanity bound for a recovered/retained epoch (Jan 1 2020, matches the GPS gate); below it the clock stays unsynced.
// restoreSoftClock() uses it so offline Long Term records aren't stamped 0 (phantom gaps).
const time_t SOFTCLOCK_SANE_EPOCH = 1577836800;

// Freshness trackers for the priority chain (NMEA → Phone → NTP), so a source that goes silent is replaced instead of
// owning currentTimeSource forever.
unsigned long lastNmea2kSystemTimeMs = 0;  // millis() of last successful PGN 126992 SystemTime parse
unsigned long lastNmea2kGnssMs       = 0;  // millis() of last successful PGN 129029 GNSS position parse
unsigned long lastNmea2kSogMs        = 0;  // millis() of last PGN 129026 parse with a valid SOG field
unsigned long lastNmea2kCogMs        = 0;  // millis() of last PGN 129026 parse with a valid COG field
unsigned long lastPhoneTimeMs        = 0;  // millis() of last phone-sourced time POST
unsigned long lastPhoneGpsMs         = 0;  // millis() of last phone-sourced GPS POST
unsigned long lastPhoneSpdMs         = 0;  // millis() of last phone-sourced speed POST
unsigned long lastPhoneHdgMs         = 0;  // millis() of last phone-sourced heading POST
const unsigned long NMEA_TIME_FRESH_MS = 300000UL;   // 5 min — fall back to other sources if no SystemTime
const unsigned long NMEA_GPS_FRESH_MS  = 60000UL;    //  1 min — GPS PGN cadence is ~1-2s, anything >60s is dead
const unsigned long PHONE_FRESH_MS     = 120000UL;   //  2 min — app posts every ~30-60s

// Phone-sourced values (held separately from NMEA so the priority chain can
// arbitrate cleanly without one source overwriting the other's globals).
double LatitudePhone  = 0.0;
double LongitudePhone = 0.0;
time_t PhoneTimeEpoch = 0;  // Unix seconds the phone reported, anchored at lastPhoneTimeMs
float  SpeedPhone     = 0.0f;  // knots (Geolocation API reports m/s; converted at the /set_phone_data handler)
float  HeadingPhone   = 0.0f;  // degrees, course over ground from the phone GNSS (doppler, true north)

// Manually typed coordinates (Weather Mode "Override manually"): while gpsManualActive they beat NMEA and phone GPS until
// cleared; the resolver reasserts them every tick. Persisted.
double LatitudeManual  = 0.0;
double LongitudeManual = 0.0;
bool   gpsManualActive = false;

// Effective GPS position source, resolved every tick by resolveSources() (4_functions.ino); CSV2 `currentGpsSource`.
enum GpsSource { GPS_NONE, GPS_NMEA, GPS_PHONE, GPS_MANUAL };
GpsSource currentGpsSource = GPS_NONE;
// Owner of speed/course (SOGNMEA/COGNMEA): selectable ONLY, phone speed is never an automatic fallback. Set by speedSourceMode,
// independent of timeSourceMode; never GPS_MANUAL (no speed). CSV2 `currentSpeedSource`.
GpsSource currentSpeedSource = GPS_NMEA;

// Owner of speed/course. SPD_SRC_PHONE = phone GNSS doppler via /set_phone_data
// (client posts ~2 s in this mode); NMEA PGN 129026 writes are suppressed so the
// record chain never mixes sources. Persisted NVS Pattern B.
enum SpeedSourceMode { SPD_SRC_NMEA, SPD_SRC_PHONE };
uint8_t speedSourceMode = SPD_SRC_NMEA;

// Position and clock are SEPARATE settings: a position fix is personal data and a clock reading is not, so phone time (the only
// source in AP mode) can be accepted without position. TSRC_AUTO = freshness-arbitrated chain; forced modes pin even when stale
// (value shown, MARK_FRESH skipped). Persisted NVS Pattern B.
enum TimeSourceMode { TSRC_AUTO, TSRC_NMEA, TSRC_PHONE, TSRC_NTP };
uint8_t timeSourceMode = TSRC_AUTO;

// GPS_SRC_AUTO = NMEA when fresh, else phone; no NTP member. The client asks location permission ONLY for GPS_SRC_PHONE,
// phone-owned speed, or Defer to Solar, so a default install never asks.
enum GpsPositionSource { GPS_SRC_AUTO, GPS_SRC_NMEA, GPS_SRC_PHONE };
uint8_t gpsPositionSource = GPS_SRC_AUTO;

struct SensorWindow {
  // Battery voltage
  int32_t battVolt_min = 999900;
  int32_t battVolt_max = 0;
  int64_t battVolt_area_v_us = 0;
  uint64_t battVolt_valid_us = 0;  // microseconds when valid

  // Battery current
  int32_t battCurr_min = 999900;
  int32_t battCurr_max = -999900;
  int64_t battCurr_area_v_us = 0;
  uint64_t battCurr_valid_us = 0;

  // Alternator current
  int32_t altCurr_min = 999900;
  int32_t altCurr_max = -999900;
  int64_t altCurr_area_v_us = 0;
  uint64_t altCurr_valid_us = 0;

  int32_t victronCurr_min = 999900;
  int32_t victronCurr_max = -999900;
  int64_t victronCurr_area_v_us = 0;
  uint64_t victronCurr_valid_us = 0;

  int32_t soc_min = 999900;
  int32_t soc_max = 0;
  int64_t soc_area_v_us = 0;
  uint64_t soc_valid_us = 0;

  // Barometric pressure
  int32_t baro_min = 999900;
  int32_t baro_max = 0;
  int64_t baro_area_v_us = 0;
  uint64_t baro_valid_us = 0;

  // Alternator temperature
  int32_t altTemp_min = 999900;
  int32_t altTemp_max = -999900;
  int64_t altTemp_area_v_us = 0;
  uint64_t altTemp_valid_us = 0;

  // Thermistor temperature
  int32_t tempTherm_min = 999900;
  int32_t tempTherm_max = -999900;
  int64_t tempTherm_area_v_us = 0;
  uint64_t tempTherm_valid_us = 0;

  // Ambient temperature
  int32_t ambTemp_min = 999900;
  int32_t ambTemp_max = -999900;
  int64_t ambTemp_area_v_us = 0;
  uint64_t ambTemp_valid_us = 0;

  // RPM is held in RAW units, not x100: per-install gain makes sub-RPM resolution meaningless, and cloud sensor_history.rpm_*
  // are INTEGER with leaderboards gated on rpm_avg > 50 REAL rpm.
  int32_t rpm_min = 999999;
  int32_t rpm_max = 0;
  int64_t rpm_area_v_us = 0;   // raw rpm * us
  uint64_t rpm_valid_us = 0;

  int32_t dutyCycle_min = 999900;
  int32_t dutyCycle_max = 0;
  int64_t dutyCycle_area_v_us = 0;
  uint64_t dutyCycle_valid_us = 0;

  // Engine-running-weighted accumulators (engineSpinning()): active_us is the denominator for the duty channel and coverage;
  // the four gated channels carry their own *_on_us.
  uint64_t active_us = 0;  // window µs with engine spinning (coverage numerator)
  uint64_t battVolt_on_us = 0;
  uint64_t battCurr_on_us = 0;
  uint64_t altCurr_on_us = 0;
  uint64_t victronCurr_on_us = 0;
  int64_t battVolt_on_area_v_us = 0;
  int64_t battCurr_on_area_v_us = 0;
  int64_t altCurr_on_area_v_us = 0;
  int64_t victronCurr_on_area_v_us = 0;
  int64_t dutyCycle_on_area_v_us = 0;

  // Dynamic alternator zero
  int32_t altZero_min = 999900;
  int32_t altZero_max = -999900;
  int64_t altZero_area_v_us = 0;
  uint64_t altZero_valid_us = 0;

  // Speed over ground
  int32_t sog_min = 999900;
  int32_t sog_max = 0;
  int32_t sogSust1m_max = 0;   // kts ×100, best 60-s average seen this window (feeds the cloud speed board)
  uint8_t sogSustPhone = 0;    // 1 = the standing sogSust1m_max was phone-GPS-sourced (leaderboard disqualification flag)
  int64_t sog_area_v_us = 0;
  uint64_t sog_valid_us = 0;

  // Course over ground
  int32_t cog_min = 999900;
  int32_t cog_max = 0;
  int64_t cog_area_v_us = 0;
  uint64_t cog_valid_us = 0;

  int32_t heading_min = 999900;
  int32_t heading_max = 0;
  int64_t heading_area_v_us = 0;
  uint64_t heading_valid_us = 0;

  // Apparent wind speed
  int32_t aws_min = 999900;
  int32_t aws_max = 0;
  int64_t aws_area_v_us = 0;
  uint64_t aws_valid_us = 0;

  // Apparent wind angle
  int32_t awa_min = 999900;
  int32_t awa_max = 0;
  int64_t awa_area_v_us = 0;
  uint64_t awa_valid_us = 0;

  // True wind speed
  int32_t tws_min = 999900;
  int32_t tws_max = 0;
  int64_t tws_area_v_us = 0;
  uint64_t tws_valid_us = 0;

  // True wind angle
  int32_t twa_min = 999900;
  int32_t twa_max = 0;
  int64_t twa_area_v_us = 0;
  uint64_t twa_valid_us = 0;

  int32_t leeway_min = 999900;
  int32_t leeway_max = -999900;
  int64_t leeway_area_v_us = 0;
  uint64_t leeway_valid_us = 0;

  int32_t vmg_min = 999900;
  int32_t vmg_max = -999900;
  int64_t vmg_area_v_us = 0;
  uint64_t vmg_valid_us = 0;

  int32_t uTargetAmps_min = 999900;
  int32_t uTargetAmps_max = -999900;
  int64_t uTargetAmps_area_v_us = 0;
  uint64_t uTargetAmps_valid_us = 0;

  // Battery temperature in use this tick (batteryTempF()). NAN = no source, so valid_us == 0 is the cloud's "absent" signal,
  // like victronCurr. The SOURCE code rides the 24 h snapshot.
  int32_t battTemp_min = 999900;
  int32_t battTemp_max = -999900;
  int64_t battTemp_area_v_us = 0;
  uint64_t battTemp_valid_us = 0;

  // EXTRA-role DS18B20 — whatever the owner assigned that probe to. Absent on most boats.
  int32_t extraTemp_min = 999900;
  int32_t extraTemp_max = -999900;
  int64_t extraTemp_area_v_us = 0;
  uint64_t extraTemp_valid_us = 0;

  // VE.Direct solar panel power / voltage (PPV / VPV). Gated on IDX_VICTRON_SOLAR freshness so a
  // boat with no MPPT reads absent rather than a flat 0 W line.
  int32_t solarPower_min = 999900;
  int32_t solarPower_max = -999900;
  int64_t solarPower_area_v_us = 0;
  uint64_t solarPower_valid_us = 0;

  int32_t solarVolt_min = 999900;
  int32_t solarVolt_max = -999900;
  int64_t solarVolt_area_v_us = 0;
  uint64_t solarVolt_valid_us = 0;

  // GPS
  double lat_current;  // Most recent smoothed latitude
  double lon_current;  // Most recent smoothed longitude

  // Timing
  uint64_t lastUpdateTime_us = 0;  // Track last update for time-weighted averaging
  unsigned long windowStartTime = 0;
};
SensorWindow *currentWindow = nullptr;  // allocated to PSRAM in init

// One channel of a closed window, reduced at roll time to what buildSnapshotJson prints: min/max as the window's scaled
// integers (x100; rpm raw; IMU accel x1000), avg already un-scaled. avg is NaN when the window never accumulated the
// channel (valid_us == 0), which is the builder's "absent" signal.
struct SensorChan {
  int32_t min;
  int32_t max;
  float   avg;
};

// PSRAM ring of completed sensor snapshots awaiting upload (not LittleFS: a sector erase stalled Core 1 ~400 ms).
// Drains to httpsQueue from the field-off gate or "Upload Now". Holds the reduced window, not the SensorWindow
// accumulators (888 B a slot became 448): pushSensorSnapshot does the time-weighted divisions once, at roll.
struct SensorSnapshot {
  int64_t collectionTime;       // time_t (64-bit, Y2038-safe) — positive epoch if synced, negative-millis marker if not
  SensorChan battVolt, battCurr, altCurr, victronCurr, soc, baro, altTemp, tempTherm, ambTemp, rpm, dutyCycle, altZero;
  SensorChan sog, cog, heading, aws, awa, tws, twa, leeway, vmg, uTargetAmps, battTemp, extraTemp, solarPower, solarVolt;
  // Engine-running-weighted averages (NaN when that channel's *_on_us was 0) and engine-on % of the window (NaN when
  // dutyCycle never accumulated).
  float battVoltOnAvg, battCurrOnAvg, altCurrOnAvg, victronCurrOnAvg, dutyCycleOnAvg;
  float enginePct;
  int32_t sogSust1m_max;        // kts x100, best 60-s average seen this window
  uint8_t sogSustPhone;         // 1 = that sustained speed was phone-GPS-sourced
  uint8_t chargeStage;          // display code (0-7) captured at window roll — uploads are deferred so this can't read live state later
  double lat_current, lon_current;
  // IMU subset frozen at roll: uploads are field-off-gated and run minutes-hours later, after imuWindow has reset.
  SensorChan heel, pitch;                  // x100
  SensorChan vertical_accel, total_accel;  // x1000
  float    msi_score, vomit_pct, anchorage_comfort;   // comfort score point values at roll time (raw firmware 0-100 scales)
  int32_t  wave_period;         // x1000; -1000 if invalid
  uint32_t slam_count;
  int32_t  slam_peak_max;       // x1000 (millig's)
  bool     suspicious;          // frozen at roll: install unvalidated/bad, or this window's heel/pitch bias is implausible
};
static_assert(sizeof(SensorSnapshot) == 448, "SensorSnapshot layout changed - the PSRAM budget and SENSOR_RING_BACKUP_VER assume 448 B");
const uint16_t SENSOR_RING_SIZE = 1000;  // 1000 × 448 B ≈ 438 KB PSRAM; ~6.9 days at SENSOR_UPLOAD_INTERVAL (10 min/sample)
SensorSnapshot *sensorRing = nullptr;    // ps_malloc'd in setup()
volatile uint16_t sensorRingHead = 0;    // next write slot
volatile uint16_t sensorRingTail = 0;    // oldest unread slot
volatile uint16_t sensorRingCount = 0;   // 0..SENSOR_RING_SIZE
volatile int32_t sensorRingInFlightIndex = -1;  // ring index currently being uploaded by Core 0 (-1 = none)
// Guards the count/tail/head read-modify-writes: Core 1 pushes, Core 0 pops,
// the async web task clears — volatile alone doesn't make uint16 RMW atomic
// on Xtensa, and a lost update near empty wraps count to 65535 (= full forever).
portMUX_TYPE sensorRingMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool forceCloudFlushPending = false;   // set by /get?forceCloudFlush=1, cleared after drain attempt
volatile bool sensorRingAnnouncedEmpty = false; // throttle for "all data uploaded" console — fires once per drain-to-empty
volatile bool sensorRingBackupStale = false;    // the Phase-4 ring dump on flash is older than the ring (an upload drain emptied it): deleted at the next field-cut flash pass, never on the upload task

// Barometric pressure history — 10-min cadence, 7 days. Feeds the Other-tab barometer
// (3hr tendency for Zambretti + plot of pressure trend). Beyond 1 week → cloud.
// Sized small (2 KB blob) to keep NVS wear negligible; saved only at field-off edge.
const uint16_t  BARO_HISTORY_SIZE       = 1008;                  // 7 d × 24 h × 6 samp/h
const uint32_t  BARO_SAMPLE_INTERVAL_MS = 10UL * 60UL * 1000UL;  // 10 minutes
uint16_t       *baroPressureHistory     = nullptr;               // ps_malloc'd in setup(); mbar × 10 (0 = no sample)
volatile uint16_t baroHistoryHead       = 0;                     // next write slot (circular)
uint16_t        prev_baroHistoryHead    = 0xFFFF;                // NVS shadow — write only when head moved
unsigned long   lastBaroSampleMs        = 0;                     // millis() of last sample
time_t          baroHistoryLastEpoch    = 0;                     // GPS/NTP epoch of newest sample (0 = unsynced — JS falls back to fixed sample spacing)

// Long Term Plots: ~1-month PSRAM ring of min/max/avg envelopes at SENSOR_UPLOAD_INTERVAL, built from the closing SensorWindow,
// served via /longTermPlots.bin, persisted (writePsramBlob/readPsramBlob) at field-off edge + shutdown, restored at boot.
// Each record carries its own epoch; an unsynced one (0) is placed at lastEpoch − (newest − N) × interval
// (the baro scheme); cadence is served in the .bin header. Brush past ~1 month →
// cloud aggregate (1b stitch). recordSize is the scaffold layout guard — pinned below.
struct LongTermRecord {                 // naturally aligned; see static_assert for true size
  // 4-byte block
  uint32_t timestamp;                   // epoch seconds of THIS record (0 = unsynced) — legit time axis + gap detection
  int32_t  lat_avg, lon_avg;            // deg ×1e5
  uint32_t validMask;                   // 1 bit/field: had valid data this window

  // ENVELOPE (min,max,avg) — 16 fields × 3 × int16
  int16_t battVolt[3];                  // V   ×100
  int16_t battCurr[3];                  // A   ×10
  int16_t altCurr[3];                   // A   ×10
  int16_t victronCurr[3];               // A   ×10
  int16_t rpm[3];                       //     ×1   (×100 overflows int16 >327 RPM)
  int16_t duty[3];                      // %   ×100  (this IS field drive; battVolt is stored too)
  int16_t altTemp[3];                   // °F  ×10
  int16_t tempTherm[3];                 // °F  ×10
  int16_t sog[3];                       // kt  ×100
  int16_t tws[3];                       // kt  ×100
  int16_t vmg[3];                       // kt  ×100
  int16_t aws[3];                       // kt  ×100
  int16_t awa[3];                       // deg ×10 (signed)
  int16_t twa[3];                       // deg ×10 (signed)
  int16_t heel[3];                      // deg ×100 (signed, IMU)
  int16_t pitch[3];                     // deg ×100 (signed, IMU)
  int16_t soc[3];                       // %   ×10 (envelope — window already tracks soc_min/max)

  // AVG-ONLY — 6 fields × int16
  int16_t baro_avg;                     // mbar×10
  int16_t ambTemp_avg;                  // °F  ×10
  int16_t cog_avg, heading_avg;         // deg ×10
  int16_t leeway_avg;                   // deg ×10 (signed)
  int16_t altZero_avg;                  //     ×100

  // byte block
  uint8_t chargeStage;                  // 0 off,1 bulk,2 absorption,3 float,4 manual,5 maintain,6 targetV,7 idle
  uint8_t _pad;

  // v4 tail (append-only — offsets above must not move): engine-running-weighted averages (÷ active_us) + window coverage.
  int16_t onAvg[5];                     // battVolt ×100, battCurr ×10, altCurr ×10, victronCurr ×10, duty ×100
  uint8_t activeFrac;                   // engine-on % of observed window ×2 (0..200 = 0..100%); 0 ⇒ onAvg invalid
  uint8_t _pad2;

  // v5 tail (append-only): battery temperature in use (batteryTempF), envelope, validMask bit 24
  int16_t battTemp[3];                  // °F  ×10
  int16_t _pad3;
};
// 16 + 102 + 12 + 2 + 10 + 2 + 8 = 152, no tail padding; pinned so a layout edit can't silently desync the scaffold guard.
static_assert(sizeof(LongTermRecord) == 152, "LongTermRecord layout changed — update the scaffold recordSize guard");

const uint16_t LONGTERM_RING_SIZE = 4320;    // 30 d × 24 h × 6/h at 10-min cadence (~641 KB @ 152 B)
LongTermRecord *longTermRing      = nullptr; // ps_malloc'd in setup()
volatile uint16_t longTermHead    = 0;       // next write slot (circular)
volatile uint16_t longTermCount   = 0;       // 0..LONGTERM_RING_SIZE
uint16_t   prev_longTermHead      = 0xFFFF;  // shadow — persist only when head moved
time_t     longTermLastEpoch      = 0;       // epoch of newest record (0 = unsynced)
uint32_t   longTermPushSeq        = 0;       // monotonic record-push counter (append-flush bookkeeping)
uint32_t   longTermFlushedSeq     = 0;       // longTermPushSeq value at last successful flush
uint32_t   longTermFileRecords    = 0;       // records currently on disk (0 = none/invalid → compact next flush)
#define LONGTERM_BACKUP_PATH  "/longterm_ring.bin"
#define LONGTERM_BACKUP_MAGIC 0x4C54504Cu    // 'LTPL'
#define LONGTERM_BACKUP_VER   5u    // v5: +battTemp[3] (144→152 B); old rings discarded
// Periodic field-off re-dump (only when the ring changed): the rising-edge dump alone loses records on a bench where the field
// never cycles. Appends only new records, so the cost is bytes, not a whole-file erase.
const unsigned long LONGTERM_DUMP_INTERVAL_MS = 15UL * 60UL * 1000UL;  // 15 min

// ===== ZERO-DRIFT CHARACTERIZATION LOG =====
// Alternator current sensor reading WHILE THE FIELD IS OFF (>= 5 s): every 1 s while spinning (RPM>=200), every 10 min engine-off.
// Logged amps are ABSOLUTE (live correction added back): logging residuals made each fit a fit of its own residual and the EMA
// converged on HALF the offset. Data source for zeroFitCompute (ZERO_DRIFT_TEMPCOMP_SPEC.md). PSRAM ring, flushed field-off only.
struct ZeroLogRecord {        // 20 bytes
  uint32_t epoch;             // wall-clock seconds (0 = unsynced)
  float    amps;              // MeasuredAmps at sample time (the zero-drift signal)
  float    p2pAmps;           // peak-to-peak of MeasuredAmps over the prior 0.5 s
  int16_t  rpm;               // engine RPM
  int16_t  battVx100;         // battery volts ×100
  int16_t  altTempFx10;       // alternator temperature °F ×10 (ZEROLOG_TEMP_BLANK = sensor absent)
  int16_t  boardTempFx10;     // regulator board temp (BMP388) °F ×10 — drift may track EITHER this or
                              // altTempFx10 per install; the daily fit keeps the better correlate.
};
const uint16_t ZEROLOG_RING_SIZE = 10000;   // ~195 KB PSRAM; ~2.8 hr @1s spinning, days @10min idle
ZeroLogRecord *zeroLogRing = nullptr;       // ps_malloc'd in zeroLogInit()
volatile uint16_t zeroLogHead = 0;          // next write slot (circular)
volatile uint16_t zeroLogCount = 0;         // 0..ZEROLOG_RING_SIZE
uint16_t  prev_zeroLogHead = 0xFFFF;        // shadow — flush only when head moved
uint32_t  zeroLogPushSeq = 0;               // monotonic sample-push counter (append-flush bookkeeping)
uint32_t  zeroLogFlushedSeq = 0;            // zeroLogPushSeq value at last successful flush
uint32_t  zeroLogFileRecords = 0;           // records currently on disk (0 = none/invalid → compact next flush)
bool      ZeroLogEnable = true;             // master toggle (NVS NK_ZeroLogEnable), default ON (permanent subsystem)
float     altAmpsP2P = 0.0f;                // latched 0.5 s peak-to-peak of MeasuredAmps (set in ReadAnalogInputs)
#define ZEROLOG_PATH  "/zerolog.bin"
#define ZEROLOG_MAGIC 0x5A45524Fu           // 'ZERO'
#define ZEROLOG_VER   3u                     // v3 logs the ABSOLUTE zero (correction added back, see zeroLogService);
                                             // v1/v2 files discarded on load: residual rows bias the fit
#define ZEROLOG_FIELDOFF_MIN_MS  5000UL     // field must be off this long before a sample (own short gate)
#define ZEROLOG_RUN_INTERVAL_MS  1000UL     // sample period while engine spinning (RPM>=200)
#define ZEROLOG_IDLE_INTERVAL_MS 600000UL   // sample period while engine off (RPM<200) = 10 min
#define ZEROLOG_FLUSH_MS         1800000UL  // field-off flush cadence = 30 min
// AlternatorTemperatureF is NaN after reboot until the first DS18B20 read; past this wait the sensor is absent → log the blank.
#define ZEROLOG_TEMP_GRACE_MS    30000UL    // post-reboot wait for the first valid alt-temp read
#define ZEROLOG_TEMP_BLANK       INT16_MIN  // alt-temp "sensor absent" sentinel → CSV emits an empty cell

// ===== COMMISSIONED ALTERNATOR-CURRENT ZERO (field at 0%, engine running or stopped) =====
// Absolute sensor zero measured with no field: a same-day anchor, since the temp-comp fit needs a 30 °F span (weeks on a boat)
// and then only tracks drift on top. Request-driven only (AltZeroCaptureNow): the tick holds the field at 0% when it is on
// (tick.altZeroMeasuring, REASON_ALTZERO_MEASURE), the rotor drains, the reading is averaged. A stopped engine is as true a zero
// as a running one; what matters is a warm sensor. No opportunistic capture (the wizard rest hold never reaches 0%). The operator
// applies the result; nothing is written behind their back.
float    altZeroCapA      = 0.0f;   // mean MeasuredAmps over the accepted window = the delta to apply
float    altZeroCapTempF  = NAN;    // alternator temperature at capture (NaN = probe absent)
float    altZeroCapBoardF = NAN;    // board temperature at capture (NaN = BMP388 absent)
int16_t  altZeroCapRpm    = 0;      // engine RPM latched when the hold engaged — the last reading the tach gave
                                    // with the field still on; at 0% field the LM2907 may read anything
uint8_t  altZeroCapEngineOff = 0;   // 1 = RPM was below ALTZERO_MIN_RPM at the engine check: no tach mask, worst-case drain
uint8_t  altZeroHoldField = 0;      // 1 = the tick holds the field at 0% for this request (always when running; engine
                                    // off only if the field was on — else the hold would raise the enable line for nothing)
uint16_t altZeroCapN      = 0;      // samples averaged
float    altZeroCapP2P    = 0.0f;   // worst 0.5 s peak-to-peak seen inside the window
uint8_t  altZeroCapAuto   = 0;      // 1 = the window was read with the learned zero correction subtracted (AutoAltCurrentZero on)
uint32_t altZeroCapEpoch  = 0;      // wall clock of the held capture (0 = clock never set, informational)
uint8_t  altZeroCapHeld   = 0;      // 1 = a capture is held for apply. NOT the epoch: getCurrentTimestamp()
                                    // reads 0 until the clock is set, which is every AP-mode commissioning
float    altZeroAppliedA  = 0.0f;   // delta of the last applied capture (the UI's "applied" line)
uint32_t altZeroAppliedEpoch = 0;   // wall clock of the last apply (0 = clock never set, informational)
uint8_t  altZeroApplied   = 0;      // 1 = a capture has been applied this power cycle
uint8_t  altZeroRejReason = 0;      // why the last request was thrown out (ALTZERO_RJ_*)
int      AltZeroCaptureNow = 0;     // momentary: 1 = a measurement is requested / in progress
uint8_t  altZeroCapPhase  = 0;      // where the request stands (ALTZERO_PH_*), for the UI's progress line
uint32_t altZeroHoldEndMs = 0;      // millis() the hold released (0 = never): the RPM gates stay masked for
                                    // FIELDCUT_RPM_GRACE_MS after it while the LM2907 re-biases (see fieldCutRpmGrace)
// Rejection reasons — the UI names them, so a refusal is never a silent nothing.
#define ALTZERO_RJ_NONE   0
#define ALTZERO_RJ_NOISE  1   // sample noise above ALTZERO_MAX_P2P_A — a disturbance, not a zero
#define ALTZERO_RJ_MAG    2   // |mean| above ALTZERO_MAX_MAG_A — wrong cable or real current, not drift
#define ALTZERO_RJ_RPM    3   // retired (the tach is unreliable at 0% field); slot kept so the UI table stays aligned
#define ALTZERO_RJ_SHORT  4   // the field came back on, or the window broke up, before enough samples
#define ALTZERO_RJ_ENGINE 5   // retired (engine-off captures are allowed); slot kept so the UI table stays aligned
#define ALTZERO_RJ_BUSY   6   // a commissioning/tuning test is driving the alternator
#define ALTZERO_RJ_FIELD  7   // the field never reached 0% under the hold (gave up at ALTZERO_FORCE_TIMEOUT_MS)
// Request phases (altZeroCapPhase).
#define ALTZERO_PH_IDLE   0
#define ALTZERO_PH_ENGINE 1   // reading the tach to decide running vs stopped (field still on, reading trustworthy)
#define ALTZERO_PH_RAMP   2   // hold engaged, field ramping to 0%
#define ALTZERO_PH_DRAIN  3   // duty at 0%, waiting out the rotor drain
#define ALTZERO_PH_AVG    4   // averaging the reading
#define ALTZERO_WIN_MS         10000UL // averaging window
#define ALTZERO_SAMPLE_MS      100UL   // sampling cadence inside the window (→ ~100 samples)
#define ALTZERO_MIN_SAMPLES    60      // too few passes landed to call the result an average
#define ALTZERO_MIN_RPM        400     // engine genuinely running (the zero log's own gate is 200); below it = engine-off capture
#define ALTZERO_ARM_GRACE_MS   5000UL  // with the field on this recently, a request waits this long for RPM >= ALTZERO_MIN_RPM
                                       // before calling the engine stopped (the LM2907's ~4.6 s false-zero after a field cut)
#define ALTZERO_RAMP_PCT       10.0f   // 12 V-base duty ramp toward 0% under the hold (%/s), x fieldScale() like the rest hold
#define ALTZERO_SETTLE_MULT    3       // field-off dwell before sampling = this × the measured drain
#define ALTZERO_SETTLE_MIN_MS  5000UL  // ... floor, for a unit whose field-decay stage never ran
#define ALTZERO_MAX_P2P_A      2.0f    // noise ceiling: above this the reading is not a settled zero
#define ALTZERO_MAX_MAG_A      10.0f   // a bigger field-off reading is a wiring fault, not sensor drift
#define ALTZERO_FORCE_TIMEOUT_MS 90000UL // a request gives up after this whatever phase it is in, so the UI
                                         // can never sit on "measuring" until the next reboot

// ===== Temperature-compensated zero correction (ZERO_DRIFT_TEMPCOMP_SPEC.md) =====
// Daily line-fit of the zero-drift log → live correction zero(T)=c+b·(T−T_REF), EMA-smoothed across days, ±3 A clamped.
#define ZF_BOARD 0
#define ZF_ALT   1
#define ZFIT_T_REF            100.0f     // °F — fixed reference; c is the offset at this temp (never change)
#define ZFIT_MIN_SPAN_F       30.0f      // fit block must span >this many °F
#define ZFIT_MIN_POINTS       30         // and hold at least this many field-off samples
#define ZFIT_MAX_LOOKBACK_S   2592000UL  // stop walking back past 30 days
#define ZFIT_MAX_RESID_A      1.5f       // fit residual RMS above this → too noisy, hold last (a clean
                                         // flat offset has tiny residual, so this passes it — R² would not)
#define ZFIT_ALPHA            0.25f      // EMA base weight → ~4-5 day response
#define ZFIT_W_MAX            0.30f      // cap on a single day's blend weight
#define ZFIT_CLAMP_A          3.0f       // correction hard-bounded to ±this many amps, always
#define ZFIT_OUTLIER_C_A      2.0f       // reject a day whose c differs from history median by >this (A)
#define ZFIT_OUTLIER_B        0.05f      // ...or whose b differs by >this (A/°F)
#define ZFIT_OUTLIER_MINHIST  5          // outlier gate arms only once history has >=this many entries
#define ZFIT_FLIP_MARGIN      0.1f       // other sensor must beat chosen sensor's R² by >this to flip
#define ZFIT_FLIP_DAYS        3          // ...for this many consecutive fits
#define ZFIT_HIST_SIZE        90         // daily-fit history ring (outlier median + trend)
#define ZFIT_INTERVAL_MS      86400000UL // recompute at most once per 24 h of runtime
#define ZFIT_RETRY_MS         3600000UL  // if a compute HELD (no spread/noisy), retry in 1 h not 24 h
#define ZFIT_PATH  "/zerofit.bin"
#define ZFIT_MAGIC 0x5A464954u           // 'ZFIT'
#define ZFIT_VER   2u                    // v2 with ZEROLOG_VER 3: fits built from residual (v2-log) rows were biased by ~half; zeroFitInit clears the live equation with a discarded history

struct ZeroFitRecord {        // one accepted daily fit
  uint32_t epoch;             // wall-clock seconds of the fit
  float    c;                 // offset at T_REF (amps)
  float    b;                 // slope (amps/°F)
  float    r2;                // correlation of the winning regression
  uint16_t sensor;           // ZF_BOARD or ZF_ALT
  uint16_t n;                 // sample count in the fit block
};
// Output of one regression pass (zeroFitRegress, 5_functions.ino) — here so the auto-prototype sees it.
struct ZFitResult { bool ok; float c, b, r2, residRms, span; uint16_t n; };
ZeroFitRecord *zeroFitHist = nullptr;       // ps_malloc'd in zeroFitInit()
uint16_t  zeroFitHistHead  = 0;             // next write slot (circular)
uint16_t  zeroFitHistCount = 0;             // 0..ZFIT_HIST_SIZE
uint16_t  prev_zeroFitHistHead = 0xFFFF;    // shadow — flush only when head moved
// Live equation (persisted, NVS storage namespace). zfValid=0 until the first good fit → correction 0.
int   zfValid  = 0;
int   zfSensor = ZF_BOARD;
float zfC = 0.0f;                           // offset at T_REF
float zfB = 0.0f;                           // slope A/°F
float zfR2 = 0.0f;
uint32_t zfLastEpoch = 0;                   // epoch of the last accepted fit
int   zfFlipPending = ZF_BOARD;             // which sensor has been trying to win
int   zfFlipStreak  = 0;                    // consecutive fits the other sensor has won

struct ImuWindow {  // lives in PSRAM to save internal SRAM
  // Raw accel signals (scaled by 1000: 1.234g → 1234)
  int32_t accel_x_min, accel_x_max;
  int64_t accel_x_area_v_us;
  uint64_t accel_x_valid_us;

  int32_t accel_y_min, accel_y_max;
  int64_t accel_y_area_v_us;
  uint64_t accel_y_valid_us;

  int32_t accel_z_min, accel_z_max;
  int64_t accel_z_area_v_us;
  uint64_t accel_z_valid_us;

  // Raw gyro signals (scaled by 100: 12.34 dps → 1234)
  int32_t gyro_x_min, gyro_x_max;
  int64_t gyro_x_area_v_us;
  uint64_t gyro_x_valid_us;

  int32_t gyro_y_min, gyro_y_max;
  int64_t gyro_y_area_v_us;
  uint64_t gyro_y_valid_us;

  int32_t gyro_z_min, gyro_z_max;
  int64_t gyro_z_area_v_us;
  uint64_t gyro_z_valid_us;

  // Calculated metrics - heel/pitch (scaled by 100: 12.34° → 1234)
  int32_t heel_min, heel_max;
  int64_t heel_area_v_us;
  uint64_t heel_valid_us;

  int32_t pitch_min, pitch_max;
  int64_t pitch_area_v_us;
  uint64_t pitch_valid_us;

  // Calculated metrics - accelerations (scaled by 1000)
  int32_t vertical_accel_min, vertical_accel_max;
  int64_t vertical_accel_area_v_us;
  uint64_t vertical_accel_valid_us;

  int32_t total_accel_min, total_accel_max;
  int64_t total_accel_area_v_us;
  uint64_t total_accel_valid_us;

  // High-frequency vibration energy (scaled by 1000000: μg² RMS)
  int32_t hf_vibe_min, hf_vibe_max;
  int64_t hf_vibe_area_v_us;
  uint64_t hf_vibe_valid_us;

  // 60s rolling window metrics (updated at ~1Hz, stored as current value)
  int32_t heel_change_60s;      // Scaled by 100
  int32_t heel_deviation_60s;   // Scaled by 100
  int32_t pitch_change_60s;     // Scaled by 100
  int32_t pitch_deviation_60s;  // Scaled by 100

  // Wave period (scaled by 1000: 6.5s → 6500, or -1000 if invalid)
  int32_t wave_period;

  // Period counters (reset each window)
  uint32_t slam_count;
  int32_t slam_peak_max;  // Scaled by 1000 (millig's)

  // Timing
  uint64_t lastUpdateTime_us;
  uint64_t lastGyroUpdateTime_us;
  unsigned long windowStartTime;
};
ImuWindow *imuWindow = nullptr;

unsigned long lastSensorUploadTime = 0;
bool sensorUploadInProgress = false;

// core0Busy: hold the field off while a long Core 0 op runs (AdjustFieldLearnMode() honors it only while GPIO4 is low).
// Set it ONLY from ops already gated on fieldCutSettled(): httpsTask, syncTimeFromNTP, OTA.
// Readers: TempTask, testInternetSpeed, scheduled restart, OTA orchestration, the AFLM gate.
volatile bool core0Busy = false;

// Task-WDT panic window, sized for the control loop: every blocking network call on a WDT-subscribed task must finish inside it
// (httpsTask caps each phase at 12 s). performOTAUpdateToVersion() widens it per attempt and restores it on every exit.
const uint32_t WDT_TIMEOUT_MS = 16000;

// Dashboard mirror of sensorRingCount. Real cap is SENSOR_RING_SIZE (1000).
int bufferedRecordCount = 0;
unsigned long lastBufferUploadAttempt = 0;

//temp debug of cloud uploads
int lastHttpStatus = -1;
// volatile with sensorRingInFlightIndex: Core 0 clears this flag BEFORE publishing index -1 (Core 1
// starts the next upload on -1 and sets both), and volatile keeps the two stores in that order.
volatile bool lastUploadWasBuffered = false;
// Core 0 sets it on a sensor-history 401 that is not token_not_found; Core 1's uploadBufferedRecords waits it out.
volatile uint32_t sensorUploadHoldUntilMs = 0;
unsigned long lastBufferStatusPrint = 0;
const unsigned long BUFFER_STATUS_INTERVAL = 60000;  // 60 seconds


// Battery SOC Monitoring Variables
int BatteryCapacity_Ah = 200;         // Battery capacity in Amp-hours
int SOC_percent = 5000;               // State of Charge percentage (0-100) but have to multiply by 100 for annoying reasons, but go with it
float ManualSOCPoint = 25.0f;              // Used to set it manually (decimals allowed)
int CoulombCount_Ah_scaled = 7500;    // Current energy in battery (Ah × 100 for precision)
// Deferred first-boot SoC seed: loadNVSData() runs before the INA228 is read and before InitSystemSettings parses the battery,
// so the voltage estimate waits for the forced ReadAnalogInputs() at the end of setup().
bool socSeedPending = false;      // SOC_percent key missing from NVS at boot
bool coulombSeedPending = false;  // CoulombCount key missing — re-derive from the seeded SoC
float PeukertRatedCurrent_A = 15.0f;  // Standard discharge rate for Peukert (C/20), will be calculated from capacity
bool FullChargeDetected = false;      // Flag for full charge detection
unsigned long FullChargeTimer = 0;    // Accumulates elapsed seconds while full-charge conditions hold; compared against ChargedDetectionTime
// Timing variables
unsigned long currentTime = 0;
unsigned long elapsedMillis = 0;
unsigned long lastSOCUpdateTime = 0;      // Last time SOC was updated
unsigned long lastEngineMonitorTime = 0;  // Last time engine metrics were updated
unsigned long lastDataSaveTime = 0;       // Last time data was saved to LittleFS
int SOCUpdateInterval = 2000;             // Update SOC every 2 seconds.   Don't make this smaller than 1 without study

//NVS Stuff
unsigned long lastNVSSaveTime = 0;  // millis() at end of last successful saveNVSDataFull() — 0 = never saved
uint32_t nvsFullSaveCount = 0;      // total saveNVSDataFull() calls since boot
uint16_t nvsFullSaveLastMs = 0;     // wall-clock duration of most recent saveNVSDataFull() (ms)
uint16_t nvsFullSaveWorstMs = 0;    // worst saveNVSDataFull() duration since boot (ms)

/*
NVS wear (N16R8, nvs = 20 KB = 5 × 4 KB pages, ~504 entries before GC, ≥100k erase cycles/page):
2-min saves with change detection, 5-21 keys/save → ~7-30 GC cycles/day → ~9-38 yr service life.
*/


/*
IMU keys write only on events / bucket transitions (capsize/pitchpole immediately, the rest at field-off saveNVSDataFull):
worst case 26 keys/save → ~37 GC cycles/day ≈ 7.4 yr, still acceptable for marine service life.
*/



// Previous values for change detection (simple globals)
uint32_t prev_ChargedEnergy = 0;
uint32_t prev_DischrgdEnergy = 0;
uint32_t prev_AltChrgdEnergy = 0;
int32_t prev_AltFuelUsed = 0;
int32_t prev_EngineRunTime = 0;
int32_t prev_EngineCycles = 0;
int32_t prev_AltOnTime = 0;
int32_t prev_SOC_percent = 0;
int32_t prev_CoulombCount = 0;
uint32_t prev_SessionDur = 0;
int32_t prev_MaxLoop = 0;
int32_t prev_MinHeap = 0;
int32_t prev_PowerCycles = 0;
float prev_InsulDamage = 0.0f;
float prev_GreaseDamage = 0.0f;
float prev_BrushDamage = 0.0f;
// Temp-comp zero-correction learned-equation shadows (auto-fire NVS compare-first)
int prev_zfValid = -1;
int prev_zfSensor = -1;
float prev_zfC = 1e30f;
float prev_zfB = 1e30f;
float prev_zfR2 = -1.0f;
uint32_t prev_zfLastEpoch = 0xFFFFFFFFu;
uint32_t prev_SolarEnergy = 0;
int32_t prev_EngineFuel = 0;
uint32_t prev_ChargeCycleEnergy = 0;
int32_t prev_TotalDist = 0;
int32_t prev_AvgSpeed = 0;
uint32_t prev_ChargedEnergy_AllTime = 0;
uint32_t prev_DischrgdEnergy_AllTime = 0;
uint32_t prev_AltChrgdEnergy_AllTime = 0;
uint32_t prev_SolarEnergy_AllTime = 0;
int32_t prev_AltFuelUsed_AllTime = 0;
int32_t prev_EngineFuel_AllTime = 0;
int32_t prev_EngineRunTime_AllTime = 0;
int32_t prev_perfSailSeconds = 0;
int32_t prev_perfMotorSeconds = 0;
int32_t prev_EngineCycles_AllTime = 0;
int32_t prev_AltOnTime_AllTime = 0;
uint32_t prev_FieldHeatLimS = 0, prev_FieldCurLimS = 0, prev_FieldHeatLimSes = 0, prev_FieldCurLimSes = 0;
float prev_ChargeCycles_AllTime = 0;
int32_t prev_TotalDist_AllTime = 0;
int32_t prev_AvgSpeed_AllTime = 0;
// Longest-trip persistence shadows. Distance scaled ×100 for 0.01-nm resolution.
int32_t prev_LongestTrip_AT = 0;
int32_t prev_CurrTripDist = 0;
uint32_t prev_CurrTripEpoch = 0;
// 24-hour max distance — watermark only (ring is session-only). Scaled ×100.
int32_t prev_Max24hrDist_AT = 0;
// Deepest anchorage — watermark only (ring is session-only). Scaled ×10 for 0.1-ft resolution.
int32_t prev_DeepAnchor_AT = 0;
// Best upwind VMG (×100, 0.01 kt) + longest gale duration (×100, 0.01 hr) — leaderboard lifetime watermarks.
int32_t prev_BestUpVMG_AT = 0;
int32_t prev_GaleHrs_AT = 0;
double prev_spdAccum_AllTime = -1.0;
uint32_t prev_spdTime_AllTime = 0;
double prev_vltAccum_AllTime = -1.0;
uint32_t prev_vltTime_AllTime = 0;
uint32_t prev_pwrTime_AllTime = 0;
uint64_t prev_timeSinceLastOverheat = 0;
int32_t prev_AvgSOC = 0;
int32_t prev_AvgSOC_AllTime = 0;
static float prev_sailing_days_alltime = -1.0f;
static float prev_sailing_dist_alltime = -1.0f;
static float prev_alt_power_max_alltime_w = -1.0f;
static float prev_solar_power_max_alltime_w = -1.0f;
uint64_t prev_socAccum_AllTime = 0;
uint32_t prev_socTime_AllTime = 0;

// NVS shadow caches — used by saveNVSDataFull() change-detection to skip unchanged keys.
float prev_MaxSpeed = 0.0f;
float prev_MaxSpeed_AllTime = 0.0f;
float prev_MeasAmpsMax = 0.0f;
float prev_MeasAmpsMax_AllTime = 0.0f;
float prev_RPMMax = 0.0f;
float prev_RPMMax_AllTime = 0.0f;
float prev_IBVMax = 0.0f;
float prev_PeakV_AllTime = 0.0f;
float prev_MinVoltage = 0.0f;
float prev_MinVoltage_AllTime = 0.0f;
float prev_board_temp_max = 0.0f;
float prev_board_temp_min = 0.0f;
float prev_baro_max = 0.0f;
float prev_baro_min = 0.0f;
float prev_MaxTempTherm = 0.0f;
float prev_MaxTempTherm_AllTime = 0.0f;
float prev_MaxAltTempF = 0.0f;
float prev_MaxAltTempF_AllTime = 0.0f;
float prev_MaxWindApp = 0.0f;
float prev_MaxWindTrue = 0.0f;
float prev_UVToday = 0.0f;
float prev_UVTomorrow = 0.0f;
float prev_UVDay2 = 0.0f;

// Accumulators for runtime tracking
unsigned long engineRunAccumulator = 0;     // Milliseconds accumulator for engine runtime
unsigned long alternatorOnAccumulator = 0;  // Milliseconds accumulator for alternator runtime

//ALternator Lifetime Prediction
// Thermal Stress Calculation - Settings
const unsigned long THERMAL_UPDATE_INTERVAL = 10000;  // 10 seconds

float WindingTempOffset = 50.0;  // User configurable winding temp offset (°F)
uint8_t displayTempUnit = 0;     // 0 = °F, 1 = °C — display preference, no firmware math changes
uint8_t displayVolUnit = 0;      // 0 = US gallons, 1 = litres — display preference; fuel table stays GPH x100 and the used-fuel totals stay litres

// Console/alarm message formatting only — all control math stays °F-native.
float dispTempF(float tF) {
  return displayTempUnit == 1 ? (tF - 32.0f) / 1.8f : tF;
}
float dispTempDeltaF(float dF) {  // spans/offsets: scale only, no -32 shift
  return displayTempUnit == 1 ? dF / 1.8f : dF;
}
const char* dispTempUnit() {
  return displayTempUnit == 1 ? "°C" : "°F";
}
float PulleyRatio = 2.0;         // User configurable pulley ratio
int ManualLifePercentage = 100;  // Manual override for life remaining %

// Thermal Stress Calculation - Accumulated Damage
float CumulativeInsulationDamage = 0.0;  // 0.0 to 1.0 (1.0 = end of life)
float CumulativeGreaseDamage = 0.0;      // 0.0 to 1.0 (1.0 = end of life)
float CumulativeBrushDamage = 0.0;       // 0.0 to 1.0 (1.0 = end of life)

// Thermal Stress Calculation - Current Status
float InsulationLifePercent = 100.0;  // Current insulation life remaining %
float GreaseLifePercent = 100.0;      // Current grease life remaining %
float BrushLifePercent = 100.0;       // Current brush life remaining %
float PredictedLifeHours = 10000.0;   // Minimum predicted life remaining
int LifeIndicatorColor = 0;           // 0=green, 1=yellow, 2=red


// Timing
unsigned long lastThermalUpdateTime = 0;
const uint32_t INA_SLOW_INTERVAL_MS = 1100;  // field off: AVG=128, CT=4120µs + 50µs temp → ~1061ms update
const uint32_t INA_FAST_INTERVAL_MS = 5;     // field on:  AVG=4,   CT=540µs  → 4.5ms update (bus+shunt+temp)
// ADC_CONFIG (0x01): MODE 0xF continuous | VBUSCT | VSHCT | VTCT | AVG. CT 4 = 540 µs, 7 = 4120 µs, 0 = 50 µs; AVG 1 = 4, 4 = 128 samples
const uint16_t INA_ADC_CFG_SLOW = (0xF << 12) | (7 << 9) | (7 << 6) | (0 << 3) | 4;
const uint16_t INA_ADC_CFG_FAST = (0xF << 12) | (4 << 9) | (4 << 6) | (0 << 3) | 1;
const uint32_t INA_DIETEMP_INTERVAL_MS = 1000;  // die temp has a multi-second thermal time constant; 1 Hz keeps it out of the 5ms IBV path
const uint8_t INA_DIETEMP_WARMUP_SAMPLES = 3;   // DIETEMP holds its 0°C reset value until the first conversion lands (~1.06s at AVG=128); nothing reaches the max until this many valid samples are behind it
const float INA_DIETEMP_STEP_F = 15.0f;         // a jump bigger than this never reaches the max unless the NEXT sample repeats it: a corrupted DIETEMP read cannot corroborate itself, a real thermal rise always does
uint32_t inaReadInterval = INA_SLOW_INTERVAL_MS;
bool inaFastModeActive = false;

// INA228 fast-mode interval stats — updated only when inaFastModeActive, frozen when field off
uint16_t ina_last_ms = 0;
float ina_avg_10s = 0.0f;
uint16_t ina_worst_10s = 0;
uint16_t ina_over2x_10s = 0;
float ina_avg_2m = 0.0f;
uint16_t ina_worst_2m = 0;
uint16_t ina_over2x_2m = 0;
float ina_avg_at = 0.0f;
uint16_t ina_worst_at = 0;
uint32_t ina_over2x_at = 0;

// Physical Constants
const float EA_INSULATION = 1.0f;     // eV, activation energy
const float BOLTZMANN_K = 8.617e-5f;  // eV/K
const float T_REF_K = 373.15f;        // 100°C reference temperature in Kelvin
const float L_REF_INSUL = 50000.0f;   // Reference insulation life at 100°C (hours)
const float L_REF_GREASE = 40000.0f;  // Reference grease life at 158°F (hours)
const float L_REF_BRUSH = 5000.0f;    // Reference brush life at 6000 RPM and 150°F (hours)

//Cool dynamic factors
// Alternator Current Zero Correction (temperature-compensated, ZERO_DRIFT_TEMPCOMP_SPEC.md)
int AutoAltCurrentZero = 0;         // 0=off, 1=on - master enable for applying the zero correction
float DynamicAltCurrentZero = 0.0;  // LIVE correction (amps) recomputed each loop from the fit; subtracted from MeasuredAmps
int ResetDynamicAltZero = 0;        // Momentary reset button (0=normal, 1=reset) — clears the learned fit + history

//Momentary Buttons and alarm logic
int FactorySettings = 0;  // Reset Button
bool alarmLatch = false;        // Current latched alarm state
int AlarmLatchEnabled = 0;      // Whether latching is enabled (0/1 for consistency)
int AlarmTest = 0;              // Momentary alarm test (1 = test active)
bool alarmOutputState = false;  // Single source of truth for GPIO 21 state — mirrors every digitalWrite(21,...)
int ResetAlarmLatch = 0;        // Momentary reset command
unsigned long alarmTestStartTime = 0;
const unsigned long ALARM_TEST_DURATION = 2000;  // 2 seconds test duration
int Alarm_Status;                                // for alarm mirror light on Client
bool alarmSilenced = false;                      // Silence button: buzzer muted for the condition sounding now; Alarm_Status reads 2 while muted. Clears when nothing sounds for 10 s or a condition outside alarmSilencedBits is raised
uint32_t alarmSilencedBits = 0;                 // CheckAlarms alarmBits at the moment of Silence; any bit outside this set sounds again
int SilenceAlarm = 0;                            // Momentary silence command (/get?SilenceAlarm=1, allowed without arming like OnOff=0)

//More Settings
// SOC Parameters
float CurrentThreshold = 0.01f;       // Ignore currents below this (amps)
int PeukertExponent_scaled = 105;     // Peukert exponent × 100 (112 = 1.12)
int ChargeEfficiency_scaled = 990;    // Charging efficiency % × 10 (990 = 99.0%)
int ChargedVoltage_Scaled = 1380;     // Voltage threshold for "charged" (V × 100) (a Battery Monitor setup parameter, nothing to do with alternator)
float TailCurrent = 5.0f;             // % of battery Ah capacity (1 decimal place)
int ShuntResistanceMicroOhm = 100;    // Shunt resistance in microohms
// Single gate for every battery-current consumer (SoC, tail-current, MaintainMode, load-dump, charge limit, CV plant fit); the
// nonzero resistance also keeps the Bcur divide finite. Defined here (compiled first) so every file sees it.
#define HAS_BATT_SHUNT (BatteryShuntPresent && ShuntResistanceMicroOhm > 0)
// Zero-current float (UseFloat=2) parks the CV target at BulkVoltage and relies on uTargetAmps=0, which needs Bcur: with no usable
// shunt it MUST degrade to plain voltage float (else absorption voltage with no current cap or expiry). Every UseFloat==2 branch
// reads this, never UseFloat, so target, timeout, stage label and zeroFloatActive never disagree.
#define EFFECTIVE_USE_FLOAT ((UseFloat == 2 && !HAS_BATT_SHUNT) ? 1 : UseFloat)
int ChargedDetectionTime = 180;       // Time at charged state to consider 100% (seconds) (3 mins, industry standard for lithium)
int IgnoreTemperature = 0;            // If no temp sensor, set to 1
int IgnoreRPM = 0;                    // If RPM sensor absent or malfunctioning, set to 1 to bypass RPM gate
// Run Without Failed Sensors (SENSOR_FAULTS_AND_OPTIONAL_SENSORS_SPEC.md §4.1). All persist in NVS.
int IgnoreAltCurrent = 0;             // run without alternator current: voltage-only regulation (tier 2)
int IgnoreBattVMain = 0;              // run without the INA228 bus voltage: fixed duty capped by the ADS1115 reading (tier 3) or plain fixed duty (tier 4)
int IgnoreBattV2 = 0;                 // run without the ADS1115 battery voltage: plausibility on IBV alone, no disagreement checks
float LimpDutyPct = 30.0f;            // field duty cap (tier 2) / fixed duty (tiers 3-4), % of THIS bus; 12 V default, seeded x fieldScale() and rescaled on a class or winding change like the other duty knobs
float NoRpmCurrentA = 0.0f;           // alternator current limit while engine speed is ignored, A (replaces the RPM table)
// Derived every tick in buildTickSnapshot (6_functions.ino); read by CSV2, CheckAlarms, TempTask, the bus senders.
bool g_engineRunning = false;         // §1.1 engineRunning: spinning held 15 s (the staleness consumers)
bool g_engineSpinningNow = false;     // §1.1 instantaneous: RPM >= 200, or the effective ignition when engine speed is ignored (TempTask cadence)
uint16_t g_sensorFaultMask = 0;       // §1.2 bits: 0 alt temp, 1 engine speed, 2 alt current, 3 main V, 4 second V, 5 V disagree, 6 batt temp, 7 ADS1115, 8 INA228, 9 regulator drive temperature (INA228 die)
uint16_t g_sensorSteadyMask = 0;      // §4.7: bit set once the matching fault bit has been clear continuously for 10 min
uint16_t g_reqFaultBits = 0;          // §11: the fault mask expanded to SENSORS (a dead ADS1115 = engine speed + alt current + second V [+ thermistor]), minus owner overrides / Not fitted. Debounced conditions only
uint8_t  g_reqFaultCount = 0;         // §11: popcount of g_reqFaultBits (a voltage disagreement counts only when neither voltage bit is set). 1 = that sensor's own reason cuts (no automatic degrade: the owner chooses on Run Without Failed Sensors), >= 2 = REASON_MULTI_SENSOR_FAULT
#define SF_ALT_TEMP        (1u << 0)
#define SF_ENGINE_SPEED    (1u << 1)
#define SF_ALT_CURRENT     (1u << 2)
#define SF_BATT_V_MAIN     (1u << 3)
#define SF_BATT_V_SECOND   (1u << 4)
#define SF_BATT_V_DISAGREE (1u << 5)
#define SF_BATT_TEMP       (1u << 6)
#define SF_ADS1115         (1u << 7)
#define SF_INA228          (1u << 8)
#define SF_DRIVE_TEMP      (1u << 9)   // required: the field-drive heat ceiling reads it; overridden only by IgnoreBattVMain (the die reading dies with the chip)
#define SF_ALL_BITS        0x3FFu
uint8_t g_battTempState = 0;          // §2.6: 0 not expected, 1 reading, 2 lost, 3 waiting for the first reading (field held off)
uint8_t g_battCurState = 0;           // §11.6 battery current (optional, declared by BatteryShuntPresent): 0 not declared, 1 reading, 2 lost 10 s (alarm + header line, never a cut)
uint8_t g_limpTier = 0;               // §4.2
bool g_altAmpsOutOfRange = false;     // CH1 reading outside the sensor's sane window (held value in MeasuredAmps)
bool g_ibvStale = false;              // §10.1 stale condition, computed in _ReadAnalogInputs_inner
bool g_inaQuietConfig = false;        // inaApplyConfig()/updateINA228OvervoltageThreshold(): skip the info lines during a runtime re-arm
volatile bool inaOvReprogramPending = false;  // web-task setters raise it, loop() runs updateINA228OvervoltageThreshold(): Wire drops its lock between register-pointer write and read, so a Core 0 transaction in that gap hits the wrong INA228 register
bool g_battVStale = false;            // §10.2 stale condition
int MinRPMForField = 125;             // Field is cut when RPM is below this threshold (crank RPM; two consecutive low readings while the field is on). Floored at the 100-RPM noise floor in ReadAnalogInputs (1-100 act alike). Clamped >= 0; 0 drops only the below-minimum cuts: the zero cut still cuts, and the field stays off until RPM reads nonzero for RPM_RESTART_CONFIRM_MS.
// Engine-stopped fast cut on a confirmed zero RPM, not the graceful ramp: at true 0 RPM there is no output (no load-dump risk), and
// an energized field couples PWM noise into the LM2907 as phantom RPM. RpmLowConfirm needs a second zero reading as well as the window.
#define RPM_ZERO_CUT_MS 200          // ms RPM must hold at exactly 0 before the immediate cut
// Hard bound when no second zero reading arrives; above the worst field-on CH2 gap ever logged (283 ms with loop stalls,
// ~70-80 ms normally), so a live tach always delivers its second reading first.
#define RPM_ZERO_MAX_WAIT_MS 300
// Spin-down cut and the low-RPM gate cutting an energized field act on the second consecutive CH2 reading below MinRPMForField
// (newer dataTimestamps[IDX_RPM]; rpmUnheldReadMs for the gate while tach-lie is armed), no wall-clock minimum; the tach-lie count
// likewise. A lone glitched read must never cut mid-ramp (an abrupt cut slams the LM2907 coupling cap → ~4.6 s false-zero). Do not
// add a minimum dwell: a phantom landing inside it reset the confirmation and nothing cut.
// Hard bound when no second reading arrives (failed CH2 reads, dead ADS1115, loop stall): cut anyway. 2 × the worst field-on
// CH2 gap (~80 ms); under RPM_ZERO_CUT_MS so this path stays the faster one.
#define RPM_BELOWMIN_MAX_WAIT_MS 160
// The same bound while tach-lie is armed: the read lone-jump rejection holds back doesn't count, so the clearing reading is the
// third after a glitch (~160 ms out); 3 gaps avoids racing it.
#define RPM_BELOWMIN_MAX_WAIT_ARMED_MS 240
// dataTimestamps[IDX_RPM] of the last CH2 read the lone-jump rejection did NOT hold back; the tach-lie count and the armed
// low-RPM gate take their second low read from it.
uint32_t rpmUnheldReadMs = 0;
// Protection-event RPM grace: for this long after any protection clamp tick an EXACTLY-zero RPM is tach dropout (a clamp near
// MinDuty can starve the LM2907), so neither the below-minimum nor zero cut fires on it; nonzero-low reads gate normally and
// rpmZeroConfirm keeps counting so a real stall cuts when it expires. Charging-enabled only: a shutdown cuts ungated.
#define PROT_RPM_GRACE_MS 5000
// Tach-lie plausibility cut: tach says running, the loop is COMMANDING current with the field well above the onset floor, and the
// alternator makes nothing — impossible above cut-in. Causes: RPM noise (PWM coupling / LM2907 re-bias), a dead alternator, or an
// open field drive; cut + escalating lockout (nextTachLieLockoutMs) suits all three. No RPM-only gate catches a phantom between
// MinRPMForField and the 1000-RPM lone-jump bar. Arm bar is install-relative, never absolute duty: zero output at the knee floor is
// by design, and headroom over ccDutyCeiling() (not raw MaxDuty) inherits class/winding/Max Field Volts scaling.
#define TACH_LIE_HEADROOM_FRAC 0.15f  // arm bar = rpmMinDuty + this fraction of (ccDutyCeiling() - rpmMinDuty)
#define TACH_LIE_DWELL_MS 3000       // condition must persist this long — outlasts ramp/settling transients
#define TACH_LIE_RELEASE_BLANK_MS 6000  // no rev-up release decision until the LM2907 settles post-cut: covers the ~40 ms collapse spike (~570 ms after cut, observed) and the full ~4.6 s re-bias interval (doc figure)
#define TACH_LIE_RELEASE_HOLD_MS 300    // rev-up must hold this long continuously (~10 tach samples) — a lone spike frees nothing
bool TachLieEnable = true;           // user disarm (NVS). Cut ignores the global protections toggle — this switch is the only disarm
uint32_t tachLieSinceMs = 0;         // millis() when the implausible condition started (0 = clear)
float tachLieTripRpm = 0.0f;         // RPM latched at trip — early-release rev-up reference
bool tachLieLockoutArmed = false;    // active lockout was armed by a tach-lie trip (enables early release)
uint32_t tachLieRevUpSinceMs = 0;    // rev-up release test continuously true since (0 = not currently true)
// Engine-restart confirmation: after a confirmed stop, RPM must hold >= MinRPMForField and above 0 this long
// before the RPM gate releases — a noise blip (or a phantom's first seconds) can't re-energize the
// field. Costs ~2 s of charging at every real engine start. Boot starts unconfirmed.
#define RPM_RESTART_CONFIRM_MS 2000
bool rpmRestartPending = true;       // set on confirmed stop, cleared by sustained above-min RPM
uint32_t rpmAboveMinSinceMs = 0;     // millis() when RPM first held >= MinRPMForField and above 0 (0 = below)
uint32_t g_lastProtClampMs = 0;      // millis() of the most recent tick any protection clamp was active
// millis() of the last ABRUPT protection cut (applyImmediateCut returns before the clamp block that stamps g_lastProtClampMs).
// The cut slams the LM2907 coupling cap, so the RPM gates mask off this stamp.
uint32_t g_lastFieldCutMs = 0;
// Field-drain early release: g_ovClampRiseMs stamps the FIRST tick of a clamp episode. Once a clamp has held for the commissioned
// drain at this speed (fdDrainMsAtRpm at g_ovClampRpm), the coil current is gone, so G2/iExcess hold latches release without the
// lagged filtered-voltage return (iExcess also waits for its EMA). OR'd with the natural release: only SHORTENS a cut; Load Dump untouched.
uint32_t g_ovClampRiseMs = 0;
float g_ovClampRpm = -1.0f;          // RPM latched at the same rising edge — the cut corrupts the tach (~4.6 s false-zero), so live RPM during a clamp is garbage; <=0 → worst-case drain
uint32_t g_tauReleaseCount = 0;      // episodes where the tau timer elapsed against an OV/iExcess-owned clamp
int bmsLogic = 0;                     // if BMS is asked to turn the alternator on and off
int bmsLogicLevelOff = 0;             // set to 0 if the BMS gives a low signal (<3V?) when no charging is desired
bool chargingEnabled;                 // defined from other variables
bool bmsSignalActive = false;         // Read from GPIO42 (live pin state, published on CSV4 for the Integrations status row)
int AlarmActivate = 0;                // set to 1 to enable alarm conditions
int GeneralFaultAlarm = 1;            // Alarms page: sound for faults with no threshold of their own (sensor/chip faults, TempTask stall, INA228 hardware OV latch). 0 = only the thresholds on that page and the charge lockouts
int TempAlarm = 190;                  // above this value, sound alarm
int TempAlarmLow = 32;                // below this value, sound alarm (0 = disabled)
float VoltageAlarmHigh = 14.8f;       // above this value, sound alarm (V, 2 decimals)
float VoltageAlarmLow = 11.9f;        // below this value, sound alarm (V, 2 decimals)
int SocAlarmLow = 10;                 // sound alarm when SoC falls below this % (0 = disabled; needs socInfoAvailable)
int CurrentAlarmHigh = 100;           // above this value, sound alarm
int MaximumAllowedBatteryAmps = 125;  // safety for battery, optional
int RPMScalingFactor = 1470;          // adjust until it matches your trusted tachometer
float AlternatorCOffset = 0;          // tare for alt current
float BatteryCOffset = 0;             // tare or batt current
int timeToFullChargeMin = -999;  // -999 = not applicable, the same marker calculateChargeTimes() writes
int timeToFullDischargeMin = -999;  // same marker as its charge twin


// fields 52-57 in CSVData2 are reserved zeros (Reset* flags were always 0; buttons use hasParam, not these vars)
int ResetDischargedEnergy;  // total discharged from battery
int ResetFuelUsed;          // fuel used by alternator
int ResetAlternatorChargedEnergy;
int ResetEngineCycles;
int ResetRPMMax;
int ResetThermTemp = 0;  // Max thermistor temp reset

unsigned long fieldCollapseTime = 0;
unsigned long FIELD_COLLAPSE_DELAY = 30000;  // ms
// Lockout after temperature or voltage-disagreement faults. RPM-too-low is not a fault and gets RPM_RECOVERY_DELAY; fast OV and the
// timed OV tiers use the adaptive nextFastOvLockoutMs ladder (0.5 s → 10 s) so a blip doesn't cost 30 s while a persistent cause
// escalates. Not user-adjustable by design.
unsigned long RPM_RECOVERY_DELAY = 500;  // ms — short by design: while stopped this cooldown re-arms every window (6_functions runShutdownPath), so a restart waits out the remainder.
// Delay applied to the CURRENTLY-armed lockout, set at arm time (RPM_RECOVERY_DELAY for the RPM gate,
// else FIELD_COLLAPSE_DELAY). The remaining/clear/inLockout checks read this, not the constant.
unsigned long activeCollapseDelay = 30000;  // ms
// Charging resumes automatically once this expires and the fault has cleared. INA228 hardware OV is NOT governed by it: its own
// 10 s latch in CheckAlarms() bypasses the lockout via shouldImmediatelyCutGPIO4().
int fieldActiveStatus = 0;  // direct read of ESP32 hardare to control the field active light in Banner
volatile uint8_t g_fieldEventReason = 0;  // last field-control reason (FieldEventReason enum) for telemetry — annotates the banner OFF word; 0 = NONE


int Voltage_scaled = 0;         // Battery voltage scaled (V × 100)
int BatteryCurrent_scaled = 0;  // A × 100
int BatteryPower_scaled = 0;    // Battery power (W × 100)
int EnergyDelta_scaled = 0;     // Energy change (Wh × 100)
float AlternatorFuelUsed = 0;   // Total fuel used by alternator (mL) - INTEGER (note: mL not L)
bool alternatorIsOn = false;    // Current alternator state
// Energy Tracking Variables
unsigned long ChargedEnergy = 0;            // Total charged energy from battery (Wh)
unsigned long ChargeCycleEnergy = 0;        // Wh charged since the session Charge Cycles Reset: its own tally, so that Reset sticks without clearing ChargedEnergy
unsigned long DischargedEnergy = 0;         // Total discharged energy from battery (Wh)
unsigned long AlternatorChargedEnergy = 0;  // Total energy from alternator (Wh)
int FuelEfficiency_scaled = 250;            // Engine efficiency: Wh per mL of fuel (× 100)
// Engine & Alternator Runtime Tracking
int EngineRunTime = 0;          // Time engine has been spinning (SECONDS — incremented by secondsRun)
int EngineCycles = 0;           // Average RPM * Minutes of run time
int AlternatorOnTime = 0;       // Time alternator has been producing current (SECONDS — incremented by secondsRun)
bool engineWasRunning = false;  // Engine state in previous check


// CH1 interval instrumentation, zero heap: 10 s ring, 2 m ring of 12 closed 10 s buckets (latest <=10 s missing), all-time
// scalars. CH1_RING must hold at least 10000 / (min expected CH1 interval ms).
uint16_t ch1_last_ms = 0;
float ch1_avg_10s = 0;
uint16_t ch1_worst_10s = 0;
uint16_t ch1_over2x_10s = 0;
uint16_t ch1_n_10s = 0;
float ch1_avg_2m = 0;
uint16_t ch1_worst_2m = 0;
uint32_t ch1_over2x_2m = 0;
uint32_t ch1_n_2m = 0;
float ch1_avg_at = 0;
uint16_t ch1_worst_at = 0;
uint32_t ch1_over2x_at = 0;
uint32_t ch1_n_at = 0;
uint16_t ch1_worst_fieldon = 0;  // worst CH1 read interval with the field gate open (hardware gate), ms — the field-blind worst above is inflated by harmless field-off flush passes

#define CH1_RING 5000
#define CH1_BUCKETS 12  // 12 closed buckets × 10 s = 2-minute window

struct Ch1Entry {
  uint32_t ts;  // millis() at read completion
  uint16_t iv;  // interval since previous CH1 read, ms (saturates at 65535)
};

struct Ch1Bucket {
  uint32_t sum;     // sum of all intervals in this bucket, ms
  uint16_t count;   // number of CH1 reads in this bucket
  uint16_t worst;   // maximum interval seen, ms
  uint16_t over2x;  // count of intervals > 2× this bucket's own mean
};

static Ch1Entry *ch1Ring = nullptr;
static uint16_t ch1Head = 0;
static uint16_t ch1Count = 0;  // valid entries; saturates at CH1_RING

// 2-minute bucket ring
static Ch1Bucket ch1Buckets[CH1_BUCKETS];
static uint8_t ch1BktHead = 0;
static uint8_t ch1BktCount = 0;
static uint32_t ch1BktStart = 0;  // millis() when current bucket opened
// ── 1s mini-buckets for O(1) 10s window stats (eliminates ring scan) ──────
#define CH1_1S_BUCKETS 10
Ch1Bucket ch1Bkt1s[CH1_1S_BUCKETS];          // closed 1s buckets ring (internal RAM, ~120 bytes)
Ch1Bucket ch1Bkt1sCurrent = { 0, 0, 0, 0 };  // currently open/accumulating 1s bucket
uint8_t ch1Bkt1sHead = 0;                    // next write slot in ch1Bkt1s[]
uint8_t ch1Bkt1sCount = 0;                   // how many closed 1s buckets are valid
uint32_t ch1Bkt1sStart = 0;                  // millis() when current 1s bucket opened
// ── End 1s mini-bucket globals ─────────────────────────────────────────────

// All-time accumulators
static uint64_t ch1AtSum = 0;  // 64-bit: survives multi-hour sessions
static uint32_t ch1AtCount = 0;
static uint16_t ch1AtWorst = 0;
static uint32_t ch1AtOver2x = 0;  // threshold = running mean at sample time (approximation)

// State
static uint32_t ch1PrevTs = 0;
static bool ch1HasPrev = false;

// Inner current PID firing interval: field-on-gated clone of the CH1 tracker (pidFire_record() sits past the GPIO4-HIGH gate;
// loop() clears pfHasPrev while the field is down so the first firing re-baselines). No ring.
uint16_t pf_last_ms = 0;
float    pf_avg_10s = 0;
uint16_t pf_worst_10s = 0;
uint16_t pf_over2x_10s = 0;  // mirrors CH1: not tracked at 1s granularity (always 0)
float    pf_avg_2m = 0;
uint16_t pf_worst_2m = 0;
uint32_t pf_over2x_2m = 0;
float    pf_avg_at = 0;
uint16_t pf_worst_at = 0;
uint32_t pf_over2x_at = 0;

#define PF_BUCKETS 12      // 12 closed buckets × 10 s = 2-minute window
#define PF_1S_BUCKETS 10   // 1s mini-buckets for O(1) 10s window stats
static Ch1Bucket pfBuckets[PF_BUCKETS];
static uint8_t pfBktHead = 0;
static uint8_t pfBktCount = 0;
static uint32_t pfBktStart = 0;
Ch1Bucket pfBkt1s[PF_1S_BUCKETS];
Ch1Bucket pfBkt1sCurrent = { 0, 0, 0, 0 };
uint8_t pfBkt1sHead = 0;
uint8_t pfBkt1sCount = 0;
uint32_t pfBkt1sStart = 0;
static uint64_t pfAtSum = 0;
static uint32_t pfAtCount = 0;
static uint16_t pfAtWorst = 0;
static uint32_t pfAtOver2x = 0;
static uint32_t pfPrevTs = 0;
bool pfHasPrev = false;

// CV voltage-loop firing interval: clone of the pf tracker, recorded only while voltageControlActive; vlHasPrev is cleared
// while CV is inactive so the first firing re-baselines.
uint16_t vl_last_ms = 0;
float    vl_avg_10s = 0;
uint16_t vl_worst_10s = 0;
uint16_t vl_over2x_10s = 0;
float    vl_avg_2m = 0;
uint16_t vl_worst_2m = 0;
uint32_t vl_over2x_2m = 0;
float    vl_avg_at = 0;
uint16_t vl_worst_at = 0;
uint32_t vl_over2x_at = 0;

#define VL_BUCKETS 12      // 12 closed buckets × 10 s = 2-minute window
#define VL_1S_BUCKETS 10   // 1s mini-buckets for O(1) 10s window stats
static Ch1Bucket vlBuckets[VL_BUCKETS];
static uint8_t vlBktHead = 0;
static uint8_t vlBktCount = 0;
static uint32_t vlBktStart = 0;
Ch1Bucket vlBkt1s[VL_1S_BUCKETS];
Ch1Bucket vlBkt1sCurrent = { 0, 0, 0, 0 };
uint8_t vlBkt1sHead = 0;
uint8_t vlBkt1sCount = 0;
uint32_t vlBkt1sStart = 0;
static uint64_t vlAtSum = 0;
static uint32_t vlAtCount = 0;
static uint16_t vlAtWorst = 0;
static uint32_t vlAtOver2x = 0;
static uint32_t vlPrevTs = 0;
bool vlHasPrev = false;

// variables used to show how long each loop takes
uint64_t starttime;
uint64_t endtime;
uint64_t LoopTime;                  // must not use unsigned long becasue cant run String() on an unsigned long and that's done by the wifi code
int WifiStrength = -999;            // must not use unsigned long becasue cant run String() on an unsigned long and that's done by the wifi code
int MaximumLoopTime;                // must not use unsigned long becasue cant run String() on an unsigned long and that's done by the wifi code
unsigned long prev_millis7888 = 0;  // used to reset the meximum loop time
// ── Per-function worst-case timing ───────────────────────────────────────────
// worstWindow resets every FUNC_TIMING_WINDOW_MS — shows current spike behavior
// worstSession resets only at boot — never lies about session worst
struct FuncTiming {
  uint32_t worstWindow;
  uint32_t worstSession;
  uint32_t lastCall;  // Most recent individual call duration (µs)
  uint32_t passSeq;   // ftLoopPassSeq at the time of lastCall — lets blame capture tell "ran this pass" from stale data
};
uint32_t ftLoopPassSeq = 0;  // increments at the top of every loop() pass (Core-0 TIMED_CALLs stamp whatever value is current — harmless, they're excluded from the blame registry)
// Worst field-on pass blame — top 3 timed consumers of the pass that set loopFieldOnSes.
// Captured only when a new session worst lands (rare), so attribution costs nothing on normal passes.
uint8_t loopBlameIdx[3] = { 255, 255, 255 };  // index into ftBlameReg[] (255 = empty slot)
uint32_t loopBlameUs[3] = { 0, 0, 0 };

// One instance per timed function
FuncTiming ft_ReadAnalogInputs;
FuncTiming ft_AdjustFieldLearnMode;
FuncTiming ft_logDashboardValues;
FuncTiming ft_updateSystemHealthStats;
FuncTiming ft_checkWiFiConnection;
FuncTiming ft_SendWifiData;
FuncTiming ft_CheckAlarms;
FuncTiming ft_calculateDerivedMetrics;
FuncTiming ft_ch1_compute_stats;
FuncTiming ft_uploadSensorHistory;
FuncTiming ft_dumpLongTermRing;  // 15-min long-term-ring flash flush (field-off only) — can cause ~250ms loop spikes
FuncTiming ft_uploadBufferedRecords;
FuncTiming ft_buildConfigPayload;
FuncTiming ft_UpdateBatterySOC;  // ~0.3ms; also wraps battery-health capacity tracking (capTrackTick/capTrackOnFull)
FuncTiming ft_updateSensorWindow;
FuncTiming ft_checkTimeSync;
FuncTiming ft_rai_total;           // ReadAnalogInputs() — full function including flash writes
FuncTiming ft_rai_ina228;          // INA228 read block only
FuncTiming ft_rai_ads_state;       // ADS1115 state machine — cost per state step
FuncTiming ft_rai_bmp_state;       // BMP388 state machine — cost per state step
FuncTiming ft_rai_imu;             // IMU FIFO drain block
FuncTiming ft_updateAccelMetrics;  // accel ring-buffer processing (updateAccelMetrics)
FuncTiming ft_ReadVEData;
FuncTiming ft_ReadNMEA0183;  // NMEA 0183 drain — the number that proves the 0.5 ms control-loop budget is held
FuncTiming ft_altHealth;
FuncTiming ft_altFold;     // per-control-tick (~75 Hz) alt fold (IDW eval cost) — distinct from ft_altHealth (SSE wrapper)
FuncTiming ft_boatPerf;
// Record-book lookup engine (FrontQuery, 7_functions.ino), shown on /debug. Per-pass timers are nested inside
// ft_altHealth / ft_boatPerf, so they stay out of the blame registry.
FuncTiming ft_altEngine;   // per loop pass: the 1 Hz alt-health job (begin, lookups, AltLive, settings echo)
FuncTiming ft_perfEngine;  // per loop pass: the boat-performance lookup queue
uint32_t altJobLastUs = 0, altJobWorstUs = 0;          // one 1 Hz alt job's total work, all passes
uint32_t altJobLastSpanMs = 0, altJobWorstSpanMs = 0;  // tick to finish
uint16_t altJobLastPasses = 0, altJobLastLookups = 0, altJobWorstLookups = 0;
uint32_t perfEngLastSecUs = 0, perfEngWorstSecUs = 0;  // boat-perf lookup work per wall-clock second
uint32_t frontDropCount = 0;   // lookups dropped by a front write (reset, import, Uploaded load, class change, demo swap)
// /debug/frontcheck?drive=N: synthetic lookups each second until millis() reaches this, field cut only (0 = off).
uint32_t frontDriveUntilMs = 0;
bool frontDriveOld = false;    // drive the inline (pre-engine) reads instead of the engine
FuncTiming ft_n2kTx;  // NMEA2000 transmit tick — proof the TX path stays out of the control loop's time budget
FuncTiming ft_dvcc;   // DVCC follow brain (1 Hz trust state machine) — proof it stays out of the loop's time budget
FuncTiming ft_n2kParse;  // NMEA2000.ParseMessages() — RX frame drain + fast-packet assembly; the only N2K cost that scales with bus traffic
FuncTiming ft_huntGov;  // oscillation-damper evaluation — self-gated to one Goertzel pass per 32 control ticks; instrumented because it sits inline in the control loop
FuncTiming ft_fastAltDrain;     // fast alt-current channel bounded DMA drain (~1 ms cap per loop pass)
FuncTiming ft_faMatrixFlush;    // fast alt-current disturbance matrix → flash (field-off gated, like the long-term ring)
FuncTiming ft_faDetector;       // fast alt-current failure-detector verdict consume on Core 1 (analysis runs on the Core-0 faDetTask); ~0 cost
FuncTiming ft_faWindowFinalize; // fast alt-current per-2s-window finalize (Goertzel finalize + matrix fold + detector arm), runs inside faDrain
// Field-off flash/NVS writers called from loop(), timed like ft_dumpLongTermRing: a rare dirty-gated write otherwise shows as
// an unattributed loop spike.
FuncTiming ft_zeroLogService;   // zero-drift diagnostic: dumpZeroLog() LittleFS flush (field-off, 30-min, dirty-gated)
FuncTiming ft_solarLedger;      // solar ledger: 1 Hz day roll (microseconds) + field-cut-only LittleFS/NVS flush
FuncTiming ft_bhFlushCapNVS;    // Battery Health: LittleFS blob write of capacity ring + results (field-off, dirty-gated)
FuncTiming ft_kneeLearnService; // Auto Min% learning: NVS write of learned onset floors (field-off, 5-min, dirty-gated)

// Fast alt-current per-session worst scalars (fleet upload). Declared here because buildConfigPayload() reads them earlier in
// 2_functions.ino. Persisted; cleared only by Reset Worsts (ResetRipplePeaks).
float faSesPkpkWorstA = 0.0f;   // worst broadband pk-pk since last manual reset, amps (persisted)
float faSesPeakWorstA = 0.0f;   // strongest single spectral peak since last manual reset, amps (persisted)
float faSesPeakWorstHz = 0.0f;  // ...and its frequency, Hz (persisted)
// NVS change-detection shadows for the persisted ripple worsts (auto-fire scalar path)
int32_t prev_faSesPkpk = 0;
int32_t prev_faSesPeakA = 0;
int32_t prev_faSesPeakHz = 0;
// ...and for the Highest Tone in Map headline (faDomAmpAX100/faDomFreqHzX10/faDomRpm in 2_functions.ino)
int32_t prev_faDomAmp = 0;
int32_t prev_faDomFreq = 0;
int32_t prev_faDomRpm = 0;
// Operating point at the instant each headline worst was set (dashboard mini-tables). Persisted with the worsts; cleared by
// the same Reset Worsts / Clear Map paths.
float    faSesPkpkAmpsA = 0.0f;   // window-mean output current where the worst pk-pk occurred, amps
float    faSesPkpkTempF = NAN;    // alternator temperature there, °F (NAN = no probe)
uint16_t faSesPkpkRpm   = 0;      // RPM (bin center) there
uint32_t faSesPkpkEpoch = 0;      // wall-clock epoch when set (0 = clock not synced)
float    faDomAmpsA     = 0.0f;   // window-mean output current at the highest tone, amps
float    faDomTempF     = NAN;    // alternator temperature there, °F
uint32_t faDomEpoch     = 0;      // wall-clock epoch when set (0 = clock not synced)
int32_t prev_faSPkAmp = 0;
int32_t prev_faSPkTmp = 0;
int32_t prev_faSPkRpm = 0;
int32_t prev_faSPkEp = 0;
int32_t prev_faDomAmps = 0;
int32_t prev_faDomTmp = 0;
int32_t prev_faDomEp = 0;

// Telemetry aliases: *Time2 = last call duration, *Time = worst-in-window.
#define AnalogReadTime ft_rai_ina228.worstWindow
#define AnalogReadTime2 ft_rai_ina228.lastCall
#define IMUReadTime ft_rai_imu.worstWindow
#define IMUReadTime2 ft_rai_imu.lastCall
// Records last, worst-window and worst-session; nested calls are included in the outer timer.
#define TIMED_CALL(ft, call) \
  do { \
    uint32_t _t0 = (uint32_t)esp_timer_get_time(); \
    call; \
    uint32_t _dt = (uint32_t)esp_timer_get_time() - _t0; \
    (ft).lastCall = _dt; \
    (ft).passSeq = ftLoopPassSeq; \
    if (_dt > (ft).worstWindow) (ft).worstWindow = _dt; \
    if (_dt > (ft).worstSession) (ft).worstSession = _dt; \
  } while (0)

// Blame registry: loop-resident TOP-LEVEL timers only (nested ft_rai_* / ft_faWindowFinalize would double-bill; Core-0 timers'
// passSeq doesn't track loop passes). Order = the blame index sent in CSV2 — MUST mirror FT_BLAME_NAMES in script.js.
FuncTiming *const ftBlameReg[] = {
  &ft_ReadAnalogInputs, &ft_ReadVEData, &ft_AdjustFieldLearnMode, &ft_calculateDerivedMetrics,
  &ft_CheckAlarms, &ft_UpdateBatterySOC, &ft_logDashboardValues, &ft_updateSystemHealthStats,
  &ft_updateSensorWindow, &ft_updateAccelMetrics, &ft_altHealth, &ft_boatPerf, &ft_huntGov,
  &ft_ch1_compute_stats, &ft_SendWifiData, &ft_checkWiFiConnection, &ft_checkTimeSync,
  &ft_uploadSensorHistory, &ft_uploadBufferedRecords, &ft_buildConfigPayload,
  &ft_dumpLongTermRing, &ft_faMatrixFlush, &ft_fastAltDrain, &ft_faDetector,
  &ft_zeroLogService, &ft_bhFlushCapNVS, &ft_kneeLearnService, &ft_n2kTx, &ft_dvcc, &ft_solarLedger,
  &ft_n2kParse, &ft_ReadNMEA0183
};
constexpr uint8_t FT_BLAME_N = sizeof(ftBlameReg) / sizeof(ftBlameReg[0]);

// Runs ONLY when a field-on pass sets a new session worst: rank this pass's timed calls,
// keep the top 3. ~27 compares, microseconds — never on the normal-pass path.
void captureLoopBlame() {
  for (int s = 0; s < 3; s++) { loopBlameIdx[s] = 255; loopBlameUs[s] = 0; }
  for (uint8_t i = 0; i < FT_BLAME_N; i++) {
    if (ftBlameReg[i]->passSeq != ftLoopPassSeq) continue;  // stale — didn't run this pass
    uint32_t us = ftBlameReg[i]->lastCall;
    for (int s = 0; s < 3; s++) {
      if (us > loopBlameUs[s]) {
        for (int m = 2; m > s; m--) { loopBlameUs[m] = loopBlameUs[m - 1]; loopBlameIdx[m] = loopBlameIdx[m - 1]; }
        loopBlameUs[s] = us;
        loopBlameIdx[s] = i;
        break;
      }
    }
  }
}

uint32_t loopTime5sWindow = 0;                     // worst loop time in last AinputTrackerTime window (µs)
constexpr unsigned long AinputTrackerTime = 5000;  // rolling window reset interval (ms)
// Previous session max loop time — snapshot of MaxLoopTime taken at boot before reset
uint32_t prevSessionMaxLoopTime = 0;  // worst loop time from the session before this one (µs)

// ── Worst-loop attribution diagnostic (loop-stall hunt) ──────────────────────
// Each new worst pass above the threshold prints ONE console line with its per-function breakdown (diagnostic only; re-armed
// by Reset Peak Values). Remove once the stall is closed.
#define LOOP_DIAG_THRESH_US 10000  // only report passes over 10 ms
uint32_t loopDiagWorstUs = 0;

// ── Dashboard-stream backpressure witnesses ───────────────────────────────────
// A client's SSE queue drains only as its TCP acks, so it backs up during lwIP retransmit waits (1.5 s, doubling) that the
// WiFi/stream-drop lines can't see. Depth polled in SendWifiData; the library drops the NEWEST message on a full queue (ESP warn
// only), so sseSend() counts those. Cleared by Reset Peak Values; on /debug; each stall >= 1 s prints a console line.
uint16_t sseQueueDepth = 0;        // last polled depth (messages, averaged over every client) - /debug only
uint16_t sseGateDepth = 0;         // the same averaged over the clients still draining (a stalled one left out) - gates the secondary channels
uint16_t sseQueuePeak = 0;         // deepest since reset
uint16_t sseQueueDeepest = 0;      // last polled depth of the deepest client's queue (/debug only)
uint16_t sseQueueDeepestPeak = 0;  // its deepest since reset
uint32_t sseStallCount = 0;        // stalls >= 1 s since reset
uint32_t sseStallLongestMs = 0;
uint32_t sseFramesDropped = 0;     // events.send() calls not fully enqueued since reset
// Every dashboard event (3_functions.ino). toStalled: history and settings echoes (CSV1, console, CSV3) queue for a stalled
// client too; the periodic current-state channels skip it while another client drains.
void sseSend(const char *msg, const char *evt, bool toStalled = false);
// Victron GX stream (/gxevents) witnesses, on /debug, cleared by Reset Peak Values.
uint32_t gxFramesSent = 0;         // frames the Core-0 GX sender handed to gxEvents
uint32_t gxFramesHeld = 0;         // sender passes that found the GX link backlogged (owed frames wait for the next try)
uint32_t gxFramesTorn = 0;         // copies Core 1 rebuilt mid-read (retried with the newer frame)
uint32_t gxFramesDropped = 0;      // gxEvents.send() calls not fully enqueued
uint32_t gxStreamDrops = 0;        // GX stream disconnects since reset
static const size_t PAYLOAD3_SIZE = 8192;   // settings echo (CSV3) frame buffer, PSRAM; ~7 B/field
uint16_t csv3LastLen = 0;          // last CSV3 frame length; peak since reset. The margin against PAYLOAD3_SIZE, on /debug
uint16_t csv3PeakLen = 0;

// ── 80MHz low-power-mode loop instrumentation ─────────────────────────────────
// Engine-off runs at 80MHz; tracked there only (gated on getCpuFrequencyMhz()<81). The accel FIFO net-fills if one pass exceeds
// ~38 ms (6-sample drain / 156 sps), so loopOver80ImuLimitCount counts near-misses; pair with imu_fifo_overrun_count.
const uint32_t LOOP80_IMU_LIMIT_US = 38000;  // ~38ms: accel FIFO drain budget per poll
uint32_t loopWorst80Win = 0;           // worst 80MHz loop pass, rolling 5s window (µs)
uint32_t loopWorst80Ses = 0;           // worst 80MHz loop pass since Reset Peak Values (µs)
uint32_t loopFieldOnWin = 0;           // worst field-ON loop pass, rolling 5s window (µs)
uint32_t loopFieldOnSes = 0;           // worst field-ON loop pass since Reset Peak Values (µs)
uint32_t loopOver80ImuLimitCount = 0;  // # of 80MHz passes over LOOP80_IMU_LIMIT_US since reset
uint32_t loop80IterCount = 0;          // total 80MHz passes since reset (denominator for the above)

// ── End per-function timing ───────────────────────────────────────────────────



// Health-requested maintenance reboot: latched by checkAndRestart() after 30 min below REBOOT_HEAP_FLOOR_KB; runs at the next
// quiet window regardless of the 24 h minimum. Only a reboot clears it.
bool maintRebootHealthReq = false;
bool systemShuttingDown = false;
// Restart runs in loop() so the HTTP response flushes first, never inside the async TCP callback.
volatile bool rebootRequested = false;
volatile unsigned long rebootRequestedAt = 0;  // writers store this BEFORE the flag; both volatile so the order holds
// Who asked, for the reboot's serial line and the next boot's reset reason; set with rebootRequestedAt. /get?RebootRegulator
// leaves REBOOT_WHY_REMOTE.
#define REBOOT_WHY_REMOTE 0
#define REBOOT_WHY_IMPORT 1       // config or records import
#define REBOOT_WHY_WIFI_SETUP 2   // the Wi-Fi setup page saved
#define REBOOT_WHY_IMPROV 3       // Wi-Fi sent over USB (Improv)
#define REBOOT_WHY_CFG_PUSH 4     // cloud settings push applied
#define REBOOT_WHY_FACTORY 5      // switched to the factory installer for a firmware update
#define REBOOT_WHY_COUNT 6
volatile uint8_t rebootRequestWhy = REBOOT_WHY_REMOTE;
// Per REBOOT_WHY_*: the reboot's serial line, the reset reason the next boot records (LastResetReason; script.js
// resetReasonLookup), and that boot's "reset:" name
const char *const REBOOT_WHY_LINE[REBOOT_WHY_COUNT] = {
  "=== REMOTE REBOOT (/get?RebootRegulator) ===",
  "=== IMPORT REBOOT (loading the imported settings) ===",
  "=== WIFI SETUP REBOOT (joining with the saved Wi-Fi settings) ===",
  "=== IMPROV WIFI REBOOT (joining the network sent over USB) ===",
  "=== CLOUD SETTINGS PUSH REBOOT (loading the pushed settings) ===",
  "=== FACTORY SWITCH REBOOT (the factory installer runs the update) ===" };
const uint8_t REBOOT_WHY_RESET[REBOOT_WHY_COUNT] = { 1, 17, 18, 19, 20, 21 };
const char *const REBOOT_WHY_NAME[REBOOT_WHY_COUNT] = { "software", "software, import restart",
  "software, Wi-Fi setup restart", "software, Improv Wi-Fi restart", "software, cloud settings push restart",
  "software, factory switch restart" };
uint8_t g_bootRebootWhy = REBOOT_WHY_REMOTE;   // this boot's, from g_rebootIntent (captureResetReason)
// Maintenance-reboot countdown for the dashboard banner (0 = none; CSV2). Only the relaxed/hard tiers count down; quiet-tier
// reboots have no client by construction.
uint32_t restartRemainingSec = 0;
const unsigned long RESTART_WARNING_WINDOW_MS = 600000UL;  // 10 minutes

// Network battery monitor (Setup > Integrations "Use as Battery Monitor"): 0 none, 1 NMEA 2000 (n2kRxBattInstance,
// 127508 + 127506), 3 VE.Direct BMV/SmartShunt. Its own SoC is the SoC shown and used, never re-integrated here. With
// no regulator shunt its current also drives the taper exit and current rebulk; it never feeds a fast loop.
int BattMonSrc = 0;
int BatteryVoltageSource = 0;  // battery volts for CHARGE DECISIONS only: 0=INA228, 1=NMEA2K Batt (127508), 3=Victron Batt. Control loops and protections always read IBV.
int timeAxisModeChanging = 0;  // toggle the time axis on and off in Plots.  Off = less janky but less info

int maxPoints;                 //number of points plotted per plot (X axis length)
int Ymin1 = -10, Ymax1 = 50;    // Current plot
float Ymin2 = 10.0, Ymax2 = 15.0;  // Voltage plot, 12V window; seeded x(class/12) on first creation
int Ymin3 = 0, Ymax3 = 4000;   // RPM plot
int Ymin4 = 50, Ymax4 = 250;   // Temperature plot


//Advanced temperature control variables:
//=============================================================================
//                        LEARNING MODE GLOBAL VARIABLES
//=============================================================================

float PIDTrackingGain = 4.0f;  // Tracking gain in 1/sec (increase for faster tracking)

enum GovernorMode { GOV_NORMAL_SLEW,
                    GOV_BYPASS_SLEW,
                    GOV_HOLD };
enum SystemMode { SYS_MODE_OFF,
                  SYS_MODE_MANUAL,
                  SYS_MODE_AUTO,
                  SYS_MODE_FAULT };

SystemMode sysMode = SYS_MODE_OFF;
GovernorMode govMode = GOV_NORMAL_SLEW;

float setpointLimited = 0.0f;
float setpointCommand = 0.0f;   // pre-slew current command (Icv in CV, uTargetAmps in idle); global so the Control Accuracy score gate can see whether setpointLimited is still slewing toward it
// Field-duty floor governor_apply enforced last call: max(Min Field %, this speed's Keep-Alive %), or 0 on every floor-bypassing
// path. Here, not in 6_functions.ino, because 3_functions.ino's CSV2 builder reads it (files concatenate in name order).
float g_fieldDutyFloor = 0.0f;
// Board field ceilings from updateFieldDerateCeilings() (6_functions.ino), here for the same reason as g_fieldDutyFloor.
float g_fieldCurDutyCeil = 99.0f;   // 10 A field-current ceiling, duty %
float g_fieldHeatDutyCeil = 99.0f;  // field-drive heat ceiling, duty %
float g_fieldRcOhm = NAN;           // FieldResistance corrected for alternator temperature, less FD_MARGIN: the 10 A cap, the heat law and the Field Amps readout all use it (NAN = no resistance entered)
uint8_t g_fieldCeilBind = 0;        // which ccDutyCeiling() term binds: 0 Max Field %, 1 Max Field Volts, 2 10 A current, 3 heat at the J2 connector, 4 heat at the D5 spreader
uint32_t g_fieldHeatLimS = 0;       // lifetime seconds held at the heat ceiling (NVS FieldHeatLimS, daily cloud field_heat_limited_s)
uint32_t g_fieldCurLimS = 0;        // lifetime seconds held at the 10 A ceiling (NVS FieldCurLimS, daily cloud field_cur_limited_s)
uint32_t g_fieldHeatLimSesS = 0;    // session twin of g_fieldHeatLimS, cleared by ResetFieldHeatLimTime
uint32_t g_fieldCurLimSesS = 0;     // session twin of g_fieldCurLimS, cleared by ResetFieldCurLimTime
volatile bool g_fieldDerateDirty = true;  // set by every writer of FieldResistance / FieldResTrefC / FieldWiring: the next tick re-runs the heat scan
uint8_t ctrlLimiter = 0;        // banner limiter code (→ CSV4/NavStream): 0 none, 1 alt current cap, 2 thermal derate, 3 CV voltage loop, 4 battery current limit, 5 field at max duty (machine/RPM limit), 6 protection (cap binding or post-protection recovery window), 7 battery above target (zero-output stand-down, altZeroOutput), 8 BMS charge-current limit (DVCC CCL), 9 BMS charge-voltage limit (DVCC CVL), 10 Manual mode (open loop — the user's duty IS the output), 11 min field floor holding duty UP (the only code where the limit raises output rather than lowering it), 12 zero-current command (Maintain Mode or zero-current float — the bank is held at 0 A on purpose), 13 warm-up ramp ceiling still climbing (WarmupRampRate), 14 charge-rate High->Low glide holding the old ceiling on the way down, 15 field at the board's 10 A field-current ceiling, 16 field-drive heat ceiling (INA228 die temperature law)
bool setpointInitialized = false;

// ===== LEARNING MODE CONTROL PARAMETERS =====
// Primary Controls
//int loggingEnabled = 0;             // 0=disabled, 1=enabled    was a dead variable
bool learningTableUpdated = false;

// Learning Parameters
int AlternatorNominalAmps = 100;               // Alternator rating for penalty calculation
float LearningUpStep = 1;                      // Table increase amount (A)
float LearningDownStep = 2;                    // Table decrease amount (A)
unsigned long MinLearningInterval = 30000;     // Min time between updates (ms)
unsigned long SafeOperationThreshold = 30000;  // Time for upward learning (ms)

// RPM Transition Filtering (add with other learning variables)
unsigned long lastSignificantRPMChange = 0;  // Timestamp of last major RPM change
int lastStableRPM = 0;                       // RPM before transition
int LearningSettlingPeriod = 30000;          // Milliseconds to wait after RPM change (default 30s)
int LearningRPMChangeThreshold = 500;        // RPM change that triggers settling period
int LearningTempHysteresis = 10;

int wavePeriod = 10;     //For PID tuning
int waveAmplitude = 10;  //For PID tuning
int tuningWaveFloor = 5; // A — floor (trough) the Current Target Generator wave sits on; shared by square + sine. Separate from the Plant tab's SystemIDStabilizeAmps.
int TuningMode = 0;      //
bool tuningSquareAbrupt = false;  // TuningMode square wave bypasses the setpoint slew for abrupt edges (CV plant fit needs a
                                  // defined step instant). Transient: cleared on TuningMode exit, never persisted.

// Battery Health: active DCIR test + passive capacity-%-vs-cycles; chemistry-agnostic (OCV curve + anchor window set per chemistry
// from the web). All NVS writes are field-off-deferred — a DCIR test always ends with the engine on.
struct BattHealthSample {
  uint32_t tMs;
  float    v;   // IBV
  float    i;   // Bcur (+ = charging)
};
struct BattHealthResult {
  uint32_t epoch;        // 0 if clock wasn't synced when the test ran
  float    dcir_mOhm;
  float    soh_pct;      // NaN until a capacity point exists
  float    battTempF;    // measured battery temperature at the run (°F); NAN = no measured source, and then no 25°C normalization is possible
  float    soc_pct;
  float    battV;
  float    stepLowA;
  float    stepDeltaA;
  uint8_t  edgesUsed;
  uint16_t dwellMsUsed;     // dwell-per-level actually used for THIS run (ms) — global setting can change, so stamp it per row for cross-run comparability
  float    fitSpread_mOhm;  // std-dev of the per-edge DCIR values across the scored edges — fit consistency / quality
};
struct BattCapPoint {        // one OCV-anchored capacity measurement (X axis = date)
  uint32_t epoch;            // wall-clock if synced, else soft-clock estimate (0 = unknown)
  float    capacityAh;       // measured full capacity (temp-normalized if enabled)
  float    capPct;           // % vs the chosen reference (rated or first-measured)
  float    socLow;           // OCV-anchored low SoC used for this measurement (%)
  float    tempC;            // measured battery temperature at the full anchor (°C); NAN = none, which disables temp normalization
  uint8_t  conf;             // 0 = low confidence, 1 = high
};

float    bhStepLowA   = 30.0f;
float    bhStepDeltaA = 30.0f;  // crest = low + delta
uint32_t bhDwellMs    = 8000;   // must exceed bhMinDwellMs() — the 30A step slews at TEST_ENTRY_RATE_A (8 A/s), so the level needs ~3.75s just to arrive, plus a settled tail for the fit window
uint8_t  bhNumEdges   = 4;  // scored steps; 3 is the floor for the spread/fit check, 4 gives one extra edge to average for little added time

volatile uint8_t bhTestState = 0;          // 0 IDLE, 1 RUNNING, 2 DONE, 3 ABORTED
volatile bool batteryHealthTestActive = false;  // read by the control loop's health branch; volatile keeps start/abort store order vs bhTestState
const char *bhAbortReason = "";
BattHealthSample *bhSamples = nullptr;
int      bhSampleCap = 0;
int      bhSampleCount = 0;
bool     bhWaveHigh = false;
uint32_t bhLastToggleMs = 0;
int      bhEdgeCount = 0;
#define  BH_MAX_TOGGLES 64
uint32_t bhToggleMs[BH_MAX_TOGGLES];        // actual toggle instants (robust to skipped ticks)
int      bhToggleCount = 0;
uint32_t bhTestStartMs = 0;
float    bhLastResultDcir = 0.0f;

BattHealthResult *bhResults = nullptr;
int      bhResultCap = 64;
int      bhResultCount = 0;
int      bhResultHead = 0;

BattCapPoint *bhCapRing = nullptr;
int      bhCapCap = 512;
int      bhCapCount = 0;
int      bhCapHead = 0;
bool     bhCapDirty = false;
bool     bhResultsDirty = false;
float    bhBaselineCapacityAh = 0.0f;       // first measured capacity (used when capRefMode==1); 0 = unset
// Both rings persist as LittleFS blobs: nvs_set_str's 4000-byte cap failed silently near ~120 capacity points.
// bhInitSettings migrates a legacy NVS string once, then removes the key.
#define BHCAP_PATH  "/bhcap.bin"
#define BHCAP_MAGIC 0x42484350u  // 'BHCP'
#define BHCAP_VER   1u
#define BHRES_PATH  "/bhres.bin"
#define BHRES_MAGIC 0x42485253u  // 'BHRS'
#define BHRES_VER   2u           // v2 = battTempF is a measured BATTERY temperature; v1 stamped the board temperature there
#define BHRES_VER_BOARDTEMP 1u   // legacy rows: bhInitSettings reads them, blanks that field to NAN, rewrites as v2

// ── Capacity tracker (OCV-anchored) ──
// Low SoC from a RESTED OCV via capOcvVolt[], independent of coulomb counting (what makes fade measurable); full anchor =
// FullChargeDetected. Capacity = Ah bridged between them ÷ SoC span.
#define  CAP_OCV_ROWS 11
const uint8_t capOcvSocPct[CAP_OCV_ROWS] = {100, 90, 80, 60, 40, 30, 20, 15, 10, 5, 0};   // fixed breakpoints
float    capOcvVolt[CAP_OCV_ROWS] = {13.6f, 13.4f, 13.3f, 13.2f, 13.1f, 13.0f, 12.9f, 12.8f, 12.5f, 12.0f, 10.0f};  // RESTED 12V LiFePO4 default; user-editable, scaled by SYSTEM_VOLTAGE_CLASS/12 at lookup
// Chemistry rested-voltage presets (12V-referenced), mirror of web CAP_OCV_PRESETS; loaded by chemistry at commissioning.
// A hand-tuned curve (matching no preset) is left untouched.
const float capOcvPresetLifepo4[CAP_OCV_ROWS] = {13.60f, 13.40f, 13.30f, 13.20f, 13.10f, 13.00f, 12.90f, 12.80f, 12.50f, 12.00f, 10.00f};
const float capOcvPresetAgm[CAP_OCV_ROWS]     = {12.85f, 12.70f, 12.55f, 12.35f, 12.15f, 12.05f, 11.90f, 11.80f, 11.70f, 11.60f, 11.50f};
const float capOcvPresetLead[CAP_OCV_ROWS]    = {12.70f, 12.50f, 12.40f, 12.20f, 12.00f, 11.90f, 11.80f, 11.73f, 11.65f, 11.55f, 11.40f};
// Exposed config
float    capRestCurrentFrac = 0.01f;  // |I_batt| < frac×RATED ("resting"); default C/100
uint16_t capRestFloorMin    = 30;     // minimum rest before OCV is trusted (min)
float    capSettleRateMv10  = 2.0f;   // dV/dt settle threshold (mV per 10 min) — voltage must be this flat
float    capSocLowMax       = 20.0f;  // low anchor must be ≤ this. LiFePO4 default 20 (only its flat curve's bottom knee reads SoC reliably); lead-acid/AGM set ~50 from the web since their OCV maps across the whole range
float    capMinSpan         = 70.0f;  // (capFullSoc − socLow) must exceed this — "deep enough"
float    capFullSoc         = 100.0f; // SoC assigned to the full-charge anchor
uint8_t  capRefMode         = 0;      // 0 = % vs rated (BatteryCapacity_Ah), 1 = % vs first measurement
uint8_t  capTempNormEnable  = 0;      // 0 = record temp only, 1 = normalize capacity to capTempRefC
float    capTempCoeffPctC   = 0.5f;   // capacity temp coefficient (%/°C), only if capTempNormEnable
float    capTempRefC        = 25.0f;
// Volatile anchor/bridge state
uint32_t capRestTimerMs     = 0;
bool     capLowAnchorValid  = false;
float    capLowAnchorSoC    = 0.0f;
float    capLowAnchorTempC  = 0.0f;
float    capBridgeAh        = 0.0f;   // independent, UNCLAMPED Ah integral since the low anchor
float    capBridgeMinAh     = 0.0f;   // most-negative excursion since anchor (re-discharge guard)
float    capPrevVForRate    = NAN;    // dV/dt tracking
uint32_t capPrevVRateMs     = 0;
float    capLastDvdtMv10    = 999.0f; // last settle rate (confidence + display)
uint32_t capLastUpdateEpoch = 0;
float    capLastPct         = NAN;    // most recent capacity % (display)

// ── CV plant-fit — ohmic-anchor pulse train (spec: CV_Gain_Ohmic_Anchor_Redesign_Spec.md) ──
// Abrupt duty pulses; fit = per-edge settled ΔV/ΔI at ~0.6 s (the timescale the CV loop reacts on),
// median of 8 edges → cvPlantKa.
struct CvPlantFitSample { uint32_t tMs; float v; float iAlt; float iBat; };  // 16 B; v=IBV iAlt=MeasuredAmps iBat=Bcur
CvPlantFitSample *cvpfBuf = nullptr;
int      cvpfBufCap   = 0;
int      cvpfBufCount = 0;
volatile bool cvPlantFitActive = false;   // master test flag (mutexes, D-term hold-off, idle-rest suppression)
volatile bool cvpfCcActive = false;       // SETTLE/PILOT/HOLD drive the real current PID (branch in AdjustField); pulses own duty
volatile uint8_t cvpfState = 0;           // 0 IDLE, 1 RUNNING, 2 DONE-OK, 3 ABORTED
volatile uint8_t cvpfPhase = 0;           // 0 SETTLE, 1 PILOT/SIZE, 2 HOLD (practice run), 3 REBASE (back to base, captures the floor duty), 4 PULSES, 5 RELEASE (UI progress)
volatile bool cvpfReady = false;          // a run finished (ok or not) — poller shows result
volatile bool cvpfOk    = false;          // fit produced a usable gain
float    cvpfK        = 0.0f;             // V/A — measured ~0.6 s stiffness (= Ka; flat curve)
float    cvpfKa       = 0.0f;             // V/A — median settled ΔV/ΔI over the pulse edges (the stiffness anchor)
float    cvpfKb       = 0.0f;             // V/A/√s — retired √t tail, always 0 (kept for JSON/Apply compat)
float    cvpfDV       = 0.0f;             // V — median per-edge ΔV
float    cvpfDI       = 0.0f;             // A — median per-edge settled current step (alt or battery)
float    cvpfSNR      = 0.0f;
float    cvpfKp       = 0.0f, cvpfKi = 0.0f;   // display gains (recomputeCvGains is authoritative on Apply)
float    cvpfHorizonS = 0.6f;             // s — the fixed ~0.6 s edge readout window; cvpfStartTest recomputes from CVPF_EDGE_V0/V1_MS (display only)
uint8_t  cvpfWarn     = 0;                 // bit0 delivery bit1 SNR bit2 edges-dropped bit3 gap-drift bit4 OV-fallback bit5 engine-speed drift
const char *cvpfAbortMsg = "";
// phase-machine internals (init in cvpfStartTest, advanced in cvpf_tick)
uint32_t cvpfPhaseStartMs = 0, cvpfSampleLastMs = 0, cvpfTestStartMs = 0;
float    cvpfBaseA = 10.0f, cvpfPilotA = 6.0f, cvpfStepA = 6.0f, cvpfTargetDV = 0.30f;
float    cvpfPilotK = 0.02f, cvpfPilotVbase = 12.0f, cvpfCmdA = 10.0f;
bool     cvpfFellBack = false;
bool     cvpfBoosted = false;   // operator accepted the weak-signal stronger step: cvpfDiMaxA is that step, target-ΔV sizing skipped
float    cvpfBaseDuty = 0.0f, cvpfStepDuty = 0.0f;  // settled duties captured from the current PID, replayed abruptly
float    cvpfPreSetpoint = 0.0f;   // setpointLimited before the test — restored pre-override so the resume snapshot is clean
uint32_t cvpfEdgeMs[8] = { 0 };    // tick timestamp of each abrupt duty edge (even idx = step-up, odd = step-down; pulse = idx>>1)
uint8_t  cvpfEdgeCount = 0;
float    cvpfEdgeK[8]   = { 0 };   // per-edge stiffness mV/A for the wizard table (0 = no reading)
uint8_t  cvpfEdgeStat[8] = { 4, 4, 4, 4, 4, 4, 4, 4 };  // 0 used · 1 weak step · 2 wrong-way · 3 window starved · 4 not fired
float    cvpfDiMaxA = 40.0f;       // per-run current-step ceiling (A); set to the requested step when the operator accepts the weak-signal re-run, reset each start
float    cvpfRpmAtFit = 0.0f;      // mean RPM over the two middle pulse segments — re-run RPM advice + result-card conditions
float    cvpfRpmMinAtFit = 0.0f, cvpfRpmMaxAtFit = 0.0f;  // RPM extremes over the same windows — result-card context only
float    cvpfBattVAtFit = 0.0f;    // mean IBV over the two middle pulse segments — result-card "test conditions" only
float    cvpfSocAtFit = -1.0f;     // mean SOC% over the two middle pulse segments (−1 when socInfoAvailable false) — result-card only
double   cvpfRpmSum = 0.0, cvpfBattVSum = 0.0, cvpfSocSum = 0.0;   // accumulators, summed over the settled tail of the mid high+low segments
uint32_t cvpfCondN = 0;            // condition samples accumulated (settled tails of segments 3 & 4 → ~1 s of stable level each)
float    cvpfCapHeadroomA = 0.0f;  // A — largest step the cap-table clamp in cvpfSizeStep allows at fit time (g_I_cap − cvpfBaseA − CVPF_CAP_MARGIN_A)
float    cvpfRebaseRpm = 0.0f;     // RPM at REBASE exit — same base current as cvpfSettleRpm, so the pair reads drift with load sag cancelled
float    cvpfDriftSetupPct = 0.0f; // signed % engine drift across setup (SETTLE exit -> REBASE exit, both at base current)
float    cvpfDriftTrainPct = 0.0f; // signed % engine drift across the pulse train (first vs last base segment, both at base duty)

// ── CV stress test (commissioning stage 8 / standalone from Tuning ▸ Stress Test) ──
// Throttle-snap acceptance test: regulator runs NORMALLY at idle (nothing overridden), the operator snaps the throttle through
// the band where output multiplies, and recovery is graded purely from the protection counters — every protection stays armed.
volatile bool cvStressActive = false;       // master flag (mutexes, idle-rest suppression)
volatile bool cvStressAbortRequested = false;
volatile bool cvStressForceCV = false;      // phases 2-3: AdjustField forces CV at cvStressTargetV (TargetVoltageMode-style)
volatile uint8_t cvStressPhase = 0;         // 0 idle 1 STAB_IDLE 2 STAB_CV 3 ARMED 4 DONE
float cvStressTargetV = 0.0f;               // achievable CV target the test computes (idle avg − CvStressDropV), driven while forced
float CvStressDropV = 0.10f;                // V (12V-equiv, ×V/12 at use) — target headroom: CV target parks this far below the settled idle voltage; smaller = more standing current at the parked target = harsher snap (the settle-up gate keeps the level a true equilibrium so CV, not the current limit, stays in command)
float CvStressFailBandV = 0.25f;            // V (12V-equiv, ×V/12 at use) — swing beyond this at a stabilization deadline = genuine instability, test cannot run; at or under it the phase proceeds with the swing reported as a marginal grade
const char *cvStressAbortMsg = "";
char (&cvsLastBlob)[224] = PsramPlace::make<char[224]>();                 // last persisted result CSV (NK_cvStressLast, ver-2), cached for /cvstress.json

// Explicit prototypes: String-return / String& functions don't survive auto-prototype ordering.
void   bhSample(uint32_t nowMs);
bool   bhStartTest();
void   bhAbort(const char *reason);
void   bhComputeDcir();
void   bhServiceCompletion();
void   cvpfSample(uint32_t nowMs);
bool   cvpfStartTest(float diMaxReq);
void   cvpfAbort(const char *reason);
void   cvpfProcess();
void   cvpfServiceCompletion();
bool   cvpf_tick(float &dutyOut, float measA, uint32_t nowMs);
bool   cvStressStartTest();
void   cvStressAbort(const char *reason);
void   cvStress_tick(uint32_t nowMs);
void   cvStressTrackEveryPass();
int    cvStressJsonBuild(char *buf, int cap);
void   bhFlushCapNVS();
void   bhInitSettings();
void   bhDeserializeResults(const String &blob);
void   bhDeserializeCap(const String &blob);
char  *bhBuildStatusJson(size_t *len);
// Capacity tracker (OCV-anchored)
float  ocvToSoC(float restedV);
void   capTrackTick(float I_batt, float V_filt, float tempC, float dtSec);
void   capTrackOnFull(uint32_t epoch, float tempC);
String capSerializeOcv();
void   capDeserializeOcv(const String &blob);
bool   ocvIsAnyPreset();
void   applyChemistryOcvPreset();

// ── Tuning→Current closed-loop sine generator (Stage 2) ───────────────────────
// Waveform: 0 = square (ISE tuning), 1 = sine manual, 2 = sine auto-sweep. Sine modes drive a sine reference and measure
// measured-vs-reference gain/phase = the closed-loop frequency response.
int     tuningWaveform     = 0;        // 0 square, 1 sine manual, 2 sine auto-sweep
float   tuningSineFreq     = 2.0f;     // Hz — manual sine frequency
float   tuningSweepStart   = 0.2f;     // Hz — auto-sweep low end
float   tuningSweepEnd      = 15.0f;   // Hz — auto-sweep high end
uint8_t tuningSweepCycles   = 2;       // analysed cycles per frequency (1 settle cycle skipped); integer-cycle windowed, so 2 is clean
volatile bool tuningSweepRequested = false;  // UI "Run Sweep" → start/restart the sweep
bool    tuningSweepActive   = false;   // sweep in progress
bool    tuningSweepDone     = false;   // sweep finished, tuningBode[] valid
uint32_t tuningRunTok       = 0;       // run token, see fieldCurveRunTok
#define TUNING_SWEEP_NPOINTS 10        // log-spaced sweep points
struct TuningBodePoint { float freqHz; float gain; float phaseDeg; };  // gain = measured/reference (unitless)
TuningBodePoint (&tuningBode)[TUNING_SWEEP_NPOINTS] = PsramPlace::make<TuningBodePoint[TUNING_SWEEP_NPOINTS]>();
uint8_t tuningBodeCount = 0;           // valid points (served by /tuningbode)

// Sine-sweep run-condition trackers, reset at sweep start and snapshotted at commit: a sweep at wandering RPM or a railed
// field isn't comparable.
float tuningSweepBaseA      = 0.0f;    // operating-point center of the sine (A)
float tuningSweepAmpA       = 0.0f;    // perturbation amplitude of the sine (A)
float tuningSweepRpmMin     = 0.0f;
float tuningSweepRpmMax     = 0.0f;
double   tuningSweepRpmSum  = 0.0;     // with tuningSweepRpmN → true run-average RPM (record avgRPM + /tuningbode)
uint32_t tuningSweepRpmN    = 0;
float tuningSweepBattV      = 0.0f;    // bus voltage snapshot during the sweep
bool  tuningSweepDutyRailed = false;   // field duty hit 0%/100% on any sine peak
float tuningSweepWorstCoh   = 1.0f;    // min IN-BAND fit coherence (0..1, 1=clean); rolled-off points excluded so slow alternators don't false-fail
float tuningSweepFsHz       = 0.0f;    // control-tick rate measured DURING the last sweep (samples/accumulate-window); 0 = not measured
// Protection-abort latch (applyImmediateCut, first cause wins, cleared at sweep start), served by /tuningbode so the dashboard
// names the cut instead of blaming field saturation / cruising speed.
volatile uint8_t tuningSweepAbortReason = 0;  // 0 = none; else FieldEventReason code
float tuningSweepAbortVolts = 0.0f;           // bus volts at the cut
float tuningSweepAbortDuty  = 0.0f;           // commanded field % at the cut

// Current closed-loop sine-sweep history (50-record ring, /tuningsweeplog.bin).
struct TuningSweepRecord {
  uint16_t runNumber;
  float    bandwidthHz;     // -3dB closed-loop bandwidth; -1 if it never tracks
  float    peakGain;        // resonance indicator
  float    peakGainFreqHz;
  float    worstPhaseDeg;
  float    worstPhaseFreqHz;
  float    kp, ki, kd;
  float    sweepStartHz;
  float    sweepEndHz;
  uint8_t  cycles;
  uint8_t  nPoints;
  float    avgRPM;
  float    avgAltTempF;
  float    sineAmpA;        // perturbation amplitude (A) — results only comparable at equal amplitude
  float    baseA;           // operating-point center (A) — the field current the sine swung around
  float    battV;           // bus voltage during the sweep
  float    rpmMin, rpmMax;  // RPM range across the sweep (wide spread = smeared Bode, distrust)
  float    worstCoherence;  // min in-band fit coherence (0..1; low = real in-band noise, e.g. RPM wander)
  uint8_t  dutyRailed;      // 1 if field duty hit 0%/100% during the sweep (clipped sine)
  uint8_t  chargeStage;     // getChargeStageDisplayCode() at commit
  uint32_t epoch;           // wall-clock Unix seconds at commit (0 = clock not synced)
  TuningBodePoint curve[TUNING_SWEEP_NPOINTS];
};
TuningSweepRecord *tuningSweepLog = nullptr;
uint8_t  tuningSweepLogCount = 0;
uint8_t  tuningSweepLogHead  = 0;
uint16_t tuningSweepRunCounter = 0;

// === PID Tuning Score System ===
struct TuningRecord {
  uint16_t runNumber;
  float score;
  float activeTimeSec;
  float kp, ki, kd;
  uint8_t sampleDivisor;
  float trackingGain;
  float dutyRampRate;
  int16_t waveAmplitude;
  int16_t wavePeriod;
  int16_t waveFloor;     // operating-point trough the wave sat on (A)
  float avgRPM;
  float avgAltTempF;
  float worstErrorA;
  float battV;           // bus voltage during the test
  uint8_t chargeStage;   // getChargeStageDisplayCode() at commit
  uint32_t epoch;        // wall-clock Unix seconds at commit (0 = clock not synced)
  char note[51];         // user free-text label captured at commit (from ccTuningNote)
};

struct TuningScoreState {
  uint8_t toggleCount;        // half-period toggles seen since test start
  uint8_t scoredToggleCount;  // toggles that occurred in scoring phase (post ring-in)
  bool ringInDone;            // true after 4 toggles (2 full cycles discarded)
  bool inScoringWindow;       // true while scoring is active (after slew settles, times out 5s after opening)
  bool pendingWindowOpen;     // toggle seen post ring-in; waiting for slew to settle before opening window
  float errorAccum;           // ISE accumulator (e² × dt)
  float activeTimeSec;        // total time spent inside scoring windows
  uint32_t lastToggleMs;      // millis() when scoring window last opened (for 5s timeout timer)
  float rpmSum;               // for computing avg RPM over test
  float tempSum;              // for computing avg alt temp over test
  uint32_t tempSampleCount;   // NON-NAN temp samples only; dividing by avgSampleCount fabricated 0.0F averages with no probe
  uint16_t avgSampleCount;
  float worstErrorA;  // largest single |error| seen during test
  float score;        // current normalized score = errorAccum / activeTimeSec
};


char ccTuningNote[51] = {0};  // live CC test note; captured into the record at commit (RAM only, not persisted)
char cvTuningNote[51] = {0};  // live CV test note; captured into the record at commit (RAM only, not persisted)

TuningRecord *tuningLog = nullptr;  // ps_malloc(50 × sizeof(TuningRecord))
uint8_t tuningLogCount = 0;         // records currently in ring buffer (0–50)
uint8_t tuningLogHead = 0;          // next write index
uint16_t tuningRunCounter = 0;      // increments each commit, persists via loadTuningLog
TuningScoreState tuningScore = {};  // active test accumulator
bool tuningParamChanged = false;             // set by server handlers when a tuning param is updated
volatile bool manualCommitTuningRequested = false;   // set by UI commit button
volatile bool manualCommitCVTuningRequested = false; // set by UI commit button

// ===== Control Accuracy v4 — routine-data loop health (CONTROL_ACCURACY_V4_ROUTINE_SPEC.md). No episodes, no reference model.
// Each valid tick lands in ONE bucket: CONSTRAINED (rail / protection clamp, reported separately, never graded) > ACTIVE (command
// moving or recent challenge, the graded denominator) > QUIET. Band exits run an excursion stopwatch; damaging-side exposure and
// worst peak are recorded unconditionally. Reset only manually (also discards live stopwatches). DOUBLE: a float sum stalls after ~a day.
struct AccLoopV4 {
  double   validSec;        // loop in authority
  double   activeSec;       // challenged time — tracking denominator
  double   inbandActiveSec; // in band while active — tracking numerator
  double   constrainedSec;  // output railed / protection clamp owned the command
  double   recovSecSum;     // Σ unpaused out-of-band seconds across excursions
  double   overExpSum;      // Σ max(0, damaging e − band)·dt (unit·seconds)
  float    worstOver;       // worst damaging-side error (physical units)
  uint16_t excursions;      // confirmed band exits since reset
};
AccLoopV4 accCur4 = {};   // inner current loop — amps; band = max(3 A, 5% of rated)
AccLoopV4 accVolt4 = {};  // CV voltage loop — 12V-equiv volts (displayed mV)

// Live excursion stopwatch: count taken at confirmed band exit; out-of-band time PAUSES while the loop is constrained in the
// direction the error needs (rail/clamp physics is not tracking failure).
struct AccExcursion {
  uint8_t  state;          // 0 = idle, 1 = out of band
  int8_t   side;           // +1 = damaging (over), -1 = under
  uint8_t  enterTicks;     // consecutive ticks beyond the band (open debounce)
  uint32_t reentryStartMs; // back inside band since (0 = currently outside)
  double   outSec;         // unpaused out-of-band seconds this excursion
};
AccExcursion excCur = {};
AccExcursion excVolt = {};

uint32_t accThermSessions = 0;       // thermal containment sessions (binding-settled rising edges)
double   accThermBindingSec = 0.0;   // binding-and-settled seconds — containment denominator
double   accThermInbandSec = 0.0;    // of those, |tempFiltered − tempPIDSetpoint_d| ≤ ACC_T_BAND_F
float    accThermWorstOverF = 0.0f;  // worst °F over TemperatureLimitF — unconditional (coast-down peaks)
uint32_t accThermBindingStartMs = 0; // millis() binding went true (0 = not binding) — settle timer
bool     accThermSettledPrev = false; // previous-tick settled state — session rising edge; reset paths read it

uint32_t accScorerLastMs = 0;        // last electrical-scorer tick — raw-gap guard
uint32_t accCurActiveUntilMs = 0;    // ACTIVE bucket holds until here (0 = never armed)
uint32_t accVoltActiveUntilMs = 0;
float    accCurPrevCmd = 0.0f;       // previous-tick setpointCommand — command-motion ACTIVE trigger
float    accVPrevTargetV = 0.0f;     // previous-tick ChargingVoltageTarget — step ACTIVE trigger
bool     accClampPrev = false;       // previous-tick g_fastOvClampActive — release edge arms ACTIVE
bool     accVAwRecovPrev = false;    // previous-tick g_cvAwRecovering — release edge arms ACTIVE

const float    ACC_CUR_BAND_FLOOR_A = 3.0f;     // current band = max(floor, frac × rated): rated-normalized so installs compare
const float    ACC_CUR_BAND_FRAC = 0.05f;
const float    ACC_V_BAND_V = 0.100f;           // voltage band, 12V-equivalent volts
const float    ACC_T_BAND_F = 3.0f;             // thermal containment band vs tempPIDSetpoint_d
const float    ACC_CUR_ACTIVE_RATE_A_S = 2.0f;  // command motion above this arms ACTIVE
const float    ACC_V_STEP_V = 0.050f;           // per-tick target delta that arms ACTIVE (temp-comp drift stays far below)
const uint32_t ACC_ACTIVE_HOLD_MS = 10000;      // ACTIVE lingers this long past the last trigger
const uint8_t  ACC_ENTER_DEBOUNCE_TICKS = 2;
const uint32_t ACC_CUR_EXIT_HOLD_MS = 500;      // sustained re-entry that ends an excursion
const uint32_t ACC_V_EXIT_HOLD_MS = 1000;
const uint32_t ACC_GAP_VOID_MS = 250;           // raw scorer-tick gap: skip the tick, discard live stopwatches
const uint32_t ACC_SETTLE_THERMAL_MS = 120000;  // thermal: require sustained binding
const uint32_t ACC_THERM_CV_YIELD_MS = 10000;   // CV must own the command this long straight to end a containment session; below it the plant (25s slope window) cannot move enough to corrupt the score

// === CV Loop Tuning Score System ===
float cvWaveAmplitudeV = 0.30f;   // V — target rises by this during HIGH phase (LOW phase sits at the real target)
int cvWavePeriodSec = 30;         // s — full period of the Waveform Generator wave (one LOW + one HIGH). UI min 8 s; below ~14 s a HIGH half can't fit settle + CV_P2P_SKIP_MS + CV_P2P_EVAL_MS, so p2p reads n/a (scoring still works).
float cvKOvershoot = 10.0f;       // penalty weight on integrated overshoot (user-exposed)
uint8_t cvConsecutiveReads = 10;  // consecutive filtered reads within the class-scaled settle band (CV_SETTLE_V_THRESH) to declare settled (~1s at 100ms rate)
int CVTuningMode = 0;             // 0=off, 1=on
bool cvWaveExitWindDown = false;  // one-shot: set when the waveform test is turned off so the elevated target glides back to base via the down-slew instead of snapping (a snap would drop the target below the still-high actual voltage and trip fast-OV/iExcess)
float cvBaseTarget = 0.0f;        // real ChargingVoltageTarget captured at test start; global so wave gen + scorer share it

const float CV_SETTLE_V_THRESH    = 0.10f;   // V at 12V — settling threshold; ×(SYSTEM_VOLTAGE_CLASS/12) at use
const float CV_HIGH_DEADBAND_V   = 0.025f;  // V at 12V — HIGH phase overshoot dead-band, ×(SYSTEM_VOLTAGE_CLASS/12) at use; below this is free
const float CV_LOW_GRACE_SEC     = 1.0f;    // s — grace period from LOW phase start before undershoot scoring begins
const float CV_LOW_RAMP_SEC      = 10.0f;   // s — undershoot weight ramps 0→1 over this window after grace
const float CV_UNDERSHOOT_SCALE  = 0.15f;   // undershoot ISE weight relative to overshoot ISE
const uint32_t CV_P2P_SKIP_MS = 1000UL;     // ms to wait after V first reaches target before the steady-state p2p window opens
const uint32_t CV_P2P_EVAL_MS = 3000UL;     // length of the steady-state p2p evaluation window

struct CVTuningRecord {
  uint16_t runNumber;
  // Results
  float score;
  float avgSettlingTimeSec;
  float worstOvershootV;
  float avgIntegratedOvershootVs;
  float activeTimeSec;
  uint16_t fastOvFires, iExcessFires, loadDumpFires, hardOcFires;
  // CV PI (no D term)
  float voltageKp, voltageKi;
  // Setpoint shaping
  float setpointRiseRate, setpointFallRate;
  // Integrator management
  float awBleedRate, awRecoverRate;
  uint16_t awSeedProtectMs;
  float reseedFrac;
  float voltageKd;  // A/(V/s) — CV D-term gain (column "KD")
  // FastOV supervisor
  float kHard;
  // iExcess
  float iExcessFrac;
  float iExcessTau;
  float iExcessKBleed;
  // Load dump
  float loadDumpDtThresh, loadDumpDtThresh1, loadDumpDtThresh3;
  // Test setup
  float waveAmplitudeV;
  uint16_t wavePeriodSec;
  float kOvershoot;
  uint8_t consecutiveReads;
  // Operating conditions
  float avgRPM, avgAltTempF;
  float battVAtStart, socAtStart;
  float chargingVoltageTarget;
  uint8_t chargeStage;   // getChargeStageDisplayCode() at commit
  // Low phase results (step-down response)
  float lowScore;
  float avgLowSettlingTimeSec;
  float avgLowIntOvVs;  // avg integrated overvoltage above lowTarget (V·s)
  float worstLowOvV;          // peak above lowTarget during any scored LOW phase (after zero crossing)
  float worstLowUndershootV;  // peak below lowTarget during any scored LOW phase
  float steadyP2PV;           // mean steady-state peak-to-peak of measured V across HIGH steps (-1 = n/a: no step long enough)
  uint32_t epoch;             // wall-clock Unix seconds at commit (0 = clock not synced)
  char note[51];              // user free-text label captured at commit (from cvTuningNote)
};

struct CVTuningScoreState {
  bool waveHigh;
  uint32_t lastToggleMs;
  uint8_t halfPeriodCount;
  bool ringInDone;
  // Per-HIGH-phase state (reset each toggle-to-high)
  uint32_t phaseStartMs;
  bool phaseSettled;
  uint8_t consecutiveInBand;
  // Steady-state peak-to-peak (per HIGH phase): stamp when V first reaches the settle band,
  // skip CV_P2P_SKIP_MS, then min/max over CV_P2P_EVAL_MS. One p2p sample per phase → mean at commit.
  uint32_t reachedTargetMs;   // 0 = V has not reached the band yet this HIGH phase
  bool p2pCaptured;           // eval window has closed for this HIGH phase
  float p2pMin, p2pMax;       // extremes of IBV during the eval window
  float totalSteadyP2PV;      // sum of per-phase (max-min) across qualifying HIGH phases
  uint8_t steadyP2PCount;     // HIGH phases that fit a full eval window
  // Accumulators across scored HIGH phases
  uint8_t scoredHighCount;
  float totalSettlingTimeSec;
  float worstOvershootV;
  float totalIntegratedOvershootVs;
  float activeTimeSec;
  // Protection deltas (snapshotted at start of each HIGH phase)
  uint32_t fastOvSnap, iExcessSnap, loadDumpSnap, hardOcSnap;
  uint16_t fastOvFires, iExcessFires, loadDumpFires, hardOcFires;
  // Per-LOW-phase state (reset each toggle-to-low after ring-in)
  uint32_t lowPhaseStartMs;
  bool lowPhaseSettled;
  uint8_t lowConsecInBand;
  // Accumulators across scored LOW phases
  uint8_t scoredLowCount;
  float totalLowSettlingTimeSec;
  float totalLowIntOvVs;       // ISE of re-overshoot above lowTarget (only after zero crossing) × dt
  float worstLowOvV;           // peak above lowTarget during any scored LOW phase (after zero crossing)
  float totalLowUndershootVs;  // weighted ISE of voltage below lowTarget (time-ramped, ×CV_UNDERSHOOT_SCALE)
  float worstLowUndershootV;   // peak below lowTarget (V, absolute) during any scored LOW phase
  bool lowCrossedBelow;        // true once IBV has crossed below lowTarget in current LOW phase
  // Protection snaps for LOW phases
  uint32_t lowFastOvSnap, lowIExSnap, lowLdSnap, lowHocSnap;
  // Operating conditions
  float battVAtStart, socAtStart;
  bool testStarted;
  // Averages
  float rpmSum, tempSum;
  uint32_t tempSampleCount;   // see TuningScore.tempSampleCount
  uint16_t avgSampleCount;
};

CVTuningRecord *cvTuningLog = nullptr;  // ps_malloc(50 × sizeof(CVTuningRecord))
uint8_t cvTuningLogCount = 0;
uint8_t cvTuningLogHead = 0;
uint16_t cvTuningRunCounter = 0;
CVTuningScoreState cvTuningScore = {};
bool cvTuningParamChanged = false;

float xTime = 30.0;     // seconds    PID Chart
int yyMax = 105;        // PID Chart     Amps
int yyMin = -25;        //  PID Chart Amps
float pidError = 0.0f;  // PID error for display (A)


// Accelerometer is always on — no enable toggle exists.

// ===== CV LOOP PARAMETERS — all tunable values consolidated here =====
// fastOV trip levels are derived inside AdjustFieldLearnMode() (6_functions.ino): G2 measured at
// target+OvMeasMarginV, G1 predictive at target+OvPredMarginV over horizon TdPred. Only PRED_GUARD
// (0.06 V arm window, per-cell-scaled by class) stays a local const there.

// --- Output current PID ---
// PidKp/Ki/Kd are referenced to a 12 V winding on a 12 V bus; recomputeCcGains() multiplies by fieldScale() to get PidK*_active
// (rated field per duty-% scales with bus over winding volts). Do NOT scale these by hand. The plant fit stores gain / fieldScale(),
// and a winding change on a commissioned unit rescales by old/new winding. Seeds = a commissioning plant fit (tau 112 ms, IMC)
// de-rated 30% (a different plant oscillates at full gain), Ki/Kp zero (8.9 rad/s) preserved. Commissioning overwrites both.
float PidKp = 0.4662f;  // 12V-equivalent proportional gain
float PidKi = 4.5384f;  // 12V-equivalent integral gain
float PidKd = 0.0f;  // 12V-equivalent derivative gain (default off; current-loop D adds noise, little benefit)
volatile float PidKp_active = 0.579f;  // DERIVED duty-space gain the PID uses (PidKp × fieldScale()). Set by recomputeCcGains().
volatile float PidKi_active = 5.150f;  // DERIVED duty-space Ki.
volatile float PidKd_active = 0.0f;  // DERIVED duty-space Kd.
// --- Voltage (CV) PID ---
// ── CV Gain Source system (Measured α/K vs Manual) — see Working Markdown Docs/CV_AUTOTUNE_PLAN.md ──
// VoltageKp/Ki are the MANUAL 12V-equivalent gains; the loop reads VoltageK*_active, which recomputeCvGains() derives from the
// selected mode × 12/SYSTEM_VOLTAGE_CLASS.
uint8_t cvGainMode   = 0;         // 0 = Manual (typed VoltageKp/Ki) — DEFAULT, so an uncommissioned device runs the seeded gains;
                                  // 1 = Measured (ohmic-anchored, Kp = α/K). cxCVPlantApply flips it to 1.
// CSV3_EXTRA1 is cvLambdaMult's reserved slot, keeping the CSV3 field count unchanged.
// cvPlantKa = measured ~0.6 s stiffness (V/A at the pack, ohmic + early soak), the Measured anchor (Kp = α/Ka).
// Pre-commission default deliberately HIGH: bigger Ka → smaller Kp → gentle until a real measurement lands.
float   cvPlantKa    = 0.0216f;
float   cvPlantKb    = 0.0f;      // retired √t tail — always 0; NVS key kept so old exports/POSTs still parse
float   cvPlantK     = 0.0216f;   // DERIVED, not persisted: = cvPlantKa. Telemetry + dashboard only.
// Measured ripple projection (RIPPLE_DETECTION_REARCH_SPEC §3.3): ripple(I) = a0 + a1·I fit to the IExcessTau-filtered excess pk-pk
// of the commissioning 3-level test. REFERENCE ONLY (Protections plot) — it NEVER moves the over-current floor.
struct RipFit {
  float a0, a1;      // ripple(I) = a0 + a1·I (A pk-pk of the IExcessTau-filtered excess)
  float rpm;         // engine RPM the 3-level test ran at (0 = no test yet)
  float iPt[3];      // the 3 commanded operating currents (A)
  float pkPt[3];     // measured filtered-excess pk-pk at each level (A)
  uint8_t nPts;      // valid measured points (0 = no fit → plot shows threshold line only)
};
RipFit ripFitAlt  = {0};   // alternator (G3) detector — ADS1115 MeasuredAmps path
// Same struct for the CV D-term deadband fit: a0/a1 = b0/b1 of slope(I)=b0+b1·I (V/s of g_cvKdFiltV), pkPt = the 3 slope points,
// from the same Step-5 sweep windows. Persisted under NK_slpFitAlt.
RipFit slpFitAlt  = {0};

// Persisted as one CSV string under NK_ripFitAlt (settings namespace, string).
static String ripFitEncode(const RipFit &r) {
  String s = String(r.a0, 4) + "," + String(r.a1, 5) + "," + String(r.rpm, 0);
  for (int i = 0; i < 3; i++) s += "," + String(r.iPt[i], 2);
  for (int i = 0; i < 3; i++) s += "," + String(r.pkPt[i], 3);
  s += "," + String((int)r.nPts);
  return s;
}
static void ripFitDecode(const String &s, RipFit &r) {
  float f[9] = {0}; int n = 0, from = 0;   // a string with fewer than 9 fields leaves the tail unwritten; it is copied out unconditionally below
  memset(&r, 0, sizeof(r));
  for (int field = 0; field < 10 && from <= s.length(); field++) {
    int comma = s.indexOf(',', from);
    String tok = (comma < 0) ? s.substring(from) : s.substring(from, comma);
    if (field < 9) f[field] = tok.toFloat(); else n = tok.toInt();
    if (comma < 0) break;
    from = comma + 1;
  }
  r.a0 = f[0]; r.a1 = f[1]; r.rpm = f[2];
  r.iPt[0] = f[3]; r.iPt[1] = f[4]; r.iPt[2] = f[5];
  r.pkPt[0] = f[6]; r.pkPt[1] = f[7]; r.pkPt[2] = f[8];
  r.nPts = (uint8_t)constrain(n, 0, 3);
}
float   cvComputedKp = 30.0f;     // user-facing (12V-equivalent) gains the Measured path produced — dashboard display only
float   cvComputedKi = 25.0f;
float   cvComputedKd = 5.0f;      // Measured path's D gain = CvKdTd·cvComputedKp (12V-equiv); dashboard display only
// CV auto-gain knobs (Tuning ▸ Voltage): Kp = cvAlpha/(cvPlantKa·vNorm), Ki = cvPiZero·Kp. α is
// dimensionless (fraction of the deadbeat gain 1/K), so one α gives every chemistry the same character.
float   cvAlpha      = 0.05f;     // fraction of deadbeat 1/K; coupled to the ~0.6 s CVPF horizon — retune together
float   cvCrossover  = 0.20f;     // rad/s — retired ω_c input, inert; NVS key + CSV3 slot kept (never repurpose)
float   cvPiZero     = 0.50f;     // rad/s — PI integral zero ρ; Ki = ρ·Kp
float   CvKdTd       = 0.85f;     // s — derivative time; Measured-source Kd = Td·Kp (D analog of ρ), so Kd inherits Kp's α/K plant anchor and scales per-install through the wizard
// Battery-temperature gain derate (batteryTempF(), sources 1..4): internal resistance = CV plant gain K_dc rises as the bank cools,
// so Kp AND Ki scale by R(T_commission)/R(T_now), holding ω and ρ. CommissionTempF is stamped when the CV plant fit is applied;
// NaN = no derate. See recomputeCvGains()/computeCvTempScale() in 6_functions.ino.
float   CommissionTempF      = NAN;   // measured battery temp (°F, batteryTempF() source) when K_dc was measured; reference for the derate
int     CommissionTempSrc    = 0;     // battTempActiveSrc at the moment CommissionTempF was stamped; 0 = legacy/unknown, 5 = retired board stamp — either makes the derate inert
bool    battTempDerateEnable = true;  // master on/off for the battery-temp gain derate
float   battTempCoeff        = 0.024f; // fractional resistance change per °C BELOW 25°C; cvResistanceRatio() halves it above 25°C.
                                       // One value for every chemistry: LFP and lead-acid measure within 12% over −20…+55°C.
// Re-commission prompt: bank resistance (the CV plant gain) roughly doubles over its life, and a Vessel Info voltage/capacity/
// chemistry change invalidates the fit; both make the loop too hot. Epoch stamped from the BROWSER clock at commissionDone
// (device clock may be unset at sea; a bad stamp would silence the prompt forever). 0 = never finished a pass ⇒ never nag.
time_t  CommissionEpoch      = 0;
bool    commissionAgeAck     = false; // user pressed Silence on the age prompt; cleared by the next pass
bool    commissionChangeFlag = false; // tuning-affecting Vessel Info change since the last pass
float   cvTempDerateScale    = 1.0f;  // live multiplier recomputeCvGains() applies to the active gains (dashboard diag)
// CV target slew: ChargingVoltageTarget ramps toward ...Req so a COMMANDED change doesn't step and trip fast-OV; the CV loop AND
// the OV protections see the smooth value. 0 = instant for that direction. Hard-shutdown unaffected.
uint8_t vTgtRampEnable = 1;       // master switch for the target slew (1=on); 0 = instant target, for A/B incl. CV tuning
float   vTgtRampUp   = 0.15f;     // V/s — max rate the target may RISE  (up-steps are loop-limited anyway)
float   vTgtRampDn   = 0.15f;     // V/s — max rate the target may FALL  (protection-reference glide; the wind-down governor below owns the actual current shed on commanded drops)
// Wind-down governor: on a commanded target DROP, walk the current command down and stop on the MEASURED bus rather than PI-track
// within the OV margin (PI shed ≈ 16 mV/s: a 0.6 V drop left the bus +110 mV over the glide and fired G2).
uint8_t cvWindDownEnable = 1;         // master switch (0 = PI + glide alone)
float   cvWindDownRate   = 0.05f;     // fraction of MaxTableValue shed per second (self-scales with alternator size)
float   cvWindDownStopV  = 0.05f;     // V real per-bus — stop walking when filtered bus is within this of the commanded target (class-scaled at store)
const float CV_WINDDOWN_TRIG_V = 0.15f;  // V ×class at use — commanded drop must exceed this to arm (small trims stay pure PI)
const float CV_TGT_PACE_LEAD_V = 0.02f;  // V ×class at use — down-glide holds while getFiltV() lags more than this behind the reference (bus-paced descent floor). Must stay well under OvMeasMarginV minus the filtered ripple crest (~70mV worst bench) or ripple can still bridge the gap
bool  cvWindDownActive = false;       // cleared by runShutdownPath/runCommissionIdle/MANUAL/the test branches (cvYieldToTest) like lastVoltageControlActive
float cvWindDownCap    = 0.0f;        // ramped ceiling on Icv (and cv_I) while active
float cvWindDownFinalV = 0.0f;        // commanded final target latched at arm; follows further drops, abort on raise
volatile float VoltageKp_active = 8.5f;  // DERIVED pack-space Kp the loop uses (selected gain × 12/SYSTEM_VOLTAGE_CLASS). Set by recomputeCvGains().
volatile float VoltageKi_active = 6.0f;  // DERIVED pack-space Ki the loop uses.
volatile float VoltageKd_active = 10.0f; // DERIVED pack-space D gain the loop uses (VoltageKd × 12/SYSTEM_VOLTAGE_CLASS × temp derate). Set by recomputeCvGains(), same pipeline as Kp/Ki.
volatile float VoltageKp = 8.5f;    // A/V — proportional gain (volatile: written from Core 0 web handler, read from Core 1 PID); from commissioning CV plant fit (K20=32.2 mV/A, 12 V-equiv)
volatile float VoltageKi = 6.0f;    // A/(V·s) — integral gain; above-target unwind uses KiDown = 7×VoltageKi; from commissioning CV plant fit. Deferred cv_I anti-windup still open (see CV_Loop_Dev_Summary.md Future Work)
// CV D term — trim is a position subtracted at the Icv output, never integrated into cv_I, so it releases the tick the rise
// stops. Do not integrate it: a slope bleed over-dosed via the ~0.6 s actuator lag.
float VoltageKd = 5.0f;             // A/(V/s), 12V-EQUIVALENT (runtime-normalized like Kp/Ki — do NOT class-scale at seed); knee: trim hits CvKdMaxTrimA at slope = cap/Kd
float CvKdDeadbandVps = 0.4f;       // V/s at zero amps — BASE of the deadband line clamp(CvKdDbFloor, base + slope·I, CvKdDbCeil); hysteretic gate: D off below the line, FULL slope past it, re-arms at half. A SAFETY MARGIN: the Step-5 ripple fit overwrites base + CvKdDbSlope to sit this margin above measured ripple; 0.4×class is the pre-commissioning fallback ONLY (CvKdDbFloor is the hard minimum)
float CvKdDbSlope = 0.0f;           // V/s per A — deadband line slope vs the slew-limited command (ripple-driven slope noise grows with output, same physics as the G3 trip line); 0 = flat, the pre-measurement behavior; set from the Step-5 slope fit b1
float CvKdDbFloor = 0.10f;          // V/s — lowest the evaluated deadband may go (guards the low-current extrapolation below the tested sweep span)
float CvKdDbCeil  = 3.5f;           // V/s — highest the evaluated deadband may go; MUST stay below CvKdSlopeCeil: the gate compares cvDSlope already clamped to ±CvKdSlopeCeil, so a deadband at/above it could never be crossed and D would go dead at that current
bool  CvKdExcessMode = true;        // true = trim ∝ (slope − deadband line), continuous from zero at the line — no engagement step, so a small coherent swing can't self-sustain (a 1.4 Hz hold limit cycle). false = legacy full-slope hysteretic latch (bench A/B only): trim jumps to Kd×deadband at the line — that step is a relay behind the ~0.2 s field lag
bool  CvKdOneSided = false;         // false = two-sided: also adds on a fast fall, ONLY below target (that gate is what makes two-sided safe)
float CvKdArmV = 0.5f;              // V below target within which D acts (0 = always); tight so D stays off the sag-recovery climb and only brakes the final approach (1.25 chopped the recovering field)
float CvKdMaxTrimA = 500.0f;        // A — deliberately-inactive backstop on the D back-off; the real bound on D depth is CvKdSlopeCeil × Kd, this only bites at Kd far above the seeded range. FLAT amps
float CvKdVoltFiltTC = 40.0f;       // ms — dedicated EMA on IBV feeding only the D slope (g_cvKdFiltV), separate from VoltageFilterTC; 0 = raw IBV
float CvKdSlopeCeil = 10.0f;        // V/s real per-bus (WYSIWYG; seed/class-scaled ×V/12 at store) — max slope the D acts on; the real cap on D depth
bool  cvHelpersEnabled = true;      // master switch for the two non-linear CV "helpers" (asymmetric 7× KiDown unwind + the one-sided D term). ON = both active (reduce overshoot / speed OV recovery). OFF = symmetric plain PI, easier to tune/understand. Does NOT touch real OV protections.
uint32_t VoltageLoopInterval = 100;  // ms — PI fires at this interval
// --- FastOV supervisor ---
float KHard = 35.0f;  // A/V — OV cap slope (Group 1: Vpred > target+OvPredMarginV; Group 2: IBV > target+OvMeasMarginV)
bool  OvGroup1Enable  = false;  // Group 1 — prediction-based cap (Vpred > target + OvPredMarginV). OFF by default: its predictive lead amplifies belt ripple 1:1 with the extra margin it buys (CV_RECOVERY_AND_OV_HANDOFF.md). Provisioned devices keep their NVS value.
bool  OvGroup2Enable  = true;   // Group 2 — measured-voltage threshold enable (IBV > target + OvMeasMarginV)
int   OutputPIDSigSrc = 0;      // Output current PID — 0=EMA(TC), 1=MA(N), 2=Raw
int   OutputPIDMA_N   = 2;      // Output current PID — MA window size (1–10 samples)
float TdPred         = 0.045f;  // Group 1 lookahead horizon (s)
float OvMeasMarginV  = 0.100f;  // Group 2 measured-voltage trigger margin above target (V)
float OvPredMarginV  = 0.150f;  // Group 1 prediction trigger margin above target (V)
// --- Timed OV cut tiers (target-relative, dwell-debounced field cuts on the G2 filtered bus) ---
// Sized so every sustained violation is cut inside 0.4 s — a BMS charge disconnect needs >= 0.5 s
// (survey floor) and ~100 ms of that budget is field de-energization. Dwells are absolute time,
// never class-scaled; margins are volt-domain (x class/12 at seed and class change). Dwell 0 = tier off.
float    OvTierLoMarginV  = 0.10f;  // LOW tier trip line, V above target (rides the Group 2 shed line)
uint32_t OvTierLoDwellMs  = 400;    // LOW tier continuous dwell (ms)
float    OvTierMidMarginV = 0.20f;  // MID tier trip line, V above target
uint32_t OvTierMidDwellMs = 150;    // MID tier continuous dwell (ms)
volatile uint8_t g_ovTierTrip = 0;  // latched tier violation: 0 none, 1 LOW, 2 MID — set in the FAST OV block, read by the reason/mode arbiters, clears when the filtered bus drops back under the tier line
float DvdtTC         = 58.0f;   // ms — TC for dvdt (rate-of-rise) EMA fed into Vpred. dt-aware: alpha = dt/(TC+dt).
// --- iExcess current supervisor (EMA / leaky-integral detector) ---
// Detection = EMA of signed current excess over command, thresholded against a FRACTION of the command; averaging cancels
// zero-mean oscillation (belt resonance, stator imbalance, rectifier ripple). See iExcess_Redesign_Spec.md, CV_Loop_Dev_Summary.md.
bool IExcessEnable    = true;    // Group 3 enable — gates BOTH iExcess detectors (CV + bulk); OV groups, Load Dump, and the hard OC trip are unaffected
float IExcessFrac     = 0.031f;  // shared trip-line SLOPE (A per A of command); commissioning sets it to the measured ripple slope. E = clamp(floor, frac·cmd + base, ceil).
float IExcessFracBulk = 0.031f;  // CC-detector slope; the UI keeps it equal to IExcessFrac so the CV and CC trip lines stay parallel. Differs from CV only on an uncommissioned device.
float IExcessBaseA    = 5.8f;    // A — trip-line intercept (CV base). 0 = through-origin. Commissioning sets it to ripple-at-idle + Safety Margin, lifting the line above the measured ripple.
float IExcessCcOffsetA = 4.0f;   // A — the CC trip line sits this far above the CV line (parallel offset). 0 = coincident with CV.
float IExcessFloorA   = 5.0f;    // A — trip-line floor; it never dips below this even at tiny commands.
float IExcessCeilA    = 20.0f;   // A — trip-line ceiling; it never rises above this on very large commands.
float BattCurrentLimitA = 100.0f;  // A — max battery charge current (G4). Ceiling on the alternator-amp command = limit + (house loads − other charge sources), signed; requires the INA228 battery shunt as Battery Current Source. 0 = disabled.
bool BattLimitEnable  = true;    // Group 4 enable — gates the battery charge-current ceiling only
bool LoadDumpEnable   = true;    // Group 5 enable — gates the Load Dump dBcur/dt trip, which survives even the global protections toggle; this is its only disarm
float IExcessTau      = 75.0f;   // ms — EMA time constant. Sized by the worst-case (idle) belt resonance; one fixed value covers the whole RPM range. dt-aware alpha.
float IExcessRelFrac  = 0.5f;    // hysteresis: release the fire latch when mExcessEma falls below IExcessFrac-threshold × this (scale-aware).
float IExcessKBleed = 0.0f;      // 0=snap-to-zero; >0=proportional bleed rate (A/s per A of excess)
float IExcessArmMarginV = 0.100f; // V below target at which iExcess voltage gate opens. Sized to block recovery double-fires (belt-resonance peaks mid-recovery) while keeping real catches (which fire at/above target)
float ReseedFrac = 0.95f;  // shunted installs: fraction of pre-event cv_I to seed on any protection recovery (the measured demandDrop already corrected the base). Below-target tails are capped by the bus-watched climb floor (CvRecovClimbRate). Must stay <1.0 (first-fire de-escalation on a persistent cause).
float ReseedFracNoShunt = 0.50f;  // no-shunt installs: the departed-load measurement needs Bcur, so a load-dump fire reseeds blind. Seed low and let the bus-watched climb rediscover demand — a 0.95 blind seed after a 24A dump re-fired G2 twice
float CvRecovClimbRate = 0.10f;   // fraction of MaxTableValue/s — recovery up-integration floor rate (0.10×150A=15A/s), gated off at projected bus arrival
// --- Anti-windup ---
float AwBleedRate = 2.0f;        // fraction of MaxTableValue/s — cv_I bleed rate while fastOV active (2.0×50A=100A/s)
float AwRecoverRate = 0.1f;      // HARDCODED, not user-adjustable. cv_I_aw_cap recovery rate (fraction of MaxTableValue/s) after fastOV clears. Only exercised on cold CV re-entry (MANUAL→AUTO, idle→bulk, post-shutdown). CSV3 slot CSV3_reserved_AwRecoverRate held for future use.
uint16_t AwSeedProtectMs = 150;  // ms to suppress AwBleed + CC-tracker after any bumpless seed fires; 0=disabled
// --- Recovery over-ramp restraint (inner output-current PID up-integration) ---
float FastSetpointRiseRate = 8.0f;       // multiplier on normal setpoint rise slew during post-protection recovery window
uint32_t FastSetpointRiseWindowMs = 5000; // hard upper bound (ms) on how long the fast-rise window stays open after any protection releases
float FastSetpointRiseHeadroomV = 0.2f;  // V below ChargingVoltageTarget at which fast-rise is allowed; gate closes once IBV climbs into target - this margin
// --- Test-mode protection override ---
// Per-test-page protection override, NOT persisted (TRUE at boot). FALSE disables G1, G2, the timed OV tiers, G3 iExcess and the
// G4 battery limit so step-tests can characterise the plant. Always active regardless: AlternatorHardShutdownV, INA228 hardware OV,
// Load Dump (G5) and the hardware OC trip (MaxTableValue+10).
bool testProtectionsEnabled = true;
bool commissionProtBackup   = true;  // testProtectionsEnabled saved at commissionStart, restored on done/abort; commissioning forces protections ON so a stray tuning-tab "off" can't weaken its steps
// --- CV loop runtime state ---
uint32_t lastVoltageLoopMs = 0;           // timestamp of last voltage loop update
uint16_t g_voltLoopActualIntervalMs = 0;  // actual interval of last voltage loop fire (ms); 0 until second fire
uint16_t g_voltLoopLogIntervalMs = 0;     // same for the pidLog/cvLog column, but 0 on the first fire after a stretch the loop did not run (vlHasPrev rule), so the off-gap is never logged; the control math keeps the raw value
bool g_cvLogVoltLoopFired = false;  // voltage loop fired since the last cvLog entry; cleared by cvLog_tick. pidLog_voltageLoopRanThisTick can't serve: it is reset every AdjustFieldLearnMode pass, so it is 0 by the next CH1 read
float g_kdTrimThisTick = 0.0f;   // D-term current reduction latched for the next cvLog entry (A) — most recent nonzero trim since the last log write; cleared by cvLog_tick after logging
float voltageTargetSlewed = 0.0f;        // rise-governor output — the setpoint the CV PI tracks. Global (not a static local) only so cvLog_tick can record it; written solely by the governor in AdjustFieldLearnMode.
float Icv = 0.0f;                         // CV PID output — direct current setpoint (A)
float cv_I = 0.0f;                        // CV integrator state (A)
bool voltageControlActive = false;        // true when voltage PID is active (non-idle stages)
bool lastVoltageControlActive = false;    // CV-entry tracker for the bumpless seed, maintained by the CV block and the MANUAL branch. runShutdownPath/runCommissionIdle return before that line, so they clear it themselves — stale-TRUE skips the entry seed and resumes a stale cv_I (double fire). The in-AUTO test branches clear it too (cvYieldToTest); nothing relies on it surviving a test.
uint32_t thermalCvOwnStartMs = 0;         // millis() the CV loop took the current command (0 = it has not); de-glitch timer for the thermal accuracy gate
// =====================================================================================
// Table Bounds & Safety
// "Group 0" in UI = hardware overcurrent trip; HardOCEnable is its group toggle (ignores the global protections flag)
float MaxTableValue = 150.0;               // Maximum table entry (A)
float MaxPenaltyPercent = 15.0;            // Max penalty as % of nominal
unsigned long MaxPenaltyDuration = 60000;  // Max penalty time (ms)

float NeighborLearningFactor = 0.25;                // Neighbor reduction factor
unsigned long LearningMemoryDuration = 2592000000;  // How long to remember events (30 days in ms)

// Safety Overrides
int IgnoreLearningDuringPenalty = 1;

// Diagnostics & Debugging
int LogAllLearningEvents = 0;       // Log every learning decision
int CloudFeatures = 1;
int RemoteDiagnostics = 1;  // gates the dashboard's support log-relay poll; firmware only stores and echoes it

// Data Management
// Deferred saves — set by Core 0 (AsyncWebServer handlers), executed on Core 1 in main loop
// to avoid blocking SSE delivery on Core 0
volatile bool pendingSaveCVTuningLog = false;
volatile bool pendingSaveTuningLog = false;
volatile bool pendingSaveSystemIDLog = false;
volatile bool pendingSaveTuningSweepLog = false;
volatile bool pendingSaveSysidSweepLog = false;
volatile bool pendingResetAlternatorHealth = false;
volatile bool pendingResetBoatPerformance = false;
volatile bool pendingClearOverheatHistory = false;
volatile bool pendingRpmAxisWipe = false;  // /get?RPMScalingFactor changed value → local wipes run on Core 1
volatile uint16_t pendingImportBootAct = 0;  // IBA_*/AXW_* work a config import left for this boot (NK_impBootAct)
// Set by a records import: saveNVSDataFull and altHealthSave would write this regulator's own copies over the
// imported keys, so they stand down until the reboot that loads them. That reboot is never optional:
// importRebootDeadlineMs forces it if the config import the dashboard sends next never arrives.
volatile bool importRebootPending = false;
volatile uint32_t importRebootDeadlineMs = 0;
float zeroBasisA = 0.0f;   // AlternatorCOffset the RAM zero-log rows and fit history are on; Core 1 only (zeroLogService moves it)
volatile bool rpmAxisWipePending = false;  // NK_RpmAxisWipePend: cloud half still owed; suppresses front sync
volatile bool altCloudWipePending = false; // NK_AltWipePend: alt-health Start Over's cloud half still owed; suppresses alt-health upload + sync-back
volatile bool perfCloudWipePending = false; // NK_PerfWipePend: a boat-performance Clear All's or records import's cloud half still owed; suppresses boat-perf upload + sync-back
uint8_t altHealthWindV = 0;                // NK_altWindV: winding the alt-health record is on; 0 until altHealthWindingInit
volatile bool pendingSaveUserTableEdits = false;
volatile bool tableSaveAnnounce = false;  // a /get table edit: saveUserTableEdits() says "saved" only once NVS took it
volatile bool pendingSaveVesselInfo = false;
bool pendingShutdownFlush = false;     // set on ignition-off edge; cleared after full flush
bool shutdownNVSFlushDone = false;     // true once NVS+sensor window saved this shutdown

// ── Commissioning ledger (COMMISSIONING_LEDGER_SPEC.md) ──────────────────────
// Append-only event evidence staged in PSRAM → /cxledger.bin → log-commissioning-event, drained under the field-off cloud gates.
// Producers run on BOTH cores, so staging is spinlock-guarded; ALL file access is Core 1 only.
#define CX_LEDGER_PATH       "/cxledger.bin"
#define CX_LEDGER_FILE_CAP   32768u   // on-flash queue cap; oldest rows dropped when full
#define CX_LEDGER_PEND_CAP   8192u    // PSRAM staging cap per buffer (double-buffered swap)
#define CX_LEDGER_ROW_MAX    4096u    // sanity bound per row (biggest real row ~2.8 KB: a full 183-point field curve)
#define CX_LEDGER_BATCH_CAP  24576u   // one upload batch: envelope + rows
#define CX_LEDGER_BATCH_ROWS 20       // max events per POST (edge fn enforces 25)
char *cxPendBuf = nullptr;            // active staging buffer (ps_malloc, cxLedgerInit)
char *cxPendSwap = nullptr;           // flush swaps this in so producers never block on flash
volatile uint32_t cxPendLen = 0;
portMUX_TYPE cxPendMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t cxLedgerSeq = 0;             // monotonic event counter, NVS-backed; cloud dedupes on (device_uid, seq)
volatile uint32_t cxLedgerDroppedRows = 0;   // rows lost to caps — confessed in the next row's data
volatile bool cxLedgerFlushPending = false;
volatile int8_t cxLedgerUpState = 0;  // 0 idle · 1 in flight · 2 sent-ok (trim due) · -1 failed
uint32_t cxLedgerUpBytes = 0;         // file bytes staged in the in-flight batch
unsigned long cxLedgerLastAttemptMs = 0;
uint32_t shutdownCloudDeadlineMs = 0;  // millis() deadline for cloud drain window

// Field-off flush triggers, two staggered gates off the GPIO4-cut edge: +20 s saveNVSDataFull(), +28 s altHealthSave() (~67 KB
// LittleFS). 20 s = 2× the fast-OV lockout cap (10 s), so OV cut/retry churn never reaches a commit (no flash work during dynamic
// protection events); must grow if that cap grows. Long static lockouts may flush mid-lockout with the field hardware-dead.
// Staggered so the LittleFS write misses the NVS relocation tail; both land before the 70 s upload TLS handshake. Re-armed on field-on.
bool fieldOffFlushDone = false;         // NVS drain done this field-off window
bool fieldOffMatrixFlushDone = false;   // matrix write done this field-off window
int8_t lastFieldStateForFlush = -1;    // -1 = uninit; 0 = field off; 1 = field on
uint32_t fieldOffEdgeMs = 0;           // millis() captured at most recent field-on -> field-off edge


uint32_t perfCountersResetMs = 0;

// ==================== ENUMS ====================
enum FieldControlMode {
  MODE_CRITICAL_RAMP,             // Critical fault: ramp to 0, cut if fault persists
  MODE_WARNING_RAMP_AND_LOCKOUT,  // Warning fault: ramp to 0, cut if fault persists, start lockout
  MODE_LOCKOUT_RAMP,              // Lockout: ramp to 0, cut when settled
  MODE_DISABLED_RAMP,             // Normal shutdown: ramp to 0, cut when settled
  MODE_NORMAL_MANUAL,             // Manual control with rate limiting
  MODE_NORMAL_AUTO,               // Auto mode (Field Control = Auto): charge stages, current/voltage loops, every protection
  MODE_COMMISSION_IDLE,           // Commissioning "rest" between guided steps: hold field at a low
                                  // duty (per-RPM floor or a voltage-scaled minimum), slow ramp, no
                                  // GPIO4 cut — keeps the RPM pickup alive instead of resuming charging
  MODE_FIELD_RES_TEST             // Field resistance test (engine off): the test's own hold duty, GPIO4 up
};

enum FieldEventReason : uint8_t {
  REASON_NONE = 0,
  REASON_TEMP_STALE,
  REASON_TEMP_CRITICAL,
  REASON_TEMP_WARNING,
  REASON_TEMP_SUSTAINED,
  REASON_VOLTAGE_IMPLAUSIBLE,
  REASON_VOLTAGE_DISAGREE_CRITICAL,
  REASON_VOLTAGE_SPIKE,
  REASON_VOLTAGE_DISAGREE_WARNING,
  REASON_LOCKOUT_ACTIVE,
  REASON_CHARGING_DISABLED,
  REASON_MANUAL_MODE,
  REASON_INA_OVERVOLTAGE,
  REASON_HARD_OVERCURRENT,
  REASON_RPM_TOO_LOW,
  REASON_CURRENT_STALE,
  REASON_FAST_OVERVOLTAGE,         // absolute OV ceiling — fires in ALL modes incl. MANUAL (live per-tick, immediate cut)
  REASON_BATTERY_TOO_COLD,         // measured battery temp below MinChargeTempF — cold-charge lockout (opt-in, lithium protection)
  REASON_COMMISSION_REST,          // commissioning idle-rest hold between guided steps (not a fault)
  REASON_TACH_IMPLAUSIBLE,         // field driven hard, zero alternator output while tach claims running — open field drive (ON/OFF/wiring/gate-drive), dead alternator, or false RPM (tach noise)
  REASON_SOLAR_PAUSE,              // weather mode is resting the alternator on a strong solar forecast — deliberate, not a fault
  REASON_BMS_DISABLED,             // the BMS on/off input is withholding permission — external command, not a fault
  REASON_OV_TIER_LOW,              // timed OV cut, LOW tier — filtered bus held above target + OvTierLoMarginV for OvTierLoDwellMs (AUTO/CV only)
  REASON_OV_TIER_MID,              // timed OV cut, MID tier — filtered bus held above target + OvTierMidMarginV for OvTierMidDwellMs (AUTO/CV only)
  REASON_BATTERY_TOO_HOT,          // 24 — measured battery temp (source 1..4) above MaxChargeTempF — hot-charge lockout (opt-in)
  REASON_CHARGE_COMPLETE_IDLE,     // 25 — charge cycle finished with UseFloat=0: battery full, resting in IDLE until a rebulk or a master-switch off/on. Same class as CHARGING_DISABLED everywhere; only the label differs
  REASON_ALTZERO_MEASURE,          // 26 — a requested current-sensor zero measurement is holding the field at 0% (altZeroCaptureService). Not a fault: MODE_COMMISSION_IDLE with a zero target, released when the window closes
  REASON_BATT_TEMP_LOST,           // 27 — an expected battery temperature (battTempSource != Not fitted) has no qualifying reading past the boot grace — graceful ramp + settle cut until it returns or the owner sets Not fitted
  REASON_VOLTAGE_STALE,            // 28 — no valid INA228 bus-voltage read committed within the stale limit (10 invalid reads field-on / 3 field-off) — immediate cut, critical tier
  REASON_VOLTAGE2_STALE,           // 29 — no valid ADS1115 CH0 battery-voltage read committed within 500 ms — critical ramp
  REASON_BATT_TEMP_WAIT,           // 30 — an expected battery temperature has not delivered its first reading yet (boot / source-change grace, §2.3): field held off quietly — no alarm, no fault, no lockout; becomes BATT_TEMP_LOST at 90 s
  REASON_MULTI_SENSOR_FAULT,       // 31 — two or more required sensors faulted at once (§11: g_reqFaultCount >= 2, owner overrides and Not fitted excluded) — immediate cut, 30 s lockout, clears after the count has been <= 1 for 10 s
  REASON_DIE_TEMP_LOST,            // 32 — no valid INA228 die-temperature sample for 10 s past 15 s of uptime: the field-drive heat ceiling has no input — graceful ramp + settle cut, like TEMP_STALE, until it reads again
  REASON_FIELD_RES_TEST            // 33 — the field resistance test is driving the field (engine off). Not a fault
};


struct TickSnapshot {
  uint32_t nowMs;
  uint32_t dt_ms;
  float currentBatteryVoltage;
  float batteryV;
  float ibv;
  float batteryCurrentA;
  float rpmMinDuty;

  bool chargingEnabled;
  bool manualMode;
  bool solarForecastPause;   // charging would be on; weather mode alone is holding it off
  bool bmsBlocking;          // charging would be on; the BMS on/off input alone is holding it off
  bool idleHold;             // charging would be on; the finished charge cycle (IDLE stage) alone is holding it off

  bool tempDataVeryStale;
  bool ignoreTemperature;
  bool ignoreRPM;
  bool rpmBelowMinimum;      // raw: below MinRPMForField or restart pending (input to the spin-down cut and rpmGateActive)
  bool rpmGateActive;        // what the arbiter's RPM gate sees: rpmBelowMinimum at once with the field off, two-reading confirmed with it on
  bool engineFullyStopped;   // RPM at 0 for RPM_ZERO_CUT_MS with a second zero reading (or RPM_ZERO_MAX_WAIT_MS) — forces immediate field cut
  bool tachImplausible;      // tach claims running + field driven hard + zero output held TACH_LIE_DWELL_MS — forces immediate cut + lockout

  bool voltagePlausible;
  bool voltageDisagreementCritical;
  bool voltageDisagreementWarning;

  bool inLockout;

  float bulkVoltage;
  float alternatorHardShutdownV;
  bool testProtectionsEnabled;  // snapshot of the per-tick value so the decision logic sees a stable flag

  float tempToUseF;
  float tempLimitF;
  float tempWarnExcessF;
  float tempCritExcessF;
  bool tempSourceIsAlt;

  bool inaOvervoltageLatched;

  bool currentDataStale;

  bool inAbsorptionStage;

  bool batteryTooCold;   // cold-charge lockout active (measured battery temp < MinChargeTempF, with hysteresis)
  bool batteryTooHot;    // hot-charge lockout active (measured battery temp > MaxChargeTempF, with hysteresis)
  float battTempF;       // batteryTempF() this tick (°F); NAN = no source qualifies
  uint8_t battTempSrc;   // batteryTempF() source code (0 none, 1 probe, 2 NMEA 2000, 3 VE.Direct, 4 RV-C)
  bool commissioningResting;  // commissioning session live + dialog alive + no test running → hold field at rest duty
  bool altZeroMeasuring;      // a requested current-sensor zero measurement wants the field at 0% (altZeroCaptureService
                              // phase >= ramp, altZeroHoldField, charging enabled, no lockout) → MODE_COMMISSION_IDLE, zero target
  // Sensor-fault / run-without-sensors state (SENSOR_FAULTS_AND_OPTIONAL_SENSORS_SPEC.md)
  bool engineRunning;         // §1.1: RPM >= 200 held 15 s, or the effective ignition when engine speed is ignored
  bool ignoreAltCurrent;
  bool ignoreBattVMain;
  bool ignoreBattV2;
  bool limpHome;
  uint8_t limpTier;           // §4.2: 0 normal, 1 closed-loop minus dependent reasons, 2 voltage-only, 3 fixed duty + second-voltage cap, 4 fixed duty
  bool battTempExpected;      // §2.2: battTempSource != 6
  uint8_t battTempState;      // §2.6: 0 not expected, 1 reading, 2 lost, 3 waiting for the first reading (field held off)
  bool ibvStale;              // §10.1 REASON_VOLTAGE_STALE condition (before override suppression)
  bool battVStale;            // §10.2 REASON_VOLTAGE2_STALE condition (before override suppression)
  bool voltageFeedbackOk;     // a fresh, trusted battery voltage is feeding the OV rungs (IBV, or the second reading in tier 3); false = fast OV / OV tiers cannot act
  bool hardOverCurrent;       // HardOCTripAmps exceeded on a FRESH alternator-current sample for HardOCDebounceMs (debounce lives in buildTickSnapshot; both ladders read the flag)
  bool multiSensorFault;      // §11: two or more required sensors faulted (g_reqFaultCount >= 2), held through the 10 s clear dwell
  bool dieTempLost;           // REASON_DIE_TEMP_LOST condition (before override suppression)
  bool fieldResTest;          // the field resistance test is running (frt.state == FRT_RUNNING)
};

// ==================== CONFIGURABLE PARAMETERS ====================
// Expose these in web UI for tuning

// --- Rate Limiting (LM2907 coupling cap protection) ---
float DutyRampRate = 40.0f;  // %/sec - max rate of duty cycle change (protects coupling cap from harsh transitions, includes OnOff toggle!)
const float DUTY_RAMP_MIN_12V = 0.1f;  // %/s x fieldScale(), both duty ramp rates: at 0 a shutdown ramp never reaches the cut
uint8_t dutySlewEnable = 1;  // master switch for the field duty slew (GOV_NORMAL_SLEW). 0 = duty steps instantly (A/B); applies in BOTH commissioning tests and normal operation
// Asymmetric setpoint slew
float SetpointRiseRate = 30.0f;  // A/sec
float SetpointFallRate = 50.0f;  // A/sec
float CvBrakeFallRate = 200.0f;  // A/s — brake-tier setpoint down-slew while the CV D-term removes current; between the 50 A/s comfort limit and the fastOvClamp slam; flat amps
uint8_t setpointSlewEnable = 1;  // master switch for the inner-loop current setpoint slew. 0 = setpoint steps instantly (drops startup ramp / big-step gentling too); applies in the CC test and normal operation
// Manual CC square-wave test slew mode (Current-tab Test Limiters): 0=Off (setpoint abrupt + duty bypassed), 1=Default (factory
// rate below, DutyRampRate kept), 2=Custom (SetpointRiseRate/FallRate). Manual square test ONLY — never auto tests or sines.
uint8_t testSlewMode = 2;
const float SETPOINT_RISE_DEFAULT = 30.0f;  // A/s — factory setpoint slew for testSlewMode=Default (matches SetpointRiseRate's own default)
const float SETPOINT_FALL_DEFAULT = 50.0f;  // A/s
// Manual CV square-wave test slew mode (Voltage-tab Test Limiters): 0=Off (target steps instantly), 1=Default (ramp below),
// 2=Custom (vTgtRampUp/Dn); manual CV square test ONLY. Off keeps the base field duty slew as a conservative default, not a
// requirement: the safety that matters is the cvWaveExitWindDown glide at test exit.
uint8_t cvTestSlewMode = 2;
const float VTGT_RAMP_DEFAULT = 0.050f;  // V/s @12V — factory target ramp for cvTestSlewMode=Default; scaled ×SYSTEM_VOLTAGE_CLASS/12 at use
uint8_t cvRiseGovEnable   = 1;   // master switch for the CV voltage-target rise governor (anti-windup clamp). 0 = rises are not clamped — integrator can wind up into an OV trip on an up-step; applies in the CV test and normal operation
uint8_t cvRecovEnable = 1;   // master switch for the post-protection integrator refill (deficit-gated Ki boost). 1 = up-integration runs at Ki × up-to-cvRecovKiMax, tapering with the remaining reseed deficit; 0 = plain PI walks itself back (pace scales with post-cut error). Default ON (A/B: single fire, ~5× refill rate, healed exit, no overshoot)
uint8_t loadServeBoostEnable = 1;  // load-serve Ki boost: while the battery shunt shows sustained discharge in CV, up-integration runs at the refill's boosted rate toward the measured house loads — a stiff plant hides a 35A load behind ~0.25V of error, so plain Ki picks it up in ~30s. Shunt-gated; inert without one. Dev default ON, production TBD
uint8_t reseedCorrEnable = 1;      // demand-corrected reseed: subtract the measured load drop at the fire from the release reseed + refill goal (shunt-gated), and escalate the ratchet ×0.85 per re-fire <1.5s after release (shunt-independent). OFF = plain ReseedFrac reseed
// --- Hunt Governor v2 (persistent speed-map oscillation damper — HUNT_GOVERNOR_V2_SPEC.md) ---
uint8_t HuntGovEnable = 0;           // 1 = watch applied duty for a sustained 0.3–3 Hz hunt (idle-knee speed-coupled plant); a verified single cut to HuntCutPct is mapped as a persistent speed pocket and applied proactively on pocket entry. Opt-in; NVS-seeded units keep their stored value
volatile float g_huntDerate = 1.0f;  // live Ki multiplier; applied inside recomputeCcGains so every SetTunings path preserves it: the map value at the current speed (HuntCutPct/100 inside a pocket core), or the held test cut during an episode
volatile float g_huntKdScale = 1.0f; // live CV D-term multiplier: 0 during a D test and inside a verified D-pocket core, wing-tapered between, applied at the kdTrim choke point. Binary, never HuntCutPct: a PARTIAL D cut only moves the deadband limit cycle to a larger amplitude
float g_huntFreqHz = 0.0f;           // last confirmed wobble frequency (CSV1/ledger)
uint8_t g_huntState = 0;  // 0 watching (gain follows the pocket map), 1 testing Ki (single cut applied, verdict pending), 3 cooldown after a failed test, 4 testing the CV D-term lever (D paused, Ki untouched); 2 unused — file-global so CSV1 can stream it to the Diag panel
uint8_t HuntCutPct = 50;       // gain the damper cuts TO during a test and holds inside a pocket core, % of user Ki. One big cut gives the verify comparison its S/N; also the map floor — repeat episodes widen pockets, never deepen. Clamp [25,80]
uint8_t HuntVerifyPct = 15;    // required ripple reduction (%) for a test to pass, 3-scan average vs 3-scan average — single windows jitter more than this on their own. Clamp [5,60]
uint8_t HuntWingPct = 15;      // pocket edge taper width, % of speed beyond each core end over which gain ramps linearly back to full. Clamp [5,40]
uint8_t HuntCooldownMin = 10;  // minutes after a failed test before another test may open. Clamp [1,60]
uint8_t HuntSteadyPct = 3;     // engine-speed steadiness tolerance (%): scan-mean RPM must stay within this of the episode reference through qualify+verify, else the episode aborts with nothing kept. Clamp [1,10]
uint8_t HuntQualifyScans = 3;  // consecutive hunt+steady scans (1.6 s each) required to confirm a wobble and open a test; their average is the verify baseline. More scans = slower but surer detection. Clamp [2,6]
float HuntTrigPct = 0.5f;      // detection bar: peak-bin field-duty swing (%) a scan must beat to count as a wobble. 12 V value, seeded x fieldScale() and rescaled on a class or winding change (a wobble's duty amplitude scales with winding volts / bus volts). Deliberately UNCAPPED above; 12 V bench episodes land at 0.39-0.62%. Floored at 0.01 because <= 0 makes every scan a detection. The D-attributed relaxed bar tracks this at 0.3x
float cvRecovSec = 2.5f;     // retired timed-window ramp span, inert; NVS key + CSV3 slot kept (never repurpose)
float cvRecovEmaxV = 0.25f;  // retired timed-window error cap, inert; NVS key + CSV3 slot kept (never repurpose)
float cvRecovKiMax = 5.0f;   // refill Ki multiplier at the moment of release (M tapers linearly to 1× as the deficit heals). Clamp [1,10]
// Error-scheduled CV P-boost: whenever the bus is below target in CV (not only after protection), P is scaled up to command
// transient overcurrent out of the voltage hole, tapering to 1× at target so it self-disarms before re-firing OV. P only (cv_I and
// the D trim untouched); independent of cvRecovEnable. Without it the post-cut return is integrator-paced.
uint8_t cvRecovBoostEnable = 1;   // master switch for the recovery P-boost. DEFAULT ON — 3×/1.5V verified live; 4×/0.6V untested — turn off if a faster climb re-trips OV on the approach
float   cvRecovBoostMax    = 4.0f;  // P-gain multiplier at ≥ cvRecovBoostErrV of shortfall; 1.0 = no boost, taper is linear from 1× at target. Clamp [1,8]
float   cvRecovBoostErrV   = 0.6f;  // shortfall (V, per-12V-block — scaled ×SYSTEM_VOLTAGE_CLASS/12 at use) at which the boost reaches cvRecovBoostMax. Clamp [0.1,5]
float   cvRecovBoostFloorV = 0.2f;  // dead area (V, per-12V-block — scaled ×class at use): no boost within this shortfall, ramp starts at 1× from its edge. Keeps boost gain out of small-signal regulation (a 0.7 Hz idle wander was boost-sustained). Clamp [0,5]; 0 = boost from zero shortfall
float   cvRecovDeepBandV = 0.75f;  // deep-recovery band (V, per-12V-block — scaled ×class at use): shortfall beyond this is DEEP — the starve-walk speeds up (cvRecovDeepMult) with a widened pause band, and the OV-episode P-boost ramp slides up to start here instead of going flat-off. Every observed re-fire happened ABOVE target, so depth below this band is not the echo the episode pause guards against (flat-off + 1.5A/s walk held the bus 2V low for 45s). Clamp [0,5]; set large (5) to disable deep mode entirely
float   cvRecovDeepMult  = 5.0f;   // starve-walk rate multiplier at full depth (2× cvRecovDeepBandV of shortfall), ramping linearly from 1× at the band edge. Clamp [1,20]; 1 = walk at the near-target crawl everywhere
float   cvRecovFlareBandV = 0.35f; // arrival flare band (V, per-12V-block — scaled ×class at use): within this shortfall the recovery ceiling tapers from the goal down to cvRecovFlareFrac×goal at target, landing with ~zero surplus instead of the full pre-trip current (re-fires arrived carrying ~7-10A of surplus into a surface-charged battery). Clamp [0,2]; 0 = no flare
float   cvRecovFlareFrac = 0.85f;  // flare ceiling at zero shortfall, fraction of recovCvGoal — the PI re-adds the held-back amps at its own pace after arrival if the load truly needs them. Clamp [0.5,1]; 1 = no flare
bool    g_autoTestActive  = false;  // set per control-loop tick: an automated/guided test owns the limiters now (commissioning / battery-health DCIR / resonance / system-ID). While true the four user limiter toggles above are inert so a stray user setting can't corrupt a measurement; each test's own built-in slew behavior governs.
const float TEST_ENTRY_RATE_A = 8.0f;  // A/s — FIXED, conservative ramp rate for an automated test's transition up from the rest floor and back down (NOT the user's SetpointRiseRate/FallRate, which a user could set aggressively). Used by DCIR, the resonance check, and the TuningMode entry ramp so a test never slams the field coming on/off. Current-domain, so no bus-voltage scaling.
const float CVPF_ENTRY_RATE_A = 50.0f; // A/s — cvpf current-PID phase setpoint rate; the measured edges are direct-duty, so this only paces the practice run
bool    g_tuningEntrySettled = false;  // set per tick from tuningEntryRamped: false while a TuningMode test is still ramping up from rest, true once settled. Gates the sine duty-slew bypass so the entry eases in (duty slew engaged) before the actuator is unclamped for the clean sine.
// Large up-step gentling: a rising setpoint whose remaining gap exceeds SetpointBigStepThresh climbs at SetpointBigStepRiseRate
// until inside the threshold; small corrections keep full SetpointRiseRate.
float SetpointBigStepThresh   = 10.0f;  // A   — gap above which a rising setpoint is gentled
float SetpointBigStepRiseRate = 15.0f;  // A/sec — gentled rise rate used for the large-step portion
float StartupRiseRate  = 10.0f;  // A/sec — setpoint slew rate applied only on field turn-on (OFF/FAULT→AUTO); user-adjustable
bool  inStartupRamp    = false;  // true from field turn-on until setpointLimited catches up to command

// --- Settle Time Before GPIO4 Cut ---
uint32_t SettleTimeBeforeCut = 1000;  // ms - how long duty must be at 0% before GPIO4 goes LOW

// --- Temperature Thresholds (°F above TemperatureLimitF) ---
float TempWarnExcess = 2.0f;             // °F above limit triggers WARNING ramp, starts lockout
float TempCritExcess = 10.0f;            // °F above limit triggers IMMEDIATE GPIO4 cut (skips Phase 1 ramp)
uint32_t TempSustainedTimeout = 120000;  // ms - WARNING temp sustained this long triggers GPIO4 cut (2 min default)

// --- Voltage Thresholds ---
// AlternatorHardShutdownV: absolute battery voltage above which the field is cut instantly (raw per-tick, all modes incl. MANUAL)
// and the adaptive fast-OV lockout starts. Above it sits ONLY the INA228 hardware ALERT (VoltageHardwareLimit, backstop for a hung
// MCU); below it the timed OV tiers and G1/2/3. Guidance: hardware rung 0.2 V × class/12 below the BMS charge-disconnect, this 0.1 ×
// class/12 under it; AGM/flooded get an absolute ceiling sized to DC-load continuous ratings (lead-acid damage is time-integrated).
float AlternatorHardShutdownV = 14.2f;    // V — absolute software instant-cut threshold; in-RAM boot placeholder only. First-boot init in 4_functions.ino seeds it as VoltageHardwareLimit − 0.1 × class/12 (after the hardware rung's own chemistry-aware seed); provisioned devices keep their NVS value.
float VoltageDisagreeThreshold = 0.15f;   // V difference between BatteryV and IBV for disagreement detection
uint32_t VoltageDisagreeTimeout = 10000;  // ms - sustained disagreement this long triggers warning
uint32_t VoltageDisagreeCriticalTimeoutMs = 3000;
// ==================== STATE VARIABLES (global for telemetry) ====================

// --- Hardware State Tracking ---
float lastAppliedDuty = -1.0f;  // Actual duty last sent to hardware (-1 = never set)
                                // Used to initialize previousDutyCycle accurately

// --- Two-Phase Shutdown State ---
uint32_t settledAtZeroDutyMs = 0;  // When duty first reached 0% (0 = not settled yet)
bool gpio4IsLow = false;           // Track actual GPIO4 state for logging/telemetry

// --- Temperature Sustained Warning Timer ---
uint32_t tempWarningStartMs = 0;  // When temp first exceeded limit+TempWarnExcess (0 = not active)

// --- Voltage Disagreement Timer ---
uint32_t voltageDisagreementStart = 0;

// --- Mode Transition Tracking ---
FieldControlMode prevMode = (FieldControlMode)255;  // Previous mode for transition detection

// --- Reporting State ---
FieldControlMode lastReportedMode = (FieldControlMode)255;
FieldEventReason lastReportedReason = (FieldEventReason)255;
uint32_t lastReportMs = 0;

// --- Lockout Transition Tracking ---
bool lockoutWasActive = false;

// Fast OV supervisor instrumentation, unified across G1/2, iExcess and LoadDump: exported after every supervisor has voted
// (bumpless tracker block in AdjustFieldLearnMode).
float g_fastOvCurrentCap = 0.0f;   // live unified cap ceiling this tick (amps)
volatile bool g_fastOvClampActive = false;  // true if ANY protection capped current this tick; written only by the CV body, so every path that skips it calls clearStaleProtClamp() (6_functions.ino)
uint32_t g_fastOvClampCount = 0;   // rising-edge counter across all protections
// CV anti-windup cap not yet recovered after a protection trip: below-target voltage is by-design recovery, not a tracking
// fault. Gates the CV accuracy score.
volatile bool g_cvAwRecovering = false;
// Zero-output stand-down: ~0 A into a bus above target means the excess is battery rest / solar / shore, which a field cut can't
// move, so G1/G2/CV iExcess must not fire. Set at the top of FAST OV (2.5 s entry dwell, single-tick exit); hard layers stay armed.
bool altZeroOutput = false;

float g_I_cap = 0.0f;  // RPM table current ceiling this tick (A); set each AUTO tick
bool g_thermalOwnsCeiling = false;  // the thermal derate, not warmup/HiLo/battery-limit/fastOV/MaxTableValue, set uTargetAmps this tick



// ===== RPM TABLE BREAKPOINTS =====
// 10 breakpoints, denser at the low end (300 keeps a sub-600 RPM idle out of the 0 RPM box), up to 4600. Linear interpolation;
// at or below the first breakpoint (100 RPM) the first entry applies — no extrapolation.
#define RPM_TABLE_SIZE 10
int rpmTableRPMPoints[RPM_TABLE_SIZE] = { 100, 300, 600, 1100, 1500, 2000, 2650, 3300, 3950, 4600 };
// Factory defaults for RPM breakpoints
int defaultRPMValues[RPM_TABLE_SIZE] = { 100, 300, 600, 1100, 1500, 2000, 2650, 3300, 3950, 4600 };

// No "target current" table: Low charge-rate mode is the capTableLo blob (loadCapTablesForMode()); the live ceiling is
// rpmCapCurrentTable. The retired rpmCurrentTable's CSV2/CSV3 slots send literal 0 placeholders.

// ===== CAP CURRENT TABLE =====
// Hard per-RPM ceiling on commanded current, enforced regardless of mode or PID output, to protect belt, shaft and mounts.
// Factory reset restores defaultCapCurrentValues.
float rpmCapCurrentTable[RPM_TABLE_SIZE] = { 0, 100, 100, 100, 100, 100, 100, 100, 100, 100 };
float defaultCapCurrentValues[RPM_TABLE_SIZE] = { 0, 100, 100, 100, 100, 100, 100, 100, 100, 100 };
// Charge health calibration's working copies of the two charge-rate columns (amps, or watts when capLimitMode == 1), filled by
// the chcStart handler so the control tick never opens flash. Here because they need RPM_TABLE_SIZE.
float chcLoTable[RPM_TABLE_SIZE], chcHiTable[RPM_TABLE_SIZE];
// Low charge-rate factory default, as a fraction of the Normal cap table. Seeds capTableLo
// and is the fallback whenever that blob is missing; a user-edited Low table overrides it.
#define LOW_MODE_CAP_FRACTION 0.33f

// ===== CAP POWER TABLE =====
// Alternative cap in watts (active only when capLimitMode=1), converted to amps with live battery voltage.
float rpmCapPowerTable[RPM_TABLE_SIZE] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
// Watts factory default = the amps default at the class's nominal volts, so a kW unit with no stored column (Reset
// Table, the boot auto-reset, a bare Amps/kW toggle) charges like an amps unit instead of to a 0 A ceiling.
static inline float defaultCapPowerW(int i, bool high) {
  return (high ? defaultCapCurrentValues[i] : defaultCapCurrentValues[i] * LOW_MODE_CAP_FRACTION) * (float)SYSTEM_VOLTAGE_CLASS;
}
uint8_t capLimitMode = 0;  // 0 = use amp cap (rpmCapCurrentTable), 1 = use kW cap (rpmCapPowerTable)

// Charge Rate Limits (/get?capBothSave): the handler stages both tables here and capBothPersistService() writes ONE blob per loop
// pass — five writes in the handler froze the dashboard stream. The spinlock covers the ~200 B copies only; loadCapTablesForMode()
// serves the staged copy while a save is pending. Static: 200 B isn't worth a PSRAM allocation plus a null path.
struct CapBothStage {
  int   pts[RPM_TABLE_SIZE];
  float hiA[RPM_TABLE_SIZE], hiW[RPM_TABLE_SIZE], loA[RPM_TABLE_SIZE], loW[RPM_TABLE_SIZE];
};
CapBothStage &capBothStage = PsramPlace::make<CapBothStage>();
portMUX_TYPE capBothMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint8_t capBothPersistStep = 0;   // 0 idle; 1..5 = next blob (rpmPoints, capTable, capPowerTable, capTableLo, capPowerTableLo)
volatile uint8_t capBothStageGen  = 0;     // bumped per staging; a re-stage mid-sequence restarts at blob 1 instead of advancing
volatile bool    capBothPersistFail = false;

// ===== MINIMUM FIELD DUTY TABLE =====
// Minimum field duty (%) per RPM breakpoint, always live: getMinimumFieldForRPM interpolates and hard-floors with MinDuty. A 0 cell
// means no minimum (full shut-off) and, as a HARD zero, drops MinDuty too — so an unmeasured bin carries the baseline; an all-zero
// table silently makes Min Field % inert.
float rpmMinDutyTable[RPM_TABLE_SIZE] = { 0.25, 0.25, 0.25, 0.25, 0.25, 0.25, 0.25, 0.25, 0.25, 0.25 };
float defaultMinDutyValues[RPM_TABLE_SIZE] = { 0.25, 0.25, 0.25, 0.25, 0.25, 0.25, 0.25, 0.25, 0.25, 0.25 };
// Floor for a never-measured bin. NOT class-scaled (unlike MinDuty): immaterial as field current on any bank, and commissioning
// overwrites every bin it measures.
#define KNEE_BASELINE_PCT 0.25f

// ===== AUTO MIN% LEARNING ("knee tracker") =====
// Per RPM bin, learns the duty where output amps begin to build (the knee) and parks rpmMinDutyTable a margin below. A bin probes
// ONCE (kneeStepPct per dwell until output rises kneeOnsetA above a fresh zero), then LOCKS at knee - margin; it re-learns (one
// margin step down) only if > kneeReArmA appears at its floor. Bin 0 is locked at 0%. Floors are RAW duty @ kneeTempRefF;
// getMinimumFieldForRPM applies a copper temp correction (~0.218 %/degF). Battery voltage deliberately NOT used: its two effects
// on the knee cancel. Spec: Working Markdown Docs/MinDuty_Knee_Learning.md.
float kneeFloor[RPM_TABLE_SIZE]      = { 0 };      // learned floor (raw duty% @ kneeTempRefF); owns rpmMinDutyTable when enabled
float kneeKnee[RPM_TABLE_SIZE]       = { 0 };      // detected knee (raw duty% @ kneeTempRefF) = floor + margin
bool  kneeFrozen[RPM_TABLE_SIZE]     = { false };  // true once the knee is found and the floor is locked
float kneeLearnTempF[RPM_TABLE_SIZE] = { 0 };      // alternator case temp (degF) at freeze (diagnostic, not persisted)
uint32_t kneeLastMs[RPM_TABLE_SIZE]  = { 0 };      // millis() of last freeze / re-arm (runtime only)
// User knobs (NVS settings namespace, NK_* keys)
bool  kneeLearnEnable = true;    // master switch: on = learner owns + auto-writes rpmMinDutyTable; off = column is hand-editable. Does NOT gate the floor itself or the commissioning Min% floor step (stage 7).
float kneeMarginPct   = 5.0f;    // park floor this many duty-% below the knee
float kneeOnsetA      = 0.8f;    // amps above the freshly-captured zero = "output building" (knee found)
float kneeReArmA      = 2.0f;    // amps at a locked floor = knee dropped -> lower floor one margin step (deadband guard)
float kneeStepPct     = 0.5f;    // probe staircase increment (duty-% per dwell) while hunting the knee
float kneeDwellSec    = 1.0f;    // settle + hold time per probe step (s)
float kneeTempRefF    = 150.0f;  // reference temp (degF) the stored floors are normalized to (anchored near steady-cruise temp)
bool  kneeTempComp    = true;    // apply the copper temp correction to the floor at apply time
float kneeMaxFloorPct = 30.0f;   // hard clamp on any learned floor; also the "no knee found" terminal value
float kneeRpmTolPct   = 10.0f;   // steady band: RPM, % of reading
float kneeTempTolF    = 4.0f;    // steady band: temperature, degF
float kneeDutyTolPct  = 1.0f;    // steady band: applied duty, duty-% points
// Live diagnostics (for /kneeLearnState)
int   kneeActiveBin   = -1;      // bin currently being observed (-1 = none)
bool  kneeFloorActive = false;   // governor is clamping up to the floor right now
bool  kneeSteadyNow   = false;   // inputs currently within steady bands
// Prototypes (defined in 6_functions.ino) — declared here so setup()/loop() and 3_functions.ino see them.
void loadCapTablesForMode(int mode);   // also called from 2_functions.ino, which concatenates before 3_functions.ino's prototype — declare in the main sketch so the earlier file sees it
// Min%-floor commissioning snapshot helpers (defined in 6_functions.ino) — called from the
// deferred-Start worker/commissionRestore() in 2_functions.ino, so declare them here too.
void commissionBackupMinPct(const float *minDuty, const float *flr, const float *kn, const bool *frz, float fitA);
bool commissionRestoreMinPct();
void commissionClearMinPctBackup();
// Deferred commissioning-start persist worker (defined in 2_functions.ino) — called from loop().
void cxStartPersistService();
void kneeLearnObserve(float rpm, float appliedDuty, float tF, float amps,
                      float dutyRequest, float rpmFloorDuty, bool modeOk);
void kneeLearnInit();
void kneeLearnService(bool fieldOff);
void saveKneeLearnState();
void kneeLearnResetDefaults();
void kneeRebuildOwnedTable();
void kneeAdoptCommittedTable(const float *tbl, const float *knee, float fitA);
String kneeLearnStateJson();
void rpmBreakpointsMoved(const int *prevPts);
void overheatSafeTimeFlush();
void kneeAdoptLearnedState(const float *tbl, const float *flr, const float *knee, const bool *frozen, float fitA);
// Parts of the engine-speed rescale wipe (rpmAxisWipeLocal, 6_functions.ino). An import that changes the scale
// wipes only the parts the file did not bring with it, at the next boot (NK_impBootAct).
#define AXW_KNEE       0x0001   // Min% floor table, knee-learner state and its commissioning backup
#define AXW_HUNT       0x0002   // oscillation-damper speed pockets
#define AXW_HUNTLOG    0x1000   // oscillation-damper episode record (its episodes carry engine speeds)
#define AXW_CXBITS     0x0004   // RPM-binned commissioning stages
#define AXW_SYSID      0x0008   // step-test ring
#define AXW_MOTORFRONT 0x0010
#define AXW_ALTHEALTH  0x0020
#define AXW_OVERHEAT   0x0040
#define AXW_FAMATRIX   0x0080
#define AXW_RIPTAB     0x0100
#define AXW_RIPFIT_ALT 0x0200
#define AXW_RIPFIT_SLP 0x0400
#define AXW_FAFLIP     0x0800
#define AXW_ALL        0x1FFF
#define IBA_PERF       0x2000   // NK_impBootAct: boat-performance Clear All (the import changed perfSpeedSrc)
void rpmAxisWipeLocal(uint16_t parts);
void importStagedCommit();
void importBootActionsLoad();
void importBootActRun(uint16_t act);
void *recordsExportJson(bool summary, size_t *outLen);
size_t recordsExportRead(void *rec, uint8_t *out, size_t maxLen, size_t index);
void recordsExportFree(void *rec);
int recordsImport(const char *body);

// Config Sharing (8_functions.ino) — export/import the cloneable settings set
String manifestConfigObject();
String exportConfigJson();
String exportTablesObject();
int applyImportConfig(const char *body, bool cloudPush);

// Per-unit identity (3_functions.ino). Used by mDNS, /identify and the AP SSID default.
const char *regulatorUid6();
const char *regulatorHostName();
String regulatorDisplayName();
String defaultApSsid();
void mdnsPublishIdentity();
// Access control + multi-regulator (3_functions.ino unless noted)
bool settingsArmActive(AsyncWebServerRequest *request);   // 5_functions.ino
bool gxFlagged(AsyncWebServerRequest *request);
void gxDevicesHandle(AsyncWebServerRequest *request);   // POST /gxDevices, 4_functions.ino
void gxDevicesForget();                                 // 3_functions.ino, at a demo switch
bool gxParamDenied(AsyncWebServerRequest *request);
void startGxSendTask();
bool armTokenValid(AsyncWebServerRequest *request);
const char *armTokenIssue();
void armTokensClear();
bool pwVerify(const char *pw);
bool pwStore(const char *pw);
void pwClear();
void accessInit();
void accessRecoveryClear();
bool pairListAdd(const char *uid, const char *name);
bool pairListRemove(const char *uid);
void pairListAppendJson(String &out);
void peerListAppendJson(String &out);
void startPeerBrowseTask();
static void cfgAppendJsonStr(String &out, const String &val);   // defined in 8_functions.ino, used earlier by /identify
int doCloudPOST(const char *endpointPath, const char *payload, char *responseBuf, size_t responseBufSize);  // 3_functions.ino — lean raw-TLS POST

// Admin config push (4_functions.ino) — boot-time pull of a cloud-queued config
void executeGetPendingConfig();
void executeClearPendingConfig();
String pendingConfigClearId = "";   // id passed from get → clear within one boot check

unsigned long lastOverheatTime[RPM_TABLE_SIZE] = { 0 };          // Timestamp of last overheat per RPM
int overheatCount[RPM_TABLE_SIZE] = { 0 };                       // Total overheat events per RPM
unsigned long cumulativeNoOverheatTime[RPM_TABLE_SIZE] = { 0 };  // Safe operation time per RPM
uint32_t learningUpCount[RPM_TABLE_SIZE] = { 0 };

// ===== LEARNING STATE VARIABLES =====
uint32_t overheatPenaltyEndMs = 0;           // as used by AdjustFieldLearning Mode
unsigned long overheatingPenaltyTimer = 0;   //Same as above, but used in interfaces (legacy)
float overheatingPenaltyAmps = 0.0;          // Current penalty amount (A)
int currentRPMTableIndex = -1;               // Which table entry we're in (-1 = invalid)
unsigned long learningSessionStartTime = 0;  // Session start time for current RPM
unsigned long lastTableUpdateTime = 0;       // Last learning update timestamp

// ===== PID CONTROLLER VARIABLES =====
double pidInput = 0.0;                                                             // Current measured amps
double pidOutput = 0.0;                                                            // PID duty cycle output
double pidSetpoint = 0.0;                                                          // Current target amps
PID currentPID(&pidInput, &pidOutput, &pidSetpoint, PidKp, PidKi, PidKd, DIRECT);  // PID object
bool pidInitialized = false;                                                       // PID initialization status

// ===== PID Term Contributions (telemetry/visualization) =====
// Output current loop (currentPID) — units: duty cycle %
// P is exact. D is from error derivative. I is residual (output - P - D).
float innerTermP = 0.0f;
float innerTermI = 0.0f;
float innerTermD = 0.0f;

// Temperature loop diagnostics (amps), logging only: outerTermP + outerTermLookahead + outerTermI == the accumulated penalty,
// recomputed every tempPID_tick() (6_functions.ino).
float outerTermP = 0.0f;
float outerTermI = 0.0f;
float outerTermLookahead = 0.0f;   // look-ahead share of outerTermP: Kp × max(0, projected − present temp), penalty-signed. No D term: Kd is hardwired 0; derivative action lives in the projected input
float thermalSlopeFPerSec = 0.0f;  // long-window slope estimate (°F/sec)

volatile bool tempPIDResetRequested = false;
volatile bool innerPIDResetRequested = false;
volatile bool cvLoopResetRequested = false;

float tempFiltered = NAN;
float thermalPenaltyLastValid = 0.0f;
// Slope ring buffer: THERMAL_SLOPE_BUF readings × 5s = 60s window (12 intervals)
#define THERMAL_SLOPE_BUF 13
float thermalSlopeBuffer[THERMAL_SLOPE_BUF];
uint8_t thermalSlopeBufIdx = 0;
bool thermalSlopeBufFull = false;
float projectedTempF = NAN;           // tempNow + slopeF_per_sec × ThermalLookaheadSec — PID process variable
uint32_t thermalSlopeLastPushMs = 0;  // gates slope buffer push to TempPIDIntervalMs cadence
// One-shot: a re-enable after a DORMANT-but-not-stale period (TuningMode exit) PRESERVES the slope buffer, so the setpoint stays at
// limit-7 instead of a spurious limit-20 warmup that derates the field for 60 s after a test. Not set on a true sensor gap (a stale
// trend must clear). Consumed in the re-enable path of tempPID_tick().
bool thermalPreserveSlopeOnResume = false;

float cvDSlope = 0.0f;              // V/s — sliding-window backward diff of g_cvKdFiltV (~VoltageLoopInterval window, refreshed every output tick); the CV D term's input and the recovery starve exit's
uint32_t g_cvKdCount = 0;           // session count of D-term engagement episodes; lifetime twin lives in g_ovTel.kdEventCount

float outerImpliedPenalty = 0.0f;
bool outerAntiWindupFired = false;
// Sticky version of outerAntiWindupFired: set on any CV-bleed event, cleared when CSV2 captures it.
// outerAntiWindupFired resets every tempPID_tick (~75 Hz) so the 5s CSV2 frame would otherwise miss it.
volatile bool thermalAntiWindupLatch = false;

// ===== CALCULATED VALUES FOR DISPLAY =====
float learningTargetFromRPM = -1.0;  // Table lookup result before corrections
float finalLearningTarget = 0.0;     // After all corrections applied
float ambientTemp = NAN;             // Current ambient temperature (°F)
float baroPressure = NAN;            // bmp380

// ===== LEARNING DIAGNOSTICS =====
unsigned long totalLearningEvents = 0;    // Total learning updates performed
unsigned long totalOverheats = 0;         // Sum of all overheatCount[]
uint64_t totalSafeMs = 0;                 //   used in AdjustFieldLearningMode
float totalSafeHours;                     //  same as above, but for displays (legacy)
uint64_t timeSinceLastOverheat = 0;  // Global time since any overheat


// ===== FIELD SHUTDOWN STATE MACHINE =====
enum ShutdownPhase {
  SHUTDOWN_PHASE_NONE = 0,
  SHUTDOWN_PHASE_1 = 1,
  SHUTDOWN_PHASE_3 = 3,
  SHUTDOWN_PHASE_4 = 4
};
ShutdownPhase shutdownPhase = SHUTDOWN_PHASE_NONE;
uint32_t shutdownPhaseEntryMs = 0;
uint32_t shutdownPhase2EntryMs = 0;
// What the displays call Shutdown: the ramp is under way and GPIO4 not yet cut. After the cut the machine parks at phase 4
// until the next field-on, and that is Off (CSV1 shutdownPhase reads 0, flags bit 5 clear).
inline bool shutdownRampActive() { return shutdownPhase != SHUTDOWN_PHASE_NONE && !gpio4IsLow; }
// Tunable parameters (expose in web UI)
float DutySlowRampRate = 0.5f;      // %/sec - Phase 3 tail: the slowest the proportional decay may go, so the ramp reaches 0
float ShutdownRampTauS = 4.0f;      // s - Phase 3 proportional decay time constant; ~2x the LM2907 coupling re-centering (20uF x 104.7k = 2.1 s) so the input tracks the shrinking stator swing. 0 = tail rate only
float NoOutputAmps = 2.0f;          // A - alternator current at or below this counts as no output (shutdown output-while-off shed, tach-lie)
#define OUTPUT_WHILE_OFF_DWELL_MS 300  // alternator amps above NoOutputAmps must hold this long during a shutdown ramp before the field sheds at DutyRampRate
uint32_t ShutdownPhase2HoldMs = 500;  // ms - hold at rpmMinDuty before slow ramp (0 = skip)

// ===== TEMPERATURE LOOP PID =====
// Tuning — all web UI configurable
float TempPIDKp = 3.0f;             // A/°F proportional gain — validated tune: lower gain blows through setpoint and trips the warn ramp without saturating its cut authority (under-gain, not over-authority).
float TempPIDKi = 0.1f;             // A/(°F·s) integral gain — must wind the full steady-state penalty alone (P contributes nothing at zero error).
float TempPIDKiDownFrac = 0.33f;   // asymmetric integral bleed: below setpoint (eI<0) the bleed uses TempPIDKi×this instead of TempPIDKi, so a transient sub-setpoint undershoot does NOT collapse the learned holding penalty. Ratio (not absolute) so it auto-scales with any Ki. 1.0 = symmetric; lower = slower release. Clamped [0,1].
float ThermalLookaheadSec = 60.0f;  // projection horizon (s), clamped [0,300]; longer horizons over-extrapolate the coarse slope and drive a limit cycle
float ThermalSlopeWindowSec = 25.0f;  // slope backward-difference window (s), clamped [10,60]; shorter = less lag, noisier slope — below ~25 the ±0.2/0.3°F/s sensor-quantization steps show through

float ThermalPenaltyRiseRate = 60.0f;  // A/s — how fast penalty can increase (restrict current)
float ThermalPenaltyFallRate = 20.0f;  // A/s — how fast penalty can decrease (allow more current)

float WarmupRampRate = 10.0f;     // A/s — rate at which output ceiling rises from 0 on field enable; 0 = disabled
float warmupCeiling = 0.0f;       // runtime warmup ceiling (not persisted)
float prevThermalPenalty = 0.0f;  // last applied penalty (slew "prev" + stale-hold seed). penalty = FF + thermalIntegral, clamped to the live cap — see tempPID_tick().
float thermalIntegral = 0.0f;     // HYBRID: holding-level integral (amps). P + projection is POSITIONAL feedforward (FF = Kp·max(0,max(proj,present)−sp)); only this integral accumulates, with the live-cap clamp (I ≤ cap−FF), asymmetric below-setpoint bleed, and approach/descent gates. Keep P+projection positional: a velocity form zeroes the projection's constant lead.
float thermalIntegralCeil = 0.0f; // instrumentation: live integral ceiling cap−FF (amps), thermal log iCeil_A. Below outerI, the I≤cap−FF clamp is DELETING earned holding integral (RPM dip / FF spike). Diagnostic only.

uint32_t TempPIDIntervalMs = 5000;  // Temperature loop update period (ms) — independent of output current loop and sensor rate
float TempPIDFilterAlpha = 0.2f;    // IIR smoothing for DS18B20 (0=frozen, 1=raw); feeds every loop pass (~300 Hz) on a frozen 5s sample
// Thermistor filter alpha is hardcoded 0.02f in the tempPID_tick IIR filter — not user-configurable
// Runtime state — expose via telemetry
double tempPIDInput_d = 77.0;        // PID process variable (°F) = max(projected, present) — projected = filtered + slope × lookahead
double tempPIDSetpoint_d = 0.0;      // effectiveSetpoint: limit −7°F (slope buffer full) or −20°F blind warmup
bool tempPIDActive = false;          // true when tempPID is in the PID library's AUTOMATIC mode (loop closed)
bool tempFilterNeedsReseed = false;  // Set true to force IIR cold-start on next tempFilterUpdate()
bool thermalIntegratorReleased = false;  // false until PRESENT temp first reaches the regulation setpoint; while false the up-driving dI is held off (P + projection dP alone handle the approach — prevents approach windup overshoot)
float outerPenaltyRaw = 0.0f;        // Tier-0a: unclamped requested penalty (prevThermalPenalty + dP + dI) before clamp + slew — for the requested-vs-applied saturation diagnostic
uint8_t thermalFreezeReason = 0;     // Tier-0a: which integrator-freeze case gated this tick (see ThermalLogEntry.freezeWhy enum)
uint32_t tempInvalidSinceMs = 0;     // millis() when TempToUse first went invalid (0 = currently valid). Debounces the tempPID deactivation so a single bad sample can't force a buffer-clear warmup re-init (the spurious "140°F setpoint" artifact).

float thermalPenaltyAmps = 0.0f;    // temperature loop output: amps subtracted from target table

// The thermal loop has no PID library object: penalty accumulates in prevThermalPenalty (tempPID_tick()); tempPIDInput_d /
// tempPIDSetpoint_d are CSV2 telemetry only. currentPID is the separate field/output-current loop.




//=============================================================================
//                    END LEARNING MODE GLOBAL VARIABLES
//=============================================================================
// ===== TEMP-FIELDCHECK BEGIN: TEMPORARY field-current validation log. REMOVE after the owner's validation drive: delete every
// block tagged TEMP-FIELDCHECK (grep -rn TEMP-FIELDCHECK: Xregulator.ino, 3_functions.ino, 6_functions.ino, web_src/index.html,
// web_src/script.js). Plan: d5_thermal_model/README.md, "FIELD DERATE - SPEC", "Validation drive". A row every 10 s plus each meter
// reading the owner types in Live Data > Alternator, so a moment the estimate (iiout) read below the meter can be found later.
// Disabled 2026-10-13 by commenting out every TEMP-FIELDCHECK block, not deleting them; uncomment all of them together to restore.
// #define FC_ROW_MS     10000   // one row per 10 s
// #define FC_SAMPLE_MS  100     // averages are fed at 10 Hz
// #define FC_SEC_N      6       // 1 s buckets behind an owner entry: the last 5 whole seconds plus the current one (the meter is slow)
// #define FC_RING_N     512     // 360 rows = 1 h, plus room for entries
// #define FC_KIND_ROW   0
// #define FC_KIND_METER 1
// #define FC_KIND_MARK  2
// struct FcStat {
//   uint16_t n; float s, mn, mx;
//   void clear() { n = 0; s = 0.0f; mn = INFINITY; mx = -INFINITY; }
//   void add(float v) { if (!isfinite(v)) return; n++; s += v; if (v < mn) mn = v; if (v > mx) mx = v; }
//   void merge(const FcStat &o) { if (!o.n) return; n += o.n; s += o.s; if (o.mn < mn) mn = o.mn; if (o.mx > mx) mx = o.mx; }
//   float avg() const { return n ? s / n : NAN; }
//   float lo() const { return n ? mn : NAN; }
//   float hi() const { return n ? mx : NAN; }
// };
// struct FcAcc {
//   FcStat est, duty, volts, rpm, altA, battA;
//   void clear() { est.clear(); duty.clear(); volts.clear(); rpm.clear(); altA.clear(); battA.clear(); }
//   void merge(const FcAcc &o) { est.merge(o.est); duty.merge(o.duty); volts.merge(o.volts); rpm.merge(o.rpm); altA.merge(o.altA); battA.merge(o.battA); }
// };
// struct FcRec {
//   uint32_t ms;
//   uint8_t kind, mode, lim, bind, stage, fieldOn, frtRun, pad;
//   float meterA;
//   float estAvg, estMin, estMax, dutyAvg, dutyMin, dutyMax, vAvg, vMin, vMax;
//   float rc, r, trefC, probeF, dieF, rpm, altA, battA, curCeil, heatCeil;
// };
// struct FcStore {
//   FcRec ring[FC_RING_N];
//   FcRec last;                            // the latest entry, for the panel
//   FcAcc row, sec[FC_SEC_N];              // Core 1 only
// };
// FcStore *fc = nullptr;                   // one PSRAM block; ring, last and the fields below are guarded by fcMux (written on Core 1, read by the web task)
// uint16_t fcHead = 0, fcCount = 0, fcEntries = 0;
// portMUX_TYPE fcMux = portMUX_INITIALIZER_UNLOCKED;
// uint32_t fcReqSeq = 0, fcDoneSeq = 0;    // the web handler bumps fcReqSeq; the control tick records the entry and catches fcDoneSeq up
// uint8_t fcReqKind = FC_KIND_MARK;
// float fcReqAmps = NAN;
// ===== TEMP-FIELDCHECK END =====
// ===== THERMAL LOG (PSRAM circular buffer) =====
#define THERMAL_LOG_SIZE 3600  // 2 h at 0.5 Hz; larger starves cvLog's PSRAM
#define THERMAL_LOG_INTERVAL_MS 2000

volatile bool thermalLogPaused = false;
volatile uint32_t thermalLogPausedAtMs = 0;
#define THERMAL_LOG_PAUSE_TIMEOUT_MS 30000



// Thermal-log scaling helpers — defined in 7_functions.ino (used by the 6_functions thermal logger).
static int16_t thermalLogScale10(float v);
static int16_t thermalLogScaleRPM(float v);
static uint8_t thermalLogGetStageCode();

struct ThermalBinDLState {
  uint8_t header[8];  // count(uint32) + intervalMs(uint32), little-endian
  int headerPos;
  int count;
  int oldest;
  int row;
  bool done;
  uint8_t entryBuf[64];  // headroom for ThermalLogEntry (52B after Tier-0a fields); actual copy uses sizeof()
  int entryLen;
  int entryPos;
};


struct ThermalLogEntry {
  uint32_t ts;

  int16_t tempFiltered;   // IIR-filtered sensor reading (no raw column — filtered is sufficient for PID tuning)
  int16_t tempProjected;  // projectedTempF = filtered + slope × lookahead (what PID sees)
  int16_t nominalTarget;
  int16_t rpmCap;
  int16_t voltCap;
  int16_t uTarget;
  int16_t spLimited;
  int16_t pidErr;
  int16_t pidOut;
  int16_t duty;
  int16_t rpm;
  int16_t battV;
  int16_t measAmps;
  int16_t penaltyAmps;

  uint8_t flags;
  uint8_t antiWindupFired;
  uint8_t chargeStageDisplay;
  uint8_t freezeWhy;  // which dI-accumulation gate applied this row: 0=dI applied (integral winding),1=approach (dI held below setpoint),2=saturation (integral clamped because FF+I hit the live cap),3=descent (above setpoint+cooling, dI held); 4/5 unused. Priority: approach>saturation>descent.

  int16_t outerTermP;
  int16_t outerTermI;
  int16_t outerTermLookahead;  // CSV column "lookahead"
  int16_t impliedPenalty;
  int16_t thermalSlope;  // thermalSlopeFPerSec × 1000 (0.001 °F/sec per count)
  int16_t penaltyRaw;    // Tier-0a: unclamped requested penalty (prevThermalPenalty + dP + dI), ×10. vs penaltyAmps = applied after clamp+slew. Gap to rpmCap reveals unused derate authority.
  int16_t holdEstimate;  // despite the name: live integral ceiling cap−FF, ×10 (CSV column iCeil_A). iCeil < outerI ⇒ the I≤cap−FF clamp is deleting earned holding integral.
  // gainKp/Ki/Lookahead written once in pidlog CONST row
};

struct ThermalDLState {
  int count;
  int oldest;
  int row;
  bool done;
  char line[500];
  int lineLen;
  int linePos;
};


ThermalLogEntry *thermalLog = nullptr;
int thermalLogHead = 0;
int thermalLogCount = 0;
bool thermalLogReady = false;
uint32_t thermalLogBurstUntilMs = 0;

#define PID_LOG_SIZE 2400
#define CV_LOG_SIZE 6000
#define CV_LOG_HEADER_SIZE 68

volatile bool pidLogPaused = false;
volatile uint32_t pidLogPausedAtMs = 0;

// Download-abort pause release: Safari drops the connection without a clean close, leaving xxxLogPaused stuck until the 60 s
// watchdog. Each handler bumps its token and clears the pause in onDisconnect only on token match, so a late-closing EARLIER
// download can't unfreeze a newer one.
volatile uint32_t thermalLogDlToken = 0;
volatile uint32_t pidLogDlToken = 0;
volatile uint32_t cvLogDlToken = 0;

struct PidLogEntry {
  // ── Timestamp ────────────────────────────────────────────────────
  uint32_t ts;  // millis() at log point

  // ── Mode / Stage ─────────────────────────────────────────────────
  uint8_t chargeStageDisplay;  // getChargeStageDisplayCode() enum value
  uint8_t TargetVoltageMode;   // runtime TargetVoltageMode flag (0 or 1)
  uint8_t flags;               // bit0=AUTO bit1=voltCtrl bit4=govBypass
  uint8_t ovFlags;             // bit0=fastOvActive bit1=iExcessBulk bit2=hardClamp bit3=iExcess bit4=loadDumpActive

  // ── CV loop ──────────────────────────────────────────────────────
  float battV;                  // tick.currentBatteryVoltage
  float ChargingVoltageTarget;  // target voltage this tick
  float vError;                 // ChargingVoltageTarget - battV (always fresh)
  float Icv;                    // CV position-form PI output — direct current setpoint (A)
  float cv_I;                   // CV position-form PI integrator state
  float tableThermalLimit;      // uTargetAmps before CV — RPM cap minus thermal penalty
  float setpointCmd;            // value fed to setpointCommand (Icv in CV, tableThermalLimit in bulk)

  // ── Voltage loop event flags ─────────────────────────────────────
  uint8_t voltageLoopRanThisTick;     // 1 only when interval/enteringCV fired
  uint8_t enteringCV;                 // 1 on first CV tick (transition only)
  uint8_t enteringTargetVoltageMode;  // 1 on mode entry tick only
  uint8_t pad1;

  // ── Output current PID ───────────────────────────────────────────
  float pidSetpoint;     // setpointLimited — slew-filtered command to PID (Maintain/zero-float: lower when the alternator ceiling binds)
  float pidInput;        // measured current fed to PID
  float pidUnsatOutput;  // pre-clamp PID output (saturation detector)
  float pidOutput;       // clamped PID output → duty request
  float innerTermP;      // P contribution this tick
  float innerTermI;      // I contribution this tick
  float innerTermD;      // D contribution this tick

  // ── Duty pipeline ────────────────────────────────────────────────
  float dutyRequest;  // value sent into governor_apply()
  float dutyApplied;  // value returned by governor_apply()

  // ── Context ──────────────────────────────────────────────────────
  float rpm;
  float measAmps;
  float innerKp;    // PidKp  — inner output-current PID gain (NOT voltage loop Kp)
  float innerKi;    // PidKi  — inner output-current PID gain (NOT voltage loop Ki)
  float innerKd;    // PidKd  — inner output-current PID gain (NOT voltage loop Kd)
  float voltageKp;  // VoltageKp — outer voltage loop proportional gain (A/V)
  float voltageKi;  // VoltageKi — outer voltage loop integral gain (A/(V·s))
  // ── Filtered signals ─────────────────────────────────────────────
  float battV_filt;  // IBV
  // ── Protection flags & signals ───────────────────────────────────────────
  float dBcur_dt;    // g_dBcur_dt (A/s) — battery current derivative for load dump
  float battI;       // getBatteryCurrent() (A) — INA228 or Victron
  // ── Timing diagnostics ───────────────────────────────────────────────────
  int16_t ch1IntervalMs;       // g_ch1LastIntervalMs — CH1 inter-sample gap this tick (ms)
  int16_t voltLoopIntervalMs;  // actual voltage loop interval when fired this tick (ms); 0 if not fired or first fire after a stop
  int16_t inaIntervalMs;       // ina_last_ms — INA228 read freshness (ms)
  int16_t pad2;                // alignment pad
  // ── iExcess (Group 3 alternator) detector tuning traces ─────────────────────────────
  float mExcessEma;            // g_mExcessEma — time-averaged signed current excess over command (A)
  float iExcessThreshold;      // g_iExcessThreshold — computed fire threshold E (A)
};                   // 140 bytes — naturally aligned, no implicit holes

struct PidDLState {
  int count;
  int oldest;
  int row;
  bool done;
  char line[1024];  // MUST hold the largest row intact (comment block 715 B, header ≈480 B): snprintf truncation eats the trailing
                    // '\n' and glues rows together. Grow it whenever a column or comment line is added.
  int lineLen;
  int linePos;
};


PidLogEntry *pidLog = nullptr;
int pidLogHead = 0;
int pidLogCount = 0;
bool pidLogReady = false;


// ── pidLog capture globals ──────────────────────────────────────────────────
// Written during AdjustFieldLearnMode(), read by pidLog_tick().
// Reset to zero at the top of each gated control tick.
float pidLog_vError = 0.0f;
float pidLog_uTargetBeforeVoltCap = 0.0f;
float pidLog_uTargetAfterVoltCap = 0.0f;
float pidLog_dutyRequest = 0.0f;
float pidLog_dutyApplied = 0.0f;
uint8_t pidLog_voltageLoopRanThisTick = 0;
uint8_t pidLog_enteringCV = 0;
uint8_t pidLog_enteringTargetVoltageMode = 0;



// CV / Voltage Tuner Log: every fresh CH1 sample (~13 ms, up to ~30 ms), no rate limiter; /cvlog.bin decoded to CSV by JS.
// 360 KB PSRAM ≈ 28 s at full rate.

// Field order/offsets must stay in lockstep with parseCvBin() in script.js.
// Unnamed scales: battV/targV/vPred ×100, vErrorMv ×1000, fastOvCap/uTarget/spLimited/iMeas/duty ×10.
struct __attribute__((packed)) CvLogEntry {
  uint32_t ts;              // millis()
  int16_t battV;            // IBV (raw bus)
  int16_t targV;            // ChargingVoltageTarget
  int16_t vErrorMv;         // targV − battV; NOT the PI error (that is targSlewed − battV)
  int16_t dvdt_x1000;
  int16_t vPred;            // IBV + TdPred × max(0,dvdt)
  int16_t fastOvCap;
  int16_t cv_I_x10;
  int16_t Icv_x10;          // CV PI output = current setpoint into the output loop
  int16_t uTarget;          // uTargetAmps (table+thermal+user ceiling)
  int16_t spLimited;        // setpointLimited (slewed command to PID)
  int16_t iMeas;
  int16_t duty;
  uint8_t flags;   // b0=fastOvActive b1=voltLoopFired b2=cvActive b3=iExcessBulk b4=hardClamp b5=iExcess b6=loadDumpActive b7=recovActive
  uint8_t awState; // 0=normal 1=frozen(supervisor/D/slew) 2=saturated 3=bleeding 4=bumpless 5=target wind-down
  int16_t rpm;
  int16_t battV_filt_x100;  // IBV_filtered × 100 (V) — display EMA, VoltageFilterTC
  int16_t ch1IntervalMs;    // last CH1 inter-sample gap   (ms)
  int16_t cvDSlope_x10000;  // cvDSlope × 10000 — sliding-window backward diff of g_cvKdFiltV (V/s); the D-term input
  int16_t battI_x10;        // getBatteryCurrent() × 10 (A) — INA228 or Victron
  int16_t dBcur_dt_Aps;       // g_dBcur_dt clamped to int16 (A/s) — load dump derivative
  int16_t voltLoopIntervalMs;      // actual voltage loop interval when fired this tick (ms); 0 if not fired or first fire after a stop
  int16_t inaIntervalMs;           // ina_last_ms at log time — INA228 read freshness (ms)
  int16_t kdTrim_x1000;            // D-term current reduction applied at the Icv output (A × 1000); most recent nonzero trim since the last log write, 0 if none; signed (negative only if CvKdOneSided off)
  uint8_t capReason;               // which layer set fastOvCap this tick: 0=none 1=KHard_G1 2=KHard_G2 3=iExcess 4=loadDump 5=iExcessBulk
  int16_t ovFilt_x100;             // g_ovIbvFilt × 100 (V) — Group 2's comparator input (plant-tau EMA of IBV)
  int16_t targSlewed_x100;         // voltageTargetSlewed × 100 (V) — rise-governor output; the setpoint the CV PI tracks
  int16_t pTerm_x10;               // g_cvPTerm × 10 (A) — P contribution to Icv (VoltageKp_active × error); I=cv_I_x10, D=kdTrim_x1000
  int16_t cvKdFiltV_x100;          // g_cvKdFiltV × 100 (V) — IBV smoothed by CvKdVoltFiltTC; the D term's slope input ("Voltage for D term")
  uint8_t huntDerate_pct;          // g_huntDerate × 100 (25..100; 100 = full inner Ki). Byte-sized on purpose: the damper's whole range fits, and cvlog is 6000 entries deep
  uint8_t huntFreq_x100;           // g_huntFreqHz × 100 — the six detector bins land on 31..234, so a byte is exact; 0 = no wobble confirmed this session
  uint8_t huntState;               // 0 watching, 1 damping episode, 2 verified/recovering, 3 standing down
};
static_assert(sizeof(CvLogEntry) == 60, "CvLogEntry must be 60 bytes");


struct CvBinDLState {
  uint8_t header[CV_LOG_HEADER_SIZE];
  int headerPos;
  uint32_t count;
  uint32_t oldest;
  uint32_t row;
  bool done;
  uint8_t entryBuf[sizeof(CvLogEntry)];
  int entryLen;
  int entryPos;
};

// 68-byte header carries the full D-term trim equation, so kdTrim is reconstructible from
// cvDSlope + spLimited alone. Layout is written (offset by offset) in the /cvlog.bin handler
// in 3_functions.ino; all fields after entrySize are float, including the 0/1 flags.

static CvLogEntry *cvLog = nullptr;
bool loggingActive = true;  // Stop/Start Logs: false freezes thermal/PID/CV ring buffers
static int cvLogHead = 0;
static int cvLogCount = 0;
static bool cvLogReady = false;
static bool cvLogPaused = false;
static uint32_t cvLogPausedAtMs = 0;


float g_fastOvDvdt = 0.0f;        // filtered dV/dt (V/s), updated every IBV fresh tick
float g_dBcur_dt = 0.0f;          // dBcur/dt (A/s), updated every INA228 read; positive = load dump
float g_fastOvVpred = 0.0f;       // predicted voltage, updated when voltageControlActive
float g_ovIbvFilt = 0.0f;         // Group 2 comparator input: plant-tau-derived EMA of IBV (see fast-OV block)
bool g_cvRecovActive = false;     // post-protection integrator-refill state, exported for cvLog flags b7
bool g_fastOvHardActive = false;  // K_HARD or hysteresis block fired this tick

// CV P/I/D decomposition telemetry (CSV1 + cvlog). g_cvKdFiltV is the CvKdVoltFiltTC EMA of IBV the D term differentiates;
// g_cvKdTrimLive persists every tick (0 idle), unlike g_kdTrimThisTick, which cvLog_tick clears after each write.
float g_cvKdFiltV = 0.0f;         // V — IBV smoothed by CvKdVoltFiltTC; the D term's slope input
float g_cvPTerm = 0.0f;           // A — proportional contribution to Icv (VoltageKp_active × error)
float g_cvKdTrimLive = 0.0f;      // A — D-term back-off applied at the Icv output this tick (0 when not trimming)
uint32_t g_fastOvHardCount = 0;

// ---- Gate-tuning live readouts: 10s rolling extreme per gated quantity (PSRAM) ----
// Each gate's live quantity, extreme over the last 10 s (ten 1-s buckets, robust to update gaps), so a threshold can be set
// relative to it. Written by sensor/control tasks, read lock-free by the web task: a torn read only blurs a display. Peak for
// trip-type gates, trough for the RPM edge-margin pass-gate.
enum { ROLL_RPMEDGE = 0, ROLL_AMPSDRIFT, ROLL_AMPSDRIFTEXC, ROLL_TONEPK, ROLL_LDSLEW, ROLL_CVSLOPE,
       // Ripple-capture (faFiltRippleUpdate) admission gates (§11): (window quantity − effective limit), peak over 10s, <=0 =
       // passing — command travel, alt/batt half-window mean-shift, RPM half-window mean-shift.
       ROLL_RIPCMDEXC, ROLL_RIPALTEXC, ROLL_RIPBATTEXC, ROLL_RIPRPMSHIFT, ROLL_COUNT };
#define ROLL_EMPTY (-2000000000)   // CSV sentinel: no sample in the 10s window; also SafeInt's NaN/Inf return — the one "not available" value on every CSV stream
struct Roll10s {
  float v[10];
  uint32_t sec[10];
  bool isMin;        // true = track minimum (trough); false = maximum (peak)
};
Roll10s *gRoll = nullptr;   // ps_malloc'd [ROLL_COUNT] in setup()

void rollUpdate(int idx, float x) {
  if (!gRoll || idx < 0 || idx >= ROLL_COUNT || isnan(x)) return;
  Roll10s &r = gRoll[idx];
  uint32_t nowSec = millis() / 1000;
  int b = nowSec % 10;
  if (r.sec[b] != nowSec) { r.sec[b] = nowSec; r.v[b] = x; }   // entered a new second → reset bucket
  else r.v[b] = r.isMin ? fminf(r.v[b], x) : fmaxf(r.v[b], x);
}

float rollRead(int idx) {   // NAN when no sample in the last 10s
  if (!gRoll || idx < 0 || idx >= ROLL_COUNT) return NAN;
  Roll10s &r = gRoll[idx];
  uint32_t nowSec = millis() / 1000;
  float ext = NAN;
  for (int i = 0; i < 10; i++) {
    if (r.sec[i] == 0 || (uint32_t)(nowSec - r.sec[i]) >= 10) continue;   // sec==0 = unwritten (first boot second only)
    if (isnan(ext)) ext = r.v[i];
    else ext = r.isMin ? fminf(ext, r.v[i]) : fmaxf(ext, r.v[i]);
  }
  return ext;
}

int rollCsv(int idx, int scale) {   // CSV2 emitter — ROLL_EMPTY sentinel when no data this window
  float x = rollRead(idx);
  return isnan(x) ? ROLL_EMPTY : (int)lroundf(x * scale);
}

// BINDING protection layer (lowest cap) that set fastOvCurrentCap this tick, so the CV log can tell "KHard fired" from "KHard
// fired but iExcess/loadDump was the real limit".
enum FastOvCapReason : uint8_t {
  CAP_REASON_NONE     = 0,  // cap at fastOvBaseCap — no protection bound
  CAP_REASON_KHARD_G1 = 1,  // Group 1 predictive KHard (Vpred)
  CAP_REASON_KHARD_G2 = 2,  // Group 2 measured KHard, rising-gated shed (filtered IBV)
  CAP_REASON_IEXCESS  = 3,  // iExcess supervisor (near-target / CV regime)
  CAP_REASON_LOADDUMP = 4,  // load-dump cutoff
  CAP_REASON_IEXCESS_BULK = 5,  // iExcess BULK sub-mode (current-control phase, vs ceiling)
};
uint8_t g_fastOvCapReason = CAP_REASON_NONE;  // exported once per full control tick alongside g_fastOvCurrentCap (see capReasonTick in AdjustFieldLearnMode)

// Plots-tab event markers: the control loop ORs a bit per single-tick event; the ~10 Hz CSV1 sender reads and clears the bits it
// consumed, so events between sends are kept. Visual only.
#define PROT_EVT_OV 0x01  // overvoltage hard clamp (KHard G1/G2)
#define PROT_EVT_IX 0x02  // iExcess current-excess supervisor (CV or Bulk)
#define PROT_EVT_LD 0x04  // load-dump cutoff
// 0x08 is NOT a protection (the D term only trimmed the setpoint): the dashboard masks 0x01|0x02|0x04 for red protection markers
// and shades 0x08 apart; any reader of this mask must do the same.
#define PROT_EVT_KD 0x08  // CV D term applied a current-reduction trim this tick
volatile uint8_t g_protEventLatch = 0;

// ── Current ring / MA / dI/dt ─────────────────────────────────────────────
// Written in ADS case 1, read by AdjustFieldLearnMode and cvLog_tick
#define I_RING_SIZE 10
struct IAmpEntry {
  uint32_t ts;
  float val;
};
static IAmpEntry iAmpRing[I_RING_SIZE];
static uint8_t iAmpHead = 0;
static uint8_t iAmpCount = 0;

float g_pidMA_N = 0.0f;   // MA(N) where N = OutputPIDMA_N (Output Current PID signal)
uint16_t g_ch1LastIntervalMs = 0;  // last CH1 inter-sample gap, for cvLog

bool g_iExcessActive = false;      // CV iExcess LATCHED this tick
bool g_iExcessBulkActive = false;  // Group 3 BULK sub-mode active this tick (current-control phase) — logged distinctly from CV iExcess
float g_iExcessDutyCap = 100.0f;
float g_mExcessEma = 0.0f;    // averaged signed current excess over command (A) — tuning trace; whichever detector gate is open writes it
float g_iExcessThreshold = 0.0f;  // the computed fire threshold E (A) for the active detector — tuning trace
// Per-CSV1-frame aggregates for the live sparkline (the loop runs far faster than ~10 Hz CSV1; the sender resets them each send).
// Peak excess + min threshold = the closest-to-firing the frame got.
float g_mExcessEmaPeak = 0.0f;      // peak of g_mExcessEma over the frame (A)
float g_iExcessThreshWinMin = 0.0f; // min of E over armed ticks this frame (A)
bool  g_iExcessArmedWin = false;    // detector armed at any tick this frame (peak/min validity)

// Load dump detection via dBcur/dt — three-tier cascade
float LoadDumpDtThresh1 = 7000.0f;  // A/s — tier 1: fires on a SINGLE sample above this (hard-switched FET disconnects)
float LoadDumpDtThresh  = 5000.0f;  // A/s — tier 2: fires when TWO consecutive samples both exceed this; lower reading of an adjacent pair peaked ~1.2 kA/s in logs
float LoadDumpDtThresh3 = 5000.0f;  // A/s — tier 3: fires when THREE consecutive samples all exceed this (slow relay-contact disconnects)
// Per-tier consecutive-sample counts. A sample = one fresh INA228 read (~6 ms, up to ~18 ms); time to act = N reads + the next
// control tick (~13 ms, up to ~30 ms). At defaults tier 3 is redundant with tier 2 (same threshold, more samples) and matters
// only with a lower tier-3 threshold. Deliberate — do not "fix".
int LoadDumpN1 = 1;
int LoadDumpN2 = 2;
int LoadDumpN3 = 3;
// Tier counters step in loadDumpStep() (INA read path); the control tick's load-dump block consumes the
// result. Both run in loop() on core 1, so plain globals are race-free.
bool  g_ldArmed     = false;  // load-dump block's gate was open on the last control tick; cleared by any AdjustFieldLearnMode pass that leaves before that block other than the fresh-CH1 wait
bool  g_ldTripNow   = false;  // the latest reading meets a tier
bool  g_ldTripLatch = false;  // a reading met a tier since the last control tick
float g_ldTripDbdt  = 0.0f;   // peak dBcur/dt among the latched readings (console message)
volatile bool g_loadDumpActive = false;
uint8_t g_awState = 0;  // integrator AW state: 0=normal 1=frozen(supervisor/D/slew) 2=saturated 3=bleeding 4=bumpless 5=target wind-down
uint32_t g_loadDumpCount = 0;

// Protection event counters — rising-edge, cleared by web UI reset buttons
uint32_t g_iExcessCount = 0;
uint32_t g_inaOVCount = 0;
uint32_t g_hardOCCount = 0;
uint32_t g_voltSpikeCount = 0;
uint32_t g_ovTierLowCount = 0;  // timed OV cut, LOW tier (session; lifetime twin in g_ovTel)
uint32_t g_ovTierMidCount = 0;  // timed OV cut, MID tier (session; lifetime twin in g_ovTel)
uint32_t g_voltDisagreeCritCount = 0;
uint32_t g_voltDisagreeWarnCount = 0;
uint32_t g_voltImplausibleCount = 0;
uint32_t g_tempCritCount = 0;
uint32_t g_tempSustainedCount = 0;
uint32_t g_tempStaleCount = 0;
uint32_t g_currentStaleCount = 0;
uint32_t g_voltStaleCount = 0;
uint32_t g_volt2StaleCount = 0;
uint32_t g_battTempLostCount = 0;
uint32_t g_dieTempLostCount = 0;

// CV loop tunable parameters live in the CV LOOP PARAMETERS block above

//additional leaderboard stuff
float sailing_days_alltime = 0.0;             // Total sailing days (lifetime)
float sailing_ratio = 0.0;                    // % of time spent sailing (calculated)
float sailing_dist_alltime = 0.0f;            // nm under sail / engine-off (lifetime)
float alt_power_max_alltime_w = 0.0f;         // peak alternator power, W (lifetime)
float solar_power_max_alltime_w = 0.0f;       // peak solar power, W (lifetime)
float VictronSolarPower_W = 0.0f;             // live solar panel power, W (VE.Direct PPV)
float VictronSolarVoltage_V = 0.0f;           // live solar panel voltage, V (VE.Direct VPV)
float VictronSolarCurrent_A = 0.0f;           // live solar panel current, A (PPV / VPV)
int VictronChargeState = -1;                  // VE.Direct CS code (0=Off,3=Bulk,4=Abs,5=Float,7=Equalize); -1 = unknown
int VictronMPPTMode = -1;                     // VE.Direct MPPT code (0=Off,1=V/I limited,2=active MPPT); -1 = unknown
int VictronError = -1;                        // VE.Direct ERR code (0=no error); -1 = unknown
float VictronYieldToday_kWh = 0.0f;           // VE.Direct H20 (0.01 kWh units -> kWh)
float VictronMaxPowerToday_W = 0.0f;          // VE.Direct H21 (W)
float VictronYieldYesterday_kWh = 0.0f;       // VE.Direct H22 (0.01 kWh units -> kWh)
float VictronMaxPowerYesterday_W = 0.0f;      // VE.Direct H23 (W)
float max_wind_speed_true_alltime = 0.0;      // Maximum true wind speed (knots)
float max_wind_speed_apparent_alltime = 0.0;  // Maximum apparent wind speed (knots)
float board_temp_max_alltime = -999.0;        // Maximum board temperature (°F)
float board_temp_min_alltime = 999.0;         // Minimum board temperature (°F)
float baro_pressure_max_alltime = 0.0;        // Maximum barometric pressure (mbar)
float baro_pressure_min_alltime = 9999.0;     // Minimum barometric pressure (mbar)
float best_upwind_vmg_alltime = 0.0;          // leaderboard: lifetime best VMG to windward (kt), gated RPM<50
float longest_gale_duration_hours_alltime = 0.0;  // leaderboard: lifetime longest continuous gale (sustained TWS>=34kt), hours
// Gale detector runtime state (not persisted): the current continuous gale run
bool galeActive = false;
uint32_t galeStartMs = 0;

// IGNITION-CYCLE WATERMARKS: min/max since boot, unlike the NVS-persisted *_AllTime / *Max / *_min. NAN-initialized so the first
// valid sample seeds both ends (wmIgnUpdate). 18 pairs × 8 bytes, internal RAM.
struct IgnWatermark { float lo; float hi; };

IgnWatermark wmIgn_amps     = { NAN, NAN };   // MeasuredAmps (A)
IgnWatermark wmIgn_altTempF = { NAN, NAN };   // AlternatorTemperatureF (°F)
IgnWatermark wmIgn_IBV      = { NAN, NAN };   // IBV — INA228 battery V
IgnWatermark wmIgn_Bcur     = { NAN, NAN };   // Bcur — INA228 battery A
IgnWatermark wmIgn_SOC      = { NAN, NAN };   // SOC percent (0..100, float)
IgnWatermark wmIgn_RPM      = { NAN, NAN };
IgnWatermark wmIgn_SOG      = { NAN, NAN };   // SOGNMEA (knots)
IgnWatermark wmIgn_AWS      = { NAN, NAN };   // ApparentWindSpeedNMEA (knots)
IgnWatermark wmIgn_TWS      = { NAN, NAN };   // TrueWindSpeedNMEA (knots)
IgnWatermark wmIgn_heel     = { NAN, NAN };   // imu_heel_deg
IgnWatermark wmIgn_pitch    = { NAN, NAN };   // imu_pitch_deg
IgnWatermark wmIgn_vacc     = { NAN, NAN };   // imu_vertical_accel_g
IgnWatermark wmIgn_baro     = { NAN, NAN };   // baroPressure (mbar)
IgnWatermark wmIgn_ambient  = { NAN, NAN };   // ambientTemp (°F)
IgnWatermark wmIgn_VMGman   = { NAN, NAN };   // VMGNMEA — VMG to manual bearing (knots)
IgnWatermark wmIgn_VMGup    = { NAN, NAN };   // VMGUpwind — VMG to windward (knots)
IgnWatermark wmIgn_battTempF  = { NAN, NAN }; // BatteryTempProbeF (°F)
IgnWatermark wmIgn_extraTempF = { NAN, NAN }; // ExtraTempF (°F)

// Watermark update/read helpers — defined in 7_functions.ino.
inline void wmIgnUpdate(IgnWatermark &w, float v);
inline int wmIgnCsv(float v, int scale);

// Universal data freshness tracking

// timing requirements are defined in the javascript file
// search for STALENESS DISPLAY THRESHOLDS at top of file

// DataIndex enum - only tracks real-time sensor data that might go stale if a sensor is disconnected
// Excludes peak/cumulative values that should persist even when source fails
enum DataIndex {
  IDX_HEADING_NMEA = 0,
  IDX_LATITUDE_NMEA,
  IDX_LONGITUDE_NMEA,
  IDX_SATELLITE_COUNT,
  IDX_VICTRON_VOLTAGE,
  IDX_VICTRON_CURRENT,
  IDX_ALTERNATOR_TEMP,
  IDX_THERMISTOR_TEMP,
  IDX_RPM,
  IDX_MEASURED_AMPS,
  IDX_BATTERY_V,                 // 10 - ADS1115 battery voltage
  IDX_IBV,                       // 11 - INA228 battery voltage
  IDX_BCUR,                      // 12 - Battery current from INA228
  IDX_CHANNEL3V,                 // 13 - ADS Ch3 Voltage
  IDX_DUTY_CYCLE,                // 14 - Field duty cycle percentage
  IDX_FIELD_VOLTS,               // 15 - vvout (calculated field voltage)
  IDX_FIELD_AMPS,                // 16 - iiout (calculated field current)
  IDX_COG_NMEA,
  IDX_SOG_NMEA,
  IDX_APPARENT_WIND_SPEED,
  IDX_APPARENT_WIND_ANGLE,
  IDX_TRUE_WIND_SPEED,
  IDX_TRUE_WIND_ANGLE,
  IDX_LEEWAY,
  IDX_VMG,
  IDX_BARO_PRESSURE,
  IDX_AMBIENT_TEMP,
  IDX_SOC_PERCENT,
  IDX_WIFI_STRENGTH,
  IDX_DYNAMIC_ALT_CURRENT_ZERO,
  IDX_CHARGING_MODE,
  IDX_TIME_TO_FULL_CHARGE,
  IDX_TIME_TO_FULL_DISCHARGE,
  IDX_IMU,                       // 33 - IMU (accel/gyro/derived angles)
  IDX_WATER_DEPTH,               // 34 - NMEA2k WaterDepth (PGN 128267)
  IDX_STW_NMEA,                  // 35 - Speed Through Water / SOW (PGN 128259) for the boat-performance polar
  IDX_VICTRON_SOLAR,             // 36 - Victron VE.Direct solar (PPV/VPV) staleness
  IDX_N2K_BATT,                  // 37 - NMEA2k received Battery Status (PGN 127508) V/A/T
  IDX_N2K_SOC,                   // 38 - NMEA2k received DC Detailed Status (PGN 127506) SOC/SOH
  IDX_DVCC,                      // 39 - DVCC follow: received charge-limit (CVL/CCL) stream
  IDX_N183,                      // 40 - NMEA 0183 serial receive (any checksum-valid sentence)
  IDX_N183_HDG,                  // 41 - NMEA 0183 heading only: IDX_N183 stays fresh on a GPS-only talker
  IDX_BATT_TEMP_PROBE,           // 42 - BATT-role DS18B20 (BatteryTempProbeF)
  IDX_EXTRA_TEMP,                // 43 - EXTRA-role DS18B20 (ExtraTempF)
  IDX_VE_BATT_TEMP,              // 44 - VE.Direct "T" battery temperature (VictronBattTempF)
  IDX_RVC_BATT_TEMP,             // 45 - RV-C DC_SOURCE_STATUS_2 battery temperature (rvcRxBattTempF)
  IDX_INA_DIE_TEMP,              // 46 - INA228 die temperature: stamped only by a sample past the warm-up discard and inside the bad-read window
  IDX_VE_SOC,                    // 47 - VE.Direct "SOC" field seen: only battery monitors send it, so it also proves the port is not an MPPT
  IDX_DVCC_VECAN,                // 48 - DVCC Victron source, VE.Can carrier: an accepted VREG limit register
  IDX_DVCC_WIFI,                 // 49 - DVCC Victron source, WiFi carrier: a /gxLimits post taken by dvccTick
  // Keep this last and increment when new added
  MAX_DATA_INDICES = 50
};

unsigned long dataTimestamps[MAX_DATA_INDICES];  // Uses the enum size automatically

const unsigned long DATA_TIMEOUT = 10000;  // 10 seconds default timeout
// Universal macros for clean syntax
#define MARK_FRESH(index) dataTimestamps[index] = millis()
#define IS_STALE(index) (millis() - dataTimestamps[index] > DATA_TIMEOUT)
// A never-stamped source holds 0, which IS_STALE reads as fresh for DATA_TIMEOUT after boot; window accumulators gate on this
// too, or every boot's first window takes the 0.0 initialiser as measured data.
#define IS_SEEN(index) (dataTimestamps[index] != 0)
#define SET_IF_STALE(index, variable, staleValue) \
  if (IS_STALE(index)) { variable = staleValue; }

// ISRG Root X1 (Let's Encrypt root CA), valid until 2035-06-04. Root, not intermediate, so R10->R12-style rotations don't
// break validation.
const char *server_root_ca =
  "-----BEGIN CERTIFICATE-----\n"
  "MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw\n"
  "TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh\n"
  "cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4\n"
  "WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu\n"
  "ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY\n"
  "MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc\n"
  "h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+\n"
  "0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U\n"
  "A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW\n"
  "T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH\n"
  "B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC\n"
  "B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv\n"
  "KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn\n"
  "OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn\n"
  "jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw\n"
  "qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI\n"
  "rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV\n"
  "HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq\n"
  "hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL\n"
  "ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ\n"
  "3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK\n"
  "NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5\n"
  "ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur\n"
  "TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC\n"
  "jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc\n"
  "oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq\n"
  "4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA\n"
  "mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d\n"
  "emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=\n"
  "-----END CERTIFICATE-----\n";


const char *OTA_PUBLIC_KEY =
  "-----BEGIN PUBLIC KEY-----\n"
  "MIICIjANBgkqhkiG9w0BAQEFAAOCAg8AMIICCgKCAgEAp2sRgMjD4wazKHo6Rk3g\n"
  "7APj7xsYGimOUKhWGWvd8wxFOEL+bHatAeKyCZXBjrZI9rdiUGtyHDdXS246YvV6\n"
  "dr56hJFT05n9iMHfx5T99NOyBl8Nm18dijqLGDSgVD9rC8v4aMrx5lSMrgzfz+bh\n"
  "HaDny6BFBHvaCQZW2KxXdnZElgnRidsoSR+5twdwdv9biHEwm4Miss1QQjmVZPVg\n"
  "SPIj4j9pf6wf8+kr57nEtjQORu+lLVj3OkM+A94N4k78+tR3gc3UMr1GvrqXm9D6\n"
  "VBH4OZDEgnx+QzraU9O60c6Jp/YXJkeLjRGFS7AaTd5+8sEFHoyx4BhvvxyUC7tX\n"
  "SpIZqziIlRjdJFE+8PjJ76u2BD1JmGeboUU+sxLJFiMpNeBskJG0kVwkFobkUtpN\n"
  "7O57CaGuzA8Eih+x3jTO85CiqgsK7rRckFyvMin1AgIvvxN9CUY+jTYvfcRdUO5Y\n"
  "7xQ8bFCvNUnpmqlWRveq3cxNZpNjOf99NinLJ6ewuaBqYXTtYbNCeAO+l0IiEl74\n"
  "c0HALRefCJO9mHmSoHsYJ7RRTLKhNO7jvoo7B+qCb2JDUKhwaLEetIDF6nw0dH22\n"
  "hONJuBOYktuwhrrsupJyBWYHHFmnFhqBcXCuTiPDhpcW05/3oAwApS7SpN8bN7dp\n"
  "SXQcqfsev1ke5IRosYpCv6cCAwEAAQ==\n"
  "-----END PUBLIC KEY-----\n";


// pre-setup stuff
// onewire
#define ONE_WIRE_BUS 13
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
DeviceAddress tempDeviceAddress;  // mirror of tempRoleAddr[TR_ALT] — the alternator read path in TempTask is unchanged by the registry below
// Multi-drop 1-Wire probe registry + role binding — spec: Working Markdown Docs/BATTERY_TEMP_SENSORS_SPEC.md §2.
// Bus search order is by ROM serial, not position, so roles bind by stored ROM code (owAddr* NVS keys),
// never by index. Written by TempTask (Core 0); /tempSensors copies it under owMux.
enum TempRole : uint8_t { TR_ALT = 0, TR_BATT = 1, TR_EXTRA = 2, TR_COUNT = 3 };
#define OW_MAX_PROBES 6
struct OwProbe {
  DeviceAddress addr;     // 8 bytes, family 0x28, ROM CRC valid
  float    lastF;         // NAN until first good read
  uint32_t lastGoodMs;    // millis() of last good read, 0 = never
  uint16_t okCount;
  uint16_t failCount;
  bool     present;       // seen on the last enumeration
};
OwProbe (&owProbes)[OW_MAX_PROBES] = PsramPlace::make<OwProbe[OW_MAX_PROBES]>();
uint8_t  owProbeCount = 0;
DeviceAddress tempRoleAddr[TR_COUNT];   // all-zero = unassigned
bool     tempRoleAssigned[TR_COUNT];
int8_t   tempRoleSlot[TR_COUNT] = { -1, -1, -1 };  // index into owProbes, -1 = not on bus
volatile bool owScanRequested = false;  // set by /get?owScan=1 and by the /get owAssign* handlers; TempTask re-enumerates and clears it
volatile bool owRolePersistPending[TR_COUNT];  // TempTask auto-bound a role; owRoleService() in loop() persists it (TempTask never writes NVS)
portMUX_TYPE owMux = portMUX_INITIALIZER_UNLOCKED;  // guards registry copies for /tempSensors
volatile bool bootHardwareInitDone = false;  // set by setup() once initializeHardware() has had its turn; TempTask's first bus search waits on it
//TEMPTASK DEBUG
volatile uint32_t tempReadSuccessCount = 0;
volatile uint32_t tempReadFailCount = 0;
volatile uint32_t tempCrcFailCount = 0;
volatile uint32_t tempCrcRecoveredCount = 0;
volatile uint32_t tempAllFFCount = 0;
volatile uint32_t tempPowerOn85Count = 0;
volatile uint32_t tempOutOfRangeCount = 0;
volatile uint32_t tempRequestFailCount = 0;
volatile uint32_t tempConnectedFailCount = 0;
volatile uint32_t tempResolutionFixCount = 0;
volatile uint32_t tempRereadFailCount = 0;
volatile uint32_t tempResolutionFixCrcFailCount = 0;
volatile uint32_t tempEnumerateFailCount = 0;
volatile uint32_t tempCoreBusySkipCount = 0;
volatile uint32_t tempStaleSkipCount = 0;
volatile float tempLastGoodF = -99.0f;
volatile unsigned long tempLastSuccessMillis = 0;
// Failure-counter snapshot taken at the moment of each good read, so a staleness trip can report
// the DELTA (what failed during the gap that starved the controller), not just boot totals.
volatile uint32_t tempFailSnapConn = 0, tempFailSnapEnum = 0, tempFailSnapCrc = 0,
                  tempFailSnapReq = 0, tempFailSnapRead = 0, tempFailSnapAllFF = 0;


//VictronEnergy
VeDirectFrameHandler &myve = PsramPlace::make<VeDirectFrameHandler>();
// WIFI STUFF
// WiFi wake button functionality (GPIO5)
int WiFiWakeButton = 0;                           // Current button state (1=pressed)
unsigned long wifiWakeButtonPressTime = 0;        // Timestamp when button pressed
const unsigned long WIFI_WAKE_DURATION = 300000;  // 5 minutes
bool wifiWakeActive = false;                      // Tracking wake mode state
unsigned long wifiWakeStart = 0;                  // millis() when wake was triggered; 0 = inactive. Never store millis()+constant — use elapsed-time comparisons to survive 49.7-day wraparound.

// WiFi Napping (Client-mode engine-off standby): stay associated in modem-sleep instead of radio-off at 80MHz, so the dashboard
// stays reachable. Full radio-off after 12 h with no dashboard; then only the wake button or ignition. Inert in AP mode.
bool wifiNapEnabled = true;           // System Setting (Client only); default on — app can wake from engine-off standby without the GPIO5 button
bool wifiNapActive = false;           // true while modem-sleep nap is active

AsyncWebServer server(80);                  // Create AsyncWebServer object on port 80
AsyncEventSource events("/events");
AsyncEventSource gxEvents("/gxevents");    // Victron GX link: its own source, so events.count() stays "a phone is watching"
unsigned long webgaugesinterval = 100;      // delay in ms between sensor updates on webpage
// Per-CSV1-window aggregation for analog channels sampled faster than the ~100ms send interval,
// so a plotted point is the window mean (anti-aliased) or peak instead of one aliased snapshot.
// Methods, not free functions, so Arduino's auto-prototype pass doesn't reference WinAgg early.
struct WinAgg {
  float sum = 0.0f;
  float maxMag = 0.0f;  // signed value of the largest-magnitude sample
  uint16_t count = 0;
  uint32_t winStartMs = 0;
  void add(float v) {
    // Self-expiring: SendWifiData resets only while an SSE client is connected, so unbounded accumulation would wrap count and make
    // the first post-reconnect point a gap-wide mean. 5× the send interval tolerates deferred sends.
    uint32_t nowMs = millis();
    if (count && (nowMs - winStartMs) > 5UL * webgaugesinterval) reset();
    if (!count) winStartMs = nowMs;
    sum += v;
    count++;
    if (fabsf(v) > fabsf(maxMag)) maxMag = v;
  }
  // fallback covers a window with no fresh sample (e.g. field off slows the INA228 below the send rate)
  float value(bool useMax, float fallback) const {
    if (!count) return fallback;
    return useMax ? maxMag : sum / (float)count;
  }
  void reset() { sum = 0.0f; maxMag = 0.0f; count = 0; }
};
// IBV/Bcur ~6ms (INA228), aggAltCur = MeasuredAmps ~13ms (ADS1115 CH1). aggBattV/aggRpm/aggCh3
// (ADS1115 CH0/CH2/CH3, ~40ms) are mean-only for cleaner plots — the Max toggle never touches them.
WinAgg aggIbv, aggBcur, aggAltCur, aggBattV, aggRpm, aggCh3;
bool battMaxMode = false;  // false = window mean (default), true = max-magnitude on the V/I traces it covers
int plotTimeWindow = 30;                    // Plot time window in seconds

// WiFi provisioning settings persist in NVS as NK_ssid / NK_pass
// Cached WiFi client credentials (loaded once, reused for reconnects)
char cached_wifi_ssid[33] = "";  // 32 + null
char (&cached_wifi_pass)[65] = PsramPlace::make<char[65]>();  // 64 + null
// Main task subscribes to the TWDT late in setup(); before that, esp_task_wdt_reset() feeds
// nothing and just logs "task not found". Boot-path reset sites gate on this to stay silent.
bool wdtMainTaskSubscribed = false;
bool cached_wifi_creds_valid = false;


typedef struct {
  unsigned long PGN;
  void (*Handler)(const tN2kMsg &N2kMsg);
} tNMEA2000Handler;

void SystemTime(const tN2kMsg &N2kMsg);
void Rudder(const tN2kMsg &N2kMsg);
void Speed(const tN2kMsg &N2kMsg);
void WaterDepth(const tN2kMsg &N2kMsg);
void DCStatus(const tN2kMsg &N2kMsg);
void BatteryStatus(const tN2kMsg &N2kMsg);
void BatteryConfigurationStatus(const tN2kMsg &N2kMsg);
void COGSOG(const tN2kMsg &N2kMsg);
void GNSS(const tN2kMsg &N2kMsg);
void Attitude(const tN2kMsg &N2kMsg);
void Heading(const tN2kMsg &N2kMsg);
void GNSSSatsInView(const tN2kMsg &N2kMsg);
void WindSpeed(const tN2kMsg &N2kMsg);
void VictronVreg126720(const tN2kMsg &N2kMsg);

//PGN Handler Table
tNMEA2000Handler NMEA2000Handlers[] = {
  { 126992L, &SystemTime },
  { 127245L, &Rudder },
  { 127250L, &Heading },
  { 127257L, &Attitude },
  { 127506L, &DCStatus },
  { 127508L, &BatteryStatus },
  { 127513L, &BatteryConfigurationStatus },
  { 128259L, &Speed },
  { 128267L, &WaterDepth },
  { 129026L, &COGSOG },
  { 129029L, &GNSS },
  { 129540L, &GNSSSatsInView },
  { 130306L, &WindSpeed },
  { 0, 0 }
};
// 126720 (Victron proprietary fast-packet, VREG carrier) is deliberately NOT in this table:
// HandleNMEA2000Msg dispatches it straight to VictronVreg126720 ahead of the receive gate,
// so DVCC follow works with receive off WITHOUT admitting other traffic to the table scan.

Stream *OutputStream = &Serial;

// ===== NMEA2000 transmit (producer) settings — spec: Working Markdown Docs/NMEA2K_TRANSMIT_SPEC.md =====
int n2kTxEnable = 0;        // master transmit switch; 0 = listen-only (no bus presence). A change joins/leaves the bus live (n2kApplyNodeConfig)
int n2kDeviceInstance = 0;  // N2K device instance (bump for a second regulator on the same bus)
int n2kBattEnable = 1;      // battery bank 127508+127506 pair
int n2kBattInstance = 0;
int n2kBattCfgEnable = 0;   // 127513 battery configuration (off: a BMV/shunt device often already claims the bank's instance)
int n2kAltEnable = 1;       // alternator as its own DC instance: 127508 + 127506 DCType=Alternator
int n2kAltInstance = 1;
int n2kAltTempEnable = 1;   // 130312 alternator temperature — suppressed entirely while the sensor reads NAN (MFD "data lost" alarms cover the failed-sensor case)
int n2kTempInstance = 4;
int n2kTempSource = 3;      // tN2kTempSource; 3 = Engine Room (no Alternator source exists in the standard set)
int n2kExtraTempEnable = 0;   // 130312 for the EXTRA-role probe (SLOT_TEMP_EXTRA) — skipped entirely while ExtraTempF is not fresh, same rule as the alternator slot
int n2kExtraTempInstance = 5;
int n2kExtraTempSource = 3;   // tN2kTempSource for the EXTRA probe
int n2kChgrEnable = 1;      // 127507 charger status
int n2kChgrInstance = 0;
int n2kChgrCfgEnable = 1;   // 127510 charger configuration — the only place field drive % reaches the bus (its ChargeCurrentLimit field)
int n2kChgrMode = 0;        // tN2kChargerMode published by both charger PGNs: 0 Standalone, 1 Primary, 2 Secondary — a label only, never a control input
int n2kEngRpmEnable = 0;    // 127488 engine RPM (off: conflicts with a real engine gateway on the same instance)
int n2kEngInstance = 0;
int n2kEngDynEnable = 0;    // 127489 engine dynamic (alternator voltage + warning bits)
int n2kEngBitsEnable = 1;   // include the discrete warning bits in 127489 (hard over-temp cut/alarm, low voltage, not-charging) — never fires on thermal derating
int n2kConvStatEnable = 1;  // 127750 converter status: a GX fills the alternator's /State from it, never from 127507's Bulk/Absorption/Float

// ===== RV-C transmit (producer) settings — spec: Working Markdown Docs/RVC_TRANSMIT_SPEC.md =====
// RV-C DGNs are J1939 PGNs and ride the same 250 kbps port and the same claimed address as our
// N2K node; they go out through the same round-robin so the one-message-per-pass budget holds.
int rvcTxEnable = 0;        // RV-C transmit master. Independent of n2kTxEnable for the toggle, but
                            // it needs the same node mode (n2kApplyNodeConfig).
int rvcChgrEnable = 1;      // CHARGER_STATUS + _2 + _3 + CHARGER_CONFIGURATION_STATUS
int rvcDcEnable = 1;        // DC_SOURCE_STATUS_1/2/3 at our own alternator DC instance
int rvcFaultEnable = 1;     // DM_RV diagnostic message (fault + operating status)
int rvcChgrInstance = 49;   // 0x31: charger instance 1 with the alternator type nibble (3) in the
                            // upper 4 bits — the de-facto convention a WS500 uses (a default WS500
                            // lands on 0x30). Not ratified RV-C; 49 is a legal instance either way.
int rvcDcInstance = 5;      // the alternator's own DC source instance. 1-4 are the standard banks
                            // (house/chassis/secondary/genset start) — never publish into those.
int rvcDevPriority = 80;    // RV-C device priority, spec Table 6.5.2b: 80 = Charger. Below a BMS's
                            // 120, so a real battery monitor always outranks us on shared data.
// Deliberately absent from N2kTransmitMessages[]: RV-C DGNs are not N2K PGNs, so listing them in
// the library's 126464 transmit-PGN reply would be a false claim about our N2K capability.

uint32_t n2kTxCount = 0;      // frames/messages accepted by SendMsg since Reset Peak Values
uint32_t n2kTxDropCount = 0;  // SendMsg failures (TX queue + retry ring full — normal when no bus is attached)
int n2kSrcAddrLive = -1;      // claimed source address; -1 = not claimed / listen-only
uint8_t n2kAddrPending = 255;    // address-claim result awaiting NVS persist (255 = none) — flushed field-off only, per the no-flash-in-control-path rule
uint8_t n2kPreferredAddr = 22;   // address a (re)join claims first: the NVS value at boot, then the latest claim result
bool n2kNodeActive = false;      // library mode in force: true = ListenAndNode, false = ListenOnly
volatile bool n2kNodeReconfigPending = false;  // set by the /get handler on the web task; n2kApplyNodeConfig() runs on Core 1, the only task that may touch the NMEA2000 object
const unsigned long N2kTransmitMessages[] PROGMEM = { 127488UL, 127489UL, 127506UL, 127507UL, 127508UL, 127510UL, 127513UL, 127750UL, 130312UL, 0 };
// Mirrors NMEA2000Handlers[] so the library's 126464 RX PGN list is truthful (spec §8).
// RV-C DGNs are deliberately absent: they are not N2K PGNs — they arrive via the fork's raw-RX tap.
const unsigned long N2kReceiveMessages[] PROGMEM = { 126720UL, 126992UL, 127245UL, 127250UL, 127257UL, 127506UL, 127508UL, 127513UL, 128259UL, 128267UL, 129026UL, 129029UL, 129540UL, 130306UL, 0 };
void nmea2kTransmitTick();
// ===== NMEA2000 received battery data (127508 + 127506 at n2kRxBattInstance) =====
// Never an input to field control, protections or targets. With BattMonSrc = 1 the SoC is the one shown and used,
// and with no regulator shunt the current drives the taper exit and current rebulk.
int n2kRxBattInstance = 0;   // battery instance to read (a shunt/BMS "virtual shunt" bridged onto N2K)
float n2kRxBattV = NAN;      // received battery voltage (127508)
float n2kRxBattA = NAN;      // received battery current (127508, signed)
float n2kRxBattTempF = NAN;  // received battery temperature (127508)
int n2kRxSoc = -1;           // received state of charge % (127506); -1 = not available
int n2kRxSoh = -1;           // received state of health % (127506); -1 = not available

// ===== DVCC-style charge-limit follow (CVL/CCL) — spec: Working Markdown Docs/DVCC_FOLLOW_SPEC.md =====
// A received charge limit is a clamp-only REQUEST: it can lower the voltage target or current ceiling, never raise; every local
// protection outranks it. Sources: RV-C DC_SOURCE_STATUS_4 / BATTERY_STATUS_4 (raw-RX tap) and Victron VE.Can VREG 0x351 (0xEF00 +
// 126720). The Victron layout is PROVISIONAL until validated against a real GX capture (spec §8a); the trust machine gates it.
int dvccEn = 0;             // master follow switch; decoded limits clamp targets only when 1
int dvccSrcType = 0;        // limit source: 0 = Victron GX (VE.Can VREG and WiFi /gxLimits carriers), 1 = RV-C
int dvccInst = 1;           // RV-C DC instance filter (RV-C is 1-oriented; 0 = accept any). Victron path follows the lowest of every Victron sender instead
int dvccSilenceS = 30;      // no valid limit frame for this long → drop the clamp, revert to local targets
int dvccVicCutHold = 1;     // 1 = a Victron do-not-charge stays held after it stops arriving; 0 = it ages out like the voltage/current limits
int dvccLostStop = 0;       // 1 = with follow on and nothing being followed (waiting, settling, stale, untrusted), charge nothing; 0 = own targets
bool dvccNoLimitStop = false;  // dvccLostStop is holding the current ceiling at zero right now (control loop, for CSV2)
int dvccSettleS = 10;       // in-range updates must span this long (and ≥2 frames) before the first clamp applies
float dvccCvlMin = 9.0f;    // plausible-CVL window, absolute volts (class-seeded/rescaled like other voltage settings);
float dvccCvlMax = 16.0f;   //   a CVL outside the window latches UNTRUSTED until manual reset
// Live state — written by the decode tap + dvccTick() (both ParseMessages/loop context, core 1)
float dvccRxCvl = NAN;      // last decoded charge-voltage limit (displayed even when follow is off)
float dvccRxCcl = NAN;      // last decoded charge-current limit (NAN = dialect sent not-available)
uint32_t dvccRxLastMs = 0;  // millis() of last decoded limit frame
uint32_t dvccRxCount = 0;   // decoded limit frames since boot (dvccTick uses it as a new-frame edge)
uint8_t dvccRxSrcAddr = 255;    // sender bus address (255 = none yet, DVCC_ADDR_WIFI = only the Victron WiFi carrier is fresh)
uint8_t dvccRxPriority = 0;     // RV-C device priority of the sender (BMS = 120)
uint8_t dvccRxChgState = 0;     // RV-C desired charge state — value 1 (do not charge) forces effective CCL 0; other values displayed only
uint8_t dvccState = 0;          // 0 off, 1 waiting, 2 settling, 3 following, 4 stale, 5 untrusted
uint8_t dvccUntrustReason = 0;  // 0 none, 1 CVL out of window, 2 CCL implausible, 3 flapping
float dvccCvlV = NAN;       // ACTIVE clamps consumed by the control path — non-NAN only while FOLLOWING
float dvccCclA = NAN;
// CCL basis: true = an alternator-current ceiling (the WiFi CCL, dvcc.py's per-alternator share, which already has solar
// subtracted and measured DC loads added); false = battery current (VE.Can / RV-C), converted with loadOffsetFilt on a live shunt
bool dvccRxCclAltBasis = false;  // of dvccRxCcl, set by each ingest
bool dvccCclAltBasis = false;    // of dvccCclA, copied with it in dvccTick
float loadOffsetFilt = 0.0f;     // EMA of MeasuredAmps − Bcur (A; negative = other sources exceed loads), control loop (6_functions.ino)
bool dvccResetReq = false;    // momentary UNTRUSTED-latch reset request from the web UI; consumed by dvccTick()
bool dvccCfgChanged = false;  // source/instance edited: dvccTick clears decode/settle state (an UNTRUSTED latch is preserved)
// The Victron source has two carriers, VE.Can and WiFi (GET /gxLimits from the GX bridge); dvccRxCvl/Ccl is their merge.
// /gxLimits runs on async_tcp and only fills this mailbox under dvccWifiMux; dvccTick (loop) takes it on the next pass.
float dvccWifiBoxCvl = NAN;
float dvccWifiBoxCcl = NAN;
volatile uint32_t dvccWifiBoxSeq = 0;  // bumped per post; read unlocked as the "anything new" test
static portMUX_TYPE dvccWifiMux = portMUX_INITIALIZER_UNLOCKED;
float dvccRxWifiCvl = NAN;  // WiFi carrier, loop side: last values taken (NAN = not sent) and when
float dvccRxWifiCcl = NAN;
uint32_t dvccRxWifiMs = 0;
float dvccRxCanCvl = NAN;   // VE.Can carrier as it entered the last merge: held registers, CCL 0 under a stop-charging command
float dvccRxCanCcl = NAN;
float dvccRxCanShare = NAN;  // VE.Can carrier: the GX's 0x2015 addressed to this node, this alternator's share (A)
#define DVCC_ADDR_WIFI 254  // dvccRxSrcAddr while only the WiFi carrier is fresh (254 is never a J1939 sender of data)
// Raw-frame capture ring (spec §8a), PSRAM, filled from the raw-RX tap (5_functions), served as hex by /dvccCapture (3_functions)
// so a customer boat can validate the decode with no tooling. Declared here so both files see it.
struct DvccCapEntry {
  uint32_t ms;
  uint32_t id;
  uint8_t len;
  uint8_t data[8];
};
#define DVCC_CAP_N 512
DvccCapEntry *dvccCapRing = nullptr;  // ps_malloc'd in initDvccCapture() (setup)
uint16_t dvccCapHead = 0;             // next write slot; wraps
uint16_t dvccCapCount = 0;            // total valid (saturates at DVCC_CAP_N)
volatile bool dvccCapPause = false;   // /dvccCapture (async_tcp task) sets while snapshotting so the loop-task tap can't overwrite an entry mid-read
// Set/read of dvccCapPause and of the head/count pair happens under this spinlock, and so does
// the tap's slot write - that is what makes the pause handshake safe across the two cores.
static portMUX_TYPE dvccCapMux = portMUX_INITIALIZER_UNLOCKED;
// ===== Bus node identity roster (spec section 15) =====
// A followed limit is uninterpretable without the publisher's identity (a GX rewrites a managed battery's numbers per brand).
// Filled PASSIVELY from PGN 60928 (NAME) and 126996 (product info): with transmit off we can't request either, so hearing nothing
// is normal. Guarded by dvccCapMux (shared with the capture ring - never nested).
struct DvccNode {
  uint32_t unique;     // NAME unique number (21-bit)
  uint32_t lastMs;     // millis() of the last identity frame from this address; also the eviction key
  uint16_t mfg;        // NAME manufacturer code (358 = Victron, 2046 = open/unregistered)
  uint16_t prodCode;   // 126996 product code
  uint16_t vicProdId;  // Victron VREG 0x0100 product id, if this node ever broadcasts one
  uint8_t addr;
  uint8_t func;        // NAME device function
  uint8_t cls;         // NAME device class
  uint8_t devInst;     // NAME device instance byte (lower 3 bits + upper 5 bits, as sent)
  uint8_t sysInst;     // NAME system instance
  uint8_t haveName;    // 60928 seen
  uint8_t haveProd;    // 126996 seen
  char model[24];      // 126996 strings, truncated and trailing-space trimmed
  char sw[16];
  char serial[16];
};
#define DVCC_NODE_N 24
DvccNode *dvccNodes = nullptr;  // ps_malloc'd in initDvccCapture()
uint8_t dvccNodeCount = 0;
uint16_t dvccAuthMfg = 0;   // identity of whoever currently holds dvccRxSrcAddr (0 = never heard)
uint16_t dvccAuthProd = 0;  // its 126996 product code, else its Victron VREG product id, else 0
void dvccCopyPlainText(char *dst, size_t dstSz, const char *src);
void dvccNoteName(uint8_t src, const unsigned char *d, int len);
void dvccNoteProductInfo(const tN2kMsg &N2kMsg);
void initDvccCapture();
void dvccTick();
bool dvccStopForNoLimits();
void dvccRawFrameTap(unsigned long id, unsigned char len, const unsigned char *buf);

// ===== NMEA 2000 bus health, raw capture, per-minute history — spec: NMEA2K_TRANSMIT_SPEC.md §16 =====
// Inputs: the _xeng driver's tNMEA2000_esp32::BusStats. All writers are on Core 1 (the raw taps inside
// ParseMessages / SendMsg, and n2kBusTick at 1 Hz); the web handlers only read, or hand buffers over under canCapMux.
uint8_t n2kBusVerdictCode = 0;     // 0 off, 1 OK, 2 nothing heard, 3 receive errors, 4 nobody answering, 5 bus-off (by severity)
uint32_t n2kRxFramesBase = 0;      // BusStats.rxFrames at the last Reset Peak Values, so CSV2 n2kRxFrames resets like n2kTxCount
uint32_t n2kRxLostRun = 0;         // rx_missed + rx_overrun counted only while ParseMessages runs: with it stopped the queue fills by design
uint16_t n2kNodesHeard = 0;        // distinct source addresses heard in the last 60 s, ours excluded
uint32_t *n2kLastSeenMs = nullptr; // [256] millis() of the last frame from each source address (0 = never); PSRAM, allocated on first run
uint32_t n2kPassMs = 0;            // millis() taken once per pass ahead of ParseMessages, so the per-frame tap does a single store
// Raw capture (§16.3). One 20-byte entry per frame; the ring has one writer (Core 1) and is read only once frozen.
struct CanCapEntry {
  uint32_t us;  // micros() since the recording started
  uint32_t id;
  uint8_t len;
  uint8_t data[8];
  uint8_t tx;   // 1 = our own frame, from the TX tap
};
#define CANCAP_MAX_N 8192
#define CANCAP_MIN_N 1024
#define CANCAP_PSRAM_RESERVE (256UL * 1024UL)  // free PSRAM a recording must leave for every other feature (§16.6)
struct CanCapCtl {
  CanCapEntry *ring;
  uint32_t cap;
  volatile uint32_t count;
  volatile bool active;     // the taps' gate; true only while state == 1
  volatile uint8_t state;   // 0 idle, 1 recording, 2 ready
  uint8_t why;              // stop reason: 1 time, 2 ring full, 3 NMEA 2000 stopped
  uint8_t reqS;
  uint8_t dlRefs;           // downloads streaming from the ring; under canCapMux, blocks every free
  uint32_t txN;
  uint32_t startUs, durUs, elapsedUs, startMs, stopMs;
  uint64_t startEpochMs;    // 0 = clock not synced at start: timestamps are seconds since boot
  uint64_t startUptimeUs;
  int selfAddr;
  uint32_t tec0, rec0, tec1, rec1, missed0, overrun0, missed1, overrun1, busOff0, busOff1;
};
CanCapCtl canCap = {};
static portMUX_TYPE canCapMux = portMUX_INITIALIZER_UNLOCKED;
// Per-minute history (§16.3a), 1440 rows = 24 h of running minutes. 20 bytes, not the spec's 16: nodes_max is added for the
// cloud block and rx_frames is 32-bit so a busy bus (≈1,500 frames/s) does not saturate it.
struct N2kBusRow {
  uint32_t stamp;    // minute start: epoch s, or uptime s when flags bit 0 is set
  uint8_t flags;
  uint8_t worst;     // worst verdict in the minute
  uint8_t tecMax, recMax, nodesMax, busOffs;
  uint16_t txFrames;
  uint32_t rxFrames;
  uint16_t rxLost, busErrors;
};
#define N2K_BUSLOG_N 1440
#define N2K_HEARD_MS 75000UL  // "heard" window for devices and the Nothing-heard verdict: longer than a 60 s heartbeat (126993)
N2kBusRow *n2kBusLog = nullptr;
uint16_t n2kBusLogHead = 0, n2kBusLogCount = 0;
volatile uint32_t n2kBusLogSeq = 0;  // odd while a row is being written; /n2kbuslog.csv copies and retries on a change
int n2kDayWorst = 0, n2kDayBusOffs = 0, n2kDayRxLost = 0, n2kDayNodesMax = 0;  // CLOUD_DAILY_LIST rows, from n2kBusDailyRefresh()
bool n2kBusRunning();
bool n2kClockTrusted();
void n2kBusTick();
void n2kBusDailyRefresh();
void n2kRawTxTap(unsigned long id, unsigned char len, const unsigned char *buf);

//ADS1115 more pre-setup crap
uint32_t adsI2CErrorCount = 0;
uint32_t adsSlowReadCount = 0;      // times the convert-register read took >5ms (I2C stall events)
// I2C bus-health: these time ONLY the Wire transactions; equal to the whole-block ft_rai_ina228 / IMU-drain timers => the bus,
// much smaller => loop preemption. All four reset with "Reset Peak Values".
uint32_t inaBusReadWorstUs = 0;    // worst µs spent in the two INA228 Wire reads since reset
uint32_t inaBusSlowCount = 0;      // INA228 bus reads > 15 ms (one Wire-timeout's worth) since reset
uint32_t ina228ErrorCount = 0;     // INA228 reads dropped (sanity fail / exception)
float inaDieTempF = NAN;           // INA228 internal die temperature (°F) — the only sensor AT the power stage; BMP388 board temp is bulk and lags it badly
float g_fdDieTempF = NAN;          // the same reading as the heat law sees it: written only with a valid sample past the warm-up discard (IDX_INA_DIE_TEMP stamp)
bool g_inaDieRewarm = false;       // set by inaApplyConfig(): a re-initialized chip reads DIETEMP 0 C until its first conversion, so the warm-up discard runs again
uint32_t g_fieldDerateWorstUs = 0;  // worst updateFieldDerateCeilings() pass since boot / Reset Peak Values (/debug): the control-loop budget for it is 500 us
float inaDieTempMaxF = NAN;        // session max of inaDieTempF, zeroed at boot and by Reset Peak Values. Uploads once per 24h as device_state_daily.ina_die_temp_max_f, so a bad first read here becomes a permanent fleet record — hence the warm-up + corroboration gate at the read site
uint32_t imuFifoFetchWorstUs = 0;  // worst µs spent in Get_FIFO_Sample since reset
uint16_t imuFifoWorstSamples = 0;  // sample count of THAT worst fetch — 42 bytes is ~1ms at 400kHz, so a
                                   // worst at the 6-sample cap proves the stall is bus/preemption, not transfer size
// AdjustFieldLearnMode worst FULL pass section profiler (early-return passes aren't recorded: an Adjust Field spike without an
// aflWorstTotalUs rise was in the pre-gate region). Read via /debug; resets with "Reset Peak Values".
#define AFL_SECTIONS 7
uint32_t aflWorstTotalUs = 0;
uint32_t aflWorstSecUs[AFL_SECTIONS] = { 0 };

enum ADS1115_State {
  ADS_IDLE,
  ADS_WAIT,
  ADS_READ_RESULT
};

ADS1115_State adsState = ADS_IDLE;
uint8_t adsCurrentChannel = 0;  // Driven by adsSeq[] = {1,0,1,2,1,3}; CH1 fires 3× per cycle (fresh CH1 ~13 ms apart, up to ~30 ms)
int adsTriggeredChannel = 0;
unsigned long adsStateEntered = 0;
const unsigned long ADS_CONVERSION_MS = 3;  // 1.16ms at 860SPS + millis() granularity margin

volatile bool ch1FreshFlag = false;  // Set when CH1 result is ready, consumed by AdjustFieldLearnMode()

uint8_t PidSampleDivisor = 1;  // 1=PID runs every CH1 sample (~13 ms apart, up to ~30 ms), 2=every other, etc.
                               // CH1 is at positions 0, 2, 4 of adsSeq[] {1,0,1,2,1,3}; loop cadence sets the slot length (10–30ms field-on)

const uint16_t adsMuxCodes[4] = {
  ADS1115_REG_CONFIG_MUX_SINGLE_0,
  ADS1115_REG_CONFIG_MUX_SINGLE_1,
  ADS1115_REG_CONFIG_MUX_SINGLE_2,
  ADS1115_REG_CONFIG_MUX_SINGLE_3
};

// Inter-sample gap meters for the slow ADS channels (CH0 battV cross-check, CH2 RPM): last gap between VALID readings, worst
// since Reset Peak Values (adsGapUpdate). CSV2; Live Data → ESP32.
uint16_t ch0GapLastMs = 0, ch0GapWorstMs = 0;   // CH0 battery-voltage read gap (ms)
uint16_t ch2GapLastMs = 0, ch2GapWorstMs = 0;   // CH2 RPM read gap (ms)
uint16_t ch0GapFieldOnWorstMs = 0, ch2GapFieldOnWorstMs = 0;  // same meters, field gate open only — excludes field-off background passes
uint32_t ch0GapPrevMs = 0, ch2GapPrevMs = 0;    // timestamp of last valid read (internal)

// One-heavy-per-pass gate, reset at the top of loop(): staggerable periodics (alt-health fold, boat-perf fold, LittleFS free refresh, WiFi
// send) claim it or defer one pass, capping loop time at floor + one heavy item. Control loop / RAI / IMU drain are NOT gated.
bool gHeavyRanThisPass = false;

// CSV2 (~548 fields, every 5 s) send cost, µs: build (snprintf) vs send (events.send → AsyncTCP); last = previous cycle, worst
// since Reset Peak Values. Decides whether to chunk the build or the send.
uint32_t csv2BuildLastUs = 0, csv2BuildWorstUs = 0;   // CSV2 snprintf build time (µs)
uint32_t csv2SendLastUs  = 0, csv2SendWorstUs  = 0;   // CSV2 events.send time (µs)

// Forward declarations (for WiFi functions)
String readFile(fs::FS &fs, const char *path);
bool writeFile(fs::FS &fs, const char *path, const char *message);
// Versioned PSRAM-blob persistence (shared scaffold — see 2_functions.ino)
uint32_t writePsramBlob(const char *path, uint32_t magic, uint32_t version,
                        uint32_t userWord, const void *base, size_t recordSize,
                        uint32_t capacity, uint32_t startIdx, uint32_t count);
uint32_t readPsramBlob(const char *path, uint32_t magic, uint32_t version,
                       void *destBase, size_t recordSize, uint32_t destCapacity,
                       uint32_t *userWordOut, bool deleteAfter);
uint32_t psramBlobVersion(const char *path, uint32_t magic);   // header peek; never deletes (readPsramBlob discards on mismatch)
// App-usage analytics (2_functions.ino)
bool usageMergeDelta(const char *body);
bool buildUsagePayload(char *buf, uint32_t cap);
bool executeUploadUsage(const char *payload);
void resetUsageAccum(time_t newStart);
void dumpUsageAccum();
void loadUsageAccum();
String usageStatsJson();
void setupWiFi();
void enterLowPowerStandby();
bool connectToWiFi(const char *ssid, const char *password, unsigned long timeout);
void setupAccessPoint();
void setupWiFiConfigServer();
void dnsHandleRequest();
void HandleNMEA2000Msg(const tN2kMsg &N2kMsg);
void performOTAUpdateToVersion(const char *targetVersion);
void performOTAUpdate(const UpdateInfo &updateInfo);

// HTML for the WiFi configuration page with traditional concatenation
const char WIFI_CONFIG_HTML[] PROGMEM =
  "<!DOCTYPE html>"
  "<html><head>"
  "<title>WiFi Configuration</title>"
  "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
  "<style>"
  "body{font-family:Arial;padding:20px;background:#f5f5f5}"
  ".card{background:white;padding:20px;border-radius:8px;max-width:400px;margin:0 auto}"
  "h1{color:#333;margin-bottom:20px;text-align:center}"
  "input,select{width:100%;padding:8px;margin:5px 0;border:1px solid #ddd;border-radius:4px}"
  "button{background:#00a19a;color:white;padding:10px 20px;border:none;border-radius:4px;cursor:pointer;width:100%}"
  "button:hover{background:#008c86}"
  ".info-box{background:#e8f4f8;border:1px solid #bee5eb;color:#0c5460;padding:12px;border-radius:4px;margin:10px 0;font-size:14px}"
  ".status{display:none;font-weight:bold;text-align:center}"
  "label{display:block;margin-top:8px;color:#333}"
  // Option-grouping boxes: each WiFi mode (its description + its inputs) lives in one bordered box
  ".option{border-radius:8px;padding:14px;margin:16px 0;border:2px solid}"
  ".option-preferred{border-color:#00a19a;background:#f1faf9}"
  ".option-secondary{border-color:#ccc;background:#fafafa}"
  ".opt-badge{display:inline-block;font-size:12px;font-weight:bold;letter-spacing:.5px;text-transform:uppercase;padding:3px 10px;border-radius:12px;margin-bottom:10px}"
  ".badge-preferred{background:#00a19a;color:white}"
  ".badge-secondary{background:#888;color:white}"
  ".opt-title{font-weight:bold;color:#333;display:block;margin-bottom:4px}"
  ".opt-desc{font-size:14px;color:#444;line-height:1.45}"
  ".opt-note{display:block;font-size:11px;color:#999;line-height:1.35;margin-top:6px}"
  ".option .info-box{margin-top:12px}"
  // box-sizing so full-width inputs sit flush inside the padded option boxes
  "input,select{box-sizing:border-box}"
  "</style>"
  "</head><body>"

  "<div class=\"card\">"
  "<h1>WiFi Configuration</h1>"

  "<form action=\"/wifi\" method=\"POST\">"

  // Preferred option: client mode credentials live inside the teal-accented box
  "<div class=\"option option-preferred\">"
  "<span class=\"opt-badge badge-preferred\">Preferred</span>"
  "<span class=\"opt-title\">Connect to your ship's WiFi (Client Mode)</span>"
  "<span class=\"opt-desc\">Enter your ship's network WiFi credentials below. The regulator will run in Client Mode and the user interface will be accessible via your local wifi.  Navigate to alternator.local in any browser, just like you'd go to google.com.</span>"
  "<label>Ship's WiFi Name (SSID):</label>"
  "<input type=\"text\" name=\"ssid\" placeholder=\"Required for client mode\">"
  "<label>Ship's WiFi Password:</label>"
  "<input type=\"password\" name=\"password\" placeholder=\"Required for client mode\">"
  "<label style=\"display:flex;align-items:center;gap:8px;margin-top:12px;cursor:pointer\">"
  "<input type=\"checkbox\" name=\"forget_client\" style=\"width:auto;margin:0\">"
  "Forget saved ship's WiFi"
  "</label>"
  "<span class=\"opt-note\">Leave the two fields above blank and check this box to erase the saved ship's WiFi credentials. Until new credentials are saved, the regulator will boot back to this setup page with charging disabled.</span>"
  "</div>"

  // Non-preferred option: hotspot/AP credentials live inside the de-emphasized grey box
  "<div class=\"option option-secondary\">"
  "<span class=\"opt-badge badge-secondary\">Non-Preferred</span>"
  "<span class=\"opt-title\">Use the regulator as a Hotspot (Access Point)</span>"
  "<span class=\"opt-note\">You may use the regulator as a Hotspot (aka Access Point). The regulator controller will broadcast its own WiFi network which you can connect to from any device (phone, ipad, laptop, etc.).  Mostly the same functionality will exist at alternator.local, but with no internet, you won't be able to use weather mode, get software updates, see Community features, etc.  To enter this mode on a reboot, connect pin 12 in RJ3 (the rightmost ethernet connector, Blue wire) to Ground.  For permanent Access Point mode, leave this hardwired.</span>"
  "<label>New Alt. Reg. Hotspot Name (SSID):</label>"
  "<input type=\"text\" name=\"hotspot_ssid\" placeholder=\"Leave blank to keep the current name\">"
  "<label>New Alt. Reg. Hotspot Password:</label>"
  "<input type=\"password\" name=\"ap_password\" placeholder=\"Leave blank to keep the current password\">"
  "<label style=\"display:flex;align-items:center;gap:8px;margin-top:12px;cursor:pointer\">"
  "<input type=\"checkbox\" name=\"reset_ap\" style=\"width:auto;margin:0\">"
  "Reset hotspot to defaults"
  "</label>"
  "<span class=\"opt-note\">Leave the two fields above blank and check this box to return the hotspot to its defaults: ALTERNATOR_WIFI-&lt;unit id&gt; and password alternator123.</span>"
  "</div>"

  "<div class=\"info-box\">"
  "To return to this Wifi Configuration page at any time, connect the WifiReset wire (pin 11, RJ3, Green/White) to Ground during a restart."
  "</div>"

  "<button type=\"submit\" id=\"sv\">Save Configuration</button>"
  "<div class=\"info-box status\" id=\"st\"></div>"

  "<div class=\"info-box\">"
  "After saving, this page may become unresponsive or disappear. In any case, wait 25 seconds, then reconnect to your chosen network to access the full alternator interface at this same url (alternator.local).  Or, just use the iOS app."
  "</div>"
  "</form>"
  "</div>"

  // Post-save countdown: the Save button is replaced by the status panel so the result can't read as a button to press. The
  // countdown covers a fetch rejection when the AP drops first, and the backstop timer below covers a socket that goes quiet.
  "<script>"
  "(function(){var f=document.forms[0],b=document.getElementById('sv'),s=document.getElementById('st'),done=0;"
  "function cd(pre){if(done)return;done=1;var n=25;(function t(){s.textContent=n>0?(pre+'Restarting - '+n+'s'):'Restart complete. Reconnect your device to your chosen network, then open alternator.local in a browser.';"
  "if(n-->0)setTimeout(t,1000)})()}"
  "function bad(m){if(done)return;done=1;s.style.color='#a00';s.textContent=m;b.style.display='';b.disabled=false}"
  "f.addEventListener('submit',function(e){e.preventDefault();b.disabled=true;b.style.display='none';"
  "s.style.display='block';s.style.color='';s.textContent='Saving...';"
  "var q=new URLSearchParams(new FormData(f));"
  // Backstop: fetch() has no default timeout, so a POST that neither answers nor errors leaves
  // 'Saving...' up forever. Wording is hedged because we never saw the 200 in that case.
  "setTimeout(function(){cd('No answer from the regulator - it may already be restarting. ')},8000);"
  // A 400 (short AP password) or 500 (NVS write failed) answers fast and RESOLVES the fetch, so only
  // r.ok may start the countdown. A rejected fetch is the restart taking the AP down = really saved.
  "fetch('/wifi',{method:'POST',body:q}).then(function(r){if(r.ok){cd('Settings saved. ');return;}"
  "function e2(){bad('Not saved - the regulator rejected the settings (HTTP '+r.status+').')}"
  "return r.text().then(function(t){var m=String(t).replace(/<[^>]*>/g,' ').replace(/\\s+/g,' ').trim();"
  "if(m)bad('Not saved: '+m);else e2()},e2)},function(){cd('Settings saved. ')})});})();"
  "</script>"

  "</body></html>";

// mbedTLS allocator routed to PSRAM: this core's CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC forces the two ~16KB TLS record buffers into
// internal RAM, which fragments until a handshake fails with MBEDTLS_ERR_SSL_ALLOC_FAILED (-0x7F00, a bogus "connection failed").
// Registered at boot via mbedtls_platform_set_calloc_free (runtime equivalent of CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC, no custom core
// build). Internal fallback if PSRAM can't satisfy a request; free() works on either heap.
bool mbedtlsPsramHooked = false;   // setup(): mbedtls_platform_set_calloc_free accepted mbedtlsPsramCalloc
volatile uint32_t tlsAllocPsram = 0, tlsAllocInternal = 0, tlsAllocFailed = 0;   // mbedtlsPsramCalloc outcomes since boot (/debug)
static void *mbedtlsPsramCalloc(size_t n, size_t size) {
  void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p) {
    tlsAllocPsram++;
    return p;
  }
  p = heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (p) tlsAllocInternal++;
  else tlsAllocFailed++;
  return p;
}

void setup() {
  pinMode(4, OUTPUT);     // This pin is used to provide a high signal to Field Enable pin
  digitalWrite(4, LOW);   // Start with field off
  gpio4IsLow = true;      // keep the shadow in sync — field starts off

  // Serial.end();  // Disabled 2026-06-24 to test if needed — at first line of setup() Serial was never begun, so this should be a no-op. Delete if next flash is fine.
  delay(100);
  Serial.begin(230400);  //note, the below settings only apply to Serial, not Serial1 or Serial2
  // Serial.setTxTimeoutMs(500);    // avoid long stalls on any possible big serial monitor bursts     DOESN'T WORK ON NEW ESP32 BOARD DEFINITION IN ARDUINO IDE

  // ========== ENABLE DETAILED CRASH DUMPS ==========
  // esp_log_level_set("*", ESP_LOG_VERBOSE);  // Verbose logging.  May want this later, but adds massive noise to SSL/HTP stuff, may change timing

  Serial.setTxBufferSize(4096);  // 4 KB TX in internal RAM
  Serial.setRxBufferSize(2048);  // optional, we don't currently Rx anyway...
  delay(100);
  Serial.println("\n\n=== SYSTEM STARTUP ===");

  g_sessionId = esp_random() & 0x7FFFFFFFu;  // hardware RNG is up before WiFi; no NVS write per boot
  Serial.printf("Telemetry session id: %u\n", (unsigned)g_sessionId);

  // Move mbedTLS handshake buffers off the fragmenting internal heap into PSRAM (see
  // mbedtlsPsramCalloc above). MUST run before the first TLS handshake (WiFi STA / any cloud call).
  if (mbedtls_platform_set_calloc_free(mbedtlsPsramCalloc, free) != 0) {
    Serial.println("WARNING: mbedtls_platform_set_calloc_free failed - TLS buffers stay in internal RAM");
  } else {
    mbedtlsPsramHooked = true;
    Serial.println("mbedTLS allocator -> PSRAM (TLS no longer needs contiguous internal RAM)");
  }
  Serial.printf("PSRAM statics: %lu B in PSRAM, %u fell back to internal RAM\n", (unsigned long)PsramPlace::psramBytes,
                (unsigned)PsramPlace::internalFallbacks);

  // Allocate buffers from PSRAM
  configPayloadBuffer = (char *)ps_malloc(CONFIG_PAYLOAD_SIZE);
  if (!configPayloadBuffer) Serial.println("FATAL: configPayloadBuffer ps_malloc failed");
  payloadBuffer = (char *)ps_malloc(PAYLOAD_BUFFER_SIZE);
  if (!payloadBuffer) Serial.println("FATAL: payloadBuffer ps_malloc failed");
  tempBuffer = (char *)ps_malloc(PAYLOAD_BUFFER_SIZE);
  if (!tempBuffer) Serial.println("FATAL: tempBuffer ps_malloc failed");
  filenameBuffer = (char *)ps_malloc(FILENAME_BUFFER_SIZE);
  if (!filenameBuffer) Serial.println("FATAL: filenameBuffer ps_malloc failed");
  timestampBuffer = (char *)ps_malloc(TIMESTAMP_BUFFER_SIZE);
  if (!timestampBuffer) Serial.println("FATAL: timestampBuffer ps_malloc failed");
  messageBuffer = (char *)ps_malloc(MESSAGE_BUFFER_SIZE);
  if (!messageBuffer) Serial.println("FATAL: messageBuffer ps_malloc failed");
  usageSlots = (UsageSlot *)ps_malloc(USAGE_SLOTS * sizeof(UsageSlot));
  if (!usageSlots) Serial.println("FATAL: usageSlots ps_malloc failed");
  else memset(usageSlots, 0, USAGE_SLOTS * sizeof(UsageSlot));
  usageMutex = xSemaphoreCreateMutex();
  cxStartMutex = xSemaphoreCreateMutex();
  consoleHist = (ConsoleHistEntry *)ps_malloc(CONSOLE_HIST_SIZE * sizeof(ConsoleHistEntry));
  if (!consoleHist) Serial.println("FATAL: consoleHist ps_malloc failed");
  else memset(consoleHist, 0, CONSOLE_HIST_SIZE * sizeof(ConsoleHistEntry));
  sensorRing = (SensorSnapshot *)ps_malloc(SENSOR_RING_SIZE * sizeof(SensorSnapshot));
  if (!sensorRing) Serial.println("FATAL: sensorRing ps_malloc failed");
  else memset(sensorRing, 0, SENSOR_RING_SIZE * sizeof(SensorSnapshot));
  sustT = (uint32_t *)ps_malloc(SUST_RING_SIZE * sizeof(uint32_t));
  if (!sustT) Serial.println("FATAL: sustT ps_malloc failed");
  else memset(sustT, 0, SUST_RING_SIZE * sizeof(uint32_t));
  sustV = (float *)ps_malloc(SUST_RING_SIZE * sizeof(float));
  if (!sustV) Serial.println("FATAL: sustV ps_malloc failed");
  else memset(sustV, 0, SUST_RING_SIZE * sizeof(float));
  baroPressureHistory = (uint16_t *)ps_malloc(BARO_HISTORY_SIZE * sizeof(uint16_t));
  if (!baroPressureHistory) Serial.println("FATAL: baroPressureHistory ps_malloc failed");
  else memset(baroPressureHistory, 0, BARO_HISTORY_SIZE * sizeof(uint16_t));
  longTermRing = (LongTermRecord *)ps_malloc(LONGTERM_RING_SIZE * sizeof(LongTermRecord));
  if (!longTermRing) Serial.println("FATAL: longTermRing ps_malloc failed");
  else memset(longTermRing, 0, LONGTERM_RING_SIZE * sizeof(LongTermRecord));
  // Anchorage detection sliding-window ring (5 hr × 60 s = 300 slots × 24 B = ~7.2 KB).
  anchorageRing = (AnchorageSample *)ps_malloc(ANCHORAGE_RING_SIZE * sizeof(AnchorageSample));
  if (!anchorageRing) Serial.println("FATAL: anchorageRing ps_malloc failed");
  else memset(anchorageRing, 0, ANCHORAGE_RING_SIZE * sizeof(AnchorageSample));
  // Battery Health PSRAM buffers (sample buffer interval-gated at 15ms: 8192 covers ~120s,
  // i.e. the default 56s test plus headroom for a raised edge count or stretched dwell).
  bhSampleCap = 8192;
  bhSamples = (BattHealthSample *)ps_malloc(bhSampleCap * sizeof(BattHealthSample));
  if (!bhSamples) Serial.println("FATAL: bhSamples ps_malloc failed");
  // CV plant-fit sample buffer (40 ms gated in the CC phases): sized past the worst-case 300→180mV OV re-step at
  // the slowest ω_c so the measured edge is never lost to a full buffer and the buffer isn't the limiter before
  // the watchdog (cvpfServiceCompletion).
  cvpfBufCap = 6144;   // 246 s at 25 Hz — worst case is 148 s (45 s rest + 23 s step, twice, on the OV fallback)
  cvpfBuf = (CvPlantFitSample *)ps_malloc(cvpfBufCap * sizeof(CvPlantFitSample));
  if (!cvpfBuf) Serial.println("FATAL: cvpfBuf ps_malloc failed");
  bhResults = (BattHealthResult *)ps_malloc(bhResultCap * sizeof(BattHealthResult));
  if (!bhResults) Serial.println("FATAL: bhResults ps_malloc failed");
  else memset(bhResults, 0, bhResultCap * sizeof(BattHealthResult));
  bhCapRing = (BattCapPoint *)ps_malloc(bhCapCap * sizeof(BattCapPoint));
  if (!bhCapRing) Serial.println("FATAL: bhCapRing ps_malloc failed");
  else memset(bhCapRing, 0, bhCapCap * sizeof(BattCapPoint));
  taskArray = (TaskStatus_t *)ps_malloc(MAX_TASKS * sizeof(TaskStatus_t));
  if (!taskArray) Serial.println("FATAL: taskArray ps_malloc failed");
  taskCensus = (TaskCensus *)ps_calloc(2, sizeof(TaskCensus));
  if (!taskCensus) Serial.println("FATAL: taskCensus ps_calloc failed");
  // CH1 interval ring — 40 KB to PSRAM
  ch1Ring = (Ch1Entry *)ps_malloc(sizeof(Ch1Entry) * CH1_RING);
  if (!ch1Ring) Serial.println("FATAL: ch1Ring ps_malloc failed");
  else memset(ch1Ring, 0, sizeof(Ch1Entry) * CH1_RING);
  // Allocate currentWindow to PSRAM — SensorWindow has non-zero defaults (999900 sentinels)
  Serial.printf("Free PSRAM before window allocs: %u bytes\n", ESP.getFreePsram());
  currentWindow = (SensorWindow *)ps_malloc(sizeof(SensorWindow));
  if (!currentWindow) {
    Serial.println("FATAL: currentWindow ps_malloc failed");
    while (1) delay(100);
  } else new (currentWindow) SensorWindow();
  // Allocate imuWindow to PSRAM — ImuWindow has no non-zero defaults
  imuWindow = (ImuWindow *)ps_malloc(sizeof(ImuWindow));
  if (!imuWindow) {
    Serial.println("FATAL: imuWindow ps_malloc failed");
    while (1) delay(100);
  } else memset(imuWindow, 0, sizeof(ImuWindow));

  // IMU ring buffer — ~30 KB to PSRAM
  imuRingBuffer = (ImuRingBuffer *)ps_malloc(sizeof(ImuRingBuffer));
  if (!imuRingBuffer) Serial.println("FATAL: imuRingBuffer ps_malloc failed");
  else memset(imuRingBuffer, 0, sizeof(ImuRingBuffer));
  // Tuning score log — 50 records × ~48 bytes = ~2.4 KB PSRAM
  tuningLog = (TuningRecord *)ps_malloc(50 * sizeof(TuningRecord));
  if (!tuningLog) Serial.println("FATAL: tuningLog ps_malloc failed");
  else memset(tuningLog, 0, 50 * sizeof(TuningRecord));
  // Control Accuracy needs no PSRAM — accCur4/accVolt4 and the thermal scalars are plain globals.
  // CV tuning score log — 50 records × ~120 bytes = ~6 KB PSRAM
  cvTuningLog = (CVTuningRecord *)ps_malloc(50 * sizeof(CVTuningRecord));
  if (!cvTuningLog) Serial.println("FATAL: cvTuningLog ps_malloc failed");
  else memset(cvTuningLog, 0, 50 * sizeof(CVTuningRecord));
  // SystemID log — 50 records × ~76 bytes = ~3.8 KB PSRAM
  systemIDLog = (SystemIDRecord *)ps_malloc(50 * sizeof(SystemIDRecord));
  if (!systemIDLog) Serial.println("FATAL: systemIDLog ps_malloc failed");
  else memset(systemIDLog, 0, 50 * sizeof(SystemIDRecord));
  // SystemID sine-sweep history — 50 records × ~176 bytes = ~8.8 KB PSRAM
  sysidSweepLog = (SysIDSweepRecord *)ps_malloc(50 * sizeof(SysIDSweepRecord));
  if (!sysidSweepLog) Serial.println("FATAL: sysidSweepLog ps_malloc failed");
  else memset(sysidSweepLog, 0, 50 * sizeof(SysIDSweepRecord));
  // Current closed-loop sine-sweep history — 50 records × ~176 bytes = ~8.8 KB PSRAM
  tuningSweepLog = (TuningSweepRecord *)ps_malloc(50 * sizeof(TuningSweepRecord));
  if (!tuningSweepLog) Serial.println("FATAL: tuningSweepLog ps_malloc failed");
  else memset(tuningSweepLog, 0, 50 * sizeof(TuningSweepRecord));
  // Gate-tuning 10s live-readout trackers — ROLL_COUNT × ~84 bytes PSRAM
  gRoll = (Roll10s *)ps_malloc(sizeof(Roll10s) * ROLL_COUNT);
  if (!gRoll) Serial.println("FATAL: gRoll ps_malloc failed");
  else {
    memset(gRoll, 0, sizeof(Roll10s) * ROLL_COUNT);
    gRoll[ROLL_RPMEDGE].isMin = true;      // worst (smallest) RPM edge margin is the binding case
    // ROLL_RIPRPMSHIFT is a peak-tracked excess like the other rip* rows (§11) — no isMin
  }
  size_t loopStackBytes = getArduinoLoopTaskStackSize();
  UBaseType_t loopHighWaterBytes = uxTaskGetStackHighWaterMark(NULL);  // bytes on ESP32-S3

  Serial.printf("Configured Arduino loop stack: %u bytes\n", (unsigned)loopStackBytes);
  Serial.printf("Loop min free so far (high-water): %u bytes\n", (unsigned)loopHighWaterBytes);

  Serial.println("\n=== LOOP TASK STACK INFO ===");
  Serial.printf("High-water at setup(): %u bytes\n", (unsigned)loopHighWaterBytes);
  Serial.println();
  // === STACK SIZE VERIFICATION ===
  // Check immediately - this should show nearly full stack
  UBaseType_t stackRemainingNow = uxTaskGetStackHighWaterMark(NULL);
  Serial.println("\n=== LOOP TASK STACK INFO ===");
  Serial.printf("Stack remaining at setup(): %u bytes\n", stackRemainingNow);
  Serial.println();
  initializeDeviceId();    // Sets the UID (real or spoofed)
  checkDeviceUIDChange();  // Compares to last boot, COMMENT THIS OUT DUE TO AVOID NEED TO REREGISTER DURING TESTING.  NEEDS UNCOMMENTING WHEN DEVICE ID CHANGES!!
  loadAuthToken();         // Loads token (will be empty if just cleared)  // for supabase
  Serial.flush();          // Ensure it's sent before continuing
  int major = 0, minor = 0, patch = 0;
  const char *version = FIRMWARE_VERSION;
  int len = strlen(version);

  int firstDot = -1;
  for (int i = 0; i < len; i++) {
    if (version[i] == '.') {
      firstDot = i;
      break;
    }
  }
  int secondDot = -1;
  for (int i = firstDot + 1; i < len; i++) {
    if (version[i] == '.') {
      secondDot = i;
      break;
    }
  }
  // Parse each part
  if (firstDot > 0 && secondDot > firstDot) {
    for (int i = 0; i < firstDot; i++) {
      major = major * 10 + (version[i] - '0');
    }
    for (int i = firstDot + 1; i < secondDot; i++) {
      minor = minor * 10 + (version[i] - '0');
    }
    for (int i = secondDot + 1; i < len; i++) {
      patch = patch * 10 + (version[i] - '0');
    }
  }

  firmwareVersionInt = major * 10000 + minor * 100 + patch;
  Serial.print("Firmware Version Int: ");
  Serial.println(firmwareVersionInt);
  Serial.println();
  printPartitionInfo();  
  initializeNVS();
  rescaleJournalReplay();   // before every settings loader: finishes a class change or winding rescale a reboot cut short
  loadFuelTableFromNVS();

  if (!ensureLittleFS()) {
    Serial.println("CRITICAL: Cannot continue without filesystem");
    Serial.println();
    while (true)
      ;  // halt
  } else {
    importStagedCommit();   // before any loader opens them: files a config import staged swap in over the live ones
    initSensorBuffer();
    // Restore PSRAM ring from LittleFS backup if Phase 4 dumped one before
    // last power-down. No-op if no backup file exists.
    restoreSensorRingFromLittleFS();
    restoreLongTermRing();  // durable month-long plot cache → PSRAM
  }
  delay(50);
  checkWebFilesExist();
  sessionStartTime = millis();
  // Reset all per-function timing structs and session loop metrics at boot
  MaximumLoopTime = 0;
  loopTime5sWindow = 0;
  memset(&ft_ReadAnalogInputs, 0, sizeof(FuncTiming));
  memset(&ft_AdjustFieldLearnMode, 0, sizeof(FuncTiming));
  memset(&ft_logDashboardValues, 0, sizeof(FuncTiming));
  memset(&ft_updateSystemHealthStats, 0, sizeof(FuncTiming));
  memset(&ft_checkWiFiConnection, 0, sizeof(FuncTiming));
  memset(&ft_SendWifiData, 0, sizeof(FuncTiming));
  memset(&ft_CheckAlarms, 0, sizeof(FuncTiming));
  memset(&ft_calculateDerivedMetrics, 0, sizeof(FuncTiming));
  memset(&ft_ch1_compute_stats, 0, sizeof(FuncTiming));
  memset(&ft_uploadSensorHistory, 0, sizeof(FuncTiming));
  memset(&ft_dumpLongTermRing, 0, sizeof(FuncTiming));
  memset(&ft_uploadBufferedRecords, 0, sizeof(FuncTiming));
  memset(&ft_buildConfigPayload, 0, sizeof(FuncTiming));
  memset(&ft_UpdateBatterySOC, 0, sizeof(FuncTiming));
  memset(&ft_updateSensorWindow, 0, sizeof(FuncTiming));
  memset(&ft_checkTimeSync, 0, sizeof(FuncTiming));
  memset(&ft_updateAccelMetrics, 0, sizeof(FuncTiming));
  memset(&ft_ReadVEData, 0, sizeof(FuncTiming));
  memset(&ft_ReadNMEA0183, 0, sizeof(FuncTiming));
  memset(&ft_altHealth, 0, sizeof(FuncTiming));
  memset(&ft_altFold, 0, sizeof(FuncTiming));
  memset(&ft_boatPerf, 0, sizeof(FuncTiming));
  memset(&ft_altEngine, 0, sizeof(FuncTiming));
  memset(&ft_perfEngine, 0, sizeof(FuncTiming));
  memset(&ft_n2kTx, 0, sizeof(FuncTiming));
  memset(&ft_dvcc, 0, sizeof(FuncTiming));
  memset(&ft_n2kParse, 0, sizeof(FuncTiming));
  memset(&ft_huntGov, 0, sizeof(FuncTiming));
  memset(&ft_fastAltDrain, 0, sizeof(FuncTiming));
  memset(&ft_faMatrixFlush, 0, sizeof(FuncTiming));
  memset(&ft_faDetector, 0, sizeof(FuncTiming));
  memset(&ft_faWindowFinalize, 0, sizeof(FuncTiming));
  memset(&ft_zeroLogService, 0, sizeof(FuncTiming));
  memset(&ft_solarLedger, 0, sizeof(FuncTiming));
  memset(&ft_bhFlushCapNVS, 0, sizeof(FuncTiming));
  memset(&ft_kneeLearnService, 0, sizeof(FuncTiming));
  memset(&ft_rai_total, 0, sizeof(FuncTiming));
  memset(&ft_rai_ina228, 0, sizeof(FuncTiming));
  memset(&ft_rai_ads_state, 0, sizeof(FuncTiming));
  memset(&ft_rai_bmp_state, 0, sizeof(FuncTiming));
  memset(&ft_rai_imu, 0, sizeof(FuncTiming));


  captureResetReason();            // immediately capture the reason for last ESP32 shutdown and store in LittleFS and variable that won't be overwritten until next boot
  ensurePreferredBootPartition();
  loadNVSData();                   // Load persistent variables from NVS- everything from last session is restored
  initNVSCache();                  // Sync change-detection cache with loaded NVS values to prevent false writes
  restoreSoftClock();              // Re-establish a usable timebase (retained RTC, else NVS epoch) so AP-mode/no-internet Long Term records aren't stamped 0 and rendered as phantom gaps. Snaps to truth when a real source reports.
  loadUsageAccum();                // App-usage accumulator survives the maintenance restart (dumped at the shutdown flush points)
  huntMapLoad();                   // Oscillation-damper speed pockets (RAM-authoritative; this is the boot restore)
  {  // Seed Free Data Storage once — boot is a guaranteed-safe moment for the ~100ms usedBytes() traversal
    size_t fsTotal = 0, fsUsed = 0;
    if (fsStatsTry(fsTotal, fsUsed) && fsTotal >= fsUsed) LittleFsFreeKb = (int)((fsTotal - fsUsed) / 1024);
  }
  {  // Web-asset partition headroom (an OTA bundle that no longer fits is otherwise invisible). Reads the webFS mount
     // checkWebFilesExist() already made: no second mount, no new lfs buffers. 0 back means unmounted.
    size_t wTotal = webFS.totalBytes(), wUsed = webFS.usedBytes();
    if (wTotal > 0 && wUsed <= wTotal) {
      WebFsTotalKb   = (int)(wTotal / 1024);
      WebFsUsedKb    = (int)(wUsed / 1024);
      WebFsPartLabel = usingFactoryWebFiles ? "factory_fs" : "prod_fs";
      int pct = 100 * WebFsUsedKb / WebFsTotalKb;
      Serial.printf("Web FS %s: %d/%d KB used (%d%%)\n", WebFsPartLabel, WebFsUsedKb, WebFsTotalKb, pct);
      if (pct >= 85) queueConsoleMessageF("Web partition %d%% full (%d of %d KB) - the next OTA bundle may not fit", pct, WebFsUsedKb, WebFsTotalKb);
    }
  }
  // Black box beats the NVS copy for last-session stats: NVS full-saves fire only at field-off / shutdown, so a session that
  // ends in a crash leaves older SessionDur/MaxLoop/MinHeap there. The snapshot is exact at death.
  if (g_blackBoxPrevValid) {
    LastSessionDuration = g_blackBoxPrev.upMillis / 1000UL;
    LastSessionMaxLoopTime = g_blackBoxPrev.maxLoopUs;
    lastSessionMinHeap = g_blackBoxPrev.minHeapKB;
  }
  //Reset some parameters to zero since we are re-starting on a re-boot
  CurrentSessionDuration = 0;
  prevSessionMaxLoopTime = (uint32_t)LastSessionMaxLoopTime;  // last session's worst (µs), black-box-corrected above when available
  MaxLoopTime = 0;                       // reset for this session (persists to NVS on next save)
  totalPowerCycles++;
  saveNVSDataFull();  // Synchronous write — persists boot-time adjustments before loop() starts
  initAlternatorHealth();
  altSettingsLoad();
  initBoatPerformance();
  perfSettingsLoad();
  faInit();  // fast alt-current channel: continuous-ADC driver + PSRAM buffers (dormant if GPIO3 rails — broken/missing jumper)
  setCpuFrequencyMhz(240);
  pinMode(5, INPUT);      // WiFi wake button
  pinMode(2, OUTPUT);     // This pin is used to provide a field PWM indicator (pin 2 of ESP32 is the LED)
  pinMode(1, INPUT_PULLUP);      // Ignition — pull-up so optocoupler-off floats HIGH (= ignition off); opto pulls LOW = on
  pinMode(21, OUTPUT);    // Alarm/Buzzer output
  digitalWrite(21, LOW);  // Start with alarm off
  alarmOutputState = false;
  pinMode(42, INPUT);  // bmsLogic
  // Switch panel inputs on Cable 3. Pull-ups so an opto that is off (nothing wired, or switch open)
  // floats HIGH = not asserted; the opto pulls LOW = asserted. Same idiom as the ignition input above.
  pinMode(panelHiRatePin, INPUT_PULLUP);     // Cable 3 pin 5 - HIGH/LOW
  pinMode(panelForceFloatPin, INPUT_PULLUP);  // Cable 3 pin 7 - FORCEFLOAT
  // The core defaults LEDC to the 40MHz XTAL on S3, capping 12-bit PWM at 9727Hz (19kHz attach fails). Force the 80MHz APB clock
  // (ceiling 19455Hz): field PWM is the only LEDC user and APB stays 80MHz across the 240<->80MHz nap swings. Must run before the
  // first ledcAttach (first setDutyPercent call).
  ledcSetClockSource(LEDC_USE_APB_CLK);
  // PWM setup (needed for basic operation)
  //ledcAttach(pwmPin, SwitchingFrequency, pwmResolution);

  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  // If you have custom headers, also:
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "*");

  InitSystemSettings();       // load all settings from NVS. If no keys exist, create them.
  altApplyClassScales();      // alt-health Vbus cell size needs SYSTEM_VOLTAGE_CLASS — initAlternatorHealth ran before settings
  altSeedClassKnobs();        // deferred first-creation seeding of the class-dependent registry knobs (same reason)
  altHealthWindingInit();     // needs fieldWindApplied; a mismatch clears the record from loop()
  bhInitSettings();           // Battery Health: DCIR test config + persisted DCIR/capacity blobs
  initWeatherModeSettings();  // Add weather mode settings--- otherwise similar to line above (InitSystemSettings)
  runSettingsMigrations();    // settings-schema migration chain — must stay after the last NVS settings loader
  importBootActionsLoad();    // wipes a config import left for this boot; they run from loop() after every loader
  accessInit();               // settings password + paired-regulator list (own NVS keys, outside the CSV3 echo on purpose)
  loadTuningLog();            // restore last session's tuning records from LittleFS
  loadCVTuningLog();          // restore CV tuning records from LittleFS
  loadSystemIDLog();          // restore plant-delay (SystemID) records from LittleFS
  loadSysidSweepLog();        // restore Plant Delay sine-sweep history from LittleFS
  cxLedgerInit();             // commissioning-ledger staging buffers + NVS event counter
  loadTuningSweepLog();       // restore Current closed-loop sine-sweep history from LittleFS
  // Check if we should wake WiFi for a pending OTA update
  nvs_handle_t wake_handle;
  if (nvs_open("update_req", NVS_READONLY, &wake_handle) == ESP_OK) {
    uint8_t wakeFlag = 0;
    if (nvs_get_u8(wake_handle, "wake_flag", &wakeFlag) == ESP_OK && wakeFlag == 1) {
      wifiWakeStart = millis();
      queueConsoleMessage("UPDATE: WiFi wake enabled for pending update (5 min)");

      // Clear the wake flag immediately
      nvs_close(wake_handle);
      nvs_handle_t clear_wake;
      if (nvs_open("update_req", NVS_READWRITE, &clear_wake) == ESP_OK) {
        nvs_erase_key(clear_wake, "wake_flag");
        nvs_commit(clear_wake);
        nvs_close(clear_wake);
      }
    } else {
      nvs_close(wake_handle);
    }
  }

  resetSensorWindow();  // Initialize sensor window (Cloud Features My History, starts hi/low water marks all fresh)
  resetAccelWindow();
  setupWiFi();  // NOW setup WiFi with all settings properly loaded

  // ===== CREATE HTTPS TASK ON CORE 0 =====
  httpsQueue = xQueueCreate(2, sizeof(HttpsRequest));  // small messages: payload is a PSRAM pointer
  Serial.println(httpsQueue ? "HTTPS queue created" : "ERROR: Queue creation failed");

  // Both stay on Core 0, but priority > 0
  xTaskCreatePinnedToCore(TempTask, "TempTask", 6144, NULL, 1, &tempTaskHandle, 0);  // 6144: multi-drop registry pass (BATTERY_TEMP_SENSORS_SPEC.md §3); ensureCore0Tasks() in 4_functions re-creates with these params and must match
  Serial.println("Temp task created on Core 0");

  // With mbedTLS record buffers in PSRAM, only call frames sit on this stack
  // (~4KB observed peak via /debug stackHWM); 12KB keeps a ~3x margin.
  xTaskCreatePinnedToCore(httpsTask, "HTTPS", 12288, NULL, 1, &httpsTaskHandle, 0);
  Serial.println("HTTPS task created on Core 0");
  startPeerBrowseTask();      // mDNS browse for the other regulators on the network (one 3 s blocking query per 30 s, core 0)

  // Rectifier fault detector compute: Core 0, prio 1 (with TempTask/HTTPS, below the network tasks), off the Core-1 control loop;
  // woken by notification, verdict via faResultSem. PSRAM stack: 8KB internal would push the TLS handshake floor (~34KB contiguous)
  // out of reach; safe on Core 0 (IDF suspends the other core during flash-cache-disable). Watch faDetWorstComputeUs.
  faResultSem = xSemaphoreCreateBinary();
  xTaskCreatePinnedToCoreWithCaps(faDetTask, "faDet", 8192, NULL, 1, &faDetTaskHandle, 0, MALLOC_CAP_SPIRAM);
  Serial.println("Fault-detector task created on Core 0 (PSRAM stack)");

  // CPU-load sampler: Core 0, prio 0 (below every network/upload task); runs every 2 s only when Core 0 is otherwise idle,
  // sleeping between runs so it never starves the idle task / watchdog. 4KB PSRAM stack (TLS-floor reason).
  xTaskCreatePinnedToCoreWithCaps(cpuLoadTask, "cpuLoad", 4096, NULL, 0, &cpuLoadTaskHandle, 0, MALLOC_CAP_SPIRAM);
  Serial.println("CPU-load task created on Core 0 (prio 0, PSRAM stack)");
  startGxSendTask();          // Victron GX stream sender (Core 0): loop() only marks finished frames, never waits on lwIP for the GX

  if (currentMode == MODE_CLIENT && WiFi.status() == WL_CONNECTED) {
    syncTimeFromNTP();  // sends the request only; ntpPoll() in loop() adopts the answer
  } else {
    // WiFi not up within the boot connect window: first NTP try ~60 s out, not 12 h (checkTimeSync's
    // throttle starts from lastTimeSyncAttempt = 0). Same fast-retry arm syncTimeFromNTP uses.
    lastTimeSyncAttempt = millis() - (TIME_SYNC_INTERVAL - 60000UL);
  }


  esp_log_level_set("esp32-hal-i2c-ng", ESP_LOG_WARN);
  queueConsoleMessage("System starting up...");

  initializeHardware();  // also in sim mode: it still drives the real field, so the INA228's electrical OV cut must be armed
  bootHardwareInitDone = true;  // releases TempTask's first bus search (2_functions.ino); unconditional so a runtime hardwarePresent flip still enumerates

  // A WDT reset is safe for the bank: every GPIO resets to 0, so pin 4 drops the field before setup() reruns.

  // Enable watchdog - only after setup is complete
  Serial.println();
  Serial.println("Enabling watchdog protection...");

  // Configure watchdog (window shared with the OTA widen/restore in performOTAUpdateToVersion)
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = WDT_TIMEOUT_MS,
    .idle_core_mask = 0,
    .trigger_panic = true
  };

  // Try reconfigure first (TWDT likely already exists on core 3.x)
  esp_err_t result = esp_task_wdt_reconfigure(&wdt_config);

  if (result == ESP_OK) {
    Serial.println("✅ Watchdog reconfigured: 16s timeout");
  } else if (result == ESP_ERR_INVALID_STATE) {
    result = esp_task_wdt_init(&wdt_config);
    if (result == ESP_OK) {
      Serial.println("✅ Watchdog initialized: 16s timeout");
    } else {
      Serial.printf("❌ Watchdog init failed: %s\n", esp_err_to_name(result));
    }
  } else {
    Serial.printf("❌ Watchdog reconfigure failed: %s\n", esp_err_to_name(result));
  }

  // Add main task
  if (result == ESP_OK) {
    esp_err_t add_result = esp_task_wdt_add(NULL);
    if (add_result == ESP_OK) {
      wdtMainTaskSubscribed = true;
      Serial.println("✅ Main task added to watchdog");
      queueConsoleMessage("Watchdog: 16s timeout active");
    } else {
      Serial.printf("⚠️ Failed to add main task: %s\n", esp_err_to_name(add_result));
    }
  } else {
    queueConsoleMessage("Watchdog: Failed to configure");
  }
  loadLearningTableFromNVS();  // Load all table data at boot
  kneeLearnInit();             // Auto Min% learning: load knobs + learned per-bin state (after the table)
  rescaleJournalFinish();    // knee-floor reset, damper-map clear and notice for a rescale replayed at boot
  fieldWindingReconcile("boot");   // after the last field-side knob loader: an import writes NVS raw, so the knobs may sit on another winding's scale
  zeroLogInit();               // Zero-drift log: load toggle, alloc PSRAM ring, restore persisted records
  zeroFitInit();               // Temp-comp zero correction: alloc daily-fit history ring, restore /zerofit.bin
  solarLedgerInit();           // Solar ledger: alloc the 90-day ring in PSRAM, restore /solarledger.bin + the day in progress
  Serial.println();
  // Force initial sensor readings before main loop starts
  delay(50);  // Brief settling time
  if (hardwarePresent == 1) {
    ReadAnalogInputs();
    delay(50);           // Give it a moment to process
    ReadAnalogInputs();  // Second reading to be sure
  }
  applyChemistryOcvPreset();  // repair a stale/wrong-chemistry rested-voltage curve (idempotent) before the seed reads it
  const bool socSeedWasPending = socSeedPending;  // seedSocFromVoltage() clears it — capture whether this boot really estimates
  seedSocFromVoltage();  // deferred first-boot SoC estimate — needs the reads above (IBV) + vessel info (battery type/voltage/capacity)
  if (!socSeedWasPending) socSeedAutoAck();  // SoC carried over from NVS — don't re-nag the commissioning popup

  const esp_partition_t *running_partition = esp_ota_get_running_partition();
  const esp_partition_t *factory_partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
  if (running_partition == factory_partition) {
    currentPartitionType = 0;  // Factory
    Serial.println("=== We are in factory partition! ===");
  } else {
    currentPartitionType = 1;  // OTA_0 (production)
    Serial.println("=== We are in OTA partition! ===");
  }
  tempPID_init();
  thermalLog_init();
  pidLog_init();
  cvLog_init();
  tuningScore_init();
  // fieldCheck_init();  // TEMP-FIELDCHECK
  // Post-allocation PSRAM baseline: largest contiguous block matters as much as total —
  // cvLog's 333 KB is one heap_caps_malloc, so total-free can look fine while it can't fit.
  LargestPSRAMBlock = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024;
  largestPsramAtMs = millis();
  Serial.printf("PSRAM after setup: %u KB free / %u KB total, largest block %u KB\n",
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024),
                (unsigned)LargestPSRAMBlock);
  walkInternalHeap();  // seeds LargestInternalBlock / Heapfrag; with the field live, cpuLoadTask's next walk can be hours away
  Serial.printf("Internal RAM after setup: %d KB free, largest block %u KB\n",
                (int)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024), (unsigned)LargestInternalBlock);
  Serial.println("=== SETUP COMPLETE ===");
}

void loop() {

  esp_task_wdt_reset();
  starttime = esp_timer_get_time();
  // Latched at the top of the pass so a pass that drops the field mid-way still counts as
  // field-on (it ran control code). Consumed by the field-ON loop worst at the bottom of loop().
  ftLoopPassSeq++;  // pass tag for blame capture — must precede every TIMED_CALL in this pass
  bool passFieldOn = !gpio4IsLow;

  // Black box heartbeat: magic is never cleared first, so a reset mid-update can leave a valid snapshot mixing this pass's and the previous pass's fields
  g_blackBox.upMillis = millis();
  g_blackBox.ibv = IBV;
  g_blackBox.duty = dutyCycle;
  g_blackBox.rpm = RPM;
  g_blackBox.measAmps = MeasuredAmps;
  g_blackBox.altTempF = (int16_t)(!isfinite(AlternatorTemperatureF) ? -999 : AlternatorTemperatureF);  // (int16_t)inf is UB, same as (int16_t)NAN
  g_blackBox.maxLoopUs = MaxLoopTime;
  g_blackBox.minHeapKB = MinFreeHeap;
  g_blackBox.sysMode = (uint8_t)sysMode;
  g_blackBox.chargeStage = chargeStageDisplay;  // global kept fresh by the control loop — no function call here
  g_blackBox.magic = demoSeal ? BLACKBOX_DEMO_MAGIC : BLACKBOX_MAGIC;   // a reset in demo must not read as a real last-alive snapshot

  gHeavyRanThisPass = false;  // reset the one-heavy-per-pass scheduler gate for this loop pass

  checkOtaWedge();     // retire a stuck otaInProgress before anything below honors it
  ensureCore0Tasks();  // bring TempTask / httpsTask back after an OTA attempt; retry + alarm if they can't
  improvSerialPoll();  // USB installer handshake (4_functions.ino); returns at once when nothing is buffered

  // Deferred reboot (rebootRequestWhy says who asked) — wait 2s so the HTTP 200 reaches the caller
  if (importRebootDeadlineMs && !rebootRequested && (int32_t)(millis() - importRebootDeadlineMs) >= 0) {
    rebootRequestWhy = REBOOT_WHY_IMPORT;
    rebootRequestedAt = millis();
    rebootRequested = true;   // a records import whose config import never followed
  }
  if (rebootRequested && millis() - rebootRequestedAt > 2000) {
    const uint8_t why = (rebootRequestWhy < REBOOT_WHY_COUNT) ? rebootRequestWhy : REBOOT_WHY_REMOTE;
    Serial.println(REBOOT_WHY_LINE[why]);
    g_rebootIntent = why ? (REBOOT_INTENT_TAG | why) : 0;
    // The maintenance restart's ring, plot and usage saves (setup() restores them); flash writes only with the field cut.
    // Not its NVS or curve saves: imports and the config push reboot through here, and altHealthSave would overwrite
    // the altbaseSec a records import just wrote.
    if (gpio4IsLow) {
      if (currentWindow->dutyCycle_valid_us > 0) { pushSensorSnapshot(computeCollectionTime()); resetSensorWindow(); }
      if (!ringIsEmpty()) dumpSensorRingToLittleFS();
      dumpLongTermRing();
      dumpUsageAccum();
    }
    ESP.restart();
  }

  // Deferred commissioning-start persist (2_functions.ino): one staged NVS commit per pass — keeps
  // the Start's restore-point burst off the network task (in-handler it froze the SSE stream ~2 s).
  cxStartPersistService();
  {
    // Asymmetric debounce: OFF is taken at once (a stop request is safety), ON must hold 1 s, or a feed hovering at the opto threshold
    // restarts the charge cycle in BULK and reruns the shutdown flush on every flicker. Written once: Core 1 never sees the transient.
    static uint32_t ignOnSinceMs = 0;
    static bool ignFirstPass = true;  // a key already ON at boot is taken at once: lastIgnitionState starts at 1, so a debounced 0 here read as an OFF edge and ran the full shutdown flush on every boot
    bool rawOn = !digitalRead(1);  // ! is for optocoupler (LOW = ignition ON)
    int ignNext;
    if (!rawOn) { ignOnSinceMs = 0; ignNext = 0; }
    else {
      if (ignOnSinceMs == 0) ignOnSinceMs = millis() ? millis() : 1;
      ignNext = (Ignition == 1 || ignFirstPass || (uint32_t)(millis() - ignOnSinceMs) >= 1000UL) ? 1 : 0;
    }
    ignFirstPass = false;
    if (ignitionForcedOn()) ignNext = 1;  // force ON (Ignition Override: bench / no ignition wire; demo mode) — can ONLY force on, never off
    Ignition = ignNext;
  }
  // IgnitionOverride != 1 (Auto, default): no override — honor the real GPIO1 reading above

  servicePanelSwitchInputs();  // Cable 3 HIGH/LOW + FORCEFLOAT optos: always debounced and reported, and driving HiLow / MaintainMode while PhysicalPanelOverride is on (6_functions.ino)
 
  WiFiWakeButton = !digitalRead(5);  // Read WiFi wake button state. Active LOW (button pulls to ground)

  // Button press extends/starts timeout
  if (WiFiWakeButton == 1) {
    wifiWakeStart = millis();  // Reset 5-min window from now
  }

  // === OTA UPDATE CHECK - USER INITIATED UPDATE FROM NVS ===
  static bool manualUpdateCheckDone = false;
  static bool pendingUpdateWaiting = false;
  static char pendingVersion[64] = { 0 };

  // Read the staged-update flag ONCE (an NVS open per pass is too costly), but consume it only with WiFi up AND the field
  // de-energized: an intermittent hotspot absent this boot must not eat the update.
  if (!manualUpdateCheckDone && millis() > 5000 && currentMode == MODE_CLIENT) {
    pendingUpdateWaiting = checkForPendingUpdateNonBlocking(pendingVersion);
    if (pendingUpdateWaiting) Serial.printf("UPDATE: Found pending update for %s (waiting for WiFi if not connected)\n", pendingVersion);
    manualUpdateCheckDone = true;
  }

  if (pendingUpdateWaiting && WiFi.status() == WL_CONNECTED) {
    if (!gpio4IsLow) {
      // performOTAUpdateToVersion blocks the control loop for the whole download+flash, freezing field PWM at its last duty with
      // every software protection dead. Never start it with the field enable line up (a duty-0 hold counts as up); nothing can raise
      // duty once the loop is seized, so a present-state check is sufficient.
      static bool stagedOtaDeferLogged = false;
      if (!stagedOtaDeferLogged) {
        queueConsoleMessage("Staged update waiting: install starts when charging stops (field off)");
        stagedOtaDeferLogged = true;
      }
    } else if (esp_ota_get_running_partition() == esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL)) {
      // Only factory installs (into ota_0); a reboot landed here before the factory boot consumed the flag, so leave it armed for
      // factory. Do not clear it and let performOTAUpdate hop to factory: that drops the update silently.
      pendingUpdateWaiting = false;
      const esp_partition_t *factory_partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
      if (factory_partition && esp_ota_set_boot_partition(factory_partition) == ESP_OK) {
        nvs_handle_t wakeH;  // this boot already consumed wake_flag; the factory boot needs its WiFi window too
        if (nvs_open("update_req", NVS_READWRITE, &wakeH) == ESP_OK) {
          nvs_set_u8(wakeH, "wake_flag", 1);
          nvs_commit(wakeH);
          nvs_close(wakeH);
        }
        Serial.printf("UPDATE: staged update for %s found on ota_0 - restarting to factory to install\n", pendingVersion);
        Serial.flush();
        ESP.restart();
      }
      clearPendingUpdateNVS();  // factory unreachable: drop the flag rather than retry it every boot
      queueConsoleMessage("Staged update cancelled: could not switch to the factory partition");
    } else {
      pendingUpdateWaiting = false;

      // Clear the flag pre-attempt so a mid-install crash can't boot-loop
      clearPendingUpdateNVS();

      // core0Busy: other tasks skip flash/heavy work during the install (contract: only set
      // with the field off — satisfied by the gate above).
      core0Busy = true;

      performOTAUpdateToVersion(pendingVersion);  // This calls prepareForOTA() which kills tasks
    }
  }

  // === OTA UPDATE: simple one-shot FORCED UPDATE check ===
  static bool otaVerSent = false, otaForcedSent = false;
  static uint32_t otaWeakRetryMs = 0;   // a weak-signal skip waits 30 s before the next try
  if (!otaCheckDone && !demoSeal && millis() > 3000 && (otaWeakRetryMs == 0 || millis() - otaWeakRetryMs >= 30000UL)) {  // 3 seconds after boot
    if (currentMode == MODE_CLIENT && WiFi.status() == WL_CONNECTED && isRegistered && currentPartitionType != 0) {
      // Only proceed if no uploads in progress
      if (!core0Busy && fieldCutSettled(0)) {
        Serial.println("OTA: Running one-time firmware/version checks...");

        // Both sends must succeed before otaCheckDone, or a queue-full / weak signal at boot skips both permanently (registration hands
        // its version report to this check too). httpsQueue is only depth-2.
        bool weak = false;

        if (!otaVerSent) {
          if (WiFi.RSSI() < -80) {
            weak = true;
          } else {
            HttpsRequest req = { .type = HTTPS_UPDATE_FW_VERSION };
            if (xQueueSend(httpsQueue, &req, 0) == pdTRUE) otaVerSent = true;
            else Serial.println("OTA: HTTPS queue full — will retry FW version push next loop");
          }
        }

        if (!otaForcedSent) {
          if (WiFi.RSSI() < -80) {
            weak = true;
          } else {
            HttpsRequest req = { .type = HTTPS_CHECK_FORCED_UPDATE };
            if (xQueueSend(httpsQueue, &req, 0) == pdTRUE) otaForcedSent = true;
            else Serial.println("OTA: HTTPS queue full — will retry forced-update check next loop");
          }
        }

        if (otaVerSent && otaForcedSent) otaCheckDone = true;
        else otaWeakRetryMs = weak ? millis() : 0;
      }
      // If blocked, will retry next loop iteration
    } else if (currentMode != MODE_CLIENT || currentPartitionType == 0) {
      // Factory partition skipped on purpose: a GPIO41 refuge boot must stay put, and a transient factory boot mid-install would report
      // the golden image's stale version. Also load-bearing: pre-0.0.3 golden images' factory_fs bundle disables the forced-update
      // confirm dialog itself — unclickable deadlock.
      Serial.println("OTA: Skipping one-time checks - not CLIENT mode, or factory partition");
      otaCheckDone = true;
    }
    // Client mode with no WiFi or registration yet stays pending, so a router that comes up after boot still gets
    // the check instead of waiting for the 6 h re-check.
  }
  // Periodic forced-update/version re-check (~6 h): at 24-72 h uptimes a cloud-staged forced update would sit until reboot and a
  // CANCELLED push would keep the cached modal stuck. Install stays user-triggered (only raises/clears hasForcedUpdate). Version FIRST:
  // the cloud auto-clears a satisfied forced flag on the version report. Queue-full: retry in 60 s (a duplicate report is harmless).
  static unsigned long lastForcedRecheck = 0;
  if (otaCheckDone && !demoSeal && millis() - lastForcedRecheck >= FORCED_RECHECK_INTERVAL_MS
      && currentMode == MODE_CLIENT && WiFi.status() == WL_CONNECTED && isRegistered
      && currentPartitionType != 0 && !core0Busy && !otaInProgress
      && fieldCutSettled(60000) && WiFi.RSSI() >= -80) {
    HttpsRequest verReq = { .type = HTTPS_UPDATE_FW_VERSION };
    HttpsRequest forcedReq = { .type = HTTPS_CHECK_FORCED_UPDATE };
    if (xQueueSend(httpsQueue, &verReq, 0) == pdTRUE
        && xQueueSend(httpsQueue, &forcedReq, 0) == pdTRUE) {
      lastForcedRecheck = millis();
    } else {
      lastForcedRecheck = millis() - FORCED_RECHECK_INTERVAL_MS + 60000UL;
    }
  }
  // // === END OTA UPDATE ===

  // Cloud half of the tach-rescale wipe. Retried until it returns 200; front sync stays suppressed
  // meanwhile, so a device rescaled offline can never have its old-scaled front shipped back to it.
  static unsigned long lastRpmAxisWipeTry = 0;
  if (rpmAxisWipePending && !demoSeal && currentMode == MODE_CLIENT && WiFi.status() == WL_CONNECTED
      && isRegistered && !core0Busy && fieldCutSettled(60000) && WiFi.RSSI() >= -80
      && (lastRpmAxisWipeTry == 0 || millis() - lastRpmAxisWipeTry > 60000)) {
    lastRpmAxisWipeTry = millis();
    HttpsRequest req = { .type = HTTPS_RESET_RPM_AXIS };
    xQueueSend(httpsQueue, &req, 0);  // queue-full → next retry picks it up
  }

  // Cloud half of the alt-health Start Over — same contract: the cloud rebuilds the front from every raw
  // point it holds, so until the device's rows are gone a sync-back would hand the cleared record book back.
  static unsigned long lastAltWipeTry = 0;
  if (altCloudWipePending && !demoSeal && currentMode == MODE_CLIENT && WiFi.status() == WL_CONNECTED
      && isRegistered && !core0Busy && fieldCutSettled(60000) && WiFi.RSSI() >= -80
      && (lastAltWipeTry == 0 || millis() - lastAltWipeTry > 60000)) {
    lastAltWipeTry = millis();
    HttpsRequest req = { .type = HTTPS_RESET_ALT_HEALTH };
    xQueueSend(httpsQueue, &req, 0);
  }

  // Cloud half of a boat-performance Clear All or records import: same contract as the alt-health wipe above.
  static unsigned long lastPerfWipeTry = 0;
  if (perfCloudWipePending && !demoSeal && currentMode == MODE_CLIENT && WiFi.status() == WL_CONNECTED
      && isRegistered && !core0Busy && fieldCutSettled(60000) && WiFi.RSSI() >= -80
      && (lastPerfWipeTry == 0 || millis() - lastPerfWipeTry > 60000)) {
    lastPerfWipeTry = millis();
    HttpsRequest req = { .type = HTTPS_RESET_BOAT_PERF };
    xQueueSend(httpsQueue, &req, 0);
  }

  // === ADMIN CONFIG PUSH: one-shot pending-config check after the OTA checks ===
  // Boot-only, like the forced-update check. Fires once after otaCheckDone so it doesn't
  // compete with the depth-2 httpsQueue; retries next loop if the queue is momentarily full.
  if (!pendingConfigCheckDone && otaCheckDone && !demoSeal && millis() > 6000) {
    if (currentMode == MODE_CLIENT && WiFi.status() == WL_CONNECTED && isRegistered && !core0Busy && fieldCutSettled(0) && WiFi.RSSI() >= -80) {
      HttpsRequest req = { .type = HTTPS_GET_PENDING_CONFIG };
      if (xQueueSend(httpsQueue, &req, 0) == pdTRUE) pendingConfigCheckDone = true;
    } else if (currentMode != MODE_CLIENT) {
      pendingConfigCheckDone = true;   // not a cloud client — nothing to pull
    }
  }

  esp_task_wdt_reset();              // Feed the watchdog to prevent timeout
  currentTime = millis();

  bhServiceCompletion();  // Battery Health: runs the DCIR fit once a test finishes / aborts on timeout
  cvpfServiceCompletion();  // CV plant fit: runs the gain fit once the step sequence finishes / aborts on timeout

  // SOC and runtime update every 2 seconds (runs regardless of hardwarePresent); held while demo exit reloads these totals on core 0
  if (currentTime - lastSOCUpdateTime >= SOCUpdateInterval && !demoStoresBusy()) {
    CurrentSessionDuration = (millis() - sessionStartTime) / 1000;  // seconds
    elapsedMillis = currentTime - lastSOCUpdateTime;
    lastSOCUpdateTime = currentTime;
    UpdateEngineRuntime(elapsedMillis);  // untimed: negligible 2s-cadence arithmetic
    UpdateEngineFuel(elapsedMillis);
    TIMED_CALL(ft_UpdateBatterySOC, UpdateBatterySOC(elapsedMillis));  // always runs: alternator stats + AvgVoltage are shunt-independent; Bcur-dependent sections gate on HAS_BATT_SHUNT internally
    UpdateTravelStatistics(elapsedMillis);
    UpdateBoardTempPressureMaximums();
    handleAltZeroReset();   // do the dynamic udpates
    // fieldCutSettled (hardware gate), never the duty-based fieldOffSettled: that reads "off" during a live duty-0 CV hold and let
    // NVS/LittleFS commits land on live control passes.
    TIMED_CALL(ft_kneeLearnService, kneeLearnService(fieldCutSettled(2000) && !demoSeal));  // Auto Min% learning: NVS flush of learned floors
    if (fieldCutSettled(2000) && !demoSeal) TIMED_CALL(ft_bhFlushCapNVS, bhFlushCapNVS());  // Battery Health persist
    if (fieldCutSettled(2000) && !demoSeal) overheatHistFlush();  // overheat-history NVS persist — dirty-flagged at overheat entry

    // Baro history sampler, 10-min cadence; skipped while BMP388 is NAN. Epoch stamped only if timeIsSynced, so after a cold boot the
    // loaded NVS epoch lets the JS detect the power-off gap and grey out those slots.
    // In demo the ring is the demo copy (9_demo.ino), so the made-up week continues with the simulated barometer.
    if ((hardwarePresent == 1 || demoPhase == DEMO_ON) && baroPressureHistory && (currentTime - lastBaroSampleMs >= BARO_SAMPLE_INTERVAL_MS)) {
      lastBaroSampleMs = currentTime;
      if (!isnan(baroPressure)) {
        baroPressureHistory[baroHistoryHead] = (uint16_t)(baroPressure * 10.0f + 0.5f);
        baroHistoryHead = (baroHistoryHead + 1) % BARO_HISTORY_SIZE;
        if (demoPhase == DEMO_ON) baroHistoryLastEpoch = demoNowEpoch();   // the demo's clock, synced or not (its seed ends at entry)
        else if (timeIsSynced) baroHistoryLastEpoch = timeBase + (currentTime - timeBaseMillis) / 1000;
      }
    }

    // Long-term ring → flash only while the field gate is physically cut (fieldCutSettled, never fieldOffSettled). Deferring across a
    // long run costs only power-cut durability (the 72 h reboot and shutdown paths dump it). Dumps on the cut-settled rising edge and
    // periodically after; prev_longTermHead skips a dump when nothing changed.
    static bool prevLongTermFieldOff = false;
    static unsigned long lastLongTermDumpMs = 0;
    bool ltFieldOff = fieldCutSettled(0);
    bool ltRisingEdge = (ltFieldOff && !prevLongTermFieldOff);
    bool ltPeriodic   = (ltFieldOff && (millis() - lastLongTermDumpMs >= LONGTERM_DUMP_INTERVAL_MS));
    if ((ltRisingEdge || ltPeriodic) && prev_longTermHead != longTermHead) {
      TIMED_CALL(ft_dumpLongTermRing, dumpLongTermRing());  // append-only in the common case; ~250ms spike only on a rare compaction
      lastLongTermDumpMs = millis();
    }
    prevLongTermFieldOff = ltFieldOff;
    // Fast alt-current disturbance matrix → flash, same field-off policy as the ring
    // above (banks during charging, flushes when safe). Timed: it's a flash writer.
    TIMED_CALL(ft_faMatrixFlush, faMatrixMaybeFlush());
  }
  TIMED_CALL(ft_zeroLogService, zeroLogService());  // Zero-drift diagnostic: sample (field-off >=5s; 1s spinning / 10min idle) + field-off-only flash flush. Cheap unless flushing.
  altZeroCaptureService();  // untimed: a running mean at 10 Hz, a flag check when no zero request is in flight
  calculateChargeTimes();  // untimed: negligible arithmetic. Might want to move into the 2s block above and unthrottle later
  // Fast alt-current channel: bounded DMA drain (~1 ms cap), out-of-band of all control (sampling is hardware-timed; this only empties
  // the driver pool). Runs engine-off too; self-suspends when the channel reads railed (broken/missing jumper).
  TIMED_CALL(ft_fastAltDrain, faDrain());
  // Failure-detector verdict consume: the analysis runs on Core-0 faDetTask (on Core 1 it stalled the loop ~30 ms once a minute); this
  // only consumes the result + fires side-effects, so ft_faDetector reads ~0.
  TIMED_CALL(ft_faDetector, faDetectorPoll());
  // No periodic NVS save here: nvs_commit() can block Core 1 for hundreds of ms during sector erase, colliding with the voltage loop
  // (OV risk). NVS persists only at the field-off edge and shutdown. Deferred Core-0 button saves run here on Core 1 so SSE isn't
  // blocked; no electrical-zone gate because every flag is set only by a manual UI action.
  if (pendingSaveCVTuningLog) {
    pendingSaveCVTuningLog = false;
    saveCVTuningLog();
  }
  if (pendingSaveTuningLog) {
    pendingSaveTuningLog = false;
    saveTuningLog();
  }
  if (pendingSaveSystemIDLog) {
    pendingSaveSystemIDLog = false;
    saveSystemIDLog();
  }
  if (pendingSaveSysidSweepLog) {
    pendingSaveSysidSweepLog = false;
    saveSysidSweepLog();
  }
  if (pendingSaveTuningSweepLog) {
    pendingSaveTuningSweepLog = false;
    saveTuningSweepLog();
  }
  if (cxLedgerFlushPending) {  // commissioning-ledger rows: PSRAM staging → /cxledger.bin
    cxLedgerFlushPending = false;
    cxLedgerFlush();
  }
  if (pendingRpmAxisWipe) {  // must run BEFORE the flags it raises, so they drain in this same pass
    pendingRpmAxisWipe = false;
    rpmAxisWipeExecute();
  }
  if (pendingImportBootAct) {  // same ordering reason
    uint16_t act = pendingImportBootAct;
    pendingImportBootAct = 0;
    importBootActRun(act);
  }
  if (pendingResetAlternatorHealth) {
    pendingResetAlternatorHealth = false;
    resetAlternatorHealth();
  }
  altHealthWindingCheck();   // a move of the winding the field settings run on (fieldWindApplied) is a user action too
  if (pendingResetBoatPerformance) {
    pendingResetBoatPerformance = false;
    resetBoatPerformance();
  }
  if (pendingClearOverheatHistory) {
    pendingClearOverheatHistory = false;
    clearOverheatHistoryAction();
  }
  if (pendingSaveUserTableEdits) {
    pendingSaveUserTableEdits = false;
    saveUserTableEdits();
  }
  capBothPersistService();  // 6_functions.ino: one staged cap-table blob per pass, no-op when idle
  if (pendingSaveVesselInfo) {
    pendingSaveVesselInfo = false;
    saveVesselInfoToNvs();
  }
  // ========== POWER MANAGEMENT: Handle ignition state and WiFi wake mode ==========
  // This runs BEFORE the mode switch to ensure WiFi is in correct state before attempting transmission
  // Power management affects AP and CLIENT modes, but NOT CONFIG mode (CONFIG mode exits early below)
  if (currentMode != MODE_CONFIG) {
    static int lastIgnitionState = 1;            // Previous ignition state; starts ON, so a key-off boot takes the OFF edge
    static bool lastWifiWakeActive = false;      // Track previous WiFi wake mode state
    static bool wakeExpiryWarningShown = false;  // Track if we've warned about wake mode expiring

    if (Ignition == 0) {
      // ===== IGNITION OFF =====
      // Edge detection FIRST — must set pendingShutdownFlush before the state machine checks it
      if (lastIgnitionState == 1) {
        pendingShutdownFlush = true;
        Serial.println("Ignition OFF");
        lastIgnitionState = 0;
      }
      wifiWakeActive = (wifiWakeStart > 0 && (millis() - wifiWakeStart) < WIFI_WAKE_DURATION);
      // Detect state change for WiFi wake mode
      if (wifiWakeActive != lastWifiWakeActive) {
        if (wifiWakeActive) {
          Serial.println("WiFi Wake Mode ACTIVATED (GPIO5 triggered)");
        } else {
          Serial.println("WiFi Wake Mode DEACTIVATED - Entering low power mode");
          wakeExpiryWarningShown = false;  // Reset warning flag for next wake cycle
        }
        lastWifiWakeActive = wifiWakeActive;
      }
      if (wifiWakeActive) {
        // Wake mode active - full power for user monitoring
        setCpuFrequencyMhz(240);
        if (wifiNapActive) {       // was napping — radio is associated, just exit modem sleep
          WiFi.setSleep(false);
          wifiNapActive = false;
        }
        if (WiFi.getMode() == WIFI_OFF) {  // WiFi was turned off, need to reconnect
          setupWiFi();
        }
        // Warn user before timeout expires
        if ((WIFI_WAKE_DURATION - (millis() - wifiWakeStart)) < 20000 && !wakeExpiryWarningShown) {
          queueConsoleMessage("WiFi wake mode expiring in 20 seconds - press button to extend");
          wakeExpiryWarningShown = true;
        }
      } else {
        if (pendingShutdownFlush) {
          if (!gpio4IsLow) {
            // Phase 1: field still ramping — hold 240MHz and WiFi until it cuts
            setCpuFrequencyMhz(240);
          } else if (!shutdownNVSFlushDone) {
            // Phase 2: field just cut — flush NVS and save partial sensor window immediately
            setCpuFrequencyMhz(240);
            saveNVSDataFull();  // absolute latest values, no critical-zone gate
            altHealthSave();
            boatPerfSave();
            uploadSensorHistory();  // save whatever is in the current window to local buffer
            if (pendingSaveCVTuningLog) {
              pendingSaveCVTuningLog = false;
              saveCVTuningLog();
            }
            if (pendingSaveTuningLog) {
              pendingSaveTuningLog = false;
              saveTuningLog();
            }
            if (pendingSaveSystemIDLog) {
              pendingSaveSystemIDLog = false;
              saveSystemIDLog();
            }
            if (pendingSaveSysidSweepLog) {
              pendingSaveSysidSweepLog = false;
              saveSysidSweepLog();
            }
            if (pendingSaveTuningSweepLog) {
              pendingSaveTuningSweepLog = false;
              saveTuningSweepLog();
            }
            if (pendingRpmAxisWipe) {  // before the flags it raises, so they drain in this same pass
              pendingRpmAxisWipe = false;
              rpmAxisWipeExecute();
            }
            if (pendingImportBootAct) {
              uint16_t act = pendingImportBootAct;
              pendingImportBootAct = 0;
              importBootActRun(act);
            }
            if (pendingResetAlternatorHealth) {
              pendingResetAlternatorHealth = false;
              resetAlternatorHealth();
            }
            altHealthWindingCheck();
            if (pendingResetBoatPerformance) {
              pendingResetBoatPerformance = false;
              resetBoatPerformance();
            }
            if (pendingClearOverheatHistory) {
              pendingClearOverheatHistory = false;
              clearOverheatHistoryAction();
            }
            if (pendingSaveUserTableEdits) {
              pendingSaveUserTableEdits = false;
              saveUserTableEdits();
            }
            if (pendingSaveVesselInfo) {
              pendingSaveVesselInfo = false;
              saveVesselInfoToNvs();
            }
            shutdownNVSFlushDone = true;
            shutdownCloudDeadlineMs = millis() + 1800000;  // 30-min window: fieldCutSettled() gates fire at 60-80s after the field is cut; 30 min gives full time for NTP, uploads, weather, and buffer drain
          } else if (shutdownCloudDeadlineMs && (int32_t)(millis() - shutdownCloudDeadlineMs) < 0) {  // rollover-safe "now < deadline"
            // Phase 3: hold 240MHz and WiFi for the full 30-min window unconditionally.
            // Lets the CloudFeatures block continue uploading, and keeps WiFi open to verify the drain.
            setCpuFrequencyMhz(240);
          } else {
            // Phase 4: drain window expired: bulk-dump unsent ring records to LittleFS so they survive power-down (setup() restores
            // them). Blocking on flash is fine, the device is about to sleep.
            if (!ringIsEmpty()) {
              Serial.printf("Shutdown Phase 4: ring still has %u records — dumping to LittleFS\n",
                            (unsigned)sensorRingCount);
              dumpSensorRingToLittleFS();
            }
            TIMED_CALL(ft_dumpLongTermRing, dumpLongTermRing());  // durable month-long plot cache → flash (final)
            dumpUsageAccum();  // app-usage counters survive a breaker cut during standby
            solarLedgerFlushNow();  // ledger + live day + learned ratio survive the same cut
            pendingShutdownFlush = false;
            shutdownNVSFlushDone = false;
            shutdownCloudDeadlineMs = 0;
          }
        } else if (!core0Busy && uxQueueMessagesWaiting(httpsQueue) == 0) {
          // Queue drained — go low power (naps WiFi instead of full-off when enabled in Client mode)
          queueDrainHoldStart = 0;
          enterLowPowerStandby();
        } else {
          // Upload in flight: hold 240MHz/WiFi-on, capped at 5 min so a stuck upload can't hold the device at high power indefinitely
          if (queueDrainHoldStart == 0) queueDrainHoldStart = millis();
          if (millis() - queueDrainHoldStart > 300000UL) {
            queueDrainHoldStart = 0;
            queueConsoleMessage("Upload drain timeout - forcing low power");
            enterLowPowerStandby();
          }
          // else: within 5-min window — retry next tick
        }
      }

    } else {
      // ===== IGNITION ON - Normal operation =====
      // Detect ignition state change to ON
      if (lastIgnitionState != 1) {
        pendingShutdownFlush = false;  // ignition back on — cancel any pending flush
        shutdownNVSFlushDone = false;
        shutdownCloudDeadlineMs = 0;
        queueDrainHoldStart = 0;
        Serial.println("Ignition ON - Normal operation mode");
        lastIgnitionState = 1;
        // Edge only: rewriting the heartbeat every pass with ignition on would hide a hung TempTask while motoring.
        if (tempTaskHandle != NULL) lastTempTaskHeartbeat = millis();  // restart the 20s window so the health check doesn't trip before the task posts its first beat
      }
      wifiWakeStart = 0;               // Clear wake mode
      lastWifiWakeActive = false;      // Reset wake mode tracking
      wakeExpiryWarningShown = false;  // Reset warning
      setCpuFrequencyMhz(240);         // Ensure full speed
      if (tempTaskHandle != NULL) {
        vTaskResume(tempTaskHandle);  // RESUME BACKGROUND TASKS AFTER SPEEDING UP CPU
        if (tempTaskSuspended) { tempTaskSuspended = false; lastTempTaskHeartbeat = millis(); }
      }
      if (wifiNapActive) {       // was napping — radio is associated, just exit modem sleep
        WiFi.setSleep(false);
        wifiNapActive = false;
      }
      // Reconnect WiFi if needed (works for both AP and CLIENT modes)
      if (WiFi.getMode() == WIFI_OFF) {
        setupWiFi();  // Reconnects in current mode
      }
    }
  }
  // ========== END POWER MANAGEMENT ==========

  // ========== MODE HANDLING: Switch statement runs for all modes ==========
  switch (currentMode) {
    case MODE_CONFIG:
      // Configuration mode - WiFi setup ONLY, no alternator operation
      // This is intentional for safety: user must configure WiFi before alternator use
      // EMERGENCY ACCESS: Hold GPIO46 LOW during boot to enter AP mode (full functionality without credentials)
      dnsHandleRequest();  // Process captive portal DNS requests
        // Throttle this later , it's not so trivial    Serial.println("You're in Config Mode, connect to the alternator's hotspot and enter credentials");
      return;  // Exit loop() entirely - prevents alternator operation without proper configuration
    case MODE_AP:
      dnsHandleRequest();  // Need DNS for captive portal in AP mode
      // Fall through to full functionality (no break statement - intentional)

    case MODE_CLIENT:
      // Full sensor/data functionality (also runs for MODE_AP due to fall-through above)

      // ========== HARDWARE-DEPENDENT SECTION ==========
      demoService();  // demo-mode entry / exit (9_demo.ino), ahead of the read so the switch lands on a whole pass
      // Ahead of the hardwarePresent branch: sim mode still drives the real field, so the comparator edit must land there too
      if (inaOvReprogramPending) { inaOvReprogramPending = false; updateINA228OvervoltageThreshold(); }
      // Read sensors: Real hardware or fake data depending on hardwarePresent flag
      if (hardwarePresent == 1) {
        checkTempTaskHealth();  // digital temperature measurement monitor (only with real hardware)

        TIMED_CALL(ft_ReadAnalogInputs, ReadAnalogInputs());
      } else {
        TIMED_CALL(ft_ReadAnalogInputs, demoReadSensors());  // simulated boat at the real sensor cadences; no I2C, no delay
      }

      // ========== END HARDWARE-DEPENDENT SECTION ==========
      // All remaining code runs regardless of hardwarePresent flag
      // This allows full development/testing even with broken hardware
      TIMED_CALL(ft_calculateDerivedMetrics, calculateDerivedMetrics());  // Calculate true wind, leeway, VMG, duty cycles
      if (VeData == 1 && hardwarePresent == 1) {
        TIMED_CALL(ft_ReadVEData, ReadVEData());
        VeTime = ft_ReadVEData.worstWindow;
        VeTime2 = ft_ReadVEData.worstSession;
      }
      // Outside the NMEA0183Data gate: a baud/polarity change made while the receiver is off must
      // still land, so the port is right the moment it is switched back on.
      if (n183ReconfigPending && hardwarePresent == 1) { n183ReconfigPending = false; applyNMEA0183Serial(); }
      if (NMEA0183Data == 1 && hardwarePresent == 1) TIMED_CALL(ft_ReadNMEA0183, ReadNMEA0183Data());  // deadline-capped inside; see the HARD REAL-TIME CONTRACT note on the function
      // Ahead of the gate: switching transmit off must still reach the library when nothing else keeps ParseMessages running.
      if (n2kNodeReconfigPending && hardwarePresent == 1) { n2kNodeReconfigPending = false; n2kApplyNodeConfig(true); }
      // n2kTxEnable / rvcTxEnable force ParseMessages (a claiming node needs address claim + heartbeat servicing), and so does
      // dvccEn (the follow must keep hearing the authority), even with the receive toggle off.
      if (n2kBusRunning()) {
        n2kPassMs = millis();
        TIMED_CALL(ft_n2kParse, NMEA2000.ParseMessages());  // CAN bus (only with real hardware)
        TIMED_CALL(ft_n2kTx, nmea2kTransmitTick());
      }
      n2kBusTick();  // 1 Hz self-gated, runs with the bus off too: verdict 0, recording stop, ring free
      // UNCONDITIONAL (pure 1 Hz arithmetic): if the gate above stops (e.g. hardwarePresent flipped to 0 mid-FOLLOW), the silence
      // timeout still reverts the clamps instead of the control loop obeying a frozen CVL/CCL forever.
      TIMED_CALL(ft_dvcc, dvccTick());
      TIMED_CALL(ft_CheckAlarms, CheckAlarms());  // Process alarms (runs with fake or real data)
      if (!demoStoresBusy()) calculateThermalStress();  // alternator lifetime modeling; paused while demo exit reloads the wear totals
      //UpdateDisplay();
      if (!demoSeal) zeroFitService();  // Temp-comp zero correction: daily fit (engine+field off) + live correction (must be before AdjustField)

      // Runs in every mode: the stale-forecast release must fire with no internet, or a solar pause holds the field off forever.
      // The fetch itself is gated on client mode + WiFi inside the call.
      updateWeatherMode();  // untimed: core-1 cost is local analysis only; fetch is queued to Core 0
      TIMED_CALL(ft_solarLedger, solarLedgerService());  // 1 Hz local-day roll of the solar ledger; flash only at field-cut-settled
      TIMED_CALL(ft_AdjustFieldLearnMode, AdjustFieldLearnMode());
      // While the field is down, drop the PID-firing and INA228 previous-sample timestamps so the first sample after re-enable
      // measures a fresh interval, not the off-gap. The INA reset also covers a field-off flash stall landing before
      // inaFastModeActive flips to slow mode (else logged as a field-on read interval).
      if (gpio4IsLow) { pfHasPrev = false; inaResetIntervalBaseline(); }
      TIMED_CALL(ft_huntGov, runHuntGovernor());  // hunt-detector evaluation (self-gated to every 32 control ticks; never inside the control call)
      TIMED_CALL(ft_altHealth, altHealth_tick(millis()));
      // Gate-tuning session logger: 10 Hz rows plus a per-pass accumulator of fresh CH1 conversions. Called every pass (not from the
      // fold) so a field-off stretch logs as field-off, not a hole. Idle cost: one compare.
      altLog_tick(millis());
      TIMED_CALL(ft_boatPerf, boatPerf_tick(millis()));   // Phase 3 boat performance (timing exposed via PerfLive registry)
      if (hardwarePresent == 1) drainIMUFifo();  // skip in fake mode — no hardware means 15ms I2C timeout per call floods the loop

      // Sync legacy display variables
      totalSafeHours = (float)totalSafeMs / 3600000.0f;  // Fractional hours for UI
      // Wrap-safe remaining time calculation
      if (overheatPenaltyEndMs > 0) {
        uint32_t now = millis();
        int32_t remaining = (int32_t)(overheatPenaltyEndMs - now);
        if (remaining > 0) {
          overheatingPenaltyTimer = (uint32_t)remaining;
        } else {
          overheatingPenaltyTimer = 0;
        }
      } else {
        overheatingPenaltyTimer = 0;
      }
      TIMED_CALL(ft_logDashboardValues, logDashboardValues());            //  nice to have some history in the Console
      TIMED_CALL(ft_updateSystemHealthStats, updateSystemHealthStats());  // samples CPU load + heap stats into globals for CSVData2
      resolveSources();                                                   // GPS/time source arbitration: promote phone fallback, demote stale labels
      TIMED_CALL(ft_updateSensorWindow, updateSensorWindow());            // Update sensor aggregation (after sensor reads)
      TIMED_CALL(ft_updateAccelMetrics, updateAccelMetrics());            // accel always on

      // ===== FIELD-OFF NVS DRAIN (+20 s / +28 s after the cut, once per field-off window) =====
      // Keyed on gpio4IsLow (the physical gate), NOT duty-derived fieldActiveStatus, which reads "off" during a live duty-0 CV hold
      // and let the heavy NVS commit and ~67 KB alt-health/boat-perf writes fire with the control loop live. Thresholds and the
      // 20 s fast-OV-lockout rationale live with the flag declarations
      // (fieldOffFlushDone block above). Skips when pendingShutdownFlush owns the
      // ignition-off sequence.
      {
        bool fieldNowActive = !gpio4IsLow;
        if (lastFieldStateForFlush == 1 && !fieldNowActive) {
          fieldOffEdgeMs = millis();  // capture the field-on -> field-off edge
        } else if (lastFieldStateForFlush == 0 && fieldNowActive) {
          fieldOffFlushDone = false;        // re-arm on field-on edge
          fieldOffMatrixFlushDone = false;
        } else if (lastFieldStateForFlush == -1 && !fieldNowActive) {
          fieldOffEdgeMs = millis();  // first observation; field already off
        }
        lastFieldStateForFlush = fieldNowActive ? 1 : 0;

        // Gate 1 (+20 s — 2x the fast-OV lockout cap): drain storage namespace to NVS
        if (!fieldOffFlushDone && !pendingShutdownFlush && !fieldNowActive && !demoSeal
            && fieldOffEdgeMs > 0 && (millis() - fieldOffEdgeMs) >= 20000UL) {
          saveNVSDataFull();
          fieldOffFlushDone = true;
        }
        // Gate 2 (+28 s): write performance matrix to LittleFS — staggered so
        // the ~150 ms file write doesn't land on the NVS commit's relocation tail
        if (!fieldOffMatrixFlushDone && !pendingShutdownFlush && !fieldNowActive && !demoSeal
            && fieldOffEdgeMs > 0 && (millis() - fieldOffEdgeMs) >= 28000UL) {
          altHealthSave();
          boatPerfSave();
          fieldOffMatrixFlushDone = true;
        }
        // Gate 3 (engine stopped 1 h, ignition still on): the two saves that otherwise wait for a key-off
        // (shutdown Phase 4) or the 72 h maintenance restart, for a boat whose ignition never goes off
        // (IgnitionOverride, or a key left on). Once per stop; the blocking ring dump is Phase 4's own.
        static uint32_t engineStoppedAtMs = 0;
        static bool restSaveDone = false;
        if (g_engineSpinningNow || fieldNowActive) {
          engineStoppedAtMs = 0;
          restSaveDone = false;
        } else if (engineStoppedAtMs == 0) {
          engineStoppedAtMs = millis() ? millis() : 1;
        }
        if (!restSaveDone && Ignition == 1 && engineStoppedAtMs != 0 && !demoSeal
            && (uint32_t)(millis() - engineStoppedAtMs) >= 3600000UL) {
          if (!ringIsEmpty()) dumpSensorRingToLittleFS();
          dumpUsageAccum();
          restSaveDone = true;
        }
      }

      // Local long-term accumulation runs regardless of CloudFeatures so the 30-day ring fills in AP mode / no-WiFi; only the
      // cloud uploads below are gated. uploadSensorHistory() self-skips during OTA.
      if (!otaInProgress) {
        unsigned long nowLocalAccum = millis();
        if (nowLocalAccum - lastSensorUploadTime >= SENSOR_UPLOAD_INTERVAL) {
          esp_task_wdt_reset();
          UpdateSailingMetrics(SENSOR_UPLOAD_INTERVAL);  // untimed: negligible arithmetic
          lastSensorUploadTime = nowLocalAccum;
          TIMED_CALL(ft_uploadSensorHistory, uploadSensorHistory());
          esp_task_wdt_reset();
        }
      }

      ntpPoll();  // outside the cloud and field-cut gates: no network or flash work, and the boot request needs it too
      if (CloudFeatures == 1) {
        if (otaInProgress) {
          return;  // Skip during OTA
        }
        unsigned long currentMillisz = millis();
        // Check time sync every 12 hours. Every internet op here waits on fieldCutSettled (hardware cut), never the duty-based
        // fieldOffSettled: no upload or fetch with the field on unless a button asked. The extra over the 20 s floor sets each op's delay.
        if (fieldCutSettled(40000)) TIMED_CALL(ft_checkTimeSync, checkTimeSync());

        // Buffered upload every BUFFER_UPLOAD_INTERVAL with the field cut 70 s (20 s floor + 50 s), so its TLS handshake misses the
        // field-off NVS drain at 20 s. "Upload Cloud Now" (forceCloudFlushPending) shortens that to the 20 s floor and bypasses the
        // 13 s throttle (still rate-limited by the HTTPS queue depth on Core 0).
        bool flushBypass = forceCloudFlushPending;
        if ((flushBypass ? fieldCutSettled(0) : fieldCutSettled(50000))
            && (flushBypass || currentMillisz - lastBufferUploadAttempt >= BUFFER_UPLOAD_INTERVAL - 7)) {
          lastBufferUploadAttempt = currentMillisz;
          if (bufferedRecordCount > 0) {
            esp_task_wdt_reset();                                           // Feed before upload
            TIMED_CALL(ft_uploadBufferedRecords, uploadBufferedRecords());  // times JSON build + queue send; HTTP transfer is on core 0 and not captured here
            esp_task_wdt_reset();                                           // Feed after upload completes
          } else if (flushBypass) {
            // Ring empty — force-flush goal met, clear the flag.
            forceCloudFlushPending = false;
            queueConsoleMessage("Cloud sync: forced flush complete (queue empty)");
          }
        }
        
        // Configuration Snapshot, field cut 70 s; sim mode (HardwarePresent=0) skips — never push fake data. Fires on the 24 h
        // backstop or configSnapshotRequested, at most once per minute so a failing gate can't spin, and behind configSnapshotRetryMs.
        if (hardwarePresent == 1 && !demoSeal && fieldCutSettled(50000)
            && (millis() - lastConfigSnapshotTime >= CONFIG_SNAPSHOT_INTERVAL
                || (configSnapshotRequested
                    && millis() - lastConfigSnapshotTime >= max(configSnapshotRetryMs, 60000UL)))) {
          lastConfigSnapshotTime = millis();
          // The 24 h backstop becomes a request too, so one that finds no WiFi retries on the 60 s throttle
          // instead of burning the day's attempt. Cleared below once a payload is queued.
          configSnapshotRequested = true;
          if (currentMode == MODE_CLIENT && WiFi.status() == WL_CONNECTED && isRegistered) {
            if (WiFi.RSSI() >= -80) {
              bool _configBuilt;
              TIMED_CALL(ft_buildConfigPayload, _configBuilt = buildConfigPayload());  // time the payload build separately from the queue send
              if (_configBuilt) {
                HttpsRequest req = {};
                req.type = HTTPS_UPLOAD_CONFIG;
                req.payloadCap = CONFIG_PAYLOAD_SIZE;            // full config
                req.payload = (char *)ps_malloc(req.payloadCap);
                if (req.payload) {
                  strncpy(req.payload, configPayloadBuffer, req.payloadCap - 1);
                  req.payload[req.payloadCap - 1] = '\0';
                  if (xQueueSend(httpsQueue, &req, 0) == pdTRUE) {
                    configSnapshotRequested = false;
                  } else {
                    free(req.payload);
                    queueConsoleMessage("Config snapshot: HTTPS queue full, will retry");
                  }
                }
              }
            }
          }
        }

        // Boat-performance aggregates → cloud (field-off gated; sim never uploads).
        // rpmAxisWipePending / perfCloudWipePending: the upload RESPONSE carries the cloud-rebuilt front, which would
        // overwrite the local wipe or import with the points the cloud still holds. Stay silent until the wipe lands.
        // One PSRAM block per upload carries the batch out and the cloud's front back, freed once Core 1 applies it. Boat and alt
        // take turns (frontSyncTurnFree), so the peak is the larger block, not the sum; a turn not taken leaves the timer due.
        if (hardwarePresent == 1 && !demoSeal && !rpmAxisWipePending && !perfCloudWipePending && fieldCutSettled(50000) && millis() - lastBoatPerfUploadTime >= BOATPERF_UPLOAD_INTERVAL
            && frontSyncTurnFree()) {
          lastBoatPerfUploadTime = millis();
          if (currentMode == MODE_CLIENT && WiFi.status() == WL_CONNECTED && isRegistered && WiFi.RSSI() >= -80) {
            size_t need = perfSyncBufBytes();   // 0 = nothing pending: no allocation
            if (need) {
              HttpsRequest req = {};
              req.type = HTTPS_UPLOAD_BOATPERF;
              req.payloadCap = need;
              req.payload = (char *)ps_malloc(need);
              syncAllocNote(SYNC_NOTE_BOAT_OUT, req.payload != nullptr, need);
              if (req.payload && buildBoatPerfPayload(req.payload, req.payloadCap)) {
                if (xQueueSend(httpsQueue, &req, 0) != pdTRUE) {
                  free(req.payload);
                  queueConsoleMessage("BoatPerf upload: HTTPS queue full, will retry next interval");
                }
              } else if (req.payload) {
                free(req.payload);   // nothing to send (build returned false) → release the buffer
              }
            }
          }
        }

        // Alternator-health records → cloud (field-off gated; sim never uploads): learned with the field ON, banked, flushed at the
        // next field-off. altSyncBufBytes() is 0 when nothing new was banked, so that interval allocates nothing.
        if (hardwarePresent == 1 && !demoSeal && !rpmAxisWipePending && !altCloudWipePending && fieldCutSettled(50000) && millis() - lastAltHealthUploadTime >= ALTHEALTH_UPLOAD_INTERVAL
            && frontSyncTurnFree()) {
          lastAltHealthUploadTime = millis();
          size_t need = (currentMode == MODE_CLIENT && WiFi.status() == WL_CONNECTED && isRegistered && WiFi.RSSI() >= -80) ? altSyncBufBytes() : 0;
          if (need) {
            HttpsRequest req = {};
            req.type = HTTPS_UPLOAD_ALTHEALTH;
            req.payloadCap = need;
            req.payload = (char *)ps_malloc(need);
            syncAllocNote(SYNC_NOTE_ALT_OUT, req.payload != nullptr, need);
            if (req.payload && buildAltHealthPayload(req.payload, req.payloadCap)) {
              if (xQueueSend(httpsQueue, &req, 0) != pdTRUE) {
                free(req.payload);
                queueConsoleMessage("AltHealth upload: HTTPS queue full, will retry next interval");
              }
            } else if (req.payload) {
              free(req.payload);   // nothing to send (build returned false) → release the buffer
            }
          }
        }

        // App-usage analytics: one POST per LOCAL day (usageTzOffsetS, wall-clock, so restarts can't stretch or skip a period),
        // retried at most once a minute; an empty period just rolls. Reset at queue time, so a later send failure loses that day
        // (lossy by design); UsageDays_AllTime counts DELIVERED uploads. fieldCutSettled, NOT fieldOffSettled: the roll writes
        // /usage.bin, so charging across midnight defers it to the first field-off minute.
        if (hardwarePresent == 1 && !demoSeal && fieldCutSettled(10000)
            && millis() - lastUsageUploadAttempt >= 60000) {
          time_t usageNowEp = time(NULL);
          if (usageNowEp > 1700000000LL && usagePeriodStartEpoch > 0
              && (usageNowEp + usageTzOffsetS) / 86400 != (usagePeriodStartEpoch + usageTzOffsetS) / 86400) {
            lastUsageUploadAttempt = millis();
            if (usageSlotCount == 0 && usageOpens == 0) {
              resetUsageAccum(usageNowEp);  // nothing happened this period — just start the next one
              dumpUsageAccum();
            } else if (currentMode == MODE_CLIENT && WiFi.status() == WL_CONNECTED && isRegistered && WiFi.RSSI() >= -80) {
              HttpsRequest req = {};
              req.type = HTTPS_UPLOAD_USAGE;
              req.payloadCap = USAGE_PAYLOAD_SIZE;
              req.payload = (char *)ps_malloc(req.payloadCap);
              if (req.payload && buildUsagePayload(req.payload, req.payloadCap)) {
                if (xQueueSend(httpsQueue, &req, 0) == pdTRUE) {
                  resetUsageAccum(usageNowEp);
                  dumpUsageAccum();  // file must not resurrect the closed period after a reboot
                } else {
                  free(req.payload);
                  queueConsoleMessage("Usage upload: HTTPS queue full, will retry next interval");
                }
              } else if (req.payload) {
                free(req.payload);
              }
            }
          }
        }

        // Commissioning-ledger drain (COMMISSIONING_LEDGER_SPEC.md) → log-commissioning-event. Never during commissioning: rows wait on
        // flash until idle. Network gates live inside the service, flash-safety gates here (the post-send trim is a flash write).
        // fieldCutSettled, NOT the duty-based fieldOffSettled.
        if (hardwarePresent == 1 && !demoSeal && fieldCutSettled(10000)) {
          cxLedgerDrainService();
        }
      }
      // Local field-cut flash pass: outside the Cloud Features block, which AP mode forces off. OTA skips it as the cloud block does.
      if (!otaInProgress && hardwarePresent == 1 && !demoSeal && fieldCutSettled(10000)) {
        huntLedgerService();  // hunt-episode records queued in RAM by the control path → flash, here only
        huntMapService();     // damper pocket map — same rule: flash only here, never in the control path
        owRoleService();      // 1-Wire role auto-binds queued by TempTask (Core 0 never writes NVS) → flash, here only
        if (sensorRingBackupStale) { sensorRingBackupStale = false; sensorRingDropBackupFile(); }  // Phase-4 ring dump outlived by an upload drain (executeUploadPayload flags it): delete here, never on the upload task
      }
      TIMED_CALL(ft_ch1_compute_stats, ch1_compute_stats());
      TIMED_CALL(ft_SendWifiData, SendWifiData());  // Safely sends data (has internal guard to check if WiFi is actually on)

      // Client-specific connection monitoring
      // if (currentMode == MODE_CLIENT) {  // moved the gating check into the checkwificonnection function WAS NOT SUFFICIENT FOR WHATEVER REASON
      // pendingShutdownFlush belongs here: the ignition-off drain keeps the radio up for up to 30 min of uploads without calling
      // enterLowPowerStandby() (wifiNapActive stays false), and the reconnect engine must run through it.
      if (currentMode == MODE_CLIENT && (Ignition == 1 || pendingShutdownFlush || (wifiWakeStart > 0 && (millis() - wifiWakeStart) < WIFI_WAKE_DURATION) || wifiNapActive)) {
        // if (currentMode == MODE_CLIENT && (Ignition == 1 || wifiWakeActive)) { // can't try to do anything wifi related unless ignition is on and clock speed is fast enough to not crash
        TIMED_CALL(ft_checkWiFiConnection, checkWiFiConnection());
        pollWiFiLinkState();  // 3_functions.ino: 5 s status poll; names the drop reason the WiFi event task recorded
      }
      break;  // Exit switch statement, continue with rest of loop()
  }
  // ========== LOOP TIMING METRICS ==========
  if (millis() - prev_millis7888 > AinputTrackerTime) {  // every 5 seconds reset the rolling window metrics
    loopTime5sWindow = 0;                                // rolling 5s loop worst resets here; MaximumLoopTime is session-persistent
    // Reset all per-function rolling windows
    ft_ReadAnalogInputs.worstWindow = 0;
    ft_AdjustFieldLearnMode.worstWindow = 0;
    ft_logDashboardValues.worstWindow = 0;
    ft_updateSystemHealthStats.worstWindow = 0;
    ft_checkWiFiConnection.worstWindow = 0;
    ft_SendWifiData.worstWindow = 0;
    ft_CheckAlarms.worstWindow = 0;
    ft_calculateDerivedMetrics.worstWindow = 0;
    ft_ch1_compute_stats.worstWindow = 0;
    ft_uploadSensorHistory.worstWindow = 0;
    ft_dumpLongTermRing.worstWindow = 0;
    ft_fastAltDrain.worstWindow = 0;
    ft_faMatrixFlush.worstWindow = 0;
    ft_faDetector.worstWindow = 0;
    ft_faWindowFinalize.worstWindow = 0;
    ft_zeroLogService.worstWindow = 0;
    ft_solarLedger.worstWindow = 0;
    ft_bhFlushCapNVS.worstWindow = 0;
    ft_kneeLearnService.worstWindow = 0;
    ft_uploadBufferedRecords.worstWindow = 0;
    ft_buildConfigPayload.worstWindow = 0;
    ft_UpdateBatterySOC.worstWindow = 0;
    ft_updateSensorWindow.worstWindow = 0;
    ft_checkTimeSync.worstWindow = 0;
    ft_rai_total.worstWindow = 0;
    ft_rai_ina228.worstWindow = 0;
    ft_rai_ads_state.worstWindow = 0;
    ft_rai_bmp_state.worstWindow = 0;
    ft_rai_imu.worstWindow = 0;
    ft_updateAccelMetrics.worstWindow = 0;
    ft_ReadVEData.worstWindow = 0;
    ft_ReadNMEA0183.worstWindow = 0;
    ft_altHealth.worstWindow = 0;
    ft_altFold.worstWindow = 0;
    ft_boatPerf.worstWindow = 0;
    ft_altEngine.worstWindow = 0;
    ft_perfEngine.worstWindow = 0;
    ft_n2kTx.worstWindow = 0;
    ft_dvcc.worstWindow = 0;
    ft_n2kParse.worstWindow = 0;
    ft_huntGov.worstWindow = 0;
    loopWorst80Win = 0;  // 80MHz low-power loop worst — rolling 5s window
    loopFieldOnWin = 0;  // field-ON loop worst — rolling 5s window

    prev_millis7888 = millis();
  }
  endtime = esp_timer_get_time();
  LoopTime = (endtime - starttime);
  // Worst-loop attribution: all ft_*.lastCall are THIS pass's durations; ctrl(inclFold) contains fold, and a part larger than total
  // was throttled (stale lastCall). Emitted after endtime so it never counts against the pass. Both serial streams are non-blocking
  // (skipped when the UART TX FIFO is full): (1) a per-event line on every over-threshold pass with the gap since the previous one,
  // throttled to 1 per 100 ms (suppressed passes report as sup=); (2) a self-resetting 30 s window summary (bucket counts + max).
  static uint32_t winStartMs = 0, winN10 = 0, winN15 = 0, winN20 = 0, winMaxUs = 0;
  static uint32_t lastStallMs = 0, lastEvtPrintMs = 0, evtSuppressed = 0;
  static uint32_t winB46 = 0, winB68 = 0, winB810 = 0;  // sub-10ms loop-pass bands — the band that stretches CH1 reads
  uint32_t ltUs = (uint32_t)LoopTime;
  uint32_t nowMsLD = millis();
  if (winStartMs == 0) winStartMs = nowMsLD;
  if (ltUs >= 4000) {
    if (ltUs < 6000) winB46++;
    else if (ltUs < 8000) winB68++;
    else if (ltUs < 10000) winB810++;
  }
  if (ltUs > LOOP_DIAG_THRESH_US) {
    uint32_t gap = lastStallMs ? (nowMsLD - lastStallMs) : 0;
    lastStallMs = nowMsLD;
    winN10++;
    if (ltUs > 15000) winN15++;
    if (ltUs > 20000) winN20++;
    if (ltUs > winMaxUs) winMaxUs = ltUs;
    if (ltUs > loopDiagWorstUs) {           // genuine new worst → classic SSE console line (dashboard)
      loopDiagWorstUs = ltUs;
      char lw[200];
      snprintf(lw, sizeof(lw),
        "LOOP WORST %.1fms | rai=%.1f ctrl(inclFold)=%.1f fold=%.1f health=%.1f boat=%.1f sysHealth=%.1f wifiSend=%.1f",
        ltUs / 1000.0, ft_rai_total.lastCall / 1000.0, ft_AdjustFieldLearnMode.lastCall / 1000.0,
        ft_altFold.lastCall / 1000.0, ft_altHealth.lastCall / 1000.0, ft_boatPerf.lastCall / 1000.0,
        ft_updateSystemHealthStats.lastCall / 1000.0, ft_SendWifiData.lastCall / 1000.0);
      queueConsoleMessage(lw);
    }
    if (nowMsLD - lastEvtPrintMs >= 100UL) {   // per-event serial line, throttled
      lastEvtPrintMs = nowMsLD;
      char lw[240];
      int lwn = snprintf(lw, sizeof(lw),
        "LW t=%lu gap=%lu loop=%.1f rai=%.1f ctrl=%.1f fold=%.1f health=%.1f boat=%.1f sysH=%.1f wifi=%.1f clients=%u mhz=%u heap=%lu sup=%lu\n",
        (unsigned long)nowMsLD, (unsigned long)gap, ltUs / 1000.0,
        ft_rai_total.lastCall / 1000.0, ft_AdjustFieldLearnMode.lastCall / 1000.0,
        ft_altFold.lastCall / 1000.0, ft_altHealth.lastCall / 1000.0, ft_boatPerf.lastCall / 1000.0,
        ft_updateSystemHealthStats.lastCall / 1000.0, ft_SendWifiData.lastCall / 1000.0,
        (unsigned)events.count(), (unsigned)getCpuFrequencyMhz(),
        (unsigned long)ESP.getFreeHeap(), (unsigned long)evtSuppressed);
      if (lwn > 0 && Serial.availableForWrite() >= lwn) Serial.write((const uint8_t *)lw, lwn);
      evtSuppressed = 0;
    } else {
      evtSuppressed++;
    }
  }
  if (nowMsLD - winStartMs >= 30000UL) {       // 30 s self-resetting window summary (also a liveness pulse)
    char ws[224];
    int wsn = snprintf(ws, sizeof(ws),
      "WIN %lus b4-6=%lu b6-8=%lu b8-10=%lu n>10=%lu n>15=%lu n>20=%lu max=%.1fms clients=%u mhz=%u heap=%lu\n",
      (unsigned long)((nowMsLD - winStartMs) / 1000UL),
      (unsigned long)winB46, (unsigned long)winB68, (unsigned long)winB810,
      (unsigned long)winN10, (unsigned long)winN15, (unsigned long)winN20,
      winMaxUs / 1000.0f, (unsigned)events.count(), (unsigned)getCpuFrequencyMhz(),
      (unsigned long)ESP.getFreeHeap());
    if (wsn > 0 && Serial.availableForWrite() >= wsn) Serial.write((const uint8_t *)ws, wsn);
    winStartMs = nowMsLD;
    winN10 = winN15 = winN20 = 0; winMaxUs = 0;
    winB46 = winB68 = winB810 = 0;
  }
  // 80MHz low-power loop health (engine-off only). One cheap freq read gates it;
  // tracks worst pass + counts passes over the accel FIFO drain limit. See LOOP80_* globals.
  if (getCpuFrequencyMhz() < 81) {
    loop80IterCount++;
    if ((uint32_t)LoopTime > loopWorst80Win) loopWorst80Win = (uint32_t)LoopTime;
    if ((uint32_t)LoopTime > loopWorst80Ses) loopWorst80Ses = (uint32_t)LoopTime;
    if (LoopTime > LOOP80_IMU_LIMIT_US) loopOver80ImuLimitCount++;
  }
  // Field-ON loop health: passes that started with the field gate open, splitting real control-path stalls (OV risk) from
  // intentional field-off background work (flash flushes, uploads) that dominates the ungated worst.
  if (passFieldOn) {
    if ((uint32_t)LoopTime > loopFieldOnWin) loopFieldOnWin = (uint32_t)LoopTime;
    if ((uint32_t)LoopTime > loopFieldOnSes) {
      loopFieldOnSes = (uint32_t)LoopTime;
      captureLoopBlame();  // new session worst — snapshot which timed calls consumed this pass
    }
  }
  if (LoopTime > 5000000) {  // 5 seconds in microseconds
    Serial.printf("WARNING: Loop took %lld - potential watchdog risk\n", LoopTime / 1000);
    if (WiFi.getMode() != WIFI_OFF) {  //  GATE
      char warnMsg[96];
      snprintf(warnMsg, sizeof(warnMsg), "WARNING: Loop took %llums - potential watchdog risk", LoopTime / 1000);
      events.send(warnMsg, "console", millis());
    }
  }
  //This one resets every 5 seconds (AinputTrackerTime) — rolling short-window worst for UI sparkle
  if (LoopTime > loopTime5sWindow) {
    loopTime5sWindow = LoopTime;
  }
  //This one is the true session worst — resets only at boot, never mid-session
  if (LoopTime > MaximumLoopTime) {
    MaximumLoopTime = LoopTime;
  }
  //This one persists to NVS — becomes prevSessionMaxLoopTime on next boot
  if (LoopTime > MaxLoopTime) {
    MaxLoopTime = LoopTime;
  }

  static unsigned long lastTempDebugPrint = 0;
  if (millis() - lastTempDebugPrint >= 300000) {  //5 minutes
    lastTempDebugPrint = millis();
    printTempDebugStatus();
  }

  // === LED FIELD INDICATOR: 10-second cycle, ON-time proportional to duty MOVE THIS OUT OF LOOP SOMEDAY ===
  {
    static unsigned long ledCycleStart = 0;
    const unsigned long LED_CYCLE_MS = 10000;
    unsigned long elapsed = millis() - ledCycleStart;
    if (elapsed >= LED_CYCLE_MS) {
      ledCycleStart = millis();
      elapsed = 0;
    }
    unsigned long onTime = (unsigned long)(dutyCycle * (LED_CYCLE_MS / 100.0f));
    if (onTime < 150) onTime = 0;
    digitalWrite(2, (elapsed < onTime) ? HIGH : LOW);
  }
  checkAndRestart();     // scheduled maintenance restart
  esp_task_wdt_reset();  // Always reset watchdog at end of loop

  taskYIELD();  //consider removing
}

//This has to stay here, something about lambda functions (?)
template<typename T> void PrintLabelValWithConversionCheckUnDef(const char *label, T val, double (*ConvFunc)(double val) = 0, bool AddLf = false, int8_t Desim = -1) {
  OutputStream->print(label);
  if (!N2kIsNA(val)) {
    if (Desim < 0) {
      if (ConvFunc) {
        OutputStream->print(ConvFunc(val));
      } else {
        OutputStream->print(val);
      }
    } else {
      if (ConvFunc) {
        OutputStream->print(ConvFunc(val), Desim);
      } else {
        OutputStream->print(val, Desim);
      }
    }
  } else OutputStream->print("not available");
  if (AddLf) OutputStream->println();
}
//*****************************************************************************
