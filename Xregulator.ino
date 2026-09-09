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
const char *FIRMWARE_VERSION = "0.0.4";

// OTA artifacts are served from a stable URL we control: ota.xengineering.net, a thin
// proxy on our own web host that forwards to the Supabase Storage "ota" bucket. The
// firmware only ever knows this domain — to re-home OTA hosting later, we repoint the
// proxy, no firmware/factory re-flash. URLs are built from OTA_BASE_URL in
// performOTAUpdateToVersion(). (setInsecure on the download path; integrity is the
// on-device RSA-4096 signature check, not TLS — see performOTAUpdate.)
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
#include <NMEA2000_esp32_xeng.h>                               // _xeng fork of NMEA2000_twai (Svante Karlsson / Timo Lappalainen): transmit never blocks — stock waits up to 100ms/frame on fast-packet frames when the TX queue is full (no bus attached)
tNMEA2000_esp32 NMEA2000(ESP32_CAN_TX_PIN, ESP32_CAN_RX_PIN);  // necessary for ESP32-S3 library
#include <N2kMessages.h>
#include <N2kMessagesEnumToStr.h>  // questionably needed
#include <WiFi.h>
#include <AsyncTCP.h>                    // for wifi stuff, IMPORTANT, don't ever update, use mathieucarbou github repository
#include <LittleFS.h>                    //
fs::LittleFSFS webFS;                    // Separate filesystem for web files
bool usingFactoryWebFiles = false;       // Track which web partition is mounted
#include <ESPAsyncWebServer.h>           // for wifi stuff, important, don't ever update, use mathieucarbou github repository
#include <DNSServer.h>                   // For captive portal (Wifi Network Provisioning) functionality
#include <ESPmDNS.h>                     // helps with wifi provisioning (to save users trouble of looking up ESP32's IP address)
#include "esp_heap_caps.h"               // needed for tracking heap usage
#include "freertos/FreeRTOS.h"           // for stack usage
#include "freertos/task.h"               // for stack usage
#define configGENERATE_RUN_TIME_STATS 1  // for CPU use tracking
#include <mbedtls/md.h>                  // security
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
// Confidence states returned by FrontStore::classify (engine lives in 7_functions.ino; shared by
// the alternator and both boat fronts; mirrored in script.js — keep the numeric order in sync with
// the dashboard labels). These #defines stay HERE because globals below initialize from them.
#define FRONT_MEASURED      0
#define FRONT_ESTIMATED     1
#define FRONT_LEARNING_EDGE 2
#define FRONT_NO_REFERENCE  3

// Make the types visible to auto-generated prototypes, hack
struct UpdateInfo;
struct StreamingExtractor;
struct HttpsRequest;
struct SensorSnapshot;  // full definition below (PSRAM sensor ring)
struct IgnWatermark;    // full definition further down; needed here so the
                        // auto-prototype of wmIgnUpdate(IgnWatermark&, float)
                        // doesn't precede the struct declaration
struct FaDetectResult;  // fast alt-current channel detector contract (full definition in 2_functions.ino)
struct FadJob;          // pulse-pattern detector state machine (full definition in 2_functions.ino;
struct FadResult;       // function bodies in 8_functions.ino) — forward-declared so their
                        // auto-generated prototypes (FadJob*/FadResult* params) compile
struct ZFitResult;      // zeroFitRegress() return type (full def near ZeroFitRecord) — same reason
struct RipFit;          // measured ripple projection (full def below); forward-declared so the auto-prototypes
                        // of ripFitEncode(const RipFit&)/ripFitDecode(...) don't precede its definition
// cxStartPersistCancel()'s verdict (2_functions.ino) — defined up here, not next to the machine, so the
// auto-prototype can't precede the type.
enum CxStartCancel : uint8_t { CX_CANCEL_NONE = 0, CX_CANCEL_FRESH = 1, CX_CANCEL_RESUME = 2 };
// Auto-prototype generator fails on default-argument functions defined in later .ino files.
bool fieldOffSettled(uint32_t extraMs = 0);
bool fieldCutSettled(uint32_t extraMs = 0);

SET_LOOP_TASK_STACK_SIZE(20 * 1024);  // 20KB stack necessary for SSL/TLS operations
int hardwarePresent = 1;              // usage varies
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
volatile uint32_t lastCloudUploadOkMs = 0;  // millis() of last ack-confirmed payload upload; 0 = never this boot. Written on Core 0 HTTPS task, read by CSV2 build.
QueueHandle_t httpsQueue;
TaskHandle_t httpsTaskHandle;

// Fault-detector Core-0 worker handoff (defined here so setup() in this file and the worker/arm
// in 2_functions.ino all see them). faResultSem returns the verdict; faDetTaskHandle is notified to arm.
TaskHandle_t faDetTaskHandle = NULL;
TaskHandle_t cpuLoadTaskHandle = NULL;   // low-prio Core-0 task: uxTaskGetSystemState CPU-load scan (moved off Core-1 loop)
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
  HTTPS_UPLOAD_CX_LEDGER,      // commissioning-ledger event batch → log-commissioning-event
  HTTPS_UPLOAD_USAGE,          // daily app-usage analytics blob → track-behavior
  HTTPS_CLOUD_OP               // staged registration/profile op (cloudOp slot in 3_functions.ino) — the
                               // 4 web handlers no longer run doCloudPOST inside the async_tcp task
};

struct HttpsRequest {
  HttpsRequestType type;
  char    *payload;     // PSRAM, OWNED: producer ps_malloc's, the httpsTask worker free's after use.
  uint32_t payloadCap;  // allocated capacity (0 / NULL payload for the no-body request types)
};

String currentUID;
#define FUNC_TIMING_WINDOW_MS 10000  // rolling window for per-function worst-case timing (ms)


// ALTERNATOR HEALTH v2 — cloud-fitted curve model + best-ever record book.
// A_pred = base(RPM, excitation) × tempCorr(T) × busCorr(Vbus).
// "excitation proxy" = temp-normalized field drive, NOT measured field current.
// The CLOUD fits these surfaces from uploaded best-ever record points (field-off +
// interval); the device only EVALUATES them to rate live output, BUFFERS new
// envelope-pushing records for the next upload, and tracks a performance%-vs-engine-
// hours TREND (the headline display). Best-ever works here because an alternator's
// output is near-deterministic given its inputs, so best-ever ≈ expected and the
// reference auto-ratchets to the healthy peak (no freeze trigger needed).
// Mirrors boat-performance v2. Full design: ALT_HEALTH_V2_REBUILD_PLAN.md.

// Excitation-proxy temp normalization (copper seed; cloud refits α later).
#define ALT_ALPHA_PER_C 0.00393f
#define ALT_TREF_C      25.0f
#define ALT_MIN_BATT_V  8.0f
#define ALT_RPM_MAX     6000.0f   // axis full-scale (RPM)
#define ALT_FI_MAX      15.0f     // axis full-scale (excitation proxy)

// Curve grid — anchors/samples. MUST match the cloud edge fn (update-alt-health) exactly.
// ============================================================
// ALTERNATOR HEALTH — Best-Ever Front. Reference = a sparse best-ever output-amps surface over
//   {RPM, excitation, Vbus, temp}. The device learns + evaluates it (Episode + FrontStore engine
//   at the top of this file); the cloud prunes + retains raw history. Replaces the old cloud-fitted
//   anchor-grid curve + record book + sliding window. Engine instance + functions: 7_functions.ino.
// ============================================================

// Performance-vs-engine-hours TREND ring (the headline). One point per engine-hour bucket:
// average + worst output-% vs the best-ever front. Survives reboot; uploads for cloud history.
#define ALT_TREND_CAP 8760     // buckets retained = 1-yr rolling window of continuous runtime (~52 KB
                               // PSRAM); device-side display window only — cloud keeps the raw points
#define ALT_TREND_DROP 720     // oldest buckets evicted per rollover (~30 days) → one full /alttrend.bin
                               // rewrite per ~month of runtime instead of per engine-hour
struct AltTrendPt {
  uint16_t engHour;            // engine-hours-since-baseline bucket index
  int16_t  worstPct;          // low-decile (P10) output-% in this bucket (×10) — was raw min before 2026-08; a single unlucky sample no longer defines the hour
  int16_t  overallPct;        // average output-% in this bucket (×10)
};
static AltTrendPt *altTrend = nullptr;
static int         altTrendCount = 0;
static uint32_t    altTrendFlushed = 0;     // trend records already in the /alttrend.bin append log
static bool        altTrendRewrite = true;  // force a full log rewrite (load-miss or ring eviction)
// Raised by /debug/fillmax (without ?persist=yes) + /debug/clearmax: in-RAM rings are synthetic/empty,
// so every field-off persister early-returns and flash keeps the real learned blobs. RAM-only →
// reboot is the sole exit.
volatile bool dbgRingsSynthetic = false;
// Current-bucket accumulators (committed when the engine-hour bucket advances).
static double altBucket_sum = 0, altBucket_n = 0;   // running average over the bucket
static float  altBucket_worst = 0;                  // raw min output-% this bucket (debug CSV only; the committed low line is P10)
static uint16_t altBucketHist[60] = {};             // bucket %-histogram (2% bins, 0..120) → committed P10
static int    altCurEngHour = -1;
double altTrendBaselineSec = 0.0;   // EngineRunTime_AllTime at last "Start Over" (trend X-axis origin)

// Live point for the dashboard gauge / trend dot, via AltLive SSE. The ~200 Hz fold writes only the
// EMA-filtered inputs; prediction + state + % are computed at 1 Hz in altHealth_tick.
static float   altLive_rpm = 0, altLive_exc = 0, altLive_amps = 0, altLive_pred = 0;
static float   altLive_vbus = 0, altLive_tF = 0, altLive_duty = 0;   // filtered fold outputs for the 1 Hz evaluator
static float   altLive_gAmps = 0;          // graded amps: episode boxcar average (same statistic the records store)
static float   altTempSlopeFMin = 0;       // case-temp slope (F/min, fast-slow EMA pair in the fold) — thermal-transient tag
static float   altLive_pct = 0;            // live output-% = graded amps ÷ LWLR prediction (NO clamp; may exceed 100)
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
static uint8_t altStatusCode = 0;          // 0 normal, 3 disabled (Ignore Temperature); verdict codes 1/2 removed
// Session statistics over the graded 1 Hz samples since boot/Start Over: mean + P10 histogram (60 × 2% bins).
static uint16_t altSessHist[60] = {};
static uint32_t altSessN = 0;
static double   altSessSum = 0;
static bool    altHiFieldAlert = false;    // high-field-low-output alert (independent of the record book)

// GUI-adjustable settings (Pattern B; build-then-tune). Per-axis deviation bounds (steady times +
// front/eval knobs altRpmSec/altDutySec/altVbusSec/altThermDegF/altThermSec/altSafetyMargin/
// altIdwPower/altPruneK live with the engine instance in 7_functions.ino):
// Bands apply to the EMA-FILTERED detector inputs (altEmaSec), so they're sized for real
// operating-point drift, not raw loop dither / sensor jitter (which the filter strips):
// Band widths are sized by each axis's measured output sensitivity (Alt_Health_Dev_Summary.md §3):
// output smear from a band = band × dI/dx, and the four add in quadrature.
float altDutyTolPct    = 0.4f;   // field-duty band (% points, filtered; raw CV dither ~3 p-p). 2.52 A/pt measured OPEN-loop at idle
                                 // 2026-08-27 (the 1.76 closed-loop fit was low) — dominates the smear budget, but a record stores the
                                 // 2 s MEAN duty and MEAN amps, which co-move down the true line, so within-run wander partly self-cancels.
                                 // Also the dominant blocker (sole-block 30% of ticks at quiet idle): do NOT re-tighten — the July-14
                                 // 0.3 squeeze starved emits
float altRpmTol        = 20.0f;  // RPM band (filtered; raw idle jitter ~50 p-p). Open-loop measured 2026-08-27: 0.044 A/rpm at idle
                                 // (0.006-0.008 at cruise revs) — 30 admitted 1.3 A = 7% of idle output, 20 admits 4.7%; quiet-idle
                                 // p90 3 s range was 19 rpm so admission cost is small. Do NOT widen on the cruise number; the real
                                 // fix is a regime-dependent band (campaign deliverable)
float altVbusTol       = 0.20f;  // bus-voltage band (V, filtered). |dI/dV| < 1.8 and sign-unstable across fits — output cost is near zero
// Admission floors:
float altMinAmps = 2.0f;
float altMinDuty = 5.0f;
// Mode:
float altPaused       = 0.0f;    // 1 = halt LEARNING into My History (sticky). INDEPENDENT of altRefSource: you can
                                 // grade against the Uploaded surface (altRefSource=1) while still learning My History.
                                 // Selecting Uploaded defaults this ON; live display + trend still update either way.
float altSimMode      = 0.0f;    // 1 = inject synthetic operating points for bench testing (not persisted)

// BOAT PERFORMANCE — sailing polar (Phase 3). Pitch-corrected best-ever speed map.
// V_best(windspeed, windangle) × pitchDerate(pitch_std).  Predict STW (fallback SOG).
// Learning unit = sliding ~60 s windows; per-cell best-ever = robust top-K of window means.
// Fouling/current = one-sided CUSUM on recent ÷ best-ever.  Manual Pause (sticky) + Reset.
// Motoring map + web dashboard = follow-up slices. Full design: BOAT_PERFORMANCE_MATRIX_PLAN.md.
#define PERF_WS_BINS    16         // wind-speed bands 0..40 kt
#define PERF_WS_MAX     40.0f
#define PERF_WA_BINS    36         // wind-angle bins 0..360° (symmetric fold uses 0..180 → first 18)
#define PERF_WA_MAX     360.0f
#define PERF_PITCH_BINS 6          // sea-state (pitch-std) bands for the 1-D derate
#define PERF_PITCH_MAX  12.0f      // pitch std (deg) full scale
#define PERF_SAIL_CELLS (PERF_WS_BINS * PERF_WA_BINS)   // 576
#define PERF_TOPK       4          // best-ever = K best window-means; reference = min(topK) (robust high pct)

// BOAT PERFORMANCE — Best-Ever Front (sailing polar + motoring curve). Two sparse best-ever
// boat-speed surfaces: SAIL over {AWS, AWA, sea-state(pitch-std)}, MOTOR over {RPM, headwind,
// sea-state}. Wind axes are APPARENT (raw both-sided stored); perfFoldSymmetric folds |AWA| at
// eval+display only. Speed = user-selected STW or SOG. Device learns + evaluates; cloud prunes +
// retains raw history. Engine instances + functions: 7_functions.ino. Generic engine: top of file.

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

// GUI-adjustable settings (registry-driven; build-then-tune). Per-axis deviation bounds here;
// steady times + sea-state window + headwind/front/eval/prune knobs live with the engine instance
// in 7_functions.ino.
float perfWsTol        = 2.0f;      // AWS deviation band (kt)
float perfWaTol        = 12.0f;     // AWA deviation band (deg)
float perfRpmTol       = 100.0f;    // motoring RPM deviation band
float perfMinBoatSpeed = 0.5f;      // admission floor (kt)
float perfMinWindSpeed = 2.0f;      // admission floor (kt, sailing)
float perfRpmFloor     = 50.0f;     // RPM ≤ this = engine off = sailing; above = motoring (motorsailing counts as motoring)
float perfSpeedSrc     = 1.0f;      // 1 = STW, 2 = SOG (switching source = Clear-All)
float perfFoldSymmetric = 1.0f;     // 1 = fold |AWA| at eval/display (default); 0 = keep both tacks
float perfPaused       = 0.0f;      // 1 = halt learning/persist (sticky across reboot); live display still updates
float perfSimMode      = 0.0f;      // 1 = inject synthetic wind/speed/RPM/pitch for bench testing; NOT persisted

// Supabase configuration......similar stuff is locally defined in a few functions that upload sensor data, config snapshots to cloud
const char *SUPABASE_URL = "https://qnbekuaoweuteylitzvo.supabase.co";
// PUBLIC anon key - safe to commit, protected by RLS policies
const char *SUPABASE_ANON_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InFuYmVrdWFvd2V1dGV5bGl0enZvIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NTE5NzY1MzUsImV4cCI6MjA2NzU1MjUzNX0.k2S_kzkdAyN1Azs_7enxLun9LouB1bA_q7Sw8x1Cp0o";


// Vessel Info
float BOAT_LENGTH_FT = 0;
float BOAT_DISPLACEMENT_LBS = 0;
// Fixed buffers, not Strings: /saveVesselInfo writes these on the web task while Core 1 reads
// BATTERY_TYPE (batteryIsLithium, N2K/RV-C TX, cvpfChemGasses). String::operator= frees the old
// heap buffer on any reassignment past SSO, so a reader could dereference freed memory; an array
// can only tear. Same reason HOME_PORT below has always been a char array. 50 chars + NUL matches
// the form's maxlength.
char BOAT_TYPE[51] = "monohull";
char BOAT_MAKE_MODEL[51] = "";
uint16_t BOAT_YEAR = 2025;
char ENGINE_MAKE[51] = "";
uint16_t ENGINE_HP = 0;
uint8_t SYSTEM_VOLTAGE_CLASS = 12;
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

// Ring buffer sample structure
struct IMUSample {
  int16_t x, y, z;
  uint32_t timestamp_us;
};

// Verify struct size at compile time (expect 10 bytes logical, possibly 12 with padding)
static_assert(sizeof(IMUSample) == 10 || sizeof(IMUSample) == 12,
              "IMUSample unexpected size - check padding");

// Ring buffer sizes (adjust based on actual sizeof(IMUSample))
// Target ~32 KB total. If sizeof=12, use smaller counts.
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
float imu_heel_deg = 0;              // Current heel angle
float imu_pitch_deg = 0;             // Current pitch angle
float imu_yaw_rate_dps = 0;          // Current yaw rate (direct from gyro_z)
float imu_vertical_accel_g = 0;      // Vertical acceleration
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
float cf_heel = 0;   // Filtered heel angle
float cf_pitch = 0;  // Filtered pitch angle
unsigned long cf_lastUpdate = 0;

// --- IMU ZERO / LEVEL CALIBRATION (captured at rest, persisted as JSON string in NVS key NK_imu_zero) ---
float imuHeelOffsetDeg = 0;   // accel-derived heel at rest; subtracted from filter
float imuPitchOffsetDeg = 0;  // accel-derived pitch at rest; subtracted from filter
float imuGxBias = 0;          // gyro pitch-rate bias at rest (dps, vessel frame)
float imuGyBias = 0;          // gyro heel-rate bias at rest (dps)
float imuGzBias = 0;          // gyro yaw-rate bias at rest (dps)

// --- IMU INSTALL VALIDATION ---
// Mount state is a latched FACT (is the board's Y axis really vertical?), decided at Zero capture and
// cleared only by another Zero. az = -sensorY in every axisRemap row, so az ≈ +1 g at a level, still Zero
// iff the board is truly mounted vertical — independent of which of the four facings the user picked. It is
// judged from the CAPTURE, never from a running window: a monohull sailing at 34° heel has a window-mean
// az of cos(34°) = 0.83 g, which would latch BAD forever on a perfectly mounted board. The heel/pitch bias
// check is a per-window JUDGEMENT, not a fact: one freak window must not condemn a boat. Suspect data is
// flagged, never discarded.
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

// Live install status for the dashboard warning. The per-window 45° bias is deliberately NOT here: that is
// a property of one window, not of the install. Code 4 is not "verified" — a board with no IMU has nothing
// to verify, and nothing to tell the user either.
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
// Safety margin so the capture ALWAYS resolves and never hangs on "calculating": if the target
// isn't met within the timeout, finalize anyway on whatever was gathered (provided a sane floor),
// else abort cleanly. updateAccelMetrics() runs every loop, so this timeout is guaranteed to fire.
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

// Admin config push, user-facing half. The apply itself stays boot-only (executeGetPendingConfig);
// these two carry the "a push is waiting" notice and the "a push just landed" receipt to the
// dashboard so a settings change never happens invisibly.
// cfgPushPendingCount is learned mid-run from the 6 h check-forced-update poll (the staged config
// is NOT fetched or applied there) — it exists so a watching user can restart early instead of
// waiting out the 24/72 h maintenance ladder. cfgPushAppliedCount is restored from NVS at boot,
// written by the push itself just before it rebooted, and cleared by the dashboard's ack.
uint8_t cfgPushPendingCount = 0;   // settings staged in the cloud; 0 = nothing queued
// Fixed storage, not String: written on the HTTPS task, read by /configPush on the webserver
// task with no lock — a race on a char array can tear text, never the heap.
char cfgPushPendingNote[84] = "";  // short admin description shown in the notice (firmware caps at 80)
uint8_t cfgPushAppliedCount = 0;   // settings the previous boot's push wrote; 0 = nothing to report
String  cfgPushAppliedKeys  = "";  // comma-separated param names it changed (capped, see CFGPUSH_KEYS_CAP)

// Opt-in collector for the param names applyImportConfig actually CHANGED (settingWrite is
// compare-first, so an unchanged value never lands here); nullptr = don't collect. Lives here,
// not next to applyImportConfig in 8_functions.ino, because 4_functions.ino sets it and the
// Arduino prototype generator forward-declares functions only — not variables. Deliberately a
// global rather than a defaulted parameter: the generator re-emits the default and the
// duplicate declaration fails to compile.
String *cfgImportChangedNames = nullptr;

// WiFiClientSecure secureClient;  // Reusable SSL client to prevent stack overflow when we had individual ones for each upload   ABANDONED, THIS WAS NOT GOOD IN FLAKY WIFI
unsigned long lastHttpsOperationTime = 0;
const unsigned long HTTPS_MIN_INTERVAL = 500;            // min spacing between any HTTPS calls — protects core0 tiny system stacks (ipc0 = 1024 bytes)
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
// Maintenance reboot ladder (checkAndRestart, 5_functions.ino). All state is uptime-relative
// (millis), nothing persists — every boot restarts the ladder. 24 h: reboot only in a fully
// quiet window (engine+field off, no SSE client, battery OK, cloud queue drained) held 5 min.
// 48 h: drop the no-client and SoC preferences — engine+field off + voltage floor still
// required; a watching client gets the 10-min countdown banner. 72 h: reboot regardless of
// engine state (graceful field-down first); only OTA / active test / brown-out voltage defer.
// 72 h must stay far below the 49.7-day millis() wrap — other timers aren't wrap-audited.
const unsigned long REBOOT_OPPORTUNISTIC_MS = 86400000UL;   // 24 h
const unsigned long REBOOT_RELAXED_MS       = 172800000UL;  // 48 h
const unsigned long REBOOT_HARD_MS          = 259200000UL;  // 72 h
const unsigned long REBOOT_QUIET_HOLD_MS    = 300000UL;     // quiet window must hold this long
const int   REBOOT_SOC_FLOOR_X100  = 3000;   // quiet-tier SoC preference (SOC_percent is %×100)
const float REBOOT_VBAT_FLOOR_V    = 11.8f;  // ×(SYSTEM_VOLTAGE_CLASS/12) — quiet/relaxed battery floor
const float REBOOT_VBAT_BROWNOUT_V = 11.5f;  // ×(SYSTEM_VOLTAGE_CLASS/12) — hard tier: a reboot's WiFi
                                             // startup current spike into a near-dead battery risks
                                             // a brown-out boot-loop / NVS write corruption
const int   REBOOT_HEAP_FLOOR_KB       = 20;          // largest contiguous internal block below this = TLS effectively dead (HTTPS preflight skips <15 KB)
const unsigned long REBOOT_HEAP_LOW_HOLD_MS = 1800000UL;  // 30 min continuously low before requesting — handshake dips recover in seconds
const unsigned long FORCED_RECHECK_INTERVAL_MS = 21600000UL;  // 6 h — periodic forced-update/version re-check (boot check alone is too slow at 24-72 h uptimes)

//Configuration Snapshot Stuff
char *configPayloadBuffer;

unsigned long lastConfigSnapshotTime = 0;
// Event-driven snapshot request: starts true so the first field-off window after boot uploads
// one; the 24 h CONFIG_SNAPSHOT_INTERVAL backstop covers the rest of a 24-72 h uptime. Also set by
// saveVesselInfoToNvs() and a successful /registerProfile, so the user_profiles vessel
// projection (update-config-snapshot) is fresh on Save instead of next-boot. Cleared only when
// a payload is actually queued, so a failed gate retries next pass.
bool configSnapshotRequested = true;
unsigned long queueDrainHoldStart = 0;  // millis() when we started waiting for queue to drain before low power; 0 = not waiting

// App-usage analytics: page dwell / button-press counters POSTed by the web UI to /track,
// merged into a fixed PSRAM pool of opaque string keys ("p:<page>" / "b:<button>" — the
// firmware never knows what the names mean, so new pages/buttons need no firmware change).
// Uploaded to track-behavior once per LOCAL calendar day — the boundary is local midnight
// via the client-reported UTC offset (wall-clock epoch math, not a millis timer), so it is
// immune to maintenance reboots, and for the same reason the pool is persisted to /usage.bin
// at the shutdown/restart flush points and reloaded at boot.
// Accumulates in every mode (AP included); only the upload requires Client mode + cloud.
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
// Non-blocking reconnect engine state (worked by checkWiFiConnection() every loop pass).
// No backoff, no give-up — presence scans at a fixed cheap cadence until the network returns
// (a phone hotspot can appear at any moment; a router user is connected ~always and never pays).
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

// Settings arm gate — replaced the admin password (LAN clients are trusted; this is an
// acknowledgment that settings may change, not authentication). RAM-only, so a reboot disarms.
// While disarmed every mutating endpoint rejects, which permanently kills replayed/stale
// settings URLs (the cvGainMode-flip class) without any password rotation.
// True while the LM2907 tach is known to be lying (field-cut test, hard-cut tail). Set every tick by
// buildTickSnapshot; read by the engine start/stop console lines so they do not report a phantom.
bool g_rpmTachMasked = false;
bool settingsArmed = false;
unsigned long settingsArmedAtMs = 0;
const unsigned long SETTINGS_ARM_TIMEOUT_MS = 30UL * 60UL * 1000UL;
// Auto-lock notice, latched when the window runs out with nothing connected so the next client
// to arrive learns settings relocked themselves. RAM-only like the gate: a reboot locks settings
// anyway and announces itself, so a surviving flag would only describe a window that no longer exists.
bool settingsAutoLockNotice = false;
uint32_t settingsAutoLockedEpoch = 0;  // 0 = no usable clock at lock time; the UI then omits the time

// ===== HEAP MONITORING =====
int rawFreeHeap = 0;      // in bytes
int FreeHeap = 0;         // in KB
int MinFreeHeap = 0;      // in KB
int FreeInternalRam = 0;  // in KB
int Heapfrag = 0;         // 0–100 %, integer only
uint32_t FreePSRAM = 0;   // KB of free PSRAM
size_t TotalInternalRam, LargestInternalBlock, TotalPSRAM;
int LittleFsFreeKb = -1;   // free space on the userdata LittleFS partition, KB; -1 until the boot seed. Event-driven: it can only change when this firmware writes a file, so writers set fsFreeDirty and the refresh runs at the next fieldCutSettled heap walk (usedBytes() is a ~100ms full-flash traversal — never on a live control pass)
bool fsFreeDirty = false;  // a LittleFS write happened since the last Free Data Storage refresh. Writers are Core 1 (loop) plus the httpsTask ring-backup remove; a bool store is one byte, and a set lost against a concurrent Core 1 clear only delays the readout to the next write
int WebFsUsedKb = -1;              // web-asset partition in use, KB; -1 until the boot seed. Only an OTA changes it, so one read at boot covers the whole run
int WebFsTotalKb = -1;             // that partition's size, KB — 1024 for both prod_fs and factory_fs
const char *WebFsPartLabel = "?";  // which partition the two above describe: a failed OTA remounts webFS on factory_fs, so the mount now is not always the mount that was read


// ===== TASK STACK MONITORING =====
const int MAX_TASKS = 50;  // Grossly big enough!
TaskStatus_t *taskArray;
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

// Black box: last-alive snapshot in RTC noinit RAM. Survives every reset type EXCEPT a true
// power-down (magic garbage after real power loss — that distinction is itself the diagnostic:
// valid magic after an unexplained reset proves the 3.3V rail never died). Written every loop
// pass; captured and re-armed once at boot in captureResetReason().
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
// Struct size folded into the magic so any future layout change auto-invalidates a snapshot
// written by older firmware instead of misreading shifted fields.
#define BLACKBOX_MAGIC (0xB1ACB0C5UL ^ (uint32_t)sizeof(BlackBoxSnap))
RTC_NOINIT_ATTR BlackBoxSnap g_blackBox;
BlackBoxSnap g_blackBoxPrev;        // copy of last session's snapshot, taken at boot
bool g_blackBoxPrevValid = false;   // true if last session's magic matched (RTC RAM survived)

// Lifetime OV excursion telemetry in RTC noinit RAM (same survival semantics as the black box:
// lost only on a true power-down — accepted). Histogram of RAW bus voltage, referenced to Bulk.
// Fine: Bulk .. Bulk+3.6V in 0.2V steps (18 bins; top ≈18V at a 14.4 Bulk). Coarse:
// Bulk+3.6 .. Bulk+15.6V in 1.0V steps (12 bins). +1 overflow bin. Bin widths scale
// ×(SYSTEM_VOLTAGE_CLASS/12) so the bin count is fixed across 12/24/36/48V. Separate from the session
// counters (g_voltSpikeCount etc.), which reset on reboot and /resetVoltageProtectionCounters;
// this struct is zeroed only by a magic mismatch or /resetOvTelemetryLifetime.
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
// Size + bin count folded into the magic so a layout change self-invalidates (no migration —
// an old-layout histogram is not comparable to a new one).
#define OVTEL_MAGIC (0x0E7E1E77UL ^ (uint32_t)sizeof(OvTelemetry) ^ ((uint32_t)OV_HIST_BINS << 8))
RTC_NOINIT_ATTR OvTelemetry g_ovTel;
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
// Slow-cellular profile (phone hotspot). CONNECT+HANDSHAKE share ONE blocking client.connect()
// with no WDT feed inside — their sum must stay < the 16 s panic WDT (DNS is pre-resolved with
// a feed at each call site). READ_TIMEOUT is IDLE (any received byte restarts it); GLOBAL_TIMEOUT
// is the wall-clock cap per attempt and may exceed the WDT because the read loops feed it.
const uint32_t CONNECT_TIMEOUT = 7000;    // ms
const uint32_t HANDSHAKE_TIMEOUT = 7000;  // ms (setHandshakeTimeout() takes SECONDS — divide at call site)
const uint32_t READ_TIMEOUT = 10000;      // ms, idle
const uint32_t GLOBAL_TIMEOUT = 45000;    // ms


