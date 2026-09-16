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


// // Temporary debug - remove it later
// static unsigned long lastForceRead = 0;
// if (millis() - lastForceRead > 1000) {  // Every 1 seconds
//   Serial.println("=== FORCED INA228 READ ===");
//   try {
//     float testVoltage = INA.getBusVoltage();
//     float testCurrent = INA.getShuntVoltage() * 1000;
//     Serial.printf("Direct read: %.3fV, %.3fmV shunt\n", testVoltage, testCurrent);
//   } catch (...) {
//     Serial.println("INA228 direct read FAILED");
//   }
//   lastForceRead = millis();
// }

// // // this one is for ESP32 regular
// bool ensureLittleFS() {
//   if (littleFSMounted) {
//     return true;
//   }
//   Serial.println("Initializing LittleFS...");
//   if (!LittleFS.begin(true, "/littlefs", 10, "spiffs")) {  /// NOTE THE PRESENCE OF spiffs
//     Serial.println("CRITICAL: LittleFS mount failed! Attempting format...");
//     if (!LittleFS.begin(true)) {
//       Serial.println("CRITICAL: LittleFS format failed - filesystem unavailable");
//       littleFSMounted = false;
//       return false;
//     } else {
//       Serial.println("LittleFS formatted and mounted successfully");
//       littleFSMounted = true;
//       return true;
//     }
//   } else {
//     Serial.println("LittleFS mounted successfully");
//   }
//   littleFSMounted = true;
//   return true;
// }



// bool setupDisplay() {
//   // Add delay for ESP32 stabilization
//   delay(100);
//   try {
//     // Initialize SPI
//     SPI.begin();
//     delay(50);                  // Let SPI settle
//     SPI.setFrequency(1000000);  // Start slow for stability
//     SPI.setDataMode(SPI_MODE0);
//     SPI.setBitOrder(MSBFIRST);
//     SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
//     SPI.endTransaction();
//     u8g2.begin();
//     u8g2.clearBuffer();
//     u8g2.sendBuffer();
//     displayAvailable = true;
//     Serial.println("Display initialized successfully");
//     queueConsoleMessage("Display initialized successfully");
//     return true;
//   } catch (...) {
//     Serial.println("Display initialization failed - exception caught");
//     queueConsoleMessage("Display initialization failed - exception caught");
//     displayAvailable = false;
//     return false;
//   }
// }
// void UpdateDisplay() {
//   // Double-check display availability
//   if (!displayAvailable) {
//     return;
//   }
//   static unsigned long lastDisplayUpdate = 0;
//   const unsigned long DISPLAY_UPDATE_INTERVAL = 3000;  // 3 seconds
//   const unsigned long DISPLAY_TIMEOUT = 2000;          // 2 seconds

//   unsigned long currentTime = millis();

//   // Check if it's time to update display
//   if (currentTime - lastDisplayUpdate <= DISPLAY_UPDATE_INTERVAL) {
//     return;
//   }

//   // Add a try-catch around all display operations
//   try {
//     unsigned long displayStart = currentTime;

//     // Try display operations with timeout
//     u8g2.clearBuffer();
//     if (millis() - displayStart > DISPLAY_TIMEOUT) {
//       Serial.println("Display timeout - disabling display");
//       queueConsoleMessage("Display timeout - disabling display");
//       displayAvailable = false;
//       lastDisplayUpdate = millis();
//       return;
//     }

//     u8g2.setFont(u8g2_font_6x10_tf);

//     // Row 1 (y=10)
//     u8g2.drawStr(0, 10, "Vlts:");
//     u8g2.setCursor(35, 10);
//     u8g2.print(BatteryV, 2);
//     u8g2.drawStr(79, 10, "R:");
//     u8g2.setCursor(90, 10);
//     u8g2.print(RPM, 0);

//     // Row 2 (y=20)
//     u8g2.drawStr(0, 20, "Acur:");
//     u8g2.setCursor(35, 20);
//     u8g2.print(MeasuredAmps, 1);
//     u8g2.drawStr(79, 20, "VV:");
//     u8g2.setCursor(90, 20);
//     u8g2.print(VictronVoltage, 2);

//     // Check timeout partway through
//     if (millis() - displayStart > DISPLAY_TIMEOUT) {
//       Serial.println("Display timeout during updates - disabling display");
//       queueConsoleMessage("Display timeout during updates - disabling display");
//       displayAvailable = false;
//       lastDisplayUpdate = millis();
//       return;
//     }

//     // Row 3 (y=30)
//     u8g2.drawStr(0, 30, "Temp:");
//     u8g2.setCursor(35, 30);
//     u8g2.print(AlternatorTemperatureF, 1);
//     u8g2.drawStr(79, 30, "t:");
//     u8g2.setCursor(90, 30);
//     u8g2.print("extra");

//     // Row 4 (y=40)
//     u8g2.drawStr(0, 40, "PWM%:");
//     u8g2.setCursor(35, 40);
//     u8g2.print(dutyCycle, 1);
//     u8g2.drawStr(79, 40, "H:");
//     u8g2.setCursor(90, 40);
//     u8g2.print(HeadingNMEA);

//     // Row 5 (y=50)
//     u8g2.drawStr(0, 50, "Vout:");
//     u8g2.setCursor(35, 50);
//     u8g2.print(vvout, 2);

//     // Row 6 (y=60)
//     u8g2.drawStr(0, 60, "Bcur:");
//     u8g2.setCursor(35, 60);
//     u8g2.print(Bcur, 1);

//     // Final timeout check before sendBuffer()
//     if (millis() - displayStart > DISPLAY_TIMEOUT) {
//       Serial.println("Display timeout before sendBuffer - disabling display");
//       queueConsoleMessage("Display timeout before sendBuffer - disabling display");
//       displayAvailable = false;
//       lastDisplayUpdate = millis();
//       return;
//     }

//     u8g2.sendBuffer();

//     // Log if display operations took a long time
//     unsigned long totalTime = millis() - displayStart;
//     if (totalTime > 1000) {
//       Serial.println("Display took: " + String(totalTime) + "ms");
//     }

//     lastDisplayUpdate = millis();

//   } catch (...) {
//     Serial.println("Display operation failed - disabling display");
//     queueConsoleMessage("Display operation failed - disabling display");
//     displayAvailable = false;
//     lastDisplayUpdate = millis();
//   }
// }




// void AdjustField() {  // AdjustField() - PWM Field Control with Freshness Tracking
//   static unsigned long lastFieldAdjustment = 0;
//   unsigned long currentTime = millis();
//   // Check if it's time to adjust field
//   if (currentTime - lastFieldAdjustment <= FieldAdjustmentInterval) {
//     return;
//   }
//   chargingEnabled = (Ignition == 1 && OnOff == 1);  //Charging will be enabled only when both Ignition and OnOff are equal to 1

//   // Check temperature data freshness for safety
//   unsigned long tempAge = currentTime - dataTimestamps[IDX_ALTERNATOR_TEMP];
//   bool tempDataVeryStale = (tempAge > 30000);  // 30 seconds
//   if (tempDataVeryStale) {
//     //ABSTRACT OUT THESE LINES LATER except perhaps the specific error message
//     Serial.println("Onewire sensor stale, sensor dead or disconnected");
//     queueConsoleMessage("OneWire sensor stale, sensor dead or disconnected");
//     digitalWrite(21, HIGH);  // sound alarm
//   }

//   // Check battery sensor redundancy and quit if problem found

//   if (abs(BatteryV - IBV) > 5000) {  /// FIX LATER thereshold
//     //ABSTRACT OUT THESE LINES LATER except perhaps the specific error message
//     static unsigned long lastVoltageDisagreementWarning = 0;
//     digitalWrite(21, HIGH);  // sound alarm
//     digitalWrite(4, 0);      // disable field
//     dutyCycle = MinDuty;     // restart duty cycle from minimum

//     // Throttle console message to once every 10 seconds
//     if (millis() - lastVoltageDisagreementWarning > 10000) {
//       char msg[128];
//       snprintf(msg, sizeof(msg),
//                "Battery Voltage disagreement! BatteryV=%.3f V, IBV=%.3f V. Field shut off for safety!",
//                BatteryV, IBV);
//       queueConsoleMessage(msg);
//       lastVoltageDisagreementWarning = millis();
//     }
//     return;
//   }


//   //Block any field control/changes during auto-zero of current sensor
//   if (autoZeroStartTime > 0) {
//     lastFieldAdjustment = currentTime;
//     return;  // Let processAutoZero() handle field control
//   }

//   //Preparation
//   updateChargingStage();  // Update charging stage (bulk/float logic)
//   float currentBatteryVoltage = getBatteryVoltage();

//   // Emergency field collapse - voltage spike protection
//   if (currentBatteryVoltage > (ChargingVoltageTarget + 0.2)) {
//     digitalWrite(4, 0);  // Immediately disable field
//     dutyCycle = MinDuty;
//     setDutyPercent((int)dutyCycle);
//     fieldCollapseTime = currentTime;  // Record when collapse happened
//     queueConsoleMessage("EMERGENCY: Field collapsed - voltage spike (" + String(currentBatteryVoltage, 2) + "V) - disabled for 10 seconds");
//     return;  // Exit function immediately
//   }

//   // Check if we're still in collapse delay period
//   if (fieldCollapseTime > 0 && (currentTime - fieldCollapseTime) < FIELD_COLLAPSE_DELAY) {
//     digitalWrite(4, 0);  // Keep field off
//     dutyCycle = MinDuty;
//     setDutyPercent((int)dutyCycle);
//     return;  // Exit function, don't do normal field control
//   }

//   // Clear the collapse flag after delay expires
//   if (fieldCollapseTime > 0 && (currentTime - fieldCollapseTime) >= FIELD_COLLAPSE_DELAY) {
//     fieldCollapseTime = 0;
//     queueConsoleMessage("Field collapse delay expired - normal operation resumed");
//   }

//   // Check for and handle BMS override toggle by updating chargingEnabled accordingly
//   //bmsLogic is simply another way to turn the alternator OFF
//   if (bmsLogic == 1) {
//     // If BMS signal is active (based on bmsLogicLevelOff setting)
//     bmsSignalActive = !digitalRead(42);  // this is the signal from the BMS itself (need "!"" because of optocouplers)
//     if (bmsLogicLevelOff == 0) {
//       // BMS gives LOW signal when charging NOT desired
//       chargingEnabled = chargingEnabled && bmsSignalActive;
//     } else {
//       // BMS gives HIGH signal when charging NOT desired
//       chargingEnabled = chargingEnabled && !bmsSignalActive;
//     }
//   }

//   //now the normal logic to control field
//   if (chargingEnabled) {           // this could be 0 due to bmsLogic, Igntion, or OnOff
//     digitalWrite(4, 1);            // Enable the MOSFET
//     if (ManualFieldToggle == 0) {  // Automatic mode       // Should move this outside the BMS logic at some point..
//       // Step 1: Determine base target amps from Hi/Low setting
//       if (HiLow == 1) {
//         uTargetAmps = TargetAmps;  // Normal target
//       } else {
//         uTargetAmps = TargetAmpL;  // Low target
//       }
//       // Step 2: Apply RPM-based modification if enabled
//       //this was removed

