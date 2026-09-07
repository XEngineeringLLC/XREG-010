
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

/*
 * KNOWN HEAP EFFICIENCY ISSUE (LOW PRIORITY): the OTA / forced-update HTTPS handlers
 * (checkForSpecificOTAUpdate, executeUpdateFirmwareVersion, executeCheckForcedUpdate)
 * build URLs / headers / JSON with String concatenation, which fragments the heap.
 * Accepted as-is: these run rarely and never in a tight loop. If OTA stability ever
 * suffers, convert them to snprintf into char buffers (see executeFetchWeatherData /
 * buildConfigPayload for the pattern).
 */

void updateCpuLoad() {
  if (!taskArray) return;  // allocated in setup()

  UBaseType_t taskCount = uxTaskGetSystemState(taskArray, MAX_TASKS, NULL);
  if (taskCount == 0) return;

  static uint64_t lastIdle0 = 0, lastIdle1 = 0;
  static uint64_t lastTotal = 0;
  static bool initialized = false;

  uint64_t idle0 = 0, idle1 = 0;
  uint64_t total = 0;

  for (UBaseType_t i = 0; i < taskCount; i++) {
    uint64_t t = (uint64_t)taskArray[i].ulRunTimeCounter;
    total += t;
    if (strcmp(taskArray[i].pcTaskName, "IDLE0") == 0) {
      idle0 = t;
    } else if (strcmp(taskArray[i].pcTaskName, "IDLE1") == 0) {
      idle1 = t;
    }
  }

  if (!initialized) {
    lastIdle0 = idle0;
    lastIdle1 = idle1;
    lastTotal = total;
    initialized = true;
    return;
  }

  // ulRunTimeCounter is 32-bit µs and wraps ~every 71 min; the uint64 cast happens before
  // differencing, so a wrapped sample makes deltaTotal enormous and both cores read 100%.
  // Discard the sample and re-baseline instead of latching a phantom max into telemetry.
  if (total < lastTotal || idle0 < lastIdle0 || idle1 < lastIdle1) {
    lastIdle0 = idle0;
    lastIdle1 = idle1;
    lastTotal = total;
    return;
  }

  uint64_t deltaIdle0 = idle0 - lastIdle0;
  uint64_t deltaIdle1 = idle1 - lastIdle1;
  uint64_t deltaTotal = total - lastTotal;

  if (deltaTotal == 0) return;

  uint32_t idlePct0 = (uint32_t)((deltaIdle0 * 200ULL) / deltaTotal);
  uint32_t idlePct1 = (uint32_t)((deltaIdle1 * 200ULL) / deltaTotal);

  if (idlePct0 > 100U) idlePct0 = 100U;
  if (idlePct1 > 100U) idlePct1 = 100U;

  cpuLoadCore0 = 100 - (int)idlePct0;
  cpuLoadCore1 = 100 - (int)idlePct1;

  if (cpuLoadCore0 < 0) cpuLoadCore0 = 0;
  if (cpuLoadCore0 > 100) cpuLoadCore0 = 100;
  if (cpuLoadCore1 < 0) cpuLoadCore1 = 0;
  if (cpuLoadCore1 > 100) cpuLoadCore1 = 100;

  if (cpuLoadCore0 > cpuLoadCore0Max) cpuLoadCore0Max = cpuLoadCore0;
  if (cpuLoadCore1 > cpuLoadCore1Max) cpuLoadCore1Max = cpuLoadCore1;

  lastIdle0 = idle0;
  lastIdle1 = idle1;
  lastTotal = total;
}

// Core-0 sampler for updateCpuLoad() — the uxTaskGetSystemState scan (~4ms) is too heavy for the
// Core-1 control loop. Runs at priority 0, so it only takes Core 0 when no network/upload task
// needs it. Writes cpuLoadCore0/1(+Max), read by the CSV2 sender on Core 1.
void cpuLoadTask(void *param) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    updateCpuLoad();
  }
}

void updateSystemHealthStats() {
  if (otaInProgress) return;

  // Only the 4s heap walk; CPU-load sampling runs in cpuLoadTask on Core 0.
  static unsigned long lastHeapSample = 0;

  unsigned long now = millis();

  if (now - lastHeapSample < 4000) return;
  if (gHeavyRanThisPass) return;  // another heavy ran this pass — defer (timer unchanged → still due)
  gHeavyRanThisPass = true;
  lastHeapSample = now;

  {
    FreeHeap = esp_get_free_heap_size() / 1024;
    MinFreeHeap = esp_get_minimum_free_heap_size() / 1024;
    FreeInternalRam = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
    TotalInternalRam = heap_caps_get_total_size(MALLOC_CAP_INTERNAL) / 1024;
    LargestInternalBlock = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024;
    FreePSRAM = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024;
    TotalPSRAM = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024;
    Heapfrag = (FreeInternalRam > 0)
                 ? 100 - (LargestInternalBlock * 100 / FreeInternalRam)
                 : 100;

    // Deliberately no low-contiguous-RAM console warning: every HTTPS handshake briefly
    // dips largest-block under 34 KB and recovers, so it would re-fire constantly.
    // The heap figures above surface it live in the ESP32 Stats panel.

    // LittleFS free space — event-driven, not polled: the value can only change when this
    // firmware writes a file, so writers set fsFreeDirty and the refresh runs here at the next
    // heap walk with the field gate physically cut (fieldCutSettled — the duty-based
    // fieldOffSettled reads "off" during a live duty-0 CV hold and let this ~96ms usedBytes()
    // traversal stall live control passes, 2026-08-13). The countdown is only a ~10 min
    // catch-all for any writer missing the fsFreeDirty hook; boot seeds the first value.
    static uint8_t fsFreeCountdown = 150;
    if (fsFreeCountdown > 0) fsFreeCountdown--;
    if ((fsFreeDirty || fsFreeCountdown == 0) && fieldCutSettled(0)) {
      size_t fsTotal = 0, fsUsed = 0;
      if (fsStatsTry(fsTotal, fsUsed) && fsTotal >= fsUsed) {
        LittleFsFreeKb = (int)((fsTotal - fsUsed) / 1024);
        fsFreeDirty = false;
        fsFreeCountdown = 150;  // 150 heap walks × ~4s
      }
    }
  }
}

bool executeFetchWeatherData() {
  // Called by HTTPS task on Core 0
  Serial.println(">>> executeFetchWeatherData() ENTERED");
  if (LatitudeNMEA == 0.0 && LongitudeNMEA == 0.0) {
    snprintf(weatherLastError, sizeof(weatherLastError), "No GPS coordinates available");
    weatherDataValid = 0;
    queueConsoleMessageF("Weather: No GPS coordinates");
    return false;
  }
  if (currentMode != MODE_CLIENT) {
    snprintf(weatherLastError, sizeof(weatherLastError), "Not in client mode");
    weatherDataValid = 0;
    return false;
  }

  if (WiFi.status() != WL_CONNECTED) {
    snprintf(weatherLastError, sizeof(weatherLastError), "WiFi not connected");
    weatherDataValid = 0;
    return false;
  }
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;

  char url[256];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.6f&longitude=%.6f&daily=shortwave_radiation_sum&timezone=auto",
           LatitudeNMEA, LongitudeNMEA);

  Serial.print("Open-Meteo URL: ");
  Serial.println(url);

  http.begin(client, url);
  http.setTimeout(WeatherTimeoutMs);

  int httpResponseCode = http.GET();
  weatherHttpResponseCode = httpResponseCode;

  if (httpResponseCode > 0) {
    String payload = http.getString();
    queueConsoleMessageF("GPS: %.6f,%.6f", LatitudeNMEA, LongitudeNMEA);
    Serial.printf("Response Code: %d\n", httpResponseCode);

    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, payload);
    if (error) {
      snprintf(weatherLastError, sizeof(weatherLastError), "JSON parse error: %s", error.c_str());
      weatherDataValid = 0;
      Serial.printf("JSON error: %s\n", error.c_str());
      http.end();
      return false;
    }
    JsonArray radiation = doc["daily"]["shortwave_radiation_sum"];
    if (radiation.size() < 3) {
      snprintf(weatherLastError, sizeof(weatherLastError), "Insufficient forecast data");
      weatherDataValid = 0;
      http.end();
      return false;
    }
    float mjToday = radiation[0];
    float mjTomorrow = radiation[1];
    float mjDay2 = radiation[2];
    pKwHrToday = (mjToday * MJ_TO_KWH_CONVERSION / STC_IRRADIANCE) * SolarWatts * performanceRatio;
    pKwHrTomorrow = (mjTomorrow * MJ_TO_KWH_CONVERSION / STC_IRRADIANCE) * SolarWatts * performanceRatio;
    pKwHr2days = (mjDay2 * MJ_TO_KWH_CONVERSION / STC_IRRADIANCE) * SolarWatts * performanceRatio;
    UVToday = mjToday * MJ_TO_KWH_CONVERSION;
    UVTomorrow = mjTomorrow * MJ_TO_KWH_CONVERSION;
    UVDay2 = mjDay2 * MJ_TO_KWH_CONVERSION;
    // Order is load-bearing: stamp the fetch age BEFORE publishing validity. analyzeWeatherMode()
    // runs on the other core and voids any forecast whose stamp is stale or 0, so a valid-but-
    // unstamped instant is enough to throw this forecast away for the whole next interval.
    weatherLastUpdate = millis();   // was never written — the freshness gate measured uptime, not fetch age
    weatherFetchEpoch = time(NULL);  // the ledger maps this fetch's "tomorrow"/"day 2" onto calendar days by the local day it was made
    weatherDataValid = 1;
    weatherLastError[0] = '\0';
    nextWeatherUpdate = millis() + WeatherUpdateInterval;   // honor the user's interval (hardcoded 1 h ignored it)
    Serial.printf("Solar forecast: Today=%.1f, Tomorrow=%.1f, Day2=%.1f kWh\n",
                  pKwHrToday, pKwHrTomorrow, pKwHr2days);
    queueConsoleMessageF("Weather updated: %.1f kWh today", pKwHrToday);


    http.end();
    return true;
  } else {
    snprintf(weatherLastError, sizeof(weatherLastError), "HTTP error: %d", httpResponseCode);
    weatherDataValid = 0;
    Serial.printf("HTTP error: %d\n", httpResponseCode);
    http.end();
    return false;
  }
}

void analyzeWeatherMode() {
  if (otaInProgress) {
    return;
  }
  // A forecast the device can no longer refresh must not keep the alternator off. currentWeatherMode
  // is only written here, so without this release it latches at 1 through AP mode, WiFi loss, or a
  // run of failed fetches — the field stays down with nothing left to re-evaluate it. Fail toward
  // charging. weatherLastUpdate == 0 means nothing was fetched this boot (pKwHr* are not persisted).
  if (weatherDataValid
      && (weatherLastUpdate == 0
          || (millis() - weatherLastUpdate) >= (unsigned long)WeatherUpdateInterval + WeatherStaleGraceMs)) {
    weatherDataValid = 0;
    queueConsoleMessageF("Weather: forecast too old - solar pause released");
  }
  // The bar a day must clear: the owner's High Solar Threshold, or — with the toggle on and consumption
  // history in the ledger — predicted consumption plus margin, so "enough sun" means enough for THIS boat.
  // Computed before the validity gate so the settings page can always show which bar is in force.
  float need = UVThresholdHigh;
  sledNeedSource = 0;
  if (solarUseConsEnable == 1 && !isnan(sledLive.predConsKwh)) {
    need = sledLive.predConsKwh * (1.0f + solarConsMarginPct / 100.0f);
    sledNeedSource = 1;
  }
  sledNeedKwh = need;
  // 2 or more of the next 3 days clearing the bar sets high-solar mode (1), which rests the alternator.
  if (!weatherDataValid || !weatherModeEnabled) {
    currentWeatherMode = 0;
    return;
  }
  int highUVDays = 0;
  if (pKwHrToday >= need) highUVDays++;
  if (pKwHrTomorrow >= need) highUVDays++;
  if (pKwHr2days >= need) highUVDays++;

  if (highUVDays >= 2) {
    currentWeatherMode = 1;  // Disable alternator
  } else {
    currentWeatherMode = 0;  // Normal operation
  }
}
void updateWeatherMode() {
  if (otaInProgress) {
    return;
  }
  if (!weatherModeEnabled) {
    currentWeatherMode = 0;
    return;
  }

  unsigned long now = millis();

  // Analysis (and the stale release inside it) is local arithmetic — it runs in every mode.
  analyzeWeatherMode();

  if (weatherDataValid && (now - weatherLastUpdate < (unsigned long)WeatherUpdateInterval)) {
    return;  // forecast still fresh — nothing to fetch
  }

  // Everything below reaches the internet.
  if (currentMode != MODE_CLIENT || WiFi.status() != WL_CONNECTED) {
    return;
  }

  // Internet fetch requires field off for 75s. Do not advance nextWeatherUpdate
  // while blocked — it fires promptly once the gate opens.
  if (!fieldOffSettled(15000)) {
    return;
  }

  // Signed delta survives the 49.7-day millis() rollover
  if ((int32_t)(now - nextWeatherUpdate) >= 0) {
    HttpsRequest req = { .type = HTTPS_FETCH_WEATHER };
    if (xQueueSend(httpsQueue, &req, 0) == pdTRUE) {
      nextWeatherUpdate = now + WeatherUpdateInterval;
    } else {
      nextWeatherUpdate = now + 2000;
    }
  }
}

// ── Solar energy ledger ─────────────────────────────────────────────────────────────────────────
// Globals, record layouts and the SLED_* constants are in Xregulator.ino next to the weather block.

// Forecast kWh at performance ratio 1.0 for a day's irradiance (kWh/m²): array watts / STC 1000 W/m².
static inline float solarIrrToKwh(float uvKwhM2) { return uvKwhM2 * (float)SolarWatts / STC_IRRADIANCE; }
static inline uint32_t sledLocalDay(time_t ep) { return (uint32_t)((ep + usageTzOffsetS) / 86400); }
static inline bool sledClockValid(time_t ep) { return ep > 1700000000LL; }

// Re-derive the three displayed predictions from the held irradiance — needed whenever the ratio or
// array watts change between fetches (learning step, settings page), else the table lags a fetch.
void weatherRecomputePredicted() {
  pKwHrToday    = solarIrrToKwh(UVToday) * performanceRatio;
  pKwHrTomorrow = solarIrrToKwh(UVTomorrow) * performanceRatio;
  pKwHr2days    = solarIrrToKwh(UVDay2) * performanceRatio;
}

void solarLedgerInit() {
  if (!sledRing) {
    sledRing = (SolarLedgerRec *)ps_malloc(SLED_SIZE * sizeof(SolarLedgerRec));
    if (!sledRing) { Serial.println("FATAL: sledRing ps_malloc failed"); return; }
    memset(sledRing, 0, SLED_SIZE * sizeof(SolarLedgerRec));
  }
  uint32_t n = readPsramBlob(SLED_PATH, SLED_MAGIC, SLED_VER, sledRing, sizeof(SolarLedgerRec), SLED_SIZE, NULL, false);
  sledCount = (uint16_t)n;
  sledHead  = (n >= SLED_SIZE) ? 0 : (uint16_t)n;
  SolarLedgerLive lv = {};
  if (readPsramBlob(SLED_LIVE_PATH, SLED_MAGIC ^ 1u, SLED_VER, &lv, sizeof(SolarLedgerLive), 1, NULL, false) == 1) {
    sledLive = lv;   // the day in progress resumes with its midnight baselines intact
  } else {
    memset(&sledLive, 0, sizeof(sledLive));
    sledLive.predHarvKwh = sledLive.predIrrKwh = sledLive.predConsKwh = NAN;
  }
  Serial.printf("solarLedgerInit: %u days restored, live day %lu\n", (unsigned)n, (unsigned long)sledLive.dayIdx);
}

// Median of the newest SLED_PRED_MEDIAN_N complete days within SLED_PRED_WINDOW days of `today`; the
// mean when fewer exist; NAN with none. A median ignores the one night the inverter ran — a mean can't.
float solarLedgerPredictCons(uint32_t today) {
  float v[SLED_PRED_MEDIAN_N];
  int n = 0;
  for (int i = 0; i < (int)sledCount && n < SLED_PRED_MEDIAN_N; i++) {
    const SolarLedgerRec &r = sledRing[(sledHead + SLED_SIZE - 1 - i) % SLED_SIZE];   // newest first
    if (r.dayIdx + SLED_PRED_WINDOW < today) break;
    if (isnan(r.actConsKwh) || (r.flags & SLED_F_PARTIAL)) continue;
    v[n++] = r.actConsKwh;
  }
  if (n == 0) return NAN;
  if (n < SLED_PRED_MEDIAN_N) { float sum = 0; for (int i = 0; i < n; i++) sum += v[i]; return sum / n; }
  for (int i = 1; i < n; i++) { float t = v[i]; int j = i - 1; while (j >= 0 && v[j] > t) { v[j + 1] = v[j]; j--; } v[j + 1] = t; }
  return v[n / 2];
}

int sledCompleteDays() {
  int c = 0;
  for (int i = 0; i < (int)sledCount; i++) if (!(sledRing[i].flags & SLED_F_PARTIAL)) c++;
  return c;
}

// Live so-far values for the day in progress (CSV2 + the CSV export's last row).
float sledLiveActHarvKwh() {
  if (sledLive.dayIdx == 0 || !(sledLive.flags & SLED_F_VEDIRECT)) return NAN;
  double wh = SolarChargedEnergy_AllTime - sledLive.baseSolarWh;
  return wh < 0 ? 0.0f : (float)(wh / 1000.0);
}
float sledLiveAltKwh() {
  if (sledLive.dayIdx == 0) return NAN;
  double wh = AlternatorChargedEnergy_AllTime - sledLive.baseAltWh;
  return wh < 0 ? 0.0f : (float)(wh / 1000.0);
}
// Consumption needs the battery terms (shunt) and every source on the bus: solar is a source only a
// VE.Direct link can see, so an array with no link leaves consumption unmeasurable — NAN, not a low number.
static bool sledConsMeasurable(uint8_t flags) {
  return (flags & SLED_F_SHUNT) && HAS_BATT_SHUNT && ((flags & SLED_F_VEDIRECT) || SolarWatts <= 0);
}
float sledLiveActConsKwh() {
  if (sledLive.dayIdx == 0 || !sledConsMeasurable(sledLive.flags)) return NAN;
  double solarWh = SolarChargedEnergy_AllTime - sledLive.baseSolarWh;       if (solarWh < 0) solarWh = 0;
  double altWh   = AlternatorChargedEnergy_AllTime - sledLive.baseAltWh;    if (altWh < 0) altWh = 0;
  double chgWh   = ChargedEnergy_AllTime - sledLive.baseChgWh;              if (chgWh < 0) chgWh = 0;
  double disWh   = DischargedEnergy_AllTime - sledLive.baseDischgWh;        if (disWh < 0) disWh = 0;
  double consWh  = altWh + solarWh + disWh - chgWh;
  return consWh < 0 ? 0.0f : (float)(consWh / 1000.0);
}
int sledTele(float kwh) { return isnan(kwh) ? -1 : (int)lroundf(kwh * 100.0f); }   // CSV2 encoding: -1 = unknown

static void solarLedgerOpenDay(uint32_t today) {
  SolarLedgerLive &L = sledLive;
  memset(&L, 0, sizeof(L));
  L.dayIdx = today;
  L.baseSolarWh  = SolarChargedEnergy_AllTime;
  L.baseAltWh    = AlternatorChargedEnergy_AllTime;
  L.baseChgWh    = ChargedEnergy_AllTime;
  L.baseDischgWh = DischargedEnergy_AllTime;
  L.predHarvKwh = L.predIrrKwh = NAN;
  // Which forecast slot named this day depends on when the last fetch happened: yesterday's fetch
  // called it "tomorrow", the day before's called it "day 2". Older than that is no forecast. Keyed on
  // the fetch stamp, not weatherDataValid — a forecast too old to HOLD a pause still said what it said.
  if (sledClockValid(weatherFetchEpoch)) {
    uint32_t fcDay = sledLocalDay(weatherFetchEpoch);
    float uv = NAN;
    if (fcDay + 1 == today) uv = UVTomorrow;
    else if (fcDay + 2 == today) uv = UVDay2;
    else if (fcDay == today) { uv = UVToday; L.flags |= SLED_F_SAMEDAY; }
    if (!isnan(uv)) {
      L.predIrrKwh  = solarIrrToKwh(uv);
      L.predHarvKwh = L.predIrrKwh * performanceRatio;
      L.flags |= SLED_F_FORECAST;
    }
  }
  L.predConsKwh = solarLedgerPredictCons(today);
  if (HAS_BATT_SHUNT) L.flags |= SLED_F_SHUNT;
  sledLiveDirty = true;
}

static void solarLedgerCloseDay() {
  SolarLedgerLive &L = sledLive;
  if (L.dayIdx == 0) return;
  SolarLedgerRec r = {};
  r.dayIdx      = L.dayIdx;
  r.predHarvKwh = L.predHarvKwh;
  r.predIrrKwh  = L.predIrrKwh;
  r.predConsKwh = L.predConsKwh;
  r.coverageMin = L.coverageMin;
  r.flags       = L.flags;
  if (r.coverageMin < SLED_MIN_COVER_MIN) r.flags |= SLED_F_PARTIAL;
  r.altKwh      = sledLiveAltKwh();
  r.actHarvKwh  = sledLiveActHarvKwh();
  r.actConsKwh  = sledLiveActConsKwh();
  r.ratioAfter  = NAN;
  // Learn: the day's actual/forecast ratio blended slowly into performanceRatio. Only a full day with
  // a real forecast, a live VE.Direct link and a meaningful promise can teach anything.
  if (solarLearnEnable == 1 && (r.flags & SLED_F_FORECAST) && !(r.flags & SLED_F_PARTIAL)
      && !isnan(r.actHarvKwh) && r.predIrrKwh >= SLED_MIN_IRR_KWH) {
    float dayRatio = r.actHarvKwh / r.predIrrKwh;
    if (dayRatio < SLED_RATIO_MIN) dayRatio = SLED_RATIO_MIN;
    if (dayRatio > SLED_RATIO_MAX) dayRatio = SLED_RATIO_MAX;
    float a = solarLearnRatePct / 100.0f;
    if (a < 0.0f) a = 0.0f;
    if (a > 0.5f) a = 0.5f;
    performanceRatio = (1.0f - a) * performanceRatio + a * dayRatio;
    if (performanceRatio < SLED_RATIO_MIN) performanceRatio = SLED_RATIO_MIN;
    if (performanceRatio > SLED_RATIO_MAX) performanceRatio = SLED_RATIO_MAX;
    weatherRecomputePredicted();
    r.ratioAfter = performanceRatio;
    r.flags |= SLED_F_LEARNED;
    sledRatioDirty = true;    // NVS write waits for field-cut in the service
    settingsDirty = true;     // CSV3 echoes the new ratio now
    queueConsoleMessageF("Solar: day ratio %.2f, performance ratio now %.2f", dayRatio, performanceRatio);
  }
  sledRing[sledHead] = r;
  sledHead = (sledHead + 1) % SLED_SIZE;
  if (sledCount < SLED_SIZE) sledCount++;
  sledDirty = true;
  L.dayIdx = 0;
}

// The flash writers. Ring + live-day blobs to LittleFS, learned ratio to NVS. Callers own the gate:
// the service only reaches here with the field gate cut; the shutdown/restart paths accept the stall.
static void sledWrite(bool ring) {
  if (!sledRing) return;
  if (ring) {
    uint16_t start = (sledCount < SLED_SIZE) ? 0 : sledHead;
    writePsramBlob(SLED_PATH, SLED_MAGIC, SLED_VER, 0, sledRing, sizeof(SolarLedgerRec), SLED_SIZE, start, sledCount);
    sledDirty = false;
  }
  writePsramBlob(SLED_LIVE_PATH, SLED_MAGIC ^ 1u, SLED_VER, 0, &sledLive, sizeof(SolarLedgerLive), 1, 0, 1);
  sledLiveDirty = false;
  if (sledRatioDirty) {
    settingWrite(NK_performanceRatio, String(performanceRatio, 3).c_str());
    sledRatioDirty = false;
  }
}
void solarLedgerFlushNow() {   // shutdown / maintenance-restart paths only
  if (!sledRing || (!sledDirty && !sledLiveDirty && !sledRatioDirty)) return;
  sledWrite(sledDirty);
}

// 1 Hz. Rolls the local day (a handful of float ops at midnight), ticks coverage once a minute, and
// flushes to flash only with the field gate physically cut (fieldCutSettled — never the duty-based
// fieldOffSettled, which reads "off" during a live duty-0 CV hold). Every other pass is one compare.
void solarLedgerService() {
  static uint32_t lastTickMs = 0, lastMinuteMs = 0, lastLiveFlushMs = 0;
  uint32_t now = millis();
  if (now - lastTickMs < 1000) return;
  lastTickMs = now;
  if (!sledRing) return;
  time_t ep = time(NULL);
  if (sledClockValid(ep)) {
    uint32_t today = sledLocalDay(ep);
    if (sledLive.dayIdx != 0 && sledLive.dayIdx != today) solarLedgerCloseDay();
    if (sledLive.dayIdx == 0) {
      solarLedgerOpenDay(today);
      lastMinuteMs = now;
    } else if (now - lastMinuteMs >= 60000) {
      lastMinuteMs = now;
      if (sledLive.coverageMin < 1440) sledLive.coverageMin++;
      if (dataTimestamps[IDX_VICTRON_SOLAR] != 0 && now - dataTimestamps[IDX_VICTRON_SOLAR] < 600000UL) sledLive.flags |= SLED_F_VEDIRECT;
      if (!HAS_BATT_SHUNT) sledLive.flags &= ~SLED_F_SHUNT;   // a shunt unconfigured mid-day voids the day's consumption
      // A forecast that only arrived after midnight (late boot) still names today — take it once, flagged.
      if (!(sledLive.flags & SLED_F_FORECAST) && sledClockValid(weatherFetchEpoch) && sledLocalDay(weatherFetchEpoch) == today) {
        sledLive.predIrrKwh  = solarIrrToKwh(UVToday);
        sledLive.predHarvKwh = sledLive.predIrrKwh * performanceRatio;
        sledLive.flags |= SLED_F_FORECAST | SLED_F_SAMEDAY;
      }
      sledLiveDirty = true;
    }
  }
  if (!fieldCutSettled(10000)) return;
  bool liveDue = sledLiveDirty && (lastLiveFlushMs == 0 || now - lastLiveFlushMs >= SLED_LIVE_FLUSH_MS);
  if (!sledDirty && !liveDue && !sledRatioDirty) return;
  sledWrite(sledDirty);
  lastLiveFlushMs = now;
}

// /get?ResetSolarLedger — flags only; the flash write waits for field-cut in the service. Today
// reopens on the next tick with fresh baselines and no history to predict from.
void solarLedgerClear() {
  sledCount = 0;
  sledHead = 0;
  sledLive.dayIdx = 0;
  sledDirty = true;
  sledLiveDirty = true;
}

// /solarledger.csv — every closed day oldest-first, then the day in progress (flag SLED_F_LIVE).
// Empty cell = unknown (NAN). Streamed straight from PSRAM, no buffer.
static void sledCell(char *buf, size_t n, float v) {
  if (isnan(v)) { buf[0] = '\0'; return; }
  snprintf(buf, n, "%.3f", v);
}
void solarLedgerCsvSend(AsyncWebServerRequest *request) {
  struct St { uint32_t total, base, idx; bool done; char line[200]; int len, pos; };
  St st;
  st.total = sledRing ? sledCount : 0;
  st.base  = (sledHead + SLED_SIZE - st.total) % SLED_SIZE;
  st.idx = 0; st.done = false; st.len = 0; st.pos = 0;
  AsyncWebServerResponse *response = request->beginChunkedResponse("text/csv",
    [st](uint8_t *buf, size_t maxLen, size_t) mutable -> size_t {
      size_t written = 0;
      while (written < maxLen) {
        if (st.pos >= st.len) {
          if (st.done) return written;
          if (st.idx == 0) {
            time_t ep = time(NULL);
            st.len = snprintf(st.line, sizeof(st.line), "# sledger v1 today=%lu tz=%ld n=%lu ratio=%.3f need=%.2f needSrc=%d learn=%d useCons=%d\n",
                              (unsigned long)(sledClockValid(ep) ? sledLocalDay(ep) : 0), (long)usageTzOffsetS,
                              (unsigned long)st.total, performanceRatio, sledNeedKwh, sledNeedSource,
                              solarLearnEnable, solarUseConsEnable);
          } else if (st.idx == 1) {
            st.len = snprintf(st.line, sizeof(st.line), "day,predHarv,predIrr,actHarv,predCons,actCons,alt,ratio,covMin,flags\n");
          } else {
            uint32_t i = st.idx - 2;
            SolarLedgerRec r;
            if (i < st.total) {
              r = sledRing[(st.base + i) % SLED_SIZE];
            } else if (i == st.total && sledLive.dayIdx != 0) {
              memset(&r, 0, sizeof(r));
              r.dayIdx = sledLive.dayIdx; r.predHarvKwh = sledLive.predHarvKwh; r.predIrrKwh = sledLive.predIrrKwh;
              r.predConsKwh = sledLive.predConsKwh; r.actHarvKwh = sledLiveActHarvKwh(); r.actConsKwh = sledLiveActConsKwh();
              r.altKwh = sledLiveAltKwh(); r.ratioAfter = NAN; r.coverageMin = sledLive.coverageMin;
              r.flags = sledLive.flags | SLED_F_LIVE;
            } else { st.done = true; return written; }
            char c1[16], c2[16], c3[16], c4[16], c5[16], c6[16], c7[16];
            sledCell(c1, sizeof(c1), r.predHarvKwh); sledCell(c2, sizeof(c2), r.predIrrKwh); sledCell(c3, sizeof(c3), r.actHarvKwh);
            sledCell(c4, sizeof(c4), r.predConsKwh); sledCell(c5, sizeof(c5), r.actConsKwh); sledCell(c6, sizeof(c6), r.altKwh);
            sledCell(c7, sizeof(c7), r.ratioAfter);
            st.len = snprintf(st.line, sizeof(st.line), "%lu,%s,%s,%s,%s,%s,%s,%s,%u,%u\n",
                              (unsigned long)r.dayIdx, c1, c2, c3, c4, c5, c6, c7, (unsigned)r.coverageMin, (unsigned)r.flags);
          }
          if (st.len > (int)sizeof(st.line) - 1) st.len = sizeof(st.line) - 1;
          st.idx++; st.pos = 0;
        }
        size_t tw = min((size_t)(st.len - st.pos), maxLen - written);
        memcpy(buf + written, st.line + st.pos, tw);
        written += tw; st.pos += (int)tw;
      }
      return written;
    });
  response->addHeader("Cache-Control", "no-cache");
  request->send(response);
}

void initWeatherModeSettings() {
  if (!settingExists(NK_LatitudeNMEA)) {
    settingWrite(NK_LatitudeNMEA, "0.0");
  } else {
    LatitudeNMEA = settingRead(NK_LatitudeNMEA).toDouble();
  }
  if (!settingExists(NK_LongitudeNMEA)) {
    settingWrite(NK_LongitudeNMEA, "0.0");
  } else {
    LongitudeNMEA = settingRead(NK_LongitudeNMEA).toDouble();
  }
  // Sticky manual GPS override (Weather Mode). Restored across reboot.
  if (!settingExists(NK_LatitudeManual)) {
    settingWrite(NK_LatitudeManual, "0.0");
  } else {
    LatitudeManual = settingRead(NK_LatitudeManual).toDouble();
  }
  if (!settingExists(NK_LongitudeManual)) {
    settingWrite(NK_LongitudeManual, "0.0");
  } else {
    LongitudeManual = settingRead(NK_LongitudeManual).toDouble();
  }
  if (!settingExists(NK_gpsManualActive)) {
    settingWrite(NK_gpsManualActive, "0");
  } else {
    gpsManualActive = (settingRead(NK_gpsManualActive).toInt() == 1);
  }
  if (gpsManualActive) {  // apply immediately so weather has coords before first resolveSources()
    LatitudeNMEA  = LatitudeManual;
    LongitudeNMEA = LongitudeManual;
  }
  if (!settingExists(NK_weatherModeEnabled)) {
    settingWrite(NK_weatherModeEnabled, String(weatherModeEnabled).c_str());
  } else {
    weatherModeEnabled = settingRead(NK_weatherModeEnabled).toInt();
  }
  if (!settingExists(NK_UVThresholdHigh)) {
    settingWrite(NK_UVThresholdHigh, String(UVThresholdHigh, 1).c_str());
  } else {
    UVThresholdHigh = settingRead(NK_UVThresholdHigh).toFloat();
  }
  if (!settingExists(NK_performanceRatio)) {
    settingWrite(NK_performanceRatio, String(performanceRatio, 2).c_str());
  } else {
    performanceRatio = settingRead(NK_performanceRatio).toFloat();
  }
  if (!settingExists(NK_solarLearnEnable)) {
    settingWrite(NK_solarLearnEnable, String(solarLearnEnable).c_str());
  } else {
    solarLearnEnable = settingRead(NK_solarLearnEnable).toInt();
  }
  if (!settingExists(NK_solarUseConsEnable)) {
    settingWrite(NK_solarUseConsEnable, String(solarUseConsEnable).c_str());
  } else {
    solarUseConsEnable = settingRead(NK_solarUseConsEnable).toInt();
  }
  if (!settingExists(NK_solarConsMarginPct)) {
    settingWrite(NK_solarConsMarginPct, String(solarConsMarginPct, 1).c_str());
  } else {
    solarConsMarginPct = settingRead(NK_solarConsMarginPct).toFloat();
  }
  if (!settingExists(NK_solarLearnRatePct)) {
    settingWrite(NK_solarLearnRatePct, String(solarLearnRatePct, 1).c_str());
  } else {
    solarLearnRatePct = settingRead(NK_solarLearnRatePct).toFloat();
  }
  if (!settingExists(NK_SolarWatts)) {
    settingWrite(NK_SolarWatts, String(SolarWatts).c_str());
  } else {
    SolarWatts = settingRead(NK_SolarWatts).toInt();
  }
  if (!settingExists(NK_weatherDataValid)) {
    settingWrite(NK_weatherDataValid, "0");
  } else {
    weatherDataValid = settingRead(NK_weatherDataValid).toInt();
  }
  if (!settingExists(NK_WeatherUpdateInterval)) {
    settingWrite(NK_WeatherUpdateInterval, String(WeatherUpdateInterval).c_str());
  } else {
    WeatherUpdateInterval = settingRead(NK_WeatherUpdateInterval).toInt();
  }
  if (!settingExists(NK_WeatherTimeoutMs)) {
    settingWrite(NK_WeatherTimeoutMs, String(WeatherTimeoutMs).c_str());
  } else {
    WeatherTimeoutMs = settingRead(NK_WeatherTimeoutMs).toInt();
  }
}


void triggerWeatherUpdate() {
  if (LatitudeNMEA == 0.0 && LongitudeNMEA == 0.0) {
    queueConsoleMessageF("Weather: Failed - No GPS lock");
    return;
  }

  if (WiFi.RSSI() < -80) {
    queueConsoleMessageF("Weather: Failed - Weak WiFi signal");
    return;
  }

  if (currentMode != MODE_CLIENT) {
    queueConsoleMessageF("Weather: Failed - Not in client mode");
    return;
  }

  HttpsRequest req = { .type = HTTPS_FETCH_WEATHER };
  if (xQueueSend(httpsQueue, &req, 0) == pdTRUE) {
    queueConsoleMessageF("Weather: Update queued");
    nextWeatherUpdate = millis() + WeatherUpdateInterval;
  } else {
    queueConsoleMessageF("Weather: Failed - Queue full, retry in 1s");
  }
}

void printTempDebugStatus() {
  unsigned long age = millis() - tempLastSuccessMillis;
  Serial.printf(
    "TempDbg: lastGood=%.2fF age=%lums ok=%lu readFail=%lu crcFail=%lu crcRec=%lu allFF=%lu 85C=%lu oor=%lu reqFail=%lu connFail=%lu resFix=%lu rereadFail=%lu resFixCrcFail=%lu enumFail=%lu coreBusySkip=%lu intervalSkip=%lu\n",
    tempLastGoodF, age,
    (unsigned long)tempReadSuccessCount,
    (unsigned long)tempReadFailCount,
    (unsigned long)tempCrcFailCount,
    (unsigned long)tempCrcRecoveredCount,
    (unsigned long)tempAllFFCount,
    (unsigned long)tempPowerOn85Count,
    (unsigned long)tempOutOfRangeCount,
    (unsigned long)tempRequestFailCount,
    (unsigned long)tempConnectedFailCount,
    (unsigned long)tempResolutionFixCount,
    (unsigned long)tempRereadFailCount,
    (unsigned long)tempResolutionFixCrcFailCount,
    (unsigned long)tempEnumerateFailCount,
    (unsigned long)tempCoreBusySkipCount,
    (unsigned long)tempStaleSkipCount);
}


void checkTempTaskHealth() {
  if (otaInProgress) {
    return;
  }
  if (tempTaskSuspended) return;  // Temp task intentionally suspended in 80MHz engine-off idle — not a hang
  static unsigned long lastTempHealthCheck = 0;
  unsigned long now = millis();

  if (now - lastTempHealthCheck < 5000) return;
  lastTempHealthCheck = now;

  if (now - lastTempTaskHeartbeat > TEMP_TASK_TIMEOUT) {
    if (tempTaskHealthy) {  // First time detecting the problem
      tempTaskHealthy = false;
      tempTaskAlarm = true;
      queueConsoleMessageF("CRITICAL: TempTask hung up - task not responding for %lu seconds", (now - lastTempTaskHeartbeat) / 1000);

      // Field cut is handled by the regulation loop: tempDataVeryStale fires at 20s of no
      // MARK_FRESH call, which produces REASON_TEMP_STALE → MODE_CRITICAL_RAMP → GPIO4 cut.
    }
  } else {
    if (!tempTaskHealthy) {  // Was unhealthy, now recovered
      tempTaskHealthy = true;
      tempTaskAlarm = false;
      queueConsoleMessage("TempTask: Recovered and responding normally");
      // Field re-enable is automatic: regulation loop re-enables GPIO4 as soon as
      // tempDataVeryStale clears, which happens on the first MARK_FRESH after a good read.
    }
  }
}
bool writeINA228Register(uint8_t i2cAddress, uint8_t reg, uint16_t value) {
  Wire.beginTransmission(i2cAddress);
  Wire.write(reg);
  Wire.write(value >> 8);
  Wire.write(value & 0xFF);
  return (Wire.endTransmission() == 0);
}


// ===== IMU Initialization =====
bool i2cProbe8bit(uint8_t addr8) {
  uint8_t addr7 = addr8 >> 1;
  Wire.beginTransmission(addr7);
  uint8_t err = Wire.endTransmission(true);
  return (err == 0);
}
void imuInit() {
  Serial.println("Initializing LSM6DSOX IMU...");
  Serial.print("Using I2C address: 0x");
  Serial.print(LSM6DSOX_ADDR, HEX);
  Serial.println(" (8-bit ST format)");
  Serial.print("IMUSample size: ");
  Serial.print(sizeof(IMUSample));
  Serial.println(" bytes");

  if (!i2cProbe8bit(LSM6DSOX_ADDR)) {
    Serial.println("ERROR: No I2C ACK from LSM6DSOX address - sensor not present");
    Serial.println("IMU disabled - will retry on next boot");
    imuEnabled = false;
    return;
  }
  Serial.println("I2C ACK detected at LSM6DSOX address");

  uint8_t who = 0;
  Wire.beginTransmission(LSM6DSOX_ADDR >> 1);
  Wire.write(0x0F);  // WHO_AM_I register
  Wire.endTransmission(false);
  Wire.requestFrom((int)(LSM6DSOX_ADDR >> 1), 1);
  if (Wire.available()) {
    who = Wire.read();
  }

  Serial.print("LSM6DSOX WHO_AM_I = 0x");
  Serial.println(who, HEX);

  if (who != 0x6C) {
    Serial.print("ERROR: WHO_AM_I mismatch (expected 0x6C, got 0x");
    Serial.print(who, HEX);
    Serial.println(") - not an LSM6DSOX");
    Serial.println("IMU disabled - check hardware");
    imuEnabled = false;
    return;
  }
  Serial.println("WHO_AM_I verified - LSM6DSOX detected");

  if (imu.begin() != LSM6DSOX_OK) {
    Serial.println("ERROR: LSM6DSOX library init failed");
    queueConsoleMessageF("IMU library init failed");
    imuEnabled = false;
    return;
  }

  if (imu.Enable_X() != LSM6DSOX_OK) {
    Serial.println("ERROR: Failed to enable accelerometer");
    queueConsoleMessageF("IMU: failed to enable accelerometer");
    imuEnabled = false;
    return;
  }

  if (imu.Enable_G() != LSM6DSOX_OK) {
    Serial.println("ERROR: Failed to enable gyroscope");
    queueConsoleMessageF("IMU: failed to enable gyroscope");
    imuEnabled = false;
    return;
  }

  // Set ODRs.
  // Accel at 104 Hz is plenty for slam peak capture (~9.6 ms sample spacing vs 20–50 ms
  // typical slam pulse), pitch/heel max, wave-period decimation (10 Hz output), and MSI
  // frequency-weighted Z (peaks 0.5–5 Hz). Engine-vibration diagnostics are out of scope
  // (board is not engine-mounted). ACCEL_INTERVAL_US in drainIMUFifo() must match: 9615 µs.
  if (imu.Set_X_ODR(104.0f) != LSM6DSOX_OK) {
    Serial.println("ERROR: Failed to set accel ODR");
    queueConsoleMessageF("IMU: failed to set accel ODR");
    imuEnabled = false;
    return;
  }

  if (imu.Set_G_ODR(52.0f) != LSM6DSOX_OK) {
    Serial.println("ERROR: Failed to set gyro ODR");
    queueConsoleMessageF("IMU: failed to set gyro ODR");
    imuEnabled = false;
    return;
  }

  // Set FIFO batching rates (BDR) - must match ODRs for continuous sampling
  if (imu.Set_FIFO_X_BDR(104.0f) != LSM6DSOX_OK) {
    Serial.println("ERROR: Failed to set FIFO accel BDR");
    queueConsoleMessageF("IMU: failed to set FIFO accel BDR");
    imuEnabled = false;
    return;
  }

  if (imu.Set_FIFO_G_BDR(52.0f) != LSM6DSOX_OK) {
    Serial.println("ERROR: Failed to set FIFO gyro BDR");
    queueConsoleMessageF("IMU: failed to set FIFO gyro BDR");
    imuEnabled = false;
    return;
  }


  // COMPRESSION LATER
  // Left OFF: the FIFO parser only decodes uncompressed tags, so enabling compression routes
  // every compressed sample to the unknown-tag path and stalls the drain. Needs delta-decode first.
  // // Enable FIFO compression (bonus headroom, don't rely on it)
  // if (imu.Set_FIFO_Compression_Algo_Init(1) != LSM6DSOX_OK || imu.Set_FIFO_Compression_Algo_Enable(1) != LSM6DSOX_OK || imu.Set_FIFO_Compression_Algo_Real_Time_Set(1) != LSM6DSOX_OK) {
  //   Serial.println("WARNING: Failed to enable FIFO compression (continuing anyway)");
  //   // Non-fatal - continue without compression
  // }

  if (imu.Set_FIFO_Mode(6) != LSM6DSOX_OK) {
    Serial.println("ERROR: Failed to set FIFO continuous mode");
    queueConsoleMessageF("IMU: failed to set FIFO mode");
    imuEnabled = false;
    return;
  }

  delay(50);  // Let FIFO stabilize

  // Empirical tag verification (one-time, informational only)
  Serial.println("Verifying FIFO tags...");
  delay(100);  // Let some samples accumulate
  uint16_t test_samples = 0;
  if (imu.Get_FIFO_Num_Samples(&test_samples) == LSM6DSOX_OK && test_samples > 0) {
    uint8_t testBuf[21];  // Read 3 samples to see tag variety
    uint16_t samples_to_check = (test_samples > 3) ? 3 : test_samples;
    if (imu.Get_FIFO_Sample(testBuf, samples_to_check) == LSM6DSOX_OK) {
      for (uint16_t i = 0; i < samples_to_check; i++) {
        uint8_t raw_tag = testBuf[i * 7];
        uint8_t tag_sensor = raw_tag >> 3;
        Serial.print("  Sample ");
        Serial.print(i);
        Serial.print(": raw_tag=0x");
        Serial.print(raw_tag, HEX);
        Serial.print(", tag_sensor=");
        Serial.print(tag_sensor);
        Serial.print(" (");
        if (tag_sensor == TAG_SENSOR_GYRO) {
          Serial.print("GYRO");
        } else if (tag_sensor == TAG_SENSOR_ACCEL) {
          Serial.print("ACCEL");
        } else if (tag_sensor == TAG_SENSOR_TEMP) {
          Serial.print("TEMP");
        } else {
          Serial.print("UNKNOWN");
        }
        Serial.println(")");
      }
    }
  }

  Serial.println("LSM6DSOX initialized successfully");
  Serial.println("  Accel ODR: 104 Hz, FIFO BDR: 104 Hz");
  Serial.println("  Gyro ODR: 52 Hz, FIFO BDR: 52 Hz");
  Serial.println("  FIFO mode: Continuous");
  Serial.print("  Poll interval: ");
  Serial.print(IMU_POLL_INTERVAL);
  Serial.print(" ms, Max drain: ");
  Serial.print(MAX_FIFO_DRAIN_PER_POLL);
  Serial.println(" samples");
  Serial.print("  Ring buffers: Accel ");
  Serial.print(ACCEL_RING_SIZE);
  Serial.print(" samples, Gyro ");
  Serial.print(GYRO_RING_SIZE);
  Serial.println(" samples");

  queueConsoleMessageF("IMU initialized: 104Hz accel, 52Hz gyro");

  imuEnabled = true;
  lastIMUPoll = millis();
}
void initIMUStructures() {
  memset(imuWindow, 0, sizeof(ImuWindow));

  // Initialize min/max to sentinel values (will be overwritten by first real data)
  imuWindow->accel_x_min = imuWindow->accel_y_min = imuWindow->accel_z_min = INT32_MAX;
  imuWindow->accel_x_max = imuWindow->accel_y_max = imuWindow->accel_z_max = INT32_MIN;
  imuWindow->total_accel_min = imuWindow->vertical_accel_min = INT32_MAX;
  imuWindow->total_accel_max = imuWindow->vertical_accel_max = INT32_MIN;
  imuWindow->gyro_x_min = imuWindow->gyro_y_min = imuWindow->gyro_z_min = INT32_MAX;
  imuWindow->gyro_x_max = imuWindow->gyro_y_max = imuWindow->gyro_z_max = INT32_MIN;
  imuWindow->heel_min = imuWindow->pitch_min = INT32_MAX;
  imuWindow->heel_max = imuWindow->pitch_max = INT32_MIN;

  imuWindow->lastUpdateTime_us = 0;
  imuWindow->lastGyroUpdateTime_us = 0;

  imuWindow->wave_period = -1000;  // -1.0s scaled = invalid

  Serial.println("IMU structures initialized");
}
void initializeHardware() {

  Serial.println("Starting hardware initialization...");
  Wire.end();
  Wire.begin(9, 10);
  Wire.setClock(400000);  // in-spec Fast-mode. Do not raise to 800 kHz: ADS1115 was declared disconnected there (adsI2CErrorCount tripwire).
  Wire.setTimeOut(15);   // do not lower — AsyncTCP preemption can stall I2C transactions this long
  delay(100);
  Serial.println("I2C initialized on SDA=9, SCL=10");
  delay(100);  // Give I2C time to initialize

//BMP390
if (!bmp388.begin(BMP3_ADDR)) {
  delay(50);
  if (!bmp388.begin(BMP3_ADDR)) {  // one retry rides out a transient boot-time I2C NAK
    Serial.println("BMP388 not found");
    queueConsoleMessage("WARNING: BMP388 pressure sensor failed - barometer/board-temp disabled");
    BMP388Disconnected = 1;
  }
}
if (!BMP388Disconnected) {
  bmp388.setPresOversampling(OVERSAMPLING_X32);
  bmp388.setTempOversampling(OVERSAMPLING_X2);
  bmp388.setIIRFilter(IIR_FILTER_32);   // use highest exposed by this library
  Serial.println("BMP388 found");
}

  // NMEA2K
  OutputStream = &Serial;
  //  NMEA2000.SetN2kCANReceiveFrameBufSize(50); // was commented
  // Do not forward bus messages at all
  NMEA2000.SetForwardType(tNMEA2000::fwdt_Text);
  NMEA2000.SetForwardStream(OutputStream);
  // Set false below, if you do not want to see messages parsed to HEX withing library
  NMEA2000.EnableForward(false);
  NMEA2000.SetMsgHandler(HandleNMEA2000Msg);
  // DVCC follow: raw-RX tap (RV-C single-frame DGNs + 0xEF00 VREG carrier + §8a capture ring).
  // Registered unconditionally — the tap self-gates on NMEA2KData/dvccEn and costs a few compares.
  tNMEA2000_esp32::SetRawRxHook(dvccRawFrameTap);
  initDvccCapture();
  //  NMEA2000.SetN2kCANMsgBufSize(2);
  // Transmit (producer) mode — spec: Working Markdown Docs/NMEA2K_TRANSMIT_SPEC.md. Applied at boot
  // only; the /get handler tells the user a toggle change needs a reboot. n2kTxEnable off keeps the
  // library's listen-only default: zero bus presence, no address claim, exactly the pre-TX behavior.
  // InitSystemSettings() has already run (setup order), so the n2k* globals hold NVS values here.
  if (n2kTxEnable == 1 || rvcTxEnable == 1) {  // RV-C rides the same node: one address claim, one mode
    NMEA2000.SetN2kCANSendFrameBufSize(80);  // software retry ring for frames the non-blocking _xeng driver refuses (TWAI queue full / no bus)
    uint64_t mac = ESP.getEfuseMac();
    static char n2kSerial[9];
    snprintf(n2kSerial, sizeof(n2kSerial), "%08lX", (unsigned long)(mac & 0xFFFFFFFFUL));
    NMEA2000.SetProductInformation(n2kSerial, 100, "XREG-010", FIRMWARE_VERSION, "V10",
                                   2);  // LEN 2 = 100 mA: isolated CAN side is backbone-powered (ISO1050 + MPM3610)
    NMEA2000.SetDeviceInformation((unsigned long)(mac & 0x1FFFFFUL),  // 21-bit unique number for address claim
                                  141,    // device function: DC Generator/Alternator
                                  35,     // device class: Electrical Generation
                                  2046);  // open manufacturer code (no NMEA membership)
    NMEA2000.SetDeviceInformationInstances((unsigned char)(n2kDeviceInstance & 0x07),
                                           (unsigned char)((n2kDeviceInstance >> 3) & 0x1F));
    int savedAddr = settingExists(NK_n2kSrcAddr) ? settingRead(NK_n2kSrcAddr).toInt() : 22;
    if (savedAddr < 0 || savedAddr > 251) savedAddr = 22;
    NMEA2000.SetMode(tNMEA2000::N2km_ListenAndNode, (uint8_t)savedAddr);
    if (n2kTxEnable == 1) NMEA2000.ExtendTransmitMessages(N2kTransmitMessages);  // never advertise N2K PGNs an RV-C-only node will not send
    NMEA2000.ExtendReceiveMessages(N2kReceiveMessages);
    n2kSrcAddrLive = savedAddr;
  }
  NMEA2000.Open();
  Serial.println("NMEA2K Running...");


  //Victron VeDirect and NMEA0183
  Serial1.setRxBufferSize(2048);               // must precede begin(); default 256 B overflows in the 2 s ReadVEData gap (BMV emits ~900 B/s)
  Serial1.begin(19200, SERIAL_8N1, 7, -1, 1);  // Victron VEDirect
  applyNMEA0183Serial();  // Serial2 / GPIO6 at the user's baud + polarity. InitSystemSettings() has already run, so NVS values are live here.

  // INA228 Battery Voltage/Current Sensor
  if (!INA.begin()) {
    Serial.println("Could not connect INA228. Fix and Reboot");
    queueConsoleMessage("WARNING: Could not connect INA228 Battery Voltage/Amp measuring chip");
    INADisconnected = 1;
  } else {
    INADisconnected = 0;

    // setAverage() takes raw register value: 0=1, 1=4, 2=16, 3=64, 4=128, 5=256, 6=512, 7=1024 samples
    INA.setMode(11);                       // Continuous shunt and bus voltage measurement
    INA.setAverage(4);                     // 128-sample averaging — 128 × 8.24ms = 1054ms register update
    INA.setBusVoltageConversionTime(7);    // 4120 µs conversion time
    INA.setShuntVoltageConversionTime(7);  // 4120 µs conversion time

    updateINA228OvervoltageThreshold();
    queueConsoleMessage("INA228 initialized: Hardware overvoltage protection enabled");
  }

  unsigned long now = millis();
  for (int i = 0; i < MAX_DATA_INDICES; i++) {
    dataTimestamps[i] = now;
  }

  // if (setupDisplay()) {
  //   Serial.println("Display ready for use");
  // } else {
  //   Serial.println("Continuing without display");
  // }

  //ADS1115
  if (!adc.testConnection()) {
    Serial.println("ADS1115 Connection failed and would have triggered a return if it wasn't commented out");
    queueConsoleMessage("WARNING: ADS1115 Analog Input chip failed");
    ADS1115Disconnected = 1;
    // return;
  } else {
    ADS1115Disconnected = 0;
  }
  adc.setGain(ADS1115_REG_CONFIG_PGA_6_144V);       // ±6.144V range
  adc.setSampleRate(ADS1115_REG_CONFIG_DR_860SPS);  // 860 SPS, 1.16ms per conversion

  delay(100);  // Give chip time to configure
  if (!adc.testConnection()) {
    Serial.println("ADS1115 failed after configuration");
  } else {
    Serial.println("ADS1115 configured successfully");
  }
  Wire.beginTransmission(0x48);  // ADS1115 default address
  Wire.write(0x01);              // Config register
  Wire.endTransmission(false);
  Wire.requestFrom(0x48, 2);
  if (Wire.available() >= 2) {
    uint16_t configReg = (Wire.read() << 8) | Wire.read();
    queueConsoleMessage("ADS1115 Config Register: 0x" + String(configReg, HEX));

    uint8_t mux = (configReg >> 12) & 0x07;
    uint8_t pga = (configReg >> 9) & 0x07;
    uint8_t mode = (configReg >> 8) & 0x01;

    queueConsoleMessage("ADS1115 MUX: " + String(mux) + ", PGA: " + String(pga) + ", Mode: " + String(mode));
    // Expected values:
    // MUX should be 4 (single-ended A0) when you read channel 0
    // PGA should be 0 (±6.144V range)
    // Mode should be 0 (continuous) or 1 (single-shot)
  } else {
    queueConsoleMessage("ADS1115 Config readback failed - I2C error");
  }
  //onewire
  sensors.setAutoSaveScratchPad(false);                  // RAM-only config changes (no EEPROM writes); must precede every owEnumerate() setResolution — TempTask's first pass waits on bootHardwareInitDone
  owEnumerate();                                         // sensors.begin() + bus search + role binding by stored ROM code (InitSystemSettings ran first); TempTask re-runs it once setup() sets bootHardwareInitDone
  if (sensors.getDeviceCount() == 0) {
    Serial.println("WARNING: No DS18B20 sensors found on the bus.");
    queueConsoleMessage("WARNING: No DS18B20 sensors found on the bus");
    sensors.setWaitForConversion(false);  // this is critical!
  }
  imuInit();
  initIMUStructures();
  Serial.println("Hardware initialization complete");
}
void InitSystemSettings() {  // load all settings from NVS.  If no keys exist, create them and populate with the hardcoded values
  // (the one-time LittleFS-to-NVS import sweep runs earlier, in setup(), before the alt/perf registry loaders)

  // Vessel Info lives in NVS (CONFIG_MANIFEST) so it survives a LittleFS formatOnFail and
  // exports/imports like every other setting. This must run before the NK_BatteryVoltage
  // block below, which reads the class the migration seeds.
  migrateVesselInfoFile();
  if (settingExists(NK_vesselSaved)) {
    BOAT_LENGTH_FT         = settingRead(NK_boatLenFt).toFloat();
    BOAT_DISPLACEMENT_LBS  = settingRead(NK_boatDispLbs).toFloat();
    // The three defaults below are meaningful (chemistry drives the OCV preset, boat type
    // drives the perf model), so an empty key keeps the compile-time value rather than ""
    String s = settingRead(NK_boatType);          if (s.length()) vesselSetText(BOAT_TYPE, sizeof(BOAT_TYPE), s.c_str());
    vesselSetText(BOAT_MAKE_MODEL, sizeof(BOAT_MAKE_MODEL), settingRead(NK_boatMakeModel).c_str());
    int y = settingRead(NK_boatYear).toInt();     if (y > 0) BOAT_YEAR = (uint16_t)y;
    strncpy(HOME_PORT, settingRead(NK_homePort).c_str(), sizeof(HOME_PORT) - 1);
    HOME_PORT[sizeof(HOME_PORT) - 1] = '\0';
    vesselSetText(ENGINE_MAKE, sizeof(ENGINE_MAKE), settingRead(NK_engineMake).c_str());
    ENGINE_HP              = (uint16_t)settingRead(NK_engineHp).toInt();
    s = settingRead(NK_batteryType);              if (s.length()) vesselSetText(BATTERY_TYPE, sizeof(BATTERY_TYPE), s.c_str());
    vesselSetText(BATTERY_MAKE_MODEL, sizeof(BATTERY_MAKE_MODEL), settingRead(NK_battMakeModel).c_str());
    vesselSetText(ALTERNATOR_BRAND_MODEL, sizeof(ALTERNATOR_BRAND_MODEL), settingRead(NK_altBrandModel).c_str());
    // Unclamped, an out-of-range value indexes past axisRemap[] and wild-reads through src[]
    imuMountOrientation    = (uint8_t)constrain(settingRead(NK_imuMountOrient).toInt(), 0, IMU_ORIENT_COUNT - 1);
    regulatorMountLoc      = (uint8_t)constrain(settingRead(NK_regMountLoc).toInt(), 0, 1);
    IMU_DIST_BOW_FT        = settingRead(NK_imuDistBowFt).toFloat();
    IMU_DIST_CL_FT         = settingRead(NK_imuDistClFt).toFloat();
    IMU_HEIGHT_WL_FT       = settingRead(NK_imuHtWlFt).toFloat();

    vesselInfoSaved = true;
    Serial.println("Vessel info loaded from NVS");
  } else {
    Serial.println("Vessel info: never saved");
  }

  // Load IMU zero/level calibration (separate from vessel_info so a profile
  // re-save from the cloud never clobbers physical-mount calibration).
  // Stored as one JSON-string NVS value.
  if (settingExists(NK_imu_zero)) {
    DynamicJsonDocument zdoc(256);
    if (!deserializeJson(zdoc, settingRead(NK_imu_zero))) {
      imuHeelOffsetDeg = zdoc["heel_offset_deg"] | 0.0f;
      imuPitchOffsetDeg = zdoc["pitch_offset_deg"] | 0.0f;
      imuGxBias = zdoc["gx_bias"] | 0.0f;
      imuGyBias = zdoc["gy_bias"] | 0.0f;
      imuGzBias = zdoc["gz_bias"] | 0.0f;
      Serial.println("IMU zero calibration loaded");
    }
  }
  imuZeroCaptured = settingExists(NK_imu_zero);
  // Stamped by the Zero capture. Absent = zeroed by a build with no mount check, or never zeroed — either
  // way UNKNOWN, which flags uploads suspicious until the user Zeroes once on this build.
  imuMountState = settingExists(NK_imu_mnt_state)
                    ? (uint8_t)settingRead(NK_imu_mnt_state).toInt()
                    : IMU_MOUNT_UNKNOWN;

  if (!settingExists(NK_InstallId)) {
    settingWrite(NK_InstallId, String((unsigned long)esp_random(), HEX).c_str());
  }
  installId = settingRead(NK_InstallId);

  if (settingExists(NK_BatteryCapacity_Ah)) BatteryCapacity_Ah = settingRead(NK_BatteryCapacity_Ah).toInt();
  // Choke point for every ingress (Vessel Info save, /get, and applyImportConfig which writes NVS raw then reboots).
  // 0 Ah zeroes the full-charge tail threshold (TailCurrent × capacity), so absorption would never end.
  BatteryCapacity_Ah = constrain(BatteryCapacity_Ah, 1, 100000);
  settingWrite(NK_BatteryCapacity_Ah, String(BatteryCapacity_Ah).c_str());   // compare-first: heals a bad import
  PeukertRatedCurrent_A = BatteryCapacity_Ah / 20.0f;  // derived; recomputed every run regardless of branch
  if (!settingExists(NK_ChargeEfficiency)) {
    // Save user-readable form (e.g. "99.0"), NOT the scaled integer, so the load path always
    // sees a value ≤ 100 and can safely apply × 10 to reconstruct the scaled integer.
    settingWrite(NK_ChargeEfficiency, String(ChargeEfficiency_scaled / 10.0f, 1).c_str());
  } else {
    // Key stores the user-readable percentage string (e.g. "99.0"). Multiply by 10 to get scaled int.
    ChargeEfficiency_scaled = (int)round(settingRead(NK_ChargeEfficiency).toFloat() * 10);
  }
  if (!settingExists(NK_TailCurrent)) {
    settingWrite(NK_TailCurrent, String(TailCurrent).c_str());
  } else {
    TailCurrent = settingRead(NK_TailCurrent).toFloat();
  }
  if (!settingExists(NK_FuelEfficiency)) {
    settingWrite(NK_FuelEfficiency, String(FuelEfficiency_scaled).c_str());
  } else {
    FuelEfficiency_scaled = settingRead(NK_FuelEfficiency).toInt();
  }
  if (!settingExists(NK_TemperatureLimitF)) {
    settingWrite(NK_TemperatureLimitF, String(TemperatureLimitF).c_str());
  } else {
    TemperatureLimitF = settingRead(NK_TemperatureLimitF).toInt();
  }
  if (!settingExists(NK_ManualDutyTarget)) {
    settingWrite(NK_ManualDutyTarget, String(ManualDutyTarget, 2).c_str());
  } else {
    ManualDutyTarget = settingRead(NK_ManualDutyTarget).toFloat();
  }
  if (!settingExists(NK_capLimitMode)) {
    settingWrite(NK_capLimitMode, String(capLimitMode).c_str());
  } else {
    capLimitMode = constrain(settingRead(NK_capLimitMode).toInt(), 0, 1);
  }
  // System voltage class (12/24/36/48). The CV/CC gain normalization (recomputeCv/CcGains, called
  // at the end of this function) divides by SYSTEM_VOLTAGE_CLASS, so a zero here is fatal.
  if (!settingExists(NK_BatteryVoltage)) {
    if (SYSTEM_VOLTAGE_CLASS != 12 && SYSTEM_VOLTAGE_CLASS != 24 && SYSTEM_VOLTAGE_CLASS != 36 && SYSTEM_VOLTAGE_CLASS != 48) SYSTEM_VOLTAGE_CLASS = 12;
    settingWrite(NK_BatteryVoltage, String((int)SYSTEM_VOLTAGE_CLASS).c_str());
  } else {
    int v = settingRead(NK_BatteryVoltage).toInt();
    if (v != 12 && v != 24 && v != 36 && v != 48) v = 12;  // reject corrupt NVS value (guards vNorm = 12/SYSTEM_VOLTAGE_CLASS div-by-zero/NaN)
    SYSTEM_VOLTAGE_CLASS = (uint8_t)v;
  }
  // First-creation class scaling. Every hardcoded default below is a 12V value; volt-domain seeds
  // scale ×(V/12), duty-domain seeds ×(12/V) — the same two domains applyNominalVoltageChange
  // rescales on a live class change. Existing keys always load verbatim, so this only fires when a
  // key is first created on a device already provisioned 24/36/48V (fresh NVS with the class known, or
  // a firmware update introducing a new setting).
  const float seedVScale = (float)SYSTEM_VOLTAGE_CLASS / 12.0f;
  const float seedDScale = 12.0f / (float)SYSTEM_VOLTAGE_CLASS;
  // RAM-only volt-domain knob (posted with each sweep start, never NVS): its hardcoded default is a
  // 12V value like every seed below, and 0.15 V at 48 V is a quarter of the per-cell margin while the
  // sweep bypasses every soft protection layer — so scale it once here (runs once, at boot).
  altSweepMarginV *= seedVScale;
  if (!settingExists(NK_BulkVoltage)) {
    BulkVoltage *= seedVScale;
    settingWrite(NK_BulkVoltage, String(BulkVoltage).c_str());
  } else {
    BulkVoltage = settingRead(NK_BulkVoltage).toFloat();
  }
  if (!settingExists(NK_socInfoAvailable)) {
    settingWrite(NK_socInfoAvailable, String(socInfoAvailable).c_str());
  } else {
    socInfoAvailable = settingRead(NK_socInfoAvailable).toInt();
  }
  if (!settingExists(NK_TailCurrent_A)) {
    settingWrite(NK_TailCurrent_A, String(TailCurrent_A).c_str());
  } else {
    TailCurrent_A = settingRead(NK_TailCurrent_A).toFloat();
  }
  if (!settingExists(NK_RebulkVoltage)) {
    RebulkVoltage *= seedVScale;
    settingWrite(NK_RebulkVoltage, String(RebulkVoltage).c_str());
  } else {
    RebulkVoltage = settingRead(NK_RebulkVoltage).toFloat();
  }
  if (!settingExists(NK_rebulkDebounceTime)) {
    settingWrite(NK_rebulkDebounceTime, String(rebulkDebounceTime).c_str());
  } else {
    rebulkDebounceTime = settingRead(NK_rebulkDebounceTime).toInt();
  }
  if (!settingExists(NK_MinFloatTime)) {
    settingWrite(NK_MinFloatTime, String(MinFloatTime).c_str());
  } else {
    MinFloatTime = settingRead(NK_MinFloatTime).toInt();
  }
  if (!settingExists(NK_SOC_BlockRebulk_percent)) {
    settingWrite(NK_SOC_BlockRebulk_percent, String(SOC_BlockRebulk_percent).c_str());
  } else {
    SOC_BlockRebulk_percent = settingRead(NK_SOC_BlockRebulk_percent).toInt();
  }
  if (!settingExists(NK_SOC_AllowRebulk_percent)) {
    settingWrite(NK_SOC_AllowRebulk_percent, String(SOC_AllowRebulk_percent).c_str());
  } else {
    SOC_AllowRebulk_percent = settingRead(NK_SOC_AllowRebulk_percent).toInt();
  }
  if (!settingExists(NK_wavePeriod)) {
    settingWrite(NK_wavePeriod, String(wavePeriod).c_str());
  } else {
    wavePeriod = settingRead(NK_wavePeriod).toInt();
  }

  if (!settingExists(NK_tuningWaveform)) {
    settingWrite(NK_tuningWaveform, String(tuningWaveform).c_str());
  } else {
    tuningWaveform = settingRead(NK_tuningWaveform).toInt();
  }
  if (!settingExists(NK_tuningSineFreq)) {
    settingWrite(NK_tuningSineFreq, String(tuningSineFreq).c_str());
  } else {
    tuningSineFreq = settingRead(NK_tuningSineFreq).toFloat();
  }
  if (!settingExists(NK_tuningSweepStart)) {
    settingWrite(NK_tuningSweepStart, String(tuningSweepStart).c_str());
  } else {
    tuningSweepStart = settingRead(NK_tuningSweepStart).toFloat();
  }
  if (!settingExists(NK_tuningSweepEnd)) {
    settingWrite(NK_tuningSweepEnd, String(tuningSweepEnd).c_str());
  } else {
    tuningSweepEnd = settingRead(NK_tuningSweepEnd).toFloat();
  }
  if (!settingExists(NK_tuningSweepCycles)) {
    settingWrite(NK_tuningSweepCycles, String(tuningSweepCycles).c_str());
  } else {
    tuningSweepCycles = (uint8_t)settingRead(NK_tuningSweepCycles).toInt();
  }

  if (!settingExists(NK_SystemIDStepAmplitude)) {
      settingWrite(NK_SystemIDStepAmplitude, String(SystemIDStepAmplitude).c_str());
    } else {
      SystemIDStepAmplitude = settingRead(NK_SystemIDStepAmplitude).toFloat();
    }

    if (!settingExists(NK_systemIDTestType)) {
      settingWrite(NK_systemIDTestType, String(systemIDTestType).c_str());
    } else {
      systemIDTestType = (uint8_t)settingRead(NK_systemIDTestType).toInt();
    }
    if (!settingExists(NK_systemIDSineFreqStart)) {
      settingWrite(NK_systemIDSineFreqStart, String(systemIDSineFreqStart).c_str());
    } else {
      systemIDSineFreqStart = settingRead(NK_systemIDSineFreqStart).toFloat();
    }
    if (!settingExists(NK_systemIDSineFreqEnd)) {
      settingWrite(NK_systemIDSineFreqEnd, String(systemIDSineFreqEnd).c_str());
    } else {
      systemIDSineFreqEnd = settingRead(NK_systemIDSineFreqEnd).toFloat();
    }
    if (!settingExists(NK_sysidPlantTau)) {
      settingWrite(NK_sysidPlantTau, String(systemIDPlantTauMs).c_str());
    } else {
      systemIDPlantTauMs = (uint16_t)settingRead(NK_sysidPlantTau).toInt();
    }
    if (!settingExists(NK_fieldDecayTau)) {
      settingWrite(NK_fieldDecayTau, String(fieldDecayTauMs).c_str());
    } else {
      fieldDecayTauMs = (uint16_t)settingRead(NK_fieldDecayTau).toInt();
    }
    if (!settingExists(NK_fdDrainLoMs)) {
      settingWrite(NK_fdDrainLoMs, String(fdDrainLoMs).c_str());
    } else {
      fdDrainLoMs = (uint16_t)settingRead(NK_fdDrainLoMs).toInt();
    }
    if (!settingExists(NK_fdDrainHiMs)) {
      settingWrite(NK_fdDrainHiMs, String(fdDrainHiMs).c_str());
    } else {
      fdDrainHiMs = (uint16_t)settingRead(NK_fdDrainHiMs).toInt();
    }
    if (!settingExists(NK_fdDrainRpmLo)) {
      settingWrite(NK_fdDrainRpmLo, String(fdDrainRpmLo).c_str());
    } else {
      fdDrainRpmLo = (uint16_t)settingRead(NK_fdDrainRpmLo).toInt();
    }
    if (!settingExists(NK_fdDrainRpmHi)) {
      settingWrite(NK_fdDrainRpmHi, String(fdDrainRpmHi).c_str());
    } else {
      fdDrainRpmHi = (uint16_t)settingRead(NK_fdDrainRpmHi).toInt();
    }
    if (!settingExists(NK_faCalGain)) {
      settingWrite(NK_faCalGain, String(faCalGain, 4).c_str());
    } else {
      faCalGain = settingRead(NK_faCalGain).toFloat();
    }
    if (!settingExists(NK_faCalOffA)) {
      settingWrite(NK_faCalOffA, String(faCalOffA, 3).c_str());
    } else {
      faCalOffA = settingRead(NK_faCalOffA).toFloat();
    }
    // Survives reboots on purpose: a device rescaled while offline must keep suppressing front
    // sync until the cloud wipe lands, or the cloud ships the old-scaled front straight back.
    rpmAxisWipePending = (settingExists(NK_RpmAxisWipePend) && settingRead(NK_RpmAxisWipePend) == "1");
    // Reboot interrupted the local wipe → re-run it whole (every clear it drives is idempotent)
    if (settingExists(NK_RpmAxisWipeLoc) && settingRead(NK_RpmAxisWipeLoc) == "1") pendingRpmAxisWipe = true;
    if (!settingExists(NK_systemIDSineCycles)) {
      settingWrite(NK_systemIDSineCycles, String(systemIDSineCycles).c_str());
    } else {
      systemIDSineCycles = (uint8_t)settingRead(NK_systemIDSineCycles).toInt();
    }
    if (!settingExists(NK_SystemIDStabilizeAmps)) {
      settingWrite(NK_SystemIDStabilizeAmps, String(SystemIDStabilizeAmps).c_str());
    } else {
      SystemIDStabilizeAmps = settingRead(NK_SystemIDStabilizeAmps).toFloat();
    }

  if (!settingExists(NK_SwitchingFrequency)) {
    settingWrite(NK_SwitchingFrequency, String(SwitchingFrequency).c_str());
  } else {
    SwitchingFrequency = settingRead(NK_SwitchingFrequency).toInt();
  }
  if (!settingExists(NK_FloatVoltage)) {
    FloatVoltage *= seedVScale;
    settingWrite(NK_FloatVoltage, String(FloatVoltage).c_str());
  } else {
    FloatVoltage = settingRead(NK_FloatVoltage).toFloat();
  }
  if (!settingExists(NK_yyMin)) {
    settingWrite(NK_yyMin, String(yyMin).c_str());
  } else {
    yyMin = settingRead(NK_yyMin).toInt();
  }
  if (!settingExists(NK_ManualFieldToggle)) {
    settingWrite(NK_ManualFieldToggle, String(ManualFieldToggle).c_str());
  } else {
    ManualFieldToggle = settingRead(NK_ManualFieldToggle).toInt();
  }
  if (!settingExists(NK_PhysicalPanelOverride)) {
    settingWrite(NK_PhysicalPanelOverride, String(PhysicalPanelOverride).c_str());
  } else {
    PhysicalPanelOverride = settingRead(NK_PhysicalPanelOverride).toInt();
  }
  if (!settingExists(NK_IgnitionOverride)) {
    settingWrite(NK_IgnitionOverride, String(IgnitionOverride).c_str());
  } else {
    IgnitionOverride = settingRead(NK_IgnitionOverride).toInt();
  }
  if (!settingExists(NK_hardwarePresent)) {
    settingWrite(NK_hardwarePresent, String(hardwarePresent).c_str());
  } else {
    hardwarePresent = settingRead(NK_hardwarePresent).toInt();
  }
  if (!settingExists(NK_timeSourceMode)) {
    settingWrite(NK_timeSourceMode, String(timeSourceMode).c_str());
  } else {
    timeSourceMode = (uint8_t)settingRead(NK_timeSourceMode).toInt();
    if (timeSourceMode > TSRC_NTP) timeSourceMode = TSRC_AUTO;  // sanity
  }
  // Position source split out of the combined key on 2026-08-24. A device upgrading
  // from a build that only had the combined setting has no NK_gpsPositionSource yet:
  // derive it from what the owner had chosen, so a forced NMEA/phone choice survives.
  // The old NTP value was time-only and left position on the auto chain.
  if (!settingExists(NK_gpsPositionSource)) {
    switch (timeSourceMode) {
      case TSRC_NMEA:  gpsPositionSource = GPS_SRC_NMEA;  break;
      case TSRC_PHONE: gpsPositionSource = GPS_SRC_PHONE; break;
      default:         gpsPositionSource = GPS_SRC_AUTO;  break;
    }
    settingWrite(NK_gpsPositionSource, String(gpsPositionSource).c_str());
  } else {
    gpsPositionSource = (uint8_t)settingRead(NK_gpsPositionSource).toInt();
    if (gpsPositionSource > GPS_SRC_PHONE) gpsPositionSource = GPS_SRC_AUTO;  // sanity
  }
  if (!settingExists(NK_speedSourceMode)) {
    settingWrite(NK_speedSourceMode, String(speedSourceMode).c_str());
  } else {
    speedSourceMode = (uint8_t)settingRead(NK_speedSourceMode).toInt();
    if (speedSourceMode > SPD_SRC_PHONE) speedSourceMode = SPD_SRC_NMEA;  // sanity
  }
  if (!settingExists(NK_MaintainMode)) {
    settingWrite(NK_MaintainMode, String(MaintainMode).c_str());
  } else {
    MaintainMode = settingRead(NK_MaintainMode).toInt();
  }
  MaintainModeUserSel = MaintainMode;  // NVS holds the app's choice; servicePanelSwitchInputs() derives the effective MaintainMode from it (or from the Cable 3 wire) on the first loop pass
  if (!settingExists(NK_TargetVoltageMode)) {
    settingWrite(NK_TargetVoltageMode, String(TargetVoltageMode).c_str());
  } else {
    TargetVoltageMode = settingRead(NK_TargetVoltageMode).toInt();
  }
  if (!settingExists(NK_OnOff)) {
    settingWrite(NK_OnOff, String(OnOff).c_str());
  } else {
    OnOff = settingRead(NK_OnOff).toInt();
  }
  if (!settingExists(NK_HiLow)) {
    settingWrite(NK_HiLow, String(HiLow).c_str());
  } else {
    HiLow = settingRead(NK_HiLow).toInt();
  }
  HiLowUserSel = HiLow;  // NVS holds the app's choice; servicePanelSwitchInputs() derives the effective HiLow from it (or from the Cable 3 wire) on the first loop pass
  if (!settingExists(NK_AmpSensorRange)) {
    settingWrite(NK_AmpSensorRange, String(AmpSensorRange).c_str());
  } else {
    AmpSensorRange = settingRead(NK_AmpSensorRange).toInt();
  }
  if (!settingExists(NK_InvertAltAmps)) {
    settingWrite(NK_InvertAltAmps, String(InvertAltAmps).c_str());
  } else {
    InvertAltAmps = settingRead(NK_InvertAltAmps).toInt();
  }
  if (!settingExists(NK_InvertBattAmps)) {
    settingWrite(NK_InvertBattAmps, String(InvertBattAmps).c_str());
  } else {
    InvertBattAmps = settingRead(NK_InvertBattAmps).toInt();
  }
  if (!settingExists(NK_BatteryShuntPresent)) {
    settingWrite(NK_BatteryShuntPresent, String(BatteryShuntPresent).c_str());
  } else {
    BatteryShuntPresent = settingRead(NK_BatteryShuntPresent).toInt();
  }
  if (!settingExists(NK_LimpHome)) {
    settingWrite(NK_LimpHome, String(LimpHome).c_str());
  } else {
    LimpHome = settingRead(NK_LimpHome).toInt();
  }
  if (!settingExists(NK_VeData)) {
    settingWrite(NK_VeData, String(VeData).c_str());
  } else {
    VeData = settingRead(NK_VeData).toInt();
  }
  if (!settingExists(NK_NMEA0183Data)) {
    settingWrite(NK_NMEA0183Data, String(NMEA0183Data).c_str());
  } else {
    NMEA0183Data = settingRead(NK_NMEA0183Data).toInt();
  }
  if (!settingExists(NK_NMEA0183Baud)) {
    settingWrite(NK_NMEA0183Baud, String(NMEA0183Baud).c_str());
  } else {
    NMEA0183Baud = settingRead(NK_NMEA0183Baud).toInt();
  }
  if (!settingExists(NK_NMEA0183Invert)) {
    settingWrite(NK_NMEA0183Invert, String(NMEA0183Invert).c_str());
  } else {
    NMEA0183Invert = settingRead(NK_NMEA0183Invert).toInt();
  }
  if (!settingExists(NK_NMEA2KData)) {
    settingWrite(NK_NMEA2KData, String(NMEA2KData).c_str());
  } else {
    NMEA2KData = settingRead(NK_NMEA2KData).toInt();
  }
  if (!settingExists(NK_n2kTxEn)) {
    settingWrite(NK_n2kTxEn, String(n2kTxEnable).c_str());
  } else {
    n2kTxEnable = settingRead(NK_n2kTxEn).toInt();
  }
  if (!settingExists(NK_n2kDevInst)) {
    settingWrite(NK_n2kDevInst, String(n2kDeviceInstance).c_str());
  } else {
    n2kDeviceInstance = settingRead(NK_n2kDevInst).toInt();
  }
  if (!settingExists(NK_n2kBattEn)) {
    settingWrite(NK_n2kBattEn, String(n2kBattEnable).c_str());
  } else {
    n2kBattEnable = settingRead(NK_n2kBattEn).toInt();
  }
  if (!settingExists(NK_n2kBattInst)) {
    settingWrite(NK_n2kBattInst, String(n2kBattInstance).c_str());
  } else {
    n2kBattInstance = settingRead(NK_n2kBattInst).toInt();
  }
  if (!settingExists(NK_n2kBattCfgEn)) {
    settingWrite(NK_n2kBattCfgEn, String(n2kBattCfgEnable).c_str());
  } else {
    n2kBattCfgEnable = settingRead(NK_n2kBattCfgEn).toInt();
  }
  if (!settingExists(NK_n2kAltEn)) {
    settingWrite(NK_n2kAltEn, String(n2kAltEnable).c_str());
  } else {
    n2kAltEnable = settingRead(NK_n2kAltEn).toInt();
  }
  if (!settingExists(NK_n2kAltInst)) {
    settingWrite(NK_n2kAltInst, String(n2kAltInstance).c_str());
  } else {
    n2kAltInstance = settingRead(NK_n2kAltInst).toInt();
  }
  if (!settingExists(NK_n2kAltTempEn)) {
    settingWrite(NK_n2kAltTempEn, String(n2kAltTempEnable).c_str());
  } else {
    n2kAltTempEnable = settingRead(NK_n2kAltTempEn).toInt();
  }
  if (!settingExists(NK_n2kTempInst)) {
    settingWrite(NK_n2kTempInst, String(n2kTempInstance).c_str());
  } else {
    n2kTempInstance = settingRead(NK_n2kTempInst).toInt();
  }
  if (!settingExists(NK_n2kTempSrc)) {
    settingWrite(NK_n2kTempSrc, String(n2kTempSource).c_str());
  } else {
    n2kTempSource = settingRead(NK_n2kTempSrc).toInt();
  }
  if (!settingExists(NK_n2kExtraTempEnable)) {
    settingWrite(NK_n2kExtraTempEnable, String(n2kExtraTempEnable).c_str());
  } else {
    n2kExtraTempEnable = settingRead(NK_n2kExtraTempEnable).toInt();
  }
  if (!settingExists(NK_n2kExtraTempInstance)) {
    settingWrite(NK_n2kExtraTempInstance, String(n2kExtraTempInstance).c_str());
  } else {
    n2kExtraTempInstance = clampLoadedSetting("n2kExtraTempInstance", NK_n2kExtraTempInstance, settingRead(NK_n2kExtraTempInstance).toInt(), 0, 252);
  }
  if (!settingExists(NK_n2kExtraTempSource)) {
    settingWrite(NK_n2kExtraTempSource, String(n2kExtraTempSource).c_str());
  } else {
    n2kExtraTempSource = clampLoadedSetting("n2kExtraTempSource", NK_n2kExtraTempSource, settingRead(NK_n2kExtraTempSource).toInt(), 0, 15);
  }
  if (!settingExists(NK_n2kChgrEn)) {
    settingWrite(NK_n2kChgrEn, String(n2kChgrEnable).c_str());
  } else {
    n2kChgrEnable = settingRead(NK_n2kChgrEn).toInt();
  }
  if (!settingExists(NK_n2kChgrInst)) {
    settingWrite(NK_n2kChgrInst, String(n2kChgrInstance).c_str());
  } else {
    n2kChgrInstance = settingRead(NK_n2kChgrInst).toInt();
  }
  if (!settingExists(NK_n2kChgrCfgEn)) {
    settingWrite(NK_n2kChgrCfgEn, String(n2kChgrCfgEnable).c_str());
  } else {
    n2kChgrCfgEnable = settingRead(NK_n2kChgrCfgEn).toInt();
  }
  if (!settingExists(NK_n2kChgrMode)) {
    settingWrite(NK_n2kChgrMode, String(n2kChgrMode).c_str());
  } else {
    n2kChgrMode = settingRead(NK_n2kChgrMode).toInt();
  }
  if (!settingExists(NK_n2kEngRpmEn)) {
    settingWrite(NK_n2kEngRpmEn, String(n2kEngRpmEnable).c_str());
  } else {
    n2kEngRpmEnable = settingRead(NK_n2kEngRpmEn).toInt();
  }
  if (!settingExists(NK_n2kEngInst)) {
    settingWrite(NK_n2kEngInst, String(n2kEngInstance).c_str());
  } else {
    n2kEngInstance = settingRead(NK_n2kEngInst).toInt();
  }
  if (!settingExists(NK_n2kEngDynEn)) {
    settingWrite(NK_n2kEngDynEn, String(n2kEngDynEnable).c_str());
  } else {
    n2kEngDynEnable = settingRead(NK_n2kEngDynEn).toInt();
  }
  if (!settingExists(NK_n2kEngBitsEn)) {
    settingWrite(NK_n2kEngBitsEn, String(n2kEngBitsEnable).c_str());
  } else {
    n2kEngBitsEnable = settingRead(NK_n2kEngBitsEn).toInt();
  }
  if (!settingExists(NK_rvcTxEn)) {
    settingWrite(NK_rvcTxEn, String(rvcTxEnable).c_str());
  } else {
    rvcTxEnable = settingRead(NK_rvcTxEn).toInt();
  }
  if (!settingExists(NK_rvcChgrEn)) {
    settingWrite(NK_rvcChgrEn, String(rvcChgrEnable).c_str());
  } else {
    rvcChgrEnable = settingRead(NK_rvcChgrEn).toInt();
  }
  if (!settingExists(NK_rvcDcEn)) {
    settingWrite(NK_rvcDcEn, String(rvcDcEnable).c_str());
  } else {
    rvcDcEnable = settingRead(NK_rvcDcEn).toInt();
  }
  if (!settingExists(NK_rvcFaultEn)) {
    settingWrite(NK_rvcFaultEn, String(rvcFaultEnable).c_str());
  } else {
    rvcFaultEnable = settingRead(NK_rvcFaultEn).toInt();
  }
  if (!settingExists(NK_rvcChgrInst)) {
    settingWrite(NK_rvcChgrInst, String(rvcChgrInstance).c_str());
  } else {
    rvcChgrInstance = settingRead(NK_rvcChgrInst).toInt();
  }
  if (!settingExists(NK_rvcDcInst)) {
    settingWrite(NK_rvcDcInst, String(rvcDcInstance).c_str());
  } else {
    rvcDcInstance = settingRead(NK_rvcDcInst).toInt();
  }
  if (!settingExists(NK_rvcDevPri)) {
    settingWrite(NK_rvcDevPri, String(rvcDevPriority).c_str());
  } else {
    rvcDevPriority = settingRead(NK_rvcDevPri).toInt();
  }
  if (!settingExists(NK_n2kRxBattInst)) {
    settingWrite(NK_n2kRxBattInst, String(n2kRxBattInstance).c_str());
  } else {
    n2kRxBattInstance = settingRead(NK_n2kRxBattInst).toInt();
  }
  if (!settingExists(NK_dvccEn)) {
    settingWrite(NK_dvccEn, String(dvccEn).c_str());
  } else {
    dvccEn = settingRead(NK_dvccEn).toInt();
  }
  if (!settingExists(NK_dvccSrcType)) {
    settingWrite(NK_dvccSrcType, String(dvccSrcType).c_str());
  } else {
    dvccSrcType = settingRead(NK_dvccSrcType).toInt();
  }
  if (!settingExists(NK_dvccInst)) {
    settingWrite(NK_dvccInst, String(dvccInst).c_str());
  } else {
    dvccInst = settingRead(NK_dvccInst).toInt();
  }
  if (!settingExists(NK_dvccSilenceS)) {
    settingWrite(NK_dvccSilenceS, String(dvccSilenceS).c_str());
  } else {
    dvccSilenceS = settingRead(NK_dvccSilenceS).toInt();
  }
  if (!settingExists(NK_dvccSettleS)) {
    settingWrite(NK_dvccSettleS, String(dvccSettleS).c_str());
  } else {
    dvccSettleS = settingRead(NK_dvccSettleS).toInt();
  }
  // Captured before either load overwrites the globals, so the recovery pair below is the compile-time
  // default scaled exactly like the seed path — never a hardcoded 12 V number.
  const float dvccCvlMinSeed = dvccCvlMin * seedVScale;
  const float dvccCvlMaxSeed = dvccCvlMax * seedVScale;
  if (!settingExists(NK_dvccCvlMin)) {
    dvccCvlMin *= seedVScale;
    settingWrite(NK_dvccCvlMin, String(dvccCvlMin, 2).c_str());
  } else {
    dvccCvlMin = settingRead(NK_dvccCvlMin).toFloat();
  }
  if (!settingExists(NK_dvccCvlMax)) {
    dvccCvlMax *= seedVScale;
    settingWrite(NK_dvccCvlMax, String(dvccCvlMax, 2).c_str());
  } else {
    dvccCvlMax = settingRead(NK_dvccCvlMax).toFloat();
  }
  {
    // Same bounds and same low < high rule the /settings handlers enforce, applied here because config
    // import and the cloud config push write these keys raw and reach the globals only through a boot.
    // A NaN survives the clamp untouched and fails the ordering test, which is the intent.
    float cvlLo = constrain(dvccCvlMin, DVCC_CVL_ABS_MIN, DVCC_CVL_ABS_MAX);
    float cvlHi = constrain(dvccCvlMax, DVCC_CVL_ABS_MIN, DVCC_CVL_ABS_MAX);
    if (!(cvlLo < cvlHi)) { cvlLo = dvccCvlMinSeed; cvlHi = dvccCvlMaxSeed; }
    if (cvlLo != dvccCvlMin || cvlHi != dvccCvlMax) {
      queueConsoleMessageF("Stored setting out of range - DVCC plausible-CVL window was %.2f-%.2f V, corrected to %.2f-%.2f V",
                           dvccCvlMin, dvccCvlMax, cvlLo, cvlHi);
      dvccCvlMin = cvlLo;
      dvccCvlMax = cvlHi;
      settingWrite(NK_dvccCvlMin, String(dvccCvlMin, 2).c_str());
      settingWrite(NK_dvccCvlMax, String(dvccCvlMax, 2).c_str());
    }
  }
  if (!settingExists(NK_waveAmplitude)) {
    settingWrite(NK_waveAmplitude, String(waveAmplitude).c_str());
  } else {
    waveAmplitude = settingRead(NK_waveAmplitude).toInt();
  }
  if (!settingExists(NK_tuningWaveFloor)) {
    settingWrite(NK_tuningWaveFloor, String(tuningWaveFloor).c_str());
  } else {
    tuningWaveFloor = settingRead(NK_tuningWaveFloor).toInt();
  }
  if (!settingExists(NK_CurrentThreshold)) {
    settingWrite(NK_CurrentThreshold, String(CurrentThreshold).c_str());
  } else {
    CurrentThreshold = settingRead(NK_CurrentThreshold).toFloat();
  }
  if (!settingExists(NK_PeukertExponent)) {
    settingWrite(NK_PeukertExponent, String(PeukertExponent_scaled).c_str());
  } else {
    float pv = settingRead(NK_PeukertExponent).toFloat();
    if (pv <= 2.0f) {
      // old file stored raw user input (e.g. "1.12") — migrate to scaled int
      PeukertExponent_scaled = (int)(pv * 100);
      settingWrite(NK_PeukertExponent, String(PeukertExponent_scaled).c_str());
    } else {
      PeukertExponent_scaled = (int)pv;
    }
  }
  if (!settingExists(NK_ChargedVoltage)) {
    ChargedVoltage_Scaled = (int)lroundf(ChargedVoltage_Scaled * seedVScale);
    settingWrite(NK_ChargedVoltage, String(ChargedVoltage_Scaled).c_str());
  } else {
    float cv = settingRead(NK_ChargedVoltage).toFloat();
    if (cv <= 70.0f) {
      // old file stored raw user input (e.g. "14.50") — migrate to scaled int
      ChargedVoltage_Scaled = (int)(cv * 100);
      settingWrite(NK_ChargedVoltage, String(ChargedVoltage_Scaled).c_str());
    } else {
      ChargedVoltage_Scaled = (int)cv;
    }
  }
  if (!settingExists(NK_ChargedDetectionTime)) {
    settingWrite(NK_ChargedDetectionTime, String(ChargedDetectionTime).c_str());
  } else {
    ChargedDetectionTime = settingRead(NK_ChargedDetectionTime).toInt();
  }
  if (!settingExists(NK_IgnoreTemperature)) {
    settingWrite(NK_IgnoreTemperature, String(IgnoreTemperature).c_str());
  } else {
    IgnoreTemperature = settingRead(NK_IgnoreTemperature).toInt();
  }
  if (!settingExists(NK_IgnoreRPM)) {
    settingWrite(NK_IgnoreRPM, String(IgnoreRPM).c_str());
  } else {
    IgnoreRPM = settingRead(NK_IgnoreRPM).toInt();
  }
  if (!settingExists(NK_MinRPMForField)) {
    settingWrite(NK_MinRPMForField, String(MinRPMForField).c_str());
  } else {
    MinRPMForField = settingRead(NK_MinRPMForField).toInt();
  }
  if (!settingExists(NK_bmsLogic)) {
    settingWrite(NK_bmsLogic, String(bmsLogic).c_str());
  } else {
    bmsLogic = settingRead(NK_bmsLogic).toInt();
  }
  if (!settingExists(NK_bmsLogicLevelOff)) {
    settingWrite(NK_bmsLogicLevelOff, String(bmsLogicLevelOff).c_str());
  } else {
    bmsLogicLevelOff = settingRead(NK_bmsLogicLevelOff).toInt();
  }
  if (!settingExists(NK_AlarmActivate)) {
    settingWrite(NK_AlarmActivate, String(AlarmActivate).c_str());
  } else {
    AlarmActivate = settingRead(NK_AlarmActivate).toInt();
  }
  if (!settingExists(NK_TempAlarm)) {
    settingWrite(NK_TempAlarm, String(TempAlarm).c_str());
  } else {
    TempAlarm = settingRead(NK_TempAlarm).toInt();
  }
  if (!settingExists(NK_TempAlarmLow)) {
    settingWrite(NK_TempAlarmLow, String(TempAlarmLow).c_str());
  } else {
    TempAlarmLow = settingRead(NK_TempAlarmLow).toInt();
  }
  if (!settingExists(NK_VoltageAlarmHigh)) {
    VoltageAlarmHigh *= seedVScale;
    settingWrite(NK_VoltageAlarmHigh, String(VoltageAlarmHigh, 2).c_str());
  } else {
    VoltageAlarmHigh = settingRead(NK_VoltageAlarmHigh).toFloat();  // pre-float NVS strings ("15") parse fine
  }
  if (!settingExists(NK_VoltageAlarmLow)) {
    VoltageAlarmLow *= seedVScale;
    settingWrite(NK_VoltageAlarmLow, String(VoltageAlarmLow, 2).c_str());
  } else {
    VoltageAlarmLow = settingRead(NK_VoltageAlarmLow).toFloat();
  }
  if (!settingExists(NK_SocAlarmLow)) {
    settingWrite(NK_SocAlarmLow, String(SocAlarmLow).c_str());
  } else {
    SocAlarmLow = settingRead(NK_SocAlarmLow).toInt();
  }
  if (!settingExists(NK_CurrentAlarmHigh)) {
    settingWrite(NK_CurrentAlarmHigh, String(CurrentAlarmHigh).c_str());
  } else {
    CurrentAlarmHigh = settingRead(NK_CurrentAlarmHigh).toInt();
  }
  if (!settingExists(NK_RPMScalingFactor)) {
    settingWrite(NK_RPMScalingFactor, String(RPMScalingFactor).c_str());
  } else {
    RPMScalingFactor = settingRead(NK_RPMScalingFactor).toInt();
  }
  if (!settingExists(NK_FieldResistance)) {
    settingWrite(NK_FieldResistance, String(FieldResistance).c_str());
  } else {
    FieldResistance = settingRead(NK_FieldResistance).toFloat();
  }
  if (!settingExists(NK_MaximumAllowedBatteryAmps)) {
    settingWrite(NK_MaximumAllowedBatteryAmps, String(MaximumAllowedBatteryAmps).c_str());
  } else {
    MaximumAllowedBatteryAmps = settingRead(NK_MaximumAllowedBatteryAmps).toInt();
  }
  if (!settingExists(NK_LoadDumpDtThresh1)) {
    settingWrite(NK_LoadDumpDtThresh1, String(LoadDumpDtThresh1).c_str());
  } else {
    LoadDumpDtThresh1 = settingRead(NK_LoadDumpDtThresh1).toFloat();
  }
  if (!settingExists(NK_LoadDumpDtThresh)) {
    settingWrite(NK_LoadDumpDtThresh, String(LoadDumpDtThresh).c_str());
  } else {
    LoadDumpDtThresh = settingRead(NK_LoadDumpDtThresh).toFloat();
  }
  if (!settingExists(NK_LoadDumpDtThresh3)) {
    settingWrite(NK_LoadDumpDtThresh3, String(LoadDumpDtThresh3).c_str());
  } else {
    LoadDumpDtThresh3 = settingRead(NK_LoadDumpDtThresh3).toFloat();
  }
  // Constrained at boot (not just in the /get handler): the config-import path writes NVS raw,
  // and N = 0 makes the detector's `count >= N` true on EVERY sample — a permanent load-dump latch.
  if (!settingExists(NK_LoadDumpN1)) {
    settingWrite(NK_LoadDumpN1, String(LoadDumpN1).c_str());
  } else {
    LoadDumpN1 = constrain(settingRead(NK_LoadDumpN1).toInt(), 1, 10);
  }
  if (!settingExists(NK_LoadDumpN2)) {
    settingWrite(NK_LoadDumpN2, String(LoadDumpN2).c_str());
  } else {
    LoadDumpN2 = constrain(settingRead(NK_LoadDumpN2).toInt(), 1, 10);
  }
  if (!settingExists(NK_LoadDumpN3)) {
    settingWrite(NK_LoadDumpN3, String(LoadDumpN3).c_str());
  } else {
    LoadDumpN3 = constrain(settingRead(NK_LoadDumpN3).toInt(), 1, 10);
  }
  // A live test must NEVER auto-resume after a reboot — force OFF on boot, ignoring any stored value (write back only if it was stuck on).
  CVTuningMode = 0;
  if (settingExists(NK_CVTuningMode) && settingRead(NK_CVTuningMode).toInt() != 0) settingWrite(NK_CVTuningMode, "0");
  if (!settingExists(NK_cvWaveAmplitudeV)) {
    cvWaveAmplitudeV *= seedVScale;
    settingWrite(NK_cvWaveAmplitudeV, String(cvWaveAmplitudeV).c_str());
  } else {
    cvWaveAmplitudeV = settingRead(NK_cvWaveAmplitudeV).toFloat();
  }
  if (!settingExists(NK_cvWavePeriodSec)) {
    settingWrite(NK_cvWavePeriodSec, String(cvWavePeriodSec).c_str());
  } else {
    cvWavePeriodSec = settingRead(NK_cvWavePeriodSec).toInt();
  }
  if (!settingExists(NK_cvKOvershoot)) {
    settingWrite(NK_cvKOvershoot, String(cvKOvershoot).c_str());
  } else {
    cvKOvershoot = settingRead(NK_cvKOvershoot).toFloat();
  }
  if (!settingExists(NK_cvConsecutiveReads)) {
    settingWrite(NK_cvConsecutiveReads, String(cvConsecutiveReads).c_str());
  } else {
    cvConsecutiveReads = (uint8_t)settingRead(NK_cvConsecutiveReads).toInt();
  }
  if (!settingExists(NK_ManualSOCPoint)) {
    settingWrite(NK_ManualSOCPoint, String(ManualSOCPoint).c_str());
  } else {
    ManualSOCPoint = settingRead(NK_ManualSOCPoint).toFloat();
  }
  if (!settingExists(NK_ManualLifePercentage)) {  // manual override of alterantor lifetime estimates this is pointless
    settingWrite(NK_ManualLifePercentage, String(ManualLifePercentage).c_str());
  } else {
    ManualLifePercentage = settingRead(NK_ManualLifePercentage).toInt();
  }
  if (!settingExists(NK_ShuntResistanceMicroOhm)) {
    settingWrite(NK_ShuntResistanceMicroOhm, String(ShuntResistanceMicroOhm).c_str());
  } else {
    ShuntResistanceMicroOhm = settingRead(NK_ShuntResistanceMicroOhm).toInt();
  }
  if (!settingExists(NK_maxPoints)) {
    settingWrite(NK_maxPoints, String(maxPoints).c_str());
  } else {
    maxPoints = settingRead(NK_maxPoints).toInt();
  }
  if (!settingExists(NK_Ymin1)) {
    settingWrite(NK_Ymin1, String(Ymin1).c_str());
  } else {
    Ymin1 = settingRead(NK_Ymin1).toInt();
  }
  if (!settingExists(NK_Ymax1)) {
    settingWrite(NK_Ymax1, String(Ymax1).c_str());
  } else {
    Ymax1 = settingRead(NK_Ymax1).toInt();
  }
  // Voltage-plot axis bounds are volt-domain: first creation scales the 12V default window ×(V/12)
  if (!settingExists(NK_Ymin2)) {
    Ymin2 *= seedVScale;
    settingWrite(NK_Ymin2, String(Ymin2).c_str());
  } else {
    Ymin2 = settingRead(NK_Ymin2).toFloat();
  }
  if (!settingExists(NK_Ymax2)) {
    Ymax2 *= seedVScale;
    settingWrite(NK_Ymax2, String(Ymax2).c_str());
  } else {
    Ymax2 = settingRead(NK_Ymax2).toFloat();
  }
  if (!settingExists(NK_Ymin3)) {
    settingWrite(NK_Ymin3, String(Ymin3).c_str());
  } else {
    Ymin3 = settingRead(NK_Ymin3).toInt();
  }
  if (!settingExists(NK_Ymax3)) {
    settingWrite(NK_Ymax3, String(Ymax3).c_str());
  } else {
    Ymax3 = settingRead(NK_Ymax3).toInt();
  }
  if (!settingExists(NK_Ymin4)) {
    settingWrite(NK_Ymin4, String(Ymin4).c_str());
  } else {
    Ymin4 = settingRead(NK_Ymin4).toInt();
  }
  if (!settingExists(NK_Ymax4)) {
    settingWrite(NK_Ymax4, String(Ymax4).c_str());
  } else {
    Ymax4 = settingRead(NK_Ymax4).toInt();
  }
  if (!settingExists(NK_MaxDuty)) {
    // Max Field % is the REAL per-bus field-duty cap. The hardcoded default (99) is a 12V value, so on
    // first creation scale it by ×(12/SYSTEM_VOLTAGE_CLASS) (→ ~50%@24V, ~25%@48V) so worst-case field
    // current never exceeds the 12V case. WYSIWYG: the dashboard box shows the actual cap; user-adjustable.
    MaxDuty = (int)lroundf(MaxDuty * 12.0f / (float)SYSTEM_VOLTAGE_CLASS);
    settingWrite(NK_MaxDuty, String(MaxDuty).c_str());
  } else {
    MaxDuty = settingRead(NK_MaxDuty).toInt();
  }
  if (!settingExists(NK_MaxFieldVolts)) {
    // Max Field Volts is a VOLT-domain seed (×seedVScale → 15/30/45/60), the opposite domain from
    // MaxDuty above: it is physical field volts, not a duty ratio. Nominal class × 1.25 sits above any
    // bank's absorb voltage, so the derived ceiling lands over 99% and the volts cap is inert at its
    // default on every class — identical behavior to before it existed. It binds only once an installer
    // enters the winding's rated voltage from the alternator datasheet.
    MaxFieldVolts *= seedVScale;
    settingWrite(NK_MaxFieldVolts, String(MaxFieldVolts, 1).c_str());
  } else {
    // A garbage NVS string (hand-edited export imported raw, or a bad admin push) parses to 0.0,
    // which pins the field ceiling at MinDuty and effectively kills charging. Enforce the web
    // handler's [0.5, 60] window here too, and fall back to the inert class default rather than
    // the window edge — a 0.5 V ceiling is still a dead field.
    MaxFieldVolts = settingRead(NK_MaxFieldVolts).toFloat();
    if (!(MaxFieldVolts >= 0.5f && MaxFieldVolts <= 60.0f)) {
      MaxFieldVolts = 1.25f * (float)SYSTEM_VOLTAGE_CLASS;
      settingWrite(NK_MaxFieldVolts, String(MaxFieldVolts, 1).c_str());
    }
  }
  if (!settingExists(NK_MinDuty)) {
    // Float field floor, first-creation scaled ×(12/SYSTEM_VOLTAGE_CLASS) like MaxDuty so the same
    // field current floor applies on any bank (0.25% @48V ≡ 1% @12V).
    MinDuty = MinDuty * 12.0f / (float)SYSTEM_VOLTAGE_CLASS;
    settingWrite(NK_MinDuty, String(MinDuty, 2).c_str());
  } else {
    MinDuty = settingRead(NK_MinDuty).toFloat();
  }
  if (!settingExists(NK_R_fixed)) {
    settingWrite(NK_R_fixed, String(R_fixed).c_str());
  } else {
    R_fixed = settingRead(NK_R_fixed).toFloat();
  }
  if (!settingExists(NK_Beta)) {
    settingWrite(NK_Beta, String(Beta).c_str());
  } else {
    Beta = settingRead(NK_Beta).toFloat();
  }
  if (!settingExists(NK_T0_C)) {
    settingWrite(NK_T0_C, String(T0_C).c_str());
  } else {
    T0_C = settingRead(NK_T0_C).toFloat();
  }
  if (!settingExists(NK_TempSource)) {
    settingWrite(NK_TempSource, String(TempSource).c_str());
  } else {
    TempSource = settingRead(NK_TempSource).toInt();
  }
  if (!settingExists(NK_AlternatorCOffset)) {
    settingWrite(NK_AlternatorCOffset, String(AlternatorCOffset).c_str());
  } else {
    AlternatorCOffset = settingRead(NK_AlternatorCOffset).toFloat();
  }
  if (!settingExists(NK_BatteryCOffset)) {
    settingWrite(NK_BatteryCOffset, String(BatteryCOffset).c_str());
  } else {
    BatteryCOffset = settingRead(NK_BatteryCOffset).toFloat();
  }
  if (!settingExists(NK_AlarmLatchEnabled)) {
    settingWrite(NK_AlarmLatchEnabled, String(AlarmLatchEnabled).c_str());
  } else {
    AlarmLatchEnabled = settingRead(NK_AlarmLatchEnabled).toInt();
  }
  if (!settingExists(NK_absorptionCompleteTime)) {
    settingWrite(NK_absorptionCompleteTime, String(absorptionCompleteTime).c_str());
  } else {
    absorptionCompleteTime = settingRead(NK_absorptionCompleteTime).toInt();
  }
  if (!settingExists(NK_FLOAT_DURATION)) {
    settingWrite(NK_FLOAT_DURATION, String(FLOAT_DURATION).c_str());
  } else {
    FLOAT_DURATION = settingRead(NK_FLOAT_DURATION).toInt();
  }

  if (!settingExists(NK_RebulkCurrent_A)) {
    settingWrite(NK_RebulkCurrent_A, String(RebulkCurrent_A).c_str());
  } else {
    RebulkCurrent_A = settingRead(NK_RebulkCurrent_A).toFloat();
  }
  if (!settingExists(NK_UseFloat)) {
    settingWrite(NK_UseFloat, String(UseFloat).c_str());
  } else {
    UseFloat = settingRead(NK_UseFloat).toInt();
  }
  // Shunt declared absent → float (needs tail current) and MaintainMode can never run. Clear them at boot
  // (runs after shunt-present, UseFloat and MaintainMode are all read) so a config import pairing no-shunt
  // with float-on can't leave the stage machine acting on a meaningless Bcur. /get does the same on a live
  // toggle. A present-but-unset resistance is deliberately NOT reconciled — it is a recoverable calibration
  // gap, and the destroyed setting could not be restored by fixing the resistance.
  if (!BatteryShuntPresent) {
    if (UseFloat != 0)     { UseFloat = 0;     settingWrite(NK_UseFloat, "0"); }
    if (MaintainMode != 0) { MaintainMode = 0; MaintainModeUserSel = 0; settingWrite(NK_MaintainMode, "0"); }
  } else if (!HAS_BATT_SHUNT) {
    queueConsoleMessage("Battery shunt resistance is not set: State of Charge, battery health, battery current limit and float charging are off.");
    queueConsoleMessage("Enter the shunt resistance to enable them. Your float setting is kept.");
  }

  if (!settingExists(NK_VMGTargetBearing)) {
    settingWrite(NK_VMGTargetBearing, String(VMGTargetBearing).c_str());
  } else {
    VMGTargetBearing = settingRead(NK_VMGTargetBearing).toFloat();
  }
  if (!settingExists(NK_AutoShuntGainCorrection)) {  // BOOLEAN
    settingWrite(NK_AutoShuntGainCorrection, String(AutoShuntGainCorrection).c_str());
  } else {
    AutoShuntGainCorrection = settingRead(NK_AutoShuntGainCorrection).toInt();
  }
  if (!settingExists(NK_AutoAltCurrentZero)) {  // BOOLEAN
    settingWrite(NK_AutoAltCurrentZero, String(AutoAltCurrentZero).c_str());
  } else {
    AutoAltCurrentZero = settingRead(NK_AutoAltCurrentZero).toInt();
  }
  if (!settingExists(NK_WindingTempOffset)) {
    settingWrite(NK_WindingTempOffset, String(WindingTempOffset, 1).c_str());
  } else {
    WindingTempOffset = settingRead(NK_WindingTempOffset).toFloat();
  }
  if (!settingExists(NK_displayTempUnit)) {
    settingWrite(NK_displayTempUnit, String(displayTempUnit).c_str());
  } else {
    displayTempUnit = (uint8_t)settingRead(NK_displayTempUnit).toInt();
  }
  if (!settingExists(NK_displayVolUnit)) {
    settingWrite(NK_displayVolUnit, String(displayVolUnit).c_str());
  } else {
    displayVolUnit = (uint8_t)settingRead(NK_displayVolUnit).toInt();
  }
  if (!settingExists(NK_PulleyRatio)) {
    settingWrite(NK_PulleyRatio, String(PulleyRatio, 2).c_str());
  } else {
    PulleyRatio = settingRead(NK_PulleyRatio).toFloat();
  }
  if (!settingExists(NK_BatteryCurrentSource)) {
    settingWrite(NK_BatteryCurrentSource, String(BatteryCurrentSource).c_str());
  } else {
    BatteryCurrentSource = settingRead(NK_BatteryCurrentSource).toInt();
  }

  if (!settingExists(NK_timeAxisModeChanging)) {
    settingWrite(NK_timeAxisModeChanging, String(timeAxisModeChanging).c_str());
  } else {
    timeAxisModeChanging = settingRead(NK_timeAxisModeChanging).toInt();
  }
  if (!settingExists(NK_webgaugesinterval)) {
    settingWrite(NK_webgaugesinterval, String(webgaugesinterval).c_str());
  } else {
    webgaugesinterval = settingRead(NK_webgaugesinterval).toInt();
    webgaugesinterval = constrain(webgaugesinterval, 1, 10000000);
  }
  if (!settingExists(NK_battMaxMode)) {
    settingWrite(NK_battMaxMode, battMaxMode ? "1" : "0");
  } else {
    battMaxMode = (settingRead(NK_battMaxMode).toInt() != 0);
  }
  if (!settingExists(NK_plotTimeWindow)) {
    settingWrite(NK_plotTimeWindow, String(plotTimeWindow).c_str());
  } else {
    plotTimeWindow = settingRead(NK_plotTimeWindow).toInt();
    plotTimeWindow = constrain(plotTimeWindow, 1, 1000000);
  }
  if (!settingExists(NK_IgnoreLearningDuringPenalty)) {
    settingWrite(NK_IgnoreLearningDuringPenalty, String(IgnoreLearningDuringPenalty).c_str());
  } else {
    IgnoreLearningDuringPenalty = settingRead(NK_IgnoreLearningDuringPenalty).toInt();
  }
  if (!settingExists(NK_CloudFeatures)) {
    settingWrite(NK_CloudFeatures, String(CloudFeatures).c_str());
  } else {
    CloudFeatures = settingRead(NK_CloudFeatures).toInt();
  }
  // A live test must NEVER auto-resume after a reboot — force OFF on boot, ignoring any stored value (write back only if it was stuck on).
  TuningMode = 0;
  if (settingExists(NK_TuningMode) && settingRead(NK_TuningMode).toInt() != 0) settingWrite(NK_TuningMode, "0");
  if (!settingExists(NK_LogAllLearningEvents)) {
    settingWrite(NK_LogAllLearningEvents, String(LogAllLearningEvents).c_str());
  } else {
    LogAllLearningEvents = settingRead(NK_LogAllLearningEvents).toInt();
  }
  if (!settingExists(NK_AlternatorNominalAmps)) {
    settingWrite(NK_AlternatorNominalAmps, String(AlternatorNominalAmps).c_str());
  } else {
    AlternatorNominalAmps = settingRead(NK_AlternatorNominalAmps).toInt();
  }
  if (!settingExists(NK_LearningUpStep)) {
    settingWrite(NK_LearningUpStep, String(LearningUpStep, 2).c_str());
  } else {
    LearningUpStep = settingRead(NK_LearningUpStep).toFloat();
  }
  if (!settingExists(NK_LearningDownStep)) {
    settingWrite(NK_LearningDownStep, String(LearningDownStep, 2).c_str());
  } else {
    LearningDownStep = settingRead(NK_LearningDownStep).toFloat();
  }
  if (!settingExists(NK_xTime)) {
    settingWrite(NK_xTime, String(xTime, 2).c_str());
  } else {
    xTime = settingRead(NK_xTime).toFloat();
  }
  if (!settingExists(NK_MinLearningInterval)) {
    settingWrite(NK_MinLearningInterval, String(MinLearningInterval).c_str());
  } else {
    MinLearningInterval = settingRead(NK_MinLearningInterval).toInt();
  }
  if (!settingExists(NK_SafeOperationThreshold)) {
    settingWrite(NK_SafeOperationThreshold, String(SafeOperationThreshold).c_str());
  } else {
    SafeOperationThreshold = settingRead(NK_SafeOperationThreshold).toInt();
  }
  if (!settingExists(NK_SetpointRiseRate)) {
    settingWrite(NK_SetpointRiseRate, String(SetpointRiseRate, 2).c_str());
  } else {
    SetpointRiseRate = settingRead(NK_SetpointRiseRate).toFloat();
  }
  if (!settingExists(NK_SetpointFallRate)) {
    settingWrite(NK_SetpointFallRate, String(SetpointFallRate, 2).c_str());
  } else {
    SetpointFallRate = settingRead(NK_SetpointFallRate).toFloat();
  }
  if (!settingExists(NK_CvBrakeFallRate)) {
    settingWrite(NK_CvBrakeFallRate, String(CvBrakeFallRate, 2).c_str());  // flat amps — no seedVScale
  } else {
    CvBrakeFallRate = settingRead(NK_CvBrakeFallRate).toFloat();
  }
  if (!settingExists(NK_SetpointBigStepThresh)) {
    settingWrite(NK_SetpointBigStepThresh, String(SetpointBigStepThresh, 2).c_str());
  } else {
    SetpointBigStepThresh = settingRead(NK_SetpointBigStepThresh).toFloat();
  }
  if (!settingExists(NK_SetpointBigStepRiseRate)) {
    settingWrite(NK_SetpointBigStepRiseRate, String(SetpointBigStepRiseRate, 2).c_str());
  } else {
    SetpointBigStepRiseRate = settingRead(NK_SetpointBigStepRiseRate).toFloat();
  }
  if (!settingExists(NK_StartupRiseRate)) {
    settingWrite(NK_StartupRiseRate, String(StartupRiseRate, 2).c_str());
  } else {
    StartupRiseRate = settingRead(NK_StartupRiseRate).toFloat();
  }
  if (!settingExists(NK_PIDTrackingGain)) {
    settingWrite(NK_PIDTrackingGain, String(PIDTrackingGain, 2).c_str());
  } else {
    PIDTrackingGain = settingRead(NK_PIDTrackingGain).toFloat();
  }
  if (!settingExists(NK_AbsorptionVoltage)) {
    AbsorptionVoltage *= seedVScale;
    settingWrite(NK_AbsorptionVoltage, String(AbsorptionVoltage).c_str());
  } else {
    AbsorptionVoltage = settingRead(NK_AbsorptionVoltage).toFloat();
  }
  if (!settingExists(NK_TargetVoltageSetpoint)) {
    TargetVoltageSetpoint *= seedVScale;
    settingWrite(NK_TargetVoltageSetpoint, String(TargetVoltageSetpoint).c_str());
  } else {
    TargetVoltageSetpoint = settingRead(NK_TargetVoltageSetpoint).toFloat();
  }
  if (!settingExists(NK_AbsorptionTimeoutMs)) {
    settingWrite(NK_AbsorptionTimeoutMs, String(AbsorptionTimeoutMs).c_str());
  } else {
    AbsorptionTimeoutMs = settingRead(NK_AbsorptionTimeoutMs).toInt();
  }
  if (!settingExists(NK_bulkVoltageHoldMs)) {
    settingWrite(NK_bulkVoltageHoldMs, String(bulkVoltageHoldMs).c_str());
  } else {
    bulkVoltageHoldMs = settingRead(NK_bulkVoltageHoldMs).toInt();
  }


  if (!settingExists(NK_VoltageKi)) {
    settingWrite(NK_VoltageKi, String(VoltageKi).c_str());
  } else {
    VoltageKi = settingRead(NK_VoltageKi).toFloat();
  }
  if (!settingExists(NK_CvKdDeadbandVps)) {
    CvKdDeadbandVps *= seedVScale;  // V/s rise rate — voltage-domain first-creation class scale
    settingWrite(NK_CvKdDeadbandVps, String(CvKdDeadbandVps, 3).c_str());
  } else {
    CvKdDeadbandVps = settingRead(NK_CvKdDeadbandVps).toFloat();
  }
  if (!settingExists(NK_CvKdDbSlope)) {
    settingWrite(NK_CvKdDbSlope, String(CvKdDbSlope, 5).c_str());  // default 0 = flat line; measured per install at Step 5
  } else {
    CvKdDbSlope = settingRead(NK_CvKdDbSlope).toFloat();
  }
  if (!settingExists(NK_CvKdDbFloor)) {
    CvKdDbFloor *= seedVScale;
    settingWrite(NK_CvKdDbFloor, String(CvKdDbFloor, 3).c_str());
  } else {
    CvKdDbFloor = settingRead(NK_CvKdDbFloor).toFloat();
  }
  if (!settingExists(NK_CvKdDbCeil)) {
    CvKdDbCeil *= seedVScale;
    settingWrite(NK_CvKdDbCeil, String(CvKdDbCeil, 3).c_str());
  } else {
    CvKdDbCeil = settingRead(NK_CvKdDbCeil).toFloat();
  }
  if (!settingExists(NK_CvKdExcessMode)) {
    settingWrite(NK_CvKdExcessMode, String((int)CvKdExcessMode).c_str());
  } else {
    CvKdExcessMode = settingRead(NK_CvKdExcessMode).toInt() != 0;
  }
  if (!settingExists(NK_VoltageKd)) {
    settingWrite(NK_VoltageKd, String(VoltageKd, 1).c_str());  // A/(V/s), 12V-equivalent — no seed scaling; runtime-normalized to VoltageKd_active like VoltageKp/Ki
  } else {
    VoltageKd = settingRead(NK_VoltageKd).toFloat();
  }
  if (!settingExists(NK_CvKdVoltFiltTC)) {
    settingWrite(NK_CvKdVoltFiltTC, String(CvKdVoltFiltTC, 0).c_str());  // ms — not class-scaled
  } else {
    CvKdVoltFiltTC = settingRead(NK_CvKdVoltFiltTC).toFloat();
  }
  if (!settingExists(NK_CvKdOneSided)) {
    settingWrite(NK_CvKdOneSided, String((int)CvKdOneSided).c_str());
  } else {
    CvKdOneSided = (bool)settingRead(NK_CvKdOneSided).toInt();
  }
  if (!settingExists(NK_CvKdMaxTrimA)) {
    settingWrite(NK_CvKdMaxTrimA, String(CvKdMaxTrimA, 1).c_str());  // A — flat, voltage-independent (like every amp setting); a per-cell-equivalent back-off is the same amps on every bank, so a flat cap keeps the knee per-cell-equal
  } else {
    CvKdMaxTrimA = settingRead(NK_CvKdMaxTrimA).toFloat();
  }
  if (!settingExists(NK_CvKdSlopeCeil)) {
    CvKdSlopeCeil *= seedVScale;  // V/s real per-bus (WYSIWYG) — first-creation class scale, like CvKdDbCeil
    settingWrite(NK_CvKdSlopeCeil, String(CvKdSlopeCeil, 1).c_str());
  } else {
    CvKdSlopeCeil = settingRead(NK_CvKdSlopeCeil).toFloat();
  }
  if (!settingExists(NK_CvStressDropV)) {
    settingWrite(NK_CvStressDropV, String(CvStressDropV, 2).c_str());  // 12V-equiv, class-scaled at use — no seed scaling
  } else {
    CvStressDropV = settingRead(NK_CvStressDropV).toFloat();
  }
  if (!settingExists(NK_CvStressFailBandV)) {
    settingWrite(NK_CvStressFailBandV, String(CvStressFailBandV, 2).c_str());
  } else {
    CvStressFailBandV = settingRead(NK_CvStressFailBandV).toFloat();
  }
  if (!settingExists(NK_CvKdTd)) {
    settingWrite(NK_CvKdTd, String(CvKdTd, 2).c_str());  // s — derivative time; Auto Kd = Td·Kp
  } else {
    CvKdTd = settingRead(NK_CvKdTd).toFloat();
  }
  // cvHelpersEnabled — master switch for the asymmetric KiDown unwind + the one-sided D term
  if (!settingExists(NK_cvHelpersEnabled)) {
    settingWrite(NK_cvHelpersEnabled, String((int)cvHelpersEnabled).c_str());
  } else {
    cvHelpersEnabled = settingRead(NK_cvHelpersEnabled).toInt() != 0;
  }
  // ── CV gain-mode system (Auto α/K plant-anchored vs Manual) + measured plant ──
  if (!settingExists(NK_cvGainMode)) {
    settingWrite(NK_cvGainMode, String((int)cvGainMode).c_str());
  } else {
    cvGainMode = (uint8_t)settingRead(NK_cvGainMode).toInt();
  }
  // CV crossover ω_c (retired/inert — auto gain is now α/K) — NVS key renamed cvOmega → cvCrossover (§F.3). Mint-new-key + migrate: adopt the old
  // value if present, folding in the misnamed-0.286 → 0.20 default fix (else ~24 % hot under the exact
  // magnitude formula), write the new key, and remove the orphaned old one. Dev-units-only window.
  if (settingExists(NK_cvCrossover)) {
    cvCrossover = settingRead(NK_cvCrossover).toFloat();
  } else if (settingExists("cvOmega")) {
    float old = settingRead("cvOmega").toFloat();
    cvCrossover = (fabsf(old - 0.286f) < 0.003f) ? 0.20f : old;
    settingWrite(NK_cvCrossover, String(cvCrossover, 3).c_str());
    settingRemove("cvOmega");
  } else {
    settingWrite(NK_cvCrossover, String(cvCrossover, 3).c_str());
  }
  // CV PI zero ρ — NVS key renamed cvKiRatio → cvPiZero. Same mint-new-key + migrate (value unchanged).
  if (settingExists(NK_cvPiZero)) {
    cvPiZero = settingRead(NK_cvPiZero).toFloat();
  } else if (settingExists("cvKiRatio")) {
    cvPiZero = settingRead("cvKiRatio").toFloat();
    settingWrite(NK_cvPiZero, String(cvPiZero, 3).c_str());
    settingRemove("cvKiRatio");
  } else {
    settingWrite(NK_cvPiZero, String(cvPiZero, 3).c_str());
  }
  if (!settingExists(NK_cvAlpha)) {
    settingWrite(NK_cvAlpha, String(cvAlpha, 3).c_str());
  } else {
    cvAlpha = settingRead(NK_cvAlpha).toFloat();
  }
  if (!settingExists(NK_vTgtRampEnable)) {
    settingWrite(NK_vTgtRampEnable, String((int)vTgtRampEnable).c_str());
  } else {
    vTgtRampEnable = (uint8_t)settingRead(NK_vTgtRampEnable).toInt();
  }
  if (!settingExists(NK_vTgtRampUp)) {
    vTgtRampUp *= seedVScale;
    settingWrite(NK_vTgtRampUp, String(vTgtRampUp, 3).c_str());
  } else {
    vTgtRampUp = settingRead(NK_vTgtRampUp).toFloat();
  }
  if (!settingExists(NK_vTgtRampDn)) {
    vTgtRampDn *= seedVScale;
    settingWrite(NK_vTgtRampDn, String(vTgtRampDn, 3).c_str());
  } else {
    vTgtRampDn = settingRead(NK_vTgtRampDn).toFloat();
  }
  if (!settingExists(NK_cvWindDownEn)) {
    settingWrite(NK_cvWindDownEn, String((int)cvWindDownEnable).c_str());
  } else {
    cvWindDownEnable = (uint8_t)settingRead(NK_cvWindDownEn).toInt();
  }
  if (!settingExists(NK_cvWindDownRate)) {
    settingWrite(NK_cvWindDownRate, String(cvWindDownRate, 3).c_str());
  } else {
    cvWindDownRate = settingRead(NK_cvWindDownRate).toFloat();
  }
  if (!settingExists(NK_cvWindDownStopV)) {
    cvWindDownStopV *= seedVScale;
    settingWrite(NK_cvWindDownStopV, String(cvWindDownStopV, 3).c_str());
  } else {
    cvWindDownStopV = settingRead(NK_cvWindDownStopV).toFloat();
  }
  if (!settingExists(NK_setpointSlewEnable)) {
    settingWrite(NK_setpointSlewEnable, String((int)setpointSlewEnable).c_str());
  } else {
    setpointSlewEnable = (uint8_t)settingRead(NK_setpointSlewEnable).toInt();
  }
  if (!settingExists(NK_cvRiseGovEnable)) {
    settingWrite(NK_cvRiseGovEnable, String((int)cvRiseGovEnable).c_str());
  } else {
    cvRiseGovEnable = (uint8_t)settingRead(NK_cvRiseGovEnable).toInt();
  }
  if (!settingExists(NK_cvRecovEnable)) {
    settingWrite(NK_cvRecovEnable, String((int)cvRecovEnable).c_str());
  } else {
    cvRecovEnable = (uint8_t)settingRead(NK_cvRecovEnable).toInt();
  }
  if (!settingExists(NK_loadServeBoostEnable)) {
    settingWrite(NK_loadServeBoostEnable, String((int)loadServeBoostEnable).c_str());
  } else {
    loadServeBoostEnable = (uint8_t)settingRead(NK_loadServeBoostEnable).toInt();
  }
  if (!settingExists(NK_HuntGovEnable)) {
    settingWrite(NK_HuntGovEnable, String((int)HuntGovEnable).c_str());
  } else {
    HuntGovEnable = (uint8_t)clampLoadedSetting("HuntGovEnable", NK_HuntGovEnable,
                                                settingRead(NK_HuntGovEnable).toInt(),
                                                HUNT_GOV_ENABLE_MIN, HUNT_GOV_ENABLE_MAX);
  }
  if (!settingExists(NK_HuntCutPct)) {
    settingWrite(NK_HuntCutPct, String((int)HuntCutPct).c_str());
  } else {
    HuntCutPct = (uint8_t)clampLoadedSetting("HuntCutPct", NK_HuntCutPct,
                                             settingRead(NK_HuntCutPct).toInt(),
                                             HUNT_CUT_PCT_MIN, HUNT_CUT_PCT_MAX);
  }
  if (!settingExists(NK_HuntVerifyPct)) {
    settingWrite(NK_HuntVerifyPct, String((int)HuntVerifyPct).c_str());
  } else {
    HuntVerifyPct = (uint8_t)clampLoadedSetting("HuntVerifyPct", NK_HuntVerifyPct,
                                                settingRead(NK_HuntVerifyPct).toInt(),
                                                HUNT_VERIFY_PCT_MIN, HUNT_VERIFY_PCT_MAX);
  }
  if (!settingExists(NK_HuntWingPct)) {
    settingWrite(NK_HuntWingPct, String((int)HuntWingPct).c_str());
  } else {
    HuntWingPct = (uint8_t)clampLoadedSetting("HuntWingPct", NK_HuntWingPct,
                                              settingRead(NK_HuntWingPct).toInt(),
                                              HUNT_WING_PCT_MIN, HUNT_WING_PCT_MAX);
  }
  if (!settingExists(NK_HuntCooldownMin)) {
    settingWrite(NK_HuntCooldownMin, String((int)HuntCooldownMin).c_str());
  } else {
    HuntCooldownMin = (uint8_t)clampLoadedSetting("HuntCooldownMin", NK_HuntCooldownMin,
                                                  settingRead(NK_HuntCooldownMin).toInt(),
                                                  HUNT_COOLDOWN_MIN_MIN, HUNT_COOLDOWN_MIN_MAX);
  }
  if (!settingExists(NK_HuntSteadyPct)) {
    settingWrite(NK_HuntSteadyPct, String((int)HuntSteadyPct).c_str());
  } else {
    HuntSteadyPct = (uint8_t)clampLoadedSetting("HuntSteadyPct", NK_HuntSteadyPct,
                                                settingRead(NK_HuntSteadyPct).toInt(),
                                                HUNT_STEADY_PCT_MIN, HUNT_STEADY_PCT_MAX);
  }
  if (!settingExists(NK_HuntQualifyScans)) {
    settingWrite(NK_HuntQualifyScans, String((int)HuntQualifyScans).c_str());
  } else {
    HuntQualifyScans = (uint8_t)clampLoadedSetting("HuntQualifyScans", NK_HuntQualifyScans,
                                                   settingRead(NK_HuntQualifyScans).toInt(),
                                                   HUNT_QUALIFY_SCANS_MIN, HUNT_QUALIFY_SCANS_MAX);
  }
  if (!settingExists(NK_HuntTrigPct)) {
    settingWrite(NK_HuntTrigPct, String(HuntTrigPct, 2).c_str());
  } else {
    // Floor only, no ceiling — see HUNT_TRIG_PCT_MIN. A garbage NVS string parses to 0.0, which would
    // make aPk > bar true on every scan, so the floor is the one thing the load path must enforce.
    HuntTrigPct = fmaxf(settingRead(NK_HuntTrigPct).toFloat(), HUNT_TRIG_PCT_MIN);
  }
  if (!settingExists(NK_reseedCorrEnable)) {
    settingWrite(NK_reseedCorrEnable, String((int)reseedCorrEnable).c_str());
  } else {
    reseedCorrEnable = (uint8_t)settingRead(NK_reseedCorrEnable).toInt();
  }
  // cvRecovSec / cvRecovEmaxV: retired timed-window knobs — seeds kept so the NVS keys stay stable (never repurpose)
  if (!settingExists(NK_cvRecovSec)) {
    settingWrite(NK_cvRecovSec, String(cvRecovSec, 2).c_str());
  } else {
    cvRecovSec = settingRead(NK_cvRecovSec).toFloat();
  }
  if (!settingExists(NK_cvRecovEmaxV)) {
    settingWrite(NK_cvRecovEmaxV, String(cvRecovEmaxV, 3).c_str());
  } else {
    cvRecovEmaxV = settingRead(NK_cvRecovEmaxV).toFloat();
  }
  if (!settingExists(NK_cvRecovKiMax)) {
    settingWrite(NK_cvRecovKiMax, String(cvRecovKiMax, 2).c_str());
  } else {
    cvRecovKiMax = settingRead(NK_cvRecovKiMax).toFloat();
  }
  if (!settingExists(NK_cvRecovBoostEnable)) {
    settingWrite(NK_cvRecovBoostEnable, String((int)cvRecovBoostEnable).c_str());
  } else {
    cvRecovBoostEnable = (uint8_t)settingRead(NK_cvRecovBoostEnable).toInt();
  }
  if (!settingExists(NK_cvRecovBoostMax)) {
    settingWrite(NK_cvRecovBoostMax, String(cvRecovBoostMax, 2).c_str());
  } else {
    cvRecovBoostMax = settingRead(NK_cvRecovBoostMax).toFloat();
  }
  if (!settingExists(NK_cvRecovBoostErrV)) {
    settingWrite(NK_cvRecovBoostErrV, String(cvRecovBoostErrV, 3).c_str());
  } else {
    cvRecovBoostErrV = settingRead(NK_cvRecovBoostErrV).toFloat();
  }
  if (!settingExists(NK_cvRecovBoostFloorV)) {
    settingWrite(NK_cvRecovBoostFloorV, String(cvRecovBoostFloorV, 3).c_str());
  } else {
    cvRecovBoostFloorV = settingRead(NK_cvRecovBoostFloorV).toFloat();
  }
  if (!settingExists(NK_cvRecovDeepBandV)) {
    settingWrite(NK_cvRecovDeepBandV, String(cvRecovDeepBandV, 3).c_str());
  } else {
    cvRecovDeepBandV = settingRead(NK_cvRecovDeepBandV).toFloat();
  }
  if (!settingExists(NK_cvRecovDeepMult)) {
    settingWrite(NK_cvRecovDeepMult, String(cvRecovDeepMult, 1).c_str());
  } else {
    cvRecovDeepMult = settingRead(NK_cvRecovDeepMult).toFloat();
  }
  if (!settingExists(NK_cvRecovFlareBandV)) {
    settingWrite(NK_cvRecovFlareBandV, String(cvRecovFlareBandV, 3).c_str());
  } else {
    cvRecovFlareBandV = settingRead(NK_cvRecovFlareBandV).toFloat();
  }
  if (!settingExists(NK_cvRecovFlareFrac)) {
    settingWrite(NK_cvRecovFlareFrac, String(cvRecovFlareFrac, 2).c_str());
  } else {
    cvRecovFlareFrac = settingRead(NK_cvRecovFlareFrac).toFloat();
  }
  if (!settingExists(NK_dutySlewEnable)) {
    settingWrite(NK_dutySlewEnable, String((int)dutySlewEnable).c_str());
  } else {
    dutySlewEnable = (uint8_t)settingRead(NK_dutySlewEnable).toInt();
  }
  if (!settingExists(NK_testSlewMode)) {
    settingWrite(NK_testSlewMode, String((int)testSlewMode).c_str());
  } else {
    testSlewMode = (uint8_t)settingRead(NK_testSlewMode).toInt();
  }
  if (!settingExists(NK_cvTestSlewMode)) {
    settingWrite(NK_cvTestSlewMode, String((int)cvTestSlewMode).c_str());
  } else {
    cvTestSlewMode = (uint8_t)settingRead(NK_cvTestSlewMode).toInt();
  }
  if (!settingExists(NK_cvPlantKa)) {
    cvPlantKa *= seedVScale;  // V/A plant stiffness is per-bus: n series 12V blocks ≈ n× the V/A
    settingWrite(NK_cvPlantKa, String(cvPlantKa, 5).c_str());
    settingWrite(NK_cvPlantKb, String(cvPlantKb, 5).c_str());
  } else {
    cvPlantKa = settingRead(NK_cvPlantKa).toFloat();
    cvPlantKb = settingExists(NK_cvPlantKb) ? settingRead(NK_cvPlantKb).toFloat() : 0.0f;
  }
  if (settingExists(NK_ripFitAlt))  ripFitDecode(settingRead(NK_ripFitAlt),  ripFitAlt);   // measured ripple projection (§3.3); absent → nPts=0 → plot shows threshold only
  if (settingExists(NK_slpFitAlt))  ripFitDecode(settingRead(NK_slpFitAlt),  slpFitAlt);   // measured voltage-slope projection (D-term deadband); same absent semantics
  // CommissionTempF — board temp stamped when the CV plant fit was applied; reference for the battery-
  // temp gain derate. No default write: absence = never commissioned = no derate (stays NaN).
  if (settingExists(NK_CommissionTempF)) {
    CommissionTempF = settingRead(NK_CommissionTempF).toFloat();
  }
  // CommissionTempSrc — battTempActiveSrc at that stamp; 0 = legacy/unknown = treated as the board class.
  if (!settingExists(NK_CommissionTempSrc)) {
    settingWrite(NK_CommissionTempSrc, String(CommissionTempSrc).c_str());
  } else {
    CommissionTempSrc = clampLoadedSetting("CommissionTempSrc", NK_CommissionTempSrc, settingRead(NK_CommissionTempSrc).toInt(), 0, 5);
  }
  // Same rule for the re-commission nag state: absence = never commissioned = never nag.
  if (settingExists(NK_CommissionEpoch)) {
    CommissionEpoch = (time_t)strtoll(settingRead(NK_CommissionEpoch).c_str(), nullptr, 10);
  }
  if (settingExists(NK_cmAgeAck))     commissionAgeAck     = settingRead(NK_cmAgeAck).toInt() != 0;
  if (settingExists(NK_cmChangeFlag)) commissionChangeFlag = settingRead(NK_cmChangeFlag).toInt() != 0;
  // Receipt from an admin config push that applied on the PREVIOUS boot ("<n>|<key,key,...>").
  // Survives here until the dashboard acks it, so an owner who was not watching at the moment
  // of the push still learns their settings were changed remotely.
  if (settingExists(NK_cfgPushNotify)) {
    String rec = settingRead(NK_cfgPushNotify);
    int bar = rec.indexOf('|');
    if (bar > 0) {
      cfgPushAppliedCount = (uint8_t)rec.substring(0, bar).toInt();
      cfgPushAppliedKeys  = rec.substring(bar + 1);
    }
  }
  if (!settingExists(NK_battTempDerateEn)) {
    settingWrite(NK_battTempDerateEn, String((int)battTempDerateEnable).c_str());
  } else {
    battTempDerateEnable = settingRead(NK_battTempDerateEn).toInt() != 0;
  }
  if (!settingExists(NK_battTempCoeff)) {
    settingWrite(NK_battTempCoeff, String(battTempCoeff, 4).c_str());
  } else {
    battTempCoeff = settingRead(NK_battTempCoeff).toFloat();
  }
  // coldChargeLockoutEnable — master on/off for the board-temp cold-charge lockout (lithium protection)
  if (!settingExists(NK_coldChargeLockoutEnable)) {
    settingWrite(NK_coldChargeLockoutEnable, String((int)coldChargeLockoutEnable).c_str());
  } else {
    coldChargeLockoutEnable = settingRead(NK_coldChargeLockoutEnable).toInt() != 0;
  }
  // MinChargeTempF — board-temp floor below which charging is locked out (°F)
  if (!settingExists(NK_MinChargeTempF)) {
    settingWrite(NK_MinChargeTempF, String(MinChargeTempF).c_str());
  } else {
    MinChargeTempF = settingRead(NK_MinChargeTempF).toFloat();
  }
  // Battery + extra temperature probes, battery-temperature source chain, hot-charge lockout
  // (BATTERY_TEMP_SENSORS_SPEC.md §6)
  if (!settingExists(NK_battTempProbeEnable)) {
    settingWrite(NK_battTempProbeEnable, String(battTempProbeEnable).c_str());
  } else {
    battTempProbeEnable = settingRead(NK_battTempProbeEnable).toInt() != 0 ? 1 : 0;
  }
  if (!settingExists(NK_extraTempProbeEnable)) {
    settingWrite(NK_extraTempProbeEnable, String(extraTempProbeEnable).c_str());
  } else {
    extraTempProbeEnable = settingRead(NK_extraTempProbeEnable).toInt() != 0 ? 1 : 0;
  }
  if (!settingExists(NK_battTempSource)) {
    settingWrite(NK_battTempSource, String(battTempSource).c_str());
  } else {
    battTempSource = clampLoadedSetting("battTempSource", NK_battTempSource, settingRead(NK_battTempSource).toInt(), 0, 6);
  }
  if (!settingExists(NK_battTempProxyEnable)) {
    settingWrite(NK_battTempProxyEnable, String(battTempProxyEnable).c_str());
  } else {
    battTempProxyEnable = settingRead(NK_battTempProxyEnable).toInt() != 0 ? 1 : 0;
  }
  if (!settingExists(NK_hotChargeLockoutEnable)) {
    settingWrite(NK_hotChargeLockoutEnable, String(hotChargeLockoutEnable).c_str());
  } else {
    hotChargeLockoutEnable = settingRead(NK_hotChargeLockoutEnable).toInt() != 0 ? 1 : 0;
  }
  // MaxChargeTempF — battery-temperature ceiling above which charging is locked out (°F, measured sources only)
  if (!settingExists(NK_MaxChargeTempF)) {
    settingWrite(NK_MaxChargeTempF, String(MaxChargeTempF).c_str());
  } else {
    MaxChargeTempF = settingRead(NK_MaxChargeTempF).toFloat();
  }
  if (!settingExists(NK_extraTempAlarmHiEnable)) {
    settingWrite(NK_extraTempAlarmHiEnable, String(extraTempAlarmHiEnable).c_str());
  } else {
    extraTempAlarmHiEnable = settingRead(NK_extraTempAlarmHiEnable).toInt() != 0 ? 1 : 0;
  }
  if (!settingExists(NK_extraTempAlarmHiF)) {
    settingWrite(NK_extraTempAlarmHiF, String(extraTempAlarmHiF).c_str());
  } else {
    extraTempAlarmHiF = settingRead(NK_extraTempAlarmHiF).toFloat();
  }
  if (!settingExists(NK_extraTempAlarmLoEnable)) {
    settingWrite(NK_extraTempAlarmLoEnable, String(extraTempAlarmLoEnable).c_str());
  } else {
    extraTempAlarmLoEnable = settingRead(NK_extraTempAlarmLoEnable).toInt() != 0 ? 1 : 0;
  }
  if (!settingExists(NK_extraTempAlarmLoF)) {
    settingWrite(NK_extraTempAlarmLoF, String(extraTempAlarmLoF).c_str());
  } else {
    extraTempAlarmLoF = settingRead(NK_extraTempAlarmLoF).toFloat();
  }
  // 1-Wire role addresses: 16-char lowercase hex ROM code per role, "" = unassigned. Loaded straight into
  // tempRoleAddr[]; owEnumerate() (initializeHardware, later TempTask) binds the bus slots.
  {
    static const char *const owRoleKeys[TR_COUNT] = { NK_owAddrAlt, NK_owAddrBatt, NK_owAddrExtra };
    for (int r = 0; r < TR_COUNT; r++) {
      if (!settingExists(owRoleKeys[r])) {
        settingWrite(owRoleKeys[r], "");
        continue;
      }
      String hex = settingRead(owRoleKeys[r]);
      if (!owHexToAddr(hex.c_str(), tempRoleAddr[r])) memset(tempRoleAddr[r], 0, sizeof(DeviceAddress));
      tempRoleAssigned[r] = !owAddrIsZero(tempRoleAddr[r]);
    }
  }
  if (!settingExists(NK_CvKdArmV)) {
    CvKdArmV *= seedVScale;
    settingWrite(NK_CvKdArmV, String(CvKdArmV, 2).c_str());
  } else {
    CvKdArmV = settingRead(NK_CvKdArmV).toFloat();
  }
  if (!settingExists(NK_PidKp)) {
    settingWrite(NK_PidKp, String(PidKp, 3).c_str());
  } else {
    PidKp = settingRead(NK_PidKp).toFloat();
  }
  if (!settingExists(NK_TempPIDKp)) {
    settingWrite(NK_TempPIDKp, String(TempPIDKp, 6).c_str());
  } else {
    TempPIDKp = settingRead(NK_TempPIDKp).toFloat();
  }
  if (!settingExists(NK_ThermalLookaheadSec)) {
    settingWrite(NK_ThermalLookaheadSec, String(ThermalLookaheadSec, 1).c_str());
  } else {
    ThermalLookaheadSec = max(0.0f, settingRead(NK_ThermalLookaheadSec).toFloat());
  }

  if (!settingExists(NK_ThermalSlopeWindowSec)) {
    settingWrite(NK_ThermalSlopeWindowSec, String(ThermalSlopeWindowSec, 1).c_str());
  } else {
    ThermalSlopeWindowSec = constrain(settingRead(NK_ThermalSlopeWindowSec).toFloat(), 10.0f, 60.0f);
  }

  if (!settingExists(NK_TempPIDKi)) {
    settingWrite(NK_TempPIDKi, String(TempPIDKi, 6).c_str());
  } else {
    TempPIDKi = settingRead(NK_TempPIDKi).toFloat();
  }

  if (!settingExists(NK_TempPIDKiDownFrac)) {
    settingWrite(NK_TempPIDKiDownFrac, String(TempPIDKiDownFrac, 3).c_str());
  } else {
    TempPIDKiDownFrac = settingRead(NK_TempPIDKiDownFrac).toFloat();
  }


  if (!settingExists(NK_TempPIDIntervalMs)) {
    settingWrite(NK_TempPIDIntervalMs, String(TempPIDIntervalMs).c_str());
  } else {
    TempPIDIntervalMs = settingRead(NK_TempPIDIntervalMs).toInt();
  }

  if (!settingExists(NK_TempPIDFilterAlpha)) {
    settingWrite(NK_TempPIDFilterAlpha, String(TempPIDFilterAlpha, 3).c_str());
  } else {
    TempPIDFilterAlpha = settingRead(NK_TempPIDFilterAlpha).toFloat();
  }


  if (!settingExists(NK_PidKi)) {
    settingWrite(NK_PidKi, String(PidKi, 3).c_str());
  } else {
    PidKi = settingRead(NK_PidKi).toFloat();
  }
  if (!settingExists(NK_PidKd)) {
    settingWrite(NK_PidKd, String(PidKd, 3).c_str());
  } else {
    PidKd = settingRead(NK_PidKd).toFloat();
  }
  if (!settingExists(NK_DutySlowRampRate)) {
    DutySlowRampRate *= seedDScale;
    settingWrite(NK_DutySlowRampRate, String(DutySlowRampRate, 2).c_str());
  } else {
    DutySlowRampRate = settingRead(NK_DutySlowRampRate).toFloat();
  }
  if (!settingExists(NK_ShutdownPhase2HoldMs)) {
    settingWrite(NK_ShutdownPhase2HoldMs, String(ShutdownPhase2HoldMs).c_str());
  } else {
    ShutdownPhase2HoldMs = settingRead(NK_ShutdownPhase2HoldMs).toInt();
  }
  if (!settingExists(NK_PidSampleDivisor)) {
    settingWrite(NK_PidSampleDivisor, String(PidSampleDivisor).c_str());
  } else {
    PidSampleDivisor = settingRead(NK_PidSampleDivisor).toInt();
  }
  if (!settingExists(NK_LearningSettlingPeriod)) {
    settingWrite(NK_LearningSettlingPeriod, String(LearningSettlingPeriod).c_str());
  } else {
    LearningSettlingPeriod = settingRead(NK_LearningSettlingPeriod).toInt();
  }
  if (!settingExists(NK_LearningRPMChangeThreshold)) {
    settingWrite(NK_LearningRPMChangeThreshold, String(LearningRPMChangeThreshold).c_str());
  } else {
    LearningRPMChangeThreshold = settingRead(NK_LearningRPMChangeThreshold).toInt();
  }
  if (!settingExists(NK_LearningTempHysteresis)) {
    settingWrite(NK_LearningTempHysteresis, String(LearningTempHysteresis).c_str());
  } else {
    LearningTempHysteresis = settingRead(NK_LearningTempHysteresis).toInt();
  }
  if (!settingExists(NK_MaxTableValue)) {
    settingWrite(NK_MaxTableValue, String(MaxTableValue, 2).c_str());
  } else {
    MaxTableValue = settingRead(NK_MaxTableValue).toFloat();
  }
  HardOCTripAmps = MaxTableValue + 10.0f;  // always derived, not persisted
  if (!settingExists(NK_MaxPenaltyPercent)) {
    settingWrite(NK_MaxPenaltyPercent, String(MaxPenaltyPercent, 2).c_str());
  } else {
    MaxPenaltyPercent = settingRead(NK_MaxPenaltyPercent).toFloat();
  }
  if (!settingExists(NK_MaxPenaltyDuration)) {
    settingWrite(NK_MaxPenaltyDuration, String(MaxPenaltyDuration).c_str());
  } else {
    MaxPenaltyDuration = settingRead(NK_MaxPenaltyDuration).toInt();
  }
  if (!settingExists(NK_NeighborLearningFactor)) {
    settingWrite(NK_NeighborLearningFactor, String(NeighborLearningFactor, 3).c_str());
  } else {
    NeighborLearningFactor = settingRead(NK_NeighborLearningFactor).toFloat();
  }
  if (!settingExists(NK_yyMax)) {
    settingWrite(NK_yyMax, String(yyMax).c_str());
  } else {
    yyMax = settingRead(NK_yyMax).toInt();
  }
  if (!settingExists(NK_LearningMemoryDuration)) {
    settingWrite(NK_LearningMemoryDuration, String(LearningMemoryDuration).c_str());
  } else {
    LearningMemoryDuration = settingRead(NK_LearningMemoryDuration).toInt();
  }
  if (!settingExists(NK_DutyRampRate)) {
    // Duty-domain knob stored in REAL %/s for THIS bus. The hardcoded default is a 12V value, so on
    // first creation scale it by ×(12/SYSTEM_VOLTAGE_CLASS) (40%/s → 10%/s @48V) before persisting — the
    // field-current slew stays constant across banks, and the dashboard box shows what's actually used.
    DutyRampRate = DutyRampRate * 12.0f / (float)SYSTEM_VOLTAGE_CLASS;
    settingWrite(NK_DutyRampRate, String(DutyRampRate, 1).c_str());
  } else {
    DutyRampRate = settingRead(NK_DutyRampRate).toFloat();
  }
  if (!settingExists(NK_SettleTimeBeforeCut)) {
    settingWrite(NK_SettleTimeBeforeCut, String(SettleTimeBeforeCut).c_str());
  } else {
    SettleTimeBeforeCut = settingRead(NK_SettleTimeBeforeCut).toInt();
  }
  if (!settingExists(NK_TempWarnExcess)) {
    settingWrite(NK_TempWarnExcess, String(TempWarnExcess, 1).c_str());
  } else {
    TempWarnExcess = settingRead(NK_TempWarnExcess).toFloat();
  }
  if (!settingExists(NK_TempCritExcess)) {
    settingWrite(NK_TempCritExcess, String(TempCritExcess, 1).c_str());
  } else {
    TempCritExcess = settingRead(NK_TempCritExcess).toFloat();
  }
  if (!settingExists(NK_TempSustainedTimeout)) {
    settingWrite(NK_TempSustainedTimeout, String(TempSustainedTimeout).c_str());
  } else {
    TempSustainedTimeout = settingRead(NK_TempSustainedTimeout).toInt();
  }
  // Over-voltage ladder absolute rungs. Both are persisted user settings; the two instant cuts
  // chain top-down from the battery's BMS charge-disconnect voltage (guidance: hardware limit
  // 0.2 V x class/12 below the BMS floor, software cut 0.1 x class/12 below the hardware limit) so
  // software always gets first shot and the INA228 pin is purely the electrical backstop.
  // VoltageHardwareLimit — INA228 ALERT comparator threshold (instant, electrical).
  // First-boot seed is chemistry-aware (lithium 14.3 x class/12 — 0.2 under the surveyed BMS trip
  // floor; else 16.0 x class/12 — protects DC loads, not the battery); the commissioning proposal
  // refines it. Seeded BEFORE AlternatorHardShutdownV so the software-cut fallback can chain from it.
  if (!settingExists(NK_VoltageHardwareLimit)) {
    VoltageHardwareLimit = (batteryIsLithium() ? 14.3f : 16.0f) * seedVScale;
    settingWrite(NK_VoltageHardwareLimit, String(VoltageHardwareLimit, 2).c_str());
  } else {
    VoltageHardwareLimit = settingRead(NK_VoltageHardwareLimit).toFloat();
  }
  // AlternatorHardShutdownV — software instant cut (absolute, all modes, raw per-tick).
  // First-boot fallback rides one rung under the hardware limit. Once written, the value is
  // treated as user-set; a later system-class change rescales both rungs x ratio in
  // applyNominalVoltageChange (chain-preserving).
  if (!settingExists(NK_AlternatorHardShutdownV)) {
    AlternatorHardShutdownV = VoltageHardwareLimit - 0.1f * seedVScale;
    settingWrite(NK_AlternatorHardShutdownV, String(AlternatorHardShutdownV, 2).c_str());
  } else {
    AlternatorHardShutdownV = settingRead(NK_AlternatorHardShutdownV).toFloat();
  }
  // Ladder-order coherence at boot. The /get handler enforces sw < hw on every write, but a
  // device upgrading from the pre-ladder firmware boots with its old NVS software cut (lithium
  // Bulk + 0.5 = 14.4) ABOVE the freshly seeded hardware rung (14.3) — a silently inverted
  // ladder where the INA228 pin fires first. Same rule as the handler: the hardware rung is the
  // anchor; the software cut is clamped 0.05 x class/12 under it and persisted. Also repairs an
  // inverted pair arriving through the config-import path (applyImportConfig writes NVS raw).
  {
    float swMax = VoltageHardwareLimit - 0.05f * seedVScale;
    if (AlternatorHardShutdownV > swMax) {
      AlternatorHardShutdownV = swMax;
      settingWrite(NK_AlternatorHardShutdownV, String(AlternatorHardShutdownV, 2).c_str());
      queueConsoleMessageF("Alternator hard-shutdown lowered to %.2fV at boot — must stay below the Hardware Shutdown Voltage (%.2fV)",
                           AlternatorHardShutdownV, VoltageHardwareLimit);
    }
  }
  if (!settingExists(NK_HardOCDebounceMs)) {
    settingWrite(NK_HardOCDebounceMs, String(HardOCDebounceMs).c_str());
  } else {
    HardOCDebounceMs = (uint32_t)settingRead(NK_HardOCDebounceMs).toInt();
  }
  if (!settingExists(NK_WarmupRampRate)) {
    settingWrite(NK_WarmupRampRate, String(WarmupRampRate, 2).c_str());
  } else {
    WarmupRampRate = max(0.0f, settingRead(NK_WarmupRampRate).toFloat());
  }
  if (!settingExists(NK_IExcessFrac)) {
    settingWrite(NK_IExcessFrac, String(IExcessFrac, 3).c_str());
  } else {
    IExcessFrac = settingRead(NK_IExcessFrac).toFloat();
  }
  if (!settingExists(NK_IExcessFracBulk)) {
    settingWrite(NK_IExcessFracBulk, String(IExcessFracBulk, 3).c_str());
  } else {
    IExcessFracBulk = settingRead(NK_IExcessFracBulk).toFloat();
  }
  if (!settingExists(NK_IExcessFloorA)) {
    settingWrite(NK_IExcessFloorA, String(IExcessFloorA, 1).c_str());
  } else {
    IExcessFloorA = settingRead(NK_IExcessFloorA).toFloat();
  }
  if (!settingExists(NK_IExcessCeilA)) {
    settingWrite(NK_IExcessCeilA, String(IExcessCeilA, 1).c_str());
  } else {
    IExcessCeilA = settingRead(NK_IExcessCeilA).toFloat();
  }
  if (!settingExists(NK_IExcessBaseA)) {
    settingWrite(NK_IExcessBaseA, String(IExcessBaseA, 1).c_str());
  } else {
    IExcessBaseA = settingRead(NK_IExcessBaseA).toFloat();
  }
  if (!settingExists(NK_IExcessCcOffsetA)) {
    settingWrite(NK_IExcessCcOffsetA, String(IExcessCcOffsetA, 1).c_str());
  } else {
    IExcessCcOffsetA = settingRead(NK_IExcessCcOffsetA).toFloat();
  }
  if (!settingExists(NK_BattCurrentLimitA)) {
    settingWrite(NK_BattCurrentLimitA, String(BattCurrentLimitA, 1).c_str());
  } else {
    BattCurrentLimitA = settingRead(NK_BattCurrentLimitA).toFloat();
  }
  if (!settingExists(NK_IExcessTau)) {
    settingWrite(NK_IExcessTau, String(IExcessTau, 1).c_str());
  } else {
    IExcessTau = settingRead(NK_IExcessTau).toFloat();
  }
  if (!settingExists(NK_IExcessRelFrac)) {
    settingWrite(NK_IExcessRelFrac, String(IExcessRelFrac, 3).c_str());
  } else {
    IExcessRelFrac = settingRead(NK_IExcessRelFrac).toFloat();
  }
  if (!settingExists(NK_IExcessKBleed)) {
    settingWrite(NK_IExcessKBleed, String(IExcessKBleed, 2).c_str());
  } else {
    IExcessKBleed = settingRead(NK_IExcessKBleed).toFloat();
  }
  // Auto-commissioning state (default 0 = NOT_COMMISSIONED on a virgin device).
  if (settingExists(NK_commissionState)) {
    commissionState = (uint8_t)settingRead(NK_commissionState).toInt();
  }
  // Current commissioning phase (default 0 = Prep; moves backward on Back). Drives the tab checklist.
  if (settingExists(NK_commissionPhase)) {
    commissionPhase = (uint8_t)settingRead(NK_commissionPhase).toInt();
  }
  // Per-stage completion bitmask (default 0 = nothing done). Loaded BEFORE the in-progress
  // revert below so that revert can also clear it. This is the source of truth for the
  // per-step ✓ marks; commissionState is derived from it.
  if (settingExists(NK_commissionDoneMask)) {
    commissionDoneMask = (uint16_t)settingRead(NK_commissionDoneMask).toInt();
  }
  // Per-stage set-by-hand bitmask (skip / mark-done-manually). Default 0.
  if (settingExists(NK_commissionManualMask)) {
    commissionManualMask = (uint16_t)settingRead(NK_commissionManualMask).toInt();
  }
  // A reboot mid-wizard must KEEP already-finished steps — only the step that was running when power
  // was lost is un-trustworthy. The in-flight step snapshot (NK_commissionStepSnap, re-taken on each
  // step entry) holds the scalar tune as of that step's start, so restoring it undoes exactly that one
  // step while every finished step stays intact. The done/manual masks and IN_PROGRESS state are left
  // as-is so the badge invites "Continue commissioning" rather than starting over. The Phase-0 origin
  // snapshot (NK_commissionSnap) is deliberately NOT touched here — it stays so a later explicit Abort
  // can still fully revert to the pre-commissioning tune.
  // NOTE: a COMMITTED-but-partial device (e.g. commissioned, then binned stages invalidated by a
  // tach rescale) is also state==1 but has NEITHER snapshot — it persists untouched and keeps
  // nagging, which is correct.
  if (commissionState == 1 && settingExists(NK_commissionStepSnap)) {
    commissionRestoreScalars(NK_commissionStepSnap);  // undo just the interrupted step's scalar applies
    settingRemove(NK_commissionStepSnap);
  }
  if (settingExists(NK_cvStressLast)) {
    strlcpy(cvsLastBlob, settingRead(NK_cvStressLast).c_str(), sizeof(cvsLastBlob));
  }
  if (!settingExists(NK_IExcessArmMarginV)) {
    IExcessArmMarginV *= seedVScale;
    settingWrite(NK_IExcessArmMarginV, String(IExcessArmMarginV, 3).c_str());
  } else {
    IExcessArmMarginV = settingRead(NK_IExcessArmMarginV).toFloat();
  }
  if (!settingExists(NK_AwBleedRate)) {
    settingWrite(NK_AwBleedRate, String(AwBleedRate, 2).c_str());
  } else {
    AwBleedRate = settingRead(NK_AwBleedRate).toFloat();
  }
  // AwRecoverRate is hardcoded (0.1f) — not persisted
  if (!settingExists(NK_AwSeedProtectMs)) {
    settingWrite(NK_AwSeedProtectMs, String(AwSeedProtectMs).c_str());
  } else {
    AwSeedProtectMs = (uint16_t)settingRead(NK_AwSeedProtectMs).toInt();
  }
  if (!settingExists(NK_FastSetpointRiseRate)) {
    settingWrite(NK_FastSetpointRiseRate, String(FastSetpointRiseRate, 1).c_str());
  } else {
    FastSetpointRiseRate = settingRead(NK_FastSetpointRiseRate).toFloat();
  }
  if (!settingExists(NK_FastSetpointRiseWindowMs)) {
    settingWrite(NK_FastSetpointRiseWindowMs, String(FastSetpointRiseWindowMs).c_str());
  } else {
    FastSetpointRiseWindowMs = (uint32_t)settingRead(NK_FastSetpointRiseWindowMs).toInt();
  }
  if (!settingExists(NK_FastSetpointRiseHeadroomV)) {
    FastSetpointRiseHeadroomV *= seedVScale;
    settingWrite(NK_FastSetpointRiseHeadroomV, String(FastSetpointRiseHeadroomV, 2).c_str());
  } else {
    FastSetpointRiseHeadroomV = settingRead(NK_FastSetpointRiseHeadroomV).toFloat();
  }
  if (!settingExists(NK_KHard)) {
    KHard *= seedDScale;
    settingWrite(NK_KHard, String(KHard, 1).c_str());
  } else {
    KHard = settingRead(NK_KHard).toFloat();
  }
  if (settingExists(NK_ReseedFrac)) {
    ReseedFrac = settingRead(NK_ReseedFrac).toFloat();
  } else {
    settingWrite(NK_ReseedFrac, String(ReseedFrac, 2).c_str());
  }
  if (settingExists(NK_ReseedFracNS)) {
    ReseedFracNoShunt = settingRead(NK_ReseedFracNS).toFloat();
  } else {
    settingWrite(NK_ReseedFracNS, String(ReseedFracNoShunt, 2).c_str());
  }
  if (settingExists(NK_CvRecovClimb)) {
    CvRecovClimbRate = settingRead(NK_CvRecovClimb).toFloat();
  } else {
    settingWrite(NK_CvRecovClimb, String(CvRecovClimbRate, 2).c_str());
  }
  if (settingExists(NK_OvGroup1Enable)) {
    OvGroup1Enable = settingRead(NK_OvGroup1Enable).toInt() != 0;
  } else {
    settingWrite(NK_OvGroup1Enable, String((int)OvGroup1Enable).c_str());
  }
  if (settingExists(NK_OvGroup2Enable)) {
    OvGroup2Enable = settingRead(NK_OvGroup2Enable).toInt() != 0;
  } else {
    settingWrite(NK_OvGroup2Enable, String((int)OvGroup2Enable).c_str());
  }
  if (!settingExists(NK_HardOCEnable)) {
    settingWrite(NK_HardOCEnable, String((int)HardOCEnable).c_str());
  } else {
    HardOCEnable = settingRead(NK_HardOCEnable).toInt() != 0;
  }
  if (!settingExists(NK_TachLieEnable)) {
    settingWrite(NK_TachLieEnable, String((int)TachLieEnable).c_str());
  } else {
    TachLieEnable = settingRead(NK_TachLieEnable).toInt() != 0;
  }
  if (!settingExists(NK_IExcessEnable)) {
    settingWrite(NK_IExcessEnable, String((int)IExcessEnable).c_str());
  } else {
    IExcessEnable = settingRead(NK_IExcessEnable).toInt() != 0;
  }
  if (!settingExists(NK_BattLimitEnable)) {
    settingWrite(NK_BattLimitEnable, String((int)BattLimitEnable).c_str());
  } else {
    BattLimitEnable = settingRead(NK_BattLimitEnable).toInt() != 0;
  }
  if (!settingExists(NK_LoadDumpEnable)) {
    settingWrite(NK_LoadDumpEnable, String((int)LoadDumpEnable).c_str());
  } else {
    LoadDumpEnable = settingRead(NK_LoadDumpEnable).toInt() != 0;
  }
  if (!settingExists(NK_OutputPIDSigSrc)) {
    settingWrite(NK_OutputPIDSigSrc, String(OutputPIDSigSrc).c_str());
  } else {
    OutputPIDSigSrc = constrain(settingRead(NK_OutputPIDSigSrc).toInt(), 0, 2);
  }
  if (!settingExists(NK_OutputPIDMA_N)) {
    settingWrite(NK_OutputPIDMA_N, String(OutputPIDMA_N).c_str());
  } else {
    OutputPIDMA_N = constrain(settingRead(NK_OutputPIDMA_N).toInt(), 1, I_RING_SIZE);
  }
  if (!settingExists(NK_OutputPIDFilterTC)) {
    settingWrite(NK_OutputPIDFilterTC, String(OutputPIDFilterTC).c_str());
  } else {
    OutputPIDFilterTC = settingRead(NK_OutputPIDFilterTC).toFloat();
  }
  if (!settingExists(NK_VoltageFilterTC)) {
    settingWrite(NK_VoltageFilterTC, String(VoltageFilterTC).c_str());
  } else {
    VoltageFilterTC = settingRead(NK_VoltageFilterTC).toFloat();
  }
  if (!settingExists(NK_TdPred)) {
    settingWrite(NK_TdPred, String(TdPred, 3).c_str());
  } else {
    TdPred = settingRead(NK_TdPred).toFloat();
  }
  if (!settingExists(NK_OvMeasMarginV)) {
    OvMeasMarginV *= seedVScale;
    settingWrite(NK_OvMeasMarginV, String(OvMeasMarginV, 3).c_str());
  } else {
    OvMeasMarginV = settingRead(NK_OvMeasMarginV).toFloat();
  }
  if (!settingExists(NK_OvPredMarginV)) {
    OvPredMarginV *= seedVScale;
    settingWrite(NK_OvPredMarginV, String(OvPredMarginV, 3).c_str());
  } else {
    OvPredMarginV = settingRead(NK_OvPredMarginV).toFloat();
  }
  // Timed OV tier margins are volt-domain (x class at seed); dwells are absolute time, never scaled.
  if (!settingExists(NK_OvTierLoMarginV)) {
    OvTierLoMarginV *= seedVScale;
    settingWrite(NK_OvTierLoMarginV, String(OvTierLoMarginV, 3).c_str());
  } else {
    OvTierLoMarginV = settingRead(NK_OvTierLoMarginV).toFloat();
  }
  if (!settingExists(NK_OvTierLoDwellMs)) {
    settingWrite(NK_OvTierLoDwellMs, String(OvTierLoDwellMs).c_str());
  } else {
    OvTierLoDwellMs = (uint32_t)settingRead(NK_OvTierLoDwellMs).toInt();
  }
  if (!settingExists(NK_OvTierMidMarginV)) {
    OvTierMidMarginV *= seedVScale;
    settingWrite(NK_OvTierMidMarginV, String(OvTierMidMarginV, 3).c_str());
  } else {
    OvTierMidMarginV = settingRead(NK_OvTierMidMarginV).toFloat();
  }
  if (!settingExists(NK_OvTierMidDwellMs)) {
    settingWrite(NK_OvTierMidDwellMs, String(OvTierMidDwellMs).c_str());
  } else {
    OvTierMidDwellMs = (uint32_t)settingRead(NK_OvTierMidDwellMs).toInt();
  }
  if (settingExists(NK_DvdtTC)) {
    DvdtTC = constrain(settingRead(NK_DvdtTC).toFloat(), 5.0f, 500.0f);
  } else {
    settingWrite(NK_DvdtTC, String(DvdtTC, 1).c_str());
  }
  if (!settingExists(NK_VoltageDisagreeThreshold)) {
    VoltageDisagreeThreshold *= seedVScale;
    settingWrite(NK_VoltageDisagreeThreshold, String(VoltageDisagreeThreshold, 2).c_str());
  } else {
    VoltageDisagreeThreshold = settingRead(NK_VoltageDisagreeThreshold).toFloat();
  }
  if (!settingExists(NK_VoltageDisagreeTimeout)) {
    settingWrite(NK_VoltageDisagreeTimeout, String(VoltageDisagreeTimeout).c_str());
  } else {
    VoltageDisagreeTimeout = settingRead(NK_VoltageDisagreeTimeout).toInt();
  }
  // Alternator-health settings load via altSettingsLoad() in setup.

  if (!settingExists(NK_VoltageKp)) {
    settingWrite(NK_VoltageKp, String(VoltageKp, 1).c_str());
  } else {
    VoltageKp = settingRead(NK_VoltageKp).toFloat();
  }
  // Derive the active CV gains now that gain mode, λ, plant, and manual Kp/Ki are all loaded.
  recomputeCvGains();
  recomputeCcGains();  // and the active CC (output-current) gains, normalized to SYSTEM_VOLTAGE_CLASS
  if (!settingExists(NK_VoltageLoopInterval)) {
    settingWrite(NK_VoltageLoopInterval, String(VoltageLoopInterval).c_str());
  } else {
    VoltageLoopInterval = settingRead(NK_VoltageLoopInterval).toInt();
  }
  if (!settingExists(NK_FIELD_COLLAPSE_DELAY)) {
    settingWrite(NK_FIELD_COLLAPSE_DELAY, String(FIELD_COLLAPSE_DELAY).c_str());
  } else {
    FIELD_COLLAPSE_DELAY = settingRead(NK_FIELD_COLLAPSE_DELAY).toInt();
  }
  // IMU safety thresholds — user-set via form, persisted (Pattern B).
  // imuMountOrientation rides on the Vessel Info record, written as one batch (separate path).
  if (!settingExists(NK_CAPSIZE_THRESHOLD_DEG)) {
    settingWrite(NK_CAPSIZE_THRESHOLD_DEG, String(CAPSIZE_THRESHOLD_DEG, 1).c_str());
  } else {
    CAPSIZE_THRESHOLD_DEG = settingRead(NK_CAPSIZE_THRESHOLD_DEG).toFloat();
  }
  if (!settingExists(NK_PITCHPOLE_THRESHOLD_DEG)) {
    settingWrite(NK_PITCHPOLE_THRESHOLD_DEG, String(PITCHPOLE_THRESHOLD_DEG, 1).c_str());
  } else {
    PITCHPOLE_THRESHOLD_DEG = settingRead(NK_PITCHPOLE_THRESHOLD_DEG).toFloat();
  }
  if (!settingExists(NK_SLAM_THRESHOLD_G)) {
    settingWrite(NK_SLAM_THRESHOLD_G, String(SLAM_THRESHOLD_G, 2).c_str());
  } else {
    SLAM_THRESHOLD_G = settingRead(NK_SLAM_THRESHOLD_G).toFloat();
  }

  // Fast alt-current diagnostic knobs (Pattern B). Defaults = the #define values, so a fresh
  // device behaves exactly as before the knobs existed. faInit() also reads NK_faEnabled
  // directly (it runs before this), but writing the default here creates the key on day one.
  if (!settingExists(NK_faEnabled)) {
    settingWrite(NK_faEnabled, faEnabled ? "1" : "0");
  } else {
    faEnabled = (settingRead(NK_faEnabled).toInt() != 0);
  }
  // WiFi Napping standby toggle (Client-mode only; default off)
  if (!settingExists(NK_wifiNapEnabled)) {
    settingWrite(NK_wifiNapEnabled, wifiNapEnabled ? "1" : "0");
  } else {
    wifiNapEnabled = (settingRead(NK_wifiNapEnabled).toInt() != 0);
  }
  if (!settingExists(NK_faAlarmEnable)) {
    settingWrite(NK_faAlarmEnable, faAlarmEnable ? "1" : "0");
  } else {
    faAlarmEnable = (settingRead(NK_faAlarmEnable).toInt() != 0);
  }
  if (!settingExists(NK_faAnomPause)) {
    settingWrite(NK_faAnomPause, faAnomPause ? "1" : "0");
  } else {
    faAnomPause = (settingRead(NK_faAnomPause).toInt() != 0);
  }
  if (!settingExists(NK_faRpmEdgeMargin)) {
    settingWrite(NK_faRpmEdgeMargin, String(faRpmEdgeMargin, 1).c_str());
  } else {
    faRpmEdgeMargin = settingRead(NK_faRpmEdgeMargin).toFloat();
  }
  if (!settingExists(NK_faAmpsDriftFloorA)) {
    settingWrite(NK_faAmpsDriftFloorA, String(faAmpsDriftFloorA, 2).c_str());
  } else {
    faAmpsDriftFloorA = settingRead(NK_faAmpsDriftFloorA).toFloat();
  }
  if (!settingExists(NK_faAmpsDriftPct)) {
    settingWrite(NK_faAmpsDriftPct, String(faAmpsDriftPct, 1).c_str());
  } else {
    faAmpsDriftPct = settingRead(NK_faAmpsDriftPct).toFloat();
  }
  // Measured-ripple capture admission gates (§10.8/§11). NVS key "ripRpmMargin" is abandoned — never reuse.
  if (!settingExists(NK_ripWinMs)) {
    settingWrite(NK_ripWinMs, String(ripWinMs, 0).c_str());
  } else {
    // §11 halves need ≥1 disturbance cycle each — floor pre-§11 stored values (250–500 ms era) up to the new minimum
    ripWinMs = fmaxf(500.0f, settingRead(NK_ripWinMs).toFloat());
  }
  if (!settingExists(NK_ripDriftFloorA)) {
    settingWrite(NK_ripDriftFloorA, String(ripDriftFloorA, 2).c_str());
  } else {
    ripDriftFloorA = settingRead(NK_ripDriftFloorA).toFloat();
  }
  if (!settingExists(NK_ripDriftPct)) {
    settingWrite(NK_ripDriftPct, String(ripDriftPct, 1).c_str());
  } else {
    ripDriftPct = settingRead(NK_ripDriftPct).toFloat();
  }
  if (!settingExists(NK_faAttenUpAmps)) {
    settingWrite(NK_faAttenUpAmps, String(faAttenUpAmps, 1).c_str());
  } else {
    faAttenUpAmps = settingRead(NK_faAttenUpAmps).toFloat();
  }
  if (!settingExists(NK_faAttenDownAmps)) {
    settingWrite(NK_faAttenDownAmps, String(faAttenDownAmps, 1).c_str());
  } else {
    faAttenDownAmps = settingRead(NK_faAttenDownAmps).toFloat();
  }
  if (!settingExists(NK_faPeakMinA)) {
    settingWrite(NK_faPeakMinA, String(faPeakMinA, 2).c_str());
  } else {
    faPeakMinA = settingRead(NK_faPeakMinA).toFloat();
  }
}

// ===== IMU Ring Buffer Helper Functions =====
inline void pushAccelSample(int16_t x, int16_t y, int16_t z, uint32_t timestamp_us) {
  uint16_t next_head = (imuRingBuffer->accel_head + 1) % ACCEL_RING_SIZE;

  if (next_head == imuRingBuffer->accel_tail) {
    // Ring buffer full - drop oldest sample (move tail forward)
    imuRingBuffer->accel_tail = (imuRingBuffer->accel_tail + 1) % ACCEL_RING_SIZE;
    imuRingBuffer->accel_dropped++;
  }

  imuRingBuffer->accel[imuRingBuffer->accel_head].x = x;
  imuRingBuffer->accel[imuRingBuffer->accel_head].y = y;
  imuRingBuffer->accel[imuRingBuffer->accel_head].z = z;
  imuRingBuffer->accel[imuRingBuffer->accel_head].timestamp_us = timestamp_us;
  imuRingBuffer->accel_head = next_head;

  imu_total_samples_accel++;
}

inline void pushGyroSample(int16_t x, int16_t y, int16_t z, uint32_t timestamp_us) {
  uint16_t next_head = (imuRingBuffer->gyro_head + 1) % GYRO_RING_SIZE;

  if (next_head == imuRingBuffer->gyro_tail) {
    // Ring buffer full - drop oldest sample (move tail forward)
    imuRingBuffer->gyro_tail = (imuRingBuffer->gyro_tail + 1) % GYRO_RING_SIZE;
    imuRingBuffer->gyro_dropped++;
  }

  imuRingBuffer->gyro[imuRingBuffer->gyro_head].x = x;
  imuRingBuffer->gyro[imuRingBuffer->gyro_head].y = y;
  imuRingBuffer->gyro[imuRingBuffer->gyro_head].z = z;
  imuRingBuffer->gyro[imuRingBuffer->gyro_head].timestamp_us = timestamp_us;
  imuRingBuffer->gyro_head = next_head;

  imu_total_samples_gyro++;
}

// ============================================================================
// IMU CONVERSION CONSTANTS
// ============================================================================
// LSM6DSOX sensitivity values (from datasheet)
// Accel: ±2g range → 0.061 mg/LSB → 0.000061 g/LSB
// Gyro: ±2000 dps range → 70 mdps/LSB → 0.070 dps/LSB
constexpr float ACCEL_SCALE = 0.000061f;  // g per LSB
constexpr float GYRO_SCALE = 0.070f;      // dps per LSB

// Axis remapping: maps sensor axes to vessel axes for each mounting orientation.
// src[i] = which sensor axis (0=X, 1=Y, 2=Z) feeds vessel axis i
// sign[i] = +1 or -1
// Index order: [0]=accel_fwd, [1]=accel_stbd, [2]=accel_up,
//              [3]=gyro_pitch, [4]=gyro_heel,  [5]=gyro_yaw
// All four orientations VERIFIED from physical tilt tests.
struct AxisRemap {
  uint8_t src[6];
  int8_t sign[6];
};

const AxisRemap axisRemap[4] = {
  { { 2, 0, 1, 2, 0, 1 }, { -1, -1, -1, -1, -1, -1 } },  // 0: Fwd bulkhead, facing aft
  { { 2, 0, 1, 2, 0, 1 }, { +1, +1, -1, +1, +1, -1 } },  // 1: Aft bulkhead, facing fwd
  { { 0, 2, 1, 0, 2, 1 }, { -1, +1, -1, -1, +1, -1 } },  // 2: Port wall, facing stbd
  { { 0, 2, 1, 0, 2, 1 }, { +1, -1, -1, +1, -1, -1 } },  // 3: Stbd wall, facing port
};
static_assert(sizeof(axisRemap) / sizeof(axisRemap[0]) == IMU_ORIENT_COUNT,
              "IMU_ORIENT_COUNT must match the axisRemap[] row count");


// Complementary filter parameters
constexpr float CF_ALPHA = 0.90f;  // Gyro weight (0.90 = trust gyro 90%, accel 10%) → time constant ~0.19 s; may show some wave flutter at sea

constexpr uint16_t ROLLING_WINDOW_SIZE = 60;  // 1 sample per second
struct RollingWindow {
  float heel[ROLLING_WINDOW_SIZE];
  float pitch[ROLLING_WINDOW_SIZE];
  uint16_t index = 0;
  uint16_t count = 0;  // How many valid samples (0-60)
} rolling60s;

// 120-second rolling window for anchor display (roll/pitch deviation + heading swing)
// heading entries: actual degrees [0,360] when fresh, -1.0 when compass stale
constexpr uint16_t ROLLING_WINDOW_120 = 120;
struct RollingWindow120 {
  float heel[ROLLING_WINDOW_120];
  float pitch[ROLLING_WINDOW_120];
  float heading[ROLLING_WINDOW_120];
  uint16_t index = 0;
  uint16_t count = 0;
} rolling120s;

// ===== IMU METRIC PROCESSING FUNCTIONS =====
void resetAccelStats() {
  // Reset all period statistics (called after uploading to Supabase)
  imu_accel_x_min = 999.0;
  imu_accel_x_max = -999.0;
  imu_accel_x_sum = 0;
  imu_accel_x_count = 0;

  imu_accel_y_min = 999.0;
  imu_accel_y_max = -999.0;
  imu_accel_y_sum = 0;
  imu_accel_y_count = 0;

  imu_accel_z_min = 999.0;
  imu_accel_z_max = -999.0;
  imu_accel_z_sum = 0;
  imu_accel_z_count = 0;

  imu_gyro_x_min = 999.0;
  imu_gyro_x_max = -999.0;
  imu_gyro_x_sum = 0;
  imu_gyro_x_count = 0;

  imu_gyro_y_min = 999.0;
  imu_gyro_y_max = -999.0;
  imu_gyro_y_sum = 0;
  imu_gyro_y_count = 0;

  imu_gyro_z_min = 999.0;
  imu_gyro_z_max = -999.0;
  imu_gyro_z_sum = 0;
  imu_gyro_z_count = 0;

  imu_heel_min = 999.0;
  imu_heel_max = -999.0;
  imu_heel_sum = 0;
  imu_heel_count = 0;

  imu_pitch_min = 999.0;
  imu_pitch_max = -999.0;
  imu_pitch_sum = 0;
  imu_pitch_count = 0;

  imu_vertical_accel_min = 999.0;
  imu_vertical_accel_max = -999.0;
  imu_vertical_accel_sum = 0;
  imu_vertical_accel_count = 0;

  imu_total_accel_min = 999.0;
  imu_total_accel_max = -999.0;
  imu_total_accel_sum = 0;
  imu_total_accel_count = 0;

  imu_slam_count = 0;
  imu_slam_peak_max = 0;
}
void complementaryFilter(float ax, float ay, float az, float gx, float gy, float dt) {
  // Accel-derived angles (noisy but no drift)
  float accel_heel = atan2(ay, sqrt(ax * ax + az * az)) * 180.0f / PI - imuHeelOffsetDeg;
  float accel_pitch = atan2(-ax, sqrt(ay * ay + az * az)) * 180.0f / PI - imuPitchOffsetDeg;

  // First sample after boot seeds the filter from accel directly. Starting from
  // 0 made the estimate slew to the true attitude over several seconds, and the
  // long-term ring recorded that sweep as a 0-to-actual min/max wedge in the
  // first bucket. (IMU-zero capture still snaps to 0 by design — that's a new
  // level reference, not a restart.)
  static bool cf_seeded = false;
  if (!cf_seeded) {
    cf_seeded = true;
    cf_heel = accel_heel;
    cf_pitch = accel_pitch;
    return;
  }

  // Integrate gyro (smooth but drifts)
  cf_heel += gy * dt;
  cf_pitch += gx * dt;

  cf_heel = CF_ALPHA * cf_heel + (1.0f - CF_ALPHA) * accel_heel;
  cf_pitch = CF_ALPHA * cf_pitch + (1.0f - CF_ALPHA) * accel_pitch;
}
// Finalize IMU zero: normally once both windows hit the target (~2s @104Hz). The timeout
// margin guarantees the capture always resolves — if samples arrive slowly it finalizes on
// whatever it gathered (provided the IMU_ZERO_MIN floor), otherwise it aborts cleanly. Either
// way settingsDirty fires so the CSV3 echo refreshes the dashboard "calculating" label at once.
// Shared by the normal metrics path and the pre-vessel-info path (metrics gated, calibration live).
void imuZeroFinalizeIfDue() {
  if (!imuZeroInProgress) return;
  bool targetMet = (imuZeroAccelN >= IMU_ZERO_TARGET && imuZeroGyroN >= IMU_ZERO_TARGET);
  bool timedOut  = (millis() - imuZeroStartMs >= IMU_ZERO_TIMEOUT_MS);
  if (targetMet || timedOut) {
    if (imuZeroAccelN >= IMU_ZERO_MIN && imuZeroGyroN >= IMU_ZERO_MIN) {
      float ax0 = imuZeroAxSum / imuZeroAccelN;
      float ay0 = imuZeroAySum / imuZeroAccelN;
      float az0 = imuZeroAzSum / imuZeroAccelN;
      imuHeelOffsetDeg = atan2(ay0, sqrt(ax0 * ax0 + az0 * az0)) * 180.0f / PI;
      imuPitchOffsetDeg = atan2(-ax0, sqrt(ay0 * ay0 + az0 * az0)) * 180.0f / PI;
      imuGxBias = imuZeroGxSum / imuZeroGyroN;
      imuGyBias = imuZeroGySum / imuZeroGyroN;
      imuGzBias = imuZeroGzSum / imuZeroGyroN;

      DynamicJsonDocument zdoc(256);
      zdoc["heel_offset_deg"] = imuHeelOffsetDeg;
      zdoc["pitch_offset_deg"] = imuPitchOffsetDeg;
      zdoc["gx_bias"] = imuGxBias;
      zdoc["gy_bias"] = imuGyBias;
      zdoc["gz_bias"] = imuGzBias;
      String zout;
      serializeJson(zdoc, zout);
      settingWrite(NK_imu_zero, zout.c_str());

      cf_heel = 0; cf_pitch = 0;  // snap filter to new level reference (feels instant)
      imuZeroCaptured = true;
      // The install verdict is decided here, from the capture the user just took with the boat level and
      // still — the one moment az is unambiguous. az0 keeps its sign, so a board mounted upside-down
      // (az0 = -1 g, both offsets ≈ 0) is caught, which a magnitude test would miss.
      float aMag = sqrtf(ax0 * ax0 + ay0 * ay0 + az0 * az0);
      imuMountState = (aMag >= IMU_MOUNT_TACC_MIN_G && aMag <= IMU_MOUNT_TACC_MAX_G
                       && az0 >= IMU_MOUNT_VACC_MIN_G) ? IMU_MOUNT_OK : IMU_MOUNT_BAD;
      settingWrite(NK_imu_mnt_state, String((int)imuMountState).c_str());

      queueConsoleMessage("IMU ZERO: captured (heel " + String(imuHeelOffsetDeg, 1) +
                          "°, pitch " + String(imuPitchOffsetDeg, 1) +
                          (targetMet ? "°)" : "°, partial — timed out)"));
      // Warn, never refuse. A capture taken while heeled bakes that heel in permanently, and no later window
      // check can see it — the offsets cancel it at rest and the vertical axis still reads 1 g.
      if (imuMountState == IMU_MOUNT_BAD) {
        queueConsoleMessage("IMU: install check failed (up axis " + String(az0, 2) + " g, |a| " +
                            String(aMag, 2) + " g). Heel, pitch and slam are flagged suspicious.");
        queueConsoleMessage("IMU: mount the regulator on a vertical bulkhead, level the boat, and Zero again.");
      }
      if (fabsf(imuHeelOffsetDeg) > IMU_ZERO_WARN_DEG || fabsf(imuPitchOffsetDeg) > IMU_ZERO_WARN_DEG)
        queueConsoleMessage("IMU ZERO: large offset captured — if the boat was not level and still, re-zero at the dock.");
      if (fabsf(aMag - 1.0f) > IMU_ZERO_WARN_AMAG_G)
        queueConsoleMessage("IMU ZERO: motion during capture (mean |a| = " + String(aMag, 3) +
                            " g, expected 1.000). Offsets may be wrong — re-zero when the boat is still.");
    } else {
      // Not enough samples even after the timeout (IMU barely streaming) — abort, keep old offsets.
      queueConsoleMessage("IMU ZERO: aborted — too few samples (is the IMU streaming?)");
    }
    imuZeroInProgress = false;
    settingsDirty = true;   // fire CSV3 now so the echo lands ~immediately, not on the 60s heartbeat
  }
}

void updateAccelMetrics() {
  // Called from main loop every iteration (~300 Hz at 3ms loop time, NOT 1 Hz).
  // Most calls find 0 new samples (arrival rate is 104 Hz post-ODR-drop) and exit quickly;
  // accel_cap=50 limits per-call work when a backlog exists. Timer reports sub-ms worst,
  // which integer-divides to 0 on the dashboard — function IS running (Ring Usage stays 0%).

  if (!imuEnabled) return;
  if (!imuRingBuffer || !imuWindow) { imuEnabled = false; return; }
  if (!vesselInfoSaved) {
    // Metrics are gated until the first Vessel Info save — anything recorded before the mount
    // orientation is known would put garbage heel/slam/sea-state into the NVS lifetime stats.
    // The level-calibration capture must still see samples, so accumulate its sums while draining.
    const AxisRemap &rg = axisRemap[imuMountOrientation];
    while (imuRingBuffer->accel_tail != imuRingBuffer->accel_head) {
      IMUSample *s = &imuRingBuffer->accel[imuRingBuffer->accel_tail];
      if (imuZeroInProgress && imuZeroAccelN < IMU_ZERO_TARGET) {
        float raw_a[3] = { (float)s->x, (float)s->y, (float)s->z };
        imuZeroAxSum += raw_a[rg.src[0]] * ACCEL_SCALE * rg.sign[0];
        imuZeroAySum += raw_a[rg.src[1]] * ACCEL_SCALE * rg.sign[1];
        imuZeroAzSum += raw_a[rg.src[2]] * ACCEL_SCALE * rg.sign[2];
        imuZeroAccelN++;
      }
      imuRingBuffer->accel_tail = (imuRingBuffer->accel_tail + 1) % ACCEL_RING_SIZE;
    }
    while (imuRingBuffer->gyro_tail != imuRingBuffer->gyro_head) {
      IMUSample *s = &imuRingBuffer->gyro[imuRingBuffer->gyro_tail];
      if (imuZeroInProgress && imuZeroGyroN < IMU_ZERO_TARGET) {
        float raw_g[3] = { (float)s->x, (float)s->y, (float)s->z };
        imuZeroGxSum += raw_g[rg.src[3]] * GYRO_SCALE * rg.sign[3];
        imuZeroGySum += raw_g[rg.src[4]] * GYRO_SCALE * rg.sign[4];
        imuZeroGzSum += raw_g[rg.src[5]] * GYRO_SCALE * rg.sign[5];
        imuZeroGyroN++;
      }
      imuRingBuffer->gyro_tail = (imuRingBuffer->gyro_tail + 1) % GYRO_RING_SIZE;
    }
    imuZeroFinalizeIfDue();
    return;
  }

  uint32_t samples_processed = 0;
  unsigned long now = millis();

  // Wave period decimation state (10 Hz output, real-time gated)
  static float wave_decim_sum = 0;
  static uint16_t wave_decim_count = 0;
  static unsigned long wave_last_decim_ms = 0;
  static bool wave_dc_initialized = false;
  static float msi_rms2_ewma = 0;    // 60s RMS² EWMA for MSI frequency-weighted vertical accel
  static bool gps_was_moving = false; // hysteresis state: ON above 1.7 kt, OFF below 1.3 kt

  // Process accel ring buffer — cap per call as a safety net against edge cases
  uint16_t accel_cap = 0;
  while (imuRingBuffer->accel_tail != imuRingBuffer->accel_head && accel_cap < 50) {
    accel_cap++;
    IMUSample *s = &imuRingBuffer->accel[imuRingBuffer->accel_tail];

    // Calculate time delta for area accumulation
    uint64_t now_us = s->timestamp_us;
    uint64_t dt_us = (imuWindow->lastUpdateTime_us > 0)
                       ? (now_us - imuWindow->lastUpdateTime_us)
                       : 0;

    // Convert to engineering units with full axis permutation + sign remapping
    const AxisRemap &r = axisRemap[imuMountOrientation];
    float raw_a[3] = { (float)s->x, (float)s->y, (float)s->z };
    float ax = raw_a[r.src[0]] * ACCEL_SCALE * r.sign[0];  // vessel forward
    float ay = raw_a[r.src[1]] * ACCEL_SCALE * r.sign[1];  // vessel starboard
    float az = raw_a[r.src[2]] * ACCEL_SCALE * r.sign[2];  // vessel up

    // IMU zero capture — accumulate at-rest accel for level reference
    if (imuZeroInProgress && imuZeroAccelN < IMU_ZERO_TARGET) {
      imuZeroAxSum += ax; imuZeroAySum += ay; imuZeroAzSum += az;
      imuZeroAccelN++;
    }

    // Scale to integers (1.234g → 1234)
    int32_t ax_scaled = (int32_t)(ax * 1000.0f);
    int32_t ay_scaled = (int32_t)(ay * 1000.0f);
    int32_t az_scaled = (int32_t)(az * 1000.0f);

    // Update window statistics with time-weighted averaging
    if (dt_us > 0 && dt_us < 100000) {  // Sanity check: 0-100ms
      if (ax_scaled < imuWindow->accel_x_min) imuWindow->accel_x_min = ax_scaled;
      if (ax_scaled > imuWindow->accel_x_max) imuWindow->accel_x_max = ax_scaled;
      imuWindow->accel_x_area_v_us += (int64_t)ax_scaled * dt_us;
      imuWindow->accel_x_valid_us += dt_us;

      if (ay_scaled < imuWindow->accel_y_min) imuWindow->accel_y_min = ay_scaled;
      if (ay_scaled > imuWindow->accel_y_max) imuWindow->accel_y_max = ay_scaled;
      imuWindow->accel_y_area_v_us += (int64_t)ay_scaled * dt_us;
      imuWindow->accel_y_valid_us += dt_us;

      if (az_scaled < imuWindow->accel_z_min) imuWindow->accel_z_min = az_scaled;
      if (az_scaled > imuWindow->accel_z_max) imuWindow->accel_z_max = az_scaled;
      imuWindow->accel_z_area_v_us += (int64_t)az_scaled * dt_us;
      imuWindow->accel_z_valid_us += dt_us;

      float total_accel = sqrt(ax * ax + ay * ay + az * az);
      int32_t total_accel_scaled = (int32_t)(total_accel * 1000.0f);
      if (total_accel_scaled < imuWindow->total_accel_min) imuWindow->total_accel_min = total_accel_scaled;
      if (total_accel_scaled > imuWindow->total_accel_max) imuWindow->total_accel_max = total_accel_scaled;
      imuWindow->total_accel_area_v_us += (int64_t)total_accel_scaled * dt_us;
      imuWindow->total_accel_valid_us += dt_us;

      // Vertical acceleration (Z-axis in vessel frame)
      float vert_accel = az;
      int32_t vert_accel_scaled = az_scaled;  // Already scaled above
      if (vert_accel_scaled < imuWindow->vertical_accel_min) imuWindow->vertical_accel_min = vert_accel_scaled;
      if (vert_accel_scaled > imuWindow->vertical_accel_max) imuWindow->vertical_accel_max = vert_accel_scaled;
      imuWindow->vertical_accel_area_v_us += (int64_t)vert_accel_scaled * dt_us;
      imuWindow->vertical_accel_valid_us += dt_us;

      // Slam detection — refractory period prevents multi-counting a single physical event
      static uint32_t lastSlamMs = 0;
      if (vert_accel > SLAM_THRESHOLD_G) {
        if ((uint32_t)now - lastSlamMs >= 300) {
          lastSlamMs = (uint32_t)now;
          imuWindow->slam_count++;
          // A non-vertical board's "up" axis isn't up, so its slam count is meaningless — keep it out of the
          // lifetime total that feeds the leaderboard. The window value still uploads, flagged suspicious.
          if (imuMountState != IMU_MOUNT_BAD) imu_slam_count_lifetime++;
        }
        // Always update peak — capture the worst reading within the event
        if (vert_accel_scaled > imuWindow->slam_peak_max) imuWindow->slam_peak_max = vert_accel_scaled;
        if (imuMountState != IMU_MOUNT_BAD && vert_accel > imu_slam_peak_lifetime) imu_slam_peak_lifetime = vert_accel;
      }
    }

    // Accumulate az for wave period decimation (emitted every 100ms after the loop)
    wave_decim_sum += az;
    wave_decim_count++;

    // Store current raw values (last sample processed, for display)
    imu_accel_x_raw = ax;
    imu_accel_y_raw = ay;
    imu_accel_z_raw = az;
    imu_vertical_accel_g = az;
    imu_total_accel_g = sqrt(ax * ax + ay * ay + az * az);
    wmIgnUpdate(wmIgn_vacc, imu_vertical_accel_g);  // ignition-cycle watermark

    // Wave period decimation and processing is handled inline below (10 Hz decimation, zero-crossing)

    imuWindow->lastUpdateTime_us = now_us;
    imuRingBuffer->accel_tail = (imuRingBuffer->accel_tail + 1) % ACCEL_RING_SIZE;
    samples_processed++;
  }

  // GPS speed gate — ON above 1.7 kt, OFF below 1.3 kt (hysteresis prevents threshold chatter)
  bool gps_is_moving = (SOGNMEA > 1.7f) || (gps_was_moving && SOGNMEA >= 1.3f);
  if (gps_is_moving != gps_was_moving) {
    // Both scores reset to zero on any mode switch — no cross-contamination between trips and anchorings
    msi_rms2_ewma = 0.0f;
    if (!gps_is_moving) {
      // Underway → anchored: flush rolling window so passage motion doesn't skew comfort score
      rolling60s.index = 0;
      rolling60s.count = 0;
      rolling120s.index = 0;
      rolling120s.count = 0;
      imu_heading_swing_120s = -1.0f;
    }
    // (Anchored → underway: msi_rms2_ewma already reset above; scores will read 0.0 until EWMA builds)
    gps_was_moving = gps_is_moving;
  }

  // Wave period detection — emit one decimated sample every 100ms (10 Hz)
  if (wave_decim_count > 0 && (now - wave_last_decim_ms) >= 100) {
    float az_dec = wave_decim_sum / wave_decim_count;
    wave_last_decim_ms = now;
    wave_decim_sum = 0;
    wave_decim_count = 0;

    // DC removal: slow EWMA with ~30s time constant at 10 Hz (alpha = 1 - 1/300 samples)
    if (!wave_dc_initialized) {
      wave_last_dc_mean = az_dec;
      wave_dc_initialized = true;
    } else {
      wave_last_dc_mean = 0.9967f * wave_last_dc_mean + 0.0033f * az_dec;
    }
    float detrended = az_dec - wave_last_dc_mean;

    // Motion Sickness Index — L&G 1987 frequency-weighted vertical accel RMS
    // Weighting function peaks at 0.2 Hz (5s wave period); falls off as a Gaussian in log-freq space
    {
      float W_e;
      if (imu_wave_period_sec > 0) {
        float f = 1.0f / imu_wave_period_sec;
        float log_ratio = logf(f / 0.2f);  // log(f/0.2); 0 at 5s, ±1.5 at ~1s/15s
        W_e = 1.0f / (1.0f + log_ratio * log_ratio * 4.43f);
      } else {
        W_e = 0.5f;  // mid-range estimate when wave period is unknown
      }
      float a_w = detrended * W_e;
      if (gps_is_moving) {
        // 60s EWMA at 10 Hz: alpha = 1/(10 × 60) = 0.00167
        msi_rms2_ewma = 0.9983f * msi_rms2_ewma + 0.0017f * (a_w * a_w);
      }
      // Always publish scores so JS can display a grayed value at anchor; EWMA just doesn't accumulate there
      {
        float a_w_rms = sqrtf(msi_rms2_ewma);
        // L&G 1987 calibration: 0.03g weighted RMS → ~10% vomiting in 2hrs
        imu_msi_score = (a_w_rms / 0.03f) * 30.0f;
        // Power-law approx of L&G empirical population curve — labeled as approximate
        imu_vomit_pct = constrain(100.0f * powf(a_w_rms / 0.03f, 1.3f) * 0.10f, 0.0f, 100.0f);
      }
    }

    // Zero-crossing with hysteresis — prevents noise-triggered false crossings
    // wave_last_crossing_positive: true = currently in positive zone (last crossed up through +HYST)
    const float WAVE_HYST = 0.02f;  // ±20 mg band
    bool crossing = false;
    if (!wave_last_crossing_positive && detrended > WAVE_HYST) {
      wave_last_crossing_positive = true;
      crossing = true;
    } else if (wave_last_crossing_positive && detrended < -WAVE_HYST) {
      wave_last_crossing_positive = false;
      crossing = true;
    }

    if (crossing) {
      if (wave_last_crossing_time > 0) {
        // Each crossing is a half-period; full period = 2 × half-period
        float half_period_s = (now - wave_last_crossing_time) / 1000.0f;
        float period_s = half_period_s * 2.0f;
        if (period_s >= 2.0f && period_s <= 20.0f) {
          if (wave_period_ewma < 0) {
            wave_period_ewma = period_s;
          } else {
            wave_period_ewma = 0.8f * wave_period_ewma + 0.2f * period_s;
          }
          imu_wave_period_sec = wave_period_ewma;
        }
      }
      wave_last_crossing_time = now;
    }

    // Invalidate if no crossing detected for >30s (boat stopped, no waves, or signal lost)
    if (wave_last_crossing_time > 0 && (now - wave_last_crossing_time) > 30000) {
      imu_wave_period_sec = -1.0f;
      wave_period_ewma = -1.0f;
      wave_last_crossing_time = 0;
    }
  }

  // Process gyro ring buffer — cap per call as a safety net
  uint16_t gyro_cap = 0;
  while (imuRingBuffer->gyro_tail != imuRingBuffer->gyro_head && gyro_cap < 20) {
    gyro_cap++;
    IMUSample *s = &imuRingBuffer->gyro[imuRingBuffer->gyro_tail];

    // Calculate time delta (uses separate gyro tracker, not shared with accel)
    uint64_t now_us = s->timestamp_us;
    uint64_t dt_us = (imuWindow->lastGyroUpdateTime_us > 0)
                       ? (now_us - imuWindow->lastGyroUpdateTime_us)
                       : 0;

    // Convert to engineering units with full axis permutation + sign remapping
    const AxisRemap &r = axisRemap[imuMountOrientation];
    float raw_g[3] = { (float)s->x, (float)s->y, (float)s->z };
    float gx = raw_g[r.src[3]] * GYRO_SCALE * r.sign[3];  // vessel pitch rate
    float gy = raw_g[r.src[4]] * GYRO_SCALE * r.sign[4];  // vessel heel rate
    float gz = raw_g[r.src[5]] * GYRO_SCALE * r.sign[5];  // vessel yaw rate

    // IMU zero capture — accumulate raw (pre-bias) gyro to measure rest bias
    if (imuZeroInProgress && imuZeroGyroN < IMU_ZERO_TARGET) {
      imuZeroGxSum += gx; imuZeroGySum += gy; imuZeroGzSum += gz;
      imuZeroGyroN++;
    }
    // Apply stored rest bias to live gyro
    gx -= imuGxBias; gy -= imuGyBias; gz -= imuGzBias;

    // Scale to integers (12.34 dps → 1234)
    int32_t gx_scaled = (int32_t)(gx * 100.0f);
    int32_t gy_scaled = (int32_t)(gy * 100.0f);
    int32_t gz_scaled = (int32_t)(gz * 100.0f);

    // Update window statistics
    if (dt_us > 0 && dt_us < 100000) {  // Sanity check
      if (gx_scaled < imuWindow->gyro_x_min) imuWindow->gyro_x_min = gx_scaled;
      if (gx_scaled > imuWindow->gyro_x_max) imuWindow->gyro_x_max = gx_scaled;
      imuWindow->gyro_x_area_v_us += (int64_t)gx_scaled * dt_us;
      imuWindow->gyro_x_valid_us += dt_us;

      if (gy_scaled < imuWindow->gyro_y_min) imuWindow->gyro_y_min = gy_scaled;
      if (gy_scaled > imuWindow->gyro_y_max) imuWindow->gyro_y_max = gy_scaled;
      imuWindow->gyro_y_area_v_us += (int64_t)gy_scaled * dt_us;
      imuWindow->gyro_y_valid_us += dt_us;

      if (gz_scaled < imuWindow->gyro_z_min) imuWindow->gyro_z_min = gz_scaled;
      if (gz_scaled > imuWindow->gyro_z_max) imuWindow->gyro_z_max = gz_scaled;
      imuWindow->gyro_z_area_v_us += (int64_t)gz_scaled * dt_us;
      imuWindow->gyro_z_valid_us += dt_us;
    }

    // Store current raw values
    imu_gyro_x_raw = gx;
    imu_gyro_y_raw = gy;
    imu_gyro_z_raw = gz;
    imu_yaw_rate_dps = gz;

    // Update complementary filter with latest accel + this gyro sample
    float dt_sec = dt_us / 1000000.0f;
    if (dt_sec > 0 && dt_sec < 1.0f) {  // Sanity check
      complementaryFilter(imu_accel_x_raw, imu_accel_y_raw, imu_accel_z_raw,
                          gx, gy, dt_sec);

      // Update heel/pitch window stats (scaled by 100: 12.34° → 1234)
      int32_t heel_scaled = (int32_t)(cf_heel * 100.0f);
      int32_t pitch_scaled = (int32_t)(cf_pitch * 100.0f);

      if (heel_scaled < imuWindow->heel_min) imuWindow->heel_min = heel_scaled;
      if (heel_scaled > imuWindow->heel_max) imuWindow->heel_max = heel_scaled;
      imuWindow->heel_area_v_us += (int64_t)heel_scaled * dt_us;
      imuWindow->heel_valid_us += dt_us;

      if (pitch_scaled < imuWindow->pitch_min) imuWindow->pitch_min = pitch_scaled;
      if (pitch_scaled > imuWindow->pitch_max) imuWindow->pitch_max = pitch_scaled;
      imuWindow->pitch_area_v_us += (int64_t)pitch_scaled * dt_us;
      imuWindow->pitch_valid_us += dt_us;
    }
    cf_lastUpdate = s->timestamp_us;

    imuWindow->lastGyroUpdateTime_us = now_us;  // separate from accel tracker (a shared one would zero the gyro dt)
    imuRingBuffer->gyro_tail = (imuRingBuffer->gyro_tail + 1) % GYRO_RING_SIZE;
  }

  imuZeroFinalizeIfDue();

  imu_heel_deg = cf_heel;
  imu_pitch_deg = cf_pitch;
  wmIgnUpdate(wmIgn_heel,  imu_heel_deg);   // ignition-cycle watermarks
  wmIgnUpdate(wmIgn_pitch, imu_pitch_deg);

  // Update 60s rolling window (called at ~1Hz from main loop)
  static unsigned long last60sUpdate = 0;
  if (now - last60sUpdate >= 1000) {
    last60sUpdate = now;

    rolling60s.heel[rolling60s.index] = cf_heel;
    rolling60s.pitch[rolling60s.index] = cf_pitch;
    rolling60s.index = (rolling60s.index + 1) % ROLLING_WINDOW_SIZE;
    if (rolling60s.count < ROLLING_WINDOW_SIZE) rolling60s.count++;

    // Compute 60s change/deviation metrics
    if (rolling60s.count > 10) {  // Need some samples
      float heel_min_60s = 999.0, heel_max_60s = -999.0, heel_sum_60s = 0;
      float pitch_min_60s = 999.0, pitch_max_60s = -999.0, pitch_sum_60s = 0;

      for (uint16_t i = 0; i < rolling60s.count; i++) {
        if (rolling60s.heel[i] < heel_min_60s) heel_min_60s = rolling60s.heel[i];
        if (rolling60s.heel[i] > heel_max_60s) heel_max_60s = rolling60s.heel[i];
        heel_sum_60s += rolling60s.heel[i];

        if (rolling60s.pitch[i] < pitch_min_60s) pitch_min_60s = rolling60s.pitch[i];
        if (rolling60s.pitch[i] > pitch_max_60s) pitch_max_60s = rolling60s.pitch[i];
        pitch_sum_60s += rolling60s.pitch[i];
      }

      float heel_mean_60s = heel_sum_60s / rolling60s.count;
      float pitch_mean_60s = pitch_sum_60s / rolling60s.count;

      // Store as scaled integers (12.34° → 1234)
      imuWindow->heel_change_60s = (int32_t)((heel_max_60s - heel_min_60s) * 100.0f);
      imuWindow->pitch_change_60s = (int32_t)((pitch_max_60s - pitch_min_60s) * 100.0f);

      float heel_dev = max(abs(heel_max_60s - heel_mean_60s), abs(heel_min_60s - heel_mean_60s));
      float pitch_dev = max(abs(pitch_max_60s - pitch_mean_60s), abs(pitch_min_60s - pitch_mean_60s));

      imuWindow->heel_deviation_60s = (int32_t)(heel_dev * 100.0f);
      imuWindow->pitch_deviation_60s = (int32_t)(pitch_dev * 100.0f);

      imu_heel_change_60s = heel_max_60s - heel_min_60s;
      imu_heel_deviation_60s = heel_dev;
      imu_pitch_change_60s = pitch_max_60s - pitch_min_60s;
      imu_pitch_deviation_60s = pitch_dev;
    }

    // --- 120-second ring buffer (same 1s tick) ---
    rolling120s.heel[rolling120s.index]    = cf_heel;
    rolling120s.pitch[rolling120s.index]   = cf_pitch;
    // Store compass heading if fresh; -1.0 sentinel means stale/unavailable
    rolling120s.heading[rolling120s.index] = IS_STALE(IDX_HEADING_NMEA) ? -1.0f : HeadingNMEA;
    rolling120s.index = (rolling120s.index + 1) % ROLLING_WINDOW_120;
    if (rolling120s.count < ROLLING_WINDOW_120) rolling120s.count++;

    if (rolling120s.count > 30) {
      // Roll deviation — peak departure from 2-min mean
      float h_min = 999.0f, h_max = -999.0f, h_sum = 0;
      float p_min = 999.0f, p_max = -999.0f, p_sum = 0;
      for (uint16_t i = 0; i < rolling120s.count; i++) {
        if (rolling120s.heel[i]  < h_min) h_min = rolling120s.heel[i];
        if (rolling120s.heel[i]  > h_max) h_max = rolling120s.heel[i];
        h_sum += rolling120s.heel[i];
        if (rolling120s.pitch[i] < p_min) p_min = rolling120s.pitch[i];
        if (rolling120s.pitch[i] > p_max) p_max = rolling120s.pitch[i];
        p_sum += rolling120s.pitch[i];
      }
      float h_mean = h_sum / rolling120s.count;
      float p_mean = p_sum / rolling120s.count;
      imu_heel_deviation_120s  = max(abs(h_max - h_mean), abs(h_min - h_mean));
      imu_pitch_deviation_120s = max(abs(p_max - p_mean), abs(p_min - p_mean));

      // Heading swing — wrap-aware using mean-vector approach (handles 0°/360° boundary)
      float sin_sum = 0, cos_sum = 0;
      int valid_hd = 0;
      for (uint16_t i = 0; i < rolling120s.count; i++) {
        float hd = rolling120s.heading[i];
        if (hd >= 0) {  // -1 sentinel = stale
          float hd_rad = hd * (float)(M_PI / 180.0);
          sin_sum += sinf(hd_rad);
          cos_sum += cosf(hd_rad);
          valid_hd++;
        }
      }
      if (valid_hd > 10) {
        float mean_rad = atan2f(sin_sum, cos_sum);
        float max_dev = 0;
        for (uint16_t i = 0; i < rolling120s.count; i++) {
          float hd = rolling120s.heading[i];
          if (hd >= 0) {
            float diff = hd * (float)(M_PI / 180.0) - mean_rad;
            while (diff >  (float)M_PI) diff -= 2.0f * (float)M_PI;
            while (diff < -(float)M_PI) diff += 2.0f * (float)M_PI;
            if (fabsf(diff) > max_dev) max_dev = fabsf(diff);
          }
        }
        imu_heading_swing_120s = max_dev * (float)(180.0 / M_PI) * 2.0f;  // peak-to-peak
      } else {
        imu_heading_swing_120s = -1.0f;  // not enough compass data
      }
    }
  }

  // Capsize/pitchpole detection
  // _triggered arms the 1s timer; _reported latches true after the message fires
  // and prevents re-firing while the angle stays above threshold. Both flags reset
  // only when the angle drops back below threshold (one message per event).
  static bool capsize_triggered = false;
  static bool capsize_reported = false;
  static bool pitchpole_triggered = false;
  static bool pitchpole_reported = false;
  static unsigned long capsize_start = 0;
  static unsigned long pitchpole_start = 0;

  if (abs(cf_heel) > CAPSIZE_THRESHOLD_DEG) {
    if (!capsize_triggered) {
      capsize_triggered = true;
      capsize_start = now;
    } else if (!capsize_reported && (now - capsize_start > 1000)) {  // fires once per event
      imu_capsize_count++;
      queueConsoleMessageF("CAPSIZE EVENT: %.1f deg", cf_heel);
      saveNVSDataFull();  // boat may lose power — commit lifetime counter now (sync)
      capsize_reported = true;
    }
  } else {
    capsize_triggered = false;
    capsize_reported = false;
  }

  if (abs(cf_pitch) > PITCHPOLE_THRESHOLD_DEG) {
    if (!pitchpole_triggered) {
      pitchpole_triggered = true;
      pitchpole_start = now;
    } else if (!pitchpole_reported && (now - pitchpole_start > 1000)) {  // fires once per event
      imu_pitchpole_count++;
      queueConsoleMessageF("PITCHPOLE EVENT: %.1f deg", cf_pitch);
      saveNVSDataFull();  // boat may lose power — commit lifetime counter now (sync)
      pitchpole_reported = true;
    }
  } else {
    pitchpole_triggered = false;
    pitchpole_reported = false;
  }

  if (abs(cf_heel) > imu_heel_max_lifetime) {
    imu_heel_max_lifetime = abs(cf_heel);
  }
  if (abs(cf_pitch) > imu_pitch_max_lifetime) {
    imu_pitch_max_lifetime = abs(cf_pitch);
  }

  // Sea state minute bucketing — tick every 60s
  // Bucket by MSI score: gentle<10, moderate<30, rough<70, extreme>=70
  // Moving vs stationary by SOGNMEA (knots): moving if >= 1.5 kts
  static unsigned long seaStateTick_ms = 0;
  if (now - seaStateTick_ms >= 60000) {
    seaStateTick_ms = now;
    bool moving = (SOGNMEA >= 1.5f);
    if (imu_msi_score < 10.0f) {
      if (moving) imu_min_moving_gentle++;  else imu_min_stat_gentle++;
    } else if (imu_msi_score < 30.0f) {
      if (moving) imu_min_moving_moderate++; else imu_min_stat_moderate++;
    } else if (imu_msi_score < 70.0f) {
      if (moving) imu_min_moving_rough++;   else imu_min_stat_rough++;
    } else {
      if (moving) imu_min_moving_extreme++; else imu_min_stat_extreme++;
    }
  }

  // Anchorage comfort — always computed so JS can show a grayed value underway; contextually meaningful only at anchor
  // Roll and pitch only — at anchor there are no underway-style slams; rocking and hobby-horsing are the discomfort drivers
  {
    float roll_penalty  = min(imu_heel_deviation_60s  / 12.0f, 1.0f) * 65.0f;  // 12 deg heel dev -> full penalty
    float pitch_penalty = min(imu_pitch_deviation_60s /  8.0f, 1.0f) * 35.0f;  //  8 deg pitch dev -> full penalty
    imu_anchorage_comfort = constrain(100.0f - roll_penalty - pitch_penalty, 0.0f, 100.0f);
  }
}


// OTA updates + cloud auth-token handling
void initializeDeviceId() {
  // STEP 1: Always read real chip ID first
  chipid = ESP.getEfuseMac();
  deviceIdUpper = (uint32_t)(chipid >> 32);
  deviceIdLower = (uint32_t)(chipid & 0xFFFFFFFF);

  // STEP 2: TESTING OVERRIDE - uncomment to fake device ID
  // Device UID spoofing conflicts with OTA updates if the hardcoded override values have changed since
  // factory partition was programmed. During OTA, the device temporarily boots to factory partition with the old UID,
  // triggering token clearance. Workarounds exist but aren't worth the complexity—just re-register after OTA if you've
  // changed the spoofed UID since initially programming the factory partition.
  //deviceIdUpper = 0xBBBBBBBB;
  //deviceIdLower = 0xAAAAAAAA;

  // STEP 3: Build string representation from the (possibly overridden) values
  snprintf(device_id_hex, sizeof(device_id_hex), "%08X%08X",
           (unsigned int)deviceIdUpper,
           (unsigned int)deviceIdLower);
}
String getDeviceId() {
  return String(device_id_hex);
}
void checkDeviceUIDChange() {
  currentUID = String(device_id_hex);

  nvs_handle_t handle;
  nvs_open("cloud", NVS_READWRITE, &handle);

  size_t len = 0;
  String lastUID = "";
  esp_err_t err = nvs_get_str(handle, "lastUID", NULL, &len);
  if (err == ESP_OK && len > 0) {
    char *buf = (char *)malloc(len);
    if (!buf) {
      Serial.println("checkDeviceUIDChange: malloc failed for lastUID buffer - skipping read");
    } else {
      nvs_get_str(handle, "lastUID", buf, &len);
      lastUID = String(buf);
      free(buf);
    }
  }

  Serial.println("=== checkDeviceUIDChange ===");
  Serial.print("Current UID: ");
  Serial.println(currentUID);
  Serial.print("Last UID: ");
  Serial.println(lastUID);

  if (lastUID != currentUID && lastUID.length() > 0) {
    Serial.println("Device UID changed - clearing token");
    nvs_erase_key(handle, "authToken");
    nvs_commit(handle);
    authToken = "";
    isRegistered = false;
    Serial.println("Auth token cleared");
  } else {
    Serial.println("UID unchanged, keeping token");
  }

  nvs_set_str(handle, "lastUID", currentUID.c_str());
  nvs_commit(handle);
  nvs_close(handle);
}
bool base64Decode(const String &input, uint8_t *output, size_t outputSize, size_t *decodedLength) {
  size_t inputLen = input.length();
  size_t expectedLen = (inputLen * 3) / 4;
  if (expectedLen > outputSize) {
    Serial.printf("Base64 decode: output buffer too small (%d needed, %d available)\n", expectedLen, outputSize);
    return false;
  }

  int ret = mbedtls_base64_decode(output, outputSize, decodedLength,
                                  (const unsigned char *)input.c_str(), inputLen);

  if (ret != 0) {
    Serial.printf("Base64 decode failed: %d\n", ret);
    return false;
  }

  return true;
}
bool initStreamingExtractor(StreamingExtractor *extractor) {
  memset(extractor, 0, sizeof(StreamingExtractor));

  extractor->inTarHeader = true;
  extractor->tarHeaderPos = 0;
  extractor->inPadding = false;
  extractor->paddingRemaining = 0;

  // Initialize hash for signature verification
  mbedtls_md_init(&extractor->hashCtx);
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  int mdRet = mbedtls_md_setup(&extractor->hashCtx, info, 0);
  if (mdRet != 0) {
    Serial.printf("OTA: mbedtls_md_setup failed: %d\n", mdRet);
    mbedtls_md_free(&extractor->hashCtx);
    return false;
  }
  mbedtls_md_starts(&extractor->hashCtx);
  extractor->hashStarted = true;

  Serial.println("✅ Streaming tar extractor initialized");
  return true;
}
bool parseTarHeader(StreamingExtractor *extractor) {
  // Validate header first (but skip the all-zeros check since it's handled in processDataChunk)
  if (memcmp(extractor->tarHeader + 257, "ustar", 5) != 0) {
    Serial.println("❌ Invalid tar header: missing ustar magic");
    Serial.print("Header start: ");
    for (int i = 0; i < 20; i++) {
      Serial.printf("%02x ", extractor->tarHeader[i]);
    }
    Serial.println();
    return false;
  }

  Serial.println("✅ Valid tar header with ustar magic");

  // Extract filename (starts at offset 0, max 100 chars)
  char rawFilename[101];
  memcpy(rawFilename, extractor->tarHeader, 100);
  rawFilename[100] = '\0';

  // Check file type (position 156 in tar header)
  char typeFlag = extractor->tarHeader[156];

  String filename = String(rawFilename);
  filename.trim();

  if (filename.startsWith("./")) {
    filename = filename.substring(2);
  }

  // Skip directories
  if (typeFlag == '5' || filename.endsWith("/")) {
    Serial.println("📁 Skipping directory entry");
    extractor->currentFileName = "";
    extractor->currentFileSize = 0;
    extractor->currentFilePos = 0;
    extractor->isCurrentFileFirmware = false;
    return true;
  }

  // Only process regular files
  if (typeFlag != '0' && typeFlag != '\0') {
    Serial.printf("⚠️  Skipping file type '%c' for: %s\n", typeFlag, filename.c_str());
    extractor->currentFileName = "";
    extractor->currentFileSize = 0;
    extractor->currentFilePos = 0;
    extractor->isCurrentFileFirmware = false;
    return true;
  }

  extractor->currentFileName = filename;

  // Extract file size (starts at offset 124, 12 chars, octal)
  char sizeStr[13];
  memcpy(sizeStr, extractor->tarHeader + 124, 12);
  sizeStr[12] = '\0';

  extractor->currentFileSize = 0;
  for (int i = 0; i < 12 && sizeStr[i] != '\0' && sizeStr[i] != ' '; i++) {
    if (sizeStr[i] >= '0' && sizeStr[i] <= '7') {
      extractor->currentFileSize = extractor->currentFileSize * 8 + (sizeStr[i] - '0');
    }
  }

  extractor->currentFilePos = 0;

  extractor->isCurrentFileFirmware = extractor->currentFileName.equals("firmware.bin");

  Serial.printf("📁 Found file: %s (%d bytes) type='%c'\n",
                extractor->currentFileName.c_str(), extractor->currentFileSize, typeFlag);

  // Route file to appropriate destination
  if (extractor->currentFileName.equals("firmware.bin")) {
    extractor->otaPartition = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    esp_err_t beginErr = esp_ota_begin(extractor->otaPartition, extractor->currentFileSize, &extractor->otaHandle);
    if (beginErr != ESP_OK) {
      // e.g. ESP_ERR_INVALID_SIZE: image exceeds the 0x280000 slot — abort now instead of
      // failing on the first esp_ota_write mid-stream.
      Serial.printf("❌ esp_ota_begin failed: %s\n", esp_err_to_name(beginErr));
      return false;
    }
    extractor->otaStarted = true;
    extractor->isCurrentFileFirmware = true;
  } else if (extractor->currentFileName.indexOf('.') > 0) {
    // Mount prod_fs for web file updates
    if (!extractor->prodFSMounted) {
      webFS.end();
      if (webFS.begin(true, "/web", 10, "prod_fs")) {
        extractor->prodFSMounted = true;
        Serial.println("OTA: Mounted prod_fs for web file updates");
        purgeProdFs();
      } else {
        Serial.println("OTA: Failed to mount prod_fs");
        return false;
      }
    }
    String filePath = "/" + extractor->currentFileName;
    extractor->currentWebFile = webFS.open(filePath, "w");
    if (!extractor->currentWebFile) {
      Serial.printf("❌ Web file open failed: %s\n", filePath.c_str());
      extractor->webWriteFailed = true;
    }
  }
  return true;
}
bool processDataChunk(StreamingExtractor *extractor, uint8_t *data, size_t dataSize) {
  size_t processed = 0;

  while (processed < dataSize) {
    if (extractor->inPadding) {
      size_t toSkip = min(extractor->paddingRemaining, dataSize - processed);
      processed += toSkip;
      extractor->paddingRemaining -= toSkip;

      if (extractor->paddingRemaining == 0) {
        extractor->inPadding = false;
        extractor->inTarHeader = true;
        extractor->tarHeaderPos = 0;
        Serial.println("✅ Padding skipped, ready for next header");
      }

    } else if (extractor->inTarHeader) {
      size_t headerRemaining = 512 - extractor->tarHeaderPos;
      size_t toCopy = min(headerRemaining, dataSize - processed);

      memcpy(extractor->tarHeader + extractor->tarHeaderPos, data + processed, toCopy);
      extractor->tarHeaderPos += toCopy;
      processed += toCopy;

      if (extractor->tarHeaderPos >= 512) {
        // Complete header received - check for end of archive
        bool allZeros = true;
        for (int i = 0; i < 512; i++) {
          if (extractor->tarHeader[i] != 0) {
            allZeros = false;
            break;
          }
        }

        if (allZeros) {
          Serial.println("✅ End of tar archive detected - extraction complete!");
          return true;  // SUCCESS! Archive is complete
        }

        if (!parseTarHeader(extractor)) {
          Serial.println("❌ Failed to parse tar header");
          return false;  // Actual parsing error
        }
        extractor->inTarHeader = false;
      }

    } else {
      // Reading file data
      size_t fileRemaining = extractor->currentFileSize - extractor->currentFilePos;
      size_t dataRemaining = dataSize - processed;
      size_t toWrite = min(fileRemaining, dataRemaining);

      if (toWrite > 0 && extractor->currentFileName.length() > 0) {
        // Only process if we have a valid filename (not a skipped entry)
        if (extractor->isCurrentFileFirmware && extractor->otaStarted) {
          esp_err_t err = esp_ota_write(extractor->otaHandle, data + processed, toWrite);
          if (err != ESP_OK) {
            Serial.printf("❌ OTA write failed: %s\n", esp_err_to_name(err));
            return false;
          }

        } else if (extractor->currentWebFile) {
          size_t written = extractor->currentWebFile.write(data + processed, toWrite);
          if (written != toWrite) {
            Serial.printf("❌ Web file write failed: %d/%d bytes\n", written, toWrite);
            extractor->webWriteFailed = true;
          }
        }
      }

      if (toWrite > 0) {
        extractor->currentFilePos += toWrite;
        processed += toWrite;
      }

      if (extractor->currentFilePos >= extractor->currentFileSize) {
        if (extractor->isCurrentFileFirmware && extractor->otaStarted) {
          Serial.println("✅ Firmware extraction completed");
        } else if (extractor->currentWebFile) {
          extractor->currentWebFile.flush();
          size_t onFlash = extractor->currentWebFile.size();
          extractor->currentWebFile.close();
          if (onFlash != extractor->currentFileSize) {
            Serial.printf("❌ Web file size mismatch: %s (%u on flash, %u expected)\n",
                          extractor->currentFileName.c_str(), (unsigned)onFlash, (unsigned)extractor->currentFileSize);
            extractor->webWriteFailed = true;
          }
          Serial.printf("✅ Web file completed: %s (%d bytes)\n",
                        extractor->currentFileName.c_str(), extractor->currentFileSize);
        } else if (extractor->currentFileName.length() > 0) {
          Serial.printf("✅ Skipped file completed: %s\n", extractor->currentFileName.c_str());
        }

        // Calculate padding (files are padded to 512-byte boundaries)
        size_t padding = (512 - (extractor->currentFileSize % 512)) % 512;

        if (padding > 0) {
          Serial.printf("📏 File complete, need to skip %d padding bytes\n", padding);
          extractor->inPadding = true;
          extractor->paddingRemaining = padding;
        } else {
          Serial.println("📏 File complete, no padding needed");
          extractor->inTarHeader = true;
          extractor->tarHeaderPos = 0;
        }
      }
    }
  }

  return true;
}
// Link-quality floor for every OTA path (download, version report, forced-update check).
// Deliberately set to "only refuse when the link is effectively dead" — an ESP32 rarely holds an
// association past about -90, so on any usable link this never fires. That is the intent: RSSI is
// a proxy for throughput and a bad one here (a crowded marina at -60 can carry less than a quiet
// anchorage at -82 — SNR decides, not signal strength). The download is outcome-protected instead:
// no-data abort, total-time cap, esp_ota_abort() + RSA verify before any commit. A 2.7 MB package
// inside the cap needs only ~12 kbps, so slow is fine and only dead is fatal. Never tune this
// hoping to fix a download problem; the timeouts below are the real limits.
static const int OTA_MIN_RSSI_DBM = -90;

// The 16 s task WDT (panic) cannot span an OTA attempt: one HTTPS GET below may legally block
// for TCP connect (60 s) + TLS handshake (120 s) + header wait (60 s) with no way to feed
// mid-call. performOTAUpdateToVersion() widens the panic window to this for the attempt and
// restores WDT_TIMEOUT_MS on every exit — a true lwip wedge still panic-reboots the (field-off)
// device within 5 min instead of never.
static const uint32_t OTA_WDT_WINDOW_MS = 300000;
static void otaSetWdtWindow(uint32_t ms) {
  esp_task_wdt_config_t cfg = { .timeout_ms = ms, .idle_core_mask = 0, .trigger_panic = true };
  esp_task_wdt_reconfigure(&cfg);
  esp_task_wdt_reset();  // start the new window fresh; harmless error on the non-subscribed web-handler task
}

// Signature fetch budget: 12 s per phase, what every other small cloud fetch uses (executeUpdateFirmwareVersion
// and friends). Connect, handshake, read, and body-stall are deliberately the SAME value: the connect timeout
// is also the socket poll period the core runs inside the TLS handshake, so a handshake cap only binds when
// it is no larger than the connect cap. Left unset, the handshake inherited the core's 120 s default, which
// with the old 60 s connect let a server that accepts TCP but never answers TLS hold the update path for
// 3 min — on the factory path that is the web-handler task, i.e. the whole dashboard. testInternetSpeed()
// has just proved the link; the patient client is the 2.7 MB download that follows, not this sub-1 KB fetch.
static const uint32_t OTA_SIG_TIMEOUT_MS = 12000;
static const int OTA_SIG_MAX_LEN = 1024;  // RSA-4096 sig is 512 B = 684 chars base64; anything bigger is not a signature

// How long prepareForOTA() waits for TempTask and httpsTask to retire themselves. The longest single cloud op
// is three 12 s phases plus the 3 s post-failure delay; TempTask's longest sleep is 5 s.
static const uint32_t OTA_TASK_EXIT_WAIT_MS = 60000;

// otaInProgress cannot legitimately stay up past this: every call on the update path is bounded (task exit
// wait 60 s, signature fetch a few 12 s phases, download 60 s connect + 120 s handshake + 60 s header + the
// 30 min OTA_MAX_DOWNLOAD_MS cap with its 120 s stall guard). Past it the path is wedged in something with
// no timeout, and checkOtaWedge() retires the flag.
static const uint32_t OTA_WEDGE_LIMIT_MS = 2400000;  // 40 min

// Retire both Core 0 tasks for the attempt. Returns false — caller aborts the update — if httpsTask is still
// alive at the end of the wait.
bool prepareForOTA() {
  otaInProgress = true;
  otaInProgressSinceMs = millis();
  // Close EventSource FIRST (before any heap measurements)
  Serial.println("Closing EventSource connections...");
  events.close();
  delay(100);

  Serial.println("🧹 Preparing system for OTA - freeing memory...");
  Serial.printf("Heap BEFORE cleanup: %u bytes\n", ESP.getFreeHeap());

  // Both tasks leave on their own once they see otaInProgress — TempTask at its loop top or conversion wait,
  // httpsTask between cloud ops. Waiting for that instead of vTaskDelete() is the point: a hard delete of
  // httpsTask mid-op leaves lwIP (DNS semaphore, socket) and mbedTLS pointing into a freed stack.
  if (tempTaskSuspended && tempTaskHandle != NULL) vTaskResume(tempTaskHandle);  // a suspended task never reaches the flag check
  uint32_t exitWaitStart = millis();
  while ((tempTaskHandle != NULL || httpsTaskHandle != NULL) && millis() - exitWaitStart < OTA_TASK_EXIT_WAIT_MS) {
    delay(100);
    esp_task_wdt_reset();
  }
  if (httpsTaskHandle != NULL) {
    // Wedged inside a network call. Abort rather than hard-delete: performOTAUpdateToVersion() restores the
    // 16 s WDT window on return, and a cloud task that is truly stuck then panic-reboots the device — the
    // designed recovery for a hung op — with the network stack intact until then.
    Serial.printf("OTA ABORT: HTTPS task still busy after %lu s - not deleting it mid-op\n", (unsigned long)(OTA_TASK_EXIT_WAIT_MS / 1000));
    return false;
  }
  if (tempTaskHandle != NULL) {
    // Holds no network state: a hard delete risks one 1-Wire transaction, which the next enumerate repairs.
    Serial.println("OTA: TempTask did not exit on its own - deleting it");
    vTaskDelete(tempTaskHandle);
    tempTaskHandle = NULL;
  }
  core0Busy = true;  // a cloud op finishing during the wait cleared it; the OTA holds the field off from here

  delay(1000);
  esp_task_wdt_reset();

  Serial.printf("Heap AFTER cleanup: %u bytes\n", ESP.getFreeHeap());
  Serial.printf("Internal free: %u, largest: %u\n",
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  Serial.println("Ready for OTA download!");
  return true;
}
bool validateHeapIntegrity() {
  // Snapshot: use heap_caps_* for reliable internal vs PSRAM separation
  size_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  size_t largestInternal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

  size_t freePsram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  size_t totalPsram = ESP.getPsramSize();  // Arduino-reported PSRAM size

  // Optional: total internal size (may be 0 on some builds; keep for debug only)
  size_t totalInternal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);

  // Sanity checks (lightweight)
  if (totalInternal > 0 && freeInternal > totalInternal) {
    Serial.printf("ERROR: Heap corruption? freeInternal=%lu > totalInternal=%lu\n",
                  (unsigned long)freeInternal, (unsigned long)totalInternal);
    return false;
  }
  if (totalPsram > 0 && freePsram > totalPsram) {
    Serial.printf("ERROR: PSRAM corruption? freePsram=%lu > totalPsram=%lu\n",
                  (unsigned long)freePsram, (unsigned long)totalPsram);
    return false;
  }
  if (freeInternal > 0 && largestInternal > freeInternal) {
    Serial.printf("ERROR: Heap corruption? largestInternal=%lu > freeInternal=%lu\n",
                  (unsigned long)largestInternal, (unsigned long)freeInternal);
    return false;
  }

  // Print (single-line where possible to reduce truncation risk)
  Serial.printf("HEAP: internal free=%luB, largest=%luB, total=%luB\n",
                (unsigned long)freeInternal,
                (unsigned long)largestInternal,
                (unsigned long)totalInternal);

  Serial.printf("PSRAM: free=%luB, total=%luB\n",
                (unsigned long)freePsram,
                (unsigned long)totalPsram);

  // Also print Arduino wrappers for correlation (not used for decisions)
  Serial.printf("ARDUINO HEAP: free=%luB, size=%luB, maxAlloc=%luB\n",
                (unsigned long)ESP.getFreeHeap(),
                (unsigned long)ESP.getHeapSize(),
                (unsigned long)ESP.getMaxAllocHeap());

  return true;
}
void performStreamingOTAUpdate(const UpdateInfo &updateInfo, const String &signatureBase64, WiFiClientSecure &client) {
  //Called by: performOTAUpdate() when on factory partition
  //Purpose: Actually downloads and installs the firmware
  //Action:
  //1) Downloads firmware from updateInfo.firmwareUrl
  //2) Streams and extracts TAR file
  //3) Verifies signature
  // 4) Installs to OTA partition

  // DECLARE ALL VARIABLES AT TOP (before any goto statements)
  bool success = true;
  // Post-download rejection (signature/finalize). The reboot-into-current-
  // firmware these paths take is the right recovery — but without this flag
  // the cleanup branch reported it as a successful update.
  bool verifyFailed = false;
  StreamingExtractor extractor = {};
  HTTPClient http;
  unsigned long downloadStartTime = 0;
  unsigned long downloadDuration = 0;
  int contentLength = 0;
  WiFiClient *stream = nullptr;
  const size_t CHUNK_SIZE = 1024;
  // no-data idle limit — a half-open socket never trips !connected(). 120 s not 60 s: a deep fade or
  // an AP roam can stall TCP well past a minute and still recover, and we would rather wait than
  // throw away a part-done download. This is also the freeze window from the wedged-socket incident,
  // so it stays bounded — the point is to be patient with a bad link, not to hang forever.
  const unsigned long OTA_STALL_TIMEOUT_MS = 120000;
  // absolute cap on the whole download. 30 min carries the 2.7 MB package at ~12 kbps, i.e. any link
  // still passing traffic at all. Cost of the generosity: TempTask is deleted for this whole window,
  // so alternator temperature is unmonitored until ensureCore0Tasks() rebuilds it after otaRestoreNormalOperation().
  const unsigned long OTA_MAX_DOWNLOAD_MS = 1800000;
  uint8_t inputBuffer[CHUNK_SIZE];
  int totalDownloaded = 0;
  unsigned long lastProgress = 0;
  unsigned long lastDataTime = 0;
  uint8_t hash[32];
  uint8_t signature[520];
  size_t sigLength = 0;
  mbedtls_pk_context pk;
  int ret = 0;
  int rssi = 0;
  size_t freeHeap = 0;
  bool beginSuccess = false;
  int httpCode = 0;

  Serial.println("=== STARTING STREAMING TAR UPDATE ===");
  // Rate limit ALL HTTPS operations system-wide
  esp_task_wdt_reset();
  unsigned long timeSinceLastHttps = millis() - lastHttpsOperationTime;
  if (timeSinceLastHttps < HTTPS_MIN_INTERVAL) {
    unsigned long waitTime = HTTPS_MIN_INTERVAL - timeSinceLastHttps;
    Serial.printf("RATE LIMIT: Waiting %lu ms before HTTPS\n", waitTime);
    delay(waitTime);
  }
  esp_task_wdt_reset();
  Serial.println("=== STREAMING OTA DEBUG ===");
  int wifiStatus = WiFi.status();
  Serial.print("1. WiFi status: ");
  Serial.println(wifiStatus);
  Serial.print("2. Current mode: ");
  Serial.println(currentMode);
  size_t freeHeap2 = ESP.getFreeHeap();
  Serial.print("3. Free heap2: ");
  Serial.print(freeHeap2);
  Serial.println(" bytes");
  Serial.print("4. URL length: ");
  Serial.println(updateInfo.firmwareUrl.length());
  Serial.print("5. Signature length: ");
  Serial.println(signatureBase64.length());

  // CRITICAL: Verify WiFi is connected before attempting SSL/HTTP operations
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("ERROR: WiFi not connected - cannot perform OTA update");
    queueConsoleMessage("OTA FAILED: WiFi disconnected before update started");
    goto cleanup;
  }

  if (currentMode != MODE_CLIENT) {
    Serial.println("ERROR: Not in client mode - cannot perform OTA update");
    queueConsoleMessage("OTA FAILED: Must be in client mode for updates");
    goto cleanup;
  }

  // No RSSI gate here on purpose. By this point prepareForOTA() has already deleted TempTask and
  // closed EventSource, the signature has already downloaded over this same link (proof it carries
  // HTTPS), and on the ota_0 path we have already rebooted into factory. Refusing here pays every
  // cost and saves nothing, and it would overrule a successful transfer with a guess. The
  // OTA_MIN_RSSI_DBM pre-flight in performOTAUpdateToVersion runs before any teardown; that is the
  // only place the check earns its keep.
  rssi = WiFi.RSSI();
  Serial.printf("WiFi status: %d, Signal: %d dBm\n", WiFi.status(), rssi);

  downloadStartTime = millis();

  if (!initStreamingExtractor(&extractor)) {
    goto cleanup;
  }
  otaHeapMark("BEFORE WiFiClientSecure");

  otaHeapMark("AFTER WiFiClientSecure");

  // setInsecure() not setCACert(): downloads go through the ota.xengineering.net
  // proxy, whose cert chain we don't pin (and it may change when re-homed). Transport
  // auth is irrelevant here — the downloaded package is RSA-signature-verified below
  // (mbedtls_pk_verify / OTA_PUBLIC_KEY), which is the real integrity guarantee.
  Serial.println("7. Setting TLS (insecure transport; payload is signature-verified)...");
  client.setInsecure();
  otaHeapMark("AFTER setInsecure");

  // CRITICAL: Set matching timeouts BEFORE connection attempt
  // Note client.setTimeout here is largely advisory: HTTPClient::connect() overwrites it with its own
  // _tcpTimeout right after connecting, so the http.setTimeout below is what actually governs reads.
  // Kept at the same value so the two agree either way.
  client.setTimeout(60000);
  // SECONDS (core multiplies by 1000). 120 s is the core's own default — the previous 30 was
  // TIGHTENING it, which is backwards for a weak link. A handshake is several round trips of large
  // records and is the step most likely to time out on a link that would still finish the download.
  client.setHandshakeTimeout(120);

  Serial.println("8. Pre-flight memory check...");
  freeHeap = ESP.getFreeHeap();
  Serial.print("Free heap: ");
  Serial.print(freeHeap);
  Serial.println(" bytes");

  Serial.println("Less than 50kb is not great, worked at 46kb in the past!");

  Serial.println("9. Creating HTTPClient...");
  otaHeapMark("BEFORE HTTPClient");

  otaHeapMark("AFTER HTTPClient");

  // Add watchdog reset before expensive SSL operation
  esp_task_wdt_reset();

  if (!validateHeapIntegrity()) {
    Serial.println("ABORT: Memory subsystem corrupted");
    goto cleanup;
  }

  Serial.println("10. Attempting http.begin() with SSL...");
  // No M17-style heap pre-check here: tried in 0.0.38, broke OTA. prepareForOTA() above
  // already frees memory for TLS; let http.begin() / mbedTLS surface failures themselves.
  beginSuccess = http.begin(client, updateInfo.firmwareUrl);
  otaHeapMark("AFTER http.begin");

  if (!beginSuccess) {
    Serial.println("ABORT: http.begin() failed - SSL handshake error");
    queueConsoleMessage("OTA FAILED: Cannot establish secure connection");
    goto cleanup;
  }

  Serial.println("11. SSL handshake completed successfully");

  // Now safe to add headers
  http.addHeader("Device-ID", getDeviceId());
  http.addHeader("Current-Version", FIRMWARE_VERSION);
  // setTimeout takes uint16_t — anything over 65535 silently wraps, so 60000 is the practical max
  // and asking for 120000 would truncate to 54464 and be SHORTER than what we asked for.
  http.setTimeout(60000);
  // The real bottleneck on a bad link: HTTPClient uses its own _connectTimeout (5 s default, NOT
  // the client.setTimeout above) for the TCP connect + TLS handshake, so on a slow or lossy link the
  // transfer died before a byte moved. int32_t here, so it takes a large value honestly.
  http.setConnectTimeout(60000);

  httpCode = http.GET();
  otaHeapMark("AFTER http.GET");

  if (httpCode != 200) {
    Serial.printf("Package download failed: %d (%s)\n",
                  httpCode,
                  http.errorToString(httpCode).c_str());
    Serial.printf("WiFi.status=%d RSSI=%d\n", WiFi.status(), WiFi.RSSI());
    Serial.printf("Internal free=%u largest=%u\n",
                  heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    http.end();
    goto cleanup;
  }

  contentLength = http.getSize();
  if (contentLength <= 0) {
    // Missing/invalid Content-Length → while-loop never enters, hash computes over zero bytes,
    // signature verify fails with a misleading "verification FAILED" message. Fail fast instead.
    Serial.printf("ABORT: OTA server returned invalid Content-Length: %d\n", contentLength);
    queueConsoleMessage("OTA FAILED: Server returned no Content-Length");
    http.end();
    goto cleanup;
  }
  stream = http.getStreamPtr();

  // Streaming tar extraction
  totalDownloaded = 0;
  lastProgress = 0;
  lastDataTime = millis();

  while (totalDownloaded < contentLength && success) {
    if (!client.connected()) {
      Serial.println("Connection lost during download");
      success = false;
      break;
    }
    esp_task_wdt_reset();
    int available = stream->available();
    if (available > 0) {
      int toRead = min(available, (int)min(CHUNK_SIZE, (size_t)(contentLength - totalDownloaded)));
      int actualRead = stream->readBytes(inputBuffer, toRead);

      if (actualRead > 0) {
        lastDataTime = millis();
        totalDownloaded += actualRead;

        mbedtls_md_update(&extractor.hashCtx, inputBuffer, actualRead);

        // Process tar data directly (no decompression needed)
        if (!processDataChunk(&extractor, inputBuffer, actualRead)) {
          success = false;
          break;
        }

        if (millis() - lastProgress > 2000) {
          Serial.printf("Progress: %d%% (%d/%d bytes)\n",
                        (totalDownloaded * 100) / contentLength, totalDownloaded, contentLength);
          lastProgress = millis();
          esp_task_wdt_reset();
        }
      }
    } else {
      // Half-open cellular socket: connected() stays true and available() stays 0 forever,
      // so without this guard the loop spins feeding the WDT — device frozen with the
      // alternator disabled until a manual power cycle.
      if (millis() - lastDataTime > OTA_STALL_TIMEOUT_MS) {
        Serial.printf("ABORT: OTA download stalled - no data for %lu s\n", OTA_STALL_TIMEOUT_MS / 1000);
        queueConsoleMessage("OTA FAILED: download stalled, connection dead - aborting");
        success = false;
        break;
      }
      delay(10);
      esp_task_wdt_reset();
    }
    if (millis() - downloadStartTime > OTA_MAX_DOWNLOAD_MS) {
      Serial.printf("ABORT: OTA download exceeded %lu min cap\n", OTA_MAX_DOWNLOAD_MS / 60000);
      queueConsoleMessage("OTA FAILED: download too slow - aborting");
      success = false;
      break;
    }
  }

  http.end();

  downloadDuration = (millis() - downloadStartTime) / 1000;
  Serial.printf("SPEED: Download completed in %lu seconds\n", downloadDuration);

  if (!success) {
    Serial.println("Streaming extraction failed");
    discardOta0Install(&extractor);
    goto cleanup;
  }

  Serial.println("Streaming download and extraction completed");

  // The signature hashes the downloaded stream, not what landed on flash — a web-file
  // write failure must fail the update here or a corrupt bundle ships with a valid sig.
  if (extractor.webWriteFailed) {
    Serial.println("OTA FAILED: web file write/open failure during extraction");
    queueConsoleMessage("OTA FAILED: web file write error - update not applied");
    discardOta0Install(&extractor);
    verifyFailed = true;
    goto cleanup;
  }

  // Verify signature
  mbedtls_md_finish(&extractor.hashCtx, hash);

  if (!base64Decode(signatureBase64, signature, sizeof(signature), &sigLength)) {
    Serial.println("SECURITY: Failed to decode signature");
    discardOta0Install(&extractor);
    verifyFailed = true;
    goto cleanup;
  }

  mbedtls_pk_init(&pk);
  ret = mbedtls_pk_parse_public_key(&pk, (const unsigned char *)OTA_PUBLIC_KEY, strlen(OTA_PUBLIC_KEY) + 1);
  if (ret != 0) {
    Serial.printf("SECURITY: Failed to parse public key: -0x%04x\n", -ret);
    discardOta0Install(&extractor);
    mbedtls_pk_free(&pk);
    verifyFailed = true;
    goto cleanup;
  }

  ret = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, 32, signature, sigLength);
  mbedtls_pk_free(&pk);

  if (ret != 0) {
    Serial.printf("SECURITY: Signature verification FAILED (error -0x%04x)\n", -ret);
    discardOta0Install(&extractor);
    verifyFailed = true;
    goto cleanup;
  }

  Serial.println("SECURITY: Signature verification PASSED");

  if (extractor.otaStarted) {
    esp_err_t err = esp_ota_end(extractor.otaHandle);
    if (err != ESP_OK) {
      Serial.printf("OTA end failed: %s\n", esp_err_to_name(err));
      discardOta0Image(extractor.otaPartition);
      verifyFailed = true;
      goto cleanup;
    }

    err = esp_ota_set_boot_partition(extractor.otaPartition);
    if (err != ESP_OK) {
      Serial.printf("Set boot partition failed: %s\n", esp_err_to_name(err));
      discardOta0Image(extractor.otaPartition);
      verifyFailed = true;
      goto cleanup;
    }
    if (!recordOta0AsVerified()) {
      Serial.println("OTA: verified-install record write failed - ota_0 boots now, but a later factory boot will not re-promote it");
    }
  }

  Serial.println("=== STREAMING OTA UPDATE SUCCESSFUL ===");
  Serial.printf("Updated from %s to %s\n", FIRMWARE_VERSION, updateInfo.version.c_str());

  // Clear forced update flags in Supabase after successful update
  if (hasForcedUpdate) {
    executeClearForcedUpdate();
  }
  Serial.println("Restarting in 3 seconds...");

cleanup:
  if (extractor.hashStarted) {
    mbedtls_md_free(&extractor.hashCtx);
  }
  if (extractor.currentWebFile) {
    extractor.currentWebFile.close();
  }
  if (extractor.prodFSMounted) {
    webFS.end();
  }

  if (success && extractor.otaStarted) {
    lastHttpsOperationTime = millis();
    if (verifyFailed) {
      // Same reboot as the success path (clean recovery into current firmware,
      // rejected image already aborted/never set bootable) — but say so honestly.
      Serial.println("=== OTA UPDATE FAILED AFTER DOWNLOAD (see error above) — rebooting on current firmware ===");
    }
    delay(3000);
    ESP.restart();
  } else {
    // No reboot on this path. If the extractor swapped webFS to prod_fs mid-download, the
    // running factory app just lost its filesystem — remount factory_fs or every PSRAM-cache
    // miss and flash-fallback asset 404s until the next power cycle.
    if (extractor.prodFSMounted) switchToFactoryWebFiles();
    otaRestoreNormalOperation(false);
  }
}
void performOTAUpdate(const UpdateInfo &updateInfo) {
  //Purpose: Handles the partition switching and signature download
  //Action:
  //1) Downloads signature from server
  //2) Checks if running on ota_0 or factory partition
  //3) If on ota_0: switches to factory and reboots
  //4) If on factory: calls performStreamingOTAUpdate()
  Serial.println("🔒 === STARTING SECURE PACKAGE UPDATE ===");
  // Rate limit ALL HTTPS operations system-wide
  unsigned long timeSinceLastHttps = millis() - lastHttpsOperationTime;
  if (timeSinceLastHttps < HTTPS_MIN_INTERVAL) {
    unsigned long waitTime = HTTPS_MIN_INTERVAL - timeSinceLastHttps;
    Serial.printf("RATE LIMIT: Waiting %lu ms before HTTPS\n", waitTime);
    delay(waitTime);
  }
  if (!prepareForOTA()) {
    otaRestoreNormalOperation(false);
    return;
  }
  // Download signature first
  WiFiClientSecure client;
  client.setInsecure();  // ota.xengineering.net proxy (unpinned chain); payload integrity comes from RSA verify, not TLS pinning
  HTTPClient http;
  Serial.printf("📥 Downloading signature from: %s\n", updateInfo.signatureUrl.c_str());
  // No 40K heap gate — see notes at firmware-download http.begin above.
  // Budget rationale at OTA_SIG_TIMEOUT_MS. The handshake cap takes SECONDS.
  client.setHandshakeTimeout(OTA_SIG_TIMEOUT_MS / 1000);
  http.setConnectTimeout(OTA_SIG_TIMEOUT_MS);
  http.begin(client, updateInfo.signatureUrl);
  http.setTimeout(OTA_SIG_TIMEOUT_MS);  // header wait + reads; after begin() like the other cloud clients
  http.addHeader("Device-ID", getDeviceId());
  int httpCode = http.GET();

  if (httpCode != 200) {
    Serial.printf("❌ Signature download failed: %d (%s)\n",
                  httpCode,
                  http.errorToString(httpCode).c_str());
    http.end();
    otaRestoreNormalOperation(false);
    return;
  }
  // The proxy always sends Content-Length (strlen of the fetched object), so a missing one is a broken reply.
  int sigLen = http.getSize();
  if (sigLen <= 0 || sigLen > OTA_SIG_MAX_LEN) {
    Serial.printf("Signature reply size invalid: %d bytes\n", sigLen);
    http.end();
    otaRestoreNormalOperation(false);
    return;
  }
  // Bounded read instead of http.getString(): HTTPClient::writeToStreamDataBlock() loops on connected() with
  // no data deadline, and a half-open socket keeps connected() true until TCP keepalive gives up (~2 h). The
  // tar loop below has a stall guard for exactly this; the signature body needs the same one.
  String signatureBase64;
  signatureBase64.reserve(sigLen);
  {
    WiFiClient *sigStream = http.getStreamPtr();
    char sigChunk[128];
    uint32_t lastSigData = millis();
    bool sigStalled = false;
    while ((int)signatureBase64.length() < sigLen) {
      if (!client.connected()) break;
      int avail = sigStream->available();
      if (avail > 0) {
        int toRead = min(avail, min((int)sizeof(sigChunk), sigLen - (int)signatureBase64.length()));
        int n = sigStream->readBytes(sigChunk, toRead);
        if (n > 0) {
          signatureBase64.concat(sigChunk, n);
          lastSigData = millis();
        }
      } else if (millis() - lastSigData > OTA_SIG_TIMEOUT_MS) {
        sigStalled = true;
        break;
      } else {
        delay(10);
      }
      esp_task_wdt_reset();
    }
    if ((int)signatureBase64.length() != sigLen) {
      Serial.printf("Signature download %s: got %d of %d bytes\n", sigStalled ? "stalled" : "cut off", signatureBase64.length(), sigLen);
      http.end();
      otaRestoreNormalOperation(false);
      return;
    }
  }
  http.end();
  signatureBase64.trim();
  Serial.printf("✅ Signature downloaded (%d chars)\n", signatureBase64.length());

  Serial.println("UPDATE: Starting firmware download, web interface will be unresponsive");
  //events.send("UPDATE: Downloading firmware - interface will freeze 60-90 sec", "console", millis());
  delay(3000);

  // Check if we need to restart to factory first
  const esp_partition_t *ota0_partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
  const esp_partition_t *factory_partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
  const esp_partition_t *running_partition = esp_ota_get_running_partition();

  if (running_partition == ota0_partition) {
    Serial.println("OTA: Currently on ota_0, switching to factory for update...");
    // events.send("OTA: Switching to factory partition for update", "console", millis());

    esp_ota_set_boot_partition(factory_partition);
    switchToFactoryWebFiles();

    Serial.println("OTA: Restarting to factory, then will download update to ota_0");
    events.send("OTA: Restarting to factory - device will reboot now", "console", millis());
    delay(3000);
    ESP.restart();
    // Execution stops here
  }

  // If we reach here, we're running from factory - proceed with update
  lastHttpsOperationTime = millis();  // UPDATE before next HTTPS call
  performStreamingOTAUpdate(updateInfo, signatureBase64, client);
}
// Returns true if update pending, fills versionOut with target version
bool checkForPendingUpdateNonBlocking(char *versionOut) {
  if (currentMode == MODE_CONFIG || currentMode == MODE_AP) {
    return false;
  }

  nvs_handle_t nvs_handle;
  if (nvs_open("update_req", NVS_READONLY, &nvs_handle) != ESP_OK) {
    return false;
  }

  uint8_t updateRequested = 0;
  size_t required_size = 64;

  bool updateFound = false;
  if (nvs_get_u8(nvs_handle, "update_flag", &updateRequested) == ESP_OK && updateRequested == 1) {
    if (nvs_get_str(nvs_handle, "target_ver", versionOut, &required_size) == ESP_OK) {
      updateFound = true;
    }
  }

  nvs_close(nvs_handle);
  return updateFound;
}
// Clear the NVS flags (call this ONLY when ready to proceed)
void clearPendingUpdateNVS() {
  nvs_handle_t clear_handle;
  if (nvs_open("update_req", NVS_READWRITE, &clear_handle) == ESP_OK) {
    nvs_erase_key(clear_handle, "update_flag");
    nvs_erase_key(clear_handle, "target_ver");
    nvs_erase_key(clear_handle, "wake_flag");
    nvs_commit(clear_handle);
    nvs_close(clear_handle);
    Serial.println("UPDATE: NVS flags cleared");
  }
}
// ---- prod_fs clean slate + ota_0 verified-install record ------------------------------------------
// Delete every file on prod_fs before the first web asset is written. A renamed asset can no longer
// leave its predecessor behind, and the write path stops depending on how the library reclaims a
// truncated file's blocks (bench 2026-09-03, esp_littlefs 1.20.4: open(...,"w") frees them at the open
// call, so the old in-place rewrite of script.js.gz never needed old+new space — a library detail,
// not a contract).
void purgeProdFs() {
  int removed = 0, failed = 0, count;
  do {
    String names[16];
    count = 0;
    File root = webFS.open("/");
    if (root && root.isDirectory()) {
      for (File f = root.openNextFile(); f && count < 16; f = root.openNextFile()) {
        names[count++] = String("/") + f.name();
        f.close();
      }
    }
    if (root) root.close();
    for (int i = 0; i < count; i++) {
      if (webFS.remove(names[i])) removed++;
      else failed++;
    }
  } while (count == 16 && failed == 0);
  Serial.printf("OTA: prod_fs purge - %d removed, %d failed, %u bytes used\n", removed, failed, (unsigned)webFS.usedBytes());
}

// ota_0 is promoted at a factory boot only when it holds exactly the image a completed, RSA-verified
// install recorded here. Identity = the SHA-256 esp_image_verify computes over the whole image (the
// digest appended at build time), never an app-descriptor field: a descriptor can be copied into a
// forged image, the image digest cannot. Lives in the "update_req" namespace beside the staged-update
// flags; a deep factory reset wipes it and the next ota_0 boot re-records itself
// (ensurePreferredBootPartition).
static const char *OTA0_RECORD_KEY = "ota0_sha";

// Appended SHA-256 of the image in `part`, read by walking the segment headers (a few small flash
// reads, no full-image hash, no mmap of the running app). Trusted only where the content has already
// been validated: the bootloader validated the running image, esp_ota_set_boot_partition validated a
// fresh install. The factory gate uses ota0ImageDigest (esp_image_verify, full content check) instead.
bool appImageAppendedDigest(const esp_partition_t *part, uint8_t *digest32) {
  if (!part) return false;
  esp_image_header_t hdr;
  if (esp_partition_read(part, 0, &hdr, sizeof(hdr)) != ESP_OK) return false;
  if (hdr.magic != ESP_IMAGE_HEADER_MAGIC || !hdr.hash_appended || hdr.segment_count == 0 || hdr.segment_count > ESP_IMAGE_MAX_SEGMENTS) return false;
  uint32_t off = sizeof(esp_image_header_t);
  for (int i = 0; i < hdr.segment_count; i++) {
    esp_image_segment_header_t seg;
    if (esp_partition_read(part, off, &seg, sizeof(seg)) != ESP_OK) return false;
    off += sizeof(seg) + seg.data_len;
    if (off > part->size) return false;
  }
  off = (off + 16) & ~15u;  // checksum byte, padded so the image ends on a 16-byte boundary
  if (off + 32 > part->size) return false;
  return esp_partition_read(part, off, digest32, 32) == ESP_OK;
}
bool ota0ImageDigest(uint8_t *digest32) {
  const esp_partition_t *ota0 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
  if (!ota0) return false;
  esp_partition_pos_t pos = { .offset = ota0->address, .size = ota0->size };
  esp_image_metadata_t meta;
  if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta) != ESP_OK) return false;
  if (!meta.image.hash_appended) return false;
  memcpy(digest32, meta.image_digest, 32);
  return true;
}
bool readOta0Record(uint8_t *digest32) {
  nvs_handle_t h;
  if (nvs_open("update_req", NVS_READONLY, &h) != ESP_OK) return false;
  size_t len = 32;
  esp_err_t e = nvs_get_blob(h, OTA0_RECORD_KEY, digest32, &len);
  nvs_close(h);
  return e == ESP_OK && len == 32;
}
bool writeOta0Record(const uint8_t *digest32) {
  nvs_handle_t h;
  if (nvs_open("update_req", NVS_READWRITE, &h) != ESP_OK) return false;
  esp_err_t e = nvs_set_blob(h, OTA0_RECORD_KEY, digest32, 32);
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);
  return e == ESP_OK;
}
void clearOta0Record() {
  nvs_handle_t h;
  if (nvs_open("update_req", NVS_READWRITE, &h) != ESP_OK) return;
  nvs_erase_key(h, OTA0_RECORD_KEY);
  nvs_commit(h);
  nvs_close(h);
}
// Record the image now in ota_0 as the verified install. Callers have just had the content validated
// (esp_ota_set_boot_partition after an install, the bootloader for a running ota_0), so the appended
// digest is read directly. False if the slot holds no well-formed image or NVS refused the write.
bool recordOta0AsVerified() {
  uint8_t d[32];
  const esp_partition_t *ota0 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
  if (!appImageAppendedDigest(ota0, d)) return false;
  if (!writeOta0Record(d)) return false;
  Serial.printf("OTA: ota_0 verified-install record %02x%02x%02x%02x%02x%02x%02x%02x\n", d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
  return true;
}
bool ota0MatchesRecord() {
  uint8_t rec[32], d[32];
  if (!readOta0Record(rec)) return false;
  if (!ota0ImageDigest(d)) return false;
  return memcmp(rec, d, 32) == 0;
}
// A failed install must leave nothing promotable behind: wipe the image header so the slot can neither
// boot nor pass a descriptor check, and drop the record (the previous ota_0 image is gone anyway —
// esp_ota_begin erased it). Bench 2026-09-03: without this a fully-streamed image that then failed
// signature verification was promoted by the next factory boot.
void discardOta0Image(const esp_partition_t *ota0) {
  if (ota0) esp_partition_erase_range(ota0, 0, 0x1000);
  clearOta0Record();
}
void discardOta0Install(StreamingExtractor *ex) {
  if (ex->otaStarted) {
    esp_ota_abort(ex->otaHandle);
    discardOta0Image(ex->otaPartition);
  } else if (ex->prodFSMounted) {
    clearOta0Record();  // web bundle already disturbed against an untouched slot: not a matched pair any more
  }
}
void performOTAUpdateToVersion(const char *targetVersion) {
  //This performs an OTA update to specific version
  // Called by: The /get handler when UpdateToVersion parameter is received
  //Purpose: Initiates a targeted version update
  //Action:
  //1) Builds the artifact URLs from OTA_BASE_URL (ota.xengineering.net proxy -> Supabase) for the target version (no check.php)
  //2) Calls performOTAUpdate() which downloads, verifies signature, and installs
  // Rate limit ALL HTTPS operations system-wide
  unsigned long timeSinceLastHttps = millis() - lastHttpsOperationTime;
  if (timeSinceLastHttps < HTTPS_MIN_INTERVAL) {
    unsigned long waitTime = HTTPS_MIN_INTERVAL - timeSinceLastHttps;
    Serial.printf("RATE LIMIT: Waiting %lu ms before HTTPS\n", waitTime);
    delay(waitTime);
  }

  //Delting this- an OTA update takes priority, and we will bulldoze thru
  // Priority still holds, but bulldozing no longer means vTaskDelete() on a mid-op cloud task:
  // prepareForOTA() raises otaInProgress and waits for httpsTask to leave between ops.
  core0Busy = true;

  Serial.println("HEAP BEFORE version check:");
  Serial.printf("  Internal: %u free, %u largest\n",
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

  Serial.println();
  Serial.println();
  Serial.printf("🎯 === PERFORMING TARGETED OTA UPDATE TO %s ===\n", targetVersion);
  Serial.println("OTA: Starting targeted update to version " + String(targetVersion));
  events.send("OTA: Starting update to version " + String(targetVersion), "console", millis());

  if (currentMode != MODE_CLIENT) {
    Serial.println("OTA: Cannot update - not in client mode");
    events.send("OTA: Cannot update - must be in client mode", "console", millis());
    core0Busy = false;
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("OTA: Cannot update - WiFi not connected");
    events.send("OTA: Cannot update - WiFi not connected", "console", millis());
    core0Busy = false;
    return;
  }

  // Every network call from here on may block past the 16 s WDT window (testInternetSpeed's
  // 10 s connect + 15 s read, then the 60/120 s OTA fetches), so widen for the whole attempt.
  // Every return below this line must restore WDT_TIMEOUT_MS. Runs WDT-subscribed on the loop
  // task (staged-update path) or unsubscribed on the web-handler task (factory path) — both safe.
  otaSetWdtWindow(OTA_WDT_WINDOW_MS);

  if (!testInternetSpeed()) {
    Serial.println("OTA: Cannot update - internet too slow or unavailable");
    events.send("OTA: Cannot update - internet connection insufficient", "console", millis());
    otaSetWdtWindow(WDT_TIMEOUT_MS);
    core0Busy = false;
    return;
  }

  int rssi = WiFi.RSSI();
  if (rssi < OTA_MIN_RSSI_DBM) {
    Serial.printf("OTA: WiFi too weak (%d dBm) - canceling update\n", rssi);
    events.send("OTA: WiFi signal too weak for safe download", "console", millis());
    otaSetWdtWindow(WDT_TIMEOUT_MS);
    core0Busy = false;
    return;
  }


  esp_task_wdt_reset();

  // No middleman: the target version is already known (forced-update from the
  // cloud, or a user pick in the dashboard), so we build the artifact URLs directly
  // from our stable OTA_BASE_URL (ota.xengineering.net proxy -> Supabase) instead of
  // asking check.php to resolve them. A version that doesn't exist surfaces as an
  // HTTP 404 inside performOTAUpdate(), which aborts safely (alternator stays off).
  String otaBase = String(OTA_BASE_URL);
  UpdateInfo updateInfo;
  updateInfo.hasUpdate = true;
  updateInfo.version = String(targetVersion);
  updateInfo.firmwareUrl = otaBase + "/firmware_" + String(targetVersion) + ".tar";
  updateInfo.signatureUrl = otaBase + "/firmware_" + String(targetVersion) + ".sig";
  updateInfo.changelog = "";
  updateInfo.firmwareSize = 0;  // unknown ahead of time; tar extractor streams to EOF

  Serial.printf("=== STARTING TARGETED UPDATE TO %s ===\n", targetVersion);
  Serial.println("🌐 Firmware URL: " + updateInfo.firmwareUrl);
  events.send("OTA: Beginning download of version " + String(targetVersion), "console", millis());
  performOTAUpdate(updateInfo);

  lastHttpsOperationTime = millis();
  Serial.println("HEAP AFTER OTA attempt:");
  Serial.printf("  Internal: %u free, %u largest\n",
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  otaSetWdtWindow(WDT_TIMEOUT_MS);  // failed attempt returns here; success rebooted inside
  core0Busy = false;
}
// FORCED OTA STUFF
void executeUpdateFirmwareVersion() {
  // Called by HTTPS task on Core 0

  if (!isRegistered || authToken.length() == 0) {
    Serial.println("FW_UPDATE: Not registered");
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("FW_UPDATE: WiFi not connected");
    return;
  }

  if (currentMode != MODE_CLIENT) {
    Serial.println("FW_UPDATE: Not in client mode");
    return;
  }

  int rssi = WiFi.RSSI();
  if (rssi < OTA_MIN_RSSI_DBM) {
    Serial.printf("FW_UPDATE: WiFi too weak (%d dBm)\n", rssi);
    return;
  }

  Serial.printf("FW_UPDATE: Reporting version %d\n", firmwareVersionInt);

  WiFiClientSecure client;
  client.setInsecure();
  // Deliberately NOT client.setTimeout(): that call was dead here. HTTPClient::connect() uses its own
  // _connectTimeout for the connect, then overwrites the client's timeout with _tcpTimeout right
  // after — so the old client.setTimeout(8) never applied, which is lucky, because 8 ms would have
  // failed every connect. setConnectTimeout here plus the setTimeout after begin() are the two that
  // actually govern; the connect one was sitting at HTTPClient's 5 s default.
  //
  // 12 s per phase, no more: this runs on httpsTask, which is subscribed to the 16 s panic WDT
  // and cannot feed mid-call — the TCP connect is a single select() for the full value. The WDT
  // must stay armed here (it is the only recovery from a wedged op leaving core0Busy stuck with
  // the field held off), so the timeouts fit inside its window instead of widening it. The
  // handshake cap is SECONDS and bounds the mid-handshake stall loop, which otherwise retries
  // 12 s recvs up to the core's 120 s default.
  client.setHandshakeTimeout(12);
  HTTPClient http;
  http.setConnectTimeout(12000);
  String url = String(SUPABASE_URL) + "/functions/v1/update-firmware-version";

  if (!http.begin(client, url)) {
    Serial.println("FW_UPDATE: HTTP begin failed");
    return;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + String(SUPABASE_ANON_KEY));
  http.setTimeout(12000);  // read timeout. Set AFTER begin() deliberately — this is the one that
                           // governs; the setConnectTimeout above governs the connect. 12 s max:
                           // must fit httpsTask's 16 s panic WDT (see connect note above).

  String payload = "{\"token\":\"" + authToken + "\",\"firmware_version_int\":" + String(firmwareVersionInt) + "}";

  int httpCode = http.POST(payload);

  if (httpCode == 200) {
    Serial.println("FW_UPDATE: Success");
    queueConsoleMessage("FW_UPDATE: Version reported");
  } else if (httpCode < 0) {
    Serial.printf("FW_UPDATE: Failed: %s\n", http.errorToString(httpCode).c_str());
  } else {
    Serial.printf("FW_UPDATE: HTTP %d\n", httpCode);
  }

  http.end();
}
void executeCheckForcedUpdate() {
  // Called by HTTPS task on Core 0

  if (currentMode != MODE_CLIENT) {
    Serial.println("FORCED_UPDATE: Not in client mode");
    hasForcedUpdate = false;
    forcedFwVersionInt = 0;
    forcedUpdateDeadline = 0;
    return;
  }

  if (!isRegistered || authToken.length() == 0) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  int rssi = WiFi.RSSI();
  if (rssi < OTA_MIN_RSSI_DBM) {
    Serial.printf("FORCED_UPDATE: WiFi too weak (%d dBm)\n", rssi);
    return;
  }

  Serial.println("FORCED_UPDATE: Checking...");

  WiFiClientSecure client;
  client.setInsecure();
  // client.setTimeout() would be dead here — see the note in executeUpdateFirmwareVersion.
  // 12 s phases + handshake cap: must fit httpsTask's 16 s panic WDT — same note.
  client.setHandshakeTimeout(12);
  HTTPClient http;
  http.setConnectTimeout(12000);
  String url = String(SUPABASE_URL) + "/functions/v1/check-forced-update";

  if (!http.begin(client, url)) {
    Serial.println("FORCED_UPDATE: HTTP begin failed");
    return;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + String(SUPABASE_ANON_KEY));
  http.setTimeout(12000);  // read timeout. Set AFTER begin() deliberately — this is the one that
                           // governs; the setConnectTimeout above governs the connect. 12 s max:
                           // must fit httpsTask's 16 s panic WDT (see connect note above).

  String payload = "{\"token\":\"" + authToken + "\"}";
  int httpCode = http.POST(payload);

  if (httpCode == 200) {
    String response = http.getString();
    Serial.println("FORCED_UPDATE: Response: " + response);

    DynamicJsonDocument doc(1536);
    DeserializationError error = deserializeJson(doc, response.c_str(), response.length());
    if (!error) {
      // Admin config push notice. Parsed BEFORE the forced-update branch on purpose — that
      // branch has an early return (already-on-forced-version) that would otherwise leave the
      // pending-config notice frozen at whatever the last poll saw. Notice only: the config is
      // neither fetched nor applied here, so a cancelled push clears on the next 6 h poll.
      int pendCount = doc["pending_config_count"] | 0;
      cfgPushPendingCount = (pendCount > 255) ? 255 : (uint8_t)pendCount;
      // Admin-authored free text. Sanitized on arrival (not at emit) so /configPush can hand it
      // out raw: drop anything that would break the JSON string, cap the length. Built locally,
      // then one memcpy to the global — the webserver task reads it lock-free.
      const char *pendNote = doc["pending_config_note"];
      char noteBuf[sizeof(cfgPushPendingNote)];
      size_t noteLen = 0;
      if (pendNote != nullptr) {
        for (const char *p = pendNote; *p && noteLen < 80; p++) {
          if (*p == '"' || *p == '\\' || *p == '<' || *p == '>' || (uint8_t)*p < 0x20) continue;
          noteBuf[noteLen++] = *p;
        }
      }
      noteBuf[noteLen] = '\0';
      memcpy(cfgPushPendingNote, noteBuf, noteLen + 1);

      const char *forced_version = doc["forced_fw_version"];
      const char *deadline_str = doc["forced_update_deadline"];

      if (forced_version != nullptr && strlen(forced_version) > 0) {
        String versionStr = String(forced_version);
        int major = 0, minor = 0, patch = 0;

        int firstDot = versionStr.indexOf('.');
        int secondDot = versionStr.indexOf('.', firstDot + 1);

        if (firstDot > 0 && secondDot > firstDot) {
          major = versionStr.substring(0, firstDot).toInt();
          minor = versionStr.substring(firstDot + 1, secondDot).toInt();
          patch = versionStr.substring(secondDot + 1).toInt();
          forcedFwVersionInt = major * 10000 + minor * 100 + patch;
        }

        // Already running the forced version — the post-install clear POST must have
        // failed (network blip before reboot). Drop the flag locally so no stale
        // "Update Now" modal shows, and queue a clear to self-heal the DB row. Safe
        // because versions are monotonic (downgrades refused), so equality is the
        // only "already satisfied" case. Best-effort: a dropped clear retries next boot.
        if (forcedFwVersionInt == firmwareVersionInt) {
          Serial.println("FORCED_UPDATE: Already on forced version — clearing stale flag");
          hasForcedUpdate = false;
          forcedFwVersionInt = 0;
          forcedUpdateDeadline = 0;
          HttpsRequest clearReq = { .type = HTTPS_CLEAR_FORCED_UPDATE };
          xQueueSend(httpsQueue, &clearReq, 0);
          http.end();
          return;
        }

        if (deadline_str != nullptr && strlen(deadline_str) > 0) {
          String timestampStr = String(deadline_str);

          int year = timestampStr.substring(0, 4).toInt();
          int month = timestampStr.substring(5, 7).toInt();
          int day = timestampStr.substring(8, 10).toInt();
          int hour = timestampStr.substring(11, 13).toInt();
          int minute = timestampStr.substring(14, 16).toInt();
          int second = timestampStr.substring(17, 19).toInt();

          struct tm timeinfo;
          timeinfo.tm_year = year - 1900;
          timeinfo.tm_mon = month - 1;
          timeinfo.tm_mday = day;
          timeinfo.tm_hour = hour;
          timeinfo.tm_min = minute;
          timeinfo.tm_sec = second;
          timeinfo.tm_isdst = 0;

          forcedUpdateDeadline = mktime(&timeinfo);
          Serial.printf("FORCED_UPDATE: Deadline: %lu\n", forcedUpdateDeadline);
        }

        hasForcedUpdate = true;
        Serial.printf("FORCED_UPDATE: Version %s required\n", forced_version);
        queueConsoleMessage("⚠️ FORCED UPDATE: Version " + String(forced_version) + " required");

      } else {
        hasForcedUpdate = false;
        forcedFwVersionInt = 0;
        forcedUpdateDeadline = 0;
        Serial.println("FORCED_UPDATE: No mandatory updates");
      }
    }
  } else if (httpCode < 0) {
    Serial.printf("FORCED_UPDATE: Failed: %s\n", http.errorToString(httpCode).c_str());
  } else {
    Serial.printf("FORCED_UPDATE: HTTP %d\n", httpCode);
  }
  http.end();
}
void executeClearForcedUpdate() {
  // Called by HTTPS task on Core 0

  if (!isRegistered || authToken.length() == 0) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (currentMode != MODE_CLIENT) {
    return;
  }

  Serial.println("CLEAR_FORCED_UPDATE: Clearing flag");

  WiFiClientSecure client;
  client.setInsecure();
  // client.setTimeout() would be dead here — see the note in executeUpdateFirmwareVersion.
  // 12 s phases + handshake cap: must fit httpsTask's 16 s panic WDT — same note.
  client.setHandshakeTimeout(12);
  HTTPClient http;
  http.setConnectTimeout(12000);
  String url = String(SUPABASE_URL) + "/functions/v1/clear-forced-update";

  if (!http.begin(client, url)) {
    Serial.println("CLEAR_FORCED_UPDATE: HTTP begin failed");
    return;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + String(SUPABASE_ANON_KEY));
  http.setTimeout(12000);  // read timeout. Set AFTER begin() deliberately — this is the one that
                           // governs; the setConnectTimeout above governs the connect. 12 s max:
                           // must fit httpsTask's 16 s panic WDT (see connect note above).

  String payload = "{\"token\":\"" + authToken + "\"}";
  int httpCode = http.POST(payload);

  if (httpCode == 200) {
    Serial.println("CLEAR_FORCED_UPDATE: Success");
    hasForcedUpdate = false;
    forcedFwVersionInt = 0;
    forcedUpdateDeadline = 0;
  } else {
    Serial.printf("CLEAR_FORCED_UPDATE: HTTP %d\n", httpCode);
  }

  http.end();
}

// Cloud half of the tach-rescale wipe: deletes this device's RPM-indexed cloud data (alt_points,
// the motoring boat_points, the RPM watermarks, the sensor_history RPM columns). The local half
// already ran; front sync stays suppressed until this confirms, otherwise the cloud would ship the
// old-scaled front straight back. Retried every boot/reconnect until it returns 200.
void executeResetRpmAxis() {
  // Called by HTTPS task on Core 0
  if (!rpmAxisWipePending) return;
  if (!isRegistered || authToken.length() == 0) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (currentMode != MODE_CLIENT) return;

  Serial.println("RESET_RPM_AXIS: Requesting cloud wipe");

  WiFiClientSecure client;
  client.setInsecure();
  // client.setTimeout() would be dead here — see the note in executeUpdateFirmwareVersion.
  // 12 s phases + handshake cap: must fit httpsTask's 16 s panic WDT — same note.
  client.setHandshakeTimeout(12);
  HTTPClient http;
  http.setConnectTimeout(12000);
  String url = String(SUPABASE_URL) + "/functions/v1/reset-rpm-axis";

  if (!http.begin(client, url)) {
    Serial.println("RESET_RPM_AXIS: HTTP begin failed");
    return;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + String(SUPABASE_ANON_KEY));
  http.setTimeout(12000);  // read timeout. Set AFTER begin() deliberately — this is the one that
                           // governs; the setConnectTimeout above governs the connect. 12 s max:
                           // must fit httpsTask's 16 s panic WDT (see connect note above).

  String payload = "{\"token\":\"" + authToken + "\"}";
  int httpCode = http.POST(payload);

  if (httpCode == 200) {
    Serial.println("RESET_RPM_AXIS: Success - cloud RPM data wiped, front sync re-enabled");
    rpmAxisWipePending = false;
    settingWrite(NK_RpmAxisWipePend, "0");
    queueConsoleMessage("Cloud RPM-indexed data wiped; alternator record book starts over");
  } else {
    Serial.printf("RESET_RPM_AXIS: HTTP %d - will retry\n", httpCode);
  }

  http.end();
}

// Extract a flat top-level "field":"value" string from a JSON body (returns "" for
// missing or null). Used for the short id fields in the pending-config response; the
// big config blob itself is handled by applyImportConfig's string scan, not ArduinoJson.
static String jsonStringField(const String &body, const char *field) {
  String needle = "\"";
  needle += field;
  needle += "\"";
  int i = body.indexOf(needle);
  if (i < 0) return "";
  i += needle.length();
  while (i < (int)body.length() && (body[i] == ' ' || body[i] == '\t' || body[i] == ':')) i++;
  if (i >= (int)body.length() || body[i] != '"') return "";   // null or non-string -> none
  i++;
  int j = body.indexOf('"', i);
  if (j < 0) return "";
  return body.substring(i, j);
}

// Admin config push (boot-only): ask the cloud whether a config is queued for this device.
// If a new one is present (id != the last applied id stored in NVS), apply tier-1 settings,
// record the id, clear the server flag, and reboot so InitSystemSettings re-reads cleanly.
// The NVS id guard makes this idempotent — a stale/un-cleared flag never re-applies or loops.
void executeGetPendingConfig() {
  if (!isRegistered || authToken.length() == 0) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (currentMode != MODE_CLIENT) return;
  if (WiFi.RSSI() < -80) return;

  // Lean raw-TLS path (doCloudPOST) instead of HTTPClient over WiFiClientSecure: the combined
  // http.begin(client,url) pattern uses far more internal RAM (CLAUDE.md) and getString() can hang,
  // which made this call return -1 when contiguous RAM was tight (e.g. right after the registration
  // handshake). doCloudPOST adds the anon-key Bearer + Content-Type itself. Response holds the full
  // config blob (exportConfigJson reserves 20 KB), so stage it in a 24 KB PSRAM scratch buffer,
  // copy to the String the parsing below expects, and free the scratch immediately.
  String response;
  int httpCode = -1;
  {
    const size_t CAP = 24576;
    char *scratch = (char *)ps_malloc(CAP);
    if (!scratch) {
      Serial.println("PENDING_CONFIG: ps_malloc failed");
      return;
    }
    String payload = "{\"token\":\"" + authToken + "\"}";
    httpCode = doCloudPOST("/functions/v1/get-pending-config", payload.c_str(), scratch, CAP);
    if (httpCode == 200) response = String(scratch);
    free(scratch);
  }
  if (httpCode != 200) {
    Serial.printf("PENDING_CONFIG: HTTP %d\n", httpCode);
    return;
  }

  String pid = jsonStringField(response, "pending_config_id");
  if (pid.length() == 0) {
    Serial.println("PENDING_CONFIG: none queued");
    return;
  }

  String lastApplied = settingExists(NK_lastAppldCfgId) ? settingRead(NK_lastAppldCfgId) : "";
  if (pid == lastApplied) {
    // Already applied — self-heal a server flag the previous boot's clear may have missed.
    Serial.println("PENDING_CONFIG: already applied; self-healing flag");
    pendingConfigClearId = pid;
    executeClearPendingConfig();
    return;
  }

  // A remote push writes every importable key, hardware/calibration included — sensor range,
  // shunt resistance, current zero offsets. applyImportConfig scans the response for the
  // embedded "config" object, so no big JSON parse is needed.
  // Collect the names it actually changes so the dashboard can show the owner what a remote
  // push touched. Written to NVS below and read back next boot — this boot reboots immediately,
  // so there is no live client to tell.
  String changed = "";
  cfgImportChangedNames = &changed;
  int n = applyImportConfig(response.c_str());
  cfgImportChangedNames = nullptr;
  if (n < 0) {
    Serial.println("PENDING_CONFIG: malformed blob, not applied");
    return;
  }
  settingWrite(NK_lastAppldCfgId, pid.c_str());
  if (n > 0) {
    settingWrite(NK_cfgPushNotify, (String(n) + "|" + changed).c_str());
  }
  Serial.printf("PENDING_CONFIG: applied %d settings from config %s; rebooting\n", n, pid.c_str());
  queueConsoleMessage("Config push: applied " + String(n) + " settings, rebooting to load");

  pendingConfigClearId = pid;
  executeClearPendingConfig();   // synchronous (same task) so it completes before the reboot
  rebootRequested = true;
  rebootRequestedAt = millis();
}

// Clear the queued config server-side after applying it. Mirrors executeClearForcedUpdate;
// config_id is matched so a newer push staged in the meantime is not wiped.
void executeClearPendingConfig() {
  if (!isRegistered || authToken.length() == 0) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (currentMode != MODE_CLIENT) return;

  // Lean raw-TLS path (doCloudPOST), same reasons as executeGetPendingConfig. Response is a tiny
  // success ack, so a small stack buffer is plenty.
  String payload = "{\"token\":\"" + authToken + "\",\"config_id\":\"" + pendingConfigClearId + "\"}";
  char response[256];
  int httpCode = doCloudPOST("/functions/v1/clear-pending-config", payload.c_str(), response, sizeof(response));
  Serial.printf("CLEAR_PENDING_CONFIG: HTTP %d\n", httpCode);
}
void printPartitionInfo() {
  Serial.println("=== PARTITION SUMMARY ===");

  const esp_partition_t *running = esp_ota_get_running_partition();
  if (running) {
    Serial.printf("🚀 RUNNING APP: %s - %d bytes (%.2f MB) at 0x%X\n",
                  running->label, running->size, running->size / 1024.0 / 1024.0, running->address);
  }

  Serial.println("\n📋 ALL PARTITIONS IN TABLE:");
  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
  int partitionCount = 0;

  while (it != NULL) {
    const esp_partition_t *partition = esp_partition_get(it);
    const char *type_str = (partition->type == ESP_PARTITION_TYPE_APP) ? "APP" : (partition->type == ESP_PARTITION_TYPE_DATA) ? "DATA"
                                                                                                                              : "OTHER";

    Serial.printf("  %s (%s) - %s: 0x%X -> 0x%X (%d bytes, %.2f MB)\n",
                  partition->label, type_str,
                  getSubtypeString(partition->type, partition->subtype).c_str(),
                  partition->address, partition->address + partition->size,
                  partition->size, partition->size / 1024.0 / 1024.0);
    partitionCount++;
    it = esp_partition_next(it);
  }
  esp_partition_iterator_release(it);
  Serial.printf("Total partitions found: %d\n", partitionCount);

  Serial.println("\n🔍 EXPECTED PARTITION VERIFICATION:");
  checkExpectedPartition("factory", ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, 0x280000);
  checkExpectedPartition("ota_0", ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, 0x280000);
  checkExpectedPartition("factory_fs", ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, 0x100000);
  checkExpectedPartition("prod_fs", ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, 0x100000);
  checkExpectedPartition("nvs", ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, 0x20000);
  checkExpectedPartition("userdata", ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, 0x8C0000);  // matches partitions.csv (0x8E0000 predated the nvs relocation and false-alarmed every boot)

  Serial.println("\n🔄 OTA PARTITION STATUS:");
  const esp_partition_t *ota_partition = esp_ota_get_next_update_partition(NULL);
  if (ota_partition) {
    Serial.printf("Next OTA target: %s at 0x%X\n", ota_partition->label, ota_partition->address);
  }

  const esp_partition_t *boot_partition = esp_ota_get_boot_partition();
  if (boot_partition) {
    Serial.printf("Boot partition: %s at 0x%X\n", boot_partition->label, boot_partition->address);
  }

  Serial.println("\n💾 FLASH & MEMORY INFO:");
  Serial.printf("Flash size: %d MB\n", ESP.getFlashChipSize() / 1024 / 1024);
  Serial.printf("Current sketch size: %d bytes (%.2f MB)\n",
                ESP.getSketchSize(), ESP.getSketchSize() / 1024.0 / 1024.0);
  Serial.printf("Free sketch space: %d bytes (%.2f MB)\n",
                ESP.getFreeSketchSpace(), ESP.getFreeSketchSpace() / 1024.0 / 1024.0);
  Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
  Serial.println("=== END PARTITION SUMMARY ===\n");
}
bool testInternetSpeed() {
  // No core0Busy skip here: the only caller is the OTA pre-flight, which sets core0Busy
  // BEFORE calling — the old skip made this entire reachability gate dead code.
  Serial.println("========================================");
  Serial.println(">>> testInternetSpeed() ENTERED");
  Serial.println("========================================");
  Serial.flush();

  Serial.printf(">>> WiFi.status() = %d (3=connected)\n", WiFi.status());
  Serial.flush();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(">>> Speed test failed: WiFi not connected");
    Serial.flush();
    queueConsoleMessage("Internet check failed: WiFi not connected");
    return false;
  }

  Serial.println(">>> WiFi connected, starting speed test");
  Serial.flush();

  esp_task_wdt_reset();
  Serial.println(">>> Watchdog reset pre-speed-test");
  Serial.flush();

  WiFiClient client;

  // 10 s, up from 3: this is a reachability test, not a latency test, and a 3 s budget failed links
  // that could have completed an OTA fine. It gates performOTAUpdateToVersion, so a false negative
  // here blocks the update outright.
  Serial.println(">>> Test 1: Attempting to connect to 1.1.1.1:80 (10s timeout)");
  Serial.flush();
  unsigned long connectStart = millis();
  if (!client.connect("1.1.1.1", 80, 10000)) {
    Serial.printf(">>> Connection FAILED after %lu ms\n", millis() - connectStart);
    Serial.flush();
    queueConsoleMessage("Internet check failed: No internet access on WiFi network");
    esp_task_wdt_reset();
    return false;
  }

  unsigned long connectTime = millis() - connectStart;
  client.stop();
  esp_task_wdt_reset();
  // Test 2: Download small file and measure speed
  Serial.println(">>> Test 2: Setting up HTTP client for speed test");
  Serial.flush();
  HTTPClient http;
  // Use a reliable small file (Cloudflare's trace - about 200-300 bytes)
  http.begin(client, "http://cloudflare.com/cdn-cgi/trace");
  http.setTimeout(15000);  // 15 s, up from 5: a 300-byte fetch timing out says nothing about a 2.7 MB one
  unsigned long downloadStart = millis();
  int httpCode = http.GET();
  Serial.printf(">>> http.GET() returned code %d after %lu ms\n", httpCode, millis() - downloadStart);
  Serial.flush();
  weatherHttpResponseCode = httpCode;
  esp_task_wdt_reset();
  Serial.println(">>> Watchdog reset #3");
  Serial.flush();

  if (httpCode != 200) {
    http.end();
    Serial.printf(">>> Speed test FAILED: HTTP error %d\n", httpCode);
    Serial.flush();
    queueConsoleMessageF("Internet check failed: Connection error (HTTP %d)", httpCode);
    return false;
  }

  Serial.println(">>> Getting payload string");
  Serial.flush();

  String payload = http.getString();
  esp_task_wdt_reset();  // Feed after potentially slow getString()
  unsigned long downloadTime = millis() - downloadStart;

  int bytesReceived = payload.length();

  Serial.printf(">>> Received %d bytes in %lu ms\n", bytesReceived, downloadTime);
  Serial.flush();

  http.end();
  Serial.println(">>> HTTP connection ended");
  Serial.flush();

  esp_task_wdt_reset();
  Serial.println(">>> Watchdog reset #4");
  Serial.flush();

  float bytesPerSecond = 0;
  if (downloadTime > 0) {
    bytesPerSecond = (bytesReceived * 1000.0) / downloadTime;
  }

  float kbps = (bytesPerSecond * 8.0) / 1000.0;

  Serial.printf(">>> Speed test result: %.2f Kbps (%d bytes in %lu ms)\n", kbps, bytesReceived, downloadTime);
  Serial.flush();

  // Reachability only — deliberately no throughput floor. The old "kbps < 5.0" test was invalid at
  // this sample size: 300 bytes at 5 kbps means the whole GET (DNS + TCP + HTTP round trips to
  // cloudflare.com) had to finish inside 480 ms, so it measured latency, not bandwidth, and refused
  // any high-RTT link that would have carried the 2.7 MB package fine. Bytes arriving at all is the
  // only thing this test can honestly assert. kbps is still logged as a rough field diagnostic.
  if (bytesReceived == 0) {
    Serial.println(">>> Speed test FAILED: connected but received no data");
    Serial.flush();
    queueConsoleMessage("Internet check failed: no data returned");
    return false;
  }

  Serial.printf(">>> Reachability PASSED: %d bytes, %.2f Kbps sample\n", bytesReceived, kbps);
  Serial.flush();
  queueConsoleMessageF("Internet reachable: %.1f Kbps sample", kbps);

  Serial.println("========================================");
  Serial.println(">>> testInternetSpeed() COMPLETE - PASSED");
  Serial.println("========================================");
  Serial.flush();

  return true;
}
void otaRestoreNormalOperation(bool success) {
  Serial.printf("OTA restore: success=%d, restoring system state...\n", success ? 1 : 0);

  core0Busy = false;
  // TempTask was dead for the whole attempt. Restart its 20 s window BEFORE releasing otaInProgress: on the
  // factory path this runs on the web-handler task, and checkTempTaskHealth() (loop) could otherwise see the
  // gap and raise the hung-task alarm before ensureCore0Tasks() has re-created the task.
  lastTempTaskHeartbeat = millis();
  otaInProgress = false;
  lastHttpsOperationTime = millis();
  // TempTask / httpsTask come back through ensureCore0Tasks() on the next loop pass — the one creator outside
  // setup(), where a failed create is retried and alarmed instead of being a Serial line here.
}

// Core 0 task supervisor — the only place outside setup() that creates TempTask / httpsTask. An OTA attempt
// retires both (prepareForOTA) and a failed attempt only clears the flags (otaRestoreNormalOperation), so
// they return here. A create can fail when internal RAM is too fragmented for the stack; that used to be one
// Serial line and a device that stayed blind — no TempTask means the 20 s temperature-stale gate cuts the
// field — until a power cycle. Now: retry every 2 s, console after three misses, and request the maintenance
// reboot that rebuilds everything. Params must match the xTaskCreatePinnedToCore calls in setup().
void ensureCore0Tasks() {
  if (otaInProgress) return;
  static uint32_t lastAttemptMs = 0;
  static uint8_t failStreak = 0;
  static bool alarmed = false;
  bool tempMissing = (tempTaskHandle == NULL);
  bool httpsMissing = (httpsTaskHandle == NULL);
  if (!tempMissing && !httpsMissing) {
    failStreak = 0;
    alarmed = false;
    return;
  }
  uint32_t now = millis();
  if (now - lastAttemptMs < 2000) return;
  lastAttemptMs = now;

  bool restarted = false, failed = false;
  if (tempMissing) {
    if (xTaskCreatePinnedToCore(TempTask, "TempTask", 6144, NULL, 1, &tempTaskHandle, 0) == pdPASS) {
      tempTaskSuspended = false;    // a fresh task runs regardless of what the retired one was doing
      lastTempTaskHeartbeat = now;  // restart the 20 s window before checkTempTaskHealth() sees the gap
      restarted = true;
    } else {
      failed = true;
    }
  }
  if (httpsMissing) {
    if (xTaskCreatePinnedToCore(httpsTask, "HTTPS", 12288, NULL, 1, &httpsTaskHandle, 0) == pdPASS) {
      restarted = true;
    } else {
      failed = true;
    }
  }
  if (restarted) {
    Serial.println("TASKS: Core 0 task(s) restarted");
    queueConsoleMessage("Background tasks restarted after update attempt");
  }
  if (!failed) return;

  size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  if (failStreak < 255) failStreak++;
  Serial.printf("TASKS: create failed (streak %u), internal largest=%u\n", failStreak, (unsigned)largest);
  if (failStreak >= 3 && !alarmed) {
    alarmed = true;
    queueConsoleMessageF("CRITICAL: %s task cannot restart (largest free RAM %u B) - maintenance reboot at next quiet window",
                         (tempTaskHandle == NULL) ? "temperature" : "cloud", (unsigned)largest);
    maintRebootHealthReq = true;
  }
}

// Expiry for otaInProgress. Every call on the update path is bounded (see OTA_WEDGE_LIMIT_MS), so a flag older
// than that means the path is wedged in something with no timeout — and the flag was silencing every recovery
// at once: the console, the TempTask health alarm, and the maintenance reboot ladder. Runs on the Core 1 loop,
// which covers the factory-partition path where the update runs on the web-handler task (not WDT-subscribed);
// the staged path runs on loop() itself and is covered by the 300 s WDT window instead.
void checkOtaWedge() {
  if (!otaInProgress) return;
  if (millis() - otaInProgressSinceMs < OTA_WEDGE_LIMIT_MS) return;
  otaInProgress = false;  // console, health alarm, ladder, and ensureCore0Tasks() all resume
  core0Busy = false;      // set by the wedged path; the ladder's cloudIdle and the field gate need it clear
  Serial.println("OTA WEDGED: update path silent past OTA_WEDGE_LIMIT_MS - flags released, reboot requested");
  queueConsoleMessage("CRITICAL: firmware update stalled with no timeout - background tasks restarting, maintenance reboot at next quiet window");
  maintRebootHealthReq = true;
}
static void otaHeapMark(const char *tag) {
  Serial.printf("HEAP %s: internal_free=%u largest=%u total_free=%u\n",
                tag,
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                ESP.getFreeHeap());
}
// TIME MANAGEMENT FUNCTIONS
// Time sync is deliberately RAM-only. NVS persistence (save/loadTimeSyncState)
// was deleted: the loader was never wired into setup(), so the per-message
// commits only wore the tiny 20KB nvs partition without ever being read back.
// After a reboot the device stays unsynced until a live source (NMEA / phone /
// NTP) reports. Don't re-add persistence casually — restoring a prior boot's
// timeBaseMillis breaks getCurrentTimestamp(): millis() restarts at 0, so the
// unsigned subtraction wraps ~49.7 days into the future.

time_t getCurrentTimestamp() {
  if (timeIsSynced && timeBase > 0) {
    unsigned long elapsedSeconds = (millis() - timeBaseMillis) / 1000;
    return timeBase + elapsedSeconds;
  }
  return 0;
}

// ── Soft clock recovery ─────────────────────────────────────────────────────
// Fixes the phantom multi-hour gaps in the Long Term history plots on a device
// with NO live time source (AP mode, no NTP/phone/GPS). Without a clock the
// long-term records are stamped 0, and the dashboard back-projects those
// zero-stamped records onto the wrong part of the time axis — good data renders
// as a giant gap. Re-establish a usable timebase at boot from two free sources,
// lowest-risk first:
//   1. RTC retention — the ESP32 system clock keeps running across a SOFTWARE
//      reset (OTA install, scheduled restart, crash/WDT). If a prior boot ran
//      settimeofday() (every real source does), time(nullptr) is still valid AND
//      already includes the brief reboot downtime → accurate, no estimate.
//   2. NVS-persisted epoch ("SoftClockEp", written at each field-off save) —
//      survives a cold power-up. Downtime is unknown, so this is an ESTIMATE
//      (assumes ~0 off-time); it snaps to truth the moment a real source reports.
// Either way the source is labeled TIME_ESTIMATED (low confidence) until a real
// source overwrites timeBase. CRITICAL: timeBaseMillis is anchored to the CURRENT
// millis() (it restarts at 0 each boot). Restoring a PRIOR boot's timeBaseMillis
// would wrap getCurrentTimestamp() ~49.7 days forward — exactly the hazard the old
// RAM-only design avoided. We never persist millis, only the epoch, so we're safe.
void restoreSoftClock() {
  if (timeIsSynced) return;   // a live source already won during early boot — leave it

  time_t rtc = time(nullptr);
  if (rtc > SOFTCLOCK_SANE_EPOCH) {            // RTC survived a soft reset → adopt as-is
    timeBase = rtc;
    timeBaseMillis = millis();
    timeIsSynced = true;
    currentTimeSource = TIME_ESTIMATED;
    Serial.printf("Soft clock: adopted retained RTC epoch=%ld\n", (long)rtc);
    return;
  }

  // Cold boot — seed from the epoch we persisted at the previous field-off edge.
  nvs_handle_t h;
  if (nvs_open("storage", NVS_READONLY, &h) == ESP_OK) {
    uint32_t saved = 0;
    if (nvs_get_u32(h, "SoftClockEp", &saved) == ESP_OK && (time_t)saved > SOFTCLOCK_SANE_EPOCH) {
      timeBase = (time_t)saved;
      timeBaseMillis = millis();
      timeIsSynced = true;
      currentTimeSource = TIME_ESTIMATED;
      struct timeval tv = { (time_t)saved, 0 };
      settimeofday(&tv, nullptr);              // keep time(nullptr) consistent for AP-mode callers
      Serial.printf("Soft clock: restored NVS epoch=%lu (estimate, downtime unknown)\n", (unsigned long)saved);
    }
    nvs_close(h);
  }
}
void syncTimeFromGPS(uint16_t daysSince1970, double secondsSinceMidnight) {
  // Manual mode gate: only AUTO and NMEA-forced accept NMEA time.
  if (timeSourceMode == TSRC_PHONE || timeSourceMode == TSRC_NTP) return;
  time_t gpsTime = (daysSince1970 * 86400UL) + (time_t)secondsSinceMidnight;

  if (gpsTime > 1577836800) {  // Jan 1, 2020
    timeBase = gpsTime;
    timeBaseMillis = millis();
    timeIsSynced = true;
    currentTimeSource = TIME_GPS;
    lastTimeSyncAttempt = millis();
    // Set the libc clock too, so time(NULL) is valid in NMEA/AP mode — not just after NTP.
    // Without this every time(NULL) timestamp (captures, sync badges) reads ~1970 off-grid.
    struct timeval tv = { (time_t)gpsTime, 0 };
    settimeofday(&tv, nullptr);

    if (NMEA2KVerbose) {
      Serial.println("Time synced from GPS");
    }
  }
}

// Adopt phone-provided time IF NMEA SystemTime isn't fresher AND the user
// hasn't forced a non-phone source. Called from /set_phone_data handler.
void syncTimeFromPhone(time_t phoneEpochSec) {
  if (phoneEpochSec <= 1577836800) return;  // sanity (pre-2020)
  // Skew sanity: reject phone times more than 24h off a timebase we ALREADY trust
  // (guards against phones with manually-set clocks or sim boots). But a low-confidence
  // soft-clock ESTIMATE (RTC/NVS restore) must NOT trigger this — it's exactly what a real
  // phone time is meant to correct. A bench unit that sat unplugged for days restores a stale
  // estimate; in AP mode the phone/browser POST is the ONLY clock source, so a >24h skew guard
  // here would permanently lock the device to the wrong day. First sync (TIME_NONE) also skips.
  if (timeIsSynced && timeBase > 0 && currentTimeSource != TIME_ESTIMATED) {
    time_t now = getCurrentTimestamp();
    long long diff = (long long)phoneEpochSec - (long long)now;
    if (diff < 0) diff = -diff;
    if (diff > 86400LL) return;  // > 24h off — likely bad phone clock
  }
  // Manual mode gate: only AUTO and PHONE-forced accept phone time.
  if (timeSourceMode == TSRC_NMEA || timeSourceMode == TSRC_NTP) return;
  // AUTO: NMEA still wins if fresh. PHONE-forced: take it regardless.
  if (timeSourceMode == TSRC_AUTO) {
    bool nmeaFresh = (lastNmea2kSystemTimeMs > 0) &&
                     (millis() - lastNmea2kSystemTimeMs < NMEA_TIME_FRESH_MS);
    if (nmeaFresh) return;
  }
  timeBase = phoneEpochSec;
  timeBaseMillis = millis();
  timeIsSynced = true;
  currentTimeSource = TIME_PHONE;
  lastTimeSyncAttempt = millis();
  // Set the libc clock too, so time(NULL) is valid in AP/phone mode — not just after NTP.
  // Without this every time(NULL) timestamp (captures, sync badges) reads ~1970 off-grid.
  struct timeval tv = { (time_t)phoneEpochSec, 0 };
  settimeofday(&tv, nullptr);
}

// Promote phone GPS into the effective Latitude/Longitude globals. Respects
// gpsPositionSource: AUTO defers to fresh NMEA; PHONE-forced always proceeds;
// NMEA-forced never proceeds. MARK_FRESH on each index so IS_STALE-gated
// consumers (distance, smoothing, sensor hist) actually see the phone fix.
void consumePhoneGps() {
  if (gpsManualActive) return;  // sticky manual override beats phone
  if (gpsPositionSource == GPS_SRC_NMEA) return;
  if (gpsPositionSource == GPS_SRC_AUTO) {
    bool nmeaFresh = (lastNmea2kGnssMs > 0) &&
                     (millis() - lastNmea2kGnssMs < NMEA_GPS_FRESH_MS);
    if (nmeaFresh) return;  // NMEA wins in AUTO
  }
  bool phoneFresh = (lastPhoneGpsMs > 0) &&
                    (millis() - lastPhoneGpsMs < PHONE_FRESH_MS);
  if (!phoneFresh) return;
  LatitudeNMEA  = LatitudePhone;
  LongitudeNMEA = LongitudePhone;
  MARK_FRESH(IDX_LATITUDE_NMEA);
  MARK_FRESH(IDX_LONGITUDE_NMEA);
  MARK_FRESH(IDX_SATELLITE_COUNT);
  currentGpsSource = GPS_PHONE;
}

// Phone speed/course — selectable source, never an automatic fallback (user decision
// 2026-08-21): acts only when speedSourceMode == SPD_SRC_PHONE. Called from the
// /set_phone_data handler on each accepted sample (client posts ~2 s in phone mode), so
// the record chain gets real samples at NMEA-like cadence. No per-tick re-assert: if the
// app stops posting, IS_STALE greys the value ~10 s later — the honest display. Not gated
// on gpsManualActive (typed coordinates carry no speed).
void applyPhoneSpeed(bool newSpd, bool newHdg) {
  if (speedSourceMode != SPD_SRC_PHONE) return;
  if (newSpd) {
    SOGNMEA = SpeedPhone;
    MARK_FRESH(IDX_SOG_NMEA);
    updateSustainedSpeed(SOGNMEA);
    wmIgnUpdate(wmIgn_SOG, SOGNMEA);
  }
  if (newHdg) {
    COGNMEA = HeadingPhone;
    MARK_FRESH(IDX_COG_NMEA);
  }
}

// Resolve current position + time source labels every loop tick. Position and
// time are INDEPENDENT settings; each resolves on its own chain.
//
// gpsPositionSource (latitude/longitude):
//   GPS_SRC_AUTO  — freshness-arbitrated: NMEA when fresh, else phone. Promotes
//                   phone GPS to the effective globals when NMEA is stale.
//   GPS_SRC_NMEA  — force NMEA. Don't touch the GNSS handler's writes. Label NMEA
//                   always (even when stale — the dashboard's IS_STALE greying
//                   alerts the user).
//   GPS_SRC_PHONE — force phone. Overwrite LatitudeNMEA/LongitudeNMEA from phone
//                   values. MARK_FRESH only if phone is actually fresh, so stale
//                   forced data doesn't poison distance/smoothing.
//
// timeSourceMode (the clock):
//   TSRC_AUTO  — NMEA SystemTime → phone → NTP, by freshness.
//   TSRC_NMEA  — force NMEA time.
//   TSRC_PHONE — force phone time. Works in AP mode, where nothing else does.
//   TSRC_NTP   — force NTP. Carries no position, so it says nothing about the
//                position chain above.
// Cheap (a few unsigned compares); safe to call every tick.
void resolveSources() {
  unsigned long now = millis();
  bool nmeaGpsFresh   = (lastNmea2kGnssMs > 0)       && (now - lastNmea2kGnssMs       < NMEA_GPS_FRESH_MS);
  bool phoneGpsFresh  = (lastPhoneGpsMs > 0)         && (now - lastPhoneGpsMs         < PHONE_FRESH_MS);
  bool nmeaTimeFresh  = (lastNmea2kSystemTimeMs > 0) && (now - lastNmea2kSystemTimeMs < NMEA_TIME_FRESH_MS);
  bool phoneTimeFresh = (lastPhoneTimeMs > 0)        && (now - lastPhoneTimeMs        < PHONE_FRESH_MS);

  // ── GPS source ─────────────────────────────────────────────────────────
  // Sticky manual override sits ABOVE the source-mode chain: when the user has
  // typed coordinates in Weather Mode, they win over boat NMEA and phone GPS
  // (and even the forced modes) and are reasserted every tick until cleared.
  if (gpsManualActive) {
    LatitudeNMEA  = LatitudeManual;
    LongitudeNMEA = LongitudeManual;
    MARK_FRESH(IDX_LATITUDE_NMEA);   // manual coords are always "fresh" — user vouched for them
    MARK_FRESH(IDX_LONGITUDE_NMEA);
    currentGpsSource = GPS_MANUAL;
  } else
  switch (gpsPositionSource) {
    case GPS_SRC_NMEA:
      currentGpsSource = GPS_NMEA;  // honor user choice even when stale
      break;
    case GPS_SRC_PHONE:
      LatitudeNMEA  = LatitudePhone;
      LongitudeNMEA = LongitudePhone;
      if (phoneGpsFresh) {
        MARK_FRESH(IDX_LATITUDE_NMEA);
        MARK_FRESH(IDX_LONGITUDE_NMEA);
        MARK_FRESH(IDX_SATELLITE_COUNT);
      }
      currentGpsSource = GPS_PHONE;
      break;
    case GPS_SRC_AUTO:
    default:
      if (nmeaGpsFresh) {
        // GNSS handler already set GPS_NMEA + MARK_FRESH on parse.
      } else if (phoneGpsFresh) {
        consumePhoneGps();  // overwrites globals, MARK_FRESH, sets GPS_PHONE
      } else {
        currentGpsSource = GPS_NONE;
      }
      break;
  }

  // ── Speed/course source ────────────────────────────────────────────────
  // Selectable, not arbitrated: speedSourceMode owns speed/course outright.
  // A dead selected source shows its last value greyed via IS_STALE.
  currentSpeedSource = (speedSourceMode == SPD_SRC_PHONE) ? GPS_PHONE : GPS_NMEA;

  // ── Time source label ──────────────────────────────────────────────────
  // (Forced modes never need label-flipping here — the syncTime* setters are
  // already gated, so only the authorized source can write the timebase.)
  if (timeSourceMode == TSRC_AUTO) {
    if (nmeaTimeFresh) {
      // SystemTime handler sets TIME_GPS on parse.
    } else if (phoneTimeFresh) {
      if (currentTimeSource != TIME_PHONE && currentTimeSource != TIME_NTP) {
        currentTimeSource = TIME_PHONE;
      }
    } else if (currentTimeSource == TIME_GPS || currentTimeSource == TIME_PHONE) {
      currentTimeSource = TIME_MILLIS;
    }
  }
}

void checkTimeSync() {
  unsigned long currentMillis = millis();

  if (currentMillis - lastTimeSyncAttempt < TIME_SYNC_INTERVAL) {
    return;
  }

  // AUTO: NTP fires only when BOTH NMEA SystemTime AND phone time are stale
  // (phone is fresher and wins). NTP-forced: always fire — syncTimeFromNTP
  // itself early-returns if the mode disallows it. NMEA/Phone-forced: never
  // touch NTP (the syncTimeFromNTP early-return handles this too, but skip
  // the call to avoid the WiFi check + log noise).
  if (timeSourceMode == TSRC_AUTO) {
    bool nmeaTimeFresh  = (lastNmea2kSystemTimeMs > 0) &&
                          (currentMillis - lastNmea2kSystemTimeMs < NMEA_TIME_FRESH_MS);
    bool phoneTimeFresh = (lastPhoneTimeMs > 0) &&
                          (currentMillis - lastPhoneTimeMs < PHONE_FRESH_MS);
    if (!nmeaTimeFresh && !phoneTimeFresh) syncTimeFromNTP();
  } else if (timeSourceMode == TSRC_NTP) {
    syncTimeFromNTP();
  }
  // TSRC_NMEA / TSRC_PHONE → never NTP.
}
// Format timestamp into static buffer - returns pointer to timestampBuffer
const char *formatTimestamp(time_t timestamp) {
  if (timestamp == 0) {
    timestampBuffer[0] = '\0';
    return timestampBuffer;
  }

  struct tm timeinfo;
  gmtime_r(&timestamp, &timeinfo);
  strftime(timestampBuffer, TIMESTAMP_BUFFER_SIZE, "%Y-%m-%dT%H:%M:%SZ", &timeinfo);

  return timestampBuffer;
}
// Compute collection time for current window
// Returns: valid epoch if time synced, negative millis if not
time_t computeCollectionTime() {
  if (timeIsSynced && timeBase > 0) {
    long long deltaMs = (long long)currentWindow->windowStartTime - (long long)timeBaseMillis;
    long long deltaSec = deltaMs / 1000;
    time_t epoch = timeBase + deltaSec;

    // Sanity check: if result is unreasonable, fall back to millis marker
    if (epoch > 1704067200 && epoch < 2000000000) {
      return epoch;
    }
  }

  // No valid time: return NEGATIVE millis to mark as "needs correction"
  return -(time_t)currentWindow->windowStartTime;
}
// Reconstruct timestamp for buffered record
// Handles negative millis markers and reboot scenarios
time_t reconstructTimestamp(time_t collectionTime) {
  if (collectionTime > 1577836800) {
    // Already a valid epoch
    return collectionTime;
  }

  if (collectionTime < 0) {
    // Was stored as negative millis marker
    unsigned long capturedMillis = (unsigned long)(-collectionTime);

    // Check if reboot happened (millis wrapped or is now less than captured)
    if (millis() < capturedMillis) {
      // Reboot detected: can't reliably reconstruct
      // Use current time as best effort
      Serial.println("Warning: Buffered record from previous boot, using current time");
      return getCurrentTimestamp();
    }

    // Same boot session: reconstruct using time base
    if (timeIsSynced && timeBase > 0) {
      long long deltaMs = (long long)capturedMillis - (long long)timeBaseMillis;
      long long deltaSec = deltaMs / 1000;
      time_t reconstructed = timeBase + deltaSec;

      // Sanity check
      if (reconstructed > 1704067200 && reconstructed < 2000000000) {
        return reconstructed;
      }
    }
  }

  // Fallback: use current time as best effort
  return getCurrentTimestamp();
}


// ══════════════════════════════════════════════════════════════════════════════
// Improv Serial responder (improv-wifi.com/serial) on the USB port
// ══════════════════════════════════════════════════════════════════════════════
// Lets the browser installer (docs.xengineering.net/software/firmware-reset/, ESP Web Tools)
// read the running firmware name / version / serial BEFORE it flashes, and recognise the board
// as the same firmware, so a reinstall is offered as "Update" instead of a "new installation"
// with an erase prompt. IMPROV_FW_NAME must equal the manifest "name" written by
// publish_webflash.sh or the installer will not treat it as the same firmware. The serial rides
// in the device-name string so the installer page can identify a board it only looked at.
// Wi-Fi settings sent from the installer take the same path as the /wifi setup page
// (NK_ssid / NK_pass, restart) — a user click, so the flash write is allowed.
// Packet: "IMPROV" ver=1 type len data checksum(sum of every prior byte & 0xFF) [\n].
// Only loop() reads Serial; each reply is one Serial.write so prints from other tasks cannot
// interleave inside a packet.
static const char IMPROV_FW_NAME[] = "Alternator Regulator Firmware";
enum : uint8_t { IMPROV_T_STATE = 1, IMPROV_T_ERROR = 2, IMPROV_T_RPC = 3, IMPROV_T_RESULT = 4 };
enum : uint8_t { IMPROV_CMD_WIFI = 1, IMPROV_CMD_STATE = 2, IMPROV_CMD_INFO = 3, IMPROV_CMD_SCAN = 4 };
enum : uint8_t { IMPROV_ST_READY = 2, IMPROV_ST_PROVISIONING = 3, IMPROV_ST_PROVISIONED = 4 };
enum : uint8_t { IMPROV_ERR_INVALID = 1, IMPROV_ERR_UNKNOWN_CMD = 2, IMPROV_ERR_UNKNOWN = 0xFF };

static void improvSend(uint8_t type, const uint8_t *data, uint8_t len) {
  uint8_t pkt[9 + 255 + 2];
  memcpy(pkt, "IMPROV", 6);
  pkt[6] = 1;
  pkt[7] = type;
  pkt[8] = len;
  if (len) memcpy(pkt + 9, data, len);
  uint32_t sum = 0;
  for (int i = 0; i < 9 + len; i++) sum += pkt[i];
  pkt[9 + len] = (uint8_t)(sum & 0xFF);
  pkt[10 + len] = '\n';
  Serial.write(pkt, 11 + len);
}

static void improvSendState(uint8_t st) { improvSend(IMPROV_T_STATE, &st, 1); }
static void improvSendError(uint8_t err) { improvSend(IMPROV_T_ERROR, &err, 1); }

// RPC result payload: [cmd, total, (len, bytes)...]. An empty string list is a valid result.
static void improvSendResult(uint8_t cmd, const char *const *strs, int n) {
  uint8_t buf[255];
  int pos = 2;
  for (int i = 0; i < n; i++) {
    if (pos >= 255) break;  // buffer full: a further length byte would land one past the end
    int l = strlen(strs[i]);
    if (pos + 1 + l > 255) l = 255 - pos - 1;
    if (l < 0) l = 0;
    buf[pos++] = (uint8_t)l;
    memcpy(buf + pos, strs[i], l);
    pos += l;
  }
  buf[0] = cmd;
  buf[1] = (uint8_t)(pos - 2);
  improvSend(IMPROV_T_RESULT, buf, (uint8_t)pos);
}

// "Visit device" target. Client mode without a link gives "" so the installer hides the button.
static String improvDeviceUrl() {
  if (currentMode == MODE_CLIENT) return WiFi.status() == WL_CONNECTED ? "http://" + WiFi.localIP().toString() : String("");
  return "http://" + WiFi.softAPIP().toString();
}

static void improvHandleRpc(const uint8_t *d, uint8_t dlen) {
  if (dlen < 2 || 2 + d[1] > dlen) { improvSendError(IMPROV_ERR_INVALID); return; }
  uint8_t cmd = d[0], n = d[1];
  const uint8_t *a = d + 2;
  switch (cmd) {
    case IMPROV_CMD_STATE: {
      uint8_t st = cached_wifi_creds_valid ? IMPROV_ST_PROVISIONED : IMPROV_ST_READY;
      improvSendState(st);
      if (st == IMPROV_ST_PROVISIONED) {  // the client waits for this URL result only when provisioned
        String url = improvDeviceUrl();
        const char *s[1] = { url.c_str() };
        improvSendResult(IMPROV_CMD_STATE, s, 1);
      }
      break;
    }
    case IMPROV_CMD_INFO: {
      // Version carries the compile stamp: several builds share one FIRMWARE_VERSION string
      // (FIRMWARE_0.0.4_PROVENANCE.md), so this is what tells them apart in the installer and
      // in webflash_attempts.fw_version_before. It also never equals the manifest version, which
      // keeps ESP Web Tools on its "Update" button instead of the erase-only same-version dashboard.
      String ver = String(FIRMWARE_VERSION) + " (" __DATE__ " " __TIME__ ")";
      String name = String("XREG-010 ") + device_id_hex;
      const char *s[4] = { IMPROV_FW_NAME, ver.c_str(), "ESP32-S3", name.c_str() };
      improvSendResult(IMPROV_CMD_INFO, s, 4);
      break;
    }
    case IMPROV_CMD_SCAN:
      improvSendResult(IMPROV_CMD_SCAN, nullptr, 0);  // empty list ends the scan; the installer falls back to a typed SSID
      break;
    case IMPROV_CMD_WIFI: {
      // [ssidLen, ssid..., passLen, pass...]
      if (n < 2 || a[0] == 0 || a[0] > 32 || 1 + a[0] >= n) { improvSendError(IMPROV_ERR_INVALID); return; }
      uint8_t sl = a[0], pl = a[1 + sl];
      if (pl > 64 || 2 + sl + pl > n) { improvSendError(IMPROV_ERR_INVALID); return; }
      char ssid[33], pass[65];
      memcpy(ssid, a + 1, sl);
      ssid[sl] = '\0';
      memcpy(pass, a + 2 + sl, pl);
      pass[pl] = '\0';
      improvSendState(IMPROV_ST_PROVISIONING);
      if (!settingWrite(NK_ssid, ssid) || !settingWrite(NK_pass, pass)) { improvSendError(IMPROV_ERR_UNKNOWN); return; }
      strncpy(cached_wifi_ssid, ssid, sizeof(cached_wifi_ssid) - 1);
      cached_wifi_ssid[sizeof(cached_wifi_ssid) - 1] = '\0';
      strncpy(cached_wifi_pass, pass, sizeof(cached_wifi_pass) - 1);
      cached_wifi_pass[sizeof(cached_wifi_pass) - 1] = '\0';
      cached_wifi_creds_valid = true;
      settingWrite(NK_first_config_done, "1");
      improvSendState(IMPROV_ST_PROVISIONED);
      const char *s[1] = { "http://alternator.local" };  // the mDNS name client mode registers; the DHCP address is unknown until after the restart
      improvSendResult(IMPROV_CMD_WIFI, s, 1);
      Serial.printf("=== IMPROV: ship WiFi '%s' saved, restarting ===\n", ssid);
      rebootRequested = true;  // loop() restarts 2 s later, after these packets have left
      rebootRequestedAt = millis();
      break;
    }
    default:
      improvSendError(IMPROV_ERR_UNKNOWN_CMD);
  }
}

// Called every loop() pass; returns immediately when nothing is buffered. Debug output shares
// the port, so bytes are scanned for the header rather than trusting alignment.
void improvSerialPoll() {
  static uint8_t buf[9 + 255 + 1];
  static uint16_t n = 0, need = 0;
  static const char HDR[6] = { 'I', 'M', 'P', 'R', 'O', 'V' };
  int guard = 0;
  while (Serial.available() > 0 && guard++ < 512) {
    uint8_t c = (uint8_t)Serial.read();
    if (n < 6) {
      if (c == HDR[n]) {
        buf[n++] = c;
      } else {
        n = (c == 'I') ? 1 : 0;
        if (n) buf[0] = c;
      }
      continue;
    }
    buf[n++] = c;
    if (n == 9) {
      if (buf[6] != 1) { n = 0; continue; }  // unsupported protocol version
      need = 9 + buf[8] + 1;
    }
    if (n >= 9 && n == need) {
      uint32_t sum = 0;
      for (uint16_t i = 0; i < need - 1; i++) sum += buf[i];
      if ((uint8_t)(sum & 0xFF) != buf[need - 1]) improvSendError(IMPROV_ERR_INVALID);
      else if (buf[7] == IMPROV_T_RPC) improvHandleRpc(buf + 9, buf[8]);
      n = 0;
    }
  }
}
