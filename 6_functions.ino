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

void applyImmediateCut(const TickSnapshot &tick, FieldEventReason reason);
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

// Commissioned field drain time at an engine speed: linear between the two tested endpoints,
// CLAMPED to the tested range — an extrapolated underestimate would release the OV clamp with the
// field still hot. No line commissioned (rpmHi<=rpmLo or a zero endpoint) or RPM unknown (<=0) →
// fieldDecayTauMs, the stored worst-case (longest) endpoint.
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
void kneeLearnResetDefaults();
String kneeLearnStateJson();
// RPM table index (for UI highlighting and bucket history)
void updateCurrentRPMTableIndex(float rpm);
bool shouldImmediatelyCutGPIO4(FieldEventReason reason);
bool shouldCutGPIO4AfterSettle(FieldEventReason reason, uint32_t nowMs, float appliedDuty);
void updateFieldTelemetry(float duty, float voltage, float fieldResistance);
void reportFieldModeEvent(uint32_t nowMs, FieldControlMode mode, FieldEventReason reason,
                          const TickSnapshot &tick, bool gpio4Low, float appliedDuty);

// String conversion for logging
const char *modeToString(FieldControlMode mode);
const char *reasonToString(FieldEventReason r);
// Control loop sub-path helpers
void handleLimpHome(uint32_t currentMillis, const TickSnapshot &tick);
void runShutdownPath(const TickSnapshot &tick, FieldControlMode mode, FieldEventReason reason,
                     float actualDtSec, bool exitingNormal);
void runCommissionIdle(const TickSnapshot &tick, FieldEventReason reason, float actualDtSec);

// FIELD CONTROL MODULE - Refactored with Unified Actuator Governor
// ====================================================================================
//
// NOTE: All enums (GovernorMode, SystemMode), constants (actualDtSec, etc.),
// and global variables (sysMode, govMode, setpointLimited, etc.) are defined
// in the main .ino file. This file contains only function implementations.
//

// ==================== HELPER FUNCTIONS ====================

float clamp_f(float x, float lo, float hi) {
  if (x < lo) return lo;
  if (x > hi) return hi;
  return x;
}

// CV AUTO-tune design constants — see Working Markdown Docs/CV_AUTOTUNE_PLAN.md §E.
// The old λ/SIMC fit (cvPlantTau, cvPlantL, cvLambdaMult) is RETIRED: a real step test showed the
// loop is stability-robust (86–95° phase margin even at the "disaster" gains), so neither phase
// margin nor a precise τ/L fit binds. The only per-install unknown that matters is the DC gain
// K_dc (cvPlantK, settled ΔV/ΔI). Anchor Kp = α/K to the ~0.6 s horizon the loop reacts on (Kp ∝ 1/K_dc);
// this stays well below the polarization pole (~0.7 rad/s) and the dead-time limit, so we never
// need to measure them. α/ρ are bench-tuned by disturbance-rejection (no protection trips), NOT
// to match any prior hand tune — they are the user-adjustable settings cvAlpha / cvPiZero (Tuning ▸
// Voltage; defaults 0.05 / 0.50). cvPlantTau/cvPlantL were fully removed 2026-07-03 (NVS keys
// retired — see 2_functions.ino); the fit confidence record lives in the cvfit.csv download.

// computeCvTempScale — battery-temperature gain derate factor (see the globals block in Xregulator.ino).
// battTempActiveF (°F, the batteryTempF() source of this tick: probe / NMEA 2000 / VE.Direct / RV-C, else
// the board temperature as a stand-in) is the battery temp; the battery's internal resistance — which IS
// the CV plant gain K_dc — rises as it cools, so gains set at the commissioning temperature run too hot
// when colder. Returns R(T_commission)/R(T_now): both ends go through cvResistanceRatio(), so the
// saturation there — not a clamp on the ratio — is what bounds the result. That makes the bite point an
// absolute temperature instead of a function of whatever the source happened to read when the fit was
// applied. Returns 1.0 when disabled, never commissioned, or no source qualifies — never amplifies blindly.
// Source CLASS rule: a measured source (1..4) and the board stand-in (5) read different temperatures for
// the same pack, so the anchor only holds inside the class it was stamped in (CommissionTempSrc, 0 =
// legacy stamp = board). A class change makes the derate inert (1.0, cvTempDerateInert) until the fit is re-run.
// This is a Kp+Ki scale, NOT a λ/ω change — ω and ρ are held fixed.
static const float CVTS_T_MIN_C = -40.0f, CVTS_T_MAX_C = 70.0f;   // BMP388 rated span; also the charging envelope
static const float CVTS_WARM_RATIO = 0.5f;
static const float CVTS_R_HOT = 0.55f, CVTS_R_COLD = 7.0f;
static const float CVTS_SCALE_MIN = 0.10f, CVTS_SCALE_MAX = 2.50f;

// R(T)/R(25°C) for the pack. TWO segments pivoting at 25°C (continuous there by construction): a battery's
// apparent activation energy roughly halves above room temperature — measured 0.026→0.010 /°C for LFP
// (Gotion IFP50160116A charge DCIR) and 0.023→0.015 /°C for lead-acid electrolyte (NBS RP738). One
// exponential misses by up to 30%; this misses by ≤12% across −20…+55°C on BOTH chemistries, which is why
// there is no per-chemistry coefficient. The clamps are backstops against a hand-cranked battTempCoeff, NOT
// physical asymptotes — nothing actually floors: a VRLA's impedance keeps falling as it heats, and that is
// precisely the thermal-runaway mechanism.
static float cvResistanceRatio(float tC) {
  float c = (tC < 25.0f) ? battTempCoeff : (battTempCoeff * CVTS_WARM_RATIO);
  return clamp_f(expf(-c * (tC - 25.0f)), CVTS_R_HOT, CVTS_R_COLD);
}

// tempProbeFresh — idle-aware age gate for the battery / extra temperature feeds, the same shape as the
// tempDataVeryStale rule in buildTickSnapshot: 20 s once the engine has been >= 200 RPM for 15 s, 90 s
// otherwise (TempTask stretches its poll to 60 s with the engine off). A false result never cuts the
// field — every consumer fails open or to neutral.
bool tempProbeFresh(uint8_t idx) {
  static uint32_t lastBelowRunRpmMs = 0;
  uint32_t now = millis();
  if (RPM < 200) lastBelowRunRpmMs = now;
  bool running = (RPM >= 200) && ((uint32_t)(now - lastBelowRunRpmMs) >= 15000UL);
  uint32_t ts = dataTimestamps[idx];
  if (ts == 0) return false;
  uint32_t age = (ts > now) ? 0 : (now - ts);   // a Core-0 stamp can lead this Core-1 clock read by a few ms
  return age <= (running ? 20000UL : 90000UL);
}

// Source codes shared with CSV2 battTempActiveSrc and the UI:
// 0 none, 1 probe, 2 NMEA 2000 (127508), 3 VE.Direct (T field), 4 RV-C (DC_SOURCE_STATUS_2), 5 board temperature
float batteryTempF(uint8_t *srcOut) {
  // Every gate is evaluated each call so tempProbeFresh's idle tracker keeps running whatever the selection.
  bool okProbe = tempProbeFresh(IDX_BATT_TEMP_PROBE) && battTempProbeEnable == 1 && isfinite(BatteryTempProbeF);
  bool okVe    = tempProbeFresh(IDX_VE_BATT_TEMP) && isfinite(VictronBattTempF);
  bool okRvc   = tempProbeFresh(IDX_RVC_BATT_TEMP) && isfinite(rvcRxBattTempF);
  bool okN2k   = !IS_STALE(IDX_N2K_BATT) && isfinite(n2kRxBattTempF);
  bool okBoard = !IS_STALE(IDX_AMBIENT_TEMP) && isfinite(ambientTemp);
  uint8_t src = 0;
  float t = NAN;
  switch (battTempSource) {
    case 0:  // Auto: first qualifying measurement, the board only as an opt-in stand-in
      if (okProbe)      { src = 1; t = BatteryTempProbeF; }
      else if (okN2k)   { src = 2; t = n2kRxBattTempF; }
      else if (okVe)    { src = 3; t = VictronBattTempF; }
      else if (okRvc)   { src = 4; t = rvcRxBattTempF; }
      else if (okBoard && battTempProxyEnable == 1) { src = 5; t = ambientTemp; }
      break;
    case 1: if (okProbe) { src = 1; t = BatteryTempProbeF; } break;
    case 2: if (okN2k)   { src = 2; t = n2kRxBattTempF; }   break;
    case 3: if (okVe)    { src = 3; t = VictronBattTempF; } break;
    case 4: if (okRvc)   { src = 4; t = rvcRxBattTempF; }   break;
    case 5: if (okBoard) { src = 5; t = ambientTemp; }      break;
    default: break;  // 6 = none
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
    case 5: return "board temperature";
    default: return "none";
  }
}

float computeCvTempScale() {
  cvTempDerateInert = false;
  if (!battTempDerateEnable) return 1.0f;
  if (isnan(CommissionTempF)) return 1.0f;
  if (battTempActiveSrc == 0 || !isfinite(battTempActiveF)) return 1.0f;
  bool nowMeasured  = (battTempActiveSrc >= 1 && battTempActiveSrc <= 4);
  bool commMeasured = (CommissionTempSrc >= 1 && CommissionTempSrc <= 4);
  if (nowMeasured != commMeasured) { cvTempDerateInert = true; return 1.0f; }
  float tCommC = clamp_f((CommissionTempF  - 32.0f) / 1.8f, CVTS_T_MIN_C, CVTS_T_MAX_C);
  float tNowC  = clamp_f((battTempActiveF - 32.0f) / 1.8f, CVTS_T_MIN_C, CVTS_T_MAX_C);
  // Asymmetric: a boost raises loop gain, and an over-large coefficient inflates it further, so the boost
  // is capped tighter than the cut. It only bites below ~0°C commissioning.
  return clamp_f(cvResistanceRatio(tCommC) / cvResistanceRatio(tNowC), CVTS_SCALE_MIN, CVTS_SCALE_MAX);
}

// recomputeCvGains — derive VoltageKp_active/VoltageKi_active from the gain mode + measured stiffness.
// Call after any related setting change, on battery-temp drift (board or measured source), and at boot. Computed in 12V-equivalent
// space (same numbers on 12/24/36/48 V), then ×vNorm bakes back to pack space; the battery-temp derate is
// the final multiplier in BOTH modes (the plant shifts with temperature however the gains were chosen).
void recomputeCvGains() {
  float vNorm = 12.0f / (float)SYSTEM_VOLTAGE_CLASS;     // 1, 0.5, 0.33, 0.25 for 12/24/36/48 V
  cvPlantK = cvPlantKa;                             // the ~0.6 s stiffness anchor; the √t-tail (cvPlantKb) is retired
  bool plantValid = (cvPlantK > 1e-6f);
  float kpNorm, kiNorm, kdNorm;                     // 12V-equivalent gains (what the user sees)
  if (cvGainMode == 1 && plantValid) {
    // AUTO: Kp = α/K anchored to the ~0.6 s stiffness — the timescale the loop reacts on
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
    // MANUAL, or AUTO with no valid fit yet → use the typed / conservative gains.
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
  // D gain normalizes identically to P/I (×vNorm×derate), so one number works on 12/24/36/48 V and tracks the
  // battery-temp derate. In Auto it is the plant-anchored Td·Kp; in Manual it is the typed VoltageKd.
  VoltageKd_active = kdNorm * vNorm * cvTempDerateScale;
}

// recomputeCcGains — CC (output-current) analog of recomputeCvGains. PidKp/Ki/Kd are 12V-equivalent;
// ×(12/SYSTEM_VOLTAGE_CLASS) bakes them into the duty-space gains the inner current PID actually applies,
// so one set of tunings behaves identically on 12/24/36/48 V (field current per duty-% scales with bus
// voltage). Call after any PidK* change and after a SYSTEM_VOLTAGE_CLASS change. currentPID is a global
// object (constructed before setup), so SetTunings is safe to call unconditionally. The hunt
// governor's Ki derate applies here — the single SetTunings choke point — so every recompute path
// (setting change, class change, governor step) preserves it.
void recomputeCcGains() {
  float vNorm = 12.0f / (float)SYSTEM_VOLTAGE_CLASS;     // 1, 0.5, 0.33, 0.25 for 12/24/36/48 V
  PidKp_active = PidKp * vNorm;
  PidKi_active = PidKi * vNorm;
  PidKd_active = PidKd * vNorm;
  currentPID.SetTunings(PidKp_active, PidKi_active * g_huntDerate, PidKd_active);
}

// ==================== HUNT GOVERNOR v2 ====================
// Persistent speed-map oscillation damper (HUNT_GOVERNOR_V2_SPEC.md). A marginally-stable loop
// meeting a speed-rotated plant (idle knee: each %duty sags rpm, the sag cancels the field gain →
// plant ~quadrature) rings at 0.3–3 Hz. v2: detect the symptom while engine speed is steady, make
// ONE cut to HuntCutPct, and keep it ONLY if averaged ripple drops ≥ HuntVerifyPct — a verified cut
// becomes a persistent speed pocket applied proactively on every future pass through that speed.
// Repeat verifications widen a pocket, never deepen it; a wobble persisting inside a pocket at the
// floor is reported as external/mechanical, not cut further.
#define HG_RING_N 128            // 6.4 s at 50 ms — two periods of the lowest bin (hard floor for 0.31 Hz selectivity)
#define HG_EVAL_EVERY 32         // evaluate every 1.6 s (window/4)
#define HG_NBINS 6
// Detection amplitude bar is the HuntTrigPct setting (default 0.5% duty). It has no upper clamp:
// the duty swing a given wobble produces scales with 12/SYSTEM_VOLTAGE_CLASS, so a 48 V install
// needs a lower bar for the same physical hunt. Bench measurement 08-21 (pidlog_20260821_212919):
// a real 0.70 Hz idle hunt read 0.39–0.62% peak-bin duty against the old fixed 0.5, so only 2 of 6
// scans qualified and HuntQualifyScans in a row never happened — the miss this setting exists for.
// KNOWN GAP (2026-08-22, unstudied): this bar is NOT class-scaled x12/V like every other duty-domain knob, so on a 24/36/48V install the same physical wobble may never reach it and the damper never fires at all — needs study before trusting v2 above 12V. See HUNT_GOVERNOR_V2_SPEC.md "Higher-voltage installs".
#define HG_TRIG_RATIO 3.0f       // peak vs median of other bins — rejected a 2.22% broadband load transient amplitude alone would have flagged
#define HG_TRIG_D_FRAC 0.3f      // relaxed duty bar for a D-attributed scan, as a FRACTION of HuntTrigPct (0.3 x 0.5 = the 0.15 the D lever was built against): the D-driven mode lives in VOLTS and its duty amplitude can sit below the quiet-log floor (08-21: 0.27% duty, 0.33 V p-p). Safe below the 0.4 quiet median ONLY because the kdTrim alternation requirement is the real gate — a quiet log has no alternations. Proportional, not fixed: a fixed 0.15 would become STRICTER than the standard bar the moment HuntTrigPct is set below it
// Qualify-scan count is the HuntQualifyScans setting (default 3): consecutive hunt+steady scans to
// open a test; baseline = their average (the HuntVerifyPct bar needs an averaged reference —
// single windows jitter more than 15% on their own)
#define HG_BLIND_SCANS 4         // clean scans discarded after the cut while the 6.4 s window refills with post-cut data
#define HG_READ_SCANS 3          // clean scans averaged for the verify reading
#define HG_EP_TIMEOUT_SCANS 25   // ~40 s — contamination can freeze clean-scan progress; bail with nothing kept
#define HG_RPM_MIN 100.0f        // below this there is no usable speed axis (no tach signal) — the damper stays inert
#define HG_CV_COVER_MIN 0.75f    // share of a scan's ticks that must be in voltage-regulated mode (CV) before that scan's |cvDSlope| mean counts as evidence. The metric only accumulates in CV, so an uncovered scan averages to 0.0 and would clear ANY improvement bar on no data at all. 0.75 of a 1.6 s scan over HG_READ_SCANS gives >= 3.6 s of CV — longer than one period of the slowest bin (0.31 Hz), so the average cannot sit entirely inside a quiet phase of the cycle
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
static float hgSlopeSum = 0.0f;  // Σ|cvDSlope| over the scan (drained each evaluation) — the D-test verify metric: the D-driven mode lives in VOLTS (its duty amplitude can sit below the quiet-log Goertzel floor), and |dV/dt| at a fixed ring frequency is a voltage amplitude proxy
static uint16_t hgSlopeCnt = 0;
static uint16_t hgKdFlips = 0;   // kdTrim sign alternations since the last scan (drained each evaluation) — D-lever attribution
static int8_t hgKdSign = 0;      // last nonzero kdTrim sign (zero spans between engagements don't break an alternation)

// Per-control-tick sampler: ONE ring write plus the contamination flags — the analysis never runs
// here. Self-clocked to ~50 ms so changes in loop cadence don't move the
// analysis band; Goertzel coefficients derive from the measured dt EMA at evaluation time.
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
  // D-lever attribution (08-21 D-driven variant): a healthy D-term fires in brief one-sided bursts
  // that decay within a second or two; sustained periodic sign ALTERNATION around zero mean is a
  // D-term doing no net work — either useless or driving a limit cycle. 50 ms sampling is enough:
  // the last-nonzero-sign compare catches an alternation even when the zero span between
  // engagements is sampled.
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
                || spDev > fmaxf(3.0f, 0.04f * (float)AlternatorNominalAmps);
  if (contam) hgContamTick = hgTick;
}

// ── Episode ledger ────────────────────────────────────────────────────────────────────────
// The verdict fires inside the control path, so it must NEVER touch flash: a LittleFS append can
// erase a sector and stall tens of ms, and fsExists() alone can block up to 5 s on the FS mutex.
// Verdicts therefore only push a record into this RAM queue (O(1), no allocation); huntLedgerService()
// does every byte of flash work, and the main loop calls it under the same field-off-settled gate
// the commissioning ledger uses. /huntledger serves file + queue so a Refresh sees both.
struct HgLedgerRec {
  uint32_t epoch;
  float freqHz, a0, aEnd, derate;
  int16_t rpm;
  uint8_t cv, steps;
  char verdict[20];
};
#define HG_LEDGER_QUEUE_N 12       // episodes that can wait in RAM for the engine to stop; past this the oldest are counted and dropped
#define HG_LEDGER_MAX_BYTES 6144   // hard file cap (~100 rows). At the cap the oldest half is dropped, so the ledger can never grow without bound
static const char *HG_LEDGER_HDR = "epoch,rpm,cv,freqHz,a0_pct,aEnd_pct,steps,verdict,derateExit";
static HgLedgerRec hgQueue[HG_LEDGER_QUEUE_N];
static uint8_t hgQueueN = 0;
static uint16_t hgQueueDropped = 0;

