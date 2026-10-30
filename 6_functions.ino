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
// Forward declarations

// Control-path serial: skip the write when the TX buffer lacks room — a connected-but-stalled
// USB host otherwise blocks the control tick. queueConsoleMessage still carries the line to the UI.
static inline void serialPrintlnNB(const char *msg) {
  size_t n = strlen(msg);
  if ((size_t)Serial.availableForWrite() >= n + 2) Serial.println(msg);
}

void applyImmediateCut(const TickSnapshot &tick, FieldEventReason reason, bool rpmTriggered);
// ==================== FIELD CONTROL HELPER FUNCTION DECLARATIONS ====================
// Snapshot builder
TickSnapshot buildTickSnapshot(uint32_t currentMillis, uint32_t dt_ms);
// Mode selection (pure functions)
FieldControlMode selectFieldControlMode(const TickSnapshot &tick);
FieldEventReason selectFieldEventReason(const TickSnapshot &tick);
// Battery-temperature source chain (defined next to computeCvTempScale below)
float batteryTempF(uint8_t *srcOut);
bool tempProbeFresh(uint8_t idx);
const char *battTempSrcName(uint8_t src);
void updateProtectionCounters(FieldEventReason reason);
// Sustained timer functions
bool isTempSustainedWarning(uint32_t nowMs, float tempToUseF, float tempLimitF,
                            float tempWarnExcessF, bool ignoreTemperature);
bool isVoltageDisagreementWarning(uint32_t nowMs, float batteryV, float ibv,
                                  bool voltagePlausible, bool voltageDisagreementCritical);
bool isVoltageSensorPlausible();
bool isVoltageDisagreementCritical();
// RPM table shared segment resolver and interpolator
int findRPMSegment(float rpm);
float interpolateRPMTable(float rpm, const float *table);
// RPM-dependent table lookups (all delegate to interpolateRPMTable)
float getMinimumFieldForRPM(float rpm);
float getCapCurrentForRPM(float rpm);

// Commissioned field drain time at this RPM: linear between the tested endpoints, CLAMPED to the tested range
// (an extrapolated underestimate would release the OV clamp with the field still hot). No line commissioned or
// RPM unknown → fieldDecayTauMs, the stored worst-case endpoint.
float fdDrainMsAtRpm(float rpm) {
  if (fdDrainRpmHi <= fdDrainRpmLo || fdDrainLoMs == 0 || fdDrainHiMs == 0 || !(rpm > 0.0f))
    return (float)fieldDecayTauMs;
  if (rpm <= (float)fdDrainRpmLo) return (float)fdDrainLoMs;
  if (rpm >= (float)fdDrainRpmHi) return (float)fdDrainHiMs;
  float f = (rpm - (float)fdDrainRpmLo) / (float)(fdDrainRpmHi - fdDrainRpmLo);
  return (float)fdDrainLoMs + f * ((float)fdDrainHiMs - (float)fdDrainLoMs);
}
// Auto Min% learning ("knee tracker") — observer + persistence (defined lower in this file)
void kneeLearnObserve(float rpm, float appliedDuty, float tF, float amps,
                      float dutyRequest, float rpmFloorDuty, bool modeOk);
void kneeLearnInit();
void kneeLearnService(bool fieldOff);
void saveKneeLearnState();
static bool kneePutStateBlobs(nvs_handle_t h);
void kneeLearnResetDefaults();
String kneeLearnStateJson();
// RPM table index (for UI highlighting and bucket history)
void updateCurrentRPMTableIndex(float rpm);
bool shouldImmediatelyCutGPIO4(FieldEventReason reason);
bool shouldCutGPIO4AfterSettle(FieldEventReason reason, uint32_t nowMs, float appliedDuty);
void updateFieldTelemetry(float duty, float voltage);
void reportFieldModeEvent(uint32_t nowMs, FieldControlMode mode, FieldEventReason reason,
                          const TickSnapshot &tick, bool gpio4Low, float appliedDuty);

// String conversion for logging
const char *modeToString(FieldControlMode mode);
const char *reasonToString(FieldEventReason r);
// Control loop sub-path helpers
void runReducedSensorControl(const TickSnapshot &tick, FieldEventReason reason, float actualDtSec);
static void endFieldTestsOutsideAuto(const TickSnapshot &tick, FieldControlMode mode, FieldEventReason reason);
static float rpmCapAmpsNow(const TickSnapshot &tick);
void reducedCtlLeaveTiers34();
bool reasonSuppressedByOverride(FieldEventReason r, const TickSnapshot &t);
uint16_t sensorOverriddenBits();
const char *sensorFaultName(uint8_t bit);
void sensorFaultNames(uint16_t bits, char *out, size_t n);
float smallestRpmTableAmps();
float largestRpmTableMinDuty();
static inline bool immediateCutNow(FieldEventReason reason, const TickSnapshot &tick);
bool fieldReasonIsFault(FieldEventReason r);
bool voltageInPlausibleWindow(float v);
void runShutdownPath(const TickSnapshot &tick, FieldControlMode mode, FieldEventReason reason,
                     float actualDtSec, bool exitingNormal);
void runCommissionIdle(const TickSnapshot &tick, FieldEventReason reason, float actualDtSec);
void runFieldResTestHold(const TickSnapshot &tick, FieldEventReason reason);

// ==================== HELPER FUNCTIONS ====================

float clamp_f(float x, float lo, float hi) {
  if (x < lo) return lo;
  if (x > hi) return hi;
  return x;
}

// Two-reading rule (RPM cuts, tach-lie count): true once `low` has held minMs AND readStamp moved while it held
// (dataTimestamps[IDX_RPM] moves per committed CH2 read; rpmUnheldReadMs skips lone-jump-held reads), or maxMs
// regardless: failed reads, a dead converter and loop stalls move neither stamp.
struct RpmLowConfirm {
  uint32_t sinceMs = 0;   // millis() when `low` started (0 = not low)
  uint32_t readMs = 0;    // readStamp (stampA) at that moment
  uint32_t readMsB = 0;   // stampB at that moment
  bool update(bool low, uint32_t nowMs, uint32_t minMs, uint32_t maxMs, uint32_t readStamp) {
    return update(low, nowMs, minMs, maxMs, readStamp, readStamp, false);
  }
  // Both stamps are recorded at the start and each is compared only with its own start value, so a caller
  // switching useB mid-dwell can neither invent a newer reading nor lose one.
  bool update(bool low, uint32_t nowMs, uint32_t minMs, uint32_t maxMs, uint32_t stampA, uint32_t stampB, bool useB) {
    if (!low) { sinceMs = 0; return false; }
    if (sinceMs == 0) { sinceMs = nowMs; readMs = stampA; readMsB = stampB; }
    const uint32_t heldMs = (uint32_t)(nowMs - sinceMs);
    const bool newerRead = useB ? (stampB != readMsB) : (stampA != readMs);
    return (heldMs >= minMs && newerRead) || heldMs >= maxMs;
  }
};

// CV auto-tune (CV_AUTOTUNE_PLAN.md §E): only the DC gain K_dc (cvPlantK) is fit; Kp = α/K at the ~0.6 s horizon,
// Ki = ρ·Kp (α/ρ = cvAlpha/cvPiZero settings). The loop is stability-robust, so τ/L are never measured.

// computeCvTempScale: Kp+Ki derate = R(T_commission)/R(T_now); pack resistance IS the CV plant gain. cvResistanceRatio's
// saturation, not a ratio clamp, bounds it, so the bite point is an absolute temperature. 1.0 when disabled,
// uncommissioned or no source; a non-measured commission stamp (src 0 or 5) also sets cvTempDerateInert.
static const float CVTS_T_MIN_C = -40.0f, CVTS_T_MAX_C = 70.0f;   // BMP388 rated span; also the charging envelope
static const float CVTS_WARM_RATIO = 0.5f;
static const float CVTS_R_HOT = 0.55f, CVTS_R_COLD = 7.0f;
static const float CVTS_SCALE_MIN = 0.10f, CVTS_SCALE_MAX = 2.50f;

// R(T)/R(25°C): two segments pivoting at 25°C, since activation energy ~halves above room temp (LFP 0.026→0.010 /°C,
// lead-acid 0.023→0.015 /°C); ≤12% error over −20…+55°C on both, so no per-chemistry coefficient. The clamps only
// backstop a hand-set battTempCoeff; real VRLA impedance keeps falling with heat (thermal runaway).
static float cvResistanceRatio(float tC) {
  float c = (tC < 25.0f) ? battTempCoeff : (battTempCoeff * CVTS_WARM_RATIO);
  return clamp_f(expf(-c * (tC - 25.0f)), CVTS_R_HOT, CVTS_R_COLD);
}

// Idle-aware age gate (same shape as tempDataVeryStale): 20 s with the engine running, 90 s otherwise (TempTask polls
// at 60 s engine-off). False on the battery probe drives REASON_BATT_TEMP_LOST; the extra probe is advisory.
bool tempProbeFresh(uint8_t idx) {
  uint32_t now = millis();
  bool running = g_engineRunning;   // §1.1: computed once per tick in buildTickSnapshot (ignition stands in for an ignored tach)
  uint32_t ts = dataTimestamps[idx];
  if (ts == 0) return false;
  uint32_t age = (ts > now) ? 0 : (now - ts);   // a Core-0 stamp can lead this Core-1 clock read by a few ms
  return age <= (running ? 20000UL : 90000UL);
}

// Source codes shared with CSV2 battTempActiveSrc and the UI:
// 0 none, 1 probe, 2 NMEA 2000 (127508), 3 VE.Direct (T field), 4 RV-C (DC_SOURCE_STATUS_2). 5 is retired
// and never emitted; the remaining codes keep their values so stored settings and CSV2 stay valid.
float batteryTempF(uint8_t *srcOut) {
  // Every gate is evaluated each call so tempProbeFresh's idle tracker keeps running whatever the selection.
  // Demo mode simulates a probe reading whether or not the probe is enabled (demoReadSensors, 9_demo.ino).
  bool okProbe = tempProbeFresh(IDX_BATT_TEMP_PROBE) && (battTempProbeEnable == 1 || hardwarePresent == 0) && isfinite(BatteryTempProbeF);
  bool okVe    = tempProbeFresh(IDX_VE_BATT_TEMP) && isfinite(VictronBattTempF);
  bool okRvc   = tempProbeFresh(IDX_RVC_BATT_TEMP) && isfinite(rvcRxBattTempF);
  bool okN2k   = !IS_STALE(IDX_N2K_BATT) && isfinite(n2kRxBattTempF);
  uint8_t src = 0;
  float t = NAN;
  switch (battTempSource) {
    case 0:  // Auto: first qualifying measurement, no fallback
      if (okProbe)      { src = 1; t = BatteryTempProbeF; }
      else if (okN2k)   { src = 2; t = n2kRxBattTempF; }
      else if (okVe)    { src = 3; t = VictronBattTempF; }
      else if (okRvc)   { src = 4; t = rvcRxBattTempF; }
      break;
    case 1: if (okProbe) { src = 1; t = BatteryTempProbeF; } break;
    case 2: if (okN2k)   { src = 2; t = n2kRxBattTempF; }   break;
    case 3: if (okVe)    { src = 3; t = VictronBattTempF; } break;
    case 4: if (okRvc)   { src = 4; t = rvcRxBattTempF; }   break;
    default: break;  // 5 retired (board stand-in); 6 = none
  }
  if (srcOut) *srcOut = src;
  return t;
}

const char *battTempSrcName(uint8_t src) {
  switch (src) {
    case 1: return "probe";
    case 2: return "NMEA 2000";
    case 3: return "VE.Direct";
    case 4: return "RV-C";
    default: return "none";
  }
}

float computeCvTempScale() {
  cvTempDerateInert = false;
  if (!battTempDerateEnable) return 1.0f;
  if (isnan(CommissionTempF)) return 1.0f;
  if (battTempActiveSrc < 1 || battTempActiveSrc > 4 || !isfinite(battTempActiveF)) return 1.0f;
  if (CommissionTempSrc < 1 || CommissionTempSrc > 4) { cvTempDerateInert = true; return 1.0f; }
  float tCommC = clamp_f((CommissionTempF  - 32.0f) / 1.8f, CVTS_T_MIN_C, CVTS_T_MAX_C);
  float tNowC  = clamp_f((battTempActiveF - 32.0f) / 1.8f, CVTS_T_MIN_C, CVTS_T_MAX_C);
  // Asymmetric: a boost raises loop gain, and an over-large coefficient inflates it further, so the boost
  // is capped tighter than the cut. It only bites below ~0°C commissioning.
  return clamp_f(cvResistanceRatio(tCommC) / cvResistanceRatio(tNowC), CVTS_SCALE_MIN, CVTS_SCALE_MAX);
}

// Call after any related setting change, on battery-temp drift, and at boot. Gains are computed in 12V-equivalent
// space and ×vNorm back to pack space; the temp derate multiplies BOTH modes.
void recomputeCvGains() {
  float vNorm = 12.0f / (float)SYSTEM_VOLTAGE_CLASS;     // 1, 0.5, 0.33, 0.25 for 12/24/36/48 V
  cvPlantK = cvPlantKa;                             // the ~0.6 s stiffness anchor; the √t-tail (cvPlantKb) is retired
  bool plantValid = (cvPlantK > 1e-6f);
  float kpNorm, kiNorm, kdNorm;                     // 12V-equivalent gains (what the user sees)
  if (cvGainMode == 1 && plantValid) {
    // Measured: Kp = α/K anchored to the ~0.6 s stiffness — the timescale the loop reacts on
    float Knorm = cvPlantK * vNorm;                 // V per 12V-equivalent, per A
    kpNorm = cvAlpha / Knorm;
    kiNorm = cvPiZero * kpNorm;                     // Ki = ρ · Kp (from the UN-clamped Kp, then clamp both)
    // Safety bounds — a bad fit must never produce dangerous gains.
    kpNorm = clamp_f(kpNorm, 2.0f, 120.0f);
    kiNorm = clamp_f(kiNorm, 1.0f, 80.0f);
    // D = Td·Kp (off the CLAMPED Kp): a fixed derivative time makes Kd track Kp, so it inherits the α/K
    // plant anchor and auto-scales per-install. Ceiling guards a pathological fit (soft plant → huge Kp).
    kdNorm = clamp_f(CvKdTd * kpNorm, 0.0f, 40.0f);
  } else {
    // Manual, or Measured with no valid fit yet → use the typed / conservative gains.
    kpNorm = VoltageKp;
    kiNorm = VoltageKi;
    kdNorm = VoltageKd;                             // Manual keeps the typed Kd (bench override)
  }
  cvComputedKp = kpNorm;                            // expose for the dashboard — BASE design gain (no temp derate)
  cvComputedKi = kiNorm;
  cvComputedKd = kdNorm;
  cvTempDerateScale = computeCvTempScale();         // battery-temp correction (1.0 unless commissioned + enabled)
  VoltageKp_active = kpNorm * vNorm * cvTempDerateScale;   // pack-space gains the loop uses with raw pack-volt error
  VoltageKi_active = kiNorm * vNorm * cvTempDerateScale;
  VoltageKd_active = kdNorm * vNorm * cvTempDerateScale;
}

// PidKp/Ki/Kd are referenced to a 12 V winding on a 12 V bus; × fieldScale() gives the duty-space gains, so one tuning
// asks the same share of rated field per amp on any class/winding. The hunt-governor Ki derate is applied here, the
// single SetTunings choke point, so every recompute path preserves it.
void recomputeCcGains() {
  const float fs = fieldScale();     // 1 at 12/12, 0.25 for a 12 V winding at 48 V, 1 for a 48 V winding at 48 V
  PidKp_active = PidKp * fs;
  PidKi_active = PidKi * fs;
  PidKd_active = PidKd * fs;
  currentPID.SetTunings(PidKp_active, PidKi_active * g_huntDerate, PidKd_active);
}

// ==================== HUNT GOVERNOR v2 ====================
// Speed-map oscillation damper (HUNT_GOVERNOR_V2_SPEC.md): while RPM is steady, make ONE cut to HuntCutPct and keep it
// only if averaged ripple drops >= HuntVerifyPct; a kept cut becomes a persistent speed pocket. Repeats widen a pocket,
// never deepen it; a wobble persisting inside a pocket is reported as external/mechanical.
#define HG_RING_N 128            // 6.4 s at 50 ms — two periods of the lowest bin (hard floor for 0.31 Hz selectivity)
#define HG_EVAL_EVERY 32         // evaluate every 1.6 s (window/4)
#define HG_NBINS 6
// Detection bar = HuntTrigPct (0.5% duty at 12 V, no upper clamp; a real idle hunt can read 0.4–0.6%), rescaled with
// fieldScale() via scaleFieldSideKnobs. Above 12 V the D-attributed bar (x HG_TRIG_D_FRAC) nears 1.5 PWM LSB, so the
// kdTrim alternation gate carries D detection (HUNT_GOVERNOR_V2_SPEC.md "Higher-voltage installs").
#define HG_TRIG_RATIO 3.0f       // peak vs median of other bins — rejected a 2.22% broadband load transient amplitude alone would have flagged
#define HG_TRIG_D_FRAC 0.3f      // D-attributed duty bar as a FRACTION of HuntTrigPct: the D-driven mode lives in VOLTS, its duty amplitude can sit below the quiet-log floor. Safe only because kdTrim alternation is the real gate (a quiet log has none); proportional so it never gets stricter than the standard bar
// HuntQualifyScans consecutive hunt+steady scans open a test; their average is the baseline (single windows
// jitter >15%, too much for the HuntVerifyPct bar)
#define HG_BLIND_SCANS 4         // clean scans discarded after the cut while the 6.4 s window refills with post-cut data
#define HG_READ_SCANS 3          // clean scans averaged for the verify reading
#define HG_EP_TIMEOUT_SCANS 25   // ~40 s — contamination can freeze clean-scan progress; bail with nothing kept
#define HG_RPM_MIN 100.0f        // below this there is no usable speed axis (no tach signal) — the damper stays inert
#define HG_CV_COVER_MIN 0.75f    // min share of a scan's ticks in CV before its |cvDSlope| mean counts: the metric only accumulates in CV, so an uncovered scan averages 0 and clears any improvement bar. 0.75 over HG_READ_SCANS gives >= 3.6 s of CV, longer than one 0.31 Hz period
#define HG_MAX_POCKETS 4         // merging keeps the real count low; least-recently-confirmed evicted if ever exceeded
static const float HG_BIN_HZ[HG_NBINS] = { 0.31f, 0.47f, 0.70f, 1.05f, 1.56f, 2.34f };
static float hgRing[HG_RING_N];  // 512 B, deliberately internal: control-adjacent and far below the PSRAM-rule size
static uint16_t hgHead = 0;
static uint32_t hgTick = 0;
static uint32_t hgContamTick = 0;  // hgTick at the last contaminated push — window is clean once hgTick-this >= HG_RING_N
static uint32_t hgLastPushMs = 0;
static float hgDtEmaMs = 50.0f;
static float hgSpEma = 0.0f;
static float hgRpmSum = 0.0f;    // scan-mean RPM accumulator (drained each evaluation) — steadiness + pocket lookup both work on window means, so the hunt's own speed ripple never disqualifies itself
static uint16_t hgRpmCnt = 0;
static float hgSlopeSum = 0.0f;  // Σ|cvDSlope| over the scan (drained each evaluation): D-test verify metric, a voltage-amplitude proxy at a fixed ring frequency
static uint16_t hgSlopeCnt = 0;
static uint16_t hgKdFlips = 0;   // kdTrim sign alternations since the last scan (drained each evaluation) — D-lever attribution
static int8_t hgKdSign = 0;      // last nonzero kdTrim sign (zero spans between engagements don't break an alternation)

// Per-control-tick sampler: one ring write plus contamination flags; analysis runs elsewhere. Self-clocked to ~50 ms
// so loop cadence changes don't move the band; Goertzel coefficients use the measured dt EMA.
void huntGovObserve(float dutyApplied, bool closedLoopOk) {
  uint32_t nowMs = millis();
  if (hgLastPushMs != 0 && (uint32_t)(nowMs - hgLastPushMs) < 45UL) return;
  float dtMs = (hgLastPushMs == 0) ? 50.0f : (float)(uint32_t)(nowMs - hgLastPushMs);
  hgLastPushMs = nowMs;
  if (dtMs > 250.0f) {  // sampling gap (mode exit, stall) — window unusable
    dtMs = 250.0f;
    hgContamTick = hgTick;
  }
  hgDtEmaMs += 0.05f * (dtMs - hgDtEmaMs);
  hgRing[hgHead] = dutyApplied;
  hgHead = (uint16_t)((hgHead + 1) % HG_RING_N);
  hgTick++;
  hgRpmSum += (float)RPM;
  hgRpmCnt++;
  // D-lever attribution: a healthy D-term fires in brief one-sided bursts; sustained sign ALTERNATION is a D-term doing
  // no net work or driving a limit cycle. Comparing to the last nonzero sign survives sampled zero spans.
  if (g_cvKdTrimLive > 0.001f) {
    if (hgKdSign < 0) hgKdFlips++;
    hgKdSign = 1;
  } else if (g_cvKdTrimLive < -0.001f) {
    if (hgKdSign > 0) hgKdFlips++;
    hgKdSign = -1;
  }
  if (voltageControlActive) {  // cvDSlope only updates in CV — never accumulate a stale CC-phase value
    hgSlopeSum += fabsf(cvDSlope);
    hgSlopeCnt++;
  }
  float spDev = fabsf(setpointLimited - hgSpEma);
  hgSpEma += (dtMs / (2000.0f + dtMs)) * (setpointLimited - hgSpEma);
  bool contam = !closedLoopOk
                || g_fastOvClampActive || g_loadDumpActive || g_cvRecovActive
                || TuningMode || CVTuningMode || batteryHealthTestActive
                || systemIDActive || fieldCurveActive || fieldCutActive
                || cvPlantFitActive || resTestActive || cvStressActive
                || protTestActive || (altSweepActive != 0) || (chcActive != 0) || (ManualFieldToggle == 1)
                || IgnoreRPM || LimpHome   // an ignored tach can neither prove a steady speed nor name a pocket
                || spDev > fmaxf(3.0f, 0.04f * (float)AlternatorNominalAmps);
  if (contam) hgContamTick = hgTick;
}

// ── Episode ledger ────────────────────────────────────────────────────────────────────────
// Verdicts fire in the control path, so they never touch flash (a LittleFS append can stall tens of ms, fsExists() up
// to 5 s on the FS mutex): they push to this RAM queue; huntLedgerService() does the flash work under the field-off gate.
struct HgLedgerRec {
  uint32_t epoch;
  float freqHz, a0, aEnd, derate;
  int16_t rpm;
  uint8_t cv, steps;
  char verdict[20];
};
#define HG_LEDGER_QUEUE_N 12       // episodes that can wait in RAM for the engine to stop; past this the oldest are counted and dropped
#define HG_LEDGER_MAX_BYTES 6144   // hard file cap (~100 rows); the oldest half is dropped at the cap
static const char *HG_LEDGER_HDR = "epoch,rpm,cv,freqHz,a0_pct,aEnd_pct,steps,verdict,derateExit";
static HgLedgerRec (&hgQueue)[HG_LEDGER_QUEUE_N] = PsramPlace::make<HgLedgerRec[HG_LEDGER_QUEUE_N]>();
static uint8_t hgQueueN = 0;
static uint16_t hgQueueDropped = 0;

static void hgLedgerQueue(const char *verdict, float freqHz, float a0, float aEnd, uint8_t steps) {
  if (IgnoreRPM || LimpHome || demoSeal) return;   // the record's speed would be an ignored reading; demo episodes are not records
  if (hgQueueN >= HG_LEDGER_QUEUE_N) {
    hgQueueDropped++;
    return;
  }
  HgLedgerRec &r = hgQueue[hgQueueN++];
  r.epoch = (uint32_t)time(nullptr);  // stamped at the verdict, not at the flush
  r.freqHz = freqHz;
  r.a0 = a0;
  r.aEnd = aEnd;
  r.derate = g_huntDerate;
  r.rpm = (int16_t)RPM;
  r.cv = voltageControlActive ? 1 : 0;
  r.steps = steps;
  strncpy(r.verdict, verdict, sizeof(r.verdict) - 1);
  r.verdict[sizeof(r.verdict) - 1] = '\0';
}

uint8_t hgLedgerPendingCount() {
  return hgQueueN;
}

// Queued (not yet on flash) records as ledger CSV, so /huntledger shows an episode before its flush.
size_t hgLedgerPendingCsv(char *out, size_t cap) {
  size_t n = 0;
  for (uint8_t i = 0; i < hgQueueN && n < cap; i++) {
    const HgLedgerRec &r = hgQueue[i];
    int w = snprintf(out + n, cap - n, "%lu,%d,%d,%.2f,%.2f,%.2f,%u,%s,%.2f\n",
                     (unsigned long)r.epoch, (int)r.rpm, (int)r.cv, r.freqHz, r.a0, r.aEnd,
                     (unsigned)r.steps, r.verdict, r.derate);
    if (w <= 0 || (size_t)w >= cap - n) break;
    n += (size_t)w;
  }
  return n;
}

// The ONLY code that writes the ledger to flash. Main loop, field-off-settled gate only.
void huntLedgerService() {
  if (hgQueueN == 0) return;
  fsTakeLock();  // fsExists()/fsRead() take this same non-recursive mutex — use raw LittleFS inside
  if (!LittleFS.exists("/huntledger.csv")) {
    File h = LittleFS.open("/huntledger.csv", "w");
    if (h) {
      h.println(HG_LEDGER_HDR);
      h.close();
    }
  } else {
    File c = LittleFS.open("/huntledger.csv", "r");
    if (c) {
      uint32_t sz = c.size();
      if (sz > HG_LEDGER_MAX_BYTES) {  // cap the ledger: drop the oldest half, keep whole lines
        c.seek(sz / 2);
        c.readStringUntil('\n');  // resync to the next line boundary
        uint32_t rem = sz - (uint32_t)c.position();
        char *tmp = (char *)ps_malloc(rem);  // PSRAM: a ~3 KB trim buffer must not fragment the internal heap
        uint32_t kept = (tmp && rem) ? c.read((uint8_t *)tmp, rem) : 0;
        c.close();
        if (rem == 0 || (tmp && kept == rem)) {  // no buffer or a short read: leave the file whole, trim on a later flush
          File w = LittleFS.open("/huntledger.csv", "w");
          if (w) {
            w.println(HG_LEDGER_HDR);
            if (kept) w.write((uint8_t *)tmp, kept);
            w.close();
          }
        }
        if (tmp) free(tmp);
      } else {
        c.close();
      }
    }
  }
  File f = LittleFS.open("/huntledger.csv", "a");
  if (f) {
    for (uint8_t i = 0; i < hgQueueN; i++) {
      const HgLedgerRec &r = hgQueue[i];
      f.printf("%lu,%d,%d,%.2f,%.2f,%.2f,%u,%s,%.2f\n", (unsigned long)r.epoch, (int)r.rpm,
               (int)r.cv, r.freqHz, r.a0, r.aEnd, (unsigned)r.steps, r.verdict, r.derate);
    }
    f.close();
    hgQueueN = 0;  // cleared only on a successful open — a failed write retries next pass
    if (hgQueueDropped) {
      queueConsoleMessageF("Oscillation damper: %u episode record(s) were lost before the engine stopped long enough to save them", (unsigned)hgQueueDropped);
      hgQueueDropped = 0;
    }
  }
  fsFreeDirty = true;
  fsReleaseLock();
}

// ── Pocket map ────────────────────────────────────────────────────────────────────────────
// Pocket = flat core [lo,hi] rpm at HuntCutPct gain plus linear taper wings HuntWingPct beyond each end. RAM is
// authoritative; /huntpockets.csv is the boot-restore copy, written only by huntMapService() under the field-off gate.
// The map compares the device's own RPM readings, but a recalibration rescales the axis, so rpmAxisWipeExecute clears it.
struct HgPocket {
  float lo, hi;      // flat-core bounds: min..max of the verified episode speeds
  uint32_t epoch;    // last confirmation (eviction order)
  uint8_t lever;     // which lever this pocket holds: 0 = inner Ki at HuntCutPct, 1 = CV D-term off. Pockets of different levers may overlap — they act on independent gains
};
static HgPocket hgPockets[HG_MAX_POCKETS];
static uint8_t hgPocketN = 0;
static bool hgMapDirty = false;

// v2 episode state — file-scope so huntMapClearAll can cancel a test in flight
static uint8_t hgConfirmCnt = 0;
static float hgQualRpm0 = 0.0f, hgA0Sum = 0.0f, hgQualRpmSum = 0.0f;
static float hgEpA0 = 0.0f, hgEpRefRpm = 0.0f;
static uint8_t hgEpScans = 0, hgEpCleanScans = 0, hgReadN = 0;
static float hgReadSum = 0.0f;
static uint32_t hgCooldownUntilMs = 0;
static float hgRpmMapEma = 0.0f;
// D-lever episode state (state 4): baseline and read accumulator for the |cvDSlope| verify metric,
// and the qualify-span alternation count that attributes the episode to the D-term
static float hgSlope0 = 0.0f, hgQualSlopeSum = 0.0f, hgReadSlopeSum = 0.0f;
static uint16_t hgQualFlips = 0;
static bool hgQualCvOk = false;  // every accumulated qualify scan met HG_CV_COVER_MIN — a slope-judged episode must not OPEN without it, because hgSlope0 comes from those same scans
static bool hgEpVoltsMode = false;  // qualify duty average was below HuntTrigPct (volts-mode wobble) — EVERY stage of this episode verifies on the slope metric; a sub-noise-floor duty comparison would be a coin flip

// Gain at rpm for one lever: min over that lever's pockets (cut in the core, linear across the wings, 1 outside).
// Continuous in rpm, so there is no edge to flap across.
static float hgMapGainLever(float rpm, uint8_t lever, float cut) {
  if (rpm < HG_RPM_MIN || hgPocketN == 0) return 1.0f;
  float wing = (float)HuntWingPct / 100.0f;
  float g = 1.0f;
  for (uint8_t i = 0; i < hgPocketN; i++) {
    const HgPocket &p = hgPockets[i];
    if (p.lever != lever) continue;
    float wLo = p.lo * (1.0f - wing), wHi = p.hi * (1.0f + wing);
    if (rpm < wLo || rpm > wHi) continue;
    float gi;
    if (rpm < p.lo) gi = 1.0f - (1.0f - cut) * (rpm - wLo) / (p.lo - wLo);
    else if (rpm > p.hi) gi = 1.0f - (1.0f - cut) * (wHi - rpm) / (wHi - p.hi);
    else gi = cut;
    if (gi < g) g = gi;
  }
  return g;
}

float huntMapGain(float rpm) {  // inner-Ki multiplier
  return hgMapGainLever(rpm, 0, (float)HuntCutPct / 100.0f);
}

// CV D-term multiplier. The D cut is 0, not HuntCutPct: the deadband limit cycle parks where its gain reaches
// |L|=1, so a PARTIAL cut only moves it to a LARGER amplitude (measured |L|=1.00 with D, 0.45 without).
float huntMapGainD(float rpm) {
  return hgMapGainLever(rpm, 1, 0.0f);
}

// Fold a verified speed into the map: widen the same-lever pocket that covers it, else add one (evicting the
// least-recently-confirmed at the cap), then merge overlapping same-lever pockets. Depth never changes.
static void huntMapAdd(float rpm, uint8_t lever) {
  if (demoSeal) return;   // demo mode: the pocket map is not learned from the simulated boat
  float wing = (float)HuntWingPct / 100.0f;
  uint32_t now = (uint32_t)time(nullptr);
  int hit = -1;
  for (uint8_t i = 0; i < hgPocketN; i++)
    if (hgPockets[i].lever == lever
        && rpm >= hgPockets[i].lo * (1.0f - wing) && rpm <= hgPockets[i].hi * (1.0f + wing)) { hit = i; break; }
  if (hit >= 0) {
    hgPockets[hit].lo = fminf(hgPockets[hit].lo, rpm);
    hgPockets[hit].hi = fmaxf(hgPockets[hit].hi, rpm);
    hgPockets[hit].epoch = now;
  } else if (hgPocketN < HG_MAX_POCKETS) {
    hgPockets[hgPocketN].lo = rpm;
    hgPockets[hgPocketN].hi = rpm;
    hgPockets[hgPocketN].epoch = now;
    hgPockets[hgPocketN].lever = lever;
    hgPocketN++;
  } else {
    int old = 0;
    for (uint8_t i = 1; i < hgPocketN; i++)
      if (hgPockets[i].epoch < hgPockets[old].epoch) old = i;
    hgPockets[old].lo = rpm;
    hgPockets[old].hi = rpm;
    hgPockets[old].epoch = now;
    hgPockets[old].lever = lever;
  }
  bool merged = true;   // O(n²) with restart — n ≤ 4
  while (merged) {
    merged = false;
    for (uint8_t i = 0; i < hgPocketN && !merged; i++)
      for (uint8_t j = i + 1; j < hgPocketN && !merged; j++)
        if (hgPockets[i].lever == hgPockets[j].lever
            && hgPockets[i].hi * (1.0f + wing) >= hgPockets[j].lo * (1.0f - wing)
            && hgPockets[j].hi * (1.0f + wing) >= hgPockets[i].lo * (1.0f - wing)) {
          hgPockets[i].lo = fminf(hgPockets[i].lo, hgPockets[j].lo);
          hgPockets[i].hi = fmaxf(hgPockets[i].hi, hgPockets[j].hi);
          if (hgPockets[j].epoch > hgPockets[i].epoch) hgPockets[i].epoch = hgPockets[j].epoch;
          hgPockets[j] = hgPockets[hgPocketN - 1];
          hgPocketN--;
          merged = true;
        }
  }
  hgMapDirty = true;
}

// Boot restore. setup() only, after the filesystem is mounted.
void huntMapLoad() {
  fsTakeLock();
  File f = LittleFS.open("/huntpockets.csv", "r");
  if (f) {
    while (f.available() && hgPocketN < HG_MAX_POCKETS) {
      String line = f.readStringUntil('\n');
      float lo, hi;
      unsigned long ep;
      unsigned lever = 0;  // 3-field line (pre-D-lever format) = a Ki pocket
      int got = sscanf(line.c_str(), "%f,%f,%lu,%u", &lo, &hi, &ep, &lever);
      if (got >= 3 && lo >= HG_RPM_MIN && hi >= lo && hi <= 100000.0f && lever <= 1) {
        hgPockets[hgPocketN].lo = lo;
        hgPockets[hgPocketN].hi = hi;
        hgPockets[hgPocketN].epoch = (uint32_t)ep;
        hgPockets[hgPocketN].lever = (uint8_t)lever;
        hgPocketN++;
      }
    }
    f.close();
  }
  fsReleaseLock();
}

// The ONLY code that writes the map to flash. Main loop, same field-off-settled call site as huntLedgerService.
void huntMapService() {
  if (!hgMapDirty || demoSeal) return;
  fsTakeLock();
  File f = LittleFS.open("/huntpockets.csv", "w");
  if (f) {
    for (uint8_t i = 0; i < hgPocketN; i++)
      f.printf("%.0f,%.0f,%lu,%u\n", hgPockets[i].lo, hgPockets[i].hi, (unsigned long)hgPockets[i].epoch, (unsigned)hgPockets[i].lever);
    f.close();
    hgMapDirty = false;
  }
  fsFreeDirty = true;
  fsReleaseLock();
}

// /huntmap JSON; cut/wing ride along so the Diag plot draws exactly the profile the firmware applies.
size_t huntMapJson(char *out, size_t cap) {
  size_t n = (size_t)snprintf(out, cap, "{\"cut\":%d,\"wing\":%d,\"enabled\":%d,\"pockets\":[",
                              (int)HuntCutPct, (int)HuntWingPct, (int)HuntGovEnable);
  for (uint8_t i = 0; i < hgPocketN && n < cap; i++)
    n += (size_t)snprintf(out + n, cap - n, "%s{\"lo\":%.0f,\"hi\":%.0f,\"epoch\":%lu,\"lever\":%u}",
                          i ? "," : "", hgPockets[i].lo, hgPockets[i].hi, (unsigned long)hgPockets[i].epoch, (unsigned)hgPockets[i].lever);
  if (n < cap - 2) n += (size_t)snprintf(out + n, cap - n, "]}");
  if (n >= cap) n = cap - 1;  // snprintf accumulates would-be lengths — never let a truncated build overread the buffer
  return n;
}

// One half of huntMapClearAll, for an import whose file brought the other half on the new engine-speed axis:
// pockets = the learned speed map and any test in flight, otherwise the episode record and its queue.
void huntMapClearPart(bool pockets, const char *why) {
  if (pockets) {
    hgPocketN = 0;
    hgMapDirty = false;
    hgConfirmCnt = 0;
    hgReadN = 0;
    hgCooldownUntilMs = 0;
    g_huntState = 0;
    g_huntFreqHz = 0.0f;
    g_huntKdScale = 1.0f;
    if (g_huntDerate < 1.0f) {
      g_huntDerate = 1.0f;
      recomputeCcGains();
    }
  } else {
    hgQueueN = 0;
    hgQueueDropped = 0;
  }
  fsTakeLock();
  LittleFS.remove(pockets ? "/huntpockets.csv" : "/huntledger.csv");
  fsFreeDirty = true;
  fsReleaseLock();
  queueConsoleMessageF("Oscillation damper: %s cleared (%s)", pockets ? "learned speed map" : "episode record", why);
}

// Wipes everything the damper learned, including any test in flight. Flash-safe callers only (HTTP task / Core 1),
// never the control path.
void huntMapClearAll(const char *why) {
  hgPocketN = 0;
  hgMapDirty = false;
  hgConfirmCnt = 0;
  hgReadN = 0;
  hgCooldownUntilMs = 0;
  g_huntState = 0;
  g_huntFreqHz = 0.0f;
  hgQueueN = 0;
  hgQueueDropped = 0;
  g_huntKdScale = 1.0f;  // cancels a D test in flight and un-pockets the D lever
  if (g_huntDerate < 1.0f) {
    g_huntDerate = 1.0f;
    recomputeCcGains();
  }
  fsTakeLock();
  LittleFS.remove("/huntpockets.csv");
  LittleFS.remove("/huntledger.csv");
  fsFreeDirty = true;
  fsReleaseLock();
  queueConsoleMessageF("Oscillation damper: learned speed map and episode record cleared (%s)", why);
}

// Restore BOTH levers BEFORE queuing so derateExit is the true exit gain. A D-rung abort carries the V/s baseline and
// its own verdict token: the plain "aborted" shape is a duty swing the web list renders as a gain cut. aEnd 0.0 = no post
// reading, never a measured zero.
static void hgEpisodeAbort() {
  bool dRung = (g_huntState == 4);
  g_huntDerate = huntMapGain(hgRpmMapEma);
  recomputeCcGains();
  g_huntKdScale = huntMapGainD(hgRpmMapEma);
  hgLedgerQueue(dRung ? "aborted-dterm" : "aborted", g_huntFreqHz, dRung ? hgSlope0 : hgEpA0, 0.0f, 1);
  g_huntState = 0;
}

// state 4 pins the CV D-term (kiCut ignored); state 1 pins inner Ki. A failed D test chaining into Ki passes its own refRpm
// back so one steadiness reference spans both rungs. hgEpVoltsMode / hgQualCvOk describe the qualify span and must stay
// untouched here.
static void hgEpisodeOpen(uint8_t state, float a0, float slope0, float refRpm, float kiCut) {
  hgEpA0 = a0;
  hgSlope0 = slope0;
  hgEpRefRpm = refRpm;
  hgEpScans = 0;
  hgEpCleanScans = 0;
  hgReadSum = 0.0f;
  hgReadSlopeSum = 0.0f;
  hgReadN = 0;
  if (state == 4) {
    g_huntKdScale = 0.0f;  // Ki untouched — it keeps following its map during the D stage
  } else {
    g_huntDerate = kiCut;
    recomputeCcGains();
  }
  g_huntState = state;
}

// Called every loop() pass; self-gates to one Goertzel pass (6 bins × 128 samples, tens of µs) per HG_EVAL_EVERY ticks.
// Flow: HuntQualifyScans qualify → one cut → HG_BLIND_SCANS refill → HG_READ_SCANS averaged → verdict (pocket, or
// revert + cooldown); outside a test gain follows the pocket map.
void runHuntGovernor() {
  static uint32_t lastEvalTick = 0;
  static float hann[HG_RING_N];
  static bool hannInit = false;

  if (!HuntGovEnable) {
    if (g_huntDerate < 1.0f) {
      g_huntDerate = 1.0f;
      recomputeCcGains();
    }
    g_huntKdScale = 1.0f;  // damper off = full gain on BOTH levers; the map is kept but inert
    g_huntState = 0;
    hgConfirmCnt = 0;
    g_huntFreqHz = 0.0f;
    hgRpmSum = 0.0f;  // keep draining — the observer accumulates regardless, and a re-enable must not read a stale hours-long sum
    hgRpmCnt = 0;
    hgSlopeSum = 0.0f;
    hgSlopeCnt = 0;
    hgKdFlips = 0;
    return;
  }
  if (hgTick - lastEvalTick < HG_EVAL_EVERY) return;
  lastEvalTick = hgTick;

  if (!hannInit) {
    for (int i = 0; i < HG_RING_N; i++) hann[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * (float)i / (float)(HG_RING_N - 1));
    hannInit = true;
  }

  // Scan-mean engine speed — the working RPM for steadiness and pocket lookup
  uint16_t scanTicks = hgRpmCnt;  // ticks this scan — read BEFORE the drains below
  float scanRpm = hgRpmCnt ? (hgRpmSum / (float)hgRpmCnt) : 0.0f;
  hgRpmSum = 0.0f;
  hgRpmCnt = 0;
  uint16_t scanCvTicks = hgSlopeCnt;
  float scanSlope = hgSlopeCnt ? (hgSlopeSum / (float)hgSlopeCnt) : 0.0f;  // scan-mean |cvDSlope| — D-test verify metric
  hgSlopeSum = 0.0f;
  hgSlopeCnt = 0;
  // No verdict may ever be read off a scan set with no CV evidence: leaving voltage control stops
  // the slope accumulator, so an uncovered scan reads 0.0 and beats any baseline by 100%.
  bool scanCvOk = scanTicks && ((float)scanCvTicks >= HG_CV_COVER_MIN * (float)scanTicks);
  uint16_t scanFlips = hgKdFlips;  // kdTrim sign alternations this scan — D-lever attribution
  hgKdFlips = 0;
  // τ ≈ 2 scans for the map lookup — fast pocket entry; a long EMA would lag the reduction in by tens of seconds
  hgRpmMapEma = (hgRpmMapEma <= 0.0f) ? scanRpm : hgRpmMapEma + 0.5f * (scanRpm - hgRpmMapEma);

  float cut = (float)HuntCutPct / 100.0f;

  // Gain follows the map whenever no test holds it pinned at the cut — per lever: Ki keeps
  // following its map during a D test (state 4), and vice versa
  if (g_huntState != 1) {
    float mg = huntMapGain(hgRpmMapEma);
    if (fabsf(mg - g_huntDerate) >= 0.01f) {
      g_huntDerate = mg;
      recomputeCcGains();
    }
  }
  if (g_huntState != 4) {
    float ds = huntMapGainD(hgRpmMapEma);
    if (fabsf(ds - g_huntKdScale) >= 0.01f) g_huntKdScale = ds;  // applied per-tick at the kdTrim choke point — no recompute needed
  }

  if (g_huntState == 3) {
    if ((int32_t)(millis() - hgCooldownUntilMs) < 0) return;
    g_huntState = 0;
  }

  bool windowClean = (hgTick >= HG_RING_N) && ((hgTick - hgContamTick) >= HG_RING_N);
  float aPk = 0.0f, aMed = 0.0f;
  int pk = 0;
  bool dirtyScan = !windowClean;
  if (windowClean) {
    float fs = 1000.0f / fmaxf(hgDtEmaMs, 1.0f);
    float s1[HG_NBINS] = { 0 }, s2[HG_NBINS] = { 0 }, coef[HG_NBINS];
    bool binOk[HG_NBINS];
    for (int b = 0; b < HG_NBINS; b++) {
      binOk[b] = HG_BIN_HZ[b] < 0.4f * fs;
      coef[b] = 2.0f * cosf(2.0f * (float)M_PI * HG_BIN_HZ[b] / fs);
    }
    float mean = 0.0f;
    for (int i = 0; i < HG_RING_N; i++) mean += hgRing[i];
    mean /= (float)HG_RING_N;
    float mn = 1e9f, mx = -1e9f;
    for (int i = 0; i < HG_RING_N; i++) {
      float raw = hgRing[(hgHead + i) % HG_RING_N];
      if (raw < mn) mn = raw;
      if (raw > mx) mx = raw;
      float v = (raw - mean) * hann[i];
      for (int b = 0; b < HG_NBINS; b++) {
        float s0 = v + coef[b] * s1[b] - s2[b];
        s2[b] = s1[b];
        s1[b] = s0;
      }
    }
    if (mx - mn > 12.0f) {  // step/transient the contamination flags didn't catch
      dirtyScan = true;
    } else {
      float amps[HG_NBINS];
      for (int b = 0; b < HG_NBINS; b++) {
        float p = s1[b] * s1[b] + s2[b] * s2[b] - coef[b] * s1[b] * s2[b];
        amps[b] = binOk[b] ? 2.0f * sqrtf(fmaxf(p, 0.0f)) / (0.5f * (float)HG_RING_N) : 0.0f;  // Hann coherent gain 0.5
        if (amps[b] > amps[pk]) pk = b;
      }
      float others[HG_NBINS - 1];
      int n = 0;
      for (int b = 0; b < HG_NBINS; b++)
        if (b != pk) others[n++] = amps[b];
      for (int i = 1; i < n; i++) {
        float k = others[i];
        int j = i - 1;
        while (j >= 0 && others[j] > k) {
          others[j + 1] = others[j];
          j--;
        }
        others[j + 1] = k;
      }
      aMed = others[n / 2];
      aPk = amps[pk];
    }
  }
  if (dirtyScan) {
    hgConfirmCnt = 0;
    if ((g_huntState == 1 || g_huntState == 4) && ++hgEpScans > HG_EP_TIMEOUT_SCANS) {  // contamination froze the test — bail, keep nothing
      hgEpisodeAbort();
    }
    return;
  }

  if (g_huntState == 1 || g_huntState == 4) {
    hgEpScans++;
    bool steady = (scanRpm >= HG_RPM_MIN) && (fabsf(scanRpm - hgEpRefRpm) <= hgEpRefRpm * (float)HuntSteadyPct / 100.0f);
    if (!steady || hgEpScans > HG_EP_TIMEOUT_SCANS) {
      // Couldn't verify on steady data — revert BOTH levers, keep nothing (transients never leave residue)
      hgEpisodeAbort();
      return;
    }
    // Same treatment for a voltage-control dropout on a rung judged by the movement metric: the
    // measurement's own regime is gone, so there is nothing to compare and nothing may be kept.
    bool slopeJudged = (g_huntState == 4) || hgEpVoltsMode;
    if (slopeJudged && !scanCvOk) {
      queueConsoleMessageF("Oscillation damper: %.2f Hz wobble test abandoned — voltage regulation (CV) dropped out mid-test, so the before/after movement reading would prove nothing", g_huntFreqHz);
      hgEpisodeAbort();
      return;
    }
    hgEpCleanScans++;
    if (hgEpCleanScans <= HG_BLIND_SCANS) return;  // window still refilling with post-cut data
    hgReadSum += aPk;
    hgReadSlopeSum += scanSlope;
    hgReadN++;
    if (hgReadN < HG_READ_SCANS) return;
    float aPost = hgReadSum / (float)HG_READ_SCANS;

    if (g_huntState == 4) {
      // D-lever verdict — judged on the |cvDSlope| metric, not duty: the D-driven mode barely
      // registers in duty, and physics predicts collapse, not a marginal drop (|L| 1.0 -> 0.45)
      float slopePost = hgReadSlopeSum / (float)HG_READ_SCANS;
      bool dPassed = slopePost <= hgSlope0 * (1.0f - (float)HuntVerifyPct / 100.0f);
      if (dPassed) huntMapAdd(hgEpRefRpm, 1);
      g_huntKdScale = huntMapGainD(hgRpmMapEma);  // pass: the new pocket now holds the cut (0 here); fail: back to the map (usually 1)
      if (dPassed) {
        hgLedgerQueue("damped-dterm", g_huntFreqHz, hgSlope0, slopePost, 1);
        queueConsoleMessageF("Oscillation damper: %.2f Hz wobble stopped when the voltage damper (D-term) was paused (%.2f -> %.2f V/s movement) — D-term mapped off at %d rpm", g_huntFreqHz, hgSlope0, slopePost, (int)hgEpRefRpm);
        g_huntState = 0;
      } else {
        hgLedgerQueue("dterm-no-resp", g_huntFreqHz, hgSlope0, slopePost, 1);
        if (g_huntDerate > cut + 0.02f) {
          // Chain to the Ki rung: the D-stage read scans are themselves a clean 3-scan average of
          // the still-present wobble, so they become the Ki test's baselines — no extra wait
          hgEpisodeOpen(1, aPost, slopePost, hgEpRefRpm, cut);  // refRpm fed back in: same episode, same speed reference
          queueConsoleMessageF("Oscillation damper: pausing the voltage damper did not stop the %.2f Hz wobble — testing at %d%% current-loop gain", g_huntFreqHz, (int)HuntCutPct);
        } else {
          // Both levers exhausted at this speed — only now may the blame leave the regulator
          queueConsoleMessageF("Oscillation damper: the %.2f Hz wobble responded to neither damper lever — external cyclic load or engine issue suspected", g_huntFreqHz);
          hgCooldownUntilMs = millis() + (uint32_t)HuntCooldownMin * 60000UL;
          g_huntState = 3;
        }
      }
      return;
    }

    // Volts-mode episodes (qualify duty below the standard bar) judge Ki on the slope metric too:
    // a duty comparison under the quiet-log floor would pass or fail on luck
    bool passed = hgEpVoltsMode
                    ? (hgReadSlopeSum / (float)HG_READ_SCANS) <= hgSlope0 * (1.0f - (float)HuntVerifyPct / 100.0f)
                    : aPost <= hgEpA0 * (1.0f - (float)HuntVerifyPct / 100.0f);
    if (passed) huntMapAdd(hgEpRefRpm, 0);
    // Restore BEFORE queuing so the record's derateExit is the true exit gain: verified — the new
    // pocket now holds the cut; failed — back to the map (usually full gain)
    g_huntDerate = huntMapGain(hgRpmMapEma);
    recomputeCcGains();
    if (passed) {
      hgLedgerQueue("damped", g_huntFreqHz, hgEpA0, aPost, 1);
      queueConsoleMessageF("Oscillation damper: %.2f Hz wobble responded (%.1f%% -> %.1f%% swing) — %d%% current-loop gain mapped at %d rpm", g_huntFreqHz, hgEpA0, aPost, (int)HuntCutPct, (int)hgEpRefRpm);
      g_huntState = 0;
    } else {
      hgLedgerQueue("external-suspected", g_huntFreqHz, hgEpA0, aPost, 1);
      queueConsoleMessageF("Oscillation damper: the %.2f Hz wobble did not respond to the gain cut — full gain restored (external cyclic load or engine issue suspected)", g_huntFreqHz);
      hgCooldownUntilMs = millis() + (uint32_t)HuntCooldownMin * 60000UL;
      g_huntState = 3;
    }
    return;
  }

  // Qualification: consecutive hunt scans at steady speed open a test (reference = first scan's mean). A scan qualifies on
  // the duty signature OR the D-attributed form: with kdTrim sign-alternating the bar drops to HuntTrigPct x HG_TRIG_D_FRAC
  // and peakiness to 2x; broadband load noise never alternates the trim.
  bool kiHunt = (aPk > HuntTrigPct) && (aPk > HG_TRIG_RATIO * aMed);
  bool dHunt = (scanFlips >= 1) && (g_huntKdScale > 0.05f)
               && (aPk > HuntTrigPct * HG_TRIG_D_FRAC) && (aPk > 2.0f * aMed);
  if ((kiHunt || dHunt) && scanRpm >= HG_RPM_MIN
      && (hgConfirmCnt == 0 || fabsf(scanRpm - hgQualRpm0) <= hgQualRpm0 * (float)HuntSteadyPct / 100.0f)) {
    if (hgConfirmCnt == 0) {
      hgQualRpm0 = scanRpm;
      hgA0Sum = 0.0f;
      hgQualRpmSum = 0.0f;
      hgQualSlopeSum = 0.0f;
      hgQualFlips = 0;
      hgQualCvOk = true;
    }
    hgConfirmCnt++;
    hgA0Sum += aPk;
    hgQualRpmSum += scanRpm;
    hgQualSlopeSum += scanSlope;
    if (!scanCvOk) hgQualCvOk = false;
    hgQualFlips = (uint16_t)fminf(65535.0f, (float)hgQualFlips + (float)scanFlips);
    if (hgConfirmCnt >= HuntQualifyScans) {
      float nq = (float)hgConfirmCnt;  // divisor = scans actually accumulated, so a live setting change mid-qualify can't skew the averages
      hgConfirmCnt = 0;
      float a0 = hgA0Sum / nq;
      float refRpm = hgQualRpmSum / nq;
      g_huntFreqHz = HG_BIN_HZ[pk];
      // Lever choice: the qualify span must show ~half the sign alternations the detected frequency would produce (2 per
      // period; floor 3). An attributed episode tests D FIRST: pausing the participant is directly causal, and the Ki lever
      // failed on every outer-loop-driven variant. Inside a D-pocket (scale ~0) there is no trim to attribute.
      uint16_t flipBar = (uint16_t)fmaxf(3.0f, HG_BIN_HZ[pk] * nq * 1.6f);
      hgEpVoltsMode = (a0 < HuntTrigPct);
      bool dAttributed = (hgQualFlips >= flipBar) && (g_huntKdScale > 0.05f);
      if (!hgQualCvOk && (dAttributed || hgEpVoltsMode)) {
        // Both slope-judged rungs verify against hgSlope0 (these scans' average), junk without CV coverage: refuse to open.
        // No cooldown, so a later pass with real CV coverage still gets tested.
        static uint32_t hgNoCvNoteMs = 0;
        if (hgNoCvNoteMs == 0 || (int32_t)(millis() - hgNoCvNoteMs) >= 0) {
          queueConsoleMessageF("Oscillation damper: %.2f Hz wobble at %d rpm not tested — the regulator was not holding voltage (voltage-regulated mode, CV) enough of the time to measure the voltage movement this test is judged on", g_huntFreqHz, (int)refRpm);
          hgNoCvNoteMs = millis() + (uint32_t)HuntCooldownMin * 60000UL;
        }
        return;
      }
      if (dAttributed) {
        hgEpisodeOpen(4, a0, hgQualSlopeSum / nq, refRpm, cut);
        queueConsoleMessageF("Oscillation damper: %.2f Hz wobble at %d rpm with the voltage damper (D-term) cycling — testing with it paused", g_huntFreqHz, (int)refRpm);
        return;
      }
      if (g_huntDerate <= cut + 0.02f) {
        // At the Ki floor inside a pocket with no D attribution — nothing left to test. The pocket
        // stands on a verified 2x-class response, so a wobble persisting here points at a physical cause.
        hgLedgerQueue("external-suspected", g_huntFreqHz, a0, a0, 0);
        queueConsoleMessageF("Oscillation damper: %.2f Hz wobble persists at %d%% gain inside a learned pocket — the cause is likely mechanical (belt tension, mounts, engine governor)", g_huntFreqHz, (int)roundf(g_huntDerate * 100.0f));
        hgCooldownUntilMs = millis() + (uint32_t)HuntCooldownMin * 60000UL;
        g_huntState = 3;
        return;
      }
      hgEpisodeOpen(1, a0, hgQualSlopeSum / nq, refRpm, cut);
      queueConsoleMessageF("Oscillation damper: %.2f Hz wobble at %d rpm (%.1f%% field swing) — testing at %d%% current-loop gain", g_huntFreqHz, (int)refRpm, a0, (int)HuntCutPct);
    }
  } else {
    hgConfirmCnt = 0;
  }
}

// Max Field Volts solved into duty by updateFieldVoltCeiling(). A global so ccDutyCeiling() stays a cheap read and every
// reader sees the value the PID's output limits were last pushed (on a deadband).
static float g_fieldVoltDutyCeil = 99.0f;
static float g_fieldVoltBusFilt  = 0.0f;   // slow-filtered bus volts the ceiling is solved against; 0 = not yet seeded
static bool  g_fieldVoltFloored  = false;  // one-shot console warn latch for the MinDuty collision below

// FIELD-DERATE-LAW-BEGIN
// Field-drive heat law and 10 A ceiling (spec: private_refs/v9_rework/d5_thermal_model/README.md, "FIELD DERATE - SPEC").
// Plain C on purpose: the host test compiles this exact text against the explainer page's law. Constants are V9-rework
// geometry fitted on soaked bench rows (J2 minus INA228 die on N wiring, D5 case top minus die on P); V10: re-measure.
// Heat law and 10 A ceiling both use Rc (fdRcOhm, same 90 % credit); FD_MARGIN on Rc is the only added margin.
#define FD_VF_EST      0.4f      // freewheel drop in the field-current estimate, V
#define FD_PIN_K       17.8f     // J2 pin heating at 10 A, K
#define FD_DIODE_KW    10.0f     // J2 heating per watt in the SB5H100 soldered into its pins, K/W
#define FD_D5_KW       21.2f     // D5 case top per watt in D5, K/W
#define FD_CONN_LIM_C  105.0f    // J2 housing (PA66) rating
#define FD_D5_LIM_C    120.0f    // Wurth film adhesive rating on D5's top
#define FD_ALPHA       0.00393f  // copper resistance per degree C
#define FD_CREDIT      0.9f      // share of the probe rise credited to the rotor; a measured rotor rose 0.56-0.95 x the probe 3-11 min into a cold start, 1.15-1.21 x from 16 min
#define FD_TCAP_C      120.0f    // credited probe temperature ceiling: bounds the credit a falsely high probe can claim
#define FD_MARGIN      0.12f     // Rc / (1 + this): a parked warm-up from cold at ~100 A output read up to 9.8 % over the credited estimate (clamp, +-2.8 %)
#define FD_TREF_DEF_C  30.0f     // Tref when none was recorded with the resistance
#define FD_FIELD_MAX_A 10.0f     // board field-current rating
#define FD_WIRING_P    1
#define FD_WIRING_N    2
#define FD_PART_NONE   0
#define FD_PART_CONN   1
#define FD_PART_D5     2

static float fdIEst(float D, float V, float R) {
  float i = (D * V - (1.0f - D) * FD_VF_EST) / R;
  return i > 0.0f ? i : 0.0f;
}
static float fdGapConn(float D, float V, float R) {
  float I = fdIEst(D, V, R);
  return FD_PIN_K * (I / 10.0f) * (I / 10.0f) + FD_DIODE_KW * I * (1.0f - D) * (0.48f + 0.0158f * I);
}
static float fdGapD5(float D, float V, float R) {
  float I = fdIEst(D, V, R);
  return FD_D5_KW * ((0.405f + 0.0175f * (I - 8.0f)) * I * (1.0f - D) + 0.013f * V * D);
}
// The heat checks at grid point k (duty k / 10 %): FD_PART_NONE = pass, else the part that failed. P wiring runs the D5
// check only, N wiring the connector check only, not set (0) both.
#define FD_SCAN_LAST_K 990
static int fdHeatCheckAt(int k, float T_C, float V, float R, int wiring) {
  float D = (float)k / 1000.0f;
  if (wiring != FD_WIRING_P && T_C + fdGapConn(D, V, R) > FD_CONN_LIM_C) return FD_PART_CONN;
  if (wiring != FD_WIRING_N && T_C + fdGapD5(D, V, R) > FD_D5_LIM_C) return FD_PART_D5;
  return FD_PART_NONE;
}
// Duty (percent) the scan reports when grid point k is the first to fail.
static float fdHeatPctBefore(int k) {
  return (k > 0) ? (float)(k - 1) / 10.0f : 0.0f;
}
// Largest duty (percent, 0.1 grid, 0..99.0) whose checks pass at EVERY duty from 0 up to it. The connector estimate is
// not monotonic in duty (pin heat rises as diode heat falls), so the first failure ends the scan. *part = the check that
// failed first. The firmware runs the same scan in slices (updateFieldDerateCeilings); this whole form is the host test's.
__attribute__((unused)) static float fdHeatCeilPct(float T_C, float V, float R, int wiring, int *part) {
  *part = FD_PART_NONE;
  for (int k = 0; k <= FD_SCAN_LAST_K; k++) {
    int p = fdHeatCheckAt(k, T_C, V, R, wiring);
    if (p != FD_PART_NONE) { *part = p; return fdHeatPctBefore(k); }
  }
  return (float)FD_SCAN_LAST_K / 10.0f;
}
// Resistance the 10 A ceiling, the heat law and the Field Amps readout use. A probe below Tref gets the full correction (a cold
// rotor draws MORE than the estimate); above it, 90 % credit up to 120 C. No probe reading (NaN): no credit. Every branch is
// then divided by 1 + FD_MARGIN: the rotor warms behind the probe, and part of the gap showed before any credit applied.
static float fdRcOhm(float R, float Tp_C, float Tref_C) {
  float rc;
  if (Tp_C != Tp_C) rc = R;
  else if (Tp_C < Tref_C) rc = R * (1.0f + FD_ALPHA * (Tp_C - Tref_C));
  else rc = R * (1.0f + FD_ALPHA * FD_CREDIT * (((Tp_C < FD_TCAP_C) ? Tp_C : FD_TCAP_C) - Tref_C));
  return rc / (1.0f + FD_MARGIN);
}
// Duty (percent) where (D V - (1 - D) 0.4) / Rc = 10 A.
static float fdCurCeilPct(float Rc, float V) {
  return 100.0f * (FD_FIELD_MAX_A * Rc + FD_VF_EST) / (V + FD_VF_EST);
}
// FIELD-DERATE-LAW-END

static float   g_fieldHeatRawPct = 99.0f;   // last finished heat scan; 99 until the first valid die sample
static uint8_t g_fieldHeatPart = FD_PART_NONE;
static bool    g_fieldCurBelowMin = false;  // one-shot console latches, same pattern as g_fieldVoltFloored
static bool    g_fieldHeatBelowMin = false;
static bool    g_fieldResMissingSaid = false;

// Heat scan in slices: FD_SCAN_SLICE grid points per control pass, inputs latched at the start, so a rescan never costs
// one pass more than a slice (the control loop's added-work budget is 0.5 ms). A full scan spans <= 10 passes (~50 ms).
#define FD_SCAN_SLICE     100
#define FD_DIE_FALL_KPS   2.0f   // fastest fall the heat law accepts from the die reading, K/s: a bad low read (0 C) cannot open the ceiling
static bool  s_fdScanActive = false;
static int   s_fdScanK = 0, s_fdScanWiring = 0;
static float s_fdScanT = 0.0f, s_fdScanV = 0.0f, s_fdScanR = 1.0f;

// Bus volts the ceilings solve against when there is no trusted main reading (limp tier 3/4, stale main voltage, before the
// first reading): the INA228 hardware limit or 16 V per 12 V of class, whichever is higher. Higher volts = lower duty.
float fieldCeilAssumedBusV() {
  return fmaxf(VoltageHardwareLimit, 16.0f * (float)SYSTEM_VOLTAGE_CLASS / 12.0f);
}

// The two board ceilings alone: they win over every duty floor (Min Field, the per-RPM onset floors).
float boardDutyCeiling() {
  return fminf(g_fieldCurDutyCeil, g_fieldHeatDutyCeil);
}

// Upper duty bound for the CC loop: lowest of the four field caps. MaxDuty is the real per-bus cap (99 on every class);
// g_fieldVoltDutyCeil (Max Field Volts on the measured bus) carries the per-class protection; the two board ceilings
// protect the regulator. Anything asking "how high may duty go" must come through here: a raw MaxDuty read skips the volts cap.
float ccDutyCeiling() {
  return fminf(fminf(MaxDuty, g_fieldVoltDutyCeil), fminf(g_fieldCurDutyCeil, g_fieldHeatDutyCeil));
}

// The alternator's own ceiling, without the board derates: alt-health's high-field test reads this so a hot
// regulator never raises an alternator alert.
float altFieldCeiling() {
  return fminf(MaxDuty, g_fieldVoltDutyCeil);
}

// Once per control tick from buildTickSnapshot, so it leads every consumer. The bus is 2 s filtered so the cap doesn't chase
// ripple or load dump, and SetOutputLimits (re-clamps output AND integrator) is pushed only past a 0.5-point deadband,
// else the ceiling becomes a moving actuator.
void updateFieldVoltCeiling(float busV, float dtSec) {
  if (!(busV > 1.0f) || isnan(busV)) return;           // sensor dropout: hold the last ceiling rather than open the cap
  if (g_fieldVoltBusFilt <= 0.0f) g_fieldVoltBusFilt = busV;   // seed on first valid reading, no ramp-in
  else {
    float a = constrain(dtSec / 2.0f, 0.0f, 1.0f);
    g_fieldVoltBusFilt += a * (busV - g_fieldVoltBusFilt);
  }

  float derived = 100.0f * MaxFieldVolts / g_fieldVoltBusFilt;

  // Floor at MinDuty: below it [MinDuty, ceiling] inverts and the per-RPM onset floors become unsatisfiable (cf.
  // applyNominalVoltageChange). A volts cap that low is a misconfiguration, so warn once.
  if (derived < MinDuty) {
    derived = MinDuty;
    if (!g_fieldVoltFloored) {
      g_fieldVoltFloored = true;
      queueConsoleMessageF("Max Field Volts (%.1fV) is below the field floor at this bus voltage (%.1fV) — field ceiling pinned to Min Field %.2f%%. Raise Max Field Volts.",
                           MaxFieldVolts, g_fieldVoltBusFilt, MinDuty);
    }
  } else {
    g_fieldVoltFloored = false;
  }
  if (derived > 99.0f) derived = 99.0f;   // matches the bootstrap-refresh cap in setDutyPercent

  if (fabsf(derived - g_fieldVoltDutyCeil) > 0.5f) {
    g_fieldVoltDutyCeil = derived;
    if (pidInitialized) applyCcOutputLimits();   // PID entry calls applyCcOutputLimits itself, so a pre-init move is not lost
  }
}

// Immediate re-solve for the settings handlers, on the same bus the control tick would choose (busUnmeasured there):
// a held or ignored main voltage must not seed the filter in a degraded mode.
void updateFieldVoltCeilingNow() {
  updateFieldVoltCeiling((g_limpTier >= 3 || g_ibvStale) ? fieldCeilAssumedBusV() : getBatteryVoltage(), 999.0f);
}

// The two board ceilings, once per control tick after updateFieldVoltCeiling(), on the same bus. Current ceiling: closed form
// each pass. Heat scan restarts (max 1/s) when the die moves 0.25 K, bus 0.1 V, Rc 0.1 % or a setting changes; a falling
// ceiling applies at once, a rising one waits for the 0.5-point deadband. An invalid die reading holds the last heat ceiling
// (REASON_DIE_TEMP_LOST ends that after 10 s). No resistance and no 12 V skipped-test default: current ceiling 0 (field off).
void updateFieldDerateCeilings(float dtSec, bool busUnmeasured) {
  const bool dirty = g_fieldDerateDirty;   // cleared only when a scan starts, so a change made while the die is invalid still lands

  const bool rMeasured = isfinite(FieldResistance) && FieldResistance > 0.0f;
  const bool rDefault = !rMeasured && fieldResDefaultInUse();
  const bool rKnown = rMeasured || rDefault;
  const float tpF = n2kAltTempF();
  const float tpC = isfinite(tpF) ? (tpF - 32.0f) / 1.8f : NAN;
  const float trefC = isfinite(FieldResTrefC) ? FieldResTrefC : FD_TREF_DEF_C;
  g_fieldRcOhm = rMeasured ? fdRcOhm(fmaxf(FieldResistance, 0.05f), tpC, trefC) : rDefault ? FD_SKIP_R_OHM : NAN;
  const float V = (busUnmeasured || g_fieldVoltBusFilt <= 0.0f) ? fmaxf(fieldCeilAssumedBusV(), g_fieldVoltBusFilt)
                                                                 : g_fieldVoltBusFilt;
  float cur = rKnown ? fdCurCeilPct(g_fieldRcOhm, V) : 0.0f;
  if (!rKnown && !g_fieldResMissingSaid) {
    g_fieldResMissingSaid = true;
    queueConsoleMessage("Field Resistance has not been measured: the 10 A field ceiling cannot be calculated, so the field stays off until it is (the Field Resistance test after the commissioning Prerequisites screen, or Setup > Alternator > Basic > Output Control > Measure; a 12 V system with no battery shunt can skip the test there)");
  } else if (rKnown) {
    g_fieldResMissingSaid = false;
  }

  // Die input to the heat law: rises pass at once, falls at most FD_DIE_FALL_KPS.
  static float dieC = NAN;
  const uint32_t now = millis();
  const uint32_t dieTs = dataTimestamps[IDX_INA_DIE_TEMP];
  const bool dieOk = (dieTs != 0) && ((int32_t)(now - dieTs) <= 10000) && isfinite(g_fdDieTempF);
  if (dieOk) {
    const float tC = (g_fdDieTempF - 32.0f) / 1.8f;
    dieC = (!isfinite(dieC) || tC >= dieC) ? tC : fmaxf(tC, dieC - FD_DIE_FALL_KPS * dtSec);
  }
  if (dieOk && rKnown) {
    static float lastT = NAN, lastV = 0.0f, lastRc = 0.0f;
    static uint32_t lastStartMs = 0;
    const bool moved = !isfinite(lastT) || fabsf(dieC - lastT) >= 0.25f || fabsf(V - lastV) >= 0.1f ||
                       fabsf(g_fieldRcOhm - lastRc) >= 0.001f * g_fieldRcOhm;
    if (dirty || (moved && (uint32_t)(now - lastStartMs) >= 1000UL)) {
      g_fieldDerateDirty = false;   // before FieldWiring is latched: a write landing later sets it again for the next pass (a resistance write also moves Rc)
      s_fdScanT = dieC;
      s_fdScanV = V;
      s_fdScanR = g_fieldRcOhm;
      s_fdScanWiring = FieldWiring;
      s_fdScanK = 0;
      s_fdScanActive = true;
      lastT = dieC;
      lastV = V;
      lastRc = g_fieldRcOhm;
      lastStartMs = now;
    }
  }
  if (s_fdScanActive) {
    const int kEnd = min(s_fdScanK + FD_SCAN_SLICE, FD_SCAN_LAST_K + 1);
    for (; s_fdScanK < kEnd; s_fdScanK++) {
      const int p = fdHeatCheckAt(s_fdScanK, s_fdScanT, s_fdScanV, s_fdScanR, s_fdScanWiring);
      if (p != FD_PART_NONE) {
        g_fieldHeatRawPct = fdHeatPctBefore(s_fdScanK);
        g_fieldHeatPart = (uint8_t)p;
        s_fdScanActive = false;
        break;
      }
    }
    if (s_fdScanActive && s_fdScanK > FD_SCAN_LAST_K) {
      g_fieldHeatRawPct = (float)FD_SCAN_LAST_K / 10.0f;
      g_fieldHeatPart = FD_PART_NONE;
      s_fdScanActive = false;
    }
  }

  if (cur > 99.0f) cur = 99.0f;
  if (cur < 0.0f) cur = 0.0f;
  float heat = constrain(g_fieldHeatRawPct, 0.0f, 99.0f);

  if (rKnown && cur < MinDuty) {
    if (!g_fieldCurBelowMin) {
      g_fieldCurBelowMin = true;
      queueConsoleMessageF("Field current ceiling: 10 A at %.2f ohm and %.1f V is %.2f%% duty, below Min Field %.2f%% - the field is held below Min Field. Check Field Resistance.",
                           g_fieldRcOhm, V, cur, MinDuty);
    }
  } else {
    g_fieldCurBelowMin = false;
  }
  if (heat < MinDuty) {
    if (!g_fieldHeatBelowMin) {
      g_fieldHeatBelowMin = true;
      const char *part = (g_fieldHeatPart == FD_PART_D5) ? "freewheel diode's heat spreader" : "field connector";
      if (heat <= 0.0f)
        queueConsoleMessageF("Field drive heat limit: board at %.0f%s, the %s is at its rating with no field at all - field held off until the regulator cools",
                             dispTempF(dieC * 1.8f + 32.0f), dispTempUnit(), part);
      else
        queueConsoleMessageF("Field drive heat limit: board at %.0f%s, the %s allows %.1f%% duty, below Min Field %.2f%% - the field is held below Min Field until the regulator cools",
                             dispTempF(dieC * 1.8f + 32.0f), dispTempUnit(), part, heat, MinDuty);
    }
  } else {
    g_fieldHeatBelowMin = false;
  }

  bool moved = false;
  if (cur < g_fieldCurDutyCeil - 0.01f || cur > g_fieldCurDutyCeil + 0.5f)    { g_fieldCurDutyCeil = cur;   moved = true; }
  if (heat < g_fieldHeatDutyCeil - 0.01f || heat > g_fieldHeatDutyCeil + 0.5f) { g_fieldHeatDutyCeil = heat; moved = true; }
  if (moved && pidInitialized) applyCcOutputLimits();

  // Ties go to the term the owner can act on (strict <, settings first).
  float ceil = MaxDuty;
  uint8_t bind = 0;
  if (g_fieldVoltDutyCeil < ceil) { ceil = g_fieldVoltDutyCeil; bind = 1; }
  if (g_fieldCurDutyCeil < ceil)  { ceil = g_fieldCurDutyCeil;  bind = 2; }
  if (g_fieldHeatDutyCeil < ceil) { ceil = g_fieldHeatDutyCeil; bind = (g_fieldHeatPart == FD_PART_D5) ? 4 : 3; }
  g_fieldCeilBind = bind;

  static uint32_t heatAccMs = 0, curAccMs = 0;
  const bool atCeil = !gpio4IsLow && dutyCycle > 0.0f && dutyCycle >= ceil - 1.0f && frt.state != FRT_RUNNING;   // the test runs past the board ceilings by design
  const uint32_t dms = (uint32_t)(dtSec * 1000.0f + 0.5f);
  if (atCeil && bind >= 3) {
    heatAccMs += dms;
    while (heatAccMs >= 1000) { heatAccMs -= 1000; g_fieldHeatLimS++; g_fieldHeatLimSesS++; }
  }
  if (atCeil && bind == 2) {
    curAccMs += dms;
    while (curAccMs >= 1000) { curAccMs -= 1000; g_fieldCurLimS++; g_fieldCurLimSesS++; }
  }
}

// Inner current PID bounds = [MinDuty, ccDutyCeiling()]: a REAL upper limit lets anti-windup track the ceiling instead of
// winding toward MaxDuty while setDutyPercent() clamps far lower (it stays the catch-all for open-loop paths). A board
// ceiling below MinDuty drags the bottom down with it; SetOutputLimits ignores min >= max, hence the 0.01 gap.
void applyCcOutputLimits() {
  const float hi = ccDutyCeiling();
  float lo = fminf(MinDuty, hi);
  if (lo > hi - 0.01f) lo = hi - 0.01f;
  currentPID.SetOutputLimits((double)lo, (double)hi);
}

// ==================== FIELD RESISTANCE TEST ====================
// BATTERY_WIRING_AND_SHUNT_SPEC.md Addendum B1. Holds are planned in field volts X so one plan suits every bus/winding:
// probe 3 V (6, 12 V while under 0.5 A), aiming hold, top at 4 A, middle at 0.6 x top measured AFTER the top (warming
// tilts R low, the safe side). R and contact drop come from top and middle only; the aiming hold cancels the drop's bias
// in a top planned by V/A. No closing probe repeat: its scatter (+-4 %) swamps the warming (1-2 %). The only path that
// drives the field with no resistance stored.
static const float FRT_PROBE_X[3] = { 3.0f, 6.0f, 12.0f };
static const char *const FRT_HOLD_NAME[FRT_N_HOLDS] = { "low hold", "aiming hold", "top hold", "middle hold" };

static float frtCeilPct() {
  return fminf(fminf(MaxDuty, g_fieldVoltDutyCeil), 99.0f);   // the alternator ceilings: they need no resistance
}
static float frtXAt(float dPct, float v) {
  const float d = dPct / 100.0f;
  return d * v - (1.0f - d) * FD_VF_EST;
}
static float frtDutyFor(float x, float v) {
  if (!(v > 1.0f)) return 0.0f;
  return constrain(100.0f * (x + FD_VF_EST) / (v + FD_VF_EST), 0.0f, frtCeilPct());
}

void frtFieldOff() {
  digitalWrite(4, LOW);
  gpio4IsLow = true;
  apply_pwm_float(0.0f);
  lastAppliedDuty = 0.0f;
  dutyCycle = 0.0f;
  fieldActiveStatus = 0;
  frt.duty = 0.0f;
}

// Every hold measured so far, for the console: the result's working on success, what was in hand on a failure.
static void frtLogHolds() {
  for (int k = 0; k < FRT_N_HOLDS; k++) {
    const FrtHold &hd = frt.h[k];
    if (!isfinite(hd.i)) continue;
    queueConsoleMessageF("  %s: duty %.2f%%, %.2f V field, %.3f A, battery %.2f V", FRT_HOLD_NAME[k], hd.d, hd.x, hd.i, hd.v);
  }
}

void frtFail(const char *why) {
  if (frt.state != FRT_RUNNING) return;
  strncpy(frt.msg, why, sizeof(frt.msg) - 1);
  frt.msg[sizeof(frt.msg) - 1] = '\0';
  frt.state = FRT_FAILED;
  frt.endMs = millis();
  frtFieldOff();
  queueConsoleMessageF("Field resistance test: %s", frt.msg);
  frtLogHolds();
}

static void frtLiveRangeReset() {
  frt.lvLo = frt.fsLo = INFINITY;
  frt.lvHi = frt.fsHi = -INFINITY;
}

static void frtResetWindow(uint32_t now) {
  frt.sumI = frt.sumV = 0.0;
  frt.n = 0;
  frt.ringN = frt.ringK = 0;
  frt.ringSum = 0.0;
  frt.phaseStartMs = now;
}

// Starts hold frt.hold at field volts x. carryA (shunt): the field-off phase just measured opens this hold too.
static void frtStartHold(uint32_t now, float x, bool carryA) {
  frt.holdX = x;
  frt.holdDuty = frtDutyFor(x, IBV);
  frt.h[frt.hold].nc = 0;
  frtLiveRangeReset();
  if (frt.method == FRT_SHUNT) {
    if (carryA) {
      frt.phI[0] = frt.phI[frt.phase - 1];   // the field-off phase just closed: 2 after a probe's first cycle, else 6
      frt.phV[0] = frt.phV[frt.phase - 1];
      frt.phase = 1;
    } else {
      frt.phase = 0;
    }
  } else {
    frt.phase = FRT_MP_HOLD;
  }
  frtResetWindow(now);
}

void frtBegin(uint32_t now) {
  const uint8_t method = frt.reqMethod;
  const uint32_t endMs = frt.endMs;
  frt = FieldResTestState();
  frt.endMs = endMs;
  frt.method = method;
  frt.hold = FRT_H_PROBE;
  frt.startMs = now;
  frt.lastPollMs = now;
  frt.probeC = fieldResProbeC();
  frt.baseA = frt.gateA = NAN;
  frt.r = frt.vc = frt.probeOffV = NAN;
  for (int k = 0; k < FRT_N_HOLDS; k++) frt.h[k] = { NAN, NAN, NAN, NAN, NAN };
  for (int k = 0; k < 2; k++) frt.rungX[k] = frt.rungI[k] = NAN;
  frtStartHold(now, FRT_PROBE_X[0], false);
  frt.state = FRT_RUNNING;
  queueConsoleMessageF("Field resistance test started (%s method), engine off, battery %.2f V",
                       method == FRT_SHUNT ? "shunt" : "meter", IBV);
}

// Shunt: cycle k of the hold in progress has just closed (phases 2k, 2k+1, 2k+2). Kept with the hold, kept or dropped,
// so the screen and the console show every cycle and not only the median.
static void frtCycleDone(int k, float d) {
  FrtHold &hd = frt.h[frt.hold];
  FrtCycle &c = hd.c[k];
  c.a0 = frt.phI[2 * k];
  c.b = frt.phI[2 * k + 1];
  c.a1 = frt.phI[2 * k + 2];
  const float step = 0.5f * (c.a0 + c.a1) - c.b;   // charging-positive amps: the field's draw lowers the reading
  c.ok = fabsf(c.a0 - c.a1) <= fmaxf(FRT_DRIFT_A, FRT_DRIFT_FRAC * fabsf(step));
  c.f = (d > 0.001f) ? step / d : NAN;
  hd.nc = k + 1;
  char drop[96] = "";
  if (!c.ok) snprintf(drop, sizeof(drop), ", dropped: the two field-off readings differ by %.3f A (the battery current moved)", fabsf(c.a0 - c.a1));
  queueConsoleMessageF("Field resistance test, %s at %.2f V, cycle %d: battery %.3f / %.3f / %.3f A (off, on, off), step %.3f A / %.2f%% = %.3f A field%s",
                       FRT_HOLD_NAME[frt.hold], frt.holdX, k + 1, c.a0, c.b, c.a1, step, 100.0f * d, c.f, drop);
  // How far the live check's two means wandered around the cycle's answer: the margin under FRT_MAX_A / FRT_FAST_MAX_A.
  if (frt.lvLo <= frt.lvHi)
    queueConsoleMessageF("  live field amps after the settle: %.2f to %.2f A (0.3 s mean), %.2f to %.2f A (40 ms mean)", frt.lvLo, frt.lvHi, frt.fsLo, frt.fsHi);
  frtLiveRangeReset();
}

// Takes the hold index, not an FrtHold: the Arduino prototype pass would declare it above the struct.
static int frtGoodCycles(int hold) {
  const FrtHold &hd = frt.h[hold];
  int good = 0;
  for (int k = 0; k < hd.nc; k++) good += hd.c[k].ok ? 1 : 0;
  return good;
}

// The stop when the battery current will not hold still. A field-off reading above FRT_CHARGING_A is a charge into the
// battery with the engine off, so the message names a charger.
static void frtFailDrift(float offA) {
  char why[260];
  if (offA > FRT_CHARGING_A)
    snprintf(why, sizeof(why), "The battery current kept changing between readings, and something was charging the battery at %.1f A with the engine off. Switch off solar, shore power and DC-DC chargers, then try again.", offA);
  else
    snprintf(why, sizeof(why), "The battery current kept changing between readings: a load switching on and off (fridge, pumps, inverter) or a charger changing its output. Switch them off and try again.");
  frtFail(why);
}

// Median step and field-on volts of the good cycles of the hold just completed (it stops at three). false: the test has
// failed.
static bool frtShuntHoldResult(float &stepA, float &vB) {
  const FrtHold &hd = frt.h[frt.hold];
  float steps[3];
  float vs[3];
  int good = 0;
  for (int k = 0; k < hd.nc && good < 3; k++) {
    if (!hd.c[k].ok) continue;
    steps[good] = 0.5f * (hd.c[k].a0 + hd.c[k].a1) - hd.c[k].b;
    vs[good] = frt.phV[2 * k + 1];
    good++;
  }
  if (good < 2) {
    frtFailDrift(frt.baseA);
    return false;
  }
  if (good == 3) {
    // The middle step itself, bit for bit, so the hold's amps match its median cycle's on the screen.
    stepA = fmaxf(fminf(steps[0], steps[1]), fminf(fmaxf(steps[0], steps[1]), steps[2]));
    vB = (vs[0] + vs[1] + vs[2]) / 3.0f;
  } else {
    stepA = 0.5f * (steps[0] + steps[1]);
    vB = 0.5f * (vs[0] + vs[1]);
  }
  return true;
}

static void frtFinish(uint32_t now) {
  const FrtHold &t = frt.h[FRT_H_TOP], &m = frt.h[FRT_H_MID], &p = frt.h[FRT_H_PROBE];
  char why[240];
  if (!(t.i >= 1.3f * m.i)) {
    snprintf(why, sizeof(why), "The field current did not rise enough between the top and middle holds (%.2f A and %.2f A). Check Max Field %% and Max Field Volts under Setup > Alternator, then try again.", t.i, m.i);
    frtFail(why);
    return;
  }
  const float r = (t.x - m.x) / (t.i - m.i);
  const float vc = m.x - m.i * r;
  if (!(r >= 0.05f && r <= 100.0f)) {
    snprintf(why, sizeof(why), "The result, %.3f ohm, is outside 0.05 to 100 ohm. Check the readings and try again.", r);
    frtFail(why);
    return;
  }
  // A drop a little below zero is scatter on a drop too small to measure; scatter scales with hold volts, so the floor is a
  // fifth of the middle hold's volts (below that: a misread hold, or a clamp offset over a fifth of its amps).
  const float vcFloor = -0.2f * m.x;
  if (vc > 3.0f) {
    snprintf(why, sizeof(why), "The brush and contact drop came out at %.2f V, over 3 V. Check the readings and try again.", vc);
    frtFail(why);
    return;
  }
  if (!(vc >= vcFloor)) {
    snprintf(why, sizeof(why), "The brush and contact drop came out at %.2f V. A real drop is zero or more, and scatter between readings reaches only %.2f V on these holds. Check the readings and try again.", vc, vcFloor);
    frtFail(why);
    return;
  }
  frt.r = r;
  frt.vc = vc;
  frt.probeOffV = p.x - (vc + r * p.i);
  frt.state = FRT_DONE;
  frt.endMs = now;
  frtFieldOff();
  snprintf(frt.msg, sizeof(frt.msg), "%.3f ohm", r);
  const float tC = isfinite(frt.probeC) ? frt.probeC : FD_TREF_DEF_C;
  queueConsoleMessageF("Field resistance test (%s): R %.3f ohm, contact drop %.2f V, alternator %.0f%s%s",
                       frt.method == FRT_SHUNT ? "shunt" : "meter", r, vc, dispTempF(tC * 1.8f + 32.0f), dispTempUnit(),
                       isfinite(frt.probeC) ? "" : " (no probe reading, 30 C assumed)");
  frtLogHolds();
  queueConsoleMessageF("  low hold off the top-middle line by %.3f V", frt.probeOffV);
  queueConsoleMessageF("  longest gap between the app's requests: %.1f s (the test stops at %.0f s)", frt.hbWorstMs / 1000.0f, FRT_HB_MS / 1000.0f);
}

// A hold's current is in: record it and plan the next hold, or finish.
static void frtHoldDone(uint32_t now, float amps, float v) {
  FrtHold &hd = frt.h[frt.hold];
  hd.d = frt.holdDuty;
  hd.v = v;
  hd.x = frtXAt(frt.holdDuty, v);
  hd.i = amps;
  hd.tC = fieldResProbeC();
  const float vNow = isfinite(IBV) ? IBV : v;
  const float xCeil = frtXAt(frtCeilPct(), vNow);
  char why[240];

  switch (frt.hold) {
    case FRT_H_PROBE: {
      if (amps < FRT_PROBE_MIN_A && frt.rung < 2 && FRT_PROBE_X[frt.rung + 1] <= xCeil + 0.01f) {
        frt.rungX[frt.rung] = hd.x;
        frt.rungI[frt.rung] = amps;
        hd.d = hd.v = hd.x = hd.i = hd.tC = NAN;   // the low hold's row is the rung in progress; the screen lists this one apart
        frt.rung++;
        frtStartHold(now, FRT_PROBE_X[frt.rung], true);
        return;
      }
      if (!(amps > 0.0f)) {
        frtFail("No field current was measured at the first holds. Check the field wiring and the field fuse.");
        return;
      }
      const float r1 = hd.x / amps;
      if (amps < FRT_AIM_SKIP_A) {
        // Planned by V / A it lands above FRT_AIM_A by the contact drop's share, held to 2.5 x the probe; at least
        // 1.25 x the probe so the slope to it is a real one.
        const float xa = fmaxf(1.25f * hd.x, fminf(FRT_AIM_A * r1, 2.5f * hd.x));
        if (xa < xCeil - 0.05f) {
          frt.hold = FRT_H_AIM;
          frtStartHold(now, xa, true);
          return;
        }
      }
      // A probe already near the top current, or the ceiling leaves no room for an aiming hold: the top from V / A.
      frt.hold = FRT_H_TOP;
      frtStartHold(now, fminf(fminf(FRT_TOP_AIM_A * r1, 4.0f * hd.x), xCeil), true);
      return;
    }
    case FRT_H_AIM: {
      const FrtHold &p = frt.h[FRT_H_PROBE];
      if (!(amps > 1.1f * p.i)) {
        snprintf(why, sizeof(why), "The field current did not rise between the first holds (%.2f A and %.2f A). Max Field %% or Max Field Volts under Setup > Alternator may be holding the field too low.", p.i, amps);
        frtFail(why);
        return;
      }
      // The contact drop is in both holds, so their slope is the winding's and the top lands near 4 A. Held between
      // this hold and twice it, so a misread meter cannot send the top far past the plan.
      const float s = (hd.x - p.x) / (amps - p.i);
      const float xt = fminf(constrain(hd.x + s * (FRT_TOP_AIM_A - amps), hd.x, 2.0f * hd.x), xCeil);
      frt.hold = FRT_H_TOP;
      frtStartHold(now, xt, true);
      return;
    }
    case FRT_H_TOP: {
      // The middle sits on the line from the hold below the top: the aiming hold, unless the top repeated its voltage.
      const FrtHold &a = frt.h[FRT_H_AIM];
      const FrtHold &b = (isfinite(a.i) && hd.x > 1.05f * a.x) ? a : frt.h[FRT_H_PROBE];
      if (!(amps > 1.1f * b.i)) {
        snprintf(why, sizeof(why), "The field current did not rise between the first holds (%.2f A and %.2f A). Max Field %% or Max Field Volts under Setup > Alternator may be holding the field too low.", b.i, amps);
        frtFail(why);
        return;
      }
      const float s = (hd.x - b.x) / (amps - b.i);
      float xm = b.x + s * (FRT_MID_FRAC * amps - b.i);
      xm = constrain(xm, 0.2f * hd.x, 0.85f * hd.x);
      frt.hold = FRT_H_MID;
      frtStartHold(now, xm, true);
      return;
    }
    default:
      frtFinish(now);
      return;
  }
}

// INA228 sample hook (every committed read, same loop task as frtService: no locking). Live current = mean of the last
// FRT_RING_N reads (~0.3 s): INA228 fast-mode windows beat slowly with the 400 Hz PWM, so an 8-read mean wanders +-6 % at
// the top hold and +-24 % at the 3 V probe (64 reads: +-2 % / +-9 %). The 8-read mean is kept for a fault-sized current.
void frtInaSample(float bcur, float v, bool shuntOk) {
  if (frt.state != FRT_RUNNING) return;
  const uint32_t el = millis() - frt.phaseStartMs;
  if (frt.method == FRT_SHUNT) {
    if (!shuntOk) return;
    if ((frt.phase & 1) && frt.holdDuty > 0.1f && isfinite(frt.baseA)) {
      if (frt.ringN == FRT_RING_N) frt.ringSum -= frt.ring[frt.ringK];
      frt.ring[frt.ringK] = bcur;
      frt.ringSum += bcur;
      frt.ringK = (frt.ringK + 1) % FRT_RING_N;
      if (frt.ringN < FRT_RING_N) frt.ringN++;
      if (frt.ringN >= 8) {
        const float d = frt.holdDuty / 100.0f;
        float s8 = 0.0f;
        for (int k = 1; k <= 8; k++) s8 += frt.ring[(frt.ringK + FRT_RING_N - k) % FRT_RING_N];
        const float fast = (frt.baseA - s8 / 8.0f) / d;
        frt.liveI = (frt.baseA - (float)(frt.ringSum / frt.ringN)) / d;
        if (el >= FRT_SETTLE_MS && frt.ringN == FRT_RING_N) {
          frt.lvLo = fminf(frt.lvLo, frt.liveI);
          frt.lvHi = fmaxf(frt.lvHi, frt.liveI);
          frt.fsLo = fminf(frt.fsLo, fast);
          frt.fsHi = fmaxf(frt.fsHi, fast);
        }
        if (!frt.overA && fast > FRT_FAST_MAX_A) {
          frt.overA = true;
          frt.tripA = fast;
          frt.tripLim = FRT_FAST_MAX_A;
        } else if (!frt.overA && frt.ringN == FRT_RING_N && frt.liveI > FRT_MAX_A) {
          frt.overA = true;
          frt.tripA = frt.liveI;
          frt.tripLim = FRT_MAX_A;
        }
      }
    }
    if (el >= FRT_SETTLE_MS) {
      frt.sumI += bcur;
      frt.sumV += v;
      frt.n++;
    }
  } else if (frt.phase == FRT_MP_HOLD && el >= FRT_SETTLE_MS) {
    frt.sumV += v;
    frt.n++;
  }
}

// Every control pass, ahead of buildTickSnapshot: requests, the run's own aborts, and the phase clock. Sets frt.duty.
void frtService(uint32_t now) {
  if (frt.reqAbort) {
    frt.reqAbort = false;
    frtFail("Test stopped.");
  }
  if (frt.reqStart) {
    frt.reqStart = false;
    if (frt.state != FRT_RUNNING) frtBegin(now);
  }
  if (frt.state != FRT_RUNNING) {
    frt.reqAgain = false;
    frt.reqReading = false;
    return;
  }
  if ((int32_t)(now - frt.lastPollMs) > (int32_t)FRT_HB_MS) {   // signed: the web task can stamp lastPollMs after now was sampled
    const float worst = frt.hbWorstMs / 1000.0f;
    frtFail("The app stopped answering, so the test stopped and the field is off.");
    queueConsoleMessageF("  no request from the app for %.1f s; the longest gap before that was %.1f s", (now - frt.lastPollMs) / 1000.0f, worst);
    return;
  }
  const uint32_t ibvTs = dataTimestamps[IDX_IBV];
  if (ibvTs == 0 || (uint32_t)(now - ibvTs) > 2500UL || !isfinite(IBV)) {
    frtFail("The battery voltage reading stopped, so the test stopped.");
    return;
  }

  if (frt.method == FRT_SHUNT) {
    if (frt.overA) {
      char why[260];
      snprintf(why, sizeof(why), "The field current went above %.0f A (%.1f A at the %s), more than the test plan allows. The field was switched off. Check the rotor and the field wiring, or a large load switched on during the test.", frt.tripLim, frt.tripA, FRT_HOLD_NAME[frt.hold]);
      frtFail(why);
      return;
    }
    const uint32_t bTs = dataTimestamps[IDX_BCUR];
    if (bTs == 0 || (uint32_t)(now - bTs) > 2500UL) {
      frtFail("The battery current reading stopped, so the test stopped.");
      return;
    }
    if ((uint32_t)(now - frt.phaseStartMs) >= FRT_PHASE_MS) {
      if (frt.n < 20) {
        frtFail("Too few battery current readings arrived during a hold. Check the shunt connection and try again.");
        return;
      }
      frt.phI[frt.phase] = (float)(frt.sumI / frt.n);
      frt.phV[frt.phase] = (float)(frt.sumV / frt.n);
      if (!(frt.phase & 1)) frt.baseA = frt.phI[frt.phase];
      // The field first comes on once two field-off readings in a row agree: a battery current that is already wandering
      // (a charger, a cycling load) stops the test here, before the coil is driven at all.
      if (frt.phase == 0 && !frt.calm) {
        if (isfinite(frt.gateA) && fabsf(frt.phI[0] - frt.gateA) <= FRT_DRIFT_A) {
          frt.calm = true;
        } else if ((uint32_t)(now - frt.startMs) >= FRT_CALM_MS) {
          queueConsoleMessageF("Field resistance test: the field-off battery current did not settle within %.0f mA in %.0f s (last two readings %.3f and %.3f A)",
                               1000.0f * FRT_DRIFT_A, FRT_CALM_MS / 1000.0f, frt.gateA, frt.phI[0]);
          frtFailDrift(frt.phI[0]);
          return;
        } else {
          queueConsoleMessageF("Field resistance test, waiting for a steady battery current: field off, battery %.3f A", frt.phI[0]);
          frt.gateA = frt.phI[0];
          frtResetWindow(now);
          frt.duty = 0.0f;
          return;
        }
      }
      frt.phase++;
      frtResetWindow(now);
      if (frt.phase >= 3 && (frt.phase & 1)) {   // a field-off phase just closed cycle k
        const uint8_t hold = frt.hold;
        const int k = (frt.phase - 3) / 2;
        const float d = frt.holdDuty / 100.0f;
        frtCycleDone(k, d);
        const FrtCycle c = frt.h[hold].c[k];
        const float step = 0.5f * (c.a0 + c.a1) - c.b;
        const int good = frtGoodCycles(hold);
        const int left = FRT_MAX_CYC - (k + 1);
        // The probe decides on its first good cycle: step up a rung, give up on the shunt, or go on to three good cycles.
        // A dropped cycle decides nothing.
        const bool firstGood = c.ok && good == 1;
        if (hold == FRT_H_PROBE && firstGood && frt.rung == 1 && step < FRT_NO_STEP_A) {
          frtFail("The battery shunt is not seeing the field current. A charger may be holding the battery voltage (switch off shore power, solar and DC-DC chargers), or the regulator's power or ground is not on the shunted battery, or its wires do not land as the wiring page shows. Otherwise use a meter instead.");
          return;
        }
        if (hold == FRT_H_PROBE && firstGood && step / d < FRT_PROBE_MIN_A && frt.rung < 2) {
          frtHoldDone(now, step / d, frt.phV[2 * k + 1]);   // up a rung, carrying the field-off phase just closed
        } else if (hold == FRT_H_AIM && c.ok) {
          frtHoldDone(now, step / d, frt.phV[2 * k + 1]);   // it only aims the top: its first good cycle is enough
        } else if (hold != FRT_H_AIM && (good == 3 || left == 0 || good + left < 2)) {   // a dropped cycle is retaken while two good can still come
          float stepA, vB;
          if (!frtShuntHoldResult(stepA, vB)) return;
          frtHoldDone(now, (d > 0.001f) ? stepA / d : NAN, vB);
        } else if (hold == FRT_H_AIM && left == 0) {
          frtFailDrift(frt.baseA);   // no good cycle in five
        }
        if (frt.state != FRT_RUNNING) return;
      }
    }
    frt.duty = (frt.phase & 1) ? frt.holdDuty : 0.0f;
  } else {
    if (frt.reqAgain) {
      frt.reqAgain = false;
      frt.holdDuty = frtDutyFor(frt.holdX, IBV);
      frt.phase = FRT_MP_HOLD;
      frtResetWindow(now);
    }
    if (frt.phase == FRT_MP_HOLD && (uint32_t)(now - frt.phaseStartMs) >= FRT_METER_HOLD_MS) {
      frt.phV[0] = (frt.n > 0) ? (float)(frt.sumV / frt.n) : IBV;
      frt.phase = FRT_MP_WAIT;
    }
    if (frt.reqReading) {
      frt.reqReading = false;
      if (frt.phase == FRT_MP_HOLD) frt.phV[0] = (frt.n > 0) ? (float)(frt.sumV / frt.n) : IBV;
      const float amps = frt.reqAmps;
      if (!(amps >= FRT_METER_MIN_A)) {
        frtFail("The meter read under 0.05 A, so it is not reading the field current. Check it is on DC amps (a clamp must be a DC clamp, zeroed), around or in series with the Regulator Field wire.");
        return;
      }
      queueConsoleMessageF("Field resistance test, %s at %.2f V: meter %.3f A", FRT_HOLD_NAME[frt.hold], frt.holdX, amps);
      frtHoldDone(now, amps, frt.phV[0]);
      if (frt.state != FRT_RUNNING) return;
    }
    frt.duty = (frt.phase == FRT_MP_HOLD) ? frt.holdDuty : 0.0f;
  }
}

// Only the normal CV body commits the unified clamp flag, so paths that skip it clear it here (wherever the CV-entry tracker
// is cleared): a stale flag fires the protection-release reseed on CV re-entry and swallows the next clamp's rising edge.
static inline void clearStaleProtClamp() {
  g_fastOvClampActive = false;
  g_ovClampRiseMs = 0;
}

// MODE_FIELD_RES_TEST: the hold duty frtService chose, with GPIO4 up for the whole run (a field-off phase is duty 0) so
// the INA228 stays in its fast mode and both phases are read the same way. Bypasses the AUTO/stage machinery like the
// commissioning rest; a fault never lands here (the arbiter selects the fault mode instead).
void runFieldResTestHold(const TickSnapshot &tick, FieldEventReason reason) {
  voltageControlActive = false;
  lastVoltageControlActive = false;
  cvWindDownActive = false;
  clearStaleProtClamp();
  uTargetAmps = 0;
  setpointLimited = 0.0f;
  ctrlLimiter = 0;
  shutdownPhase = SHUTDOWN_PHASE_NONE;
  digitalWrite(4, HIGH);
  gpio4IsLow = false;
  apply_pwm_float(frt.duty);
  currentPID.ResetIntegratorTo(0.0);
  pidOutput = 0.0;
  lastAppliedDuty = frt.duty;
  dutyCycle = frt.duty;
  updateFieldTelemetry(dutyCycle, tick.currentBatteryVoltage);
  fieldActiveStatus = (dutyCycle > 0.01f) ? 1 : 0;
  chargeStageDisplay = getChargeStageDisplayCode();
  reportFieldModeEvent(tick.nowMs, MODE_FIELD_RES_TEST, reason, tick, gpio4IsLow, dutyCycle);
}

// Settings rescale write-ahead journal: every NVS write of one rescale (class change, winding move) is staged as "key=value;",
// saved under NK_rescaleJournal before the writes and removed after; rescaleJournalReplay() finishes an interrupted set at
// boot. Without it a cut-short 48 -> 12 V change left ~58 V OV trips on a 12 V bank, and a winding factor applied twice.
static void rescaleStage(String &jr, const char *key, const String &value) {
  jr += key; jr += '='; jr += value; jr += ';';
}

static void rescaleWritePairs(const char *jr) {
  char key[16], val[24];
  const char *p = jr;
  while (*p) {
    const char *eq = strchr(p, '=');
    const char *end = eq ? strchr(eq, ';') : nullptr;
    if (!eq || !end || eq - p >= (int)sizeof(key) || end - eq - 1 >= (int)sizeof(val)) return;   // malformed: stop rather than write a guess
    memcpy(key, p, eq - p); key[eq - p] = '\0';
    memcpy(val, eq + 1, end - eq - 1); val[end - eq - 1] = '\0';
    settingWrite(key, val);
    p = end + 1;
  }
}

// The stored field-side duty knobs, with the clamp the scaled value is held to. One list for the rescale
// (scaleFieldSideKnobs) and the import's conversion of what it did not write (fieldSideConvertUnwritten). The alt-health
// registry knobs persist under their registry names (NVS key = name).
struct FieldSideKnob { const char *key; float *v; float lo; float hi; };
static const FieldSideKnob FIELD_SIDE_KNOBS[] = {
  { NK_kneeMarginPct,         &kneeMarginPct,         -INFINITY, INFINITY },
  { NK_kneeStepPct,           &kneeStepPct,           -INFINITY, INFINITY },
  { NK_kneeMaxFloorPct,       &kneeMaxFloorPct,       -INFINITY, INFINITY },
  { NK_kneeDutyTolPct,        &kneeDutyTolPct,        -INFINITY, INFINITY },   // field-duty steadiness band
  { NK_DutyRampRate,          &DutyRampRate,          -INFINITY, INFINITY },
  { NK_DutySlowRampRate,      &DutySlowRampRate,      -INFINITY, INFINITY },
  { NK_MinDuty,               &MinDuty,               -INFINITY, INFINITY },   // field floor: float, keeps sub-1% resolution
  { NK_LimpDutyPct,           &LimpDutyPct,           0.0f,      99.0f    },   // reduced-sensor tier duty cap / fixed duty
  { NK_HuntTrigPct,           &HuntTrigPct,           HUNT_TRIG_PCT_MIN, INFINITY },   // oscillation-damper detection bar
  { NK_ManualDutyTarget,      &ManualDutyTarget,      0.0f,      100.0f   },   // Manual mode field duty
  { NK_SystemIDStepAmplitude, &SystemIDStepAmplitude, -INFINITY, INFINITY },   // plant-test wave amplitude
  { "altDutyLineRms",         &altDutyLineRms,        -INFINITY, INFINITY },   // alt-health field-duty straightness limit (% points)
  { "altDutySlewMax",         &altDutySlewMax,        -INFINITY, INFINITY },   // …and its slew cap (% points per second)
  { "altMinDuty",             &altMinDuty,            -INFINITY, INFINITY },   // alt-health admission duty floor
};

// Scaled in place by r and staged into the journal. r = class factor (oldV/newV) times winding factor (newW/oldW) in one
// multiply so a clamp cannot bite between the two. Stored in REAL duty-% for the bus, so nothing multiplies them at use.
static void scaleFieldSideKnobs(float r, String &jr) {
  // 4 decimals: a 12 -> 48 -> 12 round trip must land back on the 12 V values, which 2 decimals do not (0.125 -> 0.13)
  for (const FieldSideKnob &k : FIELD_SIDE_KNOBS) {
    *k.v = constrain(*k.v * r, k.lo, k.hi);
    rescaleStage(jr, k.key, String(*k.v, 4));
  }
  altSweepRatePctS *= r;   // RAM-only sweep ramp and floor (the panel posts its own with each start)
  altSweepFromPct  *= r;
}

// True for an NVS key fieldSideConvertUnwritten converts: a FIELD_SIDE_KNOBS entry or a current-loop gain.
bool fieldSideKey(const char *key) {
  for (const FieldSideKnob &k : FIELD_SIDE_KNOBS)
    if (strcmp(k.key, key) == 0) return true;
  return strcmp(key, NK_PidKp) == 0 || strcmp(key, NK_PidKi) == 0 || strcmp(key, NK_PidKd) == 0;
}

// An import moving stored fieldWindApplied (fromW -> toW) relabels every stored field-side value onto toW's scale. Keys it
// did not write (writtenKeys = ";key;key;...;") are converted in NVS by toW/fromW so the boot reconcile moves all alike. With
// the current loop commissioned the boot rescale holds the applied gain, so PidK* convert by fromW/toW instead.
void fieldSideConvertUnwritten(int fromW, int toW, const String &writtenKeys) {
  auto norm = [](int w) { return (w == 24 || w == 36 || w == 48) ? w : 12; };   // the boot loader's reading of the key
  fromW = norm(fromW);
  toW = norm(toW);
  if (fromW == toW) return;
  const float r = (float)toW / (float)fromW;
  auto written = [&](const char *key) {
    String t = ";";
    t += key;
    t += ';';
    return writtenKeys.indexOf(t) >= 0;
  };
  for (const FieldSideKnob &k : FIELD_SIDE_KNOBS) {
    if (written(k.key) || !settingExists(k.key)) continue;
    settingWrite(k.key, String(constrain(settingRead(k.key).toFloat() * r, k.lo, k.hi), 4).c_str());
  }
  if (commissionDoneMask & (1u << 2)) {
    const char *gainKeys[] = { NK_PidKp, NK_PidKi, NK_PidKd };
    for (const char *key : gainKeys) {
      if (written(key) || !settingExists(key)) continue;
      settingWrite(key, String(settingRead(key).toFloat() / r, 6).c_str());
    }
  }
}

// Per-RPM Min% floors back to the flat KNEE_BASELINE_PCT baseline (behaves as MinDuty alone) with the knee tracker
// unlearned; bin 0 keeps its permanent 0% lock while the learner owns the table, matching the boot rebuild in kneeLearnInit.
static void kneeTableResetToBaseline() {
  for (int i = 0; i < RPM_TABLE_SIZE; i++) {
    kneeKnee[i] = 0; kneeFrozen[i] = false; kneeLearnTempF[i] = 0; kneeLastMs[i] = 0;
    kneeFloor[i] = (i == 0) ? 0.0f : defaultMinDutyValues[i];
    rpmMinDutyTable[i] = kneeLearnEnable ? kneeFloor[i] : defaultMinDutyValues[i];
  }
  kneeFitA = 0.0f; kneeFitC = 0.0f;   // no commissioning fit any more -> live correction reverts to whole-knee
  kneeFitResidPct = -1.0f; kneeFitWorstIdx = -1;   // -1/-1 = "no fit", same sentinel the wizard's clear path writes
  saveKneeLearnState();   // writes the column too
}

// No winding this board can drive draws over 10 A at its own rating, so W / R > 10 A means a 12 V winding entered as
// 24/36/48 V (below 2.4/3.6/4.8 ohm), which would run every field setting W/12 too strong: it waits on the owner's
// confirmation. Compared in whole milliohms, as the dashboard preview does, so the two cannot round a borderline R differently.
bool fieldWindingImplausible() {
  return FieldWindingV > 12 && isfinite(FieldResistance) && FieldResistance > 0.0f
         && (float)lroundf(FieldResistance * 1000.0f) * FD_FIELD_MAX_A < (float)FieldWindingV * 1000.0f;
}

static void fieldWindAckKeyFor(char *buf, size_t n) {
  snprintf(buf, n, "%u:%.4f", (unsigned)FieldWindingV, FieldResistance);
}

// The winding the field-side knobs should be scaled for now: the owner's choice, or 12 while the resistance check
// doubts it and the owner has not confirmed this exact winding and resistance.
uint8_t fieldWindingEffective() {
  if (!fieldWindingImplausible()) return FieldWindingV;
  char k[sizeof(fieldWindAck)];
  fieldWindAckKeyFor(k, sizeof(k));
  return (strcmp(k, fieldWindAck) == 0) ? FieldWindingV : 12;
}

// The dashboard's answer names the pairing it was asked about ("W:R", R to 3 decimals); it stands only while that pairing is
// still on the unit and still waiting, so a question left open in a second window cannot answer a changed state.
bool fieldWindingAnswerOpen(const char *asked) {
  unsigned w = 0;
  float r = NAN;
  if (!asked || sscanf(asked, "%u:%f", &w, &r) != 2) return false;
  return w == FieldWindingV && isfinite(FieldResistance) && fabsf(r - FieldResistance) < 0.0005f
         && fieldWindingEffective() != FieldWindingV;
}

// Moves stored field-side knobs by classRatio (oldV/newV on a class change, else 1) and oldW→newW in one multiply; a winding
// move also sets Max Field Volts to 1.25 x newW. With classJr the writes go into the class change's journal and the knee
// reset, map clear, console line and re-commission notice are left to it; otherwise this commits its own journal.
static void applyFieldWindingRescale(float classRatio, uint8_t oldW, uint8_t newW, const char *why, String *classJr) {
  if (oldW == 0 || newW == 0) return;
  const bool windMoved = (oldW != newW);
  const float r = classRatio * (float)newW / (float)oldW;
  if (!windMoved && r == 1.0f) return;
  // A commissioned current-loop gain (stage 2) was measured on this alternator, which a winding correction does not change:
  // PidK* move by oldW/newW so the duty-space gain is unchanged. Default (12 V referenced) gains follow the winding.
  const bool holdGain = windMoved && !classJr && (commissionDoneMask & (1u << 2));
  String own;
  String &jr = classJr ? *classJr : own;
  if (!classJr) own.reserve(768);
  scaleFieldSideKnobs(r, jr);
  if (windMoved) {
    MaxFieldVolts = constrain(1.25f * (float)newW, 0.5f, 60.0f);
    rescaleStage(jr, NK_MaxFieldVolts, String(MaxFieldVolts, 1));
    if (holdGain) {
      const float g = (float)oldW / (float)newW;
      PidKp *= g; PidKi *= g; PidKd *= g;
      rescaleStage(jr, NK_PidKp, String(PidKp, 6));
      rescaleStage(jr, NK_PidKi, String(PidKi, 6));
      rescaleStage(jr, NK_PidKd, String(PidKd, 6));
    }
    fieldWindApplied = newW;
    rescaleStage(jr, NK_fieldWindApplied, String((int)newW));
  }
  if (classJr) return;
  settingWrite(NK_rescaleJournal, jr.c_str());
  rescaleWritePairs(jr.c_str());
  kneeTableResetToBaseline();   // floors learned against the old ceiling and clamps; same reasoning as a class change
  huntMapClearAll("field winding scale changed");
  char gainTxt[96];
  if (holdGain) snprintf(gainTxt, sizeof(gainTxt), "current-loop gain kept as measured in commissioning (PID Kp/Ki/Kd x%.2f)", (float)oldW / (float)newW);
  else          snprintf(gainTxt, sizeof(gainTxt), "current-loop gain applied to the field x%.2f", r);
  queueConsoleMessageF("Field settings moved from a %uV to a %uV field winding (%s): field-duty settings x%.2f, %s, Max Field Volts %.1fV, tachometer keep-alive floors reset to the %.2f%% default. Re-run the Keep-Alive Floor step.",
                       (unsigned)oldW, (unsigned)newW, why, r, gainTxt, MaxFieldVolts, KNEE_BASELINE_PCT);
  if (CommissionEpoch > 0) raiseRecommissionNag();
  settingRemove(NK_rescaleJournal);
  recomputeCcGains();
  if (pidInitialized) applyCcOutputLimits();
  stateRevision++;
  settingsDirty = true;
}

// Boot, before any settings loader reads a rescaled key: writes out a rescale journal a reboot left behind. Its values
// are absolute, so writing them again is harmless. rescaleJournalFinish() does the rest once the subsystems it
// touches have loaded, and only then removes the journal, so a second reboot in between replays it again.
static bool rescaleReplayed = false;
void rescaleJournalReplay() {
  const String jr = settingRead(NK_rescaleJournal);
  if (!jr.length()) return;
  rescaleWritePairs(jr.c_str());
  rescaleReplayed = true;
}

// End of setup(), before the boot reconcile. Repeats the knee-floor reset, damper-map clear and re-commission notice the
// interrupted rescale may not have reached; where it had, running them again changes nothing. The INA228 comparator
// needs nothing here: its boot setup programs it from the replayed VoltageHardwareLimit.
void rescaleJournalFinish() {
  if (!rescaleReplayed) return;
  rescaleReplayed = false;
  kneeTableResetToBaseline();
  huntMapClearAll("settings rescale finished at boot");
  if (CommissionEpoch > 0) raiseRecommissionNag();
  settingRemove(NK_rescaleJournal);
  queueConsoleMessageF("A system voltage or field winding change was cut short by a reboot and was finished at boot: tachometer keep-alive floors reset to the %.2f%% default, learned oscillation-damper ranges cleared. Re-run the Keep-Alive Floor step.",
                       KNEE_BASELINE_PCT);
}

// Brings the stored knobs onto the scale fieldWindingEffective() asks for. Called at the end of setup() (an import
// writes NVS raw), after a change of the winding setting, the field resistance or the owner's confirmation. A move of a
// commissioned tune raises the re-commission notice inside the rescale.
bool fieldWindingReconcile(const char *why) {
  const uint8_t eff = fieldWindingEffective();
  if (eff == fieldWindApplied) return false;
  if (eff == 12 && fieldWindingImplausible())
    queueConsoleMessageF("Field winding set to %uV, but Field Resistance %.3f ohm would draw %.1fA at that rating, above the regulator's 10 A field limit. Field settings held at 12 V winding values until this is confirmed in the app.",
                         (unsigned)FieldWindingV, FieldResistance, (float)FieldWindingV / FieldResistance);
  applyFieldWindingRescale(1.0f, fieldWindApplied, eff, why, nullptr);
  return true;
}

// The owner's confirmation of a winding the resistance check doubted, recorded for this exact pairing.
void fieldWindingConfirm() {
  if (FieldWindingV <= 12 || !(isfinite(FieldResistance) && FieldResistance > 0.0f)) return;
  fieldWindAckKeyFor(fieldWindAck, sizeof(fieldWindAck));
  settingWrite(NK_fieldWindAck, fieldWindAck);
  settingsDirty = true;
  queueConsoleMessageF("Field winding %uV confirmed by the owner for Field Resistance %.3f ohm", (unsigned)FieldWindingV, FieldResistance);
}

// The owner's winding choice (Vessel Info save, /get FieldWindingV), clamped to the class. Any change clears the
// resistance-check confirmation. confirmAsked is the dashboard's "keep" answer ("W:R", asked before saving), recorded
// only if fieldWindingAnswerOpen() still holds for it after the set. Returns true when the knobs moved.
bool fieldWindingSet(int w, const char *confirmAsked) {
  if (w != 12 && w != 24 && w != 36 && w != 48) w = FieldWindingV;
  if (w > (int)SYSTEM_VOLTAGE_CLASS) w = SYSTEM_VOLTAGE_CLASS;
  if (w != (int)FieldWindingV) {
    FieldWindingV = (uint8_t)w;
    settingWrite(NK_FieldWindingV, String(w).c_str());
    fieldWindAck[0] = '\0';
    settingWrite(NK_fieldWindAck, "");
    stateRevision++;
    settingsDirty = true;
  }
  if (confirmAsked && *confirmAsked) {
    if (fieldWindingAnswerOpen(confirmAsked)) fieldWindingConfirm();
    else queueConsoleMessageF("Field winding answer for %s not recorded: the winding or Field Resistance changed after the question was asked, or it was already answered", confirmAsked);
  }
  return fieldWindingReconcile("winding setting changed");
}

// Single entry point for a system-voltage class change; call AFTER setting SYSTEM_VOLTAGE_CLASS = newV. On the import path it
// runs BEFORE the manifest loop, so a full export's own values then overwrite the conversion while a one-key class push keeps
// it. Volt-domain settings scale newV/oldV, field-side knobs and KHard oldV/newV; all go to NVS as one journaled set so a
// reboot mid-change cannot strand the OV trips at the old class. Not scaled: VoltageKd (runtime-normalized), CvKdMaxTrimA
// (flat amps), MaxDuty/MaxFieldVolts (describe the alternator). The Min% floor table is RESET, not rescaled (see below).
void applyNominalVoltageChange(int oldV, int newV) {
  if (newV != oldV && oldV > 0 && (newV == 12 || newV == 24 || newV == 36 || newV == 48)) {
    String jr;   // every NVS write of the change, committed as one journal below
    jr.reserve(2048);
    rescaleStage(jr, NK_BatteryVoltage, String(newV));
    float ratio = (float)newV / (float)oldV;
    BulkVoltage           *= ratio;
    FloatVoltage          *= ratio;
    AbsorptionVoltage     *= ratio;
    RebulkVoltage         *= ratio;
    TargetVoltageSetpoint *= ratio;
    ChargedVoltage_Scaled = (int)lroundf(ChargedVoltage_Scaled * ratio);
    VoltageAlarmHigh      *= ratio;
    VoltageAlarmLow       *= ratio;
    altSweepMarginV       *= ratio;   // sweep turnaround margin: volt-domain, RAM-only (posted per run)
    // Headroom scales with class so the OV ladder keeps its order (G2 clamp < timed tiers < absolute cuts), per-cell
    // equivalent; the INA228 comparator is reprogrammed from the scaled VoltageHardwareLimit.
    AlternatorHardShutdownV   *= ratio;
    VoltageHardwareLimit      *= ratio;
    OvMeasMarginV             *= ratio;
    OvPredMarginV             *= ratio;
    OvTierLoMarginV           *= ratio;  // timed-tier trip lines are volt-domain; dwells are absolute time — never scaled
    OvTierMidMarginV          *= ratio;
    dvccCvlMin                *= ratio;  // DVCC plausible-CVL window is volt-domain
    dvccCvlMax                *= ratio;
    VoltageDisagreeThreshold  *= ratio;
    IExcessArmMarginV         *= ratio;
    FastSetpointRiseHeadroomV *= ratio;
    CvKdArmV                  *= ratio;
    CvKdDeadbandVps           *= ratio;  // V/s — rise rate scales with class (48V bus rises ~4× faster per-cell)
    CvKdDbSlope               *= ratio;  // V/s per A — deadband line slope, same V-domain scaling as the base
    CvKdDbFloor               *= ratio;
    CvKdDbCeil                *= ratio;
    CvKdSlopeCeil             *= ratio;  // V/s real per-bus (no ×V/12 at use)
    vTgtRampUp                *= ratio;  // V/s
    vTgtRampDn                *= ratio;  // V/s
    cvWindDownStopV           *= ratio;  // V real per-bus — wind-down stop margin scales with class
    capSettleRateMv10         *= ratio;  // mV/10min rest-settle gate
    cvWaveAmplitudeV          *= ratio;  // CV waveform-test step height
    altVbusTol                *= ratio;  // alt-health bus-voltage steadiness band
    Ymin2                     *= ratio;  // voltage-plot axis window follows the bus
    Ymax2                     *= ratio;
    cvPlantKa                 *= ratio;  // Measured-gain V/A anchor is per-bus stiffness; recomputeCvGains below re-derives from it (unscaled it leaves Measured Kp/Ki newV/oldV too hot)
    KHard *= (float)oldV / (float)newV;   // A per V of OV excess: battery-side, follows the class whatever the winding
    rescaleStage(jr, NK_KHard, String(KHard, 1));
    // alt-health registry knobs persist under their registry names (NVS key = name)
    rescaleStage(jr, "altVbusTol", String(altVbusTol, 4));
    rescaleStage(jr, NK_cvWaveAmplitudeV, String(cvWaveAmplitudeV, 2));
    rescaleStage(jr, NK_BulkVoltage, String(BulkVoltage, 2));
    rescaleStage(jr, NK_FloatVoltage, String(FloatVoltage, 2));
    rescaleStage(jr, NK_AbsorptionVoltage, String(AbsorptionVoltage, 2));
    rescaleStage(jr, NK_RebulkVoltage, String(RebulkVoltage, 2));
    rescaleStage(jr, NK_TargetVoltageSetpoint, String(TargetVoltageSetpoint, 2));
    rescaleStage(jr, NK_ChargedVoltage, String(ChargedVoltage_Scaled));
    rescaleStage(jr, NK_VoltageAlarmHigh, String(VoltageAlarmHigh, 2));
    rescaleStage(jr, NK_VoltageAlarmLow, String(VoltageAlarmLow, 2));
    rescaleStage(jr, NK_AlternatorHardShutdownV, String(AlternatorHardShutdownV, 2));
    rescaleStage(jr, NK_VoltageHardwareLimit, String(VoltageHardwareLimit, 2));
    rescaleStage(jr, NK_OvMeasMarginV, String(OvMeasMarginV, 3));
    rescaleStage(jr, NK_OvPredMarginV, String(OvPredMarginV, 3));
    rescaleStage(jr, NK_OvTierLoMarginV, String(OvTierLoMarginV, 3));
    rescaleStage(jr, NK_OvTierMidMarginV, String(OvTierMidMarginV, 3));
    rescaleStage(jr, NK_dvccCvlMin, String(dvccCvlMin, 2));
    rescaleStage(jr, NK_dvccCvlMax, String(dvccCvlMax, 2));
    rescaleStage(jr, NK_VoltageDisagreeThreshold, String(VoltageDisagreeThreshold, 2));
    rescaleStage(jr, NK_IExcessArmMarginV, String(IExcessArmMarginV, 3));
    rescaleStage(jr, NK_FastSetpointRiseHeadroomV, String(FastSetpointRiseHeadroomV, 2));
    rescaleStage(jr, NK_CvKdArmV, String(CvKdArmV, 2));
    rescaleStage(jr, NK_CvKdDeadbandVps, String(CvKdDeadbandVps, 3));
    rescaleStage(jr, NK_CvKdDbSlope, String(CvKdDbSlope, 5));
    rescaleStage(jr, NK_CvKdDbFloor, String(CvKdDbFloor, 3));
    rescaleStage(jr, NK_CvKdDbCeil, String(CvKdDbCeil, 3));
    rescaleStage(jr, NK_CvKdSlopeCeil, String(CvKdSlopeCeil, 1));
    rescaleStage(jr, NK_vTgtRampUp, String(vTgtRampUp, 3));
    rescaleStage(jr, NK_vTgtRampDn, String(vTgtRampDn, 3));
    rescaleStage(jr, NK_cvWindDownStopV, String(cvWindDownStopV, 3));
    rescaleStage(jr, NK_capSettleRate, String(capSettleRateMv10, 2));
    rescaleStage(jr, NK_Ymin2, String(Ymin2));
    rescaleStage(jr, NK_Ymax2, String(Ymax2));
    rescaleStage(jr, NK_cvPlantKa, String(cvPlantKa, 5));
    if (FieldWindingV > newV) {   // a winding is never rated above the bus it sits on
      FieldWindingV = (uint8_t)newV;
      rescaleStage(jr, NK_FieldWindingV, String(newV));
      fieldWindAck[0] = '\0';
      rescaleStage(jr, NK_fieldWindAck, String());
    }
    // Field-side knobs follow fieldScale() (oldV/newV with the winding unchanged) times the winding move the clamp above
    // makes, in one multiply so the 99% Limp cap cannot clamp between them.
    applyFieldWindingRescale((float)oldV / (float)newV, fieldWindApplied, fieldWindingEffective(), "system voltage changed", &jr);
    settingWrite(NK_rescaleJournal, jr.c_str());   // from here a reboot finishes the whole set at boot
    rescaleWritePairs(jr.c_str());
    // Min% floor table: RESET, never rescaled. Max Field Volts works out to a different duty on the new bus, so old floors can
    // sit at or above the ceiling: the floor then outranks the cap, and the tach-lie arm bar becomes unsatisfiable, silently
    // disarming the phantom-RPM cut. The re-commission flag clears nothing, so the reset must happen here.
    kneeTableResetToBaseline();
    queueConsoleMessageF("System voltage %dV -> %dV: tachometer keep-alive floors reset to the %.2f%% default and their learning cleared (old floors were %dV-referenced and can exceed the field ceiling Max Field Volts works out to on a %dV bus). Re-run the Keep-Alive Floor step.",
                         oldV, newV, KNEE_BASELINE_PCT, oldV, newV);
    huntMapClearAll("system voltage class changed");  // pockets learned on the old class describe a different alternator
    settingRemove(NK_rescaleJournal);
    inaOvReprogramPending = true;  // callers run on the web task; loop() owns the I2C bus
    stateRevision++;
    settingsDirty = true;
  }
  recomputeCvGains();  // always re-derive the normalized active gains for the (possibly) new class
  recomputeCcGains();
  applyCcOutputLimits();  // ceiling tracks the new class so the PID limit + anti-windup stay correct
  altApplyClassScales();  // alt-health Vbus cell size tracks the class (stored points keep raw coords)
}

int clamp_i(int x, int lo, int hi) {
  if (x < lo) return lo;
  if (x > hi) return hi;
  return x;
}

float slew_limit_f(float prev, float target, float rise_per_s, float fall_per_s, float dt_sec) {
  // Negated form, not (dt_sec <= 0): every comparison with NaN is false, so the plain form would let a
  // NaN dt fall through and return target completely unslewed.
  if (!(dt_sec > 0.0f)) return prev;
  float max_rise = rise_per_s * dt_sec;
  float max_fall = fall_per_s * dt_sec;
  float delta = target - prev;

  if (delta > max_rise) {
    return prev + max_rise;
  }
  if (delta < -max_fall) {
    return prev - max_fall;
  }
  return target;
}

// ==================== PWM OUTPUT ====================

// The only caller of setDutyPercent(), except the hard-cut paths.
void apply_pwm_float(float dutyPercent) {
  setDutyPercent(dutyPercent);
}



// ==================== ACTUATOR GOVERNOR ====================
// ALL duty changes flow through here except hard-cut: clamps → slew (or bypass) → PWM write. effectiveMinDuty 0.0 bypasses
// the floor (shutdown ramps, sysID, active protection clamp, MANUAL); writeToHardware false when GPIO4 is already LOW.
float governor_apply(float lastAppliedDuty, float requestDutyFloat, int gmode,
                     float effectiveMinDuty, bool writeToHardware, float dtSec) {
  // clamp_f and slew_limit_f pass NaN through: a NaN applied duty makes the next slew step unlimited
  if (!isfinite(lastAppliedDuty)) lastAppliedDuty = 0.0f;
  if (!isfinite(requestDutyFloat)) requestDutyFloat = lastAppliedDuty;

  float hardMin = MinDuty;
  float hardMax = ccDutyCeiling();

  // Dynamic bounds (RPM minimum, etc.)
  float dynMin = effectiveMinDuty;
  float dynMax = ccDutyCeiling();

  // Effective bounds: most restrictive
  // For shutdown (effectiveMinDuty=0), allow going below MinDuty
  float finalMin, finalMax;
  if (effectiveMinDuty < 0.01f) {  // Essentially zero (shutdown mode)
    finalMin = 0.0f;
    finalMax = (dynMax < hardMax) ? dynMax : hardMax;
  } else {
    finalMin = (dynMin > hardMin) ? dynMin : hardMin;
    finalMax = (dynMax < hardMax) ? dynMax : hardMax;
  }
  finalMin = fminf(finalMin, boardDutyCeiling());   // the board ceilings win over every floor

  if (finalMax < finalMin) {
    finalMax = finalMin;
  }

  g_fieldDutyFloor = finalMin;   // what was really enforced, for CSV2_fieldDutyFloor — 0 on the bypass paths

  float requestClamped = clamp_f(requestDutyFloat, finalMin, finalMax);

  float nextFloat;
  switch (gmode) {
    case GOV_HOLD:
      nextFloat = lastAppliedDuty;
      break;

    case GOV_BYPASS_SLEW:
      nextFloat = requestClamped;
      break;

    case GOV_NORMAL_SLEW:
    default:
      // DutyRampRate is stored in REAL %/s for this bus (rescaled in place on class/winding change), so used as-is.
      if (!dutySlewEnable && !g_autoTestActive) {
        // Duty slew disabled (Test Limiters): instant steps for A/B study; removes the coupling-cap transient protection.
        // Inert during an automated test so a user's OFF can't strip slew out of a measurement.
        nextFloat = requestClamped;
      } else {
        nextFloat = slew_limit_f(lastAppliedDuty, requestClamped,
                                 DutyRampRate, DutyRampRate, dtSec);
      }
      // Re-clamp after slew (in case of edge effects)
      nextFloat = clamp_f(nextFloat, finalMin, finalMax);
      break;
  }

  float nextDuty = clamp_f(nextFloat, finalMin, finalMax);

  if (writeToHardware) {
    apply_pwm_float(nextDuty);
  }

  return nextDuty;
}

// ==================== MODE ENTRY FUNCTIONS (BUMPLESS TRANSFER) ====================
void enter_sys_off() {
  sysMode = SYS_MODE_OFF;
  govMode = GOV_NORMAL_SLEW;

  currentPID.SetMode(MANUAL);
  pidOutput = (double)lastAppliedDuty;
  currentPID.ResetIntegratorTo((double)lastAppliedDuty);
}

// Adaptive fast-OV lockout ladder: an isolated transient re-arms fast so the G2 clamp + refill own the retry; a persistent
// cause escalates so cut/restart churn stays bounded. 3 fires per tier, 0.5-8 s doubling, 10 s cap; 60 s clean resets. RAM-only.
uint32_t nextFastOvLockoutMs(uint32_t nowMs) {
  static uint32_t lastFireMs = 0;
  static uint8_t fireCount = 0;
  if (lastFireMs != 0 && (uint32_t)(nowMs - lastFireMs) > 60000u) fireCount = 0;
  lastFireMs = nowMs;
  uint32_t ms = 500u << (fireCount / 3u);
  if (fireCount < 15u) fireCount++;  // saturate so the shift can't run away
  return (ms > 10000u) ? 10000u : ms;
}

// Tach-lie lockout ladder: 15/30/60 s, 120 s cap; 10 min clean resets. Long because each retry burns ~3 s of field drain.
// The tier IS the back-off: buildTickSnapshot releases early ONLY on a blanked+held rev-up, never on a signal drop. RAM-only.
uint32_t nextTachLieLockoutMs(uint32_t nowMs) {
  static uint32_t lastFireMs = 0;
  static uint8_t fireCount = 0;
  if (lastFireMs != 0 && (uint32_t)(nowMs - lastFireMs) > 600000u) fireCount = 0;
  lastFireMs = nowMs;
  uint32_t ms = 15000u << fireCount;
  if (fireCount < 3u) fireCount++;
  return (ms > 120000u) ? 120000u : ms;
}

// Shared state reset for every immediate GPIO4 cut path, so all leave identical state.
void applyImmediateCut(const TickSnapshot &tick, FieldEventReason reason, bool rpmTriggered) {
  bool alreadyCut = gpio4IsLow;
  const float preCutDuty = dutyCycle;    // zeroed below the alreadyCut return; the abort latch wants the pre-cut command
  g_fieldEventReason = (uint8_t)reason;  // authoritative cut cause for the banner OFF-reason telemetry
  digitalWrite(4, LOW);
  gpio4IsLow = true;
  // Same stale-tracker/clamp clear as runShutdownPath: a one-sample stale cut with no lockout is gone the next
  // pass, so runShutdownPath never runs and the CV re-entry would fire the stale protection-release reseed.
  lastVoltageControlActive = false;
  clearStaleProtClamp();
  // Opens the tach-corruption mask window (buildTickSnapshot), NOT for RPM-derived cuts: masking the gate that just fired would
  // re-energize the field on a real stall or a lying tach. rpmTriggered covers a cut triggered by the tach but reported as
  // something else (spin-down finisher reporting master switch / BMS / solar pause): the stamp outlives the cut, so re-enabling
  // inside FIELDCUT_RPM_GRACE_MS would mask both RPM gates on a stopped engine. Only hardCutRpmGrace reads g_lastFieldCutMs.
  if (!rpmTriggered && reason != REASON_RPM_TOO_LOW && reason != REASON_TACH_IMPLAUSIBLE) g_lastFieldCutMs = tick.nowMs;
  // Fast OV and timed OV tiers share one adaptive lockout ladder so a persistent cause escalates whichever rung catches it.
  // applyImmediateCut returns before runShutdownPath's lockout-arm runs, so arm it here.
  if ((reason == REASON_FAST_OVERVOLTAGE || reason == REASON_OV_TIER_LOW || reason == REASON_OV_TIER_MID)
      && fieldCollapseTime == 0) {
    fieldCollapseTime = tick.nowMs;
    activeCollapseDelay = nextFastOvLockoutMs(tick.nowMs);
    queueConsoleMessageF("%s lockout %.1fs (adaptive: 0.5s-10s ladder, resets after 60s clean)",
                         (reason == REASON_FAST_OVERVOLTAGE) ? "Fast-OV" : "Timed-OV",
                         activeCollapseDelay / 1000.0f);
  }
  // Tach-lie: arm the escalating lockout and latch the trip RPM so the early release can tell a real rev-up from the
  // phantom's noise band. dutyCycle still holds the pre-cut value here.
  if (reason == REASON_TACH_IMPLAUSIBLE && fieldCollapseTime == 0) {
    fieldCollapseTime = tick.nowMs;
    activeCollapseDelay = nextTachLieLockoutMs(tick.nowMs);
    // Engine speed ignored: the trip RPM is meaningless, and the rev-up release (past 1.5x trip RPM) must stay out of reach if
    // the owner un-ignores engine speed mid-lockout; an unreachable value leaves tier expiry as the only release.
    tachLieTripRpm = tick.ignoreRPM ? 1e9f : RPM;
    tachLieLockoutArmed = true;
    if (tick.ignoreRPM)
      queueConsoleMessageF("Field commanded but no alternator output: %.0f%% field, %.1fA out, engine speed ignored - engine stopped with the key on, dead alternator, or open field drive (ON/OFF switch off, field wire loose, gate-drive/Q3 fault). Lockout %.1fs, then one more try; switch the alternator off if the engine is stopped",
                           dutyCycle, MeasuredAmps, activeCollapseDelay / 1000.0f);
    else
      queueConsoleMessageF("Field commanded but no alternator output: %d RPM, %.0f%% field, %.1fA out - open field drive (ON/OFF switch off, field wire loose, gate-drive/Q3 fault), dead alternator, or false RPM (tach noise). Lockout %.1fs (sustained rev-up releases early)",
                           (int)RPM, dutyCycle, MeasuredAmps, activeCollapseDelay / 1000.0f);
  }
  if (alreadyCut) return;  // hardware already cut — skip logging, don't spam
  apply_pwm_float(0.0f);
  lastAppliedDuty = 0.0f;
  dutyCycle = 0.0f;
  // A stop the user or an external authority ASKED for is not a fault (FAULT here made the arbiter flip back to OFF next tick,
  // printing "Charging disabled" twice). Listed explicitly, not keyed on tick.chargingEnabled: a hardware OV/OC outranks the
  // disabled check, can arrive with charging already off, and must still register as a fault.
  sysMode = (reason == REASON_CHARGING_DISABLED || reason == REASON_CHARGE_COMPLETE_IDLE || reason == REASON_BMS_DISABLED
             || reason == REASON_SOLAR_PAUSE) ? SYS_MODE_OFF : SYS_MODE_FAULT;
  updateFieldTelemetry(0.0f, tick.currentBatteryVoltage);
  fieldActiveStatus = 0;
  currentPID.SetMode(MANUAL);
  pidOutput = 0.0;
  currentPID.ResetIntegratorTo(0.0);
  setpointLimited = 0.0f;
  ctrlLimiter = 0;
  shutdownPhase = SHUTDOWN_PHASE_4;
  shutdownPhaseEntryMs = tick.nowMs;
  prevMode = MODE_CRITICAL_RAMP;
  const char *caughtBy = inaOvervoltageLatched ? "HW ALERT pin fired first; SW latch active"
                                               : "SW caught first (no HW alert)";
  queueConsoleMessageF("Field cut immediately: %s | %s | ADS=%.2fV INA=%.2fV D=%.3fV",
                       reasonToString(reason), caughtBy, BatteryV, IBV, fabsf(BatteryV - IBV));
  // If a plant delay test is running, abort it — results from an interrupted test are invalid
  if (systemIDActive != 0 && !systemIDAbortRequested) {
    systemIDAbortReason = (uint8_t)reason;
    systemIDAbortPhase = systemIDActive;
    systemIDAbortRequested = true;
    queueConsoleMessageF("SystemID: ABORTED — protection fired (%s) during phase %d — run the test again",
                         reasonToString(reason), (int)systemIDActive);
  }
  // Commissioning field-curve / knee ramp: flag a clean abort and latch why for the dashboard. fieldCurve_tick resets its phase
  // only after the lockout clears, so a second cut can land meanwhile: first cause wins, a later one goes to the follow-on slot.
  if (fieldCurveActive != 0) {
    if (!fieldCurveAbortRequested) {
      fieldCurveAbortReason = (uint8_t)reason;
      fieldCurveAbortVolts = tick.currentBatteryVoltage;
      fieldCurveAbortDuty = preCutDuty;
      strncpy(fieldCurveAbortMsg, reasonToString(reason), sizeof(fieldCurveAbortMsg) - 1);
      fieldCurveAbortMsg[sizeof(fieldCurveAbortMsg) - 1] = '\0';
    } else if (fieldCurveAbortFollowOn == 0 && (uint8_t)reason != fieldCurveAbortReason) {
      fieldCurveAbortFollowOn = (uint8_t)reason;
    }
    fieldCurveAbortRequested = true;
  }
  // Gate-tuning field sweep rides the same override path. Ending it at the cut is the point: the rows
  // logged past an over-voltage cut are the protection's response, not the field-vs-output relation
  // the sweep is measuring, and at a high state of charge the cut is the EXPECTED way a sweep ends.
  if (altSweepActive != 0 && !altSweepAbortRequested) {
    altSweepAbortRequested = true;
    altSweepActive = 0;   // drop the banner and the test-busy interlock NOW, not whenever the lockout
                          // clears and altSweep_tick is next reached — the run is over either way
    altSweepLastEndMs = tick.nowMs;
    queueConsoleMessageF("Field sweep: ABORTED — protection fired (%s) at %.1f%% field",
                         reasonToString(reason), preCutDuty);
  }
  // Field de-energize τ test rides the same override path — latch the abort identically. The tick that
  // consumes the flag is gated out during the fault lockout, so without the latch the test would resume
  // from a stale phase (stale phaseStartMs) once the lockout clears.
  if (fieldCutActive != 0) {
    if (!fieldCutAbortRequested) {   // first cause wins (same rationale as the field-curve latch above)
      strncpy(fieldCutAbortMsg, reasonToString(reason), sizeof(fieldCutAbortMsg) - 1);
      fieldCutAbortMsg[sizeof(fieldCutAbortMsg) - 1] = '\0';
    }
    fieldCutAbortRequested = true;
  }
  // Charge health calibration rides the same override path — latch identically. A run interrupted by
  // a protection cut has a corrupted swing in it, so it is discarded rather than fitted.
  if (chcActive != 0) {
    if (!chcAbortRequested) {
      strncpy(chcAbortMsg, reasonToString(reason), sizeof(chcAbortMsg) - 1);
      chcAbortMsg[sizeof(chcAbortMsg) - 1] = '\0';
    }
    chcAbortRequested = true;
  }
  // Closed-loop verify sine sweep (Tuning→Current auto-sweep) rides the same latch. Without it the
  // sweep ran on through the cut + lockout and graded the corrupted points as a duty-rail result.
  if (tuningSweepActive && tuningSweepAbortReason == 0) {
    tuningSweepAbortReason = (uint8_t)reason;
    tuningSweepAbortVolts = tick.currentBatteryVoltage;
    tuningSweepAbortDuty = preCutDuty;
    queueConsoleMessageF("Tuning sine sweep: ABORTED — protection fired (%s) — results discarded",
                         reasonToString(reason));
  }
  // CV plant fit: abort directly (state resets at start — no stale-phase latch; cvpfState=3 blocks a
  // partial-buffer fit).
  if (cvPlantFitActive) {
    cvpfAbort("a protection cut in during the test — re-run once charging is steady");
  }
  // DCIR runs with the soft OV layers off; left running it re-drives the same step into this cut after the lockout
  if (batteryHealthTestActive) {
    bhAbort("a protection cut in during the test — re-run once charging is steady");
  }
  g_sysIDResume.valid = false;   // an aborted test's completion must not restore its minutes-old pre-test setpoint
}

void enter_sys_fault() {
  sysMode = SYS_MODE_FAULT;
  // govMode set by caller based on fault severity

  currentPID.SetMode(MANUAL);
  pidOutput = (double)lastAppliedDuty;
  currentPID.ResetIntegratorTo((double)lastAppliedDuty);
}

void enter_sys_manual() {
  sysMode = SYS_MODE_MANUAL;
  govMode = GOV_NORMAL_SLEW;  // Manual still respects slew for tach protection

  currentPID.SetMode(MANUAL);
  pidOutput = (double)lastAppliedDuty;
  currentPID.ResetIntegratorTo((double)lastAppliedDuty);
}

void enter_sys_auto() {
  sysMode = SYS_MODE_AUTO;
  govMode = GOV_NORMAL_SLEW;

  applyCcOutputLimits();  // [MinDuty, voltage-scaled duty ceiling] so PID anti-windup respects the ceiling
  currentPID.SetSampleTime(100);
  recomputeCcGains();  // apply voltage-normalized PidK*_active
  currentPID.SetTrackingGain(PIDTrackingGain);
  currentPID.SetMode(AUTOMATIC);

  pidOutput = (double)lastAppliedDuty;
  currentPID.ResetIntegratorTo((double)lastAppliedDuty);

  // Always restart from Bulk: a full battery's voltage rises fast and updateChargingStage() jumps to Absorption/Float within
  // seconds. Do not skip Bulk on entry voltage: terminal voltage under load is not a reliable SoC indicator (premature float).

  uint32_t now = millis();

  bulkVoltageHoldTimer = 0;
  absorptionTailTimer = 0;
  rebulkTimer = 0;

  inBulkStage = true;
  inAbsorptionStage = false;
  inIdleStage = false;
  floatStartTime = 0;
  queueConsoleMessageF(
    "enter_sys_auto: starting in BULK (%.2fV src=%s)", getChargeDecisionVoltage(), chargeDecisionVoltageSourceName());

  // Always seed from alternator current regardless of MaintainMode
  float actualCurrent = max(0.0f, getTargetAmps());
  setpointLimited = actualCurrent;
  setpointInitialized = true;
}

// ==================== CONTROL LOOP SUB-PATH HELPERS ====================
// Each function handles one complete early-exit case from AdjustFieldLearnMode.
// None of these touch the CV integrator (cv_I), bumpless tracker, or iExcess state.

// runReducedSensorControl: Run Without Failed Sensors tiers 2-4 (SENSOR_FAULTS_AND_OPTIONAL_SENSORS_SPEC.md §4.3-§4.5), reached
// only from the normal AUTO branch with GPIO4 enabled, after every unsuppressed reason took its cut path, so nothing here fights
// a protection. Tier 2: voltage-only, regulating half a low-tier margin under target (builds slowly, sheds fast) so the timed OV
// tiers stay the backstop, not the regulator. Tier 3: fixed LimpDutyPct gated by the second voltage vs Bulk. Tier 4: fixed duty.
// slewChargingVoltageTarget: THE charge-target slew, shared by the AUTO CV path and tier 2 so they can never drift. The CV error
// and the RELATIVE OV protections read ChargingVoltageTarget, so this paces only the trip points; absolute backstops are unaffected.
static void slewChargingVoltageTarget(float req, bool snap, float rampUp, float rampDn, bool busPaced, float actualDtSec) {
  if (snap) {
    ChargingVoltageTarget = req;            // snap (disabled / idle / CV entry)
  } else if (req > ChargingVoltageTarget) {
    float step = (rampUp > 0.0f) ? (rampUp * actualDtSec) : 1.0e9f;
    ChargingVoltageTarget = fminf(req, ChargingVoltageTarget + step);
  } else if (req < ChargingVoltageTarget) {
    // Snap-to-proximity: collapse the inert dead band in one tick, never below req nor IBV+margin, so measured stays under
    // target and the relative OV protections can't trip; vTgtRampDn owns the final approach. Self-arming: only while target >>
    // measured, so it stays out of the delicate absorption→float-on-a-full-battery case.
    float fastFloor = IBV + 0.2f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f);
    if (ChargingVoltageTarget > fastFloor) {
      ChargingVoltageTarget = fmaxf(req, fastFloor);
    }
    float step = (rampDn > 0.0f) ? (rampDn * actualDtSec) : 1.0e9f;
    float tgtDn = ChargingVoltageTarget - step;
    // Bus-paced floor: a fixed glide outruns delivery on a soft plant and opens the gap to the G2 margin, so hold the descent
    // while the bus lags more than the lead. Pace on getFiltV, never g_ovIbvFilt (ripple troughs on the comparator's own signal
    // ratchet the floor down). Tests keep their trajectories, except the stress-test settle: CvStressDropV equals OvMeasMarginV,
    // so an unpaced landing puts the settled bus exactly on the G2 line.
    if (busPaced) {
      float paceFloor = getFiltV() - CV_TGT_PACE_LEAD_V * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f);
      tgtDn = fminf(fmaxf(tgtDn, paceFloor), ChargingVoltageTarget);
    }
    ChargingVoltageTarget = fmaxf(req, tgtDn);
  }
}

// Tier-2 charge target (§4.3): the same request and slew as the normal CV path, so a target drop (absorption -> float) cannot
// outrun the bus and walk the timed OV tiers up their lockout ladder. snap only arriving from tier 3/4 (no valid target there).
static void tier2ChargeTarget(float actualDtSec, bool snap) {
  float req = ChargingVoltageTargetReq;
  if (TargetVoltageMode == 1) req = TargetVoltageSetpoint;
  if (MaintainMode == 1) req = BulkVoltage;
  if (dvccEn == 1 && dvccState == 3 /*FOLLOWING*/ && !cxOwnsBatteryNow() && !isnan(dvccCvlV) && dvccCvlV < req) req = dvccCvlV;
  ChargingVoltageTargetReq = req;
  slewChargingVoltageTarget(req, snap || vTgtRampEnable == 0, vTgtRampUp, vTgtRampDn, true, actualDtSec);
  voltageTargetSlewed = ChargingVoltageTarget;
}

static bool s_tierCapHold = false;   // tier 3 cap hold; survives 3<->4 transitions, cleared on leaving tiers 3/4
static uint8_t s_reducedLastTier = 0; // 0 once the normal path takes back control, so a later tier 2 never inherits a stale "from tier 3/4" snap
void reducedCtlLeaveTiers34() { s_tierCapHold = false; s_reducedLastTier = 0; }

void runReducedSensorControl(const TickSnapshot &tick, FieldEventReason reason, float actualDtSec) {
  static float tierDuty = 0.0f;
  static uint32_t capDropMs = 0;
  static uint32_t lastReportMs = 0;

  const bool entering = (tick.limpTier != s_reducedLastTier);
  const bool fromTier34 = entering && (s_reducedLastTier >= 3);
  // Bumpless: a tier change, or any other path having moved the duty (a cut zeroes it), restarts the
  // tier's own integrator from where the field actually is.
  if (entering || fabsf(tierDuty - lastAppliedDuty) > 1.0f) tierDuty = lastAppliedDuty;
  if (tick.limpTier < 3) s_tierCapHold = false;
  s_reducedLastTier = tick.limpTier;

  uTargetAmps = 0;
  setpointLimited = 0.0f;
  ctrlLimiter = 0;
  cvWindDownActive = false;
  clearStaleProtClamp();
  voltageControlActive = (tick.limpTier == 2);
  lastVoltageControlActive = voltageControlActive;

  float request;
  float floorDuty;
  int gov;
  if (tick.limpTier == 2) {
    tier2ChargeTarget(actualDtSec, fromTier34);
    // Regulate half a low-tier margin UNDER the target so the natural overshoot lands short of the timed OV
    // low tier line (ChargingVoltageTarget + OvTierLoMarginV, which reads the unshifted target), and shed
    // five times faster than the build: a stiff bank otherwise walks straight into the tier cut.
    float e = (voltageTargetSlewed - 0.5f * OvTierLoMarginV - tick.currentBatteryVoltage) * 12.0f / fmaxf(1.0f, (float)SYSTEM_VOLTAGE_CLASS);
    // The error above is battery-side (12/class); the duty-rate side moves the field by bus volts over winding
    // volts per duty-%, so the rate law scales by fieldScale().
    const float dScale = fieldScale();
    float rate = (e >= 0.0f) ? clamp_f(4.0f * dScale * e, 0.0f, 2.0f * dScale)
                             : clamp_f(20.0f * dScale * e, -30.0f * dScale, 0.0f);
    tierDuty = clamp_f(tierDuty + rate * actualDtSec, 0.0f, fminf(LimpDutyPct, ccDutyCeiling()));
    request = tierDuty;
    floorDuty = fminf(tick.rpmMinDuty, LimpDutyPct);   // keep-alive floor as in normal AUTO (0 while engine speed is ignored), never above the owner's cap (§4.3)
    gov = GOV_BYPASS_SLEW;
  } else {
    if (tick.limpTier == 3) {
      float capV = tick.bulkVoltage;
      float hystV = 0.3f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f);
      if (!s_tierCapHold && tick.currentBatteryVoltage > capV) { s_tierCapHold = true; capDropMs = tick.nowMs; }
      else if (s_tierCapHold && tick.currentBatteryVoltage < capV - hystV && (uint32_t)(tick.nowMs - capDropMs) >= 5000UL) s_tierCapHold = false;
      request = s_tierCapHold ? 0.0f : LimpDutyPct;
    } else {
      request = LimpDutyPct;       // tier 4: no reading to judge the cap by; the hold is kept for a return to tier 3
    }
    tierDuty = request;
    floorDuty = 0.0f;              // a fixed duty is the owner's number; no keep-alive floor under it
    gov = GOV_NORMAL_SLEW;         // DutyRampRate eases the cap steps (LM2907 coupling-cap protection)
  }

  float applied = governor_apply(lastAppliedDuty, request, gov, floorDuty, true, actualDtSec);
  currentPID.SetMode(MANUAL);
  currentPID.ResetIntegratorTo((double)applied);
  pidOutput = (double)applied;
  lastAppliedDuty = applied;
  dutyCycle = applied;
  pidLog_dutyRequest = request;
  pidLog_dutyApplied = applied;

  updateFieldTelemetry(applied, tick.currentBatteryVoltage);
  fieldActiveStatus = (applied > 0.01f) ? 1 : 0;
  chargeStageDisplay = getChargeStageDisplayCode();
  reportFieldModeEvent(tick.nowMs, MODE_NORMAL_AUTO, reason, tick, gpio4IsLow, dutyCycle);

  if ((uint32_t)(tick.nowMs - lastReportMs) >= 30000UL) {
    lastReportMs = tick.nowMs;
    if (tick.limpTier == 2)
      queueConsoleMessageF("Running without alternator current: voltage-only regulation, field %.1f%% (cap %.0f%%), bus %.2fV target %.2fV",
                           applied, LimpDutyPct, tick.currentBatteryVoltage, voltageTargetSlewed);
    else if (tick.limpTier == 3)
      queueConsoleMessageF("Running without the main battery voltage: fixed field %.1f%% capped by the second reading %.2fV (cap %.2fV)%s",
                           applied, tick.currentBatteryVoltage, tick.bulkVoltage, s_tierCapHold ? " - holding at 0%" : "");
    else
      queueConsoleMessageF("Fixed field %.1f%% with every sensor ignored - only the INA228 hardware over-voltage alert protects the bank", applied);
  }
}

// Fault / non-normal mode: shutdown ramp (phases 1→3→4→GPIO4 cut); never reaches the CV path, caller handles prevMode.
void runShutdownPath(const TickSnapshot &tick, FieldControlMode mode, FieldEventReason reason,
                     float actualDtSec, bool exitingNormal) {
  voltageControlActive = false;
  lastVoltageControlActive = false;  // caller returns before the CV block's tracker line — stale TRUE would skip the re-entry bumpless seed on re-enable
  cvWindDownActive = false;          // same early-exit rule — a stale wind-down cap must not survive a charge-disable gap
  clearStaleProtClamp();

  // GPIO38 driven solely by CheckAlarms — do not write here

  uTargetAmps = 0;
  setpointLimited = 0.0f;
  ctrlLimiter = 0;

  if (exitingNormal) {
    shutdownPhase = SHUTDOWN_PHASE_1;
    shutdownPhaseEntryMs = tick.nowMs;
  }
  if (shutdownPhase == SHUTDOWN_PHASE_NONE) {
    shutdownPhase = SHUTDOWN_PHASE_1;
    shutdownPhaseEntryMs = tick.nowMs;
  }

  float effectiveMin = 0.0f;
  float dutyRequest = 0.0f;

  // Critical faults skip the slow ramp entirely. REASON_VOLTAGE2_STALE takes the phased ramp only while the
  // main reading supervises it (spec §10.2); in tier 3 the second reading was the only voltage, so it goes
  // straight to zero like the other unsupervised cases.
  bool isCriticalFault = (reason == REASON_VOLTAGE_DISAGREE_CRITICAL || reason == REASON_VOLTAGE_IMPLAUSIBLE
                          || reason == REASON_VOLTAGE_STALE || reason == REASON_MULTI_SENSOR_FAULT
                          || (reason == REASON_VOLTAGE2_STALE && tick.ignoreBattVMain));

  if (isCriticalFault) {
    shutdownPhase = SHUTDOWN_PHASE_4;
    effectiveMin = 0.0f;
    dutyRequest = 0.0f;
  } else {
    // Phase 1: ramp to rpmMinDuty at DutyRampRate. A duty already below the floor (MANUAL low duty, fast-OV/G2
    // clamp) holds where it is: a shutdown never raises the field.
    if (shutdownPhase == SHUTDOWN_PHASE_1) {
      effectiveMin = 0.0f;
      dutyRequest = fminf(lastAppliedDuty, tick.rpmMinDuty);
      if (lastAppliedDuty <= tick.rpmMinDuty + 0.1f) {
        if (ShutdownPhase2HoldMs > 0) {
          if (shutdownPhase2EntryMs == 0) shutdownPhase2EntryMs = tick.nowMs;
          if (tick.nowMs - shutdownPhase2EntryMs >= ShutdownPhase2HoldMs) {
            shutdownPhase = SHUTDOWN_PHASE_3;
            shutdownPhaseEntryMs = tick.nowMs;
            shutdownPhase2EntryMs = 0;
          }
        } else {
          shutdownPhase = SHUTDOWN_PHASE_3;
          shutdownPhaseEntryMs = tick.nowMs;
          shutdownPhase2EntryMs = 0;
        }
      }
    }

    // Phase 3: ramp from rpmMinDuty to 0. Below the output point the stator swing shrinks with the field, and the LM2907's 20 uF
    // coupling cap re-centers through 100k in ~2.1 s: a faster-shrinking swing leaves pin 1 off-center (false zero RPM ~4.6 s).
    // Proportional decay keeps steps a fixed fraction of what is left; DutySlowRampRate is the tail floor. Above the output point
    // the battery pins the swing, so a lower live floor and real output while off both shed at DutyRampRate.
    if (shutdownPhase == SHUTDOWN_PHASE_3) {
      static uint32_t outputSinceMs = 0;
      static uint32_t outputForEntryMs = 0;  // a dwell timer left running by an earlier ramp must not count toward this one
      if (outputForEntryMs != shutdownPhaseEntryMs) { outputForEntryMs = shutdownPhaseEntryMs; outputSinceMs = 0; }
      bool altAmpsTrusted = !tick.ignoreAltCurrent && !tick.limpHome && !tick.currentDataStale;
      if (altAmpsTrusted && MeasuredAmps > NoOutputAmps) {
        if (outputSinceMs == 0) outputSinceMs = tick.nowMs;
      } else {
        outputSinceMs = 0;
      }
      bool outputWhileOff = outputSinceMs != 0 && (uint32_t)(tick.nowMs - outputSinceMs) >= OUTPUT_WHILE_OFF_DWELL_MS;

      float rate = (ShutdownRampTauS > 0.0f) ? fmaxf(lastAppliedDuty / ShutdownRampTauS, DutySlowRampRate) : DutySlowRampRate;
      if (outputWhileOff) rate = fmaxf(rate, DutyRampRate);
      float slowDuty = slew_limit_f(lastAppliedDuty, 0.0f, rate, rate, actualDtSec);
      if (tick.rpmMinDuty > 0.0f && tick.rpmMinDuty < slowDuty) {
        float fastDuty = slew_limit_f(lastAppliedDuty, 0.0f, DutyRampRate, DutyRampRate, actualDtSec);
        slowDuty = fmaxf(tick.rpmMinDuty, fastDuty);
      }
      effectiveMin = 0.0f;
      dutyRequest = slowDuty;
      if (lastAppliedDuty <= 0.01f) {
        shutdownPhase = SHUTDOWN_PHASE_4;
        shutdownPhaseEntryMs = tick.nowMs;
      }
    }

    // Phase 4: hold at 0, wait for flux collapse, then cut GPIO4
    if (shutdownPhase == SHUTDOWN_PHASE_4) {
      effectiveMin = 0.0f;
      dutyRequest = 0.0f;
    }
  }

  bool writeToHardware = !gpio4IsLow;
  // Phase 3: slew already applied above — bypass governor's own slew, but keep clamping.
  GovernorMode shutdownGovMode = (shutdownPhase == SHUTDOWN_PHASE_3) ? GOV_BYPASS_SLEW : govMode;

  float dutyNewFloat = governor_apply(lastAppliedDuty, dutyRequest, shutdownGovMode,
                                      effectiveMin, writeToHardware, actualDtSec);
  currentPID.ResetIntegratorTo((double)dutyNewFloat);
  pidOutput = (double)dutyNewFloat;

  if (writeToHardware) lastAppliedDuty = dutyNewFloat;
  dutyCycle = dutyNewFloat;

  if (!gpio4IsLow) {
    if (shutdownPhase == SHUTDOWN_PHASE_4 && shouldCutGPIO4AfterSettle(reason, tick.nowMs, dutyNewFloat)) {
      digitalWrite(4, LOW);
      gpio4IsLow = true;
      queueConsoleMessageF("Field disabled: %s | ADS=%.2fV INA=%.2fV D=%.3fV",
                           reasonToString(reason), BatteryV, IBV, fabsf(BatteryV - IBV));
    } else {
      digitalWrite(4, HIGH);
    }
  }

  updateFieldTelemetry(dutyCycle, tick.currentBatteryVoltage);

  if (gpio4IsLow) {
    fieldActiveStatus = 0;
  } else if (shutdownPhase == SHUTDOWN_PHASE_1 || shutdownPhase == SHUTDOWN_PHASE_3) {
    fieldActiveStatus = 2;
  } else if (lastAppliedDuty > 0.01f) {
    fieldActiveStatus = 1;
  } else {
    fieldActiveStatus = 0;
  }
  chargeStageDisplay = getChargeStageDisplayCode();

  if ((mode == MODE_CRITICAL_RAMP || mode == MODE_WARNING_RAMP_AND_LOCKOUT) && fieldCollapseTime == 0) {
    fieldCollapseTime = tick.nowMs;
    activeCollapseDelay = (reason == REASON_RPM_TOO_LOW)       ? RPM_RECOVERY_DELAY
                        : (reason == REASON_FAST_OVERVOLTAGE
                           || reason == REASON_OV_TIER_LOW
                           || reason == REASON_OV_TIER_MID)    ? nextFastOvLockoutMs(tick.nowMs)
                        : (reason == REASON_TACH_IMPLAUSIBLE)  ? nextTachLieLockoutMs(tick.nowMs)
                                                               : FIELD_COLLAPSE_DELAY;
    if (reason == REASON_TACH_IMPLAUSIBLE) { tachLieTripRpm = tick.ignoreRPM ? 1e9f : RPM; tachLieLockoutArmed = true; }   // unreachable trip RPM while engine speed is ignored (see applyImmediateCut)
    // Spam guard: with the engine off the temp feed goes stale and this cut/restart cycle repeats every cooldown (~2 s). Announce
    // a new episode at once, then heartbeat at 60 s / 5 min / 30 min (a fixed 60 s filled the 200-line /consolehist.txt in ~100
    // min). Console text only; nothing here feeds fieldCollapseTime or activeCollapseDelay.
    static FieldEventReason lastCollapseReason = REASON_NONE;
    static uint32_t lastCollapseMsgMs = 0;
    static uint32_t lastCollapseCutMs = 0;
    static uint32_t collapseLoopCount = 0;
    static uint8_t  collapseHeartbeats = 0;   // "Still cycling" lines emitted for the current episode
    // A gap this long between cuts means the cycle actually stopped, so the next cut is a new episode
    // and is announced in full even when the reason is unchanged.
    bool newEpisode = (reason != lastCollapseReason)
                      || (uint32_t)(tick.nowMs - lastCollapseCutMs) >= 60000;
    uint32_t collapseHeartbeatMs = (collapseHeartbeats == 0) ? 60000UL
                                 : (collapseHeartbeats == 1) ? 300000UL
                                                             : 1800000UL;
    collapseLoopCount++;
    if (newEpisode || (uint32_t)(tick.nowMs - lastCollapseMsgMs) >= collapseHeartbeatMs) {
      if (newEpisode) {
        queueConsoleMessageF("Charging stopped (%s) - %.1fs cooldown before restart",
                             reasonToString(reason), activeCollapseDelay / 1000.0f);
        collapseHeartbeats = 0;
      } else {
        queueConsoleMessageF("Still cycling on %s (%lu cuts) - %.1fs cooldown, suppressing repeats",
                             reasonToString(reason), (unsigned long)collapseLoopCount,
                             activeCollapseDelay / 1000.0f);
        if (collapseHeartbeats < 2) collapseHeartbeats++;
      }
      lastCollapseMsgMs = tick.nowMs;
      collapseLoopCount = 0;
    }
    lastCollapseReason = reason;
    lastCollapseCutMs = tick.nowMs;
  }

  reportFieldModeEvent(tick.nowMs, mode, reason, tick, gpio4IsLow, dutyCycle);
}

// runCommissionIdle: "rest" hold between commissioning steps at a flat winding-scaled floor (COMMISSION_REST_FLOOR_PCT ×
// fieldScale()), eased in slowly. Do NOT max() in rpmMinDuty: that table is what commissioning is (re)learning, and a held onset
// duty can exceed the lower onset after RPM rises (output blip). GPIO4 stays ENABLED: a little field keeps the RPM pickup alive.
// Bypasses the AUTO/stage machinery; a real fault never lands here. The current-sensor zero hold (REASON_ALTZERO_MEASURE) rides
// this path with a ZERO target; buildTickSnapshot masks the RPM gates for the hold and its re-bias tail.
void runCommissionIdle(const TickSnapshot &tick, FieldEventReason reason, float actualDtSec) {
  voltageControlActive = false;
  lastVoltageControlActive = false;  // same stale-tracker clear as runShutdownPath (the wizard's test-FAIL→rest→retry flow double-fires without it)
  cvWindDownActive = false;          // same early-exit rule — a stale wind-down cap must not survive a commission rest
  clearStaleProtClamp();
  uTargetAmps = 0;
  setpointLimited = 0.0f;
  ctrlLimiter = 0;
  shutdownPhase = SHUTDOWN_PHASE_NONE;   // so a later real shutdown starts its ramp fresh from here

  const float fScale = fieldScale();
  const float restFloor = COMMISSION_REST_FLOOR_PCT * fScale;   // 4 / 2 / 1.33 / 1 % @ 12 / 24 / 36 / 48 V on a 12 V winding
  const bool  zeroHold   = (reason == REASON_ALTZERO_MEASURE);
  const float restRamp   = (zeroHold ? ALTZERO_RAMP_PCT : COMMISSION_REST_RAMP_PCT) * fScale;   // rest: 5 / 2.5 / 1.67 / 1.25 %/s on a 12 V winding
  const float restTarget = zeroHold ? 0.0f : restFloor;

  // Re-assert the enable unconditionally: the arbiter guarantees no fault/lockout here, but a rest resumed after an immediate
  // cut enters with gpio4IsLow true, and gating on it would hold the line LOW, killing the RPM-pickup keep-alive load.
  digitalWrite(4, HIGH);
  gpio4IsLow = false;

  // Dedicated slow slew toward the target (both directions), THEN governor in bypass-slew so it only
  // clamps (duty ceiling) and writes the PWM — we already did the slewing at the rest rate.
  float slowDuty = slew_limit_f(lastAppliedDuty, restTarget, restRamp, restRamp, actualDtSec);
  float dutyNewFloat = governor_apply(lastAppliedDuty, slowDuty, GOV_BYPASS_SLEW,
                                      0.0f, true, actualDtSec);
  currentPID.ResetIntegratorTo((double)dutyNewFloat);
  pidOutput = (double)dutyNewFloat;
  lastAppliedDuty = dutyNewFloat;
  dutyCycle = dutyNewFloat;

  updateFieldTelemetry(dutyCycle, tick.currentBatteryVoltage);
  fieldActiveStatus = (dutyCycle > 0.01f) ? 1 : 0;
  chargeStageDisplay = getChargeStageDisplayCode();
  reportFieldModeEvent(tick.nowMs, MODE_COMMISSION_IDLE, reason, tick, gpio4IsLow, dutyCycle);
}

// ==================== MAIN CONTROL FUNCTION ====================

// ===== PID TUNING SCORE — helpers called from AdjustFieldLearnMode =====

// Sizing printout only; allocation and loadTuningLog() happen in setup().
void tuningScore_init() {
  Serial.printf("TuningScore: %d record slots × %u bytes = %u bytes in PSRAM\n",
                50, (unsigned)sizeof(TuningRecord),
                (unsigned)(50 * sizeof(TuningRecord)));
}

void saveTuningLog() {
  if (!tuningLog) return;
  File f = LittleFS.open("/tuninglog.bin", "w");
  if (!f) return;
  fsFreeDirty = true;
  f.write((uint8_t *)&tuningLogCount, sizeof(tuningLogCount));
  f.write((uint8_t *)&tuningLogHead, sizeof(tuningLogHead));
  f.write((uint8_t *)&tuningRunCounter, sizeof(tuningRunCounter));
  f.write((uint8_t *)tuningLog, 50 * sizeof(TuningRecord));
  f.close();
}

void loadTuningLog() {
  if (!tuningLog) return;
  File f = LittleFS.open("/tuninglog.bin", "r");
  if (!f) return;
  // Struct size is part of the wire format: discard on mismatch rather than read garbage.
  const size_t expected = sizeof(tuningLogCount) + sizeof(tuningLogHead)
                          + sizeof(tuningRunCounter) + 50 * sizeof(TuningRecord);
  if (f.size() != expected) {
    Serial.printf("TuningLog: size mismatch (%u vs %u expected) — discarding old log\n",
                  (unsigned)f.size(), (unsigned)expected);
    f.close();
    LittleFS.remove("/tuninglog.bin");
    return;
  }
  f.read((uint8_t *)&tuningLogCount, sizeof(tuningLogCount));
  f.read((uint8_t *)&tuningLogHead, sizeof(tuningLogHead));
  f.read((uint8_t *)&tuningRunCounter, sizeof(tuningRunCounter));
  f.read((uint8_t *)tuningLog, 50 * sizeof(TuningRecord));
  f.close();
  // A corrupt count/head would index past the 50-slot ring (commit writes BEFORE the %50 wrap).
  if (tuningLogCount > 50 || tuningLogHead >= 50) {
    Serial.println("TuningLog: corrupt count/head — discarding old log");
    tuningLogCount = 0;
    tuningLogHead = 0;
    LittleFS.remove("/tuninglog.bin");
    return;
  }
  Serial.printf("TuningLog: loaded %d records, counter=%d\n", tuningLogCount, tuningRunCounter);
}

// Copy a user note into a fixed 51-byte record slot, keeping only printable ASCII and
// dropping the JSON/HTML breakers (" \ < >) so the log endpoints and the table render stay safe.
void sanitizeTuningNote(char *dst, const char *src) {
  int j = 0;
  for (int i = 0; src[i] != '\0' && j < 50; i++) {
    char c = src[i];
    if (c >= 0x20 && c <= 0x7E && c != '"' && c != '\\' && c != '<' && c != '>') dst[j++] = c;
  }
  dst[j] = '\0';
}

void commitTuningRecord() {
  if (!tuningLog) return;
  if (tuningScore.activeTimeSec < 0.5f) {
    tuningScore = {};
    return;
  }
  TuningRecord rec = {};
  rec.runNumber = ++tuningRunCounter;
  rec.score = tuningScore.errorAccum / tuningScore.activeTimeSec;
  rec.activeTimeSec = tuningScore.activeTimeSec;
  rec.kp = PidKp;
  rec.ki = PidKi;
  rec.kd = PidKd;
  rec.sampleDivisor = PidSampleDivisor;
  rec.trackingGain = PIDTrackingGain;
  rec.dutyRampRate = DutyRampRate;
  rec.waveAmplitude = (int16_t)waveAmplitude;
  rec.wavePeriod = (int16_t)wavePeriod;
  rec.waveFloor = (int16_t)tuningWaveFloor;
  rec.avgRPM = (tuningScore.avgSampleCount > 0)
                 ? (tuningScore.rpmSum / tuningScore.avgSampleCount)
                 : 0.0f;
  rec.avgAltTempF = (tuningScore.tempSampleCount > 0)
                      ? (tuningScore.tempSum / tuningScore.tempSampleCount)
                      : NAN;   // no probe read during the run -> JSON null, never a 0.0F average
  rec.worstErrorA = tuningScore.worstErrorA;
  rec.battV = BatteryV;
  rec.chargeStage = getChargeStageDisplayCode();
  rec.epoch = getCurrentTimestamp();
  sanitizeTuningNote(rec.note, ccTuningNote);

  tuningLog[tuningLogHead] = rec;
  tuningLogHead = (tuningLogHead + 1) % 50;
  if (tuningLogCount < 50) tuningLogCount++;

  saveTuningLog();
  cxLedgerLogTuneRun(0);  // every committed run uploads (RAM staging only here — no flash I/O)
  queueConsoleMessageF("TuningScore: run#%d score=%.2f kp=%.3f ki=%.3f kd=%.4f t=%.1fs",
                       rec.runNumber, rec.score, rec.kp, rec.ki, rec.kd, rec.activeTimeSec);

  tuningScore = {};  // reset for next test
}

void saveCVTuningLog() {
  if (!cvTuningLog) return;
  File f = LittleFS.open("/cvtuninglog.bin", "w");
  if (!f) return;
  fsFreeDirty = true;
  f.write((uint8_t *)&cvTuningLogCount, sizeof(cvTuningLogCount));
  f.write((uint8_t *)&cvTuningLogHead, sizeof(cvTuningLogHead));
  f.write((uint8_t *)&cvTuningRunCounter, sizeof(cvTuningRunCounter));
  f.write((uint8_t *)cvTuningLog, 50 * sizeof(CVTuningRecord));
  f.close();
}

void loadCVTuningLog() {
  if (!cvTuningLog) return;
  File f = LittleFS.open("/cvtuninglog.bin", "r");
  if (!f) return;
  // Struct size is part of the wire format: discard on mismatch rather than read garbage.
  const size_t expected = sizeof(cvTuningLogCount) + sizeof(cvTuningLogHead)
                          + sizeof(cvTuningRunCounter) + 50 * sizeof(CVTuningRecord);
  if (f.size() != expected) {
    Serial.printf("CVTuningLog: size mismatch (%u vs %u expected) — discarding old log\n",
                  (unsigned)f.size(), (unsigned)expected);
    f.close();
    LittleFS.remove("/cvtuninglog.bin");
    return;
  }
  f.read((uint8_t *)&cvTuningLogCount, sizeof(cvTuningLogCount));
  f.read((uint8_t *)&cvTuningLogHead, sizeof(cvTuningLogHead));
  f.read((uint8_t *)&cvTuningRunCounter, sizeof(cvTuningRunCounter));
  f.read((uint8_t *)cvTuningLog, 50 * sizeof(CVTuningRecord));
  f.close();
  // A corrupt count/head would index past the 50-slot ring (commit writes BEFORE the %50 wrap).
  if (cvTuningLogCount > 50 || cvTuningLogHead >= 50) {
    Serial.println("CVTuningLog: corrupt count/head — discarding old log");
    cvTuningLogCount = 0;
    cvTuningLogHead = 0;
    LittleFS.remove("/cvtuninglog.bin");
    return;
  }
  Serial.printf("CVTuningLog: loaded %d records, counter=%d\n", cvTuningLogCount, cvTuningRunCounter);
}

// TODO: CV tuning scores don't record testProtectionsEnabled; protections-on scores are loop+protection composite, off are
// pure loop. Tag each record with the flag, and reconsider the fire counts in the score when protections were off (always 0).
void commitCVTuningRecord() {
  if (!cvTuningLog || cvTuningScore.scoredHighCount < 1) {
    cvTuningScore = {};
    return;
  }
  CVTuningRecord rec = {};
  float n = (float)cvTuningScore.scoredHighCount;
  rec.runNumber = ++cvTuningRunCounter;
  rec.avgSettlingTimeSec = cvTuningScore.totalSettlingTimeSec / n;
  rec.avgIntegratedOvershootVs = cvTuningScore.totalIntegratedOvershootVs / n;
  rec.worstOvershootV = cvTuningScore.worstOvershootV;
  rec.activeTimeSec = cvTuningScore.activeTimeSec;
  // ISE/T ×1000: (HIGH ISE ×cvKOvershoot above the dead band + LOW re-overshoot + LOW undershoot ×0.15 with time ramp) ÷ active
  // time, ÷ class ratio² (V²-domain integrators) so the score and its <10/<20 bands read 12V-equivalent.
  float scoreNorm = (12.0f / (float)SYSTEM_VOLTAGE_CLASS) * (12.0f / (float)SYSTEM_VOLTAGE_CLASS);
  rec.score = (rec.activeTimeSec > 0.0f)
                ? (1000.0f * scoreNorm * (cvTuningScore.totalIntegratedOvershootVs + cvTuningScore.totalLowIntOvVs + cvTuningScore.totalLowUndershootVs)
                   / rec.activeTimeSec)
                : 0.0f;
  rec.fastOvFires = cvTuningScore.fastOvFires;
  rec.iExcessFires = cvTuningScore.iExcessFires;
  rec.loadDumpFires = cvTuningScore.loadDumpFires;
  rec.hardOcFires = cvTuningScore.hardOcFires;
  rec.voltageKp = VoltageKp_active;  // gain actually in effect (Manual or Measured α/K, normalized)
  rec.voltageKi = VoltageKi_active;
  rec.setpointRiseRate = SetpointRiseRate;
  rec.setpointFallRate = SetpointFallRate;
  rec.awBleedRate = AwBleedRate;
  rec.awRecoverRate = AwRecoverRate;
  rec.awSeedProtectMs = AwSeedProtectMs;
  rec.reseedFrac = ReseedFrac;
  rec.voltageKd = VoltageKd_active;  // effective (normalized) D gain in effect, matching rec.voltageKp
  rec.kHard = KHard;
  rec.iExcessFrac = IExcessFrac;
  rec.iExcessTau = IExcessTau;
  rec.iExcessKBleed = IExcessKBleed;
  rec.loadDumpDtThresh = LoadDumpDtThresh;
  rec.loadDumpDtThresh1 = LoadDumpDtThresh1;
  rec.loadDumpDtThresh3 = LoadDumpDtThresh3;
  rec.waveAmplitudeV = cvWaveAmplitudeV;
  rec.wavePeriodSec = (uint16_t)cvWavePeriodSec;
  rec.kOvershoot = cvKOvershoot;
  rec.consecutiveReads = cvConsecutiveReads;
  rec.avgRPM = (cvTuningScore.avgSampleCount > 0) ? (cvTuningScore.rpmSum / cvTuningScore.avgSampleCount) : 0.0f;
  rec.avgAltTempF = (cvTuningScore.tempSampleCount > 0) ? (cvTuningScore.tempSum / cvTuningScore.tempSampleCount) : NAN;   // no probe -> JSON null
  rec.battVAtStart = cvTuningScore.battVAtStart;
  rec.socAtStart = cvTuningScore.socAtStart;
  rec.chargingVoltageTarget = cvBaseTarget;
  rec.chargeStage = getChargeStageDisplayCode();
  rec.epoch = getCurrentTimestamp();
  float nl = (float)cvTuningScore.scoredLowCount;
  if (nl > 0.0f) {
    rec.avgLowSettlingTimeSec = cvTuningScore.totalLowSettlingTimeSec / nl;
    rec.avgLowIntOvVs = cvTuningScore.totalLowIntOvVs / nl;
  } else {
    rec.avgLowSettlingTimeSec = 0.0f;
    rec.avgLowIntOvVs = 0.0f;
  }
  rec.worstLowOvV = cvTuningScore.worstLowOvV;
  rec.worstLowUndershootV = cvTuningScore.worstLowUndershootV;
  rec.steadyP2PV = (cvTuningScore.steadyP2PCount > 0)
                     ? (cvTuningScore.totalSteadyP2PV / cvTuningScore.steadyP2PCount)
                     : -1.0f;  // no HIGH step long enough for a full window → n/a
  sanitizeTuningNote(rec.note, cvTuningNote);
  rec.lowScore = (rec.activeTimeSec > 0.0f)
                   ? (1000.0f * scoreNorm * (cvTuningScore.totalLowIntOvVs + cvTuningScore.totalLowUndershootVs)
                      / rec.activeTimeSec)
                   : 0.0f;

  cvTuningLog[cvTuningLogHead] = rec;
  cvTuningLogHead = (cvTuningLogHead + 1) % 50;
  if (cvTuningLogCount < 50) cvTuningLogCount++;

  saveCVTuningLog();
  cxLedgerLogTuneRun(1);  // every committed run uploads (RAM staging only here — no flash I/O)
  queueConsoleMessageF("CVTuningScore: run#%d score=%.2f settle=%.1fs overshoot=%.3fV n=%d",
                       rec.runNumber, rec.score, rec.avgSettlingTimeSec, rec.worstOvershootV, (int)n);
  cvTuningScore = {};
}

// ===== Control Accuracy v4 — routine-data engine =====
// Bucket classifiers + excursion stopwatches live in AdjustFieldLearnMode and thermalAccuracyScore_tick. The panel, tuning-log
// mirrors and cloud snapshot all read the same accumulators; ratios are computed at display time.

// True once `binding` has held continuously for settleMs; any false tick restarts the timer.
static bool accBindingReady(uint32_t &bindingStartMs, bool binding, uint32_t nowMs, uint32_t settleMs) {
  if (!binding) { bindingStartMs = 0; return false; }
  if (bindingStartMs == 0) bindingStartMs = nowMs;
  return (uint32_t)(nowMs - bindingStartMs) >= settleMs;
}

// clearLive=true (manual Reset) also discards live excursion stopwatches and the thermal containment session. clearLive=false
// carries an in-progress one WHOLE into the new window (a failure spanning midnight lands in the next row); it has no live
// caller while the daily auto-reset is disabled.
void resetAccuracyScores(bool clearLive) {
  accCur4 = {};
  accVolt4 = {};
  accThermSessions = 0;
  accThermBindingSec = 0.0;
  accThermInbandSec = 0.0;
  accThermWorstOverF = 0.0f;
  if (clearLive) {
    excCur = {};
    excVolt = {};
    accThermBindingStartMs = 0;
    accThermSettledPrev = false;
  } else {
    if (excCur.state) accCur4.excursions = 1;
    if (excVolt.state) accVolt4.excursions = 1;
    if (accThermSettledPrev) accThermSessions = 1;
  }
}

// ── SystemID (plant-delay) ring buffer log — mirrors tuning logs above ────
void saveSystemIDLog() {
  if (!systemIDLog) return;
  File f = LittleFS.open("/systemidlog.bin", "w");
  if (!f) return;
  fsFreeDirty = true;
  f.write((uint8_t *)&systemIDLogCount,   sizeof(systemIDLogCount));
  f.write((uint8_t *)&systemIDLogHead,    sizeof(systemIDLogHead));
  f.write((uint8_t *)&systemIDRunCounter, sizeof(systemIDRunCounter));
  f.write((uint8_t *)systemIDLog,         50 * sizeof(SystemIDRecord));
  f.close();
}

// Shared by /resetsystemidlog and the tach-rescale wipe. Persist is deferred to Core 1.
void systemIDLogClearAll() {
  systemIDLogCount   = 0;
  systemIDLogHead    = 0;
  systemIDRunCounter = 0;
  if (systemIDLog) memset(systemIDLog, 0, 50 * sizeof(SystemIDRecord));
  pendingSaveSystemIDLog = true;
}

void loadSystemIDLog() {
  if (!systemIDLog) return;
  File f = LittleFS.open("/systemidlog.bin", "r");
  if (!f) return;
  // Struct size is part of the wire format: discard on mismatch rather than read garbage.
  const size_t expected = sizeof(systemIDLogCount) + sizeof(systemIDLogHead)
                          + sizeof(systemIDRunCounter) + 50 * sizeof(SystemIDRecord);
  if (f.size() != expected) {
    Serial.printf("SystemIDLog: size mismatch (%u vs %u expected) — discarding old log\n",
                  (unsigned)f.size(), (unsigned)expected);
    f.close();
    LittleFS.remove("/systemidlog.bin");
    return;
  }
  f.read((uint8_t *)&systemIDLogCount,   sizeof(systemIDLogCount));
  f.read((uint8_t *)&systemIDLogHead,    sizeof(systemIDLogHead));
  f.read((uint8_t *)&systemIDRunCounter, sizeof(systemIDRunCounter));
  f.read((uint8_t *)systemIDLog,         50 * sizeof(SystemIDRecord));
  f.close();
  // A corrupt count/head would index past the 50-slot ring (commit writes BEFORE the %50 wrap).
  if (systemIDLogCount > 50 || systemIDLogHead >= 50) {
    Serial.println("SystemIDLog: corrupt count/head — discarding old log");
    systemIDLogCount = 0;
    systemIDLogHead = 0;
    LittleFS.remove("/systemidlog.bin");
    return;
  }
  Serial.printf("SystemIDLog: loaded %d records, counter=%d\n",
                systemIDLogCount, systemIDRunCounter);
}

// aborted == true: no rise/fall numbers, but still recorded so the abort shows in history.
void commitSystemIDRecord(bool aborted) {
  if (!systemIDLog) return;

  SystemIDRecord rec = {};
  rec.runNumber          = ++systemIDRunCounter;
  rec.setupStepAmplitude = SystemIDStepAmplitude;
  rec.abortReason        = systemIDAbortReason;
  rec.abortPhase         = systemIDAbortPhase;
  // Conditions at commit — RPM is the true sine-phase mean when one was accumulated (step
  // tests and pre-sine aborts have none → snapshot).
  rec.avgRPM       = (systemIDRpmAvg > 0.0f) ? systemIDRpmAvg : (float)RPM;
  rec.avgAltTempF  = AlternatorTemperatureF;   // NAN = no probe; the emitters carry it as JSON null, never 0.0F
  rec.battV        = BatteryV;
  rec.chargeStage  = getChargeStageDisplayCode();
  rec.epoch        = getCurrentTimestamp();

  if (aborted) {
    rec.score = -1.0f;
    rec.riseAvg_ms = -1.0f;
    rec.fallAvg_ms = -1.0f;
    for (int i = 0; i < 3; i++) {
      rec.riseDelays[i] = -1.0f;
      rec.fallDelays[i] = -1.0f;
      rec.stepAmps[i]   = 0.0f;
      rec.quietPP[i]    = 0.0f;
    }
  } else {
    // Score = rise average (lower = faster plant). If rise didn't measure, fall back to fall.
    rec.score = (systemIDRiseAvg_ms > 0.0f) ? systemIDRiseAvg_ms
              : (systemIDFallAvg_ms > 0.0f) ? systemIDFallAvg_ms
              : -1.0f;
    rec.riseAvg_ms = systemIDRiseAvg_ms;
    rec.fallAvg_ms = systemIDFallAvg_ms;
    for (int i = 0; i < 3; i++) {
      rec.riseDelays[i] = systemIDRiseDelay_ms[i];
      rec.fallDelays[i] = systemIDFallDelay_ms[i];
      rec.stepAmps[i]   = systemIDStepAmp_A[i];
      rec.quietPP[i]    = systemIDQuietPP_A[i];
    }
  }

  systemIDLog[systemIDLogHead] = rec;
  systemIDLogHead = (systemIDLogHead + 1) % 50;
  if (systemIDLogCount < 50) systemIDLogCount++;

  pendingSaveSystemIDLog = true;  // deferred to Core 1 — avoids blocking Core 0
  cxLedgerLogTuneRun(2);  // every committed run uploads, aborts included (RAM staging only)

  queueConsoleMessageF("SystemID: run#%d %s rise=%.0f fall=%.0f stepAmp=%.1f%%",
                       rec.runNumber,
                       aborted ? "ABORTED" : "logged",
                       rec.riseAvg_ms, rec.fallAvg_ms, rec.setupStepAmplitude);
}

void saveSysidSweepLog() {
  if (!sysidSweepLog) return;
  File f = LittleFS.open("/sysidsweeplog.bin", "w");
  if (!f) return;
  fsFreeDirty = true;
  f.write((uint8_t *)&sysidSweepLogCount,   sizeof(sysidSweepLogCount));
  f.write((uint8_t *)&sysidSweepLogHead,    sizeof(sysidSweepLogHead));
  f.write((uint8_t *)&sysidSweepRunCounter, sizeof(sysidSweepRunCounter));
  f.write((uint8_t *)sysidSweepLog,         50 * sizeof(SysIDSweepRecord));
  f.close();
}

void loadSysidSweepLog() {
  if (!sysidSweepLog) return;
  File f = LittleFS.open("/sysidsweeplog.bin", "r");
  if (!f) return;
  const size_t expected = sizeof(sysidSweepLogCount) + sizeof(sysidSweepLogHead)
                          + sizeof(sysidSweepRunCounter) + 50 * sizeof(SysIDSweepRecord);
  if (f.size() != expected) {
    Serial.printf("SysidSweepLog: size mismatch (%u vs %u expected) — discarding old log\n",
                  (unsigned)f.size(), (unsigned)expected);
    f.close();
    LittleFS.remove("/sysidsweeplog.bin");
    return;
  }
  f.read((uint8_t *)&sysidSweepLogCount,   sizeof(sysidSweepLogCount));
  f.read((uint8_t *)&sysidSweepLogHead,    sizeof(sysidSweepLogHead));
  f.read((uint8_t *)&sysidSweepRunCounter, sizeof(sysidSweepRunCounter));
  f.read((uint8_t *)sysidSweepLog,         50 * sizeof(SysIDSweepRecord));
  f.close();
  if (sysidSweepLogCount > 50 || sysidSweepLogHead >= 50) {
    Serial.println("SysidSweepLog: corrupt count/head — discarding old log");
    sysidSweepLogCount = 0;
    sysidSweepLogHead = 0;
    LittleFS.remove("/sysidsweeplog.bin");
    return;
  }
  Serial.printf("SysidSweepLog: loaded %d records, counter=%d\n",
                sysidSweepLogCount, sysidSweepRunCounter);
}

void commitSysidSweepRecord() {
  if (!sysidSweepLog) return;
  if (systemIDBodeCount < 2) return;

  SysIDSweepRecord rec = {};
  rec.runNumber      = ++sysidSweepRunCounter;
  rec.setupAmplitude = SystemIDStepAmplitude;
  rec.stabilizeAmps  = SystemIDStabilizeAmps;
  rec.sweepStartHz   = systemIDSineFreqStart;
  rec.sweepEndHz     = systemIDSineFreqEnd;
  rec.cycles         = systemIDSineCycles;
  rec.nPoints        = systemIDBodeCount;
  rec.avgRPM         = (float)RPM;
  rec.avgAltTempF    = AlternatorTemperatureF;   // NAN = no probe; the emitters carry it as JSON null, never 0.0F
  rec.battV          = BatteryV;
  rec.chargeStage    = getChargeStageDisplayCode();
  rec.epoch          = getCurrentTimestamp();

  for (int i = 0; i < systemIDBodeCount && i < SYSID_SINE_NPOINTS; i++)
    rec.curve[i] = systemIDBode[i];

  float dcGain = systemIDBode[0].gainApPct;
  rec.dcGainApPct = dcGain;
  rec.rolloffHz = -1.0f;
  if (dcGain > 0.0f) {
    float thr = 0.707f * dcGain;
    rec.rolloffHz = systemIDBode[systemIDBodeCount - 1].freqHz;  // no roll-off in range
    for (int i = 1; i < systemIDBodeCount; i++) {
      if (systemIDBode[i].gainApPct < thr) {
        float f0 = systemIDBode[i - 1].freqHz, g0 = systemIDBode[i - 1].gainApPct;
        float f1 = systemIDBode[i].freqHz,     g1 = systemIDBode[i].gainApPct;
        // Log-frequency interpolation of the crossing (sweep points are log-spaced) — matches the
        // dashboard FOPDT fit so the live readout and this logged value agree.
        float t = (g0 - thr) / (g0 - g1);
        rec.rolloffHz = (g0 != g1) ? expf(logf(f0) + (logf(f1) - logf(f0)) * t) : f0;
        break;
      }
    }
  }
  float wp = 0.0f, wpf = 0.0f;
  const float fAlias = sweepAliasLimitHz();
  for (int i = 0; i < systemIDBodeCount; i++) {
    if (fAlias > 0.5f && systemIDBode[i].freqHz > fAlias) continue;  // aliased point — its lag is noise
    float ap = fabsf(systemIDBode[i].phaseDeg);
    if (ap > wp) { wp = ap; wpf = systemIDBode[i].freqHz; }
  }
  rec.worstPhaseDeg = wp;
  rec.worstPhaseFreqHz = wpf;

  sysidSweepLog[sysidSweepLogHead] = rec;
  sysidSweepLogHead = (sysidSweepLogHead + 1) % 50;
  if (sysidSweepLogCount < 50) sysidSweepLogCount++;
  pendingSaveSysidSweepLog = true;  // deferred to Core 1

  cxLedgerLogSweep(0);  // ledger: every committed sweep uploads (RAM staging only here — no flash I/O)

  queueConsoleMessageF("SysID sweep: run#%d rolloff=%.1fHz dcGain=%.3f worstLag=%.0fdeg",
                       rec.runNumber, rec.rolloffHz, rec.dcGainApPct, rec.worstPhaseDeg);
}

void saveTuningSweepLog() {
  if (!tuningSweepLog) return;
  File f = LittleFS.open("/tuningsweeplog.bin", "w");
  if (!f) return;
  fsFreeDirty = true;
  f.write((uint8_t *)&tuningSweepLogCount,   sizeof(tuningSweepLogCount));
  f.write((uint8_t *)&tuningSweepLogHead,    sizeof(tuningSweepLogHead));
  f.write((uint8_t *)&tuningSweepRunCounter, sizeof(tuningSweepRunCounter));
  f.write((uint8_t *)tuningSweepLog,         50 * sizeof(TuningSweepRecord));
  f.close();
}

void loadTuningSweepLog() {
  if (!tuningSweepLog) return;
  File f = LittleFS.open("/tuningsweeplog.bin", "r");
  if (!f) return;
  const size_t expected = sizeof(tuningSweepLogCount) + sizeof(tuningSweepLogHead)
                          + sizeof(tuningSweepRunCounter) + 50 * sizeof(TuningSweepRecord);
  if (f.size() != expected) {
    Serial.printf("TuningSweepLog: size mismatch (%u vs %u expected) — discarding old log\n",
                  (unsigned)f.size(), (unsigned)expected);
    f.close();
    LittleFS.remove("/tuningsweeplog.bin");
    return;
  }
  f.read((uint8_t *)&tuningSweepLogCount,   sizeof(tuningSweepLogCount));
  f.read((uint8_t *)&tuningSweepLogHead,    sizeof(tuningSweepLogHead));
  f.read((uint8_t *)&tuningSweepRunCounter, sizeof(tuningSweepRunCounter));
  f.read((uint8_t *)tuningSweepLog,         50 * sizeof(TuningSweepRecord));
  f.close();
  if (tuningSweepLogCount > 50 || tuningSweepLogHead >= 50) {
    Serial.println("TuningSweepLog: corrupt count/head — discarding old log");
    tuningSweepLogCount = 0;
    tuningSweepLogHead = 0;
    LittleFS.remove("/tuningsweeplog.bin");
    return;
  }
  Serial.printf("TuningSweepLog: loaded %d records, counter=%d\n",
                tuningSweepLogCount, tuningSweepRunCounter);
}

void commitTuningSweepRecord() {
  if (!tuningSweepLog) return;
  if (tuningBodeCount < 2) return;

  TuningSweepRecord rec = {};
  rec.runNumber    = ++tuningSweepRunCounter;
  rec.kp = PidKp; rec.ki = PidKi; rec.kd = PidKd;
  rec.sweepStartHz = tuningSweepStart;
  rec.sweepEndHz   = tuningSweepEnd;
  rec.cycles       = tuningSweepCycles;
  rec.nPoints      = tuningBodeCount;
  rec.avgRPM       = tuningSweepRpmN ? (float)(tuningSweepRpmSum / tuningSweepRpmN) : (float)RPM;
  rec.avgAltTempF  = AlternatorTemperatureF;   // NAN = no probe; the emitters carry it as JSON null, never 0.0F
  // Run-condition snapshot — waveform params + how trustworthy the sweep was.
  rec.sineAmpA       = tuningSweepAmpA;
  rec.baseA          = tuningSweepBaseA;
  rec.battV          = tuningSweepBattV;
  rec.rpmMin         = tuningSweepRpmMin;
  rec.rpmMax         = tuningSweepRpmMax;
  rec.worstCoherence = tuningSweepWorstCoh;
  rec.dutyRailed     = tuningSweepDutyRailed ? 1 : 0;
  rec.chargeStage    = getChargeStageDisplayCode();
  rec.epoch          = getCurrentTimestamp();

  for (int i = 0; i < tuningBodeCount && i < TUNING_SWEEP_NPOINTS; i++)
    rec.curve[i] = tuningBode[i];

  // -3 dB bandwidth from the low end.
  rec.bandwidthHz = -1.0f;
  if (tuningBode[0].gain >= 0.707f) {
    rec.bandwidthHz = tuningBode[tuningBodeCount - 1].freqHz;  // tracks full range
    for (int i = 1; i < tuningBodeCount; i++) {
      if (tuningBode[i].gain < 0.707f) {
        float f0 = tuningBode[i - 1].freqHz, g0 = tuningBode[i - 1].gain;
        float f1 = tuningBode[i].freqHz,     g1 = tuningBode[i].gain;
        // Log-frequency interpolation of the crossing (sweep points are log-spaced) — matches the
        // dashboard FOPDT fit so the live readout and this logged value agree.
        float t = (g0 - 0.707f) / (g0 - g1);
        rec.bandwidthHz = (g0 != g1) ? expf(logf(f0) + (logf(f1) - logf(f0)) * t) : f0;
        break;
      }
    }
  }
  float pk = 0.0f, pkf = 0.0f, wp = 0.0f, wpf = 0.0f;
  const float fAlias = sweepAliasLimitHz();
  for (int i = 0; i < tuningBodeCount; i++) {
    if (tuningBode[i].gain > pk) { pk = tuningBode[i].gain; pkf = tuningBode[i].freqHz; }
    if (fAlias > 0.5f && tuningBode[i].freqHz > fAlias) continue;  // aliased point — its lag is noise
    float ap = fabsf(tuningBode[i].phaseDeg);
    if (ap > wp) { wp = ap; wpf = tuningBode[i].freqHz; }
  }
  rec.peakGain = pk; rec.peakGainFreqHz = pkf;
  rec.worstPhaseDeg = wp; rec.worstPhaseFreqHz = wpf;

  tuningSweepLog[tuningSweepLogHead] = rec;
  tuningSweepLogHead = (tuningSweepLogHead + 1) % 50;
  if (tuningSweepLogCount < 50) tuningSweepLogCount++;
  pendingSaveTuningSweepLog = true;  // deferred to Core 1

  cxLedgerLogSweep(1);  // ledger: every committed sweep uploads (RAM staging only here — no flash I/O)

  queueConsoleMessageF("Tuning sweep: run#%d BW=%.1fHz peak=%.2f worstLag=%.0fdeg",
                       rec.runNumber, rec.bandwidthHz, rec.peakGain, rec.worstPhaseDeg);
}

// Called from tempPID_tick() on every tick (~75 Hz). Feeds the thermal Control
// Accuracy containment score (v4: binding seconds + in-band seconds), authority-gated.
void thermalAccuracyScore_tick(uint32_t nowMs, float dtSec) {
  // ===== Thermal Control Accuracy — containment while binding =====
  // Authority gate: only score while the thermal derate OWNS the current command (g_thermalOwnsCeiling: the >2 A penalty set
  // uTargetAmps; not CV regulating below it; not a fast-OV clamp; not Maintain). Icv/uTargetAmps are one tick stale here but
  // as a consistent pair. Do not go back to a charge-stage proxy: one absorption tick latched the score off for whole sessions.
  bool cvOwnsCommand = voltageControlActive && (Icv < (float)uTargetAmps - 0.5f);

  // A CV handoff shorter than ACC_THERM_CV_YIELD_MS only PAUSES accumulation: 10 s moves the plant ~0.2°F against a ±3°F band,
  // while ending the session on every 0.5 A chatter across the ceiling would never let the 120 s settle finish.
  if (!cvOwnsCommand) thermalCvOwnStartMs = 0;
  else if (thermalCvOwnStartMs == 0) thermalCvOwnStartMs = nowMs;
  bool cvOwnSustained = cvOwnsCommand
                        && (uint32_t)(nowMs - thermalCvOwnStartMs) >= ACC_THERM_CV_YIELD_MS;

  bool thermalBinding = tempPIDActive && thermalSlopeBufFull && !isnan(tempFiltered)
                        && !g_fastOvClampActive && (MaintainMode == 0)
                        && g_thermalOwnsCeiling && !cvOwnSustained;
  bool thermalSettled = accBindingReady(accThermBindingStartMs, thermalBinding, nowMs, ACC_SETTLE_THERMAL_MS);
  if (thermalSettled && !accThermSettledPrev) accThermSessions++;  // containment-session counter
  accThermSettledPrev = thermalSettled;
  if (thermalSettled && !cvOwnsCommand) {
    // Referenced to the loop's ACTUAL target (tempPIDSetpoint_d, limit −7°F), NOT TemperatureLimitF, which gave a perfectly
    // regulating loop a ~7°F error floor. worstOver stays limit-referenced and is tracked unconditionally in AdjustFieldLearnMode
    // so it captures field-off cut tails this gated path never sees.
    float err = tempFiltered - (float)tempPIDSetpoint_d;
    accThermBindingSec += (double)dtSec;
    if (fabsf(err) <= ACC_T_BAND_F) accThermInbandSec += (double)dtSec;
  }
}

// OV-episode window: any hard protection clamp (Group 1/2 OV, iExcess, Load Dump) in the last 30 s. Relapses run 1-3 s apart
// (longest 12.8 s), so 30 s brackets a train with margin and stays invisible to normal operation.
static uint32_t g_ovEpisodeLastFireMs = 0;
static uint32_t g_ovEpisodeStartMs = 0;   // first fire of the current episode — freshness fence for cvSteadyHoldEma
static inline bool ovEpisodeActive() {
  return g_ovEpisodeLastFireMs != 0 && (uint32_t)(millis() - g_ovEpisodeLastFireMs) < 30000u;
}

// Recovery P-boost multiplier (scales only the P term). 1× at/above target and inside the cvRecovBoostFloorV (×class) dead area,
// then a continuous linear ramp (no engagement step) to cvRecovBoostMax at cvRecovBoostErrV of shortfall. The dead area keeps
// boost out of small-signal regulation (it sustained a 0.7 Hz idle wander). Inside an OV episode the ramp slides up to start at
// cvRecovDeepBandV: boosting the climb NEAR target re-fires the trip on a stiff lead-acid plant, but every re-fire was at/above
// target, so a bus deeper than the band keeps the boost.
static inline float cvRecovBoostMult(float shortfallV) {
  if (!cvRecovBoostEnable || shortfallV <= 0.0f) return 1.0f;
  float cls = (float)SYSTEM_VOLTAGE_CLASS / 12.0f;
  float floorV = cvRecovBoostFloorV * cls;
  float fullV = cvRecovBoostErrV * cls;
  if (ovEpisodeActive()) {
    float shift = cvRecovDeepBandV * cls - floorV;
    if (shift > 0.0f) { floorV += shift; fullV += shift; }
  }
  if (shortfallV <= floorV) return 1.0f;
  float frac = (shortfallV - floorV) / fmaxf(fullV - floorV, 0.01f);
  if (frac > 1.0f) frac = 1.0f;
  return 1.0f + (cvRecovBoostMax - 1.0f) * frac;
}

// Arrival flare: taper the recovery ceiling over the last cvRecovFlareBandV (×class) of shortfall, from the full goal to
// cvRecovFlareFrac×goal at target. Arriving with the full pre-trip current into a surface-charged battery re-fired the trip
// (~7-10 A surplus the PI can't shed in time); the PI re-adds held-back amps if needed. Disabled = band 0 / frac 1.
static inline float cvRecovFlaredCeil(float goalA, float shortfallV) {
  float band = cvRecovFlareBandV * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f);
  if (band <= 0.001f || shortfallV >= band) return goalA;
  float frac = fmaxf(shortfallV, 0.0f) / band;
  return goalA * (cvRecovFlareFrac + (1.0f - cvRecovFlareFrac) * frac);
}

// Computed LIVE at the point of use: updateChargingStage() evaluates the same expression but is suppressed in manual/Maintain/
// TargetVoltage, exactly the override modes the DVCC "clamps every writer" rule must cover.
static inline bool cxOwnsBatteryNow() {
  return (commissionState == 1) && (lastCommissionHeartbeatMs != 0)
         && ((uint32_t)(millis() - lastCommissionHeartbeatMs) < COMMISSION_HEARTBEAT_TIMEOUT_MS);
}

// An in-AUTO test branch owns the setpoint and skips the CV block, so its tracker line never runs. Same clears as
// runShutdownPath: left stale-TRUE, the first CV tick after the test skips the entry seed and resumes the pre-test
// cv_I (and any wind-down cap) against whatever bus the test left behind.
static inline void cvYieldToTest() {
  voltageControlActive = false;
  lastVoltageControlActive = false;
  cvWindDownActive = false;
  clearStaleProtClamp();
}

// Top of the AUTO command chain: the RPM-table ceiling, or the owner's one limit while engine speed is ignored.
static float rpmCapAmpsNow(const TickSnapshot &tick) {
  if (tick.ignoreRPM) {
    float I_cap = NoRpmCurrentA;   // §4.8: one owner-set limit replaces both RPM tables while engine speed is ignored
    if (!(I_cap > 0.0f)) {
      // Not configured (an override set before this setting existed): the smallest non-zero limit in the
      // active amps table stands in, said once, so the alternator is not commanded to 0 A in silence.
      float smallest = smallestRpmTableAmps();
      I_cap = smallest;
      static bool noRpmCapNoted = false;
      if (!noRpmCapNoted) {
        noRpmCapNoted = true;
        queueConsoleMessageF("Engine speed is ignored with no current limit set — using the smallest RPM-table limit, %.0f A. Set the limit under Setup > System > Run Without Failed Sensors.", smallest);
      }
    }
    return I_cap;
  }
  if (capLimitMode == 1 && tick.currentBatteryVoltage > 0.5f) {
    return interpolateRPMTable(RPM, rpmCapPowerTable) / tick.currentBatteryVoltage;
  }
  return getCapCurrentForRPM(RPM);
}

// Test ticks run only on a normal AUTO tick and keep a static phase, so a test left running across a gap would resume
// mid-phase, open-loop, when AUTO returned. A pending start would wait the same way, masking the stopped-engine RPM gates.
static uint32_t tierEndedCutTestMs = 0;   // a tier ended a field-cut or protection test: its cut still has the tach re-biasing
static void endFieldTestsOutsideAuto(const TickSnapshot &tick, FieldControlMode mode, FieldEventReason reason) {
  const bool tierOwnsField = (mode == MODE_NORMAL_AUTO && tick.limpTier >= 2);
  const char *why = tierOwnsField ? "Run Without Failed Sensors took the field" : reasonToString(reason);
  // Protection test mode 5 is the Manual-mode bench cut; mode 4 walks the ladder through its own cuts' lockouts.
  const bool protTestStays = (protTestMode == 5 && mode == MODE_NORMAL_MANUAL)
                             || (protTestMode == 4 && protTestRepDone > 0 && mode == MODE_LOCKOUT_RAMP);
  bool ended = false;

  if (altSweepActive != 0) {
    queueConsoleMessageF("Field sweep: ended (%s) — the field is back under the normal control path", why);
    altSweepActive = 0;
    altSweepLastEndMs = millis();
    altSweepAbortRequested = true;   // resets altSweep_tick's static phase on the next normal tick
    ended = true;
  }

  // chcFinish is static in 7_functions.ino, so the result flags are set here directly
  if (chcActive != 0) {
    queueConsoleMessageF("Charge health calibration: ended (%s) — the field is back under the normal control path", why);
    chcActive = 0;
    chcCcActive = false;             // the sizing phase may have owned the setpoint
    chcLastEndMs = millis();
    chcAbortRequested = true;        // resets chc_tick's static phase on the next normal tick
    if (!chcResultsReady) {
      chcOk = false;
      chcResultsReady = true;
      snprintf(chcAbortMsg, sizeof(chcAbortMsg), "ended — the field left normal Auto-mode charging (%s)", why);
    }
    ended = true;
  }

  // Active stays set until the tick consumes the abort, so the start handlers' busy check still blocks a restart
  if (systemIDActive != 0 && !systemIDAbortRequested) {
    systemIDAbortReason = tierOwnsField ? 252 : (uint8_t)reason;   // 252: sentinel outside FieldEventReason, like 253/254
    systemIDAbortPhase = systemIDActive;
    systemIDAbortRequested = true;
    queueConsoleMessageF("SystemID: ended (%s) during phase %d — run the test again", why, (int)systemIDActive);
    ended = true;
  }
  if (fieldCurveActive != 0 && !fieldCurveAbortRequested) {
    fieldCurveAbortReason = (uint8_t)reason;
    fieldCurveAbortVolts = tick.currentBatteryVoltage;
    fieldCurveAbortDuty = lastAppliedDuty;
    strncpy(fieldCurveAbortMsg, why, sizeof(fieldCurveAbortMsg) - 1);
    fieldCurveAbortMsg[sizeof(fieldCurveAbortMsg) - 1] = '\0';
    fieldCurveAbortRequested = true;
    queueConsoleMessageF("Field curve: ended (%s) — the field is back under the normal control path", why);
    ended = true;
  }
  if (fieldCutActive != 0 && !fieldCutAbortRequested) {
    strncpy(fieldCutAbortMsg, why, sizeof(fieldCutAbortMsg) - 1);
    fieldCutAbortMsg[sizeof(fieldCutAbortMsg) - 1] = '\0';
    fieldCutAbortRequested = true;
    fieldCutCcActive = false;
    if (tierOwnsField) { const uint32_t n = millis(); tierEndedCutTestMs = n ? n : 1; }
    queueConsoleMessageF("Field cut: ended (%s) — the field is back under the normal control path", why);
    ended = true;
  }
  if (protTestActive != 0 && !protTestAbortRequested && !protTestStays) {
    protTestAbortRequested = true;
    protTestCcActive = false;
    if (tierOwnsField) { const uint32_t n = millis(); tierEndedCutTestMs = n ? n : 1; }
    queueConsoleMessageF("Protection test: ended (%s) — the field is back under the normal control path", why);
    ended = true;
  }
  if (cvPlantFitActive) {
    cvpfAbort("the field left normal Auto-mode charging during the test — re-run once charging is steady");
    ended = true;
  }
  // cvStress_tick never runs from here. The OV/OC cuts and their lockout are what the test provokes and grades: those only
  // pause it (it resumes when AUTO returns), unless Cancel was pressed meanwhile.
  if (cvStressActive) {
    const bool ownCut = !tierOwnsField
                        && (reason == REASON_FAST_OVERVOLTAGE || reason == REASON_OV_TIER_LOW || reason == REASON_OV_TIER_MID
                            || reason == REASON_INA_OVERVOLTAGE || reason == REASON_HARD_OVERCURRENT || reason == REASON_LOCKOUT_ACTIVE);
    if (cvStressAbortRequested) { cvStressAbortRequested = false; cvStressAbort("cancelled by user"); ended = true; }
    else if (!ownCut)           { cvStressAbort(why); ended = true; }
  }

  if (systemIDRequested) {
    systemIDRequested = false;
    queueConsoleMessageF("SystemID: start request dropped (%s) — press Start again once charging is back in Auto", why);
  }
  if (fieldCurveRequested) {
    fieldCurveRequested = false;
    queueConsoleMessageF("Field curve: start request dropped (%s) — press Start again once charging is back in Auto", why);
  }
  if (fieldCutRequested) {
    fieldCutRequested = false;
    snprintf(fieldCutAbortMsg, sizeof(fieldCutAbortMsg), "not started: %s", why);
    fieldCutOk = false; fieldCutResultsReady = true;
    queueConsoleMessageF("Field cut: start request dropped (%s) — press Start again once charging is back in Auto", why);
  }
  if (protTestRequested && !protTestStays) {
    protTestRequested = false;
    queueConsoleMessageF("Protection test: start request dropped (%s) — press Start again once charging is back in Auto", why);
  }
  // These two expire after 30 s in their own ticks, which a tier never reaches.
  if (altSweepRequested) {
    altSweepRequested = false;
    queueConsoleMessageF("Field sweep: start request dropped (%s) — press Start again once charging is back in Auto", why);
  }
  if (chcRequested) {
    chcRequested = false;
    chcOk = false; chcResultsReady = true;
    snprintf(chcAbortMsg, sizeof(chcAbortMsg), "not started — the field left normal Auto-mode charging (%s)", why);
    queueConsoleMessageF("Charge health calibration: start request dropped (%s) — press Start again once charging is back in Auto", why);
  }

  if (ended) g_sysIDResume.valid = false;
}

void AdjustFieldLearnMode() {

  // ========== TIMING ==========
  static uint32_t lastControlTickMs = 0;
  uint32_t currentMillis = millis();
  uint32_t aflT0 = micros();  // section profiler entry mark (see aflWorstSecUs globals)

  // Runs FIRST (above every early-return below) so temperature history accumulates in all modes.
  // Internal 0.5 Hz throttle. Control-side fields freeze when their owners aren't ticking.
  thermalLog_tick(currentMillis);
  uint32_t aflM0 = micros();  // end of section 0: thermal log

  static uint32_t passCount = 0;
  passCount++;
  // Disarm load-dump counting for any pass that leaves before the detector block (fresh-CH1 wait returns restore it, the
  // block re-arms). vlHasPrev: same rule with the CV branch as restorer, so the CV-interval meter re-baselines after gaps.
  const bool ldArmedIn = g_ldArmed;
  g_ldArmed = false;
  if (!ldArmedIn) loadDumpReset();  // the last pass left the gate closed: drop counts before anything can re-arm
  const bool vlHasPrevIn = vlHasPrev;
  vlHasPrev = false;
  uint32_t actualDtMs = (lastControlTickMs == 0) ? 62 : (currentMillis - lastControlTickMs);
  if (actualDtMs > 500) actualDtMs = 500;
  if (actualDtMs == 0) actualDtMs = 1;  // floor prevents 0-div in slew calcs when tick runs twice in same ms
  float actualDtSec = (float)actualDtMs / 1000.0f;

  static float uTargetRaw_cached = 50.0f;   // always MaxTableValue (assigned from uTargetRaw); seeds fastOvBaseCap (next line) and the supervisorLimiting gate
  float uTargetRaw = (float)MaxTableValue;  // always MaxTableValue; kept for uTargetRaw_cached lineage only
  float fastOvBaseCap = clamp_f(uTargetRaw_cached, 0.0f, (float)MaxTableValue);
  float fastOvCurrentCap = fastOvBaseCap;
  // Binding-reason tracker: LOCAL through the tick, exported to g_fastOvCapReason with g_fastOvCurrentCap after all supervisors.
  // Never write the global here: iExcess/LoadDump sit BELOW the fresh-CH1 early return, so a reset on every no-CH1 pass would
  // wipe their reason while the stale cap stayed exported.
  uint8_t capReasonTick = CAP_REASON_NONE;
  bool fastOvClampActive = false;
  static uint32_t ocTripStartMs = 0;

  updateCurrentRPMTableIndex(RPM);
  updateRPMBucketHistory(currentMillis);

  frtService(currentMillis);   // before the snapshot: a start or an end this pass is in tick.fieldResTest
  TickSnapshot tick = buildTickSnapshot(currentMillis, actualDtMs);
  // Above every early-return below (fresh-CH1 gate, limp home, immediate cuts) so lifetime OV
  // dwell accumulates in all modes. RAW voltage on purpose — filtered smears ~100ms spikes.
  // Only a fresh reading the tier trusts counts: tier 3 runs on the second reading, tier 4 has none.
  const bool ovVoltsOk = (tick.limpTier == 3) ? battVFresh() : (tick.limpTier < 3 && ibvFresh());
  updateOvHistogram(tick.currentBatteryVoltage, ovVoltsOk, tick.nowMs);
  cvStressTrackEveryPass();  // CV stress peak/valley/lockout: same reason, the cut pass and the lockout return below
  // fieldCheck_tick(currentMillis, tick.currentBatteryVoltage);  // TEMP-FIELDCHECK
  uint32_t aflM1 = micros();  // end of section 1: RPM tables + tick snapshot

  // Every tick in every mode: an over-temp FAULT skips the AUTO branch and would freeze the filtered temp/slope/projection for
  // the whole cooldown. Display/estimator math only; TempToUse is fresh here (buildTickSnapshot mirrors tick.tempToUseF).
  tempFilterUpdate(currentMillis);

  // Over-temp protection compares against tempTripValue() (the lower of the DS18B20's last two readings; thermistor: filtered),
  // so a single noisy 1-wire reading can't latch the warning-ramp lockout. A dead sensor is still caught by the raw-based
  // staleness cut. Only the trip thresholds and trip console line see this value; TempToUse's other producers stay raw.
  {
    float tripT = tempTripValue();
    if (!isnan(tripT)) tick.tempToUseF = tripT;
  }

  // Worst over-temp is tracked HERE, unconditionally in every mode: the worst excursions happen in the field-off protection-cut
  // coast-down, which the containment gate never sees, and gating the peak on binding made the score blind to thermal
  // limit-cycling. Containment itself stays gated (cut noise corrupts it).
  if (!isnan(tempFiltered)) {
    float overNow = tempFiltered - TemperatureLimitF;
    if (overNow > accThermWorstOverF) accThermWorstOverF = overNow;
  }

  isTempSustainedWarning(tick.nowMs, tick.tempToUseF, tick.tempLimitF,
                         tick.tempWarnExcessF, tick.ignoreTemperature);

  if (innerPIDResetRequested) {
    innerPIDResetRequested = false;
    currentPID.SetMode(MANUAL);
    pidOutput = 0.0;
    currentPID.ResetIntegratorTo(0.0);
    // lastAppliedDuty is left alone: it is the governor's slew base, and zeroing it stepped a live field to the floor in one tick.
    setpointInitialized = false;
    // Restore AUTOMATIC (only in AUTO) or the Reset button leaves the PID stuck in MANUAL until charging is cycled; SetMode's
    // Initialize() re-confirms the clean state.
    if (sysMode == SYS_MODE_AUTO) {
      currentPID.SetMode(AUTOMATIC);
    }
    queueConsoleMessage("InnerPID: manual reset requested - integrator cleared");
  }

  if (cvLoopResetRequested) {
    cvLoopResetRequested = false;
    cv_I = 0.0f;
    lastVoltageLoopMs = 0;  // force voltage loop to fire on the very next tick
    // cv_I_aw_cap (static inside bumpless block) recovers to MaxTableValue at AwRecoverRate/s
    // automatically once fastOV is not active. No explicit reset needed.
    queueConsoleMessage("CV loop: integrator reset to zero");
  }

  // ========== PRE-GATE IMMEDIATE CUT CHECK ==========
  // Runs before the CH1 gate so INA overvoltage always cuts regardless of sensor freshness.
  // Manual protection test (mode 1/4): fire protTest_tick's armed cut through the REAL fast-OV executor so the event and the
  // adaptive lockout ladder match a genuine OV cut; deferred one tick so it lands in this top-of-loop path.
  if (protTestCutPending && !gpio4IsLow) {
    protTestCutPending = false;
    queueConsoleMessage("Protection test: firing instant field cut (fast-OV executor)");
    applyImmediateCut(tick, REASON_FAST_OVERVOLTAGE, false);
    return;
  }
  FieldEventReason preReason = selectFieldEventReason(tick);
  updateProtectionCounters(preReason);
  if (shouldImmediatelyCutGPIO4(preReason) && !gpio4IsLow) {
    applyImmediateCut(tick, preReason, false);
    return;
  }

  // Engine confirmed stopped (two zero readings over RPM_ZERO_CUT_MS): cut now, overriding any graceful ramp. CHARGING_DISABLED
  // outranks the RPM gate, so a key-off would otherwise slow-ramp ~30 s energized, coupling PWM noise into the LM2907 (phantom
  // RPM). Pre-CH1-gate for full loop rate. Manual mode is exempt (the arbiter puts MANUAL above the RPM gate; they fought at loop
  // rate) and so is the field resistance test (stopped engine, tach 0 by design).
  if (tick.engineFullyStopped && !tick.manualMode && !tick.fieldResTest && !gpio4IsLow) {
    applyImmediateCut(tick, REASON_RPM_TOO_LOW, true);
    return;
  }

  // Shutdown spin-down: charging disabled AND RPM below the field minimum, so the ramp has nothing left to soften and an
  // energized field only fakes tach readings: finish the cut. Fires on the second consecutive low CH2 reading (a lone glitched
  // low read cutting mid-ramp at real duty is the LM2907 coupling-cap slam the ramp avoids); a lone-jump-held value counts as
  // that second reading. RPM_BELOWMIN_MAX_WAIT_MS cuts when no second reading comes.
  {
    static RpmLowConfirm spinDownConfirm;
    if (spinDownConfirm.update(!tick.chargingEnabled && tick.rpmBelowMinimum, tick.nowMs,
                               0, RPM_BELOWMIN_MAX_WAIT_MS, (uint32_t)dataTimestamps[IDX_RPM])
        && !gpio4IsLow) {
      // Cut with the arbiter's OWN reason: low RPM is only the permission to finish early, the cause is whatever disabled
      // charging (hard-coding RPM_TOO_LOW blamed engine speed for the owner's own switch in the banner).
      applyImmediateCut(tick, (preReason == REASON_NONE) ? REASON_RPM_TOO_LOW : preReason, true);
      return;
    }
  }


  // ===== FAST VOLTAGE SAFETY OVERRIDE ==========
  // Every loop before the CH1 gate: fastOvCurrentCap is a per-tick hard ceiling on commanded current, applied to uTargetAmps
  // after the RPM/thermal/user overrides. The direct cv_I clamp is here because the CV loop runs only every 100 ms.

  // ── Zero-output protection stand-down (altZeroOutput) ─────────────────────
  // A protection whose actuator cannot move the protected variable must not fire: at ~0 A into a bus above target, battery rest
  // or another source holds the bus, and each no-op fire still costs the tach false-zero, cv_I reseed, episode derate and lockout.
  // Gates G1/G2 and the CV iExcess arm; absolute layers stay armed. Entry: 2.5 s dwell, strict on raw IBV. Exits on amps/CV/stale
  // are SINGLE-TICK (an RPM rise regains output with no warning). The continue/hold leg is hysteretic on g_ovIbvFilt (one tick
  // stale): the wind-down glide parks ~20 mV under the bus, so a raw strict compare chatters at ripple rate and the dwell never
  // completes. Signed amps on purpose (negative = no shed authority). Stale current data never latches.
  {
    float zeroFullScaleA = (AmpSensorRange == 0) ? 200.0f : (AmpSensorRange == 2) ? 500.0f : 300.0f;
    float zeroBandA = fmaxf(2.0f, 0.01f * zeroFullScaleA);
    static uint32_t altZeroSinceMs = 0;
    float zeroHystV = 0.05f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f);
    bool zeroLegs = voltageControlActive && !tick.currentDataStale && !tick.ignoreAltCurrent
                    && MeasuredAmps < zeroBandA;   // an ignored current sensor must not stand the OV tiers down (tier 2 relies on them)
    bool inBandNow = zeroLegs && ((altZeroOutput || altZeroSinceMs != 0)
                                    ? (g_ovIbvFilt > ChargingVoltageTarget - zeroHystV)
                                    : (IBV > ChargingVoltageTarget));
    if (inBandNow) {
      if (altZeroSinceMs == 0) altZeroSinceMs = currentMillis;
      if (!altZeroOutput && (uint32_t)(currentMillis - altZeroSinceMs) >= 2500UL) {
        altZeroOutput = true;
        queueConsoleMessageF("Protections standing down: alternator %.1fA (zero band %.1fA) with bus %.2fV over target %.2fV — battery/other source holds the bus, a field cut can't lower it; hard cuts stay armed",
                             MeasuredAmps, zeroBandA, IBV, ChargingVoltageTarget);
      }
    } else {
      altZeroSinceMs = 0;
      if (altZeroOutput) {
        altZeroOutput = false;
        queueConsoleMessageF("Protections re-armed: zero-output stand-down ended (alt %.1fA, bus %.2fV, target %.2fV)",
                             MeasuredAmps, IBV, ChargingVoltageTarget);
      }
    }
  }

  // Pre-event cv_I snapshot for the protection-release reseed (CV_Loop_Dev_Summary.md). g_fastOvClampActive here is LAST
  // tick's unified flag (set at the end of the bumpless block after every supervisor voted).
  static float preEventCvI = 0.0f;
  static float preEventIcv = 0.0f;  // commanded current at the same snapshot — recovery goal is max(cv_I, Icv)
  static float preEventLoadEma = 0.0f;  // ~2s EMA of house-load amps (MeasuredAmps − Bcur) while unclamped — demand-drop reference for the release reseed (shunt only)
  static uint32_t preEventLoadMs = 0;   // 0 = EMA unseeded
  if (!g_fastOvClampActive) {
    preEventCvI = cv_I;  // refresh while no protection is clamping
    // Min against the unboosted P+I reconstruction: a boost-inflated climb must not become the next fire's refill goal.
    preEventIcv = fminf(Icv, VoltageKp_active * fmaxf(ChargingVoltageTarget - IBV, 0.0f) + cv_I);
    if (HAS_BATT_SHUNT) {
      float loadsNow = MeasuredAmps - Bcur;
      float dtL = (preEventLoadMs == 0) ? 0.0f : fminf((float)(uint32_t)(currentMillis - preEventLoadMs), 500.0f);
      preEventLoadEma = (preEventLoadMs == 0) ? loadsNow
                                              : preEventLoadEma + (dtL / (2000.0f + dtL)) * (loadsNow - preEventLoadEma);
      preEventLoadMs = currentMillis;
    }
  }
  // Post-protection integrator refill (deficit-gated): while active, cv_I UP-integration runs at VoltageKi_active × M, M = 1 +
  // (cvRecovKiMax−1)·(remaining deficit / deficit at release), floored at CvRecovClimbRate A/s while projected arrival is short
  // of target. It rides the NORMAL integration path, so every gate applies (kdLimiting, satHi, cv_I_aw_cap, battery ceiling).
  // Exits: premise-void backstop (satHi freezes cv_I at the goal ceiling, so a stale goal would cap CV forever) and bus HELD
  // ≈target ~1 s. Icv stays ceilinged at recovCvGoal throughout: the pre-trip current held the target, so more can only
  // overshoot. Goal = max(cvSteadyHoldEma, preEventIcv) − demand drop (the raw snapshot reads ~half the hold); stale-EMA
  // fallback preEventCvI. Any new fire cancels the refill; the mid-heal cv_I becomes the next reseed base (de-escalation ratchet).
  static bool recovActive = false;
  static float recovCvGoal = 0.0f;    // max(cvSteadyHoldEma, preEventIcv) − demandDropA snapshot at release (= the seed on a rapid re-fire) — heal target AND the Icv ceiling
  static float recovDeficit0 = 0.5f;  // recovCvGoal − cv_I at release (floored 0.5A) — normalizes the Ki-boost taper
  static bool recovGoalCollapsed = false;  // rapid re-fire collapsed goal to the seed → deficit-healed exit is void (it would heal on tick 1, dropping the Icv ceiling exactly when it must hold); only held-at-target / walk-to-ceiling / new fire end the window
  static uint32_t recovStartMs = 0;     // release stamp — the premise-void + recovered exits arm 2 s later (field-lag gap right after release has the ceiling pinned on a briefly-flat bus)
  static uint8_t recovStarveTicks = 0;  // consecutive PI ticks the goal ceiling is pinned with the bus low + not rising
  static uint8_t recovHeldTicks = 0;    // consecutive PI ticks the bus has held ≈target — the "recovered" exit; without it the window latches as a stale ceiling whenever the plant heals needing less current than the goal
  static float recovVRefEma = 0.0f;     // ~3s EMA of the filtered bus — the walk's "not rising" test is a delta above this reference; a slope-EMA sign gate ripple-starves the walk
  static float demandDropA = 0.0f;      // house-load amps that left the bus at the fire (rising-edge loads vs preEventLoadEma) — subtracted from reseed base AND goal, else the refill restores a current the event proved unwanted
  static uint8_t rapidReFires = 0;      // consecutive re-fires <4s after release — proof the seed is still high; rebases the next seed on lastSeedA ×0.7
  static uint32_t lastReleaseMs = 0;
  static float lastSeedA = 0.0f;        // cv_I written at the last release — the rebase base on a rapid re-fire (preEventCvI regrows mid-train as the refill climbs)
  static bool recovWalking = false;     // starve backstop engaged (announce-once latch): the goal ceiling walks up at a bounded rate instead of releasing in one step (a one-tick release steps the command by the whole P+I surplus and re-fires). Engaged ≠ moving: the walk itself is re-gated every tick on recovStarveTicks
  static uint32_t cvStallStartMs = 0;   // dwell start for the below-target stall accelerator (0 = disarmed)
  static float    cvStallV0 = 0.0f;     // getFiltV() at dwell start; the no-progress test is a bus DELTA over the dwell, not a per-tick slope — cvDSlope reads ±0.35 V/s on ripple alone and would never accumulate consecutive ticks
  static bool     cvStallBoost = false; // stall accelerator engaged: climb floor extended outside recovActive
  static bool loadServeActive = false;    // load-serve Ki boost (loadServeBoostEnable): measured demand (loads + battery intake) exceeds cv_I with the bus low → boost up-integration toward it. e=K×ΔI hides a 35A load behind ~0.25V on lithium (plain Ki ~30 s). Do not trigger on battery discharge: the P-boost parks Bcur near 0 within ~0.3s
  static float loadServeGoal = 0.0f;      // loadsNow + battIntakeEma snapshot at engagement — taper reference and the healed-exit target
  static float loadServeDeficit0 = 0.5f;  // loadServeGoal − cv_I at engagement (floored 0.5A) — normalizes the taper like recovDeficit0
  static uint8_t loadServeTicks = 0;      // consecutive PI ticks of sustained demand deficit before engaging (chatter guard)
  static float cvSteadyHoldEma = 0.0f;  // slow EMA of cv_I during clean CV near target = true holding current; recovCvGoal basis, immune to overshoot droop and flutter-gap re-sampling; shifted down by demandDropA at a fire (its premise dies with the departed load)
  static uint32_t cvSteadyHoldMs = 0;   // last clean-window EMA update — goal basis trusts the EMA only if this postdates the episode's first fire (a pre-episode EMA can remember a departed load)
  static float battIntakeEma = 0.0f;    // slow EMA of Bcur in the same clean-CV window = battery's charging share at target; reseed floor + the loadServeGoal intake term (shunt only)
  // Post-protection fast-rise window, opened by the falling-edge handler (0 = inactive); capped again at the slew site.
  static uint32_t postProtectRiseStartMs = 0;
  // Zero these one-shot stamps once their windows pass, so a millis() wrap (49.7 days) can't reopen them
  if (postProtectRiseStartMs != 0 && (uint32_t)(currentMillis - postProtectRiseStartMs) >= FastSetpointRiseWindowMs) postProtectRiseStartMs = 0;
  if (lastReleaseMs != 0 && (uint32_t)(currentMillis - lastReleaseMs) >= 4000UL) lastReleaseMs = 0;

  // Field-drain early release: once a clamp episode has held for the commissioned drain time (MEASURED command→10%-of-output,
  // at the RPM latched when the episode began; live RPM is tach-corrupted once the field cuts), the field has bled out, so the
  // iExcess hold latch releases early (once its EMA is back under the fire line). OR'd with iExcess's natural release: can only
  // shorten a cut. G2 (self-releasing on slope) and Load Dump (no hold latch) are unaffected.
  float tauHoldMs = fdDrainMsAtRpm(g_ovClampRpm);
  bool tauReleaseNow = g_fastOvClampActive && g_ovClampRiseMs != 0 && tauHoldMs > 0.0f
                       && (float)(uint32_t)(currentMillis - g_ovClampRiseMs) >= tauHoldMs;
  // Announce once per episode, only when the iExcess hold latch owns the clamp (Load Dump, G1, G2 have no hold to clear).
  // Episode-latched, not edge-detected: the committed reason can flicker between Load-Dump tiers.
  bool tauReleaseOwned = tauReleaseNow && g_fastOvCapReason != CAP_REASON_LOADDUMP
                         && g_fastOvCapReason != CAP_REASON_KHARD_G1
                         && g_fastOvCapReason != CAP_REASON_KHARD_G2;
  static bool tauAnnounced = false;
  if (tauReleaseOwned && !tauAnnounced) {
    tauAnnounced = true;
    g_tauReleaseCount++;
    queueConsoleMessageF("Fast-OV tau-release #%lu: clamp held %lums >= drain %.0fms @ %.0f RPM — releasing iExcess hold as it cools",
                         (unsigned long)g_tauReleaseCount,
                         (unsigned long)(currentMillis - g_ovClampRiseMs), tauHoldMs, g_ovClampRpm);
  }
  if (!tauReleaseNow) tauAnnounced = false;  // episode over — re-arm for the next one

  {
    static float vPrev = 0.0f;
    static uint32_t vPrevMs = 0;
    static float dvdt = 0.0f;
    static bool ovActive = false;
    static float ibvFilt = 0.0f;
    static uint32_t tierLoSinceMs = 0;   // timed OV tiers: first tick the filtered bus held above that tier's line (0 = below)
    static uint32_t tierMidSinceMs = 0;

    g_fastOvHardActive = false;
    g_fastOvVpred = IBV;
    const bool shutdownRampLive = !gpio4IsLow && ChargingVoltageTarget > 0.0f
                                  && (shutdownPhase == SHUTDOWN_PHASE_1 || shutdownPhase == SHUTDOWN_PHASE_3);

    if (ibvFreshFlag) {
      ibvFreshFlag = false;
      if (vPrevMs > 0) {
        uint32_t dtMs = currentMillis - vPrevMs;
        if (dtMs >= 1 && dtMs <= 100) {
          float raw = (IBV - vPrev) / ((float)dtMs * 0.001f);  // V/s
          float alpha = (float)dtMs / (DvdtTC + (float)dtMs);   // dt-aware EMA: TC stays constant across sample-rate jitter
          dvdt = alpha * raw + (1.0f - alpha) * dvdt;
          // Group 2 level filter: raw IBV false-fires on belt ripple (full crest vs a fixed margin).
          // TC is DERIVED from the commissioned plant tau — never a user knob; a display/log
          // filter must not pace a safety layer.
          float ovFiltTC = clamp_f((float)systemIDPlantTauMs / 3.0f, 10.0f, 80.0f);
          float aOv = (float)dtMs / (ovFiltTC + (float)dtMs);
          ibvFilt = aOv * IBV + (1.0f - aOv) * ibvFilt;
        } else if (dtMs > 100) {
          ibvFilt = IBV;  // I2C stall — restart the level filter from live rather than trust a stale value
        }
      } else {
        ibvFilt = IBV;  // first sample after boot
      }
      vPrev = IBV;
      vPrevMs = currentMillis;
    }
    g_fastOvDvdt = dvdt;
    g_ovIbvFilt = ibvFilt;

    if (voltageControlActive) {  // Groups 1/2 target-relative; gated only on voltageControlActive
      const float TD_PRED = TdPred;
      const float V_HARD = ChargingVoltageTarget + OvPredMarginV;
      // Arm-proximity window scales with class: at 48V dV/dt is ~4× so a fixed 0.06V window
      // could be crossed inside one tick, defeating the predictive layer entirely.
      const float PRED_GUARD = 0.06f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f);

      float Vpred = IBV + TD_PRED * fmaxf(0.0f, dvdt);
      g_fastOvVpred = Vpred;

      // Test-mode bypass: G1/G2 stand down when testProtectionsEnabled is off, in TuningMode, during the DCIR test, under the
      // zero-output stand-down (a fire can't move the bus at ~0 A), and in tier 2 (no current command to cap). The fast-OV
      // ceiling and INA228 ALERT stay live. G2's arm state is recomputed every tick from g2SoftNow (same gates), so engaging the
      // bypass drops the clamp on the same tick.
      if (testProtectionsEnabled && !TuningMode && !batteryHealthTestActive && !altZeroOutput && tick.limpTier != 2 && IBV > ChargingVoltageTarget - PRED_GUARD) {
        if (OvGroup1Enable && Vpred > V_HARD) {
          float hardCap = fmaxf(0.0f, setpointLimited - KHard * (Vpred - V_HARD));
          // record reason only when this layer actually lowers the cap (equiv. to fminf)
          if (hardCap < fastOvCurrentCap) { fastOvCurrentCap = hardCap; capReasonTick = CAP_REASON_KHARD_G1; }
          fastOvClampActive = true;
          g_fastOvHardActive = true;
        }
      }

      // Lifetime soft-exceed counter tracks the EXACT Group-2 trigger below (not g_fastOvClampCount,
      // which counts all protections), so the count means "the soft current-cap actually engaged."
      bool g2SoftNow = (testProtectionsEnabled && !TuningMode && !batteryHealthTestActive && !altZeroOutput && tick.limpTier != 2 && OvGroup2Enable && ibvFilt > ChargingVoltageTarget + OvMeasMarginV);
      static bool g2SoftPrev = false;
      if (g2SoftNow && !g2SoftPrev) ovTel().softExceedCount++;
      g2SoftPrev = g2SoftNow;

      // Group 2 measured-OV: armed ONLY while the filtered bus is above target + OvMeasMarginV AND still rising (filtered dvdt
      // > 0, so belt ripple can't fake the sign). A falling bus above the line is receding; re-clamping only flickers the field and
      // compounds the reseed. Every real event crosses the line rising, a persistent overdrive re-arms on each rise, and the
      // absolute cuts backstop a runaway. Do not go back to a hysteresis hold: it re-fired every tick as the bus decayed.
      ovActive = (g2SoftNow && dvdt > 0.0f);
      if (ovActive) {
        float ovExcess = ibvFilt - (ChargingVoltageTarget + OvMeasMarginV);
        float hystCap = fmaxf(0.0f, setpointLimited - KHard * ovExcess);
        if (hystCap < fastOvCurrentCap) { fastOvCurrentCap = hystCap; capReasonTick = CAP_REASON_KHARD_G2; }
        fastOvClampActive = true;
        g_fastOvHardActive = true;
      }
    } else {
      ovActive = false;  // !voltageControlActive (idle only — MaintainMode sets voltageControlActive=true)
      if (!shutdownRampLive) ibvFilt = IBV;  // track raw while idle so CV entry starts the level filter from the live bus
    }

    // ── Timed OV cut tiers (LOW/MID) ──────────────────────────────────────
    // Sustained-violation cuts on the same filtered bus as G2: trip when held CONTINUOUSLY above target + margin for the tier's
    // dwell; any tick below resets (no hysteresis, so ripple crests can't accumulate and a target drop glides the lines). Dwells
    // cut every sustained violation inside 0.4 s (a BMS charge disconnect needs >= 0.5 s; ~100 ms is field de-energization).
    // Same gates as G1/G2; dwell 0 disables a tier. Cuts via the arbiter and the SAME fast-OV lockout ladder. Also armed through
    // a shutdown ramp with the field energized: the ramp ignores the owner's target, so an unwanted charge must still be cut.
    if (voltageControlActive || shutdownRampLive) {
      // altZeroOutput is CV-only; in the ramp the same stand-down is "no alternator output" (untrusted amps = armed).
      bool rampOutput = tick.currentDataStale || tick.ignoreAltCurrent || MeasuredAmps > NoOutputAmps;
      bool tierGate = testProtectionsEnabled && !TuningMode && !batteryHealthTestActive && !altZeroOutput
                      && (voltageControlActive || rampOutput);
      bool loAbove  = tierGate && OvTierLoDwellMs  > 0 && ibvFilt > ChargingVoltageTarget + OvTierLoMarginV;
      bool midAbove = tierGate && OvTierMidDwellMs > 0 && ibvFilt > ChargingVoltageTarget + OvTierMidMarginV;
      if (loAbove)  { if (tierLoSinceMs  == 0) tierLoSinceMs  = currentMillis; } else tierLoSinceMs  = 0;
      if (midAbove) { if (tierMidSinceMs == 0) tierMidSinceMs = currentMillis; } else tierMidSinceMs = 0;
      if (!gpio4IsLow) {  // a cut can remove nothing while the field is already electrically off
        // MID checked first: if both dwells expire on the same tick the higher rung names the cut.
        if (midAbove && (uint32_t)(currentMillis - tierMidSinceMs) >= OvTierMidDwellMs) g_ovTierTrip = 2;
        else if (loAbove && (uint32_t)(currentMillis - tierLoSinceMs) >= OvTierLoDwellMs) g_ovTierTrip = 1;
      }
      if (g_ovTierTrip == 1 && !loAbove)  g_ovTierTrip = 0;
      if (g_ovTierTrip == 2 && !midAbove) g_ovTierTrip = 0;
    } else {
      tierLoSinceMs = 0;
      tierMidSinceMs = 0;
      g_ovTierTrip = 0;
    }
  }

  // ── Group 1/2 hard-OV telemetry export ────────────────────────────────────
  // g_fastOvHardActive is raised only by Groups 1/2, so this rising-edge counter and integrator reset live here; the unified
  // telemetry is exported later in the bumpless block, after iExcess and LoadDump have voted.
  static bool g_fastOvHardActive_prev = false;
  if (g_fastOvHardActive && !g_fastOvHardActive_prev) {
    g_fastOvHardCount++;  // rising-edge only — count each new hard FastOV activation
    // (The OV-episode stamp lives on the unified protection rising edge: iExcess-shaped fire trains never raise this flag.)
    // Collapse the inner integrator on hard OV onset: fastOvCap already drives the setpoint near zero, and a wound-up integrator
    // would grind duty down at ~40%/s instead of reaching MinDuty in 1-2 PID cycles. PID stays AUTOMATIC.
    currentPID.ResetIntegratorTo(0.0);
    queueConsoleMessageF("FastOV hard #%lu: V=%.2fV (filt %.2fV) target=%.2fV — inner PID integrator reset",
                         (unsigned long)g_fastOvHardCount, IBV, g_ovIbvFilt, ChargingVoltageTarget);
  }
  g_fastOvHardActive_prev = g_fastOvHardActive;

  uint32_t aflM2 = micros();  // end of section 2: temp warning + fast-OV supervisor

  // Per-pass dt for the paths that run on EVERY pass below (shutdown ramp, commissioning rest). The CH1 gate (lastControlTickMs
  // / actualDtSec) sits inside the normal branch: above the dispatch, a failed current sensor during any ramp reason froze the
  // field at its last duty because runShutdownPath never ran.
  static uint32_t lastPassMs = 0;
  uint32_t passDtMs = (lastPassMs == 0) ? 62 : (currentMillis - lastPassMs);
  if (passDtMs > 500) passDtMs = 500;
  if (passDtMs == 0) passDtMs = 1;
  lastPassMs = currentMillis;
  const float passDtSec = (float)passDtMs / 1000.0f;

  // ── Reset per-tick pidLog capture globals ────────────────────────────────
  // Read by pidLog_tick(); reset so a skipped branch leaves zero, not a stale value. pidLog_vError is fresh every tick.
  pidLog_vError = ChargingVoltageTarget - tick.currentBatteryVoltage;
  pidLog_uTargetBeforeVoltCap = 0.0f;
  pidLog_uTargetAfterVoltCap = 0.0f;
  pidLog_dutyRequest = 0.0f;
  pidLog_dutyApplied = 0.0f;
  pidLog_voltageLoopRanThisTick = 0;
  pidLog_enteringCV = 0;
  pidLog_enteringTargetVoltageMode = 0;

  // ========== FAST-PATH: critical fault check ==========
  FieldControlMode mode = selectFieldControlMode(tick);
  FieldEventReason reason = selectFieldEventReason(tick);
  g_fieldEventReason = (uint8_t)reason;  // steady-state / commission-rest cause (immediate cuts overwrite this in applyImmediateCut)

  // Anything that outranks the field resistance test ends it (a protection, the switch, the ignition, the BMS input).
  if (frt.state == FRT_RUNNING && mode != MODE_FIELD_RES_TEST) {
    char why[160];
    snprintf(why, sizeof(why), "The field was switched off: %s.", (reason == REASON_CHARGING_DISABLED) ? "the ignition or Alternator Enable went off" : reasonToString(reason));
    frtFail(why);
  }

  if (immediateCutNow(reason, tick) && !gpio4IsLow) {
    applyImmediateCut(tick, reason, false);
    return;
  }

  // ========== COMMISSIONING IDLE REST / CURRENT-SENSOR ZERO HOLD ==========
  // Before the AUTO/MANUAL/fault/stage machinery: no charging stage, no transition logs. sysMode is left as-is (last AUTO)
  // so resuming slips back to NORMAL_AUTO with no SYS_MODE transition spam.
  if (mode == MODE_COMMISSION_IDLE) {
    // A wizard started before the system ever reached AUTO leaves sysMode at its OFF boot default with no path back (this
    // branch returns before the transition handler), so seed AUTO once (chargingEnabled is guaranteed here). Rest only: the zero
    // hold may have interrupted MANUAL, which is where it must hand back.
    if (reason == REASON_COMMISSION_REST && sysMode != SYS_MODE_AUTO) {
      enter_sys_auto();
      pidInitialized = true;
      queueConsoleMessage("Charging enabled (AUTO)");
    }
    // Zero hold only: a rest is never selected with a test live, and a Start that lands after the snapshot must survive it
    if (tick.altZeroMeasuring) endFieldTestsOutsideAuto(tick, mode, reason);
    runCommissionIdle(tick, reason, passDtSec);
    prevMode = mode;
    return;
  }

  if (mode == MODE_FIELD_RES_TEST) {
    runFieldResTestHold(tick, reason);
    prevMode = mode;
    return;
  }

  // ========== OVERRIDE MODE ENTRY/EXIT DETECTION ==========
  // Edge detection so one-shot actions fire exactly once on each transition.
  static bool lastMaintainMode = false;
  static bool lastTargetVoltageMode = false;
  bool enteringMaintainMode = (MaintainMode == 1) && !lastMaintainMode;
  bool exitingMaintainMode = (MaintainMode == 0) && lastMaintainMode;
  bool enteringTargetVoltageMode = (TargetVoltageMode == 1) && !lastTargetVoltageMode;
  bool exitingTargetVoltageMode = (TargetVoltageMode == 0) && lastTargetVoltageMode;
  lastMaintainMode = (MaintainMode == 1);
  lastTargetVoltageMode = (TargetVoltageMode == 1);

  // ========== CHARGING STAGE (bulk/absorption/float) ==========
  // Suppressed during overrides (spurious re-bulk / absorption timeouts), in MANUAL (no CV loop), and in CVTuningMode (the step
  // test raises ChargingVoltageTarget above the stage target and can expire the absorption tail mid-test). Tiers 3/4 have no
  // main voltage to decide on (§4.4), and a stale IBV must not advance the absorption/bulk-hold timers on a held reading.
  if (MaintainMode != 1 && TargetVoltageMode != 1 && !CVTuningMode && !tick.manualMode && tick.limpTier < 3 && !tick.ibvStale) {
    updateChargingStage();
  }

  // On override exit restart from BULK rather than resume possibly stale stage state; updateChargingStage() fast-forwards if
  // the battery is already charged.
  if ((exitingMaintainMode || exitingTargetVoltageMode) && sysMode == SYS_MODE_AUTO) {
    enter_sys_auto();
    if (exitingMaintainMode) queueConsoleMessage("MaintainMode exit: resuming charge from bulk");
    if (exitingTargetVoltageMode) queueConsoleMessage("TargetVoltageMode exit: resuming charge from bulk");
  }

  // Report mode changes
  static FieldControlMode lastDebugMode = (FieldControlMode)255;
  static FieldEventReason lastDebugReason = (FieldEventReason)255;
  if (mode != lastDebugMode || reason != lastDebugReason) {
    char modeMsg[100];
    snprintf(modeMsg, sizeof(modeMsg), "Control Mode: %s - %s",
             modeToString(mode), reasonToString(reason));
    serialPrintlnNB(modeMsg);
    queueConsoleMessage(modeMsg);
    lastDebugMode = mode;
    lastDebugReason = reason;
  }

  if ((mode != MODE_NORMAL_AUTO && mode != MODE_NORMAL_MANUAL) && reason == REASON_NONE) {
    static uint32_t lastNoReasonWarnMs = 0;
    if ((uint32_t)(currentMillis - lastNoReasonWarnMs) >= 5000) {
      lastNoReasonWarnMs = currentMillis;
      serialPrintlnNB("ERROR: Shutdown mode with no reason specified");
    }
  }

  chargingEnabled = tick.chargingEnabled;

  // An automated/guided test owns the limiters: the user-facing limiter toggles go inert so a stray setting can't ruin a
  // measurement. NOT bare TuningMode/CVTuningMode (manual study tabs, toggles meant live). Commissioning counts only while the
  // wizard is open (cxOwnsBatteryNow), not the bare state byte, which stays 1 for days after a stopped run.
  g_autoTestActive = cxOwnsBatteryNow() || (fieldCurveActive != 0) || batteryHealthTestActive || resTestActive || cvPlantFitActive || (systemIDActive != 0) || (fieldCutActive != 0) || cvStressActive || (protTestActive != 0) || (chcActive != 0);

  // ========== DETERMINE GOVERNOR MODE ==========
  govMode = GOV_NORMAL_SLEW;

  // Any sine waveform bypasses duty slew so the actuator follows the reference (DutyRampRate would smear the frequency
  // response), but only once g_tuningEntrySettled so the entry ramp stays gentle; once settled this drops the coupling-cap
  // transient protection during sine. AUTO only: TuningMode and g_tuningEntrySettled stay set through a shutdown, where the
  // bypass would collapse the phase-1 ramp in one step.
  if (TuningMode != 0 && tuningWaveform != 0 && g_tuningEntrySettled && mode == MODE_NORMAL_AUTO) {
    govMode = GOV_BYPASS_SLEW;
  }

  // CV plant fit: pulse edges must land in one tick and the CC phases ride their own setpoint ramp — a
  // slow DutyRampRate would smear both. Reverts when cvPlantFitActive clears. AUTO only: the teardown clears it after
  // this block, one shutdown tick too late.
  if (cvPlantFitActive && mode == MODE_NORMAL_AUTO) {
    govMode = GOV_BYPASS_SLEW;
  }

  // Field-decay ramp phase: duty slew OFF by spec (the TEST_ENTRY_RATE_A setpoint slew provides smoothness); later phases get
  // the bypass via sysIDRunning. AUTO only: the teardown clears these flags after this block, so the first shutdown/MANUAL tick
  // would collapse the phase-1 ramp in one step.
  if ((fieldCutCcActive || protTestCcActive || chcCcActive) && mode == MODE_NORMAL_AUTO) {
    govMode = GOV_BYPASS_SLEW;
  }

  // Manual CC square-wave test in Off mode: bypass duty slew (once the entry ramp has settled) so the edges
  // are truly instant, matching the abrupt setpoint. Default/Custom keep the normal DutyRampRate protection.
  if (TuningMode != 0 && tuningWaveform == 0 && !g_autoTestActive && !tuningSquareAbrupt &&
      testSlewMode == 0 && g_tuningEntrySettled && mode == MODE_NORMAL_AUTO) {
    govMode = GOV_BYPASS_SLEW;
  }

  // Voltage sensor failure: bypass slew. An owner-overridden disagreement is not a failure: with a reading ignored
  // the other one stays dead or far off for the whole run, and this would drop the duty ramp limit throughout.
  if ((!tick.voltagePlausible && !reasonSuppressedByOverride(REASON_VOLTAGE_IMPLAUSIBLE, tick))
      || (tick.voltageDisagreementCritical && !reasonSuppressedByOverride(REASON_VOLTAGE_DISAGREE_CRITICAL, tick))) {
    govMode = GOV_BYPASS_SLEW;
  }

  // CV overshoot: bypass duty slew so the current PID can drop field without the governor rate limit. Latched from
  // fastOvClampActive until battV is within 0.02 V×class of target (unscaled it sits at the 48 V noise floor), CV only. Do not
  // revert: see CV_Loop_Dev_Summary.md (govBypass latch section).
  {
    static bool cvGovBypassLatch = false;
    if (voltageControlActive) {
      if (fastOvClampActive) {
        cvGovBypassLatch = true;
      } else if (IBV < ChargingVoltageTarget + 0.02f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f)) {
        cvGovBypassLatch = false;
      }
      if (cvGovBypassLatch) {
        govMode = GOV_BYPASS_SLEW;
      }
    } else {
      cvGovBypassLatch = false;  // reset on CV exit so it doesn't carry into next CV entry
    }
  }

  // ========== MODE TRANSITION HANDLING ==========
  bool isNormalMode = (mode == MODE_NORMAL_AUTO || mode == MODE_NORMAL_MANUAL) && !immediateCutNow(reason, tick);
  bool wasNormalMode = (prevMode == MODE_NORMAL_AUTO || prevMode == MODE_NORMAL_MANUAL);
  bool enteringNormal = !wasNormalMode && isNormalMode;
  bool exitingNormal = wasNormalMode && !isNormalMode;

  SystemMode newSysMode;
  if (!tick.chargingEnabled) newSysMode = SYS_MODE_OFF;
  else if (immediateCutNow(reason, tick)) newSysMode = SYS_MODE_FAULT;
  else if (mode == MODE_NORMAL_MANUAL) newSysMode = SYS_MODE_MANUAL;
  else if (mode == MODE_NORMAL_AUTO) newSysMode = SYS_MODE_AUTO;
  else newSysMode = SYS_MODE_FAULT;

  if (newSysMode != sysMode) {
    switch (newSysMode) {
      case SYS_MODE_OFF:
        enter_sys_off();
        queueConsoleMessage(tick.bmsBlocking ? "Charging disabled - BMS on/off signal" : "Charging disabled");
        serialPrintlnNB(tick.bmsBlocking ? "Charging disabled - BMS on/off signal" : "Charging disabled");
        break;
      case SYS_MODE_MANUAL:
        enter_sys_manual();
        if (enteringNormal) {
          shutdownPhase = SHUTDOWN_PHASE_NONE;
          shutdownPhaseEntryMs = 0;
          shutdownPhase2EntryMs = 0;
          settledAtZeroDutyMs = 0;
          queueConsoleMessage("Charging enabled (MANUAL)");
          serialPrintlnNB("Charging enabled (MANUAL)");
        }
        break;
      case SYS_MODE_AUTO:
        enter_sys_auto();
        tuningScore = {};  // discard partial data — cannot score across a non-AUTO gap
        tuningParamChanged = false;
        pidInitialized = true;
        if (enteringNormal) {
          shutdownPhase = SHUTDOWN_PHASE_NONE;
          shutdownPhaseEntryMs = 0;
          shutdownPhase2EntryMs = 0;
          settledAtZeroDutyMs = 0;
          warmupCeiling = 0.0f;
          inStartupRamp = (StartupRiseRate > 0.0f);  // slow-ramp setpoint on field turn-on; cleared once setpointLimited catches command
          queueConsoleMessage("Charging enabled (AUTO)");
          serialPrintlnNB("Charging enabled (AUTO)");
        }
        break;
      case SYS_MODE_FAULT:
        enter_sys_fault();
        if (exitingNormal) {
          queueConsoleMessageF("Charging stopped: %s | ADS=%.2fV INA=%.2fV D=%.3fV",
                               reasonToString(reason), BatteryV, IBV, fabsf(BatteryV - IBV));
        }
        break;
    }
  }

  // ========== LOCKOUT TRANSITION TRACKING ==========
  bool lockoutActiveNow = tick.inLockout;
  // Spam guard: the idle stale-temp loop completes a cooldown every ~2 s, so "Cooldown complete" is held
  // COOLDOWN_DONE_CONFIRM_MS and dropped if the next cut lands first (a grinding cycle reports via the "Still cycling"
  // heartbeat); once-per-minute cap for slower repeats. The latch gates nothing but the message.
  static uint32_t cooldownDonePendingMs = 0;
  static bool     cooldownDoneNormal = false;
  static uint32_t lastCooldownDoneMs = 0;
  const uint32_t COOLDOWN_DONE_CONFIRM_MS = 5000;
  if (lockoutWasActive && !lockoutActiveNow) {
    cooldownDonePendingMs = tick.nowMs ? tick.nowMs : 1;   // 0 is the "nothing pending" sentinel
    cooldownDoneNormal = isNormalMode;
  } else if (lockoutActiveNow) {
    cooldownDonePendingMs = 0;   // back in lockout inside the window — that was cycle churn, not a recovery
  } else if (cooldownDonePendingMs != 0
             && (uint32_t)(tick.nowMs - cooldownDonePendingMs) >= COOLDOWN_DONE_CONFIRM_MS) {
    if (lastCooldownDoneMs == 0 || (uint32_t)(tick.nowMs - lastCooldownDoneMs) >= 60000) {
      queueConsoleMessage(cooldownDoneNormal ? "Cooldown complete - charging resumed" : "Cooldown complete");
      serialPrintlnNB(cooldownDoneNormal ? "Cooldown complete - charging resumed" : "Cooldown complete");
      lastCooldownDoneMs = tick.nowMs;
    }
    cooldownDonePendingMs = 0;
  }
  lockoutWasActive = lockoutActiveNow;

  if (fieldCollapseTime > 0 && (tick.nowMs - fieldCollapseTime) >= activeCollapseDelay) {
    fieldCollapseTime = 0;
  }
  uint32_t aflM3 = micros();  // end of section 3: CH1 gate + stage/governor/mode transitions

  // a tier returns into runReducedSensorControl below, before any test tick runs
  if (!isNormalMode || mode == MODE_NORMAL_MANUAL || (tick.limpTier >= 2 && sysMode == SYS_MODE_AUTO)) {
    endFieldTestsOutsideAuto(tick, mode, reason);
  }

  // ========== NON-NORMAL MODE: SHUTDOWN / FAULT HANDLING ==========
  // NOTE: pidLog_tick() is NOT called in the shutdown/fault path.
  if (!isNormalMode) {
    runShutdownPath(tick, mode, reason, passDtSec, exitingNormal);
    prevMode = mode;
    return;
  }

  // ========== NORMAL MODES (MANUAL or AUTO) ==========
  // Re-check before enabling GPIO4 — fault could have arrived this tick.
  if (immediateCutNow(reason, tick) && !gpio4IsLow) {
    applyImmediateCut(tick, reason, false);
    return;
  }

  // Any pass that left before this point (shutdown, cut, commissioning rest, lockout) breaks the run, and the next
  // closed-loop tick paces as a nominal one: the gap since the last tick would otherwise reach the 500 ms cap and
  // let the slew governors step duty ~20 points at once.
  static uint32_t lastNormalPass = 0;
  if (lastNormalPass != passCount - 1) lastControlTickMs = 0;
  lastNormalPass = passCount;

  // ========== GATE ON FRESH CH1 DATA ==========
  // PidSampleDivisor=1: PID runs every CH1 sample (fresh CH1 ~13 ms apart, up to ~30 ms)
  // PidSampleDivisor=2: every other sample, etc. Only the closed-loop body below needs a current sample;
  // every shutdown, cut and rest path has already run above this line, and the published reason is current.
  static uint8_t ch1SampleCount = 0;
  if (!ch1FreshFlag) {
    // Manual mode does not need current sensor data — let it through. Neither do the reduced-sensor
    // tiers (2-4), whose whole point is a dead or ignored CH1.
    if (!tick.manualMode && tick.limpTier < 2) {
      g_ldArmed = ldArmedIn;
      vlHasPrev = vlHasPrevIn;
      return;
    }
  } else {
    ch1SampleCount++;
    if (ch1SampleCount < PidSampleDivisor) {
      ch1FreshFlag = false;
      g_ldArmed = ldArmedIn;
      vlHasPrev = vlHasPrevIn;
      return;
    }
    ch1FreshFlag = false;
    ch1SampleCount = 0;
  }
  if (lastControlTickMs == 0) { actualDtMs = 62; actualDtSec = 0.062f; }
  lastControlTickMs = currentMillis;
  // Hold the field off while a Core 0 internet operation or queued HTTPS upload is pending, so the field can't blip between
  // uploads (fieldActiveStatus 4 = WAITING_CLOUD tells the banner why). A 10 s safety timeout flushes the queue and proceeds; the
  // in-flight upload finishes and releases its ring marker itself, and a dropped queued one keeps its slot for retry. Only while the field is cut: this
  // return skips the PID, the voltage loop and every soft protection.
  {
    static unsigned long fieldHoldStartMs = 0;
    bool cloudBusy = gpio4IsLow && (core0Busy || uxQueueMessagesWaiting(httpsQueue) > 0);
    if (cloudBusy) {
      if (fieldHoldStartMs == 0) fieldHoldStartMs = millis();
      if (millis() - fieldHoldStartMs < 10000UL) {
        fieldActiveStatus = 4;  // WAITING_CLOUD
        return;
      }
      // Drain item-by-item, NOT xQueueReset: each request OWNS a ps_malloc'd payload only the receiver frees (a reset leaks
      // 4-128 KB of PSRAM per request).
      HttpsRequest droppedReq;
      while (xQueueReceive(httpsQueue, &droppedReq, 0) == pdTRUE) {
        if (droppedReq.payload) free(droppedReq.payload);
        // Senders that hold an in-flight state wait on it with no timeout — hand them a failure.
        if (droppedReq.type == HTTPS_UPLOAD_CX_LEDGER) cxLedgerUpState = -1;
        else if (droppedReq.type == HTTPS_CLOUD_OP) cloudOpFailDropped();
        else if (droppedReq.type == HTTPS_UPLOAD_CONFIG) configSnapshotRequested = true;  // retries at the next settled field-off, not the 24 h backstop
        else if (droppedReq.type == HTTPS_GET_PENDING_CONFIG) pendingConfigCheckDone = false;  // boot-only pull: without this it waits for the next reboot
        else if (droppedReq.type == HTTPS_UPLOAD_PAYLOAD) {
          // Only a dropped queued upload releases the ring marker; one already executing still owns it and releases it itself.
          lastUploadWasBuffered = false;
          sensorRingInFlightIndex = -1;
        }
      }
      queueConsoleMessage("Field-on wait timeout (10s) — flushed cloud queue, proceeding");
      fieldHoldStartMs = 0;
    } else {
      fieldHoldStartMs = 0;
    }
  }
  if (protTestManualCutActive) {
    // Manual bench protection test (mode 5): hold the enable line OPEN for the set duration (Manual sits above the lockout, so the
    // fast-OV ladder can't hold the field off here). Gated here, not re-written per tick, so no HIGH glitch shows on a scope.
    digitalWrite(4, LOW);
    gpio4IsLow = true;
  } else {
    digitalWrite(4, HIGH);
    gpio4IsLow = false;
  }
  // GPIO38 driven solely by CheckAlarms — do not write here

  // Seed setpoint tracking on first normal tick after startup or re-enable.
  if (!setpointInitialized) {
    setpointLimited = max(0.0f, getTargetAmps());
    setpointInitialized = true;
  }

  // ========== RUN WITHOUT FAILED SENSORS: tiers 2-4 (§4.3-§4.5) ==========
  // Inside the normal branch: every unsuppressed reason already took its cut path above, so a tier can never re-raise a field a
  // protection holds off. MANUAL keeps precedence. The inner PID is parked while a tier owns the field and re-entered through
  // enter_sys_auto() on the way back, else Compute() runs on a MANUAL PID and freezes the duty.
  {
    static bool reducedCtlWasActive = false;
    if (tick.limpTier >= 2 && sysMode == SYS_MODE_AUTO) {
      if (!reducedCtlWasActive) {
        reducedCtlWasActive = true;
        queueConsoleMessageF("%s: tier %d takes the field (%s)",
                             "Run Without Failed Sensors", (int)tick.limpTier,
                             tick.limpTier == 2 ? "voltage-only regulation, duty capped" : tick.limpTier == 3 ? "fixed duty, capped by the second battery voltage" : "fixed duty");
      }
      runReducedSensorControl(tick, reason, actualDtSec);
      prevMode = mode;
      return;
    }
    if (reducedCtlWasActive) {
      reducedCtlWasActive = false;
      reducedCtlLeaveTiers34();
      voltageControlActive = false;
      lastVoltageControlActive = false;
      cvWindDownActive = false;
      // MANUAL took the field (bench tool): hand it over as-is. enter_sys_auto() here would flip
      // sysMode back to AUTO for one tick.
      if (sysMode == SYS_MODE_AUTO) {
        enter_sys_auto();
        pidInitialized = true;
        queueConsoleMessage("Run Without Failed Sensors: field handed back to normal closed-loop control");
      }
    }
  }

  // ========== SYSTEM ID OVERRIDE ==========
  // A plant-delay test bypasses all setpoint machinery and the PID: systemID_tick() commands duty directly (governor in bypass)
  // and the integrator is parked at that duty so the PID resumes smoothly.
  float sysIDDutyOut = lastAppliedDuty;

  static bool prevSysIDRunning = false;
  bool sysIDRunning = systemID_tick(sysIDDutyOut, MeasuredAmps, tick.nowMs);
  // Commissioning field-% ramp: same duty-override + bumpless-resume path (mutually exclusive via the start-handler mutex).
  sysIDRunning = fieldCurve_tick(sysIDDutyOut, MeasuredAmps, tick.nowMs) || sysIDRunning;
  // Field de-energize τ test (stage 7): RAMP returns false and drives the real current PID via fieldCutCcActive below;
  // hold/cut/ease own duty here (cut level is MinDuty, so the effectiveMinDuty=0 bypass is moot).
  sysIDRunning = fieldCut_tick(sysIDDutyOut, MeasuredAmps, tick.nowMs) || sysIDRunning;
  // Charge health calibration (stage 9): field pinned open-loop while the operator swings the
  // throttle. Same duty-override + bumpless-resume path as the field curve.
  sysIDRunning = chc_tick(sysIDDutyOut, MeasuredAmps, tick.nowMs) || sysIDRunning;
  // Manual protection-trigger test (Settings → Emergency): RAMP returns false (protTestCcActive drives the PID to protTestCmdA);
  // load-dump/graceful/ease own duty here. Modes 1/4 arm protTestCutPending for the pre-gate cut instead.
  sysIDRunning = protTest_tick(sysIDDutyOut, MeasuredAmps, tick.nowMs) || sysIDRunning;
  // CV plant-fit pulse train (Step 7 of the wizard display) — same mutex family. SETTLE/PILOT/HOLD return false (the
  // cvpfCcActive branch below drives the current PID); the abrupt PULSES/RELEASE own duty here.
  sysIDRunning = cvpf_tick(sysIDDutyOut, MeasuredAmps, tick.nowMs) || sysIDRunning;
  // Gate-tuning field sweeper (ALT_GATE_TUNING_CAPTURE_SPEC.md §8): same override path; a continuous ramp up and back down.
  sysIDRunning = altSweep_tick(sysIDDutyOut, MeasuredAmps, tick.nowMs) || sysIDRunning;
  // CV stress test (stage 8): a PURE OBSERVER. The normal loop runs as fielded; this only confirms tracking, counts protection
  // events and grades the recovery.
  cvStress_tick(tick.nowMs);

  bool sysIDJustStarted = !prevSysIDRunning && sysIDRunning;
  bool sysIDJustCompleted = prevSysIDRunning && !sysIDRunning;
  prevSysIDRunning = sysIDRunning;
  if (sysIDRunning) ctrlLimiter = 0;

  // ── Snapshot on test start (fires once, first tick sysIDRunning goes true) ──
  // Pre-test values are still clean: the override block below hasn't touched PID or setpoint yet this tick.
  if (sysIDJustStarted) {
    g_sysIDResume = {
      .valid = true,
      .sysMode = sysMode,
      .setpointLimited_snap = setpointLimited,
      .lastAppliedDuty_snap = lastAppliedDuty,
      .cv_I_snap = cv_I,
      .voltageControlActive_snap = voltageControlActive,
    };
    queueConsoleMessageF(
      "SystemID: pre-test state captured | mode=%s duty=%.1f%% setpoint=%.2fA cv_I=%.2fA cvActive=%d",
      (sysMode == SYS_MODE_MANUAL) ? "MANUAL" : "AUTO",
      g_sysIDResume.lastAppliedDuty_snap,
      g_sysIDResume.setpointLimited_snap,
      g_sysIDResume.cv_I_snap,
      g_sysIDResume.voltageControlActive_snap);
  }

  // ── While test is running: bypass all setpoint machinery ────────────────────
  if (sysIDRunning) {
    govMode = GOV_BYPASS_SLEW;
    clearStaleProtClamp();  // the CV body is skipped for the whole override
    currentPID.SetMode(MANUAL);
    currentPID.ResetIntegratorTo((double)sysIDDutyOut);
    pidOutput = (double)sysIDDutyOut;
    vlHasPrev = false;  // voltLoop_record never fires during the override: re-baseline so the test isn't logged as one giant CV-interval gap
  }

  // ── Restore on test completion ───────────────────────────────────────────────
  // Safety wins: restore only if still in a legal normal-running state. The PID integrator is seeded with the CURRENT applied
  // duty, not the stale pre-test duty; the normal path then computes the next command immediately.
  if (sysIDJustCompleted && g_sysIDResume.valid) {
    bool legalToResume = (mode == MODE_NORMAL_AUTO || mode == MODE_NORMAL_MANUAL);

    if (!legalToResume) {
      // A fault or disable arrived during the test. Do not force old state back;
      // let the current fault path win on its own terms this tick and beyond.
      queueConsoleMessage(
        "SystemID: pre-test state NOT restored — system no longer in normal running state");
    } else {

      // Restore the setpoint tracker clamped to the live ceiling (the command chain's start, I_cap − thermalPenaltyAmps), so
      // demand legal pre-test but now above the RPM/thermal/user cap is not re-asserted; slower caps below it slew the rest.
      float liveCeil = fminf(rpmCapAmpsNow(tick) - thermalPenaltyAmps, (float)MaxTableValue);
      float restoredSetpoint = clamp_f(
        g_sysIDResume.setpointLimited_snap, 0.0f, fmaxf(liveCeil, 0.0f));
      setpointLimited = restoredSetpoint;

      // Restore cv_I only if CV both before and after; otherwise the bumpless tracker re-derives it next tick.
      bool cvContextUnchanged = (g_sysIDResume.voltageControlActive_snap && voltageControlActive);
      if (cvContextUnchanged) {
        cv_I = g_sysIDResume.cv_I_snap;
      }

      // Reseed PID integrator to current applied duty (bumpless transfer).
      if (g_sysIDResume.sysMode == SYS_MODE_AUTO) {
        currentPID.SetMode(AUTOMATIC);
      }
      currentPID.ResetIntegratorTo((double)lastAppliedDuty);
      pidOutput = (double)lastAppliedDuty;

      queueConsoleMessageF(
        "SystemID: restored to %s | duty=%.1f%% setpoint=%.2fA cv_I=%.2fA (cvRestored=%d)",
        (g_sysIDResume.sysMode == SYS_MODE_MANUAL) ? "MANUAL" : "AUTO",
        lastAppliedDuty,
        restoredSetpoint,
        cvContextUnchanged ? g_sysIDResume.cv_I_snap : cv_I,
        cvContextUnchanged);
    }

    g_sysIDResume.valid = false;
  } else if (sysIDJustCompleted && sysMode == SYS_MODE_AUTO) {
    // Aborted test (snapshot invalidated): the override parked the PID in MANUAL, and with sysMode still AUTO no transition re-enables it
    currentPID.SetMode(AUTOMATIC);
    currentPID.ResetIntegratorTo((double)lastAppliedDuty);
    pidOutput = (double)lastAppliedDuty;
  }

  if (!sysIDRunning) {

    // ========== SETPOINT COMPUTATION (AUTO mode only) ==========
    setpointCommand = 0.0f;   // global (declared in Xregulator.ino) — read by the Control Accuracy score gate

    if (sysMode == SYS_MODE_AUTO) {

      static bool lastTuningMode = false;
      static bool lastBattHealth = false;

      // tempFilterUpdate() runs at the TOP of this function, ungated, so FAULT/MANUAL/OFF keep live filtered temps.

      // Deadman for the wizard's resonance current-check: no command refresh → auto-release through the same deferred wind-down
      // as a normal stop (handing straight to CV at the held test current re-arms G2 against a bus the test floated above target).
      if (resTestActive && !resTestReleasing && ((int32_t)(millis() - resTestLastCmdMs) > (int32_t)RES_TEST_DEADMAN_MS)) {   // signed: the web task can stamp resTestLastCmdMs after millis() was sampled
        resTestReleasing = true; resTestTargetA = 0.0f; ripScoreArmed = false;
        // A dead browser also ends the RPM Invaders sweep: close the fold window (AUTO folds would poison the fixed-current table)
        // and persist what committed.
        if (ripGameFill || ripTabPendingWipe) {
          ripGameFill = false; ripTabPendingWipe = false; ripTabPendingSave = true;
        }
        queueConsoleMessage("Resonance current-check auto-released (no command refresh) - winding the field down before normal control resumes");
      }

      if (batteryHealthTestActive) {
        // Active DCIR step generator. Soft protections (G1/G2, CV/bulk iExcess) are dropped via the !batteryHealthTestActive gates
        // so the step manifests; fast-OV and the INA228 OV stay live, and the test self-clears (edge count, bhAbort, watchdog).
        if (tick.nowMs - bhLastToggleMs >= bhDwellMs) {
          bhWaveHigh = !bhWaveHigh;
          bhLastToggleMs = tick.nowMs;
          if (bhToggleCount < BH_MAX_TOGGLES) bhToggleMs[bhToggleCount] = tick.nowMs;
          bhToggleCount++;
          bhEdgeCount++;
          // 2 ring-in + bhNumEdges scored + 1 trailing toggle to bound the last edge's window
          if (bhEdgeCount >= (int)bhNumEdges + 3) batteryHealthTestActive = false;
        }
        float bhTarget = bhWaveHigh ? (bhStepLowA + bhStepDeltaA) : bhStepLowA;
        setpointCommand = bhTarget;
        // FIXED conservative test rate, not the user's SetpointRiseRate/FallRate (could slam the field); the DCIR fit differences
        // SETTLED end-of-dwell levels, so a slower transition reads the same resistance.
        setpointLimited = slew_limit_f(setpointLimited, setpointCommand,
                                       TEST_ENTRY_RATE_A, TEST_ENTRY_RATE_A, actualDtSec);
        cvYieldToTest();
        ctrlLimiter = 0;
        targetCurrent = (OutputPIDSigSrc == 2) ? MeasuredAmps : (OutputPIDSigSrc == 1) ? g_pidMA_N
                                                                                       : g_pidI_filtered;
        pidInput = (double)targetCurrent;
        pidSetpoint = (double)setpointLimited;
        pidError = setpointLimited - targetCurrent;
        currentPID.Compute();
        bhSample(tick.nowMs);
        lastBattHealth = true;

      } else if (resTestActive) {
        // Wizard resonance current-check (§3.2): drive to resTestTargetA at the test entry rate. voltageControlActive=false the whole
        // test, so G1/G2 are suppressed (field can sit at 0.90×max output); only absolute fast-OV and INA228 are live. EXIT is
        // deferred (resTestReleasing): releasing straight to CV at the held current re-armed G2 against the floated bus and slammed
        // the field, so slew to ~0 here at SetpointFallRate first; CV then ramps up from below the target.
        setpointCommand = resTestReleasing ? 0.0f : resTestTargetA;
        float resFallRate = resTestReleasing ? SetpointFallRate : TEST_ENTRY_RATE_A;
        setpointLimited = slew_limit_f(setpointLimited, setpointCommand,
                                       TEST_ENTRY_RATE_A, resFallRate, actualDtSec);
        cvYieldToTest();
        ctrlLimiter = 0;
        targetCurrent = (OutputPIDSigSrc == 2) ? MeasuredAmps : (OutputPIDSigSrc == 1) ? g_pidMA_N
                                                                                       : g_pidI_filtered;
        pidInput = (double)targetCurrent;
        pidSetpoint = (double)setpointLimited;
        pidError = setpointLimited - targetCurrent;
        currentPID.Compute();
        // Scoring gate: armed once the command has finished ramping AND measured current has arrived, held RIP_SCORE_HOLD_MS; any
        // miss resets it so the hold is re-earned. faFiltRippleUpdate reads this.
        {
          static uint32_t ripAtTgtSinceMs = 0;
          float ripTgtTol = fmaxf(3.0f, 0.08f * resTestTargetA);
          bool ripAtTgt = !resTestReleasing && resTestTargetA > 1.0f
                          && setpointLimited >= resTestTargetA - 0.05f
                          && targetCurrent   >= resTestTargetA - ripTgtTol;
          if (ripAtTgt) { if (ripAtTgtSinceMs == 0) ripAtTgtSinceMs = millis(); }
          else ripAtTgtSinceMs = 0;
          ripScoreArmed = ripAtTgt && (millis() - ripAtTgtSinceMs >= RIP_SCORE_HOLD_MS);
        }
        // Field is down → hand back to normal control from a low-voltage state (CV ramps up from below).
        if (resTestReleasing && setpointLimited <= 2.0f) { resTestActive = false; resTestReleasing = false; }

      } else if (fieldCutCcActive) {
        // Field-decay ramp/settle (stage 7): real current loop to fieldCutCmdA at the test rate; G1/G2 suppressed so the level
        // holds, fast-OV/INA228/hard-OC live. fieldCut_tick flips to the duty-override path for hold/cut/ease.
        setpointCommand = fieldCutCmdA;
        setpointLimited = slew_limit_f(setpointLimited, setpointCommand,
                                       TEST_ENTRY_RATE_A, SetpointFallRate, actualDtSec);
        cvYieldToTest();
        ctrlLimiter = 0;
        targetCurrent = (OutputPIDSigSrc == 2) ? MeasuredAmps : (OutputPIDSigSrc == 1) ? g_pidMA_N
                                                                                       : g_pidI_filtered;
        pidInput = (double)targetCurrent;
        pidSetpoint = (double)setpointLimited;
        pidError = setpointLimited - targetCurrent;
        currentPID.Compute();

      } else if (chcCcActive) {
        // Charge health calibration sizing (stage 9): real current loop to chcCmdA, the share of the charge-rate column at the held
        // speed. chc_tick watches settle, freezes the landed duty and flips to the duty-override path for the three passes. G1/G2
        // suppressed; fast-OV/INA228/hard-OC live.
        setpointCommand = chcCmdA;
        setpointLimited = slew_limit_f(setpointLimited, setpointCommand,
                                       TEST_ENTRY_RATE_A, SetpointFallRate, actualDtSec);
        cvYieldToTest();
        ctrlLimiter = 0;
        targetCurrent = (OutputPIDSigSrc == 2) ? MeasuredAmps : (OutputPIDSigSrc == 1) ? g_pidMA_N
                                                                                       : g_pidI_filtered;
        pidInput = (double)targetCurrent;
        pidSetpoint = (double)setpointLimited;
        pidError = setpointLimited - targetCurrent;
        currentPID.Compute();

      } else if (protTestCcActive) {
        // Manual protection-test energize: real current loop to protTestCmdA at the test rate; G1/G2 suppressed,
        // fast-OV/INA228/hard-OC live. protTest_tick watches settle, then fires the mode.
        setpointCommand = protTestCmdA;
        setpointLimited = slew_limit_f(setpointLimited, setpointCommand,
                                       TEST_ENTRY_RATE_A, SetpointFallRate, actualDtSec);
        cvYieldToTest();
        ctrlLimiter = 0;
        targetCurrent = (OutputPIDSigSrc == 2) ? MeasuredAmps : (OutputPIDSigSrc == 1) ? g_pidMA_N
                                                                                       : g_pidI_filtered;
        pidInput = (double)targetCurrent;
        pidSetpoint = (double)setpointLimited;
        pidError = setpointLimited - targetCurrent;
        currentPID.Compute();

      } else if (cvpfCcActive) {
        // CV plant-fit current-PID phases: drive the real current loop to cvpfCmdA (set by cvpf_tick,
        // already run this tick). voltageControlActive=false suppresses target-relative G1/G2 so the
        // step manifests; fast-OV/INA228/hard-OC stay live.
        setpointCommand = cvpfCmdA;
        setpointLimited = slew_limit_f(setpointLimited, setpointCommand,
                                       CVPF_ENTRY_RATE_A, TEST_ENTRY_RATE_A, actualDtSec);
        cvYieldToTest();
        ctrlLimiter = 0;
        targetCurrent = (OutputPIDSigSrc == 2) ? MeasuredAmps : (OutputPIDSigSrc == 1) ? g_pidMA_N
                                                                                       : g_pidI_filtered;
        pidInput = (double)targetCurrent;
        pidSetpoint = (double)setpointLimited;
        pidError = setpointLimited - targetCurrent;
        currentPID.Compute();

      } else if (TuningMode) {
        // ===== TUNING MODE (square-wave setpoint generator) =====
        static bool tuningWaveHigh = false;
        static uint32_t lastTuningWaveToggle = 0;
        static bool tuningEntryRamped = false;
        // Re-arm the slewed entry each time tuning (re)starts. lastTuningMode is still false on the
        // first tuning tick (it's set true at the end of this block), so this fires exactly on entry.
        if (!lastTuningMode) tuningEntryRamped = false;

        // Parameter changed — discard accumulator and re-ring-in under new params
        if (tuningParamChanged) {
          tuningScore = {};
          tuningParamChanged = false;
        }

        if (manualCommitTuningRequested) {
          manualCommitTuningRequested = false;
          if (tuningScore.scoredToggleCount >= 4 && tuningScore.activeTimeSec > 0.5f) {
            commitTuningRecord();
          } else {
            queueConsoleMessageF("TuningScore: commit rejected — only %d/4 scored half-cycles accumulated",
                                 (int)tuningScore.scoredToggleCount);
          }
        }

        // ── Waveform: square (0) | sine manual (1) | sine auto-sweep (2) ──
        static float tuningSinePhase = 0.0f;
        if (tuningWaveform != 0) {
          // SINE (closed-loop reference). Bypass slew so the PID sees the true sine; no ISE scoring.
          tuningScore.inScoringWindow = false;
          float baseA = (float)tuningWaveFloor + waveAmplitude * 0.5f;   // midpoint; swings tuningWaveFloor .. tuningWaveFloor+waveAmplitude
          float ampA  = waveAmplitude * 0.5f;
          // A fresh sweep starts from the prior sweep's low rest current: re-arm the slewed entry so a back-to-back Run can't slam.
          if (tuningWaveform == 2 && tuningSweepRequested) tuningEntryRamped = false;
          tuningSineStep(tick.nowMs, actualDtSec, tuningSinePhase, baseA, ampA, MeasuredAmps, setpointCommand);
          // Entry: ease up from the rest floor at the FIXED test rate with duty slew still engaged (bypass is gated on
          // g_tuningEntrySettled); once caught the sine runs clean. If it never catches it stays duty-slewed, never a slam.
          if (!tuningEntryRamped) {
            setpointLimited = slew_limit_f(setpointLimited, setpointCommand,
                                           TEST_ENTRY_RATE_A, TEST_ENTRY_RATE_A, actualDtSec);
            if (fabsf(setpointLimited - setpointCommand) < 0.5f) tuningEntryRamped = true;
          } else {
            setpointLimited = setpointCommand;         // no slew limiting → clean sine reference
          }
        } else {
          uint32_t halfPeriodMs = ((uint32_t)wavePeriod * 1000) / 2;
          if (tick.nowMs - lastTuningWaveToggle >= halfPeriodMs) {
            tuningWaveHigh = !tuningWaveHigh;
            lastTuningWaveToggle = tick.nowMs;

            tuningScore.toggleCount++;
            if (tuningScore.toggleCount > 4) {
              tuningScore.ringInDone = true;  // 2 full ring-in cycles (4 half-periods) complete
            }
            if (tuningScore.ringInDone) {
              tuningScore.pendingWindowOpen = true;  // open window once slew settles, not at toggle time
              tuningScore.inScoringWindow = false;
              tuningScore.scoredToggleCount++;
            }
          }

          // Close scoring window 5s after it opened (lastToggleMs is set when window opens, not at toggle)
          if (tuningScore.inScoringWindow && (tick.nowMs - tuningScore.lastToggleMs > 5000)) {
            tuningScore.inScoringWindow = false;
          }

          uTargetAmps = tuningWaveHigh ? (tuningWaveFloor + waveAmplitude) : tuningWaveFloor;
          setpointCommand = (float)uTargetAmps;

          // tuningSquareAbrupt (commissioning CV-fit) forces a true step; the manual square test follows testSlewMode (0 Off =
          // abrupt, 1 Default = factory rate, 2 Custom = user rate). Abrupt paths STILL ease in from rest at TEST_ENTRY_RATE_A (OV
          // risk) and go abrupt only after arrival, like the sine branch; g_autoTestActive keeps commissioning off this path.
          bool abruptEdge = tuningSquareAbrupt || (!g_autoTestActive && testSlewMode == 0);
          if (abruptEdge) {
            if (!tuningEntryRamped) {
              setpointLimited = slew_limit_f(setpointLimited, setpointCommand,
                                             TEST_ENTRY_RATE_A, TEST_ENTRY_RATE_A, actualDtSec);
              if (fabsf(setpointLimited - setpointCommand) < 0.5f) tuningEntryRamped = true;
            } else {
              setpointLimited = setpointCommand;
            }
          } else {
            // Slew on. Manual Default uses the factory rate; Custom (and any auto fallback) uses the user rate.
            float rr = (!g_autoTestActive && testSlewMode == 1) ? SETPOINT_RISE_DEFAULT : SetpointRiseRate;
            float fr = (!g_autoTestActive && testSlewMode == 1) ? SETPOINT_FALL_DEFAULT : SetpointFallRate;
            setpointLimited = slew_limit_f(setpointLimited, setpointCommand, rr, fr, actualDtSec);
          }

          // Open scoring window once slew has settled (rate < 1 A/s) — fair regardless of SetpointRiseRate.
          // lastToggleMs is set here so the 5s timeout starts from when scoring actually begins.
          {
            static float tuning_prevSlewed = 0.0f;
            static bool tuning_slewInit = false;
            float tuning_slewRate = 0.0f;
            if (tuning_slewInit) {
              tuning_slewRate = fabsf(setpointLimited - tuning_prevSlewed) / actualDtSec;
            }
            tuning_prevSlewed = setpointLimited;
            tuning_slewInit = true;
            if (tuningScore.pendingWindowOpen && tuning_slewRate < 1.0f) {
              tuningScore.inScoringWindow = true;
              tuningScore.pendingWindowOpen = false;
              tuningScore.lastToggleMs = tick.nowMs;
            }
          }
        }
        cvYieldToTest();
        ctrlLimiter = 0;

        targetCurrent = (OutputPIDSigSrc == 2) ? MeasuredAmps : (OutputPIDSigSrc == 1) ? g_pidMA_N
                                                                                       : g_pidI_filtered;
        pidInput = (double)targetCurrent;
        pidSetpoint = (double)setpointLimited;
        pidError = setpointLimited - targetCurrent;
        currentPID.Compute();

        if (tuningScore.inScoringWindow) {
          float e = pidError;
          tuningScore.errorAccum += e * e * actualDtSec;
          tuningScore.activeTimeSec += actualDtSec;
          if (fabsf(e) > tuningScore.worstErrorA) tuningScore.worstErrorA = fabsf(e);
          tuningScore.rpmSum += RPM;
          float tempSample = isnan(AlternatorTemperatureF) ? TempToUse : AlternatorTemperatureF;
          if (!isnan(tempSample) && tempSample > -90.0f) { tuningScore.tempSum += tempSample; tuningScore.tempSampleCount++; }   // -99 = thermistor never read (§10.4)
          tuningScore.avgSampleCount++;
          if (tuningScore.activeTimeSec > 0.0f) {
            tuningScore.score = tuningScore.errorAccum / tuningScore.activeTimeSec;
          }
        }

        // No TrackAppliedOutput() here: the shared call at the end of the normal-mode section covers it.

        g_tuningEntrySettled = tuningEntryRamped;  // next tick's govMode block reads this to gate the sine duty-bypass

        lastTuningMode = true;

      } else {
        // ===== NORMAL AUTO =====

        // Health test just ended. Deliberately NO current-PID reseed: it kept the PID LIVE in AUTOMATIC (like TuningMode), so the
        // integrator already tracks; a sysID-style ResetIntegratorTo here is an exit bump.
        if (lastBattHealth) lastBattHealth = false;

        // Detect TuningMode exit — fires exactly once.
        if (lastTuningMode) {
          tempPIDActive = false;  // PID was dormant during tuning — force the bumpless re-seed
          // tempFilterUpdate() kept the IIR and slope buffer live through tuning, so keep the slope buffer on re-enable: wiping it
          // drops the setpoint to limit-20 for 60 s right after a test (TuningMode→0 turns off suppressWarmupMargin), derating the
          // field though the loop has a live trend. Stay at limit-7.
          thermalPreserveSlopeOnResume = true;
          tuningScore = {};  // discard accumulator — commit is always manual
          tuningSquareAbrupt = false;  // never let the abrupt-step bypass leak past a tuning session
        }
        lastTuningMode = false;
        g_tuningEntrySettled = false;  // not in TuningMode → sine duty-bypass disarmed (re-arms only after the next entry ramp settles)

        // Temperature loop PID. Library timer governs Compute() cadence.
        tempPID_tick(currentMillis, actualDtSec);

        // Command chain: I_cap (RPM-table ceiling) − thermalPenalty → uTargetAmps ([0, MaxTableValue], upper bound for the CV
        // loop) → Icv (CV PI output, [0, uTargetAmps]). Icv is never written back to the thermal state. HiLow is applied at table load.

        float I_cap = rpmCapAmpsNow(tick);
        g_I_cap = I_cap;  // expose RPM table ceiling globally for thermal scoring gate
        float I_cmd = I_cap - thermalPenaltyAmps;
        I_cmd = fminf(I_cmd, (float)MaxTableValue);
        I_cmd = fmaxf(I_cmd, 0.0f);

        uTargetAmps = I_cmd;

        // Warmup ramp: advance ceiling each tick, apply as cap on uTargetAmps
        // warmupBinding: the ramp, not the table, is what output is riding — limiter code 13. Once the
        // ceiling saturates at MaxTableValue it can no longer undercut, so this cannot latch true.
        bool warmupBinding = false;
        if (WarmupRampRate > 0.0f) {
          warmupCeiling = fminf(warmupCeiling + WarmupRampRate * actualDtSec, (float)MaxTableValue);
          if (warmupCeiling < uTargetAmps) { uTargetAmps = warmupCeiling; warmupBinding = true; }
        }

        // Charge-rate Hi->Lo ceiling glide, armed by applyChargeRateMode(): the table already dropped to the Low cap but the
        // alternator is still at the old output, so hold the ceiling up and ramp it down (rate below)
        // instead of stepping. The ramp alone can't stop an iExcess trip (field current lags ABOVE any descending reference), so it
        // also drives modeCapGlideSuppress, which holds both iExcess EMAs at 0. HARD protections stay live; self-clears at the new cap.
        bool glideBinding = false;   // the held (descending) ceiling is above the new cap — limiter code 14
        if (modeCapSlewActive) {
          if (modeCapSlew > (float)uTargetAmps) {
            // Ramp at MaxTableValue / MODE_CAP_GLIDE_SEC A/s (~2.5s full-scale) — gentle enough that the
            // field stays close to the descending ceiling, so the post-glide settling tail stays tiny.
            modeCapSlew = fmaxf((float)uTargetAmps, modeCapSlew - (MaxTableValue / MODE_CAP_GLIDE_SEC) * actualDtSec);
            uTargetAmps = modeCapSlew;
            glideBinding = true;
          } else {
            modeCapSlewActive = false;
            modeCapSlewEndMs = currentMillis;   // start the post-glide iExcess grace window
          }
        }
        // True during the glide AND MODE_CAP_GLIDE_GRACE_MS after, so both iExcess detectors ignore the deliberate descent and the
        // field-current settling tail. The stamp is zeroed past the window so a millis() wrap can't reopen it.
        bool modeCapGlideSuppress = modeCapSlewActive
                                    || (modeCapSlewEndMs != 0 && (uint32_t)(currentMillis - modeCapSlewEndMs) < MODE_CAP_GLIDE_GRACE_MS);
        if (!modeCapGlideSuppress) modeCapSlewEndMs = 0;

        // User overrides
        // Zero-current float (UseFloat=2): same zero-command regulation as MaintainMode, but entered
        // automatically by the float stage — alternator carries the house loads up to its ceiling, battery rests at 0 A.
        // Unlike the manual MaintainMode override, the stage machine (and its rebulk criteria) stays live.
        zeroFloatActive = (EFFECTIVE_USE_FLOAT == 2) && !inBulkStage && !inAbsorptionStage && !inIdleStage
                          && MaintainMode == 0 && TargetVoltageMode == 0 && !CVTuningMode;
        // The zeroed uTargetAmps caps only the battery-current command; this keeps the alternator ceiling for the PID below.
        float zeroCmdAltCeil = NAN;
        if (MaintainMode == 1 || zeroFloatActive) { zeroCmdAltCeil = (float)uTargetAmps; uTargetAmps = 0; }

        // ── House-load / other-source offset + battery charge-current ceiling (G4) ─────────
        // MeasuredAmps − Bcur = house loads MINUS other charge sources (I_alt + I_other = I_batt + I_load), light EMA (~1 s). Added
        // to the battery charge limit it gives an alternator-amp ceiling valid however many sources are on the bus, min-selected in
        // bulk AND CV. Deliberately NOT clamped ≥0: a second source out-producing the loads makes it negative, and clamping let the
        // alternator overshoot the battery limit by that source's share. Only the ceiling is floored. Needs the INA228 shunt.
        const bool bcurLive = bcurFresh();   // a held shunt reading (§10.6) must not feed the offset or the shunt-basis ceilings
        if (bcurLive) {
          const float ldoTC = 0.25f;  // s
          static bool ldoInit = false;
          float instLoad = MeasuredAmps - Bcur;
          if (!ldoInit) { loadOffsetFilt = instLoad; ldoInit = true; }
          else {
            float a = actualDtSec / (ldoTC + actualDtSec);
            loadOffsetFilt += a * (instLoad - loadOffsetFilt);
          }
        }
        bool battCeilBinding = false;  // battery charge-current ceiling won the min-select this tick (banner limiter code 4)
        if (BattLimitEnable && BattCurrentLimitA > 0.0f && HAS_BATT_SHUNT) {
          // Shunt quiet: the house-load offset is unknowable, so the battery limit caps alternator amps directly
          // (charge current can never exceed alternator output) until the reading returns.
          float battCeilAlt = bcurLive ? fmaxf(0.0f, BattCurrentLimitA + loadOffsetFilt) : BattCurrentLimitA;
          // Console note when the battery limit binds AND output rides it: once after 5 s sustained, then at most every 10 min.
          static uint32_t battCeilBindStartMs = 0;
          static uint32_t battCeilLastMsgMs = 0;
          if (battCeilAlt < uTargetAmps) {
            uTargetAmps = battCeilAlt;
            battCeilBinding = true;
            if (MeasuredAmps > battCeilAlt - 5.0f) {
              if (battCeilBindStartMs == 0) battCeilBindStartMs = currentMillis;
              else if ((uint32_t)(currentMillis - battCeilBindStartMs) > 5000UL
                       && (battCeilLastMsgMs == 0 || (uint32_t)(currentMillis - battCeilLastMsgMs) > 600000UL)) {
                battCeilLastMsgMs = currentMillis;
                queueConsoleMessageF("Battery charge limit binding: alternator command capped at %.0fA (%.0fA battery limit %+.0fA house loads minus other charge sources)",
                                     battCeilAlt, BattCurrentLimitA, loadOffsetFilt);
              }
            } else {
              battCeilBindStartMs = 0;
            }
          } else {
            battCeilBindStartMs = 0;
          }
        }

        // ── DVCC follow: external charge-current limit (CCL) ceiling ─────────────────────────
        // Same min-select as the battery limit. A battery-basis CCL (VE.Can, RV-C) with a shunt is a BATTERY-current limit,
        // converted by the house-load offset; without one it caps alternator amps directly (conservative). The WiFi CCL is
        // already this alternator's ceiling (dvccCclAltBasis): applied as is. CCL 0 is a legitimate command (cold/full/cell-high):
        // ceiling 0, recovery rides the existing rate-governed ramp.
        bool dvccCclBinding = false;
        if (dvccEn == 1 && dvccState == 3 /*FOLLOWING*/ && !cxOwnsBatteryNow() && !isnan(dvccCclA)) {
          bool shuntBasis = (!dvccCclAltBasis && HAS_BATT_SHUNT && bcurLive);
          float dvccCeil = shuntBasis ? fmaxf(0.0f, dvccCclA + loadOffsetFilt) : fmaxf(0.0f, dvccCclA);
          if (dvccCeil < uTargetAmps) {
            uTargetAmps = dvccCeil;
            dvccCclBinding = true;
          }
        }

        // Actual RPM/thermal/override ceiling — the true pre-OV current limit for logging.
        // uTargetRaw remains MaxTableValue and is not used for telemetry.
        float i_ceiling_pre_ov = (float)uTargetAmps;

        // Hoisted here so iExcess block can reset it on event onset.
        static float cv_I_aw_cap = 100.0f;

        // ── iExcess supervisor (EMA / leaky-integral detector) ──────────────
        // Fires on a SUSTAINED excess over the CV command: EMA of (MeasuredAmps − setpointLimited) crossing E (affine in
        // setpointLimited, floor/ceil). Armed only near target (IBV > target − IExcessArmMarginV) so ramp-up can't fire it; the
        // test/tuning/DCIR gates inhibit it (else branch releases and reseeds). See Working Markdown Docs/iExcess_Redesign_Spec.md.
        {
          const float K_IE = 1.0f;
          static bool iExcessActive = false;   // latched fire state (held until the average clears)
          static float mExcessEma = 0.0f;      // EMA of signed deviation iActual − setpointLimited (A)
          static bool postProtMismatch = false;  // wind-down gate: suppress an iExcess re-fire during the field-TC tail AFTER a protection releases (the "double-penalty" guard)
          static uint32_t postProtClearMs = 0;   // millis the wind-down safety cap is measured from (refreshed every clamped tick → counts from the release edge)
          // cv_I capture/reseed: preEventCvI above + the unified falling-edge reseed
          // in the bumpless tracker block.

          // Maintain / zero-current float: the command is deliberately 0, so percent-of-command is meaningless and both regimes
          // disarm. altZeroOutput generalizes to zero measured output (no fire can move the bus); Load Dump and hard cuts stay armed.
          if (IExcessEnable && testProtectionsEnabled && !TuningMode && !batteryHealthTestActive && voltageControlActive
              && MaintainMode == 0 && !zeroFloatActive && !altZeroOutput && (IBV > ChargingVoltageTarget - IExcessArmMarginV)) {
            // Affine trip line, floor/ceil guarded. Commissioning fits slope to the ripple slope and base to idle ripple + Safety
            // Margin, so the line rides a fixed margin above ripple (base 0 = through-origin).
            float E = fmaxf(IExcessFloorA, fminf(IExcessFrac * setpointLimited + IExcessBaseA, IExcessCeilA));
            // Field bled >= tau: release the hold only once last tick's EMA is at/under E; clearing while EMA > E hands the latch
            // to the edge-triggered fire, re-running its side effects every tick until the EMA decays.
            if (tauReleaseNow && mExcessEma <= E) iExcessActive = false;

            float ieActual = MeasuredAmps;

            // Post-protection wind-down gate (the "double-penalty" guard): after a clamp releases, field current lags ABOVE the
            // collapsed command for ~1-2 field TCs and would re-fire iExcess, zeroing cv_I again and halving it again via the unified
            // reseed. The while-clamped EMA hold stops at release, exactly when the wind-down begins, so arm while any protection owns
            // the clamp and HOLD until the excess decays inside the band, bounded by a safety cap (voltage backstops own the rest).
            const uint32_t kPostProtMismatchMaxMs = (uint32_t)fmaxf(150.0f, 3.0f * (float)fieldDecayTauMs);  // 3× the commissioned worst-case field-drain time (longest endpoint of the drain-vs-RPM line), floored 150ms; backstop only — normal release is the decay test
            bool clampOwned = fastOvClampActive || iExcessActive || modeCapGlideSuppress;  // include iExcess's OWN clamp so the guard arms after an iExcess-only cut, not just a voltage-OV cut
            if (clampOwned) {
              postProtMismatch = true;
              postProtClearMs  = currentMillis;  // refreshed every clamped tick, so the cap below counts from the release edge
            } else if (postProtMismatch) {
              bool decayed  = (ieActual - setpointLimited) <= E;  // field current back inside the normal band → wind-down done
              bool timedOut = (currentMillis - postProtClearMs) > kPostProtMismatchMaxMs;
              if (decayed || timedOut) postProtMismatch = false;
            }

            if (!iExcessActive && (clampOwned || postProtMismatch)) {
              // Another protection owns the clamp, or just released and the field is winding down: the mismatch is its own doing
              // (dev jumps to ~full current, crossing E in ms). Hold the EMA at 0 so only a genuinely sustained excess fires. The
              // while-clamped half is a deliberate keep vs spec §7: deleting it re-fires ~8 ms after every fastOV event.
              // modeCapGlideSuppress: the Hi->Lo glide's descending command leaves the field current lagging above it by design.
              mExcessEma = 0.0f;
            } else {
              // dt-aware EMA of the raw deviation (I²C jitter can't corrupt the time constant), same pattern as g_fastOvDvdt.
              float tauSec = IExcessTau * 0.001f;
              float alpha  = actualDtSec / (tauSec + actualDtSec);
              mExcessEma  += alpha * ((ieActual - setpointLimited) - mExcessEma);  // setpointLimited = previous tick — acceptable

              // Rising edge — fire once.
              if (!iExcessActive && mExcessEma > E) {
                cv_I_aw_cap = cv_I;         // cap the bumpless tracker ceiling to pre-event level — prevents current-limited rewind
                g_iExcessCount++;
                // Collapse inner PID integrator (same as hard-OV reset) — without it duty authority
                // is only innerKp × measured current, weak from low setpoints. See CV_Loop_Dev_Summary.md.
                currentPID.ResetIntegratorTo(0.0);
                queueConsoleMessageF("iExcess #%lu: excess=%.1fA over %.1fA cmd — inner PID integrator reset",
                                     (unsigned long)g_iExcessCount, mExcessEma, setpointLimited);
                // One-shot cv_I drain: IExcessKBleed = 0 snaps cv_I to zero, > 0 subtracts K_bleed × EMA × dt once. Either zeroes
                // the command in one tick (1e9 fall-rate override); duty collapses via the inner-PID reset. Sustained drain is
                // awBleedAmpS in the bumpless block; the final reseed is the unified falling-edge reseed when ALL protections clear.
                if (IExcessKBleed <= 0.0f) {
                  cv_I = 0.0f;
                } else {
                  cv_I = fmaxf(0.0f, cv_I - IExcessKBleed * mExcessEma * actualDtSec);
                }
                iExcessActive = true;
              }

              // While latched: re-apply the proportional cap each tick on the averaged EMA (honest capReason, no ripple chatter).
              // Not a hard 0-cap (deeper recovery undershoot). Release below E × IExcessRelFrac.
              if (iExcessActive) {
                float ieCap = fmaxf(0.0f, fastOvBaseCap - K_IE * mExcessEma);
                if (ieCap < fastOvCurrentCap) { fastOvCurrentCap = ieCap; capReasonTick = CAP_REASON_IEXCESS; }
                fastOvClampActive = true;
                if (mExcessEma < E * IExcessRelFrac) {
                  iExcessActive = false;   // release; unified reseed handles cv_I
                }
              }
            }
            g_mExcessEma = mExcessEma;       // CV detector owns the export while its gate is open (script.js iExcessLiveOnCsv1 mirrors this gate)
            g_iExcessThreshold = E;
          } else {
            // Gate closed: release and reseed the EMA so a later CV entry or setpoint step carries no stale charge into a fire.
            iExcessActive = false;
            mExcessEma = 0.0f;
            // Do NOT clear postProtMismatch here: at a low/float setpoint the field cut sags battV below the arm line mid-wind-down,
            // and wiping the guard on that dip let iExcess re-fire when the gate re-opened. It self-clears via decay/timeout.
            g_mExcessEma = 0.0f;
            g_iExcessThreshold = 0.0f;
          }
          g_iExcessActive = iExcessActive;
          g_iExcessDutyCap = 100.0f;  // retired
        }

        // ── iExcess supervisor — BULK sub-mode (Group 3, current-control phase) ──────
        // Sibling of the CV detector for bulk: gate is the strict complement (IBV ≤ target − IExcessArmMarginV), so they hand off
        // with no gap. Compares against i_ceiling_pre_ov with a looser fraction, so only a true overshoot above the ceiling fires;
        // binds via fastOvCurrentCap relative to the ceiling (zeroing cv_I can't collapse a saturated bulk setpoint). See
        // iExcess_Redesign_Spec.md.
        {
          const float K_IE = 1.0f;
          static bool iExBulkActive = false;     // latched fire state
          static float mExcessEmaBulk = 0.0f;    // EMA of signed deviation iActual − i_ceiling_pre_ov (A)
          static bool postProtMismatchBulk = false;  // wind-down gate (mirror of the CV detector's postProtMismatch): suppress a redundant bulk re-fire during the field-TC tail AFTER a clamp releases
          static uint32_t postProtClearMsBulk = 0;   // millis the wind-down safety cap is measured from (refreshed every clamped tick → counts from the release edge)

          if (IExcessEnable && testProtectionsEnabled && !TuningMode && !batteryHealthTestActive && voltageControlActive
              && MaintainMode == 0 && !zeroFloatActive
              && (IBV <= ChargingVoltageTarget - IExcessArmMarginV)) {
            // Affine trip line vs the commanded ceiling plus IExcessCcOffsetA: the UI keeps IExcessFracBulk = IExcessFrac, so it
            // runs PARALLEL to the CV line that many amps above it, catching only overshoots above the ceiling.
            float floorBulk = IExcessFloorA;
            float E = fmaxf(floorBulk, fminf(IExcessFracBulk * i_ceiling_pre_ov + IExcessBaseA + IExcessCcOffsetA, IExcessCeilA));
            // Field bled >= tau: EMA-gated hold release — same rationale as the CV detector above.
            if (tauReleaseNow && mExcessEmaBulk <= E) iExBulkActive = false;

            // Post-protection wind-down gate (bulk mirror of the CV postProtMismatch): after a clamp or Hi->Lo
            // glide releases, field current lags above the lowered ceiling ~1 field TC. Hold until the excess
            // decays inside E, bounded by a safety cap so a sustained over-current still fires.
            const uint32_t kPostProtMismatchMaxMs = (uint32_t)fmaxf(150.0f, 3.0f * (float)fieldDecayTauMs);  // 3x worst-case field-drain time; backstop only, normal release is the decay test
            bool clampOwnedBulk = fastOvClampActive || iExBulkActive || modeCapGlideSuppress;  // include bulk iExcess's OWN clamp so the guard arms after a bulk-iExcess-only cut
            if (clampOwnedBulk) {
              postProtMismatchBulk = true;
              postProtClearMsBulk  = currentMillis;
            } else if (postProtMismatchBulk) {
              bool decayed  = (MeasuredAmps - i_ceiling_pre_ov) <= E;
              bool timedOut = (currentMillis - postProtClearMsBulk) > kPostProtMismatchMaxMs;
              if (decayed || timedOut) postProtMismatchBulk = false;
            }

            if (!iExBulkActive && (clampOwnedBulk || postProtMismatchBulk)) {
              // A clamp (load dump / mode-cap glide) owns the drop, or its wind-down tail still reads above the
              // lowered ceiling: that over-ceiling reading is the protection's own doing, so hold the EMA at 0
              // to avoid a redundant bulk trip (the glide case was a mode-switch false trip).
              mExcessEmaBulk = 0.0f;
            } else {
              float tauSec = IExcessTau * 0.001f;
              float alpha  = actualDtSec / (tauSec + actualDtSec);
              mExcessEmaBulk += alpha * ((MeasuredAmps - i_ceiling_pre_ov) - mExcessEmaBulk);  // vs commanded ceiling, not slewed setpoint

              if (!iExBulkActive && mExcessEmaBulk > E) {
                cv_I_aw_cap = cv_I;         // cap bumpless tracker ceiling to pre-event level
                g_iExcessCount++;           // shared by the CV + bulk detectors
                currentPID.ResetIntegratorTo(0.0);
                queueConsoleMessageF("iExcess (bulk) #%lu: excess=%.1fA over %.1fA ceiling — inner PID integrator reset",
                                     (unsigned long)g_iExcessCount, mExcessEmaBulk, i_ceiling_pre_ov);
                if (IExcessKBleed <= 0.0f) {
                  cv_I = 0.0f;
                } else {
                  cv_I = fmaxf(0.0f, cv_I - IExcessKBleed * mExcessEmaBulk * actualDtSec);
                }
                iExBulkActive = true;
              }

              if (iExBulkActive) {
                float ieCap = fmaxf(0.0f, i_ceiling_pre_ov - K_IE * mExcessEmaBulk);  // ceiling-relative — must bite below the bulk ceiling
                if (ieCap < fastOvCurrentCap) { fastOvCurrentCap = ieCap; capReasonTick = CAP_REASON_IEXCESS_BULK; }
                fastOvClampActive = true;
                if (mExcessEmaBulk < E * IExcessRelFrac) {
                  iExBulkActive = false;    // release; unified reseed handles cv_I
                }
              }
            }
            g_mExcessEma = mExcessEmaBulk;   // bulk owns the export while its gate is open (script.js iExcessLiveOnCsv1 mirrors this gate)
            g_iExcessThreshold = E;
          } else {
            // Gate closed (near/above target — CV iExcess owns this regime, or protections off).
            // Leave g_mExcessEma / g_iExcessThreshold untouched so the CV detector's export stands.
            iExBulkActive = false;
            mExcessEmaBulk = 0.0f;
            // Keep postProtMismatchBulk: a gate dip during the wind-down must not wipe the guard.
          }
          g_iExcessBulkActive = iExBulkActive;  // export for PID/CV log flag + dashboard
        }

        // ── iExcess live-sparkline aggregation: per-CSV1-frame peak EMA / min E while armed ──────────
        // (g_iExcessThreshold > 0). The CSV1 sender ships and resets these; "fired" colour comes from PROT_EVT_IX.
        if (g_iExcessThreshold > 0.0f) {
          if (!g_iExcessArmedWin) {
            g_mExcessEmaPeak = g_mExcessEma;
            g_iExcessThreshWinMin = g_iExcessThreshold;
            g_iExcessArmedWin = true;
          } else {
            if (g_mExcessEma > g_mExcessEmaPeak) g_mExcessEmaPeak = g_mExcessEma;
            if (g_iExcessThreshold < g_iExcessThreshWinMin) g_iExcessThreshWinMin = g_iExcessThreshold;
          }
        }

        // ── Load dump detection — dBcur/dt positive spike in CV mode ─────────────────
        // Three-tier cascade stepped per INA228 reading in loadDumpStep(); this block consumes trips since the last tick.
        // Logged slope noise peaked ~3.5 kA/s, so tier 1 (7 kA/s, FET disconnects) and tiers 2/3 (5 kA/s on adjacent
        // readings, relay contacts) sit clear of it. LoadDumpEnable is the only disarm (global toggle does not apply).
        {
          // At this scope so the gate-closed else can clear it; otherwise a dump active at CV exit skips the
          // rising-edge snap and count for the next event.
          static bool ldWasActive = false;
          if (LoadDumpEnable && voltageControlActive && inaFastModeActive && HAS_BATT_SHUNT) {  // no shunt → dBcur/dt is noise; fast-OV dV/dt is the load-dump backstop
            g_ldArmed = true;
            // Latch: a reading tripped since the last tick, even if a later one cleared. Now: the latest
            // reading still trips — holds the clamp across a tick that saw no new reading.
            bool ldNow = g_ldTripLatch || g_ldTripNow;
            float ldDbdt = g_ldTripDbdt;
            g_ldTripLatch = false;
            if (ldNow) {
              fastOvCurrentCap = 0.0f;
              capReasonTick = CAP_REASON_LOADDUMP;  // hard cutoff — always the binding cap
              setpointLimited = 0.0f;
              fastOvClampActive = true;
              if (!ldWasActive) {
                cv_I = 0.0f;            // snap voltage-loop integrator on rising edge
                // Also collapse the inner PID integrator: wound up, it grinds duty down at ~40%/s instead of
                // reaching MinDuty in 1-2 cycles.
                currentPID.ResetIntegratorTo(0.0);
                g_loadDumpCount++;
                queueConsoleMessageF("Load dump #%lu detected — output cut, inner PID integrator reset (dBcur/dt=%.0f A/s)",
                                     (unsigned long)g_loadDumpCount, ldDbdt);
              }
            }
            // Falling-edge cv_I reseed: the unified reseed in the bumpless tracker block.
            g_loadDumpActive = ldNow;
            ldWasActive = ldNow;
          } else {
            // Gate closed: zero counters, latch and trip now so the next gate-open starts fresh.
            loadDumpReset();
            ldWasActive = false;
            g_loadDumpActive = false;
          }
        }

        // Re-evaluate govMode now that iExcess / load dump may have set fastOvClampActive (the first decision saw
        // only fastOV): snap duty to the floor in one tick instead of slewing at DutyRampRate.
        // BYPASS still respects rpmMinDuty/MinDuty via governor_apply's pre-clamp.
        if (fastOvClampActive && voltageControlActive) {
          govMode = GOV_BYPASS_SLEW;
        }

        // ── Apply fastOvCurrentCap to uTargetAmps (fastOV + iExcess + load dump) ──
        uTargetAmps = fminf((float)uTargetAmps, fastOvCurrentCap);

        // Thermal derate is the binding ceiling if uTargetAmps survived every lowering stage unchanged; read next tick
        // by thermalAccuracyScore_tick as its authority gate. 2 A floor: a penalty that large exists only while the
        // REVERSE PID is actively holding temperature down.
        g_thermalOwnsCeiling = (thermalPenaltyAmps > 2.0f)
                               && ((float)uTargetAmps >= g_I_cap - thermalPenaltyAmps - 0.5f);

        // ── CV command ceiling: Icv/cv_I is an ALTERNATOR-current command; uTargetAmps already folds in
        // the RPM/thermal/user/battery-charge/fastOV limits.
        float icvCeil = (float)uTargetAmps;

        // TargetVoltageMode: CV at a user voltage target; all current limits stay active.
        // cv_I is seeded by bumpless transfer on CV entry — no explicit reseed needed.
        if (TargetVoltageMode == 1) {
          // Do NOT touch inBulkStage / inAbsorptionStage: updateChargingStage() is suppressed while TVMode=1,
          // and clearing them falls through to FLOAT on TVMode exit.
          ChargingVoltageTargetReq = TargetVoltageSetpoint;   // slewed into ChargingVoltageTarget below
          if (enteringTargetVoltageMode) {
            pidLog_enteringTargetVoltageMode = 1;
            queueConsoleMessageF("TargetVoltageMode: active, target=%.2fV", TargetVoltageSetpoint);
          }
        }

        // False only in idle (UseFloat=0 post-absorption); TVMode and MaintainMode force it true.
        voltageControlActive = !inIdleStage;
        if (TargetVoltageMode == 1) voltageControlActive = true;
        if (MaintainMode == 1) {
          // Ceiling enforcer: PI runs at BulkVoltage but uTargetAmps=0 caps Icv→0, so setpoint stays 0; Groups 1/2 arm.
          voltageControlActive = true;
          ChargingVoltageTargetReq = BulkVoltage;
          if (enteringMaintainMode) {
            queueConsoleMessage("MaintainMode: active, targeting 0 net battery amps");
          }
        }
        // CV stress test (commissioning stage 8 / Tuning ▸ Stress Test): pin CV at its computed achievable target so the
        // throttle snap pushes the bus over a REACHABLE setpoint. All current/thermal/OV limits stay active.
        {
          static bool lastCvStressForce = false;
          if (cvStressForceCV) {
            // Bumpless handoff onto the pinned target: cv_I parks at ~icvHi − Kp·boost·e while Icv rails, so
            // without a reseed the P-boost (~19 A) drops out as the target lands (0.4 V dip). Seed against the
            // PINNED target's error, capped at delivered current so the handoff can only shed.
            if (!lastCvStressForce && lastVoltageControlActive) {
              float e_seed = cvStressTargetV - IBV;
              float seedHi = (e_seed < 0.0f) ? fminf((float)uTargetAmps, g_pidI_filtered) : (float)uTargetAmps;
              cv_I = clamp_f(g_pidI_filtered - VoltageKp_active * cvRecovBoostMult(e_seed) * e_seed, 0.0f, seedHi);
            }
            voltageControlActive = true;
            ChargingVoltageTargetReq = cvStressTargetV;
          }
          lastCvStressForce = cvStressForceCV;
        }
        // lastVoltageControlActive is a global so the MANUAL branch, cvYieldToTest branches and early exits
        // (which never reach this line) can reset it.
        bool enteringCV = (!lastVoltageControlActive && voltageControlActive);
        lastVoltageControlActive = voltageControlActive;

        // ===== WAVEFORM GENERATOR: voltage square-wave generator (CVTuningMode) =====
        // Steps ChargingVoltageTargetReq between base (LOW) and base + amplitude (HIGH) so the
        // CV loop step response (settling time, overshoot) can be measured and scored.
        // Only runs in the NORMAL AUTO path — incompatible with inner-loop TuningMode.
        static float cvWaveReqOut = NAN;  // Req as the wave left it, after the DVCC clamp
        bool cvWaveWroteReq = false;
        {
          static bool lastCVTuningMode = false;

          // Discard accumulator on CVTuningMode turn-off — commit is always manual. Do NOT snap the target back
          // to base: a HIGH-phase exit would drop it below actual voltage and trip relative fast-OV / iExcess;
          // the wind-down glides it back at vTgtRampDn.
          if (lastCVTuningMode && !CVTuningMode) {
            cvTuningScore = {};
            cvWaveExitWindDown = true;
          }
          lastCVTuningMode = (CVTuningMode != 0);

          if (manualCommitCVTuningRequested) {
            manualCommitCVTuningRequested = false;
            if (cvTuningScore.scoredHighCount >= 1) {
              commitCVTuningRecord();
            } else {
              queueConsoleMessage("CVTuningScore: commit rejected — no scored HIGH phases yet");
            }
          }

          if (CVTuningMode && voltageControlActive) {
            // Capture base once per test. Base is ALWAYS ChargingVoltageTargetReq, never the slewed target: a test
            // started mid wind-down would latch the old HIGH peak as base and ratchet the wave toward OV.
            // Req still equal to the wave's own last output means a Commit/Restart reset the score mid-test (the stage
            // machine is suppressed in CVTuningMode, so nothing rewrote it): keep the base, or a HIGH-half press
            // re-captures base + amplitude. An override (TVMode/Maintain/stress) rewrites Req every tick and is captured.
            if (!cvTuningScore.testStarted) {
              if (ChargingVoltageTargetReq != cvWaveReqOut) cvBaseTarget = ChargingVoltageTargetReq;
              cvTuningScore.battVAtStart = IBV;
              cvTuningScore.socAtStart = (float)SOC_percent / 100.0f;
              cvTuningScore.lastToggleMs = currentMillis;
              cvTuningScore.waveHigh = false;  // start in LOW phase (at normal setpoint); test steps UP
              cvTuningScore.phaseStartMs = currentMillis;
              cvTuningScore.testStarted = true;
            }

            // Commit on parameter change if enough cycles scored; keep the phase and restart the half-period
            // timer so the next toggle is a full half-period away.
            if (cvTuningParamChanged) {
              bool wasHigh = cvTuningScore.waveHigh;
              if (cvTuningScore.scoredHighCount >= 1) commitCVTuningRecord();
              else cvTuningScore = {};
              cvTuningScore.waveHigh = wasHigh;
              cvTuningScore.lastToggleMs = currentMillis;
              cvTuningScore.testStarted = true;
              cvTuningParamChanged = false;
            }

            // cvWavePeriodSec is the full period
            uint32_t halfPeriodMs = (uint32_t)cvWavePeriodSec * 500UL;
            if (currentMillis - cvTuningScore.lastToggleMs >= halfPeriodMs) {
              bool goingHigh = !cvTuningScore.waveHigh;
              cvTuningScore.waveHigh = goingHigh;
              cvTuningScore.lastToggleMs = currentMillis;
              cvTuningScore.halfPeriodCount++;
              // Initial LOW half-period is unscored ring-in. phaseStartMs > 0 stops a never-started HIGH
              // phase from being finalized.
              if (cvTuningScore.halfPeriodCount >= 1) cvTuningScore.ringInDone = true;

              if (goingHigh && cvTuningScore.ringInDone && cvTuningScore.phaseStartMs > 0) {
                // Finalize scored LOW phase (step-down response)
                if (!cvTuningScore.lowPhaseSettled) {
                  cvTuningScore.totalLowSettlingTimeSec += (float)cvWavePeriodSec / 2.0f;
                }
                cvTuningScore.scoredLowCount++;
                // (totalLowIntOvVs accumulated tick-by-tick in LOW phase block below)
              }
              if (goingHigh && cvTuningScore.ringInDone) {
                // cvBaseTarget is NOT refreshed here, so the wave can't ratchet up when a half-period ends before the slew finishes.
                cvTuningScore.phaseStartMs = currentMillis;
                cvTuningScore.phaseSettled = false;
                cvTuningScore.consecutiveInBand = 0;
                cvTuningScore.reachedTargetMs = 0;   // arm steady-state p2p for this HIGH phase
                cvTuningScore.p2pCaptured = false;
                cvTuningScore.fastOvSnap = g_fastOvClampCount;
                cvTuningScore.iExcessSnap = g_iExcessCount;
                cvTuningScore.loadDumpSnap = g_loadDumpCount;
                cvTuningScore.hardOcSnap = g_hardOCCount;
              }
              if (!goingHigh && cvTuningScore.ringInDone && cvTuningScore.phaseStartMs > 0) {
                // End of a scored HIGH phase — finalize settling time
                if (!cvTuningScore.phaseSettled) {
                  cvTuningScore.totalSettlingTimeSec += (float)cvWavePeriodSec / 2.0f;  // half-period penalty
                }
                cvTuningScore.scoredHighCount++;
                cvTuningScore.fastOvFires += (uint16_t)(g_fastOvClampCount - cvTuningScore.fastOvSnap);
                cvTuningScore.iExcessFires += (uint16_t)(g_iExcessCount - cvTuningScore.iExcessSnap);
                cvTuningScore.loadDumpFires += (uint16_t)(g_loadDumpCount - cvTuningScore.loadDumpSnap);
                cvTuningScore.hardOcFires += (uint16_t)(g_hardOCCount - cvTuningScore.hardOcSnap);
                cvTuningScore.lowPhaseStartMs = currentMillis;
                cvTuningScore.lowPhaseSettled = false;
                cvTuningScore.lowConsecInBand = 0;
                cvTuningScore.lowCrossedBelow = false;
                cvTuningScore.lowFastOvSnap = g_fastOvClampCount;
                cvTuningScore.lowIExSnap = g_iExcessCount;
                cvTuningScore.lowLdSnap = g_loadDumpCount;
                cvTuningScore.lowHocSnap = g_hardOCCount;
              }
            }

            // LOW = normal setpoint (battery rests there); HIGH = setpoint + amplitude (scored step-up).
            ChargingVoltageTargetReq = cvTuningScore.waveHigh ? (cvBaseTarget + cvWaveAmplitudeV)
                                                              : cvBaseTarget;   // slew below applies if vTgtRampEnable (study on/off)
            cvWaveWroteReq = true;
          }
        }

        // ── DVCC follow: external charge-voltage limit (CVL) clamp ──────────────────────────
        // After every ChargingVoltageTargetReq writer, so while following no target of any kind exceeds CVL (no
        // exemptions; bench tuning runs with follow off). Clamp-only; transitions ride the slew below.
        // Bypassed while the commissioning wizard owns the battery.
        bool dvccCvlBinding = false;
        if (dvccEn == 1 && dvccState == 3 /*FOLLOWING*/ && !cxOwnsBatteryNow()
            && !isnan(dvccCvlV) && dvccCvlV < ChargingVoltageTargetReq) {
          ChargingVoltageTargetReq = dvccCvlV;
          dvccCvlBinding = true;
        }
        if (cvWaveWroteReq) cvWaveReqOut = ChargingVoltageTargetReq;

        // ── Voltage-target slew (bidirectional) — outer layer; master switch vTgtRampEnable ──
        // Rate-limits ChargingVoltageTarget (read by the CV PI and the RELATIVE OV protections) toward Req; the
        // absolute backstops (AlternatorHardShutdownV, INA228 comparator) are unaffected. Snaps when not in CV or on
        // CV entry; a waveform-exit wind-down always glides back. Manual CV test: cvTestSlewMode (0 Off/1 Default/2
        // Custom) overrides vTgtRampEnable.
        bool cvManualTest = (CVTuningMode != 0 && !g_autoTestActive);
        bool tgtSlewOff   = cvManualTest ? (cvTestSlewMode == 0)
                                         : (vTgtRampEnable == 0 && !g_autoTestActive);
        float vtDflt   = VTGT_RAMP_DEFAULT * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f);
        float cvRampUp = (cvManualTest && cvTestSlewMode == 1) ? vtDflt : vTgtRampUp;
        float cvRampDn = (cvManualTest && cvTestSlewMode == 1) ? vtDflt : vTgtRampDn;
        bool forceSnap = (!voltageControlActive || enteringCV) ||
                         (tgtSlewOff && !cvWaveExitWindDown);
        // One shared slew (slewChargingVoltageTarget, next to tier2ChargeTarget) so tier 2 and this path
        // cannot drift; the snap-to-proximity and bus-paced-floor rationale lives with it.
        slewChargingVoltageTarget(ChargingVoltageTargetReq, forceSnap, cvRampUp, cvRampDn,
                                  (!cvManualTest && (!g_autoTestActive || cvStressForceCV)), actualDtSec);
        if (cvWaveExitWindDown &&
            (!voltageControlActive || ChargingVoltageTarget <= ChargingVoltageTargetReq + 0.001f)) {
          cvWaveExitWindDown = false;
        }

        // ── Voltage target rise governor (inner windup guard) ──
        // Clamps voltageTargetSlewed to IBV + e_needed (the error cv_I can support at the present cap) so the
        // integrator never sees a large up-step; falls are instant. Global only so cvLog_tick can record it.
        if (enteringCV) {
          voltageTargetSlewed = ChargingVoltageTarget;
        }
        if (voltageControlActive) {
          // cvRiseGovEnable=0 (Test Limiters, A/B study) lets the integrator see the full up-step and wind up into OV.
          if ((cvRiseGovEnable || g_autoTestActive) && ChargingVoltageTarget > voltageTargetSlewed + 0.01f) {
            float icvHi_gov = clamp_f(icvCeil, 0.0f, (float)MaxTableValue);  // alternator command ceiling in CV
            float e_needed = (icvHi_gov - cv_I) / VoltageKp_active;
            e_needed = fmaxf(e_needed, 0.02f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f));  // min target lead, per-cell-scaled
            voltageTargetSlewed = fminf(ChargingVoltageTarget,
                                        IBV + e_needed);  // raw INA228 — no filter lag on governor
          } else {
            voltageTargetSlewed = ChargingVoltageTarget;
          }
        }

        // CC/CV phase determination — must be after the target slew above.
        // Bumpless transfer: track cv_I toward the operating-point value when CV is inactive.
        // While CV is active, cv_I_track stays in sync for seamless re-entry.
        {
          static float cv_I_track = 0.0f;
          static uint32_t awSeedProtectStartMs = 0;  // timestamp when last bumpless seed fired

          // ── CV entry bumpless seed: cv_I = g_pidI_filtered − Kp·cvRecovBoostMult(e)·e so Icv ≈ g_pidI_filtered
          // (no setpointLimited step). Clears cv_I_aw_cap so a stale post-event cap can't constrain entry.
          if (voltageControlActive && enteringCV) {
            float e_cv = ChargingVoltageTarget - IBV;  // raw INA228 — bumpless seed uses true voltage
            // Above target the formula banks +Kp*|e| surplus that the integrator hands back as the error closes
            // (overshoot into G2); cap the seed at delivered current so entry can only shed.
            float seedHi = (e_cv < 0.0f) ? fminf((float)uTargetAmps, g_pidI_filtered) : (float)uTargetAmps;
            float seed = clamp_f(g_pidI_filtered - VoltageKp_active * cvRecovBoostMult(e_cv) * e_cv, 0.0f, seedHi);
            cv_I = seed;
            cv_I_track = seed;
            cv_I_aw_cap = (float)MaxTableValue;    // clear AW cap — stale values constrain CV entry
            awSeedProtectStartMs = currentMillis;  // start seed-protection window
            recovActive = false;                   // stale refill must not survive a CV re-entry
            cvStallStartMs = 0;                    // stall dwell likewise — a V0 frozen across CV exit would skip the 3s proof
            cvStallBoost = false;
          }
          // ── Unified protection-release reseed + telemetry export ───────────
          // Falling edge of fastOvClampActive (G1/G2 OV, iExcess, LoadDump all cleared). preEventCvI was captured while
          // unclamped, so chained events don't burn intermediate snapshots; AwSeedProtectMs keeps the per-tick bleed
          // from wiping the reseed. Never on a CV-entry tick — after any gap the entry seed owns cv_I.
          if (voltageControlActive && !enteringCV && g_fastOvClampActive && !fastOvClampActive) {
            g_ovClampRiseMs = 0;  // episode ended — disarm the tau timer until the next rising edge stamps it
            float icvHi_seed = clamp_f(icvCeil, 0.0f, (float)MaxTableValue);  // alternator command ceiling in CV
            // Seed fraction is shunt-gated: with a shunt demandDropA already corrected the base (ReseedFrac); without
            // one seed low (ReseedFracNoShunt) and let the climb floor rediscover demand. A re-fire <4s proves the seed
            // was above demand → rebase on lastSeedA ×0.7. intakeFloor: never seed below the battery's own share.
            float intakeFloor = fminf(fmaxf(battIntakeEma, 0.0f), preEventCvI);
            float seedBase, seedFrac;
            if (rapidReFires == 0) {
              seedBase = preEventCvI;
              seedFrac = HAS_BATT_SHUNT ? ReseedFrac : ReseedFracNoShunt;
            } else {
              seedBase = fminf(lastSeedA, preEventCvI);
              seedFrac = 0.7f;
            }
            cv_I = clamp_f(fmaxf(seedBase - demandDropA, intakeFloor) * seedFrac, 0.0f, icvHi_seed);
            lastSeedA = cv_I;
            if (demandDropA > 0.0f || rapidReFires > 0) {
              queueConsoleMessageF("CV reseed %.0fA: measured load drop -%.0fA, rapid re-fire x%u",
                                   cv_I, demandDropA, (unsigned)rapidReFires);
            }
            if (demandDropA > 0.0f && HAS_BATT_SHUNT) {
              // Consume the correction: snap the loads EMA to present loads so a rapid re-fire measures only NEW
              // departures; otherwise the same drop subtracts twice and zeroes a good seed.
              preEventLoadEma = MeasuredAmps - Bcur;
              preEventLoadMs = currentMillis;
            }
            cv_I_track = cv_I;
            lastReleaseMs = currentMillis;
            awSeedProtectStartMs = currentMillis;     // engage seed-protection window
            postProtectRiseStartMs = currentMillis;   // open fast-rise window (closed by either time cap or voltage gate at slew site)
            // Field-side floor reseed: the clamp ran with the tach floor dropped, so flux decayed toward MinDuty.
            // Onset duty makes ~0 A, so restoring it in one step cannot re-lift the bus.
            float floorSeed = fmaxf(tick.rpmMinDuty, MinDuty);
            if (lastAppliedDuty < floorSeed) {
              currentPID.ResetIntegratorTo((double)floorSeed);
              pidOutput = (double)floorSeed;
              lastAppliedDuty = floorSeed;
            }
            if (cvRecovEnable) {
              recovActive = true;
              recovWalking = false;
              // Use the hold EMA only if re-validated by a settled window since this episode's first fire;
              // otherwise it may remember a departed load. Stale → preEventCvI.
              bool holdFresh = (cvSteadyHoldEma > 0.5f)
                               && cvSteadyHoldMs != 0
                               && (int32_t)(cvSteadyHoldMs - g_ovEpisodeStartMs) > 0;
              float holdBasis = holdFresh ? cvSteadyHoldEma : preEventCvI;
              // Goal drops by the measured demand (floored at the reseed). After a rapid re-fire goal = seed;
              // the starve-walk lifts the ceiling if the bus then stalls low.
              recovCvGoal = (rapidReFires > 0)
                              ? cv_I
                              : clamp_f(fmaxf(holdBasis, preEventIcv) - demandDropA, cv_I, icvHi_seed);  // snapshot NOW — preEvent* refresh to post-seed values next tick
              recovGoalCollapsed = (rapidReFires > 0);
              recovDeficit0 = fmaxf(recovCvGoal - cv_I, 0.5f);
              recovStartMs = currentMillis;
              recovStarveTicks = 0;
              recovHeldTicks = 0;
              recovVRefEma = getFiltV();
            }
          }
          // Rising edge of the unified flag (any protection); must run before g_fastOvClampActive is committed below.
          if (fastOvClampActive && !g_fastOvClampActive) {
            g_fastOvClampCount++;
            g_ovClampRiseMs = currentMillis;  // stamp the episode start for the field-decay-tau early release
            // OV-episode stamp lives here, not the G1/G2 block, so every protection opens it; otherwise an
            // iExcess train runs with P-boost ×4 and a stale hold-EMA goal.
            if (!ovEpisodeActive()) {
              g_ovEpisodeStartMs = currentMillis;
              queueConsoleMessage("OV episode: recovery P-boost paused until 30s after the last fire");
            }
            g_ovEpisodeLastFireMs = currentMillis;
            // Latch pre-cut RPM for the drain-vs-RPM lookup — unless this episode starts inside the
            // prior cut's tach-corruption window (LM2907 false-zero/garbage ramp ~4.6 s): a garbage
            // low read would look up a too-short drain and release with the field still hot. -1 = worst case.
            // An owner-ignored tach (or Limp Home) gives no speed at all: worst case as well.
            bool tachSuspect = IgnoreRPM || LimpHome
                               || ((g_lastProtClampMs != 0)
                                   && ((uint32_t)(currentMillis - g_lastProtClampMs) < PROT_RPM_GRACE_MS + 2000UL));
            g_ovClampRpm = tachSuspect ? -1.0f : RPM;
            // Demand-drop capture: at the rising edge the field hasn't moved, so a jump in (loads EMA − loads now)
            // is load that left. 5 A deadband = Bcur+MeasuredAmps tick noise. A delivery overshoot raises Bcur and
            // MeasuredAmps equally, so it cancels out.
            demandDropA = 0.0f;
            if (reseedCorrEnable && HAS_BATT_SHUNT && preEventLoadMs != 0) {
              demandDropA = preEventLoadEma - (MeasuredAmps - Bcur);
              if (demandDropA < 5.0f) demandDropA = 0.0f;
              // The hold EMA's premise (this current holds target) died with the departed load —
              // correct it now, or the release goal re-aims the refill at the pre-drop hold.
              if (demandDropA > 0.0f) cvSteadyHoldEma = fmaxf(cvSteadyHoldEma - demandDropA, 0.0f);
            }
            // 4 s window: the cut → valley → climb → shed echo measured 1.55-1.7 s release-to-fire, so a shorter
            // window resets the ratchet every lap. Independent events are minutes apart.
            rapidReFires = (reseedCorrEnable && lastReleaseMs != 0
                            && (uint32_t)(currentMillis - lastReleaseMs) < 4000UL)
                             ? (uint8_t)((rapidReFires < 20) ? rapidReFires + 1 : 20)
                             : 0;
            recovActive = false;  // a new fire cancels the refill; the mid-heal cv_I becomes the next reseed base (de-escalation ratchet)
          }
          g_fastOvCurrentCap = fastOvCurrentCap;  // export unified cap (post all supervisors)
          g_fastOvCapReason = capReasonTick;      // export reason atomically with the cap — CV log reads a coherent pair
          g_fastOvClampActive = fastOvClampActive;  // commit unified flag for next tick
          if (fastOvClampActive) g_lastProtClampMs = currentMillis;  // stamps the RPM-dropout grace window (buildTickSnapshot)
          g_cvRecovActive = recovActive;            // export refill-active state for cvLog flags b7
          // Latch the binding protection into the Plots-tab marker bitmask (consumed + cleared by the CSV1 sender).
          switch (capReasonTick) {
            case CAP_REASON_KHARD_G1:
            case CAP_REASON_KHARD_G2:     g_protEventLatch |= PROT_EVT_OV; break;
            case CAP_REASON_IEXCESS:
            case CAP_REASON_IEXCESS_BULK: g_protEventLatch |= PROT_EVT_IX; break;
            case CAP_REASON_LOADDUMP:     g_protEventLatch |= PROT_EVT_LD; break;
            default: break;
          }
          bool seedProtected = (AwSeedProtectMs > 0) && (awSeedProtectStartMs != 0) && ((currentMillis - awSeedProtectStartMs) < (uint32_t)AwSeedProtectMs);
          if (!seedProtected) awSeedProtectStartMs = 0;  // a millis() wrap must not reopen the window

          // Anti-windup ceiling: bleeds down while fastOV is active so the bumpless tracker can't re-wind cv_I to the
          // bulk level after each overshoot; recovers gradually after. Rates are fractions of MaxTableValue per second
          // so aggression is independent of alternator size.
          float awBleedAmpS = AwBleedRate * (float)MaxTableValue;
          float awRecoverAmpS = AwRecoverRate * (float)MaxTableValue;

          if (fastOvClampActive && voltageControlActive) {
            cv_I_aw_cap = fmaxf(0.0f, cv_I_aw_cap - awBleedAmpS * actualDtSec);
          } else {
            cv_I_aw_cap = fminf(cv_I_aw_cap + awRecoverAmpS * actualDtSec, (float)MaxTableValue);
          }
          g_cvAwRecovering = (cv_I_aw_cap < (float)MaxTableValue - 0.5f);  // post-trip hold-down in progress

          float icvHi_bt = fminf(clamp_f((float)uTargetAmps, 0.0f, (float)MaxTableValue), cv_I_aw_cap);
          if (!voltageControlActive) {
            recovActive = false;  // CV exited mid-recovery — bumpless tracker owns cv_I now
            if (!seedProtected) {
              float e_bt = ChargingVoltageTarget - IBV;  // raw INA228 — no filter lag on bumpless tracker
              float cv_I_target = clamp_f(g_pidI_filtered - VoltageKp_active * e_bt, 0.0f, icvHi_bt);
              const float Kt = 2.0f;
              cv_I_track += Kt * (cv_I_target - cv_I_track) * actualDtSec;
              cv_I_track = clamp_f(cv_I_track, 0.0f, icvHi_bt);
              cv_I = cv_I_track;
            } else {
              cv_I_track = cv_I;  // keep tracker in sync during seed-protection window
            }
            g_awState = 4;  // not in CV mode; bumpless tracker owns cv_I
          } else {
            cv_I_track = cv_I;
            // Per-tick bleed during fastOV: the PI fires only every VoltageLoopInterval, but wind-up must shrink every
            // control tick. Suppressed in the seed-protection window to preserve the seed.
            if (fastOvClampActive && !seedProtected) {
              cv_I = fmaxf(0.0f, cv_I - awBleedAmpS * actualDtSec);
              cv_I_track = cv_I;
              g_awState = 3;  // actively bleeding cv_I due to fastOV protection
            }
          }
        }

        if (voltageControlActive) {
          bool cvLoopFired = enteringCV || ((currentMillis - lastVoltageLoopMs) >= VoltageLoopInterval);
          vlHasPrev = vlHasPrevIn;

          // ===== CV D term — sliding-window derivative, recomputed every output tick =====
          // kdTrim is a POSITION subtracted at the Icv output, never integrated into cv_I. Runs BEFORE the PI block so
          // its anti-windup reads this tick's fresh slope/trim.
          {
            static const uint8_t KDBUF_N = 64;         // spans a 100 ms window down to ~1.5 ms/tick
            static uint32_t kdBufT[KDBUF_N];
            static float    kdBufV[KDBUF_N];
            static uint8_t  kdBufN = 0, kdBufHead = 0;
            static bool kdOutUp = false, kdOutDn = false;   // deadband-latch state
            if (enteringCV) {
              kdBufN = 0; kdBufHead = 0;
              kdOutUp = false; kdOutDn = false;
              cvDSlope = 0.0f; g_cvKdTrimLive = 0.0f; g_kdTrimThisTick = 0.0f;  // no stale slope/trim across a CV re-entry
            }
            kdBufT[kdBufHead] = currentMillis;
            kdBufV[kdBufHead] = g_cvKdFiltV;
            kdBufHead = (uint8_t)((kdBufHead + 1) % KDBUF_N);
            if (kdBufN < KDBUF_N) kdBufN++;

            float kdTrim = 0.0f;
            if (!enteringCV) {
              // Backward difference over the newest sample at least one window old; cvDSlope holds 0 until the buffer
              // spans a window (avoids a noisy sub-window difference right after entry).
              uint32_t effWindowMs = (uint32_t)constrain((int)VoltageLoopInterval, 20, 200);  // capped to what the ring holds
              float vOld = 0.0f; uint32_t oldAge = 0; bool spanned = false;
              float vOldest = 0.0f; uint32_t oldestAge = 0;
              for (uint8_t i = 0; i < kdBufN; i++) {
                uint8_t idx = (uint8_t)((kdBufHead + KDBUF_N - 1 - i) % KDBUF_N);   // newest → oldest
                uint32_t age = currentMillis - kdBufT[idx];
                vOldest = kdBufV[idx]; oldestAge = age;                             // last write wins = oldest walked
                if (age >= effWindowMs) { vOld = kdBufV[idx]; oldAge = age; spanned = true; break; }
              }
              // Fast ticks can fill the ring before it spans a full window; once full, use the widest available window
              // rather than stalling the slope.
              if (!spanned && kdBufN >= KDBUF_N && oldestAge > 0) { vOld = vOldest; oldAge = oldestAge; spanned = true; }
              if (spanned && oldAge > 0) {
                float slopeCeil = CvKdSlopeCeil;  // V/s real per-bus (WYSIWYG) — no ×V/12 at use
                cvDSlope = constrain((g_cvKdFiltV - vOld) / ((float)oldAge / 1000.0f), -slopeCeil, slopeCeil);
                rollUpdate(ROLL_CVSLOPE, cvDSlope);   // D-term deadband-tuning readout (10 s peak raw rise)
              }
              // Deadband is a LINE in operating current (ripple slope noise grows with output, like the G3 trip line),
              // evaluated at the slow slew-limited command so a transient can't drag it up. The latches serve only legacy
              // full-slope mode (re-arm at half so ripple across the edge can't jab the field) and track every tick.
              float kdDb = clamp_f(CvKdDeadbandVps + CvKdDbSlope * setpointLimited, CvKdDbFloor, CvKdDbCeil);
              if (cvDSlope >  kdDb)             kdOutUp = true;
              else if (cvDSlope <  0.5f * kdDb) kdOutUp = false;
              if (cvDSlope < -kdDb)             kdOutDn = true;
              else if (cvDSlope > -0.5f * kdDb) kdOutDn = false;
              // Held off while any step/probe test owns the field (a mid-probe trim corrupts plant fits)
              // and under cvHelpersEnabled so OFF gives clean symmetric PI for tuning.
              bool fitProbeActive = (fieldCurveActive != 0) || (systemIDActive != 0) ||
                                    resTestActive || batteryHealthTestActive || cvPlantFitActive ||
                                    (altSweepActive != 0);
              if (cvHelpersEnabled && !fitProbeActive
                  && (CvKdArmV <= 0.0f || (voltageTargetSlewed - g_cvKdFiltV) < CvKdArmV)) {
                float slopeExcess = 0.0f;
                if (CvKdExcessMode) {
                  // Trim ∝ excess over the line, continuous from zero. The legacy full-slope form steps by Kd×deadband at
                  // engagement, which behind the ~0.2 s field lag self-sustained a 1.4 Hz hold limit cycle.
                  if (cvDSlope > kdDb) {
                    slopeExcess = cvDSlope - kdDb;                       // brake
                  } else if (!CvKdOneSided && cvDSlope < -kdDb
                             && (voltageTargetSlewed - g_cvKdFiltV) > 0.0f) {
                    // Two-sided add only BELOW target: damps the undershoot ring without re-lifting an above-target overshoot.
                    slopeExcess = cvDSlope + kdDb;                       // boost (below target only)
                  }
                } else {
                  if (kdOutUp && cvDSlope > 0.0f) {
                    slopeExcess = cvDSlope;                              // legacy brake: full slope
                  } else if (!CvKdOneSided && kdOutDn && cvDSlope < 0.0f
                             && (voltageTargetSlewed - g_cvKdFiltV) > 0.0f) {
                    slopeExcess = cvDSlope;                              // legacy boost (below target only): full slope
                  }
                }
                // g_huntKdScale: damper D lever (0 = D paused/pocketed) — the single choke point, so the
                // anti-windup and both Icv recomputes downstream all see the scaled trim
                kdTrim = clamp_f(VoltageKd_active * g_huntKdScale * slopeExcess, -CvKdMaxTrimA, CvKdMaxTrimA);  // flat cap → per-cell-equal knee
              }
            }
            g_cvKdTrimLive = kdTrim;   // held position: consumed by the PI-block anti-windup and both Icv recomputes

            // D-term engagement telemetry: episode = trim onset with a ≥1 s quiet gap between engagements.
            if (kdTrim != 0.0f) {
              static uint32_t lastKdTrimMs = 0;
              if (currentMillis - lastKdTrimMs > 1000) {
                g_cvKdCount++;
                ovTel().kdEventCount++;
              }
              lastKdTrimMs = currentMillis;
              g_kdTrimThisTick = kdTrim;         // captured for cvLog; cleared by cvLog_tick after logging
              g_protEventLatch |= PROT_EVT_KD;   // Plots-tab D-term shading (not a protection — see PROT_EVT_KD)
            }
          }

          if (cvLoopFired) {
            uint32_t prevVoltageLoopMs = lastVoltageLoopMs;
            lastVoltageLoopMs = currentMillis;
            if (prevVoltageLoopMs != 0) {
              uint32_t actualIv = currentMillis - prevVoltageLoopMs;
              g_voltLoopActualIntervalMs = (uint16_t)(actualIv < 65535u ? actualIv : 65535u);
            } else {
              g_voltLoopActualIntervalMs = 0;
            }
            g_voltLoopLogIntervalMs = vlHasPrev ? g_voltLoopActualIntervalMs : 0;  // before voltLoop_record sets vlHasPrev
            voltLoop_record(currentMillis);  // feed the CV-interval ladder (vl_*); vlHasPrev re-baselines on CV exit
            pidLog_voltageLoopRanThisTick = 1;
            g_cvLogVoltLoopFired = true;
            pidLog_enteringCV = enteringCV ? 1 : 0;

            float e = voltageTargetSlewed - IBV;  // raw INA228 — no filter lag on PI error (governor output)
            // Steady CV holding-current estimate (recovCvGoal basis). Updates ONLY in clean CV near target (no overshoot
            // droop, no recovActive re-samples), so it tracks the true hold, not the bled-down snapshot.
            if (voltageControlActive && !fastOvClampActive && !recovActive
                && fabsf(e) < 0.20f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f)) {
              float aH = (float)g_voltLoopActualIntervalMs / (1500.0f + (float)g_voltLoopActualIntervalMs);
              cvSteadyHoldEma = (cvSteadyHoldEma < 0.5f) ? cv_I
                                                         : cvSteadyHoldEma + aH * (cv_I - cvSteadyHoldEma);
              cvSteadyHoldMs = currentMillis;
              // Battery's charging share at target — same clean window, but not while a load-serve
              // boost holds the bus low with Bcur parked near 0 (would drag the share toward zero).
              if (HAS_BATT_SHUNT && !loadServeActive) {
                battIntakeEma += aH * (Bcur - battIntakeEma);
              }
            }
            // Refill ends when the deficit heals (void on a collapsed goal) or the bus has held ≈target ~1s.
            // Premise-void backstop: satHi freezes cv_I while Icv rails at the goal, so a stale pre-trip goal would cap
            // CV forever. Signature: goal commanded AND DELIVERED (else raising it only banks surplus that dumps into a
            // G2 re-fire), bus low and not rising. It WALKS the goal up; a one-tick release would step by the P+I surplus.
            if (recovActive) {
              float aRef = (float)g_voltLoopActualIntervalMs / (3000.0f + (float)g_voltLoopActualIntervalMs);
              recovVRefEma += aRef * (getFiltV() - recovVRefEma);
              float clsRec = (float)SYSTEM_VOLTAGE_CLASS / 12.0f;
              float shortfallF = voltageTargetSlewed - getFiltV();
              // "Not rising" = bus within the answer band of the ~3s reference — a DELTA, not a per-tick slope sign: idle
              // ripple (±0.09 V/s) flips a slope EMA and starves the walk. A genuine answer clears the band in ~30 ms. The
              // band WIDENS beyond cvRecovDeepBandV of shortfall, where only a hard takeoff is an answer.
              float busAnswerV = (0.05f + 0.08f * fmaxf(0.0f, shortfallF / clsRec - cvRecovDeepBandV)) * clsRec;
              // Shallow closure-horizon clause: over the band is not an answer unless the rise would close the remaining
              // shortfall within ~3 reference windows (a ceiling-limited 20 mV/s creep otherwise starves the walk).
              // Dimensionless, so class/chemistry/size-blind. Deep band exempt.
              float busDeltaV = getFiltV() - recovVRefEma;
              bool busAnswering = (busDeltaV >= busAnswerV)
                                  && (shortfallF >= cvRecovDeepBandV * clsRec || busDeltaV * 3.0f >= shortfallF);
              bool delivering = (setpointLimited - g_pidI_filtered) < fmaxf(5.0f, 0.15f * setpointLimited);
              // Rail test against the FLARED ceiling Icv was clamped to last tick, 1A slack: the flare moves with ripple,
              // so an exact-rail compare would flicker the 5-tick resume gate.
              bool starved = delivering && (Icv >= cvRecovFlaredCeil(recovCvGoal, shortfallF) - 1.0f)
                             && (e > 0.0f) && !busAnswering
                             && ((uint32_t)(currentMillis - recovStartMs) > 2000UL);
              recovStarveTicks = starved ? (uint8_t)(recovStarveTicks + 1) : 0;
              // Recovered exit: bus within steady-hold jitter of target ~1s means the climb is over, whatever the deficit
              // says; otherwise a plant that heals needing less current latches a stale ceiling.
              bool heldAtTarget = (e <= 0.025f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f))
                                  && ((uint32_t)(currentMillis - recovStartMs) > 2000UL);
              recovHeldTicks = heldAtTarget ? (uint8_t)(recovHeldTicks + 1) : 0;
              // A collapsed goal (goal = seed) starts already "healed", so the deficit exit would drop the ceiling one tick
              // after release; hold the window until held-at-target / walk-to-ceiling / a new fire.
              if ((cv_I >= recovCvGoal - 0.5f && !recovGoalCollapsed) || recovHeldTicks >= 10) {
                // Bumpless handoff: cv_I above the flared ceiling is refill banked before the flare tightened and would
                // step the setpoint at release.
                cv_I = fminf(cv_I, cvRecovFlaredCeil(recovCvGoal, shortfallF));
                recovActive = false;
                recovWalking = false;
              } else if (!recovWalking && recovStarveTicks >= 5) {
                recovWalking = true;
                queueConsoleMessage("CV recovery: pre-trip current no longer holds the target — walking the ceiling up");
              }
              // The walk PAUSES the tick the signature breaks and needs 5 fresh ticks to resume — NOT latched; a latched
              // walk kept ramping after the bus rose and banked surplus that re-fired G2.
              if (recovActive && recovWalking && recovStarveTicks >= 5) {
                float ceilNow = clamp_f(icvCeil, 0.0f, (float)MaxTableValue);
                // 0.5A/s + 1%-of-ceiling/s (~1A/s on 50A): slower than the battery's surface-charge refill, since "ceiling
                // too low" and "valley not yet backfilled" look identical near target. Beyond cvRecovDeepBandV of shortfall
                // no valley explains the level, so the rate ramps to ×cvRecovDeepMult at 2× the band.
                float walkMult = 1.0f + (cvRecovDeepMult - 1.0f)
                                 * clamp_f((shortfallF / clsRec - cvRecovDeepBandV) / fmaxf(cvRecovDeepBandV, 0.05f), 0.0f, 1.0f);
                recovCvGoal += walkMult * (0.5f + 0.010f * ceilNow) * ((float)g_voltLoopActualIntervalMs * 0.001f);
                if (recovCvGoal >= ceilNow) {
                  cv_I = fminf(cv_I, cvRecovFlaredCeil(recovCvGoal, shortfallF));  // same bumpless handoff; no-op here in practice
                  recovActive = false;
                  recovWalking = false;
                }
              }
            }
            // Load-serve boost: demand that holds target = house loads + battery share at target (battIntakeEma). When it
            // exceeds cv_I with the bus meaningfully low, up-integrate at the refill's boosted rate until the deficit heals
            // or the bus reaches target. Bounded: it multiplies VOLTAGE-error integration, so at target it pushes nothing.
            // Defers to the post-protection refill. Thresholds sit beyond iMeas/Bcur ripple; needs Bcur at control rate.
            if (loadServeBoostEnable && HAS_BATT_SHUNT && inaFastModeActive && !recovActive
                && !fastOvClampActive && !TuningMode && !batteryHealthTestActive
                && MaintainMode == 0 && !zeroFloatActive) {
              float servNeedA = (MeasuredAmps - Bcur) + fmaxf(battIntakeEma, 0.0f);
              bool servDeficit = (servNeedA - cv_I > 10.0f)
                                 && (e > 0.05f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f));
              loadServeTicks = servDeficit ? (uint8_t)((loadServeTicks < 200) ? loadServeTicks + 1 : 200) : 0;
              if (!loadServeActive && loadServeTicks >= 2) {
                loadServeActive = true;
                loadServeGoal = clamp_f(servNeedA, 0.0f, clamp_f(icvCeil, 0.0f, (float)MaxTableValue));
                loadServeDeficit0 = fmaxf(loadServeGoal - cv_I, 0.5f);
                queueConsoleMessageF("CV load pickup: demand %.0fA (loads %.0fA + battery %.0fA) vs cv_I %.0fA — boosting toward %.0fA",
                                     servNeedA, MeasuredAmps - Bcur, fmaxf(battIntakeEma, 0.0f), cv_I, loadServeGoal);
              }
              if (loadServeActive && (cv_I >= loadServeGoal - 0.5f || e <= 0.0f)) loadServeActive = false;
            } else {
              loadServeActive = false;
              loadServeTicks = 0;
            }
            float dtSec = (prevVoltageLoopMs == 0)
                            ? ((float)VoltageLoopInterval / 1000.0f)
                            : ((float)(currentMillis - prevVoltageLoopMs) / 1000.0f);
            dtSec = constrain(dtSec, 0.001f, 0.5f);

            float icvHi = clamp_f(icvCeil, 0.0f, (float)MaxTableValue);  // alternator command ceiling in CV
            // satHi stops cv_I growing but never drains it: after a ceiling drop cv_I would sit at the old ceiling until
            // the bus overshot by the difference over Kp. Pre-protection ceiling only — during a clamp the AW bleed and
            // release reseed own cv_I.
            cv_I = fminf(cv_I, clamp_f(i_ceiling_pre_ov, 0.0f, (float)MaxTableValue));
            // Recovery ceiling: never more than the pre-trip current, flared down over the last cvRecovFlareBandV of
            // approach. In icvHi (not a late fminf on Icv) so satHi stops cv_I integrating against it.
            if (recovActive) icvHi = fminf(icvHi, cvRecovFlaredCeil(recovCvGoal, e));
            float icvLo = 0.0f;

            if (!enteringCV) {
              float p = VoltageKp_active * cvRecovBoostMult(e) * e;  // recovery P-boost scales the proportional term while below target
              float unsat = p + cv_I;
              Icv = clamp_f(unsat, icvLo, icvHi);

              bool satHi = (Icv >= icvHi);
              bool satLo = (Icv <= icvLo);

              // Freeze up-integration while the D term holds the output down, else cv_I winds up invisibly and dumps on release.
              bool kdLimiting = (g_cvKdTrimLive > 0.0f);

              // cvHelpersEnabled OFF → symmetric plain PI (integrator unwinds at the same VoltageKi rate it builds);
              // ON → asymmetric 7× faster unwind above target (aggressive overshoot recovery). See "CV tuning helpers" toggle.
              float KiDown = cvHelpersEnabled ? 7.0f * VoltageKi_active : VoltageKi_active;
              // Deficit-gated refill boost: UP-integration only, through this normal path so every
              // gate below (kdLimiting, slew-starve, satHi, aw cap) paces the refill unchanged.
              float KiUp = VoltageKi_active;
              if (recovActive) {
                KiUp *= 1.0f + (cvRecovKiMax - 1.0f) * clamp_f((recovCvGoal - cv_I) / recovDeficit0, 0.0f, 1.0f);
              } else if (loadServeActive) {
                KiUp *= 1.0f + (cvRecovKiMax - 1.0f) * clamp_f((loadServeGoal - cv_I) / loadServeDeficit0, 0.0f, 1.0f);
              }
              float dI = (e >= 0.0f ? KiUp : KiDown) * e * dtSec;
              // Stall accelerator (same climb floor, outside recovActive) for cv_I stranded short on bare Ki*e. Arms on PROOF:
              // sustained offset AND < 0.05V*class bus movement over the 3s dwell (a DELTA; cvDSlope reads ±0.35V/s on ripple).
              // The duty veto is the safety case: accelerating into a pegged field banks amps that dump when rpm returns.
              float stallOffset = voltageTargetSlewed - getFiltV();
              bool stallOk = !recovActive && !loadServeActive && !fastOvClampActive && !cvWindDownActive
                             && (stallOffset >= 0.10f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f))
                             && (lastAppliedDuty < 0.95f * ccDutyCeiling());
              if (!stallOk) {
                cvStallStartMs = 0;
                cvStallBoost = false;
              } else if (cvStallStartMs == 0) {
                cvStallStartMs = currentMillis;
                cvStallV0 = getFiltV();
              } else if (!cvStallBoost && (uint32_t)(currentMillis - cvStallStartMs) >= 3000UL) {
                if (getFiltV() - cvStallV0 < 0.05f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f)) {
                  cvStallBoost = true;
                  queueConsoleMessage("CV: bus stalled below target with field in reserve — accelerating");
                } else {
                  cvStallStartMs = currentMillis;  // bus IS climbing: re-baseline, keep watching
                  cvStallV0 = getFiltV();
                }
              }
              // Bus-watched climb floor: the error-paced refill trickles a low seed's last amps in at sub-A/s. Gated on
              // projected arrival so the handback stops before the bus lands; freeze/satHi gates still veto it. Outside
              // recovActive it is DEPTH-SCHEDULED (no flare ceiling there; a flat floor coasted +0.56V into G2).
              if ((recovActive || cvStallBoost) && e >= 0.0f) {
                float arriveTauS = 0.10f + 0.0015f * (float)VoltageLoopInterval + 0.001f * VoltageFilterTC;
                bool arriving = (getFiltV() + fmaxf(cvDSlope, 0.0f) * arriveTauS) >= voltageTargetSlewed;
                if (!arriving) {
                  float floorRate = CvRecovClimbRate * (float)MaxTableValue;
                  if (!recovActive) {
                    float clsSt = (float)SYSTEM_VOLTAGE_CLASS / 12.0f;
                    floorRate *= clamp_f((stallOffset / clsSt - 0.10f)
                                         / fmaxf(cvRecovDeepBandV - 0.10f, 0.05f), 0.0f, 1.0f);
                  }
                  dI = fmaxf(dI, floorRate * dtSec);
                }
              }

              bool supervisorLimiting = fastOvClampActive && ((float)uTargetAmps < uTargetRaw_cached - 0.01f);
              bool slewStarved = (setpointLimited < Icv - 2.0f);  // 2A margin: >10x the steady-CV Icv-vs-setpoint jitter (p99 0.2A), so it engages only on genuine slew lag
              // Freeze up-integration while the supervisor caps the ceiling, the D term holds the output down, the slew is
              // still delivering a lower command, or the wind-down owns the ceiling: Icv is held below what the PI wants
              // with e>0, so cv_I would wind up invisibly and dump on release.
              if ((supervisorLimiting || kdLimiting || slewStarved || cvWindDownActive) && dI > 0.0f) {
                g_awState = cvWindDownActive ? 5 : 1;
              } else if (!(satHi && dI > 0.0f) && !(satLo && dI < 0.0f)) {
                cv_I += dI;
                g_awState = 0;
              } else {
                g_awState = 2;  // PID output at ceiling or floor; standard anti-windup
              }

              Icv = clamp_f(VoltageKp_active * cvRecovBoostMult(e) * e + cv_I - g_cvKdTrimLive, icvLo, icvHi);
            }
          }

          pidLog_uTargetBeforeVoltCap = i_ceiling_pre_ov;
          pidLog_uTargetAfterVoltCap = Icv;

        } else {
          pidLog_uTargetBeforeVoltCap = i_ceiling_pre_ov;
          pidLog_uTargetAfterVoltCap = (float)uTargetAmps;
          vlHasPrev = false;  // CV inactive — re-baseline the CV-interval ladder so the off-gap isn't logged
          // A protection-trip CV exit can leave a nonzero trim behind; only the CV branch writes these,
          // so zero them here or CSV1 streams the stale P/D values the whole time CV is off.
          g_cvPTerm = 0.0f;
          g_cvKdTrimLive = 0.0f;
        }

        // Per-tick Icv recompute: P responds every output tick; cv_I updates only on VoltageLoopInterval. The D trim
        // must be subtracted here too, or this recompute wipes it a tick later and negates the D term.
        {
          float e_now = voltageTargetSlewed - IBV;  // raw INA228 — no filter lag on per-tick proportional (governor output)
          float icvHi_tick = clamp_f(icvCeil, 0.0f, (float)MaxTableValue);  // alternator command ceiling in CV
          if (recovActive) icvHi_tick = fminf(icvHi_tick, cvRecovFlaredCeil(recovCvGoal, e_now));    // same flared pre-trip-current ceiling as the PI path
          // Runs on the CV-entry tick too (cv_I is the fresh seed there): nothing else writes Icv on that tick, and the
          // value left from the last CV session skipped this tick's ceilings and, at <= 0.5 A, ended the startup ramp.
          float boost_now = cvRecovBoostMult(e_now);  // recovery P-boost; 1× at/above target
          g_cvPTerm = VoltageKp_active * boost_now * e_now;  // P contribution to Icv, live (CSV1 P/I/D plot) — reflects the boost
          Icv = clamp_f(VoltageKp_active * boost_now * e_now + cv_I - g_cvKdTrimLive, 0.0f, icvHi_tick);
        }

        // ===== CV COMMANDED-TARGET WIND-DOWN GOVERNOR =====
        // The PI cannot track a target DROP inside the OvMeasMarginV band (KiDown×error caps the fall at
        // ~3.5·cvAlpha·margin V/s; a 0.6 V step fired G2 three times). So walk the Icv cap down at a set rate, hold
        // once the filtered bus reaches target + stop margin, and hand back bumplessly after the vTgtRampDn glide
        // also arrives. Protections keep the gliding target as reference. Must be the last Icv writer.
        {
          float kWd = (float)SYSTEM_VOLTAGE_CLASS / 12.0f;
          bool fitProbeActive = (fieldCurveActive != 0) || (systemIDActive != 0) ||
                                resTestActive || batteryHealthTestActive || cvPlantFitActive ||
                                (altSweepActive != 0);
          if (!voltageControlActive || !cvWindDownEnable || fitProbeActive ||
              (CVTuningMode != 0) || g_autoTestActive) {
            cvWindDownActive = false;  // tests/probes drive targets deliberately; idle exit clears like the other trackers
          } else if (!cvWindDownActive) {
            if (!enteringCV && !fastOvClampActive
                && (ChargingVoltageTarget - ChargingVoltageTargetReq) > CV_WINDDOWN_TRIG_V * kWd) {
              cvWindDownActive = true;
              cvWindDownCap = Icv;
              cvWindDownFinalV = ChargingVoltageTargetReq;
              queueConsoleMessageF("CV wind-down: target %.2f->%.2fV, walking command down from %.1fA",
                                   ChargingVoltageTarget, ChargingVoltageTargetReq, cvWindDownCap);
            }
          } else {
            cvWindDownFinalV = fminf(cvWindDownFinalV, ChargingVoltageTargetReq);  // follow further drops
            bool busArrived = (g_ovIbvFilt <= cvWindDownFinalV + cvWindDownStopV);
            bool glideDone = (ChargingVoltageTarget <= cvWindDownFinalV + 0.01f * kWd);
            bool raised = (ChargingVoltageTargetReq > cvWindDownFinalV + 0.05f * kWd);
            if (!busArrived) {
              cvWindDownCap = fmaxf(0.0f, cvWindDownCap - cvWindDownRate * (float)MaxTableValue * actualDtSec);
            }
            if ((busArrived && glideDone) || raised || fastOvClampActive) {
              // Bumpless handback: seed so Icv = Kp·boost·e + cv_I lands exactly at the cap, then the
              // PI owns the hold. On a protection abort the unified release will reseed again from
              // preEventCvI — which tracked the capped cv_I, so the de-escalation ratchet is intact.
              float icvHi_wd = clamp_f(icvCeil, 0.0f, (float)MaxTableValue);
              float e_wd = voltageTargetSlewed - IBV;
              cv_I = clamp_f(cvWindDownCap - VoltageKp_active * cvRecovBoostMult(e_wd) * e_wd, 0.0f, icvHi_wd);
              cvWindDownActive = false;
              if (raised) queueConsoleMessage("CV wind-down: released (target raised)");
              else if (fastOvClampActive) queueConsoleMessage("CV wind-down: released (protection owns)");
              else queueConsoleMessageF("CV wind-down: complete - bus %.2fV, PI resumes at %.1fA",
                                        g_ovIbvFilt, cvWindDownCap);
            }
          }
          if (cvWindDownActive) {
            cv_I = fminf(cv_I, cvWindDownCap);  // walk the integrator with the cap: handback is near-seamless and preEventCvI snapshots the true operating point
            Icv = fminf(Icv, cvWindDownCap);
          }
        }

        // ===== CV TUNING SCORE ACCUMULATION =====
        // ISE/T scoring like the current waveform test. HIGH: overshoot weighted by cvKOvershoot, undershoot ×1.
        // LOW: one-sided squared error, normal weight (battery decay is physics-limited).
        if (CVTuningMode && voltageControlActive && cvTuningScore.waveHigh && cvTuningScore.ringInDone) {
          float highTarget = cvBaseTarget + cvWaveAmplitudeV;
          float e_high = IBV - highTarget;  // raw INA228 — no filter lag on tuning score
          float e_scored, w_high;
          if (e_high > 0.0f) {
            // dead-band (per-cell-scaled by class): the first 25mV(×class) of overshoot is free
            e_scored = fmaxf(0.0f, e_high - CV_HIGH_DEADBAND_V * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f));
            w_high = cvKOvershoot;
          } else {
            e_scored = e_high;  // approach (undershoot during rise) scored normally, no dead-band
            w_high = 1.0f;
          }
          cvTuningScore.totalIntegratedOvershootVs += e_scored * e_scored * w_high * actualDtSec;
          float peakOv = fmaxf(0.0f, e_high);
          if (peakOv > cvTuningScore.worstOvershootV) cvTuningScore.worstOvershootV = peakOv;

          if (!cvTuningScore.phaseSettled) {
            float vErr = fabsf(IBV - highTarget);  // raw INA228 — settle detection on true voltage
            if (vErr <= CV_SETTLE_V_THRESH * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f)) {
              if (++cvTuningScore.consecutiveInBand >= cvConsecutiveReads) {
                cvTuningScore.phaseSettled = true;
                cvTuningScore.totalSettlingTimeSec +=
                  (float)(currentMillis - cvTuningScore.phaseStartMs) / 1000.0f;
              }
            } else {
              cvTuningScore.consecutiveInBand = 0;
            }
          }

          // Steady-state peak-to-peak: stamp when V first enters the settle band, wait
          // CV_P2P_SKIP_MS (skip the ring), then track min/max of true V for CV_P2P_EVAL_MS.
          {
            float bandV = CV_SETTLE_V_THRESH * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f);
            if (cvTuningScore.reachedTargetMs == 0 && fabsf(IBV - highTarget) <= bandV) {
              cvTuningScore.reachedTargetMs = currentMillis;
              cvTuningScore.p2pMin = 1.0e9f;
              cvTuningScore.p2pMax = -1.0e9f;
            }
            if (cvTuningScore.reachedTargetMs != 0 && !cvTuningScore.p2pCaptured) {
              uint32_t sinceReach = currentMillis - cvTuningScore.reachedTargetMs;
              if (sinceReach >= CV_P2P_SKIP_MS && sinceReach < CV_P2P_SKIP_MS + CV_P2P_EVAL_MS) {
                if (IBV < cvTuningScore.p2pMin) cvTuningScore.p2pMin = IBV;
                if (IBV > cvTuningScore.p2pMax) cvTuningScore.p2pMax = IBV;
              } else if (sinceReach >= CV_P2P_SKIP_MS + CV_P2P_EVAL_MS) {
                if (cvTuningScore.p2pMax > cvTuningScore.p2pMin) {
                  cvTuningScore.totalSteadyP2PV += (cvTuningScore.p2pMax - cvTuningScore.p2pMin);
                  cvTuningScore.steadyP2PCount++;
                }
                cvTuningScore.p2pCaptured = true;
              }
            }
          }

          cvTuningScore.activeTimeSec += actualDtSec;
          cvTuningScore.rpmSum += RPM;
          float tempSample = isnan(AlternatorTemperatureF) ? TempToUse : AlternatorTemperatureF;
          if (!isnan(tempSample) && tempSample > -90.0f) { cvTuningScore.tempSum += tempSample; cvTuningScore.tempSampleCount++; }   // -99 = thermistor never read (§10.4)
          cvTuningScore.avgSampleCount++;
        }

        // ===== CV TUNING SCORE ACCUMULATION — LOW phase (return to normal setpoint after step-up) =====
        if (CVTuningMode && voltageControlActive && !cvTuningScore.waveHigh && cvTuningScore.ringInDone) {
          float lowTarget = cvBaseTarget;
          // Zero-crossing: voltage descending from HIGH must cross below lowTarget before re-overshoot counts
          if (!cvTuningScore.lowCrossedBelow && IBV < lowTarget) cvTuningScore.lowCrossedBelow = true;
          // Overshoot (above target): only after zero crossing — avoids penalising the natural descent from HIGH
          if (cvTuningScore.lowCrossedBelow) {
            float e_low = fmaxf(0.0f, IBV - lowTarget);
            cvTuningScore.totalLowIntOvVs += e_low * e_low * actualDtSec;
            if (e_low > cvTuningScore.worstLowOvV) cvTuningScore.worstLowOvV = e_low;
          }
          // Undershoot (below target): 1s grace from phase start, then weight ramps 0→1 over 10s, capped at 1
          float undershootErr = fmaxf(0.0f, lowTarget - IBV);
          if (undershootErr > cvTuningScore.worstLowUndershootV) cvTuningScore.worstLowUndershootV = undershootErr;
          if (undershootErr > 0.0f) {
            float tPhaseSec = (currentMillis - cvTuningScore.lastToggleMs) / 1000.0f;
            if (tPhaseSec > CV_LOW_GRACE_SEC) {
              float w_under = fminf(1.0f, (tPhaseSec - CV_LOW_GRACE_SEC) / CV_LOW_RAMP_SEC);
              cvTuningScore.totalLowUndershootVs += w_under * undershootErr * undershootErr * CV_UNDERSHOOT_SCALE * actualDtSec;
            }
          }
          cvTuningScore.activeTimeSec += actualDtSec;  // LOW time counts toward the shared denominator

          if (!cvTuningScore.lowPhaseSettled) {
            float vErr = fabsf(IBV - lowTarget);  // raw INA228 — settle detection on true voltage
            if (vErr <= CV_SETTLE_V_THRESH * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f)) {
              if (++cvTuningScore.lowConsecInBand >= cvConsecutiveReads) {
                cvTuningScore.lowPhaseSettled = true;
                cvTuningScore.totalLowSettlingTimeSec +=
                  (float)(currentMillis - cvTuningScore.lowPhaseStartMs) / 1000.0f;
              }
            } else {
              cvTuningScore.lowConsecInBand = 0;
            }
          }
        }

        setpointCommand = voltageControlActive ? Icv : (float)uTargetAmps;

        bool cvBrakingDown = voltageControlActive && (g_cvKdTrimLive > 0.0f);  // D-term pulling the command down on a fast rise → controlled brake-tier fall, still finite (never the OV slam)
        float effectiveFallRate = fastOvClampActive ? 1.0e9f : (cvBrakingDown ? fmaxf(CvBrakeFallRate, SetpointFallRate) : SetpointFallRate);  // fmaxf: braking must never fall slower than normal (the submit-time floor can be stale)
        // Post-protection fast-rise: while the window is open and battV has clear headroom below target, multiply the
        // rise slew so the alternator crosses its deadband quickly. Opens on the fastOvClampActive falling edge.
        bool postProtectRiseActive =
            voltageControlActive &&
            (postProtectRiseStartMs != 0) &&
            ((currentMillis - postProtectRiseStartMs) < FastSetpointRiseWindowMs) &&
            (IBV < ChargingVoltageTarget - FastSetpointRiseHeadroomV);
        float effectiveRiseRate;
        if (inStartupRamp)              effectiveRiseRate = StartupRiseRate;
        else if (postProtectRiseActive) effectiveRiseRate = SetpointRiseRate * FastSetpointRiseRate;
        // Large up-step gentling: ramp at SetpointBigStepRiseRate until within SetpointBigStepThresh, then the normal
        // rate for the final approach. Below the post-protection branch so it never slows trip recovery.
        else if ((setpointCommand - setpointLimited) > SetpointBigStepThresh)
                                        effectiveRiseRate = SetpointBigStepRiseRate;
        else                            effectiveRiseRate = SetpointRiseRate;
        if (!setpointSlewEnable && !g_autoTestActive) {
          // Limiter disabled (Test Limiters, A/B study): setpoint steps instantly, bypassing all rise shaping above.
          // Inert during an automated test so user state can't strip slew from a measurement.
          setpointLimited = setpointCommand;
        } else {
          setpointLimited = slew_limit_f(setpointLimited, setpointCommand,
                                         effectiveRiseRate, effectiveFallRate, actualDtSec);
        }
        if (inStartupRamp && setpointLimited >= setpointCommand - 0.5f) {
          inStartupRamp = false;
        }

        // Output current PID compute. MaintainMode feedback is always INA228 Bcur, never getBatteryCurrent(): a Victron
        // source adds ~1–2 s of lag that destabilizes this loop (that dropdown only governs SoC display).
        float pidSp = setpointLimited;
        bool zeroCmdCeilBinding = false;
        {
          float pidSig = (OutputPIDSigSrc == 2) ? MeasuredAmps : (OutputPIDSigSrc == 1) ? g_pidMA_N
                                                                                        : g_pidI_filtered;
          if (MaintainMode == 1 || zeroFloatActive)   targetCurrent = Bcur;   // 0-net-amps, raw INA228
          else                                        targetCurrent = pidSig;
          // Alternator ceiling as a battery setpoint (Bcur + headroom = ceiling - house load). The min makes the error
          // min(sp - Bcur, ceiling - pidSig), continuous at the hand-off: the alternator holds its ceiling, the battery covers the rest.
          if ((MaintainMode == 1 || zeroFloatActive) && !isnan(zeroCmdAltCeil)) {
            float ceilSp = Bcur + (zeroCmdAltCeil - pidSig);
            if (ceilSp < pidSp) { pidSp = ceilSp; zeroCmdCeilBinding = true; }
          }
        }
        pidInput = (double)targetCurrent;
        pidSetpoint = (double)pidSp;
        pidError = pidSp - targetCurrent;

        currentPID.Compute();

        // Banner limiter code (→ CSV4/NavStream): always the min-select winner, so the cue doesn't blink off on ripple
        // dips or mid-slew. 5 = machine limit, read from the ACTUATOR (duty riding MaxDuty, sustained, output short),
        // since output error alone can't tell "won't get there" from "not there yet". 6 = protection, held through the
        // post-protection fast-rise (battCeilBinding latches before the fastOV fold, so it would read "Batt").
        {
          bool zeroCmd = (MaintainMode == 1 || zeroFloatActive);
          bool underTracking = ((float)pidInput < setpointLimited - fmaxf(3.0f, 0.10f * setpointLimited));
          // Min-field floor binding: duty parked on the keep-alive floor while OVER-delivering. The only limiter that
          // raises output, and the PID can't report it (its outMin IS MinDuty). rpmMinDuty <= 0 = floor disabled.
          bool overTracking = ((float)pidInput > setpointLimited + fmaxf(2.0f, 0.05f * setpointLimited));
          float floorNow = fmaxf(MinDuty, tick.rpmMinDuty);
          bool atFloorNow = (tick.rpmMinDuty > 0.01f)
                            && (lastAppliedDuty <= floorNow + fmaxf(0.1f, 0.01f * (ccDutyCeiling() - floorNow)));
          bool minFloorBinding = atFloorNow && overTracking;
          bool dutyPegged = (lastAppliedDuty >= ccDutyCeiling() - 1.0f);
          static uint32_t dutyPegStartMs = 0;
          if (dutyPegged && underTracking) {
            if (dutyPegStartMs == 0) dutyPegStartMs = currentMillis;
          } else {
            dutyPegStartMs = 0;
          }
          bool fieldSaturated = (dutyPegStartMs != 0) && ((uint32_t)(currentMillis - dutyPegStartMs) >= 2000UL);
          bool protBinding = (fastOvClampActive && ((float)uTargetAmps < i_ceiling_pre_ov - 0.01f))
                             || postProtectRiseActive;
          uint8_t rawCode;
          // 11 (min-field floor) sits below every real ceiling: it RAISES output, so an active ceiling is the story (else a
          // derate reads MIN FIELD and rvcDeratingFromLimiter publishes "not derating"). 12 precedes 14 and 13 because
          // glideBinding/warmupBinding are computed before Maintain/zero-float zero uTargetAmps; once that alternator ceiling
          // sets the zero-current setpoint, 12 yields and the codes below name the ceiling.
          if (CVTuningMode || inStartupRamp)                       rawCode = 0;
          else if (protBinding)                                   rawCode = 6;
          else if (zeroCmd && !zeroCmdCeilBinding)                rawCode = 12;
          else if (glideBinding)                                  rawCode = 14;
          else if (altZeroOutput)                                 rawCode = 7;
          else if (fieldSaturated)                                rawCode = (g_fieldCeilBind >= 3) ? 16 : (g_fieldCeilBind == 2) ? 15 : 5;  // board heat / board 10 A / the alternator's own ceiling
          else if (voltageControlActive && Icv < icvCeil - 0.5f)  rawCode = dvccCvlBinding ? 9 : 3;  // held at the BMS's voltage vs our own target
          else if (dvccCclBinding)                                rawCode = 8;  // BMS charge-current limit is the active ceiling
          else if (battCeilBinding)                               rawCode = 4;
          else if (thermalPenaltyAmps > 0.5f)                     rawCode = 2;
          else if (minFloorBinding)                               rawCode = 11;  // no ceiling binds: over-delivery really is the keep-alive floor
          else if (warmupBinding)                                 rawCode = 13;  // last real ceiling before the catch-all
          else                                                    rawCode = 1;
          // Publish a change only after 5 consecutive identical ticks: edge-hover between two codes parks the published
          // value instead of hopping at whatever instant the 500 ms CSV4 sampler catches.
          static uint8_t limCand = 0, limCandTicks = 0;
          if (rawCode == ctrlLimiter)  limCandTicks = 0;
          else if (rawCode == limCand) { if (++limCandTicks >= 5) { ctrlLimiter = rawCode; limCandTicks = 0; } }
          else                         { limCand = rawCode; limCandTicks = 1; }
        }
      }

    } else {
      // ===== MANUAL mode: no setpoint management =====
      voltageControlActive = false;
      lastVoltageControlActive = false;  // keep tracker in sync so AUTO re-entry from MANUAL fires the bumpless CV seed
      cvWindDownActive = false;          // MANUAL owns the field — drop any in-flight wind-down
      clearStaleProtClamp();
      g_thermalOwnsCeiling = false;      // ceiling chain does not run here; must not read stale-true on AUTO re-entry
      uTargetAmps = 0;
      setpointLimited = 0.0f;
      ctrlLimiter = 10;                  // the user's own duty IS the limit (banner names manual)
      if (g_fieldCeilBind >= 2 && ManualDutyTarget > ccDutyCeiling() + 0.05f)
        ctrlLimiter = (g_fieldCeilBind >= 3) ? 16 : 15;   // a board ceiling is holding the manual duty down
      pidInput = (double)((OutputPIDSigSrc == 2) ? MeasuredAmps : (OutputPIDSigSrc == 1) ? g_pidMA_N
                                                                                         : g_pidI_filtered);
    }  // end of MANUAL mode else
  }  // end if (!sysIDRunning)


  uint32_t aflM4 = micros();  // end of section 4: normal-mode control body (PID/CV/sysID)
  // ========== BUILD DUTY REQUEST ==========
  float dutyRequest;
  // A test that owns duty owns it in MANUAL too: protection test mode 5 holds PWM 0 under its hard cut, so the
  // release ramps back at DutyRampRate instead of re-enabling the field with the full manual duty already applied.
  if (sysMode == SYS_MODE_MANUAL && !sysIDRunning) {
    dutyRequest = constrain(ManualDutyTarget, 0.0f, 100.0f);
  } else {
    dutyRequest = (float)pidOutput;
  }

  pidLog_dutyRequest = dutyRequest;

  // ========== APPLY THROUGH GOVERNOR ==========
  // An active protection clamp drops the tach floor: with duty pinned at the floor, flux decays only to the floor
  // asymptote and an RPM surge on that residual carried the bus +350 mV after the cut. The release reseed restores it.
  // cvpfCcActive: plant-fit settle/pilot/practice currents can sit below the learned Min% floor's output.
  // MANUAL is open loop: duty delivered as typed, including 0. Max Field is NOT bypassed (bounds field on 24/36/48V).
  float dutyNewFloat = governor_apply(lastAppliedDuty, dutyRequest, govMode,
                                      (sysIDRunning || fastOvClampActive || cvpfCcActive
                                       || sysMode == SYS_MODE_MANUAL) ? 0.0f : tick.rpmMinDuty,
                                      true, actualDtSec);
  pidLog_dutyApplied = dutyNewFloat;

  // ========== TELL PID WHAT ACTUALLY HAPPENED ==========
  if (sysMode == SYS_MODE_AUTO) {
    currentPID.TrackAppliedOutput((double)dutyNewFloat, actualDtSec);
  } else {
    currentPID.ResetIntegratorTo((double)dutyNewFloat);
  }

  // ========== UPDATE STATE ==========
  lastAppliedDuty = dutyNewFloat;
  dutyCycle = dutyNewFloat;

  // Auto Min% learning: observe the applied floor vs. output to walk rpmMinDutyTable toward
  // (knee - margin). Observer only; gated to normal AUTO charging (no fault/shutdown/manual/sysID).
  {
    // fastOvClampActive / cvpfCcActive: the governor above dropped the floor, so the duty under it says nothing about the knee
    bool kneeModeOk = (sysMode == SYS_MODE_AUTO) && !sysIDRunning && !cvPlantFitActive && tick.chargingEnabled
                      && !tick.inLockout && !IgnoreTemperature && !IgnoreRPM && (hardwarePresent == 1)
                      && !fastOvClampActive && !cvpfCcActive
                      && !tick.currentDataStale;   // cvPlantFitActive: floor-bypassed test operation must not train the knee
    kneeLearnObserve(RPM, dutyNewFloat, TempToUse, MeasuredAmps,
                     dutyRequest, tick.rpmMinDuty, kneeModeOk);
  }

  {
    bool huntModeOk = (sysMode == SYS_MODE_AUTO) && !sysIDRunning && tick.chargingEnabled
                      && !tick.inLockout && !gpio4IsLow;
    huntGovObserve(dutyNewFloat, huntModeOk);
  }

  if (sysMode == SYS_MODE_AUTO) {
    innerTermP = (float)currentPID.GetPterm();
    innerTermI = (float)currentPID.GetIterm();
    innerTermD = (float)currentPID.GetDterm();

    // ===== Control Accuracy v4 — routine-data loop health =====
    // Spec: Working Markdown Docs/CONTROL_ACCURACY_V4_ROUTINE_SPEC.md. Tick buckets CONSTRAINED > ACTIVE (graded) >
    // QUIET. Error is PV vs the PRE-SLEW command, so the user's slew/filter choices are part of what is reported.

    // Raw scorer-tick gap: a scheduler stall, or any stretch where these blocks didn't run
    // (early returns, non-AUTO modes), must not masquerade as control data.
    bool accGap = (accScorerLastMs != 0) && ((uint32_t)(tick.nowMs - accScorerLastMs) > ACC_GAP_VOID_MS);
    accScorerLastMs = tick.nowMs;
    bool accClampFell = accClampPrev && !g_fastOvClampActive;
    accClampPrev = g_fastOvClampActive;

    // ---- Current loop. Stays live in CV mode: tracking Icv IS this loop's job there.
    {
      bool accValid = !inStartupRamp && (MaintainMode == 0) && !zeroFloatActive
                      && setpointLimited > 2.0f;  // Maintain/zeroFloat switch the PV to Bcur — a different job
      if (!accValid || accGap) {
        excCur = {};
      } else {
        float band = fmaxf(ACC_CUR_BAND_FLOOR_A, ACC_CUR_BAND_FRAC * (float)AlternatorNominalAmps);
        float e = targetCurrent - setpointCommand;  // A; + = over-current (damaging side)

        if (fabsf(setpointCommand - accCurPrevCmd) > ACC_CUR_ACTIVE_RATE_A_S * actualDtSec || accClampFell)
          accCurActiveUntilMs = tick.nowMs + ACC_ACTIVE_HOLD_MS;

        float dutyFloor = fmaxf(MinDuty, tick.rpmMinDuty);
        float railMargin = fmaxf(0.1f, 0.01f * (ccDutyCeiling() - dutyFloor));
        bool atFloor = dutyCycle <= dutyFloor + railMargin;
        bool atCeil = dutyCycle >= ccDutyCeiling() - railMargin;
        bool constrained = atFloor || atCeil || g_fastOvClampActive;
        bool active = accCurActiveUntilMs != 0 && (int32_t)(accCurActiveUntilMs - tick.nowMs) > 0;

        accCur4.validSec += (double)actualDtSec;
        if (e > band) accCur4.overExpSum += (double)(e - band) * (double)actualDtSec;
        if (e > accCur4.worstOver) accCur4.worstOver = e;
        if (constrained) {
          accCur4.constrainedSec += (double)actualDtSec;
        } else if (active) {
          accCur4.activeSec += (double)actualDtSec;
          if (fabsf(e) <= band) accCur4.inbandActiveSec += (double)actualDtSec;
        }

        // Excursion stopwatch. Out-of-band time PAUSES while the loop is constrained in the
        // direction the error needs (over needs down-authority: duty floor / protection clamp;
        // under needs up-authority: duty ceiling) — rail physics is not tracking failure.
        if (excCur.state == 0) {
          int8_t s = (e > band) ? 1 : (e < -band) ? -1 : 0;
          bool blocked = (s > 0 && (atFloor || g_fastOvClampActive)) || (s < 0 && atCeil);
          if (s != 0 && !blocked) {
            excCur.enterTicks = (s == excCur.side) ? (uint8_t)(excCur.enterTicks + 1) : 1;
            excCur.side = s;
            if (excCur.enterTicks >= ACC_ENTER_DEBOUNCE_TICKS) {
              excCur.state = 1;
              excCur.outSec = 0.0;
              excCur.reentryStartMs = 0;
              accCur4.excursions++;
            }
          } else {
            excCur.enterTicks = 0;
          }
        } else {
          bool paused = (excCur.side > 0) ? (atFloor || g_fastOvClampActive) : atCeil;
          bool inside = (excCur.side > 0) ? (e <= band) : (e >= -band);
          bool flipped = (excCur.side > 0) ? (e < -band) : (e > band);
          if (!paused && !inside) excCur.outSec += (double)actualDtSec;
          if (flipped) {
            // Swung through the band to the far side: close this excursion, open the opposite one.
            accCur4.recovSecSum += excCur.outSec;
            int8_t ns = (int8_t)-excCur.side;
            excCur = {};
            excCur.state = 1;
            excCur.side = ns;
            accCur4.excursions++;
          } else if (inside) {
            if (excCur.reentryStartMs == 0) excCur.reentryStartMs = tick.nowMs;
            else if ((uint32_t)(tick.nowMs - excCur.reentryStartMs) >= ACC_CUR_EXIT_HOLD_MS) {
              accCur4.recovSecSum += excCur.outSec;
              excCur = {};
            }
          } else {
            excCur.reentryStartMs = 0;
          }
        }
      }
      accCurPrevCmd = setpointCommand;
    }

    // ---- Voltage loop. Its one-sided authority is handled by the constrained-direction rules (CV-entry climb pins
    // Icv at the ceiling, step-down descent at zero). Measured in 12V-EQUIVALENT volts so published mV figures
    // (CSV2, /cvtuninglog, cloud acc_volt_*) compare across 12/24/36/48V systems.
    {
      float vNorm = 12.0f / (float)SYSTEM_VOLTAGE_CLASS;
      // Per-tick delta so temp-comp drift (mV-scale per tick) never reads as a step; track the
      // target every tick (valid or not) so CV re-entry can't fire a phantom step.
      bool targetStep = fabsf(ChargingVoltageTarget - accVPrevTargetV) * vNorm > ACC_V_STEP_V;
      accVPrevTargetV = ChargingVoltageTarget;
      bool awRecovFell = accVAwRecovPrev && !g_cvAwRecovering;
      accVAwRecovPrev = g_cvAwRecovering;
      // zeroFloatActive: the cascade regulates net battery amps to ~0 and lets IBV drift to
      // resting voltage — deviations from the float target are by design.
      bool accVValid = voltageControlActive && !zeroFloatActive;
      static bool accVWasValid = false;
      bool cvEntered = accVValid && !accVWasValid;
      accVWasValid = accVValid;

      if (!accVValid || accGap) {
        excVolt = {};
      } else {
        float err12 = (IBV - ChargingVoltageTarget) * vNorm;  // 12V-equiv V; + = over (damaging)
        if (targetStep || awRecovFell || cvEntered || accClampFell)
          accVoltActiveUntilMs = tick.nowMs + ACC_ACTIVE_HOLD_MS;

        bool ceilPinned = Icv >= (uTargetAmps - 0.5f);  // no up-headroom: current-limited
        bool zeroPinned = Icv <= 0.5f;                  // downward authority fully spent
        bool constrained = ceilPinned || zeroPinned || g_fastOvClampActive || g_cvAwRecovering;
        bool active = accVoltActiveUntilMs != 0 && (int32_t)(accVoltActiveUntilMs - tick.nowMs) > 0;

        accVolt4.validSec += (double)actualDtSec;
        if (err12 > ACC_V_BAND_V) accVolt4.overExpSum += (double)(err12 - ACC_V_BAND_V) * (double)actualDtSec;
        if (err12 > accVolt4.worstOver) accVolt4.worstOver = err12;
        if (constrained) {
          accVolt4.constrainedSec += (double)actualDtSec;
        } else if (active) {
          accVolt4.activeSec += (double)actualDtSec;
          if (fabsf(err12) <= ACC_V_BAND_V) accVolt4.inbandActiveSec += (double)actualDtSec;
        }

        // Excursion stopwatch — over needs down-authority (zero pin, protection clamp, and the
        // deliberate anti-windup climb-back are all down-side states); under needs up-authority
        // (ceiling; aw-recovery holds current low on purpose, so under pauses there too).
        if (excVolt.state == 0) {
          int8_t s = (err12 > ACC_V_BAND_V) ? 1 : (err12 < -ACC_V_BAND_V) ? -1 : 0;
          bool blocked = (s > 0 && (zeroPinned || g_fastOvClampActive || g_cvAwRecovering))
                         || (s < 0 && (ceilPinned || g_cvAwRecovering));
          if (s != 0 && !blocked) {
            excVolt.enterTicks = (s == excVolt.side) ? (uint8_t)(excVolt.enterTicks + 1) : 1;
            excVolt.side = s;
            if (excVolt.enterTicks >= ACC_ENTER_DEBOUNCE_TICKS) {
              excVolt.state = 1;
              excVolt.outSec = 0.0;
              excVolt.reentryStartMs = 0;
              accVolt4.excursions++;
            }
          } else {
            excVolt.enterTicks = 0;
          }
        } else {
          bool paused = (excVolt.side > 0) ? (zeroPinned || g_fastOvClampActive || g_cvAwRecovering)
                                           : (ceilPinned || g_cvAwRecovering);
          bool inside = (excVolt.side > 0) ? (err12 <= ACC_V_BAND_V) : (err12 >= -ACC_V_BAND_V);
          bool flipped = (excVolt.side > 0) ? (err12 < -ACC_V_BAND_V) : (err12 > ACC_V_BAND_V);
          if (!paused && !inside) excVolt.outSec += (double)actualDtSec;
          if (flipped) {
            // Swung through the band to the far side: close this excursion, open the opposite one.
            accVolt4.recovSecSum += excVolt.outSec;
            int8_t ns = (int8_t)-excVolt.side;
            excVolt = {};
            excVolt.state = 1;
            excVolt.side = ns;
            accVolt4.excursions++;
          } else if (inside) {
            if (excVolt.reentryStartMs == 0) excVolt.reentryStartMs = tick.nowMs;
            else if ((uint32_t)(tick.nowMs - excVolt.reentryStartMs) >= ACC_V_EXIT_HOLD_MS) {
              accVolt4.recovSecSum += excVolt.outSec;
              excVolt = {};
            }
          } else {
            excVolt.reentryStartMs = 0;
          }
        }
      }
    }
  } else {
    innerTermP = innerTermI = innerTermD = 0.0f;
  }

  updateFieldTelemetry(dutyCycle, tick.currentBatteryVoltage);

  fieldActiveStatus = (!gpio4IsLow && lastAppliedDuty > 0.01f)
                        ? ((sysMode == SYS_MODE_MANUAL) ? 3 : 1)
                        : 0;
  chargeStageDisplay = getChargeStageDisplayCode();

  uTargetRaw_cached = uTargetRaw;  // update pre-OV ceiling for next tick's fastOvBaseCap

  uint32_t aflM5 = micros();  // end of section 5: duty build + governor + state/telemetry
  // Normal-mode path only — shutdown/fault paths do not log here.
  pidLog_tick(currentMillis);
  // Reached only on a normal field-on tick; loop() clears pfHasPrev while the field is off so the first firing after
  // re-baselines instead of logging the off-gap.
  pidFire_record(currentMillis);
  // Best-Ever Front: fold one alternator-health sample per control tick (~75 Hz). Live only —
  // bench-sim folds at 10 Hz from altHealth_tick instead. Off/fault/shutdown early-return above this.
  if (altSimMode < 0.5f) TIMED_CALL(ft_altFold, altFold_tick(currentMillis));
  // Section profiler latch — full passes only. A whole-core preemption is charged to whichever section it landed in.
  // Surfaces in /debug, resets with Reset Peak Values.
  {
    uint32_t aflEnd = micros();
    uint32_t aflTot = aflEnd - aflT0;
    if (aflTot > aflWorstTotalUs) {
      aflWorstTotalUs = aflTot;
      aflWorstSecUs[0] = aflM0 - aflT0;   // thermal log
      aflWorstSecUs[1] = aflM1 - aflM0;   // RPM tables + tick snapshot
      aflWorstSecUs[2] = aflM2 - aflM1;   // temp warning + fast-OV supervisor + limp gate
      aflWorstSecUs[3] = aflM3 - aflM2;   // CH1 gate + stage/governor/mode transitions
      aflWorstSecUs[4] = aflM4 - aflM3;   // normal-mode control body (PID/CV/sysID)
      aflWorstSecUs[5] = aflM5 - aflM4;   // duty build + governor + state/telemetry
      aflWorstSecUs[6] = aflEnd - aflM5;  // pidLog + PID-interval sample + alt-health fold
    }
  }
  prevMode = mode;
}

void setDutyPercent(float percent) {
  static uint32_t lastFrequency = 0;
  static bool pwmInitialized = false;

  // Single final write to the field PWM for every duty path: hard-stop NaN here (constrain(NaN) returns NaN).
  // Hard cap 99%: at 100% LEDC is solid DC, so the high-side bootstrap never refreshes on P-type wiring (C4 drains
  // through R108 in ~0.6ms → UVLO chop). 99% leaves a refresh slice every period, verified at the 19455Hz ceiling.
  if (isnan(percent)) percent = 0.0f;
  percent = constrain(percent, 0.0f, 99.0f);

  // LEDC 12-bit ceiling: the clock divider must exceed 1.0, so 19455Hz is the max the driver accepts (19500 is
  // rejected). Clamp here since NVS can carry an out-of-range value.
  SwitchingFrequency = constrain(SwitchingFrequency, 100.0f, 19455.0f);

  // Field-duty safety net: every duty path lands here, including open-loop manual/limp/fault that bypass the PID
  // limits. Max Field Volts protects a 12V winding on a higher-voltage bus. Duty-ratio proxy, not a measured amp limit.
  // The field resistance test replaces the two board ceilings with its own hold duty; the alternator ceilings stay.
  const float ceil = (frt.state == FRT_RUNNING) ? fminf(frtCeilPct(), frt.duty) : ccDutyCeiling();
  if (percent > ceil) {
    percent = ceil;
  }

  uint32_t duty = (uint32_t)((((1UL << pwmResolution) - 1) * percent) / 100.0f);

  if (!pwmInitialized) {
    Serial.printf("Initializing PWM on pin %d...\n", (int)pwmPin);
    pwmInitialized = ledcAttach(pwmPin, (uint32_t)SwitchingFrequency, pwmResolution);
    if (!pwmInitialized) {
      queueConsoleMessage("ERROR: PWM attach failed");
      return;
    }
    lastFrequency = (uint32_t)SwitchingFrequency;
    Serial.println("PWM initialized successfully");
  }

  if ((uint32_t)SwitchingFrequency != lastFrequency) {
    uint32_t actualFreq = ledcChangeFrequency(pwmPin, (uint32_t)SwitchingFrequency, pwmResolution);
    lastFrequency = (uint32_t)SwitchingFrequency;
    delayMicroseconds(100);
    queueConsoleMessageF("PWM frequency applied to hardware: %luHz", (unsigned long)actualFreq);
  }

  ledcWrite(pwmPin, duty);
}


// Bcur is meaningless without a battery shunt (reads ~0) — stage logs print "n/a" instead of a
// fabricated number. Single control-task caller, so the shared buffer is safe.
static const char *bcurForLog() {
  static char b[16];
  if (HAS_BATT_SHUNT) snprintf(b, sizeof(b), "%.1fA", Bcur);
  else if (BattMonSrc != 0 && !isnan(netMonCurrentA())) snprintf(b, sizeof(b), "%.1fA %s", netMonCurrentA(), BattMonSrc == 3 ? "VE" : "N2K");
  else strncpy(b, "n/a", sizeof(b));
  return b;
}

void updateChargingStage() {
  const uint32_t now = millis();
  // Stage thresholds only. Control loops, protections and every dynamic measurement stay on getFiltV().
  const float v = getChargeDecisionVoltage();
  const char *vsrc = chargeDecisionVoltageSourceName();
  const int soc = socShownX100() / 100;  // %×100; the rebulk gates compare in plain percent
  // NAN when not fresh, so a stale reading can't end absorption or trigger a current rebulk (§10.6). Without a shunt
  // this is the network monitor or nothing; Bcur is never read (a -1 mV offset reads -10 A and rebulks).
  bool stageINet = false;
  const float stageI = stageBattCurrentA(stageINet);
  const bool bcurRebulk = !isnan(stageI) && RebulkCurrent_A > 0.0f && stageI < -RebulkCurrent_A;

  // While a commissioning run is live (IN_PROGRESS + fresh wizard heartbeat) the wizard owns the field. IDLE would clear
  // chargingEnabled, drop sysMode out of AUTO and freeze the wizard (Restart can't recover it); staying out of IDLE
  // also keeps G1–G4 armed. Mirrors getChargeStageDisplayCode().
  const bool commissioningOwnsBattery = (commissionState == 1) && (lastCommissionHeartbeatMs != 0) &&
      ((uint32_t)(now - lastCommissionHeartbeatMs) < COMMISSION_HEARTBEAT_TIMEOUT_MS);
  // Pack already resting in IDLE when the wizard went live: lift it back to a live charging stage now so
  // the first step has AUTO available immediately, instead of waiting on a slow voltage-sag rebulk.
  if (commissioningOwnsBattery && inIdleStage) {
    inIdleStage = false;
    inBulkStage = true;
    inAbsorptionStage = false;
    bulkVoltageHoldTimer = 0;
    rebulkTimer = 0;
  }

  // User "Restart charge cycle": force Bulk and re-run the whole cycle (same reset as enter_sys_auto()). Consumed here so
  // stage flags are written from one task only. Honored in an auto charge stage or IDLE; dropped in Manual/Target-V/
  // Maintain (flags don't drive output) and commissioning. It does NOT fix a broken tail exit (steady load / no shunt).
  if (restartChargeCycleRequested) {
    restartChargeCycleRequested = false;
    if ((fieldActiveStatus == 1 || inIdleStage) && TargetVoltageMode == 0 && MaintainMode == 0 && !commissioningOwnsBattery) {
      inBulkStage = true;
      inAbsorptionStage = false;
      inIdleStage = false;
      bulkVoltageHoldTimer = 0;
      absorptionTailTimer = 0;
      rebulkTimer = 0;
      floatStartTime = now;
      queueConsoleMessageF("Stage: RESTART→BULK (user request) | battV=%.2fV src=%s bulkTarget=%.2fV", v, vsrc, BulkVoltage);
    } else {
      queueConsoleMessage("Restart charge cycle ignored: no auto charge stage active");
    }
  }

  // Two-sided hysteresis so 30–50 mV idle noise (12V figure, scaled ×V/12) can't reset the bulk hold timer.
  const float BULK_V_BAND_ENTER = 0.05f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f);
  const float BULK_V_BAND_EXIT  = 0.10f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f);

  if (inBulkStage && !inAbsorptionStage) {
    // ===== BULK (CC) =====
    ChargingVoltageTargetReq = BulkVoltage;

    if (bulkVoltageHoldTimer == 0) {
      if (v >= (BulkVoltage - BULK_V_BAND_ENTER)) bulkVoltageHoldTimer = now;
    } else if (v < (BulkVoltage - BULK_V_BAND_EXIT)) {
      bulkVoltageHoldTimer = 0;
    } else if ((uint32_t)(now - bulkVoltageHoldTimer) >= bulkVoltageHoldMs) {
      inAbsorptionStage = true;
      ChargingVoltageTargetReq = AbsorptionVoltage;
      absorptionStartTime = now;
      absorptionTailTimer = 0;
      bulkVoltageHoldTimer = 0;
      queueConsoleMessageF(
        "Stage: BULK→ABSORPTION (hold timer) | battV=%.2fV src=%s bulkTarget=%.2fV absTarget=%.2fV tailCurrent=%.1fA timeout=%.0fmin",
        v, vsrc, BulkVoltage, AbsorptionVoltage, TailCurrent_A,
        (float)AbsorptionTimeoutMs / 60000.0f);
    }

  } else if (inBulkStage && inAbsorptionStage) {
    // ===== ABSORPTION (CV) =====
    ChargingVoltageTargetReq = AbsorptionVoltage;

    const bool thermallyConstrained = (thermalPenaltyAmps > 2.0f) && (uTargetAmps <= TailCurrent_A * 2.0f);  //if you ever want CV-awareness here you'd change it to Icv <= TailCurrent_A * 2.0f.
    // Tail current alone is ambiguous: a house load or non-thermal limiter drives Bcur to ~0 on a half-charged bank, so
    // require the bus AT the absorption target. One-sided: another source holding it ABOVE target is a legitimate tail.
    const float ABS_V_TAIL_BAND = 0.15f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f);
    const bool atAbsorbV = (v >= (AbsorptionVoltage - ABS_V_TAIL_BAND));
    // No current source (no shunt and no network monitor) → stageI is NAN and absorption ends on AbsorptionTimeoutMs.
    // A network monitor is read by magnitude: at absorption voltage the bank only accepts current, so one with the
    // opposite sign convention would otherwise read negative and end absorption on its first sample.
    const bool tailReached = !isnan(stageI) && !thermallyConstrained && atAbsorbV
                             && ((stageINet ? fabsf(stageI) : stageI) <= TailCurrent_A);

    // AbsorptionTimeoutMs counts time AT the absorption voltage, not wall clock, so a second source or idle-RPM sag can't
    // burn the budget and dump a half-charged bank into Float. absorbWallCeiling bounds the pause (except dvccCvlHolding).
    // Epoch-keyed to absorptionStartTime so every reset path re-arms it without knowing it exists.
    static bool absorbClockArmed = false;
    static uint32_t absorbClockEpoch = 0, absorbClockPrevMs = 0, absorbHeldMs = 0, absorbWallMs = 0;
    if (!absorbClockArmed || absorbClockEpoch != absorptionStartTime) {
      absorbClockArmed = true;
      absorbClockEpoch = absorptionStartTime;
      absorbHeldMs = 0;
      absorbWallMs = 0;
      absorbClockPrevMs = now;
    }
    // A followed DVCC CVL below our absorption target means the battery is deliberately holding the bus down, so the wall
    // ceiling pauses with it (DVCC_FOLLOW_SPEC "stage exits freeze naturally"); else a long clamp force-exits into IDLE
    // with the field off. Same predicate as the clamp, so a bank merely hovering below target stays bounded.
    const bool dvccCvlHolding = (dvccEn == 1) && (dvccState == 3 /*FOLLOWING*/) && !cxOwnsBatteryNow()
                                && !isnan(dvccCvlV) && (dvccCvlV < AbsorptionVoltage);
    if (atAbsorbV) absorbHeldMs += (uint32_t)(now - absorbClockPrevMs);
    if (!dvccCvlHolding) absorbWallMs += (uint32_t)(now - absorbClockPrevMs);
    absorbClockPrevMs = now;
    const uint32_t absorbWallCeiling = (AbsorptionTimeoutMs > (UINT32_MAX / 2)) ? UINT32_MAX
                                                                                : (AbsorptionTimeoutMs * 2);
    const bool timedOut = (absorbHeldMs >= AbsorptionTimeoutMs) || (absorbWallMs >= absorbWallCeiling);

    const bool tailSuppressed = HAS_BATT_SHUNT && (thermallyConstrained || !atAbsorbV);
    static bool lastTailSuppressed = false;
    static uint32_t lastTailConstraintLogMs = 0;
    // Throttle to once per 10s — thermal constraint can flap rapidly under oscillating penalty
    if (tailSuppressed != lastTailSuppressed && (uint32_t)(now - lastTailConstraintLogMs) >= 10000) {
      if (tailSuppressed) {
        queueConsoleMessageF(
          "Absorption: tail detection suppressed (%s) | battV=%.2fV absTarget=%.2fV penalty=%.1fA uTarget=%.1fA tailThresh=%.1fA",
          thermallyConstrained ? "thermal" : "below absorption voltage",
          v, AbsorptionVoltage, thermalPenaltyAmps, uTargetAmps, TailCurrent_A);
      } else {
        queueConsoleMessageF(
          "Absorption: tail detection resumed | battV=%.2fV absTarget=%.2fV penalty=%.1fA uTarget=%.1fA tailThresh=%.1fA",
          v, AbsorptionVoltage, thermalPenaltyAmps, uTargetAmps, TailCurrent_A);
      }
      lastTailConstraintLogMs = now;
    }
    lastTailSuppressed = tailSuppressed;

    static uint32_t lastAbsorbDebugMs = 0;
    if ((uint32_t)(now - lastAbsorbDebugMs) >= 60000) {
      lastAbsorbDebugMs = now;
      queueConsoleMessageF(
        "Absorption status | battV=%.2fV absTarget=%.2fV Bcur=%s tailThresh=%.1fA | atVoltage=%.0fmin of %.0fmin%s",
        v, AbsorptionVoltage, bcurForLog(), TailCurrent_A,
        (float)absorbHeldMs / 60000.0f, (float)AbsorptionTimeoutMs / 60000.0f,
        atAbsorbV ? "" : (dvccCvlHolding ? " (paused - the battery's charge voltage limit (CVL) is holding the bus below target)"
                                         : " (paused - below target voltage)"));
    }

    if (tailReached) {
      if (absorptionTailTimer == 0) {
        absorptionTailTimer = now;
      } else if ((uint32_t)(now - absorptionTailTimer) >= absorptionCompleteTime) {
        inBulkStage = false;
        inAbsorptionStage = false;
        inIdleStage = (UseFloat == 0) && !commissioningOwnsBattery;
        floatStartTime = now;
        absorptionTailTimer = 0;
        rebulkTimer = 0;
        const char *nextStage = (EFFECTIVE_USE_FLOAT == 0) ? "IDLE" : (EFFECTIVE_USE_FLOAT == 2) ? "FLOAT (zero-current)" : "FLOAT";
        queueConsoleMessageF(
          "Stage: ABSORPTION→%s (tail current) | battV=%.2fV src=%s Bcur=%s tailThresh=%.1fA",
          nextStage, v, vsrc, bcurForLog(), TailCurrent_A);
      }
    } else {
      absorptionTailTimer = 0;
    }

    if (timedOut && inAbsorptionStage) {
      inBulkStage = false;
      inAbsorptionStage = false;
      inIdleStage = (UseFloat == 0) && !commissioningOwnsBattery;  // same guard as the tail exit: IDLE mid-wizard bounces sysMode OFF for a pass
      floatStartTime = now;
      absorptionTailTimer = 0;
      rebulkTimer = 0;
      const char *nextStage = (EFFECTIVE_USE_FLOAT == 0) ? "IDLE" : (EFFECTIVE_USE_FLOAT == 2) ? "FLOAT (zero-current)" : "FLOAT";
      queueConsoleMessageF(
        "Stage: ABSORPTION→%s (%s) | atVoltage=%.0fmin of %.0fmin, elapsed=%.0fmin | battV=%.2fV src=%s Bcur=%s",
        nextStage,
        (absorbHeldMs >= AbsorptionTimeoutMs) ? "absorption time used up" : "absorption ran too long below target voltage",
        (float)absorbHeldMs / 60000.0f, (float)AbsorptionTimeoutMs / 60000.0f,
        (float)(uint32_t)(now - absorptionStartTime) / 60000.0f, v, vsrc, bcurForLog());
    }

  } else if (inIdleStage) {
    // ===== IDLE (UseFloat=0, post-absorption rest) =====
    const uint32_t tIdle = (uint32_t)(now - floatStartTime);

    static uint32_t lastIdleDebugMs = 0;
    if ((uint32_t)(now - lastIdleDebugMs) >= 60000) {
      lastIdleDebugMs = now;
      queueConsoleMessageF("Idle status | battV=%.2fV Bcur=%s tIdle=%lus rebulkV=%.2fV rebulkI=%.1fA",
                           v, bcurForLog(), (unsigned long)(tIdle / 1000), RebulkVoltage, RebulkCurrent_A);
    }

    const bool rebulkCondition = (v < RebulkVoltage) || bcurRebulk;

    bool allowRebulk = true;
    // A held SoC (stale shunt, quiet network monitor) frozen at 100 % would block every rebulk, voltage sag included.
    if (socInfoAvailable && socDecisionOk()) {
      if (soc >= SOC_BlockRebulk_percent) allowRebulk = false;
      if (soc <= SOC_AllowRebulk_percent) allowRebulk = true;
    }

    if (tIdle >= MinFloatTime) {
      if (allowRebulk && rebulkCondition) {
        if (rebulkTimer == 0) rebulkTimer = now;
        else if ((uint32_t)(now - rebulkTimer) >= rebulkDebounceTime) {
          inBulkStage = true;
          inAbsorptionStage = false;
          inIdleStage = false;
          bulkVoltageHoldTimer = 0;
          absorptionTailTimer = 0;
          rebulkTimer = 0;
          floatStartTime = now;
          const char *why = bcurRebulk ? "discharge current threshold"
                                                      : "voltage sag confirmed";
          queueConsoleMessageF(
            "Stage: IDLE→BULK (%s) | battV=%.2fV src=%s Bcur=%s tIdle=%lus",
            why, v, vsrc, bcurForLog(), (unsigned long)(tIdle / 1000));
        }
      } else {
        rebulkTimer = 0;
      }
    } else {
      rebulkTimer = 0;
    }

  } else {
    // ===== FLOAT =====
    // Float not wanted (UseFloat disabled at runtime, or all stage flags false, e.g. override exit): go IDLE rather than
    // charge a full battery — except mid-commissioning, where IDLE would drop the wizard out of AUTO.
    if (UseFloat == 0 && !commissioningOwnsBattery) {
      inIdleStage = true;
      floatStartTime = now;
      rebulkTimer = 0;
      queueConsoleMessage("Stage: FLOAT→IDLE (UseFloat disabled)");
      return;
    }

    // Zero-current float (UseFloat=2): the PI runs at BulkVoltage purely as an OV guard (like MaintainMode);
    // uTargetAmps=0 caps the setpoint.
    ChargingVoltageTargetReq = (EFFECTIVE_USE_FLOAT == 2) ? BulkVoltage : FloatVoltage;

    const uint32_t tFloat = (uint32_t)(now - floatStartTime);
    // Zero-current float has no duration expiry (like idle): the battery isn't discharging, so a
    // periodic forced rebulk buys nothing. Only voltage-float rotates back to bulk on the timer.
    const bool floatTimedOut = (EFFECTIVE_USE_FLOAT != 2) && (tFloat >= (uint32_t)(FLOAT_DURATION * 1000UL));

    static uint32_t lastFloatDebugMs = 0;
    if ((uint32_t)(now - lastFloatDebugMs) >= 60000) {
      lastFloatDebugMs = now;
      float vErr = FloatVoltage - v;
      if (EFFECTIVE_USE_FLOAT == 2) {
        queueConsoleMessageF("Float status (zero-current) | battV=%.2fV Bcur=%s tFloat=%lus rebulkV=%.2fV minFloatTime=%lus",
                             v, bcurForLog(),
                             (unsigned long)(tFloat / 1000),
                             RebulkVoltage,
                             (unsigned long)(MinFloatTime / 1000));
      } else {
        queueConsoleMessageF("Float status | battV=%.2fV floatTarget=%.2fV vErr=%.3fV Bcur=%s tFloat=%lus rebulkV=%.2fV minFloatTime=%lus",
                             v, FloatVoltage, vErr, bcurForLog(),
                             (unsigned long)(tFloat / 1000),
                             RebulkVoltage,
                             (unsigned long)(MinFloatTime / 1000));
      }
    }

    const bool rebulkCondition = (v < RebulkVoltage) || bcurRebulk;

    bool sagConfirmed = false;

    bool allowRebulk = true;
    // A held SoC (stale shunt, quiet network monitor) frozen at 100 % would block every rebulk, voltage sag included.
    if (socInfoAvailable && socDecisionOk()) {
      if (soc >= SOC_BlockRebulk_percent) allowRebulk = false;
      if (soc <= SOC_AllowRebulk_percent) allowRebulk = true;
    }

    if (tFloat >= MinFloatTime) {
      if (allowRebulk && rebulkCondition) {
        if (rebulkTimer == 0) rebulkTimer = now;
        else if ((uint32_t)(now - rebulkTimer) >= rebulkDebounceTime) {
          sagConfirmed = true;
        }
      } else {
        rebulkTimer = 0;  // also resets timer if SoC starts blocking mid-debounce- design decision
      }
    }

    if (allowRebulk && (sagConfirmed || floatTimedOut)) {
      inBulkStage = true;
      inAbsorptionStage = false;
      inIdleStage = false;
      bulkVoltageHoldTimer = 0;
      absorptionTailTimer = 0;
      rebulkTimer = 0;
      floatStartTime = now;
      const char *why = floatTimedOut               ? "float duration expired"
                        : bcurRebulk ? "discharge current threshold"
                                                    : "voltage sag confirmed";
      queueConsoleMessageF(
        "Stage: FLOAT→BULK (%s) | battV=%.2fV src=%s rebulkV=%.2fV tFloat=%lus",
        why, v, vsrc, RebulkVoltage, (unsigned long)(tFloat / 1000));
    }
  }
}

uint8_t getChargeStageDisplayCode() {
  // Commissioning outranks every stage whenever the wizard dialog is OPEN (fresh heartbeat), regardless of field state.
  // Deliberately NOT keyed off commissionState: a paused-but-closed commission is not "commissioning".
  if (lastCommissionHeartbeatMs != 0 &&
      (uint32_t)(millis() - lastCommissionHeartbeatMs) < COMMISSION_HEARTBEAT_TIMEOUT_MS) {
    return CHARGE_STAGE_COMMISSION;
  }

  // Manual mode gets its own display regardless of bulk/absorption flags
  if (fieldActiveStatus == 3) {
    return CHARGE_STAGE_MANUAL;
  }

  // Only auto-active field should show any stage — except IDLE, whose whole job is holding the field off
  // while charging is otherwise on (the stage machine's own field-off, not a key/switch/fault one).
  if (fieldActiveStatus != 1) {
    return (idleHoldActive && TargetVoltageMode == 0 && MaintainMode == 0) ? CHARGE_STAGE_IDLE : CHARGE_STAGE_NONE;
  }

  // Override modes outrank stages; TARGET_V first as the more specific state if both are set.
  if (TargetVoltageMode == 1) {
    return CHARGE_STAGE_TARGET_V;
  }
  if (MaintainMode == 1) {
    return CHARGE_STAGE_MAINTAIN;
  }
  if (inIdleStage) {
    return CHARGE_STAGE_IDLE;
  }

  // Auto-active charging stages
  if (inBulkStage && !inAbsorptionStage) {
    return CHARGE_STAGE_BULK;
  }

  if (inBulkStage && inAbsorptionStage) {
    return CHARGE_STAGE_ABSORPTION;
  }

  return CHARGE_STAGE_FLOAT;
}


bool isVoltageDisagreementWarning(uint32_t nowMs, float batteryV, float ibv,
                                  bool voltagePlausible, bool voltageDisagreementCritical) {
  static bool voltageDisagreementActive = false;

  if (!voltagePlausible || voltageDisagreementCritical) {
    voltageDisagreementStart = 0;
    voltageDisagreementActive = false;
    return false;
  }

  // VoltageDisagreeThreshold is already class-scaled at storage (like OvMeasMarginV) — compare RAW. Do not ×V/12
  // here: double-scaling desensitizes this charging-disabling check on 24/36/48V.
  if (fabsf(batteryV - ibv) > VoltageDisagreeThreshold) {
    if (!voltageDisagreementActive) {
      voltageDisagreementStart = nowMs;
      voltageDisagreementActive = true;
    } else if (nowMs - voltageDisagreementStart > VoltageDisagreeTimeout) {
      return true;
    }
  } else {
    voltageDisagreementStart = 0;
    voltageDisagreementActive = false;
  }

  return false;
}
/**
 * Keyed on SYSTEM_VOLTAGE_CLASS, not a BulkVoltage band (banding mis-bucketed near class edges).
 * Critical = one sensor has failed; voltage can't be trusted for field control.
 */
bool isVoltageDisagreementCritical() {
  float criticalThreshold = 1.0f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f);

  if (isnan(BatteryV) || isnan(IBV)) return true;
  if (BatteryV < 0.1f || IBV < 0.1f) return true;

  float difference = fabsf(BatteryV - IBV);

  static uint32_t critDisagreeStartMs = 0;
  static bool critDisagreeActive = false;

  if (difference > criticalThreshold) {
    if (!critDisagreeActive) {
      critDisagreeStartMs = millis();
      critDisagreeActive = true;
    } else if ((uint32_t)(millis() - critDisagreeStartMs) > VoltageDisagreeCriticalTimeoutMs) {
      return true;
    }
  } else {
    critDisagreeStartMs = 0;
    critDisagreeActive = false;
  }

  return false;
}
bool isTempSustainedWarning(uint32_t nowMs, float tempToUseF, float tempLimitF,
                            float tempWarnExcessF, bool ignoreTemperature) {
  if (ignoreTemperature) {
    tempWarningStartMs = 0;
    return false;
  }

  if (tempToUseF > (tempLimitF + tempWarnExcessF)) {
    if (tempWarningStartMs == 0) {
      tempWarningStartMs = nowMs;
    } else if (nowMs - tempWarningStartMs > TempSustainedTimeout) {
      return true;
    }
  } else {
    tempWarningStartMs = 0;
  }

  return false;
}

const char *modeToString(FieldControlMode mode) {
  switch (mode) {
    case MODE_CRITICAL_RAMP: return "CRITICAL_RAMP";
    case MODE_WARNING_RAMP_AND_LOCKOUT: return "WARNING_RAMP_LOCKOUT";
    case MODE_LOCKOUT_RAMP: return "LOCKOUT_RAMP";
    case MODE_DISABLED_RAMP: return "DISABLED_RAMP";
    case MODE_NORMAL_MANUAL: return "NORMAL_MANUAL";
    case MODE_NORMAL_AUTO: return "NORMAL_AUTO";
    case MODE_COMMISSION_IDLE: return "COMMISSION_IDLE";
    case MODE_FIELD_RES_TEST: return "FIELD_RES_TEST";
    default: return "UNKNOWN";
  }
}

const char *reasonToString(FieldEventReason r) {
  switch (r) {
    case REASON_NONE: return "NONE";
    case REASON_TEMP_STALE: return "TEMP_STALE";
    case REASON_TEMP_CRITICAL: return "TEMP_CRITICAL";
    case REASON_TEMP_WARNING: return "TEMP_WARNING";
    case REASON_TEMP_SUSTAINED: return "TEMP_SUSTAINED";
    case REASON_VOLTAGE_IMPLAUSIBLE: return "VOLT_IMPLAUSIBLE";
    case REASON_VOLTAGE_DISAGREE_CRITICAL: return "VOLT_DISAGREE_CRIT";
    case REASON_VOLTAGE_SPIKE: return "VOLT_SPIKE";
    case REASON_VOLTAGE_DISAGREE_WARNING: return "VOLT_DISAGREE_WARN";
    case REASON_LOCKOUT_ACTIVE: return "LOCKOUT";
    case REASON_CHARGING_DISABLED: return "DISABLED";
    case REASON_CHARGE_COMPLETE_IDLE: return "CHARGE COMPLETE (idle - battery full)";
    case REASON_MANUAL_MODE: return "MANUAL";
    case REASON_INA_OVERVOLTAGE: return "INA228 hardware overvoltage";
    case REASON_FAST_OVERVOLTAGE: return "FAST_OVERVOLTAGE";
    case REASON_HARD_OVERCURRENT: return "HARD_OVERCURRENT";
    case REASON_RPM_TOO_LOW: return "RPM_TOO_LOW";
    case REASON_CURRENT_STALE: return "CURRENT_STALE";
    case REASON_BATTERY_TOO_COLD: return "Battery too cold to charge";
    case REASON_BATTERY_TOO_HOT: return "Battery too hot to charge";
    case REASON_COMMISSION_REST: return "Commissioning idle (field resting)";
    case REASON_ALTZERO_MEASURE: return "Measuring current sensor zero (field at 0%)";
    case REASON_TACH_IMPLAUSIBLE: return "TACH_IMPLAUSIBLE";
    case REASON_SOLAR_PAUSE: return "SOLAR_PAUSE";
    case REASON_BMS_DISABLED: return "BMS_OFF";
    case REASON_OV_TIER_LOW: return "OV_TIER_LOW";
    case REASON_OV_TIER_MID: return "OV_TIER_MID";
    case REASON_BATT_TEMP_LOST: return "Battery temperature sensor not responding";
    case REASON_VOLTAGE_STALE: return "VOLTAGE_STALE";
    case REASON_VOLTAGE2_STALE: return "VOLTAGE2_STALE";
    case REASON_BATT_TEMP_WAIT: return "Waiting for the first battery temperature reading";
    case REASON_MULTI_SENSOR_FAULT: return "MULTI_SENSOR_FAULT";
    case REASON_DIE_TEMP_LOST: return "DIE_TEMP_LOST";
    case REASON_FIELD_RES_TEST: return "Field resistance test";

    default: return "UNKNOWN";
  }
}

// Fault-class reasons: every sensor/OV/OC/temperature cause plus LOCKOUT_ACTIVE, never a deliberate state.
// Mirrors the JS FIELD_FAULT_REASONS set; the N2K engine-bit sender reads it.
bool fieldReasonIsFault(FieldEventReason r) {
  switch (r) {
    case REASON_TEMP_STALE: case REASON_TEMP_CRITICAL: case REASON_TEMP_WARNING: case REASON_TEMP_SUSTAINED:
    case REASON_VOLTAGE_IMPLAUSIBLE: case REASON_VOLTAGE_DISAGREE_CRITICAL: case REASON_VOLTAGE_SPIKE:
    case REASON_VOLTAGE_DISAGREE_WARNING: case REASON_LOCKOUT_ACTIVE: case REASON_INA_OVERVOLTAGE:
    case REASON_HARD_OVERCURRENT: case REASON_CURRENT_STALE: case REASON_FAST_OVERVOLTAGE:
    case REASON_BATTERY_TOO_COLD: case REASON_TACH_IMPLAUSIBLE: case REASON_OV_TIER_LOW:
    case REASON_OV_TIER_MID: case REASON_BATTERY_TOO_HOT: case REASON_BATT_TEMP_LOST:
    case REASON_VOLTAGE_STALE: case REASON_VOLTAGE2_STALE: case REASON_MULTI_SENSOR_FAULT:
    case REASON_DIE_TEMP_LOST:
      return true;
    default:
      return false;
  }
}

// ===== Sensor fault mask (SENSOR_FAULTS_AND_OPTIONAL_SENSORS_SPEC.md §1.2) =====
// The SF_* bit defines live in Xregulator.ino beside g_sensorFaultMask (5_functions.ino reads them too).

const char *sensorFaultName(uint8_t bit) {
  switch (bit) {
    case 0: return "alternator temperature";
    case 1: return "engine speed";
    case 2: return "alternator current";
    case 3: return "main battery voltage";
    case 4: return "second battery voltage";
    case 5: return "battery voltage readings disagree";
    case 6: return "battery temperature";
    case 7: return "ADS1115 analog input chip";
    case 8: return "INA228 battery monitor chip";
    case 9: return "regulator drive temperature (INA228)";
    default: return "sensor";
  }
}

// Plain-English list of the sensors in a fault-bit set, for the alarm text and console. A chip bit names
// the chip once with the readings it carries, instead of listing them as three unrelated failures.
void sensorFaultNames(uint16_t bits, char *out, size_t n) {
  out[0] = '\0';
  size_t len = 0;
  auto add = [&](const char *s) {
    int w = snprintf(out + len, (len < n) ? n - len : 0, "%s%s", len ? ", " : "", s);
    if (w > 0) len += (size_t)w;
  };
  if (bits & SF_ADS1115) {
    add(TempSource == 1 ? "ADS1115 analog input chip (engine speed, alternator current, second battery voltage, thermistor)"
                        : "ADS1115 analog input chip (engine speed, alternator current, second battery voltage)");
    bits &= ~(SF_ENGINE_SPEED | SF_ALT_CURRENT | SF_BATT_V_SECOND | (TempSource == 1 ? SF_ALT_TEMP : 0));
  }
  if (bits & SF_INA228) {
    add((bits & SF_DRIVE_TEMP) ? "INA228 battery monitor chip (main battery voltage, regulator drive temperature)"
                               : "INA228 battery monitor chip (main battery voltage)");
    bits &= ~(SF_BATT_V_MAIN | SF_DRIVE_TEMP);
  }
  if (bits & (SF_BATT_V_MAIN | SF_BATT_V_SECOND)) bits &= ~SF_BATT_V_DISAGREE;   // a relationship, not a third sensor
  for (uint8_t b = 0; b < 7; b++) if (bits & (1u << b)) add(sensorFaultName(b));
  if (bits & SF_DRIVE_TEMP) add(sensorFaultName(9));
}

// Smallest non-zero row of the active amps table: the low-speed (worst cooling) limit. Stands in for a
// missing NoRpmCurrentA.
float smallestRpmTableAmps() {
  float smallest = 0.0f;
  for (int i = 0; i < RPM_TABLE_SIZE; i++) {
    float a = rpmCapCurrentTable[i];
    if (a > 0.0f && (smallest <= 0.0f || a < smallest)) smallest = a;
  }
  return smallest;
}

// Largest row of the min-duty table (the idle floor): the no-output cut's arm-bar base while engine speed is
// ignored, clamped like getMinimumFieldForRPM (scalar Min Field % below, the field ceiling above).
float largestRpmTableMinDuty() {
  float largest = 0.0f;
  for (int i = 0; i < RPM_TABLE_SIZE; i++) if (rpmMinDutyTable[i] > largest) largest = rpmMinDutyTable[i];
  return fminf(fmaxf(largest, MinDuty), ccDutyCeiling());
}

// Mask bits whose sensor the owner has chosen to run without (§3.3: overridden sensors never alarm).
uint16_t sensorOverriddenBits() {
  uint16_t m = 0;
  if (LimpHome) return SF_ALL_BITS;
  if (IgnoreTemperature) m |= SF_ALT_TEMP;
  if (IgnoreRPM) m |= SF_ENGINE_SPEED;
  if (IgnoreAltCurrent) m |= SF_ALT_CURRENT;
  if (IgnoreBattVMain) m |= SF_BATT_V_MAIN | SF_INA228 | SF_DRIVE_TEMP;   // the die reading dies with the chip the owner chose to run without
  if (IgnoreBattV2) m |= SF_BATT_V_SECOND;
  if (IgnoreBattVMain || IgnoreBattV2) m |= SF_BATT_V_DISAGREE;
  // the chip bits are only "overridden" once every reading that chip feeds is ignored
  if (IgnoreRPM && IgnoreAltCurrent && IgnoreBattV2) m |= SF_ADS1115;
  return m;
}

// ===== §1.3: the single reason -> sensor dependency rule =====
// A reason is skipped iff it depends on a sensor the owner chose to run without; every cut/ramp path asks this one
// function. LimpHome suppresses every sensor reason; deliberate states (switch, BMS, solar, idle, manual...) never are.
bool reasonSuppressedByOverride(FieldEventReason r, const TickSnapshot &t) {
  switch (r) {
    case REASON_TEMP_STALE:
    case REASON_TEMP_CRITICAL:
    case REASON_TEMP_WARNING:
    case REASON_TEMP_SUSTAINED:
      return t.ignoreTemperature || t.limpHome;
    case REASON_RPM_TOO_LOW:
      return t.ignoreRPM || t.limpHome;
    case REASON_TACH_IMPLAUSIBLE:      // needs only the alternator current: with engine speed ignored it is the "field on, no output" cut that catches a stalled engine
      return t.ignoreAltCurrent || t.limpHome;
    case REASON_CURRENT_STALE:
    case REASON_HARD_OVERCURRENT:
      return t.ignoreAltCurrent || t.limpHome;
    case REASON_VOLTAGE_DISAGREE_CRITICAL:
    case REASON_VOLTAGE_DISAGREE_WARNING:
      return t.ignoreBattVMain || t.ignoreBattV2 || t.limpHome;
    case REASON_VOLTAGE_IMPLAUSIBLE:   // judged on whichever reading is not ignored (isVoltageSensorPlausible): in tier 3 a second reading gone to ~0 V (lead off, still fresh) cuts
      return (t.ignoreBattVMain && t.ignoreBattV2) || t.limpHome;
    case REASON_VOLTAGE_STALE:
    case REASON_INA_OVERVOLTAGE:       // firmware latch only; the ALERT pin still acts in hardware
    case REASON_DIE_TEMP_LOST:         // the die reading dies with the INA228; there is no separate override (a board sensor, not an installer item)
      return t.ignoreBattVMain || t.limpHome;
    case REASON_VOLTAGE2_STALE:
      return t.ignoreBattV2 || t.limpHome;
    case REASON_FAST_OVERVOLTAGE:      // skipped only with no trusted fresh voltage at all (tier 3 reads the second sensor natively)
      return !t.voltageFeedbackOk;
    case REASON_OV_TIER_LOW:           // the timed tiers run inside the CV loop on IBV: gone with the main reading (inert in tiers 3/4 anyway)
    case REASON_OV_TIER_MID:
      return t.ignoreBattVMain || t.limpHome;
    case REASON_BATTERY_TOO_COLD:
    case REASON_BATTERY_TOO_HOT:
    case REASON_BATT_TEMP_LOST:
    case REASON_BATT_TEMP_WAIT:
      return t.limpHome;
    default:
      return false;
  }
}

/**
 * selectFieldControlMode()
 * PURE function - determines mode from tick snapshot only
 * Priority, first match wins: INA228 OV latch > hard over-current > fast OV / timed OV tiers while GPIO4 is high
 * > Disabled > fast OV > current-sensor zero hold > drive temp lost (Manual / field-res test) > field-res test > Manual
 * > main voltage stale > timed OV tiers > multi-sensor fault > RPM gate > tach-lie > current stale / temp critical
 * > battery too cold / too hot / temp lost / temp wait > Critical > Warning > Lockout > commissioning rest > Auto
 */
FieldControlMode selectFieldControlMode(const TickSnapshot &tick) {

  // PRIORITY 0.5 (mirrors selectFieldEventReason): INA228 latch, hard over-current and over-voltage with the field still
  // ENERGIZED outrank a switch-off — a BMS dropping its enable line arrives with the very spike fast-OV exists for.
  // Once GPIO4 is low the disabled word wins, so an off alternator doesn't walk the OV lockout ladder.
  if (tick.inaOvervoltageLatched && !reasonSuppressedByOverride(REASON_INA_OVERVOLTAGE, tick)) {
    return MODE_CRITICAL_RAMP;
  }
  if (tick.hardOverCurrent && !reasonSuppressedByOverride(REASON_HARD_OVERCURRENT, tick)) {
    return MODE_CRITICAL_RAMP;
  }
  if (!gpio4IsLow && tick.currentBatteryVoltage > tick.alternatorHardShutdownV && !reasonSuppressedByOverride(REASON_FAST_OVERVOLTAGE, tick)) {
    return MODE_WARNING_RAMP_AND_LOCKOUT;
  }
  // Timed OV tiers latch during a shutdown ramp too (FAST OV block): output the ramp is still making above the target.
  if (!gpio4IsLow && g_ovTierTrip != 0 && !reasonSuppressedByOverride(REASON_OV_TIER_LOW, tick)) {
    return MODE_WARNING_RAMP_AND_LOCKOUT;
  }

  // PRIORITY 1: DISABLED (user On/Off switch - always respected)
  if (!tick.chargingEnabled) {
    return MODE_DISABLED_RAMP;
  }

  // PRIORITY 1.5: FAST OVER-VOLTAGE — absolute ceiling, fires in ALL modes incl. MANUAL: immediate cut + adaptive
  // lockout via shouldImmediatelyCutGPIO4. Deliberately above MANUAL and NOT gated by testProtectionsEnabled
  // (over-voltage is always disastrous). Must mirror selectFieldEventReason.
  if (tick.currentBatteryVoltage > tick.alternatorHardShutdownV && !reasonSuppressedByOverride(REASON_FAST_OVERVOLTAGE, tick)) {
    return MODE_WARNING_RAMP_AND_LOCKOUT;
  }

  // PRIORITY 1.9: CURRENT-SENSOR ZERO HOLD — field at 0% for the measurement. Above MANUAL so Zero Now works there;
  // buildTickSnapshot drops the flag when charging is disabled or locked out, so it never re-enables a faulted gate.
  // Must mirror selectFieldEventReason.
  if (tick.altZeroMeasuring) {
    return MODE_COMMISSION_IDLE;
  }

  // PRIORITY 1.95: DRIVE TEMPERATURE LOST in MANUAL or the field resistance test: without the die reading the heat
  // ceiling is frozen, and at 48 V heat binds below the 10 A ceiling. IgnoreBattVMain still suppresses it.
  // Must mirror selectFieldEventReason.
  if ((tick.manualMode || tick.fieldResTest) && tick.dieTempLost && !reasonSuppressedByOverride(REASON_DIE_TEMP_LOST, tick)) {
    return MODE_CRITICAL_RAMP;
  }

  // PRIORITY 1.97: FIELD RESISTANCE TEST — engine off by its own precondition, so it sits above the RPM gate like
  // MANUAL, under everything above. It ends itself on a stale voltage and on its own checks. Mirrors selectFieldEventReason.
  if (tick.fieldResTest) {
    return MODE_FIELD_RES_TEST;
  }

  // PRIORITY 2: MANUAL MODE (UNRESTRICTED - bypasses all safeties when user wants manual control)
  if (tick.manualMode) {
    return MODE_NORMAL_MANUAL;
  }

  // PRIORITY 2.05: MAIN BATTERY VOLTAGE STALE — nothing can supervise AUTO; immediate cut (§10.1). Below MANUAL by
  // owner ruling: manual is a bench tool that must run with the chip dead. Must mirror selectFieldEventReason.
  if (tick.ibvStale && !reasonSuppressedByOverride(REASON_VOLTAGE_STALE, tick)) {
    return MODE_CRITICAL_RAMP;
  }

  // PRIORITY 2.1: TIMED OV TIERS — sustained-violation cut latched by the FAST OV block (AUTO/CV-gated, so below MANUAL
  // by construction). Immediate cut + adaptive fast-OV lockout. Must mirror selectFieldEventReason.
  if (g_ovTierTrip != 0 && !reasonSuppressedByOverride(REASON_OV_TIER_LOW, tick)) {
    return MODE_WARNING_RAMP_AND_LOCKOUT;
  }

  // PRIORITY 2.5: TWO OR MORE REQUIRED SENSORS FAULTED (spec §11). Above the RPM gate so a dead ADS1115
  // (RPM forced to 0) is reported as the chip fault it is, not as "engine not running". Immediate cut,
  // 30 s lockout, clears 10 s after the count drops to one. Mirrors selectFieldEventReason.
  if (tick.multiSensorFault) {
    return MODE_CRITICAL_RAMP;
  }

  // PRIORITY 3: RPM GATE (field must be cut when engine is not running). rpmGateActive already
  // carries the §1.3 test and the two-reading rule for an energized field (buildTickSnapshot).
  if (tick.rpmGateActive) {
    return MODE_CRITICAL_RAMP;
  }

  // PRIORITY 3.2: TACH-LIE PLAUSIBILITY (tach claims running, field driven hard, zero output —
  // the RPM signal is noise or the alternator is dead; either way charging is impossible).
  // Immediate cut via shouldImmediatelyCutGPIO4 + escalating lockout. Mirrors selectFieldEventReason.
  if (tick.tachImplausible) {
    return MODE_CRITICAL_RAMP;
  }

  // PRIORITY 3.3: IMMEDIATE-CUT SENSOR FAULTS, above the graceful battery-temperature ramps: only the top reason decides
  // the cut, so an immediate reason ranked under a running ramp would never fire. Mirrors selectFieldEventReason.
  if (tick.currentDataStale && !reasonSuppressedByOverride(REASON_CURRENT_STALE, tick)) {
    return MODE_CRITICAL_RAMP;
  }
  if (tick.tempToUseF > (tick.tempLimitF + tick.tempCritExcessF) && !reasonSuppressedByOverride(REASON_TEMP_CRITICAL, tick)) {
    return MODE_CRITICAL_RAMP;
  }

  // PRIORITY 3.5: COLD-CHARGE LOCKOUT (opt-in lithium protection). Graceful ramp + lockout, not a hard chop; the
  // settle-cut path finishes the disconnect. AUTO only; hysteresis in buildTickSnapshot. Mirrors selectFieldEventReason.
  if (tick.batteryTooCold && !reasonSuppressedByOverride(REASON_BATTERY_TOO_COLD, tick)) {
    return MODE_WARNING_RAMP_AND_LOCKOUT;
  }

  // PRIORITY 3.6: HOT-CHARGE LOCKOUT (opt-in — a measured battery temperature above MaxChargeTempF).
  // Same graceful ramp + lockout as the cold check, below MANUAL for the same reason — mirrors
  // selectFieldEventReason. Hysteresis and the source gate live in buildTickSnapshot.
  if (tick.batteryTooHot && !reasonSuppressedByOverride(REASON_BATTERY_TOO_HOT, tick)) {
    return MODE_WARNING_RAMP_AND_LOCKOUT;
  }

  // PRIORITY 3.7: BATTERY TEMPERATURE LOST (§2.3) — an expected battery temperature with no
  // qualifying reading past the boot grace. Same graceful ramp as the lockouts; clears when a reading
  // returns or the owner declares Not fitted. Mirrors selectFieldEventReason.
  if (tick.battTempState == 2 && !reasonSuppressedByOverride(REASON_BATT_TEMP_LOST, tick)) {
    return MODE_WARNING_RAMP_AND_LOCKOUT;
  }

  // PRIORITY 3.8: BATTERY TEMPERATURE WAIT (§2.3) — first reading not yet delivered. Held off like a switch-off (no
  // lockout, alarm or fault) so charging starts the tick it lands. Owner ruling: never charge blind, not even for the
  // 90 s grace. Mirrors selectFieldEventReason.
  if (tick.battTempState == 3 && !reasonSuppressedByOverride(REASON_BATT_TEMP_WAIT, tick)) {
    return MODE_DISABLED_RAMP;
  }

  // PRIORITY 4: CRITICAL CONDITIONS (auto mode only)
  if (tick.tempDataVeryStale && !reasonSuppressedByOverride(REASON_TEMP_STALE, tick)) {
    return MODE_CRITICAL_RAMP;
  }
  if (tick.dieTempLost && !reasonSuppressedByOverride(REASON_DIE_TEMP_LOST, tick)) {
    return MODE_CRITICAL_RAMP;
  }
  if (tick.battVStale && !reasonSuppressedByOverride(REASON_VOLTAGE2_STALE, tick)) {
    return MODE_CRITICAL_RAMP;
  }
  if (!tick.voltagePlausible && !reasonSuppressedByOverride(REASON_VOLTAGE_IMPLAUSIBLE, tick)) {
    return MODE_CRITICAL_RAMP;
  }
  if (tick.voltageDisagreementCritical && !reasonSuppressedByOverride(REASON_VOLTAGE_DISAGREE_CRITICAL, tick)) {
    return MODE_CRITICAL_RAMP;
  }

  // PRIORITY 5: WARNING CONDITIONS (all start lockout); the hard-shutdown OV check lives at PRIORITY 1.5.
  if (tick.voltageDisagreementWarning && !reasonSuppressedByOverride(REASON_VOLTAGE_DISAGREE_WARNING, tick)) {
    return MODE_WARNING_RAMP_AND_LOCKOUT;
  }
  if (tick.tempToUseF > (tick.tempLimitF + tick.tempWarnExcessF) && !reasonSuppressedByOverride(REASON_TEMP_WARNING, tick)) {
    return MODE_WARNING_RAMP_AND_LOCKOUT;
  }

  // PRIORITY 7: LOCKOUT
  if (tick.inLockout) {
    return MODE_LOCKOUT_RAMP;
  }

  // PRIORITY 7.6: COMMISSIONING IDLE REST — between guided wizard steps, hold the field at low duty instead of charging.
  // Below every safety/lockout gate, above NORMAL AUTO. Mirrors selectFieldEventReason.
  if (tick.commissioningResting) {
    return MODE_COMMISSION_IDLE;
  }

  // PRIORITY 8: NORMAL AUTO
  return MODE_NORMAL_AUTO;
}

/**
 * PURE function - MUST match selectFieldControlMode() priority exactly
 */
FieldEventReason selectFieldEventReason(const TickSnapshot &tick) {
  // Priority 0.5 (mirrors selectFieldControlMode). Hard over-current is debounced in buildTickSnapshot: this function
  // is called twice per tick and must not own state.
  if (tick.inaOvervoltageLatched && !reasonSuppressedByOverride(REASON_INA_OVERVOLTAGE, tick)) return REASON_INA_OVERVOLTAGE;
  if (tick.hardOverCurrent && !reasonSuppressedByOverride(REASON_HARD_OVERCURRENT, tick)) return REASON_HARD_OVERCURRENT;
  if (!gpio4IsLow && tick.currentBatteryVoltage > tick.alternatorHardShutdownV && !reasonSuppressedByOverride(REASON_FAST_OVERVOLTAGE, tick)) return REASON_FAST_OVERVOLTAGE;
  if (!gpio4IsLow && g_ovTierTrip == 2 && !reasonSuppressedByOverride(REASON_OV_TIER_MID, tick)) return REASON_OV_TIER_MID;
  if (!gpio4IsLow && g_ovTierTrip == 1 && !reasonSuppressedByOverride(REASON_OV_TIER_LOW, tick)) return REASON_OV_TIER_LOW;

  // Priority 1a: Disabled (user control - always show this reason if off)
  if (!tick.chargingEnabled) return tick.solarForecastPause ? REASON_SOLAR_PAUSE
                                    : (tick.bmsBlocking ? REASON_BMS_DISABLED
                                    : (tick.idleHold ? REASON_CHARGE_COMPLETE_IDLE : REASON_CHARGING_DISABLED));

  // Priority 1.5: Fast over-voltage — mirrors selectFieldControlMode PRIORITY 1.5.
  if (tick.currentBatteryVoltage > tick.alternatorHardShutdownV && !reasonSuppressedByOverride(REASON_FAST_OVERVOLTAGE, tick)) return REASON_FAST_OVERVOLTAGE;

  // Priority 1.9: Current-sensor zero hold (not a fault). Mirrors selectFieldControlMode.
  if (tick.altZeroMeasuring) return REASON_ALTZERO_MEASURE;

  // Priority 1.95: drive temperature lost in MANUAL or the field resistance test — mirrors selectFieldControlMode.
  if ((tick.manualMode || tick.fieldResTest) && tick.dieTempLost && !reasonSuppressedByOverride(REASON_DIE_TEMP_LOST, tick)) return REASON_DIE_TEMP_LOST;

  // Priority 1.97: field resistance test — mirrors selectFieldControlMode.
  if (tick.fieldResTest) return REASON_FIELD_RES_TEST;

  // Priority 2: Manual mode
  if (tick.manualMode) return REASON_MANUAL_MODE;

  // Priority 2.05: main battery voltage stale — below MANUAL by owner ruling. Mirrors selectFieldControlMode.
  if (tick.ibvStale && !reasonSuppressedByOverride(REASON_VOLTAGE_STALE, tick)) return REASON_VOLTAGE_STALE;

  // Priority 2.1: Timed OV tiers — mirrors selectFieldControlMode. MID before LOW (higher rung).
  if (g_ovTierTrip == 2 && !reasonSuppressedByOverride(REASON_OV_TIER_MID, tick)) return REASON_OV_TIER_MID;
  if (g_ovTierTrip == 1 && !reasonSuppressedByOverride(REASON_OV_TIER_LOW, tick)) return REASON_OV_TIER_LOW;

  // Priority 2.5: two or more required sensors faulted (§11) — mirrors selectFieldControlMode.
  if (tick.multiSensorFault) return REASON_MULTI_SENSOR_FAULT;

  // Priority 3: RPM gate (rpmGateActive already carries the §1.3 test and the two-reading rule)
  if (tick.rpmGateActive) return REASON_RPM_TOO_LOW;

  // Priority 3.2: Tach-lie plausibility — mirrors selectFieldControlMode.
  if (tick.tachImplausible) return REASON_TACH_IMPLAUSIBLE;

  // Priority 3.3: immediate-cut sensor faults above the graceful battery-temperature reasons — mirrors selectFieldControlMode.
  if (tick.currentDataStale && !reasonSuppressedByOverride(REASON_CURRENT_STALE, tick)) return REASON_CURRENT_STALE;
  if (tick.tempToUseF > (tick.tempLimitF + tick.tempCritExcessF) && !reasonSuppressedByOverride(REASON_TEMP_CRITICAL, tick)) {
    return REASON_TEMP_CRITICAL;
  }

  // Priority 3.5: Cold-charge lockout (opt-in lithium protection). Mirrors selectFieldControlMode.
  if (tick.batteryTooCold && !reasonSuppressedByOverride(REASON_BATTERY_TOO_COLD, tick)) return REASON_BATTERY_TOO_COLD;

  // Priority 3.6: Hot-charge lockout (opt-in, measured battery source only). Mirrors selectFieldControlMode.
  if (tick.batteryTooHot && !reasonSuppressedByOverride(REASON_BATTERY_TOO_HOT, tick)) return REASON_BATTERY_TOO_HOT;

  // Priority 3.7: Battery temperature lost (§2.3). Mirrors selectFieldControlMode.
  if (tick.battTempState == 2 && !reasonSuppressedByOverride(REASON_BATT_TEMP_LOST, tick)) return REASON_BATT_TEMP_LOST;

  // Priority 3.8: Battery temperature wait (§2.3). Mirrors selectFieldControlMode.
  if (tick.battTempState == 3 && !reasonSuppressedByOverride(REASON_BATT_TEMP_WAIT, tick)) return REASON_BATT_TEMP_WAIT;

  // Priority 4: Critical (auto mode only)
  if (tick.tempDataVeryStale && !reasonSuppressedByOverride(REASON_TEMP_STALE, tick)) return REASON_TEMP_STALE;
  if (tick.dieTempLost && !reasonSuppressedByOverride(REASON_DIE_TEMP_LOST, tick)) return REASON_DIE_TEMP_LOST;
  if (tick.battVStale && !reasonSuppressedByOverride(REASON_VOLTAGE2_STALE, tick)) return REASON_VOLTAGE2_STALE;
  if (!tick.voltagePlausible && !reasonSuppressedByOverride(REASON_VOLTAGE_IMPLAUSIBLE, tick)) return REASON_VOLTAGE_IMPLAUSIBLE;
  if (tick.voltageDisagreementCritical && !reasonSuppressedByOverride(REASON_VOLTAGE_DISAGREE_CRITICAL, tick)) return REASON_VOLTAGE_DISAGREE_CRITICAL;

  // Priority 5: Warning
  if (tick.voltageDisagreementWarning && !reasonSuppressedByOverride(REASON_VOLTAGE_DISAGREE_WARNING, tick)) return REASON_VOLTAGE_DISAGREE_WARNING;
  if (tick.tempToUseF > (tick.tempLimitF + tick.tempWarnExcessF) && !reasonSuppressedByOverride(REASON_TEMP_WARNING, tick)) {
    if (tempWarningStartMs > 0 && (tick.nowMs - tempWarningStartMs > TempSustainedTimeout)) {
      return REASON_TEMP_SUSTAINED;
    }
    return REASON_TEMP_WARNING;
  }

  // Priority 7: Lockout
  if (tick.inLockout) return REASON_LOCKOUT_ACTIVE;

  // Priority 7.6: Commissioning idle-rest hold (not a fault). Mirrors selectFieldControlMode.
  if (tick.commissioningResting) return REASON_COMMISSION_REST;

  return REASON_NONE;
}

// Counts rising edges of reasons that map to protection counters.
void updateProtectionCounters(FieldEventReason reason) {
  static FieldEventReason prevReason = REASON_NONE;
  if (reason != prevReason) {
    switch (reason) {
      case REASON_INA_OVERVOLTAGE:
        g_inaOVCount++;
        ovTel().inaCutCount++;  // lifetime twin (RTC) of the session counter
        break;
      case REASON_HARD_OVERCURRENT: g_hardOCCount++; break;
      case REASON_FAST_OVERVOLTAGE:
        g_voltSpikeCount++;  // reuses the OV-spike counter (REASON_VOLTAGE_SPIKE retired)
        ovTel().swHardCutCount++;  // lifetime twin (RTC) of the session counter
        break;
      case REASON_OV_TIER_LOW:
        g_ovTierLowCount++;
        ovTel().tierLowCutCount++;  // lifetime twin (RTC) of the session counter
        break;
      case REASON_OV_TIER_MID:
        g_ovTierMidCount++;
        ovTel().tierMidCutCount++;  // lifetime twin (RTC) of the session counter
        break;
      case REASON_VOLTAGE_SPIKE: g_voltSpikeCount++; break;
      case REASON_VOLTAGE_DISAGREE_CRITICAL: g_voltDisagreeCritCount++; break;
      case REASON_VOLTAGE_DISAGREE_WARNING: g_voltDisagreeWarnCount++; break;
      case REASON_VOLTAGE_IMPLAUSIBLE: g_voltImplausibleCount++; break;
      case REASON_TEMP_CRITICAL: g_tempCritCount++; break;
      case REASON_TEMP_SUSTAINED: g_tempSustainedCount++; break;
      case REASON_TEMP_STALE: g_tempStaleCount++; break;
      case REASON_CURRENT_STALE: g_currentStaleCount++; break;
      case REASON_VOLTAGE_STALE: g_voltStaleCount++; break;
      case REASON_VOLTAGE2_STALE: g_volt2StaleCount++; break;
      case REASON_BATT_TEMP_LOST: g_battTempLostCount++; break;
      case REASON_DIE_TEMP_LOST: g_dieTempLostCount++; break;
      default: break;
    }
    prevReason = reason;
  }
}

void reportFieldModeEvent(uint32_t nowMs, FieldControlMode mode, FieldEventReason reason,
                          const TickSnapshot &tick, bool gpio4Low, float appliedDuty) {
  if (mode == lastReportedMode && reason == lastReportedReason) {
    return;
  }

  lastReportedMode = mode;
  lastReportedReason = reason;
  lastReportMs = nowMs;

  char msg[300];

  char lockoutStatus[32] = "none";
  if (tick.inLockout) {
    float remaining = (activeCollapseDelay - (tick.nowMs - fieldCollapseTime)) / 1000.0f;
    snprintf(lockoutStatus, sizeof(lockoutStatus), "%.1fs remaining", remaining);
  }

  const char *fieldStatus = gpio4Low ? "OFF" : "ON";

  if (reason == REASON_TEMP_STALE || reason == REASON_TEMP_CRITICAL || reason == REASON_TEMP_WARNING || reason == REASON_TEMP_SUSTAINED) {
    snprintf(msg, sizeof(msg),
             "FIELD: %s | %s | Temp=%.1f%s (Limit=%.1f%s) | PWM=%.1f%% | Field=%s | Lockout=%s",
             modeToString(mode),
             reasonToString(reason),
             dispTempF(tick.tempToUseF),
             dispTempUnit(),
             dispTempF(tick.tempLimitF),
             dispTempUnit(),
             appliedDuty,
             fieldStatus,
             lockoutStatus);
  } else {
    float vDelta = fabsf(tick.batteryV - tick.ibv);
    snprintf(msg, sizeof(msg),
             "FIELD: %s | %s | Vbat=%.2fV IBV=%.2fV (Δ=%.3fV) | PWM=%.1f%% | Field=%s | Lockout=%s",
             modeToString(mode),
             reasonToString(reason),
             tick.currentBatteryVoltage,
             tick.ibv,
             vDelta,
             appliedDuty,
             fieldStatus,
             lockoutStatus);
  }

  Serial.println(msg);
  queueConsoleMessage(msg);
}

/**
 * Reports duty-based values even when GPIO4 is LOW (PWM disconnected), for logging consistency.
 */
void updateFieldTelemetry(float duty, float voltage) {
  vvout = duty / 100.0f * voltage;
  // The same estimate the 10 A ceiling acts on: freewheel drop over the off-time, alternator-temperature-corrected R.
  const float d = duty / 100.0f;
  // No resistance entered: NAN (the readout shows a dash, never a made-up 0).
  const float i = (d * voltage - (1.0f - d) * FD_VF_EST) / g_fieldRcOhm;
  iiout = !isfinite(g_fieldRcOhm) ? NAN : (g_fieldRcOhm > 0.001f && i > 0.0f) ? i : 0.0f;
  MARK_FRESH(IDX_DUTY_CYCLE);
  MARK_FRESH(IDX_FIELD_VOLTS);
  MARK_FRESH(IDX_FIELD_AMPS);
}


/**
 * True = skip the Phase 1 ramp and cut now. The following applyDutyCycle() then only tracks rate-limiter
 * state and telemetry (GPIO4 is already LOW).
 */
bool shouldImmediatelyCutGPIO4(FieldEventReason reason) {
  if (reason == REASON_FAST_OVERVOLTAGE) return true;   // absolute OV ceiling — instant cut in every mode
  if (reason == REASON_OV_TIER_LOW) return true;        // timed OV tiers — dwell already served in the detector; the cut itself is immediate
  if (reason == REASON_OV_TIER_MID) return true;
  if (reason == REASON_INA_OVERVOLTAGE) return true;
  if (reason == REASON_HARD_OVERCURRENT) return true;
  if (reason == REASON_RPM_TOO_LOW) return true;
  if (reason == REASON_TACH_IMPLAUSIBLE) return true;  // tach already garbage; every energized second is pure battery drain
  // Stale current must cut here: CURRENT_STALE outranks TEMP_CRITICAL in selectFieldEventReason,
  // so a non-cutting stale reason would mask the temp-critical cut too.
  if (reason == REASON_CURRENT_STALE) return true;
  if (reason == REASON_VOLTAGE_STALE) return true;   // no valid main voltage: nothing can supervise a ramp (§10.1)
  if (reason == REASON_MULTI_SENSOR_FAULT) return true;  // §11: with two sensors gone nothing is guaranteed to supervise a ramp
  if (reason == REASON_TEMP_CRITICAL) {
    return true;
  }
  return false;
}

// Tick-aware wrapper for the cut sites: in tier 3 the second reading IS the only voltage, so VOLTAGE2_STALE (normally a
// phased ramp) and VOLTAGE_IMPLAUSIBLE (a lead off still reads ~0 V and fresh) must cut at once.
static inline bool immediateCutNow(FieldEventReason reason, const TickSnapshot &tick) {
  return shouldImmediatelyCutGPIO4(reason)
      || ((reason == REASON_VOLTAGE2_STALE || reason == REASON_VOLTAGE_IMPLAUSIBLE) && tick.ignoreBattVMain);
}

/**
 * Call AFTER governor_apply() with the computed duty (even if not written) so settle detection matches hardware.
 * Mode changes do not reset settledAtZeroDutyMs while duty stays 0, so churn can't delay the cut. nowMs, not millis().
 */
bool shouldCutGPIO4AfterSettle(FieldEventReason reason, uint32_t nowMs, float appliedDuty) {
  if (appliedDuty > 0.01f) {
    settledAtZeroDutyMs = 0;
    return false;
  }

  if (settledAtZeroDutyMs == 0) {
    settledAtZeroDutyMs = nowMs;
    return false;
  }

  if (nowMs - settledAtZeroDutyMs < SettleTimeBeforeCut) {
    return false;
  }

  switch (reason) {
    // Fast-responding faults: cut if fault persists after ramp-down
    case REASON_FAST_OVERVOLTAGE:   // normally cut immediately; defensive if it ever reaches settle
    case REASON_OV_TIER_LOW:        // timed tiers: same defensive settle backstop
    case REASON_OV_TIER_MID:
    case REASON_VOLTAGE_SPIKE:
    case REASON_VOLTAGE_DISAGREE_WARNING:
    case REASON_VOLTAGE_IMPLAUSIBLE:
    case REASON_VOLTAGE_DISAGREE_CRITICAL:
    case REASON_TEMP_STALE:
    case REASON_CURRENT_STALE:
    case REASON_HARD_OVERCURRENT:
    case REASON_RPM_TOO_LOW:
    case REASON_BATTERY_TOO_COLD:   // cold-charge lockout: ramp to zero, then cut once settled
    case REASON_BATTERY_TOO_HOT:    // hot-charge lockout: same path
    case REASON_BATT_TEMP_LOST:     // expected battery temperature missing: same path
    case REASON_DIE_TEMP_LOST:      // no drive temperature, no heat ceiling: same path
    case REASON_VOLTAGE_STALE:      // normally cut immediately; defensive if it ever reaches settle
    case REASON_VOLTAGE2_STALE:     // critical ramp, then cut
      return true;

    // Temperature sustained: cut after 2-minute timeout
    case REASON_TEMP_SUSTAINED:
      return true;

    // Temperature warning: check if sustained timeout reached
    case REASON_TEMP_WARNING:
      if (tempWarningStartMs > 0 && (nowMs - tempWarningStartMs > TempSustainedTimeout)) {
        return true;
      }
      return false;

      // Intentional shutdown: cut after settle
    case REASON_LOCKOUT_ACTIVE:
      return true;

      // Normal user shutdown, and the finished-cycle rest: cut after settle (Phase 4 handles the slow ramp)
    case REASON_CHARGING_DISABLED:
    case REASON_CHARGE_COMPLETE_IDLE:
      return true;

      // Same class as CHARGING_DISABLED: only produced with chargingEnabled false (MODE_DISABLED_RAMP). Must cut, or
      // the enable line stays up (boost on, Q3 out of UVLO) while the UI reads "field off".
    case REASON_BMS_DISABLED:
    case REASON_SOLAR_PAUSE:
      return true;

      // Battery-temperature wait (§2.3): produced with chargingEnabled TRUE but lands in MODE_DISABLED_RAMP
      // like the two above. Cut once settled, no lockout, so the field returns the tick the reading lands.
    case REASON_BATT_TEMP_WAIT:
      return true;

      // Fail safe: only called from runShutdownPath, so once duty has held 0 opening the gate is always correct
      // (REASON_TEMP_WARNING, the one that must stay up, has its own case). Unknown reason (e.g. REASON_NONE) → cut.
    default:
      return true;
  }
}



// Any test that owns the field, running or queued — the hold would fight it, so a zero request mid-test is refused
// outright. Same union as buildTickSnapshot anyTestActive minus faCommissionGate (which only asks for live charging).
// Lives here, not next to its caller in 2_functions.ino, because resTestActive is declared later in that file.
bool altZeroTestBusy() {
  return (fieldCurveActive != 0) || (systemIDActive != 0) || (fieldCutActive != 0) ||
         fieldCurveRequested || systemIDRequested || fieldCutRequested ||
         (protTestActive != 0) || protTestRequested ||
         resTestActive || batteryHealthTestActive || cvPlantFitActive || cvStressActive ||
         (altSweepActive != 0) || altSweepRequested ||
         (chcActive != 0) || chcRequested ||
         TuningMode || CVTuningMode;
}

/**
 * Constructs immutable snapshot of system state for pure decision functions
 */
TickSnapshot buildTickSnapshot(uint32_t currentMillis, uint32_t dt_ms) {
  TickSnapshot tick = {};
  static bool s_multiSensorFault = false;   // §11 latch: set at count >= 2, cleared 10 s after the count is back to <= 1 (written at the end of this function)

  tick.nowMs = currentMillis;
  tick.dt_ms = dt_ms;

  tick.inaOvervoltageLatched = inaOvervoltageLatched;

  // Voltage sensors
  tick.batteryV = BatteryV;
  tick.ibv = IBV;

  // The ignore flags are the owner's Run Without Failed Sensors overrides only: no automatic degrade (owner ruling).
  tick.manualMode = (ManualFieldToggle == 1);
  tick.fieldResTest = (frt.state == FRT_RUNNING);
  tick.multiSensorFault = s_multiSensorFault;
  tick.ignoreTemperature = (IgnoreTemperature != 0);
  tick.ignoreRPM = (IgnoreRPM != 0);
  tick.ignoreAltCurrent = (IgnoreAltCurrent != 0);
  tick.ignoreBattVMain = (IgnoreBattVMain != 0);
  tick.ignoreBattV2 = (IgnoreBattV2 != 0);
  tick.limpHome = (LimpHome != 0);

  // §1.1: "spinning" is instantaneous (RPM >= 200, or effective ignition when engine speed is ignored) and paces TempTask.
  // engineRunning = spinning held 15 s: the last engine-off temp sample can be 60 s old, so staleness consumers wait.
  {
    static uint32_t lastNotSpinningMs = 0;
    bool spinningNow = tick.ignoreRPM ? (Ignition == 1) : (RPM >= 200);
    if (!spinningNow) lastNotSpinningMs = currentMillis;
    tick.engineRunning = spinningNow && ((uint32_t)(currentMillis - lastNotSpinningMs) >= 15000UL);
    g_engineSpinningNow = spinningNow;
    g_engineRunning = tick.engineRunning;
  }

  // §10.1 / §10.2 stale flags are computed in _ReadAnalogInputs_inner (read-count rules).
  tick.ibvStale = g_ibvStale;
  tick.battVStale = g_battVStale;

  // §4.2 running tier. Tier 3 vs 4 is decided by FRESHNESS of a committed second reading, never by the
  // plausibility upper bound: a fresh BatteryV above the class window is an over-voltage for the fast-OV
  // rung to act on, not a failed sensor (the commit window already spans up to 70 V x class/12).
  {
    bool battV2Usable = !tick.ignoreBattV2 && ADS1115Disconnected == 0 && !g_battVStale;
    if (tick.limpHome)             tick.limpTier = 4;
    else if (tick.ignoreBattVMain) tick.limpTier = battV2Usable ? 3 : 4;
    else if (tick.ignoreAltCurrent) tick.limpTier = 2;
    // IgnitionOverride is not a tier: a boat with no ignition wire is a normal install.
    else if (tick.ignoreTemperature || tick.ignoreRPM || tick.ignoreBattV2) tick.limpTier = 1;
    else                           tick.limpTier = 0;
    g_limpTier = tick.limpTier;
    // Fast OV and the timed tiers are skipped only when NO trusted fresh voltage feeds them (§1.3):
    // LimpHome, or the main reading ignored with the second one ignored or stale.
    tick.voltageFeedbackOk = !tick.limpHome && (!tick.ignoreBattVMain || battV2Usable);
    // §4.4: tiers 3/4 have no main voltage for the stage machine. Park it in bulk on ENTRY (before the
    // idleHold test below), so a bank left in IDLE can charge under a fixed duty.
    static uint8_t lastTierForStage = 0;
    if (tick.limpTier >= 3 && lastTierForStage < 3 && !tick.manualMode) {
      inIdleStage = false; inAbsorptionStage = false; inBulkStage = true;
    }
    lastTierForStage = tick.limpTier;
  }

  // Tier 3 (§4.4): the second reading IS the battery voltage this tick. Tier 4 has no usable voltage (§1.3 suppresses
  // the voltage reasons). Neither feeds the field ceilings.
  tick.currentBatteryVoltage = (tick.limpTier == 3) ? BatteryV : getBatteryVoltage();

  // Re-solve the field ceilings before any ccDutyCeiling() consumer this tick. Tier 3/4 or a stale main voltage solves
  // against fieldCeilAssumedBusV() (no pushing limits in a degraded mode). Own per-call dt: dt_ms grows across passes
  // between ticks and would collapse the 2 s bus filter.
  static uint32_t lastCeilMs = 0;
  uint32_t ceilDtMs = (lastCeilMs == 0) ? 0 : (currentMillis - lastCeilMs);
  lastCeilMs = currentMillis;
  if (ceilDtMs > 500) ceilDtMs = 500;
  const bool busUnmeasured = (tick.limpTier >= 3 || tick.ibvStale);
  updateFieldVoltCeiling(busUnmeasured ? fieldCeilAssumedBusV() : tick.currentBatteryVoltage, (float)ceilDtMs / 1000.0f);
  {
    const uint32_t fdT0 = micros();
    updateFieldDerateCeilings((float)ceilDtMs / 1000.0f, busUnmeasured);
    const uint32_t fdUs = micros() - fdT0;
    if (fdUs > g_fieldDerateWorstUs) g_fieldDerateWorstUs = fdUs;
  }

  tick.batteryCurrentA = Bcur;

  tick.rpmMinDuty = tick.ignoreRPM ? 0.0f : getMinimumFieldForRPM(RPM);   // §4.8: no tach to keep alive

  // Charging enabled (with BMS and weather mode overrides)
  bool chargingEnabledLocal = (Ignition == 1 && OnOff == 1);

  // GPIO42 = BMSLogic opto (U16); inverted — the phototransistor pulls the pin low when the
  // external 5-62 V signal is present. Read every tick even with bmsLogic off: the Integrations
  // status row is how an installer verifies the wiring BEFORE handing the wire any authority.
  bmsSignalActive = !digitalRead(42);
  bool bmsPermits = true;
  if (bmsLogic == 1) {
    bmsPermits = (bmsLogicLevelOff == 0) ? bmsSignalActive : !bmsSignalActive;
    chargingEnabledLocal = chargingEnabledLocal && bmsPermits;
  }
  // Sole-cause flag for the OFF reason word — true only while the BMS is the one thing saying no,
  // so a key-off or a finished charge cycle keeps its own (more useful) reason.
  tick.bmsBlocking = !bmsPermits && (Ignition == 1 && OnOff == 1) && !inIdleStage;
  // Idle stage (UseFloat=0, post-absorption): battery full, stop charging; rebulk or a master-switch off/on clears it.
  // Exempt where updateChargingStage() can't run so IDLE could never clear: MANUAL (operator override on purpose), Maintain
  // and Target Voltage (an explicit override owns the field), tiers 3/4 (no main voltage), and the field resistance test
  // (not charging).
  tick.idleHold = inIdleStage && chargingEnabledLocal && !tick.manualMode && TargetVoltageMode == 0 && MaintainMode == 0
                  && !tick.fieldResTest && tick.limpTier < 3;
  idleHoldActive = tick.idleHold;
  if (tick.idleHold) chargingEnabledLocal = false;

  // Weather mode: rest the alternator when the solar forecast is strong. Applied LAST so the flag
  // means "a sunny forecast is the ONLY thing holding charging off" — the banner must not blame
  // the forecast when the key, the on/off switch, the BMS, or a full battery is the real cause.
  // Demo mode: the forecast is the real boat's, not the simulated one's.
  tick.solarForecastPause = chargingEnabledLocal && weatherModeEnabled == 1 && currentWeatherMode == 1 && !tick.fieldResTest && !demoSeal;
  if (tick.solarForecastPause) chargingEnabledLocal = false;

  tick.chargingEnabled = chargingEnabledLocal;

  // Protection-event grace: a clamp runs the field near MinDuty, which can starve the LM2907 — an EXACTLY-zero reading in
  // the window is dropout, not a stop (rpmZeroConfirm keeps counting, so a real stall cuts at expiry). Charging-enabled only.
  // Field-cut / protection tests: the abrupt cut makes the LM2907 spike, false-zero, then ramp through garbage low values
  // for ~4.6 s, so both RPM gates are masked for ANY reading during the test and tail (absolute protections stay live).
  // testsParkedByTier: a tier never runs the test ticks, so a stale Active flag would mask both gates for the whole tier.
  const bool testsParkedByTier = (tick.limpTier >= 2) && (sysMode == SYS_MODE_AUTO);
  bool fieldCutRpmGrace = (!testsParkedByTier
                           && ((fieldCutActive != 0) || fieldCutCcActive || fieldCutRequested
                               || (protTestActive != 0) || protTestCcActive || protTestRequested))
                          || (fieldCutLastEndMs != 0
                              && (uint32_t)(currentMillis - fieldCutLastEndMs) < FIELDCUT_RPM_GRACE_MS)
                          // protTest needs its own end-stamp tail: its cut lands one tick AFTER the flags above
                          // clear, so the ~4.6s false-zero would otherwise run unmasked.
                          || (protTestLastEndMs != 0
                              && (uint32_t)(currentMillis - protTestLastEndMs) < FIELDCUT_RPM_GRACE_MS)
                          || (tierEndedCutTestMs != 0
                              && (uint32_t)(currentMillis - tierEndedCutTestMs) < FIELDCUT_RPM_GRACE_MS);
  // Hard-cut tail: an abrupt protection cut slams the LM2907 the same way (applyImmediateCut never stamps
  // g_lastProtClampMs, and the ramp is nonzero), else a phantom RPM_TOO_LOW steals the real abort reason. Masked only
  // while there is something to protect: a commissioning ramp, or charging enabled (a key-off spin-down should cut).
  bool hardCutRpmGrace = (g_lastFieldCutMs != 0)
                         && ((uint32_t)(currentMillis - g_lastFieldCutMs) < FIELDCUT_RPM_GRACE_MS)
                         && ((fieldCurveActive != 0) || chargingEnabledLocal);
  // g_rpmTachMasked is published for the engine start/stop console lines so a field-cut can't print phantom STOPPED/STARTED.
  // Current-sensor zero hold: field deliberately 0% on a running engine, so mask both gates for the hold + ~4.6 s re-bias
  // tail; nothing is energized under the hold, so this exposes nothing. Never for an engine-off capture: on a stopped
  // engine the tail would let AUTO energize a stationary field.
  bool altZeroRpmGrace = ((AltZeroCaptureNow == 1) && altZeroCapPhase >= ALTZERO_PH_RAMP && !altZeroCapEngineOff)
                         || (altZeroHoldEndMs != 0
                             && (uint32_t)(currentMillis - altZeroHoldEndMs) < FIELDCUT_RPM_GRACE_MS);
  bool rpmDropoutGrace = fieldCutRpmGrace || hardCutRpmGrace || altZeroRpmGrace
                         || ((g_lastProtClampMs != 0)
                             && ((uint32_t)(currentMillis - g_lastProtClampMs) < PROT_RPM_GRACE_MS)
                             && chargingEnabledLocal
                             && (RPM <= 0.0f));
  // Engine confirmed stopped: RPM exactly 0 for >= RPM_ZERO_CUT_MS with a second zero reading, or RPM_ZERO_MAX_WAIT_MS
  // with none (RPM is floored to 0 below 100). The hold counts through rpmDropoutGrace.
  const bool rpmReasonSuppressed = reasonSuppressedByOverride(REASON_RPM_TOO_LOW, tick);
  static RpmLowConfirm rpmZeroConfirm;
  const bool rpmZeroHeld = rpmZeroConfirm.update(!rpmReasonSuppressed && RPM <= 0.0f, currentMillis,
                                                 RPM_ZERO_CUT_MS, RPM_ZERO_MAX_WAIT_MS, (uint32_t)dataTimestamps[IDX_RPM]);
  g_rpmTachMasked = rpmDropoutGrace;
  tick.engineFullyStopped = !rpmDropoutGrace && rpmZeroHeld;

  // Engine-restart confirmation: after a confirmed stop, RPM must hold >= MinRPMForField for RPM_RESTART_CONFIRM_MS before
  // the gate releases, so a noise blip or phantom can't re-energize the field. Grace-masked dropouts never arm it.
  if (tick.engineFullyStopped) rpmRestartPending = true;
  // Engine speed turned back on mid-run (IgnoreRPM 1 -> 0) re-confirms the engine the same way a stop does: a
  // phantom present at that moment would otherwise pass the RPM gate with no dwell.
  {
    static bool prevRpmIgnored = false;
    if (prevRpmIgnored && !tick.ignoreRPM) rpmRestartPending = true;
    prevRpmIgnored = tick.ignoreRPM;
  }
  if (!rpmReasonSuppressed && RPM >= (float)MinRPMForField && RPM > 0.0f) {
    if (rpmAboveMinSinceMs == 0) rpmAboveMinSinceMs = currentMillis;
    if (rpmRestartPending && (uint32_t)(currentMillis - rpmAboveMinSinceMs) >= RPM_RESTART_CONFIRM_MS) {
      rpmRestartPending = false;
      queueConsoleMessageF("Engine restart confirmed (RPM=%d held %.1fs)", (int)RPM, RPM_RESTART_CONFIRM_MS / 1000.0f);
    }
  } else {
    rpmAboveMinSinceMs = 0;
  }
  tick.rpmBelowMinimum = (!rpmReasonSuppressed && !rpmDropoutGrace
                          && (RPM < (float)MinRPMForField || rpmRestartPending));

  // Temperature selection
  bool tempFromAlt = (TempSource == 0);
  float tempSelected = tempFromAlt ? AlternatorTemperatureF : temperatureThermistor;

  tick.tempToUseF = tempSelected;
  tick.tempLimitF = TemperatureLimitF;
  tick.tempWarnExcessF = TempWarnExcess;
  tick.tempCritExcessF = TempCritExcess;
  tick.tempSourceIsAlt = tempFromAlt;

  // Mirror to global for legacy displays
  TempToUse = tick.tempToUseF;

  // Battery temperature: one batteryTempF() call per tick. The globals mirror it for CSV2, the alarm
  // block, the CV derate and the DCIR record.
  tick.battTempF = batteryTempF(&tick.battTempSrc);
  battTempActiveF = tick.battTempF;
  battTempActiveSrc = tick.battTempSrc;

  // §2.2 / §2.3 / §2.6: expected = any source but Not fitted (Automatic with nothing reporting is a fault). State 3 (grace,
  // 90 s after boot or a source change) is not "lost" but is not charged on: REASON_BATT_TEMP_WAIT holds the field off.
  {
    static int      lastBattTempSrcSetting = -1;
    static bool     battTempEverSeen = false;
    static uint32_t battTempGraceUntilMs = 90000UL;
    if (battTempSource != lastBattTempSrcSetting) {
      if (lastBattTempSrcSetting != -1) { battTempEverSeen = false; battTempGraceUntilMs = currentMillis + 90000UL; }
      lastBattTempSrcSetting = battTempSource;
    }
    tick.battTempExpected = (battTempSource != 6);
    if (!tick.battTempExpected)                 tick.battTempState = 0;
    else if (isfinite(tick.battTempF))          { tick.battTempState = 1; battTempEverSeen = true; }
    else if (!battTempEverSeen && (int32_t)(battTempGraceUntilMs - currentMillis) > 0) tick.battTempState = 3;
    else                                        tick.battTempState = 2;
    g_battTempState = tick.battTempState;
  }

  // Cold-charge lockout: latched with ColdChargeHysteresisF to stop chatter at the threshold. A
  // missing reading holds the latch (§2.4) — BATT_TEMP_LOST owns that case; the latch clears only on
  // a real reading past the hysteresis, or when the owner declares Not fitted or switches it off.
  {
    static bool coldLatched = false;
    if (coldChargeLockoutEnable && isfinite(tick.battTempF)) {
      if (tick.battTempF < MinChargeTempF) {
        coldLatched = true;
      } else if (tick.battTempF >= MinChargeTempF + ColdChargeHysteresisF) {
        coldLatched = false;
      }
    } else if (!coldChargeLockoutEnable || !tick.battTempExpected) {
      coldLatched = false;
    }
    tick.batteryTooCold = coldLatched;
  }

  // Hot-charge lockout: mirror of the cold latch. The explicit 1..4 source gate stays so a future
  // non-measured source cannot silently start locking charging out on a warm day.
  {
    static bool hotLatched = false;
    if (hotChargeLockoutEnable == 1 && tick.battTempSrc >= 1 && tick.battTempSrc <= 4 && isfinite(tick.battTempF)) {
      if (tick.battTempF > MaxChargeTempF) {
        hotLatched = true;
      } else if (tick.battTempF <= MaxChargeTempF - HotChargeHysteresisF) {
        hotLatched = false;
      }
    } else if (hotChargeLockoutEnable != 1 || !tick.battTempExpected) {
      hotLatched = false;
    }
    tick.batteryTooHot = hotLatched;
  }

  // Battery temperature moved >= 5 F, or it appeared/disappeared -> refresh the CV gain derate, at most
  // once per 30 s. This is the only derate trigger; recomputeCvGains is arithmetic only, so it is safe on
  // the control core.
  {
    static float    lastDerateTempF = NAN;
    static uint8_t  lastDerateClass = 0;
    static uint32_t lastDerateMs = 0;
    uint8_t cls = (tick.battTempSrc >= 1 && tick.battTempSrc <= 4) ? 1 : 0;
    bool moved = isfinite(tick.battTempF)
                 && (isnan(lastDerateTempF) || fabsf(tick.battTempF - lastDerateTempF) >= 5.0f);
    if (battTempDerateEnable && !isnan(CommissionTempF) && (moved || cls != lastDerateClass)
        && (lastDerateMs == 0 || (uint32_t)(currentMillis - lastDerateMs) >= 30000UL)) {  // 0 = never ran: the first trigger is immediate
      lastDerateTempF = tick.battTempF;
      lastDerateClass = cls;
      lastDerateMs = currentMillis;
      recomputeCvGains();
    }
  }

  // Commissioning idle-rest: IN_PROGRESS + live wizard heartbeat + no test driving the field. faCommissionGate (the
  // Disturbances sweep) needs live charging, so it suppresses rest like the other tests.
  {
    // Signed: the web task can stamp the heartbeat after currentMillis was sampled. The -1000 bound keeps a stamp
    // left behind by a closed browser from reading fresh again once its age passes 24.8 days.
    const uint32_t hbMs = lastCommissionHeartbeatMs;
    const int32_t hbAge = (int32_t)(currentMillis - hbMs);
    bool dialogAlive = (hbMs != 0) && hbAge > -1000 && hbAge < (int32_t)COMMISSION_HEARTBEAT_TIMEOUT_MS;
    // Include PENDING request flags: MODE_COMMISSION_IDLE early-returns before fieldCurve_tick/systemID_tick run, so a
    // queued request would never be consumed — a permanent rest deadlock.
    bool anyTestActive = (fieldCurveActive != 0) || (systemIDActive != 0) || (fieldCutActive != 0) ||
                         fieldCurveRequested || systemIDRequested || fieldCutRequested ||
                         (protTestActive != 0) || protTestRequested ||
                         resTestActive || batteryHealthTestActive || cvPlantFitActive || cvStressActive ||
                         (altSweepActive != 0) || altSweepRequested ||
                         (chcActive != 0) || chcRequested ||
                         TuningMode || CVTuningMode || faCommissionGate;
    tick.commissioningResting = (commissionState == 1) && dialogAlive && !anyTestActive;
  }

  // Temperature staleness - check SELECTED source
  uint32_t tempTimestampIdx = tempFromAlt ? IDX_ALTERNATOR_TEMP : IDX_THERMISTOR_TEMP;
  uint32_t tempTimestamp = dataTimestamps[tempTimestampIdx];
  bool tempDataVeryStale = false;

  if (tempTimestamp == 0) {
    if (tick.nowMs > 60000) {
      tempDataVeryStale = true;
    }
  } else if (tempTimestamp > tick.nowMs) {
    // Temp task (Core 0) can stamp dataTimestamps[] a few ms ahead of tick.nowMs (Core 1); the unsigned subtraction
    // would underflow into a false CRITICAL stale cut. An "ahead" stamp means the read is current.
    tempDataVeryStale = false;
  } else {
    uint32_t tempAge = tick.nowMs - tempTimestamp;
    tempDataVeryStale = (tempAge > 20000);
  }

  const bool altTempValueBad = (isnan(tempSelected) || tempSelected < -50.0f || tempSelected > 400.0f);
  const bool altTempAgeStale = tempDataVeryStale;
  if (altTempValueBad) {
    tempDataVeryStale = true;
  }
  // Before the first reading tempSelected is NaN by construction, not a fault: the sensor fault mask
  // gets the same 60 s never-connected grace as the stale rule above, so a page opened at boot shows no banner.
  const bool altTempAwaitingFirst = (tempTimestamp == 0 && tick.nowMs <= 60000);

  // Idle-aware gate: below running speed the temp task stretches its poll (2_functions.ino), so the feed reads "stale"
  // BY DESIGN with nothing to protect. Keyed on engineRunning (§1.1, held 15 s past spin-up); a dead sensor still cuts
  // 15 s after start, even with engine speed ignored.
  if (!tick.engineRunning) {
    tempDataVeryStale = false;
  }

  tick.tempDataVeryStale = tempDataVeryStale;

  // One-shot diagnostics on staleness transitions: read age, last good value, gap vs bad value, and failure types
  // accumulated DURING the gap (the periodic TempDbg line only shows boot totals).
  {
    static bool prevTempStale = false;
    static uint32_t staleOnsetMs = 0;
    if (tempDataVeryStale && !prevTempStale) {
      staleOnsetMs = tick.nowMs;
      uint32_t ageMs = (tempTimestamp == 0) ? tick.nowMs : (tick.nowMs - tempTimestamp);
      const char *why = (tempTimestamp != 0 && ageMs <= 20000) ? "BAD-VALUE" : "READ-GAP";
      // tempLastGoodF/tempLastSuccessMillis are still at their -99F/0 init until the FIRST good read
      // ever. Printing those raw reads as a plausible cold measurement taken a lifetime ago.
      char lastGoodStr[40];
      if (tempLastSuccessMillis == 0) snprintf(lastGoodStr, sizeof(lastGoodStr), "lastGood=never read");
      else snprintf(lastGoodStr, sizeof(lastGoodStr), "lastGood=%.1fF (%.1fs ago)",
                    (float)tempLastGoodF, (tick.nowMs - tempLastSuccessMillis) / 1000.0f);
      queueConsoleMessageF(
        "TEMP STALE TRIP (%s): %s age=%.1fs %s | during gap: conn+%lu enum+%lu crc+%lu req+%lu read+%lu allFF+%lu",
        why, tempFromAlt ? "alt" : "thermistor",
        ageMs / 1000.0f, lastGoodStr,
        (unsigned long)(tempConnectedFailCount - tempFailSnapConn),
        (unsigned long)(tempEnumerateFailCount - tempFailSnapEnum),
        (unsigned long)(tempCrcFailCount - tempFailSnapCrc),
        (unsigned long)(tempRequestFailCount - tempFailSnapReq),
        (unsigned long)(tempReadFailCount - tempFailSnapRead),
        (unsigned long)(tempAllFFCount - tempFailSnapAllFF));
    } else if (!tempDataVeryStale && prevTempStale) {
      // The idle gate clears staleness on RPM alone, so this does NOT prove a read succeeded; say which cleared it.
      bool sane = !isnan(tempSelected) && tempSelected >= -50.0f && tempSelected <= 400.0f;
      if (sane) {
        queueConsoleMessageF("TEMP STALE CLEARED: fresh again after %.1fs, temp=%.1fF",
                             (tick.nowMs - staleOnsetMs) / 1000.0f, tempSelected);
      } else {
        queueConsoleMessageF("TEMP STALE CLEARED after %.1fs by the idle gate — still no valid reading (engine below running speed, nothing to protect)",
                             (tick.nowMs - staleOnsetMs) / 1000.0f);
      }
    }
    prevTempStale = tempDataVeryStale;
  }

  // Current sensor staleness — same freshness system as temperature
  {
    bool curStale = false;
    if (dataTimestamps[IDX_MEASURED_AMPS] == 0) {
      if (tick.nowMs > 30000) curStale = true;
    } else {
      curStale = (tick.nowMs - dataTimestamps[IDX_MEASURED_AMPS]) > 10000;
    }
    tick.currentDataStale = curStale;
  }

  // Die temperature: the field-drive heat ceiling's only input. The stamp moves only on a sample past the warm-up discard
  // and inside the bad-read window, so out of range, stale and missing are one condition. A chip that has never
  // produced a valid sample counts as missing from boot.
  {
    const uint32_t ts = dataTimestamps[IDX_INA_DIE_TEMP];
    const bool fresh = (ts != 0) && ((int32_t)(currentMillis - ts) <= 10000);
    tick.dieTempLost = (currentMillis > 15000UL) && !fresh;
    static bool prevLost = false;
    if (tick.dieTempLost != prevLost) {
      if (tick.dieTempLost) queueConsoleMessageF("Regulator drive temperature (INA228 die) has no valid reading for 10 s (%s) - %s",
                                                 (ts == 0) ? "never read since boot" : "last valid sample too old or out of range",
                                                 reasonSuppressedByOverride(REASON_DIE_TEMP_LOST, tick)
                                                   ? "Run Without Failed Sensors (main battery voltage) or Limp Home is on, so the field keeps running and the field-drive heat limit holds its last value"
                                                   : "the field-drive heat limit has no input, so the field ramps off in every mode, MANUAL included, until it reads again");
      else queueConsoleMessage("Regulator drive temperature (INA228 die) reading again - the field may run again");
      prevLost = tick.dieTempLost;
    }
  }

  // Hard over-current: HardOCDebounceMs on a FRESH sample only — a held value from a dead sensor must not sit
  // on top of the ladder for ever. The debounce state lives here so both ladders read one flag and
  // selectFieldEventReason (called twice per tick) owns no state.
  {
    bool hardOcNow = HardOCEnable && altAmpsFresh() && MeasuredAmps > HardOCTripAmps;
    if (hardOcNow) { if (hardOCStartMs == 0) hardOCStartMs = currentMillis ? currentMillis : 1; }
    else hardOCStartMs = 0;
    tick.hardOverCurrent = hardOcNow && (uint32_t)(currentMillis - hardOCStartMs) >= HardOCDebounceMs;
  }

  // Tach-lie plausibility: tach claims running + loop COMMANDING current + field well above the per-RPM onset floor + zero
  // output, held TACH_LIE_DWELL_MS. Means RPM noise (field-PWM coupling sustains a phantom), a dead alternator, or an open
  // field drive. Both bars are relative to the floor, never flat duty: the knee floor is DEFINED as the most field that
  // still makes ~0 A. MANUAL stays unrestricted. Commissioning sweeps hold high duty at zero output ON PURPOSE (and the knee
  // sweep rewrites the floor table), so active/pending sweeps are exempt. Armed = every term but the evidence.
  bool tachLieArmed;
  {
    bool sweepDrivingField = !testsParkedByTier
                             && ((fieldCurveActive != 0) || (systemIDActive != 0) || (fieldCutActive != 0)
                                 || fieldCurveRequested || systemIDRequested || fieldCutRequested
                                 || fieldCutCcActive || (protTestActive != 0) || protTestCcActive
                                 || (altSweepActive != 0) || altSweepRequested
                                 || (chcActive != 0) || chcRequested);
    // With engine speed ignored the ignition is the "engine on" claim, and this is the only thing that catches a stalled
    // engine with the key on; with IgnitionOverride too, it probes once per lockout tier and releases only by tier expiry.
    // Arm-bar base: with engine speed ignored rpmMinDuty is 0, which would put the bar inside the zero-output band, so the
    // min-duty table's largest row (idle floor) stands in.
    const float tachLieFloor = tick.ignoreRPM ? largestRpmTableMinDuty() : tick.rpmMinDuty;
    // One reading under MinRPMForField must not restart the count, or an occasionally-dipping phantom is never cut. Same
    // rule, timing and stamp as the low-RPM gate uses while armed.
    static RpmLowConfirm tachLieRpmLow;
    const bool tachLieRpmLowConfirmed = tachLieRpmLow.update(RPM < (float)MinRPMForField, currentMillis,
                                                             0, RPM_BELOWMIN_MAX_WAIT_ARMED_MS, rpmUnheldReadMs);
    tachLieArmed = TachLieEnable && chargingEnabledLocal && !tick.manualMode && !tick.fieldResTest
                   && !reasonSuppressedByOverride(REASON_TACH_IMPLAUSIBLE, tick)   // needs the alternator current
                   && !sweepDrivingField
                   && !tick.currentDataStale
                   && setpointLimited > NoOutputAmps
                   && dutyCycle >= tachLieFloor + TACH_LIE_HEADROOM_FRAC * fmaxf(0.0f, ccDutyCeiling() - tachLieFloor);
    bool tachLieNow = tachLieArmed
                      && (tick.ignoreRPM || !tachLieRpmLowConfirmed)
                      && fabsf(MeasuredAmps) < NoOutputAmps;
    if (tachLieNow) {
      if (tachLieSinceMs == 0) tachLieSinceMs = currentMillis;
    } else {
      tachLieSinceMs = 0;
    }
    tick.tachImplausible = (tachLieSinceMs != 0 && (uint32_t)(currentMillis - tachLieSinceMs) >= TACH_LIE_DWELL_MS);
  }

  // RPM gate: holds an already-off field at once, but cutting an energized one takes two low reads, so one glitched read
  // can't drop a charging field (LM2907 slam → ~4.6 s false zero + restart confirm). While tach-lie is armed the held
  // read doesn't count (rpmUnheldReadMs) and the bound is RPM_BELOWMIN_MAX_WAIT_ARMED_MS. After the tach-lie block so
  // both see this pass's arm state.
  {
    static RpmLowConfirm rpmGateConfirm;
    const bool confirmed = rpmGateConfirm.update(tick.rpmBelowMinimum, currentMillis,
                                                 0, tachLieArmed ? RPM_BELOWMIN_MAX_WAIT_ARMED_MS : RPM_BELOWMIN_MAX_WAIT_MS,
                                                 (uint32_t)dataTimestamps[IDX_RPM], rpmUnheldReadMs, tachLieArmed);
    tick.rpmGateActive = confirmed || (tick.rpmBelowMinimum && gpio4IsLow);
  }

  tick.voltagePlausible = isVoltageSensorPlausible();
  tick.voltageDisagreementCritical = isVoltageDisagreementCritical();
  tick.voltageDisagreementWarning = isVoltageDisagreementWarning(
    tick.nowMs, tick.batteryV, tick.ibv,
    tick.voltagePlausible, tick.voltageDisagreementCritical);

  // ── Suppress disagreement check during and after INA OV event ───────────
  // When ALERT cuts the field, ADS1115 (faster) and INA228 diverge as the field collapses; without this VOLT_SPIKE fires
  // on an already-handled cut. 10 s hold: the INA228 averaged value can lag several update cycles during transients.
  bool inaOvSuppressActive = inaOvervoltageLatched || (inaOvervoltageClearedMs > 0 && (tick.nowMs - inaOvervoltageClearedMs) < INA_OV_DISAGREE_SUPPRESS_MS);

  static bool suppressLoggedThisCycle = false;
  if (inaOvSuppressActive) {
    tick.voltageDisagreementWarning = false;
    tick.voltageDisagreementCritical = false;
    if (!suppressLoggedThisCycle) {
      suppressLoggedThisCycle = true;
      queueConsoleMessageF(
        "VOLT_SPIKE check suppressed: INA OV %s | "
        "ADS=%.2fV INA=%.2fV | Suppression holds for %ds after latch clears.",
        inaOvervoltageLatched ? "latch active" : "recently cleared",
        tick.batteryV, tick.ibv,
        INA_OV_DISAGREE_SUPPRESS_MS / 1000);
    }
  } else {
    suppressLoggedThisCycle = false;
  }

  // Tach-lie lockout early release: ONLY a real rev-up (RPM > 1.5× trip RPM) held TACH_LIE_RELEASE_HOLD_MS, and only
  // after TACH_LIE_RELEASE_BLANK_MS (the collapse slams the LM2907; a post-cut spike read as a rev-up). Do NOT add a
  // signal-dropped release: the cut itself drives a PWM-sustained phantom to zero, so it would discard the tier on exactly
  // the fault it exists for. Disabling TachLieEnable clears the lockout (the user's escape hatch); ignoreRPM zeroes the
  // hold timer so a stale stamp can't satisfy it when toggled back.
  if (tachLieLockoutArmed) {
    if (fieldCollapseTime == 0) {
      tachLieLockoutArmed = false;  // lockout expired naturally elsewhere
      tachLieRevUpSinceMs = 0;
    } else if (!TachLieEnable) {
      fieldCollapseTime = 0;
      tachLieLockoutArmed = false;
      tachLieRevUpSinceMs = 0;
      queueConsoleMessageF("Tach-lie lockout cleared: detector disabled by user");
    } else if (tick.ignoreRPM || (uint32_t)(tick.nowMs - fieldCollapseTime) < TACH_LIE_RELEASE_BLANK_MS) {
      tachLieRevUpSinceMs = 0;
    } else {
      if (RPM > tachLieTripRpm * 1.5f) {
        if (tachLieRevUpSinceMs == 0) tachLieRevUpSinceMs = tick.nowMs;
      } else {
        tachLieRevUpSinceMs = 0;
      }
      if (tachLieRevUpSinceMs != 0 && (uint32_t)(tick.nowMs - tachLieRevUpSinceMs) >= TACH_LIE_RELEASE_HOLD_MS) {
        fieldCollapseTime = 0;
        tachLieLockoutArmed = false;
        tachLieRevUpSinceMs = 0;
        queueConsoleMessageF("Tach-lie lockout released early (rev-up held %d ms, RPM=%d)",
                             (int)TACH_LIE_RELEASE_HOLD_MS, (int)RPM);
      }
    }
  }

  tick.inLockout = (fieldCollapseTime > 0 && (tick.nowMs - fieldCollapseTime) < activeCollapseDelay);

  // Current-sensor zero hold (altZeroCaptureService): wants the field at 0% for the measurement.
  // Only where the field would otherwise be ON — with charging disabled, a lockout in force, or an engine-off
  // capture that found the field already off (altZeroHoldField 0) it is already off, the measurement proceeds
  // by itself, and the hold must not re-assert the enable line.
  tick.altZeroMeasuring = (AltZeroCaptureNow == 1) && (altZeroCapPhase >= ALTZERO_PH_RAMP) && altZeroHoldField
                          && tick.chargingEnabled && !tick.inLockout;

  // Thresholds
  tick.bulkVoltage = BulkVoltage;
  tick.alternatorHardShutdownV = AlternatorHardShutdownV;
  tick.testProtectionsEnabled = testProtectionsEnabled;
  tick.inAbsorptionStage = inAbsorptionStage;

  // §1.2 sensor fault mask — real status of every sensor, overrides or not — and the §4.7 steady
  // mask (bit set once its fault bit has been clear continuously for 10 min).
  {
    uint16_t m = 0;
    if ((altTempAgeStale && tick.engineRunning) || (altTempValueBad && !altTempAwaitingFirst)) m |= SF_ALT_TEMP;
    if (((tachLieLockoutArmed || tick.tachImplausible) && !tick.ignoreRPM) || ADS1115Disconnected != 0) m |= SF_ENGINE_SPEED;   // with engine speed ignored a no-output trip is a stall or a dead alternator, not a sensor fault
    if (tick.currentDataStale || g_altAmpsOutOfRange || ADS1115Disconnected != 0) m |= SF_ALT_CURRENT;
    if (tick.ibvStale || INADisconnected != 0) m |= SF_BATT_V_MAIN;
    if (tick.battVStale || ADS1115Disconnected != 0) m |= SF_BATT_V_SECOND;
    // Disagreement is a relationship between two FRESH readings; a held IBV against a live BatteryV is the
    // main sensor's fault (bit 3), not a disagreement.
    if (!tick.ibvStale && !tick.battVStale && voltageInPlausibleWindow(IBV) && voltageInPlausibleWindow(BatteryV)
        && (tick.voltageDisagreementCritical || tick.voltageDisagreementWarning)) m |= SF_BATT_V_DISAGREE;
    if (tick.battTempState == 2) m |= SF_BATT_TEMP;
    if (ADS1115Disconnected != 0) m |= SF_ADS1115;
    if (INADisconnected != 0) m |= SF_INA228;
    if (tick.dieTempLost) m |= SF_DRIVE_TEMP;
    g_sensorFaultMask = m;

    // A DS18B20 that died mid-run holds its last value, so at rest bit 0 clears on the engine gate alone:
    // "clear" for the steady prompt and the automatic exit means the selected source has actually stamped a
    // reading within the at-rest poll window.
    const uint32_t altTs = dataTimestamps[tempFromAlt ? IDX_ALTERNATOR_TEMP : IDX_THERMISTOR_TEMP];
    const bool altTempReadRecently = (altTs != 0) && ((uint32_t)(currentMillis - altTs) <= 90000UL);

    static uint32_t clearSinceMs[10] = { 0 };
    uint16_t steady = 0;
    for (uint8_t b = 0; b < 10; b++) {
      bool clear = !(m & (1u << b)) && (b != 0 || altTempReadRecently);
      if (!clear) {
        clearSinceMs[b] = 0;
      } else {
        if (clearSinceMs[b] == 0) clearSinceMs[b] = currentMillis ? currentMillis : 1;
        if ((uint32_t)(currentMillis - clearSinceMs[b]) >= 600000UL) steady |= (1u << b);
      }
    }
    g_sensorSteadyMask = steady;

    // ===== §11 required-fault count =====
    // Counted per SENSOR, not per bit (a dead chip is every reading it carried). Debounced conditions only, so one glitch
    // can't enter a tier or fire the multi-fault cut. Owner overrides and Not fitted never count.
    uint16_t f = 0;
    // Sim mode runs on fake readings: an absent chip on a bench board is not a missing input
    const bool inaGone = (hardwarePresent == 1) && INADisconnected != 0;
    const bool adsGone = (hardwarePresent == 1) && ADS1115Disconnected != 0;
    if ((altTempAgeStale && tick.engineRunning) || (altTempValueBad && !altTempAwaitingFirst)) f |= SF_ALT_TEMP;
    if ((tachLieLockoutArmed || tick.tachImplausible) && !tick.ignoreRPM) f |= SF_ENGINE_SPEED;
    if (tick.currentDataStale) f |= SF_ALT_CURRENT;
    if (tick.ibvStale || inaGone) f |= SF_BATT_V_MAIN;
    if (tick.battVStale) f |= SF_BATT_V_SECOND;
    if (tick.battTempState == 2) f |= SF_BATT_TEMP;
    if (tick.dieTempLost || inaGone) f |= SF_DRIVE_TEMP;   // a dead chip is every reading it carried, like the ADS1115 line below
    if (adsGone) f |= SF_ENGINE_SPEED | SF_ALT_CURRENT | SF_BATT_V_SECOND | ((TempSource == 1) ? SF_ALT_TEMP : 0);
    if ((m & SF_BATT_V_DISAGREE) && !(f & (SF_BATT_V_MAIN | SF_BATT_V_SECOND))) f |= SF_BATT_V_DISAGREE;
    f &= ~sensorOverriddenBits();
    f &= (SF_ALT_TEMP | SF_ENGINE_SPEED | SF_ALT_CURRENT | SF_BATT_V_MAIN | SF_BATT_V_SECOND | SF_BATT_V_DISAGREE | SF_BATT_TEMP | SF_DRIVE_TEMP);
    g_reqFaultBits = f;
    g_reqFaultCount = (uint8_t)__builtin_popcount((unsigned)f);

    // Two or more: cut and hold (REASON_MULTI_SENSOR_FAULT next tick); release 10 s after the count is back to <= 1
    // so a harness flapping between one and two faults does not cycle the field.
    static uint32_t multiClearSinceMs = 0;
    if (g_reqFaultCount >= 2) {
      if (!s_multiSensorFault) {
        char names[200];
        sensorFaultNames(f, names, sizeof(names));
        queueConsoleMessageF("Sensor faults: %s - two or more required sensors are gone, charging stopped", names);
      }
      s_multiSensorFault = true;
      multiClearSinceMs = 0;
    } else if (s_multiSensorFault) {
      if (multiClearSinceMs == 0) multiClearSinceMs = currentMillis ? currentMillis : 1;
      if ((uint32_t)(currentMillis - multiClearSinceMs) >= 10000UL) {
        s_multiSensorFault = false;
        queueConsoleMessage("Multi-sensor fault cleared: at most one required sensor is faulted now");
      }
    }
  }

  // §11.6 battery current: an OPTIONAL sensor the owner declared present, so its loss never cuts or counts toward
  // g_reqFaultCount. Lost = the stage-current source has stamped since boot (or 90 s passed) and nothing fresh for 10 s.
  {
    static uint32_t battCurStaleSinceMs = 0;
    uint8_t st = 0;
    const bool netSrc = !HAS_BATT_SHUNT && BattMonSrc != 0;
    if (HAS_BATT_SHUNT || netSrc) {
      const uint32_t ts = dataTimestamps[!netSrc ? IDX_BCUR : (BattMonSrc == 3) ? IDX_VICTRON_CURRENT : IDX_N2K_BATT];
      const bool everSeen = (ts != 0) || (currentMillis > 90000UL);
      const bool fresh = netSrc ? !isnan(netMonCurrentA()) : bcurFresh();
      if (fresh || !everSeen) {
        battCurStaleSinceMs = 0;
        st = 1;
      } else {
        if (battCurStaleSinceMs == 0) battCurStaleSinceMs = currentMillis ? currentMillis : 1;
        st = ((uint32_t)(currentMillis - battCurStaleSinceMs) >= 10000UL) ? 2 : 1;
      }
    } else {
      battCurStaleSinceMs = 0;
    }
    if (st != g_battCurState) {
      if (st == 2 && netSrc) queueConsoleMessageF("Battery current lost: the %s battery monitor has not reported for 10 s - the tail-current absorption exit and the current rebulk wait for it; charging continues on voltage",
                                                  (BattMonSrc == 3) ? "VE.Direct" : "NMEA 2000");
      else if (st == 2) queueConsoleMessageF("Battery current lost: the INA228 shunt has not reported for 10 s - %sthe tail-current absorption exit, the current rebulk and the battery charge limit wait for it; charging continues on voltage",
                                             (BattMonSrc == 0) ? "state of charge, " : "");
      else if (g_battCurState == 2) queueConsoleMessage("Battery current reading again");
    }
    g_battCurState = st;
  }

  return tick;
}


static bool overheatHistDirty = false;
static uint64_t overheatSavedSafeMs = 0;   // totalSafeMs as last written to NVS

void resetLearningTableToDefaults() {
  overheatHistDirty = true;   // the cleared history below must reach NVS too
  for (int i = 0; i < RPM_TABLE_SIZE; i++) {
    rpmTableRPMPoints[i] = defaultRPMValues[i];
    rpmCapCurrentTable[i] = defaultCapCurrentValues[i];
    rpmCapPowerTable[i] = defaultCapPowerW(i, true);
    rpmMinDutyTable[i] = defaultMinDutyValues[i];
    lastOverheatTime[i] = 0;
    overheatCount[i] = 0;
    cumulativeNoOverheatTime[i] = 0;
  }
  overheatPenaltyEndMs = 0;
  overheatingPenaltyAmps = 0;
  totalSafeMs = 0;
  // Reset diagnostics
  totalLearningEvents = 0;
  totalOverheats = 0;
  timeSinceLastOverheat = 0;   // stops counting at totalOverheats == 0, so a stale value would stay frozen (saved as OvhtSinceMs)
  // Persist defaults for BOTH charge-rate modes WITHOUT touching HiLow: this also runs as the boot auto-reset, and changing
  // the mode would kick a Low-mode user back to Normal. capLimitMode lives only in NK_capLimitMode for the same reason.
  {
    float loDefaults[RPM_TABLE_SIZE], hiDefaultsPwr[RPM_TABLE_SIZE], loDefaultsPwr[RPM_TABLE_SIZE];
    for (int i = 0; i < RPM_TABLE_SIZE; i++) {
      loDefaults[i] = defaultCapCurrentValues[i] * LOW_MODE_CAP_FRACTION;
      hiDefaultsPwr[i] = defaultCapPowerW(i, true);
      loDefaultsPwr[i] = defaultCapPowerW(i, false);
    }
    nvs_handle_t nvs_h;
    if (nvs_open("learning", NVS_READWRITE, &nvs_h) == ESP_OK) {
      // From the defaults, not the live tables: the /get reset runs on the web task while the learner writes Min% on Core 1
      nvs_set_blob(nvs_h, "rpmPoints", defaultRPMValues, sizeof(defaultRPMValues));
      nvs_set_blob(nvs_h, "capTable", defaultCapCurrentValues, sizeof(defaultCapCurrentValues));   // Normal/High
      nvs_set_blob(nvs_h, "capPowerTable", hiDefaultsPwr, sizeof(hiDefaultsPwr));                  // Normal/High
      nvs_set_blob(nvs_h, "capTableLo", loDefaults, sizeof(loDefaults));                            // Low
      nvs_set_blob(nvs_h, "capPowerTableLo", loDefaultsPwr, sizeof(loDefaultsPwr));                 // Low
      nvs_set_blob(nvs_h, "minDutyTable", defaultMinDutyValues, sizeof(defaultMinDutyValues));
      nvs_commit(nvs_h);
      nvs_close(nvs_h);
    }
  }
  // Refresh the live arrays for the user's ACTUAL mode (Normal or Low) — not a forced Normal.
  loadCapTablesForMode(HiLow);
  queueConsoleMessage("Learning: All tables reset to factory defaults");
}

// After a request rewrote rpmTableRPMPoints (prevPts = breakpoints before it): a moved breakpoint leaves the overheat
// history beside it and every learned Min% floor keyed to the wrong RPM, so clear the moved bin and the one below, and
// wipe the knee tracker for re-measurement. True when any moved.
bool clearMovedRpmBins(const int *prevPts, const int *nowPts) {
  bool moved = false;
  for (int i = 0; i < RPM_TABLE_SIZE; i++) {
    if (nowPts[i] == prevPts[i]) continue;
    moved = true;
    for (int j = (i > 0) ? i - 1 : i; j <= i; j++) {
      overheatCount[j] = 0;
      lastOverheatTime[j] = 0;
      cumulativeNoOverheatTime[j] = 0;
      learningUpCount[j] = 0;
    }
    queueConsoleMessageF("Learning: RPM bin %d changed - cleared affected history", i);
  }
  if (moved) overheatHistDirty = true;
  return moved;
}
void rpmBreakpointsMoved(const int *prevPts) {
  if (!clearMovedRpmBins(prevPts, rpmTableRPMPoints)) return;
  kneeLearnResetDefaults();   // clear floors/knees, unfreeze, column back to the flat baseline
  commissionClearStage(7);    // Keep-alive floor + Field decay stage now stale (demotes a commissioned device to in-progress)
  queueConsoleMessage("Learning: RPM breakpoints changed — keep-alive floors cleared, re-run the Keep-Alive Floor step");
}
void loadLearningTableFromNVS() {
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("learning", NVS_READONLY, &nvs_handle);
  if (err != ESP_OK) {
    queueConsoleMessage("Learning: No saved table, using defaults");
    resetLearningTableToDefaults();
    return;
  }

  bool allValid = true;
  size_t required_size;

  // Load RPM breakpoints
  required_size = sizeof(rpmTableRPMPoints);
  err = nvs_get_blob(nvs_handle, "rpmPoints", rpmTableRPMPoints, &required_size);
  if (err != ESP_OK || required_size != sizeof(rpmTableRPMPoints)) {
    allValid = false;
  }

  // Load cap current table — key depends on HiLow mode (already loaded from LittleFS before this call)
  {
    const char *capKey = (HiLow == 1) ? "capTable" : "capTableLo";
    const char *capPwrKey = (HiLow == 1) ? "capPowerTable" : "capPowerTableLo";
    required_size = sizeof(rpmCapCurrentTable);
    err = nvs_get_blob(nvs_handle, capKey, rpmCapCurrentTable, &required_size);
    if (err != ESP_OK || required_size != sizeof(rpmCapCurrentTable)) {
      for (int i = 0; i < RPM_TABLE_SIZE; i++)
        rpmCapCurrentTable[i] = (HiLow == 1) ? defaultCapCurrentValues[i] : defaultCapCurrentValues[i] * LOW_MODE_CAP_FRACTION;
    }
    required_size = sizeof(rpmCapPowerTable);
    err = nvs_get_blob(nvs_handle, capPwrKey, rpmCapPowerTable, &required_size);
    if (err != ESP_OK || required_size != sizeof(rpmCapPowerTable)) {
      for (int i = 0; i < RPM_TABLE_SIZE; i++) rpmCapPowerTable[i] = defaultCapPowerW(i, HiLow == 1);
    }
  }

  required_size = sizeof(rpmMinDutyTable);
  err = nvs_get_blob(nvs_handle, "minDutyTable", rpmMinDutyTable, &required_size);
  if (err != ESP_OK || required_size != sizeof(rpmMinDutyTable)) {
    // Not critical - just use defaults
    for (int i = 0; i < RPM_TABLE_SIZE; i++) {
      rpmMinDutyTable[i] = defaultMinDutyValues[i];
    }
  }

  // If core data invalid, reset everything
  if (!allValid) {
    queueConsoleMessage("Learning: Core data invalid, resetting");
    resetLearningTableToDefaults();
    nvs_close(nvs_handle);
    return;
  }

  bool historicalDataValid = true;

  required_size = sizeof(overheatCount);
  err = nvs_get_blob(nvs_handle, "overheatCount", overheatCount, &required_size);
  if (err != ESP_OK || required_size != sizeof(overheatCount)) {
    memset(overheatCount, 0, sizeof(overheatCount));
    historicalDataValid = false;
  }

  required_size = sizeof(lastOverheatTime);
  err = nvs_get_blob(nvs_handle, "lastOverheat", lastOverheatTime, &required_size);
  if (err != ESP_OK || required_size != sizeof(lastOverheatTime)) {
    memset(lastOverheatTime, 0, sizeof(lastOverheatTime));
    historicalDataValid = false;
  }

  required_size = sizeof(cumulativeNoOverheatTime);
  err = nvs_get_blob(nvs_handle, "cumulativeTime", cumulativeNoOverheatTime, &required_size);
  if (err != ESP_OK || required_size != sizeof(cumulativeNoOverheatTime)) {
    memset(cumulativeNoOverheatTime, 0, sizeof(cumulativeNoOverheatTime));
    historicalDataValid = false;
  }

  // Load diagnostic counters
  uint32_t temp_uint32;
  if (nvs_get_u32(nvs_handle, "totalEvents", &temp_uint32) == ESP_OK) {
    totalLearningEvents = temp_uint32;
  } else {
    totalLearningEvents = 0;
  }

  if (nvs_get_u32(nvs_handle, "totalOverheats", &temp_uint32) == ESP_OK) {
    totalOverheats = temp_uint32;
  } else {
    totalOverheats = 0;
  }

  required_size = sizeof(totalSafeMs);
  err = nvs_get_blob(nvs_handle, "totalSafeMs", &totalSafeMs, &required_size);
  if (err != ESP_OK || required_size != sizeof(totalSafeMs)) {
    totalSafeMs = 0;
  }
  overheatSavedSafeMs = totalSafeMs;

  if (!historicalDataValid) {
    totalOverheats = 0;
    totalLearningEvents = 0;
    queueConsoleMessage("Learning: Historical data invalid, counters reset");
  }

  nvs_close(nvs_handle);
  queueConsoleMessage("Learning: Table loaded from NVS");
}

// HiLow=1 → Normal ("capTable"), 0 → Low Charge Rate ("capTableLo"); defaults if nothing saved for that mode.
void loadCapTablesForMode(int mode) {
  // A capBothSave still draining to NVS: serve the staged set so the switch sees the new tables, not
  // whichever blobs have landed so far.
  bool served = false;
  portENTER_CRITICAL(&capBothMux);
  if (capBothPersistStep != 0) {
    memcpy(rpmCapCurrentTable, (mode == 1) ? capBothStage.hiA : capBothStage.loA, sizeof(rpmCapCurrentTable));
    memcpy(rpmCapPowerTable,   (mode == 1) ? capBothStage.hiW : capBothStage.loW, sizeof(rpmCapPowerTable));
    served = true;
  }
  portEXIT_CRITICAL(&capBothMux);
  if (served) return;
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("learning", NVS_READONLY, &nvs_handle);
  if (err != ESP_OK) {
    for (int i = 0; i < RPM_TABLE_SIZE; i++) {
      rpmCapCurrentTable[i] = (mode == 1) ? defaultCapCurrentValues[i] : defaultCapCurrentValues[i] * LOW_MODE_CAP_FRACTION;
      rpmCapPowerTable[i] = defaultCapPowerW(i, mode == 1);
    }
    return;
  }
  const char *capKey = (mode == 1) ? "capTable" : "capTableLo";
  const char *capPwrKey = (mode == 1) ? "capPowerTable" : "capPowerTableLo";

  size_t sz = sizeof(rpmCapCurrentTable);
  err = nvs_get_blob(nvs_handle, capKey, rpmCapCurrentTable, &sz);
  if (err != ESP_OK || sz != sizeof(rpmCapCurrentTable)) {
    for (int i = 0; i < RPM_TABLE_SIZE; i++)
      rpmCapCurrentTable[i] = (mode == 1) ? defaultCapCurrentValues[i] : defaultCapCurrentValues[i] * LOW_MODE_CAP_FRACTION;
  }
  sz = sizeof(rpmCapPowerTable);
  err = nvs_get_blob(nvs_handle, capPwrKey, rpmCapPowerTable, &sz);
  if (err != ESP_OK || sz != sizeof(rpmCapPowerTable)) {
    for (int i = 0; i < RPM_TABLE_SIZE; i++) rpmCapPowerTable[i] = defaultCapPowerW(i, mode == 1);
  }
  nvs_close(nvs_handle);
}

// ── Switch panel inputs (Cable 3 / RJ3) ───────────────────────────────────────────────────────
// GPIO39 asks for the High charge rate, GPIO40 for Force Maintain Mode. Clean select, never a blend: PhysicalPanelOverride
// 0 = the app's stored choice is the mode (wires only report); 1 = the wires alone. HiLowUserSel / MaintainModeUserSel
// keep the app's choice either way, so toggling the override restores it.

// Effective charge-rate mode. Panel sense: switch ON (wire energized, pin LOW) = High (HiLow 1),
// switch off = Low (HiLow 0).
int resolveChargeRateMode() {
  if (PhysicalPanelOverride == 0) return (HiLowUserSel == 0) ? 0 : 1;
  return panelHiRateInput ? 1 : 0;
}

// Effective Force Maintain Mode — same select plus the /get handler's physical gates (no battery-current sensor; Maintain
// and Target Voltage are exclusive). A panel Maintain yields to Target Voltage rather than clearing it (an NVS write).
int resolveMaintainMode() {
  bool on = (PhysicalPanelOverride == 0) ? (MaintainModeUserSel == 1) : panelForceFloatInput;
  if (!BatteryShuntPresent) on = false;
  if (TargetVoltageMode == 1) on = false;
  return on ? 1 : 0;
}

// Single application path for a charge-rate change (/get?HiLow, override handler, panel service). True when the live
// mode moved. Never touches NVS — persistence belongs to whoever owns the user's stored choice.
bool applyChargeRateMode(int newMode) {
  if (newMode == HiLow) return false;
  HiLow = newMode;
  loadCapTablesForMode(HiLow);  // swap active cap tables to match new mode
  // Do NOT deactivate the thermal loop here: the penalty already clamps to the LIVE capCurrent every tick. Setting
  // tempPIDActive=false clears the slope buffer (60s warmup, setpoint drops to limit-20) and reseeds the penalty from
  // P-only. A mode switch must be bumpless for the thermal loop.
  stateRevision++;  // force immediate CSVData echo of new table values
  if (HiLow == 0) {
    // Switching to Low drops the ceiling: arm the glide from the present ceiling so the loop ramps down instead of
    // stepping (a step false-trips iExcess). Up-switches rise freely.
    modeCapSlew = uTargetAmps;
    modeCapSlewActive = true;
  }
  return true;
}

// Reads both Cable 3 optos, then re-derives the two effective modes. Debounced because each charge-rate edge swaps the
// cap tables out of NVS and can re-enter the stage machine. Reads and console lines run whether or not the panel has
// authority, so an installer can prove the wiring first.
void servicePanelSwitchInputs() {
  const uint32_t PANEL_DEBOUNCE_MS = 50;
  static bool primed = false;
  static bool rawHiPrev = false;
  static bool rawFloatPrev = false;
  static uint32_t hiChangeMs = 0;
  static uint32_t floatChangeMs = 0;
  const uint32_t now = millis();

  const bool rawHi = !digitalRead(panelHiRatePin);      // inverting opto: voltage at the connector reads LOW
  const bool rawFloat = !digitalRead(panelForceFloatPin);

  // First pass adopts the pins as-is: otherwise a panel already switched on reads as released for 50ms, which with the
  // override on is a real mode change undone a tick later.
  if (!primed) {
    primed = true;
    rawHiPrev = panelHiRateInput = rawHi;
    rawFloatPrev = panelForceFloatInput = rawFloat;
  }

  if (rawHi != rawHiPrev) {
    rawHiPrev = rawHi;
    hiChangeMs = now;
  } else if (rawHi != panelHiRateInput && (uint32_t)(now - hiChangeMs) >= PANEL_DEBOUNCE_MS) {
    panelHiRateInput = rawHi;
    queueConsoleMessageF("Switch panel: charge rate wire %s", rawHi ? "energized (asks for High)" : "open (asks for Low)");
  }
  if (rawFloat != rawFloatPrev) {
    rawFloatPrev = rawFloat;
    floatChangeMs = now;
  } else if (rawFloat != panelForceFloatInput && (uint32_t)(now - floatChangeMs) >= PANEL_DEBOUNCE_MS) {
    panelForceFloatInput = rawFloat;
    queueConsoleMessageF("Switch panel: force float wire %s", rawFloat ? "energized" : "open");
  }

  // CSV3 (the only channel carrying HiLow / MaintainMode) goes out on settingsDirty or a 60 s fallback, so this path
  // must set it too.
  if (applyChargeRateMode(resolveChargeRateMode())) {
    settingsDirty = true;
    queueConsoleMessageF("Charge rate mode: switched to %s", HiLow == 1 ? "Normal" : "Low");
  }
  const int wantMaintain = resolveMaintainMode();
  if (wantMaintain != MaintainMode) {
    MaintainMode = wantMaintain;  // the control loop edge-detects this global; no NVS write on this path
    settingsDirty = true;
    queueConsoleMessageF("MaintainMode mode %s", MaintainMode ? "enabled" : "disabled");
  }
}

// Immediate save of all user-editable columns (Save / Reset buttons)
void saveUserTableEdits() {
  const bool announce = tableSaveAnnounce;
  tableSaveAnnounce = false;
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("learning", NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK) {
    queueConsoleMessageF("Learning: Failed to open NVS (err=%d)", (int)err);
    return;
  }

  bool success = true;

  // From copies: a /get table edit on the web task can land while this loop-side save writes (see saveHistoricalDataImmediate)
  int pts[RPM_TABLE_SIZE];
  float capA[RPM_TABLE_SIZE], capW[RPM_TABLE_SIZE];
  memcpy(pts, rpmTableRPMPoints, sizeof(pts));
  memcpy(capA, rpmCapCurrentTable, sizeof(capA));
  memcpy(capW, rpmCapPowerTable, sizeof(capW));

  err = nvs_set_blob(nvs_handle, "rpmPoints", pts, sizeof(pts));
  if (err != ESP_OK) success = false;

  {
    const char *capKey = (HiLow == 1) ? "capTable" : "capTableLo";
    const char *capPwrKey = (HiLow == 1) ? "capPowerTable" : "capPowerTableLo";
    err = nvs_set_blob(nvs_handle, capKey, capA, sizeof(capA));
    if (err != ESP_OK) success = false;
    err = nvs_set_blob(nvs_handle, capPwrKey, capW, sizeof(capW));
    if (err != ESP_OK) success = false;
  }

  if (!kneePutStateBlobs(nvs_handle)) success = false;   // the Keep-Alive column plus the knee state it is rebuilt from

  err = nvs_commit(nvs_handle);
  if (err != ESP_OK) {
    queueConsoleMessageF("Learning: NVS commit failed (err=%d)", (int)err);
    success = false;
  }

  nvs_close(nvs_handle);

  if (!success) {
    queueConsoleMessage("Learning: Save completed with errors");
  } else if (announce) {
    queueConsoleMessage("Learning: Table saved to NVS");
  }
}

// Pre-commissioning Charge Rate Limits handler: stage both cap tables (Low + High) plus the live RPM breakpoints for
// capBothPersistService(). Separate from saveUserTableEdits (active mode only) so the wizard seeds both without a live
// mode toggle. ~200 B copy under the spinlock on the network task. A re-stage mid-drain restarts from blob 1, so the set that lands is always the latest.
void capBothStageBegin(const float *hiA, const float *hiW, const float *loA, const float *loW) {
  portENTER_CRITICAL(&capBothMux);
  memcpy(capBothStage.pts, rpmTableRPMPoints, sizeof(capBothStage.pts));
  memcpy(capBothStage.hiA, hiA, sizeof(capBothStage.hiA));
  memcpy(capBothStage.hiW, hiW, sizeof(capBothStage.hiW));
  memcpy(capBothStage.loA, loA, sizeof(capBothStage.loA));
  memcpy(capBothStage.loW, loW, sizeof(capBothStage.loW));
  capBothStageGen = capBothStageGen + 1;
  capBothPersistFail = false;
  capBothPersistStep = 1;
  portEXIT_CRITICAL(&capBothMux);
}

// Retire one staged blob per loop() pass: copy under the spinlock, write unlocked, advance only if no re-stage happened
// during the write. A refused write stops the sequence and reports FAILED on /capSaveState; tables stay live in RAM.
void capBothPersistService() {
  uint8_t s = capBothPersistStep;
  if (s == 0) return;
  const char *key = nullptr;
  size_t len = 0;
  union { int pts[RPM_TABLE_SIZE]; float f[RPM_TABLE_SIZE]; } local;
  portENTER_CRITICAL(&capBothMux);
  uint8_t gen = capBothStageGen;
  switch (s) {
    case 1: key = "rpmPoints";       memcpy(local.pts, capBothStage.pts, sizeof(local.pts)); len = sizeof(local.pts); break;
    case 2: key = "capTable";        memcpy(local.f, capBothStage.hiA, sizeof(local.f));     len = sizeof(local.f);   break;
    case 3: key = "capPowerTable";   memcpy(local.f, capBothStage.hiW, sizeof(local.f));     len = sizeof(local.f);   break;
    case 4: key = "capTableLo";      memcpy(local.f, capBothStage.loA, sizeof(local.f));     len = sizeof(local.f);   break;
    case 5: key = "capPowerTableLo"; memcpy(local.f, capBothStage.loW, sizeof(local.f));     len = sizeof(local.f);   break;
    default: capBothPersistStep = 0; break;
  }
  portEXIT_CRITICAL(&capBothMux);
  if (!key) return;

  nvs_handle_t h;
  esp_err_t e = nvs_open("learning", NVS_READWRITE, &h);
  if (e == ESP_OK) {
    e = nvs_set_blob(h, key, &local, len);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
  }
  if (e != ESP_OK) {
    portENTER_CRITICAL(&capBothMux);
    if (capBothStageGen == gen) { capBothPersistStep = 0; capBothPersistFail = true; }
    portEXIT_CRITICAL(&capBothMux);
    queueConsoleMessageF("Learning: Low+High cap table save FAILED at %s (err=%d) - tables are live but not stored", key, (int)e);
    return;
  }
  bool done = false;
  portENTER_CRITICAL(&capBothMux);
  if (capBothStageGen == gen) {   // no re-stage during the write → advance
    capBothPersistStep = (s < 5) ? (s + 1) : 0;
    done = (s == 5);
  }
  portEXIT_CRITICAL(&capBothMux);
  if (done) queueConsoleMessage("Learning: Low+High cap tables saved");
}

// Immediate save of historical data; never call directly from a field-on control tick.
void saveHistoricalDataImmediate() {
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("learning", NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK) {
    queueConsoleMessageF("Learning: Failed to open NVS (%d)", (int)err);
    return;
  }

  bool success = true;

  // NVS checksums a blob from the source buffer, then reads that buffer again to write it. This save also runs on Core 0
  // (demo entry, /get, import) while the control loop adds safe time every pass, and a value changed in between was stored
  // with a bad checksum that the next read deletes (cumulativeTime lost, bench 2026-10-28). The blobs go out from copies.
  int oc[RPM_TABLE_SIZE];
  unsigned long lo[RPM_TABLE_SIZE], ct[RPM_TABLE_SIZE];
  memcpy(oc, overheatCount, sizeof(oc));
  memcpy(lo, lastOverheatTime, sizeof(lo));
  memcpy(ct, cumulativeNoOverheatTime, sizeof(ct));
  const uint64_t safeMs = totalSafeMs;

  err = nvs_set_blob(nvs_handle, "overheatCount", oc, sizeof(oc));
  if (err != ESP_OK) success = false;

  err = nvs_set_blob(nvs_handle, "lastOverheat", lo, sizeof(lo));
  if (err != ESP_OK) success = false;

  err = nvs_set_blob(nvs_handle, "cumulativeTime", ct, sizeof(ct));
  if (err != ESP_OK) success = false;

  err = nvs_set_u32(nvs_handle, "totalEvents", (uint32_t)totalLearningEvents);
  if (err != ESP_OK) success = false;

  err = nvs_set_u32(nvs_handle, "totalOverheats", (uint32_t)totalOverheats);
  if (err != ESP_OK) success = false;

  err = nvs_set_blob(nvs_handle, "totalSafeMs", &safeMs, sizeof(safeMs));
  if (err != ESP_OK) success = false;

  err = nvs_commit(nvs_handle);
  if (err != ESP_OK) {
    success = false;
  }

  nvs_close(nvs_handle);

  if (!success) {
    queueConsoleMessage("Learning: Historical save completed with errors");
  } else {
    overheatSavedSafeMs = safeMs;
  }
}

void overheatHistFlush() {
  if (!overheatHistDirty) return;
  overheatHistDirty = false;
  saveHistoricalDataImmediate();
}

// Safe time accrues on every control tick, so it cannot use the dirty flag above (that would commit
// every pass while the field is cut). It rides saveNVSDataFull() instead: field-off edge, shutdown, restart.
void overheatSafeTimeFlush() {
  if (totalSafeMs == overheatSavedSafeMs) return;
  saveHistoricalDataImmediate();
}

void updateRPMBucketHistory(uint32_t nowMs) {
  static uint32_t lastHistoryMs = 0;
  if (demoSeal) { lastHistoryMs = 0; return; }   // demo mode: the overheat table shown is the demo's (9_demo.ino)
  uint32_t dtMs = (lastHistoryMs == 0) ? 0 : (uint32_t)(nowMs - lastHistoryMs);
  if (dtMs > 500) dtMs = 500;
  lastHistoryMs = nowMs;

  if (isnan(TempToUse) || TempToUse < -50.0f || TempToUse > 400.0f) return;
  if (dtMs == 0) return;
  if (IgnoreRPM) return;   // no trustworthy bucket to charge the history to (§4.8)
  if (IgnoreTemperature || LimpHome) return;   // an ignored temperature is neither an overheat nor safe time

  int bucket = currentRPMTableIndex;
  if (bucket < 0 || bucket >= RPM_TABLE_SIZE) return;

  static bool inOverheat = false;

  if (!inOverheat) {
    if (TempToUse >= TemperatureLimitF) {
      inOverheat = true;
      overheatCount[bucket]++;
      lastOverheatTime[bucket] = (unsigned long)getCurrentTimestamp();  // epoch s (persisted, so not millis())
      totalOverheats++;
      timeSinceLastOverheat = 0;
      overheatHistDirty = true;  // NVS write deferred to overheatHistFlush() at field-off settle — never on a field-on tick
    } else {
      cumulativeNoOverheatTime[bucket] += dtMs;
      totalSafeMs += (uint64_t)dtMs;
      totalSafeHours = (float)(totalSafeMs / 3600000ULL);
      // Stays pinned at 0 until at least one overheat has ever been recorded, so the display
      // reads "never overheated" rather than "0.35 hr since an overheat that never happened".
      if (totalOverheats > 0) timeSinceLastOverheat += dtMs;
    }
  } else {
    if (TempToUse < (TemperatureLimitF - 2.0f)) {
      inOverheat = false;
    }
  }
}

void clearOverheatHistoryAction() {
  for (int i = 0; i < RPM_TABLE_SIZE; i++) {
    overheatCount[i] = 0;
    lastOverheatTime[i] = 0;
    cumulativeNoOverheatTime[i] = 0;
  }
  totalOverheats = 0;
  totalSafeMs = 0;
  totalSafeHours = 0.0f;
  timeSinceLastOverheat = 0;
  saveHistoricalDataImmediate();
  queueConsoleMessage("Overheat History: All history and safe time cleared");
}
// ==================== RPM TABLE LOOKUPS ====================
// findRPMSegment() is the single source of truth for boundary conditions; every lookup, the index tracker and bucket
// history go through it.

int findRPMSegment(float rpm) {
  if (rpm < rpmTableRPMPoints[0]) return -1;
  if (rpm >= rpmTableRPMPoints[RPM_TABLE_SIZE - 1]) return RPM_TABLE_SIZE - 1;
  for (int i = 0; i < RPM_TABLE_SIZE - 1; i++) {
    if (rpm >= rpmTableRPMPoints[i] && rpm < rpmTableRPMPoints[i + 1]) return i;
  }
  return RPM_TABLE_SIZE - 1;  // Fallback — should never reach
}

float interpolateRPMTable(float rpm, const float *table) {
  int seg = findRPMSegment(rpm);
  if (seg < 0) return table[0];
  if (seg >= RPM_TABLE_SIZE - 1) return table[RPM_TABLE_SIZE - 1];
  float rpm1 = (float)rpmTableRPMPoints[seg];
  float rpm2 = (float)rpmTableRPMPoints[seg + 1];
  float fraction = (rpm - rpm1) / (rpm2 - rpm1);
  return table[seg] + fraction * (table[seg + 1] - table[seg]);
}

/**
 * Clamps to 0 below range so the first row is highlighted. Call once per tick before updateRPMBucketHistory().
 */
void updateCurrentRPMTableIndex(float rpm) {
  int seg = findRPMSegment(rpm);
  currentRPMTableIndex = (seg < 0) ? 0 : seg;
}

float getMinimumFieldForRPM(float rpm) {
  float floorV = interpolateRPMTable(rpm, rpmMinDutyTable);
  // A zero floor means "no minimum field" (above the commissioned RPM ceiling, or bin 0): keep it a hard zero so the
  // field can shut fully off there; never temp-correct it.
  if (floorV <= 0.0f) return 0.0f;
  // Copper temp correction, only while the learner owns the table. Only the RESISTIVE part of the knee (knee − kneeFitA)
  // scales with winding resistance (~0.218 %/degF); stored floors are referenced to kneeTempRefF. knee = floor+margin.
  // kneeFitA = 0 (no commissioning fit) scales the whole knee.
  const float tAlt = (IgnoreTemperature || LimpHome) ? NAN
                     : (TempSource == 0) ? (float)AlternatorTemperatureF
                     : (temperatureThermistor == -99) ? NAN : (float)temperatureThermistor;
  if (kneeLearnEnable && kneeTempComp && !isnan(tAlt)) {
    float knee = floorV + kneeMarginPct;
    float resistive = knee - kneeFitA; if (resistive < 0) resistive = 0;
    floorV -= resistive * 0.00218f * (kneeTempRefF - tAlt);
    if (floorV < 0) floorV = 0;
    if (floorV > kneeMaxFloorPct) floorV = kneeMaxFloorPct;
  }
  // The scalar "Min Field %" hard-floors every non-zero table value (a cell can raise the floor,
  // never pull it below MinDuty). The zero sentinel above is the one exception.
  floorV = fmaxf(floorV, MinDuty);
  // setDutyPercent enforces the field ceiling in hardware, so a floor above it can't be delivered; without this cap
  // lastAppliedDuty (telemetry, PID reseed, hunt observer) reports a duty the PWM silently clamps away.
  return fminf(floorV, ccDutyCeiling());
}
float getCapCurrentForRPM(float rpm) {
  return interpolateRPMTable(rpm, rpmCapCurrentTable);
}

// ===== AUTO MIN% LEARNING ("knee tracker") =====
// Learns per RPM bin the field duty where output amps begin to build (the "knee") and parks rpmMinDutyTable a margin
// below it. Observer only (rewrites the table like a user edit). Spec: Working Markdown Docs/MinDuty_Knee_Learning.md.
static bool kneeStateDirty = false;

// Per bin, a one-time probe: while parked at the floor and steady, capture a zero, then step the floor up kneeStepPct
// each dwell until output rises kneeOnsetA above it; lock at knee - margin. A locked bin re-learns only if current
// (> kneeReArmA) shows at its floor, dropping one margin step. Bin 0 is locked at 0%. Floors are RAW duty @ kneeTempRefF.
void kneeLearnObserve(float rpm, float appliedDuty, float tF, float amps,
                      float dutyRequest, float rpmFloorDuty, bool modeOk) {
  if (!kneeLearnEnable) return;   // Automatic Min% learning off — learner idle
  static uint32_t lastMs = 0, steadySince = 0, lastStepMs = 0;
  static float fRpm = 0, fTemp = 0, fDuty = 0;
  static bool fInit = false;
  static int   probeBin = -1;     // bin whose probe is in progress (-1 = none)
  static bool  haveZero = false;  // fresh zero captured for the current probe
  static float probeZero = 0.0f;  // amps baseline captured at probe start (below the knee)

  uint32_t now = millis();
  uint32_t dtMs = (lastMs == 0) ? 0 : (now - lastMs);
  lastMs = now;

  kneeActiveBin = -1; kneeFloorActive = false; kneeSteadyNow = false;

  bool valid = modeOk && !isnan(rpm) && !isnan(tF) && !isnan(amps)
               && !isnan(appliedDuty) && rpm > 0;
  if (!valid) { fInit = false; steadySince = 0; probeBin = -1; haveZero = false; return; }

  // Detector-input EMA (~0.5 s) so the steady bands reject jitter, not real operating-point moves.
  if (!fInit || dtMs == 0 || dtMs > 3000) {
    fRpm = rpm; fTemp = tF; fDuty = appliedDuty; fInit = true;
    steadySince = 0;
  } else {
    float a = (float)dtMs / (500.0f + (float)dtMs);
    fRpm += a * (rpm - fRpm); fTemp += a * (tF - fTemp); fDuty += a * (appliedDuty - fDuty);
  }

  // Attribute to the nearest RPM breakpoint.
  int b = 0; float best = 1e9f;
  for (int i = 0; i < RPM_TABLE_SIZE; i++) {
    float d = fabsf(fRpm - (float)rpmTableRPMPoints[i]);
    if (d < best) { best = d; b = i; }
  }
  kneeActiveBin = b;

  // Bin 0 (lowest RPM) is permanently locked at 0% — no usable knee that slowly. Never probe it.
  if (b == 0) {
    kneeFloor[0] = 0; if (kneeLearnEnable) rpmMinDutyTable[0] = 0;
    if (probeBin == 0) { probeBin = -1; haveZero = false; }
    return;
  }

  // Floor active = the controller wants no more field than the floor. dutyRequest (inner PID output) is clamped >= MinDuty
  // and bumpless-tracks the applied floor, so binding reads as <= floorEff — a strict "<" can never be true at MinDuty.
  float floorEff = (rpmFloorDuty > MinDuty) ? rpmFloorDuty : MinDuty;
  bool floorActive = (dutyRequest <= floorEff + 0.05f);
  kneeFloorActive = floorActive;

  // Steady-band check vs. the EMA (RPM, temp, applied duty — voltage intentionally not gated).
  float rpmTolAbs = fmaxf(20.0f, fRpm * (kneeRpmTolPct / 100.0f));
  bool steady = (fabsf(rpm - fRpm) <= rpmTolAbs)
              && (fabsf(tF - fTemp) <= kneeTempTolF)
              && (fabsf(appliedDuty - fDuty) <= kneeDutyTolPct);
  if (!steady) { steadySince = 0; if (probeBin == b) { probeBin = -1; haveZero = false; } return; }
  if (steadySince == 0) { steadySince = now; lastStepMs = now; }
  kneeSteadyNow = true;

  if (!kneeLearnEnable || !floorActive) return;
  uint32_t dwellMs = (uint32_t)(kneeDwellSec * 1000.0f); if (dwellMs < 250) dwellMs = 250;
  if ((now - steadySince) < dwellMs) return;   // initial settle after entering a steady window
  if ((now - lastStepMs) < dwellMs) return;    // one action per dwell
  lastStepMs = now;

  // Duty-domain knobs (kneeStepPct, kneeMarginPct, kneeMaxFloorPct) are REAL duty-% for THIS bus, used as-is; their
  // defaults are scaled by fieldScale() at first boot and rescaled on a class/winding change. Amps-domain knobs and the
  // learned floors are voltage-independent.
  float stepEff = kneeStepPct;
  float maxFloorEff = kneeMaxFloorPct;
  float margin = (kneeMarginPct > 0) ? kneeMarginPct : 0;
  float caseF = isnan(AlternatorTemperatureF) ? 0.0f : AlternatorTemperatureF;

  // A board ceiling (10 A / drive heat) is holding the field under the floor: the amps say nothing about this floor.
  if (g_fieldCeilBind >= 2 && appliedDuty >= ccDutyCeiling() - 0.05f) { probeBin = -1; haveZero = false; return; }

  if (kneeFrozen[b]) {
    // Locked: only act if the knee dropped below the floor (real current at the floor). Lower one step.
    if (amps > kneeReArmA) {
      kneeFloor[b] -= margin; if (kneeFloor[b] < 0) kneeFloor[b] = 0;
      kneeKnee[b] = kneeFloor[b] + margin;
      rpmMinDutyTable[b] = kneeFloor[b];
      kneeLastMs[b] = now;
      kneeStateDirty = true; settingsDirty = true;
    }
    probeBin = -1; haveZero = false;
    return;
  }

  if (probeBin != b || !haveZero) {
    // (Re)start the probe: capture a fresh zero at the current floor (we are below the knee here).
    probeBin = b; probeZero = amps; haveZero = true;
    rpmMinDutyTable[b] = kneeFloor[b];   // ensure the applied floor matches our probe state
    return;
  }

  if ((amps - probeZero) >= kneeOnsetA) {
    // Knee found at the current commanded floor. Lock at knee - margin (raw duty @ kneeTempRefF).
    kneeKnee[b]  = kneeFloor[b];
    kneeFloor[b] = kneeKnee[b] - margin; if (kneeFloor[b] < 0) kneeFloor[b] = 0;
    kneeFrozen[b] = true; kneeLearnTempF[b] = caseF;
    rpmMinDutyTable[b] = kneeFloor[b];
    probeBin = -1; haveZero = false;
    kneeLastMs[b] = now;
  } else {
    // No output yet: step the floor up one increment and keep hunting.
    kneeFloor[b] += stepEff;
    if (kneeFloor[b] >= maxFloorEff) {
      // Hit the ceiling without onset → no usable knee at this RPM. Lock at the ceiling.
      kneeFloor[b] = maxFloorEff;
      kneeKnee[b] = maxFloorEff + margin;
      kneeFrozen[b] = true; kneeLearnTempF[b] = caseF;
      probeBin = -1; haveZero = false;
      kneeLastMs[b] = now;
    }
    rpmMinDutyTable[b] = kneeFloor[b];
  }
  kneeStateDirty = true;
  settingsDirty = true;   // push the CSV3 Min% echo promptly so the table cells track the learned floor (not the 60s heartbeat)
}

// Least-squares fit of knee = a + C/RPM over the committed anchors (linear field region: onset ∝ R_field/(k·RPM); `a` =
// constant rectifier/brush threshold). Only C/RPM carries copper temperature (~0.218 %/°F), so x = (R(T)/R(Tref))/RPM
// with y = RAW onset, leaving `a` temperature-neutral. 3 anchors = fit + one residual to spot a bad point (UI captures 3).
// Returns false if < 2 anchors or the RPM span is degenerate.
bool kneeFitModel(float &outA, float &outC, float &outResidPct, int &outWorstIdx) {
  outA = 0.0f; outC = 0.0f; outResidPct = -1.0f; outWorstIdx = -1;
  if (kneeAnchorN < 2) return false;
  double sx = 0, sy = 0, sxx = 0, sxy = 0;
  int n = kneeAnchorN;
  for (int i = 0; i < n; i++) {
    float tCorr = 1.0f + 0.00218f * (kneeAnchorTempF[i] - kneeTempRefF);   // R(T)/R(Tref)
    double x = (kneeAnchorRPM[i] > 1.0f) ? (double)tCorr / kneeAnchorRPM[i] : 0.0;
    double y = kneeAnchorDuty[i];
    sx += x; sy += y; sxx += x * x; sxy += x * y;
  }
  double denom = (double)n * sxx - sx * sx;
  if (fabs(denom) < 1e-12) return false;       // all anchors at ~same RPM — no leverage on C
  double C = ((double)n * sxy - sx * sy) / denom;
  double a = (sy - C * sx) / (double)n;
  // Onset must rise as RPM falls; a perverse fit is clamped to C = 0, and the intercept refit for it
  // (the mean onset) — the negative-slope intercept sits above every measured point.
  if (C < 0) { C = 0; a = sy / (double)n; }
  outA = (float)a; outC = (float)C;
  // Worst residual (in % duty): predict each anchor's onset at ITS OWN temp (x carries R(T)/R(Tref))
  // and compare to the raw measured onset.
  float worst = 0.0f; int worstIdx = -1;
  for (int i = 0; i < n; i++) {
    float tCorr = 1.0f + 0.00218f * (kneeAnchorTempF[i] - kneeTempRefF);
    float pred = (float)(a + C * ((kneeAnchorRPM[i] > 1.0f) ? (double)tCorr / kneeAnchorRPM[i] : 0.0));
    float r = fabsf(kneeAnchorDuty[i] - pred);
    if (r > worst) { worst = r; worstIdx = i; }
  }
  outResidPct = worst; outWorstIdx = worstIdx;
  return true;
}

// Fit the Min% column from the committed anchors (kneeFitModel) into rpmMinDutyTable as the learner's FROZEN baseline.
// Bin 0 stays 0%. Returns false if < 3 anchors (matches the UI guard).
// END BEHAVIOR (SAFETY-CRITICAL — do not change to flat-hold):
//   - Below the lowest anchor: the model continues, clamped to maxKnee (a low-RPM floor never prevents shutoff).
//   - Above the highest anchor: floor forced to ZERO. A floor is minimum field the regulator can't turn off, so carrying
//     it past the commissioned range risks overvoltage/runaway. The UI's top anchor is the user's MAX working RPM for this.
bool kneeCurveApply() {
  if (kneeAnchorN < 3) return false;

  // Sort anchors ascending by RPM (tiny n — insertion sort). The fit is order-independent, but the
  // ceiling (= last anchor's RPM) and the diagnostic temp average read cleanest from sorted anchors.
  for (int i = 1; i < kneeAnchorN; i++) {
    float r = kneeAnchorRPM[i], d = kneeAnchorDuty[i], t = kneeAnchorTempF[i];
    int j = i - 1;
    while (j >= 0 && kneeAnchorRPM[j] > r) {
      kneeAnchorRPM[j + 1] = kneeAnchorRPM[j];
      kneeAnchorDuty[j + 1] = kneeAnchorDuty[j];
      kneeAnchorTempF[j + 1] = kneeAnchorTempF[j];
      j--;
    }
    kneeAnchorRPM[j + 1] = r; kneeAnchorDuty[j + 1] = d; kneeAnchorTempF[j + 1] = t;
  }

  float a, C, resid; int worstIdx;
  if (!kneeFitModel(a, C, resid, worstIdx)) return false;
  kneeFitA = a; kneeFitC = C; kneeFitResidPct = resid; kneeFitWorstIdx = worstIdx;

  // Average measured case temp across the anchors — diagnostic only (the "Learn °F" column); the live
  // correction uses the global kneeTempRefF, and the stored floors ARE normalized to kneeTempRefF.
  float tempAvg = 0.0f;
  for (int i = 0; i < kneeAnchorN; i++) tempAvg += kneeAnchorTempF[i];
  tempAvg /= (float)kneeAnchorN;

  const float maxKnee = kneeMaxFloorPct + kneeMarginPct;
  const float ceilingRPM = kneeAnchorRPM[kneeAnchorN - 1];  // highest commissioned RPM = the zero-ceiling
  float prev = 1e9f;  // running cap for monotone non-increasing knee-vs-RPM
  for (int b = 0; b < RPM_TABLE_SIZE; b++) {
    float rpmB = (float)rpmTableRPMPoints[b];
    float kneeB;
    // SAFETY: zero above the highest commissioned RPM (see header).
    if (rpmB > ceilingRPM) kneeB = 0.0f;
    else kneeB = a + C / fmaxf(rpmB, 1.0f);
    if (kneeB < 0) kneeB = 0;
    if (kneeB > prev) kneeB = prev;  // enforce non-increasing with RPM
    prev = kneeB;
    if (kneeB > maxKnee) kneeB = maxKnee;

    float floorB = (b == 0) ? 0.0f : fmaxf(0.0f, fminf(kneeB - kneeMarginPct, kneeMaxFloorPct));
    kneeKnee[b] = kneeB;
    kneeFloor[b] = floorB;
    kneeFrozen[b] = true;
    kneeLearnTempF[b] = tempAvg;
    rpmMinDutyTable[b] = floorB;
  }

  // SAFETY: interpolateRPMTable holds the LAST bin flat beyond the table, so a ceiling anchor at/above the last breakpoint
  // would let a non-zero floor ride to unlimited overspeed. Zero the last bin (tapers one bin early, the safe direction).
  if (ceilingRPM >= (float)rpmTableRPMPoints[RPM_TABLE_SIZE - 1]) {
    kneeKnee[RPM_TABLE_SIZE - 1] = 0.0f;
    kneeFloor[RPM_TABLE_SIZE - 1] = 0.0f;
    rpmMinDutyTable[RPM_TABLE_SIZE - 1] = 0.0f;
  }

  kneeStateDirty = true;            // persist knee blobs on the next field-off flush
  settingsDirty = true;             // push the CSV3 Min% echo so the table cells update now
  pendingSaveUserTableEdits = true; // persist rpmMinDutyTable via the same path as a manual edit
  queueConsoleMessageF("Min%% curve applied: %d anchors, fit a=%.1f%% C=%.0f, worst resid %.2f%%",
                       kneeAnchorN, a, C, resid);
  return true;
}

// Stage-7 entry copy of everything kneeCurveApply writes, so commissionStop can undo an applied-but-not-advanced column
// (the step snapshot holds the scalars, not the column). RAM only, deliberately: a reboot mid-step keeps an applied
// column and re-runs the step. Lives here because kneeStateDirty is file-static.
static float cxStepMinDuty[RPM_TABLE_SIZE], cxStepKneeFloor[RPM_TABLE_SIZE], cxStepKneeKnee[RPM_TABLE_SIZE], cxStepKneeTempF[RPM_TABLE_SIZE];
static bool  cxStepKneeFrozen[RPM_TABLE_SIZE];
static float cxStepKneeFitA, cxStepKneeFitC, cxStepKneeFitResid;
static int   cxStepKneeFitWorst;
static bool  cxStepMinPctValid = false;
void cxStepMinPctStash() {
  memcpy(cxStepMinDuty, rpmMinDutyTable, sizeof(cxStepMinDuty));
  memcpy(cxStepKneeFloor, kneeFloor, sizeof(cxStepKneeFloor));
  memcpy(cxStepKneeKnee, kneeKnee, sizeof(cxStepKneeKnee));
  memcpy(cxStepKneeFrozen, kneeFrozen, sizeof(cxStepKneeFrozen));
  memcpy(cxStepKneeTempF, kneeLearnTempF, sizeof(cxStepKneeTempF));
  cxStepKneeFitA = kneeFitA; cxStepKneeFitC = kneeFitC; cxStepKneeFitResid = kneeFitResidPct; cxStepKneeFitWorst = kneeFitWorstIdx;
  cxStepMinPctValid = true;
}
void cxStepMinPctDrop() { cxStepMinPctValid = false; }
// Returns true when a column was put back. Persists through the same deferred paths as an Apply.
bool cxStepMinPctRestore() {
  if (!cxStepMinPctValid) return false;
  memcpy(rpmMinDutyTable, cxStepMinDuty, sizeof(cxStepMinDuty));
  memcpy(kneeFloor, cxStepKneeFloor, sizeof(cxStepKneeFloor));
  memcpy(kneeKnee, cxStepKneeKnee, sizeof(cxStepKneeKnee));
  memcpy(kneeFrozen, cxStepKneeFrozen, sizeof(cxStepKneeFrozen));
  memcpy(kneeLearnTempF, cxStepKneeTempF, sizeof(cxStepKneeTempF));
  kneeFitA = cxStepKneeFitA; kneeFitC = cxStepKneeFitC; kneeFitResidPct = cxStepKneeFitResid; kneeFitWorstIdx = cxStepKneeFitWorst;
  cxStepMinPctValid = false;
  kneeStateDirty = true;
  settingsDirty = true;
  pendingSaveUserTableEdits = true;
  return true;
}

// Stage the per-bin knee state AND the Keep-Alive column on an open "learning" handle. Every save writes both:
// kneeLearnInit rebuilds the column from kneeFloor at boot, so a column saved without its knee state is lost on reboot.
static bool kneePutStateBlobs(nvs_handle_t h) {
  // From copies: web-task callers race the learner on Core 1 (bad-checksum blobs, see saveHistoricalDataImmediate)
  float flr[RPM_TABLE_SIZE], kn[RPM_TABLE_SIZE], minDuty[RPM_TABLE_SIZE];
  bool frz[RPM_TABLE_SIZE];
  memcpy(flr, kneeFloor, sizeof(flr));
  memcpy(kn, kneeKnee, sizeof(kn));
  memcpy(frz, kneeFrozen, sizeof(frz));
  memcpy(minDuty, rpmMinDutyTable, sizeof(minDuty));
  const float fitA = kneeFitA;
  bool ok = true;
  ok &= (nvs_set_blob(h, "kneeFloor", flr, sizeof(flr)) == ESP_OK);
  ok &= (nvs_set_blob(h, "kneeKnee", kn, sizeof(kn)) == ESP_OK);
  ok &= (nvs_set_blob(h, "kneeFrozen", frz, sizeof(frz)) == ESP_OK);
  ok &= (nvs_set_blob(h, "kneeFitA", &fitA, sizeof(fitA)) == ESP_OK);  // temp-neutral threshold for the live temp correction
  ok &= (nvs_set_blob(h, "minDutyTable", minDuty, sizeof(minDuty)) == ESP_OK);
  kneeStateDirty = false;
  return ok;
}

// Persist learned per-bin state (learning namespace blobs). Knobs persist separately via settingWrite.
void saveKneeLearnState() {
  nvs_handle_t h;
  if (nvs_open("learning", NVS_READWRITE, &h) != ESP_OK) return;
  kneePutStateBlobs(h);
  nvs_commit(h);
  nvs_close(h);
}

// A config-import Keep-Alive column is installed exactly as kneeCurveApply leaves a commissioned one: every bin locked,
// the file's knees and threshold, saved together so the boot rebuild reproduces it. Missing knees → floor + margin;
// missing threshold → 0 (whole-knee correction).
void kneeAdoptCommittedTable(const float *tbl, const float *knee, float fitA) {
  memcpy(rpmMinDutyTable, tbl, sizeof(rpmMinDutyTable));
  // The import writes the file's margin to NVS before this runs; RAM holds this unit's until the reboot.
  const float margin = settingExists(NK_kneeMarginPct) ? settingRead(NK_kneeMarginPct).toFloat() : kneeMarginPct;
  for (int i = 0; i < RPM_TABLE_SIZE; i++) {
    kneeFloor[i] = tbl[i];
    kneeKnee[i] = knee ? knee[i] : ((tbl[i] > 0.0f) ? tbl[i] + margin : 0.0f);
    kneeFrozen[i] = true;
    kneeLearnTempF[i] = 0;
  }
  kneeFitA = isfinite(fitA) ? fitA : 0.0f;
  kneeFitC = 0.0f; kneeFitResidPct = -1.0f; kneeFitWorstIdx = -1;
  saveKneeLearnState();
}

// A file that carries the learner's own per-bin state installs it as-is: the source regulator's column, floors, knees
// and locks, so the learner carries on exactly where the source's would after a reboot (the RAM-only fit slope and
// per-bin temperatures reset there too).
void kneeAdoptLearnedState(const float *tbl, const float *flr, const float *knee, const bool *frozen, float fitA) {
  memcpy(rpmMinDutyTable, tbl, sizeof(rpmMinDutyTable));
  memcpy(kneeFloor, flr, sizeof(kneeFloor));
  memcpy(kneeKnee, knee, sizeof(kneeKnee));
  memcpy(kneeFrozen, frozen, sizeof(kneeFrozen));
  memset(kneeLearnTempF, 0, sizeof(kneeLearnTempF));
  kneeFitA = isfinite(fitA) ? fitA : 0.0f;
  kneeFitC = 0.0f; kneeFitResidPct = -1.0f; kneeFitWorstIdx = -1;
  saveKneeLearnState();
}

// ── Min%-floor snapshot for the commissioning ABORT path ──────────────────────
// The pre-commissioning tune snapshot (NK_commissionSnap) excludes the Min% table stage 7 rewrites, so these back up /
// restore / clear minDutyTable + knee state as "bk_*" blobs and an abort (or reboot mid-run) reverts Min% too. Takes
// pointers: the deferred-Start worker persists RAM copies staged at the click, not the live tables.
void commissionBackupMinPct(const float *minDuty, const float *flr, const float *kn, const bool *frz, float fitA) {
  nvs_handle_t h;
  if (nvs_open("learning", NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_blob(h, "bk_minDuty", minDuty, sizeof(rpmMinDutyTable));
  nvs_set_blob(h, "bk_kneeFlr", flr, sizeof(kneeFloor));
  nvs_set_blob(h, "bk_kneeKn", kn, sizeof(kneeKnee));
  nvs_set_blob(h, "bk_kneeFrz", frz, sizeof(kneeFrozen));
  nvs_set_blob(h, "bk_kneeFitA", &fitA, sizeof(fitA));  // restore the live-correction threshold on abort too
  nvs_commit(h);
  nvs_close(h);
}

// Restore the Min% state from the Phase-0 backup, copy it into the live keys + RAM arrays, then
// delete the backups. Returns false (and changes nothing) if no complete backup is present.
bool commissionRestoreMinPct() {
  nvs_handle_t h;
  if (nvs_open("learning", NVS_READWRITE, &h) != ESP_OK) return false;
  float fMin[RPM_TABLE_SIZE], fFlr[RPM_TABLE_SIZE], fKn[RPM_TABLE_SIZE];
  bool  fFrz[RPM_TABLE_SIZE];
  size_t sz;
  bool ok = true;
  sz = sizeof(fMin); ok &= (nvs_get_blob(h, "bk_minDuty", fMin, &sz) == ESP_OK && sz == sizeof(fMin));
  sz = sizeof(fFlr); ok &= (nvs_get_blob(h, "bk_kneeFlr", fFlr, &sz) == ESP_OK && sz == sizeof(fFlr));
  sz = sizeof(fKn);  ok &= (nvs_get_blob(h, "bk_kneeKn",  fKn,  &sz) == ESP_OK && sz == sizeof(fKn));
  sz = sizeof(fFrz); ok &= (nvs_get_blob(h, "bk_kneeFrz", fFrz, &sz) == ESP_OK && sz == sizeof(fFrz));
  if (ok) {
    memcpy(rpmMinDutyTable, fMin, sizeof(fMin));
    memcpy(kneeFloor, fFlr, sizeof(fFlr));
    memcpy(kneeKnee, fKn, sizeof(fKn));
    memcpy(kneeFrozen, fFrz, sizeof(fFrz));
    nvs_set_blob(h, "minDutyTable", fMin, sizeof(fMin));   // the locals, not the live tables the learner writes
    nvs_set_blob(h, "kneeFloor", fFlr, sizeof(fFlr));
    nvs_set_blob(h, "kneeKnee", fKn, sizeof(fKn));
    nvs_set_blob(h, "kneeFrozen", fFrz, sizeof(fFrz));
    // Restore the live-correction threshold. Optional (old backups lack it) → 0 = whole-knee fallback.
    float fFitA = 0.0f; size_t szA = sizeof(fFitA);
    if (nvs_get_blob(h, "bk_kneeFitA", &fFitA, &szA) != ESP_OK || szA != sizeof(fFitA)) fFitA = 0.0f;
    kneeFitA = fFitA;
    nvs_set_blob(h, "kneeFitA", &fFitA, sizeof(fFitA));
  }
  nvs_erase_key(h, "bk_minDuty"); nvs_erase_key(h, "bk_kneeFlr");
  nvs_erase_key(h, "bk_kneeKn");  nvs_erase_key(h, "bk_kneeFrz");
  nvs_erase_key(h, "bk_kneeFitA");
  nvs_commit(h);
  nvs_close(h);
  return ok;
}

// Discard the Min% backup (commit path — the new tune stays).
void commissionClearMinPctBackup() {
  nvs_handle_t h;
  if (nvs_open("learning", NVS_READWRITE, &h) != ESP_OK) return;
  nvs_erase_key(h, "bk_minDuty"); nvs_erase_key(h, "bk_kneeFlr");
  nvs_erase_key(h, "bk_kneeKn");  nvs_erase_key(h, "bk_kneeFrz");
  nvs_erase_key(h, "bk_kneeFitA");
  nvs_commit(h);
  nvs_close(h);
}

// RPMScalingFactor is a linear gain on the engine-RPM axis, so everything learned against the old axis is stale. Core 1
// (flash writes); drives the same clear paths as the individual Clear buttons. Deliberately NOT touched: wear
// accumulators, fuel curve (true RPM), cap tables and rpmPoints (user-entered), sail front. The cloud half is gated by
// NK_RpmAxisWipePend. Runs any AXW_* subset: an import may bring some artifacts already on its own axis.
void rpmAxisWipeLocal(uint16_t parts) {
  if (parts & AXW_KNEE) {
    kneeLearnResetDefaults();       // Min% floor table + knee-learner state
    commissionClearMinPctBackup();  // its NVS backup snapshot
  }
  if ((parts & (AXW_HUNT | AXW_HUNTLOG)) == (AXW_HUNT | AXW_HUNTLOG)) huntMapClearAll("engine speed recalibrated");  // pocket centers are rpm-axis values
  else if (parts & (AXW_HUNT | AXW_HUNTLOG)) huntMapClearPart((parts & AXW_HUNT) != 0, "engine speed recalibrated");
  if (parts & AXW_CXBITS) commissionClearRpmDependents(); // every RPM-binned commissioning stage (all but Prep + Field cut)
  if (parts & AXW_SYSID) systemIDLogClearAll();          // step-test ring (records are RPM-stamped)
  if (parts & AXW_MOTORFRONT) resetMotorFrontOnly();     // motoring front only; sail front kept
  if (parts & AXW_ALTHEALTH) altHealthRecordClear("engine speed recalibrated");  // best-ever front + engine-hour trend + baseline; the damper map is AXW_HUNT's
  if (parts & AXW_OVERHEAT) pendingClearOverheatHistory = true;    // per-RPM-bin thermal history
  if (parts & AXW_FAMATRIX) faPendingMatrixClear = true;           // /famatrix.bin  (50-RPM x amp cells)
  if (parts & AXW_RIPTAB) ripTabPendingClear = true;               // /riptab.bin    (50-RPM bins)
  // both fits carry the RPM they were measured at, and came from the two tables above
  if (parts & (AXW_RIPFIT_ALT | AXW_RIPFIT_SLP))
    ripFitForget((parts & AXW_RIPFIT_ALT) != 0, (parts & AXW_RIPFIT_SLP) != 0, "RPM scaling changed");
  if (parts & AXW_FAFLIP) faPendingFlipWipeAll = true;             // /faflip.bin    (all 9 pages)
}

void rpmAxisWipeExecute() {
  rpmAxisWipeLocal(AXW_ALL);
  rpmAxisWipePending = true;
  settingWrite(NK_RpmAxisWipePend, "1");
  settingWrite(NK_RpmAxisWipeLoc, "0");  // local half done; cloud half still gated by NK_RpmAxisWipePend
  queueConsoleMessage("RPM scaling changed -- engine-RPM-indexed learning wiped; re-commission required");
}

// Throttled saver, called from loop(). Flash commit is gated on field-off so the NVS write never stalls the control
// loop while the field is on; losing an in-progress session on a power cut is harmless.
void kneeLearnService(bool fieldOff) {
  static uint32_t lastSaveMs = 0;
  if (!kneeStateDirty || !fieldOff) return;
  uint32_t now = millis();
  if (lastSaveMs != 0 && (now - lastSaveMs) < 300000UL) return;  // <= every 5 min
  lastSaveMs = now;
  saveKneeLearnState();
}

// Rebuild the visible Keep-Alive column from the learner's floors — THE one place that defines what a bin holds while the
// learner owns the table (kneeLearnInit, the enable handler and kneeLearnResetDefaults all route here).
// A decided bin (kneeFrozen) is copied verbatim, INCLUDING the deliberate zero above the commissioned RPM ceiling (a safety
// invariant). An UNDECIDED zero bin gets KNEE_BASELINE_PCT, written into kneeFloor too: an all-zero table makes
// getMinimumFieldForRPM return its hard-zero sentinel everywhere, which drops the MinDuty scalar as well. Bin 0 stays 0.
void kneeRebuildOwnedTable() {
  for (int i = 0; i < RPM_TABLE_SIZE; i++) {
    if (i != 0 && !kneeFrozen[i] && kneeFloor[i] <= 0.0f) kneeFloor[i] = KNEE_BASELINE_PCT;
    float f = (i == 0) ? 0.0f : kneeFloor[i];
    if (f < 0) f = 0; if (f > kneeMaxFloorPct) f = kneeMaxFloorPct;
    rpmMinDutyTable[i] = f;
  }
}

void kneeLearnResetDefaults() {
  for (int i = 0; i < RPM_TABLE_SIZE; i++) {
    kneeFloor[i] = 0; kneeKnee[i] = 0; kneeFrozen[i] = false; kneeLearnTempF[i] = 0; kneeLastMs[i] = 0;
  }
  kneeFitA = 0.0f; kneeFitC = 0.0f;   // no commissioning fit any more → live correction reverts to whole-knee
  // Every bin is now undecided, so this restores the flat baseline. Learner off = the hand-entered column stands.
  if (kneeLearnEnable) kneeRebuildOwnedTable();
  saveKneeLearnState();
}

// Boot init: load knobs (settings namespace, create defaults if absent) + learned state blobs.
// Call AFTER loadLearningTableFromNVS() so rpmMinDutyTable is already populated.
void kneeLearnInit() {
  if (!settingExists(NK_kneeLearnEnable)) settingWrite(NK_kneeLearnEnable, kneeLearnEnable ? "1" : "0");
  else kneeLearnEnable = (settingRead(NK_kneeLearnEnable).toInt() != 0);
  if (!settingExists(NK_kneeTempComp)) settingWrite(NK_kneeTempComp, kneeTempComp ? "1" : "0");
  else kneeTempComp = (settingRead(NK_kneeTempComp).toInt() != 0);
#define KNEE_LD_F(key, var) do { if (!settingExists(key)) settingWrite(key, String(var).c_str()); else var = settingRead(key).toFloat(); } while (0)
  // Duty-domain knobs are stored in REAL duty-% for this bus; the hardcoded globals are 12V values, so on FIRST creation
  // scale by fieldScale() before persisting. An existing key wins verbatim.
#define KNEE_LD_DUTY(key, var) do { if (!settingExists(key)) { var = var * fieldScale(); settingWrite(key, String(var, 4).c_str()); } else var = settingRead(key).toFloat(); } while (0)
  KNEE_LD_DUTY(NK_kneeMarginPct,   kneeMarginPct);
  KNEE_LD_F(NK_kneeOnsetA,      kneeOnsetA);
  KNEE_LD_F(NK_kneeReArmA,      kneeReArmA);
  KNEE_LD_DUTY(NK_kneeStepPct,     kneeStepPct);
  KNEE_LD_F(NK_kneeDwellSec,    kneeDwellSec);
  KNEE_LD_F(NK_kneeTempRefF,    kneeTempRefF);
  KNEE_LD_DUTY(NK_kneeMaxFloorPct, kneeMaxFloorPct);
  KNEE_LD_F(NK_kneeRpmTolPct,   kneeRpmTolPct);
  KNEE_LD_F(NK_kneeTempTolF,    kneeTempTolF);
  KNEE_LD_DUTY(NK_kneeDutyTolPct,  kneeDutyTolPct);   // duty-domain steadiness band, × fieldScale() like altDutyLineRms
#undef KNEE_LD_F
#undef KNEE_LD_DUTY

  nvs_handle_t h;
  bool haveState = false;
  if (nvs_open("learning", NVS_READONLY, &h) == ESP_OK) {
    size_t sz = sizeof(kneeFloor);
    bool okF = (nvs_get_blob(h, "kneeFloor", kneeFloor, &sz) == ESP_OK && sz == sizeof(kneeFloor));
    sz = sizeof(kneeKnee);
    bool okK = (nvs_get_blob(h, "kneeKnee", kneeKnee, &sz) == ESP_OK && sz == sizeof(kneeKnee));
    sz = sizeof(kneeFrozen);
    bool okZ = (nvs_get_blob(h, "kneeFrozen", kneeFrozen, &sz) == ESP_OK && sz == sizeof(kneeFrozen));
    haveState = okF && okK && okZ;
    // Temp-neutral threshold for the live correction; absent (never commissioned) → 0 = scale the whole knee.
    sz = sizeof(kneeFitA);
    if (nvs_get_blob(h, "kneeFitA", &kneeFitA, &sz) != ESP_OK || sz != sizeof(kneeFitA)) kneeFitA = 0.0f;
    nvs_close(h);
  }
  if (!haveState)
    for (int i = 0; i < RPM_TABLE_SIZE; i++) { kneeFloor[i] = 0; kneeKnee[i] = 0; kneeFrozen[i] = false; kneeLearnTempF[i] = 0; }

  // While learning owns the table the floor owns it from boot (bin 0 always 0%); learning off = hand-entered table stands.
  if (kneeLearnEnable) kneeRebuildOwnedTable();
}

// Build the /kneeLearnState JSON (knobs + live status + per-bin learned state).
String kneeLearnStateJson() {
  String j = "{";
  j += "\"enable\":";        j += (kneeLearnEnable ? 1 : 0);
  j += ",\"marginPct\":";    j += String(kneeMarginPct, 2);
  j += ",\"onsetA\":";       j += String(kneeOnsetA, 2);
  j += ",\"reArmA\":";       j += String(kneeReArmA, 2);
  j += ",\"stepPct\":";      j += String(kneeStepPct, 2);
  j += ",\"dwellSec\":";     j += String(kneeDwellSec, 1);
  j += ",\"tempRefF\":";     j += String(kneeTempRefF, 1);
  j += ",\"tempComp\":";     j += (kneeTempComp ? 1 : 0);
  j += ",\"maxFloorPct\":";  j += String(kneeMaxFloorPct, 2);
  j += ",\"rpmTolPct\":";    j += String(kneeRpmTolPct, 1);
  j += ",\"tempTolF\":";     j += String(kneeTempTolF, 1);
  j += ",\"dutyTolPct\":";   j += String(kneeDutyTolPct, 2);
  j += ",\"activeBin\":";    j += String(kneeActiveBin);
  j += ",\"floorActive\":";  j += String(kneeFloorActive ? 1 : 0);
  j += ",\"steady\":";       j += String(kneeSteadyNow ? 1 : 0);
  j += ",\"bins\":[";
  uint32_t now = millis();
  for (int i = 0; i < RPM_TABLE_SIZE; i++) {
    if (i) j += ",";
    long ageS = (kneeLastMs[i] == 0) ? -1 : (long)((now - kneeLastMs[i]) / 1000UL);
    j += "{\"rpm\":";    j += String(rpmTableRPMPoints[i]);
    j += ",\"floor\":";  j += String(rpmMinDutyTable[i], 2);
    j += ",\"knee\":";   j += String(kneeKnee[i], 2);
    j += ",\"frozen\":"; j += (kneeFrozen[i] ? 1 : 0);
    j += ",\"learnT\":"; j += String(kneeLearnTempF[i], 0);
    j += ",\"ageS\":";   j += String(ageS);
    j += "}";
  }
  j += "]}";
  return j;
}

// The per-class plausibility window both battery-voltage readings are judged against.
void voltagePlausibleWindow(float &minPlausible, float &maxPlausible) {
  // Use the user-entered nominal bank class (SYSTEM_VOLTAGE_CLASS) directly — not an autodetect from
  // BulkVoltage, which can mis-bucket and "F up" near class boundaries.
  if (SYSTEM_VOLTAGE_CLASS >= 48) {
    // 48V system (normal bulk = 55.2-57.6V)
    minPlausible = 18.0f;  // Dead battery - 0.5V buffer
    maxPlausible = 60.5f;  // Max charging + 0.5V buffer
  } else if (SYSTEM_VOLTAGE_CLASS >= 36) {
    // 36V system (normal bulk = 41.4-43.2V)
    minPlausible = 13.5f;  // Dead battery - 0.5V buffer
    maxPlausible = 46.5f;  // Max charging/equalization + buffer
  } else if (SYSTEM_VOLTAGE_CLASS >= 24) {
    // 24V system (normal bulk = 27.6-28.8V)
    minPlausible = 9.0f;   // Dead battery - 0.5V buffer
    maxPlausible = 32.5f;  // Max charging/equalization + buffer
  } else {
    // 12V system (normal bulk = 13.8-14.4V)
    minPlausible = 4.5f;   // Dead battery - 0.5V buffer
    maxPlausible = 15.5f;  // Max charging + 0.5V buffer
  }
  // Never below the overvoltage ladder: lead-acid seeds the hardware cutoff at 16.0 V x class/12, and a real
  // overvoltage under it was reported as both voltage sensors failing instead of as an overvoltage.
  maxPlausible = fmaxf(maxPlausible, VoltageHardwareLimit + 0.25f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f));
}

bool voltageInPlausibleWindow(float v) {
  float lo, hi;
  voltagePlausibleWindow(lo, hi);
  return !isnan(v) && v >= lo && v <= hi;
}

bool isVoltageSensorPlausible() {
  float minPlausible, maxPlausible;
  voltagePlausibleWindow(minPlausible, maxPlausible);

  // Check BatteryV sensor — not consulted while the owner runs without the second reading (§4.8)
  bool batteryVPlausible = !IgnoreBattV2 && (BatteryV >= minPlausible && BatteryV <= maxPlausible && !isnan(BatteryV));

  // Check IBV sensor — likewise not consulted while the owner runs without the main reading
  bool ibvPlausible = !IgnoreBattVMain && (IBV >= minPlausible && IBV <= maxPlausible && !isnan(IBV));

  return (batteryVPlausible || ibvPlausible);
}

// Call from setup() after NVS and sensors are initialized.
void tempPID_init() {
  // Start at zero; FF and the integral build only if temperature demands it. tempPID_tick()
  // does the real bumpless seed when it first activates on a valid temp.
  thermalPenaltyAmps = 0.0f;
  prevThermalPenalty = 0.0f;
  thermalIntegral = 0.0f;
  tempPIDActive = false;

  Serial.printf("TempPID: Init | Kp=%.2f Ki=%.3f Lookahead=%.1fs Interval=%lums\n",
                TempPIDKp, TempPIDKi, ThermalLookaheadSec, (unsigned long)TempPIDIntervalMs);
}
// tempFilterUpdate: IIR filter + slope estimator + projected-temp lookahead (display/estimator only; no PID/field/persisted
// state). Runs every tick; invalid temps are skipped. The per-pass IIR reaches each ~5 s 1-wire reading in <0.1 s and
// rejects nothing, so over-temp trips use the lower of the probe's last two readings (tempTripValue).
static uint32_t tempTripStamp = 0;
static float tempTripCur = NAN, tempTripPrev = NAN;
float tempTripValue() {
  if (TempSource != 0 || isnan(tempTripCur)) return tempFiltered;
  return fminf(tempTripCur, tempTripPrev);
}
void tempFilterUpdate(uint32_t nowMs) {
  if (TempSource == 0) {
    uint32_t ts = dataTimestamps[IDX_ALTERNATOR_TEMP];
    if (ts != 0 && ts != tempTripStamp) {
      tempTripStamp = ts;
      float v = AlternatorTemperatureF;  // read after the stamp: TempTask writes the value first
      if (!isnan(v) && v > -50.0f && v < 400.0f) {
        tempTripPrev = isnan(tempTripCur) ? v : tempTripCur;
        tempTripCur = v;
      }
    }
  } else {
    tempTripCur = tempTripPrev = NAN;
  }
  bool tempValueSane = !isnan(TempToUse) && (TempToUse > -50.0f) && (TempToUse < 400.0f);
  if (!tempValueSane) return;  // hold last filtered value; do not poison with garbage

  // ===== IIR filter =====
  if (isnan(tempFiltered) || tempFilterNeedsReseed) {
    tempFiltered = TempToUse;
    tempFilterNeedsReseed = false;
  } else {
    float alpha = (TempSource == 0) ? TempPIDFilterAlpha : 0.02f;  // thermistor alpha hardcoded — not user-configurable
    tempFiltered = alpha * TempToUse + (1.0f - alpha) * tempFiltered;
  }

  // Slope estimator: backward difference over the last `intervals` samples of a 13×5s=60s buffer.
  // The difference span = ThermalSlopeWindowSec (live-tunable); shorter = less lag, noisier slope.
  // The 60s buffer-full warmup gate (thermalSlopeBufFull) is independent of the difference span.
  if ((uint32_t)(nowMs - thermalSlopeLastPushMs) >= TempPIDIntervalMs) {
    thermalSlopeLastPushMs = nowMs;
    float tempSample = (TempSource == 0) ? TempToUse : tempFiltered;
    thermalSlopeBuffer[thermalSlopeBufIdx] = tempSample;
    thermalSlopeBufIdx = (thermalSlopeBufIdx + 1) % THERMAL_SLOPE_BUF;
    if (thermalSlopeBufIdx == 0) thermalSlopeBufFull = true;

    if (thermalSlopeBufFull) {
      // Clamp intervals to [2, BUF-1]: min 2 (10s) avoids single-step noise; max 12 = the full 60s buffer.
      // The newest sample is at (bufIdx-1).
      const float slopeDtSec = TempPIDIntervalMs / 1000.0f;
      int intervals = (int)lroundf(ThermalSlopeWindowSec / slopeDtSec);
      if (intervals < 2) intervals = 2;
      if (intervals > THERMAL_SLOPE_BUF - 1) intervals = THERMAL_SLOPE_BUF - 1;
      int oldIdx = ((int)thermalSlopeBufIdx - 1 - intervals) % THERMAL_SLOPE_BUF;
      if (oldIdx < 0) oldIdx += THERMAL_SLOPE_BUF;
      float oldest = thermalSlopeBuffer[oldIdx];
      const float windowSec = (float)intervals * slopeDtSec;
      float rawSlope = (tempSample - oldest) / windowSec;
      const float SLOPE_CLAMP = 0.5f;  // °F/sec — beyond this is sensor noise or fault
      if (fabsf(rawSlope) > SLOPE_CLAMP) {
        // Reject as sensor noise; hold the previous slope so a real fast rise keeps predictive
        // signal while one outlier rolls through the window. (Clamping to ±0.5 instead would
        // inject up to ±15°F of false lookahead.)
        static uint32_t slopeClampLastLogMs = 0;
        if ((uint32_t)(nowMs - slopeClampLastLogMs) >= 60000) {
          slopeClampLastLogMs = nowMs;
          queueConsoleMessageF("TempPID: raw slope %.3f °F/s rejected as sensor noise — holding previous", rawSlope);
        }
        rawSlope = thermalSlopeFPerSec;  // hold previous good value
      }
      thermalSlopeFPerSec = rawSlope;
    } else {
      thermalSlopeFPerSec = 0.0f;  // buffer not yet full — no prediction yet
    }
  }
  // Update projected temperature every call so PID input is always fresh.
  {
    float tempNow = (TempSource == 0) ? TempToUse : tempFiltered;
    projectedTempF = isnan(tempNow) ? tempFiltered : (tempNow + thermalSlopeFPerSec * ThermalLookaheadSec);
  }
}

void tempPID_tick(uint32_t nowMs, float actualDtSec) {

  // Running without the alternator temperature (§3.2, owner override): zero penalty, loop parked, a clean
  // re-seed when it clears.
  if (IgnoreTemperature) {
    if (tempPIDActive || thermalPenaltyAmps != 0.0f) {
      thermalPenaltyAmps = 0.0f;
      prevThermalPenalty = 0.0f;
      thermalPenaltyLastValid = 0.0f;
      thermalIntegral = 0.0f;
      tempPIDActive = false;
      thermalPreserveSlopeOnResume = false;
    }
    return;
  }

  // --- Hard reset path (requested externally, e.g. from web UI command) ---
  if (tempPIDResetRequested) {
    tempPIDResetRequested = false;
    thermalPenaltyAmps = 0.0f;
    prevThermalPenalty = 0.0f;          // last-applied penalty (slew/stale seed) — zero it
    thermalIntegral = 0.0f;             // holding integral — forget the learned hold on a hard reset
    tempPIDActive = false;
    tempFilterNeedsReseed = true;
    memset(thermalSlopeBuffer, 0, sizeof(thermalSlopeBuffer));
    thermalSlopeBufIdx = 0;
    thermalSlopeBufFull = false;
    thermalSlopeLastPushMs = 0;
    thermalSlopeFPerSec = 0.0f;
    projectedTempF = NAN;
    thermalIntegratorReleased = false;  // fresh approach — dI held off until present temp reaches setpoint again
    tempInvalidSinceMs = 0;             // clear the invalid-temp debounce window
    queueConsoleMessage("ThermalPID: manual reset requested - penalty accumulator and filter cleared");
    return;
  }

  // Stage bounds from inBulkStage/inAbsorptionStage directly: voltageControlActive is reassigned AFTER this returns (one
  // tick stale). With engine speed ignored g_I_cap (NoRpmCurrentA) is the ceiling; the table's 0 A first row would null the derate.
  const float capCurrent = (IgnoreRPM && g_I_cap > 0.0f) ? g_I_cap : getCapCurrentForRPM(RPM);
  const float penaltyMax = (float)MaxTableValue;
  const bool inPureBulk = (inBulkStage && !inAbsorptionStage);
  const float penaltyMin = 0.0f;  // penalty is derate-only: I_cmd = I_cap − penalty

  // Sanity guard stops PID only on an invalid value. Stale data (no new reading) is
  // handled elsewhere at 20s by tempDataVeryStale → field cut.
  bool tempValueSane = !isnan(TempToUse) && (TempToUse > -50.0f) && (TempToUse < 400.0f);

  if (!tempValueSane) {
    // Ride through brief glitches: hold last penalty, stay ACTIVE with slope buffer intact for
    // up to THERMAL_INVALID_DEBOUNCE_MS; only a sustained dropout deactivates (so the resume
    // still clears a genuinely-stale buffer). Over-temp protections + tempDataVeryStale unaffected.
    const uint32_t THERMAL_INVALID_DEBOUNCE_MS = 3000;
    if (tempInvalidSinceMs == 0) tempInvalidSinceMs = nowMs;
    thermalPreserveSlopeOnResume = false;  // an invalid sample voids a pending preserve-on-resume
    if (tempPIDActive && (uint32_t)(nowMs - tempInvalidSinceMs) >= THERMAL_INVALID_DEBOUNCE_MS) {
      tempPIDActive = false;
      queueConsoleMessageF("TempPID: temp value invalid >%lums, holding penalty at %.1fA",
                           (unsigned long)THERMAL_INVALID_DEBOUNCE_MS, thermalPenaltyLastValid);
    }
    return;  // Hold last valid penalty — do not touch thermalPenaltyAmps.
  }
  tempInvalidSinceMs = 0;  // valid sample — reset the debounce window

  // tempFiltered / slope buffer / projectedTempF are updated by tempFilterUpdate() at the top
  // of AdjustFieldLearnMode (every tick, every mode, before this fn) — already fresh here.
  const float activeTempLimit = TemperatureLimitF;

  // Suppress the warmup margin during commissioning/tuning: they reseed the slope buffer, so the margin would step the
  // setpoint and derate mid-test. Hard trips are NOT gated. Commissioning = wizard dialog open, not the state byte (a
  // stopped run must not derate-exempt for days).
  const bool suppressWarmupMargin = cxOwnsBatteryNow() || (TuningMode != 0);

  if (!tempPIDActive) {

    // Preserve the slope buffer when re-enabling from a DORMANT-but-not-stale period (TuningMode exit), else a blind warmup
    // derates right after a test. The one-shot flag is the gate, NOT a freshness timestamp: after a real sensor gap
    // tempFilterUpdate pushes a gap-spanning sample the same tick, so a time test would look fresh over garbage.
    const bool preserveSlope = thermalPreserveSlopeOnResume;
    thermalPreserveSlopeOnResume = false;  // consume the one-shot

    if (!preserveSlope) {
      // Stale slope would inflate projectedTempF used for the bumpless seed below.
      memset(thermalSlopeBuffer, 0, sizeof(thermalSlopeBuffer));
      thermalSlopeBufIdx = 0;
      thermalSlopeBufFull = false;
      thermalSlopeLastPushMs = 0;
      thermalSlopeFPerSec = 0.0f;
      // Recompute projectedTempF without stale slope — this is what the seed actually sees.
      {
        float tempNow = (TempSource == 0) ? TempToUse : tempFiltered;
        projectedTempF = isnan(tempNow) ? tempFiltered : tempNow;
      }
    }
    // Warmup setpoint: a cleared (blind) buffer takes the −20°F margin; a preserved-full buffer
    // or active suppression (commissioning/tuning) takes the normal −7°F.
    const float reEnableSetpoint = activeTempLimit -
        ((suppressWarmupMargin || thermalSlopeBufFull) ? 7.0f : 20.0f);

    tempPIDInput_d = (double)projectedTempF;

    // Bumpless resume: FF is recomputed instantly, so seed the holding integral to ZERO — penalty
    // starts at FF alone. Seeding from thermalPenaltyLastValid would overpunish if temp was high
    // when the sensor went stale but has since recovered. The integral rebuilds the hold.
    float stalePenalty = thermalPenaltyLastValid;
    float e_resume = projectedTempF - reEnableSetpoint;
    float resumePenalty = clamp_f(e_resume > 0.0f ? TempPIDKp * e_resume : 0.0f,
                                  penaltyMin, penaltyMax);

    thermalPenaltyAmps = resumePenalty;
    prevThermalPenalty = resumePenalty;
    thermalPenaltyLastValid = resumePenalty;
    thermalIntegral = 0.0f;  // FF carries the resume cut; the integral rebuilds the hold

    // Approach gate from present state: resume hot = released, resume cool = fresh approach.
    thermalIntegratorReleased = (projectedTempF >= activeTempLimit - 7.0f);

    tempPIDActive = true;
    queueConsoleMessageF("TempPID: resumed | projTemp=%.1f°F setpoint=%.1f°F penalty=%.1fA (was %.1fA) stage=%s",
                         projectedTempF, reEnableSetpoint, resumePenalty,
                         stalePenalty, inPureBulk ? "bulk" : "CV");
  }

  // effectiveSetpoint: −20°F fallback margin during the blind warmup (buffer not full) so the PID
  // reacts before projected temp reaches the limit; −7°F once the buffer fills or while suppressed.
  const float warmupMargin = suppressWarmupMargin ? 7.0f : 20.0f;
  const float effectiveSetpoint = thermalSlopeBufFull ? (activeTempLimit - 7.0f) : (activeTempLimit - warmupMargin);
  tempPIDSetpoint_d = (double)effectiveSetpoint;

  // tempNowPid (present temp) drives the approach gate and ePpos / eI; lookaheadDeltaF only feeds the
  // outerTermLookahead log share.
  float lookaheadDeltaF;
  float tempNowPid;
  {
    tempNowPid = (TempSource == 0) ? TempToUse : tempFiltered;
    lookaheadDeltaF = fmaxf(0.0f, projectedTempF - fmaxf(tempNowPid, effectiveSetpoint));
    tempPIDInput_d = (double)tempNowPid;
  }

  // Approach gate: released the first time PRESENT temp (not the projection) reaches setpoint; until then dI is held off
  // (FF handles the approach). Winding a hold early overbuilds ~2.5× and sags deep post-peak.
  if (!thermalIntegratorReleased && tempNowPid >= (activeTempLimit - 7.0f)) {
    thermalIntegratorReleased = true;
    queueConsoleMessageF("TempPID: integrator released — present temp %.1f°F reached setpoint", tempNowPid);
  }

  // HYBRID penalty = clamp(FF + thermalIntegral, 0, capCurrent): FF (P + projection) is POSITIONAL; only the holding integral
  // accumulates. ePpos is floored so the projection term never goes negative; eI is present-only so the integral never
  // winds on the projection.
  const float ePpos = fmaxf(0.0f, fmaxf(projectedTempF, tempNowPid) - effectiveSetpoint);
  const float eI    = tempNowPid - effectiveSetpoint;
  const float FF    = TempPIDKp * ePpos;

  // Integral gate: hold dI off during the approach (until released) and on the descent (above
  // setpoint AND cooling). FF is never gated.
  const bool descentHold = (eI > 0.0f) && (thermalSlopeFPerSec < 0.0f);
  const bool applyDI     = thermalIntegratorReleased && !descentHold;

  // Asymmetric bleed: below setpoint release at Ki×TempPIDKiDownFrac (not full Ki) so a transient
  // undershoot bleeds the hold slowly instead of collapsing it; above setpoint full Ki.
  const float kiEff = (eI < 0.0f) ? (TempPIDKi * TempPIDKiDownFrac) : TempPIDKi;
  const float dI    = applyDI ? (kiEff * eI * actualDtSec) : 0.0f;

  // Anti-windup: clamp the integral to [0, cap−FF] so FF+I can never exceed live authority and I
  // cannot wind into dead authority; it rebuilds as FF drops.
  const float iRaw   = thermalIntegral + dI;
  const float iCeil  = fmaxf(0.0f, capCurrent - FF);
  const bool  satClamp = (iRaw > iCeil);
  thermalIntegralCeil = iCeil;  // logged as iCeil_A; iCeil < outerI ⇒ clamp is deleting earned hold (watch reheat on recovery)
  thermalIntegral = clamp_f(iRaw, 0.0f, iCeil);

  const float penaltyRaw    = FF + iRaw;             // pre-clamp requested penalty (for the log / rpmCap diagnostic)
  const float penaltyTarget = FF + thermalIntegral;  // post-clamp, already within [0, capCurrent]

  // Asymmetric slew, then final clamp to the SAME live cap.
  thermalPenaltyAmps = slew_limit_f(prevThermalPenalty, penaltyTarget,
                                    ThermalPenaltyRiseRate, ThermalPenaltyFallRate, actualDtSec);
  thermalPenaltyAmps = clamp_f(thermalPenaltyAmps, 0.0f, capCurrent);

  prevThermalPenalty      = thermalPenaltyAmps;
  thermalPenaltyLastValid = thermalPenaltyAmps;

  // freezeWhy log enum: 0 = dI applied; 1 = approach (dI held); 2 = saturation (integral clamped
  // at the live cap); 3 = descent (above setpoint and cooling). Priority approach>saturation>descent.
  if (!thermalIntegratorReleased)  thermalFreezeReason = 1;
  else if (satClamp)               thermalFreezeReason = 2;
  else if (descentHold)            thermalFreezeReason = 3;
  else                             thermalFreezeReason = 0;
  {
    static bool satClampLogged = false;  // one-shot per saturation episode
    if (satClamp && thermalIntegratorReleased) {
      if (!satClampLogged) {
        satClampLogged = true;
        queueConsoleMessageF("TempPID: integral clamped — FF %.1fA + I covers live rpm cap %.1fA", FF, capCurrent);
      }
    } else {
      satClampLogged = false;
    }
  }

  // Logging decomposition (see "Thermal log decoding" in Thermal_Loop_Dev_Summary.md):
  // FF = outerTermP + outerTermLookahead; total penalty = FF + outerTermI.
  outerTermP         = TempPIDKp * fmaxf(0.0f, tempNowPid - effectiveSetpoint);  // present-error P share
  outerTermLookahead = TempPIDKp * lookaheadDeltaF;                              // projection share of FF
  outerTermI         = thermalIntegral;
  outerPenaltyRaw    = penaltyRaw;                                              // pre-clamp/slew, vs rpmCap = requested-vs-applied

  outerAntiWindupFired = false;  // log column dead (no negative bias to bleed under the [0,cap] clamp)

  // outerImpliedPenalty: voltage cap as a downstream penalty equivalent, log-only. inPureBulk
  // (not voltageControlActive) for the same one-tick-stale reason as the stage bounds above.
  if (!inPureBulk && Icv > 1.0f) {
    outerImpliedPenalty = fmaxf(0.0f, capCurrent - Icv);
  } else {
    outerImpliedPenalty = 0.0f;
  }

  thermalAccuracyScore_tick(nowMs, actualDtSec);
}
void pidLog_init() {
  if (!psramFound()) {
    Serial.println("FATAL: PSRAM not found, pidLog disabled");
    pidLogReady = false;
    pidLog = nullptr;
    return;
  }

  pidLog = (PidLogEntry *)heap_caps_malloc(
    PID_LOG_SIZE * sizeof(PidLogEntry),
    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

  if (!pidLog) {
    Serial.println("FATAL: Failed to allocate pidLog in PSRAM");
    pidLogReady = false;
    return;
  }

  pidLogHead = 0;
  pidLogCount = 0;
  pidLogReady = true;

  Serial.printf("PidLog: %d entries x %u bytes = %u KB in PSRAM\n",
                PID_LOG_SIZE,
                (unsigned)sizeof(PidLogEntry),
                (unsigned)(PID_LOG_SIZE * sizeof(PidLogEntry) / 1024));
}
// Call ONCE at the END of the normal control path (after governor_apply and all state updates). Not on shutdown/fault
// paths — those are not the instability target.
void pidLog_tick(uint32_t nowMs) {
  if (!loggingActive) return;  // Stop Logs: skip append, freeze buffer
  if (!pidLogReady || !pidLog) return;

  // Pause watchdog: auto-resume if download stalled or was aborted
  if (pidLogPaused) {
    if ((uint32_t)(nowMs - pidLogPausedAtMs) > THERMAL_LOG_PAUSE_TIMEOUT_MS) {
      serialPrintlnNB("pidLog: pause watchdog triggered - connection likely aborted");
      pidLogPaused = false;
    } else {
      return;
    }
  }

  // No interval gate here — tick rate is governed by the CH1 data gate upstream.

  PidLogEntry &e = pidLog[pidLogHead];

  e.ts = nowMs;

  // ── Mode / Stage ──────────────────────────────────────────────────────────
  e.chargeStageDisplay = getChargeStageDisplayCode();
  e.TargetVoltageMode = (uint8_t)TargetVoltageMode;
  e.flags = 0;
  if (sysMode == SYS_MODE_AUTO) e.flags |= (1 << 0);
  if (voltageControlActive) e.flags |= (1 << 1);
  if (govMode != GOV_NORMAL_SLEW) e.flags |= (1 << 4);
  e.ovFlags = 0;
  if (g_fastOvClampActive) e.ovFlags |= (1 << 0);
  if (g_iExcessBulkActive) e.ovFlags |= (1 << 1);  // iExcess BULK sub-mode (current-control phase)
  if (g_fastOvHardActive) e.ovFlags |= (1 << 2);
  if (g_iExcessActive) e.ovFlags |= (1 << 3);
  if (g_loadDumpActive) e.ovFlags |= (1 << 4);

  // ── Voltage loop ─────────────────────────────────────────────────────────
  e.battV = IBV;
  e.ChargingVoltageTarget = ChargingVoltageTarget;
  e.vError = pidLog_vError;
  e.Icv = Icv;
  e.cv_I = cv_I;
  e.tableThermalLimit = pidLog_uTargetBeforeVoltCap;
  e.setpointCmd = pidLog_uTargetAfterVoltCap;

  // ── Voltage loop event flags ──────────────────────────────────────────────
  e.voltageLoopRanThisTick = pidLog_voltageLoopRanThisTick;
  e.enteringCV = pidLog_enteringCV;
  e.enteringTargetVoltageMode = pidLog_enteringTargetVoltageMode;
  e.pad1 = 0;

  // ── Output current PID ────────────────────────────────────────────────────
  e.pidSetpoint = (float)pidSetpoint;
  e.pidInput = (float)pidInput;
  e.pidUnsatOutput = (sysMode == SYS_MODE_AUTO)
                       ? (float)currentPID.GetLastUnsatOutput()
                       : dutyCycle;
  e.pidOutput = (float)pidOutput;
  e.innerTermP = innerTermP;
  e.innerTermI = innerTermI;
  e.innerTermD = innerTermD;

  // ── Duty pipeline ─────────────────────────────────────────────────────────
  e.dutyRequest = pidLog_dutyRequest;
  e.dutyApplied = pidLog_dutyApplied;

  // ── Context ───────────────────────────────────────────────────────────────
  e.rpm = RPM;
  e.measAmps = MeasuredAmps;
  // Inner PID gains ACTUALLY handed to currentPID (setting × fieldScale(), Ki × the damper's live derate). The configured
  // gains ride the CSV3 echo saved beside every log, so derate = innerKi ÷ (PidKi × fieldWindApplied/SYSTEM_VOLTAGE_CLASS).
  e.innerKp = PidKp_active;
  e.innerKi = PidKi_active * g_huntDerate;
  e.innerKd = PidKd_active;
  e.voltageKp = (float)VoltageKp_active;  // outer voltage loop — gain actually in effect (Manual or Measured α/K)
  e.voltageKi = (float)VoltageKi_active;

  e.battV_filt = IBV_filtered;
  e.dBcur_dt = g_dBcur_dt;
  e.battI = getBatteryCurrent();
  e.ch1IntervalMs = (int16_t)g_ch1LastIntervalMs;
  e.voltLoopIntervalMs = pidLog_voltageLoopRanThisTick ? (int16_t)g_voltLoopLogIntervalMs : 0;
  e.inaIntervalMs = (int16_t)ina_last_ms;
  e.pad2 = 0;
  e.mExcessEma = g_mExcessEma;             // iExcess detector traces — averaged excess vs its
  e.iExcessThreshold = g_iExcessThreshold; // computed fire threshold E (both A), for offline tuning

  pidLogHead = (pidLogHead + 1) % PID_LOG_SIZE;
  if (pidLogCount < PID_LOG_SIZE) pidLogCount++;
}

// ===== TEMP-FIELDCHECK BEGIN: TEMPORARY field-current validation log (see the Xregulator.ino block). REMOVE after the drive. =====
// void fieldCheck_init() {
//   fc = (FcStore *)heap_caps_malloc(sizeof(FcStore), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
//   if (fc) {
//     memset(fc, 0, sizeof(FcStore));
//     fc->row.clear();
//     for (int i = 0; i < FC_SEC_N; i++) fc->sec[i].clear();
//   }
//   Serial.printf("fieldCheck: %u entries x %u bytes, %u KB PSRAM %s\n", (unsigned)FC_RING_N, (unsigned)sizeof(FcRec),
//                 (unsigned)(sizeof(FcStore) / 1024), fc ? "OK" : "FAILED");
// }

// // busV = the voltage the Field Amps estimate is computed from (updateFieldTelemetry's argument).
// void fieldCheck_tick(uint32_t now, float busV) {
//   if (!fc) return;
//   // Lambdas, not functions: the sketch prototyper would declare FcRec functions above the struct.
//   auto fcFill = [](FcRec &r, uint8_t kind, uint32_t now, const FcAcc &a) {
//     r.ms = now;
//     r.kind = kind;
//     r.mode = (uint8_t)sysMode;
//     r.lim = ctrlLimiter;
//     r.bind = g_fieldCeilBind;
//     r.stage = getChargeStageDisplayCode();
//     r.fieldOn = gpio4IsLow ? 0 : 1;
//     r.frtRun = (frt.state == FRT_RUNNING) ? 1 : 0;
//     r.pad = 0;
//     r.meterA = NAN;
//     r.estAvg = a.est.avg();    r.estMin = a.est.lo();    r.estMax = a.est.hi();
//     r.dutyAvg = a.duty.avg();  r.dutyMin = a.duty.lo();  r.dutyMax = a.duty.hi();
//     r.vAvg = a.volts.avg();    r.vMin = a.volts.lo();    r.vMax = a.volts.hi();
//     r.rc = g_fieldRcOhm;
//     r.r = FieldResistance;
//     r.trefC = FieldResTrefC;
//     r.probeF = n2kAltTempF();
//     r.dieF = g_fdDieTempF;
//     r.rpm = a.rpm.avg();
//     r.altA = a.altA.avg();
//     r.battA = a.battA.avg();
//     r.curCeil = g_fieldCurDutyCeil;
//     r.heatCeil = g_fieldHeatDutyCeil;
//   };
//   auto fcPush = [](const FcRec &r) {
//     portENTER_CRITICAL(&fcMux);
//     fc->ring[fcHead] = r;
//     fcHead = (uint16_t)((fcHead + 1) % FC_RING_N);
//     if (fcCount < FC_RING_N) fcCount++;
//     portEXIT_CRITICAL(&fcMux);
//   };
//   FcAcc &row = fc->row;
//   FcAcc *sec = fc->sec;
//   static uint8_t secIdx = 0;
//   static uint32_t lastSampleMs = 0, rowStartMs = 0, secStartMs = 0;
//   static bool started = false;
//   if (!started) {
//     rowStartMs = secStartMs = lastSampleMs = now;
//     started = true;
//   }

//   if ((uint32_t)(now - lastSampleMs) >= FC_SAMPLE_MS) {
//     lastSampleMs = now;
//     if ((uint32_t)(now - secStartMs) >= 1000) {
//       secStartMs = now;
//       secIdx = (uint8_t)((secIdx + 1) % FC_SEC_N);
//       sec[secIdx].clear();
//     }
//     const float batt = getBatteryCurrent();
//     FcAcc *dst[2] = { &row, &sec[secIdx] };
//     for (FcAcc *a : dst) {
//       a->est.add(iiout);
//       a->duty.add(dutyCycle);
//       a->volts.add(busV);
//       a->rpm.add(RPM);
//       a->altA.add(MeasuredAmps);
//       a->battA.add(batt);
//     }
//   }

//   uint32_t seq;
//   uint8_t kind;
//   float amps;
//   portENTER_CRITICAL(&fcMux);
//   seq = fcReqSeq;
//   kind = fcReqKind;
//   amps = fcReqAmps;
//   portEXIT_CRITICAL(&fcMux);
//   if (seq != fcDoneSeq) {
//     FcAcc w;
//     w.clear();
//     for (int i = 0; i < FC_SEC_N; i++) w.merge(sec[i]);
//     FcRec r;
//     fcFill(r, kind, now, w);
//     r.meterA = (kind == FC_KIND_METER) ? amps : NAN;
//     fcPush(r);
//     portENTER_CRITICAL(&fcMux);
//     fc->last = r;
//     fcEntries++;
//     fcDoneSeq = seq;
//     portEXIT_CRITICAL(&fcMux);
//     if (kind == FC_KIND_METER)
//       queueConsoleMessageF("Field check: meter %.2f A, estimate %.2f A (%.2f to %.2f over the last 5 s), meter minus estimate %+.2f A",
//                            r.meterA, r.estAvg, r.estMin, r.estMax, r.meterA - r.estAvg);
//     else
//       queueConsoleMessageF("Field check: mark, estimate %.2f A", r.estAvg);
//   }

//   if ((uint32_t)(now - rowStartMs) >= FC_ROW_MS) {
//     rowStartMs = now;
//     FcRec r;
//     fcFill(r, FC_KIND_ROW, now, row);
//     fcPush(r);
//     row.clear();
//   }
// }
// ===== TEMP-FIELDCHECK END =====

void thermalLog_init() {
  Serial.printf("thermalLog_init: PSRAM found=%d\n", psramFound());

  if (!psramFound()) {
    Serial.println("FATAL: PSRAM not found");
    thermalLogReady = false;
    thermalLog = nullptr;
    return;
  }

  thermalLog = (ThermalLogEntry *)heap_caps_malloc(
    THERMAL_LOG_SIZE * sizeof(ThermalLogEntry),
    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

  Serial.printf("thermalLog_init: alloc %d bytes = %p\n",
                THERMAL_LOG_SIZE * sizeof(ThermalLogEntry), thermalLog);

  if (!thermalLog) {
    Serial.println("FATAL: malloc failed");
    thermalLogReady = false;
    return;
  }

  thermalLogReady = true;
  Serial.printf("thermalLog_init: OK, %d entries x %d bytes = %d KB\n",
                THERMAL_LOG_SIZE, sizeof(ThermalLogEntry),
                THERMAL_LOG_SIZE * sizeof(ThermalLogEntry) / 1024);
}

void thermalLog_tick(uint32_t nowMs) {
  if (!loggingActive) return;  // Stop Logs: skip append, freeze buffer
  if (!thermalLogReady || !thermalLog) return;

  // Watchdog: auto-unpause if download stalled/aborted
  if (thermalLogPaused) {
    if ((uint32_t)(nowMs - thermalLogPausedAtMs) > THERMAL_LOG_PAUSE_TIMEOUT_MS) {
      serialPrintlnNB("thermalLog: pause watchdog triggered - connection likely aborted");
      thermalLogPaused = false;
    } else {
      return;
    }
  }

  static uint32_t lastLogMs = 0;
  if ((uint32_t)(nowMs - lastLogMs) < THERMAL_LOG_INTERVAL_MS) return;
  lastLogMs = nowMs;

  ThermalLogEntry &e = thermalLog[thermalLogHead];

  e.ts = nowMs;
  // NAN until the first sane temperature read; INT16_MIN = blank cell (thermalLogScale10 would stamp a fake 0.0F).
  e.tempFiltered = !isfinite(tempFiltered) ? INT16_MIN : thermalLogScale10(tempFiltered);
  e.tempProjected = !isfinite(projectedTempF) ? INT16_MIN : thermalLogScale10(projectedTempF);
  // effective setpoint and cap: mirror tempPID_tick (slopeBufFull or commissioning/tuning = 7°F margin, else 20°F
  // warmup margin; the engine-speed-ignored ceiling when IgnoreRPM)
  {
    float logLimit = TemperatureLimitF;
    const bool logSuppressWarmup = cxOwnsBatteryNow() || (TuningMode != 0);
    float logSetpoint = (thermalSlopeBufFull || logSuppressWarmup) ? (logLimit - 7.0f) : (logLimit - 20.0f);
    e.nominalTarget = thermalLogScale10(logSetpoint);
  }
  e.rpmCap = thermalLogScale10((IgnoreRPM && g_I_cap > 0.0f) ? g_I_cap : getCapCurrentForRPM(RPM));
  e.voltCap = thermalLogScale10(Icv);
  e.uTarget = thermalLogScale10((float)uTargetAmps);
  e.spLimited = thermalLogScale10(setpointLimited);
  e.pidErr = thermalLogScale10(pidError);
  e.pidOut = thermalLogScale10((float)pidOutput);
  e.duty = thermalLogScale10(dutyCycle);
  e.rpm = thermalLogScaleRPM(RPM);
  e.battV = thermalLogScale10(IBV);
  e.measAmps = thermalLogScale10(MeasuredAmps);
  e.penaltyAmps = thermalLogScale10(thermalPenaltyAmps);

  e.flags = 0;
  if (tempPIDActive) e.flags |= (1 << 0);
  if (sysMode == SYS_MODE_AUTO) e.flags |= (1 << 4);
  if (shutdownRampActive()) e.flags |= (1 << 5);

  e.antiWindupFired = outerAntiWindupFired ? 1 : 0;
  e.chargeStageDisplay = thermalLogGetStageCode();
  e.freezeWhy = thermalFreezeReason;

  e.outerTermP = thermalLogScale10(outerTermP);
  e.outerTermI = thermalLogScale10(outerTermI);
  e.outerTermLookahead = thermalLogScale10(outerTermLookahead);
  e.impliedPenalty = thermalLogScale10(outerImpliedPenalty);
  e.thermalSlope = (int16_t)(thermalSlopeFPerSec * 1000.0f);
  e.penaltyRaw = thermalLogScale10(outerPenaltyRaw);
  // Live integral ceiling cap−FF (CSV header iCeil_A, not a hold estimate). iCeil < outerI ⇒ the clamp is deleting earned
  // hold (RPM dip / FF spike) — watch for a reheat on cap recovery.
  e.holdEstimate = thermalLogScale10(thermalIntegralCeil);

  thermalLogHead = (thermalLogHead + 1) % THERMAL_LOG_SIZE;
  if (thermalLogCount < THERMAL_LOG_SIZE) thermalLogCount++;
}