//       // Step 2.4: Apply MaintainMode override if enabled
//       if (MaintainMode == 1) {
//         // Force float mode: target 0 amps at battery (perfect float charging)
//         uTargetAmps = 0;
//       }
//       // Step 2.45: Apply Weather Mode if enabled and active
//       if (weatherModeEnabled == 1 && currentWeatherMode == 1) {
//         // High solar detected - pause charging
//         uTargetAmps = -99;  // this ensures we're going to be pegged at min duty
//       }

//       // Step 2.5, figure out the actual amps reading of whichever value we are controlling on
//       if (MaintainMode == 1) {
//         // Force float mode: use battery current (should be ~0)
//         targetCurrent = Bcur;
//       } else {
//         // Normal mode: use configured current source
//         targetCurrent = getTargetAmps();  // targetCurrent is actually the current reading of the active ("target") sensor, and uTargetAmps is our actual target
//       }
//       //Step 2.6 figure out the actual temp reading o whichever value we are controlling on
//       if (TempSource == 0) {
//         TempToUse = AlternatorTemperatureF;
//       }
//       if (TempSource == 1) {
//         TempToUse = temperatureThermistor;
//       }
//       // Step 3: Apply control logic to adjust duty cycle
//       // Increase duty cycle if below target and not at maximum
//       if (targetCurrent < uTargetAmps && dutyCycle < (MaxDuty - dutyStep)) {
//         dutyCycle += dutyStep;
//       }
//       // Decrease duty cycle if above target and not at minimum
//       if (targetCurrent > uTargetAmps && dutyCycle > (MinDuty + dutyStep)) {
//         dutyCycle -= dutyStep;
//       }
//       // Temperature protection (more aggressive reduction)
//       if (!IgnoreTemperature && TempToUse > TemperatureLimitF && dutyCycle > (MinDuty + 2 * dutyStep)) {
//         dutyCycle -= 2 * dutyStep;
//         static unsigned long lastTempProtectionWarning = 0;
//         if (millis() - lastTempProtectionWarning > 10000) {
//           queueConsoleMessage("Temp limit reached, backing off...");
//           lastTempProtectionWarning = millis();
//         }
//       }
//       // Voltage protection (most aggressive reduction)
//       if (currentBatteryVoltage > ChargingVoltageTarget && dutyCycle > (MinDuty + 3 * dutyStep)) {
//         dutyCycle -= 3 * dutyStep;
//         static unsigned long lastVoltageProtectionWarning = 0;
//         if (millis() - lastVoltageProtectionWarning > 10000) {
//           queueConsoleMessage("Voltage limit reached, backing off quickly!...");
//           lastVoltageProtectionWarning = millis();
//         }
//       }
//       // Battery current protection (safety limit)
//       if (Bcur > MaximumAllowedBatteryAmps && dutyCycle > (MinDuty + dutyStep)) {
//         dutyCycle -= dutyStep;
//         static unsigned long lastCurrentProtectionWarning = 0;
//         if (millis() - lastCurrentProtectionWarning > 10000) {
//           queueConsoleMessage("Battery current limit reached, backing off...");
//           lastCurrentProtectionWarning = millis();
//         }
//       }
//       // Ensure duty cycle stays within bounds
//       dutyCycle = constrain(dutyCycle, MinDuty, MaxDuty);  //Critical that no charging can happen at MinDuty!!
//     }

//     else {  // Manual override mode
//       Serial.printf("MANUAL MODE: ManualDutyTarget=%d, chargingEnabled=%d, fieldCollapseTime=%lu\n",
//                     ManualDutyTarget, chargingEnabled, fieldCollapseTime);
//       dutyCycle = ManualDutyTarget;
//       uTargetAmps = 0;
//       dutyCycle = constrain(dutyCycle, 0, 100);
//       Serial.printf("MANUAL MODE: Final dutyCycle=%.1f, calling setDutyPercent(%d)\n",
//                     dutyCycle, (int)dutyCycle);
//       setDutyPercent((int)dutyCycle);
//       Serial.printf("MANUAL MODE: Pin 4 state=%d, PWM duty sent=%d\n", digitalRead(4), (int)dutyCycle);
//     }
//   }

//   else {
//     // Charging disabled: shut down field and reset for next enable
//     digitalWrite(4, 0);  // Disable the Field (FieldEnable)
//     dutyCycle = MinDuty;
//     uTargetAmps = 0;  //
//   }

//   // Apply the calculated duty cycle
//   setDutyPercent((int)dutyCycle);
//   dutyCycle = dutyCycle;                            //shoddy work, oh well
//   vvout = dutyCycle / 100 * currentBatteryVoltage;  //
//   iiout = vvout / FieldResistance;

//   // Mark calculated values as fresh - these are always current when calculated
//   MARK_FRESH(IDX_DUTY_CYCLE);
//   MARK_FRESH(IDX_FIELD_VOLTS);
//   MARK_FRESH(IDX_FIELD_AMPS);

//   // Update timer (only once)
//   lastFieldAdjustment = currentTime;
//   // fieldActiveStatus = (chargingEnabled && (fieldCollapseTime == 0 || (currentTime - fieldCollapseTime) >= FIELD_COLLAPSE_DELAY) && (dutyCycle > (MinDuty + 1.0))) ? 1 : 0; // old delete later problematic
//   fieldActiveStatus = (chargingEnabled &&                                                                        //The overall charging system must be enabled
//                        (fieldCollapseTime == 0 || (currentTime - fieldCollapseTime) >= FIELD_COLLAPSE_DELAY) &&  //   "Either we're in normal operation, OR if there was an emergency voltage spike that shut down the field, enough time has passed that it's safe to operate again."
//                                                                                                                  //Above prevents the field status from showing "ACTIVE" during the 10-second emergency cooldown period after a voltage spike, even if the other conditions (charging enabled, duty > 0) are met.
//                        (dutyCycle > 0))
//                         ? 1
//                         : 0;  // The field duty cycle must be greater than zero (field is actually energized)
// }






// //Display     HAD TO REMOVE FOR CONNECTOR SPACE
// // OLED pin mapping from RJ45 → ESP32
// #define OLED_CS 5      // RJ45 Pin 15
// #define OLED_DC 19     // RJ45 Pin 14
// #define OLED_RESET 27  // RJ45 Pin 13
// // SSD1306 OLED using 4-wire SPI, full framebuffer
// U8G2_SSD1306_128X64_NONAME_F_4W_HW_SPI u8g2(U8G2_R0, OLED_CS, OLED_DC, OLED_RESET);
// bool displayAvailable = false;  // Global flag to track if display is working

// void DisplayDiagnostics() {
//   static unsigned long lastDiagnostic = 0;

//   if (millis() - lastDiagnostic < 20000) {
//     return;
//   }
//   lastDiagnostic = millis();

//   Serial.println("\n========== DIAGNOSTIC DISPLAY ==========");
//   Serial.println("Time: " + String(millis() / 1000.0, 1) + " seconds");

//   // 1. Alternator fuel consumed
//   Serial.println("\n--- FUEL ---");
//   Serial.println("AlternatorFuelUsed_AllTime: " + String(AlternatorFuelUsed_AllTime, 3) + " L");
//   Serial.println("EngineFuelUsed (session): " + String(EngineFuelUsed, 3) + " L");
//   Serial.println("EngineFuelUsed_AllTime: " + String(EngineFuelUsed_AllTime, 3) + " L");
//   Serial.println("Current RPM: " + String(RPM, 0));

//   // 2. Engine hours
//   Serial.println("\n--- ENGINE RUNTIME ---");
//   Serial.println("EngineRunTime_AllTime: " + String(EngineRunTime_AllTime, 4) + " hours");
//   Serial.println("  (Should be: " + String(millis() / 3600000.0, 4) + " hours based on uptime)");

//   // 3. Alternator energy output
//   Serial.println("\n--- ENERGY ---");
//   Serial.println("AlternatorChargedEnergy_AllTime: " + String(AlternatorChargedEnergy_AllTime, 2) + " Wh");
//   Serial.println("ChargedEnergy_AllTime: " + String(ChargedEnergy_AllTime, 2) + " Wh");
//   Serial.println("  Current Amps: " + String(MeasuredAmps, 1) + " A");
//   Serial.println("  Current Voltage: " + String(BatteryV, 2) + " V");
//   Serial.println("  Current Power: " + String(MeasuredAmps * BatteryV, 1) + " W");

//   // 4. Solar energy output
//   Serial.println("\n--- SOLAR ---");
//   Serial.println("SolarChargedEnergy (session): " + String(SolarChargedEnergy, 2) + " Wh");
//   Serial.println("SolarChargedEnergy_AllTime: " + String(SolarChargedEnergy_AllTime, 2) + " Wh");
//   Serial.println("  VictronCurrent: " + String(VictronCurrent, 1) + " A");

//   // 5. Alternator on time
//   Serial.println("\n--- ALTERNATOR ON TIME ---");
//   Serial.println("AlternatorOnTime_AllTime: " + String(AlternatorOnTime_AllTime, 2) + " minutes");
//   Serial.println("  (Should be: " + String(millis() / 60000.0, 2) + " minutes based on uptime)");

//   // 6. Max alternator current
//   Serial.println("\n--- MAX CURRENT ---");
//   Serial.println("MeasuredAmpsMax_AllTime: " + String(MeasuredAmpsMax_AllTime, 1) + " A");
//   Serial.println("  Current Amps: " + String(MeasuredAmps, 1) + " A");

//   // 7. Peak voltage
//   Serial.println("\n--- VOLTAGE PEAKS ---");
//   Serial.println("PeakVoltage_AllTime: " + String(PeakVoltage_AllTime, 2) + " V");
//   Serial.println("  Current Voltage: " + String(BatteryV, 2) + " V");

//   // 8. Minimum voltage
//   Serial.println("\n--- VOLTAGE MINIMUMS ---");
//   Serial.println("MinVoltage (session): " + String(MinVoltage, 2) + " V");
//   Serial.println("MinVoltage_AllTime: " + String(MinVoltage_AllTime, 2) + " V");
//   Serial.println("  Current Voltage: " + String(BatteryV, 2) + " V");

//   // 9. Session average speed
//   Serial.println("\n--- SPEED ---");
//   Serial.println("AvgSpeed (session): " + String(AvgSpeed, 2) + " kts");
//   Serial.println("MaxSpeed (session): " + String(MaxSpeed, 2) + " kts");
//   Serial.println("MaxSpeed_AllTime: " + String(MaxSpeed_AllTime, 2) + " kts");
//   Serial.println("  Current SOG: " + String(SOGNMEA, 2) + " kts");

//   // 10. Travel distance
//   Serial.println("\n--- DISTANCE ---");
//   Serial.println("TotalDistance (session): " + String(TotalDistance, 2) + " nm");
//   Serial.println("TotalDistance_AllTime: " + String(TotalDistance_AllTime, 2) + " nm");

//   // Additional diagnostics
//   Serial.println("\n--- OTHER ---");
//   Serial.println("EngineCycles_AllTime: " + String(EngineCycles_AllTime));
//   Serial.println("ChargeCycles (session): " + String(ChargeCycles));
//   Serial.println("ChargeCycles_AllTime: " + String(ChargeCycles_AllTime));

//   Serial.println("\n--- BATTERY & ALTERNATOR STATE DEBUG ---");
//   Serial.println("BatteryCurrent_scaled: " + String(BatteryCurrent_scaled / 100.0, 1) + " A");
//   Serial.println("Bcur: " + String(Bcur, 1) + " A");
//   Serial.println("alternatorIsOn: " + String(alternatorIsOn ? "TRUE" : "FALSE"));
//   Serial.println("CurrentThreshold: " + String(CurrentThreshold) + " A");
//   Serial.println("hardwarePresent: " + String(hardwarePresent));

