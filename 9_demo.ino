// X Engineering Alternator Regulator
//     Copyright (C) 2025  Mark Nickerson
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

// DEMO MODE ("Hardware Present?" = No). One simulated boat feeds every sensor input while the field drive stays real; the
// field driver's own protections read the real chips (demoReadFieldDriveReal, owDemoAltRead). Every persistent writer is
// sealed (NVS, LittleFS, RTC, cloud), settings outside demoParamDenied's allow-list are refused, and the real state comes
// back in place, with no restart, when the switch returns to Yes. Design, seal map and the backfill contract: claude_docs/firmware-internals.md, "Demo mode".

// ============================================================================================================
// Settings overlay: while sealed, settingWrite/Read/Exists/Remove (2_functions.ino) go here, never to NVS
// ============================================================================================================
#define DEMO_OV_CAP 512
struct DemoOvEntry {
  char key[16];
  char *val;      // PSRAM copy; nullptr = removed in demo
};
static DemoOvEntry *demoOv = nullptr;
static int demoOvN = 0;
static bool demoOvLive = false;        // false while exiting: reads fall through to NVS, writes are dropped
static SemaphoreHandle_t demoOvMtx = nullptr;

static int demoOvFind(const char *key) {
  for (int i = 0; i < demoOvN; i++)
    if (strncmp(demoOv[i].key, key, 15) == 0) return i;
  return -1;
}
bool demoOvWrite(const char *key, const char *value) {
  if (!demoOvLive || !demoOv || !demoOvMtx || !key) return true;
  xSemaphoreTake(demoOvMtx, portMAX_DELAY);
  int i = demoOvFind(key);
  if (i < 0 && demoOvN < DEMO_OV_CAP) {
    i = demoOvN++;
    strncpy(demoOv[i].key, key, 15);
    demoOv[i].key[15] = '\0';
    demoOv[i].val = nullptr;
  }
  if (i >= 0) {
    if (demoOv[i].val) free(demoOv[i].val);
    demoOv[i].val = nullptr;
    if (value) {
      size_t n = strlen(value) + 1;
      demoOv[i].val = (char *)ps_malloc(n);
      if (demoOv[i].val) memcpy(demoOv[i].val, value, n);
    }
  }
  xSemaphoreGive(demoOvMtx);
  return true;
}
// The one write that passes the seal: a field OFF must stay off after exit and after a reboot (the /get OnOff=0 path).
bool demoWriteThrough(const char *key, const char *value) {
  nvs_handle_t h;
  if ((nvs_open)(SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
  esp_err_t e = nvs_set_str(h, key, value);
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);
  demoOvWrite(key, value);
  return e == ESP_OK;
}
// -1 = not overlaid (read NVS), 0 = removed in demo, 1 = found (out set)
int demoOvRead(const char *key, String *out) {
  if (!demoOvLive || !demoOv || !demoOvMtx || !key) return -1;
  xSemaphoreTake(demoOvMtx, portMAX_DELAY);
  int i = demoOvFind(key);
  int r = -1;
  if (i >= 0) {
    r = demoOv[i].val ? 1 : 0;
    if (r == 1 && out) *out = String(demoOv[i].val);
  }
  xSemaphoreGive(demoOvMtx);
  return r;
}
static void demoOvClear() {
  if (!demoOv || !demoOvMtx) return;
  xSemaphoreTake(demoOvMtx, portMAX_DELAY);
  for (int i = 0; i < demoOvN; i++) { if (demoOv[i].val) free(demoOv[i].val); demoOv[i].val = nullptr; }
  demoOvN = 0;
  xSemaphoreGive(demoOvMtx);
}

// ============================================================================================================
// The simulated boat
// ============================================================================================================
static uint32_t demoT0Ms = 0;          // millis() at entry: script time zero
static uint32_t demoEpoch0 = 0;        // wall-clock epoch at entry: the made-up history ends here
#define DEMO_FALLBACK_EPOCH 1793102400UL   // 2026-10-27 12:00 UTC, only when the clock never synced
static float demoSec() { return (float)(uint32_t)(millis() - demoT0Ms) * 0.001f; }
uint32_t demoNowEpoch() {
  if (timeIsSynced && timeBase > 0) return (uint32_t)(timeBase + (millis() - timeBaseMillis) / 1000);
  return demoEpoch0 + (uint32_t)((uint32_t)(millis() - demoT0Ms) / 1000);
}
static float demoClassK() { return (float)SYSTEM_VOLTAGE_CLASS / 12.0f; }
static float demoAnom() { return (AlternatorNominalAmps > 10) ? (float)AlternatorNominalAmps : 100.0f; }
// The owner's bank when it is a plausible house bank, else 200 Ah; demoEnter() holds BatteryCapacity_Ah at it so the
// regulator's own SoC count and the simulated battery agree
static float demoCapAh() { return (BatteryCapacity_Ah >= 40 && BatteryCapacity_Ah <= 3000) ? (float)BatteryCapacity_Ah : 200.0f; }
static float demoWrap360(float a) { while (a < 0) a += 360.0f; while (a >= 360.0f) a -= 360.0f; return a; }
static float demoWrap180(float a) { a = demoWrap360(a); return (a > 180.0f) ? a - 360.0f : a; }
static float demoSmooth(float x) { if (x <= 0) return 0; if (x >= 1) return 1; return x * x * (3.0f - 2.0f * x); }
static float demoHash01(uint32_t x) {   // deterministic 0..1 from an integer (history days, noise)
  x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
  return (float)(x & 0xFFFFFF) / 16777216.0f;
}

// ---- Noise for the simulated instruments: a function of its index, so the made-up past and the live run are one series,
// and never periodic ----
// Standard normal deviate for (index, stream): Box-Muller on two hashes
static float demoGauss(uint32_t i, uint32_t stream) {
  float u1 = demoHash01(i * 2654435761u + stream * 0x9E3779B9u) + 0.5f / 16777216.0f;
  float u2 = demoHash01(i * 0x85EBCA6Bu + stream * 0xC2B2AE35u + 0x27D4EB2Fu);
  return sqrtf(-2.0f * logf(u1)) * cosf(2.0f * PI * u2);
}
// Smooth random wander: a Catmull-Rom curve through normal deviates one unit of x apart (spread about 0.8)
static float demoWander(float x, uint32_t stream) {
  float fl = floorf(x), t = x - fl;
  uint32_t i = (uint32_t)(int32_t)fl;
  float p0 = demoGauss(i - 1u, stream), p1 = demoGauss(i, stream), p2 = demoGauss(i + 1u, stream), p3 = demoGauss(i + 2u, stream);
  return 0.5f * (2.0f * p1 + (p2 - p0) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t * t
                 + (3.0f * (p1 - p2) + p3 - p0) * t * t * t);
}
// One step of a first-order (Ornstein-Uhlenbeck) process with time constant tau and spread sigma, tau in steps
static float demoOuStep(float x, float tau, float sigma, uint32_t k, uint32_t stream) {
  float a = expf(-1.0f / tau);
  return a * x + sigma * sqrtf(1.0f - a * a) * demoGauss(k, stream);
}
// An on/off appliance or weather event: on and off spans drawn per cycle, walked from an origin before the made-up hour.
// The walk is cached; script time only moves forward, and a step back (a new entry) walks again from the origin.
#define DEMO_ORIGIN_S (-3900.0f)          // the made-up past starts here (an hour of backfill plus five minutes to settle)
struct DemoCycler {
  float start = 1e30f;
  uint32_t n = 0;
  // true while on at script second s; into = seconds since this cycle began, len = its on span
  bool at(float s, uint32_t stream, float onMin, float onMax, float offMin, float offMax, float *into, float *len) {
    if (s < start) { start = DEMO_ORIGIN_S - (onMax + offMax) * demoHash01(stream * 7919u); n = 0; }
    for (;;) {
      float on = onMin + (onMax - onMin) * demoHash01(n * 2246822519u + stream);
      float off = offMin + (offMax - offMin) * demoHash01(n * 3266489917u + stream + 1u);
      if (s < start + on + off) {
        *into = s - start;
        *len = on;
        return *into < on;
      }
      start += on + off;
      n++;
    }
  }
};

// Rockland Harbor mooring field, Penobscot Bay, Maine
#define DEMO_LAT0 44.0985
#define DEMO_LON0 -69.1000
#define DEMO_HULL_KT 7.4f
#define DEMO_TWD 215.0f                  // SW sea breeze
#define DEMO_IDLE_RPM 780.0f
#define DEMO_CRUISE_RPM 1950.0f

struct DemoLeg { float dur, hdg, rpm, kt; bool sail; bool dock = false; };   // dock: made fast on shore power, engine off
#define DEMO_MAX_LEGS 12
#define DEMO_DOCK_BRG 320.0f             // the town dock from the mooring, deg / nm
#define DEMO_DOCK_NM 0.22f
#define DEMO_DOCK_HDG 200.0f             // alongside, bow into the sea breeze
#define DEMO_DOCK_S 900.0f
static DemoLeg demoLegs[DEMO_MAX_LEGS];
static double demoLegLat[DEMO_MAX_LEGS + 1], demoLegLon[DEMO_MAX_LEGS + 1];
static int demoLegN = 0;
static float demoLoopS = 3600.0f;

// Polar: best STW vs true wind; the same shape the boat-performance simulator used
static float demoPolarShape(float twa) {
  static const float S[19] = {0,0,0,0.42f,0.60f,0.74f,0.84f,0.90f,0.95f,0.98f,1.00f,1.00f,0.98f,0.94f,0.89f,0.85f,0.81f,0.78f,0.72f};
  float pa = fabsf(demoWrap180(twa));
  float f = pa / 10.0f; int i = (int)f; if (i > 17) i = 17; float t = f - i;
  return S[i] * (1.0f - t) + S[i + 1] * t;
}
static float demoSailStw(float tws, float twa, float sea) {
  return DEMO_HULL_KT * (1.0f - expf(-tws / 8.0f)) * demoPolarShape(twa) * (1.0f - 0.03f * sea);
}
static float demoMotorStw(float rpm, float headwind, float sea) {
  float v = DEMO_HULL_KT * (1.0f - expf(-(rpm - 400.0f) / 900.0f));
  if (v < 0) v = 0;
  v -= 0.015f * (headwind > 0 ? headwind : 0);
  return v * (1.0f - 0.03f * sea);
}
// The live boat against those best-ever curves (trim, a little growth on the hull): the "% of best" the demo shows
static float demoSailEff(float s) { return constrain(0.970f + 0.010f * demoWander(s / 300.0f, 191) + 0.005f * demoWander(s / 60.0f, 192), 0.952f, 0.988f); }
static float demoMotorEff(float s) { return constrain(0.965f + 0.009f * demoWander(s / 400.0f, 193) + 0.004f * demoWander(s / 70.0f, 194), 0.950f, 0.980f); }
static void demoMovePos(double &lat, double &lon, float hdgDeg, float nm) {
  double h = hdgDeg * PI / 180.0;
  lat += nm * cos(h) / 60.0;
  lon += nm * sin(h) / (60.0 * cos(lat * PI / 180.0));
}
// Depth (m): the harbour bottom at the mooring shelving into West Penobscot Bay, with ledges and holes along the way
static float demoDepthM(double lat, double lon) {
  float n = (float)((lat - DEMO_LAT0) * 60.0), e = (float)((lon - DEMO_LON0) * 60.0 * cos(DEMO_LAT0 * PI / 180.0));
  float r = sqrtf(n * n + e * e);
  return fmaxf(3.0f, 7.0f + 33.0f * (1.0f - expf(-r / 1.1f)) + (1.0f + 3.0f * fminf(r, 1.0f)) * (demoWander(n / 0.22f, 181) + demoWander(e / 0.22f + 50.0f, 182)));
}
// Out of Rockland Harbor between the breakwater and Owls Head under power, a close reach in West Penobscot Bay, then
// home under power by way of the town dock: a quarter hour alongside with the engine off (water, pump-out, the shore
// charger on), then across to the mooring; the last legs are solved so the loop closes there.
static float demoBearingTo(double lat, double lon, double tLat, double tLon, float *nm) {
  double dLatNm = (tLat - lat) * 60.0, dLonNm = (tLon - lon) * 60.0 * cos(lat * PI / 180.0);
  if (nm) *nm = (float)sqrt(dLatNm * dLatNm + dLonNm * dLonNm);
  return demoWrap360((float)(atan2(dLonNm, dLatNm) * 180.0 / PI));
}
static void demoBuildLegs() {
  demoLegN = 0;
  float motorKt = 0.965f * demoMotorStw(DEMO_CRUISE_RPM, 0, 0.9f), idleKt = 0.965f * demoMotorStw(DEMO_IDLE_RPM, 0, 0.9f);
  float sailHdg = 160.0f, sailKt = 0.97f * demoSailStw(13.5f, demoWrap180(DEMO_TWD - sailHdg), 0.9f);
  demoLegs[demoLegN++] = { 180.0f, 100.0f, DEMO_IDLE_RPM, idleKt, false };     // engine warm-up, out through the mooring field
  demoLegs[demoLegN++] = { 900.0f, 90.0f, DEMO_CRUISE_RPM, motorKt, false };   // between the breakwater and Owls Head
  demoLegs[demoLegN++] = { 720.0f, 60.0f, DEMO_CRUISE_RPM, motorKt, false };   // up West Penobscot Bay
  demoLegs[demoLegN++] = { 180.0f, DEMO_TWD, DEMO_IDLE_RPM, idleKt, false };   // head to wind, sails up
  demoLegs[demoLegN++] = { 900.0f, sailHdg, 0.0f, sailKt, true };              // close reach, engine off
  demoLegLat[0] = DEMO_LAT0; demoLegLon[0] = DEMO_LON0;
  for (int i = 0; i < demoLegN; i++) {
    demoLegLat[i + 1] = demoLegLat[i]; demoLegLon[i + 1] = demoLegLon[i];
    demoMovePos(demoLegLat[i + 1], demoLegLon[i + 1], demoLegs[i].hdg, demoLegs[i].kt * demoLegs[i].dur / 3600.0f);
  }
  double dockLat = DEMO_LAT0, dockLon = DEMO_LON0;
  demoMovePos(dockLat, dockLon, DEMO_DOCK_BRG, DEMO_DOCK_NM);
  float dist = 0, brg = demoBearingTo(demoLegLat[demoLegN], demoLegLon[demoLegN], dockLat, dockLon, &dist);
  float idleNm = idleKt * 120.0f / 3600.0f;
  demoLegs[demoLegN] = { 120.0f, brg, DEMO_IDLE_RPM, idleKt, false };           // sails down, engine back on
  demoLegLat[demoLegN + 1] = demoLegLat[demoLegN]; demoLegLon[demoLegN + 1] = demoLegLon[demoLegN];
  demoMovePos(demoLegLat[demoLegN + 1], demoLegLon[demoLegN + 1], brg, idleNm);
  demoLegN++;
  brg = demoBearingTo(demoLegLat[demoLegN], demoLegLon[demoLegN], dockLat, dockLon, &dist);
  float inNm = idleKt * 150.0f / 3600.0f;
  demoLegs[demoLegN] = { (dist - inNm) / motorKt * 3600.0f, brg, DEMO_CRUISE_RPM, motorKt, false };   // home
  demoLegLat[demoLegN + 1] = demoLegLat[demoLegN]; demoLegLon[demoLegN + 1] = demoLegLon[demoLegN];
  demoMovePos(demoLegLat[demoLegN + 1], demoLegLon[demoLegN + 1], brg, dist - inNm);
  demoLegN++;
  demoLegs[demoLegN++] = { 150.0f, brg, DEMO_IDLE_RPM, idleKt, false };         // in to the dock at idle
  demoLegLat[demoLegN] = dockLat; demoLegLon[demoLegN] = dockLon;
  demoLegs[demoLegN++] = { DEMO_DOCK_S, DEMO_DOCK_HDG, 0.0f, 0.0f, false, true };   // alongside, engine off, shore power
  demoLegLat[demoLegN] = dockLat; demoLegLon[demoLegN] = dockLon;
  brg = demoBearingTo(dockLat, dockLon, DEMO_LAT0, DEMO_LON0, &dist);
  demoLegs[demoLegN++] = { dist / idleKt * 3600.0f, brg, DEMO_IDLE_RPM, idleKt, false };   // across to the mooring
  demoLegLat[demoLegN] = DEMO_LAT0; demoLegLon[demoLegN] = DEMO_LON0;
  demoLoopS = 0;
  for (int i = 0; i < demoLegN; i++) demoLoopS += demoLegs[i].dur;
}
// Script position: leg index and fraction at script time s
static int demoLegAt(float s, float *frac, float *intoLeg) {
  float tau = fmodf(s < 0 ? 0 : s, demoLoopS);
  for (int i = 0; i < demoLegN; i++) {
    if (tau < demoLegs[i].dur || i == demoLegN - 1) {
      if (frac) *frac = (demoLegs[i].dur > 0) ? tau / demoLegs[i].dur : 0;
      if (intoLeg) *intoLeg = tau;
      return i;
    }
    tau -= demoLegs[i].dur;
  }
  return 0;
}

// ---- Weather ----
// Sea breeze of 12-18 kn. The 10-minute mean wanders on half-hour and 7-minute scales; gusts and lulls are a ~25 s process,
// turbulence a ~4 s one, and an occasional puff (cat's paw) adds 2-5 kn for up to a minute and veers a few degrees. The
// direction has slow persistent shifts, oscillations of several minutes, ~10 s noise, and veers in the gusts. Gust factor
// (peak of a 3 s average over the 10-minute mean) about 1.25-1.35. The fast parts are stepped once a second (demoWindTo) from
// DEMO_ORIGIN_S at entry, so the made-up hour and the live run are one series.
static float demoWindMean(float s) { return 14.5f + 1.3f * demoWander(s / 1500.0f, 101) + 0.7f * demoWander(s / 420.0f, 102); }
static float demoWindDirMean(float s) { return DEMO_TWD + 5.0f * demoWander(s / 2600.0f, 111) + 5.0f * demoWander(s / 480.0f, 112); }
static DemoCycler demoPuffC;
static float demoPuff(float s, float *veer) {
  float into = 0, len = 0;
  *veer = 0;
  if (!demoPuffC.at(s, 121, 25.0f, 70.0f, 90.0f, 320.0f, &into, &len)) return 0;
  float h = demoHash01(demoPuffC.n * 40503u + 122u);
  float shape = fminf(demoSmooth(into / 6.0f), demoSmooth((len - into) / (0.45f * len)));
  *veer = (3.0f + 4.0f * h) * shape;
  return (2.0f + 3.0f * h) * shape;
}
// The fast wind processes at script second k (state in demoW; one step per second, never backwards)
static void demoWindTo(int32_t k);
static void demoTrueWind(float s, float &tws, float &twd);
// Barometer: synoptic highs and lows passing over days (and a faster wobble of hours), plus the semidiurnal pressure tide
// (about 0.5 mbar, highs near 10:00 and 22:00 local solar time at 69 W); the sensor's own noise is added where it is read
float demoBaroAt(uint32_t epoch) {   // mbar
  float d = (float)(int32_t)(epoch - 1767225600u) / 86400.0f;   // days since 2026-01-01
  float synoptic = 1014.0f + 7.5f * demoWander(d / 1.7f, 201) + 2.0f * demoWander(d / 0.45f, 202) + 0.4f * demoWander(d / 0.1f, 203);
  float solarHour = fmodf(d * 24.0f - 4.6f + 2400.0f, 24.0f);
  return synoptic + 0.5f * cosf(2.0f * PI * (solarHour - 10.0f) / 12.0f);
}
static float demoSeaFromTws(float tws) { return 0.25f + 0.045f * tws; }      // significant wave height, m (from the mean wind)
static float demoWavePeriod(float tws) { return 3.0f + 0.15f * tws; }        // s

// Battery: open-circuit voltage at the 12 V class, knee scaled to the owner's own charge voltage
static float demoOcv12(float soc01) {
  float bulk12 = BulkVoltage / demoClassK();
  if (soc01 < 0) soc01 = 0;
  if (soc01 > 1) soc01 = 1;
  if (batteryIsLithium()) {
    float base = 12.90f + 0.45f * soc01;
    if (soc01 > 0.88f) {
      float k = (soc01 - 0.88f) / 0.12f;
      base += (bulk12 - 0.025f - 13.35f) * powf(k, 2.6f);
    }
    return base;
  }
  float base = 11.95f + 0.80f * soc01;
  if (soc01 > 0.75f) {
    float k = (soc01 - 0.75f) / 0.25f;
    base += (bulk12 - 0.05f - 12.75f) * k * k;
  }
  return base;
}

// Alternator: output vs speed and field, the same function seeds the demo health record (health 1.0)
static float demoAltSpeedFrac(float rpm) { return (rpm < 450.0f) ? 0.0f : 1.0f - expf(-(rpm - 450.0f) / 700.0f); }
static float demoAltAmps(float rpm, float exc, float tF, float health) {
  float x = exc / 12.8f;
  if (x < 0) x = 0;
  if (x > 1.25f) x = 1.25f;
  float sat = (1.0f - expf(-2.0f * x)) / (1.0f - expf(-2.0f));
  float therm = 1.0f - 0.0012f * (tF - 100.0f);
  return demoAnom() * 1.3f * demoAltSpeedFrac(rpm) * sat * therm * health;   // cold-rated: about 1.15x nameplate at cruise
}
#define DEMO_ALT_HEALTH 0.985f           // the live grade reads 98-99 % (steady points spread about +-0.6 %)

struct DemoWorld {
  // script
  float rpm = 0, rpmTarget = 0, hdg = 100, stw = 0, leeway = 0, sog = 0, cog = 100;
  float tws = 13.5f, twd = DEMO_TWD, aws = 0, awa = 0, twaRel = 0;
  double lat = DEMO_LAT0, lon = DEMO_LON0;
  bool sail = false;
  float sea = 0.9f, waveT = 5.0f, heelMean = 0;
  // wind processes stepped once a second (demoWindTo): gust, turbulence, direction noise
  int32_t windK = 0;
  float wGust = 0, wTurb = 0, wDir = 0;
  float wavePh[2][9] = {};               // sea components' phases (pitch, roll), advanced per IMU sample
  // electrical
  float altA = 0, battA = 0, vBatt = 13.2f, vPol = 0, solarW = 0, solarBattA = 0, loadA = 0;
  // the made-up GX devices inside the plant (CSV4 demo*A): shore charger output, the fridge, Orion XS and inverter draws
  float shoreLevel = 0, shoreA = 0, fridgeA = 0, orionA = 0, inverterA = 0, engineStartS = 0;
  bool engineWasRun = false;
  // thermal
  float altT = 79.0f, engRoomF = 78.0f, engWarm = 0, battT = 74.0f, boardF = 78.0f;
  uint32_t lastStepUs = 0;
  uint32_t nextAccUs = 0, nextGyrUs = 0;
  float slamG = 0;
  float startSoc = 85.0f;
  float battLimitA = 120.0f;             // made-up GX "Limit charge current"
  uint32_t noiseK = 0;                   // sensor-noise draw counter (live only)
};
static DemoWorld demoW;

static void demoWindTo(int32_t k) {
  while (demoW.windK < k) {
    uint32_t i = (uint32_t)(++demoW.windK);
    demoW.wGust = demoOuStep(demoW.wGust, 25.0f, 1.2f, i, 131);
    demoW.wTurb = demoOuStep(demoW.wTurb, 4.0f, 0.55f, i, 132);
    demoW.wDir = demoOuStep(demoW.wDir, 10.0f, 3.0f, i, 133);
  }
}
static void demoTrueWind(float s, float &tws, float &twd) {
  float veer = 0, puff = demoPuff(s, &veer);
  tws = fmaxf(2.0f, demoWindMean(s) + demoW.wGust + demoW.wTurb + puff);
  twd = demoWrap360(demoWindDirMean(s) + demoW.wDir + 0.6f * demoW.wGust + veer);
}
// On the mooring the boat lies about 30 deg off the mean wind (a tidal stream across it), yawing slowly; the bow follows
// the wind's slow shifts, not its gusts, so the true wind angle also carries the wind's fast direction changes
static float demoMooredLie(float s) { return constrain(30.0f + 8.0f * demoWander(s / 70.0f, 141), 10.0f, 55.0f); }
static float demoMooredHdg(float s) { return demoWrap360(demoWindDirMean(s) - demoMooredLie(s)); }
// Ambient air: a sunny day whatever the clock, a few degrees of slow drift
static float demoAirF(float s) { return 72.0f + 2.0f * demoWander(s / 3000.0f, 142); }
// The live sensor noise: deviates drawn in sequence (they need no continuity with the made-up past)
static float demoNoise(uint32_t stream) { return demoGauss(++demoW.noiseK, stream); }
// DS18B20 resolution, 0.0625 C
static float demoProbeF(float f) { return 32.0f + roundf((f - 32.0f) / 1.8f * 16.0f) / 16.0f * 1.8f; }

static float demoFieldDuty() { return gpio4IsLow ? 0.0f : dutyCycle; }

static void demoNavStep(float s, float dt) {
  float frac = 0, into = 0;
  int li = demoLegAt(s, &frac, &into);
  const DemoLeg &L = demoLegs[li];
  demoW.sail = L.sail;
  // throttle held by hand: the governor wanders with load and the sea; idle hunts a little
  demoW.rpmTarget = (L.sail || L.dock) ? 0.0f
                  : L.rpm + ((L.rpm > 1000.0f) ? 22.0f * demoWander(s / 40.0f, 171) + 7.0f * demoWander(s / 3.0f, 172)
                                               : 6.0f * demoWander(s / 9.0f, 173) + 3.0f * demoWander(s / 1.4f, 174));
  // engine start: a 2 s crank to idle; throttle changes slew; shutdown is immediate
  if (demoW.rpmTarget <= 0.0f) demoW.rpm = 0.0f;
  else if (demoW.rpm < 300.0f) demoW.rpm += dt * 400.0f;
  else {
    float d = demoW.rpmTarget - demoW.rpm, lim = 180.0f * dt;
    demoW.rpm += (d > lim) ? lim : (d < -lim) ? -lim : d;
  }
  demoWindTo((int32_t)floorf(s));
  demoTrueWind(s, demoW.tws, demoW.twd);
  float wMean = demoWindMean(s);
  demoW.sea = demoSeaFromTws(wMean);
  demoW.waveT = demoWavePeriod(wMean);
  // autopilot (helm under sail) wander, plus yaw from the waves; made fast at the dock, a degree or so on the springs
  float hdgTarget = L.dock ? L.hdg + 0.6f * demoWander(s / 40.0f, 175)
                           : L.hdg + (L.sail ? 4.0f * demoWander(s / 35.0f, 175) : 2.5f * demoWander(s / 25.0f, 175))
                             + 1.2f * demoW.sea * demoWander(s / 4.0f, 176);
  float dh = demoWrap180(hdgTarget - demoW.hdg), hlim = 6.0f * dt;
  demoW.hdg = demoWrap360(demoW.hdg + ((dh > hlim) ? hlim : (dh < -hlim) ? -hlim : dh));
  demoW.twaRel = demoWrap180(demoW.twd - demoW.hdg);   // + = wind on starboard
  float twaRad = demoW.twaRel * PI / 180.0f;
  float headwindTrue = demoW.tws * cosf(twaRad);
  float stwTarget = L.sail ? demoSailStw(demoW.tws, demoW.twaRel, demoW.sea) * demoSailEff(s)
                           : demoMotorStw(demoW.rpm, headwindTrue + demoW.stw, demoW.sea) * demoMotorEff(s);
  if (L.dock) stwTarget = 0.0f;
  float tau = L.sail ? 20.0f : 25.0f;
  demoW.stw += (stwTarget - demoW.stw) * (1.0f - expf(-dt / tau));
  if (demoW.stw < 0) demoW.stw = 0;
  // apparent wind = true wind plus the boat's own headwind, in the boat frame
  float ax = demoW.tws * sinf(twaRad), ay = demoW.tws * cosf(twaRad) + demoW.stw;
  demoW.aws = sqrtf(ax * ax + ay * ay);
  demoW.awa = demoWrap360(atan2f(ax, ay) * 180.0f / PI);
  float awaS = demoWrap180(demoW.awa);
  float heelTarget = L.sail ? fminf(22.0f, 13.0f * (demoW.aws / 18.0f) * (demoW.aws / 18.0f) * fabsf(sinf(awaS * PI / 180.0f))) : 1.0f;
  if (awaS < 0) heelTarget = -heelTarget;              // wind to port heels to starboard (negative here)
  demoW.heelMean += (heelTarget - demoW.heelMean) * (1.0f - expf(-dt / 8.0f));
  demoW.leeway = L.sail ? -0.25f * demoW.heelMean : 0.3f;
  // over the ground: through the water along heading + leeway, plus the ebb setting out of the bay (0.2-0.4 kn to 205)
  float cur = (0.3f + 0.08f * demoWander(s / 1800.0f, 177)) * demoSmooth(demoW.stw / 1.5f), cr = 205.0f * PI / 180.0f;   // none while moored
  float br = (demoW.hdg + demoW.leeway) * PI / 180.0f;
  float vn = demoW.stw * cosf(br) + cur * cosf(cr), ve = demoW.stw * sinf(br) + cur * sinf(cr);
  demoW.sog = sqrtf(vn * vn + ve * ve);
  demoW.cog = (demoW.sog > 0.2f) ? demoWrap360(atan2f(ve, vn) * 180.0f / PI) : demoW.hdg;
  // position along the scripted track (so the loop closes on the mooring), wandering a few boat lengths off the line
  double lat = demoLegLat[li] + (demoLegLat[li + 1] - demoLegLat[li]) * frac;
  double lon = demoLegLon[li] + (demoLegLon[li + 1] - demoLegLon[li]) * frac;
  if (s > 0 && !L.dock) demoMovePos(lat, lon, L.hdg + 90.0f, 0.007f * demoWander(s / 80.0f, 178));
  demoW.lat = lat; demoW.lon = lon;
}

// House loads in plain amps (they do not grow with the bank), each appliance cycling on its own irregular rhythm:
// instruments, plotter and lights; a fridge (5-9 min on, 7-15 off, with its start surge); the fresh-water pump; the
// electric head; a watermaker run of 25-45 min every hour or so; the autopilot underway, harder in a bigger sea; and the
// inverter early in a sail leg. Under about 50 A while motoring, so the battery charge limit holds the battery current.
// The Orion XS's draw (demoPlantStep) comes on top. fridgeA / inverterA: the fridge's and the inverter's shares, which the
// GX's DC load meter and its inverter read.
static DemoCycler demoFridgeC, demoPumpC, demoHeadC, demoWmC;
static float demoLoadA(float s, float *fridgeA = nullptr, float *inverterA = nullptr) {
  float into = 0, len = 0;
  float a = 6.5f + 0.3f * demoWander(s / 45.0f, 151);
  float fridge = demoFridgeC.at(s, 152, 300.0f, 540.0f, 420.0f, 900.0f, &into, &len) ? 4.2f + 9.0f * expf(-into / 0.8f) : 0.0f;
  a += fridge;
  if (fridgeA) *fridgeA = fridge;
  if (demoPumpC.at(s, 153, 12.0f, 55.0f, 120.0f, 520.0f, &into, &len)) a += 5.5f;
  if (demoHeadC.at(s, 154, 14.0f, 24.0f, 700.0f, 2400.0f, &into, &len)) a += 14.0f;
  if (demoWmC.at(s, 155, 1500.0f, 2700.0f, 1800.0f, 4800.0f, &into, &len)) a += 12.5f + 0.4f * demoWander(s / 20.0f, 156);
  float frac = 0;
  int li = demoLegAt(s, &frac, &into);
  if (s > 0 && li > 0 && !demoLegs[li].dock) a += 1.0f + demoW.sea * (1.2f + 2.4f * fabsf(demoWander(s / 5.0f, 157)));
  const float inverter = (demoLegs[li].sail && into < 600.0f) ? fminf(0.15f * demoCapAh(), 60.0f) : 0.0f;   // under sail: coffee, a laptop
  a += inverter;
  if (inverterA) *inverterA = inverter;
  return a;
}
// Sun: a sunny day whatever the clock (a take shot at night still has it), the owner's array (555 W when unset) at 85 %,
// with cumulus shadows of 20-110 s every 40-320 s: soft edges, thin and thick patches inside, and the brief brightening
// as the edge passes (cloud enhancement)
static DemoCycler demoCloudC;
static float demoCloud(float s) {
  float into = 0, len = 0;
  float clear = 1.0f + 0.012f * demoWander(s / 12.0f, 162);
  if (!demoCloudC.at(s, 161, 20.0f, 110.0f, 40.0f, 320.0f, &into, &len)) return clear * (1.0f + 0.07f * expf(-(into - len) / 10.0f));
  float depth = 0.2f + 0.4f * demoHash01(demoCloudC.n * 40503u + 163u);
  float edge = fminf(6.0f + 10.0f * demoHash01(demoCloudC.n * 104729u + 164u), 0.45f * len);
  float e = fminf(demoSmooth(into / edge), demoSmooth((len - into) / edge));
  float core = constrain(depth * (1.0f + 0.3f * demoWander(s / 7.0f, 165)), 0.1f, 0.9f);
  return clear * (1.0f - (1.0f - core) * e);
}
static float demoSolarW(float s) {
  float array = (SolarWatts >= 100 && SolarWatts <= 3000) ? (float)SolarWatts : 555.0f;
  return array * 0.85f * demoCloud(s);
}

static void demoPlantStep(float s, float dt) {
  float C = demoCapAh(), k = demoClassK();
  float tF = demoW.altT;
  float exc = altExcitation(demoFieldDuty(), demoW.vBatt, tF);
  float iss = demoAltAmps(demoW.rpm, exc, tF, DEMO_ALT_HEALTH);
  demoW.altA += (iss - demoW.altA) * (1.0f - expf(-dt / 0.12f));
  float avail = demoSolarW(s) * 0.97f / fmaxf(demoW.vBatt, 10.0f);
  float mppt = constrain((BulkVoltage + 0.02f * k - demoW.vBatt) / (0.05f * k), 0.0f, 1.0f);   // MPPT absorption a touch above the regulator's
  demoW.solarBattA += (avail * mppt - demoW.solarBattA) * (1.0f - expf(-dt / 2.0f));
  demoW.solarW = demoW.solarBattA * demoW.vBatt / 0.97f;
  // Orion XS DC-DC (the made-up "Start battery charger"): charges the start battery from the house bank while the engine
  // runs, a few amps after each start as it puts back the crank, then a trickle
  bool run = demoW.rpm > 300.0f;
  if (run && !demoW.engineWasRun) demoW.engineStartS = s;
  demoW.engineWasRun = run;
  float sinceStart = s - demoW.engineStartS;
  demoW.orionA = run ? (0.3f + 6.0f * expf(-sinceStart / 150.0f)) * demoSmooth(sinceStart / 8.0f) : 0.0f;
  demoW.loadA = demoLoadA(s, &demoW.fridgeA, &demoW.inverterA) + demoW.orionA;
  // Shore charger (the made-up Phoenix Smart IP43 12/30): only alongside the town dock with the engine off, plugged in a
  // minute after making fast and out 10 s before casting off; soft start over about 20 s, off in 2 s, tapering near its
  // absorption voltage
  float alongside = 0;
  const DemoLeg &here = demoLegs[demoLegAt(s, nullptr, &alongside)];
  bool shore = here.dock && alongside > 60.0f && alongside < here.dur - 10.0f && !run;
  demoW.shoreLevel = shore ? fminf(1.0f, demoW.shoreLevel + dt / 20.0f) : fmaxf(0.0f, demoW.shoreLevel - dt / 2.0f);
  float shoreMax = constrain(0.15f * C, 10.0f, 30.0f), shoreOn = demoSmooth(demoW.shoreLevel);
  float shoreTaper = constrain((BulkVoltage + 0.05f * k - demoW.vBatt) / (0.15f * k), 0.08f, 1.0f);
  demoW.shoreA = fmaxf(0.0f, shoreMax * shoreOn * (shoreTaper + 0.012f * demoWander(s / 4.0f, 158)));
  demoW.battA = demoW.altA + demoW.solarBattA + demoW.shoreA - demoW.loadA;
  float soc01 = (float)SOC_percent / 10000.0f;
  float r0 = 0.55f / C * k, r1 = 0.40f / C * k;
  demoW.vPol += (demoW.battA * r1 - demoW.vPol) * (1.0f - expf(-dt / 120.0f));
  demoW.vBatt = demoOcv12(soc01) * k + demoW.battA * r0 + demoW.vPol;
  // thermal: the engine room warms with the engine, the alternator with load (capped under the owner's limit), the battery
  // slowly toward the cabin plus its own charging heat
  float air = demoAirF(s);
  demoW.engWarm += ((run ? 1.0f : 0.0f) - demoW.engWarm) * (1.0f - expf(-dt / (run ? 900.0f : 1800.0f)));
  demoW.engRoomF = air + 4.0f + 22.0f * demoW.engWarm;
  float load = demoW.altA / demoAnom();
  if (load < 0) load = 0;
  float teq = demoW.engRoomF + 70.0f * powf(load, 1.3f) + (run ? 6.0f : 0.0f);
  float cap = (float)TemperatureLimitF - 15.0f;
  if (teq > cap) teq = cap;
  demoW.altT += (teq - demoW.altT) * (1.0f - expf(-dt / (teq > demoW.altT ? 360.0f : 700.0f)));
  demoW.battT += ((air + 2.0f + 0.03f * fabsf(demoW.battA)) - demoW.battT) * (1.0f - expf(-dt / 1800.0f));
  demoW.boardF += ((demoW.engRoomF - 3.0f + 6.0f * demoFieldDuty() / 100.0f) - demoW.boardF) * (1.0f - expf(-dt / 240.0f));
}

// ---- commit helpers: the same side effects as a valid read in _ReadAnalogInputs_inner (5_functions.ino) ----
// Values are committed as the corrected readings: InvertAltAmps / InvertBattAmps fix a real sensor's wiring, there is none here.
static void demoCommitAltAmps(uint32_t now) {
  static uint32_t lastMs = 0;
  ch1_record(now);
  MeasuredAmps = demoW.altA + 0.12f * demoNoise(301);   // Hall sensor noise
  g_altAmpsOutOfRange = false;
  adsMissRun[1] = 0;   // the real read's rejected-sample run (5_functions.ino) does not move in demo
  g_altAmpsRunLost = false;
  MARK_FRESH(IDX_MEASURED_AMPS);
  if (!IgnoreAltCurrent && !LimpHome) wmIgnUpdate(wmIgn_amps, MeasuredAmps);
  aggAltCur.add(MeasuredAmps);
  ch1FreshFlag = true;
  float dtf = fmaxf(1.0f, (float)(now - lastMs));
  if (lastMs == 0 || dtf > 2000.0f) g_pidI_filtered = MeasuredAmps;
  else g_pidI_filtered += dtf / (OutputPIDFilterTC + dtf) * (MeasuredAmps - g_pidI_filtered);
  lastMs = now;
  iAmpRing[iAmpHead] = { now, MeasuredAmps };
  iAmpHead = (iAmpHead + 1) % I_RING_SIZE;
  if (iAmpCount < I_RING_SIZE) iAmpCount++;
  int n = OutputPIDMA_N < (int)iAmpCount ? OutputPIDMA_N : (int)iAmpCount;
  if (n < 1) n = 1;
  float sum = 0;
  for (int i = 0; i < n; i++) sum += iAmpRing[(iAmpHead + I_RING_SIZE - 1 - i) % I_RING_SIZE].val;
  g_pidMA_N = sum / (float)n;
  cvLog_tick(now);
  if (MeasuredAmps > MeasuredAmpsMax) MeasuredAmpsMax = MeasuredAmps;
  if (MeasuredAmps > MeasuredAmpsMax_AllTime) MeasuredAmpsMax_AllTime = MeasuredAmps;
}
static void demoCommitIna(uint32_t now) {
  static uint32_t lastMs = 0;
  IBV = demoW.vBatt + 0.0015f * demoNoise(302);
  MARK_FRESH(IDX_IBV);
  aggIbv.add(IBV);
  Bcur = demoW.battA + 0.02f * demoNoise(303);
  BatteryCurrent_scaled = Bcur * 100;
  ShuntVoltage_mV = (ShuntResistanceMicroOhm > 0) ? (InvertBattAmps ? -Bcur : Bcur) * ShuntResistanceMicroOhm / 1000.0f : 0.0f;
  MARK_FRESH(IDX_BCUR);
  aggBcur.add(Bcur);
  float dtf = fmaxf(1.0f, (float)(now - lastMs));
  if (lastMs == 0 || dtf > 5000.0f) { IBV_filtered = IBV; g_cvKdFiltV = IBV; }
  else {
    IBV_filtered += dtf / (VoltageFilterTC + dtf) * (IBV - IBV_filtered);
    g_cvKdFiltV += dtf / (fmaxf(CvKdVoltFiltTC, 1.0f) + dtf) * (IBV - g_cvKdFiltV);
  }
  lastMs = now;
  ibvFreshFlag = true;
  g_ibvStale = false;
  g_dBcur_dt = 0.0f;
  if (IBV > IBVMax) IBVMax = IBV;
  if (IBV > PeakVoltage_AllTime) PeakVoltage_AllTime = IBV;
  if (IBV < MinVoltage) MinVoltage = IBV;
  if (IBV < MinVoltage_AllTime) MinVoltage_AllTime = IBV;
  wmIgnUpdate(wmIgn_IBV, IBV);
  wmIgnUpdate(wmIgn_Bcur, Bcur);
  VictronVoltage = IBV + 0.01f;
  VictronCurrent = Bcur;
  MARK_FRESH(IDX_VICTRON_VOLTAGE);
  MARK_FRESH(IDX_VICTRON_CURRENT);
}

// Seaway: nine wave components with random phases spread around the peak period (a Pierson-Moskowitz-like shape). Pitch and
// heave follow the waves, roll the boat's own response near its 4.6 s roll period. Phases advance per sample, so a changing
// sea shifts the frequencies smoothly. Pitch and roll in deg, rates in deg/s, heave in g.
static const float DEMO_WAVE_R[9] = { 0.72f, 0.80f, 0.87f, 0.94f, 1.0f, 1.07f, 1.16f, 1.28f, 1.45f };   // x the peak frequency
static const float DEMO_WAVE_W[9] = { 0.13f, 0.28f, 0.43f, 0.50f, 0.48f, 0.40f, 0.30f, 0.20f, 0.13f };
static void demoSeaMotion(float dt, float &pitch, float &roll, float &pitchRate, float &rollRate, float &heave) {
  const float wp = 2.0f * PI / demoW.waveT, wr = 2.0f * PI / 4.6f;
  float rollMs = 0;
  pitch = roll = pitchRate = rollRate = heave = 0;
  for (int i = 0; i < 9; i++) {
    float w = wp * DEMO_WAVE_R[i], a = DEMO_WAVE_W[i];
    demoW.wavePh[0][i] = fmodf(demoW.wavePh[0][i] + w * dt, 2.0f * PI);
    demoW.wavePh[1][i] = fmodf(demoW.wavePh[1][i] + w * dt, 2.0f * PI);
    float x = w / wr, h = a / sqrtf((1.0f - x * x) * (1.0f - x * x) + 0.09f * x * x);
    float p0 = demoW.wavePh[0][i], p1 = demoW.wavePh[1][i];
    pitch += a * sinf(p0); pitchRate += a * w * cosf(p0); heave += a * w * w * sinf(p0 + 0.6f);
    roll += h * sinf(p1); rollRate += h * w * cosf(p1); rollMs += 0.5f * h * h;
  }
  const float Hs = demoW.sea, rk = 0.7f * (1.5f * Hs + (demoW.sail ? 1.0f : 2.0f)) / sqrtf(rollMs);
  pitch *= 2.2f * Hs; pitchRate *= 2.2f * Hs; roll *= rk; rollRate *= rk;
  heave *= 0.5f * Hs / 9.81f;
}
// IMU: samples at the real 104 Hz accel / 52 Hz gyro rates
static void demoImuStep() {
  if (!imuRingBuffer) return;
  uint32_t nowUs = micros();
  if (demoW.nextAccUs == 0 || (int32_t)(nowUs - demoW.nextAccUs) > 200000) { demoW.nextAccUs = nowUs; demoW.nextGyrUs = nowUs; }
  int pushed = 0;
  while ((int32_t)(nowUs - demoW.nextAccUs) >= 0 && pushed < 24) {
    uint32_t ts = demoW.nextAccUs;
    float wp, wr, pr, rr, heave;
    demoSeaMotion(0.009615f, wp, wr, pr, rr, heave);
    float pitch = wp + (demoW.rpm > 300.0f ? 1.4f : 0.0f);   // motoring trims the bow up
    float heel = demoW.heelMean + wr;
    if (demoW.slamG > 0) { heave += demoW.slamG; demoW.slamG -= 0.08f; if (demoW.slamG < 0) demoW.slamG = 0; }
    float pRad = pitch * PI / 180.0f, hRad = heel * PI / 180.0f;
    float ax = -sinf(pRad), ay = sinf(hRad) * cosf(pRad), az = cosf(hRad) * cosf(pRad) + heave;
    int vib = (int)constrain(demoW.rpm / 1.2f, 120.0f, 3000.0f);
    pushAccelSample((int16_t)constrain((int32_t)(ax / ACCEL_SCALE) + random(-vib, vib), -32767, 32767),
                    (int16_t)constrain((int32_t)(ay / ACCEL_SCALE) + random(-vib, vib), -32767, 32767),
                    (int16_t)constrain((int32_t)(az / ACCEL_SCALE) + random(-vib, vib), -32767, 32767), ts);
    demoW.nextAccUs += 9615;
    pushed++;
    if ((int32_t)(ts - demoW.nextGyrUs) >= 0) {
      float yawRate = 0.3f * rr + 0.4f * demoNoise(321);
      pushGyroSample((int16_t)((pr + random(-30, 30) / 100.0f) / GYRO_SCALE),
                     (int16_t)((rr + random(-30, 30) / 100.0f) / GYRO_SCALE),
                     (int16_t)((yawRate + random(-20, 20) / 100.0f) / GYRO_SCALE), ts);
      demoW.nextGyrUs += 19230;
    }
  }
  if (pushed) MARK_FRESH(IDX_IMU);
  // a rare slam: motoring at cruise into a short chop
  static uint32_t lastSlamCheck = 0;
  if (millis() - lastSlamCheck >= 1000) {
    lastSlamCheck = millis();
    if (!demoW.sail && demoW.rpm > 1500.0f && fabsf(demoW.twaRel) < 60.0f && random(0, 420) == 0) demoW.slamG = 0.45f + random(0, 25) / 100.0f;
  }
}

// ============================================================================================================
// Fake sensor read: replaces the real read path while hardwarePresent == 0 (called from loop(), core 1)
// ============================================================================================================
void demoReadSensors() {
  uint32_t now = millis();
  uint32_t nowUs = micros();
  if (demoW.lastStepUs == 0) demoW.lastStepUs = nowUs;
  uint32_t dUs = nowUs - demoW.lastStepUs;
  if (dUs >= 2000) {
    float dt = dUs * 1e-6f;
    if (dt > 0.1f) dt = 0.1f;
    demoW.lastStepUs = nowUs;
    float s = demoSec();
    demoNavStep(s, dt);
    demoPlantStep(s, dt);
  }
  // The INA228 still watches the real bus (its ALERT comparator, and the field driver's own inputs read just below): the same
  // field-gate-driven fast/slow ADC switch as _ReadAnalogInputs_inner, without the filter reseed
  if (INADisconnected == 0 && !demoInaGone) {
    bool fieldGateOpen = !gpio4IsLow;
    if (fieldGateOpen != inaFastModeActive && inaWriteAdcConfig(fieldGateOpen ? INA_ADC_CFG_FAST : INA_ADC_CFG_SLOW)) {
      inaFastModeActive = fieldGateOpen;
      inaReadInterval = fieldGateOpen ? INA_FAST_INTERVAL_MS : INA_SLOW_INTERVAL_MS;
    }
  }
  demoReadFieldDriveReal();   // 5_functions.ino: real bus, die and alternator temperature for the field driver's own protections
  // INA228 cadence: 5 ms with the field gate open, 1.1 s with it cut (the real chip's two modes)
  static uint32_t lastIna = 0;
  uint32_t inaIv = gpio4IsLow ? INA_SLOW_INTERVAL_MS : INA_FAST_INTERVAL_MS;
  if (now - lastIna >= inaIv) { lastIna = now; demoCommitIna(now); }
  // ADS1115 cadence: CH1 ~13 ms, CH0 / CH2 / CH3 ~40 ms
  static uint32_t lastCh1 = 0, lastCh0 = 0, lastCh2 = 0, lastCh3 = 0;
  if (now - lastCh1 >= 13) { lastCh1 = now; demoCommitAltAmps(now); }
  if (now - lastCh0 >= 40) {
    lastCh0 = now;
    BatteryV = demoW.vBatt + 0.02f + 0.004f * demoNoise(304);   // the second reading sits a little high, as on a real board
    Channel0V = BatteryV;
    MARK_FRESH(IDX_BATTERY_V);
    battVFreshFlag = true;
    g_battVStale = false;
    aggBattV.add(BatteryV);
  }
  if (now - lastCh2 >= 40 && now - lastCh0 >= 13) {
    lastCh2 = now;
    RPM = (demoW.rpm > 100.0f) ? demoW.rpm + 2.5f * demoNoise(305) : 0.0f;
    Channel2V = RPM;
    MARK_FRESH(IDX_RPM);
    rpmUnheldReadMs = (uint32_t)dataTimestamps[IDX_RPM];
    if (IgnoreRPM == 0 && !LimpHome) {
      if (RPM > RPMMax) RPMMax = RPM;
      if (RPM > RPMMax_AllTime) RPMMax_AllTime = RPM;
      wmIgnUpdate(wmIgn_RPM, RPM);
    }
    aggRpm.add(RPM);
  }
  if (now - lastCh3 >= 40 && now - lastCh0 >= 26) {
    lastCh3 = now;
    temperatureThermistor = (int)lroundf(demoW.altT + 3.0f + 0.4f * demoNoise(306));
    Channel3V = 1.65f;
    MARK_FRESH(IDX_THERMISTOR_TEMP);
    MARK_FRESH(IDX_CHANNEL3V);
    aggCh3.add(Channel3V);
    if (temperatureThermistor > MaxTemperatureThermistor) MaxTemperatureThermistor = temperatureThermistor;
    if (temperatureThermistor > MaxTemperatureThermistor_AllTime) MaxTemperatureThermistor_AllTime = temperatureThermistor;
  }
  // 1-Wire probes (in demo TempTask reads only the real alternator probe, for the field driver): alternator, battery, engine room
  static uint32_t lastOw = 0;
  if (now - lastOw >= 1500) {
    lastOw = now;
    AlternatorTemperatureF = demoProbeF(demoW.altT + 0.05f * demoNoise(307));
    MARK_FRESH(IDX_ALTERNATOR_TEMP);
    wmIgnUpdate(wmIgn_altTempF, AlternatorTemperatureF);
    if (AlternatorTemperatureF > MaxAlternatorTemperatureF) MaxAlternatorTemperatureF = AlternatorTemperatureF;
    if (AlternatorTemperatureF > MaxAlternatorTemperatureF_AllTime) MaxAlternatorTemperatureF_AllTime = AlternatorTemperatureF;
    BatteryTempProbeF = demoProbeF(demoW.battT + 0.05f * demoNoise(308));
    MARK_FRESH(IDX_BATT_TEMP_PROBE);
    wmIgnUpdate(wmIgn_battTempF, BatteryTempProbeF);
    ExtraTempF = demoProbeF(demoW.engRoomF + 0.05f * demoNoise(309));
    MARK_FRESH(IDX_EXTRA_TEMP);
    wmIgnUpdate(wmIgn_extraTempF, ExtraTempF);
  }
  // BMP388 (barometer + board) and the INA228 die, about 1 Hz
  static uint32_t lastEnv = 0;
  if (now - lastEnv >= 1000) {
    lastEnv = now;
    baroPressure = demoBaroAt(demoNowEpoch()) + 0.03f * demoNoise(310);
    MARK_FRESH(IDX_BARO_PRESSURE);
    ambientTemp = demoW.boardF + 0.05f * demoNoise(311);
    MARK_FRESH(IDX_AMBIENT_TEMP);
    inaDieTempF = demoW.boardF + 6.0f + 0.2f * demoNoise(312);
    MARK_FRESH(IDX_INA_DIE_TEMP);
  }
  // NMEA 2000 / 0183 navigation and wind, 5 Hz; position, satellites and depth 1 Hz
  static uint32_t lastNav = 0, lastGps = 0;
  if (now - lastNav >= 200) {
    lastNav = now;
    // each instrument's own jitter: compass, GPS (worse at low speed), paddlewheel (worse in a sea), masthead unit
    float s = demoSec();
    HeadingNMEA = demoWrap360(demoW.hdg + 0.3f * demoWander(s / 1.5f, 331));                                  MARK_FRESH(IDX_HEADING_NMEA);
    COGNMEA = demoWrap360(demoW.cog + (0.5f + 1.5f / fmaxf(demoW.sog, 0.5f)) * demoWander(s / 2.0f, 332));    MARK_FRESH(IDX_COG_NMEA);
    SOGNMEA = fmaxf(0.0f, demoW.sog + 0.06f * demoWander(s / 1.5f, 333));                                     MARK_FRESH(IDX_SOG_NMEA);
    STWNMEA = fmaxf(0.0f, demoW.stw + (0.03f + 0.05f * demoW.sea) * demoWander(s / 2.5f, 334));               MARK_FRESH(IDX_STW_NMEA);
    ApparentWindSpeedNMEA = fmaxf(0.0f, demoW.aws + 0.25f * demoWander(s / 0.8f, 335));                        MARK_FRESH(IDX_APPARENT_WIND_SPEED);
    ApparentWindAngleNMEA = demoWrap360(demoW.awa + 1.5f * demoWander(s / 0.8f, 336));                         MARK_FRESH(IDX_APPARENT_WIND_ANGLE);
    wmIgnUpdate(wmIgn_SOG, SOGNMEA);
    n183HeadingDeg = demoWrap360(demoW.hdg + 15.5f);   // magnetic, Penobscot Bay variation 15.5 W
    n183HdgRef = 1;
    MARK_FRESH(IDX_N183);
    MARK_FRESH(IDX_N183_HDG);
    n183SentenceCount++;
  }
  if (now - lastGps >= 1000) {
    lastGps = now;
    float s = demoSec();
    LatitudeNMEA = demoW.lat + 1.5e-5 * demoWander(s / 20.0f, 337);   // a couple of metres of wandering GPS error
    LongitudeNMEA = demoW.lon + 2.0e-5 * demoWander(s / 20.0f, 338);
    MARK_FRESH(IDX_LATITUDE_NMEA); MARK_FRESH(IDX_LONGITUDE_NMEA);
    SatelliteCountNMEA = constrain(11 + (int)lroundf(1.4f * demoWander(s / 600.0f, 339)), 8, 14);
    MARK_FRESH(IDX_SATELLITE_COUNT);
    updateSustainedSpeed(SOGNMEA);
    WaterDepth_m = demoDepthM(demoW.lat, demoW.lon) + 0.08f * demoNoise(340);
    MARK_FRESH(IDX_WATER_DEPTH);
  }
  // Victron MPPT on VE.Direct and a battery monitor SoC, every 2 s like the real port
  static uint32_t lastVe = 0;
  if (now - lastVe >= 2000) {
    float dtS = (lastVe == 0) ? 2.0f : (now - lastVe) / 1000.0f;
    lastVe = now;
    float p = demoW.solarW;
    VictronSolarPower_W = p;
    VictronSolarVoltage_V = 36.6f - 1.8f * (1.0f - fminf(1.0f, demoCloud(demoSec()))) + 0.15f * demoWander(demoSec() / 40.0f, 341);
    VictronSolarCurrent_A = p / VictronSolarVoltage_V;
    VictronChargeState = (chargeStageDisplay == 2) ? 4 : (chargeStageDisplay == 3) ? 5 : 3;
    VictronMPPTMode = 2; VictronError = 0;
    float wh = p * dtS / 3600.0f;
    VictronYieldToday_kWh += wh / 1000.0f;
    if (p > VictronMaxPowerToday_W) VictronMaxPowerToday_W = p;
    if (p > solar_power_max_alltime_w) solar_power_max_alltime_w = p;
    MARK_FRESH(IDX_VICTRON_SOLAR);
    static float acc = 0, accAt = 0;
    acc += wh; accAt += wh;
    if (acc >= 1.0f) { SolarChargedEnergy += (int)acc; acc -= (int)acc; }
    if (accAt >= 1.0f) { SolarChargedEnergy_AllTime += (int)accAt; accAt -= (int)accAt; }
    VictronSoc = (float)SOC_percent / 100.0f;
    MARK_FRESH(IDX_VE_SOC);
  }
  demoImuStep();
}

// Boat-performance and alternator-health simulators read the same boat (perfSimTick / altSimTick, 7_functions.ino)
void demoPerfSimFeed(float &aws, float &awa, float &stw, float &sog, float &rpm, float &pitch) {
  aws = ApparentWindSpeedNMEA; awa = ApparentWindAngleNMEA; stw = STWNMEA; sog = SOGNMEA; rpm = RPM; pitch = imu_pitch_deg;
}
// The sailing view while the engine runs (perfFold_tick): the boat under sail on its present heading in the live true
// wind, or on the script's close reach when the heading is too close to the wind to sail
void demoSailShadow(float &aws, float &awa, float &spd) {
  float twa = demoW.twaRel;
  if (fabsf(twa) < 45.0f) twa = (twa < 0) ? -55.0f : 55.0f;
  spd = demoSailStw(demoW.tws, twa, demoW.sea) * demoSailEff(demoSec());
  float tr = twa * PI / 180.0f, ax = demoW.tws * sinf(tr), ay = demoW.tws * cosf(tr) + spd;
  aws = sqrtf(ax * ax + ay * ay);
  awa = demoWrap360(atan2f(ax, ay) * 180.0f / PI);
}
// The motoring view while the engine is stopped: cruise revs on the present heading in the live wind
void demoMotorShadow(float &rpm, float &headwind, float &spd) {
  float s = demoSec();
  rpm = DEMO_CRUISE_RPM + 22.0f * demoWander(s / 40.0f, 171);
  float hwTrue = demoW.tws * cosf(demoW.twaRel * PI / 180.0f);
  spd = demoMotorStw(rpm, hwTrue, demoW.sea);
  spd = demoMotorStw(rpm, hwTrue + spd, demoW.sea) * demoMotorEff(s);
  headwind = hwTrue + spd;
}
// The demo's "now" dot, never outside the best-ever curve the screens draw (Mark, 2026-10-31: a real boat can beat its
// record, the demo must not, it looks wrong). The screens draw the record's local fit (FrontQuery's prediction) along the
// live point's angle or RPM, at its wind (or headwind) and sea state; the boat sails on the model the record was seeded
// from, which the fit can sit a few percent under. Each second, step 1 takes the two views' points (sent as taken, at
// steps 2 and 3) and a lookup on the demo record for each (DemoDot, run by PerfEng::step within FRONT_STEP_US) gives the
// curve at the point: the speed sent is held under DEMO_DOT_CAP of it and the % is against it. The GX draws a curve swept
// up to 15 s before, and holds the dot inside that one itself (XregChartPerf, dotSpd).
#define DEMO_DOT_CAP 0.99f              // the dot at most this share of the curve: the screens' straight lines between swept points
struct DemoDot {
  bool  sValid, mValid;                 // the views as taken at step 1, sent at steps 2 and 3
  uint8_t sSrc, mSrc;
  float sWs, sWa, sSpd, sSea, mRpm, mHw, mSpd, mSea;
  float x[2][PERF_NAXIS], y[2];         // lookups: the sailing point, the motoring point
  bool  use[2];
  int   at;                             // the lookup running; 2 = both done
  bool  begun;
  // (Members, not free functions: Arduino's prototype generator would declare those ahead of this struct.)
  void points() {
    const float pts[2][PERF_NAXIS] = { { sWs, perfFoldAwa(sWa), sSea }, { mRpm, mHw, mSea } };
    for (int k = 0; k < 2; k++) {
      for (int a = 0; a < PERF_NAXIS; a++) x[k][a] = pts[k][a];
      y[k] = 0;
    }
    use[0] = sValid; use[1] = mValid;
  }
  // The speeds sent, held under DEMO_DOT_CAP of the curve, and the % against it
  void grade(float &sPct, float &mPct) {
    if (y[0] > 0.1f) sSpd = fminf(sSpd, DEMO_DOT_CAP * y[0]);
    if (y[1] > 0.1f) mSpd = fminf(mSpd, DEMO_DOT_CAP * y[1]);
    sPct = (y[0] > 0.1f) ? 100.0f * sSpd / y[0] : 0.0f;
    mPct = (y[1] > 0.1f) ? 100.0f * mSpd / y[1] : 0.0f;
  }
};
static DemoDot *demoDot = nullptr;                    // PSRAM, with the demo stores
static FrontQuery<PERF_NAXIS> *demoDotQ = nullptr;    // PSRAM
// Step 1 in demo: take both views and queue their lookups (none: graded at once)
void demoPerfGradeStart() {
  if (!demoDot || !demoDotQ) { perfLiveReady = true; return; }
  DemoDot &D = *demoDot;
  D.sValid = perfLiveValid; D.sSrc = perfLiveSrc; D.sWs = perfLive_ws; D.sWa = perfLive_wa; D.sSpd = perfLive_spd; D.sSea = perfLive_pitch;
  D.mValid = motorLiveValid; D.mSrc = motorLiveSrc; D.mRpm = motorLive_rpm; D.mHw = motorLive_hw; D.mSpd = motorLive_spd; D.mSea = motorLive_pitch;
  D.points();
  D.at = 0; D.begun = false;
}
// PerfEng::step in demo, until untilUs: the lookups in turn; true once both are done and the views graded
bool demoPerfGradeStep(int64_t untilUs) {
  if (!demoDot || !demoDotQ) return true;
  DemoDot &D = *demoDot;
  while (D.at < 2) {
    if (!D.use[D.at]) { D.at++; continue; }
    if (!D.begun) {
      demoDotQ->begin(D.at == 0 ? sailFront : motorFront, D.x[D.at], perfIdwPower, perfRidgeFrac, perfRefRadius, perfRiskThresh, frontGenNow());
      D.begun = true;
    }
    if (!demoDotQ->step(untilUs)) return false;
    // the screens draw the local fit, or the inverse-distance average where it is singular
    if (demoDotQ->phase == FrontQuery<PERF_NAXIS>::DONE) D.y[D.at] = demoDotQ->lwlrOk ? demoDotQ->lp : demoDotQ->idw;
    demoDotQ->phase = FrontQuery<PERF_NAXIS>::IDLE;
    D.at++; D.begun = false;
    if (D.at < 2 && esp_timer_get_time() >= untilUs) return false;
  }
  float sPct, mPct;
  D.grade(sPct, mPct);
  if (D.sValid) { perfLive_best = D.y[0]; perfLive_pct = sPct; perfState = FRONT_MEASURED; }
  if (D.mValid) { motorLive_best = D.y[1]; motorLive_pct = mPct; motorState = FRONT_MEASURED; }
  return true;
}
// Steps 2 (sailing) and 3 (motoring) in demo, just before the send: the view as taken at step 1, its speed held
void demoPerfPut(bool motor) {
  if (!demoDot) return;
  const DemoDot &D = *demoDot;
  if (motor) { motorLiveValid = D.mValid; motorLiveSrc = D.mSrc; motorLive_rpm = D.mRpm; motorLive_hw = D.mHw; motorLive_spd = D.mSpd; motorLive_pitch = D.mSea; }
  else       { perfLiveValid = D.sValid; perfLiveSrc = D.sSrc; perfLive_ws = D.sWs; perfLive_wa = D.sWa; perfLive_spd = D.sSpd; perfLive_pitch = D.sSea; }
}
// The live alternator grade in demo (altHealth_tick, altProcessEmits): output against the simulated alternator's own best,
// the curve the demo record samples. Outside 97.6-99.4 % (what reads 98-99 %) is a transient between steady points, such as
// the field or the revs coming up: ungraded, the served % holds.
// About one 5 s session slot in eight reads ESTIMATED, as interpolated cells do on a real record.
uint8_t demoAltGrade(const float *x, float amps, uint32_t nowMs, float *pred) {   // x = the alt front's {rpm, exc, vbus, tempF}
  float best = demoAltAmps(x[0], x[1], x[3], 1.0f);
  *pred = 0;
  if (best < 5.0f) return FRONT_NO_REFERENCE;
  float pct = amps / best * 100.0f;
  if (!(pct > 97.6f && pct < 99.4f)) return FRONT_NO_REFERENCE;
  *pred = best;
  return (demoHash01(nowMs / ALT_SESS_DT_MS) < 0.12f) ? FRONT_ESTIMATED : FRONT_MEASURED;
}

// The made-up GX devices that live in the plant, for CSV4 (demoShoreA, demoFridgeA, demoOrionA, demoInverterA): the shore
// charger's output and the fridge's, the Orion XS's and the inverter's draws, A; NAN outside demo. The GX bridge's FakeGx
// shows them rather than making its own, so the battery it shows and the simulated battery are one sum.
void demoGxDeviceAmps(float &shoreA, float &fridgeA, float &orionA, float &inverterA) {
  const bool on = (demoPhase == DEMO_ON);
  shoreA = on ? demoW.shoreA : NAN;
  fridgeA = on ? demoW.fridgeA : NAN;
  orionA = on ? demoW.orionA : NAN;
  inverterA = on ? demoW.inverterA : NAN;
}

// ============================================================================================================
// Made-up DVCC: the GX's "Limit charge current" with no managed battery, shared like Victron's dvcc.py
// (alternator share = battery limit - solar current), posted into the same mailbox /gxLimits fills
// ============================================================================================================
static void demoDvccTick() {
  static uint32_t lastPost = 0;
  uint32_t now = millis();
  if (now - lastPost < 3000) return;
  lastPost = now;
  float ccl = demoW.battLimitA - demoW.solarBattA;
  if (ccl < 0) ccl = 0;
  portENTER_CRITICAL(&dvccWifiMux);
  dvccWifiBoxCvl = NAN;
  dvccWifiBoxCcl = roundf(ccl * 10.0f) / 10.0f;
  dvccWifiBoxSeq = dvccWifiBoxSeq + 1;
  portEXIT_CRITICAL(&dvccWifiMux);
}
static float demoTableCap(float rpm) {
  if (rpm <= rpmTableRPMPoints[0]) return rpmCapCurrentTable[0];
  for (int i = 1; i < RPM_TABLE_SIZE; i++) {
    if (rpm <= rpmTableRPMPoints[i]) {
      float span = (float)(rpmTableRPMPoints[i] - rpmTableRPMPoints[i - 1]);
      float f = (span > 0) ? (rpm - rpmTableRPMPoints[i - 1]) / span : 0;
      return rpmCapCurrentTable[i - 1] + f * (rpmCapCurrentTable[i] - rpmCapCurrentTable[i - 1]);
    }
  }
  return rpmCapCurrentTable[RPM_TABLE_SIZE - 1];
}

// ============================================================================================================
// Rolling backfill (GET /demoBackfill): 1 s samples for 300 s and 10 s wind for 1 h. Entry fills both rings with the
// made-up past (demoMakePast); the live run then records over them, so the served window always holds exactly N rows.
// ============================================================================================================
#define DEMO_BF_SHORT_N 300
#define DEMO_BF_WIND_N 360
struct DemoBfShort { float bcur, alt, fieldA, duty, altFilt, battV, ibv, rpm, altT; uint8_t stage, prot; };
struct DemoBfWind { float tws, twa, aws, awa; };
static DemoBfShort *demoBfS = nullptr;
static DemoBfWind *demoBfW = nullptr;
static uint32_t demoBfSHead = 0, demoBfSCount = 0, demoBfWHead = 0, demoBfWCount = 0;
static uint32_t demoBfSLastEpoch = 0, demoBfWLastEpoch = 0;
// The hour before entry, on the mooring with the engine off and the sea breeze in: stepped second by second through the
// same wind, sun and load processes the live run continues, ending in the state the live boat starts from. The entry
// second itself is the made-up past's, so the first live row cannot carry a reading from before the switch.
static void demoMakePast() {
  float C = demoCapAh(), k = demoClassK(), r0 = 0.55f / C * k, r1 = 0.40f / C * k;
  float ocv = demoOcv12(demoW.startSoc / 100.0f) * k;
  uint32_t e0 = demoEpoch0, firstRow = (e0 / 10) * 10 - (DEMO_BF_WIND_N - 1) * 10;
  for (int c = 0; c < 2; c++)
    for (int i = 0; i < 9; i++) demoW.wavePh[c][i] = 2.0f * PI * demoHash01(i * 977u + c * 131u + 31u);
  demoW.windK = (int32_t)DEMO_ORIGIN_S;
  demoW.wGust = demoW.wTurb = demoW.wDir = 0;
  demoBfSHead = demoBfSCount = demoBfWHead = demoBfWCount = 0;
  float solarA = 0, vPol = 0, battA = 0, load = 0, v = ocv, tws = 0, twd = DEMO_TWD, lie = 30.0f;
  for (int32_t sec = (int32_t)DEMO_ORIGIN_S; sec <= 0; sec++) {
    float s = (float)sec;
    uint32_t ep = e0 + (uint32_t)sec, u = (uint32_t)sec;
    demoWindTo(sec);
    demoTrueWind(s, tws, twd);
    demoW.sea = demoSeaFromTws(demoWindMean(s));
    lie = demoWrap180(twd - demoMooredHdg(s));
    if (ep >= firstRow && ep % 10 == 0 && demoBfWCount < DEMO_BF_WIND_N) {
      DemoBfWind &w = demoBfW[demoBfWCount++];
      w.tws = tws; w.twa = demoWrap360(lie);
      w.aws = fmaxf(0.0f, tws + 0.25f * demoWander(s / 0.8f, 335)); w.awa = demoWrap360(lie + 1.5f * demoWander(s / 0.8f, 336));
    }
    load = demoLoadA(s);
    solarA += (demoSolarW(s) * 0.97f / fmaxf(v, 10.0f) - solarA) * 0.393f;   // the MPPT's 2 s lag at 1 s steps
    battA = solarA - load;
    vPol += (battA * r1 - vPol) * 0.0083f;                                  // 120 s
    v = ocv + battA * r0 + vPol;
    if (sec > -DEMO_BF_SHORT_N) {
      float roomF = demoAirF(s) + 4.0f, alt = 0.12f * demoGauss(u, 351);
      DemoBfShort &r = demoBfS[demoBfSCount++];
      r.bcur = battA + 0.02f * demoGauss(u, 352); r.alt = alt; r.fieldA = 0; r.duty = 0; r.altFilt = 0.3f * alt;
      r.battV = v + 0.02f + 0.004f * demoGauss(u, 353); r.ibv = v + 0.0015f * demoGauss(u, 354); r.rpm = 0;
      r.altT = demoProbeF(roomF + 0.5f + 0.05f * demoGauss(u, 355)); r.stage = 0; r.prot = 0;
    }
  }
  demoBfSHead = demoBfSCount % DEMO_BF_SHORT_N; demoBfWHead = demoBfWCount % DEMO_BF_WIND_N;
  demoBfSLastEpoch = demoBfWLastEpoch = e0;
  // the live boat starts from the made-up past's last second
  demoW.tws = tws; demoW.twd = twd; demoW.hdg = demoMooredHdg(0); demoW.cog = demoW.hdg;
  demoW.solarBattA = solarA; demoW.loadA = load; demoW.battA = battA; demoW.vPol = vPol; demoW.vBatt = v;
  demoW.engRoomF = demoAirF(0) + 4.0f; demoW.altT = demoW.engRoomF + 0.5f; demoW.boardF = demoW.engRoomF - 3.0f;
  demoW.battT = demoAirF(0) + 2.0f;
}
static void demoBackfillRecord() {
  if (!demoBfS || !demoBfW) return;
  uint32_t ep = demoNowEpoch();
  if (ep != demoBfSLastEpoch) {
    demoBfSLastEpoch = ep;
    DemoBfShort &r = demoBfS[demoBfSHead];
    r.bcur = Bcur; r.alt = MeasuredAmps; r.fieldA = iiout; r.duty = dutyCycle; r.altFilt = g_pidI_filtered;
    r.battV = BatteryV; r.ibv = IBV; r.rpm = RPM; r.altT = AlternatorTemperatureF;
    r.stage = chargeStageDisplay; r.prot = 0;
    demoBfSHead = (demoBfSHead + 1) % DEMO_BF_SHORT_N;
    if (demoBfSCount < DEMO_BF_SHORT_N) demoBfSCount++;
  }
  if (ep / 10 != demoBfWLastEpoch / 10) {
    demoBfWLastEpoch = ep;
    DemoBfWind &w = demoBfW[demoBfWHead];
    w.tws = demoW.tws; w.twa = demoWrap360(demoW.twaRel);   // the boat's true wind, as the derived instrument value reads it
    w.aws = ApparentWindSpeedNMEA; w.awa = ApparentWindAngleNMEA;
    demoBfWHead = (demoBfWHead + 1) % DEMO_BF_WIND_N;
    if (demoBfWCount < DEMO_BF_WIND_N) demoBfWCount++;
  }
}
static String demoBackfillJson() {
  String out;
  out.reserve(30000);
  uint32_t now = demoNowEpoch();
  out += "{\"v\":1,\"now\":"; out += now;
  out += ",\"short\":{\"dt\":1,\"t0\":"; out += (now - (DEMO_BF_SHORT_N - 1));
  out += ",\"cols\":[\"Bcur\",\"MeasuredAmps\",\"iiout\",\"dutyCycle\",\"pidAltPV\",\"BatteryV\",\"IBV\",\"RPM\",\"AlternatorTemperatureF\",\"chargeStage\",\"protEventMask\"],\"rows\":[";
  char b[160];
  for (uint32_t i = 0; i < demoBfSCount; i++) {
    const DemoBfShort &r = demoBfS[(demoBfSHead + DEMO_BF_SHORT_N - demoBfSCount + i) % DEMO_BF_SHORT_N];
    snprintf(b, sizeof(b), "%s[%.2f,%.2f,%.2f,%.2f,%.2f,%.3f,%.3f,%.0f,%.1f,%u,%u]", i ? "," : "", r.bcur, r.alt, r.fieldA, r.duty,
             r.altFilt, r.battV, r.ibv, r.rpm, r.altT, (unsigned)r.stage, (unsigned)r.prot);
    out += b;
  }
  out += "]},\"wind\":{\"dt\":10,\"t0\":"; out += (now - (demoBfWCount - 1) * 10);
  out += ",\"cols\":[\"TrueWindSpeedNMEA\",\"TrueWindAngleNMEA\",\"ApparentWindSpeedNMEA\",\"ApparentWindAngleNMEA\"],\"rows\":[";
  for (uint32_t i = 0; i < demoBfWCount; i++) {
    const DemoBfWind &w = demoBfW[(demoBfWHead + DEMO_BF_WIND_N - demoBfWCount + i) % DEMO_BF_WIND_N];
    snprintf(b, sizeof(b), "%s[%.2f,%.1f,%.2f,%.1f]", i ? "," : "", w.tws, w.twa, w.aws, w.awa);
    out += b;
  }
  out += "]}}";
  return out;
}

// ============================================================================================================
// Made-up history: separate stores swapped in at entry, swapped back at exit (the real ones are never touched)
// ============================================================================================================
#define DEMO_ALT_CAP 1024
#define DEMO_PERF_CAP 1024
#define DEMO_LT_LIVE_N 288                // 48 h of live demo records at the 10 min cadence
static FrontPoint<ALT_NAXIS> *demoAltBuf = nullptr;
static FrontPoint<PERF_NAXIS> *demoSailBuf = nullptr, *demoMotorBuf = nullptr;
static AltTrendPt *demoTrendBuf = nullptr;
static uint16_t *demoBaroBuf = nullptr;
static LongTermRecord *demoLtLive = nullptr;
static uint16_t demoLtLiveHead = 0, demoLtLiveCount = 0;
static OvTelemetry *demoOvTelBuf = nullptr;
static BatteryWear *demoBatWearBuf = nullptr;
static AltSessPt *demoAltSessBak = nullptr;
static uint32_t demoLtBgN = 0;            // background records generated on request, ending at entry

// Every storage-namespace scalar saveNVSDataFull() keeps (5_functions.ino), plus the averages derived from them: copied at
// entry, put back at exit before the reload. loadNVSData() leaves a value alone when its key was never written, so without
// this a demo seed or a demo-grown maximum would stay in RAM and be saved as the boat's own. Left out on purpose: the
// regulator's own health counters (session length, worst loop, minimum heap, power cycles) and app-usage totals, which keep
// counting real time in demo. The field-drive limit seconds are in the list: in demo their ceilings are solved on simulated readings.
#define DEMO_LIFETIME_LIST(X) \
  X(ChargedEnergy) X(DischargedEnergy) X(AlternatorChargedEnergy) X(SolarChargedEnergy) X(AlternatorFuelUsed) X(EngineFuelUsed) \
  X(ChargedEnergy_AllTime) X(DischargedEnergy_AllTime) X(AlternatorChargedEnergy_AllTime) X(SolarChargedEnergy_AllTime) \
  X(AlternatorFuelUsed_AllTime) X(EngineFuelUsed_AllTime) X(EngineRunTime) X(EngineCycles) X(AlternatorOnTime) \
  X(EngineRunTime_AllTime) X(perfSailSeconds) X(perfMotorSeconds) X(EngineCycles_AllTime) X(AlternatorOnTime_AllTime) \
  X(ChargeCycleEnergy) X(ChargeCycles_AllTime) X(ChargeCycles) X(TotalDistance) X(AvgSpeed) X(TotalDistance_AllTime) \
  X(AvgSpeed_AllTime) X(speedAccumulator_AllTime) X(totalSpeedSampleTime_AllTime) X(AvgSOC) X(AvgSOC_AllTime) \
  X(socAccumulator) X(totalSocSampleTime) X(speedAccumulator) X(totalSpeedSampleTime) \
  X(socAccumulator_AllTime) X(totalSocSampleTime_AllTime) X(voltageAccumulator_AllTime) X(totalVoltageSampleTime_AllTime) \
  X(AvgVoltage_AllTime) X(timeSinceLastOverheat) X(SOC_percent) X(CoulombCount_Ah_scaled) X(CumulativeInsulationDamage) \
  X(CumulativeGreaseDamage) X(CumulativeBrushDamage) X(zfValid) X(zfSensor) X(zfC) X(zfB) X(zfR2) X(zfLastEpoch) \
  X(sailing_days_alltime) X(sailing_dist_alltime) X(alt_power_max_alltime_w) X(solar_power_max_alltime_w) \
  X(imu_capsize_count) X(imu_pitchpole_count) X(imu_slam_count_lifetime) X(faAnomalyCount) X(imu_min_moving_gentle) \
  X(imu_min_moving_moderate) X(imu_min_moving_rough) X(imu_min_moving_extreme) X(imu_min_stat_gentle) X(imu_min_stat_moderate) \
  X(imu_min_stat_rough) X(imu_min_stat_extreme) X(imu_heel_max_lifetime) X(imu_pitch_max_lifetime) X(imu_slam_peak_lifetime) \
  X(MaxSpeed) X(MaxSpeed_AllTime) X(LongestSingleTrip_Nm_AllTime) X(currentTripDistanceNm) X(currentTripLastUpdateEpoch) \
  X(Max24hrDistance_AllTime) X(DeepestAnchorage_Ft_AllTime) X(best_upwind_vmg_alltime) X(longest_gale_duration_hours_alltime) \
  X(faSesPkpkWorstA) X(faSesPeakWorstA) X(faSesPeakWorstHz) X(faDomAmpAX100) X(faDomFreqHzX10) X(faDomRpm) X(faDomAmpsA) \
  X(faDomTempF) X(faDomEpoch) X(faSesPkpkAmpsA) X(faSesPkpkTempF) X(faSesPkpkRpm) X(faSesPkpkEpoch) X(MeasuredAmpsMax) \
  X(MeasuredAmpsMax_AllTime) X(RPMMax) X(RPMMax_AllTime) X(IBVMax) X(PeakVoltage_AllTime) X(MinVoltage) X(MinVoltage_AllTime) \
  X(board_temp_max_alltime) X(board_temp_min_alltime) X(baro_pressure_max_alltime) X(baro_pressure_min_alltime) \
  X(MaxTemperatureThermistor) X(MaxTemperatureThermistor_AllTime) X(MaxAlternatorTemperatureF) \
  X(MaxAlternatorTemperatureF_AllTime) X(max_wind_speed_apparent_alltime) X(max_wind_speed_true_alltime) X(UVToday) \
  X(UVTomorrow) X(UVDay2) X(g_fieldHeatLimS) X(g_fieldCurLimS) X(g_fieldHeatLimSesS) X(g_fieldCurLimSesS)
struct DemoLifetimeBak {
#define DEMO_LT_FIELD(v) std::remove_volatile<decltype(::v)>::type v;
  DEMO_LIFETIME_LIST(DEMO_LT_FIELD)
#undef DEMO_LT_FIELD
};

struct DemoBackup {
  // alternator health
  FrontStore<ALT_NAXIS> altFront2; FrontPoint<ALT_NAXIS> *altFrontBuf; bool altHaveUpload; uint8_t altRefSource;
  AltTrendPt *altTrend; int altTrendCount; uint32_t altTrendFlushed; bool altTrendRewrite;
  double altBucket_sum, altBucket_n; float altBucket_worst; uint16_t altBucketHist[60]; int altCurEngHour;
  double altTrendBaselineSec; float altLatchPct, altWorstPctLive, altOverallPctLive;
  uint32_t altSessRingHead, altSessRingCount, altSessRingLastMs;
  // boat performance
  FrontStore<PERF_NAXIS> sailFront, motorFront; FrontPoint<PERF_NAXIS> *sailFrontBuf, *motorFrontBuf;
  // barometer ring
  uint16_t *baroPressureHistory; uint16_t baroHistoryHead; time_t baroHistoryLastEpoch; unsigned long lastBaroSampleMs;
  // RAM-only state the demo overwrites
  IgnWatermark wm[18];
  int overheatCount[RPM_TABLE_SIZE]; uint64_t cumulativeNoOverheatTime[RPM_TABLE_SIZE]; unsigned long lastOverheatTime[RPM_TABLE_SIZE];
  unsigned long totalOverheats; uint64_t totalSafeMs;
  float fuelCurveNMPG[FUELCURVE_BINS];
  float distHourBuckets[24]; uint8_t distHourHead; uint32_t distHourStartMs;
  float DynamicAltCurrentZero, VictronYieldYesterday_kWh, VictronMaxPowerYesterday_W, VictronYieldToday_kWh, VictronMaxPowerToday_W;
  uint32_t prot[20];
  bool imuEnabled;
  uint32_t n183SentenceCount;
  int dvccEn, dvccSrcType, dvccLostStop; uint8_t dvccState, dvccUntrustReason;
  uint8_t commissionState, commissionPhase; uint16_t commissionDoneMask;
  int IgnoreRPM, IgnoreTemperature;
  int BatteryCapacity_Ah, BatteryShuntPresent, ShuntResistanceMicroOhm, battTempSource, battTempProbeEnable, extraTempProbeEnable;
  int BattMonSrc; bool BattLimitEnable; float BattCurrentLimitA;
  // session totals put back at exit (the live excursion stopwatches and steady-run state restart instead)
  AccLoopV4 accCur4, accVolt4; uint32_t accThermSessions; double accThermBindingSec, accThermInbandSec; float accThermWorstOverF;
  uint16_t altSessHist[60]; uint32_t altSessN; double altSessSum;
  DemoLifetimeBak life;
};
static DemoBackup *demoBak = nullptr;
// The owner's settings under the ones demoEnter forces, for the field driver's real inputs (fdAltTempF, demoReadFieldDriveReal)
bool demoRealIgnoreTemperature() { return demoBak && demoBak->IgnoreTemperature != 0; }
bool demoRealShunt() { return demoBak && demoBak->BatteryShuntPresent && demoBak->ShuntResistanceMicroOhm > 0; }

static uint32_t *const DEMO_PROT[20] = {
  &g_fastOvClampCount, &g_fastOvHardCount, &g_iExcessCount, &g_cvKdCount, &g_inaOVCount, &g_hardOCCount, &g_voltSpikeCount,
  &g_ovTierLowCount, &g_ovTierMidCount, &g_voltDisagreeCritCount, &g_voltDisagreeWarnCount, &g_voltImplausibleCount,
  &g_currentStaleCount, &g_voltStaleCount, &g_volt2StaleCount, &g_battTempLostCount, &g_dieTempLostCount,
  &g_tempCritCount, &g_tempSustainedCount, &g_tempStaleCount
};

static bool demoAllocStores() {
  if (!demoOv) demoOv = (DemoOvEntry *)ps_malloc(sizeof(DemoOvEntry) * DEMO_OV_CAP);
  if (!demoBak) demoBak = (DemoBackup *)ps_malloc(sizeof(DemoBackup));
  if (!demoAltBuf) demoAltBuf = (FrontPoint<ALT_NAXIS> *)ps_malloc(sizeof(FrontPoint<ALT_NAXIS>) * DEMO_ALT_CAP);
  if (!demoSailBuf) demoSailBuf = (FrontPoint<PERF_NAXIS> *)ps_malloc(sizeof(FrontPoint<PERF_NAXIS>) * DEMO_PERF_CAP);
  if (!demoMotorBuf) demoMotorBuf = (FrontPoint<PERF_NAXIS> *)ps_malloc(sizeof(FrontPoint<PERF_NAXIS>) * DEMO_PERF_CAP);
  if (!demoTrendBuf) demoTrendBuf = (AltTrendPt *)ps_malloc(sizeof(AltTrendPt) * ALT_TREND_CAP);
  if (!demoBaroBuf) demoBaroBuf = (uint16_t *)ps_malloc(sizeof(uint16_t) * BARO_HISTORY_SIZE);
  if (!demoLtLive) demoLtLive = (LongTermRecord *)ps_malloc(sizeof(LongTermRecord) * DEMO_LT_LIVE_N);
  if (!demoOvTelBuf) demoOvTelBuf = (OvTelemetry *)ps_malloc(sizeof(OvTelemetry));
  if (!demoBatWearBuf) demoBatWearBuf = (BatteryWear *)ps_malloc(sizeof(BatteryWear));
  if (!demoAltSessBak) demoAltSessBak = (AltSessPt *)ps_malloc(sizeof(AltSessPt) * ALT_SESS_RING_N);
  if (!demoBfS) demoBfS = (DemoBfShort *)ps_malloc(sizeof(DemoBfShort) * DEMO_BF_SHORT_N);
  if (!demoBfW) demoBfW = (DemoBfWind *)ps_malloc(sizeof(DemoBfWind) * DEMO_BF_WIND_N);
  if (!demoDot) demoDot = (DemoDot *)ps_calloc(1, sizeof(DemoDot));
  if (!demoDotQ) demoDotQ = (FrontQuery<PERF_NAXIS> *)ps_calloc(1, sizeof(FrontQuery<PERF_NAXIS>));
  return demoOv && demoBak && demoAltBuf && demoSailBuf && demoMotorBuf && demoTrendBuf && demoBaroBuf && demoLtLive
         && demoOvTelBuf && demoBatWearBuf && demoAltSessBak && demoBfS && demoBfW && demoDot && demoDotQ;
}
// Freed only at exit, after every pointer swapped back, so an in-flight response never reads freed demo memory.
static void demoFreeStores() {
  free(demoAltBuf); demoAltBuf = nullptr;
  free(demoSailBuf); demoSailBuf = nullptr;
  free(demoMotorBuf); demoMotorBuf = nullptr;
  free(demoTrendBuf); demoTrendBuf = nullptr;
  free(demoAltSessBak); demoAltSessBak = nullptr;
  free(demoBfS); demoBfS = nullptr;
  free(demoBfW); demoBfW = nullptr;
  free(demoDot); demoDot = nullptr;
  free(demoDotQ); demoDotQ = nullptr;
  free(demoBaroBuf); demoBaroBuf = nullptr;
  free(demoLtLive); demoLtLive = nullptr; demoLtLiveCount = 0;
}

// The demo alternator's record book: its own output function at health 1.0, on a lattice over the envelope it runs in
static void demoSeedAltFront() {
  altFront2.pts = demoAltBuf; altFront2.cap = DEMO_ALT_CAP; altFront2.count = 0; altFront2.source = 0;
  float k = demoClassK();
  altFront2.axisScale[0] = 75.0f; altFront2.axisScale[1] = 0.30f; altFront2.axisScale[2] = 0.30f * k; altFront2.axisScale[3] = 30.0f;
  altFrontBuf = demoAltBuf;
  static const float RPM[7] = { 720, 830, 940, 1800, 1910, 2020, 2130 };
  static const float TF[2] = { 115, 155 };
  uint32_t t = demoNowEpoch();
  for (int r = 0; r < 7; r++)
    for (int e = 0; e < 18; e++)
      for (int v = 0; v < 3; v++)
        for (int q = 0; q < 2; q++) {
          FrontPoint<ALT_NAXIS> p;
          p.x[0] = RPM[r]; p.x[1] = 3.0f + 0.45f * e; p.x[2] = (13.3f + 0.45f * v) * k; p.x[3] = TF[q];
          p.y = demoAltAmps(p.x[0], p.x[1], p.x[3], 1.0f);
          if (p.y < altMinAmps) continue;
          p.ex[0] = p.x[1] * 100.0f / p.x[2]; p.ex[1] = 0; p.nSamp = 20; p.tEmit = t - (uint32_t)(demoHash01(r * 977 + e * 31 + v * 7 + q) * 200.0f * 86400.0f);
          altFront2.add(p);
        }
}
// Health vs engine hours: 336 engine-hours of slow, normal wear, ending where the live grade reads; the open hour already
// holds half an hour of graded runs, and the health chip starts on the last session's grade
static void demoSeedAltTrend() {
  altTrend = demoTrendBuf; altTrendCount = 0; altTrendFlushed = 0; altTrendRewrite = false;
  float ovN = 0, wN = 0;
  for (int h = 0; h < 336; h++) {
    ovN = demoOuStep(ovN, 6.0f, 0.25f, (uint32_t)h, 411);   // warm and cool days, belt tension: hour-to-hour drift
    wN = demoOuStep(wN, 3.0f, 0.25f, (uint32_t)h, 412);
    if (demoHash01(h * 7919u) < 0.14f) continue;           // hours with no steady run
    float overall = 99.4f - 0.8f * h / 336.0f + ovN;
    float worst = overall - 0.8f + wN - 0.3f * demoHash01(h * 15485863u);
    AltTrendPt &p = altTrend[altTrendCount++];
    p.engHour = (uint16_t)h; p.overallPct = (int16_t)lroundf(overall * 10.0f); p.worstPct = (int16_t)lroundf(worst * 10.0f);
  }
  altBucket_sum = 0; altBucket_n = 0; altBucket_worst = 1.0f; memset(altBucketHist, 0, sizeof(altBucketHist));
  for (int i = 0; i < 40; i++) {
    float f = constrain(98.5f + 0.35f * demoGauss((uint32_t)i, 413), 97.7f, 99.3f) / 100.0f;
    altBucket_sum += f; altBucket_n += 1;
    if (f < altBucket_worst) altBucket_worst = f;
    altBucketHist[(int)(f * 100.0f * 0.5f)]++;
  }
  altTrendBaselineSec = EngineRunTime_AllTime - 336.0 * 3600.0 - 1800.0;
  altCurEngHour = 336;
  altOverallPctLive = (float)(altBucket_sum / altBucket_n) * 100.0f;
  altWorstPctLive = altBucketP10();
  altLatchPct = 98.6f;
}
// This Session: the 30 minutes the plot shows (one row per ALT_SESS_DT_MS) of a cruise-speed charge that ended at entry,
// the alternator under the battery charge limit as in the live demo and warming as it runs, with the session statistics
// those rows imply (the live statistics count graded ticks, five per row). The last row is what the plot repeats while
// nothing is graded (altSessSeriesPush).
static void demoSeedAltSession() {
  altSessRingHead = 0; altSessRingCount = 0; altSessRingLastMs = 0;
  memset(altSessHist, 0, sizeof(altSessHist)); altSessN = 0; altSessSum = 0;
  if (!altSessRing) return;
  const int N = 1800000 / ALT_SESS_DT_MS;
  float k = demoClassK(), anom = demoAnom();
  uint32_t now = millis();
  float rpmN = 0, ampN = 0, tN = 0, pctN = 0, tPrev = 0;
  for (int i = 0; i < N; i++) {
    uint32_t u = (uint32_t)i;
    rpmN = demoOuStep(rpmN, 8.0f, 22.0f, u, 401);
    ampN = demoOuStep(ampN, 24.0f, 7.0f, u, 403);    // the sun and the loads moving under the battery limit
    tN = demoOuStep(tN, 10.0f, 0.4f, u, 404);
    pctN = demoOuStep(pctN, 12.0f, 0.3f, u, 405);
    float rpm = DEMO_CRUISE_RPM + rpmN + 6.0f * demoGauss(u, 402);
    float amps = 0.42f * anom + ampN + 1.0f * demoGauss(u, 406);
    float tF = 104.0f + 24.0f * (1.0f - expf(-(float)(i + 60) / 150.0f)) + tN;
    float vbus = (13.44f + 0.07f * (float)i / (float)(N - 1) + 0.004f * demoGauss(u, 407)) * k;
    bool est = demoHash01(u * 7919u + 17u) < 0.12f;
    float pct = constrain(98.5f + pctN + 0.15f * demoGauss(u, 408) - (est ? 0.5f : 0.0f), 97.65f, 99.35f);
    // field duty that makes this output at this speed and temperature (demoAltAmps inverted for the excitation)
    float sf = demoAltSpeedFrac(rpm), therm = 1.0f - 0.0012f * (tF - 100.0f);
    float sat = amps / (anom * 1.3f * sf * therm * DEMO_ALT_HEALTH);
    float x = -0.5f * logf(fmaxf(1e-3f, 1.0f - sat * (1.0f - expf(-2.0f))));
    float tc = (tF - 32.0f) / 1.8f, denom = 1.0f + ALT_ALPHA_PER_C * (tc - ALT_TREF_C);
    float duty = constrain(x * 12.8f * denom / vbus * ((float)fieldWindApplied / 12.0f) * 100.0f, 0.0f, 99.0f);
    AltSessPt &p = altSessRing[altSessRingHead];
    p.tMs = now - (uint32_t)(N - 1 - i) * ALT_SESS_DT_MS;
    p.flags = est ? 1 : (demoHash01(u * 15485863u) < 0.9f ? 0x04 : 0);
    p.gAmps = altQU16(amps, 100.0f);
    p.measA = altQU16(amps + 0.3f * demoGauss(u, 409), 100.0f);
    p.pred = altQU16(amps / (pct / 100.0f), 100.0f);
    p.rpm = altQU16(rpm, 1.0f);
    p.duty = altQU16(duty, 100.0f);
    p.vbus = altQU16(vbus, 100.0f);
    p.tF = altQI16(tF, 10.0f);
    p.tSlope = altQI16(i ? (tF - tPrev) * 12.0f : 0.0f, 100.0f);   // F per minute between 5 s rows
    tPrev = tF;
    altSessLastGraded = p;
    altSessRingHead = (altSessRingHead + 1) % ALT_SESS_RING_N;
    altSessRingCount++;
    for (int j = 0; j < 5; j++) {
      float q = pct + 0.12f * demoGauss(u * 5u + (uint32_t)j, 410);
      altSessSum += q; altSessN++;
      int bin = constrain((int)(q * 0.5f), 0, 59);
      if (altSessHist[bin] < 65535) altSessHist[bin]++;
    }
  }
  altSessRingLastMs = now;
}
// Sailing polar and motoring curve, the same functions the live boat sails on
static void demoSeedPerfFronts() {
  sailFront.pts = demoSailBuf; sailFront.cap = DEMO_PERF_CAP; sailFront.count = 0; sailFront.source = 0;
  motorFront.pts = demoMotorBuf; motorFront.cap = DEMO_PERF_CAP; motorFront.count = 0; motorFront.source = 0;
  sailFrontBuf = demoSailBuf; motorFrontBuf = demoMotorBuf;
  uint32_t t = demoNowEpoch();
  static const float SEA[3] = { 0.8f, 1.4f, 2.0f };
  for (int w = 0; w < 9; w++)
    for (int a = 0; a < 15; a++)
      for (int q = 0; q < 3; q++) {
        float tws = 6.0f + 2.0f * w, twa = 40.0f + 10.0f * a, seaHs = (SEA[q] - 0.4f) / 1.6f;
        float stw = demoSailStw(tws, twa, seaHs);
        float tr = twa * PI / 180.0f, ax = tws * sinf(tr), ay = tws * cosf(tr) + stw;
        FrontPoint<PERF_NAXIS> p;
        p.x[0] = sqrtf(ax * ax + ay * ay); p.x[1] = perfFoldAwa(atan2f(ax, ay) * 180.0f / PI); p.x[2] = SEA[q];
        p.y = stw; p.ex[0] = 0; p.ex[1] = 0; p.nSamp = 30; p.tEmit = t - (uint32_t)(demoHash01(w * 131 + a * 17 + q) * 300.0f * 86400.0f);
        sailFront.add(p);
      }
  for (int r = 0; r < 20; r++)
    for (int h = 0; h < 7; h++)
      for (int q = 0; q < 3; q++) {
        float rpm = 700.0f + 100.0f * r, hw = -10.0f + 4.0f * h, seaHs = (SEA[q] - 0.4f) / 1.6f;
        FrontPoint<PERF_NAXIS> p;
        p.x[0] = rpm; p.x[1] = hw; p.x[2] = SEA[q];
        p.y = demoMotorStw(rpm, hw, seaHs); p.ex[0] = 0; p.ex[1] = 0; p.nSamp = 30;
        p.tEmit = t - (uint32_t)(demoHash01(r * 53 + h * 11 + q) * 300.0f * 86400.0f);
        if (p.y > 0.3f) motorFront.add(p);
      }
}
static void demoSeedBaro() {
  uint32_t e0 = demoEpoch0;
  for (int i = 0; i < BARO_HISTORY_SIZE; i++) {
    uint32_t ep = e0 - (uint32_t)(BARO_HISTORY_SIZE - 1 - i) * (BARO_SAMPLE_INTERVAL_MS / 1000UL);
    demoBaroBuf[i] = (uint16_t)((demoBaroAt(ep) + 0.03f * demoGauss(ep, 204)) * 10.0f + 0.5f);   // the BMP388's own noise
  }
  baroPressureHistory = demoBaroBuf;
  baroHistoryHead = 0;
  baroHistoryLastEpoch = e0;
  lastBaroSampleMs = millis();
}
static void demoSeedOvTel() {
  memset(demoOvTelBuf, 0, sizeof(OvTelemetry));
  demoOvTelBuf->softExceedCount = 2; demoOvTelBuf->kdEventCount = 5; demoOvTelBuf->tierLowCutCount = 1;
  demoOvTelBuf->magic = OVTEL_MAGIC;
}
// A year of a cruising boat's battery wear, laid straight into the boxes (no tick simulation): mostly resting and cycling
// through the middle, a few hours a day charging, minutes a year just above absorption, one warm summer.
static void demoSeedBatteryWear() {
  BatteryWear &w = *demoBatWearBuf;
  batteryWearClear(w);
  const bool lith = batteryIsLithium();
  const float C = demoCapAh();
  const double obsS = 300.0 * 86400.0;
  static const float tRep[] = { 41.0f, 59.0f, 77.0f, 95.0f, 108.0f };
  static const float tFrac[] = { 0.04f, 0.25f, 0.50f, 0.19f, 0.02f };
  struct VS { float v12, frac; };
  static const VS vLith[] = { {12.95f, 0.02f}, {13.05f, 0.08f}, {13.15f, 0.14f}, {13.25f, 0.22f}, {13.35f, 0.20f},
    {13.45f, 0.08f}, {13.55f, 0.05f}, {13.65f, 0.06f}, {13.85f, 0.04f}, {14.05f, 0.04f}, {14.15f, 0.03f},
    {14.25f, 0.025f}, {14.35f, 0.008f}, {14.45f, 0.0015f}, {14.65f, 0.0002f}, {14.85f, 0.00003f}, {15.25f, 0.000001f} };
  static const VS vLead[] = { {12.25f, 0.05f}, {12.45f, 0.12f}, {12.65f, 0.18f}, {12.85f, 0.15f}, {13.05f, 0.10f},
    {13.25f, 0.08f}, {13.45f, 0.10f}, {13.55f, 0.07f}, {13.75f, 0.04f}, {14.05f, 0.04f}, {14.35f, 0.035f},
    {14.45f, 0.025f}, {14.65f, 0.005f}, {14.85f, 0.0008f}, {15.15f, 0.00005f} };
  const VS *vs = lith ? vLith : vLead;
  const int nv = lith ? (int)(sizeof(vLith) / sizeof(vLith[0])) : (int)(sizeof(vLead) / sizeof(vLead[0]));
  const float chargingFrom = lith ? 13.6f : 13.5f;
  for (int i = 0; i < nv; i++) {
    const int vb = bwBinOf(vs[i].v12, BW_V_EDGES_12V, BW_V_BINS - 1);
    const float aC = (vs[i].v12 >= 14.2f) ? 0.05f : (vs[i].v12 >= chargingFrom) ? 0.15f : (vs[i].v12 >= 13.3f) ? -0.02f : -0.05f;
    const float altFrac = (vs[i].v12 >= chargingFrom) ? 0.35f : 0.02f;
    for (int t = 0; t < 5; t++) {
      const uint32_t ds = (uint32_t)(obsS * vs[i].frac * tFrac[t] * 10.0);
      if (!ds) continue;
      const int tb = bwBinOf(tRep[t], BW_T_EDGES_F, BW_T_KNOWN - 1);
      w.vtDs[vb][tb] += ds;
      w.vIDs[vb] += ds;
      w.vAltDs[vb] += (uint32_t)(ds * altFrac);
      w.vSumMvMs[vb] += (uint64_t)lroundf(vs[i].v12 * 1000.0f) * (uint64_t)ds * 100ULL;
      w.iSumMaMs[vb] += (int64_t)lroundf(aC * C * 1000.0f) * (int64_t)ds * 100LL;
    }
  }
  struct SS { float soc, frac; };
  static const SS sLith[] = { {15, 0.005f}, {25, 0.02f}, {35, 0.04f}, {45, 0.07f}, {55, 0.10f}, {65, 0.13f}, {75, 0.16f},
    {85, 0.20f}, {92, 0.12f}, {98, 0.155f} };
  static const SS sLead[] = { {45, 0.02f}, {55, 0.05f}, {65, 0.10f}, {75, 0.18f}, {85, 0.25f}, {92, 0.17f}, {98, 0.23f} };
  const SS *ss = lith ? sLith : sLead;
  const int ns = lith ? (int)(sizeof(sLith) / sizeof(sLith[0])) : (int)(sizeof(sLead) / sizeof(sLead[0]));
  for (int i = 0; i < ns; i++) {
    const int sb = bwBinOf(ss[i].soc, BW_SOC_EDGES, BW_SOC_BINS - 2);
    for (int t = 0; t < 5; t++) {
      const uint32_t ds = (uint32_t)(obsS * ss[i].frac * tFrac[t] * 10.0);
      const int tb = bwBinOf(tRep[t], BW_T_EDGES_F, BW_T_KNOWN - 1);
      w.stDs[sb][tb] += ds;
      w.tSumDfMs[tb] += (int64_t)lroundf(tRep[t] * 10.0f) * (int64_t)ds * 100LL;
    }
  }
  static const float rRep[]  = { 0.01f, 0.035f, 0.07f, 0.15f, 0.3f, 0.7f };
  static const float rChg[]  = { 0.05f, 0.10f, 0.20f, 0.35f, 0.25f, 0.05f };
  static const float rDis[]  = { 0.30f, 0.35f, 0.20f, 0.10f, 0.04f, 0.01f };
  static const float cRep[]  = { 35.0f, 65.0f, 90.0f };
  static const float cFrac[] = { 0.20f, 0.45f, 0.35f };
  static const float rAlt[]  = { 0.15f, 0.30f, 0.55f, 0.80f, 0.95f, 1.00f };   // solar carries the slow charging
  static const float cAlt[]  = { 1.00f, 1.00f, 0.80f };
  const double ahYear = 150.0 * C;
  for (int t = 0; t < 5; t++) {
    const int tb = bwBinOf(tRep[t], BW_T_EDGES_F, BW_T_KNOWN - 1);
    for (int r = 0; r < 6; r++) {
      const int rb = bwBinOf(rRep[r], BW_RATE_EDGES_C, BW_RATE_BINS - 1);
      for (int c = 0; c < 3; c++) {
        const int cb = bwBinOf(cRep[c], BW_CSOC_EDGES, BW_CSOC_BINS - 2);
        const double cah = ahYear * tFrac[t] * rChg[r] * cFrac[c] * 100.0;
        w.chgCah[tb][rb][cb] += (uint32_t)cah;
        w.chgAltCah[tb][rb][cb] += (uint32_t)(cah * rAlt[r] * cAlt[c]);
      }
      w.disCah[tb][rb] += (uint32_t)(ahYear * 0.97 * tFrac[t] * rDis[r] * 100.0);
    }
  }
  const uint32_t now = demoNowEpoch();
  w.sinceEpoch = now - 365UL * 86400UL;
  w.unobservedS = 65UL * 86400UL;
  w.gapCount = 41;
  w.unobsSocS[bwBinOf(98, BW_SOC_EDGES, BW_SOC_BINS - 2)] = 40UL * 86400UL;
  w.unobsSocS[bwBinOf(65, BW_SOC_EDGES, BW_SOC_BINS - 2)] = 25UL * 86400UL;
  w.vMaxMv12 = (uint16_t)lroundf((lith ? 14.92f : 15.18f) * 1000.0f);
  w.vMaxTDf = 880; w.vMaxEpoch = now - 131UL * 86400UL;
  w.tMaxDf = 1093; w.tMaxEpoch = now - 96UL * 86400UL;
  w.tMinChgDf = 388; w.tMinChgEpoch = now - 302UL * 86400UL;
  w.socMin = lith ? 14 : 41; w.socMinEpoch = now - 57UL * 86400UL;
  // A year of cycles: daily shallow swings, fewer deep ones; lead-acid kept shallower. Half cycles per depth box.
  static const float cycD[]      = { 3.5f, 7.0f, 15.0f, 25.0f, 35.0f, 45.0f, 55.0f, 65.0f, 75.0f };
  static const uint32_t cycLith[] = { 900, 420, 260, 140, 80, 46, 22, 9, 3 };
  static const uint32_t cycLead[] = { 700, 380, 300, 120, 40, 12, 3, 1, 0 };
  for (int i = 0; i < 9; i++) {
    const uint32_t h = lith ? cycLith[i] : cycLead[i];
    if (!h) continue;
    const int cb = bwBinOf(cycD[i], BW_CYC_EDGES, BW_CYC_BINS - 1);
    const float mid = 100.0f - cycD[i] / 2.0f - (lith ? 8.0f : 5.0f);
    w.cycHalf[cb] += h;
    w.cycDepthSum[cb] += (uint32_t)lroundf(cycD[i] * 10.0f) * h;
    w.cycMidSum[cb] += (uint32_t)lroundf(mid * 10.0f) * h;
  }
  w.rfStack[0] = 1000; w.rfStack[1] = 520; w.rfStack[2] = 940; w.rfN = 3; w.rfExt = 710; w.rfDir = -1;
  static const uint32_t fullLith[] = { 12, 40, 35, 22, 9, 4, 1, 0 };
  static const uint32_t fullLead[] = { 30, 60, 40, 20, 6, 2, 0, 0 };
  static const float fullDays[]    = { 0.6f, 1.4f, 2.9f, 5.2f, 9.5f, 19.0f, 41.0f, 75.0f };
  for (int i = 0; i < BW_FULL_BINS; i++) {
    w.fullN[i] = lith ? fullLith[i] : fullLead[i];
    w.fullSumS[i] = (uint32_t)(w.fullN[i] * fullDays[i] * 86400.0f);
  }
  w.fullLastEpoch = now - 2UL * 86400UL - 5UL * 3600UL;
  w.fullAway = 1;
}
// Lifetime counters a four-season cruising boat would carry. NVS-backed values come back from loadNVSData() at exit.
static void demoSeedScalars() {
  float k = demoClassK(), anom = demoAnom(), C = demoCapAh();
  EngineRunTime_AllTime = 1512.0 * 3600.0;   AlternatorOnTime_AllTime = 1300.0 * 3600.0;
  EngineCycles_AllTime = 149688000;          EngineRunTime = 0; AlternatorOnTime = 0; EngineCycles = 0;
  ChargedEnergy_AllTime = 1597000; DischargedEnergy_AllTime = 1540000; AlternatorChargedEnergy_AllTime = 1210000;
  SolarChargedEnergy_AllTime = 387000;
  ChargedEnergy = 0; DischargedEnergy = 0; AlternatorChargedEnergy = 0; SolarChargedEnergy = 0;
  AlternatorFuelUsed_AllTime = 807; EngineFuelUsed_AllTime = 10300; AlternatorFuelUsed = 0; EngineFuelUsed = 0;
  ChargeCycles_AllTime = 312; ChargeCycles = 0; ChargeCycleEnergy = 0;
  TotalDistance_AllTime = 3284; TotalDistance = 0; MaxSpeed_AllTime = 9.4f; MaxSpeed = 0;
  totalSpeedSampleTime_AllTime = 2200000; speedAccumulator_AllTime = 5.4 * 2200000.0; AvgSpeed_AllTime = 5.4f; AvgSpeed = 0;
  speedAccumulator = 0; totalSpeedSampleTime = 0; socAccumulator = 0; totalSocSampleTime = 0;   // this trip's averages start here
  totalSocSampleTime_AllTime = 4000000; socAccumulator_AllTime = 74.0 * 4000000.0; AvgSOC_AllTime = 74.0f;
  totalVoltageSampleTime_AllTime = 4000000; voltageAccumulator_AllTime = 13.18 * k * 4000000.0; AvgVoltage_AllTime = 13.18f * k;
  LongestSingleTrip_Nm_AllTime = 312.0f; Max24hrDistance_AllTime = 148.0f; DeepestAnchorage_Ft_AllTime = 62.0f;
  best_upwind_vmg_alltime = 5.1f; longest_gale_duration_hours_alltime = 9.5f;
  sailing_days_alltime = 214.0f; sailing_dist_alltime = 2110.0f;
  alt_power_max_alltime_w = anom * 1.12f * 14.1f * k; solar_power_max_alltime_w = 590.0f;
  MeasuredAmpsMax_AllTime = anom * 1.12f; RPMMax_AllTime = 3200; PeakVoltage_AllTime = 14.12f * k; MinVoltage_AllTime = 11.80f * k;
  MaxAlternatorTemperatureF_AllTime = 181; MaxTemperatureThermistor_AllTime = 179;
  MeasuredAmpsMax = 0; RPMMax = 0; IBVMax = 0; MinVoltage = 99.0f * k; MaxAlternatorTemperatureF = 0; MaxTemperatureThermistor = 0;
  imu_slam_count_lifetime = 47; imu_heel_max_lifetime = 24.0f; imu_pitch_max_lifetime = 18.0f; imu_slam_peak_lifetime = 3.12f;
  CumulativeInsulationDamage = 0.025f; CumulativeGreaseDamage = 0.12f; CumulativeBrushDamage = 0.06f;
  timeSinceLastOverheat = 1483200ULL * 1000ULL;
  perfSailSeconds = 412.0 * 3600.0; perfMotorSeconds = 238.0 * 3600.0;
  // RAM-only values, restored from the backup at exit
  static const int OC[RPM_TABLE_SIZE] = { 0, 0, 1, 2, 0, 1, 3, 1, 0, 0 };
  static const float SH[RPM_TABLE_SIZE] = { 5, 10, 20, 30, 70, 120, 100, 40, 15, 5 };
  totalOverheats = 0; totalSafeMs = 0;
  for (int i = 0; i < RPM_TABLE_SIZE; i++) {
    overheatCount[i] = OC[i]; totalOverheats += OC[i];
    cumulativeNoOverheatTime[i] = (uint64_t)(SH[i] * 3600000.0f); totalSafeMs += cumulativeNoOverheatTime[i];
    lastOverheatTime[i] = 0;
  }
  static const float FC[FUELCURVE_BINS] = { 0, 0, 0, 9.2f, 8.8f, 8.4f, 7.9f, 7.3f, 6.6f, 5.9f, 5.2f, 4.6f, 4.0f, 3.5f, 0, 0, 0, 0 };
  for (int i = 0; i < FUELCURVE_BINS; i++) fuelCurveNMPG[i] = FC[i];
  DynamicAltCurrentZero = 0.012f;
  VictronYieldYesterday_kWh = 1.89f; VictronMaxPowerYesterday_W = 412.0f; VictronYieldToday_kWh = 0.05f; VictronMaxPowerToday_W = 40.0f;
  for (int i = 0; i < 20; i++) *DEMO_PROT[i] = 0;
  wmIgnReset("all");
  // The demo charge, held in RAM: the regulator's own Battery Charge Current Limit at 0.25 C (at most half the alternator's
  // cruise ceiling, so loads and the limit fit under it), so in bulk the battery current holds while the alternator gives
  // way to the sun and to falling loads; an hour of charging at the limit from the start SoC before the voltage loop
  // takes over. The made-up GX limit sits just above the alternator's own ceiling: following, never the binding one.
  float own = fminf(demoTableCap(DEMO_CRUISE_RPM), demoAltAmps(DEMO_CRUISE_RPM, 12.8f, 140.0f, DEMO_ALT_HEALTH));
  if (!(own > 5.0f)) own = 0.6f * anom;
  BattCurrentLimitA = fmaxf(10.0f, 5.0f * floorf(fminf(0.25f * C, 0.5f * own) / 5.0f));
  BattLimitEnable = true;
  float knee = batteryIsLithium() ? 98.0f : 88.0f;
  demoW.startSoc = constrain(knee - 100.0f * BattCurrentLimitA / C, 40.0f, 90.0f);
  SOC_percent = (int)(demoW.startSoc * 100.0f);
  CoulombCount_Ah_scaled = (int)(demoW.startSoc / 100.0f * C * 100.0f);
  demoW.battLimitA = fmaxf(20.0f, 5.0f * roundf((own + 5.0f) / 5.0f));
}

// ============================================================================================================
// Long-term plots: 30 days generated on request ending at entry, then the live demo records since
// ============================================================================================================
#define DEMO_EDT_OFFSET (-4L * 3600L)
static uint32_t demoLocalDay(uint32_t ep) { return (uint32_t)(((long)ep + DEMO_EDT_OFFSET) / 86400L); }
// The owner's month: aboard in stretches, ashore for one to four midweek days in most weeks (the boat alone on its mooring),
// aboard on the entry day and the day before it
static bool demoAboardDay(uint32_t day) {
  if (day + 1 >= demoLocalDay(demoEpoch0)) return true;
  uint32_t wk = (day + 4) / 7, dow = (day + 4) % 7;   // 1970-01-01 was a Thursday; weeks run Sunday to Saturday
  if (demoHash01(wk * 2654435761u + 41u) >= 0.65f) return true;
  uint32_t from = 1 + (uint32_t)(3.0f * demoHash01(wk * 40503u + 42u)), len = 1 + (uint32_t)(4.0f * demoHash01(wk * 7919u + 43u));
  return dow < from || dow >= from + len;
}
// Weather follows the pressure systems: a low brings cloud and wind, a high sun and light air
static float demoLtLow(uint32_t ep) { return (1014.0f - demoBaroAt(ep)) / 8.0f; }
static float demoLtCloudy(float low, float days) { return constrain(0.4f + 0.45f * low + 0.2f * demoWander(days / 0.8f, 214), 0.05f, 0.95f); }
static float demoLtGrad(float low, float days) { return fmaxf(2.0f, 7.5f + 4.0f * fmaxf(0.0f, low) + 2.5f * demoWander(days / 1.3f, 221)); }
// A sailing day: aboard that day and the day before, a workable breeze at 10:00 and no low's overcast, more often at a weekend
static bool demoSailDay(uint32_t day) {
  if (!demoAboardDay(day) || !demoAboardDay(day - 1)) return false;
  uint32_t ep10 = (uint32_t)((long)day * 86400L - DEMO_EDT_OFFSET + 10L * 3600L);
  float days = (float)(int32_t)(ep10 - 1767225600u) / 86400.0f, low = demoLtLow(ep10), grad = demoLtGrad(low, days);
  if (grad < 5.0f || grad > 19.0f || demoLtCloudy(low, days) > 0.8f) return false;
  uint32_t dow = (day + 4) % 7;
  return demoHash01(day * 2654435761u) < ((dow == 0 || dow == 6) ? 0.75f : 0.4f);
}
// One 10-minute record per step from 30 days back. A sailing day leaves between 09:00 and 11:30 and sails 3-6 h; a low bank
// aboard gets an hour or more of engine on the mooring, morning or evening. House loads follow who is aboard; the solar
// controller holds its absorption for an hour (lead: three) each day it gets there, then float. The cabin follows the air, the sun on the
// deck and the heater on cold evenings and mornings aboard; the battery follows the cabin over hours, the alternator its load.
// Noise draws are indexed by the record count k, so every request builds the same month.
struct DemoLtState {
  float soc, surf, altT, battT, roomT, cabinT, twsN, airN, absMin, arrH, leaveH;
  uint32_t k, planDay, runDay, runUntil;
  bool sailDay, aboardDay;
  void step(uint32_t ep, LongTermRecord &r);
};
static float demoDiurnal(float tod) {   // 0 at the dawn minimum (05:30), 1 at the mid-afternoon maximum (15:00)
  if (tod >= 5.5f && tod < 15.0f) return demoSmooth((tod - 5.5f) / 9.5f);
  float x = (tod >= 15.0f) ? tod - 15.0f : tod + 9.0f;   // hours since the maximum: quick cooling after sunset, slow overnight
  return 1.0f - (1.0f - expf(-x / 4.0f)) / (1.0f - expf(-14.5f / 4.0f));
}
// The bank at rest, at the 12 V class: demoOcv12 without the charging knee (LFP 13.55 V full, lead 12.75 V)
static float demoRest12(float soc01) {
  soc01 = constrain(soc01, 0.0f, 1.0f);
  if (!batteryIsLithium()) return 11.95f + 0.80f * soc01;
  float k = fmaxf(0.0f, (soc01 - 0.88f) / 0.12f);
  return 12.90f + 0.45f * soc01 + 0.20f * k * k;
}
// A bank taking I amps reads rest + knee * I / (I + 0.005 C) + I * rB (12 V class). The knee rises near full to twice the
// absorption voltage's margin over a full bank at rest, so a full bank held there takes a tail of about half a percent of C.
static float demoLtKnee(float soc01) {
  float top = fmaxf(0.2f, 2.0f * (BulkVoltage / demoClassK() + 0.02f - demoRest12(1.0f)));
  float x = batteryIsLithium() ? powf(fmaxf(0.0f, (fminf(soc01, 1.0f) - 0.88f) / 0.12f), 2.6f)
                               : powf(fmaxf(0.0f, (fminf(soc01, 1.0f) - 0.75f) / 0.25f), 2.0f);
  return top * x;
}
static void demoLtEnv(int16_t *f, float avg, float spread, float scale) {
  f[0] = (int16_t)lroundf((avg - spread) * scale); f[1] = (int16_t)lroundf((avg + spread) * scale); f[2] = (int16_t)lroundf(avg * scale);
}
static void demoLtSet(int16_t *f, float lo, float hi, float avg, float scale) {
  f[0] = (int16_t)lroundf(lo * scale); f[1] = (int16_t)lroundf(hi * scale); f[2] = (int16_t)lroundf(avg * scale);
}
void DemoLtState::step(uint32_t ep, LongTermRecord &r) {
    float C = demoCapAh(), k1 = demoClassK(), anom = demoAnom();
    // loads and array against the 12 V 200 Ah boat's, by stored energy: a bigger bank's boat uses more, not in proportion
    float eRatio = C * k1 / 200.0f, sc = (eRatio <= 1.0f) ? eRatio : powf(eRatio, 0.75f);
    long loc = (long)ep + DEMO_EDT_OFFSET;
    uint32_t day = (uint32_t)(loc / 86400L);
    float tod = (float)(loc % 86400L) / 3600.0f;
    float days = (float)(int32_t)(ep - 1767225600u) / 86400.0f;
    float s = (float)(int32_t)(ep - demoEpoch0);
    k++;
    // the day's plan, drawn once a day; the solar controller restarts its absorption each morning
    if (day != planDay) {
      planDay = day; absMin = 0;
      sailDay = demoSailDay(day); aboardDay = demoAboardDay(day);
      arrH = demoAboardDay(day - 1) ? 0.0f : 9.0f + 4.0f * demoHash01(day * 7919u + 44u);
      leaveH = demoAboardDay(day + 1) ? 24.0f : 17.5f + 2.5f * demoHash01(day * 7919u + 45u);
    }
    bool sailing = sailDay && (ep + 6 * 3600 < demoEpoch0);
    float dep = 9.0f + 2.5f * demoHash01(day * 7919u + 1u), outH = 0.5f + 0.4f * demoHash01(day * 7919u + 2u);
    float sailH = 3.0f + 3.0f * demoHash01(day * 7919u + 3u), backH = 0.4f + 0.4f * demoHash01(day * 7919u + 4u);
    int mode = 0;   // 0 mooring, 1 motoring, 2 sailing
    bool homeward = false;
    if (sailing) {
      if (tod >= dep && tod < dep + outH) mode = 1;
      else if (tod >= dep + outH && tod < dep + outH + sailH) mode = 2;
      else if (tod >= dep + outH + sailH && tod < dep + outH + sailH + backH) { mode = 1; homeward = true; }
    }
    bool aboard = mode != 0 || (aboardDay && tod >= arrH && tod < leaveH);
    float baro = demoBaroAt(ep), low = (1014.0f - baro) / 8.0f;
    float cloudy = demoLtCloudy(low, days);
    float cover = cloudy * constrain(0.5f + 0.7f * demoWander(days * 16.0f, 211), 0.0f, 1.0f);
    float sun = fmaxf(0.0f, sinf(PI * (tod - 7.2f) / 10.4f));   // a late-October day at 44 N: low sun, ten hours of it
    // the air (an air mass by the days, the diurnal swing larger in sun), the cabin, the engine room
    airN = demoOuStep(airN, 4.0f, 0.8f, k, 231);
    float air = 58.0f + 8.0f * demoWander(days / 2.5f, 232) + (7.0f + 9.0f * (1.0f - cloudy)) * demoDiurnal(tod) + airN;
    float wAir = demoSmooth((s + 86400.0f) / 86400.0f);
    if (wAir > 0) air += (demoAirF(fminf(s, 0.0f)) - air) * wAir;   // the last day warms into the live demo's
    bool heater = aboard && air < 58.0f && ((tod >= 16.5f && tod < 23.0f) || (tod >= 6.5f && tod < 9.0f));
    float cabinEq = air + 2.0f + 10.0f * sun * (1.0f - 0.8f * cover) + (aboard ? 3.0f : 0.0f);
    if (heater) cabinEq = fmaxf(cabinEq, 66.0f);
    cabinT += (cabinEq - cabinT) * 0.12f;
    // house loads in amps on the 12 V 200 Ah boat, scaled to the owner's. Ashore: the standing loads. Aboard: electronics, the
    // fridge (harder in a warm cabin), lights and screens in the evening, the galley in the morning, the heater's fan and
    // pump, the autopilot and instruments underway, now and then a few minutes of inverter (kettle, microwave).
    // loadPk is the record's peak: a compressor start, a pump, the inverter.
    float loadA, loadPk;
    if (!aboard) {
      loadA = 0.45f + 0.12f * demoWander(days * 24.0f, 241);
      loadPk = loadA + 0.6f;
    } else {
      loadA = 2.5f + 0.05f * fmaxf(0.0f, cabinT - 50.0f) + 0.3f * demoWander(days * 72.0f, 242);
      if (tod >= 17.5f && tod < 23.0f) loadA += 2.2f + 1.6f * demoHash01(day * 7919u + 46u);
      if (tod >= 6.5f && tod < 8.5f) loadA += 1.8f;
      if (heater) loadA += 1.4f;
      if (mode != 0) loadA += 2.5f;
      loadPk = loadA + 5.5f;
      float ev = demoHash01(k * 2246822519u + 47u);
      if (tod >= 7.0f && tod < 22.5f && ev < 0.05f) {
        float inv = 12.0f + 20.0f * demoHash01(k * 3266489917u + 48u);
        loadA += 0.4f * inv; loadPk += inv;
      }
    }
    if (aboard && soc < 40.0f) loadA *= 0.55f + 0.45f * fmaxf(0.0f, soc - 15.0f) / 25.0f;   // a low bank: things get switched off
    loadA *= sc / k1; loadPk *= sc / k1;
    // a 400 W array on the 200 Ah boat, giving half its rating in the low autumn sun
    float solarA = (tod > 7.2f && tod < 17.6f) ? 200.0f * sc * sun * (1.0f - 0.85f * cover) / (13.3f * k1) : 0.0f;
    // engine: motoring, or a charging run on the mooring when the bank is down to the owner's own mark that day
    if (mode == 0 && aboard && ep >= runUntil && runDay != day && ((tod >= 7.0f && tod < 9.0f) || (tod >= 17.0f && tod < 20.0f))
        && soc < 35.0f + 15.0f * demoHash01(day * 7919u + 49u)) {
      runDay = day;
      float need = 0.3f * C / fminf(0.61f * anom, 0.25f * C + loadA);   // hours to put back about a third of the bank
      runUntil = ep + (uint32_t)(3600.0f * constrain(need, 0.85f, 3.0f) * (0.85f + 0.3f * demoHash01(day * 7919u + 50u)));
    }
    bool chargeRun = (mode == 0 && ep < runUntil);
    bool engine = (mode == 1) || chargeRun;
    float rpm = (mode == 1) ? DEMO_CRUISE_RPM + 40.0f * demoWander(days * 144.0f, 213)
              : chargeRun ? 1350.0f + 30.0f * demoWander(days * 144.0f, 216) : 0.0f;
    float altCap = engine ? fminf(0.72f * anom * (chargeRun ? 0.85f : 1.0f), 0.25f * C + loadA) : 0.0f;
    // the chargers: each holds its own voltage and the bank takes what that allows (demoLtKnee). A charger held below a bank
    // still above it from its last charge leaves the loads to the bank. The solar controller's absorption sits a touch above
    // the regulator's, as the live sun's does. Bank voltages are at the 12 V class until displayed.
    float rB = 0.95f / C;   // the bank's resistance over a 10 min record, polarisation included
    float vAbs = BulkVoltage + 0.02f * k1, vFlt = fminf(FloatVoltage, vAbs - 0.2f * k1);
    float vSolar = (absMin < (batteryIsLithium() ? 60.0f : 180.0f)) ? vAbs : vFlt;
    float vHold = (solarA > 0.05f) ? (engine ? fmaxf(BulkVoltage, vSolar) : vSolar) : BulkVoltage, vH = vHold / k1;
    bool charging = engine || solarA > 0.05f;
    float soc01 = soc / 100.0f, aK = 0.005f * C, rest = demoRest12(soc01), knee = demoLtKnee(soc01);
    float bat = altCap + solarA - loadA;
    bool held = charging && bat > 0 && rest + knee * bat / (bat + aK) + bat * rB > vH;
    if (held) {
      float d = vH - rest;
      if (d <= 0) {
        bat = constrain((d - surf) / rB, -loadA, 0.0f);
      } else {   // the current at which the bank reads vH: rB I^2 + (rB aK + knee - d) I - d aK = 0
        float b = rB * aK + knee - d;
        bat = (-b + sqrtf(b * b + 4.0f * rB * d * aK)) / (2.0f * rB);
      }
    }
    float alt = engine ? constrain(bat + loadA - solarA, 0.0f, altCap) : 0.0f;
    soc = constrain(soc + bat * (10.0f / 60.0f) / C * 100.0f, 10.0f, 100.0f);
    // last 8 h: settle toward the state the live demo starts from
    float w = demoSmooth((s + 28800.0f) / 28800.0f), w0 = demoSmooth((s - 600.0f + 28800.0f) / 28800.0f);
    float f = (w > w0) ? (w - w0) / (1.0f - w0) : 0.0f;
    float liveAir = (f > 0) ? demoAirF(0.0f) : 0.0f;
    soc += (demoW.startSoc - soc) * f;
    soc01 = soc / 100.0f;
    // charging: the knee's share is the surface charge, which then fades over an hour or two
    rest = demoRest12(soc01);
    float v;
    if (bat > 0) {
      v = rest + demoLtKnee(soc01) * bat / (bat + aK) + bat * rB;
      if (charging && v > vH) held = true;   // the bank came up to the charger's voltage within the record
      surf = (held ? fminf(v, vH) : v) - rest - bat * rB;
    } else {
      surf *= 0.85f;
      v = rest + surf + bat * rB;
    }
    v *= k1;
    if (held) v = (bat > 0) ? vHold + 0.005f * k1 * demoGauss(k, 243) : fmaxf(v, vHold);
    if (held && vHold >= vAbs - 0.001f) absMin += 10.0f;
    // the battery box sits low, between the cabin and the hull the water keeps near 60 F
    float hullT = 60.0f + 3.0f * demoWander(days / 6.0f, 233);
    battT += ((0.5f * (cabinT + hullT) + 0.03f * fabsf(bat) / sc) - battT) * 0.028f;
    battT += (liveAir + 2.0f - battT) * f;
    roomT += ((cabinT + 1.0f + (engine ? 25.0f : 0.0f)) - roomT) * 0.3f;
    roomT += (liveAir + 1.0f - roomT) * f;
    float grad = demoLtGrad(low, days);
    float sbFrac = (1.0f - cloudy) * demoSmooth((tod - 10.5f) / 3.0f) * (1.0f - demoSmooth((tod - 17.0f) / 3.0f));
    twsN = demoOuStep(twsN, 3.0f, 1.0f, k, 222);
    float tws = fmaxf(1.0f, grad * (1.0f - 0.4f * sbFrac) + 9.0f * sbFrac + twsN);
    float twd = 200.0f + 40.0f * demoWander(days / 2.0f, 223);
    twd = demoWrap360(twd + demoWrap180(DEMO_TWD - twd) * sbFrac + 6.0f * demoWander(days * 48.0f, 224));
    // the last 2 h join the live demo's wind
    float wl = demoSmooth((s + 7200.0f) / 7200.0f);
    if (wl > 0) {
      tws += (demoWindMean(fminf(s, 0.0f)) - tws) * wl;
      twd = demoWrap360(twd + demoWrap180(demoWindDirMean(fminf(s, 0.0f)) - twd) * wl);
    }
    // where the bow points: lying ~30 deg off the wind on the mooring, a reach or a beat by the hour under sail
    uint32_t hr = (uint32_t)(loc / 3600L);
    float twaB = (mode == 0) ? 30.0f + 8.0f * demoWander(days * 144.0f / 2.0f, 225)
               : (mode == 2) ? ((demoHash01(hr * 2654435761u + 7u) < 0.5f) ? 1.0f : -1.0f) * (50.0f + 70.0f * demoHash01(hr * 40503u + 8u))
                             : demoWrap180(twd - (homeward ? 280.0f : 100.0f));
    float hdg = demoWrap360(twd - twaB);
    float stw = (mode == 2) ? demoSailStw(tws, twaB, 0.9f) * 0.97f : (mode == 1) ? demoMotorStw(rpm, 0, 0.9f) * 0.965f : 0.0f;
    float tr = twaB * PI / 180.0f, ax = tws * sinf(tr), ay = tws * cosf(tr) + stw;
    float aws = sqrtf(ax * ax + ay * ay), awa = atan2f(ax, ay) * 180.0f / PI;
    float heel = (mode == 2) ? fminf(18.0f, 12.0f * (aws / 18.0f) * (aws / 18.0f) * fabsf(sinf(awa * PI / 180.0f))) : 0.5f;
    float teq = engine ? roomT + 20.0f + 70.0f * powf(alt / anom, 1.3f) : roomT;
    altT += (teq - altT) * 0.75f;
    memset(&r, 0, sizeof(r));
    r.timestamp = ep;
    double lat = DEMO_LAT0, lon = DEMO_LON0;
    if (mode != 0) {
      float ang = (tod - dep) / (outH + sailH + backH) * 2.0f * PI;
      lat += 0.035 * sin(ang * 0.5f) * sin(ang * 0.5f) * -0.6; lon += 0.07 * sin(ang * 0.5f);
    }
    r.lat_avg = (int32_t)(lat * 100000.0); r.lon_avg = (int32_t)(lon * 100000.0);
    // within the record: appliance starts and cloud shadows pull the bank down, cloud edges and the engine's ripple push it up;
    // a held charger caps the top
    float dn = (loadPk - loadA) + 0.6f * solarA * cover, up = 0.8f * sc + 0.4f * solarA * cover + (engine ? 3.0f * sc : 0.0f);
    float r0 = 0.55f / C * k1;   // the bank's ohmic part, for swings inside the record
    demoLtSet(r.battVolt, v - 0.008f * k1 - dn * r0, v + ((held && bat > 0) ? 0.01f * k1 : 0.008f * k1 + up * r0), v, 100.0f);
    demoLtSet(r.battCurr, bat - dn, bat + (held ? 0.3f : 1.0f) * up, bat, 10.0f);
    demoLtEnv(r.altCurr, alt, alt > 0 ? 4.0f : 0.0f, 10.0f);
    demoLtSet(r.victronCurr, bat - dn, bat + (held ? 0.3f : 1.0f) * up, bat, 10.0f);
    demoLtEnv(r.rpm, rpm, rpm > 0 ? 40.0f : 0.0f, 1.0f);
    demoLtEnv(r.duty, engine ? 38.0f + 30.0f * alt / anom : 0.0f, engine ? 6.0f : 0.0f, 100.0f);
    demoLtEnv(r.altTemp, altT, engine ? 3.0f : 0.6f, 10.0f);
    demoLtEnv(r.tempTherm, altT + 3.0f, engine ? 3.0f : 0.8f, 10.0f);
    demoLtEnv(r.sog, stw, stw > 0 ? 0.3f + 0.05f * stw : 0.05f, 100.0f);
    demoLtEnv(r.tws, tws, 1.5f + 0.2f * tws, 100.0f);
    demoLtEnv(r.vmg, stw * cosf(tr), 0.3f, 100.0f);
    demoLtEnv(r.aws, aws, 1.5f + 0.2f * aws, 100.0f);
    demoLtEnv(r.awa, demoWrap180(awa), (mode == 0) ? 12.0f : 8.0f, 10.0f);
    demoLtEnv(r.twa, twaB, (mode == 0) ? 12.0f : 6.0f, 10.0f);
    if (mode != 2) heel += 0.3f * demoWander(days * 144.0f / 3.0f, 226);
    demoLtEnv(r.heel, (twaB < 0 ? -heel : heel), (mode == 2) ? 3.0f : 1.0f, 100.0f);
    demoLtEnv(r.pitch, (mode == 1) ? 1.4f : 0.0f, (mode == 0) ? 0.8f : 2.5f, 100.0f);
    float socSp = 0.1f + fabsf(bat) * (5.0f / 60.0f) / C * 100.0f;
    demoLtSet(r.soc, soc - socSp, fminf(100.0f, soc + socSp), soc, 10.0f);
    demoLtEnv(r.battTemp, battT, 0.3f, 10.0f);
    r.baro_avg = (int16_t)lroundf(baro * 10.0f);
    r.ambTemp_avg = (int16_t)lroundf(roomT * 10.0f);
    r.cog_avg = (int16_t)lroundf(hdg * 10.0f); r.heading_avg = (int16_t)lroundf(hdg * 10.0f);
    r.leeway_avg = (int16_t)lroundf((mode == 2 ? 3.0f : 0.0f) * 10.0f);
    r.altZero_avg = 1;
    r.validMask = 0x01FFFFFFu;
    if (mode == 0) r.validMask &= ~((1u << 8) | (1u << 10) | (1u << 19) | (1u << 21));
    r.chargeStage = engine ? ((v >= BulkVoltage - 0.02f * k1) ? 2 : 1) : 0;
    if (engine) {
      r.onAvg[0] = r.battVolt[2]; r.onAvg[1] = r.battCurr[2]; r.onAvg[2] = r.altCurr[2]; r.onAvg[3] = r.victronCurr[2]; r.onAvg[4] = r.duty[2];
      r.activeFrac = 200;
    }
}
static DemoLtState demoLtStart;      // state 30 days back, set at entry

static void demoServeLongTerm(AsyncWebServerRequest *request) {
  struct St { uint8_t header[16]; int headerPos; uint32_t idx; uint32_t bytePos; LongTermRecord rec; DemoLtState w; bool have; bool done; };
  St st;
  memset(&st, 0, sizeof(st));
  st.w = demoLtStart;
  uint16_t liveN = demoLtLiveCount;
  uint32_t total = demoLtBgN + liveN;
  if (total > 0xFFFF) total = 0xFFFF;
  uint16_t headLE = (uint16_t)total, countLE = (uint16_t)total, capLE = (uint16_t)total, recLE = sizeof(LongTermRecord);
  uint32_t newest = liveN ? demoLtLive[(demoLtLiveHead + DEMO_LT_LIVE_N - 1) % DEMO_LT_LIVE_N].timestamp : demoEpoch0;
  uint32_t ivLE = SENSOR_UPLOAD_INTERVAL / 1000UL;
  memcpy(st.header + 0, &headLE, 2); memcpy(st.header + 2, &countLE, 2); memcpy(st.header + 4, &capLE, 2);
  memcpy(st.header + 6, &recLE, 2); memcpy(st.header + 8, &newest, 4); memcpy(st.header + 12, &ivLE, 4);
  uint32_t bgN = demoLtBgN, e0 = demoEpoch0;
  uint16_t liveHead = demoLtLiveHead;
  AsyncWebServerResponse *response = request->beginChunkedResponse("application/octet-stream",
    [st, bgN, e0, liveN, liveHead, total](uint8_t *buf, size_t maxLen, size_t) mutable -> size_t {
      if (st.done) return 0;
      size_t written = 0;
      while (written < maxLen) {
        if (st.headerPos < 16) {
          size_t n = min(maxLen - written, (size_t)(16 - st.headerPos));
          memcpy(buf + written, st.header + st.headerPos, n);
          written += n; st.headerPos += (int)n;
          continue;
        }
        if (st.idx >= total) { st.done = true; return written; }
        if (!st.have) {
          if (st.idx < bgN) st.w.step(e0 - (bgN - st.idx) * 600UL, st.rec);
          else if (demoLtLive) st.rec = demoLtLive[(liveHead + DEMO_LT_LIVE_N - liveN + (st.idx - bgN)) % DEMO_LT_LIVE_N];
          st.have = true;
        }
        size_t n = min(maxLen - written, (size_t)(sizeof(LongTermRecord) - st.bytePos));
        memcpy(buf + written, ((const uint8_t *)&st.rec) + st.bytePos, n);
        written += n; st.bytePos += (uint32_t)n;
        if (st.bytePos >= sizeof(LongTermRecord)) { st.bytePos = 0; st.idx++; st.have = false; }
      }
      return written;
    });
  response->addHeader("Cache-Control", "no-cache");
  request->send(response);
}
// Called from uploadSensorHistory() at the record cadence while in demo: the live record goes to the demo ring only
void demoPushLongTermLive() {
  if (!demoLtLive || demoPhase != DEMO_ON) return;
  LongTermRecord rec;
  buildLongTermRecord(rec);
  demoLtLive[demoLtLiveHead] = rec;
  demoLtLiveHead = (demoLtLiveHead + 1) % DEMO_LT_LIVE_N;
  if (demoLtLiveCount < DEMO_LT_LIVE_N) demoLtLiveCount++;
}

// ============================================================================================================
// History endpoints that read files or real NVS: in demo they answer from here
// ============================================================================================================
static String demoBatteryHealthJson() {
  uint32_t now = demoNowEpoch(), d = 86400;
  float C = demoCapAh(), k = demoClassK();
  char b[2200];
  snprintf(b, sizeof(b),
    "{\"state\":0,\"reason\":\"\",\"elapsedMs\":0,\"totalMs\":0,\"low\":20,\"delta\":60,\"dwellMs\":4000,\"edges\":6,\"lastDcir\":6.21,"
    "\"results\":[{\"epoch\":%lu,\"dcir\":5.84,\"tF\":74,\"delta\":60,\"spread\":0.21,\"edges\":6,\"dwell\":4000,\"soc\":71,\"v\":%.2f,\"low\":20},"
    "{\"epoch\":%lu,\"dcir\":5.97,\"tF\":79,\"delta\":60,\"spread\":0.26,\"edges\":6,\"dwell\":4000,\"soc\":66,\"v\":%.2f,\"low\":20},"
    "{\"epoch\":%lu,\"dcir\":6.10,\"tF\":83,\"delta\":60,\"spread\":0.33,\"edges\":5,\"dwell\":4000,\"soc\":74,\"v\":%.2f,\"low\":20},"
    "{\"epoch\":%lu,\"dcir\":6.21,\"tF\":81,\"delta\":60,\"spread\":0.24,\"edges\":6,\"dwell\":4000,\"soc\":69,\"v\":%.2f,\"low\":20}],"
    "\"cap\":[{\"epoch\":%lu,\"pct\":99.2,\"ah\":%.0f,\"conf\":1},{\"epoch\":%lu,\"pct\":98.4,\"ah\":%.0f,\"conf\":1},"
    "{\"epoch\":%lu,\"pct\":97.9,\"ah\":%.0f,\"conf\":0},{\"epoch\":%lu,\"pct\":97.1,\"ah\":%.0f,\"conf\":1}],"
    "\"capLastUpd\":%lu,\"capAnchored\":0,\"capAnchorSoC\":0,\"capRestMin\":42,\"capDvdt\":0.8,"
    "\"capCfg\":{\"capacityAh\":%.0f,\"refMode\":0,\"restFrac\":0.6,\"restFloor\":30,\"settleRate\":2,\"socLowMax\":30,\"fullSoc\":99,"
    "\"minSpan\":40,\"chgEff\":0.99,\"tempNorm\":1,\"tempCoeff\":0.5,\"tempRef\":25},"
    "\"ocv\":[%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f],\"ocvSoc\":[100,90,80,70,60,50,40,30,20,10,0]}",
    (unsigned long)(now - 115 * d), 13.28f * k, (unsigned long)(now - 62 * d), 13.24f * k, (unsigned long)(now - 21 * d), 13.30f * k,
    (unsigned long)(now - 2 * d), 13.26f * k,
    (unsigned long)(now - 140 * d), C * 0.992f, (unsigned long)(now - 96 * d), C * 0.984f, (unsigned long)(now - 55 * d), C * 0.979f,
    (unsigned long)(now - 18 * d), C * 0.971f, (unsigned long)(now - 18 * d), C,
    demoOcv12(1.0f) * k, demoOcv12(0.9f) * k, demoOcv12(0.8f) * k, demoOcv12(0.7f) * k, demoOcv12(0.6f) * k, demoOcv12(0.5f) * k,
    demoOcv12(0.4f) * k, demoOcv12(0.3f) * k, demoOcv12(0.2f) * k, demoOcv12(0.1f) * k, demoOcv12(0.0f) * k);
  return String(b);
}
static String demoKneeJson() {
  String o = "{\"enable\":1,\"marginPct\":2,\"onsetA\":4,\"reArmA\":8,\"stepPct\":0.5,\"dwellSec\":20,\"tempComp\":1,\"tempRefF\":77,"
             "\"maxFloorPct\":25,\"rpmTolPct\":6,\"tempTolF\":15,\"dutyTolPct\":3,\"activeBin\":-1,\"floorActive\":0,\"steady\":0,\"bins\":[";
  char b[140];
  // every row learned and locked: the demo shows no row still waiting for data
  for (int i = 0; i < RPM_TABLE_SIZE; i++) {
    float knee = fmaxf(2.5f, 11.5f - 0.95f * i + 0.6f * demoHash01(i * 7919u + 3u));
    snprintf(b, sizeof(b), "%s{\"rpm\":%d,\"floor\":%.1f,\"knee\":%.1f,\"frozen\":1,\"learnT\":%d,\"ageS\":%ld}", i ? "," : "",
             rpmTableRPMPoints[i], knee + 1.5f, knee, 118 + 4 * i + (int)(9.0f * demoHash01(i * 104729u + 5u)),
             (long)(86400.0f * (2.0f + 3.5f * i) + 40000.0f * demoHash01(i * 15485863u + 7u)));
    o += b;
  }
  o += "]}";
  return o;
}
static String demoZeroFitJson() {
  uint32_t now = demoNowEpoch(), cx = now - 140 * 86400UL;
  char b[700];
  snprintf(b, sizeof(b),
    "{\"valid\":1,\"sensor\":\"alt\",\"c\":0.004,\"b\":0.0011,\"r2\":0.91,\"corrNow\":0.012,\"applied\":1,\"epoch\":%lu,\"histCount\":203,"
    "\"capHeld\":0,\"capEpoch\":%lu,\"capA\":-0.06,\"capRpm\":1850,\"capEngOff\":0,\"capN\":100,\"capP2P\":0.21,\"capAltF\":171.0,\"capBoardF\":104.0,"
    "\"capRej\":0,\"capBusy\":0,\"capPhase\":0,\"capApplied\":1,\"capAppliedA\":-0.06,\"capAppliedEp\":%lu,\"capOffset\":%.2f}",
    (unsigned long)(now - 3600), (unsigned long)cx, (unsigned long)cx, AlternatorCOffset);
  return String(b);
}
static String demoZeroFitCsv() {
  String o = "epoch,sensor,c,b,r2,n\n";
  uint32_t now = demoNowEpoch();
  char b[80];
  for (int i = 8; i >= 1; i--) {
    snprintf(b, sizeof(b), "%lu,alt,%.4f,%.5f,%.3f,%u\n", (unsigned long)(now - (uint32_t)i * 9UL * 86400UL - 3600UL),
             0.004f + 0.001f * (demoHash01(i * 31) - 0.5f), 0.0011f + 0.0002f * (demoHash01(i * 17) - 0.5f),
             0.88f + 0.06f * demoHash01(i * 7), (unsigned)(18 + (int)(demoHash01(i * 3) * 9)));
    o += b;
  }
  return o;
}
static String demoZeroLogJson() {
  uint32_t now = demoNowEpoch();
  char b[160];
  snprintf(b, sizeof(b), "{\"enable\":1,\"cap\":288,\"count\":203,\"oldest\":%lu,\"newest\":%lu}",
           (unsigned long)(now - 13 * 86400UL), (unsigned long)(now - 3600));
  return String(b);
}
static String demoTempSensorsJson() {
  char b[700];
  uint32_t age = 1 + (millis() / 1000) % 2;
  uint32_t ok = 48211 + (uint32_t)(demoSec() / 1.5f);
  snprintf(b, sizeof(b),
    "{\"count\":3,\"unassigned\":0,\"probes\":["
    "{\"id\":\"28ff641e8c1c0a3b\",\"role\":0,\"tempF\":%.1f,\"ageS\":%lu,\"ok\":%lu,\"fail\":3,\"present\":1},"
    "{\"id\":\"28ff9a2c7d1b0c44\",\"role\":1,\"tempF\":%.1f,\"ageS\":%lu,\"ok\":%lu,\"fail\":1,\"present\":1},"
    "{\"id\":\"28ffb0d55e0e0f19\",\"role\":2,\"tempF\":%.1f,\"ageS\":%lu,\"ok\":%lu,\"fail\":0,\"present\":1}],"
    "\"roles\":{\"alt\":\"28ff641e8c1c0a3b\",\"batt\":\"28ff9a2c7d1b0c44\",\"extra\":\"28ffb0d55e0e0f19\"},"
    "\"enabled\":{\"batt\":1,\"extra\":1},\"scanning\":0}",
    AlternatorTemperatureF, (unsigned long)age, (unsigned long)ok, BatteryTempProbeF, (unsigned long)age, (unsigned long)(ok - 7),
    ExtraTempF, (unsigned long)age, (unsigned long)(ok - 2));
  return String(b);
}
static String demoSolarLedgerCsv() {
  uint32_t nowS = demoNowEpoch();
  long tz = DEMO_EDT_OFFSET;
  long today = ((long)nowS + tz) / 86400L;
  const int N = 45;
  String o;
  o.reserve(4200);
  char b[200];
  snprintf(b, sizeof(b), "# sledger v1 today=%ld tz=%ld n=%d ratio=%.3f need=3.56 needSrc=1 learn=1 useCons=1\n", today, tz, N + 1, performanceRatio);
  o += b;
  o += "day,predHarv,predIrr,actHarv,predCons,actCons,alt,ratio,covMin,flags\n";
  for (int i = N; i >= 1; i--) {
    long d = today - i;
    float n1 = demoHash01(d * 3), n2 = demoHash01(d * 7 + 1), n3 = demoHash01(d * 11 + 2);
    bool cloudy = n2 > 0.82f;
    float predIrr = 4.2f + 0.6f * demoWander((float)d / 4.0f, 421) + n1 * 0.6f, r = 0.72f + 0.03f * (N - i) / N, predHarv = predIrr * r;
    float actHarv = predHarv * (cloudy ? 0.35f + n3 * 0.2f : 0.88f + n3 * 0.22f);
    float predCons = 2.7f + n1 * 0.5f, actCons = predCons * (0.85f + n2 * 0.3f), alt = demoSailDay((uint32_t)d) ? 0.9f + n1 * 0.8f : 0.0f;
    bool partial = (i == 17);
    snprintf(b, sizeof(b), "%ld,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d\n", d, predHarv, predIrr, actHarv, predCons, actCons, alt, r,
             partial ? 610 : 1380 + (int)(n2 * 60), 15 | (partial ? 16 : 0));
    o += b;
  }
  int mins = (int)((((long)nowS + tz) % 86400L) / 60L);
  snprintf(b, sizeof(b), "%ld,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,,%d,%d\n", today, 3.60f, 4.80f, VictronYieldToday_kWh, 2.85f, 1.18f, 0.12f, mins, 15 | 128);
  o += b;
  return o;
}

float demoSyncAgoS(float phase) { return fmodf(demoSec() + phase, 1800.0f); }
int demoMinutesToday() { return (int)((((long)demoNowEpoch() + DEMO_EDT_OFFSET) % 86400L) / 60L); }

// One answer for every demo-served endpoint; false = not a demo route (the caller serves the real one)
bool demoServe(AsyncWebServerRequest *request, const char *path) {
  if (demoPhase == DEMO_OFF) return false;
  if (demoPhase != DEMO_ON) { request->send(503, "text/plain", "Demo mode is switching, try again in a moment."); return true; }
  if (!strcmp(path, "/longTermPlots.bin")) { demoServeLongTerm(request); return true; }
  if (!strcmp(path, "/batteryHealth")) { request->send(200, "application/json", demoBatteryHealthJson()); return true; }
  if (!strcmp(path, "/kneeLearnState")) { request->send(200, "application/json", demoKneeJson()); return true; }
  if (!strcmp(path, "/zerofitstate")) { request->send(200, "application/json", demoZeroFitJson()); return true; }
  if (!strcmp(path, "/zerologstate")) { request->send(200, "application/json", demoZeroLogJson()); return true; }
  if (!strcmp(path, "/tempSensors")) { request->send(200, "application/json", demoTempSensorsJson()); return true; }
  if (!strcmp(path, "/solarledger.csv")) { request->send(200, "text/csv", demoSolarLedgerCsv()); return true; }
  if (!strcmp(path, "/huntledger")) { request->send(200, "text/plain", "none"); return true; }
  if (!strcmp(path, "/huntmap")) { request->send(200, "application/json", "{\"cut\":50,\"wing\":15,\"enabled\":0,\"pockets\":[]}"); return true; }
  if (!strcmp(path, "/zerofit.csv")) { request->send(200, "text/csv", demoZeroFitCsv()); return true; }
  if (!strcmp(path, "/zerolog.csv")) { request->send(200, "text/csv", "epoch,rpm,battV,altTempF,boardTempF,amps,p2pAmps\n"); return true; }
  return false;
}

// ============================================================================================================
// Owner actions refused in demo
// ============================================================================================================
// An allow-list: demo is for screenshots and for driving the real field by hand, and a setting written in demo can outlive the
// exit (a loader the exit reload does not run, a raw NVS writer). Any parameter not named here refuses the whole request.
// web_src/script.js REG_DEMO_ALLOWED mirrors the plain names for its native forms.
bool demoParamDenied(AsyncWebServerRequest *request) {
  static const char *const ALLOW[] = {
    "OnOff", "ManualFieldToggle", "ManualDutyTarget", "MaxDuty", "MaxFieldVolts", "MinDuty", "hardwarePresent", "ResetAlarmLatch",
    "altLogStop", "altSweepCancel", "bhAbort", "cancelFieldCurve", "cancelKneeSweep", "cancelSystemID", "chcCancel",
    "commissionAbort", "commissionStop", "cvPlantFitCancel", "cvStressCancel", "fieldCutCancel", "protTestCancel",
    "commissionHeartbeat",   // a RAM stamp, sent every 2 s while the wizard window is open; acted on only while commissioning runs
    // display only (units, plot axes and windows), loaded by InitSystemSettings, so the exit reload puts them back
    "displayTempUnit", "displayVolUnit", "Ymin1", "Ymax1", "Ymin2", "Ymax2", "Ymin3", "Ymax3", "Ymin4", "Ymax4", "yyMin", "yyMax",
    "xTime", "plotTimeWindow", "timeAxisModeChanging", "maxPoints",
    "uid", "tok", "src", "n", "gx",   // the request's address, unlock and sender tags, not settings
  };
  static const char *const ALLOW_ZERO[] = { "TuningMode", "CVTuningMode", "resTest", "bcurRtest", "resTestTargetA", "protTestStart",
                                            "ripGameFill", "faCommissionGate" };   // their stop value only
  for (size_t p = 0; p < request->params(); p++) {
    const AsyncWebParameter *ap = request->getParam(p);
    const String &name = ap->name();
    bool ok = false;
    for (size_t i = 0; !ok && i < sizeof(ALLOW) / sizeof(ALLOW[0]); i++) ok = (name == ALLOW[i]);
    for (size_t i = 0; !ok && i < sizeof(ALLOW_ZERO) / sizeof(ALLOW_ZERO[0]); i++)
      ok = (name == ALLOW_ZERO[i] && ap->value().toFloat() == 0.0f);
    if (!ok && name == "testProtectionsEnabled") ok = (ap->value().toInt() != 0);   // protections back on
    if (!ok) return true;
  }
  return false;
}
// 423 so the dashboard can tell a demo refusal from a locked or misaddressed one; plus a console line for forms that never read the reply
void demoRefuse(AsyncWebServerRequest *request) {
  queueConsoleMessage(DEMO_REFUSAL);
  request->send(423, "text/plain", DEMO_REFUSAL);
}

// ============================================================================================================
// Entry and exit
// ============================================================================================================
volatile bool demoEnterReq = false, demoExitReq = false;
static uint32_t demoExitCutWaitMs = 0;   // millis() the exit's field-off began (0 = no exit waiting on the field)
static uint32_t demoEnterCutWaitMs = 0;  // millis() the entry's field-off began (millis() | 1, same signed compare as exit)
#define DEMO_EXIT_CUT_WAIT_MS 20000UL
static volatile bool demoReloadDone = false;
static volatile bool demoPrepDone = false;
static bool demoLogsRan = false;         // the thermal / PID / CV logs took a sample in demo (Stop Logs can hold a real capture through it)
// "Hardware Present?" from /get (async_tcp). Entry and exit run on the loop; this only records the request.
const char *demoSwitchRequest(int v) {
  if (v == 0) {
    if (demoPhase == DEMO_ON || demoEnterReq) return nullptr;
    if (demoPhase != DEMO_OFF) return "Demo mode is still switching, try again in a moment.";
    if (otaInProgress) return "Not while a software update is running.";
    if (commissionState == 1 || systemIDActive || TuningMode || CVTuningMode) return "Finish or stop the commissioning test first.";
    if (importRebootPending || importRebootDeadlineMs) return "Not while an import is waiting to restart the regulator.";
    if (AltZeroCaptureNow) return "Not while a Measured Zero is running.";
    if (altLogState == 1 || altLogStartReq) return "Not while a gate capture is recording.";
    demoEnterReq = true;
    return nullptr;
  }
  if (demoPhase == DEMO_OFF && !demoEnterReq) return nullptr;
  if (demoEnterReq && demoPhase == DEMO_OFF) { demoEnterReq = false; return nullptr; }
  if (demoPhase != DEMO_ON) return "Demo mode is still switching, try again in a moment.";
  demoExitReq = true;
  return nullptr;
}

// Core 0, before the seal: the owner-OFF write (the field stays off after exit and after a reboot) and the one flush of the
// real storage scalars; then nothing persists until exit reloads them. Off the loop: an NVS commit can stall for seconds.
static void demoPrepTask(void *) {
  settingWrite(NK_OnOff, "0");
  saveNVSDataFull();
  demoPrepDone = true;
  vTaskDelete(NULL);
}
// Loop: the field goes off now; demoEnter() seals on the first pass after the core-0 flush finishes (DEMO_ENTERING until then)
static void demoEnterBegin() {
  if (altLogState == 1 || altLogStartReq) {   // a Record that landed after demoSwitchRequest() checked
    queueConsoleMessage("Demo mode could not start: a gate capture is recording, stop it first");
    return;
  }
  if (!demoAllocStores()) {
    queueConsoleMessage("Demo mode could not start: not enough free PSRAM for the demo stores");
    return;
  }
  if (!demoOvMtx) demoOvMtx = xSemaphoreCreateMutex();
  demoPrepDone = false;
  if (xTaskCreatePinnedToCore(demoPrepTask, "demoPrep", 8192, nullptr, 1, nullptr, 0) != pdPASS) {
    queueConsoleMessage("Demo mode could not start: no memory for the save task, try again");
    return;
  }
  if (bhTestState == 1) bhAbort("demo mode started");   // the entry ramp-down never aborts it, and its samples turn simulated at the seal
  demoPhase = DEMO_ENTERING;
  OnOff = 0;   // the owner-OFF path in /get (3_functions.ino); its NVS write is the prep task's
  demoEnterCutWaitMs = millis() | 1u;
  stateRevision++;
  settingsDirty = true;
}

static void demoEnter() {
  frontWriteBegin();   // every record-book store is swapped below; a lookup in flight drops
  // The open real sensor window is filed as a real record (RAM rings only)
  uploadSensorHistory();
  demoOvN = 0;
  demoOvLive = true;
  demoSeal = true;
  demoPhase = DEMO_ENTERING;
  huntEpisodeDrop();
  demoInaGone = false;   // normal running owned the chip since the last demo

  DemoBackup &b = *demoBak;
  b.altFront2 = altFront2; b.altFrontBuf = altFrontBuf; b.altHaveUpload = altHaveUpload; b.altRefSource = altRefSource;
  b.altTrend = altTrend; b.altTrendCount = altTrendCount; b.altTrendFlushed = altTrendFlushed; b.altTrendRewrite = altTrendRewrite;
  b.altBucket_sum = altBucket_sum; b.altBucket_n = altBucket_n; b.altBucket_worst = altBucket_worst;
  memcpy(b.altBucketHist, altBucketHist, sizeof(altBucketHist)); b.altCurEngHour = altCurEngHour;
  b.altTrendBaselineSec = altTrendBaselineSec; b.altLatchPct = altLatchPct; b.altWorstPctLive = altWorstPctLive;
  b.altOverallPctLive = altOverallPctLive;
  b.altSessRingHead = altSessRingHead; b.altSessRingCount = altSessRingCount; b.altSessRingLastMs = altSessRingLastMs;
  if (altSessRing) memcpy(demoAltSessBak, altSessRing, sizeof(AltSessPt) * ALT_SESS_RING_N);
  b.sailFront = sailFront; b.motorFront = motorFront; b.sailFrontBuf = sailFrontBuf; b.motorFrontBuf = motorFrontBuf;
  b.baroPressureHistory = baroPressureHistory; b.baroHistoryHead = baroHistoryHead;
  b.baroHistoryLastEpoch = baroHistoryLastEpoch; b.lastBaroSampleMs = lastBaroSampleMs;
  for (int i = 0; i < 18; i++) b.wm[i] = *WM_ALL[i].w;
  memcpy(b.overheatCount, overheatCount, sizeof(overheatCount));
  memcpy(b.cumulativeNoOverheatTime, cumulativeNoOverheatTime, sizeof(cumulativeNoOverheatTime));
  memcpy(b.lastOverheatTime, lastOverheatTime, sizeof(lastOverheatTime));
  b.totalOverheats = totalOverheats; b.totalSafeMs = totalSafeMs;
  memcpy(b.fuelCurveNMPG, fuelCurveNMPG, sizeof(fuelCurveNMPG));
  memcpy(b.distHourBuckets, distHourBuckets, sizeof(b.distHourBuckets)); b.distHourHead = distHourHead; b.distHourStartMs = distHourStartMs;
  b.DynamicAltCurrentZero = DynamicAltCurrentZero; b.VictronYieldYesterday_kWh = VictronYieldYesterday_kWh;
  b.VictronMaxPowerYesterday_W = VictronMaxPowerYesterday_W; b.VictronYieldToday_kWh = VictronYieldToday_kWh;
  b.VictronMaxPowerToday_W = VictronMaxPowerToday_W;
  for (int i = 0; i < 20; i++) b.prot[i] = *DEMO_PROT[i];
  b.imuEnabled = imuEnabled;
  b.n183SentenceCount = n183SentenceCount;
  b.dvccEn = dvccEn; b.dvccSrcType = dvccSrcType; b.dvccLostStop = dvccLostStop; b.dvccState = dvccState; b.dvccUntrustReason = dvccUntrustReason;
  b.commissionState = commissionState; b.commissionPhase = commissionPhase; b.commissionDoneMask = commissionDoneMask;
  b.IgnoreRPM = IgnoreRPM; b.IgnoreTemperature = IgnoreTemperature;
  b.BatteryCapacity_Ah = BatteryCapacity_Ah; b.BatteryShuntPresent = BatteryShuntPresent;
  b.ShuntResistanceMicroOhm = ShuntResistanceMicroOhm;
  b.battTempSource = battTempSource; b.battTempProbeEnable = battTempProbeEnable; b.extraTempProbeEnable = extraTempProbeEnable;
  b.BattMonSrc = BattMonSrc; b.BattLimitEnable = BattLimitEnable; b.BattCurrentLimitA = BattCurrentLimitA;
  b.accCur4 = accCur4; b.accVolt4 = accVolt4; b.accThermSessions = accThermSessions; b.accThermBindingSec = accThermBindingSec;
  b.accThermInbandSec = accThermInbandSec; b.accThermWorstOverF = accThermWorstOverF;
  memcpy(b.altSessHist, altSessHist, sizeof(altSessHist)); b.altSessN = altSessN; b.altSessSum = altSessSum;
#define DEMO_LT_SAVE(v) memcpy((void *)&b.life.v, (const void *)&v, sizeof(v));
  DEMO_LIFETIME_LIST(DEMO_LT_SAVE)
#undef DEMO_LT_SAVE

  demoT0Ms = millis();
  demoEpoch0 = (timeIsSynced && timeBase > 0) ? (uint32_t)(timeBase + (millis() - timeBaseMillis) / 1000) : DEMO_FALLBACK_EPOCH;
  demoW = DemoWorld();
  demoBuildLegs();
  // The simulated bank, held in RAM: a plausible capacity and a shunt as the battery monitor, so the regulator's own SoC
  // count integrates the simulated battery current (the SoC shown, and the battery voltage curve the demo reads back)
  BatteryCapacity_Ah = (int)demoCapAh();
  BatteryShuntPresent = 1;
  if (ShuntResistanceMicroOhm <= 0) ShuntResistanceMicroOhm = 100;
  BattMonSrc = 0;
  // its battery and engine-room probes are fitted and on, and the battery probe is the battery temperature in use
  battTempSource = 1; battTempProbeEnable = 1; extraTempProbeEnable = 1;
  demoSeedScalars();
  demoSeedAltFront();
  demoSeedAltTrend();
  altHaveUpload = false; altRefSource = 0;
  demoSeedAltSession();
  altRefOk = false; altLive_pct = 0;
  altEmitQCount = 0; altEpisode.clearRun(); altThermGate.clear(); altSessTempGate.clear();
  demoSeedPerfFronts();
  sailEpisode.clearRun(); motorEpisode.clearRun(); perfPitchHead = 0; perfPitchCount = 0;
  demoSeedBaro();
  demoSeedOvTel();
  demoOvTelPtr = demoOvTelBuf;
  demoSeedBatteryWear();
  demoBatWearPtr = demoBatWearBuf;
  demoLtLiveHead = 0; demoLtLiveCount = 0;
  demoLtBgN = 30UL * 144UL;
  demoLtStart = { 70.0f, 0.0f, 62.0f, 60.0f, 62.0f, 60.0f, 0.0f, 0.0f, 0.0f, 0.0f, 24.0f, 0u, 0u, 0u, 0u, false, true };
  demoMakePast();
  // the world's state at script time 0 (apparent wind, bank voltage), so the frames the GX gets at once below are already
  // the simulated boat's, not the fresh world's zeros
  demoNavStep(0.0f, 0.0f);
  demoPlantStep(0.0f, 0.0f);
  resetAccuracyScores(true);
  // Overrides held in RAM: commissioned through Charge Health Calibration, Ignore RPM / Temperature off, follow the made-up GX
  commissionState = 2; commissionPhase = 9; commissionDoneMask |= (uint16_t)((1u << 10) - 1u);
  IgnoreRPM = 0; IgnoreTemperature = 0;
  dvccEn = 1; dvccSrcType = 0; dvccLostStop = 0; dvccCfgChanged = true; dvccResetReq = true;
  dvccRxWifiCvl = dvccRxWifiCcl = NAN; dvccRxWifiMs = 0;
  imuEnabled = true;
  altSimMode = 1.0f; perfSimMode = 1.0f;
  hardwarePresent = 0;
  lastSOCUpdateTime = millis();   // the count's first demo interval starts here, not at the start of the entry flush
  demoLogsRan = loggingActive;
  alarmLatchReal = alarmLatch;    // a latch held into demo is real; one raised on simulated readings is dropped at exit
  demoPhase = DEMO_ON;
  settingsDirty = true;
  gxKickPending = true;           // every GX channel at once, so the GX switches to the simulated boat in one step
  gxDevicesForget();              // the GX device readings on hand are the real boat's
  queueConsoleMessage("Demo mode on: simulated boat, field off until switched on, nothing is saved or uploaded until Hardware Present is set back to Yes");
  frontWriteEnd();
}

// Core 0: the reads that bring the real values back. NVS reads stall nothing on core 1 that it does not already accept from a /get.
static bool demoReloadSettings = false;
static void demoReloadWork() {
  loadNVSData();
  baroMarkDemoGap();   // after the ring reload above
  if (demoReloadSettings) {   // a setting changed in demo: every setting and table comes back from NVS
    InitSystemSettings();
    altSettingsLoad();
    perfSettingsLoad();
    loadLearningTableFromNVS();
    kneeLearnInit();
    loadFuelTableFromNVS();
    // the overheat table in RAM can be ahead of its last flush: keep the copy taken at entry
    memcpy(overheatCount, demoBak->overheatCount, sizeof(overheatCount));
    memcpy(cumulativeNoOverheatTime, demoBak->cumulativeNoOverheatTime, sizeof(cumulativeNoOverheatTime));
    memcpy(lastOverheatTime, demoBak->lastOverheatTime, sizeof(lastOverheatTime));
    totalOverheats = demoBak->totalOverheats; totalSafeMs = demoBak->totalSafeMs;
  }
  demoReloadDone = true;
}
static void demoReloadTask(void *) {
  demoReloadWork();
  vTaskDelete(NULL);
}

static void demoExitBegin() {
  frontWriteBegin();   // the record-book stores swap back below; a lookup in flight drops
  DemoBackup &b = *demoBak;
  demoPhase = DEMO_EXITING;
  hardwarePresent = 1;
  altSimMode = 0.0f; perfSimMode = 0.0f;
  demoOvTelPtr = nullptr;
  demoBatWearPtr = nullptr;
  altFront2 = b.altFront2; altFrontBuf = b.altFrontBuf; altHaveUpload = b.altHaveUpload; altRefSource = b.altRefSource;
  altTrend = b.altTrend; altTrendCount = b.altTrendCount; altTrendFlushed = b.altTrendFlushed; altTrendRewrite = b.altTrendRewrite;
  altBucket_sum = b.altBucket_sum; altBucket_n = b.altBucket_n; altBucket_worst = b.altBucket_worst;
  memcpy(altBucketHist, b.altBucketHist, sizeof(altBucketHist)); altCurEngHour = b.altCurEngHour;
  altTrendBaselineSec = b.altTrendBaselineSec; altLatchPct = b.altLatchPct; altWorstPctLive = b.altWorstPctLive;
  altOverallPctLive = b.altOverallPctLive;
  if (altSessRing) memcpy(altSessRing, demoAltSessBak, sizeof(AltSessPt) * ALT_SESS_RING_N);
  altSessRingHead = b.altSessRingHead; altSessRingCount = b.altSessRingCount; altSessRingLastMs = b.altSessRingLastMs;
  sailFront = b.sailFront; motorFront = b.motorFront; sailFrontBuf = b.sailFrontBuf; motorFrontBuf = b.motorFrontBuf;
  baroPressureHistory = b.baroPressureHistory; baroHistoryHead = b.baroHistoryHead;
  baroHistoryLastEpoch = b.baroHistoryLastEpoch; lastBaroSampleMs = b.lastBaroSampleMs;
  for (int i = 0; i < 18; i++) *WM_ALL[i].w = b.wm[i];
  memcpy(overheatCount, b.overheatCount, sizeof(overheatCount));
  memcpy(cumulativeNoOverheatTime, b.cumulativeNoOverheatTime, sizeof(cumulativeNoOverheatTime));
  memcpy(lastOverheatTime, b.lastOverheatTime, sizeof(lastOverheatTime));
  totalOverheats = b.totalOverheats; totalSafeMs = b.totalSafeMs;
  memcpy(fuelCurveNMPG, b.fuelCurveNMPG, sizeof(fuelCurveNMPG));
  memcpy(distHourBuckets, b.distHourBuckets, sizeof(b.distHourBuckets)); distHourHead = b.distHourHead; distHourStartMs = b.distHourStartMs;
  DynamicAltCurrentZero = b.DynamicAltCurrentZero; VictronYieldYesterday_kWh = b.VictronYieldYesterday_kWh;
  VictronMaxPowerYesterday_W = b.VictronMaxPowerYesterday_W; VictronYieldToday_kWh = b.VictronYieldToday_kWh;
  VictronMaxPowerToday_W = b.VictronMaxPowerToday_W;
  for (int i = 0; i < 20; i++) *DEMO_PROT[i] = b.prot[i];
  imuEnabled = b.imuEnabled;
  n183SentenceCount = b.n183SentenceCount;
  dvccEn = b.dvccEn; dvccSrcType = b.dvccSrcType; dvccLostStop = b.dvccLostStop;
  commissionState = b.commissionState; commissionPhase = b.commissionPhase; commissionDoneMask = b.commissionDoneMask;
  IgnoreRPM = b.IgnoreRPM; IgnoreTemperature = b.IgnoreTemperature;
  BatteryCapacity_Ah = b.BatteryCapacity_Ah; BatteryShuntPresent = b.BatteryShuntPresent;
  ShuntResistanceMicroOhm = b.ShuntResistanceMicroOhm;
  battTempSource = b.battTempSource; battTempProbeEnable = b.battTempProbeEnable; extraTempProbeEnable = b.extraTempProbeEnable;
  BattMonSrc = b.BattMonSrc; BattLimitEnable = b.BattLimitEnable; BattCurrentLimitA = b.BattCurrentLimitA;
  // The made-up GX limit must not outlive the demo: clear the WiFi carrier and make the trust machine settle again
  portENTER_CRITICAL(&dvccWifiMux);
  dvccWifiBoxCvl = dvccWifiBoxCcl = NAN;
  portEXIT_CRITICAL(&dvccWifiMux);
  dvccRxWifiCvl = dvccRxWifiCcl = NAN; dvccRxWifiMs = 0; dvccCvlV = dvccCclA = NAN;
  dvccCfgChanged = true;
  // a real UNTRUSTED latch survives the demo; anything else settles again on the real authority
  dvccState = (b.dvccState == DVCC_UNTRUSTED) ? DVCC_UNTRUSTED : DVCC_WAITING;
  dvccUntrustReason = (b.dvccState == DVCC_UNTRUSTED) ? b.dvccUntrustReason : 0;
  // live front-engine state built on demo inputs
  altEmitQCount = 0; altEpisode.clearRun(); altThermGate.clear(); altSessTempGate.clear();
  memcpy(altSessHist, b.altSessHist, sizeof(altSessHist)); altSessN = b.altSessN; altSessSum = b.altSessSum;
  altSteady = false; altSessSteady = false; altLive_pct = 0; altState = FRONT_NO_REFERENCE; altHiFieldAlert = false;
  sailEpisode.clearRun(); motorEpisode.clearRun(); perfPitchHead = 0; perfPitchCount = 0;
  perfLiveValid = false; motorLiveValid = false; perfLive_pct = 0; motorLive_pct = 0;
  perfLiveArmed = false; perfLiveReady = false;   // a demo grade in flight is not the real boat's
  resetAccuracyScores(true);
  accCur4 = b.accCur4; accVolt4 = b.accVolt4; accThermSessions = b.accThermSessions; accThermBindingSec = b.accThermBindingSec;
  accThermInbandSec = b.accThermInbandSec; accThermWorstOverF = b.accThermWorstOverF;
#define DEMO_LT_RESTORE(v) memcpy((void *)&v, (const void *)&b.life.v, sizeof(v));
  DEMO_LIFETIME_LIST(DEMO_LT_RESTORE)
#undef DEMO_LT_RESTORE
  // Navigation, wind, depth and VE.Direct inputs only the simulation wrote: back to their boot values and never-seen stamps, so
  // the last simulated value cannot pass as fresh (the lifetime speed average took the demo's SOG for a DATA_TIMEOUT window).
  // True wind is derived from them and keeps its last value until its own stamp ages out, so it is cleared with them.
  // Protection inputs (voltages, currents, RPM, temperatures) are left alone: the real read path refreshes them this pass.
  static const uint8_t DEMO_NAV_IDX[] = { IDX_HEADING_NMEA, IDX_COG_NMEA, IDX_SOG_NMEA, IDX_STW_NMEA, IDX_APPARENT_WIND_SPEED,
                                          IDX_APPARENT_WIND_ANGLE, IDX_TRUE_WIND_SPEED, IDX_TRUE_WIND_ANGLE, IDX_N183, IDX_N183_HDG,
                                          IDX_LATITUDE_NMEA, IDX_LONGITUDE_NMEA, IDX_SATELLITE_COUNT, IDX_WATER_DEPTH,
                                          IDX_VICTRON_SOLAR, IDX_VE_SOC, IDX_VICTRON_VOLTAGE, IDX_VICTRON_CURRENT };
  for (size_t i = 0; i < sizeof(DEMO_NAV_IDX); i++) dataTimestamps[DEMO_NAV_IDX[i]] = 0;
  TrueWindSpeedNMEA = NAN; TrueWindAngleNMEA = NAN;
  HeadingNMEA = 0; COGNMEA = 0; SOGNMEA = 0; STWNMEA = NAN; ApparentWindSpeedNMEA = NAN; ApparentWindAngleNMEA = NAN;
  n183HeadingDeg = -1.0f; n183HdgRef = 0; LatitudeNMEA = 0; LongitudeNMEA = 0; SatelliteCountNMEA = 0; WaterDepth_m = 0.0f;
  VictronSolarPower_W = 0.0f; VictronSolarVoltage_V = 0.0f; VictronSolarCurrent_A = 0.0f; VictronChargeState = -1;
  VictronMPPTMode = -1; VictronError = -1; VictronSoc = NAN; VictronVoltage = 0; VictronCurrent = 0;
  // A 1-Wire role with no probe on the bus (or switched off) gets no real read to overwrite the demo value: back to boot state
  if (tempRoleSlot[TR_ALT] < 0) { AlternatorTemperatureF = NAN; dataTimestamps[IDX_ALTERNATOR_TEMP] = 0; }
  demoExitRealAltTemp();   // 5_functions.ino: the real alternator reading taken in demo, with its stamp, when it is fresh
  if (tempRoleSlot[TR_BATT] < 0 || battTempProbeEnable != 1) { BatteryTempProbeF = NAN; dataTimestamps[IDX_BATT_TEMP_PROBE] = 0; }
  if (tempRoleSlot[TR_EXTRA] < 0 || extraTempProbeEnable != 1) { ExtraTempF = NAN; dataTimestamps[IDX_EXTRA_TEMP] = 0; }
  // A role with a probe keeps the demo value (fresh stamp) until TempTask's next real read, about 2 s away
  __atomic_store_n(&owDemoStaleMask,
                   (uint8_t)(((tempRoleSlot[TR_BATT] >= 0 && battTempProbeEnable == 1) ? (1u << TR_BATT) : 0u)
                             | ((tempRoleSlot[TR_EXTRA] >= 0 && extraTempProbeEnable == 1) ? (1u << TR_EXTRA) : 0u)),
                   __ATOMIC_RELAXED);
  // The simulated pressure must not be the real history's next sample: no reading until the BMP388 gives one (baroMarkDemoGap)
  baroPressure = NAN; dataTimestamps[IDX_BARO_PRESSURE] = 0;
  ambientTemp = NAN; dataTimestamps[IDX_AMBIENT_TEMP] = 0;   // the BMP388's board temperature likewise (zero log, maxima, board zero fit)
  // RAM rings that would carry demo samples into later real records or a Remote Diagnostics pull
  resetSensorWindow(); resetAccelWindow(); sustainedSpeedReset();
  imuDemoExitReset(); odometerRebaseline(); huntEpisodeDrop();
  if (demoLogsRan || loggingActive) {   // loggingActive: a Start Logs that landed after this pass's demoService
    thermalLogHead = 0; thermalLogCount = 0; pidLogHead = 0; pidLogCount = 0; cvLogHead = 0; cvLogCount = 0;
  }
  // A battery capacity measurement cannot span the demo: the real amp-hours of the span were never counted
  capLowAnchorValid = false; capBridgeAh = 0.0f; capBridgeMinAh = 0.0f; capRestTimerMs = 0; capPrevVForRate = NAN; capLastDvdtMv10 = 999.0f;
  demoReloadSettings = (demoOvN > 0);
  demoOvLive = false;          // reads see real NVS again; writes stay dropped until the reload finishes
  demoReloadDone = false;
  frontWriteEnd();
  if (xTaskCreatePinnedToCore(demoReloadTask, "demoReload", 8192, nullptr, 1, nullptr, 0) != pdPASS) {
    queueConsoleMessage("Demo mode: reload task could not start, reloading on the main loop");
    demoReloadWork();
  }
}
static void demoExitFinish() {
  demoOvClear();
  demoFreeStores();
  // At the seal drop, not exitBegin: until here CheckAlarms still ran on demo thresholds and simulated 1-Wire values
  alarmLatch = alarmLatchReal;
  demoSeal = false;
  demoPhase = DEMO_OFF;
  settingsDirty = true;
  // the reload rewrote settings: hardware-facing ones are re-applied the way their /get handlers would
  inaOvReprogramPending = true; n183ReconfigPending = true; n2kNodeReconfigPending = true;
  recomputeCvGains(); recomputeCcGains();
  gxKickPending = true;
  gxDevicesForget();
  queueConsoleMessage("Demo mode off: real readings and records are back");
}

// Loop (core 1), ahead of the sensor read every pass
void demoService() {
  if (demoEnterReq && demoPhase == DEMO_OFF) { demoEnterReq = false; demoEnterBegin(); }
  // Entry waits for the gate to cut too: the real field never runs on the demo's settings and simulated readings
  if (demoPhase == DEMO_ENTERING && !demoSeal) {
    OnOff = 0;
    bool cutTimedOut = (int32_t)(millis() - demoEnterCutWaitMs) > (int32_t)DEMO_EXIT_CUT_WAIT_MS;
    if (demoPrepDone && (gpio4IsLow || cutTimedOut)) {
      if (!gpio4IsLow) queueConsoleMessage("Demo mode: field still ramping down after 20 s, entering demo with it commanded off");
      demoEnter();
    }
  }
  // Exit turns the field off first and swaps back only once the gate is cut: the real field never runs on the demo's
  // settings, and /get refuses an ON until the reload has put the real ones back (OnOff "0" in NVS since entry)
  if (demoExitReq && demoPhase == DEMO_ON) {
    demoExitReq = false;
    if (!demoExitCutWaitMs) { demoExitCutWaitMs = millis() | 1u; stateRevision++; settingsDirty = true; }
  }
  if (demoExitCutWaitMs && demoPhase == DEMO_ON) {
    OnOff = 0;
    // signed: the stamp is millis() | 1, one ahead of millis() on an even millisecond, and unsigned that read as a 20 s
    // timeout on the first pass (bench 2026-10-28: the stores swapped back with the field still at 72 %)
    if (gpio4IsLow || (int32_t)(millis() - demoExitCutWaitMs) > (int32_t)DEMO_EXIT_CUT_WAIT_MS) {
      if (!gpio4IsLow) queueConsoleMessage("Demo mode: field still ramping down after 20 s, leaving demo with it commanded off");
      demoExitCutWaitMs = 0;
      demoExitBegin();
    }
  }
  if (demoPhase == DEMO_EXITING && demoReloadDone) demoExitFinish();
  if (demoPhase == DEMO_ON) {
    demoDvccTick();
    if (loggingActive) demoLogsRan = true;
    // the entry pass and the first simulated passes still hold the real readings and the fresh world's zeros (a 0 kn,
    // 0 deg wind row at the seam); the made-up rows cover that second
    if (millis() - demoT0Ms >= 1000) demoBackfillRecord();
  }
}
bool demoStoresBusy() { return demoPhase == DEMO_ENTERING || demoPhase == DEMO_EXITING; }
// The exit's field-off is still ramping down (OnOff held 0 each pass): an ON would only restart the shutdown ramp
bool demoExitCutWaiting() { return demoExitCutWaitMs != 0; }

void demoRegisterRoutes() {
  server.on("/demoBackfill", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (demoPhase != DEMO_ON || !demoBfS || !demoBfW) { request->send(404, "text/plain", "Demo mode is off"); return; }
    request->send(200, "application/json", demoBackfillJson());
  });
}
