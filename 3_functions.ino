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
  CSV1_perfCountersResetElapsedS,  // seconds since "Reset Peak Values" press (or boot if never pressed); UI formats as "last X min" / "last X.X hr"
  CSV1_shutdownPhase,
  CSV1_BatteryV_raw,
  CSV1_pidAltPV,  // CC current-PID process variable — the OutputPIDSigSrc-selected signal (EMA/MA/raw)
  CSV1_voltageTarget,
  CSV1_Icv,
  CSV1_WaterDepth_ft,  // NMEA2k depth ×3.28084 (meters → feet), scaled ×10 (0.1 ft); 0 if stale
  CSV1_Ignition,       // effective ignition state (real GPIO1 wire OR force-on override) — on CSV1 so the banner tracks it at ~10 Hz, not the 5 s CSV2 cadence
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
  CSV1_huntFreqHz,       // hunt-governor last confirmed wobble frequency (Hz ×100; 0 = none seen this session). Rides CSV1 rather than CSV4 so the Diag live row can never show a fresh derate beside a stale frequency
  CSV1_huntState,        // hunt-governor state: 0 watching (gain follows the pocket map), 1 testing a current-loop gain (Ki) cut, 3 cooldown after a failed test, 4 testing with the voltage damper (D-term) paused (2 unused since v2)
  CSV1_rpmCeilingAmps,   // RPM-table current ceiling this tick (A ×100) — the header Limit panel's "current limit at this RPM"

  CSV1_sessionId,        // boot identity — same value in every channel this boot; proves a cached block is from this run
  CSV1_sendMs,           // millis() when this payload was built; a consumer ages every other channel against this one

  CSV1_FIELD_COUNT  // = 52
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
  CSV2_DynamicShuntGainFactor,
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
  CSV2_LargestInternalBlock,
  CSV2_FreePSRAM,
  CSV2_TotalPSRAM,
  CSV2_Heapfrag,
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

  // IMU zero/level calibration echo (Phase 2 IMU zero button)

  // Victron VE.Direct solar/MPPT live block (7 fields — PPV/VPV/derived-current moved to CSV4/NavStream)
  CSV2_VictronChargeState,       // CS code (×1)
  CSV2_VictronMPPTMode,          // MPPT tracker code (×1)
  CSV2_VictronError,             // ERR code (×1)
  CSV2_VictronYieldToday,        // H20 yield today (kWh ×100)
  CSV2_VictronMaxPowerToday,     // H21 max power today (W ×1)
  CSV2_VictronYieldYesterday,    // H22 yield yesterday (kWh ×100)
  CSV2_VictronMaxPowerYesterday, // H23 max power yesterday (W ×1)

  // (live engine fuel flow + economy — currentFuelGPH/currentNMPG — moved to CSV4/NavStream)

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

  // Lifetime nav/sailing records (so the Lifetime Statistics panel can show + individually reset
  // them). Persisted in NVS + uploaded to the leaderboards.
  CSV2_LongestTripAT,       // longest single trip, nm ×10
  CSV2_Max24hrDistAT,       // max 24-hour distance, nm ×10
  CSV2_DeepestAnchorAT,     // deepest anchorage, ft ×10
  CSV2_BestUpwindVmgAT,     // best upwind VMG, kts ×100
  CSV2_LongestGaleAT,       // longest gale duration, hours ×100

  CSV2_cpuFreqMhz,   // live CPU clock (80 or 240 MHz) — 80 = engine-off low-power throttle, 240 = full speed

  // ADS slow-channel inter-sample gap meters (ms). last = most recent gap between valid
  // readings, worst = max since Reset Peak Values. Validate the continuous-mode excursion
  // scheme keeps CH0/CH2 under their <30ms targets. Dashboard: Live Data → ESP32.
  CSV2_ch0GapLast,   // CH0 battery-voltage read gap, most recent (ms)
  CSV2_ch0GapWorst,  // CH0 battery-voltage read gap, worst since reset (ms)
  CSV2_ch2GapLast,   // CH2 RPM read gap, most recent (ms)
  CSV2_ch2GapWorst,  // CH2 RPM read gap, worst since reset (ms)

  // CSV2 send-cost breakdown (µs): build (snprintf) vs send (events.send). For deciding
  // whether to chunk the build or the send to shrink the WiFi-send spike.
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
  CSV2_dvccRxSrcAddr,     // authority bus address (255 = none yet)
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
  CSV2_fieldDutyCeil,     // enforced field-duty ceiling ×100 — ccDutyCeiling(), the lower of Max Field % and the Max Field Volts term. Neither setting alone tells a log which cap was binding
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
  CSV2_battTempActiveSrc,   // 0 none, 1 probe, 2 NMEA 2000, 3 VE.Direct, 4 RV-C, 5 board temperature
  CSV2_owProbeCount,        // DS18B20s in the 1-Wire registry
  CSV2_owUnassignedCount,   // present probes with no role
  CSV2_VictronBattTempF,    // VE.Direct "T" battery temperature (°F x10; ROLL_EMPTY = none)
  CSV2_rvcRxBattTempF,      // RV-C DC_SOURCE_STATUS_2 battery temperature (°F x10; ROLL_EMPTY = none)
  CSV2_cvTempDerateInert,   // 1 = battery-temperature source class changed since commissioning; CV derate held at 1.0
  CSV2_wmIgn_battTempF_lo,  CSV2_wmIgn_battTempF_hi,   // BatteryTempProbeF (°F, int)
  CSV2_wmIgn_extraTempF_lo, CSV2_wmIgn_extraTempF_hi,  // ExtraTempF (°F, int)

  CSV2_fieldDutyFloor,      // enforced field-duty FLOOR x100 — governor_apply's resolved max(Min Field %, this speed's Keep-Alive %), 0 when the floor is bypassed (shutdown, sysID, protection clamp, MANUAL). Twin of CSV2_fieldDutyCeil

  CSV2_sessionId,    // boot identity — matches CSV1_sessionId while this cached block is from the live run
  CSV2_sendMs,            // millis() when this payload was BUILT (CSV2 builds one pass and sends the next)

  CSV2_FIELD_COUNT // enum position is authoritative — never hand-count; CSV payload specifier count must equal this +1
};

enum Csv4Index {
  // NavStream: live nav / wind / solar / fuel readouts — sent every 500 ms (2 Hz).
  // Sits between CSV1 (10 Hz control-loop) and CSV2 (5 s status); the dial/compass/speed/solar/fuel gauges need ~2 Hz.
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
  CSV4_ctrlLimiter,             // banner limiter code: 0 none, 1 alt current cap, 2 thermal derate, 3 CV voltage loop, 4 battery current limit, 5 field at max duty, 6 protection (cap binding or recovery window), 7 battery above target, 8 BMS charge-current limit, 9 BMS charge-voltage limit, 10 manual field mode, 11 min field floor holding duty up, 12 zero-current command (Maintain / zero-current float), 13 warm-up ramp ceiling, 14 charge-rate Hi->Lo glide — rides NavStream so the banner tint updates at ~500ms
  CSV4_chargeStage,             // CHARGE_STAGE_* code — rides NavStream so the Plots-tab mode ribbon tracks stage changes at ~500ms (CSV2 still carries it for the thermal ring)
  CSV4_n183Heading,             // NMEA 0183 decoded heading (deg ×10; -10 = nothing decoded). Separate from CSV4_HeadingNMEA, which is the NMEA2000 source.
  CSV4_n183HdgRef,              // reference frame of the above: 0 none, 1 magnetic, 2 true
  CSV4_bmsSignalActive,         // live BMS on/off opto input (GPIO42): 1 = 5-28 V present at the wire, 0 = dead/open. Raw pin, before the Present/Absent polarity choice
  CSV4_panelHiRateInput,        // switch panel HIGH/LOW opto input (Cable 3 pin 5, GPIO39): 1 = wire energized = switch ON = asking for High. Debounced pin state, reported whether or not PhysicalPanelOverride lets it steer anything
  CSV4_panelForceFloatInput,    // switch panel FORCEFLOAT opto input (Cable 3 pin 7, GPIO40): 1 = wire energized = switch ON = asking for Maintain. Debounced pin state, reported whether or not PhysicalPanelOverride lets it steer anything
  CSV4_sessionId,               // boot identity — matches CSV1_sessionId while this cached block is from the live run
  CSV4_sendMs,                  // millis() when this payload was built
  CSV4_FIELD_COUNT  // = 26
};

enum Csv3Index {
  // SettingsStream: user-configurable settings — sent on change (settingsDirty) or every 60s fallback
  CSV3_TemperatureLimitF,
  CSV3_BulkVoltage,
  CSV3_wavePeriod,
  CSV3_FloatVoltage,
  CSV3_SwitchingFrequency,
  CSV3_yyMin,
  CSV3_retired1,  // was FieldAdjustmentInterval — dead slot, sends 0; kept so CSV3 indices never renumber
  CSV3_ManualDutyTarget,
  CSV3_PhysicalPanelOverride,
  CSV3_waveAmplitude,
  CSV3_CurrentThreshold,
  CSV3_PeukertExponent_scaled,
  CSV3_ChargeEfficiency_scaled,
  CSV3_ChargedVoltage_Scaled,
  CSV3_TailCurrent,
  CSV3_ChargedDetectionTime,
  CSV3_IgnoreTemperature,
  CSV3_bmsLogic,
  CSV3_bmsLogicLevelOff,
  CSV3_RPMScalingFactor,
  CSV3_MaximumAllowedBatteryAmps,
  CSV3_AlternatorNominalAmps,
  CSV3_LearningUpStep,
  CSV3_LearningDownStep,
  CSV3_xTime,
  CSV3_MinLearningInterval,
  CSV3_SafeOperationThreshold,
  CSV3_PidKp,
  CSV3_PidKi,
  CSV3_PidKd,
  CSV3_PidSampleDivisor,
  CSV3_MaxTableValue,
  CSV3_MaxPenaltyPercent,
  CSV3_MaxPenaltyDuration,
  CSV3_NeighborLearningFactor,
  CSV3_yyMax,
  CSV3_LearningMemoryDuration,
  CSV3_TuningMode,
  CSV3_ShuntResistanceMicroOhm,
  CSV3_InvertAltAmps,
  CSV3_InvertBattAmps,
  CSV3_MaxDuty,
  CSV3_MinDuty,
  CSV3_FieldResistance,
  CSV3_maxPoints,
  CSV3_AlternatorCOffset,
  CSV3_BatteryCOffset,
  CSV3_BatteryCapacity_Ah,
  CSV3_AmpSensorRange,
  CSV3_R_fixed,
  CSV3_Beta,
  CSV3_T0_C,
  CSV3_TempSource,
  CSV3_IgnitionOverride,
  CSV3_FLOAT_DURATION,
  CSV3_PulleyRatio,
  CSV3_BatteryCurrentSource,
  CSV3_rpmTableRPMPoints_0,
  CSV3_rpmTableRPMPoints_1,
  CSV3_rpmTableRPMPoints_2,
  CSV3_rpmTableRPMPoints_3,
  CSV3_rpmTableRPMPoints_4,
  CSV3_rpmTableRPMPoints_5,
  CSV3_rpmTableRPMPoints_6,
  CSV3_rpmTableRPMPoints_7,
  CSV3_rpmTableRPMPoints_8,
  CSV3_rpmTableRPMPoints_9,
  CSV3_LearningSettlingPeriod,
  CSV3_LearningRPMChangeThreshold,
  CSV3_LearningTempHysteresis,
  CSV3_fuelTableRPM_0,
  CSV3_fuelTableRPM_1,
  CSV3_fuelTableRPM_2,
  CSV3_fuelTableRPM_3,
  CSV3_fuelTableRPM_4,
  CSV3_fuelTableRPM_5,
  CSV3_fuelTableRPM_6,
  CSV3_fuelTableRPM_7,
  CSV3_fuelTableRPM_8,
  CSV3_fuelTableRPM_9,
  CSV3_fuelTableGPH_0,
  CSV3_fuelTableGPH_1,
  CSV3_fuelTableGPH_2,
  CSV3_fuelTableGPH_3,
  CSV3_fuelTableGPH_4,
  CSV3_fuelTableGPH_5,
  CSV3_fuelTableGPH_6,
  CSV3_fuelTableGPH_7,
  CSV3_fuelTableGPH_8,
  CSV3_fuelTableGPH_9,
  CSV3_stateRevision,
  CSV3_DutyRampRate,
  CSV3_SettleTimeBeforeCut,
  CSV3_TempWarnExcess,
  CSV3_TempCritExcess,
  CSV3_TempSustainedTimeout,
  CSV3_AlternatorHardShutdownV,
  CSV3_VoltageDisagreeThreshold,
  CSV3_VoltageDisagreeTimeout,
  CSV3_rpmMinDutyTable_0,
  CSV3_rpmMinDutyTable_1,
  CSV3_rpmMinDutyTable_2,
  CSV3_rpmMinDutyTable_3,
  CSV3_rpmMinDutyTable_4,
  CSV3_rpmMinDutyTable_5,
  CSV3_rpmMinDutyTable_6,
  CSV3_rpmMinDutyTable_7,
  CSV3_rpmMinDutyTable_8,
  CSV3_rpmMinDutyTable_9,
  CSV3_rpmCapCurrentTable_0,
  CSV3_rpmCapCurrentTable_1,
  CSV3_rpmCapCurrentTable_2,
  CSV3_rpmCapCurrentTable_3,
  CSV3_rpmCapCurrentTable_4,
  CSV3_rpmCapCurrentTable_5,
  CSV3_rpmCapCurrentTable_6,
  CSV3_rpmCapCurrentTable_7,
  CSV3_rpmCapCurrentTable_8,
  CSV3_rpmCapCurrentTable_9,
  CSV3_VoltageKp,
  CSV3_VoltageLoopInterval,
  CSV3_FIELD_COLLAPSE_DELAY,
  CSV3_SetpointRiseRate,
  CSV3_SetpointFallRate,
  CSV3_SetpointBigStepThresh,
  CSV3_SetpointBigStepRiseRate,
  CSV3_PIDTrackingGain,
  CSV3_CAPSIZE_THRESHOLD_DEG,
  CSV3_PITCHPOLE_THRESHOLD_DEG,
  CSV3_SLAM_THRESHOLD_G,
  CSV3_imuMountOrientation,
  CSV3_TailCurrent_A,
  CSV3_RebulkVoltage,
  CSV3_rebulkDebounceTime,
  CSV3_MinFloatTime,
  CSV3_SOC_BlockRebulk_percent,
  CSV3_SOC_AllowRebulk_percent,
  CSV3_DutySlowRampRate,
  CSV3_ShutdownPhase2HoldMs,
  CSV3_TempPIDKp,
  CSV3_TempPIDKi,
  CSV3_ThermalLookaheadSec,
  CSV3_TempPIDIntervalMs,
  CSV3_TempPIDFilterAlpha,
  CSV3_VoltageKi,
  CSV3_rpmCapPowerTable_0,
  CSV3_rpmCapPowerTable_1,
  CSV3_rpmCapPowerTable_2,
  CSV3_rpmCapPowerTable_3,
  CSV3_rpmCapPowerTable_4,
  CSV3_rpmCapPowerTable_5,
  CSV3_rpmCapPowerTable_6,
  CSV3_rpmCapPowerTable_7,
  CSV3_rpmCapPowerTable_8,
  CSV3_rpmCapPowerTable_9,
  CSV3_SystemIDStepAmplitude,
  CSV3_HardOCTripAmps,
  CSV3_HardOCDebounceMs,
  CSV3_IExcessFrac,    // CV threshold fraction (×1000)
  CSV3_IExcessFloorA,  // threshold floor (A ×10)
  CSV3_IExcessKBleed,
  CSV3_IgnoreRPM,
  CSV3_MinRPMForField,
  CSV3_AwBleedRate,
  CSV3_KHard,
  CSV3_ReseedFrac,
  CSV3_AwSeedProtectMs,
  CSV3_displayTempUnit,
  CSV3_WarmupRampRate,
  CSV3_OvGroup1Enable,
  CSV3_OvGroup2Enable,
  CSV3_IExcessCeilA,   // threshold ceiling (A ×10)
  CSV3_IExcessTau,     // EMA time constant (ms, raw int)
  CSV3_OutputPIDSigSrc,
  CSV3_TdPred,          // %.3f
  CSV3_OvMeasMarginV,   // %.3f
  CSV3_OvPredMarginV,   // %.3f
  CSV3_OutputPIDMA_N,
  CSV3_OutputPIDFilterTC,
  CSV3_VoltageFilterTC,
  CSV3_CvKdVoltFiltTC,
  CSV3_CvKdDeadbandVps,
  CSV3_VoltageKd,
  CSV3_DvdtTC,
  CSV3_CvKdArmV,
  CSV3_StartupRiseRate,
  CSV3_absorptionCompleteTime,
  CSV3_OnOff,
  CSV3_ManualFieldToggle,
  CSV3_HiLow,
  CSV3_LimpHome,
  CSV3_AlarmActivate,
  CSV3_TempAlarm,
  CSV3_VoltageAlarmHigh,
  CSV3_VoltageAlarmLow,
  CSV3_CurrentAlarmHigh,
  CSV3_AlarmTest,
  CSV3_AlarmLatchEnabled,
  CSV3_MaintainMode,
  CSV3_ManualSOCPoint,
  CSV3_IgnoreLearningDuringPenalty,
  CSV3_LogAllLearningEvents,
  CSV3_CloudFeatures,
  CSV3_AutoShuntGainCorrection,
  CSV3_AutoAltCurrentZero,
  CSV3_WindingTempOffset,
  CSV3_ManualLifePercentage,
  CSV3_UVThresholdHigh,
  CSV3_weatherModeEnabled,
  CSV3_imuEnabled,
  CSV3_AbsorptionVoltage,
  CSV3_AbsorptionTimeoutMs,
  CSV3_bulkVoltageHoldMs,
  CSV3_capLimitMode,
  CSV3_TargetVoltageMode,
  CSV3_TargetVoltageSetpoint,
  CSV3_RebulkCurrent_A,
  CSV3_UseFloat,
  CSV3_IExcessFracBulk,  // BULK threshold fraction (×1000)
  CSV3_IExcessRelFrac,   // release hysteresis fraction (×1000)
  CSV3_systemIDPlantTauMs,   // fitted plant time constant (ms), persisted
  CSV3_TempAlarmLow,
  CSV3_LoadDumpDtThresh,
  CSV3_LoadDumpDtThresh1,
  CSV3_CVTuningMode,
  CSV3_cvWaveAmplitudeV,
  CSV3_cvWavePeriodSec,
  CSV3_cvKOvershoot,
  CSV3_cvConsecutiveReads,
  // 8 dead slots — kept to preserve CSV3 indices, all send 0
  CSV3_webgaugesinterval,
  CSV3_plotTimeWindow,
  CSV3_Ymin1,
  CSV3_Ymax1,
  CSV3_Ymin2,
  CSV3_Ymax2,
  CSV3_Ymin3,
  CSV3_Ymax3,
  CSV3_Ymin4,
  CSV3_Ymax4,
  CSV3_LoadDumpDtThresh3,
  CSV3_hardwarePresent,
  CSV3_testProtectionsEnabled,  // runtime flag — not persisted, resets true (enabled) on boot
  CSV3_IExcessArmMarginV,       // %.3f — iExcess voltage gate, independent of OvMeasMarginV
  CSV3_FastSetpointRiseRate,    // ×100, 1 decimal — multiplier on setpoint rise slew during post-protection recovery
  CSV3_FastSetpointRiseWindowMs, // raw ms — hard upper bound on fast-rise window
  CSV3_FastSetpointRiseHeadroomV, // ×100, 2 decimal — V below target at which fast-rise gate stays open
  CSV3_SolarWatts,
  CSV3_performanceRatio,        // ×100, 2 decimal
  CSV3_VeData,                  // 0/1
  CSV3_NMEA0183Data,            // 0/1
  CSV3_NMEA2KData,              // 0/1
  CSV3_timeAxisModeChanging,    // 0/1
  CSV3_timeSourceMode,       // 0=auto, 1=NMEA-forced, 2=Phone-forced, 3=NTP-time-forced
  CSV3_speedSourceMode,         // 0=NMEA 2000, 1=phone GPS (speed/course owner — selectable, never auto)
  // Fast alt-current diagnostic knobs (Pattern B echo)
  CSV3_faEnabled,               // 0/1 — global ON/OFF
  CSV3_faAlarmEnable,           // 0/1 — FAULT drives audible alarm
  CSV3_faAnomPause,             // 0/1 — freeze anomaly flipbook slots
  CSV3_faRpmEdgeMargin,         // RPM ×10
  CSV3_faAmpsDriftFloorA,       // A ×100
  CSV3_faAmpsDriftPct,          // percent ×10
  CSV3_faAttenUpAmps,           // A ×10
  CSV3_faAttenDownAmps,         // A ×10
  CSV3_faPeakMinA,              // A ×100
  CSV3_wifiNapEnabled,          // 0/1 — WiFi Napping standby toggle (Client only)
  CSV3_imuHeelOffset,           // captured rest heel offset (deg ×100); on CSV3 so the Level Zero echo is fast
  CSV3_imuPitchOffset,          // captured rest pitch offset (deg ×100)
  CSV3_systemIDTestType,        // 0=step, 1=sine sweep (Plant Delay test type)
  CSV3_systemIDSineFreqStart,   // Hz ×10
  CSV3_systemIDSineFreqEnd,     // Hz ×10
  CSV3_systemIDSineCycles,      // analysed cycles per sweep frequency
  CSV3_tuningWaveform,          // 0=square, 1=sine manual, 2=sine auto-sweep
  CSV3_tuningSineFreq,          // Hz ×10 (manual sine frequency)
  CSV3_tuningSweepStart,        // Hz ×10
  CSV3_tuningSweepEnd,          // Hz ×10
  CSV3_tuningSweepCycles,       // analysed cycles per sweep frequency
  CSV3_SystemIDStabilizeAmps,   // A ×10 — plant-delay baseline/trough current
  CSV3_tuningWaveFloor,         // A — Current Target Generator wave floor (trough), shared square + sine
  CSV3_commissionState,         // auto-commissioning state: 0=not, 1=in-progress, 2=commissioned
  CSV3_commissionPhase,         // current wizard phase: 0=Prep…8=Stress test, 9=finished
  CSV3_commissionDoneMask,      // per-stage completion bitmask (bit i = stage i done)
  CSV3_cvHelpersEnabled,        // master switch: asymmetric KiDown unwind + CV D term (1=on)
  CSV3_MinChargeTempF,          // cold-charge lockout board-temp floor (°F)
  CSV3_coldChargeLockoutEnable, // cold-charge lockout master on/off (1=on)
  CSV3_cvGainMode,              // CV gain mode: 0=Manual, 1=Auto (α/K anchored)
  CSV3_cvPlantK,                // measured plant gain K (V/A); ×10000
  CSV3_cvComputedKp,            // Auto-computed Kp (12V-equiv); ×100
  CSV3_cvComputedKi,            // Auto-computed Ki (12V-equiv); ×100
  CSV3_cvCrossover,             // CV crossover ω_c (rad/s); ×100
  CSV3_cvPiZero,                // CV PI integral zero ρ (rad/s); ×100
  CSV3_vTgtRampUp,              // CV voltage-target ramp UP rate (V/s); ×1000
  CSV3_vTgtRampDn,              // CV voltage-target ramp DOWN rate (V/s); ×1000
  CSV3_vTgtRampEnable,          // CV voltage-target slew master switch (0/1)
  CSV3_setpointSlewEnable,      // inner-loop current setpoint slew master switch (0/1)
  CSV3_cvRiseGovEnable,         // CV rise governor / anti-windup master switch (0/1)
  CSV3_dutySlewEnable,          // field duty slew master switch (0/1)
  CSV3_CommissionTempF,         // board temp when CV fit applied — derate reference (°F ×10; ROLL_EMPTY = unset/NaN)
  CSV3_battTempDerateEnable,    // battery-temp gain derate master on/off (0/1)
  CSV3_battTempCoeff,           // battery fractional resistance change per °C; ×10000
  CSV3_TempPIDKiDownFrac,       // thermal velocity-form below-setpoint integral bleed ratio (×Ki); ×1000
  CSV3_ThermalSlopeWindowSec,   // thermal slope backward-difference window (s); integer
  CSV3_BattCurrentLimitA,       // max battery charge current (A ×10, G4); 0 = disabled — ceiling on the alternator command = limit + house-load offset
  // measured-ripple capture admission gates (§10.8/§11) — own knobs, decoupled from the fa* detector gates
  CSV3_ripWinMs,                // pk-pk capture window (ms, integer)
  CSV3_ripDriftFloorA,          // shared floor: command-travel gate + stationarity mean-shift tolerance (A ×100)
  CSV3_ripDriftPct,             // command-travel gate slope (% of mean, ×10) — command gate only since §11
  CSV3_SocAlarmLow,             // low-SoC alarm threshold (%, integer); 0 = disabled
  CSV3_battMaxMode,             // battery V/I plot sampling: 0 = window mean, 1 = max-magnitude
  CSV3_IExcessBaseA,            // over-current trip-line intercept / CV base (A ×10)
  CSV3_IExcessCcOffsetA,        // CC trip line offset above CV (A ×10)
  CSV3_BatteryShuntPresent,     // 1 = INA228 battery shunt fitted; 0 = no battery-current sensor
  CSV3_cvRecovEnable,           // post-protection integrator-refill master switch (0/1)
  CSV3_cvRecovSec,              // retired timed-window knob; slot kept (never repurpose); ×10
  CSV3_cvRecovEmaxV,            // retired timed-window knob; slot kept (never repurpose); ×1000
  CSV3_testSlewMode,           // manual CC square-wave test slew mode (0=off, 1=default rates, 2=custom)
  CSV3_cvTestSlewMode,         // manual CV square-wave test slew mode (0=off, 1=default rates, 2=custom)
  CSV3_CvKdOneSided,           // CV D-term mode: 1=one-sided (removes current only), 0=symmetric
  CSV3_fieldDecayTauMs,        // commissioned field drain time, command→10% of output (ms); worst-case (longest) endpoint of the drain-vs-RPM line, or the flat value
  CSV3_commissionManualMask,   // per-stage set-by-hand bitmask (skip / mark-done-manually); pairs with commissionDoneMask
  CSV3_CvKdMaxTrimA,           // CV D-term back-off ceiling (A ×10); caps kdTrim so a fast rise saturates instead of flooring the field
  CSV3_cvAlpha,                // CV auto-gain aggressiveness α (fraction of the deadbeat-ohmic gain); ×1000
  CSV3_CvKdSlopeCeil,          // CV D-term slope ceiling (V/s real per-bus ×10) — max slope the D acts on
  CSV3_cvComputedKd,           // Auto-computed D gain Kd = CvKdTd·cvComputedKp (12V-equiv); ×100
  CSV3_CvKdDbSlope,            // CV D-term deadband line slope (V/s per A ×10000)
  CSV3_CvKdDbFloor,            // CV D-term deadband line floor (V/s ×100)
  CSV3_CvKdDbCeil,             // CV D-term deadband line ceiling (V/s ×100)
  CSV3_cvRecovBoostEnable,     // post-protection recovery P-boost master switch (0/1)
  CSV3_cvRecovBoostMax,        // recovery P-boost max multiplier at full shortfall; ×100
  CSV3_cvRecovBoostErrV,       // recovery P-boost full-boost shortfall (V per 12V block); ×1000
  CSV3_fdDrainLoMs,            // drain-vs-RPM line: drain (ms) at fdDrainRpmLo; 0 = no line (flat fieldDecayTauMs)
  CSV3_fdDrainHiMs,            // drain-vs-RPM line: drain (ms) at fdDrainRpmHi
  CSV3_fdDrainRpmLo,           // drain-vs-RPM line: lowest tested RPM (lookup clamps here)
  CSV3_fdDrainRpmHi,           // drain-vs-RPM line: highest tested RPM (lookup clamps here)
  CSV3_HardOCEnable,           // Group 0 hard over-current trip enable (0/1)
  CSV3_IExcessEnable,          // Group 3 iExcess detectors enable (0/1, gates CV + bulk)
  CSV3_BattLimitEnable,        // Group 4 battery charge-current ceiling enable (0/1)
  CSV3_CvKdExcessMode,         // CV D-term response shape (1 = slope excess over the tolerance line, 0 = legacy full-slope latch)
  CSV3_CvStressDropV,          // stress-test target headroom below settled idle (V 12V-equiv ×100, class-scaled at use)
  CSV3_CvStressFailBandV,      // stress-test stability fail band (V 12V-equiv ×100, class-scaled at use)
  CSV3_CvBrakeFallRate,        // brake-tier setpoint fall rate while CV D-term removes current (A/s ×100)
  CSV3_cvRecovKiMax,           // refill Ki multiplier at release, tapering to 1x as the deficit heals; ×100
  CSV3_cvWindDownEnable,       // commanded-target wind-down governor master switch (0/1)
  CSV3_cvWindDownRate,         // wind-down shed rate (fraction of MaxTableValue per second); ×1000
  CSV3_cvWindDownStopV,        // wind-down stop margin above commanded target (V real per-bus, class-scaled at store); ×1000
  CSV3_LoadDumpEnable,         // Group 5 load dump enable (0/1)
  CSV3_loadServeBoostEnable,   // load-serve Ki boost toward measured house loads (0/1, shunt-gated)
  CSV3_reseedCorrEnable,       // demand-corrected reseed: load-drop subtraction + rapid-refire ratchet (0/1)
  CSV3_HuntGovEnable,          // hunt-governor (oscillation damper) master switch (0/1)
  CSV3_ReseedFracNoShunt,      // no-shunt recovery seed fraction (×100)
  CSV3_CvRecovClimbRate,       // recovery climb floor rate, fraction of MaxTableValue/s (×100)
  CSV3_protTestCutMs,          // protection-test manual hard-cut hold (ms)
  CSV3_protTestGapMs,          // protection-test gap between repeated cuts (ms)
  CSV3_protTestReps,           // protection-test repeat count
  CSV3_protTestAmps,           // protection-test energize target current (A); 0 = auto-seed at fire
  CSV3_cvRecovBoostFloorV,     // recovery P-boost dead area below target (V per 12V block); ×1000
  CSV3_cvRecovDeepBandV,       // deep-recovery band (V per 12V block); ×1000
  CSV3_cvRecovDeepMult,        // starve-walk rate multiplier at full depth; ×100
  CSV3_cvRecovFlareBandV,      // arrival flare band (V per 12V block); ×1000
  CSV3_cvRecovFlareFrac,       // arrival flare ceiling floor, fraction of recovery goal; ×100
  CSV3_TachLieEnable,          // tach-lie plausibility cut enable (0/1)
  CSV3_n2kTxEnable,            // NMEA2000 transmit master (0/1) — mode applied at boot
  CSV3_n2kDeviceInstance,      // N2K device instance
  CSV3_n2kBattEnable,          // battery 127508+127506 pair (0/1)
  CSV3_n2kBattInstance,
  CSV3_n2kBattCfgEnable,       // 127513 battery configuration (0/1)
  CSV3_n2kAltEnable,           // alternator 127508+127506 DCType=Alternator pair (0/1)
  CSV3_n2kAltInstance,
  CSV3_n2kAltTempEnable,       // 130312 alternator temperature (0/1)
  CSV3_n2kTempInstance,
  CSV3_n2kTempSource,          // tN2kTempSource code (3 = Engine Room)
  CSV3_n2kChgrEnable,          // 127507 charger status (0/1)
  CSV3_n2kChgrInstance,
  CSV3_n2kChgrCfgEnable,       // 127510 charger configuration, carries field drive % (0/1)
  CSV3_n2kChgrMode,            // tN2kChargerMode label: 0 Standalone, 1 Primary, 2 Secondary
  CSV3_n2kEngRpmEnable,        // 127488 engine RPM (0/1)
  CSV3_n2kEngInstance,
  CSV3_n2kEngDynEnable,        // 127489 engine dynamic (0/1)
  CSV3_n2kEngBitsEnable,       // discrete warning bits inside 127489 (0/1)
  CSV3_n2kRxBattInstance,      // battery instance to ingest (127508/127506 receive)
  CSV3_dvccEn,                 // DVCC follow master (0/1)
  CSV3_dvccSrcType,            // authority dialect: 0 Victron VE.Can (VREG), 1 RV-C
  CSV3_dvccInst,               // RV-C DC instance filter (0 = any)
  CSV3_dvccSilenceS,           // silence timeout (s)
  CSV3_dvccSettleS,            // settling time (s)
  CSV3_dvccCvlMin,             // plausible-CVL window low (V ×100)
  CSV3_dvccCvlMax,             // plausible-CVL window high (V ×100)
  CSV3_HuntCutPct,             // damper test/pocket gain, % of user Ki
  CSV3_HuntVerifyPct,          // damper verify bar, % ripple reduction required
  CSV3_HuntWingPct,            // damper pocket taper width, % of speed per side
  CSV3_HuntCooldownMin,        // damper retest cooldown after a failed test (min)
  CSV3_HuntSteadyPct,          // damper engine-speed steadiness tolerance (%)
  CSV3_HuntQualifyScans,       // damper wobble-confirm scan count (1.6 s each)
  CSV3_HuntTrigPct,            // damper detection bar: peak-bin duty swing % (x100)
  CSV3_NMEA0183Baud,           // NMEA 0183 serial baud (4800 / 9600 / 19200 / 38400)
  CSV3_NMEA0183Invert,         // NMEA 0183 UART polarity: 0 RS-232-level talker, 1 TTL-level talker
  CSV3_displayVolUnit,         // fuel volume display preference: 0 US gallons, 1 litres

  CSV3_gpsPositionSource,      // 0=auto, 1=NMEA-forced, 2=phone-forced (position only; the clock is CSV3_timeSourceMode)
  CSV3_MaxFieldVolts,          // field-volt cap (x10). The enforced ceiling it produces is CSV2_fieldDutyCeil — voltage-dependent, so it lives on the 5s channel, not here
  CSV3_OvTierLoMarginV,        // timed OV cut LOW-tier margin above target (V, %.3f)
  CSV3_OvTierLoDwellMs,        // timed OV cut LOW-tier continuous dwell (ms; 0 = tier off)
  CSV3_OvTierMidMarginV,       // timed OV cut MID-tier margin above target (V, %.3f)
  CSV3_OvTierMidDwellMs,       // timed OV cut MID-tier continuous dwell (ms; 0 = tier off)
  CSV3_VoltageHardwareLimit,   // INA228 hardware shutdown voltage (x100) — top OV-ladder rung, user setting
  CSV3_LoadDumpN1,             // load-dump tier 1 consecutive-sample count (time to act = N x ~5 ms)
  CSV3_LoadDumpN2,             // load-dump tier 2 consecutive-sample count
  CSV3_LoadDumpN3,             // load-dump tier 3 consecutive-sample count
  CSV3_solarLearnEnable,       // 0/1 — learn performanceRatio from each complete day's actual/forecast harvest
  CSV3_solarUseConsEnable,     // 0/1 — size the solar-pause bar from predicted consumption
  CSV3_solarConsMarginPct,     // x100 — headroom over predicted consumption (%)
  CSV3_solarLearnRatePct,      // x100 — per-day blend weight into performanceRatio (%)
  CSV3_rvcTxEnable,            // RV-C transmit master (0/1) — bus mode applied at boot
  CSV3_rvcChgrEnable,          // RV-C CHARGER_STATUS / _2 / _3 / CONFIGURATION_STATUS (0/1)
  CSV3_rvcDcEnable,            // RV-C DC_SOURCE_STATUS_1/2/3 at the alternator instance (0/1)
  CSV3_rvcFaultEnable,         // RV-C DM_RV diagnostic message (0/1)
  CSV3_rvcChgrInstance,        // RV-C charger instance (49 = alternator type nibble + instance 1)
  CSV3_rvcDcInstance,          // RV-C DC source instance for the alternator
  CSV3_rvcDevPriority,         // RV-C device priority (80 = Charger, below a BMS 120)
  // Battery + extra temperature probes, source chain, hot-charge lockout (BATTERY_TEMP_SENSORS_SPEC.md §6)
  CSV3_battTempProbeEnable,    // BATT-role probe feeds the battery-temperature source chain (0/1)
  CSV3_extraTempProbeEnable,   // EXTRA-role probe live (0/1)
  CSV3_battTempSource,         // 0 Auto, 1 Probe, 2 NMEA 2000, 3 VE.Direct, 4 RV-C, 5 Board, 6 None
  CSV3_battTempProxyEnable,    // Auto may fall back to the board temperature (0/1)
  CSV3_hotChargeLockoutEnable, // hot-charge lockout master on/off (0/1)
  CSV3_MaxChargeTempF,         // hot-charge lockout ceiling (°F)
  CSV3_extraTempAlarmHiEnable, // EXTRA-probe high alarm (0/1)
  CSV3_extraTempAlarmHiF,      // EXTRA-probe high alarm threshold (°F)
  CSV3_extraTempAlarmLoEnable, // EXTRA-probe low alarm (0/1)
  CSV3_extraTempAlarmLoF,      // EXTRA-probe low alarm threshold (°F)
  CSV3_n2kExtraTempEnable,     // 130312 for the EXTRA probe (0/1)
  CSV3_n2kExtraTempInstance,
  CSV3_n2kExtraTempSource,     // tN2kTempSource code for the EXTRA probe
  CSV3_CommissionTempSrc,      // battTempActiveSrc when CommissionTempF was stamped (0 = legacy/unknown = board)
  CSV3_sessionId,              // boot identity — matches CSV1_sessionId while this cached block is from the live run
  CSV3_sendMs,                 // millis() when this settings echo was built. CSV3 is event-driven with a 60 s
                               // fallback, so an age much past ~60 s means the echo stopped arriving and every
                               // setting in this block predates whatever the device is actually running.

  CSV3_FIELD_COUNT  // enum position is authoritative — never hand-count; CSV payload specifier count must equal this +1
};


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

  TS_sessionId,     // boot identity — matches CSV1_sessionId while this cached block is from the live run
  TS_sendMs,        // millis() when this payload was built. Every other TS field is an AGE, so without this
                    // a frozen block reads as a set of plausible ages that simply stopped advancing.

  TS_FIELD_COUNT  // = 41
};


// Cap current table functions
float getCapCurrentForRPM(float rpm);
void saveCapCurrentTableToNVS();
void loadCapCurrentTableFromNVS();
void loadCapTablesForMode(int mode);

int SafeInt(double f, int scale = 1) {
  // where this is matters, don't move!!
  // Param widened to double so AllTime accumulators don't narrow at call sites; float callers promote implicitly.
  // NaN/Inf -> ROLL_EMPTY, not -1: -1 is a legitimate reading for signed currents, angles, deltas and
  // Celsius temperatures, so "no sensor" and "a real -1" were indistinguishable on the wire. The browser
  // rewrites anything <= -1999999999 to NaN at parse time and every renderer prints an em dash for it.
  return isnan(f) || isinf(f) ? ROLL_EMPTY : (int)round(f * scale);
}
// %.1f on a NAN emits "nan", which is not valid JSON and kills the whole record at the parser. The
// tuning/sysid/CV ledger records stamp NAN when no probe had read; null is the honest carrier.
const char *jsonNum1(char *buf, size_t n, float v) {
  if (isnan(v) || isinf(v)) snprintf(buf, n, "null");
  else snprintf(buf, n, "%.1f", v);
  return buf;
}
void loadAPCredentials(bool forceDefaults = false) {
  if (forceDefaults) {
    esp32_ap_ssid = "ALTERNATOR_WIFI";
    esp32_ap_password = "alternator123";
    Serial.println("Using default AP credentials for password recovery or first boot");
    return;
  }

  // Load custom credentials from NVS
  if (settingExists(NK_apssid)) {
    esp32_ap_ssid = settingRead(NK_apssid);
    esp32_ap_ssid.trim();
    if (esp32_ap_ssid.length() == 0) {
      esp32_ap_ssid = "ALTERNATOR_WIFI";
    }
  } else {
    esp32_ap_ssid = "ALTERNATOR_WIFI";
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

  // Boot-mode straps:
  //   GPIO41 LOW — boot the factory partition (emergency recovery from a bad OTA)
  //   GPIO45 LOW — force CONFIG mode (WiFi setup / password recovery); alternator DISABLED, deliberately:
  //                config mode means settings are unknown/unconfigured, so charging must not run
  //   GPIO46     — LOW = AP mode, HIGH = Client mode. Both run the alternator fully, so holding 46 LOW is
  //                the credential-free emergency path: join ALTERNATOR_WIFI, browse to 192.168.4.1

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

  // GPIO45 = Configuration Mode Override (always checked first)
  if (forceConfigMode) {
    Serial.println("=== GPIO45 LOW: FORCED CONFIGURATION MODE ===");
    Serial.println("=== ALTERNATOR DISABLED FOR SAFETY - Use GPIO46 LOW for emergency operation ===");
    loadAPCredentials(true);  // Force defaults for password recovery
    setupAccessPoint();
    setupWiFiConfigServer();  // Always serve config interface when GPIO45 is low.  This can be used to reset lost passwords
    currentMode = MODE_CONFIG;
    return;  // Exit setupWiFi() - alternator will not run
  }

  // GPIO46 = Operational Mode Selection (checked early to allow emergency AP access on first boot)
  // This check happens before isFirstBoot to ensure AP mode works even on first boot
  // Rationale: If user has no WiFi or WiFi router is down, they can still run the regulator in AP mode
  // This allows emergency operation: GPIO41 low = factory firmware, GPIO41 high = OTA firmware (if valid OTA exists, else factory)
  // GPIO46 low = AP mode regardless of any credentials of any kind
  // Settings persist in userdata partition; AP credentials default to ALTERNATOR_WIFI/alternator123

  if (requestAPMode) {
    Serial.println("=== GPIO46 LOW: OPERATIONAL AP MODE ===");
    loadAPCredentials(false);  // Load custom credentials for normal AP operation
    setupAccessPoint();
    currentMode = MODE_AP;
    CloudFeatures = 0;
    if (webFS.exists("/index.html.gz")) {
      Serial.println("Serving full alternator interface in AP mode");
      setupServer();
    } else {
      Serial.println("No index.html.gz found - serving basic landing page");
      setupCaptivePortalLanding();
    }
    return;
  }
  // Check if this is truly first boot (no configuration has ever been done)
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

  // Check if the device has ever been configured with WiFi credentials before
  bool hasClientConfig = settingExists(NK_ssid);

  // Load and cache WiFi client credentials once
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

  // If no saved credentials, enter config mode
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

// mDNS starts once and never MDNS.end(): ESP32 core 3.3.8's mdns teardown null-derefs the
// netif (LoadProhibited crash on wake/reconnect). mDNS stays bound to the persistent STA
// netif across reconnects, so it keeps working without a restart.
void startMdnsOnce() {
  static bool mdnsStarted = false;
  if (!mdnsStarted && MDNS.begin("alternator")) {
    Serial.println("mDNS responder started");
    MDNS.addService("http", "tcp", 80);
    mdnsStarted = true;
  }
}

// A failed join only prints WiFi.status()==6 ("not connected"), which can't tell a wrong
// password from an AP refusing the association (e.g. an MVNO hotspot device cap) from an SSID
// not found. This logs the AP's real disconnect reason so phone-hotspot joins are diagnosable.
static const char *wifiDisconnectReasonStr(uint8_t reason) {
  switch (reason) {
    case 2:   return "AUTH_EXPIRE";
    case 4:   return "ASSOC_EXPIRE";
    case 5:   return "ASSOC_TOOMANY (AP at device limit)";
    case 6:   return "NOT_AUTHED";
    case 7:   return "NOT_ASSOCED";
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

static void onWiFiStaDisconnected(WiFiEvent_t event, WiFiEventInfo_t info) {
  uint8_t reason = info.wifi_sta_disconnected.reason;
  lastStaDiscReason = reason;
  Serial.printf("WiFi STA disconnect reason: %u (%s)\n", reason, wifiDisconnectReasonStr(reason));
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
<p style='margin-top:24px;font-size:14px;color:#666'><strong>Network:</strong> ALTERNATOR_WIFI<br><strong>Device IP:</strong> 192.168.4.1</p>
</div></body></html>)HTML";
    request->send_P(200, "text/html", PAGE);
  });
  server.begin();
  Serial.println("Landing page ready");
}

// Re-entrant on purpose. Engine-off standby powers the radio all the way down
// (enterLowPowerStandby -> WiFi.mode(WIFI_OFF)) to save standby draw, so ignition-on and the GPIO5
// wake button have to be able to raise the softAP again. A one-shot guard here left the radio off
// until a reboot. Only the mDNS start below stays one-shot.
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

    // Start mDNS in AP mode too (best-effort for alternator.local).
    // No MDNS.end() — core 3.3.8 mdns teardown null-derefs the netif (see client path); start once.
    static bool apMdnsStarted = false;
    if (!apMdnsStarted && MDNS.begin("alternator")) {
      MDNS.addService("http", "tcp", 80);
      apMdnsStarted = true;
      Serial.println("mDNS started - alternator.local available (may not work on all devices in AP mode)");
    } else if (!apMdnsStarted) {
      Serial.println("mDNS failed to start in AP mode");
    }

    Serial.println("=== AP SETUP COMPLETE ===");
  } else {
    Serial.println("=== ACCESS POINT FAILED TO START ===");
    Serial.println("This is a critical error!");
    Serial.println("Trying with default settings as fallback...");
    WiFi.softAP("ALTERNATOR_WIFI", "alternator123");
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
        "<p><strong>AP password must be at least 8 characters or left blank for default.</strong></p>"
        "<p><a href='javascript:history.back()'>Go Back and Try Again</a></p>"
        "</body></html>";
      request->send_P(400, "text/html", ERR);
      return;
    }

    if (ap_password[0] == '\0') {
      strncpy(ap_password, "alternator123", sizeof(ap_password) - 1);
      ap_password[sizeof(ap_password) - 1] = '\0';
    }

    Serial.println("=== SAVING CONFIGURATION ===");
    Serial.printf("SSID: '%s' (length: %u)\n", ssid, (unsigned)strlen(ssid));
    Serial.printf("Password: '%s' (length: %u)\n", password, (unsigned)strlen(password));

    // Credentials persist in NVS (settings namespace) — no LittleFS dependency here
    if (!settingWrite(NK_appass, ap_password)) {
      Serial.println("CRITICAL: NVS write failed");
      request->send(500, "text/plain", "Storage error - cannot save configuration");
      return;
    }
    esp32_ap_password = ap_password;

    if (hotspot_ssid[0] != '\0') {
      settingWrite(NK_apssid, hotspot_ssid);
      esp32_ap_ssid = hotspot_ssid;
    } else {
      settingRemove(NK_apssid);  // cleared custom AP SSID -> back to default
      esp32_ap_ssid = "ALTERNATOR_WIFI";
    }

    // Blank client SSID = user only came for AP settings — leave stored client creds untouched
    // (mirrors the blank-hotspot_ssid handling; an unconditional write here wiped working WiFi).
    // Deliberate erase goes through the explicit forget_client checkbox instead.
    bool forgetClient = (ssid[0] == '\0' && request->hasParam("forget_client", true));
    if (ssid[0] != '\0') {
      settingWrite(NK_ssid, ssid);
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
    delay(1000);

    request->send(200, "text/plain", ssid[0] != '\0'
                                       ? "Configuration saved! Device will restart in 3 seconds."
                                     : forgetClient
                                       ? "Ship's WiFi credentials erased. Device will restart in 3 seconds into this setup page (charging disabled until WiFi is configured or the Hotspot Wire is used)."
                                       : "Configuration saved (client WiFi credentials unchanged). Device will restart in 3 seconds.");

    Serial.println("=== CONFIGURATION SAVED - RESTARTING ===");
    settingWrite(NK_first_config_done, "1");
    delay(3000);
    ESP.restart();
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

// Raw WiFiClientSecure POST for the 4 registration/profile cloud ops (/checkRegistration
// /registerProfile /updateProfile /deleteAllData — executed via executeCloudOp on the httpsTask
// worker, never inside a web handler). Deliberately NOT HTTPClient: HTTPClient::getString() hangs on
// TLS, and http.begin(url) with no WiFiClientSecure has undefined behavior on https URLs. Pattern
// mirrors executeUploadPayload() in 2_functions.ino. Returns HTTP status code (e.g. 200,
// 401) on success, or a negative sentinel on transport failure:
//   -1 = low heap, -2 = connect fail, -3 = handshake/global timeout, -4 = send fail,
//   -5 = read timeout / no status line.
// responseBuf is null-terminated on return; pass nullptr if body not needed.
int doCloudPOST(const char *endpointPath, const char *payload,
                char *responseBuf, size_t responseBufSize) {
  if (responseBuf && responseBufSize > 0) responseBuf[0] = '\0';

  // Hard floor only — setInsecure() TLS footprint is ~5-10 KB. Do not raise it:
  // a 40 KB threshold (sized for setCACert) tripped right after sensor uploads
  // while heap wasn't reclaimed yet. A real OOM makes client.connect() return
  // false → -2 with diagnostic.
  if (ESP.getMaxAllocHeap() < 15000) {
    Serial.printf("doCloudPOST(%s): heap critically low (max alloc %u), aborting\n",
                  endpointPath, ESP.getMaxAllocHeap());
    return -1;
  }

  // Pre-resolve DNS with a WDT feed so a slow lookup (~15 s worst on dead cellular) can't stack
  // into the same unfed window as the up-to-14 s TCP+TLS connect; connect() then hits the lwIP cache.
  IPAddress preResolved;
  if (!WiFi.hostByName(host, preResolved)) {
    Serial.printf("doCloudPOST(%s): DNS lookup failed\n", endpointPath);
    queueConsoleMessageF("Cloud unreachable: DNS lookup for %s failed (check WiFi/router DNS)", host);
    return -2;
  }
  esp_task_wdt_reset();

  WiFiClientSecure client;
  // Match executeUploadPayload (2_functions.ino) which uses setInsecure(). setCACert(server_root_ca)
  // does NOT work here: that cert is our Let's Encrypt ISRG Root X1, but Supabase fronts behind
  // Cloudflare and presents a different chain, so verification fails (mbedTLS -0x2700) and
  // connect() returns false. Deliberate — marine threat model is trusted owner-WiFi.
  client.setInsecure();
  // setTimeout omitted: Stream::setTimeout is ms (not seconds as some docs claim) and
  // our read loops use available()+read() polling with explicit millis() deadlines,
  // so the Stream-level timeout doesn't gate anything in this path.
  client.setHandshakeTimeout(HANDSHAKE_TIMEOUT / 1000);  // this API takes seconds

  uint32_t start = millis();
  esp_task_wdt_reset();

  if (!client.connect(host, port, CONNECT_TIMEOUT)) {
    uint32_t connElapsed = millis() - start;  // how long the failed TLS connect took (before extra probes below)
    esp_task_wdt_reset();                     // the failed connect may have just eaten 14 s of the WDT window
    char errBuf[128] = {0};
    int lastErr = client.lastError(errBuf, sizeof(errBuf));
    // Disambiguate the three distinct failure modes for the dashboard Console — a generic
    // mbedTLS -1 can be any of them and they need very different fixes:
    //   (a) DNS lookup fails        -> router/WiFi DNS problem
    //   (b) DNS ok, raw TCP blocked -> firewall/router blocking outbound 443
    //   (c) DNS ok, TCP ok, TLS bad -> secure handshake fails (TLS version/cipher, or MTU dropping
    //                                  the large Cloudflare cert) even though a browser works
    IPAddress resolved;
    bool dnsOk = WiFi.hostByName(host, resolved);
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
      // TCP works but the TLS handshake failed. The usual cause on this device is NOT the network
      // but scarce CONTIGUOUS internal RAM — mbedTLS needs ~32-40KB in one block, and field-on
      // workload (control loop, SSE, logging) fragments it. Report the largest free block so a
      // low number (and a fast fail, not a ~5s timeout) points at RAM, not the network.
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
  // HTTP/1.0 deliberately: Cloudflare (which fronts Supabase) sends
  // `Transfer-Encoding: chunked` to HTTP/1.1 clients, and our simple read-until-EOF
  // body loop below has no chunked-decoder. HTTP/1.0 forces Cloudflare to send a
  // plain body + `Connection: close`, which the loop handles correctly. We don't
  // need keep-alive here (each handler does at most one POST).
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

  size_t sent = client.write((const uint8_t *)payload, payloadLen);
  if (sent != payloadLen) {
    Serial.printf("doCloudPOST(%s): payload write SHORT sent=%u expected=%u\n",
                  endpointPath, (unsigned)sent, (unsigned)payloadLen);
    client.stop();
    return -4;
  }

  // Read status line into local buffer
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

  // Drain headers until \r\n\r\n, then read body into responseBuf
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

// Plain-English reason for a doCloudPOST() negative return code, for the dashboard Console
// (saves a trip to the Serial Monitor). Mirrors the sentinels returned above.
const char *cloudFailReason(int code) {
  switch (code) {
    case -1: return "internal memory too low for secure connection";
    case -2: return "could not reach cloud server (check this WiFi has internet)";
    case -3: return "timed out after connecting";
    case -4: return "lost connection while sending";
    case -5: return "no response from cloud server";
    default: return "unknown error";
  }
}

// ── Async cloud-op slot: /checkRegistration /registerProfile /updateProfile /deleteAllData stage
// here and answer 202 immediately; the httpsTask worker runs doCloudPOST + the old handler's
// post-processing (executeCloudOp below); /cloudOpState serves the finished (code, body) to the
// polling client. One op in flight at a time — these are single-user dashboard actions. Plain
// globals (no slot struct) because Arduino's auto-prototype generator can't parse sketch-defined
// types in free-function signatures.
#define CLOUDOP_CHECK_REG      1
#define CLOUDOP_REGISTER       2
#define CLOUDOP_UPDATE_PROFILE 3
#define CLOUDOP_DELETE_DATA    4
#define CLOUDOP_PAYLOAD_CAP 1024
#define CLOUDOP_RESP_CAP    2048
#define CLOUDOP_BODY_CAP    2304   // resp + envelope headroom, so pass-through can never truncate
static volatile uint8_t cloudOpState = 0;   // 0 idle, 1 pending, 2 done
static volatile uint8_t cloudOpKind = 0;
static volatile int cloudOpCode = 0;        // HTTP status the old synchronous handler would have sent
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

// Runs on the httpsTask worker (HTTPS_CLOUD_OP). Mirrors the old synchronous handlers' response
// handling exactly — token save/clear, post-registration version/forced checks, snapshot request.
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
        queueConsoleMessage("Cloud: ready to register");
        DynamicJsonDocument doc(1024);
        DeserializationError error = deserializeJson(doc, resp);
        String errorMsg = (!error && doc.containsKey("error")) ? doc["error"].as<String>() : String("");
        // Only clear credentials if the token genuinely doesn't exist in the database
        if (errorMsg == "Invalid token" || errorMsg == "Token not found") {
          Serial.println("Clearing invalid credentials - device ready for re-registration");
          clearAuthToken();
        }
        snprintf(cloudOpBody, CLOUDOP_BODY_CAP, "{\"registered\":false,\"error\":\"validation_failed\"}");
      } else {
        snprintf(cloudOpBody, CLOUDOP_BODY_CAP, "{\"registered\":false,\"error\":\"network_error\"}");
      }
      outCode = 200;   // the old handler answered 200 on every one of these paths
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
              saveAuthToken(newToken);
              Serial.println("Token saved to NVS");
              // The boot checks latched otaCheckDone while this device was still unregistered, so
              // nothing would report our version or see a forced update until the next reboot.
              // Version first: the cloud clears a forced flag once the reported version reaches it.
              // Factory partition excluded for the same reasons as the boot check in Xregulator.ino.
              if (currentPartitionType != 0) {
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
        if (httpCode == 200) {
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

// Accumulator for the /perfUploadFront POST body (Load CSV) — filled across body chunks, then
// ingested once the request completes. LAN-only single-client dashboard, so one global is fine.
static String perfUploadBuf;
static String altUploadBuf;   // same, for /altUploadFront (alternator-health Load CSV)
static String importConfigBuf;   // body accumulator for POST /importConfig (config sharing)

// Editing the DVCC source dialect or the instance filter points the follower at a different
// authority, so everything decoded from the old one has to go and the new one must settle fresh.
static void dvccResetAuthority() {
  dvccRxCvl = dvccRxCcl = NAN;  // clear the old dialect's values; a new authority must settle fresh
  dvccRxLastMs = 0;
  dvccRxSrcAddr = 255;
  dvccCvlV = dvccCclA = NAN;
  if (dvccState != 5) dvccState = (dvccEn == 1) ? 1 : 0;  // preserve an UNTRUSTED latch across source edits
  dvccCfgChanged = true;  // dvccTick also clears its decode-side state (sender lock, flap history)
  dataTimestamps[IDX_DVCC] = 0;
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

  server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest *request) {  // just get rid of annoying chrome Console error messages
    static const uint8_t blank[] = { 0 };
    request->send(200, "image/x-icon", blank, sizeof(blank));
  });


  server.on("/factoryReset", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!settingsArmActive()) {
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
            // `> count` stopped one early and always dropped the newest entry (.bin emits it).
            if (state.row > state.count + 1) {
              thermalLogPaused = false;
              state.done = true;
              return written;
            }
            if (state.row == 0) {
              // Header row
              state.lineLen = snprintf(
                state.line, sizeof(state.line),
                "ts_ms,tempFilt_F,tempProj_F,nominalTarget_A,"
                "rpmCap_A,voltCap_A,uTarget_A,spLimited_A,"
                "pidErr_A,pidOut_pct,duty_pct,RPM,battV,measAmps_A,"
                "penaltyAmps_A,flags,chargeStageDisplay,"
                "outerP,outerI,lookahead,impliedPenalty,antiWindupFired,thermalSlope_F_sec,"
                "freezeWhy,penaltyRaw_A,iCeil_A\n");
            } else if (state.row == 1) {
              // Constants row — written once, Python detects via "CONST" in ts_ms field.
              // limit/warn/crit let the plotter draw the limit + warning-trip (limit+warn) +
              // critical-trip (limit+crit) reference lines. nominalTarget column carries the
              // live regulation setpoint in °F.
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

          // Refill line buffer when previous line is exhausted
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

          // Copy as much of the current line into the output buffer as fits
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
  // Serves what is on flash PLUS the episodes still queued in RAM — queued rows only reach flash
  // once the field gate has been cut ~30 s (huntLedgerService), and a Refresh must not have to wait
  // for that to show a fresh episode. Read-only by design: this handler never writes flash. Runs on
  // the async web-server task, so the ~6 KB read costs the control loop nothing.
  server.on("/huntledger", HTTP_GET, [](AsyncWebServerRequest *request) {
    uint8_t pendN = hgLedgerPendingCount();
    size_t pendCap = (size_t)pendN * 96 + 96;
    fsTakeLock();
    File f = LittleFS.open("/huntledger.csv", "r");
    size_t fsz = f ? f.size() : 0;
    if (fsz == 0 && pendN == 0) {
      if (f) f.close();
      fsReleaseLock();
      request->send(200, "text/plain", "no hunt episodes recorded");
      return;
    }
    char *buf = (char *)ps_malloc(fsz + pendCap + 1);  // file is hard-capped at HG_LEDGER_MAX_BYTES
    size_t n = 0;
    if (buf) {
      if (fsz) n = f.read((uint8_t *)buf, fsz);
      else n = (size_t)snprintf(buf, pendCap, "%s\n", "epoch,rpm,cv,freqHz,a0_pct,aEnd_pct,steps,verdict,derateExit");
    }
    if (f) f.close();
    fsReleaseLock();
    if (!buf) {
      request->send(503, "text/plain", "hunt ledger unavailable — out of memory");
      return;
    }
    if (n && buf[n - 1] != '\n') buf[n++] = '\n';
    n += hgLedgerPendingCsv(buf + n, pendCap);
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
    char buf[384];
    size_t n = huntMapJson(buf, sizeof(buf));
    AsyncWebServerResponse *response = request->beginResponse(200, "application/json", String(buf, n));
    response->addHeader("Cache-Control", "no-cache");
    request->send(response);
  });

  // Clear System for the oscillation damper: learned pockets + episode ledger + any test in
  // flight, together. The web UI fronts this with an explicit confirm.
  server.on("/huntclear", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!settingsArmActive()) {
      request->send(403, "text/plain", "Settings not armed");
      return;
    }
    huntMapClearAll("cleared from the dashboard");
    request->send(200, "text/plain", "OK");
  });

  // ── DVCC raw-frame capture (spec §8a): candidate CAN frames as hex text, for charge-limit
  // decoder ground-truth validation from any browser/relay path. Observe-only by construction —
  // the ring fills from the receive tap; reading it never touches control. Line format:
  // "ms id_hex len data_hex", oldest first.
  server.on("/dvccCapture", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!dvccCapRing || dvccCapCount == 0) {
      request->send(200, "text/plain", "no candidate frames captured yet (needs NMEA2K receive or DVCC follow enabled, and bus traffic)");
      return;
    }
    // Pause the tap while copying out: the oldest entries — serialized first — are exactly the
    // slots a concurrent write overwrites next, and a torn line (one frame's id, another's data)
    // would corrupt the decode ground truth this endpoint exists to provide. The tap drops
    // frames only for the raw memcpy below, not for the ~4600 snprintf calls that format it —
    // those run with the tap live, off a private copy.
    // Setting the pause and reading the head/count pair under dvccCapMux is what closes the
    // handshake: the tap takes the same lock around its whole slot write, so once this critical
    // section exits with the pause set, no write can be in flight and the ring is frozen.
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
    size_t cap = (size_t)cnt * 44 + 4352;  // + header: run stamp, authority, node roster, battery record, the not-the-battery's-number note
    char *buf = (char *)ps_malloc(cap);
    if (!buf) {
      free(snap);
      request->send(503, "text/plain", "capture unavailable - out of memory");
      return;
    }
    size_t n = 0;
    // Header first. Hex frames alone are an anecdote: what makes them generalisable is who
    // published them and under which firmware. The roster is copied out of PSRAM under the lock
    // and formatted unlocked — snprintf must never run inside the critical section.
    char authTxt[24];
    if (dvccRxSrcAddr == 255) snprintf(authTxt, sizeof(authTxt), "none heard yet");
    else snprintf(authTxt, sizeof(authTxt), "addr %u", (unsigned)dvccRxSrcAddr);
    n += (size_t)snprintf(buf + n, cap - n,
                          "# XREG-010 DVCC capture - fw %s - uptime %lu ms - %u frames\n"
                          "# dialect %s - RV-C instance filter %d - follow state %u - authority %s\n"
                          "# nodes heard passively (PGN 60928 NAME / 126996 product info). Hearing nothing is\n"
                          "# normal with transmit off: neither PGN can be requested, so this is only what the\n"
                          "# bus volunteered while we were listening.\n"
                          "# addr  mfg func class   unique  prod vprod  model | sw | serial\n",
                          FIRMWARE_VERSION, (unsigned long)millis(), (unsigned)cnt,
                          dvccSrcType == 1 ? "RV-C" : "Victron VE.Can", dvccInst, (unsigned)dvccState, authTxt);
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
    // The battery behind a GX is on the GX's other CAN port and never reaches our bus, so the one
    // fact that makes these numbers generalisable is the owner's own record of what battery this
    // is. Stamped from Vessel Info rather than asked for in the covering email, which is the half
    // that gets forgotten. Sanitised on the way out: the stored strings pre-date any input filter.
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
    // Streamed straight out of PSRAM, same pattern as /huntledger — a char* send() would copy
    // the whole dump into an internal-heap String. shared_ptr deleter frees on any completion path.
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

  // ── Console history: device-side source of truth for the Download Logs console file ──
  // Tab-separated lines: epoch \t millis \t message. Client formats timestamps (pre-2020 epoch =
  // clock unsynced at queue time → shows relative uptime instead). Entries memcpy'd under
  // consoleMux so a concurrent append can't tear a line; no pause flag — the ring has no
  // tick-driven writer to freeze.
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
    request->send(response);
  });

  // ── Fast alternator-current channel: live scope dump ──
  // 500 ms raw ring (20 kSPS, calibrated mV) + 16 B header — see faScopeSnapshot for layout.
  // Snapshot is copied into a PSRAM buffer up front (spinlock vs the loop() drain), then
  // streamed; the shared_ptr deleter frees it on any completion path including client abort.
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
  // Then (refSlots+anomSlots) × FaFlipPage verbatim (2020 B each, layout pinned by
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
  // Reads the game-scoped 1-D ripTab: one row per 50-RPM bin with any state, IExcessTau-filtered pk-pk
  // (== detector mExcessEma pk-pk) captured at the game's fixed commanded current. First three columns
  // keep the legacy layout (parsers read [0..2] after a length ≥ 3 check); altSt/battSt appended:
  // 0 = none, 1 = pending (value = the unconfirmed candidate), 2 = committed. The wizard pick and the
  // game's kill/coverage logic use committed only; the game strip may draw pending dimmed.
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

  // ── Filtered-ripple fold forensics (diagnostic) ──
  // One row per ripTab fold event, oldest→newest — the "why won't this bin settle" trace. event:
  // P = pending set, D = disagree (otherA = the pending it replaced), C = commit (otherA = the pair
  // partner; the committed value is their average). pkpkA is always the window's own value.
  // crossings ~2 = one-shot transient; many = sustained oscillation.
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

  // ── Measured ripple projection (§3.3/§4) — the stored per-detector fit for the Protections plot ──
  // ripple(I)=a0+a1·I plus the 3 measured (I, pk-pk) points and the RPM the test ran at. n=0 → no test
  // yet (plot shows the threshold line only). Read on the Protections tab and by the Step-6 review.
  // "slp" = the same record shape for the CV D-term deadband: worst-positive voltage slope (V/s) vs I.
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
    request->send(200, "application/json", kneeLearnStateJson());
  });
  // Deferred commissioning-start persist status: PENDING while the loop worker retires the Start's
  // staged NVS writes, FAILED if the restore-point write was refused, IDLE otherwise. The wizard
  // polls this after commissionStart and advances only on IDLE — HTTP 200 means accepted, not saved.
  server.on("/cxStartState", HTTP_GET, [](AsyncWebServerRequest *request) {
    const char *st = (cxStartPersistStep != 0) ? "PENDING" : (cxStartPersistFail ? "FAILED" : "IDLE");
    request->send(200, "application/json", String("{\"state\":\"") + st + "\"}");
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
    request->send(200, "application/json", bhBuildStatusJson());
  });
  // Zero-drift characterization log: small status JSON for the dashboard panel (enable, fill, span).
  server.on("/zerologstate", HTTP_GET, [](AsyncWebServerRequest *request) {
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
    String j = "{\"valid\":";   j += zfValid;
    j += ",\"sensor\":\"";      j += (zfSensor == ZF_ALT ? "alt" : "board"); j += "\"";
    j += ",\"c\":";             j += String(zfC, 4);
    j += ",\"b\":";             j += String(zfB, 5);
    j += ",\"r2\":";            j += String(zfR2, 3);
    j += ",\"corrNow\":";       j += String(DynamicAltCurrentZero, 3);
    j += ",\"applied\":";       j += (AutoAltCurrentZero ? 1 : 0);
    j += ",\"epoch\":";         j += String((unsigned)zfLastEpoch);
    j += ",\"histCount\":";     j += String((unsigned)zeroFitHistCount);
    j += "}";
    request->send(200, "application/json", j);
  });
  // Multi-drop 1-Wire probe registry for Setup > Temperature (BATTERY_TEMP_SENSORS_SPEC.md §8). The registry
  // is copied under owMux first and the JSON built from that copy on the stack, so TempTask never waits on
  // the network and the handler never allocates. "role" comes from the stored role addresses, so a probe
  // that dropped off the bus still reports its role with present:0.
  server.on("/tempSensors", HTTP_GET, [](AsyncWebServerRequest *request) {
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
    solarLedgerCsvSend(request);
  });
  // Emit-window sizing probe — TEMPORARY, REMOVE AFTER AUGUST 2026 (see the probe block in 7_functions).
  server.on("/altwinstats.csv", HTTP_GET, [](AsyncWebServerRequest *request) {
    altWinStatsCsvSend(request);
  });
  // Gate-tuning capture dump: 136 B header + 17 B rows, streamed binary, decoded to CSV in the
  // browser (parseAltLogBin in script.js) and handed over with deliverFile(). Refuses while
  // recording. Clearing is a separate press (/get?altLogClear=1) so the file is safely in hand
  // before the buffer is freed.
  server.on("/altlog.bin", HTTP_GET, [](AsyncWebServerRequest *request) {
    altLogBinSend(request);
  });
  // Performance-vs-engine-hours trend (header + points, chunked). This is the headline. Decimated to
  // <= TR_MAXOUT output points for readability + payload (full hourly history stays on the device):
  // each output point is one bucket of source hours — the low line = min of the source hours' P10s
  // (preserve the early-warning envelope), overall = mean. stride==1 (<= TR_MAXOUT total) streams
  // every hour, as before.
  extern float altTrendBucketSec;   // defined in 7_functions.ino (concatenated later) — needed to convert bucket index → engine-hours
  server.on("/alttrend.csv", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!altTrend) { request->send(200, "text/plain", "engHours,worstPct,overallPct\n"); return; }
    const int TR_MAXOUT = 200;
    // Optional ?hours=N window: decimate WITHIN the last N engine-hours so the dashboard's
    // 10h/100h shortcuts aren't starved. A single global 200-pt stride over a multi-thousand-hour
    // history lands ~0 points inside a recent 10h slice; windowing the base index first fixes that.
    // hours<=0 or absent = full history (the "All" shortcut).
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
              // Stored engHour is a BUCKET INDEX. Convert to true engine-hours so the X axis is correct at
              // any bucket size (index × bucketSec/3600): at 3600 s index==hours; at 600 s six buckets = 1 h.
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

  // Load CSV (import a shared/saved polar): POST the BEFRONT1 sail+motor pair as the raw body to
  // /perfUploadFront. The body handler accumulates chunks into perfUploadBuf; the
  // request handler (runs once the body is complete) arm-gates, then perfUploadFrontCsv()
  // replaces both fronts and applies the user's chosen mode (?fixed=1 freeze / 0 learn) + persists.
  // Same ingest path as cloud-sync/Load-saved.
  server.on("/perfUploadFront", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      if (!settingsArmActive()) {
        perfUploadBuf = ""; request->send(403, "text/plain", "Settings not armed"); return;
      }
      if (perfUploadBuf.length() < 8 || perfUploadBuf.indexOf("BEFRONT1") < 0) {
        perfUploadBuf = ""; request->send(400, "text/plain", "No BEFRONT1 data in upload"); return;
      }
      char *bodyc = strdup(perfUploadBuf.c_str());
      perfUploadBuf = "";
      if (!bodyc) { request->send(500, "text/plain", "Out of memory"); return; }
      // fixed=1 → FIXED+paused (freeze), fixed=0 → LEARNED+resumed (learn). Default freeze if absent.
      bool fixed = (!request->hasParam("fixed")) || (request->getParam("fixed")->value().toInt() != 0);
      bool ok = perfUploadFrontCsv(bodyc, fixed);
      free(bodyc);
      request->send(ok ? 200 : 400, "text/plain", ok ? "OK" : "Parse failed");
    },
    NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      if (total > 131072) return;  // a real BEFRONT1 is a few KB — a mispicked 30 MB log would OOM the accumulator
      if (index == 0) { perfUploadBuf = ""; perfUploadBuf.reserve(total + 1); }
      perfUploadBuf.concat((const char *)data, len);
    });

  // Load CSV (alternator health): POST the BEFRONT1 front as the raw body to /altUploadFront.
  // Mirrors /perfUploadFront. ?fixed=1 freeze (local only) / 0 learn (adopt to cloud, tagged). The
  // request handler arm-gates once the body is complete, then altUploadFrontCsv() applies it.
  server.on("/altUploadFront", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      if (!settingsArmActive()) {
        altUploadBuf = ""; request->send(403, "text/plain", "Settings not armed"); return;
      }
      if (altUploadBuf.length() < 8 || altUploadBuf.indexOf("BEFRONT1") < 0) {
        altUploadBuf = ""; request->send(400, "text/plain", "No BEFRONT1 data in upload"); return;
      }
      char *bodyc = strdup(altUploadBuf.c_str());
      altUploadBuf = "";
      if (!bodyc) { request->send(500, "text/plain", "Out of memory"); return; }
      // fixed=1 → FIXED+paused (freeze), fixed=0 → LEARNED+resumed+adopt (learn). Default freeze if absent.
      bool fixed = (!request->hasParam("fixed")) || (request->getParam("fixed")->value().toInt() != 0);
      bool ok = altUploadFrontCsv(bodyc, fixed);
      free(bodyc);
      request->send(ok ? 200 : 400, "text/plain", ok ? "OK" : "Parse failed");
    },
    NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      if (total > 131072) return;  // same cap rationale as /perfUploadFront above
      if (index == 0) { altUploadBuf = ""; altUploadBuf.reserve(total + 1); }
      altUploadBuf.concat((const char *)data, len);
    });

  // Config Sharing — export the cloneable settings set as one JSON blob (for download
  // or cloud submission). Arm-gated like the upload endpoints.
  server.on("/exportConfig", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!settingsArmActive()) {
      request->send(403, "text/plain", "Settings not armed"); return;
    }
    request->send(200, "application/json", exportConfigJson());
  });

  // App-usage analytics delta from the web UI / Capacitor app (page dwell, button counts).
  // Deliberately NOT arm-gated: pure counter increments, no settings touched, works in AP
  // and Client mode. sendBeacon posts land here too (text/plain — content type is ignored).
  // Body rides request->_tempObject (freed by the request destructor, aborted requests included) so
  // two clients posting at once can't interleave into one shared accumulator.
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

  // Config Sharing — apply an imported config blob (POST the /exportConfig JSON as the body).
  // Only allowlisted manifest keys are written; everything else in the body is ignored.
  // Reboots after applying so InitSystemSettings re-reads the whole set consistently
  // (suppress with ?noReboot=1). Mirrors /perfUploadFront's body-accumulator pattern.
  server.on("/importConfig", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      if (!settingsArmActive()) {
        importConfigBuf = ""; request->send(403, "text/plain", "Settings not armed"); return;
      }
      if (importConfigBuf.length() < 2) {
        importConfigBuf = ""; request->send(400, "text/plain", "Empty body"); return;
      }
      char *bodyc = strdup(importConfigBuf.c_str());
      importConfigBuf = "";
      if (!bodyc) { request->send(500, "text/plain", "Out of memory"); return; }
      String preClassV = settingRead(NK_BatteryVoltage);
      // Recorded BEFORE the write burst, because the burst itself is what may stall the loop and the
      // console line is the only trace afterwards. The UI warns pre-Apply; this covers every other
      // caller (bench noReboot, curl) and leaves the stutter explained in the log.
      bool fieldWasLive = (fieldActiveStatus > 0);
      if (fieldWasLive) queueConsoleMessage("Config import starting with the field driving - each setting saved to NVS can briefly pause the control loop; prefer importing with charging off");
      int n = applyImportConfig(bodyc);
      free(bodyc);
      if (n < 0) { request->send(400, "text/plain", "No config object in body"); return; }
      // noReboot is a bench affordance (no UI path uses it) and it leaves RAM stale against the NVS the
      // manifest loop just wrote. Harmless for ordinary keys; NOT harmless for a class change, where the
      // conversion mutates RAM while the loop's overwrites land only in NVS. Force the reboot there.
      bool classMoved = (settingRead(NK_BatteryVoltage) != preClassV);
      bool reboot = classMoved || !request->hasParam("noReboot") || request->getParam("noReboot")->value().toInt() == 0;
      if (reboot) { rebootRequested = true; rebootRequestedAt = millis(); }
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
    float kp = (float)VoltageKp_active;  // gains ACTUALLY in effect (Manual or Auto α/K, 12V-block normalized) — not the raw manual VoltageKp, which the loop ignores in Auto mode / on 24-48V
    float ki = (float)VoltageKi_active;
    uint32_t interval = (uint32_t)VoltageLoopInterval;

    float kdDeadband = CvKdDeadbandVps;
    float kdActiveGain = VoltageKd_active;  // the gain the trim law multiplies (Auto Td·Kp or Manual, ×vNorm×derate) — the typed VoltageKd here once mislabeled every Auto-mode log
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


  // Barometric pressure 14-day history dump. 8-byte header + 4032 little-endian uint16
  // samples (mbar × 10; 0 = no sample). Dashboard fetches once on Other-tab activation
  // and every 5 min thereafter.
  // Header layout (little-endian):
  //   [0..1]  uint16 headIdx  — next write slot (samples[headIdx-1] is newest)
  //   [2..5]  uint32 epoch    — wall-clock seconds of newest sample (0 if never synced)
  //   [6..7]  uint16 count    — BARO_HISTORY_SIZE, for client-side sanity check
  server.on("/baroHistory.bin", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!baroPressureHistory) {
      request->send(503, "application/octet-stream", "");
      return;
    }
    struct BaroDLState {
      uint8_t  header[8];
      int      headerPos;
      uint16_t samplePos;
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
          if (state.samplePos >= BARO_HISTORY_SIZE) {
            state.done = true;
            return written;
          }
          size_t bytesLeft = (BARO_HISTORY_SIZE - state.samplePos) * sizeof(uint16_t);
          size_t canSend = min(maxLen - written, bytesLeft);
          memcpy(buf + written,
                 ((uint8_t *)baroPressureHistory) + state.samplePos * sizeof(uint16_t),
                 canSend);
          written += canSend;
          state.samplePos += canSend / sizeof(uint16_t);
        }
        return written;
      });
    response->addHeader("Cache-Control", "no-cache");
    request->send(response);
  });

  // Long Term Plots history download — header (16 B LE) + raw ring array. Lazy:
  // dashboard fetches once on tab open. JS walks records in chrono order via head +
  // count; record time = lastEpoch − (count−1−i) × intervalSec (no per-record stamp).
  //   [0..1] u16 head  [2..3] u16 count  [4..5] u16 capacity  [6..7] u16 recordSize
  //   [8..11] u32 lastEpoch  [12..15] u32 intervalSec
  server.on("/longTermPlots.bin", HTTP_GET, [](AsyncWebServerRequest *request) {
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

    // Body = `count` records in CHRONOLOGICAL order (oldest first), NOT the full ring.
    // Sending the whole 4320-slot ring (~540 KB) made the async server truncate the
    // chunked transfer (ERR_INCOMPLETE_CHUNKED_ENCODING); count×recSize stays small until
    // the ring is genuinely full. JS reads records linearly (base = 16 + i*recSize).
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
    loggingActive = false;
    request->send(200, "text/plain", "Logging stopped");
  });

  server.on("/startlogs", HTTP_POST, [](AsyncWebServerRequest *request) {
    loggingActive = true;
    request->send(200, "text/plain", "Logging started");
  });


  // Derived view of the Vessel Info NVS record — no longer a file. Deliberately ungated
  // (unlike /exportConfig): the dashboard form and the Download Logs bundle both need it
  // without arming settings. 404 until the form has been saved, which is what the client
  // reads as "vessel info incomplete".
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
  // Admin config push detail + ack. The CSV2 counts tell the dashboard THAT something is queued
  // or landed; this hands over the text the popup needs. ?ack=1 clears the applied receipt (the
  // pending notice is not ackable — it clears itself when the cloud flag goes away or the push
  // lands). Not arm-gated: it neither changes a setting nor reveals anything a CSV2 frame doesn't.
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
    if (!settingsArmActive()) {
      request->send(401, "application/json", "{\"success\":false,\"error\":\"Settings not armed\"}");
      return;
    }
    // Get JSON data
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
    // Update RAM variables
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
    // An absent/invalid key yields 0, which zeroes the full-charge tail-current threshold
    // (TailCurrent * BatteryCapacity_Ah) and the displayed capacity. Keep the current value instead.
    BatteryCapacity_Ah = constrain((int)(doc["battery_capacity_ah"] | BatteryCapacity_Ah), 1, 100000);
    PeukertRatedCurrent_A = BatteryCapacity_Ah / 20.0f;  // /get derives this on write; import must too
    vesselSetText(BATTERY_TYPE, sizeof(BATTERY_TYPE), doc["battery_type"] | "");
    // Chemistry no longer moves the INA228 limit — VoltageHardwareLimit is a persisted user
    // setting; the chemistry-specific value arrives via the battery-defaults proposal.
    // Voltage/capacity/chemistry all move the CV plant gain the commissioning fit measured, so the stored
    // tune no longer describes this bank. Only nag once a pass has actually been finished (epoch stamped).
    if (CommissionEpoch > 0 && (newBatteryVoltage != oldBatteryVoltage
                                || BatteryCapacity_Ah != oldCapacityAh
                                || strcmp(BATTERY_TYPE, oldBatteryType) != 0)) {
      raiseRecommissionNag();
    }
    vesselSetText(BATTERY_MAKE_MODEL, sizeof(BATTERY_MAKE_MODEL), doc["battery_make_model"] | "");
    vesselSetText(ALTERNATOR_BRAND_MODEL, sizeof(ALTERNATOR_BRAND_MODEL), doc["alternator_brand_model"] | "");
    SolarWatts = doc["solar_watts"];
    // Unclamped, an out-of-range value indexes past axisRemap[] and wild-reads through src[]
    uint8_t prevOrient = imuMountOrientation;
    imuMountOrientation = (uint8_t)constrain((int)(doc["imu_mount_orientation"] | 0), 0, IMU_ORIENT_COUNT - 1);
    if (imuMountOrientation != prevOrient && imuMountState != IMU_MOUNT_UNKNOWN) {
      imuMountState = IMU_MOUNT_UNKNOWN;   // verdict was judged in the old frame; next Zero re-latches it
      settingRemove(NK_imu_mnt_state);
    }
    regulatorMountLoc = doc["regulator_mount_loc"] | 0;
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
    bool foundParameter = false;
    bool nvsPersistNow = false;   // set by discrete reset/set handlers that write saveNVSDataFull()-owned vars; forces ONE immediate persist at the end so a reboot before the next field-off edge can't revert the action
    String inputMessage;

    // === SAFETY: Allow field OFF without arming ===
    if (request->hasParam("OnOff")) {
      int requestedState = request->getParam("OnOff")->value().toInt();
      if (requestedState == 0) {
        // Turning OFF is ALWAYS allowed — safety critical
        OnOff = 0;
        settingWrite(NK_OnOff, "0");
        stateRevision++;
        // Sender fingerprint: unexplained OnOff=0 arrivals were seen on the bench (2026-08).
        // The header form carries exactly one param, so extra params = a different sender,
        // and IP+UA tell laptop from phone from a stale client replaying the request.
        {
          String qs;
          for (size_t i = 0; i < request->params() && qs.length() < 120; i++) {
            const AsyncWebParameter *p = request->getParam(i);
            if (i) qs += '&';
            qs += p->name();
            qs += '=';
            qs += p->value();
          }
          const AsyncWebHeader *uaH = request->getHeader("User-Agent");
          String ua = uaH ? uaH->value() : String("-");
          if (ua.length() > 80) ua = ua.substring(0, 80);
          queueConsoleMessageF("FIELD OFF: Safety override (no arming required) [from %s | %s | UA %s]",
                               request->client()->remoteIP().toString().c_str(), qs.c_str(), ua.c_str());
        }
        request->send(200, "text/plain", "0");
        return;
      }
      // If turning ON, fall through to arm check below
    }

    if (!settingsArmActive()) {
      // Console line so a rejected write is visible — a stale/replayed URL lands here silently.
      queueConsoleMessage("Settings write REJECTED: not armed (press Unlock Settings first)");
      request->send(403, "text/plain", "Settings not armed");
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
        performOTAUpdateToVersion(selectedVersion);
        inputMessage = "Update initiated from factory partition";

      } else {
        Serial.println("UPDATE DEBUG: Storing update request in NVS");

        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open("update_req", NVS_READWRITE, &nvs_handle);
        if (err != ESP_OK) {
          Serial.printf("UPDATE DEBUG: Failed to open NVS namespace 'update_req': %s\n", esp_err_to_name(err));
          request->send(500, "text/plain", "NVS open failed");
          return;
        }

        nvs_erase_key(nvs_handle, "target_ver");
        nvs_erase_key(nvs_handle, "update_flag");

        err = nvs_set_str(nvs_handle, "target_ver", selectedVersion);
        if (err != ESP_OK) {
          Serial.printf("UPDATE DEBUG: Failed to set target_ver: %s\n", esp_err_to_name(err));
          nvs_close(nvs_handle);
          request->send(500, "text/plain", "NVS write failed");
          return;
        }

        uint8_t updateFlag = 1;
        err = nvs_set_u8(nvs_handle, "update_flag", updateFlag);
        if (err != ESP_OK) {
          Serial.printf("UPDATE DEBUG: Failed to set update_flag: %s\n", esp_err_to_name(err));
          nvs_close(nvs_handle);
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
        delay(1000);
        Serial.println("Restarting now...");
        Serial.println("========================================");
        Serial.println("NOTE: Task termination backtrace next is expected and harmless");
        Serial.println("========================================");
        Serial.flush();
        delay(5000);
        ESP.restart();
        return;
      }
    }

    if (request->hasParam("SystemIDStepAmplitude")) {
      foundParameter = true;
      inputMessage = request->getParam("SystemIDStepAmplitude")->value();
      settingWrite(NK_SystemIDStepAmplitude, inputMessage.c_str());
      SystemIDStepAmplitude = inputMessage.toFloat();
    }
    if (request->hasParam("systemIDTestType")) {
      foundParameter = true;
      inputMessage = request->getParam("systemIDTestType")->value();
      settingWrite(NK_systemIDTestType, inputMessage.c_str());
      systemIDTestType = (uint8_t)inputMessage.toInt();
    }
    if (request->hasParam("systemIDSineFreqStart")) {
      foundParameter = true;
      inputMessage = request->getParam("systemIDSineFreqStart")->value();
      settingWrite(NK_systemIDSineFreqStart, inputMessage.c_str());
      systemIDSineFreqStart = inputMessage.toFloat();
    }
    if (request->hasParam("systemIDSineFreqEnd")) {
      foundParameter = true;
      inputMessage = request->getParam("systemIDSineFreqEnd")->value();
      settingWrite(NK_systemIDSineFreqEnd, inputMessage.c_str());
      systemIDSineFreqEnd = inputMessage.toFloat();
    }
    if (request->hasParam("systemIDPlantTauMs")) {
      // Fitted plant tau from the dashboard Plant Delay sweep. Persisted with no field-off gate
      // (cheap compare-first scalar) so the actionable-disturbance readout survives reboots.
      foundParameter = true;
      inputMessage = request->getParam("systemIDPlantTauMs")->value();
      systemIDPlantTauMs = (uint16_t)inputMessage.toInt();
      settingWrite(NK_sysidPlantTau, String(systemIDPlantTauMs).c_str());
    }
    if (request->hasParam("fieldDecayTauMs")) {
      // Commissioned field drain time (ms) — worst-case value used when RPM is unknown and by the 3×
      // post-protection backstops. Writing it FLATTENS the drain-vs-RPM line (endpoints cleared), so a
      // manual override applies at every speed; the wizard Apply sends the endpoint params in the SAME
      // request (handled below, after this flatten) to commission a line. Clamped to a sane physical band.
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
    if (request->hasParam("fdDrainLoMs")) {
      foundParameter = true;
      int v = request->getParam("fdDrainLoMs")->value().toInt();
      fdDrainLoMs = (uint16_t)constrain(v, 0, 900);
      settingWrite(NK_fdDrainLoMs, String(fdDrainLoMs).c_str());
    }
    if (request->hasParam("fdDrainHiMs")) {
      foundParameter = true;
      int v = request->getParam("fdDrainHiMs")->value().toInt();
      fdDrainHiMs = (uint16_t)constrain(v, 0, 900);
      settingWrite(NK_fdDrainHiMs, String(fdDrainHiMs).c_str());
    }
    if (request->hasParam("fdDrainRpmLo")) {
      foundParameter = true;
      int v = request->getParam("fdDrainRpmLo")->value().toInt();
      fdDrainRpmLo = (uint16_t)constrain(v, 0, 10000);
      settingWrite(NK_fdDrainRpmLo, String(fdDrainRpmLo).c_str());
    }
    if (request->hasParam("fdDrainRpmHi")) {
      foundParameter = true;
      int v = request->getParam("fdDrainRpmHi")->value().toInt();
      fdDrainRpmHi = (uint16_t)constrain(v, 0, 10000);
      settingWrite(NK_fdDrainRpmHi, String(fdDrainRpmHi).c_str());
    }
    if (request->hasParam("systemIDSineCycles")) {
      foundParameter = true;
      inputMessage = request->getParam("systemIDSineCycles")->value();
      settingWrite(NK_systemIDSineCycles, inputMessage.c_str());
      systemIDSineCycles = (uint8_t)inputMessage.toInt();
    }
    if (request->hasParam("SystemIDStabilizeAmps")) {
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
                                 : (altSweepActive != 0) ? "Gate-tuning field sweep" : nullptr;
      if (sysMode == SYS_MODE_MANUAL) {
        queueConsoleMessage("SystemID: start blocked — not allowed in manual mode (duty is fixed; test cannot drive the field)");
      } else if (!sysidModeOK) {
        queueConsoleMessage("SystemID: start blocked — only allowed in AUTO mode (bulk, absorption, float, or target voltage)");
      } else if (activeTuning != nullptr) {
        queueConsoleMessageF("SystemID: start blocked — %s is active. Turn it off before running the Plant Delay Test.", activeTuning);
      } else if (systemIDActive == 0 && (millis() - systemIDLastEndMs) > 2000UL) {
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
    if (request->hasParam("cvPlantFitStart")) {
      foundParameter = true;
      float diMaxReq = request->hasParam("diMax") ? request->getParam("diMax")->value().toFloat() : 0.0f;
      if (!cvpfStartTest(diMaxReq)) queueConsoleMessageF("CV plant-fit: cannot start — %s", cvpfAbortMsg);
    }
    if (request->hasParam("cvPlantFitCancel")) {
      foundParameter = true;
      if (cvpfState == 1) cvpfAbort("cancelled by user");
    }

    // ── CV stress test (commissioning stage 8 / standalone from Tuning ▸ Stress Test) ──
    if (request->hasParam("CvStressDropV")) {
      foundParameter = true;
      inputMessage = request->getParam("CvStressDropV")->value();
      // floor: a non-positive drop parks the CV target AT/ABOVE idle — unreachable, inverts the test's "guaranteed reachable" premise
      CvStressDropV = clamp_f(inputMessage.toFloat(), 0.05f, 1.0f);
      settingWrite(NK_CvStressDropV, String(CvStressDropV, 2).c_str());
      queueConsoleMessageF("CV stress test target headroom: %.2f V below settled idle", CvStressDropV);
    }
    if (request->hasParam("CvStressFailBandV")) {
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
                         : (altSweepActive != 0) ? "Gate-tuning field sweep" : nullptr;
      if (sysMode != SYS_MODE_AUTO) {
        queueConsoleMessage("Field curve: start blocked — only allowed in AUTO mode");
      } else if (busy != nullptr) {
        queueConsoleMessageF("Field curve: start blocked — %s is active", busy);
      } else if ((millis() - fieldCurveLastEndMs) > 2000UL) {
        fieldCurveOnsetMode = false;  // saturation sweep (SystemID amplitudes)
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
                         : (altSweepActive != 0) ? "Gate-tuning field sweep" : nullptr;
      if (sysMode != SYS_MODE_AUTO) {
        queueConsoleMessage("Field cut: start blocked — only allowed in AUTO mode");
      } else if (busy != nullptr) {
        queueConsoleMessageF("Field cut: start blocked — %s is active", busy);
      } else if ((millis() - fieldCutLastEndMs) > 2000UL) {
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

    // ── Protection Actuation Tests (Settings → Emergency & Troubleshooting) ──
    //   Value setters (Set buttons): store into the globals; Fire uses the stored values. Independent
    //   ifs so each Set form (which carries only its own param) is handled on its own.
    if (request->hasParam("protTestCutMs")) {
      protTestCutMs = (uint16_t)constrain(request->getParam("protTestCutMs")->value().toInt(), 20, 10000);
      foundParameter = true;
      queueConsoleMessageF("Protection test: cut duration = %u ms", protTestCutMs);
    }
    if (request->hasParam("protTestGapMs")) {
      protTestGapMs = (uint16_t)constrain(request->getParam("protTestGapMs")->value().toInt(), 100, 20000);
      foundParameter = true;
      queueConsoleMessageF("Protection test: gap = %u ms", protTestGapMs);
    }
    if (request->hasParam("protTestReps")) {
      protTestReps = (uint8_t)constrain(request->getParam("protTestReps")->value().toInt(), 1, 20);
      foundParameter = true;
      queueConsoleMessageF("Protection test: repeats = %u", protTestReps);
    }
    if (request->hasParam("protTestAmps")) {
      float ptAmps = request->getParam("protTestAmps")->value().toFloat();
      protTestCmdA = (ptAmps < 5.0f) ? 0.0f : constrain(ptAmps, 5.0f, 100.0f);  // 0 = auto-seed at fire, and the only way BACK to auto
      foundParameter = true;
      if (protTestCmdA == 0.0f) queueConsoleMessage("Protection test: test current = auto (picked at fire)");
      else queueConsoleMessageF("Protection test: test current = %.0f A", protTestCmdA);
    }
    //   Fire buttons: trigger a run using the stored values. mode 1=instant cut, 2=load-dump,
    //   3=graceful ramp, 4=ladder walk (all AUTO + live-field gated), 5=manual bench cut (Manual Field).
    if (request->hasParam("protTestStart")) {
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
                         : (altSweepActive != 0) ? "Gate-tuning field sweep" : nullptr;
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
        queueConsoleMessage("Protection test: start blocked — engine-running tests need AUTO mode (use Manual Bench Testing instead)");
      } else if (!chargingEnabled) {
        queueConsoleMessage("Protection test: start blocked — charging must be enabled (engine running, On/Off on)");
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
    // Reuses the field-curve ramp in onset-stop mode (stops at first current). Each completed
    // sweep is committed as an anchor; applyKneeCurve fits the Min% column across the anchors.
    // Deliberately NOT gated on kneeLearnEnable: the floor table it fills is always live (max
    // with the scalar MinDuty) — the learning toggle gates only the background learner.
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
                         : (altSweepActive != 0) ? "Gate-tuning field sweep" : nullptr;
      if (sysMode != SYS_MODE_AUTO) {
        queueConsoleMessage("Keep-alive onset: start blocked — only allowed in AUTO mode");
      } else if (busy != nullptr) {
        queueConsoleMessageF("Keep-alive onset: start blocked — %s is active", busy);
      } else if ((millis() - fieldCurveLastEndMs) > 2000UL) {
        fieldCurveOnsetMode = true;  // onset-stop sweep (tachometer keep-alive floor)
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
    // Buttons, not URL parameters typed by hand: this is operated while also managing a throttle.
    // The browser confirms the discard before pressing Record over an un-dumped session.
    // Each of these only RAISES a flag: the 192 KB is allocated, freed and state-changed on the
    // control loop (altLogService), so a press can never free the buffer while a row is being
    // written. Refusals and confirmations come back on the console a few ms later, exactly as the
    // field curve reports its own start.
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
      if (request->hasParam("rate")) altSweepRatePctS = request->getParam("rate")->value().toFloat();
      if (request->hasParam("from")) altSweepFromPct  = request->getParam("from")->value().toFloat();
      if (request->hasParam("to"))   altSweepToPct    = request->getParam("to")->value().toFloat();
      // Turnaround margins. 0 is legal and means "turn around on the limit itself" — the ramp then
      // relies on the hard-cut layer alone, which is the behaviour the margins exist to avoid, so it
      // is allowed but never the default. Upper clamps keep a fat-fingered entry from making the
      // span shorter than the 1% minimum and refusing every sweep.
      if (request->hasParam("marginA"))
        altSweepMarginA = constrain(request->getParam("marginA")->value().toFloat(), 0.0f, 100.0f);
      if (request->hasParam("marginV"))
        altSweepMarginV = constrain(request->getParam("marginV")->value().toFloat(),
                                    0.0f, 2.0f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f));
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
                         : (altSweepActive != 0) ? "Gate-tuning field sweep" : nullptr;
      if (sysMode != SYS_MODE_AUTO) {
        // Same rule as the field curve: the duty override lives in the AUTO control path, and the
        // bumpless resume restores AUTO state. In MANUAL the user's fixed duty owns the field —
        // which is the throttle-sweep condition anyway, and needs no sweeper.
        queueConsoleMessage("Field sweep: start blocked — only allowed in AUTO mode");
      } else if (busy != nullptr) {
        queueConsoleMessageF("Field sweep: start blocked — %s is active", busy);
      } else if ((millis() - altSweepLastEndMs) > 2000UL) {
        // Deliberately does NOT clear altSweepAbortRequested: after an external teardown
        // (protection cut / leaving AUTO) the tick's static phase is still mid-ramp, and only the
        // latch makes the next normal tick reset it. Clearing it here would let a Start pressed
        // before that tick resume the stale ramp as an instant field step. altSweep_tick consumes
        // a stale latch itself: the abort gate resets phase first, then phase 0 starts this request.
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
    if (request->hasParam("addKneeAnchor")) {
      foundParameter = true;
      if (kneeSweepKneeDuty <= 0.0f) {
        queueConsoleMessage("Keep-alive onset: no sweep result to commit");
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
    if (request->hasParam("faCommissionGate")) {
      foundParameter = true;
      faCommissionGate = (request->getParam("faCommissionGate")->value().toInt() != 0);
      queueConsoleMessageF("Commissioning matrix gate %s", faCommissionGate ? "RELAXED (Phase 2)" : "strict");
    }

    // ── RPM ripple table game-fill window (RPM_RIPPLE_TABLE_SPEC §2.2) ──
    // Arm = wipe + stamp + open the fold (deferred to Core 1); disarm = freeze + persist. The browser
    // sends ripGameIdleRpm and the resTest level BEFORE arming — the wipe stamps both into the session.
    if (request->hasParam("ripGameFill")) {
      foundParameter = true;
      bool on = (request->getParam("ripGameFill")->value().toInt() != 0);
      if (on) {
        ripTabPendingWipe = true;
      } else {
        ripGameFill = false;
        ripTabPendingWipe = false;  // cancel an arm that never executed
        ripTabPendingSave = true;
      }
      queueConsoleMessageF("RPM ripple sweep %s", on ? "started (table wiped)" : "ended (table frozen + saved)");
    }
    if (request->hasParam("ripGameIdleRpm")) {
      foundParameter = true;
      long v = request->getParam("ripGameIdleRpm")->value().toInt();
      ripIdleRpmStage = (uint16_t)((v < 0) ? 0 : (v > 4000) ? 4000 : v);
    }

    // ── Active 3-current resonance test (COMMISSIONING_SPEC §3.2): arm = clear the ring + collect ──
    if (request->hasParam("bcurRtest")) {
      foundParameter = true;
      bool on = (request->getParam("bcurRtest")->value().toInt() != 0);
      if (on) bcurRtestCount = 0;  // arm clears; windows append only when the §10 steadiness gates pass (both sensors)
      bcurRtestActive = on;
      queueConsoleMessageF("Resonance current test %s", on ? "ARMED (vary charge current)" : "stopped");
    }

    // ── Auto-commissioning: state machine ───────────────────────────────────
    if (request->hasParam("commissionStart")) {
      foundParameter = true;
      // Repeat click while a Start is already staging (double-click / second tab): first staging
      // wins — the wizard's /cxStartState poll resolves either way.
      if (cxStartPersistStep == 0) {
        // Resuming a live run (IN_PROGRESS with its origin snapshot still present — e.g. reopening the
        // wizard after a page reload or a mid-wizard reboot) must NOT re-baseline: the origin snapshot and
        // the protection-flag backup keep their values from the original Start, so a later Abort still
        // reverts to the true pre-commissioning tune. Any other Start (fresh, restart after abort/done,
        // re-run on a demoted state==1 device that has no snapshot) captures the current tune as origin.
        bool resumingRun = (commissionState == 1 && settingExists(NK_commissionSnap));
        if (!resumingRun) commissionProtBackup = testProtectionsEnabled;
        // Force protections ON for the run so the tuning-tab Protections sliders can't leak a stray "off"
        // into commissioning. Restored to the backup on done/abort.
        testProtectionsEnabled = true;
        // RAM-only staging of the origin snapshot + step baseline + state keys; the loop worker
        // retires the NVS writes one commit per pass so this handler never stalls the network task
        // (the old in-handler burst froze the SSE stream ~2 s). The wizard polls /cxStartState and
        // advances only when the worker reports IDLE — the HTTP 200 alone means accepted, not saved.
        cxStartPersistBegin(resumingRun);
        // Wipe the live onset-knee FIT SCRATCH so the Min% floor step (stage 7) starts its
        // sweeps clean. (This is only the in-session anchor/fit state — NOT the applied rpmMinDutyTable
        // floors, which are left intact. cxStartPersistBegin above already captured kneeFitA for the backup.)
        kneeAnchorN = 0;
        kneeFitResidPct = -1.0f; kneeFitWorstIdx = -1; kneeFitA = 0.0f; kneeFitC = 0.0f;
      }
    }
    // Mark one wizard stage complete (i = 0=Prep…8=Stress test). Sets its done bit and clears
    // any downstream stage it feeds (coupling: see commissionDependentsMask). Drives the ✓ marks.
    if (request->hasParam("commissionStageDone")) {
      foundParameter = true;
      int s = request->getParam("commissionStageDone")->value().toInt();
      commissionMarkStage(s);
      settingsDirty = true;
      queueConsoleMessageF("Commissioning: step %d complete", s + 1);
    }
    // Skip a step "for now": leave it outstanding (badge keeps nagging) but flag it hand-touched so the
    // Finish summary lists it for manual setup. Does not advance phase — the wizard moves on client-side.
    if (request->hasParam("commissionStageSkip")) {
      foundParameter = true;
      int s = request->getParam("commissionStageSkip")->value().toInt();
      commissionSkipStage(s);
      settingsDirty = true;
      queueConsoleMessageF("Commissioning: step %d skipped (set manually later)", s + 1);
    }
    // Mark a step done BY HAND (unmeasured): counts toward COMMISSIONED so the nag can stop, flagged manual.
    if (request->hasParam("commissionStageManual")) {
      foundParameter = true;
      int s = request->getParam("commissionStageManual")->value().toInt();
      commissionManualStage(s);
      settingsDirty = true;
      queueConsoleMessageF("Commissioning: step %d marked done manually", s + 1);
    }
    if (request->hasParam("commissionAbort")) {
      foundParameter = true;
      // One atomic cancel picks the branch — a separate "still pending?" read could be retired by the
      // worker before the cancel lands. Resume/none fall through: a resume's cancel only stops the
      // bookkeeping writes, the original snapshot is intact for the teardown below.
      if (cxStartPersistCancel() == CX_CANCEL_FRESH) {
        // A fresh Start still staging (state=1 not yet committed) never began: cancel back to the
        // exact pre-click state. The live-run teardown below would wrongly demote a previously-
        // commissioned device whose re-run never actually started.
        settingsDirty = true;
        queueConsoleMessage("Commissioning: start cancelled before the restore point was saved — nothing changed");
      } else if (commissionState != 1 && !settingExists(NK_commissionSnap)) {
        // No run in flight: a finished pass (commissionDone) drops BOTH the origin snapshot and the
        // pre-run record, so the teardown below would find neither, fall through to the legacy zeroing
        // and silently demote a COMMISSIONED device to NOT_COMMISSIONED — wiping every done/manual
        // mark — while reporting "settings left as they are". Abort is a no-op here. State 1 with no
        // snapshot still takes the teardown: that IS the legacy/older-firmware run the zeroing is for.
        queueConsoleMessage("Commissioning: nothing to abort — no run is in progress, the committed tune and its record are untouched");
      } else {
        faCommissionGate = false;
        // Abort is a teardown path too: committed cells from an aborted sweep are honest data captured
        // at level — freeze + persist, never discard (RPM_RIPPLE_TABLE_SPEC §2.2).
        if (ripGameFill || ripTabPendingWipe) { ripGameFill = false; ripTabPendingWipe = false; ripTabPendingSave = true; }
        testProtectionsEnabled = commissionProtBackup;  // restore the user's manual-tuning protection setting
        bool reverted = commissionRestore();  // revert every setting to the Phase-0 snapshot
        // Bookkeeping goes back to the pre-run record (a device that was COMMISSIONED before a targeted
        // redo is commissioned again — its tune just came back). No record (runs started on older
        // firmware, or Clear-and-restart on a committed device) → everything clears, as before.
        if (!commissionRestorePreRun()) {
          commissionSetState(0);                // NOT_COMMISSIONED
          commissionSetPhase(0);                // clear checklist progress
          commissionDoneMask = 0;               // revert also drops all per-stage completion
          commissionWriteDoneMask();
          commissionManualMask = 0;             // …and all hand-set flags
          commissionWriteManualMask();
        }
        settingRemove(NK_commissionStepSnap); // teardown: no interrupted step to revert on next boot
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
      commissionClearMinPctBackup();        // discard the Min% backup too — the new floors stay
      commissionRecomputeState();           // COMMISSIONED if every required stage done (stress test optional), else IN_PROGRESS (partial)
      commissionSetPhase(COMMISSION_STAGE_COUNT);  // wizard pass finished (= one past the last step)
      // Browser wall clock, not the device soft clock (which can be unset at sea). A garbage stamp is
      // rejected rather than written, because a far-future epoch would silence the age prompt forever.
      if (request->hasParam("cmEpoch")) {
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
    // Wizard heartbeat: persist the current wizard phase so the tab checklist survives
    // a page reload / a different client. Clamped 0..COMMISSION_STAGE_COUNT (last value = finished).
    // Does not change commissionState.
    if (request->hasParam("commissionPhase")) {
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
      }
      settingsDirty = true;
    }
    // Commissioning idle-rest heartbeat. The open wizard pings =1 every ~2 s to keep the field "rested"
    // at a low duty between steps; on a clean close it pings =0 to drop the hold and resume charging
    // immediately. A missing ping (crash / Wi-Fi drop) goes stale on its own after the firmware timeout.
    if (request->hasParam("commissionHeartbeat")) {
      foundParameter = true;
      lastCommissionHeartbeatMs = (request->getParam("commissionHeartbeat")->value().toInt() != 0)
                                  ? millis() : 0;   // 0 = explicit exit → stale now → charging resumes
    }

    if (request->hasParam("TemperatureLimitF")) {
      foundParameter = true;
      inputMessage = request->getParam("TemperatureLimitF")->value();
      settingWrite(NK_TemperatureLimitF, inputMessage.c_str());
      TemperatureLimitF = inputMessage.toInt();
    }
    // Cold-charge lockout (lithium protection) — master on/off. Turning it OFF is the user "override".
    if (request->hasParam("coldChargeLockoutEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("coldChargeLockoutEnable")->value();
      coldChargeLockoutEnable = inputMessage.toInt() != 0;
      settingWrite(NK_coldChargeLockoutEnable, String((int)coldChargeLockoutEnable).c_str());
      queueConsoleMessageF("Cold-charge lockout (board-temp battery proxy): %s", coldChargeLockoutEnable ? "ENABLED" : "DISABLED");
    }
    // Cold-charge lockout temperature floor (board temp °F, stored raw)
    if (request->hasParam("MinChargeTempF")) {
      foundParameter = true;
      inputMessage = request->getParam("MinChargeTempF")->value();
      settingWrite(NK_MinChargeTempF, inputMessage.c_str());
      MinChargeTempF = inputMessage.toFloat();
    }
    // Battery + extra temperature probes, battery-temperature source chain, hot-charge lockout
    // (BATTERY_TEMP_SENSORS_SPEC.md §6). Temperatures arrive in °F (the UI converts).
    if (request->hasParam("battTempProbeEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("battTempProbeEnable")->value();
      battTempProbeEnable = (inputMessage.toInt() != 0) ? 1 : 0;
      settingWrite(NK_battTempProbeEnable, String(battTempProbeEnable).c_str());
    }
    if (request->hasParam("extraTempProbeEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("extraTempProbeEnable")->value();
      extraTempProbeEnable = (inputMessage.toInt() != 0) ? 1 : 0;
      settingWrite(NK_extraTempProbeEnable, String(extraTempProbeEnable).c_str());
    }
    if (request->hasParam("battTempSource")) {
      foundParameter = true;
      inputMessage = request->getParam("battTempSource")->value();
      battTempSource = constrain(inputMessage.toInt(), 0, 6);  // 0 Auto .. 5 Board, 6 None
      settingWrite(NK_battTempSource, String(battTempSource).c_str());
    }
    if (request->hasParam("battTempProxyEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("battTempProxyEnable")->value();
      battTempProxyEnable = (inputMessage.toInt() != 0) ? 1 : 0;
      settingWrite(NK_battTempProxyEnable, String(battTempProxyEnable).c_str());
    }
    // Hot-charge lockout — mirror of the cold one; acts only on a measured battery temperature (source 1..4).
    if (request->hasParam("hotChargeLockoutEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("hotChargeLockoutEnable")->value();
      hotChargeLockoutEnable = (inputMessage.toInt() != 0) ? 1 : 0;
      settingWrite(NK_hotChargeLockoutEnable, String(hotChargeLockoutEnable).c_str());
      queueConsoleMessageF("Hot-charge lockout (measured battery temperature): %s", hotChargeLockoutEnable ? "ENABLED" : "DISABLED");
    }
    if (request->hasParam("MaxChargeTempF")) {
      foundParameter = true;
      inputMessage = request->getParam("MaxChargeTempF")->value();
      settingWrite(NK_MaxChargeTempF, inputMessage.c_str());
      MaxChargeTempF = inputMessage.toFloat();
    }
    if (request->hasParam("extraTempAlarmHiEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("extraTempAlarmHiEnable")->value();
      extraTempAlarmHiEnable = (inputMessage.toInt() != 0) ? 1 : 0;
      settingWrite(NK_extraTempAlarmHiEnable, String(extraTempAlarmHiEnable).c_str());
    }
    if (request->hasParam("extraTempAlarmHiF")) {
      foundParameter = true;
      inputMessage = request->getParam("extraTempAlarmHiF")->value();
      settingWrite(NK_extraTempAlarmHiF, inputMessage.c_str());
      extraTempAlarmHiF = inputMessage.toFloat();
    }
    if (request->hasParam("extraTempAlarmLoEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("extraTempAlarmLoEnable")->value();
      extraTempAlarmLoEnable = (inputMessage.toInt() != 0) ? 1 : 0;
      settingWrite(NK_extraTempAlarmLoEnable, String(extraTempAlarmLoEnable).c_str());
    }
    if (request->hasParam("extraTempAlarmLoF")) {
      foundParameter = true;
      inputMessage = request->getParam("extraTempAlarmLoF")->value();
      settingWrite(NK_extraTempAlarmLoF, inputMessage.c_str());
      extraTempAlarmLoF = inputMessage.toFloat();
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
    if (request->hasParam("faEnabled")) {
      foundParameter = true;
      faEnabled = (request->getParam("faEnabled")->value().toInt() != 0);
      settingWrite(NK_faEnabled, faEnabled ? "1" : "0");
    }
    if (request->hasParam("faAlarmEnable")) {
      foundParameter = true;
      faAlarmEnable = (request->getParam("faAlarmEnable")->value().toInt() != 0);
      settingWrite(NK_faAlarmEnable, faAlarmEnable ? "1" : "0");
    }
    if (request->hasParam("faAnomPause")) {
      foundParameter = true;
      faAnomPause = (request->getParam("faAnomPause")->value().toInt() != 0);
      settingWrite(NK_faAnomPause, faAnomPause ? "1" : "0");
    }
    if (request->hasParam("faRpmEdgeMargin")) {
      foundParameter = true;
      faRpmEdgeMargin = request->getParam("faRpmEdgeMargin")->value().toFloat();
      settingWrite(NK_faRpmEdgeMargin, String(faRpmEdgeMargin, 1).c_str());
    }
    if (request->hasParam("faAmpsDriftFloorA")) {
      foundParameter = true;
      faAmpsDriftFloorA = request->getParam("faAmpsDriftFloorA")->value().toFloat();
      settingWrite(NK_faAmpsDriftFloorA, String(faAmpsDriftFloorA, 2).c_str());
    }
    if (request->hasParam("faAmpsDriftPct")) {
      foundParameter = true;
      faAmpsDriftPct = request->getParam("faAmpsDriftPct")->value().toFloat();
      settingWrite(NK_faAmpsDriftPct, String(faAmpsDriftPct, 1).c_str());
    }
    // Measured-ripple capture admission gates (§10.8/§11) — own knobs, decoupled from the fa* detector gates
    if (request->hasParam("ripWinMs")) {
      foundParameter = true;
      // Floor 500 ms: each HALF-window (§11 stationarity/min-of-halves) must hold ≥1 cycle of the lowest
      // resolvable disturbance; ceiling 4 s bounds capture latency. Also sizes the min-sample gate.
      float ripWinPrev = ripWinMs;
      ripWinMs = clamp_f(request->getParam("ripWinMs")->value().toFloat(), 500.0f, 4000.0f);
      settingWrite(NK_ripWinMs, String(ripWinMs, 0).c_str());
      // Window length defines BOTH measured quantities: the min-of-halves pk-pk and the worst
      // half-window voltage slope. Drop both fits (see ripFitForget).
      if (fabsf(ripWinMs - ripWinPrev) > 0.5f) ripFitForget(true, "Ripple capture window changed");
    }
    if (request->hasParam("ripDriftFloorA")) {
      foundParameter = true;
      ripDriftFloorA = clamp_f(request->getParam("ripDriftFloorA")->value().toFloat(), 0.0f, 50.0f);
      settingWrite(NK_ripDriftFloorA, String(ripDriftFloorA, 2).c_str());
    }
    if (request->hasParam("ripDriftPct")) {
      foundParameter = true;
      ripDriftPct = clamp_f(request->getParam("ripDriftPct")->value().toFloat(), 0.0f, 50.0f);
      settingWrite(NK_ripDriftPct, String(ripDriftPct, 1).c_str());
    }
    if (request->hasParam("faAttenUpAmps")) {
      foundParameter = true;
      faAttenUpAmps = request->getParam("faAttenUpAmps")->value().toFloat();
      settingWrite(NK_faAttenUpAmps, String(faAttenUpAmps, 1).c_str());
    }
    if (request->hasParam("faAttenDownAmps")) {
      foundParameter = true;
      faAttenDownAmps = request->getParam("faAttenDownAmps")->value().toFloat();
      settingWrite(NK_faAttenDownAmps, String(faAttenDownAmps, 1).c_str());
    }
    if (request->hasParam("faPeakMinA")) {
      foundParameter = true;
      faPeakMinA = request->getParam("faPeakMinA")->value().toFloat();
      settingWrite(NK_faPeakMinA, String(faPeakMinA, 2).c_str());
    }
    // ---- Auto Min% learning ("knee tracker") knobs ----
    if (request->hasParam("kneeLearnEnable")) {
      foundParameter = true;
      kneeLearnEnable = (request->getParam("kneeLearnEnable")->value().toInt() != 0);
      settingWrite(NK_kneeLearnEnable, kneeLearnEnable ? "1" : "0");
      if (kneeLearnEnable) {
        // Take ownership of the Min% column immediately from the learned floors (bin 0 always 0%,
        // undecided bins to the baseline) — same rule as boot and reset, one implementation.
        kneeRebuildOwnedTable();
      }
    }
    if (request->hasParam("kneeMarginPct")) {
      foundParameter = true;
      kneeMarginPct = request->getParam("kneeMarginPct")->value().toFloat();
      settingWrite(NK_kneeMarginPct, String(kneeMarginPct, 2).c_str());
    }
    if (request->hasParam("kneeOnsetA")) {
      foundParameter = true;
      kneeOnsetA = request->getParam("kneeOnsetA")->value().toFloat();
      settingWrite(NK_kneeOnsetA, String(kneeOnsetA, 2).c_str());
    }
    if (request->hasParam("kneeReArmA")) {
      foundParameter = true;
      kneeReArmA = request->getParam("kneeReArmA")->value().toFloat();
      settingWrite(NK_kneeReArmA, String(kneeReArmA, 2).c_str());
    }
    if (request->hasParam("kneeDwellSec")) {
      foundParameter = true;
      kneeDwellSec = request->getParam("kneeDwellSec")->value().toFloat();
      settingWrite(NK_kneeDwellSec, String(kneeDwellSec, 1).c_str());
    }
    if (request->hasParam("kneeStepPct")) {
      foundParameter = true;
      kneeStepPct = request->getParam("kneeStepPct")->value().toFloat();
      settingWrite(NK_kneeStepPct, String(kneeStepPct, 2).c_str());
    }
    if (request->hasParam("kneeTempComp")) {
      foundParameter = true;
      kneeTempComp = (request->getParam("kneeTempComp")->value().toInt() != 0);
      settingWrite(NK_kneeTempComp, kneeTempComp ? "1" : "0");
    }
    if (request->hasParam("kneeTempRefF")) {
      foundParameter = true;
      kneeTempRefF = request->getParam("kneeTempRefF")->value().toFloat();
      settingWrite(NK_kneeTempRefF, String(kneeTempRefF, 1).c_str());
    }
    if (request->hasParam("kneeMaxFloorPct")) {
      foundParameter = true;
      kneeMaxFloorPct = request->getParam("kneeMaxFloorPct")->value().toFloat();
      settingWrite(NK_kneeMaxFloorPct, String(kneeMaxFloorPct, 2).c_str());
    }
    if (request->hasParam("kneeRpmTolPct")) {
      foundParameter = true;
      kneeRpmTolPct = request->getParam("kneeRpmTolPct")->value().toFloat();
      settingWrite(NK_kneeRpmTolPct, String(kneeRpmTolPct, 1).c_str());
    }
    if (request->hasParam("kneeTempTolF")) {
      foundParameter = true;
      kneeTempTolF = request->getParam("kneeTempTolF")->value().toFloat();
      settingWrite(NK_kneeTempTolF, String(kneeTempTolF, 1).c_str());
    }
    if (request->hasParam("kneeDutyTolPct")) {
      foundParameter = true;
      kneeDutyTolPct = request->getParam("kneeDutyTolPct")->value().toFloat();
      settingWrite(NK_kneeDutyTolPct, String(kneeDutyTolPct, 2).c_str());
    }
    if (request->hasParam("ResetKneeLearn")) {
      foundParameter = true;
      kneeLearnResetDefaults();
    }
    // ---- Zero-drift characterization log (diagnostic) ----
    if (request->hasParam("ZeroLogEnable")) {
      foundParameter = true;
      ZeroLogEnable = (request->getParam("ZeroLogEnable")->value().toInt() != 0);
      settingWrite(NK_ZeroLogEnable, ZeroLogEnable ? "1" : "0");
    }
    if (request->hasParam("ResetZeroLog")) {
      foundParameter = true;
      zeroLogResetAll();
    }
    if (request->hasParam("wifiNapEnabled")) {
      foundParameter = true;
      wifiNapEnabled = (request->getParam("wifiNapEnabled")->value().toInt() != 0);
      settingWrite(NK_wifiNapEnabled, wifiNapEnabled ? "1" : "0");
    }
    if (request->hasParam("altSimMode")) {
      foundParameter = true;
      altSimMode = request->getParam("altSimMode")->value().toFloat();  // bench simulator (not persisted)
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
    if (request->hasParam("perfSimMode")) {
      foundParameter = true;
      perfSimMode = request->getParam("perfSimMode")->value().toFloat();  // bench NMEA simulator (not persisted)
    }
    // Boat-performance polar settings (registry-driven, incl. sticky Pause + speed-source selector)
    if (perfSettingsHandle(request)) {
      foundParameter = true;
      sendPerfSettings();
    }

    if (request->hasParam("ManualDutyTarget")) {
      foundParameter = true;
      inputMessage = request->getParam("ManualDutyTarget")->value();
      ManualDutyTarget = constrain(inputMessage.toFloat(), 0.0f, 100.0f);
      settingWrite(NK_ManualDutyTarget, String(ManualDutyTarget, 2).c_str());
    }
    if (request->hasParam("socInfoAvailable")) {
      foundParameter = true;
      inputMessage = request->getParam("socInfoAvailable")->value();
      settingWrite(NK_socInfoAvailable, inputMessage.c_str());
      socInfoAvailable = inputMessage.toInt();
    }
    if (request->hasParam("TailCurrent_A")) {
      foundParameter = true;
      inputMessage = request->getParam("TailCurrent_A")->value();
      settingWrite(NK_TailCurrent_A, inputMessage.c_str());
      TailCurrent_A = inputMessage.toFloat();
    }
    if (request->hasParam("RebulkVoltage")) {
      foundParameter = true;
      inputMessage = request->getParam("RebulkVoltage")->value();
      settingWrite(NK_RebulkVoltage, inputMessage.c_str());
      RebulkVoltage = inputMessage.toFloat();
    }
    if (request->hasParam("rebulkDebounceTime")) {
      foundParameter = true;
      inputMessage = request->getParam("rebulkDebounceTime")->value();
      rebulkDebounceTime = (uint32_t)inputMessage.toInt() * 1000UL;  // sec → ms
      // Save the ms value so boot load is consistent
      settingWrite(NK_rebulkDebounceTime, String(rebulkDebounceTime).c_str());
    }

    if (request->hasParam("MinFloatTime")) {
      foundParameter = true;
      inputMessage = request->getParam("MinFloatTime")->value();
      MinFloatTime = (uint32_t)inputMessage.toInt() * 60000UL;  // min → ms
      settingWrite(NK_MinFloatTime, String(MinFloatTime).c_str());
    }
    if (request->hasParam("SOC_BlockRebulk_percent")) {
      foundParameter = true;
      inputMessage = request->getParam("SOC_BlockRebulk_percent")->value();
      settingWrite(NK_SOC_BlockRebulk_percent, inputMessage.c_str());
      SOC_BlockRebulk_percent = inputMessage.toInt();
    }
    if (request->hasParam("SOC_AllowRebulk_percent")) {
      foundParameter = true;
      inputMessage = request->getParam("SOC_AllowRebulk_percent")->value();
      settingWrite(NK_SOC_AllowRebulk_percent, inputMessage.c_str());
      SOC_AllowRebulk_percent = inputMessage.toInt();
    }
    if (request->hasParam("BulkVoltage")) {
      foundParameter = true;
      inputMessage = request->getParam("BulkVoltage")->value();
      settingWrite(NK_BulkVoltage, inputMessage.c_str());
      BulkVoltage = inputMessage.toFloat();
    }
    // NOTE: system voltage (12/24/36/48V) is NOT a /get setting — it lives in Vessel Info and the whole
    // class-change rescale (charge profile, hard-shutdown, INA228 OV, normalized CV/CC gains) runs in
    // the /saveVesselInfo handler via applyNominalVoltageChange(). SYSTEM_VOLTAGE_CLASS is the sole source.
    if (request->hasParam("wavePeriod")) {
      foundParameter = true;
      inputMessage = request->getParam("wavePeriod")->value();
      settingWrite(NK_wavePeriod, inputMessage.c_str());
      wavePeriod = inputMessage.toInt();
      if (TuningMode) tuningParamChanged = true;
    }
    if (request->hasParam("tuningWaveform")) {
      foundParameter = true;
      inputMessage = request->getParam("tuningWaveform")->value();
      settingWrite(NK_tuningWaveform, inputMessage.c_str());
      tuningWaveform = inputMessage.toInt();
      if (TuningMode) tuningParamChanged = true;
    }
    if (request->hasParam("tuningSineFreq")) {
      foundParameter = true;
      inputMessage = request->getParam("tuningSineFreq")->value();
      settingWrite(NK_tuningSineFreq, inputMessage.c_str());
      tuningSineFreq = inputMessage.toFloat();
    }
    if (request->hasParam("tuningSweepStart")) {
      foundParameter = true;
      inputMessage = request->getParam("tuningSweepStart")->value();
      settingWrite(NK_tuningSweepStart, inputMessage.c_str());
      tuningSweepStart = inputMessage.toFloat();
    }
    if (request->hasParam("tuningSweepEnd")) {
      foundParameter = true;
      inputMessage = request->getParam("tuningSweepEnd")->value();
      settingWrite(NK_tuningSweepEnd, inputMessage.c_str());
      tuningSweepEnd = inputMessage.toFloat();
    }
    if (request->hasParam("tuningSweepCycles")) {
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
        tuningSweepRequested = true;   // momentary — the TuningMode sine block consumes it
      }
    }
    if (request->hasParam("SwitchingFrequency")) {
      foundParameter = true;
      inputMessage = request->getParam("SwitchingFrequency")->value();
      // LEDC 12-bit ceiling is 19455Hz (clock divider must exceed 1.0) — higher values are rejected
      // by the driver and, once in NVS, kill the boot-time PWM attach (bench-confirmed 2026-08-16).
      int requestedFreq = constrain(inputMessage.toInt(), 100, 19455);
      settingWrite(NK_SwitchingFrequency, String(requestedFreq).c_str());
      SwitchingFrequency = requestedFreq;
      queueConsoleMessageF("Frequency target set to %dHz", requestedFreq);
    }
    if (request->hasParam("FloatVoltage")) {
      foundParameter = true;
      inputMessage = request->getParam("FloatVoltage")->value();
      settingWrite(NK_FloatVoltage, inputMessage.c_str());
      FloatVoltage = inputMessage.toFloat();
    }
    if (request->hasParam("yyMin")) {
      foundParameter = true;
      inputMessage = request->getParam("yyMin")->value();
      settingWrite(NK_yyMin, inputMessage.c_str());
      yyMin = inputMessage.toInt();
    }
    if (request->hasParam("ManualFieldToggle")) {
      foundParameter = true;
      inputMessage = request->getParam("ManualFieldToggle")->value();
      settingWrite(NK_ManualFieldToggle, inputMessage.c_str());
      ManualFieldToggle = inputMessage.toInt();
    }
    if (request->hasParam("capLimitMode")) {
      foundParameter = true;
      inputMessage = request->getParam("capLimitMode")->value();
      settingWrite(NK_capLimitMode, inputMessage.c_str());
      capLimitMode = constrain(inputMessage.toInt(), 0, 1);
    }
    if (request->hasParam("PhysicalPanelOverride")) {
      foundParameter = true;
      inputMessage = request->getParam("PhysicalPanelOverride")->value();
      PhysicalPanelOverride = (inputMessage.toInt() == 1) ? 1 : 0;  // clamped: the resolvers test == 0 and the app gates on === 1, so any other value would split them
      settingWrite(NK_PhysicalPanelOverride, String(PhysicalPanelOverride).c_str());
      // Ownership of both modes just moved. Re-resolve here rather than waiting for the loop pass, so
      // the CSV3 echo this request triggers already carries the modes the new owner asks for — turning
      // the override off has to put the app's stored choice back on screen in the same round trip.
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
    }
    if (request->hasParam("MaintainMode")) {
      foundParameter = true;
      inputMessage = request->getParam("MaintainMode")->value();
      MaintainModeUserSel = inputMessage.toInt();  // the app's stored choice; inert while PhysicalPanelOverride is on, but still recorded so turning the override back off restores it
      if (!BatteryShuntPresent) MaintainModeUserSel = 0;  // no battery-current sensor at all → 0-net-amps hold impossible
      settingWrite(NK_MaintainMode, String(MaintainModeUserSel).c_str());
      if (MaintainModeUserSel && PhysicalPanelOverride == 0) {
        // MaintainMode and TargetVoltageMode are mutually exclusive — clear the other. Only while the app
        // owns the mode: with the panel override on this choice is inert, so clearing Target Voltage for
        // it would cost the user that mode for nothing.
        TargetVoltageMode = 0;
        settingWrite(NK_TargetVoltageMode, "0");
      }
      MaintainMode = resolveMaintainMode();  // effective value: this choice only lands while the override is off
      if (PhysicalPanelOverride) queueConsoleMessageF("MaintainMode choice stored (%s) - the Cable 3 Force Float switch decides while Physical Panel Override is on", MaintainModeUserSel ? "on" : "off");
      else queueConsoleMessageF("MaintainMode mode %s", MaintainMode ? "enabled" : "disabled");
    }
    if (request->hasParam("TargetVoltageMode")) {
      foundParameter = true;
      inputMessage = request->getParam("TargetVoltageMode")->value();
      TargetVoltageMode = inputMessage.toInt();
      settingWrite(NK_TargetVoltageMode, inputMessage.c_str());
      if (TargetVoltageMode) {
        // MaintainMode and TargetVoltageMode are mutually exclusive — clear the other.
        MaintainMode = 0;
        MaintainModeUserSel = 0;  // clear the stored choice too, or switching Target Voltage back off would silently resurrect Maintain
        settingWrite(NK_MaintainMode, "0");
      }
      queueConsoleMessageF("TargetVoltageMode %s", TargetVoltageMode ? "enabled" : "disabled");
    }
    if (request->hasParam("OnOff")) {
      // NOTE: OnOff==0 already caused an early return above, so this
      // branch only runs for OnOff==1 (or any non-zero value) after
      // the arm check has passed.
      foundParameter = true;
      inputMessage = request->getParam("OnOff")->value();
      settingWrite(NK_OnOff, inputMessage.c_str());
      OnOff = inputMessage.toInt();
    }
    if (request->hasParam("HiLow")) {
      foundParameter = true;
      inputMessage = request->getParam("HiLow")->value();
      HiLowUserSel = inputMessage.toInt();  // the app's stored choice; inert while PhysicalPanelOverride is on, but still recorded so turning the override back off restores it
      settingWrite(NK_HiLow, inputMessage.c_str());
      // Applied here as well as in servicePanelSwitchInputs() so the mode is live before this request
      // returns; the loop-side call then finds nothing left to do. applyChargeRateMode() (6_functions.ino)
      // carries the table swap, the thermal-loop note and the Hi->Lo ceiling glide. With the override on
      // this resolves back to the panel's mode and the request changes nothing but the stored choice.
      if (applyChargeRateMode(resolveChargeRateMode())) {
        queueConsoleMessageF("Charge rate mode: switched to %s", HiLow == 1 ? "Normal" : "Low");
      } else if (PhysicalPanelOverride) {
        queueConsoleMessageF("Charge rate choice stored (%s) - the Cable 3 switch decides while Physical Panel Override is on", HiLowUserSel == 0 ? "Low" : "Normal");
      }
    }
    if (request->hasParam("InvertAltAmps")) {
      foundParameter = true;
      inputMessage = request->getParam("InvertAltAmps")->value();
      settingWrite(NK_InvertAltAmps, inputMessage.c_str());
      InvertAltAmps = inputMessage.toInt();
    }
    if (request->hasParam("InvertBattAmps")) {
      foundParameter = true;
      inputMessage = request->getParam("InvertBattAmps")->value();
      settingWrite(NK_InvertBattAmps, inputMessage.c_str());
      InvertBattAmps = inputMessage.toInt();
    }
    if (request->hasParam("BatteryShuntPresent")) {
      foundParameter = true;
      inputMessage = request->getParam("BatteryShuntPresent")->value();
      settingWrite(NK_BatteryShuntPresent, inputMessage.c_str());
      BatteryShuntPresent = inputMessage.toInt();
      // Persist the force-off ONLY for the sticky statement "no shunt". A shunt declared present but with no
      // resistance entered is a recoverable calibration gap: every Bcur consumer already gates on
      // HAS_BATT_SHUNT, so suppress at runtime and leave the user's Float/Maintain choice in NVS.
      if (!BatteryShuntPresent) {
        if (UseFloat != 0)     { UseFloat = 0;     settingWrite(NK_UseFloat, "0"); }
        if (MaintainMode != 0 || MaintainModeUserSel != 0) { MaintainMode = 0; MaintainModeUserSel = 0; settingWrite(NK_MaintainMode, "0"); }
        queueConsoleMessage("Battery shunt marked absent: State of Charge, battery health, battery current limit and float charging are off.");
        queueConsoleMessage("Load-dump detection is off too. The alternator current limit now protects the battery.");
      } else if (!HAS_BATT_SHUNT) {
        queueConsoleMessage("Battery shunt resistance is not set: State of Charge, battery health, battery current limit and float charging are off.");
        queueConsoleMessage("Enter the shunt resistance to enable them. Your float setting is kept.");
      }
    }
    if (request->hasParam("MaxDuty")) {
      foundParameter = true;
      inputMessage = request->getParam("MaxDuty")->value();
      settingWrite(NK_MaxDuty, inputMessage.c_str());
      MaxDuty = inputMessage.toInt();
      if (pidInitialized) {
        applyCcOutputLimits();  // one owner for the ceiling expression — a raw SetOutputLimits here silently diverges the moment ccDutyCeiling() stops being plain MaxDuty
      }
      queueConsoleMessageF("Max Duty updated to: %d%%", MaxDuty);
    }
    if (request->hasParam("MaxFieldVolts")) {
      foundParameter = true;
      inputMessage = request->getParam("MaxFieldVolts")->value();
      MaxFieldVolts = constrain(inputMessage.toFloat(), 0.5f, 60.0f);
      settingWrite(NK_MaxFieldVolts, String(MaxFieldVolts, 1).c_str());
      // Force an immediate re-solve rather than waiting for the tick filter to drift the derived
      // ceiling past its 0.5-point deadband: a large dtSec collapses the bus filter onto the present
      // reading, so the new cap is enforced on the next duty write, not up to 2s later.
      updateFieldVoltCeiling(getBatteryVoltage(), 999.0f);
      queueConsoleMessageF("Max Field Volts updated to: %.1fV (field ceiling now %.1f%%)", MaxFieldVolts, ccDutyCeiling());
    }
    if (request->hasParam("MinDuty")) {
      foundParameter = true;
      inputMessage = request->getParam("MinDuty")->value();
      settingWrite(NK_MinDuty, inputMessage.c_str());
      MinDuty = inputMessage.toFloat();
      if (pidInitialized) {
        applyCcOutputLimits();
      }
      queueConsoleMessageF("Min Field (tachometer keep-alive) updated to: %.2f%%", MinDuty);
    }
    if (request->hasParam("LimpHome")) {
      foundParameter = true;
      inputMessage = request->getParam("LimpHome")->value();
      settingWrite(NK_LimpHome, inputMessage.c_str());
      LimpHome = inputMessage.toInt();
    }
    if (request->hasParam("VeData")) {
      foundParameter = true;
      inputMessage = request->getParam("VeData")->value();
      settingWrite(NK_VeData, inputMessage.c_str());
      VeData = inputMessage.toInt();
    }
    if (request->hasParam("NMEA0183Data")) {
      foundParameter = true;
      inputMessage = request->getParam("NMEA0183Data")->value();
      settingWrite(NK_NMEA0183Data, inputMessage.c_str());
      NMEA0183Data = inputMessage.toInt();
      if (NMEA0183Data != 1) n183ClearHeading();  // receiver stopped: nothing will ever age this value out again
    }
    if (request->hasParam("NMEA0183Baud")) {
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
    if (request->hasParam("NMEA0183Invert")) {
      foundParameter = true;
      inputMessage = request->getParam("NMEA0183Invert")->value();
      NMEA0183Invert = (inputMessage.toInt() == 1) ? 1 : 0;
      settingWrite(NK_NMEA0183Invert, String(NMEA0183Invert).c_str());
      n183ReconfigPending = true;  // Core 1 applies it; see the flag's declaration
      queueConsoleMessageF("NMEA 0183 polarity set to %s", NMEA0183Invert ? "inverted (TTL talker)" : "normal (RS-232 talker)");
    }
    if (request->hasParam("NMEA2KData")) {
      foundParameter = true;
      inputMessage = request->getParam("NMEA2KData")->value();
      settingWrite(NK_NMEA2KData, inputMessage.c_str());
      NMEA2KData = inputMessage.toInt();
    }
    if (request->hasParam("n2kTxEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kTxEnable")->value();
      settingWrite(NK_n2kTxEn, inputMessage.c_str());
      int newVal = inputMessage.toInt();
      if (newVal != n2kTxEnable) queueConsoleMessage("NMEA2000 transmit: bus mode is set at boot — reboot to apply");
      n2kTxEnable = newVal;  // per-PGN toggles/instances below apply live; only the node/listen mode itself is boot-time
    }
    if (request->hasParam("n2kDeviceInstance")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kDeviceInstance")->value();
      n2kDeviceInstance = constrain(inputMessage.toInt(), 0, 252);
      settingWrite(NK_n2kDevInst, String(n2kDeviceInstance).c_str());
      queueConsoleMessage("NMEA2000 device instance: applied at boot — reboot to take effect on the bus");
    }
    if (request->hasParam("n2kBattEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kBattEnable")->value();
      settingWrite(NK_n2kBattEn, inputMessage.c_str());
      n2kBattEnable = inputMessage.toInt();
    }
    if (request->hasParam("n2kBattInstance")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kBattInstance")->value();
      n2kBattInstance = constrain(inputMessage.toInt(), 0, 252);
      settingWrite(NK_n2kBattInst, String(n2kBattInstance).c_str());
    }
    if (request->hasParam("n2kBattCfgEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kBattCfgEnable")->value();
      settingWrite(NK_n2kBattCfgEn, inputMessage.c_str());
      n2kBattCfgEnable = inputMessage.toInt();
    }
    if (request->hasParam("n2kAltEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kAltEnable")->value();
      settingWrite(NK_n2kAltEn, inputMessage.c_str());
      n2kAltEnable = inputMessage.toInt();
    }
    if (request->hasParam("n2kAltInstance")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kAltInstance")->value();
      n2kAltInstance = constrain(inputMessage.toInt(), 0, 252);
      settingWrite(NK_n2kAltInst, String(n2kAltInstance).c_str());
    }
    if (request->hasParam("n2kAltTempEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kAltTempEnable")->value();
      settingWrite(NK_n2kAltTempEn, inputMessage.c_str());
      n2kAltTempEnable = inputMessage.toInt();
    }
    if (request->hasParam("n2kTempInstance")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kTempInstance")->value();
      n2kTempInstance = constrain(inputMessage.toInt(), 0, 252);
      settingWrite(NK_n2kTempInst, String(n2kTempInstance).c_str());
    }
    if (request->hasParam("n2kTempSource")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kTempSource")->value();
      n2kTempSource = constrain(inputMessage.toInt(), 0, 15);  // tN2kTempSource 4-bit field
      settingWrite(NK_n2kTempSrc, String(n2kTempSource).c_str());
    }
    if (request->hasParam("n2kExtraTempEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kExtraTempEnable")->value();
      settingWrite(NK_n2kExtraTempEnable, inputMessage.c_str());
      n2kExtraTempEnable = inputMessage.toInt();
    }
    if (request->hasParam("n2kExtraTempInstance")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kExtraTempInstance")->value();
      n2kExtraTempInstance = constrain(inputMessage.toInt(), 0, 252);
      settingWrite(NK_n2kExtraTempInstance, String(n2kExtraTempInstance).c_str());
    }
    if (request->hasParam("n2kExtraTempSource")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kExtraTempSource")->value();
      n2kExtraTempSource = constrain(inputMessage.toInt(), 0, 15);  // tN2kTempSource 4-bit field
      settingWrite(NK_n2kExtraTempSource, String(n2kExtraTempSource).c_str());
    }
    if (request->hasParam("n2kChgrEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kChgrEnable")->value();
      settingWrite(NK_n2kChgrEn, inputMessage.c_str());
      n2kChgrEnable = inputMessage.toInt();
    }
    if (request->hasParam("n2kChgrInstance")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kChgrInstance")->value();
      n2kChgrInstance = constrain(inputMessage.toInt(), 0, 252);
      settingWrite(NK_n2kChgrInst, String(n2kChgrInstance).c_str());
    }
    if (request->hasParam("n2kChgrCfgEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kChgrCfgEnable")->value();
      settingWrite(NK_n2kChgrCfgEn, inputMessage.c_str());
      n2kChgrCfgEnable = inputMessage.toInt();
    }
    if (request->hasParam("n2kChgrMode")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kChgrMode")->value();
      n2kChgrMode = constrain(inputMessage.toInt(), 0, 2);  // tN2kChargerMode: Standalone/Primary/Secondary
      settingWrite(NK_n2kChgrMode, String(n2kChgrMode).c_str());
    }
    if (request->hasParam("n2kEngRpmEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kEngRpmEnable")->value();
      settingWrite(NK_n2kEngRpmEn, inputMessage.c_str());
      n2kEngRpmEnable = inputMessage.toInt();
    }
    if (request->hasParam("n2kEngInstance")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kEngInstance")->value();
      n2kEngInstance = constrain(inputMessage.toInt(), 0, 252);
      settingWrite(NK_n2kEngInst, String(n2kEngInstance).c_str());
    }
    if (request->hasParam("n2kEngDynEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kEngDynEnable")->value();
      settingWrite(NK_n2kEngDynEn, inputMessage.c_str());
      n2kEngDynEnable = inputMessage.toInt();
    }
    if (request->hasParam("n2kEngBitsEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kEngBitsEnable")->value();
      settingWrite(NK_n2kEngBitsEn, inputMessage.c_str());
      n2kEngBitsEnable = inputMessage.toInt();
    }
    if (request->hasParam("n2kRxBattInstance")) {
      foundParameter = true;
      inputMessage = request->getParam("n2kRxBattInstance")->value();
      n2kRxBattInstance = constrain(inputMessage.toInt(), 0, 252);
      settingWrite(NK_n2kRxBattInst, String(n2kRxBattInstance).c_str());
      n2kRxBattV = n2kRxBattA = n2kRxBattTempF = NAN;  // clear the old bank's values; SOC too
      n2kRxSoc = n2kRxSoh = -1;
      dataTimestamps[IDX_N2K_BATT] = 0;
      dataTimestamps[IDX_N2K_SOC] = 0;
    }
    if (request->hasParam("rvcTxEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("rvcTxEnable")->value();
      int newVal = inputMessage.toInt();
      settingWrite(NK_rvcTxEn, inputMessage.c_str());
      // Same boot-time constraint as n2kTxEnable: becoming a bus node (address claim) happens once
      // in initializeHardware. The per-DGN toggles and instances below apply live.
      if (newVal != rvcTxEnable && n2kTxEnable != 1) queueConsoleMessage("RV-C transmit: bus mode is set at boot — reboot to apply");
      rvcTxEnable = newVal;
    }
    if (request->hasParam("rvcChgrEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("rvcChgrEnable")->value();
      settingWrite(NK_rvcChgrEn, inputMessage.c_str());
      rvcChgrEnable = inputMessage.toInt();
    }
    if (request->hasParam("rvcDcEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("rvcDcEnable")->value();
      settingWrite(NK_rvcDcEn, inputMessage.c_str());
      rvcDcEnable = inputMessage.toInt();
    }
    if (request->hasParam("rvcFaultEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("rvcFaultEnable")->value();
      settingWrite(NK_rvcFaultEn, inputMessage.c_str());
      rvcFaultEnable = inputMessage.toInt();
    }
    if (request->hasParam("rvcChgrInstance")) {
      foundParameter = true;
      inputMessage = request->getParam("rvcChgrInstance")->value();
      rvcChgrInstance = constrain(inputMessage.toInt(), 1, 250);  // spec 6.20.8b: 0 is invalid
      settingWrite(NK_rvcChgrInst, String(rvcChgrInstance).c_str());
    }
    if (request->hasParam("rvcDcInstance")) {
      foundParameter = true;
      inputMessage = request->getParam("rvcDcInstance")->value();
      rvcDcInstance = constrain(inputMessage.toInt(), 1, 250);  // spec 6.5.2b: 0 is invalid
      settingWrite(NK_rvcDcInst, String(rvcDcInstance).c_str());
    }
    if (request->hasParam("rvcDevPriority")) {
      foundParameter = true;
      inputMessage = request->getParam("rvcDevPriority")->value();
      rvcDevPriority = constrain(inputMessage.toInt(), 0, 250);
      settingWrite(NK_rvcDevPri, String(rvcDevPriority).c_str());
    }
    if (request->hasParam("dvccEn")) {
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
    if (request->hasParam("dvccSrcType")) {
      foundParameter = true;
      inputMessage = request->getParam("dvccSrcType")->value();
      dvccSrcType = constrain(inputMessage.toInt(), 0, 1);
      settingWrite(NK_dvccSrcType, String(dvccSrcType).c_str());
      dvccResetAuthority();
    }
    if (request->hasParam("dvccInst")) {
      foundParameter = true;
      inputMessage = request->getParam("dvccInst")->value();
      dvccInst = constrain(inputMessage.toInt(), 0, 250);  // RV-C DC instance range; 0 = any
      settingWrite(NK_dvccInst, String(dvccInst).c_str());
      dvccResetAuthority();
    }
    if (request->hasParam("dvccSilenceS")) {
      foundParameter = true;
      inputMessage = request->getParam("dvccSilenceS")->value();
      dvccSilenceS = constrain(inputMessage.toInt(), 5, 600);
      settingWrite(NK_dvccSilenceS, String(dvccSilenceS).c_str());
    }
    if (request->hasParam("dvccSettleS")) {
      foundParameter = true;
      inputMessage = request->getParam("dvccSettleS")->value();
      dvccSettleS = constrain(inputMessage.toInt(), 5, 600);
      settingWrite(NK_dvccSettleS, String(dvccSettleS).c_str());
    }
    if (request->hasParam("dvccCvlMin")) {
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
    if (request->hasParam("dvccCvlMax")) {
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
    if (request->hasParam("waveAmplitude")) {
      foundParameter = true;
      inputMessage = request->getParam("waveAmplitude")->value();
      settingWrite(NK_waveAmplitude, inputMessage.c_str());
      waveAmplitude = inputMessage.toInt();
      if (TuningMode) tuningParamChanged = true;
    }
    if (request->hasParam("tuningWaveFloor")) {
      foundParameter = true;
      inputMessage = request->getParam("tuningWaveFloor")->value();
      settingWrite(NK_tuningWaveFloor, inputMessage.c_str());
      tuningWaveFloor = inputMessage.toInt();
      if (TuningMode) tuningParamChanged = true;
    }
    if (request->hasParam("CurrentThreshold")) {
      foundParameter = true;
      inputMessage = request->getParam("CurrentThreshold")->value();
      settingWrite(NK_CurrentThreshold, inputMessage.c_str());
      CurrentThreshold = inputMessage.toFloat();
    }
    if (request->hasParam("PeukertExponent")) {
      foundParameter = true;
      inputMessage = request->getParam("PeukertExponent")->value();
      PeukertExponent_scaled = (int)(inputMessage.toFloat() * 100);
      settingWrite(NK_PeukertExponent, String(PeukertExponent_scaled).c_str());
    }
    if (request->hasParam("ChargeEfficiency")) {
      foundParameter = true;
      inputMessage = request->getParam("ChargeEfficiency")->value();
      settingWrite(NK_ChargeEfficiency, inputMessage.c_str());
      ChargeEfficiency_scaled = (int)round(inputMessage.toFloat() * 10);  // store as % × 10
    }
    if (request->hasParam("ChargedVoltage")) {
      foundParameter = true;
      inputMessage = request->getParam("ChargedVoltage")->value();
      ChargedVoltage_Scaled = (int)(inputMessage.toFloat() * 100);
      settingWrite(NK_ChargedVoltage, String(ChargedVoltage_Scaled).c_str());
    }
    if (request->hasParam("TailCurrent")) {
      foundParameter = true;
      inputMessage = request->getParam("TailCurrent")->value();
      settingWrite(NK_TailCurrent, inputMessage.c_str());
      TailCurrent = inputMessage.toFloat();
    }
    if (request->hasParam("ChargedDetectionTime")) {
      foundParameter = true;
      inputMessage = request->getParam("ChargedDetectionTime")->value();
      settingWrite(NK_ChargedDetectionTime, inputMessage.c_str());
      ChargedDetectionTime = inputMessage.toInt();
    }
    if (request->hasParam("IgnoreTemperature")) {
      foundParameter = true;
      inputMessage = request->getParam("IgnoreTemperature")->value();
      settingWrite(NK_IgnoreTemperature, inputMessage.c_str());
      IgnoreTemperature = inputMessage.toInt();
    }
    if (request->hasParam("IgnoreRPM")) {
      foundParameter = true;
      inputMessage = request->getParam("IgnoreRPM")->value();
      settingWrite(NK_IgnoreRPM, inputMessage.c_str());
      IgnoreRPM = inputMessage.toInt();
    }
    if (request->hasParam("MinRPMForField")) {
      foundParameter = true;
      inputMessage = request->getParam("MinRPMForField")->value();
      settingWrite(NK_MinRPMForField, inputMessage.c_str());
      MinRPMForField = inputMessage.toInt();
    }
    if (request->hasParam("bmsLogic")) {
      foundParameter = true;
      inputMessage = request->getParam("bmsLogic")->value();
      settingWrite(NK_bmsLogic, inputMessage.c_str());
      bmsLogic = inputMessage.toInt();
    }
    if (request->hasParam("bmsLogicLevelOff")) {
      foundParameter = true;
      inputMessage = request->getParam("bmsLogicLevelOff")->value();
      settingWrite(NK_bmsLogicLevelOff, inputMessage.c_str());
      bmsLogicLevelOff = inputMessage.toInt();
    }
    if (request->hasParam("AlarmActivate")) {
      foundParameter = true;
      inputMessage = request->getParam("AlarmActivate")->value();
      settingWrite(NK_AlarmActivate, inputMessage.c_str());
      AlarmActivate = inputMessage.toInt();
    }
    if (request->hasParam("TempAlarm")) {
      foundParameter = true;
      inputMessage = request->getParam("TempAlarm")->value();
      settingWrite(NK_TempAlarm, inputMessage.c_str());
      TempAlarm = inputMessage.toInt();
    }
    if (request->hasParam("TempAlarmLow")) {
      foundParameter = true;
      inputMessage = request->getParam("TempAlarmLow")->value();
      settingWrite(NK_TempAlarmLow, inputMessage.c_str());
      TempAlarmLow = inputMessage.toInt();
    }
    if (request->hasParam("VoltageAlarmHigh")) {
      foundParameter = true;
      inputMessage = request->getParam("VoltageAlarmHigh")->value();
      settingWrite(NK_VoltageAlarmHigh, inputMessage.c_str());
      VoltageAlarmHigh = inputMessage.toFloat();
    }
    if (request->hasParam("VoltageAlarmLow")) {
      foundParameter = true;
      inputMessage = request->getParam("VoltageAlarmLow")->value();
      settingWrite(NK_VoltageAlarmLow, inputMessage.c_str());
      VoltageAlarmLow = inputMessage.toFloat();
    }
    if (request->hasParam("SocAlarmLow")) {
      foundParameter = true;
      inputMessage = request->getParam("SocAlarmLow")->value();
      SocAlarmLow = constrain(inputMessage.toInt(), 0, 100);
      settingWrite(NK_SocAlarmLow, String(SocAlarmLow).c_str());
    }
    if (request->hasParam("CurrentAlarmHigh")) {
      foundParameter = true;
      inputMessage = request->getParam("CurrentAlarmHigh")->value();
      settingWrite(NK_CurrentAlarmHigh, inputMessage.c_str());
      CurrentAlarmHigh = inputMessage.toInt();
    }
    if (request->hasParam("RPMScalingFactor")) {
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
    if (request->hasParam("FieldResistance")) {
      foundParameter = true;
      inputMessage = request->getParam("FieldResistance")->value();
      settingWrite(NK_FieldResistance, inputMessage.c_str());
      FieldResistance = inputMessage.toFloat();
    }
    if (request->hasParam("AlternatorCOffset")) {
      foundParameter = true;
      inputMessage = request->getParam("AlternatorCOffset")->value();
      settingWrite(NK_AlternatorCOffset, inputMessage.c_str());
      AlternatorCOffset = inputMessage.toFloat();
    }
    if (request->hasParam("BatteryCOffset")) {
      foundParameter = true;
      inputMessage = request->getParam("BatteryCOffset")->value();
      settingWrite(NK_BatteryCOffset, inputMessage.c_str());
      BatteryCOffset = inputMessage.toFloat();
    }
    if (request->hasParam("AmpSensorRange")) {
      foundParameter = true;
      inputMessage = request->getParam("AmpSensorRange")->value();
      settingWrite(NK_AmpSensorRange, inputMessage.c_str());
      AmpSensorRange = inputMessage.toInt();
      queueConsoleMessageF("AmpSensorRange changed to: %d", AmpSensorRange);
    }
    if (request->hasParam("R_fixed")) {
      foundParameter = true;
      inputMessage = request->getParam("R_fixed")->value();
      settingWrite(NK_R_fixed, inputMessage.c_str());
      R_fixed = inputMessage.toFloat();
    }
    if (request->hasParam("Beta")) {
      foundParameter = true;
      inputMessage = request->getParam("Beta")->value();
      settingWrite(NK_Beta, inputMessage.c_str());
      Beta = inputMessage.toFloat();
    }
    if (request->hasParam("T0_C")) {
      foundParameter = true;
      inputMessage = request->getParam("T0_C")->value();
      settingWrite(NK_T0_C, inputMessage.c_str());
      T0_C = inputMessage.toFloat();
    }
    if (request->hasParam("TempSource")) {
      foundParameter = true;
      inputMessage = request->getParam("TempSource")->value();
      settingWrite(NK_TempSource, inputMessage.c_str());
      TempSource = inputMessage.toInt();
    }
    if (request->hasParam("IgnitionOverride")) {
      foundParameter = true;
      inputMessage = request->getParam("IgnitionOverride")->value();
      settingWrite(NK_IgnitionOverride, inputMessage.c_str());
      IgnitionOverride = inputMessage.toInt();
    }
    if (request->hasParam("AlarmLatchEnabled")) {
      foundParameter = true;
      inputMessage = request->getParam("AlarmLatchEnabled")->value();
      settingWrite(NK_AlarmLatchEnabled, inputMessage.c_str());
      AlarmLatchEnabled = inputMessage.toInt();
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
      queueConsoleMessage("ALARM LATCH: Reset requested from web interface NO FUNCTION!");
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
        if (request->hasParam("imuOrient")) {
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
      rebootRequested = true;          // deferred — loop() does the actual restart after response flushes
      rebootRequestedAt = millis();
      queueConsoleMessage("REBOOT: requested from /get endpoint");
      inputMessage = "1";
    }
    if (request->hasParam("absorptionCompleteTime")) {
      foundParameter = true;
      inputMessage = request->getParam("absorptionCompleteTime")->value();
      uint32_t seconds = (uint32_t)inputMessage.toInt();
      absorptionCompleteTime = seconds * 1000UL;
      settingWrite(NK_absorptionCompleteTime, String(absorptionCompleteTime).c_str());
    }
    if (request->hasParam("FLOAT_DURATION")) {
      foundParameter = true;
      inputMessage = request->getParam("FLOAT_DURATION")->value();
      float hours = inputMessage.toFloat();
      int seconds = (int)(hours * 3600.0f);  // fractional hours preserved
      FLOAT_DURATION = seconds;
      settingWrite(NK_FLOAT_DURATION, String(seconds).c_str());
    }
    if (request->hasParam("UseFloat")) {
      foundParameter = true;
      inputMessage = request->getParam("UseFloat")->value();
      UseFloat = constrain(inputMessage.toInt(), 0, 2);  // 0=idle, 1=voltage float, 2=zero-current float
      // Keyed on the sticky "no shunt" statement, not HAS_BATT_SHUNT: with a shunt fitted but no resistance
      // entered, plain float still works (absorption ends on AbsorptionTimeoutMs, CV then holds FloatVoltage)
      // and zeroFloatActive already self-gates. Refusing here would persist a 0 the user can't undo.
      if (!BatteryShuntPresent) UseFloat = 0;
      settingWrite(NK_UseFloat, String(UseFloat).c_str());
    }
    if (request->hasParam("RebulkCurrent_A")) {
      foundParameter = true;
      inputMessage = request->getParam("RebulkCurrent_A")->value();
      RebulkCurrent_A = inputMessage.toFloat();
      settingWrite(NK_RebulkCurrent_A, String(RebulkCurrent_A).c_str());
    }
    if (request->hasParam("VMGTargetBearing")) {
      foundParameter = true;
      inputMessage = request->getParam("VMGTargetBearing")->value();
      VMGTargetBearing = constrain(inputMessage.toFloat(), -1.0f, 359.0f);  // -1 = not set
      settingWrite(NK_VMGTargetBearing, String(VMGTargetBearing).c_str());
    }
    // No VMGUseTrueWind param — both VMGs (manual + upwind) are always computed.
    if (request->hasParam("timeSourceMode")) {
      foundParameter = true;
      inputMessage = request->getParam("timeSourceMode")->value();
      uint8_t m = (uint8_t)inputMessage.toInt();
      if (m > TSRC_NTP) m = TSRC_AUTO;  // sanity
      timeSourceMode = m;
      settingWrite(NK_timeSourceMode, String(timeSourceMode).c_str());
      const char *lbl = (m == TSRC_AUTO)  ? "auto"
                      : (m == TSRC_NMEA)  ? "NMEA only"
                      : (m == TSRC_PHONE) ? "phone only"
                                         : "NTP time only";
      queueConsoleMessageF("Time source set to %s", lbl);
    }
    if (request->hasParam("gpsPositionSource")) {
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
    if (request->hasParam("speedSourceMode")) {
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
      queueConsoleMessage("Max Thermistor Temp: Reset requested from web interface");
    }
    if (request->hasParam("ResetTemp")) {
      foundParameter = true;
      MaxAlternatorTemperatureF = 0;
      queueConsoleMessage("Max Alterantor Temp: Reset requested from web interface");
    }
    if (request->hasParam("ResetVoltage")) {
      foundParameter = true;
      IBVMax = 0;
      queueConsoleMessage("Max Voltage: Reset requested from web interface");
    }
    if (request->hasParam("ResetCurrent")) {
      foundParameter = true;
      MeasuredAmpsMax = 0;
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
      ChargeCycles = 0;
      nvsPersistNow = true;
      queueConsoleMessage("Charge Cycles: Reset requested from web interface");
    }
    if (request->hasParam("ResetMinVoltage")) {
      foundParameter = true;
      MinVoltage = 999.0;
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
      nvsPersistNow = true;
      queueConsoleMessage("Average Speed: Reset requested from web interface");
    }
    if (request->hasParam("ResetMaxSpeed")) {
      foundParameter = true;
      MaxSpeed = 0;
      nvsPersistNow = true;
      queueConsoleMessage("Sustained Speed: Reset requested from web interface");
    }
    // Lifetime nav/sailing records — individual resets (moved off the diagnostics "Reset Peak
    // Values" button). prev_* shadows are intentionally NOT touched so saveNVSDataFull() sees the
    // change and actually persists the cleared value.
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
    if (request->hasParam("MaximumAllowedBatteryAmps")) {
      foundParameter = true;
      inputMessage = request->getParam("MaximumAllowedBatteryAmps")->value();
      settingWrite(NK_MaximumAllowedBatteryAmps, inputMessage.c_str());
      MaximumAllowedBatteryAmps = inputMessage.toInt();
    }
    if (request->hasParam("LoadDumpDtThresh")) {
      foundParameter = true;
      inputMessage = request->getParam("LoadDumpDtThresh")->value();
      LoadDumpDtThresh = inputMessage.toFloat();
      settingWrite(NK_LoadDumpDtThresh, String(LoadDumpDtThresh).c_str());
    }
    if (request->hasParam("LoadDumpDtThresh1")) {
      foundParameter = true;
      inputMessage = request->getParam("LoadDumpDtThresh1")->value();
      LoadDumpDtThresh1 = inputMessage.toFloat();
      settingWrite(NK_LoadDumpDtThresh1, String(LoadDumpDtThresh1).c_str());
    }
    if (request->hasParam("LoadDumpDtThresh3")) {
      foundParameter = true;
      inputMessage = request->getParam("LoadDumpDtThresh3")->value();
      LoadDumpDtThresh3 = inputMessage.toFloat();
      settingWrite(NK_LoadDumpDtThresh3, String(LoadDumpDtThresh3).c_str());
    }
    if (request->hasParam("LoadDumpN1")) {
      foundParameter = true;
      inputMessage = request->getParam("LoadDumpN1")->value();
      LoadDumpN1 = constrain(inputMessage.toInt(), 1, 10);
      settingWrite(NK_LoadDumpN1, String(LoadDumpN1).c_str());
    }
    if (request->hasParam("LoadDumpN2")) {
      foundParameter = true;
      inputMessage = request->getParam("LoadDumpN2")->value();
      LoadDumpN2 = constrain(inputMessage.toInt(), 1, 10);
      settingWrite(NK_LoadDumpN2, String(LoadDumpN2).c_str());
    }
    if (request->hasParam("LoadDumpN3")) {
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
    if (request->hasParam("ManualSOCPoint")) {
      foundParameter = true;
      inputMessage = request->getParam("ManualSOCPoint")->value();
      settingWrite(NK_ManualSOCPoint, inputMessage.c_str());
      ManualSOCPoint = inputMessage.toFloat();
      SOC_percent = (int)roundf(ManualSOCPoint * 100.0f);   // SOC_percent is percent x100; round so decimals seed exactly
      CoulombCount_Ah_scaled = (ManualSOCPoint * BatteryCapacity_Ah);
      shadowCoulombX100 = CoulombCount_Ah_scaled;  // external seed re-anchors the shadow twin
      nvsPersistNow = true;  // persist SoC + coulomb count NOW (single save at end of handler). NVS otherwise only saves at the field-off edge, so a reboot before then (e.g. a forced OTA) would revert the manual seed and the loop would re-derive SoC from the stale/zero coulomb count.
      queueConsoleMessageF("SoC manually set to: %.2f%%", ManualSOCPoint);
    }
    if (request->hasParam("BatteryCapacity_Ah")) {
      foundParameter = true;
      inputMessage = request->getParam("BatteryCapacity_Ah")->value();
      BatteryCapacity_Ah = constrain(inputMessage.toInt(), 1L, 100000L);  // reject 0 Ah — it divides-by-zero into SOC
      settingWrite(NK_BatteryCapacity_Ah, String(BatteryCapacity_Ah).c_str());
      PeukertRatedCurrent_A = BatteryCapacity_Ah / 20.0f;
      queueConsoleMessageF("Battery capacity set to: %d Ah", BatteryCapacity_Ah);
    }
    if (request->hasParam("ShuntResistanceMicroOhm")) {
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

    if (request->hasParam("VoltageKp")) {
      foundParameter = true;
      inputMessage = request->getParam("VoltageKp")->value();
      settingWrite(NK_VoltageKp, inputMessage.c_str());
      VoltageKp = inputMessage.toFloat();
      recomputeCvGains();  // manual gain changed → refresh the active gain
      if (CVTuningMode) cvTuningParamChanged = true;
    }

    if (request->hasParam("maxPoints")) {
      foundParameter = true;
      inputMessage = request->getParam("maxPoints")->value();
      settingWrite(NK_maxPoints, inputMessage.c_str());
      maxPoints = inputMessage.toInt();
    }
    if (request->hasParam("Ymin1")) {
      foundParameter = true;
      inputMessage = request->getParam("Ymin1")->value();
      settingWrite(NK_Ymin1, inputMessage.c_str());
      Ymin1 = inputMessage.toInt();
    }
    if (request->hasParam("Ymax1")) {
      foundParameter = true;
      inputMessage = request->getParam("Ymax1")->value();
      settingWrite(NK_Ymax1, inputMessage.c_str());
      Ymax1 = inputMessage.toInt();
    }
    if (request->hasParam("Ymin2")) {
      foundParameter = true;
      inputMessage = request->getParam("Ymin2")->value();
      settingWrite(NK_Ymin2, inputMessage.c_str());
      Ymin2 = inputMessage.toFloat();
    }
    if (request->hasParam("Ymax2")) {
      foundParameter = true;
      inputMessage = request->getParam("Ymax2")->value();
      settingWrite(NK_Ymax2, inputMessage.c_str());
      Ymax2 = inputMessage.toFloat();
    }
    if (request->hasParam("Ymin3")) {
      foundParameter = true;
      inputMessage = request->getParam("Ymin3")->value();
      settingWrite(NK_Ymin3, inputMessage.c_str());
      Ymin3 = inputMessage.toInt();
    }
    if (request->hasParam("Ymax3")) {
      foundParameter = true;
      inputMessage = request->getParam("Ymax3")->value();
      settingWrite(NK_Ymax3, inputMessage.c_str());
      Ymax3 = inputMessage.toInt();
    }
    if (request->hasParam("Ymin4")) {
      foundParameter = true;
      inputMessage = request->getParam("Ymin4")->value();
      settingWrite(NK_Ymin4, inputMessage.c_str());
      Ymin4 = inputMessage.toInt();
    }
    if (request->hasParam("Ymax4")) {
      foundParameter = true;
      inputMessage = request->getParam("Ymax4")->value();
      settingWrite(NK_Ymax4, inputMessage.c_str());
      Ymax4 = inputMessage.toInt();
    }
    if (request->hasParam("AutoShuntGainCorrection")) {
      foundParameter = true;
      inputMessage = request->getParam("AutoShuntGainCorrection")->value();
      settingWrite(NK_AutoShuntGainCorrection, inputMessage.c_str());
      AutoShuntGainCorrection = inputMessage.toInt();
    }
    if (request->hasParam("AutoAltCurrentZero")) {
      foundParameter = true;
      inputMessage = request->getParam("AutoAltCurrentZero")->value();
      settingWrite(NK_AutoAltCurrentZero, inputMessage.c_str());
      AutoAltCurrentZero = inputMessage.toInt();
    }
    if (request->hasParam("ResetDynamicShuntGain")) {
      foundParameter = true;
      ResetDynamicShuntGain = 1;  // Set the flag - don't save to file as it's momentary
      queueConsoleMessage("SOC Gain: Reset requested from web interface");
      inputMessage = "1";
    }
    if (request->hasParam("ResetDynamicAltZero")) {
      foundParameter = true;
      ResetDynamicAltZero = 1;  // Set the flag - don't save to file as it's momentary
      queueConsoleMessage("Alt Zero: Reset requested from web interface");
      inputMessage = "1";
    }
    if (request->hasParam("WindingTempOffset")) {
      foundParameter = true;
      inputMessage = request->getParam("WindingTempOffset")->value();
      settingWrite(NK_WindingTempOffset, inputMessage.c_str());
      WindingTempOffset = inputMessage.toFloat();
    }
    if (request->hasParam("displayTempUnit")) {
      foundParameter = true;
      inputMessage = request->getParam("displayTempUnit")->value();
      settingWrite(NK_displayTempUnit, inputMessage.c_str());
      displayTempUnit = (uint8_t)inputMessage.toInt();
    }
    if (request->hasParam("displayVolUnit")) {
      foundParameter = true;
      inputMessage = request->getParam("displayVolUnit")->value();
      settingWrite(NK_displayVolUnit, inputMessage.c_str());
      displayVolUnit = (uint8_t)inputMessage.toInt();
    }
    if (request->hasParam("PulleyRatio")) {
      foundParameter = true;
      inputMessage = request->getParam("PulleyRatio")->value();
      settingWrite(NK_PulleyRatio, inputMessage.c_str());
      PulleyRatio = inputMessage.toFloat();
    }
    if (request->hasParam("ManualLifePercentage")) {
      foundParameter = true;
      inputMessage = request->getParam("ManualLifePercentage")->value();
      settingWrite(NK_ManualLifePercentage, inputMessage.c_str());
      ManualLifePercentage = inputMessage.toInt();
      float life_fraction = ManualLifePercentage / 100.00;
      CumulativeInsulationDamage = 1.0 - life_fraction;
      CumulativeGreaseDamage = 1.0 - life_fraction;
      CumulativeBrushDamage = 1.0 - life_fraction;
      nvsPersistNow = true;   // commit InsulDamage/GreaseDamage/BrushDamage now (single save at end of handler) instead of waiting for the next field-off edge.
      queueConsoleMessageF("Alternator life manually set to %d%%", ManualLifePercentage);
    }
    if (request->hasParam("webgaugesinterval")) {
      foundParameter = true;
      inputMessage = request->getParam("webgaugesinterval")->value();
      int newInterval = inputMessage.toInt();
      newInterval = constrain(newInterval, 1, 10000000);
      settingWrite(NK_webgaugesinterval, String(newInterval).c_str());
      webgaugesinterval = newInterval;
      queueConsoleMessageF("Update interval set to: %dms", newInterval);
    }
    if (request->hasParam("battMaxMode")) {
      foundParameter = true;
      inputMessage = request->getParam("battMaxMode")->value();
      battMaxMode = (inputMessage.toInt() != 0);
      settingWrite(NK_battMaxMode, battMaxMode ? "1" : "0");
      queueConsoleMessageF("Battery V/I plot sampling: %s", battMaxMode ? "Max" : "Average");
    }
    if (request->hasParam("BatteryCurrentSource")) {
      foundParameter = true;
      inputMessage = request->getParam("BatteryCurrentSource")->value();
      settingWrite(NK_BatteryCurrentSource, inputMessage.c_str());
      BatteryCurrentSource = inputMessage.toInt();
      queueConsoleMessageF("Battery current source changed to: %d", BatteryCurrentSource);
    }
    if (request->hasParam("totalPowerCycles")) {
      foundParameter = true;
      inputMessage = request->getParam("totalPowerCycles")->value();
      settingWrite(NK_totalPowerCycles, inputMessage.c_str());
      totalPowerCycles = inputMessage.toInt();
      nvsPersistNow = true;   // also commit the storage-namespace "PowerCycles" (what loadNVSData reads at boot) now, not at the field-off edge.
    }
    if (request->hasParam("timeAxisModeChanging")) {
      foundParameter = true;
      inputMessage = request->getParam("timeAxisModeChanging")->value();
      settingWrite(NK_timeAxisModeChanging, inputMessage.c_str());
      timeAxisModeChanging = inputMessage.toInt();
      queueConsoleMessageF("Time axis mode changed to: %s", timeAxisModeChanging ? "UNIX timestamps" : "relative time");
    }
    if (request->hasParam("plotTimeWindow")) {
      foundParameter = true;
      inputMessage = request->getParam("plotTimeWindow")->value();
      settingWrite(NK_plotTimeWindow, inputMessage.c_str());
      plotTimeWindow = inputMessage.toInt();
    }
    if (request->hasParam("LatitudeNMEA") && request->hasParam("LongitudeNMEA")) {
      foundParameter = true;
      // Engage the sticky manual override: these coords now beat boat NMEA and
      // phone GPS until the user clears it (clearGpsManual). resolveSources()
      // reasserts them every tick, so the auto-sources can't clobber them.
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
    if (request->hasParam("weatherModeEnabled")) {
      foundParameter = true;
      inputMessage = request->getParam("weatherModeEnabled")->value();
      settingWrite(NK_weatherModeEnabled, inputMessage.c_str());
      weatherModeEnabled = inputMessage.toInt();
      queueConsoleMessageF("Weather Mode %s", weatherModeEnabled ? "enabled" : "disabled");
    }
    if (request->hasParam("UVThresholdHigh")) {
      foundParameter = true;
      inputMessage = request->getParam("UVThresholdHigh")->value();
      settingWrite(NK_UVThresholdHigh, inputMessage.c_str());
      UVThresholdHigh = inputMessage.toFloat();
    }
    if (request->hasParam("SolarWatts")) {
      foundParameter = true;
      inputMessage = request->getParam("SolarWatts")->value();
      settingWrite(NK_SolarWatts, inputMessage.c_str());
      SolarWatts = inputMessage.toInt();
      weatherRecomputePredicted();
    }
    if (request->hasParam("performanceRatio")) {
      foundParameter = true;
      inputMessage = request->getParam("performanceRatio")->value();
      settingWrite(NK_performanceRatio, inputMessage.c_str());
      performanceRatio = inputMessage.toFloat();
      sledRatioDirty = false;   // the typed value is now the stored value — a pending learned write must not overwrite it
      weatherRecomputePredicted();
    }
    if (request->hasParam("solarLearnEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("solarLearnEnable")->value();
      settingWrite(NK_solarLearnEnable, inputMessage.c_str());
      solarLearnEnable = inputMessage.toInt();
    }
    if (request->hasParam("solarUseConsEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("solarUseConsEnable")->value();
      settingWrite(NK_solarUseConsEnable, inputMessage.c_str());
      solarUseConsEnable = inputMessage.toInt();
    }
    if (request->hasParam("solarConsMarginPct")) {
      foundParameter = true;
      inputMessage = request->getParam("solarConsMarginPct")->value();
      settingWrite(NK_solarConsMarginPct, inputMessage.c_str());
      solarConsMarginPct = inputMessage.toFloat();
    }
    if (request->hasParam("solarLearnRatePct")) {
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
      // Every rejection path must give user feedback — a silent fall-through (e.g. no GPS) reads as
      // "nothing happened". Check each precondition separately so the message names the actual reason.
      // Deliberately NOT gated on field state: the scheduled fetch waits for fieldOffSettled, but a
      // user pressing the button has asked for it now and owns the consequences.
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
      queueConsoleMessage("Learning table reset from web - refresh to see defaults");
      inputMessage = "1";
    }
    if (request->hasParam("ClearOverheatHistory")) {
      foundParameter = true;
      pendingClearOverheatHistory = true;  // deferred to Core 1 to avoid SSE gap
      inputMessage = "1";
    }
    if (request->hasParam("CAPSIZE_THRESHOLD_DEG")) {
      foundParameter = true;
      inputMessage = request->getParam("CAPSIZE_THRESHOLD_DEG")->value();
      settingWrite(NK_CAPSIZE_THRESHOLD_DEG, inputMessage.c_str());
      CAPSIZE_THRESHOLD_DEG = inputMessage.toFloat();
    }
    if (request->hasParam("PITCHPOLE_THRESHOLD_DEG")) {
      foundParameter = true;
      inputMessage = request->getParam("PITCHPOLE_THRESHOLD_DEG")->value();
      settingWrite(NK_PITCHPOLE_THRESHOLD_DEG, inputMessage.c_str());
      PITCHPOLE_THRESHOLD_DEG = inputMessage.toFloat();
    }
    if (request->hasParam("SLAM_THRESHOLD_G")) {
      foundParameter = true;
      inputMessage = request->getParam("SLAM_THRESHOLD_G")->value();
      settingWrite(NK_SLAM_THRESHOLD_G, inputMessage.c_str());
      SLAM_THRESHOLD_G = inputMessage.toFloat();
    }
    if (request->hasParam("IgnoreLearningDuringPenalty")) {
      foundParameter = true;
      inputMessage = request->getParam("IgnoreLearningDuringPenalty")->value();
      settingWrite(NK_IgnoreLearningDuringPenalty, inputMessage.c_str());
      IgnoreLearningDuringPenalty = inputMessage.toInt();
    }
    if (request->hasParam("CloudFeatures")) {
      foundParameter = true;
      inputMessage = request->getParam("CloudFeatures")->value();
      settingWrite(NK_CloudFeatures, inputMessage.c_str());
      CloudFeatures = inputMessage.toInt();
    }
    if (request->hasParam("testProtectionsEnabled")) {
      foundParameter = true;
      inputMessage = request->getParam("testProtectionsEnabled")->value();
      testProtectionsEnabled = (inputMessage.toInt() != 0);
      // NOT persisted to LittleFS — resets TRUE (enabled) on every boot by design.
      // foundParameter=true above bumps settingsDirty so CSV3 echoes the change immediately,
      // which fires the dashboard banner show/hide and syncs the per-page toggle states.
      if (testProtectionsEnabled) {
        queueConsoleMessage("PROTECTIONS ENABLED — all protection layers restored");
      } else {
        queueConsoleMessage("PROTECTIONS DISABLED for tuning — G1/G2 over-voltage, timed OV cut tiers + G3 iExcess over-current bypassed; fast-OV hard-cut (AlternatorHardShutdownV), G4 load-dump/battery-current limit, INA228, and hardware OC remain active");
      }
    }
    if (request->hasParam("TuningMode")) {
      foundParameter = true;
      inputMessage = request->getParam("TuningMode")->value();
      int requested = inputMessage.toInt();
      // Mutex: refuse turn-on if another test is already running. All four tests must run independently.
      if (requested == 1 && TuningMode == 0) {
        const char *blocker = (systemIDActive != 0) ? "Plant Delay Test"
                              : (altSweepActive != 0) ? "Gate-tuning field sweep"
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
    if (request->hasParam("tuningSquareAbrupt")) {
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
    if (request->hasParam("bhStepLowA")) {
      foundParameter = true;
      bhStepLowA = request->getParam("bhStepLowA")->value().toFloat();
      settingWrite(NK_bhStepLowA, String(bhStepLowA, 1).c_str());
    }
    if (request->hasParam("bhStepDeltaA")) {
      foundParameter = true;
      bhStepDeltaA = request->getParam("bhStepDeltaA")->value().toFloat();
      if (bhStepDeltaA < 5.0f) bhStepDeltaA = 5.0f;
      settingWrite(NK_bhStepDeltaA, String(bhStepDeltaA, 1).c_str());
      if (bhDwellMs < bhMinDwellMs()) {   // bigger step lengthens the slew traverse — stretch dwell to match
        bhDwellMs = bhMinDwellMs();
        settingWrite(NK_bhDwellMs, String(bhDwellMs).c_str());
      }
    }
    if (request->hasParam("bhDwellMs")) {   // stored unit — the CONFIG_MANIFEST key, so the fleet
      foundParameter = true;                // snapshot names what it holds. bhDwellSec is the UI alias.
      bhDwellMs = (uint32_t)request->getParam("bhDwellMs")->value().toInt();
      if (bhDwellMs < bhMinDwellMs()) bhDwellMs = bhMinDwellMs();
      settingWrite(NK_bhDwellMs, String(bhDwellMs).c_str());
    }
    if (request->hasParam("bhDwellSec")) {
      foundParameter = true;   // UI is seconds, internal is ms
      bhDwellMs = (uint32_t)(request->getParam("bhDwellSec")->value().toFloat() * 1000.0f);
      if (bhDwellMs < bhMinDwellMs()) bhDwellMs = bhMinDwellMs();
      settingWrite(NK_bhDwellMs, String(bhDwellMs).c_str());
    }
    if (request->hasParam("bhNumEdges")) {
      foundParameter = true;
      int e = request->getParam("bhNumEdges")->value().toInt();
      if (e < 3) e = 3;
      if (e > BH_MAX_TOGGLES - 3) e = BH_MAX_TOGGLES - 3;
      bhNumEdges = (uint8_t)e;
      settingWrite(NK_bhNumEdges, String(bhNumEdges).c_str());
    }
    if (request->hasParam("bhStartOver")) {
      foundParameter = true;   // battery replaced: re-baseline + wipe history
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
    if (request->hasParam("capRestFrac"))   { foundParameter = true; capRestCurrentFrac = request->getParam("capRestFrac")->value().toFloat();   settingWrite(NK_capRestFrac, String(capRestCurrentFrac, 4).c_str()); }
    if (request->hasParam("capRestFloor"))  { foundParameter = true; capRestFloorMin    = (uint16_t)request->getParam("capRestFloor")->value().toInt(); settingWrite(NK_capRestFloor, String(capRestFloorMin).c_str()); }
    if (request->hasParam("capSettleRate")) { foundParameter = true; capSettleRateMv10  = request->getParam("capSettleRate")->value().toFloat(); settingWrite(NK_capSettleRate, String(capSettleRateMv10, 2).c_str()); }
    if (request->hasParam("capSocLowMax"))  { foundParameter = true; capSocLowMax       = request->getParam("capSocLowMax")->value().toFloat();  settingWrite(NK_capSocLowMax, String(capSocLowMax, 1).c_str()); }
    if (request->hasParam("capMinSpan"))    { foundParameter = true; capMinSpan         = request->getParam("capMinSpan")->value().toFloat();    settingWrite(NK_capMinSpan, String(capMinSpan, 1).c_str()); }
    if (request->hasParam("capFullSoc"))    { foundParameter = true; capFullSoc         = request->getParam("capFullSoc")->value().toFloat();    settingWrite(NK_capFullSoc, String(capFullSoc, 1).c_str()); }
    if (request->hasParam("capRefMode"))    { foundParameter = true; capRefMode         = (uint8_t)request->getParam("capRefMode")->value().toInt(); settingWrite(NK_capRefMode, String(capRefMode).c_str()); }
    if (request->hasParam("capTempNorm"))   { foundParameter = true; capTempNormEnable  = (uint8_t)request->getParam("capTempNorm")->value().toInt(); settingWrite(NK_capTempNorm, String(capTempNormEnable).c_str()); }
    if (request->hasParam("capTempCoeff"))  { foundParameter = true; capTempCoeffPctC   = request->getParam("capTempCoeff")->value().toFloat();  settingWrite(NK_capTempCoeff, String(capTempCoeffPctC, 3).c_str()); }
    if (request->hasParam("capTempRef"))    { foundParameter = true; capTempRefC        = request->getParam("capTempRef")->value().toFloat();   settingWrite(NK_capTempRef, String(capTempRefC, 1).c_str()); }
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
    if (request->hasParam("LogAllLearningEvents")) {
      foundParameter = true;
      inputMessage = request->getParam("LogAllLearningEvents")->value();
      settingWrite(NK_LogAllLearningEvents, inputMessage.c_str());
      LogAllLearningEvents = inputMessage.toInt();
    }
    if (request->hasParam("hardwarePresent")) {
      foundParameter = true;
      inputMessage = request->getParam("hardwarePresent")->value();
      settingWrite(NK_hardwarePresent, inputMessage.c_str());
      hardwarePresent = inputMessage.toInt();
      queueConsoleMessageF("hardwarePresent mode %s", hardwarePresent ? "enabled" : "disabled");
    }

    // RPM breakpoint and current table handlers
    bool tableChangedThisRequest = false;
    for (int i = 0; i < 10; i++) {
      char paramName[32];

      snprintf(paramName, sizeof(paramName), "rpmTableRPMPoints%d", i);
      if (request->hasParam(paramName)) {
        foundParameter = true;
        rpmTableRPMPoints[i] = request->getParam(paramName)->value().toInt();
        tableChangedThisRequest = true;
      }

      snprintf(paramName, sizeof(paramName), "rpmCapCurrentTable%d", i);
      if (request->hasParam(paramName)) {
        foundParameter = true;
        rpmCapCurrentTable[i] = request->getParam(paramName)->value().toFloat();
        tableChangedThisRequest = true;
      }

      snprintf(paramName, sizeof(paramName), "rpmCapKW%d", i);
      if (request->hasParam(paramName)) {
        foundParameter = true;
        rpmCapPowerTable[i] = request->getParam(paramName)->value().toFloat() * 1000.0f;
        tableChangedThisRequest = true;
      }

      snprintf(paramName, sizeof(paramName), "rpmMinDutyTable%d", i);
      if (request->hasParam(paramName)) {
        foundParameter = true;
        rpmMinDutyTable[i] = request->getParam(paramName)->value().toFloat();
        tableChangedThisRequest = true;
      }
    }

    if (tableChangedThisRequest) {
      // Track which RPM breakpoints changed
      learningTableUpdated = true;  // ← update the global so buildConfigPayload() sees it

      static int previousRPMPoints[RPM_TABLE_SIZE] = { 0 };
      static bool firstRun = true;

      if (firstRun) {
        for (int i = 0; i < RPM_TABLE_SIZE; i++) {
          previousRPMPoints[i] = rpmTableRPMPoints[i];
        }
        firstRun = false;
      }

      bool rpmPointChanged = false;
      for (int i = 0; i < RPM_TABLE_SIZE; i++) {
        if (rpmTableRPMPoints[i] != previousRPMPoints[i]) {
          rpmPointChanged = true;
          overheatCount[i] = 0;
          lastOverheatTime[i] = 0;
          cumulativeNoOverheatTime[i] = 0;
          learningUpCount[i] = 0;

          if (i > 0) {
            overheatCount[i - 1] = 0;
            lastOverheatTime[i - 1] = 0;
            cumulativeNoOverheatTime[i - 1] = 0;
            learningUpCount[i - 1] = 0;
          }

          queueConsoleMessageF("Learning: RPM bin %d changed - cleared affected history", i);
          previousRPMPoints[i] = rpmTableRPMPoints[i];
        }
      }

      // An RPM breakpoint moved → every learned Min% floor is now keyed to the wrong RPM
      // (the per-RPM onset-knee fit spans all bins). Wipe the knee tracker entirely so the floors are
      // re-measured at the new breakpoints instead of silently re-applied at shifted RPMs.
      if (rpmPointChanged) {
        kneeLearnResetDefaults();   // clear floors/knees, unfreeze, column back to the flat baseline
        commissionClearStage(7);    // Keep-alive floor + Field decay stage now stale (demotes a commissioned device to in-progress)
        queueConsoleMessage("Learning: RPM breakpoints changed — keep-alive floors cleared, re-run the Keep-Alive Floor step");
      }

      pendingSaveUserTableEdits = true;  // deferred to Core 1 to avoid SSE gap
      queueConsoleMessage("Learning: Table saved to NVS");
    }

    // Pre-commissioning "Charge Rate Limits" screen: seed BOTH the Low and High cap tables in one
    // request, independent of the live HiLow mode. Only the active mode's live ceiling is refreshed
    // here (so the running alternator honors the edit immediately); the mode switch itself rides a
    // separate HiLow request so its ceiling glide runs. Both blobs are written on Core 1.
    if (request->hasParam("capBothSave")) {
      foundParameter = true;
      char pn[32];
      bool ptsChanged = false;
      float hiA[RPM_TABLE_SIZE], loA[RPM_TABLE_SIZE], hiW[RPM_TABLE_SIZE], loW[RPM_TABLE_SIZE];
      for (int i = 0; i < RPM_TABLE_SIZE; i++) {
        hiA[i] = rpmCapCurrentTable[i]; loA[i] = rpmCapCurrentTable[i];
        hiW[i] = rpmCapPowerTable[i];   loW[i] = rpmCapPowerTable[i];
        snprintf(pn, sizeof(pn), "rpmCapHi%d", i);    if (request->hasParam(pn)) hiA[i] = request->getParam(pn)->value().toFloat();
        snprintf(pn, sizeof(pn), "rpmCapLo%d", i);    if (request->hasParam(pn)) loA[i] = request->getParam(pn)->value().toFloat();
        snprintf(pn, sizeof(pn), "rpmCapPwrHi%d", i); if (request->hasParam(pn)) hiW[i] = request->getParam(pn)->value().toFloat() * 1000.0f;
        snprintf(pn, sizeof(pn), "rpmCapPwrLo%d", i); if (request->hasParam(pn)) loW[i] = request->getParam(pn)->value().toFloat() * 1000.0f;

        snprintf(pn, sizeof(pn), "rpmTableRPMPoints%d", i);
        if (request->hasParam(pn)) {
          int v = request->getParam(pn)->value().toInt();
          if (v != rpmTableRPMPoints[i]) { ptsChanged = true; rpmTableRPMPoints[i] = v; }
        }
        // Refresh only the ACTIVE mode's live ceiling (so a running alternator honors the edit now);
        // the mode switch itself rides a separate HiLow request so its ceiling glide runs.
        rpmCapCurrentTable[i] = (HiLow == 1) ? hiA[i] : loA[i];
        rpmCapPowerTable[i]   = (HiLow == 1) ? hiW[i] : loW[i];
      }
      if (ptsChanged) {
        kneeLearnResetDefaults();
        commissionClearStage(7);
        queueConsoleMessage("Learning: RPM breakpoints changed — keep-alive floors cleared, re-run the Keep-Alive Floor step");
      }
      saveBothCapTables(hiA, hiW, loA, loW);   // synchronous: NVS fresh before any following HiLow read
      learningTableUpdated = true;
      stateRevision++;
      queueConsoleMessage("Learning: Low+High cap tables saved");
    }

    // Fuel table handlers
    bool fuelTableUpdated = false;
    for (int i = 0; i < 10; i++) {
      char paramName[32];

      snprintf(paramName, sizeof(paramName), "fuelTableRPM%d", i);
      if (request->hasParam(paramName)) {
        foundParameter = true;
        float value = request->getParam(paramName)->value().toFloat();
        fuelTableRPM[i] = value;
        fuelTableUpdated = true;
      }

      snprintf(paramName, sizeof(paramName), "fuelTableGPH%d", i);
      if (request->hasParam(paramName)) {
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


    if (request->hasParam("AlternatorNominalAmps")) {
      foundParameter = true;
      inputMessage = request->getParam("AlternatorNominalAmps")->value();
      settingWrite(NK_AlternatorNominalAmps, inputMessage.c_str());
      AlternatorNominalAmps = inputMessage.toInt();
    }
    if (request->hasParam("LearningUpStep")) {
      foundParameter = true;
      inputMessage = request->getParam("LearningUpStep")->value();
      settingWrite(NK_LearningUpStep, inputMessage.c_str());
      LearningUpStep = inputMessage.toFloat();
    }
    if (request->hasParam("LearningDownStep")) {
      foundParameter = true;
      inputMessage = request->getParam("LearningDownStep")->value();
      settingWrite(NK_LearningDownStep, inputMessage.c_str());
      LearningDownStep = inputMessage.toFloat();
    }
    if (request->hasParam("xTime")) {
      foundParameter = true;
      inputMessage = request->getParam("xTime")->value();
      settingWrite(NK_xTime, inputMessage.c_str());
      xTime = inputMessage.toFloat();
    }
    if (request->hasParam("MinLearningInterval")) {
      foundParameter = true;
      inputMessage = request->getParam("MinLearningInterval")->value();
      int temp = inputMessage.toInt() * 1000;  // from seconds (entry into html) to ms
      settingWrite(NK_MinLearningInterval, String(temp).c_str());
      MinLearningInterval = temp;
    }
    if (request->hasParam("SetpointRiseRate")) {
      foundParameter = true;
      inputMessage = request->getParam("SetpointRiseRate")->value();
      settingWrite(NK_SetpointRiseRate, inputMessage.c_str());
      SetpointRiseRate = inputMessage.toFloat();
      if (TuningMode) tuningParamChanged = true;
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    if (request->hasParam("SetpointBigStepThresh")) {
      foundParameter = true;
      inputMessage = request->getParam("SetpointBigStepThresh")->value();
      settingWrite(NK_SetpointBigStepThresh, inputMessage.c_str());
      SetpointBigStepThresh = inputMessage.toFloat();
      if (TuningMode) tuningParamChanged = true;
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    if (request->hasParam("SetpointBigStepRiseRate")) {
      foundParameter = true;
      inputMessage = request->getParam("SetpointBigStepRiseRate")->value();
      settingWrite(NK_SetpointBigStepRiseRate, inputMessage.c_str());
      SetpointBigStepRiseRate = inputMessage.toFloat();
      if (TuningMode) tuningParamChanged = true;
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    if (request->hasParam("SetpointFallRate")) {
      foundParameter = true;
      inputMessage = request->getParam("SetpointFallRate")->value();
      settingWrite(NK_SetpointFallRate, inputMessage.c_str());
      SetpointFallRate = inputMessage.toFloat();
      if (TuningMode) tuningParamChanged = true;
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    if (request->hasParam("CvBrakeFallRate")) {
      foundParameter = true;
      inputMessage = request->getParam("CvBrakeFallRate")->value();
      CvBrakeFallRate = fmaxf(SetpointFallRate, inputMessage.toFloat());  // a brake rate below the normal fall rate is meaningless
      settingWrite(NK_CvBrakeFallRate, String(CvBrakeFallRate, 2).c_str());
      queueConsoleMessageF("CV D-term brake fall rate: %.1f A/s", CvBrakeFallRate);
      if (TuningMode) tuningParamChanged = true;
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    if (request->hasParam("StartupRiseRate")) {
      foundParameter = true;
      inputMessage = request->getParam("StartupRiseRate")->value();
      settingWrite(NK_StartupRiseRate, inputMessage.c_str());
      StartupRiseRate = inputMessage.toFloat();
      queueConsoleMessageF("Startup rise rate set to: %.2f A/sec", StartupRiseRate);
    }
    if (request->hasParam("PIDTrackingGain")) {
      foundParameter = true;
      inputMessage = request->getParam("PIDTrackingGain")->value();
      float temp = inputMessage.toFloat();
      settingWrite(NK_PIDTrackingGain, String(temp).c_str());
      PIDTrackingGain = temp;
      if (pidInitialized) currentPID.SetTrackingGain(PIDTrackingGain);  // enter_sys_auto() is the only other caller, so without this a live edit did nothing until the next AUTO entry
      if (TuningMode) tuningParamChanged = true;
    }
    if (request->hasParam("SafeOperationThreshold")) {
      foundParameter = true;
      inputMessage = request->getParam("SafeOperationThreshold")->value();
      int temp = inputMessage.toInt() * 1000;  // from seconds (entry into html) to ms
      settingWrite(NK_SafeOperationThreshold, String(temp).c_str());
      SafeOperationThreshold = temp;
    }
    if (request->hasParam("PidKp")) {
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
    if (request->hasParam("AbsorptionVoltage")) {
      foundParameter = true;
      inputMessage = request->getParam("AbsorptionVoltage")->value();
      AbsorptionVoltage = inputMessage.toFloat();
      settingWrite(NK_AbsorptionVoltage, inputMessage.c_str());
    }
    if (request->hasParam("TargetVoltageSetpoint")) {
      foundParameter = true;
      inputMessage = request->getParam("TargetVoltageSetpoint")->value();
      TargetVoltageSetpoint = inputMessage.toFloat();
      settingWrite(NK_TargetVoltageSetpoint, inputMessage.c_str());
    }

    if (request->hasParam("AbsorptionTimeoutMs")) {
      foundParameter = true;
      inputMessage = request->getParam("AbsorptionTimeoutMs")->value();
      uint32_t minutes = (uint32_t)inputMessage.toInt();
      AbsorptionTimeoutMs = minutes * 60000UL;
      settingWrite(NK_AbsorptionTimeoutMs, String(AbsorptionTimeoutMs).c_str());
    }
    if (request->hasParam("bulkVoltageHoldMs")) {
      foundParameter = true;
      inputMessage = request->getParam("bulkVoltageHoldMs")->value();
      float seconds = inputMessage.toFloat();
      bulkVoltageHoldMs = (uint32_t)(seconds * 1000.0f);
      settingWrite(NK_bulkVoltageHoldMs, String(bulkVoltageHoldMs).c_str());
    }
    if (request->hasParam("VoltageKi")) {
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
      // Strict "0"/"1" only: toInt("") decodes as 0 = Manual, silently swapping the fitted gains for the
      // typed fallbacks (07-24 unexplained flip). Client IP makes any future flip attributable.
      if (inputMessage == "0" || inputMessage == "1") {
        cvGainMode = (uint8_t)(inputMessage == "1" ? 1 : 0);
        settingWrite(NK_cvGainMode, String((int)cvGainMode).c_str());
        recomputeCvGains();
        queueConsoleMessageF("CV gain mode: %s (set by %s)", cvGainMode ? "AUTO (lambda-based)" : "MANUAL",
                             request->client()->remoteIP().toString().c_str());
      } else {
        queueConsoleMessageF("CV gain mode: malformed value '%s' from %s IGNORED - gain source unchanged",
                             inputMessage.c_str(), request->client()->remoteIP().toString().c_str());
      }
    }
    if (request->hasParam("cvAlpha")) {  // CV auto-gain aggressiveness α — Kp = α / measured stiffness K
      foundParameter = true;
      cvAlpha = clamp_f(request->getParam("cvAlpha")->value().toFloat(), 0.02f, 0.50f);
      settingWrite(NK_cvAlpha, String(cvAlpha, 3).c_str());
      recomputeCvGains();
      queueConsoleMessageF("CV aggressiveness alpha: %.2f", cvAlpha);
    }
    // Legacy — inert in AUTO under the ohmic-anchor rule; kept so a bench curl / stale page can't 404.
    if (request->hasParam("cvCrossover")) {  // CV crossover ω_c (rad/s)
      foundParameter = true;
      cvCrossover = clamp_f(request->getParam("cvCrossover")->value().toFloat(), 0.05f, 0.80f);  // 0.80 ≈ 5 s, fast end of the seconds control
      settingWrite(NK_cvCrossover, String(cvCrossover, 3).c_str());
      recomputeCvGains();
      queueConsoleMessageF("CV crossover omega_c: %.2f rad/s", cvCrossover);
    }
    // User-facing CV Response Time in seconds → ω_c = 4/sec (settle ≈ 4 time constants). Stored as cvCrossover;
    // the dashboard relabels ω_c as seconds, but ω_c stays the internal/NVS unit so recomputeCvGains is unchanged.
    if (request->hasParam("cvRespS")) {
      foundParameter = true;
      float respS = request->getParam("cvRespS")->value().toFloat();
      cvCrossover = clamp_f((respS > 0.0f) ? (4.0f / respS) : 0.20f, 0.05f, 0.80f);
      settingWrite(NK_cvCrossover, String(cvCrossover, 3).c_str());
      recomputeCvGains();
      queueConsoleMessageF("CV response time: %.0f s (crossover %.2f rad/s)", 4.0f / cvCrossover, cvCrossover);
    }
    if (request->hasParam("cvPiZero")) {  // CV PI integral zero ρ (rad/s) — Ki = ρ·Kp
      foundParameter = true;
      cvPiZero = clamp_f(request->getParam("cvPiZero")->value().toFloat(), 0.2f, 1.5f);
      settingWrite(NK_cvPiZero, String(cvPiZero, 3).c_str());
      recomputeCvGains();
      queueConsoleMessageF("CV PI zero (Ki/Kp ratio): %.2f", cvPiZero);
    }
    if (request->hasParam("battTempDerateEnable")) {  // master on/off for the battery-temp gain derate
      foundParameter = true;
      battTempDerateEnable = (request->getParam("battTempDerateEnable")->value().toInt() != 0);
      settingWrite(NK_battTempDerateEn, String((int)battTempDerateEnable).c_str());
      recomputeCvGains();
      queueConsoleMessageF("Battery-temp gain derate: %s", battTempDerateEnable ? "ON" : "OFF");
    }
    if (request->hasParam("battTempCoeff")) {  // battery fractional resistance change per °C (R rises as T falls)
      foundParameter = true;
      battTempCoeff = clamp_f(request->getParam("battTempCoeff")->value().toFloat(), 0.0f, 0.10f);
      settingWrite(NK_battTempCoeff, String(battTempCoeff, 4).c_str());
      recomputeCvGains();
      queueConsoleMessageF("Battery-temp resistance coeff: %.3f /°C", battTempCoeff);
    }
    if (request->hasParam("vTgtRampEnable")) {  // master switch for the voltage-target slew (1=on, 0=instant)
      foundParameter = true;
      vTgtRampEnable = (uint8_t)(request->getParam("vTgtRampEnable")->value().toInt() ? 1 : 0);
      settingWrite(NK_vTgtRampEnable, String((int)vTgtRampEnable).c_str());
      queueConsoleMessageF("CV target ramp limiter: %s", vTgtRampEnable ? "ON" : "OFF (instant)");
    }
    if (request->hasParam("setpointSlewEnable")) {  // master switch for inner-loop current setpoint slew (1=on, 0=instant)
      foundParameter = true;
      setpointSlewEnable = (uint8_t)(request->getParam("setpointSlewEnable")->value().toInt() ? 1 : 0);
      settingWrite(NK_setpointSlewEnable, String((int)setpointSlewEnable).c_str());
      queueConsoleMessageF("Current setpoint slew limiter: %s", setpointSlewEnable ? "ON" : "OFF (instant)");
    }
    if (request->hasParam("cvRiseGovEnable")) {  // master switch for CV rise governor / anti-windup clamp (1=on, 0=off)
      foundParameter = true;
      cvRiseGovEnable = (uint8_t)(request->getParam("cvRiseGovEnable")->value().toInt() ? 1 : 0);
      settingWrite(NK_cvRiseGovEnable, String((int)cvRiseGovEnable).c_str());
      queueConsoleMessageF("CV rise governor (anti-windup): %s", cvRiseGovEnable ? "ON" : "OFF — OV-trip risk on up-steps");
    }
    if (request->hasParam("cvRecovEnable")) {  // master switch for the post-protection integrator refill (1=on, 0=plain PI)
      foundParameter = true;
      cvRecovEnable = (uint8_t)(request->getParam("cvRecovEnable")->value().toInt() ? 1 : 0);
      settingWrite(NK_cvRecovEnable, String((int)cvRecovEnable).c_str());
      queueConsoleMessageF("CV recovery refill: %s", cvRecovEnable ? "ON" : "OFF — recovery pace scales with post-cut error");
    }
    if (request->hasParam("loadServeBoostEnable")) {  // load-serve Ki boost toward the measured house loads (shunt-gated)
      foundParameter = true;
      loadServeBoostEnable = (uint8_t)(request->getParam("loadServeBoostEnable")->value().toInt() ? 1 : 0);
      settingWrite(NK_loadServeBoostEnable, String((int)loadServeBoostEnable).c_str());
      queueConsoleMessageF("Load pickup boost: %s", loadServeBoostEnable ? "ON" : "OFF — load pickup pace scales with voltage error only");
    }
    if (request->hasParam("reseedCorrEnable")) {  // demand-corrected reseed: measured load-drop subtraction + rapid-refire ratchet escalation
      foundParameter = true;
      reseedCorrEnable = (uint8_t)(request->getParam("reseedCorrEnable")->value().toInt() ? 1 : 0);
      settingWrite(NK_reseedCorrEnable, String((int)reseedCorrEnable).c_str());
      queueConsoleMessageF("Smart reseed: %s", reseedCorrEnable ? "ON" : "OFF — every release restores the plain seed fraction");
    }
    if (request->hasParam("HuntGovEnable")) {  // oscillation damper: hunt detector + verified inner-Ki derate
      foundParameter = true;
      HuntGovEnable = (uint8_t)(request->getParam("HuntGovEnable")->value().toInt() ? 1 : 0);
      settingWrite(NK_HuntGovEnable, String((int)HuntGovEnable).c_str());
      queueConsoleMessageF("Oscillation damper: %s", HuntGovEnable ? "ON" : "OFF — hunting persists at full gain");
    }
    if (request->hasParam("HuntCutPct")) {
      foundParameter = true;
      HuntCutPct = (uint8_t)constrain(request->getParam("HuntCutPct")->value().toInt(), HUNT_CUT_PCT_MIN, HUNT_CUT_PCT_MAX);
      settingWrite(NK_HuntCutPct, String((int)HuntCutPct).c_str());
    }
    if (request->hasParam("HuntVerifyPct")) {
      foundParameter = true;
      HuntVerifyPct = (uint8_t)constrain(request->getParam("HuntVerifyPct")->value().toInt(), HUNT_VERIFY_PCT_MIN, HUNT_VERIFY_PCT_MAX);
      settingWrite(NK_HuntVerifyPct, String((int)HuntVerifyPct).c_str());
    }
    if (request->hasParam("HuntWingPct")) {
      foundParameter = true;
      HuntWingPct = (uint8_t)constrain(request->getParam("HuntWingPct")->value().toInt(), HUNT_WING_PCT_MIN, HUNT_WING_PCT_MAX);
      settingWrite(NK_HuntWingPct, String((int)HuntWingPct).c_str());
    }
    if (request->hasParam("HuntCooldownMin")) {
      foundParameter = true;
      HuntCooldownMin = (uint8_t)constrain(request->getParam("HuntCooldownMin")->value().toInt(), HUNT_COOLDOWN_MIN_MIN, HUNT_COOLDOWN_MIN_MAX);
      settingWrite(NK_HuntCooldownMin, String((int)HuntCooldownMin).c_str());
    }
    if (request->hasParam("HuntQualifyScans")) {
      foundParameter = true;
      HuntQualifyScans = (uint8_t)constrain(request->getParam("HuntQualifyScans")->value().toInt(), HUNT_QUALIFY_SCANS_MIN, HUNT_QUALIFY_SCANS_MAX);
      settingWrite(NK_HuntQualifyScans, String((int)HuntQualifyScans).c_str());
    }
    if (request->hasParam("HuntTrigPct")) {
      foundParameter = true;
      HuntTrigPct = fmaxf(request->getParam("HuntTrigPct")->value().toFloat(), HUNT_TRIG_PCT_MIN);
      settingWrite(NK_HuntTrigPct, String(HuntTrigPct, 2).c_str());
    }
    if (request->hasParam("HuntSteadyPct")) {
      foundParameter = true;
      HuntSteadyPct = (uint8_t)constrain(request->getParam("HuntSteadyPct")->value().toInt(), HUNT_STEADY_PCT_MIN, HUNT_STEADY_PCT_MAX);
      settingWrite(NK_HuntSteadyPct, String((int)HuntSteadyPct).c_str());
    }
    if (request->hasParam("cvRecovSec")) {  // retired timed-window knob — writes the inert global (UI field removed)
      foundParameter = true;
      cvRecovSec = clamp_f(request->getParam("cvRecovSec")->value().toFloat(), 0.5f, 30.0f);
      settingWrite(NK_cvRecovSec, String(cvRecovSec, 2).c_str());
      queueConsoleMessageF("CV recovery time: %.1f s", cvRecovSec);
    }
    if (request->hasParam("cvRecovEmaxV")) {  // retired timed-window knob — writes the inert global (UI field removed)
      foundParameter = true;
      cvRecovEmaxV = clamp_f(request->getParam("cvRecovEmaxV")->value().toFloat(), 0.05f, 2.0f);
      settingWrite(NK_cvRecovEmaxV, String(cvRecovEmaxV, 3).c_str());
      queueConsoleMessageF("CV recovery error cap: %.2f V", cvRecovEmaxV);
    }
    if (request->hasParam("cvRecovKiMax")) {  // refill Ki multiplier at release, tapering to 1x as the deficit heals
      foundParameter = true;
      cvRecovKiMax = clamp_f(request->getParam("cvRecovKiMax")->value().toFloat(), 1.0f, 10.0f);
      settingWrite(NK_cvRecovKiMax, String(cvRecovKiMax, 2).c_str());
      queueConsoleMessageF("CV recovery refill max rate: %.1fx", cvRecovKiMax);
    }
    if (request->hasParam("cvRecovBoostEnable")) {  // master switch for the post-protection recovery P-boost (1=on, 0=off)
      foundParameter = true;
      cvRecovBoostEnable = (uint8_t)(request->getParam("cvRecovBoostEnable")->value().toInt() ? 1 : 0);
      settingWrite(NK_cvRecovBoostEnable, String((int)cvRecovBoostEnable).c_str());
      queueConsoleMessageF("CV recovery P-boost: %s", cvRecovBoostEnable ? "ON" : "OFF");
    }
    if (request->hasParam("cvRecovBoostMax")) {  // recovery P-boost max multiplier at full shortfall
      foundParameter = true;
      cvRecovBoostMax = clamp_f(request->getParam("cvRecovBoostMax")->value().toFloat(), 1.0f, 8.0f);
      settingWrite(NK_cvRecovBoostMax, String(cvRecovBoostMax, 2).c_str());
      queueConsoleMessageF("CV recovery P-boost max: %.2fx", cvRecovBoostMax);
    }
    if (request->hasParam("cvRecovBoostErrV")) {  // shortfall (V per 12V block) at which the boost reaches max
      foundParameter = true;
      cvRecovBoostErrV = clamp_f(request->getParam("cvRecovBoostErrV")->value().toFloat(), 0.1f, 5.0f);
      settingWrite(NK_cvRecovBoostErrV, String(cvRecovBoostErrV, 3).c_str());
      queueConsoleMessageF("CV recovery P-boost full-boost shortfall: %.2f V", cvRecovBoostErrV);
    }
    if (request->hasParam("cvRecovBoostFloorV")) {  // dead area (V per 12V block): no boost within this shortfall of target
      foundParameter = true;
      cvRecovBoostFloorV = clamp_f(request->getParam("cvRecovBoostFloorV")->value().toFloat(), 0.0f, 5.0f);
      settingWrite(NK_cvRecovBoostFloorV, String(cvRecovBoostFloorV, 3).c_str());
      queueConsoleMessageF("CV recovery P-boost dead area: %.2f V", cvRecovBoostFloorV);
    }
    if (request->hasParam("cvRecovDeepBandV")) {  // deep-recovery band (V per 12V block): beyond this shortfall the walk speeds up and episode boost stays live
      foundParameter = true;
      cvRecovDeepBandV = clamp_f(request->getParam("cvRecovDeepBandV")->value().toFloat(), 0.0f, 5.0f);
      settingWrite(NK_cvRecovDeepBandV, String(cvRecovDeepBandV, 3).c_str());
      queueConsoleMessageF("CV recovery deep band: %.2f V", cvRecovDeepBandV);
    }
    if (request->hasParam("cvRecovDeepMult")) {  // starve-walk rate multiplier at full depth
      foundParameter = true;
      cvRecovDeepMult = clamp_f(request->getParam("cvRecovDeepMult")->value().toFloat(), 1.0f, 20.0f);
      settingWrite(NK_cvRecovDeepMult, String(cvRecovDeepMult, 1).c_str());
      queueConsoleMessageF("CV recovery deep walk multiplier: %.1fx", cvRecovDeepMult);
    }
    if (request->hasParam("cvRecovFlareBandV")) {  // arrival flare band (V per 12V block): recovery ceiling tapers within this shortfall
      foundParameter = true;
      cvRecovFlareBandV = clamp_f(request->getParam("cvRecovFlareBandV")->value().toFloat(), 0.0f, 2.0f);
      settingWrite(NK_cvRecovFlareBandV, String(cvRecovFlareBandV, 3).c_str());
      queueConsoleMessageF("CV recovery arrival flare band: %.2f V", cvRecovFlareBandV);
    }
    if (request->hasParam("cvRecovFlareFrac")) {  // flare ceiling at target, fraction of the recovery goal
      foundParameter = true;
      cvRecovFlareFrac = clamp_f(request->getParam("cvRecovFlareFrac")->value().toFloat(), 0.5f, 1.0f);
      settingWrite(NK_cvRecovFlareFrac, String(cvRecovFlareFrac, 2).c_str());
      queueConsoleMessageF("CV recovery arrival flare floor: %.2f of goal", cvRecovFlareFrac);
    }
    if (request->hasParam("dutySlewEnable")) {  // master switch for field duty slew (1=on, 0=instant)
      foundParameter = true;
      dutySlewEnable = (uint8_t)(request->getParam("dutySlewEnable")->value().toInt() ? 1 : 0);
      settingWrite(NK_dutySlewEnable, String((int)dutySlewEnable).c_str());
      queueConsoleMessageF("Field duty slew limiter: %s", dutySlewEnable ? "ON" : "OFF (instant)");
    }
    if (request->hasParam("testSlewMode")) {  // manual CC test slew: 0=off/instant, 1=default rates, 2=custom
      foundParameter = true;
      int m = request->getParam("testSlewMode")->value().toInt();
      testSlewMode = (uint8_t)(m < 0 ? 0 : (m > 2 ? 2 : m));
      settingWrite(NK_testSlewMode, String((int)testSlewMode).c_str());
      queueConsoleMessageF("Manual test slew mode: %s",
                           testSlewMode == 0 ? "Off (instant)" : testSlewMode == 1 ? "Default rates" : "Custom rates");
    }
    if (request->hasParam("cvTestSlewMode")) {  // manual CV test slew: 0=off/instant target, 1=default rate, 2=custom
      foundParameter = true;
      int m = request->getParam("cvTestSlewMode")->value().toInt();
      cvTestSlewMode = (uint8_t)(m < 0 ? 0 : (m > 2 ? 2 : m));
      settingWrite(NK_cvTestSlewMode, String((int)cvTestSlewMode).c_str());
      queueConsoleMessageF("Manual CV test slew mode: %s",
                           cvTestSlewMode == 0 ? "Off (instant)" : cvTestSlewMode == 1 ? "Default rate" : "Custom rate");
    }
    if (request->hasParam("vTgtRampUp")) {  // CV voltage-target ramp UP rate (V/s); 0 = instant
      foundParameter = true;
      vTgtRampUp = clamp_f(request->getParam("vTgtRampUp")->value().toFloat(), 0.0f, 5.0f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f));
      settingWrite(NK_vTgtRampUp, String(vTgtRampUp, 3).c_str());
      queueConsoleMessageF("CV target ramp up: %.3f V/s", vTgtRampUp);
    }
    if (request->hasParam("vTgtRampDn")) {  // CV voltage-target ramp DOWN rate (V/s); 0 = instant
      foundParameter = true;
      vTgtRampDn = clamp_f(request->getParam("vTgtRampDn")->value().toFloat(), 0.0f, 5.0f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f));
      settingWrite(NK_vTgtRampDn, String(vTgtRampDn, 3).c_str());
      queueConsoleMessageF("CV target ramp down: %.3f V/s", vTgtRampDn);
    }
    if (request->hasParam("cvWindDownEnable")) {  // commanded-target wind-down governor master switch
      foundParameter = true;
      cvWindDownEnable = (uint8_t)(request->getParam("cvWindDownEnable")->value().toInt() != 0);
      settingWrite(NK_cvWindDownEn, String((int)cvWindDownEnable).c_str());
      queueConsoleMessageF("CV target wind-down: %s", cvWindDownEnable ? "enabled" : "disabled");
    }
    if (request->hasParam("cvWindDownRate")) {  // fraction of MaxTableValue shed per second
      foundParameter = true;
      cvWindDownRate = clamp_f(request->getParam("cvWindDownRate")->value().toFloat(), 0.005f, 1.0f);
      settingWrite(NK_cvWindDownRate, String(cvWindDownRate, 3).c_str());
      queueConsoleMessageF("CV wind-down rate: %.3f of max amps/s", cvWindDownRate);
    }
    if (request->hasParam("cvWindDownStopV")) {  // V real per-bus — stop margin above the commanded target
      foundParameter = true;
      cvWindDownStopV = clamp_f(request->getParam("cvWindDownStopV")->value().toFloat(), 0.0f, 0.3f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f));
      settingWrite(NK_cvWindDownStopV, String(cvWindDownStopV, 3).c_str());
      queueConsoleMessageF("CV wind-down stop margin: %.3f V", cvWindDownStopV);
    }
    // Plant curve K(t) = Ka + Kb·√t. The fit step writes both; a bench hand-entry may send cvPlantK alone,
    // which means "flat curve" (Kb = 0) — the old single-point behaviour.
    if (request->hasParam("cvPlantK")) {   // alias: flat curve at the given gain
      foundParameter = true;
      cvPlantKa = request->getParam("cvPlantK")->value().toFloat();
      cvPlantKb = 0.0f;
      settingWrite(NK_cvPlantKa, String(cvPlantKa, 5).c_str());
      settingWrite(NK_cvPlantKb, "0.00000");
      recomputeCvGains();
    }
    if (request->hasParam("cvPlantKa")) {
      foundParameter = true;
      cvPlantKa = request->getParam("cvPlantKa")->value().toFloat();
      settingWrite(NK_cvPlantKa, String(cvPlantKa, 5).c_str());
      // Stamp the battery temperature and its source at the moment the curve was measured — the reference for
      // the battery-temp gain derate (computeCvTempScale compares source classes, measured vs board). The
      // batteryTempF() result of the current tick when it has one; the live board reading otherwise (source 5);
      // neither → leave the prior stamp (no false reference).
      if (isfinite(battTempActiveF)) {
        CommissionTempF = battTempActiveF;
        CommissionTempSrc = (int)battTempActiveSrc;
        settingWrite(NK_CommissionTempF, String(CommissionTempF, 2).c_str());
        settingWrite(NK_CommissionTempSrc, String(CommissionTempSrc).c_str());
      } else if (!IS_STALE(IDX_AMBIENT_TEMP) && isfinite(ambientTemp)) {
        CommissionTempF = ambientTemp;
        CommissionTempSrc = 5;
        settingWrite(NK_CommissionTempF, String(CommissionTempF, 2).c_str());
        settingWrite(NK_CommissionTempSrc, String(CommissionTempSrc).c_str());
      }
      recomputeCvGains();
    }
    if (request->hasParam("cvPlantKb")) {
      foundParameter = true;
      cvPlantKb = fmaxf(0.0f, request->getParam("cvPlantKb")->value().toFloat());   // a falling tail is never physical
      settingWrite(NK_cvPlantKb, String(cvPlantKb, 5).c_str());
      recomputeCvGains();
    }
    // Measured ripple projection (§3.3): the browser fits ripple(I)=a0+a1·I from the 3-level current-check
    // points and POSTs the whole record as a CSV string. Reference data for the Protections
    // ripple-vs-threshold plot ONLY — never touches the over-current floor.
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
    if (request->hasParam("resTest")) {
      foundParameter = true;
      bool arm = (request->getParam("resTest")->value().toInt() != 0);
      if (arm && (cvStressActive || protTestActive != 0)) {
        queueConsoleMessageF("Resonance current-check: start blocked — %s is active",
                             cvStressActive ? "CV stress test" : "Protection test");
      } else if (arm) { resTestActive = true; resTestReleasing = false; }
      // Release = DEFERRED: keep the field under test-control (OV still suppressed) and let the loop wind the
      // current down to ~0 first; it drops resTestActive itself once the field is down (6_functions.ino), so
      // CV re-enters at low voltage and slews up from below rather than slamming G2 on the transition edge.
      else if (resTestActive) { resTestReleasing = true; resTestTargetA = 0.0f; }
      resTestLastCmdMs = millis();
      queueConsoleMessageF("Resonance current-check %s", arm ? "ARMED (wizard commands current)" : "winding down");
    }
    // The `else` is load-bearing: a batched resTest=0&resTestTargetA=N would otherwise zero the target in the
    // disarm above and then command N amps back into a field that was asked to wind down. Arm/disarm wins.
    else if (request->hasParam("resTestTargetA")) {
      foundParameter = true;
      resTestTargetA = request->getParam("resTestTargetA")->value().toFloat();
      resTestLastCmdMs = millis();  // keepalive refresh (deadman)
    }
    if (request->hasParam("CvKdDeadbandVps")) {
      foundParameter = true;
      inputMessage = request->getParam("CvKdDeadbandVps")->value();
      CvKdDeadbandVps = inputMessage.toFloat();
      settingWrite(NK_CvKdDeadbandVps, String(CvKdDeadbandVps, 3).c_str());
      queueConsoleMessageF("CV D-term deadband base: %.3f V/s", CvKdDeadbandVps);
    }
    if (request->hasParam("CvKdDbSlope")) {
      foundParameter = true;
      inputMessage = request->getParam("CvKdDbSlope")->value();
      CvKdDbSlope = inputMessage.toFloat();
      settingWrite(NK_CvKdDbSlope, String(CvKdDbSlope, 5).c_str());
      queueConsoleMessageF("CV D-term deadband slope: %.5f V/s per A", CvKdDbSlope);
    }
    if (request->hasParam("CvKdDbFloor")) {
      foundParameter = true;
      inputMessage = request->getParam("CvKdDbFloor")->value();
      CvKdDbFloor = inputMessage.toFloat();
      settingWrite(NK_CvKdDbFloor, String(CvKdDbFloor, 3).c_str());
      queueConsoleMessageF("CV D-term deadband floor: %.3f V/s", CvKdDbFloor);
    }
    if (request->hasParam("CvKdDbCeil")) {
      foundParameter = true;
      inputMessage = request->getParam("CvKdDbCeil")->value();
      CvKdDbCeil = inputMessage.toFloat();
      settingWrite(NK_CvKdDbCeil, String(CvKdDbCeil, 3).c_str());
      queueConsoleMessageF("CV D-term deadband ceiling: %.3f V/s", CvKdDbCeil);
    }
    if (request->hasParam("CvKdOneSided")) {
      foundParameter = true;
      inputMessage = request->getParam("CvKdOneSided")->value();
      CvKdOneSided = inputMessage.toInt() != 0;
      settingWrite(NK_CvKdOneSided, String((int)CvKdOneSided).c_str());
      queueConsoleMessageF("CV D-term mode: %s", CvKdOneSided ? "ONE-SIDED (removes current only)" : "SYMMETRIC (adds on falling bus)");
    }
    if (request->hasParam("CvKdExcessMode")) {
      foundParameter = true;
      inputMessage = request->getParam("CvKdExcessMode")->value();
      CvKdExcessMode = inputMessage.toInt() != 0;
      settingWrite(NK_CvKdExcessMode, String((int)CvKdExcessMode).c_str());
      queueConsoleMessageF("CV D-term response: %s", CvKdExcessMode ? "GRADUAL (slope excess over tolerance line)" : "STEPPED (legacy full-slope latch)");
    }
    if (request->hasParam("VoltageKd")) {
      foundParameter = true;
      inputMessage = request->getParam("VoltageKd")->value();
      VoltageKd = inputMessage.toFloat();
      settingWrite(NK_VoltageKd, String(VoltageKd, 1).c_str());
      recomputeCvGains();  // manual gain changed → refresh the active gain
      queueConsoleMessageF("CV D-term gain: %.1f A/(V/s)", VoltageKd);
    }
    if (request->hasParam("CvKdVoltFiltTC")) {
      foundParameter = true;
      inputMessage = request->getParam("CvKdVoltFiltTC")->value();
      CvKdVoltFiltTC = inputMessage.toFloat();
      settingWrite(NK_CvKdVoltFiltTC, String(CvKdVoltFiltTC, 0).c_str());
      queueConsoleMessageF("CV D-term voltage filter TC: %.0f ms", CvKdVoltFiltTC);
    }
    if (request->hasParam("cvHelpersEnabled")) {
      foundParameter = true;
      inputMessage = request->getParam("cvHelpersEnabled")->value();
      cvHelpersEnabled = inputMessage.toInt() != 0;
      settingWrite(NK_cvHelpersEnabled, String((int)cvHelpersEnabled).c_str());
      queueConsoleMessageF("CV tuning helpers (asymmetric unwind + D term): %s", cvHelpersEnabled ? "ENABLED" : "DISABLED");
    }
    if (request->hasParam("CvKdArmV")) {
      foundParameter = true;
      inputMessage = request->getParam("CvKdArmV")->value();
      CvKdArmV = inputMessage.toFloat();
      settingWrite(NK_CvKdArmV, String(CvKdArmV, 2).c_str());
      queueConsoleMessageF("CV D-term arm window: %.2f V", CvKdArmV);
    }
    if (request->hasParam("CvKdMaxTrimA")) {
      foundParameter = true;
      inputMessage = request->getParam("CvKdMaxTrimA")->value();
      CvKdMaxTrimA = fmaxf(0.0f, inputMessage.toFloat());
      settingWrite(NK_CvKdMaxTrimA, String(CvKdMaxTrimA, 1).c_str());
      queueConsoleMessageF("CV D-term max back-off: %.1f A", CvKdMaxTrimA);
    }
    if (request->hasParam("CvKdSlopeCeil")) {
      foundParameter = true;
      inputMessage = request->getParam("CvKdSlopeCeil")->value();
      CvKdSlopeCeil = clamp_f(inputMessage.toFloat(), 1.0f*((float)SYSTEM_VOLTAGE_CLASS/12.0f), 20.0f*((float)SYSTEM_VOLTAGE_CLASS/12.0f));  // real per-bus — bounds scale with class like the input widget
      settingWrite(NK_CvKdSlopeCeil, String(CvKdSlopeCeil, 1).c_str());
      queueConsoleMessageF("CV D-term slope ceiling: %.1f V/s", CvKdSlopeCeil);
    }
    if (request->hasParam("CvKdTd")) {  // CV D derivative time — Auto-mode Kd = Td·Kp
      foundParameter = true;
      inputMessage = request->getParam("CvKdTd")->value();
      CvKdTd = clamp_f(inputMessage.toFloat(), 0.0f, 3.0f);
      settingWrite(NK_CvKdTd, String(CvKdTd, 2).c_str());
      recomputeCvGains();  // Td feeds the auto Kd
      queueConsoleMessageF("CV D-term time Td: %.2f s (Auto Kd = Td x Kp)", CvKdTd);
    }
    if (request->hasParam("TempPIDKp")) {
      foundParameter = true;
      inputMessage = request->getParam("TempPIDKp")->value();
      settingWrite(NK_TempPIDKp, inputMessage.c_str());
      TempPIDKp = inputMessage.toFloat();
      // Velocity form reads TempPIDKp directly each tick — no library object to retune.
      queueConsoleMessageF("Temp PID Kp updated to: %.6f", TempPIDKp);
    }
    if (request->hasParam("TempPIDKi")) {
      foundParameter = true;
      inputMessage = request->getParam("TempPIDKi")->value();
      settingWrite(NK_TempPIDKi, inputMessage.c_str());
      TempPIDKi = inputMessage.toFloat();
      // Velocity form reads TempPIDKi directly each tick — no library object to retune.
      queueConsoleMessageF("Temp PID Ki updated to: %.6f", TempPIDKi);
    }
    if (request->hasParam("TempPIDKiDownFrac")) {
      foundParameter = true;
      inputMessage = request->getParam("TempPIDKiDownFrac")->value();
      TempPIDKiDownFrac = constrain(inputMessage.toFloat(), 0.0f, 1.0f);  // ratio of Ki used to bleed below setpoint
      settingWrite(NK_TempPIDKiDownFrac, String(TempPIDKiDownFrac, 3).c_str());
      queueConsoleMessageF("Temp PID below-setpoint bleed ratio set to: %.2f x Ki", TempPIDKiDownFrac);
    }
    if (request->hasParam("ThermalLookaheadSec")) {
      foundParameter = true;
      inputMessage = request->getParam("ThermalLookaheadSec")->value();
      ThermalLookaheadSec = clamp_f(inputMessage.toFloat(), 0.0f, 300.0f);
      settingWrite(NK_ThermalLookaheadSec, String(ThermalLookaheadSec, 1).c_str());      queueConsoleMessageF("ThermalLookaheadSec set to: %.1f s", ThermalLookaheadSec);
    }
    if (request->hasParam("ThermalSlopeWindowSec")) {
      foundParameter = true;
      inputMessage = request->getParam("ThermalSlopeWindowSec")->value();
      ThermalSlopeWindowSec = clamp_f(inputMessage.toFloat(), 10.0f, 60.0f);  // slope backward-difference window
      settingWrite(NK_ThermalSlopeWindowSec, String(ThermalSlopeWindowSec, 1).c_str());
      queueConsoleMessageF("ThermalSlopeWindowSec set to: %.1f s", ThermalSlopeWindowSec);
    }
    if (request->hasParam("TempPIDIntervalMs")) {
      foundParameter = true;
      inputMessage = request->getParam("TempPIDIntervalMs")->value();
      settingWrite(NK_TempPIDIntervalMs, inputMessage.c_str());
      TempPIDIntervalMs = inputMessage.toInt();      queueConsoleMessageF("Temp PID interval updated to: %d ms", TempPIDIntervalMs);
    }
    if (request->hasParam("TempPIDFilterAlpha")) {
      foundParameter = true;
      inputMessage = request->getParam("TempPIDFilterAlpha")->value();
      settingWrite(NK_TempPIDFilterAlpha, inputMessage.c_str());
      TempPIDFilterAlpha = inputMessage.toFloat();      queueConsoleMessageF("Temp PID filter alpha updated to: %.3f", TempPIDFilterAlpha);
    }
    if (request->hasParam("PidKi")) {
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
    if (request->hasParam("PidKd")) {
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
    if (request->hasParam("PidSampleDivisor")) {
      foundParameter = true;
      inputMessage = request->getParam("PidSampleDivisor")->value();
      settingWrite(NK_PidSampleDivisor, inputMessage.c_str());
      PidSampleDivisor = inputMessage.toInt();
      if (TuningMode) tuningParamChanged = true;
    }
    if (request->hasParam("LearningSettlingPeriod")) {
      foundParameter = true;
      inputMessage = request->getParam("LearningSettlingPeriod")->value();
      int temp = inputMessage.toInt() * 1000;  // Convert seconds to ms
      settingWrite(NK_LearningSettlingPeriod, String(temp).c_str());
      LearningSettlingPeriod = temp;
    }
    if (request->hasParam("LearningRPMChangeThreshold")) {
      foundParameter = true;
      inputMessage = request->getParam("LearningRPMChangeThreshold")->value();
      settingWrite(NK_LearningRPMChangeThreshold, inputMessage.c_str());
      LearningRPMChangeThreshold = inputMessage.toInt();
    }
    if (request->hasParam("LearningTempHysteresis")) {
      foundParameter = true;
      inputMessage = request->getParam("LearningTempHysteresis")->value();
      settingWrite(NK_LearningTempHysteresis, inputMessage.c_str());
      LearningTempHysteresis = inputMessage.toInt();
    }
    // "Group 0" in UI = hardware overcurrent trip (no protection-group integration yet)
    if (request->hasParam("MaxTableValue")) {
      foundParameter = true;
      inputMessage = request->getParam("MaxTableValue")->value();
      settingWrite(NK_MaxTableValue, inputMessage.c_str());
      MaxTableValue = inputMessage.toFloat();
      HardOCTripAmps = MaxTableValue + 10.0f;  // always 10A above current limit
      queueConsoleMessageF("Alternator current limit set to %.1fA — OC trip threshold: %.1fA", MaxTableValue, HardOCTripAmps);
    }
    if (request->hasParam("MaxPenaltyPercent")) {
      foundParameter = true;
      inputMessage = request->getParam("MaxPenaltyPercent")->value();
      settingWrite(NK_MaxPenaltyPercent, inputMessage.c_str());
      MaxPenaltyPercent = inputMessage.toFloat();
    }
    if (request->hasParam("MaxPenaltyDuration")) {
      foundParameter = true;
      inputMessage = request->getParam("MaxPenaltyDuration")->value();
      int temp = inputMessage.toInt() * 1000;
      settingWrite(NK_MaxPenaltyDuration, String(temp).c_str());
      MaxPenaltyDuration = temp;
    }
    if (request->hasParam("NeighborLearningFactor")) {
      foundParameter = true;
      inputMessage = request->getParam("NeighborLearningFactor")->value();
      settingWrite(NK_NeighborLearningFactor, inputMessage.c_str());
      NeighborLearningFactor = inputMessage.toFloat();
    }
    if (request->hasParam("yyMax")) {
      foundParameter = true;
      inputMessage = request->getParam("yyMax")->value();
      settingWrite(NK_yyMax, inputMessage.c_str());
      yyMax = inputMessage.toInt();
    }
    if (request->hasParam("LearningMemoryDuration")) {
      foundParameter = true;
      inputMessage = request->getParam("LearningMemoryDuration")->value();
      settingWrite(NK_LearningMemoryDuration, inputMessage.c_str());
      LearningMemoryDuration = inputMessage.toInt();
    }
    if (request->hasParam("DutyRampRate")) {
      foundParameter = true;
      inputMessage = request->getParam("DutyRampRate")->value();
      settingWrite(NK_DutyRampRate, inputMessage.c_str());
      DutyRampRate = inputMessage.toFloat();
      if (TuningMode) tuningParamChanged = true;
      queueConsoleMessageF("Duty ramp rate set to: %.1f %%/sec", DutyRampRate);
    }
    if (request->hasParam("DutySlowRampRate")) {
      foundParameter = true;
      inputMessage = request->getParam("DutySlowRampRate")->value();
      settingWrite(NK_DutySlowRampRate, inputMessage.c_str());
      DutySlowRampRate = inputMessage.toFloat();
      queueConsoleMessageF("Shutdown slow ramp rate set to: %.2f %%/s", DutySlowRampRate);
    }
    if (request->hasParam("ShutdownPhase2HoldMs")) {
      foundParameter = true;
      inputMessage = request->getParam("ShutdownPhase2HoldMs")->value();
      uint32_t ms = (uint32_t)inputMessage.toInt();
      settingWrite(NK_ShutdownPhase2HoldMs, String(ms).c_str());
      ShutdownPhase2HoldMs = ms;
      queueConsoleMessageF("Shutdown phase 2 hold set to: %u ms", ShutdownPhase2HoldMs);
    }
    if (request->hasParam("SettleTimeBeforeCut")) {
      foundParameter = true;
      inputMessage = request->getParam("SettleTimeBeforeCut")->value();
      settingWrite(NK_SettleTimeBeforeCut, inputMessage.c_str());
      SettleTimeBeforeCut = inputMessage.toInt();
      queueConsoleMessageF("Settle time before cut set to: %d ms", SettleTimeBeforeCut);
    }
    if (request->hasParam("TempWarnExcess")) {
      foundParameter = true;
      inputMessage = request->getParam("TempWarnExcess")->value();
      settingWrite(NK_TempWarnExcess, inputMessage.c_str());
      TempWarnExcess = inputMessage.toFloat();
      queueConsoleMessageF("Temp warning threshold set to: +%.1f%s above limit", dispTempDeltaF(TempWarnExcess), dispTempUnit());
    }
    if (request->hasParam("TempCritExcess")) {
      foundParameter = true;
      inputMessage = request->getParam("TempCritExcess")->value();
      settingWrite(NK_TempCritExcess, inputMessage.c_str());
      TempCritExcess = inputMessage.toFloat();
      queueConsoleMessageF("Temp critical threshold set to: +%.1f%s above limit", dispTempDeltaF(TempCritExcess), dispTempUnit());
    }
    if (request->hasParam("TempSustainedTimeout")) {
      foundParameter = true;
      inputMessage = request->getParam("TempSustainedTimeout")->value();
      int temp = inputMessage.toInt() * 1000;  // user enters seconds
      settingWrite(NK_TempSustainedTimeout, String(temp).c_str());
      TempSustainedTimeout = temp;
      queueConsoleMessageF("Temp sustained timeout set to: %d seconds", inputMessage.toInt());
    }
    // Both OV-ladder rungs can arrive in ONE request (battery-defaults proposal, Other-chemistry
    // prep), so read both first and enforce the ladder order ONCE — clamping one at a time
    // against the value being replaced would corrupt a joint move in either direction. The
    // hardware rung is the anchor; the software cut must sit strictly below it (0.05 x class/12
    // guard band) so software always gets first shot.
    bool swRungPresent = request->hasParam("AlternatorHardShutdownV");
    bool hwRungPresent = request->hasParam("VoltageHardwareLimit");
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
        updateINA228OvervoltageThreshold();  // reprogram the comparator immediately — never leave it stale
        queueConsoleMessageF("Hardware shutdown voltage set to: %.2fV (INA228 ALERT, absolute)", VoltageHardwareLimit);
      }
      if (swClamped)
        queueConsoleMessageF("Alternator hard-shutdown clamped to %.2fV — must stay below the Hardware Shutdown Voltage (%.2fV)", AlternatorHardShutdownV, VoltageHardwareLimit);
      else if (swRungPresent)
        queueConsoleMessageF("Alternator hard-shutdown voltage set to: %.2fV (absolute)", AlternatorHardShutdownV);
    }
    // "Group 0" in UI = hardware overcurrent trip (no protection-group integration yet)
    if (request->hasParam("HardOCDebounceMs")) {
      foundParameter = true;
      inputMessage = request->getParam("HardOCDebounceMs")->value();
      settingWrite(NK_HardOCDebounceMs, inputMessage.c_str());
      HardOCDebounceMs = (uint32_t)inputMessage.toInt();
      queueConsoleMessageF("Overcurrent trip debounce set to: %ums", HardOCDebounceMs);
    }
    if (request->hasParam("WarmupRampRate")) {
      foundParameter = true;
      inputMessage = request->getParam("WarmupRampRate")->value();
      WarmupRampRate = max(0.0f, inputMessage.toFloat());
      settingWrite(NK_WarmupRampRate, String(WarmupRampRate, 2).c_str());
      queueConsoleMessageF("Warmup ramp rate set to: %.2f A/s", WarmupRampRate);
    }
    if (request->hasParam("IExcessFrac")) {
      foundParameter = true;
      inputMessage = request->getParam("IExcessFrac")->value();  // UI sends percent; stored as fraction
      IExcessFrac = constrain(inputMessage.toFloat() / 100.0f, 0.02f, 0.50f);
      settingWrite(NK_IExcessFrac, String(IExcessFrac, 3).c_str());
      queueConsoleMessageF("IExcess CV threshold set to: %.0f%% of command", IExcessFrac * 100.0f);
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    if (request->hasParam("IExcessFracBulk")) {
      foundParameter = true;
      inputMessage = request->getParam("IExcessFracBulk")->value();  // UI sends percent; stored as fraction
      IExcessFracBulk = constrain(inputMessage.toFloat() / 100.0f, 0.02f, 0.60f);
      settingWrite(NK_IExcessFracBulk, String(IExcessFracBulk, 3).c_str());
      queueConsoleMessageF("IExcess bulk threshold set to: %.0f%% of ceiling", IExcessFracBulk * 100.0f);
    }
    if (request->hasParam("IExcessFloorA")) {
      foundParameter = true;
      inputMessage = request->getParam("IExcessFloorA")->value();
      IExcessFloorA = constrain(inputMessage.toFloat(), 1.0f, 20.0f);
      settingWrite(NK_IExcessFloorA, String(IExcessFloorA, 1).c_str());
      queueConsoleMessageF("IExcess threshold floor set to: %.1fA", IExcessFloorA);
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    if (request->hasParam("IExcessCeilA")) {
      foundParameter = true;
      inputMessage = request->getParam("IExcessCeilA")->value();
      IExcessCeilA = constrain(inputMessage.toFloat(), 5.0f, 80.0f);
      settingWrite(NK_IExcessCeilA, String(IExcessCeilA, 1).c_str());
      queueConsoleMessageF("IExcess threshold ceiling set to: %.1fA", IExcessCeilA);
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    if (request->hasParam("IExcessBaseA")) {
      foundParameter = true;
      inputMessage = request->getParam("IExcessBaseA")->value();
      IExcessBaseA = constrain(inputMessage.toFloat(), 0.0f, 40.0f);
      settingWrite(NK_IExcessBaseA, String(IExcessBaseA, 1).c_str());
      queueConsoleMessageF("IExcess trip-line base set to: %.1fA", IExcessBaseA);
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    if (request->hasParam("IExcessCcOffsetA")) {
      foundParameter = true;
      inputMessage = request->getParam("IExcessCcOffsetA")->value();
      IExcessCcOffsetA = constrain(inputMessage.toFloat(), 0.0f, 40.0f);
      settingWrite(NK_IExcessCcOffsetA, String(IExcessCcOffsetA, 1).c_str());
      queueConsoleMessageF("IExcess CC offset set to: %.1fA above CV", IExcessCcOffsetA);
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    // Max battery charge current (G4). Ceiling on the alternator command = limit + measured
    // house-load offset; requires the INA228 battery shunt. 0 disables the feature.
    if (request->hasParam("BattCurrentLimitA")) {
      foundParameter = true;
      inputMessage = request->getParam("BattCurrentLimitA")->value();
      BattCurrentLimitA = constrain(inputMessage.toFloat(), 0.0f, 500.0f);
      settingWrite(NK_BattCurrentLimitA, String(BattCurrentLimitA, 1).c_str());
      if (BattCurrentLimitA > 0.0f) queueConsoleMessageF("Battery charge current limit set to: %.1fA", BattCurrentLimitA);
      else queueConsoleMessage("Battery charge current limit disabled");
    }
    if (request->hasParam("IExcessTau")) {
      foundParameter = true;
      inputMessage = request->getParam("IExcessTau")->value();
      float iexTauPrev = IExcessTau;
      IExcessTau = constrain(inputMessage.toFloat(), 20.0f, 300.0f);
      settingWrite(NK_IExcessTau, String(IExcessTau, 1).c_str());
      queueConsoleMessageF("IExcess averaging TC set to: %.0f ms", IExcessTau);
      // The a0+a1·I fit is the pk-pk of THIS low-pass, so a tau change invalidates it. The slope fit
      // is not measured through this filter and survives; the Resonance & Ripple Map never was
      // (raw stream, own fixed window) — see ripFitForget.
      if (fabsf(IExcessTau - iexTauPrev) > 0.05f) ripFitForget(false, "IExcess averaging TC changed");
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    if (request->hasParam("IExcessRelFrac")) {
      foundParameter = true;
      inputMessage = request->getParam("IExcessRelFrac")->value();  // UI sends percent; stored as fraction
      IExcessRelFrac = constrain(inputMessage.toFloat() / 100.0f, 0.1f, 0.9f);
      settingWrite(NK_IExcessRelFrac, String(IExcessRelFrac, 3).c_str());
      queueConsoleMessageF("IExcess release hysteresis set to: %.0f%% of threshold", IExcessRelFrac * 100.0f);
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    if (request->hasParam("IExcessKBleed")) {
      foundParameter = true;
      inputMessage = request->getParam("IExcessKBleed")->value();
      IExcessKBleed = inputMessage.toFloat();
      settingWrite(NK_IExcessKBleed, String(IExcessKBleed, 2).c_str());
      queueConsoleMessageF("K_bleed set to: %.2f A/s per A", IExcessKBleed);
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    if (request->hasParam("IExcessArmMarginV")) {
      foundParameter = true;
      inputMessage = request->getParam("IExcessArmMarginV")->value();
      IExcessArmMarginV = constrain(inputMessage.toFloat(), 0.020f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f), 5.000f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f));
      settingWrite(NK_IExcessArmMarginV, String(IExcessArmMarginV, 3).c_str());
      queueConsoleMessageF("iExcess arming margin set to: %.0f mV below target", IExcessArmMarginV * 1000.0f);
    }
    if (request->hasParam("AwBleedRate")) {
      foundParameter = true;
      inputMessage = request->getParam("AwBleedRate")->value();
      AwBleedRate = inputMessage.toFloat();
      settingWrite(NK_AwBleedRate, String(AwBleedRate, 1).c_str());
      queueConsoleMessageF("AW bleed rate set to: %.1f A/s", AwBleedRate);
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    // No AwRecoverRate handler — hardcoded in firmware (0.1f)
    if (request->hasParam("FastSetpointRiseRate")) {
      foundParameter = true;
      inputMessage = request->getParam("FastSetpointRiseRate")->value();
      FastSetpointRiseRate = constrain(inputMessage.toFloat(), 1.0f, 50.0f);
      settingWrite(NK_FastSetpointRiseRate, String(FastSetpointRiseRate, 1).c_str());
      queueConsoleMessageF("Fast setpoint rise rate set to: %.1fx", FastSetpointRiseRate);
    }
    if (request->hasParam("FastSetpointRiseWindowMs")) {
      foundParameter = true;
      inputMessage = request->getParam("FastSetpointRiseWindowMs")->value();
      FastSetpointRiseWindowMs = (uint32_t)constrain(inputMessage.toInt(), 500, 30000);
      settingWrite(NK_FastSetpointRiseWindowMs, String(FastSetpointRiseWindowMs).c_str());
      queueConsoleMessageF("Fast rise window set to: %ums", FastSetpointRiseWindowMs);
    }
    if (request->hasParam("FastSetpointRiseHeadroomV")) {
      foundParameter = true;
      inputMessage = request->getParam("FastSetpointRiseHeadroomV")->value();
      FastSetpointRiseHeadroomV = constrain(inputMessage.toFloat(), 0.05f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f), 2.0f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f));
      settingWrite(NK_FastSetpointRiseHeadroomV, String(FastSetpointRiseHeadroomV, 2).c_str());
      queueConsoleMessageF("Fast rise headroom set to: %.2fV", FastSetpointRiseHeadroomV);
    }
    if (request->hasParam("AwSeedProtectMs")) {
      foundParameter = true;
      inputMessage = request->getParam("AwSeedProtectMs")->value();
      AwSeedProtectMs = (uint16_t)constrain(inputMessage.toInt(), 0, 2000);
      settingWrite(NK_AwSeedProtectMs, String(AwSeedProtectMs).c_str());
      queueConsoleMessageF("AW seed protect window set to: %u ms", (unsigned)AwSeedProtectMs);
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    if (request->hasParam("KHard")) {
      foundParameter = true;
      inputMessage = request->getParam("KHard")->value();
      KHard = inputMessage.toFloat();
      settingWrite(NK_KHard, String(KHard, 1).c_str());
      queueConsoleMessageF("KHard set to: %.1f A/V", KHard);
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    if (request->hasParam("OvGroup1Enable")) {
      foundParameter = true;
      inputMessage = request->getParam("OvGroup1Enable")->value();
      OvGroup1Enable = inputMessage.toInt() != 0;
      settingWrite(NK_OvGroup1Enable, String((int)OvGroup1Enable).c_str());
      queueConsoleMessageF("OV Group 1 (prediction-based cap): %s", OvGroup1Enable ? "ENABLED" : "DISABLED");
    }
    if (request->hasParam("OvGroup2Enable")) {
      foundParameter = true;
      inputMessage = request->getParam("OvGroup2Enable")->value();
      OvGroup2Enable = inputMessage.toInt() != 0;
      settingWrite(NK_OvGroup2Enable, String((int)OvGroup2Enable).c_str());
      queueConsoleMessageF("OV Group 2 (measured-voltage threshold): %s", OvGroup2Enable ? "ENABLED" : "DISABLED");
    }
    if (request->hasParam("TachLieEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("TachLieEnable")->value();
      TachLieEnable = inputMessage.toInt() != 0;
      settingWrite(NK_TachLieEnable, String((int)TachLieEnable).c_str());
      queueConsoleMessageF("Tach plausibility cut (commanded current, zero output): %s", TachLieEnable ? "ENABLED" : "DISABLED");
    }
    if (request->hasParam("HardOCEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("HardOCEnable")->value();
      HardOCEnable = inputMessage.toInt() != 0;
      settingWrite(NK_HardOCEnable, String((int)HardOCEnable).c_str());
      queueConsoleMessageF("Group 0 (hard over-current trip): %s", HardOCEnable ? "ENABLED" : "DISABLED");
    }
    if (request->hasParam("IExcessEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("IExcessEnable")->value();
      IExcessEnable = inputMessage.toInt() != 0;
      settingWrite(NK_IExcessEnable, String((int)IExcessEnable).c_str());
      queueConsoleMessageF("Group 3 (alternator iExcess detectors): %s", IExcessEnable ? "ENABLED" : "DISABLED");
    }
    if (request->hasParam("BattLimitEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("BattLimitEnable")->value();
      BattLimitEnable = inputMessage.toInt() != 0;
      settingWrite(NK_BattLimitEnable, String((int)BattLimitEnable).c_str());
      queueConsoleMessageF("Group 4 (battery charge current limit): %s", BattLimitEnable ? "ENABLED" : "DISABLED");
    }
    if (request->hasParam("LoadDumpEnable")) {
      foundParameter = true;
      inputMessage = request->getParam("LoadDumpEnable")->value();
      LoadDumpEnable = inputMessage.toInt() != 0;
      settingWrite(NK_LoadDumpEnable, String((int)LoadDumpEnable).c_str());
      queueConsoleMessageF("Group 5 (load dump): %s", LoadDumpEnable ? "ENABLED" : "DISABLED");
    }
    if (request->hasParam("OutputPIDSigSrc")) {
      foundParameter = true;
      inputMessage = request->getParam("OutputPIDSigSrc")->value();
      OutputPIDSigSrc = constrain(inputMessage.toInt(), 0, 2);
      settingWrite(NK_OutputPIDSigSrc, String(OutputPIDSigSrc).c_str());
      const char* sigNames[] = { "EMA(TC)", "MA(N)", "Raw" };
      queueConsoleMessageF("Output PID signal source: %s", sigNames[OutputPIDSigSrc]);
    }
    if (request->hasParam("OutputPIDMA_N")) {
      foundParameter = true;
      inputMessage = request->getParam("OutputPIDMA_N")->value();
      OutputPIDMA_N = constrain(inputMessage.toInt(), 1, I_RING_SIZE);
      settingWrite(NK_OutputPIDMA_N, String(OutputPIDMA_N).c_str());
      queueConsoleMessageF("Output PID MA window: N=%d", OutputPIDMA_N);
    }
    if (request->hasParam("OutputPIDFilterTC")) {
      foundParameter = true;
      inputMessage = request->getParam("OutputPIDFilterTC")->value();
      OutputPIDFilterTC = inputMessage.toFloat();
      settingWrite(NK_OutputPIDFilterTC, String(OutputPIDFilterTC).c_str());
      queueConsoleMessageF("Output PID EMA TC: %.1f ms", OutputPIDFilterTC);
    }
    if (request->hasParam("VoltageFilterTC")) {
      foundParameter = true;
      inputMessage = request->getParam("VoltageFilterTC")->value();
      VoltageFilterTC = inputMessage.toFloat();
      settingWrite(NK_VoltageFilterTC, String(VoltageFilterTC).c_str());
      queueConsoleMessageF("Voltage EMA TC: %.1f ms", VoltageFilterTC);
    }
    if (request->hasParam("TdPred")) {
      foundParameter = true;
      inputMessage = request->getParam("TdPred")->value();
      TdPred = constrain(inputMessage.toFloat(), 0.01f, 0.30f);
      settingWrite(NK_TdPred, String(TdPred, 3).c_str());
      queueConsoleMessageF("OV prediction horizon set to: %.3f s", TdPred);
    }
    if (request->hasParam("OvMeasMarginV")) {
      foundParameter = true;
      inputMessage = request->getParam("OvMeasMarginV")->value();
      OvMeasMarginV = constrain(inputMessage.toFloat(), 0.020f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f), 0.500f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f));
      settingWrite(NK_OvMeasMarginV, String(OvMeasMarginV, 3).c_str());
      queueConsoleMessageF("Group 2 measured-voltage trigger margin set to: %.0f mV", OvMeasMarginV * 1000.0f);
    }
    if (request->hasParam("OvPredMarginV")) {
      foundParameter = true;
      inputMessage = request->getParam("OvPredMarginV")->value();
      OvPredMarginV = constrain(inputMessage.toFloat(), 0.050f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f), 1.000f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f));
      settingWrite(NK_OvPredMarginV, String(OvPredMarginV, 3).c_str());
      queueConsoleMessageF("Group 1 prediction trigger margin set to: %.0f mV", OvPredMarginV * 1000.0f);
    }
    if (request->hasParam("OvTierLoMarginV")) {
      foundParameter = true;
      inputMessage = request->getParam("OvTierLoMarginV")->value();
      OvTierLoMarginV = constrain(inputMessage.toFloat(), 0.020f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f), 2.000f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f));
      settingWrite(NK_OvTierLoMarginV, String(OvTierLoMarginV, 3).c_str());
      queueConsoleMessageF("Timed OV cut LOW-tier margin set to: %.0f mV above target", OvTierLoMarginV * 1000.0f);
    }
    if (request->hasParam("OvTierLoDwellMs")) {
      foundParameter = true;
      inputMessage = request->getParam("OvTierLoDwellMs")->value();
      OvTierLoDwellMs = (uint32_t)constrain(inputMessage.toInt(), 0, 5000);
      settingWrite(NK_OvTierLoDwellMs, String(OvTierLoDwellMs).c_str());
      if (OvTierLoDwellMs == 0) queueConsoleMessage("Timed OV cut LOW tier DISABLED (dwell 0)");
      else queueConsoleMessageF("Timed OV cut LOW-tier dwell set to: %ums", OvTierLoDwellMs);
    }
    if (request->hasParam("OvTierMidMarginV")) {
      foundParameter = true;
      inputMessage = request->getParam("OvTierMidMarginV")->value();
      OvTierMidMarginV = constrain(inputMessage.toFloat(), 0.020f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f), 2.000f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f));
      settingWrite(NK_OvTierMidMarginV, String(OvTierMidMarginV, 3).c_str());
      queueConsoleMessageF("Timed OV cut MID-tier margin set to: %.0f mV above target", OvTierMidMarginV * 1000.0f);
    }
    if (request->hasParam("OvTierMidDwellMs")) {
      foundParameter = true;
      inputMessage = request->getParam("OvTierMidDwellMs")->value();
      OvTierMidDwellMs = (uint32_t)constrain(inputMessage.toInt(), 0, 5000);
      settingWrite(NK_OvTierMidDwellMs, String(OvTierMidDwellMs).c_str());
      if (OvTierMidDwellMs == 0) queueConsoleMessage("Timed OV cut MID tier DISABLED (dwell 0)");
      else queueConsoleMessageF("Timed OV cut MID-tier dwell set to: %ums", OvTierMidDwellMs);
    }
    if (request->hasParam("DvdtTC")) {
      foundParameter = true;
      inputMessage = request->getParam("DvdtTC")->value();
      DvdtTC = constrain(inputMessage.toFloat(), 5.0f, 500.0f);
      settingWrite(NK_DvdtTC, String(DvdtTC, 1).c_str());
      queueConsoleMessageF("dvdt EMA TC set to: %.1f ms (dt-aware; alpha computed per-sample)", DvdtTC);
    }
    if (request->hasParam("ReseedFrac")) {
      foundParameter = true;
      inputMessage = request->getParam("ReseedFrac")->value();
      ReseedFrac = clamp_f(inputMessage.toFloat(), 0.2f, 1.0f);  // at exactly 1.0 the first-fire de-escalation on a persistent cause is dead — UI max stays 0.95, 1.0 is deliberate-raw-URL territory
      settingWrite(NK_ReseedFrac, String(ReseedFrac, 2).c_str());
      queueConsoleMessageF("Recovery seed fraction set to: %.2f", ReseedFrac);
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    if (request->hasParam("ReseedFracNoShunt")) {
      foundParameter = true;
      inputMessage = request->getParam("ReseedFracNoShunt")->value();
      ReseedFracNoShunt = clamp_f(inputMessage.toFloat(), 0.1f, 0.95f);
      settingWrite(NK_ReseedFracNS, String(ReseedFracNoShunt, 2).c_str());
      queueConsoleMessageF("No-shunt recovery seed fraction set to: %.2f", ReseedFracNoShunt);
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    if (request->hasParam("CvRecovClimbRate")) {
      foundParameter = true;
      inputMessage = request->getParam("CvRecovClimbRate")->value();
      CvRecovClimbRate = clamp_f(inputMessage.toFloat(), 0.01f, 1.0f);
      settingWrite(NK_CvRecovClimb, String(CvRecovClimbRate, 2).c_str());
      queueConsoleMessageF("Recovery climb rate set to: %.2f of max/s (%.0f A/s here)", CvRecovClimbRate, CvRecovClimbRate * (float)MaxTableValue);
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    if (request->hasParam("CVTuningMode")) {
      foundParameter = true;
      inputMessage = request->getParam("CVTuningMode")->value();
      int requested = inputMessage.toInt();
      // Mutex: refuse turn-on if another test is already running. All four tests must run independently.
      if (requested == 1 && CVTuningMode == 0) {
        const char *blocker = (systemIDActive != 0) ? "Plant Delay Test"
                              : (altSweepActive != 0) ? "Gate-tuning field sweep"
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
    if (request->hasParam("cvWaveAmplitudeV")) {
      foundParameter = true;
      inputMessage = request->getParam("cvWaveAmplitudeV")->value();
      cvWaveAmplitudeV = constrain(inputMessage.toFloat(), 0.05f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f), 2.0f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f));
      settingWrite(NK_cvWaveAmplitudeV, String(cvWaveAmplitudeV, 2).c_str());
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    if (request->hasParam("cvWavePeriodSec")) {
      foundParameter = true;
      inputMessage = request->getParam("cvWavePeriodSec")->value();
      cvWavePeriodSec = inputMessage.toInt();
      settingWrite(NK_cvWavePeriodSec, String(cvWavePeriodSec).c_str());
      if (CVTuningMode) cvTuningParamChanged = true;
    }
    if (request->hasParam("cvKOvershoot")) {
      foundParameter = true;
      inputMessage = request->getParam("cvKOvershoot")->value();
      cvKOvershoot = inputMessage.toFloat();
      settingWrite(NK_cvKOvershoot, String(cvKOvershoot, 1).c_str());
    }
    if (request->hasParam("cvConsecutiveReads")) {
      foundParameter = true;
      inputMessage = request->getParam("cvConsecutiveReads")->value();
      cvConsecutiveReads = (uint8_t)constrain(inputMessage.toInt(), 1, 20);
      settingWrite(NK_cvConsecutiveReads, String(cvConsecutiveReads).c_str());
    }
    if (request->hasParam("VoltageDisagreeThreshold")) {
      foundParameter = true;
      inputMessage = request->getParam("VoltageDisagreeThreshold")->value();
      settingWrite(NK_VoltageDisagreeThreshold, inputMessage.c_str());
      VoltageDisagreeThreshold = inputMessage.toFloat();
      queueConsoleMessageF("Voltage disagreement threshold set to: %.2fV", VoltageDisagreeThreshold);
    }
    if (request->hasParam("VoltageDisagreeTimeout")) {
      foundParameter = true;
      inputMessage = request->getParam("VoltageDisagreeTimeout")->value();
      int temp = inputMessage.toInt() * 1000;  // user enters seconds
      settingWrite(NK_VoltageDisagreeTimeout, String(temp).c_str());
      VoltageDisagreeTimeout = temp;
      queueConsoleMessageF("Voltage disagreement timeout set to: %d seconds", inputMessage.toInt());
    }
    if (request->hasParam("VoltageLoopInterval")) {
      foundParameter = true;
      inputMessage = request->getParam("VoltageLoopInterval")->value();
      settingWrite(NK_VoltageLoopInterval, inputMessage.c_str());
      VoltageLoopInterval = inputMessage.toInt();
    }
    if (request->hasParam("FIELD_COLLAPSE_DELAY")) {
      foundParameter = true;
      inputMessage = request->getParam("FIELD_COLLAPSE_DELAY")->value();
      int temp = inputMessage.toInt() * 1000;
      settingWrite(NK_FIELD_COLLAPSE_DELAY, String(temp).c_str());
      FIELD_COLLAPSE_DELAY = temp;
    }
    if (request->hasParam("ResetPerfCounters")) {
      foundParameter = true;
      // Function timing — session worsts (full list mirrors the periodic
      // worstWindow reset block in loop() so every timer on the dashboard
      // actually clears on button press).
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
      // Deliberately NOT cleared here: the ripple analyzer's per-session worsts (faSesPkpkWorstA /
      // faSesPeakWorstA / faSesPeakWorstHz) persist across reboot and are cleared only by the
      // ripple panel's own reset (ResetRipplePeaks handler).
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
      ft_n2kTx.worstSession = 0;
      ft_dvcc.worstSession = 0;
      ft_n2kParse.worstSession = 0;
      n2kTxCount = 0;
      n2kTxDropCount = 0;
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
      // INA228 interval stats — must clear all windows AND all-time accumulators,
      // else stale ina_worst_at/ina_worst_2m spikes survive the reset.
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
      // Deliberately NOT cleared here: the lifetime nav/sailing records reset individually from
      // the Lifetime Statistics panel (ResetLongestTrip / ResetMax24hrDist / ResetDeepestAnchor /
      // ResetBestUpwindVMG / ResetLongestGale) — "Reset Peak Values" must never nuke leaderboard records.
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
      // Worst-loop attribution diagnostic — re-arm so the next worst pass prints to Console
      loopDiagWorstUs = 0;
      // Fault-detector overall compute time — clear so /debug tracks worst since-reset.
      faDetWorstComputeUs = 0;
      faDetLastComputeUs = 0;
      // Core-0 HTTPS task cloud-op time — clear last + worst-since-reset
      httpsUploadWorstMs = 0;
      httpsUploadLastMs = 0;
      // NVS full-save diagnostics — clear the worst-duration watermark and the call
      // counter so "Worst Save Duration" and "Save Count" track since-reset, not since-boot.
      // "Last Save Duration" is zeroed too (it just shows 0 until the next field-off save).
      nvsFullSaveWorstMs = 0;
      nvsFullSaveCount = 0;
      nvsFullSaveLastMs = 0;
      // Reset to the "never saved this boot" sentinel so "Time Since Last Save" reads 0
      // and STAYS 0 until the next field-off save fires — turns the field into a live
      // "is the save actually firing?" indicator after a reset.
      lastNVSSaveTime = 0;
      // Stamp the reset moment. Dashboard reads CSV1 slot 28 = (millis()-this)/1000
      // and formats it as "last 12 min" / "last 1.4 hr" on all .session-window-label spans.
      perfCountersResetMs = millis();
      queueConsoleMessage("Peak counters reset from web interface");
    }

    if (request->hasParam("forceCloudFlush")) {
      foundParameter = true;
      // "Upload Cloud Now" button — bypasses fieldOffSettled and the 13-sec
      // interval throttle. The cloud-feature block in loop() reads this flag,
      // drains the PSRAM ring through the HTTPS queue, and clears the flag
      // once the ring is empty.
      forceCloudFlushPending = true;
      queueConsoleMessage("Cloud sync: forced flush requested");
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

    if (nvsPersistNow) saveNVSDataFull();   // commit storage-namespace resets/sets now (at most one save per request); otherwise they'd wait for the field-off edge and a reboot/power-cut before then would revert them
    if (foundParameter) {
      stateRevision++;      // Increment whenever any setting changed
      settingsDirty = true; // trigger immediate CSV3 settings echo
    }
    if (!foundParameter) {
      inputMessage = "No message sent, the request_hasParam found no match";
    }
    request->send(200, "text/plain", inputMessage);
  });

  // Settings arm gate (replaced /setPassword + /checkPassword). ?arm=1 opens the 30-min
  // write window, ?arm=0 closes it, no param just reports state — the dashboard polls this
  // to restore/expire its unlocked UI, so a reload while armed comes back unlocked.
  // ?ackLock=1 clears the auto-lock notice. serviceSettingsArmHold() holds the window open while
  // any client is attached, so remainSec reads full-scale instead of counting down under a live user.
  server.on("/armSettings", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("arm")) {
      bool arm = request->getParam("arm")->value().toInt() != 0;
      if (arm) {
        settingsArmed = true;
        settingsArmedAtMs = millis();
        settingsAutoLockNotice = false;  // the user is back and re-armed: the notice has done its job
        queueConsoleMessage("Settings ARMED: changes accepted for 30 min");
      } else if (settingsArmed) {
        settingsArmed = false;
        queueConsoleMessage("Settings locked");
      }
    }
    // The ack lands on the DEVICE, not per browser: one lock event, one notice, so a second phone
    // is not nagged about a lock the first already cleared and a stale flag cannot outlive it.
    if (request->hasParam("ackLock")) settingsAutoLockNotice = false;
    bool active = settingsArmActive();
    unsigned long remainSec = active ? (SETTINGS_ARM_TIMEOUT_MS - (millis() - settingsArmedAtMs)) / 1000UL : 0;
    char out[128];
    snprintf(out, sizeof(out), "{\"armed\":%d,\"remainSec\":%lu,\"lockNotice\":%d,\"lockEpoch\":%lu}",
             active ? 1 : 0, remainSec, settingsAutoLockNotice ? 1 : 0, (unsigned long)settingsAutoLockedEpoch);
    request->send(200, "application/json", out);
  });

  // Explicit routes for all static web assets — served directly, never hit onNotFound
  // PSRAM-preload-failed fallback still has to carry no-cache, or the flash path reintroduces the
  // stale-dashboard-off-an-unreachable-device problem serveCachedAsset() closes.
  // The other four assets carry it for the matched-set reason instead: this path has no ETag and
  // no Last-Modified (AsyncFileResponse sets neither, unlike serveStatic), so a browser has no
  // validator to revalidate with and refetches anyway — the header only makes that explicit.
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
    client->send("hello!", NULL, millis(), 10000);
  });
  // MUST precede "/debug": a plain-string URI matches exact-or-prefix-with-slash and the server
  // dispatches in registration order, so "/debug" registered first swallows both of these.
  // Bench-only data-growth ceiling test: fill every ring to cap + measure the worst-case
  // scans (handlers defined at the tail of 8_functions.ino where the ring symbols are in scope).
  { void debugFillMax(AsyncWebServerRequest *); void debugClearMax(AsyncWebServerRequest *);
    server.on("/debug/fillmax",  HTTP_GET, debugFillMax);
    server.on("/debug/clearmax", HTTP_GET, debugClearMax); }

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
    // Ground truth for the Core-1 preemption investigation: where the network tasks
    // actually landed. async_tcp must read 0 if the Core-0 pin made it into THIS image
    // (the pin is a compile-time default — a stale factory/IDE flash won't have it).
    // 2147483647 = tskNO_AFFINITY (floating), -99 = task not found by name.
    TaskHandle_t hAsyncTcp = xTaskGetHandle("async_tcp");
    TaskHandle_t hLwip = xTaskGetHandle("tiT");
    int asyncTcpCore = hAsyncTcp ? (int)xTaskGetCoreID(hAsyncTcp) : -99;
    int lwipCore = hLwip ? (int)xTaskGetCoreID(hLwip) : -99;
    const char *faStateName = (faChanState == 1) ? "live" : (faChanState == 2) ? "RAILED-dormant (jumper open?)" : "off";
    // mbedTLS allocator self-test: do a TLS-sized alloc through mbedtls_calloc and see which heap it
    // lands in. Proves the setup() mbedtls_platform_set_calloc_free(->PSRAM) override is live, without
    // relying on catching the early boot serial line. "PSRAM" = TLS no longer needs contiguous internal.
    size_t tlsPsBefore = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    void *tlsTest = mbedtls_calloc(1, 16384);
    size_t tlsPsAfter = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const char *tlsMem = !tlsTest ? "ALLOC-FAILED" : ((tlsPsBefore - tlsPsAfter) >= 16000 ? "PSRAM" : "INTERNAL");
    if (tlsTest) mbedtls_free(tlsTest);
    char out[2944];
    int dpos = snprintf(out, sizeof(out),
             "Partition: %s\nVersion: %s\nFree heap: %lu\n"
             "Web FS %s: %d/%d KB used (%d%%) - boot seed, -1 = unread\n"
             "TLS buffers -> %s (largest internal block %u B, free PSRAM %u B)\n"
             "Net task cores (0/1=pinned, 2147483647=floating, -99=not found): async_tcp=%d lwIP=%d\n"
             "AdjustField worst full pass (ms): total=%.1f | thermal=%.1f snapshot=%.1f fastov=%.1f modes=%.1f control=%.1f duty=%.1f tail=%.1f\n"
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
             tlsMem,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             asyncTcpCore, lwipCore,
             aflWorstTotalUs / 1000.0f,
             aflWorstSecUs[0] / 1000.0f, aflWorstSecUs[1] / 1000.0f, aflWorstSecUs[2] / 1000.0f,
             aflWorstSecUs[3] / 1000.0f, aflWorstSecUs[4] / 1000.0f, aflWorstSecUs[5] / 1000.0f,
             aflWorstSecUs[6] / 1000.0f,
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
    // Floating-task census for the Core-1 preemption hunt: any task with core=FLOAT
    // (tskNO_AFFINITY) can land on Core 1 and stall the priority-1 loop task. The loop runs
    // at priority 1 — read the core column, then suspect any FLOAT task above it.
    if (dpos > 0 && dpos < (int)sizeof(out)) {
      const UBaseType_t maxTasks = 32;   // uxTaskGetSystemState returns 0 if this is too small
      TaskStatus_t st[maxTasks];
      UBaseType_t nTasks = uxTaskGetSystemState(st, maxTasks, nullptr);
      dpos += snprintf(out + dpos, sizeof(out) - dpos, "Tasks (name prio core stackHWM):");
      for (UBaseType_t i = 0; i < nTasks && dpos < (int)sizeof(out) - 64; i++) {
        int core = (int)xTaskGetCoreID(st[i].xHandle);
        dpos += snprintf(out + dpos, sizeof(out) - dpos, " [%s p%u c%s hwm%u]",
                         st[i].pcTaskName,
                         (unsigned)st[i].uxCurrentPriority,
                         core == 2147483647 ? "FLOAT" : (core == 0 ? "0" : "1"),
                         (unsigned)st[i].usStackHighWaterMark);
      }
      snprintf(out + dpos, sizeof(out) - dpos, "\n");
    }
    request->send(200, "text/plain", out);
  });

  // Phone-sourced GPS + time backup. Browser + Capacitor app both POST here
  // periodically (every ~30-60s) when they have a location fix. The priority
  // chain (NMEA → Phone → NTP) consumes these via the *Phone globals and the
  // lastPhone*Ms freshness timestamps. Partial submissions are OK — send only
  // the fields you have.
  server.on("/set_phone_data", HTTP_GET, [](AsyncWebServerRequest *request) {
    // Deliberately NOT arm-gated: this is background telemetry (clients push every ~30-60s
    // whether or not settings are armed), and phone GPS/speed/time are backup sources a LAN
    // client is trusted to provide. NMEA always outranks phone data, and range checks reject garbage.
    bool acceptedGps = false, acceptedTime = false;
    if (request->hasParam("lat") && request->hasParam("lon")) {
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
      // JS Date.now() is millis since 1970. We store seconds for parity with
      // the GPS/NTP path. Reject anything before Jan 1, 2020 (sanity).
      uint64_t ms = (uint64_t)strtoull(request->getParam("epochMs")->value().c_str(), nullptr, 10);
      time_t sec = (time_t)(ms / 1000ULL);
      if (sec > 1577836800) {
        PhoneTimeEpoch  = sec;
        lastPhoneTimeMs = millis();
        acceptedTime = true;
      }
    }
    // Phone GNSS doppler speed (m/s, Geolocation coords.speed) and course over
    // ground (degrees, coords.heading). Sent independently of lat/lon — heading
    // is null on a stationary phone, so each gets its own freshness stamp.
    bool acceptedSpd = false, acceptedHdg = false;
    if (request->hasParam("spd")) {
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
    if (request->hasParam("hdg")) {
      const char *hdgStr = request->getParam("hdg")->value().c_str();
      char *hdgEnd = nullptr;
      double hdg = strtod(hdgStr, &hdgEnd);
      if (hdgEnd != hdgStr && *hdgEnd == '\0' && !isnan(hdg) && hdg >= 0.0 && hdg <= 360.0) {
        HeadingPhone = (float)((hdg >= 360.0) ? 0.0 : hdg);
        lastPhoneHdgMs = millis();
        acceptedHdg = true;
      }
    }
    // Promote phone data into the effective globals. GPS/time are no-ops when
    // NMEA is fresh (NMEA wins); speed/course apply only when the user has
    // selected phone as the speed source (never an automatic fallback).
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
    if (!settingsArmActive()) {
      request->send(403, "text/plain", "Settings not armed");
      return;
    }

    Serial.println("=== checkRegistration called ===");
    if (!isRegistered) {
      request->send(200, "application/json", "{\"registered\":false}");
      return;
    }

    // Validate token with Supabase (send only token, not device_uid) — staged onto the httpsTask
    // worker; a synchronous doCloudPOST here starved every other endpoint for up to ~45 s on a
    // dead uplink. Response handling lives in executeCloudOp; the client polls /cloudOpState.
    DynamicJsonDocument payloadDoc(256);
    payloadDoc["token"] = authToken;
    String payload;
    serializeJson(payloadDoc, payload);
    cloudOpStageAndReply(request, CLOUDOP_CHECK_REG, payload.c_str());
  });

  // Result poll for the staged cloud ops (/checkRegistration /registerProfile /updateProfile
  // /deleteAllData). "code" is the HTTP status the old synchronous handler would have sent;
  // "body" is its exact JSON reply.
  server.on("/cloudOpState", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!settingsArmActive()) {
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

  // Tiny always-answers identity endpoint for app-side subnet discovery: when mDNS fails
  // across a phone hotspot, the Capacitor app sweeps 172.20.10.2-14 and latches onto this
  // reply. No password, no side effects; CORS header because the app's origin is not us.
  server.on("/identify", HTTP_GET, [](AsyncWebServerRequest *request) {
    char idBuf[128];
    snprintf(idBuf, sizeof(idBuf), "{\"device\":\"xreg-010\",\"uid\":\"%s\",\"fw\":\"%s\"}",
             device_id_hex, FIRMWARE_VERSION);
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
    if (!settingsArmActive()) {
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
    DynamicJsonDocument doc(512);
    doc["device_uid"] = deviceUID;
    doc["username"] = request->getParam("username", true)->value();
    doc["email"] = request->getParam("email", true)->value();
    String payload;
    serializeJson(doc, payload);
    Serial.println("=== REGISTRATION REQUEST ===");
    // Staged onto the httpsTask worker (see /checkRegistration). Token save, post-registration
    // version/forced checks and the snapshot request all live in executeCloudOp.
    cloudOpStageAndReply(request, CLOUDOP_REGISTER, payload.c_str());
  });

  server.on("/updateProfile", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!settingsArmActive()) {
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
    if (!settingsArmActive()) {
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
    tempPIDResetRequested = true;
    request->send(200, "text/plain", "OK");
  });

  server.on("/resetInnerPID", HTTP_POST, [](AsyncWebServerRequest *request) {
    innerPIDResetRequested = true;
    request->send(200, "text/plain", "OK");
  });

  server.on("/tuninglog", HTTP_GET, [](AsyncWebServerRequest *request) {
    // PSRAM-backed buffer; shared_ptr deleter frees on response destruction
    // (normal completion OR abort). Chunked send keeps only ~1.5 KB on the internal heap.
    // 16 KB (was 10 KB): the per-record Notes field widened each row.
    std::shared_ptr<char> bufPtr((char *)ps_malloc(16384), [](char *p) { if (p) free(p); });
    if (!bufPtr) { request->send(500, "text/plain", "OOM"); return; }
    char *buf = bufPtr.get();

    // Build sorted index (insertion sort — 50 entries max)
    uint8_t sortIdx[50];
    for (int i = 0; i < tuningLogCount; i++) sortIdx[i] = i;
    for (int i = 1; i < tuningLogCount; i++) {
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
    for (int i = 0; i < tuningLogCount && pos < 15900; i++) {
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
    // "live" carries the since-reset Control Accuracy v4 numbers for this loop:
    // [Tracking % (in-band while active), worst over-current (A), 0, 0]. (4-slot
    // shape kept for the tuning UI parser.)
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
    pos += snprintf(buf + pos, 2048 - pos, "],\"active\":%d,\"done\":%d,\"coh\":%.3f,\"railed\":%d,\"fs\":%.1f,"
                    "\"aborted\":%d,\"abortWhy\":%d,\"abortV\":%.2f,\"abortD\":%.1f,"
                    "\"rpmAvg\":%.0f,\"rpmMin\":%.0f,\"rpmMax\":%.0f}",
                    tuningSweepActive ? 1 : 0, tuningSweepDone ? 1 : 0,
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
    pos += snprintf(buf + pos, 2048 - pos, "],\"active\":%d,\"ready\":%d,\"aborted\":%d,\"amp\":%.1f,\"fs\":%.1f,"
                    "\"rpmAvg\":%.0f,\"rpmMin\":%.0f,\"rpmMax\":%.0f}",
                    active ? 1 : 0,
                    (systemIDResultsReady && systemIDTestType == 1) ? 1 : 0,
                    systemIDAbortRequested ? 1 : 0,
                    SystemIDStepAmplitude,
                    systemIDFsHz > 0.0f ? systemIDFsHz : ch1SampleHz(),
                    systemIDRpmAvg, systemIDRpmMin, systemIDRpmMax);
    request->send(200, "application/json", buf);
  });

  // Auto-commissioning field-% curve: the duty→amps points + the saturation knee + the
  // proposed open-loop-sine settings. Dashboard shows these and lets the user Apply.
  server.on("/fieldcurve.json", HTTP_GET, [](AsyncWebServerRequest *request) {
    std::shared_ptr<char> bufPtr((char *)ps_malloc(2048), [](char *p) { if (p) free(p); });
    if (!bufPtr) { request->send(500, "text/plain", "OOM"); return; }
    char *buf = bufPtr.get();
    int pos = 0;
    pos += snprintf(buf + pos, 2048 - pos, "{\"pts\":[");
    // Points are display-only and the poller reads them once, off the FINISHED run. Shipping them on
    // the in-run polls put ~1.7 kB on the wire every 1.5 s for the whole ramp, contending with the
    // 10 Hz SSE telemetry on the same TCP stack, for bytes the client discards.
    const bool fcvDone = (fieldCurveActive == 0) && fieldCurveResultsReady;
    for (int i = 0; fcvDone && i < fieldCurveCount && pos < 1700; i++) {
      pos += snprintf(buf + pos, 2048 - pos, "%s{\"d\":%.1f,\"a\":%.2f}",
                      i > 0 ? "," : "", fieldCurveBuf[i].duty, fieldCurveBuf[i].amps);
    }
    // "aborted" is the protection-abort latch (fieldCurveAbortRequested). It is reported INDEPENDENTLY
    // of "active" because a protection cut latches the abort but leaves fieldCurveActive set until
    // fieldCurve_tick next runs — and that tick is gated out during the fault lockout, so "active"
    // can stay 1 indefinitely. The poller checks "aborted" so it never hangs waiting for !active.
    pos += snprintf(buf + pos, 2048 - pos,
                    "],\"active\":%d,\"ready\":%d,\"ok\":%d,\"kneeDuty\":%.1f,\"kneeAmps\":%.2f,"
                    "\"targetA\":%.1f,\"propStabA\":%.1f,\"propStepPct\":%.2f,\"ceilLimited\":%d,\"aborted\":%d,\"abort\":\"%s\","
                    "\"abortWhy\":%d,\"abortNext\":%d,\"abortV\":%.2f,\"abortD\":%.1f,"
                    "\"rpmAvg\":%.0f,\"rpmMin\":%.0f,\"rpmMax\":%.0f}",
                    fieldCurveActive != 0 ? 1 : 0, fieldCurveResultsReady ? 1 : 0, fieldCurveOk ? 1 : 0,
                    fieldCurveKneeDuty, fieldCurveKneeAmps, fieldCurveTargetLimitA,
                    fieldCurvePropStabA, fieldCurvePropStepPct, fieldCurveCeilingLimited ? 1 : 0,
                    fieldCurveAbortRequested ? 1 : 0, fieldCurveAbortMsg,
                    (int)fieldCurveAbortReason, (int)fieldCurveAbortFollowOn,
                    fieldCurveAbortVolts, fieldCurveAbortDuty,
                    fieldCurveRpmAvg, fieldCurveRpmMin, fieldCurveRpmMax);
    request->send(200, "application/json", buf);
  });

  // Field de-energize test (runs per speed inside commissioning stage 7): filtered decay trace +
  // fitted τ / 90→10 fall time + the hold-averaged RPM and pre-cut amps this run's drain point is
  // reported at. pts.t is ms relative to the detected cut edge (small negative lead-in shows the
  // plateau). The wizard polls this once per Min%-floor speed and fits drain-vs-RPM across the runs.
  // "aborted" reported independently of "active" (same rationale as /fieldcurve.json).
  server.on("/fieldcut.json", HTTP_GET, [](AsyncWebServerRequest *request) {
    std::shared_ptr<char> bufPtr((char *)ps_malloc(8192), [](char *p) { if (p) free(p); });
    if (!bufPtr) { request->send(500, "text/plain", "OOM"); return; }
    char *buf = bufPtr.get();
    int pos = 0;
    pos += snprintf(buf + pos, 8192 - pos, "{\"pts\":[");
    // Same as /fieldcurve.json: display-only, read once off the finished run. fieldCutProcess fills
    // these at cut+4 s but `active` stays set through the 1.5 s ease-out, so the in-run polls were
    // pushing ~5 kB apiece across exactly the window where the field re-engages.
    const bool fcDone = (fieldCutActive == 0) && fieldCutResultsReady;
    for (int i = 0; fcDone && i < fcPlotN && pos < 7400; i++) {
      pos += snprintf(buf + pos, 8192 - pos, "%s{\"t\":%.1f,\"a\":%.2f}",
                      i > 0 ? "," : "", fcPlotMs[i], fcPlotA[i]);
    }
    pos += snprintf(buf + pos, 8192 - pos,
                    "],\"active\":%d,\"phase\":%d,\"ready\":%d,\"ok\":%d,\"tauMs\":%.1f,\"fallMs\":%.1f,\"drainMs\":%.1f,"
                    "\"baseA\":%.1f,\"floorA\":%.2f,\"rpm\":%.0f,\"residPct\":%.1f,\"nPts\":%d,\"src\":%d,"
                    "\"calGain\":%.4f,\"calOffA\":%.3f,\"aborted\":%d,\"abort\":\"%s\"}",
                    fieldCutActive != 0 ? 1 : 0, (int)fieldCutPhase, fieldCutResultsReady ? 1 : 0,
                    fieldCutOk ? 1 : 0, fieldCutTauMs, fieldCutFallMs, fieldCutDrainMs, fieldCutBaseA, fieldCutFloorA,
                    fieldCutRpm, fieldCutResidPct, fcPlotN, (int)fieldCutSrc, faCalGain, faCalOffA,
                    fieldCutAbortRequested ? 1 : 0, fieldCutAbortMsg);
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
                    "{\"active\":%d,\"ready\":%d,\"ok\":%d,\"kneeDuty\":%.1f,\"rpm\":%.0f,\"tempF\":%.0f,"
                    "\"fitResid\":%.2f,\"fitWorstIdx\":%d,\"anchors\":[",
                    (fieldCurveActive != 0 && fieldCurveOnsetMode) ? 1 : 0,
                    (fieldCurveResultsReady && fieldCurveOnsetMode) ? 1 : 0, kneeSweepOk ? 1 : 0,
                    kneeSweepKneeDuty, kneeSweepRPM, kneeSweepTempF, kneeFitResidPct, kneeFitWorstIdx);
    for (int i = 0; i < kneeAnchorN && pos < 900; i++) {
      pos += snprintf(buf + pos, 1024 - pos, "%s{\"rpm\":%.0f,\"duty\":%.1f,\"tempF\":%.0f}",
                      i > 0 ? "," : "", kneeAnchorRPM[i], kneeAnchorDuty[i], kneeAnchorTempF[i]);
    }
    // "aborted" = protection-abort latch, reported independently of "active" (see /fieldcurve.json note).
    // "ceilLimited" = the sweep stopped at the 24/36/48V field-duty ceiling before finding onset.
    pos += snprintf(buf + pos, 1024 - pos, "],\"ceilLimited\":%d,\"aborted\":%d,\"abort\":\"%s\","
                    "\"abortWhy\":%d,\"abortNext\":%d,\"abortV\":%.2f,\"abortD\":%.1f}",
                    fieldCurveCeilingLimited ? 1 : 0, fieldCurveAbortRequested ? 1 : 0, fieldCurveAbortMsg,
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
    uint8_t sortIdx[50];
    for (int i = 0; i < tuningSweepLogCount; i++) sortIdx[i] = i;
    for (int i = 1; i < tuningSweepLogCount; i++) {
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
    for (int i = 0; i < tuningSweepLogCount && pos < CAP - 600; i++) {
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
    uint8_t sortIdx[50];
    for (int i = 0; i < sysidSweepLogCount; i++) sortIdx[i] = i;
    for (int i = 1; i < sysidSweepLogCount; i++) {
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
    for (int i = 0; i < sysidSweepLogCount && pos < CAP - 600; i++) {
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
    // PSRAM-backed buffer (50 records × ~200 bytes ≈ 10 KB).
    std::shared_ptr<char> bufPtr((char *)ps_malloc(10240), [](char *p) { if (p) free(p); });
    if (!bufPtr) { request->send(500, "text/plain", "OOM"); return; }
    char *buf = bufPtr.get();

    // Sort by score (lower = faster plant = better). Aborted runs (-1) sink to end.
    uint8_t sortIdx[50];
    for (int i = 0; i < systemIDLogCount; i++) sortIdx[i] = i;
    for (int i = 1; i < systemIDLogCount; i++) {
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
    pos += snprintf(buf + pos, 10240 - pos, "{\"rec\":[");
    for (int i = 0; i < systemIDLogCount && pos < 9800; i++) {
      SystemIDRecord &r = systemIDLog[sortIdx[i]];
      char tbuf[12];   // avgAltTempF is NAN when no probe read during the run -> JSON null
      pos += snprintf(buf + pos, 10240 - pos,
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
    pos += snprintf(buf + pos, 10240 - pos,
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
    // PSRAM-backed buffer (50 records × ~420 bytes ≈ 21 KB). shared_ptr deleter runs
    // when the chunked-response lambda is destroyed (normal completion OR abort), so
    // no manual free is needed and aborted requests can't leak.
    std::shared_ptr<char> bufPtr((char *)ps_malloc(32768), [](char *p) { if (p) free(p); });
    if (!bufPtr) { request->send(500, "text/plain", "OOM"); return; }
    char *buf = bufPtr.get();

    // Build sorted index (insertion sort, best score first)
    uint8_t sortIdx[50];
    for (int i = 0; i < cvTuningLogCount; i++) sortIdx[i] = i;
    for (int i = 1; i < cvTuningLogCount; i++) {
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
    for (int i = 0; i < cvTuningLogCount && pos < 32000; i++) {
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
    // "live" carries the since-reset Control Accuracy v4 numbers for the CV loop:
    // [Tracking % (in-band while active), worst over-voltage (mV 12V-equiv), 0, 0]. (4-slot
    // shape kept for the tuning UI parser.)
    pos += snprintf(buf + pos, (pos >= 32768 ? 0 : 32768 - pos),
      "],\"live\":[%.2f,%.0f,%.0f,%.0f],"
      "\"ts\":%.2f,\"tc\":%d,\"ta\":%d}",
      (accVolt4.activeSec > 0.5) ? (float)(100.0 * accVolt4.inbandActiveSec / accVolt4.activeSec) : 0.0f,
      accVolt4.worstOver * 1000.0f, 0.0f, 0.0f,  // mV
      cvts, (int)cvTuningScore.scoredHighCount, cvTestActive ? 1 : 0);

    // Chunked send: only ~1.5 KB per chunk lands on the internal heap. bufPtr is
    // captured by value (ref-count) so the PSRAM buffer lives for the duration of
    // the response and is freed automatically when the lambda is destroyed.
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

  // Manual "Reset" button under the Control Accuracy panel. Clears all accumulators AND the live
  // excursion stopwatches. (The daily post-snapshot auto-reset passes false so an in-progress
  // excursion carries whole into the new window.)
  server.on("/resetAccuracyScores", HTTP_POST, [](AsyncWebServerRequest *request) {
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
    systemIDLogClearAll();  // persist deferred to Core 1 — avoids blocking Core 0 SSE
    request->send(200, "text/plain", "OK");
  });

  server.on("/resettuningsweeplog", HTTP_POST, [](AsyncWebServerRequest *request) {
    tuningSweepLogCount   = 0;
    tuningSweepLogHead    = 0;
    tuningSweepRunCounter = 0;
    if (tuningSweepLog) memset(tuningSweepLog, 0, 50 * sizeof(TuningSweepRecord));
    pendingSaveTuningSweepLog = true;  // deferred to Core 1
    request->send(200, "text/plain", "OK");
  });

  server.on("/resetsysidsweeplog", HTTP_POST, [](AsyncWebServerRequest *request) {
    sysidSweepLogCount   = 0;
    sysidSweepLogHead    = 0;
    sysidSweepRunCounter = 0;
    if (sysidSweepLog) memset(sysidSweepLog, 0, 50 * sizeof(SysIDSweepRecord));
    pendingSaveSysidSweepLog = true;  // deferred to Core 1
    request->send(200, "text/plain", "OK");
  });

  server.on("/resetVoltageLoop", HTTP_POST, [](AsyncWebServerRequest *request) {
    cvLoopResetRequested = true;
    request->send(200, "text/plain", "OK");
  });

  server.on("/resetVoltageProtectionCounters", HTTP_POST, [](AsyncWebServerRequest *request) {
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
    request->send(200, "text/plain", "OK");
  });

  // Deliberately SEPARATE from the session-counter reset above: routine bench resets must not
  // destroy the lifetime OV record. The web UI fronts this with an explicit confirm.
  server.on("/resetOvTelemetryLifetime", HTTP_POST, [](AsyncWebServerRequest *request) {
    memset(&g_ovTel, 0, sizeof(g_ovTel));
    g_ovTel.magic = OVTEL_MAGIC;
    request->send(200, "text/plain", "OK");
  });

  // Lifetime OV telemetry for the Diagnostics panel. Not in CSV2 (avoids field-count churn) —
  // polled on demand. time_ms as decimal strings: uint64 dwell is beyond JS's 53-bit integers.
  server.on("/getOvTelemetry", HTTP_GET, [](AsyncWebServerRequest *request) {
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
                       (unsigned long)g_ovTel.softExceedCount, (unsigned long)g_ovTel.swHardCutCount,
                       (unsigned long)g_ovTel.inaCutCount, (unsigned long)g_ovTel.kdEventCount,
                       (unsigned long)g_ovTel.tierLowCutCount, (unsigned long)g_ovTel.tierMidCutCount);
    for (int i = 0; i < OV_HIST_BINS && off > 0 && off < (int)cap; i++)
      off += snprintf(buf + off, cap - off, "%s%lu", i ? "," : "", (unsigned long)g_ovTel.events[i]);
    if (off > 0 && off < (int)cap) off += snprintf(buf + off, cap - off, "],\"time_ms\":[");
    for (int i = 0; i < OV_HIST_BINS && off > 0 && off < (int)cap; i++)
      off += snprintf(buf + off, cap - off, "%s\"%llu\"", i ? "," : "", (unsigned long long)g_ovTel.timeMs[i]);
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
    g_tempCritCount = 0;
    g_tempSustainedCount = 0;
    g_tempStaleCount = 0;
    request->send(200, "text/plain", "OK");
  });

  server.on("/resetTempTaskCounters", HTTP_POST, [](AsyncWebServerRequest *request) {
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

  server.addHandler(&events);
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
// Engine-off standby power drop. Normally powers WiFi fully off and slows the CPU to 80MHz. With
// WiFi Napping enabled in Client mode it instead keeps WiFi associated in modem-sleep so the
// dashboard stays reachable with no button press — indefinitely while the regulator stays on the
// router. Nap costs only ~1mA over full-off (measured), so there is no idle timeout. While napping
// the CPU bumps to 240MHz whenever a dashboard is connected (snappy UI) and drops to 80MHz when
// idle. The temp task is NO LONGER suspended here — the DS18B20/OneWire path reads fine at 80MHz
// (all its timing is wall-clock, not CPU-cycle, based), and the task self-throttles to a 10-min
// cadence while the engine is off (see TempTask), which keeps alt temperature fresh for the
// zero-drift log at negligible standby power. Modem-sleep stays on, so there's still ~100-300ms
// beacon latency on the first packet, which is fine. Ordering matters: do the WiFi op, then set the
// clock.
void enterLowPowerStandby() {
  if (wifiNapEnabled && currentMode == MODE_CLIENT) {
    if (!wifiNapActive) {
      // Napping can be switched on after a previous standby already took the WIFI_OFF branch below.
      // Declaring the nap without raising STA first strands checkWiFiConnection() on its WIFI_OFF
      // early-out while the call-site gate believes WiFi is up — dead until ignition-on or GPIO5.
      if (WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);  // seek engine does the join, non-blocking
      WiFi.setSleep(true);           // modem sleep — WiFi naps between DTIM beacons
      wifiNapActive = true;
      queueConsoleMessage("WiFi napping: dashboard stays reachable at low power (no button needed)");
    }
  } else {
    WiFi.mode(WIFI_OFF);             // THIS MUST BE DONE FIRST
    wifiNapActive = false;           // napping disabled (or AP mode) — clear stale nap state
  }
  // Temp task intentionally NOT suspended (reads fine at 80MHz; self-throttles to 10 min engine-off).
  // While napping, run 240MHz when a dashboard is actually connected (snappy UI), else 80MHz.
  // WiFi-off standby always stays 80MHz. Guarded so the PLL isn't reconfigured every loop pass.
  uint32_t targetMhz = (wifiNapActive && events.count() > 0) ? 240 : 80;
  if (getCpuFrequencyMhz() != targetMhz) {
    setCpuFrequencyMhz(targetMhz);   // THIS MUST BE DONE AFTER THE WIFI OP
  }
}
void checkWiFiConnection() {
  // Non-blocking reconnect engine — never waits on the radio in loop() (Core 1). Presence scans
  // run async+passive on the Core-0 WiFi task; joins are fire-and-poll across loop passes. No
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
void SendWifiData() {
  // Don't send WiFi data during HTTPS operations
  unsigned long start66 = micros();
  // === THROTTLED WIFI CHECKS (every 2 seconds) ===
  static int cachedWiFiMode = WIFI_OFF;
  static int cachedWiFiConnected = 0;
  static int cachedWiFiRSSI = -100;
  static unsigned long lastWiFiCheck = 0;

  unsigned long now = millis();

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

  // One-shot boot announcement. Emitted only once a browser is connected, so it
  // survives the 10-slot console queue churning during boot and lands right after
  // the SSE auto-reconnect that follows a reboot — exactly the remote-debug case.
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
    } else {
      queueConsoleMessage("BLACKBOX: RTC RAM lost - true power interruption preceded this boot");
    }
  }

  // PRIORITY 1: CSVData
  if (!sentSomething && now - prev_millis5 >= webgaugesinterval && events.count() > 0) {
    WifiHeartBeat = WifiHeartBeat + 1;

    static char *payload1 = nullptr;
    static const size_t PAYLOAD1_SIZE = 1400;
    if (!payload1) {
      payload1 = (char *)ps_malloc(PAYLOAD1_SIZE);  // bumped from 500, allocated to PSRAM
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

    int payload1Len = snprintf(payload1, PAYLOAD1_SIZE,
                               "%d,"  // CSV1_FIELD_COUNT
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"  // +2: mExcessEmaPeak, iExcessThreshMin; +1: fieldEventReason; +4: cvPTerm, cvIterm, cvKdTrim, cvKdFiltV; +1: huntDerate; +2: huntFreqHz, huntState; +1: rpmCeilingAmps
                               "%u,%u",  // +2: sessionId, sendMs

                               CSV1_FIELD_COUNT,
                               !isfinite(AlternatorTemperatureF) ? ROLL_EMPTY : SafeInt(AlternatorTemperatureF, 100),  // no probe bound to the alt role -> "not available"; SafeInt's -1 rendered as a real -0.0F

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
                               SafeInt(shutdownPhase),
                               SafeInt(BatteryV, 100),                  // raw ADS1115
                               SafeInt(pidAltPV, 100),  // CSV1_pidAltPV
                               SafeInt(ChargingVoltageTarget * 100),
                               SafeInt(Icv * 100),
                               // Water depth in feet ×10 (0.1 ft resolution). 0 if NMEA depth stale or unavailable.
                               SafeInt(IS_STALE(IDX_WATER_DEPTH) ? 0 : (WaterDepth_m * 3.28084f), 10),
                               SafeInt(Ignition),  // effective ignition (override applied at top of loop)
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

    events.send(payload1, "CSVData");
    SendWifiTime = micros() - start66;
    prev_millis5 = now;
    lastEventSourceSend = now;
    sentSomething = true;
  }
  // PRIORITY 2: CSVData4 / NavStream — live nav/wind/solar/fuel at 2 Hz (500 ms).
  // High priority (right after CSV1) so these gauges aren't starved by the slower CSV2/CSV3/TS
  // channels. These fields used to ride the 5 s CSV2 cadence and looked frozen on the dial/helm.
  if (!sentSomething && now - lastpayload4send >= 500UL && events.count() > 0) {
    static char *payload4 = nullptr;
    static const size_t PAYLOAD4_SIZE = 512;  // ~7 B/field, plus the two 10-digit stamp fields; rounded up with headroom
                                              // Was 256: SafeInt's sentinel widened from -1 to -2000000000, and 19 SafeInt
                                              // fields at 11 chars each puts the arithmetic worst case at 253 of 256 bytes.
                                              // snprintf truncates and the length guard below drops the frame, so an
                                              // overrun would silently stop NavStream rather than corrupt memory.
    if (!payload4) {
      payload4 = (char *)ps_malloc(PAYLOAD4_SIZE);  // allocated to PSRAM
      if (!payload4) {
        Serial.println("FATAL: payload4 ps_malloc failed");
        return;
      }
    }
    int payload4Len = snprintf(payload4, PAYLOAD4_SIZE,
                               "%d,"  // CSV4_FIELD_COUNT
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,"
                               "%u,%u",  // +2: sessionId, sendMs

                               CSV4_FIELD_COUNT,
                               SafeInt(HeadingNMEA),                     // CSV4_HeadingNMEA
                               SafeInt(SOGNMEA, 100),                    // CSV4_SOGNMEA
                               SafeInt(COGNMEA),                         // CSV4_COGNMEA
                               !isfinite(STWNMEA) ? ROLL_EMPTY : SafeInt(STWNMEA, 100),                                  // CSV4_STWNMEA (knots ×100)
                               SafeInt(ApparentWindSpeedNMEA, 100),      // CSV4_ApparentWindSpeedNMEA
                               SafeInt(ApparentWindAngleNMEA),           // CSV4_ApparentWindAngleNMEA
                               // No wind instrument / no target bearing / no boat speed leaves these NAN. SafeInt's -1
                               // rendered as real readings (-0.0 kt, -1 deg) and banked a flat line in the trend rings.
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
                               (unsigned)g_sessionId,                    // CSV4_sessionId
                               (unsigned)millis()                        // CSV4_sendMs
    );
    if (payload4Len < 0 || payload4Len >= PAYLOAD4_SIZE) {
      Serial.printf("payload4 truncated or format error: %d\n", payload4Len);
      return;
    }
    events.send(payload4, "CSVData4");
    lastpayload4send = now;
    lastEventSourceSend = now;
    sentSomething = true;
  }
  // PRIORITY 3: Console
  trySendConsoleSSE(sentSomething, now);
  // PRIORITY 4: CSVData2 (status/settings data).
  // During a plant delay test, bypass the sentSomething gate and send every 500 ms so the
  // UI phase-progress display stays within one poll tick of the actual firmware state.
  // Normal operation: every 5 s, gated behind CSV1 to avoid double-sending per tick.
  const bool sysIDRunning = (systemIDActive != 0);
  if ((sysIDRunning || !sentSomething) && now - lastpayload2send >= (sysIDRunning ? 500UL : 5000UL) && events.count() > 0) {
    static char *payload2 = nullptr;
    // Sized by the ~7 B/field rule rather than the observed frame, because an overflow returns early
    // from SendWifiData and kills CSV2 + CSV3 + TS until reboot. PSRAM.
    static const size_t PAYLOAD2_SIZE = 8192;
    static int payload2Len = 0;       // persists between the build pass and the send pass
    static uint8_t csv2Phase = 0;     // 0 = build this due pass, 1 = send the built payload next pass
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
    uint32_t _csv2b0 = micros();   // CSV2 build-cost timer (snprintf)
        payload2Len = snprintf(payload2, PAYLOAD2_SIZE,
                               "%d,"  // CSV2_FIELD_COUNT
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%u,%u,%d,"
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
                               "%u,%u\n",       // +2: sessionId, sendMs
                               CSV2_FIELD_COUNT,
                               SafeInt(IBVMax, 100),
                               SafeInt(MeasuredAmpsMax, 100),
                               SafeInt(RPMMax),
                               HAS_BATT_SHUNT ? SafeInt(SOC_percent) : ROLL_EMPTY,   // no shunt -> coulomb counting is off, the value is a frozen NVS leftover
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
                               SafeInt(DynamicShuntGainFactor, 1000),
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
                               SafeInt(imuRingBuffer->accel_dropped),
                               SafeInt(imuRingBuffer->gyro_dropped),
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
                               SafeInt((tempPIDActive ? (1 << 0) : 0) | (sysMode == SYS_MODE_AUTO ? (1 << 4) : 0) | (shutdownPhase != SHUTDOWN_PHASE_NONE ? (1 << 5) : 0)),
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
                               sledTele(sledLive.dayIdx ? sledLive.predHarvKwh : NAN),   // CSV2_sledPredHarvToday
                               sledTele(sledLiveActHarvKwh()),                           // CSV2_sledActHarvToday
                               sledTele(sledLive.dayIdx ? sledLive.predConsKwh : NAN),   // CSV2_sledPredConsToday
                               sledTele(sledLiveActConsKwh()),                           // CSV2_sledActConsToday
                               SafeInt(sledNeedKwh, 100),                                // CSV2_sledNeedKwh
                               SafeInt(sledNeedSource),                                  // CSV2_sledNeedSource
                               SafeInt(sledCompleteDays()),                              // CSV2_sledDaysValid
                               SafeInt(sledLive.dayIdx ? sledLive.coverageMin : 0),      // CSV2_sledCoverageMin
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
                               (unsigned)g_sessionId,   // CSV2_sessionId
                               (unsigned)millis());     // CSV2_sendMs — build time; the send happens one pass later
    csv2BuildLastUs = micros() - _csv2b0;   // CSV2 build (snprintf) cost
    if (csv2BuildLastUs > csv2BuildWorstUs) csv2BuildWorstUs = csv2BuildLastUs;
    // Clear the anti-windup latch now that this CSV2 frame has captured it (set in tempPID_tick on each CV-bleed event)
    thermalAntiWindupLatch = false;
    if (payload2Len < 0 || payload2Len >= (int)PAYLOAD2_SIZE) {
      Serial.printf("payload2 truncated or format error: %d\n", payload2Len);
      csv2Phase = 0;
      return;
    }
    csv2Phase = 1;          // built OK — send it on the next pass
    sentSomething = true;   // claim this pass (the heavy build ran)
    } else {
      // PASS 2 — send the payload built last pass (cheap ~1.3 ms; kept off the build pass).
      uint32_t _csv2s0 = micros();   // CSV2 send-cost timer (events.send → AsyncTCP)
      events.send(payload2, "CSVData2");
      csv2SendLastUs = micros() - _csv2s0;
      if (csv2SendLastUs > csv2SendWorstUs) csv2SendWorstUs = csv2SendLastUs;
      lastpayload2send = now;
      lastEventSourceSend = now;
      csv2Phase = 0;
      sentSomething = true;
    }
  }

  // PRIORITY 5: CSVData3 — sent immediately when settingsDirty (event-driven), or every 60s fallback
  if (!sentSomething && (settingsDirty || now - lastpayload3send >= 60000) && events.count() > 0) {
    static char *payload3 = nullptr;
    // ~7 B/field, and this is the tightest of the five buffers — CSV3 grows every time a setting is
    // added, so check it when adding a block of them. Overflow is caught below rather than truncating,
    // but the catch returns early from SendWifiData, which stops CSV3 for good and skips TS on any pass
    // CSV3 is due. Raise this before that happens, not after.
    static const size_t PAYLOAD3_SIZE = 3000;
    if (!payload3) {
      payload3 = (char *)ps_malloc(PAYLOAD3_SIZE);  // allocated to PSRAM
      if (!payload3) {
        Serial.println("FATAL: payload3 ps_malloc failed");
        return;
      }
    }
    /// ALL THIS SAFEINT STUFF MAY BE UNNECESSAREY BAD ADVICE, COULD HAVE JUST SENT ROUNDED FLOATS FOR 1 Byte (or bit?) xtra
    //WifiSendTime was 834uS before increasing csv3 payload size from 1100 to 1400     No change after.  Again, this separation into groups and worry about wifi packet size seems like AI nonsense.

    int     payload3Len = snprintf(payload3, PAYLOAD3_SIZE,
                               "%d,"  // CSV3_FIELD_COUNT
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
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%.3f,%.3f,%.3f,%d,"
                               "%d,"  // +1 CvKdVoltFiltTC (int) — pairs the arg inserted after VoltageFilterTC; sits in the all-integer run before IExcessArmMarginV so every field stays type-aligned
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%.3f,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,%d,%d,%d,"
                               "%d,%d,%d,"   // damper detection bar (x100) + 2 NMEA 0183: baud, polarity
                               "%d,"         // displayVolUnit
                               "%d,"         // gpsPositionSource
                               "%d,"         // MaxFieldVolts (x10)
                               "%.3f,%d,%.3f,%d,"  // timed OV tiers: LOW margin (V), LOW dwell (ms), MID margin (V), MID dwell (ms)
                               "%d,"         // VoltageHardwareLimit (x100)
                               "%d,%d,%d,"   // load-dump consecutive-sample counts N1/N2/N3
                               "%d,%d,%d,%d," // solar ledger toggles + margins: learn, use consumption, margin % (x100), learn rate % (x100)
                               "%d,%d,%d,%d,%d,%d,%d,"  // RV-C: tx master, charger DGNs, DC source DGNs, DM_RV, charger instance, DC instance, device priority
                               "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"  // +14 battery/extra temperature settings (§6 order, CommissionTempSrc last)
                               "%u,%u\n",    // +2: sessionId, sendMs
                               CSV3_FIELD_COUNT,
                               SafeInt(TemperatureLimitF),
                               SafeInt(BulkVoltage, 100),
                               SafeInt(wavePeriod),
                               SafeInt(FloatVoltage, 100),
                               SafeInt(SwitchingFrequency),
                               SafeInt(yyMin),
                               0,  // CSV3_retired1
                               SafeInt(ManualDutyTarget, 100),
                               SafeInt(PhysicalPanelOverride),
                               SafeInt(waveAmplitude),
                               SafeInt(CurrentThreshold, 100),
                               SafeInt(PeukertExponent_scaled),
                               SafeInt(ChargeEfficiency_scaled),
                               SafeInt(ChargedVoltage_Scaled),
                               SafeInt(TailCurrent, 10),
                               SafeInt(ChargedDetectionTime),
                               SafeInt(IgnoreTemperature),
                               SafeInt(bmsLogic),
                               SafeInt(bmsLogicLevelOff),
                               SafeInt(RPMScalingFactor),
                               SafeInt(MaximumAllowedBatteryAmps),
                               SafeInt(AlternatorNominalAmps),
                               SafeInt(LearningUpStep, 100),
                               SafeInt(LearningDownStep, 100),
                               SafeInt(xTime),
                               SafeInt(MinLearningInterval),
                               SafeInt(SafeOperationThreshold),
                               SafeInt(PidKp, 1000),
                               SafeInt(PidKi, 1000),
                               SafeInt(PidKd, 1000),
                               SafeInt(PidSampleDivisor),
                               SafeInt(MaxTableValue, 100),
                               SafeInt(MaxPenaltyPercent, 100),
                               SafeInt(MaxPenaltyDuration / 1000),
                               SafeInt(NeighborLearningFactor, 1000),
                               SafeInt(yyMax),
                               SafeInt(LearningMemoryDuration / 86400000),
                               SafeInt(TuningMode),
                               SafeInt(ShuntResistanceMicroOhm),
                               SafeInt(InvertAltAmps),
                               SafeInt(InvertBattAmps),
                               SafeInt(MaxDuty),
                               SafeInt(MinDuty, 100),
                               SafeInt(FieldResistance, 100),
                               SafeInt(maxPoints),
                               SafeInt(AlternatorCOffset, 100),
                               SafeInt(BatteryCOffset, 100),
                               SafeInt(BatteryCapacity_Ah),
                               SafeInt(AmpSensorRange),
                               SafeInt(R_fixed, 100),
                               SafeInt(Beta, 100),
                               SafeInt(T0_C, 100),
                               SafeInt(TempSource),
                               SafeInt(IgnitionOverride),
                               SafeInt(FLOAT_DURATION),
                               SafeInt(PulleyRatio, 100),
                               SafeInt(BatteryCurrentSource),
                               SafeInt(rpmTableRPMPoints[0]),
                               SafeInt(rpmTableRPMPoints[1]),
                               SafeInt(rpmTableRPMPoints[2]),
                               SafeInt(rpmTableRPMPoints[3]),
                               SafeInt(rpmTableRPMPoints[4]),
                               SafeInt(rpmTableRPMPoints[5]),
                               SafeInt(rpmTableRPMPoints[6]),
                               SafeInt(rpmTableRPMPoints[7]),
                               SafeInt(rpmTableRPMPoints[8]),
                               SafeInt(rpmTableRPMPoints[9]),
                               SafeInt(LearningSettlingPeriod),
                               SafeInt(LearningRPMChangeThreshold),
                               SafeInt(LearningTempHysteresis),
                               SafeInt(fuelTableRPM[0]),
                               SafeInt(fuelTableRPM[1]),
                               SafeInt(fuelTableRPM[2]),
                               SafeInt(fuelTableRPM[3]),
                               SafeInt(fuelTableRPM[4]),
                               SafeInt(fuelTableRPM[5]),
                               SafeInt(fuelTableRPM[6]),
                               SafeInt(fuelTableRPM[7]),
                               SafeInt(fuelTableRPM[8]),
                               SafeInt(fuelTableRPM[9]),
                               SafeInt(fuelTableGPH[0], 100),
                               SafeInt(fuelTableGPH[1], 100),
                               SafeInt(fuelTableGPH[2], 100),
                               SafeInt(fuelTableGPH[3], 100),
                               SafeInt(fuelTableGPH[4], 100),
                               SafeInt(fuelTableGPH[5], 100),
                               SafeInt(fuelTableGPH[6], 100),
                               SafeInt(fuelTableGPH[7], 100),
                               SafeInt(fuelTableGPH[8], 100),
                               SafeInt(fuelTableGPH[9], 100),
                               SafeInt(stateRevision),
                               SafeInt(DutyRampRate, 100),
                               SafeInt(SettleTimeBeforeCut),
                               SafeInt(TempWarnExcess, 100),
                               SafeInt(TempCritExcess, 100),
                               SafeInt(TempSustainedTimeout / 1000),
                               SafeInt(AlternatorHardShutdownV, 100),
                               SafeInt(VoltageDisagreeThreshold, 100),
                               SafeInt(VoltageDisagreeTimeout / 1000),
                               SafeInt(rpmMinDutyTable[0], 100),
                               SafeInt(rpmMinDutyTable[1], 100),
                               SafeInt(rpmMinDutyTable[2], 100),
                               SafeInt(rpmMinDutyTable[3], 100),
                               SafeInt(rpmMinDutyTable[4], 100),
                               SafeInt(rpmMinDutyTable[5], 100),
                               SafeInt(rpmMinDutyTable[6], 100),
                               SafeInt(rpmMinDutyTable[7], 100),
                               SafeInt(rpmMinDutyTable[8], 100),
                               SafeInt(rpmMinDutyTable[9], 100),
                               SafeInt(rpmCapCurrentTable[0], 100),
                               SafeInt(rpmCapCurrentTable[1], 100),
                               SafeInt(rpmCapCurrentTable[2], 100),
                               SafeInt(rpmCapCurrentTable[3], 100),
                               SafeInt(rpmCapCurrentTable[4], 100),
                               SafeInt(rpmCapCurrentTable[5], 100),
                               SafeInt(rpmCapCurrentTable[6], 100),
                               SafeInt(rpmCapCurrentTable[7], 100),
                               SafeInt(rpmCapCurrentTable[8], 100),
                               SafeInt(rpmCapCurrentTable[9], 100),
                               SafeInt(VoltageKp, 100),
                               SafeInt(VoltageLoopInterval),
                               SafeInt(FIELD_COLLAPSE_DELAY),
                               SafeInt(SetpointRiseRate, 100),
                               SafeInt(SetpointFallRate, 100),
                               SafeInt(SetpointBigStepThresh, 100),
                               SafeInt(SetpointBigStepRiseRate, 100),
                               SafeInt(PIDTrackingGain, 100),
                               SafeInt(CAPSIZE_THRESHOLD_DEG),
                               SafeInt(PITCHPOLE_THRESHOLD_DEG),
                               SafeInt(SLAM_THRESHOLD_G, 10),
                               SafeInt(imuMountOrientation),
                               SafeInt(TailCurrent_A, 100),
                               SafeInt(RebulkVoltage, 100),
                               SafeInt(rebulkDebounceTime),
                               SafeInt(MinFloatTime),
                               SafeInt(SOC_BlockRebulk_percent),
                               SafeInt(SOC_AllowRebulk_percent),
                               SafeInt(DutySlowRampRate, 100),
                               SafeInt(ShutdownPhase2HoldMs),
                               SafeInt(TempPIDKp, 1000),
                               SafeInt(TempPIDKi, 1000),
                               SafeInt(ThermalLookaheadSec),
                               SafeInt(TempPIDIntervalMs),
                               SafeInt(TempPIDFilterAlpha, 1000),
                               SafeInt(VoltageKi, 100),
                               (int)rpmCapPowerTable[0],
                               (int)rpmCapPowerTable[1],
                               (int)rpmCapPowerTable[2],
                               (int)rpmCapPowerTable[3],
                               (int)rpmCapPowerTable[4],
                               (int)rpmCapPowerTable[5],
                               (int)rpmCapPowerTable[6],
                               (int)rpmCapPowerTable[7],
                               (int)rpmCapPowerTable[8],
                               (int)rpmCapPowerTable[9],
                               SafeInt(SystemIDStepAmplitude, 10),
                               SafeInt(HardOCTripAmps, 10),
                               SafeInt(HardOCDebounceMs),
                               SafeInt(IExcessFrac, 1000),
                               SafeInt(IExcessFloorA, 10),
                               SafeInt(IExcessKBleed, 100),
                               SafeInt(IgnoreRPM),
                               SafeInt(MinRPMForField),
                               SafeInt(AwBleedRate, 10),
                               SafeInt(KHard, 10),
                               SafeInt(ReseedFrac, 100),
                               (int)AwSeedProtectMs,
                               SafeInt(displayTempUnit),
                               SafeInt(WarmupRampRate, 10),
                               (int)OvGroup1Enable,
                               (int)OvGroup2Enable,
                               SafeInt(IExcessCeilA, 10),
                               SafeInt(IExcessTau),
                               OutputPIDSigSrc,
                               TdPred,
                               OvMeasMarginV,
                               OvPredMarginV,
                               OutputPIDMA_N,
                               (int)OutputPIDFilterTC,
                               (int)VoltageFilterTC,
                               (int)CvKdVoltFiltTC,
                               SafeInt(CvKdDeadbandVps, 100),
                               SafeInt(VoltageKd, 10),
                               SafeInt(DvdtTC, 10),
                               SafeInt(CvKdArmV, 100),
                               SafeInt(StartupRiseRate, 100),
                               SafeInt(absorptionCompleteTime),
                               SafeInt(OnOff),
                               SafeInt(ManualFieldToggle),
                               SafeInt(HiLow),
                               SafeInt(LimpHome),
                               SafeInt(AlarmActivate),
                               SafeInt(TempAlarm),
                               SafeInt(VoltageAlarmHigh, 100),
                               SafeInt(VoltageAlarmLow, 100),
                               SafeInt(CurrentAlarmHigh),
                               SafeInt(AlarmTest),
                               SafeInt(AlarmLatchEnabled),
                               SafeInt(MaintainMode),
                               SafeInt(ManualSOCPoint, 100),
                               SafeInt(IgnoreLearningDuringPenalty),
                               SafeInt(LogAllLearningEvents),
                               SafeInt(CloudFeatures),
                               SafeInt(AutoShuntGainCorrection),
                               SafeInt(AutoAltCurrentZero),
                               SafeInt(WindingTempOffset),
                               SafeInt(ManualLifePercentage),
                               SafeInt(UVThresholdHigh, 100),
                               SafeInt(weatherModeEnabled),
                               SafeInt(imuEnabled ? 1 : 0),
                               SafeInt(AbsorptionVoltage * 100),
                               SafeInt(AbsorptionTimeoutMs),
                               SafeInt(bulkVoltageHoldMs),
                               SafeInt(capLimitMode),
                               SafeInt(TargetVoltageMode),
                               SafeInt(TargetVoltageSetpoint, 100),
                               SafeInt(RebulkCurrent_A, 100),
                               SafeInt(UseFloat),
                               SafeInt(IExcessFracBulk, 1000),
                               SafeInt(IExcessRelFrac, 1000),
                               SafeInt(systemIDPlantTauMs),
                               SafeInt(TempAlarmLow),
                               SafeInt(LoadDumpDtThresh),
                               SafeInt(LoadDumpDtThresh1),
                               (int)CVTuningMode,
                               SafeInt(cvWaveAmplitudeV, 100),
                               (int)cvWavePeriodSec,
                               SafeInt(cvKOvershoot, 10),
                               (int)cvConsecutiveReads,
                               SafeInt(webgaugesinterval),
                               SafeInt(plotTimeWindow),
                               SafeInt(Ymin1),
                               SafeInt(Ymax1),
                               SafeInt(Ymin2, 100),
                               SafeInt(Ymax2, 100),
                               SafeInt(Ymin3),
                               SafeInt(Ymax3),
                               SafeInt(Ymin4),
                               SafeInt(Ymax4),
                               SafeInt(LoadDumpDtThresh3),
                               SafeInt(hardwarePresent),
                               (int)testProtectionsEnabled,
                               IExcessArmMarginV,
                               SafeInt(FastSetpointRiseRate, 100),
                               (int)FastSetpointRiseWindowMs,
                               SafeInt(FastSetpointRiseHeadroomV, 100),
                               SafeInt(SolarWatts),
                               SafeInt(performanceRatio, 100),
                               SafeInt(VeData),
                               SafeInt(NMEA0183Data),
                               SafeInt(NMEA2KData),
                               SafeInt(timeAxisModeChanging),
                               (int)timeSourceMode,
                               (int)speedSourceMode,
                               (int)faEnabled,
                               (int)faAlarmEnable,
                               (int)faAnomPause,
                               SafeInt(faRpmEdgeMargin, 10),
                               SafeInt(faAmpsDriftFloorA, 100),
                               SafeInt(faAmpsDriftPct, 10),
                               SafeInt(faAttenUpAmps, 10),
                               SafeInt(faAttenDownAmps, 10),
                               SafeInt(faPeakMinA, 100),
                               (int)wifiNapEnabled,
                               SafeInt(imuHeelOffsetDeg, 100),
                               SafeInt(imuPitchOffsetDeg, 100),
                               SafeInt(systemIDTestType),
                               SafeInt(systemIDSineFreqStart, 10),
                               SafeInt(systemIDSineFreqEnd, 10),
                               SafeInt(systemIDSineCycles),
                               SafeInt(tuningWaveform),
                               SafeInt(tuningSineFreq, 10),
                               SafeInt(tuningSweepStart, 10),
                               SafeInt(tuningSweepEnd, 10),
                               SafeInt(tuningSweepCycles),
                               SafeInt(SystemIDStabilizeAmps, 10),
                               SafeInt(tuningWaveFloor),
                               (int)commissionState,
                               (int)commissionPhase,
                               (int)commissionDoneMask,
                               (int)cvHelpersEnabled,
                               SafeInt(MinChargeTempF),
                               (int)coldChargeLockoutEnable,
                               (int)cvGainMode,
                               SafeInt(cvPlantK, 10000),
                               SafeInt(cvComputedKp, 100),
                               SafeInt(cvComputedKi, 100),
                               SafeInt(cvCrossover, 100),
                               SafeInt(cvPiZero, 100),
                               SafeInt(vTgtRampUp, 1000),
                               SafeInt(vTgtRampDn, 1000),
                               (int)vTgtRampEnable,
                               (int)setpointSlewEnable,
                               (int)cvRiseGovEnable,
                               (int)dutySlewEnable,
                               isnan(CommissionTempF) ? ROLL_EMPTY : (int)lroundf(CommissionTempF * 10.0f),
                               (int)battTempDerateEnable,
                               SafeInt(battTempCoeff, 10000),
                               SafeInt(TempPIDKiDownFrac, 1000),
                               SafeInt(ThermalSlopeWindowSec),
                               SafeInt(BattCurrentLimitA, 10),
                               SafeInt(ripWinMs),
                               SafeInt(ripDriftFloorA, 100),
                               SafeInt(ripDriftPct, 10),
                               SafeInt(SocAlarmLow),
                               SafeInt(battMaxMode),
                               SafeInt(IExcessBaseA, 10),
                               SafeInt(IExcessCcOffsetA, 10),
                               SafeInt(BatteryShuntPresent),
                               (int)cvRecovEnable,
                               SafeInt(cvRecovSec, 10),
                               SafeInt(cvRecovEmaxV, 1000),
                               (int)testSlewMode,
                               (int)cvTestSlewMode,
                               (int)CvKdOneSided,
                               SafeInt(fieldDecayTauMs),
                               (int)commissionManualMask,
                               SafeInt(CvKdMaxTrimA, 10),
                               SafeInt(cvAlpha, 1000),
                               SafeInt(CvKdSlopeCeil, 10),
                               SafeInt(cvComputedKd, 100),
                               SafeInt(CvKdDbSlope, 10000),
                               SafeInt(CvKdDbFloor, 100),
                               SafeInt(CvKdDbCeil, 100),
                               (int)cvRecovBoostEnable,
                               SafeInt(cvRecovBoostMax, 100),
                               SafeInt(cvRecovBoostErrV, 1000),
                               SafeInt(fdDrainLoMs),
                               SafeInt(fdDrainHiMs),
                               SafeInt(fdDrainRpmLo),
                               SafeInt(fdDrainRpmHi),
                               (int)HardOCEnable,
                               (int)IExcessEnable,
                               (int)BattLimitEnable,
                               (int)CvKdExcessMode,
                               SafeInt(CvStressDropV, 100),
                               SafeInt(CvStressFailBandV, 100),
                               SafeInt(CvBrakeFallRate, 100),
                               SafeInt(cvRecovKiMax, 100),
                               (int)cvWindDownEnable,
                               SafeInt(cvWindDownRate, 1000),
                               SafeInt(cvWindDownStopV, 1000),
                               (int)LoadDumpEnable,
                               (int)loadServeBoostEnable,
                               (int)reseedCorrEnable,
                               (int)HuntGovEnable,
                               SafeInt(ReseedFracNoShunt, 100),
                               SafeInt(CvRecovClimbRate, 100),
                               SafeInt(protTestCutMs),
                               SafeInt(protTestGapMs),
                               SafeInt(protTestReps),
                               SafeInt(protTestCmdA),
                               SafeInt(cvRecovBoostFloorV, 1000),
                               SafeInt(cvRecovDeepBandV, 1000),
                               SafeInt(cvRecovDeepMult, 100),
                               SafeInt(cvRecovFlareBandV, 1000),
                               SafeInt(cvRecovFlareFrac, 100),
                               (int)TachLieEnable,
                               SafeInt(n2kTxEnable),
                               SafeInt(n2kDeviceInstance),
                               SafeInt(n2kBattEnable),
                               SafeInt(n2kBattInstance),
                               SafeInt(n2kBattCfgEnable),
                               SafeInt(n2kAltEnable),
                               SafeInt(n2kAltInstance),
                               SafeInt(n2kAltTempEnable),
                               SafeInt(n2kTempInstance),
                               SafeInt(n2kTempSource),
                               SafeInt(n2kChgrEnable),
                               SafeInt(n2kChgrInstance),
                               SafeInt(n2kChgrCfgEnable),
                               SafeInt(n2kChgrMode),
                               SafeInt(n2kEngRpmEnable),
                               SafeInt(n2kEngInstance),
                               SafeInt(n2kEngDynEnable),
                               SafeInt(n2kEngBitsEnable),
                               SafeInt(n2kRxBattInstance),
                               SafeInt(dvccEn),
                               SafeInt(dvccSrcType),
                               SafeInt(dvccInst),
                               SafeInt(dvccSilenceS),
                               SafeInt(dvccSettleS),
                               SafeInt(dvccCvlMin, 100),
                               SafeInt(dvccCvlMax, 100),
                               (int)HuntCutPct,
                               (int)HuntVerifyPct,
                               (int)HuntWingPct,
                               (int)HuntCooldownMin,
                               (int)HuntSteadyPct,
                               (int)HuntQualifyScans,
                               SafeInt(HuntTrigPct, 100),
                               (int)NMEA0183Baud,
                               (int)NMEA0183Invert,
                               SafeInt(displayVolUnit),
                               (int)gpsPositionSource,
                               SafeInt(MaxFieldVolts, 10),
                               OvTierLoMarginV,             // CSV3_OvTierLoMarginV (%.3f)
                               SafeInt(OvTierLoDwellMs),    // CSV3_OvTierLoDwellMs
                               OvTierMidMarginV,            // CSV3_OvTierMidMarginV (%.3f)
                               SafeInt(OvTierMidDwellMs),   // CSV3_OvTierMidDwellMs
                               SafeInt(VoltageHardwareLimit, 100),  // CSV3_VoltageHardwareLimit
                               SafeInt(LoadDumpN1),
                               SafeInt(LoadDumpN2),
                               SafeInt(LoadDumpN3),
                               SafeInt(solarLearnEnable),           // CSV3_solarLearnEnable
                               SafeInt(solarUseConsEnable),         // CSV3_solarUseConsEnable
                               SafeInt(solarConsMarginPct, 100),    // CSV3_solarConsMarginPct
                               SafeInt(solarLearnRatePct, 100),     // CSV3_solarLearnRatePct
                               SafeInt(rvcTxEnable),                // CSV3_rvcTxEnable
                               SafeInt(rvcChgrEnable),              // CSV3_rvcChgrEnable
                               SafeInt(rvcDcEnable),                // CSV3_rvcDcEnable
                               SafeInt(rvcFaultEnable),             // CSV3_rvcFaultEnable
                               SafeInt(rvcChgrInstance),            // CSV3_rvcChgrInstance
                               SafeInt(rvcDcInstance),              // CSV3_rvcDcInstance
                               SafeInt(rvcDevPriority),             // CSV3_rvcDevPriority
                               SafeInt(battTempProbeEnable),        // CSV3_battTempProbeEnable
                               SafeInt(extraTempProbeEnable),       // CSV3_extraTempProbeEnable
                               SafeInt(battTempSource),             // CSV3_battTempSource
                               SafeInt(battTempProxyEnable),        // CSV3_battTempProxyEnable
                               SafeInt(hotChargeLockoutEnable),     // CSV3_hotChargeLockoutEnable
                               SafeInt(MaxChargeTempF),             // CSV3_MaxChargeTempF
                               SafeInt(extraTempAlarmHiEnable),     // CSV3_extraTempAlarmHiEnable
                               SafeInt(extraTempAlarmHiF),          // CSV3_extraTempAlarmHiF
                               SafeInt(extraTempAlarmLoEnable),     // CSV3_extraTempAlarmLoEnable
                               SafeInt(extraTempAlarmLoF),          // CSV3_extraTempAlarmLoF
                               SafeInt(n2kExtraTempEnable),         // CSV3_n2kExtraTempEnable
                               SafeInt(n2kExtraTempInstance),       // CSV3_n2kExtraTempInstance
                               SafeInt(n2kExtraTempSource),         // CSV3_n2kExtraTempSource
                               SafeInt(CommissionTempSrc),          // CSV3_CommissionTempSrc
                               (unsigned)g_sessionId,   // CSV3_sessionId
                               (unsigned)millis());     // CSV3_sendMs
    if (payload3Len < 0 || payload3Len >= PAYLOAD3_SIZE) {
      Serial.printf("payload3 truncated or format error: %d\n", payload3Len);
      return;
    }

    events.send(payload3, "CSVData3");
    settingsDirty = false;
    lastpayload3send = now;
    lastEventSourceSend = now;
    sentSomething = true;
  }

  // PRIORITY 6: TimestampData (staleness data - every 3 seconds)
  if (!sentSomething && now - lastTimestampSend >= 3000 && events.count() > 0) {

    static char *timestampPayload = nullptr;
    static const size_t TIMESTAMP_PAYLOAD_SIZE = 512;  // every field is an age and the weather one can be 10 digits; an overflow kills TS until reboot
    if (!timestampPayload) {
      timestampPayload = (char *)ps_malloc(TIMESTAMP_PAYLOAD_SIZE);  // allocated to PSRAM
      if (!timestampPayload) {
        Serial.println("FATAL: timestampPayload ps_malloc failed");
        return;
      }
    }
    int timestampPayloadLen = snprintf(timestampPayload, TIMESTAMP_PAYLOAD_SIZE,
                                       "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,"
                                       "%lu,%lu,%lu,%lu,"  // +4 battery/extra temperature: BATT probe, EXTRA probe, VE.Direct T, RV-C T
                                       "%lu,%lu",  // +2: sessionId, sendMs
                                       (unsigned long)TS_FIELD_COUNT,
                                       (dataTimestamps[IDX_HEADING_NMEA] == 0) ? 999999 : (now - dataTimestamps[IDX_HEADING_NMEA]),
                                       (dataTimestamps[IDX_LATITUDE_NMEA] == 0) ? 999999 : (now - dataTimestamps[IDX_LATITUDE_NMEA]),
                                       (dataTimestamps[IDX_LONGITUDE_NMEA] == 0) ? 999999 : (now - dataTimestamps[IDX_LONGITUDE_NMEA]),
                                       (dataTimestamps[IDX_SATELLITE_COUNT] == 0) ? 999999 : (now - dataTimestamps[IDX_SATELLITE_COUNT]),
                                       (dataTimestamps[IDX_VICTRON_VOLTAGE] == 0) ? 999999 : (now - dataTimestamps[IDX_VICTRON_VOLTAGE]),
                                       (dataTimestamps[IDX_VICTRON_CURRENT] == 0) ? 999999 : (now - dataTimestamps[IDX_VICTRON_CURRENT]),
                                       (dataTimestamps[IDX_ALTERNATOR_TEMP] == 0) ? 999999 : (now - dataTimestamps[IDX_ALTERNATOR_TEMP]),
                                       (dataTimestamps[IDX_THERMISTOR_TEMP] == 0) ? 999999 : (now - dataTimestamps[IDX_THERMISTOR_TEMP]),
                                       (dataTimestamps[IDX_RPM] == 0) ? 999999 : (now - dataTimestamps[IDX_RPM]),
                                       (dataTimestamps[IDX_MEASURED_AMPS] == 0) ? 999999 : (now - dataTimestamps[IDX_MEASURED_AMPS]),
                                       (dataTimestamps[IDX_BATTERY_V] == 0) ? 999999 : (now - dataTimestamps[IDX_BATTERY_V]),
                                       (dataTimestamps[IDX_IBV] == 0) ? 999999 : (now - dataTimestamps[IDX_IBV]),
                                       (dataTimestamps[IDX_BCUR] == 0) ? 999999 : (now - dataTimestamps[IDX_BCUR]),
                                       (dataTimestamps[IDX_CHANNEL3V] == 0) ? 999999 : (now - dataTimestamps[IDX_CHANNEL3V]),
                                       (dataTimestamps[IDX_DUTY_CYCLE] == 0) ? 999999 : (now - dataTimestamps[IDX_DUTY_CYCLE]),
                                       (dataTimestamps[IDX_FIELD_VOLTS] == 0) ? 999999 : (now - dataTimestamps[IDX_FIELD_VOLTS]),
                                       (dataTimestamps[IDX_FIELD_AMPS] == 0) ? 999999 : (now - dataTimestamps[IDX_FIELD_AMPS]),
                                       (dataTimestamps[IDX_COG_NMEA] == 0) ? 999999 : (now - dataTimestamps[IDX_COG_NMEA]),
                                       (dataTimestamps[IDX_SOG_NMEA] == 0) ? 999999 : (now - dataTimestamps[IDX_SOG_NMEA]),
                                       (dataTimestamps[IDX_APPARENT_WIND_SPEED] == 0) ? 999999 : (now - dataTimestamps[IDX_APPARENT_WIND_SPEED]),
                                       (dataTimestamps[IDX_APPARENT_WIND_ANGLE] == 0) ? 999999 : (now - dataTimestamps[IDX_APPARENT_WIND_ANGLE]),
                                       (dataTimestamps[IDX_TRUE_WIND_SPEED] == 0) ? 999999 : (now - dataTimestamps[IDX_TRUE_WIND_SPEED]),
                                       (dataTimestamps[IDX_TRUE_WIND_ANGLE] == 0) ? 999999 : (now - dataTimestamps[IDX_TRUE_WIND_ANGLE]),
                                       (dataTimestamps[IDX_LEEWAY] == 0) ? 999999 : (now - dataTimestamps[IDX_LEEWAY]),
                                       (dataTimestamps[IDX_VMG] == 0) ? 999999 : (now - dataTimestamps[IDX_VMG]),
                                       (dataTimestamps[IDX_BARO_PRESSURE] == 0) ? 999999 : (now - dataTimestamps[IDX_BARO_PRESSURE]),
                                       (dataTimestamps[IDX_AMBIENT_TEMP] == 0) ? 999999 : (now - dataTimestamps[IDX_AMBIENT_TEMP]),
                                       (dataTimestamps[IDX_IMU] == 0) ? 999999 : (now - dataTimestamps[IDX_IMU]),
                                       (dataTimestamps[IDX_VICTRON_SOLAR] == 0) ? 999999 : (now - dataTimestamps[IDX_VICTRON_SOLAR]),
                                       (dataTimestamps[IDX_STW_NMEA] == 0) ? 999999 : (now - dataTimestamps[IDX_STW_NMEA]),
                                       (dataTimestamps[IDX_N2K_BATT] == 0) ? 999999 : (now - dataTimestamps[IDX_N2K_BATT]),
                                       (dataTimestamps[IDX_N2K_SOC] == 0) ? 999999 : (now - dataTimestamps[IDX_N2K_SOC]),
                                       (dataTimestamps[IDX_DVCC] == 0) ? 999999 : (now - dataTimestamps[IDX_DVCC]),
                                       // Own "never" sentinel: a forecast age of 999999 ms is only
                                       // 17 min, well inside the normal 6-hour refresh interval, so
                                       // the shared sensor sentinel would read as a real age here.
                                       (weatherLastUpdate == 0) ? 4294967295UL : (unsigned long)(now - weatherLastUpdate),
                                       (dataTimestamps[IDX_N183] == 0) ? 999999 : (now - dataTimestamps[IDX_N183]),
                                       (dataTimestamps[IDX_BATT_TEMP_PROBE] == 0) ? 999999 : (now - dataTimestamps[IDX_BATT_TEMP_PROBE]),
                                       (dataTimestamps[IDX_EXTRA_TEMP] == 0) ? 999999 : (now - dataTimestamps[IDX_EXTRA_TEMP]),
                                       (dataTimestamps[IDX_VE_BATT_TEMP] == 0) ? 999999 : (now - dataTimestamps[IDX_VE_BATT_TEMP]),
                                       (dataTimestamps[IDX_RVC_BATT_TEMP] == 0) ? 999999 : (now - dataTimestamps[IDX_RVC_BATT_TEMP]),
                                       (unsigned long)g_sessionId,   // TS_sessionId
                                       (unsigned long)now            // TS_sendMs (`now` is this dispatch tick's millis())
    );
    if (timestampPayloadLen < 0 || timestampPayloadLen >= TIMESTAMP_PAYLOAD_SIZE) {
      Serial.printf("timestampPayload truncated or format error: %d\n", timestampPayloadLen);
      return;
    }

    events.send(timestampPayload, "TimestampData");
    lastTimestampSend = now;
    lastEventSourceSend = now;
    sentSomething = true;
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
void saveAuthToken(String token) {
  nvs_handle_t handle;
  esp_err_t err = nvs_open("cloud", NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    Serial.printf("ERROR opening NVS for write: %d\n", err);
    return;
  }

  err = nvs_set_str(handle, "authToken", token.c_str());
  if (err != ESP_OK) {
    Serial.printf("ERROR writing token: %d\n", err);
  }
  nvs_commit(handle);
  nvs_close(handle);

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

void debugStackBeforeHTTPS(const char *functionName) {
  UBaseType_t loopStackBytes = uxTaskGetStackHighWaterMark(NULL);  // Already in bytes

  Serial.printf("=== BEFORE %s ===\n", functionName);
  Serial.printf("Loop stack free: %d bytes\n", loopStackBytes);
  Serial.printf("Free heap: %u KB\n", ESP.getFreeHeap() / 1024);
  Serial.printf("Largest heap block: %u bytes\n", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
#ifdef BOARD_HAS_PSRAM
  Serial.printf("Free PSRAM: %u KB\n", ESP.getFreePsram() / 1024);
#endif
  Serial.println("==================");
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
  vesselNvsSet(h, NK_regMountLoc,    String((int)(doc["regulator_mount_loc"] | 0)).c_str());
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
  vesselNvsSet(h, NK_SolarWatts,         String(SolarWatts).c_str());
  vesselNvsSet(h, NK_imuMountOrient,     String((int)imuMountOrientation).c_str());
  vesselNvsSet(h, NK_regMountLoc,        String((int)regulatorMountLoc).c_str());
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
  applyChemistryOcvPreset();  // chemistry-match the rested-voltage curve before the seed reads it
  seedSocFromVoltage();  // factory-fresh path: seed was deferred until real chemistry/capacity existed
  // Refresh the cloud's user_profiles vessel projection on Save, not at the next boot/24 h
  // backstop — the snapshot's settings jsonb is the only carrier of the vessel record.
  configSnapshotRequested = true;
}