//   Serial.println("\n--- CHARGE TIME DEBUG ---");
//   Serial.println("AmpSensorRange: " + String(AmpSensorRange));
//   Serial.println("Bcur: " + String(Bcur, 2) + " A");
//   Serial.println("MeasuredAmps: " + String(MeasuredAmps, 2) + " A");
//   Serial.println("getTargetAmps(): " + String(getTargetAmps(), 2) + " A");
//   Serial.println("SOC_percent: " + String(SOC_percent / 100.0, 2) + " %");
//   Serial.println("BatteryCapacity_Ah: " + String(BatteryCapacity_Ah) + " Ah");
//   Serial.println("timeToFullChargeMin: " + String(timeToFullChargeMin));
//   Serial.println("timeToFullDischargeMin: " + String(timeToFullDischargeMin));

//   Serial.println("\n--- SETTINGS DEBUG ---");
//   Serial.print("BatteryCapacity_Ah raw: ");
//   Serial.println(BatteryCapacity_Ah);
//   Serial.print("BatteryCapacity_Ah type size: ");
//   Serial.println(sizeof(BatteryCapacity_Ah));
//   Serial.print("CurrentThreshold raw: ");
//   Serial.println(CurrentThreshold);
//   Serial.print("CurrentThreshold type size: ");
//   Serial.println(sizeof(CurrentThreshold));
//   Serial.print("getBatteryCurrent(): ");
//   Serial.println(getBatteryCurrent());
//   Serial.println("getBatteryCurrent(): " + String(getBatteryCurrent(), 2) + " A");

//   // 2. Engine hours
//   Serial.println("\n--- ENGINE RUNTIME ---");
//   Serial.println("EngineRunTime (session): " + String(EngineRunTime) + " seconds");
//   Serial.println("EngineRunTime_AllTime: " + String(EngineRunTime_AllTime, 4) + " hours");
//   Serial.println("  (Should be: " + String(millis() / 3600000.0, 4) + " hours based on uptime)");

//   // 5. Alternator on time
//   Serial.println("\n--- ALTERNATOR ON TIME ---");
//   Serial.println("AlternatorOnTime (session): " + String(AlternatorOnTime) + " seconds");
//   Serial.println("AlternatorOnTime_AllTime: " + String(AlternatorOnTime_AllTime, 2) + " minutes");
//   Serial.println("  (Should be: " + String(millis() / 60000.0, 2) + " minutes based on uptime)");

//   Serial.println("========================================\n");
// }



// void printBufferStatus() {
//   // Silent guards first - NO Serial operations until we know it's safe
//   if (systemShuttingDown) return;
//   if (core0Busy) return;

//   esp_task_wdt_reset();
//   delay(10);  // Let Serial/WiFi buffers settle

//   // Check heap silently
//   uint32_t freeHeap = ESP.getFreeHeap();
//   if (freeHeap < 10000) return;  // Less than 10KB free - abort silently

//   if (bufferedRecordCount == 0) {
//     return;
//   }

//   fsTakeLock();

//   File root = LittleFS.open(SENSOR_BUFFER_DIR);
//   if (!root || !root.isDirectory()) {
//     fsReleaseLock();
//     return;
//   }

//   int count = 0;
//   File file = root.openNextFile();

//   while (file) {
//     esp_task_wdt_reset();
//     if (!file.isDirectory()) {
//       count++;
//     }
//     file = root.openNextFile();
//   }

//   fsReleaseLock();

//   if (count != bufferedRecordCount) {
//     //    Serial.printf("WARNING: Count mismatch! bufferedRecordCount=%d but found %d files\n",
//     //bufferedRecordCount, count);
//   }
// }














//  // === SSID diagnostics (added) ===
//   Serial.printf("SSID raw: '%s'\n", ssid);
//   Serial.print("SSID hex: ");
//   for (size_t i = 0; i < strlen(ssid); i++) {
//     Serial.printf("%02X ", (uint8_t)ssid[i]);
//   }
//   Serial.println();

//   int n = WiFi.scanNetworks();
//   Serial.printf("scanNetworks found %d networks:\n", n);
//   for (int i = 0; i < n; i++) {
//     Serial.printf("%2d: '%s' RSSI=%d ch=%d enc=%d\n",
//                   i + 1,
//                   WiFi.SSID(i).c_str(),
//                   WiFi.RSSI(i),
//                   WiFi.channel(i),
//                   WiFi.encryptionType(i));
//   }
//   // === end diagnostics ===


// ============================================================================
// IDEAS RECORDED TO PROTECT FUTURE IMPLEMENTATIONS
// ============================================================================
// First published 2026-09-11 in the public repository
// https://github.com/XEngineeringLLC/XREG-010 (GPLv3). The commit date of this
// block in that repository is its publication date.
//
// None of the methods below are built. They are recorded as a public technical
// disclosure so that they are publicly known and dated, and so that XREG-010
// remains free to implement any of them later. Written to be enabling: where a
// threshold, a timing, or a failure mode is needed to actually build the thing,
// it is stated. Vague idea lists do not teach a person of ordinary skill how to
// practice a method, and a disclosure that does not teach is worth nothing.
//
// ---------------------------------------------------------------------------
// THE GENERAL METHOD
// ---------------------------------------------------------------------------
// Determining whether an engine is running, in an alternator regulator, with no
// dedicated ignition-sense wire and with no requirement that the alternator have
// a tachometer terminal, by either or both of:
//
//   (a) EXCITATION PROBE. While the run state is unknown and the field is off,
//       periodically apply a brief bounded excitation to the alternator field
//       and observe any sensed quantity that can only respond if the rotor is
//       turning. Run state follows from the presence, sign, magnitude, spectrum,
//       or time constant of that response.
//
//   (b) PASSIVE OBSERVATION. Observe a rotation-dependent or combustion-dependent
//       signal that needs no excitation at all.
//
// The sensed quantity is explicitly NOT limited to an alternator tachometer
// output. Any quantity that requires shaft rotation or engine combustion to
// change is a valid detector, and the variants below are examples, not limits.
//
// ---------------------------------------------------------------------------
// A. ALTERNATOR OUTPUT CURRENT AS THE PROBE RESPONSE
// ---------------------------------------------------------------------------
// Pulse the field, watch alternator output current (here: the Hall sensor on
// ADS1115 channel 1, MeasuredAmps). Rotor turning gives output; rotor stopped
// gives none, whatever the field is doing.
//   - Detect threshold must clear the sensor zero-drift band, not just its noise
//     floor. A few amps is realistic on a 500 A-class sensor.
//   - Probe duty MUST sit meaningfully above the per-RPM minimum-duty floor.
//     That floor is DEFINED as the most field that still produces about zero
//     output, so probing at the floor is guaranteed to report "stopped" on a
//     running engine. Use floor + a fraction of the span to the duty ceiling,
//     which keeps the bar install-relative and voltage-class-relative.
//   - Probe 0.5 to 2 s; long enough for field current to reach a useful fraction
//     of steady state through the field L/R, short enough not to matter.
//   - Needs no tachometer terminal and no battery shunt, only the alternator
//     current sensor already required for regulation.
//   - Failure mode, recorded deliberately: a thrown or badly slipping belt reads
//     as "engine stopped." For a regulator that is arguably the correct answer.
//
// ---------------------------------------------------------------------------
// B. FIELD CURRENT ITSELF AS THE PROBE RESPONSE
// ---------------------------------------------------------------------------
// Sense current in the field circuit (low-side shunt in the field return, or the
// current-sense output of the high-side switch) and read run state out of the
// field current alone. No alternator output sensor needed.
//   - Rotor standing: field current is a clean L/R exponential toward V/Rf, with
//     ripple only at the PWM carrier.
//   - Rotor turning: armature reaction and slot-harmonic coupling superimpose an
//     AC component on the field current at a frequency proportional to shaft
//     speed. Band-pass around the plausible range, compare band energy against a
//     standstill baseline captured when the engine is known stopped.
//   - The frequency of that component also yields a speed estimate, so this is a
//     tachometer substitute with no tachometer wire and no extra sensor.
//   - Slower corroborator on the DC level: a turning rotor heats, field winding
//     resistance rises, and steady current at a fixed duty decays over minutes.
//
// ---------------------------------------------------------------------------
// C. BUS VOLTAGE RESPONSE TO THE PROBE
// ---------------------------------------------------------------------------
// Works with neither a shunt nor an alternator current sensor.
//   - Sample bus voltage immediately before, during, and after the pulse.
//   - Turning alternator: the pulse LIFTS the bus.
//   - Stopped alternator: the pulse only adds field draw, which sags the bus
//     slightly.
//   - The SIGN of the step across the pulse is the discriminator, with magnitude
//     required to exceed the observed bus noise band over the same window so a
//     coincident load switching event is not read as a start.
//
// ---------------------------------------------------------------------------
// D. STATOR RIPPLE SIGNATURE ON THE BUS
// ---------------------------------------------------------------------------
// Rotation puts ripple on B+ at the stator fundamental, which for a three-phase
// machine is shaft speed times pole pairs times six per revolution after
// rectification. Detect the PRESENCE and FREQUENCY of that ripple rather than a
// tachometer edge.
//   - Residual rotor magnetism alone often produces detectable ripple at cruising
//     speed with the field completely off, so this can be a fully passive
//     detector with zero battery cost.
//   - With a probe pulse it becomes a strong detector at any speed.
//   - Reuses the ripple analysis path already in the firmware.
//
// ---------------------------------------------------------------------------
// E. INERTIAL / VIBRATION DETECTION
// ---------------------------------------------------------------------------
// The board already carries an IMU (LSM6DSOX). A running engine puts broadband
// vibration into the hull with a peak at the firing frequency, which for a
// four-stroke is shaft speed times half the cylinder count.
//   - Separate from sea state by band: wave-induced hull motion is below roughly
//     1 Hz, engine vibration lives from roughly 10 to 200 Hz.
//   - Declare running when energy in the engine band exceeds a learned at-rest
//     baseline; refine an RPM estimate from the peak frequency.
//   - Needs no excitation, costs no battery current, and still works when the
//     belt is off, which is exactly where the excitation probes fail.
//   - Baseline is captured while the engine is known stopped, and re-learned as
//     the mounting ages.
//
// ---------------------------------------------------------------------------
// F. ACOUSTIC DETECTION
// ---------------------------------------------------------------------------
// Microphone or a piezo bonded to the enclosure, same spectral argument as E: a
// level step above a learned quiet baseline, or a peak at the firing frequency.
// Cheapest possible added sensor, and the only one that needs no mechanical
// coupling to the engine.
//
// ---------------------------------------------------------------------------
// G. TEMPERATURE PATTERN
// ---------------------------------------------------------------------------
// A running engine warms the alternator body even at zero field output, through
// belt friction and engine bay air.
//   - Running: sustained positive rate of rise, or a sustained differential
//     against board or ambient temperature.
//   - Stopped: monotonic decay toward ambient with a time constant near the known
//     cool-down tau for that installation.
//   - Minutes-scale, so this is a corroborator and a stopped-engine confirmer,
//     never the primary start detector.
//
// ---------------------------------------------------------------------------
// H. RUN STATE ALREADY ON THE BOAT'S DATA BUSES
// ---------------------------------------------------------------------------
// Engine speed and engine hours from NMEA 2000 (PGN 127488 / 127489), NMEA 0183
// engine sentences, RV-C, or a SignalK feed, taken as a run-state source behind
// a staleness gate. Free wherever the boat already has any of them, and the
// firmware already receives all of these.
//
// ---------------------------------------------------------------------------
// I. FUSION AND ADAPTIVE PROBING
// ---------------------------------------------------------------------------
//   - Vote across any subset of A through H, each channel carrying its own
//     confidence and staleness, declaring running on an N-of-M or weighted score
//     rather than on any single channel.
//   - Probe cadence adapts rather than being fixed: back off geometrically while
//     the answer stays "stopped" (for example 15 / 30 / 60 / 120 s, capped, with
//     decay back to the fast tier after a quiet period), and collapse immediately
//     to the fast tier when any zero-cost channel hints at a start (a vibration
//     step, a temperature rise, a bus message). A real start is then caught
//     quickly without paying continuous battery draw while the boat sits.
//   - Skip the excitation probe entirely for as long as a no-excitation channel
//     (D, E, F, G, H) is answering with sufficient confidence.
//
// ---------------------------------------------------------------------------
// J. THE INVERSE: PROVING A RUN SIGNAL FALSE
// ---------------------------------------------------------------------------
// The same probe-and-observe primitive run backwards detects a LYING speed
// signal: field driven above the onset floor, control loop demanding current,
// and near-zero alternator output held past a dwell, means the speed reading is
// not coming from a turning alternator.
//
// This direction is BUILT AND SHIPPED, and its public dates are the ones that
// matter for anything above:
//   - engine-stopped zero-hold cut (RPM_ZERO_CUT_MS, tick.engineFullyStopped),
//     public 2026-06-23, commit 92f37f2
//   - tach-lie detector and escalating lockout ladder (tick.tachImplausible,
//     REASON_TACH_IMPLAUSIBLE, nextTachLieLockoutMs), public 2026-07-23,
//     commit 238b1f3, where the lockout tier is explicitly the back-off interval
//     between field-energize probes
//
// ---------------------------------------------------------------------------
// SCOPE OF THIS DISCLOSURE
// ---------------------------------------------------------------------------
// Intended to cover all combinations of the above, any sensed quantity that
// requires shaft rotation or engine combustion to change, any excitation
// waveform (single pulse, pulse burst, duty ramp, stepped duty, dither
// superimposed on an existing drive), and any probe cadence (fixed interval,
// adaptive back-off, event-triggered, or one-shot on power-up).
// ============================================================================