unsigned long lastConsoleMessageTime = 0;
const unsigned long CONSOLE_MESSAGE_INTERVAL = 700;  //
// Fixed-size circular buffer:
// 128 silently truncated 14 diagnostics mid-sentence (every INA228 over-voltage line, both SystemID failure
// explanations) — always the tail, which is the part that says what to do. Queue is ps_malloc'd, so the extra
// 1280 B is PSRAM. Also caps queueConsoleMessageF's vsnprintf.
#define CONSOLE_MSG_LEN 256
struct ConsoleMessage {
  char message[CONSOLE_MSG_LEN];
  unsigned long timestamp;
};
#define CONSOLE_QUEUE_SIZE 10
ConsoleMessage *consoleQueue;
volatile int consoleHead = 0;
volatile int consoleTail = 0;
volatile int consoleCount = 0;
static portMUX_TYPE consoleMux = portMUX_INITIALIZER_UNLOCKED;
// Console history ring (~55 KB PSRAM), served at /consolehist.txt. The 10-slot queue drains to
// whichever SSE clients happen to be connected and the browser-side buffer lives in per-browser
// localStorage — so a fresh browser (or a dropped SSE message) loses history forever. This ring is
// the device-side source of truth the Download Logs console file is built from.
#define CONSOLE_HIST_SIZE 200
struct ConsoleHistEntry {
  uint32_t ms;      // millis() at queue time
  time_t epoch;     // soft-clock epoch at queue time (pre-2020 value = clock not yet synced)
  char msg[CONSOLE_MSG_LEN];
};
ConsoleHistEntry *consoleHist = nullptr;
volatile int consoleHistHead = 0;
volatile int consoleHistCount = 0;


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
// Boot identity stamped into every SSE payload alongside its build-time millis(). A client caches
// the last payload of each channel; when one channel stops arriving (schema gate rejecting it after
// an OTA, a dropped reconnect) the cache silently keeps serving pre-flash values. Same sessionId +
// a sane age proves a cached block belongs to this boot; a mismatch proves it does not. 31 bits so
// it always fits a signed int on the way through SafeInt-shaped consumers.
static uint32_t g_sessionId = 0;
int VeTime = 0;                     // rolling
int VeTime2 = 0;                    //session

int SendWifiTime = 0;
//Needed for ALERT pin on INA228 to shut off Field and delay 10 seconds before retrying if fault condition is gone
bool inaOvervoltageLatched = false;
unsigned long inaOvervoltageTime = 0;
uint32_t inaOvervoltageClearedMs = 0;  // timestamp when INA OV latch last cleared; used to suppress disagreement check
// How long after INA OV latch clears to keep suppressing the ADS/INA
// voltage disagreement check. Long enough to cover field collapse + both
// sensors resettling. Must be longer than INA228 averaged update rate.
const uint32_t INA_OV_DISAGREE_SUPPRESS_MS = 10000;  // 10 seconds


// ── Input filter (disturbance rejection) ─────────────────────────────────
// IIR EMA applied to CH0 (BatteryV), CH1 (MeasuredAmps), CH2 (RPM).
// α = dt / (TC + dt), computed per-sample from actual elapsed time so
// variable loop cadence is handled correctly without a fixed assumption.
// _filtered variables are the smoothed outputs; originals are unchanged.
// Protections read the RAW originals — iExcess runs its own IExcessTau EMA on the
// excess (measured − command), never on a _filtered signal. Each _filtered signal
// carries its own TC so CC PID and CV loop are tuned independently.
// Thermistor (CH3) is left on its own filter inside tempPID_tick().
float OutputPIDFilterTC = 34.0f;   // ms — Output Current PID EMA TC, NVS-backed (τ/3 at commissioning plant τ≈103ms)
float VoltageFilterTC = 103.0f;    // ms — IBV EMA TC for CV voltage loop, NVS-backed (full plant τ at commissioning, τ≈103ms)
float g_pidI_filtered = 0.0f;        // Output Current PID EMA signal
float IBV_filtered = 0.0f;           // EMA of INA228 bus voltage — used by getFiltV()

// ── SystemID — plant delay measurement ───────────────────────────────────
// Step test: baseline → 3× (duty up / duty down) → post-process.
// Samples stored in PSRAM. Buffer allocated on first test run, never freed.
// Results populate the JS popup and optionally update the current-loop and voltage filter TCs in flash.
// Legal in any SYS_MODE_AUTO state (bulk, absorption, float, target voltage).
// Manual mode is banned: duty is locked to ManualDutyTarget so the test's duty commands are silently ignored.
// Note: voltageControlActive = !inIdleStage, so it is true even in bulk — do NOT gate on !voltageControlActive.
// Enforced in the /get startSystemID handler and in the JS preflight check.
float SystemIDStepAmplitude = 6.4f;  // % duty step — web-configurable; default from the 2026-07-03 commissioning field curve

// ── SystemID sine-sweep (open-loop plant Bode) — Stage 1 ──────────────────────
// Test type selects step (rise/fall delay) vs sine sweep (per-frequency gain/phase
// of measured current vs commanded duty, PID off = the plant's own frequency response).
uint8_t systemIDTestType   = 0;        // 0 = step delay, 1 = sine sweep
float   systemIDSineFreqStart = 0.5f;  // Hz, sweep low end
float   systemIDSineFreqEnd   = 20.0f; // Hz, sweep high end
uint8_t systemIDSineCycles    = 2;     // analysed cycles per frequency (1 settle cycle skipped); integer-cycle windowed, so 2 is clean
// Fitted plant time constant (ms) from the dashboard's Plant Delay sweep fit. 0 = not yet fitted.
// Persisted (NVS) so the Biggest Actionable Disturbance readout survives reboots/reloads; only
// re-fit when the plant changes (alternator, belt, field wiring).
uint16_t systemIDPlantTauMs   = 112;   // ms — default from the 2026-07-03 commissioning plant fit; overwritten by each fit
#define SYSID_SINE_NPOINTS 10          // log-spaced sweep points
#define SYSID_STEP_HOLD_MS 5000        // ms — fixed step-test per-phase dwell; sized to plant settling (several plant τ)
struct SysIDBodePoint { float freqHz; float gainApPct; float phaseDeg; };  // gain = A output per %duty
SysIDBodePoint systemIDBode[SYSID_SINE_NPOINTS] = {};
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
uint32_t systemIDLastEndMs = 0;                // millis() when last test ended (cooldown guard)
uint8_t systemIDAbortReason = 0;               // FieldEventReason code if protection aborted the test; 0 = no abort / clean exit
uint8_t systemIDAbortPhase = 0;                // phase (1-9) at moment of protection abort; 0 = no abort

// ── Stabilize-phase constants ────────────────────────────────────────────────
float SystemIDStabilizeAmps = 35.0f;      // target alternator output before the sweep (= sweep trough); default from the 2026-07-03 commissioning field curve
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
// Mirrors tuningLog / cvTuningLog pattern. Captured on
// every test completion (success or abort) so fleet snapshots can ship a
// short history of plant-delay results.
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
// 15000 × 12 bytes = 180 KB in PSRAM. Covers TC up to ~714 ms at 5 ms CH1 cadence.
static const int SYSID_BUF_SIZE = 15000;
SystemIDSample *sysIDBuffer = nullptr;  // ps_malloc'd on first test run
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
// Quasi-static duty→amps ramp. Shares the SystemID duty-override path (mutually
// exclusive with SystemID / tuning sweeps). At each step it parks duty for a
// generous dwell (>5× any realistic field L/R) so the lag doesn't bias the curve,
// then records the settled (duty, amps) pair. Post-ramp it finds the saturation
// knee (where dAmps/dDuty flattens) and proposes the two open-loop sine settings:
//   SystemIDStabilizeAmps  = trough current  = 50% of the table limit-at-speed
//   SystemIDStepAmplitude  = AC duty amplitude = (duty@75% − duty@50%) / 2
// placed in the linear region BELOW the knee (slid down if 75% is into saturation),
// so K is never measured near saturation (would read low → over-aggressive Kp).
#define FIELDCURVE_MAX_PTS    48        // ramp samples (duty 5%→max)
#define FIELDCURVE_DUTY_START 5.0f      // first step (%)
#define FIELDCURVE_DUTY_MAX   92.0f     // ramp ceiling (%) — stop here if amps never reach the limit
#define FIELDCURVE_DUTY_STEP  2.0f      // duty increment per step (%)
#define FIELDCURVE_ONSET_FINE_STEP 0.25f // onset sweep: fine step backing DOWN to refine the coarse knee (%)
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
// Results (read via /fieldcurve.csv)
float fieldCurveKneeDuty   = -1.0f;            // duty at the saturation knee (%), -1 = none found
float fieldCurveKneeAmps   = -1.0f;
float fieldCurveTargetLimitA = 0.0f;           // table limit-at-speed used for the 50/75% targets (A)
float fieldCurvePropStabA  = 0.0f;             // proposed SystemIDStabilizeAmps (A)
float fieldCurvePropStepPct = 0.0f;            // proposed SystemIDStepAmplitude (% duty)
// Engine speed across the last ramp's settled dwells — advisory context served by /fieldcurve.json.
// The avg is the reference speed the wizard suggests holding for the downstream sweeps (loose ±500
// RPM advisory, never a gate). Reset at each start; valid for the last run until reboot.
float fieldCurveRpmMin = 0.0f, fieldCurveRpmMax = 0.0f, fieldCurveRpmAvg = 0.0f;
bool  fieldCurveOk = false;                     // true if a usable linear region + proposals were found
bool  fieldCurveCeilingLimited = false;         // true if the ramp was capped by the Max Field % limit
                                                // (MaxDuty, the real per-bus cap) BEFORE reaching the amp target —
                                                // curve is truncated; dashboard tells the user to raise Max Field %
                                                // and re-run. Cleared at each sweep start.
// Abort reason for the field-curve / knee sweeps (latched at the protection cut, cleared at each start).
// fieldCurveAbortMsg is built from reasonToString() in 6_functions.ino and read back by the dashboard
// via /fieldcurve.json & /kneesweep.json so the commissioning dialog can say WHY a test stopped.
// FIRST cause wins — the cut that killed the ramp is the one reported. A hard cut corrupts the tach
// for ~4.6 s, so a phantom RPM_TOO_LOW cut lands during the teardown; before the mask + this latch it
// overwrote the real reason and the wizard blamed engine speed for an over-voltage.
volatile uint8_t fieldCurveAbortReason = 0;     // 0 = none / user-cancel; else FieldEventReason code
char  fieldCurveAbortMsg[48] = {0};             // human reason text at the cut ("" = none)
volatile uint8_t fieldCurveAbortFollowOn = 0;   // 2nd distinct cut reason in the same teardown (0 = none)
float fieldCurveAbortVolts = 0.0f;              // bus volts at the first cut
float fieldCurveAbortDuty  = 0.0f;              // commanded field % at the first cut

// ── Min% onset-knee sweep (commissioning) ────────────────────────────────────
// Reuses the field-curve ramp but STOPS the instant settled amps lift off the baseline
// (the onset knee = where output current begins to build), so it forces almost no current.
// Run at 3 held RPMs in DESCENDING order (max working RPM → mid → idle), each warm-started from the
// last; applyKneeCurve() least-squares fits the physical model onset = a + C/RPM across them and
// writes rpmMinDutyTable (Min% column). Above the highest anchor RPM the floor is forced to ZERO.
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
// Live preview of the onset = a + C/RPM least-squares fit over the current anchors (recomputed when an
// anchor is committed). Exposed via /kneesweep.json so the review screen can flag a bad point before
// Apply. kneeFitResidPct = worst |measured − fit| in % duty; kneeFitWorstIdx = which anchor (capture
// order). Valid only once kneeAnchorN >= 2; -1 / -1 otherwise.
float kneeFitA         = 0.0f;                  // fitted intercept a (% duty, the RPM→∞ asymptote)
float kneeFitC         = 0.0f;                  // fitted slope C (% duty · RPM; onset = a + C/RPM)
float kneeFitResidPct  = -1.0f;                 // worst-anchor fit residual (% duty), -1 = not fitted
int   kneeFitWorstIdx  = -1;                    // anchor index (capture order) of the worst residual

// ── Gate-tuning capture: manual session logger + field sweeper ────────────────
// Measures how much output actually rides on each of the four axes the health record book matches
// on, so the admission gates can be sized from a measurement instead of from a closed-loop fit the
// regulator contaminated. Spec: Working Markdown Docs/ALT_GATE_TUNING_CAPTURE_SPEC.md.
// Implementation lives with the alt-health module in 7_functions.ino; the state is declared here so
// the /get handlers (3_functions.ino) and the duty-override chain (6_functions.ino) can see it.
volatile uint8_t altLogState   = 0;   // 0 IDLE, 1 RECORDING, 2 STOPPED, 3 FULL
uint32_t altLogRows            = 0;   // rows captured this session
float    altLogSecLeft         = 0;   // countdown: capacity remaining at the fixed 10 Hz rate (s)
// Every button press is a REQUEST consumed on the control loop (altLogService), never an action
// taken on the web task: allocation, freeing and the state machine all run on the one task that
// also writes rows, so a Discard can never free the buffer out from under a row write.
volatile bool    altLogStartReq = false, altLogStopReq = false, altLogClearReq = false;
// The sweeper is a stimulus generator, not a test with a result: a continuous duty ramp up and back
// down under the SAME duty-override path the field curve uses (GOV_BYPASS_SLEW + MANUAL PID). Up-and-
// down is what cancels the field lag and decorrelates speed sag / case warming from the field axis.
uint8_t  altSweepActive        = 0;   // 0 idle, 1 ramping up, 2 ramping down, 3 easing out
// True only during the constant-rate ramp itself — the ease-in/out transitions move duty far faster
// than the ramp rate, so their rows must not carry the sweep flag the offline fit splits on.
bool     altSweepMeasuredLeg   = false;
volatile bool altSweepRequested = false;      // set by /get?altSweepStart
volatile bool altSweepAbortRequested = false; // set by /get?altSweepCancel, and by the fast-OV cut
float    altSweepRatePctS      = 1.0f;        // ramp rate (% duty per second)
// How far short of a limit the ramp turns around. Panel controls, posted with the start request like
// rate/from/to — see altSweepLimitNow() in 7_functions.ino for why a margin is needed at all.
float    altSweepMarginA       = 10.0f;      // amps below every current limit
float    altSweepMarginV       = 0.20f;      // volts below every voltage limit (12V value; ×class/12 at boot and on class change).
                                             // 0.15 + the 250 ms confirm lost both 2026-08-27 float-stage sweeps: the bank's
                                             // acceptance knee ran >=1.2 V/s and the hard cut fired ~1.5 s after turnaround
float    altSweepFromPct       = FIELDCURVE_DUTY_START;   // ramp floor (%)
float    altSweepToPct         = 0.0f;        // ramp ceiling (%); 0 = resolve to the live field ceiling at start
uint32_t altSweepLastEndMs     = 0;           // cooldown guard, same 2 s rule as the other field tests
uint32_t altSweepReqMs         = 0;           // millis() of the start request — it expires (see altSweep_tick)
bool     altSweepLimitRev      = false;       // this sweep turned around early, not at its commanded ceiling
uint8_t  altSweepRevWhy        = 0;           // 0 not turned early, 1 a control limit, 2 the Reverse now button
volatile bool altSweepReverseReq = false;     // set by /get?altSweepReverse — turn the up leg around now

// ── Field de-energize time-constant test (runs inside commissioning stage 7) ──
// Ramps the REAL current loop to the commissioned stabilize level, freezes the duty for a settled
// baseline, steps the field to MinDuty (the exact floor a real OV clamp drives to) and fits the
// alternator-current decay for the field's electrical de-energize time constant (τ = L/R) — the
// time the OV protection's field drain physically takes. Decay is captured on the 20 kSPS fast
// alt-current channel when present (ADS CH1 fallback), calibrated against the ADS reading at the
// high (hold) and low (tail) points of this very test. The observed drain time IS RPM-dependent
// (bench 07-20: at lower RPM the output collapses below cut-in sooner), so the wizard runs one cut
// per Min%-floor speed (max/mid/idle) and fits drain-vs-RPM as a line (see fdDrain* below).
// Ramp phase runs through the normal AUTO path (fieldCutCcActive branch, real currentPID);
// hold/cut/ease phases share the SystemID duty-override + bumpless-resume path (OR'd into sysIDRunning).
#define FIELDCUT_BUF_SIZE   256      // ADS fallback decay samples (~10-30 ms CH1 cadence, PSRAM)
#define FIELDCUT_SETTLE_MS  3000UL   // measured current must sit in-band this long (after the setpoint slew arrives) before the hold starts
#define FIELDCUT_SETTLE_TIMEOUT_MS 30000UL  // never settles (e.g. target unreachable at idle) → proceed with what we have if it clears FIELDCUT_MIN_BASE_A
#define FIELDCUT_HOLD_MS    5000UL   // duty FROZEN this long before the cut — field at a constant, settled operating point; baseline + high calibration point averaged over the tail of this window
#define FIELDCUT_MEAS_MS    4000UL   // cut window: field at MinDuty. Decay is done in ~0.2 s (τ tens of ms); the window only needs to outlast it, then average the residual floor + low calibration point over the final 2.5 s. Was 10 s — 5-7.5 s of it measured nothing.
#define FIELDCUT_SNAP_MS    400UL    // grab the 500 ms fast-channel ring this long after the cut → window spans ~[cut-100, cut+400] ms even with drain lag
#define FIELDCUT_EASE_MS    1500.0f  // ease duty back to the pre-test operating point on exit — gentle re-engagement can't trip a protection
#define FIELDCUT_MIN_BASE_A 5.0f     // baseline-above-floor must clear this or there is no decay to fit
#define FIELDCUT_RPM_GRACE_MS 6000UL // keep masking the RPM gates this long after a run: the cut starves the LM2907 pickup, which spikes/false-zeros/ramps garbage for ~4.6 s while its front end re-biases (see reference_lm2907_frontend_final). The RPM reported per run (fieldCutRpm) is latched during the HOLD, before the cut corrupts the tach.
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
char  fieldCutAbortMsg[48] = {0};                  // human reason text on a failed/aborted run
// ── Manual protection-trigger test (Settings → Emergency & Troubleshooting) ────────────────
// Reproduces each field-collapse waveform on demand so an electrical fault can be provoked in a
// controlled bench run. Reuses the field-decay energize handoff (protTestCcActive) to bring the
// field to protTestCmdA through the real current loop, then fires the REAL executor for the mode.
// Set only from /get?protTestStart, gated by AUTO / RPM / charging / no-other-test.
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
// Commissioned field DRAIN time (ms): the measured command→10%-of-output time (≈ dead-time + 2.3 L/R
// time constants). Since 07-20 the wizard measures one drain per Min%-floor speed and stores the
// fitted drain-vs-RPM LINE as two endpoints (fdDrain* below); this scalar is kept as the WORST-CASE
// (longest) endpoint — the value used when RPM is unknown and by the 3× post-protection backstops.
// A manual edit of this setting flattens the line (endpoints cleared) and applies everywhere.
// Name kept for NVS/CSV3 continuity (key `fieldDecayTau`, CSV3 slot 314) — nothing fielded ever stored
// the old 1τ semantics. Safe placeholder default until the step is run.
uint16_t fieldDecayTauMs = 60;
// Drain-vs-RPM line, stored as its two TESTED endpoints (parallel-shifted in the wizard so the line
// sits at/above every measured point). Lookup interpolates between them and CLAMPS outside the tested
// range — never extrapolates (an underestimate releases the OV clamp with the field still hot).
// Degenerate (rpmHi<=rpmLo or a zero endpoint) → flat fieldDecayTauMs. See fdDrainMsAtRpm().
uint16_t fdDrainLoMs  = 0;   // drain (ms) at fdDrainRpmLo
uint16_t fdDrainHiMs  = 0;   // drain (ms) at fdDrainRpmHi
uint16_t fdDrainRpmLo = 0;   // lowest tested RPM (idle point)
uint16_t fdDrainRpmHi = 0;   // highest tested RPM (max-working point)
float fdDrainMsAtRpm(float rpm);
// Fast alt-current channel amps calibration (NVS): ampsCal = faCalGain × nominal + faCalOffA. The ESP32
// ADC path has poor absolute accuracy; the field-decay test two-points it against the ADS1115 reading
// (hold ≈30 A, tail ≈0 A) each run. Defaults = uncalibrated pass-through.
float faCalGain = 1.0f;
float faCalOffA = 0.0f;

// ── Auto-commissioning state machine ──────────────────────────────────────────
// Persistent across reboots (NVS). 0=NOT_COMMISSIONED, 1=IN_PROGRESS, 2=COMMISSIONED.
// Phase 0 snapshots every setting the flow may write (positional CSV string in one
// NVS key) so an explicit abort reverts to the pre-commissioning tune.
uint8_t commissionState = 0;                    // mirrors NK_commissionState
uint8_t commissionPhase = 0;                    // mirrors NK_commissionPhase — current wizard phase (0=Prep…8=Stress test, 9=finished; moves backward on Back); drives the Commissioning tab checklist
// Per-stage completion bitmask (mirrors NK_commissionDoneMask). bit i = stage i has been completed:
// 0=Prep 1=Field curve 2=Plant fit 3=Verify 4=Disturbances 5=Thresholds 6=CV plant fit
// 7=Min% floor + Field decay 8=Stress test. Drives the per-step ✓ marks and the default checkbox
// selection for partial re-runs. COMMISSION_ALL_DONE = 0x1FF, but the COMMISSIONED badge needs only
// COMMISSION_REQUIRED_MASK = 0x0FF — stage 8 (Stress test) is an optional reference check.
uint16_t commissionDoneMask = 0;                // mirrors NK_commissionDoneMask
// Per-stage "set by hand, not measured" bitmask (mirrors NK_commissionManualMask). Combined with the
// done bit it distinguishes four states per stage: measured (done, !manual), hand-marked complete
// (done, manual), skipped-outstanding (!done, manual), and pending (!done, !manual). Cleared for a
// stage whenever it is properly measured or its done bit is cleared.
uint16_t commissionManualMask = 0;              // mirrors NK_commissionManualMask
volatile bool faCommissionGate = false;         // Phase 2: relax the matrix steadiness gate (RPM-in-bin, drift OK)

// Commissioning "rest" hold (between guided steps). The open wizard dialog pings commissionHeartbeat
// every ~2 s; if the ping goes stale (dialog closed / crashed / Wi-Fi dropped) the hold drops and
// normal charging resumes within COMMISSION_HEARTBEAT_TIMEOUT_MS. Both the floor and the ramp scale
// ×12/SYSTEM_VOLTAGE_CLASS (vNorm), so a 12 V base gives 4/2/1.33/1 % and 5/2.5/1.67/1.25 %/s at 12/24/36/48 V.
volatile uint32_t lastCommissionHeartbeatMs = 0;        // millis() of the last dialog heartbeat (0 = stale/exited)
#define COMMISSION_REST_FLOOR_PCT 4.0f                  // 12 V-base rest hold floor (% duty); bench-tune knob
#define COMMISSION_REST_RAMP_PCT 5.0f                   // 12 V-base rest ramp rate (%/s)
#define COMMISSION_HEARTBEAT_TIMEOUT_MS 20000UL         // stale ping → drop the hold, resume charging. 20 s, NOT 5 s: Download Logs blocks the browser main thread (cvlog binary→CSV) and hogs the async server, starving the 2 s ping — at 5 s that dropped the rest hold mid-wizard and the field ramped up "on its own"

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
char weatherLastError[64];
int weatherHttpResponseCode = 0;

int weatherModeEnabled = 0;            // 0=disabled, 1=enabled
float UVThresholdHigh = 2.1;           // UV index above this = high solar expected (kWh)
int WeatherUpdateInterval = 21600000;  // Update every 6 hours (in milliseconds)
// A forecast this far past its refresh time stops holding the field off (see analyzeWeatherMode).
// Grace on top of the interval: at exactly the interval the refetch is queued in the same tick, so
// releasing there blips the field on for the seconds the fetch takes, every interval.
const unsigned long WeatherStaleGraceMs = 1800000UL;  // 30 min
int WeatherTimeoutMs = 10000;          // HTTP timeout in milliseconds

int currentWeatherMode = 0;           // 0=normal, 1=high solar, 2=low solar
unsigned long nextWeatherUpdate = 0;  // When next update is due
time_t weatherFetchEpoch = 0;         // wall clock of the last successful forecast fetch (0 = none this boot) — tells the ledger which local day "tomorrow" meant

// ── Solar energy ledger: predicted vs actual harvest + consumption, one record per LOCAL day ──
// Closes the loop on Defer to Solar. The forecast for day X is frozen the evening before (the number
// the pause decision was acting on overnight), the panels' actual harvest and the boat's actual
// consumption are booked at the next midnight, and the day's actual/forecast ratio drifts
// performanceRatio (solarLearnEnable). Consumption history sizes the pause threshold
// (solarUseConsEnable). Ring + live-day state live in PSRAM, flash only at field-cut-settled.
#define SLED_SIZE           90
#define SLED_PATH           "/solarledger.bin"
#define SLED_LIVE_PATH      "/solarlive.bin"
#define SLED_MAGIC          0x534C4547u   // 'SLEG'
#define SLED_VER            1u
#define SLED_MIN_COVER_MIN  1200          // a day feeds learning/prediction only if the device was awake >= 20 h of it
#define SLED_MIN_IRR_KWH    0.30f         // forecast (at ratio 1.0) below this can't ratio-learn — a near-zero promise makes the ratio noise
#define SLED_RATIO_MIN      0.15f
#define SLED_RATIO_MAX      1.20f
#define SLED_LIVE_FLUSH_MS  1800000UL     // live-day state to flash at most every 30 min while the field is cut
#define SLED_PRED_WINDOW    30            // consumption prediction looks back at most this many ledger days
#define SLED_PRED_MEDIAN_N  7             // ...and takes the median of the newest N complete days (mean when fewer exist)
#define SLED_F_FORECAST 0x01   // predHarvKwh came from a real forecast
#define SLED_F_VEDIRECT 0x02   // VE.Direct frames were seen during the day
#define SLED_F_SHUNT    0x04   // battery shunt configured (consumption is measurable)
#define SLED_F_LEARNED  0x08   // this day moved performanceRatio
#define SLED_F_PARTIAL  0x10   // coverage below SLED_MIN_COVER_MIN — excluded from learning and prediction
#define SLED_F_SAMEDAY  0x20   // forecast only arrived after midnight (device booted late) — a full-day number, but not the one any overnight pause acted on
#define SLED_F_LIVE     0x80   // CSV export only: the day still in progress
struct SolarLedgerRec {
  uint32_t dayIdx;        // local calendar day number: (epoch + usageTzOffsetS) / 86400
  float predHarvKwh;      // forecast harvest at the ratio then in force (NAN = no forecast the evening before)
  float predIrrKwh;       // the same forecast at ratio 1.0 (irradiance x array watts) — the learning basis
  float actHarvKwh;       // VE.Direct panel-power integral over the day (NAN = no VE.Direct all day)
  float predConsKwh;      // consumption predicted for this day from ledger history (NAN = no history yet)
  float actConsKwh;       // house loads = alternator + solar + battery discharge - battery charge (NAN = not measurable)
  float altKwh;           // alternator share of the day's sources
  float ratioAfter;       // performanceRatio after this day's learning step (NAN = day did not learn)
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
  uint8_t  pad;
};
SolarLedgerRec *sledRing = nullptr;   // ps_malloc'd in solarLedgerInit()
uint16_t sledHead = 0;                // next write slot
uint16_t sledCount = 0;               // 0..SLED_SIZE
SolarLedgerLive sledLive = {};
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

// Charge-rate Hi->Lo ceiling slew (see AdjustField in 6_functions.ino). When the user drops the
// charge-rate mode to Low, the ceiling is glided down from its present value instead of stepping,
// so the field follows it and the iExcess supervisor never sees a false measured-vs-command excess.
// Armed by applyChargeRateMode() on any Hi->Lo change (app toggle, panel switch, commissioning restore);
// self-clears in the control loop once the new (lower) cap is reached.
float modeCapSlew = 0.0f;          // imposed ceiling during the down-glide (A)
bool modeCapSlewActive = false;    // true while a Hi->Lo glide is in progress
uint32_t modeCapSlewEndMs = 0;     // millis() the glide self-cleared; iExcess stays suppressed for
                                   // MODE_CAP_GLIDE_GRACE_MS after, to cover the field-current settling tail
#define MODE_CAP_GLIDE_SEC 2.5f      // full-scale Hi->Lo glide time (s); rate = MaxTableValue / this
#define MODE_CAP_GLIDE_GRACE_MS 1000 // iExcess suppression hold AFTER the glide ends (ms)

float FloatVoltage = 13.6;
float BulkVoltage = 13.9;                        // this could have been called Target Bulk Voltage to be more clear
float ChargingVoltageTarget = 0;                 // This becomes active target — now the SLEWED/smoothed value the
                                                 // CV loop AND the over-voltage protections read (rate-limited from
                                                 // ChargingVoltageTargetReq by the slew in AdjustFieldLearnMode).
float ChargingVoltageTargetReq = 0;              // Instantaneous DESIRED target (set each tick by stage logic /
                                                 // TargetVoltageMode / MaintainMode). ChargingVoltageTarget ramps to it.
float VoltageHardwareLimit = 14.3f;  // boot placeholder — persisted user setting (NVS), seeded/loaded in InitSystemSettings; updateINA228OvervoltageThreshold() programs the INA228 ALERT comparator from it. Top rung of the OV ladder: sits ABOVE the software instant cut (AlternatorHardShutdownV) as the electrical backstop, 0.2 V x class/12 below the battery's BMS charge-disconnect voltage.
bool inBulkStage = true;

// System voltage class (12/24/36/48V) lives in SYSTEM_VOLTAGE_CLASS (set from Vessel Info). It is the sole
// source of truth: the CV/CC gain normalization, knee/slew scaling, duty ceiling, and the boot SoC
// anchor's OCV brackets (seedSocFromVoltage) all scale by it, and a class change (via /saveVesselInfo →
// applyNominalVoltageChange) rescales the charge-voltage profile + the whole OV ladder (tier margins,
// software cut, INA228 hardware limit).

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