static void hgLedgerQueue(const char *verdict, float freqHz, float a0, float aEnd, uint8_t steps) {
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

// Render the queued (not yet on flash) records as ledger CSV lines. Read-only — used by the
// /huntledger handler so the UI never has to wait for a flush to see a fresh episode.
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
        File w = LittleFS.open("/huntledger.csv", "w");
        if (w) {
          w.println(HG_LEDGER_HDR);
          if (kept) w.write((uint8_t *)tmp, kept);
          w.close();
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
// A pocket is a flat core [lo,hi] rpm held at HuntCutPct gain, with linear taper wings spanning
// HuntWingPct of speed beyond each end. RAM is authoritative; /huntpockets.csv is the boot-restore
// copy, written only by huntMapService() under the same field-off gate as the episode ledger (no
// flash in the control path). Absolute RPM scale error is irrelevant — the map compares the
// device's own readings against its own readings — but a recalibration rescales the axis, so
// rpmAxisWipeExecute clears the map along with every other RPM-keyed artifact.
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

// Applied gain at speed r for one lever: min across that lever's pockets of (the lever's cut
// inside the core, linear ramp across the wings, full gain outside). Continuous in r, so there
// is no edge to flap across.
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

// CV D-term multiplier. The D cut is 0, not HuntCutPct: the deadband limit cycle parks where its
// amplitude-dependent gain reaches |L|=1, so a PARTIAL D cut only moves the equilibrium to a
// LARGER amplitude — the lever is binary off (08-21 measurement: |L|=1.00 with D, 0.45 without).
float huntMapGainD(float rpm) {
  return hgMapGainLever(rpm, 1, 0.0f);
}

// Fold a verified speed into the map: extend the pocket whose profile already covers it, else a
// new pocket (evicting the least-recently-confirmed at the cap), then merge overlapping pockets.
// Widening only — depth is fixed per lever (HuntCutPct for Ki, 0 for D). Matching and merging are
// same-lever only: a Ki pocket and a D pocket overlapping in speed are independent rules.
static void huntMapAdd(float rpm, uint8_t lever) {
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

// The ONLY code that writes the map to flash. Main loop, same field-off-settled call site as
// huntLedgerService.
void huntMapService() {
  if (!hgMapDirty) return;
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

// JSON for the /huntmap endpoint — the live map the Diag plot draws. Cut/wing ride along so the
// client renders exactly the profile the firmware applies, atomically with the pockets.
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

// Wipes everything the damper has learned: pockets, episode ledger, queued records, any test in
// flight. Callers: /huntclear, the voltage-class change, rpmAxisWipeExecute — all flash-safe
// contexts (HTTP task / Core 1), never the control path.
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

// Abort an episode in flight: restore BOTH levers to the map BEFORE queuing so the record's
// derateExit is the true exit gain, then queue a lever-aware record. A D-rung abort carries the
// V/s movement baseline and its own verdict token, matching damped-dterm / dterm-no-resp — the
// plain "aborted" shape is a duty swing and the web list renders it as a gain cut. aEnd 0.0 means
// "no post reading exists", never a measured zero.
static void hgEpisodeAbort() {
  bool dRung = (g_huntState == 4);
  g_huntDerate = huntMapGain(hgRpmMapEma);
  recomputeCcGains();
  g_huntKdScale = huntMapGainD(hgRpmMapEma);
  hgLedgerQueue(dRung ? "aborted-dterm" : "aborted", g_huntFreqHz, dRung ? hgSlope0 : hgEpA0, 0.0f, 1);
  g_huntState = 0;
}

// Arm a test: zero the episode accumulators, take the baselines the verdict is measured against,
// pin ONE lever at its cut, and enter that rung. state 4 pins the CV D-term and ignores kiCut;
// state 1 pins the inner Ki gain, which is what the recompute is for. refRpm is the episode's
// steadiness reference — the chain out of a failed D test passes the D stage's own reference back
// in, so one reference spans both rungs. hgEpVoltsMode / hgQualCvOk describe the qualify span, not
// the rung, and must stay untouched here.
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

// Evaluation + state machine. Called every loop() pass from Xregulator.ino; self-gates to one
// Goertzel pass (6 bins × 128 samples, tens of µs) per HG_EVAL_EVERY control ticks. v2 flow
// (HUNT_GOVERNOR_V2_SPEC.md): qualify (HG_QUALIFY_SCANS hunt+steady scans) → one cut to
// HuntCutPct → HG_BLIND_SCANS while the window refills → HG_READ_SCANS averaged → verdict
// (≥ HuntVerifyPct averaged drop maps a pocket; anything else reverts, cooldown). Outside a
// test, gain simply follows the pocket map at the current smoothed speed.
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

  // Watching: qualification. HG_QUALIFY_SCANS consecutive scans of hunt signature at a steady
  // speed open a test; speed reference is the first confirming scan's mean. A scan qualifies on
  // the duty signature OR on the D-attributed form: when the D-term is sign-alternating, the duty
  // amplitude bar drops to HuntTrigPct x HG_TRIG_D_FRAC and the peakiness ratio relaxes to 2x — the alternation
  // itself is the periodicity evidence (broadband load noise never alternates the trim).
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
      // Lever choice by attribution: the qualify span must show at least ~half the sign
      // alternations the detected frequency would produce (2 per period; floor 3). An attributed
      // episode tests the D lever FIRST — pausing the participant is directly causal, and the
      // inner-Ki lever is three-for-three falsified on outer-loop-driven variants (idle-hunt
      // memory 07-24/08-21). D inside a D-pocket (scale already ~0) can't attribute: no trim, no flips.
      uint16_t flipBar = (uint16_t)fmaxf(3.0f, HG_BIN_HZ[pk] * nq * 1.6f);
      hgEpVoltsMode = (a0 < HuntTrigPct);
      bool dAttributed = (hgQualFlips >= flipBar) && (g_huntKdScale > 0.05f);
      if (!hgQualCvOk && (dAttributed || hgEpVoltsMode)) {
        // Both slope-judged rungs verify against hgSlope0, which is these qualify scans' average —
        // without CV coverage it is junk, so the episode could only ever produce a garbage verdict.
        // Refuse to open rather than open one that cannot be judged; no cooldown, so a later pass
        // with real CV coverage still gets tested.
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

// Derived duty ceiling from Max Field Volts, refreshed by updateFieldVoltCeiling() below. Held in a
// global rather than solved inside ccDutyCeiling() because that function is called from the tight
// paths and must stay a cheap read, and because the PID's output limits are pushed on a deadband
// (see updateFieldVoltCeiling) — every reader has to see the same value the PID was last told.
static float g_fieldVoltDutyCeil = 99.0f;
static float g_fieldVoltBusFilt  = 0.0f;   // slow-filtered bus volts the ceiling is solved against; 0 = not yet seeded
static bool  g_fieldVoltFloored  = false;  // one-shot console warn latch for the MinDuty collision below

// ccDutyCeiling — the upper duty bound the CC loop is allowed to reach: the lower of the two field
// caps. MaxDuty (Max Field %) is the REAL per-bus duty cap — its default is scaled down on
// higher-voltage banks (~50%@24V, 25%@48V) and rescaled on a voltage change, so the user-visible Max
// Field box IS the cap the loop respects, no hidden ×12/Vbatt. g_fieldVoltDutyCeil is Max Field Volts
// solved against the measured bus, which is the same limit in the units on the alternator datasheet.
// A user wanting full duty sets Max Field to 99 AND leaves Max Field Volts above the bank's absorb
// voltage (its class-scaled default already is, so the volts term is inert until deliberately lowered).
// Everything that asks "how high may duty go" must come through here — a raw MaxDuty read silently
// skips the volts cap. MaxDuty itself stays the persisted setting and the class-scaling target.
float ccDutyCeiling() {
  return fminf(MaxDuty, g_fieldVoltDutyCeil);
}

// updateFieldVoltCeiling — re-solve the volts cap into duty against the measured bus, once per control
// tick. Called from buildTickSnapshot so it leads every consumer in the same tick.
//
// The bus is filtered (2s) for two reasons: the cap must not chase ripple or a load-dump transient into
// the actuator, and pushing SetOutputLimits is not free — it re-clamps the live output AND the
// integrator, which is the wanted anti-windup on a discrete ceiling change but would make the ceiling
// a moving actuator if done at tick rate. Hence the 0.5-point deadband: the PID only hears about a
// ceiling that actually moved.
void updateFieldVoltCeiling(float busV, float dtSec) {
  if (!(busV > 1.0f) || isnan(busV)) return;           // sensor dropout: hold the last ceiling rather than open the cap
  if (g_fieldVoltBusFilt <= 0.0f) g_fieldVoltBusFilt = busV;   // seed on first valid reading, no ramp-in
  else {
    float a = constrain(dtSec / 2.0f, 0.0f, 1.0f);
    g_fieldVoltBusFilt += a * (busV - g_fieldVoltBusFilt);
  }

  float derived = 100.0f * MaxFieldVolts / g_fieldVoltBusFilt;

  // Floor at MinDuty. Below it the [MinDuty, ceiling] range inverts and the per-RPM onset floors
  // collapse unsatisfiable — the same silent failure applyNominalVoltageChange guards against on a
  // class change. A volts cap that low is a misconfiguration, so say so once rather than fail quietly.
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

// applyCcOutputLimits — set the inner current PID's output bounds to [MinDuty, ccDutyCeiling()]. Making
// the ceiling the PID's REAL upper limit lets its anti-windup track it: otherwise the loop would wind
// the integrator toward MaxDuty while setDutyPercent() clamps the applied duty far lower, then lag on
// the way down. setDutyPercent() still applies the same clamp as the catch-all for the open-loop paths
// (manual/limp/fault) that bypass the PID. Call on AUTO entry, on a ceiling toggle, and on a voltage
// change — SetOutputLimits also re-clamps the live output + integrator, which is the desired anti-windup.
void applyCcOutputLimits() {
  currentPID.SetOutputLimits((double)MinDuty, (double)ccDutyCeiling());
}

// applyNominalVoltageChange — single entry point for a system-voltage class change (12/24/36/48 V).
// Two callers: the Vessel Info save, and applyImportConfig when an imported/pushed payload carries a
// different class (SYSTEM_VOLTAGE_CLASS is the sole source of truth). On the import path it runs BEFORE
// the manifest loop, so a full export's own values then overwrite the conversion key by key while a
// one-key class push keeps it. Call AFTER setting SYSTEM_VOLTAGE_CLASS = newV. When the class actually changes it persists the new class to NVS
// (NK_BatteryVoltage), then rescales the PERSISTED
// charge-voltage profile by newV/oldV (Bulk/Float/Absorption/Rebulk/Target/Charged/alarms), the
// volt-domain protection/helper margins and V/s rates (OV margins incl. the timed-tier trip lines, Max Field Volts,
// disagreement threshold, iExcess arm margin, fast-rise headroom, CV D-term arm/deadband thresholds, target ramps,
// rest-settle gate, CV wave amplitude, alt-health Vbus band), rescales both absolute OV rungs
// (AlternatorHardShutdownV, VoltageHardwareLimit) x ratio and reprograms the INA228 comparator. Writing the
// class and the rescaled profile to NVS in the SAME call (synchronously) keeps them atomic — a reboot
// mid-change can never strand the overvoltage trips at the wrong voltage. It always re-derives
// both control loops' normalized gains. It also rescales the field-duty knobs (knee margin/step/
// maxfloor + DutyRampRate + DutySlowRampRate + MaxDuty/Max Field % + MinDuty + alt-health duty
// band/floor) and the amp-per-volt gain KHard
// (bank resistance rises with class, so the same per-cell excess needs the same amp response) in place
// by oldV/newV and persists them, so they stay WYSIWYG in real per-bus units
// (the live paths do not multiply by 12/Vbatt at use; nothing reads SYSTEM_VOLTAGE_CLASS at duty-clamp
// time). Currents/times/normalized gains are voltage-independent. VoltageKd is NOT here — it is
// runtime-normalized like VoltageKp/Ki (recomputeCvGains below re-derives VoltageKd_active for the new
// class). CvKdMaxTrimA is NOT here either — a flat amp cap, voltage-independent by design.
// The per-RPM Min% floor table is the one commissioned artifact this RESETS rather than rescales
// (to the 1% default, knee tracker unlearned) — see the block at the end of the function.
void applyNominalVoltageChange(int oldV, int newV) {
  if (newV != oldV && oldV > 0 && (newV == 12 || newV == 24 || newV == 36 || newV == 48)) {
    settingWrite(NK_BatteryVoltage, String(newV).c_str());  // persist class FIRST, same transaction as the profile below
    float ratio = (float)newV / (float)oldV;
    BulkVoltage           *= ratio;
    FloatVoltage          *= ratio;
    AbsorptionVoltage     *= ratio;
    RebulkVoltage         *= ratio;
    TargetVoltageSetpoint *= ratio;
    ChargedVoltage_Scaled = (int)lroundf(ChargedVoltage_Scaled * ratio);
    VoltageAlarmHigh      *= ratio;
    VoltageAlarmLow       *= ratio;
    MaxFieldVolts         *= ratio;   // Max Field Volts: volt-domain, NOT the duty-domain group below
    altSweepMarginV       *= ratio;   // sweep turnaround margin: volt-domain, RAM-only (posted per run)
    // Headroom scales with class so the OV ladder keeps its order (G2 clamp at target+OvMeasMarginV
    // < the timed tiers < the absolute cuts) — all per-cell-equivalent. The two absolute rungs
    // scale x ratio like every other volt-domain setting (chain-preserving: software cut stays the
    // same per-cell distance under the INA228 rung); updateINA228OvervoltageThreshold below
    // reprograms the comparator from the scaled VoltageHardwareLimit.
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
    CvKdSlopeCeil             *= ratio;  // V/s real per-bus — class-change scale, now conforms to this function's WYSIWYG rule (no ×V/12 left at use)
    vTgtRampUp                *= ratio;  // V/s
    vTgtRampDn                *= ratio;  // V/s
    cvWindDownStopV           *= ratio;  // V real per-bus — wind-down stop margin scales with class
    capSettleRateMv10         *= ratio;  // mV/10min rest-settle gate
    cvWaveAmplitudeV          *= ratio;  // CV waveform-test step height
    altVbusTol                *= ratio;  // alt-health bus-voltage steadiness band
    Ymin2                     *= ratio;  // voltage-plot axis window follows the bus
    Ymax2                     *= ratio;
    cvPlantKa                 *= ratio;  // Auto-gain V/A anchor is per-bus stiffness; recomputeCvGains below re-derives from it (unscaled it leaves Auto Kp/Ki newV/oldV too hot)
    // Knee duty-domain knobs are stored in REAL duty-% for the bus, so rescale them by the INVERSE
    // ratio (oldV/newV): a 5% margin at 12V becomes 1.25% at 48V. Persist so the dashboard box shows
    // the new value — the math is visible, never hidden behind a runtime multiply.
    float dutyRatio = (float)oldV / (float)newV;
    kneeMarginPct   *= dutyRatio;
    kneeStepPct     *= dutyRatio;
    kneeMaxFloorPct *= dutyRatio;
    kneeDutyTolPct  *= dutyRatio;   // field-duty steadiness band — inverse-scale like altDutyLineRms
    DutyRampRate    *= dutyRatio;
    DutySlowRampRate *= dutyRatio;
    MaxDuty          = (int)lroundf(MaxDuty * dutyRatio);  // Max Field %: real per-bus cap, scales down on higher banks
    MinDuty         *= dutyRatio;  // field floor: float, keeps sub-1% resolution on higher banks
    KHard           *= dutyRatio;  // A per V of OV excess
    altDutyLineRms  *= dutyRatio;  // alt-health field-duty straightness limit (% points)
    altDutySlewMax  *= dutyRatio;  // …and its slew cap (% points per second) — same duty domain
    altMinDuty      *= dutyRatio;  // alt-health admission duty floor
    settingWrite(NK_kneeMarginPct,   String(kneeMarginPct, 2).c_str());
    settingWrite(NK_kneeStepPct,     String(kneeStepPct, 2).c_str());
    settingWrite(NK_kneeMaxFloorPct, String(kneeMaxFloorPct, 2).c_str());
    settingWrite(NK_kneeDutyTolPct,  String(kneeDutyTolPct, 2).c_str());
    settingWrite(NK_DutyRampRate,    String(DutyRampRate, 1).c_str());
    settingWrite(NK_DutySlowRampRate, String(DutySlowRampRate, 2).c_str());
    settingWrite(NK_MaxDuty,         String(MaxDuty).c_str());
    settingWrite(NK_MinDuty,         String(MinDuty, 2).c_str());
    settingWrite(NK_KHard,           String(KHard, 1).c_str());
    // Alt-health registry knobs persist under their registry names (NVS key = name), and the
    // CV wave amplitude under its 15-char NK_ key.
    settingWrite("altVbusTol",    String(altVbusTol, 4).c_str());
    settingWrite("altDutyLineRms", String(altDutyLineRms, 4).c_str());
    settingWrite("altDutySlewMax", String(altDutySlewMax, 4).c_str());
    settingWrite("altMinDuty",    String(altMinDuty, 4).c_str());
    settingWrite(NK_cvWaveAmplitudeV, String(cvWaveAmplitudeV, 2).c_str());
    settingWrite(NK_BulkVoltage, String(BulkVoltage, 2).c_str());
    settingWrite(NK_FloatVoltage, String(FloatVoltage, 2).c_str());
    settingWrite(NK_AbsorptionVoltage, String(AbsorptionVoltage, 2).c_str());
    settingWrite(NK_RebulkVoltage, String(RebulkVoltage, 2).c_str());
    settingWrite(NK_TargetVoltageSetpoint, String(TargetVoltageSetpoint, 2).c_str());
    settingWrite(NK_ChargedVoltage, String(ChargedVoltage_Scaled).c_str());
    settingWrite(NK_VoltageAlarmHigh, String(VoltageAlarmHigh, 2).c_str());
    settingWrite(NK_VoltageAlarmLow, String(VoltageAlarmLow, 2).c_str());
    settingWrite(NK_MaxFieldVolts, String(MaxFieldVolts, 1).c_str());
    settingWrite(NK_AlternatorHardShutdownV, String(AlternatorHardShutdownV, 2).c_str());
    settingWrite(NK_VoltageHardwareLimit, String(VoltageHardwareLimit, 2).c_str());
    settingWrite(NK_OvMeasMarginV, String(OvMeasMarginV, 3).c_str());
    settingWrite(NK_OvPredMarginV, String(OvPredMarginV, 3).c_str());
    settingWrite(NK_OvTierLoMarginV, String(OvTierLoMarginV, 3).c_str());
    settingWrite(NK_OvTierMidMarginV, String(OvTierMidMarginV, 3).c_str());
    settingWrite(NK_dvccCvlMin, String(dvccCvlMin, 2).c_str());
    settingWrite(NK_dvccCvlMax, String(dvccCvlMax, 2).c_str());
    settingWrite(NK_VoltageDisagreeThreshold, String(VoltageDisagreeThreshold, 2).c_str());
    settingWrite(NK_IExcessArmMarginV, String(IExcessArmMarginV, 3).c_str());
    settingWrite(NK_FastSetpointRiseHeadroomV, String(FastSetpointRiseHeadroomV, 2).c_str());
    settingWrite(NK_CvKdArmV, String(CvKdArmV, 2).c_str());
    settingWrite(NK_CvKdDeadbandVps, String(CvKdDeadbandVps, 3).c_str());
    settingWrite(NK_CvKdDbSlope, String(CvKdDbSlope, 5).c_str());
    settingWrite(NK_CvKdDbFloor, String(CvKdDbFloor, 3).c_str());
    settingWrite(NK_CvKdDbCeil, String(CvKdDbCeil, 3).c_str());
    settingWrite(NK_CvKdSlopeCeil, String(CvKdSlopeCeil, 1).c_str());
    settingWrite(NK_vTgtRampUp, String(vTgtRampUp, 3).c_str());
    settingWrite(NK_vTgtRampDn, String(vTgtRampDn, 3).c_str());
    settingWrite(NK_cvWindDownStopV, String(cvWindDownStopV, 3).c_str());
    settingWrite(NK_capSettleRate, String(capSettleRateMv10, 2).c_str());
    settingWrite(NK_Ymin2, String(Ymin2).c_str());
    settingWrite(NK_Ymax2, String(Ymax2).c_str());
    settingWrite(NK_cvPlantKa, String(cvPlantKa, 5).c_str());
    // Min% floor table: RESET, never rescaled. The duty-knee is class-invariant by design (see
    // rpmMinDutyTable in Xregulator.ino — the field-strength and rectifier-threshold effects
    // cancel), but MaxDuty just moved by 12/newV as a real per-bus FIELD-CURRENT cap, so floors
    // learned on the old class can now sit at or above the new cap. Two things break there: the
    // floor outranks the cap in the duty path, and the tach-lie arm bar
    // (rpmMinDuty + frac x (MaxDuty - rpmMinDuty)) collapses to an unsatisfiable value, silently
    // disarming the phantom-RPM cut. The re-commission flag is only an advisory nag — it clears no
    // commissioned value — so the reset has to happen here. A class change also means a different
    // alternator, which invalidates the learned knee anyway. Back to the flat baseline (behaves
    // as MinDuty alone) with the knee tracker unlearned; bin 0 keeps its permanent 0% lock while
    // the learner owns the table, matching what the boot rebuild in kneeLearnInit produces.
    for (int i = 0; i < RPM_TABLE_SIZE; i++) {
      kneeKnee[i] = 0; kneeFrozen[i] = false; kneeLearnTempF[i] = 0; kneeLastMs[i] = 0;
      kneeFloor[i] = (i == 0) ? 0.0f : defaultMinDutyValues[i];
      rpmMinDutyTable[i] = kneeLearnEnable ? kneeFloor[i] : defaultMinDutyValues[i];
    }
    kneeFitA = 0.0f; kneeFitC = 0.0f;   // no commissioning fit any more -> live correction reverts to whole-knee
    kneeFitResidPct = -1.0f; kneeFitWorstIdx = -1;   // -1/-1 = "no fit", same sentinel the wizard's clear path writes
    saveKneeLearnState();
    {
      nvs_handle_t nvs_h;
      if (nvs_open("learning", NVS_READWRITE, &nvs_h) == ESP_OK) {
        nvs_set_blob(nvs_h, "minDutyTable", rpmMinDutyTable, sizeof(rpmMinDutyTable));
        nvs_commit(nvs_h);
        nvs_close(nvs_h);
      }
    }
    // %.0f for MaxDuty: it is a float, and a float in a %d slot reads garbage bits.
    queueConsoleMessageF("System voltage %dV -> %dV: tachometer keep-alive floors reset to the %.2f%% default and their learning cleared (old floors were %dV-referenced and can exceed the new %.0f%% Max Field cap). Re-run the Keep-Alive Floor step.",
                         oldV, newV, KNEE_BASELINE_PCT, oldV, MaxDuty);
    huntMapClearAll("system voltage class changed");  // pockets learned on the old class describe a different alternator
    updateINA228OvervoltageThreshold();
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

/**
 * slew_limit_f - Asymmetric slew limiter for floats
 * @param prev     Previous value
 * @param target   Desired value
 * @param rise_per_s  Max rise rate (units/sec)
 * @param fall_per_s  Max fall rate (units/sec)
 * @param dt_sec   Time step (seconds)
 * @return         Slew-limited value
 */
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

/**
 * apply_pwm_float - Write PWM duty to hardware
 * This is the ONLY function that should call setDutyPercent()
 * (except for hard-cut which bypasses everything)
 */
void apply_pwm_float(float dutyPercent) {
  setDutyPercent(dutyPercent);
}



// ==================== ACTUATOR GOVERNOR ====================
/**
 * governor_apply - Central actuator control
 * 
 * ALL duty changes flow through here (except hard-cut).
 * Applies: clamps → slew (or bypass) → PWM write
 * 
 * @param lastAppliedDuty  Previous applied duty (float)
 * @param requestDutyFloat Requested duty from PID/manual/fault
 * @param gmode            Governor mode (slew/bypass/hold)
 * @param effectiveMinDuty RPM-dependent minimum; 0.0 = floor bypassed entirely (shutdown ramps,
 *                         sysID, an active protection clamp, and MANUAL field mode)
 * @param writeToHardware  false if GPIO4 already LOW (skip PWM write)
 * @return                 The actual duty applied (float)
 */
float governor_apply(float lastAppliedDuty, float requestDutyFloat, int gmode,
                     float effectiveMinDuty, bool writeToHardware, float dtSec) {

  float hardMin = MinDuty;
  float hardMax = ccDutyCeiling();

  // Dynamic bounds (RPM minimum, etc.)
  float dynMin = effectiveMinDuty;
  float dynMax = ccDutyCeiling();

  // Effective bounds: most restrictive
  // For shutdown (effectiveMinDuty=0), allow going below MinDuty
  float finalMin, finalMax;
  if (effectiveMinDuty < 0.01f) {  // Essentially zero (shutdown mode)
    // Shutdown mode: allow 0 to MaxDuty
    finalMin = 0.0f;
    finalMax = (dynMax < hardMax) ? dynMax : hardMax;
  } else {
    // Normal mode: enforce RPM minimum
    finalMin = (dynMin > hardMin) ? dynMin : hardMin;
    finalMax = (dynMax < hardMax) ? dynMax : hardMax;
  }

  // Sanity check
  if (finalMax < finalMin) {
    finalMax = finalMin;
  }

  g_fieldDutyFloor = finalMin;   // what was really enforced, for CSV2_fieldDutyFloor — 0 on the bypass paths

  float requestClamped = clamp_f(requestDutyFloat, finalMin, finalMax);

  // Apply governor mode
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
      // Slew from last applied duty. DutyRampRate is stored in REAL %/s for this bus (its default was
      // scaled by 12/Vbatt at first boot and it's rescaled in place on a voltage change), so it's used
      // as-is here — WYSIWYG with the dashboard box, no hidden runtime multiply.
      if (!dutySlewEnable && !g_autoTestActive) {
        // Duty slew limiter disabled (Test Limiters, both tabs) → duty steps instantly. For A/B study;
        // removes the coupling-cap transient protection, so leave ON in normal operation. Inert during an
        // automated test (g_autoTestActive) so a user's OFF can't strip duty slew out of a measurement.
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
/**
 * enter_sys_off - Transition to OFF mode
 */
void enter_sys_off() {
  sysMode = SYS_MODE_OFF;
  govMode = GOV_NORMAL_SLEW;

  currentPID.SetMode(MANUAL);
  pidOutput = (double)lastAppliedDuty;
  currentPID.ResetIntegratorTo((double)lastAppliedDuty);
}

// Adaptive fast-OV lockout ladder. An isolated transient (RPM blip crest) re-arms fast so the
// G2 clamp + post-release refill own the retry; a persistent cause escalates so cut/restart churn
// stays bounded. 3 fires per tier: 0.5s, 1s, 2s, 4s, 8s, then 10s cap. 60s with no fast-OV fire
// resets to the first tier. RAM-only by design — a reboot restarts the ladder.
uint32_t nextFastOvLockoutMs(uint32_t nowMs) {
  static uint32_t lastFireMs = 0;
  static uint8_t fireCount = 0;
  if (lastFireMs != 0 && (uint32_t)(nowMs - lastFireMs) > 60000u) fireCount = 0;
  lastFireMs = nowMs;
  uint32_t ms = 500u << (fireCount / 3u);
  if (fireCount < 15u) fireCount++;  // saturate so the shift can't run away
  return (ms > 10000u) ? 10000u : ms;
}

// Adaptive tach-lie lockout ladder: 15s, 30s, 60s, then 120s cap; 10 min with no trip resets.
// Tiers are long relative to fast-OV because every retry burns ~3s of field drain producing
// nothing. The tier IS the back-off between probes — buildTickSnapshot releases early ONLY on a
// blanked+held rev-up (running engine), never on a signal drop: a stopped-then-restarted engine
// waits out the tier remainder (tier 1 usually elapses while the engine is off), and deep tiers
// mean recurring trips where withholding the field is correct. RAM-only by design — a reboot
// restarts the ladder.
uint32_t nextTachLieLockoutMs(uint32_t nowMs) {
  static uint32_t lastFireMs = 0;
  static uint8_t fireCount = 0;
  if (lastFireMs != 0 && (uint32_t)(nowMs - lastFireMs) > 600000u) fireCount = 0;
  lastFireMs = nowMs;
  uint32_t ms = 15000u << fireCount;
  if (fireCount < 3u) fireCount++;
  return (ms > 120000u) ? 120000u : ms;
}

/**
 * applyImmediateCut - Shared state reset for all immediate GPIO4 cut paths.
 * Every cut path leaves identical state: GPIO4 LOW, PWM 0, PID manual,
 * telemetry updated, fieldActiveStatus cleared.
 */
void applyImmediateCut(const TickSnapshot &tick, FieldEventReason reason) {
  bool alreadyCut = gpio4IsLow;
  const float preCutDuty = dutyCycle;    // zeroed below the alreadyCut return; the abort latch wants the pre-cut command
  g_fieldEventReason = (uint8_t)reason;  // authoritative cut cause for the banner OFF-reason telemetry
  digitalWrite(4, LOW);
  gpio4IsLow = true;
  // Opens the tach-corruption mask window (buildTickSnapshot). NOT for the two RPM-derived cuts:
  // masking the gate that just fired would re-energize the field on a real stall or against a lying
  // tach for the length of the window.
  if (reason != REASON_RPM_TOO_LOW && reason != REASON_TACH_IMPLAUSIBLE) g_lastFieldCutMs = tick.nowMs;
  // Fast OV and the timed OV tiers: arm the adaptive cooldown lockout (nextFastOvLockoutMs
  // ladder, not FIELD_COLLAPSE_DELAY) — one shared ladder so a persistent cause escalates
  // regardless of which OV rung catches it. applyImmediateCut returns before runShutdownPath's
  // lockout-arm ever runs, so set it here.
  if ((reason == REASON_FAST_OVERVOLTAGE || reason == REASON_OV_TIER_LOW || reason == REASON_OV_TIER_MID)
      && fieldCollapseTime == 0) {
    fieldCollapseTime = tick.nowMs;
    activeCollapseDelay = nextFastOvLockoutMs(tick.nowMs);
    queueConsoleMessageF("%s lockout %.1fs (adaptive: 0.5s-10s ladder, resets after 60s clean)",
                         (reason == REASON_FAST_OVERVOLTAGE) ? "Fast-OV" : "Timed-OV",
                         activeCollapseDelay / 1000.0f);
  }
  // Tach-lie: same pattern — arm the escalating lockout here, latch the trip RPM so the
  // buildTickSnapshot early-release can tell a real rev-up from the phantom's noise band.
  // dutyCycle still holds the pre-cut value at this point (zeroed below the alreadyCut return).
  if (reason == REASON_TACH_IMPLAUSIBLE && fieldCollapseTime == 0) {
    fieldCollapseTime = tick.nowMs;
    activeCollapseDelay = nextTachLieLockoutMs(tick.nowMs);
    tachLieTripRpm = RPM;
    tachLieLockoutArmed = true;
    queueConsoleMessageF("Field commanded but no alternator output: %d RPM, %.0f%% field, %.1fA out - open field drive (ON/OFF switch off, field wire loose, gate-drive/Q3 fault), dead alternator, or false RPM (tach noise). Lockout %.1fs (sustained rev-up releases early)",
                         (int)RPM, dutyCycle, MeasuredAmps, activeCollapseDelay / 1000.0f);
  }
  if (alreadyCut) return;  // hardware already cut — skip logging, don't spam
  apply_pwm_float(0.0f);
  lastAppliedDuty = 0.0f;
  dutyCycle = 0.0f;
  sysMode = SYS_MODE_FAULT;
  updateFieldTelemetry(0.0f, tick.currentBatteryVoltage, FieldResistance);
  fieldActiveStatus = 0;
  currentPID.SetMode(MANUAL);
  pidOutput = 0.0;
  currentPID.ResetIntegratorTo(0.0);
  setpointLimited = 0.0f;
  ctrlLimiter = 0;
  shutdownPhase = SHUTDOWN_PHASE_4;
  shutdownPhaseEntryMs = tick.nowMs;
  prevMode = MODE_CRITICAL_RAMP;
  // inaOvervoltageLatched means HW ALERT pin fired first; SW is catching up.
  // !inaOvervoltageLatched means SW caught it before hardware (e.g. hard OC, temp).
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
  // Commissioning field-curve / knee ramp shares the same override path — flag it for a clean abort
  // AND latch why, so the dashboard can report it instead of the dialog sitting there silently.
  // fieldCurve_tick resets its static phase when it next runs (after the cooldown lockout clears) —
  // so a second cut can still land while this latch is set. First cause wins; later ones go to the
  // follow-on slot rather than overwriting the reason the user actually needs to act on.
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
}

/**
 * enter_sys_fault - Transition to FAULT mode
 */
void enter_sys_fault() {
  sysMode = SYS_MODE_FAULT;
  // govMode set by caller based on fault severity

  currentPID.SetMode(MANUAL);
  pidOutput = (double)lastAppliedDuty;
  currentPID.ResetIntegratorTo((double)lastAppliedDuty);
}

/**
 * enter_sys_manual - Transition to MANUAL mode
 */
void enter_sys_manual() {
  sysMode = SYS_MODE_MANUAL;
  govMode = GOV_NORMAL_SLEW;  // Manual still respects slew for tach protection

  currentPID.SetMode(MANUAL);
  pidOutput = (double)lastAppliedDuty;
  currentPID.ResetIntegratorTo((double)lastAppliedDuty);
}

/**
 * enter_sys_auto - Transition to AUTO mode
 */
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

  // ---- Charging stage selection on AUTO entry ----
  //
  // Always restart from Bulk. If the battery is already full, voltage rises
  // quickly and the overshoot detection in updateChargingStage() fires within
  // seconds, jumping directly to Absorption or Float — no meaningful spike.
  // Attempting to skip Bulk based on entry voltage caused premature float
  // transitions because terminal voltage under load is not a reliable indicator
  // of state of charge.

  uint32_t now = millis();

  bulkVoltageHoldTimer = 0;
  absorptionTailTimer = 0;
  rebulkTimer = 0;

  inBulkStage = true;
  inAbsorptionStage = false;
  inIdleStage = false;
  floatStartTime = 0;
  queueConsoleMessageF(
    "enter_sys_auto: starting in BULK (%.2fV)", getBatteryVoltage());

  // Always seed from alternator current regardless of MaintainMode
  float actualCurrent = max(0.0f, getTargetAmps());
  setpointLimited = actualCurrent;
  setpointInitialized = true;
}

// ==================== CONTROL LOOP SUB-PATH HELPERS ====================
// Each function handles one complete early-exit case from AdjustFieldLearnMode.
// None of these touch the CV integrator (cv_I), bumpless tracker, or iExcess state.

// handleLimpHome — Emergency limp-home path. Bypasses all safeties except INA228.
// Call only when LimpHome==1. Caller returns immediately after.
void handleLimpHome(uint32_t currentMillis, const TickSnapshot &tick) {
  ctrlLimiter = 0;
  if (OnOff == 0) {
    digitalWrite(4, LOW);
    gpio4IsLow = true;
    apply_pwm_float(0.0f);
    lastAppliedDuty = 0.0f;
    dutyCycle = 0.0f;
    return;
  }
  static uint32_t lastLimpTick = 0;
  static uint32_t lastLimpReport = 0;
  if (currentMillis - lastLimpTick >= 100) {
    lastLimpTick = currentMillis;
    // Respect INA228 hardware OV latch even in limp home — prevents
    // oscillation against the PRE-GATE cut. Hardware ALERT pin already
    // pulled GPIO4 low; don't fight it.
    if (!inaOvervoltageLatched) {
      digitalWrite(4, HIGH);
      gpio4IsLow = false;
      apply_pwm_float(30.0f);
    }
    lastAppliedDuty = 30.0f;
    dutyCycle = 30.0f;
    currentPID.SetMode(MANUAL);
    pidOutput = 30.0;
    currentPID.ResetIntegratorTo(30.0);
    // alarmOutputState cleared by CheckAlarms — do not write GPIO38 here
    updateFieldTelemetry(30.0f, tick.currentBatteryVoltage, FieldResistance);
    fieldActiveStatus = 1;
    if (currentMillis - lastLimpReport >= 30000) {
      lastLimpReport = currentMillis;
      queueConsoleMessage("LIMP HOME MODE: 30% duty, all safeties bypassed");
      serialPrintlnNB("LIMP HOME MODE: 30% duty, all safeties bypassed");
    }
  }
}

// runShutdownPath — Fault / non-normal mode state machine.
// Runs the full shutdown ramp (phases 1→3→4→GPIO4 cut) and returns.
// Never reaches the CV control path. prevMode and return are handled by caller.
void runShutdownPath(const TickSnapshot &tick, FieldControlMode mode, FieldEventReason reason,
                     float actualDtSec, bool exitingNormal) {
  voltageControlActive = false;
  lastVoltageControlActive = false;  // caller returns before the CV block's tracker line — stale TRUE would skip the re-entry bumpless seed on re-enable
  cvWindDownActive = false;          // same early-exit rule — a stale wind-down cap must not survive a charge-disable gap

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

  // Critical faults skip the slow ramp entirely
  bool isCriticalFault = (reason == REASON_VOLTAGE_DISAGREE_CRITICAL || reason == REASON_VOLTAGE_IMPLAUSIBLE);

  if (isCriticalFault) {
    shutdownPhase = SHUTDOWN_PHASE_4;
    effectiveMin = 0.0f;
    dutyRequest = 0.0f;
  } else {
    // Phase 1: ramp to rpmMinDuty at DutyRampRate
    if (shutdownPhase == SHUTDOWN_PHASE_1) {
      effectiveMin = 0.0f;
      dutyRequest = tick.rpmMinDuty;
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

    // Phase 3: slow ramp from rpmMinDuty to 0
    if (shutdownPhase == SHUTDOWN_PHASE_3) {
      float slowDuty = slew_limit_f(lastAppliedDuty, 0.0f,
                                    DutySlowRampRate, DutySlowRampRate, actualDtSec);
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

  updateFieldTelemetry(dutyCycle, tick.currentBatteryVoltage, FieldResistance);

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
    if (reason == REASON_TACH_IMPLAUSIBLE) { tachLieTripRpm = RPM; tachLieLockoutArmed = true; }
    // Spam guard: with the engine off the temp feed ages past the 20s staleness threshold, so this
    // cut/restart cycle repeats every cooldown (~2s). Announce a new reason immediately, then throttle
    // to one heartbeat per minute so a genuinely new fault never gets buried under the idle loop.
    static FieldEventReason lastCollapseReason = REASON_NONE;
    static uint32_t lastCollapseMsgMs = 0;
    static uint32_t collapseLoopCount = 0;
    bool newReason = (reason != lastCollapseReason);
    collapseLoopCount++;
    if (newReason || (uint32_t)(tick.nowMs - lastCollapseMsgMs) >= 60000) {
      if (newReason) {
        queueConsoleMessageF("Charging stopped (%s) - %.1fs cooldown before restart",
                             reasonToString(reason), activeCollapseDelay / 1000.0f);
      } else {
        queueConsoleMessageF("Still cycling on %s (%lu cuts) - %.1fs cooldown, suppressing repeats",
                             reasonToString(reason), (unsigned long)collapseLoopCount,
                             activeCollapseDelay / 1000.0f);
      }
      lastCollapseMsgMs = tick.nowMs;
      collapseLoopCount = 0;
    }
    lastCollapseReason = reason;
  }

  reportFieldModeEvent(tick.nowMs, mode, reason, tick, gpio4IsLow, dutyCycle);
}

// runCommissionIdle — "rest" hold between commissioning steps. Holds the field at a flat voltage-scaled
// minimum (COMMISSION_REST_FLOOR_PCT × 12/Vbatt), eased in at a slow dedicated rate
// (COMMISSION_REST_RAMP_PCT × 12/Vbatt). Deliberately does NOT max() in the per-RPM onset floor
// (rpmMinDuty): that floor lives in rpmMinDutyTable, the very table commissioning is (re)learning, so
// resting on it means leaning on a possibly stale/half-written table; it also lets a held high-RPM-low
// onset duty briefly exceed the (lower) onset after RPM rises, producing a real output blip until the
// slew catches up. A flat floor is predictable and table-independent. GPIO4 stays ENABLED — a little
// field load keeps the RPM pickup alive (going fully to 0 can drop the signal). Deliberately bypasses
// the whole AUTO/MANUAL/fault/stage machinery (the caller returns right after this), so no charging
// stage runs, no "Charging stopped/enabled" spam, and no GPIO4 cut. A real fault never lands here —
// selectFieldControlMode returns the fault mode instead, so this is only reached when nominal.
void runCommissionIdle(const TickSnapshot &tick, FieldEventReason reason, float actualDtSec) {
  voltageControlActive = false;
  lastVoltageControlActive = false;  // same stale-tracker clear as runShutdownPath — the wizard's test-FAIL→rest→retry flow is the path that double-fired 2026-07-20
  cvWindDownActive = false;          // same early-exit rule — a stale wind-down cap must not survive a commission rest
  uTargetAmps = 0;
  setpointLimited = 0.0f;
  ctrlLimiter = 0;
  shutdownPhase = SHUTDOWN_PHASE_NONE;   // so a later real shutdown starts its ramp fresh from here

  const float vNorm = 12.0f / fmaxf(1.0f, (float)SYSTEM_VOLTAGE_CLASS);
  const float restFloor = COMMISSION_REST_FLOOR_PCT * vNorm;   // 4 / 2 / 1.33 / 1 % @ 12 / 24 / 36 / 48 V
  const float restRamp = COMMISSION_REST_RAMP_PCT * vNorm;     // 5 / 2.5 / 1.67 / 1.25 %/s
  const float restTarget = restFloor;

  // Re-assert the field enable unconditionally: the mode arbiter guarantees no fault or lockout is
  // active when COMMISSION_IDLE is selected (lockout is priority 7, rest 7.6), but a rest resumed
  // after an immediate cut (e.g. engine stopped mid-rest) enters with gpio4IsLow still true — gating
  // the write on it left the line LOW for the whole rest, killing the RPM-pickup keep-alive load.
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

  updateFieldTelemetry(dutyCycle, tick.currentBatteryVoltage, FieldResistance);
  fieldActiveStatus = (dutyCycle > 0.01f) ? 1 : 0;
  chargeStageDisplay = getChargeStageDisplayCode();
  reportFieldModeEvent(tick.nowMs, MODE_COMMISSION_IDLE, reason, tick, gpio4IsLow, dutyCycle);
}

// ==================== MAIN CONTROL FUNCTION ====================

// ===== PID TUNING SCORE — helpers called from AdjustFieldLearnMode =====

// Called from setup() after PSRAM allocations and loadTuningLog() are done.
// Prints sizing info; allocation and load happen in setup() proper.
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
  // Struct size is part of the wire format. Bail cleanly on mismatch (e.g. after
  // TuningRecord layout changed) instead of reading garbage into the array.
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
  // Corrupt count/head would index past the 50-slot ring (commit writes BEFORE
  // the %50 wrap) — discard rather than trust a bad header.
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
  // Struct size is part of the wire format. Bail cleanly on mismatch (e.g. after
  // CVTuningRecord layout changed) instead of reading garbage into the array.
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
  // Corrupt count/head would index past the 50-slot ring (commit writes BEFORE
  // the %50 wrap) — discard rather than trust a bad header.
  if (cvTuningLogCount > 50 || cvTuningLogHead >= 50) {
    Serial.println("CVTuningLog: corrupt count/head — discarding old log");
    cvTuningLogCount = 0;
    cvTuningLogHead = 0;
    LittleFS.remove("/cvtuninglog.bin");
    return;
  }
  Serial.printf("CVTuningLog: loaded %d records, counter=%d\n", cvTuningLogCount, cvTuningRunCounter);
}

// TODO (future): the CV tuning score (ISE/T, settling, overshoot) currently does NOT
// distinguish runs taken with testProtectionsEnabled=true vs =false. Scores under
// protections-on are loop+protection composite; under protections-off they are pure
// loop. Comparing them directly is misleading. Future redo: tag each record with the
// flag state at the time of the run so the UI can group/filter, and rethink whether
// fastOvFires/iExcessFires/loadDumpFires belong in the score formula when protections
// were disabled (they can't fire then, so the counts will always be zero).
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
  // ISE/T: (HIGH ISE + LOW re-overshoot ISE + LOW undershoot ISE) ÷ total active time, ×1000.
  // HIGH overshoot above the class-scaled dead-band weighted ×cvKOvershoot; LOW undershoot ×0.15
  // with time ramp. ÷ class ratio² (the integrators are V²-domain) so the score and its
  // good<10/<20 dashboard bands read 12V-equivalent on 24/36/48V banks.
  float scoreNorm = (12.0f / (float)SYSTEM_VOLTAGE_CLASS) * (12.0f / (float)SYSTEM_VOLTAGE_CLASS);
  rec.score = (rec.activeTimeSec > 0.0f)
                ? (1000.0f * scoreNorm * (cvTuningScore.totalIntegratedOvershootVs + cvTuningScore.totalLowIntOvVs + cvTuningScore.totalLowUndershootVs)
                   / rec.activeTimeSec)
                : 0.0f;
  rec.fastOvFires = cvTuningScore.fastOvFires;
  rec.iExcessFires = cvTuningScore.iExcessFires;
  rec.loadDumpFires = cvTuningScore.loadDumpFires;
  rec.hardOcFires = cvTuningScore.hardOcFires;
  rec.voltageKp = VoltageKp_active;  // gain actually in effect (Manual or Auto α/K, normalized)
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
// Bucket classifiers + excursion stopwatches live in AdjustFieldLearnMode (electrical loops)
// and thermalAccuracyScore_tick (thermal containment — pinned at the limit is continuous
// challenge). No committed/live split: the panel, the tuning-log mirrors, and the cloud
// snapshot all read the same accumulators; ratios are computed at display time.

// Settle/debounce gate: returns true only once the loop's authority condition has held continuously
// for settleMs. The false→true edge stamps bindingStartMs; any false tick clears it (restart timer).
static bool accBindingReady(uint32_t &bindingStartMs, bool binding, uint32_t nowMs, uint32_t settleMs) {
  if (!binding) { bindingStartMs = 0; return false; }
  if (bindingStartMs == 0) bindingStartMs = nowMs;
  return (uint32_t)(nowMs - bindingStartMs) >= settleMs;
}

// Zero all accumulators. clearLive=true (manual Reset button) also discards the live excursion
// stopwatches and the live thermal containment session — the next scored second costs a fresh
// ACC_SETTLE_THERMAL_MS. clearLive=false carries an in-progress excursion or containment session
// WHOLE into the new window (counted there, out-of-band time preserved), so a failure spanning
// midnight lands in the next day's row rather than being erased — that was the daily auto-reset's
// mode, and with that reset disabled the false path currently has no live caller.
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
  // Struct size is part of the wire format. Bail cleanly on mismatch (e.g. after
  // SystemIDRecord layout changed) instead of reading garbage into the array.
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
  // Corrupt count/head would index past the 50-slot ring (commit writes BEFORE
  // the %50 wrap) — discard rather than trust a bad header.
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

// Push the current SystemID result globals into the ring buffer.
// `aborted == true` means the test never produced rise/fall numbers — we
// still record so users see the abort in the history.
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

// Called from tempPID_tick() on every tick (16 Hz). Feeds the thermal Control
// Accuracy containment score (v4: binding seconds + in-band seconds), authority-gated.
void thermalAccuracyScore_tick(uint32_t nowMs, float dtSec) {
  // ===== Thermal Control Accuracy — containment while binding =====
  // Authority gate: only score while the thermal derate actually OWNS the current command.
  //   g_thermalOwnsCeiling — the penalty (>2A) set uTargetAmps; false when warmup/HiLo/battery
  //                          limit/fastOV/MaxTableValue capped it lower, i.e. someone else binds
  //   cvOwnsCommand        — CV regulating BELOW that ceiling: its output is the real command and
  //                          thermal error stops reflecting thermal control quality
  //   g_fastOvClampActive  — OV supervisor cutting current for voltage, not thermal
  //   MaintainMode         — output forced to zero; thermal loop does nothing
  // Icv/uTargetAmps are one tick stale here (tempPID_tick runs before the ceiling+CV chain) but
  // stale as a consistent pair, so the comparison holds. This ownership test replaced a charge-stage
  // proxy (score only in BULK, 3-min blanking otherwise) that latched the score off for whole
  // sessions: one absorption tick re-armed the blanking even while the derate was plainly binding.
  bool cvOwnsCommand = voltageControlActive && (Icv < (float)uTargetAmps - 0.5f);

  // A CV handoff shorter than ACC_THERM_CV_YIELD_MS only PAUSES accumulation (below); only a
  // sustained one ends the containment session. The plant cannot move far enough in 10 s to corrupt
  // the score — ~0.2°F at typical ramp rates, against a ±3°F band and a 25 s slope window — and
  // tearing down on every 0.5A chatter across the ceiling would never let the 120 s settle finish.
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
    // Containment is referenced to the loop's ACTUAL regulation target (limit −7°F once the slope
    // buffer is full — tempPIDSetpoint_d), NOT TemperatureLimitF: limit-referencing gave a perfectly
    // regulating loop a built-in ~7°F error floor. worstOver stays limit-referenced (damage metric)
    // and is tracked UNCONDITIONALLY in AdjustFieldLearnMode (right after tempFilterUpdate), so
    // the peak captures field-off protection-cut tails this gated path never runs for.
    float err = tempFiltered - (float)tempPIDSetpoint_d;
    accThermBindingSec += (double)dtSec;
    if (fabsf(err) <= ACC_T_BAND_F) accThermInbandSec += (double)dtSec;
  }
}

// OV-episode window: any hard protection clamp (Group 1/2 OV, iExcess CV/bulk, or Load Dump) within the last 30s. Observed relapse periods are
// 1-3s (longest gap 12.8s, 18:31 07-22 AGM), so 30s brackets a whole train with margin while
// staying invisible to normal operation.
static uint32_t g_ovEpisodeLastFireMs = 0;
static uint32_t g_ovEpisodeStartMs = 0;   // first fire of the current episode — freshness fence for cvSteadyHoldEma
static inline bool ovEpisodeActive() {
  return g_ovEpisodeLastFireMs != 0 && (uint32_t)(millis() - g_ovEpisodeLastFireMs) < 30000u;
}

// Recovery P-boost multiplier — error-scheduled CV proportional authority. Returns 1× at/above
// target and inside the cvRecovBoostFloorV (×class) dead area below it, then ramps linearly —
// continuous from 1× at the dead-area edge, no engagement step (the D-term latch-relay lesson) —
// to cvRecovBoostMax at cvRecovBoostErrV (×class) of shortfall. The dead area keeps boost gain
// out of small-signal regulation: the 07-24 0.7 Hz idle wander (shortfalls ≤0.16 V) was
// boost-sustained — measured outer gain 12.9 vs 8.5 A/V configured lifted loop gain 0.6→0.99.
// Inside an OV episode the whole ramp SLIDES UP to start at cvRecovDeepBandV (×class), same
// width: the post-cut hole below target is created by the cut itself, and boosting the climb
// NEAR target re-fires the trip on a stiff-topped lead-acid plant (pTerm 25-37A vs a ~19A hold,
// 18:31 07-22) — but every observed re-fire happened at/above target, so a bus still deeper than
// the band cannot be that echo and keeps the boost (the 07-25 flat-off held a bus 2V low at 1×).
// Outside an episode any below-target error past the dead area is boosted (load-serve holes).
// Scales only the P term; cv_I and the D trim are untouched.
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

// Arrival flare — recovery-ceiling taper over the last cvRecovFlareBandV (×class) of shortfall,
// from the full goal at the band edge down to cvRecovFlareFrac×goal at target. Both 07-25
// recovery re-fires arrived at target carrying the full pre-trip current into a surface-charged
// battery — ~7-10A of surplus the PI could not shed in the 0.6s the last 0.6V took. The flare
// lands with ~zero surplus; the PI re-adds the held-back amps at its own pace if truly needed.
// Continuous at the band edge; returns the plain goal when disabled (band 0 / frac 1).
static inline float cvRecovFlaredCeil(float goalA, float shortfallV) {
  float band = cvRecovFlareBandV * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f);
  if (band <= 0.001f || shortfallV >= band) return goalA;
  float frac = fmaxf(shortfallV, 0.0f) / band;
  return goalA * (cvRecovFlareFrac + (1.0f - cvRecovFlareFrac) * frac);
}

// Commissioning-ownership predicate for the DVCC clamp bypass, computed LIVE at the point of
// use: updateChargingStage() evaluates the same expression but is suppressed in manual/Maintain/
// TargetVoltage modes — a mirror refreshed there goes stale in exactly the override modes the
// "clamps every writer, no exemptions" decision must cover.
static inline bool cxOwnsBatteryNow() {
  return (commissionState == 1) && (lastCommissionHeartbeatMs != 0)
         && ((uint32_t)(millis() - lastCommissionHeartbeatMs) < COMMISSION_HEARTBEAT_TIMEOUT_MS);
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

  uint32_t actualDtMs = (lastControlTickMs == 0) ? 62 : (currentMillis - lastControlTickMs);
  if (actualDtMs > 500) actualDtMs = 500;
  if (actualDtMs == 0) actualDtMs = 1;  // floor prevents 0-div in slew calcs when tick runs twice in same ms
  float actualDtSec = (float)actualDtMs / 1000.0f;

  static float uTargetRaw_cached = 50.0f;   // always MaxTableValue (assigned from uTargetRaw); seeds fastOvBaseCap (next line) and the supervisorLimiting gate
  float uTargetRaw = (float)MaxTableValue;  // always MaxTableValue; kept for uTargetRaw_cached lineage only
  float fastOvBaseCap = clamp_f(uTargetRaw_cached, 0.0f, (float)MaxTableValue);
  float fastOvCurrentCap = fastOvBaseCap;
  // Binding-reason tracker — LOCAL through the tick, exported to g_fastOvCapReason
  // alongside g_fastOvCurrentCap after all supervisors. Must NOT write the global
  // directly here: the iExcess/LoadDump supervisors sit BELOW the fresh-CH1 early
  // return, so a global reset on every no-CH1 pass would wipe their reason while
  // the stale cap stayed exported (iExcess-lowered caps with capReason=0).
  uint8_t capReasonTick = CAP_REASON_NONE;
  bool fastOvClampActive = false;
  static uint32_t ocTripStartMs = 0;

  updateCurrentRPMTableIndex(RPM);
  updateRPMBucketHistory(currentMillis);

  TickSnapshot tick = buildTickSnapshot(currentMillis, actualDtMs);
  // Above every early-return below (fresh-CH1 gate, limp home, immediate cuts) so lifetime OV
  // dwell accumulates in all modes. RAW voltage on purpose — filtered smears ~100ms spikes.
  updateOvHistogram(tick.currentBatteryVoltage, tick.nowMs);
  uint32_t aflM1 = micros();  // end of section 1: RPM tables + tick snapshot

  // Runs every tick in every mode (not just AUTO): an over-temp event drops to SYS_MODE_FAULT,
  // which skips the AUTO branch, so the filtered temp/slope/projection would otherwise freeze
  // for the whole cooldown. Pure display/estimator math (no PID/field side effects); runs before
  // tempPID_tick. TempToUse is fresh here (buildTickSnapshot mirrors tick.tempToUseF to it).
  tempFilterUpdate(currentMillis);

  // Over-temp protection (warn ramp + critical cut) compares against the FILTERED temperature, not the
  // raw DS18B20 sample. A single noisy 1-wire reading >limit+warn could instantly latch the warning-ramp
  // lockout while the true/filtered temp was ~10°F lower. tempFiltered holds its last good value through garbage reads, so a genuinely
  // dead sensor is still caught separately by the raw-based staleness CRITICAL cut (tick.tempDataVeryStale).
  // Override here (after tempFilterUpdate, before the protection calls); TempToUse and the rest of
  // tick.tempToUseF's producers stay raw — only the trip thresholds and the trip console line see filtered.
  if (!isnan(tempFiltered)) {
    tick.tempToUseF = tempFiltered;
  }

  // Worst over-temp for the Control Accuracy panel is tracked HERE — unconditionally, every tick in
  // every mode (field on, FAULT, lockout, OFF cooldown). Over-temp damages the alternator regardless
  // of which subsystem holds the field, and the worst excursions happen during the protection-cut
  // coast-down (field OFF), which tempPID_tick / thermalAccuracyScore_tick never run for. Gating the
  // peak on sustained thermal binding (the containment gate, still in thermalAccuracyScore_tick) made
  // the score blind to thermal limit-cycling — the exact failure it should flag (the 120s settle
  // outlasts the trip-to-trip period and the peaks land while the field is cut). Containment stays
  // gated (regulation quality; cut noise corrupts it).
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
    lastAppliedDuty = 0.0f;
    setpointInitialized = false;
    // Restore AUTOMATIC so Compute() resumes; otherwise the UI "Reset Inner PID"
    // button leaves the PID stuck in MANUAL until charging is cycled off/on.
    // Only restore if sysMode is AUTO — MANUAL/FAULT/OFF should keep PID in MANUAL.
    // SetMode(AUTOMATIC) triggers library Initialize() which re-confirms a clean state
    // (outputSum=pidOutput=0, lastUnsatOutput=0) consistent with the reset above.
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
  // Manual protection test (mode 1/4): protTest_tick armed a cut after the field settled at target —
  // fire it here through the REAL fast-OV executor so the electrical event AND the adaptive lockout
  // ladder are identical to a genuine over-voltage cut. Deferred one tick from the settle so the cut
  // lands in this top-of-loop path (applyImmediateCut returns from the whole function).
  if (protTestCutPending && !gpio4IsLow) {
    protTestCutPending = false;
    queueConsoleMessage("Protection test: firing instant field cut (fast-OV executor)");
    applyImmediateCut(tick, REASON_FAST_OVERVOLTAGE);
    return;
  }
  FieldEventReason preReason = selectFieldEventReason(tick);
  updateProtectionCounters(preReason);
  if (shouldImmediatelyCutGPIO4(preReason) && !gpio4IsLow) {
    applyImmediateCut(tick, preReason);
    return;
  }

  // Engine confirmed stopped (RPM held at 0 for >= RPM_ZERO_CUT_MS): cut the field
  // immediately, overriding any graceful shutdown ramp. CHARGING_DISABLED (priority 1a)
  // outranks the RPM gate, so on a normal key-off + engine-stop the field would otherwise
  // slow-ramp for ~30s while still energized — coupling PWM noise into the LM2907 RPM sense
  // (phantom RPM spikes). Placed pre-CH1-gate so it fires at full loop rate regardless of
  // current-sensor freshness. Forced reason RPM_TOO_LOW so the log/telemetry name the cause.
  // Manual field mode is exempt: the arbiter puts MANUAL above the RPM gate, so this cut and
  // the manual path fought at loop rate (GPIO4 oscillation), and manual exists precisely for
  // engine-off wiring/diagnostic tests (same doctrine as manual-beats-lockout).
  if (tick.engineFullyStopped && !tick.manualMode && !gpio4IsLow) {
    applyImmediateCut(tick, REASON_RPM_TOO_LOW);
    return;
  }

  // Shutdown spin-down: charging already disabled AND RPM below the field minimum — the graceful
  // CHARGING_DISABLED ramp has nothing left to soften (no alternator output below MinRPMForField)
  // and an energized field only fakes tach readings, so finish the cut. The RPM_BELOWMIN_CUT_MS
  // dwell (≥ two ADS CH2 samples) keeps a lone glitched low read from cutting mid-ramp at real
  // duty — that abrupt amplitude step is the LM2907 coupling-cap slam the ramp exists to avoid.
  // Still ~3x faster than the zero-cut's RPM_ZERO_CUT_MS on a real spin-down. Engine running
  // above minimum keeps the full graceful ramp.
  {
    static uint32_t rpmBelowMinSinceMs = 0;
    if (!tick.chargingEnabled && tick.rpmBelowMinimum) {
      if (rpmBelowMinSinceMs == 0) rpmBelowMinSinceMs = tick.nowMs;
      if ((uint32_t)(tick.nowMs - rpmBelowMinSinceMs) >= RPM_BELOWMIN_CUT_MS && !gpio4IsLow) {
        applyImmediateCut(tick, REASON_RPM_TOO_LOW);
        return;
      }
    } else {
      rpmBelowMinSinceMs = 0;
    }
  }


  // ===== FAST VOLTAGE SAFETY OVERRIDE ==========
  // Runs every loop before the CH1 gate.
  // Computes fastOvCurrentCap — a per-tick hard ceiling on commanded current.
  // Applied to uTargetAmps after RPM/thermal/user overrides in the AUTO path.
  // Direct cv_I clamp kept here because the CV loop only runs every 100ms;
  // without it cv_I builds positive for up to 100ms while battV is above target.

  // ── Zero-output protection stand-down (altZeroOutput) ─────────────────────
  // A protection whose actuator cannot move the protected variable must not fire. With the
  // alternator delivering ~0 A into a bus above target, the excess is authored by battery rest
  // (CV target set below resting V) or another source (solar/shore/DC-DC) — a field cut removes
  // nothing, and each no-op fire still costs the tach false-zero, the cv_I reseed, the OV-episode
  // derate and the lockout ladder. Gates G1/G2 and the CV iExcess arm; the bulk iExcess arms only
  // below target − margin, unreachable while this holds. Absolute layers (AlternatorHardShutdownV,
  // fast-OV ceiling, INA228 ALERT, Load Dump) stay armed. Zero band scales with the configured
  // hall sensor's full scale (different noise floors). Entry needs a 2.5 s dwell so a normal decay
  // through target can't latch; the amps/CV/stale exits are SINGLE-TICK — an RPM rise at unchanged
  // field regains real output with no warning, and re-arming one tick late is a real overvoltage.
  // The voltage leg alone is hysteretic (strict above-target to start, target − ~50 mV×class to
  // continue and to hold the latch): the wind-down glide parks the target ~20 mV under the bus, so
  // a strict comparison chatters at ripple rate — the dwell never completed (2026-07-28 log:
  // longest run 0.30 s of 2.5 s, so the stand-down could only ever arm AFTER something had already
  // fired) and a completed latch would have chattered the same way. The continue/hold leg reads
  // g_ovIbvFilt (the G2 level filter, one tick stale — computed just below): raw IBV rides 110 mV
  // p-p through the 50 mV band at a MILD operating point, and a single sample exactly on the bar
  // reset the whole dwell 115 ms before it would have armed. Same doctrine as G2 — a sustained-
  // state decision must not be paced by raw ripple. Entry stays strict on raw (the permissive
  // direction), and a ripple dip below target at ~0 A restores no shed authority, so holding
  // through it is safe. Signed amps on purpose (no fabsf): negative reads as no shed authority.
  // Stale current data = never latch.
  {
    float zeroFullScaleA = (AmpSensorRange == 0) ? 200.0f : (AmpSensorRange == 2) ? 500.0f : 300.0f;
    float zeroBandA = fmaxf(2.0f, 0.01f * zeroFullScaleA);
    static uint32_t altZeroSinceMs = 0;
    float zeroHystV = 0.05f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f);
    bool zeroLegs = voltageControlActive && !tick.currentDataStale
                    && MeasuredAmps < zeroBandA;
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

  // Pre-event cv_I snapshot for the protection-release reseed (rationale: CV_Loop_Dev_Summary.md).
  // Timing note: g_fastOvClampActive here holds LAST tick's final value — it is updated at the
  // end of the bumpless block, after every supervisor has voted, so this read reflects the
  // unified flag.
  static float preEventCvI = 0.0f;
  static float preEventIcv = 0.0f;  // commanded current at the same snapshot — recovery goal is max(cv_I, Icv)
  static float preEventLoadEma = 0.0f;  // ~2s EMA of house-load amps (MeasuredAmps − Bcur) while unclamped — demand-drop reference for the release reseed (shunt only)
  static uint32_t preEventLoadMs = 0;   // 0 = EMA unseeded
  if (!g_fastOvClampActive) {
    preEventCvI = cv_I;  // refresh while no protection is clamping
    // Min against the unboosted P+I reconstruction: a boost-inflated climb must not become the
    // next fire's refill goal (36-50A goals vs a ~19A hold, 18:31 07-22).
    preEventIcv = fminf(Icv, VoltageKp_active * fmaxf(ChargingVoltageTarget - IBV, 0.0f) + cv_I);
    if (HAS_BATT_SHUNT) {
      float loadsNow = MeasuredAmps - Bcur;
      float dtL = (preEventLoadMs == 0) ? 0.0f : fminf((float)(uint32_t)(currentMillis - preEventLoadMs), 500.0f);
      preEventLoadEma = (preEventLoadMs == 0) ? loadsNow
                                              : preEventLoadEma + (dtL / (2000.0f + dtL)) * (loadsNow - preEventLoadEma);
      preEventLoadMs = currentMillis;
    }
  }
  // Post-protection integrator refill (deficit-gated rate law). While active, UP-integration of
  // cv_I runs at VoltageKi_active × M, M = 1 + (cvRecovKiMax−1)·(remaining deficit / deficit at
  // release) — fast while the reseed hole is big, tapering to normal as it heals — floored at
  // CvRecovClimbRate A/s while the projected bus arrival is still short of target (07-23), so a
  // low seed heals in seconds instead of error-paced tails. It rides the NORMAL integration path,
  // so every existing gate applies (kdLimiting freeze, satHi, cv_I_aw_cap, battery-current
  // ceiling) — which is why the
  // old timed window's error cap is unnecessary. A premise-void backstop survives (see the
  // termination block): satHi freezes cv_I whenever Icv rails at the goal ceiling, so if the
  // pre-trip current stops holding the target the deficit could never heal and the ceiling
  // would cap CV at the stale pre-trip current forever. A second exit ends the window once the
  // bus has HELD ≈target ~1s (recovered exit) — without it the window latches whenever the plant
  // heals needing less current than the goal. Icv stays ceilinged at
  // recovCvGoal for the life of the refill: the pre-trip current held the target, so commanding
  // more on the way back can only overshoot. Goal basis is max(cvSteadyHoldEma, preEventIcv) −
  // the measured demand drop at the fire (see the rising-edge capture) —
  // the EMA is the current that actually holds the target, immune to the overshoot droop and
  // release-gap re-samples that drag the raw snapshot to ~half the hold (21A vs 39A,
  // cvlog_20260718_1117); stale-EMA fallback is preEventCvI. Any new fire cancels the refill —
  // the mid-heal cv_I becomes the next reseed base (de-escalation ratchet).
  static bool recovActive = false;
  static float recovCvGoal = 0.0f;    // max(cvSteadyHoldEma, preEventIcv) − demandDropA snapshot at release (= the seed on a rapid re-fire) — heal target AND the Icv ceiling
  static float recovDeficit0 = 0.5f;  // recovCvGoal − cv_I at release (floored 0.5A) — normalizes the Ki-boost taper
  static bool recovGoalCollapsed = false;  // rapid re-fire collapsed goal to the seed → deficit-healed exit is void (heals on tick 1, dropping the Icv ceiling exactly when it must hold — 53A commanded on a 10.7A ceiling, 16:49 07-24); only held-at-target / walk-to-ceiling / new fire end the window
  static uint32_t recovStartMs = 0;     // release stamp — the premise-void + recovered exits arm 2 s later (field-lag gap right after release has the ceiling pinned on a briefly-flat bus)
  static uint8_t recovStarveTicks = 0;  // consecutive PI ticks the goal ceiling is pinned with the bus low + not rising
  static uint8_t recovHeldTicks = 0;    // consecutive PI ticks the bus has held ≈target — the "recovered" exit; without it the window latches as a stale ceiling whenever the plant heals needing less current than the goal (13:56 07-22 zombie)
  static float recovVRefEma = 0.0f;     // ~3s EMA of the filtered bus — the walk's "not rising" test is a delta above this reference; a slope-EMA sign gate ripple-starved the walk to 7% duty / 0.11 A/s (21:36 07-24, no-shunt load stuck 1.7V low ~9 min)
  static float demandDropA = 0.0f;      // house-load amps that left the bus at the fire (rising-edge loads vs preEventLoadEma) — subtracted from reseed base AND goal, else the refill restores a current the event proved unwanted
  static uint8_t rapidReFires = 0;      // consecutive re-fires <4s after release — proof the seed is still high; rebases the next seed on lastSeedA ×0.7
  static uint32_t lastReleaseMs = 0;
  static float lastSeedA = 0.0f;        // cv_I written at the last release — the rebase base on a rapid re-fire (preEventCvI regrows mid-train as the refill climbs: 22:17 07-22, 3 fires shedding only ~14%/cycle)
  static bool recovWalking = false;     // starve backstop engaged (announce-once latch): the goal ceiling walks up at a bounded rate instead of releasing in one step (a one-tick release steps the command by the whole P+I surplus — 25-30A steps re-fired immediately, 18:31 07-22). Engaged ≠ moving: the walk itself is re-gated every tick on recovStarveTicks
  static uint32_t cvStallStartMs = 0;   // dwell start for the below-target stall accelerator (0 = disarmed)
  static float    cvStallV0 = 0.0f;     // getFiltV() at dwell start; the no-progress test is a bus DELTA over the dwell, not a per-tick slope — cvDSlope reads ±0.35 V/s on ripple alone and would never accumulate consecutive ticks
  static bool     cvStallBoost = false; // stall accelerator engaged: climb floor extended outside recovActive
  static bool loadServeActive = false;    // load-serve Ki boost (loadServeBoostEnable): measured demand (loads + battery intake) exceeds cv_I with the bus low → boost up-integration toward it. e=K×ΔI hides a 35A load behind ~0.25V on lithium, so plain Ki serves it in ~30s; the old battery-discharge trigger never held — the P-boost parks Bcur near 0 within ~0.3s (16:24 07-22 log)
  static float loadServeGoal = 0.0f;      // loadsNow + battIntakeEma snapshot at engagement — taper reference and the healed-exit target
  static float loadServeDeficit0 = 0.5f;  // loadServeGoal − cv_I at engagement (floored 0.5A) — normalizes the taper like recovDeficit0
  static uint8_t loadServeTicks = 0;      // consecutive PI ticks of sustained demand deficit before engaging (chatter guard)
  static float cvSteadyHoldEma = 0.0f;  // slow EMA of cv_I during clean CV near target = true holding current; recovCvGoal basis, immune to overshoot droop and flutter-gap re-sampling; shifted down by demandDropA at a fire (its premise dies with the departed load)
  static uint32_t cvSteadyHoldMs = 0;   // last clean-window EMA update — goal basis trusts the EMA only if this postdates the episode's first fire (a pre-episode EMA can remember a departed load: 55A goal vs ~12A plant, 18:33 07-22 no-shunt)
  static float battIntakeEma = 0.0f;    // slow EMA of Bcur in the same clean-CV window = battery's charging share at target; reseed floor + the loadServeGoal intake term (shunt only)
  // Post-protection fast-rise window — opened by the falling-edge handler below.
  // 0 = window inactive. Gated again at slew site by FastSetpointRiseWindowMs cap
  // and FastSetpointRiseHeadroomV vs ChargingVoltageTarget.
  static uint32_t postProtectRiseStartMs = 0;

  // Field-drain early release: once a clamp episode has held for the commissioned drain time — the
  // MEASURED command→10%-of-output time (~dead-time + 2.3 L/R time constants), evaluated at the RPM
  // latched when the episode began (drain is RPM-dependent; live RPM is tach-corrupted once the field
  // cuts) — the field coil driving the excess has bled out, so the iExcess hold latch releases early
  // instead of waiting out its lagged excess EMA (once that EMA is back under the fire line — see the
  // site gates). G2 no longer uses this: it is armed only while the bus is rising and self-releases
  // the instant the slope goes non-positive, so it never has a stale hold to drain. OR'd with
  // iExcess's natural release — can only shorten a cut, never lengthen it. A Load-Dump-owned clamp
  // (no hold latch, re-asserts every tick) is unaffected.
  float tauHoldMs = fdDrainMsAtRpm(g_ovClampRpm);
  bool tauReleaseNow = g_fastOvClampActive && g_ovClampRiseMs != 0 && tauHoldMs > 0.0f
                       && (float)(uint32_t)(currentMillis - g_ovClampRiseMs) >= tauHoldMs;
  // Count/announce once per episode, and only when the iExcess hold latch owns the clamp: Load Dump
  // re-asserts every tick (no hold latch), G1 is predictive (no hold latch), and G2 is rising-gated
  // (self-releasing), so the timer elapsing against any of them clears nothing. Episode-latched rather
  // than edge-detected — the committed reason can flicker (Load-Dump tiers vote per-sample), and an
  // edge-detect would re-announce per flicker.
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

      // Test-mode bypass: when testProtectionsEnabled is false (user toggled it off on a
      // tuning page) OR TuningMode is active (current-waveform step test) OR the battery-health
      // DCIR test is running (batteryHealthTestActive — a current step test that must not be
      // fought by the soft layers at any SoC), G1 and G2 are inhibited from firing so the test
      // can characterise the plant/battery without protection layers fighting the input. The
      // fast-OV ceiling (priority 1.5) and the INA228 hardware ALERT stay live regardless. G2's arm
      // state (ovActive) is recomputed every tick from g2SoftNow, which carries these same gates, so
      // engaging the bypass drops the clamp on the same tick — no stranded latch to de-assert.
      // altZeroOutput (zero-output stand-down, computed above) rides the same gates: at ~0 A
      // delivered a fire cannot move the bus, so G1/G2 stand down until real output returns.
      if (testProtectionsEnabled && !TuningMode && !batteryHealthTestActive && !altZeroOutput && IBV > ChargingVoltageTarget - PRED_GUARD) {
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
      bool g2SoftNow = (testProtectionsEnabled && !TuningMode && !batteryHealthTestActive && !altZeroOutput && OvGroup2Enable && ibvFilt > ChargingVoltageTarget + OvMeasMarginV);
      static bool g2SoftPrev = false;
      if (g2SoftNow && !g2SoftPrev) g_ovTel.softExceedCount++;
      g2SoftPrev = g2SoftNow;

      // Group 2 measured-OV: armed ONLY while the filtered bus is above the re-fire line
      // (target + OvMeasMarginV) AND still rising (filtered dvdt > 0). A falling bus above the line
      // is a receding danger — the field is already cut and the bus drops on its own, so re-clamping
      // it only flickers the field tick-by-tick and compounds the protection reseed. Every real event
      // crosses the line while rising, so the first fire is never missed; a persistent overdrive
      // re-arms on each fresh rise (sawtooth just over the line); the absolute AlternatorHardShutdownV
      // and INA228 hardware cut backstop a runaway regardless of slope. Replaces the old hysteresis
      // hold + dvdt/tau early-release, which held the clamp across the falling band and re-fired every
      // tick as the bus decayed through it. Filtered dvdt (not raw) so belt ripple can't fake the sign.
      // cv_I reseed is the unified falling-edge reseed in the bumpless tracker block.
      ovActive = (g2SoftNow && dvdt > 0.0f);
      if (ovActive) {
        float ovExcess = ibvFilt - (ChargingVoltageTarget + OvMeasMarginV);
        float hystCap = fmaxf(0.0f, setpointLimited - KHard * ovExcess);
        if (hystCap < fastOvCurrentCap) { fastOvCurrentCap = hystCap; capReasonTick = CAP_REASON_KHARD_G2; }
        fastOvClampActive = true;
        g_fastOvHardActive = true;
      }

      // ── Timed OV cut tiers (LOW/MID) ──────────────────────────────────────
      // Sustained-violation field cuts on the same filtered bus as G2 (ripple-honest, slewed-
      // target-relative). Trip = held CONTINUOUSLY above target + margin for the tier's dwell;
      // any tick below the line resets that tier's timer — no hysteresis, so ripple crests
      // crossing momentarily reset rather than accumulate, and a commanded target drop glides
      // the trip lines with the reference. Dwells are sized so every sustained violation is cut
      // inside 0.4 s: a BMS charge disconnect needs >= 0.5 s and ~100 ms of the budget is field
      // de-energization. Gates are identical to G1/G2 (tests/tuning/zero-output stand-down),
      // so MANUAL keeps instant-only protection and the absolute rungs stay always-on. Dwell 0
      // disables that tier. Both timers run independently; whichever expires first cuts, via
      // the reason arbiter (REASON_OV_TIER_LOW/MID -> shouldImmediatelyCutGPIO4 immediate cut +
      // the SAME adaptive fast-OV lockout ladder, so a persistent cause escalates regardless of
      // which rung catches it). The g_ovTierTrip latch clears once the filtered bus is back
      // under that tier's line — after a cut the bus falls and the condition drops.
      {
        bool tierGate = testProtectionsEnabled && !TuningMode && !batteryHealthTestActive && !altZeroOutput;
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
      }
    } else {
      ovActive = false;  // !voltageControlActive (idle only — MaintainMode sets voltageControlActive=true)
      ibvFilt = IBV;     // track raw while idle so CV entry starts the level filter from the live bus
      tierLoSinceMs = 0;
      tierMidSinceMs = 0;
      g_ovTierTrip = 0;
    }
  }

  // ── Group 1/2 hard-OV telemetry export ────────────────────────────────────
  // g_fastOvHardActive is set only by Group 1 (predictive K_HARD) and Group 2
  // (measurement hysteresis) in the FAST OV block above — iExcess and LoadDump
  // never raise it. So this rising-edge counter and the inner-PID integrator
  // reset belong here, right after the Group 1/2 supervisor finishes.
  // The unified telemetry (g_fastOvClampActive, g_fastOvClampCount, g_fastOvCurrentCap)
  // is exported later in the bumpless tracker block, after iExcess and LoadDump
  // have also had a chance to vote — otherwise it would only reflect Group 1/2.
  static bool g_fastOvHardActive_prev = false;
  if (g_fastOvHardActive && !g_fastOvHardActive_prev) {
    g_fastOvHardCount++;  // rising-edge only — count each new hard FastOV activation
    // OV-episode stamp moved to the unified protection rising edge (bumpless tracker block):
    // an iExcess-shaped fire train never raised g_fastOvHardActive, so the P-boost pause and
    // hold-EMA freshness fence sat out a 6-fire headlight-off train (16:49 07-24).
    // Collapse inner PID integrator on hard OV onset. fastOvCap already drives
    // setpoint to near-zero on this tick; without this, a wound-up integrator
    // resists the setpoint collapse and grinds duty down at ~40%/s instead of
    // reaching MinDuty in 1–2 inner PID cycles. PID stays in AUTOMATIC —
    // recovery rebuilds from integrator=0 once fastOV clears.
    currentPID.ResetIntegratorTo(0.0);
    queueConsoleMessageF("FastOV hard #%lu: V=%.2fV (filt %.2fV) target=%.2fV — inner PID integrator reset",
                         (unsigned long)g_fastOvHardCount, IBV, g_ovIbvFilt, ChargingVoltageTarget);
  }
  g_fastOvHardActive_prev = g_fastOvHardActive;

  // ========== EMERGENCY LIMP HOME MODE (runs every loop) ==========
  // WARNING: BYPASSES ALL SAFETY SYSTEMS EXCEPT INA228 HARDCODED
  if (LimpHome == 1) {
    handleLimpHome(currentMillis, tick);
    return;
  }
  uint32_t aflM2 = micros();  // end of section 2: temp warning + fast-OV supervisor + limp gate

  // ========== GATE ON FRESH CH1 DATA ==========
  // PidSampleDivisor=1: PID runs every CH1 sample (fresh CH1 measured 5–25ms apart, assume ~30ms)
  // PidSampleDivisor=2: every other sample, etc.
  static uint8_t ch1SampleCount = 0;

  if (!ch1FreshFlag) {
    // Manual mode does not need current sensor data — let it through.
    if (!tick.manualMode) {
      return;
    }
  } else {
    ch1SampleCount++;
    if (ch1SampleCount < PidSampleDivisor) {
      ch1FreshFlag = false;
      return;
    }
    ch1FreshFlag = false;
    ch1SampleCount = 0;
  }
  lastControlTickMs = currentMillis;

  // ── Reset per-tick pidLog capture globals ────────────────────────────────
  // Written at specific points below; read by pidLog_tick() at end of normal
  // path. Reset here so a skipped branch leaves a clean zero, not a stale value.
  // pidLog_vError is always fresh — computed every tick regardless of loop interval.
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

  if (shouldImmediatelyCutGPIO4(reason) && !gpio4IsLow) {
    applyImmediateCut(tick, reason);
    return;
  }

  // ========== COMMISSIONING IDLE REST ==========
  // Handled here, before the AUTO/MANUAL/fault/stage machinery, so it neither runs a charging stage
  // nor logs mode transitions. sysMode is intentionally left as-is (last AUTO), so resuming a test or
  // ending the session slips back to NORMAL_AUTO with no SYS_MODE transition spam.
  if (mode == MODE_COMMISSION_IDLE) {
    // Rest preserves the prior AUTO sysMode — but a wizard started before the system ever reached
    // AUTO leaves sysMode at its OFF boot default with no path back (this branch returns before the
    // transition handler), so every field step is AUTO-gated off. chargingEnabled is guaranteed true
    // here (selectFieldControlMode PRIORITY 1), so seed AUTO once.
    if (sysMode != SYS_MODE_AUTO) {
      enter_sys_auto();
      pidInitialized = true;
      queueConsoleMessage("Charging enabled (AUTO)");
    }
    runCommissionIdle(tick, reason, actualDtSec);
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
  // Suppressed while any override is active — letting it run would fire
  // spurious re-bulk transitions and absorption timeouts.
  // Stage tracking only meaningful in AUTO. In MANUAL, voltageControlActive=false
  // and no CV loop runs, so stage state is meaningless.
  // CVTuningMode suppressed for same reason as TargetVoltageMode: the step test
  // temporarily raises ChargingVoltageTarget above the real stage target, which
  // can trigger bulk→absorption transitions and cause the absorption tail timer
  // to expire mid-test, killing voltageControlActive.
  if (MaintainMode != 1 && TargetVoltageMode != 1 && !CVTuningMode && !tick.manualMode) {
    updateChargingStage();
  }

  // On override exit: call enter_sys_auto() to reset stage to BULK rather than
  // resuming pre-override stage state that may be stale. updateChargingStage()
  // will fast-forward to the correct stage on the first resumed tick if the
  // battery is already charged.
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

  if ((mode != MODE_NORMAL_AUTO_PID && mode != MODE_NORMAL_MANUAL) && reason == REASON_NONE) {
    static uint32_t lastNoReasonWarnMs = 0;
    if ((uint32_t)(currentMillis - lastNoReasonWarnMs) >= 5000) {
      lastNoReasonWarnMs = currentMillis;
      serialPrintlnNB("ERROR: Shutdown mode with no reason specified");
    }
  }

  chargingEnabled = tick.chargingEnabled;

  // An automated/guided test owns the limiters while it runs — the four user-facing limiter toggles
  // go inert so a stray user setting can't ruin a commissioning/health measurement (each test carries
  // its own built-in slew behavior). NOT bare TuningMode/CVTuningMode — those are the manual study tabs
  // where the toggles are meant to be live. Commissioning counts only while the wizard dialog is open
  // (cxOwnsBatteryNow: IN_PROGRESS + fresh heartbeat), NOT on the bare state byte: a run stopped for
  // days keeps state==1, and on the bare byte it also kept the maintenance-reboot ladder, the CV
  // wind-down governor and the limiter toggles frozen the whole time.
  g_autoTestActive = cxOwnsBatteryNow() || (fieldCurveActive != 0) || batteryHealthTestActive || resTestActive || cvPlantFitActive || (systemIDActive != 0) || (fieldCutActive != 0) || cvStressActive || (protTestActive != 0) || (chcActive != 0);

  // ========== DETERMINE GOVERNOR MODE ==========
  govMode = GOV_NORMAL_SLEW;

  // Any sine waveform (manual sine, sine sweep, commissioning Verify) bypasses duty slew so the actuator
  // can follow the reference unclamped — DutyRampRate would otherwise smear the frequency response. The
  // setpoint is already clean (sine branch skips setpoint slew); this removes the matching duty-side clamp.
  // Gated on g_tuningEntrySettled: while the field is still ramping UP from the rest floor the duty slew
  // stays engaged (gentle entry); the bypass only kicks in once the entry ramp has settled. On exit
  // (TuningMode→0) the bypass drops and duty slew re-engages, easing the field back down.
  // NB: once settled this drops the coupling-cap transient protection during sine (same as open-loop plant-ID).
  if (TuningMode != 0 && tuningWaveform != 0 && g_tuningEntrySettled) {
    govMode = GOV_BYPASS_SLEW;
  }

  // CV plant fit: pulse edges must land in one tick and the CC phases ride their own setpoint ramp — a
  // slow DutyRampRate would smear both. Reverts when cvPlantFitActive clears.
  if (cvPlantFitActive) {
    govMode = GOV_BYPASS_SLEW;
  }

  // Field-decay ramp phase: duty slew OFF by spec — the TEST_ENTRY_RATE_A setpoint slew provides the
  // smoothness, the PID must be free to move duty as it needs. The later hold/cut/ease phases get the
  // same bypass from the sysIDRunning override path.
  if (fieldCutCcActive || protTestCcActive || chcCcActive) {
    govMode = GOV_BYPASS_SLEW;
  }

  // Manual CC square-wave test in Off mode: bypass duty slew (once the entry ramp has settled) so the edges
  // are truly instant, matching the abrupt setpoint. Default/Custom keep the normal DutyRampRate protection.
  if (TuningMode != 0 && tuningWaveform == 0 && !g_autoTestActive && !tuningSquareAbrupt &&
      testSlewMode == 0 && g_tuningEntrySettled) {
    govMode = GOV_BYPASS_SLEW;
  }

  // Major overvoltage: bypass slew for fast field collapse.
  // Triggers when battery is 0.5V (per-cell-scaled by class) above the hard-shutdown threshold —
  // by this point the fault path is already ramping; this just removes the slew limit so the
  // ramp is instant.
  if (tick.currentBatteryVoltage > (tick.alternatorHardShutdownV + 0.5f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f))) {
    govMode = GOV_BYPASS_SLEW;
  }
  // Voltage sensor failure: bypass slew
  if (!tick.voltagePlausible || tick.voltageDisagreementCritical) {
    govMode = GOV_BYPASS_SLEW;
  }

  // CV overshoot: bypass duty slew so the output current PID can drop field current without the
  // governor rate limit holding it back. Triggered by fastOvClampActive, latched until battV is
  // back within 0.02 V (per-cell-scaled by class — unscaled it sits at the 48V noise floor and
  // releases erratically) of target (anti-chatter), CV stages only.
  // Rationale + "do not revert" history: CV_Loop_Dev_Summary.md (govBypass latch section).
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
  bool isNormalMode = (mode == MODE_NORMAL_AUTO_PID || mode == MODE_NORMAL_MANUAL) && !shouldImmediatelyCutGPIO4(reason);
  bool wasNormalMode = (prevMode == MODE_NORMAL_AUTO_PID || prevMode == MODE_NORMAL_MANUAL);
  bool enteringNormal = !wasNormalMode && isNormalMode;
  bool exitingNormal = wasNormalMode && !isNormalMode;

  SystemMode newSysMode;
  if (!tick.chargingEnabled) newSysMode = SYS_MODE_OFF;
  else if (shouldImmediatelyCutGPIO4(reason)) newSysMode = SYS_MODE_FAULT;
  else if (mode == MODE_NORMAL_MANUAL) newSysMode = SYS_MODE_MANUAL;
  else if (mode == MODE_NORMAL_AUTO_PID) newSysMode = SYS_MODE_AUTO;
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
  if (lockoutWasActive && !lockoutActiveNow) {
    // Spam guard mirrors the cut announcement above: the idle stale-temp loop completes a cooldown
    // every ~2s. Show recovery at most once per minute so a genuine "resumed" isn't buried under it.
    static uint32_t lastCooldownDoneMs = 0;
    if (lastCooldownDoneMs == 0 || (uint32_t)(tick.nowMs - lastCooldownDoneMs) >= 60000) {
      queueConsoleMessage(isNormalMode ? "Cooldown complete - charging resumed" : "Cooldown complete");
      serialPrintlnNB(isNormalMode ? "Cooldown complete - charging resumed" : "Cooldown complete");
      lastCooldownDoneMs = tick.nowMs;
    }
  }
  lockoutWasActive = lockoutActiveNow;

  if (fieldCollapseTime > 0 && (tick.nowMs - fieldCollapseTime) >= activeCollapseDelay) {
    fieldCollapseTime = 0;
  }
  uint32_t aflM3 = micros();  // end of section 3: CH1 gate + stage/governor/mode transitions

  // ── Gate-tuning field sweep: end it whenever the field leaves normal AUTO ──────────────────────
  // The sweeper commands duty through the override chain, which lives in the normal AUTO body below.
  // Leaving normal AUTO — the On/Off switch, a BMS opening, a lockout, a fault, or a flip to manual —
  // returns before that chain runs, so the field is handed straight back to the shutdown ramp (or to
  // the manual duty) and goes off exactly as it always does. The sweeper cannot hold it on.
  // What does NOT happen by itself is the sweep ENDING: altSweep_tick just stops being called, so
  // without this it would resume mid-ramp the instant normal running returned — a field ramp starting
  // with nobody pressing anything, the same hazard the 30 s start-request expiry exists to prevent.
  // Tear it down here instead, which also drops the TUNING SWEEP banner on the same tick.
  if (altSweepActive != 0 && (!isNormalMode || mode == MODE_NORMAL_MANUAL)) {
    queueConsoleMessageF("Field sweep: ended (%s) — the field is back under the normal control path",
                         reasonToString(reason));
    altSweepActive = 0;
    altSweepLastEndMs = millis();
    altSweepAbortRequested = true;   // resets altSweep_tick's static phase on the next normal tick
  }

  // Charge health calibration holds the field open-loop through the same override, and needs the
  // same teardown for the same reasons: chc_tick stops being called outside normal AUTO, so the run
  // would resume mid-phase when normal running returned, and chcActive would stay latched for the
  // whole lockout — blocking every other test and the wizard with it. chcFinish is static in
  // 7_functions.ino, so the result flags are set here directly.
  if (chcActive != 0 && (!isNormalMode || mode == MODE_NORMAL_MANUAL)) {
    queueConsoleMessageF("Charge health calibration: ended (%s) — the field is back under the normal control path",
                         reasonToString(reason));
    chcActive = 0;
    chcCcActive = false;             // the sizing phase may have owned the setpoint
    chcLastEndMs = millis();
    chcAbortRequested = true;        // resets chc_tick's static phase on the next normal tick
    if (!chcResultsReady) {
      chcOk = false;
      chcResultsReady = true;
      snprintf(chcAbortMsg, sizeof(chcAbortMsg), "ended — the field left normal AUTO (%s)", reasonToString(reason));
    }
  }

  // ========== NON-NORMAL MODE: SHUTDOWN / FAULT HANDLING ==========
  // NOTE: pidLog_tick() is NOT called in the shutdown/fault path.
  if (!isNormalMode) {
    runShutdownPath(tick, mode, reason, actualDtSec, exitingNormal);
    prevMode = mode;
    return;
  }

  // ========== NORMAL MODES (MANUAL or AUTO) ==========
  // Re-check before enabling GPIO4 — fault could have arrived this tick.
  if (shouldImmediatelyCutGPIO4(reason) && !gpio4IsLow) {
    applyImmediateCut(tick, reason);
    return;
  }
  // Hold field off while Core 0 internet operation is in progress OR while
  // there are still pending HTTPS uploads in the queue (so the field can't
  // "blip" through between consecutive queued uploads). Sets fieldActiveStatus
  // to 4 (WAITING_CLOUD) so the header banner shows why the field isn't
  // engaging despite charging being enabled.
  //
  // 10-second safety timeout: if WiFi is hosed or an upload hangs, flush the
  // queue and proceed with field-on so charging isn't blocked indefinitely.
  // The in-flight Core 0 upload finishes naturally; sensorRingInFlightIndex=-1
  // means its success won't pop the ring slot (record stays for retry).
  {
    static unsigned long fieldHoldStartMs = 0;
    bool cloudBusy = (core0Busy || uxQueueMessagesWaiting(httpsQueue) > 0);
    if (cloudBusy) {
      if (fieldHoldStartMs == 0) fieldHoldStartMs = millis();
      if (millis() - fieldHoldStartMs < 10000UL) {
        fieldActiveStatus = 4;  // WAITING_CLOUD
        return;
      }
      // Timeout: drop queued requests and let charging proceed.
      // Drain item-by-item, NOT xQueueReset — each queued request OWNS a
      // ps_malloc'd payload that only the receiver frees; a raw reset
      // leaks 4-128KB of PSRAM per flushed request.
      HttpsRequest droppedReq;
      while (xQueueReceive(httpsQueue, &droppedReq, 0) == pdTRUE) {
        if (droppedReq.payload) free(droppedReq.payload);
      }
      sensorRingInFlightIndex = -1;
      queueConsoleMessage("Field-on wait timeout (10s) — flushed cloud queue, proceeding");
      fieldHoldStartMs = 0;
    } else {
      fieldHoldStartMs = 0;
    }
  }
  if (protTestManualCutActive) {
    // Manual bench protection test (mode 5): hold the field-enable line OPEN (true hard cut) for the
    // set duration. Manual mode sits above the lockout in the arbiter, so the fast-OV ladder can't
    // hold the field off here — this does. Gated here (not a per-tick re-write by protTest_tick) so
    // the enable line has no tick-rate HIGH glitch that would show on a scope during a cut test.
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

  // ========== SYSTEM ID OVERRIDE ==========
  // When a plant delay measurement test is running, bypass all setpoint
  // machinery and PID computation. Duty is commanded directly by
  // systemID_tick(). govMode is forced to GOV_BYPASS_SLEW so the
  // governor does not slew or clamp the step transitions.
  // The integrator is parked at the commanded duty so the PID resumes
  // smoothly from the operating point when the test ends.
  float sysIDDutyOut = lastAppliedDuty;

  static bool prevSysIDRunning = false;
  bool sysIDRunning = systemID_tick(sysIDDutyOut, MeasuredAmps, tick.nowMs);
  // Commissioning field-% ramp shares the identical duty-override + bumpless-resume path
  // (mutually exclusive with SystemID via the start-handler mutex). OR it in so the snapshot,
  // override, and restore logic below cover it too.
  sysIDRunning = fieldCurve_tick(sysIDDutyOut, MeasuredAmps, tick.nowMs) || sysIDRunning;
  // Field de-energize τ test (commissioning stage 7) — mutually exclusive with the others via the
  // start-handler mutex. Its RAMP phase returns false and drives the real current PID through the
  // fieldCutCcActive branch below; hold/cut/ease own duty here (cut level is MinDuty, the real
  // OV-clamp floor, so the effectiveMinDuty=0 bypass below is moot but harmless).
  sysIDRunning = fieldCut_tick(sysIDDutyOut, MeasuredAmps, tick.nowMs) || sysIDRunning;
  // Charge health calibration (stage 9): field pinned open-loop while the operator swings the
  // throttle. Same duty-override + bumpless-resume path as the field curve.
  sysIDRunning = chc_tick(sysIDDutyOut, MeasuredAmps, tick.nowMs) || sysIDRunning;
  // Manual protection-trigger test (Settings → Emergency) — same mutex family + duty-override/bumpless
  // path. RAMP returns false (the protTestCcActive branch above drives the real current PID to
  // protTestCmdA); the load-dump/graceful/ease phases own duty here. Mode 1/4 arm protTestCutPending
  // for the pre-gate fast-OV cut instead of owning duty.
  sysIDRunning = protTest_tick(sysIDDutyOut, MeasuredAmps, tick.nowMs) || sysIDRunning;
  // CV plant-fit pulse train (Step 7 of the wizard display) — same mutex family. SETTLE/PILOT/HOLD return false (the
  // cvpfCcActive branch below drives the current PID); the abrupt PULSES/RELEASE own duty here.
  sysIDRunning = cvpf_tick(sysIDDutyOut, MeasuredAmps, tick.nowMs) || sysIDRunning;
  // Gate-tuning field sweeper (ALT_GATE_TUNING_CAPTURE_SPEC.md §8) — same mutex family and the same
  // duty-override + bumpless-resume path as the field curve. Continuous ramp up and back down; the
  // session logger records what it produces.
  sysIDRunning = altSweep_tick(sysIDDutyOut, MeasuredAmps, tick.nowMs) || sysIDRunning;
  // CV stress test (commissioning stage 8) — same mutex family but a PURE OBSERVER: the normal
  // loop runs exactly as fielded (table/Lo/thermal command chain untouched) and this tick only
  // confirms tracking, counts protection events, and grades the recovery.
  cvStress_tick(tick.nowMs);

  bool sysIDJustStarted = !prevSysIDRunning && sysIDRunning;
  bool sysIDJustCompleted = prevSysIDRunning && !sysIDRunning;
  prevSysIDRunning = sysIDRunning;
  if (sysIDRunning) ctrlLimiter = 0;

  // ── Snapshot on test start (fires once, first tick sysIDRunning goes true) ──
  // All pre-test values are still clean at this point — the override block below
  // hasn't touched PID or setpoint yet this tick.
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
    currentPID.SetMode(MANUAL);
    currentPID.ResetIntegratorTo((double)sysIDDutyOut);
    pidOutput = (double)sysIDDutyOut;
    vlHasPrev = false;  // CV voltage loop is bypassed during the sweep (whole control path is under
                        // !sysIDRunning), so voltLoop_record never fires. Re-baseline the CV-interval
                        // tracker so the dead time across the test isn't logged as one giant gap.
                        // Mirrors the CV-inactive re-baseline (vlHasPrev = false) in the
                        // voltageControlActive else-branch further below.
  }

  // ── Restore on test completion ───────────────────────────────────────────────
  // Hierarchy:
  //   1. Safety wins — only restore if still in a legal normal-running state.
  //   2. Restore control architecture (mode, setpoint tracker, CV integrator).
  //   3. Bumpless seed — integrator gets CURRENT applied duty, NOT the pre-test
  //      duty. Pre-test duty is stale; conditions (voltage, RPM, temp) may have
  //      changed during the test. Current applied duty is the actual operating point.
  //   4. Let the normal control path compute the next command immediately.
  if (sysIDJustCompleted && g_sysIDResume.valid) {
    bool legalToResume = (mode == MODE_NORMAL_AUTO_PID || mode == MODE_NORMAL_MANUAL);

    if (!legalToResume) {
      // A fault or disable arrived during the test. Do not force old state back;
      // let the current fault path win on its own terms this tick and beyond.
      queueConsoleMessage(
        "SystemID: pre-test state NOT restored — system no longer in normal running state");
    } else {

      // Restore setpoint tracker, clamped to the current valid ceiling.
      // This prevents re-asserting demand that was legal pre-test but is now
      // above the RPM/thermal/user cap.
      float restoredSetpoint = clamp_f(
        g_sysIDResume.setpointLimited_snap, 0.0f, (float)MaxTableValue);
      setpointLimited = restoredSetpoint;

      // Restore CV integrator only when both pre- and post-test state are in CV.
      // If we left or entered CV during the test, let the bumpless tracker
      // re-derive cv_I naturally on the next tick instead of seeding stale state.
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
  }

  if (!sysIDRunning) {

    // ========== SETPOINT COMPUTATION (AUTO mode only) ==========
    setpointCommand = 0.0f;   // global (declared in Xregulator.ino) — read by the Control Accuracy score gate

    if (sysMode == SYS_MODE_AUTO) {

      static bool lastTuningMode = false;
      static bool lastBattHealth = false;

      // tempFilterUpdate() runs unconditionally at the TOP of this function — NOT here gated by
      // SYS_MODE_AUTO — so the filtered temp / slope / projection stay live in FAULT/MANUAL/OFF
      // cooldown too. It still runs before tempPID_tick, so the PID input is unchanged.

      // Deadman for the wizard-commanded resonance current-check: if the browser stops refreshing the command
      // (wizard closed / disconnected), auto-release so the field isn't left commanded to a stale test level.
      if (resTestActive && (millis() - resTestLastCmdMs > RES_TEST_DEADMAN_MS)) {
        resTestActive = false; resTestTargetA = 0.0f; resTestReleasing = false; ripScoreArmed = false;
        // A dead browser also ends the RPM Invaders sweep: close the fold window (folds from AUTO
        // running would poison the fixed-current table) and persist what committed — every teardown
        // path saves, including this one.
        if (ripGameFill || ripTabPendingWipe) {
          ripGameFill = false; ripTabPendingWipe = false; ripTabPendingSave = true;
        }
        queueConsoleMessage("Resonance current-check auto-released (no command refresh)");
      }

      if (batteryHealthTestActive) {
        // Active DCIR step generator. Soft protections (G1/G2 + CV/bulk iExcess) are deliberately
        // dropped during the test via the !batteryHealthTestActive gates so the step actually
        // manifests; fast-OV (priority 1.5) and the INA228 hardware OV stay live as the safety net,
        // and the test self-clears (edge-count exit, /get?bhAbort, watchdog). bhComputeDcir() still
        // rejects a run whose step didn't form cleanly.
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
        // Slew at the FIXED conservative test rate (NOT the user's SetpointRiseRate/FallRate — a user could
        // set those aggressively and slam the field on entry/exit). The DCIR fit differences SETTLED
        // end-of-dwell levels, so the slower transition reads the same resistance; entry up from rest and
        // exit back down both stay gentle and user-independent.
        setpointLimited = slew_limit_f(setpointLimited, setpointCommand,
                                       TEST_ENTRY_RATE_A, TEST_ENTRY_RATE_A, actualDtSec);
        voltageControlActive = false;
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
        // Wizard-commanded resonance current-check (§3.2): drive the loop to resTestTargetA, slewed exactly
        // like the DCIR / current-tuning square wave so ENTRY is gentle. Protections stay live; if one fires
        // the ripple sample is simply not trusted. voltageControlActive=false the whole test, so the
        // target-relative over-voltage (G1/G2) is suppressed — that is why the field can sit at 0.90×max
        // output without tripping; only the absolute fast-OV cut and INA228 comparator are live.
        //
        // EXIT is deferred (resTestReleasing, set on resTest=0): the failure this fixes is that releasing
        // straight to CV at the high held current re-armed G2 the instant voltage control resumed (the bus
        // was legitimately floated above the charge target during current control), which read as an
        // over-target event and slammed the field. So instead we slew the current to ~0 HERE first — fast
        // (SetpointFallRate; dropping the field is the safe direction) but still current-controlled with OV
        // suppressed — and only drop out of the test once the field is down. CV then re-enters from a
        // low-voltage state and ramps the field back up FROM BELOW the target, which never trips G2.
        setpointCommand = resTestReleasing ? 0.0f : resTestTargetA;
        float resFallRate = resTestReleasing ? SetpointFallRate : TEST_ENTRY_RATE_A;
        setpointLimited = slew_limit_f(setpointLimited, setpointCommand,
                                       TEST_ENTRY_RATE_A, resFallRate, actualDtSec);
        voltageControlActive = false;
        ctrlLimiter = 0;
        targetCurrent = (OutputPIDSigSrc == 2) ? MeasuredAmps : (OutputPIDSigSrc == 1) ? g_pidMA_N
                                                                                       : g_pidI_filtered;
        pidInput = (double)targetCurrent;
        pidSetpoint = (double)setpointLimited;
        pidError = setpointLimited - targetCurrent;
        currentPID.Compute();
        // Scoring gate: arm only once the COMMAND has finished ramping (setpointLimited at target — the
        // field is being driven to full test current, not sitting at the ~4% rest floor) AND the measured
        // current has actually arrived, held RIP_SCORE_HOLD_MS. Reset the instant it isn't (ramp, release,
        // a bin the alternator can't source) so the hold must be re-earned. faFiltRippleUpdate reads this.
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
        // Field-decay ramp/settle (commissioning stage 7): drive the REAL current loop to
        // fieldCutCmdA (= the commissioned SystemIDStabilizeAmps), manually slewed at the fixed
        // conservative test rate — same shape as resTest. voltageControlActive=false suppresses the
        // target-relative OV layers (G1/G2) so the level can hold; fast-OV/INA228/hard-OC stay live.
        // fieldCut_tick watches for settle and flips to the duty-override path for hold/cut/ease.
        setpointCommand = fieldCutCmdA;
        setpointLimited = slew_limit_f(setpointLimited, setpointCommand,
                                       TEST_ENTRY_RATE_A, SetpointFallRate, actualDtSec);
        voltageControlActive = false;
        ctrlLimiter = 0;
        targetCurrent = (OutputPIDSigSrc == 2) ? MeasuredAmps : (OutputPIDSigSrc == 1) ? g_pidMA_N
                                                                                       : g_pidI_filtered;
        pidInput = (double)targetCurrent;
        pidSetpoint = (double)setpointLimited;
        pidError = setpointLimited - targetCurrent;
        currentPID.Compute();

      } else if (chcCcActive) {
        // Charge health calibration sizing phase (commissioning stage 9): drive the REAL current
        // loop to chcCmdA — the share of the charge-rate column the test runs at, evaluated at the
        // speed the operator is holding. Same shape as the field-decay ramp; chc_tick watches for
        // settle, freezes the duty the loop landed on, and flips to the duty-override path for the
        // three passes. voltageControlActive=false suppresses the target-relative OV layers (G1/G2)
        // so the level can be reached; fast-OV/INA228/hard-OC stay live.
        setpointCommand = chcCmdA;
        setpointLimited = slew_limit_f(setpointLimited, setpointCommand,
                                       TEST_ENTRY_RATE_A, SetpointFallRate, actualDtSec);
        voltageControlActive = false;
        ctrlLimiter = 0;
        targetCurrent = (OutputPIDSigSrc == 2) ? MeasuredAmps : (OutputPIDSigSrc == 1) ? g_pidMA_N
                                                                                       : g_pidI_filtered;
        pidInput = (double)targetCurrent;
        pidSetpoint = (double)setpointLimited;
        pidError = setpointLimited - targetCurrent;
        currentPID.Compute();

      } else if (protTestCcActive) {
        // Manual protection-test energize (Settings → Emergency): drive the REAL current loop to
        // protTestCmdA (the user-set target field current) at the conservative test rate — same shape
        // as the field-decay ramp. voltageControlActive=false suppresses the target-relative OV layers
        // (G1/G2); fast-OV/INA228/hard-OC stay live. protTest_tick watches settle, then fires the mode.
        setpointCommand = protTestCmdA;
        setpointLimited = slew_limit_f(setpointLimited, setpointCommand,
                                       TEST_ENTRY_RATE_A, SetpointFallRate, actualDtSec);
        voltageControlActive = false;
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
        voltageControlActive = false;
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
          // A fresh sweep now starts from the low rest current the prior sweep eased to — re-arm the slewed
          // entry so a back-to-back Run doesn't slam up to the swing center.
          if (tuningWaveform == 2 && tuningSweepRequested) tuningEntryRamped = false;
          tuningSineStep(tick.nowMs, actualDtSec, tuningSinePhase, baseA, ampA, MeasuredAmps, setpointCommand);
          // Entry: ease up from the rest floor to the live sine at the FIXED conservative test rate, with
          // duty slew still engaged (govMode bypass is gated on g_tuningEntrySettled). The rising ramp meets
          // the oscillating sine within a period; once caught the sine runs clean (setpoint AND duty
          // unclamped). If it never catches (pathological), it simply stays duty-slewed — safe degradation,
          // never a slam.
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

          // tuningSquareAbrupt (commissioning CV-fit) forces a true step. For the MANUAL square test the
          // Current-tab Test Limiters slew mode decides: 0 Off = abrupt edges, 1 Default = factory setpoint
          // rate, 2 Custom = the user's SetpointRiseRate/FallRate. Either abrupt path STILL eases in from rest
          // at the fixed conservative TEST_ENTRY_RATE_A so start-up never slams the field (OV risk); only after
          // arrival (tuningEntryRamped) do the edges go abrupt — symmetric with the sine branch, and
          // g_autoTestActive keeps commissioning off this path.
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
        voltageControlActive = false;
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
          if (!isnan(tempSample)) { tuningScore.tempSum += tempSample; tuningScore.tempSampleCount++; }
          tuningScore.avgSampleCount++;
          if (tuningScore.activeTimeSec > 0.0f) {
            tuningScore.score = tuningScore.errorAccum / tuningScore.activeTimeSec;
          }
        }

        // TrackAppliedOutput() is NOT needed here — falls through to the shared
        // call at the end of the normal-mode section.

        g_tuningEntrySettled = tuningEntryRamped;  // next tick's govMode block reads this to gate the sine duty-bypass

        lastTuningMode = true;

      } else {
        // ===== NORMAL AUTO =====

        // Health test just ended. Deliberately NO current-PID reseed: unlike sysID (which runs the
        // PID in MANUAL and must reseed), the Health test kept it LIVE in AUTOMATIC — like TuningMode,
        // which also does not reseed. The integrator already tracks; normal AUTO's slew resumes
        // gently. The sysID-style ResetIntegratorTo here was the exit bump.
        if (lastBattHealth) lastBattHealth = false;

        // Detect TuningMode exit — fires exactly once.
        if (lastTuningMode) {
          tempPIDActive = false;  // PID was dormant during tuning — force the bumpless re-seed
          // tempFilterUpdate() kept BOTH the IIR filter and the slope buffer live throughout
          // tuning (no tempFilterNeedsReseed → IIR preserved). Set thermalPreserveSlopeOnResume
          // so the re-enable path also KEEPS the slope buffer instead of wiping it. Without this,
          // the re-enable wipe sets thermalSlopeBufFull=false → the setpoint drops to limit-20 for
          // 60s the instant tuning exits (TuningMode→0 turns off suppressWarmupMargin), derating
          // the field right after a test. We have a live trend, so the loop is NOT blind — keep
          // the projection and stay at limit-7.
          thermalPreserveSlopeOnResume = true;
          tuningScore = {};  // discard accumulator — commit is always manual
          tuningSquareAbrupt = false;  // never let the abrupt-step bypass leak past a tuning session
        }
        lastTuningMode = false;
        g_tuningEntrySettled = false;  // not in TuningMode → sine duty-bypass disarmed (re-arms only after the next entry ramp settles)

        // Temperature loop PID. Library timer governs Compute() cadence.
        tempPID_tick(currentMillis, actualDtSec);

        // Command chain: I_cap (RPM-table ceiling) − thermalPenalty → uTargetAmps (clamped
        // [0, MaxTableValue], the table+thermal limit and upper bound for the CV loop) → Icv
        // (CV PI output, clamped [0, uTargetAmps]). Icv is never written back to the thermal state.
        // HiLow is applied at table-load time (loadCapTablesForMode), not by runtime halving.

        float I_cap;
        if (capLimitMode == 1 && tick.currentBatteryVoltage > 0.5f) {
          I_cap = interpolateRPMTable(RPM, rpmCapPowerTable) / tick.currentBatteryVoltage;
        } else {
          I_cap = getCapCurrentForRPM(RPM);
        }

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

        // Charge-rate Hi->Lo ceiling glide. Armed by applyChargeRateMode() whenever the mode drops to Low: the
        // table has already dropped to the Low cap, but the alternator is still at the old output. Hold
        // the ceiling up and ramp it down to the new (lower) uTargetAmps at MaxTableValue A/s (~1s full
        // scale) so the field tracks it instead of stepping. The ramp shrinks the drop but cannot by
        // itself stop an iExcess trip: both iExcess detectors compare actual current against a reference
        // that descends during the glide (CV: setpointLimited; bulk: i_ceiling_pre_ov), and the field
        // current lags ABOVE any descending reference — so the glide ALSO drives modeCapGlideSuppress
        // (glide window + a settling-tail grace), which both iExcess detectors read to hold their EMA
        // at 0 for the duration (see those blocks below).
        // The HARD protections stay live throughout — hardware OV, fast OV, and (unless Group 0 is toggled off) the HardOCTripAmps trip.
        // The glide self-clears once the held ceiling reaches the new cap (or an up-switch raises it past).
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
        // True while the glide runs AND for MODE_CAP_GLIDE_GRACE_MS after it self-clears, so iExcess
        // ignores both the deliberate command descent and the field-current settling tail that follows.
        // Both iExcess detectors below read this. (uint32_t subtraction wraps cleanly; once past the
        // grace window the difference is large and the term is false.)
        bool modeCapGlideSuppress = modeCapSlewActive
                                    || (modeCapSlewEndMs != 0 && (uint32_t)(currentMillis - modeCapSlewEndMs) < MODE_CAP_GLIDE_GRACE_MS);

        // User overrides
        // Zero-current float (UseFloat=2): same zero-command regulation as MaintainMode, but entered
        // automatically by the float stage — alternator carries the house loads, battery rests at 0 A.
        // Unlike the manual MaintainMode override, the stage machine (and its rebulk criteria) stays live.
        zeroFloatActive = (EFFECTIVE_USE_FLOAT == 2) && !inBulkStage && !inAbsorptionStage && !inIdleStage
                          && MaintainMode == 0 && TargetVoltageMode == 0 && !CVTuningMode;
        if (MaintainMode == 1 || zeroFloatActive) uTargetAmps = 0;

        // ── House-load / other-source offset + battery charge-current ceiling (G4) ─────────
        // MeasuredAmps − Bcur is house loads MINUS anything else charging the bank (solar, shore, DC-DC):
        // the node balance is I_alt + I_other = I_batt + I_load. Light EMA so it tracks real changes within
        // ~1 s without chasing INA ripple. Adding it to the operator's battery charge limit converts that
        // limit into an alternator-amp ceiling that stays correct however many sources are on the bus, and
        // min-selects it into the command ceiling — applies in bulk AND CV (icvCeil inherits uTargetAmps).
        // Deliberately NOT clamped ≥0: a second source out-producing the house loads makes this legitimately
        // negative, and clamping it there let the alternator overshoot the battery limit by exactly that
        // source's contribution. Only the resulting ceiling is floored at 0.
        // Gated on a configured INA228 battery shunt; 0 = feature disabled.
        static float loadOffsetFilt = 0.0f;  // EMA of MeasuredAmps − Bcur (A; negative = other sources exceed loads)
        {
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
        if (BattLimitEnable && BattCurrentLimitA > 0.0f && BatteryCurrentSource == 0 && ShuntResistanceMicroOhm > 0 && BatteryShuntPresent) {
          float battCeilAlt = fmaxf(0.0f, BattCurrentLimitA + loadOffsetFilt);
          // Console note when the battery limit is the binding ceiling AND output is actually riding it
          // (e.g. absorption approaching target slowly, or bulk capped below the RPM table): once after
          // 5 s sustained, reminder at most every 10 min. Diagnosis aid, throttled against spam.
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
        // Enters the same min-select as the battery charge limit above. With a battery shunt the
        // CCL is a BATTERY-current limit, so the house-load offset converts it to an alternator
        // ceiling exactly like BattCurrentLimitA (battery current stays a LIMIT, never the process
        // variable). Without a shunt it caps alternator amps directly — conservative by
        // construction, since battery charge current = alternator output − house loads. CCL 0 is a
        // legitimate command (cold/full/cell-high battery): the ceiling goes to 0 and recovery
        // rides the existing rate-governed ramp when the authority raises it again.
        bool dvccCclBinding = false;
        if (dvccEn == 1 && dvccState == 3 /*FOLLOWING*/ && !cxOwnsBatteryNow() && !isnan(dvccCclA)) {
          bool shuntBasis = (BatteryCurrentSource == 0 && ShuntResistanceMicroOhm > 0 && BatteryShuntPresent);
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
        // Fires on a SUSTAINED current excess over the CV command: an EMA of
        // (MeasuredAmps − setpointLimited) crossing E = clamp(IExcessFrac × setpointLimited,
        // floor, ceil). Voltage-gated to near target (IBV > target − IExcessArmMarginV) so it
        // can't fire during ramp-up; IExcessEnable=false (Group 3 toggle), testProtectionsEnabled=false,
        // TuningMode=1, or the battery-health DCIR test (batteryHealthTestActive) inhibit it
        // (else branch releases the latch and reseeds the EMA).
        // Full math + rationale: Working Markdown Docs/iExcess_Redesign_Spec.md.
        {
          const float K_IE = 1.0f;
          static bool iExcessActive = false;   // latched fire state (held until the average clears)
          static float mExcessEma = 0.0f;      // EMA of signed deviation iActual − setpointLimited (A)
          static bool postProtMismatch = false;  // wind-down gate: suppress an iExcess re-fire during the field-TC tail AFTER a protection releases (the "double-penalty" guard)
          static uint32_t postProtClearMs = 0;   // millis the wind-down safety cap is measured from (refreshed every clamped tick → counts from the release edge)
          // cv_I capture/reseed: preEventCvI above + the unified falling-edge reseed
          // in the bumpless tracker block.

          // MaintainMode / zero-current float: the command is deliberately 0 while the alternator supplies
          // the house loads, so percent-of-command has no meaning — both iExcess regimes disarm (there,
          // OV groups / Load Dump / hard OC stay armed). altZeroOutput is the generalization to zero
          // measured OUTPUT, any cause: delivering ~0 A no fire can move the bus, so this arm AND G1/G2
          // stand down together; Load Dump and the hard/absolute cuts stay armed.
          if (IExcessEnable && testProtectionsEnabled && !TuningMode && !batteryHealthTestActive && voltageControlActive
              && MaintainMode == 0 && !zeroFloatActive && !altZeroOutput && (IBV > ChargingVoltageTarget - IExcessArmMarginV)) {
            // Affine trip line: slope·command + base, floor/ceiling guarded. Commissioning fits slope to the
            // measured ripple slope and base to ripple-at-idle + Safety Margin, so the line rides a fixed
            // margin above the ripple (base 0 = legacy through-origin behaviour).
            float E = fmaxf(IExcessFloorA, fminf(IExcessFrac * setpointLimited + IExcessBaseA, IExcessCeilA));
            // Field bled >= tau: release the hold, but only once the excess EMA (last tick's value) is
            // back at/under the fire line — below E the latch is pure hysteresis-tail lag. Clearing while
            // EMA > E would hand the latch to the edge-triggered fire above, re-running the full fire
            // side effects (count/console/cv_I snap/inner-PID reset) every tick until the EMA decays.
            if (tauReleaseNow && mExcessEma <= E) iExcessActive = false;

            float ieActual = MeasuredAmps;

            // Post-protection wind-down gate (the "double-penalty" guard). When fastOV/hardOV releases, the
            // field current keeps lagging ABOVE the collapsed command for ~1–2 field TCs (L/R fall), so
            // without this guard it re-fires iExcess on the release edge — zeroing cv_I a SECOND time; and
            // because preEventCvI has already refreshed down to the first reseed during the quiet gap, the
            // unified reseed then halves cv_I AGAIN (28→14→7 observed). That second halving is the double
            // penalty. The while-clamped EMA hold below does NOT cover this — it stops the instant the clamp
            // releases, which is exactly when the wind-down begins. So: arm while any protection owns the
            // clamp, then HOLD past release until the wind-down excess has decayed back inside the normal
            // band (self-clearing, no fixed timer), bounded by a safety cap so a genuinely sustained
            // over-current is never muted forever (the voltage backstops own that case regardless).
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
              // Another protection already owns the clamp (clampOwned), OR it just released and the field is
              // still winding down (postProtMismatch). Either way the actual-vs-command mismatch is THAT
              // protection's own doing, not a real over-current — dev would jump to ~full current, crossing
              // E in a few ms. Hold the EMA at 0 so we don't fire a redundant iExcess. When both clear the
              // EMA restarts from 0 and only a genuinely sustained excess can fire. (The while-clamped half
              // is a deliberate keep vs spec §7 — verified necessary: deleting it re-fires ~8 ms after every
              // fastOV event. The postProtMismatch half extends that through the wind-down tail.)
              // modeCapGlideSuppress: the Hi->Lo ceiling glide is deliberately ramping setpointLimited
              // down — the field current lags ABOVE the descending command (plant can't fall as fast
              // as the ramp), and that lag is the glide's own doing, not a fault.
              mExcessEma = 0.0f;
            } else {
              // dt-aware EMA of the raw-current deviation (raw Bcur/MeasuredAmps; the EMA does all
              // the filtering). dt = real elapsed control-tick seconds, so I²C jitter stretching
              // a tick can't corrupt the time constant — same pattern as g_fastOvDvdt.
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
                // One-shot cv_I drain on the rising edge. IExcessKBleed = 0: snap cv_I to zero
                // (deepest starting point for recovery). > 0: subtract K_bleed × averaged-excess ×
                // dtSec from cv_I once. Both zero the current COMMAND in one tick (1e9 fall-rate
                // override on setpointLimited); the duty collapses via the inner-PID reset above.
                // Sustained per-tick drain for the rest of the event comes from awBleedAmpS in the
                // bumpless tracker block. Final reseed is the unified falling-edge reseed when ALL
                // protections clear. The IExcessKBleed knob only sets post-event recovery depth.
                if (IExcessKBleed <= 0.0f) {
                  cv_I = 0.0f;
                } else {
                  cv_I = fmaxf(0.0f, cv_I - IExcessKBleed * mExcessEma * actualDtSec);
                }
                iExcessActive = true;
              }

              // While latched: re-apply the proportional cap and hold govBypass each tick;
              // release on hysteresis. Cap input is mExcessEma (the averaged signal) so capReason
              // is honest without chattering on ripple. Deliberately not a hard 0-cap — that
              // would deepen recovery undershoot. Release when the average falls below
              // E × IExcessRelFrac (scale-aware hysteresis).
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
            // Gate closed (battV below target − ArmMargin, or protections off / tuning) — release
            // and reseed the EMA so a later CV entry or setpoint step doesn't carry stale charge
            // into a startup fire. Unified reseed fires on the falling edge of fastOvClampActive.
            iExcessActive = false;
            mExcessEma = 0.0f;
            // Do NOT clear postProtMismatch here. At a low/float setpoint the field cut sags battV below the
            // arm line mid-wind-down, briefly closing this gate; wiping the guard on that dip is exactly what
            // let iExcess re-fire when the gate re-opened. The guard self-clears via its decay/timeout while
            // the gate is open and counts from the release edge, so a genuine sustained CV exit still clears it.
            g_mExcessEma = 0.0f;
            g_iExcessThreshold = 0.0f;
          }
          g_iExcessActive = iExcessActive;
          g_iExcessDutyCap = 100.0f;  // retired
        }

        // ── iExcess supervisor — BULK sub-mode (Group 3, current-control phase) ──────
        // Sibling of the CV iExcess above, for battV FAR below target (bulk). Gate is the
        // strict complement (IBV ≤ target − IExcessArmMarginV), so the two hand off at that
        // line with no gap/overlap. Compares actual against i_ceiling_pre_ov (the commanded
        // ceiling) not setpointLimited, with a looser IExcessFracBulk, so a normal RPM ramp
        // produces no excess — only a true overshoot above the mechanical ceiling fires it.
        // Response binds via fastOvCurrentCap RELATIVE TO THE CEILING (zeroing cv_I can't
        // collapse a saturated bulk setpoint); recovery shares the unified fastOvClampActive
        // path. Full rationale: Working Markdown Docs/iExcess_Redesign_Spec.md.
        {
          const float K_IE = 1.0f;
          static bool iExBulkActive = false;     // latched fire state
          static float mExcessEmaBulk = 0.0f;    // EMA of signed deviation iActual − i_ceiling_pre_ov (A)
          static bool postProtMismatchBulk = false;  // wind-down gate (mirror of the CV detector's postProtMismatch): suppress a redundant bulk re-fire during the field-TC tail AFTER a clamp releases
          static uint32_t postProtClearMsBulk = 0;   // millis the wind-down safety cap is measured from (refreshed every clamped tick → counts from the release edge)

          if (IExcessEnable && testProtectionsEnabled && !TuningMode && !batteryHealthTestActive && voltageControlActive
              && MaintainMode == 0 && !zeroFloatActive
              && (IBV <= ChargingVoltageTarget - IExcessArmMarginV)) {
            // Affine trip line vs the commanded ceiling: slope·ceiling + base + CC offset, floor/ceiling
            // guarded. The UI keeps IExcessFracBulk equal to IExcessFrac, so this runs PARALLEL to the CV
            // line, sitting IExcessCcOffsetA amps above it — the CC phase tolerates that much more
            // command-vs-actual error, catching only overshoots above ceiling. Always alternator-domain.
            float floorBulk = IExcessFloorA;
            float E = fmaxf(floorBulk, fminf(IExcessFracBulk * i_ceiling_pre_ov + IExcessBaseA + IExcessCcOffsetA, IExcessCeilA));
            // Field bled >= tau: EMA-gated hold release — same rationale as the CV detector above.
            if (tauReleaseNow && mExcessEmaBulk <= E) iExBulkActive = false;

            // Post-protection wind-down gate — bulk mirror of the CV detector's postProtMismatch.
            // A Hi->Lo ceiling glide ramps i_ceiling_pre_ov DOWN; when
            // the glide flag drops, the field current still lags ABOVE the freshly-lowered ceiling for
            // ~1 field TC, so the bare test would let the bulk EMA rebuild and cross E — a redundant trip
            // (counter bump + log + brief re-clamp). Arm while any clamp owns the drop, then HOLD past
            // release until the excess decays back inside the band (self-clearing, no fixed timer),
            // bounded by a safety cap so a genuinely sustained over-current still fires (the voltage
            // backstops own that interim regardless).
            const uint32_t kPostProtMismatchMaxMs = (uint32_t)fmaxf(150.0f, 3.0f * (float)fieldDecayTauMs);  // 3× the commissioned worst-case field-drain time (longest endpoint of the drain-vs-RPM line), floored 150ms; backstop only — normal release is the decay test
            bool clampOwnedBulk = fastOvClampActive || iExBulkActive || modeCapGlideSuppress;  // include bulk iExcess's OWN clamp so the guard arms after a bulk-iExcess-only cut
            if (clampOwnedBulk) {
              postProtMismatchBulk = true;
              postProtClearMsBulk  = currentMillis;  // refreshed every clamped tick, so the cap counts from the release edge
            } else if (postProtMismatchBulk) {
              bool decayed  = (MeasuredAmps - i_ceiling_pre_ov) <= E;  // current back below the (settled) ceiling → wind-down done
              bool timedOut = (currentMillis - postProtClearMsBulk) > kPostProtMismatchMaxMs;
              if (decayed || timedOut) postProtMismatchBulk = false;
            }

            if (!iExBulkActive && (clampOwnedBulk || postProtMismatchBulk)) {
              // Another protection (a load dump, the only other fast supervisor active in bulk) owns the
              // clamp and has collapsed the command (clampOwnedBulk), OR it just released and the field is
              // still winding down above the lowered ceiling (postProtMismatchBulk). Either way the
              // over-ceiling reading is THAT protection's own doing, not a real over-current — hold the
              // EMA at 0 so we don't fire a redundant bulk iExcess. When both clear the EMA restarts from 0
              // and only a genuinely sustained excess can fire.
              // modeCapGlideSuppress: the Hi->Lo ceiling glide ramps i_ceiling_pre_ov DOWN, but the
              // field current lags above the descending ceiling (it can't fall as fast as the
              // ramp) — that lag reads as over-current and was the bulk false-trip on a mode switch.
              // The glide owns this drop, so hold the EMA at 0 for the glide plus the wind-down tail.
              mExcessEmaBulk = 0.0f;
            } else {
              float tauSec = IExcessTau * 0.001f;
              float alpha  = actualDtSec / (tauSec + actualDtSec);
              mExcessEmaBulk += alpha * ((MeasuredAmps - i_ceiling_pre_ov) - mExcessEmaBulk);  // vs commanded ceiling, not slewed setpoint

              if (!iExBulkActive && mExcessEmaBulk > E) {
                cv_I_aw_cap = cv_I;         // cap bumpless tracker ceiling to pre-event level
                g_iExcessCount++;           // shared iExcess trip counter — CV + Bulk detectors, both alternator-current (no battery-domain detector feeds it)
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
            // Keep postProtMismatchBulk (see the CV mirror): a gate dip during the field wind-down must not
            // wipe the guard. It self-clears via its decay/timeout while the gate is open.
          }
          g_iExcessBulkActive = iExBulkActive;  // export for PID/CV log flag + dashboard
        }

        // ── iExcess live-sparkline aggregation (every control tick) ──────────────────
        // Accumulate the per-CSV1-frame worst case: peak averaged excess and min threshold E
        // over the ticks where a detector gate is open (g_iExcessThreshold > 0 = armed). The
        // CSV1 sender ships and resets these. The "fired" colour comes from protEventMask
        // (bit PROT_EVT_IX, latched above on a real cap), not from these aggregates.
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
        // Three-tier cascade: LoadDumpN1 consecutive samples above LoadDumpDtThresh1, LoadDumpN2 above
        // LoadDumpDtThresh, LoadDumpN3 above LoadDumpDtThresh3 (counts are user settings; time to act =
        // N x the ~5 ms INA fast-mode cadence). INA228 noise is alternating-sign (high/low/high/low) —
        // two consecutive same-direction crossings cannot be measurement noise. Tier 1 catches
        // hard-switched FET disconnects; tiers 2/3 catch relay-contact events spread over multiple samples.
        // Gate: LoadDumpEnable (Group 5 toggle — the only disarm; global protections toggle does not apply),
        // voltageControlActive (= !inIdleStage; bulk/absorption/float/TVMode/MaintainMode) and fast INA228 reads active (5ms cadence).
        // On detection: snaps cv_I = 0 on rising edge and collapses setpointLimited + fastOvCurrentCap to 0.
        // Recovery: AW bleed drives recovery naturally — same path as fastOV.
        {
          // Statics live at this scope so the gate-closed else can clear them.
          // Otherwise, a load dump active at the moment CV exits leaves
          // ldWasActive=true; on CV re-entry the rising-edge cv_I=0 snap and
          // g_loadDumpCount increment are skipped for the next event.
          static bool ldWasActive = false;
          static int ldCount1 = 0;  // consecutive samples above LoadDumpDtThresh1
          static int ldCount2 = 0;  // consecutive samples above LoadDumpDtThresh
          static int ldCount3 = 0;  // consecutive samples above LoadDumpDtThresh3
          if (LoadDumpEnable && voltageControlActive && inaFastModeActive && HAS_BATT_SHUNT) {  // no shunt → dBcur/dt is noise; fast-OV dV/dt is the load-dump backstop
            if (g_dBcur_dt > LoadDumpDtThresh1) {
              ldCount1++;
            } else {
              ldCount1 = 0;
            }
            if (g_dBcur_dt > LoadDumpDtThresh) {
              ldCount2++;
            } else {
              ldCount2 = 0;
            }
            if (g_dBcur_dt > LoadDumpDtThresh3) {
              ldCount3++;
            } else {
              ldCount3 = 0;
            }
            bool ldNow = (ldCount1 >= LoadDumpN1) || (ldCount2 >= LoadDumpN2) || (ldCount3 >= LoadDumpN3);
            if (ldNow) {
              fastOvCurrentCap = 0.0f;
              capReasonTick = CAP_REASON_LOADDUMP;  // hard cutoff — always the binding cap
              setpointLimited = 0.0f;
              fastOvClampActive = true;
              if (!ldWasActive) {
                cv_I = 0.0f;            // snap voltage-loop integrator on rising edge
                // Collapse inner PID integrator too (same as iExcess / hard-OV): without
                // it a wound-up integrator resists the setpoint collapse and grinds duty
                // down at ~40%/s instead of reaching MinDuty in 1-2 cycles. Load dump is
                // the most catastrophic over-current, so it gets the fastest field collapse.
                currentPID.ResetIntegratorTo(0.0);
                g_loadDumpCount++;
                queueConsoleMessageF("Load dump #%lu detected — output cut, inner PID integrator reset (dBcur/dt=%.0f A/s)",
                                     (unsigned long)g_loadDumpCount, g_dBcur_dt);
              }
            }
            // Falling-edge cv_I reseed handled by the unified reseed in the bumpless
            // tracker block — fires once all protection paths have cleared.
            g_loadDumpActive = ldNow;
            ldWasActive = ldNow;
          } else {
            // Gate closed — clean state so next gate-open starts fresh.
            ldCount1 = 0;
            ldCount2 = 0;
            ldCount3 = 0;
            ldWasActive = false;
            g_loadDumpActive = false;
          }
        }

        // ── Re-evaluate govMode now that iExcess + load-dump have had a chance to
        // set fastOvClampActive. The initial govMode decision (above) only saw the
        // fastOV supervisor's contribution; without this, a pure iExcess or load-dump
        // event collapses setpointLimited instantly (via the fall-rate override at
        // effectiveFallRate=1e9) but the governor still slews duty at DutyRampRate
        // (80%/s) instead of snapping to the rpmMinDuty/MinDuty floor in one tick.
        // BYPASS still respects rpmMinDuty and MinDuty via governor_apply's pre-clamp,
        // so the tach floor is preserved — only the descent speed changes.
        if (fastOvClampActive && voltageControlActive) {
          govMode = GOV_BYPASS_SLEW;
        }

        // ── Apply fastOvCurrentCap to uTargetAmps (fastOV + iExcess + load dump) ──
        uTargetAmps = fminf((float)uTargetAmps, fastOvCurrentCap);

        // Who set the ceiling? uTargetAmps starts at I_cap − thermalPenaltyAmps and every stage above
        // can only lower it, so surviving that chain unchanged means the thermal derate is the binding
        // ceiling. Read one tick later by thermalAccuracyScore_tick as its authority gate. The 2 A floor
        // is the binding-constraint signal: the REVERSE PID floors penalty at 0 when cool, so a penalty
        // this large only exists while the loop is actively holding temperature down.
        g_thermalOwnsCeiling = (thermalPenaltyAmps > 2.0f)
                               && ((float)uTargetAmps >= g_I_cap - thermalPenaltyAmps - 0.5f);

        // ── CV command ceiling ──
        // Icv/cv_I is the upper-loop output the inner PID tracks — an ALTERNATOR-current command.
        // Its ceiling is the fully-derated alternator command ceiling (RPM/thermal/user/battery
        // charge limit/fastOV), all already folded into uTargetAmps above.
        float icvCeil = (float)uTargetAmps;

        // TargetVoltageMode: run CV at a user-specified voltage target.
        // Forces float-equivalent stage flags so voltageControlActive goes true
        // below, then overrides ChargingVoltageTarget with the user value.
        // All current limits (RPM cap, thermal penalty, MaxTableValue, user
        // overrides) remain fully active — this only changes the voltage target.
        // cv_I is seeded by bumpless transfer tracking on CV entry — no explicit reseed needed.
        if (TargetVoltageMode == 1) {
          // Do NOT touch inBulkStage / inAbsorptionStage here. updateChargingStage() is
          // suppressed while TVMode=1, so those flags are irrelevant during execution.
          // Mutating them caused all three stage flags to be false on TVMode exit, which
          // fell through to the FLOAT branch on the first updateChargingStage() call back.
          ChargingVoltageTargetReq = TargetVoltageSetpoint;   // slewed into ChargingVoltageTarget below
          if (enteringTargetVoltageMode) {
            pidLog_enteringTargetVoltageMode = 1;
            queueConsoleMessageF("TargetVoltageMode: active, target=%.2fV", TargetVoltageSetpoint);
          }
        }

        // voltageControlActive: true in bulk, absorption, float, TargetVoltageMode (= !inIdleStage),
        // and MaintainMode (forced true below — ceiling-enforcer pattern, Groups 1/2 stay armed).
        // False only in idle (UseFloat=0 post-absorption).
        voltageControlActive = !inIdleStage;
        if (TargetVoltageMode == 1) voltageControlActive = true;  // force CV active even from idle
        if (MaintainMode == 1) {
          // Ceiling enforcer: PI runs at BulkVoltage but uTargetAmps=0 caps Icv→0, so setpoint stays 0; Groups 1/2 arm.
          voltageControlActive = true;
          ChargingVoltageTargetReq = BulkVoltage;
          if (enteringMaintainMode) {
            queueConsoleMessage("MaintainMode: active, targeting 0 net battery amps");
          }
        }
        // CV stress test (commissioning stage 8 / standalone Tuning ▸ Stress Test): force CV at the achievable target it computed —
        // same hook as TargetVoltageMode — so the throttle snap can push the bus over a REACHABLE setpoint.
        // All current/thermal/OV limits stay fully active; the test only pins the voltage target.
        {
          static bool lastCvStressForce = false;
          if (cvStressForceCV) {
            // Bumpless handoff onto the pinned target (rising edge, CV already active). Phase 1 parks
            // cv_I at ~icvHi − Kp·boost·e — satHi blocks pre-build while Icv rails against the
            // unreachable stage target — so without a re-seed the whole P-boost contribution (~19 A at
            // auto gains) leaves the command as the target lands: 0.4 V dip + bare-Ki crawl back
            // (08-05 bench). Seed against the PINNED target's error (not the still-slewing
            // ChargingVoltageTarget), capped at delivered current so the handoff can only shed.
            // A CV-inactive start is left to the enteringCV seed below.
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
        // Detect CV entry so the voltage loop fires immediately on the first CV tick.
        // (lastVoltageControlActive is a global: the MANUAL branch and the shutdown/
        // commission-idle early exits — which never reach this line — all reset it.)
        bool enteringCV = (!lastVoltageControlActive && voltageControlActive);
        lastVoltageControlActive = voltageControlActive;

        // ===== WAVEFORM GENERATOR: voltage square-wave generator (CVTuningMode) =====
        // Dithers ChargingVoltageTarget between base (HIGH) and base−amp (LOW) so the
        // CV loop step response (settling time, overshoot) can be measured and scored.
        // Only runs in the NORMAL AUTO path — incompatible with inner-loop TuningMode.
        {
          static bool lastCVTuningMode = false;

          // Discard accumulator on CVTuningMode turn-off — commit is always manual.
          // Do NOT snap the real target back to base here: if the test is turned off during a
          // HIGH phase the actual voltage is still near the peak, so an instant drop of the
          // target below it trips the relative fast-OV / iExcess protections. Arm a wind-down
          // instead so the down-slew glides the target back to base at vTgtRampDn.
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
            // Capture base target and initial conditions once per test.
            // Base is ALWAYS the CV setpoint (ChargingVoltageTargetReq = the stage's commanded
            // voltage), never the slewed ChargingVoltageTarget: capturing the slewed value lets a
            // test started before the prior test's wind-down finished latch the old HIGH peak as the
            // new base, ratcheting the wave up toward OV. Req is bounded by the stage (Bulk/Abs/Float).
            if (!cvTuningScore.testStarted) {
              cvBaseTarget = ChargingVoltageTargetReq;
              cvTuningScore.battVAtStart = IBV;
              cvTuningScore.socAtStart = (float)SOC_percent / 100.0f;
              cvTuningScore.lastToggleMs = currentMillis;
              cvTuningScore.waveHigh = false;  // start in LOW phase (at normal setpoint); test steps UP
              cvTuningScore.phaseStartMs = currentMillis;
              cvTuningScore.testStarted = true;
            }

            // Commit on parameter change if enough cycles have scored.
            // Preserve current phase and restart the half-period timer from now so the
            // next toggle is a full symmetric half-period away regardless of when the
            // param change arrived.
            if (cvTuningParamChanged) {
              bool wasHigh = cvTuningScore.waveHigh;
              if (cvTuningScore.scoredHighCount >= 1) commitCVTuningRecord();
              else cvTuningScore = {};
              cvTuningScore.waveHigh = wasHigh;
              cvTuningScore.lastToggleMs = currentMillis;
              cvTuningScore.testStarted = true;
              cvTuningParamChanged = false;
            }

            // Half-period toggle — cvWavePeriodSec is the full period, so each half is / 2
            uint32_t halfPeriodMs = (uint32_t)cvWavePeriodSec * 500UL;
            if (currentMillis - cvTuningScore.lastToggleMs >= halfPeriodMs) {
              bool goingHigh = !cvTuningScore.waveHigh;
              cvTuningScore.waveHigh = goingHigh;
              cvTuningScore.lastToggleMs = currentMillis;
              cvTuningScore.halfPeriodCount++;
              // 1 half-period ring-in: initial LOW (resting at normal setpoint) is unscored.
              // ringInDone becomes true on the first LOW→HIGH toggle.
              // Guard on phaseStartMs > 0 prevents the initial HIGH→LOW from finalizing
              // a never-started scored HIGH phase.
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
                // Start of a new scored HIGH phase. cvBaseTarget intentionally NOT refreshed here (captured once at test start) so the wave can't ratchet up when a half-period ends before the slew finishes.
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

            // Override ChargingVoltageTarget for this tick.
            // LOW phase = normal setpoint (battery rests here naturally).
            // HIGH phase = normal setpoint + amplitude (the step-up response being scored).
            ChargingVoltageTargetReq = cvTuningScore.waveHigh ? (cvBaseTarget + cvWaveAmplitudeV)
                                                              : cvBaseTarget;   // slew below applies if vTgtRampEnable (study on/off)
          }
        }

        // ── DVCC follow: external charge-voltage limit (CVL) clamp ──────────────────────────
        // Applied at the single choke point AFTER every ChargingVoltageTargetReq writer (stage,
        // manual target, Maintain, stress test, square-wave), so with follow on NO target of any
        // kind ever exceeds the authority's CVL (decision 2026-08-19: no exemptions — bench tuning
        // that needs exact targets runs with follow off). Clamp-only: min() can never raise the
        // target. Down-steps ride the existing bus-paced slew below; release rides the rate-
        // governed up-slew — never a step injection. Bypassed while the commissioning wizard owns
        // the battery (its prerequisite copy also says to switch follow off).
        bool dvccCvlBinding = false;
        if (dvccEn == 1 && dvccState == 3 /*FOLLOWING*/ && !cxOwnsBatteryNow()
            && !isnan(dvccCvlV) && dvccCvlV < ChargingVoltageTargetReq) {
          ChargingVoltageTargetReq = dvccCvlV;
          dvccCvlBinding = true;
        }

        // ── Voltage-target slew (bidirectional) — outer layer; master switch vTgtRampEnable ──
        // Rate-limits the REAL ChargingVoltageTarget toward the commanded ChargingVoltageTargetReq at
        // vTgtRampUp / vTgtRampDn (V/s). ChargingVoltageTarget is what both the CV PI error and the
        // RELATIVE over-voltage protections read, so this limits only how fast the trip points move —
        // the absolute backstops (AlternatorHardShutdownV, the INA228 comparator) are unaffected.
        // A commanded DROP collapses in one tick through the dead band far above battery voltage (the field
        // is already saturated there, so the target is inert), then glides the final approach BUS-PACED:
        // descent holds while getFiltV lags more than CV_TGT_PACE_LEAD_V behind the reference, so a gliding
        // reference can never outrun delivery and open the measured-vs-target gap to the OV margin.
        // Snap (no ramp) when not doing voltage control or on the first CV tick — a bumpless seed, not a ramp
        // up from a stale target. A waveform-exit wind-down overrides the instant path so an elevated target
        // always glides back to base; idle / CV-entry still snap and abort the glide. In the manual CV test
        // the Test-Limiters slew mode overrides vTgtRampEnable (0 Off / 1 Default / 2 Custom); Off there still
        // keeps the base duty slew engaged — conservative, not required, since the field-slew safety is the
        // ramp-to-zero at test exit (cvWaveExitWindDown).
        bool cvManualTest = (CVTuningMode != 0 && !g_autoTestActive);
        bool tgtSlewOff   = cvManualTest ? (cvTestSlewMode == 0)
                                         : (vTgtRampEnable == 0 && !g_autoTestActive);
        float vtDflt   = VTGT_RAMP_DEFAULT * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f);
        float cvRampUp = (cvManualTest && cvTestSlewMode == 1) ? vtDflt : vTgtRampUp;
        float cvRampDn = (cvManualTest && cvTestSlewMode == 1) ? vtDflt : vTgtRampDn;
        bool forceSnap = (!voltageControlActive || enteringCV) ||
                         (tgtSlewOff && !cvWaveExitWindDown);
        if (forceSnap) {
          ChargingVoltageTarget = ChargingVoltageTargetReq;            // snap (disabled / idle / CV entry)
        } else if (ChargingVoltageTargetReq > ChargingVoltageTarget) {
          float step = (cvRampUp > 0.0f) ? (cvRampUp * actualDtSec) : 1.0e9f;
          ChargingVoltageTarget = fminf(ChargingVoltageTargetReq, ChargingVoltageTarget + step);
        } else if (ChargingVoltageTargetReq < ChargingVoltageTarget) {
          // Snap-to-proximity: collapse the inert dead band in one tick, but never below the
          // commanded setpoint (fmaxf) nor below IBV+margin — so measured stays under target and the
          // relative OV protections can't trip. vTgtRampDn then owns the final approach. Self-arming:
          // fires only while target >> measured (saturated); stays disengaged once measured is near
          // target, which is exactly the delicate absorption→float-on-a-full-battery case.
          float fastFloor = IBV + 0.2f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f);
          if (ChargingVoltageTarget > fastFloor) {
            ChargingVoltageTarget = fmaxf(ChargingVoltageTargetReq, fastFloor);
          }
          float step = (cvRampDn > 0.0f) ? (cvRampDn * actualDtSec) : 1.0e9f;
          float tgtDn = ChargingVoltageTarget - step;
          // Bus-paced floor: a fixed glide outruns delivery on a soft plant (lithium 7.5 mV/A bench:
          // 150 mV/s glide vs 68 mV/s achievable bus opened the gap to the G2 margin — 4 fires,
          // 2026-07-22 131Drop log). Hold the descent while the bus lags more than the lead. Pace on
          // getFiltV, never g_ovIbvFilt: ripple troughs on the comparator's own signal would ratchet
          // the floor down and crests re-open the gap. Tests keep their deliberate target trajectories —
          // except the stress test's settle, which pins one target and waits (nothing graded): it keeps
          // the floor, because CvStressDropV (0.10) equals the shipped OvMeasMarginV (0.100) and an
          // unpaced landing puts the still-settled bus exactly on the G2 line.
          if (!cvManualTest && (!g_autoTestActive || cvStressForceCV)) {
            float paceFloor = getFiltV() - CV_TGT_PACE_LEAD_V * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f);
            tgtDn = fminf(fmaxf(tgtDn, paceFloor), ChargingVoltageTarget);
          }
          ChargingVoltageTarget = fmaxf(ChargingVoltageTargetReq, tgtDn);
        }
        if (cvWaveExitWindDown &&
            (!voltageControlActive || ChargingVoltageTarget <= ChargingVoltageTargetReq + 0.001f)) {
          cvWaveExitWindDown = false;
        }

        // ── Voltage target rise governor (inner windup guard) ──
        // Clamps voltageTargetSlewed to IBV + e_needed, where e_needed is the voltage error the current
        // cv_I can support at the present current cap. Prevents the integrator from seeing a large up-step
        // when the target jumps. Falls are instantaneous. Operates on the (slew-limited)
        // ChargingVoltageTarget and feeds the CV PI error below.
        // voltageTargetSlewed is a global (Xregulator.ino) purely so cvLog_tick can record it; it was a
        // static local here and retains the same write-once-per-tick, persist-across-calls behaviour.
        if (enteringCV) {
          voltageTargetSlewed = ChargingVoltageTarget;
        }
        if (voltageControlActive) {
          // cvRiseGovEnable=0 (Voltage tab Test Limiters) disarms the rise clamp → the integrator sees the
          // full up-step and can wind up into an OV trip; falls were already instant. For A/B study.
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

          // ── CV entry bumpless seed ─────────────────────────────────────────────────
          // Seeds cv_I at voltageControlActive entry so that Icv = Kp*e + cv_I ≈ g_pidI_filtered,
          // preventing a step change in setpointLimited. Resets cv_I_aw_cap so a stale
          // post-event cap does not constrain the CV loop on entry.
          //   seed = g_pidI_filtered - Kp*e  →  Icv = g_pidI_filtered  →  no step in setpointLimited.
          if (voltageControlActive && enteringCV) {
            float e_cv = ChargingVoltageTarget - IBV;  // raw INA228 — bumpless seed uses true voltage
            // Entering ABOVE target the bumpless formula banks +Kp*|e| of surplus into cv_I, which the
            // integrator hands straight back the tick the error closes (manual->auto handback from a bus
            // parked 0.47V over target: shed, then overshoot into G2). Cap the seed at the delivered
            // current there, so entry can only ever shed. Below target the formula is unchanged.
            float seedHi = (e_cv < 0.0f) ? fminf((float)uTargetAmps, g_pidI_filtered) : (float)uTargetAmps;
            float seed = clamp_f(g_pidI_filtered - VoltageKp_active * e_cv, 0.0f, seedHi);
            cv_I = seed;
            cv_I_track = seed;
            cv_I_aw_cap = (float)MaxTableValue;    // clear AW cap — stale values constrain CV entry
            awSeedProtectStartMs = currentMillis;  // start seed-protection window
            recovActive = false;                   // stale refill must not survive a CV re-entry
            cvStallStartMs = 0;                    // stall dwell likewise — a V0 frozen across CV exit would skip the 3s proof
            cvStallBoost = false;
          }
          // ── Unified protection-release reseed + unified telemetry export ───────────
          // Single falling-edge handler for ALL three protection paths (Group 1/2 OV,
          // iExcess, LoadDump). Fires when fastOvClampActive goes 1 → 0 — i.e., every
          // protection has cleared. Uses preEventCvI captured back when no protection
          // was clamping, so chained/overlapping events don't burn intermediate
          // snapshots. Engages AwSeedProtectMs so the per-tick cv_I bleed below
          // cannot wipe the reseed if a new event re-fires within the protect window.
          //
          // g_fastOvClampActive (read here, written at end of this block) is the
          // unified flag — every supervisor has voted by the time we reach this point.
          if (voltageControlActive && g_fastOvClampActive && !fastOvClampActive) {
            g_ovClampRiseMs = 0;  // episode ended — disarm the tau timer until the next rising edge stamps it
            float icvHi_seed = clamp_f(icvCeil, 0.0f, (float)MaxTableValue);  // alternator command ceiling in CV
            // Seed fraction is shunt-gated: with a shunt demandDropA already corrected the base
            // (restore ReseedFrac); without one the drop is unmeasurable — seed low
            // (ReseedFracNoShunt) and let the bus-watched climb floor rediscover demand.
            // A re-fire <4s proves the SEED itself was above demand → rebase on the previous
            // seed ×0.7, min-ed with preEventCvI (the refill regrows preEventCvI mid-train, which
            // is how the old ×0.85^n ratchet took 3 fires to shed a 24A dump — 22:17 07-22).
            // intakeFloor: a load departure never justifies seeding below the battery's own share.
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
              // Consume the correction: snap the slow loads EMA to present loads so a rapid re-fire
              // measures only NEW departures. Without this the same drop subtracts twice — the
              // 16:25 07-22 double-dump re-subtracted 42A from the already-corrected 40A base,
              // reseeding a perfect seed to zero and turning a 3s recovery into 28s.
              preEventLoadEma = MeasuredAmps - Bcur;
              preEventLoadMs = currentMillis;
            }
            cv_I_track = cv_I;
            lastReleaseMs = currentMillis;
            awSeedProtectStartMs = currentMillis;     // engage seed-protection window
            postProtectRiseStartMs = currentMillis;   // open fast-rise window (closed by either time cap or voltage gate at slew site)
            // Field-side floor reseed: the clamp ran with the tach floor dropped (governor site),
            // so flux decayed toward MinDuty. Onset duty makes ~0 A by definition, so restoring it
            // in one step cannot re-lift the bus — and re-delivery isn't gated on a MinDuty->floor
            // duty slew.
            float floorSeed = fmaxf(tick.rpmMinDuty, MinDuty);
            if (lastAppliedDuty < floorSeed) {
              currentPID.ResetIntegratorTo((double)floorSeed);
              pidOutput = (double)floorSeed;
              lastAppliedDuty = floorSeed;
            }
            if (cvRecovEnable) {
              recovActive = true;
              recovWalking = false;
              // EMA is the true hold — but only when re-validated by a clean settled window SINCE
              // this episode's first fire; otherwise it can remember a departed load and the refill
              // re-inflates every lap. Stale (or never-settled) → preEventCvI, which self-converges
              // with the reseed ratchet.
              bool holdFresh = (cvSteadyHoldEma > 0.5f)
                               && cvSteadyHoldMs != 0
                               && (int32_t)(cvSteadyHoldMs - g_ovEpisodeStartMs) > 0;
              float holdBasis = holdFresh ? cvSteadyHoldEma : preEventCvI;
              // Goal drops by the measured demand too — else the refill drives cv_I right back to the
              // departed-load current and re-fires. Floored at the reseed so the deficit stays ≥0.
              // A rapid re-fire proved every memory-derived basis above demand: goal = seed, no
              // refill past it (the starve-walk lifts the ceiling if the bus then stalls low).
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
          // Unified-flag rising-edge counter — counts every distinct activation of
          // ANY protection (G1/2 OV, iExcess, LoadDump). Must be incremented BEFORE
          // g_fastOvClampActive is updated for next tick.
          if (fastOvClampActive && !g_fastOvClampActive) {
            g_fastOvClampCount++;
            g_ovClampRiseMs = currentMillis;  // stamp the episode start for the field-decay-tau early release
            // OV-episode stamp — here, not the G1/G2 hard block, so ALL protections open it
            // (iExcess CV/bulk and LoadDump included). An iExcess-shaped train otherwise runs
            // with P-boost ×4 and a stale hold-EMA goal: 6 fires on a 12A headlight-off
            // release, pTerm 33A vs a 20.5A true hold (16:49 07-24).
            if (!ovEpisodeActive()) {
              g_ovEpisodeStartMs = currentMillis;
              queueConsoleMessage("OV episode: recovery P-boost paused until 30s after the last fire");
            }
            g_ovEpisodeLastFireMs = currentMillis;
            // Latch pre-cut RPM for the drain-vs-RPM lookup — unless this episode starts inside the
            // prior cut's tach-corruption window (LM2907 false-zero/garbage ramp ~4.6 s): a garbage
            // low read would look up a too-short drain and release with the field still hot. -1 = worst case.
            bool tachSuspect = (g_lastProtClampMs != 0)
                               && ((uint32_t)(currentMillis - g_lastProtClampMs) < PROT_RPM_GRACE_MS + 2000UL);
            g_ovClampRpm = tachSuspect ? -1.0f : RPM;
            // Demand-drop capture — at the rising edge the field hasn't moved yet, so a jump in
            // (loads EMA − loads now) is load that left the bus, not the cut's own doing. Deadband
            // 5A: Bcur+MeasuredAmps tick noise; below that a train can't sustain anyway. A delivery
            // overshoot (alternator surge into the battery) raises Bcur AND MeasuredAmps equally,
            // so it cancels out of the loads difference and never shaves a benign reseed.
            demandDropA = 0.0f;
            if (reseedCorrEnable && HAS_BATT_SHUNT && preEventLoadMs != 0) {
              demandDropA = preEventLoadEma - (MeasuredAmps - Bcur);
              if (demandDropA < 5.0f) demandDropA = 0.0f;
              // The hold EMA's premise (this current holds target) died with the departed load —
              // correct it now, or the release goal re-aims the refill at the pre-drop hold.
              if (demandDropA > 0.0f) cvSteadyHoldEma = fmaxf(cvSteadyHoldEma - demandDropA, 0.0f);
            }
            // 4s window (was 1.5s): the echo cycle — cut → valley → climb → shed race — measured
            // 1.55-1.7s release-to-fire on the 16:49 07-24 train, so 1.5s reset the ratchet every
            // lap and the train ran to 6. Independent events are minutes apart, never seconds.
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
          // Latch which protection bound the cap this tick into the Plots-tab marker
          // bitmask (consumed + cleared by the CSV1 sender). capReasonTick is a faithful
          // proxy for fastOvClampActive — every clamp site sets both.
          switch (capReasonTick) {
            case CAP_REASON_KHARD_G1:
            case CAP_REASON_KHARD_G2:     g_protEventLatch |= PROT_EVT_OV; break;
            case CAP_REASON_IEXCESS:
            case CAP_REASON_IEXCESS_BULK: g_protEventLatch |= PROT_EVT_IX; break;
            case CAP_REASON_LOADDUMP:     g_protEventLatch |= PROT_EVT_LD; break;
            default: break;
          }
          bool seedProtected = (AwSeedProtectMs > 0) && ((currentMillis - awSeedProtectStartMs) < (uint32_t)AwSeedProtectMs);

          // Anti-windup ceiling: bleeds down while fastOV is active so the bumpless
          // tracker cannot immediately re-wind cv_I to the bulk-charging level after
          // each overshoot. Recovers gradually after fastOV clears, giving the battery
          // time to settle before full current ramps back up.
          // cv_I_aw_cap is declared above the iExcess block so both blocks share it.
          // AwBleedRate is a user-adjustable global (NVS-persisted, settings namespace). AwRecoverRate is hardcoded (0.1f).
          // Scale bleed/recover rates by MaxTableValue so a fixed fraction-per-second
          // applies the same proportional aggression regardless of alternator size.
          // AwBleedRate=2.0 at 50A table → 100 A/s; at 150A table → 300 A/s.
          // AwRecoverRate=0.1 at 50A table → 5 A/s; at 150A table → 15 A/s.
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
            // Per-tick cv_I bleed during active fastOV: voltage PI fires at 100ms but
            // the integrator needs to reduce every 5ms tick to counteract bulk-phase wind-up.
            // Suppressed during seed-protection window to preserve the seeded cv_I value.
            if (fastOvClampActive && !seedProtected) {
              cv_I = fmaxf(0.0f, cv_I - awBleedAmpS * actualDtSec);
              cv_I_track = cv_I;
              g_awState = 3;  // actively bleeding cv_I due to fastOV protection
            }
          }
        }

        if (voltageControlActive) {
          bool cvLoopFired = enteringCV || ((currentMillis - lastVoltageLoopMs) >= VoltageLoopInterval);

          // ===== CV D term — sliding-window derivative, recomputed every output tick =====
          // kdTrim is a POSITION (VoltageKd_active × slope EXCESS over the deadband line — or × full slope,
          // latch-gated, in legacy CvKdExcessMode=0 — capped), subtracted at the Icv output, never integrated
          // into cv_I. Runs BEFORE the PI block so its anti-windup reads this tick's fresh slope/trim.
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
              // Newest sample at least one window old → backward difference over that window. Until the
              // buffer spans a full window (first ~100 ms after CV entry) cvDSlope holds 0 — same "no slope
              // until one interval has elapsed" behaviour as the old 100 ms-cadence diff, minus the noise
              // of a sub-window difference right after entry.
              uint32_t effWindowMs = (uint32_t)constrain((int)VoltageLoopInterval, 20, 200);  // capped to what the ring holds
              float vOld = 0.0f; uint32_t oldAge = 0; bool spanned = false;
              float vOldest = 0.0f; uint32_t oldestAge = 0;
              for (uint8_t i = 0; i < kdBufN; i++) {
                uint8_t idx = (uint8_t)((kdBufHead + KDBUF_N - 1 - i) % KDBUF_N);   // newest → oldest
                uint32_t age = currentMillis - kdBufT[idx];
                vOldest = kdBufV[idx]; oldestAge = age;                             // last write wins = oldest walked
                if (age >= effWindowMs) { vOld = kdBufV[idx]; oldAge = age; spanned = true; break; }
              }
              // Fast ticks can fill the ring before it spans a full window; once it IS full, fall back to
              // the widest available window rather than stalling the slope. Buffer-not-yet-full (just after
              // CV entry) still holds cvDSlope at 0 until a real window exists.
              if (!spanned && kdBufN >= KDBUF_N && oldestAge > 0) { vOld = vOldest; oldAge = oldestAge; spanned = true; }
              if (spanned && oldAge > 0) {
                float slopeCeil = CvKdSlopeCeil;  // V/s real per-bus (WYSIWYG) — no ×V/12 at use
                cvDSlope = constrain((g_cvKdFiltV - vOld) / ((float)oldAge / 1000.0f), -slopeCeil, slopeCeil);
                rollUpdate(ROLL_CVSLOPE, cvDSlope);   // D-term deadband-tuning readout (10 s peak raw rise)
              }
              // The deadband is a LINE in operating current (ripple-driven slope noise grows with
              // output, mirroring the G3 trip line), evaluated at the slew-limited command — slow by
              // construction, so a genuine transient can't drag the threshold up while the D should fire.
              // In excess mode the line is a SUBTRACTION (trim continuous from zero at it) and needs no
              // hysteresis; the latches below serve only the legacy full-slope mode, where the engagement
              // step needs a re-arm-at-half gate so ripple flickering across the edge can't jab the field.
              // Latches track the slope every tick — outside the arm gate too — so a legacy re-arm never
              // acts on stale state.
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
                  // Trim ∝ excess over the line, continuous from zero at it. The legacy full-slope form
                  // jumped by Kd×deadband at engagement — a relay step that, behind the ~0.2 s field lag
                  // and the stiff near-saturation local plant, self-sustained a 1.4 Hz hold limit cycle
                  // (2026-07-21 12:48 log). Excess form gives a small swing near the line near-zero
                  // authority, so the cycle cannot feed itself; deep-slope response is reduced by only
                  // the band width (Td is the recovery knob if that haircut ever matters).
                  if (cvDSlope > kdDb) {
                    slopeExcess = cvDSlope - kdDb;                       // brake
                  } else if (!CvKdOneSided && cvDSlope < -kdDb
                             && (voltageTargetSlewed - g_cvKdFiltV) > 0.0f) {
                    // two-sided add: only when BELOW target — damps the undershoot ring without
                    // re-lifting an above-target overshoot (the old symmetric-D failure was adding here)
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
                g_ovTel.kdEventCount++;
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
            voltLoop_record(currentMillis);  // feed the CV-interval ladder (vl_*); vlHasPrev re-baselines on CV exit
            pidLog_voltageLoopRanThisTick = 1;
            pidLog_enteringCV = enteringCV ? 1 : 0;

            float e = voltageTargetSlewed - IBV;  // raw INA228 — no filter lag on PI error (governor output)
            // Steady CV holding-current estimate (recovCvGoal basis). Updates ONLY in clean CV near
            // target: overshoot droop (|e| large, V over target) and flutter-gap re-samples (recovActive)
            // are both excluded, so it tracks the true hold (~39A) not the bled-down snapshot (~21A).
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
            // Refill ends when the deficit is healed (void on a collapsed goal — recovGoalCollapsed)
            // OR the bus has held ≈target ~1s (or the existing cancels: new fire, CV exit/re-entry).
            // The goal ceiling on icvHi lifts with it. Premise-void backstop:
            // healing needs cv_I to rise, but satHi freezes cv_I the whole time Icv rails at the
            // goal ceiling — so if the pre-trip current no longer holds the target (load/plant
            // changed since the trip) the refill would latch and cap CV at the stale pre-trip
            // current forever. Signature: full goal commanded AND DELIVERED, bus still low and not
            // rising. The delivered half is load-bearing (07-23 blip log): when the plant can't
            // deliver the command (idle-knee sag while refilling a deep reseed hole), raising the
            // goal moves nothing on the bus — it only stores surplus that dumps when delivery
            // returns (+8.6A over true hold → G2 re-fire). Walking is only meaningful while the
            // goal is the binding constraint, i.e. delivered ≈ commanded.
            // The backstop WALKS the goal up rather than dropping the ceiling: a one-tick release
            // steps the command by the whole P+I surplus and re-fires on a stiff-topped plant.
            if (recovActive) {
              float aRef = (float)g_voltLoopActualIntervalMs / (3000.0f + (float)g_voltLoopActualIntervalMs);
              recovVRefEma += aRef * (getFiltV() - recovVRefEma);
              float clsRec = (float)SYSTEM_VOLTAGE_CLASS / 12.0f;
              float shortfallF = voltageTargetSlewed - getFiltV();
              // "Not rising" = bus still within the answer band of the ~3s reference — a DELTA, not a
              // per-tick slope sign: idle ripple (±0.09 V/s on a flat bus) flips a slope EMA and
              // resets the 5-tick resume gate, starving the walk to ~7% duty (0.11 A/s vs 1.5
              // design) — a ~43A no-shunt load then holds the bus 1.7V low for ~9 min (21:36 07-24).
              // A genuine answer still pauses within a tick: field-lag-rate rises clear the
              // threshold in ~30 ms. The band WIDENS below cvRecovDeepBandV of shortfall: a fixed
              // 0.05V×class band read a 23 mV/s battery-driven creep 1.8V below target as
              // "answering" and froze the ceiling at 47.9A for 10s (12:45 07-25 stuck) — at that
              // depth only a hard takeoff is an answer; near target the original band is unchanged.
              float busAnswerV = (0.05f + 0.08f * fmaxf(0.0f, shortfallF / clsRec - cvRecovDeepBandV)) * clsRec;
              // Shallow closure-horizon clause: over the band alone is not an answer unless the
              // rise would also close the remaining shortfall within ~3 reference windows (~10s).
              // A 20 mV/s ceiling-limited creep parks the 3s delta just over the 0.05V band and
              // starved the walk to ~0.07 A/s for 27s of a 33s recovery (13:15 07-28 no-shunt
              // load removal). Dimensionless (delta vs shortfall, both real volts) so it is
              // class/chemistry/size-blind. Deep band exempt — bit-identical to the widened-band
              // behavior above: at the 5x walk rate a saturated delta must still pause a takeoff
              // on the plain band.
              float busDeltaV = getFiltV() - recovVRefEma;
              bool busAnswering = (busDeltaV >= busAnswerV)
                                  && (shortfallF >= cvRecovDeepBandV * clsRec || busDeltaV * 3.0f >= shortfallF);
              bool delivering = (setpointLimited - g_pidI_filtered) < fmaxf(5.0f, 0.15f * setpointLimited);
              // Rail test against the FLARED ceiling (the one Icv was actually clamped to last
              // tick), 1A slack: the flare moves with ripple (~26A/V of goal-60 slope), so an
              // exact-rail compare would flicker the 5-tick resume gate inside the flare band.
              bool starved = delivering && (Icv >= cvRecovFlaredCeil(recovCvGoal, shortfallF) - 1.0f)
                             && (e > 0.0f) && !busAnswering
                             && ((uint32_t)(currentMillis - recovStartMs) > 2000UL);
              recovStarveTicks = starved ? (uint8_t)(recovStarveTicks + 1) : 0;
              // Recovered exit — bus holding within normal steady-hold jitter of target for ~1s means
              // the climb is over, whatever the amp deficit says. Without it the window latches as a
              // stale ceiling whenever the plant heals needing less current than the goal (battery
              // filled during the event, load gone, target dropped) and ambushes the next disturbance.
              bool heldAtTarget = (e <= 0.025f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f))
                                  && ((uint32_t)(currentMillis - recovStartMs) > 2000UL);
              recovHeldTicks = heldAtTarget ? (uint8_t)(recovHeldTicks + 1) : 0;
              // A collapsed goal (goal = seed) starts already "healed" — the deficit exit would
              // drop the ceiling one tick after release. Hold the window; the walk lifts the
              // ceiling from bus truth and held-at-target / walk-to-ceiling / a new fire end it.
              if ((cv_I >= recovCvGoal - 0.5f && !recovGoalCollapsed) || recovHeldTicks >= 10) {
                // Bumpless handoff: the flared ceiling is what was holding the bus; cv_I above it
                // is refill banked before the flare tightened (freeze stops growth, never drains)
                // and would step the setpoint at release (08-05: +5A bank → 14.41V on a 14.00 target).
                cv_I = fminf(cv_I, cvRecovFlaredCeil(recovCvGoal, shortfallF));
                recovActive = false;
                recovWalking = false;
              } else if (!recovWalking && recovStarveTicks >= 5) {
                recovWalking = true;
                queueConsoleMessage("CV recovery: pre-trip current no longer holds the target — walking the ceiling up");
              }
              // The walk PAUSES the tick the signature breaks (bus answers, delivery lost, error
              // closes) and needs 5 fresh ticks to resume — it is NOT latched. 19:22 07-23: the
              // latched version kept ramping ~0.9s after the bus had begun rising, storing +10A
              // over true hold that shed authority could not drain → G2 re-fire, ×3.
              if (recovActive && recovWalking && recovStarveTicks >= 5) {
                float ceilNow = clamp_f(icvCeil, 0.0f, (float)MaxTableValue);
                // 0.5A/s + 1%-of-ceiling/s: scale-invariant, ~1A/s on a 50A machine. Deliberately
                // slower than the battery's own surface-charge refill, because "ceiling too low"
                // and "valley not yet backfilled" are the SAME observable — at this rate the bus
                // resolves which one it is before the walk can add meaningful surplus. Held-at-
                // target exit stops the walk at the true hold; clearing the alternator ceiling
                // means there is nothing left to release. That ambiguity only exists NEAR target:
                // beyond cvRecovDeepBandV of shortfall no surface-charge valley explains the
                // level — the ceiling is simply below demand — so the rate ramps to
                // ×cvRecovDeepMult at 2× the band (12:45 07-25: 0.74A/s avg against a 30A
                // collapsed-goal deficit held the bus 2V low for 45s). Continuous at the band
                // edge; the widened answer band above keeps a genuine takeoff pausing it.
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
            // Load-serve boost engagement. Trigger on the measured demand itself: amps that hold
            // target = present house loads + the battery's charging share at target (battIntakeEma).
            // When that exceeds cv_I with the bus meaningfully low, up-integration runs at the
            // refill's boosted rate toward the snapshot goal and exits when the deficit heals or the
            // bus reaches target. The old battery-discharge trigger (Bcur < −5A × 5 ticks, release
            // Bcur > −2A) never engaged in practice: the P-boost serves the initial hole in ~0.3s,
            // after which the battery parks near 0A while the bus hangs 0.2V low for 30s (16:24
            // 07-22 log — longest discharge streak 3 ticks). Sensor lies stay bounded by
            // construction: the boost multiplies integration of the VOLTAGE error, so at target it
            // pushes nothing regardless of what the goal claims. Defers to the post-protection
            // refill (its goal ceiling wins); worst case (load removed mid-boost) is one G2 fire
            // caught by the demand-drop reseed. inaFastModeActive = Bcur live at control rate (same
            // gate as Load Dump). 10A deficit + 0.05V/class error + 2 ticks: beyond the correlated
            // iMeas/Bcur ripple in the loads difference, and 200ms is field-lag scale anyway.
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
            // Recovery ceiling — never command more than the pre-trip current, flared down over the
            // last cvRecovFlareBandV of approach. Folded into icvHi (not a late fminf on Icv) so
            // satHi anti-windup stops cv_I integrating against it.
            if (recovActive) icvHi = fminf(icvHi, cvRecovFlaredCeil(recovCvGoal, e));
            float icvLo = 0.0f;

            // cvDSlope + kdTrim (g_cvKdTrimLive) are computed once per output tick above (sliding window);
            // this block only consumes them.

            if (!enteringCV) {
              float p = VoltageKp_active * cvRecovBoostMult(e) * e;  // recovery P-boost scales the proportional term while below target
              float unsat = p + cv_I;
              Icv = clamp_f(unsat, icvLo, icvHi);

              bool satHi = (Icv >= icvHi);
              bool satLo = (Icv <= icvLo);

              // The D term (g_cvKdTrimLive) was computed once this output tick above; the PI block only
              // reads whether it is limiting so the anti-windup can freeze up-integration while it holds
              // the output down (else cv_I winds up invisibly and dumps on release).
              bool kdLimiting = (g_cvKdTrimLive > 0.0f);  // D term is actively removing current

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
              // Stall accelerator (07-24) — the SAME climb floor, armed outside recovActive: a load
              // apply or commanded-target drop strands cv_I tens of amps short with NOTHING limiting
              // it, on bare Ki*e (Load Pickup Boost is HAS_BATT_SHUNT-gated and cannot cover it).
              // Arming needs PROOF: sustained offset AND < 0.05V*class of bus movement across the
              // dwell — a DELTA, because cvDSlope reads +/-0.35V/s on ripple alone. The duty veto is
              // the whole safety case: accelerating into a pegged field banks amps that dump when
              // rpm returns. Every threshold is voltage-class scaled or a fraction of a LIVE setting
              // (MaxDuty, icvCeil) — none is fitted to one boat; the 3s dwell is a debounce, benign
              // either way (too short only arms on a genuinely crawling bus, still arrival-gated).
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
              // Bus-watched climb floor: the error-paced refill trickles a low seed's last amps in
              // at sub-A/s (07-21 below-target tails; the 16:25 07-22 zeroed seed took 28s). Gated
              // on projected arrival — the A3 brake's staleness+actuator horizon — so the handback
              // stops before the bus lands. The freeze/satHi gates below still veto the floor.
              // Outside recovActive the floor is DEPTH-SCHEDULED (full rate at cvRecovDeepBandV of
              // shortfall, →0 at the 0.10V stall-arm threshold): the stall path has no flare
              // ceiling, so the flat floor rode the 0.354s arrival horizon into a ~1V/s target
              // crossing that coasted +0.56V into the G2 measured margin (13:14 07-28 load apply).
              // recovActive keeps the flat floor — cvRecovFlaredCeil bounds its momentum.
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
              // Freeze upward integration while the fast-OV supervisor caps the ceiling, the D term
              // holds the output down, the setpoint slew is still delivering a lower command, or the
              // commanded-target wind-down owns the ceiling: in all four cases Icv is held below what
              // the PI wants while V is still under target (e>0), so unfrozen cv_I would wind up
              // invisibly and dump on release (58->79A wind during the 13A/s crawl, 2026-07-20 double
              // fire). Wind-down positive error is real (the gliding protection reference sits above
              // the bus mid-descent) but undeliverable by design.
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

        // Per-tick Icv recompute — proportional path responds every output current loop tick; cv_I still
        // updates only on VoltageLoopInterval cadence. The D-term back-off (g_cvKdTrimLive) is recomputed
        // every tick in the D block above and subtracted here too, so the fast P path never overwrites the
        // trim (an earlier build subtracted it only in the 100ms path, and this recompute wiped it ~5ms
        // later — the D term then had authority for ~5% of ticks and was effectively negated). It is a
        // position, not an integral: zeroed on CV entry, recomputed from the present slope each tick.
        {
          float e_now = voltageTargetSlewed - IBV;  // raw INA228 — no filter lag on per-tick proportional (governor output)
          float icvHi_tick = clamp_f(icvCeil, 0.0f, (float)MaxTableValue);  // alternator command ceiling in CV
          if (recovActive) icvHi_tick = fminf(icvHi_tick, cvRecovFlaredCeil(recovCvGoal, e_now));    // same flared pre-trip-current ceiling as the PI path
          if (!enteringCV) {
            float boost_now = cvRecovBoostMult(e_now);  // recovery P-boost; 1× at/above target
            g_cvPTerm = VoltageKp_active * boost_now * e_now;  // P contribution to Icv, live (CSV1 P/I/D plot) — reflects the boost
            Icv = clamp_f(VoltageKp_active * boost_now * e_now + cv_I - g_cvKdTrimLive, 0.0f, icvHi_tick);
          }
        }

        // ===== CV COMMANDED-TARGET WIND-DOWN GOVERNOR =====
        // A commanded target DROP cannot be tracked by the PI inside the OvMeasMarginV band: the shed
        // rate is KiDown×error, so the bus can fall at most ~3.5·cvAlpha·margin V/s (~16 mV/s with the
        // auto gains — plant-independent, since Kp = α/K). The 2026-07-21 FAIL log: a 13.9→13.3 V step
        // through the 0.15 V/s glide alone left the bus +110 mV over the gliding target and fired G2
        // three times, parking the bus 140 mV UNDER the new target. Fix: walk the current command down
        // at a deliberate rate and stop on the MEASURED bus — cap Icv (and cv_I with it) along a ramp
        // from the pre-step command, hold once the filtered bus reaches the commanded target + stop
        // margin, and hand back to the PI with a bumpless seed after the protection-reference glide
        // (vTgtRampDn) has also arrived (releasing earlier would let the PI chase the still-elevated
        // slewed target back up). Protections are untouched: the gliding ChargingVoltageTarget stays
        // their reference and G2 backstops a stalled wind-down exactly as before. Sits after the
        // per-tick Icv recompute so the cap is the last writer before setpointCommand reads Icv.
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
        // ISE/T scoring — same shape as current waveform test (errorAccum / activeTimeSec).
        // HIGH phase: asymmetric squared error — overshoot (above target) weighted by cvKOvershoot
        // ("Overshoot Penalty (K)" on the Voltage tab) vs undershoot weighted ×1.
        // LOW phase: one-sided squared error, normal weight (battery decay is physics-limited).
        // HIGH target = cvBaseTarget + cvWaveAmplitudeV (the step-up we're testing).
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
          if (!isnan(tempSample)) { cvTuningScore.tempSum += tempSample; cvTuningScore.tempSampleCount++; }
          cvTuningScore.avgSampleCount++;
        }

        // ===== CV TUNING SCORE ACCUMULATION — LOW phase (return to normal setpoint after step-up) =====
        if (CVTuningMode && voltageControlActive && !cvTuningScore.waveHigh && cvTuningScore.ringInDone) {
          float lowTarget = cvBaseTarget;  // LOW phase is the normal setpoint; track voltage return from high step
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
        float effectiveFallRate = fastOvClampActive ? 1.0e9f : (cvBrakingDown ? fmaxf(CvBrakeFallRate, SetpointFallRate) : SetpointFallRate);  // fmaxf: the submit-time floor can be stale (SetpointFallRate raised later, or config import) — braking must never fall slower than normal
        // Post-protection fast-rise: while the window is open AND battV still has
        // clear headroom below the active charge target, multiply the normal rise
        // slew so the alternator crosses its deadband and starts producing again
        // quickly. Window opens on the unified fastOvClampActive falling edge above;
        // closes when EITHER the time cap expires OR battV climbs into the headroom.
        bool postProtectRiseActive =
            voltageControlActive &&
            (postProtectRiseStartMs != 0) &&
            ((currentMillis - postProtectRiseStartMs) < FastSetpointRiseWindowMs) &&
            (IBV < ChargingVoltageTarget - FastSetpointRiseHeadroomV);
        float effectiveRiseRate;
        if (inStartupRamp)              effectiveRiseRate = StartupRiseRate;
        else if (postProtectRiseActive) effectiveRiseRate = SetpointRiseRate * FastSetpointRiseRate;
        // Large up-step gentling: when the commanded current jumps well above the current
        // slew-limited setpoint, ramp at the slower SetpointBigStepRiseRate until the remaining
        // gap closes to within SetpointBigStepThresh — then fall back to the normal fast rate for
        // the final approach (preserves snappy small-error corrections). Down-moves untouched.
        // Sits below the post-protection fast-rise so it never slows recovery out of a trip.
        else if ((setpointCommand - setpointLimited) > SetpointBigStepThresh)
                                        effectiveRiseRate = SetpointBigStepRiseRate;
        else                            effectiveRiseRate = SetpointRiseRate;
        if (!setpointSlewEnable && !g_autoTestActive) {
          // Limiter disabled (Current tab Test Limiters): setpoint steps instantly — also bypasses the
          // startup ramp, big-step gentling and post-protection fast-rise shaping above. For A/B study.
          // Inert during an automated test (g_autoTestActive) so user state can't strip slew from a measurement.
          setpointLimited = setpointCommand;
        } else {
          setpointLimited = slew_limit_f(setpointLimited, setpointCommand,
                                         effectiveRiseRate, effectiveFallRate, actualDtSec);
        }
        if (inStartupRamp && setpointLimited >= setpointCommand - 0.5f) {
          inStartupRamp = false;
        }

        // Output current PID compute.
        // MaintainMode regulates to 0 net battery amps. Feedback is always INA228 (Bcur),
        // never getBatteryCurrent() — picking Victron as Battery Current Source would add
        // ~1–2 s of lag that destabilizes this loop. The dropdown only governs SoC display.
        {
          float pidSig = (OutputPIDSigSrc == 2) ? MeasuredAmps : (OutputPIDSigSrc == 1) ? g_pidMA_N
                                                                                        : g_pidI_filtered;
          if (MaintainMode == 1 || zeroFloatActive)   targetCurrent = Bcur;   // 0-net-amps, raw INA228
          else                                        targetCurrent = pidSig; // alternator-current signal selected by OutputPIDSigSrc
        }
        pidInput = (double)targetCurrent;
        pidSetpoint = (double)setpointLimited;
        pidError = setpointLimited - targetCurrent;

        currentPID.Compute();

        // Banner limiter code (→ CSV4/NavStream): always the min-select winner — transient
        // under-tracking / mid-slew still show the constraint the loop is converging to, so the
        // cue doesn't blink off on ripple dips or setpoint moves. 0 only when nobody limits
        // charging (zero-current cmd, CV square-wave tuning, startup ramp). 5 = machine limit,
        // read from the ACTUATOR (applied duty riding MaxDuty, sustained, output still short) —
        // output error alone can't distinguish "won't get there" from "not there yet".
        // 6 = protection: the fastOvCurrentCap fold undercut the pre-OV ceiling (battCeilBinding
        // latches BEFORE that fold, so without this check the whole event misattributes to "Batt"),
        // extended through the post-protection fast-rise window so the entire recovery climb reads
        // as protection, not the ceiling the command happens to rail against.
        {
          bool zeroCmd = (MaintainMode == 1 || zeroFloatActive);
          bool underTracking = ((float)pidInput < setpointLimited - fmaxf(3.0f, 0.10f * setpointLimited));
          // Min-field floor binding: duty parked on the keep-alive floor while the loop is still
          // OVER-delivering. The only limiter that raises output instead of lowering it, so it needs
          // its own test rather than a min-select place — the PID cannot report it either, since its
          // own outMin IS MinDuty (applyCcOutputLimits), so pidOutput rails at the floor rather than
          // asking for less. tick.rpmMinDuty <= 0 is the sentinel that disables the whole floor in
          // governor_apply (above the commissioned RPM ceiling), so gate on it here too.
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
          // 11 (min-field floor) sits below every real ceiling: it is the one code that RAISES output, so
          // whenever a ceiling is also active the ceiling is the story — a thermal/BMS/battery derate that
          // drives the command under the floor's output was reporting MIN FIELD, which also made
          // rvcDeratingFromLimiter publish "not derating" to the RV-C bus during a real derate.
          // 12 (zero command) and 14 (Hi->Lo glide) sit BELOW zeroCmd's own test in this order for a
          // reason: warmupBinding and glideBinding are both computed ABOVE the Maintain/zero-float
          // assignment of uTargetAmps = 0, so either can still read true while the real story is the
          // zero command. Letting 12 answer first keeps that from surfacing as a stale ramp.
          if (CVTuningMode || inStartupRamp)                       rawCode = 0;
          else if (protBinding)                                   rawCode = 6;
          else if (zeroCmd)                                       rawCode = 12;
          else if (glideBinding)                                  rawCode = 14;
          else if (altZeroOutput)                                 rawCode = 7;
          else if (fieldSaturated)                                rawCode = 5;
          else if (voltageControlActive && Icv < icvCeil - 0.5f)  rawCode = dvccCvlBinding ? 9 : 3;  // held at the BMS's voltage vs our own target
          else if (dvccCclBinding)                                rawCode = 8;  // BMS charge-current limit is the active ceiling
          else if (battCeilBinding)                               rawCode = 4;
          else if (thermalPenaltyAmps > 0.5f)                     rawCode = 2;
          else if (minFloorBinding)                               rawCode = 11;  // no ceiling binds: over-delivery really is the keep-alive floor
          else if (warmupBinding)                                 rawCode = 13;  // last real ceiling before the catch-all
          else                                                    rawCode = 1;
          // Publish a change only after 5 consecutive identical ticks (~150 ms): edge-hover
          // between two codes (Icv at icvCeil, thermal penalty at 0.5 A) parks the published
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
      g_thermalOwnsCeiling = false;      // ceiling chain does not run here; must not read stale-true on AUTO re-entry
      uTargetAmps = 0;
      setpointLimited = 0.0f;
      ctrlLimiter = 10;                  // the user's own duty IS the limit here — published so the banner
                                         // and limit panel can name manual instead of "nothing limiting"
      pidInput = (double)((OutputPIDSigSrc == 2) ? MeasuredAmps : (OutputPIDSigSrc == 1) ? g_pidMA_N
                                                                                         : g_pidI_filtered);
    }  // end of MANUAL mode else
  }  // end if (!sysIDRunning)


  uint32_t aflM4 = micros();  // end of section 4: normal-mode control body (PID/CV/sysID)
  // ========== BUILD DUTY REQUEST ==========
  float dutyRequest;
  if (sysMode == SYS_MODE_MANUAL) {
    dutyRequest = constrain(ManualDutyTarget, 0.0f, 100.0f);
  } else {
    dutyRequest = (float)pidOutput;
  }

  pidLog_dutyRequest = dutyRequest;

  // ========== APPLY THROUGH GOVERNOR ==========
  // An active protection clamp drops the tach floor: with duty pinned at the floor, rotor flux
  // decays to the floor asymptote instead of ~0, and an RPM surge on that residual flux carried
  // the bus +350 mV AFTER the command cut (13.4 V event, 2026-07-12). Duty falls to MinDuty
  // (PID outMin) for the life of the clamp; the release reseed in the bumpless block restores
  // the measured floor in one step.
  // cvpfCcActive: the plant-fit's settle/pilot/practice phases drive this loop to currents whose
  // duty can sit BELOW the learned Min% floor's output — a stale floor otherwise pins all three
  // phases at one current, both captured duties come out identical, and every pulse edge drops
  // ("no clean current step", bench 2026-08-04). The pulse phases already bypass via sysIDRunning.
  // SYS_MODE_MANUAL: manual is open loop by definition — the commanded duty is delivered as typed,
  // including 0. Passing 0 here drops the scalar Min Field % too (governor_apply's shutdown branch),
  // which is the point: the tachometer keep-alive floor is the last thing manual still obeyed. The
  // Max Field ceiling is deliberately NOT bypassed — it is what bounds field current on 24/36/48V.
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
    bool kneeModeOk = (sysMode == SYS_MODE_AUTO) && !sysIDRunning && !cvPlantFitActive && tick.chargingEnabled
                      && !tick.inLockout && !IgnoreTemperature && (hardwarePresent == 1)
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
    // Spec: Working Markdown Docs/CONTROL_ACCURACY_V4_ROUTINE_SPEC.md. Each valid tick lands in
    // ONE bucket: CONSTRAINED (rail/protection — reported, never graded) > ACTIVE (command moving
    // or recent challenge — the graded tracking denominator) > QUIET (steady dwell — earns
    // nothing). Band exits run an excursion stopwatch; damaging-side exposure + worst peak are
    // recorded unconditionally within valid time. No reference model: error is the PV vs the
    // PRE-SLEW command, so the user's slew/filter choices are part of what the numbers report.

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

    // ---- Voltage loop. Its one-sided authority (drives voltage up via current; can only
    // command zero and wait for loads to bring it down) is handled entirely by the
    // constrained-direction rules: the CV-entry climb (Icv pinned at the ceiling) and a
    // step-down descent (Icv pinned at zero) exclude themselves — no arrival/regulation
    // regimes needed. Measured in 12V-EQUIVALENT volts so every published mV figure (CSV2,
    // /cvtuninglog live, cloud acc_volt_* columns) compares across 12/24/36/48V systems.
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

  updateFieldTelemetry(dutyCycle, tick.currentBatteryVoltage, FieldResistance);

  fieldActiveStatus = (!gpio4IsLow && lastAppliedDuty > 0.01f)
                        ? ((sysMode == SYS_MODE_MANUAL) ? 3 : 1)
                        : 0;
  chargeStageDisplay = getChargeStageDisplayCode();

  uTargetRaw_cached = uTargetRaw;  // update pre-OV ceiling for next tick's fastOvBaseCap

  uint32_t aflM5 = micros();  // end of section 5: duty build + governor + state/telemetry
  // All temperature loop, output current PID, and duty pipeline state is now final for this tick.
  // Called only in the normal-mode path — shutdown/fault paths do not log here.
  pidLog_tick(currentMillis);
  // Inner Current PID firing-interval sample — reached only on a normal field-on tick
  // (past digitalWrite(4,HIGH)). loop() clears pfHasPrev while the field is off so the
  // first firing after a field-off stretch re-baselines instead of logging the off-gap.
  pidFire_record(currentMillis);
  // Best-Ever Front: fold one alternator-health sample per control tick (~200 Hz). Live only —
  // bench-sim folds at 1 Hz from altHealth_tick instead. Off/fault/shutdown early-return above this.
  if (altSimMode < 0.5f) TIMED_CALL(ft_altFold, altFold_tick(currentMillis));
  // Section profiler latch — full passes only; early returns above never reach here.
  // A whole-core preemption is charged to whichever section it landed in, same caveat
  // as the Function Timing table. Breakdown surfaces in /debug, resets with Reset Peak Values.
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

  // constrain(NaN,…) returns NaN (both compares false) and (uint32_t)NaN is undefined. This is the single
  // final write to the field PWM for every duty path, so it is the place to hard-stop a NaN.
  // Hard cap 99%: at 100% the LEDC output is solid DC with zero off-edges, so the high-side bootstrap
  // never refreshes on P-type wiring (C4 drains through R108 in ~0.6ms → UVLO chop). 99% guarantees a
  // refresh slice every period: 25us at the 400Hz default, 526ns at the 19455Hz ceiling (≈ 2.4 recharge
  // time constants — the ceiling is the tight case and it verified sufficient).
  if (isnan(percent)) percent = 0.0f;
  percent = constrain(percent, 0.0f, 99.0f);

  // LEDC 12-bit ceiling: the clock divider must exceed 1.0, so 19455Hz is the max the driver accepts
  // (19500 is rejected — bench 2026-08-16). NVS can carry an out-of-range value from older firmware;
  // clamp here so boot attach and frequency changes can never fail.
  SwitchingFrequency = constrain(SwitchingFrequency, 100.0f, 19455.0f);

  // Field-duty safety net for higher-voltage banks. Every duty path (Auto/manual/limp/fault) lands
  // here, so this is the one place that hard-bounds field duty even on the open-loop paths that bypass
  // the PID's MaxDuty limit (manual/limp/fault). MaxDuty is the real per-bus cap (its default is scaled
  // down on 24/36/48V so worst-case field current never exceeds the 12V case); clamp to it. Gated to >12V
  // so 12V manual mode bypasses MaxDuty (up to the 99% bootstrap cap above). Duty-ratio proxy, not a
  // measured amp limit.
  if (SYSTEM_VOLTAGE_CLASS > 12 && percent > MaxDuty) {
    percent = MaxDuty;
  }
  // Max Field Volts arm of the same net, deliberately NOT class-gated: a winding can be under-rated
  // for its own 12V bank, and the volts cap sits inert at its class-scaled default, so applying it on
  // 12V costs nothing until the installer lowers it on purpose. Reads the derived global directly
  // rather than ccDutyCeiling() so the MaxDuty gate above keeps its own class condition.
  if (percent > g_fieldVoltDutyCeil) {
    percent = g_fieldVoltDutyCeil;
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
  static char b[12];
  if (HAS_BATT_SHUNT) snprintf(b, sizeof(b), "%.1fA", Bcur);
  else strncpy(b, "n/a", sizeof(b));
  return b;
}

void updateChargingStage() {
  const uint32_t now = millis();
  const float v = getFiltV();
  const int soc = SOC_percent / 100;  // SOC_percent is %×100; the rebulk gates compare in plain percent

  // While a commissioning run is live (IN_PROGRESS and the wizard dialog heartbeat is still fresh) the
  // field is owned by the wizard — resting between steps (COMMISSION_IDLE) or driven by the running
  // test — so the pack is never actually being topped off. The IDLE stage would clear chargingEnabled
  // (buildTickSnapshot), dropping sysMode out of AUTO and gating off every field-driving step, which
  // freezes the wizard (and Restart can't recover it, since Restart doesn't touch the charge stage).
  // Staying out of IDLE also keeps voltageControlActive true so G1–G4 stay armed. A stale/closed dialog
  // clears within COMMISSION_HEARTBEAT_TIMEOUT_MS and normal IDLE resumes. Mirrors getChargeStageDisplayCode().
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

  // User "Restart charge cycle" button: force the machine back to Bulk (CC) and re-run the whole
  // bulk→absorption→float cycle, re-arming the tail-current and absorption timers. Same reset
  // enter_sys_auto() does on AUTO entry. Consumed here (control-loop task) rather than in the async
  // web handler so the stage flags are only ever written from one task. Only honored while an auto
  // charge stage owns the field: in Manual/Target-V/Maintain the flags don't drive output, and during
  // commissioning it would fight the wizard — those cases drop the request. If the pack is already full
  // it climbs straight back to the target and the tail/overshoot logic returns it to float/idle in
  // seconds; the request does NOT fix a broken tail exit (steady load / no shunt) — it re-sticks there.
  if (restartChargeCycleRequested) {
    restartChargeCycleRequested = false;
    if (fieldActiveStatus == 1 && TargetVoltageMode == 0 && MaintainMode == 0 && !commissioningOwnsBattery) {
      inBulkStage = true;
      inAbsorptionStage = false;
      inIdleStage = false;
      bulkVoltageHoldTimer = 0;
      absorptionTailTimer = 0;
      rebulkTimer = 0;
      floatStartTime = now;
      queueConsoleMessageF("Stage: RESTART→BULK (user request) | battV=%.2fV bulkTarget=%.2fV", v, BulkVoltage);
    } else {
      queueConsoleMessage("Restart charge cycle ignored: no auto charge stage active");
    }
  }

  // Two-sided hysteresis: timer arms when V reaches BulkVoltage − ENTER, resets only when V falls below BulkVoltage − EXIT. Prevents 30–50 mV idle noise (12V-bank figure) from resetting the hold timer — scaled ×V/12 so the band tracks the ~proportionally larger idle noise on 24/36/48V banks (BulkVoltage is class-scaled).
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
        "Stage: BULK→ABSORPTION (hold timer) | battV=%.2fV bulkTarget=%.2fV absTarget=%.2fV tailCurrent=%.1fA timeout=%.0fmin",
        v, BulkVoltage, AbsorptionVoltage, TailCurrent_A,
        (float)AbsorptionTimeoutMs / 60000.0f);
    }

  } else if (inBulkStage && inAbsorptionStage) {
    // ===== ABSORPTION (CV) =====
    ChargingVoltageTargetReq = AbsorptionVoltage;

    const bool thermallyConstrained = (thermalPenaltyAmps > 2.0f) && (uTargetAmps <= TailCurrent_A * 2.0f);  //if you ever want CV-awareness here you'd change it to Icv <= TailCurrent_A * 2.0f.
    // Tail current alone is ambiguous: a house load — or any non-thermal limiter holding the alternator
    // short of target — drives Bcur to ~0 or negative while the pack is nowhere near absorbed, ending
    // absorption on a half-charged bank. Require the bus to actually BE at the absorption target first.
    // One-sided: another charge source holding the bus ABOVE target is a legitimate tail condition.
    const float ABS_V_TAIL_BAND = 0.15f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f);
    const bool atAbsorbV = (v >= (AbsorptionVoltage - ABS_V_TAIL_BAND));
    // No battery shunt → Bcur is meaningless (reads ~0), which would false-trip tail on tick 1 and drop
    // the bank out of absorption instantly. Disable the tail path; absorption then ends on AbsorptionTimeoutMs.
    const bool tailReached = HAS_BATT_SHUNT && !thermallyConstrained && atAbsorbV && (Bcur <= TailCurrent_A);

    // AbsorptionTimeoutMs counts time actually spent AT the absorption voltage, not wall clock. Entering
    // absorption only proves the bank hit the voltage once — it says nothing about who put it there, so a
    // second charge source (or one idle-RPM sag) could otherwise burn the whole budget down and dump a
    // half-charged bank into Float. absorbWallCeiling bounds the pause so a bank that can never reach
    // target still leaves absorption (except while a followed CVL is what holds it below target — see
    // dvccCvlHolding below). Epoch-keyed to absorptionStartTime so every reset path
    // (enter_sys_auto, Restart button, bulk entry) re-arms this without knowing it exists.
    static bool absorbClockArmed = false;
    static uint32_t absorbClockEpoch = 0, absorbClockPrevMs = 0, absorbHeldMs = 0, absorbWallMs = 0;
    if (!absorbClockArmed || absorbClockEpoch != absorptionStartTime) {
      absorbClockArmed = true;
      absorbClockEpoch = absorptionStartTime;
      absorbHeldMs = 0;
      absorbWallMs = 0;
      absorbClockPrevMs = now;
    }
    // A followed charge-voltage limit (DVCC CVL) below our absorption target means the battery
    // authority is deliberately holding the bus down (cell balancing, a cell-high event, cold-charge
    // limiting), so the wall ceiling pauses with it — DVCC_FOLLOW_SPEC.md's "stage exits freeze
    // naturally" rule. Without it a long clamp force-exits absorption and, with UseFloat=0, parks in
    // IDLE with the field off while the battery is still asking for charge. Deliberately the exact
    // predicate that applies the clamp, so a bank merely hovering below target with no clamp stays
    // bounded. Wall time is accumulated rather than derived from absorptionStartTime so the pause
    // cannot outlive its stage (the epoch re-arm zeroes it) or a reboot (statics come up unarmed).
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
          "Stage: ABSORPTION→%s (tail current) | battV=%.2fV Bcur=%s tailThresh=%.1fA",
          nextStage, v, bcurForLog(), TailCurrent_A);
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
        "Stage: ABSORPTION→%s (%s) | atVoltage=%.0fmin of %.0fmin, elapsed=%.0fmin | battV=%.2fV Bcur=%s",
        nextStage,
        (absorbHeldMs >= AbsorptionTimeoutMs) ? "absorption time used up" : "absorption ran too long below target voltage",
        (float)absorbHeldMs / 60000.0f, (float)AbsorptionTimeoutMs / 60000.0f,
        (float)(uint32_t)(now - absorptionStartTime) / 60000.0f, v, bcurForLog());
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

    const bool rebulkCondition = (v < RebulkVoltage) || (RebulkCurrent_A > 0.0f && Bcur < -RebulkCurrent_A);

    bool allowRebulk = true;
    if (socInfoAvailable) {
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
          const char *why = (Bcur < -RebulkCurrent_A) ? "discharge current threshold"
                                                      : "voltage sag confirmed";
          queueConsoleMessageF(
            "Stage: IDLE→BULK (%s) | battV=%.2fV Bcur=%s tIdle=%lus",
            why, v, bcurForLog(), (unsigned long)(tIdle / 1000));
        }
      } else {
        rebulkTimer = 0;
      }
    } else {
      rebulkTimer = 0;
    }

  } else {
    // ===== FLOAT =====
    // UseFloat may have been disabled at runtime while already in float, or all
    // stage flags may be false for another reason (e.g. override mode exit).
    // Either way: if float is not wanted, move to IDLE now rather than charging
    // an already-full battery at an unregulated float voltage — except mid-commissioning, where IDLE
    // would drop the wizard out of AUTO (commissioningOwnsBattery); hold in float instead (field rests).
    if (UseFloat == 0 && !commissioningOwnsBattery) {
      inIdleStage = true;
      floatStartTime = now;
      rebulkTimer = 0;
      queueConsoleMessage("Stage: FLOAT→IDLE (UseFloat disabled)");
      return;
    }

    // Zero-current float (UseFloat=2): the voltage PI runs at BulkVoltage purely as an over-voltage
    // guard (same pattern as MaintainMode) — uTargetAmps=0 caps the setpoint, so no voltage is chased.
    // Voltage-float mode holds FloatVoltage as a real target.
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

    const bool rebulkCondition = (v < RebulkVoltage) || (RebulkCurrent_A > 0.0f && Bcur < -RebulkCurrent_A);

    bool sagConfirmed = false;

    bool allowRebulk = true;
    if (socInfoAvailable) {
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
                        : (Bcur < -RebulkCurrent_A) ? "discharge current threshold"
                                                    : "voltage sag confirmed";
      queueConsoleMessageF(
        "Stage: FLOAT→BULK (%s) | battV=%.2fV rebulkV=%.2fV tFloat=%lus",
        why, v, RebulkVoltage, (unsigned long)(tFloat / 1000));
    }
  }
}

uint8_t getChargeStageDisplayCode() {
  // Commissioning outranks every charge stage: whenever the wizard dialog is OPEN (its
  // commissionHeartbeat is still fresh) the device is being commissioned, so both the live
  // displays and the logged stage read COMMISSIONING — regardless of field state, including
  // the low-duty idle-rest hold between guided steps. A closed/crashed dialog goes stale within
  // COMMISSION_HEARTBEAT_TIMEOUT_MS and the stage reverts to whatever charging is actually doing.
  // Deliberately NOT keyed off commissionState: a paused-but-closed commission is not "commissioning".
  if (lastCommissionHeartbeatMs != 0 &&
      (uint32_t)(millis() - lastCommissionHeartbeatMs) < COMMISSION_HEARTBEAT_TIMEOUT_MS) {
    return CHARGE_STAGE_COMMISSION;
  }

  // Manual mode gets its own display regardless of bulk/absorption flags
  if (fieldActiveStatus == 3) {
    return CHARGE_STAGE_MANUAL;
  }

  // Only auto-active field should show any stage
  if (fieldActiveStatus != 1) {
    return CHARGE_STAGE_NONE;
  }

  // Override modes take priority over the normal stage display.
  // TargetVoltageMode checked first: if both flags were somehow set simultaneously,
  // TARGET_V is the more specific and intentional state to show.
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

  // VoltageDisagreeThreshold is already class-scaled at storage (seedVScale at creation +
  // applyNominalVoltageChange on a live class change), exactly like OvMeasMarginV — so it is compared
  // RAW. A prior build ALSO multiplied ×V/12 here, double-scaling it (×4 at 24V, ×16 at 48V) and
  // desensitizing MODE_WARNING_RAMP_AND_LOCKOUT (which DISABLES charging) on 24/36/48V systems.
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
 * isVoltageDisagreementCritical()
 * 
 * Critical disagreement threshold is 1.0V on a 12V bank, scaled by the system voltage class
 * (2.0V @24V, 3.0V @36V, 4.0V @48V). Keyed on SYSTEM_VOLTAGE_CLASS, not a BulkVoltage band —
 * BulkVoltage banding mis-bucketed near class edges (a 36V bulk ~41.7V landed in the 48V band).
 *
 * A critical disagreement means one sensor has completely failed and we
 * cannot trust voltage readings for field control decisions.
 *
 * @return true if sensors disagree by critical amount or either is invalid
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
/**
 * isTempSustainedWarning()
 * Returns true if temp has exceeded warning threshold for TempSustainedTimeout
 * Resets if temp drops below threshold (must be continuously above)
 */
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
    case MODE_NORMAL_AUTO_PID: return "NORMAL_AUTO_PID";
    case MODE_COMMISSION_IDLE: return "COMMISSION_IDLE";
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
    case REASON_MANUAL_MODE: return "MANUAL";
    case REASON_INA_OVERVOLTAGE: return "INA228 hardware overvoltage";
    case REASON_FAST_OVERVOLTAGE: return "FAST_OVERVOLTAGE";
    case REASON_HARD_OVERCURRENT: return "HARD_OVERCURRENT";
    case REASON_RPM_TOO_LOW: return "RPM_TOO_LOW";
    case REASON_CURRENT_STALE: return "CURRENT_STALE";
    case REASON_BATTERY_TOO_COLD: return "Battery too cold to charge";
    case REASON_BATTERY_TOO_HOT: return "Battery too hot to charge";
    case REASON_COMMISSION_REST: return "Commissioning idle (field resting)";
    case REASON_TACH_IMPLAUSIBLE: return "TACH_IMPLAUSIBLE";
    case REASON_SOLAR_PAUSE: return "SOLAR_PAUSE";
    case REASON_BMS_DISABLED: return "BMS_OFF";
    case REASON_OV_TIER_LOW: return "OV_TIER_LOW";
    case REASON_OV_TIER_MID: return "OV_TIER_MID";

    default: return "UNKNOWN";
  }
}

/**
 * selectFieldControlMode()
 * PURE function - determines mode from tick snapshot only
 * Priority: Disabled (user off switch) > Manual (unrestricted) > Critical > Warning > Lockout > Auto
 */
FieldControlMode selectFieldControlMode(const TickSnapshot &tick) {

  // PRIORITY 1: DISABLED (user On/Off switch - always respected)
  if (!tick.chargingEnabled) {
    return MODE_DISABLED_RAMP;
  }

  // PRIORITY 1.5: FAST OVER-VOLTAGE — absolute ceiling, fires in ALL modes incl. MANUAL.
  // Live per-tick voltage vs AlternatorHardShutdownV. Reason REASON_FAST_OVERVOLTAGE is in
  // shouldImmediatelyCutGPIO4 → instant field cut + adaptive lockout (nextFastOvLockoutMs
  // ladder, 0.5s escalating to 10s). Deliberately ABOVE the manual
  // branch (manual otherwise bypasses all safeties) and NOT gated by testProtectionsEnabled,
  // because over-voltage is always disastrous. Must mirror selectFieldEventReason.
  if (tick.currentBatteryVoltage > tick.alternatorHardShutdownV) {
    return MODE_WARNING_RAMP_AND_LOCKOUT;
  }

  // PRIORITY 2: MANUAL MODE (UNRESTRICTED - bypasses all safeties when user wants manual control)
  if (tick.manualMode) {
    return MODE_NORMAL_MANUAL;
  }

  // PRIORITY 2.1: TIMED OV TIERS — sustained-violation cut latched by the FAST OV block
  // (detector is AUTO/CV-gated like G1/G2, so this is below MANUAL by construction; the latch
  // clears once the filtered bus drops back under the tier line). Immediate cut + adaptive
  // fast-OV lockout via shouldImmediatelyCutGPIO4. Must mirror selectFieldEventReason.
  if (g_ovTierTrip != 0) {
    return MODE_WARNING_RAMP_AND_LOCKOUT;
  }

  // PRIORITY 3: RPM GATE (field must be cut when engine is not running)
  if (tick.rpmBelowMinimum) {
    return MODE_CRITICAL_RAMP;
  }

  // PRIORITY 3.2: TACH-LIE PLAUSIBILITY (tach claims running, field driven hard, zero output —
  // the RPM signal is noise or the alternator is dead; either way charging is impossible).
  // Immediate cut via shouldImmediatelyCutGPIO4 + escalating lockout. Mirrors selectFieldEventReason.
  if (tick.tachImplausible) {
    return MODE_CRITICAL_RAMP;
  }

  // PRIORITY 3.5: COLD-CHARGE LOCKOUT (opt-in lithium protection — battery temperature below floor).
  // Graceful ramp-to-zero + lockout (not an instantaneous emergency, so no hard chop); the field
  // settle-cut path finishes the disconnect. Below MANUAL (manual stays unrestricted), so it only
  // applies in AUTO — mirrors selectFieldEventReason. Hysteresis lives in buildTickSnapshot.
  if (tick.batteryTooCold) {
    return MODE_WARNING_RAMP_AND_LOCKOUT;
  }

  // PRIORITY 3.6: HOT-CHARGE LOCKOUT (opt-in — a MEASURED battery temperature above MaxChargeTempF; the
  // board stand-in never drives it). Same graceful ramp + lockout as the cold check, below MANUAL for the
  // same reason — mirrors selectFieldEventReason. Hysteresis and the source gate live in buildTickSnapshot.
  if (tick.batteryTooHot) {
    return MODE_WARNING_RAMP_AND_LOCKOUT;
  }

  // PRIORITY 4: CRITICAL CONDITIONS (auto mode only)
  if (tick.tempDataVeryStale && !tick.ignoreTemperature) {
    return MODE_CRITICAL_RAMP;
  }
  if (!tick.voltagePlausible || tick.voltageDisagreementCritical) {
    return MODE_CRITICAL_RAMP;
  }
  if (!tick.ignoreTemperature && tick.tempToUseF > (tick.tempLimitF + tick.tempCritExcessF)) {
    return MODE_CRITICAL_RAMP;
  }

  // PRIORITY 5: WARNING CONDITIONS (all start lockout)
  // (The AlternatorHardShutdownV over-voltage check lives at PRIORITY 1.5 as the fast-OV
  //  immediate cut — armed in every mode and ungated — so it is not repeated here.)
  if (tick.voltageDisagreementWarning) {
    return MODE_WARNING_RAMP_AND_LOCKOUT;
  }
  if (!tick.ignoreTemperature && tick.tempToUseF > (tick.tempLimitF + tick.tempWarnExcessF)) {
    return MODE_WARNING_RAMP_AND_LOCKOUT;
  }

  // PRIORITY 7: LOCKOUT
  if (tick.inLockout) {
    return MODE_LOCKOUT_RAMP;
  }

  // PRIORITY 7.6: COMMISSIONING IDLE REST — between guided wizard steps (session live, dialog alive,
  // no test running), hold the field at a low duty instead of resuming bulk/CV charging. Below every
  // safety/lockout gate above (a real fault still wins and ramps to 0); above NORMAL AUTO. Only ever
  // reached in AUTO, since MANUAL returns at PRIORITY 2. Mirrors selectFieldEventReason.
  if (tick.commissioningResting) {
    return MODE_COMMISSION_IDLE;
  }

  // PRIORITY 8: NORMAL AUTO
  return MODE_NORMAL_AUTO_PID;
}

/**
 * selectFieldEventReason()
 * PURE function - MUST match selectFieldControlMode() priority exactly
 */
FieldEventReason selectFieldEventReason(const TickSnapshot &tick) {
  // Priority 1: Hardware overvoltage - overrides everything
  if (tick.inaOvervoltageLatched) return REASON_INA_OVERVOLTAGE;

  if (HardOCEnable && MeasuredAmps > HardOCTripAmps) {
    if (hardOCStartMs == 0) hardOCStartMs = tick.nowMs;
    if ((tick.nowMs - hardOCStartMs) >= HardOCDebounceMs) {
      return REASON_HARD_OVERCURRENT;
    }
  } else {
    hardOCStartMs = 0;
  }

  // Priority 1a: Disabled (user control - always show this reason if off)
  if (!tick.chargingEnabled) return tick.solarForecastPause ? REASON_SOLAR_PAUSE
                                    : (tick.bmsBlocking ? REASON_BMS_DISABLED : REASON_CHARGING_DISABLED);

  // Priority 1.5: Fast over-voltage — absolute ceiling, above the manual bypass and ungated.
  // Mirrors selectFieldControlMode PRIORITY 1.5. Live voltage → immediate cut + adaptive lockout.
  if (tick.currentBatteryVoltage > tick.alternatorHardShutdownV) return REASON_FAST_OVERVOLTAGE;

  // Priority 2: Manual mode
  if (tick.manualMode) return REASON_MANUAL_MODE;

  // Priority 2.1: Timed OV tiers — mirrors selectFieldControlMode. MID before LOW (higher rung).
  if (g_ovTierTrip == 2) return REASON_OV_TIER_MID;
  if (g_ovTierTrip == 1) return REASON_OV_TIER_LOW;

  // Priority 3: RPM gate
  if (tick.rpmBelowMinimum) return REASON_RPM_TOO_LOW;

  // Priority 3.2: Tach-lie plausibility — mirrors selectFieldControlMode.
  if (tick.tachImplausible) return REASON_TACH_IMPLAUSIBLE;

  // Priority 3.5: Cold-charge lockout (opt-in lithium protection). Mirrors selectFieldControlMode.
  if (tick.batteryTooCold) return REASON_BATTERY_TOO_COLD;

  // Priority 3.6: Hot-charge lockout (opt-in, measured battery source only). Mirrors selectFieldControlMode.
  if (tick.batteryTooHot) return REASON_BATTERY_TOO_HOT;

  // Priority 4: Critical (auto mode only)
  if (tick.tempDataVeryStale && !tick.ignoreTemperature) return REASON_TEMP_STALE;
  if (tick.currentDataStale) return REASON_CURRENT_STALE;
  if (!tick.voltagePlausible) return REASON_VOLTAGE_IMPLAUSIBLE;
  if (tick.voltageDisagreementCritical) return REASON_VOLTAGE_DISAGREE_CRITICAL;
  if (!tick.ignoreTemperature && tick.tempToUseF > (tick.tempLimitF + tick.tempCritExcessF)) {
    return REASON_TEMP_CRITICAL;
  }

  // Priority 5: Warning
  // (AlternatorHardShutdownV over-voltage is handled at Priority 1.5 as REASON_FAST_OVERVOLTAGE.)
  if (tick.voltageDisagreementWarning) return REASON_VOLTAGE_DISAGREE_WARNING;
  if (!tick.ignoreTemperature && tick.tempToUseF > (tick.tempLimitF + tick.tempWarnExcessF)) {
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

// Tracks rising edges on reasons that map to protection counters.
// Called once per loop tick after selectFieldEventReason().
void updateProtectionCounters(FieldEventReason reason) {
  static FieldEventReason prevReason = REASON_NONE;
  if (reason != prevReason) {
    switch (reason) {
      case REASON_INA_OVERVOLTAGE:
        g_inaOVCount++;
        g_ovTel.inaCutCount++;  // lifetime twin (RTC) of the session counter
        break;
      case REASON_HARD_OVERCURRENT: g_hardOCCount++; break;
      case REASON_FAST_OVERVOLTAGE:
        g_voltSpikeCount++;  // reuses the OV-spike counter (REASON_VOLTAGE_SPIKE retired)
        g_ovTel.swHardCutCount++;  // lifetime twin (RTC) of the session counter
        break;
      case REASON_OV_TIER_LOW:
        g_ovTierLowCount++;
        g_ovTel.tierLowCutCount++;  // lifetime twin (RTC) of the session counter
        break;
      case REASON_OV_TIER_MID:
        g_ovTierMidCount++;
        g_ovTel.tierMidCutCount++;  // lifetime twin (RTC) of the session counter
        break;
      case REASON_VOLTAGE_SPIKE: g_voltSpikeCount++; break;
      case REASON_VOLTAGE_DISAGREE_CRITICAL: g_voltDisagreeCritCount++; break;
      case REASON_VOLTAGE_DISAGREE_WARNING: g_voltDisagreeWarnCount++; break;
      case REASON_VOLTAGE_IMPLAUSIBLE: g_voltImplausibleCount++; break;
      case REASON_TEMP_CRITICAL: g_tempCritCount++; break;
      case REASON_TEMP_SUSTAINED: g_tempSustainedCount++; break;
      case REASON_TEMP_STALE: g_tempStaleCount++; break;
      case REASON_CURRENT_STALE: g_currentStaleCount++; break;
      default: break;
    }
    prevReason = reason;
  }
}

void reportFieldModeEvent(uint32_t nowMs, FieldControlMode mode, FieldEventReason reason,
                          const TickSnapshot &tick, bool gpio4Low, float appliedDuty) {
  // Only report when mode or reason actually changes
  if (mode == lastReportedMode && reason == lastReportedReason) {
    return;  // Same state - skip
  }

  lastReportedMode = mode;
  lastReportedReason = reason;
  lastReportMs = nowMs;

  char msg[300];

  // Calculate lockout time remaining
  char lockoutStatus[32] = "none";
  if (tick.inLockout) {
    float remaining = (activeCollapseDelay - (tick.nowMs - fieldCollapseTime)) / 1000.0f;
    snprintf(lockoutStatus, sizeof(lockoutStatus), "%.1fs remaining", remaining);
  }

  const char *fieldStatus = gpio4Low ? "OFF" : "ON";

  // Build message based on reason type
  if (reason == REASON_TEMP_STALE || reason == REASON_TEMP_CRITICAL || reason == REASON_TEMP_WARNING || reason == REASON_TEMP_SUSTAINED) {
    // Temperature-related events
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
    // Voltage-related or other events
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
 * updateFieldTelemetry()
 * Centralized telemetry update
 * Note: When GPIO4 is LOW, PWM is electrically disconnected, but we still
 * report duty-based values for logging consistency
 */
void updateFieldTelemetry(float duty, float voltage, float fieldResistance) {
  vvout = duty / 100.0f * voltage;
  iiout = (fieldResistance > 0.001f) ? (vvout / fieldResistance) : 0.0f;
  MARK_FRESH(IDX_DUTY_CYCLE);
  MARK_FRESH(IDX_FIELD_VOLTS);
  MARK_FRESH(IDX_FIELD_AMPS);
}


/**
 * shouldImmediatelyCutGPIO4()
 * Returns true if conditions warrant skipping Phase 1 ramp and cutting immediately
 * 
 * NOTE: When this returns true, the subsequent applyDutyCycle() call is for
 * rate-limiter state tracking and telemetry only. The PWM output is electrically
 * irrelevant since GPIO4 will already be LOW.
 */
bool shouldImmediatelyCutGPIO4(FieldEventReason reason) {
  if (reason == REASON_FAST_OVERVOLTAGE) return true;   // absolute OV ceiling — instant cut in every mode
  if (reason == REASON_OV_TIER_LOW) return true;        // timed OV tiers — dwell already served in the detector; the cut itself is immediate
  if (reason == REASON_OV_TIER_MID) return true;
  if (reason == REASON_INA_OVERVOLTAGE) return true;
  if (reason == REASON_HARD_OVERCURRENT) return true;
  if (reason == REASON_RPM_TOO_LOW) return true;
  if (reason == REASON_TACH_IMPLAUSIBLE) return true;  // tach already garbage; every energized second is pure battery drain
  // Stale current must cut here: a dead CH1 means ch1FreshFlag never sets, so
  // AdjustFieldLearnMode returns at its freshness gate before the ramp-down /
  // settle-cut path ever runs — without this the field freezes at last duty.
  // (CURRENT_STALE also outranks TEMP_CRITICAL in selectFieldEventReason, so
  // a non-cutting stale reason would mask the temp-critical cut too.)
  if (reason == REASON_CURRENT_STALE) return true;
  if (reason == REASON_TEMP_CRITICAL) {
    return true;
  }
  return false;
}

/**
 * shouldCutGPIO4AfterSettle()
 * Returns true if GPIO4 should go LOW after duty has settled at 0%
 * 
 * IMPORTANT: Call this AFTER governor_apply() with the duty that was
 * computed (even if not written to hardware). This ensures settle detection
 * matches what would be sent to hardware.
 * 
 * Does NOT reset settledAtZeroDutyMs on mode changes if duty is still at 0.
 * This prevents mode churn from delaying the cut indefinitely.
 * 
 * @param reason       Current fault/event reason
 * @param nowMs        Current timestamp (use consistently, not millis())
 * @param appliedDuty  The duty that was applied (float)
 * @return             true if GPIO4 should be cut LOW
 */
bool shouldCutGPIO4AfterSettle(FieldEventReason reason, uint32_t nowMs, float appliedDuty) {
  // Check if duty has reached 0% (use small threshold for float comparison)
  if (appliedDuty > 0.01f) {
    settledAtZeroDutyMs = 0;  // Not settled - reset timer
    return false;
  }

  // Duty is at 0%, start or continue settle timer
  if (settledAtZeroDutyMs == 0) {
    settledAtZeroDutyMs = nowMs;
    return false;
  }

  // Check if settled long enough
  if (nowMs - settledAtZeroDutyMs < SettleTimeBeforeCut) {
    return false;
  }

  // Settled at 0% for SettleTimeBeforeCut - check if cut is warranted
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

      // Normal user shutdown: cut after settle (Phase 4 handles the slow ramp, we're done)
    case REASON_CHARGING_DISABLED:
      return true;

    default:
      return false;
  }
}



/**
 * buildTickSnapshot()
 * Constructs immutable snapshot of system state for pure decision functions
 */
TickSnapshot buildTickSnapshot(uint32_t currentMillis, uint32_t dt_ms) {
  TickSnapshot tick = {};

  tick.nowMs = currentMillis;
  tick.dt_ms = dt_ms;

  tick.inaOvervoltageLatched = inaOvervoltageLatched;

  // Voltage sensors
  tick.batteryV = BatteryV;
  tick.ibv = IBV;
  tick.currentBatteryVoltage = getBatteryVoltage();

  // Re-solve the Max Field Volts ceiling before any consumer of ccDutyCeiling() runs this tick.
  updateFieldVoltCeiling(tick.currentBatteryVoltage, (float)dt_ms / 1000.0f);

  tick.batteryCurrentA = Bcur;

  tick.rpmMinDuty = getMinimumFieldForRPM(RPM);

  // Control state
  tick.manualMode = (ManualFieldToggle == 1);
  tick.ignoreTemperature = (IgnoreTemperature != 0);
  tick.ignoreRPM = (IgnoreRPM != 0);

  // Charging enabled (with BMS and weather mode overrides)
  bool chargingEnabledLocal = (Ignition == 1 && OnOff == 1);

  // GPIO42 = BMSLogic opto (U16); inverted — the phototransistor pulls the pin low when the
  // external 5-28 V signal is present. Read every tick even with bmsLogic off: the Integrations
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
  // Idle stage (UseFloat=0, post-absorption): battery is full, stop charging.
  // Rebulk logic in updateChargingStage() still runs and clears inIdleStage when
  // voltage sags or discharge current threshold is hit — chargingEnabled recovers
  // automatically on the next tick.
  if (inIdleStage) chargingEnabledLocal = false;

  // Weather mode: rest the alternator when the solar forecast is strong. Applied LAST so the flag
  // means "a sunny forecast is the ONLY thing holding charging off" — the banner must not blame
  // the forecast when the key, the on/off switch, the BMS, or a full battery is the real cause.
  tick.solarForecastPause = chargingEnabledLocal && weatherModeEnabled == 1 && currentWeatherMode == 1;
  if (tick.solarForecastPause) chargingEnabledLocal = false;

  tick.chargingEnabled = chargingEnabledLocal;

  // Protection-event grace: a clamp runs the field near MinDuty (tach floor dropped), which can
  // starve the LM2907 pickup — an EXACTLY-zero reading inside the window is signal dropout, not a
  // stopped engine. Only literal 0 is masked (a real post-blip idle droop reads nonzero and gates
  // normally); a genuine stall cuts the moment the window expires because rpmZeroSinceMs keeps
  // counting below. Only while charging is enabled: a shutdown (toggle/key-off/BMS/weather) has
  // nothing to recover, and deferring the cuts just keeps the field energized where its PWM fakes
  // tach readings (phantom RPM in datalogs).
  // Field-cut test (commissioning stage 7) deliberately drops the field to MinDuty to time the
  // current decay; the abrupt cut corrupts the LM2907 pickup — it spikes HIGH, false-zeros, then
  // ramps back through garbage low-nonzero values for ~4.6 s while the front end re-biases. RPM
  // plays no role in the measurement, so mask BOTH RPM gates for ANY reading (not just exact zero —
  // the 0<RPM<MinRPMForField re-bias ramp was aborting finished runs with RPM_TOO_LOW) while the
  // test runs and through the re-bias tail. Field at/below the test level cannot over-volt; the
  // absolute protections stay live. Not gated on chargingEnabled (the test always eases the field
  // back, so there is always something to recover).
  bool fieldCutRpmGrace = (fieldCutActive != 0) || fieldCutCcActive || fieldCutRequested
                          || (protTestActive != 0) || protTestCcActive || protTestRequested
                          || (fieldCutLastEndMs != 0
                              && (uint32_t)(currentMillis - fieldCutLastEndMs) < FIELDCUT_RPM_GRACE_MS)
                          // protTest needs its own end-stamp tail: the mode-1/4 cut lands one tick AFTER
                          // the flags above clear (pre-gate consumes protTestCutPending), so the whole
                          // ~4.6s false-zero would otherwise run unmasked → phantom engine-stop + restart confirm.
                          || (protTestLastEndMs != 0
                              && (uint32_t)(currentMillis - protTestLastEndMs) < FIELDCUT_RPM_GRACE_MS);
  // Hard-cut tail: an abrupt protection cut slams the LM2907 the same way the field-cut test does —
  // spike HIGH, false-zero, then a garbage low-NONZERO ramp for ~4.6 s. The clamp term below misses
  // it twice over (applyImmediateCut never stamps g_lastProtClampMs, and the ramp is nonzero), so
  // rpmBelowMinimum used to fire a phantom RPM_TOO_LOW cut on top of the real protection — stealing
  // the abort reason from a commissioning ramp and arming rpmRestartPending's 2 s dwell for nothing.
  // Mask both gates for ANY reading, but only while there is something to protect: a commissioning
  // ramp owns the field outright, and in normal running chargingEnabled excludes a key-off/BMS
  // spin-down, where the cut is wanted and an energized field only fakes tach readings.
  bool hardCutRpmGrace = (g_lastFieldCutMs != 0)
                         && ((uint32_t)(currentMillis - g_lastFieldCutMs) < FIELDCUT_RPM_GRACE_MS)
                         && ((fieldCurveActive != 0) || chargingEnabledLocal);
  // Published for the engine start/stop console lines (5_functions.ino). The protection gates below
  // already ignore the corrupted tach; the log line did not, so a commissioning field-cut printed
  // "Engine STOPPED"/"STARTED" for an engine that never changed state.
  bool rpmDropoutGrace = fieldCutRpmGrace || hardCutRpmGrace
                         || ((g_lastProtClampMs != 0)
                             && ((uint32_t)(currentMillis - g_lastProtClampMs) < PROT_RPM_GRACE_MS)
                             && chargingEnabledLocal
                             && (RPM <= 0.0f));
  // Engine confirmed stopped: RPM held at exactly 0 for >= RPM_ZERO_CUT_MS. RPM is already
  // floored to 0 below 100 in ReadAnalogInputs, so RPM <= 0 means a true zero. Skipped when
  // IgnoreRPM is set (no trustworthy RPM signal). Drives the immediate-cut override below.
  if (!tick.ignoreRPM && RPM <= 0.0f) {
    if (rpmZeroSinceMs == 0) rpmZeroSinceMs = currentMillis;
  } else {
    rpmZeroSinceMs = 0;
  }
  g_rpmTachMasked = rpmDropoutGrace;
  tick.engineFullyStopped = (!rpmDropoutGrace && rpmZeroSinceMs != 0 && (currentMillis - rpmZeroSinceMs) >= RPM_ZERO_CUT_MS);

  // Engine-restart confirmation: a confirmed stop arms rpmRestartPending; RPM must then hold
  // >= MinRPMForField for RPM_RESTART_CONFIRM_MS before the RPM gate releases, so a lone noise
  // blip (or a phantom's first seconds) can't re-energize the field. Grace-masked dropouts never
  // arm it — only a true zero-hold stop does. Real starts pass through the dwell unnoticed
  // beyond the ~2 s charging delay.
  if (tick.engineFullyStopped) rpmRestartPending = true;
  if (!tick.ignoreRPM && RPM >= (float)MinRPMForField) {
    if (rpmAboveMinSinceMs == 0) rpmAboveMinSinceMs = currentMillis;
    if (rpmRestartPending && (uint32_t)(currentMillis - rpmAboveMinSinceMs) >= RPM_RESTART_CONFIRM_MS) {
      rpmRestartPending = false;
      queueConsoleMessageF("Engine restart confirmed (RPM=%d held %.1fs)", (int)RPM, RPM_RESTART_CONFIRM_MS / 1000.0f);
    }
  } else {
    rpmAboveMinSinceMs = 0;
  }
  tick.rpmBelowMinimum = (!tick.ignoreRPM && !rpmDropoutGrace
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

  // Cold-charge lockout: latched with ColdChargeHysteresisF to stop chatter at the threshold.
  // Fail-open when no battery temperature source qualifies (NAN) so a dead sensor can't brick charging.
  {
    static bool coldLatched = false;
    if (coldChargeLockoutEnable && isfinite(tick.battTempF)) {
      if (tick.battTempF < MinChargeTempF) {
        coldLatched = true;
      } else if (tick.battTempF >= MinChargeTempF + ColdChargeHysteresisF) {
        coldLatched = false;
      }
    } else {
      coldLatched = false;
    }
    tick.batteryTooCold = coldLatched;
  }

  // Hot-charge lockout: mirror of the cold latch. Only a measured battery source (1..4) may drive it —
  // the board stand-in runs warmer than the pack and would lock charging out on a warm day.
  {
    static bool hotLatched = false;
    if (hotChargeLockoutEnable == 1 && tick.battTempSrc >= 1 && tick.battTempSrc <= 4 && isfinite(tick.battTempF)) {
      if (tick.battTempF > MaxChargeTempF) {
        hotLatched = true;
      } else if (tick.battTempF <= MaxChargeTempF - HotChargeHysteresisF) {
        hotLatched = false;
      }
    } else {
      hotLatched = false;
    }
    tick.batteryTooHot = hotLatched;
  }

  // Battery temperature moved >= 5 F, or its source class (measured / board / none) changed -> refresh the
  // CV gain derate, at most once per 30 s. Mirrors the BMP388 drift trigger in 5_functions.ino;
  // recomputeCvGains is arithmetic only, so it is safe on the control core.
  {
    static float    lastDerateTempF = NAN;
    static uint8_t  lastDerateClass = 0;
    static uint32_t lastDerateMs = 0;
    uint8_t cls = (tick.battTempSrc >= 1 && tick.battTempSrc <= 4) ? 1 : ((tick.battTempSrc == 5) ? 2 : 0);
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

  // Commissioning idle-rest: session IN_PROGRESS + the wizard dialog still pinging + no commissioning
  // test currently driving the field. Stale heartbeat (closed/crashed dialog, dropped Wi-Fi) clears
  // it within COMMISSION_HEARTBEAT_TIMEOUT_MS so normal charging resumes. The Disturbances sweep
  // (faCommissionGate) needs live charging, so it suppresses rest like the other tests.
  {
    bool dialogAlive = (lastCommissionHeartbeatMs != 0) &&
                       ((uint32_t)(currentMillis - lastCommissionHeartbeatMs) < COMMISSION_HEARTBEAT_TIMEOUT_MS);
    // Include the PENDING request flags, not just the active ones. selectFieldControlMode returns
    // MODE_COMMISSION_IDLE → AdjustField early-returns at runCommissionIdle() BEFORE fieldCurve_tick/
    // systemID_tick run, so a freshly-queued request would never be consumed and fieldCurveActive/
    // systemIDActive would never rise — a permanent rest deadlock (the field ramp only started once the
    // dialog closed and resting cleared). Treating a pending request as "test active" lets the control
    // path fall through and consume it on the same tick.
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
    // Temp task (Core 0) can stamp dataTimestamps[] a few ms ahead of tick.nowMs (captured at loop
    // top on Core 1). The unsigned subtraction below would then underflow to ~4.29e9 ms (0xFFFFFFFF)
    // and trip a false CRITICAL stale cut. An "ahead" stamp means the read is current — treat it fresh.
    tempDataVeryStale = false;
  } else {
    uint32_t tempAge = tick.nowMs - tempTimestamp;
    tempDataVeryStale = (tempAge > 20000);
  }

  if (isnan(tempSelected) || tempSelected < -50.0f || tempSelected > 400.0f) {
    tempDataVeryStale = true;
  }

  // Idle-aware gate: below running speed the temp task stretches its poll (5s/60s in 2_functions.ino),
  // so the feed reads "stale" by the 20s rule BY DESIGN — field is off, nothing to protect, and a
  // CRITICAL stale cut would just loop the 2s cooldown. Suppression holds 15s past spin-up because
  // the last engine-off sample can be 60s old; a genuinely dead sensor still cuts 15s after start.
  static uint32_t lastBelowRunRpmMs = 0;
  if (RPM < 200) lastBelowRunRpmMs = tick.nowMs;
  if (RPM < 200 || (uint32_t)(tick.nowMs - lastBelowRunRpmMs) < 15000UL) {
    tempDataVeryStale = false;
  }

  tick.tempDataVeryStale = tempDataVeryStale;

  // One-shot diagnostics on staleness transitions so a field cut from a starved temp feed explains
  // itself in the serial log: read age, last good value, WHY (a read gap vs an implausible value),
  // and which failure types accumulated DURING the gap (delta vs the snapshot taken at the last good
  // read). The periodic TempDbg line only shows boot totals — this pins blame to the actual event.
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
      // The idle gate above clears staleness on RPM alone, so this transition does NOT prove a read
      // succeeded. With no probe tempSelected is still NAN here — "fresh again, temp=nanF" claimed a
      // recovery that never happened. Say which of the two actually cleared it.
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

  // Tach-lie plausibility: tach claims running + loop COMMANDING current + field driven well above
  // the per-RPM onset floor + zero alternator output, held TACH_LIE_DWELL_MS. Impossible with a live
  // alternator above cut-in, so one of: the RPM signal is noise (field-PWM coupling sustains it — the
  // 2026-07-22 phantom runaway), the alternator is dead, or the field drive is open so the commanded
  // field makes no current (ON/OFF switch off, field wire loose, gate-drive/Q3 fault). The first two
  // waste battery through the field; the open-drive case wastes nothing but the cut still stops the churn.
  // Duty parked AT the floor with setpointLimited ~0 is the commissioned zero-output state — the knee
  // floor is DEFINED as the most field that still makes ~0 A — so both bars are relative, never flat
  // duty (2026-07-28: a flat 25% bar sat below the ~27% idle floor and false-cut three CV re-entries).
  // The setpoint bar also covers slew lag: a rev-up drops the floor faster than duty can follow, but
  // the loop is demanding nothing there. A real below-cut-in idle demanding current can still trip —
  // harmless (zero output foregone); a sustained rev-up past 1.5x the trip RPM (after the release
  // blank window) or plain tier expiry frees it.
  // MANUAL mode stays unrestricted by design; a stale current sensor can't fake the zero.
  // Commissioning sweeps (field curve/knee, SystemID, τ drain) hold high duty with zero output
  // below the onset knee ON PURPOSE — the lie signature exactly — and drive duty open-loop while
  // setpointLimited holds its pre-test value, so an active or pending sweep stays exempt; the knee
  // sweep is also mid-rewrite of the very floor table the arm bar reads (07-22 bench: idle knee
  // sweep aborted TACH_IMPLAUSIBLE at 743 RPM / 30% field).
  {
    bool sweepDrivingField = (fieldCurveActive != 0) || (systemIDActive != 0) || (fieldCutActive != 0)
                             || fieldCurveRequested || systemIDRequested || fieldCutRequested
                             || fieldCutCcActive || (protTestActive != 0) || protTestCcActive
                             || (altSweepActive != 0) || altSweepRequested
                             || (chcActive != 0) || chcRequested;
    bool tachLieNow = TachLieEnable && chargingEnabledLocal && !tick.manualMode && !tick.ignoreRPM
                      && !sweepDrivingField
                      && !tick.currentDataStale
                      && RPM >= (float)MinRPMForField
                      && setpointLimited > TACH_LIE_MAX_AMPS
                      && dutyCycle >= tick.rpmMinDuty + TACH_LIE_HEADROOM_FRAC * fmaxf(0.0f, ccDutyCeiling() - tick.rpmMinDuty)
                      && fabsf(MeasuredAmps) < TACH_LIE_MAX_AMPS;
    if (tachLieNow) {
      if (tachLieSinceMs == 0) tachLieSinceMs = currentMillis;
    } else {
      tachLieSinceMs = 0;
    }
    tick.tachImplausible = (tachLieSinceMs != 0 && (uint32_t)(currentMillis - tachLieSinceMs) >= TACH_LIE_DWELL_MS);
  }

  tick.voltagePlausible = isVoltageSensorPlausible();
  tick.voltageDisagreementCritical = isVoltageDisagreementCritical();
  tick.voltageDisagreementWarning = isVoltageDisagreementWarning(
    tick.nowMs, tick.batteryV, tick.ibv,
    tick.voltagePlausible, tick.voltageDisagreementCritical);

  // ── Suppress disagreement check during and after INA OV event ───────────
  // When the ALERT pin cuts the field, ADS1115 and INA228 will diverge
  // naturally as the field collapses — ADS responds faster. Without
  // suppression, VOLT_SPIKE fires on top of an already-handled hardware cut.
  // Suppression holds for INA_OV_DISAGREE_SUPPRESS_MS after the latch clears
  // to allow both sensors to resettle. 10s is deliberate — the INA228
  // averaged value can lag for several update cycles during transients.
  bool inaOvSuppressActive = inaOvervoltageLatched || (inaOvervoltageClearedMs > 0 && (tick.nowMs - inaOvervoltageClearedMs) < INA_OV_DISAGREE_SUPPRESS_MS);

  if (inaOvSuppressActive) {
    tick.voltageDisagreementWarning = false;
    tick.voltageDisagreementCritical = false;
    // Log once on entry to suppression so it's visible in the console
    static bool suppressLoggedThisCycle = false;
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
    static bool suppressLoggedThisCycle = false;
    suppressLoggedThisCycle = false;  // reset for next OV event
  }

  // Tach-lie lockout early release: ONLY a real rev-up (RPM well above the latched trip value —
  // sustained noise stays in its band, a throttle change doesn't) ends the standoff early, and only
  // after the tach can be trusted again: no release decision until TACH_LIE_RELEASE_BLANK_MS after
  // the cut (the collapse slams the LM2907 — a ~40 ms upward spike ~570 ms post-cut read as a
  // rev-up and released the 15 s lockout in ~600 ms, 2026-07-28 log), then the test must hold
  // TACH_LIE_RELEASE_HOLD_MS continuously so a lone spike frees nothing.
  // There is deliberately NO signal-dropped release — do not re-add one. A phantom is sustained by
  // field PWM, so the cut itself drives RPM to zero: releasing on the drop discards the ladder tier
  // on exactly the fault the ladder exists for, and the 07-22 phantom later RESEEDED with the field
  // off and passed the 2 s restart confirmation — no RPM-comes-back test discriminates either. The
  // tier is the back-off between probes (energize + TACH_LIE_DWELL_MS is the only real
  // phantom-vs-alternator test this hardware has); while RPM reads low the RPM gate holds the field
  // off regardless, and a real restart waits only the tier remainder, usually already elapsed.
  // Disabling the detector (TachLieEnable off) clears an in-flight lockout — the toggle is the
  // user's escape hatch and must not leave a 120 s tier running. An ignored tach (ignoreRPM) makes
  // no release decisions and must zero the hold timer, or a stale stamp satisfies the hold the
  // instant ignoreRPM is toggled back off. Every re-trip restamps fieldCollapseTime, so the blank
  // window self-resets.
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

  // Thresholds
  tick.bulkVoltage = BulkVoltage;
  tick.alternatorHardShutdownV = AlternatorHardShutdownV;
  tick.testProtectionsEnabled = testProtectionsEnabled;
  tick.inAbsorptionStage = inAbsorptionStage;

  return tick;
}


void resetLearningTableToDefaults() {
  for (int i = 0; i < RPM_TABLE_SIZE; i++) {
    rpmTableRPMPoints[i] = defaultRPMValues[i];
    rpmCapCurrentTable[i] = defaultCapCurrentValues[i];
    rpmCapPowerTable[i] = defaultCapPowerValues[i];
    rpmMinDutyTable[i] = defaultMinDutyValues[i];
    lastOverheatTime[i] = 0;
    overheatCount[i] = 0;
    cumulativeNoOverheatTime[i] = 0;
  }
  capLimitMode = 0;
  overheatPenaltyEndMs = 0;
  overheatingPenaltyAmps = 0;
  totalSafeMs = 0;
  // Reset diagnostics
  totalLearningEvents = 0;
  totalOverheats = 0;
  // Persist factory defaults for BOTH charge-rate modes explicitly, WITHOUT touching HiLow — this
  // also runs as the boot auto-reset when learning NVS is missing/invalid, so changing the mode here
  // would silently kick a Low-mode user back to Normal.
  {
    float loDefaults[RPM_TABLE_SIZE];
    float loDefaultsPwr[RPM_TABLE_SIZE] = { 0 };
    for (int i = 0; i < RPM_TABLE_SIZE; i++) loDefaults[i] = defaultCapCurrentValues[i] * LOW_MODE_CAP_FRACTION;
    nvs_handle_t nvs_h;
    if (nvs_open("learning", NVS_READWRITE, &nvs_h) == ESP_OK) {
      nvs_set_blob(nvs_h, "rpmPoints", rpmTableRPMPoints, sizeof(rpmTableRPMPoints));
      nvs_set_blob(nvs_h, "capTable", defaultCapCurrentValues, sizeof(defaultCapCurrentValues));   // Normal/High
      nvs_set_blob(nvs_h, "capPowerTable", defaultCapPowerValues, sizeof(defaultCapPowerValues));  // Normal/High
      nvs_set_blob(nvs_h, "capTableLo", loDefaults, sizeof(loDefaults));                            // Low
      nvs_set_blob(nvs_h, "capPowerTableLo", loDefaultsPwr, sizeof(loDefaultsPwr));                 // Low
      nvs_set_u8(nvs_h, "capLimitMode", capLimitMode);
      nvs_set_blob(nvs_h, "minDutyTable", rpmMinDutyTable, sizeof(rpmMinDutyTable));
      nvs_commit(nvs_h);
      nvs_close(nvs_h);
    }
  }
  // Refresh the live arrays for the user's ACTUAL mode (Normal or Low) — not a forced Normal.
  loadCapTablesForMode(HiLow);
  queueConsoleMessage("Learning: All tables reset to factory defaults");
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
      for (int i = 0; i < RPM_TABLE_SIZE; i++) rpmCapPowerTable[i] = 0.0f;
    }
  }

  // Load cap limit mode (non-critical, default to amps)
  err = nvs_get_u8(nvs_handle, "capLimitMode", &capLimitMode);
  if (err != ESP_OK) {
    capLimitMode = 0;
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

  if (!historicalDataValid) {
    totalOverheats = 0;
    totalLearningEvents = 0;
    queueConsoleMessage("Learning: Historical data invalid, counters reset");
  }

  nvs_close(nvs_handle);
  queueConsoleMessage("Learning: Table loaded from NVS");
}

// Loads the correct cap tables from NVS into the active arrays based on HiLow mode.
// HiLow=1 → Normal (key "capTable"), HiLow=0 → Low Charge Rate (key "capTableLo").
// Falls back to defaults if no saved data exists for that mode.
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
      rpmCapPowerTable[i] = 0.0f;
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
    for (int i = 0; i < RPM_TABLE_SIZE; i++) rpmCapPowerTable[i] = 0.0f;
  }
  nvs_close(nvs_handle);
}

// ── Switch panel inputs (Cable 3 / RJ3) ───────────────────────────────────────────────────────
// Two isolated inputs share the ignition wire's inverting-opto topology: HIGH/LOW on GPIO39 asks for
// the High charge rate, FORCEFLOAT on GPIO40 asks for Force "Maintain Mode". Which source is obeyed
// is a clean select, never a blend: PhysicalPanelOverride == 0 (the default) means the app's stored
// choice IS the mode and the wires only report their state; == 1 means the wires alone are the mode
// and the app's toggles are inert. HiLowUserSel / MaintainModeUserSel keep holding the app's choice
// either way, so switching the override on and back off restores exactly what the app had set.

// Effective charge-rate mode. Panel sense: switch ON (wire energized, pin LOW) = High (HiLow 1),
// switch off = Low (HiLow 0).
int resolveChargeRateMode() {
  if (PhysicalPanelOverride == 0) return (HiLowUserSel == 0) ? 0 : 1;
  return panelHiRateInput ? 1 : 0;
}

// Effective Force "Maintain Mode" — same select, plus the two physical gates the /get handler
// applies and that hold no matter who owns the mode: no battery-current sensor means a 0-net-amps
// hold is impossible, and Maintain/Target Voltage are mutually exclusive. A panel asking for
// Maintain yields to an active Target Voltage mode rather than clearing it, because clearing it is
// an NVS write and this runs in the control loop.
int resolveMaintainMode() {
  bool on = (PhysicalPanelOverride == 0) ? (MaintainModeUserSel == 1) : panelForceFloatInput;
  if (!BatteryShuntPresent) on = false;
  if (TargetVoltageMode == 1) on = false;
  return on ? 1 : 0;
}

// Single application path for a charge-rate change, shared by the /get?HiLow handler, the override
// handler and the panel service so every route behaves identically. Returns true when the live mode
// actually moved. Never touches NVS — persistence belongs to whoever owns the user's stored choice.
bool applyChargeRateMode(int newMode) {
  if (newMode == HiLow) return false;
  HiLow = newMode;
  loadCapTablesForMode(HiLow);  // swap active cap tables to match new mode
  // Do NOT deactivate the thermal loop here. The new cap is already honored: the
  // velocity-form penalty update clamps to the LIVE capCurrent (which tracks the new
  // table) every tick, so the penalty re-bounds to the new cap immediately. Setting
  // tempPIDActive=false would instead force the re-enable path, which CLEARS the slope
  // buffer — that restarts the 60s warmup window and drops the setpoint to limit-20 (the
  // spurious 20°F reduction seen on a Lo<->Hi switch), and also re-seeds the penalty
  // accumulator from P-only, dumping the learned holding level. A mode switch must be
  // bumpless for the thermal loop.
  stateRevision++;  // force immediate CSVData echo of new table values
  if (HiLow == 0) {
    // Switching to Low drops the ceiling. Capture the present ceiling and arm the glide so the
    // control loop ramps it down to the new Low cap instead of stepping (prevents the iExcess
    // false-trip on the deliberate command drop). Up-switches don't glide — the loop lets them rise.
    modeCapSlew = uTargetAmps;
    modeCapSlewActive = true;
  }
  return true;
}

// Reads both Cable 3 optos, then re-derives the two effective modes from whoever currently owns them.
// Called every loop pass from loop(), immediately after the ignition read. Debounced because each
// charge-rate edge swaps the cap tables out of NVS and can re-enter the charge-stage machine — a
// bouncing mechanical contact must not do that repeatedly. The ignition input needs no debounce
// because nothing edge-triggers off it. The reads and the console lines happen whether or not the
// panel has authority, so an installer can prove the wiring before handing it any.
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

  // First pass adopts the pins as-is. Without this the debounced states start at false, so a panel
  // that is already switched on reads as a released switch for the first 50ms — with the override on
  // that is a real mode change (cap-table swap, ceiling glide, console line) undone one tick later.
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

  // CSV3 (the only channel carrying HiLow / MaintainMode) goes out on settingsDirty or a 60 s fallback.
  // The /get handlers set the flag themselves; this loop path has to as well, or the app's toggles show
  // the old mode for up to a minute after a panel switch is flipped.
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

// Immediate save of all user-editable columns (no throttle)
// Used when user clicks Save button or Reset button
void saveUserTableEdits() {
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("learning", NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK) {
    queueConsoleMessageF("Learning: Failed to open NVS (err=%d)", (int)err);
    return;
  }

  bool success = true;

  // Save all user-editable columns
  err = nvs_set_blob(nvs_handle, "rpmPoints", rpmTableRPMPoints, sizeof(rpmTableRPMPoints));
  if (err != ESP_OK) success = false;

  {
    const char *capKey = (HiLow == 1) ? "capTable" : "capTableLo";
    const char *capPwrKey = (HiLow == 1) ? "capPowerTable" : "capPowerTableLo";
    err = nvs_set_blob(nvs_handle, capKey, rpmCapCurrentTable, sizeof(rpmCapCurrentTable));
    if (err != ESP_OK) success = false;
    err = nvs_set_blob(nvs_handle, capPwrKey, rpmCapPowerTable, sizeof(rpmCapPowerTable));
    if (err != ESP_OK) success = false;
  }

  err = nvs_set_u8(nvs_handle, "capLimitMode", capLimitMode);
  if (err != ESP_OK) success = false;

  err = nvs_set_blob(nvs_handle, "minDutyTable", rpmMinDutyTable, sizeof(rpmMinDutyTable));
  if (err != ESP_OK) success = false;

  err = nvs_commit(nvs_handle);
  if (err != ESP_OK) {
    queueConsoleMessageF("Learning: NVS commit failed (err=%d)", (int)err);
    success = false;
  }

  nvs_close(nvs_handle);

  if (!success) {
    queueConsoleMessage("Learning: Save completed with errors");
  }
}

// Persist both charge-rate cap tables (Low + High) plus the shared RPM breakpoints in one commit,
// from the pre-commissioning Charge Rate Limits web handler. Kept separate from saveUserTableEdits
// (which writes only the active mode) so the wizard seeds both without a live mode toggle. Written
// synchronously in the request so a following HiLow mode switch reads fresh blobs (no NVS race).
// Stage both cap tables plus the live RPM breakpoints for capBothPersistService(). Runs in the /get handler
// on the network task; the copy is ~200 B under the spinlock and nothing else. A second Continue while a
// save is still draining restarts the sequence from blob 1 with the newer values — every blob is rewritten,
// so the set that lands is always the latest one.
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

// Retire one staged blob per loop() pass; no-op when idle. The blob is copied out under the spinlock and
// written with the lock released. Advances only if no re-stage happened during the write, so a restarted
// sequence is never skipped past. A refused write stops the sequence and reports FAILED on /capSaveState;
// the tables stay live in RAM (the handler already applied the active mode's ceiling).
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

// Immediate save of historical data (no throttle)
// Called by clearOverheatHistoryAction() (user-initiated, off the control loop) and
// overheatHistFlush() (field-off settle) — never directly from a field-on control tick.
void saveHistoricalDataImmediate() {
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("learning", NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK) {
    queueConsoleMessageF("Learning: Failed to open NVS (%d)", (int)err);
    return;
  }

  bool success = true;

  // Save historical data only
  err = nvs_set_blob(nvs_handle, "overheatCount", overheatCount, sizeof(overheatCount));
  if (err != ESP_OK) success = false;

  err = nvs_set_blob(nvs_handle, "lastOverheat", lastOverheatTime, sizeof(lastOverheatTime));
  if (err != ESP_OK) success = false;

  err = nvs_set_blob(nvs_handle, "cumulativeTime", cumulativeNoOverheatTime, sizeof(cumulativeNoOverheatTime));
  if (err != ESP_OK) success = false;

  // Save diagnostic counters
  err = nvs_set_u32(nvs_handle, "totalEvents", (uint32_t)totalLearningEvents);
  if (err != ESP_OK) success = false;

  err = nvs_set_u32(nvs_handle, "totalOverheats", (uint32_t)totalOverheats);
  if (err != ESP_OK) success = false;

  err = nvs_set_blob(nvs_handle, "totalSafeMs", &totalSafeMs, sizeof(totalSafeMs));
  if (err != ESP_OK) success = false;

  err = nvs_commit(nvs_handle);
  if (err != ESP_OK) {
    success = false;
  }

  nvs_close(nvs_handle);

  if (!success) {
    queueConsoleMessage("Learning: Historical save completed with errors");
  }
}

static bool overheatHistDirty = false;

void overheatHistFlush() {
  if (!overheatHistDirty) return;
  overheatHistDirty = false;
  saveHistoricalDataImmediate();
}

void updateRPMBucketHistory(uint32_t nowMs) {
  static uint32_t lastHistoryMs = 0;
  uint32_t dtMs = (lastHistoryMs == 0) ? 0 : (uint32_t)(nowMs - lastHistoryMs);
  if (dtMs > 500) dtMs = 500;
  lastHistoryMs = nowMs;

  if (isnan(TempToUse) || TempToUse < -50.0f || TempToUse > 400.0f) return;
  if (dtMs == 0) return;

  int bucket = currentRPMTableIndex;
  if (bucket < 0 || bucket >= RPM_TABLE_SIZE) return;

  static bool inOverheat = false;

  if (!inOverheat) {
    if (TempToUse >= TemperatureLimitF) {
      inOverheat = true;
      overheatCount[bucket]++;
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
//
// All segment walking and interpolation is centralized here.
// findRPMSegment() is the single source of truth for boundary conditions —
// edit it once and all lookups, the index tracker, and bucket history
// stay consistent automatically.

/**
 * findRPMSegment()
 * Returns the lower-bound segment index for a given RPM.
 *
 *  -1              : rpm is below the first breakpoint (clamp to index 0)
 *   0..N-2         : rpm falls within segment [i, i+1]
 *   RPM_TABLE_SIZE-1 : rpm is at or above the last breakpoint (clamp to last)
 *
 * All table lookup functions and updateCurrentRPMTableIndex() call this
 * so boundary logic can never diverge between them.
 */
int findRPMSegment(float rpm) {
  if (rpm < rpmTableRPMPoints[0]) return -1;
  if (rpm >= rpmTableRPMPoints[RPM_TABLE_SIZE - 1]) return RPM_TABLE_SIZE - 1;
  for (int i = 0; i < RPM_TABLE_SIZE - 1; i++) {
    if (rpm >= rpmTableRPMPoints[i] && rpm < rpmTableRPMPoints[i + 1]) return i;
  }
  return RPM_TABLE_SIZE - 1;  // Fallback — should never reach
}

/**
 * interpolateRPMTable()
 * Linearly interpolates any float table at the given RPM.
 * Clamps to first/last entry when RPM is out of range.
 */
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
 * updateCurrentRPMTableIndex()
 * Sets currentRPMTableIndex to the segment that owns this RPM.
 * Clamps to 0 when below range so the first row is always highlighted.
 * Call once per tick before updateRPMBucketHistory().
 */
void updateCurrentRPMTableIndex(float rpm) {
  int seg = findRPMSegment(rpm);
  currentRPMTableIndex = (seg < 0) ? 0 : seg;
}

// One-liner lookups — all boundary logic lives in findRPMSegment/interpolateRPMTable
float getMinimumFieldForRPM(float rpm) {
  float floorV = interpolateRPMTable(rpm, rpmMinDutyTable);
  // A zero floor means "no minimum field" (above the commissioned RPM ceiling, or bin 0): keep it a
  // hard zero. Never let a temp correction lift it off zero, or the field could be forced on past the
  // ceiling where it must be able to shut fully off.
  if (floorV <= 0.0f) return 0.0f;
  // Copper temp correction — only while the learner machinery owns the table (master system toggle +
  // learner sub-toggle both on); otherwise the floor is the literal hand-entered value.
  // Only the RESISTIVE part of the knee scales with field-winding resistance (~0.218 %/degF); the
  // constant brush/rectifier threshold (kneeFitA) does NOT. Stored floors are referenced to
  // kneeTempRefF; shift the resistive part (knee − kneeFitA) to the live case temp. knee = floor+margin.
  // (kneeFitA = 0 when no commissioning fit exists → falls back to scaling the whole knee, as before.)
  if (kneeLearnEnable && kneeTempComp && !isnan(AlternatorTemperatureF)) {
    float knee = floorV + kneeMarginPct;
    float resistive = knee - kneeFitA; if (resistive < 0) resistive = 0;
    floorV -= resistive * 0.00218f * (kneeTempRefF - AlternatorTemperatureF);
    if (floorV < 0) floorV = 0;
    if (floorV > kneeMaxFloorPct) floorV = kneeMaxFloorPct;
  }
  // The scalar "Min Field %" hard-floors every non-zero table value (a cell can raise the floor,
  // never pull it below MinDuty). The zero sentinel above is the one exception.
  floorV = fmaxf(floorV, MinDuty);
  // The field ceiling (Max Field % / Max Field Volts) is enforced in hardware by setDutyPercent, so a
  // floor promised above it cannot be delivered — without this cap the governor resolves the inverted
  // bounds to the floor and lastAppliedDuty (telemetry, PID bumpless reseed, the hunt observer) reports
  // a duty the PWM is silently clamping away. Concede the floor to the ceiling so believed == actual.
  return fminf(floorV, ccDutyCeiling());
}
float getCapCurrentForRPM(float rpm) {
  return interpolateRPMTable(rpm, rpmCapCurrentTable);
}

// ====================================================================================
// AUTO MIN% LEARNING ("knee tracker")
// ====================================================================================
// Learns, per RPM bin, the field duty where output amps begin to build (the "knee") and parks
// rpmMinDutyTable a margin below it. Observer only: governor_apply is untouched — this rewrites
// rpmMinDutyTable like a user edit, and getMinimumFieldForRPM adds the live temp correction.
// Spec: Working Markdown Docs/MinDuty_Knee_Learning.md.
static bool kneeStateDirty = false;

// Called every control tick from the governor site (observer; cheap). Per RPM bin it runs a
// one-time probe: while parked at the floor and steady, capture a fresh zero, then step the floor
// up kneeStepPct each dwell until output rises kneeOnsetA above that zero (the knee), then lock the
// floor at knee - margin. A locked bin re-learns only if current (> kneeReArmA) later shows at its
// floor (knee dropped) — then the floor drops one margin step. Bin 0 is permanently locked at 0%.
// Stored floors are RAW duty @ kneeTempRefF; getMinimumFieldForRPM does the live temp correction.
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

  // Floor active = the controller wants no more field than the floor — the low-output / topped /
  // thermally-limited regime where the floor (not the PID) is setting the field. dutyRequest is the
  // inner PID output, which is clamped to >= MinDuty and (via bumpless tracking) sits AT the applied
  // floor while the floor is binding. So "floor binding" reads as dutyRequest <= floorEff, NOT
  // < floorEff: a strict "<" can never be true once floorEff == MinDuty (the PID output can't go
  // below MinDuty), so every bin would sit at 0% forever and the learner would never leave "watching".
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

  // The duty-domain knobs — kneeStepPct (staircase), kneeMarginPct (park margin / step-down),
  // kneeMaxFloorPct (ceiling) — are stored in REAL duty-% for THIS bus, so they're used as-is here
  // (no runtime normalization — WYSIWYG with the dashboard box). Their defaults are scaled by
  // ×(12/SYSTEM_VOLTAGE_CLASS) at first boot (kneeLearnInit) and rescaled in place on a system-voltage
  // change (applyNominalVoltageChange) so a 5% 12V margin shows/uses as 1.25% on a 48V bank — keeping
  // the field-CURRENT margin constant without hiding the math. Amps-domain knobs (kneeOnsetA,
  // kneeReArmA) and the learned floors are voltage-independent (the probe observes real onset).
  float stepEff = kneeStepPct;
  float maxFloorEff = kneeMaxFloorPct;
  float margin = (kneeMarginPct > 0) ? kneeMarginPct : 0;
  float caseF = isnan(AlternatorTemperatureF) ? 0.0f : AlternatorTemperatureF;

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

// Least-squares fit of the physical onset model  knee = a + C/RPM  over the committed anchors.
// Physics: in the linear field region the onset duty (where output current just begins) goes as
// R_field/(k·RPM) — inversely with RPM, battery-voltage-independent to first order. `a` absorbs the
// small constant rectifier/brush threshold; `C` is the lumped R_field/k at kneeTempRefF. Only the
// resistive C/RPM term carries temperature (copper R, ~0.218 %/°F of itself); the `a` threshold does
// NOT. So instead of temp-normalizing the whole onset (which wrongly dragged `a` with temperature),
// each anchor is regressed against x = (R(T)/R(Tref))/RPM with y = the RAW measured onset — the fitted
// `a` then comes out temperature-neutral and a + C/RPM evaluates directly as the kneeTempRefF knee.
// With a 2-parameter model, 3 anchors give the fit PLUS one residual to spot a bad point — hence the UI
// captures exactly 3. Outputs the worst |measured − fit| residual and which anchor it was, for the
// review screen's outlier flag. Returns false if < 2 anchors or the RPM span is degenerate.
bool kneeFitModel(float &outA, float &outC, float &outResidPct, int &outWorstIdx) {
  outA = 0.0f; outC = 0.0f; outResidPct = -1.0f; outWorstIdx = -1;
  if (kneeAnchorN < 2) return false;
  // Regress y = a + C·x with y = RAW onset and x = (R(T)/R(Tref))/RPM, so the copper-resistance ratio
  // lives in the regressor and `a` (the temp-independent threshold) is not temperature-scaled.
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
  if (C < 0) C = 0;                            // onset must rise as RPM falls; clamp a perverse fit
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

// Fit the Min% column from the committed onset-knee anchors (commissioning) via the kneeFitModel()
// physical curve above, then write it into rpmMinDutyTable (the learner's FROZEN baseline, so the
// background re-arm just maintains drift from here). Bin 0 stays locked at 0%. Returns false if < 3
// anchors (the UI captures 3 before Apply; this is the matching firmware guard).
//
// END BEHAVIOR (SAFETY-CRITICAL — do not change to flat-hold):
//   - BELOW the lowest anchor RPM: the model continues (onset keeps rising), clamped to maxKnee. Safe
//     — a floor at low RPM never prevents the field from shutting off.
//   - ABOVE the highest anchor RPM: force the floor to ZERO. The floor is a MINIMUM field the
//     regulator is forced to hold; carrying any floor past the commissioned range would force minimum
//     excitation the regulator could never turn off at high RPM → overvoltage/runaway. The UI captures
//     the user's MAXIMUM working RPM as the top anchor for exactly this reason — it is the zero-ceiling.
//     (A 1/RPM model is monotone-decreasing by construction, so it can never PRODUCE a dangerous high
//     floor at high RPM either — but the hard zero above the ceiling is the guarantee.)
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
    // SAFETY: above the highest commissioned RPM force the floor to ZERO (see header). Otherwise
    // evaluate the fitted onset = a + C/RPM at this bin.
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

  // SAFETY: interpolateRPMTable holds the LAST bin flat for every RPM beyond the table, so when
  // the ceiling anchor lands at/above the last breakpoint no bin is "above ceiling" and a
  // non-zero floor would ride to unlimited overspeed. Force the last bin to zero in that case —
  // the floor tapers out one bin early, which errs in the safe direction.
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

// Persist learned per-bin state (learning namespace blobs). Knobs persist separately via settingWrite.
void saveKneeLearnState() {
  nvs_handle_t h;
  if (nvs_open("learning", NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_blob(h, "kneeFloor", kneeFloor, sizeof(kneeFloor));
  nvs_set_blob(h, "kneeKnee", kneeKnee, sizeof(kneeKnee));
  nvs_set_blob(h, "kneeFrozen", kneeFrozen, sizeof(kneeFrozen));
  nvs_set_blob(h, "kneeFitA", &kneeFitA, sizeof(kneeFitA));  // temp-neutral threshold for the live temp correction
  nvs_commit(h);
  nvs_close(h);
  kneeStateDirty = false;
}

// ── Min%-floor snapshot for the commissioning ABORT path ──────────────────────
// The pre-commissioning tune snapshot (NK_commissionSnap, staged by cxStartPersistBegin) covers gains/filters/thresholds but
// NOT the Min% floor table, which the wizard's Min% floor step (stage 7) rewrites. These three back up / restore / clear
// the persistent Min% state (minDutyTable + the knee tracker: floor, knee, frozen) as parallel
// "bk_*" blobs in the "learning" namespace so an abort (or a reboot mid-run) reverts Min% too.
// kneeLearnTempF is diagnostic-only and never persisted, so it is not backed up.
// Takes the arrays by pointer because the deferred-Start worker persists RAM copies staged at the
// click, not the live tables (which the ~1 s persist window could have moved under it).
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
    nvs_set_blob(h, "minDutyTable", rpmMinDutyTable, sizeof(rpmMinDutyTable));
    nvs_set_blob(h, "kneeFloor", kneeFloor, sizeof(kneeFloor));
    nvs_set_blob(h, "kneeKnee", kneeKnee, sizeof(kneeKnee));
    nvs_set_blob(h, "kneeFrozen", kneeFrozen, sizeof(kneeFrozen));
    // Restore the live-correction threshold. Optional (old backups lack it) → 0 = whole-knee fallback.
    float fFitA = 0.0f; size_t szA = sizeof(fFitA);
    if (nvs_get_blob(h, "bk_kneeFitA", &fFitA, &szA) != ESP_OK || szA != sizeof(fFitA)) fFitA = 0.0f;
    kneeFitA = fFitA;
    nvs_set_blob(h, "kneeFitA", &kneeFitA, sizeof(kneeFitA));
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

// RPMScalingFactor is a linear gain on the engine-RPM axis, so everything learned or measured
// against the old axis is stale. Runs on Core 1 (flash writes). Drives the SAME clear paths the
// individual Clear buttons use, so there is one implementation per artifact. Deliberately does NOT
// touch: alternator wear accumulators (real integrated damage), the fuel curve (entered in true
// engine RPM), the Hi/Lo cap tables and rpmPoints (user-entered), or the sail front (no RPM axis).
// The cloud half is owed separately — NK_RpmAxisWipePend gates front sync until it lands.
void rpmAxisWipeExecute() {
  kneeLearnResetDefaults();       // Min% floor table + knee-learner state
  huntMapClearAll("engine speed recalibrated");  // pocket centers are rpm-axis values
  commissionClearMinPctBackup();  // its NVS backup snapshot
  commissionClearRpmDependents(); // every RPM-binned commissioning stage (all but Prep + Field cut)
  systemIDLogClearAll();          // step-test ring (records are RPM-stamped)
  resetMotorFrontOnly();          // motoring front only; sail front kept
  pendingResetAlternatorHealth = true;  // best-ever front + engine-hour trend + baseline
  pendingClearOverheatHistory  = true;  // per-RPM-bin thermal history
  faPendingMatrixClear = true;          // /famatrix.bin  (50-RPM x amp cells)
  ripTabPendingWipe    = true;          // /riptab.bin    (50-RPM bins)
  ripFitForget(true, true, "RPM scaling changed");   // both fits carry the RPM they were measured at, and came from the two tables above
  faPendingFlipWipeAll = true;          // /faflip.bin    (all 9 pages)
  rpmAxisWipePending = true;
  settingWrite(NK_RpmAxisWipePend, "1");
  settingWrite(NK_RpmAxisWipeLoc, "0");  // local half done; cloud half still gated by NK_RpmAxisWipePend
  queueConsoleMessage("RPM scaling changed -- engine-RPM-indexed learning wiped; re-commission required");
}

// Throttled saver — called from loop() (NOT the control tick). Flash commit is gated on field-off
// (like dumpLongTermRing) so the NVS write never stalls the control loop while the field is on.
// Learned state accrues to RAM during field-on float/low-demand and is persisted once the field
// drops; losing an in-progress session on a power cut is harmless (slow-learned, safe defaults).
void kneeLearnService(bool fieldOff) {
  static uint32_t lastSaveMs = 0;
  if (!kneeStateDirty || !fieldOff) return;
  uint32_t now = millis();
  if (lastSaveMs != 0 && (now - lastSaveMs) < 300000UL) return;  // <= every 5 min
  lastSaveMs = now;
  saveKneeLearnState();
}

// Reset learned state: every bin back to 0% and unlocked, so learning restarts from scratch.
// The live table is only zeroed while the learner owns it (learning on) — with learning off, a
// hand-entered table survives the knee-state wipe.
// Rebuild the visible Keep-Alive column from the learner's stored floors. THE one place that
// expresses "what does a bin hold while the learner owns the table" — kneeLearnInit, the
// kneeLearnEnable turn-on handler and kneeLearnResetDefaults all route through here so they cannot
// drift apart (they had, before: two of them produced a flat baseline and the third produced zeros).
//
// A bin the learner or commissioning has decided (kneeFrozen) is copied verbatim, INCLUDING a
// deliberate zero above the commissioned RPM ceiling — that zero is the 2026-06-23 safety invariant
// and must survive. An UNDECIDED bin sitting at zero gets KNEE_BASELINE_PCT instead: without it a
// fresh or freshly-wiped device carries an all-zero table, getMinimumFieldForRPM returns its
// hard-zero sentinel at every speed, governor_apply reads that as "no floor" and drops the MinDuty
// scalar too — so the user's Min Field % setting does nothing at any engine speed and nothing says so.
// The baseline is written back into kneeFloor, not just the column, or the learner's next probe
// restart (rpmMinDutyTable[b] = kneeFloor[b]) knocks it straight back to zero.
// Bin 0 keeps its permanent zero: it sits at 100 RPM, below MinRPMForField, so the field is cut
// there regardless and the learner never probes it.
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
  // Unfreezing every bin above makes them all "undecided", so this restores the flat baseline rather
  // than the zeros this used to leave behind. Learner off = the hand-entered column stands untouched.
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
  // Duty-domain knobs are stored in REAL duty-% for this bus. Their hardcoded globals are 12V values,
  // so on FIRST creation scale the default by ×(12/SYSTEM_VOLTAGE_CLASS) (5%→1.25% at 48V) before persisting.
  // Once a key exists the stored value wins verbatim (no scaling) — that's the WYSIWYG dashboard value.
#define KNEE_LD_DUTY(key, var) do { if (!settingExists(key)) { var = var * 12.0f / (float)SYSTEM_VOLTAGE_CLASS; settingWrite(key, String(var).c_str()); } else var = settingRead(key).toFloat(); } while (0)
  KNEE_LD_DUTY(NK_kneeMarginPct,   kneeMarginPct);
  KNEE_LD_F(NK_kneeOnsetA,      kneeOnsetA);
  KNEE_LD_F(NK_kneeReArmA,      kneeReArmA);
  KNEE_LD_DUTY(NK_kneeStepPct,     kneeStepPct);
  KNEE_LD_F(NK_kneeDwellSec,    kneeDwellSec);
  KNEE_LD_F(NK_kneeTempRefF,    kneeTempRefF);
  KNEE_LD_DUTY(NK_kneeMaxFloorPct, kneeMaxFloorPct);
  KNEE_LD_F(NK_kneeRpmTolPct,   kneeRpmTolPct);
  KNEE_LD_F(NK_kneeTempTolF,    kneeTempTolF);
  KNEE_LD_DUTY(NK_kneeDutyTolPct,  kneeDutyTolPct);   // duty-domain steadiness band — inverse-scale ×12/V like altDutyLineRms
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
    // Temp-neutral threshold for getMinimumFieldForRPM's live correction. Optional (absent on tables
    // built before this existed, or never commissioned) → fall back to 0 = scale the whole knee.
    sz = sizeof(kneeFitA);
    if (nvs_get_blob(h, "kneeFitA", &kneeFitA, &sz) != ESP_OK || sz != sizeof(kneeFitA)) kneeFitA = 0.0f;
    nvs_close(h);
  }
  if (!haveState)
    for (int i = 0; i < RPM_TABLE_SIZE; i++) { kneeFloor[i] = 0; kneeKnee[i] = 0; kneeFrozen[i] = false; kneeLearnTempF[i] = 0; }

  // While learning owns the table, the floor owns it from boot (bin 0 always 0%).
  // Learning off = hand-entered table stands. Also repairs a device already carrying an all-zero
  // table from before the baseline existed — undecided bins refill, measured ones are left alone.
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

/**
 * isVoltageSensorPlausible()
 * Voltage ranges (with safety buffer):
 * - 12V system: 4.5V to 15.5V
 * - 24V system: 9.0V to 32.5V   (upper allows a 24V AGM equalization ~31-32V)
 * - 36V system: 13.5V to 46.5V  (upper allows equalization, same 15.5V/12V-cell ceiling as the 12V class)
 * - 48V system: 18V to 60.5V
 *
 * @return true if at least one voltage sensor shows plausible reading
 */
bool isVoltageSensorPlausible() {
  float minPlausible, maxPlausible;

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

  // Check BatteryV sensor
  bool batteryVPlausible = (BatteryV >= minPlausible && BatteryV <= maxPlausible && !isnan(BatteryV));

  // Check IBV sensor
  bool ibvPlausible = (IBV >= minPlausible && IBV <= maxPlausible && !isnan(IBV));

  // Valid if at least one sensor is plausible
  // If BOTH are implausible, that's a critical fault
  return (batteryVPlausible || ibvPlausible);
}

// tempPID_init()
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
// tempFilterUpdate — IIR filter + slope estimator + projected-temp lookahead. Pure
// display/estimator math (updates tempFiltered, slope buffer, thermalSlopeFPerSec,
// projectedTempF; no PID/field/persisted state). Called every tick so the plot/log show
// real temperature during tuning. Skips on invalid temp so garbage never poisons the filter.
void tempFilterUpdate(uint32_t nowMs) {
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
      // intervals = sample steps back for the difference. Clamp to [2, BUF-1]:
      // min 2 (10s) keeps it from going single-step-noisy; max BUF-1 (12 = 60s) is
      // the legacy full-buffer span. The newest sample sits at (bufIdx-1).
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

  // Stage bounds from inBulkStage/inAbsorptionStage directly, NOT voltageControlActive:
  // that flag is reassigned AFTER this fn returns, so it is one stage-transition tick stale.
  const float capCurrent = getCapCurrentForRPM(RPM);
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

  // Suppress the warmup margin during commissioning/tuning: those toggle the field/TuningMode,
  // re-seeding the slope buffer, so the margin would step the setpoint and derate mid-test.
  // Hard trips (warning ramp, critical cut) are NOT gated by this. Commissioning = wizard dialog
  // open (heartbeat-fresh), not the bare state byte — a stopped run must not derate-exempt for days.
  const bool suppressWarmupMargin = cxOwnsBatteryNow() || (TuningMode != 0);

  if (!tempPIDActive) {

    // Preserve the slope buffer when re-enabling from a DORMANT-but-not-stale period (TuningMode
    // exit): the trend stayed live, so clearing it would force a blind warmup that derates right
    // after a test. The one-shot flag is the gate, NOT a freshness timestamp — on a true sensor
    // gap tempFilterUpdate pushes a sample spanning the gap the same tick, so a "time since last
    // push" test would look fresh while the buffer holds garbage. Default (stale/cold) clears.
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

  // lookaheadDeltaF / tempNowPid are kept only for the logging decomposition (outerTermLookahead/
  // outerTermP) and tempPIDInput_d telemetry — they no longer feed any library object.
  float lookaheadDeltaF;
  float tempNowPid;
  {
    tempNowPid = (TempSource == 0) ? TempToUse : tempFiltered;
    lookaheadDeltaF = fmaxf(0.0f, projectedTempF - fmaxf(tempNowPid, effectiveSetpoint));
    tempPIDInput_d = (double)tempNowPid;
  }

  // Approach gate: released the first time PRESENT temp (not the projection) reaches setpoint;
  // until then dI is held off (FF handles the approach). Winding a hold before there is one to
  // hold caused the ~2.5× overbuild + deep post-peak sag.
  if (!thermalIntegratorReleased && tempNowPid >= (activeTempLimit - 7.0f)) {
    thermalIntegratorReleased = true;
    queueConsoleMessageF("TempPID: integrator released — present temp %.1f°F reached setpoint", tempNowPid);
  }

  // HYBRID penalty: penalty = clamp(FF + thermalIntegral, 0, capCurrent). FF (P + projection) is
  // POSITIONAL feedforward; only the holding integral is accumulated. ePpos is floored so the
  // projection-led safety term never goes negative; eI is present-only so the integral never winds
  // on the projection.
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

  // Existing asymmetric slew, then final clamp to the SAME live cap.
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
// pidLog_tick()
// Call ONCE, at the END of the normal control path in AdjustFieldLearnMode(),
// after governor_apply() and all state updates. Captures a snapshot of the
// complete outer→inner→duty pipeline for that tick.
// NOT called during shutdown/fault paths — those are not the instability target.
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

  // ── Timestamp ─────────────────────────────────────────────────────────────
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
  // Inner output-current PID — the gains ACTUALLY handed to currentPID (setting × voltage-class norm,
  // Ki × the oscillation damper's live derate), same convention as voltageKp/Ki below. The configured
  // PidKp/Ki/Kd ride the CSV3 settings echo in the snapshot JSON saved beside every log, so the derate
  // is recoverable as innerKi ÷ (PidKi × 12/SYSTEM_VOLTAGE_CLASS).
  e.innerKp = PidKp_active;
  e.innerKi = PidKi_active * g_huntDerate;
  e.innerKd = PidKd_active;
  e.voltageKp = (float)VoltageKp_active;  // outer voltage loop — gain actually in effect (Manual or Auto α/K)
  e.voltageKi = (float)VoltageKi_active;

  e.battV_filt = IBV_filtered;
  e.dBcur_dt = g_dBcur_dt;
  e.battI = getBatteryCurrent();
  e.ch1IntervalMs = (int16_t)g_ch1LastIntervalMs;
  e.voltLoopIntervalMs = pidLog_voltageLoopRanThisTick ? (int16_t)g_voltLoopActualIntervalMs : 0;
  e.inaIntervalMs = (int16_t)ina_last_ms;
  e.pad2 = 0;
  e.mExcessEma = g_mExcessEma;             // iExcess detector traces — averaged excess vs its
  e.iExcessThreshold = g_iExcessThreshold; // computed fire threshold E (both A), for offline tuning

  pidLogHead = (pidLogHead + 1) % PID_LOG_SIZE;
  if (pidLogCount < PID_LOG_SIZE) pidLogCount++;
}

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
  // Both are NAN until the first sane temperature read, and the logger has no temp-validity gate, so
  // thermalLogScale10's 0 stamped a real 0.0F into EVERY row. INT16_MIN = blank cell.
  e.tempFiltered = !isfinite(tempFiltered) ? INT16_MIN : thermalLogScale10(tempFiltered);
  e.tempProjected = !isfinite(projectedTempF) ? INT16_MIN : thermalLogScale10(projectedTempF);
  // effective setpoint: mirrors the logic in tempPID_tick (slopeBufFull = 7°F margin, else 20°F warmup margin)
  {
    float logLimit = TemperatureLimitF;
    float logSetpoint = thermalSlopeBufFull ? (logLimit - 7.0f) : (logLimit - 20.0f);
    e.nominalTarget = thermalLogScale10(logSetpoint);
  }
  e.rpmCap = thermalLogScale10(getCapCurrentForRPM(RPM));
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
  if (shutdownPhase != SHUTDOWN_PHASE_NONE) e.flags |= (1 << 5);

  e.antiWindupFired = outerAntiWindupFired ? 1 : 0;
  e.chargeStageDisplay = thermalLogGetStageCode();
  e.freezeWhy = thermalFreezeReason;

  e.outerTermP = thermalLogScale10(outerTermP);
  e.outerTermI = thermalLogScale10(outerTermI);
  e.outerTermLookahead = thermalLogScale10(outerTermLookahead);
  e.impliedPenalty = thermalLogScale10(outerImpliedPenalty);
  e.thermalSlope = (int16_t)(thermalSlopeFPerSec * 1000.0f);
  e.penaltyRaw = thermalLogScale10(outerPenaltyRaw);
  // Carries the live integral ceiling cap−FF (CSV header: iCeil_A, not a hold estimate).
  // iCeil < outerI ⇒ the I≤cap−FF clamp is deleting earned holding integral
  // (RPM dip / FF spike) — the destructive-clamp signal to watch for a reheat on cap recovery.
  e.holdEstimate = thermalLogScale10(thermalIntegralCeil);

  thermalLogHead = (thermalLogHead + 1) % THERMAL_LOG_SIZE;
  if (thermalLogCount < THERMAL_LOG_SIZE) thermalLogCount++;
}