// ============================================================================
// IDEAS RECORDED TO PROTECT FUTURE IMPLEMENTATIONS, PART 2:
// FAST FIELD DE-EXCITATION (REMOVING FIELD CURRENT FASTER THAN L/R ALLOWS)
// ============================================================================
// Added 2026-09-13 to the public repository
// https://github.com/XEngineeringLLC/XREG-010 (GPLv3). The commit date of this
// block in that repository is its publication date. The concept was first
// published 2026-08-29 in the blog post
// https://xengineering.net/blog/what-the-car-industry-uses/ under the headings
// "Why put a full bridge on a DC field?" and "Faster demagnetization without
// touching the brushes". This block is the enabling detail behind that post
// plus the variants the post did not cover.
//
// None of the hardware below is built. Same purpose and same rule as Part 1:
// written so that a person of ordinary skill can build it, with the numbers
// that decide whether it works stated.
//
// ---------------------------------------------------------------------------
// WHAT EXISTS TODAY, AND WHY IT IS SLOW
// ---------------------------------------------------------------------------
// The field power stage is a single N-channel switch (Q3) on one field
// terminal. The other end of the winding is tied inside the alternator to
// ground (P-type, high-side drive) or to B+ (N-type, low-side drive). When Q3
// turns off, the winding current freewheels through a diode: D5 to ground in
// P-type, the return diode to the BAT+ input in N-type. In both cases the
// winding sees only one diode drop opposing its current, so the decay is the
// winding's own L/R exponential:
//
//   v = L di/dt + i R,  v = -Vf  ->  i(t) = (I0 + Vf/R) exp(-t/tau) - Vf/R,
//   tau = L/R,  output at 10 % after  t10 = tau ln((I0 R + Vf)/(0.1 I0 R + Vf))
//
// Bench machine, measured: R = 3.0 ohm (4.2 A at 12.8 V), tau in the tens of
// milliseconds, so L about 0.1 H and stored energy 0.5 L I^2 about 0.8 J at
// 4 A. The command-to-10 %-output drain time is about 60 ms (the model gives
// 62 ms with Vf = 0.8 V). The firmware already measures this per installation
// in the commissioning wizard's field-decay test and stores it as a
// drain-vs-RPM line (fieldDecayTauMs, fdDrainLoMs/fdDrainHiMs, read back by
// fdDrainMsAtRpm), and the soft-clamp early release already consumes it. Those
// measured numbers are the baseline everything below is judged against.
//
// One caveat that applies to every method here, H-bridge included: the OUTPUT
// follows the winding current with an extra lag from eddy currents in the
// solid claw-pole rotor. That lag is a property of the machine and is the
// floor on how fast output can be removed by any means. The commissioning
// test measures output, so it captures that lag; the winding-current times
// below do not.
//
// Consequence of the diode path: the regulator can add field far faster than
// it can remove it. A 4 A field keeps the alternator producing for about
// 60 ms after the switch opens, into a bus that has already been judged too
// high, or into a BMS that has just opened its charge path.
//
// ---------------------------------------------------------------------------
// THE PHYSICS OF DOING IT FASTER
// ---------------------------------------------------------------------------
// Replace the diode drop with a larger opposing voltage Vc while the current
// decays:
//
//   di/dt = -(Vc + i R)/L
//   time to zero current  t0 = (L/R) ln(1 + I0 R / Vc)  ~  L I0 / Vc  when Vc >> I0 R
//
// Bench machine (L 0.1 H, R 3 ohm, I0 4 A), winding current to zero:
//   diode, Vf 0.8 V     92 ms   (output at 10 % by ~60 ms)
//   Vc = 12 V           23 ms
//   Vc = 24 V           13 ms
//   Vc = 48 V            7 ms
//   Vc = 80 V            5 ms
//
// The energy that must go somewhere is fixed by the machine, not by Vc:
// essentially all of 0.5 L I0^2 (0.8 J here; it scales with L and with I^2,
// so a 10 A large-frame field with similar L stores about 5 J). Vc only sets
// how fast that energy is delivered. So the clamp is chosen by energy per
// event and by repetition rate, never by peak power alone, and whatever
// absorbs the energy (avalanche diode, Zener, a MOSFET in linear mode, a
// capacitor) must be rated for that pulse at the pulse width above. The field
// switch must withstand Vbus(max) + Vc + Vf in either wiring type. With the
// 150 V switch planned for the next board and a 48 V class bus (about 62 V
// max), Vc can be up to about 75 V; with an 80 V switch on a 12 V class bus,
// about 55 V; an 80 V switch on a 48 V class bus has no clamp headroom at all,
// so the 150 V switch is a prerequisite there. The clamp element is oriented
// so that the freewheel current flows through it in its breakdown direction.
//
// ---------------------------------------------------------------------------
// A. FIXED HIGH-VOLTAGE CLAMP IN THE FREEWHEEL PATH (ALWAYS ON)
// ---------------------------------------------------------------------------
// The direct reading of the blog post: a clamp element in series with the
// freewheel diode (P-type: ground -> clamp -> D5 -> field terminal; N-type:
// field terminal -> return diode -> clamp -> BAT+), or a Zener from the
// switch's drain to its gate so the switch itself clamps in linear mode.
// Every turn-off then decays at Vc.
//
// Recorded with its defect stated, because the defect is what shapes B: a PWM
// regulator turns the switch off several hundred times a second, and with an
// always-on clamp every one of those off-intervals dissipates Vc x I in the
// clamp instead of Vf x I in a diode. At 4 A, 50 % duty and Vc = 48 V that is
// about 96 W continuous against 1.6 W for the diode, and the field-current
// ripple grows by the same ratio. The dissipation is Vc x I x (1 - duty) and
// does not fall with PWM frequency. An always-on clamp is therefore viable
// only where the switch does not chop in normal operation (a linear or very
// low-frequency drive, or a drive whose normal off-state is a low-voltage
// recirculation as in an H-bridge), or with a small Vc (a few volts: a few
// times faster for a few times the loss).
//
// ---------------------------------------------------------------------------
// B. SWITCH-SELECTABLE DECAY PATH (THE CORE OF THIS DISCLOSURE)
// ---------------------------------------------------------------------------
// Two freewheel paths on one field terminal, selected by the regulator:
//
//   SLOW path: the existing diode (Vf). Used for every normal PWM off-interval.
//   FAST path: the diode in series with a clamp element at Vc. Used only when
//              the regulator has decided the field must go away now.
//
// Simplest realisation: the clamp element sits permanently in series with the
// freewheel diode, and a bypass switch is connected across the clamp element.
// Bypass closed = clamp shorted out = slow path. Bypass open = fast path.
// Equivalent forms: two parallel freewheel branches with a series switch in
// the slow one; a half-bridge mid-node whose return rail is either the supply
// or a clamp rail; a drain-gate Zener on the field switch with a small series
// switch that enables it.
//
// Design rules for the bypass switch, because they are where a build fails:
//   - Its body diode must be REVERSE to the freewheel current, or the body
//     diode bypasses the clamp permanently and the fast path never exists.
//     With an N-channel device in the P-type branch that means drain on the
//     ground side; in the N-type branch, source on the BAT+ side.
//   - The switch is static (held on for minutes at a time), so a bootstrap
//     gate supply is unsuitable. Use a charge-pump high-side driver (the same
//     class of part that already drives the input-protection MOSFETs on this
//     board), a photovoltaic isolator driving the gate, or a P-channel device
//     with a gate-source Zener where its body-diode orientation permits. The
//     gate must be referenced to the switch's own source: during a clamp event
//     that node moves by Vc, and a ground-referenced gate would turn the
//     switch back on and defeat the clamp.
//   - Fail-safe direction: loss of gate drive leaves the bypass OPEN, so a
//     dead regulator, a dead driver rail or a hung processor gets the fast
//     path. The winding always has at least one path; nothing here can leave
//     it open.
//   - The bypass carries the freewheel current continuously (every normal
//     off-interval) at a few milliohms; the clamp element is rated for the
//     pulse energy above and its repetition.
//
// Sequencing on a fast cut, inside one control tick:
//   1. open the bypass (no current flows in the branch while the field switch
//      is on, so this costs nothing and may precede the cut by any amount),
//   2. drop PWM to zero and de-assert field enable (today's applyImmediateCut),
//   3. the decay runs at Vc. The field-terminal voltage sits at Vbus + Vc
//      (N-type) or at -Vc (P-type) for exactly as long as current flows and
//      steps back to the supply level when it reaches zero. That step is an
//      exact "decay complete" edge: a comparator or divider on the field
//      terminal delivers it to a GPIO,
//   4. close the bypass once current is zero (from the edge in 3, from a
//      field-current shunt if fitted, or from a timer set to twice the
//      predicted t0), so any restart runs at low loss.
//
// Which events select the fast path, and which do not:
//   FAST: the software fast-overvoltage cut and the timed overvoltage tiers,
//         the current monitor's hardware overvoltage alert, load-dump
//         detection from battery-current slope, a BMS charge-stop or
//         pre-disconnect input, a charge-limit step from a supervisory bus
//         (DVCC, NMEA 2000, RV-C) larger than the loop can follow, and any
//         protection cut taken while the bus is measured to be rising.
//   SLOW: every normal control decrement, the soft overvoltage clamp to the
//         duty floor, thermal derates, the engine-stopped cut, the
//         implausible-tach cut, manual mode, and every commissioning sweep.
//   GRADED rule that decides without a table: predict the time for the bus to
//         reach the next protection rung or the BMS floor from the measured
//         bus slope; if that time is shorter than the commissioned slow drain
//         time at the present RPM, take the fast path, otherwise the slow one.
//         The clamp is then used only when the slow path would lose.
//
// What it buys in the protection ladder: the working law "cut a sustained
// violation within 0.4 s" is 0.5 s of BMS margin minus about 100 ms of
// de-energisation. With a fast path the subtraction is about 10 ms, so every
// threshold can sit closer to its ceiling for the same BMS margin. A load-dump
// surge on a BMS disconnect lasts as long as the rotor stays magnetised, so it
// shortens by the same ratio, which shrinks the energy rating the regulator's
// own input clamp needs.
//
// ---------------------------------------------------------------------------
// C. ENERGY-RECOVERY VARIANT: FREEWHEEL INTO A CAPACITOR
// ---------------------------------------------------------------------------
// Replace the dissipative clamp element with a capacitor pre-charged to the
// supply node (ground for P-type, BAT+ for N-type). On a fast cut the
// freewheel current charges the capacitor beyond the supply; the opposing
// voltage rises as the current falls, and the current reaches zero in about a
// quarter period of the L-C pair:
//
//   C >= L I0^2 / (Vmax^2 - Vsupply^2)      t0 ~ (pi/2) sqrt(L C)
//
// Bench machine, Vmax 60 V on a 12 V bus: C about 460 uF, t0 about 11 ms,
// comparable to a 48 V clamp, with the 0.8 J stored rather than burned. The
// stored energy is then returned: through a bleed resistor sized for average
// power (the dissipation is spread over seconds instead of milliseconds, so
// an ordinary resistor replaces a pulse-rated clamp), or through a small
// converter back into the battery or the gate-driver rail. The slow/fast
// selection is a switch that holds the capacitor at the supply level (slow)
// or lets it float (fast); its body-diode rule is the same as in B.
//
// ---------------------------------------------------------------------------
// D. ACTIVE REVERSE DRIVE WITHOUT A SECOND BRUSH
// ---------------------------------------------------------------------------
// A full H-bridge needs both winding ends, which a standard alternator does
// not bring out. The same reverse voltage can be applied through the ONE
// accessible terminal by driving it beyond the rail the far end is tied to:
//   P-type (far end at ground): a negative rail on the regulator, generated by
//     a small inverting converter or charge pump from the gate-driver supply,
//     and a switch from the field terminal to that rail.
//   N-type (far end at B+): a rail above B+ (a boost from the bus) and a
//     switch from the field terminal to it.
// The rail needs almost no continuous capability: it only has to absorb one
// field's worth of energy per event, so it is a capacitor held at the target
// voltage plus a bleed, which is variant C pre-biased so the full opposing
// voltage is present from the first microsecond instead of ramping from zero.
// The switch to the rail is timed or current-limited, and the decay-complete
// edge of B applies unchanged.
//
// ---------------------------------------------------------------------------
// E. DEGAUSS PULSE: REMOVING RESIDUAL OUTPUT, NOT ONLY FIELD CURRENT
// ---------------------------------------------------------------------------
// After the current reaches zero the rotor keeps its remanent magnetism, which
// is why an alternator with the field off still shows stator ripple and a
// small output at speed. Variant D (or an H-bridge) can push a short REVERSE
// current through the winding after the decay: a small fraction of rated
// field, of the order of 5 to 10 %, for tens of milliseconds. The right
// amplitude is found by the regulator itself: step the reverse pulse up until
// the stator ripple amplitude, or the measured output at speed, reaches its
// minimum, then store that value per installation. Uses: a true zero-output
// state during a BMS disconnect or an overvoltage lockout; a clean zero
// baseline for the excitation-probe and field-decay tests in Part 1; and
// removal of the residual-magnetism output that otherwise sets how low the
// bus can be held with the battery disconnected. Excitation from the
// regulator does not depend on remanence, so the pulse costs nothing at the
// next start.
//
// ---------------------------------------------------------------------------
// F. CONTROLLED DOWNWARD SLEW RATE (SYMMETRIC RAMPING)
// ---------------------------------------------------------------------------
// Chopping the bypass of B (or the switch of D) at the field PWM rate with a
// second duty d_fast makes the average opposing voltage adjustable:
//
//   V_opposing = Vf + d_fast (Vc - Vf)
//
// so the field current can be commanded DOWN at any rate between the L/R
// decay and the full clamp, as it can already be commanded up through the
// field duty. Today only the upward rate is controllable. Uses: limiting how
// fast alternator torque is removed from the engine (a step torque release is
// a throttle transient to a small diesel's governor); holding a belt below
// its slip threshold on release; shaping the bus dip and overshoot on a large
// target step; and giving the CV wind-down and hunt governors a downward rate
// they can actually achieve rather than the one the winding imposes. The
// commanded rate is bounded by the clamp's thermal budget in G.
//
// ---------------------------------------------------------------------------
// G. CLAMP ENERGY BUDGET AND SELF-TEST IN FIRMWARE
// ---------------------------------------------------------------------------
//   - Per-event energy E = 0.5 L I0^2 with L = tau x R: R from the commissioned
//     field resistance (FieldResistance, or measured volts over amps when a
//     field shunt exists), tau from the commissioned drain time, I0 from the
//     field volts and R at the instant of the cut.
//   - Integrate E per event into a one-pole thermal model of the clamp element
//     (rated energy per pulse and cooling time constant), and withhold the
//     fast path, falling back to the slow one, while the modelled temperature
//     is above its limit. The slow path is always safe; the budget only ever
//     withholds the fast one.
//   - Count and log every clamp event with I0, E and the measured decay time.
//   - Self-test inside the commissioning field-decay test: run the cut once
//     with the bypass closed and once open. The open run must be faster by at
//     least the ratio the physics above predicts from Vc, Vf and I0 R. Equal
//     times mean the bypass is stuck closed (no fast path); a closed run as
//     fast as the open one means the bypass is stuck open (the clamp is
//     dissipating on every PWM cycle and will be destroyed). Either state is
//     an alarm, and the second one also disables the field until cleared.
//
// ---------------------------------------------------------------------------
// H. OPENING THE SWITCH FASTER: GATE THE PWM, NOT ONLY THE DRIVER SUPPLY
// ---------------------------------------------------------------------------
// Today both the firmware field-enable line and the current monitor's
// hardware alert act on the field switch by disabling the boost converter
// that feeds the gate driver; the switch opens only when that rail has decayed
// to the driver's undervoltage lockout, tens of milliseconds set by the rail's
// capacitance and the driver's quiescent draw. The firmware cut also drops the
// PWM to zero, which opens the switch within one PWM period; the hardware
// alert alone does not. Faster and cheaper: gate the PWM signal at the driver
// input with the alert and the enable (a logic gate, or the driver's own
// enable pin) so any of them opens the switch within the current PWM period,
// and route the alert to a processor interrupt that stops the PWM directly
// instead of waiting for the next control tick. Combined with B, the same
// alert also opens the bypass.
//
// ---------------------------------------------------------------------------
// I. WHERE THE HARDWARE LIVES
// ---------------------------------------------------------------------------
// Any of A to H on the regulator board; as a potted module at the regulator's
// field/power connector (the form the present N-type return diode and crowbar
// take); as a module at the alternator's field terminal or inside the brush
// holder, commanded by the regulator over the field wire itself (a signature
// on the field waveform, or the field-terminal voltage level) or over a
// separate line; or built into an alternator that brings one field terminal
// out. The disclosure is not limited by where the parts sit.
//
// ---------------------------------------------------------------------------
// PRIOR ART ACKNOWLEDGED
// ---------------------------------------------------------------------------
// Clamped fast demagnetisation of inductive loads is standard practice in
// solenoid and injector drivers (TI and ST both describe fast-decay clamps in
// their automotive driver literature); full H-bridge rotor excitation with
// rapid demagnetisation is published for 48 V belt starter-generators; and at
// least one automotive regulator IC carries an active freewheeling MOSFET.
// What is recorded here is the application of those elements to a
// single-switch aftermarket alternator regulator on one field terminal: the
// selectable slow/fast path with its sequencing and event rules, the
// recovery and reverse-drive forms, the degauss pulse, the symmetric slew
// control, the direct PWM gating, and the firmware that budgets, verifies
// and logs all of it.
//
// ---------------------------------------------------------------------------
// SCOPE OF THIS DISCLOSURE
// ---------------------------------------------------------------------------
// Intended to cover all combinations of A to I on either wiring type, any
// clamp element (avalanche diode, Zener, MOSFET in linear mode, capacitor,
// converter, resistor, or a rail beyond the far-end supply), any selection
// mechanism (series bypass switch, parallel branch switch, half-bridge rail
// selection, enable of a drain-gate clamp), any trigger (voltage level,
// voltage slope, current slope, external request, predicted time to
// threshold), any decay-complete detection (terminal-voltage edge,
// field-current sense, timer), any decay-rate modulation, and any location
// of the hardware.
// ============================================================================


