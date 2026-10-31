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



// Alternator temperature (C) recorded with a Field Resistance write. 90 s, not n2kAltTempF's 20 s: the engine-off poll is
// 60 s, and a cold engine is exactly when the resistance is measured. NAN = no reading to record.
float fieldResProbeC() {
  if (IgnoreTemperature || LimpHome) return NAN;
  const uint32_t ts = dataTimestamps[(TempSource == 0) ? IDX_ALTERNATOR_TEMP : IDX_THERMISTOR_TEMP];
  if (ts == 0 || (uint32_t)(millis() - ts) > 90000UL) return NAN;
  const float f = (TempSource == 0) ? (float)AlternatorTemperatureF : ((temperatureThermistor == -99) ? NAN : (float)temperatureThermistor);
  return isfinite(f) ? (f - 32.0f) / 1.8f : NAN;
}

// 409 body for a resistance-check answer that no longer matches the unit (fieldWindingAnswerOpen); the dashboard shows it.
static const char FIELD_WIND_ANSWER_STALE[] = "Not changed: this field winding question was already answered, or the winding or Field Resistance changed after it was asked.";

// Sole writer of Field Resistance: typed entry (/get, typed=true) and /fieldResTest save=1. probeC is the temperature
// at measurement (typed) or the probe at test start; the coil-temperature credit is relative to it.
void fieldResRecord(float r, float probeC, bool typed) {
  // Held at the 3 decimals NVS stores: the winding confirmation key is built from this RAM value, and a finer one would
  // stop matching the reloaded value after a reboot and drop a confirmed 24/36/48 V winding back to 12 V values.
  FieldResistance = roundf(r * 1000.0f) / 1000.0f;
  settingWrite(NK_FieldResistance, String(FieldResistance, 3).c_str());
  FieldResTrefC = isfinite(probeC) ? probeC : 30.0f;
  settingWrite(NK_FieldResTrefC, String(FieldResTrefC, 1).c_str());
  if (FieldResDefault) {
    FieldResDefault = false;
    settingWrite(NK_FieldResDefault, "0");
  }
  g_fieldDerateDirty = true;
  stateRevision++;
  settingsDirty = true;
  if (isfinite(probeC)) queueConsoleMessageF("Field Resistance %.3f ohm recorded at %.0f%s %s", FieldResistance, dispTempF(probeC * 1.8f + 32.0f), dispTempUnit(),
                                             typed ? "as entered" : "from the alternator probe");
  else queueConsoleMessageF("Field Resistance %.3f ohm: no alternator temperature reading, %.0f%s assumed", FieldResistance, dispTempF(86.0f), dispTempUnit());
  fieldWindingReconcile("Field Resistance changed");   // a new R re-runs the resistance check on a bus-rated winding
}

// Skipping to the FD_SKIP_R_OHM default is allowed only on 12 V with no battery shunt: there the default cannot bring
// either field ceiling into play, and without a shunt the test needs a meter.
bool fieldResSkipAllowed() {
  return SYSTEM_VOLTAGE_CLASS == 12 && !HAS_BATT_SHUNT;
}

// The default stands in only while nothing is measured, and only on a 12 V class: a class change strands it, so the field
// waits for a measurement as it does with nothing recorded.
bool fieldResDefaultInUse() {
  return FieldResDefault && SYSTEM_VOLTAGE_CLASS == 12 && !(isfinite(FieldResistance) && FieldResistance > 0.0f);
}

// /fieldResTest skip=1: the owner's explicit choice on the test screen, so it may write flash. The measured pair is removed,
// so one value is in force.
void fieldResUseDefault() {
  // RAM first, default flag before the clear: the control pass on core 1 must never see neither in force, and the
  // NVS commits below take milliseconds.
  FieldResDefault = true;
  FieldResistance = NAN;
  FieldResTrefC = NAN;
  g_fieldDerateDirty = true;
  settingRemove(NK_FieldResistance);
  settingRemove(NK_FieldResTrefC);
  settingWrite(NK_FieldResDefault, "1");
  stateRevision++;
  settingsDirty = true;
  queueConsoleMessageF("Field Resistance test skipped: the regulator uses %.0f ohm (12 V system, no battery shunt). Charging and protection are unchanged; only the Field Amps reading is approximate", FD_SKIP_R_OHM);
}

// Why a field resistance test may not start now, or nullptr. Read on the web task; the control pass starts the run.
const char *frtRefuseReason(uint8_t method) {
  static char (&busyMsg)[96] = PsramPlace::make<char[96]>();
  const uint32_t now = millis();
  if (frt.state == FRT_RUNNING || frt.reqStart) return "A field resistance test is already running.";
  if (hardwarePresent != 1 || demoSeal) return "Not available in demo mode. Set Hardware Present to Yes to use it.";
  const char *busy = (systemIDActive != 0) ? "The Plant Delay test"
                     : TuningMode ? "Current tuning"
                     : CVTuningMode ? "Voltage tuning"
                     : cvPlantFitActive ? "Voltage Control Autotuning"
                     : batteryHealthTestActive ? "The battery health test"
                     : resTestActive ? "The resonance current-check"
                     : (fieldCurveActive != 0) ? "The field curve"
                     : (fieldCutActive != 0) ? "The field cut test"
                     : (protTestActive != 0) ? "A protection test"
                     : cvStressActive ? "The CV stress test"
                     : (altSweepActive != 0 || altSweepRequested) ? "The gate-tuning field sweep"
                     : (chcActive != 0 || chcRequested) ? "The charge health calibration" : nullptr;
  if (busy) {
    snprintf(busyMsg, sizeof(busyMsg), "%s is running. Wait for it to finish.", busy);
    return busyMsg;
  }
  if (IgnoreTemperature || IgnoreRPM || IgnoreAltCurrent || IgnoreBattVMain || IgnoreBattV2 || LimpHome)
    return "The test needs every sensor. Stop running without failed sensors first (Setup > System > Run Without Failed Sensors).";
  if (ManualFieldToggle == 1) return "Manual mode is on. Switch Field Control back to Auto first.";
  if (Ignition != 1) return "The ignition input is off, so the regulator will not drive the field. Turn the key on, or, with no ignition wire connected, turn on Ignition Override under Setup > Engine.";
  if (OnOff != 1) return "Alternator Enable is off. Switch it on (top right) to run the test.";
  if (bmsLogic == 1 && !((bmsLogicLevelOff == 0) ? bmsSignalActive : !bmsSignalActive)) return "The BMS on/off input is withholding permission, so the regulator will not drive the field.";
  if (RPM > 0.0f) return "The engine is turning. The test runs with the engine off.";
  if (inaOvervoltageLatched) return "The hardware over-voltage latch is set. Wait for it to clear.";
  const uint8_t cut = g_fieldEventReason;
  if ((cut == REASON_FAST_OVERVOLTAGE || cut == REASON_OV_TIER_LOW || cut == REASON_OV_TIER_MID || cut == REASON_INA_OVERVOLTAGE || cut == REASON_HARD_OVERCURRENT)
      && fieldCollapseTime > 0 && (now - fieldCollapseTime) < activeCollapseDelay)
    return "An over-voltage or over-current lockout is running. Wait a few seconds and try again.";
  const uint32_t ibvTs = dataTimestamps[IDX_IBV];
  if (ibvTs == 0 || (uint32_t)(now - ibvTs) > 3000UL) return "The main battery voltage reading is missing.";
  const uint32_t dieTs = dataTimestamps[IDX_INA_DIE_TEMP];
  if (dieTs == 0 || (uint32_t)(now - dieTs) > 10000UL) return "The regulator's drive temperature reading is missing.";
  if (method == FRT_SHUNT) {
    if (!HAS_BATT_SHUNT || (ShuntLead != 1 && ShuntLead != 2)) return "The shunt method needs the battery shunt installed and its location set.";
    const uint32_t bTs = dataTimestamps[IDX_BCUR];
    if (bTs == 0 || (uint32_t)(now - bTs) > 3000UL) return "The battery shunt reading is missing.";
  }
  return nullptr;
}

// Why a test or tool that reads engine speed may not start, or nullptr. Each start path reports it its own way.
const char *rpmTestRefuseReason() {
  if (IgnoreRPM || LimpHome)
    return "The test needs engine speed. Stop running without it first (Setup > System > Run Without Failed Sensors).";
  return nullptr;
}

// Gate for every numeric setting: the boxes are blank by design, and toInt("")/toFloat("") is 0
// Ids, tokens and text stay on plain hasParam(): a hex id like "0001e315" fails this gate.
bool paramOk(AsyncWebServerRequest *request, const char *name) {
  if (!request->hasParam(name)) return false;
  const String &v = request->getParam(name)->value();
  // strtof takes "1e3" and "0x10", but an integer setting then stores toInt() of them (1, 0)
  const bool plain = (strpbrk(v.c_str(), "eExX") == nullptr);
  if (!plain || !floatTokenValid(v.c_str())) {
    static char lastName[40] = "";
    static String lastValue;
    static uint32_t lastMs = 0;
    bool same = (strcmp(lastName, name) == 0) && (lastValue == v) && (millis() - lastMs < 2000UL);   // the same parameter is often tested twice per request
    if (!same) {
      strncpy(lastName, name, sizeof(lastName) - 1); lastName[sizeof(lastName) - 1] = '\0';
      lastValue = v; lastMs = millis();
      queueConsoleMessageF("WRITE REJECTED: %s=\"%s\" is not a %s number", name, v.c_str(), plain ? "finite" : "plain");
    }
    return false;
  }
  return true;
}

enum Csv1Index {
  // LiveStream: fast-changing sensor readings sent at webgaugesinterval
  CSV1_AlternatorTemperatureF,
  CSV1_dutyCycle,
  CSV1_BatteryV,
  CSV1_MeasuredAmps,
  CSV1_RPM,
  CSV1_Channel3V,
  CSV1_IBV,
  CSV1_Bcur,
  CSV1_VictronVoltage,
  CSV1_LoopTime,
  CSV1_WifiHeartBeat,
  CSV1_vvout,
  CSV1_iiout,
  CSV1_FreeHeap,
  CSV1_Alarm_Status,
  CSV1_fieldActiveStatus,
  CSV1_currentMode,
  CSV1_stateRevision,
  CSV1_setpointLimited,
  CSV1_uTargetAmps,
  CSV1_pidInput,
  CSV1_pidOutput,
  CSV1_pidError,
  CSV1_imu_heel_deg,
  CSV1_imu_pitch_deg,
  CSV1_imu_vertical_accel_g,
  CSV1_imu_yaw_rate_dps,
  CSV1_imu_total_accel_g,
  CSV1_perfCountersResetElapsedS,  // s since Reset Peak Values press (or boot)
  CSV1_shutdownPhase,
  CSV1_BatteryV_raw,
  CSV1_pidAltPV,  // CC current-PID process variable — the OutputPIDSigSrc-selected signal (EMA/MA/raw)
  CSV1_voltageTarget,
  CSV1_Icv,
  CSV1_WaterDepth_ft,  // NMEA2k depth ×3.28084 (meters → feet), scaled ×10 (0.1 ft); 0 if stale
  CSV1_Ignition,       // effective ignition (GPIO1 wire OR force-on override); on CSV1 so the banner tracks it at ~10 Hz
  CSV1_OnOff,          // master Alternator Enable; on CSV1 (other half of chargingEnabled) so the header switch and field-status word come off the same frame
  CSV1_mExcessEma,       // iExcess detector: averaged signed current excess over command (A ×10) — tuning trace
  CSV1_iExcessThreshold, // iExcess detector: computed fire threshold E (A ×10) — tuning trace
  CSV1_mExcessEmaPeak,   // iExcess: per-CSV1-frame peak averaged excess (A ×10) — live sparkline
  CSV1_iExcessThreshMin, // iExcess: per-CSV1-frame min fire threshold E (A ×10) — live sparkline
  CSV1_protEventMask,    // event bitmask this frame (1=OV 2=iExcess 4=LoadDump — vertical markers; 8=CV D term — Voltage-plot shading, NOT a protection)
  CSV1_fieldEventReason, // FieldEventReason enum code — plain-English cause the banner shows next to OFF
  CSV1_cvPTerm,          // CV loop P contribution to Icv (A ×100) — VoltageKp_active × error; P/I/D tuning plot
  CSV1_cvIterm,          // CV loop I contribution to Icv (A ×100) — cv_I integrator, live (also on CSV2 at 5 s); P/I/D plot
  CSV1_cvKdTrim,         // CV loop D back-off applied at the Icv output (A ×100); plotted negated as the D contribution
  CSV1_cvKdFiltV,        // IBV smoothed by CvKdVoltFiltTC (V ×100) — the D term's slope input; "Voltage for D term" trace
  CSV1_huntDerate,       // hunt-governor live Ki derate (×100; 100 = full gain)
  CSV1_huntFreqHz,       // hunt-governor last confirmed wobble frequency (Hz ×100; 0 = none this session); on CSV1 so it never lags huntDerate
  CSV1_huntState,        // hunt-governor state: 0 watching (pocket map), 1 testing a Ki cut, 3 cooldown after a failed test, 4 testing with D-term paused (2 unused)
  CSV1_rpmCeilingAmps,   // RPM-table current ceiling this tick (A ×100) — the header Limit panel's "current limit at this RPM"

  CSV1_sessionId,        // boot identity — same value in every channel this boot; proves a cached block is from this run
  CSV1_sendMs,           // millis() when this payload was built; a consumer ages every other channel against this one

  CSV1_FIELD_COUNT  // = 53
};

enum Csv2Index {
  // DiagStream: slower-changing telemetry, diagnostics, computed values — sent every 5s
  CSV2_IBVMax,
  CSV2_MeasuredAmpsMax,
  CSV2_RPMMax,
  CSV2_SOC_percent,
  CSV2_EngineRunTime,
  CSV2_AlternatorOnTime,
  CSV2_AlternatorFuelUsed,
  CSV2_ChargedEnergy,
  CSV2_DischargedEnergy,
  CSV2_AlternatorChargedEnergy,
  CSV2_MaxAlternatorTemperatureF,
  CSV2_temperatureThermistor,
  CSV2_MaxTemperatureThermistor,
  CSV2_timeToFullChargeMin,
  CSV2_timeToFullDischargeMin,
  CSV2_LatitudeNMEA,
  CSV2_LongitudeNMEA,
  CSV2_SatelliteCountNMEA,
  CSV2_LastSessionDuration,
  CSV2_LastSessionMaxLoopTime,
  CSV2_lastSessionMinHeap,
  CSV2_wifiReconnectsTotal,
  CSV2_LastResetReason,
  CSV2_ancientResetReason,
  CSV2_totalPowerCycles,
  CSV2_MinFreeHeap,
  CSV2_currentWeatherMode,
  CSV2_UVToday,
  CSV2_UVTomorrow,
  CSV2_UVDay2,
  CSV2_weatherDataValid,
  CSV2_AlarmLatchState,
  CSV2_ResetAlarmLatch,
  CSV2_DynamicAltCurrentZero,
  CSV2_InsulationLifePercent,
  CSV2_GreaseLifePercent,
  CSV2_BrushLifePercent,
  CSV2_PredictedLifeHours,
  CSV2_LifeIndicatorColor,
  CSV2_pKwHrToday,
  CSV2_pKwHrTomorrow,
  CSV2_pKwHr2days,
  CSV2_ambientTemp,
  CSV2_baroPressure,
  CSV2_firmwareVersionInt,
  CSV2_deviceIdUpper,
  CSV2_deviceIdLower,
  CSV2_ChargedEnergy_AllTime,
  CSV2_AlternatorFuelUsed_AllTime,
  CSV2_PeakVoltage_AllTime,
  CSV2_EngineRunTime_AllTime,
  CSV2_MinVoltage,
  CSV2_MinVoltage_AllTime,
  CSV2_ChargeCycles,
  CSV2_ChargeCycles_AllTime,
  CSV2_EngineFuelUsed,
  CSV2_EngineFuelUsed_AllTime,
  CSV2_TotalDistance,
  CSV2_TotalDistance_AllTime,
  CSV2_MaxSpeed,
  CSV2_MaxSpeed_AllTime,
  CSV2_SolarChargedEnergy,
  CSV2_SolarChargedEnergy_AllTime,
  CSV2_AlternatorChargedEnergy_AllTime,
  CSV2_DischargedEnergy_AllTime,
  CSV2_AvgSOC_AllTime,
  CSV2_AvgSpeed_AllTime,
  CSV2_AvgSpeed,
  CSV2_AlternatorOnTime_AllTime,
  CSV2_EngineCycles_AllTime,
  CSV2_MaxAlternatorTemperatureF_AllTime,
  CSV2_MaxTemperatureThermistor_AllTime,
  CSV2_MeasuredAmpsMax_AllTime,
  CSV2_RPMMax_AllTime,
  CSV2_BulkStage,
  CSV2_WifiWakeSecondsRemaining,
  CSV2_BufferedRecordCount,
  CSV2_BufferedRecordPercent,
  CSV2_BufferedRecordCap,
  CSV2_VMGTargetBearing,
  CSV2_cpuLoadCore0,
  CSV2_cpuLoadCore0Max,
  CSV2_cpuLoadCore1,
  CSV2_cpuLoadCore1Max,
  CSV2_hasForcedUpdate,
  CSV2_forcedFwVersionInt,
  CSV2_forcedUpdateDeadline,
  CSV2_stateRevision,
  CSV2_imu_accel_x_raw,
  CSV2_imu_accel_y_raw,
  CSV2_imu_accel_z_raw,
  CSV2_imu_gyro_x_raw,
  CSV2_imu_gyro_y_raw,
  CSV2_imu_gyro_z_raw,
  CSV2_accel_x_min,
  CSV2_accel_x_max,
  CSV2_accel_x_avg,
  CSV2_accel_y_min,
  CSV2_accel_y_max,
  CSV2_accel_y_avg,
  CSV2_accel_z_min,
  CSV2_accel_z_max,
  CSV2_accel_z_avg,
  CSV2_gyro_x_min,
  CSV2_gyro_x_max,
  CSV2_gyro_x_avg,
  CSV2_gyro_y_min,
  CSV2_gyro_y_max,
  CSV2_gyro_y_avg,
  CSV2_gyro_z_min,
  CSV2_gyro_z_max,
  CSV2_gyro_z_avg,
  CSV2_heel_min,
  CSV2_heel_max,
  CSV2_heel_avg,
  CSV2_pitch_min,
  CSV2_pitch_max,
  CSV2_pitch_avg,
  CSV2_vertical_accel_min,
  CSV2_vertical_accel_max,
  CSV2_vertical_accel_avg,
  CSV2_total_accel_min,
  CSV2_total_accel_max,
  CSV2_total_accel_avg,
  CSV2_imu_slam_count,
  CSV2_imu_slam_peak_max,
  CSV2_imu_slam_count_lifetime,
  CSV2_imu_capsize_count,
  CSV2_imu_pitchpole_count,
  CSV2_imu_heel_change_60s,
  CSV2_imu_heel_deviation_60s,
  CSV2_imu_pitch_change_60s,
  CSV2_imu_pitch_deviation_60s,
  CSV2_imu_wave_period_sec,
  CSV2_imu_heel_max_lifetime,
  CSV2_imu_pitch_max_lifetime,
  CSV2_imu_slam_peak_lifetime,
  CSV2_imu_fifo_overrun_count,
  CSV2_imu_i2c_error_count,
  CSV2_imu_unknown_tag_count,
  CSV2_imu_accel_dropped,
  CSV2_imu_gyro_dropped,
  CSV2_imu_total_samples_accel,
  CSV2_imu_total_samples_gyro,
  CSV2_IMUReadTime2,
  CSV2_IMUReadTime,
  CSV2_adsI2CErrorCount,
  CSV2_tempPIDActive,
  CSV2_tempPIDInput_d,
  CSV2_tempPIDSetpoint_d,
  CSV2_thermalPenaltyAmps,
  CSV2_innerTermP,
  CSV2_innerTermI,
  CSV2_innerTermD,
  CSV2_outerTermP,
  CSV2_outerTermI,
  CSV2_outerTermLookahead,  // look-ahead share of outerTermP (A ×100)
  CSV2_thermalSlopeFPerSec,
  CSV2_chargeStageDisplay,
  CSV2_voltageControlActive,
  CSV2_voltageError,
  CSV2_cv_I,
  CSV2_inIdleStage,
  CSV2_altBaselineFrozen,    // 1 = a cloud-fitted curve is held (else awaiting first fit)
  CSV2_ft_rai_total_win,
  CSV2_ft_rai_total_ses,
  CSV2_ft_rai_ina228_win,
  CSV2_ft_rai_ina228_ses,
  CSV2_ft_rai_ads_state_win,
  CSV2_ft_rai_ads_state_ses,
  CSV2_ft_rai_bmp_state_win,
  CSV2_ft_rai_bmp_state_ses,
  CSV2_ft_rai_imu_win,
  CSV2_ft_rai_imu_ses,
  CSV2_tempReadFailCount,
  CSV2_tempCrcFailCount,
  CSV2_tempCrcRecoveredCount,
  CSV2_tempAllFFCount,
  CSV2_tempPowerOn85Count,
  CSV2_tempOutOfRangeCount,
  CSV2_tempRequestFailCount,
  CSV2_tempConnectedFailCount,
  CSV2_tempResolutionFixCount,
  CSV2_tempRereadFailCount,
  CSV2_tempResolutionFixCrcFailCount,
  CSV2_tempEnumerateFailCount,
  CSV2_warmupCeiling,
  CSV2_imu_min_moving_gentle,
  CSV2_imu_min_moving_moderate,
  CSV2_imu_min_moving_rough,
  CSV2_imu_min_moving_extreme,
  CSV2_imu_min_stat_gentle,
  CSV2_imu_min_stat_moderate,
  CSV2_imu_min_stat_rough,
  CSV2_imu_min_stat_extreme,
  CSV2_imu_heel_deviation_120s,
  CSV2_imu_pitch_deviation_120s,
  CSV2_imu_heading_swing_120s,
  CSV2_dBcur_dt,
  CSV2_loadDumpActive,
  CSV2_ft_updateAccelMetrics_win,
  CSV2_ft_updateAccelMetrics_ses,
  // Diagnostics block
  CSV2_WifiStrength,
  CSV2_SendWifiTime,
  CSV2_AnalogReadTime,
  CSV2_VeTime,
  CSV2_MaximumLoopTime,
  CSV2_EngineCycles,
  CSV2_CurrentSessionDuration,
  CSV2_currentPartitionType,
  CSV2_fastOvCurrentCap,
  CSV2_fastOvClampCount,
  CSV2_fastOvHardCount,
  CSV2_ch1_last_ms,
  CSV2_ch1_avg_10s,
  CSV2_ch1_worst_10s,
  CSV2_ch1_over2x_10s,
  CSV2_ch1_n_10s,
  CSV2_ch1_avg_2m,
  CSV2_ch1_worst_2m,
  CSV2_ch1_over2x_2m,
  CSV2_ch1_n_2m,
  CSV2_ch1_avg_at,
  CSV2_ch1_worst_at,
  CSV2_ch1_over2x_at,
  CSV2_ch1_n_at,
  CSV2_iExcessCount,
  CSV2_inaOVCount,
  CSV2_hardOCCount,
  CSV2_voltSpikeCount,
  CSV2_voltDisagreeCritCount,
  CSV2_voltDisagreeWarnCount,
  CSV2_voltImplausibleCount,
  CSV2_tempCritCount,
  CSV2_tempSustainedCount,
  CSV2_tempStaleCount,
  CSV2_currentStaleCount,
  CSV2_imu_msi_score,
  CSV2_imu_vomit_pct,
  CSV2_imu_anchorage_comfort,
  CSV2_ina_last_ms,
  CSV2_ina_avg_10s,
  CSV2_ina_worst_10s,
  CSV2_ina_over2x_10s,
  CSV2_ina_avg_2m,
  CSV2_ina_worst_2m,
  CSV2_ina_over2x_2m,
  CSV2_ina_avg_at,
  CSV2_ina_worst_at,
  CSV2_ina_over2x_at,
  CSV2_loopTime5sWindow_ms,
  CSV2_MaximumLoopTime_ms,
  CSV2_ft_SendWifiData_win,
  CSV2_ft_SendWifiData_ses,
  CSV2_ft_CheckAlarms_win,
  CSV2_ft_CheckAlarms_ses,
  CSV2_ft_calculateDerivedMetrics_win,
  CSV2_ft_calculateDerivedMetrics_ses,
  CSV2_ft_logDashboardValues_win,
  CSV2_ft_logDashboardValues_ses,
  CSV2_ft_updateSystemHealthStats_win,
  CSV2_ft_updateSystemHealthStats_ses,
  CSV2_ft_checkWiFiConnection_win,
  CSV2_ft_checkWiFiConnection_ses,
  CSV2_ft_ch1_compute_stats_win,
  CSV2_ft_ch1_compute_stats_ses,
  CSV2_ft_UpdateBatterySOC_win,
  CSV2_ft_UpdateBatterySOC_ses,
  CSV2_ft_updateSensorWindow_win,
  CSV2_ft_updateSensorWindow_ses,
  CSV2_ft_checkTimeSync_win,
  CSV2_ft_checkTimeSync_ses,
  // Field-off flash/NVS-flush timers (loop-direct; see ft_dumpLongTermRing rationale)
  CSV2_ft_zeroLogService_win,
  CSV2_ft_zeroLogService_ses,
  CSV2_ft_bhFlushCapNVS_win,
  CSV2_ft_bhFlushCapNVS_ses,
  CSV2_ft_kneeLearnService_win,
  CSV2_ft_kneeLearnService_ses,
  // Firmware-computed values block
  CSV2_currentRPMTableIndex,
  CSV2_pidInitialized,
  CSV2_pidSetpoint,
  CSV2_TempToUse,
  CSV2_learningTargetFromRPM,
  CSV2_finalLearningTarget,
  CSV2_overheatingPenaltyTimer,
  CSV2_overheatingPenaltyAmps,
  CSV2_timeSinceLastOverheat,
  CSV2_socInfoAvailable,
  CSV2_overheatCount0,
  CSV2_overheatCount1,
  CSV2_overheatCount2,
  CSV2_overheatCount3,
  CSV2_overheatCount4,
  CSV2_overheatCount5,
  CSV2_overheatCount6,
  CSV2_overheatCount7,
  CSV2_overheatCount8,
  CSV2_overheatCount9,
  CSV2_cumulativeNoOverheatTime0,
  CSV2_cumulativeNoOverheatTime1,
  CSV2_cumulativeNoOverheatTime2,
  CSV2_cumulativeNoOverheatTime3,
  CSV2_cumulativeNoOverheatTime4,
  CSV2_cumulativeNoOverheatTime5,
  CSV2_cumulativeNoOverheatTime6,
  CSV2_cumulativeNoOverheatTime7,
  CSV2_cumulativeNoOverheatTime8,
  CSV2_cumulativeNoOverheatTime9,
  CSV2_learningUpCount0,
  CSV2_learningUpCount1,
  CSV2_learningUpCount2,
  CSV2_learningUpCount3,
  CSV2_learningUpCount4,
  CSV2_learningUpCount5,
  CSV2_learningUpCount6,
  CSV2_learningUpCount7,
  CSV2_learningUpCount8,
  CSV2_learningUpCount9,
  CSV2_totalLearningEvents,
  CSV2_totalOverheats,
  CSV2_totalSafeHours,
  CSV2_FreeInternalRam,
  CSV2_TotalInternalRam,
  CSV2_LargestInternalBlock,   // KB (cpuLoadTask walk every 20 s, field cut only; held while charging)
  CSV2_FreePSRAM,
  CSV2_TotalPSRAM,
  CSV2_Heapfrag,               // % from the same walk, so held while charging too
  CSV2_ft_ReadAnalogInputs_win,
  CSV2_ft_ReadAnalogInputs_ses,
  CSV2_ft_AdjustFieldLearnMode_win,
  CSV2_ft_AdjustFieldLearnMode_ses,
  CSV2_ft_uploadSensorHistory_win,
  CSV2_ft_uploadSensorHistory_ses,
  CSV2_ft_uploadBufferedRecords_win,
  CSV2_ft_uploadBufferedRecords_ses,
  CSV2_ft_buildConfigPayload_win,
  CSV2_ft_buildConfigPayload_ses,
  CSV2_VeTime2,
  CSV2_systemIDRiseDelay_0,
  CSV2_systemIDRiseDelay_1,
  CSV2_systemIDRiseDelay_2,
  CSV2_systemIDFallDelay_0,
  CSV2_systemIDFallDelay_1,
  CSV2_systemIDFallDelay_2,
  CSV2_systemIDRiseAvg,
  CSV2_systemIDFallAvg,
  CSV2_ft_altHealth_win,
  CSV2_ft_altHealth_ses,
  CSV2_ft_altFold_win,
  CSV2_ft_altFold_ses,
  CSV2_ft_boatPerf_win,
  CSV2_ft_boatPerf_ses,
  CSV2_ft_huntGov_win,
  CSV2_ft_huntGov_ses,
  CSV2_systemIDActive,
  CSV2_systemIDResultsReady,
  CSV2_systemIDStepAmp_0,
  CSV2_systemIDStepAmp_1,
  CSV2_systemIDStepAmp_2,
  CSV2_systemIDQuietPP_0,
  CSV2_systemIDQuietPP_1,
  CSV2_systemIDQuietPP_2,
  CSV2_systemIDAbortReason,       // FieldEventReason code if protection aborted last test; 0=no abort
  CSV2_systemIDAbortPhase,        // phase 1-9 at moment of protection abort; 0=no abort
  // CV voltage-loop firing-interval ladder (vl_*), CH1/pf-style stats
  CSV2_vl_last_ms,
  CSV2_vl_avg_10s,
  CSV2_vl_worst_10s,
  CSV2_vl_over2x_10s,
  CSV2_vl_avg_2m,
  CSV2_vl_worst_2m,
  CSV2_vl_over2x_2m,
  CSV2_vl_avg_at,
  CSV2_vl_worst_at,
  CSV2_vl_over2x_at,
  // NVS full-save diagnostics — saveNVSDataFull() fires only at field-off edge / shutdown / capsize.
  CSV2_nvsSecsSinceLastSave,      // seconds since last successful saveNVSDataFull() (0 = never saved this boot)
  CSV2_nvsFullSaveLastMs,         // wall-clock duration of most recent saveNVSDataFull() (ms)
  CSV2_nvsFullSaveWorstMs,        // worst saveNVSDataFull() duration since boot (ms)
  CSV2_nvsFullSaveCount,          // total saveNVSDataFull() calls since boot

  // Ignition-cycle watermarks (lo + hi pairs, reset every boot). See wmIgn_* globals in Xregulator.ino.
  CSV2_wmIgn_amps_lo,     CSV2_wmIgn_amps_hi,      // MeasuredAmps (A, int)
  CSV2_wmIgn_altTempF_lo, CSV2_wmIgn_altTempF_hi,  // AlternatorTemperatureF (°F, int)
  CSV2_wmIgn_IBV_lo,      CSV2_wmIgn_IBV_hi,       // INA228 battery V (×10, 1 decimal)
  CSV2_wmIgn_Bcur_lo,     CSV2_wmIgn_Bcur_hi,      // INA228 battery A (int)
  CSV2_wmIgn_SOC_lo,      CSV2_wmIgn_SOC_hi,       // SOC percent (0..100, int)
  CSV2_wmIgn_RPM_lo,      CSV2_wmIgn_RPM_hi,       // Engine RPM (int)
  CSV2_wmIgn_SOG_lo,      CSV2_wmIgn_SOG_hi,       // SOGNMEA knots (int)
  CSV2_wmIgn_AWS_lo,      CSV2_wmIgn_AWS_hi,       // ApparentWindSpeedNMEA knots (int)
  CSV2_wmIgn_TWS_lo,      CSV2_wmIgn_TWS_hi,       // TrueWindSpeedNMEA knots (int)
  CSV2_wmIgn_heel_lo,     CSV2_wmIgn_heel_hi,      // imu_heel_deg (int)
  CSV2_wmIgn_pitch_lo,    CSV2_wmIgn_pitch_hi,     // imu_pitch_deg (int)
  CSV2_wmIgn_vacc_lo,     CSV2_wmIgn_vacc_hi,      // imu_vertical_accel_g (×10, 1 decimal)
  CSV2_wmIgn_baro_lo,     CSV2_wmIgn_baro_hi,      // baroPressure mbar (int)
  CSV2_wmIgn_ambient_lo,  CSV2_wmIgn_ambient_hi,   // ambientTemp °F (int)
  CSV2_restartRemainingSec,                        // seconds until scheduled reboot (0 = outside 10-min warning window)
  CSV2_currentGpsSource,                           // 0=none, 1=NMEA, 2=Phone, 3=Manual (GpsSource enum)
  CSV2_currentTimeSource,                          // 0=none, 1=GPS, 2=Phone, 3=NTP, 4=drifting (TimeSource enum)
  CSV2_currentSpeedSource,                         // 0=none, 1=NMEA, 2=Phone (GpsSource enum; speed/course can differ from position source)
  CSV2_loggingActive,   // 1 if logging active, 0 if stopped (Stop/Start Logs)
  CSV2_sustainedTWS,                               // 2-min sustained true wind, knots ×10 (Beaufort + gale basis)
  CSV2_currentGaleMinutes,                         // live minutes continuously in a gale (sustained ≥34kt), int
  CSV2_wmIgn_VMGman_lo,   CSV2_wmIgn_VMGman_hi,    // VMG manual session min/max (knots ×10)
  CSV2_wmIgn_VMGup_lo,    CSV2_wmIgn_VMGup_hi,     // VMG upwind session min/max (knots ×10)

  // Alternator (charging-system) health summary
  CSV2_altHealthPct,        // worst-region performance % ×10
  CSV2_altHealthStatus,     // 0 insufficient/awaiting fit, 1 healthy, 2 drifting
  CSV2_altCoveragePct,      // record-book fill % ×10
  CSV2_altObsCount,         // banked best-ever record count

  // Victron VE.Direct solar/MPPT live block (PPV/VPV/derived current are on CSV4/NavStream)
  CSV2_VictronChargeState,       // CS code (×1)
  CSV2_VictronMPPTMode,          // MPPT tracker code (×1)
  CSV2_VictronError,             // ERR code (×1)
  CSV2_VictronYieldToday,        // H20 yield today (kWh ×100)
  CSV2_VictronMaxPowerToday,     // H21 max power today (W ×1)
  CSV2_VictronYieldYesterday,    // H22 yield yesterday (kWh ×100)
  CSV2_VictronMaxPowerYesterday, // H23 max power yesterday (W ×1)

  // Session fuel-economy curve: mpg per 250-RPM bin (18 bins, 0..4500), naut mi/gal ×100, 0 = empty
  CSV2_fuelCurveNMPG_0,  CSV2_fuelCurveNMPG_1,  CSV2_fuelCurveNMPG_2,
  CSV2_fuelCurveNMPG_3,  CSV2_fuelCurveNMPG_4,  CSV2_fuelCurveNMPG_5,
  CSV2_fuelCurveNMPG_6,  CSV2_fuelCurveNMPG_7,  CSV2_fuelCurveNMPG_8,
  CSV2_fuelCurveNMPG_9,  CSV2_fuelCurveNMPG_10, CSV2_fuelCurveNMPG_11,
  CSV2_fuelCurveNMPG_12, CSV2_fuelCurveNMPG_13, CSV2_fuelCurveNMPG_14,
  CSV2_fuelCurveNMPG_15, CSV2_fuelCurveNMPG_16, CSV2_fuelCurveNMPG_17,
  CSV2_fuelCurveTopRPM,          // top configured fuel-table RPM -> chart x-axis scale (×1)

  // 80MHz low-power loop instrumentation (4 fields) — health while engine off / CPU throttled
  CSV2_loopWorst80Win_ms,        // worst 80MHz loop pass, rolling 5s (ms ×1)
  CSV2_loopWorst80Ses_ms,        // worst 80MHz loop pass since Reset Peak Values (ms ×1)
  CSV2_loopOver80ImuLimitCount,  // # 80MHz passes over accel FIFO drain limit (~38ms) since reset
  CSV2_loop80IterCount,          // total 80MHz passes since reset (denominator)
  // Field-ON loop instrumentation (2 fields) — worst pass while actually regulating (gate latched
  // at top of pass); splits control-path stalls from intentional field-off background work
  CSV2_loopFieldOnWin_ms,        // worst field-ON loop pass, rolling 5s (ms ×1)
  CSV2_loopFieldOnSes_ms,        // worst field-ON loop pass since Reset Peak Values (ms ×1)
  // Thermal tuning plot live-stream fields
  CSV2_tempFiltered,             // IIR-filtered alt temp (°F ×100); distinct from raw AlternatorTemperatureF, used as PID base
  CSV2_outerImpliedPenalty,      // voltage cap expressed as a downstream amps penalty (A ×100); Plot 2 "Implied Penalty"
  CSV2_thermalFlags,             // state-strip bitfield: bit0 tempPIDActive, bit4 AUTO, bit5 shutdown
  CSV2_thermalAntiWindupLatch,   // 1 = CV-bleed anti-windup fired since last CSV2 send (latched; JS draws red ticks)

  // Inner Current PID firing interval (field-on-gated), CH1-style stats (avg ×100)
  CSV2_pf_last_ms,
  CSV2_pf_avg_10s,
  CSV2_pf_worst_10s,
  CSV2_pf_over2x_10s,
  CSV2_pf_avg_2m,
  CSV2_pf_worst_2m,
  CSV2_pf_over2x_2m,
  CSV2_pf_avg_at,
  CSV2_pf_worst_at,
  CSV2_pf_over2x_at,

  // I2C bus-health — bus-only timing isolates a true bus stall from loop preemption
  CSV2_inaBusReadWorstUs,    // worst µs in the two INA228 Wire reads (vs whole-block ft_rai_ina228)
  CSV2_inaBusSlowCount,      // INA228 bus reads > 15 ms since reset
  CSV2_ina228ErrorCount,     // INA228 reads dropped (sanity fail / exception)
  CSV2_imuFifoFetchWorstUs,  // worst µs in Get_FIFO_Sample
  CSV2_imuFifoWorstSamples,  // sample count of that worst fetch — small count + big µs = stall/preemption, not transfer size

  // Long-term-ring flash-flush timer (field-off 15-min dump)
  CSV2_dumpLongTermRing_win,  // worst µs of the flush, rolling 5s window
  CSV2_dumpLongTermRing_ses,  // worst µs of the flush since last Reset Peak Values

  // Fast alternator-current channel (GPIO3) — timers, status, detector, session worsts
  CSV2_fastAltDrain_win,    // worst µs of the bounded DMA drain, rolling 5s window
  CSV2_fastAltDrain_ses,    // ...since last Reset Peak Values
  CSV2_faMatrixFlush_win,   // worst µs of the disturbance-matrix/flipbook flash flush, rolling 5s window
  CSV2_faMatrixFlush_ses,   // ...since last Reset Peak Values
  CSV2_faDetector_win,      // detector whole-analysis compute on the Core-0 worker — LAST run (µs); JS /1000 -> ms
  CSV2_faDetector_ses,      // detector whole-analysis compute — WORST since last Reset Peak Values (µs)
  CSV2_faWindowFinalize_win, // worst µs of one per-2s-window finalize (Goertzel/matrix-fold/detector-arm), rolling 5s
  CSV2_faWindowFinalize_ses, // ...since last Reset Peak Values
  CSV2_faChanState,         // 0 = off, 1 = sampling, 2 = railed/dormant (jumper open)
  CSV2_faCellsUsed,         // disturbance-matrix cells with ≥1 qualified window
  CSV2_faDetectK,           // failure detector: fault class of the last FAULT verdict, 0 = quiet
  CSV2_faSesPkpkWorst,      // session-worst broadband pk-pk, A ×100
  CSV2_faSesPeakWorst,      // session-worst spectral peak, A ×100
  CSV2_faSesPeakWorstHz,    // ...its frequency, Hz ×10
  CSV2_faAnomalyCount,      // lifetime detector FAULT-verdict count (persisted fleet scalar)
  CSV2_faDomFreqHz,         // Highest Tone in Map: frequency, Hz ×10
  CSV2_faDomAmp,            // ...amplitude, A ×100
  CSV2_faDomRpm,            // ...RPM (bin center) where it occurs
  CSV2_faDomAmps,           // ...window-mean output current there, A ×10
  CSV2_faDomTempF,          // ...alternator temperature there, °F ×10 (-1 = no probe)
  CSV2_faDomEpoch,          // ...wall-clock epoch when set (0 = clock not synced)
  CSV2_faSesPkpkRpm,        // Session Worst Pk-Pk: RPM (bin center) where it occurred
  CSV2_faSesPkpkAmps,       // ...window-mean output current there, A ×10
  CSV2_faSesPkpkTempF,      // ...alternator temperature there, °F ×10 (-1 = no probe)
  CSV2_faSesPkpkEpoch,      // ...wall-clock epoch when set (0 = clock not synced)

  // gate-tuning 10s live readouts (firmware Roll10s extreme; ROLL_EMPTY sentinel when no sample in window)
  CSV2_faRpmEdge10sMin,     // RPM edge margin, 10s trough (RPM ×10)
  CSV2_faAmpsDrift10sMax,   // amps-drift EMA spread, 10s peak (A ×100)
  CSV2_faAmpsDriftExc10sMax,// drift spread minus its effective gate limit, 10s peak (A ×100); <=0 = drift gate passing
  CSV2_faTonePk10sMax,      // largest spectral peak, 10s peak (A ×100)
  CSV2_ldSlew10sMax,        // current slew g_dBcur_dt, 10s peak (A/s ×10)
  CSV2_cvSlope10sMax,       // voltage rise cvDSlope, 10s peak (V/s ×10000)
  // ripple-capture (faFiltRippleUpdate §11 stationarity) admission gates — window quantity minus its limit, 10s peak; <=0 = that gate passing
  CSV2_ripCmdExc10sMax,     // setpointLimited travel − amplitude limit (A ×100)
  CSV2_ripAltExc10sMax,     // alternator half-window mean-shift − stationarity limit (A ×100)
  CSV2_ripBattExc10sMax,    // battery half-window mean-shift − stationarity limit (A ×100)
  CSV2_ripRpmShift10sMax,   // RPM half-window mean-shift − stationarity limit, 10s peak (RPM ×10); <=0 = passing
  CSV2_ripAltAdmitCount,    // windows that passed ALL alt-fold gates (lifetime; UI diffs frames for rate)
  CSV2_ripBattAdmitCount,   // same for the battery detector

  // Lifetime nav/sailing records (NVS-persisted, uploaded to the leaderboards)
  CSV2_LongestTripAT,       // longest single trip, nm ×10
  CSV2_Max24hrDistAT,       // max 24-hour distance, nm ×10
  CSV2_DeepestAnchorAT,     // deepest anchorage, ft ×10
  CSV2_BestUpwindVmgAT,     // best upwind VMG, kts ×100
  CSV2_LongestGaleAT,       // longest gale duration, hours ×100

  CSV2_cpuFreqMhz,   // live CPU clock (80 or 240 MHz) — 80 = engine-off low-power throttle, 240 = full speed

  // ADS slow-channel inter-sample gap meters (ms). Single-shot round-robin: CH0/CH2 each get one slot in six,
  // ~40 ms apart (~80 ms worst field-on).
  CSV2_ch0GapLast,   // CH0 battery-voltage read gap, most recent (ms)
  CSV2_ch0GapWorst,  // CH0 battery-voltage read gap, worst since reset (ms)
  CSV2_ch2GapLast,   // CH2 RPM read gap, most recent (ms)
  CSV2_ch2GapWorst,  // CH2 RPM read gap, worst since reset (ms)

  // CSV2 send-cost breakdown (µs): build (snprintf) vs send (events.send)
  CSV2_csv2BuildLast,   // CSV2 build (snprintf) time, previous cycle (µs)
  CSV2_csv2BuildWorst,  // CSV2 build time, worst since reset (µs)
  CSV2_csv2SendLast,    // CSV2 send (events.send) time, previous cycle (µs)
  CSV2_csv2SendWorst,   // CSV2 send time, worst since reset (µs)

  // Core-0 HTTPS task — cloud op wall-clock time (ms). LAST op + WORST since Reset Peak Values.
  CSV2_httpsUpload_win, // Core-0 cloud op time, LAST (ms)  [_win = LAST, matching the faDetector row]
  CSV2_httpsUpload_ses, // Core-0 cloud op time, WORST since last Reset Peak Values (ms)

  CSV2_cvTempDerateScale, // live battery-temp gain derate multiplier on the active CV gains; ×1000

  // Control Accuracy v4 routine-data loop health (spec: CONTROL_ACCURACY_V4_ROUTINE_SPEC.md).
  // Raw accumulators; the UI derives Tracking % = inbandS/activeS, mean recovery = recovS10/10/exc,
  // Constrained % = constS/validS. Current in A, voltage in 12V-equiv (mV where scaled), thermal °F.
  CSV2_accCurValidS,    // current loop: seconds in authority
  CSV2_accCurActiveS,   // challenged seconds — tracking denominator
  CSV2_accCurInbandS,   // in-band seconds while active — tracking numerator
  CSV2_accCurConstS,    // seconds output railed / protection clamp
  CSV2_accCurExc,       // excursions (band exits) since reset
  CSV2_accCurRecovS10,  // out-of-band seconds summed across excursions (s ×10)
  CSV2_accCurOverExp,   // damaging exposure ∫(e−band)+dt (A·s ×100)
  CSV2_accCurWorst,     // worst over-current vs command (A ×100)
  CSV2_accVoltValidS,   // voltage loop: seconds CV engaged (non-zeroFloat)
  CSV2_accVoltActiveS,
  CSV2_accVoltInbandS,
  CSV2_accVoltConstS,   // seconds Icv pinned (ceiling/zero) / protection / aw-recovery
  CSV2_accVoltExc,
  CSV2_accVoltRecovS10,
  CSV2_accVoltOverExp,  // V·s 12V-equiv ×100
  CSV2_accVoltWorst,    // worst over-voltage (mV 12V-equiv)
  CSV2_accThermBindS,   // thermal: binding-and-settled seconds
  CSV2_accThermInbandS, // of those, within ±3°F of the regulation setpoint
  CSV2_accThermSess,    // containment sessions since reset
  CSV2_accThermWorst,   // worst over-temp vs limit (°F ×100) — unconditional
  CSV2_imuInstallCode,  // 0=OK 1=never zeroed 2=mount not vertical 3=zeroed pre-mount-check 4=no IMU
  CSV2_cvKdCount,       // CV D-term engagement episodes this session (rising edge, ≥1s quiet re-arm)
  CSV2_littleFsFreeKb,  // free space on the userdata LittleFS partition, kB; -1 until the boot seed; refreshed after each file write once the field gate is cut
  CSV2_cloudUpAgeS,     // seconds since last ack-confirmed cloud payload upload; -1 = never this boot
  CSV2_deviceEpoch,     // regulator wall clock, UTC epoch seconds; 0 = clock never set this boot
  CSV2_cfgPushPending,  // admin config push staged in the cloud: settings it will change; 0 = none queued
  CSV2_cfgPushApplied,  // admin config push applied on the previous boot: settings it changed; 0 = nothing to report
  CSV2_ch1FieldOnWorst,     // worst CH1 read interval with field gate open, ms — control-relevant split of ch1_worst_at
  CSV2_ch0GapFieldOnWorst,  // ADS battV read-gap worst with field gate open, ms
  CSV2_ch2GapFieldOnWorst,  // ADS RPM read-gap worst with field gate open, ms
  CSV2_blameIdx1, CSV2_blameUs1,  // worst field-on pass blame: top 3 timed consumers of the pass that set loopFieldOnSes
  CSV2_blameIdx2, CSV2_blameUs2,  // idx = ftBlameReg[] position (255 = empty), us = that call's duration in µs
  CSV2_blameIdx3, CSV2_blameUs3,
  CSV2_ft_n2kTx_win,   // NMEA2000 transmit tick worst µs (window)
  CSV2_ft_n2kTx_ses,   // NMEA2000 transmit tick worst µs (session)
  CSV2_n2kTxCount,     // N2K messages accepted by SendMsg since Reset Peak Values
  CSV2_n2kTxDrops,     // N2K messages dropped (TX queue + retry ring full — normal with no bus attached)
  CSV2_n2kSrcAddr,     // claimed N2K source address; -1 = listen-only / not claimed
  CSV2_n2kRxBattV,     // received 127508 battery voltage (V ×100; -2000000000 = not available)
  CSV2_n2kRxBattA,     // received 127508 battery current (A ×100, signed; -2000000000 = not available)
  CSV2_n2kRxBattTempF, // received 127508 battery temperature (F ×10; -2000000000 = not available)
  CSV2_n2kRxSoc,       // received 127506 state of charge (%; -1 = not available)
  CSV2_n2kRxSoh,       // received 127506 state of health (%; -1 = not available)
  CSV2_dvccState,         // DVCC follow state: 0 off, 1 waiting, 2 settling, 3 following, 4 stale, 5 untrusted
  CSV2_dvccRxCvl,         // last decoded charge-voltage limit (V ×100; -2000000000 = none)
  CSV2_dvccRxCcl,         // last decoded charge-current limit (A ×10; -2000000000 = none)
  CSV2_dvccRxSrcAddr,     // authority bus address (255 = none yet, 254 = only the Victron WiFi carrier is fresh)
  CSV2_dvccUntrustReason, // 0 none, 1 CVL out of window, 2 CCL implausible, 3 flapping
  CSV2_ft_dvcc_win,       // DVCC brain tick worst µs (window)
  CSV2_ft_dvcc_ses,       // DVCC brain tick worst µs (session)
  CSV2_ft_n2kParse_win,   // NMEA2000.ParseMessages worst µs (window) — RX drain, scales with bus traffic
  CSV2_ft_n2kParse_ses,   // NMEA2000.ParseMessages worst µs (session)
  CSV2_huntKdScale,       // damper D-lever multiplier ×100 (100 = D-term running, 0 = paused for a test / mapped off at this speed)
  CSV2_n183Sentences,     // NMEA 0183 checksum-valid sentences received since boot (any type)
  CSV2_n183ChecksumErrs,  // NMEA 0183 sentences that failed checksum — climbs on wrong polarity or a noisy line
  CSV2_ft_ReadNMEA0183_win,  // NMEA 0183 drain worst us (window) — the 0.5 ms control-loop budget check
  CSV2_ft_ReadNMEA0183_ses,  // NMEA 0183 drain worst us (session)
  CSV2_fieldDutyCeil,     // enforced field-duty ceiling ×100: ccDutyCeiling(), lowest of Max Field %, the Max Field Volts term, fieldCurDutyCeil, fieldHeatDutyCeil (fieldCeilBind names the binding one)
  CSV2_dvccAuthMfg,       // NAME manufacturer code of the node publishing the limits (358 = Victron, 2046 = open code; 0 = identity never heard)
  CSV2_dvccAuthProd,      // its 126996 product code, else its Victron VREG 0x0100 product id, else 0
  CSV2_ovTierLowCount,    // timed OV cut, LOW tier — session rising-edge counter (lifetime twin in ov_telemetry)
  CSV2_ovTierMidCount,    // timed OV cut, MID tier — session rising-edge counter
  // Solar ledger — the day in progress (kWh x100, -1 = unknown) + the pause bar in force + coverage
  CSV2_sledPredHarvToday,   // harvest the forecast promised for today, frozen the evening before
  CSV2_sledActHarvToday,    // VE.Direct panel-power integral so far today
  CSV2_sledPredConsToday,   // consumption predicted for today from the ledger
  CSV2_sledActConsToday,    // house-load energy so far today (alt + solar + batt discharge - batt charge)
  CSV2_sledNeedKwh,         // the bar the 2-of-3 rule is using right now (x100)
  CSV2_sledNeedSource,      // 0 = High Solar Threshold setting, 1 = predicted consumption + margin
  CSV2_sledDaysValid,       // complete days on the ledger (feed learning + prediction)
  CSV2_sledCoverageMin,     // minutes of today the device has been awake
  CSV2_ft_solarLedger_win,  // solar ledger service worst us (window)
  CSV2_ft_solarLedger_ses,  // solar ledger service worst us (session)
  // Battery + extra temperature probes and the battery-temperature source chain (BATTERY_TEMP_SENSORS_SPEC.md §7)
  CSV2_BatteryTempProbeF,   // BATT-role DS18B20 (°F x10; ROLL_EMPTY = no reading yet)
  CSV2_ExtraTempF,          // EXTRA-role DS18B20 (°F x10; ROLL_EMPTY = no reading yet)
  CSV2_battTempActiveF,     // batteryTempF() result this tick (°F x10; ROLL_EMPTY = no source qualifies)
  CSV2_battTempActiveSrc,   // 0 none, 1 probe, 2 NMEA 2000, 3 VE.Direct, 4 RV-C (5 retired, never emitted)
  CSV2_owProbeCount,        // DS18B20s in the 1-Wire registry
  CSV2_owUnassignedCount,   // present probes with no role
  CSV2_VictronBattTempF,    // VE.Direct "T" battery temperature (°F x10; ROLL_EMPTY = none)
  CSV2_rvcRxBattTempF,      // RV-C DC_SOURCE_STATUS_2 battery temperature (°F x10; ROLL_EMPTY = none)
  CSV2_cvTempDerateInert,   // 1 = battery-temperature source class changed since commissioning; CV derate held at 1.0
  CSV2_wmIgn_battTempF_lo,  CSV2_wmIgn_battTempF_hi,   // BatteryTempProbeF (°F, int)
  CSV2_wmIgn_extraTempF_lo, CSV2_wmIgn_extraTempF_hi,  // ExtraTempF (°F, int)

  CSV2_fieldDutyFloor,      // enforced field-duty FLOOR x100: governor_apply's max(Min Field %, this speed's Keep-Alive %), 0 when bypassed (shutdown, sysID, protection clamp, MANUAL)

  CSV2_inaDieTempF,         // INA228 internal die temperature (°F x10; ROLL_EMPTY = no reading)
  CSV2_inaDieTempMaxF,      // session max of the above (°F x10; ROLL_EMPTY = nothing has cleared the warm-up + corroboration gate yet); uploads daily

  CSV2_voltStaleCount,      // session count of REASON_VOLTAGE_STALE field events (main battery voltage, INA228)
  CSV2_volt2StaleCount,     // session count of REASON_VOLTAGE2_STALE field events (second battery voltage, ADS1115)
  CSV2_battTempLostCount,   // session count of REASON_BATT_TEMP_LOST field events

  // Sensor faults / Run Without Failed Sensors (SENSOR_FAULTS_AND_OPTIONAL_SENSORS_SPEC.md §1.2, §2.6, §4.2, §4.7)
  CSV2_sensorFaultMask,     // bit0 alt temp, 1 engine speed, 2 alt current, 3 main V, 4 second V, 5 V disagree, 6 batt temp, 7 ADS1115, 8 INA228, 9 regulator drive temperature (INA228 die) — computed regardless of overrides
  CSV2_battTempState,       // 0 not expected (source Not fitted), 1 reading, 2 lost (charging stopped), 3 waiting for the first reading (field held off)
  CSV2_limpTier,            // 0 normal, 1 closed-loop minus dependent reasons, 2 voltage-only, 3 fixed duty + second-voltage cap, 4 fixed duty
  CSV2_sensorSteadyMask,    // same bits as sensorFaultMask; set once that fault bit has been clear continuously for 10 min (the "reading again" prompt)
  CSV2_reqFaultCount,       // §11: number of required sensors faulted (chip loss expanded to its readings; owner overrides and Not fitted excluded); >= 2 = REASON_MULTI_SENSOR_FAULT
  CSV2_battCurState,        // §11.6 battery current (regulator shunt, or with none fitted the network battery monitor): 0 not declared, 1 reading, 2 lost 10 s (alarm + header line; charging continues on voltage)
  // Board field ceilings (6_functions.ino updateFieldDerateCeilings)
  CSV2_fieldCurDutyCeil,    // 10 A field-current ceiling, duty % x100
  CSV2_fieldHeatDutyCeil,   // field-drive heat ceiling, duty % x100 (99 until the first valid die sample)
  CSV2_fieldCeilBind,       // binding ccDutyCeiling() term: 0 Max Field %, 1 Max Field Volts, 2 10 A current, 3 heat at the J2 connector, 4 heat at the D5 spreader
  CSV2_fieldRcOhm,          // FieldResistance corrected for alternator temperature, less FD_MARGIN, ohm x100 (both board ceilings and Field Amps use it)
  CSV2_fieldHeatLimS,       // lifetime seconds held at the heat ceiling
  CSV2_fieldCurLimS,        // lifetime seconds held at the 10 A ceiling
  CSV2_fieldHeatLimSesS,    // session seconds held at the heat ceiling (ResetFieldHeatLimTime)
  CSV2_fieldCurLimSesS,     // session seconds held at the 10 A ceiling (ResetFieldCurLimTime)
  CSV2_dieTempLostCount,    // session count of REASON_DIE_TEMP_LOST field events

  // NMEA 2000 bus health (NMEA2K_TRANSMIT_SPEC.md §16.2; 5_functions.ino n2kBusTick)
  CSV2_n2kBusVerdict,       // 0 off, 1 OK, 2 nothing heard, 3 receive errors, 4 nobody answering, 5 bus-off (numbered by severity)
  CSV2_n2kTec,              // CAN transmit error counter (TEC), driver's 1 s status poll
  CSV2_n2kRec,              // CAN receive error counter (REC)
  CSV2_n2kRxFrames,         // frames received since Reset Peak Values (same reset as n2kTxCount)
  CSV2_n2kBusOffCount,      // bus-off recoveries since boot
  CSV2_n2kRxLost,           // received frames lost (driver queue full + controller FIFO overrun) while NMEA 2000 runs, since boot
  CSV2_n2kNodesHeard,       // distinct source addresses heard within N2K_HEARD_MS (75 s), ours excluded
  CSV2_LargestPSRAMBlock,   // largest contiguous free PSRAM block, KB (cpuLoadTask walk every 20 s, field cut only; held while charging)
  // DVCC Victron source, per carrier (dvccRxCvl/Ccl above is their merge); ages in TS_DvccVeCan / TS_DvccWifi
  CSV2_dvccRxCanCvl,        // VE.Can carrier CVL as last merged (V x100; -2000000000 = none)
  CSV2_dvccRxCanCcl,        // VE.Can carrier CCL as last merged, 0 under a stop-charging command (A x10; -2000000000 = none)
  CSV2_dvccRxWifiCvl,       // WiFi carrier (/gxLimits) last CVL (V x100; -2000000000 = not sent)
  CSV2_dvccRxWifiCcl,       // WiFi carrier last CCL (A x10; -2000000000 = not sent)
  CSV2_dvccRxCanShare,      // VE.Can: the GX's 0x2015 addressed to this node, this alternator's share (A x10; -2000000000 = none)
  CSV2_dvccNoLimitStop,     // 1 = Stop Charging Without Limits is holding the current ceiling at zero

  CSV2_sessionId,    // boot identity — matches CSV1_sessionId while this cached block is from the live run
  CSV2_sendMs,            // millis() when this payload was BUILT (CSV2 builds one pass and sends the next)

  CSV2_FIELD_COUNT // enum position is authoritative — never hand-count; CSV payload specifier count must equal this +1
};

enum Csv4Index {
  // NavStream: live nav / wind / solar / fuel readouts — sent every 500 ms (2 Hz), the rate the gauges need.
  CSV4_HeadingNMEA,             // heading (deg, int)
  CSV4_SOGNMEA,                 // speed over ground (knots ×100)
  CSV4_COGNMEA,                 // course over ground (deg, int)
  CSV4_STWNMEA,                 // speed through water (SOW, knots ×100; NAN/no-log -> 0)
  CSV4_ApparentWindSpeedNMEA,   // AWS (knots ×100)
  CSV4_ApparentWindAngleNMEA,   // AWA (deg, int)
  CSV4_TrueWindSpeedNMEA,       // TWS (knots ×100)
  CSV4_TrueWindAngleNMEA,       // TWA (deg, int)
  CSV4_LeewayNMEA,              // leeway (deg, int)
  CSV4_VMGNMEA,                 // VMG (knots ×100)
  CSV4_VMGUpwind,               // VMG to windward = SOG·cos(TWA), knots ×100
  CSV4_VictronSolarPower,       // PPV panel power (W ×1)
  CSV4_VictronSolarVoltage,     // VPV panel voltage (V ×100)
  CSV4_VictronSolarCurrent,     // derived panel current (A ×100)
  CSV4_VictronCurrent,          // Victron battery current (A ×100)
  CSV4_currentFuelGPH,          // live fuel flow (gal/hr ×100)
  CSV4_currentNMPG,             // live fuel economy (naut mi/gal ×100)
  CSV4_ctrlLimiter,             // banner limiter code: 0 none, 1 alt current cap, 2 thermal derate, 3 CV voltage loop, 4 battery current limit, 5 field at max duty, 6 protection (cap binding or recovery window), 7 battery above target, 8 BMS charge-current limit, 9 BMS charge-voltage limit, 10 Manual mode, 11 min field floor holding duty up, 12 zero-current command (Maintain / zero-current float), 13 warm-up ramp ceiling, 14 charge-rate Hi->Lo glide, 15 board 10 A field-current ceiling, 16 field-drive heat ceiling — rides NavStream so the banner tint updates at ~500ms
  CSV4_chargeStage,             // CHARGE_STAGE_* code — rides NavStream so the Plots-tab mode ribbon tracks stage changes at ~500ms (CSV2 still carries it for the thermal ring)
  CSV4_n183Heading,             // NMEA 0183 decoded heading (deg ×10; -10 = nothing decoded). Separate from CSV4_HeadingNMEA, which is the NMEA2000 source.
  CSV4_n183HdgRef,              // reference frame of the above: 0 none, 1 magnetic, 2 true
  CSV4_bmsSignalActive,         // live BMS on/off opto input (GPIO42): 1 = 5-62 V present at the wire, 0 = dead/open. Raw pin, before the Present/Absent polarity choice
  CSV4_panelHiRateInput,        // switch panel HIGH/LOW opto input (Cable 3 pin 5, GPIO39): 1 = wire energized = switch ON = asking for High. Debounced pin state, reported whether or not PhysicalPanelOverride lets it steer anything
  CSV4_panelForceFloatInput,    // switch panel FORCEFLOAT opto input (Cable 3 pin 7, GPIO40): 1 = wire energized = switch ON = asking for Maintain. Debounced pin state, reported whether or not PhysicalPanelOverride lets it steer anything
  CSV4_VictronSoc,              // VE.Direct battery monitor's own state of charge (% ×10; ROLL_EMPTY when stale, unsynced or not a battery monitor)
  CSV4_gxStreams,               // connected Victron GX streams (/gxevents clients)
  // Power flow, one instant (powerFlowSample): A ×100, + = into the battery for flowBattA; ROLL_EMPTY = not measured
  CSV4_flowBattA,               // battery current the regulator trusts (shunt, else the selected network monitor)
  CSV4_flowAltA,                // alternator output current
  CSV4_flowSolarA,              // battery-side solar current (0 with no array fitted)
  CSV4_flowSrc,                 // battery source ×10 + solar source (codes at powerFlowSample)
  CSV4_flowSolarSer,            // hash of the VE.Direct MPPT's serial while it is the solar source, else 0 (GX de-duplication)
  // Demo only, the same instant: made-up GX devices inside the simulated plant, A ×100, ROLL_EMPTY outside demo (the GX
  // bridge's FakeGx shows them, so its battery and the simulated battery are one sum)
  CSV4_demoShoreA,              // shore charger output, into the battery
  CSV4_demoFridgeA,             // fridge draw (a DC load meter on the GX)
  CSV4_demoOrionA,              // Orion XS draw from the house bank (charging the start battery)
  CSV4_demoInverterA,           // inverter draw (DC side)
  CSV4_sessionId,               // boot identity — matches CSV1_sessionId while this cached block is from the live run
  CSV4_sendMs,                  // millis() when this payload was built
  CSV4_FIELD_COUNT  // = 37
};

// CSV3 wire contract, declared ONCE: expanded into the Csv3Index enum, the snprintf format and argument list, and the
// field-name list the build uses to regenerate CSV3_FIELDS in web_src/script.js.
// Comments inside MUST be /* */: a // swallows the continuation backslash and truncates every expansion consistently,
// which only the CSV3_EXPECTED_FIELDS static_assert catches.
#define CSV3_EXPECTED_FIELDS 450
#define CSV3_LIST(X) \
  /* SettingsStream: user-configurable settings — sent on change (settingsDirty) or every 60s fallback */ \
  X(TemperatureLimitF, "%d", SafeInt(TemperatureLimitF)) \
  X(BulkVoltage, "%d", SafeInt(BulkVoltage, 100)) \
  X(wavePeriod, "%d", SafeInt(wavePeriod)) \
  X(FloatVoltage, "%d", SafeInt(FloatVoltage, 100)) \
  X(SwitchingFrequency, "%d", SafeInt(SwitchingFrequency)) \
  X(yyMin, "%d", SafeInt(yyMin)) \
  X(retired1, "%d", 0)                                                                                            /* dead slot, sends 0; kept so CSV3 indices never renumber */ \
  X(ManualDutyTarget, "%d", SafeInt(ManualDutyTarget, 100)) \
  X(PhysicalPanelOverride, "%d", SafeInt(PhysicalPanelOverride)) \
  X(waveAmplitude, "%d", SafeInt(waveAmplitude)) \
  X(CurrentThreshold, "%d", SafeInt(CurrentThreshold, 100)) \
  X(PeukertExponent_scaled, "%d", SafeInt(PeukertExponent_scaled)) \
  X(ChargeEfficiency_scaled, "%d", SafeInt(ChargeEfficiency_scaled)) \
  X(ChargedVoltage_Scaled, "%d", SafeInt(ChargedVoltage_Scaled)) \
  X(TailCurrent, "%d", SafeInt(TailCurrent, 10)) \
  X(ChargedDetectionTime, "%d", SafeInt(ChargedDetectionTime)) \
  X(IgnoreTemperature, "%d", SafeInt(IgnoreTemperature)) \
  X(bmsLogic, "%d", SafeInt(bmsLogic)) \
  X(bmsLogicLevelOff, "%d", SafeInt(bmsLogicLevelOff)) \
  X(RPMScalingFactor, "%d", SafeInt(RPMScalingFactor)) \
  X(MaximumAllowedBatteryAmps, "%d", SafeInt(MaximumAllowedBatteryAmps)) \
  X(AlternatorNominalAmps, "%d", SafeInt(AlternatorNominalAmps)) \
  X(LearningUpStep, "%d", SafeInt(LearningUpStep, 100)) \
  X(LearningDownStep, "%d", SafeInt(LearningDownStep, 100)) \
  X(xTime, "%d", SafeInt(xTime)) \
  X(MinLearningInterval, "%d", SafeInt(MinLearningInterval)) \
  X(SafeOperationThreshold, "%d", SafeInt(SafeOperationThreshold)) \
  X(PidKp, "%d", SafeInt(PidKp, 1000)) \
  X(PidKi, "%d", SafeInt(PidKi, 1000)) \
  X(PidKd, "%d", SafeInt(PidKd, 1000)) \
  X(PidSampleDivisor, "%d", SafeInt(PidSampleDivisor)) \
  X(MaxTableValue, "%d", SafeInt(MaxTableValue, 100)) \
  X(MaxPenaltyPercent, "%d", SafeInt(MaxPenaltyPercent, 100)) \
  X(MaxPenaltyDuration, "%d", SafeInt(MaxPenaltyDuration / 1000)) \
  X(NeighborLearningFactor, "%d", SafeInt(NeighborLearningFactor, 1000)) \
  X(yyMax, "%d", SafeInt(yyMax)) \
  X(LearningMemoryDuration, "%d", SafeInt(LearningMemoryDuration / 86400000)) \
  X(TuningMode, "%d", SafeInt(TuningMode)) \
  X(ShuntResistanceMicroOhm, "%d", SafeInt(ShuntResistanceMicroOhm)) \
  X(InvertAltAmps, "%d", SafeInt(InvertAltAmps)) \
  X(InvertBattAmps, "%d", SafeInt(InvertBattAmps)) \
  X(MaxDuty, "%d", SafeInt(MaxDuty)) \
  X(MinDuty, "%d", SafeInt(MinDuty, 100)) \
  X(FieldResistance, "%d", SafeInt(FieldResistance, 1000)) \
  X(maxPoints, "%d", SafeInt(maxPoints)) \
  X(AlternatorCOffset, "%d", SafeInt(AlternatorCOffset, 100)) \
  X(BatteryCOffset, "%d", SafeInt(BatteryCOffset, 100)) \
  X(BatteryCapacity_Ah, "%d", SafeInt(BatteryCapacity_Ah)) \
  X(AmpSensorRange, "%d", SafeInt(AmpSensorRange)) \
  X(R_fixed, "%d", SafeInt(R_fixed, 100)) \
  X(Beta, "%d", SafeInt(Beta, 100)) \
  X(T0_C, "%d", SafeInt(T0_C, 100)) \
  X(TempSource, "%d", SafeInt(TempSource)) \
  X(IgnitionOverride, "%d", SafeInt(IgnitionOverride)) \
  X(FLOAT_DURATION, "%d", SafeInt(FLOAT_DURATION)) \
  X(PulleyRatio, "%d", SafeInt(PulleyRatio, 100)) \
  X(BattMonSrc, "%d", SafeInt(BattMonSrc)) \
  X(rpmTableRPMPoints0, "%d", SafeInt(rpmTableRPMPoints[0])) \
  X(rpmTableRPMPoints1, "%d", SafeInt(rpmTableRPMPoints[1])) \
  X(rpmTableRPMPoints2, "%d", SafeInt(rpmTableRPMPoints[2])) \
  X(rpmTableRPMPoints3, "%d", SafeInt(rpmTableRPMPoints[3])) \
  X(rpmTableRPMPoints4, "%d", SafeInt(rpmTableRPMPoints[4])) \
  X(rpmTableRPMPoints5, "%d", SafeInt(rpmTableRPMPoints[5])) \
  X(rpmTableRPMPoints6, "%d", SafeInt(rpmTableRPMPoints[6])) \
  X(rpmTableRPMPoints7, "%d", SafeInt(rpmTableRPMPoints[7])) \
  X(rpmTableRPMPoints8, "%d", SafeInt(rpmTableRPMPoints[8])) \
  X(rpmTableRPMPoints9, "%d", SafeInt(rpmTableRPMPoints[9])) \
  X(LearningSettlingPeriod, "%d", SafeInt(LearningSettlingPeriod)) \
  X(LearningRPMChangeThreshold, "%d", SafeInt(LearningRPMChangeThreshold)) \
  X(LearningTempHysteresis, "%d", SafeInt(LearningTempHysteresis)) \
  X(fuelTableRPM0, "%d", SafeInt(fuelTableRPM[0])) \
  X(fuelTableRPM1, "%d", SafeInt(fuelTableRPM[1])) \
  X(fuelTableRPM2, "%d", SafeInt(fuelTableRPM[2])) \
  X(fuelTableRPM3, "%d", SafeInt(fuelTableRPM[3])) \
  X(fuelTableRPM4, "%d", SafeInt(fuelTableRPM[4])) \
  X(fuelTableRPM5, "%d", SafeInt(fuelTableRPM[5])) \
  X(fuelTableRPM6, "%d", SafeInt(fuelTableRPM[6])) \
  X(fuelTableRPM7, "%d", SafeInt(fuelTableRPM[7])) \
  X(fuelTableRPM8, "%d", SafeInt(fuelTableRPM[8])) \
  X(fuelTableRPM9, "%d", SafeInt(fuelTableRPM[9])) \
  X(fuelTableGPH0, "%d", SafeInt(fuelTableGPH[0], 100)) \
  X(fuelTableGPH1, "%d", SafeInt(fuelTableGPH[1], 100)) \
  X(fuelTableGPH2, "%d", SafeInt(fuelTableGPH[2], 100)) \
  X(fuelTableGPH3, "%d", SafeInt(fuelTableGPH[3], 100)) \
  X(fuelTableGPH4, "%d", SafeInt(fuelTableGPH[4], 100)) \
  X(fuelTableGPH5, "%d", SafeInt(fuelTableGPH[5], 100)) \
  X(fuelTableGPH6, "%d", SafeInt(fuelTableGPH[6], 100)) \
  X(fuelTableGPH7, "%d", SafeInt(fuelTableGPH[7], 100)) \
  X(fuelTableGPH8, "%d", SafeInt(fuelTableGPH[8], 100)) \
  X(fuelTableGPH9, "%d", SafeInt(fuelTableGPH[9], 100)) \
  X(stateRevision, "%d", SafeInt(stateRevision)) \
  X(DutyRampRate, "%d", SafeInt(DutyRampRate, 100)) \
  X(SettleTimeBeforeCut, "%d", SafeInt(SettleTimeBeforeCut)) \
  X(TempWarnExcess, "%d", SafeInt(TempWarnExcess, 100)) \
  X(TempCritExcess, "%d", SafeInt(TempCritExcess, 100)) \
  X(TempSustainedTimeout, "%d", SafeInt(TempSustainedTimeout / 1000)) \
  X(AlternatorHardShutdownV, "%d", SafeInt(AlternatorHardShutdownV, 100)) \
  X(VoltageDisagreeThreshold, "%d", SafeInt(VoltageDisagreeThreshold, 100)) \
  X(VoltageDisagreeTimeout, "%d", SafeInt(VoltageDisagreeTimeout / 1000)) \
  X(rpmMinDutyTable0, "%d", SafeInt(rpmMinDutyTable[0], 100)) \
  X(rpmMinDutyTable1, "%d", SafeInt(rpmMinDutyTable[1], 100)) \
  X(rpmMinDutyTable2, "%d", SafeInt(rpmMinDutyTable[2], 100)) \
  X(rpmMinDutyTable3, "%d", SafeInt(rpmMinDutyTable[3], 100)) \
  X(rpmMinDutyTable4, "%d", SafeInt(rpmMinDutyTable[4], 100)) \
  X(rpmMinDutyTable5, "%d", SafeInt(rpmMinDutyTable[5], 100)) \
  X(rpmMinDutyTable6, "%d", SafeInt(rpmMinDutyTable[6], 100)) \
  X(rpmMinDutyTable7, "%d", SafeInt(rpmMinDutyTable[7], 100)) \
  X(rpmMinDutyTable8, "%d", SafeInt(rpmMinDutyTable[8], 100)) \
  X(rpmMinDutyTable9, "%d", SafeInt(rpmMinDutyTable[9], 100)) \
  X(rpmCapCurrentTable0, "%d", SafeInt(rpmCapCurrentTable[0], 100)) \
  X(rpmCapCurrentTable1, "%d", SafeInt(rpmCapCurrentTable[1], 100)) \
  X(rpmCapCurrentTable2, "%d", SafeInt(rpmCapCurrentTable[2], 100)) \
  X(rpmCapCurrentTable3, "%d", SafeInt(rpmCapCurrentTable[3], 100)) \
  X(rpmCapCurrentTable4, "%d", SafeInt(rpmCapCurrentTable[4], 100)) \
  X(rpmCapCurrentTable5, "%d", SafeInt(rpmCapCurrentTable[5], 100)) \
  X(rpmCapCurrentTable6, "%d", SafeInt(rpmCapCurrentTable[6], 100)) \
  X(rpmCapCurrentTable7, "%d", SafeInt(rpmCapCurrentTable[7], 100)) \
  X(rpmCapCurrentTable8, "%d", SafeInt(rpmCapCurrentTable[8], 100)) \
  X(rpmCapCurrentTable9, "%d", SafeInt(rpmCapCurrentTable[9], 100)) \
  X(VoltageKp, "%d", SafeInt(VoltageKp, 100)) \
  X(VoltageLoopInterval, "%d", SafeInt(VoltageLoopInterval)) \
  X(FIELD_COLLAPSE_DELAY, "%d", SafeInt(FIELD_COLLAPSE_DELAY)) \
  X(SetpointRiseRate, "%d", SafeInt(SetpointRiseRate, 100)) \
  X(SetpointFallRate, "%d", SafeInt(SetpointFallRate, 100)) \
  X(SetpointBigStepThresh, "%d", SafeInt(SetpointBigStepThresh, 100)) \
  X(SetpointBigStepRiseRate, "%d", SafeInt(SetpointBigStepRiseRate, 100)) \
  X(PIDTrackingGain, "%d", SafeInt(PIDTrackingGain, 100)) \
  X(CAPSIZE_THRESHOLD_DEG, "%d", SafeInt(CAPSIZE_THRESHOLD_DEG)) \
  X(PITCHPOLE_THRESHOLD_DEG, "%d", SafeInt(PITCHPOLE_THRESHOLD_DEG)) \
  X(SLAM_THRESHOLD_G, "%d", SafeInt(SLAM_THRESHOLD_G, 10)) \
  X(imuMountOrientation, "%d", SafeInt(imuMountOrientation)) \
  X(TailCurrent_A, "%d", SafeInt(TailCurrent_A, 100)) \
  X(RebulkVoltage, "%d", SafeInt(RebulkVoltage, 100)) \
  X(rebulkDebounceTime, "%d", SafeInt(rebulkDebounceTime)) \
  X(MinFloatTime, "%d", SafeInt(MinFloatTime)) \
  X(SOC_BlockRebulk_percent, "%d", SafeInt(SOC_BlockRebulk_percent)) \
  X(SOC_AllowRebulk_percent, "%d", SafeInt(SOC_AllowRebulk_percent)) \
  X(DutySlowRampRate, "%d", SafeInt(DutySlowRampRate, 100)) \
  X(ShutdownPhase2HoldMs, "%d", SafeInt(ShutdownPhase2HoldMs)) \
  X(TempPIDKp, "%d", SafeInt(TempPIDKp, 1000)) \
  X(TempPIDKi, "%d", SafeInt(TempPIDKi, 1000)) \
  X(ThermalLookaheadSec, "%d", SafeInt(ThermalLookaheadSec)) \
  X(TempPIDIntervalMs, "%d", SafeInt(TempPIDIntervalMs)) \
  X(TempPIDFilterAlpha, "%d", SafeInt(TempPIDFilterAlpha, 1000)) \
  X(VoltageKi, "%d", SafeInt(VoltageKi, 100)) \
  X(rpmCapPowerTable0, "%d", (int)rpmCapPowerTable[0]) \
  X(rpmCapPowerTable1, "%d", (int)rpmCapPowerTable[1]) \
  X(rpmCapPowerTable2, "%d", (int)rpmCapPowerTable[2]) \
  X(rpmCapPowerTable3, "%d", (int)rpmCapPowerTable[3]) \
  X(rpmCapPowerTable4, "%d", (int)rpmCapPowerTable[4]) \
  X(rpmCapPowerTable5, "%d", (int)rpmCapPowerTable[5]) \
  X(rpmCapPowerTable6, "%d", (int)rpmCapPowerTable[6]) \
  X(rpmCapPowerTable7, "%d", (int)rpmCapPowerTable[7]) \
  X(rpmCapPowerTable8, "%d", (int)rpmCapPowerTable[8]) \
  X(rpmCapPowerTable9, "%d", (int)rpmCapPowerTable[9]) \
  X(SystemIDStepAmplitude, "%d", SafeInt(SystemIDStepAmplitude, 10)) \
  X(HardOCTripAmps, "%d", SafeInt(HardOCTripAmps, 10)) \
  X(HardOCDebounceMs, "%d", SafeInt(HardOCDebounceMs)) \
  X(IExcessFrac, "%d", SafeInt(IExcessFrac, 1000))                                                                /* CV threshold fraction (×1000) */ \
  X(IExcessFloorA, "%d", SafeInt(IExcessFloorA, 10))                                                              /* threshold floor (A ×10) */ \
  X(IExcessKBleed, "%d", SafeInt(IExcessKBleed, 100)) \
  X(IgnoreRPM, "%d", SafeInt(IgnoreRPM)) \
  X(MinRPMForField, "%d", SafeInt(MinRPMForField)) \
  X(AwBleedRate, "%d", SafeInt(AwBleedRate, 10)) \
  X(KHard, "%d", SafeInt(KHard, 10)) \
  X(ReseedFrac, "%d", SafeInt(ReseedFrac, 100)) \
  X(AwSeedProtectMs, "%d", (int)AwSeedProtectMs) \
  X(displayTempUnit, "%d", SafeInt(displayTempUnit)) \
  X(WarmupRampRate, "%d", SafeInt(WarmupRampRate, 10)) \
  X(OvGroup1Enable, "%d", (int)OvGroup1Enable) \
  X(OvGroup2Enable, "%d", (int)OvGroup2Enable) \
  X(IExcessCeilA, "%d", SafeInt(IExcessCeilA, 10))                                                                /* threshold ceiling (A ×10) */ \
  X(IExcessTau, "%d", SafeInt(IExcessTau))                                                                        /* EMA time constant (ms, raw int) */ \
  X(OutputPIDSigSrc, "%d", OutputPIDSigSrc) \
  X(TdPred, "%.3f", TdPred) \
  X(OvMeasMarginV, "%.3f", OvMeasMarginV) \
  X(OvPredMarginV, "%.3f", OvPredMarginV) \
  X(OutputPIDMA_N, "%d", OutputPIDMA_N) \
  X(OutputPIDFilterTC, "%d", (int)OutputPIDFilterTC) \
  X(VoltageFilterTC, "%d", (int)VoltageFilterTC) \
  X(CvKdVoltFiltTC, "%d", (int)CvKdVoltFiltTC) \
  X(CvKdDeadbandVps, "%d", SafeInt(CvKdDeadbandVps, 100)) \
  X(VoltageKd, "%d", SafeInt(VoltageKd, 10)) \
  X(DvdtTC, "%d", SafeInt(DvdtTC, 10)) \
  X(CvKdArmV, "%d", SafeInt(CvKdArmV, 100)) \
  X(StartupRiseRate, "%d", SafeInt(StartupRiseRate, 100)) \
  X(absorptionCompleteTime, "%d", SafeInt(absorptionCompleteTime)) \
  X(OnOff, "%d", SafeInt(OnOff)) \
  X(ManualFieldToggle, "%d", SafeInt(ManualFieldToggle)) \
  X(HiLow, "%d", SafeInt(HiLow)) \
  X(LimpHome, "%d", SafeInt(LimpHome)) \
  X(AlarmActivate, "%d", SafeInt(AlarmActivate)) \
  X(TempAlarm, "%d", SafeInt(TempAlarm)) \
  X(VoltageAlarmHigh, "%d", SafeInt(VoltageAlarmHigh, 100)) \
  X(VoltageAlarmLow, "%d", SafeInt(VoltageAlarmLow, 100)) \
  X(CurrentAlarmHigh, "%d", SafeInt(CurrentAlarmHigh)) \
  X(AlarmTest, "%d", SafeInt(AlarmTest)) \
  X(AlarmLatchEnabled, "%d", SafeInt(AlarmLatchEnabled)) \
  X(GeneralFaultAlarm, "%d", SafeInt(GeneralFaultAlarm)) \
  X(MaintainMode, "%d", SafeInt(MaintainMode)) \
  X(ManualSOCPoint, "%d", SafeInt(ManualSOCPoint, 100)) \
  X(IgnoreLearningDuringPenalty, "%d", SafeInt(IgnoreLearningDuringPenalty)) \
  X(LogAllLearningEvents, "%d", SafeInt(LogAllLearningEvents)) \
  X(CloudFeatures, "%d", SafeInt(CloudFeatures)) \
  X(AutoAltCurrentZero, "%d", SafeInt(AutoAltCurrentZero)) \
  X(WindingTempOffset, "%d", SafeInt(WindingTempOffset)) \
  X(ManualLifePercentage, "%d", SafeInt(ManualLifePercentage)) \
  X(UVThresholdHigh, "%d", SafeInt(UVThresholdHigh, 100)) \
  X(weatherModeEnabled, "%d", SafeInt(weatherModeEnabled)) \
  X(imuEnabled, "%d", SafeInt(imuEnabled ? 1 : 0)) \
  X(AbsorptionVoltage, "%d", SafeInt(AbsorptionVoltage * 100)) \
  X(AbsorptionTimeoutMs, "%d", SafeInt(AbsorptionTimeoutMs)) \
  X(bulkVoltageHoldMs, "%d", SafeInt(bulkVoltageHoldMs)) \
  X(capLimitMode, "%d", SafeInt(capLimitMode)) \
  X(TargetVoltageMode, "%d", SafeInt(TargetVoltageMode)) \
  X(TargetVoltageSetpoint, "%d", SafeInt(TargetVoltageSetpoint, 100)) \
  X(RebulkCurrent_A, "%d", SafeInt(RebulkCurrent_A, 100)) \
  X(UseFloat, "%d", SafeInt(UseFloat)) \
  X(IExcessFracBulk, "%d", SafeInt(IExcessFracBulk, 1000))                                                        /* BULK threshold fraction (×1000) */ \
  X(IExcessRelFrac, "%d", SafeInt(IExcessRelFrac, 1000))                                                          /* release hysteresis fraction (×1000) */ \
  X(systemIDPlantTauMs, "%d", SafeInt(systemIDPlantTauMs))                                                        /* fitted plant time constant (ms), persisted */ \
  X(TempAlarmLow, "%d", SafeInt(TempAlarmLow)) \
  X(LoadDumpDtThresh, "%d", SafeInt(LoadDumpDtThresh)) \
  X(LoadDumpDtThresh1, "%d", SafeInt(LoadDumpDtThresh1)) \
  X(CVTuningMode, "%d", (int)CVTuningMode) \
  X(cvWaveAmplitudeV, "%d", SafeInt(cvWaveAmplitudeV, 100)) \
  X(cvWavePeriodSec, "%d", (int)cvWavePeriodSec) \
  X(cvKOvershoot, "%d", SafeInt(cvKOvershoot, 10)) \
  X(cvConsecutiveReads, "%d", (int)cvConsecutiveReads) \
  X(webgaugesinterval, "%d", SafeInt(webgaugesinterval)) \
  X(plotTimeWindow, "%d", SafeInt(plotTimeWindow)) \
  X(Ymin1, "%d", SafeInt(Ymin1)) \
  X(Ymax1, "%d", SafeInt(Ymax1)) \
  X(Ymin2, "%d", SafeInt(Ymin2, 100)) \
  X(Ymax2, "%d", SafeInt(Ymax2, 100)) \
  X(Ymin3, "%d", SafeInt(Ymin3)) \
  X(Ymax3, "%d", SafeInt(Ymax3)) \
  X(Ymin4, "%d", SafeInt(Ymin4)) \
  X(Ymax4, "%d", SafeInt(Ymax4)) \
  X(LoadDumpDtThresh3, "%d", SafeInt(LoadDumpDtThresh3)) \
  X(hardwarePresent, "%d", SafeInt(hardwarePresent)) \
  X(testProtectionsEnabled, "%d", (int)testProtectionsEnabled)                                                    /* runtime flag — not persisted, resets true (enabled) on boot */ \
  X(IExcessArmMarginV, "%.3f", IExcessArmMarginV)                                                                 /* %.3f — iExcess voltage gate, independent of OvMeasMarginV */ \
  X(FastSetpointRiseRate, "%d", SafeInt(FastSetpointRiseRate, 100))                                               /* ×100, 1 decimal — multiplier on setpoint rise slew during post-protection recovery */ \
  X(FastSetpointRiseWindowMs, "%d", (int)FastSetpointRiseWindowMs)                                                /* raw ms — hard upper bound on fast-rise window */ \
  X(FastSetpointRiseHeadroomV, "%d", SafeInt(FastSetpointRiseHeadroomV, 100))                                     /* ×100, 2 decimal — V below target at which fast-rise gate stays open */ \
  X(SolarWatts, "%d", SafeInt(SolarWatts)) \
  X(performanceRatio, "%d", SafeInt(performanceRatio, 100))                                                       /* ×100, 2 decimal */ \
  X(VeData, "%d", SafeInt(VeData))                                                                                /* 0/1 */ \
  X(NMEA0183Data, "%d", SafeInt(NMEA0183Data))                                                                    /* 0/1 */ \
  X(NMEA2KData, "%d", SafeInt(NMEA2KData))                                                                        /* 0/1 */ \
  X(timeAxisModeChanging, "%d", SafeInt(timeAxisModeChanging))                                                    /* 0/1 */ \
  X(timeSourceMode, "%d", (int)timeSourceMode)                                                                    /* 0=auto, 1=NMEA-forced, 2=Phone-forced, 3=NTP-time-forced */ \
  X(speedSourceMode, "%d", (int)speedSourceMode)                                                                  /* 0=NMEA 2000, 1=phone GPS (speed/course owner — selectable, never auto) */ \
  /* Fast alt-current diagnostic knobs (Pattern B echo) */ \
  X(faEnabled, "%d", (int)faEnabled)                                                                              /* 0/1 — global ON/OFF */ \
  X(faAlarmEnable, "%d", (int)faAlarmEnable)                                                                      /* 0/1 — FAULT drives audible alarm */ \
  X(faAnomPause, "%d", (int)faAnomPause)                                                                          /* 0/1 — freeze anomaly flipbook slots */ \
  X(faRpmEdgeMargin, "%d", SafeInt(faRpmEdgeMargin, 10))                                                          /* RPM ×10 */ \
  X(faAmpsDriftFloorA, "%d", SafeInt(faAmpsDriftFloorA, 100))                                                     /* A ×100 */ \
  X(faAmpsDriftPct, "%d", SafeInt(faAmpsDriftPct, 10))                                                            /* percent ×10 */ \
  X(faAttenUpAmps, "%d", SafeInt(faAttenUpAmps, 10))                                                              /* A ×10 */ \
  X(faAttenDownAmps, "%d", SafeInt(faAttenDownAmps, 10))                                                          /* A ×10 */ \
  X(faPeakMinA, "%d", SafeInt(faPeakMinA, 100))                                                                   /* A ×100 */ \
  X(wifiNapEnabled, "%d", (int)wifiNapEnabled)                                                                    /* 0/1 — WiFi Napping standby toggle (Client only) */ \
  X(imuHeelOffset, "%d", SafeInt(imuHeelOffsetDeg, 100))                                                          /* captured rest heel offset (deg ×100); on CSV3 so the Level Zero echo is fast */ \
  X(imuPitchOffset, "%d", SafeInt(imuPitchOffsetDeg, 100))                                                        /* captured rest pitch offset (deg ×100) */ \
  X(systemIDTestType, "%d", SafeInt(systemIDTestType))                                                            /* 0=step, 1=sine sweep (Plant Delay test type) */ \
  X(systemIDSineFreqStart, "%d", SafeInt(systemIDSineFreqStart, 10))                                              /* Hz ×10 */ \
  X(systemIDSineFreqEnd, "%d", SafeInt(systemIDSineFreqEnd, 10))                                                  /* Hz ×10 */ \
  X(systemIDSineCycles, "%d", SafeInt(systemIDSineCycles))                                                        /* analysed cycles per sweep frequency */ \
  X(tuningWaveform, "%d", SafeInt(tuningWaveform))                                                                /* 0=square, 1=sine manual, 2=sine auto-sweep */ \
  X(tuningSineFreq, "%d", SafeInt(tuningSineFreq, 10))                                                            /* Hz ×10 (manual sine frequency) */ \
  X(tuningSweepStart, "%d", SafeInt(tuningSweepStart, 10))                                                        /* Hz ×10 */ \
  X(tuningSweepEnd, "%d", SafeInt(tuningSweepEnd, 10))                                                            /* Hz ×10 */ \
  X(tuningSweepCycles, "%d", SafeInt(tuningSweepCycles))                                                          /* analysed cycles per sweep frequency */ \
  X(SystemIDStabilizeAmps, "%d", SafeInt(SystemIDStabilizeAmps, 10))                                              /* A ×10 — plant-delay baseline/trough current */ \
  X(tuningWaveFloor, "%d", SafeInt(tuningWaveFloor))                                                              /* A — Current Target Generator wave floor (trough), shared square + sine */ \
  X(commissionState, "%d", (int)commissionState)                                                                  /* auto-commissioning state: 0=not, 1=in-progress, 2=commissioned */ \
  X(commissionPhase, "%d", (int)commissionPhase)                                                                  /* current wizard phase: 0=Prep…9=Charge health calibration, 10=finished */ \
  X(commissionDoneMask, "%d", (int)commissionDoneMask)                                                            /* per-stage completion bitmask (bit i = stage i done) */ \
  X(cvHelpersEnabled, "%d", (int)cvHelpersEnabled)                                                                /* master switch: asymmetric KiDown unwind + CV D term (1=on) */ \
  X(MinChargeTempF, "%d", SafeInt(MinChargeTempF))                                                                /* cold-charge lockout board-temp floor (°F) */ \
  X(coldChargeLockoutEnable, "%d", (int)coldChargeLockoutEnable)                                                  /* cold-charge lockout master on/off (1=on) */ \
  X(cvGainMode, "%d", (int)cvGainMode)                                                                            /* CV Gain Source: 0=Manual, 1=Measured (α/K anchored) */ \
  X(cvPlantK, "%d", SafeInt(cvPlantK, 10000))                                                                     /* measured plant gain K (V/A); ×10000 */ \
  X(cvComputedKp, "%d", SafeInt(cvComputedKp, 100))                                                               /* Plant-fit Kp, run under CV Gain Source = Measured (12V-equiv); ×100 */ \
  X(cvComputedKi, "%d", SafeInt(cvComputedKi, 100))                                                               /* Plant-fit Ki, run under CV Gain Source = Measured (12V-equiv); ×100 */ \
  X(cvCrossover, "%d", SafeInt(cvCrossover, 100))                                                                 /* CV crossover ω_c (rad/s); ×100 */ \
  X(cvPiZero, "%d", SafeInt(cvPiZero, 100))                                                                       /* CV PI integral zero ρ (rad/s); ×100 */ \
  X(vTgtRampUp, "%d", SafeInt(vTgtRampUp, 1000))                                                                  /* CV voltage-target ramp UP rate (V/s); ×1000 */ \
  X(vTgtRampDn, "%d", SafeInt(vTgtRampDn, 1000))                                                                  /* CV voltage-target ramp DOWN rate (V/s); ×1000 */ \
  X(vTgtRampEnable, "%d", (int)vTgtRampEnable)                                                                    /* CV voltage-target slew master switch (0/1) */ \
  X(setpointSlewEnable, "%d", (int)setpointSlewEnable)                                                            /* inner-loop current setpoint slew master switch (0/1) */ \
  X(cvRiseGovEnable, "%d", (int)cvRiseGovEnable)                                                                  /* CV rise governor / anti-windup master switch (0/1) */ \
  X(dutySlewEnable, "%d", (int)dutySlewEnable)                                                                    /* field duty slew master switch (0/1) */ \
  X(CommissionTempF, "%d", isnan(CommissionTempF) ? ROLL_EMPTY : (int)lroundf(CommissionTempF * 10.0f))           /* measured battery temp when CV fit applied — derate reference (°F ×10; ROLL_EMPTY = unset/NaN) */ \
  X(battTempDerateEnable, "%d", (int)battTempDerateEnable)                                                        /* battery-temp gain derate master on/off (0/1) */ \
  X(battTempCoeff, "%d", SafeInt(battTempCoeff, 10000))                                                           /* battery fractional resistance change per °C; ×10000 */ \
  X(TempPIDKiDownFrac, "%d", SafeInt(TempPIDKiDownFrac, 1000))                                                    /* thermal velocity-form below-setpoint integral bleed ratio (×Ki); ×1000 */ \
  X(ThermalSlopeWindowSec, "%d", SafeInt(ThermalSlopeWindowSec))                                                  /* thermal slope backward-difference window (s); integer */ \
  X(BattCurrentLimitA, "%d", SafeInt(BattCurrentLimitA, 10))                                                      /* max battery charge current (A ×10, G4); 0 = disabled — ceiling on the alternator command = limit + house-load offset */ \
  /* measured-ripple capture admission gates (§10.8/§11) — own knobs, decoupled from the fa* detector gates */ \
  X(ripWinMs, "%d", SafeInt(ripWinMs))                                                                            /* pk-pk capture window (ms, integer) */ \
  X(ripDriftFloorA, "%d", SafeInt(ripDriftFloorA, 100))                                                           /* shared floor: command-travel gate + stationarity mean-shift tolerance (A ×100) */ \
  X(ripDriftPct, "%d", SafeInt(ripDriftPct, 10))                                                                  /* command-travel gate slope (% of mean, ×10) — command gate only since §11 */ \
  X(SocAlarmLow, "%d", SafeInt(SocAlarmLow))                                                                      /* low-SoC alarm threshold (%, integer); 0 = disabled */ \
  X(battMaxMode, "%d", SafeInt(battMaxMode))                                                                      /* battery V/I plot sampling: 0 = window mean, 1 = max-magnitude */ \
  X(IExcessBaseA, "%d", SafeInt(IExcessBaseA, 10))                                                                /* over-current trip-line intercept / CV base (A ×10) */ \
  X(IExcessCcOffsetA, "%d", SafeInt(IExcessCcOffsetA, 10))                                                        /* CC trip line offset above CV (A ×10) */ \
  X(BatteryShuntPresent, "%d", SafeInt(BatteryShuntPresent))                                                      /* 1 = INA228 battery shunt fitted; 0 = no battery-current sensor */ \
  X(cvRecovEnable, "%d", (int)cvRecovEnable)                                                                      /* post-protection integrator-refill master switch (0/1) */ \
  X(cvRecovSec, "%d", SafeInt(cvRecovSec, 10))                                                                    /* retired timed-window knob; slot kept (never repurpose); ×10 */ \
  X(cvRecovEmaxV, "%d", SafeInt(cvRecovEmaxV, 1000))                                                              /* retired timed-window knob; slot kept (never repurpose); ×1000 */ \
  X(testSlewMode, "%d", (int)testSlewMode)                                                                        /* manual CC square-wave test slew mode (0=off, 1=default rates, 2=custom) */ \
  X(cvTestSlewMode, "%d", (int)cvTestSlewMode)                                                                    /* manual CV square-wave test slew mode (0=off, 1=default rates, 2=custom) */ \
  X(CvKdOneSided, "%d", (int)CvKdOneSided)                                                                        /* CV D-term mode: 1=one-sided (removes current only), 0=symmetric */ \
  X(fieldDecayTauMs, "%d", SafeInt(fieldDecayTauMs))                                                              /* commissioned field drain time, command→10% of output (ms); worst-case (longest) endpoint of the drain-vs-RPM line, or the flat value */ \
  X(commissionManualMask, "%d", (int)commissionManualMask)                                                        /* per-stage set-by-hand bitmask (skip / mark-done-manually); pairs with commissionDoneMask */ \
  X(CvKdMaxTrimA, "%d", SafeInt(CvKdMaxTrimA, 10))                                                                /* CV D-term back-off ceiling (A ×10); caps kdTrim so a fast rise saturates instead of flooring the field */ \
  X(cvAlpha, "%d", SafeInt(cvAlpha, 1000))                                                                        /* CV auto-gain aggressiveness α (fraction of the deadbeat-ohmic gain); ×1000 */ \
  X(CvKdSlopeCeil, "%d", SafeInt(CvKdSlopeCeil, 10))                                                              /* CV D-term slope ceiling (V/s real per-bus ×10) — max slope the D acts on */ \
  X(cvComputedKd, "%d", SafeInt(cvComputedKd, 100))                                                               /* Plant-fit D gain Kd = CvKdTd·cvComputedKp (12V-equiv); ×100 */ \
  X(CvKdDbSlope, "%d", SafeInt(CvKdDbSlope, 10000))                                                               /* CV D-term deadband line slope (V/s per A ×10000) */ \
  X(CvKdDbFloor, "%d", SafeInt(CvKdDbFloor, 100))                                                                 /* CV D-term deadband line floor (V/s ×100) */ \
  X(CvKdDbCeil, "%d", SafeInt(CvKdDbCeil, 100))                                                                   /* CV D-term deadband line ceiling (V/s ×100) */ \
  X(cvRecovBoostEnable, "%d", (int)cvRecovBoostEnable)                                                            /* post-protection recovery P-boost master switch (0/1) */ \
  X(cvRecovBoostMax, "%d", SafeInt(cvRecovBoostMax, 100))                                                         /* recovery P-boost max multiplier at full shortfall; ×100 */ \
  X(cvRecovBoostErrV, "%d", SafeInt(cvRecovBoostErrV, 1000))                                                      /* recovery P-boost full-boost shortfall (V per 12V block); ×1000 */ \
  X(fdDrainLoMs, "%d", SafeInt(fdDrainLoMs))                                                                      /* drain-vs-RPM line: drain (ms) at fdDrainRpmLo; 0 = no line (flat fieldDecayTauMs) */ \
  X(fdDrainHiMs, "%d", SafeInt(fdDrainHiMs))                                                                      /* drain-vs-RPM line: drain (ms) at fdDrainRpmHi */ \
  X(fdDrainRpmLo, "%d", SafeInt(fdDrainRpmLo))                                                                    /* drain-vs-RPM line: lowest tested RPM (lookup clamps here) */ \
  X(fdDrainRpmHi, "%d", SafeInt(fdDrainRpmHi))                                                                    /* drain-vs-RPM line: highest tested RPM (lookup clamps here) */ \
  X(HardOCEnable, "%d", (int)HardOCEnable)                                                                        /* Group 0 hard over-current trip enable (0/1) */ \
  X(IExcessEnable, "%d", (int)IExcessEnable)                                                                      /* Group 3 iExcess detectors enable (0/1, gates CV + bulk) */ \
  X(BattLimitEnable, "%d", (int)BattLimitEnable)                                                                  /* Group 4 battery charge-current ceiling enable (0/1) */ \
  X(CvKdExcessMode, "%d", (int)CvKdExcessMode)                                                                    /* CV D-term response shape (1 = slope excess over the tolerance line, 0 = legacy full-slope latch) */ \
  X(CvStressDropV, "%d", SafeInt(CvStressDropV, 100))                                                             /* stress-test target headroom below settled idle (V 12V-equiv ×100, class-scaled at use) */ \
  X(CvStressFailBandV, "%d", SafeInt(CvStressFailBandV, 100))                                                     /* stress-test stability fail band (V 12V-equiv ×100, class-scaled at use) */ \
  X(CvBrakeFallRate, "%d", SafeInt(CvBrakeFallRate, 100))                                                         /* brake-tier setpoint fall rate while CV D-term removes current (A/s ×100) */ \
  X(cvRecovKiMax, "%d", SafeInt(cvRecovKiMax, 100))                                                               /* refill Ki multiplier at release, tapering to 1x as the deficit heals; ×100 */ \
  X(cvWindDownEnable, "%d", (int)cvWindDownEnable)                                                                /* commanded-target wind-down governor master switch (0/1) */ \
  X(cvWindDownRate, "%d", SafeInt(cvWindDownRate, 1000))                                                          /* wind-down shed rate (fraction of MaxTableValue per second); ×1000 */ \
  X(cvWindDownStopV, "%d", SafeInt(cvWindDownStopV, 1000))                                                        /* wind-down stop margin above commanded target (V real per-bus, class-scaled at store); ×1000 */ \
  X(LoadDumpEnable, "%d", (int)LoadDumpEnable)                                                                    /* Group 5 load dump enable (0/1) */ \
  X(loadServeBoostEnable, "%d", (int)loadServeBoostEnable)                                                        /* load-serve Ki boost toward measured house loads (0/1, shunt-gated) */ \
  X(reseedCorrEnable, "%d", (int)reseedCorrEnable)                                                                /* demand-corrected reseed: load-drop subtraction + rapid-refire ratchet (0/1) */ \
  X(HuntGovEnable, "%d", (int)HuntGovEnable)                                                                      /* hunt-governor (oscillation damper) master switch (0/1) */ \
  X(ReseedFracNoShunt, "%d", SafeInt(ReseedFracNoShunt, 100))                                                     /* no-shunt recovery seed fraction (×100) */ \
  X(CvRecovClimbRate, "%d", SafeInt(CvRecovClimbRate, 100))                                                       /* recovery climb floor rate, fraction of MaxTableValue/s (×100) */ \
  X(protTestCutMs, "%d", SafeInt(protTestCutMs))                                                                  /* protection-test manual hard-cut hold (ms) */ \
  X(protTestGapMs, "%d", SafeInt(protTestGapMs))                                                                  /* protection-test gap between repeated cuts (ms) */ \
  X(protTestReps, "%d", SafeInt(protTestReps))                                                                    /* protection-test repeat count */ \
  X(protTestAmps, "%d", SafeInt(protTestCmdA))                                                                    /* protection-test energize target current (A); 0 = auto-seed at fire */ \
  X(cvRecovBoostFloorV, "%d", SafeInt(cvRecovBoostFloorV, 1000))                                                  /* recovery P-boost dead area below target (V per 12V block); ×1000 */ \
  X(cvRecovDeepBandV, "%d", SafeInt(cvRecovDeepBandV, 1000))                                                      /* deep-recovery band (V per 12V block); ×1000 */ \
  X(cvRecovDeepMult, "%d", SafeInt(cvRecovDeepMult, 100))                                                         /* starve-walk rate multiplier at full depth; ×100 */ \
  X(cvRecovFlareBandV, "%d", SafeInt(cvRecovFlareBandV, 1000))                                                    /* arrival flare band (V per 12V block); ×1000 */ \
  X(cvRecovFlareFrac, "%d", SafeInt(cvRecovFlareFrac, 100))                                                       /* arrival flare ceiling floor, fraction of recovery goal; ×100 */ \
  X(TachLieEnable, "%d", (int)TachLieEnable)                                                                      /* tach-lie plausibility cut enable (0/1) */ \
  X(n2kTxEnable, "%d", SafeInt(n2kTxEnable))                                                                      /* NMEA2000 transmit master (0/1) */ \
  X(n2kDeviceInstance, "%d", SafeInt(n2kDeviceInstance))                                                          /* N2K device instance */ \
  X(n2kBattEnable, "%d", SafeInt(n2kBattEnable))                                                                  /* battery 127508+127506 pair (0/1) */ \
  X(n2kBattInstance, "%d", SafeInt(n2kBattInstance)) \
  X(n2kBattCfgEnable, "%d", SafeInt(n2kBattCfgEnable))                                                            /* 127513 battery configuration (0/1) */ \
  X(n2kAltEnable, "%d", SafeInt(n2kAltEnable))                                                                    /* alternator 127508+127506 DCType=Alternator pair (0/1) */ \
  X(n2kAltInstance, "%d", SafeInt(n2kAltInstance)) \
  X(n2kAltTempEnable, "%d", SafeInt(n2kAltTempEnable))                                                            /* 130312 alternator temperature (0/1) */ \
  X(n2kTempInstance, "%d", SafeInt(n2kTempInstance)) \
  X(n2kTempSource, "%d", SafeInt(n2kTempSource))                                                                  /* tN2kTempSource code (3 = Engine Room) */ \
  X(n2kChgrEnable, "%d", SafeInt(n2kChgrEnable))                                                                  /* 127507 charger status (0/1) */ \
  X(n2kChgrInstance, "%d", SafeInt(n2kChgrInstance)) \
  X(n2kChgrCfgEnable, "%d", SafeInt(n2kChgrCfgEnable))                                                            /* 127510 charger configuration, carries field drive % (0/1) */ \
  X(n2kChgrMode, "%d", SafeInt(n2kChgrMode))                                                                      /* tN2kChargerMode label: 0 Standalone, 1 Primary, 2 Secondary */ \
  X(n2kEngRpmEnable, "%d", SafeInt(n2kEngRpmEnable))                                                              /* 127488 engine RPM (0/1) */ \
  X(n2kEngInstance, "%d", SafeInt(n2kEngInstance)) \
  X(n2kEngDynEnable, "%d", SafeInt(n2kEngDynEnable))                                                              /* 127489 engine dynamic (0/1) */ \
  X(n2kEngBitsEnable, "%d", SafeInt(n2kEngBitsEnable))                                                            /* discrete warning bits inside 127489 (0/1) */ \
  X(n2kConvStatEnable, "%d", SafeInt(n2kConvStatEnable))                                                          /* 127750 converter status (0/1) */ \
  X(n2kRxBattInstance, "%d", SafeInt(n2kRxBattInstance))                                                          /* battery instance to ingest (127508/127506 receive) */ \
  X(dvccEn, "%d", SafeInt(dvccEn))                                                                                /* DVCC follow master (0/1) */ \
  X(dvccSrcType, "%d", SafeInt(dvccSrcType))                                                                      /* limit source: 0 Victron GX (VE.Can and WiFi carriers), 1 RV-C */ \
  X(dvccInst, "%d", SafeInt(dvccInst))                                                                            /* RV-C DC instance filter (0 = any) */ \
  X(dvccSilenceS, "%d", SafeInt(dvccSilenceS))                                                                    /* silence timeout (s) */ \
  X(dvccSettleS, "%d", SafeInt(dvccSettleS))                                                                      /* settling time (s) */ \
  X(dvccCvlMin, "%d", SafeInt(dvccCvlMin, 100))                                                                   /* plausible-CVL window low (V ×100) */ \
  X(dvccCvlMax, "%d", SafeInt(dvccCvlMax, 100))                                                                   /* plausible-CVL window high (V ×100) */ \
  X(dvccVicCutHold, "%d", SafeInt(dvccVicCutHold))                                                                /* Victron do-not-charge held after it stops arriving (0/1) */ \
  X(dvccLostStop, "%d", SafeInt(dvccLostStop))                                                                    /* charge nothing while follow is on and nothing is followed (0/1) */ \
  X(HuntCutPct, "%d", (int)HuntCutPct)                                                                            /* damper test/pocket gain, % of user Ki */ \
  X(HuntVerifyPct, "%d", (int)HuntVerifyPct)                                                                      /* damper verify bar, % ripple reduction required */ \
  X(HuntWingPct, "%d", (int)HuntWingPct)                                                                          /* damper pocket taper width, % of speed per side */ \
  X(HuntCooldownMin, "%d", (int)HuntCooldownMin)                                                                  /* damper retest cooldown after a failed test (min) */ \
  X(HuntSteadyPct, "%d", (int)HuntSteadyPct)                                                                      /* damper engine-speed steadiness tolerance (%) */ \
  X(HuntQualifyScans, "%d", (int)HuntQualifyScans)                                                                /* damper wobble-confirm scan count (1.6 s each) */ \
  X(HuntTrigPct, "%d", SafeInt(HuntTrigPct, 1000))                                                                 /* damper detection bar: peak-bin duty swing % (x1000) */ \
  X(NMEA0183Baud, "%d", (int)NMEA0183Baud)                                                                        /* NMEA 0183 serial baud (4800 / 9600 / 19200 / 38400) */ \
  X(NMEA0183Invert, "%d", (int)NMEA0183Invert)                                                                    /* NMEA 0183 UART polarity: 0 RS-232-level talker, 1 TTL-level talker */ \
  X(displayVolUnit, "%d", SafeInt(displayVolUnit))                                                                /* fuel volume display preference: 0 US gallons, 1 litres */ \
  X(gpsPositionSource, "%d", (int)gpsPositionSource)                                                              /* 0=auto, 1=NMEA-forced, 2=phone-forced (position only; the clock is CSV3_timeSourceMode) */ \
  X(MaxFieldVolts, "%d", SafeInt(MaxFieldVolts, 10))                                                              /* field-volt cap (x10); the enforced ceiling is CSV2_fieldDutyCeil */ \
  X(OvTierLoMarginV, "%.3f", OvTierLoMarginV)                                                                     /* timed OV cut LOW-tier margin above target (V, %.3f) */ \
  X(OvTierLoDwellMs, "%d", SafeInt(OvTierLoDwellMs))                                                              /* timed OV cut LOW-tier continuous dwell (ms; 0 = tier off) */ \
  X(OvTierMidMarginV, "%.3f", OvTierMidMarginV)                                                                   /* timed OV cut MID-tier margin above target (V, %.3f) */ \
  X(OvTierMidDwellMs, "%d", SafeInt(OvTierMidDwellMs))                                                            /* timed OV cut MID-tier continuous dwell (ms; 0 = tier off) */ \
  X(VoltageHardwareLimit, "%d", SafeInt(VoltageHardwareLimit, 100))                                               /* INA228 hardware shutdown voltage (x100) — top OV-ladder rung, user setting */ \
  X(LoadDumpN1, "%d", SafeInt(LoadDumpN1))                                                                        /* load-dump tier 1 consecutive-reading count (time to act = N INA228 readings ~6 ms each + next control tick ~13 ms) */ \
  X(LoadDumpN2, "%d", SafeInt(LoadDumpN2))                                                                        /* load-dump tier 2 consecutive-sample count */ \
  X(LoadDumpN3, "%d", SafeInt(LoadDumpN3))                                                                        /* load-dump tier 3 consecutive-sample count */ \
  X(solarLearnEnable, "%d", SafeInt(solarLearnEnable))                                                            /* 0/1 — learn performanceRatio from each complete day's actual/forecast harvest */ \
  X(solarUseConsEnable, "%d", SafeInt(solarUseConsEnable))                                                        /* 0/1 — size the solar-pause bar from predicted consumption */ \
  X(solarConsMarginPct, "%d", SafeInt(solarConsMarginPct, 100))                                                   /* x100 — headroom over predicted consumption (%) */ \
  X(solarLearnRatePct, "%d", SafeInt(solarLearnRatePct, 100))                                                     /* x100 — per-day blend weight into performanceRatio (%) */ \
  X(rvcTxEnable, "%d", SafeInt(rvcTxEnable))                                                                      /* RV-C transmit master (0/1) */ \
  X(rvcChgrEnable, "%d", SafeInt(rvcChgrEnable))                                                                  /* RV-C CHARGER_STATUS / _2 / _3 / CONFIGURATION_STATUS (0/1) */ \
  X(rvcDcEnable, "%d", SafeInt(rvcDcEnable))                                                                      /* RV-C DC_SOURCE_STATUS_1/2/3 at the alternator instance (0/1) */ \
  X(rvcFaultEnable, "%d", SafeInt(rvcFaultEnable))                                                                /* RV-C DM_RV diagnostic message (0/1) */ \
  X(rvcChgrInstance, "%d", SafeInt(rvcChgrInstance))                                                              /* RV-C charger instance (49 = alternator type nibble + instance 1) */ \
  X(rvcDcInstance, "%d", SafeInt(rvcDcInstance))                                                                  /* RV-C DC source instance for the alternator */ \
  X(rvcDevPriority, "%d", SafeInt(rvcDevPriority))                                                                /* RV-C device priority (80 = Charger, below a BMS 120) */ \
  /* Battery + extra temperature probes, source chain, hot-charge lockout (BATTERY_TEMP_SENSORS_SPEC.md §6) */ \
  X(battTempProbeEnable, "%d", SafeInt(battTempProbeEnable))                                                      /* BATT-role probe feeds the battery-temperature source chain (0/1) */ \
  X(extraTempProbeEnable, "%d", SafeInt(extraTempProbeEnable))                                                    /* EXTRA-role probe live (0/1) */ \
  X(battTempSource, "%d", SafeInt(battTempSource))                                                                /* 0 Auto, 1 Probe, 2 NMEA 2000, 3 VE.Direct, 4 RV-C, 6 None (5 retired) */ \
  X(hotChargeLockoutEnable, "%d", SafeInt(hotChargeLockoutEnable))                                                /* hot-charge lockout master on/off (0/1) */ \
  X(MaxChargeTempF, "%d", SafeInt(MaxChargeTempF))                                                                /* hot-charge lockout ceiling (°F) */ \
  X(extraTempAlarmHiEnable, "%d", SafeInt(extraTempAlarmHiEnable))                                                /* EXTRA-probe high alarm (0/1) */ \
  X(extraTempAlarmHiF, "%d", SafeInt(extraTempAlarmHiF))                                                          /* EXTRA-probe high alarm threshold (°F) */ \
  X(extraTempAlarmLoEnable, "%d", SafeInt(extraTempAlarmLoEnable))                                                /* EXTRA-probe low alarm (0/1) */ \
  X(extraTempAlarmLoF, "%d", SafeInt(extraTempAlarmLoF))                                                          /* EXTRA-probe low alarm threshold (°F) */ \
  X(n2kExtraTempEnable, "%d", SafeInt(n2kExtraTempEnable))                                                        /* 130312 for the EXTRA probe (0/1) */ \
  X(n2kExtraTempInstance, "%d", SafeInt(n2kExtraTempInstance)) \
  X(n2kExtraTempSource, "%d", SafeInt(n2kExtraTempSource))                                                        /* tN2kTempSource code for the EXTRA probe */ \
  X(CommissionTempSrc, "%d", SafeInt(CommissionTempSrc))                                                          /* battTempActiveSrc when CommissionTempF was stamped (0 = legacy/unknown, 5 = retired board stamp) */ \
  X(maxWorkingRpm, "%d", SafeInt(maxWorkingRpm))                                                                  /* stage-9 setup screen; 0 = never entered */ \
  X(RemoteDiagnostics, "%d", SafeInt(RemoteDiagnostics))                                                          /* owner switch for the dashboard's support log-relay poll (0/1) */ \
  X(retired2, "%d", 0)                                                                                            /* dead slot, sends 0; kept so CSV3 indices never renumber */ \
  X(BatteryVoltageSource, "%d", SafeInt(BatteryVoltageSource))                                                /* charge-decision battery volts: 0 INA228, 1 NMEA 2000, 3 Victron */ \
  X(IgnoreAltCurrent, "%d", SafeInt(IgnoreAltCurrent))                                                          /* run without alternator current (0/1) — voltage-only regulation */ \
  X(IgnoreBattVMain, "%d", SafeInt(IgnoreBattVMain))                                                            /* run without the INA228 bus voltage (0/1) — fixed duty */ \
  X(IgnoreBattV2, "%d", SafeInt(IgnoreBattV2))                                                                  /* run without the ADS1115 battery voltage (0/1) */ \
  X(LimpDutyPct, "%d", SafeInt(LimpDutyPct, 10))                                                                /* field duty cap / fixed duty for the reduced-sensor tiers, % x10 */ \
  X(NoRpmCurrentA, "%d", SafeInt(NoRpmCurrentA, 10))                                                            /* alternator current limit while engine speed is ignored, A x10 */ \
  X(ShutdownRampTauS, "%d", SafeInt(ShutdownRampTauS, 100))                                                      /* shutdown ramp proportional decay time constant, s x100 */ \
  X(NoOutputAmps, "%d", SafeInt(NoOutputAmps, 100))                                                              /* alternator amps at or below this = no output, A x100 */ \
  X(FieldResTrefC, "%d", SafeInt(FieldResTrefC, 10))                                                              /* alternator temperature recorded with FieldResistance, C x10; ROLL_EMPTY = never recorded (Prerequisites gate) */ \
  X(FieldResDefault, "%d", fieldResDefaultInUse() ? 1 : 0)                                                         /* 1 = the skipped-test 2 ohm default is in use (12 V, nothing measured); the stored flag is NK_FieldResDefault */ \
  X(FieldWiring, "%d", SafeInt(FieldWiring))                                                                       /* field wiring for the drive-heat limit: 0 not set, 1 P-type, 2 N-type */ \
  X(ShuntLead, "%d", SafeInt(ShuntLead))                                                                           /* shunt location: 0 not set, 1 negative lead (IBV corrected), 2 positive lead */ \
  X(FieldWindingV, "%d", (int)FieldWindingV)                                                                       /* field winding rated voltage from Vessel Info, V */ \
  X(fieldWindApplied, "%d", (int)fieldWindApplied)                                                                 /* winding the field-side knobs are scaled for; below FieldWindingV = held on 12 V values until the owner confirms */ \
  X(sessionId, "%u", (unsigned)g_sessionId)                                                                   /* boot identity — matches CSV1_sessionId while this cached block is from the live run */ \
  X(sendMs, "%u", (unsigned)millis())                                                                             /* millis() when built; CSV3 resends at least every 60 s, */ \
  /* so an age well past ~60 s means the echo stopped */ \
  /* and these settings may be stale. */

#define CSV3_ENUM_X(name, fmt, expr) CSV3_##name,
#define CSV3_FMT_X(name, fmt, expr)  "," fmt
#define CSV3_ARG_X(name, fmt, expr)  , expr

enum Csv3Index {
  CSV3_LIST(CSV3_ENUM_X)
  CSV3_FIELD_COUNT  // enum position is authoritative — never hand-count; CSV payload specifier count must equal this +1
};
static_assert(CSV3_FIELD_COUNT == CSV3_EXPECTED_FIELDS,
              "CSV3_LIST expanded to the wrong number of fields. A lost continuation backslash "
              "truncates the enum, the format string, the argument list and the generated JS array "
              "TOGETHER and consistently, so the compress_web.sh count/order gate cannot see it. "
              "Fix the macro, or bump CSV3_EXPECTED_FIELDS if a field was genuinely added.");


enum TsIndex {
  TS_HeadingNMEA,
  TS_LatitudeNMEA,
  TS_LongitudeNMEA,
  TS_SatelliteCount,
  TS_VictronVoltage,
  TS_VictronCurrent,
  TS_AlternatorTemp,
  TS_ThermistorTemp,
  TS_RPM,
  TS_MeasuredAmps,
  TS_BatteryV,
  TS_IBV,
  TS_Bcur,
  TS_Channel3V,
  TS_DutyCycle,
  TS_FieldVolts,
  TS_FieldAmps,
  TS_CogNMEA,
  TS_SogNMEA,
  TS_AppWindSpeed,
  TS_AppWindAngle,
  TS_TrueWindSpeed,
  TS_TrueWindAngle,
  TS_Leeway,
  TS_VMG,
  TS_BaroPressure,
  TS_AmbientTemp,
  TS_IMU,
  TS_VictronSolar,  // VE.Direct solar (PPV/VPV) staleness
  TS_StwNMEA,       // Speed Through Water (SOW, PGN 128259) staleness
  TS_N2kBatt,       // received Battery Status (PGN 127508) staleness
  TS_N2kSoc,        // received DC Detailed Status (PGN 127506) staleness
  TS_Dvcc,          // DVCC follow: received charge-limit (CVL/CCL) stream staleness
  TS_WeatherFetch,  // age of the last successful solar-forecast fetch (Open-Meteo)
  TS_N183,          // NMEA 0183 serial receive staleness (any checksum-valid sentence)
  TS_BattTempProbe, // BATT-role DS18B20 staleness (IDX_BATT_TEMP_PROBE)
  TS_ExtraTemp,     // EXTRA-role DS18B20 staleness (IDX_EXTRA_TEMP)
  TS_VeBattTemp,    // VE.Direct "T" battery temperature staleness (IDX_VE_BATT_TEMP)
  TS_RvcBattTemp,   // RV-C DC_SOURCE_STATUS_2 battery temperature staleness (IDX_RVC_BATT_TEMP)
  TS_DieTemp,       // INA228 die temperature: age of the last valid sample (IDX_INA_DIE_TEMP), the field-drive heat ceiling's input
  TS_DvccVeCan,     // DVCC Victron source, VE.Can carrier: age of the last accepted limit register (IDX_DVCC_VECAN)
  TS_DvccWifi,      // DVCC Victron source, WiFi carrier: age of the last /gxLimits post (IDX_DVCC_WIFI)

  TS_sessionId,     // boot identity — matches CSV1_sessionId while this cached block is from the live run
  TS_sendMs,        // millis() when built; every other TS field is an AGE, so this is what exposes a frozen block

  TS_FIELD_COUNT  // = 44
};

float getCapCurrentForRPM(float rpm);
void saveCapCurrentTableToNVS();
void loadCapCurrentTableFromNVS();
void loadCapTablesForMode(int mode);

int SafeInt(double f, int scale = 1) {
  // where this is matters, don't move!!
  // double so AllTime accumulators don't narrow. NaN/Inf -> ROLL_EMPTY, not -1 (a real reading for signed values);
  // the browser parses anything <= -1999999999 as NaN and renders an em dash.
  return isnan(f) || isinf(f) ? ROLL_EMPTY : (int)round(f * scale);
}
// %.1f on NAN emits "nan", invalid JSON that kills the whole ledger record; emit null instead.
const char *jsonNum1(char *buf, size_t n, float v) {
  if (isnan(v) || isinf(v)) snprintf(buf, n, "null");
  else snprintf(buf, n, "%.1f", v);
  return buf;
}
void loadAPCredentials(bool forceDefaults = false) {
  if (forceDefaults) {
    esp32_ap_ssid = defaultApSsid();
    esp32_ap_password = "alternator123";
    Serial.println("Using default AP credentials for password recovery or first boot");
    return;
  }

  if (settingExists(NK_apssid)) {
    esp32_ap_ssid = settingRead(NK_apssid);
    esp32_ap_ssid.trim();
    if (esp32_ap_ssid.length() == 0) {
      esp32_ap_ssid = defaultApSsid();
    }
  } else {
    esp32_ap_ssid = defaultApSsid();
  }

  if (settingExists(NK_appass)) {
    esp32_ap_password = settingRead(NK_appass);
    esp32_ap_password.trim();
    if (esp32_ap_password.length() == 0) {
      esp32_ap_password = "alternator123";
    }
  } else {
    esp32_ap_password = "alternator123";
  }

  Serial.println("Loaded AP credentials: " + esp32_ap_ssid + " / [" + String(esp32_ap_password.length()) + " chars]");
}

void setupWiFi() {
  Serial.println("\n=== WiFi Setup Starting ===");

  // Boot straps: GPIO41 LOW = factory partition (recovery from a bad OTA); GPIO45 LOW = CONFIG mode with the alternator
  // DISABLED (settings unknown, so no charging); GPIO46 LOW = AP mode, HIGH = Client. Both run the alternator, so 46 LOW
  // is the credential-free emergency path (ALTERNATOR_WIFI-<unit id>, 192.168.4.1).

  // Harness wires (RJ3 pins 11/12), documented as read "during a restart". Read once: loop() re-enters
  // here from standby to raise the radio in the mode already chosen, and a live re-read could flip
  // currentMode and stack a second "/" under the dashboard (the route once-flags are per function).
  static bool strapsLatched = false;
  static bool forceConfigMode = false;
  static bool requestAPMode = false;
  if (!strapsLatched) {
    pinMode(45, INPUT_PULLUP);
    forceConfigMode = (digitalRead(45) == LOW);
    pinMode(46, INPUT_PULLUP);
    requestAPMode = (digitalRead(46) == LOW);
    strapsLatched = true;
  }

  if (forceConfigMode) {
    Serial.println("=== GPIO45 LOW: FORCED CONFIGURATION MODE ===");
    Serial.println("=== ALTERNATOR DISABLED FOR SAFETY - Use GPIO46 LOW for emergency operation ===");
    loadAPCredentials(true);  // Force defaults for password recovery
    accessRecoveryClear();    // same physical-access rule for the settings password (Setup > System)
    setupAccessPoint();
    setupWiFiConfigServer();  // Always serve config interface when GPIO45 is low.  This can be used to reset lost passwords
    currentMode = MODE_CONFIG;
    return;  // Exit setupWiFi() - alternator will not run
  }

  // Checked before isFirstBoot so AP mode works on first boot too, regardless of any saved credentials.

  if (requestAPMode) {
    Serial.println("=== GPIO46 LOW: OPERATIONAL AP MODE ===");
    loadAPCredentials(false);  // Load custom credentials for normal AP operation
    setupAccessPoint();
    currentMode = MODE_AP;
    CloudFeatures = 0;
    static const bool haveIndexGz = webFS.exists("/index.html.gz");   // first pass only: the wake re-entry from loop() must not read flash
    if (haveIndexGz) {
      Serial.println("Serving full alternator interface in AP mode");
      setupServer();
    } else {
      Serial.println("No index.html.gz found - serving basic landing page");
      setupCaptivePortalLanding();
    }
    return;
  }
  bool isFirstBoot = !settingExists(NK_first_config_done);

  if (isFirstBoot) {
    Serial.println("=== FIRST BOOT DETECTED: FORCING CONFIGURATION MODE ===");
    Serial.println("User must configure credentials before accessing interface");
    loadAPCredentials(true);  // Use defaults so user can learn them
    setupAccessPoint();
    setupWiFiConfigServer();
    currentMode = MODE_CONFIG;
    return;
  }

  bool hasClientConfig = settingExists(NK_ssid);

  if (hasClientConfig) {
    String ssid = settingRead(NK_ssid);
    String pass = settingRead(NK_pass);
    ssid.trim();
    pass.trim();

    strncpy(cached_wifi_ssid, ssid.c_str(), sizeof(cached_wifi_ssid) - 1);
    cached_wifi_ssid[sizeof(cached_wifi_ssid) - 1] = '\0';

    strncpy(cached_wifi_pass, pass.c_str(), sizeof(cached_wifi_pass) - 1);
    cached_wifi_pass[sizeof(cached_wifi_pass) - 1] = '\0';

    cached_wifi_creds_valid = (strlen(cached_wifi_ssid) > 0);
  } else {
    cached_wifi_ssid[0] = '\0';
    cached_wifi_pass[0] = '\0';
    cached_wifi_creds_valid = false;
  }

  Serial.println("=== GPIO46 HIGH: CLIENT MODE REQUESTED ===");

  if (!hasClientConfig || !cached_wifi_creds_valid || strlen(cached_wifi_ssid) == 0) {
    Serial.println("=== NO CLIENT CREDENTIALS: ENTERING CONFIGURATION MODE ===");
    Serial.println(!hasClientConfig ? "Reason: No saved credentials file" : "Reason: Empty SSID");
    loadAPCredentials(false);  // Use custom AP credentials for config mode
    setupAccessPoint();
    setupWiFiConfigServer();  // Serve WiFi configuration interface
    currentMode = MODE_CONFIG;
    return;
  }

  // Normal operation - load custom AP credentials in case we need them later
  loadAPCredentials(false);

  // Attempt client connection with timeout (safe for 15s watchdog)
  if (connectToWiFi(cached_wifi_ssid, cached_wifi_pass, 10000)) {
    currentMode = MODE_CLIENT;
    Serial.println("=== CLIENT MODE SUCCESS ===");
    Serial.println("WiFi connected successfully!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    setupServer();
  } else {
    Serial.println("=== CLIENT MODE FAILED ===");
    Serial.println("WiFi connection failed - will retry periodically (no AP fallback)");
    currentMode = MODE_CLIENT;  // Keep trying client mode
    // No automatic AP fallback - let intelligent reconnection logic handle it
  }


  Serial.println("=== WiFi Setup Complete ===");
}

// ── Per-unit identity ───────────────────────────────────────────────────────────────
// Several regulators can share a LAN and all answer to alternator.local (whichever wins the resolver race), so
// everything below derives from the eFuse UID. alternator.local stays for single-regulator installs and tooling.

// Six hex of the UID. device_id_hex prints getEfuseMac() big-endian over little-endian-packed MAC bytes, so the string
// is the MAC reversed: its TAIL is the OUI shared by every module. Take the six after the always-zero high half.
const char *regulatorUid6() {
  static char u6[7] = "";
  if (!u6[0]) {
    size_t n = strlen(device_id_hex);
    const char *src = (n >= 12) ? device_id_hex + (n - 12) : device_id_hex;
    strncpy(u6, src, 6);
    u6[6] = '\0';
  }
  return u6;
}

// A hostname that can only reach this board: xreg-04a7ac.local.
const char *regulatorHostName() {
  static char h[16] = "";
  if (!h[0]) {
    snprintf(h, sizeof(h), "xreg-%s", regulatorUid6());
    for (char *p = h; *p; p++) *p = (char)tolower((unsigned char)*p);
  }
  return h;
}

// Never empty: an unnamed board must still be distinguishable from the one next to it.
String regulatorDisplayName() {
  return REGULATOR_NAME[0] ? String(REGULATOR_NAME) : (String("XREG-") + regulatorUid6());
}

// Per-unit so two unprovisioned units in range (a twin-engine install) don't broadcast the same SSID.
String defaultApSsid() {
  return String("ALTERNATOR_WIFI-") + regulatorUid6();
}

// The friendly label rides a TXT record, not the instance name: the instance name has to stay
// PREDICTABLE (Android's NsdManager matches services by instance name at browse time, before
// any TXT is available), so it is the hostname and a rename never re-announces the service.
static bool mdnsStarted = false;   // true once MDNS.begin() has succeeded (startMdnsOnce); every raw mdns_* call is gated on it
static void mdnsRefreshPair();     // defined with the pair list below; startMdnsOnce publishes the list once the service exists
static void mdnsRefreshName() {
  if (!mdnsStarted) return;   // the IDF service call takes a mutex that only mdns_init creates
  mdns_service_txt_item_set("_http", "_tcp", "name", regulatorDisplayName().c_str());
  mdns_service_txt_item_set("_xreg", "_tcp", "name", regulatorDisplayName().c_str());
}

// alternator.local is a DELEGATED name: the primary hostname must be unique because it is the SRV target, or resolving
// unit B's service lands on unit A. A delegated name has no netif binding, so republish whenever the address moves.
void mdnsPublishIdentity() {
  IPAddress ip = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP() : WiFi.softAPIP();
  if ((uint32_t)ip == 0) return;
  static uint32_t published = 0;
  if (published == (uint32_t)ip) return;
  mdns_ip_addr_t a = {};
  a.addr.type = ESP_IPADDR_TYPE_V4;
  a.addr.u_addr.ip4.addr = (uint32_t)ip;
  a.next = nullptr;
  esp_err_t e = published ? mdns_delegate_hostname_set_address("alternator", &a)
                          : mdns_delegate_hostname_add("alternator", &a);
  if (e == ESP_OK) {
    published = (uint32_t)ip;
    Serial.printf("mDNS: %s.local + alternator.local -> %s\n", regulatorHostName(), ip.toString().c_str());
  } else {
    Serial.printf("mDNS: alternator.local delegation failed (%d) — this unit answers only to %s.local\n",
                  (int)e, regulatorHostName());
  }
  mdnsRefreshName();
}

// mDNS starts once and never MDNS.end(): ESP32 core 3.3.8's mdns teardown null-derefs the
// netif (LoadProhibited crash on wake/reconnect). mDNS stays bound to the persistent STA
// netif across reconnects, so it keeps working without a restart.
void startMdnsOnce() {
  if (!mdnsStarted && MDNS.begin(regulatorHostName())) {
    Serial.println("mDNS responder started");
    MDNS.setInstanceName(regulatorHostName());   // must precede addService — it names the instance
    MDNS.addService("http", "tcp", 80);
    MDNS.addServiceTxt("http", "tcp", "uid", (const char *)device_id_hex);   // cast: char[] makes the char*/const char* overloads ambiguous
    MDNS.addServiceTxt("http", "tcp", "host", regulatorHostName());
    MDNS.addServiceTxt("http", "tcp", "fw", FIRMWARE_VERSION);
    MDNS.addServiceTxt("http", "tcp", "name", regulatorDisplayName().c_str());
    // Private service type for the peer browse (peerBrowseOnce): a marina network's _http._tcp is
    // full of printers and TVs and a browse caps at 20 answers. Same TXT as the http service.
    MDNS.addService("xreg", "tcp", 80);
    MDNS.addServiceTxt("xreg", "tcp", "uid", (const char *)device_id_hex);
    MDNS.addServiceTxt("xreg", "tcp", "host", regulatorHostName());
    MDNS.addServiceTxt("xreg", "tcp", "fw", FIRMWARE_VERSION);
    MDNS.addServiceTxt("xreg", "tcp", "name", regulatorDisplayName().c_str());
    mdnsStarted = true;
    mdnsRefreshPair();   // "pair" TXT on both services: the uid6 list of this boat's other regulators
  }
  if (mdnsStarted) mdnsPublishIdentity();   // address may have moved since the last call
}

// ── Access: settings password, paired regulators, network peers ─────────────────────────────────
// With the password on, /armSettings needs pw= and returns a token; mutating requests carry tok= (settingsArmActive), so
// an unlock belongs to one client. Salted SHA-256 only; 5 wrong tries = 60 s lockout; a pin-11 recovery boot clears it.
static void copyField(char *dst, size_t n, const String &src) {
  strncpy(dst, src.c_str(), n - 1);
  dst[n - 1] = '\0';
}

static void sha256Hex(const char *salt, const char *pw, char out[65]) {
  String in = String(salt) + String(pw);
  unsigned char d[32];
  mbedtls_sha256((const unsigned char *)in.c_str(), in.length(), d, 0);
  for (int i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", d[i]);
  out[64] = '\0';
}

bool pwVerify(const char *pw) {
  if (!pwRequired) return true;
  if (!pw || !*pw) return false;
  char h[65];
  sha256Hex(pwSaltHex, pw, h);
  uint8_t diff = 0;   // constant-time: an early exit would leak how much of the hash matched
  for (int i = 0; i < 64; i++) diff |= (uint8_t)(h[i] ^ pwHashHex[i]);
  return diff == 0;
}

void armTokensClear() {
  for (int i = 0; i < ARM_TOKENS; i++) armTok[i][0] = '\0';
}

const char *armTokenIssue() {
  char *t = armTok[armTokNext];
  armTokNext = (uint8_t)((armTokNext + 1) % ARM_TOKENS);
  snprintf(t, 17, "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
  return t;
}

bool armTokenValid(AsyncWebServerRequest *request) {
  if (!request || !request->hasParam("tok")) return false;
  const String &t = request->getParam("tok")->value();
  if (t.length() != 16) return false;
  for (int i = 0; i < ARM_TOKENS; i++)
    if (armTok[i][0] && t.equals(armTok[i])) return true;
  return false;
}

bool pwStore(const char *pw) {
  char salt[17], h[65];
  snprintf(salt, sizeof(salt), "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
  sha256Hex(salt, pw, h);
  // Requirement off first, on last: salt and hash commit separately, and a cut between them left a new salt beside the
  // old hash, a complete-looking record no password verifies. Interrupted this way, the next boot has no password.
  if (!settingWrite(NK_pwReq, "0") || !settingWrite(NK_pwSalt, salt) || !settingWrite(NK_pwHash, h)
      || !settingWrite(NK_pwReq, "1")) return false;
  strcpy(pwSaltHex, salt);
  strcpy(pwHashHex, h);
  pwRequired = true;
  armTokensClear();   // every unlocked page re-arms with the new password
  return true;
}

void pwClear() {
  settingWrite(NK_pwReq, "0");
  settingRemove(NK_pwHash);
  settingRemove(NK_pwSalt);
  pwRequired = false;
  pwSaltHex[0] = '\0';
  pwHashHex[0] = '\0';
  armTokensClear();
}

void accessRecoveryClear() {
  if (settingExists(NK_pwReq) && settingRead(NK_pwReq).toInt() == 1) {
    pwClear();
    Serial.println("ACCESS: settings password cleared (pin 11 recovery boot)");
  }
}

// Paired regulators (Setup > System > Regulators on this boat), stored as uid<US>name records joined by <RS>. The
// dashboard writes both units' lists, so neither firmware trusts a claim made by the other.
#define PAIR_MAX 8
struct PairedUnit { char uid[17]; char name[33]; };
static PairedUnit (&pairList)[PAIR_MAX] = PsramPlace::make<PairedUnit[PAIR_MAX]>();
static uint8_t pairCount = 0;
static const char PAIR_RS = '\x1e', PAIR_US = '\x1f';

static void pairListLoad() {
  pairCount = 0;
  String s = settingRead(NK_pairList);
  int pos = 0;
  while (pos < (int)s.length() && pairCount < PAIR_MAX) {
    int end = s.indexOf(PAIR_RS, pos);
    if (end < 0) end = s.length();
    String rec = s.substring(pos, end);
    int us = rec.indexOf(PAIR_US);
    String uid = us < 0 ? rec : rec.substring(0, us);
    String name = us < 0 ? String() : rec.substring(us + 1);
    if (uid.length() == 16) {
      copyField(pairList[pairCount].uid, sizeof(pairList[pairCount].uid), uid);
      copyField(pairList[pairCount].name, sizeof(pairList[pairCount].name), name);
      pairCount++;
    }
    pos = end + 1;
  }
}

static bool pairListSave() {
  String s;
  for (int i = 0; i < pairCount; i++) {
    if (i) s += PAIR_RS;
    s += pairList[i].uid;
    s += PAIR_US;
    s += pairList[i].name;
  }
  return settingWrite(NK_pairList, s.c_str());
}

// Six-character form of a paired uid: the same characters regulatorUid6() picks for this unit.
static void uid6Of(const char *uid16, char out[7]) {
  size_t n = strlen(uid16);
  const char *src = (n >= 12) ? uid16 + (n - 12) : uid16;
  strncpy(out, src, 6);
  out[6] = '\0';
  for (char *c = out; *c; c++) *c = (char)tolower((unsigned char)*c);
}

static void pairListUid6Csv(char *out, size_t n) {
  out[0] = '\0';
  for (int i = 0; i < pairCount; i++) {
    char u6[7];
    uid6Of(pairList[i].uid, u6);
    size_t len = strlen(out);
    snprintf(out + len, n - len, "%s%s", i ? "," : "", u6);
  }
}

// The pair list rides the mDNS TXT of both services, so any unit's browse can group the boat's
// regulators together without asking each one.
static void mdnsRefreshPair() {
  if (!mdnsStarted) return;
  char csv[PAIR_MAX * 7 + 1];
  pairListUid6Csv(csv, sizeof(csv));
  mdns_service_txt_item_set("_http", "_tcp", "pair", csv);
  mdns_service_txt_item_set("_xreg", "_tcp", "pair", csv);
}

static bool uidIsHex16(const char *u) {
  if (!u || strlen(u) != 16) return false;
  for (int i = 0; i < 16; i++)
    if (!isxdigit((unsigned char)u[i])) return false;
  return true;
}

bool pairListAdd(const char *uid, const char *name) {
  if (!uidIsHex16(uid) || strcasecmp(uid, device_id_hex) == 0) return false;
  int k = -1;
  for (int i = 0; i < pairCount; i++)
    if (strcasecmp(pairList[i].uid, uid) == 0) k = i;
  if (k < 0) {
    if (pairCount >= PAIR_MAX) return false;
    k = pairCount++;
    for (int i = 0; i < 16; i++) pairList[k].uid[i] = (char)toupper((unsigned char)uid[i]);
    pairList[k].uid[16] = '\0';
  }
  copyField(pairList[k].name, sizeof(pairList[k].name), String(name ? name : ""));
  bool ok = pairListSave();
  mdnsRefreshPair();
  return ok;
}

bool pairListRemove(const char *uid) {
  if (!uid) return false;
  int k = -1;
  for (int i = 0; i < pairCount; i++)
    if (strcasecmp(pairList[i].uid, uid) == 0) k = i;
  if (k < 0) return true;   // already absent: the remove is satisfied
  for (int i = k; i + 1 < pairCount; i++) pairList[i] = pairList[i + 1];
  pairCount--;
  bool ok = pairListSave();
  mdnsRefreshPair();
  return ok;
}

void pairListAppendJson(String &out) {
  out += '[';
  for (int i = 0; i < pairCount; i++) {
    if (i) out += ',';
    out += "{\"uid\":\"";
    out += pairList[i].uid;
    out += "\",\"name\":";
    cfgAppendJsonStr(out, String(pairList[i].name));
    out += '}';
  }
  out += ']';
}

// Victron GX writes: a request carrying gx=1 comes from the GX bridge. Its /get writes skip Unlock Controls (anyone at
// the GX screen may change settings) and never read or write settingsArmed or an armTok.
bool gxFlagged(AsyncWebServerRequest *request) {
  const AsyncWebParameter *p = request->hasParam("gx", true) ? request->getParam("gx", true) : request->getParam("gx");
  return p && p->value().toInt() == 1;
}

// What a GX may not do: commissioning (out of scope on the GX), firmware updates, and the Vessel Info field
// winding (FieldWindingV / FieldWindingAck answer its resistance check). /saveVesselInfo and /fieldResTest refuse a GX
// request through settingsArmActive. Stop and cancel stay allowed, so a GX can always end anything; the flagged
// entries refuse only a nonzero (start) value.
bool gxParamDenied(AsyncWebServerRequest *request) {
  static const char *const DENY[] = {
    "UpdateToVersion", "FieldWindingV", "FieldWindingAck",
    "commissionStart", "commissionPhase", "commissionStageDone", "commissionStageManual", "commissionStageSkip",
    "commissionDone", "commissionHeartbeat",
    "startSystemID", "startTuningSweep", "restartCVTest",
    "startFieldCurve", "startKneeSweep", "applyKneeCurve", "addKneeAnchor", "clearKneeAnchors",
    "chcStart", "cvPlantFitStart", "fieldCutStart", "cvStressStart", "ripFitAlt", "slpFitAlt", "capBothSave",
  };
  static const char *const DENY_NONZERO[] = { "faCommissionGate", "TuningMode", "CVTuningMode",
                                              "resTest", "bcurRtest", "ripGameFill" };
  for (size_t i = 0; i < sizeof(DENY) / sizeof(DENY[0]); i++)
    if (request->hasParam(DENY[i])) return true;
  for (size_t i = 0; i < sizeof(DENY_NONZERO) / sizeof(DENY_NONZERO[0]); i++)
    if (request->hasParam(DENY_NONZERO[i]) && request->getParam(DENY_NONZERO[i])->value().toInt() != 0) return true;
  return false;
}

#define GX_STREAM_MAX 4
struct GxStreamRec { const void *client; char ip[16]; uint32_t atMs; };
// Written by gxEvents' connect/disconnect callbacks and read by GET /gxStatus, all on async_tcp.
static GxStreamRec *gxStreamRecs = nullptr;

static void gxStreamAdd(const void *client, const char *ip) {
  if (!gxStreamRecs) gxStreamRecs = (GxStreamRec *)ps_calloc(GX_STREAM_MAX, sizeof(GxStreamRec));
  if (!gxStreamRecs) return;
  for (int i = 0; i < GX_STREAM_MAX; i++) {
    if (!gxStreamRecs[i].client) {
      gxStreamRecs[i].client = client;
      snprintf(gxStreamRecs[i].ip, sizeof(gxStreamRecs[i].ip), "%s", ip ? ip : "");
      gxStreamRecs[i].atMs = millis();
      return;
    }
  }
}

// By pointer value only: the library hands the disconnecting client "for reference purposes".
static void gxStreamRemove(const void *client) {
  if (!gxStreamRecs) return;
  for (int i = 0; i < GX_STREAM_MAX; i++)
    if (gxStreamRecs[i].client == client) memset(&gxStreamRecs[i], 0, sizeof(GxStreamRec));
}

static void gxStatusAppendJson(String &out, uint32_t now) {
  out += "{\"streams\":[";
  bool first = true;
  for (int i = 0; gxStreamRecs && i < GX_STREAM_MAX; i++) {
    const GxStreamRec &s = gxStreamRecs[i];
    if (!s.client) continue;
    if (!first) out += ',';
    first = false;
    out += "{\"ip\":\"";
    out += s.ip;
    out += "\",\"upS\":";
    out += String((now - s.atMs) / 1000UL);
    out += '}';
  }
  out += "]}";
}

// ── Victron GX stream (/gxevents) ───────────────────────────────────────────────────────────────
// The GX gets the phone's five frames, byte for byte, thinned to its own schedule. Core 1 brackets each frame build with a
// sequence bump (odd = writing) and offers finished frames; the Core-0 sender copies one out, re-checks the sequence and
// sends it, so loop() never waits on lwIP for the GX. Latest frame wins: nothing queues behind a slow GX.
// 0-4 are SendWifiData's channels; 5-9 are the schema-zipped side streams built in 7_functions.ino (gxSideOffer).
#define GX_CH_COUNT 10
#define GX_CH_ALTLIVE 5
#define GX_CH_PERFLIVE 6
#define GX_CH_MOTORLIVE 7
#define GX_CH_ALTSET 8
#define GX_CH_PERFSET 9
static const char *const GX_EVT[GX_CH_COUNT] = { "CSVData", "CSVData4", "CSVData2", "CSVData3", "TimestampData",
                                                 "AltLive", "PerfLive", "MotorLive", "AltSettings", "PerfSettings" };
// Rows: 0 engine running (or a GX page asked for live data), 1 ignition on with the engine stopped, 2 ignition-off WiFi nap.
// The side streams are built at 1 Hz (live) and every 5 s (settings) whatever the schedule; a row only thins them.
static const uint32_t GX_IV[3][GX_CH_COUNT] = { { 500, 1000, 10000, 60000, 6000, 1000, 1000, 1000, 5000, 5000 },
                                                { 1000, 1000, 30000, 120000, 30000, 1000, 1000, 1000, 30000, 30000 },
                                                { 10000, 30000, 60000, 300000, 60000, 30000, 30000, 30000, 60000, 60000 } };
#define GX_SIDE_SIZE 512          // >= the largest side-stream buffer (AltLive, 512)
// The side-stream builders tick on their own clocks, so a frame can land a little short of an interval equal to its
// build period; without this slack the GX would get every other one.
#define GX_SIDE_SLACK_MS 250UL
static const char *const GX_TIER_NAME[3] = { "run", "stopped", "nap" };
#define GX_RUN_HOLD_MS 30000UL    // the running schedule holds this long after the engine and field stop
#define GX_FAST_HOLD_MS 60000UL   // GET /gxrate?fast=1 holds the running schedule this long (fast=2 the same, CSV1 at the web app's rate)
// fast=2: CSV1 no faster than this whatever webgaugesinterval says; the GX bridge parses every frame and the GX screen
// redraws on it (VICTRON_GX_BENCH.md §9a, "GX live data at the web app's rate")
#define GX_LIVE_FLOOR_MS 100UL
#define GX_FLOW_HOLD_MS 60000UL   // GET /gxrate?flow=1 holds CSV4 at GX_FLOW_CSV4_MS this long (the GX power-flow tile)
#define GX_FLOW_CSV4_MS 1000UL
#define GX_STAGE_SIZE 8192        // >= PAYLOAD2_SIZE and PAYLOAD3_SIZE, the largest frames
// Unacked frames on the GX link at which the sender waits (the frame stays owed); a connect sends a burst of every
// channel, so the floor sits above a healthy burst.
static const uint16_t GX_BACKLOG_DEPTH = 8;
static uint32_t gxSeq[GX_CH_COUNT];
static const char *gxBuf[GX_CH_COUNT];
static int gxLen[GX_CH_COUNT];          // -1 = the last build failed its length guard
static uint8_t gxWant[GX_CH_COUNT];     // 1 = the sender owes the GX this channel's current frame
static uint32_t gxLastOfferMs[GX_CH_COUNT];
static bool gxKick[GX_CH_COUNT];        // Core 1 only: offer this channel at once (a GX just connected)
static volatile bool gxKickPending = false;
// Kept by gxEvents' connect/disconnect callbacks (async_tcp). Core 1 reads this, never gxEvents.count(): the Core-0
// sender holds that lock across its blocking lwIP send, at priority 1 where anything on Core 0 can preempt it.
static volatile uint8_t gxStreamCount = 0;
static volatile uint8_t gxTier = 1;
static volatile uint32_t gxFastUntilMs = 0;   // 0 = no fast hold
static volatile bool gxFastLive = false;      // the fast hold came as fast=2: CSV1 at the web app's rate too
static volatile uint32_t gxFlowUntilMs = 0;   // 0 = no flow hold
static uint32_t gxRunSeenMs = 0;
static TaskHandle_t gxSendTaskHandle = nullptr;
static char *gxStage = nullptr;
static char *gxSide = nullptr;          // PSRAM copies of the side streams (built in stack buffers), GX_SIDE_SIZE each
static bool gxLiveNow = false;          // Core 1: a GX stream is up and the sender runs (refreshed every SendWifiData pass)

// The GX's device readings (POST /gxDevices, GX link 4) for the web app's Live Data > Integrations. RAM only, never NVS.
// The handler (gxDevicesHandle, 4_functions.ino, async_tcp) is the only writer: it re-serializes the bridge's list into
// gxDevText between two gxDevSeq increments (odd = mid-write). SendWifiData copies it out on Core 1 and checks the
// sequence again, so neither side ever waits on the other. A demo switch (Core 1) only raises gxDevClearSeq: data at or
// below it belongs to the other boat and reads as none.
#define GXDEV_TEXT_SIZE 4096
#define GXDEV_MAX 16
#define GXDEV_EVERY_WATCHED_S 5      // the post interval asked for while a dashboard is connected
#define GXDEV_EVERY_IDLE_S 30        // with none
static char *gxDevText = nullptr;       // PSRAM: the "d" array as JSON
static uint16_t gxDevLen = 0;
static uint32_t gxDevAtMs = 0;          // millis() of the post (never 0 once one arrived)
static uint8_t gxDevEveryS = 0;         // the interval that post was told
static uint32_t gxDevSeq = 0;
static uint32_t gxDevClearSeq = 0;      // Core 1
static volatile bool gxDevKick = false; // a dashboard connected: send GxDevices at once
// A dashboard connected: its first CSV2 and TS go on the next free passes, not up to 5 s / 3 s later, so a card that needs
// a CSV2 state and a TS age (Charge-Limit Follow) is right after a reload. Built as usual; the 5 s / 3 s clock restarts.
static volatile bool csv2Kick = false;
static volatile bool tsKick = false;
static volatile bool phoneLiveNow = false;   // Core 1, every SendWifiData pass: a dashboard stream is connected

// Core 1, at a demo switch: the readings on hand are the other boat's until the next post.
void gxDevicesForget() {
  gxDevClearSeq = __atomic_load_n(&gxDevSeq, __ATOMIC_ACQUIRE) | 1u;
  gxDevKick = true;
}

static bool gxFastActive(uint32_t now) {
  const uint32_t until = gxFastUntilMs;
  return until && (int32_t)(until - now) > 0;
}

static uint8_t gxTierEffective(uint32_t now) { return gxFastActive(now) ? 0 : gxTier; }

static bool gxFlowActive(uint32_t now) {
  const uint32_t until = gxFlowUntilMs;
  return until && (int32_t)(until - now) > 0;
}

// 0 none, 1 fast=1, 2 fast=2
static uint8_t gxFastLevel(uint32_t now) { return gxFastActive(now) ? (gxFastLive ? 2 : 1) : 0; }

// The channel's GX interval now: the tier's, with CSV4 no slower than GX_FLOW_CSV4_MS under a flow hold, and CSV1 at
// webgaugesinterval (never under GX_LIVE_FLOOR_MS, never slower than the running schedule) under a fast=2 hold.
static uint32_t gxIv(uint8_t ch, uint32_t now) {
  const uint32_t iv = GX_IV[gxTierEffective(now)][ch];
  if (ch == 0 && gxFastLevel(now) == 2) {
    const uint32_t web = (uint32_t)webgaugesinterval;
    const uint32_t live = web > GX_LIVE_FLOOR_MS ? web : GX_LIVE_FLOOR_MS;
    return live < iv ? live : iv;
  }
  return (ch == 1 && iv > GX_FLOW_CSV4_MS && gxFlowActive(now)) ? GX_FLOW_CSV4_MS : iv;
}

#define GX_RATE_JSON_SIZE 320
// devPost: the POST /gxDevices interval wanted now, ms (the answer's everyS), so the bridge moves the moment a dashboard
// connects or the last one leaves instead of at its next post's answer
void gxRateJson(char *out, size_t n) {
  const uint32_t now = millis();
  const uint8_t t = gxTierEffective(now);
  uint32_t iv[GX_CH_COUNT];
  for (uint8_t ch = 0; ch < GX_CH_COUNT; ch++) iv[ch] = gxIv(ch, now);
  snprintf(out, n, "{\"tier\":\"%s\",\"fast\":%d,\"flow\":%d,\"csv1\":%lu,\"csv4\":%lu,\"csv2\":%lu,\"csv3\":%lu,\"ts\":%lu,"
                   "\"altLive\":%lu,\"perfLive\":%lu,\"motorLive\":%lu,\"altSettings\":%lu,\"perfSettings\":%lu,\"devPost\":%lu}",
           GX_TIER_NAME[t], (int)gxFastLevel(now), gxFlowActive(now) ? 1 : 0, (unsigned long)iv[0], (unsigned long)iv[1],
           (unsigned long)iv[2], (unsigned long)iv[3], (unsigned long)iv[4], (unsigned long)iv[5], (unsigned long)iv[6],
           (unsigned long)iv[7], (unsigned long)iv[8], (unsigned long)iv[9],
           (unsigned long)(phoneLiveNow ? GXDEV_EVERY_WATCHED_S : GXDEV_EVERY_IDLE_S) * 1000UL);
}

// level: 0 releases, 1 the running schedule, 2 that and CSV1 at the web app's rate
static void gxFastHold(uint8_t level) {
  gxFastLive = level >= 2;
  gxFastUntilMs = level ? ((millis() + GX_FAST_HOLD_MS) | 1UL) : 0;
  if (gxSendTaskHandle) xTaskNotifyGive(gxSendTaskHandle);   // a changed schedule goes out as GxRate
}
static void gxFlowHold(bool on) {
  gxFlowUntilMs = on ? ((millis() + GX_FLOW_HOLD_MS) | 1UL) : 0;
  if (gxSendTaskHandle) xTaskNotifyGive(gxSendTaskHandle);
}

// Core 1, every SendWifiData pass.
static void gxTierUpdate(uint32_t now) {
  if (g_engineSpinningNow || fieldActiveStatus != 0) gxRunSeenMs = now ? now : 1;
  const uint8_t t = (gxRunSeenMs && now - gxRunSeenMs < GX_RUN_HOLD_MS) ? 0 : (wifiNapActive ? 2 : 1);
  if (t != gxTier) {
    gxTier = t;
    if (gxSendTaskHandle) xTaskNotifyGive(gxSendTaskHandle);
  }
}

static inline void gxBeginWrite(uint8_t ch) {
  __atomic_add_fetch(&gxSeq[ch], 1, __ATOMIC_SEQ_CST);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
}
// Called right after the build, before its length guard can return, so the sequence is always left even.
static inline void gxEndWrite(uint8_t ch, const char *buf, int len, size_t cap) {
  __atomic_store_n(&gxBuf[ch], buf, __ATOMIC_RELAXED);
  __atomic_store_n(&gxLen[ch], (len > 0 && (size_t)len < cap && len < GX_STAGE_SIZE) ? len : -1, __ATOMIC_RELAXED);
  __atomic_add_fetch(&gxSeq[ch], 1, __ATOMIC_RELEASE);
}

// GX-only frame gate: the GX's own interval, or at once after a connect.
static inline bool gxDue(uint8_t ch, uint32_t now, uint32_t last, bool gxLive) {
  return gxLive && (gxKick[ch] || now - last >= gxIv(ch, now));
}

// force: the frame was built for the GX (no phone) or carries a settings change; otherwise a phone-paced frame is
// passed on only at the GX's interval.
static void gxOffer(uint8_t ch, uint32_t now, bool gxLive, bool force) {
  if (!gxLive || !gxSendTaskHandle) return;
  if (!force && !gxKick[ch] && now - gxLastOfferMs[ch] < gxIv(ch, now)) return;
  gxLastOfferMs[ch] = now;
  gxKick[ch] = false;
  __atomic_store_n(&gxWant[ch], 1, __ATOMIC_RELEASE);
  xTaskNotifyGive(gxSendTaskHandle);
}

// Side streams (AltLive, PerfLive, MotorLive, AltSettings, PerfSettings): the builder's stack buffer is copied into the
// channel's PSRAM slot under the sequence. Core 1 only, so each channel keeps one writer (a /get handler on Core 0 also
// sends the settings echoes; those reach the phone only, and the GX gets the next periodic one).
void gxSideOffer(uint8_t ch, const char *buf, int len) {
  if (!gxLiveNow || !gxSide || ch < GX_CH_ALTLIVE || ch >= GX_CH_COUNT || xPortGetCoreID() != 1) return;
  if (len <= 0 || len >= GX_SIDE_SIZE) return;
  const uint32_t now = millis();
  if (!gxKick[ch] && now - gxLastOfferMs[ch] + GX_SIDE_SLACK_MS < gxIv(ch, now)) return;
  char *slot = gxSide + (size_t)(ch - GX_CH_ALTLIVE) * GX_SIDE_SIZE;
  gxBeginWrite(ch);
  memcpy(slot, buf, (size_t)len);
  gxEndWrite(ch, slot, len, GX_SIDE_SIZE);
  gxOffer(ch, now, true, true);
}

#define GX_COPY_OK 0
#define GX_COPY_NONE 1   // no usable frame (never built, or the last build failed its guard)
#define GX_COPY_BUSY 2   // Core 1 kept rebuilding it; the newer frame is offered on the next try
static uint8_t gxCopyFrame(uint8_t ch) {
  for (int attempt = 0; attempt < 3; attempt++) {
    const uint32_t s1 = __atomic_load_n(&gxSeq[ch], __ATOMIC_ACQUIRE);
    if (s1 & 1u) {
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    const char *buf = __atomic_load_n(&gxBuf[ch], __ATOMIC_RELAXED);
    const int len = __atomic_load_n(&gxLen[ch], __ATOMIC_RELAXED);
    if (!buf || len <= 0 || len >= GX_STAGE_SIZE) return GX_COPY_NONE;
    memcpy(gxStage, buf, (size_t)len);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&gxSeq[ch], __ATOMIC_RELAXED) == s1) {
      gxStage[len] = '\0';
      return GX_COPY_OK;
    }
    gxFramesTorn++;
  }
  return GX_COPY_BUSY;
}

static void gxSendTask(void *) {
  uint8_t sentTier = 0xFF, sentFast = 0xFF, sentFlow = 0xFF, sentPhone = 0xFF;
  uint32_t sentCsv1 = 0;
  for (;;) {
    bool owed = false;
    for (uint8_t ch = 0; ch < GX_CH_COUNT; ch++) owed |= __atomic_load_n(&gxWant[ch], __ATOMIC_ACQUIRE) != 0;
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(owed ? 100 : 1000));
    if (otaInProgress) continue;
    if (gxEvents.count() == 0) {
      // A connecting client is counted (onConnect) before the library lists it: its kick burst stays owed.
      if (gxStreamCount == 0)
        for (uint8_t ch = 0; ch < GX_CH_COUNT; ch++) __atomic_store_n(&gxWant[ch], 0, __ATOMIC_RELEASE);
      continue;
    }
    const uint32_t now = millis();
    const uint8_t tier = gxTierEffective(now), fast = gxFastLevel(now), flow = gxFlowActive(now) ? 1 : 0;
    const uint8_t phone = phoneLiveNow ? 1 : 0;
    const uint32_t csv1 = gxIv(0, now);   // under fast=2 it follows webgaugesinterval, which the owner can change
    if (tier != sentTier || fast != sentFast || flow != sentFlow || phone != sentPhone || csv1 != sentCsv1) {
      char rate[GX_RATE_JSON_SIZE];
      gxRateJson(rate, sizeof(rate));
      if (gxEvents.send(rate, "GxRate") != AsyncEventSource::ENQUEUED) gxFramesDropped++;
      sentTier = tier;
      sentFast = fast;
      sentFlow = flow;
      sentPhone = phone;
      sentCsv1 = csv1;
    }
    for (uint8_t ch = 0; ch < GX_CH_COUNT; ch++) {
      if (!__atomic_load_n(&gxWant[ch], __ATOMIC_ACQUIRE)) continue;
      if (gxEvents.avgPacketsWaiting() >= GX_BACKLOG_DEPTH) {
        gxFramesHeld++;
        break;
      }
      // Cleared before the copy, so an offer that lands during it is not lost.
      __atomic_store_n(&gxWant[ch], 0, __ATOMIC_RELEASE);
      const uint8_t got = gxCopyFrame(ch);
      if (got == GX_COPY_BUSY) __atomic_store_n(&gxWant[ch], 1, __ATOMIC_RELEASE);
      if (got != GX_COPY_OK) continue;
      if (gxEvents.send(gxStage, GX_EVT[ch]) == AsyncEventSource::ENQUEUED) gxFramesSent++;
      else gxFramesDropped++;
    }
  }
}

void startGxSendTask() {
  if (gxSendTaskHandle) return;
  if (!gxStage) gxStage = (char *)ps_malloc(GX_STAGE_SIZE);
  if (!gxSide) gxSide = (char *)ps_malloc((size_t)(GX_CH_COUNT - GX_CH_ALTLIVE) * GX_SIDE_SIZE);
  if (!gxStage || !gxSide) {
    Serial.println("GX stream: staging buffer allocation failed, /gxevents carries no frames this boot");
    return;
  }
  // Priority 1 like TempTask and HTTPS; never retires (idles through an OTA), so it is not in ensureCore0Tasks().
  if (xTaskCreatePinnedToCoreWithCaps(gxSendTask, "gxSend", 6144, NULL, 1, &gxSendTaskHandle, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
    gxSendTaskHandle = nullptr;
    Serial.println("GX stream: sender task create failed, /gxevents carries no frames this boot");
    return;
  }
  Serial.println("GX stream sender task created on Core 0 (prio 1, PSRAM stack)");
}

void accessInit() {
  String req = settingRead(NK_pwReq);
  copyField(pwSaltHex, sizeof(pwSaltHex), settingRead(NK_pwSalt));
  copyField(pwHashHex, sizeof(pwHashHex), settingRead(NK_pwHash));
  pwRequired = (req.toInt() == 1) && strlen(pwSaltHex) == 16 && strlen(pwHashHex) == 64;
  if (req.toInt() == 1 && !pwRequired) {
    settingWrite(NK_pwReq, "0");   // half a record (interrupted store) must not lock the owner out
    Serial.println("ACCESS: settings-password record incomplete, requirement cleared");
  }
  pairListLoad();
  Serial.printf("ACCESS: settings password %s, %u paired regulator(s)\n", pwRequired ? "ON" : "off", (unsigned)pairCount);
}

// Peers: other regulators heard via a browse of _xreg._tcp. Own core-0 task (one query blocks ~3 s); the result is
// pre-rendered JSON behind peerMux. All network input: bounded, escaped by cfgAppendJsonStr, rendered as text.
#define PEER_MAX 12
#define PEER_STALE_MS 120000UL   // one missed 30 s query must not flicker a unit out of the list
#define PEERS_JSON_CAP 3072
struct PeerUnit { char uid[17]; char host[24]; char ip[16]; char fw[12]; char name[33]; char pair[PAIR_MAX * 7 + 1]; uint32_t seenMs; };
static PeerUnit *peers = nullptr;   // ps_calloc'd in startPeerBrowseTask
static uint8_t peerCount = 0;
static char *peersJson = nullptr;   // ps_malloc'd, PEERS_JSON_CAP
static SemaphoreHandle_t peerMux = nullptr;
static TaskHandle_t peerTaskHandle = nullptr;

static void peerBrowseOnce() {
  int n = MDNS.queryService("xreg", "tcp");
  uint32_t now = millis();
  for (int i = 0; i < n; i++) {
    String uid = MDNS.txt(i, "uid");
    if (!uidIsHex16(uid.c_str()) || uid.equalsIgnoreCase(device_id_hex)) continue;
    int k = -1, oldest = 0;
    for (int j = 0; j < peerCount; j++) {
      if (uid.equalsIgnoreCase(peers[j].uid)) { k = j; break; }
      if (peers[j].seenMs < peers[oldest].seenMs) oldest = j;
    }
    if (k < 0) k = (peerCount < PEER_MAX) ? peerCount++ : oldest;
    PeerUnit &p = peers[k];
    copyField(p.uid, sizeof(p.uid), uid);
    String host = MDNS.hostname(i);
    if (host.length() && host.indexOf(".local") < 0) host += ".local";
    copyField(p.host, sizeof(p.host), host);
    copyField(p.ip, sizeof(p.ip), MDNS.address(i).toString());
    copyField(p.fw, sizeof(p.fw), MDNS.txt(i, "fw"));
    copyField(p.name, sizeof(p.name), MDNS.txt(i, "name"));
    copyField(p.pair, sizeof(p.pair), MDNS.txt(i, "pair"));
    p.seenMs = now;
  }
  for (int j = 0; j < peerCount;) {
    if (now - peers[j].seenMs > PEER_STALE_MS) {
      for (int m = j; m + 1 < peerCount; m++) peers[m] = peers[m + 1];
      peerCount--;
    } else {
      j++;
    }
  }
  String out;
  out.reserve(64 + peerCount * 220);
  out += '[';
  for (int j = 0; j < peerCount; j++) {
    if (j) out += ',';
    out += "{\"uid\":\"";
    out += peers[j].uid;   // hex-checked above
    out += "\",\"host\":";
    cfgAppendJsonStr(out, String(peers[j].host));
    out += ",\"ip\":";
    cfgAppendJsonStr(out, String(peers[j].ip));
    out += ",\"fw\":";
    cfgAppendJsonStr(out, String(peers[j].fw));
    out += ",\"name\":";
    cfgAppendJsonStr(out, String(peers[j].name));
    out += ",\"pair\":";
    cfgAppendJsonStr(out, String(peers[j].pair));
    out += ",\"age\":";
    out += String((now - peers[j].seenMs) / 1000);
    out += '}';
  }
  out += ']';
  if (out.length() >= PEERS_JSON_CAP) out = "[]";   // PEER_MAX keeps it well under; this is the guarantee, not a path
  if (peerMux && xSemaphoreTake(peerMux, pdMS_TO_TICKS(200)) == pdTRUE) {
    strcpy(peersJson, out.c_str());
    xSemaphoreGive(peerMux);
  }
}

static void peerBrowseTask(void *) {
  bool first = true;
  for (;;) {
    if (!mdnsStarted) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
    vTaskDelay(pdMS_TO_TICKS(first ? 5000 : 30000));
    first = false;
    if ((uint32_t)WiFi.localIP() == 0 && (uint32_t)WiFi.softAPIP() == 0) continue;
    peerBrowseOnce();
  }
}

void startPeerBrowseTask() {
  peers = (PeerUnit *)ps_calloc(PEER_MAX, sizeof(PeerUnit));
  peersJson = (char *)ps_malloc(PEERS_JSON_CAP);
  if (!peers || !peersJson) {
    Serial.println("PEERS: PSRAM alloc failed, peer list disabled");
    return;
  }
  strcpy(peersJson, "[]");
  peerMux = xSemaphoreCreateMutex();
  // PSRAM stack: this task must never reach flash (an NVS read or a file op asserts on a PSRAM stack).
  if (xTaskCreatePinnedToCoreWithCaps(peerBrowseTask, "PeerBrowse", 5120, NULL, 1, &peerTaskHandle, 0, MALLOC_CAP_SPIRAM) != pdPASS)
    Serial.println("PEERS: task create failed");
}

void peerListAppendJson(String &out) {
  if (peersJson && peerMux && xSemaphoreTake(peerMux, pdMS_TO_TICKS(50)) == pdTRUE) {
    out += peersJson;
    xSemaphoreGive(peerMux);
  } else {
    out += "[]";
  }
}

// WiFi.status()==6 can't tell a wrong password from an AP refusing association (e.g. a hotspot device cap) or a
// missing SSID; this names the AP's actual disconnect reason.
static const char *wifiDisconnectReasonStr(uint8_t reason) {
  switch (reason) {
    case 2:   return "AUTH_EXPIRE";
    case 4:   return "ASSOC_EXPIRE";
    case 5:   return "ASSOC_TOOMANY (AP at device limit)";
    case 6:   return "NOT_AUTHED";
    case 7:   return "NOT_ASSOCED";
    case 8:   return "ASSOC_LEAVE (this device dropped the link itself)";
    case 15:  return "4WAY_HANDSHAKE_TIMEOUT (usually wrong password)";
    case 23:  return "802.1X_AUTH_FAILED";
    case 24:  return "CIPHER_SUITE_REJECTED";
    case 200: return "BEACON_TIMEOUT";
    case 201: return "NO_AP_FOUND (SSID not seen / wrong band)";
    case 202: return "AUTH_FAIL";
    case 203: return "ASSOC_FAIL";
    case 204: return "HANDSHAKE_TIMEOUT (PMF/auth negotiation)";
    case 205: return "CONNECTION_FAIL";
    default:  return "see wifi_err_reason_t in esp_wifi_types.h";
  }
}

// 0 = no disconnect event received; written from the WiFi task, read by the boot-connect verdict
static volatile uint8_t lastStaDiscReason = 0;
// One outage's record for pollWiFiLinkState(): an outage starts at the first disconnect after a GOT_IP and that event's
// reason is the cause; later failed rejoins only bump the count. Events before the boot's first GOT_IP are ignored.
// Single aligned words, event task writes, loop reads: no lock.
static volatile uint32_t staUpMs         = 0;   // millis of the last GOT_IP; 0 = never up this boot
static volatile uint32_t staOutageStartMs = 0;  // millis of the first event of the current/last outage
static volatile uint8_t  staDropReason   = 0;   // that first event's reason
static volatile uint16_t staDropEvents   = 0;   // events in the outage so far
static volatile bool     staDropPending  = false;  // set at outage start, cleared once the loop has reported it

static void onWiFiStaDisconnected(WiFiEvent_t event, WiFiEventInfo_t info) {
  uint8_t reason = info.wifi_sta_disconnected.reason;
  lastStaDiscReason = reason;
  if (staUpMs != 0) {
    if ((int32_t)(staUpMs - staOutageStartMs) > 0) {   // link came up since the last outage began → a new outage
      staOutageStartMs = millis();
      staDropReason = reason;
      staDropEvents = 0;
      staDropPending = true;
    }
    if (staDropEvents < 65535) staDropEvents = staDropEvents + 1;
  }
  Serial.printf("WiFi STA disconnect reason: %u (%s)\n", reason, wifiDisconnectReasonStr(reason));
}

static void onWiFiStaGotIp(WiFiEvent_t event, WiFiEventInfo_t info) {
  staUpMs = millis();
}

// 5 s link poll from loop() (Client-mode radio-up gate). Drops are reported here, never from the event task:
// queueConsoleMessageF formats into a 256 B stack buffer. A drop shorter than the poll never shows as a status edge,
// so the pending record is checked on connected passes too.
void pollWiFiLinkState() {
  static unsigned long lastWiFiStatusCheck = 0;
  static bool lastWiFiConnected = (WiFi.status() == WL_CONNECTED);   // seeded on the first call: a failed boot join is not a drop
  if (millis() - lastWiFiStatusCheck <= 5000) return;
  lastWiFiStatusCheck = millis();
  bool currentlyConnected = (WiFi.status() == WL_CONNECTED);
  bool pending = staDropPending;
  if (pending) staDropPending = false;   // clearing unread would wipe an outage start that landed between the two lines
  uint8_t r = staDropReason;
  uint32_t t0 = staOutageStartMs, up = staUpMs;
  uint16_t n = staDropEvents;
  bool selfDrop = (r == 8);   // ASSOC_LEAVE: this device dropped the link (standby radio-off, OTA, a forced rejoin), not an outage
  float downS = (t0 && (int32_t)(up - t0) > 0) ? (up - t0) / 1000.0f : 0.0f;

  if (lastWiFiConnected && !currentlyConnected) {
    wifiDisconnectCount++;
    if (pending && !selfDrop) queueConsoleMessageF("WiFi disconnected #%u: reason %u (%s)%s", wifiDisconnectCount, r, wifiDisconnectReasonStr(r), n > 1 ? " - rejoin attempts failing too" : "");
    else if (pending)         queueConsoleMessageF("WiFi disconnected #%u: radio was switched off by this device - rejoining", wifiDisconnectCount);
    else                      queueConsoleMessageF("WiFi disconnected #%u (no disconnect event recorded)", wifiDisconnectCount);
  } else if (!lastWiFiConnected && currentlyConnected) {
    wifiReconnectsTotal++;
    if (downS > 0 && !selfDrop) queueConsoleMessageF("WiFi reconnected #%u after %.1f s (RSSI %d dBm)", wifiReconnectsTotal, downS, WiFi.RSSI());
    else                        queueConsoleMessageF("WiFi reconnected #%u (RSSI %d dBm)", wifiReconnectsTotal, WiFi.RSSI());
  } else if (currentlyConnected && pending && !selfDrop) {
    // dropped and re-associated inside one poll interval — the status edges above never saw it
    wifiDisconnectCount++;
    queueConsoleMessageF("WiFi dropped #%u: reason %u (%s), back after %.1f s (RSSI %d dBm)", wifiDisconnectCount, r, wifiDisconnectReasonStr(r), downS, WiFi.RSSI());
  }
  lastWiFiConnected = currentlyConnected;
}

static const char *wifiStatusStr(int s) {
  switch (s) {
    case WL_IDLE_STATUS: return "IDLE";
    case WL_NO_SSID_AVAIL: return "NO_SSID_AVAIL (network not found)";
    case WL_SCAN_COMPLETED: return "SCAN_COMPLETED";
    case WL_CONNECTED: return "CONNECTED";
    case WL_CONNECT_FAILED: return "CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED: return "DISCONNECTED";
    default: return "UNKNOWN";
  }
}

// Boot-time (setup) connect only — a blocking wait is fine before the control loop starts.
// Runtime reconnects go through checkWiFiConnection()'s non-blocking engine.
bool connectToWiFi(const char *ssid, const char *password, unsigned long timeout) {

  if (!ssid || strlen(ssid) == 0) {
    Serial.println("ERROR: No SSID provided for WiFi connection");
    return false;
  }

  Serial.printf("Connecting to WiFi: %s\n", ssid);
  Serial.printf("Password length: %d\n", strlen(password));

  WiFi.disconnect();
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);          // Disable WiFi power save
  WiFi.setAutoReconnect(false);  // checkWiFiConnection() handles reconnection

  static bool wifiReasonLoggerRegistered = false;
  if (!wifiReasonLoggerRegistered) {
    WiFi.onEvent(onWiFiStaDisconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    WiFi.onEvent(onWiFiStaGotIp, ARDUINO_EVENT_WIFI_STA_GOT_IP);
    wifiReasonLoggerRegistered = true;
  }

  lastStaDiscReason = 0;

  if (strlen(password) > 0) {
    WiFi.begin(ssid, password);
  } else {
    WiFi.begin(ssid);  // Open network
  }

  unsigned long startTime = millis();
  int attempts = 0;
  const int maxAttempts = timeout / 100;  // Check every 100ms — detects connect up to ~400ms sooner on wake

  while (WiFi.status() != WL_CONNECTED && attempts < maxAttempts) {
    delay(100);
    if (wdtMainTaskSubscribed) esp_task_wdt_reset();
    attempts++;

    // Print progress every 5 seconds — a normal ~2s connect stays silent; only slow/stuck connects log
    if (attempts % 50 == 0) {
      Serial.printf("WiFi Status: %d, attempt %d/%d\n", WiFi.status(), attempts, maxAttempts);
    }
  }

  if (WiFi.status() == WL_CONNECTED) {

    Serial.println("WiFi connection successful!");
    Serial.printf("IP address: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("Signal strength: %d dBm\n", WiFi.RSSI());

    startMdnsOnce();

    return true;

  } else {

    Serial.printf("WiFi connection failed after %lu ms\n", timeout);
    int st = WiFi.status();
    Serial.printf("Final status: %d (%s)\n", st, wifiStatusStr(st));

    uint8_t r = lastStaDiscReason;
    if (r == 0 || r == 201) {
      Serial.printf("DIAGNOSIS: network '%s' was never seen. The name is case-sensitive - check spelling, and check the network is broadcasting (iPhone hotspot: keep the Personal Hotspot screen open while connecting).\n", ssid);
    } else {
      Serial.printf("DIAGNOSIS: network '%s' was found, but the connection failed (%s). A wrong password is the most common cause.\n", ssid, wifiDisconnectReasonStr(r));
    }

    // List what IS visible so a typo'd SSID is obvious at the bench. Blocking ~2-3 s scan:
    // acceptable only because every caller runs with the field off: setup(), or a loop() wake out of standby.
    WiFi.disconnect();
    if (wdtMainTaskSubscribed) esp_task_wdt_reset();
    int16_t n = WiFi.scanNetworks();
    if (wdtMainTaskSubscribed) esp_task_wdt_reset();
    if (n > 0) {
      Serial.printf("Networks visible now (%d):\n", n);
      for (int16_t i = 0; i < n && i < 10; i++) {
        Serial.printf("  '%s' (ch %d, %d dBm)\n", WiFi.SSID(i).c_str(), (int)WiFi.channel(i), (int)WiFi.RSSI(i));
      }
      if (n > 10) Serial.printf("  ... and %d more\n", (int)(n - 10));
    } else {
      Serial.println("No networks visible in scan");
    }
    WiFi.scanDelete();
    return false;
  }
}
void setupCaptivePortalLanding() {
  static bool captiveInitialized = false;
  if (captiveInitialized) {
    Serial.println("setupCaptivePortalLanding() already initialized - skipping");
    return;
  }
  captiveInitialized = true;
  Serial.println("Setting up captive portal landing...");
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    static const char PAGE[] PROGMEM = R"HTML(<!DOCTYPE html><html><head><title>Alternator Regulator Connected</title>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<style>
body{font-family:Arial;text-align:center;padding:20px;background:#f5f5f5;line-height:1.6}
.card{background:white;padding:24px;border-left:4px solid #ff6600;border-radius:8px;max-width:500px;margin:0 auto;box-shadow:0 2px 4px rgba(0,0,0,0.1)}
h1{color:#333;margin-bottom:1rem;font-size:24px}
.success-box{background:#d4edda;border:1px solid #c3e6cb;color:#155724;padding:16px;border-radius:8px;margin:20px 0}
.big-button{display:inline-block;background:#ff6600;color:white;padding:16px 32px;text-decoration:none;border-radius:8px;font-weight:bold;font-size:18px;margin:20px 0}
.big-button:hover{background:#e65c00}
.bookmark-info{background:#f8f9fa;border:1px solid #dee2e6;padding:12px;border-radius:8px;margin:16px 0;font-size:14px}
.ip-address{font-family:monospace;font-size:20px;font-weight:bold;color:#ff6600;background:#f8f9fa;padding:8px 12px;border-radius:8px;display:inline-block;margin:8px 0}
</style></head><body>
<div class='card'>
<h1>Alternator Regulator</h1>
<div class='success-box'><strong>Successfully Connected!</strong><br>You are now connected to the alternator regulator's WiFi network.</div>
<p>Access the full alternator control interface:</p>
<a href='http://192.168.4.1' class='big-button'>Open Alternator Interface</a>
<div class='bookmark-info'><strong>For easy future access:</strong><br>Bookmark this address: <span class='ip-address'>192.168.4.1</span></div>
<p style='margin-top:24px;font-size:14px;color:#666'><strong>Network:</strong> %APSSID%<br><strong>Device IP:</strong> 192.168.4.1</p>
</div></body></html>)HTML";
    // %APSSID% is substituted live: the AP name is per-unit.
    String page(PAGE);
    page.replace("%APSSID%", esp32_ap_ssid);
    request->send(200, "text/html", page);
  });
  server.begin();
  Serial.println("Landing page ready");
}

// Re-entrant on purpose: engine-off standby sets WIFI_OFF, so ignition-on and the GPIO5 wake button must be able to
// raise the softAP again. Only the mDNS start stays one-shot.
void setupAccessPoint() {
  Serial.println("=== SETTING UP ACCESS POINT ===");
  Serial.println("Using SSID: '" + esp32_ap_ssid + "'");
  Serial.println("Using password: [" + String(esp32_ap_password.length()) + " chars]");
  Serial.println("Password length: " + String(esp32_ap_password.length()));
  Serial.println("SSID length: " + String(esp32_ap_ssid.length()));

  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);  // Disable WiFi power save

  bool apStarted = WiFi.softAP(esp32_ap_ssid.c_str(), esp32_ap_password.c_str());
  delay(500);
  if (apStarted) {
    Serial.println("=== ACCESS POINT STARTED SUCCESSFULLY ===");
    Serial.print("AP SSID: ");
    Serial.println(esp32_ap_ssid);
    Serial.print("AP Password length: ");
    Serial.println(esp32_ap_password.length());
    Serial.print("AP IP address: ");
    Serial.println(WiFi.softAPIP());

    // Start DNS server for captive portal
    IPAddress apIP = WiFi.softAPIP();
    Serial.println("AP IP before DNS start: " + apIP.toString());
    dnsServer.stop();  // on an AP restart the old socket is bound to a netif WIFI_OFF destroyed
    bool dnsStarted = dnsServer.start(DNS_PORT, "*", apIP);
    Serial.println("DNS server start result: " + String(dnsStarted));
    Serial.println("DNS server started for captive portal");

    // One startMdnsOnce() guard for both the AP and STA paths (never MDNS.end(), see startMdnsOnce).
    startMdnsOnce();
    Serial.printf("mDNS on the AP: alternator.local + %s.local (may not work on all devices in AP mode)\n",
                  regulatorHostName());

    Serial.println("=== AP SETUP COMPLETE ===");
  } else {
    Serial.println("=== ACCESS POINT FAILED TO START ===");
    Serial.println("This is a critical error!");
    Serial.println("Trying with default settings as fallback...");
    WiFi.softAP(defaultApSsid().c_str(), "alternator123");
  }
}

static void sendWifiConfigPortal(AsyncWebServerRequest *request) {
  AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", WIFI_CONFIG_HTML);
  response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  response->addHeader("Pragma", "no-cache");
  response->addHeader("Expires", "0");
  request->send(response);
}
void setupWiFiConfigServer() {
  static bool configServerInitialized = false;
  if (configServerInitialized) {
    Serial.println("setupWiFiConfigServer() already initialized - skipping");
    return;
  }
  configServerInitialized = true;
  Serial.println("\n=== SETTING UP WIFI CONFIG SERVER ===");
  Serial.println("=== IMPORTANT: Alternator is DISABLED in CONFIG mode ===");
  Serial.println("=== For emergency operation: Power down, hold GPIO46 LOW, power up ===");
  Serial.println("=== This enters AP mode with full alternator functionality ===\n");

  server.on("/generate_204", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println("Android captive portal detection");
    request->redirect("http://" + WiFi.softAPIP().toString() + "/");
  });
  server.on("/gen_204", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println("Android captive portal detection (alt)");
    request->redirect("http://" + WiFi.softAPIP().toString() + "/");
  });

  server.on("/hotspot-detect.html", HTTP_ANY, [](AsyncWebServerRequest *request) {
    Serial.println("Apple captive portal detection");
    sendWifiConfigPortal(request);
  });

  server.on("/ncsi.txt", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println("Windows NCSI detection");
    request->redirect("http://" + WiFi.softAPIP().toString() + "/");
  });
  server.on("/connecttest.txt", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println("Windows captive portal detection");
    request->redirect("http://" + WiFi.softAPIP().toString() + "/");
  });

  server.on("/", HTTP_ANY, [](AsyncWebServerRequest *request) {
    Serial.println("=== CONFIG PAGE REQUEST ===");
    Serial.println("Client IP: " + request->client()->remoteIP().toString());
    Serial.println("User-Agent: " + request->header("User-Agent"));
    Serial.println("Serving WiFi configuration page");
    sendWifiConfigPortal(request);
  });

  server.on("/wifi", HTTP_POST, [](AsyncWebServerRequest *request) {
    Serial.println("\n=== WIFI CONFIG POST RECEIVED ===");

    char ssid[33] = "";
    char password[65] = "";
    char ap_password[65] = "";
    char hotspot_ssid[33] = "";

    auto copyParamTrim = [](AsyncWebServerRequest *req, const char *name, char *dst, size_t dstSize) -> bool {
      if (!req->hasParam(name, true)) return false;
      const String &v = req->getParam(name, true)->value();
      size_t n = v.length();
      if (n >= dstSize) n = dstSize - 1;
      memcpy(dst, v.c_str(), n);
      dst[n] = '\0';

      size_t start = 0;
      while (dst[start] == ' ' || dst[start] == '\t' || dst[start] == '\r' || dst[start] == '\n') start++;
      if (start) memmove(dst, dst + start, strlen(dst + start) + 1);

      size_t end = strlen(dst);
      while (end > 0 && (dst[end - 1] == ' ' || dst[end - 1] == '\t' || dst[end - 1] == '\r' || dst[end - 1] == '\n')) {
        dst[end - 1] = '\0';
        end--;
      }
      return true;
    };

    copyParamTrim(request, "ap_password", ap_password, sizeof(ap_password));
    copyParamTrim(request, "hotspot_ssid", hotspot_ssid, sizeof(hotspot_ssid));
    copyParamTrim(request, "ssid", ssid, sizeof(ssid));
    copyParamTrim(request, "password", password, sizeof(password));

    if (ap_password[0] != '\0' && strlen(ap_password) < 8) {
      static const char ERR[] PROGMEM =
        "<!DOCTYPE html><html><head><title>Configuration Error</title></head><body>"
        "<h1>Configuration Error</h1>"
        "<p><strong>Hotspot password must be at least 8 characters, or left blank to keep the current one.</strong></p>"
        "<p><a href='javascript:history.back()'>Go Back and Try Again</a></p>"
        "</body></html>";
      request->send_P(400, "text/html", ERR);
      return;
    }

    Serial.println("=== SAVING CONFIGURATION ===");
    Serial.printf("SSID: '%s' (length: %u)\n", ssid, (unsigned)strlen(ssid));
    Serial.printf("Password: '%s' (length: %u)\n", password, (unsigned)strlen(password));

    // Credentials persist in NVS (settings namespace) — no LittleFS dependency here
    static const char STORAGE_ERR[] = "Storage error - cannot save configuration";

    // Blank hotspot field = keep the stored value: the form never pre-fills these, so writing the default on blank reset a
    // custom name/password on every save. reset_ap returns each blank field to its default (absent key = default at load).
    bool resetAp = request->hasParam("reset_ap", true);
    if (ap_password[0] != '\0') {
      if (!settingWrite(NK_appass, ap_password)) {
        Serial.println("CRITICAL: NVS write failed");
        request->send(500, "text/plain", STORAGE_ERR);
        return;
      }
      esp32_ap_password = ap_password;
    } else if (resetAp) {
      settingRemove(NK_appass);
      esp32_ap_password = "alternator123";
    }

    if (hotspot_ssid[0] != '\0') {
      settingWrite(NK_apssid, hotspot_ssid);
      esp32_ap_ssid = hotspot_ssid;
    } else if (resetAp) {
      settingRemove(NK_apssid);
      esp32_ap_ssid = defaultApSsid();
    }

    // Blank client SSID = user only came for AP settings: leave stored client creds untouched (an unconditional write
    // wiped working WiFi). Deliberate erase goes through the forget_client checkbox.
    bool forgetClient = (ssid[0] == '\0' && request->hasParam("forget_client", true));
    if (ssid[0] != '\0') {
      if (!settingWrite(NK_ssid, ssid)) {
        Serial.println("CRITICAL: NVS write failed");
        request->send(500, "text/plain", STORAGE_ERR);
        return;
      }
      settingWrite(NK_pass, password);

      strncpy(cached_wifi_ssid, ssid, sizeof(cached_wifi_ssid) - 1);
      cached_wifi_ssid[sizeof(cached_wifi_ssid) - 1] = '\0';

      strncpy(cached_wifi_pass, password, sizeof(cached_wifi_pass) - 1);
      cached_wifi_pass[sizeof(cached_wifi_pass) - 1] = '\0';

      cached_wifi_creds_valid = true;
    } else if (forgetClient) {
      settingRemove(NK_ssid);
      settingRemove(NK_pass);
      cached_wifi_ssid[0] = '\0';
      cached_wifi_pass[0] = '\0';
      cached_wifi_creds_valid = false;
    }

    Serial.printf("Verification - SSID: '%s'\n", cached_wifi_ssid);
    Serial.printf("Verification - Password: '%s'\n", cached_wifi_pass);

    request->send(200, "text/plain", ssid[0] != '\0'
                                       ? "Configuration saved! Device will restart in 2 seconds."
                                     : forgetClient
                                       ? "Ship's WiFi credentials erased. Device will restart in 2 seconds into this setup page (charging disabled until WiFi is configured or the Hotspot Wire is used)."
                                       : "Configuration saved (client WiFi credentials unchanged). Device will restart in 2 seconds.");

    Serial.println("=== CONFIGURATION SAVED - RESTARTING ===");
    settingWrite(NK_first_config_done, "1");
    // ESPAsyncWebServer 3.x sends the response only after the handler returns, so an in-handler ESP.restart() drops the
    // 200 and leaves the portal stuck on "Saving..." (a chip reset sends no FIN/RST).
    rebootRequestWhy = REBOOT_WHY_WIFI_SETUP;
    rebootRequestedAt = millis();
    rebootRequested = true;
  });

  server.onNotFound([](AsyncWebServerRequest *request) {
    if (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) {
      request->redirect("http://" + WiFi.softAPIP().toString() + "/");
    } else {
      request->send(404, "text/plain", "Not Found");
    }
  });

  server.begin();
  Serial.println("=== WIFI CONFIG SERVER STARTED ===");
  Serial.println("=== WIFI CONFIG SERVER SETUP COMPLETE ===");
}

// Raw WiFiClientSecure POST for the 4 registration/profile cloud ops, run via executeCloudOp on the httpsTask worker,
// never in a web handler. Not HTTPClient: getString() hangs on TLS. Mirrors executeUploadPayload() in 2_functions.ino.
// Returns the HTTP status, or -1 low heap, -2 connect fail, -3 handshake/global timeout, -4 send fail, -5 read timeout /
// no status line. responseBuf is null-terminated on return; nullptr if the body is not needed.
int doCloudPOST(const char *endpointPath, const char *payload,
                char *responseBuf, size_t responseBufSize) {
  if (responseBuf && responseBufSize > 0) responseBuf[0] = '\0';
  if (demoSeal) return -6;   // demo mode: nothing reaches the cloud

  // Hard floor only (setInsecure TLS needs ~5-10 KB). Do not raise it: 40 KB tripped right after sensor uploads before
  // heap was reclaimed; a real OOM fails client.connect() -> -2.
  if (ESP.getMaxAllocHeap() < 15000) {
    Serial.printf("doCloudPOST(%s): heap critically low (max alloc %u), aborting\n",
                  endpointPath, ESP.getMaxAllocHeap());
    return -1;
  }

  // Pre-resolve DNS with a WDT feed so a slow lookup (~15 s worst on dead cellular) can't stack
  // into the same unfed window as the up-to-14 s TCP+TLS connect; connect() then hits the lwIP cache.
  IPAddress preResolved;
  // hostByName returns 1 on success and the lwIP error code on failure, never 0
  if (WiFi.hostByName(host, preResolved) != 1) {
    Serial.printf("doCloudPOST(%s): DNS lookup failed\n", endpointPath);
    queueConsoleMessageF("Cloud unreachable: DNS lookup for %s failed (check WiFi/router DNS)", host);
    return -2;
  }
  esp_task_wdt_reset();

  WiFiClientSecure client;
  // setInsecure, as in executeUploadPayload: setCACert(server_root_ca) fails (mbedTLS -0x2700) because Supabase sits
  // behind Cloudflare's chain, not ISRG Root X1. Deliberate: the threat model is trusted owner WiFi.
  client.setInsecure();
  // No setTimeout: the read loops poll available()/read() against explicit millis() deadlines.
  client.setHandshakeTimeout(HANDSHAKE_TIMEOUT / 1000);  // this API takes seconds

  uint32_t start = millis();
  esp_task_wdt_reset();

  if (!client.connect(host, port, CONNECT_TIMEOUT)) {
    uint32_t connElapsed = millis() - start;  // how long the failed TLS connect took (before extra probes below)
    esp_task_wdt_reset();                     // the failed connect may have just eaten 14 s of the WDT window
    char errBuf[128] = {0};
    int lastErr = client.lastError(errBuf, sizeof(errBuf));
    // A generic mbedTLS -1 can be DNS, blocked TCP 443, or a failed TLS handshake (version/cipher, MTU dropping the large
    // Cloudflare cert); each needs a different fix, so probe and name which.
    IPAddress resolved;
    bool dnsOk = (WiFi.hostByName(host, resolved) == 1);
    bool tcpOk = false;
    if (dnsOk) {
      WiFiClient probe;  // plain TCP, no TLS — tests whether port 443 is reachable at all
      tcpOk = probe.connect(resolved, port, CONNECT_TIMEOUT);
      probe.stop();
    }
    if (!dnsOk) {
      queueConsoleMessageF("Cloud unreachable: DNS lookup for %s failed (check WiFi/router DNS)", host);
    } else if (!tcpOk) {
      queueConsoleMessageF("Cloud unreachable: %s resolves to %s but port 443 is blocked (firewall/router/client isolation)",
                           host, resolved.toString().c_str());
    } else {
      // Usually not the network: mbedTLS needs ~32-40 KB CONTIGUOUS internal RAM and field-on work fragments it. A low
      // largest-block figure with a fast fail points at RAM.
      queueConsoleMessageF("Cloud unreachable: TCP reached %s:443 but TLS handshake failed in %u ms; largest free internal block %u B (mbedTLS needs ~32-40KB contiguous — if low, it's RAM not network; try with field OFF)",
                           resolved.toString().c_str(), (unsigned)connElapsed, (unsigned)ESP.getMaxAllocHeap());
    }
    Serial.printf("doCloudPOST(%s): connect FAIL in %u ms, mbedTLS err=%d (0x%X) '%s', dnsOk=%d tcpOk=%d\n",
                  endpointPath, (unsigned)(millis() - start), lastErr, lastErr, errBuf, (int)dnsOk, (int)tcpOk);
    client.stop();
    return -2;
  }
  esp_task_wdt_reset();
  if (millis() - start > GLOBAL_TIMEOUT) {
    Serial.printf("doCloudPOST(%s): GLOBAL_TIMEOUT after connect at %u ms\n",
                  endpointPath, (unsigned)(millis() - start));
    client.stop();
    return -3;
  }

  size_t payloadLen = strlen(payload);
  // HTTP/1.0 deliberately: Cloudflare sends chunked encoding to HTTP/1.1 clients and the body loop below has no chunked
  // decoder. 1.0 forces a plain body + Connection: close.
  int headerBytes = client.printf(
    "POST %s HTTP/1.0\r\n"
    "Host: %s\r\n"
    "Content-Type: application/json\r\n"
    "Authorization: Bearer %s\r\n"
    "Connection: close\r\n"
    "Content-Length: %u\r\n\r\n",
    endpointPath, host, SUPABASE_ANON_KEY, (unsigned)payloadLen);

  if (headerBytes <= 0 || !client.connected()) {
    Serial.printf("doCloudPOST(%s): header write FAIL headerBytes=%d connected=%d\n",
                  endpointPath, headerBytes, (int)client.connected());
    client.stop();
    return -4;
  }

  if (!cloudWriteBody(client, payload, payloadLen, start)) {
    Serial.printf("doCloudPOST(%s): payload write FAIL (%u bytes)\n", endpointPath, (unsigned)payloadLen);
    client.stop();
    return -4;
  }

  int httpCode = 0;
  uint32_t readStart = millis();
  char statusBuf[64];
  size_t statusLen = 0;
  bool gotStatusLine = false;

  while (client.connected() && (millis() - readStart < READ_TIMEOUT)) {
    esp_task_wdt_reset();
    while (client.available()) {
      char c = (char)client.read();
      readStart = millis();  // idle timeout — each byte restarts it
      if (statusLen < sizeof(statusBuf) - 1) statusBuf[statusLen++] = c;
      if (c == '\n') { gotStatusLine = true; break; }
    }
    if (gotStatusLine) break;
    if (millis() - start > GLOBAL_TIMEOUT) break;
    delay(1);
  }
  if (!gotStatusLine) {
    Serial.printf("doCloudPOST(%s): no status line within READ_TIMEOUT (got %u bytes, connected=%d)\n",
                  endpointPath, (unsigned)statusLen, (int)client.connected());
    client.stop();
    return -5;
  }
  statusBuf[statusLen] = '\0';
  char *sp = strchr(statusBuf, ' ');
  if (sp) httpCode = atoi(sp + 1);
  if (httpCode <= 0) {
    Serial.printf("doCloudPOST(%s): unparseable status line '%s'\n", endpointPath, statusBuf);
    client.stop();
    return -5;
  }

  enum { ST_HDR, ST_BODY } state = ST_HDR;
  int crlfRun = 0;
  size_t bodyLen = 0;

  while (client.connected() && (millis() - readStart < READ_TIMEOUT)) {
    esp_task_wdt_reset();
    while (client.available()) {
      char c = (char)client.read();
      readStart = millis();
      if (state == ST_HDR) {
        if (c == '\r') continue;
        if (c == '\n') {
          crlfRun++;
          if (crlfRun >= 2) state = ST_BODY;
        } else {
          crlfRun = 0;
        }
      } else {
        if (responseBuf && bodyLen < responseBufSize - 1) responseBuf[bodyLen++] = c;
      }
    }
    if (millis() - start > GLOBAL_TIMEOUT) break;
    delay(1);
  }
  if (responseBuf && responseBufSize > 0) responseBuf[bodyLen] = '\0';

  client.stop();
  return httpCode;
}

// Plain-English reason for a doCloudPOST() negative return code, for the dashboard Console.
const char *cloudFailReason(int code) {
  switch (code) {
    case -1: return "internal memory too low for secure connection";
    case -2: return "could not reach cloud server (check this WiFi has internet)";
    case -3: return "timed out after connecting";
    case -4: return "lost connection while sending";
    case -5: return "no response from cloud server";
    case -6: return "demo mode is on";
    default: return "unknown error";
  }
}

// ── Async cloud-op slot: the four profile endpoints stage here and answer 202; the httpsTask worker runs executeCloudOp
// and /cloudOpState serves the finished (code, body). One op in flight at a time. Plain globals, no struct: Arduino's
// auto-prototype generator can't parse sketch-defined types in free-function signatures.
#define CLOUDOP_CHECK_REG      1
#define CLOUDOP_REGISTER       2
#define CLOUDOP_UPDATE_PROFILE 3
#define CLOUDOP_DELETE_DATA    4
#define CLOUDOP_PAYLOAD_CAP 1024
#define CLOUDOP_RESP_CAP    2048
#define CLOUDOP_BODY_CAP    2304   // resp + envelope headroom, so pass-through can never truncate
static volatile uint8_t cloudOpState = 0;   // 0 idle, 1 pending, 2 done
static volatile uint8_t cloudOpKind = 0;
static volatile int cloudOpCode = 0;        // HTTP status returned to the poll
static uint32_t cloudOpSeq = 0;
static char *cloudOpPayload = nullptr;      // PSRAM, lazy — staged request JSON
static char *cloudOpResp = nullptr;         // PSRAM, lazy — raw cloud response scratch (worker only)
static char *cloudOpBody = nullptr;         // PSRAM, lazy — final JSON body for the poll

static bool cloudOpBuffersReady() {
  if (!cloudOpPayload) cloudOpPayload = (char *)ps_malloc(CLOUDOP_PAYLOAD_CAP);
  if (!cloudOpResp)    cloudOpResp    = (char *)ps_malloc(CLOUDOP_RESP_CAP);
  if (!cloudOpBody)    cloudOpBody    = (char *)ps_malloc(CLOUDOP_BODY_CAP);
  return cloudOpPayload && cloudOpResp && cloudOpBody;
}

// Pass a cloud response through to the poll body — the poll envelope embeds it verbatim, so it
// must be JSON; anything else (proxy error page) is replaced instead of corrupting the envelope.
static void cloudOpSetBodyJson(const char *resp) {
  if (resp[0] == '{' || resp[0] == '[') strlcpy(cloudOpBody, resp, CLOUDOP_BODY_CAP);
  else snprintf(cloudOpBody, CLOUDOP_BODY_CAP, "{\"error\":\"malformed cloud response\"}");
}

static void cloudOpStageAndReply(AsyncWebServerRequest *request, uint8_t kind, const char *payload) {
  if (demoSeal) {
    char b[160];
    snprintf(b, sizeof(b), "{\"success\":false,\"error\":\"%s\"}", DEMO_REFUSAL);
    request->send(423, "application/json", b);
    return;
  }
  if (!cloudOpBuffersReady()) {
    request->send(500, "application/json", "{\"error\":\"alloc failed\"}");
    return;
  }
  if (cloudOpState == 1) {
    request->send(503, "application/json", "{\"error\":\"cloud operation already in progress\"}");
    return;
  }
  if (strlen(payload) >= CLOUDOP_PAYLOAD_CAP) {
    request->send(500, "application/json", "{\"error\":\"payload too large\"}");
    return;
  }
  strcpy(cloudOpPayload, payload);
  cloudOpKind = kind;
  cloudOpCode = 0;
  cloudOpBody[0] = '\0';
  cloudOpSeq++;
  cloudOpState = 1;
  HttpsRequest req = { .type = HTTPS_CLOUD_OP };
  if (xQueueSend(httpsQueue, &req, 0) != pdTRUE) {
    cloudOpState = 0;
    request->send(503, "application/json", "{\"error\":\"cloud queue full, retry shortly\"}");
    return;
  }
  char buf[64];
  snprintf(buf, sizeof(buf), "{\"state\":\"pending\",\"op\":%u}", (unsigned)cloudOpSeq);
  request->send(202, "application/json", buf);
}

// Runs on the httpsTask worker (HTTPS_CLOUD_OP).
void executeCloudOp() {
  if (cloudOpState != 1 || !cloudOpBuffersReady()) { cloudOpState = 0; return; }
  char *resp = cloudOpResp;
  resp[0] = '\0';
  int outCode = 200;
  snprintf(cloudOpBody, CLOUDOP_BODY_CAP, "{\"error\":\"internal\"}");
  switch (cloudOpKind) {
    case CLOUDOP_CHECK_REG: {
      int httpCode = doCloudPOST("/functions/v1/validate-token", cloudOpPayload, resp, CLOUDOP_RESP_CAP);
      Serial.printf("Validate-token HTTP code: %d\n", httpCode);
      if (httpCode == 200) {
        queueConsoleMessage("Cloud: profile verified");
        DynamicJsonDocument doc(2048);
        DeserializationError error = deserializeJson(doc, resp);
        if (error) {
          snprintf(cloudOpBody, CLOUDOP_BODY_CAP, "{\"registered\":false,\"error\":\"parse_failed\"}");
        } else if (doc["device_uid"].as<String>() != String(device_id_hex)) {
          Serial.println("ERROR: Token device mismatch!");
          snprintf(cloudOpBody, CLOUDOP_BODY_CAP, "{\"registered\":false,\"error\":\"device_mismatch\"}");
        } else {
          cloudOpSetBodyJson(resp);   // token valid and matches — full profile data through
        }
      } else if (httpCode == 401) {
        DynamicJsonDocument doc(1024);
        DeserializationError error = deserializeJson(doc, resp);
        // Clear credentials only on code token_not_found (absent row): a bare 401 (gateway or lookup fault) must not
        // erase a token the device cannot get back.
        String code = (!error && doc.containsKey("code")) ? doc["code"].as<String>() : String("");
        if (code == "token_not_found") {
          Serial.println("Clearing invalid credentials - device ready for re-registration");
          queueConsoleMessage("Cloud: ready to register");
          clearAuthToken();
        }
        snprintf(cloudOpBody, CLOUDOP_BODY_CAP, "{\"registered\":false,\"error\":\"validation_failed\"}");
      } else {
        snprintf(cloudOpBody, CLOUDOP_BODY_CAP, "{\"registered\":false,\"error\":\"network_error\"}");
      }
      outCode = 200;   // 200 on every path; the body carries the verdict
      break;
    }
    case CLOUDOP_REGISTER: {
      int httpCode = doCloudPOST("/functions/v1/register-device", cloudOpPayload, resp, CLOUDOP_RESP_CAP);
      Serial.printf("Register response code: %d\n", httpCode);
      if (httpCode <= 0) {
        queueConsoleMessageF("Registration failed: %s (code %d)", cloudFailReason(httpCode), httpCode);
        snprintf(cloudOpBody, CLOUDOP_BODY_CAP, "{\"error\":\"Connection to cloud failed\",\"code\":%d}", httpCode);
        outCode = 503;
      } else if (resp[0] == '\0') {
        queueConsoleMessageF("Registration failed: empty response from cloud (HTTP %d)", httpCode);
        snprintf(cloudOpBody, CLOUDOP_BODY_CAP, "{\"error\":\"Empty response from cloud\",\"code\":%d}", httpCode);
        outCode = 502;
      } else {
        if (httpCode == 200) {
          DynamicJsonDocument responseDoc(1024);
          DeserializationError error = deserializeJson(responseDoc, resp);
          if (!error && responseDoc.containsKey("token")) {
            String newToken = responseDoc["token"].as<String>();
            if (newToken.length() > 0) {
              esp_err_t saveErr = saveAuthToken(newToken);
              if (saveErr != ESP_OK) {
                queueConsoleMessageF("Registration failed: token could not be saved on the device (NVS error %d)", (int)saveErr);
                snprintf(cloudOpBody, CLOUDOP_BODY_CAP,
                         "{\"error\":\"Registration failed: token could not be saved on the device\",\"code\":%d}", (int)saveErr);
                outCode = 500;
                break;
              }
              Serial.println("Token saved to NVS");
              // A new record (not a nonce recovery) holds none of this unit's curves; owe them to it.
              if (responseDoc["message"].as<String>() == "Registration successful") recSeedAfterNewRegistration();
              // The boot one-shot in loop() sends this pair once isRegistered; only a re-registration after it ran
              // (otaCheckDone) queues it here. Version first: the cloud clears a forced flag once the version reaches
              // it. Factory partition excluded as in the boot check (Xregulator.ino).
              if (otaCheckDone && currentPartitionType != 0) {
                HttpsRequest verReq = { .type = HTTPS_UPDATE_FW_VERSION };
                HttpsRequest forcedReq = { .type = HTTPS_CHECK_FORCED_UPDATE };
                bool verSent = (xQueueSend(httpsQueue, &verReq, 0) == pdTRUE);
                bool forcedSent = (xQueueSend(httpsQueue, &forcedReq, 0) == pdTRUE);
                if (!verSent || !forcedSent) {
                  Serial.println("REGISTER: HTTPS queue full - version/forced check deferred to next boot");
                }
              }
              // Fill the new profile's vessel columns now — registration is identity-only, the
              // snapshot's settings jsonb is what carries the vessel record to the cloud.
              configSnapshotRequested = true;
              configSnapshotRetryMs = 0;
            }
          }
        }
        cloudOpSetBodyJson(resp);
        outCode = httpCode;
      }
      break;
    }
    case CLOUDOP_UPDATE_PROFILE: {
      int httpCode = doCloudPOST("/functions/v1/update-profile", cloudOpPayload, resp, CLOUDOP_RESP_CAP);
      Serial.printf("Update-profile response code: %d\n", httpCode);
      if (httpCode <= 0) {
        queueConsoleMessageF("Profile update failed: %s (code %d)", cloudFailReason(httpCode), httpCode);
        snprintf(cloudOpBody, CLOUDOP_BODY_CAP,
                 "{\"success\":false,\"error\":\"Connection to cloud failed\",\"code\":%d}", httpCode);
        outCode = 503;
      } else {
        cloudOpSetBodyJson(resp);
        outCode = httpCode;
      }
      break;
    }
    case CLOUDOP_DELETE_DATA: {
      int httpCode = doCloudPOST("/functions/v1/delete-user-data", cloudOpPayload, resp, CLOUDOP_RESP_CAP);
      Serial.printf("Delete-user-data response code: %d\n", httpCode);
      if (httpCode <= 0) {
        queueConsoleMessageF("Account delete failed: %s (code %d)", cloudFailReason(httpCode), httpCode);
        snprintf(cloudOpBody, CLOUDOP_BODY_CAP,
                 "{\"success\":false,\"error\":\"Connection to cloud failed\",\"code\":%d}", httpCode);
        outCode = 503;
      } else {
        if (httpCode == 200 && strstr(resp, "\"success\":true")) {   // a portal's 200 must not erase a token the cloud still honors
          clearAuthToken();
          Serial.println("Auth token cleared from NVS");
        }
        cloudOpSetBodyJson(resp);
        outCode = httpCode;
      }
      break;
    }
    default:
      snprintf(cloudOpBody, CLOUDOP_BODY_CAP, "{\"error\":\"unknown op\"}");
      outCode = 500;
      break;
  }
  cloudOpCode = outCode;
  cloudOpState = 2;   // last: the poll reads code/body only after seeing state done
}

// The field-on queue flush (AdjustFieldLearnMode) dropped the staged op before the worker ran it.
void cloudOpFailDropped() {
  if (cloudOpState != 1) return;
  snprintf(cloudOpBody, CLOUDOP_BODY_CAP, "{\"error\":\"cloud request dropped when charging started, try again\"}");
  cloudOpCode = 503;
  cloudOpState = 2;
}

// Body of a /perfUploadFront or /altUploadFront POST (Load CSV): one PSRAM copy on request->_tempObject, filled
// across body chunks, ingested once the request completes, and free()d by the web server when the request ends or
// aborts. Not the 512 KB sync cap (PERF_UPLOAD_BUF_SIZE): runtime free PSRAM cannot spare a block that size. Over the cap, nothing is buffered.
#define FRONT_UPLOAD_CAP 131072u
#define FU_OK      0
#define FU_TOO_BIG 1   // answered 413 with the real size
#define FU_NO_MEM  2   // answered 503
struct FrontUpload { uint32_t total; uint8_t state; char text[1]; };
static String importConfigBuf;   // body accumulator for POST /importConfig (config sharing)

static void frontUploadBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  FrontUpload *u = (FrontUpload *)request->_tempObject;
  if (index == 0 && !u && total > 0) {
    uint8_t st = (total > FRONT_UPLOAD_CAP) ? FU_TOO_BIG : FU_OK;
    if (st == FU_OK) u = (FrontUpload *)ps_malloc(sizeof(FrontUpload) + total);
    if (!u) {
      if (st == FU_OK) st = FU_NO_MEM;
      u = (FrontUpload *)ps_malloc(sizeof(FrontUpload));
      if (!u) u = (FrontUpload *)malloc(sizeof(FrontUpload));   // header only, so the reply can still say why
      if (!u) return;
    }
    u->total = (uint32_t)total; u->state = st; u->text[0] = '\0';
    request->_tempObject = u;
  }
  if (!u || u->state != FU_OK || index + len > u->total) return;
  memcpy(u->text + index, data, len);
  u->text[index + len] = '\0';
}

// The received CSV text (the caller may mutate it), or nullptr once the request has been answered with why not.
static char *frontUploadText(AsyncWebServerRequest *request) {
  FrontUpload *u = (FrontUpload *)request->_tempObject;
  char msg[112];
  if (u && u->state == FU_TOO_BIG) {
    snprintf(msg, sizeof(msg), "The curve data is %u KB; the regulator accepts up to %u KB.",
             (unsigned)((u->total + 1023u) / 1024u), (unsigned)(FRONT_UPLOAD_CAP / 1024u));
    request->send(413, "text/plain", msg);
    return nullptr;
  }
  if (u && u->state == FU_NO_MEM) {
    snprintf(msg, sizeof(msg), "Not enough free memory on the regulator to receive this %u KB file.",
             (unsigned)((u->total + 1023u) / 1024u));
    request->send(503, "text/plain", msg);
    return nullptr;
  }
  if (!u || strlen(u->text) < 8 || !strstr(u->text, "BEFRONT1")) {
    request->send(400, "text/plain", "No BEFRONT1 data in upload");
    return nullptr;
  }
  return u->text;
}

// Editing the DVCC source dialect or the instance filter points the follower at a different
// authority, so everything decoded from the old one has to go and the new one must settle fresh.
static void dvccResetAuthority() {
  dvccRxCvl = dvccRxCcl = NAN;
  dvccRxLastMs = 0;
  dvccRxSrcAddr = 255;
  dvccCvlV = dvccCclA = NAN;
  if (dvccState != 5) dvccState = (dvccEn == 1) ? 1 : 0;  // preserve an UNTRUSTED latch across source edits
  dvccCfgChanged = true;  // dvccTick also clears its decode-side state (sender lock, flap history)
  dataTimestamps[IDX_DVCC] = 0;
  dataTimestamps[IDX_DVCC_VECAN] = 0;
}

// Reads the caller's run token (run=) off a test-start request. Absent = 0, which is what a caller
// that doesn't use tokens gets: its frames then match only other token-less runs. Its own name on
// the wire: tok= is the settings arm token, and a /get carries both on a password-protected unit.
static uint32_t webRunTok(AsyncWebServerRequest *request) {
  if (!request->hasParam("run")) return 0;
  return (uint32_t)strtoul(request->getParam("run")->value().c_str(), nullptr, 10);
}

// Sender fingerprint for a client write that changes what the field may do (master switch, field control), both
// directions, same shape so an OFF and the ON after it pair up in the console. The query carries the dashboard's src/n
// tag (clientWriteTrack): same src and n twice = one request delivered twice; a different src = another tab.
static void logClientWrite(AsyncWebServerRequest *request, const char *what) {
  String qs;
  bool anyParam = false;
  for (size_t i = 0; i < request->params() && qs.length() < 120; i++) {
    const AsyncWebParameter *p = request->getParam(i);
    if (p->name() == "tok" || p->name() == "pw") continue;   // the console reaches every client on the network
    if (anyParam) qs += '&';
    anyParam = true;
    qs += p->name();
    qs += '=';
    qs += p->value();
  }
  const AsyncWebHeader *uaH = request->getHeader("User-Agent");
  String ua = uaH ? uaH->value() : String("-");
  if (ua.length() > 80) ua = ua.substring(0, 80);
  String from = request->client()->remoteIP().toString();
  if (gxFlagged(request)) from += " Victron GX";
  queueConsoleMessageF("%s [from %s | %s | UA %s]", what, from.c_str(), qs.c_str(), ua.c_str());
}

// Per-tab ordering guard. The dashboard tags writes with src (page-load nonce) and n (press counter); same src with n at
// or below the last applied is a duplicate or held-back delivery, not a press. Returns true when out of order; untagged
// writes are never gated and never advance it; the caller decides whether to drop (an OFF never is). CW_SLOTS recent
// tabs per parameter, round-robin. Built-in types only: the Arduino prototype generator emits this above any tab's structs.
#define CW_SLOTS 4
static char (&g_onOffWriteSrc)[CW_SLOTS * 17] = PsramPlace::make<char[CW_SLOTS * 17]>();
static uint32_t g_onOffWriteN[CW_SLOTS] = { 0 };
static uint8_t g_onOffWriteNext = 0;
static char (&g_manualWriteSrc)[CW_SLOTS * 17] = PsramPlace::make<char[CW_SLOTS * 17]>();
static uint32_t g_manualWriteN[CW_SLOTS] = { 0 };
static uint8_t g_manualWriteNext = 0;
// isRepeat (out): n == last applied is the dashboard's ~500 ms hedge copy of an unanswered write; n below it is a
// superseded press. Both are refused; only the console wording differs, so the log never invents a press.
static bool clientWriteTrack(AsyncWebServerRequest *request, char *srcTab, uint32_t *nTab, uint8_t *nextSlot, bool *isRepeat) {
  if (isRepeat) *isRepeat = false;
  if (!request->hasParam("src") || !request->hasParam("n")) return false;
  const String &src = request->getParam("src")->value();
  if (src.length() == 0 || src.length() > 16) return false;
  uint32_t n = (uint32_t)strtoul(request->getParam("n")->value().c_str(), nullptr, 10);
  for (uint8_t i = 0; i < CW_SLOTS; i++) {
    char *slot = srcTab + i * 17;
    if (slot[0] && strcmp(slot, src.c_str()) == 0) {
      if (n <= nTab[i]) {
        if (isRepeat) *isRepeat = (n == nTab[i]);
        return true;
      }
      nTab[i] = n;
      return false;
    }
  }
  uint8_t i = *nextSlot;
  *nextSlot = (uint8_t)((i + 1) % CW_SLOTS);
  snprintf(srcTab + i * 17, 17, "%s", src.c_str());
  nTab[i] = n;
  return false;
}

static bool addressedElsewhere(AsyncWebServerRequest *request) {
  if (!request->hasParam("uid")) return false;
  const String &want = request->getParam("uid")->value();
  return want.length() && !want.equalsIgnoreCase(device_id_hex);
}
// Not logClientWrite: it echoes params, and /setPassword's are the password.
static bool refuseMisaddressed(AsyncWebServerRequest *request) {
  if (!addressedElsewhere(request)) return false;
  queueConsoleMessageF("WRITE REJECTED: %s addressed to another regulator [from %s]",
                       request->url().c_str(), request->client()->remoteIP().toString().c_str());
  request->send(409, "text/plain", device_id_hex);
  return true;
}

// One /gxLimits value. Absent = not available (NAN). Present = the whole string must be a finite number, else false.
static bool gxLimitParam(AsyncWebServerRequest *request, const char *name, float &out) {
  out = NAN;
  if (!request->hasParam(name)) return true;
  const char *s = request->getParam(name)->value().c_str();
  char *end = nullptr;
  float v = strtof(s, &end);
  if (end == s || *end != 0 || !isfinite(v)) return false;
  out = v;
  return true;
}

// ── NMEA 2000 bus health / raw capture / history endpoints (NMEA2K_TRANSMIT_SPEC.md §16). Observe-only, so unarmed.
// All formatting runs here on the web task; Core 1 only ever wrote the data.

// Quoted JSON string from an untrusted bus string: dvccCopyPlainText first (printable ASCII only), then escaping.
static size_t n2kJsonStr(char *dst, size_t cap, const char *src) {
  char clean[48];
  dvccCopyPlainText(clean, sizeof(clean), src);
  size_t n = 0;
  if (cap < 3) return 0;
  dst[n++] = '"';
  for (const char *p = clean; *p && n + 4 <= cap; p++) {
    if (*p == '"' || *p == '\\') dst[n++] = '\\';
    dst[n++] = *p;
  }
  dst[n++] = '"';
  dst[n] = 0;
  return n;
}

// Roster copy for formatting: taken under dvccCapMux one node at a time, formatted outside it. The receive path takes
// the same spinlock on Core 1 inside ParseMessages() for every identity frame, so the lock is never held for the whole
// 1.9 KB roster; slots are only appended or rewritten in place (dvccNodeSlot), never compacted, so a per-slot copy is
// consistent. Returns a PSRAM DvccNode array the caller frees (void*: a DvccNode in the signature breaks the
// Arduino-generated prototype, see dvccNodeSlot).
static void *n2kRosterCopy(uint8_t *count) {
  *count = 0;
  if (!dvccNodes) return nullptr;
  DvccNode *ns = (DvccNode *)ps_malloc(sizeof(DvccNode) * DVCC_NODE_N);
  if (!ns) return nullptr;
  uint8_t nc = 0;
  for (uint8_t i = 0; i < DVCC_NODE_N; i++) {
    portENTER_CRITICAL(&dvccCapMux);
    if (i >= dvccNodeCount) {
      portEXIT_CRITICAL(&dvccCapMux);
      break;
    }
    ns[i] = dvccNodes[i];
    portEXIT_CRITICAL(&dvccCapMux);
    nc = i + 1;
  }
  *count = nc;
  return ns;
}

static void sendPsramText(AsyncWebServerRequest *request, const char *type, char *buf, size_t n) {
  std::shared_ptr<char> sp(buf, free);
  AsyncWebServerResponse *response = request->beginResponse(type, n,
    [sp, n](uint8_t *out, size_t maxLen, size_t index) -> size_t {
      if (index >= n) return 0;
      size_t tw = n - index;
      if (tw > maxLen) tw = maxLen;
      memcpy(out, sp.get() + index, tw);
      return tw;
    });
  response->addHeader("Cache-Control", "no-cache");
  request->send(response);
}

// GET /n2kNodes: addresses heard within N2K_HEARD_MS, sorted by address, with roster identity where it is known.
static void handleN2kNodes(AsyncWebServerRequest *request) {
  int self = (n2kSrcAddrLive >= 0 && n2kSrcAddrLive <= 251) ? n2kSrcAddrLive : -1;
  uint32_t now = millis();
  uint16_t heard = 0;
  if (n2kLastSeenMs)
    for (int a = 0; a < 255; a++)
      if (n2kLastSeenMs[a] && a != self && (uint32_t)(now - n2kLastSeenMs[a]) <= N2K_HEARD_MS) heard++;
  const size_t PER = 240;  // worst-case entry with every string escaped
  size_t cap = 48 + (size_t)heard * PER;
  char *out = (char *)ps_malloc(cap);
  if (!out) {
    request->send(503, "application/json", "{\"err\":\"out of memory\"}");
    return;
  }
  uint8_t nc = 0;
  DvccNode *ns = (DvccNode *)n2kRosterCopy(&nc);
  size_t n = (size_t)snprintf(out, cap, "{\"self\":%d,\"nodes\":[", self);
  bool first = true;
  for (int a = 0; a < 255 && n2kLastSeenMs && n + PER < cap; a++) {
    uint32_t t = n2kLastSeenMs[a];
    if (!t || a == self || (uint32_t)(now - t) > N2K_HEARD_MS) continue;
    const DvccNode *nd = nullptr;
    for (uint8_t i = 0; ns && i < nc; i++)
      if (ns[i].addr == a) nd = &ns[i];
    n += (size_t)snprintf(out + n, cap - n, "%s{\"addr\":%d,\"ageS\":%lu,\"mfg\":%u,\"func\":%u,\"cls\":%u,\"model\":",
                          first ? "" : ",", a, (unsigned long)((now - t) / 1000UL),
                          (nd && nd->haveName) ? (unsigned)nd->mfg : 0u, (nd && nd->haveName) ? (unsigned)nd->func : 0u,
                          (nd && nd->haveName) ? (unsigned)nd->cls : 0u);
    bool prod = nd && nd->haveProd;
    n += n2kJsonStr(out + n, cap - n, prod ? nd->model : "");
    n += (size_t)snprintf(out + n, cap - n, ",\"sw\":");
    n += n2kJsonStr(out + n, cap - n, prod ? nd->sw : "");
    n += (size_t)snprintf(out + n, cap - n, ",\"serial\":");
    n += n2kJsonStr(out + n, cap - n, prod ? nd->serial : "");
    if (n + 2 < cap) out[n++] = '}';
    first = false;
  }
  if (ns) free(ns);
  n += (size_t)snprintf(out + n, cap - n, "]}");
  sendPsramText(request, "application/json", out, n);
}

// Live state of one /canCapture download. Releasing the download hold lets the ring be freed again; the destructor
// covers an aborted transfer (the request deletes its response, and with it this state).
struct CanCapDl {
  char *hdr = nullptr;
  size_t hdrLen = 0, hdrPos = 0;
  uint32_t i = 0, n = 0;
  char line[64];
  uint8_t lineLen = 0, linePos = 0;
  bool released = false;
  void release() {
    if (released) return;
    released = true;
    portENTER_CRITICAL(&canCapMux);
    if (canCap.dlRefs) canCap.dlRefs--;
    portEXIT_CRITICAL(&canCapMux);
  }
  ~CanCapDl() {
    if (hdr) free(hdr);
    release();
  }
};

static void canCapErr(AsyncWebServerRequest *request, int code, const char *err) {
  char out[160];
  snprintf(out, sizeof(out), "{\"ok\":0,\"err\":\"%s\"}", err);
  request->send(code, "application/json", out);
}

// Takes the ring out of service and frees it, unless a recording or a download holds it. Web task.
static const char *canCapRelease() {
  CanCapEntry *old = nullptr;
  const char *err = nullptr;
  portENTER_CRITICAL(&canCapMux);
  if (canCap.state == 1) err = "a recording is running";
  else if (canCap.dlRefs) err = "a download is in progress";
  else {
    old = canCap.ring;
    canCap.ring = nullptr;
    canCap.cap = 0;
    canCap.count = 0;
    canCap.state = 0;
  }
  portEXIT_CRITICAL(&canCapMux);
  if (old) free(old);
  return err;
}

static void canCapStart(AsyncWebServerRequest *request) {
  int s = 10;
  if (paramOk(request, "s")) s = constrain((int)request->getParam("s")->value().toInt(), 1, 60);
  if (!n2kBusRunning()) {
    canCapErr(request, 409, "NMEA 2000 is not running");
    return;
  }
  const char *err = canCapRelease();
  if (err) {
    canCapErr(request, 409, strcmp(err, "a recording is running") == 0 ? "already recording" : err);
    return;
  }
  // §16.6: min(8192, (free - 256 KB) / entry), refused below 1024. A fragmented heap is met by stepping the size down an
  // eighth per failed try: a failing TLSF malloc is constant-time, where asking for the largest block walks the whole heap.
  size_t freeB = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  size_t n = (freeB > CANCAP_PSRAM_RESERVE) ? (freeB - CANCAP_PSRAM_RESERVE) / sizeof(CanCapEntry) : 0;
  if (n > CANCAP_MAX_N) n = CANCAP_MAX_N;
  CanCapEntry *ring = nullptr;
  while (n >= CANCAP_MIN_N) {
    ring = (CanCapEntry *)ps_malloc(n * sizeof(CanCapEntry));
    if (ring || n == CANCAP_MIN_N) break;
    n -= n / 8;
    if (n < CANCAP_MIN_N) n = CANCAP_MIN_N;
  }
  if (!ring) {
    canCapErr(request, 503, "not enough free PSRAM for a recording");
    return;
  }
  const tNMEA2000_esp32::tBusStats &bs = tNMEA2000_esp32::BusStats;
  canCap.ring = ring;
  canCap.cap = (uint32_t)n;
  canCap.count = 0;
  canCap.txN = 0;
  canCap.why = 0;
  canCap.reqS = (uint8_t)s;
  canCap.durUs = (uint32_t)s * 1000000UL;
  canCap.elapsedUs = 0;
  canCap.selfAddr = (n2kSrcAddrLive >= 0 && n2kSrcAddrLive <= 251) ? n2kSrcAddrLive : -1;
  canCap.tec0 = bs.tec;
  canCap.rec0 = bs.rec;
  canCap.missed0 = bs.rxMissed;
  canCap.overrun0 = bs.rxOverrun;
  canCap.busOff0 = bs.busOffCount;
  canCap.startEpochMs = n2kClockTrusted() ? (uint64_t)timeBase * 1000ULL + (uint32_t)(millis() - timeBaseMillis) : 0;
  canCap.startMs = millis();
  canCap.startUptimeUs = (uint64_t)esp_timer_get_time();
  canCap.startUs = (uint32_t)canCap.startUptimeUs;  // micros() is the low word of the same clock
  portENTER_CRITICAL(&canCapMux);
  canCap.state = 1;
  canCap.active = true;
  portEXIT_CRITICAL(&canCapMux);
  char out[64];
  snprintf(out, sizeof(out), "{\"ok\":1,\"id\":%lu,\"cap\":%lu,\"s\":%d}", (unsigned long)canCap.startMs, (unsigned long)n, s);
  request->send(200, "application/json", out);
}

static void canCapDownload(AsyncWebServerRequest *request) {
  portENTER_CRITICAL(&canCapMux);
  bool ok = (canCap.state == 2 && canCap.ring != nullptr);
  if (ok) canCap.dlRefs++;
  portEXIT_CRITICAL(&canCapMux);
  if (!ok) {
    request->send(404, "text/plain", "No finished recording.");
    return;
  }
  std::shared_ptr<CanCapDl> st(new CanCapDl());
  st->n = canCap.count;
  // Header, formatted once into PSRAM. candump readers skip # lines.
  uint8_t nc = 0;
  DvccNode *ns = (DvccNode *)n2kRosterCopy(&nc);
  size_t cap = 2048 + (size_t)nc * 112;
  st->hdr = (char *)ps_malloc(cap);
  if (!st->hdr) {
    if (ns) free(ns);
    request->send(503, "text/plain", "out of memory");
    return;  // st's destructor releases the download hold
  }
  char addrTxt[12], startTxt[64], nameTs[32];
  if (canCap.selfAddr >= 0) snprintf(addrTxt, sizeof(addrTxt), "%d", canCap.selfAddr);
  else snprintf(addrTxt, sizeof(addrTxt), "none");
  uint32_t upMs = canCap.startMs;
  if (canCap.startEpochMs) {
    time_t es = (time_t)(canCap.startEpochMs / 1000ULL);
    struct tm tmv;
    gmtime_r(&es, &tmv);
    snprintf(startTxt, sizeof(startTxt), "%04d-%02d-%02d %02d:%02d:%02d UTC (epoch %lu)", tmv.tm_year + 1900, tmv.tm_mon + 1,
             tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (unsigned long)es);
    snprintf(nameTs, sizeof(nameTs), "%04d%02d%02d-%02d%02d%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  } else {
    snprintf(startTxt, sizeof(startTxt), "clock not synced");
    snprintf(nameTs, sizeof(nameTs), "uptime-%lus", (unsigned long)(upMs / 1000UL));
  }
  char whyTxt[40];
  if (canCap.why == 2) snprintf(whyTxt, sizeof(whyTxt), "full (%lu frames)", (unsigned long)canCap.cap);
  else snprintf(whyTxt, sizeof(whyTxt), "%s", canCap.why == 3 ? "NMEA 2000 stopped" : "time");
  uint32_t missed = canCap.missed1 - canCap.missed0, overrun = canCap.overrun1 - canCap.overrun0;
  size_t h = (size_t)snprintf(st->hdr, cap,
    "# XREG-010 NMEA 2000 bus recording (candump log)\n"
    "# fw %s, serial %s, our address %s\n"
    "# start %s, uptime %lu.%03lu s; timestamps in %s\n"
    "# requested %u s, recorded %lu.%03lu s, stopped: %s\n"
    "# frames %lu: %lu received, %lu ours (recorded when queued; TEC and bus-offs say if they reached the bus)\n"
    "# received frames lost %lu (queue full %lu, overrun %lu): missing here, not silence\n"
    "# bus-offs %lu (%lu since boot); TEC %lu -> %lu, REC %lu -> %lu\n"
    "# nodes heard (60928 NAME / 126996): addr mfg func class model | sw | serial\n",
    FIRMWARE_VERSION, device_id_hex, addrTxt, startTxt, (unsigned long)(upMs / 1000UL), (unsigned long)(upMs % 1000UL),
    canCap.startEpochMs ? "Unix epoch seconds" : "seconds since boot",
    (unsigned)canCap.reqS, (unsigned long)(canCap.elapsedUs / 1000000UL), (unsigned long)((canCap.elapsedUs / 1000UL) % 1000UL), whyTxt,
    (unsigned long)st->n, (unsigned long)(st->n - canCap.txN), (unsigned long)canCap.txN,
    (unsigned long)(missed + overrun), (unsigned long)missed, (unsigned long)overrun,
    (unsigned long)(canCap.busOff1 - canCap.busOff0), (unsigned long)canCap.busOff1,
    (unsigned long)canCap.tec0, (unsigned long)canCap.tec1, (unsigned long)canCap.rec0, (unsigned long)canCap.rec1);
  for (uint8_t i = 0; i < nc && h + 112 < cap; i++) {
    char m[24], sw[16], sn[16];
    dvccCopyPlainText(m, sizeof(m), ns[i].haveProd ? ns[i].model : "");
    dvccCopyPlainText(sw, sizeof(sw), ns[i].haveProd ? ns[i].sw : "");
    dvccCopyPlainText(sn, sizeof(sn), ns[i].haveProd ? ns[i].serial : "");
    h += (size_t)snprintf(st->hdr + h, cap - h, "# %4u %4u %4u %5u  %s | %s | %s\n", (unsigned)ns[i].addr,
                          (unsigned)ns[i].mfg, (unsigned)ns[i].func, (unsigned)ns[i].cls, m, sw, sn);
  }
  if (nc == 0 && h + 40 < cap) h += (size_t)snprintf(st->hdr + h, cap - h, "# (no identity frames heard)\n");
  if (ns) free(ns);
  st->hdrLen = (h < cap) ? h : cap - 1;
  // The ring is frozen (state 2, held by dlRefs), so frames format straight from it.
  const CanCapEntry *ring = canCap.ring;
  uint64_t base = canCap.startEpochMs ? canCap.startEpochMs * 1000ULL : canCap.startUptimeUs;
  AsyncWebServerResponse *response = request->beginChunkedResponse("text/plain",
    [st, ring, base](uint8_t *buf, size_t maxLen, size_t) -> size_t {
      size_t w = 0;
      while (w < maxLen) {
        if (st->hdrPos < st->hdrLen) {
          size_t c = min(maxLen - w, st->hdrLen - st->hdrPos);
          memcpy(buf + w, st->hdr + st->hdrPos, c);
          w += c;
          st->hdrPos += c;
          continue;
        }
        if (st->linePos >= st->lineLen) {
          if (st->i >= st->n) {
            st->release();
            return w;
          }
          const CanCapEntry &e = ring[st->i++];
          uint64_t ts = base + e.us;
          const bool sff = (e.id & tNMEA2000_esp32::RawSffFlag) != 0;
          int k = snprintf(st->line, sizeof(st->line), sff ? "(%lu.%06lu) can0 %03lX#%s" : "(%lu.%06lu) can0 %08lX#%s",
                           (unsigned long)(ts / 1000000ULL), (unsigned long)(ts % 1000000ULL),
                           (unsigned long)(e.id & (sff ? 0x7FFUL : 0x1FFFFFFFUL)),
                           (e.id & tNMEA2000_esp32::RawRtrFlag) ? "R" : "");
          static const char hx[] = "0123456789ABCDEF";
          for (uint8_t b = 0; b < e.len && b < 8; b++) {
            st->line[k++] = hx[e.data[b] >> 4];
            st->line[k++] = hx[e.data[b] & 0x0F];
          }
          st->line[k++] = '\n';
          st->lineLen = (uint8_t)k;
          st->linePos = 0;
        }
        size_t c = min(maxLen - w, (size_t)(st->lineLen - st->linePos));
        memcpy(buf + w, st->line + st->linePos, c);
        w += c;
        st->linePos += (uint8_t)c;
      }
      return w;
    });
  char cd[96];
  snprintf(cd, sizeof(cd), "attachment; filename=\"xreg-can-%s-%s.log\"", device_id_hex, nameTs);
  response->addHeader("Content-Disposition", cd);
  response->addHeader("Access-Control-Expose-Headers", "Content-Disposition");
  response->addHeader("Cache-Control", "no-cache");
  request->send(response);
}

// GET /canCapture: start=1&s=N | status=1 | free=1 | (none) = download the finished recording. Start and free change
// state, so they refuse a request addressed to another regulator, like the other state-changing GETs; reads do not.
static void handleCanCapture(AsyncWebServerRequest *request) {
  if (request->hasParam("start")) {
    if (refuseMisaddressed(request)) return;
    canCapStart(request);
  } else if (request->hasParam("status")) {
    uint8_t st = canCap.state;
    uint32_t el = 0;
    if (st == 1) el = min((uint32_t)(micros() - canCap.startUs), canCap.durUs) / 1000UL;
    else if (st == 2) el = canCap.elapsedUs / 1000UL;
    char out[128];
    // id = the recording's start millis: lets a log pull tell its own recording from one the owner started after it.
    snprintf(out, sizeof(out), "{\"state\":\"%s\",\"id\":%lu,\"frames\":%lu,\"cap\":%lu,\"elapsedMs\":%lu,\"s\":%u}",
             st == 1 ? "recording" : (st == 2 ? "ready" : "idle"), st ? (unsigned long)canCap.startMs : 0UL,
             (unsigned long)canCap.count, (unsigned long)canCap.cap, (unsigned long)el, st ? (unsigned)canCap.reqS : 0u);
    request->send(200, "application/json", out);
  } else if (request->hasParam("free")) {
    if (refuseMisaddressed(request)) return;
    const char *err = canCapRelease();
    if (err) canCapErr(request, 409, err);
    else request->send(200, "application/json", "{\"ok\":1}");
  } else {
    canCapDownload(request);
  }
}

// GET /n2kbuslog.csv: the per-minute bus history, oldest first, formatted straight from the ring. The writer's sequence
// count is checked around the pass, so a row landing mid-format (once a minute) forces a retry, never a torn line.
static void handleN2kBusLog(AsyncWebServerRequest *request) {
  if (!n2kBusLog || n2kBusLogCount == 0) {
    request->send(200, "text/plain", "No NMEA 2000 bus history yet.");
    return;
  }
  const size_t LINE = 64;  // worst-case row
  size_t cap = 128 + (size_t)N2K_BUSLOG_N * LINE;
  char *out = (char *)ps_malloc(cap);
  if (!out) {
    request->send(503, "text/plain", "bus history unavailable - out of memory");
    return;
  }
  size_t n = 0;
  for (uint8_t tries = 0; tries < 4; tries++) {
    uint32_t q = n2kBusLogSeq;
    if (q & 1) continue;
    __sync_synchronize();
    uint16_t cnt = n2kBusLogCount, oldest = (uint16_t)((n2kBusLogHead + N2K_BUSLOG_N - cnt) % N2K_BUSLOG_N);
    n = (size_t)snprintf(out, cap, "time,time_kind,worst_verdict,tec_max,rec_max,nodes_max,bus_offs,rx_frames,tx_frames,rx_lost,bus_errors\n");
    for (uint16_t k = 0; k < cnt && n + LINE < cap; k++) {
      const N2kBusRow &r = n2kBusLog[(oldest + k) % N2K_BUSLOG_N];
      n += (size_t)snprintf(out + n, cap - n, "%lu,%s,%u,%u,%u,%u,%u,%lu,%u,%u,%u\n", (unsigned long)r.stamp,
                            (r.flags & 1) ? "uptime_s" : "epoch", (unsigned)r.worst, (unsigned)r.tecMax, (unsigned)r.recMax,
                            (unsigned)r.nodesMax, (unsigned)r.busOffs, (unsigned long)r.rxFrames, (unsigned)r.txFrames,
                            (unsigned)r.rxLost, (unsigned)r.busErrors);
    }
    __sync_synchronize();
    if (n2kBusLogSeq == q) break;
    n = 0;
  }
  if (n == 0) {
    free(out);
    request->send(503, "text/plain", "bus history busy - retry");
    return;
  }
  sendPsramText(request, "text/csv", out, n);
}

void setupServer() {

  static bool serverInitialized = false;
  if (serverInitialized) {
    Serial.println("setupServer() already initialized - skipping");
    return;
  }
  serverInitialized = true;
  Serial.println("=== SETTING UP MAIN SERVER ===");

  ensureWebFS();  // Mount appropriate web partition (prod_fs with factory_fs fallback)
  // Route objects (~130 handlers, their URI Strings and list nodes, ~22 KB) are permanent and only touched by
  // async_tcp, so they go to PSRAM. Opened after the web FS mount, closed before the listener starts.
  PsramPlace::mallocWindow(true);

  server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest *request) {  // just get rid of annoying chrome Console error messages
    static const uint8_t blank[] = { 0 };
    request->send(200, "image/x-icon", blank, sizeof(blank));
  });


  server.on("/factoryReset", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (refuseMisaddressed(request)) return;
    if (demoSeal) { demoRefuse(request); return; }
    if (!settingsArmActive(request)) {
      request->send(403, "text/plain", "Settings not armed");
      return;
    }
    Serial.println("\n=== FACTORY RESET INITIATED FROM WEB ===");
    queueConsoleMessage("FACTORY RESET: Initiated from web interface");
    request->send(200, "text/plain", "OK");  // Respond before blocking restart
    performDeepFactoryReset();  // Reformats LittleFS, erases all NVS, restarts; defaults re-seed at boot
  });

  server.on("/thermallog.csv", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!thermalLogReady || !thermalLog || thermalLogCount == 0) {
      request->send(200, "text/plain", "No thermal log data yet.");
      return;
    }
    ThermalDLState state;
    state.count = thermalLogCount;
    state.oldest = (thermalLogHead - thermalLogCount + THERMAL_LOG_SIZE) % THERMAL_LOG_SIZE;
    state.row = 0;
    state.done = false;
    state.lineLen = 0;
    state.linePos = 0;
    thermalLogPaused = true;
    thermalLogPausedAtMs = millis();
    // Abrupt client teardown (Safari) never reaches the clean-completion unpause — release on
    // disconnect, token-guarded so a stale keep-alive closing late can't unfreeze a newer transfer.
    uint32_t dlTok = ++thermalLogDlToken;
    request->onDisconnect([dlTok]() {
      if (dlTok == thermalLogDlToken) thermalLogPaused = false;
    });
    AsyncWebServerResponse *response = request->beginChunkedResponse(
      "text/csv",
      [state](uint8_t *buf, size_t maxLen, size_t) mutable -> size_t {
        thermalLogPausedAtMs = millis();
        if (state.done) return 0;
        size_t written = 0;
        while (written < maxLen) {
          if (state.linePos >= state.lineLen) {
            // Rows: 0=header, 1=constants, data at idx=oldest+row-2 → last data row is count+1.
            if (state.row > state.count + 1) {
              thermalLogPaused = false;
              state.done = true;
              return written;
            }
            if (state.row == 0) {
              state.lineLen = snprintf(
                state.line, sizeof(state.line),
                "ts_ms,tempFilt_F,tempProj_F,nominalTarget_A,"
                "rpmCap_A,voltCap_A,uTarget_A,spLimited_A,"
                "pidErr_A,pidOut_pct,duty_pct,RPM,battV,measAmps_A,"
                "penaltyAmps_A,flags,chargeStageDisplay,"
                "outerP,outerI,lookahead,impliedPenalty,antiWindupFired,thermalSlope_F_sec,"
                "freezeWhy,penaltyRaw_A,iCeil_A\n");
            } else if (state.row == 1) {
              // Constants row; the plotter detects it by "CONST" in ts_ms and draws limit, limit+warn and limit+crit
              // lines. nominalTarget carries the live regulation setpoint in °F.
              state.lineLen = snprintf(
                state.line, sizeof(state.line),
                "CONST,kp=%.6g,ki=%.6g,lookahead=%.1f,limit=%.1f,warn=%.1f,crit=%.1f,kidownfrac=%.3g,slopewin=%.1f\n",
                TempPIDKp,
                TempPIDKi,
                ThermalLookaheadSec,
                TemperatureLimitF,
                TempWarnExcess,
                TempCritExcess,
                TempPIDKiDownFrac,
                ThermalSlopeWindowSec);
            } else {
              // Data rows — index offset by 2 (header + constants row)
              int idx = (state.oldest + state.row - 2) % THERMAL_LOG_SIZE;
              ThermalLogEntry e;
              memcpy(&e, &thermalLog[idx], sizeof(ThermalLogEntry));
              // INT16_MIN = no valid temperature at that row; an empty cell, never a fabricated 0.0F.
              char tFilt[12], tProj[12];
              if (e.tempFiltered == INT16_MIN) tFilt[0] = '\0'; else snprintf(tFilt, sizeof(tFilt), "%.1f", e.tempFiltered / 10.0f);
              if (e.tempProjected == INT16_MIN) tProj[0] = '\0'; else snprintf(tProj, sizeof(tProj), "%.1f", e.tempProjected / 10.0f);
              state.lineLen = snprintf(
                state.line, sizeof(state.line),
                "%lu,%s,%s,%.1f,%.1f,%.1f,%.1f,%.1f,"
                "%.1f,%.1f,%.1f,%d,%.1f,%.1f,%.1f,%u,%u,"
                "%.1f,%.1f,%.1f,%.1f,%u,%.1f,"
                "%u,%.1f,%.1f\n",
                (unsigned long)e.ts,
                tFilt,
                tProj,
                e.nominalTarget / 10.0f,
                e.rpmCap / 10.0f,
                e.voltCap / 10.0f,
                e.uTarget / 10.0f,
                e.spLimited / 10.0f,
                e.pidErr / 10.0f,
                e.pidOut / 10.0f,
                e.duty / 10.0f,
                (int)e.rpm,
                e.battV / 10.0f,
                e.measAmps / 10.0f,
                e.penaltyAmps / 10.0f,
                (unsigned)e.flags,
                (unsigned)e.chargeStageDisplay,
                e.outerTermP / 10.0f,
                e.outerTermI / 10.0f,
                e.outerTermLookahead / 10.0f,
                e.impliedPenalty / 10.0f,
                (unsigned)e.antiWindupFired,
                e.thermalSlope / 1000.0f,
                (unsigned)e.freezeWhy,
                e.penaltyRaw / 10.0f,
                e.holdEstimate / 10.0f);
            }
            state.linePos = 0;
            state.row++;
          }
          size_t canSend = min(maxLen - written,
                               (size_t)(state.lineLen - state.linePos));
          memcpy(buf + written, state.line + state.linePos, canSend);
          written += canSend;
          state.linePos += (int)canSend;
        }
        return written;
      });

    response->addHeader("Content-Disposition", "attachment");
    response->addHeader("Cache-Control", "no-cache");
    request->send(response);
  });

  server.on("/thermallog.bin", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!thermalLogReady || !thermalLog || thermalLogCount == 0) {
      request->send(200, "application/octet-stream", "");
      return;
    }
    ThermalBinDLState state;
    uint32_t cnt = (uint32_t)thermalLogCount;
    uint32_t intMs = THERMAL_LOG_INTERVAL_MS;
    memcpy(state.header, &cnt, 4);
    memcpy(state.header + 4, &intMs, 4);
    state.headerPos = 0;
    state.count = thermalLogCount;
    state.oldest = (thermalLogHead - thermalLogCount + THERMAL_LOG_SIZE) % THERMAL_LOG_SIZE;
    state.row = 0;
    state.done = false;
    state.entryLen = 0;
    state.entryPos = 0;
    thermalLogPaused = true;
    thermalLogPausedAtMs = millis();
    // Token-guarded unpause on client abort (same pattern as /thermallog.csv) — without it a
    // torn-down transfer stays paused until the 30 s watchdog, losing samples.
    uint32_t dlTok = ++thermalLogDlToken;
    request->onDisconnect([dlTok]() {
      if (dlTok == thermalLogDlToken) thermalLogPaused = false;
    });
    AsyncWebServerResponse *response = request->beginChunkedResponse(
      "application/octet-stream",
      [state](uint8_t *buf, size_t maxLen, size_t) mutable -> size_t {
        thermalLogPausedAtMs = millis();
        if (state.done) return 0;
        size_t written = 0;
        while (written < maxLen) {
          // Phase 1: drain 8-byte header
          if (state.headerPos < 8) {
            size_t canSend = min(maxLen - written, (size_t)(8 - state.headerPos));
            memcpy(buf + written, state.header + state.headerPos, canSend);
            written += canSend;
            state.headerPos += (int)canSend;
            continue;
          }
          // Phase 2: stream entries as raw struct bytes
          if (state.entryPos >= state.entryLen) {
            if (state.row >= state.count) {
              thermalLogPaused = false;
              state.done = true;
              return written;
            }
            int idx = (state.oldest + state.row) % THERMAL_LOG_SIZE;
            memcpy(state.entryBuf, &thermalLog[idx], sizeof(ThermalLogEntry));
            state.entryLen = sizeof(ThermalLogEntry);
            state.entryPos = 0;
            state.row++;
          }
          size_t canSend = min(maxLen - written, (size_t)(state.entryLen - state.entryPos));
          memcpy(buf + written, state.entryBuf + state.entryPos, canSend);
          written += canSend;
          state.entryPos += (int)canSend;
        }
        return written;
      });
    response->addHeader("Cache-Control", "no-cache");
    request->send(response);
  });

  server.on("/pidlog.csv", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!pidLogReady || !pidLog || pidLogCount == 0) {
      request->send(200, "text/plain", "No PID log data yet.");
      return;
    }

    PidDLState state;
    state.count = pidLogCount;
    state.oldest = (pidLogHead - pidLogCount + PID_LOG_SIZE) % PID_LOG_SIZE;
    state.row = 0;
    state.done = false;
    state.lineLen = 0;
    state.linePos = 0;

    pidLogPaused = true;
    pidLogPausedAtMs = millis();
    // Same Safari abrupt-teardown release as /thermallog.csv.
    uint32_t dlTok = ++pidLogDlToken;
    request->onDisconnect([dlTok]() {
      if (dlTok == pidLogDlToken) pidLogPaused = false;
    });

    AsyncWebServerResponse *response = request->beginChunkedResponse(
      "text/csv",
      [state](uint8_t *buf, size_t maxLen, size_t) mutable -> size_t {
        // Refresh pause watchdog on every chunk callback
        pidLogPausedAtMs = millis();

        if (state.done) return 0;

        size_t written = 0;

        while (written < maxLen) {

          if (state.linePos >= state.lineLen) {

            // ── Row 0: comment block ──────────────────────────────────────
            if (state.row == 0) {
              state.lineLen = snprintf(
                state.line, sizeof(state.line),
                "# PID diagnostic log — CV loop / output current PID / duty pipeline\n"
                "# flags: bit0=AUTO bit1=voltCtrl bit4=govBypass\n"
                "# ovFlags: bit0=fastOvActive bit1=iExcessBulk(current-control phase) bit2=hardClamp bit3=iExcess bit4=loadDumpActive\n"
                "# voltageLoopRanThisTick=1 means Icv/cv_I updated this row\n"
                "# vError: always fresh every tick regardless of loop interval\n"
                "# Icv: CV position-form PI output — the direct current setpoint in CV modes\n"
                "# cv_I: CV position-form PI integrator state\n"
                "# tableThermalLimit: RPM cap minus thermal penalty, before CV\n"
                "# setpointCmd: Icv in CV modes, tableThermalLimit in bulk\n"
                "# dBcur_dt: battery current derivative (A/s) — positive = load dump event\n"
                "# battI: battery current from INA228 or Victron (A)\n");
              state.lineLen = min((int)state.lineLen, (int)sizeof(state.line) - 1);

            } else if (state.row == 1) {
              state.lineLen = snprintf(
                state.line, sizeof(state.line),
                "ts_ms,"
                "chargeStageDisplay,"
                "TargetVoltageMode,"
                "battV,"
                "ChargingVoltageTarget,"
                "vError,"
                "Icv,"
                "cv_I,"
                "tableThermalLimit,"
                "setpointCmd,"
                "voltageLoopRanThisTick,"
                "pidSetpoint,"
                "pidInput,"
                "pidUnsatOutput,"
                "pidOutput,"
                "innerTermP,"
                "innerTermI,"
                "innerTermD,"
                "dutyRequest,"
                "dutyApplied,"
                "enteringCV,"
                "enteringTargetVoltageMode,"
                "rpm,"
                "measAmps,"
                "innerKp,"
                "innerKi,"
                "innerKd,"
                "voltageKp,"
                "voltageKi,"
                "battV_filt_V,"
                "flags,"
                "ovFlags,"
                "dBcur_dt,"
                "battI,"
                "ch1IntervalMs,"
                "voltLoopIntervalMs,"
                "inaIntervalMs,"
                "mExcessEma,"
                "iExcessThreshold\n");

              state.lineLen = min((int)state.lineLen, (int)sizeof(state.line) - 1);

              // ── Row 2+: data rows ─────────────────────────────────────────
            } else {
              int dataRow = state.row - 2;
              if (dataRow >= state.count) {
                pidLogPaused = false;
                state.done = true;
                return written;
              }

              int idx = (state.oldest + dataRow) % PID_LOG_SIZE;
              PidLogEntry e;
              memcpy(&e, &pidLog[idx], sizeof(PidLogEntry));

              // No battery shunt -> battI is INA input noise and dBcur_dt its slope. Empty cells,
              // not numbers: this file gets loaded into pandas and read as ground truth.
              char dBcurS[16], battIS[16];
              if (HAS_BATT_SHUNT) {
                snprintf(dBcurS, sizeof(dBcurS), "%.2f", e.dBcur_dt);
                snprintf(battIS, sizeof(battIS), "%.3f", e.battI);
              } else {
                dBcurS[0] = '\0';
                battIS[0] = '\0';
              }

              state.lineLen = snprintf(
                state.line, sizeof(state.line),
                "%lu,"
                "%u,%u,"
                "%.3f,%.3f,%.4f,"
                "%.3f,%.4f,"     // Icv, cv_I
                "%.3f,%.3f,%u,"  // tableThermalLimit, setpointCmd, voltageLoopRanThisTick
                "%.3f,%.3f,%.3f,%.3f,"
                "%.4f,%.4f,%.4f,"
                "%.2f,%.2f,"
                "%u,%u,"
                "%.0f,%.2f,"
                "%.4f,%.4f,%.4f,"   // innerKp, innerKi, innerKd
                "%.4f,%.4f,"   // voltageKp, voltageKi
                "%.3f,"  // battV_filt
                "%u,%u,"          // flags, ovFlags
                "%s,%s,"          // dBcur_dt, battI (empty when no battery shunt)
                "%d,%d,%d,"       // ch1IntervalMs, voltLoopIntervalMs, inaIntervalMs
                "%.3f,%.3f\n",     // mExcessEma, iExcessThreshold (A)
                (unsigned long)e.ts,
                (unsigned)e.chargeStageDisplay,
                (unsigned)e.TargetVoltageMode,
                e.battV,
                e.ChargingVoltageTarget,
                e.vError,
                e.Icv,
                e.cv_I,
                e.tableThermalLimit,
                e.setpointCmd,
                (unsigned)e.voltageLoopRanThisTick,
                e.pidSetpoint,
                e.pidInput,
                e.pidUnsatOutput,
                e.pidOutput,
                e.innerTermP,
                e.innerTermI,
                e.innerTermD,
                e.dutyRequest,
                e.dutyApplied,
                (unsigned)e.enteringCV,
                (unsigned)e.enteringTargetVoltageMode,
                e.rpm,
                e.measAmps,
                e.innerKp,
                e.innerKi,
                e.innerKd,
                e.voltageKp,
                e.voltageKi,
                e.battV_filt,
                (unsigned)e.flags,
                (unsigned)e.ovFlags,
                dBcurS,
                battIS,
                (int)e.ch1IntervalMs,
                (int)e.voltLoopIntervalMs,
                (int)e.inaIntervalMs,
                e.mExcessEma,
                e.iExcessThreshold);
              state.lineLen = min((int)state.lineLen, (int)sizeof(state.line) - 1);
            }

            state.linePos = 0;
            state.row++;
          }

          size_t canSend = min(maxLen - written,
                               (size_t)(state.lineLen - state.linePos));
          memcpy(buf + written, state.line + state.linePos, canSend);
          written += canSend;
          state.linePos += (int)canSend;
        }

        return written;
      });

    response->addHeader("Content-Disposition", "attachment");
    response->addHeader("Cache-Control", "no-cache");
    request->send(response);
  });

  // ── Hunt-governor episode ledger (Hunt_Governor_Spec.md): one CSV line per damping episode ──
  // Serves flash PLUS episodes still queued in RAM (they reach flash only ~30 s after the field gate is cut,
  // huntLedgerService). Read-only by design: never writes flash.
  server.on("/huntledger", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (demoServe(request, "/huntledger")) return;   // demo mode: made-up history (9_demo.ino)
    // Queue counted and rendered under the FS lock too: huntLedgerService moves it to the file and
    // zeroes it under this same lock, so a flush between the two reads would drop those rows from both.
    fsTakeLock();
    uint8_t pendN = hgLedgerPendingCount();
    size_t pendCap = (size_t)pendN * 96 + 96;
    File f = LittleFS.open("/huntledger.csv", "r");
    size_t fsz = f ? f.size() : 0;
    if (fsz == 0 && pendN == 0) {
      if (f) f.close();
      fsReleaseLock();
      request->send(200, "text/plain", "no hunt episodes recorded");
      return;
    }
    const size_t bufCap = fsz + pendCap + 1;
    char *buf = (char *)ps_malloc(bufCap);  // file is hard-capped at HG_LEDGER_MAX_BYTES
    size_t n = 0;
    if (buf) {
      if (fsz) n = f.read((uint8_t *)buf, fsz);
      else n = (size_t)snprintf(buf, pendCap, "%s\n", "epoch,rpm,cv,freqHz,a0_pct,aEnd_pct,steps,verdict,derateExit");
      if (n && buf[n - 1] != '\n') buf[n++] = '\n';
      n += hgLedgerPendingCsv(buf + n, bufCap - n);
    }
    if (f) f.close();
    fsReleaseLock();
    if (!buf) {
      request->send(503, "text/plain", "hunt ledger unavailable — out of memory");
      return;
    }
    // Streamed straight out of PSRAM — passing a char* to send() would copy the whole ledger into
    // an Arduino String on the internal heap. The shared_ptr deleter frees on any completion path.
    std::shared_ptr<char> sp(buf, free);
    AsyncWebServerResponse *response = request->beginResponse("text/plain", n,
      [sp, n](uint8_t *out, size_t maxLen, size_t index) -> size_t {
        if (index >= n) return 0;
        size_t tw = n - index;
        if (tw > maxLen) tw = maxLen;
        memcpy(out, sp.get() + index, tw);
        return tw;
      });
    response->addHeader("Cache-Control", "no-cache");
    request->send(response);
  });

  // Live damper speed-pocket map for the Diag plot: cut/wing/pockets in one JSON so the client
  // renders exactly the profile the firmware applies. Tiny (≤4 pockets) — no PSRAM streaming.
  server.on("/huntmap", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (demoServe(request, "/huntmap")) return;   // demo mode: made-up history (9_demo.ino)
    char buf[384];
    size_t n = huntMapJson(buf, sizeof(buf));
    AsyncWebServerResponse *response = request->beginResponse(200, "application/json", String(buf, n));
    response->addHeader("Cache-Control", "no-cache");
    request->send(response);
  });

  // Clear System for the oscillation damper: learned pockets + episode ledger + any test in
  // flight, together. The web UI fronts this with an explicit confirm.
  server.on("/huntclear", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (refuseMisaddressed(request)) return;
    if (demoSeal) { demoRefuse(request); return; }
    if (!gxFlagged(request) && !settingsArmActive(request)) {
      request->send(403, "text/plain", "Settings not armed");
      return;
    }
    huntMapClearAll("cleared from the dashboard");
    request->send(200, "text/plain", "OK");
  });

  // ── DVCC raw-frame capture (spec §8a): candidate CAN frames as hex, for charge-limit decoder ground truth.
  // Observe-only; reading never touches control. Line format: "ms id_hex len data_hex", oldest first.
  server.on("/dvccCapture", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!dvccCapRing || dvccCapCount == 0) {
      request->send(200, "text/plain", "no candidate frames captured yet (needs NMEA2K receive or DVCC follow enabled, and bus traffic)");
      return;
    }
    // Pause the tap while copying: the oldest slots are the next ones a concurrent write overwrites, and a torn line would
    // corrupt the ground truth. Setting the pause and reading head/count under dvccCapMux (the tap holds it for its whole
    // slot write) guarantees no write is in flight; formatting then runs with the tap live, off the private copy.
    portENTER_CRITICAL(&dvccCapMux);
    dvccCapPause = true;
    uint16_t cnt = dvccCapCount;
    uint16_t head = dvccCapHead;
    portEXIT_CRITICAL(&dvccCapMux);
    DvccCapEntry *snap = (DvccCapEntry *)ps_malloc((size_t)cnt * sizeof(DvccCapEntry));
    if (!snap) {
      dvccCapPause = false;
      request->send(503, "text/plain", "capture unavailable - out of memory");
      return;
    }
    uint16_t start = (uint16_t)((head + DVCC_CAP_N - cnt) % DVCC_CAP_N);
    uint16_t run = (uint16_t)((start + cnt > DVCC_CAP_N) ? (DVCC_CAP_N - start) : cnt);  // entries before the ring wraps
    memcpy(snap, dvccCapRing + start, (size_t)run * sizeof(DvccCapEntry));
    if (run < cnt) memcpy(snap + run, dvccCapRing, (size_t)(cnt - run) * sizeof(DvccCapEntry));
    dvccCapPause = false;
    size_t cap = (size_t)cnt * 44 + 4480;  // + header: run stamp, authority, WiFi carrier, node roster, battery record, the not-the-battery's-number note
    char *buf = (char *)ps_malloc(cap);
    if (!buf) {
      free(snap);
      request->send(503, "text/plain", "capture unavailable - out of memory");
      return;
    }
    size_t n = 0;
    // Header first: who published the frames and under which firmware. The roster is copied under the lock and
    // formatted unlocked; snprintf must never run inside the critical section.
    char authTxt[24];
    if (dvccRxSrcAddr == 255) snprintf(authTxt, sizeof(authTxt), "none heard yet");
    else if (dvccRxSrcAddr == DVCC_ADDR_WIFI) snprintf(authTxt, sizeof(authTxt), "WiFi only");
    else snprintf(authTxt, sizeof(authTxt), "addr %u", (unsigned)dvccRxSrcAddr);
    const unsigned long wifiTs = dataTimestamps[IDX_DVCC_WIFI];
    n += (size_t)snprintf(buf + n, cap - n,
                          "# XREG-010 DVCC capture - fw %s - uptime %lu ms - %u frames\n"
                          "# dialect %s - RV-C instance filter %d - follow state %u - authority %s\n"
                          "# WiFi carrier (/gxLimits): CVL %.2f V, CCL %.1f A (nan = not sent), last post %ld ms ago (-1 = never)\n"
                          "# nodes heard passively (PGN 60928 NAME / 126996 product info). Hearing nothing is\n"
                          "# normal with transmit off: neither PGN can be requested, so this is only what the\n"
                          "# bus volunteered while we were listening.\n"
                          "# addr  mfg func class   unique  prod vprod  model | sw | serial\n",
                          FIRMWARE_VERSION, (unsigned long)millis(), (unsigned)cnt,
                          dvccSrcType == 1 ? "RV-C" : "Victron GX", dvccInst, (unsigned)dvccState, authTxt,
                          (double)dvccRxWifiCvl, (double)dvccRxWifiCcl, wifiTs ? (long)(millis() - wifiTs) : -1L);
    DvccNode *nsnap = (DvccNode *)ps_malloc(sizeof(DvccNode) * DVCC_NODE_N);
    uint8_t nodeCnt = 0;
    if (nsnap && dvccNodes) {
      portENTER_CRITICAL(&dvccCapMux);
      nodeCnt = dvccNodeCount;
      if (nodeCnt) memcpy(nsnap, dvccNodes, sizeof(DvccNode) * (size_t)nodeCnt);
      portEXIT_CRITICAL(&dvccCapMux);
    }
    for (uint8_t i = 0; i < nodeCnt && n + 160 < cap; i++) {
      const DvccNode &nd = nsnap[i];
      n += (size_t)snprintf(buf + n, cap - n, "# %4u %4u %4u %5u %08lX %5u %5u  %s | %s | %s\n",
                            (unsigned)nd.addr, (unsigned)nd.mfg, (unsigned)nd.func, (unsigned)nd.cls,
                            (unsigned long)nd.unique, (unsigned)nd.prodCode, (unsigned)nd.vicProdId,
                            nd.model, nd.sw, nd.serial);
    }
    if (nodeCnt == 0) n += (size_t)snprintf(buf + n, cap - n, "# (no identity frames heard)\n");
    if (nsnap) free(nsnap);
    // The battery behind a GX never reaches our bus, so stamp the owner's Vessel Info battery record. Sanitised on the
    // way out: the stored strings pre-date any input filter.
    char battTxt[80], battChem[24];
    dvccCopyPlainText(battTxt, sizeof(battTxt), BATTERY_MAKE_MODEL);
    dvccCopyPlainText(battChem, sizeof(battChem), BATTERY_TYPE);
    if (battTxt[0]) {
      n += (size_t)snprintf(buf + n, cap - n, "# battery on this boat: %s | %s | %d Ah | %u V\n",
                            battTxt, battChem[0] ? battChem : "chemistry not set",
                            BatteryCapacity_Ah, (unsigned)SYSTEM_VOLTAGE_CLASS);
    } else {
      n += (size_t)snprintf(buf + n, cap - n,
                            "# battery on this boat: NOT RECORDED - fill in Setup - Vessel Information, or send\n"
                            "# the battery make and model along with this capture.\n");
    }
    n += (size_t)snprintf(buf + n, cap - n,
                          "# NOTE: a limit published by a Victron GX is Victron's own computed number, not the\n"
                          "# battery's - it is rewritten per battery brand, which is why the line above matters.\n");
    for (uint16_t i = 0; i < cnt && n + 44 < cap; i++) {
      const DvccCapEntry &e = snap[i];
      n += (size_t)snprintf(buf + n, cap - n, "%lu %08lX %u ", (unsigned long)e.ms, (unsigned long)e.id, (unsigned)e.len);
      for (uint8_t b = 0; b < e.len && b < 8; b++) n += (size_t)snprintf(buf + n, cap - n, "%02X", e.data[b]);
      if (n < cap - 1) buf[n++] = '\n';
    }
    free(snap);  // both buffers are PSRAM; hand the ~10 KB copy back before the response holds the text
    // Streamed out of PSRAM, same pattern as /huntledger.
    std::shared_ptr<char> sp(buf, free);
    AsyncWebServerResponse *response = request->beginResponse("text/plain", n,
      [sp, n](uint8_t *out, size_t maxLen, size_t index) -> size_t {
        if (index >= n) return 0;
        size_t tw = n - index;
        if (tw > maxLen) tw = maxLen;
        memcpy(out, sp.get() + index, tw);
        return tw;
      });
    response->addHeader("Cache-Control", "no-cache");
    request->send(response);
  });

  server.on("/n2kNodes", HTTP_GET, handleN2kNodes);
  server.on("/canCapture", HTTP_GET, handleCanCapture);
  server.on("/n2kbuslog.csv", HTTP_GET, handleN2kBusLog);

  // ── Console history: device-side source of truth for the Download Logs console file ──
  // Lines: epoch \t millis \t message (pre-2020 epoch = clock unsynced, client shows uptime). Entries are memcpy'd under
  // consoleMux so an append can't tear a line; no pause flag, the ring has no tick-driven writer.
  server.on("/consolehist.txt", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!consoleHist || consoleHistCount == 0) {
      request->send(200, "text/plain", "");
      return;
    }
    struct ChDLState {
      int count; int oldest; int row; bool done; int lineLen; int linePos;
      char line[CONSOLE_MSG_LEN + 48];
    };
    ChDLState state;
    memset(&state, 0, sizeof(state));
    portENTER_CRITICAL(&consoleMux);
    state.count = consoleHistCount;
    state.oldest = (consoleHistHead - consoleHistCount + CONSOLE_HIST_SIZE) % CONSOLE_HIST_SIZE;
    portEXIT_CRITICAL(&consoleMux);
    AsyncWebServerResponse *response = request->beginChunkedResponse(
      "text/plain",
      [state](uint8_t *buf, size_t maxLen, size_t) mutable -> size_t {
        if (state.done) return 0;
        size_t written = 0;
        while (written < maxLen) {
          if (state.linePos >= state.lineLen) {
            if (state.row >= state.count) {
              state.done = true;
              return written;
            }
            int idx = (state.oldest + state.row) % CONSOLE_HIST_SIZE;
            ConsoleHistEntry e;
            portENTER_CRITICAL(&consoleMux);
            memcpy(&e, &consoleHist[idx], sizeof(ConsoleHistEntry));
            portEXIT_CRITICAL(&consoleMux);
            state.lineLen = snprintf(state.line, sizeof(state.line), "%lld\t%lu\t%s\n",
                                     (long long)e.epoch, (unsigned long)e.ms, e.msg);
            state.lineLen = min((int)state.lineLen, (int)sizeof(state.line) - 1);
            state.linePos = 0;
            state.row++;
          }
          size_t canSend = min(maxLen - written, (size_t)(state.lineLen - state.linePos));
          memcpy(buf + written, state.line + state.linePos, canSend);
          written += canSend;
          state.linePos += (int)canSend;
        }
        return written;
      });
    response->addHeader("Cache-Control", "no-cache");
    // Device uptime at send: the browser places ring lines by age on its own clock, so the post-outage splice needs no
    // time source (AP-mode users have none). Exposed for the app's cross-origin fetch.
    char nowBuf[16];
    snprintf(nowBuf, sizeof(nowBuf), "%lu", (unsigned long)millis());
    response->addHeader("X-Device-Millis", nowBuf);
    response->addHeader("Access-Control-Expose-Headers", "X-Device-Millis");
    request->send(response);
  });

  // ── Fast alternator-current channel: live scope dump ──
  // 500 ms raw ring (20 kSPS, calibrated mV) + 24 B header — see faScopeSnapshot for layout.
  // Snapshot copied into PSRAM up front (spinlock vs the loop() drain), then streamed.
  server.on("/fastscope.bin", HTTP_GET, [](AsyncWebServerRequest *request) {
    const size_t cap = 24 + (size_t)FA_RAW_RING_N * 2;  // header (24 B) + full scope ring
    uint8_t *buf = (uint8_t *)ps_malloc(cap);
    if (!buf) {
      request->send(503, "text/plain", "no mem");
      return;
    }
    size_t n = faScopeSnapshot(buf, cap);
    if (n == 0) {
      free(buf);
      request->send(503, "text/plain", "snapshot failed");
      return;
    }
    std::shared_ptr<uint8_t> sp(buf, free);
    AsyncWebServerResponse *response = request->beginResponse("application/octet-stream", n,
      [sp, n](uint8_t *out, size_t maxLen, size_t index) -> size_t {
        if (index >= n) return 0;
        size_t tw = n - index;
        if (tw > maxLen) tw = maxLen;
        memcpy(out, sp.get() + index, tw);
        return tw;
      });
    response->addHeader("Cache-Control", "no-cache");
    request->send(response);
  });

  // ── Fast alternator-current channel: reference flipbook dump ──
  // Header (8 B LE): u32 magic 'FFLP', u8 refSlots, u8 anomSlots, u16 sampleRate/10.
  // Then (refSlots+anomSlots) × FaFlipPage verbatim (8020 B each, layout pinned by
  // static_assert in 2_functions.ino). Pages freeze once captured, so no lock is needed;
  // `used` is set last on capture so a mid-capture read just sees an empty slot.
  server.on("/faflip.bin", HTTP_GET, [](AsyncWebServerRequest *request) {
    const size_t pgSize = sizeof(FaFlipPage);
    const size_t total = 8 + pgSize * FA_FLIP_SLOTS;
    uint8_t *buf = (uint8_t *)ps_malloc(total);
    if (!buf || !faFlip) {
      if (buf) free(buf);
      request->send(503, "text/plain", "no data");
      return;
    }
    uint32_t magic = 0x46464C50UL;  // 'FFLP'
    uint16_t rateDiv10 = FA_SAMPLE_RATE_HZ / 10;  // 20 kSPS (full rate, matches the live scope)
    memcpy(buf + 0, &magic, 4);
    buf[4] = FA_FLIP_BANDS;
    buf[5] = FA_FLIP_ANOM;
    memcpy(buf + 6, &rateDiv10, 2);
    memcpy(buf + 8, faFlip, pgSize * FA_FLIP_SLOTS);
    std::shared_ptr<uint8_t> sp(buf, free);
    AsyncWebServerResponse *response = request->beginResponse("application/octet-stream", total,
      [sp, total](uint8_t *out, size_t maxLen, size_t index) -> size_t {
        if (index >= total) return 0;
        size_t tw = total - index;
        if (tw > maxLen) tw = maxLen;
        memcpy(out, sp.get() + index, tw);
        return tw;
      });
    response->addHeader("Cache-Control", "no-cache");
    request->send(response);
  });

  // ── Fast alternator-current channel: disturbance matrix CSV export ──
  // One row per populated cell (RPM bin × amps bin): top-6 mean-accumulated peaks +
  // broadband pk-pk + window count. Chunked like /alttrend.csv. Reads race the Core-1
  // merge harmlessly (diagnostic export — a torn cell is one stale row).
  server.on("/famatrix.csv", HTTP_GET, [](AsyncWebServerRequest *request) {
    struct FmExp { int idx; bool header, done; char line[280]; int len, pos; };
    FmExp st;
    st.idx = 0; st.header = true; st.done = false; st.len = 0; st.pos = 0;
    AsyncWebServerResponse *response = request->beginChunkedResponse("text/csv",
      [st](uint8_t *buf, size_t maxLen, size_t) mutable -> size_t {
        if (st.done) return 0;
        size_t written = 0;
        const int NCELLS = FA_RPM_BINS * FA_AMP_BINS;
        while (written < maxLen) {
          if (st.pos >= st.len) {
            if (st.header) {
              st.len = snprintf(st.line, sizeof(st.line),
                                "rpmLo,ampLo,windows,pkpkA,f1Hz,a1A,n1,f2Hz,a2A,n2,f3Hz,a3A,n3,f4Hz,a4A,n4,f5Hz,a5A,n5,f6Hz,a6A,n6\n");
              st.header = false;
            } else {
              while (st.idx < NCELLS && faMatrix && faMatrix[st.idx].windows == 0) st.idx++;
              if (st.idx >= NCELLS || !faMatrix) { st.done = true; return written; }
              FaCell *c = &faMatrix[st.idx];
              int rpmLo = (st.idx / FA_AMP_BINS) * FA_RPM_BIN_W;
              int ampLo = FA_AMP_BIN_LO + (st.idx % FA_AMP_BINS) * FA_AMP_BIN_W;
              int l = snprintf(st.line, sizeof(st.line), "%d,%d,%u,%.2f",
                               rpmLo, ampLo, (unsigned)c->windows, c->pkpkAX100 / 100.0);
              for (int s = 0; s < FA_CELL_PEAKS; s++)
                l += snprintf(st.line + l, sizeof(st.line) - l, ",%.1f,%.2f,%u",
                              c->pk[s].freqHzX10 / 10.0, c->pk[s].ampAX100 / 100.0, (unsigned)c->pk[s].nAcc);
              l += snprintf(st.line + l, sizeof(st.line) - l, "\n");
              st.len = l;
              st.idx++;
            }
            st.pos = 0;
          }
          size_t tw = min((size_t)(st.len - st.pos), maxLen - written);
          memcpy(buf + written, st.line + st.pos, tw);
          written += tw;
          st.pos += (int)tw;
        }
        return written;
      });
    response->addHeader("Cache-Control", "no-cache");
    request->send(response);
  });

  // ── Measured filtered ripple per RPM bin (RPM_RIPPLE_TABLE_SPEC) ──
  // One row per 50-RPM ripTab bin with any state: IExcessTau-filtered pk-pk at the game's fixed commanded current.
  // Columns [0..2] are a fixed layout (parsers check length >= 3); altSt/battSt: 0 none, 1 pending (value = candidate),
  // 2 committed. The wizard pick and the game's kill/coverage logic use committed only.
  server.on("/filtripple.csv", HTTP_GET, [](AsyncWebServerRequest *request) {
    String out = "rpmLo,altFiltPkA,battFiltPkA,altSt,battSt\n";
    for (int r = 0; r < RIPTAB_BINS; r++) {
      const RipTabCell *c = &ripTab.cell[r];
      if (c->state == 0) continue;
      int altSt  = (c->state & RIPTAB_ALT_DONE)  ? 2 : (c->state & RIPTAB_ALT_PEND)  ? 1 : 0;
      int battSt = (c->state & RIPTAB_BATT_DONE) ? 2 : (c->state & RIPTAB_BATT_PEND) ? 1 : 0;
      uint16_t altV  = (altSt == 2)  ? c->altPkX100  : c->altPendX100;
      uint16_t battV = (battSt == 2) ? c->battPkX100 : c->battPendX100;
      out += String(r * FA_RPM_BIN_W);
      out += ',';
      out += String(altV / 100.0f, 2);
      out += ',';
      out += String(battV / 100.0f, 2);
      out += ',';
      out += String(altSt);
      out += ',';
      out += String(battSt);
      out += '\n';
    }
    AsyncWebServerResponse *response = request->beginResponse(200, "text/csv", out);
    response->addHeader("Cache-Control", "no-cache");
    request->send(response);
  });

  // ── RPM ripple table session stamps (RPM_RIPPLE_TABLE_SPEC §4) ──
  // "captured at N A, 13.1–13.6 V" for the wizard + Protections overlay annotation. levelA 0 = no
  // sweep recorded yet; ibv fields 0 when no window folded.
  server.on("/riptabmeta", HTTP_GET, [](AsyncWebServerRequest *request) {
    bool ibvOk = (ripTab.sess.ibvMaxV >= ripTab.sess.ibvMinV);
    String out = "{\"level\":" + String(ripTab.sess.levelA, 1)
               + ",\"ibvMin\":" + String(ibvOk ? ripTab.sess.ibvMinV : 0.0f, 2)
               + ",\"ibvMax\":" + String(ibvOk ? ripTab.sess.ibvMaxV : 0.0f, 2)
               + ",\"idleRpm\":" + String(ripTab.sess.idleRpm)
               + ",\"epoch\":" + String(ripTab.sess.epoch)
               + ",\"gameActive\":" + String((ripGameFill || ripTabPendingWipe) ? 1 : 0) + "}";
    AsyncWebServerResponse *response = request->beginResponse(200, "application/json", out);
    response->addHeader("Cache-Control", "no-cache");
    request->send(response);
  });

  // ── Filtered-ripple fold forensics (diagnostic): one row per ripTab fold event, oldest first ──
  // event: P pending set, D disagree (otherA = the pending it replaced), C commit (otherA = pair partner; committed =
  // their average). crossings ~2 = one-shot transient, many = sustained oscillation.
  server.on("/ripforensic.csv", HTTP_GET, [](AsyncWebServerRequest *request) {
    String out = "detector,rpm,ampLo,meanA,pkpkA,meanShiftA,cmdTravelA,crossings,event,otherA\n";
    uint16_t n = ripForensicCount < RIP_FORENSIC_CAP ? ripForensicCount : RIP_FORENSIC_CAP;
    for (uint16_t i = 0; i < n; i++) {
      uint16_t idx = (ripForensicHead + RIP_FORENSIC_CAP - n + i) % RIP_FORENSIC_CAP;
      const RipForensicPt &p = ripForensic[idx];
      out += (p.detector == 0 ? "alt" : "batt");
      out += ',' + String(p.rpm) + ',' + String(p.ampLo) + ',';
      out += String(p.meanX100 / 100.0f, 2) + ',' + String(p.pkpkX100 / 100.0f, 2) + ',';
      out += String(p.shiftX100 / 100.0f, 2) + ',' + String(p.cmdTravelX100 / 100.0f, 2) + ',';
      out += String(p.crossings) + ',' + String(p.event) + ',' + String(p.otherX100 / 100.0f, 2) + '\n';
    }
    AsyncWebServerResponse *response = request->beginResponse(200, "text/csv", out);
    response->addHeader("Cache-Control", "no-cache");
    request->send(response);
  });

  // Active 3-current resonance test points: (rpm, operating current, pk-pk) per window since arm. Small
  // (≤BCUR_RTEST_CAP rows) → plain non-chunked response. Browser fits ripple = a0 + a1·I from these.
  server.on("/bcurrtest.csv", HTTP_GET, [](AsyncWebServerRequest *request) {
    String out = "rpm,iA,pkpkA,iAltA,pkpkAltA,slopeVps\n";  // iA/pkpkA = battery (INA); iAltA/pkpkAltA = alternator (ADS); slopeVps = worst positive g_cvKdFiltV slope (D-term deadband)
    uint16_t n = bcurRtestCount; if (n > BCUR_RTEST_CAP) n = BCUR_RTEST_CAP;
    for (uint16_t i = 0; i < n; i++) {
      out += String(bcurRtest[i].rpm);
      out += ',';
      out += String(bcurRtest[i].iX100 / 100.0f, 2);
      out += ',';
      out += String(bcurRtest[i].pkpkX100 / 100.0f, 2);
      out += ',';
      out += String(bcurRtest[i].iAltX100 / 100.0f, 2);
      out += ',';
      out += String(bcurRtest[i].altPkpkX100 / 100.0f, 2);
      out += ',';
      out += String(bcurRtest[i].slopeVpsX1000 / 1000.0f, 3);
      out += '\n';
    }
    AsyncWebServerResponse *response = request->beginResponse(200, "text/csv", out);
    response->addHeader("Cache-Control", "no-cache");
    request->send(response);
  });

  // ── Measured ripple projection (§3.3/§4): stored per-detector fit ripple(I)=a0+a1·I, its 3 measured (I, pk-pk) points
  // and test RPM; n=0 = no test yet. Read by the Protections tab and the Step-6 review. "slp" = same shape for the CV
  // D-term deadband (worst positive voltage slope, V/s, vs I).
  server.on("/ripfit", HTTP_GET, [](AsyncWebServerRequest *request) {
    auto j = [](const RipFit &r) {
      String s = "{\"a0\":" + String(r.a0, 4) + ",\"a1\":" + String(r.a1, 5)
               + ",\"rpm\":" + String(r.rpm, 0) + ",\"n\":" + String((int)r.nPts) + ",\"i\":[";
      for (int k = 0; k < 3; k++) { if (k) s += ','; s += String(r.iPt[k], 2); }
      s += "],\"pk\":[";
      for (int k = 0; k < 3; k++) { if (k) s += ','; s += String(r.pkPt[k], 3); }
      s += "]}";
      return s;
    };
    request->send(200, "application/json", "{\"alt\":" + j(ripFitAlt) + ",\"slp\":" + j(slpFitAlt) + "}");
  });

  // ── Alternator (charging-system) health v2 — schema + curve + records + trend exports ──
  // Self-describing schema; the dashboard zips these names against AltLive/AltSettings SSE values.
  server.on("/altschema", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", altSchemaJson());
  });
  server.on("/installid", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", installId + "|" + FIRMWARE_VERSION);
  });
  // Network identity for the ESP32 tab's Connectivity & Cloud card — SSID/IP are strings,
  // so they ride this tiny JSON instead of the numeric CSV channels. Fetched on tab open.
  server.on("/netinfo", HTTP_GET, [](AsyncWebServerRequest *request) {
    bool client = (currentMode == MODE_CLIENT);
    String ss = client ? WiFi.SSID() : esp32_ap_ssid;
    String j = "{\"ssid\":\"";
    for (size_t i = 0; i < ss.length(); i++) {
      char c = ss[i];
      if (c == '"' || c == '\\') j += '\\';
      j += c;
    }
    j += "\",\"ip\":\"";
    j += client ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
    j += "\",\"mode\":";
    j += String((int)currentMode);
    j += "}";
    request->send(200, "application/json", j);
  });
  // Auto Min% learning ("knee tracker") state: knobs + live status + per-bin learned floors.
  server.on("/kneeLearnState", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (demoServe(request, "/kneeLearnState")) return;   // demo mode: made-up history (9_demo.ino)
    request->send(200, "application/json", kneeLearnStateJson());
  });
  // Deferred commissioning-start persist status: PENDING while the loop worker retires the Start's
  // staged NVS writes, FAILED if the restore-point write was refused, IDLE otherwise. The wizard
  // polls this after commissionStart and advances only on IDLE — HTTP 200 means accepted, not saved.
  server.on("/cxStartState", HTTP_GET, [](AsyncWebServerRequest *request) {
    const char *st = (cxStartPersistStep != 0) ? "PENDING" : (cxStartPersistFail ? "FAILED" : "IDLE");
    request->send(200, "application/json", String("{\"state\":\"") + st + "\"}");
  });
  // Charge Rate Limits screen: PENDING while the loop is still writing the staged cap-table blobs, FAILED
  // if a write was refused, IDLE otherwise. rpmCapContinue polls this after /get?capBothSave and sends the
  // HiLow switch only on IDLE — the save's HTTP 200 means accepted, not stored.
  server.on("/capSaveState", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json",
                  (capBothPersistStep != 0) ? "{\"state\":\"PENDING\"}" : (capBothPersistFail ? "{\"state\":\"FAILED\"}" : "{\"state\":\"IDLE\"}"));
  });
  // First-boot SoC seed record for the commissioning popup; ack=1 once the user pressed Finish.
  server.on("/socseed", HTTP_GET, [](AsyncWebServerRequest *request) {
    String snap = settingExists(NK_SocSeedSnap) ? settingRead(NK_SocSeedSnap) : String();
    String ack = settingExists(NK_SocSeedAck) ? settingRead(NK_SocSeedAck) : String();
    if (snap.length() == 0) snap = "null";
    if (ack.length() == 0) ack = "1";
    request->send(200, "application/json", String("{\"snap\":") + snap + ",\"ack\":" + ack + "}");
  });
  // Battery Health: DCIR test status + result table + capacity-vs-cycles trend.
  server.on("/batteryHealth", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (demoServe(request, "/batteryHealth")) return;   // demo mode: made-up history (9_demo.ino)
    size_t n = 0;
    char *body = bhBuildStatusJson(&n);   // PSRAM, owned by the streamer below
    if (!body) {
      request->send(503, "application/json", "{\"err\":\"out of memory\"}");
      return;
    }
    sendPsramText(request, "application/json", body, n);
  });
  // Zero-drift characterization log: small status JSON for the dashboard panel (enable, fill, span).
  server.on("/zerologstate", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (demoServe(request, "/zerologstate")) return;   // demo mode: made-up history (9_demo.ino)
    uint32_t oldest = 0, newest = 0;
    if (zeroLogRing && zeroLogCount > 0) {
      uint16_t oi = (zeroLogCount < ZEROLOG_RING_SIZE) ? 0 : zeroLogHead;
      uint16_t ni = (uint16_t)((zeroLogHead + ZEROLOG_RING_SIZE - 1) % ZEROLOG_RING_SIZE);
      oldest = zeroLogRing[oi].epoch;
      newest = zeroLogRing[ni].epoch;
    }
    String j = "{\"enable\":";  j += (ZeroLogEnable ? 1 : 0);
    j += ",\"count\":";  j += String((unsigned)zeroLogCount);
    j += ",\"cap\":";    j += String((unsigned)ZEROLOG_RING_SIZE);
    j += ",\"oldest\":"; j += String(oldest);
    j += ",\"newest\":"; j += String(newest);
    j += "}";
    request->send(200, "application/json", j);
  });
  // Temp-comp zero correction: live learned-equation state for the dashboard panel.
  server.on("/zerofitstate", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (demoServe(request, "/zerofitstate")) return;   // demo mode: made-up history (9_demo.ino)
    String j = "{\"valid\":";   j += zfValid;
    j += ",\"sensor\":\"";      j += (zfSensor == ZF_ALT ? "alt" : "board"); j += "\"";
    j += ",\"c\":";             j += String(zfC, 4);
    j += ",\"b\":";             j += String(zfB, 5);
    j += ",\"r2\":";            j += String(zfR2, 3);
    j += ",\"corrNow\":";       j += String(DynamicAltCurrentZero, 3);
    j += ",\"applied\":";       j += (AutoAltCurrentZero ? 1 : 0);
    j += ",\"epoch\":";         j += String((unsigned)zfLastEpoch);
    j += ",\"histCount\":";     j += String((unsigned)zeroFitHistCount);
    // Commissioned zero capture (field at 0%, engine running or stopped) — held until the operator applies it.
    j += ",\"capHeld\":";       j += (altZeroCapHeld ? 1 : 0);
    j += ",\"capEpoch\":";      j += String((unsigned)altZeroCapEpoch);
    j += ",\"capA\":";          j += String(altZeroCapA, 3);
    j += ",\"capRpm\":";        j += String((int)altZeroCapRpm);
    j += ",\"capEngOff\":";     j += (altZeroCapEngineOff ? 1 : 0);
    j += ",\"capN\":";          j += String((unsigned)altZeroCapN);
    j += ",\"capP2P\":";        j += String(altZeroCapP2P, 3);
    j += ",\"capAltF\":";       j += (isnan(altZeroCapTempF)  ? String("null") : String(altZeroCapTempF, 1));
    j += ",\"capBoardF\":";     j += (isnan(altZeroCapBoardF) ? String("null") : String(altZeroCapBoardF, 1));
    j += ",\"capRej\":";        j += String((unsigned)altZeroRejReason);
    j += ",\"capBusy\":";       j += (AltZeroCaptureNow ? 1 : 0);
    j += ",\"capPhase\":";      j += String((unsigned)altZeroCapPhase);
    j += ",\"capApplied\":";    j += (altZeroApplied ? 1 : 0);
    j += ",\"capAppliedA\":";   j += String(altZeroAppliedA, 3);
    j += ",\"capAppliedEp\":";  j += String((unsigned)altZeroAppliedEpoch);
    j += ",\"capOffset\":";     j += String(AlternatorCOffset, 3);
    j += "}";
    request->send(200, "application/json", j);
  });
  // Multi-drop 1-Wire probe registry for Setup > Temperature (BATTERY_TEMP_SENSORS_SPEC.md §8). Copied under owMux and
  // built from the stack copy, so TempTask never waits on the network. "role" is from stored role addresses (present:0 kept).
  server.on("/tempSensors", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (demoServe(request, "/tempSensors")) return;   // demo mode: made-up history (9_demo.ino)
    OwProbe snap[OW_MAX_PROBES];
    DeviceAddress roleAddr[TR_COUNT];
    int8_t roleSlot[TR_COUNT];
    uint8_t n;
    portENTER_CRITICAL(&owMux);
    n = owProbeCount;
    memcpy(snap, owProbes, sizeof(OwProbe) * n);
    memcpy(roleAddr, tempRoleAddr, sizeof(roleAddr));
    memcpy(roleSlot, tempRoleSlot, sizeof(roleSlot));
    portEXIT_CRITICAL(&owMux);
    const uint32_t nowMs = millis();
    unsigned unassigned = 0;
    for (uint8_t i = 0; i < n; i++) {
      if (!snap[i].present) continue;
      bool bound = false;
      for (int r = 0; r < TR_COUNT; r++) if (roleSlot[r] == (int8_t)i) { bound = true; break; }
      if (!bound) unassigned++;
    }
    char out[1200];  // 6 probes x ~100 chars + roles block; the overflow guard below returns 500 rather than truncated JSON
    int pos = snprintf(out, sizeof(out), "{\"count\":%u,\"unassigned\":%u,\"probes\":[", (unsigned)n, unassigned);
    for (uint8_t i = 0; i < n && pos < (int)sizeof(out); i++) {
      char hex[17];
      owAddrToHex(snap[i].addr, hex);
      int role = -1;
      for (int r = 0; r < TR_COUNT; r++) if (memcmp(roleAddr[r], snap[i].addr, sizeof(DeviceAddress)) == 0) { role = r; break; }
      char tempStr[16], ageStr[16];
      if (snap[i].lastGoodMs == 0) {
        strcpy(tempStr, "null");
        strcpy(ageStr, "null");
      } else {
        snprintf(tempStr, sizeof(tempStr), "%.1f", snap[i].lastF);
        snprintf(ageStr, sizeof(ageStr), "%lu", (unsigned long)((nowMs - snap[i].lastGoodMs) / 1000UL));
      }
      pos += snprintf(out + pos, sizeof(out) - pos,
                      "%s{\"id\":\"%s\",\"role\":%d,\"tempF\":%s,\"ageS\":%s,\"ok\":%u,\"fail\":%u,\"present\":%d}",
                      i ? "," : "", hex, role, tempStr, ageStr, (unsigned)snap[i].okCount, (unsigned)snap[i].failCount, snap[i].present ? 1 : 0);
    }
    char roleHex[TR_COUNT][17];
    for (int r = 0; r < TR_COUNT; r++) {
      if (owAddrIsZero(roleAddr[r])) roleHex[r][0] = '\0';
      else owAddrToHex(roleAddr[r], roleHex[r]);
    }
    if (pos < (int)sizeof(out)) {
      pos += snprintf(out + pos, sizeof(out) - pos,
                      "],\"roles\":{\"alt\":\"%s\",\"batt\":\"%s\",\"extra\":\"%s\"},\"enabled\":{\"batt\":%d,\"extra\":%d},\"scanning\":%d}",
                      roleHex[TR_ALT], roleHex[TR_BATT], roleHex[TR_EXTRA],
                      battTempProbeEnable ? 1 : 0, extraTempProbeEnable ? 1 : 0, owScanRequested ? 1 : 0);
    }
    if (pos >= (int)sizeof(out)) {
      request->send(500, "text/plain", "tempSensors: response buffer overflow");
      return;
    }
    request->send(200, "application/json", out);
  });
  // Temp-comp zero correction: daily-fit history (≤90 rows) for the trend view. Small enough to build inline.
  server.on("/zerofit.csv", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (demoServe(request, "/zerofit.csv")) return;   // demo mode: made-up history (9_demo.ino)
    String out = "epoch,sensor,c,b,r2,n\n";
    if (zeroFitHist && zeroFitHistCount > 0) {
      for (uint16_t i = 0; i < zeroFitHistCount; i++) {
        uint16_t idx = (zeroFitHistCount < ZFIT_HIST_SIZE) ? i
                       : (uint16_t)((zeroFitHistHead + i) % ZFIT_HIST_SIZE);
        ZeroFitRecord &r = zeroFitHist[idx];
        char line[80];
        snprintf(line, sizeof(line), "%u,%s,%.4f,%.5f,%.3f,%u\n",
                 (unsigned)r.epoch, (r.sensor == ZF_ALT ? "alt" : "board"),
                 r.c, r.b, r.r2, (unsigned)r.n);
        out += line;
      }
    }
    request->send(200, "text/csv", out);
  });
  // Zero-drift log → CSV, streamed oldest-first (constant RAM). Reads the live PSRAM ring, so the
  // download is always complete regardless of the last flash flush.
  server.on("/zerolog.csv", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (demoServe(request, "/zerolog.csv")) return;   // demo mode: made-up history (9_demo.ino)
    if (!zeroLogRing || zeroLogCount == 0) {
      request->send(200, "text/csv", "epoch,rpm,battV,altTempF,boardTempF,amps,p2pAmps\n");
      return;
    }
    struct ZExp { uint16_t head, count, idx; bool header, done; char line[96]; int len, pos; };
    ZExp st;
    st.head = zeroLogHead; st.count = zeroLogCount; st.idx = 0;
    st.header = true; st.done = false; st.len = 0; st.pos = 0;
    AsyncWebServerResponse *response = request->beginChunkedResponse("text/csv",
      [st](uint8_t *buf, size_t maxLen, size_t) mutable -> size_t {
        if (st.done) return 0;
        size_t written = 0;
        while (written < maxLen) {
          if (st.pos >= st.len) {
            if (st.header) {
              st.len = snprintf(st.line, sizeof(st.line), "epoch,rpm,battV,altTempF,boardTempF,amps,p2pAmps\n");
              st.header = false;
            } else {
              if (st.idx >= st.count) { st.done = true; return written; }
              uint16_t ai = (st.count < ZEROLOG_RING_SIZE) ? st.idx
                            : (uint16_t)((st.head + st.idx) % ZEROLOG_RING_SIZE);
              ZeroLogRecord &r = zeroLogRing[ai];
              char altF[12];   // empty cell when the alt-temp sensor was absent (blank sentinel)
              if (r.altTempFx10 == ZEROLOG_TEMP_BLANK) altF[0] = '\0';
              else snprintf(altF, sizeof(altF), "%.1f", r.altTempFx10 / 10.0f);
              char boardF[12]; // empty cell when the board-temp sensor was absent (blank sentinel)
              if (r.boardTempFx10 == ZEROLOG_TEMP_BLANK) boardF[0] = '\0';
              else snprintf(boardF, sizeof(boardF), "%.1f", r.boardTempFx10 / 10.0f);
              st.len = snprintf(st.line, sizeof(st.line), "%u,%d,%.2f,%s,%s,%.3f,%.3f\n",
                                (unsigned)r.epoch, (int)r.rpm, r.battVx100 / 100.0f,
                                altF, boardF, r.amps, r.p2pAmps);
              st.idx++;
            }
            st.pos = 0;
          }
          size_t tw = min((size_t)(st.len - st.pos), maxLen - written);
          memcpy(buf + written, st.line + st.pos, tw);
          written += tw; st.pos += (int)tw;
        }
        return written;
      });
    response->addHeader("Cache-Control", "no-cache");
    request->send(response);
  });
  // ===== TEMP-FIELDCHECK BEGIN: TEMPORARY field-current validation log (see the Xregulator.ino block). REMOVE after the drive. =====
  // /fieldcheck?amps=<A> records a meter reading, ?mark=1 a mark; both answer {ok, seq} and the control tick records the entry
  // on its next pass (fcDoneSeq catches up). No arguments: the panel's status.
  // server.on("/fieldcheck", HTTP_GET, [](AsyncWebServerRequest *request) {
  //   if (!fc) { request->send(200, "application/json", "{\"ok\":0,\"err\":\"no log buffer\"}"); return; }
  //   char buf[512];
  //   const bool isAmps = request->hasParam("amps"), isMark = request->hasParam("mark");
  //   if (isAmps || isMark) {
  //     float a = NAN;
  //     if (isAmps) {
  //       const String v = request->getParam("amps")->value();
  //       char *end = nullptr;
  //       a = strtof(v.c_str(), &end);
  //       if (end == v.c_str() || !(a >= 0.0f && a <= 30.0f)) {
  //         request->send(400, "application/json", "{\"ok\":0,\"err\":\"amps must be a number from 0 to 30\"}");
  //         return;
  //       }
  //     }
  //     uint32_t seq;
  //     portENTER_CRITICAL(&fcMux);
  //     fcReqKind = isAmps ? FC_KIND_METER : FC_KIND_MARK;
  //     fcReqAmps = a;
  //     seq = ++fcReqSeq;
  //     portEXIT_CRITICAL(&fcMux);
  //     snprintf(buf, sizeof(buf), "{\"ok\":1,\"seq\":%lu}", (unsigned long)seq);
  //     request->send(200, "application/json", buf);
  //     return;
  //   }
  //   FcRec last;
  //   uint16_t n, ent;
  //   uint32_t done, oldMs = 0, newMs = 0;
  //   portENTER_CRITICAL(&fcMux);
  //   last = fc->last;
  //   n = fcCount;
  //   ent = fcEntries;
  //   done = fcDoneSeq;
  //   if (n) {
  //     oldMs = fc->ring[(fcHead + FC_RING_N - n) % FC_RING_N].ms;
  //     newMs = fc->ring[(fcHead + FC_RING_N - 1) % FC_RING_N].ms;
  //   }
  //   portEXIT_CRITICAL(&fcMux);
  //   auto num = [](char *p, size_t cap, const char *key, float v, int prec) -> int {
  //     return isfinite(v) ? snprintf(p, cap, ",\"%s\":%.*f", key, prec, v) : snprintf(p, cap, ",\"%s\":null", key);
  //   };
  //   int pos = snprintf(buf, sizeof(buf), "{\"ok\":1,\"n\":%u,\"entries\":%u,\"spanS\":%lu,\"seq\":%lu,\"last\":",
  //                      (unsigned)n, (unsigned)ent, (unsigned long)((newMs - oldMs) / 1000UL), (unsigned long)done);
  //   if (!ent) {
  //     pos += snprintf(buf + pos, sizeof(buf) - pos, "null}");
  //   } else {
  //     pos += snprintf(buf + pos, sizeof(buf) - pos, "{\"kind\":%u,\"ageS\":%lu", (unsigned)last.kind,
  //                     (unsigned long)((uint32_t)(millis() - last.ms) / 1000UL));
  //     pos += num(buf + pos, sizeof(buf) - pos, "meter", last.meterA, 2);
  //     pos += num(buf + pos, sizeof(buf) - pos, "est", last.estAvg, 2);
  //     pos += num(buf + pos, sizeof(buf) - pos, "estMin", last.estMin, 2);
  //     pos += num(buf + pos, sizeof(buf) - pos, "estMax", last.estMax, 2);
  //     pos += num(buf + pos, sizeof(buf) - pos, "dutyMin", last.dutyMin, 1);
  //     pos += num(buf + pos, sizeof(buf) - pos, "dutyMax", last.dutyMax, 1);
  //     pos += num(buf + pos, sizeof(buf) - pos, "vMin", last.vMin, 2);
  //     pos += num(buf + pos, sizeof(buf) - pos, "vMax", last.vMax, 2);
  //     pos += snprintf(buf + pos, sizeof(buf) - pos, "}}");
  //   }
  //   request->send(200, "application/json", buf);
  // });
  // // Oldest first. The first line is "#now_ms,<millis>" so the page can put a clock time on every row; it strips that line.
  // server.on("/fieldcheck.csv", HTTP_GET, [](AsyncWebServerRequest *request) {
  //   struct FcExp { uint16_t head, count, idx; uint8_t stage; bool done; char line[480]; int len, pos; };
  //   FcExp st = {};
  //   if (fc) {
  //     portENTER_CRITICAL(&fcMux);
  //     st.head = fcHead;
  //     st.count = fcCount;
  //     portEXIT_CRITICAL(&fcMux);
  //   }
  //   AsyncWebServerResponse *response = request->beginChunkedResponse("text/csv",
  //     [st](uint8_t *buf, size_t maxLen, size_t) mutable -> size_t {
  //       if (st.done) return 0;
  //       size_t written = 0;
  //       while (written < maxLen) {
  //         if (st.pos >= st.len) {
  //           if (st.stage == 0) {
  //             st.len = snprintf(st.line, sizeof(st.line), "#now_ms,%lu\n", (unsigned long)millis());
  //             st.stage = 1;
  //           } else if (st.stage == 1) {
  //             st.len = snprintf(st.line, sizeof(st.line),
  //                               "kind,ms,meterA,meter_minus_estA,estA_avg,estA_min,estA_max,duty_avg,duty_min,duty_max,"
  //                               "busV_avg,busV_min,busV_max,Rc_ohm,R_ohm,Tref_F,altTemp_F,boardTemp_F,rpm,altA,battA,"
  //                               "ceil10A_pct,ceilHeat_pct,ceilBind,limitCode,mode,stage,fieldOn,fieldResTest\n");
  //             st.stage = 2;
  //           } else {
  //             if (st.idx >= st.count) { st.done = true; return written; }
  //             FcRec r;
  //             portENTER_CRITICAL(&fcMux);
  //             r = fc->ring[(st.head + FC_RING_N - st.count + st.idx) % FC_RING_N];
  //             portEXIT_CRITICAL(&fcMux);
  //             st.idx++;
  //             // Blank cell for a value that was never read (NAN).
  //             char *p = st.line;
  //             size_t cap = sizeof(st.line);
  //             auto cell = [&p, &cap](float v, int prec) {
  //               int w = isfinite(v) ? snprintf(p, cap, ",%.*f", prec, v) : snprintf(p, cap, ",");
  //               if (w > 0 && (size_t)w < cap) { p += w; cap -= w; }
  //             };
  //             static const char *const kinds[] = { "row", "meter", "mark" };
  //             static const char *const modes[] = { "off", "manual", "auto", "fault" };
  //             int w = snprintf(p, cap, "%s,%lu", r.kind < 3 ? kinds[r.kind] : "?", (unsigned long)r.ms);
  //             p += w; cap -= w;
  //             cell(r.meterA, 2);
  //             cell(r.meterA - r.estAvg, 2);
  //             cell(r.estAvg, 3); cell(r.estMin, 3); cell(r.estMax, 3);
  //             cell(r.dutyAvg, 2); cell(r.dutyMin, 2); cell(r.dutyMax, 2);
  //             cell(r.vAvg, 3); cell(r.vMin, 3); cell(r.vMax, 3);
  //             cell(r.rc, 4); cell(r.r, 4); cell(r.trefC * 1.8f + 32.0f, 1);
  //             cell(r.probeF, 1); cell(r.dieF, 1);
  //             cell(r.rpm, 0); cell(r.altA, 1); cell(r.battA, 1);
  //             cell(r.curCeil, 2); cell(r.heatCeil, 2);
  //             w = snprintf(p, cap, ",%u,%u,%s,%u,%u,%u\n", (unsigned)r.bind, (unsigned)r.lim,
  //                          r.mode < 4 ? modes[r.mode] : "?", (unsigned)r.stage, (unsigned)r.fieldOn, (unsigned)r.frtRun);
  //             p += w;
  //             st.len = (int)(p - st.line);
  //           }
  //           st.pos = 0;
  //         }
  //         size_t tw = min((size_t)(st.len - st.pos), maxLen - written);
  //         memcpy(buf + written, st.line + st.pos, tw);
  //         written += tw; st.pos += (int)tw;
  //       }
  //       return written;
  //     });
  //   response->addHeader("Cache-Control", "no-cache");
  //   request->send(response);
  // });
  // ===== TEMP-FIELDCHECK END =====
  // The held best-ever front (BEFRONT1 CSV artifact) — streamed (constant RAM at any front size).
  server.on("/altcurve.csv", HTTP_GET, [](AsyncWebServerRequest *request) {
    altCurveCsvSend(request);
  });
  // Front support points as a plain scatter table — streamed.
  server.on("/altrecords.csv", HTTP_GET, [](AsyncWebServerRequest *request) {
    altFrontRecordsCsvSend(request);
  });
  // Full alt-health state dump for offline / AI debugging (Download Debug CSV) — streamed.
  server.on("/altdebug.csv", HTTP_GET, [](AsyncWebServerRequest *request) {
    altDebugCsvSend(request);
  });
  // Session-% series so a page opened mid-session (or reconnecting after a WiFi gap) recovers the
  // whole run — the live stream never backfills. RAM-only ring, streamed.
  server.on("/altsess.csv", HTTP_GET, [](AsyncWebServerRequest *request) {
    altSessCsvSend(request);
  });
  // Solar ledger: predicted vs actual harvest and consumption per local day, plus the day in progress.
  server.on("/solarledger.csv", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (demoServe(request, "/solarledger.csv")) return;   // demo mode: made-up history (9_demo.ino)
    solarLedgerCsvSend(request);
  });
  // Gate-tuning capture dump: 164 B header + 17 B rows, decoded in the browser (parseAltLogBin in script.js). Refuses
  // while recording; clearing is a separate press (/get?altLogClear=1) so the file is in hand before the buffer is freed.
  server.on("/altlog.bin", HTTP_GET, [](AsyncWebServerRequest *request) {
    altLogBinSend(request);
  });
  // Performance-vs-engine-hours trend, decimated to <= TR_MAXOUT points (full hourly history stays on the device): each
  // point is one bucket of source hours, low line = min of their P10s (keeps the early-warning envelope), overall = mean.
  extern float altTrendBucketSec;   // defined in 7_functions.ino (concatenated later) — needed to convert bucket index → engine-hours
  server.on("/alttrend.csv", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!altTrend) { request->send(200, "text/plain", "engHours,worstPct,overallPct\n"); return; }
    const int TR_MAXOUT = 200;
    // Optional ?hours=N: decimate WITHIN the last N engine-hours, or a global 200-pt stride over a long history leaves
    // ~0 points in a 10 h slice. hours<=0 or absent = full history.
    int base = 0;
    if (request->hasParam("hours")) {
      int winHours = request->getParam("hours")->value().toInt();
      if (winHours > 0 && altTrendCount > 0) {
        float hrPerBucket = altTrendBucketSec / 3600.0f;
        float cutoff = altTrend[altTrendCount - 1].engHour * hrPerBucket - (float)winHours;
        for (int i = 0; i < altTrendCount; i++) {
          if (altTrend[i].engHour * hrPerBucket >= cutoff) { base = i; break; }
        }
      }
    }
    struct TrExp { int base, total, stride, outIdx, numOut; bool header, done; char line[64]; int len, pos; };
    TrExp st;
    st.base = base;
    st.total = altTrendCount - base;
    st.stride = (st.total > TR_MAXOUT) ? ((st.total + TR_MAXOUT - 1) / TR_MAXOUT) : 1;
    st.numOut = (st.stride > 0) ? ((st.total + st.stride - 1) / st.stride) : 0;
    st.outIdx = 0; st.header = true; st.done = false; st.len = 0; st.pos = 0;
    AsyncWebServerResponse *response = request->beginChunkedResponse("text/csv",
      [st](uint8_t *buf, size_t maxLen, size_t) mutable -> size_t {
        if (st.done) return 0;
        size_t written = 0;
        while (written < maxLen) {
          if (st.pos >= st.len) {
            if (st.header) {
              st.len = snprintf(st.line, sizeof(st.line), "engHours,worstPct,overallPct\n");
              st.header = false;
            } else {
              if (st.outIdx >= st.numOut) { st.done = true; return written; }
              int start = st.base + st.outIdx * st.stride, end = start + st.stride;
              if (end > st.base + st.total) end = st.base + st.total;
              float worst = 1e9f, sum = 0; int n = 0;
              for (int i = start; i < end; i++) {
                float w = altTrend[i].worstPct / 10.0f;
                if (w < worst) worst = w;
                sum += altTrend[i].overallPct / 10.0f; n++;
              }
              // Stored engHour is a BUCKET INDEX; convert to engine-hours (index × bucketSec/3600).
              float ehHours = (float)altTrend[end - 1].engHour * (altTrendBucketSec / 3600.0f);
              st.len = snprintf(st.line, sizeof(st.line), "%.3f,%.1f,%.1f\n", ehHours, worst, n ? sum / n : 0.0f);
              st.outIdx++;
            }
            st.pos = 0;
          }
          size_t tw = min((size_t)(st.len - st.pos), maxLen - written);
          memcpy(buf + written, st.line + st.pos, tw);
          written += tw; st.pos += (int)tw;
        }
        return written;
      });
    response->addHeader("Cache-Control", "no-cache");
    request->send(response);
  });

  // Boat-performance telemetry schema — the dashboard fetches this once and zips the names
  // against the PerfLive/PerfSettings SSE values, so no field array is hand-kept in JS.
  server.on("/perfschema", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", perfSchemaJson());
  });

  // The held best-ever fronts (BEFRONT1 CSV pair: SAIL + MOTOR blocks) — dashboard polar/curve source. Streamed.
  server.on("/perfcurve.csv", HTTP_GET, [](AsyncWebServerRequest *request) {
    perfCurveCsvSend(request);
  });

  // Front support points as a plain scatter table (sail + motor, mode-tagged) — streamed.
  server.on("/perfrecords.csv", HTTP_GET, [](AsyncWebServerRequest *request) {
    perfRecordsCsvSend(request);
  });

  // Load CSV (import a polar): POST the BEFRONT1 sail+motor pair as the raw body; frontUploadBody buffers it on the
  // request, then perfUploadFrontCsv() replaces both fronts (?fixed=1 freeze / 0 learn). Same ingest as cloud-sync.
  server.on("/perfUploadFront", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      if (refuseMisaddressed(request)) return;
      if (demoSeal) { demoRefuse(request); return; }
      if (!settingsArmActive(request)) { request->send(403, "text/plain", "Settings not armed"); return; }
      char *body = frontUploadText(request);
      if (!body) return;
      // fixed=1 → FIXED+paused (freeze), fixed=0 → LEARNED+resumed (learn). Default freeze if absent.
      bool fixed = (!request->hasParam("fixed")) || (request->getParam("fixed")->value().toInt() != 0);
      bool ok = perfUploadFrontCsv(body, fixed);
      request->send(ok ? 200 : 400, "text/plain", ok ? "OK" : "Parse failed");
    },
    NULL, frontUploadBody);

  // Load CSV (alternator health): mirrors /perfUploadFront. Loads into the Uploaded surface (My History kept); ?fixed=1
  // pauses My History learning, 0 keeps it learning.
  server.on("/altUploadFront", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      if (refuseMisaddressed(request)) return;
      if (demoSeal) { demoRefuse(request); return; }
      if (!settingsArmActive(request)) { request->send(403, "text/plain", "Settings not armed"); return; }
      char *body = frontUploadText(request);
      if (!body) return;
      // fixed=1 → Pause ON (grade against the upload only), fixed=0 → My History keeps learning. Default Pause ON if absent.
      bool fixed = (!request->hasParam("fixed")) || (request->getParam("fixed")->value().toInt() != 0);
      bool ok = altUploadFrontCsv(body, fixed);
      request->send(ok ? 200 : 400, "text/plain", ok ? "OK" : "Parse failed");
    },
    NULL, frontUploadBody);

  // Config Sharing — export the cloneable settings set as one JSON blob (for download
  // or cloud submission). Arm-gated like the upload endpoints.
  server.on("/exportConfig", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!settingsArmActive(request)) {
      request->send(403, "text/plain", "Settings not armed"); return;
    }
    // demo mode: no config file (download=1 is the Export button); the app's own reads of the current settings still answer
    if (demoSeal && paramOk(request, "download") && request->getParam("download")->value().toInt() != 0) { demoRefuse(request); return; }
    request->send(200, "application/json", exportConfigJson());
  });

  // App-usage analytics delta (page dwell, button counts). NOT arm-gated: counter increments only, AP and Client mode;
  // sendBeacon posts land here too (content type ignored). Body rides request->_tempObject (freed by the request
  // destructor) so two concurrent posters can't interleave.
  server.on("/track", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      char *body = (char *)request->_tempObject;
      if (!body || strlen(body) < 2) { request->send(400, "text/plain", "empty"); return; }
      bool ok = usageMergeDelta(body);
      request->send(ok ? 200 : 400, "text/plain", ok ? "ok" : "bad");
    },
    NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      if (total > 4096) return;   // a real delta is a few hundred bytes; reject anything absurd
      if (index == 0 && !request->_tempObject) {
        request->_tempObject = ps_malloc(total + 1);
        if (request->_tempObject) ((char *)request->_tempObject)[0] = '\0';  // guard-rejected chunks must not leave strlen() garbage
      }
      char *body = (char *)request->_tempObject;
      if (!body || index + len > total) return;
      memcpy(body + index, data, len);
      body[index + len] = '\0';
    });

  // App Usage card (Live Data → ESP32): period + lifetime stats. Read-only, so not arm-gated.
  server.on("/trackstats", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", usageStatsJson());
  });

  // Equipment records (8_functions.ino): the boat's history files, their NVS keys and the lifetime namespace, saved
  // in the same file as the config by the dashboard. ?summary=1 = sizes and content hashes only, for the preview.
  // Streamed from PSRAM — hundreds of KB of base64 must never become an internal-heap String.
  server.on("/exportRecords", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!settingsArmActive(request)) {
      request->send(403, "text/plain", "Settings not armed"); return;
    }
    if (demoSeal) { demoRefuse(request); return; }   // demo mode: the stores in RAM are the demo's
    size_t n = 0;
    void *rec = recordsExportJson(paramOk(request, "summary") && request->getParam("summary")->value().toInt() != 0, &n);
    if (!rec) { request->send(503, "text/plain", "records unavailable (out of memory, or a record file could not be read)"); return; }
    std::shared_ptr<void> sp(rec, recordsExportFree);
    AsyncWebServerResponse *response = request->beginResponse("application/json", n,
      [sp](uint8_t *out, size_t maxLen, size_t index) -> size_t {
        return recordsExportRead(sp.get(), out, maxLen, index);
      });
    response->addHeader("Cache-Control", "no-cache");
    request->send(response);
  });

  // Apply the records groups the preview picked (body {"records":{...}}). Files are staged for the next boot, so
  // the regulator always reboots after this: soon when nothing else follows, or within 30 s when the dashboard is
  // about to post the config import too (?then=config), whose own reboot normally comes first.
  server.on("/importRecords", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      if (refuseMisaddressed(request)) return;
      if (demoSeal) { demoRefuse(request); return; }
      if (!settingsArmActive(request)) { request->send(403, "text/plain", "Settings not armed"); return; }
      const char *body = (const char *)request->_tempObject;
      if (!body || strlen(body) < 2) { request->send(400, "text/plain", "Empty body"); return; }
      int n = recordsImport(body);
      if (n == -2) { request->send(500, "text/plain", "The records could not be staged (out of memory or flash space); nothing was changed"); return; }
      if (n < 0) { request->send(400, "text/plain", "No records object in body"); return; }
      const bool thenConfig = request->hasParam("then") && request->getParam("then")->value() == "config";
      if (thenConfig) importRebootDeadlineMs = millis() + 30000UL;
      else { rebootRequestWhy = REBOOT_WHY_IMPORT; rebootRequestedAt = millis(); rebootRequested = true; }
      String resp = "{\"applied\":";
      resp += n; resp += ",\"reboot\":1}";
      request->send(200, "application/json", resp);
    },
    NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      if (total > 4194304) return;   // bound: a full set of records is well under 1 MB of base64
      if (index == 0 && !request->_tempObject) {
        request->_tempObject = ps_malloc(total + 1);
        if (request->_tempObject) ((char *)request->_tempObject)[0] = '\0';
      }
      char *body = (char *)request->_tempObject;
      if (!body || index + len > total) return;
      memcpy(body + index, data, len);
      body[index + len] = '\0';
    });

  // Config Sharing import (POST the /exportConfig JSON). Only allowlisted manifest keys are written. Reboots afterwards
  // so InitSystemSettings re-reads the set consistently (?noReboot=1 suppresses).
  server.on("/importConfig", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      if (refuseMisaddressed(request)) { importConfigBuf = ""; return; }
      if (demoSeal) { importConfigBuf = ""; demoRefuse(request); return; }
      if (!settingsArmActive(request)) {
        importConfigBuf = ""; request->send(403, "text/plain", "Settings not armed"); return;
      }
      if (importConfigBuf.length() < 2) {
        importConfigBuf = ""; request->send(400, "text/plain", "Empty body"); return;
      }
      char *bodyc = strdup(importConfigBuf.c_str());
      importConfigBuf = "";
      if (!bodyc) { request->send(500, "text/plain", "Out of memory"); return; }
      String preClassV = settingRead(NK_BatteryVoltage);
      String preWindV = settingRead(NK_FieldWindingV) + "/" + settingRead(NK_fieldWindApplied);
      // Logged BEFORE the write burst, which is what may stall the loop; the console line is the only trace afterwards
      // for callers the UI's pre-Apply warning doesn't cover.
      bool fieldWasLive = (fieldActiveStatus > 0);
      if (fieldWasLive) queueConsoleMessage("Config import starting with the field driving - each setting saved to NVS can briefly pause the control loop; prefer importing with charging off");
      int n = applyImportConfig(bodyc, false);
      free(bodyc);
      if (n < 0) { request->send(400, "text/plain", "No config object in body"); return; }
      // noReboot (bench only) leaves RAM stale against the NVS just written; harmless for ordinary keys, but a class
      // change mutates RAM while the overwrites land only in NVS, so force the reboot there.
      bool classMoved = (settingRead(NK_BatteryVoltage) != preClassV)
                        || (settingRead(NK_FieldWindingV) + "/" + settingRead(NK_fieldWindApplied) != preWindV);   // a winding move converts the same way
      bool reboot = classMoved || !request->hasParam("noReboot") || request->getParam("noReboot")->value().toInt() == 0;
      if (reboot) { rebootRequestWhy = REBOOT_WHY_IMPORT; rebootRequestedAt = millis(); rebootRequested = true; }
      String resp = "{\"applied\":";
      resp += n; resp += ",\"reboot\":"; resp += (reboot ? 1 : 0); resp += "}";
      request->send(200, "application/json", resp);
    },
    NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      if (total > 65536) return;   // bound: a real config blob is ~10 KB; reject anything absurd
      if (index == 0) { importConfigBuf = ""; importConfigBuf.reserve(total + 1); }
      importConfigBuf.concat((const char *)data, len);
    });

  server.on("/cvlog.bin", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!cvLogReady || !cvLog || cvLogCount == 0) {
      request->send(200, "application/octet-stream", "");
      return;
    }

    CvBinDLState state;
    memset(&state, 0, sizeof(state));

    uint32_t cnt = (uint32_t)cvLogCount;
    uint32_t entrySize = (uint32_t)sizeof(CvLogEntry);
    float kp = (float)VoltageKp_active;  // gains in effect (Manual or Measured α/K, 12V-block normalized), not raw VoltageKp
    float ki = (float)VoltageKi_active;
    uint32_t interval = (uint32_t)VoltageLoopInterval;

    float kdDeadband = CvKdDeadbandVps;
    float kdActiveGain = VoltageKd_active;  // the gain the trim law multiplies (Measured Td·Kp or Manual, ×vNorm×derate), not the typed VoltageKd
    float kdArmV     = CvKdArmV;
    float kdOneSided = (float)(CvKdOneSided ? 1 : 0);
    float kdVoltFiltTC = CvKdVoltFiltTC;
    float kdDbSlope  = CvKdDbSlope;
    float kdDbFloor  = CvKdDbFloor;
    float kdDbCeil   = CvKdDbCeil;
    float kdSlopeCeil = CvKdSlopeCeil;  // real per-bus — matches the ±clamp on the logged cvDSlope
    float kdMaxTrimA = CvKdMaxTrimA;
    float kdExcessMode = (float)(CvKdExcessMode ? 1 : 0);
    float brakeFallRate = CvBrakeFallRate;

    memcpy(state.header + 0,  &cnt,       4);
    memcpy(state.header + 4,  &entrySize, 4);
    memcpy(state.header + 8,  &kp,        4);
    memcpy(state.header + 12, &ki,        4);
    memcpy(state.header + 16, &interval,  4);
    memcpy(state.header + 20, &kdDeadband, 4);  // deadband line BASE (b of clamp(floor, b + m·spLimited, ceil))
    memcpy(state.header + 24, &kdActiveGain, 4);  // VoltageKd_active (A/(V/s))
    memcpy(state.header + 28, &kdArmV,     4);  // CvKdArmV (V)
    memcpy(state.header + 32, &kdOneSided, 4);  // CvKdOneSided (0/1)
    memcpy(state.header + 36, &kdVoltFiltTC, 4);  // CvKdVoltFiltTC (ms) — D-term voltage EMA TC
    memcpy(state.header + 40, &kdDbSlope,  4);  // deadband line slope m (V/s per A)
    memcpy(state.header + 44, &kdDbFloor,  4);  // deadband line clamp floor (V/s)
    memcpy(state.header + 48, &kdDbCeil,   4);  // deadband line clamp ceiling (V/s)
    memcpy(state.header + 52, &kdSlopeCeil, 4);  // slope ceiling (V/s, real per-bus)
    memcpy(state.header + 56, &kdMaxTrimA, 4);  // flat trim cap (A)
    memcpy(state.header + 60, &kdExcessMode, 4);  // 1 = excess-over-line trim, 0 = legacy full-slope latch
    memcpy(state.header + 64, &brakeFallRate, 4);  // CvBrakeFallRate (A/s) at log time

    state.count = cvLogCount;
    state.oldest = (cvLogHead - cvLogCount + CV_LOG_SIZE) % CV_LOG_SIZE;

    cvLogPaused = true;
    cvLogPausedAtMs = millis();
    // Same Safari abrupt-teardown release as /thermallog.csv.
    uint32_t dlTok = ++cvLogDlToken;
    request->onDisconnect([dlTok]() {
      if (dlTok == cvLogDlToken) cvLogPaused = false;
    });

    AsyncWebServerResponse *response = request->beginChunkedResponse(
      "application/octet-stream",
      [state](uint8_t *buf, size_t maxLen, size_t) mutable -> size_t {
        cvLogPausedAtMs = millis();
        if (state.done) return 0;

        size_t written = 0;
        while (written < maxLen) {
          // Phase 1: drain the CV_LOG_HEADER_SIZE-byte header
          if (state.headerPos < CV_LOG_HEADER_SIZE) {
            size_t canSend = min(maxLen - written,
                                 (size_t)(CV_LOG_HEADER_SIZE - state.headerPos));
            memcpy(buf + written, state.header + state.headerPos, canSend);
            written += canSend;
            state.headerPos += (int)canSend;
            continue;
          }
          // Phase 2: stream entries
          if (state.entryPos >= state.entryLen) {
            if (state.row >= state.count) {
              cvLogPaused = false;
              state.done = true;
              return written;
            }
            int idx = (state.oldest + state.row) % CV_LOG_SIZE;
            memcpy(state.entryBuf, &cvLog[idx], sizeof(CvLogEntry));
            state.entryLen = (int)sizeof(CvLogEntry);
            state.entryPos = 0;
            state.row++;
          }
          size_t canSend = min(maxLen - written,
                               (size_t)(state.entryLen - state.entryPos));
          memcpy(buf + written, state.entryBuf + state.entryPos, canSend);
          written += canSend;
          state.entryPos += (int)canSend;
        }
        return written;
      });

    response->addHeader("Cache-Control", "no-cache");
    request->send(response);
  });


  // Barometric pressure 14-day history: 8-byte LE header + 4032 LE uint16 samples (mbar × 10; 0 = no sample).
  //   [0..1] u16 headIdx = next write slot  [2..5] u32 epoch of newest sample (0 = never synced)  [6..7] u16 count
  server.on("/baroHistory.bin", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!baroPressureHistory) {
      request->send(503, "application/octet-stream", "");
      return;
    }
    struct BaroDLState {
      uint8_t  header[8];
      int      headerPos;
      uint32_t bytePos;  // bytes, not samples: maxLen can be odd, and a sample cursor re-sent the split byte
      bool     done;
    };
    BaroDLState state;
    memset(&state, 0, sizeof(state));
    uint16_t headLE  = baroHistoryHead;
    uint32_t epochLE = (uint32_t)baroHistoryLastEpoch;
    uint16_t countLE = BARO_HISTORY_SIZE;
    memcpy(state.header + 0, &headLE,  2);
    memcpy(state.header + 2, &epochLE, 4);
    memcpy(state.header + 6, &countLE, 2);

    AsyncWebServerResponse *response = request->beginChunkedResponse(
      "application/octet-stream",
      [state](uint8_t *buf, size_t maxLen, size_t) mutable -> size_t {
        if (state.done) return 0;
        size_t written = 0;
        while (written < maxLen) {
          if (state.headerPos < 8) {
            size_t canSend = min(maxLen - written, (size_t)(8 - state.headerPos));
            memcpy(buf + written, state.header + state.headerPos, canSend);
            written += canSend;
            state.headerPos += (int)canSend;
            continue;
          }
          if (state.bytePos >= BARO_HISTORY_SIZE * sizeof(uint16_t)) {
            state.done = true;
            return written;
          }
          size_t bytesLeft = BARO_HISTORY_SIZE * sizeof(uint16_t) - state.bytePos;
          size_t canSend = min(maxLen - written, bytesLeft);
          memcpy(buf + written, ((uint8_t *)baroPressureHistory) + state.bytePos, canSend);
          written += canSend;
          state.bytePos += canSend;
        }
        return written;
      });
    response->addHeader("Cache-Control", "no-cache");
    request->send(response);
  });

  // Long Term Plots history: 16 B LE header + records; record time = lastEpoch − (count−1−i) × intervalSec.
  //   [0..1] u16 head  [2..3] u16 count  [4..5] u16 capacity  [6..7] u16 recordSize
  //   [8..11] u32 lastEpoch  [12..15] u32 intervalSec
  server.on("/longTermPlots.bin", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (demoServe(request, "/longTermPlots.bin")) return;   // demo mode: made-up history (9_demo.ino)
    if (!longTermRing) {
      request->send(503, "application/octet-stream", "");
      return;
    }
    struct LtDLState {
      uint8_t  header[16];
      int      headerPos;
      uint32_t recIdx;      // which chronological record (0..count-1)
      uint32_t recBytePos;  // byte offset within the current record
      bool     done;
    };
    LtDLState state;
    memset(&state, 0, sizeof(state));
    uint16_t headLE = longTermHead, countLE = longTermCount;
    uint16_t capLE = LONGTERM_RING_SIZE, recLE = (uint16_t)sizeof(LongTermRecord);
    uint32_t epochLE = (uint32_t)longTermLastEpoch;
    uint32_t intervalLE = SENSOR_UPLOAD_INTERVAL / 1000UL;
    uint16_t tail = (countLE < capLE) ? 0 : headLE;   // oldest record (chronological start)
    memcpy(state.header + 0,  &headLE,     2);
    memcpy(state.header + 2,  &countLE,    2);
    memcpy(state.header + 4,  &capLE,      2);
    memcpy(state.header + 6,  &recLE,      2);
    memcpy(state.header + 8,  &epochLE,    4);
    memcpy(state.header + 12, &intervalLE, 4);

    // Body = `count` records oldest first, NOT the full ring: the whole 4320-slot ring (~540 KB) made the async server
    // truncate the chunked transfer. JS reads records linearly (base = 16 + i*recSize).
    AsyncWebServerResponse *response = request->beginChunkedResponse(
      "application/octet-stream",
      [state, countLE, tail, capLE, recLE](uint8_t *buf, size_t maxLen, size_t) mutable -> size_t {
        if (state.done) return 0;
        size_t written = 0;
        while (written < maxLen) {
          if (state.headerPos < 16) {
            size_t canSend = min(maxLen - written, (size_t)(16 - state.headerPos));
            memcpy(buf + written, state.header + state.headerPos, canSend);
            written += canSend;
            state.headerPos += (int)canSend;
            continue;
          }
          if (state.recIdx >= countLE) { state.done = true; return written; }
          uint32_t srcIdx = ((uint32_t)tail + state.recIdx) % capLE;   // ring → chronological
          const uint8_t *src = (const uint8_t *)longTermRing + (size_t)srcIdx * recLE;
          size_t canSend = min(maxLen - written, (size_t)(recLE - state.recBytePos));
          memcpy(buf + written, src + state.recBytePos, canSend);
          written += canSend;
          state.recBytePos += (uint32_t)canSend;
          if (state.recBytePos >= recLE) { state.recBytePos = 0; state.recIdx++; }
        }
        return written;
      });
    response->addHeader("Cache-Control", "no-cache");
    request->send(response);
  });

  server.on("/resetlogs", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (refuseMisaddressed(request)) return;
    thermalLogHead = 0;
    thermalLogCount = 0;
    thermalLogPaused = false;
    pidLogHead = 0;
    pidLogCount = 0;
    pidLogPaused = false;
    cvLogHead = 0;
    cvLogCount = 0;
    cvLogPaused = false;
    request->send(200, "text/plain", "OK");
  });

  server.on("/stoplogs", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (refuseMisaddressed(request)) return;
    loggingActive = false;
    request->send(200, "text/plain", "Logging stopped");
  });

  server.on("/startlogs", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (refuseMisaddressed(request)) return;
    loggingActive = true;
    request->send(200, "text/plain", "Logging started");
  });


  // Derived view of the Vessel Info NVS record. Deliberately ungated: the form and the Download Logs bundle need it
  // unarmed. 404 until the form has been saved (the client reads that as "vessel info incomplete").
  server.on("/vessel_info.json", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (settingExists(NK_vesselSaved)) {
      request->send(200, "application/json", vesselInfoJson());
    } else {
      request->send(404, "application/json", "{\"error\":\"Vessel info not found\"}");
    }
  });
  // Re-commission nag state. epoch=0 means no pass has ever finished (or the browser clock was bad),
  // in which case the client must not nag. Age is compared against the BROWSER clock, not this device's.
  server.on("/recommission.json", HTTP_GET, [](AsyncWebServerRequest *request) {
    char buf[80];
    snprintf(buf, sizeof(buf), "{\"epoch\":%lld,\"ageAck\":%d,\"change\":%d}",
             (long long)CommissionEpoch, commissionAgeAck ? 1 : 0, commissionChangeFlag ? 1 : 0);
    request->send(200, "application/json", buf);
  });
  // Deliberately NOT behind the settings arm gate: the age prompt fires on a cold app open, when
  // settings are normally still locked, and a 403 there would silently un-silence it every session.
  // These two flags carry no control authority — the worst a caller can do is mute a maintenance nag.
  server.on("/recommissionAck", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (refuseMisaddressed(request)) return;
    if (request->hasParam("commissionAgeAck")) {
      commissionAgeAck = true;
      settingWrite(NK_cmAgeAck, "1");
    }
    if (request->hasParam("commissionChangeAck")) {
      commissionChangeFlag = false;
      settingWrite(NK_cmChangeFlag, "0");
    }
    request->send(200, "application/json", "{\"ok\":true}");
  });
  // Admin config push detail + ack: the text the popup needs. ?ack=1 clears the applied receipt (pending clears itself).
  // Not arm-gated: it changes no setting and reveals nothing a CSV2 frame doesn't.
  server.on("/configPush", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("ack")) {
      cfgPushAppliedCount = 0;
      cfgPushAppliedKeys = "";
      settingRemove(NK_cfgPushNotify);
    }
    String out = "{\"pendingCount\":" + String((int)cfgPushPendingCount)
               + ",\"pendingNote\":\"" + cfgPushPendingNote
               + "\",\"appliedCount\":" + String((int)cfgPushAppliedCount)
               + ",\"appliedKeys\":\"" + cfgPushAppliedKeys + "\"}";
    request->send(200, "application/json", out);
  });
  server.on("/saveVesselInfo", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (refuseMisaddressed(request)) return;
    if (!settingsArmActive(request)) {
      request->send(401, "application/json", "{\"success\":false,\"error\":\"Settings not armed\"}");
      return;
    }
    if (!request->hasParam("vesselData", true)) {
      request->send(400, "application/json", "{\"success\":false,\"error\":\"No vessel data\"}");
      return;
    }
    String jsonStr = request->getParam("vesselData", true)->value();
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, jsonStr);
    if (error) {
      request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid JSON\"}");
      return;
    }
    BOAT_LENGTH_FT = doc["boat_length_ft"];
    BOAT_DISPLACEMENT_LBS = doc["boat_displacement_lbs"];
    vesselSetText(BOAT_TYPE, sizeof(BOAT_TYPE), doc["boat_type"] | "");
    vesselSetText(BOAT_MAKE_MODEL, sizeof(BOAT_MAKE_MODEL), doc["boat_make_model"] | "");
    BOAT_YEAR = doc["boat_year"];
    if (doc.containsKey("home_port") && !doc["home_port"].isNull()) {
      const char *homePortStr = doc["home_port"];
      strncpy(HOME_PORT, homePortStr, 50);
      HOME_PORT[50] = '\0';
    } else {
      HOME_PORT[0] = '\0';  // Empty string if not provided
    }
    vesselSetText(ENGINE_MAKE, sizeof(ENGINE_MAKE), doc["engine_make"] | "");
    ENGINE_HP = doc["engine_hp"];
    // System voltage is the sole source of truth for the 12/24/36/48V class. On a change, rescale the
    // whole charge-voltage profile + both absolute OV rungs (software cut, INA228 hardware limit) + both
    // control loops' normalized gains (CV and CC). The dashboard warns the user before submitting.
    int oldBatteryVoltage = SYSTEM_VOLTAGE_CLASS;
    int newBatteryVoltage = doc["battery_voltage"] | (int)SYSTEM_VOLTAGE_CLASS;
    if (newBatteryVoltage != 12 && newBatteryVoltage != 24 && newBatteryVoltage != 36 && newBatteryVoltage != 48) newBatteryVoltage = oldBatteryVoltage;
    int    oldCapacityAh = BatteryCapacity_Ah;
    char oldBatteryType[sizeof(BATTERY_TYPE)];
    strncpy(oldBatteryType, BATTERY_TYPE, sizeof(oldBatteryType));
    SYSTEM_VOLTAGE_CLASS = (uint8_t)newBatteryVoltage;
    applyNominalVoltageChange(oldBatteryVoltage, newBatteryVoltage);
    // Field winding after the class, so the new class bounds it. field_winding_ack is the owner's "keep" answer to the
    // resistance check the dashboard asks before saving, as the "W:R" pairing it was asked about; an absent key keeps
    // the present winding.
    fieldWindingSet((int)(doc["field_winding_v"] | (int)FieldWindingV), doc["field_winding_ack"] | (const char *)nullptr);
    // An absent/invalid key yields 0, which zeroes the full-charge tail-current threshold
    // (TailCurrent * BatteryCapacity_Ah) and the displayed capacity. Keep the current value instead.
    BatteryCapacity_Ah = constrain((int)(doc["battery_capacity_ah"] | BatteryCapacity_Ah), 1, 100000);
    PeukertRatedCurrent_A = BatteryCapacity_Ah / 20.0f;  // /get derives this on write; import must too
    vesselSetText(BATTERY_TYPE, sizeof(BATTERY_TYPE), doc["battery_type"] | "");
    // Chemistry does not move VoltageHardwareLimit (a user setting; chemistry values come via the battery-defaults
    // proposal). Voltage/capacity/chemistry all move the CV plant gain the commissioning fit measured, so nag, but only
    // once a pass has finished (epoch stamped).
    if (CommissionEpoch > 0 && (newBatteryVoltage != oldBatteryVoltage
                                || BatteryCapacity_Ah != oldCapacityAh
                                || strcmp(BATTERY_TYPE, oldBatteryType) != 0)) {
      raiseRecommissionNag();
    }
    vesselSetText(BATTERY_MAKE_MODEL, sizeof(BATTERY_MAKE_MODEL), doc["battery_make_model"] | "");
    vesselSetText(ALTERNATOR_BRAND_MODEL, sizeof(ALTERNATOR_BRAND_MODEL), doc["alternator_brand_model"] | "");
    // Per-unit label. It reaches an mDNS TXT record and an unauthenticated JSON response, so the
    // control characters are stripped once here rather than at every read site.
    vesselSetText(REGULATOR_NAME, sizeof(REGULATOR_NAME), doc["regulator_name"] | "");
    for (char *p = REGULATOR_NAME; *p; p++) {
      if ((unsigned char)*p < 0x20 || (unsigned char)*p == 0x7F) *p = ' ';
    }
    SolarWatts = doc["solar_watts"];
    // Unclamped, an out-of-range value indexes past axisRemap[] and wild-reads through src[]
    uint8_t prevOrient = imuMountOrientation;
    imuMountOrientation = (uint8_t)constrain((int)(doc["imu_mount_orientation"] | 0), 0, IMU_ORIENT_COUNT - 1);
    if (imuMountOrientation != prevOrient && imuMountState != IMU_MOUNT_UNKNOWN) {
      imuMountState = IMU_MOUNT_UNKNOWN;   // verdict was judged in the old frame; next Zero re-latches it
      settingRemove(NK_imu_mnt_state);
    }
    IMU_DIST_BOW_FT = doc["imu_dist_bow_ft"];
    IMU_DIST_CL_FT = doc["imu_dist_cl_ft"];
    IMU_HEIGHT_WL_FT = doc["imu_height_wl_ft"];
    // Defer to Core 1 — the post-save applyChemistryOcvPreset/seedSocFromVoltage stall SSE delivery.
    // firstSave = !vesselInfoSaved: true across reboots until the form has been saved once; the client
    // fires the battery-defaults proposal on it, immune to a stale pre-flash browser tab.
    bool firstSave = !vesselInfoSaved;
    pendingSaveVesselInfo = true;
    request->send(200, "application/json",
                  String("{\"success\":true,\"firstSave\":") + (firstSave ? "true" : "false") + "}");
  });
  server.on("/get", HTTP_GET, [](AsyncWebServerRequest *request) {
    // Addressed write: the dashboard names the unit it is showing (uid=). One naming another unit came through a shared
    // address (alternator.local) and is refused whole, OnOff=0 included: the user is looking at the other board.
    if (addressedElsewhere(request)) {
      logClientWrite(request, "WRITE REJECTED: addressed to another regulator");
      request->send(409, "text/plain", device_id_hex);
      return;
    }
    bool foundParameter = false;
    bool heartbeat = false;       // commissionHeartbeat= seen: answered, never counted as a setting write (see the CSV3 echo at the end)
    bool nvsPersistNow = false;   // set by discrete reset/set handlers that write saveNVSDataFull()-owned vars; forces ONE immediate persist at the end so a reboot before the next field-off edge can't revert the action
    String inputMessage;

    // === SAFETY: Allow field OFF without arming ===
    if (request->hasParam("OnOff")) {
      int requestedState = request->getParam("OnOff")->value().toInt();
      if (requestedState == 0) {
        // Turning OFF is ALWAYS allowed — safety critical
        OnOff = 0;
        if (demoSeal) demoWriteThrough(NK_OnOff, "0");   // a safety OFF outlives demo mode: never back on at exit
        else settingWrite(NK_OnOff, "0");
        stateRevision++;
        // This path returns before the foundParameter block, so raise settingsDirty here for the CSV3 echo.
        settingsDirty = true;
        // Sender fingerprint: the dashboard sends only OnOff + src/n/uid, so any other param is a different sender.
        // Tracked so a later out-of-order ON from the same tab is recognized; an OFF itself is never dropped.
        bool offIsRepeat = false;
        clientWriteTrack(request, g_onOffWriteSrc, g_onOffWriteN, &g_onOffWriteNext, &offIsRepeat);
        logClientWrite(request, offIsRepeat ? "FIELD OFF: duplicate copy of a press already applied (field was already off)"
                                            : "FIELD OFF: Safety override (no arming required)");
        request->send(200, "text/plain", "0");
        return;
      }
      // If turning ON, fall through to arm check below
    }

    // Silence is allowed without arming, like OFF: it changes no setting, and a locked app must still be
    // able to quiet the buzzer. CheckAlarms consumes the flag on its next 250 ms pass.
    if (request->hasParam("SilenceAlarm")) {
      SilenceAlarm = 1;
      logClientWrite(request, "ALARM: silence requested (no arming required)");
      request->send(200, "text/plain", "1");
      return;
    }

    // A Victron GX write (gx=1) skips Unlock Controls, password or not, and never reads or changes the phones' unlock.
    const bool fromGx = gxFlagged(request);
    if (!fromGx && !settingsArmActive(request)) {
      // Console line so a rejected write is visible — a stale/replayed URL lands here silently.
      queueConsoleMessage("Settings write REJECTED: not armed (press Unlock Controls first)");
      request->send(403, "text/plain", "Settings not armed");
      return;
    }
    if (fromGx && gxParamDenied(request)) {
      request->send(403, "text/plain", "Not available from a Victron GX");
      return;
    }
    if (demoPhase == DEMO_EXITING) {   // the real settings are reloading; a write now would be lost
      request->send(503, "text/plain", "Leaving demo mode, try again in a moment.");
      return;
    }
    if (demoExitCutWaiting() && request->hasParam("OnOff") && request->getParam("OnOff")->value().toInt() != 0) {
      request->send(503, "text/plain", "Leaving demo mode, try again in a moment.");
      return;
    }
    if (demoSeal && demoParamDenied(request)) {
      demoRefuse(request);
      return;
    }

    // Arm-gated like every other mutation — a stale/replayed update URL must never reboot the
    // device. The dashboard's two update flows arm just-in-time on the user's confirm click.
    if (request->hasParam("UpdateToVersion")) {
      foundParameter = true;
      String selectedVersionStr = request->getParam("UpdateToVersion")->value();

      char selectedVersion[32];
      strncpy(selectedVersion, selectedVersionStr.c_str(), 31);
      selectedVersion[31] = '\0';

      // RAM only, so the reboot after a completed install brings Enable back from NVS. Every path below that ends
      // without a reboot puts it back here instead.
      const int onOffBeforeOta = OnOff;
      auto otaRestoreEnable = [onOffBeforeOta]() {
        if (OnOff != 0 || onOffBeforeOta == 0) return;
        OnOff = onOffBeforeOta;
        settingsDirty = true;
        queueConsoleMessage("OTA UPDATE: not installed - Alternator Enable back to its setting before the update");
      };
      OnOff = 0;
      Serial.println("OTA UPDATE: Alternator disabled for safety");
      queueConsoleMessage("OTA UPDATE: Alternator disabled for safety");

      unsigned long timeSinceLastNVSSave = millis() - lastNVSSaveTime;
      Serial.printf("UPDATE DEBUG: Time since last NVS save: %lu ms\n", timeSinceLastNVSSave);
      Serial.printf("UPDATE DEBUG: Free heap: %u bytes\n", ESP.getFreeHeap());

      size_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
      size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
      Serial.printf("UPDATE DEBUG: Free internal: %u, Largest block: %u\n", freeInternal, largestBlock);

      const esp_partition_t *running_partition = esp_ota_get_running_partition();
      const esp_partition_t *factory_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);

      if (running_partition == factory_partition) {
        Serial.printf("Direct update to version %s from factory partition\n", selectedVersion);
        performOTAUpdateToVersion(selectedVersion);   // a completed install reboots inside, so returning means it failed
        otaRestoreEnable();
        inputMessage = "Update initiated from factory partition";

      } else {
        Serial.println("UPDATE DEBUG: Storing update request in NVS");

        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open("update_req", NVS_READWRITE, &nvs_handle);
        if (err != ESP_OK) {
          Serial.printf("UPDATE DEBUG: Failed to open NVS namespace 'update_req': %s\n", esp_err_to_name(err));
          otaRestoreEnable();
          request->send(500, "text/plain", "NVS open failed");
          return;
        }

        nvs_erase_key(nvs_handle, "target_ver");
        nvs_erase_key(nvs_handle, "update_flag");

        err = nvs_set_str(nvs_handle, "target_ver", selectedVersion);
        if (err != ESP_OK) {
          Serial.printf("UPDATE DEBUG: Failed to set target_ver: %s\n", esp_err_to_name(err));
          nvs_close(nvs_handle);
          otaRestoreEnable();
          request->send(500, "text/plain", "NVS write failed");
          return;
        }

        uint8_t updateFlag = 1;
        err = nvs_set_u8(nvs_handle, "update_flag", updateFlag);
        if (err != ESP_OK) {
          Serial.printf("UPDATE DEBUG: Failed to set update_flag: %s\n", esp_err_to_name(err));
          nvs_close(nvs_handle);
          otaRestoreEnable();
          request->send(500, "text/plain", "NVS write failed");
          return;
        }

        uint8_t wakeFlag = 1;
        err = nvs_set_u8(nvs_handle, "wake_flag", wakeFlag);
        if (err != ESP_OK) {
          Serial.printf("UPDATE DEBUG: Failed to set wake_flag: %s\n", esp_err_to_name(err));
        }

        err = nvs_commit(nvs_handle);

        nvs_handle_t verify_handle;
        if (nvs_open("update_req", NVS_READONLY, &verify_handle) == ESP_OK) {
          uint8_t verifyFlag = 0;
          char verifyVersion[32] = { 0 };
          size_t verifySize = 32;

          if (nvs_get_u8(verify_handle, "update_flag", &verifyFlag) == ESP_OK && nvs_get_str(verify_handle, "target_ver", verifyVersion, &verifySize) == ESP_OK) {
            Serial.printf("UPDATE DEBUG: NVS write verified - flag: %u, version: %s\n", verifyFlag, verifyVersion);
          } else {
            Serial.println("UPDATE DEBUG: NVS write verification FAILED");
          }
          nvs_close(verify_handle);
        }

        Serial.printf("Update to version %s requested - rebooting to factory\n", selectedVersion);
        esp_ota_set_boot_partition(factory_partition);
        inputMessage = "Rebooting to factory for update";

        request->send(200, "text/plain", inputMessage);
        Serial.println("=== RESTART SEQUENCE ===");
        Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());
        Serial.printf("Active tasks: %u\n", uxTaskGetNumberOfTasks());
        Serial.println("Closing event connections...");
        events.close();
        gxEvents.close();
        Serial.println("========================================");
        Serial.println("NOTE: Task termination backtrace next is expected and harmless");
        Serial.println("========================================");
        Serial.flush();
        // Deferred like every other reboot path: the 200 above is only written once this handler
        // returns, so restarting inline discarded it and left the caller waiting on a dead socket.
        rebootRequestWhy = REBOOT_WHY_FACTORY;
        rebootRequestedAt = millis();
        rebootRequested = true;
        return;
      }
    }

    if (paramOk(request, "SystemIDStepAmplitude")) {
      foundParameter = true;
      inputMessage = request->getParam("SystemIDStepAmplitude")->value();
      settingWrite(NK_SystemIDStepAmplitude, inputMessage.c_str());
      SystemIDStepAmplitude = inputMessage.toFloat();
    }
    if (paramOk(request, "systemIDTestType")) {
      foundParameter = true;
      inputMessage = request->getParam("systemIDTestType")->value();
      settingWrite(NK_systemIDTestType, inputMessage.c_str());
      systemIDTestType = (uint8_t)inputMessage.toInt();
    }
    if (paramOk(request, "systemIDSineFreqStart")) {
      foundParameter = true;
      inputMessage = request->getParam("systemIDSineFreqStart")->value();
      settingWrite(NK_systemIDSineFreqStart, inputMessage.c_str());
      systemIDSineFreqStart = inputMessage.toFloat();
    }
    if (paramOk(request, "systemIDSineFreqEnd")) {
      foundParameter = true;
      inputMessage = request->getParam("systemIDSineFreqEnd")->value();
      settingWrite(NK_systemIDSineFreqEnd, inputMessage.c_str());
      systemIDSineFreqEnd = inputMessage.toFloat();
    }
    if (paramOk(request, "systemIDPlantTauMs")) {
      // Fitted plant tau from the dashboard Plant Delay sweep. Persisted with no field-off gate
      // (cheap compare-first scalar) so the actionable-disturbance readout survives reboots.
      foundParameter = true;
      inputMessage = request->getParam("systemIDPlantTauMs")->value();
      systemIDPlantTauMs = (uint16_t)inputMessage.toInt();
      settingWrite(NK_sysidPlantTau, String(systemIDPlantTauMs).c_str());
    }
    if (paramOk(request, "fieldDecayTauMs")) {
      // Commissioned field drain time (ms): worst case when RPM is unknown and for the 3× post-protection backstops.
      // Writing it FLATTENS the drain-vs-RPM line so a manual override applies at every speed; the wizard Apply sends
      // the endpoint params in the SAME request (handled after this flatten) to commission a line.
      foundParameter = true;
      inputMessage = request->getParam("fieldDecayTauMs")->value();
      int v = inputMessage.toInt();
      if (v < 5) v = 5; else if (v > 900) v = 900;
      fieldDecayTauMs = (uint16_t)v;
      settingWrite(NK_fieldDecayTau, String(fieldDecayTauMs).c_str());
      fdDrainLoMs = 0; fdDrainHiMs = 0; fdDrainRpmLo = 0; fdDrainRpmHi = 0;
      settingWrite(NK_fdDrainLoMs, "0"); settingWrite(NK_fdDrainHiMs, "0");
      settingWrite(NK_fdDrainRpmLo, "0"); settingWrite(NK_fdDrainRpmHi, "0");
    }
    if (paramOk(request, "fdDrainLoMs")) {
      foundParameter = true;
      int v = request->getParam("fdDrainLoMs")->value().toInt();
      fdDrainLoMs = (uint16_t)constrain(v, 0, 900);
      settingWrite(NK_fdDrainLoMs, String(fdDrainLoMs).c_str());
    }
    if (paramOk(request, "fdDrainHiMs")) {
      foundParameter = true;
      int v = request->getParam("fdDrainHiMs")->value().toInt();
      fdDrainHiMs = (uint16_t)constrain(v, 0, 900);
      settingWrite(NK_fdDrainHiMs, String(fdDrainHiMs).c_str());
    }
    if (paramOk(request, "fdDrainRpmLo")) {
      foundParameter = true;
      int v = request->getParam("fdDrainRpmLo")->value().toInt();
      fdDrainRpmLo = (uint16_t)constrain(v, 0, 10000);
      settingWrite(NK_fdDrainRpmLo, String(fdDrainRpmLo).c_str());
    }
    if (paramOk(request, "fdDrainRpmHi")) {
      foundParameter = true;
      int v = request->getParam("fdDrainRpmHi")->value().toInt();
      fdDrainRpmHi = (uint16_t)constrain(v, 0, 10000);
      settingWrite(NK_fdDrainRpmHi, String(fdDrainRpmHi).c_str());
    }
    if (paramOk(request, "systemIDSineCycles")) {
      foundParameter = true;
      inputMessage = request->getParam("systemIDSineCycles")->value();
      settingWrite(NK_systemIDSineCycles, inputMessage.c_str());
      systemIDSineCycles = (uint8_t)inputMessage.toInt();
    }
    if (paramOk(request, "SystemIDStabilizeAmps")) {
      foundParameter = true;
      inputMessage = request->getParam("SystemIDStabilizeAmps")->value();
      settingWrite(NK_SystemIDStabilizeAmps, inputMessage.c_str());
      SystemIDStabilizeAmps = inputMessage.toFloat();
    }

    if (request->hasParam("startSystemID")) {
      foundParameter = true;
      bool sysidModeOK = (sysMode == SYS_MODE_AUTO);
      // Mutex: refuse if any other field-driving test or tuning mode is on — they must run one at a time.
      const char *activeTuning = TuningMode ? "Current tuning"
                                 : CVTuningMode ? "Voltage tuning"
                                 : cvPlantFitActive ? "Voltage Control Autotuning"
                                 : batteryHealthTestActive ? "Battery health test"
                                 : resTestActive ? "Resonance current-check"
                                 : (fieldCurveActive != 0) ? "Field curve"
                                 : (fieldCutActive != 0) ? "Field cut"
                                 : (protTestActive != 0) ? "Protection test"
                                 : cvStressActive ? "CV stress test"
                                 : (altSweepActive != 0 || altSweepRequested) ? "Gate-tuning field sweep"
                         : (chcActive != 0 || chcRequested) ? "Charge health calibration" : nullptr;
      if (sysMode == SYS_MODE_MANUAL) {
        queueConsoleMessage("SystemID: start blocked — not allowed in Manual mode (duty is fixed; test cannot drive the field)");
      } else if (!sysidModeOK) {
        queueConsoleMessage("SystemID: start blocked — only allowed while charging in Auto mode (bulk, absorption, float, or target voltage)");
      } else if (activeTuning != nullptr) {
        queueConsoleMessageF("SystemID: start blocked — %s is active. Turn it off before running the Plant Delay Test.", activeTuning);
      } else if (systemIDActive == 0 && (millis() - systemIDLastEndMs) > 2000UL) {
        systemIDRunTok = webRunTok(request);
        systemIDRequested = true;
        systemIDResultsReady = false;
        systemIDAbortRequested = false;   // clear any stale abort from a prior run
        systemIDAbortReason = 0;          // clear prior abort reason so UI doesn't show stale value
        systemIDAbortPhase = 0;
        queueConsoleMessage("SystemID: test requested via web UI");
      } else {
        queueConsoleMessage("SystemID: start ignored (cooldown or already active)");
      }
    }

    if (request->hasParam("cancelSystemID")) {
      foundParameter = true;
      systemIDAbortRequested = true;
      queueConsoleMessage("SystemID: abort requested via web UI");
    }

    // ── CV plant fit (firmware voltage-loop identification, commissioning Step 7) ──
    if (paramOk(request, "cvPlantFitStart")) {
      foundParameter = true;
      float diMaxReq = paramOk(request, "diMax") ? request->getParam("diMax")->value().toFloat() : 0.0f;
      if (!cvpfStartTest(diMaxReq)) queueConsoleMessageF("CV plant-fit: cannot start — %s", cvpfAbortMsg);
    }
    if (request->hasParam("cvPlantFitCancel")) {
      foundParameter = true;
      if (cvpfState == 1) cvpfAbort("cancelled by user");
    }

    // ── CV stress test (commissioning stage 8 / standalone from Tuning ▸ Stress Test) ──
    if (paramOk(request, "CvStressDropV")) {
      foundParameter = true;
      inputMessage = request->getParam("CvStressDropV")->value();
      // floor: a non-positive drop parks the CV target AT/ABOVE idle — unreachable, inverts the test's "guaranteed reachable" premise
      CvStressDropV = clamp_f(inputMessage.toFloat(), 0.05f, 1.0f);
      settingWrite(NK_CvStressDropV, String(CvStressDropV, 2).c_str());
      queueConsoleMessageF("CV stress test target headroom: %.2f V below settled idle", CvStressDropV);
    }
    if (paramOk(request, "CvStressFailBandV")) {
      foundParameter = true;
      inputMessage = request->getParam("CvStressFailBandV")->value();
      CvStressFailBandV = clamp_f(inputMessage.toFloat(), 0.05f, 1.0f);
      settingWrite(NK_CvStressFailBandV, String(CvStressFailBandV, 2).c_str());
      queueConsoleMessageF("CV stress test stability fail band: %.2f V", CvStressFailBandV);
    }
    if (request->hasParam("cvStressStart")) {
      foundParameter = true;
      if (!cvStressStartTest()) queueConsoleMessageF("CV stress test: cannot start — %s", cvStressAbortMsg);
    }
    if (request->hasParam("cvStressCancel")) {
      foundParameter = true;
      if (cvStressActive) cvStressAbortRequested = true;
    }

    // ── Auto-commissioning: field-% curve (Phase 1a) ────────────────────────
    if (request->hasParam("startFieldCurve")) {
      foundParameter = true;
      const char *busy = (systemIDActive != 0) ? "Plant Delay test"
                         : TuningMode ? "Current tuning"
                         : CVTuningMode ? "Voltage tuning"
                         : cvPlantFitActive ? "Voltage Control Autotuning"
                         : batteryHealthTestActive ? "Battery health test"
                         : resTestActive ? "Resonance current-check"
                         : (fieldCurveActive != 0) ? "Field curve"
                         : (fieldCutActive != 0) ? "Field cut"
                         : (protTestActive != 0) ? "Protection test"
                         : cvStressActive ? "CV stress test"
                         : (altSweepActive != 0 || altSweepRequested) ? "Gate-tuning field sweep"
                         : (chcActive != 0 || chcRequested) ? "Charge health calibration" : nullptr;
      if (sysMode != SYS_MODE_AUTO) {
        queueConsoleMessage("Field curve: start blocked — only allowed while charging in Auto mode");
      } else if (const char *rr = rpmTestRefuseReason()) {
        queueConsoleMessageF("Field curve: start blocked — %s", rr);
      } else if (busy != nullptr) {
        queueConsoleMessageF("Field curve: start blocked — %s is active", busy);
      } else if ((millis() - fieldCurveLastEndMs) > 2000UL) {
        fieldCurveOnsetMode = false;  // saturation sweep (SystemID amplitudes)
        fieldCurveRunTok = webRunTok(request);
        fieldCurveRequested = true;
        fieldCurveResultsReady = false;
        fieldCurveAbortRequested = false;
        fieldCurveAbortReason = 0; fieldCurveAbortMsg[0] = '\0';  // clear prior abort latch
        fieldCurveAbortFollowOn = 0; fieldCurveAbortVolts = 0.0f; fieldCurveAbortDuty = 0.0f;
        queueConsoleMessage("Field curve: requested via web UI");
      } else {
        queueConsoleMessage("Field curve: start ignored (cooldown)");
      }
    }
    if (request->hasParam("cancelFieldCurve")) {
      foundParameter = true;
      fieldCurveAbortRequested = true;
      queueConsoleMessage("Field curve: abort requested via web UI");
    }

    // ── Auto-commissioning: field de-energize drain run (fires per speed inside stage 7) ──
    if (request->hasParam("fieldCutStart")) {
      foundParameter = true;
      const char *busy = (systemIDActive != 0) ? "Plant Delay test"
                         : TuningMode ? "Current tuning"
                         : CVTuningMode ? "Voltage tuning"
                         : cvPlantFitActive ? "Voltage Control Autotuning"
                         : batteryHealthTestActive ? "Battery health test"
                         : resTestActive ? "Resonance current-check"
                         : (fieldCurveActive != 0) ? "Field curve"
                         : (fieldCutActive != 0) ? "Field cut"
                         : (protTestActive != 0) ? "Protection test"
                         : cvStressActive ? "CV stress test"
                         : (altSweepActive != 0 || altSweepRequested) ? "Gate-tuning field sweep"
                         : (chcActive != 0 || chcRequested) ? "Charge health calibration" : nullptr;
      if (sysMode != SYS_MODE_AUTO) {
        queueConsoleMessage("Field cut: start blocked — only allowed while charging in Auto mode");
      } else if (const char *rr = rpmTestRefuseReason()) {
        queueConsoleMessageF("Field cut: start blocked — %s", rr);
      } else if (busy != nullptr) {
        queueConsoleMessageF("Field cut: start blocked — %s is active", busy);
      } else if ((millis() - fieldCutLastEndMs) > 2000UL) {
        fieldCutRunTok = webRunTok(request);
        fieldCutRequested = true;
        fieldCutResultsReady = false;
        fieldCutAbortRequested = false;
        fieldCutAbortMsg[0] = '\0';
        queueConsoleMessage("Field cut: requested via web UI");
      } else {
        queueConsoleMessage("Field cut: start ignored (cooldown)");
      }
    }
    if (request->hasParam("fieldCutCancel")) {
      foundParameter = true;
      fieldCutAbortRequested = true;
      queueConsoleMessage("Field cut: abort requested via web UI");
    }

    // ── Charge health system calibration (stage 9): one clock-scheduled run of four throttle cycles ──
    // The engine's maximum working RPM, entered on the setup screen. The test top is CHC_TOP_FRAC of it.
    if (paramOk(request, "maxWorkingRpm")) {
      foundParameter = true;
      maxWorkingRpm = (int)constrain(request->getParam("maxWorkingRpm")->value().toInt(), 800L, 8000L);
      settingWrite(NK_maxWorkingRpm, String(maxWorkingRpm).c_str());
      queueConsoleMessageF("Maximum working RPM set to %d (charge health calibration tops out at %d)", maxWorkingRpm, (int)lroundf(CHC_TOP_FRAC * maxWorkingRpm));
    }
    if (request->hasParam("chcStart")) {
      foundParameter = true;
      const char *busy = (systemIDActive != 0) ? "Plant Delay test"
                         : TuningMode ? "Current tuning"
                         : CVTuningMode ? "Voltage tuning"
                         : cvPlantFitActive ? "Voltage Control Autotuning"
                         : batteryHealthTestActive ? "Battery health test"
                         : resTestActive ? "Resonance current-check"
                         : (fieldCurveActive != 0) ? "Field curve"
                         : (fieldCutActive != 0) ? "Field cut"
                         : (protTestActive != 0) ? "Protection test"
                         : cvStressActive ? "CV stress test"
                         : (altSweepActive != 0 || altSweepRequested) ? "Gate-tuning field sweep"
                         : (chcActive != 0) ? "Charge health calibration" : nullptr;
      // A refusal is latched as a failed result, not just a console line: the wizard consumes a not-ok ready result at
      // once, otherwise it polls the full run timeout for a run never armed. Not when chc itself is the blocker (that
      // would abort the run in progress from the wizard's view).
      if (sysMode != SYS_MODE_AUTO) {
        queueConsoleMessage("Charge health calibration: start blocked — only allowed while charging in Auto mode");
        if (chcActive == 0) {
          chcOk = false; chcResultsReady = true;
          snprintf(chcAbortMsg, sizeof(chcAbortMsg), "start blocked — only allowed while charging in Auto mode");
        }
      } else if (chcRequested) {
        queueConsoleMessage("Charge health calibration: start ignored (already requested)");
      } else if (const char *rr = rpmTestRefuseReason()) {
        queueConsoleMessageF("Charge health calibration: start blocked — %s", rr);
        if (chcActive == 0) {
          chcOk = false; chcResultsReady = true;
          snprintf(chcAbortMsg, sizeof(chcAbortMsg), "start blocked — %s", rr);
        }
      } else if (maxWorkingRpm <= 0 && chcActive == 0) {
        queueConsoleMessage("Charge health calibration: start blocked — enter the maximum working RPM first");
        chcOk = false; chcResultsReady = true;
        snprintf(chcAbortMsg, sizeof(chcAbortMsg), "start blocked — enter the maximum working RPM first");
      } else if (busy != nullptr) {
        queueConsoleMessageF("Charge health calibration: start blocked — %s is active", busy);
        if (chcActive == 0) {
          chcOk = false; chcResultsReady = true;
          snprintf(chcAbortMsg, sizeof(chcAbortMsg), "start blocked — %s is active", busy);
        }
      } else if ((millis() - chcLastEndMs) > 2000UL) {
        if (paramOk(request, "chcPct"))
          chcTestPct = (uint8_t)constrain(request->getParam("chcPct")->value().toInt(), 20, 80);
        // NVS reads belong here, on the web task — never in the control tick that consumes them.
        chcLoadCapColumn(false, chcLoTable);
        chcLoadCapColumn(true,  chcHiTable);
        chcReqMs = millis();
        chcRequested = true;
        chcResultsReady = false;
        chcAbortMsg[0] = '\0';
        queueConsoleMessage("Charge health calibration: requested via web UI");
      } else {
        queueConsoleMessage("Charge health calibration: start ignored (cooldown)");
        if (chcActive == 0) {
          chcOk = false; chcResultsReady = true;
          snprintf(chcAbortMsg, sizeof(chcAbortMsg), "start blocked — cooling down, try again in a moment");
        }
      }
    }
    if (request->hasParam("chcCancel")) {
      foundParameter = true;
      chcAbortRequested = true;
      queueConsoleMessage("Charge health calibration: abort requested via web UI");
    }

    // ── Protection Actuation Tests (Settings → Emergency & Troubleshooting) ──
    //   Value setters (Set buttons): store into the globals; Fire uses the stored values. Independent
    //   ifs so each Set form (which carries only its own param) is handled on its own.
    if (paramOk(request, "protTestCutMs")) {
      protTestCutMs = (uint16_t)constrain(request->getParam("protTestCutMs")->value().toInt(), 20, 10000);
      foundParameter = true;
      queueConsoleMessageF("Protection test: cut duration = %u ms", protTestCutMs);
    }
    if (paramOk(request, "protTestGapMs")) {
      protTestGapMs = (uint16_t)constrain(request->getParam("protTestGapMs")->value().toInt(), 100, 20000);
      foundParameter = true;
      queueConsoleMessageF("Protection test: gap = %u ms", protTestGapMs);
    }
    if (paramOk(request, "protTestReps")) {
      protTestReps = (uint8_t)constrain(request->getParam("protTestReps")->value().toInt(), 1, 20);
      foundParameter = true;
      queueConsoleMessageF("Protection test: repeats = %u", protTestReps);
    }
    if (paramOk(request, "protTestAmps")) {
      float ptAmps = request->getParam("protTestAmps")->value().toFloat();
      protTestCmdA = (ptAmps < 5.0f) ? 0.0f : constrain(ptAmps, 5.0f, 100.0f);  // 0 = auto-seed at fire, and the only way BACK to auto
      foundParameter = true;
      if (protTestCmdA == 0.0f) queueConsoleMessage("Protection test: test current = auto (picked at fire)");
      else queueConsoleMessageF("Protection test: test current = %.0f A", protTestCmdA);
    }
    //   Fire buttons: trigger a run using the stored values. mode 1=instant cut, 2=load-dump,
    //   3=graceful ramp, 4=ladder walk (all AUTO + live-field gated), 5=manual bench cut (Manual Field).
    if (paramOk(request, "protTestStart")) {
      foundParameter = true;
      int mode = request->getParam("protTestStart")->value().toInt();
      const char *busy = (systemIDActive != 0) ? "Plant Delay test"
                         : TuningMode ? "Current tuning"
                         : CVTuningMode ? "Voltage tuning"
                         : cvPlantFitActive ? "Voltage Control Autotuning"
                         : batteryHealthTestActive ? "Battery health test"
                         : resTestActive ? "Resonance current-check"
                         : (fieldCurveActive != 0) ? "Field curve"
                         : (fieldCutActive != 0) ? "Field cut"
                         : cvStressActive ? "CV stress test"
                         : (altSweepActive != 0 || altSweepRequested) ? "Gate-tuning field sweep"
                         : (chcActive != 0 || chcRequested) ? "Charge health calibration" : nullptr;
      if (mode < 1 || mode > 5) {
        queueConsoleMessage("Protection test: start blocked — invalid mode");
      } else if (protTestActive != 0) {
        queueConsoleMessage("Protection test: already running");
      } else if (busy != nullptr) {
        queueConsoleMessageF("Protection test: start blocked — %s is active", busy);
      } else if ((millis() - protTestLastEndMs) < 5000UL) {
        queueConsoleMessage("Protection test: start ignored (cooldown)");
      } else if (mode == 5) {
        // Manual bench cut: uses the field the user already set (Manual Field into a resistor). No
        // AUTO / RPM gate — Ignore RPM is expected during bench work. Charging must be on: without it
        // there is no manual field to cut, and the queued request would fire later when it came on.
        if (ManualFieldToggle != 1) {
          queueConsoleMessage("Protection test: manual cut blocked — turn Manual Field ON first");
        } else if (!chargingEnabled) {
          queueConsoleMessage("Protection test: manual cut blocked — turn charging (On/Off) on first");
        } else {
          protTestMode = 5;
          protTestRepDone = 0;
          protTestAbortRequested = false;
          protTestRequested = true;
          queueConsoleMessageF("Protection test: manual hard cut requested — %u ms x%u (gap %u ms)",
                               protTestCutMs, protTestReps, protTestGapMs);
        }
      } else if (sysMode != SYS_MODE_AUTO) {
        queueConsoleMessage("Protection test: start blocked — engine-running tests need charging in Auto mode (in Manual mode, use Bench Testing instead)");
      } else if (!chargingEnabled) {
        queueConsoleMessage("Protection test: start blocked — charging must be enabled (engine running, On/Off on)");
      } else if (const char *rr = rpmTestRefuseReason()) {
        queueConsoleMessageF("Protection test: start blocked — %s", rr);
      } else if (RPM < (float)MinRPMForField) {
        queueConsoleMessageF("Protection test: start blocked — RPM %d below Min RPM For Field (%d); raise engine speed",
                             (int)RPM, MinRPMForField);
      } else {
        protTestMode = (uint8_t)mode;
        if (protTestCmdA < 5.0f) protTestCmdA = (SystemIDStabilizeAmps >= 5.0f) ? SystemIDStabilizeAmps : 30.0f;
        protTestRepDone = 0;
        protTestAbortRequested = false;
        protTestRequested = true;
        queueConsoleMessageF("Protection test: mode %d requested at %.0f A%s", mode, protTestCmdA,
                             mode == 4 ? " (ladder walk)" : "");
      }
    }
    if (request->hasParam("protTestCancel")) {
      foundParameter = true;
      protTestAbortRequested = true;
      queueConsoleMessage("Protection test: abort requested via web UI");
    }

    // ── Min% onset-knee sweep (commissioning stage 7) ───────────────────────
    // Field-curve ramp in onset-stop mode; each completed sweep is an anchor, applyKneeCurve fits the Min% column. NOT
    // gated on kneeLearnEnable: the floor table is always live (max with MinDuty); the toggle gates only the learner.
    if (request->hasParam("startKneeSweep")) {
      foundParameter = true;
      const char *busy = (systemIDActive != 0) ? "Plant Delay test"
                         : TuningMode ? "Current tuning"
                         : CVTuningMode ? "Voltage tuning"
                         : cvPlantFitActive ? "Voltage Control Autotuning"
                         : batteryHealthTestActive ? "Battery health test"
                         : resTestActive ? "Resonance current-check"
                         : (fieldCurveActive != 0) ? "Field curve"
                         : (fieldCutActive != 0) ? "Field cut"
                         : (protTestActive != 0) ? "Protection test"
                         : cvStressActive ? "CV stress test"
                         : (altSweepActive != 0 || altSweepRequested) ? "Gate-tuning field sweep"
                         : (chcActive != 0 || chcRequested) ? "Charge health calibration" : nullptr;
      if (sysMode != SYS_MODE_AUTO) {
        queueConsoleMessage("Keep-alive onset: start blocked — only allowed while charging in Auto mode");
      } else if (const char *rr = rpmTestRefuseReason()) {
        queueConsoleMessageF("Keep-alive onset: start blocked — %s", rr);
      } else if (busy != nullptr) {
        queueConsoleMessageF("Keep-alive onset: start blocked — %s is active", busy);
      } else if ((millis() - fieldCurveLastEndMs) > 2000UL) {
        fieldCurveOnsetMode = true;  // onset-stop sweep (tachometer keep-alive floor)
        fieldCurveRunTok = webRunTok(request);
        fieldCurveRequested = true;
        fieldCurveResultsReady = false;
        fieldCurveAbortRequested = false;
        fieldCurveAbortReason = 0; fieldCurveAbortMsg[0] = '\0';  // clear prior abort latch
        fieldCurveAbortFollowOn = 0; fieldCurveAbortVolts = 0.0f; fieldCurveAbortDuty = 0.0f;
        queueConsoleMessage("Keep-alive onset: sweep requested via web UI");
      } else {
        queueConsoleMessage("Keep-alive onset: start ignored (cooldown)");
      }
    }
    if (request->hasParam("cancelKneeSweep")) {
      foundParameter = true;
      fieldCurveAbortRequested = true;
      queueConsoleMessage("Keep-alive onset: abort requested via web UI");
    }

    // ── Gate-tuning capture: session logger + field sweeper (ALT_GATE_TUNING_CAPTURE_SPEC.md) ──
    // Each press only RAISES a flag: the 192 KB buffer is allocated, freed and state-changed on the control loop
    // (altLogService), so a press can never free it mid-row. Replies come back on the console.
    if (request->hasParam("altLogRecord")) {
      foundParameter = true;
      altLogStartReq = true;
    }
    if (request->hasParam("altLogStop")) {
      foundParameter = true;
      altLogStopReq = true;
    }
    if (request->hasParam("altLogClear")) {
      foundParameter = true;
      // Sent by the browser only AFTER the decoded CSV is in the user's hands, and by the Discard
      // button. The service refuses it while recording, so a stray press cannot erase a live capture.
      altLogClearReq = true;
    }
    if (request->hasParam("altSweepStart")) {
      foundParameter = true;
      const char *busy = (systemIDActive != 0) ? "Plant Delay test"
                         : TuningMode ? "Current tuning"
                         : CVTuningMode ? "Voltage tuning"
                         : cvPlantFitActive ? "Voltage Control Autotuning"
                         : batteryHealthTestActive ? "Battery health test"
                         : resTestActive ? "Resonance current-check"
                         : (fieldCurveActive != 0) ? "Field curve"
                         : (fieldCutActive != 0) ? "Field cut"
                         : (protTestActive != 0) ? "Protection test"
                         : cvStressActive ? "CV stress test"
                         : (altSweepActive != 0) ? "Gate-tuning field sweep"
                         : (chcActive != 0 || chcRequested) ? "Charge health calibration" : nullptr;
      if (sysMode != SYS_MODE_AUTO) {
        // Same rule as the field curve: the duty override lives in the AUTO control path, and the
        // bumpless resume restores AUTO state. In MANUAL the user's fixed duty owns the field —
        // which is the throttle-sweep condition anyway, and needs no sweeper.
        queueConsoleMessage("Field sweep: start blocked — only allowed while charging in Auto mode");
      } else if (altSweepRequested) {
        queueConsoleMessage("Field sweep: start ignored (already requested)");
      } else if (busy != nullptr) {
        queueConsoleMessageF("Field sweep: start blocked — %s is active", busy);
      } else if ((millis() - altSweepLastEndMs) > 2000UL) {
        // Do NOT clear altSweepAbortRequested: after an external teardown the tick's static phase is still mid-ramp and
        // only the latch makes the next tick reset it; clearing it would let this Start resume the stale ramp as an
        // instant field step (altSweep_tick consumes a stale latch itself). Parameters are taken only here because
        // altSweepLimitNow() reads the margins live, so a refused press must not rewrite the running sweep's turnaround.
        if (paramOk(request, "rate")) altSweepRatePctS = request->getParam("rate")->value().toFloat();
        if (paramOk(request, "from")) altSweepFromPct  = request->getParam("from")->value().toFloat();
        if (paramOk(request, "to"))   altSweepToPct    = request->getParam("to")->value().toFloat();
        // Margin 0 is legal ("turn around on the limit", relying on the hard-cut layer alone) but never the default.
        // Upper clamps keep a fat-fingered entry from shrinking the span under the 1% minimum and refusing every sweep.
        if (paramOk(request, "marginA"))
          altSweepMarginA = constrain(request->getParam("marginA")->value().toFloat(), 0.0f, 100.0f);
        if (paramOk(request, "marginV"))
          altSweepMarginV = constrain(request->getParam("marginV")->value().toFloat(),
                                      0.0f, 2.0f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f));
        altSweepReqMs = millis();
        altSweepRequested = true;
      } else {
        queueConsoleMessage("Field sweep: start ignored (cooldown)");
      }
    }
    if (request->hasParam("altSweepCancel")) {
      foundParameter = true;
      altSweepAbortRequested = true;
      queueConsoleMessage("Field sweep: abort requested via web UI");
    }
    // Reverse now: only the up leg can be turned around, and only while one is actually running —
    // a flag left armed from a refused press would turn the NEXT sweep around at its first tick.
    if (request->hasParam("altSweepReverse")) {
      foundParameter = true;
      if (altSweepActive == 1) altSweepReverseReq = true;
      else queueConsoleMessage("Field sweep: Reverse now ignored — no ramp is on its way up");
    }
    if (paramOk(request, "addKneeAnchor")) {
      foundParameter = true;
      // Keyed to the run that produced the result and committed exactly once: a sweep's completion frame reaches every
      // in-flight poll and each asks to commit. The token rejects another run's result, the consume below a repeat. ok
      // gates as well as duty: the "no onset found" exit stores the ceiling duty, which is > 0.
      const uint32_t tok = (uint32_t)strtoul(request->getParam("addKneeAnchor")->value().c_str(), nullptr, 10);
      if (tok != fieldCurveRunTok) {
        queueConsoleMessage("Keep-alive onset: commit ignored — it names a different sweep than the one that just ran");
      } else if (!kneeSweepOk || kneeSweepKneeDuty <= 0.0f) {
        queueConsoleMessage("Keep-alive onset: nothing to commit — this sweep found no clean onset, or its result was already committed");
      } else if (kneeAnchorN >= KNEE_ANCHOR_MAX) {
        queueConsoleMessage("Keep-alive onset: anchor list full");
      } else {
        kneeAnchorRPM[kneeAnchorN] = kneeSweepRPM;
        kneeAnchorDuty[kneeAnchorN] = kneeSweepKneeDuty;
        kneeAnchorTempF[kneeAnchorN] = kneeSweepTempF;
        kneeAnchorN++;
        // Refresh the live onset = a + C/RPM fit so /kneesweep.json (the review screen) can flag a bad
        // point before Apply. Needs >= 2 anchors; leaves residual = -1 until then.
        {
          float a, C, resid; int wi;
          if (kneeFitModel(a, C, resid, wi)) { kneeFitA = a; kneeFitC = C; kneeFitResidPct = resid; kneeFitWorstIdx = wi; }
          else { kneeFitResidPct = -1.0f; kneeFitWorstIdx = -1; }
        }
        queueConsoleMessageF("Keep-alive onset: anchor %d committed (%.1f%% @ %.0f RPM, %.0fF)",
                             kneeAnchorN, kneeSweepKneeDuty, kneeSweepRPM, kneeSweepTempF);
        kneeSweepKneeDuty = -1.0f;
        kneeSweepOk = false;
      }
    }
    if (request->hasParam("clearKneeAnchors")) {
      foundParameter = true;
      kneeAnchorN = 0;
      kneeFitResidPct = -1.0f; kneeFitWorstIdx = -1; kneeFitA = 0.0f; kneeFitC = 0.0f;
      queueConsoleMessage("Keep-alive onset: anchors cleared");
    }
    if (request->hasParam("applyKneeCurve")) {
      foundParameter = true;
      if (!kneeCurveApply())
        queueConsoleMessage("Keep-alive onset: need at least 3 anchors to fit a curve");
    }

    // ── Auto-commissioning: Phase-2 relaxed matrix gate ─────────────────────
    if (paramOk(request, "faCommissionGate")) {
      foundParameter = true;
      faCommissionGate = (request->getParam("faCommissionGate")->value().toInt() != 0);
      queueConsoleMessageF("Commissioning matrix gate %s", faCommissionGate ? "RELAXED (Phase 2)" : "strict");
    }

    // ── RPM ripple table game-fill window (RPM_RIPPLE_TABLE_SPEC §2.2) ──
    // Arm = wipe + stamp + open the fold (deferred to Core 1); disarm = freeze + persist. The browser
    // sends ripGameIdleRpm and the resTest level BEFORE arming — the wipe stamps both into the session.
    if (paramOk(request, "ripGameFill")) {
      foundParameter = true;
      bool on = (request->getParam("ripGameFill")->value().toInt() != 0);
      const char *rr = on ? rpmTestRefuseReason() : nullptr;   // an arm would wipe the map with nothing to refill it
      if (rr) {
        queueConsoleMessageF("RPM ripple sweep: start blocked — %s", rr);
      } else if (on) {
        ripTabPendingWipe = true;
      } else {
        ripGameFill = false;
        ripTabPendingWipe = false;  // cancel an arm that never executed
        ripTabPendingSave = true;
      }
      if (!rr) queueConsoleMessageF("RPM ripple sweep %s", on ? "started (table wiped)" : "ended (table frozen + saved)");
    }
    if (paramOk(request, "ripGameIdleRpm")) {
      foundParameter = true;
      long v = request->getParam("ripGameIdleRpm")->value().toInt();
      ripIdleRpmStage = (uint16_t)((v < 0) ? 0 : (v > 4000) ? 4000 : v);
    }

    // ── Active 3-current resonance test (COMMISSIONING_SPEC §3.2): arm = clear the ring + collect ──
    if (paramOk(request, "bcurRtest")) {
      foundParameter = true;
      bool on = (request->getParam("bcurRtest")->value().toInt() != 0);
      if (const char *rr = on ? rpmTestRefuseReason() : nullptr) {
        queueConsoleMessageF("Resonance current test: start blocked — %s", rr);
      } else {
        if (on) bcurRtestCount = 0;  // arm clears; windows append only when the §10 steadiness gates pass (both sensors)
        bcurRtestActive = on;
        queueConsoleMessageF("Resonance current test %s", on ? "ARMED (vary charge current)" : "stopped");
      }
    }

    // ── Auto-commissioning: state machine ───────────────────────────────────
    if (request->hasParam("commissionStart")) {
      foundParameter = true;
      // Repeat click while a Start is already staging (double-click / second tab): first staging
      // wins — the wizard's /cxStartState poll resolves either way.
      if (cxStartPersistStep == 0) {
        // Resuming a live run (IN_PROGRESS with its origin snapshot present) must NOT re-baseline: the snapshot and the
        // protection-flag backup keep their original values so a later Abort reverts to the true pre-commissioning tune.
        // Any other Start captures the current tune as origin.
        bool resumingRun = (commissionState == 1 && settingExists(NK_commissionSnap));
        if (!resumingRun) commissionProtBackup = testProtectionsEnabled;
        // Force protections ON for the run so the tuning-tab Protections sliders can't leak a stray "off"
        // into commissioning. Restored to the backup on done/abort.
        testProtectionsEnabled = true;
        // RAM-only staging; the loop worker retires the NVS writes one commit per pass so this handler never stalls the
        // network task. The wizard polls /cxStartState: HTTP 200 alone means accepted, not saved.
        cxStartPersistBegin(resumingRun);
        // Wipe the in-session onset-knee anchor/fit scratch so stage 7 starts clean; the applied rpmMinDutyTable floors
        // stay (cxStartPersistBegin already captured kneeFitA for the backup).
        kneeAnchorN = 0;
        kneeFitResidPct = -1.0f; kneeFitWorstIdx = -1; kneeFitA = 0.0f; kneeFitC = 0.0f;
      }
    }
    // Mark one wizard stage complete (i = 0=Prep…8=Stress test). Sets its done bit and clears
    // any downstream stage it feeds (coupling: see commissionDependentsMask). Drives the ✓ marks.
    if (paramOk(request, "commissionStageDone")) {
      foundParameter = true;
      int s = request->getParam("commissionStageDone")->value().toInt();
      commissionMarkStage(s);
      settingsDirty = true;
      queueConsoleMessageF("Commissioning: step %d complete", s + 1);
    }
    // Skip a step "for now": leave it outstanding (badge keeps nagging) but flag it hand-touched so the
    // Finish summary lists it for manual setup. Does not advance phase — the wizard moves on client-side.
    if (paramOk(request, "commissionStageSkip")) {
      foundParameter = true;
      int s = request->getParam("commissionStageSkip")->value().toInt();
      commissionSkipStage(s);
      settingsDirty = true;
      queueConsoleMessageF("Commissioning: step %d skipped (set manually later)", s + 1);
    }
    // Mark a step done BY HAND (unmeasured): counts toward COMMISSIONED so the nag can stop, flagged manual.
    if (paramOk(request, "commissionStageManual")) {
      foundParameter = true;
      int s = request->getParam("commissionStageManual")->value().toInt();
      commissionManualStage(s);
      settingsDirty = true;
      queueConsoleMessageF("Commissioning: step %d marked done manually", s + 1);
    }
    if (request->hasParam("commissionAbort")) {
      foundParameter = true;
      // Hoisted above the branch: every Abort must drop the analyzer gate and the ripple-fill mode, including
      // the cancel-before-start and nothing-to-abort branches. Both are idempotent when no run was in flight.
      faCommissionGate = false;
      // Abort is a teardown path too: committed cells from an aborted sweep are honest data captured
      // at level — freeze + persist, never discard (RPM_RIPPLE_TABLE_SPEC §2.2).
      if (ripGameFill || ripTabPendingWipe) { ripGameFill = false; ripTabPendingWipe = false; ripTabPendingSave = true; }
      // One atomic cancel picks the branch — a separate "still pending?" read could be retired by the
      // worker before the cancel lands. Resume/none fall through: a resume's cancel only stops the
      // bookkeeping writes, the original snapshot is intact for the teardown below.
      if (cxStartPersistCancel() == CX_CANCEL_FRESH) {
        // A fresh Start still staging never began: cancel back to the exact pre-click state. The live-run teardown
        // below would wrongly demote a previously-commissioned device.
        settingsDirty = true;
        queueConsoleMessage("Commissioning: start cancelled before the restore point was saved — nothing changed");
      } else if (commissionState != 1 && !settingExists(NK_commissionSnap)) {
        // No run in flight: a finished pass drops both the origin snapshot and the pre-run record, so the teardown below
        // would fall through to the zeroing and silently demote a COMMISSIONED device. Abort is a no-op here. State 1
        // with no snapshot still takes the teardown (older-firmware runs, which the zeroing is for).
        queueConsoleMessage("Commissioning: nothing to abort — no run is in progress, the committed tune and its record are untouched");
      } else {
        testProtectionsEnabled = commissionProtBackup;  // restore the user's manual-tuning protection setting
        bool reverted = commissionRestore();  // revert every setting to the Phase-0 snapshot
        // Bookkeeping goes back to the pre-run record (a COMMISSIONED device redoing a step is commissioned again). No
        // record (older-firmware run, or Clear-and-restart on a committed device) -> everything clears.
        if (!commissionRestorePreRun()) {
          commissionSetState(0);                // NOT_COMMISSIONED
          commissionSetPhase(0);                // clear checklist progress
          commissionDoneMask = 0;               // revert also drops all per-stage completion
          commissionWriteDoneMask();
          commissionManualMask = 0;             // …and all hand-set flags
          commissionWriteManualMask();
          cvStressForgetLast();                 // bit 8 went with the mask, and the revert undid the tune the verdict graded
        }
        settingRemove(NK_commissionStepSnap); // teardown: no interrupted step to revert on next boot
        cxStepMinPctDrop();                   // the origin Min% backup above already put the column back
        settingsDirty = true;
        queueConsoleMessageF("Commissioning: aborted — %s",
                             reverted ? "settings reverted to the pre-commissioning snapshot"
                                      : "no usable pre-state snapshot, settings left as they are");
      }
    }
    // Stop for now: keep every finished step (done marks, applied values, origin snapshot) and undo only
    // the step in progress, exactly what a reboot mid-wizard does. State stays IN_PROGRESS so the tab
    // offers Continue; the origin snapshot stays so a later Abort / Clear-and-restart can still fully revert.
    if (request->hasParam("commissionStop")) {
      foundParameter = true;
      // One atomic cancel picks the branch (see commissionAbort). FRESH = the worker was stopped before
      // state=1 was committed, so nothing began; resume/none get the live-run teardown.
      if (cxStartPersistCancel() == CX_CANCEL_FRESH) {
        settingsDirty = true;
        queueConsoleMessage("Commissioning: stopped before the restore point was saved — nothing changed");
      } else {
        faCommissionGate = false;
        if (ripGameFill || ripTabPendingWipe) { ripGameFill = false; ripTabPendingWipe = false; ripTabPendingSave = true; }
        testProtectionsEnabled = commissionProtBackup;
        bool undone = false;
        if (commissionState == 1 && settingExists(NK_commissionStepSnap)) undone = commissionRestoreScalars(NK_commissionStepSnap);
        // Stage 7 in progress: the keep-alive column it may have applied goes back to its entry copy too.
        if (commissionState == 1 && commissionPhase == 7 && cxStepMinPctRestore()) undone = true;
        cxStepMinPctDrop();
        settingRemove(NK_commissionStepSnap);
        settingsDirty = true;
        queueConsoleMessageF("Commissioning: stopped — finished steps kept%s", undone ? ", the step in progress was undone" : "");
      }
    }
    if (request->hasParam("commissionDone")) {
      foundParameter = true;
      cxStartPersistCancel();  // never expected mid-Done, but a raced Start must not commit state=1 after this teardown
      faCommissionGate = false;
      if (ripGameFill || ripTabPendingWipe) { ripGameFill = false; ripTabPendingWipe = false; ripTabPendingSave = true; }
      testProtectionsEnabled = commissionProtBackup;  // restore the user's manual-tuning protection setting
      settingRemove(NK_commissionSnap);     // commit the new tune (no snapshot ⇒ reboot won't revert)
      settingRemove(NK_commissionPreRun);   // the run is committed — nothing to abort back to
      settingRemove(NK_commissionStepSnap); // run over: drop the in-flight step baseline too
      cxStepMinPctDrop();
      commissionClearMinPctBackup();        // discard the Min% backup too — the new floors stay
      commissionRecomputeState();           // COMMISSIONED if every required stage done (stress test optional), else IN_PROGRESS (partial)
      commissionSetPhase(COMMISSION_STAGE_COUNT);  // wizard pass finished (= one past the last step)
      // Browser wall clock, not the device soft clock (which can be unset at sea). A garbage stamp is
      // rejected rather than written, because a far-future epoch would silence the age prompt forever.
      if (paramOk(request, "cmEpoch")) {
        long long e = strtoll(request->getParam("cmEpoch")->value().c_str(), nullptr, 10);
        if (e > 1735689600LL) {   // 2025-01-01
          CommissionEpoch = (time_t)e;
          char ebuf[24];
          snprintf(ebuf, sizeof(ebuf), "%lld", e);
          settingWrite(NK_CommissionEpoch, ebuf);
        }
      }
      commissionAgeAck = false;      settingWrite(NK_cmAgeAck, "0");
      commissionChangeFlag = false;  settingWrite(NK_cmChangeFlag, "0");
      cxLedgerLogFinish();  // ledger completion row: masks + state + CommissionTempF/Epoch (epoch stamped above)
      settingsDirty = true;
      queueConsoleMessageF("Commissioning: pass finished — %s",
                           commissionDoneMask >= COMMISSION_ALL_DONE ? "all steps complete, device COMMISSIONED"
                           : commissionRequiredComplete()            ? "device COMMISSIONED (stress test skipped — it's an optional reference check)"
                                                                     : "partial — some steps still pending");
    }
    // Wizard heartbeat: persist the phase so the tab checklist survives a reload / another client (last value =
    // finished). Does not change commissionState.
    if (paramOk(request, "commissionPhase")) {
      foundParameter = true;
      int p = request->getParam("commissionPhase")->value().toInt();
      if (p < 0) p = 0; if (p > COMMISSION_STAGE_COUNT) p = COMMISSION_STAGE_COUNT;
      bool phaseChanged = ((uint8_t)p != commissionPhase);
      commissionSetPhase((uint8_t)p);
      // Re-baseline the in-flight step snapshot on ENTERING a runnable step (not on a same-phase
      // heartbeat, which would swallow this step's own applies into the baseline). A reboot then undoes
      // only the step you're on; finish (p==STAGE_COUNT) and Prep (p==0) have no step to snapshot.
      if (phaseChanged && commissionState == 1 && p >= 1 && p < COMMISSION_STAGE_COUNT) {
        commissionStepSnapshot();
        // The keep-alive column is outside the scalar snapshot — stage 7 gets its own entry copy for Stop.
        if (p == 7) cxStepMinPctStash(); else cxStepMinPctDrop();
      }
      settingsDirty = true;
    }
    // Commissioning idle-rest heartbeat. The open wizard pings =1 every ~2 s to keep the field "rested"
    // at a low duty between steps; on a clean close it pings =0 to drop the hold and resume charging
    // immediately. A missing ping (crash / Wi-Fi drop) goes stale on its own after the firmware timeout.
    if (paramOk(request, "commissionHeartbeat")) {
      heartbeat = true;
      lastCommissionHeartbeatMs = (request->getParam("commissionHeartbeat")->value().toInt() != 0)
                                  ? millis() : 0;   // 0 = explicit exit → stale now → charging resumes
    }

    if (paramOk(request, "TemperatureLimitF")) {
      foundParameter = true;
      inputMessage = request->getParam("TemperatureLimitF")->value();
      settingWrite(NK_TemperatureLimitF, inputMessage.c_str());
      TemperatureLimitF = inputMessage.toInt();
    }
    // Cold-charge lockout (lithium protection) — master on/off. Turning it OFF is the user "override".
    if (paramOk(request, "coldChargeLockoutEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("coldChargeLockoutEnable")->value();
      coldChargeLockoutEnable = inputMessage.toInt() != 0;
      settingWrite(NK_coldChargeLockoutEnable, String((int)coldChargeLockoutEnable).c_str());
      queueConsoleMessageF("Cold-charge lockout (measured battery temperature): %s", coldChargeLockoutEnable ? "ENABLED" : "DISABLED");
    }
    // Cold-charge lockout temperature floor (battery temperature °F, stored raw)
    if (paramOk(request, "MinChargeTempF")) {
      foundParameter = true;
      inputMessage = request->getParam("MinChargeTempF")->value();
      MinChargeTempF = constrain(inputMessage.toFloat(), -40.0f, 120.0f);
      settingWrite(NK_MinChargeTempF, String(MinChargeTempF, 1).c_str());   // one decimal: a Celsius entry arrives as e.g. 35.6 (convertTempFormIfNeeded)
    }
    // Battery + extra temperature probes, battery-temperature source chain, hot-charge lockout
    // (BATTERY_TEMP_SENSORS_SPEC.md §6). Temperatures arrive in °F (the UI converts).
    if (paramOk(request, "battTempProbeEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("battTempProbeEnable")->value();
      battTempProbeEnable = (inputMessage.toInt() != 0) ? 1 : 0;
      settingWrite(NK_battTempProbeEnable, String(battTempProbeEnable).c_str());
    }
    if (paramOk(request, "extraTempProbeEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("extraTempProbeEnable")->value();
      extraTempProbeEnable = (inputMessage.toInt() != 0) ? 1 : 0;
      settingWrite(NK_extraTempProbeEnable, String(extraTempProbeEnable).c_str());
    }
    if (paramOk(request, "battTempSource")) {
      foundParameter = true;
      inputMessage = request->getParam("battTempSource")->value();
      battTempSource = constrain(inputMessage.toInt(), 0, 6);  // 0 Auto, 1..4 pinned sources, 6 None
      if (battTempSource == 5) battTempSource = 0;               // 5 (board) is retired: falls back to Auto
      settingWrite(NK_battTempSource, String(battTempSource).c_str());
    }
    // Hot-charge lockout — mirror of the cold one; acts only on a measured battery temperature (source 1..4).
    if (paramOk(request, "hotChargeLockoutEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("hotChargeLockoutEnable")->value();
      hotChargeLockoutEnable = (inputMessage.toInt() != 0) ? 1 : 0;
      settingWrite(NK_hotChargeLockoutEnable, String(hotChargeLockoutEnable).c_str());
      queueConsoleMessageF("Hot-charge lockout (measured battery temperature): %s", hotChargeLockoutEnable ? "ENABLED" : "DISABLED");
    }
    if (paramOk(request, "MaxChargeTempF")) {
      foundParameter = true;
      inputMessage = request->getParam("MaxChargeTempF")->value();
      MaxChargeTempF = constrain(inputMessage.toFloat(), 32.0f, 200.0f);   // the form's range; stored raw, a bad value (0 from a non-number) would lock charging out
      settingWrite(NK_MaxChargeTempF, String(MaxChargeTempF, 1).c_str());
    }
    if (paramOk(request, "extraTempAlarmHiEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("extraTempAlarmHiEnable")->value();
      extraTempAlarmHiEnable = (inputMessage.toInt() != 0) ? 1 : 0;
      settingWrite(NK_extraTempAlarmHiEnable, String(extraTempAlarmHiEnable).c_str());
    }
    if (paramOk(request, "extraTempAlarmHiF")) {
      foundParameter = true;
      inputMessage = request->getParam("extraTempAlarmHiF")->value();
      extraTempAlarmHiF = constrain(inputMessage.toFloat(), -40.0f, 300.0f);
      settingWrite(NK_extraTempAlarmHiF, String(extraTempAlarmHiF, 1).c_str());
    }
    if (paramOk(request, "extraTempAlarmLoEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("extraTempAlarmLoEnable")->value();
      extraTempAlarmLoEnable = (inputMessage.toInt() != 0) ? 1 : 0;
      settingWrite(NK_extraTempAlarmLoEnable, String(extraTempAlarmLoEnable).c_str());
    }
    if (paramOk(request, "extraTempAlarmLoF")) {
      foundParameter = true;
      inputMessage = request->getParam("extraTempAlarmLoF")->value();
      extraTempAlarmLoF = constrain(inputMessage.toFloat(), -40.0f, 300.0f);
      settingWrite(NK_extraTempAlarmLoF, String(extraTempAlarmLoF, 1).c_str());
    }
    // 1-Wire probe registry actions. A scan is a request to TempTask (it owns the bus); a role assignment
    // writes NVS right here (explicit user action, the one exception to the field-off flash rule) and
    // then asks TempTask to rebind. An id already bound to another role is refused.
    {
      static const char *const owAssignParams[TR_COUNT] = { "owAssignAlt", "owAssignBatt", "owAssignExtra" };
      static const char *const owAssignKeys[TR_COUNT] = { NK_owAddrAlt, NK_owAddrBatt, NK_owAddrExtra };
      for (int r = 0; r < TR_COUNT; r++) {
        if (!request->hasParam(owAssignParams[r])) continue;
        foundParameter = true;
        inputMessage = request->getParam(owAssignParams[r])->value();
        inputMessage.trim();
        inputMessage.toLowerCase();
        DeviceAddress a;
        const bool clearRole = (inputMessage.length() == 0 || inputMessage == "none");
        if (clearRole) {
          memset(a, 0, sizeof(a));
        } else if (!owHexToAddr(inputMessage.c_str(), a)) {
          inputMessage = "Rejected: probe id must be 16 hex characters or none";
          continue;
        }
        bool taken = false;
        if (!clearRole) {
          for (int o = 0; o < TR_COUNT; o++) {
            if (o != r && memcmp(tempRoleAddr[o], a, sizeof(DeviceAddress)) == 0) taken = true;
          }
        }
        if (taken) {
          inputMessage = "Rejected: that probe is already assigned to another role";
          continue;
        }
        portENTER_CRITICAL(&owMux);
        memcpy(tempRoleAddr[r], a, sizeof(DeviceAddress));
        tempRoleAssigned[r] = !clearRole;
        tempRoleSlot[r] = -1;
        if (!clearRole) {
          for (uint8_t i = 0; i < owProbeCount; i++) {
            if (owProbes[i].present && memcmp(owProbes[i].addr, a, sizeof(DeviceAddress)) == 0) { tempRoleSlot[r] = (int8_t)i; break; }
          }
        }
        owRolePersistPending[r] = false;  // this write supersedes any queued auto-bind
        portEXIT_CRITICAL(&owMux);
        settingWrite(owAssignKeys[r], clearRole ? "" : inputMessage.c_str());
        owScanRequested = true;
        if (clearRole) queueConsoleMessageF("Temperature probe role cleared: %s", owRoleName(r));
        else queueConsoleMessageF("Temperature probe %s assigned to the %s role", inputMessage.c_str(), owRoleName(r));
      }
    }
    // Placed after the assignment loop so config_drift_check.py's hasParam window sees no settingWrite (momentary action, persists nothing).
    if (request->hasParam("owScan")) {
      foundParameter = true;
      owScanRequested = true;
      inputMessage = "1";
    }
    if (request->hasParam("ClearBuffer")) {
      foundParameter = true;
      clearSensorBuffer();
      queueConsoleMessage("Upload buffer manually cleared from web");
      inputMessage = "1";
    }
    if (request->hasParam("ResetAlternatorHealth")) {
      foundParameter = true;
      pendingResetAlternatorHealth = true;  // deferred to Core 1 to avoid SSE gap (Start Over)
    }
    if (request->hasParam("FastAltClearMatrix")) {
      foundParameter = true;
      faPendingMatrixClear = true;  // deferred to Core 1 (flash remove) — fast alt-current disturbance matrix wipe
    }
    if (request->hasParam("ResetRipplePeaks")) {
      // Ripple analyzer's own worst-value reset (Session Worst Pk-Pk / Worst Peak / Worst Hz).
      // These persist across reboot, so clear + commit now; prev_* shadows left alone so the
      // cleared values actually write (prev != 0 → saveNVSDataFull writes).
      foundParameter = true;
      faSesPkpkWorstA = 0.0f;
      faSesPkpkAmpsA = 0.0f;     // clear the pk-pk operating-point context too
      faSesPkpkTempF = NAN;
      faSesPkpkRpm = 0;
      faSesPkpkEpoch = 0;
      faSesPeakWorstA = 0.0f;
      faSesPeakWorstHz = 0.0f;
      faDomReset();  // Highest Tone in Map headline clears with the other worsts
      nvsPersistNow = true;
      queueConsoleMessage("Ripple analyzer worst values: Reset requested from web interface");
    }
    if (request->hasParam("FastAltRebaseline")) {
      foundParameter = true;
      faPendingRebaseline = true;  // deferred to Core 1 — clears the reference flipbook (freeze-once pages re-capture)
    }
    // Fast alt-current diagnostic knobs (Pattern B). Globals are updated live so faDrain()
    // and the steady-state gate react immediately; no reboot needed (faEnabled toggles the driver).
    if (paramOk(request, "faEnabled")) {
      foundParameter = true;
      faEnabled = (request->getParam("faEnabled")->value().toInt() != 0);
      settingWrite(NK_faEnabled, faEnabled ? "1" : "0");
    }
    if (paramOk(request, "faAlarmEnable")) {
      foundParameter = true;
      faAlarmEnable = (request->getParam("faAlarmEnable")->value().toInt() != 0);
      settingWrite(NK_faAlarmEnable, faAlarmEnable ? "1" : "0");
    }
    if (paramOk(request, "faAnomPause")) {
      foundParameter = true;
      faAnomPause = (request->getParam("faAnomPause")->value().toInt() != 0);
      settingWrite(NK_faAnomPause, faAnomPause ? "1" : "0");
    }
    if (paramOk(request, "faRpmEdgeMargin")) {
      foundParameter = true;
      faRpmEdgeMargin = request->getParam("faRpmEdgeMargin")->value().toFloat();
      settingWrite(NK_faRpmEdgeMargin, String(faRpmEdgeMargin, 1).c_str());
    }
    if (paramOk(request, "faAmpsDriftFloorA")) {
      foundParameter = true;
      faAmpsDriftFloorA = request->getParam("faAmpsDriftFloorA")->value().toFloat();
      settingWrite(NK_faAmpsDriftFloorA, String(faAmpsDriftFloorA, 2).c_str());
    }
    if (paramOk(request, "faAmpsDriftPct")) {
      foundParameter = true;
      faAmpsDriftPct = request->getParam("faAmpsDriftPct")->value().toFloat();
      settingWrite(NK_faAmpsDriftPct, String(faAmpsDriftPct, 1).c_str());
    }
    // Measured-ripple capture admission gates (§10.8/§11) — own knobs, decoupled from the fa* detector gates
    if (paramOk(request, "ripWinMs")) {
      foundParameter = true;
      // Floor 500 ms: each HALF-window (§11 stationarity/min-of-halves) must hold ≥1 cycle of the lowest
      // resolvable disturbance; ceiling 4 s bounds capture latency. Also sizes the min-sample gate.
      float ripWinPrev = ripWinMs;
      ripWinMs = clamp_f(request->getParam("ripWinMs")->value().toFloat(), 500.0f, 4000.0f);
      settingWrite(NK_ripWinMs, String(ripWinMs, 0).c_str());
      // Window length defines BOTH measured quantities: the min-of-halves pk-pk and the worst
      // half-window voltage slope. Drop both fits (see ripFitForget).
      if (fabsf(ripWinMs - ripWinPrev) > 0.5f) ripFitForget(true, true, "Ripple capture window changed");
    }
    if (paramOk(request, "ripDriftFloorA")) {
      foundParameter = true;
      ripDriftFloorA = clamp_f(request->getParam("ripDriftFloorA")->value().toFloat(), 0.0f, 50.0f);
      settingWrite(NK_ripDriftFloorA, String(ripDriftFloorA, 2).c_str());
    }
    if (paramOk(request, "ripDriftPct")) {
      foundParameter = true;
      ripDriftPct = clamp_f(request->getParam("ripDriftPct")->value().toFloat(), 0.0f, 50.0f);
      settingWrite(NK_ripDriftPct, String(ripDriftPct, 1).c_str());
    }
    if (paramOk(request, "faAttenUpAmps")) {
      foundParameter = true;
      faAttenUpAmps = request->getParam("faAttenUpAmps")->value().toFloat();
      settingWrite(NK_faAttenUpAmps, String(faAttenUpAmps, 1).c_str());
    }
    if (paramOk(request, "faAttenDownAmps")) {
      foundParameter = true;
      faAttenDownAmps = request->getParam("faAttenDownAmps")->value().toFloat();
      settingWrite(NK_faAttenDownAmps, String(faAttenDownAmps, 1).c_str());
    }
    if (paramOk(request, "faPeakMinA")) {
      foundParameter = true;
      faPeakMinA = request->getParam("faPeakMinA")->value().toFloat();
      settingWrite(NK_faPeakMinA, String(faPeakMinA, 2).c_str());
    }
    // ---- Auto Min% learning ("knee tracker") knobs ----
    if (paramOk(request, "kneeLearnEnable")) {
      foundParameter = true;
      kneeLearnEnable = (request->getParam("kneeLearnEnable")->value().toInt() != 0);
      settingWrite(NK_kneeLearnEnable, kneeLearnEnable ? "1" : "0");
      if (kneeLearnEnable) {
        // Take ownership of the Min% column immediately from the learned floors (bin 0 always 0%,
        // undecided bins to the baseline) — same rule as boot and reset, one implementation.
        kneeRebuildOwnedTable();
      }
    }
    if (paramOk(request, "kneeMarginPct")) {
      foundParameter = true;
      kneeMarginPct = request->getParam("kneeMarginPct")->value().toFloat();
      settingWrite(NK_kneeMarginPct, String(kneeMarginPct, 2).c_str());
    }
    if (paramOk(request, "kneeOnsetA")) {
      foundParameter = true;
      kneeOnsetA = request->getParam("kneeOnsetA")->value().toFloat();
      settingWrite(NK_kneeOnsetA, String(kneeOnsetA, 2).c_str());
    }
    if (paramOk(request, "kneeReArmA")) {
      foundParameter = true;
      kneeReArmA = request->getParam("kneeReArmA")->value().toFloat();
      settingWrite(NK_kneeReArmA, String(kneeReArmA, 2).c_str());
    }
    if (paramOk(request, "kneeDwellSec")) {
      foundParameter = true;
      kneeDwellSec = request->getParam("kneeDwellSec")->value().toFloat();
      settingWrite(NK_kneeDwellSec, String(kneeDwellSec, 1).c_str());
    }
    if (paramOk(request, "kneeStepPct")) {
      foundParameter = true;
      kneeStepPct = request->getParam("kneeStepPct")->value().toFloat();
      settingWrite(NK_kneeStepPct, String(kneeStepPct, 2).c_str());
    }
    if (paramOk(request, "kneeTempComp")) {
      foundParameter = true;
      kneeTempComp = (request->getParam("kneeTempComp")->value().toInt() != 0);
      settingWrite(NK_kneeTempComp, kneeTempComp ? "1" : "0");
    }
    if (paramOk(request, "kneeTempRefF")) {
      foundParameter = true;
      kneeTempRefF = request->getParam("kneeTempRefF")->value().toFloat();
      settingWrite(NK_kneeTempRefF, String(kneeTempRefF, 1).c_str());
    }
    if (paramOk(request, "kneeMaxFloorPct")) {
      foundParameter = true;
      kneeMaxFloorPct = request->getParam("kneeMaxFloorPct")->value().toFloat();
      settingWrite(NK_kneeMaxFloorPct, String(kneeMaxFloorPct, 2).c_str());
    }
    if (paramOk(request, "kneeRpmTolPct")) {
      foundParameter = true;
      kneeRpmTolPct = request->getParam("kneeRpmTolPct")->value().toFloat();
      settingWrite(NK_kneeRpmTolPct, String(kneeRpmTolPct, 1).c_str());
    }
    if (paramOk(request, "kneeTempTolF")) {
      foundParameter = true;
      kneeTempTolF = request->getParam("kneeTempTolF")->value().toFloat();
      settingWrite(NK_kneeTempTolF, String(kneeTempTolF, 1).c_str());
    }
    if (paramOk(request, "kneeDutyTolPct")) {
      foundParameter = true;
      kneeDutyTolPct = request->getParam("kneeDutyTolPct")->value().toFloat();
      settingWrite(NK_kneeDutyTolPct, String(kneeDutyTolPct, 2).c_str());
    }
    if (request->hasParam("ResetKneeLearn")) {
      foundParameter = true;
      kneeLearnResetDefaults();
    }
    // ---- Zero-drift characterization log (diagnostic) ----
    if (paramOk(request, "ZeroLogEnable")) {
      foundParameter = true;
      ZeroLogEnable = (request->getParam("ZeroLogEnable")->value().toInt() != 0);
      settingWrite(NK_ZeroLogEnable, ZeroLogEnable ? "1" : "0");
    }
    if (request->hasParam("ResetZeroLog")) {
      foundParameter = true;
      zeroLogResetAll();
    }
    if (paramOk(request, "wifiNapEnabled")) {
      foundParameter = true;
      wifiNapEnabled = (request->getParam("wifiNapEnabled")->value().toInt() != 0);
      settingWrite(NK_wifiNapEnabled, wifiNapEnabled ? "1" : "0");
    }
    // Alternator-health steady-state / window / record settings (registry-driven)
    if (altSettingsHandle(request)) {
      foundParameter = true;
      sendAltSettings();
    }
    if (request->hasParam("ResetBoatPerformance")) {
      foundParameter = true;
      pendingResetBoatPerformance = true;  // deferred to Core 1 to avoid SSE gap (Clear all data)
    }
    // Boat-performance polar settings (registry-driven, incl. sticky Pause + speed-source selector)
    if (perfSettingsHandle(request)) {
      foundParameter = true;
      sendPerfSettings();
    }

    if (paramOk(request, "ManualDutyTarget")) {
      foundParameter = true;
      inputMessage = request->getParam("ManualDutyTarget")->value();
      ManualDutyTarget = constrain(inputMessage.toFloat(), 0.0f, 100.0f);
      settingWrite(NK_ManualDutyTarget, String(ManualDutyTarget, 2).c_str());
    }
    if (paramOk(request, "socInfoAvailable")) {
      foundParameter = true;
      inputMessage = request->getParam("socInfoAvailable")->value();
      settingWrite(NK_socInfoAvailable, inputMessage.c_str());
      socInfoAvailable = inputMessage.toInt();
    }
    if (paramOk(request, "TailCurrent_A")) {
      foundParameter = true;
      inputMessage = request->getParam("TailCurrent_A")->value();
      settingWrite(NK_TailCurrent_A, inputMessage.c_str());
      TailCurrent_A = inputMessage.toFloat();
    }
    if (paramOk(request, "RebulkVoltage")) {
      foundParameter = true;
      inputMessage = request->getParam("RebulkVoltage")->value();
      settingWrite(NK_RebulkVoltage, inputMessage.c_str());
      RebulkVoltage = inputMessage.toFloat();
    }
    if (paramOk(request, "rebulkDebounceTime")) {
      foundParameter = true;
      inputMessage = request->getParam("rebulkDebounceTime")->value();
      rebulkDebounceTime = (uint32_t)inputMessage.toInt() * 1000UL;  // sec → ms
      // Save the ms value so boot load is consistent
      settingWrite(NK_rebulkDebounceTime, String(rebulkDebounceTime).c_str());
    }

    if (paramOk(request, "MinFloatTime")) {
      foundParameter = true;
      inputMessage = request->getParam("MinFloatTime")->value();
      MinFloatTime = (uint32_t)inputMessage.toInt() * 60000UL;  // min → ms
      settingWrite(NK_MinFloatTime, String(MinFloatTime).c_str());
    }
    if (paramOk(request, "SOC_BlockRebulk_percent")) {
      foundParameter = true;
      inputMessage = request->getParam("SOC_BlockRebulk_percent")->value();
      settingWrite(NK_SOC_BlockRebulk_percent, inputMessage.c_str());
      SOC_BlockRebulk_percent = inputMessage.toInt();
    }
    if (paramOk(request, "SOC_AllowRebulk_percent")) {
      foundParameter = true;
      inputMessage = request->getParam("SOC_AllowRebulk_percent")->value();
      settingWrite(NK_SOC_AllowRebulk_percent, inputMessage.c_str());
      SOC_AllowRebulk_percent = inputMessage.toInt();
    }
    if (paramOk(request, "BulkVoltage")) {
      foundParameter = true;
      inputMessage = request->getParam("BulkVoltage")->value();
      settingWrite(NK_BulkVoltage, inputMessage.c_str());
      BulkVoltage = inputMessage.toFloat();
    }
    // System voltage class is not a /get setting: /saveVesselInfo sets it via applyNominalVoltageChange().
    if (paramOk(request, "wavePeriod")) {
      foundParameter = true;
      inputMessage = request->getParam("wavePeriod")->value();
      settingWrite(NK_wavePeriod, inputMessage.c_str());
      wavePeriod = inputMessage.toInt();
      if (TuningMode) tuningParamChanged = true;
    }
    if (paramOk(request, "tuningWaveform")) {
      foundParameter = true;
      inputMessage = request->getParam("tuningWaveform")->value();
      settingWrite(NK_tuningWaveform, inputMessage.c_str());
      tuningWaveform = inputMessage.toInt();
      if (TuningMode) tuningParamChanged = true;
    }
    if (paramOk(request, "tuningSineFreq")) {
      foundParameter = true;
      inputMessage = request->getParam("tuningSineFreq")->value();
      settingWrite(NK_tuningSineFreq, inputMessage.c_str());
      tuningSineFreq = inputMessage.toFloat();
    }
    if (paramOk(request, "tuningSweepStart")) {
      foundParameter = true;
      inputMessage = request->getParam("tuningSweepStart")->value();
      settingWrite(NK_tuningSweepStart, inputMessage.c_str());
      tuningSweepStart = inputMessage.toFloat();
    }
    if (paramOk(request, "tuningSweepEnd")) {
      foundParameter = true;
      inputMessage = request->getParam("tuningSweepEnd")->value();
      settingWrite(NK_tuningSweepEnd, inputMessage.c_str());
      tuningSweepEnd = inputMessage.toFloat();
    }
    if (paramOk(request, "tuningSweepCycles")) {
      foundParameter = true;
      inputMessage = request->getParam("tuningSweepCycles")->value();
      settingWrite(NK_tuningSweepCycles, inputMessage.c_str());
      tuningSweepCycles = (uint8_t)inputMessage.toInt();
    }
    if (request->hasParam("startTuningSweep")) {
      foundParameter = true;
      if (systemIDActive != 0 || fieldCurveActive != 0) {
        queueConsoleMessage("Tuning sweep: start blocked — a SystemID/Field-curve test is active");
      } else {
        tuningRunTok = webRunTok(request);
        tuningSweepRequested = true;   // momentary — the TuningMode sine block consumes it
      }
    }
    if (paramOk(request, "SwitchingFrequency")) {
      foundParameter = true;
      inputMessage = request->getParam("SwitchingFrequency")->value();
      // LEDC 12-bit ceiling is 19455 Hz (clock divider must exceed 1.0); a higher value in NVS kills the boot-time PWM attach.
      int requestedFreq = constrain(inputMessage.toInt(), 100, 19455);
      settingWrite(NK_SwitchingFrequency, String(requestedFreq).c_str());
      SwitchingFrequency = requestedFreq;
      queueConsoleMessageF("Frequency target set to %dHz", requestedFreq);
    }
    if (paramOk(request, "FloatVoltage")) {
      foundParameter = true;
      inputMessage = request->getParam("FloatVoltage")->value();
      settingWrite(NK_FloatVoltage, inputMessage.c_str());
      FloatVoltage = inputMessage.toFloat();
    }
    if (paramOk(request, "yyMin")) {
      foundParameter = true;
      inputMessage = request->getParam("yyMin")->value();
      settingWrite(NK_yyMin, inputMessage.c_str());
      yyMin = inputMessage.toInt();
    }
    if (paramOk(request, "ManualFieldToggle")) {
      foundParameter = true;
      bool manualIsRepeat = false;
      if (clientWriteTrack(request, g_manualWriteSrc, g_manualWriteN, &g_manualWriteNext, &manualIsRepeat)) {
        logClientWrite(request, manualIsRepeat ? "FIELD CONTROL write IGNORED: duplicate copy of a press already applied"
                                               : "FIELD CONTROL write IGNORED: out of order for its tab (a later press already applied)");
      } else {
        inputMessage = request->getParam("ManualFieldToggle")->value();
        settingWrite(NK_ManualFieldToggle, inputMessage.c_str());
        ManualFieldToggle = inputMessage.toInt();
        // Fingerprinted like the master switch: this is the write that bypasses the protections.
        logClientWrite(request, (ManualFieldToggle == 1) ? "FIELD CONTROL: MANUAL (protections bypassed)"
                                                         : "FIELD CONTROL: AUTO");
      }
    }
    if (paramOk(request, "capLimitMode")) {
      foundParameter = true;
      inputMessage = request->getParam("capLimitMode")->value();
      settingWrite(NK_capLimitMode, inputMessage.c_str());
      capLimitMode = constrain(inputMessage.toInt(), 0, 1);
    }
    if (paramOk(request, "PhysicalPanelOverride")) {
      foundParameter = true;
      inputMessage = request->getParam("PhysicalPanelOverride")->value();
      PhysicalPanelOverride = (inputMessage.toInt() == 1) ? 1 : 0;  // clamped: the resolvers test == 0 and the app gates on === 1, so any other value would split them
      settingWrite(NK_PhysicalPanelOverride, String(PhysicalPanelOverride).c_str());
      // Re-resolve now so the CSV3 echo of this request already carries the new owner's modes (turning the
      // override off must put the app's stored choice back on screen in the same round trip).
      if (applyChargeRateMode(resolveChargeRateMode())) {
        queueConsoleMessageF("Charge rate mode: switched to %s", HiLow == 1 ? "Normal" : "Low");
      }
      const int wantMaintain = resolveMaintainMode();
      if (wantMaintain != MaintainMode) {
        MaintainMode = wantMaintain;
        queueConsoleMessageF("MaintainMode mode %s", MaintainMode ? "enabled" : "disabled");
      }
      queueConsoleMessage(PhysicalPanelOverride ? "Physical Panel Override on: the Cable 3 switches now set the charge rate and Force Maintain Mode, and the toggles in this app are inert."
                                                : "Physical Panel Override off: this app sets the charge rate and Force Maintain Mode. The Cable 3 switches are still reported but change nothing.");
      // Handing the mode back to a stored Maintain choice that Target Voltage outranks looks like the app ignored the switch.
      if (PhysicalPanelOverride == 0 && MaintainModeUserSel == 1 && TargetVoltageMode == 1) {
        queueConsoleMessage("Your stored Force Maintain Mode choice stays off: Target Voltage mode is on and the two are mutually exclusive. Turn Target Voltage mode off to get Maintain Mode back.");
      }
    }
    if (paramOk(request, "MaintainMode")) {
      foundParameter = true;
      inputMessage = request->getParam("MaintainMode")->value();
      MaintainModeUserSel = inputMessage.toInt();  // the app's stored choice; inert while PhysicalPanelOverride is on, but still recorded so turning the override back off restores it
      if (!BatteryShuntPresent) MaintainModeUserSel = 0;  // no battery-current sensor at all → 0-net-amps hold impossible
      settingWrite(NK_MaintainMode, String(MaintainModeUserSel).c_str());
      if (MaintainModeUserSel && PhysicalPanelOverride == 0) {
        // Mutually exclusive with TargetVoltageMode. Cleared only while the app owns the mode: under the panel
        // override this choice is inert, so clearing Target Voltage would cost the user that mode for nothing.
        TargetVoltageMode = 0;
        settingWrite(NK_TargetVoltageMode, "0");
      }
      MaintainMode = resolveMaintainMode();  // effective value: this choice only lands while the override is off
      if (PhysicalPanelOverride) queueConsoleMessageF("MaintainMode choice stored (%s) - the Cable 3 Force Float switch decides while Physical Panel Override is on", MaintainModeUserSel ? "on" : "off");
      else queueConsoleMessageF("MaintainMode mode %s", MaintainMode ? "enabled" : "disabled");
    }
    if (paramOk(request, "TargetVoltageMode")) {
      foundParameter = true;
      inputMessage = request->getParam("TargetVoltageMode")->value();
      TargetVoltageMode = inputMessage.toInt();
      settingWrite(NK_TargetVoltageMode, inputMessage.c_str());
      if (TargetVoltageMode) {
        // MaintainMode and TargetVoltageMode are mutually exclusive — clear the other.
        MaintainMode = 0;
        // Only while the app owns the mode: with the panel override on the stored app choice is inert, and
        // erasing it here would cost the user that choice the moment the override goes back off.
        if (PhysicalPanelOverride == 0) {
          MaintainModeUserSel = 0;  // clear the stored choice too, or switching Target Voltage back off would silently resurrect Maintain
          settingWrite(NK_MaintainMode, "0");
        }
      }
      queueConsoleMessageF("TargetVoltageMode %s", TargetVoltageMode ? "enabled" : "disabled");
    }
    if (paramOk(request, "OnOff")) {
      // OnOff==0 returned early above; only non-zero values past the arm check reach here.
      foundParameter = true;
      bool onIsRepeat = false;
      if (clientWriteTrack(request, g_onOffWriteSrc, g_onOffWriteN, &g_onOffWriteNext, &onIsRepeat)) {
        // Either a superseded press (applying it would re-energize a field the user just switched off) or the
        // hedged second copy of a press that already landed; the log must tell the two apart.
        logClientWrite(request, onIsRepeat ? "OnOff=1 IGNORED: duplicate copy of a press already applied"
                                           : "OnOff=1 IGNORED: out of order for its tab (an OFF or later ON already applied)");
      } else {
        inputMessage = request->getParam("OnOff")->value();
        settingWrite(NK_OnOff, inputMessage.c_str());
        const bool wasOff = (OnOff == 0);
        OnOff = inputMessage.toInt();
        stateRevision++;
        logClientWrite(request, "ALTERNATOR ENABLED: master switch ON");
        // Off->on restarts a pack resting in IDLE from BULK, else the switch stays dead until a voltage-sag
        // rebulk. Consumed by updateChargingStage() on the control task.
        if (wasOff && inIdleStage) restartChargeCycleRequested = true;
      }
    }
    if (paramOk(request, "HiLow")) {
      foundParameter = true;
      inputMessage = request->getParam("HiLow")->value();
      HiLowUserSel = inputMessage.toInt();  // the app's stored choice; inert while PhysicalPanelOverride is on, but still recorded so turning the override back off restores it
      settingWrite(NK_HiLow, inputMessage.c_str());
      // Applied here as well as in servicePanelSwitchInputs() so the mode is live before this request returns.
      // With the override on this resolves back to the panel's mode and only the stored choice changes.
      if (applyChargeRateMode(resolveChargeRateMode())) {
        queueConsoleMessageF("Charge rate mode: switched to %s", HiLow == 1 ? "Normal" : "Low");
      } else if (PhysicalPanelOverride) {
        queueConsoleMessageF("Charge rate choice stored (%s) - the Cable 3 switch decides while Physical Panel Override is on", HiLowUserSel == 0 ? "Low" : "Normal");
      }
    }
    if (paramOk(request, "InvertAltAmps")) {
      foundParameter = true;
      inputMessage = request->getParam("InvertAltAmps")->value();
      settingWrite(NK_InvertAltAmps, inputMessage.c_str());
      InvertAltAmps = inputMessage.toInt();
    }
    if (paramOk(request, "InvertBattAmps")) {
      foundParameter = true;
      inputMessage = request->getParam("InvertBattAmps")->value();
      settingWrite(NK_InvertBattAmps, inputMessage.c_str());
      InvertBattAmps = inputMessage.toInt();
    }
    if (paramOk(request, "ShuntLead")) {
      foundParameter = true;
      const int lead = request->getParam("ShuntLead")->value().toInt();
      if (lead == 1 || lead == 2) {
        ShuntLead = lead;
        settingWrite(NK_ShuntLead, String(lead).c_str());
        queueConsoleMessageF("Shunt location: %s", (lead == 1) ? "negative lead. Battery voltage is corrected for the shunt's drop." : "positive lead. No correction.");
      } else {
        queueConsoleMessageF("WRITE REJECTED: ShuntLead=%d - negative lead (1) or positive lead (2) only", lead);
      }
    }
    if (paramOk(request, "BatteryShuntPresent")) {
      foundParameter = true;
      inputMessage = request->getParam("BatteryShuntPresent")->value();
      settingWrite(NK_BatteryShuntPresent, inputMessage.c_str());
      BatteryShuntPresent = inputMessage.toInt();
      // "No shunt" drops only the battery-current laws: zero-current float demotes to voltage float, Maintain clears.
      // Shunt present but resistance unset is a recoverable gap: Bcur consumers gate on HAS_BATT_SHUNT, so the
      // user's Float/Maintain choice stays in NVS.
      if (!BatteryShuntPresent) {
        if (UseFloat == 2)     { UseFloat = 1;     settingWrite(NK_UseFloat, "1"); }
        if (MaintainMode != 0 || MaintainModeUserSel != 0) { MaintainMode = 0; MaintainModeUserSel = 0; settingWrite(NK_MaintainMode, "0"); }
        queueConsoleMessage("Battery shunt marked absent: State of Charge, battery health, battery current limit and zero-current float are off. Absorption ends on its time limit, then float holds the float voltage.");
        queueConsoleMessage("Load-dump detection is off too. The alternator current limit now protects the battery.");
      } else if (!HAS_BATT_SHUNT) {
        queueConsoleMessage("Battery shunt resistance is not set: State of Charge, battery health, battery current limit and zero-current float are off. Absorption ends on its time limit.");
        queueConsoleMessage("Enter the shunt resistance to enable them. Your float setting is kept.");
      }
    }
    if (paramOk(request, "MaxDuty")) {
      foundParameter = true;
      inputMessage = request->getParam("MaxDuty")->value();
      settingWrite(NK_MaxDuty, inputMessage.c_str());
      MaxDuty = inputMessage.toInt();
      if (pidInitialized) {
        applyCcOutputLimits();  // single owner of the ceiling expression; never a raw SetOutputLimits here
      }
      queueConsoleMessageF("Max Duty updated to: %d%%", (int)MaxDuty);
    }
    if (paramOk(request, "MaxFieldVolts")) {
      foundParameter = true;
      inputMessage = request->getParam("MaxFieldVolts")->value();
      MaxFieldVolts = constrain(inputMessage.toFloat(), 0.5f, 60.0f);  // same window as InitSystemSettings' boot sanity check; 60 V covers a 48 V winding
      settingWrite(NK_MaxFieldVolts, String(MaxFieldVolts, 1).c_str());
      // Re-solve now: the tick filter would take up to 2 s to move the ceiling past its 0.5-point deadband.
      updateFieldVoltCeilingNow();
      queueConsoleMessageF("Max Field Volts updated to: %.1fV (field ceiling now %.1f%%)", MaxFieldVolts, ccDutyCeiling());
    }
    // Field winding rating outside a Vessel Info save: the dashboard's answer "use a 12 V winding" to the resistance check.
    // FieldWindingAsked is the "W:R" pairing the question was asked about; the answer is refused once that has moved on.
    if (paramOk(request, "FieldWindingV")) {
      foundParameter = true;
      if (request->hasParam("FieldWindingAsked")
          && !fieldWindingAnswerOpen(request->getParam("FieldWindingAsked")->value().c_str())) {
        request->send(409, "text/plain", FIELD_WIND_ANSWER_STALE);
        return;
      }
      fieldWindingSet(request->getParam("FieldWindingV")->value().toInt(), nullptr);
      updateFieldVoltCeilingNow();
    }
    // The owner's "keep this winding" answer to the resistance check, carrying the "W:R" pairing it was asked about.
    if (request->hasParam("FieldWindingAck")) {
      foundParameter = true;
      if (!fieldWindingAnswerOpen(request->getParam("FieldWindingAck")->value().c_str())) {
        request->send(409, "text/plain", FIELD_WIND_ANSWER_STALE);
        return;
      }
      fieldWindingConfirm();
      fieldWindingReconcile("owner confirmed the winding");
      updateFieldVoltCeilingNow();
    }
    if (paramOk(request, "MinDuty")) {
      foundParameter = true;
      inputMessage = request->getParam("MinDuty")->value();
      settingWrite(NK_MinDuty, inputMessage.c_str());
      MinDuty = inputMessage.toFloat();
      if (pidInitialized) {
        applyCcOutputLimits();
      }
      queueConsoleMessageF("Min Field (tachometer keep-alive) updated to: %.2f%%", MinDuty);
    }
    if (paramOk(request, "LimpHome")) {
      foundParameter = true;
      inputMessage = request->getParam("LimpHome")->value();
      settingWrite(NK_LimpHome, inputMessage.c_str());
      LimpHome = inputMessage.toInt();
    }
    if (paramOk(request, "VeData")) {
      foundParameter = true;
      inputMessage = request->getParam("VeData")->value();
      settingWrite(NK_VeData, inputMessage.c_str());
      VeData = inputMessage.toInt();
    }
    if (paramOk(request, "NMEA0183Data")) {
      foundParameter = true;
      inputMessage = request->getParam("NMEA0183Data")->value();
      settingWrite(NK_NMEA0183Data, inputMessage.c_str());
      NMEA0183Data = inputMessage.toInt();
      if (NMEA0183Data != 1) n183ClearHeading();  // receiver stopped: nothing will ever age this value out again
    }
    if (paramOk(request, "NMEA0183Baud")) {
      foundParameter = true;
      inputMessage = request->getParam("NMEA0183Baud")->value();
      int b = inputMessage.toInt();
      if (b == 4800 || b == 9600 || b == 19200 || b == 38400) {  // reject anything the front end isn't specified for rather than bricking the port
        NMEA0183Baud = b;
        settingWrite(NK_NMEA0183Baud, String(NMEA0183Baud).c_str());
        n183ReconfigPending = true;  // Core 1 applies it; see the flag's declaration
        queueConsoleMessageF("NMEA 0183 baud set to %d", NMEA0183Baud);
      } else {
        queueConsoleMessageF("NMEA 0183 baud must be 4800/9600/19200/38400 - %d rejected, keeping %d", b, NMEA0183Baud);
      }
    }
    if (paramOk(request, "NMEA0183Invert")) {
      foundParameter = true;
      inputMessage = request->getParam("NMEA0183Invert")->value();
      NMEA0183Invert = (inputMessage.toInt() == 1) ? 1 : 0;
      settingWrite(NK_NMEA0183Invert, String(NMEA0183Invert).c_str());
      n183ReconfigPending = true;  // Core 1 applies it; see the flag's declaration
      queueConsoleMessageF("NMEA 0183 polarity set to %s", NMEA0183Invert ? "inverted (TTL talker)" : "normal (RS-232 talker)");
    }
    if (paramOk(request, "NMEA2KData")) {
      foundParameter = true;
      inputMessage = request->getParam("NMEA2KData")->value();
      settingWrite(NK_NMEA2KData, inputMessage.c_str());
      NMEA2KData = inputMessage.toInt();
    }
    if (paramOk(request, "n2kTxEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kTxEnable")->value();
      settingWrite(NK_n2kTxEn, inputMessage.c_str());
      n2kTxEnable = inputMessage.toInt();
      n2kNodeReconfigPending = true;  // Core 1 joins or leaves the bus; see the flag's declaration
    }
    if (paramOk(request, "n2kDeviceInstance")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kDeviceInstance")->value();
      n2kDeviceInstance = constrain(inputMessage.toInt(), 0, 252);
      settingWrite(NK_n2kDevInst, String(n2kDeviceInstance).c_str());
      n2kNodeReconfigPending = true;  // Core 1 applies it; a live node re-announces itself with the new instance
    }
    if (paramOk(request, "n2kBattEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kBattEnable")->value();
      settingWrite(NK_n2kBattEn, inputMessage.c_str());
      n2kBattEnable = inputMessage.toInt();
    }
    if (paramOk(request, "n2kBattInstance")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kBattInstance")->value();
      n2kBattInstance = constrain(inputMessage.toInt(), 0, 252);
      settingWrite(NK_n2kBattInst, String(n2kBattInstance).c_str());
    }
    if (paramOk(request, "n2kBattCfgEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kBattCfgEnable")->value();
      settingWrite(NK_n2kBattCfgEn, inputMessage.c_str());
      n2kBattCfgEnable = inputMessage.toInt();
    }
    if (paramOk(request, "n2kAltEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kAltEnable")->value();
      settingWrite(NK_n2kAltEn, inputMessage.c_str());
      n2kAltEnable = inputMessage.toInt();
    }
    if (paramOk(request, "n2kAltInstance")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kAltInstance")->value();
      n2kAltInstance = constrain(inputMessage.toInt(), 0, 252);
      settingWrite(NK_n2kAltInst, String(n2kAltInstance).c_str());
    }
    if (paramOk(request, "n2kAltTempEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kAltTempEnable")->value();
      settingWrite(NK_n2kAltTempEn, inputMessage.c_str());
      n2kAltTempEnable = inputMessage.toInt();
    }
    if (paramOk(request, "n2kTempInstance")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kTempInstance")->value();
      n2kTempInstance = constrain(inputMessage.toInt(), 0, 252);
      settingWrite(NK_n2kTempInst, String(n2kTempInstance).c_str());
    }
    if (paramOk(request, "n2kTempSource")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kTempSource")->value();
      n2kTempSource = constrain(inputMessage.toInt(), 0, 15);  // tN2kTempSource 4-bit field
      settingWrite(NK_n2kTempSrc, String(n2kTempSource).c_str());
    }
    if (paramOk(request, "n2kExtraTempEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kExtraTempEnable")->value();
      settingWrite(NK_n2kExtraTempEnable, inputMessage.c_str());
      n2kExtraTempEnable = inputMessage.toInt();
    }
    if (paramOk(request, "n2kExtraTempInstance")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kExtraTempInstance")->value();
      n2kExtraTempInstance = constrain(inputMessage.toInt(), 0, 252);
      settingWrite(NK_n2kExtraTempInstance, String(n2kExtraTempInstance).c_str());
    }
    if (paramOk(request, "n2kExtraTempSource")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kExtraTempSource")->value();
      n2kExtraTempSource = constrain(inputMessage.toInt(), 0, 15);  // tN2kTempSource 4-bit field
      settingWrite(NK_n2kExtraTempSource, String(n2kExtraTempSource).c_str());
    }
    if (paramOk(request, "n2kChgrEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kChgrEnable")->value();
      settingWrite(NK_n2kChgrEn, inputMessage.c_str());
      n2kChgrEnable = inputMessage.toInt();
    }
    if (paramOk(request, "n2kChgrInstance")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kChgrInstance")->value();
      n2kChgrInstance = constrain(inputMessage.toInt(), 0, 252);
      settingWrite(NK_n2kChgrInst, String(n2kChgrInstance).c_str());
    }
    if (paramOk(request, "n2kChgrCfgEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kChgrCfgEnable")->value();
      settingWrite(NK_n2kChgrCfgEn, inputMessage.c_str());
      n2kChgrCfgEnable = inputMessage.toInt();
    }
    if (paramOk(request, "n2kChgrMode")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kChgrMode")->value();
      n2kChgrMode = constrain(inputMessage.toInt(), 0, 2);  // tN2kChargerMode: Standalone/Primary/Secondary
      settingWrite(NK_n2kChgrMode, String(n2kChgrMode).c_str());
    }
    if (paramOk(request, "n2kEngRpmEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kEngRpmEnable")->value();
      settingWrite(NK_n2kEngRpmEn, inputMessage.c_str());
      n2kEngRpmEnable = inputMessage.toInt();
    }
    if (paramOk(request, "n2kEngInstance")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kEngInstance")->value();
      n2kEngInstance = constrain(inputMessage.toInt(), 0, 252);
      settingWrite(NK_n2kEngInst, String(n2kEngInstance).c_str());
    }
    if (paramOk(request, "n2kEngDynEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kEngDynEnable")->value();
      settingWrite(NK_n2kEngDynEn, inputMessage.c_str());
      n2kEngDynEnable = inputMessage.toInt();
    }
    if (paramOk(request, "n2kEngBitsEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kEngBitsEnable")->value();
      settingWrite(NK_n2kEngBitsEn, inputMessage.c_str());
      n2kEngBitsEnable = inputMessage.toInt();
    }
    if (paramOk(request, "n2kConvStatEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kConvStatEnable")->value();
      settingWrite(NK_n2kConvStatEn, inputMessage.c_str());
      n2kConvStatEnable = inputMessage.toInt();
    }
    if (paramOk(request, "n2kRxBattInstance")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kRxBattInstance")->value();
      n2kRxBattInstance = constrain(inputMessage.toInt(), 0, 252);
      settingWrite(NK_n2kRxBattInst, String(n2kRxBattInstance).c_str());
      n2kRxBattV = n2kRxBattA = n2kRxBattTempF = NAN;  // clear the old bank's values; SOC too
      n2kRxSoc = n2kRxSoh = -1;
      dataTimestamps[IDX_N2K_BATT] = 0;
      dataTimestamps[IDX_N2K_SOC] = 0;
    }
    if (paramOk(request, "rvcTxEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("rvcTxEnable")->value();
      settingWrite(NK_rvcTxEn, inputMessage.c_str());
      rvcTxEnable = inputMessage.toInt();
      n2kNodeReconfigPending = true;  // same node as NMEA2000 transmit; Core 1 joins or leaves the bus
    }
    if (paramOk(request, "rvcChgrEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("rvcChgrEnable")->value();
      settingWrite(NK_rvcChgrEn, inputMessage.c_str());
      rvcChgrEnable = inputMessage.toInt();
    }
    if (paramOk(request, "rvcDcEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("rvcDcEnable")->value();
      settingWrite(NK_rvcDcEn, inputMessage.c_str());
      rvcDcEnable = inputMessage.toInt();
    }
    if (paramOk(request, "rvcFaultEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("rvcFaultEnable")->value();
      settingWrite(NK_rvcFaultEn, inputMessage.c_str());
      rvcFaultEnable = inputMessage.toInt();
    }
    if (paramOk(request, "rvcChgrInstance")) {
      foundParameter = true;
      inputMessage = request->getParam("rvcChgrInstance")->value();
      rvcChgrInstance = constrain(inputMessage.toInt(), 1, 250);  // spec 6.20.8b: 0 is invalid
      settingWrite(NK_rvcChgrInst, String(rvcChgrInstance).c_str());
    }
    if (paramOk(request, "rvcDcInstance")) {
      foundParameter = true;
      inputMessage = request->getParam("rvcDcInstance")->value();
      rvcDcInstance = constrain(inputMessage.toInt(), 1, 250);  // spec 6.5.2b: 0 is invalid
      settingWrite(NK_rvcDcInst, String(rvcDcInstance).c_str());
    }
    if (paramOk(request, "rvcDevPriority")) {
      foundParameter = true;
      inputMessage = request->getParam("rvcDevPriority")->value();
      rvcDevPriority = constrain(inputMessage.toInt(), 0, 250);
      settingWrite(NK_rvcDevPri, String(rvcDevPriority).c_str());
    }
    if (paramOk(request, "dvccEn")) {
      foundParameter = true;
      inputMessage = request->getParam("dvccEn")->value();
      settingWrite(NK_dvccEn, inputMessage.c_str());
      int newDvccEn = inputMessage.toInt();
      if (newDvccEn == 1 && dvccEn != 1) {
        // Observe mode keeps decoding while off, so a flap latch from hours ago (e.g. a GX
        // reboot) would convert to UNTRUSTED on the first enabled tick. dvccTick consumes this
        // flag ahead of that check; set it BEFORE the enable lands so a tick can't interleave.
        dvccCfgChanged = true;
      }
      dvccEn = newDvccEn;
      if (dvccEn != 1) {
        dvccState = 0;  // immediate clamp release — don't wait for the next brain tick
        dvccCvlV = dvccCclA = NAN;
      } else {
        queueConsoleMessage("Charge-limit follow enabled: authority must settle before limits apply. Verify the shown CVL/CCL match what the sender itself displays before relying on it.");
      }
    }
    if (paramOk(request, "dvccSrcType")) {
      foundParameter = true;
      inputMessage = request->getParam("dvccSrcType")->value();
      dvccSrcType = constrain(inputMessage.toInt(), 0, 1);
      settingWrite(NK_dvccSrcType, String(dvccSrcType).c_str());
      dvccResetAuthority();
    }
    if (paramOk(request, "dvccInst")) {
      foundParameter = true;
      inputMessage = request->getParam("dvccInst")->value();
      dvccInst = constrain(inputMessage.toInt(), 0, 250);  // RV-C DC instance range; 0 = any
      settingWrite(NK_dvccInst, String(dvccInst).c_str());
      dvccResetAuthority();
    }
    if (paramOk(request, "dvccSilenceS")) {
      foundParameter = true;
      inputMessage = request->getParam("dvccSilenceS")->value();
      dvccSilenceS = constrain(inputMessage.toInt(), 5, 600);
      settingWrite(NK_dvccSilenceS, String(dvccSilenceS).c_str());
    }
    if (paramOk(request, "dvccSettleS")) {
      foundParameter = true;
      inputMessage = request->getParam("dvccSettleS")->value();
      dvccSettleS = constrain(inputMessage.toInt(), 5, 600);
      settingWrite(NK_dvccSettleS, String(dvccSettleS).c_str());
    }
    if (paramOk(request, "dvccVicCutHold")) {
      foundParameter = true;
      inputMessage = request->getParam("dvccVicCutHold")->value();
      dvccVicCutHold = constrain(inputMessage.toInt(), 0, 1);
      settingWrite(NK_dvccVicCutHold, String(dvccVicCutHold).c_str());
    }
    if (paramOk(request, "dvccLostStop")) {
      foundParameter = true;
      inputMessage = request->getParam("dvccLostStop")->value();
      dvccLostStop = constrain(inputMessage.toInt(), 0, 1);
      settingWrite(NK_dvccLostStop, String(dvccLostStop).c_str());
    }
    if (paramOk(request, "dvccCvlMin")) {
      foundParameter = true;
      inputMessage = request->getParam("dvccCvlMin")->value();
      // Constrained + cross-checked: toFloat() of garbage is 0.0, and an empty/inverted window
      // would latch UNTRUSTED against every healthy authority while blaming the BMS.
      float vMin = constrain(inputMessage.toFloat(), DVCC_CVL_ABS_MIN, DVCC_CVL_ABS_MAX);
      if (vMin < dvccCvlMax) {
        dvccCvlMin = vMin;
        settingWrite(NK_dvccCvlMin, String(dvccCvlMin, 2).c_str());
      } else {
        queueConsoleMessage("DVCC: CVL window low must be below the high bound - not applied");
      }
    }
    if (paramOk(request, "dvccCvlMax")) {
      foundParameter = true;
      inputMessage = request->getParam("dvccCvlMax")->value();
      float vMax = constrain(inputMessage.toFloat(), DVCC_CVL_ABS_MIN, DVCC_CVL_ABS_MAX);
      if (vMax > dvccCvlMin) {
        dvccCvlMax = vMax;
        settingWrite(NK_dvccCvlMax, String(dvccCvlMax, 2).c_str());
      } else {
        queueConsoleMessage("DVCC: CVL window high must be above the low bound - not applied");
      }
    }
    if (request->hasParam("DvccResetTrust")) {
      foundParameter = true;
      dvccResetReq = true;  // consumed by dvccTick(); momentary action, no NVS
    }
    if (paramOk(request, "waveAmplitude")) {
      foundParameter = true;
      inputMessage = request->getParam("waveAmplitude")->value();
      settingWrite(NK_waveAmplitude, inputMessage.c_str());
      waveAmplitude = inputMessage.toInt();
      if (TuningMode) tuningParamChanged = true;
    }
    if (paramOk(request, "tuningWaveFloor")) {
      foundParameter = true;
      inputMessage = request->getParam("tuningWaveFloor")->value();
      settingWrite(NK_tuningWaveFloor, inputMessage.c_str());
      tuningWaveFloor = inputMessage.toInt();
      if (TuningMode) tuningParamChanged = true;
    }
    if (paramOk(request, "CurrentThreshold")) {
      foundParameter = true;
      inputMessage = request->getParam("CurrentThreshold")->value();
      settingWrite(NK_CurrentThreshold, inputMessage.c_str());
      CurrentThreshold = inputMessage.toFloat();
    }
    if (paramOk(request, "PeukertExponent")) {
      foundParameter = true;
      inputMessage = request->getParam("PeukertExponent")->value();
      PeukertExponent_scaled = (int)lroundf(inputMessage.toFloat() * 100.0f);  // round, not truncate: float32 1.05 x 100 = 104.99999 and (int) stored 1.04
      settingWrite(NK_PeukertExponent, String(PeukertExponent_scaled).c_str());
    }
    if (paramOk(request, "ChargeEfficiency")) {
      foundParameter = true;
      inputMessage = request->getParam("ChargeEfficiency")->value();
      settingWrite(NK_ChargeEfficiency, inputMessage.c_str());
      ChargeEfficiency_scaled = (int)round(inputMessage.toFloat() * 10);  // store as % × 10
    }
    if (paramOk(request, "ChargedVoltage")) {
      foundParameter = true;
      inputMessage = request->getParam("ChargedVoltage")->value();
      ChargedVoltage_Scaled = (int)lroundf(inputMessage.toFloat() * 100.0f);  // round, not truncate: float32 14.2 x 100 = 1419.99998 and (int) stored 14.19
      settingWrite(NK_ChargedVoltage, String(ChargedVoltage_Scaled).c_str());
    }
    if (paramOk(request, "TailCurrent")) {
      foundParameter = true;
      inputMessage = request->getParam("TailCurrent")->value();
      settingWrite(NK_TailCurrent, inputMessage.c_str());
      TailCurrent = inputMessage.toFloat();
    }
    if (paramOk(request, "ChargedDetectionTime")) {
      foundParameter = true;
      inputMessage = request->getParam("ChargedDetectionTime")->value();
      settingWrite(NK_ChargedDetectionTime, inputMessage.c_str());
      ChargedDetectionTime = inputMessage.toInt();
    }
    if (paramOk(request, "IgnoreTemperature")) {
      foundParameter = true;
      inputMessage = request->getParam("IgnoreTemperature")->value();
      settingWrite(NK_IgnoreTemperature, inputMessage.c_str());
      IgnoreTemperature = inputMessage.toInt();
    }
    if (paramOk(request, "IgnoreRPM")) {
      foundParameter = true;
      inputMessage = request->getParam("IgnoreRPM")->value();
      settingWrite(NK_IgnoreRPM, inputMessage.c_str());
      IgnoreRPM = inputMessage.toInt();
    }
    if (paramOk(request, "IgnoreAltCurrent")) {
      foundParameter = true;
      IgnoreAltCurrent = request->getParam("IgnoreAltCurrent")->value().toInt() ? 1 : 0;
      settingWrite(NK_IgnoreAltCurrent, String(IgnoreAltCurrent).c_str());
    }
    if (paramOk(request, "IgnoreBattVMain")) {
      foundParameter = true;
      IgnoreBattVMain = request->getParam("IgnoreBattVMain")->value().toInt() ? 1 : 0;
      settingWrite(NK_IgnoreBattVMain, String(IgnoreBattVMain).c_str());
    }
    if (paramOk(request, "IgnoreBattV2")) {
      foundParameter = true;
      IgnoreBattV2 = request->getParam("IgnoreBattV2")->value().toInt() ? 1 : 0;
      settingWrite(NK_IgnoreBattV2, String(IgnoreBattV2).c_str());
    }
    if (paramOk(request, "LimpDutyPct")) {
      foundParameter = true;
      LimpDutyPct = constrain(request->getParam("LimpDutyPct")->value().toFloat(), 0.0f, 99.0f);
      settingWrite(NK_LimpDutyPct, String(LimpDutyPct, 1).c_str());
    }
    if (paramOk(request, "NoRpmCurrentA")) {
      foundParameter = true;
      NoRpmCurrentA = fmaxf(0.0f, request->getParam("NoRpmCurrentA")->value().toFloat());
      settingWrite(NK_NoRpmCurrentA, String(NoRpmCurrentA, 1).c_str());
    }
    if (paramOk(request, "MinRPMForField")) {
      foundParameter = true;
      inputMessage = request->getParam("MinRPMForField")->value();
      MinRPMForField = inputMessage.toInt();
      if (MinRPMForField < 0) MinRPMForField = 0;
      settingWrite(NK_MinRPMForField, String(MinRPMForField).c_str());
    }
    if (paramOk(request, "bmsLogic")) {
      foundParameter = true;
      inputMessage = request->getParam("bmsLogic")->value();
      settingWrite(NK_bmsLogic, inputMessage.c_str());
      bmsLogic = inputMessage.toInt();
    }
    if (paramOk(request, "bmsLogicLevelOff")) {
      foundParameter = true;
      inputMessage = request->getParam("bmsLogicLevelOff")->value();
      settingWrite(NK_bmsLogicLevelOff, inputMessage.c_str());
      bmsLogicLevelOff = inputMessage.toInt();
    }
    if (paramOk(request, "AlarmActivate")) {
      foundParameter = true;
      inputMessage = request->getParam("AlarmActivate")->value();
      settingWrite(NK_AlarmActivate, inputMessage.c_str());
      AlarmActivate = inputMessage.toInt();
    }
    if (paramOk(request, "TempAlarm")) {
      foundParameter = true;
      inputMessage = request->getParam("TempAlarm")->value();
      settingWrite(NK_TempAlarm, inputMessage.c_str());
      TempAlarm = inputMessage.toInt();
    }
    if (paramOk(request, "TempAlarmLow")) {
      foundParameter = true;
      inputMessage = request->getParam("TempAlarmLow")->value();
      settingWrite(NK_TempAlarmLow, inputMessage.c_str());
      TempAlarmLow = inputMessage.toInt();
    }
    if (paramOk(request, "VoltageAlarmHigh")) {
      foundParameter = true;
      inputMessage = request->getParam("VoltageAlarmHigh")->value();
      settingWrite(NK_VoltageAlarmHigh, inputMessage.c_str());
      VoltageAlarmHigh = inputMessage.toFloat();
    }
    if (paramOk(request, "VoltageAlarmLow")) {
      foundParameter = true;
      inputMessage = request->getParam("VoltageAlarmLow")->value();
      settingWrite(NK_VoltageAlarmLow, inputMessage.c_str());
      VoltageAlarmLow = inputMessage.toFloat();
    }
    if (paramOk(request, "SocAlarmLow")) {
      foundParameter = true;
      inputMessage = request->getParam("SocAlarmLow")->value();
      SocAlarmLow = constrain(inputMessage.toInt(), 0, 100);
      settingWrite(NK_SocAlarmLow, String(SocAlarmLow).c_str());
    }
    if (paramOk(request, "CurrentAlarmHigh")) {
      foundParameter = true;
      inputMessage = request->getParam("CurrentAlarmHigh")->value();
      settingWrite(NK_CurrentAlarmHigh, inputMessage.c_str());
      CurrentAlarmHigh = inputMessage.toInt();
    }
    if (paramOk(request, "RPMScalingFactor")) {
      foundParameter = true;
      inputMessage = request->getParam("RPMScalingFactor")->value();
      int newScale = inputMessage.toInt();
      // Linear gain on the engine-RPM axis, so any real change invalidates every RPM-indexed
      // artifact. Refuse while the field is driving rather than wipe learning out from under it.
      bool axisMoved = (newScale > 0 && newScale != RPMScalingFactor);
      if (axisMoved && fieldActiveStatus > 0) {
        request->send(409, "text/plain", "Turn the alternator off before changing RPM scaling");
        return;
      }
      if (newScale > 0) {
        if (axisMoved) {
          // Persist wipe-owed (local + cloud) BEFORE the scale itself: a reboot between the writes
          // re-runs the wipe at boot instead of keeping old-axis learning under the new scale.
          settingWrite(NK_RpmAxisWipeLoc, "1");
          settingWrite(NK_RpmAxisWipePend, "1");
          rpmAxisWipePending = true;
        }
        settingWrite(NK_RPMScalingFactor, inputMessage.c_str());
        RPMScalingFactor = newScale;
      }
      if (axisMoved) pendingRpmAxisWipe = true;  // Core 1 runs the wipe
    }
    if (paramOk(request, "FieldResistance")) {
      foundParameter = true;
      inputMessage = request->getParam("FieldResistance")->value();
      const float r = inputMessage.toFloat();
      // The temperature it was measured at comes with it, typed by the owner: a resistance from another day stamped with
      // the probe now would carry the wrong reference into the coil-temperature credit.
      const float tC = paramOk(request, "FieldResTrefC") ? request->getParam("FieldResTrefC")->value().toFloat() : NAN;
      if (!(r >= 0.05f && r <= 100.0f)) {   // a measured value or nothing: no clamping a typo into a resistance the ceilings act on
        queueConsoleMessageF("WRITE REJECTED: FieldResistance=%s - enter the measured resistance, 0.05 to 100 ohm", inputMessage.c_str());
      } else if (!(tC >= -40.0f && tC <= 150.0f)) {   // the range boot accepts for a stored pair
        queueConsoleMessageF("WRITE REJECTED: FieldResistance=%s - enter the alternator temperature it was measured at with it", inputMessage.c_str());
      } else {
        fieldResRecord(r, tC, true);
      }
    }
    if (paramOk(request, "FieldWiring")) {
      foundParameter = true;
      const int w = request->getParam("FieldWiring")->value().toInt();
      if (w == 1 || w == 2) {
        FieldWiring = w;
        settingWrite(NK_FieldWiring, String(w).c_str());
        g_fieldDerateDirty = true;
        queueConsoleMessageF("Field wiring set to %s: the drive-heat limit protects the %s", (w == 1) ? "P-type" : "N-type",
                             (w == 1) ? "freewheel diode's heat spreader" : "field connector");
      } else {
        queueConsoleMessageF("WRITE REJECTED: FieldWiring=%d - P-type (1) or N-type (2) only", w);
      }
    }
    if (paramOk(request, "AlternatorCOffset")) {
      foundParameter = true;
      inputMessage = request->getParam("AlternatorCOffset")->value();
      settingWrite(NK_AlternatorCOffset, inputMessage.c_str());
      const float offBefore = AlternatorCOffset;
      AlternatorCOffset = inputMessage.toFloat();
      if (AutoAltCurrentZero != 1) zeroCorrFollowOffset(offBefore - AlternatorCOffset);   // typed against the uncorrected reading
    }
    // Alternator-zero capture, both momentary: AltZeroCaptureNow requests one measurement (control tick holds
    // the field at 0%); altZeroApply folds the held capture into AlternatorCOffset. A new request discards it.
    if (request->hasParam("AltZeroCaptureNow")) {
      foundParameter = true;
      altZeroCapHeld    = 0;
      altZeroRejReason  = ALTZERO_RJ_NONE;
      AltZeroCaptureNow = 1;
      queueConsoleMessage("Alt zero: requested - the field will be held at 0% for the measurement");
    }
    if (request->hasParam("altZeroApply")) {
      foundParameter = true;
      if (!altZeroApply()) queueConsoleMessage("Alt zero: nothing captured to apply");
    }
    if (paramOk(request, "BatteryCOffset")) {
      foundParameter = true;
      inputMessage = request->getParam("BatteryCOffset")->value();
      settingWrite(NK_BatteryCOffset, inputMessage.c_str());
      BatteryCOffset = inputMessage.toFloat();
    }
    if (paramOk(request, "AmpSensorRange")) {
      foundParameter = true;
      inputMessage = request->getParam("AmpSensorRange")->value();
      settingWrite(NK_AmpSensorRange, inputMessage.c_str());
      AmpSensorRange = inputMessage.toInt();
      queueConsoleMessageF("AmpSensorRange changed to: %d", AmpSensorRange);
    }
    if (paramOk(request, "R_fixed")) {
      foundParameter = true;
      inputMessage = request->getParam("R_fixed")->value();
      settingWrite(NK_R_fixed, inputMessage.c_str());
      R_fixed = inputMessage.toFloat();
    }
    if (paramOk(request, "Beta")) {
      foundParameter = true;
      inputMessage = request->getParam("Beta")->value();
      settingWrite(NK_Beta, inputMessage.c_str());
      Beta = inputMessage.toFloat();
    }
    if (paramOk(request, "T0_C")) {
      foundParameter = true;
      inputMessage = request->getParam("T0_C")->value();
      settingWrite(NK_T0_C, inputMessage.c_str());
      T0_C = inputMessage.toFloat();
    }
    if (paramOk(request, "TempSource")) {
      foundParameter = true;
      inputMessage = request->getParam("TempSource")->value();
      settingWrite(NK_TempSource, inputMessage.c_str());
      TempSource = inputMessage.toInt();
    }
    if (paramOk(request, "IgnitionOverride")) {
      foundParameter = true;
      inputMessage = request->getParam("IgnitionOverride")->value();
      settingWrite(NK_IgnitionOverride, inputMessage.c_str());
      IgnitionOverride = inputMessage.toInt();
    }
    if (paramOk(request, "AlarmLatchEnabled")) {
      foundParameter = true;
      inputMessage = request->getParam("AlarmLatchEnabled")->value();
      settingWrite(NK_AlarmLatchEnabled, inputMessage.c_str());
      AlarmLatchEnabled = inputMessage.toInt();
    }
    if (paramOk(request, "GeneralFaultAlarm")) {
      foundParameter = true;
      inputMessage = request->getParam("GeneralFaultAlarm")->value();
      GeneralFaultAlarm = inputMessage.toInt() ? 1 : 0;
      settingWrite(NK_GeneralFaultAlarm, String(GeneralFaultAlarm).c_str());
    }
    if (request->hasParam("AlarmTest")) {
      foundParameter = true;
      AlarmTest = 1;  // Set the flag - don't save to file as it's momentary
      queueConsoleMessage("ALARM TEST: Initiated from web interface");
      inputMessage = "1";
    }
    if (request->hasParam("ResetAlarmLatch")) {
      foundParameter = true;
      ResetAlarmLatch = 1;  // Set the flag - don't save to file as it's momentary
      queueConsoleMessage("ALARM LATCH: Reset requested from web interface");
      inputMessage = "1";
    }
    if (request->hasParam("ZeroIMU")) {
      foundParameter = true;
      if (!imuEnabled) {
        queueConsoleMessage("IMU ZERO: rejected — IMU not enabled");
      } else if (imuZeroInProgress) {
        queueConsoleMessage("IMU ZERO: already in progress");
      } else {
        // Adopt the form's selected mount orientation (RAM only — persisted by the Vessel Info
        // save), so calibrating BEFORE the first save still captures offsets in the right frame.
        if (paramOk(request, "imuOrient")) {
          int o = request->getParam("imuOrient")->value().toInt();
          if (o >= 0 && o < IMU_ORIENT_COUNT) imuMountOrientation = (uint8_t)o;
        }
        // Start a fresh capture window; offsets are written when N samples collected
        imuZeroAxSum = imuZeroAySum = imuZeroAzSum = 0;
        imuZeroGxSum = imuZeroGySum = imuZeroGzSum = 0;
        imuZeroAccelN = imuZeroGyroN = 0;
        imuZeroInProgress = true;
        imuZeroStartMs = millis();   // timeout reference so the capture always resolves
        queueConsoleMessage("IMU ZERO: hold still ~2s, capturing level reference");
      }
      inputMessage = "1";
    }
    if (request->hasParam("ClearIMUZero")) {
      foundParameter = true;
      imuHeelOffsetDeg = imuPitchOffsetDeg = 0;
      imuGxBias = imuGyBias = imuGzBias = 0;
      settingRemove(NK_imu_zero);
      imuZeroCaptured = false;   // the mirror this flag promises to be — else uploads keep claiming the
                                 // heel/pitch data is trustworthy with no level reference behind it
      imuMountState = IMU_MOUNT_UNKNOWN;   // the verdict lived in the capture that just went away
      settingRemove(NK_imu_mnt_state);
      queueConsoleMessage("IMU ZERO: calibration cleared");
      inputMessage = "1";
    }

    if (request->hasParam("RebootRegulator")) {
      foundParameter = true;
      rebootRequestedAt = millis();
      rebootRequested = true;          // deferred — loop() does the actual restart after response flushes
      queueConsoleMessage("REBOOT: requested from /get endpoint");
      inputMessage = "1";
    }
    if (paramOk(request, "absorptionCompleteTime")) {
      foundParameter = true;
      inputMessage = request->getParam("absorptionCompleteTime")->value();
      uint32_t seconds = (uint32_t)inputMessage.toInt();
      absorptionCompleteTime = seconds * 1000UL;
      settingWrite(NK_absorptionCompleteTime, String(absorptionCompleteTime).c_str());
    }
    if (paramOk(request, "FLOAT_DURATION")) {
      foundParameter = true;
      inputMessage = request->getParam("FLOAT_DURATION")->value();
      float hours = inputMessage.toFloat();
      int seconds = (int)(hours * 3600.0f);  // fractional hours preserved
      FLOAT_DURATION = seconds;
      settingWrite(NK_FLOAT_DURATION, String(seconds).c_str());
    }
    if (paramOk(request, "UseFloat")) {
      foundParameter = true;
      inputMessage = request->getParam("UseFloat")->value();
      UseFloat = constrain(inputMessage.toInt(), 0, 2);  // 0=idle, 1=voltage float, 2=zero-current float
      // Zero-current float is a battery-current law, so with the shunt declared absent it is stored as voltage
      // float (EFFECTIVE_USE_FLOAT does the same at runtime for a shunt present but unset).
      if (!BatteryShuntPresent && UseFloat == 2) UseFloat = 1;
      settingWrite(NK_UseFloat, String(UseFloat).c_str());
    }
    if (paramOk(request, "RebulkCurrent_A")) {
      foundParameter = true;
      inputMessage = request->getParam("RebulkCurrent_A")->value();
      RebulkCurrent_A = inputMessage.toFloat();
      settingWrite(NK_RebulkCurrent_A, String(RebulkCurrent_A).c_str());
    }
    if (paramOk(request, "VMGTargetBearing")) {
      foundParameter = true;
      inputMessage = request->getParam("VMGTargetBearing")->value();
      VMGTargetBearing = constrain(inputMessage.toFloat(), -1.0f, 359.0f);  // -1 = not set
      settingWrite(NK_VMGTargetBearing, String(VMGTargetBearing).c_str());
    }
    // No VMGUseTrueWind param — both VMGs (manual + upwind) are always computed.
    if (paramOk(request, "timeSourceMode")) {
      foundParameter = true;
      inputMessage = request->getParam("timeSourceMode")->value();
      uint8_t m = (uint8_t)inputMessage.toInt();
      if (m > TSRC_NTP) m = TSRC_AUTO;  // sanity
      timeSourceMode = m;
      settingWrite(NK_timeSourceMode, String(timeSourceMode).c_str());
      const char *lbl = (m == TSRC_AUTO)  ? "auto"
                      : (m == TSRC_NMEA)  ? "NMEA only"
                      : (m == TSRC_PHONE) ? "this device only"
                                         : "NTP time only";
      queueConsoleMessageF("Time source set to %s", lbl);
    }
    if (paramOk(request, "gpsPositionSource")) {
      foundParameter = true;
      inputMessage = request->getParam("gpsPositionSource")->value();
      uint8_t m = (uint8_t)inputMessage.toInt();
      if (m > GPS_SRC_PHONE) m = GPS_SRC_AUTO;  // sanity
      gpsPositionSource = m;
      settingWrite(NK_gpsPositionSource, String(gpsPositionSource).c_str());
      queueConsoleMessageF("Position source set to %s",
                           (m == GPS_SRC_AUTO)  ? "auto"
                         : (m == GPS_SRC_NMEA)  ? "NMEA only"
                                                : "phone only");
    }
    if (paramOk(request, "speedSourceMode")) {
      foundParameter = true;
      inputMessage = request->getParam("speedSourceMode")->value();
      uint8_t m = (uint8_t)inputMessage.toInt();
      if (m > SPD_SRC_PHONE) m = SPD_SRC_NMEA;  // sanity
      speedSourceMode = m;
      settingWrite(NK_speedSourceMode, String(speedSourceMode).c_str());
      queueConsoleMessageF("Speed/course source set to %s",
                           (m == SPD_SRC_PHONE) ? "phone GPS" : "NMEA 2000");
    }
    if (request->hasParam("ResetThermTemp")) {
      foundParameter = true;
      MaxTemperatureThermistor = 0;
      nvsPersistNow = true;
      queueConsoleMessage("Max Thermistor Temp: Reset requested from web interface");
    }
    if (request->hasParam("ResetTemp")) {
      foundParameter = true;
      MaxAlternatorTemperatureF = 0;
      nvsPersistNow = true;
      queueConsoleMessage("Max Alternator Temp: Reset requested from web interface");
    }
    if (request->hasParam("ResetVoltage")) {
      foundParameter = true;
      IBVMax = 0;
      nvsPersistNow = true;
      queueConsoleMessage("Max Voltage: Reset requested from web interface");
    }
    if (request->hasParam("ResetCurrent")) {
      foundParameter = true;
      MeasuredAmpsMax = 0;
      nvsPersistNow = true;
      queueConsoleMessage("Max Battery Current: Reset requested from web interface");
    }
    if (request->hasParam("ResetEngineRunTime")) {
      foundParameter = true;
      EngineRunTime = 0;
      nvsPersistNow = true;
      queueConsoleMessage("Engine Run Time: Reset requested from web interface");
    }
    if (request->hasParam("ResetAlternatorOnTime")) {
      foundParameter = true;
      AlternatorOnTime = 0;
      nvsPersistNow = true;
      queueConsoleMessage("Alternator On Time: Reset requested from web interface");
    }
    if (request->hasParam("ResetFieldHeatLimTime")) {
      foundParameter = true;
      g_fieldHeatLimSesS = 0;
      nvsPersistNow = true;
      queueConsoleMessage("Field Held by Drive Heat: session reset requested from web interface");
    }
    if (request->hasParam("ResetFieldCurLimTime")) {
      foundParameter = true;
      g_fieldCurLimSesS = 0;
      nvsPersistNow = true;
      queueConsoleMessage("Field Held at 10 A Limit: session reset requested from web interface");
    }
    if (request->hasParam("ResetEnergy")) {
      foundParameter = true;
      ChargedEnergy = 0;
      nvsPersistNow = true;
      queueConsoleMessage("Battery Charged Energy: Reset requested from web interface");
    }
    if (request->hasParam("RestartChargeCycle")) {
      foundParameter = true;
      restartChargeCycleRequested = true;   // consumed by updateChargingStage() on the control-loop task
      queueConsoleMessage("Restart charge cycle: requested from web interface");
    }
    if (request->hasParam("ResetDischargedEnergy")) {
      foundParameter = true;
      DischargedEnergy = 0;
      nvsPersistNow = true;
      queueConsoleMessage("Battery Discharged Energy: Reset requested from web interface");
    }
    if (request->hasParam("ResetFuelUsed")) {
      foundParameter = true;
      AlternatorFuelUsed = 0;
      nvsPersistNow = true;
      queueConsoleMessage("Fuel Used: Reset requested from web interface");
    }
    if (request->hasParam("ResetAlternatorChargedEnergy")) {
      foundParameter = true;
      AlternatorChargedEnergy = 0;
      nvsPersistNow = true;
      queueConsoleMessage("Alternator Charged Energy: Reset requested from web interface");
    }
    if (request->hasParam("ResetEngineCycles")) {
      foundParameter = true;
      EngineCycles = 0;
      nvsPersistNow = true;
      queueConsoleMessage("Engine Cycles: Reset requested from web interface");
    }
    if (request->hasParam("ResetRPMMax")) {
      foundParameter = true;
      RPMMax = 0;
      nvsPersistNow = true;
      queueConsoleMessage("Max Engine Speed: Reset requested from web interface");
    }
    if (request->hasParam("ResetSolarEnergy")) {
      foundParameter = true;
      SolarChargedEnergy = 0;
      nvsPersistNow = true;
      queueConsoleMessage("Solar Energy: Reset requested from web interface");
    }
    if (request->hasParam("ResetChargeCycles")) {
      foundParameter = true;
      ChargeCycleEnergy = 0;   // ChargeCycles is recomputed from this every SoC pass
      ChargeCycles = 0;
      nvsPersistNow = true;
      queueConsoleMessage("Charge Cycles: Reset requested from web interface");
    }
    if (request->hasParam("ResetMinVoltage")) {
      foundParameter = true;
      MinVoltage = 999.0;
      nvsPersistNow = true;
      queueConsoleMessage("Min Voltage: Reset requested from web interface");
    }
    if (request->hasParam("ResetTotalDistance")) {
      foundParameter = true;
      TotalDistance = 0;
      nvsPersistNow = true;
      queueConsoleMessage("Total Distance: Reset requested from web interface");
    }
    if (request->hasParam("ResetAvgSpeed")) {
      foundParameter = true;
      AvgSpeed = 0;
      avgSpeedResetRequested = true;
      nvsPersistNow = true;
      queueConsoleMessage("Average Speed: Reset requested from web interface");
    }
    if (request->hasParam("ResetMaxSpeed")) {
      foundParameter = true;
      MaxSpeed = 0;
      nvsPersistNow = true;
      queueConsoleMessage("Sustained Speed: Reset requested from web interface");
    }
    // Lifetime nav/sailing record resets. prev_* shadows are deliberately left alone so saveNVSDataFull()
    // sees the change and persists the cleared value.
    if (request->hasParam("ResetLongestTrip")) {
      foundParameter = true;
      LongestSingleTrip_Nm_AllTime = 0.0f;   // in-progress trip (currentTripDistanceNm) intentionally preserved
      nvsPersistNow = true;
      queueConsoleMessage("Longest Single Trip: Reset requested from web interface");
    }
    if (request->hasParam("ResetMax24hrDist")) {
      foundParameter = true;
      // clear watermark + the 24-bucket ring, else the pre-reset window keeps the old peak alive
      Max24hrDistance_AllTime = 0.0f;
      for (uint8_t i = 0; i < 24; i++) distHourBuckets[i] = 0.0f;
      distHourHead = 0;
      distHourStartMs = millis();
      nvsPersistNow = true;
      queueConsoleMessage("Max 24-Hour Distance: Reset requested from web interface");
    }
    if (request->hasParam("ResetDeepestAnchor")) {
      foundParameter = true;
      // clear watermark + anchorage ring (same rationale as the 24h window)
      DeepestAnchorage_Ft_AllTime = 0.0f;
      if (anchorageRing) memset(anchorageRing, 0, ANCHORAGE_RING_SIZE * sizeof(AnchorageSample));
      anchorageRingHead = 0;
      anchorageRingCount = 0;
      lastAnchorageSampleMs = 0;
      nvsPersistNow = true;
      queueConsoleMessage("Deepest Anchorage: Reset requested from web interface");
    }
    if (request->hasParam("ResetBestUpwindVMG")) {
      foundParameter = true;
      best_upwind_vmg_alltime = 0.0f;
      nvsPersistNow = true;
      queueConsoleMessage("Best Upwind VMG: Reset requested from web interface");
    }
    if (request->hasParam("ResetLongestGale")) {
      foundParameter = true;
      // clear the watermark AND the in-progress gale run so it can't keep extending post-reset
      longest_gale_duration_hours_alltime = 0.0f;
      galeActive = false;
      currentGaleMinutes = 0;
      nvsPersistNow = true;
      queueConsoleMessage("Longest Gale Duration: Reset requested from web interface");
    }
    if (request->hasParam("ResetEngineFuelUsed")) {
      foundParameter = true;
      EngineFuelUsed = 0;
      nvsPersistNow = true;
      queueConsoleMessage("Engine Fuel Used: Reset requested from web interface");
    }
    if (request->hasParam("ResetFuelCurve")) {
      foundParameter = true;
      memset(fuelCurveNMPG, 0, sizeof(fuelCurveNMPG));  // clear all bins
      fcRun = false;                                    // break any in-progress steady run
      queueConsoleMessage("Fuel Economy Curve: Reset requested from web interface");
    }
    if (paramOk(request, "MaximumAllowedBatteryAmps")) {
      foundParameter = true;
      inputMessage = request->getParam("MaximumAllowedBatteryAmps")->value();
      settingWrite(NK_MaximumAllowedBatteryAmps, inputMessage.c_str());
      MaximumAllowedBatteryAmps = inputMessage.toInt();
    }
    if (paramOk(request, "LoadDumpDtThresh")) {
      foundParameter = true;
      inputMessage = request->getParam("LoadDumpDtThresh")->value();
      LoadDumpDtThresh = inputMessage.toFloat();
      settingWrite(NK_LoadDumpDtThresh, String(LoadDumpDtThresh).c_str());
    }
    if (paramOk(request, "LoadDumpDtThresh1")) {
      foundParameter = true;
      inputMessage = request->getParam("LoadDumpDtThresh1")->value();
      LoadDumpDtThresh1 = inputMessage.toFloat();
      settingWrite(NK_LoadDumpDtThresh1, String(LoadDumpDtThresh1).c_str());
    }
    if (paramOk(request, "LoadDumpDtThresh3")) {
      foundParameter = true;
      inputMessage = request->getParam("LoadDumpDtThresh3")->value();
      LoadDumpDtThresh3 = inputMessage.toFloat();
      settingWrite(NK_LoadDumpDtThresh3, String(LoadDumpDtThresh3).c_str());
    }
    if (paramOk(request, "LoadDumpN1")) {
      foundParameter = true;
      inputMessage = request->getParam("LoadDumpN1")->value();
      LoadDumpN1 = constrain(inputMessage.toInt(), 1, 10);
      settingWrite(NK_LoadDumpN1, String(LoadDumpN1).c_str());
    }
    if (paramOk(request, "LoadDumpN2")) {
      foundParameter = true;
      inputMessage = request->getParam("LoadDumpN2")->value();
      LoadDumpN2 = constrain(inputMessage.toInt(), 1, 10);
      settingWrite(NK_LoadDumpN2, String(LoadDumpN2).c_str());
    }
    if (paramOk(request, "LoadDumpN3")) {
      foundParameter = true;
      inputMessage = request->getParam("LoadDumpN3")->value();
      LoadDumpN3 = constrain(inputMessage.toInt(), 1, 10);
      settingWrite(NK_LoadDumpN3, String(LoadDumpN3).c_str());
    }
    if (request->hasParam("SocSeedAck")) {
      foundParameter = true;
      inputMessage = request->getParam("SocSeedAck")->value();
      settingWrite(NK_SocSeedAck, inputMessage.c_str());
    }
    if (paramOk(request, "ManualSOCPoint")) {
      foundParameter = true;
      inputMessage = request->getParam("ManualSOCPoint")->value();
      settingWrite(NK_ManualSOCPoint, inputMessage.c_str());
      ManualSOCPoint = inputMessage.toFloat();
      SOC_percent = (int)roundf(ManualSOCPoint * 100.0f);   // SOC_percent is percent x100; round so decimals seed exactly
      CoulombCount_Ah_scaled = (ManualSOCPoint * BatteryCapacity_Ah);
      nvsPersistNow = true;  // NVS otherwise saves only at the field-off edge; a reboot before then would revert the seed
      queueConsoleMessageF("SoC manually set to: %.2f%%", ManualSOCPoint);
    }
    if (paramOk(request, "BatteryCapacity_Ah")) {
      foundParameter = true;
      inputMessage = request->getParam("BatteryCapacity_Ah")->value();
      BatteryCapacity_Ah = constrain(inputMessage.toInt(), 1L, 100000L);  // reject 0 Ah — it divides-by-zero into SOC
      settingWrite(NK_BatteryCapacity_Ah, String(BatteryCapacity_Ah).c_str());
      PeukertRatedCurrent_A = BatteryCapacity_Ah / 20.0f;
      queueConsoleMessageF("Battery capacity set to: %d Ah", BatteryCapacity_Ah);
    }
    if (paramOk(request, "ShuntResistanceMicroOhm")) {
      foundParameter = true;
      inputMessage = request->getParam("ShuntResistanceMicroOhm")->value();
      long r = inputMessage.toInt();   // a blank number input submits as "" → 0, which disables every Bcur feature
      // Reject, never clamp: Bcur = ShuntVoltage_mV·1000/R, so a 1 µΩ floor would read 1000× high — far worse
      // than the zero it was guarding against. foundParameter still fires, so CSV3 echoes the kept value back.
      if (r < 1 || r > 5000) {
        queueConsoleMessageF("Shunt resistance must be 1-5000 microohms - rejected, keeping %d", ShuntResistanceMicroOhm);
      } else {
        ShuntResistanceMicroOhm = (int)r;
        settingWrite(NK_ShuntResistanceMicroOhm, String(ShuntResistanceMicroOhm).c_str());
      }
    }

    if (paramOk(request, "VoltageKp")) {
      foundParameter = true;
      inputMessage = request->getParam("VoltageKp")->value();
      settingWrite(NK_VoltageKp, inputMessage.c_str());
      VoltageKp = inputMessage.toFloat();
      recomputeCvGains();  // manual gain changed → refresh the active gain
      if (CVTuningMode) cvTuningParamChanged = true;
    }

    if (paramOk(request, "maxPoints")) {
      foundParameter = true;
      inputMessage = request->getParam("maxPoints")->value();
      settingWrite(NK_maxPoints, inputMessage.c_str());
      maxPoints = inputMessage.toInt();
    }
    if (paramOk(request, "Ymin1")) {
      foundParameter = true;
      inputMessage = request->getParam("Ymin1")->value();
      settingWrite(NK_Ymin1, inputMessage.c_str());
      Ymin1 = inputMessage.toInt();
    }
    if (paramOk(request, "Ymax1")) {
      foundParameter = true;
      inputMessage = request->getParam("Ymax1")->value();
      settingWrite(NK_Ymax1, inputMessage.c_str());
      Ymax1 = inputMessage.toInt();
    }
    if (paramOk(request, "Ymin2")) {
      foundParameter = true;
      inputMessage = request->getParam("Ymin2")->value();
      settingWrite(NK_Ymin2, inputMessage.c_str());
      Ymin2 = inputMessage.toFloat();
    }
    if (paramOk(request, "Ymax2")) {
      foundParameter = true;
      inputMessage = request->getParam("Ymax2")->value();
      settingWrite(NK_Ymax2, inputMessage.c_str());
      Ymax2 = inputMessage.toFloat();
    }
    if (paramOk(request, "Ymin3")) {
      foundParameter = true;
      inputMessage = request->getParam("Ymin3")->value();
      settingWrite(NK_Ymin3, inputMessage.c_str());
      Ymin3 = inputMessage.toInt();
    }
    if (paramOk(request, "Ymax3")) {
      foundParameter = true;
      inputMessage = request->getParam("Ymax3")->value();
      settingWrite(NK_Ymax3, inputMessage.c_str());
      Ymax3 = inputMessage.toInt();
    }
    if (paramOk(request, "Ymin4")) {
      foundParameter = true;
      inputMessage = request->getParam("Ymin4")->value();
      settingWrite(NK_Ymin4, inputMessage.c_str());
      Ymin4 = inputMessage.toInt();
    }
    if (paramOk(request, "Ymax4")) {
      foundParameter = true;
      inputMessage = request->getParam("Ymax4")->value();
      settingWrite(NK_Ymax4, inputMessage.c_str());
      Ymax4 = inputMessage.toInt();
    }
    if (paramOk(request, "AutoAltCurrentZero")) {
      foundParameter = true;
      inputMessage = request->getParam("AutoAltCurrentZero")->value();
      settingWrite(NK_AutoAltCurrentZero, inputMessage.c_str());
      AutoAltCurrentZero = inputMessage.toInt();
    }
    if (request->hasParam("ResetDynamicAltZero")) {
      foundParameter = true;
      inputMessage = request->getParam("ResetDynamicAltZero")->value();
      // the Auto Zero form also carries this field at 0 (older web apps send it on every Auto Zero toggle)
      if (inputMessage.toInt() == 1) {
        ResetDynamicAltZero = 1;  // Set the flag - don't save to file as it's momentary
        queueConsoleMessage("Alt Zero: Reset requested from web interface");
      }
    }
    if (paramOk(request, "WindingTempOffset")) {
      foundParameter = true;
      inputMessage = request->getParam("WindingTempOffset")->value();
      settingWrite(NK_WindingTempOffset, inputMessage.c_str());
      WindingTempOffset = inputMessage.toFloat();
    }
    if (paramOk(request, "displayTempUnit")) {
      foundParameter = true;
      inputMessage = request->getParam("displayTempUnit")->value();
      settingWrite(NK_displayTempUnit, inputMessage.c_str());
      displayTempUnit = (uint8_t)inputMessage.toInt();
    }
    if (paramOk(request, "displayVolUnit")) {
      foundParameter = true;
      inputMessage = request->getParam("displayVolUnit")->value();
      settingWrite(NK_displayVolUnit, inputMessage.c_str());
      displayVolUnit = (uint8_t)inputMessage.toInt();
    }
    if (paramOk(request, "PulleyRatio")) {
      foundParameter = true;
      inputMessage = request->getParam("PulleyRatio")->value();
      settingWrite(NK_PulleyRatio, inputMessage.c_str());
      PulleyRatio = inputMessage.toFloat();
    }
    if (paramOk(request, "ManualLifePercentage")) {
      foundParameter = true;
      inputMessage = request->getParam("ManualLifePercentage")->value();
      settingWrite(NK_ManualLifePercentage, inputMessage.c_str());
      ManualLifePercentage = inputMessage.toInt();
      float life_fraction = ManualLifePercentage / 100.00;
      CumulativeInsulationDamage = 1.0 - life_fraction;
      CumulativeGreaseDamage = 1.0 - life_fraction;
      CumulativeBrushDamage = 1.0 - life_fraction;
      nvsPersistNow = true;   // commit the damage values now, not at the next field-off edge
      queueConsoleMessageF("Alternator life manually set to %d%%", ManualLifePercentage);
    }
    if (paramOk(request, "webgaugesinterval")) {
      foundParameter = true;
      inputMessage = request->getParam("webgaugesinterval")->value();
      int newInterval = inputMessage.toInt();
      newInterval = constrain(newInterval, 1, 10000000);
      settingWrite(NK_webgaugesinterval, String(newInterval).c_str());
      webgaugesinterval = newInterval;
      queueConsoleMessageF("Update interval set to: %dms", newInterval);
    }
    if (paramOk(request, "battMaxMode")) {
      foundParameter = true;
      inputMessage = request->getParam("battMaxMode")->value();
      battMaxMode = (inputMessage.toInt() != 0);
      settingWrite(NK_battMaxMode, battMaxMode ? "1" : "0");
      queueConsoleMessageF("Battery V/I plot sampling: %s", battMaxMode ? "Max" : "Average");
    }
    if (paramOk(request, "BattMonSrc")) {
      foundParameter = true;
      const int src = request->getParam("BattMonSrc")->value().toInt();
      if (src == 0 || src == 1 || src == 3) {
        BattMonSrc = src;
        settingWrite(NK_BattMonSrc, String(BattMonSrc).c_str());
        queueConsoleMessageF("Battery monitor: %s", src == 3 ? "VE.Direct" : src == 1 ? "NMEA 2000" : "none");
      } else {
        queueConsoleMessageF("WRITE REJECTED: BattMonSrc=%d - 0 (none), 1 (NMEA 2000) or 3 (VE.Direct) only", src);
      }
    }
    if (paramOk(request, "BatteryVoltageSource")) {
      foundParameter = true;
      inputMessage = request->getParam("BatteryVoltageSource")->value();
      int vsrc = inputMessage.toInt();
      if (vsrc != 1 && vsrc != 3) vsrc = 0;   // 2 (NMEA 0183) has no received battery voltage to read
      BatteryVoltageSource = vsrc;
      settingWrite(NK_BatteryVoltageSource, String(BatteryVoltageSource).c_str());
      queueConsoleMessageF("Battery voltage source for charge decisions: %s",
                           BatteryVoltageSource == 1 ? "NMEA 2000" : BatteryVoltageSource == 3 ? "Victron VE.Direct" : "onboard INA228");
    }
    if (paramOk(request, "totalPowerCycles")) {
      foundParameter = true;
      inputMessage = request->getParam("totalPowerCycles")->value();
      settingWrite(NK_totalPowerCycles, inputMessage.c_str());
      totalPowerCycles = inputMessage.toInt();
      nvsPersistNow = true;   // also commit the storage-namespace "PowerCycles" (what loadNVSData reads at boot) now, not at the field-off edge.
    }
    if (paramOk(request, "timeAxisModeChanging")) {
      foundParameter = true;
      inputMessage = request->getParam("timeAxisModeChanging")->value();
      settingWrite(NK_timeAxisModeChanging, inputMessage.c_str());
      timeAxisModeChanging = inputMessage.toInt();
      queueConsoleMessageF("Time axis mode changed to: %s", timeAxisModeChanging ? "UNIX timestamps" : "relative time");
    }
    if (paramOk(request, "plotTimeWindow")) {
      foundParameter = true;
      inputMessage = request->getParam("plotTimeWindow")->value();
      settingWrite(NK_plotTimeWindow, inputMessage.c_str());
      plotTimeWindow = inputMessage.toInt();
    }
    if (paramOk(request, "LatitudeNMEA") && paramOk(request, "LongitudeNMEA")) {
      foundParameter = true;
      // Sticky manual override: beats boat NMEA and phone GPS until clearGpsManual; resolveSources()
      // reasserts it every tick.
      LatitudeManual  = request->getParam("LatitudeNMEA")->value().toDouble();
      LongitudeManual = request->getParam("LongitudeNMEA")->value().toDouble();
      gpsManualActive = true;
      LatitudeNMEA  = LatitudeManual;
      LongitudeNMEA = LongitudeManual;
      settingWrite(NK_LatitudeManual, String(LatitudeManual, 6).c_str());
      settingWrite(NK_LongitudeManual, String(LongitudeManual, 6).c_str());
      settingWrite(NK_gpsManualActive, "1");
      queueConsoleMessageF("GPS: Manual override set to %.6f, %.6f (sticky — beats NMEA & phone)", LatitudeManual, LongitudeManual);
      nextWeatherUpdate = millis();  // fire next tick (signed-delta safe; '= 0' would miss past 24.8d uptime)
    }
    if (request->hasParam("clearGpsManual")) {
      foundParameter = true;
      gpsManualActive = false;
      settingWrite(NK_gpsManualActive, "0");
      queueConsoleMessage("GPS: Manual override cleared — back to automatic (NMEA/phone)");
      nextWeatherUpdate = millis();  // refresh weather with whatever the auto chain now resolves
    }
    if (paramOk(request, "weatherModeEnabled")) {
      foundParameter = true;
      inputMessage = request->getParam("weatherModeEnabled")->value();
      settingWrite(NK_weatherModeEnabled, inputMessage.c_str());
      weatherModeEnabled = inputMessage.toInt();
      queueConsoleMessageF("Weather Mode %s", weatherModeEnabled ? "enabled" : "disabled");
    }
    if (paramOk(request, "UVThresholdHigh")) {
      foundParameter = true;
      inputMessage = request->getParam("UVThresholdHigh")->value();
      settingWrite(NK_UVThresholdHigh, inputMessage.c_str());
      UVThresholdHigh = inputMessage.toFloat();
    }
    if (paramOk(request, "SolarWatts")) {
      foundParameter = true;
      inputMessage = request->getParam("SolarWatts")->value();
      settingWrite(NK_SolarWatts, inputMessage.c_str());
      SolarWatts = inputMessage.toInt();
      weatherRecomputePredicted();
    }
    if (paramOk(request, "performanceRatio")) {
      foundParameter = true;
      inputMessage = request->getParam("performanceRatio")->value();
      settingWrite(NK_performanceRatio, inputMessage.c_str());
      performanceRatio = inputMessage.toFloat();
      sledRatioDirty = false;   // the typed value is now the stored value — a pending learned write must not overwrite it
      weatherRecomputePredicted();
    }
    if (paramOk(request, "solarLearnEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("solarLearnEnable")->value();
      settingWrite(NK_solarLearnEnable, inputMessage.c_str());
      solarLearnEnable = inputMessage.toInt();
    }
    if (paramOk(request, "solarUseConsEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("solarUseConsEnable")->value();
      settingWrite(NK_solarUseConsEnable, inputMessage.c_str());
      solarUseConsEnable = inputMessage.toInt();
    }
    if (paramOk(request, "solarConsMarginPct")) {
      foundParameter = true;
      inputMessage = request->getParam("solarConsMarginPct")->value();
      settingWrite(NK_solarConsMarginPct, inputMessage.c_str());
      solarConsMarginPct = inputMessage.toFloat();
    }
    if (paramOk(request, "solarLearnRatePct")) {
      foundParameter = true;
      inputMessage = request->getParam("solarLearnRatePct")->value();
      settingWrite(NK_solarLearnRatePct, inputMessage.c_str());
      solarLearnRatePct = inputMessage.toFloat();
    }
    if (request->hasParam("ResetSolarLedger")) {
      foundParameter = true;
      solarLedgerClear();
      queueConsoleMessage("Solar ledger cleared - history and today's baselines reset");
    }
    if (request->hasParam("TriggerWeatherUpdate")) {
      foundParameter = true;
      // Every rejection names its reason; a silent fall-through reads as "nothing happened". Not gated on
      // field state: the scheduled fetch waits for fieldCutSettled, but a button press asks for it now.
      if (LatitudeNMEA == 0.0 && LongitudeNMEA == 0.0) {
        queueConsoleMessage("Weather update failed: no GPS position yet — wait for a fix or set a manual lat/lon");
        inputMessage = "no_gps";
      } else if (WiFi.RSSI() < -80) {
        queueConsoleMessage("Weather update failed: WiFi signal too weak");
        inputMessage = "weak_wifi";
      } else {
        HttpsRequest req = { .type = HTTPS_FETCH_WEATHER };
        if (xQueueSend(httpsQueue, &req, 0) == pdTRUE) {
          queueConsoleMessage("Weather: Manual update triggered");
        } else {
          queueConsoleMessage("Weather: HTTPS queue full, try again in a moment");
        }
      }
    }
    if (request->hasParam("ResetLearningTable")) {
      foundParameter = true;
      resetLearningTableToDefaults();
      kneeLearnResetDefaults();   // the learner's floors would otherwise rebuild the old column at the next boot
      commissionClearStage(7);    // the commissioned Keep-Alive column is gone
      queueConsoleMessage("Learning table reset from web - refresh to see defaults");
      inputMessage = "1";
    }
    if (request->hasParam("ClearOverheatHistory")) {
      foundParameter = true;
      pendingClearOverheatHistory = true;  // deferred to Core 1 to avoid SSE gap
      inputMessage = "1";
    }
    if (paramOk(request, "CAPSIZE_THRESHOLD_DEG")) {
      foundParameter = true;
      inputMessage = request->getParam("CAPSIZE_THRESHOLD_DEG")->value();
      settingWrite(NK_CAPSIZE_THRESHOLD_DEG, inputMessage.c_str());
      CAPSIZE_THRESHOLD_DEG = inputMessage.toFloat();
    }
    if (paramOk(request, "PITCHPOLE_THRESHOLD_DEG")) {
      foundParameter = true;
      inputMessage = request->getParam("PITCHPOLE_THRESHOLD_DEG")->value();
      settingWrite(NK_PITCHPOLE_THRESHOLD_DEG, inputMessage.c_str());
      PITCHPOLE_THRESHOLD_DEG = inputMessage.toFloat();
    }
    if (paramOk(request, "SLAM_THRESHOLD_G")) {
      foundParameter = true;
      inputMessage = request->getParam("SLAM_THRESHOLD_G")->value();
      settingWrite(NK_SLAM_THRESHOLD_G, inputMessage.c_str());
      SLAM_THRESHOLD_G = inputMessage.toFloat();
    }
    if (paramOk(request, "IgnoreLearningDuringPenalty")) {
      foundParameter = true;
      inputMessage = request->getParam("IgnoreLearningDuringPenalty")->value();
      settingWrite(NK_IgnoreLearningDuringPenalty, inputMessage.c_str());
      IgnoreLearningDuringPenalty = inputMessage.toInt();
    }
    if (paramOk(request, "CloudFeatures")) {
      foundParameter = true;
      inputMessage = request->getParam("CloudFeatures")->value();
      settingWrite(NK_CloudFeatures, inputMessage.c_str());
      CloudFeatures = inputMessage.toInt();
    }
    if (paramOk(request, "RemoteDiagnostics")) {
      foundParameter = true;
      inputMessage = request->getParam("RemoteDiagnostics")->value();
      settingWrite(NK_RemoteDiagnostics, inputMessage.c_str());
      RemoteDiagnostics = inputMessage.toInt();
    }
    if (paramOk(request, "testProtectionsEnabled")) {
      foundParameter = true;
      inputMessage = request->getParam("testProtectionsEnabled")->value();
      testProtectionsEnabled = (inputMessage.toInt() != 0);
      // Not persisted: resets to enabled on every boot by design. The CSV3 echo drives the dashboard banner
      // and the per-page toggles.
      if (testProtectionsEnabled) {
        queueConsoleMessage("PROTECTIONS ENABLED — all protection layers restored");
      } else {
        queueConsoleMessage("PROTECTIONS DISABLED for tuning — G1/G2 over-voltage, timed OV cut tiers + G3 iExcess over-current bypassed; fast-OV hard-cut (AlternatorHardShutdownV), G4 load-dump/battery-current limit, INA228, and hardware OC remain active");
      }
    }
    if (paramOk(request, "TuningMode")) {
      foundParameter = true;
      inputMessage = request->getParam("TuningMode")->value();
      int requested = inputMessage.toInt();
      // Mutex: refuse turn-on if another test is already running. All four tests must run independently.
      if (requested == 1 && TuningMode == 0) {
        const char *blocker = (systemIDActive != 0) ? "Plant Delay Test"
                              : (altSweepActive != 0 || altSweepRequested) ? "Gate-tuning field sweep"
                         : (chcActive != 0) ? "Charge health calibration"
                                                      : (CVTuningMode ? "Voltage tuning" : nullptr);
        if (blocker != nullptr) {
          queueConsoleMessageF("Current tuning: turn-on blocked — %s is active. Turn it off first.", blocker);
        } else {
          settingWrite(NK_TuningMode, inputMessage.c_str());
          TuningMode = requested;
        }
      } else {
        settingWrite(NK_TuningMode, inputMessage.c_str());
        TuningMode = requested;
      }
    }
    if (paramOk(request, "tuningSquareAbrupt")) {
      foundParameter = true;   // transient (NOT persisted): CV plant fit asks for abrupt square edges
      tuningSquareAbrupt = (request->getParam("tuningSquareAbrupt")->value().toInt() != 0);
    }
    if (request->hasParam("ccTuningNote")) {
      foundParameter = true;   // transient (NOT persisted): free-text label for the next committed CC record
      sanitizeTuningNote(ccTuningNote, request->getParam("ccTuningNote")->value().c_str());
    }
    if (request->hasParam("commitTuningScore")) {
      foundParameter = true;
      manualCommitTuningRequested = true;
      queueConsoleMessage("TuningScore: manual commit requested via UI");
    }
    // ── Battery Health (DCIR active test) ──
    if (request->hasParam("BatteryHealthTest")) {
      foundParameter = true;
      if (!bhStartTest()) queueConsoleMessageF("BATT HEALTH: cannot start — %s", bhAbortReason);
    }
    if (request->hasParam("bhAbort")) {
      foundParameter = true;   // wizard Abort — only meaningful mid-run; the loop resumes normal slew-limited AUTO control
      if (bhTestState == 1) bhAbort("cancelled by user");
    }
    if (paramOk(request, "bhStepLowA")) {
      foundParameter = true;
      bhStepLowA = request->getParam("bhStepLowA")->value().toFloat();
      settingWrite(NK_bhStepLowA, String(bhStepLowA, 1).c_str());
    }
    if (paramOk(request, "bhStepDeltaA")) {
      foundParameter = true;
      bhStepDeltaA = request->getParam("bhStepDeltaA")->value().toFloat();
      if (bhStepDeltaA < 5.0f) bhStepDeltaA = 5.0f;
      settingWrite(NK_bhStepDeltaA, String(bhStepDeltaA, 1).c_str());
      if (bhDwellMs < bhMinDwellMs()) {   // bigger step lengthens the slew traverse — stretch dwell to match
        bhDwellMs = bhMinDwellMs();
        settingWrite(NK_bhDwellMs, String(bhDwellMs).c_str());
      }
    }
    if (paramOk(request, "bhDwellMs")) {   // stored unit — the CONFIG_MANIFEST key, so the fleet
      foundParameter = true;                // snapshot names what it holds. bhDwellSec is the UI alias.
      bhDwellMs = (uint32_t)request->getParam("bhDwellMs")->value().toInt();
      if (bhDwellMs < bhMinDwellMs()) bhDwellMs = bhMinDwellMs();
      settingWrite(NK_bhDwellMs, String(bhDwellMs).c_str());
    }
    if (paramOk(request, "bhDwellSec")) {
      foundParameter = true;   // UI is seconds, internal is ms
      bhDwellMs = (uint32_t)(request->getParam("bhDwellSec")->value().toFloat() * 1000.0f);
      if (bhDwellMs < bhMinDwellMs()) bhDwellMs = bhMinDwellMs();
      settingWrite(NK_bhDwellMs, String(bhDwellMs).c_str());
    }
    if (paramOk(request, "bhNumEdges")) {
      foundParameter = true;
      int e = request->getParam("bhNumEdges")->value().toInt();
      if (e < 3) e = 3;
      if (e > BH_MAX_TOGGLES - 3) e = BH_MAX_TOGGLES - 3;
      bhNumEdges = (uint8_t)e;
      settingWrite(NK_bhNumEdges, String(bhNumEdges).c_str());
    }
    if (request->hasParam("bhStartOver")) {
      foundParameter = true;   // battery replaced: re-baseline + wipe history
      // bhAbort is the only path that clears batteryHealthTestActive
      if (bhTestState == 1) bhAbort("history reset (battery replaced) during the test");
      bhBaselineCapacityAh = 0.0f;
      bhResultCount = 0; bhResultHead = 0; bhResultsDirty = false;
      bhCapCount = 0; bhCapHead = 0; bhCapDirty = false;
      bhTestState = 0;
      capLowAnchorValid = false; capLastPct = NAN; capLastUpdateEpoch = 0;  // reset the capacity anchor/measurement state
      fsRemove(BHRES_PATH);
      fsRemove(BHCAP_PATH);
      settingRemove(NK_bhResults);  // legacy NVS copies — keep the boot migration from resurrecting them
      settingRemove(NK_bhCapBlob);
      settingWrite(NK_bhBaseline, "0");
      queueConsoleMessage("BATT HEALTH: history cleared, baseline reset (battery replaced)");
    }
    if (request->hasParam("bhClearHistory")) {
      foundParameter = true;   // wipe ONLY the resistance-test table; capacity baseline + trend untouched
      bhResultCount = 0; bhResultHead = 0; bhResultsDirty = false;
      bhLastResultDcir = 0.0f;
      fsRemove(BHRES_PATH);
      settingRemove(NK_bhResults);
      queueConsoleMessage("BATT HEALTH: resistance-test history cleared");
    }
    // ── Capacity tracker config + OCV table ──
    if (paramOk(request, "capRestFrac"))   { foundParameter = true; capRestCurrentFrac = request->getParam("capRestFrac")->value().toFloat();   settingWrite(NK_capRestFrac, String(capRestCurrentFrac, 4).c_str()); }
    if (paramOk(request, "capRestFloor"))  { foundParameter = true; capRestFloorMin    = (uint16_t)request->getParam("capRestFloor")->value().toInt(); settingWrite(NK_capRestFloor, String(capRestFloorMin).c_str()); }
    if (paramOk(request, "capSettleRate")) { foundParameter = true; capSettleRateMv10  = request->getParam("capSettleRate")->value().toFloat(); settingWrite(NK_capSettleRate, String(capSettleRateMv10, 2).c_str()); }
    if (paramOk(request, "capSocLowMax"))  { foundParameter = true; capSocLowMax       = request->getParam("capSocLowMax")->value().toFloat();  settingWrite(NK_capSocLowMax, String(capSocLowMax, 1).c_str()); }
    if (paramOk(request, "capMinSpan"))    { foundParameter = true; capMinSpan         = request->getParam("capMinSpan")->value().toFloat();    settingWrite(NK_capMinSpan, String(capMinSpan, 1).c_str()); }
    if (paramOk(request, "capFullSoc"))    { foundParameter = true; capFullSoc         = request->getParam("capFullSoc")->value().toFloat();    settingWrite(NK_capFullSoc, String(capFullSoc, 1).c_str()); }
    if (paramOk(request, "capRefMode"))    { foundParameter = true; capRefMode         = (uint8_t)request->getParam("capRefMode")->value().toInt(); settingWrite(NK_capRefMode, String(capRefMode).c_str()); }
    if (paramOk(request, "capTempNorm"))   { foundParameter = true; capTempNormEnable  = (uint8_t)request->getParam("capTempNorm")->value().toInt(); settingWrite(NK_capTempNorm, String(capTempNormEnable).c_str()); }
    if (paramOk(request, "capTempCoeff"))  { foundParameter = true; capTempCoeffPctC   = request->getParam("capTempCoeff")->value().toFloat();  settingWrite(NK_capTempCoeff, String(capTempCoeffPctC, 3).c_str()); }
    if (paramOk(request, "capTempRef"))    { foundParameter = true; capTempRefC        = request->getParam("capTempRef")->value().toFloat();   settingWrite(NK_capTempRef, String(capTempRefC, 1).c_str()); }
    if (request->hasParam("capOcv")) {
      foundParameter = true;   // full OCV table as a comma-joined string of CAP_OCV_ROWS rested voltages
      String body = request->getParam("capOcv")->value();
      capDeserializeOcv(body);
      settingWrite(NK_capOcvBlob, body.c_str());
    }
    if (request->hasParam("capClearPoints")) {
      foundParameter = true;   // wipe ONLY the capacity trend + baseline; resistance history untouched
      bhBaselineCapacityAh = 0.0f;
      bhCapCount = 0; bhCapHead = 0; bhCapDirty = false;
      capLowAnchorValid = false; capLastPct = NAN; capLastUpdateEpoch = 0;
      fsRemove(BHCAP_PATH);
      settingRemove(NK_bhCapBlob);
      settingWrite(NK_bhBaseline, "0");
      queueConsoleMessage("BATT CAP: capacity trend + baseline cleared");
    }
    if (paramOk(request, "LogAllLearningEvents")) {
      foundParameter = true;
      inputMessage = request->getParam("LogAllLearningEvents")->value();
      settingWrite(NK_LogAllLearningEvents, inputMessage.c_str());
      LogAllLearningEvents = inputMessage.toInt();
    }
    if (paramOk(request, "hardwarePresent")) {   // demo mode switch: RAM only, entry and exit run on the loop (9_demo.ino)
      foundParameter = true;
      const char *why = demoSwitchRequest(request->getParam("hardwarePresent")->value().toInt() != 0 ? 1 : 0);
      if (why) {
        queueConsoleMessage(why);
        request->send(503, "text/plain", why);
        return;
      }
    }

    // RPM breakpoint and current table handlers
    bool tableChangedThisRequest = false;
    int prevRPMPoints[RPM_TABLE_SIZE];
    memcpy(prevRPMPoints, rpmTableRPMPoints, sizeof(prevRPMPoints));
    const bool capBothReq = paramOk(request, "capBothSave");   // its handler below owns this request's rpmTableRPMPoints*
    for (int i = 0; i < 10; i++) {
      char paramName[32];

      snprintf(paramName, sizeof(paramName), "rpmTableRPMPoints%d", i);
      if (!capBothReq && paramOk(request, paramName)) {
        foundParameter = true;
        rpmTableRPMPoints[i] = request->getParam(paramName)->value().toInt();
        tableChangedThisRequest = true;
      }

      snprintf(paramName, sizeof(paramName), "rpmCapCurrentTable%d", i);
      if (paramOk(request, paramName)) {
        foundParameter = true;
        rpmCapCurrentTable[i] = request->getParam(paramName)->value().toFloat();
        tableChangedThisRequest = true;
      }

      snprintf(paramName, sizeof(paramName), "rpmCapKW%d", i);
      if (paramOk(request, paramName)) {
        foundParameter = true;
        rpmCapPowerTable[i] = request->getParam(paramName)->value().toFloat() * 1000.0f;
        tableChangedThisRequest = true;
      }

      snprintf(paramName, sizeof(paramName), "rpmMinDutyTable%d", i);
      if (paramOk(request, paramName)) {
        foundParameter = true;
        rpmMinDutyTable[i] = request->getParam(paramName)->value().toFloat();
        tableChangedThisRequest = true;
      }
    }

    if (tableChangedThisRequest) {
      learningTableUpdated = true;  // ← update the global so buildConfigPayload() sees it
      rpmBreakpointsMoved(prevRPMPoints);
      tableSaveAnnounce = true;
      pendingSaveUserTableEdits = true;  // deferred to Core 1 to avoid SSE gap
    }

    // Pre-commissioning "Charge Rate Limits" screen: seeds BOTH Low and High cap tables in one request,
    // independent of the live HiLow mode. Both blobs are written on Core 1.
    if (paramOk(request, "capBothSave")) {
      foundParameter = true;
      char pn[32];
      float hiA[RPM_TABLE_SIZE], loA[RPM_TABLE_SIZE], hiW[RPM_TABLE_SIZE], loW[RPM_TABLE_SIZE];
      for (int i = 0; i < RPM_TABLE_SIZE; i++) {
        hiA[i] = rpmCapCurrentTable[i]; loA[i] = rpmCapCurrentTable[i];
        hiW[i] = rpmCapPowerTable[i];   loW[i] = rpmCapPowerTable[i];
        snprintf(pn, sizeof(pn), "rpmCapHi%d", i);    if (paramOk(request, pn)) hiA[i] = request->getParam(pn)->value().toFloat();
        snprintf(pn, sizeof(pn), "rpmCapLo%d", i);    if (paramOk(request, pn)) loA[i] = request->getParam(pn)->value().toFloat();
        snprintf(pn, sizeof(pn), "rpmCapPwrHi%d", i); if (paramOk(request, pn)) hiW[i] = request->getParam(pn)->value().toFloat() * 1000.0f;
        snprintf(pn, sizeof(pn), "rpmCapPwrLo%d", i); if (paramOk(request, pn)) loW[i] = request->getParam(pn)->value().toFloat() * 1000.0f;

        snprintf(pn, sizeof(pn), "rpmTableRPMPoints%d", i);
        if (paramOk(request, pn)) rpmTableRPMPoints[i] = request->getParam(pn)->value().toInt();
        // Refresh only the ACTIVE mode's live ceiling (so a running alternator honors the edit now);
        // the mode switch itself rides a separate HiLow request so its ceiling glide runs.
        rpmCapCurrentTable[i] = (HiLow == 1) ? hiA[i] : loA[i];
        rpmCapPowerTable[i]   = (HiLow == 1) ? hiW[i] : loW[i];
      }
      rpmBreakpointsMoved(prevRPMPoints);
      capBothStageBegin(hiA, hiW, loA, loW);   // the loop writes one blob per pass and announces "saved"; /capSaveState reports it
      learningTableUpdated = true;
      stateRevision++;
    }

    // Fuel table handlers
    bool fuelTableUpdated = false;
    for (int i = 0; i < 10; i++) {
      char paramName[32];

      snprintf(paramName, sizeof(paramName), "fuelTableRPM%d", i);
      if (paramOk(request, paramName)) {
        foundParameter = true;
        float value = request->getParam(paramName)->value().toFloat();
        fuelTableRPM[i] = value;
        fuelTableUpdated = true;
      }

      snprintf(paramName, sizeof(paramName), "fuelTableGPH%d", i);
      if (paramOk(request, paramName)) {
        foundParameter = true;
        float value = request->getParam(paramName)->value().toFloat();
        fuelTableGPH[i] = value;
        fuelTableUpdated = true;
      }
    }

    if (fuelTableUpdated) {
      saveFuelTableToNVS();
      memset(fuelCurveNMPG, 0, sizeof(fuelCurveNMPG));  // GPH map + bin scale changed -> old mpg stale
      fcRun = false;
      queueConsoleMessage("Fuel table updated from web interface");
    }


    if (paramOk(request, "AlternatorNominalAmps")) {
      foundParameter = true;
      inputMessage = request->getParam("AlternatorNominalAmps")->value();
      settingWrite(NK_AlternatorNominalAmps, inputMessage.c_str());
      AlternatorNominalAmps = inputMessage.toInt();
    }
    if (paramOk(request, "LearningUpStep")) {
      foundParameter = true;
      inputMessage = request->getParam("LearningUpStep")->value();
      settingWrite(NK_LearningUpStep, inputMessage.c_str());
      LearningUpStep = inputMessage.toFloat();
    }
    if (paramOk(request, "LearningDownStep")) {
      foundParameter = true;
      inputMessage = request->getParam("LearningDownStep")->value();
      settingWrite(NK_LearningDownStep, inputMessage.c_str());
      LearningDownStep = inputMessage.toFloat();
    }
    if (paramOk(request, "xTime")) {
      foundParameter = true;
      inputMessage = request->getParam("xTime")->value();
      settingWrite(NK_xTime, inputMessage.c_str());
      xTime = inputMessage.toFloat();
    }
    if (paramOk(request, "MinLearningInterval")) {
      foundParameter = true;
      inputMessage = request->getParam("MinLearningInterval")->value();
      int temp = inputMessage.toInt() * 1000;  // from seconds (entry into html) to ms
      settingWrite(NK_MinLearningInterval, String(temp).c_str());
      MinLearningInterval = temp;
    }
    if (paramOk(request, "SetpointRiseRate")) {
      foundParameter = true;
      inputMessage = request->getParam("SetpointRiseRate")->value();
      settingWrite(NK_SetpointRiseRate, inputMessage.c_str());
      SetpointRiseRate = inputMessage.toFloat();
      if (TuningMode) tuningParamChanged = true;
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    if (paramOk(request, "SetpointBigStepThresh")) {
      foundParameter = true;
      inputMessage = request->getParam("SetpointBigStepThresh")->value();
      settingWrite(NK_SetpointBigStepThresh, inputMessage.c_str());
      SetpointBigStepThresh = inputMessage.toFloat();
      if (TuningMode) tuningParamChanged = true;
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    if (paramOk(request, "SetpointBigStepRiseRate")) {
      foundParameter = true;
      inputMessage = request->getParam("SetpointBigStepRiseRate")->value();
      settingWrite(NK_SetpointBigStepRiseRate, inputMessage.c_str());
      SetpointBigStepRiseRate = inputMessage.toFloat();
      if (TuningMode) tuningParamChanged = true;
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    if (paramOk(request, "SetpointFallRate")) {
      foundParameter = true;
      inputMessage = request->getParam("SetpointFallRate")->value();
      settingWrite(NK_SetpointFallRate, inputMessage.c_str());
      SetpointFallRate = inputMessage.toFloat();
      if (TuningMode) tuningParamChanged = true;
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    if (paramOk(request, "CvBrakeFallRate")) {
      foundParameter = true;
      inputMessage = request->getParam("CvBrakeFallRate")->value();
      CvBrakeFallRate = fmaxf(SetpointFallRate, inputMessage.toFloat());  // a brake rate below the normal fall rate is meaningless
      settingWrite(NK_CvBrakeFallRate, String(CvBrakeFallRate, 2).c_str());
      queueConsoleMessageF("CV D-term brake fall rate: %.1f A/s", CvBrakeFallRate);
      if (TuningMode) tuningParamChanged = true;
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    if (paramOk(request, "StartupRiseRate")) {
      foundParameter = true;
      inputMessage = request->getParam("StartupRiseRate")->value();
      settingWrite(NK_StartupRiseRate, inputMessage.c_str());
      StartupRiseRate = inputMessage.toFloat();
      queueConsoleMessageF("Startup rise rate set to: %.2f A/sec", StartupRiseRate);
    }
    if (paramOk(request, "PIDTrackingGain")) {
      foundParameter = true;
      inputMessage = request->getParam("PIDTrackingGain")->value();
      float temp = inputMessage.toFloat();
      settingWrite(NK_PIDTrackingGain, String(temp).c_str());
      PIDTrackingGain = temp;
      if (pidInitialized) currentPID.SetTrackingGain(PIDTrackingGain);  // else a live edit waits for the next enter_sys_auto()
      if (TuningMode) tuningParamChanged = true;
    }
    if (paramOk(request, "SafeOperationThreshold")) {
      foundParameter = true;
      inputMessage = request->getParam("SafeOperationThreshold")->value();
      int temp = inputMessage.toInt() * 1000;  // from seconds (entry into html) to ms
      settingWrite(NK_SafeOperationThreshold, String(temp).c_str());
      SafeOperationThreshold = temp;
    }
    if (paramOk(request, "PidKp")) {
      foundParameter = true;
      inputMessage = request->getParam("PidKp")->value();
      settingWrite(NK_PidKp, inputMessage.c_str());
      PidKp = inputMessage.toFloat();
      if (pidInitialized) {
        recomputeCcGains();  // apply voltage-normalized PidK*_active
      }
      if (TuningMode) tuningParamChanged = true;
      queueConsoleMessageF("PID Kp updated to: %.6f", PidKp);
    }
    if (paramOk(request, "AbsorptionVoltage")) {
      foundParameter = true;
      inputMessage = request->getParam("AbsorptionVoltage")->value();
      AbsorptionVoltage = inputMessage.toFloat();
      settingWrite(NK_AbsorptionVoltage, inputMessage.c_str());
    }
    if (paramOk(request, "TargetVoltageSetpoint")) {
      foundParameter = true;
      inputMessage = request->getParam("TargetVoltageSetpoint")->value();
      TargetVoltageSetpoint = inputMessage.toFloat();
      settingWrite(NK_TargetVoltageSetpoint, inputMessage.c_str());
    }

    if (paramOk(request, "AbsorptionTimeoutMs")) {
      foundParameter = true;
      inputMessage = request->getParam("AbsorptionTimeoutMs")->value();
      uint32_t minutes = (uint32_t)inputMessage.toInt();
      AbsorptionTimeoutMs = minutes * 60000UL;
      settingWrite(NK_AbsorptionTimeoutMs, String(AbsorptionTimeoutMs).c_str());
    }
    if (paramOk(request, "bulkVoltageHoldMs")) {
      foundParameter = true;
      inputMessage = request->getParam("bulkVoltageHoldMs")->value();
      float seconds = inputMessage.toFloat();
      bulkVoltageHoldMs = (uint32_t)(seconds * 1000.0f);
      settingWrite(NK_bulkVoltageHoldMs, String(bulkVoltageHoldMs).c_str());
    }
    if (paramOk(request, "VoltageKi")) {
      foundParameter = true;
      inputMessage = request->getParam("VoltageKi")->value();
      VoltageKi = inputMessage.toFloat();
      settingWrite(NK_VoltageKi, String(VoltageKi).c_str());
      recomputeCvGains();  // manual gain changed → refresh the active gain
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    // ── CV gain-mode system: mode toggle, λ multiplier, and (bench) hand-entered plant ──
    if (request->hasParam("cvGainMode")) {
      foundParameter = true;
      inputMessage = request->getParam("cvGainMode")->value();
      // Strict "0"/"1" only: toInt("") decodes as 0 = Manual, silently swapping in the typed fallback gains.
      // Client IP makes any flip attributable.
      if (inputMessage == "0" || inputMessage == "1") {
        cvGainMode = (uint8_t)(inputMessage == "1" ? 1 : 0);
        settingWrite(NK_cvGainMode, String((int)cvGainMode).c_str());
        recomputeCvGains();
        queueConsoleMessageF("CV Gain Source: %s (set by %s)", cvGainMode ? "Measured (lambda-based)" : "Manual",
                             request->client()->remoteIP().toString().c_str());
      } else {
        queueConsoleMessageF("CV Gain Source: malformed value '%s' from %s IGNORED - gain source unchanged",
                             inputMessage.c_str(), request->client()->remoteIP().toString().c_str());
      }
    }
    if (paramOk(request, "cvAlpha")) {  // CV aggressiveness α (Measured gain source) — Kp = α / measured stiffness K
      foundParameter = true;
      cvAlpha = clamp_f(request->getParam("cvAlpha")->value().toFloat(), 0.02f, 0.50f);
      settingWrite(NK_cvAlpha, String(cvAlpha, 3).c_str());
      recomputeCvGains();
      queueConsoleMessageF("CV aggressiveness alpha: %.2f", cvAlpha);
    }
    // Legacy — inert under the Measured gain source (ohmic-anchor rule); kept so a bench curl / stale page can't 404.
    if (paramOk(request, "cvCrossover")) {  // CV crossover ω_c (rad/s)
      foundParameter = true;
      cvCrossover = clamp_f(request->getParam("cvCrossover")->value().toFloat(), 0.05f, 0.80f);  // 0.80 ≈ 5 s, fast end of the seconds control
      settingWrite(NK_cvCrossover, String(cvCrossover, 3).c_str());
      recomputeCvGains();
      queueConsoleMessageF("CV crossover omega_c: %.2f rad/s", cvCrossover);
    }
    // User-facing CV Response Time in seconds → ω_c = 4/sec (settle ≈ 4 time constants). Stored as cvCrossover;
    // the dashboard relabels ω_c as seconds, but ω_c stays the internal/NVS unit so recomputeCvGains is unchanged.
    if (paramOk(request, "cvRespS")) {
      foundParameter = true;
      float respS = request->getParam("cvRespS")->value().toFloat();
      cvCrossover = clamp_f((respS > 0.0f) ? (4.0f / respS) : 0.20f, 0.05f, 0.80f);
      settingWrite(NK_cvCrossover, String(cvCrossover, 3).c_str());
      recomputeCvGains();
      queueConsoleMessageF("CV response time: %.0f s (crossover %.2f rad/s)", 4.0f / cvCrossover, cvCrossover);
    }
    if (paramOk(request, "cvPiZero")) {  // CV PI integral zero ρ (rad/s) — Ki = ρ·Kp
      foundParameter = true;
      cvPiZero = clamp_f(request->getParam("cvPiZero")->value().toFloat(), 0.2f, 1.5f);
      settingWrite(NK_cvPiZero, String(cvPiZero, 3).c_str());
      recomputeCvGains();
      queueConsoleMessageF("CV PI zero (Ki/Kp ratio): %.2f", cvPiZero);
    }
    if (paramOk(request, "battTempDerateEnable")) {  // master on/off for the battery-temp gain derate
      foundParameter = true;
      battTempDerateEnable = (request->getParam("battTempDerateEnable")->value().toInt() != 0);
      settingWrite(NK_battTempDerateEn, String((int)battTempDerateEnable).c_str());
      recomputeCvGains();
      queueConsoleMessageF("Battery-temp gain derate: %s", battTempDerateEnable ? "ON" : "OFF");
    }
    if (paramOk(request, "battTempCoeff")) {  // battery fractional resistance change per °C (R rises as T falls)
      foundParameter = true;
      battTempCoeff = clamp_f(request->getParam("battTempCoeff")->value().toFloat(), 0.0f, 0.10f);
      settingWrite(NK_battTempCoeff, String(battTempCoeff, 4).c_str());
      recomputeCvGains();
      queueConsoleMessageF("Battery-temp resistance coeff: %.3f /°C", battTempCoeff);
    }
    if (paramOk(request, "vTgtRampEnable")) {  // master switch for the voltage-target slew (1=on, 0=instant)
      foundParameter = true;
      vTgtRampEnable = (uint8_t)(request->getParam("vTgtRampEnable")->value().toInt() ? 1 : 0);
      settingWrite(NK_vTgtRampEnable, String((int)vTgtRampEnable).c_str());
      queueConsoleMessageF("CV target ramp limiter: %s", vTgtRampEnable ? "ON" : "OFF (instant)");
    }
    if (paramOk(request, "setpointSlewEnable")) {  // master switch for inner-loop current setpoint slew (1=on, 0=instant)
      foundParameter = true;
      setpointSlewEnable = (uint8_t)(request->getParam("setpointSlewEnable")->value().toInt() ? 1 : 0);
      settingWrite(NK_setpointSlewEnable, String((int)setpointSlewEnable).c_str());
      queueConsoleMessageF("Current setpoint slew limiter: %s", setpointSlewEnable ? "ON" : "OFF (instant)");
    }
    if (paramOk(request, "cvRiseGovEnable")) {  // master switch for CV rise governor / anti-windup clamp (1=on, 0=off)
      foundParameter = true;
      cvRiseGovEnable = (uint8_t)(request->getParam("cvRiseGovEnable")->value().toInt() ? 1 : 0);
      settingWrite(NK_cvRiseGovEnable, String((int)cvRiseGovEnable).c_str());
      queueConsoleMessageF("CV rise governor (anti-windup): %s", cvRiseGovEnable ? "ON" : "OFF — OV-trip risk on up-steps");
    }
    if (paramOk(request, "cvRecovEnable")) {  // master switch for the post-protection integrator refill (1=on, 0=plain PI)
      foundParameter = true;
      cvRecovEnable = (uint8_t)(request->getParam("cvRecovEnable")->value().toInt() ? 1 : 0);
      settingWrite(NK_cvRecovEnable, String((int)cvRecovEnable).c_str());
      queueConsoleMessageF("CV recovery refill: %s", cvRecovEnable ? "ON" : "OFF — recovery pace scales with post-cut error");
    }
    if (paramOk(request, "loadServeBoostEnable")) {  // load-serve Ki boost toward the measured house loads (shunt-gated)
      foundParameter = true;
      loadServeBoostEnable = (uint8_t)(request->getParam("loadServeBoostEnable")->value().toInt() ? 1 : 0);
      settingWrite(NK_loadServeBoostEnable, String((int)loadServeBoostEnable).c_str());
      queueConsoleMessageF("Load pickup boost: %s", loadServeBoostEnable ? "ON" : "OFF — load pickup pace scales with voltage error only");
    }
    if (paramOk(request, "reseedCorrEnable")) {  // demand-corrected reseed: measured load-drop subtraction + rapid-refire ratchet escalation
      foundParameter = true;
      reseedCorrEnable = (uint8_t)(request->getParam("reseedCorrEnable")->value().toInt() ? 1 : 0);
      settingWrite(NK_reseedCorrEnable, String((int)reseedCorrEnable).c_str());
      queueConsoleMessageF("Smart reseed: %s", reseedCorrEnable ? "ON" : "OFF — every release restores the plain seed fraction");
    }
    if (paramOk(request, "HuntGovEnable")) {  // oscillation damper: hunt detector + verified inner-Ki derate
      foundParameter = true;
      HuntGovEnable = (uint8_t)(request->getParam("HuntGovEnable")->value().toInt() ? 1 : 0);
      settingWrite(NK_HuntGovEnable, String((int)HuntGovEnable).c_str());
      queueConsoleMessageF("Oscillation damper: %s", HuntGovEnable ? "ON" : "OFF — hunting persists at full gain");
    }
    if (paramOk(request, "HuntCutPct")) {
      foundParameter = true;
      HuntCutPct = (uint8_t)constrain(request->getParam("HuntCutPct")->value().toInt(), HUNT_CUT_PCT_MIN, HUNT_CUT_PCT_MAX);
      settingWrite(NK_HuntCutPct, String((int)HuntCutPct).c_str());
    }
    if (paramOk(request, "HuntVerifyPct")) {
      foundParameter = true;
      HuntVerifyPct = (uint8_t)constrain(request->getParam("HuntVerifyPct")->value().toInt(), HUNT_VERIFY_PCT_MIN, HUNT_VERIFY_PCT_MAX);
      settingWrite(NK_HuntVerifyPct, String((int)HuntVerifyPct).c_str());
    }
    if (paramOk(request, "HuntWingPct")) {
      foundParameter = true;
      HuntWingPct = (uint8_t)constrain(request->getParam("HuntWingPct")->value().toInt(), HUNT_WING_PCT_MIN, HUNT_WING_PCT_MAX);
      settingWrite(NK_HuntWingPct, String((int)HuntWingPct).c_str());
    }
    if (paramOk(request, "HuntCooldownMin")) {
      foundParameter = true;
      HuntCooldownMin = (uint8_t)constrain(request->getParam("HuntCooldownMin")->value().toInt(), HUNT_COOLDOWN_MIN_MIN, HUNT_COOLDOWN_MIN_MAX);
      settingWrite(NK_HuntCooldownMin, String((int)HuntCooldownMin).c_str());
    }
    if (paramOk(request, "HuntQualifyScans")) {
      foundParameter = true;
      HuntQualifyScans = (uint8_t)constrain(request->getParam("HuntQualifyScans")->value().toInt(), HUNT_QUALIFY_SCANS_MIN, HUNT_QUALIFY_SCANS_MAX);
      settingWrite(NK_HuntQualifyScans, String((int)HuntQualifyScans).c_str());
    }
    if (paramOk(request, "HuntTrigPct")) {
      foundParameter = true;
      HuntTrigPct = fmaxf(request->getParam("HuntTrigPct")->value().toFloat(), HUNT_TRIG_PCT_MIN);
      settingWrite(NK_HuntTrigPct, String(HuntTrigPct, 3).c_str());
    }
    if (paramOk(request, "HuntSteadyPct")) {
      foundParameter = true;
      HuntSteadyPct = (uint8_t)constrain(request->getParam("HuntSteadyPct")->value().toInt(), HUNT_STEADY_PCT_MIN, HUNT_STEADY_PCT_MAX);
      settingWrite(NK_HuntSteadyPct, String((int)HuntSteadyPct).c_str());
    }
    if (paramOk(request, "cvRecovSec")) {  // retired knob: writes an inert global
      foundParameter = true;
      cvRecovSec = clamp_f(request->getParam("cvRecovSec")->value().toFloat(), 0.5f, 30.0f);
      settingWrite(NK_cvRecovSec, String(cvRecovSec, 2).c_str());
      queueConsoleMessageF("CV recovery time: %.1f s", cvRecovSec);
    }
    if (paramOk(request, "cvRecovEmaxV")) {  // retired knob: writes an inert global
      foundParameter = true;
      cvRecovEmaxV = clamp_f(request->getParam("cvRecovEmaxV")->value().toFloat(), 0.05f, 2.0f);
      settingWrite(NK_cvRecovEmaxV, String(cvRecovEmaxV, 3).c_str());
      queueConsoleMessageF("CV recovery error cap: %.2f V", cvRecovEmaxV);
    }
    if (paramOk(request, "cvRecovKiMax")) {  // refill Ki multiplier at release, tapering to 1x as the deficit heals
      foundParameter = true;
      cvRecovKiMax = clamp_f(request->getParam("cvRecovKiMax")->value().toFloat(), 1.0f, 10.0f);
      settingWrite(NK_cvRecovKiMax, String(cvRecovKiMax, 2).c_str());
      queueConsoleMessageF("CV recovery refill max rate: %.1fx", cvRecovKiMax);
    }
    if (paramOk(request, "cvRecovBoostEnable")) {  // master switch for the post-protection recovery P-boost (1=on, 0=off)
      foundParameter = true;
      cvRecovBoostEnable = (uint8_t)(request->getParam("cvRecovBoostEnable")->value().toInt() ? 1 : 0);
      settingWrite(NK_cvRecovBoostEnable, String((int)cvRecovBoostEnable).c_str());
      queueConsoleMessageF("CV recovery P-boost: %s", cvRecovBoostEnable ? "ON" : "OFF");
    }
    if (paramOk(request, "cvRecovBoostMax")) {  // recovery P-boost max multiplier at full shortfall
      foundParameter = true;
      cvRecovBoostMax = clamp_f(request->getParam("cvRecovBoostMax")->value().toFloat(), 1.0f, 8.0f);
      settingWrite(NK_cvRecovBoostMax, String(cvRecovBoostMax, 2).c_str());
      queueConsoleMessageF("CV recovery P-boost max: %.2fx", cvRecovBoostMax);
    }
    if (paramOk(request, "cvRecovBoostErrV")) {  // shortfall (V per 12V block) at which the boost reaches max
      foundParameter = true;
      cvRecovBoostErrV = clamp_f(request->getParam("cvRecovBoostErrV")->value().toFloat(), 0.1f, 5.0f);
      settingWrite(NK_cvRecovBoostErrV, String(cvRecovBoostErrV, 3).c_str());
      queueConsoleMessageF("CV recovery P-boost full-boost shortfall: %.2f V", cvRecovBoostErrV);
    }
    if (paramOk(request, "cvRecovBoostFloorV")) {  // dead area (V per 12V block): no boost within this shortfall of target
      foundParameter = true;
      cvRecovBoostFloorV = clamp_f(request->getParam("cvRecovBoostFloorV")->value().toFloat(), 0.0f, 5.0f);
      settingWrite(NK_cvRecovBoostFloorV, String(cvRecovBoostFloorV, 3).c_str());
      queueConsoleMessageF("CV recovery P-boost dead area: %.2f V", cvRecovBoostFloorV);
    }
    if (paramOk(request, "cvRecovDeepBandV")) {  // deep-recovery band (V per 12V block): beyond this shortfall the walk speeds up and episode boost stays live
      foundParameter = true;
      cvRecovDeepBandV = clamp_f(request->getParam("cvRecovDeepBandV")->value().toFloat(), 0.0f, 5.0f);
      settingWrite(NK_cvRecovDeepBandV, String(cvRecovDeepBandV, 3).c_str());
      queueConsoleMessageF("CV recovery deep band: %.2f V", cvRecovDeepBandV);
    }
    if (paramOk(request, "cvRecovDeepMult")) {  // starve-walk rate multiplier at full depth
      foundParameter = true;
      cvRecovDeepMult = clamp_f(request->getParam("cvRecovDeepMult")->value().toFloat(), 1.0f, 20.0f);
      settingWrite(NK_cvRecovDeepMult, String(cvRecovDeepMult, 1).c_str());
      queueConsoleMessageF("CV recovery deep walk multiplier: %.1fx", cvRecovDeepMult);
    }
    if (paramOk(request, "cvRecovFlareBandV")) {  // arrival flare band (V per 12V block): recovery ceiling tapers within this shortfall
      foundParameter = true;
      cvRecovFlareBandV = clamp_f(request->getParam("cvRecovFlareBandV")->value().toFloat(), 0.0f, 2.0f);
      settingWrite(NK_cvRecovFlareBandV, String(cvRecovFlareBandV, 3).c_str());
      queueConsoleMessageF("CV recovery arrival flare band: %.2f V", cvRecovFlareBandV);
    }
    if (paramOk(request, "cvRecovFlareFrac")) {  // flare ceiling at target, fraction of the recovery goal
      foundParameter = true;
      cvRecovFlareFrac = clamp_f(request->getParam("cvRecovFlareFrac")->value().toFloat(), 0.5f, 1.0f);
      settingWrite(NK_cvRecovFlareFrac, String(cvRecovFlareFrac, 2).c_str());
      queueConsoleMessageF("CV recovery arrival flare floor: %.2f of goal", cvRecovFlareFrac);
    }
    if (paramOk(request, "dutySlewEnable")) {  // master switch for field duty slew (1=on, 0=instant)
      foundParameter = true;
      dutySlewEnable = (uint8_t)(request->getParam("dutySlewEnable")->value().toInt() ? 1 : 0);
      settingWrite(NK_dutySlewEnable, String((int)dutySlewEnable).c_str());
      queueConsoleMessageF("Field duty slew limiter: %s", dutySlewEnable ? "ON" : "OFF (instant)");
    }
    if (paramOk(request, "testSlewMode")) {  // manual CC test slew: 0=off/instant, 1=default rates, 2=custom
      foundParameter = true;
      int m = request->getParam("testSlewMode")->value().toInt();
      testSlewMode = (uint8_t)(m < 0 ? 0 : (m > 2 ? 2 : m));
      settingWrite(NK_testSlewMode, String((int)testSlewMode).c_str());
      queueConsoleMessageF("Manual test slew mode: %s",
                           testSlewMode == 0 ? "Off (instant)" : testSlewMode == 1 ? "Default rates" : "Custom rates");
    }
    if (paramOk(request, "cvTestSlewMode")) {  // manual CV test slew: 0=off/instant target, 1=default rate, 2=custom
      foundParameter = true;
      int m = request->getParam("cvTestSlewMode")->value().toInt();
      cvTestSlewMode = (uint8_t)(m < 0 ? 0 : (m > 2 ? 2 : m));
      settingWrite(NK_cvTestSlewMode, String((int)cvTestSlewMode).c_str());
      queueConsoleMessageF("Manual CV test slew mode: %s",
                           cvTestSlewMode == 0 ? "Off (instant)" : cvTestSlewMode == 1 ? "Default rate" : "Custom rate");
    }
    if (paramOk(request, "vTgtRampUp")) {  // CV voltage-target ramp UP rate (V/s); 0 = instant
      foundParameter = true;
      vTgtRampUp = clamp_f(request->getParam("vTgtRampUp")->value().toFloat(), 0.0f, 5.0f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f));
      settingWrite(NK_vTgtRampUp, String(vTgtRampUp, 3).c_str());
      queueConsoleMessageF("CV target ramp up: %.3f V/s", vTgtRampUp);
    }
    if (paramOk(request, "vTgtRampDn")) {  // CV voltage-target ramp DOWN rate (V/s); 0 = instant
      foundParameter = true;
      vTgtRampDn = clamp_f(request->getParam("vTgtRampDn")->value().toFloat(), 0.0f, 5.0f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f));
      settingWrite(NK_vTgtRampDn, String(vTgtRampDn, 3).c_str());
      queueConsoleMessageF("CV target ramp down: %.3f V/s", vTgtRampDn);
    }
    if (paramOk(request, "cvWindDownEnable")) {  // commanded-target wind-down governor master switch
      foundParameter = true;
      cvWindDownEnable = (uint8_t)(request->getParam("cvWindDownEnable")->value().toInt() != 0);
      settingWrite(NK_cvWindDownEn, String((int)cvWindDownEnable).c_str());
      queueConsoleMessageF("CV target wind-down: %s", cvWindDownEnable ? "enabled" : "disabled");
    }
    if (paramOk(request, "cvWindDownRate")) {  // fraction of MaxTableValue shed per second
      foundParameter = true;
      cvWindDownRate = clamp_f(request->getParam("cvWindDownRate")->value().toFloat(), 0.005f, 1.0f);
      settingWrite(NK_cvWindDownRate, String(cvWindDownRate, 3).c_str());
      queueConsoleMessageF("CV wind-down rate: %.3f of max amps/s", cvWindDownRate);
    }
    if (paramOk(request, "cvWindDownStopV")) {  // V real per-bus — stop margin above the commanded target
      foundParameter = true;
      cvWindDownStopV = clamp_f(request->getParam("cvWindDownStopV")->value().toFloat(), 0.0f, 0.3f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f));
      settingWrite(NK_cvWindDownStopV, String(cvWindDownStopV, 3).c_str());
      queueConsoleMessageF("CV wind-down stop margin: %.3f V", cvWindDownStopV);
    }
    // Plant curve K(t) = Ka + Kb·√t; the fit writes both, a bench hand-entry may send cvPlantK alone.
    if (paramOk(request, "cvPlantK")) {   // alias: flat curve at the given gain
      foundParameter = true;
      cvPlantKa = request->getParam("cvPlantK")->value().toFloat();
      cvPlantKb = 0.0f;
      settingWrite(NK_cvPlantKa, String(cvPlantKa, 5).c_str());
      settingWrite(NK_cvPlantKb, "0.00000");
      recomputeCvGains();
    }
    if (paramOk(request, "cvPlantKa")) {
      foundParameter = true;
      cvPlantKa = request->getParam("cvPlantKa")->value().toFloat();
      settingWrite(NK_cvPlantKa, String(cvPlantKa, 5).c_str());
      // Stamp battery temp + source at measurement time as the battery-temp derate reference. No qualifying
      // source keeps the prior stamp; computeCvTempScale stays inert until a fit runs with one.
      if (isfinite(battTempActiveF) && battTempActiveSrc >= 1 && battTempActiveSrc <= 4) {
        CommissionTempF = battTempActiveF;
        CommissionTempSrc = (int)battTempActiveSrc;
        settingWrite(NK_CommissionTempF, String(CommissionTempF, 2).c_str());
        settingWrite(NK_CommissionTempSrc, String(CommissionTempSrc).c_str());
      }
      recomputeCvGains();
    }
    if (paramOk(request, "cvPlantKb")) {
      foundParameter = true;
      cvPlantKb = fmaxf(0.0f, request->getParam("cvPlantKb")->value().toFloat());   // a falling tail is never physical
      settingWrite(NK_cvPlantKb, String(cvPlantKb, 5).c_str());
      recomputeCvGains();
    }
    // Measured ripple projection (§3.3): browser-fitted ripple(I)=a0+a1·I record as CSV. Reference for the
    // Protections ripple-vs-threshold plot ONLY — never touches the over-current floor.
    if (request->hasParam("ripFitAlt")) {
      foundParameter = true;
      ripFitDecode(request->getParam("ripFitAlt")->value(), ripFitAlt);
      settingWrite(NK_ripFitAlt, ripFitEncode(ripFitAlt).c_str());
    }
    // Measured voltage-slope projection (CV D-term deadband): same record shape, slope points in V/s.
    // Reference for the deadband card + wizard Set — never moves CvKdDeadbandVps by itself.
    if (request->hasParam("slpFitAlt")) {
      foundParameter = true;
      ripFitDecode(request->getParam("slpFitAlt")->value(), slpFitAlt);
      settingWrite(NK_slpFitAlt, ripFitEncode(slpFitAlt).c_str());
    }
    // Resonance current-check (§3.2): arm/disarm field-commanding, and set the commanded level. Disarm
    // also zeroes the target so the loop slews back toward the normal AUTO setpoint gently.
    if (paramOk(request, "resTest")) {
      foundParameter = true;
      bool arm = (request->getParam("resTest")->value().toInt() != 0);
      const char *rr = arm ? rpmTestRefuseReason() : nullptr;
      if (arm && (cvStressActive || protTestActive != 0)) {
        queueConsoleMessageF("Resonance current-check: start blocked — %s is active",
                             cvStressActive ? "CV stress test" : "Protection test");
      } else if (rr) {
        queueConsoleMessageF("Resonance current-check: start blocked — %s", rr);
      } else if (arm) { resTestActive = true; resTestReleasing = false; }
      // Release = DEFERRED: keep the field under test-control (OV still suppressed) and let the loop wind the
      // current down to ~0 first; it drops resTestActive itself once the field is down (6_functions.ino), so
      // CV re-enters at low voltage and slews up from below rather than slamming G2 on the transition edge.
      else if (resTestActive) { resTestReleasing = true; resTestTargetA = 0.0f; }
      resTestLastCmdMs = millis();
      if (!rr) queueConsoleMessageF("Resonance current-check %s", arm ? "ARMED (wizard commands current)" : "winding down");
    }
    // The `else` is load-bearing: a batched resTest=0&resTestTargetA=N would otherwise zero the target in the
    // disarm above and then command N amps back into a field that was asked to wind down. Arm/disarm wins.
    else if (paramOk(request, "resTestTargetA")) {
      foundParameter = true;
      resTestTargetA = request->getParam("resTestTargetA")->value().toFloat();
      resTestLastCmdMs = millis();  // keepalive refresh (deadman)
    }
    if (paramOk(request, "CvKdDeadbandVps")) {
      foundParameter = true;
      inputMessage = request->getParam("CvKdDeadbandVps")->value();
      CvKdDeadbandVps = inputMessage.toFloat();
      settingWrite(NK_CvKdDeadbandVps, String(CvKdDeadbandVps, 3).c_str());
      queueConsoleMessageF("CV D-term deadband base: %.3f V/s", CvKdDeadbandVps);
    }
    if (paramOk(request, "CvKdDbSlope")) {
      foundParameter = true;
      inputMessage = request->getParam("CvKdDbSlope")->value();
      CvKdDbSlope = inputMessage.toFloat();
      settingWrite(NK_CvKdDbSlope, String(CvKdDbSlope, 5).c_str());
      queueConsoleMessageF("CV D-term deadband slope: %.5f V/s per A", CvKdDbSlope);
    }
    if (paramOk(request, "CvKdDbFloor")) {
      foundParameter = true;
      inputMessage = request->getParam("CvKdDbFloor")->value();
      CvKdDbFloor = inputMessage.toFloat();
      settingWrite(NK_CvKdDbFloor, String(CvKdDbFloor, 3).c_str());
      queueConsoleMessageF("CV D-term deadband floor: %.3f V/s", CvKdDbFloor);
    }
    if (paramOk(request, "CvKdDbCeil")) {
      foundParameter = true;
      inputMessage = request->getParam("CvKdDbCeil")->value();
      CvKdDbCeil = inputMessage.toFloat();
      settingWrite(NK_CvKdDbCeil, String(CvKdDbCeil, 3).c_str());
      queueConsoleMessageF("CV D-term deadband ceiling: %.3f V/s", CvKdDbCeil);
    }
    if (paramOk(request, "CvKdOneSided")) {
      foundParameter = true;
      inputMessage = request->getParam("CvKdOneSided")->value();
      CvKdOneSided = inputMessage.toInt() != 0;
      settingWrite(NK_CvKdOneSided, String((int)CvKdOneSided).c_str());
      queueConsoleMessageF("CV D-term mode: %s", CvKdOneSided ? "ONE-SIDED (removes current only)" : "SYMMETRIC (adds on falling bus)");
    }
    if (paramOk(request, "CvKdExcessMode")) {
      foundParameter = true;
      inputMessage = request->getParam("CvKdExcessMode")->value();
      CvKdExcessMode = inputMessage.toInt() != 0;
      settingWrite(NK_CvKdExcessMode, String((int)CvKdExcessMode).c_str());
      queueConsoleMessageF("CV D-term response: %s", CvKdExcessMode ? "GRADUAL (slope excess over tolerance line)" : "STEPPED (legacy full-slope latch)");
    }
    if (paramOk(request, "VoltageKd")) {
      foundParameter = true;
      inputMessage = request->getParam("VoltageKd")->value();
      VoltageKd = inputMessage.toFloat();
      settingWrite(NK_VoltageKd, String(VoltageKd, 1).c_str());
      recomputeCvGains();  // manual gain changed → refresh the active gain
      queueConsoleMessageF("CV D-term gain: %.1f A/(V/s)", VoltageKd);
    }
    if (paramOk(request, "CvKdVoltFiltTC")) {
      foundParameter = true;
      inputMessage = request->getParam("CvKdVoltFiltTC")->value();
      float kdFiltPrev = CvKdVoltFiltTC;
      CvKdVoltFiltTC = inputMessage.toFloat();
      settingWrite(NK_CvKdVoltFiltTC, String(CvKdVoltFiltTC, 0).c_str());
      queueConsoleMessageF("CV D-term voltage filter TC: %.0f ms", CvKdVoltFiltTC);
      // slpFitAlt is the worst slope OF g_cvKdFiltV, the output of this very filter, so this TC changes the
      // measured quantity exactly as IExcessTau does for the ripple fit. The pk-pk fit is untouched by it.
      if (fabsf(CvKdVoltFiltTC - kdFiltPrev) > 0.5f) ripFitForget(false, true, "CV D-term voltage filter TC changed");
    }
    if (paramOk(request, "cvHelpersEnabled")) {
      foundParameter = true;
      inputMessage = request->getParam("cvHelpersEnabled")->value();
      cvHelpersEnabled = inputMessage.toInt() != 0;
      settingWrite(NK_cvHelpersEnabled, String((int)cvHelpersEnabled).c_str());
      queueConsoleMessageF("CV tuning helpers (asymmetric unwind + D term): %s", cvHelpersEnabled ? "ENABLED" : "DISABLED");
    }
    if (paramOk(request, "CvKdArmV")) {
      foundParameter = true;
      inputMessage = request->getParam("CvKdArmV")->value();
      CvKdArmV = inputMessage.toFloat();
      settingWrite(NK_CvKdArmV, String(CvKdArmV, 2).c_str());
      queueConsoleMessageF("CV D-term arm window: %.2f V", CvKdArmV);
    }
    if (paramOk(request, "CvKdMaxTrimA")) {
      foundParameter = true;
      inputMessage = request->getParam("CvKdMaxTrimA")->value();
      CvKdMaxTrimA = fmaxf(0.0f, inputMessage.toFloat());
      settingWrite(NK_CvKdMaxTrimA, String(CvKdMaxTrimA, 1).c_str());
      queueConsoleMessageF("CV D-term max back-off: %.1f A", CvKdMaxTrimA);
    }
    if (paramOk(request, "CvKdSlopeCeil")) {
      foundParameter = true;
      inputMessage = request->getParam("CvKdSlopeCeil")->value();
      CvKdSlopeCeil = clamp_f(inputMessage.toFloat(), 1.0f*((float)SYSTEM_VOLTAGE_CLASS/12.0f), 20.0f*((float)SYSTEM_VOLTAGE_CLASS/12.0f));  // real per-bus — bounds scale with class like the input widget
      settingWrite(NK_CvKdSlopeCeil, String(CvKdSlopeCeil, 1).c_str());
      queueConsoleMessageF("CV D-term slope ceiling: %.1f V/s", CvKdSlopeCeil);
    }
    if (paramOk(request, "CvKdTd")) {  // CV D derivative time — Measured-source Kd = Td·Kp
      foundParameter = true;
      inputMessage = request->getParam("CvKdTd")->value();
      CvKdTd = clamp_f(inputMessage.toFloat(), 0.0f, 3.0f);
      settingWrite(NK_CvKdTd, String(CvKdTd, 2).c_str());
      recomputeCvGains();  // Td feeds the auto Kd
      queueConsoleMessageF("CV D-term time Td: %.2f s (Measured Kd = Td x Kp)", CvKdTd);
    }
    if (paramOk(request, "TempPIDKp")) {
      foundParameter = true;
      inputMessage = request->getParam("TempPIDKp")->value();
      settingWrite(NK_TempPIDKp, inputMessage.c_str());
      TempPIDKp = inputMessage.toFloat();
      queueConsoleMessageF("Temp PID Kp updated to: %.6f", TempPIDKp);
    }
    if (paramOk(request, "TempPIDKi")) {
      foundParameter = true;
      inputMessage = request->getParam("TempPIDKi")->value();
      settingWrite(NK_TempPIDKi, inputMessage.c_str());
      TempPIDKi = inputMessage.toFloat();
      queueConsoleMessageF("Temp PID Ki updated to: %.6f", TempPIDKi);
    }
    if (paramOk(request, "TempPIDKiDownFrac")) {
      foundParameter = true;
      inputMessage = request->getParam("TempPIDKiDownFrac")->value();
      TempPIDKiDownFrac = constrain(inputMessage.toFloat(), 0.0f, 1.0f);  // ratio of Ki used to bleed below setpoint
      settingWrite(NK_TempPIDKiDownFrac, String(TempPIDKiDownFrac, 3).c_str());
      queueConsoleMessageF("Temp PID below-setpoint bleed ratio set to: %.2f x Ki", TempPIDKiDownFrac);
    }
    if (paramOk(request, "ThermalLookaheadSec")) {
      foundParameter = true;
      inputMessage = request->getParam("ThermalLookaheadSec")->value();
      ThermalLookaheadSec = clamp_f(inputMessage.toFloat(), 0.0f, 300.0f);
      settingWrite(NK_ThermalLookaheadSec, String(ThermalLookaheadSec, 1).c_str());      queueConsoleMessageF("ThermalLookaheadSec set to: %.1f s", ThermalLookaheadSec);
    }
    if (paramOk(request, "ThermalSlopeWindowSec")) {
      foundParameter = true;
      inputMessage = request->getParam("ThermalSlopeWindowSec")->value();
      ThermalSlopeWindowSec = clamp_f(inputMessage.toFloat(), 10.0f, 60.0f);  // slope backward-difference window
      settingWrite(NK_ThermalSlopeWindowSec, String(ThermalSlopeWindowSec, 1).c_str());
      queueConsoleMessageF("ThermalSlopeWindowSec set to: %.1f s", ThermalSlopeWindowSec);
    }
    if (paramOk(request, "TempPIDIntervalMs")) {
      foundParameter = true;
      inputMessage = request->getParam("TempPIDIntervalMs")->value();
      long tpi = strtol(inputMessage.c_str(), nullptr, 10);   // signed: the global is uint32_t, so "-5" must clamp before assignment
      TempPIDIntervalMs = (uint32_t)constrain(tpi, 500L, 60000L);   // 0 makes the thermal slope NaN; UI range is 500..60000
      settingWrite(NK_TempPIDIntervalMs, String(TempPIDIntervalMs).c_str());
      queueConsoleMessageF("Temp PID interval updated to: %d ms", TempPIDIntervalMs);
    }
    if (paramOk(request, "TempPIDFilterAlpha")) {
      foundParameter = true;
      inputMessage = request->getParam("TempPIDFilterAlpha")->value();
      settingWrite(NK_TempPIDFilterAlpha, inputMessage.c_str());
      TempPIDFilterAlpha = inputMessage.toFloat();      queueConsoleMessageF("Temp PID filter alpha updated to: %.3f", TempPIDFilterAlpha);
    }
    if (paramOk(request, "PidKi")) {
      foundParameter = true;
      inputMessage = request->getParam("PidKi")->value();
      settingWrite(NK_PidKi, inputMessage.c_str());
      PidKi = inputMessage.toFloat();
      if (pidInitialized) {
        recomputeCcGains();  // apply voltage-normalized PidK*_active
      }
      if (TuningMode) tuningParamChanged = true;
      queueConsoleMessageF("PID Ki updated to: %.6f", PidKi);
    }
    if (paramOk(request, "PidKd")) {
      foundParameter = true;
      inputMessage = request->getParam("PidKd")->value();
      settingWrite(NK_PidKd, inputMessage.c_str());
      PidKd = inputMessage.toFloat();
      if (pidInitialized) {
        recomputeCcGains();  // apply voltage-normalized PidK*_active
      }
      if (TuningMode) tuningParamChanged = true;
      queueConsoleMessageF("PID Kd updated to: %.6f", PidKd);
    }
    if (paramOk(request, "PidSampleDivisor")) {
      foundParameter = true;
      inputMessage = request->getParam("PidSampleDivisor")->value();
      settingWrite(NK_PidSampleDivisor, inputMessage.c_str());
      PidSampleDivisor = inputMessage.toInt();
      if (TuningMode) tuningParamChanged = true;
    }
    if (paramOk(request, "LearningSettlingPeriod")) {
      foundParameter = true;
      inputMessage = request->getParam("LearningSettlingPeriod")->value();
      int temp = inputMessage.toInt() * 1000;  // Convert seconds to ms
      settingWrite(NK_LearningSettlingPeriod, String(temp).c_str());
      LearningSettlingPeriod = temp;
    }
    if (paramOk(request, "LearningRPMChangeThreshold")) {
      foundParameter = true;
      inputMessage = request->getParam("LearningRPMChangeThreshold")->value();
      settingWrite(NK_LearningRPMChangeThreshold, inputMessage.c_str());
      LearningRPMChangeThreshold = inputMessage.toInt();
    }
    if (paramOk(request, "LearningTempHysteresis")) {
      foundParameter = true;
      inputMessage = request->getParam("LearningTempHysteresis")->value();
      settingWrite(NK_LearningTempHysteresis, inputMessage.c_str());
      LearningTempHysteresis = inputMessage.toInt();
    }
    // "Group 0" in UI = hardware overcurrent trip (no protection-group integration yet)
    if (paramOk(request, "MaxTableValue")) {
      foundParameter = true;
      inputMessage = request->getParam("MaxTableValue")->value();
      settingWrite(NK_MaxTableValue, inputMessage.c_str());
      MaxTableValue = inputMessage.toFloat();
      HardOCTripAmps = MaxTableValue + 10.0f;
      queueConsoleMessageF("Alternator current limit set to %.1fA — OC trip threshold: %.1fA", MaxTableValue, HardOCTripAmps);
    }
    if (paramOk(request, "MaxPenaltyPercent")) {
      foundParameter = true;
      inputMessage = request->getParam("MaxPenaltyPercent")->value();
      settingWrite(NK_MaxPenaltyPercent, inputMessage.c_str());
      MaxPenaltyPercent = inputMessage.toFloat();
    }
    if (paramOk(request, "MaxPenaltyDuration")) {
      foundParameter = true;
      inputMessage = request->getParam("MaxPenaltyDuration")->value();
      int temp = inputMessage.toInt() * 1000;
      settingWrite(NK_MaxPenaltyDuration, String(temp).c_str());
      MaxPenaltyDuration = temp;
    }
    if (paramOk(request, "NeighborLearningFactor")) {
      foundParameter = true;
      inputMessage = request->getParam("NeighborLearningFactor")->value();
      settingWrite(NK_NeighborLearningFactor, inputMessage.c_str());
      NeighborLearningFactor = inputMessage.toFloat();
    }
    if (paramOk(request, "yyMax")) {
      foundParameter = true;
      inputMessage = request->getParam("yyMax")->value();
      settingWrite(NK_yyMax, inputMessage.c_str());
      yyMax = inputMessage.toInt();
    }
    if (paramOk(request, "LearningMemoryDuration")) {
      foundParameter = true;
      inputMessage = request->getParam("LearningMemoryDuration")->value();
      settingWrite(NK_LearningMemoryDuration, inputMessage.c_str());
      LearningMemoryDuration = inputMessage.toInt();
    }
    if (paramOk(request, "DutyRampRate")) {
      foundParameter = true;
      inputMessage = request->getParam("DutyRampRate")->value();
      const float rampMin = DUTY_RAMP_MIN_12V * fieldScale();
      if (inputMessage.toFloat() < rampMin) {
        queueConsoleMessageF("Duty ramp rate must be at least %.2f %%/sec - rejected, keeping %.2f", rampMin, DutyRampRate);
      } else {
        settingWrite(NK_DutyRampRate, inputMessage.c_str());
        DutyRampRate = inputMessage.toFloat();
        if (TuningMode) tuningParamChanged = true;
        queueConsoleMessageF("Duty ramp rate set to: %.1f %%/sec", DutyRampRate);
      }
    }
    if (paramOk(request, "DutySlowRampRate")) {
      foundParameter = true;
      inputMessage = request->getParam("DutySlowRampRate")->value();
      const float rampMin = DUTY_RAMP_MIN_12V * fieldScale();
      if (inputMessage.toFloat() < rampMin) {
        queueConsoleMessageF("Shutdown final ramp rate must be at least %.2f %%/s - rejected, keeping %.2f", rampMin, DutySlowRampRate);
      } else {
        settingWrite(NK_DutySlowRampRate, inputMessage.c_str());
        DutySlowRampRate = inputMessage.toFloat();
        queueConsoleMessageF("Shutdown final ramp rate set to: %.2f %%/s", DutySlowRampRate);
      }
    }
    if (paramOk(request, "ShutdownRampTauS")) {
      foundParameter = true;
      ShutdownRampTauS = clamp_f(request->getParam("ShutdownRampTauS")->value().toFloat(), 0.0f, 60.0f);
      settingWrite(NK_ShutdownRampTauS, String(ShutdownRampTauS, 2).c_str());
      queueConsoleMessageF("Shutdown ramp time constant set to: %.2f s", ShutdownRampTauS);
    }
    if (paramOk(request, "NoOutputAmps")) {
      foundParameter = true;
      NoOutputAmps = clamp_f(request->getParam("NoOutputAmps")->value().toFloat(), 0.1f, 50.0f);
      settingWrite(NK_NoOutputAmps, String(NoOutputAmps, 2).c_str());
      queueConsoleMessageF("No-output current set to: %.2f A", NoOutputAmps);
    }
    if (paramOk(request, "ShutdownPhase2HoldMs")) {
      foundParameter = true;
      inputMessage = request->getParam("ShutdownPhase2HoldMs")->value();
      uint32_t ms = (uint32_t)inputMessage.toInt();
      settingWrite(NK_ShutdownPhase2HoldMs, String(ms).c_str());
      ShutdownPhase2HoldMs = ms;
      queueConsoleMessageF("Shutdown phase 2 hold set to: %u ms", ShutdownPhase2HoldMs);
    }
    if (paramOk(request, "SettleTimeBeforeCut")) {
      foundParameter = true;
      inputMessage = request->getParam("SettleTimeBeforeCut")->value();
      settingWrite(NK_SettleTimeBeforeCut, inputMessage.c_str());
      SettleTimeBeforeCut = inputMessage.toInt();
      queueConsoleMessageF("Settle time before cut set to: %d ms", SettleTimeBeforeCut);
    }
    if (paramOk(request, "TempWarnExcess")) {
      foundParameter = true;
      inputMessage = request->getParam("TempWarnExcess")->value();
      settingWrite(NK_TempWarnExcess, inputMessage.c_str());
      TempWarnExcess = inputMessage.toFloat();
      queueConsoleMessageF("Temp warning threshold set to: +%.1f%s above limit", dispTempDeltaF(TempWarnExcess), dispTempUnit());
    }
    if (paramOk(request, "TempCritExcess")) {
      foundParameter = true;
      inputMessage = request->getParam("TempCritExcess")->value();
      settingWrite(NK_TempCritExcess, inputMessage.c_str());
      TempCritExcess = inputMessage.toFloat();
      queueConsoleMessageF("Temp critical threshold set to: +%.1f%s above limit", dispTempDeltaF(TempCritExcess), dispTempUnit());
    }
    if (paramOk(request, "TempSustainedTimeout")) {
      foundParameter = true;
      inputMessage = request->getParam("TempSustainedTimeout")->value();
      int temp = inputMessage.toInt() * 1000;  // user enters seconds
      settingWrite(NK_TempSustainedTimeout, String(temp).c_str());
      TempSustainedTimeout = temp;
      queueConsoleMessageF("Temp sustained timeout set to: %d seconds", inputMessage.toInt());
    }
    // Both OV-ladder rungs can arrive in ONE request, so enforce the ladder order once on the joint values;
    // clamping one at a time against the value being replaced corrupts a joint move. The software cut sits
    // strictly below the hardware rung (0.05 x class/12 guard band) so software always gets first shot.
    bool swRungPresent = paramOk(request, "AlternatorHardShutdownV");
    bool hwRungPresent = paramOk(request, "VoltageHardwareLimit");
    if (swRungPresent || hwRungPresent) {
      foundParameter = true;
      float reqSw = swRungPresent ? request->getParam("AlternatorHardShutdownV")->value().toFloat()
                                  : AlternatorHardShutdownV;
      float reqHw = hwRungPresent ? constrain(request->getParam("VoltageHardwareLimit")->value().toFloat(), 10.0f, 70.0f)
                                  : VoltageHardwareLimit;
      float swMax = reqHw - 0.05f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f);
      bool swClamped = reqSw > swMax;
      AlternatorHardShutdownV = swClamped ? swMax : reqSw;
      VoltageHardwareLimit = reqHw;
      settingWrite(NK_AlternatorHardShutdownV, String(AlternatorHardShutdownV, 2).c_str());
      if (hwRungPresent) {
        settingWrite(NK_VoltageHardwareLimit, String(VoltageHardwareLimit, 2).c_str());
        inaOvReprogramPending = true;  // loop() reprograms the comparator on its next pass — never leave it stale, never touch I2C from this task
        queueConsoleMessageF("Hardware shutdown voltage set to: %.2fV (INA228 ALERT, absolute)", VoltageHardwareLimit);
      }
      if (swClamped)
        queueConsoleMessageF("Alternator hard-shutdown clamped to %.2fV — must stay below the Hardware Shutdown Voltage (%.2fV)", AlternatorHardShutdownV, VoltageHardwareLimit);
      else if (swRungPresent)
        queueConsoleMessageF("Alternator hard-shutdown voltage set to: %.2fV (absolute)", AlternatorHardShutdownV);
    }
    // "Group 0" in UI = hardware overcurrent trip (no protection-group integration yet)
    if (paramOk(request, "HardOCDebounceMs")) {
      foundParameter = true;
      inputMessage = request->getParam("HardOCDebounceMs")->value();
      settingWrite(NK_HardOCDebounceMs, inputMessage.c_str());
      HardOCDebounceMs = (uint32_t)inputMessage.toInt();
      queueConsoleMessageF("Overcurrent trip debounce set to: %ums", HardOCDebounceMs);
    }
    if (paramOk(request, "WarmupRampRate")) {
      foundParameter = true;
      inputMessage = request->getParam("WarmupRampRate")->value();
      WarmupRampRate = max(0.0f, inputMessage.toFloat());
      settingWrite(NK_WarmupRampRate, String(WarmupRampRate, 2).c_str());
      queueConsoleMessageF("Warmup ramp rate set to: %.2f A/s", WarmupRampRate);
    }
    if (paramOk(request, "IExcessFrac")) {
      foundParameter = true;
      inputMessage = request->getParam("IExcessFrac")->value();  // UI sends percent; stored as fraction
      IExcessFrac = constrain(inputMessage.toFloat() / 100.0f, 0.02f, 0.50f);
      settingWrite(NK_IExcessFrac, String(IExcessFrac, 3).c_str());
      queueConsoleMessageF("IExcess CV threshold set to: %.0f%% of command", IExcessFrac * 100.0f);
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    if (paramOk(request, "IExcessFracBulk")) {
      foundParameter = true;
      inputMessage = request->getParam("IExcessFracBulk")->value();  // UI sends percent; stored as fraction
      IExcessFracBulk = constrain(inputMessage.toFloat() / 100.0f, 0.02f, 0.60f);
      settingWrite(NK_IExcessFracBulk, String(IExcessFracBulk, 3).c_str());
      queueConsoleMessageF("IExcess bulk threshold set to: %.0f%% of ceiling", IExcessFracBulk * 100.0f);
    }
    if (paramOk(request, "IExcessFloorA")) {
      foundParameter = true;
      inputMessage = request->getParam("IExcessFloorA")->value();
      IExcessFloorA = constrain(inputMessage.toFloat(), 1.0f, 20.0f);
      settingWrite(NK_IExcessFloorA, String(IExcessFloorA, 1).c_str());
      queueConsoleMessageF("IExcess threshold floor set to: %.1fA", IExcessFloorA);
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    if (paramOk(request, "IExcessCeilA")) {
      foundParameter = true;
      inputMessage = request->getParam("IExcessCeilA")->value();
      IExcessCeilA = constrain(inputMessage.toFloat(), 5.0f, 200.0f);
      settingWrite(NK_IExcessCeilA, String(IExcessCeilA, 1).c_str());
      queueConsoleMessageF("IExcess threshold ceiling set to: %.1fA", IExcessCeilA);
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    if (paramOk(request, "IExcessBaseA")) {
      foundParameter = true;
      inputMessage = request->getParam("IExcessBaseA")->value();
      IExcessBaseA = constrain(inputMessage.toFloat(), 0.0f, 150.0f);
      settingWrite(NK_IExcessBaseA, String(IExcessBaseA, 1).c_str());
      queueConsoleMessageF("IExcess trip-line base set to: %.1fA", IExcessBaseA);
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    if (paramOk(request, "IExcessCcOffsetA")) {
      foundParameter = true;
      inputMessage = request->getParam("IExcessCcOffsetA")->value();
      IExcessCcOffsetA = constrain(inputMessage.toFloat(), 0.0f, 150.0f);
      settingWrite(NK_IExcessCcOffsetA, String(IExcessCcOffsetA, 1).c_str());
      queueConsoleMessageF("IExcess CC offset set to: %.1fA above CV", IExcessCcOffsetA);
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    // Max battery charge current (G4). Ceiling on the alternator command = limit + measured
    // house-load offset; requires the INA228 battery shunt. 0 disables the feature.
    if (paramOk(request, "BattCurrentLimitA")) {
      foundParameter = true;
      inputMessage = request->getParam("BattCurrentLimitA")->value();
      BattCurrentLimitA = constrain(inputMessage.toFloat(), 0.0f, 2000.0f);  // battery-side limit, unrelated to the alternator ceiling: a large bank at 1C wants far more than 500
      settingWrite(NK_BattCurrentLimitA, String(BattCurrentLimitA, 1).c_str());
      if (BattCurrentLimitA > 0.0f) queueConsoleMessageF("Battery charge current limit set to: %.1fA", BattCurrentLimitA);
      else queueConsoleMessage("Battery charge current limit disabled");
    }
    if (paramOk(request, "IExcessTau")) {
      foundParameter = true;
      inputMessage = request->getParam("IExcessTau")->value();
      float iexTauPrev = IExcessTau;
      IExcessTau = constrain(inputMessage.toFloat(), 20.0f, 300.0f);
      settingWrite(NK_IExcessTau, String(IExcessTau, 1).c_str());
      queueConsoleMessageF("IExcess averaging TC set to: %.0f ms", IExcessTau);
      // The a0+a1·I fit is the pk-pk of THIS low-pass, so a tau change invalidates it; the slope fit and the
      // Ripple Map are not measured through it (see ripFitForget).
      if (fabsf(IExcessTau - iexTauPrev) > 0.05f) ripFitForget(true, false, "IExcess averaging TC changed");
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    if (paramOk(request, "IExcessRelFrac")) {
      foundParameter = true;
      inputMessage = request->getParam("IExcessRelFrac")->value();  // UI sends percent; stored as fraction
      IExcessRelFrac = constrain(inputMessage.toFloat() / 100.0f, 0.1f, 0.9f);
      settingWrite(NK_IExcessRelFrac, String(IExcessRelFrac, 3).c_str());
      queueConsoleMessageF("IExcess release hysteresis set to: %.0f%% of threshold", IExcessRelFrac * 100.0f);
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    if (paramOk(request, "IExcessKBleed")) {
      foundParameter = true;
      inputMessage = request->getParam("IExcessKBleed")->value();
      IExcessKBleed = inputMessage.toFloat();
      settingWrite(NK_IExcessKBleed, String(IExcessKBleed, 2).c_str());
      queueConsoleMessageF("K_bleed set to: %.2f A/s per A", IExcessKBleed);
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    if (paramOk(request, "IExcessArmMarginV")) {
      foundParameter = true;
      inputMessage = request->getParam("IExcessArmMarginV")->value();
      IExcessArmMarginV = constrain(inputMessage.toFloat(), 0.020f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f), 5.000f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f));
      settingWrite(NK_IExcessArmMarginV, String(IExcessArmMarginV, 3).c_str());
      queueConsoleMessageF("iExcess arming margin set to: %.0f mV below target", IExcessArmMarginV * 1000.0f);
    }
    if (paramOk(request, "AwBleedRate")) {
      foundParameter = true;
      inputMessage = request->getParam("AwBleedRate")->value();
      AwBleedRate = inputMessage.toFloat();
      settingWrite(NK_AwBleedRate, String(AwBleedRate, 1).c_str());
      queueConsoleMessageF("AW bleed rate set to: %.1f A/s", AwBleedRate);
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    // No AwRecoverRate handler — hardcoded in firmware (0.1f)
    if (paramOk(request, "FastSetpointRiseRate")) {
      foundParameter = true;
      inputMessage = request->getParam("FastSetpointRiseRate")->value();
      FastSetpointRiseRate = constrain(inputMessage.toFloat(), 1.0f, 50.0f);
      settingWrite(NK_FastSetpointRiseRate, String(FastSetpointRiseRate, 1).c_str());
      queueConsoleMessageF("Fast setpoint rise rate set to: %.1fx", FastSetpointRiseRate);
    }
    if (paramOk(request, "FastSetpointRiseWindowMs")) {
      foundParameter = true;
      inputMessage = request->getParam("FastSetpointRiseWindowMs")->value();
      FastSetpointRiseWindowMs = (uint32_t)constrain(inputMessage.toInt(), 500, 30000);
      settingWrite(NK_FastSetpointRiseWindowMs, String(FastSetpointRiseWindowMs).c_str());
      queueConsoleMessageF("Fast rise window set to: %ums", FastSetpointRiseWindowMs);
    }
    if (paramOk(request, "FastSetpointRiseHeadroomV")) {
      foundParameter = true;
      inputMessage = request->getParam("FastSetpointRiseHeadroomV")->value();
      FastSetpointRiseHeadroomV = constrain(inputMessage.toFloat(), 0.05f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f), 2.0f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f));
      settingWrite(NK_FastSetpointRiseHeadroomV, String(FastSetpointRiseHeadroomV, 2).c_str());
      queueConsoleMessageF("Fast rise headroom set to: %.2fV", FastSetpointRiseHeadroomV);
    }
    if (paramOk(request, "AwSeedProtectMs")) {
      foundParameter = true;
      inputMessage = request->getParam("AwSeedProtectMs")->value();
      AwSeedProtectMs = (uint16_t)constrain(inputMessage.toInt(), 0, 2000);
      settingWrite(NK_AwSeedProtectMs, String(AwSeedProtectMs).c_str());
      queueConsoleMessageF("AW seed protect window set to: %u ms", (unsigned)AwSeedProtectMs);
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    if (paramOk(request, "KHard")) {
      foundParameter = true;
      inputMessage = request->getParam("KHard")->value();
      KHard = inputMessage.toFloat();
      settingWrite(NK_KHard, String(KHard, 1).c_str());
      queueConsoleMessageF("KHard set to: %.1f A/V", KHard);
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    if (paramOk(request, "OvGroup1Enable")) {
      foundParameter = true;
      inputMessage = request->getParam("OvGroup1Enable")->value();
      OvGroup1Enable = inputMessage.toInt() != 0;
      settingWrite(NK_OvGroup1Enable, String((int)OvGroup1Enable).c_str());
      queueConsoleMessageF("OV Group 1 (prediction-based cap): %s", OvGroup1Enable ? "ENABLED" : "DISABLED");
    }
    if (paramOk(request, "OvGroup2Enable")) {
      foundParameter = true;
      inputMessage = request->getParam("OvGroup2Enable")->value();
      OvGroup2Enable = inputMessage.toInt() != 0;
      settingWrite(NK_OvGroup2Enable, String((int)OvGroup2Enable).c_str());
      queueConsoleMessageF("OV Group 2 (measured-voltage threshold): %s", OvGroup2Enable ? "ENABLED" : "DISABLED");
    }
    if (paramOk(request, "TachLieEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("TachLieEnable")->value();
      TachLieEnable = inputMessage.toInt() != 0;
      settingWrite(NK_TachLieEnable, String((int)TachLieEnable).c_str());
      queueConsoleMessageF("Tach plausibility cut (commanded current, zero output): %s", TachLieEnable ? "ENABLED" : "DISABLED");
    }
    if (paramOk(request, "HardOCEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("HardOCEnable")->value();
      HardOCEnable = inputMessage.toInt() != 0;
      settingWrite(NK_HardOCEnable, String((int)HardOCEnable).c_str());
      queueConsoleMessageF("Group 0 (hard over-current trip): %s", HardOCEnable ? "ENABLED" : "DISABLED");
    }
    if (paramOk(request, "IExcessEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("IExcessEnable")->value();
      IExcessEnable = inputMessage.toInt() != 0;
      settingWrite(NK_IExcessEnable, String((int)IExcessEnable).c_str());
      queueConsoleMessageF("Group 3 (alternator iExcess detectors): %s", IExcessEnable ? "ENABLED" : "DISABLED");
    }
    if (paramOk(request, "BattLimitEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("BattLimitEnable")->value();
      BattLimitEnable = inputMessage.toInt() != 0;
      settingWrite(NK_BattLimitEnable, String((int)BattLimitEnable).c_str());
      queueConsoleMessageF("Group 4 (battery charge current limit): %s", BattLimitEnable ? "ENABLED" : "DISABLED");
    }
    if (paramOk(request, "LoadDumpEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("LoadDumpEnable")->value();
      LoadDumpEnable = inputMessage.toInt() != 0;
      settingWrite(NK_LoadDumpEnable, String((int)LoadDumpEnable).c_str());
      queueConsoleMessageF("Group 5 (load dump): %s", LoadDumpEnable ? "ENABLED" : "DISABLED");
    }
    if (paramOk(request, "OutputPIDSigSrc")) {
      foundParameter = true;
      inputMessage = request->getParam("OutputPIDSigSrc")->value();
      OutputPIDSigSrc = constrain(inputMessage.toInt(), 0, 2);
      settingWrite(NK_OutputPIDSigSrc, String(OutputPIDSigSrc).c_str());
      const char* sigNames[] = { "EMA(TC)", "MA(N)", "Raw" };
      queueConsoleMessageF("Output PID signal source: %s", sigNames[OutputPIDSigSrc]);
    }
    if (paramOk(request, "OutputPIDMA_N")) {
      foundParameter = true;
      inputMessage = request->getParam("OutputPIDMA_N")->value();
      OutputPIDMA_N = constrain(inputMessage.toInt(), 1, I_RING_SIZE);
      settingWrite(NK_OutputPIDMA_N, String(OutputPIDMA_N).c_str());
      queueConsoleMessageF("Output PID MA window: N=%d", OutputPIDMA_N);
    }
    if (paramOk(request, "OutputPIDFilterTC")) {
      foundParameter = true;
      inputMessage = request->getParam("OutputPIDFilterTC")->value();
      OutputPIDFilterTC = inputMessage.toFloat();
      settingWrite(NK_OutputPIDFilterTC, String(OutputPIDFilterTC).c_str());
      queueConsoleMessageF("Output PID EMA TC: %.1f ms", OutputPIDFilterTC);
    }
    if (paramOk(request, "VoltageFilterTC")) {
      foundParameter = true;
      inputMessage = request->getParam("VoltageFilterTC")->value();
      VoltageFilterTC = inputMessage.toFloat();
      settingWrite(NK_VoltageFilterTC, String(VoltageFilterTC).c_str());
      queueConsoleMessageF("Voltage EMA TC: %.1f ms", VoltageFilterTC);
    }
    if (paramOk(request, "TdPred")) {
      foundParameter = true;
      inputMessage = request->getParam("TdPred")->value();
      TdPred = constrain(inputMessage.toFloat(), 0.01f, 0.30f);
      settingWrite(NK_TdPred, String(TdPred, 3).c_str());
      queueConsoleMessageF("OV prediction horizon set to: %.3f s", TdPred);
    }
    if (paramOk(request, "OvMeasMarginV")) {
      foundParameter = true;
      inputMessage = request->getParam("OvMeasMarginV")->value();
      OvMeasMarginV = constrain(inputMessage.toFloat(), 0.020f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f), 2.000f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f));  // ceiling must match OvTierLoMarginV's: the LOW tier rides this line
      settingWrite(NK_OvMeasMarginV, String(OvMeasMarginV, 3).c_str());
      queueConsoleMessageF("Group 2 measured-voltage trigger margin set to: %.0f mV", OvMeasMarginV * 1000.0f);
    }
    if (paramOk(request, "OvPredMarginV")) {
      foundParameter = true;
      inputMessage = request->getParam("OvPredMarginV")->value();
      OvPredMarginV = constrain(inputMessage.toFloat(), 0.050f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f), 2.000f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f));
      settingWrite(NK_OvPredMarginV, String(OvPredMarginV, 3).c_str());
      queueConsoleMessageF("Group 1 prediction trigger margin set to: %.0f mV", OvPredMarginV * 1000.0f);
    }
    if (paramOk(request, "OvTierLoMarginV")) {
      foundParameter = true;
      inputMessage = request->getParam("OvTierLoMarginV")->value();
      OvTierLoMarginV = constrain(inputMessage.toFloat(), 0.020f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f), 2.000f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f));
      settingWrite(NK_OvTierLoMarginV, String(OvTierLoMarginV, 3).c_str());
      queueConsoleMessageF("Timed OV cut LOW-tier margin set to: %.0f mV above target", OvTierLoMarginV * 1000.0f);
    }
    if (paramOk(request, "OvTierLoDwellMs")) {
      foundParameter = true;
      inputMessage = request->getParam("OvTierLoDwellMs")->value();
      OvTierLoDwellMs = (uint32_t)constrain(inputMessage.toInt(), 0, 5000);
      settingWrite(NK_OvTierLoDwellMs, String(OvTierLoDwellMs).c_str());
      if (OvTierLoDwellMs == 0) queueConsoleMessage("Timed OV cut LOW tier DISABLED (dwell 0)");
      else queueConsoleMessageF("Timed OV cut LOW-tier dwell set to: %ums", OvTierLoDwellMs);
    }
    if (paramOk(request, "OvTierMidMarginV")) {
      foundParameter = true;
      inputMessage = request->getParam("OvTierMidMarginV")->value();
      OvTierMidMarginV = constrain(inputMessage.toFloat(), 0.020f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f), 2.000f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f));
      settingWrite(NK_OvTierMidMarginV, String(OvTierMidMarginV, 3).c_str());
      queueConsoleMessageF("Timed OV cut MID-tier margin set to: %.0f mV above target", OvTierMidMarginV * 1000.0f);
    }
    if (paramOk(request, "OvTierMidDwellMs")) {
      foundParameter = true;
      inputMessage = request->getParam("OvTierMidDwellMs")->value();
      OvTierMidDwellMs = (uint32_t)constrain(inputMessage.toInt(), 0, 5000);
      settingWrite(NK_OvTierMidDwellMs, String(OvTierMidDwellMs).c_str());
      if (OvTierMidDwellMs == 0) queueConsoleMessage("Timed OV cut MID tier DISABLED (dwell 0)");
      else queueConsoleMessageF("Timed OV cut MID-tier dwell set to: %ums", OvTierMidDwellMs);
    }
    if (paramOk(request, "DvdtTC")) {
      foundParameter = true;
      inputMessage = request->getParam("DvdtTC")->value();
      DvdtTC = constrain(inputMessage.toFloat(), 5.0f, 500.0f);
      settingWrite(NK_DvdtTC, String(DvdtTC, 1).c_str());
      queueConsoleMessageF("dvdt EMA TC set to: %.1f ms (dt-aware; alpha computed per-sample)", DvdtTC);
    }
    if (paramOk(request, "ReseedFrac")) {
      foundParameter = true;
      inputMessage = request->getParam("ReseedFrac")->value();
      ReseedFrac = clamp_f(inputMessage.toFloat(), 0.2f, 1.0f);  // at 1.0 first-fire de-escalation on a persistent cause is dead; UI max is 0.95
      settingWrite(NK_ReseedFrac, String(ReseedFrac, 2).c_str());
      queueConsoleMessageF("Recovery seed fraction set to: %.2f", ReseedFrac);
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    if (paramOk(request, "ReseedFracNoShunt")) {
      foundParameter = true;
      inputMessage = request->getParam("ReseedFracNoShunt")->value();
      ReseedFracNoShunt = clamp_f(inputMessage.toFloat(), 0.1f, 0.95f);
      settingWrite(NK_ReseedFracNS, String(ReseedFracNoShunt, 2).c_str());
      queueConsoleMessageF("No-shunt recovery seed fraction set to: %.2f", ReseedFracNoShunt);
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    if (paramOk(request, "CvRecovClimbRate")) {
      foundParameter = true;
      inputMessage = request->getParam("CvRecovClimbRate")->value();
      CvRecovClimbRate = clamp_f(inputMessage.toFloat(), 0.01f, 1.0f);
      settingWrite(NK_CvRecovClimb, String(CvRecovClimbRate, 2).c_str());
      queueConsoleMessageF("Recovery climb rate set to: %.2f of max/s (%.0f A/s here)", CvRecovClimbRate, CvRecovClimbRate * (float)MaxTableValue);
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    if (paramOk(request, "CVTuningMode")) {
      foundParameter = true;
      inputMessage = request->getParam("CVTuningMode")->value();
      int requested = inputMessage.toInt();
      // Mutex: refuse turn-on if another test is already running. All four tests must run independently.
      if (requested == 1 && CVTuningMode == 0) {
        const char *blocker = (systemIDActive != 0) ? "Plant Delay Test"
                              : (altSweepActive != 0 || altSweepRequested) ? "Gate-tuning field sweep"
                         : (chcActive != 0) ? "Charge health calibration"
                                                      : (TuningMode ? "Current tuning" : nullptr);
        if (blocker != nullptr) {
          queueConsoleMessageF("Voltage tuning: turn-on blocked — %s is active. Turn it off first.", blocker);
        } else {
          settingWrite(NK_CVTuningMode, inputMessage.c_str());
          CVTuningMode = requested;
        }
      } else {
        settingWrite(NK_CVTuningMode, inputMessage.c_str());
        CVTuningMode = requested;
      }
    }
    if (request->hasParam("cvTuningNote")) {
      foundParameter = true;   // transient (NOT persisted): free-text label for the next committed CV record
      sanitizeTuningNote(cvTuningNote, request->getParam("cvTuningNote")->value().c_str());
    }
    if (request->hasParam("commitCVTuningScore")) {
      foundParameter = true;
      manualCommitCVTuningRequested = true;
      queueConsoleMessage("CVTuningScore: manual commit requested via UI");
    }
    if (request->hasParam("restartCVTest")) {
      foundParameter = true;
      cvTuningScore = {};
      cvTuningParamChanged = false;
      queueConsoleMessage("CVTuningScore: test restarted via UI");
    }
    if (paramOk(request, "cvWaveAmplitudeV")) {
      foundParameter = true;
      inputMessage = request->getParam("cvWaveAmplitudeV")->value();
      cvWaveAmplitudeV = constrain(inputMessage.toFloat(), 0.05f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f), 2.0f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f));
      settingWrite(NK_cvWaveAmplitudeV, String(cvWaveAmplitudeV, 2).c_str());
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    if (paramOk(request, "cvWavePeriodSec")) {
      foundParameter = true;
      inputMessage = request->getParam("cvWavePeriodSec")->value();
      cvWavePeriodSec = inputMessage.toInt();
      settingWrite(NK_cvWavePeriodSec, String(cvWavePeriodSec).c_str());
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    if (paramOk(request, "cvKOvershoot")) {
      foundParameter = true;
      inputMessage = request->getParam("cvKOvershoot")->value();
      cvKOvershoot = inputMessage.toFloat();
      settingWrite(NK_cvKOvershoot, String(cvKOvershoot, 1).c_str());
    }
    if (paramOk(request, "cvConsecutiveReads")) {
      foundParameter = true;
      inputMessage = request->getParam("cvConsecutiveReads")->value();
      cvConsecutiveReads = (uint8_t)constrain(inputMessage.toInt(), 1, 20);
      settingWrite(NK_cvConsecutiveReads, String(cvConsecutiveReads).c_str());
    }
    if (paramOk(request, "VoltageDisagreeThreshold")) {
      foundParameter = true;
      inputMessage = request->getParam("VoltageDisagreeThreshold")->value();
      settingWrite(NK_VoltageDisagreeThreshold, inputMessage.c_str());
      VoltageDisagreeThreshold = inputMessage.toFloat();
      queueConsoleMessageF("Voltage disagreement threshold set to: %.2fV", VoltageDisagreeThreshold);
    }
    if (paramOk(request, "VoltageDisagreeTimeout")) {
      foundParameter = true;
      inputMessage = request->getParam("VoltageDisagreeTimeout")->value();
      int temp = inputMessage.toInt() * 1000;  // user enters seconds
      settingWrite(NK_VoltageDisagreeTimeout, String(temp).c_str());
      VoltageDisagreeTimeout = temp;
      queueConsoleMessageF("Voltage disagreement timeout set to: %d seconds", inputMessage.toInt());
    }
    if (paramOk(request, "VoltageLoopInterval")) {
      foundParameter = true;
      inputMessage = request->getParam("VoltageLoopInterval")->value();
      settingWrite(NK_VoltageLoopInterval, inputMessage.c_str());
      VoltageLoopInterval = inputMessage.toInt();
    }
    if (paramOk(request, "FIELD_COLLAPSE_DELAY")) {
      foundParameter = true;
      inputMessage = request->getParam("FIELD_COLLAPSE_DELAY")->value();
      int temp = inputMessage.toInt() * 1000;
      settingWrite(NK_FIELD_COLLAPSE_DELAY, String(temp).c_str());
      FIELD_COLLAPSE_DELAY = temp;
    }
    if (request->hasParam("ResetPerfCounters")) {
      foundParameter = true;
      // Function timing session worsts; mirrors the worstWindow reset block in loop(), keep the two in sync.
      ft_ReadAnalogInputs.worstSession = 0;
      ft_AdjustFieldLearnMode.worstSession = 0;
      ft_logDashboardValues.worstSession = 0;
      ft_updateSystemHealthStats.worstSession = 0;
      ft_checkWiFiConnection.worstSession = 0;
      ft_SendWifiData.worstSession = 0;
      ft_CheckAlarms.worstSession = 0;
      ft_calculateDerivedMetrics.worstSession = 0;
      ft_ch1_compute_stats.worstSession = 0;
      ft_uploadSensorHistory.worstSession = 0;
      ft_dumpLongTermRing.worstSession = 0;
      ft_fastAltDrain.worstSession = 0;
      ft_faMatrixFlush.worstSession = 0;
      ft_faDetector.worstSession = 0;
      ft_faWindowFinalize.worstSession = 0;
      ft_zeroLogService.worstSession = 0;
      ft_solarLedger.worstSession = 0;
      ft_bhFlushCapNVS.worstSession = 0;
      ft_kneeLearnService.worstSession = 0;
      // Not cleared here: the ripple analyzer's session worsts (faSes*) persist across reboot and are
      // cleared only by ResetRipplePeaks.
      ft_uploadBufferedRecords.worstSession = 0;
      ft_buildConfigPayload.worstSession = 0;
      ft_UpdateBatterySOC.worstSession = 0;
      ft_updateSensorWindow.worstSession = 0;
      ft_checkTimeSync.worstSession = 0;
      ft_rai_total.worstSession = 0;
      ft_rai_ina228.worstSession = 0;
      ft_rai_ads_state.worstSession = 0;
      ft_rai_bmp_state.worstSession = 0;
      ft_rai_imu.worstSession = 0;
      ft_updateAccelMetrics.worstSession = 0;
      ft_ReadVEData.worstSession = 0;
      ft_ReadNMEA0183.worstSession = 0;
      ft_altHealth.worstSession = 0;
      ft_altFold.worstSession = 0;
      ft_boatPerf.worstSession = 0;
      ft_altEngine.worstSession = 0;
      ft_perfEngine.worstSession = 0;
      altJobWorstUs = 0; altJobWorstSpanMs = 0; altJobWorstLookups = 0; perfEngWorstSecUs = 0;
      ft_n2kTx.worstSession = 0;
      ft_dvcc.worstSession = 0;
      ft_n2kParse.worstSession = 0;
      n2kTxCount = 0;
      n2kTxDropCount = 0;
      n2kRxFramesBase = tNMEA2000_esp32::BusStats.rxFrames;
      n183SentenceCount = 0;
      n183ChecksumErrCount = 0;
      ft_huntGov.worstSession = 0;
      VeTime2 = 0;
      cpuLoadCore0Max = 0;
      cpuLoadCore1Max = 0;
      // Session max loop time (this session only — last session is preserved)
      MaximumLoopTime = 0;
      // CH1 all-time accumulators
      ch1AtWorst = 0;
      ch1AtOver2x = 0;
      ch1AtSum = 0;
      ch1AtCount = 0;
      ch1_worst_at = 0;
      ch1_over2x_at = 0;
      ch1_avg_at = 0.0f;
      ch1_n_at = 0;
      ch1_worst_fieldon = 0;
      // CH1 10s ring buffer
      ch1Head = 0;
      ch1Count = 0;
      ch1_avg_10s = 0.0f;
      ch1_worst_10s = 0;
      ch1_over2x_10s = 0;
      ch1_n_10s = 0;
      // CH1 2m bucket ring
      ch1BktHead = 0;
      ch1BktCount = 0;
      ch1BktStart = millis();
      ch1_avg_2m = 0.0f;
      ch1_worst_2m = 0;
      ch1_over2x_2m = 0;
      ch1_n_2m = 0;
      // CH1 1s mini-buckets
      ch1Bkt1sCount = 0;
      ch1Bkt1sHead = 0;
      ch1Bkt1sCurrent = { 0, 0, 0, 0 };
      ch1Bkt1sStart = millis();
      // Reset interval baseline so first post-reset sample doesn't carry stale timestamp gap
      ch1HasPrev = false;
      // Inner Current PID firing-interval stats — clears all windows AND all-time accumulators
      pfAtWorst = 0;
      pfAtOver2x = 0;
      pfAtSum = 0;
      pfAtCount = 0;
      pf_worst_at = 0;
      pf_over2x_at = 0;
      pf_avg_at = 0.0f;
      pf_avg_10s = 0.0f;
      pf_worst_10s = 0;
      pf_over2x_10s = 0;
      pf_avg_2m = 0.0f;
      pf_worst_2m = 0;
      pf_over2x_2m = 0;
      pfBktHead = 0;
      pfBktCount = 0;
      pfBktStart = millis();
      pfBkt1sCount = 0;
      pfBkt1sHead = 0;
      pfBkt1sCurrent = { 0, 0, 0, 0 };
      pfBkt1sStart = millis();
      pfHasPrev = false;
      // INA228 interval stats: all windows AND all-time accumulators, else stale worst spikes survive.
      resetINA228AllStats();
      // CV voltage-loop firing-interval ladder — clears all windows AND all-time accumulators
      vlAtWorst = 0;
      vlAtOver2x = 0;
      vlAtSum = 0;
      vlAtCount = 0;
      vl_worst_at = 0;
      vl_over2x_at = 0;
      vl_avg_at = 0.0f;
      vl_avg_10s = 0.0f;
      vl_worst_10s = 0;
      vl_over2x_10s = 0;
      vl_avg_2m = 0.0f;
      vl_worst_2m = 0;
      vl_over2x_2m = 0;
      vlBktHead = 0;
      vlBktCount = 0;
      vlBktStart = millis();
      vlBkt1sCount = 0;
      vlBkt1sHead = 0;
      vlBkt1sCurrent = { 0, 0, 0, 0 };
      vlBkt1sStart = millis();
      vlHasPrev = false;
      // Not cleared here: lifetime nav/sailing records have their own resets; "Reset Peak Values" must never
      // nuke leaderboard records.
      // 80MHz low-power loop instrumentation — clear session worst + near-miss counters
      loopWorst80Ses = 0;
      loopOver80ImuLimitCount = 0;
      loop80IterCount = 0;
      // field-ON loop instrumentation — clear session worst + its blame snapshot (stale blame
      // would describe a worst that no longer exists)
      loopFieldOnSes = 0;
      for (int bs = 0; bs < 3; bs++) { loopBlameIdx[bs] = 255; loopBlameUs[bs] = 0; }
      // I2C bus-health — clear bus-only worst-timers and stall/error counts for a fresh window
      inaBusReadWorstUs = 0;
      inaBusSlowCount = 0;
      ina228ErrorCount = 0;
      adsI2CErrorCount = 0;
      imuFifoFetchWorstUs = 0;
      imuFifoWorstSamples = 0;
      // ADS slow-channel gap meters — clear worst (keep last; prev re-seeds on next read)
      ch0GapWorstMs = 0;
      ch2GapWorstMs = 0;
      ch0GapFieldOnWorstMs = 0;
      ch2GapFieldOnWorstMs = 0;
      // CSV2 build/send cost — clear worsts
      csv2BuildWorstUs = 0;
      csv2SendWorstUs = 0;
      // AdjustField section profiler — clear the worst-full-pass latch + breakdown (/debug)
      aflWorstTotalUs = 0;
      memset(aflWorstSecUs, 0, sizeof(aflWorstSecUs));
      g_fieldDerateWorstUs = 0;
      // Worst-loop attribution diagnostic — re-arm so the next worst pass prints to Console
      loopDiagWorstUs = 0;
      // Fault-detector overall compute time — clear so /debug tracks worst since-reset.
      faDetWorstComputeUs = 0;
      faDetLastComputeUs = 0;
      // Core-0 HTTPS task cloud-op time — clear last + worst-since-reset
      httpsUploadWorstMs = 0;
      httpsUploadLastMs = 0;
      // NVS full-save diagnostics: worst/count/last track since-reset, not since-boot.
      nvsFullSaveWorstMs = 0;
      nvsFullSaveCount = 0;
      nvsFullSaveLastMs = 0;
      // "Never saved this boot" sentinel: "Time Since Last Save" stays 0 until the next field-off save,
      // a live is-the-save-firing indicator.
      lastNVSSaveTime = 0;
      // Dashboard-stream backpressure witnesses (/debug line + stall console lines)
      sseQueuePeak = 0;
      sseQueueDeepestPeak = 0;
      sseStallCount = 0;
      sseStallLongestMs = 0;
      sseFramesDropped = 0;
      csv3PeakLen = 0;
      gxFramesSent = 0;
      gxFramesHeld = 0;
      gxFramesTorn = 0;
      gxFramesDropped = 0;
      gxStreamDrops = 0;
      // INA228 die-temperature session max — same "since last reset" semantics as the rest of this block.
      inaDieTempMaxF = NAN;
      // Stamp the reset moment. Dashboard reads CSV1 slot 28 = (millis()-this)/1000
      // and formats it as "last 12 min" / "last 1.4 hr" on all .session-window-label spans.
      perfCountersResetMs = millis();
      queueConsoleMessage("Peak counters reset from web interface");
    }

    if (request->hasParam("forceCloudFlush")) {
      foundParameter = true;
      // "Upload Cloud Now": shortens the field-off settle to fieldCutSettled's 20 s floor and bypasses the
      // 13 s throttle; loop()'s cloud block drains the PSRAM ring and clears the flag once it is empty.
      forceCloudFlushPending = true;
      queueConsoleMessage(gpio4IsLow ? "Cloud sync: forced flush requested"
                                     : "Cloud sync: forced flush requested - it starts 20 s after the field turns off");
    }

    if (request->hasParam("ResetAccelSession")) {
      foundParameter = true;
      imu_total_samples_accel      = 0;
      imu_total_samples_gyro       = 0;
      imuRingBuffer->accel_dropped = 0;
      imuRingBuffer->gyro_dropped  = 0;
      imu_fifo_overrun_count       = 0;
      imu_i2c_error_count          = 0;
      imu_unknown_tag_count        = 0;
      imu_slam_count               = 0;
      imu_slam_peak_max            = 0;
      imuWindow->slam_count        = 0;
      imuWindow->slam_peak_max     = 0;
      queueConsoleMessage("Accel session stats reset from web interface");
    }

    if (request->hasParam("ResetAccelLifetime")) {
      foundParameter = true;
      imu_heel_max_lifetime        = 0;
      imu_pitch_max_lifetime       = 0;
      imu_slam_peak_lifetime       = 0;
      imu_slam_count_lifetime      = 0;
      imu_capsize_count            = 0;
      imu_pitchpole_count          = 0;
      // Reset shadow vars so the next saveNVSDataFull() sees a change and writes 0s to NVS
      prev_imu_heel_max_lifetime   = -1;
      prev_imu_pitch_max_lifetime  = -1;
      prev_imu_slam_peak_lifetime  = -1;
      prev_imu_slam_count_lifetime = UINT32_MAX;
      prev_imu_capsize_count       = UINT32_MAX;
      prev_imu_pitchpole_count     = UINT32_MAX;
      nvsPersistNow = true;   // commit the cleared IMU lifetime stats now, not at the next field-off edge.
      queueConsoleMessage("Accel lifetime stats reset from web interface");
    }

    if (nvsPersistNow) saveNVSDataFull();   // one save per request; else a reboot before the field-off edge reverts these
    // The wizard's 2 s keep-alive ping never sets foundParameter, so it does not fire the CSV3 echo; a
    // heartbeat riding along with a real setting still echoes.
    if (foundParameter) {
      stateRevision++;
      settingsDirty = true; // trigger immediate CSV3 settings echo
    }
    if (!foundParameter) {
      inputMessage = heartbeat ? "heartbeat" : "No message sent, the request_hasParam found no match";
    }
    request->send(200, "text/plain", inputMessage);
  });

  // Settings arm gate: ?arm=1 opens the write window, ?arm=0 closes it, no param reports state (the dashboard
  // polls it so every tab stays honest). Nothing times out; see settingsArmActive(). With pwRequired, arm=1 is
  // a POST carrying pw in the body (never a URL) and answers tok= for every mutating request; arm=0 needs a
  // live tok; "armed" in the reply is THIS client's standing, not the global flag.
  server.on("/armSettings", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request) {
    int code = 200;
    const char *issued = nullptr;
    uint32_t waitS = 0;
    const bool armInBody = request->hasParam("arm", true);
    // A GX must never arm or lock the phones' shared unlock.
    if (gxFlagged(request)) {
      request->send(400, "application/json", "{\"err\":\"gx\"}");
      return;
    }
    if (armInBody || request->hasParam("arm")) {
      bool arm = (armInBody ? request->getParam("arm", true) : request->getParam("arm"))->value().toInt() != 0;
      if (arm && !pwRequired) {
        settingsArmed = true;
        queueConsoleMessage("Settings ARMED: changes accepted until locked or rebooted");
      } else if (arm) {
        uint32_t now = millis();
        if (pwLockUntilMs && (int32_t)(pwLockUntilMs - now) > 0) {
          code = 429;
          waitS = (pwLockUntilMs - now + 999) / 1000;
        } else {
          const char *pw = request->hasParam("pw", true) ? request->getParam("pw", true)->value().c_str() : nullptr;
          if (pwVerify(pw)) {
            pwFailCount = 0;
            pwLockUntilMs = 0;
            settingsArmed = true;
            issued = armTokenIssue();
            queueConsoleMessageF("Settings ARMED with password from %s: changes accepted until locked or rebooted",
                                 request->client()->remoteIP().toString().c_str());
          } else {
            code = 403;
            // Only a presented wrong password is a strike: a request without one is the app learning that
            // a password exists (or a stale bundle), not a guess.
            if (pw && *pw) {
              if (++pwFailCount >= 5) {
                pwLockUntilMs = now + 60000UL;
                pwFailCount = 0;
              }
              queueConsoleMessageF("Unlock Controls REFUSED: wrong password from %s", request->client()->remoteIP().toString().c_str());
            }
          }
        }
      } else if (settingsArmed) {
        if (pwRequired && !armTokenValid(request)) {
          code = 403;
        } else {
          settingsArmed = false;
          armTokensClear();
          queueConsoleMessage("Settings locked");
        }
      }
    }
    String out = "{\"armed\":";
    out += (issued || settingsArmActive(request)) ? "1" : "0";
    out += ",\"pw\":";
    out += pwRequired ? "1" : "0";
    if (issued) {
      out += ",\"tok\":\"";
      out += issued;
      out += '"';
    }
    if (code == 403) out += ",\"err\":\"password\"";
    if (code == 429) {
      out += ",\"err\":\"locked\",\"wait\":";
      out += String(waitS);
    }
    out += '}';
    request->send(code, "application/json", out);
  });

  // Settings password: arm-gated, and a change or turn-off also needs the current password.
  // POST body only: the password never rides in a URL a log or a proxy could keep.
  server.on("/setPassword", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (refuseMisaddressed(request)) return;
    if (!settingsArmActive(request)) {
      request->send(403, "application/json", "{\"ok\":0,\"err\":\"armed\"}");
      return;
    }
    String cur = request->hasParam("cur", true) ? request->getParam("cur", true)->value() : String();
    if (pwRequired && !pwVerify(cur.c_str())) {
      request->send(403, "application/json", "{\"ok\":0,\"err\":\"password\"}");
      return;
    }
    if (request->hasParam("off", true) && request->getParam("off", true)->value().toInt() == 1) {
      pwClear();
      queueConsoleMessage("Settings password turned OFF");
      request->send(200, "application/json", "{\"ok\":1,\"pw\":0}");
      return;
    }
    String nw = request->hasParam("new", true) ? request->getParam("new", true)->value() : String();
    nw.trim();
    if (nw.length() < 4 || nw.length() > 32) {
      request->send(400, "application/json", "{\"ok\":0,\"err\":\"length\"}");
      return;
    }
    if (!pwStore(nw.c_str())) {
      request->send(500, "application/json", "{\"ok\":0,\"err\":\"store\"}");
      return;
    }
    queueConsoleMessage("Settings password set: Unlock Controls now asks for it");
    String out = "{\"ok\":1,\"pw\":1,\"tok\":\"";
    out += armTokenIssue();   // pwStore retired every token; this client stays unlocked
    out += "\"}";
    request->send(200, "application/json", out);
  });

  // The boat's other regulators (Setup > System > Regulators on this boat). The dashboard writes
  // both units, so each side is a plain arm-gated store of the other's uid and name.
  server.on("/pair", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!settingsArmActive(request)) {
      request->send(403, "text/plain", "Settings not armed");
      return;
    }
    String uid = request->hasParam("uid") ? request->getParam("uid")->value() : String();
    String name = request->hasParam("name") ? request->getParam("name")->value() : String();
    if (!pairListAdd(uid.c_str(), name.c_str())) {
      request->send(400, "text/plain", "Not a regulator id, this unit, or the list is full");
      return;
    }
    queueConsoleMessageF("Paired with regulator %s", name.length() ? name.c_str() : uid.c_str());
    String out = "{\"ok\":1,\"paired\":";
    pairListAppendJson(out);
    out += '}';
    request->send(200, "application/json", out);
  });
  server.on("/unpair", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!settingsArmActive(request)) {
      request->send(403, "text/plain", "Settings not armed");
      return;
    }
    String uid = request->hasParam("uid") ? request->getParam("uid")->value() : String();
    pairListRemove(uid.c_str());
    String out = "{\"ok\":1,\"paired\":";
    pairListAppendJson(out);
    out += '}';
    request->send(200, "application/json", out);
  });

  // GX schedule: fast=1 holds the engine-running schedule for 60 s whatever the engine does (the GX repeats it while a
  // page shows live data), fast=2 that and CSV1 at the web app's rate (gxIv), fast=0 releases it; flow=1 / flow=0 the
  // same for CSV4 alone at 1 s (the power-flow tile). Firmware before fast=2 reads it as 1. Cadence only; the GX still
  // never counts as a watching dashboard.
  server.on("/gxrate", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (refuseMisaddressed(request)) return;
    const String fast = request->hasParam("fast") ? request->getParam("fast")->value() : String();
    if (fast.length()) {
      const long level = fast.toInt();
      gxFastHold(level == 0 ? 0 : level >= 2 ? 2 : 1);
    }
    const String flow = request->hasParam("flow") ? request->getParam("flow")->value() : String();
    if (flow.length()) gxFlowHold(flow.toInt() != 0);
    char rate[GX_RATE_JSON_SIZE];
    gxRateJson(rate, sizeof(rate));
    request->send(200, "application/json", rate);
  });

  // Setup > System > Victron GX: the GX streams connected now (address, seconds up). Read-only.
  server.on("/gxStatus", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (refuseMisaddressed(request)) return;
    String out;
    gxStatusAppendJson(out, millis());
    request->send(200, "application/json", out);
  });

  // DVCC over WiFi: the GX bridge forwards what systemcalc writes to its alternator service, every ~3 s. Contract:
  // claude_docs/firmware-internals.md, "Victron GX stream". Not a settings write: no NVS, no console line, no CSV3 echo.
  server.on("/gxLimits", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (refuseMisaddressed(request)) return;
    if (!gxFlagged(request)) {
      request->send(400, "text/plain", "gx=1 required");
      return;
    }
    float cvl, ccl;
    if (!gxLimitParam(request, "cvl", cvl) || !gxLimitParam(request, "ccl", ccl)) {
      request->send(400, "text/plain", "cvl and ccl must be numbers");
      return;
    }
    if (!demoSeal) {   // demo mode follows its own made-up GX limit (9_demo.ino); a real GX post is answered, not taken
      portENTER_CRITICAL(&dvccWifiMux);
      dvccWifiBoxCvl = cvl;
      dvccWifiBoxCcl = ccl;
      dvccWifiBoxSeq = dvccWifiBoxSeq + 1;
      portEXIT_CRITICAL(&dvccWifiMux);
    }
    char out[64];
    snprintf(out, sizeof(out), "{\"ok\":1,\"follow\":%d,\"source\":%d,\"state\":%u}", dvccEn, dvccSrcType,
             (unsigned)dvccState);
    request->send(200, "application/json", out);
  });

  // The GX's device readings for the web app's Live Data > Integrations, posted by the GX bridge (GX link 4). Contract:
  // claude_docs/firmware-internals.md, "Victron GX stream". RAM only, demo mode included; never the control loop.
  server.on("/gxDevices", HTTP_POST, [](AsyncWebServerRequest *request) { gxDevicesHandle(request); });

  // Explicit routes for static assets, never via onNotFound. The flash fallback carries no-cache: for index.html
  // so a stale dashboard can't outlive an unreachable device, for the rest because this path has no ETag or
  // Last-Modified validator, so browsers refetch anyway.
  auto sendAssetFromFlash = [](AsyncWebServerRequest *request, const char *path, const char *type) {
    AsyncWebServerResponse *resp = request->beginResponse(webFS, path, type);
    resp->addHeader("Cache-Control", "no-cache");
    request->send(resp);
  };
  auto sendIndexFromFlash = [sendAssetFromFlash](AsyncWebServerRequest *request) {
    sendAssetFromFlash(request, "/index.html", "text/html");
  };
  server.on("/", HTTP_GET, [sendIndexFromFlash](AsyncWebServerRequest *request) {
    if (!serveCachedAsset(request, "/index.html", "text/html"))
      sendIndexFromFlash(request);
  });
  server.on("/index.html", HTTP_GET, [sendIndexFromFlash](AsyncWebServerRequest *request) {
    if (!serveCachedAsset(request, "/index.html", "text/html"))
      sendIndexFromFlash(request);
  });
  server.on("/styles.css", HTTP_GET, [sendAssetFromFlash](AsyncWebServerRequest *request) {
    if (!serveCachedAsset(request, "/styles.css", "text/css"))
      sendAssetFromFlash(request, "/styles.css", "text/css");
  });
  server.on("/uPlot.min.css", HTTP_GET, [sendAssetFromFlash](AsyncWebServerRequest *request) {
    if (!serveCachedAsset(request, "/uPlot.min.css", "text/css"))
      sendAssetFromFlash(request, "/uPlot.min.css", "text/css");
  });
  server.on("/uPlot.iife.min.js", HTTP_GET, [sendAssetFromFlash](AsyncWebServerRequest *request) {
    if (!serveCachedAsset(request, "/uPlot.iife.min.js", "application/javascript"))
      sendAssetFromFlash(request, "/uPlot.iife.min.js", "application/javascript");
  });
  server.on("/script.js", HTTP_GET, [sendAssetFromFlash](AsyncWebServerRequest *request) {
    if (!serveCachedAsset(request, "/script.js", "application/javascript"))
      sendAssetFromFlash(request, "/script.js", "application/javascript");
  });
  server.onNotFound([](AsyncWebServerRequest *request) {
    if (currentMode == MODE_CONFIG) {
      sendWifiConfigPortal(request);  // provisioning: captive probe → WiFi-setup page (correct for new users)
    } else if (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) {
      // Operational AP: every OS connectivity probe must read "internet is fine" so no captive sheet
      // auto-pops. Android/ChromeOS need 204 + EMPTY body (a 200 with a body = "sign in", and Android
      // may then drop the AP); Windows needs those literal strings; Apple needs 200 with "Success".
      const String &probe = request->url();
      if (probe == "/generate_204" || probe == "/gen_204") {
        request->send(204);
      } else if (probe == "/connecttest.txt") {
        request->send(200, "text/plain", "Microsoft Connect Test");
      } else if (probe == "/ncsi.txt") {
        request->send(200, "text/plain", "Microsoft NCSI");
      } else {
        request->send(200, "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
      }
    } else {
      request->send(404, "text/plain", "Not Found");
    }
  });
  // Setup event source for real-time updates
  events.onConnect([](AsyncEventSourceClient *client) {
    if (client->lastId()) {
      Serial.printf("Client reconnected! Last message ID that it got is: %u\n", client->lastId());
    }
    settingsDirty = true;  // send CSV3 immediately so new client gets current settings
    gxDevKick = true;      // and the GX's device readings, which otherwise go out only when the GX posts
    csv2Kick = true;       // and the live state (CSV2) and sensor ages (TS), not on their 5 s / 3 s schedule
    tsKick = true;
    // AsyncTCP's ack timeout closes an event-source client outright (AsyncEventSourceClient::_onTimeout),
    // counted from the last write that got out, so at the 5 s default a radio stall destroys the stream.
    // 20 s rides out phone-hotspot stalls; a client that vanishes without a FIN holds its slot 20 s.
    // The _xeng fork closes a client that acks but stops reading (zero window) on the same limit (_onPoll).
    if (client->client()) client->client()->setAckTimeout(20000);
    client->send("hello!", NULL, millis(), 10000);
  });
  // Victron GX stream: same frames on the GX schedule (gxSendTask). Its own source, so a connected GX never counts
  // as a phone watching (TempTask cadence, the nap clock, the maintenance-reboot window, the phone backlog witness).
  gxEvents.onConnect([](AsyncEventSourceClient *client) {
    settingsDirty = true;   // CSV3 at once: the GX needs every settings echo before its first write
    if (client->client()) client->client()->setAckTimeout(20000);
    client->send("hello!", NULL, millis(), 10000);
    char rate[GX_RATE_JSON_SIZE];
    gxRateJson(rate, sizeof(rate));
    client->send(rate, "GxRate");
    gxKickPending = true;
    gxStreamAdd(client, client->client() ? client->client()->remoteIP().toString().c_str() : "");
    gxStreamCount = (uint8_t)(gxStreamCount + 1);
  });
  gxEvents.onDisconnect([](AsyncEventSourceClient *client) {
    gxStreamRemove(client);
    if (gxStreamCount) gxStreamCount = (uint8_t)(gxStreamCount - 1);
    gxStreamDrops++;
  });
  // MUST precede "/debug": a plain-string URI matches exact-or-prefix-with-slash and the server
  // dispatches in registration order, so "/debug" registered first swallows all of these.
  // Bench-only data-growth ceiling test; handlers live at the tail of 8_functions.ino.
  { void debugFillMax(AsyncWebServerRequest *); void debugClearMax(AsyncWebServerRequest *);
    void debugFrontCheck(AsyncWebServerRequest *);
    server.on("/debug/fillmax",  HTTP_GET, [](AsyncWebServerRequest *r) { if (demoSeal) demoRefuse(r); else debugFillMax(r); });
    server.on("/debug/clearmax", HTTP_GET, [](AsyncWebServerRequest *r) { if (demoSeal) demoRefuse(r); else debugClearMax(r); });
    server.on("/debug/frontcheck", HTTP_GET, [](AsyncWebServerRequest *r) { if (demoSeal) demoRefuse(r); else debugFrontCheck(r); }); }

  // Diagnostic endpoint to check partition and version
  server.on("/debug", HTTP_GET, [](AsyncWebServerRequest *request) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    const char *timeSrcName = (currentTimeSource == TIME_GPS)   ? "NMEA-GPS"
                            : (currentTimeSource == TIME_PHONE) ? "Phone"
                            : (currentTimeSource == TIME_NTP)   ? "NTP"
                            : (currentTimeSource == TIME_MILLIS) ? "drifting"
                            : (currentTimeSource == TIME_ESTIMATED) ? "estimated" : "none";
    const char *gpsSrcName  = (currentGpsSource == GPS_NMEA)   ? "NMEA"
                            : (currentGpsSource == GPS_PHONE)  ? "Phone"
                            : (currentGpsSource == GPS_MANUAL) ? "Manual" : "none";
    const char *spdSrcName  = (currentSpeedSource == GPS_NMEA)  ? "NMEA"
                            : (currentSpeedSource == GPS_PHONE) ? "Phone" : "none";
    unsigned long now = millis();
    // Heap walks and the task census are cpuLoadTask's field-cut cache: a live largest-block walk holds a heap lock, and
    // uxTaskGetSystemState / xTaskGetHandle hold the kernel lock, which stalls Core 1. Stamps can postdate `now`.
    auto ageS = [now](uint32_t at) -> unsigned long {
      return ((int32_t)((uint32_t)now - at) > 0) ? ((uint32_t)now - at) / 1000UL : 0UL;
    };
    const TaskCensus *cen = (taskCensus && taskCensusAtMs) ? &taskCensus[taskCensusPub] : nullptr;
    taskCensusWanted = true;   // the next field-cut window takes a fresh one; reload /debug to see it
    // Where the network tasks landed: async_tcp reads 0 only if this image has the Core-0 pin (compile-time
    // default; a stale flash won't). 2147483647 = tskNO_AFFINITY (floating), -99 = not found.
    int asyncTcpCore = -99, lwipCore = -99;
    for (uint16_t i = 0; cen && i < cen->n; i++) {
      if (strcmp(cen->row[i].name, "async_tcp") == 0) asyncTcpCore = (int)cen->row[i].core;
      else if (strcmp(cen->row[i].name, "tiT") == 0) lwipCore = (int)cen->row[i].core;
    }
    const char *faStateName = (faChanState == 1) ? "live" : (faChanState == 2) ? "RAILED-dormant (jumper open?)" : "off";
    // "PSRAM" = setup()'s mbedTLS calloc override is registered; the counts show where real TLS allocations landed.
    const char *tlsMem = mbedtlsPsramHooked ? "PSRAM" : "INTERNAL (override not registered)";
    char out[4200];
    int dpos = snprintf(out, sizeof(out),
             "Partition: %s\nVersion: %s\nFree heap: %lu\n"
             "Web FS %s: %d/%d KB used (%d%%) - boot seed, -1 = unread\n"
             "TLS buffers -> %s (mbedTLS allocations since boot: PSRAM %lu, internal fallback %lu, failed %lu)\n"
             "Heap walks (Core 0, field cut only): largest internal block %u KB %lus ago, largest PSRAM block %u KB %lus ago; free PSRAM %u B\n"
             "Net task cores (0/1=pinned, 2147483647=floating, -99=not found or no census yet): async_tcp=%d lwIP=%d\n"
             "Dashboard stream: clients=%u queue avg=%u deepest=%u peak avg=%u deepest=%u cap=%u stalls=%lu longest=%.1fs dropped=%lu csv3=%uB peak=%uB of %uB\n"
             "GX stream: clients=%u queue=%u schedule=%s%s sender=%s sent=%lu held=%lu torn=%lu dropped=%lu disconnects=%lu\n"
             "AdjustField worst full pass (ms): total=%.1f | thermal=%.1f snapshot=%.1f fastov=%.1f modes=%.1f control=%.1f duty=%.1f tail=%.1f\n"
             "Field derate worst pass: %lu us (budget 500 us; part of snapshot above)\n"
             "Time source: %s (NMEA last sync: %lus ago, Phone last: %lus ago)\n"
             "GPS source:  %s (NMEA last fix: %lus ago, Phone last: %lus ago)\n"
             "Speed source: %s (NMEA last SOG: %lus ago, Phone last: %lus ago)\n"
             "Lat/Lon: %.6f, %.6f\n"
             "FastAlt: %s range=%sdB windows ok=%lu disc=%lu matrixCells=%u sesPkpk=%.1fA sesPeak=%.1fA@%.0fHz\n",
             (running && running->label) ? running->label : "unknown",
             FIRMWARE_VERSION,
             (unsigned long)ESP.getFreeHeap(),
             WebFsPartLabel, WebFsUsedKb, WebFsTotalKb,
             (WebFsTotalKb > 0) ? (100 * WebFsUsedKb / WebFsTotalKb) : -1,
             tlsMem, (unsigned long)tlsAllocPsram, (unsigned long)tlsAllocInternal, (unsigned long)tlsAllocFailed,
             (unsigned)LargestInternalBlock, ageS(largestInternalAtMs),
             (unsigned)LargestPSRAMBlock, ageS(largestPsramAtMs),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             asyncTcpCore, lwipCore,
             (unsigned)events.count(), (unsigned)sseQueueDepth, (unsigned)sseQueueDeepest,
             (unsigned)sseQueuePeak, (unsigned)sseQueueDeepestPeak, (unsigned)SSE_MAX_QUEUED_MESSAGES,
             (unsigned long)sseStallCount, sseStallLongestMs / 1000.0f, (unsigned long)sseFramesDropped,
             (unsigned)csv3LastLen, (unsigned)csv3PeakLen, (unsigned)PAYLOAD3_SIZE,
             (unsigned)gxEvents.count(), (unsigned)gxEvents.avgPacketsWaiting(), GX_TIER_NAME[gxTierEffective(now)],
             gxFastLevel(now) == 2 ? " (live hold)" : gxFastActive(now) ? " (fast hold)" : "", gxSendTaskHandle ? "running" : "NOT RUNNING",
             (unsigned long)gxFramesSent, (unsigned long)gxFramesHeld, (unsigned long)gxFramesTorn,
             (unsigned long)gxFramesDropped, (unsigned long)gxStreamDrops,
             aflWorstTotalUs / 1000.0f,
             aflWorstSecUs[0] / 1000.0f, aflWorstSecUs[1] / 1000.0f, aflWorstSecUs[2] / 1000.0f,
             aflWorstSecUs[3] / 1000.0f, aflWorstSecUs[4] / 1000.0f, aflWorstSecUs[5] / 1000.0f,
             aflWorstSecUs[6] / 1000.0f,
             (unsigned long)g_fieldDerateWorstUs,
             timeSrcName,
             lastNmea2kSystemTimeMs ? (now - lastNmea2kSystemTimeMs) / 1000UL : 0UL,
             lastPhoneTimeMs        ? (now - lastPhoneTimeMs)        / 1000UL : 0UL,
             gpsSrcName,
             lastNmea2kGnssMs       ? (now - lastNmea2kGnssMs)       / 1000UL : 0UL,
             lastPhoneGpsMs         ? (now - lastPhoneGpsMs)         / 1000UL : 0UL,
             spdSrcName,
             lastNmea2kSogMs        ? (now - lastNmea2kSogMs)        / 1000UL : 0UL,
             lastPhoneSpdMs         ? (now - lastPhoneSpdMs)         / 1000UL : 0UL,
             LatitudeNMEA, LongitudeNMEA,
             faStateName, faAttenIs12 ? "12" : "6",
             (unsigned long)faWindowsAccepted, (unsigned long)faWindowsDiscarded,
             (unsigned)faCellsUsed, faSesPkpkWorstA, faSesPeakWorstA, faSesPeakWorstHz);
    if (dpos > 0 && dpos < (int)sizeof(out) - 64) {
      int n = frontEngineDebugLine(out + dpos, sizeof(out) - dpos);
      if (n > 0) dpos += n;
      if (dpos > (int)sizeof(out) - 1) dpos = (int)sizeof(out) - 1;
    }
    // Floating-task census: a core=FLOAT (tskNO_AFFINITY) task above the priority-1 loop task can land on
    // Core 1 and stall it.
    if (dpos > 0 && dpos < (int)sizeof(out) - 64) {
      if (!cen) {
        dpos += snprintf(out + dpos, sizeof(out) - dpos, "Tasks: no census yet (taken once the field has been cut 20 s)\n");
      } else {
        // Per-task CPU share = run-time counter delta against the previous census (the other slot) over the wall time
        // between the two. That slot is intact here: the writer only fills it during the next census, which runs with
        // Core 0's scheduler suspended, so this handler (async_tcp, Core 0) never reads a half-written one. The 32-bit
        // counters wrap at 71.6 min, so shares are shown only for a shorter interval.
        const TaskCensus *prev = &taskCensus[taskCensusPub ^ 1];
        const bool haveShares = prev->atUs != 0 && prev->n > 0 && cen->atUs > prev->atUs
                                && (cen->atUs - prev->atUs) < 4000000000LL;
        const double spanUs = haveShares ? (double)(cen->atUs - prev->atUs) : 0.0;
        if (haveShares)
          dpos += snprintf(out + dpos, sizeof(out) - dpos,
                           "Tasks (name prio core stackHWM cpu%% of its core over the %lus before the census), census %lus ago with the field cut, next one at the next field-cut window%s:",
                           (unsigned long)(spanUs / 1e6), ageS(cen->atMs), cen->n ? "" : " - none listed, more than MAX_TASKS tasks?");
        else
          dpos += snprintf(out + dpos, sizeof(out) - dpos,
                           "Tasks (name prio core stackHWM; cpu%% appears from the second census on), census %lus ago with the field cut, next one at the next field-cut window%s:",
                           ageS(cen->atMs), cen->n ? "" : " - none listed, more than MAX_TASKS tasks?");
        for (uint16_t i = 0; i < cen->n && dpos < (int)sizeof(out) - 64; i++) {
          const TaskCensusRow &r = cen->row[i];
          int core = (int)r.core;
          char cpuS[14] = "";
          if (haveShares) {
            for (uint16_t k = 0; k < prev->n; k++) {
              const TaskCensusRow &p = prev->row[k];
              if (p.handle != r.handle || strcmp(p.name, r.name) != 0) continue;
              double pct = 100.0 * (double)(uint32_t)(r.runUs - p.runUs) / spanUs;
              if (pct > 100.0) strncpy(cpuS, " cpu?", sizeof(cpuS));   // same handle and name, fresh counter: the task was recreated
              else snprintf(cpuS, sizeof(cpuS), " cpu%.1f%%", pct);
              break;
            }
          }
          dpos += snprintf(out + dpos, sizeof(out) - dpos, " [%s p%u c%s hwm%u%s]",
                           r.name,
                           (unsigned)r.prio,
                           core == 2147483647 ? "FLOAT" : (core == 0 ? "0" : "1"),
                           (unsigned)r.hwm, cpuS);
        }
        if (dpos < (int)sizeof(out)) snprintf(out + dpos, sizeof(out) - dpos, "\n");
      }
    }
    request->send(200, "text/plain", out);
  });

  // Phone GPS + time backup: browser and Capacitor app GET this every ~30-60 s with a fix; partial submissions
  // are OK. Feeds the NMEA -> Phone -> NTP chain via the *Phone globals and lastPhone*Ms stamps.
  server.on("/set_phone_data", HTTP_GET, [](AsyncWebServerRequest *request) {
    // Deliberately NOT arm-gated: background telemetry from a trusted LAN client, pushed whether or not
    // settings are armed. NMEA always outranks phone data, and range checks reject garbage.
    bool acceptedGps = false, acceptedTime = false;
    if (!demoSeal && paramOk(request, "lat") && paramOk(request, "lon")) {   // demo: the simulated boat is the only position, speed and course
      // Use strtod + endptr (not String::toDouble — that silently returns 0.0
      // for unparseable input, so "lat=NaN&lon=42" would pass as (0.0, 42)).
      const char *latStr = request->getParam("lat")->value().c_str();
      const char *lonStr = request->getParam("lon")->value().c_str();
      char *latEnd = nullptr, *lonEnd = nullptr;
      double lat = strtod(latStr, &latEnd);
      double lon = strtod(lonStr, &lonEnd);
      bool latOk = (latEnd != latStr) && (*latEnd == '\0' || *latEnd == ' ');
      bool lonOk = (lonEnd != lonStr) && (*lonEnd == '\0' || *lonEnd == ' ');
      if (latOk && lonOk &&
          !isnan(lat) && !isnan(lon) &&
          fabs(lat) <= 90.0 && fabs(lon) <= 180.0 &&
          !(lat == 0.0 && lon == 0.0)) {
        LatitudePhone  = lat;
        LongitudePhone = lon;
        lastPhoneGpsMs = millis();
        acceptedGps = true;
      }
    }
    if (request->hasParam("epochMs")) {
      // Date.now() ms -> seconds like the GPS/NTP path; reject anything before 2020-01-01.
      uint64_t ms = (uint64_t)strtoull(request->getParam("epochMs")->value().c_str(), nullptr, 10);
      time_t sec = (time_t)(ms / 1000ULL);
      if (sec > 1577836800) {
        PhoneTimeEpoch  = sec;
        lastPhoneTimeMs = millis();
        acceptedTime = true;
      }
    }
    // Phone GNSS speed (m/s, coords.speed) and course (deg, coords.heading), independent of lat/lon:
    // heading is null on a stationary phone, so each gets its own freshness stamp.
    bool acceptedSpd = false, acceptedHdg = false;
    if (!demoSeal && paramOk(request, "spd")) {
      const char *spdStr = request->getParam("spd")->value().c_str();
      char *spdEnd = nullptr;
      double spd = strtod(spdStr, &spdEnd);
      // 77 m/s ≈ 150 kt — same ceiling as the odometer's implied-speed teleport gate.
      if (spdEnd != spdStr && *spdEnd == '\0' && !isnan(spd) && spd >= 0.0 && spd <= 77.0) {
        SpeedPhone = (float)(spd * 1.94384);  // m/s → knots, matching the NMEA path
        lastPhoneSpdMs = millis();
        acceptedSpd = true;
      }
    }
    if (!demoSeal && paramOk(request, "hdg")) {
      const char *hdgStr = request->getParam("hdg")->value().c_str();
      char *hdgEnd = nullptr;
      double hdg = strtod(hdgStr, &hdgEnd);
      if (hdgEnd != hdgStr && *hdgEnd == '\0' && !isnan(hdg) && hdg >= 0.0 && hdg <= 360.0) {
        HeadingPhone = (float)((hdg >= 360.0) ? 0.0 : hdg);
        lastPhoneHdgMs = millis();
        acceptedHdg = true;
      }
    }
    // GPS/time are no-ops while NMEA is fresh; speed/course apply only when the user selected phone as
    // the speed source (never an automatic fallback).
    if (acceptedGps)  consumePhoneGps();
    applyPhoneSpeed(acceptedSpd, acceptedHdg);
    if (acceptedTime) syncTimeFromPhone(PhoneTimeEpoch);
    char out[64];
    snprintf(out, sizeof(out), "gps=%d time=%d spd=%d hdg=%d",
             acceptedGps ? 1 : 0, acceptedTime ? 1 : 0, acceptedSpd ? 1 : 0, acceptedHdg ? 1 : 0);
    request->send(200, "text/plain", out);
  });

  // Cloud Features
  server.on("/checkRegistration", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!settingsArmActive(request)) {
      request->send(403, "text/plain", "Settings not armed");
      return;
    }

    Serial.println("=== checkRegistration called ===");
    if (!isRegistered) {
      request->send(200, "application/json", "{\"registered\":false}");
      return;
    }

    // Token-only validation, staged onto the httpsTask worker: a synchronous doCloudPOST here starves every
    // endpoint for up to ~45 s on a dead uplink. Reply handling is in executeCloudOp; the client polls /cloudOpState.
    DynamicJsonDocument payloadDoc(256);
    payloadDoc["token"] = authToken;
    String payload;
    serializeJson(payloadDoc, payload);
    cloudOpStageAndReply(request, CLOUDOP_CHECK_REG, payload.c_str());
  });

  // Result poll for the staged cloud ops (/checkRegistration /registerProfile /updateProfile /deleteAllData):
  // "code" is the op's HTTP status, "body" its exact JSON reply.
  server.on("/cloudOpState", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!settingsArmActive(request)) {
      request->send(403, "text/plain", "Settings not armed");
      return;
    }
    if (cloudOpState == 0) {
      request->send(200, "application/json", "{\"state\":\"idle\"}");
      return;
    }
    if (cloudOpState == 1) {
      char buf[64];
      snprintf(buf, sizeof(buf), "{\"state\":\"pending\",\"op\":%u}", (unsigned)cloudOpSeq);
      request->send(200, "application/json", buf);
      return;
    }
    String out;
    out.reserve(CLOUDOP_BODY_CAP + 64);
    out = "{\"state\":\"done\",\"op\":" + String(cloudOpSeq) + ",\"code\":" + String(cloudOpCode) + ",\"body\":";
    out += (cloudOpBody && cloudOpBody[0]) ? cloudOpBody : "null";
    out += "}";
    request->send(200, "application/json", out);
  });

  // Always-answers identity endpoint for app-side subnet discovery (mDNS fails across a phone hotspot, so the
  // app sweeps 172.20.10.2-14). No password, no side effects; CORS because the app's origin is not us. uid is
  // what the app pins to, name what it shows, host the per-unit mDNS name, ap this unit's own SSID; pwreq =
  // Unlock Controls asks for a password; paired + peers (regulators heard on the LAN) build the dashboard picker.
  server.on("/identify", HTTP_GET, [](AsyncWebServerRequest *request) {
    String idBuf = "{\"device\":\"xreg-010\",\"uid\":\"";
    idBuf += device_id_hex;
    idBuf += "\",\"fw\":\"";
    idBuf += FIRMWARE_VERSION;
    idBuf += "\",\"name\":";
    cfgAppendJsonStr(idBuf, regulatorDisplayName());
    idBuf += ",\"host\":\"";
    idBuf += regulatorHostName();
    idBuf += ".local\",\"ap\":";
    cfgAppendJsonStr(idBuf, esp32_ap_ssid);
    // Own address: a browser tab opened by name has no IPv4 of its own to seed the LAN sweep with.
    idBuf += ",\"ip\":\"";
    idBuf += ((WiFi.status() == WL_CONNECTED) ? WiFi.localIP() : WiFi.softAPIP()).toString();
    idBuf += "\",\"pwreq\":";
    idBuf += pwRequired ? "1" : "0";
    idBuf += ",\"paired\":";
    pairListAppendJson(idBuf);
    idBuf += ",\"peers\":";
    peerListAppendJson(idBuf);
    idBuf += ",\"gx\":4";   // Victron GX link version: 2 = /gxevents, /gxrate, /gxStatus and gx=1 writes; 3 adds /gxLimits; 4 adds /gxDevices
    idBuf += ",\"demo\":";  // 1 = demo mode (simulated boat): a GX must not log or forward these values
    idBuf += demoSeal ? "1" : "0";
    idBuf += "}";
    AsyncWebServerResponse *r = request->beginResponse(200, "application/json", idBuf);
    r->addHeader("Access-Control-Allow-Origin", "*");
    request->send(r);
  });

  server.on("/getAuthToken", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isRegistered) {
      request->send(200, "application/json", "{\"registered\":false}");
      return;
    }

    DynamicJsonDocument doc(256);
    doc["registered"] = true;
    doc["token"] = authToken;

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });
  // Cloud Features
  server.on("/registerProfile", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (refuseMisaddressed(request)) return;
    if (!settingsArmActive(request)) {
      request->send(403, "text/plain", "Settings not armed");
      return;
    }
    String deviceUID = String(device_id_hex);
    if (!request->hasParam("username", true) || !request->hasParam("email", true)) {
      request->send(400, "application/json", "{\"error\":\"missing username/email\"}");
      return;
    }
    // Identity only — the vessel record reaches user_profiles via the config snapshot
    // (update-config-snapshot projects the settings jsonb), never via registration.
    DynamicJsonDocument doc(768);
    doc["device_uid"] = deviceUID;
    doc["username"] = request->getParam("username", true)->value();
    doc["email"] = request->getParam("email", true)->value();
    String nonce = regNonce();
    if (nonce.length()) doc["reg_nonce"] = nonce;
    String payload;
    serializeJson(doc, payload);
    Serial.println("=== REGISTRATION REQUEST ===");
    // Staged onto the httpsTask worker (see /checkRegistration). Token save, post-registration
    // version/forced checks and the snapshot request all live in executeCloudOp.
    cloudOpStageAndReply(request, CLOUDOP_REGISTER, payload.c_str());
  });

  server.on("/updateProfile", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (refuseMisaddressed(request)) return;
    if (!settingsArmActive(request)) {
      request->send(403, "text/plain", "Settings not armed");
      return;
    }

    String token = loadAuthToken();
    if (token.length() == 0) {
      request->send(400, "application/json", "{\"success\":false,\"error\":\"Not registered\"}");
      return;
    }

    String deviceUID = String(device_id_hex);

    if (!request->hasParam("username", true) || !request->hasParam("email", true)) {
      request->send(400, "application/json", "{\"error\":\"missing username/email\"}");
      return;
    }
    // Identity only — vessel data rides the config snapshot, same as /registerProfile.
    DynamicJsonDocument doc(512);
    doc["device_uid"] = deviceUID;
    doc["token"] = token;
    doc["username"] = request->getParam("username", true)->value();
    doc["email"] = request->getParam("email", true)->value();

    String payload;
    serializeJson(doc, payload);

    Serial.println("=== UPDATE PROFILE REQUEST ===");
    Serial.println("Device UID: " + deviceUID);
    // Staged onto the httpsTask worker (see /checkRegistration).
    cloudOpStageAndReply(request, CLOUDOP_UPDATE_PROFILE, payload.c_str());
  });

  server.on("/deleteAllData", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (refuseMisaddressed(request)) return;
    if (!settingsArmActive(request)) {
      request->send(403, "text/plain", "Settings not armed");
      return;
    }

    // Cloud requires auth_token to authorize the cascade delete
    String token = loadAuthToken();
    if (token.length() == 0) {
      request->send(400, "application/json", "{\"success\":false,\"error\":\"Not registered\"}");
      return;
    }

    DynamicJsonDocument doc(256);
    doc["token"] = token;

    String payload;
    serializeJson(doc, payload);

    Serial.println("=== DELETE ALL DATA REQUEST ===");
    // Staged onto the httpsTask worker (see /checkRegistration); the token clear on HTTP 200
    // lives in executeCloudOp.
    cloudOpStageAndReply(request, CLOUDOP_DELETE_DATA, payload.c_str());
  });

  server.on("/resetThermalPID", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (refuseMisaddressed(request)) return;
    tempPIDResetRequested = true;
    request->send(200, "text/plain", "OK");
  });

  server.on("/resetInnerPID", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (refuseMisaddressed(request)) return;
    innerPIDResetRequested = true;
    request->send(200, "text/plain", "OK");
  });

  server.on("/tuninglog", HTTP_GET, [](AsyncWebServerRequest *request) {
    // PSRAM buffer freed by the shared_ptr deleter on completion OR abort; chunked send keeps ~1.5 KB on the
    // internal heap. 16 KB fits the per-record Notes field.
    std::shared_ptr<char> bufPtr((char *)ps_malloc(16384), [](char *p) { if (p) free(p); });
    if (!bufPtr) { request->send(500, "text/plain", "OOM"); return; }
    char *buf = bufPtr.get();

    // Build sorted index (insertion sort — 50 entries max)
    const int count = tuningLogCount;   // read once: a record committed mid-handler would index an unset sortIdx slot
    uint8_t sortIdx[50];
    for (int i = 0; i < count; i++) sortIdx[i] = i;
    for (int i = 1; i < count; i++) {
      uint8_t key = sortIdx[i];
      float keyScore = tuningLog[key].score;
      int j = i - 1;
      while (j >= 0 && tuningLog[sortIdx[j]].score > keyScore) {
        sortIdx[j + 1] = sortIdx[j];
        j--;
      }
      sortIdx[j + 1] = key;
    }

    int pos = 0;
    pos += snprintf(buf + pos, 16384 - pos, "{\"rec\":[");
    for (int i = 0; i < count && pos < 15900; i++) {
      TuningRecord &r = tuningLog[sortIdx[i]];
      char tbuf[12];   // avgAltTempF is NAN when no probe read during the run -> JSON null
      pos += snprintf(buf + pos, 16384 - pos,
        "%s{\"n\":%d,\"s\":%.2f,\"t\":%.1f,"
        "\"kp\":%.4f,\"ki\":%.4f,\"kd\":%.5f,"
        "\"sd\":%d,\"tg\":%.2f,\"dr\":%.1f,"
        "\"wa\":%d,\"wp\":%d,\"wf\":%d,"
        "\"rpm\":%.0f,\"temp\":%s,\"worst\":%.1f,\"bv\":%.2f,\"cs\":%d,\"ts\":%u,\"note\":\"%s\"}",
        i > 0 ? "," : "",
        r.runNumber, r.score, r.activeTimeSec,
        r.kp, r.ki, r.kd,
        r.sampleDivisor, r.trackingGain, r.dutyRampRate,
        (int)r.waveAmplitude, (int)r.wavePeriod, (int)r.waveFloor,
        r.avgRPM, jsonNum1(tbuf, sizeof(tbuf), r.avgAltTempF), r.worstErrorA, r.battV, (int)r.chargeStage, (unsigned)r.epoch, r.note);
    }
    bool testActive = (TuningMode && tuningScore.toggleCount > 0);
    float ts = (tuningScore.activeTimeSec > 0.0f)
                 ? (tuningScore.errorAccum / tuningScore.activeTimeSec) : 0.0f;
    // "live" = since-reset Control Accuracy v4: [Tracking % in-band while active, worst over-current A, 0, 0];
    // the 4-slot shape is what the tuning UI parser expects.
    pos += snprintf(buf + pos, 16384 - pos,
      "],\"live\":[%.2f,%.2f,%.2f,%.2f],"
      "\"ts\":%.2f,\"tt\":%d,\"ta\":%d}",
      (accCur4.activeSec > 0.5) ? (float)(100.0 * accCur4.inbandActiveSec / accCur4.activeSec) : 0.0f,
      accCur4.worstOver, 0.0f, 0.0f,
      ts, (int)tuningScore.toggleCount, testActive ? 1 : 0);

    // Chunked send — only one TCP-MSS chunk (~1.5 KB) lands on the internal heap.
    size_t total = (size_t)pos;
    AsyncWebServerResponse *response = request->beginChunkedResponse(
      "application/json",
      [bufPtr, total](uint8_t *out, size_t maxLen, size_t index) -> size_t {
        if (index >= total) return 0;
        size_t toSend = (maxLen < (total - index)) ? maxLen : (total - index);
        memcpy(out, bufPtr.get() + index, toSend);
        return toSend;
      });
    request->send(response);
  });

  // Closed-loop Bode from the Tuning→Current sine auto-sweep. Small (≤10 points) → plain send.
  server.on("/tuningbode", HTTP_GET, [](AsyncWebServerRequest *request) {
    std::shared_ptr<char> bufPtr((char *)ps_malloc(2048), [](char *p) { if (p) free(p); });
    if (!bufPtr) { request->send(500, "text/plain", "OOM"); return; }
    char *buf = bufPtr.get();
    int pos = 0;
    pos += snprintf(buf + pos, 2048 - pos, "{\"pts\":[");
    for (int i = 0; i < tuningBodeCount && pos < 1900; i++) {
      pos += snprintf(buf + pos, 2048 - pos, "%s{\"f\":%.2f,\"g\":%.4f,\"ph\":%.1f}",
                      i > 0 ? "," : "",
                      tuningBode[i].freqHz, tuningBode[i].gain, tuningBode[i].phaseDeg);
    }
    pos += snprintf(buf + pos, 2048 - pos, "],\"active\":%d,\"done\":%d,\"tok\":%u,\"coh\":%.3f,\"railed\":%d,\"fs\":%.1f,"
                    "\"aborted\":%d,\"abortWhy\":%d,\"abortV\":%.2f,\"abortD\":%.1f,"
                    "\"rpmAvg\":%.0f,\"rpmMin\":%.0f,\"rpmMax\":%.0f}",
                    tuningSweepActive ? 1 : 0, tuningSweepDone ? 1 : 0, (unsigned)tuningRunTok,
                    tuningSweepWorstCoh, tuningSweepDutyRailed ? 1 : 0,
                    tuningSweepFsHz > 0.0f ? tuningSweepFsHz : ch1SampleHz(),
                    tuningSweepAbortReason != 0 ? 1 : 0, (int)tuningSweepAbortReason,
                    tuningSweepAbortVolts, tuningSweepAbortDuty,
                    tuningSweepRpmN ? (float)(tuningSweepRpmSum / tuningSweepRpmN) : 0.0f,
                    tuningSweepRpmMin, tuningSweepRpmMax);
    request->send(200, "application/json", buf);
  });

  // Open-loop plant Bode from the SystemID sine sweep. Small (≤10 points) → plain send.
  server.on("/sysidbode", HTTP_GET, [](AsyncWebServerRequest *request) {
    std::shared_ptr<char> bufPtr((char *)ps_malloc(2048), [](char *p) { if (p) free(p); });
    if (!bufPtr) { request->send(500, "text/plain", "OOM"); return; }
    char *buf = bufPtr.get();
    int pos = 0;
    pos += snprintf(buf + pos, 2048 - pos, "{\"pts\":[");
    for (int i = 0; i < systemIDBodeCount && pos < 1900; i++) {
      pos += snprintf(buf + pos, 2048 - pos, "%s{\"f\":%.2f,\"g\":%.4f,\"ph\":%.1f}",
                      i > 0 ? "," : "",
                      systemIDBode[i].freqHz, systemIDBode[i].gainApPct, systemIDBode[i].phaseDeg);
    }
    bool active = (systemIDActive != 0 && systemIDTestType == 1);
    // "aborted" = protection-abort latch (systemIDAbortRequested), reported independently of "active":
    // a protection cut leaves systemIDActive set until systemID_tick clears it, and that tick is gated
    // out during the fault/lockout, so the plant-fit poller would otherwise wait the full 240s timeout.
    pos += snprintf(buf + pos, 2048 - pos, "],\"active\":%d,\"ready\":%d,\"tok\":%u,\"aborted\":%d,\"amp\":%.1f,\"fs\":%.1f,"
                    "\"rpmAvg\":%.0f,\"rpmMin\":%.0f,\"rpmMax\":%.0f}",
                    active ? 1 : 0,
                    (systemIDResultsReady && systemIDTestType == 1) ? 1 : 0,
                    (unsigned)systemIDRunTok,
                    systemIDAbortRequested ? 1 : 0,
                    SystemIDStepAmplitude,
                    systemIDFsHz > 0.0f ? systemIDFsHz : ch1SampleHz(),
                    systemIDRpmAvg, systemIDRpmMin, systemIDRpmMax);
    request->send(200, "application/json", buf);
  });

  // Auto-commissioning field-% curve: the duty→amps points + the saturation knee + the
  // proposed open-loop-sine settings. Dashboard shows these and lets the user Apply.
  server.on("/fieldcurve.json", HTTP_GET, [](AsyncWebServerRequest *request) {
    // Sized for FIELDCURVE_MAX_PTS points at ~24 characters each, plus the ~500-character tail.
    static const int FCJ_BUF = FIELDCURVE_MAX_PTS * 26 + 768;
    std::shared_ptr<char> bufPtr((char *)ps_malloc(FCJ_BUF), [](char *p) { if (p) free(p); });
    if (!bufPtr) { request->send(500, "text/plain", "OOM"); return; }
    char *buf = bufPtr.get();
    int pos = 0;
    pos += snprintf(buf + pos, FCJ_BUF - pos, "{\"pts\":[");
    // Points are display-only and read once off the FINISHED run; on in-run polls ~1.7 kB every 1.5 s would
    // contend with the 10 Hz SSE telemetry for bytes the client discards.
    const bool fcvDone = (fieldCurveActive == 0) && fieldCurveResultsReady;
    for (int i = 0; fcvDone && i < fieldCurveCount && pos < FCJ_BUF - 640; i++) {
      pos += snprintf(buf + pos, FCJ_BUF - pos, "%s{\"d\":%.1f,\"a\":%.2f}",
                      i > 0 ? "," : "", fieldCurveBuf[i].duty, fieldCurveBuf[i].amps);
    }
    // "aborted" (fieldCurveAbortRequested) is reported independently of "active": a protection cut leaves
    // fieldCurveActive set and fieldCurve_tick is gated out during the lockout, so the poller checks "aborted".
    pos += snprintf(buf + pos, FCJ_BUF - pos,
                    "],\"active\":%d,\"ready\":%d,\"ok\":%d,\"tok\":%u,\"kneeDuty\":%.1f,\"kneeAmps\":%.2f,"
                    "\"targetA\":%.1f,\"propStabA\":%.1f,\"propStepPct\":%.2f,\"ceilLimited\":%d,\"ceilBind\":%d,\"aborted\":%d,\"abort\":\"%s\","
                    "\"abortWhy\":%d,\"abortNext\":%d,\"abortV\":%.2f,\"abortD\":%.1f,"
                    "\"rpmAvg\":%.0f,\"rpmMin\":%.0f,\"rpmMax\":%.0f}",
                    fieldCurveActive != 0 ? 1 : 0, fieldCurveResultsReady ? 1 : 0, fieldCurveOk ? 1 : 0,
                    (unsigned)fieldCurveRunTok,
                    fieldCurveKneeDuty, fieldCurveKneeAmps, fieldCurveTargetLimitA,
                    fieldCurvePropStabA, fieldCurvePropStepPct, fieldCurveCeilingLimited ? 1 : 0, (int)fieldCurveCeilBind,
                    fieldCurveAbortRequested ? 1 : 0, fieldCurveAbortMsg,
                    (int)fieldCurveAbortReason, (int)fieldCurveAbortFollowOn,
                    fieldCurveAbortVolts, fieldCurveAbortDuty,
                    fieldCurveRpmAvg, fieldCurveRpmMin, fieldCurveRpmMax);
    request->send(200, "application/json", buf);
  });

  // Field resistance test (BATTERY_WIRING_AND_SHUNT_SPEC.md Addendum B1). Every request is the app's heartbeat: a run
  // ends FRT_HB_MS after the last one. start=1&method=1|2 (shunt|meter), reading=<amps>, again=1, save=1, skip=1 and
  // abort=1 act; none = status read. abort works locked, like OnOff=0; the others need the arm. save=1 / skip=1 (the
  // 12 V no-shunt default, B1.14) are explicit owner actions, so they may write flash here.
  server.on("/fieldResTest", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (refuseMisaddressed(request)) return;
    const uint32_t nowMs = millis();
    if (frt.state == FRT_RUNNING) {
      const uint32_t gap = nowMs - frt.lastPollMs;
      if ((int32_t)gap > 0 && gap > frt.hbWorstMs) frt.hbWorstMs = gap;
    }
    frt.lastPollMs = nowMs;
    const char *refused = nullptr;
    if (request->hasParam("abort") && frt.state == FRT_RUNNING) {
      frt.reqAbort = true;
      logClientWrite(request, "FIELD RESISTANCE TEST: stop requested");
    }
    const bool acts = request->hasParam("start") || request->hasParam("reading") || request->hasParam("again") || request->hasParam("save")
                      || request->hasParam("skip");
    if (acts && !settingsArmActive(request)) {
      refused = "Unlock controls first.";
    } else if (request->hasParam("skip")) {
      if (frt.state == FRT_RUNNING || frt.reqStart) {
        refused = "Stop the test first.";
      } else if (!fieldResSkipAllowed()) {
        refused = "Only a 12 V system with no battery shunt can skip the test.";
      } else {
        fieldResUseDefault();
        logClientWrite(request, "FIELD RESISTANCE TEST: skipped, 2 ohm default");
      }
    } else if (request->hasParam("save")) {
      if (frt.state != FRT_DONE) {
        refused = "There is no test result to save.";
      } else if (!frt.saved) {
        fieldResRecord(frt.r, frt.probeC, false);
        frt.saved = true;
        logClientWrite(request, "FIELD RESISTANCE TEST: result saved");
      }
    } else if (request->hasParam("start")) {
      const uint8_t m = (request->hasParam("method") && request->getParam("method")->value().toInt() == FRT_SHUNT) ? FRT_SHUNT : FRT_METER;
      refused = frtRefuseReason(m);
      if (!refused) {
        frt.reqMethod = m;
        frt.state = FRT_IDLE;   // a status read between here and the control pass must not show the last run's result
        frt.reqStart = true;
        logClientWrite(request, (m == FRT_SHUNT) ? "FIELD RESISTANCE TEST: start (shunt method)" : "FIELD RESISTANCE TEST: start (meter method)");
      }
    } else if (request->hasParam("again")) {
      if (frt.state == FRT_RUNNING && frt.method == FRT_METER) frt.reqAgain = true;
    } else if (paramOk(request, "reading")) {
      // A clamp or meter the wrong way round reads negative; the magnitude is the field current.
      const float a = fabsf(request->getParam("reading")->value().toFloat());
      if (frt.state == FRT_RUNNING && frt.method == FRT_METER) {
        frt.reqAmps = a;
        frt.reqReading = true;
      }
    }

    const int cap = 3072;
    std::shared_ptr<char> bufPtr((char *)ps_malloc(cap), [](char *p) { if (p) free(p); });
    if (!bufPtr) { request->send(500, "text/plain", "OOM"); return; }
    char *buf = bufPtr.get();
    int pos = 0;
    auto num = [&](const char *key, float v, int dp) {
      if (isfinite(v)) pos += snprintf(buf + pos, cap - pos, "\"%s\":%.*f,", key, dp, v);
      else pos += snprintf(buf + pos, cap - pos, "\"%s\":null,", key);
    };
    const bool starting = frt.reqStart;
    // sh: the shunt method is on offer (the screen picks the method from it; the start refusal has the full check).
    pos += snprintf(buf + pos, cap - pos, "{\"st\":%u,\"m\":%u,\"h\":%u,\"rung\":%u,\"ph\":%u,\"el\":%lu,\"phMs\":%lu,\"holdMs\":%lu,\"sh\":%d,",
                    starting ? (unsigned)FRT_RUNNING : (unsigned)frt.state, (unsigned)(starting ? frt.reqMethod : frt.method),
                    (unsigned)frt.hold, (unsigned)frt.rung, (unsigned)frt.phase,
                    (unsigned long)((frt.state == FRT_RUNNING) ? (millis() - frt.phaseStartMs) : 0UL),
                    (unsigned long)FRT_PHASE_MS, (unsigned long)FRT_METER_HOLD_MS,
                    (HAS_BATT_SHUNT && (ShuntLead == 1 || ShuntLead == 2)) ? 1 : 0);
    num("d", frt.duty, 2);
    num("hd", frt.holdDuty, 2);
    num("hx", frt.holdX, 2);
    num("li", (frt.method == FRT_SHUNT && (frt.phase & 1)) ? frt.liveI : NAN, 2);
    num("ibv", IBV, 2);
    num("r", frt.r, 3);
    num("vc", frt.vc, 3);
    num("off", frt.probeOffV, 3);
    num("tC", frt.probeC, 1);
    num("tNowC", fieldResProbeC(), 1);   // the hold in progress shows it
    pos += snprintf(buf + pos, cap - pos, "\"calm\":%d,", frt.calm ? 1 : 0);   // shunt: 0 while the opening field-off readings wait to agree
    num("R", FieldResistance, 3);
    num("Tref", FieldResTrefC, 1);
    // sk: the skip is on offer (12 V, no battery shunt); def: the skipped-test default is what the regulator uses now.
    pos += snprintf(buf + pos, cap - pos, "\"sk\":%d,\"def\":%d,\"defR\":%.1f,", fieldResSkipAllowed() ? 1 : 0, fieldResDefaultInUse() ? 1 : 0,
                    (double)FD_SKIP_R_OHM);
    // rungs: probe rungs stepped up from, [x, i]. holds[k].c: the shunt cycles, [off, on, off battery amps, kept 1/0,
    // field amps].
    pos += snprintf(buf + pos, cap - pos, "\"rungs\":[");
    for (int k = 0; k < 2 && k < frt.rung; k++) {
      if (!isfinite(frt.rungX[k]) || !isfinite(frt.rungI[k])) continue;
      pos += snprintf(buf + pos, cap - pos, "%s[%.2f,%.3f]", (buf[pos - 1] == '[') ? "" : ",", frt.rungX[k], frt.rungI[k]);
    }
    pos += snprintf(buf + pos, cap - pos, "],\"holds\":[");
    for (int k = 0; k < FRT_N_HOLDS; k++) {
      const FrtHold &hd = frt.h[k];
      pos += snprintf(buf + pos, cap - pos, "%s{", k ? "," : "");
      num("x", hd.x, 2);
      num("d", hd.d, 2);
      num("i", hd.i, 3);
      num("tC", hd.tC, 1);
      pos += snprintf(buf + pos, cap - pos, "\"c\":[");
      for (int j = 0; j < hd.nc && j < FRT_MAX_CYC; j++) {
        const FrtCycle &c = hd.c[j];
        pos += snprintf(buf + pos, cap - pos, "%s[%.3f,%.3f,%.3f,%d,", j ? "," : "", c.a0, c.b, c.a1, c.ok ? 1 : 0);
        pos += isfinite(c.f) ? snprintf(buf + pos, cap - pos, "%.3f]", c.f) : snprintf(buf + pos, cap - pos, "null]");
      }
      pos += snprintf(buf + pos, cap - pos, "]}");
    }
    // msg and refused are firmware-authored ASCII without quotes or backslashes.
    pos += snprintf(buf + pos, cap - pos, "],\"saved\":%d,\"msg\":\"%s\",\"refused\":\"%s\"}",
                    frt.saved ? 1 : 0, (frt.state == FRT_FAILED || frt.state == FRT_DONE) ? frt.msg : "", refused ? refused : "");
    request->send(200, "application/json", buf);
  });

  // Field de-energize test (commissioning stage 7, once per Min%-floor speed): decay trace, fitted τ / 90→10 fall
  // time, hold-averaged RPM and pre-cut amps. pts.t is ms relative to the detected cut edge (negative lead-in shows
  // the plateau). "aborted" is reported independently of "active" (see /fieldcurve.json).
  server.on("/fieldcut.json", HTTP_GET, [](AsyncWebServerRequest *request) {
    std::shared_ptr<char> bufPtr((char *)ps_malloc(8192), [](char *p) { if (p) free(p); });
    if (!bufPtr) { request->send(500, "text/plain", "OOM"); return; }
    char *buf = bufPtr.get();
    int pos = 0;
    pos += snprintf(buf + pos, 8192 - pos, "{\"pts\":[");
    // Same as /fieldcurve.json: display-only, read once off the finished run (`active` stays set through the
    // 1.5 s ease-out, exactly when the field re-engages).
    const bool fcDone = (fieldCutActive == 0) && fieldCutResultsReady;
    for (int i = 0; fcDone && i < fcPlotN && pos < 7400; i++) {
      pos += snprintf(buf + pos, 8192 - pos, "%s{\"t\":%.1f,\"a\":%.2f}",
                      i > 0 ? "," : "", fcPlotMs[i], fcPlotA[i]);
    }
    pos += snprintf(buf + pos, 8192 - pos,
                    "],\"active\":%d,\"phase\":%d,\"ready\":%d,\"ok\":%d,\"tok\":%u,\"tauMs\":%.1f,\"fallMs\":%.1f,\"drainMs\":%.1f,"
                    "\"baseA\":%.1f,\"floorA\":%.2f,\"rpm\":%.0f,\"residPct\":%.1f,\"nPts\":%d,\"src\":%d,"
                    "\"calGain\":%.4f,\"calOffA\":%.3f,\"aborted\":%d,\"abort\":\"%s\"}",
                    fieldCutActive != 0 ? 1 : 0, (int)fieldCutPhase, fieldCutResultsReady ? 1 : 0,
                    fieldCutOk ? 1 : 0, (unsigned)fieldCutRunTok,
                    fieldCutTauMs, fieldCutFallMs, fieldCutDrainMs, fieldCutBaseA, fieldCutFloorA,
                    fieldCutRpm, fieldCutResidPct, fcPlotN, (int)fieldCutSrc, faCalGain, faCalOffA,
                    fieldCutAbortRequested ? 1 : 0, fieldCutAbortMsg);
    request->send(200, "application/json", buf);
  });

  // Charge health calibration status (commissioning stage 9). Only the fits leave the device. runMs/rampMs/idleRpm/top
  // are the schedule the browser charts against, so screen and firmware share one clock. topCapA is valid BEFORE a
  // run: the charge-rate limit at the would-be test top, for the setup screen.
  server.on("/chc.json", HTTP_GET, [](AsyncWebServerRequest *request) {
    char buf[768];
    snprintf(buf, sizeof(buf),
             "{\"active\":%d,\"phase\":%d,\"ready\":%d,\"ok\":%d,\"pct\":%d,"
             "\"targetA\":%.1f,\"capA\":%.0f,\"topCapA\":%.0f,\"hiCol\":%d,\"duty\":%.1f,\"amps\":%.1f,"
             "\"vbus\":%.2f,\"rpm\":%.0f,\"idleRpm\":%.0f,\"top\":%.0f,\"maxRpm\":%d,"
             "\"rampMs\":%lu,\"runMs\":%lu,\"cyc\":%d,\"seg\":%d,\"frozen\":%d,"
             "\"minA\":%.1f,\"peakA\":%.0f,\"leadSec\":%.3f,"
             "\"lead\":[%.3f,%.3f,%.3f],\"gapB\":[%.1f,%.1f,%.1f],\"gapA\":[%.1f,%.1f,%.1f],"
             "\"rateMin\":%.0f,\"rateAvg\":%.0f,\"rateMax\":%.0f,"
             "\"nPts\":%d,\"abort\":\"%s\"}",
             chcActive != 0 ? 1 : 0, (int)chcPhase, chcResultsReady ? 1 : 0, chcOk ? 1 : 0, (int)chcTestPct,
             chcTargetA, chcSizeCapA,
             (chcActive != 0) ? chcSizeCapA : chcPreviewCapA(),   // no flash read while a run is polling this every 700 ms
             (BatteryCapacity_Ah >= CHC_LOHI_AH) ? 1 : 0, chcHoldDuty, MeasuredAmps,
             BatteryV, RPM, chcIdleRpm, chcTopRpm, maxWorkingRpm,
             (unsigned long)chcRampMs, (unsigned long)chcRunMs, (int)chcCycle, (int)chcSeg, chcFrozen ? 1 : 0,
             chcMinA, chcPeakA, chcLeadSec,
             chcLead[0], chcLead[1], chcLead[2], chcGapB[0], chcGapB[1], chcGapB[2], chcGapA[0], chcGapA[1], chcGapA[2],
             chcRateMin, chcRateAvg, chcRateMax,
             chcCount, chcAbortMsg);
    request->send(200, "application/json", buf);
  });

  // CV plant-fit status + result (commissioning Step 7 polls this). "aborted" reported independently of
  // "active" (same rationale as /fieldcurve.json — a protection cut can leave active latched).
  server.on("/cvplantfit.json", HTTP_GET, [](AsyncWebServerRequest *request) {
    std::shared_ptr<char> bufPtr((char *)ps_malloc(1024), [](char *p) { if (p) free(p); });
    if (!bufPtr) { request->send(500, "text/plain", "OOM"); return; }
    char *buf = bufPtr.get();
    char eK[96], eS[48]; int pK = 0, pS = 0;   // per-edge stiffness + keep/drop status for the wizard table
    for (int i = 0; i < 8; i++) {
      pK += snprintf(eK + pK, sizeof(eK) - pK, i ? ",%.1f" : "%.1f", cvpfEdgeK[i]);
      pS += snprintf(eS + pS, sizeof(eS) - pS, i ? ",%d" : "%d", (int)cvpfEdgeStat[i]);
    }
    snprintf(buf, 1024,
             "{\"active\":%d,\"phase\":%d,\"ready\":%d,\"ok\":%d,\"aborted\":%d,"
             "\"K_mVpA\":%.1f,\"Ka_mVpA\":%.2f,\"Kb_mVpA\":%.2f,\"dV_mV\":%.0f,\"dI\":%.2f,\"snr\":%.0f,\"horizonS\":%.1f,"
             "\"Kp\":%.2f,\"Ki\":%.2f,\"stepA\":%.1f,\"capHeadroomA\":%.1f,\"rpmAtFit\":%.0f,\"rpmMinAtFit\":%.0f,\"rpmMaxAtFit\":%.0f,\"battVAtFit\":%.2f,\"socAtFit\":%.0f,\"diMaxA\":%.1f,\"warn\":%d,"
             "\"driftSetupPct\":%.1f,\"driftTrainPct\":%.1f,"
             "\"eK\":[%s],\"eS\":[%s],\"abort\":\"%s\"}",
             cvPlantFitActive ? 1 : 0, (int)cvpfPhase, cvpfReady ? 1 : 0, cvpfOk ? 1 : 0, (cvpfState == 3) ? 1 : 0,
             cvpfK * 1000.0f, cvpfKa * 1000.0f, cvpfKb * 1000.0f, cvpfDV * 1000.0f, cvpfDI, cvpfSNR, cvpfHorizonS,
             cvpfKp, cvpfKi, cvpfStepA, cvpfCapHeadroomA, cvpfRpmAtFit, cvpfRpmMinAtFit, cvpfRpmMaxAtFit, cvpfBattVAtFit, cvpfSocAtFit, cvpfDiMaxA, (int)cvpfWarn,
             cvpfDriftSetupPct, cvpfDriftTrainPct, eK, eS, cvpfAbortMsg);
    request->send(200, "application/json", buf);
  });

  // CV stress-test status + result (wizard stage 8 and the Tuning ▸ Stress Test standalone modal poll this; the
  // poll itself stamps the browser-alive deadman inside cvStressJsonBuild).
  server.on("/cvstress.json", HTTP_GET, [](AsyncWebServerRequest *request) {
    std::shared_ptr<char> bufPtr((char *)ps_malloc(1408), [](char *p) { if (p) free(p); });
    if (!bufPtr) { request->send(500, "text/plain", "OOM"); return; }
    cvStressJsonBuild(bufPtr.get(), 1408);
    request->send(200, "application/json", bufPtr.get());
  });

  // CV plant-fit raw trace (offline audit) — chunked so a full buffer never lands in internal RAM.
  server.on("/cvfit.csv", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!cvpfBuf || cvpfBufCount == 0) { request->send(200, "text/csv", "# no cvfit data\n"); return; }
    struct CvfExp { int stage; int idx; char line[128]; int llen, lpos; uint32_t t0; char hdr[448]; int hlen; };
    auto st = std::make_shared<CvfExp>();
    st->stage = 0; st->idx = 0; st->llen = 0; st->lpos = 0; st->t0 = cvpfBuf[0].tMs;
    st->hlen = snprintf(st->hdr, sizeof(st->hdr),
                        "# cvfit confidence record (firmware)\n"
                        "# K_mV_per_A,%.2f\n# horizon_S,%.2f\n# dV_mV,%.1f\n# dI_A,%.3f\n# snr,%.1f\n"
                        "# Kp,%.2f,Ki,%.2f\n# stepA,%.1f,warnBits,%d\n"
                        "# baseDuty_pct,%.2f,stepDuty_pct,%.2f\n"
                        "# driftSetup_pct,%.1f,driftTrain_pct,%.1f\n"
                        "t_s,IBV_V,Bcur_A,MeasuredAmps_A\n",
                        cvpfK * 1000.0f, cvpfHorizonS, cvpfDV * 1000.0f, cvpfDI, cvpfSNR,
                        cvpfKp, cvpfKi, cvpfStepA, (int)cvpfWarn,
                        cvpfBaseDuty, cvpfStepDuty, cvpfDriftSetupPct, cvpfDriftTrainPct);
    AsyncWebServerResponse *response = request->beginChunkedResponse("text/csv",
      [st](uint8_t *out, size_t maxLen, size_t) mutable -> size_t {
        size_t w = 0;
        while (st->stage == 0 && w < maxLen) {
          if (st->idx >= st->hlen) { st->stage = 1; st->idx = 0; break; }
          out[w++] = (uint8_t)st->hdr[st->idx++];
        }
        while (st->stage == 1 && w < maxLen) {
          if (st->lpos >= st->llen) {
            if (st->idx >= cvpfBufCount) break;   // fully drained → return w (0 on the final call)
            CvPlantFitSample &s = cvpfBuf[st->idx++];
            // No battery shunt -> iBat is INA input noise. Empty cell; the fit itself already runs
            // off ΔI_alt in that mode (cvFitSinglePulse noShunt path), so nothing here needs it.
            char iBatS[16];
            if (HAS_BATT_SHUNT) snprintf(iBatS, sizeof(iBatS), "%.3f", s.iBat);
            else                iBatS[0] = '\0';
            st->llen = snprintf(st->line, sizeof(st->line), "%.3f,%.4f,%s,%.3f\n",
                                (s.tMs - st->t0) / 1000.0f, s.v, iBatS, s.iAlt);
            st->lpos = 0;
          }
          while (st->lpos < st->llen && w < maxLen) out[w++] = (uint8_t)st->line[st->lpos++];
        }
        return w;
      });
    response->addHeader("Cache-Control", "no-cache");
    request->send(response);
  });

  // Min% onset-knee sweep status + committed anchors (commissioning step polls this).
  server.on("/kneesweep.json", HTTP_GET, [](AsyncWebServerRequest *request) {
    std::shared_ptr<char> bufPtr((char *)ps_malloc(1024), [](char *p) { if (p) free(p); });
    if (!bufPtr) { request->send(500, "text/plain", "OOM"); return; }
    char *buf = bufPtr.get();
    int pos = 0;
    pos += snprintf(buf + pos, 1024 - pos,
                    "{\"active\":%d,\"ready\":%d,\"ok\":%d,\"tok\":%u,\"kneeDuty\":%.1f,\"rpm\":%.0f,\"tempF\":%.0f,"
                    "\"fitResid\":%.2f,\"fitWorstIdx\":%d,\"anchors\":[",
                    (fieldCurveActive != 0 && fieldCurveOnsetMode) ? 1 : 0,
                    (fieldCurveResultsReady && fieldCurveOnsetMode) ? 1 : 0, kneeSweepOk ? 1 : 0,
                    (unsigned)fieldCurveRunTok,
                    kneeSweepKneeDuty, kneeSweepRPM, kneeSweepTempF, kneeFitResidPct, kneeFitWorstIdx);
    for (int i = 0; i < kneeAnchorN && pos < 900; i++) {
      pos += snprintf(buf + pos, 1024 - pos, "%s{\"rpm\":%.0f,\"duty\":%.1f,\"tempF\":%.0f}",
                      i > 0 ? "," : "", kneeAnchorRPM[i], kneeAnchorDuty[i], kneeAnchorTempF[i]);
    }
    // "aborted" = protection-abort latch, reported independently of "active" (see /fieldcurve.json note).
    // "ceilLimited" = the sweep stopped at the field-duty ceiling before finding onset; "ceilBind" = which term.
    pos += snprintf(buf + pos, 1024 - pos, "],\"ceilLimited\":%d,\"ceilBind\":%d,\"aborted\":%d,\"abort\":\"%s\","
                    "\"abortWhy\":%d,\"abortNext\":%d,\"abortV\":%.2f,\"abortD\":%.1f}",
                    fieldCurveCeilingLimited ? 1 : 0, (int)fieldCurveCeilBind, fieldCurveAbortRequested ? 1 : 0, fieldCurveAbortMsg,
                    (int)fieldCurveAbortReason, (int)fieldCurveAbortFollowOn,
                    fieldCurveAbortVolts, fieldCurveAbortDuty);
    request->send(200, "application/json", buf);
  });

  server.on("/tuningsweeplog", HTTP_GET, [](AsyncWebServerRequest *request) {
    const int CAP = 32768;
    std::shared_ptr<char> bufPtr((char *)ps_malloc(CAP), [](char *p) { if (p) free(p); });
    if (!bufPtr) { request->send(500, "text/plain", "OOM"); return; }
    char *buf = bufPtr.get();
    // Sort by bandwidth; never-tracks (<0) sinks to end.
    const int count = tuningSweepLogCount;   // read once: a record committed mid-handler would index an unset sortIdx slot
    uint8_t sortIdx[50];
    for (int i = 0; i < count; i++) sortIdx[i] = i;
    for (int i = 1; i < count; i++) {
      uint8_t key = sortIdx[i];
      float kb = tuningSweepLog[key].bandwidthHz; if (kb < 0.0f) kb = -1e9f;
      int j = i - 1;
      while (j >= 0) {
        float pv = tuningSweepLog[sortIdx[j]].bandwidthHz; if (pv < 0.0f) pv = -1e9f;
        if (pv >= kb) break;
        sortIdx[j + 1] = sortIdx[j]; j--;
      }
      sortIdx[j + 1] = key;
    }
    int pos = 0;
    pos += snprintf(buf + pos, CAP - pos, "{\"rec\":[");
    for (int i = 0; i < count && pos < CAP - 600; i++) {
      TuningSweepRecord &r = tuningSweepLog[sortIdx[i]];
      char tbuf[12];   // avgAltTempF is NAN when no probe read during the run -> JSON null
      pos += snprintf(buf + pos, CAP - pos,
        "%s{\"n\":%d,\"bw\":%.2f,\"pg\":%.3f,\"pgf\":%.2f,\"wp\":%.0f,\"wpf\":%.2f,"
        "\"kp\":%.4f,\"ki\":%.4f,\"kd\":%.5f,\"f0\":%.2f,\"f1\":%.2f,\"cy\":%d,"
        "\"rpm\":%.0f,\"temp\":%s,"
        "\"amp\":%.2f,\"base\":%.2f,\"bv\":%.2f,\"rmin\":%.0f,\"rmax\":%.0f,"
        "\"coh\":%.3f,\"clip\":%d,\"cs\":%d,\"ts\":%u,\"pts\":[",
        i > 0 ? "," : "",
        r.runNumber, r.bandwidthHz, r.peakGain, r.peakGainFreqHz, r.worstPhaseDeg, r.worstPhaseFreqHz,
        r.kp, r.ki, r.kd, r.sweepStartHz, r.sweepEndHz, (int)r.cycles,
        r.avgRPM, jsonNum1(tbuf, sizeof(tbuf), r.avgAltTempF),
        r.sineAmpA, r.baseA, r.battV, r.rpmMin, r.rpmMax,
        r.worstCoherence, (int)r.dutyRailed, (int)r.chargeStage, (unsigned)r.epoch);
      for (int k = 0; k < r.nPoints && k < TUNING_SWEEP_NPOINTS && pos < CAP - 60; k++) {
        pos += snprintf(buf + pos, CAP - pos, "%s{\"f\":%.2f,\"g\":%.4f,\"ph\":%.1f}",
                        k > 0 ? "," : "", r.curve[k].freqHz, r.curve[k].gain, r.curve[k].phaseDeg);
      }
      pos += snprintf(buf + pos, CAP - pos, "]}");
    }
    pos += snprintf(buf + pos, CAP - pos, "],\"active\":%d,\"done\":%d}",
                    tuningSweepActive ? 1 : 0, tuningSweepDone ? 1 : 0);
    size_t total = (size_t)pos;
    AsyncWebServerResponse *response = request->beginChunkedResponse(
      "application/json",
      [bufPtr, total](uint8_t *out, size_t maxLen, size_t index) -> size_t {
        if (index >= total) return 0;
        size_t toSend = (maxLen < (total - index)) ? maxLen : (total - index);
        memcpy(out, bufPtr.get() + index, toSend);
        return toSend;
      });
    request->send(response);
  });

  server.on("/sysidsweeplog", HTTP_GET, [](AsyncWebServerRequest *request) {
    const int CAP = 32768;
    std::shared_ptr<char> bufPtr((char *)ps_malloc(CAP), [](char *p) { if (p) free(p); });
    if (!bufPtr) { request->send(500, "text/plain", "OOM"); return; }
    char *buf = bufPtr.get();
    // Sort by roll-off; <0 sinks to end.
    const int count = sysidSweepLogCount;   // read once: a record committed mid-handler would index an unset sortIdx slot
    uint8_t sortIdx[50];
    for (int i = 0; i < count; i++) sortIdx[i] = i;
    for (int i = 1; i < count; i++) {
      uint8_t key = sortIdx[i];
      float kb = sysidSweepLog[key].rolloffHz; if (kb < 0.0f) kb = -1e9f;
      int j = i - 1;
      while (j >= 0) {
        float pv = sysidSweepLog[sortIdx[j]].rolloffHz; if (pv < 0.0f) pv = -1e9f;
        if (pv >= kb) break;
        sortIdx[j + 1] = sortIdx[j]; j--;
      }
      sortIdx[j + 1] = key;
    }
    int pos = 0;
    pos += snprintf(buf + pos, CAP - pos, "{\"rec\":[");
    for (int i = 0; i < count && pos < CAP - 600; i++) {
      SysIDSweepRecord &r = sysidSweepLog[sortIdx[i]];
      char tbuf[12];   // avgAltTempF is NAN when no probe read during the run -> JSON null
      pos += snprintf(buf + pos, CAP - pos,
        "%s{\"n\":%d,\"ro\":%.2f,\"dc\":%.4f,\"wp\":%.0f,\"wpf\":%.2f,"
        "\"amp\":%.1f,\"floor\":%.1f,\"f0\":%.2f,\"f1\":%.2f,\"cy\":%d,"
        "\"rpm\":%.0f,\"temp\":%s,\"bv\":%.2f,\"cs\":%d,\"ts\":%u,\"pts\":[",
        i > 0 ? "," : "",
        r.runNumber, r.rolloffHz, r.dcGainApPct, r.worstPhaseDeg, r.worstPhaseFreqHz,
        r.setupAmplitude, r.stabilizeAmps, r.sweepStartHz, r.sweepEndHz, (int)r.cycles,
        r.avgRPM, jsonNum1(tbuf, sizeof(tbuf), r.avgAltTempF), r.battV, (int)r.chargeStage, (unsigned)r.epoch);
      for (int k = 0; k < r.nPoints && k < SYSID_SINE_NPOINTS && pos < CAP - 60; k++) {
        pos += snprintf(buf + pos, CAP - pos, "%s{\"f\":%.2f,\"g\":%.4f,\"ph\":%.1f}",
                        k > 0 ? "," : "", r.curve[k].freqHz, r.curve[k].gainApPct, r.curve[k].phaseDeg);
      }
      pos += snprintf(buf + pos, CAP - pos, "]}");
    }
    bool active = (systemIDActive != 0 && systemIDTestType == 1);
    pos += snprintf(buf + pos, CAP - pos, "],\"active\":%d}", active ? 1 : 0);
    size_t total = (size_t)pos;
    AsyncWebServerResponse *response = request->beginChunkedResponse(
      "application/json",
      [bufPtr, total](uint8_t *out, size_t maxLen, size_t index) -> size_t {
        if (index >= total) return 0;
        size_t toSend = (maxLen < (total - index)) ? maxLen : (total - index);
        memcpy(out, bufPtr.get() + index, toSend);
        return toSend;
      });
    request->send(response);
  });

  server.on("/systemidlog", HTTP_GET, [](AsyncWebServerRequest *request) {
    // PSRAM-backed buffer (50 records × ~215 bytes ≈ 11 KB, so 16 KB like /tuninglog).
    std::shared_ptr<char> bufPtr((char *)ps_malloc(16384), [](char *p) { if (p) free(p); });
    if (!bufPtr) { request->send(500, "text/plain", "OOM"); return; }
    char *buf = bufPtr.get();

    // Sort by score (lower = faster plant = better). Aborted runs (-1) sink to end.
    const int count = systemIDLogCount;   // read once: a record committed mid-handler would index an unset sortIdx slot
    uint8_t sortIdx[50];
    for (int i = 0; i < count; i++) sortIdx[i] = i;
    for (int i = 1; i < count; i++) {
      uint8_t key = sortIdx[i];
      float keyScore = systemIDLog[key].score;
      if (keyScore < 0.0f) keyScore = 1e9f;  // aborted → sort last
      int j = i - 1;
      while (j >= 0) {
        float prev = systemIDLog[sortIdx[j]].score;
        if (prev < 0.0f) prev = 1e9f;
        if (prev <= keyScore) break;
        sortIdx[j + 1] = sortIdx[j];
        j--;
      }
      sortIdx[j + 1] = key;
    }

    int pos = 0;
    pos += snprintf(buf + pos, 16384 - pos, "{\"rec\":[");
    for (int i = 0; i < count && pos < 15900; i++) {
      SystemIDRecord &r = systemIDLog[sortIdx[i]];
      char tbuf[12];   // avgAltTempF is NAN when no probe read during the run -> JSON null
      pos += snprintf(buf + pos, 16384 - pos,
        "%s{\"n\":%u,\"s\":%.1f,"
        "\"rd\":[%.0f,%.0f,%.0f],\"fd\":[%.0f,%.0f,%.0f],"
        "\"ra\":%.1f,\"fa\":%.1f,"
        "\"sa\":[%.2f,%.2f,%.2f],\"qp\":[%.3f,%.3f,%.3f],"
        "\"ar\":%u,\"ap\":%u,\"amp\":%.2f,"
        "\"rpm\":%.0f,\"temp\":%s,\"bv\":%.2f,\"cs\":%d,\"ts\":%u}",
        i > 0 ? "," : "",
        (unsigned)r.runNumber, r.score,
        r.riseDelays[0], r.riseDelays[1], r.riseDelays[2],
        r.fallDelays[0], r.fallDelays[1], r.fallDelays[2],
        r.riseAvg_ms, r.fallAvg_ms,
        r.stepAmps[0], r.stepAmps[1], r.stepAmps[2],
        r.quietPP[0], r.quietPP[1], r.quietPP[2],
        (unsigned)r.abortReason, (unsigned)r.abortPhase, r.setupStepAmplitude,
        r.avgRPM, jsonNum1(tbuf, sizeof(tbuf), r.avgAltTempF), r.battV, (int)r.chargeStage, (unsigned)r.epoch);
    }
    pos += snprintf(buf + pos, 16384 - pos,
      "],\"active\":%d,\"ready\":%d}",
      (int)systemIDActive, systemIDResultsReady ? 1 : 0);

    size_t total = (size_t)pos;
    AsyncWebServerResponse *response = request->beginChunkedResponse(
      "application/json",
      [bufPtr, total](uint8_t *out, size_t maxLen, size_t index) -> size_t {
        if (index >= total) return 0;
        size_t toSend = (maxLen < (total - index)) ? maxLen : (total - index);
        memcpy(out, bufPtr.get() + index, toSend);
        return toSend;
      });
    request->send(response);
  });

  server.on("/cvtuninglog", HTTP_GET, [](AsyncWebServerRequest *request) {
    // PSRAM buffer (50 records × ~420 B ≈ 21 KB); the shared_ptr deleter frees it on completion OR abort.
    std::shared_ptr<char> bufPtr((char *)ps_malloc(32768), [](char *p) { if (p) free(p); });
    if (!bufPtr) { request->send(500, "text/plain", "OOM"); return; }
    char *buf = bufPtr.get();

    // Build sorted index (insertion sort, best score first)
    const int count = cvTuningLogCount;   // read once: a record committed mid-handler would index an unset sortIdx slot
    uint8_t sortIdx[50];
    for (int i = 0; i < count; i++) sortIdx[i] = i;
    for (int i = 1; i < count; i++) {
      uint8_t key = sortIdx[i];
      float keyScore = cvTuningLog[key].score;
      int j = i - 1;
      while (j >= 0 && cvTuningLog[sortIdx[j]].score > keyScore) {
        sortIdx[j + 1] = sortIdx[j];
        j--;
      }
      sortIdx[j + 1] = key;
    }

    int pos = 0;
    pos += snprintf(buf + pos, (pos >= 32768 ? 0 : 32768 - pos), "{\"rec\":[");
    for (int i = 0; i < count && pos < 32000; i++) {
      CVTuningRecord &r = cvTuningLog[sortIdx[i]];
      char tbuf[12];   // avgAltTempF is NAN when no probe read during the run -> JSON null
      pos += snprintf(buf + pos, (pos >= 32768 ? 0 : 32768 - pos),
        "%s{\"n\":%d,\"s\":%.2f,\"st\":%.1f,\"wo\":%.3f,\"io\":%.4f,\"t\":%.1f,"
        "\"ls\":%.2f,\"lst\":%.1f,\"lwo\":%.3f,\"lio\":%.4f,\"lus\":%.3f,"
        "\"fov\":%d,\"iex\":%d,\"ld\":%d,\"hoc\":%d,"
        "\"vkp\":%.3f,\"vki\":%.3f,"
        "\"srr\":%.1f,\"sfr\":%.1f,"
        "\"abl\":%.2f,\"arl\":%.3f,\"asp\":%d,\"irf\":%.2f,"
        "\"ks\":%.1f,\"kh\":%.1f,"
        "\"iefr\":%.3f,\"ietau\":%.0f,\"iekb\":%.2f,"
        "\"lddt\":%.0f,\"ldt1\":%.0f,\"ldt3\":%.0f,"
        "\"wa\":%.2f,\"wp\":%d,\"ko\":%.1f,\"cr\":%d,"
        "\"rpm\":%.0f,\"tmp\":%s,\"bv\":%.2f,\"soc\":%.1f,\"cvt\":%.2f,\"cs\":%d,\"ts\":%u,\"p2p\":%.3f,\"note\":\"%s\"}",
        i > 0 ? "," : "",
        r.runNumber, r.score, r.avgSettlingTimeSec, r.worstOvershootV,
        r.avgIntegratedOvershootVs, r.activeTimeSec,
        r.lowScore, r.avgLowSettlingTimeSec, r.worstLowOvV, r.avgLowIntOvVs, r.worstLowUndershootV,
        (int)r.fastOvFires, (int)r.iExcessFires, (int)r.loadDumpFires, (int)r.hardOcFires,
        r.voltageKp, r.voltageKi,
        r.setpointRiseRate, r.setpointFallRate,
        r.awBleedRate, r.awRecoverRate, (int)r.awSeedProtectMs, r.reseedFrac,
        r.voltageKd, r.kHard,
        r.iExcessFrac, r.iExcessTau, r.iExcessKBleed,
        r.loadDumpDtThresh, r.loadDumpDtThresh1, r.loadDumpDtThresh3,
        r.waveAmplitudeV, (int)r.wavePeriodSec, r.kOvershoot, (int)r.consecutiveReads,
        r.avgRPM, jsonNum1(tbuf, sizeof(tbuf), r.avgAltTempF), r.battVAtStart, r.socAtStart * 100.0f, r.chargingVoltageTarget,
        (int)r.chargeStage, (unsigned)r.epoch, r.steadyP2PV, r.note);
    }
    bool cvTestActive = (CVTuningMode && cvTuningScore.testStarted);
    float cvts = 0.0f;
    if (cvTestActive && cvTuningScore.activeTimeSec > 0.0f) {
      // ÷ class ratio² to match commitCVTuningRecord's 12V-equivalent score normalization
      float liveNorm = (12.0f / (float)SYSTEM_VOLTAGE_CLASS) * (12.0f / (float)SYSTEM_VOLTAGE_CLASS);
      cvts = 1000.0f * liveNorm * (cvTuningScore.totalIntegratedOvershootVs
                        + cvTuningScore.totalLowIntOvVs
                        + cvTuningScore.totalLowUndershootVs)
             / cvTuningScore.activeTimeSec;
    }
    // "live" = since-reset Control Accuracy v4 for the CV loop: [Tracking % in-band while active, worst
    // over-voltage mV 12V-equiv, 0, 0]; the 4-slot shape is what the tuning UI parser expects.
    pos += snprintf(buf + pos, (pos >= 32768 ? 0 : 32768 - pos),
      "],\"live\":[%.2f,%.0f,%.0f,%.0f],"
      "\"ts\":%.2f,\"tc\":%d,\"ta\":%d}",
      (accVolt4.activeSec > 0.5) ? (float)(100.0 * accVolt4.inbandActiveSec / accVolt4.activeSec) : 0.0f,
      accVolt4.worstOver * 1000.0f, 0.0f, 0.0f,  // mV
      cvts, (int)cvTuningScore.scoredHighCount, cvTestActive ? 1 : 0);

    // Chunked send: ~1.5 KB per chunk on the internal heap; the captured bufPtr keeps the PSRAM buffer alive.
    size_t total = (size_t)pos;
    AsyncWebServerResponse *response = request->beginChunkedResponse(
      "application/json",
      [bufPtr, total](uint8_t *out, size_t maxLen, size_t index) -> size_t {
        if (index >= total) return 0;
        size_t toSend = (maxLen < (total - index)) ? maxLen : (total - index);
        memcpy(out, bufPtr.get() + index, toSend);
        return toSend;
      });
    request->send(response);
  });

  server.on("/resetcvtuninglog", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (refuseMisaddressed(request)) return;
    if (demoSeal) { demoRefuse(request); return; }
    cvTuningLogCount     = 0;
    cvTuningLogHead      = 0;
    cvTuningRunCounter   = 0;
    cvTuningScore        = {};
    cvTuningParamChanged = false;
    if (cvTuningLog) memset(cvTuningLog, 0, 50 * sizeof(CVTuningRecord));
    accVolt4 = {};  // clear the CV loop's Control Accuracy numbers too (incl. live excursion)
    excVolt = {};
    pendingSaveCVTuningLog = true;  // deferred to Core 1 — avoids blocking Core 0 SSE
    request->send(200, "text/plain", "OK");
  });

  server.on("/resettuninglog", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (refuseMisaddressed(request)) return;
    if (demoSeal) { demoRefuse(request); return; }
    tuningLogCount    = 0;
    tuningLogHead     = 0;
    tuningRunCounter  = 0;
    tuningScore       = {};
    tuningParamChanged = false;
    if (tuningLog) memset(tuningLog, 0, 50 * sizeof(TuningRecord));
    accCur4 = {};  // clear the inner current loop's Control Accuracy numbers too (incl. live excursion)
    excCur = {};
    pendingSaveTuningLog = true;  // deferred to Core 1 — avoids blocking Core 0 SSE
    request->send(200, "text/plain", "OK");
  });

  // Manual Control Accuracy "Reset": clears accumulators AND the live excursion stopwatches (the daily
  // auto-reset passes false so an in-progress excursion carries whole into the new window).
  server.on("/resetAccuracyScores", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (refuseMisaddressed(request)) return;
    resetAccuracyScores(true);
    request->send(200, "text/plain", "OK");
  });

  // Control Accuracy v4 diagnostics — raw accumulators + live excursion state, fetched on demand
  // by the panel's expander (kept out of CSV2 to bound field growth).
  server.on("/accstate", HTTP_GET, [](AsyncWebServerRequest *request) {
    char buf[768];
    int p = snprintf(buf, sizeof(buf),
                     "{\"cur\":{\"validS\":%.0f,\"activeS\":%.0f,\"inbandS\":%.0f,\"constS\":%.0f,"
                     "\"exc\":%u,\"recovS\":%.1f,\"overExp\":%.2f,\"worst\":%.2f,"
                     "\"liveExc\":%d,\"liveSide\":%d,\"liveOutS\":%.1f},",
                     (float)accCur4.validSec, (float)accCur4.activeSec, (float)accCur4.inbandActiveSec,
                     (float)accCur4.constrainedSec, (unsigned)accCur4.excursions, (float)accCur4.recovSecSum,
                     (float)accCur4.overExpSum, accCur4.worstOver,
                     (int)excCur.state, (int)excCur.side, (float)excCur.outSec);
    // snprintf returns the would-be length, so p can exceed the buffer; sizeof(buf) - p would then
    // underflow to a huge size_t and the second call would overrun the stack.
    if (p < 0) p = 0;
    if (p > (int)sizeof(buf)) p = (int)sizeof(buf);
    p += snprintf(buf + p, sizeof(buf) - p,
                  "\"volt\":{\"validS\":%.0f,\"activeS\":%.0f,\"inbandS\":%.0f,\"constS\":%.0f,"
                  "\"exc\":%u,\"recovS\":%.1f,\"overExpMv\":%.0f,\"worstMv\":%.0f,"
                  "\"liveExc\":%d,\"liveSide\":%d,\"liveOutS\":%.1f},"
                  "\"therm\":{\"sessions\":%u,\"bindS\":%.0f,\"inbandS\":%.0f,\"worstF\":%.1f}}",
                  (float)accVolt4.validSec, (float)accVolt4.activeSec, (float)accVolt4.inbandActiveSec,
                  (float)accVolt4.constrainedSec, (unsigned)accVolt4.excursions, (float)accVolt4.recovSecSum,
                  (float)(accVolt4.overExpSum * 1000.0), accVolt4.worstOver * 1000.0f,
                  (int)excVolt.state, (int)excVolt.side, (float)excVolt.outSec,
                  accThermSessions, (float)accThermBindingSec, (float)accThermInbandSec, accThermWorstOverF);
    request->send(200, "application/json", buf);
  });

  server.on("/resetsystemidlog", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (refuseMisaddressed(request)) return;
    if (demoSeal) { demoRefuse(request); return; }
    systemIDLogClearAll();  // persist deferred to Core 1 — avoids blocking Core 0 SSE
    request->send(200, "text/plain", "OK");
  });

  server.on("/resettuningsweeplog", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (refuseMisaddressed(request)) return;
    if (demoSeal) { demoRefuse(request); return; }
    tuningSweepLogCount   = 0;
    tuningSweepLogHead    = 0;
    tuningSweepRunCounter = 0;
    if (tuningSweepLog) memset(tuningSweepLog, 0, 50 * sizeof(TuningSweepRecord));
    pendingSaveTuningSweepLog = true;  // deferred to Core 1
    request->send(200, "text/plain", "OK");
  });

  server.on("/resetsysidsweeplog", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (refuseMisaddressed(request)) return;
    if (demoSeal) { demoRefuse(request); return; }
    sysidSweepLogCount   = 0;
    sysidSweepLogHead    = 0;
    sysidSweepRunCounter = 0;
    if (sysidSweepLog) memset(sysidSweepLog, 0, 50 * sizeof(SysIDSweepRecord));
    pendingSaveSysidSweepLog = true;  // deferred to Core 1
    request->send(200, "text/plain", "OK");
  });

  server.on("/resetVoltageLoop", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (refuseMisaddressed(request)) return;
    cvLoopResetRequested = true;
    request->send(200, "text/plain", "OK");
  });

  server.on("/resetVoltageProtectionCounters", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (refuseMisaddressed(request)) return;
    g_fastOvClampCount = 0;
    g_fastOvHardCount = 0;
    g_iExcessCount = 0;
    g_cvKdCount = 0;
    g_inaOVCount = 0;
    g_hardOCCount = 0;
    g_voltSpikeCount = 0;
    g_ovTierLowCount = 0;
    g_ovTierMidCount = 0;
    g_voltDisagreeCritCount = 0;
    g_voltDisagreeWarnCount = 0;
    g_voltImplausibleCount = 0;
    g_currentStaleCount = 0;
    g_voltStaleCount = 0;
    g_volt2StaleCount = 0;
    g_battTempLostCount = 0;
    g_dieTempLostCount = 0;
    request->send(200, "text/plain", "OK");
  });

  // Deliberately SEPARATE from the session-counter reset above: routine bench resets must not
  // destroy the lifetime OV record. The web UI fronts this with an explicit confirm.
  server.on("/resetOvTelemetryLifetime", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (refuseMisaddressed(request)) return;
    OvTelemetry &ov = ovTel();   // the demo copy while in demo
    memset(&ov, 0, sizeof(ov));
    ov.magic = OVTEL_MAGIC;
    request->send(200, "text/plain", "OK");
  });

  // Lifetime OV telemetry for the Diagnostics panel. Not in CSV2 (avoids field-count churn) —
  // polled on demand. time_ms as decimal strings: uint64 dwell is beyond JS's 53-bit integers.
  server.on("/getOvTelemetry", HTTP_GET, [](AsyncWebServerRequest *request) {
    const OvTelemetry &ov = ovTel();
    const size_t cap = 2048;  // worst-case body ~1.2 KB (31 bins × two arrays + metadata)
    char *buf = (char *)ps_malloc(cap);
    if (!buf) {
      request->send(500, "text/plain", "Out of memory");
      return;
    }
    int off = snprintf(buf, cap,
                       "{\"bulk\":%.2f,\"k\":%.2f,\"bins_fine\":%d,\"bins_coarse\":%d,"
                       "\"fine_width_12v\":0.2,\"coarse_width_12v\":1.0,"
                       "\"soft\":%lu,\"sw_hard\":%lu,\"ina\":%lu,\"kd\":%lu,"
                       "\"tier_low\":%lu,\"tier_mid\":%lu,\"events\":[",
                       BulkVoltage, (float)SYSTEM_VOLTAGE_CLASS / 12.0f, OV_HIST_FINE_BINS, OV_HIST_COARSE_BINS,
                       (unsigned long)ov.softExceedCount, (unsigned long)ov.swHardCutCount,
                       (unsigned long)ov.inaCutCount, (unsigned long)ov.kdEventCount,
                       (unsigned long)ov.tierLowCutCount, (unsigned long)ov.tierMidCutCount);
    for (int i = 0; i < OV_HIST_BINS && off > 0 && off < (int)cap; i++)
      off += snprintf(buf + off, cap - off, "%s%lu", i ? "," : "", (unsigned long)ov.events[i]);
    if (off > 0 && off < (int)cap) off += snprintf(buf + off, cap - off, "],\"time_ms\":[");
    for (int i = 0; i < OV_HIST_BINS && off > 0 && off < (int)cap; i++)
      off += snprintf(buf + off, cap - off, "%s\"%llu\"", i ? "," : "", (unsigned long long)ov.timeMs[i]);
    if (off > 0 && off < (int)cap) off += snprintf(buf + off, cap - off, "]}");
    if (off <= 0 || off >= (int)cap) {
      free(buf);
      request->send(500, "text/plain", "Payload overflow");
      return;
    }
    request->send(200, "application/json", buf);
    free(buf);
  });

  server.on("/resetThermalProtectionCounters", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (refuseMisaddressed(request)) return;
    g_tempCritCount = 0;
    g_tempSustainedCount = 0;
    g_tempStaleCount = 0;
    request->send(200, "text/plain", "OK");
  });

  server.on("/resetTempTaskCounters", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (refuseMisaddressed(request)) return;
    tempReadFailCount = 0;
    tempCrcFailCount = 0;
    tempCrcRecoveredCount = 0;
    tempAllFFCount = 0;
    tempPowerOn85Count = 0;
    tempOutOfRangeCount = 0;
    tempRequestFailCount = 0;
    tempConnectedFailCount = 0;
    tempResolutionFixCount = 0;
    tempRereadFailCount = 0;
    tempResolutionFixCrcFailCount = 0;
    tempEnumerateFailCount = 0;
    request->send(200, "text/plain", "OK");
  });

  // Cloud Features debug (raw NVS, no Preferences)
  server.on("/debugToken", HTTP_GET, [](AsyncWebServerRequest *request) {
    char storedToken[256] = "";
    bool haveStored = false;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("cloud", NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
      size_t len = 0;
      err = nvs_get_str(nvs_handle, "authToken", nullptr, &len);

      if (err == ESP_OK && len > 0) {
        if (len >= sizeof(storedToken)) len = sizeof(storedToken) - 1;
        size_t tmpLen = len + 1;  // nvs expects space for null
        err = nvs_get_str(nvs_handle, "authToken", storedToken, &tmpLen);
        if (err == ESP_OK) {
          storedToken[sizeof(storedToken) - 1] = '\0';
          haveStored = true;
        } else {
          Serial.printf("ERROR: nvs_get_str(authToken) failed on second read (err=%d)\n", (int)err);
        }
      } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        Serial.println("NVS: authToken key not found in 'cloud' namespace");
      } else if (err != ESP_OK) {
        Serial.printf("ERROR: nvs_get_str(authToken) len query failed (err=%d)\n", (int)err);
      }

      nvs_close(nvs_handle);
    } else {
      Serial.printf("ERROR: Failed to open NVS namespace 'cloud' (err=%d)\n", (int)err);
    }

    // Redact token to last 4 chars - this endpoint is unauthenticated; the full
    // token would let anyone on the LAN impersonate the device against the cloud.
    auto tail4 = [](const char *s) -> const char * {
      size_t n = strlen(s);
      return (n > 4) ? (s + n - 4) : s;
    };
    char out[512];
    const char *globalTok = authToken.c_str();
    snprintf(out, sizeof(out),
             "isRegistered: %d\nauthToken global: ...%s (len=%u)\nNVS stored token: ...%s (len=%u)\n",
             (int)isRegistered,
             globalTok && *globalTok ? tail4(globalTok) : "(empty)",
             (unsigned)strlen(globalTok),
             haveStored ? tail4(storedToken) : "(empty)",
             haveStored ? (unsigned)strlen(storedToken) : 0u);
    request->send(200, "text/plain", out);
  });

  demoRegisterRoutes();   // GET /demoBackfill (9_demo.ino)
  server.addHandler(&events);
  server.addHandler(&gxEvents);
  PsramPlace::mallocWindow(false);
  server.begin();
  Serial.println("=== MAIN SERVER STARTED ===");
  Serial.println("=== MAIN SERVER SETUP COMPLETE ===");
}
void dnsHandleRequest() {  // process dns request for captive portals
  if (currentMode == MODE_AP || currentMode == MODE_CONFIG) {
    // Only process if there might actually be a request waiting
    // This is a workaround for ESP32-S3 DNS library blocking issues
    static unsigned long lastDNSProcess = 0;
    if (millis() - lastDNSProcess > 10) {  // Throttle to every 10ms for fast captive portal response
      dnsServer.processNextRequest();
      lastDNSProcess = millis();
    }
  }
}
// Engine-off standby. Normally WiFi fully off + 80 MHz. With WiFi Napping in Client mode WiFi stays associated
// in modem-sleep so the dashboard stays reachable (~1 mA over full-off, measured, so no idle timeout); CPU runs
// 240 MHz while a dashboard is connected, else 80 MHz. The temp task is not suspended: its OneWire timing is
// wall-clock and it self-throttles engine-off (see TempTask). Ordering matters: WiFi op first, then the clock.
void enterLowPowerStandby() {
  if (wifiNapEnabled && currentMode == MODE_CLIENT) {
    if (!wifiNapActive) {
      // Napping can be enabled after a standby already took the WIFI_OFF branch below; declaring the nap without
      // raising STA strands checkWiFiConnection() on its WIFI_OFF early-out until ignition-on or GPIO5.
      if (WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);  // seek engine does the join, non-blocking
      WiFi.setSleep(true);           // modem sleep — WiFi naps between DTIM beacons
      wifiNapActive = true;
      queueConsoleMessage("WiFi napping: dashboard stays reachable at low power (no button needed)");
    }
  } else {
    WiFi.mode(WIFI_OFF);             // THIS MUST BE DONE FIRST
    wifiNapActive = false;           // napping disabled (or AP mode) — clear stale nap state
  }
  // 240 MHz only while napping with a dashboard connected; guarded so the PLL isn't reconfigured every pass.
  uint32_t targetMhz = (wifiNapActive && events.count() > 0) ? 240 : 80;
  if (getCpuFrequencyMhz() != targetMhz) {
    setCpuFrequencyMhz(targetMhz);   // THIS MUST BE DONE AFTER THE WIFI OP
  }
}
void checkWiFiConnection() {
  // Non-blocking reconnect engine — never waits on the radio in loop() (Core 1). Presence scans
  // run async on the Core-0 WiFi task; joins are fire-and-poll across loop passes. No
  // backoff, no give-up: a phone hotspot can appear at any moment, and holding the association
  // once joined is what keeps the iPhone's ~90 s no-client hotspot auto-off at bay.
  if (currentMode != MODE_CLIENT) return;

  static int cachedWiFiMode = WIFI_STA;
  static int cachedCpuFreq = 240;
  static unsigned long lastDriverPoll = 0;
  unsigned long now = millis();
  if (now - lastDriverPoll > 2000) {  // getMode/getCpuFrequencyMhz cross cores — poll gently
    cachedWiFiMode = WiFi.getMode();
    cachedCpuFreq = getCpuFrequencyMhz();
    lastDriverPoll = now;
  }
  if (cachedWiFiMode == WIFI_OFF) return;
  if (cachedCpuFreq < 81 && !wifiNapActive) return;  // napping reconnects at 80MHz so a router drop can't strand it

  if (WiFi.status() == WL_CONNECTED) {  // event-cached in the core — no cross-core call
    if (wifiRecon.state != WIFI_RECON_IDLE) {
      wifiRecon.state = WIFI_RECON_IDLE;
      wifiRecon.channel = WiFi.channel();  // seed the single-channel presence scan
      WiFi.scanDelete();
      Serial.printf("WiFi: connected, IP %s, RSSI %d dBm, ch %d\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI(), (int)wifiRecon.channel);
      if (wifiRecon.attemptCount > 0) {
        queueConsoleMessageF("WiFi reconnected after %d join attempts", wifiRecon.attemptCount);
      }
      wifiRecon.attemptCount = 0;
      startMdnsOnce();
      setupServer();  // no-op when already up; covers a boot that had no WiFi (routes never registered)
    }
    static unsigned long lastRssiPoll = 0;
    if (now - lastRssiPoll > 2000) {
      wifiRecon.lastSignalStrength = WiFi.RSSI();
      lastRssiPoll = now;
    }
    return;
  }

  // Disconnected — use cached credentials only (no filesystem reads here)
  if (!cached_wifi_creds_valid || strlen(cached_wifi_ssid) == 0) return;

  if (wifiRecon.state == WIFI_RECON_JOINING) {
    if (now - wifiRecon.joinStart > WIFI_JOIN_TIMEOUT_MS) {
      wifiRecon.attemptCount++;
      Serial.printf("WiFi: join timed out (attempt %d), back to seeking\n", wifiRecon.attemptCount);
      WiFi.disconnect();  // clears the half-finished join; posts to the driver, doesn't wait
      wifiRecon.state = WIFI_RECON_SEEKING;
      wifiRecon.lastScanKickoff = now;
    }
    return;  // join success is caught by the WL_CONNECTED branch on a later pass
  }

  // Seeking: reap a finished presence scan, else kick one off at cadence
  static bool lastScanWasFullSweep = false;
  static uint8_t fullSweepMisses = 0;  // consecutive all-channel sweeps with no sighting
  int16_t sc = WiFi.scanComplete();
  if (sc == WIFI_SCAN_RUNNING) return;

  if (sc >= 0) {
    int32_t seenChannel = 0;
    for (int16_t i = 0; i < sc; i++) {
      if (WiFi.SSID(i) == cached_wifi_ssid) {
        seenChannel = WiFi.channel(i);
        break;
      }
    }
    WiFi.scanDelete();
    if (seenChannel == 0 && lastScanWasFullSweep && fullSweepMisses < 250) {
      if (++fullSweepMisses == 3) {  // ~30 s of all-channel misses; print once, reset on sighting
        Serial.printf("WiFi: '%s' not seen in any scan - name is case-sensitive; check spelling and that it is broadcasting\n", cached_wifi_ssid);
      }
    }
    if (seenChannel > 0) {
      fullSweepMisses = 0;
      wifiRecon.channel = seenChannel;
      Serial.printf("WiFi: '%s' sighted on ch %d - joining\n", cached_wifi_ssid, (int)seenChannel);
      // Fire-and-poll: begin() returns immediately; association + DHCP run on the Core-0 WiFi task
      WiFi.begin(cached_wifi_ssid, strlen(cached_wifi_pass) > 0 ? cached_wifi_pass : nullptr, seenChannel);
      wifiRecon.joinStart = now;
      wifiRecon.state = WIFI_RECON_JOINING;
      return;
    }
  }

  if (now - wifiRecon.lastScanKickoff >= WIFI_SCAN_CADENCE_MS) {
    wifiRecon.lastScanKickoff = now;
    // iPhone hotspots pick a fresh channel per session — sweep all channels every Nth scan
    // (and whenever no channel is cached); otherwise it's 120 ms on one channel.
    bool fullSweep = (wifiRecon.channel == 0) || (++wifiRecon.scansSinceSweep >= WIFI_FULL_SWEEP_EVERY);
    if (fullSweep) wifiRecon.scansSinceSweep = 0;
    lastScanWasFullSweep = fullSweep;
    // Active + show_hidden: a hidden AP beacons with a blank SSID, so a passive scan can never
    // match cached_wifi_ssid. Only a directed probe makes it answer with its real name.
    WiFi.scanNetworks(true, true, false, 120, fullSweep ? 0 : (uint8_t)wifiRecon.channel, cached_wifi_ssid);
    wifiRecon.state = WIFI_RECON_SEEKING;
  }
}
// Queue depth at which a client's link counts as stalled. A healthy link holds 0-1 messages (the
// frame in flight until its ack); 4 means no ack for several frames.
static const uint16_t SSE_BACKLOG_DEPTH = 4;
// A client whose queue has held messages this long with no ack has stopped draining (a stopped tab, a link in a retransmit
// wait): the backlog gate and the current-state channels leave it out while another client drains, so it holds back no one.
static const uint32_t SSE_STALLED_MS = 1000;
static volatile bool sseLeaveStalledOut = false;   // last 10 Hz census: a client stalled while another drains
// Every dashboard event goes out through here so a full client queue is counted instead of silent.
void sseSend(const char *msg, const char *evt, bool toStalled) {
  if (events.count() == 0) return;
  const AsyncEventSource::SendStatus st = (toStalled || !sseLeaveStalledOut) ? events.send(msg, evt)
                                                                           : events.sendSkipStalled(msg, evt, SSE_STALLED_MS);
  if (st != AsyncEventSource::ENQUEUED) sseFramesDropped++;
}
// True while the draining clients' queues are backed up (TCP not draining). CSV1 keeps sending so the queue budget
// is spent on plot frames; the periodic channels wait it out and resend on their next tick, which
// costs nothing because they carry current state, not history.
bool sseBacklogged() { return sseGateDepth >= SSE_BACKLOG_DEPTH; }
void SendWifiData() {
  unsigned long start66 = micros();
  // === THROTTLED WIFI CHECKS (every 2 seconds) ===
  static int cachedWiFiMode = WIFI_OFF;
  static int cachedWiFiConnected = 0;
  static int cachedWiFiRSSI = -100;
  static unsigned long lastWiFiCheck = 0;

  unsigned long now = millis();

  // Dashboard-stream census, ahead of the WiFi gate so a radio-down window counts too. With no listener,
  // events.send() and trySendConsoleSSE discard silently, so the gap is reported on reconnect into the
  // /consolehist.txt ring. The 2 s floor keeps a page reload or tab swap out of the log.
  static uint8_t  sseLastClients = 0;
  static uint32_t sseDownSinceMs = 0;
  static uint16_t sseDropCount = 0;
  uint8_t sseClients = (uint8_t)events.count();
  // phoneLive keeps every phone gate exactly as it was; with no phone, a connected GX gets frames built on its schedule.
  const bool phoneLive = sseClients > 0;
  if (phoneLive != phoneLiveNow) {
    phoneLiveNow = phoneLive;
    if (gxSendTaskHandle) xTaskNotifyGive(gxSendTaskHandle);   // GxRate devPost: the bridge's post interval follows at once
  }
  const uint8_t gxStreamsNow = gxStreamCount;
  const bool gxLive = gxStreamsNow > 0 && gxSendTaskHandle != nullptr;
  gxLiveNow = gxLive;
  gxTierUpdate(now);
  if (gxKickPending) {
    gxKickPending = false;
    for (uint8_t ch = 0; ch < GX_CH_COUNT; ch++) gxKick[ch] = true;
  }
  if (sseLastClients > 0 && sseClients == 0) {
    sseDownSinceMs = now;
    sseDropCount++;
  } else if (sseLastClients == 0 && sseClients > 0 && sseDownSinceMs != 0) {
    if (now - sseDownSinceMs >= 2000)
      queueConsoleMessageF("Dashboard stream was down %.1fs (drop #%u) - anything said in that window reached no browser",
                           (now - sseDownSinceMs) / 1000.0f, (unsigned)sseDropCount);
    sseDownSinceMs = 0;
  }
  // onConnect sets its kicks before the library lists the client (AsyncEventSource::_addClient), and a pass in that gap
  // spends them on the GX or the pages already open: a newly listed dashboard arms them again.
  if (sseClients > sseLastClients) {
    settingsDirty = true;
    gxDevKick = true;
    csv2Kick = true;
    tsKick = true;
  }
  sseLastClients = sseClients;

  // Backpressure witness, 10 Hz: a stall is logged when every queue is empty again, sized from the first client's backlog
  // to full drain, with that client's own depth. Console lines wait while the draining clients are backed up
  // (trySendConsoleSSE is a gated channel) and queue behind a stalled client's backlog.
  static uint32_t sseDepthPollMs = 0, sseStallStartMs = 0, sseStallDropsAtStart = 0;
  static uint16_t sseStallPeak = 0;
  if (now - sseDepthPollMs >= 100) {
    sseDepthPollMs = now;
    const AsyncEventSource::QueueCensus q = events.queueCensus(SSE_STALLED_MS);
    sseQueueDepth = sseClients ? (uint16_t)q.avg : 0;
    sseQueueDeepest = (uint16_t)q.deepest;
    // with every client stalled there is no one to leave out: the gate is the plain average, as with one client
    sseLeaveStalledOut = q.stalled && q.stalled < q.clients;
    sseGateDepth = sseClients ? (uint16_t)(sseLeaveStalledOut ? q.drainingAvg : q.avg) : 0;
    if (sseQueueDepth > sseQueuePeak) sseQueuePeak = sseQueueDepth;
    if (sseQueueDeepest > sseQueueDeepestPeak) sseQueueDeepestPeak = sseQueueDeepest;
    if (sseQueueDeepest >= SSE_BACKLOG_DEPTH) {
      if (!sseStallStartMs) { sseStallStartMs = now; sseStallPeak = 0; sseStallDropsAtStart = sseFramesDropped; }
      if (sseQueueDeepest > sseStallPeak) sseStallPeak = sseQueueDeepest;
    } else if (sseStallStartMs && sseQueueDeepest == 0) {
      uint32_t stallMs = now - sseStallStartMs;
      sseStallStartMs = 0;
      if (stallMs >= 1000) {
        sseStallCount++;
        if (stallMs > sseStallLongestMs) sseStallLongestMs = stallMs;
        queueConsoleMessageF("Dashboard stream stalled %.1fs: the link stopped taking data (WiFi and socket stayed up; send queue peaked %u/%u, %lu frames dropped)",
                             stallMs / 1000.0f, (unsigned)sseStallPeak, (unsigned)SSE_MAX_QUEUED_MESSAGES,
                             (unsigned long)(sseFramesDropped - sseStallDropsAtStart));
      }
    }
  }

  if (now - lastWiFiCheck > 2000) {  // Check WiFi every 2 seconds
    cachedWiFiMode = WiFi.getMode();
    cachedWiFiConnected = (WiFi.status() == WL_CONNECTED) ? 1 : 0;
    cachedWiFiRSSI = WiFi.RSSI();
    lastWiFiCheck = now;
  }

  // Use cached values (no ipc0 calls!)
  if (cachedWiFiMode == WIFI_OFF) return;
  if (currentMode == MODE_CLIENT && !cachedWiFiConnected) return;

  // === END THROTTLED CHECKS ===

  // Static variables for timing control
  static unsigned long prev_millis5 = 0;
  static unsigned long lastConsoleMessageTime = 0;
  static unsigned long lastpayload2send = 0;
  static unsigned long lastpayload3send = 0;
  static unsigned long lastpayload4send = 0;
  static unsigned long lastTimestampSend = 0;
  const unsigned long EVENTSOURCE_COOLDOWN = 10;
  const unsigned long CONSOLE_MESSAGE_INTERVAL = 1000;

  bool canSendNow = (now - lastEventSourceSend >= EVENTSOURCE_COOLDOWN);
  if (!canSendNow) return;
  if (gHeavyRanThisPass) return;   // one-heavy-per-pass gate; defer the send (cooldown unchanged → retries next pass)
  gHeavyRanThisPass = true;

  bool sentSomething = false;

  // One-shot boot announcement, sent only once a browser is connected so it survives boot-time console
  // queue churn and lands right after the post-reboot SSE auto-reconnect.
  static bool bootLineSent = false;
  if (!bootLineSent && events.count() > 0) {
    bootLineSent = true;
    queueConsoleMessageF("BOOTED firmware v%s (reset: %s | esp=%d rtc0=%d rtc1=%d)",
                         FIRMWARE_VERSION, resetReasonName(), g_rawResetEsp, g_rawResetRtc0, g_rawResetRtc1);
    if (g_blackBoxPrevValid) {
      char bbTemp[12];   // -999 is the store-side "no probe had read" marker (loop()); never print it raw
      if (g_blackBoxPrev.altTempF == -999) snprintf(bbTemp, sizeof(bbTemp), "n/a");
      else snprintf(bbTemp, sizeof(bbTemp), "%dF", (int)g_blackBoxPrev.altTempF);
      queueConsoleMessageF("BLACKBOX: up=%lus IBV=%.2fV duty=%.1f%% RPM=%d amps=%.1f altT=%s mode=%u stage=%u loop=%.1fms heap=%ldKB",
                           (unsigned long)(g_blackBoxPrev.upMillis / 1000UL), g_blackBoxPrev.ibv,
                           g_blackBoxPrev.duty, (int)g_blackBoxPrev.rpm, g_blackBoxPrev.measAmps,
                           bbTemp, g_blackBoxPrev.sysMode, g_blackBoxPrev.chargeStage,
                           g_blackBoxPrev.maxLoopUs / 1000.0f, (long)g_blackBoxPrev.minHeapKB);
    } else if (g_blackBoxPrev.magic == BLACKBOX_DEMO_MAGIC) {
      queueConsoleMessage("BLACKBOX: the last session ended in demo mode (simulated readings) - not recorded");
    } else {
      queueConsoleMessage("BLACKBOX: RTC RAM lost - true power interruption preceded this boot");
    }
  }

  // PRIORITY 1: CSVData
  if (!sentSomething && (phoneLive ? (now - prev_millis5 >= webgaugesinterval) : gxDue(0, now, prev_millis5, gxLive))) {
    WifiHeartBeat = WifiHeartBeat + 1;

    static char *payload1 = nullptr;
    static const size_t PAYLOAD1_SIZE = 1400;
    if (!payload1) {
      payload1 = (char *)ps_malloc(PAYLOAD1_SIZE);  // allocated to PSRAM
      if (!payload1) {
        Serial.println("FATAL: payload1 ps_malloc failed");
        return;
      }
    }
    // Consume the protection-event latch for this frame — clear only the bits we read so
    // an event firing on the control-loop core mid-build isn't lost.
    uint8_t protMask = g_protEventLatch;
    g_protEventLatch &= ~protMask;

    // The "Alt Current filtered" plot trace must show the SAME signal the CC current PID
    // acts on — the OutputPIDSigSrc-selected PV (mirrors the ternary in AdjustFieldLearnMode).
    float pidAltPV = (OutputPIDSigSrc == 2) ? MeasuredAmps : (OutputPIDSigSrc == 1) ? g_pidMA_N
                                                                                    : g_pidI_filtered;

    // Fast analog traces report the window mean (default). The Max toggle covers only the
    // current/voltage channels (IBV, Bcur, MeasuredAmps); RPM/BatteryV(ADS)/Channel3V are always
    // mean. winAggValue falls back to the latest value when no sample landed this window.
    float ibvOut   = aggIbv.value(battMaxMode, IBV);
    float bcurOut  = aggBcur.value(battMaxMode, Bcur);
    float altOut   = aggAltCur.value(battMaxMode, MeasuredAmps);
    float battVOut = aggBattV.value(false, BatteryV);
    float rpmOut   = aggRpm.value(false, RPM);
    float ch3Out   = aggCh3.value(false, Channel3V);

    gxBeginWrite(0);
    int payload1Len = snprintf(payload1, PAYLOAD1_SIZE,
                               "%d,"  // CSV1_FIELD_COUNT
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%u,%u",  // +2: sessionId, sendMs

                               CSV1_FIELD_COUNT,
                               !isfinite(AlternatorTemperatureF) ? ROLL_EMPTY : SafeInt(AlternatorTemperatureF, 100),  // no alt-role probe -> "not available" (SafeInt's -1 would render as -0.0F)

                               SafeInt(dutyCycle, 100),
                               SafeInt(battVOut, 100),
                               SafeInt(altOut, 100),
                               SafeInt(rpmOut),
                               SafeInt(ch3Out, 100),
                               SafeInt(ibvOut, 100),
                               HAS_BATT_SHUNT ? SafeInt(bcurOut, 100) : ROLL_EMPTY,   // no shunt -> "not available", never INA input noise
                               SafeInt(VictronVoltage, 100),
                               SafeInt(LoopTime),
                               SafeInt(WifiHeartBeat),
                               SafeInt(vvout, 100),
                               SafeInt(iiout, 100),
                               SafeInt(FreeHeap),
                               SafeInt(Alarm_Status),
                               SafeInt(fieldActiveStatus),
                               SafeInt((int)currentMode),
                               SafeInt(stateRevision),
                               SafeInt(setpointLimited, 100),
                               SafeInt(uTargetAmps, 100),
                               SafeInt(pidInput, 100),
                               SafeInt(pidOutput, 100),
                               SafeInt(pidError, 100),
                               SafeInt(imu_heel_deg, 100),
                               SafeInt(imu_pitch_deg, 100),
                               SafeInt(imu_vertical_accel_g, 1000),
                               SafeInt(imu_yaw_rate_dps, 100),
                               SafeInt(imu_total_accel_g, 1000),
                               SafeInt((int32_t)((millis() - perfCountersResetMs) / 1000UL)),  // seconds since perf-counters reset (0 = boot)
                               SafeInt(shutdownRampActive() ? (int)shutdownPhase : 0),
                               SafeInt(BatteryV, 100),                  // raw ADS1115
                               SafeInt(pidAltPV, 100),  // CSV1_pidAltPV
                               SafeInt(ChargingVoltageTarget * 100),
                               SafeInt(Icv * 100),
                               // Water depth in feet ×10 (0.1 ft resolution). 0 if NMEA depth stale or unavailable.
                               SafeInt(IS_STALE(IDX_WATER_DEPTH) ? 0 : (WaterDepth_m * 3.28084f), 10),
                               SafeInt(Ignition),  // effective ignition (override applied at top of loop)
                               SafeInt(OnOff),     // CSV1_OnOff — master Alternator Enable, same frame as fieldActiveStatus
                               SafeInt(g_mExcessEma, 10),       // CSV1_mExcessEma — averaged current excess (A ×10)
                               SafeInt(g_iExcessThreshold, 10), // CSV1_iExcessThreshold — fire threshold E (A ×10)
                               SafeInt(g_iExcessArmedWin ? g_mExcessEmaPeak : g_mExcessEma, 10),       // CSV1_mExcessEmaPeak (A ×10)
                               SafeInt(g_iExcessArmedWin ? g_iExcessThreshWinMin : g_iExcessThreshold, 10), // CSV1_iExcessThreshMin (A ×10)
                               SafeInt(protMask),              // CSV1_protEventMask — protection-event bits this frame
                               SafeInt(g_fieldEventReason),    // CSV1_fieldEventReason — FieldEventReason code (banner OFF-reason)
                               SafeInt(g_cvPTerm, 100),        // CSV1_cvPTerm — P contribution to Icv (A ×100)
                               SafeInt(cv_I, 100),             // CSV1_cvIterm — I contribution to Icv (A ×100)
                               SafeInt(g_cvKdTrimLive, 100),   // CSV1_cvKdTrim — D back-off at the Icv output (A ×100)
                               SafeInt(g_cvKdFiltV, 100),      // CSV1_cvKdFiltV — IBV smoothed by CvKdVoltFiltTC (V ×100)
                               SafeInt(g_huntDerate, 100),     // CSV1_huntDerate — hunt-governor live Ki derate (×100)
                               SafeInt(g_huntFreqHz, 100),     // CSV1_huntFreqHz — last confirmed wobble frequency (Hz ×100)
                               (int)g_huntState,               // CSV1_huntState — 0 watching, 1 testing a current-loop gain (Ki) cut, 3 cooldown, 4 testing with the voltage damper (D-term) paused (2 unused since v2)
                               SafeInt(g_I_cap, 100),          // CSV1_rpmCeilingAmps — RPM-table current ceiling this tick (A ×100)
                               (unsigned)g_sessionId,          // CSV1_sessionId
                               (unsigned)millis()              // CSV1_sendMs — the reference clock every other channel is aged against
    );
    gxEndWrite(0, payload1, payload1Len, PAYLOAD1_SIZE);
    // Reset the per-frame iExcess sparkline aggregates now that they've been captured.
    g_mExcessEmaPeak = 0.0f;
    g_iExcessThreshWinMin = 0.0f;
    g_iExcessArmedWin = false;
    // Reset the fast-channel window accumulators for the next send interval.
    aggIbv.reset();
    aggBcur.reset();
    aggAltCur.reset();
    aggBattV.reset();
    aggRpm.reset();
    aggCh3.reset();
    if (payload1Len < 0 || payload1Len >= PAYLOAD1_SIZE) {
      Serial.printf("payload1 truncated or format error: %d\n", payload1Len);
      return;
    }

    sseSend(payload1, "CSVData", true);
    gxOffer(0, now, gxLive, !phoneLive);
    SendWifiTime = micros() - start66;
    prev_millis5 = now;
    lastEventSourceSend = now;
    sentSomething = true;
  }
  // PRIORITY 2: CSVData4 / NavStream — live nav/wind/solar/fuel at 2 Hz, right after CSV1 so the slower
  // CSV2/CSV3/TS channels can't starve these gauges.
  if (!sentSomething && (phoneLive ? (now - lastpayload4send >= 500UL) : gxDue(1, now, lastpayload4send, gxLive)) && !sseBacklogged()) {
    static char *payload4 = nullptr;
    static const size_t PAYLOAD4_SIZE = 512;  // ~7 B/field (SafeInt sentinel is 11 chars) plus two 10-digit stamps, with headroom;
                                              // an overrun drops the frame at the length guard below, silently stopping NavStream.
    if (!payload4) {
      payload4 = (char *)ps_malloc(PAYLOAD4_SIZE);  // allocated to PSRAM
      if (!payload4) {
        Serial.println("FATAL: payload4 ps_malloc failed");
        return;
      }
    }
    float flowBattA, flowAltA, flowSolarA;
    int flowSrc;
    uint32_t flowSolarSer;
    powerFlowSample(flowBattA, flowAltA, flowSolarA, flowSrc, flowSolarSer);
    float demoShoreA, demoFridgeA, demoOrionA, demoInverterA;
    demoGxDeviceAmps(demoShoreA, demoFridgeA, demoOrionA, demoInverterA);
    gxBeginWrite(1);
    int payload4Len = snprintf(payload4, PAYLOAD4_SIZE,
                               "%d,"  // CSV4_FIELD_COUNT
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,"
                               "%u,%u",  // +2: sessionId, sendMs

                               CSV4_FIELD_COUNT,
                               SafeInt(HeadingNMEA),                     // CSV4_HeadingNMEA
                               SafeInt(SOGNMEA, 100),                    // CSV4_SOGNMEA
                               SafeInt(COGNMEA),                         // CSV4_COGNMEA
                               !isfinite(STWNMEA) ? ROLL_EMPTY : SafeInt(STWNMEA, 100),                                  // CSV4_STWNMEA (knots ×100)
                               SafeInt(ApparentWindSpeedNMEA, 100),      // CSV4_ApparentWindSpeedNMEA
                               SafeInt(ApparentWindAngleNMEA),           // CSV4_ApparentWindAngleNMEA
                               // NAN (no wind instrument / target bearing / boat speed) -> ROLL_EMPTY; SafeInt maps NaN the same way, so the test is redundant.
                               !isfinite(TrueWindSpeedNMEA) ? ROLL_EMPTY : SafeInt(TrueWindSpeedNMEA, 100),                // CSV4_TrueWindSpeedNMEA
                               !isfinite(TrueWindAngleNMEA) ? ROLL_EMPTY : SafeInt(TrueWindAngleNMEA),                     // CSV4_TrueWindAngleNMEA
                               !isfinite(LeewayNMEA) ? ROLL_EMPTY : SafeInt(LeewayNMEA),                                   // CSV4_LeewayNMEA
                               !isfinite(VMGNMEA) ? ROLL_EMPTY : SafeInt(VMGNMEA, 100),                                    // CSV4_VMGNMEA
                               !isfinite(VMGUpwind) ? ROLL_EMPTY : SafeInt(VMGUpwind, 100),                                // CSV4_VMGUpwind (VMG to windward, knots ×100)
                               SafeInt(VictronSolarPower_W),             // CSV4_VictronSolarPower
                               SafeInt(VictronSolarVoltage_V, 100),      // CSV4_VictronSolarVoltage
                               SafeInt(VictronSolarCurrent_A, 100),      // CSV4_VictronSolarCurrent
                               SafeInt(VictronCurrent, 100),             // CSV4_VictronCurrent
                               SafeInt(currentFuelGPH, 100),             // CSV4_currentFuelGPH
                               SafeInt(currentNMPG, 100),                // CSV4_currentNMPG
                               (int)ctrlLimiter,                         // CSV4_ctrlLimiter -> banner limiter code
                               SafeInt(chargeStageDisplay),              // CSV4_chargeStage -> Plots-tab mode ribbon
                               SafeInt(n183HeadingDeg, 10),              // CSV4_n183Heading (deg ×10; -1 stays -10)
                               (int)n183HdgRef,                          // CSV4_n183HdgRef
                               (int)bmsSignalActive,                     // CSV4_bmsSignalActive
                               (int)panelHiRateInput,                    // CSV4_panelHiRateInput
                               (int)panelForceFloatInput,                // CSV4_panelForceFloatInput
                               SafeInt(veSocFresh() ? VictronSoc : NAN, 10),  // CSV4_VictronSoc
                               (int)gxStreamsNow,                        // CSV4_gxStreams
                               SafeInt(flowBattA, 100),                  // CSV4_flowBattA
                               SafeInt(flowAltA, 100),                   // CSV4_flowAltA
                               SafeInt(flowSolarA, 100),                 // CSV4_flowSolarA
                               flowSrc,                                  // CSV4_flowSrc
                               (int)flowSolarSer,                        // CSV4_flowSolarSer (31 bits, never negative)
                               SafeInt(demoShoreA, 100),                 // CSV4_demoShoreA
                               SafeInt(demoFridgeA, 100),                // CSV4_demoFridgeA
                               SafeInt(demoOrionA, 100),                 // CSV4_demoOrionA
                               SafeInt(demoInverterA, 100),              // CSV4_demoInverterA
                               (unsigned)g_sessionId,                    // CSV4_sessionId
                               (unsigned)millis()                        // CSV4_sendMs
    );
    gxEndWrite(1, payload4, payload4Len, PAYLOAD4_SIZE);
    if (payload4Len < 0 || payload4Len >= PAYLOAD4_SIZE) {
      Serial.printf("payload4 truncated or format error: %d\n", payload4Len);
      return;
    }
    sseSend(payload4, "CSVData4");
    gxOffer(1, now, gxLive, !phoneLive);
    lastpayload4send = now;
    lastEventSourceSend = now;
    sentSomething = true;
  }
  // PRIORITY 3: Console
  trySendConsoleSSE(sentSomething, now);
  // PRIORITY 4: CSVData2. Every 5 s gated behind CSV1; during a plant delay test every 500 ms, bypassing the
  // gate, so the UI phase-progress display stays within one poll tick of the firmware.
  const bool sysIDRunning = (systemIDActive != 0);
  if ((sysIDRunning || !sentSomething)
      && (phoneLive ? (csv2Kick || now - lastpayload2send >= (sysIDRunning ? 500UL : 5000UL)) : gxDue(2, now, lastpayload2send, gxLive))
      && !sseBacklogged()) {
    static char *payload2 = nullptr;
    // Sized by the ~7 B/field rule rather than the observed frame, because an overflow returns early
    // from SendWifiData and kills CSV2 + CSV3 + TS until reboot. PSRAM.
    static const size_t PAYLOAD2_SIZE = 8192;
    static int payload2Len = 0;       // persists between the build pass and the send pass
    static uint8_t csv2Phase = 0;     // 0 = build this due pass, 1 = send the built payload next pass
    static unsigned long csv2BuiltMs = 0;
    // The send pass's gate can fail for hours (last client gone, queue backed up): a frame that old is rebuilt, never sent.
    if (csv2Phase == 1 && now - csv2BuiltMs > 1000UL) csv2Phase = 0;
    if (!payload2) {
      payload2 = (char *)ps_malloc(PAYLOAD2_SIZE);  // allocated to PSRAM
      if (!payload2) {
        Serial.println("FATAL: payload2 ps_malloc failed");
        return;
      }
    }
    if (csv2Phase == 0) {
    // PASS 1 — build only. The snprintf is the ~4.3 ms cost; the events.send is deferred to PASS 2
    // (next pass) so build + send never stack in one loop tick (CH1/Vbus loop-stall work).
    WifiStrength = cachedWiFiRSSI;
    ch1_compute_stats();
    pidFire_compute_stats();
    voltLoop_compute_stats();
    // Format string:
    gxBeginWrite(2);
    uint32_t _csv2b0 = micros();   // CSV2 build-cost timer (snprintf)
        payload2Len = snprintf(payload2, PAYLOAD2_SIZE,
                               "%d,"  // CSV2_FIELD_COUNT
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%u,%u,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,"   // +4 NMEA 0183: sentences, checksum errors, drain worst us window/session
                               "%d,"            // +1 enforced field-duty ceiling (x100)
                               "%d,%d,"         // +2 DVCC authority identity: NAME manufacturer code, product code
                               "%d,%d,"         // +2 timed OV cut session counters: LOW tier, MID tier
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"  // +10 solar ledger: today's pred/act harvest + consumption, bar, source, days, coverage, ft win/ses
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"  // +13 battery/extra temperature: BATT probe, EXTRA probe, active F, active src, probe count, unassigned, VE.Direct T, RV-C T, derate inert, batt wm lo/hi, extra wm lo/hi
                               "%d,"            // +1 enforced field-duty floor (x100)
                               "%d,%d,"         // +2 INA228 die temperature: live, session max (°F x10)
                               "%d,%d,%d,"      // +3 sensor-fault field-event counters: voltage stale, voltage2 stale, battery temp lost
                               "%d,%d,%d,%d,%d,%d,"   // +6 sensor faults: fault mask, battery-temperature state, limp tier, steady mask, required-fault count, battery-current state
                               "%d,%d,%d,%d,%lu,%lu,%lu,%lu,%d,"   // +9 board field ceilings: current, heat, binding term, corrected R, lifetime/session seconds at each, die-temp-lost count
                               "%d,%d,%d,%d,%d,%d,%d,"   // +7 NMEA 2000 bus health: verdict, TEC, REC, frames received, bus-offs, lost frames, nodes heard
                               "%d,"            // +1 largest free PSRAM block (KB)
                               "%d,%d,%d,%d,"   // +4 DVCC Victron carriers: VE.Can CVL/CCL, WiFi CVL/CCL
                               "%d,%d,"         // +2 DVCC: VE.Can share addressed to this node, Stop Charging Without Limits holding
                               "%u,%u\n",       // +2: sessionId, sendMs
                               CSV2_FIELD_COUNT,
                               SafeInt(IBVMax, 100),
                               SafeInt(MeasuredAmpsMax, 100),
                               SafeInt(RPMMax),
                               socShownOk() ? SafeInt(socShownX100()) : ROLL_EMPTY,   // no shunt and no network monitor -> SOC_percent is a frozen NVS leftover
                               SafeInt((double)EngineRunTime * 100.0 / 3600.0, 1),    // double: int math overflows (UB) at 5,965 engine-hours
                               SafeInt((double)AlternatorOnTime * 100.0 / 3600.0, 1),
                               SafeInt(AlternatorFuelUsed, 100),
                               SafeInt(ChargedEnergy),
                               SafeInt(DischargedEnergy),
                               SafeInt(AlternatorChargedEnergy),
                               SafeInt(MaxAlternatorTemperatureF),
                               SafeInt(temperatureThermistor),
                               SafeInt(MaxTemperatureThermistor),
                               SafeInt(timeToFullChargeMin),
                               SafeInt(timeToFullDischargeMin),
                               SafeInt(LatitudeNMEA * 1000000),
                               SafeInt(LongitudeNMEA * 1000000),
                               SafeInt(SatelliteCountNMEA),
                               SafeInt(LastSessionDuration),
                               SafeInt(LastSessionMaxLoopTime),
                               SafeInt(lastSessionMinHeap),
                               SafeInt(wifiReconnectsTotal),
                               SafeInt(LastResetReason),
                               SafeInt(ancientResetReason),
                               SafeInt(totalPowerCycles),
                               SafeInt(MinFreeHeap),
                               SafeInt(currentWeatherMode),
                               SafeInt(UVToday, 100),
                               SafeInt(UVTomorrow, 100),
                               SafeInt(UVDay2, 100),
                               SafeInt(weatherDataValid),
                               SafeInt(alarmLatch ? 1 : 0),
                               SafeInt(ResetAlarmLatch),
                               SafeInt(DynamicAltCurrentZero, 1000),
                               SafeInt(InsulationLifePercent, 100),
                               SafeInt(GreaseLifePercent, 100),
                               SafeInt(BrushLifePercent, 100),
                               SafeInt(PredictedLifeHours),
                               SafeInt(LifeIndicatorColor),
                               SafeInt(pKwHrToday, 100),
                               SafeInt(pKwHrTomorrow, 100),
                               SafeInt(pKwHr2days, 100),
                               !isfinite(ambientTemp) ? ROLL_EMPTY : SafeInt(ambientTemp),   // no BMP388 -> "—"
                               SafeInt(baroPressure, 10),
                               SafeInt(firmwareVersionInt),
                               deviceIdUpper,
                               deviceIdLower,
                               SafeInt(ChargedEnergy_AllTime),
                               SafeInt(AlternatorFuelUsed_AllTime, 100),
                               SafeInt(PeakVoltage_AllTime, 100),
                               SafeInt(EngineRunTime_AllTime * 100 / 3600, 1),
                               SafeInt(MinVoltage, 100),
                               SafeInt(MinVoltage_AllTime, 100),
                               SafeInt(ChargeCycles, 100),
                               SafeInt(ChargeCycles_AllTime, 100),
                               SafeInt(EngineFuelUsed, 100),
                               SafeInt(EngineFuelUsed_AllTime, 100),
                               SafeInt(TotalDistance, 10),
                               SafeInt(TotalDistance_AllTime, 10),
                               SafeInt(MaxSpeed, 100),
                               SafeInt(MaxSpeed_AllTime, 100),
                               SafeInt(SolarChargedEnergy),
                               SafeInt(SolarChargedEnergy_AllTime),
                               SafeInt(AlternatorChargedEnergy_AllTime),
                               SafeInt(DischargedEnergy_AllTime),
                               SafeInt(AvgSOC_AllTime, 100),
                               SafeInt(AvgSpeed_AllTime, 100),
                               SafeInt(AvgSpeed, 100),
                               SafeInt(AlternatorOnTime_AllTime * 100 / 3600, 1),
                               SafeInt(EngineCycles_AllTime),
                               SafeInt(MaxAlternatorTemperatureF_AllTime),
                               SafeInt(MaxTemperatureThermistor_AllTime),
                               SafeInt(MeasuredAmpsMax_AllTime, 100),
                               SafeInt(RPMMax_AllTime),
                               SafeInt(inBulkStage ? 1 : 0),
                               SafeInt( (wifiWakeStart > 0 && (millis() - wifiWakeStart) < WIFI_WAKE_DURATION) ? (WIFI_WAKE_DURATION - (millis() - wifiWakeStart)) / 1000 : (pendingShutdownFlush && shutdownCloudDeadlineMs > millis()) ? (shutdownCloudDeadlineMs - millis()) / 1000 : 0),
                               SafeInt(bufferedRecordCount),
                               SafeInt((bufferedRecordCount * 100) / SENSOR_RING_SIZE),
                               SafeInt(SENSOR_RING_SIZE),
                               SafeInt(VMGTargetBearing),
                               SafeInt(cpuLoadCore0),
                               SafeInt(cpuLoadCore0Max),
                               SafeInt(cpuLoadCore1),
                               SafeInt(cpuLoadCore1Max),
                               SafeInt(hasForcedUpdate ? 1 : 0),
                               SafeInt(forcedFwVersionInt),
                               (forcedUpdateDeadline),
                               SafeInt(stateRevision),
                               SafeInt(imu_accel_x_raw, 1000),
                               SafeInt(imu_accel_y_raw, 1000),
                               SafeInt(imu_accel_z_raw, 1000),
                               SafeInt(imu_gyro_x_raw, 100),
                               SafeInt(imu_gyro_y_raw, 100),
                               SafeInt(imu_gyro_z_raw, 100),
                               SafeInt(imuWindow->accel_x_min),
                               SafeInt(imuWindow->accel_x_max),
                               SafeInt(imuWindow->accel_x_valid_us > 0 ? (int)((int64_t)imuWindow->accel_x_area_v_us / (int64_t)imuWindow->accel_x_valid_us) : 0),
                               SafeInt(imuWindow->accel_y_min),
                               SafeInt(imuWindow->accel_y_max),
                               SafeInt(imuWindow->accel_y_valid_us > 0 ? (int)((int64_t)imuWindow->accel_y_area_v_us / (int64_t)imuWindow->accel_y_valid_us) : 0),
                               SafeInt(imuWindow->accel_z_min),
                               SafeInt(imuWindow->accel_z_max),
                               SafeInt(imuWindow->accel_z_valid_us > 0 ? (int)((int64_t)imuWindow->accel_z_area_v_us / (int64_t)imuWindow->accel_z_valid_us) : 0),
                               SafeInt(imuWindow->gyro_x_min),
                               SafeInt(imuWindow->gyro_x_max),
                               SafeInt(imuWindow->gyro_x_valid_us > 0 ? (int)((int64_t)imuWindow->gyro_x_area_v_us / (int64_t)imuWindow->gyro_x_valid_us) : 0),
                               SafeInt(imuWindow->gyro_y_min),
                               SafeInt(imuWindow->gyro_y_max),
                               SafeInt(imuWindow->gyro_y_valid_us > 0 ? (int)((int64_t)imuWindow->gyro_y_area_v_us / (int64_t)imuWindow->gyro_y_valid_us) : 0),
                               SafeInt(imuWindow->gyro_z_min),
                               SafeInt(imuWindow->gyro_z_max),
                               SafeInt(imuWindow->gyro_z_valid_us > 0 ? (int)((int64_t)imuWindow->gyro_z_area_v_us / (int64_t)imuWindow->gyro_z_valid_us) : 0),
                               SafeInt(imuWindow->heel_min),
                               SafeInt(imuWindow->heel_max),
                               SafeInt(imuWindow->heel_valid_us > 0 ? (int)((int64_t)imuWindow->heel_area_v_us / (int64_t)imuWindow->heel_valid_us) : 0),
                               SafeInt(imuWindow->pitch_min),
                               SafeInt(imuWindow->pitch_max),
                               SafeInt(imuWindow->pitch_valid_us > 0 ? (int)((int64_t)imuWindow->pitch_area_v_us / (int64_t)imuWindow->pitch_valid_us) : 0),
                               SafeInt(imuWindow->vertical_accel_min),
                               SafeInt(imuWindow->vertical_accel_max),
                               SafeInt(imuWindow->vertical_accel_valid_us > 0 ? (int)((int64_t)imuWindow->vertical_accel_area_v_us / (int64_t)imuWindow->vertical_accel_valid_us) : 0),
                               SafeInt(imuWindow->total_accel_min),
                               SafeInt(imuWindow->total_accel_max),
                               SafeInt(imuWindow->total_accel_valid_us > 0 ? (int)((int64_t)imuWindow->total_accel_area_v_us / (int64_t)imuWindow->total_accel_valid_us) : 0),
                               SafeInt(imuWindow->slam_count),
                               SafeInt(imuWindow->slam_peak_max),
                               SafeInt(imu_slam_count_lifetime),
                               SafeInt(imu_capsize_count),
                               SafeInt(imu_pitchpole_count),
                               SafeInt(imuWindow->heel_change_60s),
                               SafeInt(imuWindow->heel_deviation_60s),
                               SafeInt(imuWindow->pitch_change_60s),
                               SafeInt(imuWindow->pitch_deviation_60s),
                               SafeInt(imuWindow->wave_period),
                               SafeInt(imu_heel_max_lifetime, 100),
                               SafeInt(imu_pitch_max_lifetime, 100),
                               SafeInt(imu_slam_peak_lifetime, 1000),
                               SafeInt(imu_fifo_overrun_count),
                               SafeInt(imu_i2c_error_count),
                               SafeInt(imu_unknown_tag_count),
                               SafeInt(imuRingBuffer ? imuRingBuffer->accel_dropped : 0),
                               SafeInt(imuRingBuffer ? imuRingBuffer->gyro_dropped : 0),
                               SafeInt(imu_total_samples_accel),
                               SafeInt(imu_total_samples_gyro),
                               SafeInt(IMUReadTime2),
                               SafeInt(IMUReadTime),
                               SafeInt(adsI2CErrorCount),
                               SafeInt(tempPIDActive ? 1 : 0),
                               SafeInt(tempPIDInput_d, 100),
                               SafeInt(tempPIDSetpoint_d, 100),
                               SafeInt(thermalPenaltyAmps, 100),
                               SafeInt(innerTermP, 100),
                               SafeInt(innerTermI, 100),
                               SafeInt(innerTermD, 100),
                               SafeInt(outerTermP, 100),
                               SafeInt(outerTermI, 100),
                               SafeInt(outerTermLookahead, 100),
                               SafeInt(thermalSlopeFPerSec, 1000),
                               SafeInt(chargeStageDisplay),
                               SafeInt(voltageControlActive),
                               SafeInt((ChargingVoltageTarget - getBatteryVoltage()) * 100),
                               SafeInt(cv_I * 100),
                               SafeInt(inIdleStage),
                               altHaveFront(),
                               SafeInt(ft_rai_total.worstWindow),
                               SafeInt(ft_rai_total.worstSession),
                               SafeInt(ft_rai_ina228.worstWindow),
                               SafeInt(ft_rai_ina228.worstSession),
                               SafeInt(ft_rai_ads_state.worstWindow),
                               SafeInt(ft_rai_ads_state.worstSession),
                               SafeInt(ft_rai_bmp_state.worstWindow),
                               SafeInt(ft_rai_bmp_state.worstSession),
                               SafeInt(ft_rai_imu.worstWindow),
                               SafeInt(ft_rai_imu.worstSession),
                               SafeInt(tempReadFailCount),
                               SafeInt(tempCrcFailCount),
                               SafeInt(tempCrcRecoveredCount),
                               SafeInt(tempAllFFCount),
                               SafeInt(tempPowerOn85Count),
                               SafeInt(tempOutOfRangeCount),
                               SafeInt(tempRequestFailCount),
                               SafeInt(tempConnectedFailCount),
                               SafeInt(tempResolutionFixCount),
                               SafeInt(tempRereadFailCount),
                               SafeInt(tempResolutionFixCrcFailCount),
                               SafeInt(tempEnumerateFailCount),
                               SafeInt(warmupCeiling),
                               SafeInt(imu_min_moving_gentle),
                               SafeInt(imu_min_moving_moderate),
                               SafeInt(imu_min_moving_rough),
                               SafeInt(imu_min_moving_extreme),
                               SafeInt(imu_min_stat_gentle),
                               SafeInt(imu_min_stat_moderate),
                               SafeInt(imu_min_stat_rough),
                               SafeInt(imu_min_stat_extreme),
                               SafeInt(imu_heel_deviation_120s, 100),
                               SafeInt(imu_pitch_deviation_120s, 100),
                               SafeInt(imu_heading_swing_120s, 10),
                               HAS_BATT_SHUNT ? SafeInt(g_dBcur_dt, 10) : ROLL_EMPTY,
                               (int)g_loadDumpActive,
                               SafeInt(ft_updateAccelMetrics.worstWindow),
                               SafeInt(ft_updateAccelMetrics.worstSession),
                               SafeInt(WifiStrength),
                               SafeInt(SendWifiTime),
                               SafeInt(AnalogReadTime),
                               SafeInt(VeTime),
                               SafeInt(MaximumLoopTime),
                               SafeInt(EngineCycles),
                               SafeInt(CurrentSessionDuration),
                               SafeInt(currentPartitionType),
                               SafeInt(g_fastOvCurrentCap, 100),
                               SafeInt(g_fastOvClampCount),
                               SafeInt(g_fastOvHardCount),
                               SafeInt(ch1_last_ms),
                               SafeInt(ch1_avg_10s, 100),
                               SafeInt(ch1_worst_10s),
                               SafeInt(ch1_over2x_10s),
                               SafeInt(ch1_n_10s),
                               SafeInt(ch1_avg_2m, 100),
                               SafeInt(ch1_worst_2m),
                               SafeInt(ch1_over2x_2m),
                               SafeInt(ch1_n_2m),
                               SafeInt(ch1_avg_at, 100),
                               SafeInt(ch1_worst_at),
                               SafeInt(ch1_over2x_at),
                               SafeInt(ch1_n_at),
                               SafeInt(g_iExcessCount),
                               SafeInt(g_inaOVCount),
                               SafeInt(g_hardOCCount),
                               SafeInt(g_voltSpikeCount),
                               SafeInt(g_voltDisagreeCritCount),
                               SafeInt(g_voltDisagreeWarnCount),
                               SafeInt(g_voltImplausibleCount),
                               SafeInt(g_tempCritCount),
                               SafeInt(g_tempSustainedCount),
                               SafeInt(g_tempStaleCount),
                               SafeInt(g_currentStaleCount),
                               SafeInt(imu_msi_score, 100),
                               SafeInt(imu_vomit_pct, 100),
                               SafeInt(imu_anchorage_comfort, 100),
                               SafeInt(ina_last_ms),
                               SafeInt(ina_avg_10s, 100),
                               SafeInt(ina_worst_10s),
                               SafeInt(ina_over2x_10s),
                               SafeInt(ina_avg_2m, 100),
                               SafeInt(ina_worst_2m),
                               SafeInt(ina_over2x_2m),
                               SafeInt(ina_avg_at, 100),
                               SafeInt(ina_worst_at),
                               SafeInt(ina_over2x_at),
                               SafeInt(loopTime5sWindow / 1000),
                               SafeInt(MaximumLoopTime / 1000),
                               SafeInt(ft_SendWifiData.worstWindow),
                               SafeInt(ft_SendWifiData.worstSession),
                               SafeInt(ft_CheckAlarms.worstWindow),
                               SafeInt(ft_CheckAlarms.worstSession),
                               SafeInt(ft_calculateDerivedMetrics.worstWindow),
                               SafeInt(ft_calculateDerivedMetrics.worstSession),
                               SafeInt(ft_logDashboardValues.worstWindow),
                               SafeInt(ft_logDashboardValues.worstSession),
                               SafeInt(ft_updateSystemHealthStats.worstWindow),
                               SafeInt(ft_updateSystemHealthStats.worstSession),
                               SafeInt(ft_checkWiFiConnection.worstWindow),
                               SafeInt(ft_checkWiFiConnection.worstSession),
                               SafeInt(ft_ch1_compute_stats.worstWindow),
                               SafeInt(ft_ch1_compute_stats.worstSession),
                               SafeInt(ft_UpdateBatterySOC.worstWindow),
                               SafeInt(ft_UpdateBatterySOC.worstSession),
                               SafeInt(ft_updateSensorWindow.worstWindow),
                               SafeInt(ft_updateSensorWindow.worstSession),
                               SafeInt(ft_checkTimeSync.worstWindow),
                               SafeInt(ft_checkTimeSync.worstSession),
                               SafeInt(ft_zeroLogService.worstWindow),
                               SafeInt(ft_zeroLogService.worstSession),
                               SafeInt(ft_bhFlushCapNVS.worstWindow),
                               SafeInt(ft_bhFlushCapNVS.worstSession),
                               SafeInt(ft_kneeLearnService.worstWindow),
                               SafeInt(ft_kneeLearnService.worstSession),
                               SafeInt(currentRPMTableIndex),
                               SafeInt(pidInitialized ? 1 : 0),
                               SafeInt(pidSetpoint, 100),
                               !isfinite(TempToUse) ? ROLL_EMPTY : SafeInt(TempToUse),   // no probe on either source -> "—"
                               SafeInt(learningTargetFromRPM, 100),
                               SafeInt(finalLearningTarget, 100),
                               SafeInt(overheatingPenaltyTimer / 1000),
                               SafeInt(overheatingPenaltyAmps, 100),
                               SafeInt(timeSinceLastOverheat / 1000),
                               SafeInt(socInfoAvailable),
                               SafeInt(overheatCount[0]),
                               SafeInt(overheatCount[1]),
                               SafeInt(overheatCount[2]),
                               SafeInt(overheatCount[3]),
                               SafeInt(overheatCount[4]),
                               SafeInt(overheatCount[5]),
                               SafeInt(overheatCount[6]),
                               SafeInt(overheatCount[7]),
                               SafeInt(overheatCount[8]),
                               SafeInt(overheatCount[9]),
                               SafeInt(cumulativeNoOverheatTime[0] / 1000),
                               SafeInt(cumulativeNoOverheatTime[1] / 1000),
                               SafeInt(cumulativeNoOverheatTime[2] / 1000),
                               SafeInt(cumulativeNoOverheatTime[3] / 1000),
                               SafeInt(cumulativeNoOverheatTime[4] / 1000),
                               SafeInt(cumulativeNoOverheatTime[5] / 1000),
                               SafeInt(cumulativeNoOverheatTime[6] / 1000),
                               SafeInt(cumulativeNoOverheatTime[7] / 1000),
                               SafeInt(cumulativeNoOverheatTime[8] / 1000),
                               SafeInt(cumulativeNoOverheatTime[9] / 1000),
                               SafeInt(learningUpCount[0]),
                               SafeInt(learningUpCount[1]),
                               SafeInt(learningUpCount[2]),
                               SafeInt(learningUpCount[3]),
                               SafeInt(learningUpCount[4]),
                               SafeInt(learningUpCount[5]),
                               SafeInt(learningUpCount[6]),
                               SafeInt(learningUpCount[7]),
                               SafeInt(learningUpCount[8]),
                               SafeInt(learningUpCount[9]),
                               SafeInt(totalLearningEvents),
                               SafeInt(totalOverheats),
                               SafeInt(totalSafeHours),
                               SafeInt(FreeInternalRam),
                               SafeInt(TotalInternalRam),
                               SafeInt(LargestInternalBlock),
                               SafeInt(FreePSRAM),
                               SafeInt(TotalPSRAM),
                               SafeInt(Heapfrag),
                               SafeInt(ft_ReadAnalogInputs.worstWindow),
                               SafeInt(ft_ReadAnalogInputs.worstSession),
                               SafeInt(ft_AdjustFieldLearnMode.worstWindow),
                               SafeInt(ft_AdjustFieldLearnMode.worstSession),
                               SafeInt(ft_uploadSensorHistory.worstWindow),
                               SafeInt(ft_uploadSensorHistory.worstSession),
                               SafeInt(ft_uploadBufferedRecords.worstWindow),
                               SafeInt(ft_uploadBufferedRecords.worstSession),
                               SafeInt(ft_buildConfigPayload.worstWindow),
                               SafeInt(ft_buildConfigPayload.worstSession),
                               SafeInt(VeTime2),
                               (int)systemIDRiseDelay_ms[0],
                               (int)systemIDRiseDelay_ms[1],
                               (int)systemIDRiseDelay_ms[2],
                               (int)systemIDFallDelay_ms[0],
                               (int)systemIDFallDelay_ms[1],
                               (int)systemIDFallDelay_ms[2],
                               (int)systemIDRiseAvg_ms,
                               (int)systemIDFallAvg_ms,
                               SafeInt(ft_altHealth.worstWindow),
                               SafeInt(ft_altHealth.worstSession),
                               SafeInt(ft_altFold.worstWindow),
                               SafeInt(ft_altFold.worstSession),
                               SafeInt(ft_boatPerf.worstWindow),
                               SafeInt(ft_boatPerf.worstSession),
                               SafeInt(ft_huntGov.worstWindow),
                               SafeInt(ft_huntGov.worstSession),
                               (int)systemIDActive,
                               (int)systemIDResultsReady,
                               (int)(systemIDStepAmp_A[0] * 10),
                               (int)(systemIDStepAmp_A[1] * 10),
                               (int)(systemIDStepAmp_A[2] * 10),
                               (int)(systemIDQuietPP_A[0] * 10),
                               (int)(systemIDQuietPP_A[1] * 10),
                               (int)(systemIDQuietPP_A[2] * 10),
                               (int)systemIDAbortReason,
                               (int)systemIDAbortPhase,
                               SafeInt(vl_last_ms),
                               SafeInt(vl_avg_10s, 100),
                               SafeInt(vl_worst_10s),
                               SafeInt(vl_over2x_10s),
                               SafeInt(vl_avg_2m, 100),
                               SafeInt(vl_worst_2m),
                               SafeInt(vl_over2x_2m),
                               SafeInt(vl_avg_at, 100),
                               SafeInt(vl_worst_at),
                               SafeInt(vl_over2x_at),
                               (int)((lastNVSSaveTime == 0) ? 0 : ((millis() - lastNVSSaveTime) / 1000UL)),
                               SafeInt(nvsFullSaveLastMs),
                               SafeInt(nvsFullSaveWorstMs),
                               SafeInt(nvsFullSaveCount),
                               wmIgnCsv(wmIgn_amps.lo, 1),
                               wmIgnCsv(wmIgn_amps.hi, 1),
                               wmIgnCsv(wmIgn_altTempF.lo, 1),
                               wmIgnCsv(wmIgn_altTempF.hi, 1),
                               wmIgnCsv(wmIgn_IBV.lo, 10),
                               wmIgnCsv(wmIgn_IBV.hi, 10),
                               wmIgnCsv(wmIgn_Bcur.lo, 1),
                               wmIgnCsv(wmIgn_Bcur.hi, 1),
                               wmIgnCsv(wmIgn_SOC.lo, 1),
                               wmIgnCsv(wmIgn_SOC.hi, 1),
                               wmIgnCsv(wmIgn_RPM.lo, 1),
                               wmIgnCsv(wmIgn_RPM.hi, 1),
                               wmIgnCsv(wmIgn_SOG.lo, 1),
                               wmIgnCsv(wmIgn_SOG.hi, 1),
                               wmIgnCsv(wmIgn_AWS.lo, 1),
                               wmIgnCsv(wmIgn_AWS.hi, 1),
                               wmIgnCsv(wmIgn_TWS.lo, 1),
                               wmIgnCsv(wmIgn_TWS.hi, 1),
                               wmIgnCsv(wmIgn_heel.lo, 1),
                               wmIgnCsv(wmIgn_heel.hi, 1),
                               wmIgnCsv(wmIgn_pitch.lo, 1),
                               wmIgnCsv(wmIgn_pitch.hi, 1),
                               wmIgnCsv(wmIgn_vacc.lo, 10),
                               wmIgnCsv(wmIgn_vacc.hi, 10),
                               wmIgnCsv(wmIgn_baro.lo, 1),
                               wmIgnCsv(wmIgn_baro.hi, 1),
                               wmIgnCsv(wmIgn_ambient.lo, 1),
                               wmIgnCsv(wmIgn_ambient.hi, 1),
                               (int)restartRemainingSec,
                               (int)currentGpsSource,
                               (int)currentTimeSource,
                               (int)currentSpeedSource,
                               (int)loggingActive,
                               SafeInt(sustainedTWS, 10),
                               SafeInt(currentGaleMinutes, 1),
                               wmIgnCsv(wmIgn_VMGman.lo, 10),
                               wmIgnCsv(wmIgn_VMGman.hi, 10),
                               wmIgnCsv(wmIgn_VMGup.lo, 10),
                               wmIgnCsv(wmIgn_VMGup.hi, 10),
                               SafeInt(altWorstPct(), 10),
                               altStatus(),
                               SafeInt(altCoveragePct(), 10),
                               altFrontCount(),
                               SafeInt(VictronChargeState),
                               SafeInt(VictronMPPTMode),
                               SafeInt(VictronError),
                               SafeInt(VictronYieldToday_kWh, 100),
                               SafeInt(VictronMaxPowerToday_W),
                               SafeInt(VictronYieldYesterday_kWh, 100),
                               SafeInt(VictronMaxPowerYesterday_W),
                               SafeInt(fuelCurveNMPG[0], 100),
                               SafeInt(fuelCurveNMPG[1], 100),
                               SafeInt(fuelCurveNMPG[2], 100),
                               SafeInt(fuelCurveNMPG[3], 100),
                               SafeInt(fuelCurveNMPG[4], 100),
                               SafeInt(fuelCurveNMPG[5], 100),
                               SafeInt(fuelCurveNMPG[6], 100),
                               SafeInt(fuelCurveNMPG[7], 100),
                               SafeInt(fuelCurveNMPG[8], 100),
                               SafeInt(fuelCurveNMPG[9], 100),
                               SafeInt(fuelCurveNMPG[10], 100),
                               SafeInt(fuelCurveNMPG[11], 100),
                               SafeInt(fuelCurveNMPG[12], 100),
                               SafeInt(fuelCurveNMPG[13], 100),
                               SafeInt(fuelCurveNMPG[14], 100),
                               SafeInt(fuelCurveNMPG[15], 100),
                               SafeInt(fuelCurveNMPG[16], 100),
                               SafeInt(fuelCurveNMPG[17], 100),
                               SafeInt(currentFuelTopRPM),
                               SafeInt(loopWorst80Win / 1000),
                               SafeInt(loopWorst80Ses / 1000),
                               SafeInt(loopOver80ImuLimitCount),
                               SafeInt(loop80IterCount),
                               SafeInt(loopFieldOnWin / 1000),
                               SafeInt(loopFieldOnSes / 1000),
                               !isfinite(tempFiltered) ? ROLL_EMPTY : SafeInt(tempFiltered, 100),   // NAN until the first sane read -> "—", never a real -0.01F in the thermal plot
                               SafeInt(outerImpliedPenalty, 100),
                               SafeInt((tempPIDActive ? (1 << 0) : 0) | (sysMode == SYS_MODE_AUTO ? (1 << 4) : 0) | (shutdownRampActive() ? (1 << 5) : 0)),
                               SafeInt(thermalAntiWindupLatch ? 1 : 0),
                               SafeInt(pf_last_ms),
                               SafeInt(pf_avg_10s, 100),
                               SafeInt(pf_worst_10s),
                               SafeInt(pf_over2x_10s),
                               SafeInt(pf_avg_2m, 100),
                               SafeInt(pf_worst_2m),
                               SafeInt(pf_over2x_2m),
                               SafeInt(pf_avg_at, 100),
                               SafeInt(pf_worst_at),
                               SafeInt(pf_over2x_at),
                               SafeInt(inaBusReadWorstUs),
                               SafeInt(inaBusSlowCount),
                               SafeInt(ina228ErrorCount),
                               SafeInt(imuFifoFetchWorstUs),
                               SafeInt(imuFifoWorstSamples),
                               SafeInt(ft_dumpLongTermRing.worstWindow),
                               SafeInt(ft_dumpLongTermRing.worstSession),
                               SafeInt(ft_fastAltDrain.worstWindow),
                               SafeInt(ft_fastAltDrain.worstSession),
                               SafeInt(ft_faMatrixFlush.worstWindow),
                               SafeInt(ft_faMatrixFlush.worstSession),
                               SafeInt(faDetLastComputeUs),
                               SafeInt(faDetWorstComputeUs),
                               SafeInt(ft_faWindowFinalize.worstWindow),
                               SafeInt(ft_faWindowFinalize.worstSession),
                               SafeInt(faChanState),
                               SafeInt(faCellsUsed),
                               SafeInt(faDetectLastK),
                               SafeInt(faSesPkpkWorstA, 100),
                               SafeInt(faSesPeakWorstA, 100),
                               SafeInt(faSesPeakWorstHz, 10),
                               SafeInt(faAnomalyCount),
                               SafeInt(faDomFreqHzX10),
                               SafeInt(faDomAmpAX100),
                               SafeInt(faDomRpm),
                               SafeInt(faDomAmpsA, 10),
                               SafeInt(faDomTempF, 10),
                               SafeInt((double)faDomEpoch),
                               SafeInt(faSesPkpkRpm),
                               SafeInt(faSesPkpkAmpsA, 10),
                               SafeInt(faSesPkpkTempF, 10),
                               SafeInt((double)faSesPkpkEpoch),
                               rollCsv(ROLL_RPMEDGE, 10),
                               rollCsv(ROLL_AMPSDRIFT, 100),
                               rollCsv(ROLL_AMPSDRIFTEXC, 100),
                               rollCsv(ROLL_TONEPK, 100),
                               rollCsv(ROLL_LDSLEW, 10),
                               rollCsv(ROLL_CVSLOPE, 10000),
                               rollCsv(ROLL_RIPCMDEXC, 100),
                               rollCsv(ROLL_RIPALTEXC, 100),
                               rollCsv(ROLL_RIPBATTEXC, 100),
                               rollCsv(ROLL_RIPRPMSHIFT, 10),
                               (int)(g_ripAltAdmitCount & 0x7FFFFFFF),
                               (int)(g_ripBattAdmitCount & 0x7FFFFFFF),
                               SafeInt(LongestSingleTrip_Nm_AllTime, 10),
                               SafeInt(Max24hrDistance_AllTime, 10),
                               SafeInt(DeepestAnchorage_Ft_AllTime, 10),
                               SafeInt(best_upwind_vmg_alltime, 100),
                               SafeInt(longest_gale_duration_hours_alltime, 100),
                               SafeInt(getCpuFrequencyMhz()),
                               SafeInt(ch0GapLastMs),
                               SafeInt(ch0GapWorstMs),
                               SafeInt(ch2GapLastMs),
                               SafeInt(ch2GapWorstMs),
                               SafeInt(csv2BuildLastUs),
                               SafeInt(csv2BuildWorstUs),
                               SafeInt(csv2SendLastUs),
                               SafeInt(csv2SendWorstUs),
                               SafeInt(httpsUploadLastMs),
                               SafeInt(httpsUploadWorstMs),
                               SafeInt(cvTempDerateScale, 1000),
                               (int)accCur4.validSec,
                               (int)accCur4.activeSec,
                               (int)accCur4.inbandActiveSec,
                               (int)accCur4.constrainedSec,
                               (int)accCur4.excursions,
                               (int)(accCur4.recovSecSum * 10.0),
                               SafeInt((float)accCur4.overExpSum, 100),
                               SafeInt(accCur4.worstOver, 100),
                               (int)accVolt4.validSec,
                               (int)accVolt4.activeSec,
                               (int)accVolt4.inbandActiveSec,
                               (int)accVolt4.constrainedSec,
                               (int)accVolt4.excursions,
                               (int)(accVolt4.recovSecSum * 10.0),
                               SafeInt((float)accVolt4.overExpSum, 100),
                               SafeInt(accVolt4.worstOver, 1000),
                               (int)accThermBindingSec,
                               (int)accThermInbandSec,
                               (int)accThermSessions,
                               SafeInt(accThermWorstOverF, 100),
                               (int)imuInstallCode(),
                               SafeInt(g_cvKdCount),
                               LittleFsFreeKb,
                               (lastCloudUploadOkMs == 0) ? -1 : (int)((millis() - lastCloudUploadOkMs) / 1000UL),
                               (int)getCurrentTimestamp(),
                               (int)cfgPushPendingCount,
                               (int)cfgPushAppliedCount,
                               SafeInt(ch1_worst_fieldon),
                               SafeInt(ch0GapFieldOnWorstMs),
                               SafeInt(ch2GapFieldOnWorstMs),
                               (int)loopBlameIdx[0], (int)loopBlameUs[0],
                               (int)loopBlameIdx[1], (int)loopBlameUs[1],
                               (int)loopBlameIdx[2], (int)loopBlameUs[2],
                               SafeInt(ft_n2kTx.worstWindow),
                               SafeInt(ft_n2kTx.worstSession),
                               SafeInt(n2kTxCount),
                               SafeInt(n2kTxDropCount),
                               n2kSrcAddrLive,
                               isnan(n2kRxBattV) ? ROLL_EMPTY : (int)lroundf(n2kRxBattV * 100.0f),      // NAN -> sentinel: SafeInt's -1 collides with real small negative currents
                               isnan(n2kRxBattA) ? ROLL_EMPTY : (int)lroundf(n2kRxBattA * 100.0f),
                               isnan(n2kRxBattTempF) ? ROLL_EMPTY : (int)lroundf(n2kRxBattTempF * 10.0f),
                               n2kRxSoc,
                               n2kRxSoh,
                               (int)dvccState,
                               isnan(dvccRxCvl) ? ROLL_EMPTY : (int)lroundf(dvccRxCvl * 100.0f),  // NAN -> sentinel, same convention as n2kRx
                               isnan(dvccRxCcl) ? ROLL_EMPTY : (int)lroundf(dvccRxCcl * 10.0f),
                               (int)dvccRxSrcAddr,
                               (int)dvccUntrustReason,
                               SafeInt(ft_dvcc.worstWindow),
                               SafeInt(ft_dvcc.worstSession),
                               SafeInt(ft_n2kParse.worstWindow),
                               SafeInt(ft_n2kParse.worstSession),
                               (int)lroundf(g_huntKdScale * 100.0f),
                               (int)n183SentenceCount,
                               (int)n183ChecksumErrCount,
                               SafeInt(ft_ReadNMEA0183.worstWindow),
                               SafeInt(ft_ReadNMEA0183.worstSession),
                               SafeInt(ccDutyCeiling(), 100),
                               (int)dvccAuthMfg,
                               (int)dvccAuthProd,
                               SafeInt(g_ovTierLowCount),   // CSV2_ovTierLowCount
                               SafeInt(g_ovTierMidCount),   // CSV2_ovTierMidCount
                               // demo mode: today's row of the made-up ledger (9_demo.ino), the real ledger is frozen
                               sledTele(demoSeal ? 3.60f : (sledLive.dayIdx ? sledLive.predHarvKwh : NAN)),   // CSV2_sledPredHarvToday
                               sledTele(demoSeal ? VictronYieldToday_kWh : sledLiveActHarvKwh()),             // CSV2_sledActHarvToday
                               sledTele(demoSeal ? 2.85f : (sledLive.dayIdx ? sledLive.predConsKwh : NAN)),   // CSV2_sledPredConsToday
                               sledTele(demoSeal ? 1.18f : sledLiveActConsKwh()),                             // CSV2_sledActConsToday
                               SafeInt(demoSeal ? 3.56f : sledNeedKwh, 100),                                  // CSV2_sledNeedKwh
                               SafeInt(demoSeal ? 1 : sledNeedSource),                                        // CSV2_sledNeedSource
                               SafeInt(demoSeal ? 45 : sledCompleteDays()),                                   // CSV2_sledDaysValid
                               SafeInt(demoSeal ? demoMinutesToday() : (sledLive.dayIdx ? sledLive.coverageMin : 0)),   // CSV2_sledCoverageMin
                               SafeInt(ft_solarLedger.worstWindow),                      // CSV2_ft_solarLedger_win
                               SafeInt(ft_solarLedger.worstSession),                     // CSV2_ft_solarLedger_ses
                               isfinite(BatteryTempProbeF) ? (int)lroundf(BatteryTempProbeF * 10.0f) : ROLL_EMPTY,  // CSV2_BatteryTempProbeF
                               isfinite(ExtraTempF) ? (int)lroundf(ExtraTempF * 10.0f) : ROLL_EMPTY,                // CSV2_ExtraTempF
                               isfinite(battTempActiveF) ? (int)lroundf(battTempActiveF * 10.0f) : ROLL_EMPTY,      // CSV2_battTempActiveF
                               (int)battTempActiveSrc,                                   // CSV2_battTempActiveSrc
                               (int)owProbeCount,                                        // CSV2_owProbeCount
                               (int)owUnassignedCount(),                                 // CSV2_owUnassignedCount
                               isfinite(VictronBattTempF) ? (int)lroundf(VictronBattTempF * 10.0f) : ROLL_EMPTY,    // CSV2_VictronBattTempF
                               isfinite(rvcRxBattTempF) ? (int)lroundf(rvcRxBattTempF * 10.0f) : ROLL_EMPTY,        // CSV2_rvcRxBattTempF
                               (int)cvTempDerateInert,                                   // CSV2_cvTempDerateInert
                               wmIgnCsv(wmIgn_battTempF.lo, 1),                // CSV2_wmIgn_battTempF_lo
                               wmIgnCsv(wmIgn_battTempF.hi, 1),                // CSV2_wmIgn_battTempF_hi
                               wmIgnCsv(wmIgn_extraTempF.lo, 1),               // CSV2_wmIgn_extraTempF_lo
                               wmIgnCsv(wmIgn_extraTempF.hi, 1),               // CSV2_wmIgn_extraTempF_hi
                               SafeInt(g_fieldDutyFloor, 100),                           // CSV2_fieldDutyFloor
                               SafeInt(inaDieTempF, 10),                                 // CSV2_inaDieTempF
                               SafeInt(inaDieTempMaxF, 10),                              // CSV2_inaDieTempMaxF
                               SafeInt(g_voltStaleCount),                                // CSV2_voltStaleCount
                               SafeInt(g_volt2StaleCount),                               // CSV2_volt2StaleCount
                               SafeInt(g_battTempLostCount),                             // CSV2_battTempLostCount
                               (int)g_sensorFaultMask,                                   // CSV2_sensorFaultMask
                               (int)g_battTempState,                                     // CSV2_battTempState
                               (int)g_limpTier,                                          // CSV2_limpTier
                               (int)g_sensorSteadyMask,                                  // CSV2_sensorSteadyMask
                               (int)g_reqFaultCount,                                     // CSV2_reqFaultCount
                               (int)g_battCurState,                                      // CSV2_battCurState
                               SafeInt(g_fieldCurDutyCeil, 100),                         // CSV2_fieldCurDutyCeil
                               SafeInt(g_fieldHeatDutyCeil, 100),                        // CSV2_fieldHeatDutyCeil
                               (int)g_fieldCeilBind,                                     // CSV2_fieldCeilBind
                               SafeInt(g_fieldRcOhm, 100),                               // CSV2_fieldRcOhm
                               (unsigned long)g_fieldHeatLimS,                           // CSV2_fieldHeatLimS
                               (unsigned long)g_fieldCurLimS,                            // CSV2_fieldCurLimS
                               (unsigned long)g_fieldHeatLimSesS,                        // CSV2_fieldHeatLimSesS
                               (unsigned long)g_fieldCurLimSesS,                         // CSV2_fieldCurLimSesS
                               SafeInt(g_dieTempLostCount),                              // CSV2_dieTempLostCount
                               (int)n2kBusVerdictCode,                                   // CSV2_n2kBusVerdict
                               (int)tNMEA2000_esp32::BusStats.tec,                       // CSV2_n2kTec
                               (int)tNMEA2000_esp32::BusStats.rec,                       // CSV2_n2kRec
                               (int)((tNMEA2000_esp32::BusStats.rxFrames - n2kRxFramesBase) & 0x7FFFFFFF),  // CSV2_n2kRxFrames
                               (int)(tNMEA2000_esp32::BusStats.busOffCount & 0x7FFFFFFF),                   // CSV2_n2kBusOffCount
                               (int)(n2kRxLostRun & 0x7FFFFFFF),                         // CSV2_n2kRxLost
                               (int)n2kNodesHeard,                                       // CSV2_n2kNodesHeard
                               SafeInt(LargestPSRAMBlock),                               // CSV2_LargestPSRAMBlock
                               SafeInt(dvccRxCanCvl, 100),                               // CSV2_dvccRxCanCvl
                               SafeInt(dvccRxCanCcl, 10),                                // CSV2_dvccRxCanCcl
                               SafeInt(dvccRxWifiCvl, 100),                              // CSV2_dvccRxWifiCvl
                               SafeInt(dvccRxWifiCcl, 10),                               // CSV2_dvccRxWifiCcl
                               SafeInt(dvccRxCanShare, 10),                              // CSV2_dvccRxCanShare
                               (int)dvccNoLimitStop,                                     // CSV2_dvccNoLimitStop
                               (unsigned)g_sessionId,   // CSV2_sessionId
                               (unsigned)millis());     // CSV2_sendMs — build time; the send happens one pass later
    csv2BuildLastUs = micros() - _csv2b0;   // CSV2 build (snprintf) cost
    if (csv2BuildLastUs > csv2BuildWorstUs) csv2BuildWorstUs = csv2BuildLastUs;
    gxEndWrite(2, payload2, payload2Len, PAYLOAD2_SIZE);
    // Clear the anti-windup latch now that this CSV2 frame has captured it (set in tempPID_tick on each CV-bleed event)
    thermalAntiWindupLatch = false;
    if (payload2Len < 0 || payload2Len >= (int)PAYLOAD2_SIZE) {
      Serial.printf("payload2 truncated or format error: %d\n", payload2Len);
      csv2Phase = 0;
      return;
    }
    csv2Phase = 1;          // built OK — send it on the next pass
    csv2BuiltMs = now;
    sentSomething = true;   // claim this pass (the heavy build ran)
    } else {
      // PASS 2 — send the payload built last pass (cheap ~1.3 ms; kept off the build pass).
      uint32_t _csv2s0 = micros();   // CSV2 send-cost timer (events.send → AsyncTCP)
      csv2Kick = false;              // before the send: a dashboard connecting after this line gets the next frame
      sseSend(payload2, "CSVData2");
      csv2SendLastUs = micros() - _csv2s0;
      if (csv2SendLastUs > csv2SendWorstUs) csv2SendWorstUs = csv2SendLastUs;
      gxOffer(2, now, gxLive, !phoneLive);
      lastpayload2send = now;
      lastEventSourceSend = now;
      csv2Phase = 0;
      sentSomething = true;
    }
  }

  // PRIORITY 5: CSVData3 — sent immediately when settingsDirty (event-driven), or every 60s fallback
  if (!sentSomething
      && (phoneLive ? (settingsDirty || now - lastpayload3send >= 60000) : (gxLive && (settingsDirty || gxDue(3, now, lastpayload3send, gxLive))))
      && !sseBacklogged()) {
    static char *payload3 = nullptr;
    // Overflow is caught below, but the catch returns early from SendWifiData, stopping CSV3 for good and
    // skipping TS when CSV3 is due; csv3LastLen / csv3PeakLen on /debug measure the margin.
    if (!payload3) {
      payload3 = (char *)ps_malloc(PAYLOAD3_SIZE);  // allocated to PSRAM
      if (!payload3) {
        Serial.println("FATAL: payload3 ps_malloc failed");
        return;
      }
    }
    /// ALL THIS SAFEINT STUFF MAY BE UNNECESSAREY BAD ADVICE, COULD HAVE JUST SENT ROUNDED FLOATS FOR 1 Byte (or bit?) xtra
    //WifiSendTime was 834uS before increasing csv3 payload size from 1100 to 1400     No change after.  Again, this separation into groups and worry about wifi packet size seems like AI nonsense.

    // Cleared BEFORE the build: a /get on core 0 landing mid-build re-arms it for the next pass
    // instead of being wiped after a frame that already read the old value.
    const bool csv3Dirty = settingsDirty;
    settingsDirty = false;
    gxBeginWrite(3);
    int     payload3Len = snprintf(payload3, PAYLOAD3_SIZE,
                               "%d" CSV3_LIST(CSV3_FMT_X) "\n",
                               CSV3_FIELD_COUNT CSV3_LIST(CSV3_ARG_X));
    gxEndWrite(3, payload3, payload3Len, PAYLOAD3_SIZE);
    if (payload3Len < 0 || payload3Len >= PAYLOAD3_SIZE) {
      Serial.printf("payload3 truncated or format error: %d\n", payload3Len);
      return;
    }
    csv3LastLen = (uint16_t)payload3Len;
    if (csv3LastLen > csv3PeakLen) csv3PeakLen = csv3LastLen;

    sseSend(payload3, "CSVData3", true);
    gxOffer(3, now, gxLive, !phoneLive || csv3Dirty);   // a settings change reaches the GX at once
    lastpayload3send = now;
    lastEventSourceSend = now;
    sentSomething = true;
  }

  // PRIORITY 6: TimestampData (staleness data - every 3 seconds)
  if (!sentSomething && (phoneLive ? (tsKick || now - lastTimestampSend >= 3000) : gxDue(4, now, lastTimestampSend, gxLive)) && !sseBacklogged()) {

    static char *timestampPayload = nullptr;
    static const size_t TIMESTAMP_PAYLOAD_SIZE = 512;  // every field is an age and the weather one can be 10 digits; an overflow kills TS until reboot
    if (!timestampPayload) {
      timestampPayload = (char *)ps_malloc(TIMESTAMP_PAYLOAD_SIZE);  // allocated to PSRAM
      if (!timestampPayload) {
        Serial.println("FATAL: timestampPayload ps_malloc failed");
        return;
      }
    }
    // An age past 0xFFFF0000 is a wrap, not 49.7 days: TempTask and httpsTask on core 0 can stamp after `now` was sampled.
    auto tsAge = [now](unsigned long ts) -> unsigned long {
      if (ts == 0) return 999999;
      const unsigned long age = now - ts;
      return (age > 0xFFFF0000UL) ? 0 : age;
    };
    gxBeginWrite(4);
    int timestampPayloadLen = snprintf(timestampPayload, TIMESTAMP_PAYLOAD_SIZE,
                                       "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,"
                                       "%lu,%lu,%lu,%lu,"  // +4 battery/extra temperature: BATT probe, EXTRA probe, VE.Direct T, RV-C T
                                       "%lu,"      // +1 INA228 die temperature
                                       "%lu,%lu,"  // +2 DVCC Victron carriers: VE.Can, WiFi
                                       "%lu,%lu",  // +2: sessionId, sendMs
                                       (unsigned long)TS_FIELD_COUNT,
                                       tsAge(dataTimestamps[IDX_HEADING_NMEA]),
                                       tsAge(dataTimestamps[IDX_LATITUDE_NMEA]),
                                       tsAge(dataTimestamps[IDX_LONGITUDE_NMEA]),
                                       tsAge(dataTimestamps[IDX_SATELLITE_COUNT]),
                                       tsAge(dataTimestamps[IDX_VICTRON_VOLTAGE]),
                                       tsAge(dataTimestamps[IDX_VICTRON_CURRENT]),
                                       tsAge(dataTimestamps[IDX_ALTERNATOR_TEMP]),
                                       tsAge(dataTimestamps[IDX_THERMISTOR_TEMP]),
                                       tsAge(dataTimestamps[IDX_RPM]),
                                       tsAge(dataTimestamps[IDX_MEASURED_AMPS]),
                                       tsAge(dataTimestamps[IDX_BATTERY_V]),
                                       tsAge(dataTimestamps[IDX_IBV]),
                                       tsAge(dataTimestamps[IDX_BCUR]),
                                       tsAge(dataTimestamps[IDX_CHANNEL3V]),
                                       tsAge(dataTimestamps[IDX_DUTY_CYCLE]),
                                       tsAge(dataTimestamps[IDX_FIELD_VOLTS]),
                                       tsAge(dataTimestamps[IDX_FIELD_AMPS]),
                                       tsAge(dataTimestamps[IDX_COG_NMEA]),
                                       tsAge(dataTimestamps[IDX_SOG_NMEA]),
                                       tsAge(dataTimestamps[IDX_APPARENT_WIND_SPEED]),
                                       tsAge(dataTimestamps[IDX_APPARENT_WIND_ANGLE]),
                                       tsAge(dataTimestamps[IDX_TRUE_WIND_SPEED]),
                                       tsAge(dataTimestamps[IDX_TRUE_WIND_ANGLE]),
                                       tsAge(dataTimestamps[IDX_LEEWAY]),
                                       tsAge(dataTimestamps[IDX_VMG]),
                                       tsAge(dataTimestamps[IDX_BARO_PRESSURE]),
                                       tsAge(dataTimestamps[IDX_AMBIENT_TEMP]),
                                       tsAge(dataTimestamps[IDX_IMU]),
                                       tsAge(dataTimestamps[IDX_VICTRON_SOLAR]),
                                       tsAge(dataTimestamps[IDX_STW_NMEA]),
                                       tsAge(dataTimestamps[IDX_N2K_BATT]),
                                       tsAge(dataTimestamps[IDX_N2K_SOC]),
                                       tsAge(dataTimestamps[IDX_DVCC]),
                                       // Own "never" sentinel: 999999 ms is only 17 min, inside the 6 h forecast
                                       // refresh, so the shared sensor sentinel would read as a real age.
                                       (weatherLastUpdate == 0) ? 4294967295UL : tsAge(weatherLastUpdate),
                                       tsAge(dataTimestamps[IDX_N183]),
                                       tsAge(dataTimestamps[IDX_BATT_TEMP_PROBE]),
                                       tsAge(dataTimestamps[IDX_EXTRA_TEMP]),
                                       tsAge(dataTimestamps[IDX_VE_BATT_TEMP]),
                                       tsAge(dataTimestamps[IDX_RVC_BATT_TEMP]),
                                       tsAge(dataTimestamps[IDX_INA_DIE_TEMP]),
                                       tsAge(dataTimestamps[IDX_DVCC_VECAN]),
                                       tsAge(dataTimestamps[IDX_DVCC_WIFI]),
                                       (unsigned long)g_sessionId,   // TS_sessionId
                                       (unsigned long)now            // TS_sendMs (`now` is this dispatch tick's millis())
    );
    gxEndWrite(4, timestampPayload, timestampPayloadLen, TIMESTAMP_PAYLOAD_SIZE);
    if (timestampPayloadLen < 0 || timestampPayloadLen >= TIMESTAMP_PAYLOAD_SIZE) {
      Serial.printf("timestampPayload truncated or format error: %d\n", timestampPayloadLen);
      return;
    }

    tsKick = false;   // before the send, as csv2Kick
    sseSend(timestampPayload, "TimestampData");
    gxOffer(4, now, gxLive, !phoneLive);
    lastTimestampSend = now;
    lastEventSourceSend = now;
    sentSomething = true;
  }

  // PRIORITY 7: GxDevices, the GX's device readings (POST /gxDevices), JSON: {"v":1,"ageMs":ms since the post or null
  // (none since boot or the last demo switch),"everyS":the interval that post was told,"d":[{"k","n","p","a","age","on"}]}.
  // Sent when a post lands and at once to a dashboard that connects; the page ages it from there. Dashboards only: the
  // GX has these readings already.
  static uint32_t gxDevSentSeq = 0;
  if (!sentSomething && phoneLive && (gxDevKick || __atomic_load_n(&gxDevSeq, __ATOMIC_ACQUIRE) != gxDevSentSeq)
      && !sseBacklogged()) {
    static char *gxDevCopy = nullptr, *gxDevOut = nullptr;
    if (!gxDevCopy) gxDevCopy = (char *)ps_malloc(GXDEV_TEXT_SIZE);
    if (!gxDevOut) gxDevOut = (char *)ps_malloc(GXDEV_TEXT_SIZE + 96);
    const uint32_t s1 = __atomic_load_n(&gxDevSeq, __ATOMIC_ACQUIRE);
    if (gxDevCopy && gxDevOut && !(s1 & 1u)) {
      const char *src = __atomic_load_n(&gxDevText, __ATOMIC_RELAXED);
      const uint16_t len = __atomic_load_n(&gxDevLen, __ATOMIC_RELAXED);
      const uint32_t at = __atomic_load_n(&gxDevAtMs, __ATOMIC_RELAXED);
      const uint8_t every = __atomic_load_n(&gxDevEveryS, __ATOMIC_RELAXED);
      const bool have = src && len > 1 && len < GXDEV_TEXT_SIZE && at && s1 > gxDevClearSeq;
      if (have) memcpy(gxDevCopy, src, len);
      __atomic_thread_fence(__ATOMIC_ACQUIRE);
      if (__atomic_load_n(&gxDevSeq, __ATOMIC_RELAXED) == s1) {   // else the handler wrote meanwhile: next pass
        gxDevCopy[have ? len : 0] = '\0';
        char age[16];
        // a post stamped on Core 0 after this pass sampled `now` is 0 ms old, not 49 days
        if (have) snprintf(age, sizeof(age), "%lu", (unsigned long)((int32_t)(now - at) > 0 ? now - at : 0));
        else snprintf(age, sizeof(age), "null");
        const int n = snprintf(gxDevOut, GXDEV_TEXT_SIZE + 96, "{\"v\":1,\"ageMs\":%s,\"everyS\":%u,\"d\":%s}", age,
                               (unsigned)every, have ? gxDevCopy : "[]");
        gxDevKick = false;
        gxDevSentSeq = s1;
        if (n > 0 && n < GXDEV_TEXT_SIZE + 96) sseSend(gxDevOut, "GxDevices");
        lastEventSourceSend = now;
        sentSomething = true;
      }
    }
  }
}
// CLOUD FEATURES STUFF BELOW
// AUTH TOKEN MANAGEMENT
String loadAuthToken() {
  nvs_handle_t handle;
  esp_err_t err = nvs_open("cloud", NVS_READONLY, &handle);

  authToken = "";

  if (err == ESP_OK) {
    size_t len = 0;
    err = nvs_get_str(handle, "authToken", NULL, &len);

    if (err == ESP_OK && len > 0) {
      char *buf = (char *)malloc(len);
      if (!buf) {
        nvs_close(handle);
        return authToken;
      }
      nvs_get_str(handle, "authToken", buf, &len);
      authToken = String(buf);
      free(buf);
    }

    nvs_close(handle);
  }

  isRegistered = (authToken.length() > 0);

  Serial.println("=== loadAuthToken ===");

  if (isRegistered) {
    Serial.println("Auth token loaded (registered)");
  } else {
    Serial.println("No auth token found - device not registered");
  }

  return authToken;
}
esp_err_t saveAuthToken(String token) {
  nvs_handle_t handle;
  esp_err_t err = nvs_open("cloud", NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    Serial.printf("ERROR opening NVS for write: %d\n", err);
    return err;
  }

  err = nvs_set_str(handle, "authToken", token.c_str());
  if (err != ESP_OK) {
    Serial.printf("ERROR writing token: %d\n", err);
  } else {
    err = nvs_commit(handle);
    if (err != ESP_OK) Serial.printf("ERROR committing token: %d\n", err);
  }
  nvs_close(handle);
  // Not registered on a failed write: running registered would end, unannounced, at the next reboot.
  if (err != ESP_OK) return err;

  authToken = token;
  isRegistered = true;

  Serial.println("\n========================================");
  Serial.println("REGISTRATION SUCCESSFUL");
  Serial.println("========================================");
  Serial.printf("Auth Token: %s\n", token.c_str());
  Serial.println("========================================");
  Serial.println("Copy token above and add to Vercel URL:");
  Serial.printf("?token=%s\n", token.c_str());
  Serial.println("========================================\n");
  return ESP_OK;
}
// Random per-board registration nonce, saved in NVS before it is ever sent and reused on every
// attempt. The cloud stores its hash with the device row, so a retry after a lost registration reply
// gets the existing token back instead of "already registered". Empty if NVS can't hold it.
String regNonce() {
  char buf[33] = { 0 };
  nvs_handle_t h;
  if (nvs_open("cloud", NVS_READWRITE, &h) != ESP_OK) return String();
  size_t len = sizeof(buf);
  if (nvs_get_str(h, "regNonce", buf, &len) != ESP_OK || strlen(buf) != 32) {
    for (int i = 0; i < 4; i++) snprintf(buf + i * 8, 9, "%08lx", (unsigned long)esp_random());
    esp_err_t e = nvs_set_str(h, "regNonce", buf);
    if (e == ESP_OK) e = nvs_commit(h);
    if (e != ESP_OK) { nvs_close(h); return String(); }
  }
  nvs_close(h);
  return String(buf);
}

void clearAuthToken() {
  nvs_handle_t handle;
  esp_err_t err = nvs_open("cloud", NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    Serial.printf("ERROR opening NVS for clear: %d\n", err);
    return;
  }
  nvs_erase_key(handle, "authToken");
  nvs_commit(handle);
  nvs_close(handle);
  authToken = "";
  isRegistered = false;
  Serial.println("Auth token cleared");
}


// One nvs handle + ONE commit for the whole record — 19 settingWrite() calls would be 19
// separate flash commits. compare-first mirrors settingWrite so an unchanged field costs nothing.
static void vesselNvsSet(nvs_handle_t h, const char *key, const char *val) {
  char cur[128];
  size_t len = sizeof(cur);
  if (nvs_get_str(h, key, cur, &len) == ESP_OK && strcmp(cur, val) == 0) return;
  nvs_set_str(h, key, val);
}

// One-time fold of the pre-NVS /vessel_info.json into NVS; the file is deleted after.
// Remove once no device can still be running a build that wrote that file.
void migrateVesselInfoFile() {
  if (settingExists(NK_vesselSaved)) return;
  if (!littleFSMounted && !ensureLittleFS()) return;
  if (!LittleFS.exists("/vessel_info.json")) return;
  File file = LittleFS.open("/vessel_info.json", "r");
  if (!file) return;
  DynamicJsonDocument doc(4096);
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  if (error) {
    Serial.printf("Vessel info migration: JSON parse failed: %s\n", error.c_str());
    return;
  }
  nvs_handle_t h;
  if (nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
  vesselNvsSet(h, NK_boatLenFt,      String((float)(doc["boat_length_ft"] | 0.0f), 2).c_str());
  vesselNvsSet(h, NK_boatDispLbs,    String((float)(doc["boat_displacement_lbs"] | 0.0f), 0).c_str());
  vesselNvsSet(h, NK_boatType,       (const char *)(doc["boat_type"] | "monohull"));
  vesselNvsSet(h, NK_boatMakeModel,  (const char *)(doc["boat_make_model"] | ""));
  vesselNvsSet(h, NK_boatYear,       String((int)(doc["boat_year"] | 2025)).c_str());
  vesselNvsSet(h, NK_homePort,       (const char *)(doc["home_port"] | ""));
  vesselNvsSet(h, NK_engineMake,     (const char *)(doc["engine_make"] | ""));
  vesselNvsSet(h, NK_engineHp,       String((int)(doc["engine_hp"] | 0)).c_str());
  vesselNvsSet(h, NK_batteryType,    (const char *)(doc["battery_type"] | "lifepo4"));
  vesselNvsSet(h, NK_battMakeModel,  (const char *)(doc["battery_make_model"] | ""));
  vesselNvsSet(h, NK_altBrandModel,  (const char *)(doc["alternator_brand_model"] | ""));
  vesselNvsSet(h, NK_imuMountOrient, String((int)(doc["imu_mount_orientation"] | 0)).c_str());
  vesselNvsSet(h, NK_imuDistBowFt,   String((float)(doc["imu_dist_bow_ft"] | 0.0f), 2).c_str());
  vesselNvsSet(h, NK_imuDistClFt,    String((float)(doc["imu_dist_cl_ft"] | 0.0f), 2).c_str());
  vesselNvsSet(h, NK_imuHtWlFt,      String((float)(doc["imu_height_wl_ft"] | 0.0f), 2).c_str());
  // The three shared keys are normally already in NVS and authoritative there; only seed them
  // if this device predates them, never let the file's stale mirror overwrite a live value.
  if (!settingExists(NK_BatteryVoltage))      vesselNvsSet(h, NK_BatteryVoltage,      String((int)(doc["battery_voltage"] | 12)).c_str());
  if (!settingExists(NK_BatteryCapacity_Ah))  vesselNvsSet(h, NK_BatteryCapacity_Ah,  String((int)(doc["battery_capacity_ah"] | 300)).c_str());
  if (!settingExists(NK_SolarWatts))          vesselNvsSet(h, NK_SolarWatts,          String((int)(doc["solar_watts"] | 0)).c_str());
  vesselNvsSet(h, NK_vesselSaved, "1");
  // One commit for the whole record: either every key lands or none does, so a power cut
  // here leaves the file intact and the migration simply re-runs next boot.
  esp_err_t e = nvs_commit(h);
  nvs_close(h);
  if (e != ESP_OK) {
    Serial.printf("Vessel info migration: NVS commit failed: %d\n", (int)e);
    return;
  }
  bool held = (fsMutex && xSemaphoreTake(fsMutex, pdMS_TO_TICKS(5000)) == pdTRUE);
  LittleFS.remove("/vessel_info.json");   // unconditional: a lingering file would never be retried
  if (held) xSemaphoreGive(fsMutex);
  Serial.println("Vessel info migrated from LittleFS to NVS");
}

// Called from Core 1 main loop via pendingSaveVesselInfo flag — applyChemistryOcvPreset() and
// seedSocFromVoltage() below are not safe to run on Core 0 alongside SSE delivery.
void saveVesselInfoToNvs() {
  nvs_handle_t h;
  if (nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
    Serial.println("saveVesselInfoToNvs: nvs_open failed");
    return;
  }
  vesselNvsSet(h, NK_boatLenFt,          String(BOAT_LENGTH_FT, 2).c_str());
  vesselNvsSet(h, NK_boatDispLbs,        String(BOAT_DISPLACEMENT_LBS, 0).c_str());
  vesselNvsSet(h, NK_boatType,           BOAT_TYPE);
  vesselNvsSet(h, NK_boatMakeModel,      BOAT_MAKE_MODEL);
  vesselNvsSet(h, NK_boatYear,           String((int)BOAT_YEAR).c_str());
  vesselNvsSet(h, NK_homePort,           HOME_PORT);
  vesselNvsSet(h, NK_engineMake,         ENGINE_MAKE);
  vesselNvsSet(h, NK_engineHp,           String((int)ENGINE_HP).c_str());
  vesselNvsSet(h, NK_BatteryVoltage,     String((int)SYSTEM_VOLTAGE_CLASS).c_str());
  vesselNvsSet(h, NK_BatteryCapacity_Ah, String(BatteryCapacity_Ah).c_str());
  vesselNvsSet(h, NK_batteryType,        BATTERY_TYPE);
  vesselNvsSet(h, NK_battMakeModel,      BATTERY_MAKE_MODEL);
  vesselNvsSet(h, NK_altBrandModel,      ALTERNATOR_BRAND_MODEL);
  vesselNvsSet(h, NK_regName,            REGULATOR_NAME);
  vesselNvsSet(h, NK_SolarWatts,         String(SolarWatts).c_str());
  vesselNvsSet(h, NK_imuMountOrient,     String((int)imuMountOrientation).c_str());
  vesselNvsSet(h, NK_imuDistBowFt,       String(IMU_DIST_BOW_FT, 2).c_str());
  vesselNvsSet(h, NK_imuDistClFt,        String(IMU_DIST_CL_FT, 2).c_str());
  vesselNvsSet(h, NK_imuHtWlFt,          String(IMU_HEIGHT_WL_FT, 2).c_str());
  vesselNvsSet(h, NK_vesselSaved, "1");
  esp_err_t e = nvs_commit(h);
  nvs_close(h);
  if (e != ESP_OK) {
    Serial.printf("saveVesselInfoToNvs: commit failed: %d\n", (int)e);
    return;
  }

  vesselInfoSaved = true;
  mdnsRefreshName();   // a rename has to reach the network now, not at the next boot
  applyChemistryOcvPreset();  // chemistry-match the rested-voltage curve before the seed reads it
  seedSocFromVoltage();  // factory-fresh path: seed was deferred until real chemistry/capacity existed
  // Refresh the cloud's user_profiles vessel projection on Save, not at the next boot/24 h
  // backstop — the snapshot's settings jsonb is the only carrier of the vessel record.
  configSnapshotRequested = true;
  configSnapshotRetryMs = 0;
}