// FieldAdjustmentInterval retired 2026-08-20: it gated nothing. The field loop is free-running —
// AdjustFieldLearnMode() runs once per loop() pass (~200 Hz) and the inner PID fires on fresh ADS
// CH1 samples (5-25 ms, assume ~30 ms). The only gate that ever used it is commented out in
// 99_obsolete.ino. NVS key NK_FieldAdjustmentInterval and CSV3 slot retired with it — never reuse.
float TemperatureLimitF = 175;       // measured at the case probe; internal/metal temps run roughly +40 to +50 °F above this depending on sensor installation. Strategy rationale: docs Charging Strategy page
// Cold-charge lockout: board temp (BMP388 ambientTemp, °F) is a proxy for battery temp and reads
// warmer than ambient, so set MinChargeTempF with margin. Default ON (lithium); lead-acid can opt out.
bool  coldChargeLockoutEnable = true;    // master on/off for the cold-charge lockout (default ON)
float MinChargeTempF = 40.0f;            // board-temp floor below which charging is locked out (°F)
const float ColdChargeHysteresisF = 2.0f;// °F rise above MinChargeTempF before charging re-arms (anti-chatter)
// Hot-charge lockout, mirror of the cold one. Acts only on a MEASURED battery temperature (source 1..4);
// the board stand-in never drives it. Spec: Working Markdown Docs/BATTERY_TEMP_SENSORS_SPEC.md §5.
int   hotChargeLockoutEnable = 0;        // master on/off for the hot-charge lockout (default OFF)
float MaxChargeTempF = 122.0f;           // battery-temperature ceiling above which charging is locked out (°F)
const float HotChargeHysteresisF = 2.0f; // °F fall below MaxChargeTempF before charging re-arms (anti-chatter)
// Battery + extra temperature probes (multi-drop 1-Wire) and the battery-temperature source chain.
// Spec: Working Markdown Docs/BATTERY_TEMP_SENSORS_SPEC.md. batteryTempF() (6_functions.ino) picks the
// source; buildTickSnapshot mirrors its result into battTempActiveF/battTempActiveSrc once per tick.
int   battTempProbeEnable = 0;      // 1 = the BATT-role DS18B20 feeds BatteryTempProbeF + IDX_BATT_TEMP_PROBE
int   extraTempProbeEnable = 0;     // 1 = the EXTRA-role DS18B20 feeds ExtraTempF + IDX_EXTRA_TEMP
int   battTempSource = 0;           // 0 Auto (probe, NMEA 2000, VE.Direct, RV-C, then board if proxy enabled), 1 Probe, 2 NMEA 2000, 3 VE.Direct, 4 RV-C, 5 Board, 6 None
int   battTempProxyEnable = 1;      // Auto may fall back to the board temperature (BMP388 ambientTemp) when no battery measurement qualifies
int   extraTempAlarmHiEnable = 0;   // EXTRA-probe high alarm on/off
float extraTempAlarmHiF = 150.0f;   // EXTRA-probe high alarm threshold (°F)
int   extraTempAlarmLoEnable = 0;   // EXTRA-probe low alarm on/off
float extraTempAlarmLoF = 32.0f;    // EXTRA-probe low alarm threshold (°F)
float BatteryTempProbeF = NAN;      // BATT-role probe (°F); written by TempTask only, NAN until the first good read
float ExtraTempF = NAN;             // EXTRA-role probe (°F); written by TempTask only
float battTempActiveF = NAN;        // batteryTempF() result of the current tick (°F); NAN = no source qualifies
uint8_t battTempActiveSrc = 0;      // 0 none, 1 probe, 2 NMEA 2000 (127508), 3 VE.Direct (T), 4 RV-C (DC_SOURCE_STATUS_2), 5 board temperature
float VictronBattTempF = NAN;       // VE.Direct "T" field (°F), BMV/SmartShunt with a temperature sensor
float rvcRxBattTempF = NAN;         // RV-C DC_SOURCE_STATUS_2 source temperature (°F) at the dvccInst instance
bool  cvTempDerateInert = false;    // battery-temp source class (measured vs board) differs from CommissionTempSrc's class; derate returns 1.0 until the CV fit is re-run
int ManualFieldToggle = 0;           // 0 = Auto (PID) — fresh-flash default. Set to 1 for manual field control (debugging).
int PhysicalPanelOverride = 0;       // 0 (default) = the app owns HiLow and MaintainMode and the Cable 3 panel wires only report; 1 = the panel wires own both modes and the app toggles go inert
int MaintainMode = 0;                // Set to 1 to target 0 amps at battery
int TargetVoltageMode = 0;
float TargetVoltageSetpoint = 14.0f;
int OnOff = 0;             // 0 is charger off, 1 is charger On (corresponds to Alternator Enable in Basic Settings)
int Ignition = 0;          // Digital Input      NEED THIS TO HAVE WIFI ON , FOR NOW
int IgnitionOverride = 2;  // Auto (any value != 1) = follow real GPIO1 ignition wire (default); 1 = force ON. Cannot force off.
int HiLow = 1;             // 0 will be a low setting, 1 a high setting
// ── Switch panel inputs (data Cable 3 / connector RJ3) ────────────────────────────────────────
// RJ3 pin 5 = net HIGH/LOW   -> R47 -> opto U10 -> GPIO39: switch ON = the High charge rate (HiLow 1).
// RJ3 pin 7 = net FORCEFLOAT -> R76/R52 -> opto U17 -> GPIO40: switch ON = Force "Maintain Mode".
// Same inverting optocoupler topology as the ignition wire on GPIO1: voltage applied at the connector
// makes the opto conduct and pulls the ESP32 pin LOW, so asserted == !digitalRead(pin).
// RJ3 pin 3 (net EXTRA_OPTICAL_GPIO1 -> GPIO8) has no defined function and is left untouched.
// Both wires are always read and always reported; PhysicalPanelOverride alone decides whether they
// steer anything (resolveChargeRateMode / resolveMaintainMode, 6_functions.ino).
const int panelHiRatePin = 39;
const int panelForceFloatPin = 40;
bool panelHiRateInput = false;      // debounced HIGH/LOW input: true = voltage present at Cable 3 pin 5, i.e. switch ON = High rate
bool panelForceFloatInput = false;  // debounced FORCEFLOAT input: true = voltage present at Cable 3 pin 7, i.e. switch ON = Maintain
// HiLow and MaintainMode above are the EFFECTIVE modes every consumer reads. These two hold what the
// web/app toggle last asked for, which is also what NVS carries, so the override can be switched on
// and back off again and the app's own choice comes back exactly as it was.
int HiLowUserSel = 1;
int MaintainModeUserSel = 0;
int AmpSensorRange = 1;    // 0=±200A, 1=±300A (default), 2=±500A — hall effect sensor range
int LimpHome = 0;          // 1 will set to limp home mode, whatever that gets set up to be
int resolution = 12;       // for OneWire temp sensor measurement
int VeData = 0;            // Set to 1 if VE serial data exists
int NMEA0183Data = 0;      // Master switch for the NMEA 0183 serial receiver: only 1 lets the timed loop call ReadNMEA0183Data (which also self-checks it); turning it off clears any held 0183 heading, since nothing would ever age that value out again
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
float SwitchingFrequency = 400;      // Field switching frequency. Reverted from 19000 (bench 2026-08-20): 19kHz field edges coupled into the DS18B20 OneWire line (~5 CRC fails/min). The P-type bootstrap chop below ~5kHz (LM5109A C4 drained by R108, UVLO-chops long on-pulses) is benign — measured, see FIELD_DRIVE_BOOTSTRAP_DROOP.md. Ceiling stays 19455Hz (LEDC 12-bit divider limit; higher kills boot attach). Efficiency measured flat 100Hz-19kHz.
const int MIN_SAFE_FREQUENCY = 50;     // Above most audible issues // set to 2000 later
const int MAX_SAFE_FREQUENCY = 25000;  // Below core loss and EMI concerns
// Max Field % is the REAL per-bus field-duty cap (no hidden scaling). Its default is scaled down at
// first boot for higher-voltage banks (~99%@12V, ~50%@24V, ~25%@48V) and rescaled on a voltage change,
// so worst-case field current never exceeds the 12V-at-MaxDuty case. setDutyPercent() clamps commanded
// duty to it across the open-loop paths (manual/limp/fault) too on >12V banks. The user-visible Max
// Field box IS the cap; set it to 99 for full duty. A duty-RATIO proxy (no field-current sensor).
float MaxDuty = 99.0;
// Max Field Volts — the same ceiling expressed as physical field volts instead of a duty ratio, so it
// can be typed from the alternator's datasheet. Field makers reuse one 12V winding across their 24/48V
// models, so the winding in a high-voltage alternator may still be a 12V part. ccDutyCeiling() enforces
// whichever of this and MaxDuty binds first; this term solves against the MEASURED bus every tick, so it
// hands duty back as the bank sags where the class-scaled MaxDuty cannot. Thermal (I^2R) limit only —
// the field is switched, so the winding still sees full bus volts on every on-edge whatever the cap.
// Derived from duty x battery volts, not sensed at the field: true field volts run above it by the
// battery-to-alternator cable drop. Hardcoded default is a 12V value, class-scaled on first creation.
float MaxFieldVolts = 15.0;
float MinDuty = 1.0;       // field-duty floor: inner-PID outMin + soft-clamp decay floor. Compile-time default only — overwritten from NVS (voltage-scaled) at boot; per-RPM onset floor lives in rpmMinDutyTable.
float ManualDutyTarget = 4.0f;  // manual-mode field duty %, 0.01 resolution (open-loop plant tests need sub-1% steps)
int InvertAltAmps = 0;     // change sign of alternator amp reading
int InvertBattAmps = 0;    // change sign of battery amp reading
int BatteryShuntPresent = 1;  // 1 = INA228 battery shunt fitted (default). 0 = no battery-current sensor: Bcur meaningless, every battery-current consumer takes its safe/off branch and the UI hides those features.
uint32_t Freq = 0;         // ESP32 switching Frequency in case we want to report it for debugging

//Variables to store measurements
float ShuntVoltage_mV;                        // Battery shunt voltage from INA228
float Bcur;                                   // battery shunt current from INA228
float targetCurrent;                          // This is used in the field adjustment loop, gets set to the desired source of current info (ie battery shunt, alt hall sensor, victron, etc.)
float IBV;                                    // Ina 228 battery voltage
float IBVMax = 6;                             // used to track maximum battery voltage (NVS-persisted, shown on dashboard)
float dutyCycle;                              // Field outout %--- this is just what's transmitted over Wifi (case sensitive)
float FieldResistance = 2;                    // Field resistance in Ohms usually between 2 and 6 Ω, changes 10-20% with temp
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
TaskHandle_t tempTaskHandle = NULL;    // separate CPU task for temp reading (reads are non-blocking now, so the separate task is vestigial)
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
float ApparentWindSpeedNMEA = 0;       // Apparent wind speed from NMEA2K (knots)
float ApparentWindAngleNMEA = 0;       // Apparent wind angle from NMEA2K (degrees)
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
// Energy (Wh) — _AllTime totals are double so they survive years of accumulation
// without losing precision (float mantissa runs out around 16M Wh)
double ChargedEnergy_AllTime = 0.0;            // Wh (lifetime total produced from all sources)
double DischargedEnergy_AllTime = 0.0;         // Wh (lifetime)
float SolarChargedEnergy = 0.0f;               // Solar Wh (session)
double SolarChargedEnergy_AllTime = 0.0;       // Solar Wh (lifetime)
double AlternatorChargedEnergy_AllTime = 0.0;  // Alternator Wh (lifetime)
float EngineFuelUsed = 0.0f;              // L (session)
float EngineFuelUsed_AllTime = 0.0f;      // L (lifetime)
float AlternatorFuelUsed_AllTime = 0.0f;  // L (lifetime)
// Engine fuel consumption table - RPM vs. gallons per hour
// Fuel table defaults
#define FUEL_TABLE_SIZE 10
float fuelTableRPM[FUEL_TABLE_SIZE] = { 0, 500, 1000, 1500, 2000, 2500, 3000, 3500, 4000, 4500 };
float fuelTableGPH[FUEL_TABLE_SIZE] = { 0.0, 0.3, 0.5, 0.8, 1.2, 1.8, 2.5, 3.5, 5.0, 7.0 };
float defaultFuelRPMValues[FUEL_TABLE_SIZE] = { 0, 500, 1000, 1500, 2000, 2500, 3000, 3500, 4000, 4500 };
float defaultFuelGPHValues[FUEL_TABLE_SIZE] = { 0.0, 0.3, 0.5, 0.8, 1.2, 1.8, 2.5, 3.5, 5.0, 7.0 };
float currentFuelGPH = 0.0f;  // live fuel flow, gallons/hr (interpolated from table by RPM; 0 = engine off)
float currentNMPG = 0.0f;     // live fuel economy, nautical miles per gallon (SOG / GPH; 0 = unavailable)
// Session fuel-economy curve: observed mpg binned by RPM, this session only (zeroed at boot + Reset).
// A reading is binned only once RPM and boat speed have BOTH held within tolerance for the hold time
// (steady-state gate, mirrors the boat-perf Episode banding) so accel/decel transients don't pollute it.
// UNIVERSAL SCALE: bins span [0, top configured fuel-table RPM], so binWidth auto-fits the engine
// (default table top 4500 -> 250-RPM bins; an 8000-RPM engine -> 444-RPM bins). No hardcoded ceiling.
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
float distHourBuckets[24] = {0};                 // per-hour distance accumulators (nm)
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
float AlternatorOnTime_AllTime = 0.0f;           // Lifetime alternator on time (minutes) - random test
int EngineCycles_AllTime = 0;                    // Lifetime engine cycles - random test
float MaxAlternatorTemperatureF_AllTime = 0.0f;  // Lifetime max alternator temp (°F) - random test
float MaxTemperatureThermistor_AllTime = -99;    // Lifetime max thermistor temp (°C) - random test
float MeasuredAmpsMax_AllTime = 0.0f;            // Lifetime max alternator output (A) - random test
float RPMMax_AllTime = 0.0f;                     // Lifetime max RPM - random test
// SOC tracking accumulators (must be global for NVS save/load)
double socAccumulator_AllTime = 0.0;     // %·s (lifetime) — double; float loses integer exactness past 2^24, about 2-4 days of 2 s samples
double totalSocSampleTime_AllTime = 0.0;      // s (lifetime) — double so the denominator carries the same fractional seconds as the numerator; saved to NVS as u32
double totalVoltageSampleTime_AllTime = 0.0;  // s (lifetime) — double, same reason; saved to NVS as u32
double totalSpeedSampleTime_AllTime = 0.0;    // s (lifetime) — double, same reason; saved to NVS as u32
double voltageAccumulator_AllTime = 0.0;  // V·s (lifetime) — double; float overflowed in ~14 days
double speedAccumulator_AllTime = 0.0;    // kt·s (lifetime) — double
float AvgVoltage_AllTime = 0.0f;


String installId = "";  // NK_InstallId; /installid serves it + "|" + FIRMWARE_VERSION — browsers auto-clear console history when either changes (NVS erase or reflash)

//supabase authentications stuff
String authToken = "";      // Stored auth token
bool isRegistered = false;  // Registration status


// CLOUD FEATURES - STATIC BUFFERS// For ESP32-S3 with 16GB and OPI PSRAM
const int PAYLOAD_BUFFER_SIZE = 4096;
const int CONFIG_PAYLOAD_SIZE = 32768;  // sized for the config picker spec (~190 fields)
const int ALT_UPLOAD_BUF_SIZE  = 256 * 1024;  // PSRAM upload buffer — holds a full alt front batch (≤4096 pts)
const int PERF_UPLOAD_BUF_SIZE = 512 * 1024;  // PSRAM upload buffer — holds sail+motor batches (≤4096 pts EACH)

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

// Lower sanity bound for any recovered/retained epoch (Jan 1 2020, matches the GPS
// gate). Below this we treat the clock as garbage and stay unsynced. Used by
// restoreSoftClock() — the boot-time soft clock that keeps Long Term history records
// from being stamped 0 (and rendered as phantom multi-hour gaps) on a no-connectivity
// device with no NTP/phone/GPS source.
const time_t SOFTCLOCK_SANE_EPOCH = 1577836800;

// Freshness trackers for the priority chain (NMEA → Phone → NTP).
// Without these, once a source sets currentTimeSource, the device never
// re-considers other sources even if the chosen source goes silent (the
// "once TIME_GPS, never NTP again" bug).
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

// Manually typed coordinates (Weather Mode "Override manually"). When
// gpsManualActive is set, these win over BOTH boat NMEA and phone GPS and stick
// until the user clears the override. Held separately so the resolver can
// reassert them every tick without the auto-sources clobbering them. Persisted.
double LatitudeManual  = 0.0;
double LongitudeManual = 0.0;
bool   gpsManualActive = false;

// Which source the *effective* GPS position currently comes from. Resolved
// every tick by resolveSources() in 4_functions.ino. Published in CSV2 slot
// `currentGpsSource` for the dashboard's live source indicator.
enum GpsSource { GPS_NONE, GPS_NMEA, GPS_PHONE, GPS_MANUAL };
GpsSource currentGpsSource = GPS_NONE;
// Which source owns speed/course (SOGNMEA/COGNMEA). Selectable ONLY — phone speed is
// never an automatic fallback (user decision 2026-08-21): speedSourceMode below picks
// the owner outright, independent of timeSourceMode. Never GPS_MANUAL (typed
// coordinates carry no speed). Published in CSV2 slot `currentSpeedSource`.
GpsSource currentSpeedSource = GPS_NMEA;

// Owner of speed/course. SPD_SRC_PHONE = phone GNSS doppler via /set_phone_data
// (client posts ~2 s in this mode); NMEA PGN 129026 writes are suppressed so the
// record chain never mixes sources. Persisted NVS Pattern B.
enum SpeedSourceMode { SPD_SRC_NMEA, SPD_SRC_PHONE };
uint8_t speedSourceMode = SPD_SRC_NMEA;

// Position and clock are SEPARATE settings (split 2026-08-24): a position fix is
// personal data and a clock reading is not, so the owner can accept phone time —
// the only time source that exists in AP mode — without surrendering position.
// Default TSRC_AUTO runs the freshness-arbitrated chain. The forced modes pin
// behaviour even if the chosen source is stale (resolver still shows the value but
// skips MARK_FRESH, so distance/smoothing know not to use it). Persisted NVS Pattern B.
enum TimeSourceMode { TSRC_AUTO, TSRC_NMEA, TSRC_PHONE, TSRC_NTP };
uint8_t timeSourceMode = TSRC_AUTO;

// Owner of latitude/longitude. GPS_SRC_AUTO takes NMEA when fresh and falls back to
// the phone; GPS_SRC_PHONE pins the phone. No NTP member — NTP carries no position.
// The client requests location permission ONLY when this is GPS_SRC_PHONE, the phone
// owns speed, or Defer to Solar is on, so the default install never asks.
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
  int32_t altCurr_max = 0;
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

  // RPM is the one window field held in RAW units, not x100 like its neighbours: the engine-RPM
  // axis carries an unknown per-install gain (hand-typed RPMScalingFactor), so sub-RPM resolution
  // is meaningless. Cloud sensor_history.rpm_* are INTEGER and update-sensor-history gates the
  // leaderboards on rpm_avg > 50 REAL rpm - scaling here would break both.
  int32_t rpm_min = 999999;
  int32_t rpm_max = 0;
  int64_t rpm_area_v_us = 0;   // raw rpm * us
  uint64_t rpm_valid_us = 0;

  // WiFi strength
  int32_t wifiStr_min = 999900;
  int32_t wifiStr_max = -999900;
  int64_t wifiStr_area_v_us = 0;
  uint64_t wifiStr_valid_us = 0;

  int32_t dutyCycle_min = 999900;
  int32_t dutyCycle_max = 0;
  int64_t dutyCycle_area_v_us = 0;
  uint64_t dutyCycle_valid_us = 0;

  // Engine-running-weighted accumulators (gated by engineSpinning()). All five source
  // channels accumulate unconditionally every tick, so ONE shared on-denominator
  // (active_us) serves them all — per-field on_valid_us would be five copies of it.
  uint64_t active_us = 0;  // window µs with engine spinning (coverage numerator)
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

  int32_t tempMargin_min = 999900;
  int32_t tempMargin_max = -999900;
  int64_t tempMargin_area_v_us = 0;
  uint64_t tempMargin_valid_us = 0;

  // GPS
  double lat_current;  // Most recent smoothed latitude
  double lon_current;  // Most recent smoothed longitude

  // Timing
  uint64_t lastUpdateTime_us = 0;  // Track last update for time-weighted averaging
  unsigned long windowStartTime = 0;
};
SensorWindow *currentWindow = nullptr;  // allocated to PSRAM in init

// Subset of ImuWindow + the three comfort score globals, captured at
// window-roll time and frozen into the SensorSnapshot. Without this, buffered
// records would all read the live (post-reset) imuWindow at upload time,
// because uploads are field-off-gated and run minutes-to-hours after the
// snapshot was rolled. Only fields actually uploaded to the cloud are kept
// (raw per-axis accel/gyro, hf_vibe, 60s rolling metrics intentionally excluded).
struct ImuSnapshot {
  // Heel (×100)
  int32_t  heel_min;
  int32_t  heel_max;
  int64_t  heel_area_v_us;
  uint64_t heel_valid_us;
  // Pitch (×100)
  int32_t  pitch_min;
  int32_t  pitch_max;
  int64_t  pitch_area_v_us;
  uint64_t pitch_valid_us;
  // Vertical accel (×1000)
  int32_t  vertical_accel_min;
  int32_t  vertical_accel_max;
  int64_t  vertical_accel_area_v_us;
  uint64_t vertical_accel_valid_us;
  // Total accel magnitude (×1000, always positive)
  int32_t  total_accel_min;
  int32_t  total_accel_max;
  int64_t  total_accel_area_v_us;
  uint64_t total_accel_valid_us;
  // Comfort score point values at roll time (raw firmware 0-100 scales)
  float    msi_score;
  float    vomit_pct;
  float    anchorage_comfort;
  // Wave period (×1000; -1000 if invalid)
  int32_t  wave_period;
  // Events this window
  uint32_t slam_count;
  int32_t  slam_peak_max;  // ×1000 (millig's)
  bool     suspicious;     // frozen at roll: install unvalidated/bad, or this window's heel/pitch bias is implausible
};

// PSRAM ring of completed sensor snapshots, waiting to upload to Supabase.
// Replaces the old per-snapshot LittleFS write that stalled Core 1 for ~400 ms
// during sector erase. Pushes are microseconds. Uploads drain to httpsQueue
// from the cloud-feature block (field-off gate) or via the "Upload Now" button.
struct SensorSnapshot {
  int64_t collectionTime;       // time_t (64-bit, Y2038-safe) — positive epoch if synced, negative-millis marker if not
  SensorWindow window;          // accumulated min/max/area for this window
  ImuSnapshot imu;              // frozen IMU subset (resists imuWindow reset between roll and upload)
  uint8_t chargeStage;          // display code (0-7) captured at window roll — uploads are deferred so this can't read live state later
};
const uint16_t SENSOR_RING_SIZE = 1000;  // 1000 × ~870 B ≈ 870 KB PSRAM; ~6.9 days at SENSOR_UPLOAD_INTERVAL (10 min/sample)
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

// Long Term Plots — local fast tier for the history tab (Phase 1). ~1-month PSRAM
// ring of min/max/avg envelopes at the SENSOR_UPLOAD_INTERVAL cadence (10 min in
// prod), derived from the closing SensorWindow in uploadSensorHistory(). Served
// from PSRAM via /longTermPlots.bin; persisted via the Phase-0 scaffold
// (writePsramBlob/readPsramBlob) at field-off edge + shutdown; restored at boot.
// No per-record timestamp: record N's time = lastEpoch − (newest − N) × interval
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

  // v4 tail (append-only — offsets above must not move): engine-running-weighted
  // averages + window coverage. Denominator = µs the engine was spinning (active_us),
  // vs the wall-clock avg above that dilutes mixed run/rest windows.
  int16_t onAvg[5];                     // battVolt ×100, battCurr ×10, altCurr ×10, victronCurr ×10, duty ×100
  uint8_t activeFrac;                   // engine-on % of observed window ×2 (0..200 = 0..100%); 0 ⇒ onAvg invalid
  uint8_t _pad2;
};
// 16 (4-byte block) + 102 (17 envelope) + 12 (6 avg) + 2 (byte) + 10 (onAvg) + 2 (byte)
// = 144, a multiple of 4 (no tail padding). Use sizeof() as recordSize everywhere; this
// pins it so a layout edit can't silently desync the scaffold guard.
static_assert(sizeof(LongTermRecord) == 144, "LongTermRecord layout changed — update the scaffold recordSize guard");

const uint16_t LONGTERM_RING_SIZE = 4320;    // 30 d × 24 h × 6/h at 10-min cadence (~608 KB @ 144 B)
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
#define LONGTERM_BACKUP_VER   4u    // v4: +onAvg[5]+activeFrac (132→144 B); old rings discarded
// Periodic field-off dump interval. The rising-edge dump alone captures a nearly
// empty ring on a bench (field never cycles), so accumulated records were lost on
// reboot/power-loss. This re-dumps every interval WHILE field-off (only when the
// ring actually changed). dumpLongTermRing() appends just the new records each time
// (full rewrite only on compaction), so this cadence costs a few hundred bytes, not
// a ~501 KB whole-file erase — the flash-endurance fix for always-powered installs.
const unsigned long LONGTERM_DUMP_INTERVAL_MS = 15UL * 60UL * 1000UL;  // 15 min

// ===== ZERO-DRIFT CHARACTERIZATION LOG (temporary diagnostic) =====
// Records the alternator current sensor's reading WHILE THE FIELD IS OFF, to characterize how its
// zero point drifts vs. time / temperature / RPM. Sampled only when field-off >= 5 s: every 1 s
// while the engine spins (RPM>=200, for fast RPM sweeps), every 10 min while the engine is off.
// MeasuredAmps IS the raw sensor zero as long as auto-zero + manual offset are off. The field-off
// samples here are the data source for the temperature-compensated zero correction (zeroFitCompute,
// ZERO_DRIFT_TEMPCOMP_SPEC.md) — no separate active zeroing cycle. PSRAM ring, flushed to LittleFS
// field-off only; /zerolog.csv reads the live ring (always complete). ZeroLogEnable default ON.
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
#define ZEROLOG_VER   2u                     // v2 added boardTempFx10; v1 files discarded on load
#define ZEROLOG_FIELDOFF_MIN_MS  5000UL     // field must be off this long before a sample (own short gate)
#define ZEROLOG_RUN_INTERVAL_MS  1000UL     // sample period while engine spinning (RPM>=200)
#define ZEROLOG_IDLE_INTERVAL_MS 600000UL   // sample period while engine off (RPM<200) = 10 min
#define ZEROLOG_FLUSH_MS         1800000UL  // field-off flush cadence = 30 min
// After a reboot the DS18B20 hasn't produced its first reading yet (AlternatorTemperatureF
// is NaN). Wait up to this long for a real value before sampling, so we never log a bogus
// NaN-derived temperature. If still NaN past the window the sensor is absent → log the blank.
#define ZEROLOG_TEMP_GRACE_MS    30000UL    // post-reboot wait for the first valid alt-temp read
#define ZEROLOG_TEMP_BLANK       INT16_MIN  // alt-temp "sensor absent" sentinel → CSV emits an empty cell

// ===== Temperature-compensated zero correction (ZERO_DRIFT_TEMPCOMP_SPEC.md) =====
// Daily line-fit of the zero-drift log → live correction zero(T)=c+b·(T−T_REF), EMA-smoothed across
// days, ±3 A clamped. Replaces the old active auto-zero cycle.
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
#define ZFIT_VER   1u

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

// core0Busy — "hold the field off while a long Core 0 op is in flight." AdjustFieldLearnMode()
// early-returns on it, so it freezes the ENTIRE control loop (voltage PI + inner current PID),
// not just the field-enable line.
// Set it ONLY from ops that are themselves gated on fieldOffSettled() (>= 60 s field-off): httpsTask,
// syncTimeFromNTP, OTA. Anything that can fire during active charging must NOT set it — that blinds
// the protections and the PID for the full duration of the op.
// Readers: TempTask, testInternetSpeed, scheduled restart, OTA orchestration, the AFLM gate.
volatile bool core0Busy = false;

// Task-WDT panic window. Sized for the control loop; every blocking network call on a
// WDT-subscribed task must complete inside it (httpsTask cloud ops cap each phase at 12 s for
// this). An OTA attempt may legally block far longer, so performOTAUpdateToVersion() widens the
// window for the attempt and restores this value on every exit.
const uint32_t WDT_TIMEOUT_MS = 16000;

// Dashboard mirror of sensorRingCount. Real cap is SENSOR_RING_SIZE (1000).
int bufferedRecordCount = 0;
unsigned long lastBufferUploadAttempt = 0;

//temp debug of cloud uploads
int lastHttpStatus = -1;
bool lastUploadWasBuffered = false;
unsigned long lastBufferStatusPrint = 0;
const unsigned long BUFFER_STATUS_INTERVAL = 60000;  // 60 seconds


// Battery SOC Monitoring Variables
int BatteryCapacity_Ah = 200;         // Battery capacity in Amp-hours
int SOC_percent = 5000;               // State of Charge percentage (0-100) but have to multiply by 100 for annoying reasons, but go with it
float ManualSOCPoint = 25.0f;              // Used to set it manually (decimals allowed)
int CoulombCount_Ah_scaled = 7500;    // Current energy in battery (Ah × 100 for precision)
// Unsaturated twin of CoulombCount_Ah_scaled: same deltas, never constrained to [0, capacity],
// re-anchored to capacity at each full-charge event and to the live count at every external seed.
// Exists so the shunt-gain correction sees drift in BOTH directions — the live counter's top clamp
// erases the over-reading (count-too-high) evidence before the full-charge anchor is reached.
int shadowCoulombX100 = 7500;
// Deferred first-boot SoC seed: loadNVSData() runs before the INA228 is ever read (IBV still 0)
// and before InitSystemSettings parses battery type/voltage/capacity, so the voltage-based
// estimate must wait until the forced ReadAnalogInputs() at the end of setup().
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
NVS lifetime analysis (ESP32-S3-WROOM-1U-N16R8, 16 MB flash)
Partition: nvs @ 0x9000, size = 0x5000 (20 KB = 5 pages × 4 KB)
Effective pages for wear leveling: ~4 (1 reserved for GC)
Entries/page ≈ 126 (32 B each) → total ~504 entries capacity before GC
Flash endurance: ≥100,000 erase cycles per page