// ============================================================================
// IDEAS RECORDED TO PROTECT FUTURE IMPLEMENTATIONS, PART 3:
// ENGINE AND BELT DIAGNOSTICS FROM STATOR-PHASE EDGE TIMING
// ============================================================================
// Added 2026-09-13. The commit date of this block in the public repository
// https://github.com/XEngineeringLLC/XREG-010 is its publication date. Not
// built. Same rule as Parts 1 and 2: enabling detail, numbers stated.
//
// ---------------------------------------------------------------------------
// WHAT EXISTS TODAY
// ---------------------------------------------------------------------------
// Engine speed comes from one stator phase through a frequency-to-voltage
// converter (LM2907) into the ADS1115, read about every 30 ms, then scaled by
// a hand-entered factor (RPMScalingFactor) and a hand-entered pulley ratio
// (PulleyRatio) that the operator aligns against the boat's own tachometer.
// The converter's coupling network has a 2 s time constant, so an abrupt loss
// of stator amplitude gives several seconds of false zero. The speed axis is
// therefore a calibration, not a measurement, and its resolution is that of
// an analog voltage. Separately, the fast alternator-current channel (tens of
// kilosamples per second on the ESP32 ADC) already does ripple and rectifier
// fault analysis on AMPLITUDE. Automotive regulator ICs measure phase
// frequency directly and divide by a programmed pole-pair count, to about
// 10 %. None of this resolves what happens INSIDE one revolution.
//
// ---------------------------------------------------------------------------
// THE SIGNAL
// ---------------------------------------------------------------------------
// Timestamp every zero crossing of one stator phase in hardware and treat the
// interval sequence as an instantaneous angular speed (IAS) signal.
//
// Arithmetic: a claw-pole alternator with p pole pairs puts p electrical
// cycles per rotor revolution on each phase, so 2p zero-crossing edges per
// rotor revolution (12 for the common p = 6). With pulley ratio r that is 2pr
// edges per engine revolution: 30 at r = 2.5, one edge every 12 degrees of
// crank. A four-stroke fires N/2 times per revolution, so a four-cylinder
// gives about 15 edges per firing event and a two-cylinder about 30, which
// resolves the speed rise and fall of every firing event several times over.
//
// Front end: AC-couple the phase tap through a divider into a comparator with
// a few tens of millivolts of hysteresis (or a dedicated zero-crossing
// detector), output to an ESP32-S3 capture peripheral: MCPWM capture for
// edge timestamps at the 80 MHz peripheral clock (12.5 ns), PCNT where only a
// count is needed, RMT for a raw edge stream, or a GPIO interrupt with the
// microsecond timer. At a 125 Hz phase (4 ms between edges) 12.5 ns is 3 ppm;
// the real noise floor is comparator threshold jitter on the stator waveform,
// of the order of 0.01 to 0.1 % per interval, which is still ten to a hundred
// times finer than the firing-event modulation being measured. The analog
// converter can stay for its low-amplitude idle behaviour or be replaced; a
// low comparator threshold also reads residual-magnetism edges with the field
// off at cruising speed, which Part 1 D relies on.
//
// ---------------------------------------------------------------------------
// A. THE TWO CORRECTIONS THAT MAKE THE SIGNAL USABLE
// ---------------------------------------------------------------------------
// 1. Pole-geometry correction. The p pole pairs are not identical, so each of
//    the 2p intervals in a rotor revolution carries a fixed geometric error,
//    up to a few percent, exactly the "tooth error" of a trigger wheel. Learn
//    it: at steady speed, average each interval position modulo 2p over many
//    revolutions into a per-edge correction table, then divide every measured
//    interval by its table entry. The table is a property of the alternator
//    and is stored per installation.
// 2. Rotor reference from that same signature. Because the 2p-interval
//    pattern repeats every rotor revolution, autocorrelation of the raw
//    interval sequence peaks at lag 2p. That gives p with no user entry, and
//    it gives every edge its index within the revolution, i.e. a virtual
//    once-per-revolution mark with no sensor. Rotor revolutions are then
//    counted exactly, and with the pulley ratio, engine revolutions and crank
//    angle to within the belt's compliance.
//
// The belt: it is a spring between crank and rotor, and with the rotor's
// inertia forms a resonance typically in the tens of hertz, so the rotor sees
// the crank's torsional motion through a resonant low-pass plus any slip.
// Two ways to live with that, both recorded: characterise the transfer per
// installation by comparing the IAS spectrum at the firing frequency against
// the IMU's vibration spectrum at the same frequency across engine speed (the
// IMU sees the block, not the belt); or use only RELATIVE comparisons within
// one engine cycle at one speed (cylinder against cylinder), where the
// transfer cancels to first order. The regulator's own field steps also
// modulate rotor speed; it knows exactly what it commanded, so it subtracts
// them, or uses them deliberately (D below).
//
// ---------------------------------------------------------------------------
// B. CYCLE SEGMENTATION AND PER-CYLINDER METRICS
// ---------------------------------------------------------------------------
// Resample the corrected IAS onto crank angle using the virtual mark and the
// pulley ratio (order tracking), then fold by the engine cycle: one
// revolution for a two-stroke, two for a four-stroke. Which one, and the
// cylinder count, are read off the data: the number of speed humps per cycle
// and whether the pattern repeats every revolution or every second one. Per
// segment compute the speed gained across the firing event (the classic IAS
// misfire metric) and its variance over many cycles.
//   - Misfire: one segment's gain collapses; count events per thousand cycles.
//   - Weak cylinder: one segment persistently below the others by more than a
//     stated fraction (a few percent at steady load), trended over engine
//     hours like the existing alternator-health trends.
//   - Rough running index: variance of the per-segment gains, normalised by
//     load and speed.
//   - Cylinder labelling: without a reference the report reads "cylinder k of
//     N in firing order". A one-time operator action while the engine runs
//     (cracking one injector line, or lifting one decompression lever on
//     engines that have them) labels segment k as that physical cylinder;
//     the label is stored.
//
// ---------------------------------------------------------------------------
// C. BELT SLIP AND BELT CONDITION
// ---------------------------------------------------------------------------
//   - Ratio drift: rotor revolutions are exact (A.2). Against ANY independent
//     engine-speed source (NMEA 2000 PGN 127488, NMEA 0183, RV-C, or the IMU's
//     firing frequency) the ratio falls below the commissioned pulley ratio by
//     the slip fraction. Report slip in percent; alarm above a threshold.
//   - Slip on load step, no second source needed: a field step is a torque
//     step at the rotor. On a slipping belt the rotor decelerates against the
//     belt while the block does not, so the IAS drops more than the IMU firing
//     frequency does. The size of the mismatch is the slip.
//   - Micro-slip: excess randomness in the interval sequence beyond the
//     geometric and firing components (residual variance after A and B) is a
//     belt-condition index; it rises before gross slip.
//   - Tension: the belt resonance frequency scales with the square root of
//     tension; its drift over engine hours is a tension-loss indicator, read
//     from the IAS spectrum where an engine order crosses it.
//   - Control action: at engine speeds where an order crosses the belt-drive
//     resonance, cap alternator torque (field) to hold belt tension
//     oscillation down, and tell the operator which speeds those are.
//
// ---------------------------------------------------------------------------
// D. THE ENGINE AS A PLANT: GOVERNOR CHARACTERISATION BY THE REGULATOR
// ---------------------------------------------------------------------------
// The regulator is a controllable load on the engine. Step the field by a
// known amount (a known electrical power step, from output current and bus
// voltage), record the speed droop and its recovery from the IAS series, and
// fit engine droop (rpm per kW) and the governor's time constant, per
// installation, at several speeds. Use them to set the charge-rate ramp
// limits and the downward slew of Part 2 F so the regulator never demands a
// torque step the governor cannot follow, to detect governor or fuel-system
// degradation as those numbers drift over engine hours, and to size the
// probe of Part 1 A. Run it inside commissioning and repeat it opportunistically
// at steady cruise.
//
// ---------------------------------------------------------------------------
// E. CRANKING RELATIVE COMPRESSION
// ---------------------------------------------------------------------------
// During cranking apply a probe-level field (below the output knee, so no
// electrical load on the starter) and record the IAS through the cranking
// revolutions. Each compression stroke slows the crank in proportion to that
// cylinder's compression; the per-cylinder dips, compared against each other
// and against their own history, are the relative-compression test that a
// mechanic does with a clamp meter on the starter cable, done automatically
// at every start. Cranking is detected from the ignition input, from bus
// voltage sag, or from the Part 1 methods.
//
// ---------------------------------------------------------------------------
// F. OUTPUT
// ---------------------------------------------------------------------------
// Telemetry: engine speed at edge resolution, per-cylinder gain bars, misfire
// count, rough-running index, slip percent, belt-condition index, droop and
// governor time constant, cranking compression bars; trends over engine
// hours; alarms on thresholds the operator sets. Engine speed with no
// hand-entered scale where a second source exists to calibrate against.
//
// ---------------------------------------------------------------------------
// PRIOR ART ACKNOWLEDGED
// ---------------------------------------------------------------------------
// Misfire detection from crankshaft-sensor instantaneous angular speed is old
// and widely patented in automotive; tachometers driven from alternator
// phase or ripple are older still; tooth-error learning on trigger wheels is
// standard. What is recorded here is doing the IAS analysis on the alternator
// phase inside the charge regulator: the pole-signature virtual mark and
// automatic pole-pair count, the belt-path handling and the belt-slip and
// tension methods, the governor characterisation using the regulator's own
// load steps, the cranking compression test under a probe field, the
// resonance-avoidance torque cap, and their use for charge-rate limits.
//
// ---------------------------------------------------------------------------
// SCOPE OF THIS DISCLOSURE
// ---------------------------------------------------------------------------
// Intended to cover any edge-timing capture of any stator-derived signal
// (phase tap, neutral, rectified ripple, or the current channel's zero
// crossings), any geometric correction and reference derived from the
// pole pattern, any belt-path treatment, any per-cylinder or per-cycle
// metric derived from the interval sequence, any use of the regulator's own
// field as the excitation for engine or belt identification, and any
// resulting control action or report.
// ============================================================================


