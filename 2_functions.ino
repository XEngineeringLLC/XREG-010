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

String readFile(fs::FS &fs, const char *path) {
  if (!littleFSMounted && !ensureLittleFS()) {
    Serial.printf("Cannot read file - LittleFS not available: %s\n", path);
    return String();
  }

  if (!fsMutex) {
    Serial.printf("readFile: fsMutex NULL: %s\n", path);
    return String();
  }

  if (xSemaphoreTake(fsMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    Serial.printf("readFile: mutex timeout: %s\n", path);
    return String();
  }

  File file = fs.open(path, "r");
  if (!file || file.isDirectory()) {
    xSemaphoreGive(fsMutex);
    Serial.printf("readFile: open failed or is directory: %s\n", path);
    return String();
  }

  size_t size = file.size();
  if (size > 4096) {
    file.close();
    xSemaphoreGive(fsMutex);
    Serial.printf("readFile: file too large (%u): %s\n", (unsigned)size, path);
    return String();  // Callers can't distinguish "too large" from "not found" — both return String(); valid config JSON (8192-byte payload buffer) could exceed 4096.
  }

  char *buffer = (char *)ps_malloc(size + 1);
  if (!buffer) {
    file.close();
    xSemaphoreGive(fsMutex);
    Serial.println("readFile: allocation failed");
    return String();
  }

  size_t bytesRead = file.readBytes(buffer, size);
  buffer[bytesRead] = '\0';

  file.close();
  xSemaphoreGive(fsMutex);

  String result(buffer);
  free(buffer);
  return result;
}

bool writeFile(fs::FS &fs, const char *path, const char *message) {
  if (!littleFSMounted && !ensureLittleFS()) {
    return false;
  }

  if (!fsMutex || xSemaphoreTake(fsMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    Serial.printf("writeFile: mutex timeout: %s\n", path);
    return false;
  }

  File file = fs.open(path, "w");
  if (!file) {
    xSemaphoreGive(fsMutex);
    return false;
  }

  const size_t expected = strlen(message);
  const size_t written  = file.print(message);
  file.flush();
  file.close();

  // Truncated write — LittleFS full or other I/O failure. Delete the partial
  // file so the next boot reads "no setting" (and Pattern B re-initializes the
  // default) instead of "" → toFloat()→0.0 → silent zeroing of a critical setting.
  if (written != expected) {
    Serial.printf("writeFile: truncated write on %s (%u/%u bytes) — deleting\n",
                  path, (unsigned)written, (unsigned)expected);
    fs.remove(path);
    xSemaphoreGive(fsMutex);
    return false;
  }

  xSemaphoreGive(fsMutex);
  return true;
}
bool fsExists(const char *path) {
  if (!fsMutex || xSemaphoreTake(fsMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    Serial.println("fsExists: mutex timeout");
    return false;
  }
  bool result = LittleFS.exists(path);
  xSemaphoreGive(fsMutex);
  return result;
}

bool fsRemove(const char *path) {
  if (!fsMutex || xSemaphoreTake(fsMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    Serial.println("fsRemove: mutex timeout");
    return false;
  }
  bool result = LittleFS.remove(path);
  xSemaphoreGive(fsMutex);
  return result;
}

// ==== NVS settings layer ====
// Settings live in the NVS "settings" namespace as strings (not LittleFS).
// Rationale + full key map: Working Markdown Docs/NVS_SETTINGS_KEY_MAP.md.
// NVS keys cap at 15 chars — long names map to short NK_* keys below; the
// alt*/perf* registry tables (7_functions.ino) key off entry names truncated
// to 15 chars, so keep new registry names unique in their first 15 chars.
#define NK_AbsorptionTimeoutMs "AbsorptionTmtMs"
#define NK_AbsorptionVoltage "AbsorptionVoltg"
#define NK_AlarmActivate "AlarmActivate"
#define NK_AlarmLatchEnabled "AlarmLatchEnbld"
#define NK_AlternatorCOffset "AlternatrCOffst"
#define NK_AlternatorHardShutdownV "AltrntrHrdShtdw"
#define NK_AlternatorNominalAmps "AltrntrNmnlAmps"
#define NK_AmpSensorRange "AmpSensorRange"
#define NK_AutoAltCurrentZero "AutoAltCurrntZr"
#define NK_AutoShuntGainCorrection "AutShntGnCrrctn"
#define NK_AwBleedRate "AwBleedRate"
#define NK_AwSeedProtectMs "AwSeedProtectMs"
#define NK_BatteryCOffset "BatteryCOffset"
#define NK_BatteryCapacity_Ah "BatteryCapctyAh"
#define NK_BatteryCurrentSource "BatteryCrrntSrc"
#define NK_BatteryVoltage "BatteryVoltage"   // 12/24/36/48 nominal bank class
#define NK_Beta "Beta"
#define NK_BulkVoltage "BulkVoltage"
#define NK_CAPSIZE_THRESHOLD_DEG "CAPSIZETHRESHOL"
#define NK_CVTuningMode "CVTuningMode"
#define NK_ChargeEfficiency "ChargeEfficincy"
#define NK_ChargedDetectionTime "ChargedDetctnTm"
#define NK_ChargedVoltage "ChargedVoltage"
#define NK_CloudFeatures "CloudFeatures"
#define NK_CurrentAlarmHigh "CurrentAlarmHgh"
#define NK_CurrentThreshold "CurrentThreshld"
#define NK_DutyRampRate "DutyRampRate"
#define NK_DutySlowRampRate "DutySlowRampRat"
#define NK_DvdtTC "DvdtTC"
#define NK_FIELD_COLLAPSE_DELAY "FIELDCOLLAPSEDE"
#define NK_FLOAT_DURATION "FLOAT_DURATION"
#define NK_FastSetpointRiseHeadroomV "FstStpntRsHdrmV"
#define NK_FastSetpointRiseRate "FastSetpontRsRt"
#define NK_FastSetpointRiseWindowMs "FstStpntRsWndwM"
// RETIRED (2026-08-20, unreferenced): kept so the 15-char NVS key is never handed to a new setting.
// Stale values may still sit in NVS on units flashed before this; nothing reads them.
// #define NK_FieldAdjustmentInterval "FldAdjstmntIntr"
#define NK_FieldResistance "FieldResistance"
#define NK_FloatVoltage "FloatVoltage"
#define NK_FuelEfficiency "FuelEfficiency"
#define NK_HardOCDebounceMs "HardOCDebouncMs"
#define NK_HardOCEnable "HardOCEnable"
#define NK_HiLow "HiLow"
#define NK_IExcessArmMarginV "IExcessArmMrgnV"
#define NK_IExcessCeilA "IExcessCeilA"
#define NK_IExcessEnable "IExcessEnable"
#define NK_IExcessFloorA "IExcessFloorA"
#define NK_BattCurrentLimitA "BattCurLimA"
#define NK_BattLimitEnable "BattLimitEnable"
#define NK_IExcessFrac "IExcessFrac"
#define NK_IExcessFracBulk "IExcessFrcBlk"
#define NK_IExcessBaseA "IExcessBaseA"
#define NK_IExcessCcOffsetA "IExcessCcOffA"
#define NK_IExcessKBleed "IExcessKBleed"
#define NK_IExcessRelFrac "IExcessRelFrac"
#define NK_IExcessTau "IExcessTau"
#define NK_IgnitionOverride "IgnitionOverrid"
#define NK_IgnoreLearningDuringPenalty "IgnrLrnngDrngPn"
#define NK_IgnoreRPM "IgnoreRPM"
#define NK_IgnoreTemperature "IgnoreTemperatr"
#define NK_InstallId       "InstallId"       // random device identity; survives reflash, regenerated only when NVS is erased
#define NK_InvertAltAmps "InvertAltAmps"
#define NK_InvertBattAmps "InvertBattAmps"
#define NK_BatteryShuntPresent "BattShuntPresnt"
#define NK_KHard "KHard"
#define NK_LastResetReason "LastResetReason"
#define NK_LatitudeManual "LatitudeManual"
#define NK_LatitudeNMEA "LatitudeNMEA"
#define NK_LearningDownStep "LearningDownStp"
#define NK_LearningMemoryDuration "LearnngMmryDrtn"
#define NK_LearningRPMChangeThreshold "LrnngRPMChngThr"
#define NK_LearningSettlingPeriod "LernngSttlngPrd"
#define NK_LearningTempHysteresis "LrnngTmpHystrss"
#define NK_LearningUpStep "LearningUpStep"
#define NK_LimpHome "LimpHome"
#define NK_LoadDumpDtThresh "LoadDumpDtThrsh"
#define NK_LoadDumpDtThresh1 "LoadDmpDtThrsh1"
#define NK_LoadDumpDtThresh3 "LoadDmpDtThrsh3"
#define NK_LoadDumpEnable "LoadDumpEnable"
#define NK_LoadDumpN1 "LoadDumpN1"
#define NK_LoadDumpN2 "LoadDumpN2"
#define NK_LoadDumpN3 "LoadDumpN3"
#define NK_LogAllLearningEvents "LgAllLrnngEvnts"
#define NK_LongitudeManual "LongitudeManual"
#define NK_LongitudeNMEA "LongitudeNMEA"
#define NK_MaintainMode "MaintainMode"
#define NK_ManualDutyTarget "ManualDutyTargt"
#define NK_ManualFieldToggle "ManualFieldTggl"
#define NK_ManualLifePercentage "ManualLifPrcntg"
#define NK_ManualSOCPoint "ManualSOCPoint"
#define NK_MaxDuty "MaxDuty"
#define NK_MaxFieldVolts "MaxFieldVolts"
#define NK_MaxPenaltyDuration "MaxPenaltyDurtn"
#define NK_MaxPenaltyPercent "MaxPenaltyPrcnt"
#define NK_MaxTableValue "MaxTableValue"
#define NK_MaximumAllowedBatteryAmps "MxmmAllwdBttryA"
#define NK_MinDuty "MinDuty"
#define NK_MinFloatTime "MinFloatTime"
#define NK_MinLearningInterval "MinLernngIntrvl"
#define NK_MinRPMForField "MinRPMForField"
#define NK_NMEA0183Baud "NMEA0183Baud"
#define NK_NMEA0183Data "NMEA0183Data"
#define NK_NMEA0183Invert "NMEA0183Invert"
#define NK_NMEA2KData "NMEA2KData"
#define NK_NeighborLearningFactor "NeghbrLrnngFctr"
#define NK_OnOff "OnOff"
#define NK_OutputPIDFilterTC "OutputPIDFltrTC"
#define NK_OutputPIDMA_N "OutputPIDMA_N"
#define NK_OutputPIDSigSrc "OutputPIDSigSrc"
#define NK_OvGroup1Enable "OvGroup1Enable"
#define NK_OvGroup2Enable "OvGroup2Enable"
#define NK_OvMeasMarginV "OvMeasMarginV"
#define NK_OvPredMarginV "OvPredMarginV"
#define NK_OvTierLoMarginV "OvTierLoMargV"
#define NK_OvTierLoDwellMs "OvTierLoMs"
#define NK_OvTierMidMarginV "OvTierMidMargV"
#define NK_OvTierMidDwellMs "OvTierMidMs"
#define NK_PIDTrackingGain "PIDTrackingGain"
#define NK_TachLieEnable "TachLieEnable"
#define NK_PITCHPOLE_THRESHOLD_DEG "PITCHPOLETHRESH"
#define NK_PeukertExponent "PeukertExponent"
#define NK_PidKd "PidKd"
#define NK_PidKi "PidKi"
#define NK_PidKp "PidKp"
#define NK_PidSampleDivisor "PidSampleDivisr"
#define NK_PulleyRatio "PulleyRatio"
#define NK_RPMScalingFactor "RPMScalingFactr"
#define NK_RpmAxisWipePend "RpmAxWipePend"
#define NK_RpmAxisWipeLoc "RpmAxWipeLoc"
#define NK_R_fixed "R_fixed"
#define NK_RebulkCurrent_A "RebulkCurrent_A"
#define NK_RebulkVoltage "RebulkVoltage"
#define NK_ReseedFrac "ReseedFrac"
#define NK_ReseedFracNS "ReseedFracNS"
#define NK_CvRecovClimb "CvRecovClimb"
#define NK_SLAM_THRESHOLD_G "SLAMTHRESHOLDG"
#define NK_SOC_AllowRebulk_percent "SOCAllwRblkprcn"
#define NK_SOC_BlockRebulk_percent "SOCBlckRblkprcn"
#define NK_SafeOperationThreshold "SafOprtnThrshld"
#define NK_SocAlarmLow "SocAlarmLow"
#define NK_SocSeedAck "SocSeedAck"
#define NK_SocSeedSnap "SocSeedSnap"
#define NK_SetpointFallRate "SetpointFallRat"
#define NK_CvBrakeFallRate "CvBrakeFallRate"
#define NK_SetpointRiseRate "SetpointRiseRat"
#define NK_SetpointBigStepThresh "SetpntBigStpTh"
#define NK_SetpointBigStepRiseRate "SetpntBigStpRt"
#define NK_SettleTimeBeforeCut "SettleTimeBfrCt"
#define NK_ShuntResistanceMicroOhm "ShntRsstncMcrOh"
#define NK_ShutdownPhase2HoldMs "ShtdwnPhs2HldMs"
#define NK_cvHelpersEnabled "cvHelpersEn"
#define NK_cvGainMode "cvGainMode"
#define NK_cvCrossover "cvCrossover"
#define NK_cvPiZero "cvPiZero"
#define NK_cvAlpha "cvAlpha"
#define NK_vTgtRampEnable "vTgtRampEn"
#define NK_vTgtRampUp "vTgtRampUp"
#define NK_vTgtRampDn "vTgtRampDn"
#define NK_cvWindDownEn "cvWindDownEn"
#define NK_cvWindDownRate "cvWindDownRate"
#define NK_cvWindDownStopV "cvWindDownStopV"
#define NK_setpointSlewEnable "setptSlewEn"
#define NK_cvRiseGovEnable "cvRiseGovEn"
#define NK_cvRecovEnable "cvRecovEn"
#define NK_cvRecovSec "cvRecovSec"       // retired timed-window knob — key kept (never repurpose)
#define NK_cvRecovEmaxV "cvRecovEmaxV"   // retired timed-window knob — key kept (never repurpose)
#define NK_cvRecovKiMax "cvRecovKiMax"
#define NK_cvRecovBoostEnable "cvRcvBoostEn"
#define NK_cvRecovBoostMax "cvRcvBoostMax"
#define NK_cvRecovBoostErrV "cvRcvBoostErrV"
#define NK_cvRecovBoostFloorV "cvRcvBoostFlrV"
#define NK_cvRecovDeepBandV "cvRcvDeepBandV"
#define NK_cvRecovDeepMult "cvRcvDeepMult"
#define NK_cvRecovFlareBandV "cvRcvFlareBndV"
#define NK_cvRecovFlareFrac "cvRcvFlareFrac"
#define NK_loadServeBoostEnable "loadServeBoost"
#define NK_HuntGovEnable "HuntGovEn"
#define NK_HuntCutPct "HuntCutPct"
#define NK_HuntVerifyPct "HuntVerifyPct"
#define NK_HuntWingPct "HuntWingPct"
#define NK_HuntCooldownMin "HuntCooldwnMin"
#define NK_HuntSteadyPct "HuntSteadyPct"
#define NK_HuntQualifyScans "HuntQualScans"
#define NK_HuntTrigPct "HuntTrigPct"
// Accepted ranges for the oscillation-damper (hunt-governor) knobs. Single source of truth for the
// /settings handlers AND the NVS load path — they used to carry the same numbers separately, and the
// load path silently lost them (HuntCutPct = 0 from a garbage NVS string zeroes inner Ki in every
// learned pocket). The /settings handlers in 3_functions.ino constrain() against these macros.
#define HUNT_GOV_ENABLE_MIN     0
#define HUNT_GOV_ENABLE_MAX     1
#define HUNT_CUT_PCT_MIN       25
#define HUNT_CUT_PCT_MAX       80
#define HUNT_VERIFY_PCT_MIN     5
#define HUNT_VERIFY_PCT_MAX    60
#define HUNT_WING_PCT_MIN       5
#define HUNT_WING_PCT_MAX      40
#define HUNT_COOLDOWN_MIN_MIN   1
#define HUNT_COOLDOWN_MIN_MAX  60
#define HUNT_STEADY_PCT_MIN     1
#define HUNT_STEADY_PCT_MAX    10
#define HUNT_QUALIFY_SCANS_MIN  2
#define HUNT_QUALIFY_SCANS_MAX  6
// No upper bound: a 48 V install sees a quarter the duty swing for the same wobble, so the useful
// bar is a property of the install, not a range we can pick. The floor only keeps 0 (= every scan
// detects) out of the detector.
#define HUNT_TRIG_PCT_MIN   0.01f
#define NK_reseedCorrEnable "reseedCorrEn"
#define NK_dutySlewEnable "dutySlewEn"
#define NK_testSlewMode "testSlewMode"
#define NK_cvTestSlewMode "cvTestSlewMode"
#define NK_cvPlantKa "cvPlantKa"
#define NK_cvPlantKb "cvPlantKb"
// Measured ripple projection (§3.3) — one CSV-encoded string: "a0,a1,rpm,i0,i1,i2,pk0,pk1,pk2,n"
#define NK_ripFitAlt  "ripFitAlt"
// Measured voltage-slope projection (CV D-term deadband) — same encoding, slope points in V/s
#define NK_slpFitAlt  "slpFitAlt"
// Measured-ripple capture admission gates (§10.8/§11) — own knobs, deliberately DECOUPLED from the
// fa* anomaly-detector gates so tuning capture admission can't loosen detector arming.
// NK "ripRpmMargin" is RETIRED — key abandoned in NVS, never reuse it for a different meaning.
#define NK_ripWinMs "ripWinMs"
#define NK_ripDriftFloorA "ripDriftFloorA"
#define NK_ripDriftPct "ripDriftPct"
// RETIRED NVS keys — never reuse these key strings for a new setting (old devices still hold
// stored values under them): "cvPlantTau", "cvPlantL" (removed 2026-07-03; τ/L fit retired);
// "cvPlantK" (2026-07-08 — the single-point gain became the derived Ka + Kb·√t curve).
#define NK_CommissionTempF "CommissionTmpF"
#define NK_CommissionEpoch "CommissionEpch"
#define NK_cmAgeAck "cmAgeAck"
#define NK_cmChangeFlag "cmChangeFlag"
#define NK_battMaxMode "battMaxMode"   // battery V/I plot sampling: 0=window mean, 1=max-magnitude
#define NK_battTempDerateEn "battTmpDerEn"
#define NK_battTempCoeff "battTmpCoeff"
// CV D term (2026-07-17). The four keys "SlopeBleedK"/"CvBrakeThrVps"/"CvBrakeTauMs"/"CvBrakeArmV"
// are RETIRED orphans — the 2026-07-15 flashed device holds brake-semantics values under them.
// NEVER reuse those strings: a fielded SlopeBleedK=50 read as VoltageKd would be a wrong gain.
#define NK_VoltageKd "VoltageKd"
#define NK_CvKdDeadbandVps "CvKdDeadband"
#define NK_CvKdOneSided "CvKdOneSided"
#define NK_CvKdArmV "CvKdArmV"
#define NK_CvKdMaxTrimA "CvKdMaxTrimA"
#define NK_CvKdVoltFiltTC "CvKdVoltFiltTC"
// Deadband line: deadband = clamp(CvKdDbFloor, CvKdDeadbandVps + CvKdDbSlope·I, CvKdDbCeil)
#define NK_CvKdDbSlope "CvKdDbSlope"
#define NK_CvKdDbFloor "CvKdDbFloor"
#define NK_CvKdDbCeil "CvKdDbCeil"
#define NK_CvKdExcessMode "CvKdExcessMode"
#define NK_CvKdSlopeCeil "CvKdSlopeCeil"
#define NK_CvStressDropV "CvStressDropV"
#define NK_CvStressFailBandV "CvStressFailBnd"
#define NK_CvKdTd "CvKdTd"
#define NK_SolarWatts "SolarWatts"
#define NK_StartupRiseRate "StartupRiseRate"
#define NK_PhysicalPanelOverride "PhysPanelOvrrd"   // renamed from the dead placeholder "SwtchCntrlOvrrd"; that string is retired, never reuse it
#define NK_SwitchingFrequency "SwitchingFrqncy"
#define NK_SystemIDStepAmplitude "SystmIDStpAmplt"
#define NK_systemIDTestType "SysIDTestType"
#define NK_systemIDSineFreqStart "SysIDSineFStrt"
#define NK_systemIDSineFreqEnd "SysIDSineFEnd"
#define NK_sysidPlantTau "sysidPlantTau"
#define NK_fieldDecayTau "fieldDecayTau"       // commissioned field drain time command→10% (ms) — worst-case (longest) endpoint of the drain-vs-RPM line, or the flat value; key name is historical
#define NK_fdDrainLoMs "fdDrainLoMs"           // drain-vs-RPM line: drain (ms) at NK_fdDrainRpmLo (wizard-fitted, shifted to sit at/above every measured point)
#define NK_fdDrainHiMs "fdDrainHiMs"           // drain-vs-RPM line: drain (ms) at NK_fdDrainRpmHi
#define NK_fdDrainRpmLo "fdDrainRpmLo"         // drain-vs-RPM line: lowest tested RPM; lookup clamps here, never extrapolates
#define NK_fdDrainRpmHi "fdDrainRpmHi"         // drain-vs-RPM line: highest tested RPM; rpmHi<=rpmLo or a zero endpoint = no line (flat)
#define NK_faCalGain "faCalGain"               // fast alt-current channel amps calibration: gain vs ADS1115 (field-decay test writes it)
#define NK_faCalOffA "faCalOffA"               // fast alt-current channel amps calibration: offset (A)
#define NK_systemIDSineCycles "SysIDSineCyc"
#define NK_SystemIDStabilizeAmps "SysIDStabAmps"
#define NK_commissionState "commissnState"   // 0=not / 1=in-progress / 2=commissioned
#define NK_commissionPhase "commissnPhase"    // current wizard phase (0=Prep…8=Stress test, 9=finished); moves backward on Back
#define NK_commissionDoneMask "commissnDoneMsk" // per-stage completion bitmask (bit i = stage i done); 15-char max
#define NK_commissionManualMask "commissnManMsk" // per-stage set-by-hand bitmask (skip / mark-done-manually)
#define NK_commissionSnap "commissnSnap"      // Phase-0 origin snapshot (explicit-abort full revert): positional CSV of the settings the flow writes
#define NK_commissionStepSnap "commissnStepSnp" // in-flight step snapshot: scalars as of the current step's entry — reboot undoes only that step
#define NK_commissionPreRun "commissnPreRun"    // "state,phase,doneMask,manualMask" as of a fresh Start — Abort restores it so a device commissioned before the run stays commissioned
#define NK_cxLedgerSeq "cxLedgerSeq"          // commissioning-ledger monotonic event counter (persisted at flush; cloud dedupes on device_uid+seq)
#define NK_cvStressLast "cvStressLast"        // last CV stress-test result (positional CSV, ver-prefixed); diagnostic record, not a tune input
#define NK_T0_C "T0_C"
#define NK_TailCurrent "TailCurrent"
#define NK_TailCurrent_A "TailCurrent_A"
#define NK_TargetVoltageMode "TargetVoltageMd"
#define NK_TargetVoltageSetpoint "TargetVltgStpnt"
#define NK_TdPred "TdPred"
#define NK_TempAlarm "TempAlarm"
#define NK_TempAlarmLow "TempAlarmLow"
#define NK_TempCritExcess "TempCritExcess"
#define NK_TempPIDFilterAlpha "TempPIDFltrAlph"
#define NK_TempPIDIntervalMs "TempPIDIntrvlMs"
#define NK_TempPIDKi "TempPIDKi"
#define NK_TempPIDKiDownFrac "TmpKiDownFrac"
#define NK_TempPIDKp "TempPIDKp"
#define NK_TempSource "TempSource"
#define NK_TempSustainedTimeout "TempSustaindTmt"
#define NK_TempWarnExcess "TempWarnExcess"
#define NK_TemperatureLimitF "TemperatureLmtF"
#define NK_coldChargeLockoutEnable "coldChrgLock"
#define NK_MinChargeTempF "MinChargeTempF"
#define NK_ThermalLookaheadSec "ThermalLookhdSc"
#define NK_ThermalSlopeWindowSec "ThrmSlopeWinS"
#define NK_TuningMode "TuningMode"
#define NK_UVThresholdHigh "UVThresholdHigh"
#define NK_UseFloat "UseFloat"
#define NK_VeData "VeData"
#define NK_VMGTargetBearing "VMGTargetBrg"
#define NK_VoltageAlarmHigh "VoltageAlarmHgh"
#define NK_VoltageAlarmLow "VoltageAlarmLow"
#define NK_VoltageHardwareLimit "VoltHwLimitV"
#define NK_VoltageDisagreeThreshold "VltgDsgrThrshld"
#define NK_VoltageDisagreeTimeout "VoltageDisgrTmt"
#define NK_VoltageFilterTC "VoltageFilterTC"
#define NK_VoltageKi "VoltageKi"
#define NK_VoltageKp "VoltageKp"
#define NK_VoltageLoopInterval "VoltageLpIntrvl"
#define NK_WarmupRampRate "WarmupRampRate"
#define NK_WeatherTimeoutMs "WeatherTimeotMs"
#define NK_WeatherUpdateInterval "WethrUpdtIntrvl"
#define NK_WindingTempOffset "WindingTmpOffst"
#define NK_Ymax1 "Ymax1"
#define NK_Ymax2 "Ymax2"
#define NK_Ymax3 "Ymax3"
#define NK_Ymax4 "Ymax4"
#define NK_Ymin1 "Ymin1"
#define NK_Ymin2 "Ymin2"
#define NK_Ymin3 "Ymin3"
#define NK_Ymin4 "Ymin4"
#define NK_absorptionCompleteTime "absorptnCmpltTm"
#define NK_altPaused "altPaused"
#define NK_altbaseSec "altbaseSec"
#define NK_altRefSrc "altRefSrc"
#define NK_bmsLogic "bmsLogic"
#define NK_bmsLogicLevelOff "bmsLogicLevlOff"
#define NK_bulkVoltageHoldMs "bulkVoltagHldMs"
#define NK_capLimitMode "capLimitMode"
#define NK_cvConsecutiveReads "cvConsecutivRds"
#define NK_cvKOvershoot "cvKOvershoot"
#define NK_cvWaveAmplitudeV "cvWaveAmplitudV"
#define NK_cvWavePeriodSec "cvWavePeriodSec"
#define NK_displayTempUnit "displayTempUnit"
#define NK_displayVolUnit "displayVolUnit"
#define NK_gpsManualActive "gpsManualActive"
#define NK_timeSourceMode "gpsTimeSourceMd"   // legacy key string: predates the 2026-08-24 position/time split
#define NK_gpsPositionSource "gpsPosSource"
#define NK_speedSourceMode "speedSourceMode"
#define NK_hardwarePresent "hardwarePresent"
#define NK_maxPoints "maxPoints"
#define NK_perfPaused "perfPaused"
#define NK_performanceRatio "performanceRati"
#define NK_solarLearnEnable "solarLearnEnbl"
#define NK_solarUseConsEnable "solarUseConsEnb"
#define NK_solarConsMarginPct "solarConsMargin"
#define NK_solarLearnRatePct "solarLearnRate"
#define NK_plotTimeWindow "plotTimeWindow"
#define NK_rebulkDebounceTime "rebulkDebouncTm"
#define NK_socInfoAvailable "socInfoAvailabl"
#define NK_timeAxisModeChanging "timeAxsMdChngng"
#define NK_totalPowerCycles "totalPowerCycls"
#define NK_waveAmplitude "waveAmplitude"
#define NK_tuningWaveFloor "tuningWaveFloor"
#define NK_wavePeriod "wavePeriod"
#define NK_tuningWaveform "tuningWaveform"
#define NK_tuningSineFreq "tuningSineFreq"
#define NK_tuningSweepStart "tuningSwpStrt"
#define NK_tuningSweepEnd "tuningSwpEnd"
#define NK_tuningSweepCycles "tuningSwpCyc"
// Battery Health (DCIR test config + persisted blobs). Keys ≤15 chars.
#define NK_bhStepLowA "bhStepLowA"
#define NK_bhStepDeltaA "bhStepDeltaA"
#define NK_bhDwellMs "bhDwellMs"
#define NK_bhNumEdges "bhNumEdges"
#define NK_bhBaseline "bhBaselineAh"
#define NK_bhResults "bhResultsBlob"
#define NK_bhCapBlob "bhCapBlob"
// Capacity tracker (OCV-anchored) config. Keys ≤15 chars.
#define NK_capOcvBlob "capOcvBlob"
#define NK_capRestFrac "capRestFrac"
#define NK_capRestFloor "capRestFloorMin"
#define NK_capSettleRate "capSettleRate"
#define NK_capSocLowMax "capSocLowMax"
#define NK_capMinSpan "capMinSpan"
#define NK_capFullSoc "capFullSoc"
#define NK_capRefMode "capRefMode"
#define NK_capTempNorm "capTempNorm"
#define NK_capTempCoeff "capTempCoeff"
#define NK_capTempRef "capTempRef"
#define NK_weatherDataValid "weatherDataVald"
#define NK_weatherModeEnabled "weatherModEnbld"
#define NK_webgaugesinterval "webgaugesintrvl"
#define NK_wifiNapEnabled "wifiNapEnabled"
#define NK_xTime "xTime"
#define NK_yyMax "yyMax"
#define NK_yyMin "yyMin"
// WiFi provisioning + IMU level calibration
// Retired admin-password keys "password"/"passwordHash" (arm-gate replaced the password,
// 2026-07) may still hold orphaned values in fielded NVS — never repurpose those key strings.
#define NK_ssid "ssid"
#define NK_pass "pass"
#define NK_apssid "apssid"
#define NK_appass "appass"
#define NK_first_config_done "firstconfigdone"
#define NK_imu_zero "imu_zero"
#define NK_imu_mnt_state "imu_mnt_state"

// Fast alt-current diagnostic knobs (Pattern B). Value strings ≤15 chars (NVS key limit).
#define NK_faEnabled "faEnabled"
#define NK_faAlarmEnable "faAlarmEnable"
#define NK_faAnomPause "faAnomPause"
#define NK_faRpmEdgeMargin "faRpmEdgeMrgn"
#define NK_faAmpsDriftFloorA "faAmpDriftFlrA"
#define NK_faAmpsDriftPct "faAmpDriftPct"
#define NK_faAttenUpAmps "faAttenUpA"
#define NK_faAttenDownAmps "faAttenDnA"
#define NK_faPeakMinA "faPeakMinA"
// Auto Min% learning ("knee tracker") knobs (keys <= 15 chars)
// NK "minPctSystem" is RETIRED (master-toggle experiment, 2026-07-19 only) — key abandoned in NVS, never reuse it.
#define NK_kneeLearnEnable "kneeLearnEn"
#define NK_kneeMarginPct   "kneeMarginPct"
#define NK_kneeOnsetA      "kneeOnsetA"
#define NK_kneeReArmA      "kneeReArmA"
#define NK_kneeStepPct     "kneeStepPct"
#define NK_kneeDwellSec    "kneeDwellSec"
#define NK_kneeTempRefF    "kneeTempRefF"
#define NK_kneeTempComp    "kneeTempComp"
#define NK_kneeMaxFloorPct "kneeMaxFloorPc"
#define NK_kneeRpmTolPct   "kneeRpmTolPct"
#define NK_kneeTempTolF    "kneeTempTolF"
#define NK_kneeDutyTolPct  "kneeDutyTolPct"
#define NK_ZeroLogEnable   "ZeroLogEnable"   // Zero-drift characterization log master toggle
#define NK_lastAppldCfgId  "lastAppldCfgId"  // id of the last admin-pushed config applied (reboot-loop guard)
#define NK_cfgPushNotify   "cfgPushNotify"   // "<n>|<key,key,...>" receipt written by an applied config push, read back one boot later for the dashboard popup, cleared by its ack
#define NK_cfgSchema       "cfgSchema"       // persisted settings-schema version (runSettingsMigrations)

// Vessel Info. Was /vessel_info.json on LittleFS; moved into NVS so the boat's identity,
// geometry and chemistry survive a formatOnFail and ride the manifest export like every
// other setting. The JSON is now a derived view (vesselInfoJson()), not the store.
// NK_vesselSaved is the "user has completed Vessel Info" sentinel — tier 3, so an imported
// config can never assert it on a device that never filled the form.
#define NK_boatLenFt       "boatLenFt"
#define NK_boatDispLbs     "boatDispLbs"
#define NK_boatType        "boatType"
#define NK_boatMakeModel   "boatMakeModel"
#define NK_boatYear        "boatYear"
#define NK_homePort        "homePort"
#define NK_engineMake      "engineMake"
#define NK_engineHp        "engineHp"
#define NK_batteryType     "batteryType"
#define NK_battMakeModel   "battMakeModel"
#define NK_altBrandModel   "altBrandModel"
#define NK_imuMountOrient  "imuMountOrient"
#define NK_regMountLoc     "regMountLoc"
#define NK_imuDistBowFt    "imuDistBowFt"
#define NK_imuDistClFt     "imuDistClFt"
#define NK_imuHtWlFt       "imuHtWlFt"
#define NK_vesselSaved     "vesselSaved"
// NMEA2000 transmit (producer)
#define NK_n2kTxEn         "n2kTxEn"
#define NK_n2kDevInst      "n2kDevInst"
#define NK_n2kBattEn       "n2kBattEn"
#define NK_n2kBattInst     "n2kBattInst"
#define NK_n2kBattCfgEn    "n2kBattCfgEn"
#define NK_n2kAltEn        "n2kAltEn"
#define NK_n2kAltInst      "n2kAltInst"
#define NK_n2kAltTempEn    "n2kAltTempEn"
#define NK_n2kTempInst     "n2kTempInst"
#define NK_n2kTempSrc      "n2kTempSrc"
#define NK_n2kChgrEn       "n2kChgrEn"
#define NK_n2kChgrInst     "n2kChgrInst"
#define NK_n2kChgrCfgEn    "n2kChgrCfgEn"
#define NK_n2kChgrMode     "n2kChgrMode"
#define NK_n2kEngRpmEn     "n2kEngRpmEn"
#define NK_n2kEngInst      "n2kEngInst"
#define NK_n2kEngDynEn     "n2kEngDynEn"
#define NK_n2kEngBitsEn    "n2kEngBitsEn"
#define NK_n2kSrcAddr      "n2kSrcAddr"
#define NK_rvcTxEn         "rvcTxEn"
#define NK_rvcChgrEn       "rvcChgrEn"
#define NK_rvcDcEn         "rvcDcEn"
#define NK_rvcFaultEn      "rvcFaultEn"
#define NK_rvcChgrInst     "rvcChgrInst"
#define NK_rvcDcInst       "rvcDcInst"
#define NK_rvcDevPri       "rvcDevPri"
#define NK_n2kRxBattInst   "n2kRxBattInst"
// DVCC-style charge-limit follow (CVL/CCL)
#define NK_dvccEn          "dvccEn"
#define NK_dvccSrcType     "dvccSrcType"
#define NK_dvccInst        "dvccInst"
#define NK_dvccSilenceS    "dvccSilenceS"
#define NK_dvccSettleS     "dvccSettleS"
#define NK_dvccCvlMin      "dvccCvlMin"
#define NK_dvccCvlMax      "dvccCvlMax"
// Battery + extra temperature probes, battery-temperature source chain, hot-charge lockout
// (BATTERY_TEMP_SENSORS_SPEC.md §6). owAddr* hold a 16-char lowercase hex ROM code, "" = unassigned.
#define NK_battTempProbeEnable    "battTmpPrbEn"
#define NK_extraTempProbeEnable   "extraTmpPrbEn"
#define NK_battTempSource         "battTmpSrc"
#define NK_battTempProxyEnable    "battTmpProxyEn"
#define NK_hotChargeLockoutEnable "hotChrgLock"
#define NK_MaxChargeTempF         "MaxChargeTempF"
#define NK_extraTempAlarmHiEnable "xTmpAlmHiEn"
#define NK_extraTempAlarmHiF      "xTmpAlmHiF"
#define NK_extraTempAlarmLoEnable "xTmpAlmLoEn"
#define NK_extraTempAlarmLoF      "xTmpAlmLoF"
#define NK_n2kExtraTempEnable     "n2kXTmpEn"
#define NK_n2kExtraTempInstance   "n2kXTmpInst"
#define NK_n2kExtraTempSource     "n2kXTmpSrc"
#define NK_CommissionTempSrc      "CommissionTmpS"
#define NK_owAddrAlt              "owAddrAlt"
#define NK_owAddrBatt             "owAddrBatt"
#define NK_owAddrExtra            "owAddrExtra"
// Plausible-CVL window bounds, absolute volts and deliberately class-agnostic so one pair covers
// 12/24/36/48 V. Same numbers the /settings handlers in 3_functions.ino constrain to. The window
// must also stay ordered, low < high: an inverted or
// degenerate window latches DVCC follow to UNTRUSTED against every healthy BMS and "Reset Trust"
// re-latches within a tick, so it is unrecoverable from the UI.
#define DVCC_CVL_ABS_MIN 1.0f
#define DVCC_CVL_ABS_MAX 70.0f

#define SETTINGS_NVS_NAMESPACE "settings"

bool settingExists(const char *key) {
  nvs_handle_t h;
  if (nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
  size_t len = 0;
  bool found = (nvs_get_str(h, key, NULL, &len) == ESP_OK);
  nvs_close(h);
  return found;
}

String settingRead(const char *key) {
  nvs_handle_t h;
  if (nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return String();
  String out;
  size_t len = 0;
  if (nvs_get_str(h, key, NULL, &len) == ESP_OK && len > 0 && len < 4096) {
    char *buf = (char *)malloc(len);
    if (buf) {
      if (nvs_get_str(h, key, buf, &len) == ESP_OK) out = String(buf);
      free(buf);
    }
  }
  nvs_close(h);
  return out;
}

bool settingWrite(const char *key, const char *value) {
  if (!key || !value) return false;
  nvs_handle_t h;
  if (nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
  // compare-first: unchanged value -> no flash write (wear + entry churn)
  char cur[128];
  size_t len = sizeof(cur);
  if (nvs_get_str(h, key, cur, &len) == ESP_OK && strcmp(cur, value) == 0) {
    nvs_close(h);
    return true;
  }
  esp_err_t e = nvs_set_str(h, key, value);
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);
  return e == ESP_OK;
}

bool settingRemove(const char *key) {
  nvs_handle_t h;
  if (nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
  esp_err_t e = nvs_erase_key(h, key);
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);
  return e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND;  // already-absent counts as removed
}

// Range guard for the NVS load path (InitSystemSettings). The /settings handlers constrain what they
// accept, but NVS is also written raw by config import and the cloud config push — both of which only
// take effect through a reboot, so this load-side clamp is the one chokepoint that covers every writer.
// A missing or corrupt NVS string parses to 0, which is inside no useful range. Writes the corrected
// value back so the corruption cannot re-surprise on the next boot.
long clampLoadedSetting(const char *name, const char *nvsKey, long value, long lo, long hi) {
  long clamped = (value < lo) ? lo : ((value > hi) ? hi : value);
  if (clamped != value) {
    queueConsoleMessageF("Stored setting out of range - %s was %ld, corrected to %ld", name, value, clamped);
    settingWrite(nvsKey, String(clamped).c_str());
  }
  return clamped;
}

#define SETTINGS_SCHEMA_VERSION 2

// Cumulative settings-schema migration chain. Runs once at boot, right after the last NVS
// settings loader (initWeatherModeSettings), so steps see the fully seeded key set — a step
// that transforms a key already loaded this boot must also fix the RAM global. Standing rules:
//  - Any key rename/retire/unit-scale/enum change ships a permanent numbered step here in the
//    SAME commit; steps are never edited or deleted once shipped.
//  - NVS key strings are never reused or repurposed (firmware-internals.md).
//  - Pure additions need NO step: a missing key falls back to its hardcoded default at load.
//  - Steps must be idempotent: the schema stamp is written LAST, so a power cut mid-step
//    re-runs that whole step on the next boot.
void runSettingsMigrations() {
  int stored = settingRead(NK_cfgSchema).toInt();  // absent key reads "" -> 0 = pre-schema device
  while (stored < SETTINGS_SCHEMA_VERSION) {
    switch (stored) {
      case 0:
        break;  // 0->1: stamp only — fielded pre-schema settings are valid as-is
      case 1:
        // 1->2: SwitchControlOverride (key "SwtchCntrlOvrrd", a dead placeholder) became
        // PhysicalPanelOverride ("PhysPanelOvrrd") on 2026-09-06 with the opposite sense (old 1 = web owns
        // the modes = new 0), so the value is not carried over; the retired key is erased.
        settingRemove("SwtchCntrlOvrrd");
        break;
    }
    stored++;
    settingWrite(NK_cfgSchema, String(stored).c_str());
    Serial.printf("Settings schema migrated to %d\n", stored);
  }
}

// ── Auto-commissioning snapshot / restore ────────────────────────────────────
// Phase 0 captures every setting the commissioning flow may overwrite as one
// positional CSV in a single NVS key, so an explicit abort reverts to the
// pre-commissioning tune. The field order is fixed and MUST match between save and
// restore. (Positional CSV, not JSON — dependency-free, matches the codebase ethos.)
// Field count of the positional snapshot CSV. Bump when adding a field — commissionRestore accepts only an
// exact match, so a snapshot from a different field set is refused rather than misread slot-for-slot.
static const int COMMISSION_SNAP_FIELDS = 14;

// Serialize the current scalar tune into `key` as one positional CSV. 12th field = HiLow (charge-rate
// mode — the app's stored selection HiLowUserSel, not the effective HiLow, which the switch panel owns
// while the override is on) so a revert never strands the user in the wrong mode; fields 13/14 = IExcessBaseA/CcOffsetA (the
// affine trip-line the Thresholds step writes). This scalar set IS the whole positional snapshot; the
// Min% floor table is backed up separately (see the deferred-Start worker, cxStartPersistService).
void commissionSnapshotScalarsToBuf(char *buf, size_t n) {
  snprintf(buf, n,
           "%.4f,%.4f,%.3f,%.3f,%.1f,%.1f,%.1f,%.3f,%.3f,%.2f,%.3f,%d,%.1f,%.1f",
           PidKp, PidKi, OutputPIDFilterTC, VoltageFilterTC,
           IExcessTau, IExcessFloorA, IExcessCeilA, IExcessFrac, IExcessFracBulk,
           SystemIDStabilizeAmps, SystemIDStepAmplitude, HiLowUserSel, IExcessBaseA, IExcessCcOffsetA);
}

void commissionSnapshotScalars(const char* key) {
  char buf[180];
  commissionSnapshotScalarsToBuf(buf, sizeof(buf));
  settingWrite(key, buf);
}

// In-flight step snapshot: re-taken on each step entry, so it holds the scalar tune as of the START of the
// step currently running (every finished step is already baked in). A reboot mid-wizard restores it to undo
// ONLY the interrupted step. Scalars only — a partially-swept Min% floor is left as valid data and the
// un-done step is simply re-run.
void commissionStepSnapshot() {
  commissionSnapshotScalars(NK_commissionStepSnap);
}

// Discard the measured-ripple projections. Sibling of cvStressForgetLast below: a stored measurement
// whose defining conditions moved is not stale data to be labelled, it is a measurement of a quantity
// this device no longer computes. And it is not inert — ripFitAlt draws the red "ripple crosses the trip
// line" shading and the verdict on the Protections plot, so a stale fit is an active false alarm.
//
// Scope, from the capture path (faFiltRippleUpdate): the pk-pk is an IExcessTau low-pass sampled over a
// ripWinMs window, split in half for the min-of-halves estimator. Both knobs change the QUANTITY, not its
// precision, so both invalidate ripFitAlt.
//   alsoSlope: ONLY for a ripWinMs change. slpFitAlt's value is the worst positive slope of g_cvKdFiltV,
//   the D term's own filtered voltage — IExcessTau does not touch it, and only decides which windows were
//   admitted. An admitted point stays a valid slope, so a tau change must not throw it away.
// NOT in scope for either knob: the Resonance & Ripple Map. faWindowFinalize measures the raw decimated
// stream (dmv / faToneBuf) over its own fixed FA_WIN_DECIM_N window with hard-coded EMAs (FA_EMA_ALPHA,
// FA_AMPS_EMA_ALPHA). Neither knob reaches it. Earlier console copy told the operator to clear the map;
// that was wrong on both counts — it named the one table these knobs cannot affect, and left the two
// they do affect in place.
void ripFitForget(bool alsoSlope, const char *why) {
  bool had = (ripFitAlt.nPts > 0) || (alsoSlope && slpFitAlt.nPts > 0);
  memset(&ripFitAlt, 0, sizeof(ripFitAlt));
  settingRemove(NK_ripFitAlt);
  if (alsoSlope) { memset(&slpFitAlt, 0, sizeof(slpFitAlt)); settingRemove(NK_slpFitAlt); }
  // Generic consequence: the callers cover two different failures — a knob change makes the stored fit a
  // measurement of a quantity the device no longer computes, while a tach rescale leaves the measurement
  // intact but relabels the RPM it was taken at. "No longer valid" is true of both.
  if (had) queueConsoleMessageF("%s - the stored ripple current-check fit is no longer valid and was discarded; re-run the current check to replace it", why);
}

// Apply a positional-CSV scalar snapshot from `key`. Returns false (and drops a corrupt key) on a short
// read. Does NOT touch the Min% backup and does NOT remove `key` on success — the caller owns cleanup.
bool commissionRestoreScalars(const char* key) {
  if (!settingExists(key)) return false;
  String s = settingRead(key);
  float v[COMMISSION_SNAP_FIELDS];
  int n = sscanf(s.c_str(), "%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f",
                 &v[0], &v[1], &v[2], &v[3], &v[4], &v[5],
                 &v[6], &v[7], &v[8], &v[9], &v[10], &v[11], &v[12], &v[13]);
  // Exact count only. A short read means a truncated/corrupt snapshot, or one written by a build with a
  // different field set — either way the slots no longer mean what this code assumes. Refuse rather than
  // half-apply values into live control parameters. Log here, not at the call sites: the boot path discards
  // the return value, and leaving the snapshot in NVS would make every later revert repeat this silently.
  if (n != COMMISSION_SNAP_FIELDS) {
    queueConsoleMessageF("Commissioning: snapshot unreadable (%d of %d fields) - settings NOT reverted",
                         n, COMMISSION_SNAP_FIELDS);
    settingRemove(key);
    return false;
  }
  PidKp = v[0];                  settingWrite(NK_PidKp, String(PidKp, 4).c_str());
  PidKi = v[1];                  settingWrite(NK_PidKi, String(PidKi, 4).c_str());
  OutputPIDFilterTC = v[2];      settingWrite(NK_OutputPIDFilterTC, String(OutputPIDFilterTC, 2).c_str());
  VoltageFilterTC = v[3];        settingWrite(NK_VoltageFilterTC, String(VoltageFilterTC, 2).c_str());
  if (fabsf(v[4] - IExcessTau) > 0.05f) ripFitForget(false, "Commissioning revert moved the IExcess averaging TC");
  IExcessTau = v[4];             settingWrite(NK_IExcessTau, String(IExcessTau, 1).c_str());
  IExcessFloorA = v[5];          settingWrite(NK_IExcessFloorA, String(IExcessFloorA, 1).c_str());
  IExcessCeilA = v[6];           settingWrite(NK_IExcessCeilA, String(IExcessCeilA, 1).c_str());
  IExcessFrac = v[7];            settingWrite(NK_IExcessFrac, String(IExcessFrac, 3).c_str());
  IExcessFracBulk = v[8];        settingWrite(NK_IExcessFracBulk, String(IExcessFracBulk, 3).c_str());
  SystemIDStabilizeAmps = v[9];  settingWrite(NK_SystemIDStabilizeAmps, String(SystemIDStabilizeAmps, 2).c_str());
  SystemIDStepAmplitude = v[10]; settingWrite(NK_SystemIDStepAmplitude, String(SystemIDStepAmplitude, 3).c_str());
  // HiLow (12th field). When it differs, restore the mode AND swap the active cap tables to match.
  int snapMode = (int)(v[11] + 0.5f);
  if (snapMode != HiLowUserSel) {
    HiLowUserSel = snapMode;  // restore the app's stored choice; it only reaches the live mode while the panel override is off
    settingWrite(NK_HiLow, String(HiLowUserSel).c_str());
    applyChargeRateMode(resolveChargeRateMode());
  }
  // Affine trip-line intercept + CC offset (13th/14th fields, always written as a pair).
  IExcessBaseA = v[12];     settingWrite(NK_IExcessBaseA, String(IExcessBaseA, 1).c_str());
  IExcessCcOffsetA = v[13]; settingWrite(NK_IExcessCcOffsetA, String(IExcessCcOffsetA, 1).c_str());
  recomputeCcGains();  // re-apply CC gains live (normalized to SYSTEM_VOLTAGE_CLASS)
  return true;
}

// Restore from the Phase-0 origin snapshot (explicit-abort path): scalars + Min% floor table, then drop
// the origin key. Returns false if none exists.
bool commissionRestore() {
  bool ok = commissionRestoreScalars(NK_commissionSnap);
  // Min% revert runs unconditionally: the bk_* blobs are an independent store, so an unreadable
  // scalar snapshot (field-count drift from an OTA mid-run) must not strand a valid floor backup.
  // Safe when no complete backup exists — it returns false and changes nothing.
  ok = commissionRestoreMinPct() || ok;
  settingRemove(NK_commissionSnap);
  return ok;
}

// Put the lifecycle bookkeeping back to exactly what it was before the run that is being aborted
// (written by the deferred Start worker on every fresh Start). Returns false when no record exists —
// the caller then falls back to the legacy zeroing.
bool commissionRestorePreRun() {
  if (!settingExists(NK_commissionPreRun)) return false;
  String s = settingRead(NK_commissionPreRun);
  int st = 0, ph = 0, dm = 0, mm = 0;
  int n = sscanf(s.c_str(), "%d,%d,%d,%d", &st, &ph, &dm, &mm);
  settingRemove(NK_commissionPreRun);
  if (n != 4 || st < 0 || st > 2) return false;
  commissionSetState((uint8_t)st);
  commissionSetPhase((uint8_t)ph);
  commissionDoneMask = (uint16_t)dm;
  commissionWriteDoneMask();
  commissionManualMask = (uint16_t)mm;
  commissionWriteManualMask();
  return true;
}

// Persist the commissioning state byte (0=not / 1=in-progress / 2=commissioned).
void commissionSetState(uint8_t st) {
  commissionState = st;
  settingWrite(NK_commissionState, String((int)st).c_str());
}

// Persist the current wizard phase (0=Prep…8=Stress test, 9=finished; moves backward on Back). Drives
// the Commissioning tab checklist so step progress survives a page reload / new client.
void commissionSetPhase(uint8_t p) {
  commissionPhase = p;
  settingWrite(NK_commissionPhase, String((int)p).c_str());
}

// ── Per-stage completion tracking ─────────────────────────────────────────────
// commissionDoneMask carries one bit per stage (0=Prep, 1=Field curve … 7=Min% floor + Field decay,
// 8=Stress test). It is the source of truth for the per-step ✓ marks and for the default checkbox
// selection of a partial re-run. commissionState (0/1/2) is the lifecycle badge and is DERIVED from
// the mask wherever it is recomputed below.
#define COMMISSION_STAGE_COUNT 9
#define COMMISSION_ALL_DONE    0x1FF  // bits 0..8 set = every stage complete (7 = Min% floor + Field decay, 8 = CV stress test)
// COMMISSIONED requires only bits 0..7: the Stress test(8) is a diagnostic reference check that writes
// no settings, so skipping it never blocks the badge or keeps the nag alive. Its done bit still drives
// the step-9 ✓ and goes stale on upstream retunes like any other stage.
#define COMMISSION_REQUIRED_MASK 0x0FF
static bool commissionRequiredComplete() { return (commissionDoneMask & COMMISSION_REQUIRED_MASK) == COMMISSION_REQUIRED_MASK; }

// Downstream stages invalidated when an upstream stage is (re)completed — see the coupling
// analysis: Field curve(1) feeds Plant fit(2) + Verify(3); Plant fit(2) feeds Verify(3);
// Disturbances(4) feeds Thresholds(5). CV plant fit(6) measures the current→voltage plant, which
// sits downstream of the whole inner current loop — so any current-loop retune (Field curve 1,
// Plant fit 2, or Verify 3) makes the CV fit stale and clears bit 6. The Stress test(8) verdict
// grades the tuned CV loop, so it goes stale with any retune upstream of it (1/2/3/6). Min% floor +
// Field decay(7) is independent — nothing feeds it, it feeds nothing; it runs late so the engine is
// warm for the max-RPM hold. Tach alignment (RPMScalingFactor/PulleyRatio) is set on a pre-wizard
// screen, not a stage — a later rescale invalidates the binned stages via
// commissionClearRpmDependents. Re-doing an upstream stage clears its dependents' done bits; the
// wizard forces them into the same run to be re-measured.
static uint16_t commissionDependentsMask(int stage) {
  switch (stage) {
    case 1: return (1 << 2) | (1 << 3) | (1 << 6) | (1 << 8);  // Field curve → Plant fit, Verify, CV plant fit, Stress test
    case 2: return (1 << 3) | (1 << 6) | (1 << 8);             // Plant fit   → Verify, CV plant fit, Stress test
    case 3: return (1 << 6) | (1 << 8);                        // Verify      → CV plant fit, Stress test
    case 4: return (1 << 5);                                   // Disturbances → Thresholds
    case 6: return (1 << 8);                                   // CV plant fit → Stress test (a re-fit re-gains the loop the verdict graded)
    default: return 0;
  }
}

void commissionWriteDoneMask() {
  settingWrite(NK_commissionDoneMask, String((int)commissionDoneMask).c_str());
}

void commissionWriteManualMask() {
  settingWrite(NK_commissionManualMask, String((int)commissionManualMask).c_str());
}

// Skip a stage "for now": leave it NOT done (badge keeps nagging) but flag it as hand-touched so the
// Finish summary and checklist can show it as skipped rather than merely un-reached. Also un-marks done
// (a skip of a previously-done stage demotes it to outstanding).
void commissionSkipStage(int stage) {
  if (stage < 0 || stage >= COMMISSION_STAGE_COUNT) return;
  commissionDoneMask &= ~(1 << stage);
  commissionManualMask |= (1 << stage);
  commissionWriteDoneMask();
  commissionWriteManualMask();
  cxLedgerLogStage(stage, "skip");
  if (commissionState == 2 && !commissionRequiredComplete()) commissionSetState(1);
}

// Mark a stage done BY HAND (unmeasured): sets its done bit so completion math is satisfied and the nag
// can stop, flagged manual. Unlike a measured completion it does NOT invalidate downstream stages —
// nothing was re-measured that could stale them.
void commissionManualStage(int stage) {
  if (stage < 0 || stage >= COMMISSION_STAGE_COUNT) return;
  commissionDoneMask |= (1 << stage);
  commissionManualMask |= (1 << stage);
  commissionWriteDoneMask();
  commissionWriteManualMask();
  cxLedgerLogStage(stage, "manual");
}

// Derive the lifecycle byte from the mask: none done → NOT, required stages done (stress test
// optional) → COMMISSIONED, anything in between → IN_PROGRESS. Called at FINISH only — during an
// active wizard, commissionState is held at IN_PROGRESS explicitly (set by commissionStart) so the
// badge reads "in progress" even while the mask is briefly still all-set on a re-commission.
void commissionRecomputeState() {
  uint8_t st = (commissionDoneMask == 0) ? 0 : (commissionRequiredComplete() ? 2 : 1);
  if (st != commissionState) commissionSetState(st);
}

// The stress verdict grades ONE tuned CV loop. The moment a retune clears its done bit the stored
// blob stops being a statement about this device, so it has to go with the bit — otherwise the
// wizard's Step 9, the standalone Tuning modal and the config export all keep serving a verdict for
// a loop that no longer exists. Called from every path that clears bit 8 by invalidation; NOT from
// commissionSkipStage, where skipping means "not run this pass", not "the old one is wrong".
static void cvStressForgetLast() {
  if (cvsLastBlob[0] == '\0') return;
  cvsLastBlob[0] = '\0';
  settingRemove(NK_cvStressLast);
  queueConsoleMessage("Stress-test verdict discarded — the loop it graded was retuned");
}

// Mark a stage complete, then invalidate (clear) every downstream stage it feeds so an
// interrupted partial run can't leave a dependent showing a stale ✓. Does NOT touch
// commissionState — that is owned by start/abort/done.
void commissionMarkStage(int stage) {
  if (stage < 0 || stage >= COMMISSION_STAGE_COUNT) return;
  uint16_t deps = commissionDependentsMask(stage);
  commissionDoneMask |= (1 << stage);
  commissionDoneMask &= ~deps;
  commissionWriteDoneMask();
  if (deps & (1 << 8)) cvStressForgetLast();
  // This stage is now MEASURED (not hand-set); its invalidated dependents revert to pending, not manual.
  commissionManualMask &= ~((1 << stage) | deps);
  commissionWriteManualMask();
  cxLedgerLogStage(stage, "stage");  // ledger row carries the stage's measured results + applied values
}

// Clear a single stage's done bit plus anything downstream of it. A previously-COMMISSIONED
// device with a now-stale step is demoted to IN_PROGRESS so the badge nags that a step needs
// re-running. (Called when an RPM-breakpoint change stales the Min% floor + Field decay stage, 7.)
void commissionClearStage(int stage) {
  if (stage < 0 || stage >= COMMISSION_STAGE_COUNT) return;
  uint16_t cleared = (1 << stage) | commissionDependentsMask(stage);
  commissionDoneMask &= ~cleared;
  commissionWriteDoneMask();
  if (cleared & (1 << 8)) cvStressForgetLast();
  commissionManualMask &= ~cleared;   // a staled stage is no longer satisfied, hand-set or otherwise
  commissionWriteManualMask();
  if (commissionState == 2 && !commissionRequiredComplete()) commissionSetState(1);
}

// A tach rescale (RPMScalingFactor/PulleyRatio change) moves the engine-RPM axis every binned stage
// was measured against, so all of them must be re-run. Hangs off the SETTING change, wherever it comes
// from (pre-wizard alignment screen or normal Settings). Clears every RPM-binned wizard stage; only
// Prep(0) and Stress test(8) are exempt — the stress verdict grades recovery behavior, which a
// display-axis rescale can't stale. Min% floor + Field decay(7) IS binned (per-RPM onset anchors AND
// the drain-vs-RPM line endpoints), so it clears.
void commissionClearRpmDependents() {
  uint16_t rpmBits = (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4) | (1 << 5) | (1 << 6) | (1 << 7);
  commissionDoneMask &= ~rpmBits;
  commissionWriteDoneMask();
  commissionManualMask &= ~rpmBits;
  commissionWriteManualMask();
  if (commissionState == 2) commissionSetState(1);
}

// ── Deferred commissioning-start persist ─────────────────────────────────────
// Start used to retire its whole restore point (origin snapshot + Min% blobs + state keys, ~7 NVS
// commits) inside the /get handler; on the shared network task that starved the SSE stream and froze
// the interface ~2 s. The handler now stages everything in RAM — atomic at the click — and
// cxStartPersistService(), called once per loop() pass, retires ONE commit per pass. Write order is
// the crash guarantee: commissionState=1 is committed LAST, so a persisted state byte proves the
// whole restore point is on flash; a reboot mid-burst boots as "never started". The wizard polls
// /cxStartState and advances only on IDLE — the HTTP 200 means accepted, not saved.
static char  cxStartTuneCsv[192];   // scalar tune at the click — origin snapshot AND step baseline
static float cxStartMinDuty[RPM_TABLE_SIZE], cxStartKneeFloor[RPM_TABLE_SIZE], cxStartKneeKnee[RPM_TABLE_SIZE];
static bool  cxStartKneeFrozen[RPM_TABLE_SIZE];
static float cxStartKneeFitA;
static bool  cxStartResume;         // resuming a live run: origin snapshot + Min% backup kept from the original Start
static uint8_t  cxStartPrevState, cxStartPrevPhase;   // pre-click wizard bookkeeping, restored exactly if a fresh
static uint16_t cxStartPrevDoneMask, cxStartPrevManualMask;  // still-pending Start is cancelled; also persisted as NK_commissionPreRun for Abort
volatile uint8_t cxStartPersistStep = 0;  // 0 = idle; 1..7 = next write to retire
volatile bool cxStartPersistFail = false; // restore-point write refused → start cancelled

void cxStartPersistBegin(bool resuming) {
  // cxStartMutex serializes this against the worker: the give below is the release barrier that
  // publishes the staged buffers to Core 1, and step=1 stays invisible until then.
  if (!cxStartMutex) return;
  xSemaphoreTake(cxStartMutex, portMAX_DELAY);  // worker holds it at most one staged NVS write
  commissionSnapshotScalarsToBuf(cxStartTuneCsv, sizeof(cxStartTuneCsv));
  memcpy(cxStartMinDuty, rpmMinDutyTable, sizeof(cxStartMinDuty));
  memcpy(cxStartKneeFloor, kneeFloor, sizeof(cxStartKneeFloor));
  memcpy(cxStartKneeKnee, kneeKnee, sizeof(cxStartKneeKnee));
  memcpy(cxStartKneeFrozen, kneeFrozen, sizeof(cxStartKneeFrozen));
  cxStartKneeFitA = kneeFitA;
  cxStartPrevState = commissionState;
  cxStartPrevPhase = commissionPhase;
  cxStartPrevDoneMask = commissionDoneMask;
  cxStartPrevManualMask = commissionManualMask;
  cxStartResume = resuming;
  cxStartPersistFail = false;
  cxStartPersistStep = 1;
  xSemaphoreGive(cxStartMutex);
}

// Cancel a still-pending Start (abort/done raced the worker). Fresh start: nothing has run, so this
// is an exact teardown to the pre-click state — NOT a revert, and the caller must not fall into the
// live-run teardown (which would demote a previously-commissioned device whose re-run never began).
// Resume: just stop the worker — the original committed snapshot is intact for the normal teardown.
// Whole body under cxStartMutex: once the take succeeds the worker is provably between cases, so the
// machine cannot advance again (it re-reads step under the lock) and the compensating writes below
// can never interleave with an in-flight staged write. Returns with the cancel COMPLETE.
// The fresh/resume/none verdict is decided under that same take: a caller that instead re-read the
// step could branch on a Start the worker had already retired to state=1 and skip the live teardown.
CxStartCancel cxStartPersistCancel() {
  if (!cxStartMutex) return CX_CANCEL_NONE;
  xSemaphoreTake(cxStartMutex, portMAX_DELAY);  // worker holds it at most one staged NVS write
  if (cxStartPersistStep == 0) { xSemaphoreGive(cxStartMutex); return CX_CANCEL_NONE; }
  bool fresh = !cxStartResume;
  cxStartPersistStep = 0;
  if (fresh) {
    testProtectionsEnabled = commissionProtBackup;
    kneeFitA = cxStartKneeFitA;             // the handler zeroed it on the click; it is live in the Min% temp correction, not scratch
    settingRemove(NK_commissionSnap);       // drop whatever subset the worker already wrote
    settingRemove(NK_commissionPreRun);
    settingRemove(NK_commissionStepSnap);
    commissionClearMinPctBackup();
    commissionSetPhase(cxStartPrevPhase);
    commissionDoneMask = cxStartPrevDoneMask;
    commissionWriteDoneMask();
    commissionManualMask = cxStartPrevManualMask;
    commissionWriteManualMask();
  }
  xSemaphoreGive(cxStartMutex);
  return fresh ? CX_CANCEL_FRESH : CX_CANCEL_RESUME;
}

// Retire one staged write per call — called every loop() pass, no-op when idle. Each case runs
// under cxStartMutex, so a Cancel from the network task can only land BETWEEN cases, never inside
// one — the old lock-free double-checks left an instruction-wide window where a mid-case cancel
// was overwritten by the advance, resurrecting a torn-down Start all the way to commissionSetState(1).
// The take is zero-timeout: a Cancel/Begin holding the lock just costs this pass, never a loop stall.
void cxStartPersistService() {
  if (cxStartPersistStep == 0) return;  // lock-free fast path — Begin publishes step=1 via the mutex
  if (!cxStartMutex || xSemaphoreTake(cxStartMutex, 0) != pdTRUE) return;
  uint8_t s = cxStartPersistStep;
  switch (s) {
    case 0: break;  // cancel landed between the fast path and the take
    case 1:  // origin snapshot FIRST — it is the abort path's entire restore point
      if (!cxStartResume && !settingWrite(NK_commissionSnap, cxStartTuneCsv)) {
        // No restore point ⇒ refuse the run: starting anyway would leave Abort with nothing to revert to.
        testProtectionsEnabled = commissionProtBackup;
        kneeFitA = cxStartKneeFitA;   // same teardown as the fresh cancel path
        cxStartPersistStep = 0;
        cxStartPersistFail = true;
        settingsDirty = true;
        queueConsoleMessage("Commissioning: could not save the settings restore point (flash write failed) — start cancelled");
        break;
      }
      cxStartPersistStep = 2;
      break;
    case 2:  // pre-run bookkeeping — Abort restores it, so a device commissioned before this run ends up commissioned again
      if (!cxStartResume) {
        char b[40];
        snprintf(b, sizeof(b), "%d,%d,%d,%d", (int)cxStartPrevState, (int)cxStartPrevPhase, (int)cxStartPrevDoneMask, (int)cxStartPrevManualMask);
        settingWrite(NK_commissionPreRun, b);
      }
      cxStartPersistStep = 3;
      break;
    case 3:
      if (!cxStartResume) commissionBackupMinPct(cxStartMinDuty, cxStartKneeFloor, cxStartKneeKnee, cxStartKneeFrozen, cxStartKneeFitA);
      cxStartPersistStep = 4;
      break;
    case 4: settingWrite(NK_commissionStepSnap, cxStartTuneCsv); cxStartPersistStep = 5; break;
    case 5: commissionSetPhase(0); cxStartPersistStep = 6; break;
    case 6: commissionMarkStage(0); cxStartPersistStep = 7; break;  // Prep complete: snapshot staged, preconditions checked
    case 7:
      commissionSetState(1);  // LAST — a persisted state=1 proves the restore point is on flash
      cxStartPersistStep = 0;
      settingsDirty = true;   // push the CSV3 state echo promptly
      queueConsoleMessage("Commissioning: started — settings snapshotted");
      break;
  }
  xSemaphoreGive(cxStartMutex);
}

bool fsMkdir(const char *path) {
  if (!fsMutex || xSemaphoreTake(fsMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    Serial.println("fsMkdir: mutex timeout");
    return false;
  }
  bool result = LittleFS.mkdir(path);
  xSemaphoreGive(fsMutex);
  return result;
}

size_t fsTotalBytes() {
  if (!fsMutex || xSemaphoreTake(fsMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    return 0;
  }
  size_t result = LittleFS.totalBytes();
  xSemaphoreGive(fsMutex);
  return result;
}

size_t fsUsedBytes() {
  if (!fsMutex || xSemaphoreTake(fsMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    return 0;
  }
  size_t result = LittleFS.usedBytes();
  xSemaphoreGive(fsMutex);
  return result;
}

// Short-try variant for the main loop's periodic free-space stat: a busy mutex means
// skip this round (caller keeps the previous value) rather than stalling the loop.
// Zero-tick take: this runs in the control path with field possibly on, so it must never
// wait on a Core-0 holder (a /huntledger read, console flush) even briefly.
bool fsStatsTry(size_t &total, size_t &used) {
  if (!fsMutex || xSemaphoreTake(fsMutex, 0) != pdTRUE) return false;
  total = LittleFS.totalBytes();
  used = LittleFS.usedBytes();
  xSemaphoreGive(fsMutex);
  return true;
}

void fsTakeLock() {
  if (fsMutex) {
    if (xSemaphoreTake(fsMutex, pdMS_TO_TICKS(10000)) != pdTRUE) {
      Serial.println("fsTakeLock: mutex timeout - filesystem unprotected!");
    }
  }
}

void fsReleaseLock() {
  // Only give if this task actually holds it: after a fsTakeLock timeout, giving a mutex
  // owned by another task trips configASSERT in xTaskPriorityDisinherit (panic/reboot).
  if (fsMutex && xSemaphoreGetMutexHolder(fsMutex) == xTaskGetCurrentTaskHandle()) {
    xSemaphoreGive(fsMutex);
  }
}

// Versioned PSRAM-blob persistence — the shared scaffold for "accumulate in
// PSRAM, dump to a versioned LittleFS blob at field-off, restore at boot."
// Built for the long-term-plot ring and the alternator/boat matrices so each
// system stops re-hand-rolling the header, fsLock, chunked I/O, watchdog resets,
// and layout-change guard. The sensor ring + eff matrix predate this and keep
// their own copies; retrofit them onto this later. Field-off only (LittleFS
// writes can stall a core ~300 ms — never on a live control loop).
struct PsramBlobHeader {
  uint32_t magic;       // caller-chosen dataset id
  uint32_t version;     // caller-chosen layout version (bump on record-layout change)
  uint32_t count;       // records in body
  uint32_t recordSize;  // sizeof(record) — layout-change guard
  uint32_t userWord;    // caller flags (e.g. "reference finalized"); 0 if unused
};

// Dump `count` records to a versioned LittleFS blob. Records live in `base` as a
// ring of `capacity` slots starting at `startIdx`, written in logical order so a
// circular buffer dumps chronologically; a plain array passes startIdx=0,
// count=capacity (no wrap). Body goes out in ≤2 contiguous spans. Removes the
// partial file on any short write. Returns records written (0 on failure).
uint32_t writePsramBlob(const char *path, uint32_t magic, uint32_t version,
                        uint32_t userWord, const void *base, size_t recordSize,
                        uint32_t capacity, uint32_t startIdx, uint32_t count) {
  if (!base || recordSize == 0 || capacity == 0) return 0;
  if (count > capacity) count = capacity;

  fsTakeLock();
  File f = LittleFS.open(path, "w");
  if (!f) {
    fsReleaseLock();
    Serial.printf("writePsramBlob: open failed (%s)\n", path);
    return 0;
  }
  fsFreeDirty = true;

  PsramBlobHeader hdr = { magic, version, count, (uint32_t)recordSize, userWord };
  bool ok = (f.write((const uint8_t *)&hdr, sizeof(hdr)) == sizeof(hdr));

  // Body in up to two contiguous spans: [startIdx..capacity) then wrap [0..rem).
  const uint8_t *bytes = (const uint8_t *)base;
  uint32_t remaining = count;
  uint32_t idx = startIdx % capacity;
  while (ok && remaining > 0) {
    uint32_t span = capacity - idx;          // slots to end of buffer
    if (span > remaining) span = remaining;
    size_t spanBytes = (size_t)span * recordSize;
    ok = (f.write(bytes + (size_t)idx * recordSize, spanBytes) == spanBytes);
    remaining -= span;
    idx = 0;                                 // wrapped span starts at buffer top
    if (wdtMainTaskSubscribed) esp_task_wdt_reset();
  }

  f.close();
  if (!ok) LittleFS.remove(path);            // drop the partial file
  fsReleaseLock();
  if (!ok) {
    Serial.printf("writePsramBlob: short write (%s) — file removed\n", path);
    return 0;
  }
  return count;
}

// Restore a writePsramBlob() blob into linear buffer `destBase` (`destCapacity`
// slots). Validates magic/version/recordSize; on any mismatch or short read it
// deletes the file and returns 0. Returns records restored (≤ destCapacity), and
// the stored userWord via `userWordOut` if non-null. `deleteAfter` removes the
// file post-restore (rings set true to avoid double-upload; grids set false).
uint32_t readPsramBlob(const char *path, uint32_t magic, uint32_t version,
                       void *destBase, size_t recordSize, uint32_t destCapacity,
                       uint32_t *userWordOut, bool deleteAfter) {
  if (!destBase || recordSize == 0 || destCapacity == 0) return 0;
  if (!fsExists(path)) return 0;

  fsTakeLock();
  File f = LittleFS.open(path, "r");
  if (!f) {
    fsReleaseLock();
    Serial.printf("readPsramBlob: open failed (%s)\n", path);
    return 0;
  }

  PsramBlobHeader hdr;
  bool ok = (f.readBytes((char *)&hdr, sizeof(hdr)) == sizeof(hdr)
             && hdr.magic == magic
             && hdr.version == version
             && hdr.recordSize == (uint32_t)recordSize);
  if (!ok) {
    f.close();
    LittleFS.remove(path);
    fsReleaseLock();
    Serial.printf("readPsramBlob: header mismatch (%s) — discarded\n", path);
    return 0;
  }

  uint32_t toRead = (hdr.count > destCapacity) ? destCapacity : hdr.count;
  size_t bodyBytes = (size_t)toRead * recordSize;
  size_t bodyRead = f.readBytes((char *)destBase, bodyBytes);
  if (wdtMainTaskSubscribed) esp_task_wdt_reset();
  f.close();
  if (deleteAfter) LittleFS.remove(path);
  fsReleaseLock();

  if (bodyRead != bodyBytes) {
    Serial.printf("readPsramBlob: short body read (%u/%u) (%s)\n",
                  (unsigned)bodyRead, (unsigned)bodyBytes, path);
    return 0;
  }
  if (userWordOut) *userWordOut = hdr.userWord;
  return toRead;
}

// ── Append-extend a writePsramBlob file (flash-wear optimization for high-cadence rings) ──────
// A circular PSRAM ring persisted by full-file rewrite erases every block on each flush; at a
// 15/30-min field-off cadence that is the dominant lifetime flash-write source. Instead, the
// periodic flush APPENDS only the records pushed since the last flush (LittleFS "a" touches just
// the tail block), and a full writePsramBlob() compaction runs only when the file would grow past
// ~2× the ring. The file is one chronological oldest→newest sequence (compacted body + appended
// tail); restoreRingBlob() derives the true record count from file size and keeps the newest
// `capacity`, so a plain writePsramBlob snapshot (zero appends) restores identically — no format
// version bump, old field files stay valid.
//
// Appends the newest `delta` records of ring `base[capacity]` (slots ending at head-1, oldest of
// those first → chronological). Returns bytes written, 0 on any failure (caller then compacts).
uint32_t appendRingBlob(const char *path, const void *base, size_t recordSize,
                        uint32_t capacity, uint32_t head, uint32_t delta) {
  if (!base || recordSize == 0 || capacity == 0 || delta == 0 || delta > capacity) return 0;
  fsTakeLock();
  File f = LittleFS.open(path, "a");
  if (!f) { fsReleaseLock(); return 0; }
  fsFreeDirty = true;
  const uint8_t *bytes = (const uint8_t *)base;
  bool ok = true;
  for (uint32_t i = delta; i >= 1 && ok; i--) {          // i=delta → oldest new record, i=1 → newest
    uint32_t slot = (head + capacity - i) % capacity;
    ok = (f.write(bytes + (size_t)slot * recordSize, recordSize) == recordSize);
    if (wdtMainTaskSubscribed) esp_task_wdt_reset();
  }
  f.close();
  fsReleaseLock();
  return ok ? delta * (uint32_t)recordSize : 0;
}

// Restore a (possibly append-extended) writePsramBlob file into linear `base` (tail=0, head=count).
// File may hold MORE than `capacity` records (accumulated appends) — keeps the newest `capacity`.
// Validates magic/version/recordSize (mismatch → delete + 0). *fileRecordsOut = total records on
// disk (may exceed capacity → caller compacts when capped). Returns records restored (≤ capacity).
uint32_t restoreRingBlob(const char *path, uint32_t magic, uint32_t version,
                         void *base, size_t recordSize, uint32_t capacity,
                         uint32_t *fileRecordsOut) {
  if (fileRecordsOut) *fileRecordsOut = 0;
  if (!base || recordSize == 0 || capacity == 0 || !fsExists(path)) return 0;
  fsTakeLock();
  File f = LittleFS.open(path, "r");
  if (!f) { fsReleaseLock(); return 0; }
  PsramBlobHeader hdr;
  bool ok = (f.readBytes((char *)&hdr, sizeof(hdr)) == sizeof(hdr)
             && hdr.magic == magic && hdr.version == version
             && hdr.recordSize == (uint32_t)recordSize);
  if (!ok) {
    f.close();
    LittleFS.remove(path);
    fsReleaseLock();
    Serial.printf("restoreRingBlob: header mismatch (%s) — discarded\n", path);
    return 0;
  }
  size_t sz = f.size();
  uint32_t fileRecords = (sz > sizeof(hdr)) ? (uint32_t)((sz - sizeof(hdr)) / recordSize) : 0;
  uint32_t keep = (fileRecords > capacity) ? capacity : fileRecords;
  if (keep > 0) {
    f.seek(sizeof(hdr) + (size_t)(fileRecords - keep) * recordSize);   // skip evicted overflow
    size_t want = (size_t)keep * recordSize;
    if (f.readBytes((char *)base, want) != want) { f.close(); fsReleaseLock(); return 0; }
  }
  f.close();
  fsReleaseLock();
  if (wdtMainTaskSubscribed) esp_task_wdt_reset();
  if (fileRecordsOut) *fileRecordsOut = fileRecords;
  return keep;
}

void performDeepFactoryReset() {
  Serial.println("\n=== DEEP FACTORY RESET INITIATED ===");
  queueConsoleMessage("DEEP FACTORY RESET: Scorched-earth reset starting...");

  // Step 0: Preserve cloud identity (authToken) across the wipe so that pressing
  // Erase All Memory does NOT lock the user out of their cloud account. Token is
  // held in a stack buffer for the ~2-3 second wipe; power-loss in that window
  // loses the token and requires support contact for re-registration.
  char savedToken[256] = { 0 };   // 256 matches /debugToken; cloud sets no length cap on issued tokens
  {
    nvs_handle_t h;
    if (nvs_open("cloud", NVS_READONLY, &h) == ESP_OK) {
      size_t len = sizeof(savedToken);
      esp_err_t e = nvs_get_str(h, "authToken", savedToken, &len);
      if (e != ESP_OK) {
        savedToken[0] = '\0';
        // A too-long token would silently de-register the device here — say so distinctly.
        if (e == ESP_ERR_NVS_INVALID_LENGTH)
          Serial.println("RESET: authToken exceeds preserve buffer — it will NOT survive the wipe!");
      }
      nvs_close(h);
    }
    if (savedToken[0] != '\0') {
      Serial.println("RESET: Preserving authToken across NVS wipe");
    } else {
      Serial.println("RESET: No authToken to preserve (device unregistered)");
    }
  }

  // Console delivery window comes BEFORE the wipe: once NVS is erased, every ms until
  // restart is a chance for a background task to re-persist something, so from the
  // format onward we sprint to ESP.restart() with no delays.
  queueConsoleMessage("DEEP FACTORY RESET: Wiping all settings and data, restarting...");
  delay(1500);

  // Step 1: Unmount and reformat LittleFS (userdata partition - rings, logs, buffer files;
  // user settings AND Vessel Info live in NVS and are wiped in Step 2). fsMutex is taken
  // and deliberately NEVER released — any task that would re-create a file before the
  // restart blocks on its timeout instead. The reboot clears the mutex.
  Serial.println("RESET: Acquiring FS mutex and unmounting LittleFS...");
  if (fsMutex) {
    xSemaphoreTake(fsMutex, pdMS_TO_TICKS(5000));  // Block other FS ops
  }
  LittleFS.end();
  littleFSMounted = false;

  Serial.println("RESET: Formatting LittleFS...");
  if (LittleFS.format()) {
    Serial.println("RESET: LittleFS formatted successfully");
  } else {
    Serial.println("RESET: WARNING - LittleFS format failed - continuing anyway");
  }

  // Step 2: Erase ALL NVS namespaces (storage, cloud, auth, update_req, timesync, etc.)
  Serial.println("RESET: Erasing entire NVS flash partition...");
  nvs_flash_deinit();  // Deinit before erase to avoid stale handles
  esp_err_t nvs_err = nvs_flash_erase();
  if (nvs_err == ESP_OK) {
    Serial.println("RESET: All NVS namespaces erased successfully");
  } else {
    Serial.printf("RESET: WARNING - NVS erase failed: %s\n", esp_err_to_name(nvs_err));
  }
  nvs_err = nvs_flash_init();
  if (nvs_err != ESP_OK) {
    Serial.printf("RESET: WARNING - NVS reinit failed: %s\n", esp_err_to_name(nvs_err));
  } else {
    Serial.println("RESET: NVS flash reinitialized");
  }

  // NO InitSystemSettings() here. RAM still holds the user's live values, and its
  // create-if-missing re-seed would write all of them straight back into the freshly
  // erased NVS — the bug that made settings survive this reset. The reboot below
  // reruns setup() with pristine compile-time defaults instead.

  // Step 3: Restore preserved authToken so cloud account survives the wipe
  if (savedToken[0] != '\0') {
    nvs_handle_t h;
    if (nvs_open("cloud", NVS_READWRITE, &h) == ESP_OK) {
      esp_err_t e = nvs_set_str(h, "authToken", savedToken);
      if (e == ESP_OK) e = nvs_commit(h);
      nvs_close(h);
      if (e == ESP_OK) {
        Serial.println("RESET: authToken restored - cloud account preserved");
      } else {
        Serial.printf("RESET: WARNING - authToken restore failed: %s\n", esp_err_to_name(e));
      }
    }
  }

  Serial.println("=== DEEP FACTORY RESET COMPLETE - RESTARTING ===\n");
  Serial.flush();
  ESP.restart();
}

// ==== Multi-drop 1-Wire probe registry (BATTERY_TEMP_SENSORS_SPEC.md §2) ====
// Roles bind by stored ROM code (owAddr* NVS keys), never by bus index: the search order is by serial.

void owAddrToHex(const uint8_t *addr, char *out) {  // out: 17 bytes, lowercase, no separators
  static const char hx[] = "0123456789abcdef";
  for (int i = 0; i < 8; i++) {
    out[i * 2] = hx[addr[i] >> 4];
    out[i * 2 + 1] = hx[addr[i] & 0x0F];
  }
  out[16] = '\0';
}

bool owHexToAddr(const char *hex, uint8_t *addr) {  // exactly 16 hex chars, either case; false leaves addr untouched
  if (!hex || strlen(hex) != 16) return false;
  uint8_t tmp[8] = { 0 };
  for (int i = 0; i < 16; i++) {
    char c = hex[i];
    uint8_t v;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else return false;
    tmp[i >> 1] |= (i & 1) ? v : (v << 4);
  }
  memcpy(addr, tmp, 8);
  return true;
}

bool owAddrIsZero(const uint8_t *a) {
  for (int i = 0; i < 8; i++) if (a[i]) return false;
  return true;
}

const char *owRoleName(int r) {
  return r == TR_ALT ? "alternator" : (r == TR_BATT ? "battery" : "extra");
}

// Present probes with no role — CSV2 owUnassignedCount and /tempSensors "unassigned". Counted under owMux
// because the CSV2 builder runs on Core 1 while owEnumerate() (Core 0) rewrites the registry.
uint8_t owUnassignedCount() {
  uint8_t n = 0;
  portENTER_CRITICAL(&owMux);
  for (uint8_t i = 0; i < owProbeCount; i++) {
    if (!owProbes[i].present) continue;
    bool bound = false;
    for (int r = 0; r < TR_COUNT; r++) if (tempRoleSlot[r] == (int8_t)i) { bound = true; break; }
    if (!bound) n++;
  }
  portEXIT_CRITICAL(&owMux);
  return n;
}

// Bus search, registry merge and role binding. Called from initializeHardware() at boot and from
// TempTask (Core 0) afterwards; it owns the bus for the search, so never from loop(). Auto-binds the
// alternator role only in the one-probe / digital-alternator-sensor (TempSource 0) / both-extra-enables-off
// case (fielded single-probe devices and a replaced alternator probe) — on a thermistor install a lone probe
// is a battery or extra probe awaiting its role — and queues the persist for owRoleService(); no NVS write here.
void owEnumerate() {
  static bool unassignedNoticeSent = false;  // one console line per boot
  sensors.begin();
  sensors.setWaitForConversion(false);  // re-apply after begin() in case it resets state
  sensors.setCheckForConversion(true);
  DeviceAddress found[OW_MAX_PROBES];
  uint8_t nFound = 0;
  uint8_t nDev = sensors.getDeviceCount();
  for (uint8_t i = 0; i < nDev && nFound < OW_MAX_PROBES; i++) {
    DeviceAddress a;
    if (!sensors.getAddress(a, i)) continue;
    if (a[0] != 0x28 || !sensors.validAddress(a)) continue;  // DS18B20 family with a valid ROM CRC only
    sensors.setResolution(a, resolution);
    memcpy(found[nFound++], a, sizeof(DeviceAddress));
  }

  OwProbe next[OW_MAX_PROBES];
  uint8_t nNext = 0;
  bool autoBound = false;
  portENTER_CRITICAL(&owMux);
  for (uint8_t f = 0; f < nFound; f++) {
    OwProbe &p = next[nNext++];
    memset(&p, 0, sizeof(p));
    memcpy(p.addr, found[f], sizeof(DeviceAddress));
    p.lastF = NAN;
    p.present = true;
    for (uint8_t i = 0; i < owProbeCount; i++) {  // keep the stats of an id already known
      if (memcmp(owProbes[i].addr, found[f], sizeof(DeviceAddress)) != 0) continue;
      p.lastF = owProbes[i].lastF;
      p.lastGoodMs = owProbes[i].lastGoodMs;
      p.okCount = owProbes[i].okCount;
      p.failCount = owProbes[i].failCount;
      break;
    }
  }
  // An id that dropped off the bus stays listed (present=0) only while a role still points at it.
  for (uint8_t i = 0; i < owProbeCount && nNext < OW_MAX_PROBES; i++) {
    bool inFound = false;
    for (uint8_t f = 0; f < nFound; f++) if (memcmp(owProbes[i].addr, found[f], sizeof(DeviceAddress)) == 0) { inFound = true; break; }
    if (inFound) continue;
    bool roleBound = false;
    for (int r = 0; r < TR_COUNT; r++) if (memcmp(owProbes[i].addr, tempRoleAddr[r], sizeof(DeviceAddress)) == 0) { roleBound = true; break; }
    if (!roleBound) continue;
    next[nNext] = owProbes[i];
    next[nNext].present = false;
    nNext++;
  }
  memcpy(owProbes, next, sizeof(OwProbe) * nNext);
  owProbeCount = nNext;

  for (int r = 0; r < TR_COUNT; r++) {
    tempRoleAssigned[r] = !owAddrIsZero(tempRoleAddr[r]);
    tempRoleSlot[r] = -1;
    if (!tempRoleAssigned[r]) continue;
    for (uint8_t i = 0; i < owProbeCount; i++) {
      if (owProbes[i].present && memcmp(owProbes[i].addr, tempRoleAddr[r], sizeof(DeviceAddress)) == 0) { tempRoleSlot[r] = (int8_t)i; break; }
    }
  }
  if (tempRoleSlot[TR_ALT] < 0 && nFound == 1 && TempSource == 0 && battTempProbeEnable == 0 && extraTempProbeEnable == 0) {
    int8_t only = -1;
    for (uint8_t i = 0; i < owProbeCount; i++) if (owProbes[i].present) { only = (int8_t)i; break; }
    if (only >= 0 && tempRoleSlot[TR_BATT] != only && tempRoleSlot[TR_EXTRA] != only) {
      memcpy(tempRoleAddr[TR_ALT], owProbes[only].addr, sizeof(DeviceAddress));
      tempRoleAssigned[TR_ALT] = true;
      tempRoleSlot[TR_ALT] = only;
      owRolePersistPending[TR_ALT] = true;
      autoBound = true;
    }
  }
  // The auto-bind may have moved ALT off an absent id the merge above kept; drop absent entries no role
  // points at any more. Absent entries sit at the tail, so present slots keep their indices.
  {
    uint8_t w = 0;
    for (uint8_t i = 0; i < owProbeCount; i++) {
      bool keep = owProbes[i].present;
      if (!keep) for (int r = 0; r < TR_COUNT; r++) if (memcmp(owProbes[i].addr, tempRoleAddr[r], sizeof(DeviceAddress)) == 0) { keep = true; break; }
      if (!keep) continue;
      if (w != i) owProbes[w] = owProbes[i];
      w++;
    }
    owProbeCount = w;
  }
  // tempDeviceAddress is the ALT read path's address: the bound probe, else the stored-but-absent id (its
  // isConnected pre-check then misses exactly as an unplugged single probe did), else zero.
  if (tempRoleSlot[TR_ALT] >= 0) memcpy(tempDeviceAddress, owProbes[tempRoleSlot[TR_ALT]].addr, sizeof(DeviceAddress));
  else if (tempRoleAssigned[TR_ALT]) memcpy(tempDeviceAddress, tempRoleAddr[TR_ALT], sizeof(DeviceAddress));
  else memset(tempDeviceAddress, 0, sizeof(DeviceAddress));
  portEXIT_CRITICAL(&owMux);

  if (autoBound) {
    char hex[17];
    owAddrToHex(tempRoleAddr[TR_ALT], hex);
    queueConsoleMessageF("Temperature probe %s bound to the alternator role", hex);
  }
  if (nFound >= 2 && !unassignedNoticeSent && owUnassignedCount() > 0) {
    unassignedNoticeSent = true;
    queueConsoleMessageF("Temperature probes: %u found, roles unassigned — assign them under Setup > Temperature", (unsigned)nFound);
  }
}

// loop() only (the field-off flash block): persists the role auto-binds TempTask queued. TempTask never
// calls settingWrite — a flash write on Core 0 stalls Core 1 while the cache is off.
void owRoleService() {
  static const char *const keys[TR_COUNT] = { NK_owAddrAlt, NK_owAddrBatt, NK_owAddrExtra };
  for (int r = 0; r < TR_COUNT; r++) {
    if (!owRolePersistPending[r]) continue;
    DeviceAddress a;
    portENTER_CRITICAL(&owMux);
    owRolePersistPending[r] = false;
    memcpy(a, tempRoleAddr[r], sizeof(DeviceAddress));
    portEXIT_CRITICAL(&owMux);
    char hex[17];
    owAddrToHex(a, hex);
    settingWrite(keys[r], hex);
    // The /get owAssign handler (Core 0) rewrites tempRoleAddr[] and NVS on its own; if it landed during the
    // write above its NVS value may now be under ours, so re-persist whatever the role holds now.
    DeviceAddress b;
    portENTER_CRITICAL(&owMux);
    memcpy(b, tempRoleAddr[r], sizeof(DeviceAddress));
    portEXIT_CRITICAL(&owMux);
    if (memcmp(a, b, sizeof(DeviceAddress)) != 0) {
      owAddrToHex(b, hex);
      settingWrite(keys[r], owAddrIsZero(b) ? "" : hex);
    }
  }
}

// The ALT read path keeps its own globals/counters; this copies each cycle's outcome into the alternator
// probe's registry row so /tempSensors lists it live like the other probes.
static void owMirrorAltResult(bool good, float tempF) {
  portENTER_CRITICAL(&owMux);
  const int8_t s = tempRoleSlot[TR_ALT];
  if (s >= 0 && s < (int8_t)owProbeCount) {
    if (good) {
      owProbes[s].lastF = tempF;
      owProbes[s].lastGoodMs = millis();
      owProbes[s].okCount++;
    } else {
      owProbes[s].failCount++;
    }
  }
  portEXIT_CRITICAL(&owMux);
}

// DS18B20 reader, 5 s cadence. Rejects CRC fails, all-0xFF and the power-on 85C/185F signature, holds
// the last good value on any fault (freshness tracked separately), re-enumerates after a disconnect and
// re-applies 12-bit resolution if an EEPROM or sensor reset changed it. tempTaskHealthy is owned by
// checkTempTaskHealth(); this task only ever sets it true.
// Multi-drop (BATTERY_TEMP_SENSORS_SPEC.md §3): one Skip ROM convert covers every probe on the bus, then
// the scratchpads are read in turn — the alternator (ALT role, tempDeviceAddress) first with the checks
// above (its outcome mirrored into its own owProbes[] row by owMirrorAltResult), then every other present
// probe into owProbes[] whether bound or not; a BATT/EXTRA-bound probe
// also updates its role global when that role's enable is 1. Only the ALT result drives lastReadWasSuccess
// (the 1 s retry); a BATT/EXTRA miss only feeds its own 3-cycle re-enumeration streak. An engine start
// (RPM crossing 200) forces one immediate cycle. This task NEVER calls settingWrite — a flash write on
// Core 0 stalls Core 1 while the cache is off — so role auto-binds wait in owRolePersistPending[] for
// owRoleService() in loop().
// Reads core0Busy as a courtesy but must NEVER set it — TempTask runs during active charging, and
// core0Busy gates AdjustFieldLearnMode, so setting it would freeze both control loops ~190-750 ms every 5 s.
void TempTask(void *parameter) {
  // NO watchdog registration - keep it separate from main loop watchdog

  static uint8_t scratchPad[9];
  static unsigned long lastTempRead = 0;
  static bool lastReadWasSuccess = true;  // false after any failure; drives 1s retry vs 5s normal poll
  static float lastValidTemp = -99;  // Track last valid reading (-99 = uninitialized)
  static bool sensorEnumerated = false;
  static uint8_t connFailStreak = 0;  // consecutive isConnected() misses before forcing re-enumeration
  static uint8_t roleMissStreak[TR_COUNT] = { 0, 0, 0 };  // BATT/EXTRA only: consecutive cycles a bound role gave no good read; 3 forces re-enumeration
  static bool engineWasRunning = false;

// Config byte derived from global resolution (9..12 bit -> 0x1F/0x3F/0x5F/0x7F); target is 12-bit (0x7F)
#define DS18B20_CFG_BYTE (0x1F | ((resolution - 9) << 5))

  for (;;) {
    unsigned long now = millis();

    lastTempTaskHeartbeat = now;

    if (otaInProgress) {
      tempTaskHandle = NULL;
      vTaskDelete(NULL);
    }

    if (hardwarePresent == 0) {
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }

    if (!bootHardwareInitDone) {  // initializeHardware() (Core 1) owns the boot bus search; two cores searching at once corrupt both
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    if (owScanRequested) {  // /get?owScan=1 or a role assignment: re-search the bus and rebind
      owScanRequested = false;
      sensorEnumerated = false;
    }

    if (!sensorEnumerated) {
      owEnumerate();
      if (tempRoleSlot[TR_ALT] >= 0) {
        sensorEnumerated = true;
      } else {
        tempEnumerateFailCount++;
        uint8_t presentNow = 0;
        for (uint8_t i = 0; i < owProbeCount; i++) if (owProbes[i].present) presentNow++;
        if (presentNow == 0) {
          // While charging (RPM>=200) the 20s control-staleness gate is armed, so a failed enumerate must
          // retry fast — stacked 5s delays here are what cut the field on a brief bus glitch. Engine-off
          // keeps the slow retry to save standby power (field is already off, staleness can't cut it).
          vTaskDelay(pdMS_TO_TICKS(RPM >= 200 ? 1000 : 5000));
          continue;
        }
        sensorEnumerated = true;  // no alternator probe but others answer: read them; the ALT miss streak keeps re-searching
      }
    }

    if (core0Busy) {
      tempCoreBusySkipCount++;
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    {  // engine start forces an immediate cycle so the first control ticks see a fresh alternator temperature
      bool engineRunning = (RPM >= 200);
      if (engineRunning && !engineWasRunning) lastTempRead = 0;
      engineWasRunning = engineRunning;
    }

    uint32_t pollInterval = lastReadWasSuccess ? 5000 : 1000;
    // Engine off: throttle unwatched reads to save standby power. Watched stays at 5s so the
    // client's 12s stale threshold keeps margin (a 10s poll grayed the gauge on any hiccup).
    if (RPM < 200) pollInterval = (events.count() > 0) ? 5000 : 60000;
    if (now - lastTempRead < pollInterval) {
      tempStaleSkipCount++;
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    bool thisReadSucceeded = false;
    bool writeMaxTemp = false;
    bool writeMaxTempAllTime = false;
    float pendingMaxTemp = 0.0f;
    float pendingMaxTempAllTime = 0.0f;
    bool roleGood[TR_COUNT] = { false, false, false };  // BATT/EXTRA: a good read landed this cycle (feeds roleMissStreak)
    bool altOk = tempRoleAssigned[TR_ALT];              // false = no ALT scratchpad read this cycle
    uint8_t otherPresent = 0;                           // present probes other than ALT
    for (uint8_t i = 0; i < owProbeCount; i++) {
      if (owProbes[i].present && tempRoleSlot[TR_ALT] != (int8_t)i) otherPresent++;
    }

    // A single isConnected() false-negative is usually OneWire noise (field-PWM coupling), not a real
    // disconnect — at a user-set 19kHz every 1-Wire bit slot spans switching edges (~5 CRC fails/min,
    // a third never recovering); at the 400Hz default edges are sparse and hits are rare (2 in 48min,
    // both recovered, 2026-08-21). Either way corruption is
    // probabilistic and an immediate re-roll usually lands clean (the CRC retry recovers ~2/3 the same
    // way). Retry in place before counting a miss. Tearing down enumeration on a counted miss forces the
    // slow re-enumerate path and its retry delays, which can stack past the 20s control-staleness gate
    // and needlessly cut the field. Only re-enumerate after 3 consecutive counted misses; otherwise
    // fast-retry (1s, post-failure) and keep the enumerated address so a good read clears staleness.
    if (altOk && !sensors.isConnected(tempDeviceAddress)) {
      vTaskDelay(pdMS_TO_TICKS(5));
      if (!sensors.isConnected(tempDeviceAddress)) {
        tempConnectedFailCount++;
        if (++connFailStreak >= 3) {
          lastValidTemp = -99;
          sensorEnumerated = false;
          connFailStreak = 0;
        }
        owMirrorAltResult(false, 0.0f);
        altOk = false;
      }
    }
    if (altOk) connFailStreak = 0;
    if (!altOk && otherPresent == 0) goto cleanup;  // nothing to convert for: same exit as a lone unplugged probe

    // One Skip ROM + Convert T for every probe on the bus. requestTemperatures() has no presence check and
    // isConversionComplete() reads an empty pulled-up bus as done, so a probe lost after the isConnected
    // pre-check surfaces at its readScratchPad (tempReadFailCount / CRC path), not as the timeout below.
    sensors.requestTemperatures();

    {
      // Poll for conversion complete instead of a fixed 2000ms block.
      // Timeout is resolution-based per library + small guard band. isConversionComplete() is wired-AND:
      // it reads 0 until the slowest probe on the bus finishes.
      unsigned long convStart = millis();
      unsigned long convTimeout = sensors.millisToWaitForConversion(resolution) + 50;

      while (!sensors.isConversionComplete()) {
        vTaskDelay(pdMS_TO_TICKS(10));
        lastTempTaskHeartbeat = millis();
        if (otaInProgress) goto cleanup;

        if (millis() - convStart > convTimeout) {
          tempRequestFailCount++;
          goto cleanup;
        }
      }
    }

    // ALT (alternator) scratchpad: the original single-probe path, unchanged. Its failure exits go to
    // altDone so the other probes are still read this cycle; an OTA start still leaves via cleanup.
    if (altOk) {
      if (sensors.readScratchPad(tempDeviceAddress, scratchPad)) {

        // CHECK 1: CRC validation
        uint8_t crc = OneWire::crc8(scratchPad, 8);
        if (crc != scratchPad[8]) {
          tempCrcFailCount++;

          // Immediate single retry
          vTaskDelay(pdMS_TO_TICKS(2));
          if (sensors.readScratchPad(tempDeviceAddress, scratchPad)) {
            uint8_t crc2 = OneWire::crc8(scratchPad, 8);
            if (crc2 != scratchPad[8]) {
              goto altDone;
            } else {
              tempCrcRecoveredCount++;
            }
          } else {
            tempReadFailCount++;
            goto altDone;
          }
        }

        // CHECK 2: Detect all-0xFF (disconnected sensor)
        bool allFF = true;
        for (int i = 0; i < 9; i++) {
          if (scratchPad[i] != 0xFF) {
            allFF = false;
            break;
          }
        }
        if (allFF) {
          tempAllFFCount++;
          lastValidTemp = -99;
          sensorEnumerated = false;
          goto altDone;
        }

        int16_t raw = (scratchPad[1] << 8) | scratchPad[0];
        float tempC = raw / 16.0f;
        float tempF = tempC * 1.8f + 32.0f;

        // CHECK 3: Power-on signature (0x0550 = 85°C = 185°F)
        if (raw == 0x0550) {
          tempPowerOn85Count++;
          goto altDone;
        }

        // CHECK 4: Verify resolution; auto-correct if EEPROM or reset changed it
        if (scratchPad[4] != DS18B20_CFG_BYTE) {
          tempResolutionFixCount++;
          sensors.setResolution(tempDeviceAddress, resolution);
          if (!sensors.requestTemperaturesByAddress(tempDeviceAddress)) {
            tempRequestFailCount++;
            goto altDone;
          }

          {
            // Poll for conversion complete after resolution fix; same approach as primary wait.
            unsigned long convStart2 = millis();
            unsigned long convTimeout2 = sensors.millisToWaitForConversion(resolution) + 50;

            while (!sensors.isConversionComplete()) {
              vTaskDelay(pdMS_TO_TICKS(10));
              lastTempTaskHeartbeat = millis();
              if (otaInProgress) goto cleanup;

              if (millis() - convStart2 > convTimeout2) {
                tempRequestFailCount++;
                goto altDone;
              }
            }
          }

          if (!sensors.readScratchPad(tempDeviceAddress, scratchPad)) {
            tempRereadFailCount++;
            goto altDone;
          }
          if (OneWire::crc8(scratchPad, 8) != scratchPad[8]) {
            tempResolutionFixCrcFailCount++;
            goto altDone;
          }
          // Recalculate from corrected scratchpad
          raw = (scratchPad[1] << 8) | scratchPad[0];
          tempC = raw / 16.0f;
          tempF = tempC * 1.8f + 32.0f;
        }

        // CHECK 5: Sanity range
        if (tempF > -50 && tempF < 300) {
          unsigned long ageBeforeThisGoodRead = millis() - tempLastSuccessMillis;
          tempReadSuccessCount++;
          tempLastGoodF = tempF;
          tempLastSuccessMillis = millis();
          // Snapshot failure counters at this good read so a later staleness trip can report deltas.
          tempFailSnapConn = tempConnectedFailCount;
          tempFailSnapEnum = tempEnumerateFailCount;
          tempFailSnapCrc = tempCrcFailCount;
          tempFailSnapReq = tempRequestFailCount;
          tempFailSnapRead = tempReadFailCount;
          tempFailSnapAllFF = tempAllFFCount;

          AlternatorTemperatureF = tempF;
          lastValidTemp = tempF;
          tempTaskHealthy = true;
          thisReadSucceeded = true;
          MARK_FRESH(IDX_ALTERNATOR_TEMP);
          wmIgnUpdate(wmIgn_altTempF, AlternatorTemperatureF);  // ignition-cycle watermark

          if (AlternatorTemperatureF > MaxAlternatorTemperatureF) {
            pendingMaxTemp = AlternatorTemperatureF;
            writeMaxTemp = true;
          }
          if (AlternatorTemperatureF > MaxAlternatorTemperatureF_AllTime) {
            pendingMaxTempAllTime = AlternatorTemperatureF;
            writeMaxTempAllTime = true;
          }
        } else {
          tempOutOfRangeCount++;
        }
      } else {
        tempReadFailCount++;
      }
    }

altDone:
    if (altOk) owMirrorAltResult(thisReadSucceeded, AlternatorTemperatureF);  // every ALT exit above lands here except the OTA goto cleanup
    // Every other PRESENT probe in the registry, bound or not, so the assignment card shows live values.
    // A role whose enable setting is 0 is read into the registry but does not touch its role global or
    // freshness stamp. The ALT slot was handled above and never re-read here.
    {
      const int8_t altSlot = tempRoleSlot[TR_ALT];
      for (uint8_t i = 0; i < owProbeCount; i++) {
        if (!owProbes[i].present || altSlot == (int8_t)i) continue;
        lastTempTaskHeartbeat = millis();
        bool good = false;
        bool absent = false;
        float tempF = 0.0f;
        if (sensors.readScratchPad(owProbes[i].addr, scratchPad)) {
          bool allFF = true;
          for (int b = 0; b < 9; b++) {
            if (scratchPad[b] != 0xFF) { allFF = false; break; }
          }
          bool crcOk = !allFF && (OneWire::crc8(scratchPad, 8) == scratchPad[8]);
          if (!allFF && !crcOk) {  // one in-place re-roll, same rationale as the ALT CRC retry
            vTaskDelay(pdMS_TO_TICKS(2));
            crcOk = sensors.readScratchPad(owProbes[i].addr, scratchPad) && (OneWire::crc8(scratchPad, 8) == scratchPad[8]);
          }
          if (allFF) {
            absent = true;  // nothing answered this ROM: gone from the bus (all-0xFF never passes CRC, so it is tested first)
          } else if (crcOk) {
            int16_t raw = (scratchPad[1] << 8) | scratchPad[0];
            if (raw != 0x0550) {  // 0x0550 = power-on 85C signature: skip this cycle
              if (scratchPad[4] != DS18B20_CFG_BYTE) sensors.setResolution(owProbes[i].addr, resolution);  // this device only, no re-convert; the next cycle reads at 12-bit
              tempF = (raw / 16.0f) * 1.8f + 32.0f;
              good = (tempF > -50 && tempF < 300);
            }
          }
        }
        portENTER_CRITICAL(&owMux);
        if (good) {
          owProbes[i].lastF = tempF;
          owProbes[i].lastGoodMs = millis();
          owProbes[i].okCount++;
        } else {
          owProbes[i].failCount++;
          if (absent) owProbes[i].present = false;
        }
        portEXIT_CRITICAL(&owMux);
        if (absent) sensorEnumerated = false;  // re-search next cycle, as the ALT all-0xFF path does; a noise glitch costs one cycle, not three
        if (!good) continue;
        if (tempRoleSlot[TR_BATT] == (int8_t)i) {
          roleGood[TR_BATT] = true;
          if (battTempProbeEnable == 1) {
            BatteryTempProbeF = tempF;
            MARK_FRESH(IDX_BATT_TEMP_PROBE);
            wmIgnUpdate(wmIgn_battTempF, tempF);
          }
        }
        if (tempRoleSlot[TR_EXTRA] == (int8_t)i) {
          roleGood[TR_EXTRA] = true;
          if (extraTempProbeEnable == 1) {
            ExtraTempF = tempF;
            MARK_FRESH(IDX_EXTRA_TEMP);
            wmIgnUpdate(wmIgn_extraTempF, tempF);
          }
        }
      }
    }

cleanup:
    // No alternator role bound (multi-probe bus awaiting assignment, or a thermistor alternator with 1-Wire
    // battery/extra probes only): nothing on this bus is expected to feed the alternator path, so the cycle
    // keeps the normal 5 s cadence and the consecutive-failure alert stays quiet.
    const bool altExpected = tempRoleAssigned[TR_ALT];
    lastReadWasSuccess = thisReadSucceeded || !altExpected;
    lastTempRead = millis();
    lastTempTaskHeartbeat = millis();

    // BATT/EXTRA re-enumeration streaks (ALT's is connFailStreak): a bound role with no good read for 3
    // consecutive cycles forces a bus re-search. Never touches the cadence or the other roles.
    for (int r = TR_BATT; r < TR_COUNT; r++) {
      if (!tempRoleAssigned[r] || roleGood[r]) {
        roleMissStreak[r] = 0;
      } else if (++roleMissStreak[r] >= 3) {
        roleMissStreak[r] = 0;
        sensorEnumerated = false;
      }
    }

    // Alert on persistent sensor failure — 5 consecutive non-success reads
    {
      static uint8_t tempConsecFail = 0;
      static unsigned long lastTempFailAlert = 0;
      if (thisReadSucceeded || !altExpected) {
        tempConsecFail = 0;
      } else {
        tempConsecFail++;
        if (tempConsecFail >= 5 && (millis() - lastTempFailAlert > 60000)) {
          queueConsoleMessage("DS18B20 WARNING: 5+ consecutive read failures — check sensor connection");
          lastTempFailAlert = millis();
          tempConsecFail = 0;
        }
      }
    }

    if (writeMaxTemp) {
      MaxAlternatorTemperatureF = pendingMaxTemp;
    }
    if (writeMaxTempAllTime) {
      MaxAlternatorTemperatureF_AllTime = pendingMaxTempAllTime;
    }

    if (otaInProgress) {
      tempTaskHandle = NULL;
      vTaskDelete(NULL);
    }
  }
}

// Core-0 HTTPS task per-operation wall-clock time (ms): last cloud op + worst since Reset Peak Values.
// Wall-clock is correct here — a network transfer's real cost includes the time on the wire.
uint32_t httpsUploadLastMs = 0;
uint32_t httpsUploadWorstMs = 0;

void httpsTask(void *param) {
  if (otaInProgress) {
    // Returning from a FreeRTOS task function aborts in vPortTaskWrapper (panic-reboot) —
    // self-delete cleanly instead, matching TempTask. Unreachable today, latent trap otherwise.
    httpsTaskHandle = NULL;
    vTaskDelete(NULL);
    return;  // never reached — silences no-return analysis
  }
  esp_task_wdt_add(NULL);
  // Serial.println("HTTPS Task started on Core 0");
  // Serial.printf("sizeof(HttpsRequest) = %d bytes\n", sizeof(HttpsRequest));
  // Serial.printf("Queue item size = %d bytes\n", sizeof(HttpsRequest));

  static int consecutiveFailures = 0;
  const int MAX_CONSECUTIVE_FAILURES = 5;
  static unsigned long uploadsSuspendedUntil = 0;
  for (;;) {
    if (otaInProgress) {
      // Leave between ops, never mid-op: prepareForOTA() waits for this handle to clear instead of
      // vTaskDelete()-ing a task that may be inside a DNS lookup or TLS read with lwIP/mbedTLS holding
      // pointers into this stack. Unsubscribe first or the task WDT keeps expecting our feed.
      esp_task_wdt_delete(NULL);
      httpsTaskHandle = NULL;
      vTaskDelete(NULL);
    }
    if (uploadsSuspendedUntil && (int32_t)(millis() - uploadsSuspendedUntil) < 0) {  // rollover-safe "now < deadline"
      vTaskDelay(pdMS_TO_TICKS(500));
      esp_task_wdt_reset();
      continue;
    }

    esp_task_wdt_reset();

    HttpsRequest request;
    if (xQueueReceive(httpsQueue, &request, pdMS_TO_TICKS(1000))) {

      if (request.type == HTTPS_UPLOAD_PAYLOAD || request.type == HTTPS_UPLOAD_CONFIG || request.type == HTTPS_UPLOAD_BOATPERF || request.type == HTTPS_UPLOAD_ALTHEALTH || request.type == HTTPS_UPLOAD_CX_LEDGER || request.type == HTTPS_UPLOAD_USAGE) {
        size_t payloadLen = request.payload ? strlen(request.payload) : 0;
        // payloadCap is the ps_malloc'd capacity (NOT sizeof a pointer); free + skip on any bad payload.
        if (!request.payload || payloadLen >= request.payloadCap || payloadLen == 0) {
          if (request.payload) free(request.payload);
          continue;
        }
        if (request.payload[0] != '{') {
          free(request.payload);
          continue;
        }
      }

      core0Busy = true;
      unsigned long opStart = millis();
      bool opSuccess = false;

      switch (request.type) {
        case HTTPS_UPLOAD_PAYLOAD:
          opSuccess = executeUploadPayload(request.payload);
          break;
        case HTTPS_UPLOAD_CONFIG:
          opSuccess = executeUploadConfig(request.payload);
          break;
        case HTTPS_UPLOAD_BOATPERF:
          opSuccess = executeUploadBoatPerf(request.payload);
          break;
        case HTTPS_UPLOAD_ALTHEALTH:
          opSuccess = executeUploadAltHealth(request.payload);
          break;
        case HTTPS_UPLOAD_CX_LEDGER:
          opSuccess = executeUploadCxLedger(request.payload);  // sets cxLedgerUpState itself (2 = trim due, -1 = retry)
          break;
        case HTTPS_UPLOAD_USAGE:
          opSuccess = executeUploadUsage(request.payload);
          break;
        case HTTPS_FETCH_WEATHER:
          executeFetchWeatherData();
          opSuccess = true;
          break;
        case HTTPS_UPDATE_FW_VERSION:
          executeUpdateFirmwareVersion();
          opSuccess = true;
          break;
        case HTTPS_CHECK_FORCED_UPDATE:
          executeCheckForcedUpdate();
          opSuccess = true;
          break;
        case HTTPS_CLEAR_FORCED_UPDATE:
          executeClearForcedUpdate();
          opSuccess = true;
          break;
        case HTTPS_GET_PENDING_CONFIG:
          executeGetPendingConfig();
          opSuccess = true;
          break;
        case HTTPS_CLEAR_PENDING_CONFIG:
          executeClearPendingConfig();
          opSuccess = true;
          break;
        case HTTPS_RESET_RPM_AXIS:
          executeResetRpmAxis();
          opSuccess = true;
          break;
        case HTTPS_CLOUD_OP:
          executeCloudOp();
          opSuccess = true;
          break;
      }

      unsigned long opDuration = millis() - opStart;
      httpsUploadLastMs = (uint32_t)opDuration;                                 // Core-0 cloud op time, dashboard Core-0 section
      if ((uint32_t)opDuration > httpsUploadWorstMs) httpsUploadWorstMs = (uint32_t)opDuration;

      if (opDuration > 9000) {
        Serial.printf("WARNING: Operation took %lums (>9s limit)\n", opDuration);
      }

      if (request.type == HTTPS_UPLOAD_PAYLOAD) {
        if (opSuccess) {
          consecutiveFailures = 0;
        } else {
          // Transport-level failures (WiFi drop, connect fail, send fail, read
          // timeout) return from executeUploadPayload before its response
          // handling, leaving the buffered in-flight marker set — clear it here
          // or uploadBufferedRecords() refuses to queue forever and the full
          // ring silently drops every new snapshot. The record itself stays in
          // the ring for retry. (Idempotent: parsed-response failure paths
          // already cleared it.)
          if (lastUploadWasBuffered && sensorRingInFlightIndex >= 0) {
            sensorRingInFlightIndex = -1;
            lastUploadWasBuffered = false;
          }
        }
        if (!opSuccess && lastHttpResponseCode != 400 && lastHttpResponseCode != 401) {
          // Don't count 400/401 as failures - they indicate bad data that gets auto-deleted
          consecutiveFailures++;
          //Serial.printf("Consecutive failures: %d/%d\n", consecutiveFailures, MAX_CONSECUTIVE_FAILURES);MAX_CONSECUTIVE_FAILURES);

          // Add delay after failure to prevent rapid retries
          // Serial.println("Upload failed - delaying 3s before next attempt");
          unsigned long delayStart = millis();
          while (millis() - delayStart < 3000) {
            esp_task_wdt_reset();
            delay(100);
          }

          if (consecutiveFailures >= MAX_CONSECUTIVE_FAILURES) {
            uploadsSuspendedUntil = millis() + 30000;
            Serial.println("ERROR: Too many failures - suspending uploads for 30s");
            queueConsoleMessage("Cloud sync paused, too many failures, retry in 30s");
            consecutiveFailures = 0;
          }
        }
      }

      lastHttpsOperationTime = millis();
      core0Busy = false;  // Clear lock AFTER operation completes
      esp_task_wdt_reset();

      if (request.payload) { free(request.payload); request.payload = nullptr; }  // release the PSRAM payload we own
    }
  }
}
// SENSOR AGGREGATION FUNCTIONS
// Init at boot: reset the in-progress accumulator windows. The PSRAM ring
// itself is restored by restoreSensorRingFromLittleFS() right after this.
// Also one-shot scrub of the legacy /sensor_buffer/ dir from pre-May2026
// firmware — no-op on fresh devices.
void initSensorBuffer() {
  resetSensorWindow();
  resetAccelWindow();

  const char *legacyDir = "/sensor_buffer";
  if (fsExists(legacyDir)) {
    fsTakeLock();
    File root = LittleFS.open(legacyDir);
    if (root && root.isDirectory()) {
      File f = root.openNextFile();
      while (f) {
        esp_task_wdt_reset();
        if (!f.isDirectory()) {
          String p = f.path();
          f.close();
          LittleFS.remove(p.c_str());
        }
        f = root.openNextFile();
      }
      root.close();
      LittleFS.rmdir(legacyDir);
    }
    fsReleaseLock();
    Serial.println("Scrubbed legacy /sensor_buffer/ dir");
  }
}

void resetSensorWindow() {
  currentWindow->battVolt_min = 999900;
  currentWindow->battVolt_max = 0;
  currentWindow->battVolt_area_v_us = 0;
  currentWindow->battVolt_valid_us = 0;

  currentWindow->battCurr_min = 999900;
  currentWindow->battCurr_max = -999900;
  currentWindow->battCurr_area_v_us = 0;
  currentWindow->battCurr_valid_us = 0;

  currentWindow->altCurr_min = 999900;
  currentWindow->altCurr_max = 0;
  currentWindow->altCurr_area_v_us = 0;
  currentWindow->altCurr_valid_us = 0;

  currentWindow->victronCurr_min = 999900;
  currentWindow->victronCurr_max = -999900;
  currentWindow->victronCurr_area_v_us = 0;
  currentWindow->victronCurr_valid_us = 0;

  currentWindow->soc_min = 999900;
  currentWindow->soc_max = 0;
  currentWindow->soc_area_v_us = 0;
  currentWindow->soc_valid_us = 0;

  currentWindow->baro_min = 999900;
  currentWindow->baro_max = 0;
  currentWindow->baro_area_v_us = 0;
  currentWindow->baro_valid_us = 0;

  currentWindow->altTemp_min = 999900;
  currentWindow->altTemp_max = -999900;
  currentWindow->altTemp_area_v_us = 0;
  currentWindow->altTemp_valid_us = 0;

  currentWindow->tempTherm_min = 999900;
  currentWindow->tempTherm_max = -999900;
  currentWindow->tempTherm_area_v_us = 0;
  currentWindow->tempTherm_valid_us = 0;

  currentWindow->ambTemp_min = 999900;
  currentWindow->ambTemp_max = -999900;
  currentWindow->ambTemp_area_v_us = 0;
  currentWindow->ambTemp_valid_us = 0;

  currentWindow->rpm_min = 999999;
  currentWindow->rpm_max = 0;
  currentWindow->rpm_area_v_us = 0;
  currentWindow->rpm_valid_us = 0;

  currentWindow->wifiStr_min = 999900;
  currentWindow->wifiStr_max = -999900;
  currentWindow->wifiStr_area_v_us = 0;
  currentWindow->wifiStr_valid_us = 0;

  currentWindow->dutyCycle_min = 999900;
  currentWindow->dutyCycle_max = 0;
  currentWindow->dutyCycle_area_v_us = 0;
  currentWindow->dutyCycle_valid_us = 0;

  currentWindow->active_us = 0;
  currentWindow->battVolt_on_area_v_us = 0;
  currentWindow->battCurr_on_area_v_us = 0;
  currentWindow->altCurr_on_area_v_us = 0;
  currentWindow->victronCurr_on_area_v_us = 0;
  currentWindow->dutyCycle_on_area_v_us = 0;

  currentWindow->altZero_min = 999900;
  currentWindow->altZero_max = -999900;
  currentWindow->altZero_area_v_us = 0;
  currentWindow->altZero_valid_us = 0;

  currentWindow->sog_min = 999900;
  currentWindow->sog_max = 0;
  currentWindow->sogSust1m_max = 0;
  currentWindow->sogSustPhone = 0;
  currentWindow->sog_area_v_us = 0;
  currentWindow->sog_valid_us = 0;

  currentWindow->cog_min = 999900;
  currentWindow->cog_max = 0;
  currentWindow->cog_area_v_us = 0;
  currentWindow->cog_valid_us = 0;

  currentWindow->heading_min = 999900;
  currentWindow->heading_max = 0;
  currentWindow->heading_area_v_us = 0;
  currentWindow->heading_valid_us = 0;

  currentWindow->aws_min = 999900;
  currentWindow->aws_max = 0;
  currentWindow->aws_area_v_us = 0;
  currentWindow->aws_valid_us = 0;

  currentWindow->awa_min = 999900;
  currentWindow->awa_max = 0;
  currentWindow->awa_area_v_us = 0;
  currentWindow->awa_valid_us = 0;

  currentWindow->tws_min = 999900;
  currentWindow->tws_max = 0;
  currentWindow->tws_area_v_us = 0;
  currentWindow->tws_valid_us = 0;

  currentWindow->twa_min = 999900;
  currentWindow->twa_max = 0;
  currentWindow->twa_area_v_us = 0;
  currentWindow->twa_valid_us = 0;

  currentWindow->leeway_min = 999900;
  currentWindow->leeway_max = -999900;
  currentWindow->leeway_area_v_us = 0;
  currentWindow->leeway_valid_us = 0;

  currentWindow->vmg_min = 999900;
  currentWindow->vmg_max = -999900;
  currentWindow->vmg_area_v_us = 0;
  currentWindow->vmg_valid_us = 0;

  currentWindow->lat_current = 0;
  currentWindow->lon_current = 0;

  currentWindow->uTargetAmps_min = 999900;
  currentWindow->uTargetAmps_max = -999900;
  currentWindow->uTargetAmps_area_v_us = 0;
  currentWindow->uTargetAmps_valid_us = 0;

  currentWindow->tempMargin_min = 999900;
  currentWindow->tempMargin_max = -999900;
  currentWindow->tempMargin_area_v_us = 0;
  currentWindow->tempMargin_valid_us = 0;

  currentWindow->lastUpdateTime_us = micros();
  currentWindow->windowStartTime = millis();
}

void resetAccelWindow() {
  // Raw accel (signed - can be negative, scaled by 1000)
  imuWindow->accel_x_min = 999900;
  imuWindow->accel_x_max = -999900;
  imuWindow->accel_x_area_v_us = 0;
  imuWindow->accel_x_valid_us = 0;

  imuWindow->accel_y_min = 999900;
  imuWindow->accel_y_max = -999900;
  imuWindow->accel_y_area_v_us = 0;
  imuWindow->accel_y_valid_us = 0;

  imuWindow->accel_z_min = 999900;
  imuWindow->accel_z_max = -999900;
  imuWindow->accel_z_area_v_us = 0;
  imuWindow->accel_z_valid_us = 0;

  // Raw gyro (signed - can be negative, scaled by 100)
  imuWindow->gyro_x_min = 999900;
  imuWindow->gyro_x_max = -999900;
  imuWindow->gyro_x_area_v_us = 0;
  imuWindow->gyro_x_valid_us = 0;

  imuWindow->gyro_y_min = 999900;
  imuWindow->gyro_y_max = -999900;
  imuWindow->gyro_y_area_v_us = 0;
  imuWindow->gyro_y_valid_us = 0;

  imuWindow->gyro_z_min = 999900;
  imuWindow->gyro_z_max = -999900;
  imuWindow->gyro_z_area_v_us = 0;
  imuWindow->gyro_z_valid_us = 0;

  // Calculated metrics - heel/pitch (signed, scaled by 100)
  imuWindow->heel_min = 999900;
  imuWindow->heel_max = -999900;
  imuWindow->heel_area_v_us = 0;
  imuWindow->heel_valid_us = 0;

  imuWindow->pitch_min = 999900;
  imuWindow->pitch_max = -999900;
  imuWindow->pitch_area_v_us = 0;
  imuWindow->pitch_valid_us = 0;

  // Calculated metrics - accelerations (signed, scaled by 1000)
  imuWindow->vertical_accel_min = 999900;
  imuWindow->vertical_accel_max = -999900;
  imuWindow->vertical_accel_area_v_us = 0;
  imuWindow->vertical_accel_valid_us = 0;

  imuWindow->total_accel_min = 999900;
  imuWindow->total_accel_max = 0;  // Total accel is always positive
  imuWindow->total_accel_area_v_us = 0;
  imuWindow->total_accel_valid_us = 0;

  // Vibration energy (scaled by 1000000)
  imuWindow->hf_vibe_min = 999900000;
  imuWindow->hf_vibe_max = 0;
  imuWindow->hf_vibe_area_v_us = 0;
  imuWindow->hf_vibe_valid_us = 0;

  // 60s rolling metrics
  imuWindow->heel_change_60s = 0;
  imuWindow->heel_deviation_60s = 0;
  imuWindow->pitch_change_60s = 0;
  imuWindow->pitch_deviation_60s = 0;

  imuWindow->wave_period = -1000;  // -1.0s scaled

  // Period counters
  imuWindow->slam_count = 0;
  imuWindow->slam_peak_max = 0;

  // Timing
  imuWindow->lastUpdateTime_us = micros();
  imuWindow->windowStartTime = millis();
}

void updateSensorWindow() {
  uint64_t now_us = micros();
  uint64_t delta_us;

  if (currentWindow->lastUpdateTime_us == 0) {
    // First call after reset - no delta yet
    delta_us = 0;
  } else {
    delta_us = now_us - currentWindow->lastUpdateTime_us;
  }

  currentWindow->lastUpdateTime_us = now_us;

  // If this is the first call (delta_us == 0), skip accumulation but still update min/max
  bool shouldAccumulate = (delta_us > 0);

  // Engine-running-weighted accumulators — engine spinning (tach gate), NOT field-on,
  // so the on-avg stays stable across bulk/absorption/float.
  bool engineOn = engineSpinning();

  int32_t battVolt = (int32_t)(getBatteryVoltage() * 100.0);
  if (battVolt < currentWindow->battVolt_min) currentWindow->battVolt_min = battVolt;
  if (battVolt > currentWindow->battVolt_max) currentWindow->battVolt_max = battVolt;
  if (shouldAccumulate) {
    currentWindow->battVolt_area_v_us += (int64_t)battVolt * delta_us;
    currentWindow->battVolt_valid_us += delta_us;
    if (engineOn) {
      currentWindow->active_us += delta_us;
      currentWindow->battVolt_on_area_v_us += (int64_t)battVolt * delta_us;
    }
  }

  // Battery current - CONDITIONAL on the shunt (same pattern as baro/alt-temp below). No shunt ->
  // Bcur is INA input noise and SOC_percent is a frozen NVS value; leaving the window untouched
  // keeps *_valid_us at 0, which the uploader turns into JSON null instead of invented numbers.
  if (HAS_BATT_SHUNT) {
    int32_t battCurr = (int32_t)(Bcur * 100.0);
    if (battCurr < currentWindow->battCurr_min) currentWindow->battCurr_min = battCurr;
    if (battCurr > currentWindow->battCurr_max) currentWindow->battCurr_max = battCurr;
    if (shouldAccumulate) {
      currentWindow->battCurr_area_v_us += (int64_t)battCurr * delta_us;
      currentWindow->battCurr_valid_us += delta_us;
      if (engineOn) currentWindow->battCurr_on_area_v_us += (int64_t)battCurr * delta_us;
    }
  }

  // Alternator current
  int32_t altCurr = (int32_t)(MeasuredAmps * 100.0);
  if (altCurr < currentWindow->altCurr_min) currentWindow->altCurr_min = altCurr;
  if (altCurr > currentWindow->altCurr_max) currentWindow->altCurr_max = altCurr;
  if (shouldAccumulate) {
    currentWindow->altCurr_area_v_us += (int64_t)altCurr * delta_us;
    currentWindow->altCurr_valid_us += delta_us;
    if (engineOn) currentWindow->altCurr_on_area_v_us += (int64_t)altCurr * delta_us;
  }

  if (IS_SEEN(IDX_VICTRON_CURRENT) && !IS_STALE(IDX_VICTRON_CURRENT)) {   // no VE.Direct shunt fitted must read absent, not a measured 0.0 A
    int32_t victronCurr = (int32_t)(VictronCurrent * 100.0);
    if (victronCurr < currentWindow->victronCurr_min) currentWindow->victronCurr_min = victronCurr;
    if (victronCurr > currentWindow->victronCurr_max) currentWindow->victronCurr_max = victronCurr;
    if (shouldAccumulate) {
      currentWindow->victronCurr_area_v_us += (int64_t)victronCurr * delta_us;
      currentWindow->victronCurr_valid_us += delta_us;
      if (engineOn) currentWindow->victronCurr_on_area_v_us += (int64_t)victronCurr * delta_us;
    }
  }

  if (HAS_BATT_SHUNT) {
    int32_t soc = SOC_percent;
    if (soc < currentWindow->soc_min) currentWindow->soc_min = soc;
    if (soc > currentWindow->soc_max) currentWindow->soc_max = soc;
    if (shouldAccumulate) {
      currentWindow->soc_area_v_us += (int64_t)soc * delta_us;
      currentWindow->soc_valid_us += delta_us;
    }
  }

  // Barometric pressure - CONDITIONAL on freshness
  if (!IS_STALE(IDX_BARO_PRESSURE) && isfinite(baroPressure)) {
    int32_t baro = (int32_t)(baroPressure * 100.0);
    if (baro < currentWindow->baro_min) currentWindow->baro_min = baro;
    if (baro > currentWindow->baro_max) currentWindow->baro_max = baro;
    if (shouldAccumulate) {
      currentWindow->baro_area_v_us += (int64_t)baro * delta_us;
      currentWindow->baro_valid_us += delta_us;
    }
  }

  // Alternator temperature - CONDITIONAL on validity (matches thermistor pattern below).
  // NaN (no OneWire reading yet) and the thermistor's -99 marker must never touch the
  // window, or the long-term plot's min envelope dips to -99 after every boot.
  if (TempSource == 0) {
    TempToUse = AlternatorTemperatureF;
  } else {
    TempToUse = temperatureThermistor;
  }
  if (!isnan(TempToUse) && TempToUse != -99) {
    int32_t altTemp = (int32_t)(TempToUse * 100.0);
    if (altTemp < currentWindow->altTemp_min) currentWindow->altTemp_min = altTemp;
    if (altTemp > currentWindow->altTemp_max) currentWindow->altTemp_max = altTemp;
    if (shouldAccumulate) {
      currentWindow->altTemp_area_v_us += (int64_t)altTemp * delta_us;
      currentWindow->altTemp_valid_us += delta_us;
    }
  }

  // Thermistor temperature - CONDITIONAL on freshness (matches baro/ambient pattern)
  if (!IS_STALE(IDX_THERMISTOR_TEMP) && temperatureThermistor != -99) {
    int32_t tempTherm = (int32_t)(temperatureThermistor * 100.0);
    if (tempTherm < currentWindow->tempTherm_min) currentWindow->tempTherm_min = tempTherm;
    if (tempTherm > currentWindow->tempTherm_max) currentWindow->tempTherm_max = tempTherm;
    if (shouldAccumulate) {
      currentWindow->tempTherm_area_v_us += (int64_t)tempTherm * delta_us;
      currentWindow->tempTherm_valid_us += delta_us;
    }
  }

  // Ambient temperature - CONDITIONAL on freshness
  if (!IS_STALE(IDX_AMBIENT_TEMP) && isfinite(ambientTemp)) {
    int32_t ambTemp = (int32_t)(ambientTemp * 100.0);
    if (ambTemp < currentWindow->ambTemp_min) currentWindow->ambTemp_min = ambTemp;
    if (ambTemp > currentWindow->ambTemp_max) currentWindow->ambTemp_max = ambTemp;
    if (shouldAccumulate) {
      currentWindow->ambTemp_area_v_us += (int64_t)ambTemp * delta_us;
      currentWindow->ambTemp_valid_us += delta_us;
    }
  }

  int32_t rpm = (int32_t)RPM;
  if (rpm < currentWindow->rpm_min) currentWindow->rpm_min = rpm;
  if (rpm > currentWindow->rpm_max) currentWindow->rpm_max = rpm;
  if (shouldAccumulate) {
    currentWindow->rpm_area_v_us += (int64_t)rpm * delta_us;
    currentWindow->rpm_valid_us += delta_us;
  }

  int32_t wifiStr = WifiStrength;
  if (wifiStr < currentWindow->wifiStr_min) currentWindow->wifiStr_min = wifiStr;
  if (wifiStr > currentWindow->wifiStr_max) currentWindow->wifiStr_max = wifiStr;
  if (shouldAccumulate) {
    currentWindow->wifiStr_area_v_us += (int64_t)wifiStr * delta_us;
    currentWindow->wifiStr_valid_us += delta_us;
  }

  int32_t duty = (int32_t)(dutyCycle * 100.0);
  if (duty < currentWindow->dutyCycle_min) currentWindow->dutyCycle_min = duty;
  if (duty > currentWindow->dutyCycle_max) currentWindow->dutyCycle_max = duty;
  if (shouldAccumulate) {
    currentWindow->dutyCycle_area_v_us += (int64_t)duty * delta_us;
    currentWindow->dutyCycle_valid_us += delta_us;
    if (engineOn) currentWindow->dutyCycle_on_area_v_us += (int64_t)duty * delta_us;
  }

  int32_t altZero = (int32_t)(DynamicAltCurrentZero * 100.0);
  if (altZero < currentWindow->altZero_min) currentWindow->altZero_min = altZero;
  if (altZero > currentWindow->altZero_max) currentWindow->altZero_max = altZero;
  if (shouldAccumulate) {
    currentWindow->altZero_area_v_us += (int64_t)altZero * delta_us;
    currentWindow->altZero_valid_us += delta_us;
  }

  int32_t targetAmps = (int32_t)(uTargetAmps * 100.0);
  if (targetAmps < currentWindow->uTargetAmps_min) currentWindow->uTargetAmps_min = targetAmps;
  if (targetAmps > currentWindow->uTargetAmps_max) currentWindow->uTargetAmps_max = targetAmps;
  if (shouldAccumulate) {
    currentWindow->uTargetAmps_area_v_us += (int64_t)targetAmps * delta_us;
    currentWindow->uTargetAmps_valid_us += delta_us;
  }

  float tempMargin = TemperatureLimitF - TempToUse;
  int32_t tempMarginScaled = (int32_t)(tempMargin * 100.0);
  if (tempMarginScaled < currentWindow->tempMargin_min) currentWindow->tempMargin_min = tempMarginScaled;
  if (tempMarginScaled > currentWindow->tempMargin_max) currentWindow->tempMargin_max = tempMarginScaled;
  if (shouldAccumulate) {
    currentWindow->tempMargin_area_v_us += (int64_t)tempMarginScaled * delta_us;
    currentWindow->tempMargin_valid_us += delta_us;
  }

  // A speed-source switch does not empty the 60-s ring in updateSustainedSpeed(), so for one window
  // after it sogSust1m still carries the OLD source's samples while the stamp below would name the
  // NEW one — and a mislabelled cloud speed record is permanent. Hold the record off for a full ring
  // window after any change. Detected by watching the variable rather than by an invalidator call at
  // each writer: the ring and its NMEA feeder live elsewhere, and a future writer would not know to
  // call one.
  static uint8_t sustSrcSeen = 0xFF;   // 0xFF = first pass, establish the baseline without a hold-off
  static uint32_t sustSrcChangedMs = 0;
  static bool sustSrcHold = false;
  if ((uint8_t)currentSpeedSource != sustSrcSeen) {
    if (sustSrcSeen != 0xFF) { sustSrcChangedMs = millis(); sustSrcHold = true; }
    sustSrcSeen = (uint8_t)currentSpeedSource;
  }
  if (sustSrcHold && (millis() - sustSrcChangedMs) >= SUST_WINDOW_MS) sustSrcHold = false;

  // Speed over ground - CONDITIONAL on freshness
  if (IS_SEEN(IDX_SOG_NMEA) && !IS_STALE(IDX_SOG_NMEA)) {
    int32_t sog = (int32_t)(SOGNMEA * 100.0);
    if (sog < currentWindow->sog_min) currentWindow->sog_min = sog;
    if (sog > currentWindow->sog_max) currentWindow->sog_max = sog;
    // 0 while the 60-s window is uncovered — nothing to record then.
    int32_t sust = (int32_t)(sogSust1m * 100.0);
    if (!sustSrcHold && sust > currentWindow->sogSust1m_max) {
      currentWindow->sogSust1m_max = sust;
      // Stamp the standing max's source so the cloud speed board can disqualify
      // phone-GPS-sourced records (the hold-off above is what keeps the ring single-source).
      currentWindow->sogSustPhone = (currentSpeedSource == GPS_PHONE) ? 1 : 0;
    }
    if (shouldAccumulate) {
      currentWindow->sog_area_v_us += (int64_t)sog * delta_us;
      currentWindow->sog_valid_us += delta_us;
    }
  }

  // Course over ground - CONDITIONAL on freshness
  if (IS_SEEN(IDX_COG_NMEA) && !IS_STALE(IDX_COG_NMEA)) {
    int32_t cog = (int32_t)(COGNMEA * 100.0);
    if (cog < currentWindow->cog_min) currentWindow->cog_min = cog;
    if (cog > currentWindow->cog_max) currentWindow->cog_max = cog;
    if (shouldAccumulate) {
      currentWindow->cog_area_v_us += (int64_t)cog * delta_us;
      currentWindow->cog_valid_us += delta_us;
    }
  }

  if (IS_SEEN(IDX_HEADING_NMEA) && !IS_STALE(IDX_HEADING_NMEA)) {
    int32_t heading = (int32_t)(HeadingNMEA * 100.0);
    if (heading < currentWindow->heading_min) currentWindow->heading_min = heading;
    if (heading > currentWindow->heading_max) currentWindow->heading_max = heading;
    if (shouldAccumulate) {
      currentWindow->heading_area_v_us += (int64_t)heading * delta_us;
      currentWindow->heading_valid_us += delta_us;
    }
  }

  // Apparent wind - CONDITIONAL on freshness. Ungated, a boat with no wind instrument
  // accumulated the 0.0 initialiser as measured data and the cloud drew a solid 0-kt line.
  if (IS_SEEN(IDX_APPARENT_WIND_SPEED) && !IS_STALE(IDX_APPARENT_WIND_SPEED)) {
    int32_t aws = (int32_t)(ApparentWindSpeedNMEA * 100.0);
    if (aws < currentWindow->aws_min) currentWindow->aws_min = aws;
    if (aws > currentWindow->aws_max) currentWindow->aws_max = aws;
    if (shouldAccumulate) {
      currentWindow->aws_area_v_us += (int64_t)aws * delta_us;
      currentWindow->aws_valid_us += delta_us;
    }
  }

  if (IS_SEEN(IDX_APPARENT_WIND_ANGLE) && !IS_STALE(IDX_APPARENT_WIND_ANGLE)) {
    int32_t awa = (int32_t)(ApparentWindAngleNMEA * 100.0);
    if (awa < currentWindow->awa_min) currentWindow->awa_min = awa;
    if (awa > currentWindow->awa_max) currentWindow->awa_max = awa;
    if (shouldAccumulate) {
      currentWindow->awa_area_v_us += (int64_t)awa * delta_us;
      currentWindow->awa_valid_us += delta_us;
    }
  }

  if (!isnan(TrueWindSpeedNMEA)) {
    int32_t tws = (int32_t)(TrueWindSpeedNMEA * 100.0);
    if (tws < currentWindow->tws_min) currentWindow->tws_min = tws;
    if (tws > currentWindow->tws_max) currentWindow->tws_max = tws;
    if (shouldAccumulate) {
      currentWindow->tws_area_v_us += (int64_t)tws * delta_us;
      currentWindow->tws_valid_us += delta_us;
    }
  }

  if (!isnan(TrueWindAngleNMEA)) {
    int32_t twa = (int32_t)(TrueWindAngleNMEA * 100.0);
    if (twa < currentWindow->twa_min) currentWindow->twa_min = twa;
    if (twa > currentWindow->twa_max) currentWindow->twa_max = twa;
    if (shouldAccumulate) {
      currentWindow->twa_area_v_us += (int64_t)twa * delta_us;
      currentWindow->twa_valid_us += delta_us;
    }
  }

  // Leeway - CONDITIONAL on valid
  if (!isnan(LeewayNMEA)) {
    int32_t leeway = (int32_t)(LeewayNMEA * 100.0);
    if (leeway < currentWindow->leeway_min) currentWindow->leeway_min = leeway;
    if (leeway > currentWindow->leeway_max) currentWindow->leeway_max = leeway;
    if (shouldAccumulate) {
      currentWindow->leeway_area_v_us += (int64_t)leeway * delta_us;
      currentWindow->leeway_valid_us += delta_us;
    }
  }

  // VMG - CONDITIONAL on valid
  if (!isnan(VMGNMEA)) {
    int32_t vmg = (int32_t)(VMGNMEA * 100.0);
    if (vmg < currentWindow->vmg_min) currentWindow->vmg_min = vmg;
    if (vmg > currentWindow->vmg_max) currentWindow->vmg_max = vmg;
    if (shouldAccumulate) {
      currentWindow->vmg_area_v_us += (int64_t)vmg * delta_us;
      currentWindow->vmg_valid_us += delta_us;
    }
  }

  updateGPSBuffer();
  double smoothLat, smoothLon;
  getSmoothedGPS(smoothLat, smoothLon);
  currentWindow->lat_current = smoothLat;
  currentWindow->lon_current = smoothLon;
}

// A window whose sensor was absent for the whole window must upload JSON null, not the untouched
// min/max init sentinels (999900 / -999900) and not a 0.0 average. A fabricated number here is
// permanent: it lands in the cloud time series and every aggregate that reads the column.
// Cloud rule (CLOUD_PLATFORM.md): no firmware-written column is NOT NULL, so null always ingests.
static const char *ltJsonNum(char *buf, size_t n, double v, bool valid) {
  if (valid) snprintf(buf, n, "%.2f", v);
  else       strncpy(buf, "null", n);
  return buf;
}

// Build the cloud upload JSON for one sensor snapshot into the global
// payloadBuffer. Returns bytes written (excluding null terminator), 0 on
// overflow. Used by the PSRAM-ring upload path (uploadBufferedRecords).
// All payload values come from the snapshot itself (snap.window + snap.imu);
// lifetime/session accumulators moved to the 24h config snapshot upload.
size_t buildSnapshotJson(const SensorSnapshot &snap) {
// Per-window avg = time-weighted area / valid microseconds, then un-scale.
// Three scale factors in play: SensorWindow uses ×100 for most fields but RAW for rpm;
// ImuWindow uses ×100 for heel/pitch but ×1000 for vertical/total accel.
#define SAFE_AVG_100(area, valid)  ((valid) > 0 ? ((double)(area) / (double)(valid)) / 100.0  : 0.0)
#define SAFE_AVG_1000(area, valid) ((valid) > 0 ? ((double)(area) / (double)(valid)) / 1000.0 : 0.0)
#define SAFE_AVG_RAW(area, valid)  ((valid) > 0 ? ((double)(area) / (double)(valid))          : 0.0)
  time_t finalTs = reconstructTimestamp((time_t)snap.collectionTime);
  const char *timestampStr = formatTimestamp(finalTs);
  // Battery current + SoC upload as null when the window never saw a shunt (BatteryShuntPresent=0
  // or no shunt resistance): the accumulators are skipped upstream, so valid_us == 0 is the signal.
  // engine-on avg rides the same flag — active_us can be non-zero while battCurr never accumulated.
  const bool bcOk  = snap.window.battCurr_valid_us > 0;
  const bool socOk = snap.window.soc_valid_us > 0;
  char bcMinS[16], bcMaxS[16], bcAvgS[16], bcOnAvgS[16], socMinS[16], socMaxS[16], socAvgS[16];
  // Same rule for the VE.Direct shunt and the four wind fields: updateSensorWindow skips their
  // accumulators while the source is stale or never seen, so valid_us == 0 means absent. The cloud
  // nulls the 9999 min/max sentinels itself but plots a 0.0 average as data, which is the flat
  // 0-kt / 0 A line a boat without the instrument used to draw.
  const bool vcOk  = snap.window.victronCurr_valid_us > 0;
  const bool awsOk = snap.window.aws_valid_us > 0;
  const bool twsOk = snap.window.tws_valid_us > 0;
  const bool awaOk = snap.window.awa_valid_us > 0;
  const bool twaOk = snap.window.twa_valid_us > 0;
  char vcMinS[16], vcMaxS[16], vcAvgS[16], vcOnAvgS[16];
  char awsMinS[16], awsMaxS[16], awsAvgS[16], twsMinS[16], twsMaxS[16], twsAvgS[16];
  char awaMinS[16], awaMaxS[16], awaAvgS[16], twaMinS[16], twaMaxS[16], twaAvgS[16];
  int written = snprintf(
    payloadBuffer, PAYLOAD_BUFFER_SIZE,
    "{"
    // Identity & metadata
    "\"device_uid\":\"%s\","
    "\"token\":\"%s\","
    "\"timestamp\":\"%s\","
    "\"firmware_version_int\":%d,"
    // payload_v = ingest payload schema version; bump when this body's shape changes.
    // Edge fn inserts an explicit column whitelist, so it safely ignores this. See CLOUD_PLATFORM.md §3a.
    // v2 adds imu_suspicious (IMU install-validation flag).
    // v3 adds engine-on-weighted averages (*_onavg) + engine_on_pct coverage.
    // v4 adds sog_sust1m_max — the window's best 60-s average SOG, which now feeds the
    // cloud speed board in place of sog_max (a single-sample peak).
    // v5 adds speed_source_phone — true when the window's standing sog_sust1m_max was
    // phone-GPS-sourced (selectable speedSourceMode), so leaderboards can disqualify.
    "\"payload_v\":5,"
    "\"current_time_source\":%d,"
    // Battery
    "\"batt_volt_min\":%.2f,\"batt_volt_max\":%.2f,\"batt_volt_avg\":%.2f,"
    "\"batt_curr_min\":%s,\"batt_curr_max\":%s,\"batt_curr_avg\":%s,"
    // Alternator
    "\"alt_curr_min\":%.2f,\"alt_curr_max\":%.2f,\"alt_curr_avg\":%.2f,"
    "\"duty_cycle_min\":%.2f,\"duty_cycle_max\":%.2f,\"duty_cycle_avg\":%.2f,"
    "\"victron_curr_min\":%s,\"victron_curr_max\":%s,\"victron_curr_avg\":%s,"
    "\"soc_min\":%s,\"soc_max\":%s,\"soc_avg\":%s,"
    // Engine
    "\"rpm_min\":%d,\"rpm_max\":%d,\"rpm_avg\":%d,"
    // Temperatures
    "\"alt_temp_min\":%.2f,\"alt_temp_max\":%.2f,\"alt_temp_avg\":%.2f,"
    "\"temp_therm_min\":%.2f,\"temp_therm_max\":%.2f,\"temp_therm_avg\":%.2f,"
    "\"amb_temp_min\":%.2f,\"amb_temp_max\":%.2f,\"amb_temp_avg\":%.2f,"
    // Pressure
    "\"baro_min\":%.2f,\"baro_max\":%.2f,\"baro_avg\":%.2f,"
    // Control loop diagnostics
    "\"u_target_amps_min\":%.2f,\"u_target_amps_max\":%.2f,\"u_target_amps_avg\":%.2f,"
    // NMEA navigation
    "\"sog_min\":%.2f,\"sog_max\":%.2f,\"sog_avg\":%.2f,"
    "\"sog_sust1m_max\":%.2f,"
    "\"speed_source_phone\":%s,"
    "\"lat_current\":%.6f,\"lon_current\":%.6f,"
    // NMEA wind & sailing
    "\"aws_min\":%s,\"aws_max\":%s,\"aws_avg\":%s,"
    "\"tws_min\":%s,\"tws_max\":%s,\"tws_avg\":%s,"
    "\"vmg_min\":%.2f,\"vmg_max\":%.2f,\"vmg_avg\":%.2f,"
    "\"leeway_min\":%.2f,\"leeway_max\":%.2f,\"leeway_avg\":%.2f,"
    // Wind/heading angles + alt-zero + charge stage (long-term-plot stitch fields).
    // awa/twa envelope; cog/heading/alt_zero avg-only; charge_stage categorical (0-7).
    "\"awa_min\":%s,\"awa_max\":%s,\"awa_avg\":%s,"
    "\"twa_min\":%s,\"twa_max\":%s,\"twa_avg\":%s,"
    "\"cog_avg\":%.2f,\"heading_avg\":%.2f,\"alt_zero_avg\":%.2f,"
    "\"charge_stage\":%d,"
    // Engine-on-weighted averages (denominator = µs engine was spinning this window)
    // + coverage. engine_on_pct 0 ⇒ engine never ran ⇒ *_onavg values meaningless (0s).
    "\"batt_volt_onavg\":%.2f,\"batt_curr_onavg\":%s,\"alt_curr_onavg\":%.2f,"
    "\"victron_curr_onavg\":%s,\"duty_cycle_onavg\":%.2f,\"engine_on_pct\":%.2f,"
    // IMU peak motion (per-window aggregates)
    "\"imu_heel_min\":%.2f,\"imu_heel_max\":%.2f,\"imu_heel_avg\":%.2f,"
    "\"imu_pitch_min\":%.2f,\"imu_pitch_max\":%.2f,\"imu_pitch_avg\":%.2f,"
    "\"imu_vertical_accel_min\":%.3f,\"imu_vertical_accel_max\":%.3f,\"imu_vertical_accel_avg\":%.3f,"
    "\"imu_total_accel_min\":%.3f,\"imu_total_accel_max\":%.3f,\"imu_total_accel_avg\":%.3f,"
    // IMU comfort scores (point values at upload time)
    "\"imu_msi_score\":%.2f,"
    "\"imu_vomit_pct\":%.2f,"
    "\"imu_anchorage_comfort\":%.2f,"
    "\"imu_wave_period_sec\":%.2f,"
    "\"imu_slam_count_window\":%u,"
    "\"imu_slam_peak_max_window\":%.3f,"
    // true = this window's IMU data is not trustworthy (install unvalidated/bad, or implausible heel/pitch
    // bias). The row still uploads in full — the cloud filters on the flag rather than losing the window.
    "\"imu_suspicious\":%s"
    "}",
    device_id_hex,
    authToken.c_str(),
    timestampStr,
    firmwareVersionInt,
    (int)currentTimeSource,
    snap.window.battVolt_min / 100.0, snap.window.battVolt_max / 100.0,
    SAFE_AVG_100(snap.window.battVolt_area_v_us, snap.window.battVolt_valid_us),
    ltJsonNum(bcMinS, sizeof(bcMinS), snap.window.battCurr_min / 100.0, bcOk),
    ltJsonNum(bcMaxS, sizeof(bcMaxS), snap.window.battCurr_max / 100.0, bcOk),
    ltJsonNum(bcAvgS, sizeof(bcAvgS), SAFE_AVG_100(snap.window.battCurr_area_v_us, snap.window.battCurr_valid_us), bcOk),
    snap.window.altCurr_min / 100.0, snap.window.altCurr_max / 100.0,
    SAFE_AVG_100(snap.window.altCurr_area_v_us, snap.window.altCurr_valid_us),
    snap.window.dutyCycle_min / 100.0, snap.window.dutyCycle_max / 100.0,
    SAFE_AVG_100(snap.window.dutyCycle_area_v_us, snap.window.dutyCycle_valid_us),
    ltJsonNum(vcMinS, sizeof(vcMinS), snap.window.victronCurr_min / 100.0, vcOk),
    ltJsonNum(vcMaxS, sizeof(vcMaxS), snap.window.victronCurr_max / 100.0, vcOk),
    ltJsonNum(vcAvgS, sizeof(vcAvgS), SAFE_AVG_100(snap.window.victronCurr_area_v_us, snap.window.victronCurr_valid_us), vcOk),
    ltJsonNum(socMinS, sizeof(socMinS), snap.window.soc_min / 100.0, socOk),
    ltJsonNum(socMaxS, sizeof(socMaxS), snap.window.soc_max / 100.0, socOk),
    ltJsonNum(socAvgS, sizeof(socAvgS), SAFE_AVG_100(snap.window.soc_area_v_us, snap.window.soc_valid_us), socOk),
    snap.window.rpm_min, snap.window.rpm_max,
    (int)SAFE_AVG_RAW(snap.window.rpm_area_v_us, snap.window.rpm_valid_us),
    snap.window.altTemp_min / 100.0, snap.window.altTemp_max / 100.0,
    SAFE_AVG_100(snap.window.altTemp_area_v_us, snap.window.altTemp_valid_us),
    snap.window.tempTherm_min / 100.0, snap.window.tempTherm_max / 100.0,
    SAFE_AVG_100(snap.window.tempTherm_area_v_us, snap.window.tempTherm_valid_us),
    snap.window.ambTemp_min / 100.0, snap.window.ambTemp_max / 100.0,
    SAFE_AVG_100(snap.window.ambTemp_area_v_us, snap.window.ambTemp_valid_us),
    snap.window.baro_min / 100.0, snap.window.baro_max / 100.0,
    SAFE_AVG_100(snap.window.baro_area_v_us, snap.window.baro_valid_us),
    snap.window.uTargetAmps_min / 100.0, snap.window.uTargetAmps_max / 100.0,
    SAFE_AVG_100(snap.window.uTargetAmps_area_v_us, snap.window.uTargetAmps_valid_us),
    snap.window.sog_min / 100.0, snap.window.sog_max / 100.0,
    SAFE_AVG_100(snap.window.sog_area_v_us, snap.window.sog_valid_us),
    snap.window.sogSust1m_max / 100.0,
    snap.window.sogSustPhone ? "true" : "false",
    snap.window.lat_current, snap.window.lon_current,
    ltJsonNum(awsMinS, sizeof(awsMinS), snap.window.aws_min / 100.0, awsOk),
    ltJsonNum(awsMaxS, sizeof(awsMaxS), snap.window.aws_max / 100.0, awsOk),
    ltJsonNum(awsAvgS, sizeof(awsAvgS), SAFE_AVG_100(snap.window.aws_area_v_us, snap.window.aws_valid_us), awsOk),
    ltJsonNum(twsMinS, sizeof(twsMinS), snap.window.tws_min / 100.0, twsOk),
    ltJsonNum(twsMaxS, sizeof(twsMaxS), snap.window.tws_max / 100.0, twsOk),
    ltJsonNum(twsAvgS, sizeof(twsAvgS), SAFE_AVG_100(snap.window.tws_area_v_us, snap.window.tws_valid_us), twsOk),
    snap.window.vmg_min / 100.0, snap.window.vmg_max / 100.0,
    SAFE_AVG_100(snap.window.vmg_area_v_us, snap.window.vmg_valid_us),
    snap.window.leeway_min / 100.0, snap.window.leeway_max / 100.0,
    SAFE_AVG_100(snap.window.leeway_area_v_us, snap.window.leeway_valid_us),
    ltJsonNum(awaMinS, sizeof(awaMinS), snap.window.awa_min / 100.0, awaOk),
    ltJsonNum(awaMaxS, sizeof(awaMaxS), snap.window.awa_max / 100.0, awaOk),
    ltJsonNum(awaAvgS, sizeof(awaAvgS), SAFE_AVG_100(snap.window.awa_area_v_us, snap.window.awa_valid_us), awaOk),
    ltJsonNum(twaMinS, sizeof(twaMinS), snap.window.twa_min / 100.0, twaOk),
    ltJsonNum(twaMaxS, sizeof(twaMaxS), snap.window.twa_max / 100.0, twaOk),
    ltJsonNum(twaAvgS, sizeof(twaAvgS), SAFE_AVG_100(snap.window.twa_area_v_us, snap.window.twa_valid_us), twaOk),
    SAFE_AVG_100(snap.window.cog_area_v_us, snap.window.cog_valid_us),
    SAFE_AVG_100(snap.window.heading_area_v_us, snap.window.heading_valid_us),
    SAFE_AVG_100(snap.window.altZero_area_v_us, snap.window.altZero_valid_us),
    (int)snap.chargeStage,
    SAFE_AVG_100(snap.window.battVolt_on_area_v_us, snap.window.active_us),
    ltJsonNum(bcOnAvgS, sizeof(bcOnAvgS), SAFE_AVG_100(snap.window.battCurr_on_area_v_us, snap.window.active_us), bcOk),
    SAFE_AVG_100(snap.window.altCurr_on_area_v_us, snap.window.active_us),
    ltJsonNum(vcOnAvgS, sizeof(vcOnAvgS), SAFE_AVG_100(snap.window.victronCurr_on_area_v_us, snap.window.active_us), vcOk),
    SAFE_AVG_100(snap.window.dutyCycle_on_area_v_us, snap.window.active_us),
    snap.window.battVolt_valid_us > 0
      ? 100.0 * (double)snap.window.active_us / (double)snap.window.battVolt_valid_us : 0.0,
    // IMU values read from the frozen ImuSnapshot — values matching the moment
    // the window was rolled, not whatever imuWindow currently holds at upload time.
    snap.imu.heel_min / 100.0, snap.imu.heel_max / 100.0,
    SAFE_AVG_100(snap.imu.heel_area_v_us, snap.imu.heel_valid_us),
    snap.imu.pitch_min / 100.0, snap.imu.pitch_max / 100.0,
    SAFE_AVG_100(snap.imu.pitch_area_v_us, snap.imu.pitch_valid_us),
    snap.imu.vertical_accel_min / 1000.0, snap.imu.vertical_accel_max / 1000.0,
    SAFE_AVG_1000(snap.imu.vertical_accel_area_v_us, snap.imu.vertical_accel_valid_us),
    snap.imu.total_accel_min / 1000.0, snap.imu.total_accel_max / 1000.0,
    SAFE_AVG_1000(snap.imu.total_accel_area_v_us, snap.imu.total_accel_valid_us),
    snap.imu.msi_score,
    snap.imu.vomit_pct,
    snap.imu.anchorage_comfort,
    snap.imu.wave_period / 1000.0,
    (unsigned int)snap.imu.slam_count,
    snap.imu.slam_peak_max / 1000.0,
    snap.imu.suspicious ? "true" : "false");
#undef SAFE_AVG_100
#undef SAFE_AVG_1000
  if (written <= 0 || written >= PAYLOAD_BUFFER_SIZE) return 0;
  return (size_t)written;
}

void uploadBufferedRecords() {
  if (otaInProgress) return;
  if (hardwarePresent != 1) return;   // sim mode (HardwarePresent=0): never upload fake data to the cloud
  if (ringIsEmpty()) return;
  if (!isRegistered || authToken.isEmpty()) {
    Serial.println("uploadBufferedRecords: skipping — not registered");
    return;
  }
  if (!canUploadNow()) return;

  // One upload in flight at a time. Core 0 advances the tail when it succeeds
  // (or hits a 400/401). On network failure it leaves the slot alone for retry.
  if (sensorRingInFlightIndex >= 0) return;

  SensorSnapshot snap;
  if (!peekTailSnapshot(&snap)) return;

  size_t written = buildSnapshotJson(snap);
  if (written == 0) {
    // JSON build overflow — drop this slot so we don't loop on it forever.
    Serial.println("uploadBufferedRecords: JSON overflow, dropping tail snapshot");
    popTailSnapshot();
    return;
  }

  HttpsRequest req = {};
  req.type = HTTPS_UPLOAD_PAYLOAD;
  req.payloadCap = PAYLOAD_BUFFER_SIZE;
  req.payload = (char *)ps_malloc(req.payloadCap);
  if (req.payload) {
    strncpy(req.payload, payloadBuffer, req.payloadCap - 1);
    req.payload[req.payloadCap - 1] = '\0';
    if (xQueueSend(httpsQueue, &req, 0) == pdTRUE) {
      sensorRingInFlightIndex = (int32_t)sensorRingTail;
      lastUploadWasBuffered = true;
    } else {
      free(req.payload);
      Serial.println("uploadBufferedRecords: httpsQueue full, will retry next tick");
    }
  }
  esp_task_wdt_reset();
}
// Defined here, ahead of the ring dump/restore section that owns it, because the drain-to-empty
// path in executeUploadPayload below deletes the file.
#define SENSOR_RING_BACKUP_PATH  "/sensor_ring_backup.bin"
// The ring just emptied by consumption, so the Phase-4 dump on flash is stale: left in place it is
// restored at the next boot and every record is re-POSTed. Raw remove: fsExists() would re-take the
// non-recursive fsMutex and block 5 s. Runs on httpsTask (Core 0).
static void sensorRingDropBackupFile() {
  fsTakeLock();
  LittleFS.remove(SENSOR_RING_BACKUP_PATH);
  fsReleaseLock();
  fsFreeDirty = true;
}

bool executeUploadPayload(const char *payload) {
  lastHttpResponseCode = 0;  // per-attempt reset — a transport failure must not leave the previous attempt's code standing
  if (WiFi.status() != WL_CONNECTED) return false;
  if (WiFi.RSSI() < -80) return false;
  if (!isRegistered || authToken.isEmpty()) {
    Serial.println("executeUploadPayload: ABORT - no token (file stays in buffer)");
    return false;
  }
  WiFiClientSecure client;
  client.setInsecure();
  // setTimeout omitted: Stream::setTimeout is ms (not seconds as some docs claim) and
  // the read loops below use available()+read() polling with explicit millis() deadlines,
  // so the Stream-level timeout doesn't gate anything here.
  client.setHandshakeTimeout(HANDSHAKE_TIMEOUT / 1000);  // this API takes seconds

  uint32_t start = millis();
  esp_task_wdt_reset();

  // Pre-resolve DNS (worst ~15 s on dead cellular) so it can't stack into the same unfed-WDT
  // window as the up-to-14 s connect below; the connect's own lookup then hits the lwIP cache.
  IPAddress hostIP;
  if (!WiFi.hostByName(host, hostIP)) {
    Serial.println("Upload: DNS fail");
    return false;
  }
  esp_task_wdt_reset();

  // Hard-bounded TLS connect (silent on success; logs only on failure)
  if (!client.connect(host, port, CONNECT_TIMEOUT)) {
    Serial.println("Upload: connect fail");
    client.stop();
    return false;
  }

  // Defensive global timeout check after connect/handshake
  if (millis() - start > GLOBAL_TIMEOUT) {
    Serial.println("Upload: connect exceeded global timeout");
    client.stop();
    return false;
  }

  esp_task_wdt_reset();

  // Serial.println("=== REQUEST DEBUG ===");
  // Serial.printf("Host: %s\n", host);
  // Serial.printf("Port: %d\n", port);
  // Serial.printf("Endpoint: /functions/v1/update-sensor-history\n");
  // Serial.printf("API Key (first 50 chars): %.50s...\n", SUPABASE_ANON_KEY);
  // Serial.println("====================");



  // ===== Send headers (small, no heap) =====
  int headerBytes = client.printf(
    "POST /functions/v1/update-sensor-history HTTP/1.1\r\n"
    "Host: %s\r\n"
    "Content-Type: application/json\r\n"
    "Authorization: Bearer %s\r\n"
    "Connection: close\r\n"
    "Content-Length: %u\r\n\r\n",
    host,
    SUPABASE_ANON_KEY,
    (unsigned)strlen(payload));

  if (headerBytes <= 0 || !client.connected()) {
    Serial.println("Header send fail");
    client.stop();
    return false;
  }

  // ===== Stream payload directly =====
  size_t payloadLen = strlen(payload);
  size_t sent = client.write((const uint8_t *)payload, payloadLen);
  if (sent != payloadLen) {
    Serial.println("Payload send fail");
    client.stop();
    return false;
  }
  esp_task_wdt_reset();

  // ===== Read status line + drain headers (no header parsing/no truncation spam) =====
  int httpCode = 0;
  bool ackConfirmed = false;
  uint32_t readStart = millis();

  // 1) Read ONLY the first line (status line). READ_TIMEOUT is idle — each byte restarts it.
  char statusBuf[64];
  size_t statusLen = 0;
  bool gotStatusLine = false;

  while (client.connected() && (millis() - readStart < READ_TIMEOUT)) {
    esp_task_wdt_reset();

    while (client.available()) {
      char c = (char)client.read();
      readStart = millis();

      // collect status line until '\n'
      if (statusLen < sizeof(statusBuf) - 1) {
        statusBuf[statusLen++] = c;
      }

      if (c == '\n') {
        statusBuf[statusLen] = '\0';
        gotStatusLine = true;

        // Parse: "HTTP/1.1 200 ..." or "HTTP/2 200 ..."
        if (strncmp(statusBuf, "HTTP/", 5) == 0) {
          const char *sp = strchr(statusBuf, ' ');
          if (sp) httpCode = atoi(sp + 1);
        }
        goto drain_headers;
      }
    }

    if (millis() - start > GLOBAL_TIMEOUT) break;
    delay(1);
  }

drain_headers:
  // 2) Drain headers until CRLFCRLF (end of headers). No logging.
  // If we never got the status line, this will still just time out and we’ll return "No response".
  {
    uint32_t drainStart = millis();
    uint8_t state = 0;  // match sequence: \r \n \r \n

    while (client.connected() && (millis() - drainStart < READ_TIMEOUT)) {
      esp_task_wdt_reset();

      while (client.available()) {
        char c = (char)client.read();
        drainStart = millis();

        if (state == 0 && c == '\r') state = 1;
        else if (state == 1 && c == '\n') state = 2;
        else if (state == 2 && c == '\r') state = 3;
        else if (state == 3 && c == '\n') goto done_headers;
        else state = 0;
      }

      if (millis() - start > GLOBAL_TIMEOUT) break;
      delay(1);
    }
  }

done_headers:
  // Read the small ack body: the edge function returns {"success":true,...} on a real ingest.
  // A captive portal answering 200 with a login page has no such marker, so the ring slot is
  // kept and retried instead of being dropped on a lie. Retries are duplicate-safe (the server
  // treats a (device_uid,timestamp) collision as an idempotent 200).
  {
    char bodyBuf[257];
    size_t bodyLen = 0;
    uint32_t bodyStart = millis();
    while (client.connected() && bodyLen < sizeof(bodyBuf) - 1 && (millis() - bodyStart < READ_TIMEOUT)) {
      esp_task_wdt_reset();
      if (client.available()) bodyStart = millis();
      while (client.available() && bodyLen < sizeof(bodyBuf) - 1) bodyBuf[bodyLen++] = (char)client.read();
      bodyBuf[bodyLen] = '\0';
      // strstr on the raw bytes tolerates HTTP/1.1 chunked framing around the JSON
      if (strstr(bodyBuf, "\"success\":true")) { ackConfirmed = true; break; }
      if (millis() - start > GLOBAL_TIMEOUT) break;
      delay(1);
    }
  }
  client.stop();

  esp_task_wdt_reset();

  if (httpCode == 0) {
    Serial.println("No response received (timeout)");
    return false;
  }

  // Log only non-200 codes — success is the common path and was noisy.
  if (httpCode != 200) Serial.printf("Upload: HTTP %d\n", httpCode);
  lastHttpResponseCode = httpCode;
  if (httpCode == 200 && ackConfirmed) lastCloudUploadOkMs = millis();

  // ===== Handle PSRAM-ring slot based on response code =====
  // sensorRingInFlightIndex was set by uploadBufferedRecords() when it queued
  // this request. On 200 → pop tail (slot consumed). On 400/401 → also pop
  // (bad data, no point retrying). On network/server error → leave ring as-is
  // and the next uploadBufferedRecords() tick will re-queue the same slot.
  if (lastUploadWasBuffered && sensorRingInFlightIndex >= 0) {
    if (httpCode == 200 && ackConfirmed) {
      popTailSnapshot();
      if (ringIsEmpty()) {
        // Only announce "all uploaded" once per drain-to-empty transition.
        // pushSensorSnapshot re-arms the flag when count goes back above zero.
        if (!sensorRingAnnouncedEmpty) {
          queueConsoleMessage("Cloud sync: all data uploaded");
          sensorRingAnnouncedEmpty = true;
          sensorRingDropBackupFile();
        }
      } else {
        // Throttle the "N queued" progress chatter to at most one per minute.
        // Full drain visibility is still on the dashboard's bufferedRecordCount.
        static unsigned long lastQueuedMsgMs = 0;
        if (millis() - lastQueuedMsgMs >= 60000) {
          lastQueuedMsgMs = millis();
          snprintf(messageBuffer, MESSAGE_BUFFER_SIZE, "Cloud sync: %u records queued", (unsigned)sensorRingCount);
          queueConsoleMessage(messageBuffer);
        }
      }
      sensorRingInFlightIndex = -1;

    } else if (httpCode == 400 || httpCode == 401) {
      Serial.printf("HTTP %d: dropping bad-data ring slot\n", httpCode);
      popTailSnapshot();
      // A drain can end on a rejected record too, and the dump is just as stale then (see the 200 branch).
      if (ringIsEmpty() && !sensorRingAnnouncedEmpty) { sensorRingAnnouncedEmpty = true; sensorRingDropBackupFile(); }
      snprintf(messageBuffer, MESSAGE_BUFFER_SIZE, "Cleared bad data (%u queued)", (unsigned)sensorRingCount);
      queueConsoleMessage(messageBuffer);
      sensorRingInFlightIndex = -1;

    } else {
      // Network/server error — or a 200 whose body lacked the edge-function ack
      // (captive portal / interception). Keep slot for retry on next tick.
      Serial.printf("⚠ HTTP %d%s: keeping ring slot for retry\n", httpCode, (httpCode == 200) ? " (no ack in body)" : "");
      if (httpCode == 200) {
        queueConsoleMessage("Cloud sync failed (server reply was not the cloud's ack - captive portal?)");
      } else {
        snprintf(messageBuffer, MESSAGE_BUFFER_SIZE, "Cloud sync failed (HTTP %d)", httpCode);
        queueConsoleMessage(messageBuffer);
      }
      sensorRingInFlightIndex = -1;  // allow re-queue next tick
    }
    lastUploadWasBuffered = false;
  } else if (lastUploadWasBuffered && httpCode != 200) {
    // Buffered flag set but no in-flight ring slot (shouldn't normally happen).
    snprintf(messageBuffer, MESSAGE_BUFFER_SIZE, "Cloud sync failed (HTTP %d)", httpCode);
    queueConsoleMessage(messageBuffer);
    lastUploadWasBuffered = false;
  }

  return (httpCode == 200 && ackConfirmed);
}

// PSRAM sensor-snapshot ring (replaces the old LittleFS /buffer/*.json layout).
// One slot per completed SENSOR_UPLOAD_INTERVAL window. Push is microseconds;
// no flash I/O, no Core 1 stall. Drain to Supabase happens from the cloud-
// feature block in loop() under the same field-off gate as uploadBufferedRecords.
// On overflow, the oldest unread slot is dropped.
inline bool ringIsFull()  { return sensorRingCount >= SENSOR_RING_SIZE; }
inline bool ringIsEmpty() { return sensorRingCount == 0; }

// Push current window snapshot. Drops oldest if ring is full (unless the
// oldest slot is currently being uploaded — then drop the new sample instead
// so we don't yank the rug out from Core 0 mid-upload).
void pushSensorSnapshot(time_t collectionTime) {
  if (!sensorRing) return;
  // Index RMWs under sensorRingMux (Core 0 pops concurrently). The big struct
  // copy below stays OUTSIDE the lock — the head slot is invisible to Core 0
  // until the count++ publish at the end.
  bool dropNewest = false;
  portENTER_CRITICAL(&sensorRingMux);
  if (ringIsFull()) {
    if (sensorRingInFlightIndex == (int32_t)sensorRingTail) {
      // Oldest slot is being uploaded; drop the NEW snapshot instead.
      dropNewest = true;
    } else {
      // Drop oldest to make room.
      sensorRingTail = (sensorRingTail + 1) % SENSOR_RING_SIZE;
      sensorRingCount--;
    }
  }
  portEXIT_CRITICAL(&sensorRingMux);
  if (dropNewest) {
    Serial.println("sensorRing full + tail in-flight; dropping newest snapshot");
    return;
  }
  sensorRing[sensorRingHead].collectionTime = (int64_t)collectionTime;
  sensorRing[sensorRingHead].window = *currentWindow;  // struct copy from PSRAM to PSRAM
  // Freeze the IMU subset (caller resets imuWindow right after, so we capture
  // the closing window's values here before they get clobbered).
  ImuSnapshot &is = sensorRing[sensorRingHead].imu;
  is.heel_min                  = imuWindow->heel_min;
  is.heel_max                  = imuWindow->heel_max;
  is.heel_area_v_us            = imuWindow->heel_area_v_us;
  is.heel_valid_us             = imuWindow->heel_valid_us;
  is.pitch_min                 = imuWindow->pitch_min;
  is.pitch_max                 = imuWindow->pitch_max;
  is.pitch_area_v_us           = imuWindow->pitch_area_v_us;
  is.pitch_valid_us            = imuWindow->pitch_valid_us;
  is.vertical_accel_min        = imuWindow->vertical_accel_min;
  is.vertical_accel_max        = imuWindow->vertical_accel_max;
  is.vertical_accel_area_v_us  = imuWindow->vertical_accel_area_v_us;
  is.vertical_accel_valid_us   = imuWindow->vertical_accel_valid_us;
  is.total_accel_min           = imuWindow->total_accel_min;
  is.total_accel_max           = imuWindow->total_accel_max;
  is.total_accel_area_v_us     = imuWindow->total_accel_area_v_us;
  is.total_accel_valid_us      = imuWindow->total_accel_valid_us;
  is.msi_score                 = imu_msi_score;
  is.vomit_pct                 = imu_vomit_pct;
  is.anchorage_comfort         = imu_anchorage_comfort;
  is.wave_period               = imuWindow->wave_period;
  is.slam_count                = imuWindow->slam_count;
  is.slam_peak_max             = imuWindow->slam_peak_max;
  // Per-window bias judgement. The mount verdict is NOT decided here — imuZeroFinalizeIfDue owns it, because
  // a running window cannot tell a bad mount from a heeled boat. This path must stay free of NVS writes: it
  // runs on the Core-1 control tick with the field live. SAFE_AVG_* is #undef'd above buildSnapshotJson, so
  // the averages are inline.
  const bool haveImuData = imuWindow->total_accel_valid_us >= IMU_WINDOW_MIN_VALID_US;
  bool biasSuspect = false;
  if (haveImuData) {
    float heelAvg  = imuWindow->heel_valid_us  ? (float)((double)imuWindow->heel_area_v_us  / (double)imuWindow->heel_valid_us)  / 100.0f : 0.0f;
    float pitchAvg = imuWindow->pitch_valid_us ? (float)((double)imuWindow->pitch_area_v_us / (double)imuWindow->pitch_valid_us) / 100.0f : 0.0f;
    biasSuspect = (fabsf(heelAvg) > IMU_BIAS_SUSPECT_DEG || fabsf(pitchAvg) > IMU_BIAS_SUSPECT_DEG);
  }
  is.suspicious = !haveImuData || !imuZeroCaptured
                  || imuMountState != IMU_MOUNT_OK || biasSuspect;
  // Charge stage at window-roll time (cloud upload is deferred minutes/hours, so
  // we can't read live state at JSON-build time). Matches the LT-ring capture.
  sensorRing[sensorRingHead].chargeStage = getChargeStageDisplayCode();
  portENTER_CRITICAL(&sensorRingMux);
  sensorRingHead = (sensorRingHead + 1) % SENSOR_RING_SIZE;
  sensorRingCount++;
  bufferedRecordCount = sensorRingCount;  // dashboard mirror
  portEXIT_CRITICAL(&sensorRingMux);
  // Re-arm the "all uploaded" console message — count just went non-zero so
  // the next time we drain to empty, we want to announce it once.
  extern volatile bool sensorRingAnnouncedEmpty;
  sensorRingAnnouncedEmpty = false;
}

// Peek at oldest unread snapshot without removing it. Caller passes a pointer
// to a SensorSnapshot it'll copy into. Returns false if ring is empty or busy.
bool peekTailSnapshot(SensorSnapshot *out) {
  if (!sensorRing || ringIsEmpty() || !out) return false;
  *out = sensorRing[sensorRingTail];
  return true;
}

// Pop oldest. Caller must have already successfully consumed it (e.g. queued
// for upload and Core 0 confirmed 200 OK).
void popTailSnapshot() {
  if (!sensorRing) return;
  // Runs on Core 0 (httpsTask) against Core 1's push — empty-check and RMW
  // must be one atomic unit or a lost decrement wraps count to 65535.
  portENTER_CRITICAL(&sensorRingMux);
  if (ringIsEmpty()) {
    portEXIT_CRITICAL(&sensorRingMux);
    return;
  }
  sensorRingTail = (sensorRingTail + 1) % SENSOR_RING_SIZE;
  sensorRingCount--;
  bufferedRecordCount = sensorRingCount;  // dashboard mirror
  portEXIT_CRITICAL(&sensorRingMux);
}

// Binary-format file used by Phase 3 (shutdown dump + boot restore) so the
// PSRAM ring survives a power-cycle when WiFi/cloud couldn't drain everything
// during the 30-min ignition-off window.
#define SENSOR_RING_BACKUP_MAGIC 0x53524258u  // 'SRBX'
#define SENSOR_RING_BACKUP_VER   3u  // v3: + chargeStage byte (LT-plot cloud-stitch field)

struct SensorRingBackupHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t count;
  uint32_t entrySize;  // sizeof(SensorSnapshot) — sanity guard against struct layout change
};

// One-shot bulk dump of the PSRAM ring to LittleFS. Called from shutdown
// Phase 4 when the 30-min cloud drain window expired with records remaining.
// Returns number of snapshots written, or 0 if ring was empty / on error.
// Blocking flash write is acceptable here — the field is already off and the
// device is about to sleep.
uint16_t dumpSensorRingToLittleFS() {
  if (!sensorRing || ringIsEmpty()) return 0;
  fsTakeLock();
  File f = LittleFS.open(SENSOR_RING_BACKUP_PATH, "w");
  if (!f) {
    fsReleaseLock();
    Serial.println("dumpSensorRingToLittleFS: open failed");
    return 0;
  }
  fsFreeDirty = true;
  SensorRingBackupHeader hdr = {
    SENSOR_RING_BACKUP_MAGIC,
    SENSOR_RING_BACKUP_VER,
    (uint32_t)sensorRingCount,
    (uint32_t)sizeof(SensorSnapshot),
  };
  size_t hdrWritten = f.write((const uint8_t *)&hdr, sizeof(hdr));
  if (hdrWritten != sizeof(hdr)) {
    f.close();
    LittleFS.remove(SENSOR_RING_BACKUP_PATH);
    fsReleaseLock();
    Serial.println("dumpSensorRingToLittleFS: header write failed");
    return 0;
  }
  uint16_t written = 0;
  uint16_t idx = sensorRingTail;
  for (uint16_t i = 0; i < sensorRingCount; i++) {
    size_t n = f.write((const uint8_t *)&sensorRing[idx], sizeof(SensorSnapshot));
    if (n != sizeof(SensorSnapshot)) {
      Serial.printf("dumpSensorRingToLittleFS: short write at entry %u\n", i);
      break;
    }
    written++;
    idx = (idx + 1) % SENSOR_RING_SIZE;
    esp_task_wdt_reset();
  }
  f.close();
  fsReleaseLock();
  Serial.printf("dumpSensorRingToLittleFS: wrote %u snapshots\n", written);
  return written;
}

// Boot-time restore: read the LittleFS backup file (if present) and re-push
// each snapshot into the empty PSRAM ring. Delete the file regardless of
// success — a partial restore is acceptable, but leaving a stale backup
// around would cause double-uploads on the next boot.
// Returns count restored. Safe to call when no backup file exists.
uint16_t restoreSensorRingFromLittleFS() {
  if (!sensorRing) return 0;
  if (!fsExists(SENSOR_RING_BACKUP_PATH)) return 0;
  fsTakeLock();
  File f = LittleFS.open(SENSOR_RING_BACKUP_PATH, "r");
  if (!f) {
    fsReleaseLock();
    Serial.println("restoreSensorRingFromLittleFS: open failed");
    return 0;
  }
  SensorRingBackupHeader hdr;
  if (f.readBytes((char *)&hdr, sizeof(hdr)) != sizeof(hdr)) {
    f.close();
    LittleFS.remove(SENSOR_RING_BACKUP_PATH);
    fsReleaseLock();
    Serial.println("restoreSensorRingFromLittleFS: short header read, file removed");
    return 0;
  }
  if (hdr.magic != SENSOR_RING_BACKUP_MAGIC
      || hdr.version != SENSOR_RING_BACKUP_VER
      || hdr.entrySize != sizeof(SensorSnapshot)) {
    f.close();
    LittleFS.remove(SENSOR_RING_BACKUP_PATH);
    fsReleaseLock();
    Serial.printf("restoreSensorRingFromLittleFS: header mismatch (magic=%08x ver=%u sz=%u), discarding\n",
                  hdr.magic, hdr.version, hdr.entrySize);
    return 0;
  }
  uint16_t restored = 0;
  uint32_t toRestore = (hdr.count > SENSOR_RING_SIZE) ? SENSOR_RING_SIZE : hdr.count;
  SensorSnapshot tmp;
  for (uint32_t i = 0; i < toRestore; i++) {
    if (f.readBytes((char *)&tmp, sizeof(tmp)) != sizeof(tmp)) {
      Serial.printf("restoreSensorRingFromLittleFS: short read at entry %u\n", i);
      break;
    }
    // Push directly into the ring (bypass pushSensorSnapshot to avoid the
    // currentWindow copy — we have the full snapshot already).
    sensorRing[sensorRingHead] = tmp;
    sensorRingHead = (sensorRingHead + 1) % SENSOR_RING_SIZE;
    if (sensorRingCount < SENSOR_RING_SIZE) sensorRingCount++;
    restored++;
    esp_task_wdt_reset();
  }
  bufferedRecordCount = sensorRingCount;
  f.close();
  LittleFS.remove(SENSOR_RING_BACKUP_PATH);
  fsReleaseLock();
  Serial.printf("restoreSensorRingFromLittleFS: restored %u snapshots\n", restored);
  return restored;
}

// Snapshot current sensor window into the PSRAM ring. Replaces the old
// LittleFS write path; this is microseconds and does not stall Core 1.
// Actual upload to Supabase happens later from the cloud-feature block
// (gated by field-off settle, or forced by the "Upload Now" button).
// Rescale a SensorWindow ×100 value to a long-term record's target scale (divisor =
// 100/targetScale: 1 for ×100, 10 for ×10, 100 for ×1) and clamp to int16.
static inline int16_t ltScale(int32_t v100, int32_t divisor) {
  int32_t s = v100 / divisor;
  if (s > 32767) s = 32767;
  if (s < -32768) s = -32768;
  return (int16_t)s;
}

// Derive one LongTermRecord from the closing SensorWindow + ImuWindow and push it
// into the month-long PSRAM ring. Called every SENSOR_UPLOAD_INTERVAL from
// uploadSensorHistory() (both paths) so the fixed-cadence timeline never gaps within
// a session. validMask bit per field (set only when that field had valid data):
//   ENVELOPE 0..16: battVolt battCurr altCurr victronCurr rpm duty altTemp tempTherm
//                   sog tws vmg aws awa twa heel pitch soc
//   AVG-ONLY 17..22: baro ambTemp cog heading leeway altZero
//   POSITION 23: lat/lon. (Mirror this order in the dashboard JS.)
// envelope (min,max,avg) from currentWindow; avg = area/valid, then rescaled+clamped.
#define LT_ENV(dst, field, divisor, bit) do { \
  if (currentWindow->field##_valid_us > 0) { \
    int32_t _a = (int32_t)(currentWindow->field##_area_v_us / (int64_t)currentWindow->field##_valid_us); \
    rec.dst[0] = ltScale(currentWindow->field##_min, divisor); \
    rec.dst[1] = ltScale(currentWindow->field##_max, divisor); \
    rec.dst[2] = ltScale(_a, divisor); \
    rec.validMask |= (1u << (bit)); \
  } } while (0)
#define LT_AVG(dst, field, divisor, bit) do { \
  if (currentWindow->field##_valid_us > 0) { \
    int32_t _a = (int32_t)(currentWindow->field##_area_v_us / (int64_t)currentWindow->field##_valid_us); \
    rec.dst = ltScale(_a, divisor); \
    rec.validMask |= (1u << (bit)); \
  } } while (0)
#define LT_IMU(dst, field, divisor, bit) do { \
  if (imuWindow && imuWindow->field##_valid_us > 0) { \
    int32_t _a = (int32_t)(imuWindow->field##_area_v_us / (int64_t)imuWindow->field##_valid_us); \
    rec.dst[0] = ltScale(imuWindow->field##_min, divisor); \
    rec.dst[1] = ltScale(imuWindow->field##_max, divisor); \
    rec.dst[2] = ltScale(_a, divisor); \
    rec.validMask |= (1u << (bit)); \
  } } while (0)
void pushLongTermRecord() {
  if (!longTermRing) return;
  LongTermRecord rec = {};  // zero-init: absent fields stay 0 with validMask bit clear

  // Real epoch of THIS record (0 if unsynced) — drives the legit time axis + gap detection.
  uint32_t nowEpoch = (timeIsSynced && timeBase > 0)
                        ? (uint32_t)(timeBase + (millis() - timeBaseMillis) / 1000) : 0;
  rec.timestamp = nowEpoch;

  LT_ENV(battVolt,    battVolt,    1,  0);
  LT_ENV(battCurr,    battCurr,    10, 1);
  LT_ENV(altCurr,     altCurr,     10, 2);
  LT_ENV(victronCurr, victronCurr, 10, 3);
  LT_ENV(rpm,         rpm,         1,  4);   // store RAW RPM (fits int16; matches JS scale=1 + cloud raw)
  LT_ENV(duty,        dutyCycle,   1,  5);
  LT_ENV(altTemp,     altTemp,     10, 6);
  LT_ENV(tempTherm,   tempTherm,   10, 7);
  LT_ENV(sog,         sog,         1,  8);
  LT_ENV(tws,         tws,         1,  9);
  LT_ENV(vmg,         vmg,         1,  10);
  LT_ENV(aws,         aws,         1,  11);
  LT_ENV(awa,         awa,         10, 12);
  LT_ENV(twa,         twa,         10, 13);
  LT_IMU(heel,        heel,        1,  14);
  LT_IMU(pitch,       pitch,       1,  15);
  LT_ENV(soc,         soc,         10, 16);

  LT_AVG(baro_avg,    baro,    10, 17);
  LT_AVG(ambTemp_avg, ambTemp, 10, 18);
  LT_AVG(cog_avg,     cog,     10, 19);
  LT_AVG(heading_avg, heading, 10, 20);
  LT_AVG(leeway_avg,  leeway,  10, 21);
  LT_AVG(altZero_avg, altZero, 1,  22);

  // Position: smoothed lat/lon (deg ×1e5), valid only when GPS isn't stale.
  if (!IS_STALE(IDX_LATITUDE_NMEA) && !IS_STALE(IDX_LONGITUDE_NMEA)) {
    rec.lat_avg = (int32_t)(currentWindow->lat_current * 100000.0);
    rec.lon_avg = (int32_t)(currentWindow->lon_current * 100000.0);
    rec.validMask |= (1u << 23);
  }
  rec.chargeStage = getChargeStageDisplayCode();

  // v4 tail: coverage + engine-on-weighted averages. Coverage = active/observed,
  // ×2 (0.5% steps), ROUNDED — a window with <0.25% engine time rounds to 0 and is
  // treated as "engine never ran" (XFF-style discard; the min/max band keeps any spike).
  // Divisors mirror the LT_ENV calls above. rec zero-init leaves activeFrac 0 otherwise.
  if (currentWindow->battVolt_valid_us > 0 && currentWindow->active_us > 0) {
    uint64_t obs = currentWindow->battVolt_valid_us;
    uint64_t fr = (currentWindow->active_us * 200ULL + obs / 2) / obs;
    rec.activeFrac = (fr > 200) ? 200 : (uint8_t)fr;
    int64_t on_us = (int64_t)currentWindow->active_us;
    rec.onAvg[0] = ltScale((int32_t)(currentWindow->battVolt_on_area_v_us / on_us), 1);
    // Battery current rides its own envelope bit: with no shunt the accumulator is skipped, so
    // active_us alone would publish a fabricated 0 A engine-on average.
    if (rec.validMask & (1u << 1)) rec.onAvg[1] = ltScale((int32_t)(currentWindow->battCurr_on_area_v_us / on_us), 10);
    rec.onAvg[2] = ltScale((int32_t)(currentWindow->altCurr_on_area_v_us / on_us), 10);
    if (rec.validMask & (1u << 3)) rec.onAvg[3] = ltScale((int32_t)(currentWindow->victronCurr_on_area_v_us / on_us), 10);  // VE.Direct: same absent-sensor rule
    rec.onAvg[4] = ltScale((int32_t)(currentWindow->dutyCycle_on_area_v_us / on_us), 1);
  }

  longTermRing[longTermHead] = rec;
  longTermHead = (longTermHead + 1) % LONGTERM_RING_SIZE;
  if (longTermCount < LONGTERM_RING_SIZE) longTermCount++;
  longTermPushSeq++;   // drives the append-flush delta (records new since last flush)
  // Header anchor for the .bin (newest record's epoch); unchanged when unsynced.
  if (nowEpoch) longTermLastEpoch = nowEpoch;
}
#undef LT_ENV
#undef LT_AVG
#undef LT_IMU

void uploadSensorHistory() {
  if (otaInProgress) return;

  // Sim mode (HardwarePresent=0): live display only — no long-term ring, no cloud
  // buffering. Still roll the window so it doesn't accumulate fake data unbounded.
  if (hardwarePresent != 1) {
    resetSensorWindow();
    resetAccelWindow();
    return;
  }

  // Long-term plot record fires every interval regardless of charging state (so the
  // fixed-cadence timeline stays gap-free); reads the closing window before reset.
  pushLongTermRecord();

  if (currentWindow->battVolt_valid_us == 0) {  // no data collected this window
    resetSensorWindow();
    resetAccelWindow();
    return;
  }

  // Stamp collection time NOW (still reflects when the window closed even if
  // upload is deferred minutes/hours).
  time_t collectionTime = computeCollectionTime();
  pushSensorSnapshot(collectionTime);

  resetSensorWindow();
  resetAccelWindow();
}

// Long-term plot ring persistence. Durable month-long cache (NOT drained like the sensor ring),
// so restore keeps the file (deleteAfter=false). To spare flash, the periodic field-off flush
// APPENDS only records pushed since the last flush (see appendRingBlob); a full writePsramBlob
// compaction runs only for the first write, when the file would exceed ~2× the ring, or if an
// append fails. Field-off only — called on the field-off-settled edge + at shutdown.
void dumpLongTermRing() {
  if (dbgRingsSynthetic) return;   // fillmax/clearmax: RAM ring is synthetic/empty — keep the real flash blob
  if (!longTermRing || longTermCount == 0) return;
  uint32_t delta = longTermPushSeq - longTermFlushedSeq;   // records new since last flush (unsigned wrap-safe)
  if (delta == 0 && longTermFileRecords > 0) return;       // file already holds every record — nothing to persist
  if (longTermFileRecords > 0 && delta > 0 && delta <= longTermCount
      && longTermFileRecords + delta <= (uint32_t)LONGTERM_RING_SIZE * 2
      && fsExists(LONGTERM_BACKUP_PATH)
      && appendRingBlob(LONGTERM_BACKUP_PATH, longTermRing, sizeof(LongTermRecord),
                        LONGTERM_RING_SIZE, longTermHead, delta) > 0) {
    longTermFileRecords += delta;
    longTermFlushedSeq   = longTermPushSeq;
    prev_longTermHead    = longTermHead;
    return;
  }
  // Compaction: self-contained oldest-first snapshot the append path can extend. lastEpoch rides
  // in the scaffold userWord (unused by restore, kept for continuity).
  uint16_t startIdx = (longTermCount < LONGTERM_RING_SIZE) ? 0 : longTermHead;  // oldest record
  uint32_t n = writePsramBlob(LONGTERM_BACKUP_PATH, LONGTERM_BACKUP_MAGIC, LONGTERM_BACKUP_VER,
                              (uint32_t)longTermLastEpoch, longTermRing, sizeof(LongTermRecord),
                              LONGTERM_RING_SIZE, startIdx, longTermCount);
  if (n > 0) {
    longTermFileRecords = n;
    longTermFlushedSeq  = longTermPushSeq;
    prev_longTermHead   = longTermHead;
  }
  Serial.printf("dumpLongTermRing: compacted %u records\n", (unsigned)n);
}

// Boot restore. Reads the newest LONGTERM_RING_SIZE records (file may hold more — appended tail)
// into linear order (tail=0, head=count) and keeps the file as the durable copy. A plain
// writePsramBlob snapshot from an older firmware restores identically. No-op if absent / mismatched.
void restoreLongTermRing() {
  if (!longTermRing) return;
  uint32_t fileRecords = 0;
  uint32_t n = restoreRingBlob(LONGTERM_BACKUP_PATH, LONGTERM_BACKUP_MAGIC, LONGTERM_BACKUP_VER,
                               longTermRing, sizeof(LongTermRecord), LONGTERM_RING_SIZE, &fileRecords);
  if (n == 0) { longTermFileRecords = 0; longTermPushSeq = 0; longTermFlushedSeq = 0; return; }
  longTermCount = (uint16_t)n;
  longTermHead = (n >= LONGTERM_RING_SIZE) ? 0 : (uint16_t)n;
  longTermLastEpoch = 0;   // newest non-zero record epoch = exactly how pushLongTermRecord maintains it
  for (int i = (int)n - 1; i >= 0; i--) { if (longTermRing[i].timestamp) { longTermLastEpoch = (time_t)longTermRing[i].timestamp; break; } }
  longTermFileRecords = fileRecords;              // may exceed ring size → next capped flush compacts
  longTermPushSeq = longTermFlushedSeq = 0;       // file already holds every restored record
  prev_longTermHead = longTermHead;
  Serial.printf("restoreLongTermRing: restored %u of %u file records\n", (unsigned)n, (unsigned)fileRecords);
}

// ===== Zero-drift characterization log (temporary diagnostic) =====
// Flush the PSRAM ring to LittleFS so a reboot doesn't lose the session. Field-off only (the
// caller gates it). Mirrors dumpLongTermRing: the periodic flush APPENDS only samples new since
// the last flush; a full writePsramBlob compaction runs only for the first write, past ~2× the
// ring, or on append failure.
void dumpZeroLog() {
  if (dbgRingsSynthetic) return;   // fillmax/clearmax: RAM ring is synthetic/empty — keep the real flash blob
  if (!zeroLogRing || zeroLogCount == 0) return;
  uint32_t delta = zeroLogPushSeq - zeroLogFlushedSeq;   // samples new since last flush (unsigned wrap-safe)
  if (delta == 0 && zeroLogFileRecords > 0) return;      // file already holds every sample — nothing to persist
  if (zeroLogFileRecords > 0 && delta > 0 && delta <= zeroLogCount
      && zeroLogFileRecords + delta <= (uint32_t)ZEROLOG_RING_SIZE * 2
      && fsExists(ZEROLOG_PATH)
      && appendRingBlob(ZEROLOG_PATH, zeroLogRing, sizeof(ZeroLogRecord),
                        ZEROLOG_RING_SIZE, zeroLogHead, delta) > 0) {
    zeroLogFileRecords += delta;
    zeroLogFlushedSeq   = zeroLogPushSeq;
    prev_zeroLogHead    = zeroLogHead;
    return;
  }
  uint16_t startIdx = (zeroLogCount < ZEROLOG_RING_SIZE) ? 0 : zeroLogHead;  // oldest record
  uint32_t n = writePsramBlob(ZEROLOG_PATH, ZEROLOG_MAGIC, ZEROLOG_VER, 0,
                              zeroLogRing, sizeof(ZeroLogRecord),
                              ZEROLOG_RING_SIZE, startIdx, zeroLogCount);
  if (n > 0) {
    zeroLogFileRecords = n;
    zeroLogFlushedSeq  = zeroLogPushSeq;
    prev_zeroLogHead   = zeroLogHead;
  }
}

// Boot init: load the enable toggle, alloc the PSRAM ring, restore any persisted records
// (linearized tail=0, head=count). Called from setup() after kneeLearnInit().
void zeroLogInit() {
  // Default ON (permanent subsystem — it is the temp-comp correction's data source). A unit that
  // previously stored "0" keeps its choice; only first-ever boot seeds the new default.
  if (!settingExists(NK_ZeroLogEnable)) settingWrite(NK_ZeroLogEnable, "1");
  else ZeroLogEnable = (settingRead(NK_ZeroLogEnable).toInt() != 0);
  if (!zeroLogRing) {
    zeroLogRing = (ZeroLogRecord *)ps_malloc(ZEROLOG_RING_SIZE * sizeof(ZeroLogRecord));
    if (!zeroLogRing) { Serial.println("FATAL: zeroLogRing ps_malloc failed"); return; }
    memset(zeroLogRing, 0, ZEROLOG_RING_SIZE * sizeof(ZeroLogRecord));
  }
  uint32_t fileRecords = 0;
  uint32_t n = restoreRingBlob(ZEROLOG_PATH, ZEROLOG_MAGIC, ZEROLOG_VER,
                               zeroLogRing, sizeof(ZeroLogRecord), ZEROLOG_RING_SIZE, &fileRecords);
  zeroLogCount = (uint16_t)n;
  zeroLogHead  = (n >= ZEROLOG_RING_SIZE) ? 0 : (uint16_t)n;
  zeroLogFileRecords = fileRecords;               // may exceed ring size → next capped flush compacts
  zeroLogPushSeq = zeroLogFlushedSeq = 0;         // file already holds every restored record
  prev_zeroLogHead = zeroLogHead;
  if (n > 0) Serial.printf("zeroLogInit: restored %u of %u file records\n", (unsigned)n, (unsigned)fileRecords);
}

// Dashboard "Reset Log" handler: empty the ring + delete the LittleFS backup (fresh session).
void zeroLogResetAll() {
  zeroLogHead = 0; zeroLogCount = 0; prev_zeroLogHead = 0xFFFF;
  zeroLogFileRecords = 0; zeroLogPushSeq = 0; zeroLogFlushedSeq = 0;   // file removed → next flush compacts fresh
  if (zeroLogRing) memset(zeroLogRing, 0, ZEROLOG_RING_SIZE * sizeof(ZeroLogRecord));
  fsTakeLock();
  LittleFS.remove(ZEROLOG_PATH);  // raw call — fsExists() would re-take the non-recursive fsMutex and block 5 s
  fsReleaseLock();
  Serial.println("Cleared zero-drift log + backup");
}

// Called every loop pass. Samples while field-off >= 5 s (1 s spinning / 10 min idle) into the ring
// (RAM only), and flushes to flash field-off only (60s-settled, every 30 min, new-data-gated — same
// flash discipline as dumpLongTermRing). The flush is the only flash write; sampling is trivial.
void zeroLogService() {
  if (!zeroLogRing || !ZeroLogEnable) return;
  uint32_t now = millis();

  // SAMPLE — own short field-off timer. fieldOffSettled() has a 60 s floor (it gates flash writes,
  // a different job), so we track field-off start here for the 5 s sampling threshold.
  static uint32_t lastFieldOnMs = 0;
  if (fieldActiveStatus > 0) lastFieldOnMs = now;
  if ((now - lastFieldOnMs) >= ZEROLOG_FIELDOFF_MIN_MS) {
    uint32_t interval = (RPM >= 200) ? ZEROLOG_RUN_INTERVAL_MS : ZEROLOG_IDLE_INTERVAL_MS;
    static uint32_t lastSampleMs = 0;
    if (lastSampleMs == 0 || (now - lastSampleMs) >= interval) {
      // Right after a reboot AlternatorTemperatureF is still NaN (DS18B20 not read yet).
      // Defer the sample (don't advance lastSampleMs) so it captures a real temperature
      // instead of a fabricated one — the sensor reads within ~1 s of boot, so this adds
      // no visible gap. Past the grace window the sensor is absent, so log ZEROLOG_TEMP_BLANK
      // (rendered as an empty CSV cell) rather than block the log forever or invent a value.
      bool tempValid = !isnan(AlternatorTemperatureF);
      if (!tempValid && now < ZEROLOG_TEMP_GRACE_MS) return;

      lastSampleMs = now;
      ZeroLogRecord &r = zeroLogRing[zeroLogHead];
      r.epoch       = (uint32_t)getCurrentTimestamp();
      r.amps        = MeasuredAmps;
      r.p2pAmps     = altAmpsP2P;
      r.rpm         = (int16_t)constrain((long)lroundf(RPM), -32768L, 32767L);
      r.battVx100   = (int16_t)lroundf(getBatteryVoltage() * 100.0f);
      r.altTempFx10 = tempValid ? (int16_t)lroundf(AlternatorTemperatureF * 10.0f)
                                : ZEROLOG_TEMP_BLANK;   // sensor absent → blank, never fabricated
      bool boardValid = !isnan(ambientTemp) && !isinf(ambientTemp);
      r.boardTempFx10 = boardValid ? (int16_t)lroundf(ambientTemp * 10.0f)
                                   : ZEROLOG_TEMP_BLANK; // board sensor (BMP388) absent → blank
      zeroLogHead = (zeroLogHead + 1) % ZEROLOG_RING_SIZE;
      if (zeroLogCount < ZEROLOG_RING_SIZE) zeroLogCount++;
      zeroLogPushSeq++;   // drives the append-flush delta (samples new since last flush)
    }
  }

  // FLUSH — field gate physically cut (fieldCutSettled, never the duty-based
  // fieldOffSettled), every 30 min, only when the ring actually changed. The ring wraps by
  // design, so deferred flushes cost only power-cut durability of this diagnostic.
  static bool prevFieldOff = false;
  static uint32_t lastFlushMs = 0;
  bool ffSettled = fieldCutSettled(0);
  bool rising    = (ffSettled && !prevFieldOff);
  bool periodic  = (ffSettled && (now - lastFlushMs >= ZEROLOG_FLUSH_MS));
  if ((rising || periodic) && prev_zeroLogHead != zeroLogHead) {
    dumpZeroLog();
    lastFlushMs = now;
  }
  prevFieldOff = ffSettled;
}

// Dashboard "Clear" button handler. Empties the PSRAM ring and removes the
// LittleFS shutdown-dump file so nothing comes back on next boot.
void clearSensorBuffer() {
  // Runs on the async web task — third context touching the ring indices.
  portENTER_CRITICAL(&sensorRingMux);
  sensorRingHead = 0;
  sensorRingTail = 0;
  sensorRingCount = 0;
  sensorRingInFlightIndex = -1;
  sensorRingAnnouncedEmpty = false;
  bufferedRecordCount = 0;
  portEXIT_CRITICAL(&sensorRingMux);

  fsTakeLock();
  LittleFS.remove(SENSOR_RING_BACKUP_PATH);  // raw call — fsExists() would re-take the non-recursive fsMutex and block 5 s
  fsReleaseLock();

  Serial.println("Cleared sensor ring + shutdown backup");
}

// ===== ESP32 CONFIG SNAPSHOT FUNCTIONS =====
// Build config snapshot JSON: { device_uid, token, snapshot_timestamp, settings: {…}, state: {…} }
// settings = the full manifest + alt/perf-registry set from manifestConfigObject (~260 keys, grows
// automatically as settings are added, stored verbatim as jsonb) → device_settings_snapshots,
// owner-visible only, never leaderboards. state → device_state_daily + an UPSERT of the lifetime
// fields into device_statistics. Field names and grouping mirror configsnapshot_picker.html.
// snprintf's size argument is size_t: once offset passes the buffer end cfgRemain(offset) goes
// negative and converts to an enormous size_t, defeating the bound. Clamp to 0.
static inline size_t cfgRemain(int off) {
  return (off >= CONFIG_PAYLOAD_SIZE) ? (size_t)0 : (size_t)(CONFIG_PAYLOAD_SIZE - off);
}

bool buildConfigPayload() {
  time_t now_ts = time(NULL);
  const char *timestampStr = formatTimestamp(now_ts);

  int offset = snprintf(configPayloadBuffer, CONFIG_PAYLOAD_SIZE,
    "{\"device_uid\":\"%s\",\"token\":\"%s\",\"snapshot_timestamp\":\"%s\","
    // payload_v = ingest payload schema version; bump when this body's shape changes.
    // Edge fn destructures named keys so it ignores this; present for version tracing.
    // v2 adds state.ov_telemetry (lifetime OV histogram + counters → device_statistics jsonb).
    // v3 nests settings.tables (RPM/fuel tables) so the daily settings jsonb equals the full
    // /exportConfig record, and update-config-snapshot projects the vessel keys → user_profiles.
    // v4 renames settings.commissioning_results → learned_state and drops its stress_test
    // (moved to the commissioning ledger, COMMISSIONING_LEDGER_SPEC.md).
    "\"payload_v\":4,"
    "\"settings\":",
    device_id_hex, authToken.c_str(), timestampStr);
  if (offset < 0 || offset >= CONFIG_PAYLOAD_SIZE) return false;

  // ─── Settings ───────────────────────────────────────────────────────────────
  // Manifest-driven: the complete settings set as raw NVS strings,
  // sharing CONFIG_MANIFEST (8_functions.ino) with /exportConfig — one source of truth,
  // so the fleet config snapshot can never drift behind the dashboard as settings are
  // added. update-config-snapshot stores it verbatim as one jsonb column (no per-key DB).
  // tables{} nests INSIDE settings (like learned_state) so the cloud record is the
  // complete boat with zero cloud schema change.
  {
    String cfgObj = manifestConfigObject();
    cfgObj.remove(cfgObj.length() - 1);   // reopen the object to append the tables section
    offset += snprintf(configPayloadBuffer + offset, cfgRemain(offset), "%s", cfgObj.c_str());
    String tblObj = exportTablesObject();
    offset += snprintf(configPayloadBuffer + offset, cfgRemain(offset), ",\"tables\":%s}", tblObj.c_str());
  }

  // ─── State ─────────────────────────────────────────────────────────────────
  offset += snprintf(configPayloadBuffer + offset, cfgRemain(offset), ",\"state\":{");

  // Lifetime accumulators — every field also UPSERTs into device_statistics.
  // eng_hrs / alt_hrs sent as RAW SECONDS (firmware-canonical).
  offset += snprintf(configPayloadBuffer + offset, cfgRemain(offset),
    "\"voltage_avg_lifetime\":%.4f,\"voltage_sample_time\":%lu,"
    "\"soc_avg_lifetime\":%.4f,\"soc_sample_time\":%lu,"
    "\"speed_avg_lifetime\":%.4f,\"speed_sample_time\":%lu,"
    "\"eng_hrs\":%lu,\"alt_hrs\":%lu,\"eng_cycles\":%lu,"
    "\"eng_fuel\":%.3f,\"alt_fuel\":%.3f,\"charge_cycles\":%.2f,"
    "\"solar_kwh_alltime\":%.3f,\"charged_energy_alltime\":%.3f,"
    "\"discharged_energy_alltime\":%.3f,\"alt_charged_energy_alltime\":%.3f,"
    "\"total_dist_alltime\":%.3f,\"sailing_days_alltime\":%.3f,\"sailing_ratio\":%.3f,"
    "\"total_overheats\":%lu,\"total_safe_hours\":%.3f,"
    "\"longest_single_trip_nm_alltime\":%.3f,\"max_24hr_distance\":%.3f,"
    "\"deepest_anchorage_ft\":%.2f,"
    "\"best_upwind_vmg_alltime\":%.2f,\"longest_gale_duration_hours_alltime\":%.3f,"
    "\"sailing_dist_alltime\":%.3f,\"alt_power_max_alltime_w\":%.3f,\"solar_power_max_alltime_w\":%.3f",
    AvgVoltage_AllTime, (unsigned long)totalVoltageSampleTime_AllTime,
    AvgSOC_AllTime, (unsigned long)totalSocSampleTime_AllTime,
    AvgSpeed_AllTime, (unsigned long)totalSpeedSampleTime_AllTime,
    (unsigned long)EngineRunTime_AllTime, (unsigned long)AlternatorOnTime_AllTime, (unsigned long)EngineCycles_AllTime,
    EngineFuelUsed_AllTime, AlternatorFuelUsed_AllTime, ChargeCycles_AllTime,
    SolarChargedEnergy_AllTime / 1000.0, ChargedEnergy_AllTime / 1000.0,
    DischargedEnergy_AllTime / 1000.0, AlternatorChargedEnergy_AllTime / 1000.0,
    TotalDistance_AllTime, sailing_days_alltime, sailing_ratio,
    (unsigned long)totalOverheats, (double)totalSafeHours,
    LongestSingleTrip_Nm_AllTime, Max24hrDistance_AllTime,
    DeepestAnchorage_Ft_AllTime,
    best_upwind_vmg_alltime, longest_gale_duration_hours_alltime,
    sailing_dist_alltime, alt_power_max_alltime_w, solar_power_max_alltime_w);

  // Session totals (point values at upload time; owner-visible only, no leaderboard)
  offset += snprintf(configPayloadBuffer + offset, cfgRemain(offset),
    ",\"total_dist_session\":%.3f,\"solar_kwh_session\":%.3f,"
    "\"charged_energy_session\":%.3f,\"discharged_energy_session\":%.3f,"
    "\"alt_charged_energy_session\":%.3f",
    TotalDistance, SolarChargedEnergy / 1000.0,
    ChargedEnergy / 1000.0, DischargedEnergy / 1000.0,
    AlternatorChargedEnergy / 1000.0);

  // Slow-changing runtime scalars
  offset += snprintf(configPayloadBuffer + offset, cfgRemain(offset),
    ",\"current_weather_mode\":%d,\"uv_today\":%.2f",
    currentWeatherMode, UVToday);

  // Fast alt-current channel: per-session worst scalars (fleet aggregation, consumer 5).
  // CLOUD CONTRACT: update-config-snapshot spreads EVERY state key into the
  // device_state_daily INSERT (flattenForInsert) — an unknown key 500s the whole daily
  // snapshot. The fa_pkpk_worst_session / fa_peak_worst_a_session / fa_peak_worst_hz_session
  // columns MUST exist in device_state_daily before this firmware is flashed.
  offset += snprintf(configPayloadBuffer + offset, cfgRemain(offset),
    ",\"fa_pkpk_worst_session\":%.2f,\"fa_peak_worst_a_session\":%.2f,\"fa_peak_worst_hz_session\":%.1f",
    faSesPkpkWorstA, faSesPeakWorstA, faSesPeakWorstHz);

  // Lifetime OV excursion telemetry (RTC-RAM histogram + counters — OvTelemetry in Xregulator.ino).
  // One nested object → device_statistics.ov_telemetry (jsonb); the edge fn must route it there and
  // EXCLUDE it from the device_state_daily flatten (a nested value in that INSERT 500s the snapshot).
  // time_ms as decimal strings (uint64 dwell is beyond JS 53-bit integers). After a true power cut
  // the struct zeroes and the next snapshot reports lower values — edge fn overwrites as-is, accepted.
  offset += snprintf(configPayloadBuffer + offset, cfgRemain(offset),
    ",\"ov_telemetry\":{\"soft\":%lu,\"sw_hard\":%lu,\"ina\":%lu,\"kd\":%lu,"
    "\"tier_low\":%lu,\"tier_mid\":%lu,\"bulk\":%.2f,\"k\":%.2f,"
    "\"bins_fine\":%d,\"bins_coarse\":%d,\"events\":[",
    (unsigned long)g_ovTel.softExceedCount, (unsigned long)g_ovTel.swHardCutCount,
    (unsigned long)g_ovTel.inaCutCount, (unsigned long)g_ovTel.kdEventCount,
    (unsigned long)g_ovTel.tierLowCutCount, (unsigned long)g_ovTel.tierMidCutCount,
    BulkVoltage, (float)SYSTEM_VOLTAGE_CLASS / 12.0f,
    OV_HIST_FINE_BINS, OV_HIST_COARSE_BINS);
  for (int i = 0; i < OV_HIST_BINS; i++)
    offset += snprintf(configPayloadBuffer + offset, cfgRemain(offset), "%s%lu", i ? "," : "", (unsigned long)g_ovTel.events[i]);
  offset += snprintf(configPayloadBuffer + offset, cfgRemain(offset), "],\"time_ms\":[");
  for (int i = 0; i < OV_HIST_BINS; i++)
    offset += snprintf(configPayloadBuffer + offset, cfgRemain(offset), "%s\"%llu\"", i ? "," : "", (unsigned long long)g_ovTel.timeMs[i]);
  offset += snprintf(configPayloadBuffer + offset, cfgRemain(offset), "]}");

  // Control Accuracy numbers since the last MANUAL reset. The post-upload auto-reset is disabled
  // (see the upload-success branch below), so consecutive daily snapshots are a RUNNING TOTAL, not
  // independent one-day samples — differencing two rows is what isolates a day.
  // CLOUD CONTRACT: like every state key, these are spread into the device_state_daily INSERT —
  // the 6 columns (acc_cur_rms_a, acc_cur_peak_a, acc_volt_rms_mv, acc_volt_peak_mv,
  // acc_therm_rms_f, acc_therm_peak_f) MUST exist in device_state_daily before this firmware ships,
  // or the whole daily snapshot 500s.
  // v4 SEMANTICS BREAK (fw > 0.0.46): the *_rms columns now carry Tracking/containment % (0-100,
  // in-band fraction of challenged/binding time — spec CONTROL_ACCURACY_V4_ROUTINE_SPEC.md); the
  // *_peak columns keep their physical units (worst damaging overshoot: A, 12V-equiv mV, °F over
  // limit). NOT trend-continuous with v2/v3 rows — filter fleet trends on firmware_version_int.
  // -1 = never challenged today (distinguishes "not observed" from "0% tracking").
  offset += snprintf(configPayloadBuffer + offset, cfgRemain(offset),
    ",\"acc_cur_rms_a\":%.2f,\"acc_cur_peak_a\":%.2f,"
    "\"acc_volt_rms_mv\":%.0f,\"acc_volt_peak_mv\":%.0f,"
    "\"acc_therm_rms_f\":%.2f,\"acc_therm_peak_f\":%.2f",
    (accCur4.activeSec > 0.5) ? (float)(100.0 * accCur4.inbandActiveSec / accCur4.activeSec) : -1.0f,
    accCur4.worstOver,
    (accVolt4.activeSec > 0.5) ? (float)(100.0 * accVolt4.inbandActiveSec / accVolt4.activeSec) : -1.0f,
    accVolt4.worstOver * 1000.0f,
    (accThermBindingSec > 0.5) ? (float)(100.0 * accThermInbandSec / accThermBindingSec) : -1.0f,
    accThermWorstOverF);

  // Close state, close root object
  offset += snprintf(configPayloadBuffer + offset, cfgRemain(offset), "}}");

  if (offset >= CONFIG_PAYLOAD_SIZE - 1) {
    Serial.println("ERROR: Config payload truncated");
    return false;
  }
  return true;
}
bool executeUploadConfig(const char *payload) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (WiFi.RSSI() < -80) return false;
  if (!isRegistered || authToken.isEmpty()) return false;

  WiFiClientSecure client;
  client.setInsecure();
  // setTimeout omitted: Stream::setTimeout is ms (not seconds as some docs claim) and
  // the read loops below use available()+read() polling with explicit millis() deadlines,
  // so the Stream-level timeout doesn't gate anything here.
  client.setHandshakeTimeout(HANDSHAKE_TIMEOUT / 1000);  // this API takes seconds

  uint32_t start = millis();
  esp_task_wdt_reset();

  IPAddress hostIP;  // pre-resolve: keep DNS out of the connect's unfed-WDT window (see executeUploadPayload)
  if (!WiFi.hostByName(host, hostIP)) {
    Serial.println("Config: DNS fail");
    return false;
  }
  esp_task_wdt_reset();

  // Serial.println("Config: Connecting...");
  // Hard-bounded TLS connect
  if (!client.connect(host, port, CONNECT_TIMEOUT)) {
    Serial.println("Config: Connect fail");
    client.stop();
    return false;
  }

  // Defensive global timeout check after connect/handshake
  if (millis() - start > GLOBAL_TIMEOUT) {
    Serial.println("Config: Connect exceeded global timeout");
    client.stop();
    return false;
  }

  esp_task_wdt_reset();

  int headerBytes = client.printf(
    "POST /functions/v1/update-config-snapshot HTTP/1.1\r\n"
    "Host: %s\r\n"
    "Content-Type: application/json\r\n"
    "Authorization: Bearer %s\r\n"
    "Connection: close\r\n"
    "Content-Length: %u\r\n\r\n",
    host,
    SUPABASE_ANON_KEY,
    (unsigned)strlen(payload));

  // ===== Stream payload directly =====
  size_t payloadLen = strlen(payload);
  size_t sent = client.write((const uint8_t *)payload, payloadLen);
  if (sent != payloadLen) {
    Serial.println("Config: Payload send fail");
    client.stop();
    return false;
  }
  esp_task_wdt_reset();

  // ===== Read status line + drain headers (no String, no header parsing) =====
  int httpCode = 0;
  uint32_t readStart = millis();

  // 1) Read ONLY the first line (status line). READ_TIMEOUT is idle — each byte restarts it.
  char statusBuf[64];
  size_t statusLen = 0;

  while (client.connected() && (millis() - readStart < READ_TIMEOUT)) {
    esp_task_wdt_reset();

    while (client.available()) {
      char c = (char)client.read();
      readStart = millis();

      if (statusLen < sizeof(statusBuf) - 1) {
        statusBuf[statusLen++] = c;
      }

      if (c == '\n') {
        statusBuf[statusLen] = '\0';

        if (strncmp(statusBuf, "HTTP/", 5) == 0) {
          const char *sp = strchr(statusBuf, ' ');
          if (sp) httpCode = atoi(sp + 1);
        }
        goto drain_headers_cfg;
      }
    }

    if (millis() - start > GLOBAL_TIMEOUT) break;
    delay(1);
  }

drain_headers_cfg:
  // 2) Drain headers until CRLFCRLF (end of headers). No logging.
  {
    uint32_t drainStart = millis();
    uint8_t state = 0;  // \r \n \r \n

    while (client.connected() && (millis() - drainStart < READ_TIMEOUT)) {
      esp_task_wdt_reset();

      while (client.available()) {
        char c = (char)client.read();
        drainStart = millis();

        if (state == 0 && c == '\r') state = 1;
        else if (state == 1 && c == '\n') state = 2;
        else if (state == 2 && c == '\r') state = 3;
        else if (state == 3 && c == '\n') goto done_headers_cfg;
        else state = 0;
      }

      if (millis() - start > GLOBAL_TIMEOUT) break;
      delay(1);
    }
  }

done_headers_cfg:
  client.stop();

  esp_task_wdt_reset();

  if (httpCode == 0) {
    Serial.println("Config: No response received (timeout)");
    queueConsoleMessage("Config upload failed (timeout)");
    return false;
  }

  //Serial.printf("Config: HTTP %d\n", httpCode); //this was useful debugging

  // ===== Handle response =====
  bool success = (httpCode == 200);
  if (success) {
    queueConsoleMessage("Config snapshot uploaded");
    // Control Accuracy counters accumulate until a manual /resetAccuracyScores — the daily
    // post-upload auto-reset is disabled so the displayed window matches what the UI copy promises.
    // resetAccuracyScores(false);
  } else if (httpCode > 0) {
    snprintf(messageBuffer, MESSAGE_BUFFER_SIZE, "Config upload failed HTTP %d", httpCode);
    queueConsoleMessage(messageBuffer);
  }

  return success;
}

// Mirrors executeUploadConfig() exactly (proven HTTPS pattern) — only the endpoint + result
// signaling differ. Ships a commissioning-ledger event batch to log-commissioning-event and
// stamps cxLedgerUpState for the Core-1 drain service: 2 = cloud has the rows, trim the sent
// file prefix; -1 = transport/server failure, leave the file and retry. HTTP 400 also stamps 2:
// a batch the edge fn rejects as malformed would poison the queue forever if retried — the
// (device_uid, seq) gap it leaves is the audit trail.
bool executeUploadCxLedger(const char *payload) {
  if (WiFi.status() != WL_CONNECTED) { cxLedgerUpState = -1; return false; }
  if (WiFi.RSSI() < -80) { cxLedgerUpState = -1; return false; }
  if (!isRegistered || authToken.isEmpty()) { cxLedgerUpState = -1; return false; }

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(HANDSHAKE_TIMEOUT / 1000);  // this API takes seconds

  uint32_t start = millis();
  esp_task_wdt_reset();

  IPAddress hostIP;  // pre-resolve: keep DNS out of the connect's unfed-WDT window (see executeUploadPayload)
  if (!WiFi.hostByName(host, hostIP)) {
    Serial.println("CxLedger: DNS fail");
    cxLedgerUpState = -1;
    return false;
  }
  esp_task_wdt_reset();

  if (!client.connect(host, port, CONNECT_TIMEOUT)) {
    Serial.println("CxLedger: Connect fail");
    client.stop();
    cxLedgerUpState = -1;
    return false;
  }
  if (millis() - start > GLOBAL_TIMEOUT) {
    client.stop();
    cxLedgerUpState = -1;
    return false;
  }
  esp_task_wdt_reset();

  client.printf(
    "POST /functions/v1/log-commissioning-event HTTP/1.1\r\n"
    "Host: %s\r\n"
    "Content-Type: application/json\r\n"
    "Authorization: Bearer %s\r\n"
    "Connection: close\r\n"
    "Content-Length: %u\r\n\r\n",
    host,
    SUPABASE_ANON_KEY,
    (unsigned)strlen(payload));

  size_t payloadLen = strlen(payload);
  size_t sent = client.write((const uint8_t *)payload, payloadLen);
  if (sent != payloadLen) {
    Serial.println("CxLedger: Payload send fail");
    client.stop();
    cxLedgerUpState = -1;
    return false;
  }
  esp_task_wdt_reset();

  int httpCode = 0;
  uint32_t readStart = millis();
  char statusBuf[64];
  size_t statusLen = 0;

  while (client.connected() && (millis() - readStart < READ_TIMEOUT)) {
    esp_task_wdt_reset();
    while (client.available()) {
      char c = (char)client.read();
      readStart = millis();
      if (statusLen < sizeof(statusBuf) - 1) {
        statusBuf[statusLen++] = c;
      }
      if (c == '\n') {
        statusBuf[statusLen] = '\0';
        if (strncmp(statusBuf, "HTTP/", 5) == 0) {
          const char *sp = strchr(statusBuf, ' ');
          if (sp) httpCode = atoi(sp + 1);
        }
        goto drain_headers_cxl;
      }
    }
    if (millis() - start > GLOBAL_TIMEOUT) break;
    delay(1);
  }

drain_headers_cxl:
  {
    uint32_t drainStart = millis();
    uint8_t state = 0;  // \r \n \r \n
    while (client.connected() && (millis() - drainStart < READ_TIMEOUT)) {
      esp_task_wdt_reset();
      while (client.available()) {
        char c = (char)client.read();
        drainStart = millis();
        if (state == 0 && c == '\r') state = 1;
        else if (state == 1 && c == '\n') state = 2;
        else if (state == 2 && c == '\r') state = 3;
        else if (state == 3 && c == '\n') goto done_headers_cxl;
        else state = 0;
      }
      if (millis() - start > GLOBAL_TIMEOUT) break;
      delay(1);
    }
  }

done_headers_cxl:
  client.stop();
  esp_task_wdt_reset();

  if (httpCode == 200) {
    queueConsoleMessage("Commissioning ledger uploaded");
    cxLedgerUpState = 2;
    return true;
  }
  if (httpCode == 400) {
    queueConsoleMessage("Commissioning ledger: batch rejected as malformed — discarded");
    cxLedgerUpState = 2;   // drop the poison batch; seq gap documents it
    return false;
  }
  if (httpCode == 0) Serial.println("CxLedger: No response received (timeout)");
  else Serial.printf("CxLedger: HTTP %d\n", httpCode);
  cxLedgerUpState = -1;
  return false;
}

// Shared response-body buffer for the two front sync-backs (boat + alt). PSRAM, allocated once and
// kept (both sync paths run sequentially in the same httpsTask worker). Must hold the boat pair at
// full cap (2 × 4096 pts). A read that fills it completely is reported and NOT parsed — a truncated
// front must never wholesale-replace a good local one.
static char *syncBody = nullptr;
#define SYNC_BODY_CAP 524288u
static char *syncBodyGet() {
  if (!syncBody) syncBody = (char *)ps_malloc(SYNC_BODY_CAP);
  return syncBody;
}

// Drains HTTP response headers (already past the status line) up to the blank line, capturing
// Content-Length (-1 when absent / headers never completed) and Transfer-Encoding: chunked. Lets
// the sync-back callers tell a mid-body connection drop from a complete short body — BEFRONT1 has
// no terminator, so a truncated body parses cleanly and would wholesale-replace the local front.
// (Probed 2026-08-14: the Supabase/Cloudflare edge over HTTP/1.1 sends chunked, no Content-Length,
// so the chunked path is the live one; Content-Length is kept for framing changes upstream.)
static long drainHeadersCaptureLen(WiFiClientSecure &client, uint32_t startMs, bool *chunked) {
  uint32_t drainStart = millis();
  char line[48];
  size_t ll = 0;
  long contentLen = -1;
  *chunked = false;
  while (client.connected() && (millis() - drainStart < READ_TIMEOUT)) {
    esp_task_wdt_reset();
    while (client.available()) {
      char c = (char)client.read();
      drainStart = millis();
      if (c == '\n') {
        if (ll && line[ll - 1] == '\r') ll--;
        line[ll] = '\0';
        if (ll == 0) return contentLen;   // blank line = end of headers
        if (strncasecmp(line, "Content-Length:", 15) == 0) contentLen = atol(line + 15);
        else if (strncasecmp(line, "Transfer-Encoding:", 18) == 0 && strcasestr(line + 18, "chunked")) *chunked = true;
        ll = 0;
      } else if (ll < sizeof(line) - 1) {
        line[ll++] = c;   // overlong header lines just lose their tail — the two we match never are
      }
    }
    if (millis() - startMs > GLOBAL_TIMEOUT) break;
    delay(1);
  }
  return contentLen;
}

// Reads a response body into buf (cap-1 usable), DE-CHUNKING when the response is chunked — the raw
// reader used to store the chunk framing (hex size lines) into the CSV, and a chunk boundary landing
// mid-row could admit a column-shifted garbage front point. Sets *complete=false when a chunked
// body's terminating 0-chunk never arrived, or a Content-Length body came up short: the caller must
// not hand an incomplete body to the wholesale-replacing front ingest.
static size_t readSyncBody(WiFiClientSecure &client, char *buf, size_t cap, bool chunked,
                           long contentLen, uint32_t startMs, bool *complete) {
  size_t bl = 0;
  bool done = false;
  uint32_t bodyStart = millis();
  uint8_t cs = 0;       // chunked state: 0 = size line, 1 = payload, 2/3 = CR/LF after payload
  long remain = 0;
  bool sawSize = false, inExt = false;
  while (!done && client.connected() && bl < cap - 1 && (millis() - bodyStart < READ_TIMEOUT)) {
    esp_task_wdt_reset();
    if (client.available()) bodyStart = millis();
    while (!done && client.available() && bl < cap - 1) {
      char c = (char)client.read();
      if (!chunked) { buf[bl++] = c; continue; }
      switch (cs) {
        case 0: {   // hex size line; ';' starts an extension we ignore
          if (c == '\n') {
            if (sawSize) { if (remain == 0) done = true; else cs = 1; }
            sawSize = inExt = false;
          } else if (c == ';') inExt = true;
          else if (!inExt && c != '\r') {
            int v = (c >= '0' && c <= '9') ? c - '0'
                  : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                  : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (v >= 0) { remain = remain * 16 + v; sawSize = true; }
          }
          break;
        }
        case 1:
          buf[bl++] = c;
          if (--remain == 0) cs = 2;
          break;
        case 2:
          cs = (c == '\n') ? 0 : 3;   // tolerate bare LF
          break;
        case 3:
          if (c == '\n') cs = 0;
          break;
      }
    }
    if (millis() - startMs > GLOBAL_TIMEOUT) break;
    delay(1);
  }
  *complete = chunked ? done : (contentLen < 0 || (long)bl == contentLen);
  return bl;
}

// Mirrors executeUploadConfig() exactly (proven HTTPS pattern) — only the endpoint + log labels
// differ. Uploads the boat-performance aggregates to the update-boat-performance edge function.
bool executeUploadBoatPerf(const char *payload) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (WiFi.RSSI() < -80) return false;
  if (!isRegistered || authToken.isEmpty()) return false;

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(HANDSHAKE_TIMEOUT / 1000);  // this API takes seconds

  uint32_t start = millis();
  esp_task_wdt_reset();

  IPAddress hostIP;  // pre-resolve: keep DNS out of the connect's unfed-WDT window (see executeUploadPayload)
  if (!WiFi.hostByName(host, hostIP)) {
    Serial.println("BoatPerf: DNS fail");
    return false;
  }
  esp_task_wdt_reset();

  if (!client.connect(host, port, CONNECT_TIMEOUT)) {
    Serial.println("BoatPerf: Connect fail");
    client.stop();
    return false;
  }
  if (millis() - start > GLOBAL_TIMEOUT) {
    client.stop();
    return false;
  }
  esp_task_wdt_reset();

  client.printf(
    "POST /functions/v1/update-boat-performance HTTP/1.1\r\n"
    "Host: %s\r\n"
    "Content-Type: application/json\r\n"
    "Authorization: Bearer %s\r\n"
    "Connection: close\r\n"
    "Content-Length: %u\r\n\r\n",
    host, SUPABASE_ANON_KEY, (unsigned)strlen(payload));

  size_t payloadLen = strlen(payload);
  if (client.write((const uint8_t *)payload, payloadLen) != payloadLen) {
    Serial.println("BoatPerf: Payload send fail");
    client.stop();
    return false;
  }
  esp_task_wdt_reset();

  int httpCode = 0;
  uint32_t readStart = millis();
  char statusBuf[64];
  size_t statusLen = 0;
  while (client.connected() && (millis() - readStart < READ_TIMEOUT)) {
    esp_task_wdt_reset();
    while (client.available()) {
      char c = (char)client.read();
      readStart = millis();
      if (statusLen < sizeof(statusBuf) - 1) statusBuf[statusLen++] = c;
      if (c == '\n') {
        statusBuf[statusLen] = '\0';
        if (strncmp(statusBuf, "HTTP/", 5) == 0) {
          const char *sp = strchr(statusBuf, ' ');
          if (sp) httpCode = atoi(sp + 1);
        }
        goto drain_headers_bp;
      }
    }
    if (millis() - start > GLOBAL_TIMEOUT) break;
    delay(1);
  }
drain_headers_bp:
  long bpContentLen;
  bool bpChunked, bpComplete;
  bpContentLen = drainHeadersCaptureLen(client, start, &bpChunked);
  // Read the response BODY (both pruned BEFRONT1 blocks) into the shared PSRAM sync buffer.
  {
    char *bpBody = syncBodyGet();
    if (!bpBody) { client.stop(); return false; }   // PSRAM alloc failed — skip, retry next cycle
    size_t bl = readSyncBody(client, bpBody, SYNC_BODY_CAP, bpChunked, bpContentLen, start, &bpComplete);
    bpBody[bl] = '\0';
    client.stop();
    esp_task_wdt_reset();

    bool success = (httpCode == 200);
    if (success && bl >= SYNC_BODY_CAP - 1) {
      // Buffer filled completely → the front CSV may be cut mid-stream. Never parse a possibly-
      // truncated front (the ingest replaces the local front wholesale).
      queueConsoleMessage("WARN: boat-perf sync response overran the sync buffer — front NOT updated");
      if (timeIsSynced) lastBoatPerfSyncEpoch = (int64_t)time(NULL);
      perfClearPending();   // upload itself succeeded; only the sync-back is unusable
      return success;
    }
    if (success && !bpComplete) {
      // Chunked terminator (or Content-Length worth of body) never arrived → connection dropped
      // mid-body. Same rule as the overrun guard: a truncated front must never reach the
      // wholesale-replacing ingest.
      queueConsoleMessageF("WARN: boat-perf sync body truncated (%u bytes, incomplete) — front NOT updated",
                           (unsigned)bl);
      if (timeIsSynced) lastBoatPerfSyncEpoch = (int64_t)time(NULL);
      perfClearPending();   // upload itself succeeded; only the sync-back is unusable
      return success;
    }
    if (success) {
      if (timeIsSynced) lastBoatPerfSyncEpoch = (int64_t)time(NULL);   // for the "synced N ago" badge
      perfClearPending();   // cloud accepted the batch (raw history) → drop the pending points
      if (perfIngestFrontCsv(bpBody)) {
        // Field-off-gate the flash write: a LittleFS write stalls the flash cache (both cores).
        // If the field re-engaged while this upload was in flight, skip — the in-memory front is
        // already updated, and the field-off Gate-2 edge persists it later (no data loss).
        if (fieldActiveStatus <= 0) boatPerfSave();   // persist the cloud's pruned fronts
        queueConsoleMessage("Boat performance: fronts updated from cloud");
      } else {
        queueConsoleMessage("Boat performance uploaded (no front in response)");
      }
    } else if (httpCode > 0) {
      snprintf(messageBuffer, MESSAGE_BUFFER_SIZE, "BoatPerf upload failed HTTP %d", httpCode);
      queueConsoleMessage(messageBuffer);
    }
    return success;
  }
}

// Alternator (charging-system) health v2 upload — clone of executeUploadBoatPerf. POST the
// best-ever record batch, read the fitted ALTCURVE1 curve from the HTTP response BODY, and
// persist it. Same WiFiClientSecure raw-write + manual-read pattern (low internal RAM).
bool executeUploadAltHealth(const char *payload) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (WiFi.RSSI() < -80) return false;
  if (!isRegistered || authToken.isEmpty()) return false;

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(HANDSHAKE_TIMEOUT / 1000);  // this API takes seconds

  uint32_t start = millis();
  esp_task_wdt_reset();

  IPAddress hostIP;  // pre-resolve: keep DNS out of the connect's unfed-WDT window (see executeUploadPayload)
  if (!WiFi.hostByName(host, hostIP)) {
    Serial.println("AltHealth: DNS fail");
    return false;
  }
  esp_task_wdt_reset();

  if (!client.connect(host, port, CONNECT_TIMEOUT)) {
    Serial.println("AltHealth: Connect fail");
    client.stop();
    return false;
  }
  if (millis() - start > GLOBAL_TIMEOUT) {
    client.stop();
    return false;
  }
  esp_task_wdt_reset();

  client.printf(
    "POST /functions/v1/update-alt-health HTTP/1.1\r\n"
    "Host: %s\r\n"
    "Content-Type: application/json\r\n"
    "Authorization: Bearer %s\r\n"
    "Connection: close\r\n"
    "Content-Length: %u\r\n\r\n",
    host, SUPABASE_ANON_KEY, (unsigned)strlen(payload));

  size_t payloadLen = strlen(payload);
  if (client.write((const uint8_t *)payload, payloadLen) != payloadLen) {
    Serial.println("AltHealth: Payload send fail");
    client.stop();
    return false;
  }
  esp_task_wdt_reset();

  int httpCode = 0;
  uint32_t readStart = millis();
  char statusBuf[64];
  size_t statusLen = 0;
  while (client.connected() && (millis() - readStart < READ_TIMEOUT)) {
    esp_task_wdt_reset();
    while (client.available()) {
      char c = (char)client.read();
      readStart = millis();
      if (statusLen < sizeof(statusBuf) - 1) statusBuf[statusLen++] = c;
      if (c == '\n') {
        statusBuf[statusLen] = '\0';
        if (strncmp(statusBuf, "HTTP/", 5) == 0) {
          const char *sp = strchr(statusBuf, ' ');
          if (sp) httpCode = atoi(sp + 1);
        }
        goto drain_headers_ah;
      }
    }
    if (millis() - start > GLOBAL_TIMEOUT) break;
    delay(1);
  }
drain_headers_ah:
  long ahContentLen;
  bool ahChunked, ahComplete;
  ahContentLen = drainHeadersCaptureLen(client, start, &ahChunked);
  // Read the response BODY (the pruned BEFRONT1 front CSV) into the shared PSRAM sync buffer.
  {
    char *ahBody = syncBodyGet();
    if (!ahBody) { client.stop(); return false; }   // PSRAM alloc failed — skip, retry next cycle
    size_t bl = readSyncBody(client, ahBody, SYNC_BODY_CAP, ahChunked, ahContentLen, start, &ahComplete);
    ahBody[bl] = '\0';
    client.stop();
    esp_task_wdt_reset();

    bool success = (httpCode == 200);
    if (success && bl >= SYNC_BODY_CAP - 1) {
      // Buffer filled completely → the front CSV may be cut mid-stream. Never parse a possibly-
      // truncated front (the ingest replaces the local front wholesale).
      queueConsoleMessage("WARN: alt-health sync response overran the sync buffer — front NOT updated");
      if (timeIsSynced) lastAltHealthSyncEpoch = (int64_t)time(NULL);
      altClearPending();   // upload itself succeeded; only the sync-back is unusable
      return success;
    }
    if (success && !ahComplete) {
      // Chunked terminator (or Content-Length worth of body) never arrived → connection dropped
      // mid-body. Same rule as the overrun guard: a truncated front must never reach the
      // wholesale-replacing ingest.
      queueConsoleMessageF("WARN: alt-health sync body truncated (%u bytes, incomplete) — front NOT updated",
                           (unsigned)bl);
      if (timeIsSynced) lastAltHealthSyncEpoch = (int64_t)time(NULL);
      altClearPending();   // upload itself succeeded; only the sync-back is unusable
      return success;
    }
    if (success) {
      if (timeIsSynced) lastAltHealthSyncEpoch = (int64_t)time(NULL);   // for the "synced N ago" badge
      altClearPending();   // cloud accepted the batch (raw history) → drop the pending points
      if (altIngestFrontCsv(ahBody)) {
        // Field-off-gate the flash write: a LittleFS write stalls the flash cache (both cores).
        // If the field re-engaged while this upload was in flight, skip — the in-memory front is
        // already updated, and the field-off Gate-2 edge persists it later (no data loss).
        if (fieldActiveStatus <= 0) altHealthSave();   // persist the cloud's pruned front
        queueConsoleMessage("Alternator health: front updated from cloud");
      } else {
        queueConsoleMessage("Alternator health uploaded (no front in response)");
      }
    } else if (httpCode > 0) {
      snprintf(messageBuffer, MESSAGE_BUFFER_SIZE, "AltHealth upload failed HTTP %d", httpCode);
      queueConsoleMessage(messageBuffer);
    }
    return success;
  }
}

// ── App-usage analytics ──────────────────────────────────────────────────────
// Accumulator globals + design rationale live with the struct in Xregulator.ino.

// Keys arrive from the client; sanitize so a stray quote/backslash can never break
// the hand-built payload JSON (which then needs no escaping anywhere).
static void usageMakeKey(char *out, size_t outSize, char prefix, const char *name) {
  size_t o = 0;
  if (outSize < 4) { if (outSize) out[0] = '\0'; return; }
  out[o++] = prefix;
  out[o++] = ':';
  for (const char *c = name; *c && o < outSize - 1; c++) {
    char ch = *c;
    bool ok = (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')
              || ch == '_' || ch == ':' || ch == '.' || ch == '-';
    out[o++] = ok ? ch : '_';
  }
  out[o] = '\0';
}

// Linear scan is fine: ≤192 slots, touched only on /track posts (≈1/min/client) and the daily build.
static void usageAdd(const char *key, uint32_t n, uint32_t ms) {
  for (uint16_t i = 0; i < usageSlotCount; i++) {
    if (strcmp(usageSlots[i].key, key) == 0) {
      usageSlots[i].n += n;
      usageSlots[i].ms += ms;
      return;
    }
  }
  if (usageSlotCount < USAGE_SLOTS) {
    UsageSlot &s = usageSlots[usageSlotCount++];
    strncpy(s.key, key, sizeof(s.key) - 1);
    s.key[sizeof(s.key) - 1] = '\0';
    s.n = n;
    s.ms = ms;
  } else {
    usageOverflowN += n;
  }
}

// Merge one /track delta POST: {"o":1,"a":1,"p":{"<page>":{"n":1,"ms":1234}},"b":{"<btn>":2}}
// Runs on the async_tcp task — pure counter increments under the mutex, no I/O.
bool usageMergeDelta(const char *body) {
  if (!usageSlots || !usageMutex) return false;
  DynamicJsonDocument doc(6144);
  if (deserializeJson(doc, body)) return false;
  if (xSemaphoreTake(usageMutex, pdMS_TO_TICKS(200)) != pdTRUE) return false;
  uint32_t o = doc["o"] | 0U;
  bool isApp = (doc["a"] | 0) != 0;
  if (doc.containsKey("z")) {
    int32_t z = doc["z"] | 0;
    if (z >= -50400 && z <= 50400) usageTzOffsetS = z;  // ±14 h — the real-world UTC-offset range
  }
  usageOpens += o;
  UsageOpens_AllTime += o;
  if (isApp) {
    usageOpensApp += o;
    usageAppSeen = true;
  }
  JsonObject p = doc["p"];
  for (JsonPair kv : p) {
    uint32_t n = kv.value()["n"] | 0U;
    uint32_t ms = kv.value()["ms"] | 0U;
    if (ms > 14400000UL) ms = 14400000UL;  // one delta can't claim >4 h of dwell (client-bug guard)
    char key[32];
    usageMakeKey(key, sizeof(key), 'p', kv.key().c_str());
    usageAdd(key, n, ms);
    UsageOpenTime_AllTime += ms / 1000.0;
  }
  JsonObject b = doc["b"];
  for (JsonPair kv : b) {
    uint32_t n = kv.value() | 0U;
    char key[32];
    usageMakeKey(key, sizeof(key), 'b', kv.key().c_str());
    usageAdd(key, n, 0);
  }
  if (usagePeriodStartEpoch == 0) {
    time_t nowEp = time(NULL);
    if (nowEp > 1700000000LL) usagePeriodStartEpoch = nowEp;  // period opens at first data with a valid clock
  }
  usageDirty = true;
  xSemaphoreGive(usageMutex);
  return true;
}

void resetUsageAccum(time_t newStart) {
  if (!usageMutex || xSemaphoreTake(usageMutex, pdMS_TO_TICKS(200)) != pdTRUE) return;
  usageSlotCount = 0;
  usageOverflowN = 0;
  usageOpens = 0;
  usageOpensApp = 0;
  usageAppSeen = false;
  usagePeriodStartEpoch = newStart;
  usageDirty = true;
  xSemaphoreGive(usageMutex);
}

// Daily payload for track-behavior. Keys are pre-sanitized so no JSON escaping is needed.
bool buildUsagePayload(char *buf, uint32_t cap) {
  if (!usageSlots || !usageMutex) return false;
  if (xSemaphoreTake(usageMutex, pdMS_TO_TICKS(200)) != pdTRUE) return false;
  uint64_t totalMs = 0;
  for (uint16_t i = 0; i < usageSlotCount; i++)
    if (usageSlots[i].key[0] == 'p') totalMs += usageSlots[i].ms;
  int off = snprintf(buf, cap,
                     "{\"device_uid\":\"%s\",\"token\":\"%s\",\"payload_v\":1,"
                     "\"fw_version_int\":%d,\"web_version\":\"%s\",\"app_seen\":%d,"
                     "\"period_start\":%lld,\"period_end\":%lld,\"tz\":%ld,"
                     "\"opens\":%u,\"opens_app\":%u,\"total_ms\":%llu,\"overflow\":%u,\"pages\":{",
                     device_id_hex, authToken.c_str(), firmwareVersionInt, FIRMWARE_VERSION,
                     usageAppSeen ? 1 : 0, (long long)usagePeriodStartEpoch, (long long)time(NULL),
                     (long)usageTzOffsetS, usageOpens, usageOpensApp, (unsigned long long)totalMs, usageOverflowN);
  bool first = true;
  for (uint16_t i = 0; i < usageSlotCount && off > 0 && off < (int)cap; i++) {
    if (strncmp(usageSlots[i].key, "p:", 2) != 0) continue;
    off += snprintf(buf + off, cap - off, "%s\"%s\":{\"n\":%u,\"ms\":%u}",
                    first ? "" : ",", usageSlots[i].key + 2, usageSlots[i].n, usageSlots[i].ms);
    first = false;
  }
  if (off > 0 && off < (int)cap) off += snprintf(buf + off, cap - off, "},\"btns\":{");
  first = true;
  for (uint16_t i = 0; i < usageSlotCount && off > 0 && off < (int)cap; i++) {
    if (strncmp(usageSlots[i].key, "b:", 2) != 0) continue;
    off += snprintf(buf + off, cap - off, "%s\"%s\":%u",
                    first ? "" : ",", usageSlots[i].key + 2, usageSlots[i].n);
    first = false;
  }
  if (off > 0 && off < (int)cap) off += snprintf(buf + off, cap - off, "}}");
  xSemaphoreGive(usageMutex);
  return (off > 0 && off < (int)cap - 1);
}

bool executeUploadUsage(const char *payload) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (WiFi.RSSI() < -80) return false;
  if (!isRegistered || authToken.isEmpty()) return false;

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(HANDSHAKE_TIMEOUT / 1000);  // this API takes seconds

  uint32_t start = millis();
  esp_task_wdt_reset();

  IPAddress hostIP;  // pre-resolve: keep DNS out of the connect's unfed-WDT window (see executeUploadPayload)
  if (!WiFi.hostByName(host, hostIP)) {
    Serial.println("Usage: DNS fail");
    return false;
  }
  esp_task_wdt_reset();

  if (!client.connect(host, port, CONNECT_TIMEOUT)) {
    Serial.println("Usage: Connect fail");
    client.stop();
    return false;
  }
  if (millis() - start > GLOBAL_TIMEOUT) {
    client.stop();
    return false;
  }
  esp_task_wdt_reset();

  client.printf(
    "POST /functions/v1/track-behavior HTTP/1.1\r\n"
    "Host: %s\r\n"
    "Content-Type: application/json\r\n"
    "Authorization: Bearer %s\r\n"
    "Connection: close\r\n"
    "Content-Length: %u\r\n\r\n",
    host, SUPABASE_ANON_KEY, (unsigned)strlen(payload));

  size_t payloadLen = strlen(payload);
  if (client.write((const uint8_t *)payload, payloadLen) != payloadLen) {
    Serial.println("Usage: Payload send fail");
    client.stop();
    return false;
  }
  esp_task_wdt_reset();

  // Response body is ignored — only the status code matters.
  int httpCode = 0;
  uint32_t readStart = millis();
  char statusBuf[64];
  size_t statusLen = 0;
  while (client.connected() && (millis() - readStart < READ_TIMEOUT)) {
    esp_task_wdt_reset();
    while (client.available()) {
      char c = (char)client.read();
      readStart = millis();
      if (statusLen < sizeof(statusBuf) - 1) statusBuf[statusLen++] = c;
      if (c == '\n') {
        statusBuf[statusLen] = '\0';
        if (strncmp(statusBuf, "HTTP/", 5) == 0) {
          const char *sp = strchr(statusBuf, ' ');
          if (sp) httpCode = atoi(sp + 1);
        }
        goto done_status_us;
      }
    }
    if (millis() - start > GLOBAL_TIMEOUT) break;
    delay(1);
  }
done_status_us:
  client.stop();
  esp_task_wdt_reset();

  bool success = (httpCode == 200);
  if (success) {
    UsageDays_AllTime++;  // counts DELIVERED days — queue admission alone must not count
    queueConsoleMessage("App-usage analytics uploaded");
  } else if (httpCode > 0) {
    snprintf(messageBuffer, MESSAGE_BUFFER_SIZE, "Usage upload failed HTTP %d", httpCode);
    queueConsoleMessage(messageBuffer);
  }
  return success;
}

// /trackstats JSON for the Live Data → ESP32 "App Usage" card: period scalars,
// top-5 pages by dwell, top-5 buttons by count, lifetime totals.
String usageStatsJson() {
  String out;
  out.reserve(1400);  // headroom for the 40-char key cap
  if (!usageSlots || !usageMutex || xSemaphoreTake(usageMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
    return String("{\"err\":1}");
  }
  uint64_t totalMs = 0;
  for (uint16_t i = 0; i < usageSlotCount; i++)
    if (usageSlots[i].key[0] == 'p') totalMs += usageSlots[i].ms;
  out += "{\"today\":{\"opens\":";
  out += usageOpens;
  out += ",\"ms\":";
  out += (unsigned long)totalMs;
  out += ",\"pages\":[";
  bool used[USAGE_SLOTS] = {};
  for (int rank = 0; rank < 5; rank++) {  // selection by dwell — N≤192, once per tab open
    int best = -1;
    for (uint16_t i = 0; i < usageSlotCount; i++) {
      if (used[i] || strncmp(usageSlots[i].key, "p:", 2) != 0) continue;
      if (best < 0 || usageSlots[i].ms > usageSlots[best].ms) best = i;
    }
    if (best < 0 || usageSlots[best].ms == 0) break;
    used[best] = true;
    if (rank) out += ",";
    out += "[\"";
    out += (usageSlots[best].key + 2);
    out += "\",";
    out += usageSlots[best].n;
    out += ",";
    out += usageSlots[best].ms;
    out += "]";
  }
  out += "],\"btns\":[";
  memset(used, 0, sizeof(used));
  for (int rank = 0; rank < 5; rank++) {
    int best = -1;
    for (uint16_t i = 0; i < usageSlotCount; i++) {
      if (used[i] || strncmp(usageSlots[i].key, "b:", 2) != 0) continue;
      if (best < 0 || usageSlots[i].n > usageSlots[best].n) best = i;
    }
    if (best < 0 || usageSlots[best].n == 0) break;
    used[best] = true;
    if (rank) out += ",";
    out += "[\"";
    out += (usageSlots[best].key + 2);
    out += "\",";
    out += usageSlots[best].n;
    out += "]";
  }
  out += "]},\"life\":{\"opens\":";
  out += UsageOpens_AllTime;
  out += ",\"s\":";
  out += (unsigned long)UsageOpenTime_AllTime;
  out += ",\"days\":";
  out += UsageDays_AllTime;
  out += "}}";
  xSemaphoreGive(usageMutex);
  return out;
}

// Period persistence across the maintenance restart. Layout change auto-invalidates
// via the sizeof-XORed magic (same trick as the reset black box). Lifetime totals are
// NOT in this file — they ride saveNVSDataFull like every other *_AllTime.
struct UsageFileHdr {
  uint32_t magic;
  uint32_t opens, opensApp, overflowN;
  int64_t periodStart;
  int32_t tzOffsetS;
  uint8_t appSeen;
  uint8_t _pad[3];
  uint16_t slotCount;
  uint16_t _pad2;
};
#define USAGE_FILE_MAGIC (0x55534741UL ^ (uint32_t)sizeof(UsageSlot) ^ ((uint32_t)sizeof(UsageFileHdr) << 8))
#define USAGE_FILE_PATH "/usage.bin"

void dumpUsageAccum() {
  if (!usageSlots || !usageDirty) return;
  if (!usageMutex || xSemaphoreTake(usageMutex, pdMS_TO_TICKS(200)) != pdTRUE) return;
  fsTakeLock();
  File f = LittleFS.open(USAGE_FILE_PATH, "w");
  if (f) {
    fsFreeDirty = true;
    UsageFileHdr h = {};
    h.magic = USAGE_FILE_MAGIC;
    h.opens = usageOpens;
    h.opensApp = usageOpensApp;
    h.overflowN = usageOverflowN;
    h.periodStart = usagePeriodStartEpoch;
    h.tzOffsetS = usageTzOffsetS;
    h.appSeen = usageAppSeen ? 1 : 0;
    h.slotCount = usageSlotCount;
    bool ok = f.write((const uint8_t *)&h, sizeof(h)) == sizeof(h);
    if (ok && usageSlotCount > 0) {
      size_t bytes = (size_t)usageSlotCount * sizeof(UsageSlot);
      ok = f.write((const uint8_t *)usageSlots, bytes) == bytes;
    }
    f.close();
    if (ok) usageDirty = false;
    else LittleFS.remove(USAGE_FILE_PATH);  // never leave a torn file for boot to trust
  }
  fsReleaseLock();
  xSemaphoreGive(usageMutex);
}

// Boot restore (setup(), before the web server exists — no mutex contention possible).
void loadUsageAccum() {
  if (!usageSlots) return;
  fsTakeLock();
  File f = LittleFS.open(USAGE_FILE_PATH, "r");
  if (f) {
    UsageFileHdr h = {};
    bool ok = f.read((uint8_t *)&h, sizeof(h)) == sizeof(h)
              && h.magic == USAGE_FILE_MAGIC && h.slotCount <= USAGE_SLOTS;
    if (ok && h.slotCount > 0) {
      size_t bytes = (size_t)h.slotCount * sizeof(UsageSlot);
      ok = f.read((uint8_t *)usageSlots, bytes) == bytes;
    }
    if (ok) {
      usageOpens = h.opens;
      usageOpensApp = h.opensApp;
      usageOverflowN = h.overflowN;
      usagePeriodStartEpoch = h.periodStart;
      usageTzOffsetS = h.tzOffsetS;
      usageAppSeen = h.appSeen != 0;
      usageSlotCount = h.slotCount;
      for (uint16_t i = 0; i < usageSlotCount; i++)
        usageSlots[i].key[sizeof(usageSlots[i].key) - 1] = '\0';  // a corrupt file must not yield an unterminated key
      usageDirty = false;
      Serial.printf("Usage accumulator restored: %u keys, opens=%u\n", (unsigned)usageSlotCount, (unsigned)usageOpens);
    }
    f.close();
  }
  fsReleaseLock();
}

void syncTimeFromNTP() {
  if (otaInProgress) {
    return;  // Skip during OTA — don't arm fast retry; OTA path is brief.
  }
  // Manual mode gate: only AUTO and NTP-forced ever fall through to NTP.
  if (timeSourceMode == TSRC_NMEA || timeSourceMode == TSRC_PHONE) return;
  if (currentMode != MODE_CLIENT || WiFi.status() != WL_CONNECTED) {
    // WiFi not ready yet (typical on cold boot before association). Arm a
    // fast retry so the next checkTimeSync() tick fires ~60s from now
    // instead of waiting the full 12h. Underflow on early-boot millis is
    // intentional and handled by unsigned wraparound in the gate.
    lastTimeSyncAttempt = millis() - (TIME_SYNC_INTERVAL - 60000UL);
    return;
  }

  Serial.println("Starting NTP sync...");
  // Hold core0Busy so RunAlternator will not enable the field while we block
  // on the SNTP wait. Cleared on all exit paths below.
  core0Busy = true;
  esp_task_wdt_reset();

  // Sync status is reset BEFORE configTime() on purpose: sntp_init() fires a request immediately,
  // and resetting afterwards could wipe a COMPLETED that had already landed.
  //
  // Why not getLocalTime(): it only tests tm_year > 2016, so any plausible-looking libc clock
  // satisfies it instantly. restoreSoftClock() puts exactly such a clock up on a cold boot — an
  // NVS epoch as old as the last field-off — so the old code returned "synced" before a single
  // packet arrived, adopted the stale epoch as timeBase, stamped it TIME_NTP, and took the full
  // 12h throttle. Two harms: uploads carried days-old timestamps, and the source left
  // TIME_ESTIMATED, which is the one value syncTimeFromPhone() lets bypass its 24h skew guard —
  // so the phone could no longer correct the very clock that was wrong.
  sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  // Single-shot — no retry loop, no delay(500). checkTimeSync() retries on its
  // normal TIME_SYNC_INTERVAL cadence. A retry loop with delay(500) would
  // block Core 1 for up to 10.5s (3×3000ms timeout + 3×500ms) if NTP is down.
  // Same 3s Core-1 budget the getLocalTime(&timeinfo, 3000) call used to have.
  // sntp_get_sync_status() self-clears on the read that returns COMPLETED, so read it once.
  bool sntpAnswered = false;
  uint32_t sntpWaitStart = millis();
  while (millis() - sntpWaitStart < 3000) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) { sntpAnswered = true; break; }
    delay(10);
  }
  esp_task_wdt_reset();

  if (sntpAnswered) {
    timeBase = time(nullptr);
    timeBaseMillis = millis();
    timeIsSynced = true;
    currentTimeSource = TIME_NTP;
    lastTimeSyncAttempt = millis();

    queueConsoleMessage("Time synced from NTP");
    Serial.printf("NTP synced: epoch=%ld\n", timeBase);
    core0Busy = false;
    return;
  }

  Serial.println("NTP sync attempt failed");
  // Arm fast retry — only successful syncs get the full 12h throttle.
  lastTimeSyncAttempt = millis() - (TIME_SYNC_INTERVAL - 60000UL);
  core0Busy = false;
}

bool canUploadNow() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (WiFi.RSSI() < -80) return false;
  return true;
}

// FAST ALTERNATOR-CURRENT CHANNEL — GPIO3 / ADC1_CH2
// Hardware: CH1 buffer output (U9 pins 6+7) jumpered to J3 pin 11 (net EXTRA6, C66
// removed). 3.33 mV/A at the node for the 300 A sensor; zero amps = 1.25 V. Spec:
// Working Markdown Docs/CV_Loop_Dev_Summary.md, "Fast alternator-current ADC channel".
// This channel is an INSTRUMENT — never a control input; nothing here may couple
// into control or protections. Sampling is hardware-timed continuous DMA (crystal-
// derived, ~ppm); CPU stalls can only delay the drain, never bend sample spacing.
// Consumers: disturbance matrix, failure detector (plug-in stub), live scope,
// reference flipbook, fleet scalars.
// A board with a broken/missing jumper reads pegged full-scale through the R110
// pullup (fail-obvious by design) — the subsystem detects that and goes dormant.

#define FA_ADC_CHANNEL ADC_CHANNEL_2  // GPIO3
#define FA_SAMPLE_RATE_HZ 20000
#define FA_RAW_RING_N 10000  // 500 ms @ 20 kSPS — live scope depth (≥3 engine revs above ~360 RPM, so a slow 400 RPM idle still shows a few). uint16 head/count cap is 65535, so this has headroom.
#define FA_FRAME_SAMPLES 256
#define FA_FRAME_BYTES (FA_FRAME_SAMPLES * SOC_ADC_DIGI_RESULT_BYTES)
#define FA_POOL_BYTES (FA_FRAME_BYTES * 12)  // ~150 ms of driver-pool slack (internal RAM — the DMA pool must be DRAM)
#define FA_DRAIN_BUDGET_US 1000              // hard cap per loop() pass (the loop-budget contract)
#define FA_ZERO_MV 1250.0f                   // hall sensor 2.5 V @ 0 A → ÷2 divider
#define FA_RAILED_RAW 4090                   // 12-bit code treated as railed full-scale
#define FA_DECIM 16                          // boxcar-16 → 1.25 kSPS effective (nulls at 1.25 kHz)
#define FA_WIN_DECIM_N 625                   // 0.5 s tone/gate/matrix window, decimated samples (crystal-timed).
                                             // Kept short so the per-window flat-top FFT tracks RPM with less
                                             // drift smear. The DETECTOR window is decoupled (FA_DET_WIN_N,
                                             // fixed 2 s) so its analysis length is independent.
// 6 dB ceiling ≈ +150..+210 A above zero (S3 sources spec full-scale anywhere 1.75–1.95 V).
// BENCH-VERIFY the real rail point, then set switch-up ~30 A below the measured ceiling,
// switch-down ~30 A below that (hysteresis). Defaults assume the conservative 1.75 V end.
#define FA_ATTEN_UP_AMPS 120.0f   // mean amps (ADS path) above this → switch to 12 dB
#define FA_ATTEN_DOWN_AMPS 90.0f  // mean amps below this → back to 6 dB
#define FA_ATTEN_DWELL_MS 5000UL  // min spacing between attenuation switches
#define FA_ABSENT_RAILED_SEC 10   // consistently railed this long (at low ADS amps) → channel absent
#define FA_REPROBE_MS 300000UL    // dormant channel re-probe interval

// Channel state: 0 = off (init failed / no driver), 1 = present (sampling), 2 = absent
// (railed full-scale → jumper open/missing; driver stopped, periodic re-probe).
uint8_t faChanState = 0;
uint8_t faAttenIs12 = 0;          // 0 = 6 dB (default floor), 1 = 12 dB (high-current range)
uint32_t faTotalSamples = 0;      // lifetime drained samples (diagnostics)
uint32_t faWindowsAccepted = 0;   // windows that survived railed/switch screening (steady-state gate applies on top)
uint32_t faWindowsDiscarded = 0;  // windows thrown out: railed codes, atten switch

static adc_continuous_handle_t faAdcHandle = NULL;
static portMUX_TYPE faRingMux = portMUX_INITIALIZER_UNLOCKED;
static int16_t *faRawRing = NULL;   // PSRAM — calibrated mV, int16
static uint16_t faRingHead = 0;     // next write index
static uint16_t faDecimWinN = 0;  // decimated samples this window — drives window length + winMeanAmps
// (The decimated stream itself is consumed live in faProcessDecimated — Goertzel bank, EMAs and
// pk-pk min/max — so it is never stored. The detector reads the raw stream, faDetWin.)
static int32_t faBoxAcc = 0;  // boxcar-16 accumulator
static uint8_t faBoxN = 0;
static bool faWinRailed = false;         // any railed code this window → discard
static bool faWinAttenSwitched = false;  // window started on a fresh range → discard
static uint8_t faAttenPending = 0xFF;    // requested range (0/1); 0xFF = none. Applied ONLY between windows.
static unsigned long faAttenLastSwitchMs = 0;
static uint32_t faRailedStreak = 0;  // consecutive railed samples (presence detection)
static bool faAbsentWarned = false;
static unsigned long faLastProbeMs = 0;
// eFuse-corrected linear scaling raw→mV, precomputed per attenuation at init (the factory
// curve is near-linear; a 2-point fit keeps the correction at zero per-sample cost).
static float faMvSlope = 1750.0f / 4095.0f, faMvOff = 0.0f;
static float faMvSlope6 = 1750.0f / 4095.0f, faMvOff6 = 0.0f;
static float faMvSlope12 = 3100.0f / 4095.0f, faMvOff12 = 0.0f;

// ── Disturbance matrix (consumer 1) ──
// 80 RPM bins (50 RPM, 0–4000) × 15 amps bins (20 A starting at 5 A) = 1200 cells in
// PSRAM. Each cell holds the top-6 (frequency, amplitude) peaks — matched across windows
// by frequency ±5% and RECENT-AVERAGED (a running 1/N mean, N = FA_CELL_AVG_N windows:
// cumulative while warming up, then a fixed 1/N weight so the cell tracks the machine
// lately and never freezes — smooths single-window noise without the outlier-blindness of
// a max-envelope) — plus ONE broadband pk-pk (the composite envelope an iExcess threshold
// must actually clear; if pk-pk ≫ sum of stored bands, energy exists outside the surveyed
// 10–400 Hz range) and a saturating window count (trustworthiness, independent of N).
#define FA_RPM_BIN_W 50
#define FA_RPM_BINS 80
#define FA_AMP_BIN_W 20
#define FA_AMP_BIN_LO 5
#define FA_AMP_BINS 15
#define FA_CELL_PEAKS 6
#define FA_PEAK_MIN_A 0.5f  // peaks below this never enter a cell
#define FA_CELL_AVG_N 4     // recent-average depth: cell value = running 1/min(n,N) mean (never freezes)
#define FA_MATRIX_PATH "/famatrix.bin"
#define FA_MATRIX_MAGIC 0x46414D58u  // 'FAMX'
#define FA_MATRIX_VER 5              // bump on any FaCell layout change — older blobs rejected (map re-learns)
#define FA_MATRIX_FLUSH_MS 900000UL  // 15-min field-off cadence, like the long-term ring

struct FaPeak {
  uint16_t freqHzX10;  // band center, Hz ×10 (parabolic-refined)
  uint16_t ampAX100;   // mean amplitude, A ×100
  uint16_t nAcc;       // windows folded into this peak's mean (0 = slot free)
};
struct FaCell {
  FaPeak pk[FA_CELL_PEAKS];
  uint16_t pkpkAX100;   // mean broadband pk-pk of the decimated stream, A ×100
  uint16_t windows;     // qualified windows merged (saturating)
};
// FaCell carries no filtered-ripple fields (RPM_RIPPLE_TABLE_SPEC): the raw matrix is always-on and
// welcomes every operating point; the filtered pk-pk lives in the game-scoped 1-D ripTab below,
// rated at ONE fixed commanded current.
// NOTE (§12, built then REVERTED): a "detector's-eye excursion peak" per cell
// (max mExcessEma while armed+unclamped) was added here and fed to a threshold-recommendation
// verdict. Timeline analysis of the vicious-cycle log killed it: the recorded excursions were one
// commanded throttle blip + five post-trip RECOVERY OVERSHOOTS (duty ramping 7 pts past its steady
// value) — the protection's own dynamics. Recommending floors from them deafens the protection to
// cover a control defect. Do not re-add without solving that self-reference (e.g., exclude a window
// after any trip release).
static FaCell *faMatrix = NULL;  // PSRAM — FA_RPM_BINS × FA_AMP_BINS cells
uint16_t faCellsUsed = 0;        // cells with ≥1 qualified window (diagnostics)
// Highest Tone in Map (dashboard headline) — strongest single tone across the whole learned map,
// rescanned whenever the map changes. Read by the CSV2 builder in 3_functions.ino.
uint16_t faDomFreqHzX10 = 0;     // frequency, Hz ×10
uint16_t faDomAmpAX100 = 0;      // pk-pk amplitude (2 × sine amplitude), A ×100
uint16_t faDomRpm = 0;           // RPM (bin center) where it occurs
static uint32_t faMatrixDirtyWindows = 0;
static volatile bool faPendingMatrixClear = false;  // set by /get handler (Core 0), executed on Core 1

// ── Measured filtered ripple capture (RIPPLE_DETECTION_REARCH_SPEC §3.1) ──
// For BOTH detectors, run an always-on IExcessTau LOW-PASS EMA directly on the sensor current — INA228
// Bcur for the battery (G4) detector, ADS1115 MeasuredAmps for the alternator (G3) detector — and take
// the pk-pk (max−min) of that low-passed signal over each 0.5 s window. Because the (slow) setpoint
// cancels in a pk-pk, this EQUALS the detector's own mExcessEma pk-pk — the real number it trips on —
// WITHOUT needing the loop bound, so the battery figure is measurable even in bulk. This is NOT the old
// drift-removed AC pk-pk: that high-pass kept the fast belt content the IExcessTau low-pass is meant to
// attenuate, so it read far too large. Qualifying windows fold into the game-scoped ripTab below
// (RPM_RIPPLE_TABLE_SPEC) — used only to PICK the current-check RPM and to draw the Protections
// reference plot. It NEVER moves the over-current floor. Called every INA fast-read (~4.3 ms,
// field-active) from 5_functions.ino.
// ADMISSION (spec §10): a window folds only if it was genuinely steady — the IExcessTau low-pass passes
// slow current ramps straight through, so a ramping window would record the RAMP size (10–20 A) as
// "ripple" and peak-hold it forever. Gates: min sample count, no protection clamp, command travel
// within limit, per-detector 300 ms-EMA drift within limit, same-RPM-bin (table fold only). NO wizard
// relaxation — a steady-state quantity cannot be measured while ramping; the wizard's instructed
// pauses are the admission windows. Live readouts: ROLL_RIPCMDEXC/RIPALTEXC/RIPBATTEXC on the Diag page.
#define FILT_RIPPLE_ADM_TC 0.300f      // admission-EMA time constant (s) — heavy enough that belt ripple averages out of it, so its window spread measures operating-point drift, not ripple. Matches Path A's 300 ms precedent. NOT the measurement filter.
// Capture-gate knobs (Pattern B, Diag ▸ Measured-Ripple Capture Gating card; each has a live
// pass/fail readout there). Own settings, NOT the fa* anomaly-detector gates.
float ripWinMs = 2000.0f;       // ms — pk-pk measurement window. Must hold ≥2 periods of the slowest disturbance to characterize (idle hunt ~1 s → 2 s default; the old 500 ms saw hunt as drift and rejected it). CAVEAT: the window defines the measured quantity — map/fit values captured under a different length are not comparable (clear map + re-run current check after changing). NVS NOTE: devices flashed before 2026-07-01 hold 500 in NVS — set 2000 on the Diag card after flashing.
float ripDriftFloorA = 2.0f;    // A — floor shared by the command-travel gate AND the stationarity gate (mean-shift tolerance below which a window always passes)
float ripDriftPct = 5.0f;       // % of window-mean alt current — command-travel gate ONLY (commands are ripple-free, so amplitude gating stays correct for them; the sensor gates are stationarity-based below)
// Stationarity gate (spec §11 — replaces the §10.8 amplitude drift gate): a window is "steady" if its
// two HALVES have (nearly) the same mean — a transient/ramp walks the mean monotonically (half-means
// differ by ~half the excursion), while hunt/ripple recurs (half-means cancel). Self-scaling: tolerance
// grows with the window's own measured pk-pk, so a dirty-but-stationary idle admits while a same-size
// ramp rejects. Design-time constants, not knobs — they set the SHAPE of the test, not an operating point.
#define RIP_STAT_K 0.25f           // mean-shift tolerance as a fraction of that sensor's full-window filtered pk-pk. A linear ramp shifts the half-means by 0.50×pk-pk → rejected with 2× margin; a ≥2-cycle hunt shifts them <0.1×pk-pk → admitted with 2.5× margin.
#define RIP_RPM_STAT_FLOOR 10.0f   // RPM — mean-shift floor for the RPM stationarity test (tach jitter tolerance)
#define RIP_CROSS_MIN 4            // table fold only — min crossings of the measurement EMA about its 300 ms baseline per window. A one-shot transient crosses ~2×; real ripple/hunt crosses constantly. Ring exempt (median-of-8 already robust; must not starve the wizard hold).
uint32_t g_ripAltAdmitCount = 0;   // windows that passed ALL alt-fold gates (throughput readout, CSV2; not persisted)
uint32_t g_ripBattAdmitCount = 0;  // same for the battery detector

// ── RPM ripple table (RPM_RIPPLE_TABLE_SPEC) ──
// 1-D per-50-RPM-bin filtered pk-pk (both detectors), filled ONLY while the RPM Invaders game holds
// a fixed commanded current through resTest. Replaces the FaCell filtered layer, which had no
// reproducibility test, smeared the amp axis with a max, and captured at whatever current AUTO
// produced (the 6.27 A @ 1125 phantom). Agree-twice commit: pending → second qualifying window within
// tolerance commits the AVERAGE of the pair; disagree replaces the pending. Cells are immutable once
// committed; the whole table is wiped at game start and persisted (frozen) on every teardown path,
// including Abort — refreshing the data = re-run the sweep. No expiry timer: a game-scoped table
// can't cross-session pair. §11 gates, live readouts, and admit counters stay always-on upstream;
// ONLY this fold is game-gated.
#define RIPTAB_BINS 40               // [0, 2000) RPM in 50-RPM bins
#define RIPTAB_PATH "/riptab.bin"
#define RIPTAB_MAGIC 0x52495054u     // 'RIPT'
#define RIPTAB_VER 1
#define RIPTAB_AGREE_FRAC 0.33f      // agree when |new − pend| ≤ max(floor, frac × max(new, pend))
#define RIPTAB_AGREE_FLOOR_A 0.5f    // lets near-zero battery ripple pair trivially
#define RIPTAB_ALT_PEND  0x01
#define RIPTAB_ALT_DONE  0x02
#define RIPTAB_BATT_PEND 0x04
#define RIPTAB_BATT_DONE 0x08
struct RipTabCell {
  uint16_t altPkX100, battPkX100;      // committed values (0 = none)
  uint16_t altPendX100, battPendX100;  // agree-twice pending candidates
  uint8_t  state;                      // RIPTAB_* bits
};
struct RipTabSession {   // stamps persisted with the table — "captured at N A, 13.1–13.6 V"
  float levelA;          // fixed commanded current (resTestTargetA at game start)
  float ibvMinV, ibvMaxV;  // IBV span over the windows that actually folded
  uint16_t idleRpm;      // browser-detected idle at game start (coverage-band floor source)
  uint32_t epoch;        // game-start wall clock (0 = clock not synced)
};
struct RipTab { RipTabSession sess; RipTabCell cell[RIPTAB_BINS]; };
static RipTab ripTab;                       // ~410 B — static, not worth a PSRAM alloc
volatile bool ripGameFill = false;          // fold window is OPEN (game running); set on Core 1 at wipe
static volatile bool ripTabPendingWipe = false;  // /get arm → wipe+stamp runs on Core 1 (faMatrixMaybeFlush) so it can't race the folds
static volatile bool ripTabPendingSave = false;  // /get disarm (or deadman) → persist runs on Core 1
static volatile uint16_t ripIdleRpmStage = 0;    // browser sends idle BEFORE arming; stamped into the session at wipe
static float altFiltEma = 0.0f, battFiltEma = 0.0f;             // IExcessTau low-pass of each sensor current (A) — the MEASUREMENT
static float altFiltWinMin = 1e9f, altFiltWinMax = -1e9f;       // FULL-window extremes of the low-passed alt signal — stationarity self-scale + forensics
static float battFiltWinMin = 1e9f, battFiltWinMax = -1e9f;     // same, batt
// Per-HALF accumulators (spec §11): the committed pk-pk is the LESSER of the two half-window pk-pks —
// a one-shot event inflates only one half so the min stays honest, while real ripple/hunt (period ≤
// half the window) inflates both. The half sums/counts feed the mean-shift stationarity test.
static float altFiltH1Min = 1e9f, altFiltH1Max = -1e9f, altFiltH2Min = 1e9f, altFiltH2Max = -1e9f;
static float battFiltH1Min = 1e9f, battFiltH1Max = -1e9f, battFiltH2Min = 1e9f, battFiltH2Max = -1e9f;
static float altFiltH1Sum = 0.0f, altFiltH2Sum = 0.0f, battFiltH1Sum = 0.0f, battFiltH2Sum = 0.0f;
static float rpmH1Sum = 0.0f, rpmH2Sum = 0.0f;                  // RPM half-means → RPM stationarity + mean-RPM binning
static uint32_t filtRippleH1N = 0, filtRippleH2N = 0;
// Admission gates (steady-state qualification — spec §11). Command travel (setpointLimited max−min,
// ripple-free by construction) exactly rejects every commanded ramp: test level steps, warmup,
// big-step gentling, Hi→Lo glide. Protection latch rejects any window a clamp touched (G1/G2, iExcess,
// load dump). Sensor steadiness is the half-mean stationarity test (per-detector — a stationary-alt /
// ramping-batt window folds alt only). The 300 ms EMAs remain ONLY as the crossings baseline.
static float altAdmEma = 0.0f, battAdmEma = 0.0f;               // 300 ms baselines for the crossings tally (NOT the measurement, NOT a drift gate)
static float cmdWinMin = 1e9f, cmdWinMax = -1e9f;               // setpointLimited travel over the window
static float filtRippleWinRpmMin = 1e9f, filtRippleWinRpmMax = -1e9f;  // window RPM span → RPM stationarity self-scale
static bool filtRippleWinProt = false;                          // any protection clamp during the window
static uint32_t filtRippleWinN = 0;
static uint32_t filtRippleLastMs = 0, filtRippleWinStartMs = 0;
static bool filtRippleArmed = false;   // false until the first sample seeds the EMAs (seed, don't measure)
// Crossings tally — GATE for the table fold (≥ RIP_CROSS_MIN) and recorded into the commit forensics ring:
// how many times the fast measurement EMA crossed its slow (300 ms) baseline within the window. A one-shot
// transient crosses ~twice; sustained oscillation many times.
static uint16_t altFiltCross = 0, battFiltCross = 0;
static bool altAboveAdm = false, battAboveAdm = false;   // sign of (measurement − baseline) last sample

// CV D-term deadband capture (CV_Dterm_Deadband_Commissioning_Spec): worst positive slope of the D
// term's own filtered voltage per §11 window, for the bcurRtest ring. g_cvKdFiltV is refreshed by the
// same INA fast-read block that calls faFiltRippleUpdate, so the exact signal the D term differentiates
// is fresh here; the slope is the control path's own sliding backward-diff (~VoltageLoopInterval span,
// 6_functions.ino kdBuf) so the measured noise floor and the runtime cvDSlope are the same quantity.
// Min-of-halves like the pk-pk estimator: a one-shot rise inflates one half only; real ripple recurs.
#define KDSLP_BUF_N 64
static uint32_t kdSlpT[KDSLP_BUF_N];
static float kdSlpV[KDSLP_BUF_N];
static uint8_t kdSlpN = 0, kdSlpHead = 0;
static float slpH1Max = 0.0f, slpH2Max = 0.0f;   // worst positive g_cvKdFiltV slope per half-window (V/s)

// Reset the window accumulators — fresh window baselined at the running EMAs (the caller reseeds the
// EMAs themselves first on arm/stall; a normal window rollover keeps them running).
static void filtRippleWinReset(float rpm, uint32_t now) {
  altFiltWinMin = altFiltWinMax = altFiltEma;
  battFiltWinMin = battFiltWinMax = battFiltEma;
  altFiltH1Min = altFiltH1Max = altFiltH2Min = altFiltH2Max = altFiltEma;
  battFiltH1Min = battFiltH1Max = battFiltH2Min = battFiltH2Max = battFiltEma;
  altFiltH1Sum = altFiltH2Sum = battFiltH1Sum = battFiltH2Sum = 0.0f;
  rpmH1Sum = rpmH2Sum = 0.0f;
  filtRippleH1N = filtRippleH2N = 0;
  cmdWinMin = cmdWinMax = setpointLimited;
  filtRippleWinN = 0;
  filtRippleWinRpmMin = filtRippleWinRpmMax = rpm;
  filtRippleWinProt = false;
  filtRippleWinStartMs = now;
  altFiltCross = battFiltCross = 0;
  altAboveAdm  = (altFiltEma  >= altAdmEma);
  battAboveAdm = (battFiltEma >= battAdmEma);
  slpH1Max = slpH2Max = 0.0f;
}

// Active 3-current resonance test (COMMISSIONING_SPEC §3.2): when armed, log each completed window's
// (rpm, operating current, pk-pk) so the browser can fit ripple = a0 + a1·I and project to max output.
// Logged for BOTH detectors in the same window: battery (iX100/pkpkX100, INA path) and alternator
// (iAltX100/altPkpkX100, ADS path) — so the browser fits a battery slope AND a symmetric bulk slope.
// pk-pk here is the IExcessTau low-passed pk-pk (same quantity as the FaCell fill), so the fit matches
// what the detector trips on. This keeps MULTIPLE samples so a slope can be fit. RAM-only (ephemeral
// test), cleared on arm. Ring appends only while bcurRtestActive; the FaCell fill runs always.
#define BCUR_RTEST_CAP 64
// slopeVpsX1000: worst positive slope of g_cvKdFiltV over the window (V/s, min-of-halves) — the CV
// D-term deadband measurement. 0 = slope ring hadn't spanned a full diff window yet (browser skips).
struct BcurRtestPt { uint16_t rpm; uint16_t iX100; uint16_t pkpkX100; uint16_t iAltX100; uint16_t altPkpkX100; uint16_t slopeVpsX1000; };
BcurRtestPt bcurRtest[BCUR_RTEST_CAP];
volatile bool bcurRtestActive = false;
volatile uint16_t bcurRtestCount = 0;  // appends stop at CAP; browser stops well before
// Wizard-commanded current levels for the resonance test (§3.2): when active, the control loop drives the
// alternator to resTestTargetA (slewed, protections live — modeled on the Battery-Health DCIR generator).
volatile bool resTestActive = false;
volatile float resTestTargetA = 0.0f;
volatile uint32_t resTestLastCmdMs = 0;       // deadman: browser refreshes this; loop auto-releases if it goes stale
// Deferred release: on resTest=0 the loop slews the field to ~0 FIRST (still current-controlled, so the
// target-relative over-voltage stays suppressed) THEN drops resTestActive — so CV re-enters from a
// low-voltage state and ramps the field back up FROM BELOW the charge target, instead of resuming at the
// test's high held current and slamming the soft OV (G2) the instant voltage control re-arms.
volatile bool resTestReleasing = false;
#define RES_TEST_DEADMAN_MS 8000UL            // > the browser's ~3 s keepalive; catches a closed/crashed wizard
// Ripple-game scoring gate: no bin may fold until the measured current has held AT resTestTargetA for
// RIP_SCORE_HOLD_MS. Without it the first invaders "die" while the field is still ramping up from the ~4%
// rest floor, locking a bogus near-zero-current ripple into the idle bin (the exact spot resonance lives).
// Set in the resTest control block (which owns setpointLimited/targetCurrent), read in faFiltRippleUpdate.
#define RIP_SCORE_HOLD_MS 2000UL
volatile bool ripScoreArmed = false;


// Fold forensics (diagnostic ring, RAM-only): snapshots the window behind every ripTab fold event so
// a phantom table value stays traceable without a live CSV log — the "why won't this bin settle"
// trace. event: 'P' pending set, 'D' disagree (pending replaced; otherX100 = the value it replaced),
// 'C' commit (otherX100 = the partner it averaged with). pkpkX100 is always the window's own value.
// Never touches control or the table. Read by /ripforensic.csv. Overwrites oldest; a torn row is a
// harmless stale read.
#define RIP_FORENSIC_CAP 32
struct RipForensicPt { uint16_t rpm, ampLo, meanX100, pkpkX100, shiftX100, cmdTravelX100, crossings, otherX100; uint8_t detector; char event; };
RipForensicPt ripForensic[RIP_FORENSIC_CAP];
volatile uint16_t ripForensicHead = 0;    // next write slot (wraps)
volatile uint16_t ripForensicCount = 0;   // total fold events recorded (saturates)
static void ripForensicPush(uint8_t det, uint16_t rpm, int ampLo, float meanA, uint16_t pkpkX100,
                            float meanShiftA, float cmdTravelA, uint16_t crossings,
                            char event, uint16_t otherX100) {
  RipForensicPt &p = ripForensic[ripForensicHead];
  p.detector = det;
  p.rpm = rpm;
  p.ampLo = (uint16_t)(ampLo < 0 ? 0 : ampLo);
  p.meanX100 = (uint16_t)fminf(fabsf(meanA) * 100.0f + 0.5f, 65535.0f);
  p.pkpkX100 = pkpkX100;
  p.shiftX100 = (uint16_t)fminf(fabsf(meanShiftA) * 100.0f + 0.5f, 65535.0f);
  p.cmdTravelX100 = (uint16_t)fminf(fabsf(cmdTravelA) * 100.0f + 0.5f, 65535.0f);
  p.crossings = crossings;
  p.event = event;
  p.otherX100 = otherX100;
  ripForensicHead = (ripForensicHead + 1) % RIP_FORENSIC_CAP;
  if (ripForensicCount < 65535) ripForensicCount++;
}

// Fold one INA battery-current sample (bcur) + the co-sampled ADS alternator current (macur) into the
// measured filtered-ripple capture. ALWAYS-ON — the map fills during normal running too (§3.1); the
// resonance-test ring only appends while bcurRtestActive. Called every INA fast-read from 5_functions.ino.
// See the block comment above: pk-pk is of the IExcessTau LOW-PASS of each sensor, == the detector's own
// mExcessEma pk-pk, so the map/ring read exactly what the trip reads. rpm bins the cell; altMean is the
// cell's amp axis; the battery figure rides in the same cell (its own current is logged only in the ring).
void faFiltRippleUpdate(float bcur, float macur, float rpm) {
  uint32_t now = millis();
  if (!filtRippleArmed) {  // seed the EMAs, start a fresh window, measure next time
    altFiltEma = macur; battFiltEma = bcur;
    altAdmEma = macur; battAdmEma = bcur;
    filtRippleLastMs = now;
    filtRippleWinReset(rpm, now);
    kdSlpN = 0; kdSlpHead = 0;   // stale voltage samples across a gap would fake a slope
    filtRippleArmed = true;
    return;
  }
  float dt = (now - filtRippleLastMs) * 0.001f;
  filtRippleLastMs = now;
  if (dt <= 0.0f || dt > 1.0f) {  // stall / field-off slow read → reseed baselines, drop this window
    altFiltEma = macur; battFiltEma = bcur;
    altAdmEma = macur; battAdmEma = bcur;
    filtRippleWinReset(rpm, now);
    kdSlpN = 0; kdSlpHead = 0;
    return;
  }
  float tauSec = IExcessTau * 0.001f;
  float alpha  = dt / (tauSec + dt);   // exact IExcessTau low-pass the detector uses — NOT a drift high-pass
  altFiltEma  += alpha * (macur - altFiltEma);
  battFiltEma += alpha * (bcur  - battFiltEma);
  float admAlpha = dt / (FILT_RIPPLE_ADM_TC + dt);   // 300 ms baseline EMA — crossings-tally reference only
  altAdmEma  += admAlpha * (macur - altAdmEma);
  battAdmEma += admAlpha * (bcur  - battAdmEma);
  { bool a = (altFiltEma  >= altAdmEma);  if (a != altAboveAdm)  { altFiltCross++;  altAboveAdm  = a; }   // crossings tally — fold gate + forensics (see decl)
    bool b = (battFiltEma >= battAdmEma); if (b != battAboveAdm) { battFiltCross++; battAboveAdm = b; } }
  // D-term deadband capture: the control path's sliding backward-diff of g_cvKdFiltV (same window
  // clamp, same full-ring fallback as the kdBuf block in 6_functions.ino), folded as a per-half
  // worst-positive below. No slope until the ring spans one window — mirrors "no cvDSlope until
  // one interval" at CV entry.
  float kdSlpNow = 0.0f; bool kdSlpOk = false;
  kdSlpT[kdSlpHead] = now;
  kdSlpV[kdSlpHead] = g_cvKdFiltV;
  kdSlpHead = (uint8_t)((kdSlpHead + 1) % KDSLP_BUF_N);
  if (kdSlpN < KDSLP_BUF_N) kdSlpN++;
  {
    uint32_t effWindowMs = (uint32_t)constrain((int)VoltageLoopInterval, 20, 200);
    float vOld = 0.0f; uint32_t oldAge = 0; bool spanned = false;
    float vOldest = 0.0f; uint32_t oldestAge = 0;
    for (uint8_t i = 0; i < kdSlpN; i++) {
      uint8_t idx = (uint8_t)((kdSlpHead + KDSLP_BUF_N - 1 - i) % KDSLP_BUF_N);   // newest → oldest
      uint32_t age = now - kdSlpT[idx];
      vOldest = kdSlpV[idx]; oldestAge = age;
      if (age >= effWindowMs) { vOld = kdSlpV[idx]; oldAge = age; spanned = true; break; }
    }
    if (!spanned && kdSlpN >= KDSLP_BUF_N && oldestAge > 0) { vOld = vOldest; oldAge = oldestAge; spanned = true; }
    if (spanned && oldAge > 0) {
      kdSlpNow = (g_cvKdFiltV - vOld) / ((float)oldAge / 1000.0f);
      kdSlpOk = true;
    }
  }
  if (altFiltEma  < altFiltWinMin)  altFiltWinMin  = altFiltEma;
  if (altFiltEma  > altFiltWinMax)  altFiltWinMax  = altFiltEma;
  if (battFiltEma < battFiltWinMin) battFiltWinMin = battFiltEma;
  if (battFiltEma > battFiltWinMax) battFiltWinMax = battFiltEma;
  // Half-window routing (spec §11): first vs second half by elapsed time. Half extremes feed the
  // min-of-halves estimator; half sums feed the mean-shift stationarity test and mean-RPM binning.
  if ((now - filtRippleWinStartMs) * 2 < (uint32_t)ripWinMs) {
    if (altFiltEma  < altFiltH1Min)  altFiltH1Min  = altFiltEma;
    if (altFiltEma  > altFiltH1Max)  altFiltH1Max  = altFiltEma;
    if (battFiltEma < battFiltH1Min) battFiltH1Min = battFiltEma;
    if (battFiltEma > battFiltH1Max) battFiltH1Max = battFiltEma;
    altFiltH1Sum += altFiltEma; battFiltH1Sum += battFiltEma; rpmH1Sum += rpm; filtRippleH1N++;
    if (kdSlpOk && kdSlpNow > slpH1Max) slpH1Max = kdSlpNow;
  } else {
    if (altFiltEma  < altFiltH2Min)  altFiltH2Min  = altFiltEma;
    if (altFiltEma  > altFiltH2Max)  altFiltH2Max  = altFiltEma;
    if (battFiltEma < battFiltH2Min) battFiltH2Min = battFiltEma;
    if (battFiltEma > battFiltH2Max) battFiltH2Max = battFiltEma;
    altFiltH2Sum += altFiltEma; battFiltH2Sum += battFiltEma; rpmH2Sum += rpm; filtRippleH2N++;
    if (kdSlpOk && kdSlpNow > slpH2Max) slpH2Max = kdSlpNow;
  }
  if (setpointLimited < cmdWinMin) cmdWinMin = setpointLimited;
  if (setpointLimited > cmdWinMax) cmdWinMax = setpointLimited;
  if (rpm < filtRippleWinRpmMin) filtRippleWinRpmMin = rpm;
  if (rpm > filtRippleWinRpmMax) filtRippleWinRpmMax = rpm;
  if (g_fastOvClampActive) filtRippleWinProt = true;
  filtRippleWinN++;
  if (now - filtRippleWinStartMs >= (uint32_t)ripWinMs && filtRippleWinN > 0) {
    // Committed pk-pk = the LESSER of the two half-window pk-pks (§11 min-of-halves): a one-shot event
    // inflates only one half, so the min stays honest; anything real (period ≤ half the window) recurs
    // in both. The FULL-window pk-pk is kept as the stationarity self-scale — for a ramp it equals the
    // whole excursion, which is exactly the size the mean-shift test must reject against.
    float altPkFull  = altFiltWinMax  - altFiltWinMin;
    float battPkFull = battFiltWinMax - battFiltWinMin;
    float altPk   = fminf(altFiltH1Max  - altFiltH1Min,  altFiltH2Max  - altFiltH2Min);
    float battPk  = fminf(battFiltH1Max - battFiltH1Min, battFiltH2Max - battFiltH2Min);
    float altMean  = (altFiltH1Sum  + altFiltH2Sum)  / (float)filtRippleWinN;   // operating alternator current over the window
    float battMean = (battFiltH1Sum + battFiltH2Sum) / (float)filtRippleWinN;   // operating battery current over the window
    uint16_t altV  = (uint16_t)fminf(altPk  * 100.0f + 0.5f, 65535.0f);
    uint16_t battV = (uint16_t)fminf(battPk * 100.0f + 0.5f, 65535.0f);
    // ── Steady-state admission gates (spec §11 — stationarity, not smallness) ────────────────────
    // "Steady" = the window's two halves have (nearly) the same mean. A transient/throttle ramp walks
    // the mean monotonically (half-means differ by ~half the full excursion); dirty-but-stationary
    // operation (idle hunt, load churn) swings hard but recurs, so the half-means cancel. Tolerance
    // self-scales on that sensor's own full-window pk-pk — the dirtier the signal, the more mean
    // movement is allowed. This is what lets a "dirty speed" in while still rejecting ramps: the old
    // amplitude drift gate rejected hunting idle every window, which starved the map at the exact RPM
    // that trips the bulk over-current supervisor.
    uint32_t minN = (uint32_t)(ripWinMs * 0.1f);            // ~50% of expected samples at the ~5 ms INA cadence
    bool halvesOk = (filtRippleH1N >= minN / 4) && (filtRippleH2N >= minN / 4);  // both half-means must be real averages, not a few stray samples
    float altShift = 1e9f, battShift = 1e9f, rpmShift = 1e9f, rpmMean = 0.0f;
    if (halvesOk) {
      altShift  = fabsf(altFiltH1Sum  / (float)filtRippleH1N - altFiltH2Sum  / (float)filtRippleH2N);
      battShift = fabsf(battFiltH1Sum / (float)filtRippleH1N - battFiltH2Sum / (float)filtRippleH2N);
      rpmShift  = fabsf(rpmH1Sum / (float)filtRippleH1N - rpmH2Sum / (float)filtRippleH2N);
      rpmMean   = (rpmH1Sum + rpmH2Sum) / (float)filtRippleWinN;
    }
    float limStatAlt  = fmaxf(ripDriftFloorA, RIP_STAT_K * altPkFull);
    float limStatBatt = fmaxf(ripDriftFloorA, RIP_STAT_K * battPkFull);
    float limStatRpm  = fmaxf(RIP_RPM_STAT_FLOOR, RIP_STAT_K * (filtRippleWinRpmMax - filtRippleWinRpmMin));
    // Command travel uses an amplitude limit (max(floor, pct%·mean)) — setpointLimited is
    // ripple-free by construction, so smallness IS the correct test for it.
    float limCmd = fmaxf(ripDriftFloorA, ripDriftPct * 0.01f * fabsf(altMean));
    float cmdTravel = cmdWinMax - cmdWinMin;
    // Live gate readouts — fed every window, before gating, so the dashboard shows why windows
    // are/aren't admitting. All four rows are (quantity − limit), 10 s peak, <=0 = passing.
    rollUpdate(ROLL_RIPCMDEXC,  cmdTravel - limCmd);
    rollUpdate(ROLL_RIPALTEXC,  altShift  - limStatAlt);
    rollUpdate(ROLL_RIPBATTEXC, battShift - limStatBatt);
    rollUpdate(ROLL_RIPRPMSHIFT, rpmShift - limStatRpm);
    bool commonOk = (filtRippleWinN >= minN) && halvesOk     // stall-starved window → extremes under-sampled
                    && !filtRippleWinProt                    // a protection clamp owned part of the window
                    && (cmdTravel <= limCmd);                // a commanded ramp is not ripple
    bool altSteady  = commonOk && (altShift  <= limStatAlt);
    bool battSteady = commonOk && (battShift <= limStatBatt);
    // RPM stationarity replaces the old same-bin edge-margin gate, and the table bins on the window-MEAN
    // RPM — a hunt wobbling across a 50-RPM boundary attributes to its center of mass instead of being
    // rejected (the straddle rejection was the other half of the idle starvation).
    bool rpmOk = (filtRippleWinRpmMin > 0.0f) && (rpmShift <= limStatRpm);
    int rpmBin = (int)(rpmMean / FA_RPM_BIN_W);
    bool tabValid = rpmOk && rpmMean > 0.0f && rpmBin >= 0 && rpmBin < RIPTAB_BINS;
    // Crossings gate (table fold only, per-detector): the measurement EMA must cross its 300 ms baseline
    // ≥ RIP_CROSS_MIN times in the window — a one-shot that slipped the stationarity test crosses ~2×,
    // real ripple/hunt crosses constantly. The test ring is exempt (a wizard hold must not starve; the
    // browser's median-of-8 is the robustness there).
    bool altFold  = tabValid && altSteady  && (altFiltCross  >= RIP_CROSS_MIN);
    // No shunt → Bcur is the raw INA228 input (noise when nothing is wired across it), and the battery
    // over-current detector this map feeds is itself HAS_BATT_SHUNT-gated — never fold battery ripple.
    bool battFold = HAS_BATT_SHUNT && tabValid && battSteady && (battFiltCross >= RIP_CROSS_MIN);
    // Throughput readouts: count windows that passed ALL of that detector's fold gates (game running or
    // not) — a frozen counter with green gate rows points at protection/starve.
    if (altFold)  g_ripAltAdmitCount++;
    if (battFold) g_ripBattAdmitCount++;
    // Agree-twice fold, game-gated (RPM_RIPPLE_TABLE_SPEC §2.3): first qualifying window becomes the
    // pending candidate; a second that agrees (±33% or ±0.5 A) commits the AVERAGE of the pair and
    // freezes that detector's cell; a disagree replaces the pending (the monster regrows). Every event
    // is forensic-logged so an unsettled bin stays explainable.
    if (ripGameFill && ripScoreArmed && (altFold || battFold)) {   // ripScoreArmed: current held at target ≥2s — never fold at the rest floor
      RipTabCell *c = &ripTab.cell[rpmBin];
      uint16_t rpmU = (uint16_t)fminf(rpm + 0.5f, 65535.0f);
      int ampLo = (altMean >= FA_AMP_BIN_LO)
                    ? FA_AMP_BIN_LO + ((int)((altMean - FA_AMP_BIN_LO) / FA_AMP_BIN_W)) * FA_AMP_BIN_W : 0;
      if (altFold && !(c->state & RIPTAB_ALT_DONE)) {
        if (!(c->state & RIPTAB_ALT_PEND)) {
          c->altPendX100 = altV;
          c->state |= RIPTAB_ALT_PEND;
          ripForensicPush(0, rpmU, ampLo, altMean, altV, altShift, cmdTravel, altFiltCross, 'P', 0);
        } else {
          float newA = altV * 0.01f, pendA = c->altPendX100 * 0.01f;
          if (fabsf(newA - pendA) <= fmaxf(RIPTAB_AGREE_FLOOR_A, RIPTAB_AGREE_FRAC * fmaxf(newA, pendA))) {
            c->altPkX100 = (uint16_t)(((uint32_t)altV + c->altPendX100 + 1) / 2);
            c->state = (uint8_t)((c->state & ~RIPTAB_ALT_PEND) | RIPTAB_ALT_DONE);
            ripForensicPush(0, rpmU, ampLo, altMean, altV, altShift, cmdTravel, altFiltCross, 'C', c->altPendX100);
            c->altPendX100 = 0;
          } else {
            ripForensicPush(0, rpmU, ampLo, altMean, altV, altShift, cmdTravel, altFiltCross, 'D', c->altPendX100);
            c->altPendX100 = altV;
          }
        }
      }
      if (battFold && !(c->state & RIPTAB_BATT_DONE)) {
        if (!(c->state & RIPTAB_BATT_PEND)) {
          c->battPendX100 = battV;
          c->state |= RIPTAB_BATT_PEND;
          ripForensicPush(1, rpmU, ampLo, battMean, battV, battShift, cmdTravel, battFiltCross, 'P', 0);
        } else {
          float newA = battV * 0.01f, pendA = c->battPendX100 * 0.01f;
          if (fabsf(newA - pendA) <= fmaxf(RIPTAB_AGREE_FLOOR_A, RIPTAB_AGREE_FRAC * fmaxf(newA, pendA))) {
            c->battPkX100 = (uint16_t)(((uint32_t)battV + c->battPendX100 + 1) / 2);
            c->state = (uint8_t)((c->state & ~RIPTAB_BATT_PEND) | RIPTAB_BATT_DONE);
            ripForensicPush(1, rpmU, ampLo, battMean, battV, battShift, cmdTravel, battFiltCross, 'C', c->battPendX100);
            c->battPendX100 = 0;
          } else {
            ripForensicPush(1, rpmU, ampLo, battMean, battV, battShift, cmdTravel, battFiltCross, 'D', c->battPendX100);
            c->battPendX100 = battV;
          }
        }
      }
      // IBV session stamp — span over windows that actually reached the fold
      if (!isnan(IBV) && IBV > 0.0f) {
        if (IBV < ripTab.sess.ibvMinV) ripTab.sess.ibvMinV = IBV;
        if (IBV > ripTab.sess.ibvMaxV) ripTab.sess.ibvMaxV = IBV;
      }
    }
    // Ring rows need BOTH sensors steady (one row carries both detectors' values).
    if (bcurRtestActive && altSteady && battSteady && bcurRtestCount < BCUR_RTEST_CAP) {
      BcurRtestPt &p = bcurRtest[bcurRtestCount];
      p.rpm = (uint16_t)fminf(rpm + 0.5f, 65535.0f);
      p.iX100 = (uint16_t)fminf(fabsf(battMean) * 100.0f + 0.5f, 65535.0f);   // operating battery current
      p.pkpkX100 = battV;
      p.iAltX100 = (uint16_t)fminf(fabsf(altMean) * 100.0f + 0.5f, 65535.0f); // operating alternator current
      p.altPkpkX100 = altV;
      // Worst positive voltage slope, min-of-halves (0 if either half never spanned a diff window)
      p.slopeVpsX1000 = (uint16_t)fminf(fmaxf(fminf(slpH1Max, slpH2Max), 0.0f) * 1000.0f + 0.5f, 65535.0f);
      bcurRtestCount++;
    }
    filtRippleWinReset(rpm, now);
  }
}

// ── Flat-top windowed FFT ──
// One FFT per 0.5 s window over the decimated 1.25 kSPS AC stream. Replaces the 16-bin log
// Goertzel bank, which scalloped: its bins spaced ~28% apart with main lobes only ~f/10 wide
// left gaps a tone could fall into and read up to ~6x low (a 28 Hz, 8.7 A real tone read 4.1 A).
// The FFT has uniform 0.5 Hz-class resolution with no gaps. Flat-top window = amplitude-accurate
// (near-zero scalloping); single transform per window, NO run-to-run averaging. faToneBuf holds
// this window's raw current (window mean removed at finalize for an exact DC reference);
// faFftRe/Im are the zero-padded transform workspace. All PSRAM.
#define FA_FFT_N 1024                       // power-of-2 transform size (zero-padded from 625)
#define FA_MIN_TONE_HZ 4.0f                 // peak-search floor. Below ~one cycle per window
                                            // (1/T = 2 Hz at 0.5 s) a tone isn't resolvable, and the
                                            // flat-top window's wide flat main lobe smears any near-DC
                                            // residual up into the lowest bins as a phantom 1-2 Hz tone.
                                            // 4 Hz clears both; real tones of interest start ~10 Hz.
static float *faToneBuf = NULL;             // PSRAM — FA_WIN_DECIM_N raw decimated current samples (window mean removed at finalize)
static float *faFftRe = NULL, *faFftIm = NULL;  // PSRAM — FA_FFT_N transform workspace
static float *faFtWin = NULL;               // PSRAM — FA_WIN_DECIM_N flat-top window table
static float faFtWinSum = 1.0f;             // Σ window (single-sided amplitude normalization)
static float *faTwidRe = NULL, *faTwidIm = NULL;  // PSRAM — FA_FFT_N/2 twiddle table (float32-safe, no recurrence drift)
static uint16_t *faBitRev = NULL;           // PSRAM — FA_FFT_N bit-reversal permutation

// ── Steady-state gate (independent of the alt-health detector, deliberately cheap) ──
// 1 s-TC EMAs on RPM and on this channel's own amps. The 1 s constant kills the 10 Hz
// band ~60× and the 28 Hz band ~175× — the gate must not see the resonances being
// measured. Over the 0.5 s window: RPM EMA inside one bin with ≥10 RPM edge margin; amps
// EMA drift ≤ max(2 A, 5% of mean); no protection active; no attenuation switch; no
// railed codes; no sample loss (wall-clock audit vs the crystal-timed sample count).
#define FA_EMA_ALPHA (1.0f / 1250.0f)  // 1 s TC — RPM EMA (binning, page label, detector ctx)
// Amps gate EMA: 300 ms TC, fast enough that an end-of-window current collapse shows as drift
// (1 s was too slow — a ~38 A tail collapse moved it only ~2 A, under the floor, so it passed).
#define FA_AMPS_EMA_ALPHA (1.0f / 375.0f)  // 300 ms TC at the 1250 SPS decimated rate
#define FA_RPM_EDGE_MARGIN 2.0f    // guard band each side of a 50-RPM bin edge; m carves 2m RPM of map dead-zone per bin (2 -> 8%). The same-bin (no-straddle) test is the real correctness gate; this is just fringe-filing guard.
#define FA_WIN_WALL_MAX_MS 580UL   // 0.5 s nominal (625 @ 1250 SPS) + ~80 ms loop-jitter margin; beyond this the window lost samples (DMA pool overflow)

// ── User-tunable knobs (Pattern B settings — NVS-backed, echoed on CSV3) ──
// Converted from the design-time #defines above so the bench rail point and the steady-state
// gate can be retuned in the field without a reflash. Defaults equal the #define values, so a
// fresh device behaves identically. Detector internals stay design-time (spec: no user knob).
bool faEnabled = true;                       // global ON/OFF for the whole fast-alt subsystem
bool faAlarmEnable = false;                  // FAULT verdict drives the audible alarm (AlarmActivate-gated)
bool faAnomPause = false;                    // freeze the anomaly flipbook slots (stop overwrites)
float faRpmEdgeMargin = FA_RPM_EDGE_MARGIN;  // RPM band-edge margin the steady gate needs (RPM)
float faAmpsDriftFloorA = 2.0f;              // amps-drift gate floor (A) — gate uses max(this, pct% of mean)
float faAmpsDriftPct = 5.0f;                 // amps-drift gate slope (percent of mean amps)
float faAttenUpAmps = FA_ATTEN_UP_AMPS;      // switch to 12 dB above this mean current (A)
float faAttenDownAmps = FA_ATTEN_DOWN_AMPS;  // back to 6 dB below this mean current (A)
float faPeakMinA = FA_PEAK_MIN_A;            // spectral peaks below this never enter a matrix cell (A)

// Anomaly counter (item 6) — increments on every detector FAULT verdict; persisted via
// saveNVSDataFull() ("storage" namespace, prev_ shadow) and echoed on CSV2. Fleet scalar.
uint32_t faAnomalyCount = 0;
uint32_t prev_faAnomalyCount = 0;
unsigned long faLastFaultMs = 0;  // millis() of the most recent FAULT verdict (alarm dwell window)

static float faAmpsEma = 0.0f, faRpmEma = 0.0f;
static bool faEmaSeeded = false;
static float faWinRpmEmaMin, faWinRpmEmaMax;
static float faWinAmpsEmaMin, faWinAmpsEmaMax;
static float faWinDAmpH1Min, faWinDAmpH1Max, faWinDAmpH2Min, faWinDAmpH2Max;  // per-half decimated extremes → min-of-halves pk-pk (a one-shot lands in one half and is rejected)
static double faWinAmpsSum = 0.0;
static bool faWinProtection = false;
static unsigned long faWinStartMs = 0;

// Per-session worsts (fleet scalars, consumer 5) — faSesPkpkWorstA / faSesPeakWorstA /
// faSesPeakWorstHz are declared in Xregulator.ino (buildConfigPayload above reads them).

// ── Reference flipbook (consumer 4) ──
// ONE axis: 1000-RPM bands. A page is captured when its band has no snapshot, amps are
// 20–100 A, and the steady-state gate passed. 20 kSPS (raw ring, no decimation), 200 ms,
// ~8 KB/page. Full rate so the rectifier-ripple shape stays unaliased across the whole RPM
// band — the 6-pulse ripple is 0.6·PulleyRatio·engineRPM Hz, which exceeds the old 5 kSPS
// Nyquist (2.5 kHz) above idle; 20 kSPS holds clean to the top of the 5000-RPM bands.
// Pages FREEZE once captured; only the re-baseline button clears the book.
// Anomaly-triggered captures (detector fired) are stored alongside for before/after.
#define FA_FLIP_BANDS 5  // 1000-RPM bands, 0–4999
#define FA_FLIP_ANOM 4   // anomaly capture slots (round-robin)
#define FA_FLIP_SLOTS (FA_FLIP_BANDS + FA_FLIP_ANOM)
#define FA_FLIP_NSAMP 4000  // 200 ms @ 20 kSPS
#define FA_FLIP_PATH "/faflip.bin"
#define FA_FLIP_MAGIC 0x46414642u  // 'FAFB'
#define FA_FLIP_VER 1
struct FaFlipPage {
  uint16_t rpm;       // tach at capture
  uint16_t ampsX10;   // window mean amps ×10
  uint32_t epoch;     // unix time at capture (0 = clock not yet synced)
  uint8_t used;       // 0 = slot empty (set LAST on capture so readers never see a torn page)
  uint8_t isAnomaly;  // 0 = reference page, 1 = detector-fired capture
  uint8_t patternK;   // anomaly: winning modulo-k fault class
  uint8_t attenIs12;  // range at capture (scaling context)
  float score;        // anomaly: detector score
  uint16_t zeroMv;    // amps conversion at capture: (mv − zeroMv) × apv / 1000
  uint16_t apv;
  int16_t mv[FA_FLIP_NSAMP];
};
static_assert(sizeof(FaFlipPage) == 8020, "FaFlipPage layout is parsed byte-by-byte in script.js — keep in sync");
static FaFlipPage *faFlip = NULL;  // PSRAM: slots 0..4 = reference bands, 5..8 = anomaly ring
static bool faFlipDirty = false;
static uint8_t faAnomNext = 0;
static volatile bool faPendingRebaseline = false;  // set by /get handler (Core 0), executed on Core 1
static volatile bool faPendingFlipWipeAll = false;  // tach rescale: wipe ALL 9 pages, anomaly captures included

// Detector outputs for the dashboard (CSV2) + console rate limiting
uint8_t faDetectLastK = 0;  // dashboard field: winning fault class of the last FAULT verdict, 0 = quiet
static unsigned long faDetectMsgMs = 0, faAnomCapMs = 0, faDetTrendMsgMs = 0;

// Failure detector (consumer 2).
// The algorithm is the FAD core further down: a faithful port of rect_fault_detector.py,
// gated 18/18 against the synthetic set on desktop both before and after integration.
// It does NOT read the scope ring or the decimated stream — crest picking needs the raw
// 20 kSPS stream — so each measurement window's raw samples are also captured into
// faDetWin (window-aligned to within one boxcar-16 group). A qualified window ARMS a job
// on that buffer; the Core-0 faDetTask runs the analysis and faDetectorPoll() consumes the
// verdict on Core 1 ~tens of ms later (latency is irrelevant — a human response takes minutes,
// and keeping the heavy math off Core 1 is the whole point). Division of
// labor: first-line pk-pk-vs-cell-history detection
// belongs to the disturbance matrix (consumer 1); this detector covers cold start (no cell
// history yet), fault classification (winning k), and triggering the flipbook capture.
#define FA_DET_WIN_N 40000  // 2 s raw detector capture. DECOUPLED from the 0.5 s tone window
                            // (FA_WIN_DECIM_N) so the detector keeps its 2 s analysis length: it
                            // fills across 4 consecutive CLEAN tone windows (faWindowFinalize resets the
                            // capture on any non-clean window), preserving "one contiguous clean 2 s capture".
#define FA_DET_MIN_PERIOD_MS 60000UL              // one analysis per minute is plenty
static int16_t *faDetWin = NULL;  // PSRAM — window-aligned raw capture (detector input)
static int faDetWinN = 0;
static bool faDetFilling = false;  // faWinReset opens the fill; arming freezes the buffer
static bool faDetBusy = false;     // a job owns faDetWin until its verdict lands
static unsigned long faDetLastStartMs = 0;
static uint16_t faDetCtxRpm = 0;  // gate context at arm time (flipbook page metadata)
static float faDetCtxAmps = 0.0f;

static inline float faAmpsPerVolt() {
  // AmpSensorRange 0/1/2 = 200/300/500 A sensors — all are 200/300/500 A per ADC volt
  static const float t[3] = { 200.0f, 300.0f, 500.0f };
  return (AmpSensorRange >= 0 && AmpSensorRange <= 2) ? t[AmpSensorRange] : 300.0f;
}

static inline float faMvToAmps(float mv) {
  // faCalGain/faCalOffA: two-point calibration against the ADS1115 path, learned by the
  // field-decay test (defaults 1.0/0.0 = uncalibrated). Gain also scales AC amplitudes
  // (matrix/ripple) — a correction, not a break; offset cancels in anything AC.
  return (mv - FA_ZERO_MV) * (faAmpsPerVolt() * 0.001f) * faCalGain + faCalOffA;
}

// UNcalibrated conversion — the field-decay test solves for faCalGain/faCalOffA from
// nominal readings vs ADS references, so it must see the raw scaling.
static inline float faMvToAmpsNominal(float mv) {
  return (mv - FA_ZERO_MV) * (faAmpsPerVolt() * 0.001f);
}

// 2-point linear fit through the eFuse factory calibration curve for one attenuation.
// Falls back to nominal full-scale scaling if the chip lacks curve-fitting data.
static void faCaliCompute(adc_atten_t atten, float nominalFsMv, float *slope, float *off) {
  *slope = nominalFsMv / 4095.0f;
  *off = 0.0f;
  adc_cali_handle_t h = NULL;
  adc_cali_curve_fitting_config_t cfg = {};
  cfg.unit_id = ADC_UNIT_1;
  cfg.chan = FA_ADC_CHANNEL;
  cfg.atten = atten;
  cfg.bitwidth = ADC_BITWIDTH_12;
  if (adc_cali_create_scheme_curve_fitting(&cfg, &h) == ESP_OK) {
    int mvLo = 0, mvHi = 0;
    if (adc_cali_raw_to_voltage(h, 400, &mvLo) == ESP_OK && adc_cali_raw_to_voltage(h, 3600, &mvHi) == ESP_OK && mvHi > mvLo) {
      *slope = (float)(mvHi - mvLo) / 3200.0f;
      *off = (float)mvLo - (*slope) * 400.0f;
    }
    adc_cali_delete_scheme_curve_fitting(h);
  }
}

// (Re)configure the continuous driver for one attenuation and start it. Caller must have
// stopped the driver first (or never started it). ~ms — only ever runs at init or between
// measurement windows, never inside one.
static bool faConfigAndStart(bool atten12) {
  adc_digi_pattern_config_t pat = {};
  pat.atten = atten12 ? ADC_ATTEN_DB_12 : ADC_ATTEN_DB_6;
  pat.channel = FA_ADC_CHANNEL;
  pat.unit = ADC_UNIT_1;
  pat.bit_width = ADC_BITWIDTH_12;
  adc_continuous_config_t dig = {};
  dig.pattern_num = 1;
  dig.adc_pattern = &pat;
  dig.sample_freq_hz = FA_SAMPLE_RATE_HZ;
  dig.conv_mode = ADC_CONV_SINGLE_UNIT_1;
  dig.format = ADC_DIGI_OUTPUT_FORMAT_TYPE2;
  if (adc_continuous_config(faAdcHandle, &dig) != ESP_OK) return false;
  if (atten12) {
    faMvSlope = faMvSlope12;
    faMvOff = faMvOff12;
  } else {
    faMvSlope = faMvSlope6;
    faMvOff = faMvOff6;
  }
  return adc_continuous_start(faAdcHandle) == ESP_OK;
}

// Build the flat-top window, twiddle and bit-reversal tables (once, from faInit). Tables are
// precomputed so the per-window transform does no cos/sf and no float32 twiddle recurrence
// (which drifts over 512 stages) — keeps amplitude accuracy.
static void faFftInit() {
  if (!faFtWin || !faTwidRe || !faTwidIm || !faBitRev) return;
  // 5-term flat-top (matches the validated Python prototype)
  const float a0 = 0.21557895f, a1 = 0.41663158f, a2 = 0.277263158f,
              a3 = 0.083578947f, a4 = 0.006947368f;
  float sum = 0.0f;
  for (int i = 0; i < FA_WIN_DECIM_N; i++) {
    float th = 2.0f * (float)M_PI * (float)i / (float)(FA_WIN_DECIM_N - 1);
    float w = a0 - a1 * cosf(th) + a2 * cosf(2.0f * th) - a3 * cosf(3.0f * th) + a4 * cosf(4.0f * th);
    faFtWin[i] = w;
    sum += w;
  }
  faFtWinSum = (sum > 1e-6f) ? sum : 1.0f;
  for (int i = 0; i < FA_FFT_N / 2; i++) {
    float ang = -2.0f * (float)M_PI * (float)i / (float)FA_FFT_N;
    faTwidRe[i] = cosf(ang);
    faTwidIm[i] = sinf(ang);
  }
  for (int i = 0; i < FA_FFT_N; i++) {
    int j = 0, x = i;
    for (int b = 1; b < FA_FFT_N; b <<= 1) { j = (j << 1) | (x & 1); x >>= 1; }
    faBitRev[i] = (uint16_t)j;
  }
}

// In-place iterative radix-2 DIT FFT on faFftRe/faFftIm (length FA_FFT_N), table-driven twiddles.
// Validated against numpy (max err 7e-12) recovering 8.72 A on the real capture.
static void faFft() {
  const int n = FA_FFT_N;
  for (int i = 0; i < n; i++) {       // bit-reversal permutation
    int j = faBitRev[i];
    if (i < j) {
      float tr = faFftRe[i]; faFftRe[i] = faFftRe[j]; faFftRe[j] = tr;
      float ti = faFftIm[i]; faFftIm[i] = faFftIm[j]; faFftIm[j] = ti;
    }
  }
  for (int len = 2; len <= n; len <<= 1) {     // butterfly stages
    int half = len >> 1;
    int step = n / len;                        // twiddle stride into the table
    for (int start = 0; start < n; start += len) {
      int ti = 0;
      for (int k = 0; k < half; k++, ti += step) {
        float wr = faTwidRe[ti], wi = faTwidIm[ti];
        int a = start + k, b = a + half;
        float tr = wr * faFftRe[b] - wi * faFftIm[b];
        float tj = wr * faFftIm[b] + wi * faFftRe[b];
        faFftRe[b] = faFftRe[a] - tr; faFftIm[b] = faFftIm[a] - tj;
        faFftRe[a] += tr;             faFftIm[a] += tj;
      }
    }
  }
}

static void faWinReset() {
  faDecimWinN = 0;
  // Detector capture is DECOUPLED from this 0.5 s window (FA_DET_WIN_N = 2 s): it accumulates
  // ACROSS windows and is NOT zeroed here. We only resume/pause filling around an active job;
  // faWindowFinalize zeroes faDetWinN on a non-clean window (restart contiguous-clean capture),
  // and faDetectorPoll zeroes it on completion. The boxcar-16 group may straddle a boundary
  // (raw/decimated skew ≤15 samples, 0.75 ms) — immaterial for a self-referenced detector.
  faDetFilling = (!faDetBusy && faDetWin != NULL);
  faWinRailed = false;
  faWinProtection = false;
  faWinRpmEmaMin = faWinAmpsEmaMin = faWinDAmpH1Min = faWinDAmpH2Min = 1e9f;
  faWinRpmEmaMax = faWinAmpsEmaMax = faWinDAmpH1Max = faWinDAmpH2Max = -1e9f;
  faWinAmpsSum = 0.0;
  faWinStartMs = millis();
  // (No spectral state to clear — the FFT reads faToneBuf[0..faDecimWinN-1] fresh each window.)
}

// Fold one found peak into a cell: ±5% frequency match → recent-average (1/min(n+1,N)); else take a
// free slot; else displace the weakest stored peak only if the newcomer is stronger
// (restarting that slot's mean — the cell keeps its 6 most energetic bands over time).
static void faCellFoldPeak(int cellIdx, float fHz, float ampA) {
  FaCell *c = &faMatrix[cellIdx];
  int freeSlot = -1, weakest = -1;
  for (int s = 0; s < FA_CELL_PEAKS; s++) {
    if (c->pk[s].nAcc == 0) {
      if (freeSlot < 0) freeSlot = s;
      continue;
    }
    float F = c->pk[s].freqHzX10 / 10.0f;
    if (fabsf(fHz - F) <= 0.05f * F) {  // matched: recent-average freq + amplitude (1/min(n+1,N))
      uint32_t n = c->pk[s].nAcc;
      float w = (float)((n + 1 < FA_CELL_AVG_N) ? (n + 1) : FA_CELL_AVG_N);
      float fMean = F + (fHz - F) / w;
      float aMean = c->pk[s].ampAX100 / 100.0f;
      aMean += (ampA - aMean) / w;
      c->pk[s].freqHzX10 = (uint16_t)(fMean * 10.0f + 0.5f);
      c->pk[s].ampAX100 = (uint16_t)fminf(aMean * 100.0f + 0.5f, 65535.0f);
      if (c->pk[s].nAcc < 65535) c->pk[s].nAcc++;
      return;
    }
    if (weakest < 0 || c->pk[s].ampAX100 < c->pk[weakest].ampAX100) weakest = s;
  }
  int slot = (freeSlot >= 0) ? freeSlot : ((weakest >= 0 && ampA * 100.0f > c->pk[weakest].ampAX100) ? weakest : -1);
  if (slot < 0) return;
  c->pk[slot].freqHzX10 = (uint16_t)(fHz * 10.0f + 0.5f);
  c->pk[slot].ampAX100 = (uint16_t)fminf(ampA * 100.0f + 0.5f, 65535.0f);
  c->pk[slot].nAcc = 1;
}

// Window finalize — runs every FA_WIN_DECIM_N decimated samples (0.5 s, crystal-timed).
// Applies the steady-state gate, runs the flat-top FFT and merges its peaks into the
// disturbance matrix, and (further down the chain) feeds the flipbook + detector.
static void faWindowFinalize() {
  bool clean = !(faWinRailed || faWinAttenSwitched);
  // Sample-loss audit: the window is exactly 0.5 s of crystal-timed samples; if more wall
  // time than that passed, the DMA pool overflowed during a loop stall and samples are gone.
  if (millis() - faWinStartMs > FA_WIN_WALL_MAX_MS) clean = false;

  bool gated = false;
  bool detWindowOk = false;  // this window suitable to extend the detector's contiguous-clean 2 s capture
  float winMeanAmps = (faDecimWinN > 0) ? (float)(faWinAmpsSum / faDecimWinN) : 0.0f;
  if (clean && !faWinProtection && faEmaSeeded) {
    int rpmBinLo = (int)(faWinRpmEmaMin / FA_RPM_BIN_W);
    int rpmBinHi = (int)(faWinRpmEmaMax / FA_RPM_BIN_W);
    bool rpmSteady = (rpmBinLo == rpmBinHi)
                     && (faWinRpmEmaMin - rpmBinLo * FA_RPM_BIN_W >= faRpmEdgeMargin)
                     && ((rpmBinLo + 1) * FA_RPM_BIN_W - faWinRpmEmaMax >= faRpmEdgeMargin);
    float ampsDriftMax = fmaxf(faAmpsDriftFloorA, faAmpsDriftPct * 0.01f * fabsf(winMeanAmps));
    bool ampsSteady = (faWinAmpsEmaMax - faWinAmpsEmaMin) <= ampsDriftMax;
    // Gate-tuning readouts: feed the live 10s trackers with the exact quantities these gates compare —
    // the worst (smallest) of the two RPM edge margins, and the amps EMA spread. Negative edge margin
    // when RPM straddled a bin boundary is intentional (the gate fails it too).
    rollUpdate(ROLL_RPMEDGE, fminf(faWinRpmEmaMin - rpmBinLo * FA_RPM_BIN_W,
                                   (rpmBinLo + 1) * FA_RPM_BIN_W - faWinRpmEmaMax));
    rollUpdate(ROLL_AMPSDRIFT, faWinAmpsEmaMax - faWinAmpsEmaMin);
    // Same EMA spread minus this window's own effective limit (floor vs pct-of-mean, whichever binds).
    // <=0 means the drift gate passed; the 10s peak of this is the dashboard's exact pass/fail signal —
    // no client-side recompute or raw-INA proxy for the mean current.
    rollUpdate(ROLL_AMPSDRIFTEXC, (faWinAmpsEmaMax - faWinAmpsEmaMin) - ampsDriftMax);
    bool binValid = (rpmBinLo >= 0 && rpmBinLo < FA_RPM_BINS);
    if (faCommissionGate) {
      // Commissioning Phase-2 variant: the operator creeps the throttle up continuously, so the
      // strict dead-steady gate (edge margin + amps-steady) would qualify nothing. Relax to
      // "RPM stayed inside one 50-RPM bin during this 0.5 s window" — drift allowed, no edge
      // margin, no amps-steadiness. The map is a bootstrap (floor + ongoing refinement), so
      // one window/bin is coverage, not depth. Strict gate stays for normal-operation accumulation.
      gated = (rpmBinLo == rpmBinHi) && binValid;
    } else {
      gated = rpmSteady && ampsSteady && binValid;
    }
    // Detector arming is RPM-FREE (see faMaybeArmDetector): a clean, current-steady window with
    // real current flowing is enough — no RPM-steadiness or valid-bin requirement. This keeps
    // fault detection working when the tach is dead or erratic, the failure modes that motivated
    // a self-referenced detector. The matrix/flipbook below still require the full RPM gate.
    detWindowOk = (ampsSteady && winMeanAmps >= FA_AMP_BIN_LO);
    if (detWindowOk) faMaybeArmDetector(winMeanAmps);
    if (gated && faMatrix) {
      int ampBin = (int)((winMeanAmps - FA_AMP_BIN_LO) / FA_AMP_BIN_W);
      if (winMeanAmps >= FA_AMP_BIN_LO && ampBin >= 0 && ampBin < FA_AMP_BINS) {
        // Flat-top windowed FFT of this window's AC samples (zero-padded to FA_FFT_N), single-
        // sided amplitude spectrum, then local maxima with 3-bin parabolic interpolation. Uniform
        // resolution, no scalloping gaps — the bug the log Goertzel bank had (a 28 Hz tone read
        // ~2x low). Magnitudes are taken on the fly to avoid a full 512-bin spectrum buffer.
        for (int i = 0; i < FA_WIN_DECIM_N; i++) faFftRe[i] = (faToneBuf[i] - winMeanAmps) * faFtWin[i];
        for (int i = FA_WIN_DECIM_N; i < FA_FFT_N; i++) faFftRe[i] = 0.0f;
        for (int i = 0; i < FA_FFT_N; i++) faFftIm[i] = 0.0f;
        faFft();
        const int half = FA_FFT_N / 2;
        const float binHz = 1250.0f / (float)FA_FFT_N;
        const float ampScale = 2.0f / faFtWinSum;  // single-sided amplitude, window-corrected
        float pf[FA_CELL_PEAKS * 4], pa[FA_CELL_PEAKS * 4];
        const int paCap = (int)(sizeof(pf) / sizeof(pf[0]));
        int np = 0;
        // Peak search starts at FA_MIN_TONE_HZ, not bin 1: the lowest bins carry only DC-leakage
        // residual and sub-one-cycle content, not resolvable tones. aPrev seeds from the bin just
        // below so the first searched bin's local-max test is still valid.
        int firstBin = (int)ceilf(FA_MIN_TONE_HZ / binHz);
        if (firstBin < 1) firstBin = 1;
        float winTonePk = 0.0f;
        float aPrev = (firstBin > 1) ? ampScale * sqrtf(faFftRe[firstBin - 1] * faFftRe[firstBin - 1] + faFftIm[firstBin - 1] * faFftIm[firstBin - 1]) : 0.0f;
        float aCur = ampScale * sqrtf(faFftRe[firstBin] * faFftRe[firstBin] + faFftIm[firstBin] * faFftIm[firstBin]);
        for (int i = firstBin; i < half - 1; i++) {
          float aNext = ampScale * sqrtf(faFftRe[i + 1] * faFftRe[i + 1] + faFftIm[i + 1] * faFftIm[i + 1]);
          if (aCur > winTonePk) winTonePk = aCur;
          if (aCur >= faPeakMinA && aCur > aPrev && aCur >= aNext && np < paCap) {
            float fHz = (float)i * binHz, aPk = aCur;
            float denom = aPrev - 2.0f * aCur + aNext;
            if (fabsf(denom) > 1e-9f) {
              float delta = 0.5f * (aPrev - aNext) / denom;
              if (delta > -0.5f && delta < 0.5f) {
                fHz = ((float)i + delta) * binHz;
                aPk = aCur - 0.25f * (aPrev - aNext) * delta;
              }
            }
            pf[np] = fHz;
            pa[np] = aPk;
            np++;
          }
          aPrev = aCur;
          aCur = aNext;
        }
        // Gate-tuning readout: largest spectral amplitude this window (what faPeakMinA floors out).
        rollUpdate(ROLL_TONEPK, winTonePk);
        // Top-6 by amplitude (selection sort over ≤16 entries)
        for (int i = 0; i < np && i < FA_CELL_PEAKS; i++) {
          int best = i;
          for (int j = i + 1; j < np; j++)
            if (pa[j] > pa[best]) best = j;
          float tf = pf[i], ta = pa[i];
          pf[i] = pf[best]; pa[i] = pa[best];
          pf[best] = tf; pa[best] = ta;
        }
        if (np > FA_CELL_PEAKS) np = FA_CELL_PEAKS;
        int cellIdx = rpmBinLo * FA_AMP_BINS + ampBin;
        FaCell *c = &faMatrix[cellIdx];
        float apvK = faAmpsPerVolt() * 0.001f;
        // Min-of-halves broadband pk-pk: keep the SMALLER of the two half-windows' extremes. A one-shot
        // transient inflates only one half, so the min stays honest; real ripple (period ≤ quarter-window)
        // recurs in both. Feeds the map cell AND the worst-ripple hold, so both reject single spikes.
        float pkH1 = faWinDAmpH1Max - faWinDAmpH1Min;
        float pkH2 = faWinDAmpH2Max - faWinDAmpH2Min;
        float pkpkA = (pkH1 >= 0.0f && pkH2 >= 0.0f) ? fminf(pkH1, pkH2) * apvK : 0.0f;
        // pk-pk recent-averaged like the peaks (consistency for drift comparison): windows
        // here is the pre-increment count, so windows+1 is this window's ordinal, capped at N
        float pkMean = c->pkpkAX100 / 100.0f;
        uint32_t pw = (uint32_t)c->windows + 1;
        if (pw > FA_CELL_AVG_N) pw = FA_CELL_AVG_N;
        pkMean += (pkpkA - pkMean) / (float)pw;
        c->pkpkAX100 = (uint16_t)fminf(pkMean * 100.0f + 0.5f, 65535.0f);
        for (int i = 0; i < np; i++) faCellFoldPeak(cellIdx, pf[i], pa[i]);
        if (c->windows == 0) faCellsUsed++;
        if (c->windows < 65535) c->windows++;
        faMatrixDirtyWindows++;
        // Fleet scalars: per-session worsts
        if (pkpkA > faSesPkpkWorstA) {
          faSesPkpkWorstA = pkpkA;
          // Capture the operating point for the dashboard mini-table
          faSesPkpkAmpsA = winMeanAmps;
          faSesPkpkTempF = AlternatorTemperatureF;
          faSesPkpkRpm   = (uint16_t)(rpmBinLo * FA_RPM_BIN_W + FA_RPM_BIN_W / 2);
          faSesPkpkEpoch = timeIsSynced ? (uint32_t)time(NULL) : 0;
        }
        if (np > 0 && pa[0] > faSesPeakWorstA) {
          faSesPeakWorstA = pa[0];
          faSesPeakWorstHz = pf[0];
        }
        // Highest Tone in Map headline — peak-hold of the loudest tone (pk-pk) and the RPM
        // bin center where it occurred. Tracked independently of the persistent map so it
        // clears properly (see faDomReset): only Reset Worsts / Clear Map zero it.
        if (np > 0) {
          uint16_t domPkpkX100 = (uint16_t)fminf(pa[0] * 2.0f * 100.0f + 0.5f, 65535.0f);
          if (domPkpkX100 > faDomAmpAX100) {
            faDomAmpAX100 = domPkpkX100;
            faDomFreqHzX10 = (uint16_t)fminf(pf[0] * 10.0f + 0.5f, 65535.0f);
            faDomRpm = (uint16_t)(rpmBinLo * FA_RPM_BIN_W + FA_RPM_BIN_W / 2);
            // Capture the operating point for the dashboard mini-table
            faDomAmpsA = winMeanAmps;
            faDomTempF = AlternatorTemperatureF;
            faDomEpoch = timeIsSynced ? (uint32_t)time(NULL) : 0;
          }
        }
        // Flipbook capture + detector run hang here (qualified window, RPM/amps known)
        faQualifiedWindowHook(rpmBinLo * FA_RPM_BIN_W, winMeanAmps);
      }
    }
  }
  // Detector capture is contiguous-clean: a window unsuitable for the detector (not clean, under
  // protection, or current not steady/flowing) discards the partial 2 s capture so the analyzed
  // buffer is always one uninterrupted clean stretch assembled from consecutive 0.5 s windows.
  if (!detWindowOk && !faDetBusy) faDetWinN = 0;
  if (clean) faWindowsAccepted++;
  else faWindowsDiscarded++;

  bool switchedNow = false;
  // Attenuation switch executes ONLY here, between windows. The first window on the new
  // range is discarded (conservative — reconfig gap + fresh range mid-stream).
  if (faAttenPending != 0xFF) {
    uint8_t want = faAttenPending;
    faAttenPending = 0xFF;
    if (want != faAttenIs12) {
      adc_continuous_stop(faAdcHandle);
      faAttenIs12 = want;
      if (!faConfigAndStart(want != 0)) {
        faChanState = 0;
        queueConsoleMessage("Fast alt-current channel: range switch failed -- channel off");
        return;
      }
      faAttenLastSwitchMs = millis();
      switchedNow = true;
      faBoxAcc = 0;
      faBoxN = 0;
    }
  }
  faWinReset();
  faWinAttenSwitched = switchedNow;
}

static void faProcessDecimated(int16_t dmv) {
  if (faDecimWinN == 0) faWinStartMs = millis();
  // RPM EMA slow (1 s, just labels/bins); amps EMA fast (300 ms) so the drift gate sees
  // end-of-window collapses. FFT DC reference is the window mean, not either EMA.
  float ampsNow = faMvToAmps((float)dmv);
  if (!faEmaSeeded) {
    faAmpsEma = ampsNow;
    faRpmEma = RPM;
    faEmaSeeded = true;
  } else {
    faAmpsEma += FA_AMPS_EMA_ALPHA * (ampsNow - faAmpsEma);
    faRpmEma += FA_EMA_ALPHA * (RPM - faRpmEma);
  }
  if (faRpmEma < faWinRpmEmaMin) faWinRpmEmaMin = faRpmEma;
  if (faRpmEma > faWinRpmEmaMax) faWinRpmEmaMax = faRpmEma;
  if (faAmpsEma < faWinAmpsEmaMin) faWinAmpsEmaMin = faAmpsEma;
  if (faAmpsEma > faWinAmpsEmaMax) faWinAmpsEmaMax = faAmpsEma;
  if (faDecimWinN * 2 < FA_WIN_DECIM_N) {   // first vs second half by sample index (crystal-timed, pre-increment)
    if ((float)dmv < faWinDAmpH1Min) faWinDAmpH1Min = (float)dmv;
    if ((float)dmv > faWinDAmpH1Max) faWinDAmpH1Max = (float)dmv;
  } else {
    if ((float)dmv < faWinDAmpH2Min) faWinDAmpH2Min = (float)dmv;
    if ((float)dmv > faWinDAmpH2Max) faWinDAmpH2Max = (float)dmv;
  }
  faWinAmpsSum += ampsNow;
  if (g_loadDumpActive || alarmLatch) faWinProtection = true;  // "no protection active" gate leg
  // Buffer the raw decimated current for this window's flat-top FFT; the window MEAN (not the
  // lagging 1 s EMA) is subtracted in one shot at finalize. Subtracting the EMA here left a
  // residual DC offset the flat-top's wide flat main lobe read as a phantom ~1-2 Hz tone.
  if (faToneBuf && faDecimWinN < FA_WIN_DECIM_N) faToneBuf[faDecimWinN] = ampsNow;
  faDecimWinN++;
  if (faDecimWinN >= FA_WIN_DECIM_N) TIMED_CALL(ft_faWindowFinalize, faWindowFinalize());
}

static inline void faProcessSample(int16_t mv) {
  if (faDetFilling && faDetWinN < FA_DET_WIN_N) faDetWin[faDetWinN++] = mv;  // detector raw capture
  faBoxAcc += mv;
  if (++faBoxN >= FA_DECIM) {
    int16_t dmv = (int16_t)(faBoxAcc / FA_DECIM);
    faBoxAcc = 0;
    faBoxN = 0;
    faProcessDecimated(dmv);
  }
}

// Bounded drain — called from loop() every pass via TIMED_CALL(ft_fastAltDrain, ...).
// Empties the driver's DMA pool in ≤256-sample chunks, hard-capped at ~1 ms. Pool overflow
// during a long loop stall silently drops samples; a window that lost samples is junk
// spectrally, but it is also time-dilated and non-steady, so the steady-state gate
// rejects it before anything is recorded.
// Bring the continuous-ADC driver up (handle + 6 dB config + start). Shared by faInit() and
// the runtime OFF->ON path in faDrain(). Buffers must already be allocated by faInit().
static bool faStartDriver() {
  if (faAdcHandle) return true;  // already running
  adc_continuous_handle_cfg_t hcfg = {};
  hcfg.max_store_buf_size = FA_POOL_BYTES;
  hcfg.conv_frame_size = FA_FRAME_BYTES;
  if (adc_continuous_new_handle(&hcfg, &faAdcHandle) != ESP_OK) {
    Serial.println("faStartDriver: adc_continuous_new_handle failed -- fast alt-current channel off");
    faAdcHandle = NULL;
    return false;
  }
  faCaliCompute(ADC_ATTEN_DB_6, 1750.0f, &faMvSlope6, &faMvOff6);
  faCaliCompute(ADC_ATTEN_DB_12, 3100.0f, &faMvSlope12, &faMvOff12);
  if (!faConfigAndStart(false)) {
    Serial.println("faStartDriver: ADC config/start failed -- fast alt-current channel off");
    return false;
  }
  faAttenIs12 = 0;
  faChanState = 1;
  faAttenLastSwitchMs = millis();
  Serial.printf("faStartDriver: fast alt-current channel sampling GPIO3 @ %d Hz, 6 dB\n", FA_SAMPLE_RATE_HZ);
  return true;
}

// Tear the driver down for a global OFF (item 4). Buffers stay allocated so a later ON can
// restart without re-allocating; the matrix/flipbook are left intact for the UI to read.
static void faStopDriver() {
  if (faAdcHandle) {
    adc_continuous_stop(faAdcHandle);
    adc_continuous_deinit(faAdcHandle);
    faAdcHandle = NULL;
  }
  faChanState = 0;
}

void faDrain() {
  if (!faEnabled) {                                   // global ON/OFF (item 4) — disabled: never sample
    if (faAdcHandle) faStopDriver();                  // stop a running driver if just turned off
    return;
  }
  if (faChanState == 0) {                             // enabled but not sampling (boot-disabled then on, or post-stop)
    if (faRawRing && !faAdcHandle) faStartDriver();   // buffers exist from faInit() — bring the driver up
    return;
  }
  if (!faAdcHandle) return;
  if (faChanState == 2) {  // dormant — near-zero cost; re-probe occasionally
    if (millis() - faLastProbeMs >= FA_REPROBE_MS) {
      faLastProbeMs = millis();
      if (adc_continuous_start(faAdcHandle) == ESP_OK) {
        faChanState = 1;
        faRailedStreak = 0;
      }
    }
    return;
  }
  // Range-switch request keys on mean amps from the ADS path (the firmware's authoritative
  // slow current). Request only — the switch itself waits for a window boundary.
  if (millis() - faAttenLastSwitchMs >= FA_ATTEN_DWELL_MS && faAttenPending == 0xFF) {
    if (!faAttenIs12 && MeasuredAmps > faAttenUpAmps) faAttenPending = 1;
    else if (faAttenIs12 && MeasuredAmps < faAttenDownAmps) faAttenPending = 0;
  }
  uint32_t t0 = (uint32_t)esp_timer_get_time();
  static uint8_t dmaBuf[FA_FRAME_BYTES];
  int16_t staged[FA_FRAME_SAMPLES];
  while ((uint32_t)esp_timer_get_time() - t0 < FA_DRAIN_BUDGET_US) {
    uint32_t outLen = 0;
    if (adc_continuous_read(faAdcHandle, dmaBuf, sizeof(dmaBuf), &outLen, 0) != ESP_OK || outLen == 0) break;
    int n = (int)(outLen / SOC_ADC_DIGI_RESULT_BYTES);
    int stagedN = 0;
    for (int i = 0; i < n; i++) {
      adc_digi_output_data_t *p = (adc_digi_output_data_t *)&dmaBuf[i * SOC_ADC_DIGI_RESULT_BYTES];
      if (p->type2.channel != FA_ADC_CHANNEL) continue;  // defensive — single-pattern config
      uint16_t raw = p->type2.data;
      if (raw >= FA_RAILED_RAW) {
        faRailedStreak++;
        faWinRailed = true;
      } else {
        faRailedStreak = 0;
      }
      int16_t mv = (int16_t)(faMvSlope * raw + faMvOff);
      staged[stagedN++] = mv;
      faProcessSample(mv);
    }
    if (stagedN > 0 && faRawRing) {
      portENTER_CRITICAL(&faRingMux);
      int idx = faRingHead;
      for (int k = 0; k < stagedN; k++) {
        faRawRing[idx] = staged[k];
        if (++idx >= FA_RAW_RING_N) idx = 0;
      }
      faRingHead = (uint16_t)idx;
      portEXIT_CRITICAL(&faRingMux);
      faTotalSamples += (uint32_t)stagedN;
    }
    // Presence: consistently railed for ~10 s while the ADS path agrees current is low
    // (a 6 dB over-range at high output also rails — that is NOT a missing jumper).
    if (faRailedStreak >= (uint32_t)FA_SAMPLE_RATE_HZ * FA_ABSENT_RAILED_SEC && fabsf(MeasuredAmps) < 50.0f) {
      adc_continuous_stop(faAdcHandle);
      faChanState = 2;
      faLastProbeMs = millis();
      faWinReset();  // clears window stats — no stale spectra on resume
      faBoxAcc = 0;
      faBoxN = 0;
      if (!faAbsentWarned) {
        faAbsentWarned = true;
        queueConsoleMessage("Fast alt-current channel reads railed full-scale -- jumper open/missing? Channel dormant, re-probing every 5 min");
      }
      return;
    }
  }
}

// Capture one flipbook page from the raw ring: most-recent 200 ms at full 20 kSPS (no
// decimation). `used` is written last so a concurrent /faflip.bin read never serves a torn page.
static void faCapturePage(int slot, uint16_t rpm, float amps, uint8_t isAnom, uint8_t patternK, float score) {
  if (!faFlip || !faRawRing || slot < 0 || slot >= FA_FLIP_SLOTS) return;
  FaFlipPage *pg = &faFlip[slot];
  pg->used = 0;
  portENTER_CRITICAL(&faRingMux);
  int idx = (int)faRingHead - FA_FLIP_NSAMP;
  while (idx < 0) idx += FA_RAW_RING_N;
  for (int i = 0; i < FA_FLIP_NSAMP; i++) {
    pg->mv[i] = faRawRing[idx];
    if (++idx >= FA_RAW_RING_N) idx = 0;
  }
  portEXIT_CRITICAL(&faRingMux);
  pg->rpm = rpm;
  pg->ampsX10 = (uint16_t)fminf(fmaxf(amps, 0.0f) * 10.0f + 0.5f, 65535.0f);
  pg->epoch = timeIsSynced ? (uint32_t)time(NULL) : 0;
  pg->isAnomaly = isAnom;
  pg->patternK = patternK;
  pg->attenIs12 = faAttenIs12;
  pg->score = score;
  pg->zeroMv = (uint16_t)FA_ZERO_MV;
  pg->apv = (uint16_t)faAmpsPerVolt();
  pg->used = 1;
  faFlipDirty = true;
}

// ── Failure detector (consumer 2) — algorithm core ──
// Faithful C++ port of the offline prototype (rect_fault_detector.py, 18/18 on the synthetic
// gate). The algorithm bodies live in 8_functions.ino; the type definitions it shares with the
// consumers below (FadJob / FadResult / FADV_*) are defined here because they are used
// by value/sizeof in this file, which precedes 8_functions.ino in the Arduino build.
// Verdicts. QUIET doubles as "no-signal" (too little periodicity/crests to analyze) —
// every consumer treats it as silence.
#define FADV_QUIET 0
#define FADV_HEALTHY 1
#define FADV_TREND 2
#define FADV_FAULT 3

struct FadResult {
  uint8_t verdict;
  uint8_t winningK;      // modulo-k fault class (5-6 diode-class, 2-4 phase-class)
  uint8_t featInterval;  // 1 = interval stream made the call, 0 = height stream
  uint8_t sync;          // gap-anchored sync mode supplied the winner
  float score;           // winning effect size D
  float marginThr;       // D / its FAULT threshold
  float marginRunner;    // D / runner-up D
  float Dheight, Dinterval, Fwin;
  float acfRatio, periodRatio;
  float pitch;     // median crest interval, samples
  float subtrain;  // wye-tap minor-train density (first-session topology signature)
  int nCrests;
};

// Analysis state + workspace pointers. The detector runs straight-line on the Core-0 worker
// (fadStep in 8_functions.ino) — no cooperative-slicing state machine; intra-stage cursors are
// plain locals. Only cross-stage results live here.
struct FadJob {
  // carved buffers (one external block; fadCarve lays them out)
  float *xf;     // input as float, n
  float *s;      // boxcar-3 smoothed; becomes rn (r/env) after the cycle-detrend
  float *rr;     // r1 (rough-detrended) then r (cycle-detrended)
  float *env;    // 2-cycle envelope
  double *pre;   // prefix sums, n+1
  float *acf;    // FAD_ACF_LAG_CAP entries, indexed lag − lagLo
  int32_t *idx;  // raw local maxima (also reused as the sync counting-sort histogram)
  int32_t *kept; // merged (then weak-filtered) crests
  int32_t *gpos, *instA, *instB, *cnts;
  float *tt, *hh, *hn, *iv, *dn, *scratch;
  float *rowsH, *rowsI;  // sync-mode phase-aligned segment rows, concatenated
  float *blkStats;       // block stats: 4 planes × 7 k × blocksCap
  int n, crestCap, blocksCap, rowsCap;
  const int16_t *src;  // NULL = xf pre-filled by caller
  float fs;

  // regime
  int lagLo, lagHi;
  float Tcycle;
  uint8_t cruise;
  float rungW[3], rungF[3];
  int nRungs;

  // stats
  double r1Mean, r1Var, rnMean, rnVar;

  // pick output
  int nIdx, nKept;
  uint8_t pickSane, idxOverflow;

  float P;
  int nCyc, envN;
  float rms;

  // per-rung analysis
  uint8_t sane2;
  float medTall, scaleH, medIv, refIv;
  int nGap;
  float subtrain;
  float Dh[9], Fh[9], Di[9], Fi[9];  // medians over blocks, indexed by k = 2..8
  int kH, kI;
  float dH, dI, runH, runI;
  uint8_t syncUsed;
  float Tsync;

  // best-rung selection (Python: lexicographic (sane, headroom) max, first wins ties)
  uint8_t haveBest, bestSane;
  float bestHeadroom;
  FadResult best;
};

static FadJob *faDetJob = NULL;   // PSRAM — analysis state machine (alloc'd once in faInit)
static uint8_t *faDetMem = NULL;  // PSRAM — ~1.9 MB carved workspace (fadCarve)

// Cross-core handoff (Core 1 control loop <-> faDetTask on Core 0), using the FreeRTOS primitives
// the rest of the firmware uses cross-core (proven on this silicon), not hand-placed fences: a
// task notification arms the worker, a binary semaphore returns the verdict. faDetBusy still gates
// faDetWin refill so Core 1 never writes the buffer while the worker reads it. faDetTaskHandle /
// faResultSem are declared in Xregulator.ino. Detail: Working Markdown Docs/Fault_Detector_Dev_Summary.md.
FadResult faSharedResult;

// Whole-analysis compute time (us) on the Core-0 worker — last run + worst since ResetPerfCounters.
uint32_t faDetLastComputeUs = 0;
uint32_t faDetWorstComputeUs = 0;

void faDetTask(void *pv) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // block until Core 1 arms a job (the notify is the barrier)
    FadResult res;
    // CPU compute time, immune to Core-0 network/WiFi preemption: ulRunTimeCounter accumulates ONLY
    // the cycles this task actually executes — time the analysis is paused while WiFi/lwIP borrows
    // the core is NOT counted. The catch is units: the run-time-stats counter on this silicon ticks
    // in CPU CLOCK CYCLES, not microseconds — divide by the live CPU MHz to get true microseconds.
    // getCpuFrequencyMhz() keeps it correct even in the 80 MHz engine-off idle (CPU is 240 MHz
    // whenever the dashboard is actually open).
    TaskStatus_t ts;
    vTaskGetInfo(NULL, &ts, pdFALSE, eInvalid);
    uint32_t c0 = (uint32_t)ts.ulRunTimeCounter;
    fadStep(faDetJob, 0, &res);  // straight-line: runs the whole analysis to completion
    vTaskGetInfo(NULL, &ts, pdFALSE, eInvalid);
    uint32_t cpuMHz = getCpuFrequencyMhz();
    uint32_t computeUs = ((uint32_t)ts.ulRunTimeCounter - c0) / (cpuMHz ? cpuMHz : 240);
    faDetLastComputeUs = computeUs;
    if (computeUs > faDetWorstComputeUs) faDetWorstComputeUs = computeUs;
    faSharedResult = res;
    xSemaphoreGive(faResultSem);  // hand the verdict back to Core 1 (give/take carry the memory barrier)
  }
}

// Verdict consumer — called from loop() every pass via TIMED_CALL(ft_faDetector, ...). The heavy
// fadStep work runs on faDetTask (Core 0); this only consumes a finished result (cheap, one
// non-blocking semaphore poll). TREND verdicts are log-only.
// Side-effects stay on Core 1 to avoid faFlip cross-core races.
void faDetectorPoll() {
  if (!faResultSem || xSemaphoreTake(faResultSem, 0) != pdTRUE) return;  // no verdict ready
  FadResult res = faSharedResult;
  faDetBusy = false;
  faDetWinN = 0;  // buffer consumed — refills window-aligned at the next faWinReset
  faDetectLastK = (res.verdict == FADV_FAULT) ? res.winningK : 0;
  if (res.verdict == FADV_FAULT) {
    faAnomalyCount++;          // item 6 — every FAULT verdict (not just rate-limited captures)
    faLastFaultMs = millis();  // item 5 — opens the audible-alarm dwell window
    if (millis() - faDetectMsgMs > 60000UL) {  // console alert, once a minute max
      faDetectMsgMs = millis();
      char m[176];
      snprintf(m, sizeof(m),
               "ALERT: alternator current pulse-pattern anomaly (class k=%u, %s pattern, %.1fx over threshold) -- check rectifier/stator",
               res.winningK, res.featInterval ? "interval" : "height", res.marginThr);
      queueConsoleMessage(m);
    }
    if (faFlip && !faAnomPause && millis() - faAnomCapMs > 300000UL) {  // anomaly capture, one per 5 min max (item 8: pause freezes the slots)
      faAnomCapMs = millis();
      // The scope ring holds "now" (~1-2 s after the analyzed window); a rectifier/stator
      // fault persists, so the page still shows the faulted waveform.
      faCapturePage(FA_FLIP_BANDS + faAnomNext, faDetCtxRpm, faDetCtxAmps, 1, res.winningK, res.score);
      faAnomNext = (faAnomNext + 1) % FA_FLIP_ANOM;
    }
  } else if (res.verdict == FADV_TREND) {
    if (millis() - faDetTrendMsgMs > 3600000UL) {  // log-only, once an hour max
      faDetTrendMsgMs = millis();
      char m[160];
      snprintf(m, sizeof(m),
               "Alternator current pulse-pattern trend (k=%u, D=%.2f) -- below the fault threshold, monitoring",
               res.winningK, res.score);
      queueConsoleMessage(m);
    }
  }
}

// Highest Tone in Map headline — peak-held at the matrix merge site (faWinFinalize) from each
// qualified window's loudest tone, reported PEAK-TO-PEAK (2 × the stored sine amplitude) so it
// matches the max−min a user eyeballs on the live waveform. It is NOT re-derived from the
// persistent map (a full-map scan would always return the historical worst and immediately undo
// a reset). Clear it ONLY here: Reset Worsts (ResetRipplePeaks) and Clear Map (FastAltClearMatrix).
void faDomReset() {
  faDomAmpAX100 = 0;
  faDomFreqHzX10 = 0;
  faDomRpm = 0;
  faDomAmpsA = 0.0f;
  faDomTempF = NAN;
  faDomEpoch = 0;
}

// Qualified-window hook — called only when a window passed the steady-state gate and
// landed in a matrix cell (the matrix merge has already happened by this point).
static void faQualifiedWindowHook(int rpmBandLo, float meanAmps) {
  // Reference flipbook: freeze-once page per 1000-RPM band, 20–100 A only. The waveform is the
  // point, so it's captured immediately regardless of clock — NEVER gated on time, or a headless
  // AP session (no client connected) would lose its references entirely. If the first capture
  // happened before any clock was available (epoch 0), allow ONE re-capture once time syncs, so
  // the permanent page ends up with a real timestamp instead of "clock not set".
  if (faFlip && meanAmps >= 20.0f && meanAmps <= 100.0f) {
    int band = rpmBandLo / 1000;
    // Defensive: band 0 must not freeze a near-zero-RPM page (engines don't idle below ~100 RPM).
    // Bounds-check band BEFORE indexing faFlip[band] for the capture-needed test.
    if (band >= 0 && band < FA_FLIP_BANDS && !(band == 0 && faRpmEma < 100.0f)
        && (!faFlip[band].used || (faFlip[band].epoch == 0 && timeIsSynced))) {
      faCapturePage(band, (uint16_t)fmaxf(faRpmEma, 0.0f), meanAmps, 0, 0, 0.0f);
      char m[96];
      int bandLo = (band == 0) ? 100 : band * 1000;          // band 0 labelled 100–1000 to match the dashboard
      int bandHi = (band == 0) ? 1000 : band * 1000 + 999;
      snprintf(m, sizeof(m), "Fast alt-current flipbook: reference waveform captured for %d-%d RPM band",
               bandLo, bandHi);
      queueConsoleMessage(m);
    }
  }
}

// Failure-detector arming — RPM-INDEPENDENT, deliberately split out of faQualifiedWindowHook
// (which is RPM-gated because the matrix and reference flipbook are binned/labelled by engine
// speed). The detection algorithm in 8_functions.ino is fully self-referenced — it reads
// neither the tach nor any RPM-derived value — so its trigger must NOT inherit the matrix's
// RPM-steadiness requirement. Several rectifier/stator faults corrupt the LM2907 RPM, and
// re-coupling the trigger to RPM would blind the detector in exactly those cases. The caller
// arms on a clean, current-steady window with real current flowing; the operating RPM bin is
// irrelevant here. A qualified window ARMS the analysis job (far too heavy for the Core-1
// control loop — it stalled it ~30 ms once a minute); the Core-0 faDetTask runs it and
// faDetectorPoll() fires the consumers on Core 1 (console alert, CSV2 faDetectK, anomaly
// flipbook capture) when the verdict lands. faDetWin holds exactly this window's raw 20 kSPS
// stream, frozen while the job runs. faDetCtxRpm is captured as page metadata only — if the
// tach is dead it records the broken value, which is itself diagnostic.
static void faMaybeArmDetector(float meanAmps) {
  if (faDetJob && faDetTaskHandle && !faDetBusy && faDetWinN == FA_DET_WIN_N
      && millis() - faDetLastStartMs >= FA_DET_MIN_PERIOD_MS) {
    faDetLastStartMs = millis();
    faDetCtxRpm = (uint16_t)fmaxf(faRpmEma, 0.0f);
    faDetCtxAmps = meanAmps;
    faDetBusy = true;       // owns faDetWin: Core-1 window refill stops (faDetFilling gate)
    faDetFilling = false;
    fadStart(faDetJob, faDetWin, FA_DET_WIN_N, (float)FA_SAMPLE_RATE_HZ);
    xTaskNotifyGive(faDetTaskHandle);  // wake the Core-0 worker (the notify publishes fadStart()'s writes)
  }
}

// Disturbance matrix → flash. Whole-blob write (~19 KB) via the shared versioned-blob
// scaffold; LittleFS, never NVS (blobs don't belong in NVS). Field-off only — caller gates.
void faMatrixFlush() {
  if (!faMatrix) return;
  uint32_t n = writePsramBlob(FA_MATRIX_PATH, FA_MATRIX_MAGIC, FA_MATRIX_VER,
                              faWindowsAccepted, faMatrix, sizeof(FaCell),
                              FA_RPM_BINS * FA_AMP_BINS, 0, FA_RPM_BINS * FA_AMP_BINS);
  if (n > 0) faMatrixDirtyWindows = 0;
}

// RPM ripple table + session stamps → flash (one ~420 B record). Written ONLY on game teardown
// (done/Abort/modal close/deadman) — the table is frozen between sweeps, so there is nothing to
// flush periodically.
void ripTabFlush() {
  writePsramBlob(RIPTAB_PATH, RIPTAB_MAGIC, RIPTAB_VER, 0, &ripTab, sizeof(RipTab), 1, 0, 1);
}

// Reference flipbook → flash (whole blob, ~18 KB) via the shared versioned-blob scaffold.
void faFlipFlush() {
  if (!faFlip) return;
  uint32_t n = writePsramBlob(FA_FLIP_PATH, FA_FLIP_MAGIC, FA_FLIP_VER,
                              faAnomNext, faFlip, sizeof(FaFlipPage),
                              FA_FLIP_SLOTS, 0, FA_FLIP_SLOTS);
  if (n > 0) faFlipDirty = false;
}

// Field-off flush gate + deferred destructive actions (Core 1). Same policy as the
// long-term ring: matrix + flipbook bank during charging, flush on the field-off settled
// rising edge and every 15 min thereafter while new data has merged.
void faMatrixMaybeFlush() {
  // RPM ripple table service (deferred from /get so wipe/save run on Core 1, same core as the
  // faFiltRippleUpdate folds — no locking needed). Wipe = game start: zero + stamp the session,
  // THEN open the fold window. Save = game teardown: persist the frozen table.
  if (ripTabPendingWipe) {
    ripTabPendingWipe = false;
    memset(&ripTab, 0, sizeof(ripTab));
    ripTab.sess.levelA = resTestTargetA;   // browser sets the level before arming the fill flag
    ripTab.sess.ibvMinV = 1e9f;
    ripTab.sess.ibvMaxV = -1e9f;
    ripTab.sess.idleRpm = ripIdleRpmStage;
    ripTab.sess.epoch = timeIsSynced ? (uint32_t)time(NULL) : 0;
    ripGameFill = true;
  }
  if (ripTabPendingSave) {
    ripTabPendingSave = false;
    ripTabFlush();
  }
  if (!faMatrix) return;
  if (faPendingMatrixClear) {  // user-clicked (arm-gated /get) — runs here on Core 1
    faPendingMatrixClear = false;
    memset(faMatrix, 0, sizeof(FaCell) * FA_RPM_BINS * FA_AMP_BINS);
    faCellsUsed = 0;
    faMatrixDirtyWindows = 0;
    fsTakeLock();
    LittleFS.remove(FA_MATRIX_PATH);
    fsReleaseLock();
    filtRippleArmed = false;  // reseed the low-pass EMAs on the next sample
    faDomReset();  // map wiped — clear the Highest Tone in Map headline too (persists at the next save)
    queueConsoleMessage("Resonance & Ripple Map cleared");
    return;
  }
  if (faPendingFlipWipeAll) {  // tach rescale — every page is labelled by an engine-RPM band that just moved
    faPendingFlipWipeAll = false;
    if (faFlip) {
      memset(faFlip, 0, sizeof(FaFlipPage) * FA_FLIP_SLOTS);  // all 9: reference bands AND anomaly captures
      faAnomNext = 0;
      faFlipDirty = true;
      faFlipFlush();
      queueConsoleMessage("Fast alt-current flipbook fully cleared (reference + anomaly) -- engine-RPM axis rescaled");
    }
    return;
  }
  if (faPendingRebaseline) {  // user-clicked (arm-gated /get) — clears the REFERENCE pages only
    faPendingRebaseline = false;
    if (faFlip) {
      memset(faFlip, 0, sizeof(FaFlipPage) * FA_FLIP_BANDS);  // slots 0..FA_FLIP_BANDS-1 = reference bands; anomaly captures kept
      faFlipDirty = true;
      faFlipFlush();  // persist now (whole blob, scoped zeros) so the cleared pages survive a reboot before the next field-off flush
      queueConsoleMessage("Fast alt-current reference pages re-baselined -- they re-capture on the next steady runs (anomaly captures kept)");
    }
    return;
  }
  static bool prevOff = false;
  static unsigned long lastFlushMs = 0;
  bool off = fieldCutSettled(0);  // hardware gate — the duty-based fieldOffSettled reads "off" during a live duty-0 CV hold
  bool rising = off && !prevOff;
  bool periodic = off && (millis() - lastFlushMs >= FA_MATRIX_FLUSH_MS);
  prevOff = off;
  if ((rising || periodic) && (faMatrixDirtyWindows > 0 || faFlipDirty)) {
    if (faMatrixDirtyWindows > 0) faMatrixFlush();
    if (faFlipDirty) faFlipFlush();
    lastFlushMs = millis();
  }
}

// Snapshot the scope ring for the /fastscope.bin endpoint. Header (24 B, little-endian):
// u32 magic 'FSC1', u16 sampleRate/10, u16 count, u16 zero-amps mV, u16 ampsPerVolt,
// u8 attenIs12, u8 chanState, u16 RPM, i16 altTempF (whole °F, INT16_MIN = no probe had read),
// u16 reserved. Then count × int16 calibrated mV, oldest-first. The capture is the trailing
// ring ending NOW, so RPM/temp/epoch are stamped at request time = capture-end time.
// Returns total bytes filled (0 = caller buffer too small).
size_t faScopeSnapshot(uint8_t *buf, size_t cap) {
  uint16_t count = (faChanState == 1 && faRawRing)
                     ? ((faTotalSamples >= FA_RAW_RING_N) ? (uint16_t)FA_RAW_RING_N : (uint16_t)faTotalSamples)
                     : 0;
  size_t need = 24 + (size_t)count * 2;
  if (!buf || cap < need) return 0;
  uint32_t magic = 0x46534331UL;  // 'FSC1'
  uint16_t rateDiv10 = FA_SAMPLE_RATE_HZ / 10;
  uint16_t zeroMv = (uint16_t)FA_ZERO_MV;
  uint16_t apv = (uint16_t)faAmpsPerVolt();
  uint16_t rpm = (uint16_t)fmaxf(0.0f, fminf(RPM, 65535.0f));
  // (int16_t)NAN is undefined behaviour and stamped garbage into the header. Same blank convention
  // as the zero-log's altTempFx10.
  int16_t altTempF = INT16_MIN;   // blank
  if (isfinite(AlternatorTemperatureF)) {   // lroundf out of int16 range is undefined; -32767 floor keeps INT16_MIN unreachable
    float t = AlternatorTemperatureF;
    altTempF = (t > 32767.0f) ? 32767 : (t < -32767.0f) ? -32767 : (int16_t)lroundf(t);
  }
  uint32_t epoch = timeIsSynced ? (uint32_t)time(NULL) : 0;
  memcpy(buf + 0, &magic, 4);
  memcpy(buf + 4, &rateDiv10, 2);
  memcpy(buf + 6, &count, 2);
  memcpy(buf + 8, &zeroMv, 2);
  memcpy(buf + 10, &apv, 2);
  buf[12] = faAttenIs12;
  buf[13] = faChanState;
  memcpy(buf + 14, &rpm, 2);
  memcpy(buf + 16, &altTempF, 2);
  memcpy(buf + 18, &epoch, 4);
  buf[22] = 0;
  buf[23] = 0;
  if (count > 0) {
    int16_t *out = (int16_t *)(buf + 24);
    portENTER_CRITICAL(&faRingMux);
    uint16_t head = faRingHead;
    if (count >= FA_RAW_RING_N) {  // full ring: oldest sample sits at head
      size_t tail = (size_t)(FA_RAW_RING_N - head);
      memcpy(out, faRawRing + head, tail * 2);
      if (head > 0) memcpy(out + tail, faRawRing, (size_t)head * 2);
    } else {  // partial: data is [0, head)
      memcpy(out, faRawRing, (size_t)count * 2);
    }
    portEXIT_CRITICAL(&faRingMux);
  }
  return need;
}

// Copy the most recent maxN raw samples (calibrated mV, oldest-first) into dst.
// Returns samples copied, 0 if the channel isn't sampling. Same mux-held copy as
// faScopeSnapshot. Consumer: the field-decay test (decay record + calibration means).
int faGrabRecent(int16_t *dst, int maxN) {
  if (faChanState != 1 || !faRawRing || !dst || maxN <= 0) return 0;
  int count = (faTotalSamples >= (uint32_t)FA_RAW_RING_N) ? FA_RAW_RING_N : (int)faTotalSamples;
  if (count > maxN) count = maxN;
  if (count <= 0) return 0;
  portENTER_CRITICAL(&faRingMux);
  uint16_t head = faRingHead;  // next write = oldest sample when the ring is full
  // newest `count` samples end just before head
  int start = (int)head - count;
  if (faTotalSamples < (uint32_t)FA_RAW_RING_N) {
    if (start < 0) start = 0;  // partial fill: data is [0, head)
    memcpy(dst, faRawRing + start, (size_t)count * 2);
  } else {
    if (start < 0) start += FA_RAW_RING_N;
    int tail = FA_RAW_RING_N - start;
    if (tail >= count) {
      memcpy(dst, faRawRing + start, (size_t)count * 2);
    } else {
      memcpy(dst, faRawRing + start, (size_t)tail * 2);
      memcpy(dst + tail, faRawRing, (size_t)(count - tail) * 2);
    }
  }
  portEXIT_CRITICAL(&faRingMux);
  return count;
}

// Boot init. PSRAM buffers + continuous driver, 6 dB default range. Any failure leaves the
// subsystem off (faChanState 0) — every consumer checks that, so a failed init is silent
// beyond one serial line.
void faInit() {
  faRawRing = (int16_t *)ps_malloc(FA_RAW_RING_N * sizeof(int16_t));
  faMatrix = (FaCell *)ps_malloc(sizeof(FaCell) * FA_RPM_BINS * FA_AMP_BINS);
  faFlip = (FaFlipPage *)ps_malloc(sizeof(FaFlipPage) * FA_FLIP_SLOTS);
  // Flat-top FFT workspace (PSRAM, ~19 KB): tone buffer + transform + window/twiddle/bitrev tables.
  faToneBuf = (float *)ps_malloc(FA_WIN_DECIM_N * sizeof(float));
  faFftRe = (float *)ps_malloc(FA_FFT_N * sizeof(float));
  faFftIm = (float *)ps_malloc(FA_FFT_N * sizeof(float));
  faFtWin = (float *)ps_malloc(FA_WIN_DECIM_N * sizeof(float));
  faTwidRe = (float *)ps_malloc((FA_FFT_N / 2) * sizeof(float));
  faTwidIm = (float *)ps_malloc((FA_FFT_N / 2) * sizeof(float));
  faBitRev = (uint16_t *)ps_malloc(FA_FFT_N * sizeof(uint16_t));
  if (!faRawRing || !faMatrix || !faFlip || !faToneBuf || !faFftRe || !faFftIm
      || !faFtWin || !faTwidRe || !faTwidIm || !faBitRev) {
    Serial.println("faInit: ps_malloc failed -- fast alt-current channel off");
    return;
  }
  memset(faRawRing, 0, FA_RAW_RING_N * sizeof(int16_t));
  memset(faMatrix, 0, sizeof(FaCell) * FA_RPM_BINS * FA_AMP_BINS);
  memset(faFlip, 0, sizeof(FaFlipPage) * FA_FLIP_SLOTS);
  // Failure-detector buffers: 2 s raw capture (80 KB) + carved analysis workspace
  // (~1.9 MB), all PSRAM. Failure leaves only the detector off — the channel, matrix,
  // scope and flipbook all keep running.
  faDetWin = (int16_t *)ps_malloc(FA_DET_WIN_N * sizeof(int16_t));
  faDetJob = (FadJob *)ps_malloc(sizeof(FadJob));
  uint8_t *detMem = NULL;
  if (faDetWin && faDetJob) {
    size_t detNeed = fadCarve(faDetJob, NULL, FA_DET_WIN_N);
    detMem = (uint8_t *)ps_malloc(detNeed);
  }
  if (faDetWin && faDetJob && detMem) {
    memset(faDetJob, 0, sizeof(FadJob));
    fadCarve(faDetJob, detMem, FA_DET_WIN_N);
    faDetMem = detMem;
  } else {
    if (faDetWin) { free(faDetWin); faDetWin = NULL; }
    if (faDetJob) { free(faDetJob); faDetJob = NULL; }
    Serial.println("faInit: detector workspace ps_malloc failed -- failure detector off");
  }
  uint32_t priorWindows = 0;
  uint32_t restored = readPsramBlob(FA_MATRIX_PATH, FA_MATRIX_MAGIC, FA_MATRIX_VER,
                                    faMatrix, sizeof(FaCell), FA_RPM_BINS * FA_AMP_BINS,
                                    &priorWindows, false);
  if (restored > 0) {
    for (int i = 0; i < FA_RPM_BINS * FA_AMP_BINS; i++)
      if (faMatrix[i].windows > 0) faCellsUsed++;
    Serial.printf("faInit: disturbance matrix restored, %u cells populated\n", (unsigned)faCellsUsed);
  }
  uint32_t anomNext32 = 0;
  if (readPsramBlob(FA_FLIP_PATH, FA_FLIP_MAGIC, FA_FLIP_VER,
                    faFlip, sizeof(FaFlipPage), FA_FLIP_SLOTS, &anomNext32, false) > 0)
    faAnomNext = (uint8_t)(anomNext32 % FA_FLIP_ANOM);
  // RPM ripple table (game-scoped, own blob): restore for the wizard pick + Protections plots.
  if (readPsramBlob(RIPTAB_PATH, RIPTAB_MAGIC, RIPTAB_VER, &ripTab, sizeof(RipTab), 1, NULL, false) > 0) {
    for (int i = 0; i < RIPTAB_BINS; i++) {
      // Pending candidates never span sessions — the agree-twice pair must come from one sweep.
      ripTab.cell[i].altPendX100 = 0;
      ripTab.cell[i].battPendX100 = 0;
      ripTab.cell[i].state &= (RIPTAB_ALT_DONE | RIPTAB_BATT_DONE);
    }
  }
  faFftInit();
  faWinReset();
  // Global ON/OFF (Pattern B, item 4). InitSystemSettings() runs AFTER faInit() in setup(),
  // so read the persisted value straight from NVS here. Buffers above stay allocated either
  // way, so a runtime OFF->ON (handled in faDrain) starts the driver without re-allocating.
  if (settingExists(NK_faEnabled)) faEnabled = (settingRead(NK_faEnabled).toInt() != 0);
  if (!faEnabled) {
    faChanState = 0;
    Serial.println("faInit: fast alt-current subsystem disabled by setting -- driver not started");
    return;
  }
  if (!faStartDriver())
    Serial.println("faInit: driver start failed -- fast alt-current channel off");
}