Save cadence = 2 minutes → 720 saves/day
Case A (5 keys change/save): 3,600 entries/day → 3,600/504 ≈ 7.14 GC cycles/day
 → 7.14 erases/page/day → lifetime ≈ 100,000/7.14 ≈ 14,000 days ≈ 38 years
Case B (21 keys change/save): 15,120 entries/day → 30 GC cycles/day
 → 30 erases/page/day → lifetime ≈ 3,333 days ≈ 9 years

Conclusion: At 2-minute saves with change-detection, NVS wear is well within
multi-year (≈10–40 yr) service life even in worst case.
*/


/*
UPDATE: IMU NVS additions (estimate +3-8 keys changing per save):
  Always changing (if IMU enabled):
    None - IMU lifetime counters only write on events or threshold changes
  
  Event-driven writes (outside normal save cycle):
    imu_capsize_count, imu_pitchpole_count - immediate write on event (rare)
  
  Periodic/threshold writes (committed by saveNVSDataFull at field-off edge):
    imu_slam_count_lifetime - only if slams occurred (variable)
    imu_hours_gentle/mod/rough/extr - once per hour when bucket transitions
    VibEnvelope blob - only when envelope changes >threshold (~1/hour)
  
  Updated Case B estimate with IMU: 21 + 5 = 26 keys → 18,720 entries/day
    → 37 GC cycles/day → lifetime ≈ 2,700 days ≈ 7.4 years
  
  Still acceptable for marine service life.
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
int32_t prev_ShadowCoulomb = 0;
uint32_t prev_SessionDur = 0;
int32_t prev_MaxLoop = 0;
int32_t prev_MinHeap = 0;
int32_t prev_PowerCycles = 0;
float prev_InsulDamage = 0.0f;
float prev_GreaseDamage = 0.0f;
float prev_BrushDamage = 0.0f;
float prev_ShuntGain = 0.0f;
uint32_t prev_LastGainTime = 0;
// Temp-comp zero-correction learned-equation shadows (auto-fire NVS compare-first)
int prev_zfValid = -1;
int prev_zfSensor = -1;
float prev_zfC = 1e30f;
float prev_zfB = 1e30f;
float prev_zfR2 = -1.0f;
uint32_t prev_zfLastEpoch = 0xFFFFFFFFu;
uint32_t prev_SolarEnergy = 0;
int32_t prev_EngineFuel = 0;
float prev_ChargeCycles = 0;
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
const uint32_t INA_SLOW_INTERVAL_MS = 1100;  // field off: AVG=128, CT=4120µs → 1054ms update
const uint32_t INA_FAST_INTERVAL_MS = 5;     // field on:  AVG=4,   CT=540µs  → 4.3ms update
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
// SOC Correction Dynamic Learning
int AutoShuntGainCorrection = 0;           // 0=off, 1=on - enable/disable auto-correction
float DynamicShuntGainFactor = 1.0;        // Learned gain correction factor (starts at 1.0)
int ResetDynamicShuntGain = 0;             // Momentary reset button (0=normal, 1=reset)
unsigned long lastGainCorrectionTime = 0;  // Last time gain correction was applied

// SOC Correction Protection Constants
const float MAX_GAIN_ADJUSTMENT_PER_CYCLE = 0.05;            // Max 5% change per full charge detection
const float MIN_DYNAMIC_GAIN_FACTOR = 0.8;                   // Don't go below 80%
const float MAX_DYNAMIC_GAIN_FACTOR = 1.2;                   // Don't go above 120%
const float MAX_REASONABLE_ERROR = 0.2;                      // Don't correct if error > 20%
const unsigned long MIN_GAIN_CORRECTION_INTERVAL = 3600000;  // 1 hour minimum between corrections

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

//More Settings
// SOC Parameters
float CurrentThreshold = 0.01f;       // Ignore currents below this (amps)
int PeukertExponent_scaled = 105;     // Peukert exponent × 100 (112 = 1.12)
int ChargeEfficiency_scaled = 990;    // Charging efficiency % × 10 (990 = 99.0%)
int ChargedVoltage_Scaled = 1380;     // Voltage threshold for "charged" (V × 100) (a Battery Monitor setup parameter, nothing to do with alternator)
float TailCurrent = 5.0f;             // % of battery Ah capacity (1 decimal place)
int ShuntResistanceMicroOhm = 100;    // Shunt resistance in microohms
// Single gate every battery-current consumer reads (SoC, tail-current, MaintainMode, load-dump, charge-
// current limit, CV plant fit). True only with a declared shunt AND nonzero resistance — the nonzero
// resistance also keeps the Bcur divide finite. Defined here (compiled first) so every file sees it.
#define HAS_BATT_SHUNT (BatteryShuntPresent && ShuntResistanceMicroOhm > 0)
// Zero-current float (UseFloat=2) parks the CV target at BulkVoltage and relies on zeroFloatActive forcing
// uTargetAmps=0 to keep the bank from ever reaching it. That regulation needs Bcur, so with no usable shunt
// the mode MUST degrade to a plain voltage float — otherwise the float stage holds absorption voltage with no
// current cap and no duration expiry. Every UseFloat==2 branch reads this, never UseFloat directly, so the
// target, the timeout, the stage label and zeroFloatActive can never disagree.
#define EFFECTIVE_USE_FLOAT ((UseFloat == 2 && !HAS_BATT_SHUNT) ? 1 : UseFloat)
int ChargedDetectionTime = 180;       // Time at charged state to consider 100% (seconds) (3 mins, industry standard for lithium)
int IgnoreTemperature = 0;            // If no temp sensor, set to 1
int IgnoreRPM = 0;                    // If RPM sensor absent or malfunctioning, set to 1 to bypass RPM gate
int MinRPMForField = 125;             // Field is cut when RPM is below this threshold (RPM). Engine (crank) RPM. Floored to no lower than the 100-RPM noise floor in ReadAnalogInputs — values <=100 collapse to that floor.
// Engine-stopped fast field cut: when RPM is a confirmed zero, drop the field immediately
// instead of letting the graceful shutdown ramp keep it energized. At true 0 RPM the
// alternator makes no output, so there is no load-dump risk; an energized field during the
// slow ramp couples PWM switching noise into the LM2907 RPM sense and shows as phantom RPM
// spikes. Confirmation window kept short (<=0.2s) so a single bad ADC read can't snap it off.
#define RPM_ZERO_CUT_MS 200          // ms RPM must hold at exactly 0 before the immediate cut
uint32_t rpmZeroSinceMs = 0;         // millis() when RPM first hit 0 (0 = not currently zero)
// Shutdown spin-down cut dwell: charging disabled + RPM below MinRPMForField must persist this
// long (≥ two ADS CH2 samples at ~30 ms cadence) before the immediate cut, so a single glitched
// low read can never cut mid-ramp at real duty — an abrupt cut from meaningful field amplitude
// slams the LM2907 coupling cap (100k bias kept for low-frequency performance → ~4.6 s re-bias
// dropout of false-zero RPM). Real spin-downs and dropout streaks are sustained, so they pass.
#define RPM_BELOWMIN_CUT_MS 75
// Protection-event RPM grace: for this long after any protection clamp tick, an EXACTLY-zero RPM
// reading is treated as tach-signal dropout (the clamp runs the field near MinDuty, which can
// starve the LM2907 pickup), not as a stopped engine — neither the below-minimum immediate cut
// nor the zero-cut fires on it. Nonzero-but-low readings gate normally, and rpmZeroSinceMs keeps
// counting through the grace so a genuine stall still cuts the moment the window expires.
// Applies ONLY while charging is enabled — a shutdown has nothing to recover, so both cuts fire
// ungated and a stopping engine can't sit with an energized field faking tach readings.
#define PROT_RPM_GRACE_MS 5000
// Tach-lie plausibility cut: tach claims the engine is running, the loop is COMMANDING current, the
// field is driven well above the per-RPM onset floor, and the alternator makes nothing — physically
// impossible above cut-in speed. Cause is one of: RPM signal noise (field-PWM coupling / LM2907
// re-bias), a dead alternator, or an open field drive where the commanded field delivers no current
// (ON/OFF switch off, field wire loose, gate-drive/Q3 fault). The first two drain the battery through
// the field; the open-drive case does not, but the immediate cut + escalating lockout
// (nextTachLieLockoutMs) is correct for all three. The 2026-07-22 runaway: phantom ~950 RPM sustained
// 99% duty at -0.4 A output indefinitely — no RPM-only gate can catch it because the phantom sits
// above MinRPMForField and below the 1000-RPM lone-jump bar.
// Both arm bars are install-relative, never absolute duty: the knee floor (rpmMinDutyTable) is
// DEFINED as the most field that still makes ~0 A, so zero output parked at the floor with the loop
// demanding nothing is designed behavior, not a lie (2026-07-28: a flat 25% bar sat below the ~27%
// commissioned idle floor and false-cut three CV re-entries). A phantom always has the loop demanding
// current — the bus is low and nothing charges it — with duty wound far above the floor. The headroom
// fraction inherits voltage-class scaling for free: rpmMinDuty and the field ceiling both already carry
// it (~99% ceiling @12V scales to ~25% @48V, where any flat bar goes blind). The span is taken from
// ccDutyCeiling(), not raw MaxDuty, so a Max Field Volts cap lowers the bar with the reachable duty
// instead of parking it above anything the loop can command.
#define TACH_LIE_HEADROOM_FRAC 0.15f  // arm bar = rpmMinDuty + this fraction of (ccDutyCeiling() - rpmMinDuty)
#define TACH_LIE_MAX_AMPS 2.0f       // |alternator amps| below this = no output; commanded amps above this = loop demanding
#define TACH_LIE_DWELL_MS 3000       // condition must persist this long — outlasts ramp/settling transients
#define TACH_LIE_RELEASE_BLANK_MS 6000  // no rev-up release decision until the LM2907 settles post-cut: covers the ~40 ms collapse spike (~570 ms after cut, observed) and the full ~4.6 s re-bias interval (doc figure)
#define TACH_LIE_RELEASE_HOLD_MS 300    // rev-up must hold this long continuously (~10 tach samples) — a lone spike frees nothing
bool TachLieEnable = true;           // user disarm (NVS). Cut ignores the global protections toggle — this switch is the only disarm
uint32_t tachLieSinceMs = 0;         // millis() when the implausible condition started (0 = clear)
float tachLieTripRpm = 0.0f;         // RPM latched at trip — early-release rev-up reference
bool tachLieLockoutArmed = false;    // active lockout was armed by a tach-lie trip (enables early release)
uint32_t tachLieRevUpSinceMs = 0;    // rev-up release test continuously true since (0 = not currently true)
// Engine-restart confirmation: after a confirmed stop, RPM must hold >= MinRPMForField this long
// before the RPM gate releases — a noise blip (or a phantom's first seconds) can't re-energize the
// field. Costs ~2 s of charging at every real engine start. Boot starts unconfirmed.
#define RPM_RESTART_CONFIRM_MS 2000
bool rpmRestartPending = true;       // set on confirmed stop, cleared by sustained above-min RPM
uint32_t rpmAboveMinSinceMs = 0;     // millis() when RPM first held >= MinRPMForField (0 = below)
uint32_t g_lastProtClampMs = 0;      // millis() of the most recent tick any protection clamp was active
// millis() of the most recent ABRUPT protection cut (applyImmediateCut). g_lastProtClampMs never
// covers these: applyImmediateCut returns out of the control loop long before the clamp block that
// stamps it. The cut slams the LM2907 coupling cap, so the RPM gates mask off this stamp instead.
uint32_t g_lastFieldCutMs = 0;
// Field-drain early release. g_ovClampRiseMs stamps the FIRST tick of a clamp episode (rising
// edge) — distinct from g_lastProtClampMs, which restamps every clamped tick. Once a clamp has held
// for the commissioned drain time at this speed (fdDrainMsAtRpm at g_ovClampRpm — the measured
// command→10%-of-output time), the coil current sustaining the excess has bled out, so the G2/iExcess
// hold latches are released without waiting for the lagged filtered-voltage return (iExcess additionally
// waits for its excess EMA to fall back under the fire line — see the site gates). OR'd with the natural
// release, so it can only ever SHORTEN a cut; Load Dump is untouched (re-asserts every tick, no hold latch).
uint32_t g_ovClampRiseMs = 0;
float g_ovClampRpm = -1.0f;          // RPM latched at the same rising edge — the cut corrupts the tach (~4.6 s false-zero), so live RPM during a clamp is garbage; <=0 → worst-case drain
uint32_t g_tauReleaseCount = 0;      // episodes where the tau timer elapsed against an OV/iExcess-owned clamp
int bmsLogic = 0;                     // if BMS is asked to turn the alternator on and off
int bmsLogicLevelOff = 0;             // set to 0 if the BMS gives a low signal (<3V?) when no charging is desired
bool chargingEnabled;                 // defined from other variables
bool bmsSignalActive = false;         // Read from GPIO42 (live pin state, published on CSV4 for the Integrations status row)
int AlarmActivate = 0;                // set to 1 to enable alarm conditions
int TempAlarm = 190;                  // above this value, sound alarm
int TempAlarmLow = 32;                // below this value, sound alarm (0 = disabled)
float VoltageAlarmHigh = 14.8f;       // above this value, sound alarm (V, 2 decimals)
float VoltageAlarmLow = 11.9f;        // below this value, sound alarm (V, 2 decimals)
int SocAlarmLow = 10;                 // sound alarm when SoC falls below this % (0 = disabled; needs socInfoAvailable)
int CurrentAlarmHigh = 100;           // above this value, sound alarm
int MaximumAllowedBatteryAmps = 125;  // safety for battery, optional
int RPMScalingFactor = 1330;          // adjust until it matches your trusted tachometer
float AlternatorCOffset = 0;          // tare for alt current
float BatteryCOffset = 0;             // tare or batt current
int timeToFullChargeMin = NAN;
int timeToFullDischargeMin = NAN;


// fields 52-57 in CSVData2 are reserved zeros (Reset* flags were always 0; buttons use hasParam, not these vars)
int ResetDischargedEnergy;  // total discharged from battery
int ResetFuelUsed;          // fuel used by alternator
int ResetAlternatorChargedEnergy;
int ResetEngineCycles;
int ResetRPMMax;
int ResetThermTemp = 0;  // Max thermistor temp reset

unsigned long fieldCollapseTime = 0;
unsigned long FIELD_COLLAPSE_DELAY = 30000;  // ms
// Lockout period after temperature or voltage-disagreement faults.
// RPM-too-low is NOT a fault — it's a normal idle/engine-stop state — so it gets its own short
// lockout instead of borrowing the 30s fault cooldown. Lets charging resume ~immediately on engine
// restart. Fast over-voltage (software hard cut) and the timed OV tiers use the adaptive
// nextFastOvLockoutMs ladder (0.5s escalating to 10s) so a transient blip doesn't cost a 30s
// outage — a persistent cause escalates regardless of which OV rung catches it; the remaining
// faults (OT, voltage disagreement) keep FIELD_COLLAPSE_DELAY. Not user-adjustable by design.
unsigned long RPM_RECOVERY_DELAY = 500;  // ms — short by design: while the engine sits stopped this cooldown re-arms every window (see 6_functions runShutdownPath), so on restart you wait out whatever remains of the current window before the field re-engages. Keep it small so that residual wait is short.
// Delay applied to the CURRENTLY-armed lockout, set at arm time (RPM_RECOVERY_DELAY for the RPM gate,
// else FIELD_COLLAPSE_DELAY). The remaining/clear/inLockout checks read this, not the constant.
unsigned long activeCollapseDelay = 30000;  // ms
// Charging resumes automatically once this expires and the fault has cleared.
// NOTE: INA228 hardware overvoltage is NOT governed by this timer — it uses
// its own independent 10s latch in CheckAlarms() and bypasses the lockout
// entirely via shouldImmediatelyCutGPIO4().
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
unsigned long DischargedEnergy = 0;         // Total discharged energy from battery (Wh)
unsigned long AlternatorChargedEnergy = 0;  // Total energy from alternator (Wh)
int FuelEfficiency_scaled = 250;            // Engine efficiency: Wh per mL of fuel (× 100)
// Engine & Alternator Runtime Tracking
int EngineRunTime = 0;          // Time engine has been spinning (SECONDS — incremented by secondsRun)
int EngineCycles = 0;           // Average RPM * Minutes of run time
int AlternatorOnTime = 0;       // Time alternator has been producing current (SECONDS — incremented by secondsRun)
bool engineWasRunning = false;  // Engine state in previous check


// CH1 Interval Instrumentation — zero heap, all storage static. Three tiers: a 10 s ring of
// timestamped intervals (stats on demand), a 2 m ring of 12 closed 10 s buckets, and all-time
// scalars. The 2 m tier only counts CLOSED buckets, so the most recent <=10 s is missing — fine
// for a diagnostic. CH1_RING must hold at least 10000 / (min expected CH1 interval ms).
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

// Inner Current PID Firing Interval — field-on-gated clone of the CH1 tracker. pidFire_record()
// sits past the digitalWrite(4,HIGH) gate so field-off ticks never reach it, and loop() clears
// pfHasPrev while the field is down so the first firing afterwards re-baselines instead of logging
// the whole off-gap. No ring: CH1's 5000-entry ring only ever yielded "last", tracked here directly.
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

// CV Voltage Loop Firing Interval — CV-mode-gated clone of the pf tracker above.
// Records the gap between successive CV (constant-voltage) loop firings, but ONLY
// while voltageControlActive (the regulator is actively holding a voltage target).
// voltLoop_record() is called once per CV fire; the control loop clears vlHasPrev
// whenever CV is inactive so the first firing after a CV-off stretch re-baselines
// instead of logging the whole off-gap. Reuses Ch1Bucket.
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
FuncTiming ft_altFold;     // 200 Hz alt fold (IDW eval cost) — distinct from ft_altHealth (SSE wrapper)
FuncTiming ft_boatPerf;
FuncTiming ft_n2kTx;  // NMEA2000 transmit tick — proof the TX path stays out of the control loop's time budget
FuncTiming ft_dvcc;   // DVCC follow brain (1 Hz trust state machine) — proof it stays out of the loop's time budget
FuncTiming ft_n2kParse;  // NMEA2000.ParseMessages() — RX frame drain + fast-packet assembly; the only N2K cost that scales with bus traffic
FuncTiming ft_huntGov;  // oscillation-damper evaluation — self-gated to one Goertzel pass per 32 control ticks; instrumented because it sits inline in the control loop
FuncTiming ft_fastAltDrain;     // fast alt-current channel bounded DMA drain (~1 ms cap per loop pass)
FuncTiming ft_faMatrixFlush;    // fast alt-current disturbance matrix → flash (field-off gated, like the long-term ring)
FuncTiming ft_faDetector;       // fast alt-current failure-detector verdict consume on Core 1 (analysis runs on the Core-0 faDetTask); ~0 cost
FuncTiming ft_faWindowFinalize; // fast alt-current per-2s-window finalize (Goertzel finalize + matrix fold + detector arm), runs inside faDrain
// Field-off flash/NVS writers called directly from loop() — timed for the same reason as
// ft_dumpLongTermRing: a dirty-gated flash write is rare but, when it fires, shows up as an
// otherwise-unattributed loop spike in the stall hunt. All three are field-off only (no OV risk).
FuncTiming ft_zeroLogService;   // zero-drift diagnostic: dumpZeroLog() LittleFS flush (field-off, 30-min, dirty-gated)
FuncTiming ft_solarLedger;      // solar ledger: 1 Hz day roll (microseconds) + field-cut-only LittleFS/NVS flush
FuncTiming ft_bhFlushCapNVS;    // Battery Health: LittleFS blob write of capacity ring + results (field-off, dirty-gated)
FuncTiming ft_kneeLearnService; // Auto Min% learning: NVS write of learned onset floors (field-off, 5-min, dirty-gated)

// Fast alt-current per-session worst scalars (fleet upload, consumer 5). Declared HERE
// (not in the 2_functions.ino fa section) because buildConfigPayload() reads them and
// sits earlier in that file. Updated at qualified-window finalize; persisted across reboot
// and cleared only by the ripple panel's own "Reset Worsts" button (ResetRipplePeaks).
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
// Operating-point context captured at the instant each headline worst was set — feeds the dashboard
// mini-tables under "Session Worst Ripple Pk-Pk" and "Highest Tone in Map". Persisted alongside the
// worsts so the context survives a reboot; cleared by the same Reset Worsts / Clear Map paths.
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

// Aliases so existing telemetry plumbing (CSVData2, dashboard) needs no changes.
// Time2 = last call duration; Time = worst-in-window. Both now backed by FuncTiming.
#define AnalogReadTime ft_rai_ina228.worstWindow
#define AnalogReadTime2 ft_rai_ina228.lastCall
#define IMUReadTime ft_rai_imu.worstWindow
#define IMUReadTime2 ft_rai_imu.lastCall
// Wrap any void call — records last, worst-window, and worst-session into FuncTiming.
// Nested calls (e.g. a flash write inside ReadAnalogInputs) are fully included in
// the outer timer, allowing triangulation without needing to instrument sub-calls.
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

// Blame registry: loop-resident TOP-LEVEL timers only. Nested sub-blocks (ft_rai_*,
// ft_faWindowFinalize) are excluded so a parent and its child never double-bill the same
// microseconds; Core-0 task timers are excluded because their passSeq stamps don't
// correspond to loop passes. Order = the blame index sent in CSV2 — any change here MUST
// be mirrored in FT_BLAME_NAMES in script.js.
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
// On each new worst loop pass above this threshold, ONE Console line prints that
// pass's per-function breakdown so the stall offender names itself during a soak.
// Self-throttling (fires only when the record breaks). Pure diagnostic — no control
// effect, no payload change. Re-armed by "Reset Peak Values". Remove once stall is closed.
#define LOOP_DIAG_THRESH_US 10000  // only report passes over 10 ms
uint32_t loopDiagWorstUs = 0;

// ── 80MHz low-power-mode loop instrumentation ─────────────────────────────────
// Engine-off drops the CPU to 80MHz; these track loop health in that state only
// (gated on getCpuFrequencyMhz()<81 at the read site — one cheap register read).
// The accel FIFO net-fills if a single loop pass exceeds ~38ms (6-sample drain
// budget / 156 sample-per-sec fill), so loopOver80ImuLimitCount is a "near-miss to
// IMU overrun" counter — pair it with imu_fifo_overrun_count (actual losses).
const uint32_t LOOP80_IMU_LIMIT_US = 38000;  // ~38ms: accel FIFO drain budget per poll
uint32_t loopWorst80Win = 0;           // worst 80MHz loop pass, rolling 5s window (µs)
uint32_t loopWorst80Ses = 0;           // worst 80MHz loop pass since Reset Peak Values (µs)
uint32_t loopFieldOnWin = 0;           // worst field-ON loop pass, rolling 5s window (µs)
uint32_t loopFieldOnSes = 0;           // worst field-ON loop pass since Reset Peak Values (µs)
uint32_t loopOver80ImuLimitCount = 0;  // # of 80MHz passes over LOOP80_IMU_LIMIT_US since reset
uint32_t loop80IterCount = 0;          // total 80MHz passes since reset (denominator for the above)

// ── End per-function timing ───────────────────────────────────────────────────



// Health-requested maintenance reboot: latched by checkAndRestart() when contiguous internal
// RAM sits below REBOOT_HEAP_FLOOR_KB for 30 min straight. Executes at the next quiet window
// regardless of the 24 h ladder minimum (a wedged TLS stack shouldn't wait a day). Only a
// reboot clears it.
bool maintRebootHealthReq = false;
bool systemShuttingDown = false;
// Deferred reboot via /get?RebootRegulator — flag set in handler, restart done in loop()
// so the HTTP response flushes first and we don't restart inside the async TCP callback
volatile bool rebootRequested = false;
unsigned long rebootRequestedAt = 0;
// Maintenance-reboot user warning. 0 = no countdown running (dashboard hides banner). Only the
// relaxed/hard reboot tiers run a countdown — quiet-tier reboots have no client by construction.
// Updated each loop tick by checkAndRestart(); published via CSV2.
uint32_t restartRemainingSec = 0;
const unsigned long RESTART_WARNING_WINDOW_MS = 600000UL;  // 10 minutes

int BatteryCurrentSource = 0;  // 0=INA228, 1=NMEA2K Batt, 2=NMEA0183 Batt, 3=Victron Batt
int timeAxisModeChanging = 0;  // toggle the time axis on and off in Plots.  Off = less janky but less info

int maxPoints;                 //number of points plotted per plot (X axis length)
int Ymin1 = -10, Ymax1 = 50;    // Current plot
float Ymin2 = 12.0, Ymax2 = 16.0;  // Voltage plot
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
// The field-duty floor governor_apply actually enforced on its last call — the resolved
// max(Min Field %, this speed's Keep-Alive %), or 0 on every path that bypasses the floor (shutdown
// ramps, sysID, an active protection clamp, MANUAL). Lives here rather than beside g_fieldVoltDutyCeil
// in 6_functions.ino because 3_functions.ino's CSV2 builder reads it, and the .ino files concatenate
// in filename order — a definition in 6_ is not in scope there.
float g_fieldDutyFloor = 0.0f;
uint8_t ctrlLimiter = 0;        // banner limiter code (→ CSV4/NavStream): 0 none, 1 alt current cap, 2 thermal derate, 3 CV voltage loop, 4 battery current limit, 5 field at max duty (machine/RPM limit), 6 protection (cap binding or post-protection recovery window), 7 battery above target (zero-output stand-down, altZeroOutput), 8 BMS charge-current limit (DVCC CCL), 9 BMS charge-voltage limit (DVCC CVL), 10 manual field mode (open loop — the user's duty IS the output), 11 min field floor holding duty UP (the only code where the limit raises output rather than lowering it), 12 zero-current command (Maintain Mode or zero-current float — the bank is held at 0 A on purpose), 13 warm-up ramp ceiling still climbing (WarmupRampRate), 14 charge-rate High->Low glide holding the old ceiling on the way down
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
bool tuningSquareAbrupt = false;  // when set, the TuningMode square wave bypasses the setpoint slew so the
                                  // current makes abrupt edges (only the current loop's own bandwidth limits
                                  // the rise). Used by the CV plant fit, which needs a well-defined step
                                  // instant for its edge-gain extraction. Transient — auto-cleared on
                                  // TuningMode exit; never persisted.

// Battery Health: active DCIR test + passive capacity-%-vs-cycles. Chemistry-agnostic — DCIR is
// pure ohms; the capacity-trend OCV curve + anchor window are set per chemistry from the web
// (LiFePO4 / lead-acid / AGM presets via the Vessel-Info populator; type "other" enters its own).
// All NVS writes are field-off-deferred — a DCIR test always ends with the engine on.
struct BattHealthSample {
  uint32_t tMs;
  float    v;   // IBV
  float    i;   // Bcur (+ = charging)
};
struct BattHealthResult {
  uint32_t epoch;        // 0 if clock wasn't synced when the test ran
  float    dcir_mOhm;
  float    soh_pct;      // NaN until a capacity point exists
  float    boardTempF;
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
  float    tempC;            // board temp at the full anchor
  uint8_t  conf;             // 0 = low confidence, 1 = high
};

float    bhStepLowA   = 30.0f;
float    bhStepDeltaA = 30.0f;  // crest = low + delta
uint32_t bhDwellMs    = 8000;   // must exceed bhMinDwellMs() — the 30A step slews at TEST_ENTRY_RATE_A (8 A/s), so the level needs ~3.75s just to arrive, plus a settled tail for the fit window
uint8_t  bhNumEdges   = 4;  // scored steps; 3 is the floor for the spread/fit check, 4 gives one extra edge to average for little added time

volatile uint8_t bhTestState = 0;          // 0 IDLE, 1 RUNNING, 2 DONE, 3 ABORTED
bool     batteryHealthTestActive = false;  // read by the control loop's health branch
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
// Both rings persist as LittleFS blobs (writePsramBlob) — the legacy NVS-string path
// (NK_bhCapBlob/NK_bhResults) hit nvs_set_str's 4000-byte cap near ~120 capacity points
// and failed silently; bhInitSettings migrates a legacy string once, then removes the key.
#define BHCAP_PATH  "/bhcap.bin"
#define BHCAP_MAGIC 0x42484350u  // 'BHCP'
#define BHCAP_VER   1u
#define BHRES_PATH  "/bhres.bin"
#define BHRES_MAGIC 0x42485253u  // 'BHRS'
#define BHRES_VER   1u

// ── Capacity tracker (OCV-anchored). Replaces the old circular coulomb-derived version. ──
// Low SoC comes from a RESTED open-circuit-voltage reading mapped through capOcvVolt[] —
// independent of coulomb counting, which is what makes fade actually measurable. The full
// anchor is the existing FullChargeDetected. Capacity = Ah bridged between them ÷ SoC span.
#define  CAP_OCV_ROWS 11
const uint8_t capOcvSocPct[CAP_OCV_ROWS] = {100, 90, 80, 60, 40, 30, 20, 15, 10, 5, 0};   // fixed breakpoints
float    capOcvVolt[CAP_OCV_ROWS] = {13.6f, 13.4f, 13.3f, 13.2f, 13.1f, 13.0f, 12.9f, 12.8f, 12.5f, 12.0f, 10.0f};  // RESTED 12V LiFePO4 default; user-editable, scaled by SYSTEM_VOLTAGE_CLASS/12 at lookup
// Chemistry rested-voltage presets (12V-referenced), mirror of web CAP_OCV_PRESETS. Loaded into
// capOcvVolt[] by chemistry at commissioning so both the SoC seed and the capacity anchor read the
// bank's real curve; a hand-tuned curve (matching no preset) is left untouched.
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
float    cvpfKb       = 0.0f;             // V/A/√s — retired √t tail; always 0 now (kept for JSON/Apply compat)
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
float    cvpfBaseDuty = 0.0f, cvpfStepDuty = 0.0f;  // settled duties captured from the current PID, replayed abruptly
float    cvpfPreSetpoint = 0.0f;   // setpointLimited before the test — restored pre-override so the resume snapshot is clean
uint32_t cvpfEdgeMs[8] = { 0 };    // tick timestamp of each abrupt duty edge (even idx = step-up, odd = step-down; pulse = idx>>1)
uint8_t  cvpfEdgeCount = 0;
float    cvpfEdgeK[8]   = { 0 };   // per-edge stiffness mV/A for the wizard table (0 = no reading)
uint8_t  cvpfEdgeStat[8] = { 4, 4, 4, 4, 4, 4, 4, 4 };  // 0 used · 1 weak step · 2 wrong-way · 3 window starved · 4 not fired
float    cvpfDiMaxA = 40.0f;       // per-run current-step ceiling (A); raised only when the operator accepts the weak-signal re-run, reset each start
float    cvpfRpmAtFit = 0.0f;      // mean RPM over the two middle pulse segments — re-run RPM advice + result-card conditions
float    cvpfRpmMinAtFit = 0.0f, cvpfRpmMaxAtFit = 0.0f;  // RPM extremes over the same windows — result-card context only
float    cvpfBattVAtFit = 0.0f;    // mean IBV over the two middle pulse segments — result-card "test conditions" only
float    cvpfSocAtFit = -1.0f;     // mean SOC% over the two middle pulse segments (−1 when socInfoAvailable false) — result-card only
double   cvpfRpmSum = 0.0, cvpfBattVSum = 0.0, cvpfSocSum = 0.0;   // accumulators, summed over the settled tail of the mid high+low segments
uint32_t cvpfCondN = 0;            // condition samples accumulated (settled tails of segments 3 & 4 → ~1 s of stable level each)
float    cvpfCapHeadroomA = 0.0f;  // A — alternator cap-table headroom at fit time (g_I_cap − cvpfBaseA); how much bigger a step the table allows
float    cvpfRebaseRpm = 0.0f;     // RPM at REBASE exit — same base current as cvpfSettleRpm, so the pair reads drift with load sag cancelled
float    cvpfDriftSetupPct = 0.0f; // signed % engine drift across setup (SETTLE exit -> REBASE exit, both at base current)
float    cvpfDriftTrainPct = 0.0f; // signed % engine drift across the pulse train (first vs last base segment, both at base duty)

// ── CV stress test (commissioning stage 8 / standalone from Tuning ▸ Stress Test) ──
// Throttle-snap acceptance test: with the regulator running NORMALLY at idle (command = RPM
// table with Lo/Hi and thermal derates — nothing overridden), the operator snaps the throttle
// through the RPM band where alternator output multiplies, and the watcher grades the recovery
// purely from the existing protection counters — every protection stays fully armed and untouched.
volatile bool cvStressActive = false;       // master flag (mutexes, idle-rest suppression)
volatile bool cvStressAbortRequested = false;
volatile bool cvStressForceCV = false;      // phases 2-3: AdjustField forces CV at cvStressTargetV (TargetVoltageMode-style)
volatile uint8_t cvStressPhase = 0;         // 0 idle 1 STAB_IDLE 2 STAB_CV 3 ARMED 4 DONE
float cvStressTargetV = 0.0f;               // achievable CV target the test computes (idle avg − CvStressDropV), driven while forced
float CvStressDropV = 0.10f;                // V (12V-equiv, ×V/12 at use) — target headroom: CV target parks this far below the settled idle voltage; smaller = more standing current at the parked target = harsher snap (the settle-up gate keeps the level a true equilibrium so CV, not the current limit, stays in command)
float CvStressFailBandV = 0.25f;            // V (12V-equiv, ×V/12 at use) — swing beyond this at a stabilization deadline = genuine instability, test cannot run; at or under it the phase proceeds with the swing reported as a marginal grade
const char *cvStressAbortMsg = "";
char cvsLastBlob[224] = "";                 // last persisted result CSV (NK_cvStressLast, ver-2), cached for /cvstress.json

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
int    cvStressJsonBuild(char *buf, int cap);
void   bhFlushCapNVS();
void   bhInitSettings();
void   bhDeserializeResults(const String &blob);
void   bhDeserializeCap(const String &blob);
String bhBuildStatusJson();
// Capacity tracker (OCV-anchored)
float  ocvToSoC(float restedV);
void   capTrackTick(float I_batt, float V_filt, float tempC, float dtSec);
void   capTrackOnFull(uint32_t epoch, float tempC);
String capSerializeOcv();
void   capDeserializeOcv(const String &blob);
bool   ocvIsAnyPreset();
void   applyChemistryOcvPreset();

// ── Tuning→Current closed-loop sine generator (Stage 2) ───────────────────────
// Waveform: 0 = square (existing ISE tuning), 1 = sine manual, 2 = sine auto-sweep.
// Sine modes drive a sine setpoint (reference) and let the current PID chase it, then
// measure measured-vs-reference gain/phase = the closed-loop frequency response.
int     tuningWaveform     = 0;        // 0 square, 1 sine manual, 2 sine auto-sweep
float   tuningSineFreq     = 2.0f;     // Hz — manual sine frequency
float   tuningSweepStart   = 0.2f;     // Hz — auto-sweep low end
float   tuningSweepEnd      = 15.0f;   // Hz — auto-sweep high end
uint8_t tuningSweepCycles   = 2;       // analysed cycles per frequency (1 settle cycle skipped); integer-cycle windowed, so 2 is clean
volatile bool tuningSweepRequested = false;  // UI "Run Sweep" → start/restart the sweep
bool    tuningSweepActive   = false;   // sweep in progress
bool    tuningSweepDone     = false;   // sweep finished, tuningBode[] valid
#define TUNING_SWEEP_NPOINTS 10        // log-spaced sweep points
struct TuningBodePoint { float freqHz; float gain; float phaseDeg; };  // gain = measured/reference (unitless)
TuningBodePoint tuningBode[TUNING_SWEEP_NPOINTS] = {};
uint8_t tuningBodeCount = 0;           // valid points (served by /tuningbode)

// Sine-sweep run-condition trackers — reset on sweep start, snapshotted into the record at commit.
// baseA/ampA describe the injected waveform; rpmMin/Max + worst coherence + duty-rail flag describe
// how trustworthy the run was (a sweep at a wandering RPM or a railed field isn't comparable).
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
// Protection-abort latch (latched in applyImmediateCut, first cause wins, cleared at sweep start).
// Served by /tuningbode so the dashboard can name the real cause and discard the run. Pre-latch, the
// sweep ran on through the cut + lockout, the low duty rail set tuningSweepDutyRailed, and the
// dashboard blamed field saturation / cruising speed for what was really a protection cut.
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
  uint32_t tempSampleCount;   // NON-NAN temp samples only; avgSampleCount counts every sample, so
                              // dividing by that fabricated 0.0F averages when no probe was bound
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

// ===== Control Accuracy v4 — routine-data loop health. Full design:
// Working Markdown Docs/CONTROL_ACCURACY_V4_ROUTINE_SPEC.md. No episodes, no reference
// model. Per loop, each valid tick lands in ONE bucket, priority CONSTRAINED (output at a
// rail / protection clamp — reported as its own number, never graded as loop error) >
// ACTIVE (command moving or recent challenge — the graded tracking denominator) > QUIET
// (steady dwell — earns nothing toward the graded number). Band exits run an excursion
// stopwatch (count + out-of-band seconds → mean recovery time, dilution-immune); damaging-
// side exposure and worst peak are RECORDED unconditionally within valid time (facts about
// what the battery/alternator experienced, separate from graded blame). Reset only by a manual
// Reset (the post-upload auto-reset is disabled — see buildConfigPayload); that Reset also discards live stopwatches.
// Accumulators are DOUBLE on purpose: a float32 running sum silently STOPS growing once
// it passes ~a day of accumulated time (per-tick dt rounds below the float ULP).
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

// Live excursion stopwatch — no commit/void machinery: the count is taken at confirmed
// band exit; out-of-band time PAUSES while the loop is constrained in the direction the
// error needs (rail/clamp physics is not tracking failure — v3's one kept authority idea).
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
const uint32_t ACC_SETTLE_THERMAL_MS = 120000;  // thermal: require sustained binding (kept from v2)
const uint32_t ACC_THERM_CV_YIELD_MS = 10000;   // CV must own the command this long straight to end a containment session; below it the plant (25s slope window) cannot move enough to corrupt the score

// === CV Loop Tuning Score System ===
float cvWaveAmplitudeV = 0.30f;   // V — target rises by this during HIGH phase (LOW phase sits at the real target)
int cvWavePeriodSec = 30;         // s — full period of the Waveform Generator wave (one LOW + one HIGH); each half-period = this / 2. UI minimum 8s; the UI warns below ~14s where a HIGH half no longer fits settle + CV_P2P_SKIP_MS + CV_P2P_EVAL_MS, so the p2p metric reads n/a (scoring still works).
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
// PidKp/Ki/Kd are 12V-EQUIVALENT gains — the same numbers work on 12/24/36/48V banks. recomputeCcGains()
// bakes in x(12/SYSTEM_VOLTAGE_CLASS) to reach the duty-space gains the loop applies (PidK*_active), because
// field current per duty-% scales with bus voltage. Do NOT scale these per-class by hand.
// Seeds are the 2026-07-03 plant fit (tau 112 ms, IMC) de-rated 30%: that fit is one alternator at one
// operating point, and a fresh install on a different plant oscillates at full gain. The Ki/Kp ratio
// (integral zero, 8.9 rad/s) is preserved — loop gain only, not a reshape. Commissioning overwrites both.
float PidKp = 0.4662f;  // 12V-equivalent proportional gain
float PidKi = 4.5384f;  // 12V-equivalent integral gain
float PidKd = 0.0f;  // 12V-equivalent derivative gain (default off; current-loop D adds noise, little benefit)
volatile float PidKp_active = 0.579f;  // DERIVED duty-space gain the PID uses (PidKp × 12/SYSTEM_VOLTAGE_CLASS). Set by recomputeCcGains().
volatile float PidKi_active = 5.150f;  // DERIVED duty-space Ki.
volatile float PidKd_active = 0.0f;  // DERIVED duty-space Kd.
// --- Voltage (CV) PID ---
// ── CV gain-mode system (Auto α/K vs Manual) — see Working Markdown Docs/CV_AUTOTUNE_PLAN.md ──
// VoltageKp/VoltageKi below are the MANUAL gains (12V-equivalent space). The control loop never reads
// them directly — it reads VoltageKp_active/VoltageKi_active, which recomputeCvGains() derives from the
// selected mode and bakes the 12V-block normalization (×12/SYSTEM_VOLTAGE_CLASS) into.
uint8_t cvGainMode   = 0;         // 0 = Manual (use the typed VoltageKp/Ki) — DEFAULT, so a fresh/never-commissioned
                                  // device runs the seeded manual gains; 1 = AUTO (ohmic-anchored, Kp = α/K).
                                  // The CV plant-fit commissioning step flips this to 1 (Auto) on Apply (cxCVPlantApply).
// cvLambdaMult's old CSV3 slot survives as the reserved placeholder CSV3_EXTRA1 so the field count is unchanged.
// cvPlantKa = measured ~0.6 s stiffness (V/A at the pack, ohmic + early soak), the AUTO anchor (Kp = α/Ka).
// Pre-commission default deliberately HIGH: bigger Ka → smaller Kp → gentle until a real measurement lands.
float   cvPlantKa    = 0.0216f;
float   cvPlantKb    = 0.0f;      // retired √t tail — always 0; NVS key kept so old exports/POSTs still parse
float   cvPlantK     = 0.0216f;   // DERIVED, not persisted: = cvPlantKa. Telemetry + dashboard only.
// Measured ripple projection (RIPPLE_DETECTION_REARCH_SPEC §3.3). The commissioning current-check
// runs a 3-level test per detector and line-fits ripple(I) = a0 + a1·I to the IExcessTau-filtered
// excess pk-pk (the same quantity the detector trips on). This is REFERENCE DATA ONLY — it drives the
// ripple-vs-threshold plot on Protections so the operator can see whether their floor/ceiling/% clear
// the real ripple. It NEVER moves the over-current floor (no auto-floor, no tracking slope).
struct RipFit {
  float a0, a1;      // ripple(I) = a0 + a1·I (A pk-pk of the IExcessTau-filtered excess)
  float rpm;         // engine RPM the 3-level test ran at (0 = no test yet)
  float iPt[3];      // the 3 commanded operating currents (A)
  float pkPt[3];     // measured filtered-excess pk-pk at each level (A)
  uint8_t nPts;      // valid measured points (0 = no fit → plot shows threshold line only)
};
RipFit ripFitAlt  = {0};   // alternator (G3) detector — ADS1115 MeasuredAmps path
// Same struct reused for the CV D-term deadband measurement: a0/a1 = b0/b1 of the worst-positive-
// voltage-slope fit slope(I)=b0+b1·I (V/s of g_cvKdFiltV, the D term's own filtered signal), pkPt =
// the 3 measured slope points. Captured in the same Step-5 sweep windows as ripFitAlt; reference for
// the deadband card + wizard Set. Persisted under NK_slpFitAlt.
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
float   cvComputedKp = 30.0f;     // user-facing (12V-equivalent) gains the Auto path produced — dashboard display only
float   cvComputedKi = 25.0f;
float   cvComputedKd = 5.0f;      // Auto path's D gain = CvKdTd·cvComputedKp (12V-equiv); dashboard display only
// CV auto-gain knobs (Tuning ▸ Voltage): Kp = cvAlpha/(cvPlantKa·vNorm), Ki = cvPiZero·Kp. α is
// dimensionless (fraction of the deadbeat gain 1/K), so one α gives every chemistry the same character.
float   cvAlpha      = 0.05f;     // fraction of deadbeat 1/K; coupled to the ~0.6 s CVPF horizon — retune together
float   cvCrossover  = 0.20f;     // rad/s — retired ω_c input, inert; NVS key + CSV3 slot kept (never repurpose)
float   cvPiZero     = 0.50f;     // rad/s — PI integral zero ρ; Ki = ρ·Kp
float   CvKdTd       = 0.85f;     // s — derivative time; Auto-mode Kd = Td·Kp (D analog of ρ), so Kd inherits Kp's α/K plant anchor and scales per-install through the wizard
// Battery-temperature gain derate. Board temp (ambientTemp, °F) is a PROXY for battery temp; as the
// battery cools its internal resistance — which IS the CV plant gain K_dc — rises, so gains computed at
// the commissioning temperature run too hot when it's colder (and sluggish when warmer). We counter-
// scale Kp AND Ki by the resistance ratio R(T_commission)/R(T_now), holding the loop bandwidth ω and
// damping ρ put (NOT a λ/ω change). CommissionTempF is stamped when the CV plant fit is applied; NaN =
// never commissioned → no derate. See recomputeCvGains()/computeCvTempScale() in 6_functions.ino.
float   CommissionTempF      = NAN;   // battery temp (°F, batteryTempF() source; board temp on legacy stamps) when K_dc was measured; reference for the derate
int     CommissionTempSrc    = 0;     // battTempActiveSrc at the moment CommissionTempF was stamped; 0 = legacy/unknown = treat as board (5)
bool    battTempDerateEnable = true;  // master on/off for the battery-temp gain derate
float   battTempCoeff        = 0.024f; // fractional resistance change per °C BELOW 25°C (R rises as T falls); cvResistanceRatio()
                                       // halves it above 25°C. One value for every chemistry — LFP and lead-acid measure
                                       // within 12% of each other over −20…+55°C, so a per-chemistry split isn't supported.
// Re-commission nagging. The bank's internal resistance — the CV plant gain the commissioning fit
// measured — roughly doubles over a bank's life, and a Vessel Info voltage/capacity/chemistry change
// invalidates the fit outright. Both drift the loop the same way: gain too high, response too fast.
// Epoch is stamped from the BROWSER clock at commissionDone (the device soft clock can be unset at sea,
// and a bad stamp would silence the prompt forever). 0 = never finished a pass ⇒ never nag.
time_t  CommissionEpoch      = 0;
bool    commissionAgeAck     = false; // user pressed Silence on the age prompt; cleared by the next pass
bool    commissionChangeFlag = false; // tuning-affecting Vessel Info change since the last pass
uint8_t regulatorMountLoc    = 0;     // 0 = battery compartment (board temp tracks the bank), 1 = engine room
float   cvTempDerateScale    = 1.0f;  // live multiplier recomputeCvGains() applies to the active gains (dashboard diag)
// CV voltage-target slew (bidirectional ramp). ChargingVoltageTarget ramps toward ...Req at these rates so a
// COMMANDED target change (absorption→float, manual setpoint) no longer steps instantly and trips fast-OV; the
// CV loop AND the OV protections see the smooth value. 0 = instant for that direction. Hard-shutdown unaffected.
uint8_t vTgtRampEnable = 1;       // master switch for the target slew (1=on, default). 0 = instant target,
                                  // byte-identical to pre-ramp behaviour; lets you A/B the limiter, incl. in CV tuning.
float   vTgtRampUp   = 0.15f;     // V/s — max rate the target may RISE  (up-steps are loop-limited anyway)
float   vTgtRampDn   = 0.15f;     // V/s — max rate the target may FALL  (protection-reference glide; the wind-down governor below owns the actual current shed on commanded drops)
// Commanded-target wind-down governor: on a commanded target DROP, walk the current command down at a
// deliberate rate and stop on the MEASURED bus instead of asking the PI to track within the OV margin
// (PI shed rate = KiDown×error ≈ 3.5·cvAlpha·margin V/s ≈ 16 mV/s — a 0.6 V drop through the glide alone
// left the bus +110 mV over the gliding target and fired G2 three times, 2026-07-21 FAIL log).
uint8_t cvWindDownEnable = 1;         // master switch (0 = old behaviour: PI + glide alone)
float   cvWindDownRate   = 0.05f;     // fraction of MaxTableValue shed per second (self-scales with alternator size)
float   cvWindDownStopV  = 0.05f;     // V real per-bus — stop walking when filtered bus is within this of the commanded target (class-scaled at store)
const float CV_WINDDOWN_TRIG_V = 0.15f;  // V ×class at use — commanded drop must exceed this to arm (small trims stay pure PI)
const float CV_TGT_PACE_LEAD_V = 0.02f;  // V ×class at use — down-glide holds while getFiltV() lags more than this behind the reference (bus-paced descent floor). Must stay well under OvMeasMarginV minus the filtered ripple crest (~70mV worst bench) or ripple can still bridge the gap
bool  cvWindDownActive = false;       // cleared by runShutdownPath/runCommissionIdle/MANUAL like lastVoltageControlActive
float cvWindDownCap    = 0.0f;        // ramped ceiling on Icv (and cv_I) while active
float cvWindDownFinalV = 0.0f;        // commanded final target latched at arm; follows further drops, abort on raise
volatile float VoltageKp_active = 8.5f;  // DERIVED pack-space Kp the loop uses (selected gain × 12/SYSTEM_VOLTAGE_CLASS). Set by recomputeCvGains().
volatile float VoltageKi_active = 6.0f;  // DERIVED pack-space Ki the loop uses.
volatile float VoltageKd_active = 10.0f; // DERIVED pack-space D gain the loop uses (VoltageKd × 12/SYSTEM_VOLTAGE_CLASS × temp derate). Set by recomputeCvGains(), same pipeline as Kp/Ki.
volatile float VoltageKp = 8.5f;    // A/V — proportional gain (volatile: written from Core 0 web handler, read from Core 1 PID); from commissioning CV plant fit (K20=32.2 mV/A, 12 V-equiv)
volatile float VoltageKi = 6.0f;    // A/(V·s) — integral gain; above-target unwind uses KiDown = 7×VoltageKi; from commissioning CV plant fit. Deferred cv_I anti-windup still open (see CV_Loop_Dev_Summary.md Future Work)
// CV D term — trim is a position subtracted at the Icv output, never integrated into cv_I, so it
// releases the tick the rise stops. Replaced the integrating slope bleed (over-dosed via ~0.6 s actuator lag).
float VoltageKd = 5.0f;             // A/(V/s), 12V-EQUIVALENT (runtime-normalized like Kp/Ki — do NOT class-scale at seed); knee: trim hits CvKdMaxTrimA at slope = cap/Kd
float CvKdDeadbandVps = 0.4f;       // V/s at zero amps — BASE of the deadband line clamp(CvKdDbFloor, base + slope·I, CvKdDbCeil); hysteretic gate: D off below the line, FULL slope once past, re-arms at half. This is a SAFETY MARGIN, not a threshold: commissioning's Step-5 ripple fit measures the real threshold and overwrites base + CvKdDbSlope, placing the line this margin above measured ripple. 0.4×class here is the pre-commissioning fallback ONLY (uncommissioned / skipped-fit device), where CvKdDbFloor (0.10) is the hard runtime minimum
float CvKdDbSlope = 0.0f;           // V/s per A — deadband line slope vs the slew-limited command (ripple-driven slope noise grows with output, same physics as the G3 trip line); 0 = flat, the pre-measurement behavior; set from the Step-5 slope fit b1
float CvKdDbFloor = 0.10f;          // V/s — lowest the evaluated deadband may go (guards the low-current extrapolation below the tested sweep span)
float CvKdDbCeil  = 3.5f;           // V/s — highest the evaluated deadband may go; MUST stay below CvKdSlopeCeil: the gate compares cvDSlope already clamped to ±CvKdSlopeCeil, so a deadband at/above it could never be crossed and D would go dead at that current
bool  CvKdExcessMode = true;        // true = trim ∝ (slope − deadband line), continuous from zero at the line — no engagement step, so a small coherent swing can't self-sustain (the 2026-07-21 1.4 Hz hold limit cycle). false = legacy full-slope hysteretic latch (bench A/B only): trim jumps to Kd×deadband at the line — that step is a relay behind the ~0.2 s field lag
bool  CvKdOneSided = false;         // false = two-sided: also adds on a fast fall, ONLY below target (that gate is what makes two-sided safe)
float CvKdArmV = 0.5f;              // V below target within which D acts (0 = always); tight so D stays off the sag-recovery climb and only brakes the final approach to setpoint (1.25 armed 1.25V early and chopped the recovering field)
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
// Detection = time-averaged current excess over command (an EMA of the signed deviation),
// thresholded against a FRACTION of the command. The averaging cancels any zero-mean
// oscillation (belt resonance, stator imbalance, rectifier ripple) to ~0 before the
// threshold is applied.
// See Working Markdown Docs/iExcess_Redesign_Spec.md and CV_Loop_Dev_Summary.md.
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
float ReseedFrac = 0.95f;  // shunted installs: fraction of pre-event cv_I to seed on any protection recovery (the measured demandDrop already corrected the base). 0.75→0.95 2026-07-21: blip log showed the seed dock (not the clamp) caused the multi-second below-target tails — tails now capped by the bus-watched climb floor (CvRecovClimbRate). Must stay <1.0 (first-fire de-escalation on a persistent cause).
float ReseedFracNoShunt = 0.50f;  // no-shunt installs: the departed-load measurement needs Bcur, so a load-dump fire reseeds blind. Seed low and let the bus-watched climb rediscover demand — 22:17 07-22 log: 0.95 blind seed after a 24A dump re-fired G2 twice
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
// User-controlled flag (per test page). When TRUE (default) G1, G2, the timed OV cut tiers
// (OvTierLo/Mid), the G3 iExcess over-current detector (alternator), and the G4 battery current
// limit all fire normally. When FALSE the user has disabled them so step-tests can characterise
// the plant without protection layers fighting the test. NOT persisted — resets to TRUE
// (enabled) on every boot. The absolute rungs stay active regardless of this flag:
// AlternatorHardShutdownV (software instant cut), INA228 hardware OV, Load Dump (the G5
// battery rate-of-change tiers), and the hardware OC trip (MaxTableValue+10).
bool testProtectionsEnabled = true;
bool commissionProtBackup   = true;  // saved value of testProtectionsEnabled at commissionStart; restored on done/abort. Commissioning forces protections ON for its run so a stray manual "off" from the tuning-tab sliders can't weaken the wizard's steps (the manual sliders are scoped to manual tuning only).
// --- CV loop runtime state ---
uint32_t lastVoltageLoopMs = 0;           // timestamp of last voltage loop update
uint16_t g_voltLoopActualIntervalMs = 0;  // actual interval of last voltage loop fire (ms); 0 until second fire
float g_kdTrimThisTick = 0.0f;   // D-term current reduction latched for the next cvLog entry (A) — most recent nonzero trim since the last log write; cleared by cvLog_tick after logging
float voltageTargetSlewed = 0.0f;        // rise-governor output — the setpoint the CV PI tracks. Global (not a static local) only so cvLog_tick can record it; written solely by the governor in AdjustFieldLearnMode.
float Icv = 0.0f;                         // CV PID output — direct current setpoint (A)
float cv_I = 0.0f;                        // CV integrator state (A)
bool voltageControlActive = false;        // true when voltage PID is active (non-idle stages)
bool lastVoltageControlActive = false;    // CV-entry tracker for the bumpless seed. Maintained by the CV block and the MANUAL branch; runShutdownPath/runCommissionIdle return before that line, so they clear it themselves — left stale-TRUE there, a re-enable skips the entry seed and resumes a stale cv_I (2026-07-20 double fire). Deliberately NOT cleared by the in-AUTO test branches: their capture/restore contract (cvContextUnchanged) relies on it staying set.
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
volatile bool rpmAxisWipePending = false;  // NK_RpmAxisWipePend: cloud half still owed; suppresses front sync
volatile bool pendingSaveUserTableEdits = false;
volatile bool pendingSaveVesselInfo = false;
bool pendingShutdownFlush = false;     // set on ignition-off edge; cleared after full flush
bool shutdownNVSFlushDone = false;     // true once NVS+sensor window saved this shutdown

// ── Commissioning ledger (COMMISSIONING_LEDGER_SPEC.md) ──────────────────────
// Append-only event evidence (stage completions, skips/manual marks, finish, Bode sweeps)
// staged in PSRAM → /cxledger.bin → log-commissioning-event edge fn, drained under the same
// field-off cloud gates as every other upload. Producers run on BOTH cores (web handlers +
// sweep commits), so staging is spinlock-guarded; ALL file access is Core 1 only.
#define CX_LEDGER_PATH       "/cxledger.bin"
#define CX_LEDGER_FILE_CAP   32768u   // on-flash queue cap; oldest rows dropped when full
#define CX_LEDGER_PEND_CAP   8192u    // PSRAM staging cap per buffer (double-buffered swap)
#define CX_LEDGER_ROW_MAX    4096u    // sanity bound per row (biggest real row ~1.2 KB)
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

// Field-off flush triggers — two staggered gates off the same GPIO4-cut edge.
// +20s: saveNVSDataFull()      drains the storage namespace to NVS (heavy commit).
// +28s: altHealthSave() writes the alt-health blobs (/altbase.bin + 3 more) to LittleFS (~67 KB).
// 20s, not 5s: 2x the fast-OV lockout cap (10s), so OV cut/retry churn can never reach the
// commit — no flash work during dynamic protection events (user policy 2026-08-13). Long
// static lockouts (tach-lie deep rungs, cold-charge) may exceed 20s and flush mid-lockout;
// field is hardware-dead there, deliberate. Must grow if the fast-OV ladder cap grows.
// Staggered so the LittleFS write doesn't pile onto the NVS commit's flash relocation tail,
// and both land well before the buffered-upload TLS handshake at 70s. Both re-arm on the
// next field-on edge.
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
  MODE_NORMAL_AUTO_PID,           // PID control with learning
  MODE_COMMISSION_IDLE            // Commissioning "rest" between guided steps: hold field at a low
                                  // duty (per-RPM floor or a voltage-scaled minimum), slow ramp, no
                                  // GPIO4 cut — keeps the RPM pickup alive instead of resuming charging
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
  REASON_BATTERY_TOO_COLD,         // board temp (battery proxy) below MinChargeTempF — cold-charge lockout (opt-in, lithium protection)
  REASON_COMMISSION_REST,          // commissioning idle-rest hold between guided steps (not a fault)
  REASON_TACH_IMPLAUSIBLE,         // field driven hard, zero alternator output while tach claims running — open field drive (ON/OFF/wiring/gate-drive), dead alternator, or false RPM (tach noise)
  REASON_SOLAR_PAUSE,              // weather mode is resting the alternator on a strong solar forecast — deliberate, not a fault
  REASON_BMS_DISABLED,             // the BMS on/off input is withholding permission — external command, not a fault
  REASON_OV_TIER_LOW,              // timed OV cut, LOW tier — filtered bus held above target + OvTierLoMarginV for OvTierLoDwellMs (AUTO/CV only)
  REASON_OV_TIER_MID,              // timed OV cut, MID tier — filtered bus held above target + OvTierMidMarginV for OvTierMidDwellMs (AUTO/CV only)
  REASON_BATTERY_TOO_HOT           // 24 — measured battery temp (source 1..4, never the board stand-in) above MaxChargeTempF — hot-charge lockout (opt-in)
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

  bool tempDataVeryStale;
  bool ignoreTemperature;
  bool ignoreRPM;
  bool rpmBelowMinimum;
  bool engineFullyStopped;   // RPM confirmed at 0 for >= RPM_ZERO_CUT_MS — forces immediate field cut
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

  bool batteryTooCold;   // cold-charge lockout active (board temp proxy < MinChargeTempF, with hysteresis)
  bool batteryTooHot;    // hot-charge lockout active (measured battery temp > MaxChargeTempF, with hysteresis; never from the board stand-in)
  float battTempF;       // batteryTempF() this tick (°F); NAN = no source qualifies
  uint8_t battTempSrc;   // batteryTempF() source code (0 none, 1 probe, 2 NMEA 2000, 3 VE.Direct, 4 RV-C, 5 board)
  bool commissioningResting;  // commissioning session live + dialog alive + no test running → hold field at rest duty
};

// ==================== CONFIGURABLE PARAMETERS ====================
// Expose these in web UI for tuning

// --- Rate Limiting (LM2907 coupling cap protection) ---
float DutyRampRate = 40.0f;  // %/sec - max rate of duty cycle change (protects coupling cap from harsh transitions, includes OnOff toggle!)
uint8_t dutySlewEnable = 1;  // master switch for the field duty slew (GOV_NORMAL_SLEW). 0 = duty steps instantly (old behaviour, A/B); applies in BOTH commissioning tests and normal operation
// Asymmetric setpoint slew
float SetpointRiseRate = 30.0f;  // A/sec
float SetpointFallRate = 50.0f;  // A/sec
float CvBrakeFallRate = 200.0f;  // A/s — brake-tier setpoint down-slew while the CV D-term removes current; between the 50 A/s comfort limit and the fastOvClamp slam; flat amps
uint8_t setpointSlewEnable = 1;  // master switch for the inner-loop current setpoint slew. 0 = setpoint steps instantly (drops startup ramp / big-step gentling too); applies in the CC test and normal operation
// Manual CC square-wave test slew mode (Current-tab Test Limiters). 0=Off (setpoint abrupt + duty bypassed),
// 1=Default (factory setpoint rate below; field keeps its DutyRampRate protection), 2=Custom (user
// SetpointRiseRate/FallRate). Default 2 = prior behavior. Governs ONLY the manual square test — never
// commissioning/auto tests (g_autoTestActive) or sines. setpointSlewEnable above still governs normal op.
uint8_t testSlewMode = 2;
const float SETPOINT_RISE_DEFAULT = 30.0f;  // A/s — factory setpoint slew for testSlewMode=Default (matches SetpointRiseRate's own default)
const float SETPOINT_FALL_DEFAULT = 50.0f;  // A/s
// Manual CV square-wave test slew mode (Voltage-tab Test Limiters). 0=Off (target steps instantly),
// 1=Default (factory target ramp below), 2=Custom (user vTgtRampUp/Dn). Default 2 = prior behavior. Governs
// ONLY the manual CV square test — never commissioning/auto tests. vTgtRampEnable still governs normal op.
// Unlike the CC test, Off keeps the base field duty slew engaged — a conservative default, NOT a requirement:
// the field-slew safety that matters is the controlled ramp-to-zero at test EXIT (the cvWaveExitWindDown
// glide), not the base rate during the test. Bypassing duty here (like the CC Off path) is a valid future
// change if a need arises.
uint8_t cvTestSlewMode = 2;
const float VTGT_RAMP_DEFAULT = 0.050f;  // V/s @12V — factory target ramp for cvTestSlewMode=Default; scaled ×SYSTEM_VOLTAGE_CLASS/12 at use
uint8_t cvRiseGovEnable   = 1;   // master switch for the CV voltage-target rise governor (anti-windup clamp). 0 = rises are not clamped — integrator can wind up into an OV trip on an up-step; applies in the CV test and normal operation
uint8_t cvRecovEnable = 1;   // master switch for the post-protection integrator refill (deficit-gated Ki boost, replaced the timed window 2026-07-21). 1 = up-integration runs at Ki × up-to-cvRecovKiMax, tapering with the remaining reseed deficit; 0 = plain PI walks itself back (pace scales with post-cut error). Default ON since the 07-22 lithium A/B (single fire, ~5× refill rate, healed exit, no overshoot)
uint8_t loadServeBoostEnable = 1;  // load-serve Ki boost (07-22): while the battery shunt shows sustained discharge in CV, up-integration runs at the refill's boosted rate toward the measured house loads — a stiff plant hides a 35A load behind ~0.25V of error, so plain Ki picks it up in ~30s. Shunt-gated; inert without one. Dev default ON, production TBD
uint8_t reseedCorrEnable = 1;      // demand-corrected reseed (07-22, the 8-fire train): subtract the measured load drop at the fire from the release reseed + refill goal (shunt-gated), and escalate the ratchet ×0.85 per re-fire <1.5s after release (shunt-independent). OFF = plain ReseedFrac reseed
// --- Hunt Governor v2 (persistent speed-map oscillation damper — HUNT_GOVERNOR_V2_SPEC.md) ---
uint8_t HuntGovEnable = 0;           // 1 = watch applied duty for a sustained 0.3–3 Hz hunt (idle-knee speed-coupled plant, 07-22 22:16 AGM logs); a verified single cut to HuntCutPct is mapped as a persistent speed pocket and applied proactively on pocket entry. Opt-in (operator decision 08-20); NVS-seeded units keep their stored value
volatile float g_huntDerate = 1.0f;  // live Ki multiplier; applied inside recomputeCcGains so every SetTunings path preserves it. In v2 this is the map value at the current speed (HuntCutPct/100 inside a pocket core), or the held test cut during an episode
volatile float g_huntKdScale = 1.0f; // live CV D-term multiplier (second damper lever, 08-21 D-driven variant): 0 during a D test and inside a verified D-pocket core, wing-tapered between. Applied at the kdTrim choke point every tick — a PARTIAL D cut only moves the deadband limit cycle to a larger amplitude, so the D lever is binary off, never HuntCutPct
float g_huntFreqHz = 0.0f;           // last confirmed wobble frequency (CSV1/ledger)
uint8_t g_huntState = 0;  // 0 watching (gain follows the pocket map), 1 testing Ki (single cut applied, verdict pending), 3 cooldown after a failed test, 4 testing the CV D-term lever (D paused, Ki untouched); 2 is unused in v2 (was the v1 hold/creep state) — file-global so CSV1 can stream it to the Diag panel
uint8_t HuntCutPct = 50;       // gain the damper cuts TO during a test and holds inside a pocket core, % of user Ki. One big cut gives the verify comparison its S/N; also the map floor — repeat episodes widen pockets, never deepen. Clamp [25,80]
uint8_t HuntVerifyPct = 15;    // required ripple reduction (%) for a test to pass, 3-scan average vs 3-scan average — single windows jitter more than this on their own. Clamp [5,60]
uint8_t HuntWingPct = 15;      // pocket edge taper width, % of speed beyond each core end over which gain ramps linearly back to full. Clamp [5,40]
uint8_t HuntCooldownMin = 10;  // minutes after a failed test before another test may open. Clamp [1,60]
uint8_t HuntSteadyPct = 3;     // engine-speed steadiness tolerance (%): scan-mean RPM must stay within this of the episode reference through qualify+verify, else the episode aborts with nothing kept. Clamp [1,10]
uint8_t HuntQualifyScans = 3;  // consecutive hunt+steady scans (1.6 s each) required to confirm a wobble and open a test; their average is the verify baseline. More scans = slower but surer detection. Clamp [2,6]
float HuntTrigPct = 0.5f;      // detection bar: peak-bin field-duty swing (%) a scan must beat to count as a wobble. Deliberately UNCAPPED above - the duty amplitude of a given wobble scales with 12/SYSTEM_VOLTAGE_CLASS (same field watts, quarter the duty at 48 V), and real 12 V bench episodes land at 0.39-0.62% against the old fixed 0.5. Floored at 0.01 only because <= 0 would make every scan a detection. The D-attributed relaxed bar tracks this at 0.3x
float cvRecovSec = 2.5f;     // retired timed-window ramp span, inert; NVS key + CSV3 slot kept (never repurpose)
float cvRecovEmaxV = 0.25f;  // retired timed-window error cap, inert; NVS key + CSV3 slot kept (never repurpose)
float cvRecovKiMax = 5.0f;   // refill Ki multiplier at the moment of release (M tapers linearly to 1× as the deficit heals). Clamp [1,10]
// Error-scheduled CV P-boost: whenever the bus is below target in CV — NOT gated to protection
// events; post-protection recovery is the motivating case, but CV-entry climbs and load dips get
// it too — the proportional term is scaled up to command transient overcurrent that climbs out of
// the voltage hole faster than the Ki-limited crawl, then tapers to 1× at target so it self-disarms
// before it can re-fire an OV. Only the P term is scaled — cv_I and the D trim are untouched.
// Independent of cvRecovEnable (works with the refill off). The bare loop's peak recovery
// current sits ~5% above the pre-trip hold, so without this the post-cut return is integrator-paced.
uint8_t cvRecovBoostEnable = 1;   // master switch for the recovery P-boost. DEFAULT ON — seen live at 3×/1.5V (2026-07-20 G2 recovery, ~2.4× in the valley); 4×/0.6V untested — turn off if a faster climb re-trips OV on the approach
float   cvRecovBoostMax    = 4.0f;  // P-gain multiplier at ≥ cvRecovBoostErrV of shortfall; 1.0 = no boost, taper is linear from 1× at target. Clamp [1,8]
float   cvRecovBoostErrV   = 0.6f;  // shortfall (V, per-12V-block — scaled ×SYSTEM_VOLTAGE_CLASS/12 at use) at which the boost reaches cvRecovBoostMax. Clamp [0.1,5]
float   cvRecovBoostFloorV = 0.2f;  // dead area (V, per-12V-block — scaled ×class at use): no boost within this shortfall, ramp starts at 1× from its edge. Keeps boost gain out of small-signal regulation (the 07-24 0.7 Hz idle wander was boost-sustained). Clamp [0,5]; 0 = boost from zero shortfall
float   cvRecovDeepBandV = 0.75f;  // deep-recovery band (V, per-12V-block — scaled ×class at use): shortfall beyond this is DEEP — the starve-walk speeds up (cvRecovDeepMult) with a widened pause band, and the OV-episode P-boost ramp slides up to start here instead of going flat-off. Every observed recovery re-fire happened ABOVE target, so depth below this band cannot be the echo the episode pause guards against (12:45 07-25 stuck: flat-off + 1.5A/s walk held the bus 2V low for 45s). Clamp [0,5]; set large (5) to disable deep mode entirely
float   cvRecovDeepMult  = 5.0f;   // starve-walk rate multiplier at full depth (2× cvRecovDeepBandV of shortfall), ramping linearly from 1× at the band edge. Clamp [1,20]; 1 = walk at the near-target crawl everywhere
float   cvRecovFlareBandV = 0.35f; // arrival flare band (V, per-12V-block — scaled ×class at use): within this shortfall the recovery ceiling tapers from the goal down to cvRecovFlareFrac×goal at target, landing with ~zero surplus instead of the full pre-trip current (both 07-25 re-fires arrived carrying ~7-10A of surplus into a surface-charged battery). Clamp [0,2]; 0 = no flare
float   cvRecovFlareFrac = 0.85f;  // flare ceiling at zero shortfall, fraction of recovCvGoal — the PI re-adds the held-back amps at its own pace after arrival if the load truly needs them. Clamp [0.5,1]; 1 = no flare
bool    g_autoTestActive  = false;  // set per control-loop tick: an automated/guided test owns the limiters now (commissioning / battery-health DCIR / resonance / system-ID). While true the four user limiter toggles above are inert so a stray user setting can't corrupt a measurement; each test's own built-in slew behavior governs.
const float TEST_ENTRY_RATE_A = 8.0f;  // A/s — FIXED, conservative ramp rate for an automated test's transition up from the rest floor and back down (NOT the user's SetpointRiseRate/FallRate, which a user could set aggressively). Used by DCIR, the resonance check, and the TuningMode entry ramp so a test never slams the field coming on/off. Current-domain, so no bus-voltage scaling.
const float CVPF_ENTRY_RATE_A = 50.0f; // A/s — cvpf current-PID phase setpoint rate; the measured edges are direct-duty, so this only paces the practice run
bool    g_tuningEntrySettled = false;  // set per tick from tuningEntryRamped: false while a TuningMode test is still ramping up from rest, true once settled. Gates the sine duty-slew bypass so the entry eases in (duty slew engaged) before the actuator is unclamped for the clean sine.
// Large up-step gentling: up-moves whose remaining gap exceeds SetpointBigStepThresh climb at the
// slower SetpointBigStepRiseRate (instead of SetpointRiseRate) until the gap closes inside the
// threshold; small corrections within the threshold keep full SetpointRiseRate responsiveness.
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
// AlternatorHardShutdownV: absolute battery voltage above which the alternator field is cut
// instantly (raw per-tick, all modes incl. MANUAL) and the adaptive fast-OV lockout starts.
// Its place in the OV ladder: above it sits ONLY the INA228 hardware ALERT (VoltageHardwareLimit,
// the electrical backstop for a hung MCU) — software always gets first shot; below it sit the
// timed OV cut tiers (OvTierLo/Mid, dwell-debounced field cuts) and the Group 1/2/3 throttling
// protections. Guidance: hardware rung 0.2 V x class/12 below the battery's BMS
// charge-disconnect voltage, this software cut 0.1 x class/12 under the hardware rung; the
// chemistry-specific values arrive via the battery-defaults proposal (AGM/flooded get an
// absolute ceiling sized to the DC loads' continuous ratings — lead-acid damage is
// time-integrated, indifferent to brief bounded spikes).
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

// Fast OV supervisor instrumentation — unified across all protection paths
// (Group 1/2 OV, iExcess, LoadDump). Exported from the bumpless tracker block
// in AdjustFieldLearnMode after every supervisor has voted, so all three flags
// below reflect the true combined state, not just Group 1/2.
float g_fastOvCurrentCap = 0.0f;   // live unified cap ceiling this tick (amps)
volatile bool g_fastOvClampActive = false;  // true if ANY protection capped current this tick
uint32_t g_fastOvClampCount = 0;   // rising-edge counter across all protections
// True while the CV anti-windup cap has not yet recovered to full after a protection trip — i.e. the
// loop is deliberately holding current below where free integration would put it, so voltage reading
// under target is by-design recovery, not a tracking fault. Gates the CV accuracy score.
volatile bool g_cvAwRecovering = false;
// Zero-output protection stand-down: alternator delivering ~0 A into a bus above target means the
// excess is battery rest / solar / shore — a field cut cannot move the bus, so the target-relative
// layers (G1/G2, CV iExcess) must not fire. Computed every tick at the top of the FAST OV section
// in AdjustFieldLearnMode (2.5 s entry dwell, single-tick exit). Hard/absolute layers stay armed.
bool altZeroOutput = false;

float g_I_cap = 0.0f;  // RPM table current ceiling this tick (A); set each AUTO tick
bool g_thermalOwnsCeiling = false;  // the thermal derate, not warmup/HiLo/battery-limit/fastOV/MaxTableValue, set uTargetAmps this tick



// ===== RPM TABLE BREAKPOINTS =====
// 10 RPM points covering the alternator's full operating range, denser at the low
// end (extra breakpoint at 300) so a sub-600 RPM idle isn't lumped in with the 0 RPM
// box; evenly balanced across the top up to the unchanged 4600 ceiling.
// Linear interpolation is used between points. Values at or below the first
// breakpoint (100 RPM) use the first entry directly — no extrapolation below it.
#define RPM_TABLE_SIZE 10
int rpmTableRPMPoints[RPM_TABLE_SIZE] = { 100, 300, 600, 1100, 1500, 2000, 2650, 3300, 3950, 4600 };
// Factory defaults for RPM breakpoints
int defaultRPMValues[RPM_TABLE_SIZE] = { 100, 300, 600, 1100, 1500, 2000, 2650, 3300, 3950, 4600 };

// There is no "target current" table. Low charge-rate mode is the capTableLo blob via
// loadCapTablesForMode(); the live per-RPM ceiling is the CAP table (rpmCapCurrentTable) below.
// The retired rpmCurrentTable's old CSV2/CSV3 slots send literal 0 as reserved placeholders
// so the payload positions are unchanged.

// ===== CAP CURRENT TABLE =====
// Hard ceiling on commanded current at each RPM, always enforced regardless of
// mode or PID output. Exists to protect the belt, shaft, and mounting hardware
// from mechanical overload at any RPM. The target table above cannot push current
// above this ceiling. Factory reset restores defaultCapCurrentValues.
float rpmCapCurrentTable[RPM_TABLE_SIZE] = { 0, 100, 100, 100, 100, 100, 100, 100, 100, 100 };
float defaultCapCurrentValues[RPM_TABLE_SIZE] = { 0, 100, 100, 100, 100, 100, 100, 100, 100, 100 };
// Low charge-rate factory default, as a fraction of the Normal cap table. Seeds capTableLo
// and is the fallback whenever that blob is missing; a user-edited Low table overrides it.
#define LOW_MODE_CAP_FRACTION 0.33f

// ===== CAP POWER TABLE =====
// Alternative cap expressed in kW instead of amps (active only when capLimitMode=1).
// The firmware converts to an equivalent amp limit using live battery voltage.
// All zeros = disabled (no power cap). capLimitMode selects which cap is active.
float rpmCapPowerTable[RPM_TABLE_SIZE] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
float defaultCapPowerValues[RPM_TABLE_SIZE] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
uint8_t capLimitMode = 0;  // 0 = use amp cap (rpmCapCurrentTable), 1 = use kW cap (rpmCapPowerTable)

// ===== MINIMUM FIELD DUTY TABLE =====
// Minimum PWM duty cycle (%) applied to the field at each RPM breakpoint. Always live:
// getMinimumFieldForRPM interpolates this table and hard-floors the result with the scalar MinDuty,
// so the flat baseline behaves exactly like MinDuty alone until a cell is hand-raised (or the
// Auto Min% learner rewrites it). A 0 cell means "no minimum field" (full shut-off allowed) — and
// because getMinimumFieldForRPM returns a HARD zero there, a zero cell drops the MinDuty scalar with
// it. That is why an unmeasured bin carries the baseline rather than 0: an all-zero table silently
// makes the user's Min Field % setting inert at every engine speed.
float rpmMinDutyTable[RPM_TABLE_SIZE] = { 0.25, 0.25, 0.25, 0.25, 0.25, 0.25, 0.25, 0.25, 0.25, 0.25 };
float defaultMinDutyValues[RPM_TABLE_SIZE] = { 0.25, 0.25, 0.25, 0.25, 0.25, 0.25, 0.25, 0.25, 0.25, 0.25 };
// Floor a bin carries when nothing has ever been measured there. Deliberately NOT voltage-class
// scaled (unlike the MinDuty scalar): it is small enough to be immaterial as field current on any
// bank, and commissioning overwrites every bin it measures within minutes of first power-up.
#define KNEE_BASELINE_PCT 0.25f

// ===== AUTO MIN% LEARNING ("knee tracker") =====
// Learns, per RPM bin, the field duty where output amps begin to build (the "knee") and parks
// rpmMinDutyTable a margin below it. One axis: RPM. Every bin starts at 0% and probes ONCE — it
// steps the floor up kneeStepPct each dwell until output rises kneeOnsetA above a freshly captured
// zero (the knee), then LOCKS (freezes) the floor at knee - margin. A locked bin re-learns only if
// real current (> kneeReArmA) later appears at its floor (knee dropped): the floor is lowered by
// one margin step. Bin 0 (lowest RPM) is permanently locked at 0%.
//   Stored floors are RAW duty referenced to kneeTempRefF. getMinimumFieldForRPM applies a
//   copper-resistance temperature correction at apply time (the duty-knee scales with field-winding
//   resistance, ~0.218 %/degF). Battery voltage is deliberately NOT used: its two effects on the
//   knee (raising the rectifier threshold vs. strengthening the field for the same duty) cancel,
//   leaving the duty-knee voltage-independent. Full spec: Working Markdown Docs/MinDuty_Knee_Learning.md.
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
String kneeLearnStateJson();

// Config Sharing (8_functions.ino) — export/import the cloneable settings set
String manifestConfigObject();
String exportConfigJson();
String exportTablesObject();
int applyImportConfig(const char *body);
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

// Temperature loop diagnostics — units: amps. Logging-only decomposition of the hybrid-form
// penalty (outerTermP + outerTermLookahead + outerTermI == the accumulated penalty); recomputed
// every tempPID_tick(). See tempPID_tick() in 6_functions.ino for the control law.
float outerTermP = 0.0f;
float outerTermI = 0.0f;
float outerTermLookahead = 0.0f;   // look-ahead share of outerTermP: Kp × max(0, projected − present temp), penalty-signed. Replaced always-zero outerTermD (Kd is hardwired 0; derivative action lives in the projected input)
float thermalSlopeFPerSec = 0.0f;  // long-window slope estimate (°F/sec); replaces outerTermDExternal

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
// One-shot: set when the thermal PID is being re-enabled after a DORMANT-but-not-stale period
// (TuningMode exit) where tempFilterUpdate() kept the slope buffer live the whole time. Tells
// the re-enable path in tempPID_tick() to PRESERVE the slope buffer instead of clearing it, so
// thermalSlopeBufFull stays true and the setpoint stays at limit-7 (no spurious limit-20 warmup
// drop that would derate the field for 60s right after a test). NOT set on a true sensor gap —
// there the trend really is stale and must be cleared. Consumed (reset) inside the re-enable path.
bool thermalPreserveSlopeOnResume = false;

float cvDSlope = 0.0f;              // V/s — sliding-window backward diff of g_cvKdFiltV (~VoltageLoopInterval window, refreshed every output tick); the CV D term's input and the recovery starve exit's
uint32_t g_cvKdCount = 0;           // session count of D-term engagement episodes; lifetime twin lives in g_ovTel.kdEventCount

float outerImpliedPenalty = 0.0f;
bool outerAntiWindupFired = false;
// Sticky version of outerAntiWindupFired: set on any CV-bleed event, cleared when CSV2 captures it.
// outerAntiWindupFired resets every tempPID_tick (~16Hz) so the 5s CSV2 frame would otherwise miss it.
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
unsigned long timeSinceLastOverheat = 0;  // Global time since any overheat


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
// Tunable parameters (expose in web UI)
float DutySlowRampRate = 0.5f;      // %/sec - Phase 3 slow ramp to 0
uint32_t ShutdownPhase2HoldMs = 500;  // ms - hold at rpmMinDuty before slow ramp (0 = skip)

// ===== TEMPERATURE LOOP PID (replaces thermal model) =====
// Tuning — all web UI configurable
float TempPIDKp = 3.0f;             // A/°F proportional gain — validated tune. Derating it makes the loop blow through setpoint and trip the warn ramp without ever saturating its cut authority (signature of under-gain, not over-authority).
float TempPIDKi = 0.1f;             // A/(°F·s) integral gain — must wind the full steady-state penalty alone (P contributes nothing at zero error).
float TempPIDKiDownFrac = 0.33f;   // asymmetric integral bleed: below setpoint (eI<0) the bleed uses TempPIDKi×this instead of TempPIDKi, so a transient sub-setpoint undershoot does NOT collapse the learned holding penalty. Ratio (not absolute) so it auto-scales with any Ki. 1.0 = symmetric; lower = slower release. Clamped [0,1].
float ThermalLookaheadSec = 60.0f;  // projection horizon (s), clamped [0,300]; longer horizons over-extrapolate the coarse slope and drive a limit cycle
float ThermalSlopeWindowSec = 25.0f;  // slope backward-difference window (s), clamped [10,60]; shorter = less lag, noisier slope — below ~25 the ±0.2/0.3°F/s sensor-quantization steps show through

float ThermalPenaltyRiseRate = 60.0f;  // A/s — how fast penalty can increase (restrict current)
float ThermalPenaltyFallRate = 20.0f;  // A/s — how fast penalty can decrease (allow more current)

float WarmupRampRate = 10.0f;     // A/s — rate at which output ceiling rises from 0 on field enable; 0 = disabled
float warmupCeiling = 0.0f;       // runtime warmup ceiling (not persisted)
float prevThermalPenalty = 0.0f;  // last applied penalty (slew "prev" + stale-hold seed). penalty = FF + thermalIntegral, clamped to the live cap — see tempPID_tick().
float thermalIntegral = 0.0f;     // HYBRID: holding-level integral (amps). The P + projection term is POSITIONAL feedforward (instant, FF = Kp·max(0,max(proj,present)−sp)); only this integral is accumulated, with the live-cap anti-windup clamp (I ≤ cap−FF), asymmetric below-setpoint bleed, and approach/descent gates. A pure velocity form silently zeroes the projection's constant lead (a difference form can't carry a feedforward level), landing the approach cut at a fraction of its strength — keep P+projection positional.
float thermalIntegralCeil = 0.0f; // instrumentation: the live integral ceiling cap−FF (amps), logged in the thermal log's iCeil_A column. When this falls below outerI, the I≤cap−FF clamp is actively DELETING earned holding integral (RPM dip / FF spike) — watch for a reheat transient on cap recovery. Diagnostic only, drives nothing.

uint32_t TempPIDIntervalMs = 5000;  // Temperature loop update period (ms) — independent of output current loop and sensor rate
float TempPIDFilterAlpha = 0.2f;    // IIR smoothing for DS18B20 (0=frozen, 1=raw); feeds slowly at 16Hz on a frozen 5s sample
// Thermistor filter alpha is hardcoded 0.02f in the tempPID_tick IIR filter — not user-configurable
// Runtime state — expose via telemetry
double tempPIDInput_d = 77.0;        // PID process variable (°F) = max(projected, present) — projected = filtered + slope × lookahead
double tempPIDSetpoint_d = 0.0;      // effectiveSetpoint: limit −7°F (slope buffer full) or −20°F blind warmup
bool tempPIDActive = false;          // true when temperature PID is in AUTO
bool tempFilterNeedsReseed = false;  // Set true to force IIR cold-start on next tempFilterUpdate()
bool thermalIntegratorReleased = false;  // false until PRESENT temp first reaches the regulation setpoint; while false the up-driving dI is held off (P + projection dP alone handle the approach — prevents approach windup overshoot)
float outerPenaltyRaw = 0.0f;        // Tier-0a: unclamped requested penalty (prevThermalPenalty + dP + dI) before clamp + slew — for the requested-vs-applied saturation diagnostic
uint8_t thermalFreezeReason = 0;     // Tier-0a: which integrator-freeze case gated this tick (see ThermalLogEntry.freezeWhy enum)
uint32_t tempInvalidSinceMs = 0;     // millis() when TempToUse first went invalid (0 = currently valid). Debounces the tempPID deactivation so a single bad sample can't force a buffer-clear warmup re-init (the spurious "140°F setpoint" artifact).

float thermalPenaltyAmps = 0.0f;    // temperature loop output: amps subtracted from target table

// The thermal loop uses no PID library object — penalty is accumulated directly in
// prevThermalPenalty (see tempPID_tick()). tempPIDInput_d / tempPIDSetpoint_d are plain CSV2
// telemetry globals. currentPID (the field/output-current loop) is a SEPARATE instance.




//=============================================================================
//                    END LEARNING MODE GLOBAL VARIABLES
//=============================================================================
// ===== THERMAL LOG (PSRAM circular buffer) =====
#define THERMAL_LOG_SIZE 3600  // covers 2 hours at 0.5 Hz (halved from 7200/1Hz to reclaim ~183 KB PSRAM for cvLog)
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

// Download-abort pause release. Safari (unlike Chrome) tears the connection down without a clean
// close after the last byte, leaving xxxLogPaused stuck until the 60s watchdog — freezing log
// appends (data gap) for the whole window. Each download handler bumps its token and registers
// request->onDisconnect to clear the pause, guarded by token equality so a lingering keep-alive
// connection from an EARLIER download closing late can't unfreeze a newer in-flight transfer.
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
  float pidSetpoint;     // setpointLimited — slew-filtered command to PID
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
  int16_t voltLoopIntervalMs;  // actual voltage loop interval when fired this tick (ms); 0 if not fired
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
  char line[1024];  // MUST hold the largest single row intact: comment block=715B, header≈480B (41 cols).
                    // snprintf truncation is NOT harmless here — it eats the row's trailing '\n', gluing
                    // that row onto the next one in the download. Grow this whenever a column or comment
                    // line is added.
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



// CV / Voltage Tuner Log
// Logs every CH1 sample (fresh CH1 arrives 5–25ms apart, assume ~30ms) — no internal rate limiter.
// Binary download via /cvlog.bin, decoded to CSV by JS.
// 60 bytes/entry × 6000 entries = 360 KB PSRAM → ~28 sec at full rate.

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
  int16_t voltLoopIntervalMs;      // actual voltage loop interval when fired this tick (ms); 0 if not fired
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

// CV P/I/D decomposition telemetry — the three contributions to Icv, streamed live (CSV1) for the
// tuning plots and logged to cvlog. g_cvKdFiltV is the dedicated CvKdVoltFiltTC-EMA of IBV the D
// term differentiates (also the "Voltage for D term" plot trace). g_cvKdTrimLive persists every
// tick (0 when idle) — unlike g_kdTrimThisTick, which cvLog_tick clears after each log write.
float g_cvKdFiltV = 0.0f;         // V — IBV smoothed by CvKdVoltFiltTC; the D term's slope input
float g_cvPTerm = 0.0f;           // A — proportional contribution to Icv (VoltageKp_active × error)
float g_cvKdTrimLive = 0.0f;      // A — D-term back-off applied at the Icv output this tick (0 when not trimming)
uint32_t g_fastOvHardCount = 0;

// ---- Gate-tuning live readouts: 10s rolling extreme per gated quantity (PSRAM) ----
// Each gate compares a live/windowed quantity against a user threshold; the dashboard shows the
// extreme that quantity reached in the last 10s so the threshold can be set relative to it. Ten
// 1-second buckets, read = extreme over buckets stamped within the last 10s — robust to update
// gaps. Float buckets are written from sensor/control tasks and read by the web task without a
// lock; a torn read only blurs a tuning display, never control. Direction per gate: peak for
// trip-type gates (slew/drift/tone), trough for the RPM edge-margin pass-gate.
enum { ROLL_RPMEDGE = 0, ROLL_AMPSDRIFT, ROLL_AMPSDRIFTEXC, ROLL_TONEPK, ROLL_LDSLEW, ROLL_CVSLOPE,
       // Ripple-capture (faFiltRippleUpdate) admission gates (§11 stationarity). All four entries are
       // (window quantity − its effective limit), peak over 10s, <=0 = passing: command travel vs the
       // amplitude limit, alt/batt half-window mean-shift vs the stationarity limit, and RPM half-window
       // mean-shift vs its stationarity limit.
       ROLL_RIPCMDEXC, ROLL_RIPALTEXC, ROLL_RIPBATTEXC, ROLL_RIPRPMSHIFT, ROLL_COUNT };
#define ROLL_EMPTY (-2000000000)   // CSV sentinel: no sample in the 10s window. Also SafeInt's NaN/Inf return, so this is now the one "not available" value on every CSV stream
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

// Which protection layer actually set the final fastOvCurrentCap this tick (the BINDING
// constraint — lowest cap wins). Lets the CV log distinguish "KHard fired" from "KHard
// fired but iExcess/loadDump was the real limit". 0 = cap left at base (unclamped).
enum FastOvCapReason : uint8_t {
  CAP_REASON_NONE     = 0,  // cap at fastOvBaseCap — no protection bound
  CAP_REASON_KHARD_G1 = 1,  // Group 1 predictive KHard (Vpred)
  CAP_REASON_KHARD_G2 = 2,  // Group 2 measured KHard, rising-gated shed (filtered IBV)
  CAP_REASON_IEXCESS  = 3,  // iExcess supervisor (near-target / CV regime)
  CAP_REASON_LOADDUMP = 4,  // load-dump cutoff
  CAP_REASON_IEXCESS_BULK = 5,  // iExcess BULK sub-mode (current-control phase, vs ceiling)
};
uint8_t g_fastOvCapReason = CAP_REASON_NONE;  // exported once per full control tick alongside g_fastOvCurrentCap (see capReasonTick in AdjustFieldLearnMode)

// Event marker bitmask for the Plots-tab designators. The control loop ORs in a bit whenever one of
// these momentary, single-tick events fires; the CSV1 sender reads it at ~10 Hz and clears the bits
// it consumed, so events that fire between sends are still captured. Visual indicator only — exact
// reason lives elsewhere.
#define PROT_EVT_OV 0x01  // overvoltage hard clamp (KHard G1/G2)
#define PROT_EVT_IX 0x02  // iExcess current-excess supervisor (CV or Bulk)
#define PROT_EVT_LD 0x04  // load-dump cutoff
// 0x08 is NOT a protection and no current cap bound — the CV D term merely trimmed the current setpoint. The
// dashboard masks 0x01|0x02|0x04 for its red "a protection fired" markers and shades 0x08 separately;
// anything reading this mask must do the same or a D-term tick reads as a protection trip.
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
// Per-CSV1-frame aggregates for the live sparkline. The control loop runs far faster than the
// ~10 Hz CSV1 stream, so the detector accumulates the worst case each tick and the CSV1 sender
// resets these after each send. Peak excess + min threshold = the closest-to-firing the frame got.
float g_mExcessEmaPeak = 0.0f;      // peak of g_mExcessEma over the frame (A)
float g_iExcessThreshWinMin = 0.0f; // min of E over armed ticks this frame (A)
bool  g_iExcessArmedWin = false;    // detector armed at any tick this frame (peak/min validity)

// Load dump detection via dBcur/dt — three-tier cascade
float LoadDumpDtThresh1 = 7000.0f;  // A/s — tier 1: fires on a SINGLE sample above this (hard-switched FET disconnects)
float LoadDumpDtThresh  = 5000.0f;  // A/s — tier 2: fires when TWO consecutive samples both exceed this; noise ceiling ~354 A/s consecutive
float LoadDumpDtThresh3 = 5000.0f;  // A/s — tier 3: fires when THREE consecutive samples all exceed this (slow relay-contact disconnects)
// Per-tier consecutive-sample counts (user settings). Time to act = N x INA fast-mode cadence (~5 ms).
// At default thresholds tier 3 is redundant (same threshold as tier 2, more samples — tier 2 always
// fires first); it becomes meaningful only with a lower tier-3 threshold. Deliberate — do not "fix".
int LoadDumpN1 = 1;
int LoadDumpN2 = 2;
int LoadDumpN3 = 3;
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

// IGNITION-CYCLE WATERMARKS
// Min/max since boot (= since ignition cycled). SEPARATE from the
// *_AllTime / *Max / *_min systems above — those persist across boots
// via NVS; these reset every boot. Initialized to NAN so the first valid
// sample seeds both ends (see wmIgnUpdate).
// 18 pairs × 8 bytes = 144 bytes, internal RAM, plain globals.
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
  IDX_DYNAMIC_SHUNT_GAIN,
  IDX_IMU,                       // 34 - IMU (accel/gyro/derived angles)
  IDX_WATER_DEPTH,               // 35 - NMEA2k WaterDepth (PGN 128267)
  IDX_STW_NMEA,                  // 36 - Speed Through Water / SOW (PGN 128259) for the boat-performance polar
  IDX_VICTRON_SOLAR,             // 37 - Victron VE.Direct solar (PPV/VPV) staleness
  IDX_N2K_BATT,                  // 38 - NMEA2k received Battery Status (PGN 127508) V/A/T
  IDX_N2K_SOC,                   // 39 - NMEA2k received DC Detailed Status (PGN 127506) SOC/SOH
  IDX_DVCC,                      // 40 - DVCC follow: received charge-limit (CVL/CCL) stream
  IDX_N183,                      // 41 - NMEA 0183 serial receive (any checksum-valid sentence)
  IDX_N183_HDG,                  // 42 - NMEA 0183 heading only: IDX_N183 stays fresh on a GPS-only talker
  IDX_BATT_TEMP_PROBE,           // 43 - BATT-role DS18B20 (BatteryTempProbeF)
  IDX_EXTRA_TEMP,                // 44 - EXTRA-role DS18B20 (ExtraTempF)
  IDX_VE_BATT_TEMP,              // 45 - VE.Direct "T" battery temperature (VictronBattTempF)
  IDX_RVC_BATT_TEMP,             // 46 - RV-C DC_SOURCE_STATUS_2 battery temperature (rvcRxBattTempF)
  // Keep this last and increment when new added
  MAX_DATA_INDICES = 47
};

unsigned long dataTimestamps[MAX_DATA_INDICES];  // Uses the enum size automatically

const unsigned long DATA_TIMEOUT = 10000;  // 10 seconds default timeout
// Universal macros for clean syntax
#define MARK_FRESH(index) dataTimestamps[index] = millis()
#define IS_STALE(index) (millis() - dataTimestamps[index] > DATA_TIMEOUT)
// A source that has never stamped still holds 0, which IS_STALE reads as fresh for the first DATA_TIMEOUT
// ms after boot. Window accumulators gate on this too, or the first window of every boot takes the
// variable's 0.0 initialiser as measured data.
#define IS_SEEN(index) (dataTimestamps[index] != 0)
#define SET_IF_STALE(index, variable, staleValue) \
  if (IS_STALE(index)) { variable = staleValue; }

// ISRG Root X1 Certificate (Let's Encrypt Root CA)
// Valid until: June 4, 2035 (expires in 2035, stable for 10+ years)
// This root certificate doesn't change like intermediate certificates (R10->R12 rotation)
// Using root instead of intermediate prevents certificate validation failures during Let's Encrypt updates
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
OwProbe  owProbes[OW_MAX_PROBES];
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
VeDirectFrameHandler myve;
// WIFI STUFF
// WiFi wake button functionality (GPIO5)
int WiFiWakeButton = 0;                           // Current button state (1=pressed)
unsigned long wifiWakeButtonPressTime = 0;        // Timestamp when button pressed
const unsigned long WIFI_WAKE_DURATION = 300000;  // 5 minutes
bool wifiWakeActive = false;                      // Tracking wake mode state
unsigned long wifiWakeStart = 0;                  // millis() when wake was triggered; 0 = inactive. Never store millis()+constant — use elapsed-time comparisons to survive 49.7-day wraparound.

// WiFi Napping (Client-mode engine-off standby). When enabled, instead of powering the radio
// fully off at 80MHz we keep WiFi associated in modem-sleep so the dashboard stays reachable
// with no button press. Falls back to full radio-off after 12h with no dashboard connected;
// from there only the wake button or ignition brings it back. Inert in AP mode (a SoftAP can't sleep).
bool wifiNapEnabled = true;           // System Setting (Client only); default on — app can wake from engine-off standby without the GPIO5 button
bool wifiNapActive = false;           // true while modem-sleep nap is active

AsyncWebServer server(80);                  // Create AsyncWebServer object on port 80
AsyncEventSource events("/events");
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
    // Self-expiring window: SendWifiData only resets these while an SSE client is connected —
    // unbounded accumulation would wrap count (~5.5 min at 5 ms) and make the first
    // post-reconnect point a gap-wide mean. 5× the send interval tolerates deferred sends.
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
// IBV/Bcur ~5ms (INA228), aggAltCur = MeasuredAmps ~5-25ms (ADS1115 CH1). aggBattV/aggRpm/aggCh3
// (ADS1115 CH0/CH2/CH3, ~30ms) are mean-only for cleaner plots — the Max toggle never touches them.
WinAgg aggIbv, aggBcur, aggAltCur, aggBattV, aggRpm, aggCh3;
bool battMaxMode = false;  // false = window mean (default), true = max-magnitude on the V/I traces it covers
int plotTimeWindow = 30;                    // Plot time window in seconds

// WiFi provisioning settings persist in NVS as NK_ssid / NK_pass
// Cached WiFi client credentials (loaded once, reused for reconnects)
char cached_wifi_ssid[33] = "";  // 32 + null
char cached_wifi_pass[65] = "";  // 64 + null
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
int n2kTxEnable = 0;        // master transmit switch; 0 = listen-only exactly as before (no bus presence). Mode is set at boot — changing this takes effect after reboot
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

// ===== RV-C transmit (producer) settings — spec: Working Markdown Docs/RVC_TRANSMIT_SPEC.md =====
// RV-C DGNs are J1939 PGNs and ride the same 250 kbps port and the same claimed address as our
// N2K node; they go out through the same round-robin so the one-message-per-pass budget holds.
int rvcTxEnable = 0;        // RV-C transmit master. Independent of n2kTxEnable for the toggle, but
                            // the node mode it needs is the same one n2kTxEnable sets at boot.
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
const unsigned long N2kTransmitMessages[] PROGMEM = { 127488UL, 127489UL, 127506UL, 127507UL, 127508UL, 127510UL, 127513UL, 130312UL, 0 };
// Mirrors NMEA2000Handlers[] so the library's 126464 RX PGN list is truthful (spec §8).
// RV-C DGNs are deliberately absent: they are not N2K PGNs — they arrive via the fork's raw-RX tap.
const unsigned long N2kReceiveMessages[] PROGMEM = { 126720UL, 126992UL, 127245UL, 127250UL, 127257UL, 127506UL, 127508UL, 127513UL, 128259UL, 128267UL, 129026UL, 129029UL, 129540UL, 130306UL, 0 };
void nmea2kTransmitTick();
// ===== NMEA2000 received battery data (127508 + 127506 at n2kRxBattInstance) =====
// Display/telemetry only — never an input to field control, protections, targets, or SoC.
int n2kRxBattInstance = 0;   // battery instance to read (a shunt/BMS "virtual shunt" bridged onto N2K)
float n2kRxBattV = NAN;      // received battery voltage (127508)
float n2kRxBattA = NAN;      // received battery current (127508, signed)
float n2kRxBattTempF = NAN;  // received battery temperature (127508)
int n2kRxSoc = -1;           // received state of charge % (127506); -1 = not available
int n2kRxSoh = -1;           // received state of health % (127506); -1 = not available

// ===== DVCC-style charge-limit follow (CVL/CCL) — spec: Working Markdown Docs/DVCC_FOLLOW_SPEC.md =====
// A received charge limit is a clamp-only REQUEST: it can lower the effective voltage target or
// current ceiling below local values, never raise anything. Every local protection outranks it.
// Sources on the 250k bus: RV-C DC_SOURCE_STATUS_4 / BATTERY_STATUS_4 (single-frame, decoded in the
// fork's raw-RX tap) and Victron VE.Can VREG 0x351 (0xEF00 proprietary single-frame + 126720
// fast-packet). The Victron register layout is PROVISIONAL until validated against a real GX
// capture (spec §8a) — the trust machine and observe-before-enable workflow gate it meanwhile.
int dvccEn = 0;             // master follow switch; decoded limits clamp targets only when 1
int dvccSrcType = 0;        // authority dialect: 0 = Victron VE.Can (VREG), 1 = RV-C
int dvccInst = 1;           // RV-C DC instance filter (RV-C is 1-oriented; 0 = accept any). Victron path locks to the first sending bus address instead
int dvccSilenceS = 30;      // no valid limit frame for this long → drop the clamp, revert to local targets
int dvccSettleS = 10;       // in-range updates must span this long (and ≥2 frames) before the first clamp applies
float dvccCvlMin = 9.0f;    // plausible-CVL window, absolute volts (class-seeded/rescaled like other voltage settings);
float dvccCvlMax = 16.0f;   //   a CVL outside the window latches UNTRUSTED until manual reset
// Live state — written by the decode tap + dvccTick() (both ParseMessages/loop context, core 1)
float dvccRxCvl = NAN;      // last decoded charge-voltage limit (displayed even when follow is off)
float dvccRxCcl = NAN;      // last decoded charge-current limit (NAN = dialect sent not-available)
uint32_t dvccRxLastMs = 0;  // millis() of last decoded limit frame
uint32_t dvccRxCount = 0;   // decoded limit frames since boot (dvccTick uses it as a new-frame edge)
uint8_t dvccRxSrcAddr = 255;    // sender bus address (255 = none yet)
uint8_t dvccRxPriority = 0;     // RV-C device priority of the sender (BMS = 120)
uint8_t dvccRxChgState = 0;     // RV-C desired charge state — value 1 (do not charge) forces effective CCL 0; other values displayed only
uint8_t dvccState = 0;          // 0 off, 1 waiting, 2 settling, 3 following, 4 stale, 5 untrusted
uint8_t dvccUntrustReason = 0;  // 0 none, 1 CVL out of window, 2 CCL implausible, 3 flapping
float dvccCvlV = NAN;       // ACTIVE clamps consumed by the control path — non-NAN only while FOLLOWING
float dvccCclA = NAN;
bool dvccResetReq = false;    // momentary UNTRUSTED-latch reset request from the web UI; consumed by dvccTick()
bool dvccCfgChanged = false;  // source/instance edited: dvccTick clears decode/settle state (an UNTRUSTED latch is preserved)
// Raw-frame capture ring (spec §8a) — PSRAM, filled from the raw-RX tap (5_functions), served as
// hex text by /dvccCapture (3_functions). Lets a customer boat validate the Victron/RV-C decode
// with zero special tooling. Storage lives here so both files see it (concatenation order).
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
// A followed limit is uninterpretable without the identity of the node that published it: a GX
// rewrites a managed battery's numbers per battery brand, so "40 A at 55 V from addr 35" is an
// anecdote until we know what addr 35 is. Filled PASSIVELY from PGN 60928 (NAME) and 126996
// (product information) - with transmit off we can never REQUEST either, so this collects only
// what the bus volunteers: node power-up claims, plus the broadcast answers to any MFD's own
// device scan. Hearing nothing is a normal outcome on a bus that booted before us, not a fault.
// Roster reads/writes are guarded by dvccCapMux (shared with the capture ring - never nested).
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
void dvccRawFrameTap(unsigned long id, unsigned char len, const unsigned char *buf);

//ADS1115 more pre-setup crap
uint32_t adsI2CErrorCount = 0;
uint32_t adsSlowReadCount = 0;      // times the convert-register read took >5ms (I2C stall events)
// I2C bus-health instrumentation — separates a true bus stall from loop preemption.
// inaBusReadWorstUs / imuFifoFetchWorstUs time ONLY the Wire transactions; compare them
// to the whole-block ft_rai_ina228 / IMU-drain timers: equal => the bus, much smaller =>
// the loop was preempted mid-read by Core-1 load. All four reset with "Reset Peak Values".
uint32_t inaBusReadWorstUs = 0;    // worst µs spent in the two INA228 Wire reads since reset
uint32_t inaBusSlowCount = 0;      // INA228 bus reads > 15 ms (one Wire-timeout's worth) since reset
uint32_t ina228ErrorCount = 0;     // INA228 reads dropped (sanity fail / exception)
uint32_t imuFifoFetchWorstUs = 0;  // worst µs spent in Get_FIFO_Sample since reset
uint16_t imuFifoWorstSamples = 0;  // sample count of THAT worst fetch — 42 bytes is ~1ms at 400kHz, so a
                                   // worst at the 6-sample cap proves the stall is bus/preemption, not transfer size
uint32_t adsLastSlowEndTxUs = 0;    // endTransmission duration on most recent slow read (µs)
uint32_t adsLastSlowReqFromUs = 0;  // requestFrom duration on most recent slow read (µs)
// AdjustFieldLearnMode worst-pass section profiler — post-AsyncTCP-pin residual stalls
// (~16ms) keep landing in this one function; this latches a per-section breakdown of the
// worst FULL pass (early-return passes are not recorded — if the Adjust Field row in
// Function Timing spikes but aflWorstTotalUs doesn't, the spike was in an early-return
// pass, i.e. the pre-gate region). Read via /debug; resets with "Reset Peak Values".
#define AFL_SECTIONS 7
uint32_t aflWorstTotalUs = 0;
uint32_t aflWorstSecUs[AFL_SECTIONS] = { 0 };

enum ADS1115_State {
  ADS_IDLE,
  ADS_WAIT,
  ADS_READ_RESULT
};

ADS1115_State adsState = ADS_IDLE;
uint8_t adsCurrentChannel = 0;  // Driven by adsSeq[] = {1,0,1,2,1,3}; CH1 fires 3× per cycle (fresh CH1 measured 5–25ms apart, assume ~30ms)
int adsTriggeredChannel = 0;
unsigned long adsStateEntered = 0;
const unsigned long ADS_CONVERSION_MS = 3;  // 1.16ms at 860SPS + millis() granularity margin
const unsigned long ADS_TIMEOUT_MS = 10;    // hardware fault catcher
const uint32_t ADS_SLOT_US = 4500;  // absolute per-slot grid; CH1 = 2 slots ≈ 9ms. Must exceed conversion time (1.16ms@860SPS).

volatile bool ch1FreshFlag = false;  // Set when CH1 result is ready, consumed by AdjustFieldLearnMode()

uint8_t PidSampleDivisor = 1;  // 1=PID runs every CH1 sample (measured 5–25ms apart, assume ~30ms), 2=every other, etc.
                               // CH1 fires at positions 0, 2, 4 in adsSeq[] {1,0,1,2,1,3};
                               // ADS_SLOT_US grid puts CH1 at ~9ms nominal, but loop timing stretches it — measured 5–25ms

const uint16_t adsMuxCodes[4] = {
  ADS1115_REG_CONFIG_MUX_SINGLE_0,
  ADS1115_REG_CONFIG_MUX_SINGLE_1,
  ADS1115_REG_CONFIG_MUX_SINGLE_2,
  ADS1115_REG_CONFIG_MUX_SINGLE_3
};

// Inter-sample gap meters for the two slow ADS channels (CH0 battV cross-check, CH2 RPM)
// — diagnostic for the round-robin sampler's CH0/CH2 cadence. "last" = most recent gap
// between VALID readings; "worst" = max since the last Reset Peak Values press. Updated
// by adsGapUpdate() in the ADS read path. Dashboard: Live Data → ESP32. CSV2 (Pattern A2).
uint16_t ch0GapLastMs = 0, ch0GapWorstMs = 0;   // CH0 battery-voltage read gap (ms)
uint16_t ch2GapLastMs = 0, ch2GapWorstMs = 0;   // CH2 RPM read gap (ms)
uint16_t ch0GapFieldOnWorstMs = 0, ch2GapFieldOnWorstMs = 0;  // same meters, field gate open only — excludes field-off background passes
uint32_t ch0GapPrevMs = 0, ch2GapPrevMs = 0;    // timestamp of last valid read (internal)

// One-heavy-per-pass scheduler gate. Reset false at the top of loop(); the staggerable
// periodics (alt-health fold, boat-perf fold, system-health heap walk, WiFi send) each
// claim it before doing work and defer one loop if another already claimed it this pass —
// so at most ONE multi-ms periodic runs per loop pass, capping worst-case loop time at
// floor + one heavy item instead of their sum. Control loop / RAI / IMU drain are NOT
// gated (they must run every pass). Set true by whichever periodic claims the pass.
bool gHeavyRanThisPass = false;

// CSV2 (heavy ~548-field diagnostic blob, sent every 5s) send-cost breakdown: build (the
// snprintf) vs send (events.send → AsyncTCP). Microseconds. "last" = previous CSV2 cycle,
// "worst" = max since Reset Peak Values. Diagnostic to decide whether chunking the BUILD or
// the SEND shrinks the ~5.8ms WiFi-send spike. Dashboard: Live Data → ESP32.
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
  "<span class=\"opt-desc\">Leave the two fields above blank and check this box to erase the saved ship's WiFi credentials. Until new credentials are saved, the regulator will boot back to this setup page with charging disabled - use the Hotspot Wire (below) for normal operation without ship's WiFi. Leaving the fields blank WITHOUT checking the box keeps the saved credentials unchanged.</span>"
  "</div>"

  // Non-preferred option: hotspot/AP credentials live inside the de-emphasized grey box
  "<div class=\"option option-secondary\">"
  "<span class=\"opt-badge badge-secondary\">Non-Preferred</span>"
  "<span class=\"opt-title\">Use the regulator as a Hotspot (Access Point)</span>"
  "<span class=\"opt-desc\">As backup, or for ships without existing WiFi networks, you may use the regualtor as a Hotspot (aka Access Point). The regulator controller will broadcast its own WiFi network which you can connect to from any device (phone, ipad, laptop, etc.).  Mostly the same functionality will exist at alternator.local, but with no internet, you won't be able to use weather mode, get software updates, see Community features, etc.  This mode is less supported.  To enter this mode on a reboot, you must connect pin 12 in RJ3 (the rightmost ethernet connector, Blue wire) to Ground.  Leave it connected to GND forever if you prefer this mode.</span>"
  "<label>New Alt. Reg. Hotspot Name (SSID):</label>"
  "<input type=\"text\" name=\"hotspot_ssid\" placeholder=\"Leave blank for default: ALTERNATOR_WIFI\">"
  "<label>New Alt. Reg. Hotspot Password:</label>"
  "<input type=\"password\" name=\"ap_password\" placeholder=\"Leave blank for default: alternator123\">"
  "<div class=\"info-box\">"
  "***To boot into Hotspot mode, the Hotspot Wire (pin 12 in RJ3, the rightmost ethernet connector, Blue wire) must be connected to Ground during a restart.***"
  "</div>"
  "</div>"

  "<div class=\"info-box\">"
  "To return to this Wifi Configuration page at any time, connect the WifiReset wire (pin 11, RJ3, Green/White) to Ground during a restart."
  "</div>"

  "<button type=\"submit\" id=\"sv\">Save Configuration</button>"
  "<div class=\"info-box status\" id=\"st\"></div>"

  "<div class=\"info-box\">"
  "After saving, this page may become unresponsive or disappear. In any case, wait 20 seconds, then reconnect to your chosen network to access the full alternator interface at this same url (alternator.local).  Or, just use the iOS app."
  "</div>"
  "</form>"
  "</div>"

  // Post-save countdown. This local status panel is the only feedback the user gets — the POST answer
  // usually never lands, since the device restarts ~4s later and this page loses the AP with it. The
  // Save button is hidden and replaced by the panel so the finished message can't read as a button
  // waiting to be pressed.
  "<script>"
  "(function(){var f=document.forms[0],b=document.getElementById('sv'),s=document.getElementById('st');"
  "f.addEventListener('submit',function(e){e.preventDefault();b.disabled=true;b.style.display='none';"
  "s.style.display='block';"
  "var q=new URLSearchParams(new FormData(f));"
  "fetch('/wifi',{method:'POST',body:q}).catch(function(){});"
  "var n=25;(function t(){s.textContent=n>0?('Settings saved. Restarting - '+n+'s'):'Restart complete. Reconnect your device to your chosen network, then open alternator.local in a browser.';"
  "if(n-->0)setTimeout(t,1000)})()});})();"
  "</script>"

  "</body></html>";

// mbedTLS allocator routed to PSRAM. This core is built with CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC, which
// forces the two ~16KB TLS record buffers into INTERNAL RAM. Over runtime the internal heap fragments
// (e.g. 65KB free but largest block 24KB), so a handshake can't carve two 16KB contiguous blocks and
// fails with MBEDTLS_ERR_SSL_ALLOC_FAILED (-0x7F00) — which surfaces as a bogus "connection failed".
// PSRAM has ~1.8MB and doesn't fragment that way. Registered at boot via mbedtls_platform_set_calloc_free
// (MBEDTLS_PLATFORM_MEMORY is on with a runtime pointer, not a compile-time macro), so this is the
// runtime equivalent of CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC — no custom core build needed. Internal
// fallback covers the (near-impossible) case PSRAM can't satisfy a request. free() works on either heap.
static void *mbedtlsPsramCalloc(size_t n, size_t size) {
  void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  return p;
}

void setup() {

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
    Serial.println("mbedTLS allocator -> PSRAM (TLS no longer needs contiguous internal RAM)");
  }

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
  consoleQueue = (ConsoleMessage *)ps_malloc(CONSOLE_QUEUE_SIZE * sizeof(ConsoleMessage));
  if (!consoleQueue) Serial.println("FATAL: consoleQueue ps_malloc failed");
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
  if (consoleQueue) memset(consoleQueue, 0, CONSOLE_QUEUE_SIZE * sizeof(ConsoleMessage));
  // Battery Health PSRAM buffers (sample buffer interval-gated at 15ms: 8192 covers ~120s,
  // i.e. the default 56s test plus headroom for a raised edge count or stretched dwell).
  bhSampleCap = 8192;
  bhSamples = (BattHealthSample *)ps_malloc(bhSampleCap * sizeof(BattHealthSample));
  if (!bhSamples) Serial.println("FATAL: bhSamples ps_malloc failed");
  // CV plant-fit sample buffer (40ms gated: 4096 covers ~164s ≥ the worst-case 300→180mV OV re-step at the
  // slowest ω_c — two 45s settled-rests + two horizon-scaled step-holds + release — so the measured edge is
  // never lost to a full buffer and the buffer isn't the limiter before the watchdog (cvpfServiceCompletion)).
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
  // CH1 interval ring — 30 KB to PSRAM
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
  loadFuelTableFromNVS();

  if (!ensureLittleFS()) {
    Serial.println("CRITICAL: Cannot continue without filesystem");
    Serial.println();
    while (true)
      ;  // halt
  } else {
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
  {  // Web-asset partition headroom — nothing in the product measured it before, so an OTA bundle that
     // no longer fits was only visible from a local build. checkWebFilesExist() already mounted webFS,
     // so this reads the live mount: no second mount, no new lfs buffers. 0 back means unmounted.
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
  // Black box beats the NVS copy for last-session stats: NVS full-saves fire only at the
  // field-off edge / shutdown, so a session that ends in a crash or reset leaves SessionDur/
  // MaxLoop/MinHeap holding an older session's values. The snapshot is exact at death.
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
  pinMode(4, OUTPUT);     // This pin is used to provide a high signal to Field Enable pin
  digitalWrite(4, LOW);   // Start with field off
  gpio4IsLow = true;      // keep the shadow in sync — field starts off
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
  // The Arduino core defaults LEDC to the 40MHz XTAL clock on S3, which caps 12-bit PWM at 9727Hz —
  // 19kHz attach fails outright (bench 2026-08-16). Force the 80MHz APB clock (ceiling 19455Hz).
  // Safe here: field PWM is the only LEDC user, and the WiFi-nap ladder only swings CPU 240<->80MHz,
  // where APB stays 80MHz. Must run before the first ledcAttach (first setDutyPercent call).
  ledcSetClockSource(LEDC_USE_APB_CLK);
  // PWM setup (needed for basic operation)
  //ledcAttach(pwmPin, SwitchingFrequency, pwmResolution);

  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  // If you have custom headers, also:
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "*");

  InitSystemSettings();       // load all settings from NVS. If no keys exist, create them.
  altApplyClassScales();      // alt-health Vbus cell size needs SYSTEM_VOLTAGE_CLASS — initAlternatorHealth ran before settings
  altSeedClassKnobs();        // deferred first-creation seeding of the class-dependent registry knobs (same reason)
  bhInitSettings();           // Battery Health: DCIR test config + persisted DCIR/capacity blobs
  initWeatherModeSettings();  // Add weather mode settings--- otherwise similar to line above (InitSystemSettings)
  runSettingsMigrations();    // settings-schema migration chain — must stay after the last NVS settings loader
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
  httpsQueue = xQueueCreate(2, sizeof(HttpsRequest));  // tiny messages now (payload is a PSRAM pointer, not an inline 6 KB buffer)
  Serial.println(httpsQueue ? "HTTPS queue created" : "ERROR: Queue creation failed");

  // Both stay on Core 0, but priority > 0
  xTaskCreatePinnedToCore(TempTask, "TempTask", 6144, NULL, 1, &tempTaskHandle, 0);  // 6144: multi-drop registry pass (BATTERY_TEMP_SENSORS_SPEC.md §3); ensureCore0Tasks() in 4_functions re-creates with these params and must match
  Serial.println("Temp task created on Core 0");

  // With mbedTLS record buffers in PSRAM, only call frames sit on this stack
  // (~4KB observed peak via /debug stackHWM); 12KB keeps a ~3x margin.
  xTaskCreatePinnedToCore(httpsTask, "HTTPS", 12288, NULL, 1, &httpsTaskHandle, 0);
  Serial.println("HTTPS task created on Core 0");

  // Rectifier fault detector compute — Core 0, priority 1 (same as TempTask/HTTPS, below the
  // network tasks). Moves the analysis off the Core-1 control loop, which used to freeze in
  // lockstep once a minute. faResultSem returns the verdict; the task is woken by notification.
  // Stack in PSRAM (MALLOC_CAP_SPIRAM): its 8KB was half the internal-RAM regression that pushed
  // the TLS handshake floor (~34KB contiguous) out of reach. Safe on Core 0 — the IDF suspends the
  // other core during flash-cache-disable windows. Its working data (faMatrix/faRawRing) is already
  // PSRAM, so the added stack-access latency is modest; watch faDetWorstComputeUs on the dashboard.
  faResultSem = xSemaphoreCreateBinary();
  xTaskCreatePinnedToCoreWithCaps(faDetTask, "faDet", 8192, NULL, 1, &faDetTaskHandle, 0, MALLOC_CAP_SPIRAM);
  Serial.println("Fault-detector task created on Core 0 (PSRAM stack)");

  // CPU-load sampler — Core 0, priority 0 (BELOW every network/upload task). uxTaskGetSystemState
  // is a ~4ms scan that used to run on the Core-1 control loop; here it runs every 2s only when
  // Core 0 is otherwise idle, so it yields to WiFi/uploads and can't worsen Core-0 spikes. It
  // sleeps 2s between runs, so it never starves the Core-0 idle task / watchdog.
  // 4KB stack in PSRAM for the same internal-RAM/TLS-floor reason as faDetTask above.
  xTaskCreatePinnedToCoreWithCaps(cpuLoadTask, "cpuLoad", 4096, NULL, 0, &cpuLoadTaskHandle, 0, MALLOC_CAP_SPIRAM);
  Serial.println("CPU-load task created on Core 0 (prio 0, PSRAM stack)");

  if (currentMode == MODE_CLIENT && WiFi.status() == WL_CONNECTED) {
    Serial.println("Starting NTP sync...");
    syncTimeFromNTP();
    Serial.printf("Time synced. Timestamp: %lu\n", getCurrentTimestamp());
  }


  esp_log_level_set("esp32-hal-i2c-ng", ESP_LOG_WARN);
  queueConsoleMessage("System starting up...");

  if (hardwarePresent == 1) {
    initializeHardware();  // Initialize hardware systems
  };
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
  // Post-allocation PSRAM baseline: largest contiguous block matters as much as total —
  // cvLog's 333 KB is one heap_caps_malloc, so total-free can look fine while it can't fit.
  Serial.printf("PSRAM after setup: %u KB free / %u KB total, largest block %u KB\n",
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024),
                (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
  Serial.println("=== SETUP COMPLETE ===");
}

void loop() {

  esp_task_wdt_reset();

  // Black box heartbeat: magic written last so a reset mid-update can't leave a stale-but-valid snapshot
  g_blackBox.upMillis = millis();
  g_blackBox.ibv = IBV;
  g_blackBox.duty = dutyCycle;
  g_blackBox.rpm = RPM;
  g_blackBox.measAmps = MeasuredAmps;
  g_blackBox.altTempF = (int16_t)(isnan(AlternatorTemperatureF) ? -999 : AlternatorTemperatureF);
  g_blackBox.maxLoopUs = MaxLoopTime;
  g_blackBox.minHeapKB = MinFreeHeap;
  g_blackBox.sysMode = (uint8_t)sysMode;
  g_blackBox.chargeStage = chargeStageDisplay;  // global kept fresh by the control loop — no function call here
  g_blackBox.magic = BLACKBOX_MAGIC;

  gHeavyRanThisPass = false;  // reset the one-heavy-per-pass scheduler gate for this loop pass

  checkOtaWedge();     // retire a stuck otaInProgress before anything below honors it
  ensureCore0Tasks();  // bring TempTask / httpsTask back after an OTA attempt; retry + alarm if they can't
  improvSerialPoll();  // USB installer handshake (4_functions.ino); returns at once when nothing is buffered

  // Deferred reboot from /get?RebootRegulator — wait 2s so the HTTP 200 reaches the caller
  if (rebootRequested && millis() - rebootRequestedAt > 2000) {
    Serial.println("=== REMOTE REBOOT (/get?RebootRegulator) ===");
    ESP.restart();
  }

  // Deferred commissioning-start persist (2_functions.ino): one staged NVS commit per pass — keeps
  // the Start's restore-point burst off the network task (in-handler it froze the SSE stream ~2 s).
  cxStartPersistService();
  serviceSettingsArmHold();  // 5_functions.ino: an open interface holds the settings arm window open
  Ignition = !digitalRead(1);  // ! is for optocoupler (LOW = ignition ON)
  if (IgnitionOverride == 1) {
    Ignition = 1;  // force ON (bench / no ignition wire) — override can ONLY force on, never off
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

  // Read the staged-update flag ONCE (NVS open per loop pass would be too costly), but only
  // consume it once WiFi is up AND the field is de-energized: an intermittent hotspot absent at
  // this boot must not eat the update — the flag stays armed and the install fires when the
  // network next appears and charging is off.
  if (!manualUpdateCheckDone && millis() > 5000 && currentMode == MODE_CLIENT) {
    pendingUpdateWaiting = checkForPendingUpdateNonBlocking(pendingVersion);
    if (pendingUpdateWaiting) Serial.printf("UPDATE: Found pending update for %s (waiting for WiFi if not connected)\n", pendingVersion);
    manualUpdateCheckDone = true;
  }

  if (pendingUpdateWaiting && WiFi.status() == WL_CONNECTED) {
    if (fieldActiveStatus > 0) {
      // performOTAUpdateToVersion blocks THIS task — the control loop — for the whole
      // download+flash, freezing the LEDC field PWM at its last commanded duty with every
      // software protection dead. Never start it with the field energized; nothing can raise
      // the duty once the loop is seized, so a present-state check is sufficient.
      static bool stagedOtaDeferLogged = false;
      if (!stagedOtaDeferLogged) {
        queueConsoleMessage("Staged update waiting: install starts when charging stops (field off)");
        stagedOtaDeferLogged = true;
      }
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

  // === OTA UPDATE: simple one-shot FORCED UPDATE check after 3s ===
  static bool otaCheckDone = false;

  if (!otaCheckDone && millis() > 3000) {  // 3 seconds after boot
    if (currentMode == MODE_CLIENT && WiFi.status() == WL_CONNECTED && isRegistered && currentPartitionType != 0) {
      // Only proceed if no uploads in progress
      if (!core0Busy) {
        Serial.println("OTA: Running one-time firmware/version checks after 3s...");

        // Both sends must succeed before we mark otaCheckDone — otherwise a queue-full
        // at boot would skip both retries permanently. httpsQueue is only depth-2.
        bool bothSent = true;

        debugStackBeforeHTTPS("updateFirmwareVersionInSupabase");
        if (WiFi.RSSI() >= -80) {
          HttpsRequest req = { .type = HTTPS_UPDATE_FW_VERSION };
          if (xQueueSend(httpsQueue, &req, 0) != pdTRUE) {
            Serial.println("OTA: HTTPS queue full — will retry FW version push next loop");
            bothSent = false;
          }
        }

        debugStackBeforeHTTPS("checkForForcedUpdate");
        if (WiFi.RSSI() >= -80) {
          HttpsRequest req = { .type = HTTPS_CHECK_FORCED_UPDATE };
          if (xQueueSend(httpsQueue, &req, 0) != pdTRUE) {
            Serial.println("OTA: HTTPS queue full — will retry forced-update check next loop");
            bothSent = false;
          }
        }

        if (bothSent) otaCheckDone = true;
      }
      // If blocked, will retry next loop iteration
    } else {
      // Factory partition is skipped on purpose: a GPIO41 refuge boot must stay put, and the
      // transient factory boot mid-install would otherwise report the golden image's stale version.
      // Also load-bearing: pre-0.0.3 golden images serve a factory_fs web bundle whose
      // disableAllInputs() disables the forced-update confirm dialog itself — unclickable deadlock.
      Serial.println("OTA: Skipping one-time checks - not CLIENT mode, no WiFi/registration, or factory partition");
      otaCheckDone = true;
    }
  }
  // Periodic forced-update/version re-check (~6 h). The boot check above is one-shot, which was
  // fine at 12 h uptimes — at 24-72 h a forced update staged in the cloud sits dormant until the
  // next reboot, and a CANCELLED push keeps the boot-cached modal stuck. Re-reporting the version
  // and re-reading the flag mid-uptime fixes both; install stays user-triggered (the check only
  // raises/clears hasForcedUpdate for the dashboard modal). Version FIRST — the cloud auto-clears
  // a satisfied forced flag on the version report, so the forced read then sees the cleared row.
  // Queue-full: retry in 60 s (a duplicate version report is harmless).
  static unsigned long lastForcedRecheck = 0;
  if (otaCheckDone && millis() - lastForcedRecheck >= FORCED_RECHECK_INTERVAL_MS
      && currentMode == MODE_CLIENT && WiFi.status() == WL_CONNECTED && isRegistered
      && currentPartitionType != 0 && !core0Busy && !otaInProgress
      && fieldOffSettled(20000) && WiFi.RSSI() >= -80) {
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
  if (rpmAxisWipePending && currentMode == MODE_CLIENT && WiFi.status() == WL_CONNECTED
      && isRegistered && !core0Busy && WiFi.RSSI() >= -80
      && (lastRpmAxisWipeTry == 0 || millis() - lastRpmAxisWipeTry > 60000)) {
    lastRpmAxisWipeTry = millis();
    HttpsRequest req = { .type = HTTPS_RESET_RPM_AXIS };
    xQueueSend(httpsQueue, &req, 0);  // queue-full → next retry picks it up
  }

  // === ADMIN CONFIG PUSH: one-shot pending-config check after the OTA checks ===
  // Boot-only, like the forced-update check. Fires once after otaCheckDone so it doesn't
  // compete with the depth-2 httpsQueue; retries next loop if the queue is momentarily full.
  static bool pendingConfigCheckDone = false;
  if (!pendingConfigCheckDone && otaCheckDone && millis() > 6000) {
    if (currentMode == MODE_CLIENT && WiFi.status() == WL_CONNECTED && isRegistered && !core0Busy && WiFi.RSSI() >= -80) {
      HttpsRequest req = { .type = HTTPS_GET_PENDING_CONFIG };
      if (xQueueSend(httpsQueue, &req, 0) == pdTRUE) pendingConfigCheckDone = true;
    } else if (currentMode != MODE_CLIENT) {
      pendingConfigCheckDone = true;   // not a cloud client — nothing to pull
    }
  }

  esp_task_wdt_reset();              // Feed the watchdog to prevent timeout
  starttime = esp_timer_get_time();
  // Latched at the top of the pass so a pass that drops the field mid-way still counts as
  // field-on (it ran control code). Consumed by the field-ON loop worst at the bottom of loop().
  ftLoopPassSeq++;  // pass tag for blame capture — must precede every TIMED_CALL in this pass
  bool passFieldOn = !gpio4IsLow;
  currentTime = millis();

  bhServiceCompletion();  // Battery Health: runs the DCIR fit once a test finishes / aborts on timeout
  cvpfServiceCompletion();  // CV plant fit: runs the gain fit once the step sequence finishes / aborts on timeout

  // SOC and runtime update every 2 seconds (runs regardless of hardwarePresent)
  if (currentTime - lastSOCUpdateTime >= SOCUpdateInterval) {
    CurrentSessionDuration = (millis() - sessionStartTime) / 1000;  // seconds
    elapsedMillis = currentTime - lastSOCUpdateTime;
    lastSOCUpdateTime = currentTime;
    UpdateEngineRuntime(elapsedMillis);  // untimed: negligible 2s-cadence arithmetic
    UpdateEngineFuel(elapsedMillis);
    TIMED_CALL(ft_UpdateBatterySOC, UpdateBatterySOC(elapsedMillis));  // always runs: alternator stats + AvgVoltage are shunt-independent; Bcur-dependent sections gate on HAS_BATT_SHUNT internally
    UpdateTravelStatistics(elapsedMillis);
    UpdateBoardTempPressureMaximums();
    handleSocGainReset();   // do the dynamic updates
    handleAltZeroReset();   // do the dynamic udpates
    // These three flash writers use fieldCutSettled (hardware gate) — the duty-based
    // fieldOffSettled reads "off" during a live duty-0 CV hold (2026-08-13 loop-stall
    // incident class), which let small NVS/LittleFS commits land on live control passes.
    TIMED_CALL(ft_kneeLearnService, kneeLearnService(fieldCutSettled(2000)));  // Auto Min% learning: NVS flush of learned floors
    if (fieldCutSettled(2000)) TIMED_CALL(ft_bhFlushCapNVS, bhFlushCapNVS());  // Battery Health persist
    if (fieldCutSettled(2000)) overheatHistFlush();  // overheat-history NVS persist — dirty-flagged at overheat entry

    // Barometric pressure history sampler — 5-min cadence into baroPressureHistory ring.
    // Skipped if BMP388 hasn't reported (NAN). Wall-clock epoch stamped only if timeIsSynced
    // (NTP or GPS); on cold boot before sync, last-sample epoch stays at its loaded NVS value
    // so the JS can detect a power-off gap and grey out the affected slots.
    if (hardwarePresent == 1 && baroPressureHistory && (currentTime - lastBaroSampleMs >= BARO_SAMPLE_INTERVAL_MS)) {   // sim mode: no fake baro history
      lastBaroSampleMs = currentTime;
      if (!isnan(baroPressure)) {
        baroPressureHistory[baroHistoryHead] = (uint16_t)(baroPressure * 10.0f + 0.5f);
        baroHistoryHead = (baroHistoryHead + 1) % BARO_HISTORY_SIZE;
        if (timeIsSynced) baroHistoryLastEpoch = timeBase + (currentTime - timeBaseMillis) / 1000;
      }
    }

    // Long-term plot ring → flash while the field gate is physically cut (fieldCutSettled —
    // never the duty-based fieldOffSettled, which reads "off" during a live duty-0 CV hold;
    // 2026-08-13 loop-stall incident). Never write while the control loop is live. The ring
    // is a 30-day rolling cache, so deferring the flush across a long engine run costs only
    // power-cut durability — the 72 h hard maintenance reboot and the shutdown paths both
    // dump it explicitly. Dumps on the cut-settled rising edge AND periodically thereafter.
    // The prev_longTermHead guard skips a dump when nothing changed since the last one.
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
  calculateChargeTimes();  // untimed: negligible arithmetic. Might want to move into the 2s block above and unthrottle later
  // Fast alt-current channel: bounded DMA drain (~1 ms hard cap). Unconditional and
  // out-of-band of all control — sampling is hardware-timed (DMA fills itself), this
  // only empties the driver pool. Runs engine-off too (noise-floor scope view) and
  // self-suspends when the channel reads railed (broken/missing jumper).
  TIMED_CALL(ft_fastAltDrain, faDrain());
  // Failure-detector verdict consume: the heavy pulse-pattern analysis runs on the Core-0
  // faDetTask (on Core 1 it stalled the loop ~30 ms once a minute). This only consumes a finished
  // result + fires side-effects on Core 1, so ft_faDetector reads ~0; idle cost is a flag check.
  TIMED_CALL(ft_faDetector, faDetectorPoll());
  // No periodic NVS save here: nvs_commit() can block Core 1 for hundreds of ms during
  // flash sector erase, which collides with the voltage loop and risks OV on transients.
  // NVS persists only at the field-off edge (saveNVSDataFull() further down in loop)
  // and at the shutdown sequence — both run with field off, so any commit duration is safe.
  // Deferred saves from Core 0 button handlers — executed here on Core 1 so Core 0 SSE is not blocked.
  // Fired immediately, no electrical-zone gate: every flag here is set only by a manual UI action
  // (reset/clear/save buttons, or a user-requested SystemID run), so the brief flash write can only
  // happen as a direct result of a user click — never autonomously. All flags execute in the same tick.
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
  if (pendingResetAlternatorHealth) {
    pendingResetAlternatorHealth = false;
    resetAlternatorHealth();
  }
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
  // ========== POWER MANAGEMENT: Handle ignition state and WiFi wake mode ==========
  // This runs BEFORE the mode switch to ensure WiFi is in correct state before attempting transmission
  // Power management affects AP and CLIENT modes, but NOT CONFIG mode (CONFIG mode exits early below)
  if (currentMode != MODE_CONFIG) {
    static int lastIgnitionState = 1;            // Track previous ignition state (-1 = uninitialized)
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
            if (pendingResetAlternatorHealth) {
              pendingResetAlternatorHealth = false;
              resetAlternatorHealth();
            }
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
            shutdownCloudDeadlineMs = millis() + 1800000;  // 30-min window: fieldOffSettled() gates fire at 60-75s after field off; 30 min gives full time for NTP, uploads, weather, and buffer drain
          } else if (shutdownCloudDeadlineMs && (int32_t)(millis() - shutdownCloudDeadlineMs) < 0) {  // rollover-safe "now < deadline"
            // Phase 3: hold 240MHz and WiFi for the full 30-min window unconditionally.
            // Lets the CloudFeatures block continue uploading, and keeps WiFi open to verify the drain.
            setCpuFrequencyMhz(240);
          } else {
            // Phase 4: 30-min cloud drain window expired. If the PSRAM ring
            // still has unsent records, bulk-dump them to LittleFS so they
            // survive the imminent power-down. setup() will restore them on
            // next boot. Acceptable to block on flash here — device is about
            // to sleep anyway.
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
          // Upload in flight — hold at 240MHz/WiFi-on, but cap the wait to 5 minutes
          // so a stuck upload (bad WiFi, server error, malformed payload) can't hold
          // the device at high power indefinitely
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
      }
      wifiWakeStart = 0;               // Clear wake mode
      lastWifiWakeActive = false;      // Reset wake mode tracking
      wakeExpiryWarningShown = false;  // Reset warning
      setCpuFrequencyMhz(240);         // Ensure full speed
      if (tempTaskHandle != NULL) {
        vTaskResume(tempTaskHandle);  // RESUME BACKGROUND TASKS AFTER SPEEDING UP CPU
        tempTaskSuspended = false;
        lastTempTaskHeartbeat = millis();  // restart the 20s window so the health check doesn't trip before the task posts its first beat
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
      // Read sensors: Real hardware or fake data depending on hardwarePresent flag
      if (hardwarePresent == 1) {
        checkTempTaskHealth();  // digital temperature measurement monitor (only with real hardware)

        TIMED_CALL(ft_ReadAnalogInputs, ReadAnalogInputs());
      } else {
        imuEnabled = true;        //hack but it works for now
        ReadAnalogInputs_Fake();  // Fake sensor readings for development
        delay(1);                 // yield to asyncTCP callbacks — I2C wait states do this naturally in real mode
        // Notify user every 60 seconds that we're in fake mode
        static unsigned long lastFakeWarning = 0;
        if (millis() - lastFakeWarning > 60000) {
          Serial.println("hardwarePresent=0: Using fake sensor data");
          if (WiFi.getMode() != WIFI_OFF) {
            events.send("hardwarePresent flag is 0 - using fake sensor data", "console", millis());
          }
          lastFakeWarning = millis();
        }
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
      // n2kTxEnable also forces ParseMessages: node mode needs it for address claim + heartbeat
      // servicing even if the user left the receive toggle off. dvccEn forces it too: the follow
      // feature must keep hearing the authority even with the general receive toggle off. So does
      // rvcTxEnable — an RV-C-only producer is still a claiming node on the same bus.
      if ((NMEA2KData == 1 || n2kTxEnable == 1 || rvcTxEnable == 1 || dvccEn == 1) && hardwarePresent == 1) {
        TIMED_CALL(ft_n2kParse, NMEA2000.ParseMessages());  // CAN bus (only with real hardware)
        TIMED_CALL(ft_n2kTx, nmea2kTransmitTick());
      }
      // The DVCC brain runs UNCONDITIONALLY (pure 1 Hz arithmetic, no bus/I2C): if the gate above
      // stops (e.g. hardwarePresent flipped to 0 mid-FOLLOW), silence timeout still reverts the
      // clamps instead of the control loop obeying a frozen CVL/CCL forever.
      TIMED_CALL(ft_dvcc, dvccTick());
      TIMED_CALL(ft_CheckAlarms, CheckAlarms());  // Process alarms (runs with fake or real data)
      calculateThermalStress();                   // alternator lifetime modeling (runs with fake or real data)
      //UpdateDisplay();
      zeroFitService();  // Temp-comp zero correction: daily fit (engine+field off) + live correction (must be before AdjustField)

      // Runs in every mode: the stale-forecast release inside must fire with no internet (AP mode,
      // WiFi loss) or a solar pause latches on and holds the field off forever. The fetch itself is
      // still gated on client mode + WiFi inside the call.
      updateWeatherMode();  // untimed: core-1 cost is local analysis only; fetch is queued to Core 0
      TIMED_CALL(ft_solarLedger, solarLedgerService());  // 1 Hz local-day roll of the solar ledger; flash only at field-cut-settled
      TIMED_CALL(ft_AdjustFieldLearnMode, AdjustFieldLearnMode());
      // Inner-PID firing + INA228 read interval re-baseline: while the field is down, drop the
      // previous-sample timestamp so the first sample after re-enable measures a fresh interval,
      // not the whole field-off gap. The INA reset also closes the over-temp case where a field-off
      // flash write stalls the entire loop before inaFastModeActive flips to slow mode — without it
      // that stall is logged as a fast-mode (field-on) read interval. gpio4IsLow reflects field
      // state set inside the call above.
      if (gpio4IsLow) { pfHasPrev = false; inaResetIntervalBaseline(); }
      TIMED_CALL(ft_huntGov, runHuntGovernor());  // hunt-detector evaluation (self-gated to every 32 control ticks; never inside the control call)
      TIMED_CALL(ft_altHealth, altHealth_tick(millis()));
      // Gate-tuning session logger: 10 Hz row writes, plus a per-pass accumulator that averages every
      // fresh CH1 conversion into the row's raw-amps column. Called every pass (not from the fold) so a
      // field-off stretch is logged as a field-off stretch instead of as a hole. Idle cost: one compare.
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

      // ===== FIELD-OFF NVS DRAIN (60s settled, once per field-off window) =====
      // Keyed on gpio4IsLow (the physical gate), NOT duty-derived fieldActiveStatus — the
      // duty signal reads "off" during a live duty-0 CV hold, which let the heavy NVS
      // commit and the ~67 KB alt-health/boat-perf writes fire while the control loop and
      // every field-on clock were live (2026-08-13 loop-stall incident). Thresholds and the
      // 60s-not-5s protection-lockout rationale live with the flag declarations
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
        if (!fieldOffFlushDone && !pendingShutdownFlush && !fieldNowActive
            && fieldOffEdgeMs > 0 && (millis() - fieldOffEdgeMs) >= 20000UL) {
          saveNVSDataFull();
          fieldOffFlushDone = true;
        }
        // Gate 2 (+28 s): write performance matrix to LittleFS — staggered so
        // the ~150 ms file write doesn't land on the NVS commit's relocation tail
        if (!fieldOffMatrixFlushDone && !pendingShutdownFlush && !fieldNowActive
            && fieldOffEdgeMs > 0 && (millis() - fieldOffEdgeMs) >= 28000UL) {
          altHealthSave();
          boatPerfSave();
          fieldOffMatrixFlushDone = true;
        }
      }

      // Local long-term accumulation — runs regardless of CloudFeatures so the 30-day
      // local ring (and its Long Term plots) fill even in AP mode / no-WiFi. Only the
      // cloud uploads below are gated. uploadSensorHistory() self-skips during OTA; the
      // window accumulates while the field is on and is saved at any time, no internet.
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

      if (CloudFeatures == 1) {
        if (otaInProgress) {
          return;  // Skip during OTA
        }
        unsigned long currentMillisz = millis();
        // Check time sync every 12 hours — requires field off for 60s (fieldOffSettled)
        if (fieldOffSettled(0)) TIMED_CALL(ft_checkTimeSync, checkTimeSync());

        // Upload buffered records every BUFFER_UPLOAD_INTERVAL — requires field off for 70s.
        // Bumped from 65s to 70s so the buffered upload's TLS handshake doesn't land on top
        // of the field-off NVS drain (fires at 5s). 60s baseline + 10s extra = 70s total.
        // The "Upload Cloud Now" dashboard button sets forceCloudFlushPending = true which
        // bypasses BOTH the field-off settle AND the 13s interval throttle so records drain
        // back-to-back (still rate-limited by the HTTPS queue depth on Core 0).
        bool flushBypass = forceCloudFlushPending;
        if ((fieldOffSettled(10000) || flushBypass)
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
        
        // Configuration Snapshot — requires field off for 10s. Sim mode (HardwarePresent=0):
        // skip — never push fake stats/state/leaderboard data to the cloud.
        // Fires on the 24 h backstop OR on configSnapshotRequested (boot / vessel save /
        // registration), at most once per minute so a persistently-failing gate can't spin.
        if (hardwarePresent == 1 && fieldOffSettled(10000)
            && (millis() - lastConfigSnapshotTime >= CONFIG_SNAPSHOT_INTERVAL
                || (configSnapshotRequested && millis() - lastConfigSnapshotTime >= 60000))) {
          lastConfigSnapshotTime = millis();
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
                    queueConsoleMessage("Config snapshot: HTTPS queue full, will retry next interval");
                  }
                }
              }
            }
          }
        }

        // Boat-performance aggregates → cloud (field-off gated; sim never uploads).
        // rpmAxisWipePending: the upload RESPONSE carries the cloud-rebuilt front, which would
        // overwrite the local wipe with old-scaled points. Stay silent until the cloud wipe lands.
        if (hardwarePresent == 1 && !rpmAxisWipePending && fieldOffSettled(10000) && millis() - lastBoatPerfUploadTime >= BOATPERF_UPLOAD_INTERVAL) {
          lastBoatPerfUploadTime = millis();
          if (currentMode == MODE_CLIENT && WiFi.status() == WL_CONNECTED && isRegistered && WiFi.RSSI() >= -80) {
            HttpsRequest req = {};
            req.type = HTTPS_UPLOAD_BOATPERF;
            req.payloadCap = PERF_UPLOAD_BUF_SIZE;
            req.payload = (char *)ps_malloc(req.payloadCap);
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

        // Alternator-health best-ever records → cloud (field-off gated; sim never uploads).
        // Alt-health LEARNS with the field ON, but uploads (like all flash/HTTPS) are field-OFF:
        // records bank during charging and flush at the next field-off. buildAltHealthPayload
        // is dirty-gated, so this is a no-op when nothing new has been banked.
        if (hardwarePresent == 1 && !rpmAxisWipePending && fieldOffSettled(10000) && millis() - lastAltHealthUploadTime >= ALTHEALTH_UPLOAD_INTERVAL) {
          lastAltHealthUploadTime = millis();
          if (currentMode == MODE_CLIENT && WiFi.status() == WL_CONNECTED && isRegistered && WiFi.RSSI() >= -80) {
            HttpsRequest req = {};
            req.type = HTTPS_UPLOAD_ALTHEALTH;
            req.payloadCap = ALT_UPLOAD_BUF_SIZE;
            req.payload = (char *)ps_malloc(req.payloadCap);
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

        // App-usage analytics — one small POST per LOCAL calendar day (boundary = local
        // midnight via usageTzOffsetS; wall-clock, so maintenance restarts can't stretch or
        // skip a period). Retries at most once per minute while a gate fails. An empty period
        // just rolls forward without uploading. Reset happens at queue time: a send that later
        // fails loses that day (lossy by design, matches the accumulator's power-cut policy);
        // UsageDays_AllTime counts DELIVERED uploads in executeUploadUsage, not queue admission.
        // fieldCutSettled (hardware gate), NOT fieldOffSettled: the period roll writes
        // /usage.bin, and the duty-based gate reads "off" during a live duty-0 CV hold
        // (2026-08-13 loop-stall incident) — so charging across midnight defers the roll to
        // the first field-off minute after.
        if (hardwarePresent == 1 && fieldCutSettled(10000)
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

        // Commissioning-ledger drain (COMMISSIONING_LEDGER_SPEC.md): queued stage/sweep events →
        // log-commissioning-event. Deliberately NEVER fires during commissioning itself — rows wait
        // on flash until the boat is idle; the network gates live inside the service, the flash-safety
        // gates here, because the post-send file trim is a flash write. fieldCutSettled (hardware
        // gate), NOT fieldOffSettled: the duty-based gate reads "off" during a live duty-0 CV hold,
        // which let these flash writes stall live control passes (2026-08-13 loop-stall incident).
        if (hardwarePresent == 1 && fieldCutSettled(10000)) {
          cxLedgerDrainService();
          huntLedgerService();  // hunt-episode records queued in RAM by the control path → flash, here only
          huntMapService();     // damper pocket map — same rule: flash only here, never in the control path
          owRoleService();      // 1-Wire role auto-binds queued by TempTask (Core 0 never writes NVS) → flash, here only
        }
      }
      TIMED_CALL(ft_ch1_compute_stats, ch1_compute_stats());
      TIMED_CALL(ft_SendWifiData, SendWifiData());  // Safely sends data (has internal guard to check if WiFi is actually on)

      // Client-specific connection monitoring
      // if (currentMode == MODE_CLIENT) {  // moved the gating check into the checkwificonnection function WAS NOT SUFFICIENT FOR WHATEVER REASON
      // pendingShutdownFlush belongs here: the ignition-off drain holds 240MHz and the radio up for
      // up to 30 min to finish uploads, but never calls enterLowPowerStandby(), so wifiNapActive is
      // still false. Without it the reconnect engine sat idle through exactly the window whose whole
      // purpose is network work. checkWiFiConnection() self-guards if the radio is actually off.
      if (currentMode == MODE_CLIENT && (Ignition == 1 || pendingShutdownFlush || (wifiWakeStart > 0 && (millis() - wifiWakeStart) < WIFI_WAKE_DURATION) || wifiNapActive)) {
        // if (currentMode == MODE_CLIENT && (Ignition == 1 || wifiWakeActive)) { // can't try to do anything wifi related unless ignition is on and clock speed is fast enough to not crash
        TIMED_CALL(ft_checkWiFiConnection, checkWiFiConnection());

        // Track actual WiFi disconnects
        static unsigned long lastWiFiStatusCheck = 0;
        static bool lastWiFiConnected = true;

        if (millis() - lastWiFiStatusCheck > 5000) {
          bool currentlyConnected = (WiFi.status() == WL_CONNECTED);

          if (lastWiFiConnected && !currentlyConnected) {
            wifiDisconnectCount++;
            queueConsoleMessageF("WiFi disconnected #%u", wifiDisconnectCount);
          } else if (!lastWiFiConnected && currentlyConnected) {
            wifiReconnectsTotal++;
            queueConsoleMessageF("WiFi reconnected #%u", wifiReconnectsTotal);
          }

          lastWiFiConnected = currentlyConnected;
          lastWiFiStatusCheck = millis();
        }
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
  // Worst-loop attribution. All ft_*.lastCall are THIS pass's durations. ctrl(inclFold) already contains
  // fold. A part larger than total means that function was throttled this pass and its lastCall is stale —
  // judge by total. Emitted after endtime so it never counts against the measured pass.
  // Two headless serial streams, both non-blocking (skipped when the UART TX FIFO is full, so printing
  // never adds loop latency). No watermark auto-reset — the stall cadence is unknown, so instead:
  //   (1) a PER-EVENT line on every over-threshold pass (not just a new worst), carrying the gap since the
  //       previous one so cadence reads directly off the stream. Throttled to 1 per 100 ms; suppressed
  //       passes still count toward the window and report as sup=.
  //   (2) a 30 s WINDOW summary that SELF-RESETS — 10/15/20 ms bucket counts + window max. This is the
  //       live signal with no client attached and no watermark to clear.
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
  // Field-ON loop health: same pass timer, gated to passes that started with the field gate
  // open. Splits real control-path stalls (CV-tick / overvoltage risk) from intentional
  // field-off background work (flash flushes, uploads) that dominates the ungated worst.
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