// ============================================================================
// IDEAS RECORDED TO PROTECT FUTURE IMPLEMENTATIONS, PART 4:
// FIELD-CURRENT MEASUREMENT ON A SINGLE-TERMINAL DRIVE, AND WHAT IT ENABLES
// ============================================================================
// Added 2026-09-13. The commit date of this block in the public repository
// https://github.com/XEngineeringLLC/XREG-010 is its publication date. Not
// built. Same rule as the other parts.
//
// ---------------------------------------------------------------------------
// WHAT EXISTS TODAY
// ---------------------------------------------------------------------------
// Field current is calculated, not measured: field volts (duty times bus
// voltage) divided by FieldResistance, a typed setting of 2 to 6 ohm that
// moves 10 to 20 % with winding temperature. The minimum-field floor is
// corrected for that drift with the CASE probe temperature as a proxy, using
// copper's coefficient (0.00218 per degree F). Automotive regulator ICs
// measure field current to about 0.25 A and use it for a current limit,
// torque reporting to the engine computer, and a rotor-fault flag. This
// block records how to measure it on a one-terminal drive and everything the
// measurement unlocks.
//
// ---------------------------------------------------------------------------
// A. SENSING TOPOLOGIES ON ONE FIELD TERMINAL
// ---------------------------------------------------------------------------
// 1. One low-side shunt serving BOTH wiring types. On this board the N-type
//    jumper lands the switch's source on ground, and the P-type freewheel
//    diode's anode is on ground. Bring both to a common node and put a shunt
//    between that node and ground:
//      N-type: the ON-interval current flows switch -> node -> shunt -> ground.
//      P-type: the OFF-interval (freewheel) current flows ground -> shunt ->
//              node -> diode -> field terminal, the opposite sign.
//    Inductor current is continuous, so the first off-interval sample equals
//    the last on-interval sample; the switching-edge current is captured
//    exactly in both types, and the cycle average follows from the ripple
//    model or from a second sample. Sampling is keyed to the PWM edge by the
//    same timer that generates it. Ground-referenced, so an ordinary
//    amplifier and no common-mode problem. The SIGN of the shunt current
//    tells the firmware which wiring type is installed, so the type becomes
//    self-detected instead of a setting.
// 2. Half-bridge mid-node shunt with a high-common-mode, PWM-rejecting
//    current-sense amplifier (100 V class): continuous in both types.
// 3. Hall-effect sensor on the field wire or a board trace: isolated, both
//    types, no change to the power stage, bandwidth of 100 kHz-class parts is
//    ample.
// 4. The switch's own on-resistance as the shunt (drain-source drop during
//    the on-interval, corrected for temperature from the board probe): coarse
//    but free.
// 5. A current-sense output on the switch or driver IC where one exists.
//
// ---------------------------------------------------------------------------
// B. GETTING R AND L OUT OF THE SAMPLES
// ---------------------------------------------------------------------------
//   - Steady state, cycle average: the average of L di/dt over one full PWM
//     period is zero, so  R = duty x (Vbus - Vswitch) / I_avg  exactly, with
//     no knowledge of L. This is the workhorse: one number per PWM period.
//   - Within the on-interval, two samples give  V = L di/dt + i R  twice, so
//     L and R both, per period. L feeds Part 2 (energy per event, decay
//     prediction) and the OV early-release drain estimate continuously
//     instead of only at commissioning.
//   - The brush and slip-ring contact resistance (tens of milliohms) and the
//     wiring are inside the measured R as a near-constant offset; they are
//     calibrated into the reference value and their drift is a signal (E).
//
// ---------------------------------------------------------------------------
// C. ROTOR WINDING TEMPERATURE BY RESISTANCE
// ---------------------------------------------------------------------------
//   T = T_ref + (R / R_ref - 1) / alpha,   alpha = 0.00393 per degree C
//
// The reference pair (R_ref, T_ref) is taken when the winding is known to be
// at the case probe's temperature: at commissioning, and at any start after
// the regulator has seen the engine off long enough for the case temperature
// to have settled at ambient (it tracks both). A 1 % error in R is 2.5 C.
// The winding is the hottest part of the machine and the part whose
// insulation class sets the limit; the case probe lags it by minutes and
// under-reads it by tens of degrees, worst at idle in a hot engine room where
// the fan moves least air. Measuring the winding directly lets the
// temperature limit be set on the winding, with the case probe demoted to a
// cross-check, and it catches the fast heating case the probe cannot.
//
// ---------------------------------------------------------------------------
// D. CONTROL USES
// ---------------------------------------------------------------------------
// 1. Inner loop in amperes. The inner control loop today outputs duty, and
//    its plant gain (output amps per percent duty) moves with R (temperature)
//    and with bus voltage. Closing the inner loop on measured field current
//    removes both dependencies: commissioned gains then hold over temperature
//    and voltage class, and the field ceiling becomes a true amp rating
//    (Max Field Amps) rather than a duty or volts proxy.
// 2. Floor in amperes. The minimum-field floor is defined by a field current,
//    not a duty; with measurement it is stored and applied in amps, and the
//    copper proxy correction is retired. The onset law knee = a + C/RPM keeps
//    its form with C in amps.
// 3. Alternator health. The expected-output model becomes (field current,
//    speed, temperature) instead of (duty, speed), so the winding's
//    resistance drift no longer has to be tolerated as model slack, and the
//    trend detection sharpens by that margin.
// 4. Torque and engine load. Mechanical input power is electrical output
//    divided by the machine's efficiency at (field current, speed, output)
//    plus field power; torque is that over shaft speed. Report it to the
//    engine bus (NMEA 2000 PGN 127489 load fields) and feed it to the
//    charge-rate limits and to the governor model of Part 3 D.
// 5. Machine identification. Cold R, L, and the field-current-to-output
//    curve fingerprint the alternator frame; seed defaults and detect a
//    swapped alternator.
//
// ---------------------------------------------------------------------------
// E. FAULT DETECTION
// ---------------------------------------------------------------------------
//   - Open field, lost brush contact, open field fuse: zero current at
//     non-zero duty, detected in one PWM period instead of the seconds the
//     output-based test of Part 1 J needs.
//   - Shorted turns: R falls below what temperature can explain AND L falls
//     faster than R (L scales with the square of turns, R linearly), so the
//     L/R ratio separates a shorted rotor from a merely hot or cold one.
//   - Brush wear and slip-ring contamination: the cold-start R_ref drifts
//     upward over months; intermittent contact at speed shows as
//     sub-millisecond current dropouts that the shunt sees and the output
//     does not. Count them.
//   - Current with the field commanded off (shorted switch, miswired jumper):
//     alarm and refuse to run. Today invisible in N-type wiring, where the
//     input-protection stage never sees field current.
//   - Rotor insulation to ground is NOT visible from current alone; recorded
//     as the limit of the method.
//
// ---------------------------------------------------------------------------
// PRIOR ART ACKNOWLEDGED
// ---------------------------------------------------------------------------
// Automotive regulator ICs measure field current; resistance-based winding
// temperature estimation is standard in motor drives and has been proposed
// for automotive alternators. What is recorded here is the single low-side
// shunt serving both wiring types by interval-keyed sampling with sign-based
// type detection, the cycle-average R identity, the cold-start
// self-referencing, the L/R shorted-turn discriminator, brush wear from
// reference drift and dropout counting, the ampere-domain inner loop and
// floor with the commissioned-gain argument, and the health, torque and
// engine-bus uses in an aftermarket regulator.
//
// ---------------------------------------------------------------------------
// SCOPE OF THIS DISCLOSURE
// ---------------------------------------------------------------------------
// Intended to cover any field-current sensing element in any position on a
// single-terminal drive, any sampling keyed to the switching waveform, any
// derivation of resistance, inductance or temperature from those samples,
// any control loop, floor, ceiling, health model, torque estimate or fault
// test that consumes them, and any combination with Parts 1 to 3.
// ============================================================================


// ============================================================================
// IDEAS RECORDED TO PROTECT FUTURE IMPLEMENTATIONS, PART 5:
// ALTERNATOR TEMPERATURE AND CONDITION FROM THE ELECTRICAL BEHAVIOR OF THE
// FIELD COIL WHILE THE MACHINE IS SPINNING
// ============================================================================
// Added 2026-09-15. The commit date of this block in the public repository
// https://github.com/XEngineeringLLC/XREG-010 is its publication date. Not
// built. Same rule as Parts 1 to 4: written to be enabling, with the numbers
// that decide whether it works stated.
//
// Part 4 recorded how to MEASURE field current on a single-terminal drive and
// listed winding temperature as one of its uses. This part is the inference
// itself, independent of how the electrical quantity is obtained: the field
// coil of a turning alternator is treated as an instrument, read continuously
// by the regulator that is already driving it, and the drift of its electrical
// behavior against a baseline is read out as BOTH the temperature of the
// machine AND its state of wear. The variants in D need no new sensor at all.
//
// ---------------------------------------------------------------------------
// WHAT EXISTS TODAY
// ---------------------------------------------------------------------------
// Alternator temperature is a single DS18B20 probe (AlternatorTemperatureF)
// zip-tied or bolted to the case, and everything thermal hangs off it: the
// temperature derate, the maximum-temperature watermarks, and the copper
// correction on the minimum-field floor. The case is not the hot part. The
// rotor winding is, it is buried inside the machine behind an air gap, and the
// probe lags it by minutes and under-reads it by tens of degrees. The gap is
// worst exactly where it matters: high output at low speed in a hot engine
// room, where the fan is moving the least air. Field current is not measured;
// it is duty times bus voltage divided by FieldResistance, a typed setting.
//
// ---------------------------------------------------------------------------
// THE GENERAL METHOD
// ---------------------------------------------------------------------------
// While the alternator is turning and under normal regulation, observe any
// electrical characteristic of the field circuit -- current at a known applied
// field voltage, its rise or fall slope, its ripple, its noise, its time
// constants -- and compare it against a baseline captured for that same
// machine. The deviation is decomposed into:
//
//   (a) a REVERSIBLE part that follows copper's temperature coefficient and a
//       thermal model of the machine, which is the winding temperature; and
//   (b) an IRREVERSIBLE or unmodeled residual, which is the condition of the
//       machine: brush and slip-ring wear, shorted turns, a degrading cooling
//       path, loose or corroded field wiring.
//
// The measurement is taken WITH THE ROTOR SPINNING, which is what separates
// this from standstill winding-resistance measurement. It costs no downtime,
// it runs every PWM period of every charge session, and several of the wear
// signals in F exist only while the machine is turning.
//
// ---------------------------------------------------------------------------
// A. THE PRIMARY OBSERVABLE: FIELD CURRENT AGAINST A KNOWN FIELD VOLTAGE
// ---------------------------------------------------------------------------
// Over one whole PWM period the average of L di/dt is zero, so
//
//     R_field = duty x (Vbus - Vswitch) / I_field_avg
//
// exactly, with no knowledge of inductance. One resistance number per PWM
// period, from a field current obtained by any of the sensing topologies in
// Part 4 A (low-side shunt, mid-node amplifier, Hall sensor, the switch's own
// on-resistance, or a driver IC's current-sense pin).
//
// The signal is large. A field of 2 to 6 ohm cold, taken from 20 to 120 degC
// rotor winding temperature, rises about 39 % in resistance, so field current
// at a fixed duty and a fixed bus voltage falls about 28 %. This is not a
// microvolt measurement. A 1 % error in R is 2.5 degC, so even a coarse
// current measurement -- 2 to 3 % -- resolves the winding to better than
// 10 degC, which is already far better than the case probe's error against
// the winding.
//
// Averaging on a spinning machine, which is the part standstill methods do
// not have to deal with:
//   - Average over WHOLE PWM periods, keyed to the timer that generates the
//     PWM, so the switching ripple cancels rather than aliases.
//   - Average over several shaft revolutions as well. Armature reaction and
//     slot-harmonic coupling superimpose an AC component on field current at
//     shaft-related frequencies (Part 1 B); a revolution-synchronous average
//     removes it from the DC estimate, and the residue it leaves is itself a
//     useful signal (F.4).
//   - Reject the sample when the operating point is moving: hold the estimate
//     across load steps, target changes, protection events, and the first
//     few hundred ms after any duty step, because the cycle-average identity
//     assumes steady state within the period.
//   - Bus voltage must be sampled coherently with the current, since it is a
//     multiplicative term. A stale or differently-filtered Vbus is a direct
//     percentage error in R, hence a direct degC error.
//
// ---------------------------------------------------------------------------
// B. THE BASELINE, AND WHAT MAKES IT VALID
// ---------------------------------------------------------------------------
// The method needs one moment when the winding temperature is KNOWN, to pin
// the pair (R_ref, T_ref). The winding is known to be at the case probe's
// temperature whenever the machine has been off long enough to be isothermal:
//
//   - At commissioning, on a cold machine.
//   - At any engine start where the regulator's own history shows the engine
//     has been off long enough for the case probe to have settled, and the
//     case probe agrees with any other ambient reference the system has
//     (battery temperature probe, board temperature). Several hours is safe;
//     the settling test is that the case probe's own rate of change has been
//     below a small threshold for a long interval, not a fixed clock.
//   - On a stopped rotor by deliberate excitation: apply a brief bounded
//     field current with the engine off (the probe of Part 1) and read R with
//     no rotation at all. This gives a clean reference uncontaminated by
//     rotation-dependent effects, and it can be repeated on a schedule.
//
// Keep a rolling history of these cold references with their temperatures and
// their dates. The single most recent one is the temperature reference; the
// TREND across them over months is the wear signal of F.1. Store them in the
// commissioning ledger so they survive a settings reset, and store enough of
// them to fit a slope.
//
// A second, richer baseline is an OPERATING baseline: the expected resistance
// as a function of the operating point (field current, shaft speed, output
// current, ambient), learned over the first healthy weeks of a machine's life
// and stored per machine. Deviation from the operating baseline, rather than
// from the cold reference, is what makes the condition monitoring of F work
// without demanding that the boat ever sit long enough for a cold reference.
//
// ---------------------------------------------------------------------------
// C. FROM RESISTANCE TO TEMPERATURE
// ---------------------------------------------------------------------------
//     T_winding = T_ref + (R / R_ref - 1) / alpha
//
// with alpha = 0.00393 per degC for copper (0.00218 per degF, the coefficient
// already used for the minimum-field floor correction). Aluminum-wound rotors
// take 0.00403; the coefficient is a per-machine constant that can be typed
// or identified during a known heating transient.
//
// What this buys over the case probe:
//   - The limit can be set where the physics is. Insulation class, not case
//     metal, is what fails: 155 degC for class F, 180 degC for class H, at
//     the winding. A case-probe limit is a guess at that number with an
//     unknown offset, so it is set conservatively and costs output all the
//     time, or set optimistically and fails to protect during the one event
//     that matters.
//   - It sees fast heating. Rotor thermal time constants are on the order of
//     minutes; the path from winding to case to probe adds minutes more. A
//     hard pull from cold can put real heat in the winding before the case
//     probe has moved at all.
//   - It still works when the probe is missing, unplugged, or was never
//     installed, which is a real install condition, not a hypothetical.
//   - The case probe is not discarded: it becomes the ambient and cross-check
//     channel, and disagreement between the two beyond what the thermal model
//     allows is itself a fault (probe fallen off, or E).
//
// ---------------------------------------------------------------------------
// D. VARIANTS THAT NEED NO FIELD-CURRENT SENSOR
// ---------------------------------------------------------------------------
// Everything above assumes field current is measured. It does not have to be.
// Field resistance is observable, more coarsely, from quantities this system
// already has, and these variants are recorded so that the disclosure is not
// limited to hardware that adds a sensing element.
//
// 1. MINIMUM-FIELD FLOOR INVERSION. The per-speed minimum-duty floor is
//    defined as the most field that still yields about zero output, which is
//    a FIELD CURRENT threshold expressed in duty. Its position in duty is
//    therefore proportional to R. Today the floor is corrected downward for a
//    hot machine using the case probe as a proxy. Invert it: learn the floor
//    by observation (the existing knee-learning machinery), and read the
//    winding temperature out of where the learned floor sits relative to the
//    cold-reference floor. Same hardware, no new part, and the two uses can
//    coexist because a learned floor and an assumed floor are independently
//    available.
// 2. DUTY-DOMAIN PROXY IN CLOSED LOOP. At a steady operating point -- speed,
//    bus voltage, and output current all held -- the duty the regulator must
//    command to hold that output is a function of the field ampere-turns it
//    has to produce, hence of R. Drift of commanded duty at an otherwise
//    unchanged operating point is a temperature proxy. Confounded by stator
//    heating (stator resistance also rises and cuts output at fixed field)
//    and by saturation; usable as a corroborator or as the only channel on a
//    board with no field sensing, not as the primary.
// 3. TIME CONSTANT FROM THE EXISTING SWITCHING WAVEFORM. Any observable that
//    carries the field's L/R time constant yields R once L is known, and L is
//    nearly temperature-independent. The decay tail after a field-off event
//    is already characterized by this firmware (the field-decay time constant
//    work); its tau is L/R, so tau falling over a heating session is a
//    direct, sensorless temperature reading. The same applies to the rise
//    slope after a duty step observed on ANY channel that responds to field
//    current, including alternator output current.
// 4. STANDSTILL PROBE. The bounded excitation probe of Part 1, applied with
//    the engine stopped, gives settled current and slope, hence R and L, on
//    any topology that can observe either. Used for the cold reference of B
//    and for a post-shutdown cool-down curve (G.2).
//
// ---------------------------------------------------------------------------
// E. SEPARATING TEMPERATURE FROM CONDITION
// ---------------------------------------------------------------------------
// The measured resistance is not only copper:
//
//     R_measured = R_copper(T) + R_contact + R_wiring
//
// where R_contact is the two brush-to-slip-ring interfaces (tens of
// milliohms, a fraction of a percent of a 2 to 6 ohm field) and R_wiring is
// the field lead, its terminations, and the switch drop if not separately
// accounted. Both are nearly temperature-independent compared with copper.
// That difference is the separator:
//
//   - During a known heating or cooling TRANSIENT, fit R_measured against the
//     thermal model. The part that swings with temperature is copper; the
//     intercept that does not swing is (R_contact + R_wiring). The fit needs
//     no absolute reference, only a transient with enough span.
//   - Track that intercept over months. It is the wear channel of F.1, and
//     removing it from R before applying the temperature law removes the slow
//     bias that brush wear would otherwise inject into every temperature
//     reading. Without this separation, a worn machine reads permanently hot.
//   - A step change in the intercept between one session and the next is not
//     wear. It is a connection: a field terminal backed off, a crimp gone
//     high-resistance, a corroded lug. Alarm on the step, trend the slope.
//
// ---------------------------------------------------------------------------
// F. CONDITION AND WEAR DIAGNOSTICS
// ---------------------------------------------------------------------------
// 1. BRUSH AND SLIP-RING WEAR. Brushes wear, slip rings glaze, oxidize, and
//    groove. All three raise the contact term of E, slowly and permanently.
//    The cold reference R_ref rises across months while the temperature-
//    dependent fraction of R stays constant in absolute terms, so the ratio
//    (copper share)/(total) falls. Trend it, project the crossing of a wear
//    threshold, and report remaining service life as a date, not a flag. This
//    is the "the alternator is wearing out" channel, and it is the one that
//    gives useful warning before a failure rather than after it.
// 2. SHORTED TURNS IN THE ROTOR. Resistance falls below what any plausible
//    temperature explains. Confirm against inductance: L scales with the
//    square of turns and R linearly, so a shorted-turn fault drops L faster
//    than R, and the L/R ratio separates a shorted rotor from a merely cold
//    one. Corroborate on output: a rotor with shorted turns makes less output
//    per ampere of field, which the health model already tracks.
// 3. OPEN FIELD, LOST BRUSH CONTACT, OPEN FUSE. Zero current at non-zero
//    duty, in one PWM period.
// 4. INTERMITTENT CONTACT AND MECHANICAL RUNOUT, VISIBLE ONLY WHILE SPINNING.
//    A worn brush in a worn holder bounces; a slip ring worn oval or a rotor
//    running out modulates contact resistance once per revolution. Both show
//    in field current as structure at shaft frequency and its harmonics, and
//    as sub-millisecond dropouts, neither of which exists at standstill and
//    neither of which reaches the alternator's output in a form the output
//    channels can see. Count the dropouts per hour and track the shaft-rate
//    modulation index. Rising modulation at a constant average resistance is
//    mechanical wear, and it leads the resistance trend of F.1.
// 5. DEGRADED COOLING PATH. See G.
// 6. THE WINDING TEMPERATURE ITSELF AS A HEALTH INDEX. An alternator that
//    must run hotter than its own history to make the same amps at the same
//    speed and the same ambient is degrading, whatever the mechanism. Feed
//    the winding temperature into the existing expected-output health model
//    as a response variable, not only as a correction term.
// 7. CONSUMED INSULATION LIFE. Insulation life halves for roughly every
//    10 degC of winding temperature. Integrate the actual measured winding
//    temperature over the machine's whole service life, weighted by that law,
//    and keep a running consumed-life fraction in non-volatile storage. This
//    converts the temperature measurement into a maintenance number for the
//    owner and turns every derate decision into a life-budget decision. No
//    case probe can support this, because it never saw the real temperature.
//
// ---------------------------------------------------------------------------
// G. THE THERMAL MODEL, AND COOLING-PATH DIAGNOSIS
// ---------------------------------------------------------------------------
// Once the winding temperature is observable, the machine's own thermal
// behavior becomes identifiable, and its drift is another condition channel.
//
// 1. RISE PER WATT. Field dissipation is I_field squared times R, which is
//    now measured directly, and total machine loss follows from output power
//    and the efficiency model. At a steady operating point the winding's
//    temperature rise above ambient divided by the dissipated power is a
//    thermal resistance, and it is a strong function of airflow, hence of
//    shaft speed. Learn the surface (rise per watt) against (RPM, ambient)
//    during healthy operation. A machine whose surface shifts upward over
//    time is losing its cooling path: a fan blade lost or clogged, a case
//    caked in oil mist and dust, a blocked engine-room vent, an air gap
//    fouled, a bearing dragging. None of that is visible to a case probe,
//    which shifts in the same direction and hides the change.
// 2. TIME CONSTANT. Excite a step in field dissipation and watch the
//    exponential approach; tau is thermal mass over heat transfer. Thermal
//    mass does not change, so tau is a second, independent read on the same
//    cooling path. After shutdown the cool-down curve is available for free,
//    sampled by brief standstill probes (D.4), and it isolates the natural-
//    convection path from the fan-driven one.
// 3. PREDICTION. With a fitted model the regulator can forecast the winding
//    temperature a few minutes ahead from the present operating point, and
//    derate BEFORE the limit rather than after it. A predictive derate is
//    smooth; a reactive one is a cliff the operator notices.
//
// ---------------------------------------------------------------------------
// H. ERROR BUDGET AND ACKNOWLEDGED LIMITS
// ---------------------------------------------------------------------------
//   - Bus voltage and duty enter R multiplicatively; their errors are
//     percentage errors in R and therefore 2.5 degC per percent.
//   - The switch drop must be modeled or measured, and it is itself
//     temperature dependent; a fixed assumed drop injects a load-dependent
//     bias into R.
//   - Contact and lead resistance sit inside R and must be separated by E, or
//     a worn machine reads hot forever.
//   - The method reads the ROTOR. A shorted stator, a failed rectifier diode,
//     or a stator hot spot is invisible here and needs the ripple and output
//     channels. Rotor insulation resistance to ground is likewise not visible
//     from field current at all.
//   - The temperature reference decays in validity: a boat used daily may not
//     offer a true cold start for weeks, and the operating baseline of B is
//     what carries the method through that.
//   - A belt that is slipping or thrown changes the thermal picture without
//     changing the electrical one, which is why the cooling diagnosis of G
//     must be gated on a confirmed speed.
//
// ---------------------------------------------------------------------------
// PRIOR ART ACKNOWLEDGED
// ---------------------------------------------------------------------------
// Winding-resistance thermometry is old and standard: resistance-rise tests
// on machines, DC injection for stator temperature in variable-speed motor
// drives, and proposals for alternator rotor temperature estimation in
// automotive regulator ICs that already measure field current. Automotive
// regulators also flag open and shorted fields.
//
// What is recorded here is the combination in an aftermarket marine
// regulator, continuously on a SPINNING machine driven by the regulator's own
// PWM with no interruption of charging: the cold-reference and operating-
// baseline scheme with reference trending, the thermal-transient intercept
// that separates temperature-independent contact resistance from copper so
// that brush wear does not masquerade as heat and heat does not masquerade as
// wear, the shaft-rate contact-modulation index and dropout counting as a
// mechanical wear channel that exists only under rotation, the sensorless
// variants that read temperature out of the learned minimum-field floor, the
// commanded duty, or the field-decay time constant, the rise-per-watt cooling
// surface as a cooling-path diagnostic, the predictive derate, and the
// integration of measured winding temperature into a consumed-insulation-life
// number reported to the owner.
//
// ---------------------------------------------------------------------------
// SCOPE OF THIS DISCLOSURE
// ---------------------------------------------------------------------------
// Intended to cover the determination of the temperature, the condition, the
// remaining life, or the cooling state of an alternator or of any part of it,
// in a regulator, from any electrical characteristic of the field circuit --
// resistance, current, inductance, time constant, ripple, noise, modulation,
// or any quantity derived from them -- measured or inferred while the machine
// is turning, or at standstill for reference purposes, against any baseline
// however obtained; any decomposition of that measurement into temperature
// and condition parts; any sensorless proxy for it including duty, a learned
// minimum-field floor, a decay time constant, or an output-derived estimate;
// any thermal or wear model fitted to it; and any resulting derate, limit,
// protection action, alarm, service prediction, life accounting, health grade,
// or report to a display or a vessel data bus. Combinable with Parts 1 to 4.
// ============================================================================
