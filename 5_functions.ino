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

// Battery-temperature source chain helpers (defined in 6_functions.ino next to computeCvTempScale)
bool tempProbeFresh(uint8_t idx);
const char *battTempSrcName(uint8_t src);

void SystemTime(const tN2kMsg &N2kMsg) {
  unsigned char SID;
  uint16_t SystemDate;
  double SystemTime;
  tN2kTimeSource TimeSource;

  if (ParseN2kSystemTime(N2kMsg, SID, SystemDate, SystemTime, TimeSource)) {
    lastNmea2kSystemTimeMs = millis();  // freshness for priority chain (NMEA > Phone > NTP)
    syncTimeFromGPS(SystemDate, SystemTime);

    if (NMEA2KVerbose) {
      OutputStream->println("System time:");
      PrintLabelValWithConversionCheckUnDef("  SID: ", SID, 0, true);
      PrintLabelValWithConversionCheckUnDef("  days since 1.1.1970: ", SystemDate, 0, true);
      PrintLabelValWithConversionCheckUnDef("  seconds since midnight: ", SystemTime, 0, true);
      OutputStream->print("  time source: ");
      PrintN2kEnumType(TimeSource, OutputStream);
    }
  } else {
    if (NMEA2KVerbose) {
      OutputStream->print("Failed to parse PGN: ");
      OutputStream->println(N2kMsg.PGN);
    }
  }
}

void Rudder(const tN2kMsg &N2kMsg) {
  unsigned char Instance;
  tN2kRudderDirectionOrder RudderDirectionOrder;
  double RudderPosition;
  double AngleOrder;

  if (ParseN2kRudder(N2kMsg, RudderPosition, Instance, RudderDirectionOrder, AngleOrder)) {
    //Uncomment below to get serial montior back
    // PrintLabelValWithConversionCheckUnDef("Rudder: ", Instance, 0, true);
    // PrintLabelValWithConversionCheckUnDef("  position (deg): ", RudderPosition, &RadToDeg, true);
    // OutputStream->print("  direction order: ");
    // PrintN2kEnumType(RudderDirectionOrder, OutputStream);
    // PrintLabelValWithConversionCheckUnDef("  angle order (deg): ", AngleOrder, &RadToDeg, true);
  } else {
    OutputStream->print("Failed to parse PGN: ");
    OutputStream->println(N2kMsg.PGN);
  }
}
//*****************************************************************************
void Heading(const tN2kMsg &N2kMsg) {
  static unsigned long lastHeadingUpdate = 0;
  if (millis() - lastHeadingUpdate < 2000) return;
  lastHeadingUpdate = millis();
  unsigned char SID;
  tN2kHeadingReference HeadingReference;
  double Heading;
  double Deviation;
  double Variation;

  if (ParseN2kHeading(N2kMsg, SID, Heading, Deviation, Variation, HeadingReference)) {
    if (N2kIsNA(Heading)) return;        // F-RES-04: skip NA field, otherwise -1e9 leaks into HeadingNMEA
    HeadingNMEA = Heading * 180.0 / PI;
    HeadingRefNMEA = (HeadingReference == N2khr_true) ? 0
                   : (HeadingReference == N2khr_magnetic) ? 1 : -1;
    HeadingVariationDeg = N2kIsNA(Variation) ? NAN : (float)(Variation * 180.0 / PI);
    MARK_FRESH(IDX_HEADING_NMEA);

    // Uncomment below to get serial monitor output back
    // OutputStream->println("Heading:");
    // PrintLabelValWithConversionCheckUnDef("  SID: ", SID, 0, true);
    // OutputStream->print("  reference: ");
    // PrintN2kEnumType(HeadingReference, OutputStream);
    // PrintLabelValWithConversionCheckUnDef("  Heading (deg): ", Heading, &RadToDeg, true);
    // PrintLabelValWithConversionCheckUnDef("  Deviation (deg): ", Deviation, &RadToDeg, true);
    // PrintLabelValWithConversionCheckUnDef("  Variation (deg): ", Variation, &RadToDeg, true);
  } else {
    // Parsing failed - do NOT call MARK_FRESH, data will go stale automatically
    OutputStream->print("Failed to parse PGN: ");
    OutputStream->println(N2kMsg.PGN);
  }
}
//*****************************************************************************
// Best 60-second average speed over ground — the number behind the Sustained Speed
// record and the fleet speed board. SOG arrives at ~0.5 Hz (COGSOG throttles itself to
// 2 s), so the window is integrated time-weighted from timestamped samples instead of
// binned; the metric stays correct if that cadence ever changes. Samples are stored at
// most 1/s so the ring always spans more than a minute whatever the source rate. A score
// needs a fully covered 60 s with no gap past SUST_MAX_GAP_MS — a NMEA dropout must not
// be able to manufacture a record out of two samples an hour apart.
void updateSustainedSpeed(float sogKn) {
  static uint8_t head = 0, count = 0;
  if (!sustT || !sustV) return;   // PSRAM ring (allocated in setup)
  uint32_t *t = sustT;
  float    *v = sustV;

  const uint32_t now = millis();
  const uint8_t newest = (uint8_t)((head + SUST_RING_SIZE - 1) % SUST_RING_SIZE);

  if (count) {
    if (now < t[newest]) count = 0;                          // millis() rollover
    else if (now - t[newest] < SUST_STORE_MIN_MS) return;    // decimate to 1 Hz
    else if (now - t[newest] > SUST_MAX_GAP_MS) count = 0;   // dropout — window starts over
  }

  t[head] = now;
  v[head] = sogKn;
  head = (uint8_t)((head + 1) % SUST_RING_SIZE);
  if (count < SUST_RING_SIZE) count++;

  sogSust1m = 0.0f;
  if (count < 2) return;

  // Newest → oldest, step-hold: each sample's value held until the next one arrived.
  double area = 0.0;   // kn·ms
  uint32_t span = 0;
  bool covered = false;
  for (uint8_t k = 1; k < count; k++) {
    const uint8_t newerIdx = (uint8_t)((head + SUST_RING_SIZE - k) % SUST_RING_SIZE);
    const uint8_t olderIdx = (uint8_t)((head + SUST_RING_SIZE - k - 1) % SUST_RING_SIZE);
    uint32_t dt = t[newerIdx] - t[olderIdx];
    if (dt == 0) continue;
    if (dt > SUST_MAX_GAP_MS) break;
    if (span + dt >= SUST_WINDOW_MS) { dt = SUST_WINDOW_MS - span; covered = true; }
    area += (double)v[olderIdx] * dt;
    span += dt;
    if (covered) break;
  }
  if (!covered || span == 0) return;

  sogSust1m = (float)(area / span);
  if (sogSust1m > MaxSpeed) MaxSpeed = sogSust1m;
  if (sogSust1m > MaxSpeed_AllTime) MaxSpeed_AllTime = sogSust1m;
}

//*****************************************************************************
void COGSOG(const tN2kMsg &N2kMsg) {
  static unsigned long lastCOGSOGUpdate = 0;
  if (millis() - lastCOGSOGUpdate < 2000) return;
  lastCOGSOGUpdate = millis();

  unsigned char SID;
  tN2kHeadingReference HeadingReference;
  double COG;
  double SOG;

  if (ParseN2kCOGSOGRapid(N2kMsg, SID, HeadingReference, COG, SOG)) {
    // F-RES-04: bail entirely if both fields NA; otherwise update each independently.
    if (N2kIsNA(COG) && N2kIsNA(SOG)) return;
    // When the user has selected phone GPS as the speed/course source, NMEA writes are
    // suppressed entirely (freshness stamps still land, for /debug diagnostics) so the
    // record chain and leaderboard flag never mix sources.
    if (!N2kIsNA(COG)) {
      lastNmea2kCogMs = millis();
      if (speedSourceMode != SPD_SRC_PHONE) {
        COGNMEA = COG * 180.0 / PI;
        MARK_FRESH(IDX_COG_NMEA);
      }
    }
    if (!N2kIsNA(SOG)) {
      lastNmea2kSogMs = millis();
      if (speedSourceMode != SPD_SRC_PHONE) {
        SOGNMEA = SOG * 1.94384;     // m/s → knots
        MARK_FRESH(IDX_SOG_NMEA);

        updateSustainedSpeed(SOGNMEA);
        wmIgnUpdate(wmIgn_SOG, SOGNMEA);  // ignition-cycle watermark
      }
    }

  } else {
    OutputStream->print("Failed to parse PGN: ");
    OutputStream->println(N2kMsg.PGN);
  }
}

//*****************************************************************************

void GNSS(const tN2kMsg &N2kMsg) {
  //Serial.println("=== GNSS function called ===");
  //Serial.printf("PGN: %lu\n", N2kMsg.PGN);
  static unsigned long lastGNSSUpdate = 0;
  if (millis() - lastGNSSUpdate < 2000) return;
  lastGNSSUpdate = millis();

  unsigned char SID;
  uint16_t DaysSince1970;
  double SecondsSinceMidnight;
  double Latitude;
  double Longitude;
  double Altitude;
  tN2kGNSStype GNSStype;
  tN2kGNSSmethod GNSSmethod;
  unsigned char nSatellites;
  double HDOP;
  double PDOP;
  double GeoidalSeparation;
  unsigned char nReferenceStations;
  tN2kGNSStype ReferenceStationType;
  uint16_t ReferenceStationID;
  double AgeOfCorrection;

  if (ParseN2kGNSS(N2kMsg, SID, DaysSince1970, SecondsSinceMidnight,
                   Latitude, Longitude, Altitude,
                   GNSStype, GNSSmethod,
                   nSatellites, HDOP, PDOP, GeoidalSeparation,
                   nReferenceStations, ReferenceStationType, ReferenceStationID,
                   AgeOfCorrection)) {

    //Serial.println("GNSS parsing SUCCESS!");
    //Serial.printf("Raw values - Lat: %f, Lon: %f, Sats: %d\n", Latitude, Longitude, nSatellites);

    if (!isnan(Latitude) && !isnan(Longitude) && Latitude != 0.0 && Longitude != 0.0 && abs(Latitude) <= 90.0 && abs(Longitude) <= 180.0 && nSatellites > 0) {

      LatitudeNMEA = Latitude;
      LongitudeNMEA = Longitude;
      SatelliteCountNMEA = nSatellites;
      lastNmea2kGnssMs = millis();  // freshness for GPS priority chain (NMEA > Phone)
      currentGpsSource = GPS_NMEA;  // NMEA wins when present; consumePhoneGps() flips this when NMEA goes stale

      MARK_FRESH(IDX_LATITUDE_NMEA);
      MARK_FRESH(IDX_LONGITUDE_NMEA);
      MARK_FRESH(IDX_SATELLITE_COUNT);

      //Serial.printf("Valid GNSS data stored - LatNMEA: %f, LonNMEA: %f, SatNMEA: %d\n", LatitudeNMEA, LongitudeNMEA, SatelliteCountNMEA);
    } else {
      //Serial.println("GNSS data invalid - values are NaN, zero, or out of range");
      // Invalid data - do NOT call MARK_FRESH, let data go stale
    }
  } else {
    //Serial.println("GNSS parsing FAILED!");
    // Parse failed - do NOT call MARK_FRESH, let data go stale
  }
}
//*****************************************************************************
void GNSSSatsInView(const tN2kMsg &N2kMsg) {
  unsigned char SID;
  tN2kRangeResidualMode Mode;
  uint8_t NumberOfSVs;
  tSatelliteInfo SatelliteInfo;

  if (ParseN2kPGNSatellitesInView(N2kMsg, SID, Mode, NumberOfSVs)) {

    if (NMEA2KVerbose) {
      OutputStream->println("Satellites in view: ");
      OutputStream->print("  mode: ");
      OutputStream->println(Mode);
      OutputStream->print("  number of satellites: ");
      OutputStream->println(NumberOfSVs);
      for (uint8_t i = 0; i < NumberOfSVs && ParseN2kPGNSatellitesInView(N2kMsg, i, SatelliteInfo); i++) {
        OutputStream->print("  Satellite PRN: ");
        OutputStream->println(SatelliteInfo.PRN);
        PrintLabelValWithConversionCheckUnDef("    elevation: ", SatelliteInfo.Elevation, &RadToDeg, true, 1);
        PrintLabelValWithConversionCheckUnDef("    azimuth:   ", SatelliteInfo.Azimuth, &RadToDeg, true, 1);
        PrintLabelValWithConversionCheckUnDef("    SNR:       ", SatelliteInfo.SNR, 0, true, 1);
        PrintLabelValWithConversionCheckUnDef("    residuals: ", SatelliteInfo.RangeResiduals, 0, true, 1);
        OutputStream->print("    status: ");
        OutputStream->println(SatelliteInfo.UsageStatus);
      }
    }
  }
}
//*****************************************************************************
void BatteryConfigurationStatus(const tN2kMsg &N2kMsg) {
  unsigned char BatInstance;
  tN2kBatType BatType;
  tN2kBatEqSupport SupportsEqual;
  tN2kBatNomVolt BatNominalVoltage;
  tN2kBatChem BatChemistry;
  double BatCapacity;
  int8_t BatTemperatureCoefficient;
  double PeukertExponent;
  int8_t ChargeEfficiencyFactor;

  if (NMEA2KVerbose != 1) return;  // display-only handler; unconditional prints firehosed the console at bus rate
  if (ParseN2kBatConf(N2kMsg, BatInstance, BatType, SupportsEqual, BatNominalVoltage, BatChemistry, BatCapacity, BatTemperatureCoefficient, PeukertExponent, ChargeEfficiencyFactor)) {
    PrintLabelValWithConversionCheckUnDef("Battery instance: ", BatInstance, 0, true);
    OutputStream->print("  - type: ");
    PrintN2kEnumType(BatType, OutputStream);
    OutputStream->print("  - support equal.: ");
    PrintN2kEnumType(SupportsEqual, OutputStream);
    OutputStream->print("  - nominal voltage: ");
    PrintN2kEnumType(BatNominalVoltage, OutputStream);
    OutputStream->print("  - chemistry: ");
    PrintN2kEnumType(BatChemistry, OutputStream);
    PrintLabelValWithConversionCheckUnDef("  - capacity (Ah): ", BatCapacity, &CoulombToAh, true);
    PrintLabelValWithConversionCheckUnDef("  - temperature coefficient (%): ", BatTemperatureCoefficient, 0, true);
    PrintLabelValWithConversionCheckUnDef("  - peukert exponent: ", PeukertExponent, 0, true);
    PrintLabelValWithConversionCheckUnDef("  - charge efficiency factor (%): ", ChargeEfficiencyFactor, 0, true);
  } else {
    OutputStream->print("Failed to parse PGN: ");
    OutputStream->println(N2kMsg.PGN);
  }
}
//*****************************************************************************
void DCStatus(const tN2kMsg &N2kMsg) {
  unsigned char SID;
  unsigned char DCInstance;
  tN2kDCType DCType;
  unsigned char StateOfCharge;
  unsigned char StateOfHealth;
  double TimeRemaining;
  double RippleVoltage;
  double Capacity;

  if (!ParseN2kDCStatus(N2kMsg, SID, DCInstance, DCType, StateOfCharge, StateOfHealth, TimeRemaining, RippleVoltage, Capacity)) {
    if (NMEA2KVerbose == 1) {
      OutputStream->print("Failed to parse PGN: ");
      OutputStream->println(N2kMsg.PGN);
    }
    return;
  }
  // Ingest SOC/SOH from the selected battery bank (display/telemetry only, never control)
  if (DCInstance == n2kRxBattInstance && DCType == N2kDCt_Battery) {
    n2kRxSoc = (StateOfCharge > 100) ? -1 : (int)StateOfCharge;  // >100 = N2K not-available encoding
    n2kRxSoh = (StateOfHealth > 100) ? -1 : (int)StateOfHealth;
    if (n2kRxSoc >= 0 || n2kRxSoh >= 0) MARK_FRESH(IDX_N2K_SOC);
  }
  if (NMEA2KVerbose == 1) {  // print firehose gated like SystemTime
    OutputStream->print("DC instance: ");
    OutputStream->println(DCInstance);
    OutputStream->print("  - type: ");
    PrintN2kEnumType(DCType, OutputStream);
    OutputStream->print("  - state of charge (%): ");
    OutputStream->println(StateOfCharge);
    OutputStream->print("  - state of health (%): ");
    OutputStream->println(StateOfHealth);
    OutputStream->print("  - time remaining (h): ");
    OutputStream->println(TimeRemaining / 60);
    OutputStream->print("  - ripple voltage: ");
    OutputStream->println(RippleVoltage);
    OutputStream->print("  - capacity: ");
    OutputStream->println(Capacity);
  }
}
//*****************************************************************************
void BatteryStatus(const tN2kMsg &N2kMsg) {
  unsigned char SID;
  unsigned char BatteryInstance;
  double BatteryVoltage;
  double BatteryCurrent;
  double BatteryTemperature;

  if (!ParseN2kDCBatStatus(N2kMsg, BatteryInstance, BatteryVoltage, BatteryCurrent, BatteryTemperature, SID)) {
    if (NMEA2KVerbose == 1) {
      OutputStream->print("Failed to parse PGN: ");
      OutputStream->println(N2kMsg.PGN);
    }
    return;
  }
  // Ingest V/A/T from the selected battery bank — a shunt or BMS "virtual shunt" bridged
  // onto N2K. V/A are display/telemetry only; T is a batteryTempF() candidate (source 2) for the
  // charge lockouts and the CV derate. Missing fields stay NAN, never fabricated.
  if (BatteryInstance == n2kRxBattInstance) {
    if (N2kIsNA(BatteryVoltage) && N2kIsNA(BatteryCurrent) && N2kIsNA(BatteryTemperature)) return;
    n2kRxBattV = N2kIsNA(BatteryVoltage) ? NAN : (float)BatteryVoltage;
    n2kRxBattA = N2kIsNA(BatteryCurrent) ? NAN : (float)BatteryCurrent;
    n2kRxBattTempF = N2kIsNA(BatteryTemperature) ? NAN : (float)KelvinToF(BatteryTemperature);
    MARK_FRESH(IDX_N2K_BATT);
  }
  if (NMEA2KVerbose == 1) {
    OutputStream->print("Battery instance: ");
    OutputStream->println(BatteryInstance);
    PrintLabelValWithConversionCheckUnDef("  - voltage (V): ", BatteryVoltage, 0, true);
    PrintLabelValWithConversionCheckUnDef("  - current (A): ", BatteryCurrent, 0, true);
    PrintLabelValWithConversionCheckUnDef("  - temperature (C): ", BatteryTemperature, &KelvinToC, true);
  }
}
//*****************************************************************************
void Speed(const tN2kMsg &N2kMsg) {
  unsigned char SID;
  double SOW;
  double SOG;
  tN2kSpeedWaterReferenceType SWRT;

  if (ParseN2kBoatSpeed(N2kMsg, SID, SOW, SOG, SWRT)) {
    // Surface speed-through-water (SOW) for the boat-performance polar — removes current,
    // unlike SOG. SOG stays sourced from the COG/SOG rapid PGN (its canonical source).
    if (!N2kIsNA(SOW)) {
      STWNMEA = SOW * 1.94384;      // m/s → knots
      MARK_FRESH(IDX_STW_NMEA);
    }
    if (NMEA2KVerbose == 1) {      // prints only — the STW store above always runs
      OutputStream->print("Boat speed:");
      PrintLabelValWithConversionCheckUnDef(" SOW:", N2kIsNA(SOW) ? SOW : msToKnots(SOW));
      PrintLabelValWithConversionCheckUnDef(", SOG:", N2kIsNA(SOG) ? SOG : msToKnots(SOG));
      OutputStream->print(", ");
      PrintN2kEnumType(SWRT, OutputStream, true);
    }
  }
}
void WindSpeed(const tN2kMsg &N2kMsg) {
  static unsigned long lastWindUpdate = 0;
  if (millis() - lastWindUpdate < 2000) return;
  lastWindUpdate = millis();

  unsigned char SID;
  double WindSpeed;
  double WindAngle;
  tN2kWindReference WindReference;

  if (ParseN2kWindSpeed(N2kMsg, SID, WindSpeed, WindAngle, WindReference)) {
    // F-RES-04: bail entirely if both fields NA; otherwise update each independently.
    if (N2kIsNA(WindSpeed) && N2kIsNA(WindAngle)) return;

    // PGN 130306 carries either apparent OR true wind — branch on WindReference so
    // true-wind values never land in apparent globals (calculateDerivedMetrics would
    // run apparent→true math on them again). True_North/Magnetic are earth-frame TWD,
    // converted to boat-relative TWA via HeadingNMEA; True_boat/True_water are already boat-relative.
    bool isTrue = (WindReference == N2kWind_True_North ||
                   WindReference == N2kWind_Magnetic ||
                   WindReference == N2kWind_True_boat ||
                   WindReference == N2kWind_True_water);

    if (isTrue) {
      if (!N2kIsNA(WindSpeed)) {
        TrueWindSpeedNMEA = WindSpeed * 1.94384;
        MARK_FRESH(IDX_TRUE_WIND_SPEED);
      }
      if (!N2kIsNA(WindAngle)) {
        float angDeg = WindAngle * 180.0 / PI;
        while (angDeg < 0)       angDeg += 360.0;
        while (angDeg >= 360.0)  angDeg -= 360.0;
        if ((WindReference == N2kWind_True_North || WindReference == N2kWind_Magnetic) &&
            !IS_STALE(IDX_HEADING_NMEA)) {
          // Earth-frame TWD → boat-relative TWA. The subtraction is frame-valid only when wind and
          // heading share a reference (both true-north or both magnetic); on a mismatch shift the
          // heading into the wind's frame with the variation PGN 127250 carries (true = magnetic +
          // east-positive variation). Unknown reference or missing variation falls back to the raw
          // subtract — off by local variation, same as the pre-fix behavior.
          float headingForFrame = HeadingNMEA;
          if (!isnan(HeadingVariationDeg) && HeadingRefNMEA >= 0) {
            bool windIsTrueNorth = (WindReference == N2kWind_True_North);
            bool headingIsMagnetic = (HeadingRefNMEA == 1);
            if (windIsTrueNorth && headingIsMagnetic)        headingForFrame = HeadingNMEA + HeadingVariationDeg;
            else if (!windIsTrueNorth && !headingIsMagnetic) headingForFrame = HeadingNMEA - HeadingVariationDeg;
          }
          float twa = angDeg - headingForFrame;
          while (twa < 0)        twa += 360.0;
          while (twa >= 360.0)   twa -= 360.0;
          TrueWindAngleNMEA = twa;
        } else {
          // True_boat / True_water → already boat-relative
          // (or earth-frame without heading — fall through; better than nothing for Zambretti
          //  which adds heading back in JS)
          TrueWindAngleNMEA = angDeg;
        }
        MARK_FRESH(IDX_TRUE_WIND_ANGLE);
      }
      if (!N2kIsNA(WindSpeed)) UpdateWindMaximums();
    } else {
      // Apparent (N2kWind_Apparent or unknown reference)
      if (!N2kIsNA(WindSpeed)) {
        ApparentWindSpeedNMEA = WindSpeed * 1.94384;
        MARK_FRESH(IDX_APPARENT_WIND_SPEED);
      }
      if (!N2kIsNA(WindAngle)) {
        ApparentWindAngleNMEA = WindAngle * 180.0 / PI;
        MARK_FRESH(IDX_APPARENT_WIND_ANGLE);
      }
      if (!N2kIsNA(WindSpeed)) UpdateWindMaximums();  // also guards TrueWindSpeedNMEA internally
    }

  } else {
    OutputStream->print("Failed to parse PGN: ");
    OutputStream->println(N2kMsg.PGN);
  }
}
//*****************************************************************************
void WaterDepth(const tN2kMsg &N2kMsg) {
  unsigned char SID;
  double DepthBelowTransducer;
  double Offset;

  if (ParseN2kWaterDepth(N2kMsg, SID, DepthBelowTransducer, Offset)) {
    // Store effective depth (transducer + offset if available; raw transducer otherwise).
    // Offset > 0 = depth below waterline; Offset < 0 = depth below keel. Either way the sum is the
    // physical water depth at the transducer position.
    if (!N2kIsNA(DepthBelowTransducer)) {
      double effectiveDepth_m = DepthBelowTransducer + (N2kIsNA(Offset) ? 0.0 : Offset);
      if (effectiveDepth_m >= 0) {
        WaterDepth_m = (float)effectiveDepth_m;
        MARK_FRESH(IDX_WATER_DEPTH);
      }
    }
    if (N2kIsNA(Offset) || Offset == 0) {
      PrintLabelValWithConversionCheckUnDef("Depth below transducer", DepthBelowTransducer);
      if (N2kIsNA(Offset)) {
        OutputStream->println(", offset not available");
      } else {
        OutputStream->println(", offset=0");
      }
    } else {
      if (Offset > 0) {
        OutputStream->print("Water depth:");
      } else {
        OutputStream->print("Depth below keel:");
      }
      if (!N2kIsNA(DepthBelowTransducer)) {
        OutputStream->println(DepthBelowTransducer + Offset);
      } else {
        OutputStream->println(" not available");
      }
    }
  }
}
//*****************************************************************************
void Attitude(const tN2kMsg &N2kMsg) {
  unsigned char SID;
  double Yaw;
  double Pitch;
  double Roll;

  if (NMEA2KVerbose != 1) return;  // display-only handler; PGN 127257 arrives at 10 Hz from common compasses
  if (ParseN2kAttitude(N2kMsg, SID, Yaw, Pitch, Roll)) {
    OutputStream->println("Attitude:");
    PrintLabelValWithConversionCheckUnDef("  SID: ", SID, 0, true);
    PrintLabelValWithConversionCheckUnDef("  Yaw (deg): ", Yaw, &RadToDeg, true);
    PrintLabelValWithConversionCheckUnDef("  Pitch (deg): ", Pitch, &RadToDeg, true);
    PrintLabelValWithConversionCheckUnDef("  Roll (deg): ", Roll, &RadToDeg, true);
  } else {
    OutputStream->print("Failed to parse PGN: ");
    OutputStream->println(N2kMsg.PGN);
  }
}
void HandleNMEA2000Msg(const tN2kMsg &N2kMsg) {
  // Victron proprietary fast-packet (VREG carrier): dispatched here, ahead of the receive gate,
  // because the DVCC follow decode has its own master switch — and ONLY to its own decoder, so
  // "receive off" still means nothing reaches the verbose print or the handler-table scan.
  // Gate matches dvccRawFrameTap's (NMEA2KData || dvccEn) on purpose: the UI tells the user to
  // watch the decoded CVL/CCL and confirm them against the GX BEFORE enabling follow, which is
  // impossible if this carrier only decodes once follow is already on. Decode-only — the clamps
  // stay gated on dvccEn at their application sites and in dvccTick.
  if (N2kMsg.PGN == 126720UL) {
    if ((dvccEn == 1 || NMEA2KData == 1) && dvccSrcType == 0) VictronVreg126720(N2kMsg);
    return;
  }
  // Who is on the bus (spec §15): PGN 60928 NAME + 126996 product information. Same pre-gate
  // placement and gate as the VREG carrier above — a capture is uninterpretable without the
  // publisher's identity, and identity has to accumulate during the follow-off observe window.
  // Passive only: with transmit off neither PGN can be requested. These two fall through rather
  // than returning, so verbose logging and the handler-table scan behave exactly as before.
  if (N2kMsg.PGN == 60928UL && (dvccEn == 1 || NMEA2KData == 1)) {
    dvccNoteName((uint8_t)N2kMsg.Source, N2kMsg.Data, N2kMsg.DataLen);
  } else if (N2kMsg.PGN == 126996UL && (dvccEn == 1 || NMEA2KData == 1)) {
    dvccNoteProductInfo(N2kMsg);
  }
  // Receive toggle off: drop bus data here. ParseMessages may still be running purely to service
  // the transmit node's protocol layer (address claim, heartbeat), which the library handles
  // before this handler — the user's "receive off" must still mean no nav/battery data flows in.
  if (NMEA2KData != 1) return;
  int iHandler;
  if (NMEA2KVerbose == 1) {
    OutputStream->print("In Main Handler: ");
    OutputStream->println(N2kMsg.PGN);
  }
  for (iHandler = 0; NMEA2000Handlers[iHandler].PGN != 0 && !(N2kMsg.PGN == NMEA2000Handlers[iHandler].PGN); iHandler++)
    ;

  if (NMEA2000Handlers[iHandler].PGN != 0) {
    NMEA2000Handlers[iHandler].Handler(N2kMsg);
  }
}

// ===== DVCC-style charge-limit follow (CVL/CCL) — spec: Working Markdown Docs/DVCC_FOLLOW_SPEC.md =====
// Decode lives here (raw tap + 126720 handler): bounded arithmetic on already-received frames, no
// allocation, no I/O, no flash. The 1 Hz brain (dvccTick) runs the trust state machine and publishes
// the clamps the control path reads (dvccCvlV / dvccCclA). Control integration: AdjustFieldLearnMode.

// Trust-machine constants (protection-adjacent numbers approved by Mark 2026-08-19)
#define DVCC_CCL_MAX_A 1500.0f     // charge-current limit above this (or negative) = implausible → UNTRUSTED
#define DVCC_FLAP_DELTA_V 1.0f     // per-12V-class CVL jump between consecutive updates that counts as a flap
#define DVCC_FLAP_COUNT 5          // more than this many flaps inside the window → UNTRUSTED
#define DVCC_FLAP_WINDOW_MS 60000UL
#define DVCC_SETTLE_MIN_FRAMES 2   // RV-C broadcasts every 5 s, so settling is time-based plus this frame floor
#define DVCC_HANDOFF_SILENCE_MS 10000UL  // a rival sender is accepted after the current authority is silent this long

// dvccState values
#define DVCC_OFF 0
#define DVCC_WAITING 1
#define DVCC_SETTLING 2
#define DVCC_FOLLOWING 3
#define DVCC_STALE 4
#define DVCC_UNTRUSTED 5

static float dvccUntrustVal = 0.0f;    // the offending value, for the UI message
static uint8_t dvccVicLockAddr = 255;  // Victron path: bus address locked as the authority (255 = none)

// Flap detection lives in the INGEST path (per decoded update, per the spec's "between
// consecutive updates" rule) — a 1 Hz tick sampling the mailbox aliases fast flapping to
// steady. Ingest and tick both run on the loop task, so no synchronization is needed.
static float dvccFlapLastCvl = NAN;
static uint32_t dvccFlapWindowStartMs = 0;
static uint8_t dvccFlapCount = 0;
static bool dvccFlapLatch = false;  // set by ingest, converted to UNTRUSTED by dvccTick
static float dvccFlapVal = 0.0f;

// Victron VREG registers arrive ONE FIELD PER FRAME (0x2001 voltage, 0x2015 current, 0x2108
// allowed-to-charge), so the last-seen value of each is held here and re-ingested together.
static float dvccVicCvl = NAN;
static float dvccVicCcl = NAN;
static bool dvccVicCut = false;  // 0x2108 said do-not-charge → effective CCL forced to 0

// One clearing point for every piece of decode-side trust state. Called on manual latch reset,
// master toggle off, and source/instance edits (dvccCfgChanged) — a stale sender lock or flap
// history must never carry across any of those into judging a fresh authority.
static void dvccClearDecodeState() {
  dvccFlapLastCvl = NAN;
  dvccFlapWindowStartMs = 0;
  dvccFlapCount = 0;
  dvccFlapLatch = false;
  dvccVicLockAddr = 255;
  dvccVicCvl = dvccVicCcl = NAN;
  dvccVicCut = false;
}

void initDvccCapture() {
  dvccCapRing = (DvccCapEntry *)ps_malloc(sizeof(DvccCapEntry) * DVCC_CAP_N);
  if (!dvccCapRing) queueConsoleMessage("DVCC: capture ring PSRAM alloc failed (diagnostics only - follow unaffected)");
  dvccNodes = (DvccNode *)ps_malloc(sizeof(DvccNode) * DVCC_NODE_N);
  if (dvccNodes) memset(dvccNodes, 0, sizeof(DvccNode) * DVCC_NODE_N);
  else queueConsoleMessage("DVCC: node roster PSRAM alloc failed (capture loses sender identity - follow unaffected)");
}

// Copy free text into a fixed slot for the capture file: pad spaces trimmed, anything outside
// printable ASCII replaced. Two untrusted sources go through it — the 126996 strings a bus node
// sends, and the owner-typed battery make/model — and the capture is line-oriented, so a newline
// or control character reaching it unfiltered would forge header lines.
void dvccCopyPlainText(char *dst, size_t dstSz, const char *src) {
  size_t n = 0;
  for (; src[n] != 0 && n + 1 < dstSz; n++) {
    char c = src[n];
    dst[n] = (c >= 32 && c <= 126) ? c : '?';
  }
  while (n > 0 && dst[n - 1] == ' ') n--;
  dst[n] = 0;
}

// Roster slot for a bus address: allocates on first sight, and once full evicts the node heard
// longest ago. Callers hold dvccCapMux. Returns an INDEX, not a pointer — the Arduino
// preprocessor hoists its generated prototype above the struct definition in Xregulator.ino,
// so a function returning DvccNode* does not compile here.
static int dvccNodeSlot(uint8_t addr) {
  if (!dvccNodes) return -1;
  for (uint8_t i = 0; i < dvccNodeCount; i++)
    if (dvccNodes[i].addr == addr) return (int)i;
  uint8_t k;
  if (dvccNodeCount < DVCC_NODE_N) {
    k = dvccNodeCount++;
  } else {
    k = 0;
    for (uint8_t i = 1; i < DVCC_NODE_N; i++)
      if ((int32_t)(dvccNodes[i].lastMs - dvccNodes[k].lastMs) < 0) k = i;
  }
  memset(&dvccNodes[k], 0, sizeof(DvccNode));
  dvccNodes[k].addr = addr;
  return (int)k;
}

// PGN 60928 ISO Address Claim. The 8 NAME bytes, little-endian:
//   [0..3] u32: bits 0-20 unique number, bits 21-31 manufacturer code
//   [4] device instance (lower 3 bits | upper 5 bits)   [5] device function
//   [6] bit 0 reserved, bits 1-7 device class
//   [7] bit 7 arbitrary-address-capable, bits 4-6 industry group, bits 0-3 system instance
void dvccNoteName(uint8_t src, const unsigned char *d, int len) {
  if (!dvccNodes || len < 8) return;
  uint32_t lo = (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
  uint32_t now = millis();
  portENTER_CRITICAL(&dvccCapMux);
  int k = dvccNodeSlot(src);
  if (k >= 0) {
    // A different NAME on the same address means the node moved or was replaced. The 126996
    // strings sitting in this slot belong to the previous occupant — dropping them is what stops
    // a capture from naming the wrong device after an address-claim reshuffle.
    if (dvccNodes[k].haveName && (dvccNodes[k].unique != (lo & 0x1FFFFFUL) || dvccNodes[k].mfg != (uint16_t)(lo >> 21))) {
      dvccNodes[k].prodCode = 0;
      dvccNodes[k].vicProdId = 0;
      dvccNodes[k].model[0] = dvccNodes[k].sw[0] = dvccNodes[k].serial[0] = 0;
      dvccNodes[k].haveProd = 0;
    }
    dvccNodes[k].unique = lo & 0x1FFFFFUL;
    dvccNodes[k].mfg = (uint16_t)(lo >> 21);
    dvccNodes[k].devInst = d[4];
    dvccNodes[k].func = d[5];
    dvccNodes[k].cls = (uint8_t)(d[6] >> 1);
    dvccNodes[k].sysInst = (uint8_t)(d[7] & 0x0F);
    dvccNodes[k].haveName = 1;
    dvccNodes[k].lastMs = now;
  }
  portEXIT_CRITICAL(&dvccCapMux);
}

// PGN 126996 Product Information - fast-packet, so it arrives here already reassembled by the core
// library. The model string is what actually names a node ("Cerbo GX", "Lynx Smart BMS 500"), and
// the software version is what says which Venus release's DVCC behaviour a capture was taken under.
void dvccNoteProductInfo(const tN2kMsg &N2kMsg) {
  if (!dvccNodes) return;
  unsigned short n2kVer = 0, prodCode = 0;
  unsigned char cert = 0, loadEq = 0;
  char model[33] = { 0 }, sw[33] = { 0 }, modelVer[33] = { 0 }, serial[33] = { 0 };
  if (!ParseN2kPGN126996(N2kMsg, n2kVer, prodCode, sizeof(model), model, sizeof(sw), sw,
                         sizeof(modelVer), modelVer, sizeof(serial), serial, cert, loadEq)) return;
  uint32_t now = millis();
  portENTER_CRITICAL(&dvccCapMux);
  int k = dvccNodeSlot((uint8_t)N2kMsg.Source);
  if (k >= 0) {
    dvccNodes[k].prodCode = prodCode;
    dvccCopyPlainText(dvccNodes[k].model, sizeof(dvccNodes[k].model), model);
    dvccCopyPlainText(dvccNodes[k].sw, sizeof(dvccNodes[k].sw), sw);
    dvccCopyPlainText(dvccNodes[k].serial, sizeof(dvccNodes[k].serial), serial);
    dvccNodes[k].haveProd = 1;
    dvccNodes[k].lastMs = now;
  }
  portEXIT_CRITICAL(&dvccCapMux);
}

// CSV2 identity of the address the limits are currently arriving from. Called from dvccTick ahead
// of the follow gate: the observe-before-enable workflow needs it while follow is still off.
static void dvccAuthIdentityRefresh() {
  uint16_t mfg = 0, prod = 0;
  if (dvccNodes && dvccRxSrcAddr != 255) {
    portENTER_CRITICAL(&dvccCapMux);
    for (uint8_t i = 0; i < dvccNodeCount; i++) {
      if (dvccNodes[i].addr != dvccRxSrcAddr) continue;
      mfg = dvccNodes[i].mfg;
      prod = dvccNodes[i].prodCode ? dvccNodes[i].prodCode : dvccNodes[i].vicProdId;
      break;
    }
    portEXIT_CRITICAL(&dvccCapMux);
  }
  dvccAuthMfg = mfg;
  dvccAuthProd = prod;
}

static void dvccCaptureFrame(unsigned long id, unsigned char len, const unsigned char *buf) {
  if (!dvccCapRing) return;
  uint32_t now = millis();
  static uint32_t bucketMs = 0;
  static uint8_t bucket = 0;
  if (now - bucketMs >= 1000UL) {
    bucketMs = now;
    bucket = 0;
  }
  if (bucket >= 20) return;
  if (dvccCapPause) return;  // /dvccCapture is snapshotting the ring; dropping a frame beats tearing one
  static uint32_t dedupKey[16];
  static uint32_t dedupMs[16];
  // Key covers the FULL payload: identical repeats collapse, but a frame whose limit VALUE
  // changed must never dedup away — a GX limit ramp is exactly what a capture is for.
  uint32_t key = (uint32_t)id;
  for (uint8_t b = 0; b < len && b < 8; b++) key = key * 31u + buf[b];
  uint8_t slot = key & 0x0F;
  if (dedupKey[slot] == key && (uint32_t)(now - dedupMs[slot]) < 2000UL) return;
  dedupKey[slot] = key;
  dedupMs[slot] = now;
  bucket++;
  // Re-checked under dvccCapMux, not just at entry: the unlocked check above can pass a frame
  // that is still mid-copy when /dvccCapture (other core) sets the pause. Holding the lock across
  // the whole slot write is what removes that one-frame tear window - the reader takes the same
  // lock to set the pause, so a writer has either finished or backs off here.
  portENTER_CRITICAL(&dvccCapMux);
  if (!dvccCapPause) {
    DvccCapEntry &e = dvccCapRing[dvccCapHead];
    e.ms = now;
    e.id = (uint32_t)id;
    e.len = (len > 8) ? 8 : len;
    memset(e.data, 0, sizeof(e.data));
    memcpy(e.data, buf, e.len);
    dvccCapHead = (uint16_t)((dvccCapHead + 1) % DVCC_CAP_N);
    if (dvccCapCount < DVCC_CAP_N) dvccCapCount++;
  }
  portEXIT_CRITICAL(&dvccCapMux);
}

// RV-C Table 5.3 uint16 scalings: voltage 0.05 V/bit (>64250 = not available);
// current 0.05 A/bit offset −1600 A (0x7D00 = 0 A; >64250 = not available).
static inline float rvcU16ToVolts(uint16_t raw) {
  return (raw > 64250U) ? NAN : raw * 0.05f;
}
static inline float rvcU16ToAmps(uint16_t raw) {
  return (raw > 64250U) ? NAN : (raw - 32000.0f) * 0.05f;
}

// Common mailbox write for a decoded limit pair (either dialect). Flap detection happens here,
// on every real update — the spec's rule is per consecutive UPDATE, and only the ingest path
// sees them all (the 1 Hz tick would alias a fast flapper to a steady value).
static void dvccIngest(float cvl, float ccl, uint8_t srcAddr, uint8_t priority, uint8_t chgState) {
  if (!isnan(cvl)) {
    if (!isnan(dvccFlapLastCvl) && fabsf(cvl - dvccFlapLastCvl) > DVCC_FLAP_DELTA_V * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f)) {
      uint32_t nowMs = millis();
      if (dvccFlapWindowStartMs == 0 || (uint32_t)(nowMs - dvccFlapWindowStartMs) > DVCC_FLAP_WINDOW_MS) {
        dvccFlapWindowStartMs = nowMs;
        dvccFlapCount = 1;
      } else if (++dvccFlapCount > DVCC_FLAP_COUNT) {
        dvccFlapLatch = true;  // dvccTick converts this to the UNTRUSTED latch within 1 s
        dvccFlapVal = cvl;
      }
    }
    dvccFlapLastCvl = cvl;
  }
  dvccRxCvl = cvl;
  dvccRxCcl = ccl;
  dvccRxSrcAddr = srcAddr;
  dvccRxPriority = priority;
  dvccRxChgState = chgState;
  dvccRxLastMs = millis();
  dvccRxCount++;
  MARK_FRESH(IDX_DVCC);
}

// RV-C: one authority at a time. A different sender is accepted only with strictly higher
// device priority, or after the current authority has gone silent (VSR-style arbitration).
static bool dvccRvcAcceptSender(uint8_t srcAddr, uint8_t priority) {
  if (dvccRxSrcAddr == 255 || srcAddr == dvccRxSrcAddr) return true;
  if (priority > dvccRxPriority) return true;
  return (uint32_t)(millis() - dvccRxLastMs) > DVCC_HANDOFF_SILENCE_MS;
}

// Victron: no instance field on the VREG carrier — lock to the first sending bus address.
static bool dvccVicAcceptSender(uint8_t srcAddr) {
  if (dvccVicLockAddr == 255 || srcAddr == dvccVicLockAddr) {
    dvccVicLockAddr = srcAddr;
    return true;
  }
  if ((uint32_t)(millis() - dvccRxLastMs) > DVCC_HANDOFF_SILENCE_MS) {
    dvccVicLockAddr = srcAddr;
    dvccVicCvl = dvccVicCcl = NAN;  // never combine a new authority's first field with the old authority's held values
    dvccVicCut = false;
    return true;
  }
  return false;
}

// Victron VREG 0x351 payload (PROVISIONAL until §8a capture validation): mirrors the de-facto
// BMS-Can 0x351 register it is named after — CVL u16 0.1 V/bit, CCL s16 0.1 A/bit, little-endian.
static bool dvccDecodeVreg351(const unsigned char *p, int len, float &cvl, float &ccl) {
  if (len < 4) return false;
  uint16_t rawV = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
  int16_t rawA = (int16_t)((uint16_t)p[2] | ((uint16_t)p[3] << 8));
  if (rawV == 0xFFFF) return false;
  cvl = rawV * 0.1f;
  ccl = (rawA == (int16_t)0x7FFF) ? NAN : rawA * 0.1f;
  return true;
}

// One Victron VREG register frame (either carrier), payload p = the bytes after the vreg id.
// 0x2001 VE_REG_LINK_VSET un16 0.01 V/bit, 0xFFFF = not available: confirmed directly by Victron
// L3 support 2026-08-31 as THE register a GX broadcasts for DVCC voltage control (spec §17).
// 0x2016 VE_REG_LINK_CHARGE_VOLTAGE_SETPOINT is the other official vset register, decoded
// identically — not what a GX sends, kept because another Victron node may use it.
// 0x2015 VE_REG_LINK_CHARGE_CURRENT_LIMIT un16 0.1 A/bit (0xFFFF = limit not available/removed).
// 0x2108 VE_REG_BMS_IO field layout is from Victron's open Lynx Smart BMS decoder (spec §14);
// the register id itself and the 0x351 BMS-Can-mirror guess are still capture-pending.
// A wrong provisional read stays fail-safe by construction: CVL only ever clamps downward and a
// far-off value trips the plausibility window; CCL only moves a min-selected ceiling.
// Returns true when the register id was recognized (even if the sender lock rejected it).
static bool dvccVicHandleReg(uint16_t vreg, const unsigned char *p, int len, uint8_t src) {
  if (vreg == 0x0351) {
    float cvl = NAN, ccl = NAN;
    if (!dvccDecodeVreg351(p, len, cvl, ccl)) return false;
    if (!dvccVicAcceptSender(src)) return true;
    dvccVicCvl = cvl;
    dvccVicCcl = ccl;
  } else if (vreg == 0x2001 || vreg == 0x2016) {  // target voltage → CVL (0x2001 is the one a GX broadcasts)
    if (len < 2) return false;
    uint16_t raw = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    if (!dvccVicAcceptSender(src)) return true;
    // 0xFFFF = not available. It is a WITHDRAWAL, not a value: the GX publishes this register only
    // while DVCC is on with a smart battery or a manual CVL override, so the limit really can go
    // away while frames keep flowing. Clearing re-ingests NAN and dvccTick drops to WAITING within
    // 1 s; treating it as an unrecognized frame instead held the last clamp until the silence timeout.
    dvccVicCvl = (raw == 0xFFFF) ? NAN : raw * 0.01f;
  } else if (vreg == 0x2015) {  // current limit → CCL (un16; 0xFFFF clears the held limit, it is NOT a value)
    if (len < 2) return false;
    uint16_t raw = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    if (!dvccVicAcceptSender(src)) return true;
    dvccVicCcl = (raw == 0xFFFF) ? NAN : raw * 0.1f;
  } else if (vreg == 0x2108) {  // VE_REG_BMS_IO: 2-bit fields, allowed-to-charge at bits 2-3
    // (0 = unknown, 1 = allowed, 2/3 = not allowed) per Victron's own Lynx Smart BMS decoder,
    // dbus-ble-sensors/src/victron-lsbms.c. Revatek's whole-byte ==2 reading is ORed in until a
    // capture picks a winner: both hypotheses mean stop charging, and a spurious cut is the safe
    // error — 0xFF stays the not-available sentinel so a glitched byte never reads as a cut.
    if (len < 1 || p[0] == 0xFF) return false;
    if (!dvccVicAcceptSender(src)) return true;
    uint8_t atc = (uint8_t)((p[0] >> 2) & 0x03);
    dvccVicCut = (atc >= 2) || (p[0] == 2);
  } else if (vreg == 0x0100) {  // VE_REG_PRODUCT_ID — identity only, never a limit
    // Deliberately falls through as UNRECOGNIZED (return false): the caller uses the return value
    // to decide whether to retry the frame under the other VREG framing, and identity must never
    // cost us a limit decode on a frame whose id only coincidentally reads as 0x0100.
    if (len >= 2 && dvccNodes) {
      uint16_t pid = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
      uint32_t now = millis();
      portENTER_CRITICAL(&dvccCapMux);
      int k = dvccNodeSlot(src);
      if (k >= 0) {
        dvccNodes[k].vicProdId = pid;
        dvccNodes[k].lastMs = now;
      }
      portEXIT_CRITICAL(&dvccCapMux);
    }
    return false;
  } else {
    return false;
  }
  // Registers arrive one field per frame — re-ingest the held pair so the trust machine and
  // telemetry always see the combined view. A do-not-charge command is the strongest limit:
  // effective CCL 0 (the same obey-zero semantics Mark approved for a literal CCL=0).
  dvccIngest(dvccVicCvl, dvccVicCut ? 0.0f : dvccVicCcl, src, 0, 0);
  return true;
}

// Victron proprietary fast-packet (126720): 2-byte proprietary header (manufacturer code 358 +
// industry group), then the VREG id. Both plausible framings are tried — vreg id directly at
// bytes 2-3, or after a 1-byte command at bytes 3-4 — until the §8a capture pins the real one.
void VictronVreg126720(const tN2kMsg &N2kMsg) {
  if (dvccSrcType != 0) return;
  if (N2kMsg.DataLen < 6) return;
  // A control vreg is only ours as a broadcast or addressed to our node — an addressed frame to
  // another charger carries THAT node's per-charger allocation, never the battery limit (§13).
  if (N2kMsg.Destination != 0xFF && N2kMsg.Destination != NMEA2000.GetN2kSource()) return;
  uint16_t mfr = ((uint16_t)N2kMsg.Data[0] | ((uint16_t)N2kMsg.Data[1] << 8)) & 0x07FF;
  if (mfr != 358) return;  // Victron manufacturer code
  uint16_t reg23 = (uint16_t)N2kMsg.Data[2] | ((uint16_t)N2kMsg.Data[3] << 8);
  bool got = dvccVicHandleReg(reg23, &N2kMsg.Data[4], N2kMsg.DataLen - 4, N2kMsg.Source);
  if (!got && N2kMsg.DataLen >= 7) {
    uint16_t reg34 = (uint16_t)N2kMsg.Data[3] | ((uint16_t)N2kMsg.Data[4] << 8);
    dvccVicHandleReg(reg34, &N2kMsg.Data[5], N2kMsg.DataLen - 5, N2kMsg.Source);
  }
}

// Raw-RX tap, registered on the NMEA2000_esp32_xeng fork. Runs in ParseMessages caller context
// (loop task, core 1) for EVERY received frame — the non-candidate exit is a few integer compares.
// Carries the §8a capture feed plus the single-frame decodes the core library can't be trusted to
// surface: RV-C DGNs (fast-packet-range collisions) and the 0xEF00 proprietary VREG carrier.
void dvccRawFrameTap(unsigned long id, unsigned char len, const unsigned char *buf) {
  if (NMEA2KData != 1 && dvccEn != 1) return;
  uint32_t pgn = (uint32_t)((id >> 8) & 0x1FFFFUL);
  uint8_t pf = (uint8_t)((id >> 16) & 0xFF);
  uint8_t dest = 0xFF;  // PDU2 frames are inherently broadcast
  if (pf < 240) {
    dest = (uint8_t)((id >> 8) & 0xFF);
    pgn &= 0x1FF00UL;  // PDU1 (destination-addressed): low byte is the destination, not part of the PGN
  }
  uint8_t src = (uint8_t)(id & 0xFF);
  bool candidate = (pgn == 126720UL) || (pgn == 61184UL) || (pgn >= 130560UL && pgn <= 131071UL)  // RV-C DC/battery families + N2K proprietary fast-packet range
                   || (pgn >= 65280UL && pgn <= 65535UL);                                         // single-frame proprietary range
  if (!candidate) return;
  dvccCaptureFrame(id, len, buf);
  if (dvccSrcType == 1) {
    // RV-C battery authority: DC_SOURCE_STATUS_4 (1FEC9h) / BATTERY_STATUS_4 (1FE92h), single frame
    if ((pgn == 0x1FEC9UL || pgn == 0x1FE92UL) && len >= 7) {
      uint8_t inst = buf[0];
      if (dvccInst != 0 && inst != (uint8_t)dvccInst) return;
      uint8_t prio = (pgn == 0x1FEC9UL) ? buf[1] : 120;  // BATTERY_STATUS_4 has no priority field; treat as BMS-grade
      if (!dvccRvcAcceptSender(src, prio)) return;
      float cvl = rvcU16ToVolts((uint16_t)buf[3] | ((uint16_t)buf[4] << 8));
      float ccl = rvcU16ToAmps((uint16_t)buf[5] | ((uint16_t)buf[6] << 8));
      if (buf[2] == 1) ccl = 0.0f;  // desired charge state 1 = do not charge (stop immediately) — same obey-zero semantics as a literal CCL=0
      if (isnan(cvl) && isnan(ccl)) return;
      dvccIngest(cvl, ccl, src, prio, buf[2]);
    }
    // DC_SOURCE_STATUS_2 (1FFFCh): source temperature of the same instance -> batteryTempF() source 4.
    // Bytes 2-3 LE u16, 0.03125 degC/bit with a -273 degC offset, 0xFFFF = not available.
    if (pgn == 0x1FFFCUL && len >= 4) {
      uint8_t inst = buf[0];
      if (dvccInst != 0 && inst != (uint8_t)dvccInst) return;
      if (!dvccRvcAcceptSender(src, buf[1])) return;
      uint16_t raw = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
      if (raw != 0xFFFF) {
        float tC = raw * 0.03125f - 273.0f;
        if (tC >= -40.0f && tC <= 100.0f) {  // same plausibility window as the VE.Direct T field
          rvcRxBattTempF = tC * 1.8f + 32.0f;
          MARK_FRESH(IDX_RVC_BATT_TEMP);
        }
      }
    }
  } else {
    // Victron VREG on the 0xEF00 proprietary single-frame carrier: vreg id at bytes 2-3 (LE),
    // payload after (framing per the Revatek prior art; provisional until capture validation).
    // Manufacturer code 358 in the 2-byte proprietary header, same as the 126720 carrier.
    if (pgn == 61184UL && len >= 5) {
      // Broadcast or addressed-to-us only — an addressed frame to another charger carries THAT
      // node's per-charger allocation, never the battery limit (§13).
      if (dest != 0xFF && dest != NMEA2000.GetN2kSource()) return;
      uint16_t mfr = ((uint16_t)buf[0] | ((uint16_t)buf[1] << 8)) & 0x07FF;
      if (mfr != 358) return;
      uint16_t vreg = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
      dvccVicHandleReg(vreg, &buf[4], (int)len - 4, src);
    }
  }
}

// The 1 Hz brain: trust state machine (spec §3). Publishes dvccCvlV / dvccCclA for the control
// path. Arithmetic only. The clamps are additionally gated at the application sites on
// dvccEn == 1, so a mid-session disable kills them even before this tick runs again.
void dvccTick() {
  static uint32_t lastTickMs = 0;
  uint32_t now = millis();
  if ((uint32_t)(now - lastTickMs) < 1000UL) return;
  lastTickMs = now;
  dvccAuthIdentityRefresh();

  static uint32_t lastSeenCount = 0;
  static uint32_t settleStartMs = 0;
  static uint16_t settleFrames = 0;
  static bool offStateCleared = false;

  if (dvccResetReq) {
    dvccResetReq = false;
    if (dvccState == DVCC_UNTRUSTED) {
      dvccState = DVCC_WAITING;
      dvccUntrustReason = 0;
      settleStartMs = 0;
      settleFrames = 0;
      dvccClearDecodeState();
      queueConsoleMessage("DVCC: trust latch reset - authority must settle again before being followed");
    }
  }
  if (dvccCfgChanged) {
    // Source/instance edited, or follow just enabled: judge the authority with clean decode
    // state (sender lock, flap history, held Victron registers). An UNTRUSTED latch is
    // deliberately preserved — it clears only via the reset button, the master toggle, or reboot.
    dvccCfgChanged = false;
    settleStartMs = 0;
    settleFrames = 0;
    dvccClearDecodeState();
  }

  if (dvccEn != 1) {
    dvccState = DVCC_OFF;
    dvccCvlV = dvccCclA = NAN;
    dvccUntrustReason = 0;  // master off/on is an approved latch-reset path
    settleStartMs = 0;
    settleFrames = 0;
    // Clear on the OFF transition only. Victron sends CVL and CCL as separate register frames, so
    // clearing every tick while off meant the held pair was wiped between them and the two limits
    // could never be read together — exactly the check the UI asks for before follow is enabled.
    if (!offStateCleared) {
      dvccClearDecodeState();
      offStateCleared = true;
    }
    return;
  }
  offStateCleared = false;

  bool newFrame = (dvccRxCount != lastSeenCount);
  lastSeenCount = dvccRxCount;
  bool silent = (dvccRxLastMs == 0) || ((uint32_t)(now - dvccRxLastMs) > (uint32_t)dvccSilenceS * 1000UL);

  if (dvccState == DVCC_UNTRUSTED) {
    dvccCvlV = dvccCclA = NAN;  // latched: local control until manual reset / master toggle / reboot
    return;
  }

  // Ingest-side flap detection latched (checked ahead of silence — a lying authority that then
  // goes quiet must still latch, never quietly re-settle).
  if (dvccFlapLatch) {
    dvccFlapLatch = false;
    dvccState = DVCC_UNTRUSTED;
    dvccUntrustReason = 3;
    dvccUntrustVal = dvccFlapVal;
    dvccCvlV = dvccCclA = NAN;
    queueConsoleMessage("DVCC: authority CVL flapping - UNTRUSTED (local control; manual reset required)");
    return;
  }

  if (silent) {
    if (dvccState == DVCC_FOLLOWING) {
      queueConsoleMessageF("DVCC: authority silent >%ds - reverting to local targets", dvccSilenceS);
      dvccState = DVCC_STALE;
    } else if (dvccState != DVCC_STALE) {
      dvccState = DVCC_WAITING;
    }
    dvccCvlV = dvccCclA = NAN;
    settleStartMs = 0;
    settleFrames = 0;
    dvccClearDecodeState();  // held registers / sender lock / flap history must not resurface as "fresh" when frames return
    return;
  }

  // Frames are arriving — validate the latest values every tick (a lying authority must not
  // keep steering; one implausible value latches UNTRUSTED, never retried automatically).
  bool cvlOk = isnan(dvccRxCvl) || (dvccRxCvl >= dvccCvlMin && dvccRxCvl <= dvccCvlMax);
  bool cclOk = isnan(dvccRxCcl) || (dvccRxCcl >= 0.0f && dvccRxCcl <= DVCC_CCL_MAX_A);
  if (!cvlOk || !cclOk) {
    dvccState = DVCC_UNTRUSTED;
    dvccUntrustReason = !cvlOk ? 1 : 2;
    dvccUntrustVal = !cvlOk ? dvccRxCvl : dvccRxCcl;
    dvccCvlV = dvccCclA = NAN;
    queueConsoleMessageF("DVCC: implausible %s %.2f from authority - UNTRUSTED (local control; manual reset required)",
                         !cvlOk ? "CVL" : "CCL", dvccUntrustVal);
    return;
  }
  if (isnan(dvccRxCvl) && isnan(dvccRxCcl)) {
    dvccState = DVCC_WAITING;  // frames but no usable limit fields
    dvccCvlV = dvccCclA = NAN;
    // Stale settle progress here would promote the next usable limits to FOLLOWING on their
    // first frame, skipping the settling vet (spec: consecutive in-range updates required).
    settleStartMs = 0;
    settleFrames = 0;
    return;
  }

  if (dvccState != DVCC_FOLLOWING) {
    if (settleStartMs == 0) {
      settleStartMs = now;
      settleFrames = 0;
    }
    if (newFrame) settleFrames++;
    dvccState = DVCC_SETTLING;
    if ((uint32_t)(now - settleStartMs) >= (uint32_t)dvccSettleS * 1000UL && settleFrames >= DVCC_SETTLE_MIN_FRAMES) {
      dvccState = DVCC_FOLLOWING;
      queueConsoleMessageF("DVCC: following authority (addr %u) CVL=%.2fV CCL=%.1fA (-1 = not sent)",
                           (unsigned)dvccRxSrcAddr, isnan(dvccRxCvl) ? -1.0f : dvccRxCvl, isnan(dvccRxCcl) ? -1.0f : dvccRxCcl);
    }
  }
  if (dvccState == DVCC_FOLLOWING) {
    dvccCvlV = dvccRxCvl;
    dvccCclA = dvccRxCcl;
  } else {
    dvccCvlV = dvccCclA = NAN;
  }
}

// ===== NMEA2000 transmit (producer) — spec: Working Markdown Docs/NMEA2K_TRANSMIT_SPEC.md =====

tN2kChargeState n2kChargeStateFromStage(uint8_t stage) {
  switch (stage) {
    case CHARGE_STAGE_BULK: return N2kCS_Bulk;
    case CHARGE_STAGE_ABSORPTION: return N2kCS_Absorption;
    case CHARGE_STAGE_FLOAT: return N2kCS_Float;
    case CHARGE_STAGE_MAINTAIN: return N2kCS_Float;
    case CHARGE_STAGE_MANUAL:
    case CHARGE_STAGE_TARGET_V:
    case CHARGE_STAGE_COMMISSION: return N2kCS_Constant_VI;
    default: return N2kCS_Not_Charging;  // NONE, IDLE
  }
}

// Labelling only — both charger PGNs carry it; nothing in the control path reads it.
tN2kChargerMode n2kChargerModeSetting() {
  switch (n2kChgrMode) {
    case 1: return N2kCM_Primary;
    case 2: return N2kCM_Secondary;
    default: return N2kCM_Standalone;
  }
}

// The alternator temperature the alarm engine uses: TempSource picks OneWire vs thermistor,
// -99 is the thermistor's disconnected sentinel. NAN = no usable reading. Same 20s freshness
// test as the stale alarm / temp-stale field cut — without it a sensor that dies mid-run
// transmits its last good value forever and the MFD data-lost alarm can never fire.
float n2kAltTempF() {
  unsigned long ts = dataTimestamps[(TempSource == 0) ? IDX_ALTERNATOR_TEMP : IDX_THERMISTOR_TEMP];
  if (ts == 0 || (millis() - ts) > 20000) return NAN;
  if (TempSource == 0) return AlternatorTemperatureF;
  return (temperatureThermistor == -99) ? NAN : (float)temperatureThermistor;
}

// ===== RV-C producer — spec: Working Markdown Docs/RVC_TRANSMIT_SPEC.md =====
// RV-C is a J1939 application layer that shares the 250 kbps wire with NMEA 2000, so its DGNs are
// composed as tN2kMsg and go out through the same SendMsg path and the same claimed address.
// Field scalings are RV-C Table 5.3 (spec revision July 31, 2025). Every "not available" value is
// all-bits-1, the J1939 convention — a source we do not have is NEVER encoded as a number.

// Forces SINGLE-FRAME transmit. DGNs 130816-131071 (1FFC7h, 1FFFBh-1FFFDh) sit inside the range
// the NMEA2000 library treats as proprietary FAST PACKET; without this, SendMsg would prepend a
// fast-packet sequence byte and split an 8-byte RV-C frame across two CAN frames, which no RV-C
// node can decode. tNMEA2000::IsFastPacket() returns false for Priority>=0x80, and N2ktoCanID()
// masks priority to 3 bits, so the wire still carries RV-C's default priority 6.
static void rvcBeginMsg(tN2kMsg &m, unsigned long dgn) {
  m.SetPGN(dgn);
  m.Priority = 6 | 0x80;
}

static inline uint16_t rvcVoltsU16(float v) {  // 0.050 V/bit, 0..3212.5 V
  if (!isfinite(v) || v < 0.0f || v > 3212.5f) return 0xFFFF;
  return (uint16_t)lroundf(v / 0.05f);
}
static inline uint16_t rvcAmpsU16(float a) {  // 0.05 A/bit, -1600..1612.5 A, 0 A = 7D00h
  if (!isfinite(a) || a < -1600.0f || a > 1612.5f) return 0xFFFF;
  return (uint16_t)lroundf(a / 0.05f + 32000.0f);
}
static inline uint32_t rvcAmpsU32(float a) {  // 0.001 A/bit, 0 A = 77359400h
  if (!isfinite(a) || a < -2000000.0f || a > 2221081.0f) return 0xFFFFFFFFUL;
  return (uint32_t)(llround((double)a * 1000.0) + 2000000000LL);
}
static inline uint16_t rvcTempU16(float c) {  // 0.03125 degC/bit, -273..1735 degC
  if (!isfinite(c) || c < -273.0f || c > 1735.0f) return 0xFFFF;
  return (uint16_t)lroundf((c + 273.0f) / 0.03125f);
}
static inline uint8_t rvcTempU8(float c) {  // 1 degC/bit, -40..210 degC
  if (!isfinite(c) || c < -40.0f || c > 210.0f) return 0xFF;
  return (uint8_t)lroundf(c + 40.0f);
}
static inline uint8_t rvcPctU8(float p) {  // 0.5 %/bit, 0..125 %
  if (!isfinite(p) || p < 0.0f || p > 125.0f) return 0xFF;
  return (uint8_t)lroundf(p / 0.5f);
}
static inline float rvcAltTempC() {
  float f = n2kAltTempF();
  return isfinite(f) ? (f - 32.0f) / 1.8f : NAN;
}

// RV-C charger operating state (spec Table 6.20.8b). Unlike the N2K map this has an explicit
// Disable state, so the master switch being off is distinguishable from "on but not charging".
static uint8_t rvcChargeState() {
  if (OnOff != 1) return 0;  // Disable
  switch (getChargeStageDisplayCode()) {
    case CHARGE_STAGE_BULK: return 2;
    case CHARGE_STAGE_ABSORPTION: return 3;
    case CHARGE_STAGE_FLOAT:
    case CHARGE_STAGE_MAINTAIN: return 6;
    case CHARGE_STAGE_MANUAL:
    case CHARGE_STAGE_TARGET_V:
    case CHARGE_STAGE_COMMISSION: return 7;  // constant voltage / current
    default: return 1;                       // Not charging (NONE, IDLE, protective cut)
  }
}

// CHARGER_STATUS_3 derating reason (spec Table 6.20.10b) from our banner limiter code. The RV-C
// enum only names thermal, battery-temperature and battery-voltage causes, so the limiters with
// no equivalent (RPM/belt cap, max duty, protection recovery, BMS current limit) report the
// derating bit with reason 255 = not available rather than a wrong named cause.
static void rvcDeratingFromLimiter(uint8_t &derating, uint8_t &reason) {
  if (ctrlLimiter == 0) { derating = 0; reason = 0; return; }
  derating = 1;
  switch (ctrlLimiter) {
    case 2: reason = 1; break;  // thermal derate -> High Internal Temperature
    case 3:                     // CV voltage loop
    case 9: reason = 3; break;  // BMS charge-voltage limit -> Battery Voltage
    default: reason = 0xFF; break;
  }
}

// Highest-severity active fault as an RV-C service point. SPN encoding per spec 3.2.5.5:
// MSB byte, ISB byte, then 3 LS bits. Under DSA 76 (Charge Controller) the spec defines no SPN
// table, so ours is documented in RVC_TRANSMIT_SPEC.md §5 — MSB 1 keeps the Charger table's
// numbering for the points that exist there (DC voltage / DC current / battery temperature) and
// MSB 2 carries the alternator-specific points the Charger table has no room for.
// Returns true when a fault is active; red = severe (charging stopped), else yellow.
static bool rvcActiveFault(uint8_t &msb, uint8_t &lsb, uint8_t &fmi, bool &red) {
  red = true;
  bool cut = gpio4IsLow;
  if (cut) {
    switch (g_fieldEventReason) {
      case REASON_FAST_OVERVOLTAGE:
      case REASON_INA_OVERVOLTAGE:
      case REASON_OV_TIER_LOW:
      case REASON_OV_TIER_MID:
      case REASON_LOCKOUT_ACTIVE:
        msb = 1; lsb = 0; fmi = 0; return true;  // DC voltage above normal range
      case REASON_VOLTAGE_IMPLAUSIBLE:
      case REASON_VOLTAGE_DISAGREE_CRITICAL:
      case REASON_VOLTAGE_SPIKE:
        msb = 1; lsb = 0; fmi = 2; return true;  // DC voltage erratic or invalid
      case REASON_HARD_OVERCURRENT:
        msb = 1; lsb = 1; fmi = 0; return true;  // DC current above normal range
      case REASON_CURRENT_STALE:
        msb = 1; lsb = 1; fmi = 9; return true;  // DC current not updating at proper rate
      case REASON_BATTERY_TOO_COLD:
        msb = 1; lsb = 2; fmi = 1; return true;  // battery temperature below normal range
      case REASON_BATTERY_TOO_HOT:
        msb = 1; lsb = 2; fmi = 0; return true;  // battery temperature above normal range
      case REASON_TEMP_CRITICAL:
      case REASON_TEMP_WARNING:
      case REASON_TEMP_SUSTAINED:
        msb = 2; lsb = 0; fmi = 0; return true;  // alternator temperature above normal range
      case REASON_TEMP_STALE:
        msb = 2; lsb = 0; fmi = 9; return true;  // alternator temperature not updating
      case REASON_TACH_IMPLAUSIBLE:
        msb = 2; lsb = 3; fmi = 2; return true;  // engine speed erratic or invalid
      default: break;                            // disabled / manual / low RPM / rest / solar / BMS are not faults
    }
  }
  red = false;
  float tF = n2kAltTempF();
  if (TempAlarm > 0 && isfinite(tF) && tF > (float)TempAlarm) {
    msb = 2; lsb = 0; fmi = 15; return true;  // valid but above normal operating range, least severe
  }
  if (g_fieldEventReason == REASON_VOLTAGE_DISAGREE_WARNING) {
    msb = 1; lsb = 0; fmi = 2; return true;
  }
  return false;
}

// Sends AT MOST one PGN per loop pass (rotating scan for fairness); compose + enqueue is µs-scale
// because the _xeng driver never blocks — frames the TWAI queue refuses land in the core library's
// retry ring and eventually drop (counted). Missing sources publish N2K not-available, never a
// stale/fabricated number; the single-value 130312 is skipped entirely instead.
void nmea2kTransmitTick() {
  if (n2kTxEnable != 1 && rvcTxEnable != 1) return;

  // Persist a claim/renegotiation result. NVS write is deferred to field-off (no flash in the
  // control path); claims normally complete at boot with the field down anyway.
  if (NMEA2000.ReadResetAddressChanged()) {
    n2kSrcAddrLive = NMEA2000.GetN2kSource();
    n2kAddrPending = (uint8_t)n2kSrcAddrLive;
  }
  if (n2kAddrPending != 255 && gpio4IsLow) {
    settingWrite(NK_n2kSrcAddr, String((int)n2kAddrPending).c_str());
    n2kAddrPending = 255;
  }

  enum : uint8_t { SLOT_BATT = 0,
                   SLOT_BATT_DC,
                   SLOT_ALT,
                   SLOT_ALT_DC,
                   SLOT_TEMP,
                   SLOT_TEMP_EXTRA,
                   SLOT_CHGR,
                   SLOT_CHGRCFG,
                   SLOT_ENG_RAPID,
                   SLOT_ENG_DYN,
                   SLOT_BATTCFG,
                   SLOT_RVC_CHGR,      // ---- RV-C family below this line (see the master gate in the scan) ----
                   SLOT_RVC_CHGR2,
                   SLOT_RVC_CHGR3,
                   SLOT_RVC_CHGRCFG,
                   SLOT_RVC_DC1,
                   SLOT_RVC_DC2,
                   SLOT_RVC_DC3,
                   SLOT_RVC_DM,
                   SLOT_N };
  // ms. N2K slots use the NMEA-recommended rates; RV-C slots use its "normal broadcast gap"
  // (spec 6.20.8a/6.20.9a/6.20.10a/6.5.2a-6.5.4a). DM_RV re-arms to 1000 ms while faulted.
  static const uint32_t ivl[SLOT_N] = { 1500, 1500, 1500, 1500, 2000, 2000, 1500, 1500, 100, 500, 15000,
                                        5000, 500, 500, 5000, 500, 500, 500, 5000 };
  static uint32_t nextDue[SLOT_N];
  static bool seeded = false;
  // Per-stream SIDs: each 127508+127506 pair shares one (ties the pair to the same sample set)
  // and each stream bumps its own, so disabling one stream can't freeze another's SID.
  static uint8_t sidBatt = 0, sidAlt = 0, sidTemp = 0, sidTempExtra = 0;
  static uint8_t rr = 0;
  static tN2kChargeState lastChgState = N2kCS_Unavailable;
  static uint8_t lastRvcState = 0xFF;
  uint32_t now = millis();
  if (!seeded) {  // stagger initial phases so the 1500ms PGNs never bunch into one pass
    seeded = true;
    for (uint8_t i = 0; i < SLOT_N; i++) nextDue[i] = now + 500 + 200 * i;
  }

  // Charge-stage change sends 127507 promptly instead of waiting out its interval
  if (n2kTxEnable == 1 && n2kChgrEnable == 1 && n2kChargeStateFromStage(getChargeStageDisplayCode()) != lastChgState && (int32_t)(now - nextDue[SLOT_CHGR]) < 0) {
    nextDue[SLOT_CHGR] = now;
  }
  // Same idea on the RV-C side: CHARGER_STATUS is "5000 ms or on change" (spec 6.20.8a)
  if (rvcTxEnable == 1 && rvcChgrEnable == 1 && rvcChargeState() != lastRvcState && (int32_t)(now - nextDue[SLOT_RVC_CHGR]) < 0) {
    nextDue[SLOT_RVC_CHGR] = now;
  }

  for (uint8_t k = 0; k < SLOT_N; k++) {
    uint8_t s = (rr + k) % SLOT_N;
    if ((int32_t)(now - nextDue[s]) < 0) continue;
    nextDue[s] = now + ivl[s];
    // Family master gate. The per-DGN toggles inside each case are subordinate to it, and the
    // schedule is advanced first so a disabled family cannot re-arm every pass.
    if ((s >= SLOT_RVC_CHGR) ? (rvcTxEnable != 1) : (n2kTxEnable != 1)) continue;

    tN2kMsg N2kMsg;
    bool composed = false;
    switch (s) {
      case SLOT_BATT:
        if (n2kBattEnable == 1) {
          sidBatt = (uint8_t)((sidBatt + 1) % 253);
          // Battery temperature: the BATT probe only — never the board stand-in, never a re-broadcast network value
          bool battProbeOk = battTempProbeEnable == 1 && tempProbeFresh(IDX_BATT_TEMP_PROBE) && isfinite(BatteryTempProbeF);
          SetN2kDCBatStatus(N2kMsg, (unsigned char)n2kBattInstance, getBatteryVoltage(),
                            HAS_BATT_SHUNT ? (double)Bcur : N2kDoubleNA,
                            battProbeOk ? FToKelvin(BatteryTempProbeF) : N2kDoubleNA,
                            sidBatt);
          composed = true;
        }
        break;
      case SLOT_BATT_DC:
        if (n2kBattEnable == 1) {
          SetN2kDCStatus(N2kMsg, sidBatt, (unsigned char)n2kBattInstance, N2kDCt_Battery,
                         (HAS_BATT_SHUNT && socInfoAvailable) ? (unsigned char)(SOC_percent / 100) : N2kUInt8NA,
                         N2kUInt8NA, N2kDoubleNA, N2kDoubleNA, N2kDoubleNA);
          composed = true;
        }
        break;
      case SLOT_ALT:
        if (n2kAltEnable == 1) {
          float tF = n2kAltTempF();
          sidAlt = (uint8_t)((sidAlt + 1) % 253);
          SetN2kDCBatStatus(N2kMsg, (unsigned char)n2kAltInstance, (double)BatteryV, (double)MeasuredAmps,
                            isfinite(tF) ? FToKelvin(tF) : N2kDoubleNA, sidAlt);
          composed = true;
        }
        break;
      case SLOT_ALT_DC:
        if (n2kAltEnable == 1) {
          SetN2kDCStatus(N2kMsg, sidAlt, (unsigned char)n2kAltInstance, N2kDCt_Alternator,
                         N2kUInt8NA, N2kUInt8NA, N2kDoubleNA, N2kDoubleNA, N2kDoubleNA);
          composed = true;
        }
        break;
      case SLOT_TEMP:
        if (n2kAltTempEnable == 1) {
          float tF = n2kAltTempF();
          if (isfinite(tF)) {  // skipped entirely while the sensor is dead — MFD data-lost alarms own that case
            sidTemp = (uint8_t)((sidTemp + 1) % 253);
            SetN2kTemperature(N2kMsg, sidTemp, (unsigned char)n2kTempInstance, (tN2kTempSource)n2kTempSource,
                              FToKelvin(tF), N2kDoubleNA);
            composed = true;
          }
        }
        break;
      case SLOT_TEMP_EXTRA:
        if (n2kExtraTempEnable == 1) {
          if (tempProbeFresh(IDX_EXTRA_TEMP) && isfinite(ExtraTempF)) {  // skipped entirely while the probe is dead, as SLOT_TEMP
            sidTempExtra = (uint8_t)((sidTempExtra + 1) % 253);
            SetN2kTemperature(N2kMsg, sidTempExtra, (unsigned char)n2kExtraTempInstance, (tN2kTempSource)n2kExtraTempSource,
                              FToKelvin(ExtraTempF), N2kDoubleNA);
            composed = true;
          }
        }
        break;
      case SLOT_CHGR:
        if (n2kChgrEnable == 1) {
          lastChgState = n2kChargeStateFromStage(getChargeStageDisplayCode());
          SetN2kChargerStatus(N2kMsg, (unsigned char)n2kChgrInstance, (unsigned char)n2kBattInstance,
                              lastChgState, n2kChargerModeSetting(),
                              (OnOff == 1) ? N2kOnOff_On : N2kOnOff_Off);
          composed = true;
        }
        break;
      case SLOT_CHGRCFG:
        if (n2kChgrCfgEnable == 1) {
          // ChargeCurrentLimit is the field the one integrated alternator regulator (Wakespeed,
          // venus#779) puts field drive % in — 1 %/bit, so it is the whole carrier for the number.
          unsigned char fieldPct = isfinite(dutyCycle) ? (unsigned char)lroundf(constrain(dutyCycle, 0.0f, 100.0f))
                                                       : N2kUInt8NA;
          SetN2kChargerConf(N2kMsg, (unsigned char)n2kChgrInstance, (unsigned char)n2kBattInstance,
                            (OnOff == 1) ? N2kOnOff_On : N2kOnOff_Off, fieldPct,
                            N2kCA_3State, n2kChargerModeSetting(), N2kBT_NotAvailable,
                            N2kOnOff_Off, N2kOnOff_Off, 0);
          composed = true;
        }
        break;
      case SLOT_ENG_RAPID:
        if (n2kEngRpmEnable == 1) {
          SetN2kEngineParamRapid(N2kMsg, (unsigned char)n2kEngInstance, (double)RPM);
          composed = true;
        }
        break;
      case SLOT_ENG_DYN:
        if (n2kEngDynEnable == 1) {
          tN2kEngineDiscreteStatus1 s1 = 0;
          if (n2kEngBitsEnable == 1) {
            // Over Temperature: HARD over-temp field cut or the user's high-temp alarm point —
            // deliberately never the thermal derate (output reduction is normal operation).
            bool hardTempCut = gpio4IsLow && (g_fieldEventReason == REASON_TEMP_CRITICAL || g_fieldEventReason == REASON_TEMP_WARNING || g_fieldEventReason == REASON_TEMP_SUSTAINED);
            float tF = n2kAltTempF();
            if (hardTempCut || (TempAlarm > 0 && isfinite(tF) && tF > TempAlarm)) s1.Bits.OverTemperature = 1;
            float v = getBatteryVoltage();
            // same class-scaled disconnected-sensor floor as CheckAlarms
            if (VoltageAlarmLow > 0 && v < VoltageAlarmLow && v > 8.0f * SYSTEM_VOLTAGE_CLASS / 12.0f) s1.Bits.LowSystemVoltage = 1;
            // Charge Indicator = not charging when it should be: protective field cut (the JS
            // FIELD_FAULT_REASONS set: 1-9, 12, 13, 15-17 — covers the implausible-sensor cuts)
            // while the engine turns fast enough that the field would otherwise be allowed.
            bool faultCut = gpio4IsLow && g_fieldEventReason >= 1 && g_fieldEventReason <= 17
                            && g_fieldEventReason != REASON_CHARGING_DISABLED && g_fieldEventReason != REASON_MANUAL_MODE && g_fieldEventReason != REASON_RPM_TOO_LOW;
            if (faultCut && RPM > MinRPMForField) s1.Bits.ChargeIndicator = 1;
          }
          SetN2kEngineDynamicParam(N2kMsg, (unsigned char)n2kEngInstance, N2kDoubleNA, N2kDoubleNA, N2kDoubleNA,
                                   (double)BatteryV, N2kDoubleNA, N2kDoubleNA, N2kDoubleNA, N2kDoubleNA,
                                   N2kInt8NA, N2kInt8NA, s1, 0);
          composed = true;
        }
        break;
      case SLOT_BATTCFG:
        if (n2kBattCfgEnable == 1) {
          tN2kBatType bt = N2kDCbt_Flooded;
          tN2kBatChem bc = N2kDCbc_LeadAcid;
          if (strcasecmp(BATTERY_TYPE, "lifepo4") == 0) bc = N2kDCbc_LiIon;
          else if (strcasecmp(BATTERY_TYPE, "agm") == 0) bt = N2kDCbt_AGM;
          else if (strcasecmp(BATTERY_TYPE, "gel") == 0) bt = N2kDCbt_Gel;
          tN2kBatNomVolt nv = (SYSTEM_VOLTAGE_CLASS == 12) ? N2kDCbnv_12v
                              : (SYSTEM_VOLTAGE_CLASS == 24) ? N2kDCbnv_24v
                              : (SYSTEM_VOLTAGE_CLASS == 48) ? N2kDCbnv_48v
                                                             : (tN2kBatNomVolt)0x0F;  // no 36V code in the 4-bit field — NA
          SetN2kBatConf(N2kMsg, (unsigned char)n2kBattInstance, bt,
                        (bt == N2kDCbt_Flooded && bc == N2kDCbc_LeadAcid) ? N2kDCES_Yes : N2kDCES_No,
                        nv, bc, AhToCoulomb((double)BatteryCapacity_Ah), N2kInt8NA,
                        PeukertExponent_scaled / 100.0, (int8_t)(ChargeEfficiency_scaled / 10));
          composed = true;
        }
        break;

      // ---- RV-C ----------------------------------------------------------------------------
      case SLOT_RVC_CHGR:  // CHARGER_STATUS 1FFC7h — what we are TRYING to deliver
        if (rvcChgrEnable == 1) {
          lastRvcState = rvcChargeState();
          float pctMax = (AlternatorNominalAmps > 0 && isfinite(uTargetAmps))
                           ? (100.0f * uTargetAmps / (float)AlternatorNominalAmps) : NAN;
          rvcBeginMsg(N2kMsg, 0x1FFC7UL);
          N2kMsg.AddByte((unsigned char)rvcChgrInstance);
          N2kMsg.Add2ByteUInt(rvcVoltsU16(ChargingVoltageTarget));
          N2kMsg.Add2ByteUInt(rvcAmpsU16(uTargetAmps));
          N2kMsg.AddByte(rvcPctU8(pctMax));
          N2kMsg.AddByte(lastRvcState);
          // b7: default state on power-up (OnOff persists, so it is the same bit), auto-recharge
          // enabled (we re-enter bulk on our own), force-charge never commanded by us.
          N2kMsg.AddByte((unsigned char)(((OnOff == 1) ? 0x01 : 0x00) | 0x04));
          composed = true;
        }
        break;
      case SLOT_RVC_CHGR2:  // CHARGER_STATUS_2 1FEA3h — what we are actually MEASURING
        if (rvcChgrEnable == 1) {
          rvcBeginMsg(N2kMsg, 0x1FEA3UL);
          N2kMsg.AddByte((unsigned char)rvcChgrInstance);
          N2kMsg.AddByte((unsigned char)rvcDcInstance);
          N2kMsg.AddByte((unsigned char)rvcDevPriority);
          N2kMsg.Add2ByteUInt(rvcVoltsU16(BatteryV));      // measured at the charger = alternator side
          N2kMsg.Add2ByteUInt(rvcAmpsU16(MeasuredAmps));   // delivered by the charger
          N2kMsg.AddByte(rvcTempU8(rvcAltTempC()));        // the charging element's own temperature
          composed = true;
        }
        break;
      case SLOT_RVC_CHGR3:  // CHARGER_STATUS_3 1FDCAh — derating state, the only standard limit-reason carrier
        if (rvcChgrEnable == 1) {
          uint8_t derating = 0, reason = 0;
          rvcDeratingFromLimiter(derating, reason);
          rvcBeginMsg(N2kMsg, 0x1FDCAUL);
          N2kMsg.AddByte((unsigned char)rvcChgrInstance);
          N2kMsg.AddByte((unsigned char)(derating | 0xFC));  // bits 2-7 undefined -> leave at 1s
          N2kMsg.AddByte(reason);
          for (uint8_t i = 0; i < 5; i++) N2kMsg.AddByte(0xFF);  // bytes 3-7 undefined by the spec
          composed = true;
        }
        break;
      case SLOT_RVC_CHGRCFG:  // CHARGER_CONFIGURATION_STATUS 1FFC6h
        if (rvcChgrEnable == 1) {
          uint8_t bt = 0x0F;  // 4-bit field, all 1s = unknown
          if (strcasecmp(BATTERY_TYPE, "lead_acid") == 0) bt = 0;
          else if (strcasecmp(BATTERY_TYPE, "gel") == 0) bt = 1;
          else if (strcasecmp(BATTERY_TYPE, "agm") == 0) bt = 2;
          else if (strcasecmp(BATTERY_TYPE, "lifepo4") == 0) bt = 3;
          rvcBeginMsg(N2kMsg, 0x1FFC6UL);
          N2kMsg.AddByte((unsigned char)rvcChgrInstance);
          N2kMsg.AddByte(2);  // charging algorithm: 3-stage (matches the N2K N2kCA_3State we publish)
          N2kMsg.AddByte((unsigned char)constrain(n2kChgrMode, 0, 2));  // 0 stand-alone / 1 primary / 2 secondary — same label setting both stacks use
          // b3: no battery temperature sensor in use (00b); AC installation line not applicable to
          // an alternator (11b = no value); battery type in the upper nibble.
          N2kMsg.AddByte((unsigned char)(0x00 | (0x03 << 2) | (bt << 4)));
          N2kMsg.Add2ByteUInt((uint16_t)constrain(BatteryCapacity_Ah, 0, 65530));
          N2kMsg.Add2ByteUInt(rvcAmpsU16((float)AlternatorNominalAmps));
          composed = true;
        }
        break;
      case SLOT_RVC_DC1:  // DC_SOURCE_STATUS_1 1FFFDh — the alternator as its own DC source
        if (rvcDcEnable == 1) {
          rvcBeginMsg(N2kMsg, 0x1FFFDUL);
          N2kMsg.AddByte((unsigned char)rvcDcInstance);
          N2kMsg.AddByte((unsigned char)rvcDevPriority);
          N2kMsg.Add2ByteUInt(rvcVoltsU16(BatteryV));
          // Spec 6.5.2b: positive = current flowing FROM the source, so alternator output is
          // positive as measured. No sign flip (that would only apply to a battery instance).
          N2kMsg.Add4ByteUInt(rvcAmpsU32(MeasuredAmps));
          composed = true;
        }
        break;
      case SLOT_RVC_DC2:  // DC_SOURCE_STATUS_2 1FFFCh
        if (rvcDcEnable == 1) {
          rvcBeginMsg(N2kMsg, 0x1FFFCUL);
          N2kMsg.AddByte((unsigned char)rvcDcInstance);
          N2kMsg.AddByte((unsigned char)rvcDevPriority);
          N2kMsg.Add2ByteUInt(rvcTempU16(rvcAltTempC()));
          N2kMsg.AddByte(0xFF);        // state of charge: meaningless for an alternator
          N2kMsg.Add2ByteUInt(0xFFFF); // time remaining: likewise
          N2kMsg.AddByte(0xFF);        // time-remaining interpretation: no value
          composed = true;
        }
        break;
      case SLOT_RVC_DC3:  // DC_SOURCE_STATUS_3 1FFFBh
        if (rvcDcEnable == 1) {
          rvcBeginMsg(N2kMsg, 0x1FFFBUL);
          N2kMsg.AddByte((unsigned char)rvcDcInstance);
          N2kMsg.AddByte((unsigned char)rvcDevPriority);
          N2kMsg.AddByte(0xFF);         // state of health: a battery datum
          N2kMsg.Add2ByteUInt(0xFFFF);  // capacity remaining: a battery datum
          N2kMsg.AddByte(0xFF);         // relative capacity: a battery datum
          // AC RMS ripple: not-available BY MEASUREMENT, not by omission. Built and removed
          // 2026-08-28 — the INA228 bus channel cannot carry this field. See CAN_UPGRADES.md
          // item 2 for the numbers; the short version is that the INA's own 4x540us averaging
          // puts a healthy rectifier ripple through at ~0.11 with speed-dependent nulls, while
          // normal control-loop and charging activity puts 15-70mV into the same number, so a
          // failing diode reads no higher than a healthy alternator mid-ramp. Needs a fast
          // AC-coupled voltage tap on an ESP32 ADC1 channel — a board revision.
          N2kMsg.Add2ByteUInt(0xFFFF);
          composed = true;
        }
        break;
      case SLOT_RVC_DM:  // DM_RV 1FECAh — operating status + the one active fault
        if (rvcFaultEnable == 1) {
          uint8_t msb = 0, lsb = 0, fmi = 31;  // 31 = failure mode not available
          bool red = false;
          bool faulted = rvcActiveFault(msb, lsb, fmi, red);
          nextDue[s] = now + (faulted ? 1000UL : 5000UL);  // spec 3.2.5.1a: faster while a fault stands
          uint8_t op = 0;  // 00b OFF + 00b standby = disabled and not operating
          if (OnOff == 1) op = gpio4IsLow ? 0x01 : 0x05;  // ON+standby (waiting) vs ON+active
          if (faulted) op |= red ? 0x40 : 0x10;           // bits 6-7 red lamp, bits 4-5 yellow lamp
          rvcBeginMsg(N2kMsg, 0x1FECAUL);
          N2kMsg.AddByte(op);
          N2kMsg.AddByte(76);   // DSA: Charge Controller (spec Table 6.2.6c) — see RVC_TRANSMIT_SPEC.md §5
          N2kMsg.AddByte(msb);  // SPN MSB
          N2kMsg.AddByte((unsigned char)(msb ? rvcChgrInstance : 0));  // ISB = instance for node-specific SPNs, 0 for standard ones
          N2kMsg.AddByte((unsigned char)((lsb << 5) | (fmi & 0x1F)));
          N2kMsg.AddByte(0xFF);  // occurrence count not available (7Fh) + reserved bit 7 always 1
          N2kMsg.AddByte(0xFF);  // no DSA extension defined for this product
          N2kMsg.AddByte(0xFF);  // bank select not applicable
          composed = true;
        }
        break;
    }
    if (composed) {
      if (NMEA2000.SendMsg(N2kMsg)) n2kTxCount++;
      else n2kTxDropCount++;
      rr = (uint8_t)((s + 1) % SLOT_N);
      break;  // one PGN per pass — the TX cost never stacks inside a control tick
    }
    // disabled or skipped slot: interval already advanced, keep scanning so it can't starve the others
  }
}

void ReadVEData() {
  if (VeData != 1) {
    return;
  }
  static unsigned long lastVEDataRead = 0;
  static unsigned long lastSolarEnergyUpdate = 0;
  const unsigned long VE_DATA_INTERVAL = 2000;

  unsigned long currentTime = millis();

  if (currentTime - lastVEDataRead <= VE_DATA_INTERVAL) {
    return;
  }

  int start1 = micros();
  bool dataReceived = false;
  float solarPower_W = 0.0f;

  // Drain budget must exceed worst-case arrival between 2 s ticks (BMV ~900 B/s -> ~1800 B)
  // or the stream starves and most frames fail checksum. Cheap now: the field-extraction
  // loop below runs once per tick, not per byte.
  int veDrainBudget = 2048;
  uint32_t veFramesBefore = myve.frameCounter;
  while (veDrainBudget-- > 0 && Serial1.available()) {
    myve.rxData(Serial1.read());  // corrupted-frame frameIndex clamp lives in the _xeng fork now (textRxEvent)
    yield();
  }
  // Only read the public table when a NEW checksum-valid frame landed this tick: the table
  // persists stale values forever, so unconditional MARK_FRESH here defeated IS_STALE (and
  // integrated phantom solar power) whenever bytes arrived but no frame validated.
  if (myve.frameCounter != veFramesBefore) {
    for (int i = 0; i < myve.veEnd; i++) {
      if (strcmp(myve.veName[i], "V") == 0) {
        float newVoltage = (atof(myve.veValue[i]) / 1000);
        if (newVoltage > 0 && newVoltage < 100) {  // Sanity check
          VictronVoltage = newVoltage;
          MARK_FRESH(IDX_VICTRON_VOLTAGE);
          dataReceived = true;
        }
      }
      if (strcmp(myve.veName[i], "I") == 0) {
        float newCurrent = (atof(myve.veValue[i]) / 1000);
        if (newCurrent > -1000 && newCurrent < 1000) {  // Sanity check
          VictronCurrent = newCurrent;
          MARK_FRESH(IDX_VICTRON_CURRENT);
          dataReceived = true;
        }
      }
      if (strcmp(myve.veName[i], "PPV") == 0) {
        solarPower_W = atof(myve.veValue[i]);             // PPV is already in Watts
        if (solarPower_W >= 0 && solarPower_W < 10000) {  // Sanity check (0-10kW)
          dataReceived = true;
          VictronSolarPower_W = solarPower_W;             // expose live for dashboard + leaderboard
          MARK_FRESH(IDX_VICTRON_SOLAR);
          if (solarPower_W > solar_power_max_alltime_w) solar_power_max_alltime_w = solarPower_W;  // leaderboard: peak solar power
        } else {
          solarPower_W = 0.0f;
        }
      }
      if (strcmp(myve.veName[i], "VPV") == 0) {
        float vpv = atof(myve.veValue[i]) / 1000.0f;      // VPV is in mV -> V
        if (vpv >= 0 && vpv < 400) {                      // Sanity check (0-400V panel array)
          VictronSolarVoltage_V = vpv;
          dataReceived = true;
        }
      }
      // MPPT status + daily-yield fields (all free from the same VE.Direct frame)
      if (strcmp(myve.veName[i], "CS") == 0)   { VictronChargeState = atoi(myve.veValue[i]); dataReceived = true; }
      if (strcmp(myve.veName[i], "MPPT") == 0) { VictronMPPTMode    = atoi(myve.veValue[i]); dataReceived = true; }
      if (strcmp(myve.veName[i], "ERR") == 0)  { VictronError       = atoi(myve.veValue[i]); dataReceived = true; }
      if (strcmp(myve.veName[i], "H20") == 0)  { VictronYieldToday_kWh     = atof(myve.veValue[i]) / 100.0f; dataReceived = true; }  // 0.01 kWh units
      if (strcmp(myve.veName[i], "H21") == 0)  { VictronMaxPowerToday_W    = atof(myve.veValue[i]);          dataReceived = true; }
      if (strcmp(myve.veName[i], "H22") == 0)  { VictronYieldYesterday_kWh = atof(myve.veValue[i]) / 100.0f; dataReceived = true; }  // 0.01 kWh units
      if (strcmp(myve.veName[i], "H23") == 0)  { VictronMaxPowerYesterday_W = atof(myve.veValue[i]);         dataReceived = true; }
      // Battery temperature (degC), present only on a BMV / SmartShunt with a temperature sensor -> batteryTempF() source 3
      if (strcmp(myve.veName[i], "T") == 0) {
        float tC = atof(myve.veValue[i]);
        if (tC >= -40.0f && tC <= 100.0f) {
          VictronBattTempF = tC * 1.8f + 32.0f;
          MARK_FRESH(IDX_VE_BATT_TEMP);
        }
      }
    }
  }

  // Panel current derived from power / voltage (VE.Direct MPPTs report PPV + VPV, not panel A)
  VictronSolarCurrent_A = (VictronSolarVoltage_V > 1.0f) ? (VictronSolarPower_W / VictronSolarVoltage_V) : 0.0f;

  if (dataReceived && lastSolarEnergyUpdate > 0) {
    unsigned long elapsedMillis = currentTime - lastSolarEnergyUpdate;
    float elapsedSeconds = elapsedMillis / 1000.0f;
    float solarEnergyDelta_Wh = (solarPower_W * elapsedSeconds) / 3600.0f;

    static float solarEnergyAccumulator = 0.0f;
    static float solarEnergyAccumulator_AllTime = 0.0f;

    solarEnergyAccumulator += solarEnergyDelta_Wh;
    solarEnergyAccumulator_AllTime += solarEnergyDelta_Wh;

    if (solarEnergyAccumulator >= 1.0f) {
      SolarChargedEnergy += (int)solarEnergyAccumulator;
      solarEnergyAccumulator -= (int)solarEnergyAccumulator;
    }

    if (solarEnergyAccumulator_AllTime >= 1.0f) {
      SolarChargedEnergy_AllTime += (int)solarEnergyAccumulator_AllTime;
      solarEnergyAccumulator_AllTime -= (int)solarEnergyAccumulator_AllTime;
    }
  }

  lastSolarEnergyUpdate = currentTime;

  int end1 = micros();
  VeTime = end1 - start1;
  lastVEDataRead = currentTime;
}

// ── NMEA 0183 receive ────────────────────────────────────────────────────────
// Serial2 on GPIO6, its own opto channel and its own connector pin (RJ3 p4) — nothing else on the
// board or in firmware touches either, so this port is free to run at whatever baud/polarity the
// attached talker wants. Receive only: no TX pin exists and the front end is one-way.
//
// Heading is decoded as a worked example. It is deliberately kept OUT of HeadingNMEA and
// IDX_HEADING_NMEA: those belong to the NMEA2000 receiver, and silently merging two sources would
// make "which one am I looking at" unanswerable. Anything that wants to consume 0183 heading for
// real should become an explicit user-selected source, never an automatic fallback.

// Heading back to absent. -1 is the "nothing decoded" sentinel CSV4_n183Heading already ships,
// so the client needs no new field to tell a dead talker from a boat pointing north.
void n183ClearHeading() {
  n183HeadingDeg = -1.0f;
  n183HdgRef = 0;
  dataTimestamps[IDX_N183_HDG] = 0;
}

void applyNMEA0183Serial() {
  n183ClearHeading();  // the port is about to change under it; a heading decoded at the old baud says nothing about the new one
  Serial2.end();
  Serial2.setRxBufferSize(2048);  // must precede begin(); 19200 fills the 256 B default in ~130 ms
  Serial2.begin(NMEA0183Baud, SERIAL_8N1, 6, -1, NMEA0183Invert == 1);
  while (Serial2.available()) Serial2.read();  // drop the partial line left by the old baud. flush() is TX-side only and Serial2 has no TX pin.
}

// One checksum-valid sentence, NUL-terminated, '$'/'!' and CRLF already stripped.
static void n183HandleSentence(char *s) {
  n183SentenceCount++;
  MARK_FRESH(IDX_N183);

  // Address field is <talker><type>, e.g. "HCHDT" — the talker prefix varies by device, the
  // 3-char type does not, so match on the type only.
  if (strlen(s) < 6 || s[5] != ',') return;
  const char *type = s + 2;
  uint8_t ref = 0;
  if (!strncmp(type, "HDT", 3) || !strncmp(type, "THS", 3)) ref = 2;       // true heading
  else if (!strncmp(type, "HDM", 3)) ref = 1;                              // magnetic heading
  else if (!strncmp(type, "HDG", 3)) ref = 1;                              // magnetic; deviation/variation fields ignored, so never call it true
  else return;

  char *deg = s + 6;
  char *end = strchr(deg, ',');
  if (end) *end = '\0';
  if (*deg == '\0') return;  // null field: talker is alive but has no fix/heading yet

  // THS field 2 is a mode indicator: only 'A' (autonomous) and 'D' (differential) are usable data.
  if (!strncmp(type, "THS", 3) && end) {
    char mode = end[1];
    if (mode != 'A' && mode != 'D') return;
  }

  float h = atof(deg);
  if (h < 0.0f || h > 360.0f) return;
  n183HeadingDeg = (h == 360.0f) ? 0.0f : h;
  n183HdgRef = ref;
  MARK_FRESH(IDX_N183_HDG);
}

// Assemble one byte into the sentence buffer. Returns nothing; commits through n183HandleSentence
// on a line ending. Pure RAM work — no UART access, so the caller controls all driver-lock cost.
static char n183Line[96];   // longest standard sentence is 82 B; anything longer is noise
static uint8_t n183Len = 0;
static bool n183Overrun = false;

static void n183Feed(char c) {
  if (c == '$' || c == '!') {         // start of sentence: abandon whatever came before
    n183Len = 0;
    n183Overrun = false;
    return;
  }
  if (c != '\r' && c != '\n') {
    if (n183Len < sizeof(n183Line) - 1) n183Line[n183Len++] = c;
    else n183Overrun = true;
    return;
  }
  if (n183Len == 0) return;           // bare line ending, nothing buffered
  n183Line[n183Len] = '\0';
  uint8_t completed = n183Len;
  n183Len = 0;
  if (n183Overrun) { n183Overrun = false; n183ChecksumErrCount++; return; }

  // Trailing "*HH" is the XOR of everything between '$' and '*'. Sentences without one are rare in
  // practice and unverifiable, so they are counted as errors rather than trusted.
  char *star = strrchr(n183Line, '*');
  if (!star || star != n183Line + completed - 3) { n183ChecksumErrCount++; return; }
  *star = '\0';
  uint8_t sum = 0;
  for (char *p = n183Line; *p; p++) sum ^= (uint8_t)*p;
  if (sum != (uint8_t)strtol(star + 1, nullptr, 16)) { n183ChecksumErrCount++; return; }

  n183HandleSentence(n183Line);
}

// HARD REAL-TIME CONTRACT: this must never hold the control loop for more than 500 us.
// Four independent limits enforce it, so no single estimate has to be right:
//   1. A per-tick byte cap. 512 B is the most work this can ever be asked to do in one pass.
//   2. A micros() deadline checked after every 64 B chunk, so the overrun exposure is one chunk,
//      not one tick. Whatever is not consumed stays in the 2 KB driver ring and is picked up next
//      tick — backpressure costs latency, never bytes.
//   3. Block reads. Serial2.read(buf, n) takes the UART lock ONCE per chunk and memcpys, with
//      timeout_ms = 0 so it never waits. read() byte-at-a-time pays that lock ~2 us PER BYTE and
//      blew the whole budget on its own; readBytes() would be worse still — it blocks for
//      getTimeout(), a full second by default. Neither may be used here.
//   4. Parsing runs over the RAM chunk, not the UART, at roughly 20 ns/byte.
// Throughput check: 512 B every 50 ms = 10 kB/s, against 3840 B/s arriving at the fastest
// supported baud (38400). Nearly 3x margin, and the 2 KB ring absorbs a half second of burst.
// The worst cost actually observed is published as ft_ReadNMEA0183 in the Function Timing table,
// and the firmware complains to the console on its own if it ever crosses the budget.
#define N183_DEADLINE_US   250   // checked between chunks; leaves room for one more chunk under the 500 us budget
#define N183_TICK_MS       50
#define N183_MAX_PER_TICK  512
#define N183_BUDGET_US     500   // the promise being policed, not a control value

void ReadNMEA0183Data() {
  if (NMEA0183Data != 1) return;

  static unsigned long lastRead = 0;
  unsigned long nowMs = millis();
  if (nowMs - lastRead < N183_TICK_MS) return;
  lastRead = nowMs;

  const uint32_t t0 = micros();
  // A talker that goes quiet must stop reading as live. Same 10s DATA_TIMEOUT the NMEA2000
  // heading ages out on; IDX_N183 can't serve here because it stays fresh on a GPS-only talker.
  // Inside the t0 window on purpose: everything this function does is policed by N183_BUDGET_US.
  if (n183HdgRef != 0 && IS_STALE(IDX_N183_HDG)) n183ClearHeading();
  uint8_t buf[64];
  uint16_t total = 0;

  while (total < N183_MAX_PER_TICK) {
    size_t got = Serial2.read(buf, sizeof(buf));   // non-blocking: returns what is already buffered
    if (got == 0) break;
    total += got;
    for (size_t i = 0; i < got; i++) n183Feed((char)buf[i]);
    if ((uint32_t)(micros() - t0) >= N183_DEADLINE_US) break;
  }

  // Self-policing. If the four limits above ever fail to hold the promise, say so out loud once a
  // minute rather than letting it hide in a stats row nobody is looking at.
  uint32_t spent = micros() - t0;
  if (spent > N183_BUDGET_US) {
    static unsigned long lastGripe = 0;
    if (nowMs - lastGripe > 60000UL) {
      lastGripe = nowMs;
      queueConsoleMessageF("WARNING: NMEA 0183 drain took %luus (budget %dus) - report this", (unsigned long)spent, N183_BUDGET_US);
    }
  }
}
// Maintenance reboot engine. Tier constants + full ladder description live with
// REBOOT_OPPORTUNISTIC_MS in Xregulator.ino. Runs every loop pass.
void checkAndRestart() {
  unsigned long up = millis();  // device boots at millis()=0, so uptime IS millis(); the 72 h cap keeps us far from the 49.7-day wrap

  // Health requester: LargestInternalBlock (KB, sampled ~4 s by the heap sampler) below the
  // TLS-death floor continuously for 30 min. Brief HTTPS-handshake dips recover in seconds and
  // reset the timer, so this only latches when the heap is genuinely fragmented.
  static unsigned long heapLowSince = 0;
  if (LargestInternalBlock > 0 && LargestInternalBlock < (size_t)REBOOT_HEAP_FLOOR_KB) {
    if (heapLowSince == 0) heapLowSince = up;
    if (!maintRebootHealthReq && up - heapLowSince >= REBOOT_HEAP_LOW_HOLD_MS) {
      maintRebootHealthReq = true;
      queueConsoleMessage("Maintenance: contiguous RAM low for 30 min - reboot at next quiet window");
    }
  } else {
    heapLowSince = 0;
  }

  static unsigned long quietSince = 0;      // continuous-hold start of the current quiet window
  static unsigned long countdownStart = 0;  // 10-min warning start (relaxed/hard tiers only)

  bool wantReboot = maintRebootHealthReq || up >= REBOOT_OPPORTUNISTIC_MS;
  if (!wantReboot) {
    restartRemainingSec = 0;
    return;
  }

  // Absolute holds — no tier reboots through an OTA install or a running guided test
  // (g_autoTestActive covers commissioning + battery-health + resonance + system-ID + field-cut
  // + CV stress + protection tests; each is bounded by its own timeout).
  if (otaInProgress || g_autoTestActive) {
    quietSince = 0;
    countdownStart = 0;
    restartRemainingSec = 0;
    return;
  }

  float vScale = SYSTEM_VOLTAGE_CLASS / 12.0f;
  bool vValid = (IBV > 5.0f);  // a dead/absent INA228 reads ~0 V; sensor-dead must not block the ladder forever
  bool engineOff = !engineSpinning();
  bool fieldOff = (fieldActiveStatus == 0);
  bool noClient = (events.count() == 0);
  bool cloudReachable = (currentMode == MODE_CLIENT && WiFi.status() == WL_CONNECTED && isRegistered);
  // Offline there is nothing to wait for — the flush below dumps the un-uploaded ring to flash.
  bool cloudIdle = !core0Busy && (bufferedRecordCount == 0 || !cloudReachable);
  bool socOk = (!socInfoAvailable || SOC_percent >= REBOOT_SOC_FLOOR_X100);
  bool vQuietOk = (!vValid || IBV >= REBOOT_VBAT_FLOOR_V * vScale);
  bool vBrownOk = (!vValid || IBV >= REBOOT_VBAT_BROWNOUT_V * vScale);

  bool hardTier = (up >= REBOOT_HARD_MS);
  bool relaxedTier = (up >= REBOOT_RELAXED_MS);

  bool windowOk;
  if (hardTier) {
    windowOk = vBrownOk;  // engine/clients no longer defer; brown-out risk is the only battery hold
  } else if (relaxedTier) {
    windowOk = engineOff && fieldOff && cloudIdle && vQuietOk;
  } else {
    windowOk = engineOff && fieldOff && noClient && cloudIdle && socOk && vQuietOk;
  }

  if (!windowOk) {
    quietSince = 0;
    countdownStart = 0;
    restartRemainingSec = 0;
    return;
  }

  // Quiet/relaxed tiers: the window must hold 5 min continuously (lets the field-off upload
  // burst fire at 60-75 s and drain before we commit). Hard tier skips the hold — its 10-min
  // countdown is the notice period.
  if (!hardTier) {
    if (quietSince == 0) quietSince = up;
    if (up - quietSince < REBOOT_QUIET_HOLD_MS) {
      restartRemainingSec = 0;
      return;
    }
  }

  // Countdown only when someone could be watching: always in the hard tier, and in the relaxed
  // tier when a client is connected. Quiet-tier reboots are silent — a client connecting
  // mid-window breaks noClient and aborts above. Window conditions keep being enforced during
  // the countdown, so an engine start or brown-out mid-countdown aborts it.
  if (hardTier || (relaxedTier && !noClient)) {
    if (countdownStart == 0) {
      countdownStart = up;
      queueConsoleMessage("Maintenance reboot in 10 minutes");
    }
    unsigned long cdElapsed = up - countdownStart;
    if (cdElapsed < RESTART_WARNING_WINDOW_MS) {
      uint32_t sec = (uint32_t)((RESTART_WARNING_WINDOW_MS - cdElapsed) / 1000UL);
      restartRemainingSec = (sec == 0) ? 1 : sec;
      return;
    }
  }

  {
    restartRemainingSec = 1;  // keep banner visible through the shutdown sequence
    Serial.println("=== MAINTENANCE RESTART ===");

    // Graceful field-down: zero PWM immediately and gate the PID via OnOff=0.
    // OnOff is RAM-only here — LittleFS is not touched, so on reboot the device
    // returns to whatever OnOff state the user last set.
    Serial.println("Restart: hard-zero PWM and disable charging");
    apply_pwm_float(0.0f);
    dutyCycle = 0.0f;
    lastAppliedDuty = 0.0f;
    OnOff = 0;
    delay(500);            // brief settle so any in-flight tick sees OnOff=0
    esp_task_wdt_reset();

    // NVS drain: persist counters/extrema/lifetime values before reboot.
    Serial.println("Restart: flushing NVS");
    saveNVSDataFull();
    esp_task_wdt_reset();

    // Wait for any in-progress uploads (max 30 seconds)
    unsigned long shutdownStart = millis();
    int waitCycles = 0;

    while (core0Busy && (millis() - shutdownStart < 30000)) {
      esp_task_wdt_reset();
      delay(500);
      waitCycles++;

      if (waitCycles % 10 == 0) {
        Serial.printf("Waiting for upload to complete... %ds\n",
                      (waitCycles * 500) / 1000);
      }
    }

    if (core0Busy) {
      Serial.println("Upload still in progress after 30s, proceeding with restart");
      core0Busy = false;
    }

    // Wait for HTTP operations to fully complete
    delay(1000);
    esp_task_wdt_reset();

    // Save final sensor window if it has data — push into PSRAM ring so the
    // shutdown Phase 4 dump (or next-tick cloud upload) preserves it.
    if (currentWindow->battVolt_valid_us > 0) {
      Serial.println("Pushing final sensor window into ring before restart");
      time_t collectionTime = computeCollectionTime();
      pushSensorSnapshot(collectionTime);
      resetSensorWindow();
    }

    // Un-uploaded telemetry (offline/AP case — cloudIdle waives the drain when the cloud is
    // unreachable): bulk-dump the PSRAM ring to flash, same as shutdown Phase 4; setup()
    // restores and deletes the file on next boot.
    if (!ringIsEmpty()) {
      Serial.printf("Maintenance restart: ring still has %u records - dumping to LittleFS\n",
                    (unsigned)sensorRingCount);
      dumpSensorRingToLittleFS();
    }

    // Persist the long-term plot ring so the maintenance restart doesn't shed up to 15 min of
    // unflushed records (the periodic flush only fires every LONGTERM_DUMP_INTERVAL_MS).
    dumpLongTermRing();
    dumpUsageAccum();  // app-usage period counters span the restart via /usage.bin
    solarLedgerFlushNow();  // ledger + live day + learned ratio span the restart too

    if (WiFi.getMode() != WIFI_OFF) {
      // Console event only — the client has no "status" listener, so that event was composed and dropped
      events.send("Performing maintenance restart", "console");
      delay(500);             // Give events time to actually send
      events.close();         // Close all SSE connections
      delay(100);             // Let close complete
      WiFi.disconnect(true);
      delay(100);
    }

    Serial.println("=== RESTARTING NOW ===");
    Serial.flush();
    delay(100);
    writeFile(LittleFS, "/ScheduledRestart.flag", "1");
    esp_task_wdt_delete(NULL);
    delay(100);
    ESP.restart();
  }
}
void captureResetReason() {
  String fileContent = settingRead(NK_LastResetReason);

  // Parse "last,ancient" format
  int commaPos = fileContent.indexOf(',');
  int previousLast = 0;
  int previousAncient = 0;

  if (commaPos > 0) {
    previousLast = fileContent.substring(0, commaPos).toInt();
    previousAncient = fileContent.substring(commaPos + 1).toInt();
  } else {
    previousLast = fileContent.toInt();  // Legacy: single value
    previousAncient = 0;
  }

  // Shift: previous last becomes new ancient
  ancientResetReason = previousLast;

  bool wasScheduled = false;
  if (fsExists("/ScheduledRestart.flag")) {
    String flagContent = readFile(LittleFS, "/ScheduledRestart.flag");
    if (flagContent == "1") {
      wasScheduled = true;
    }
    fsRemove("/ScheduledRestart.flag");
  }

  int rawReason = (int)esp_reset_reason();
  switch (rawReason) {
    case ESP_RST_POWERON: LastResetReason = 0; break;
    case ESP_RST_SW:
      LastResetReason = wasScheduled ? 11 : 1;  // 11=scheduled, 1=unscheduled
      break;
    case ESP_RST_DEEPSLEEP: LastResetReason = 2; break;
    case ESP_RST_EXT: LastResetReason = 3; break;
    case ESP_RST_TASK_WDT: LastResetReason = 4; break;
    case ESP_RST_PANIC: LastResetReason = 5; break;
    case ESP_RST_BROWNOUT: LastResetReason = 6; break;
    case ESP_RST_INT_WDT: LastResetReason = 7; break;
    case ESP_RST_WDT: LastResetReason = 9; break;
    case ESP_RST_SDIO: LastResetReason = 10; break;
    case ESP_RST_PWR_GLITCH: LastResetReason = 12; break;
    case ESP_RST_CPU_LOCKUP: LastResetReason = 13; break;
    case ESP_RST_USB: LastResetReason = 14; break;
    case ESP_RST_JTAG: LastResetReason = 15; break;
    case ESP_RST_EFUSE: LastResetReason = 16; break;
    default: LastResetReason = 8; break;
  }

  // Raw codes for the BOOTED console line: the ROM per-CPU causes carry detail the esp-level
  // enum collapses (S3 ROM codes of interest: 15=RTCWDT_BROWN_OUT 18=SUPER_WDT 19=GLITCH_RTC
  // 23=POWER_GLITCH — full list in rom/rtc.h RESET_REASON).
  g_rawResetEsp = rawReason;
  g_rawResetRtc0 = (int)rtc_get_reset_reason(0);
  g_rawResetRtc1 = (int)rtc_get_reset_reason(1);

  // Black box: capture last session's snapshot before loop() starts overwriting it, then
  // invalidate so a reset before the first loop pass can't replay this session's copy.
  memcpy(&g_blackBoxPrev, &g_blackBox, sizeof(g_blackBoxPrev));
  g_blackBoxPrevValid = (g_blackBoxPrev.magic == BLACKBOX_MAGIC);
  g_blackBox.magic = 0;

  // Lifetime OV telemetry: bad magic = true power-down or a firmware layout change — start clean
  // (no migration; an old bin layout is not comparable). Valid magic carries the history through.
  if (g_ovTel.magic != OVTEL_MAGIC) {
    memset(&g_ovTel, 0, sizeof(g_ovTel));
    g_ovTel.magic = OVTEL_MAGIC;
  }

  Serial.printf("RESET: %s | esp=%d rtc0=%d rtc1=%d\n",
                resetReasonName(), g_rawResetEsp, g_rawResetRtc0, g_rawResetRtc1);
  if (g_blackBoxPrevValid) {
    Serial.printf("BLACKBOX: up=%lus IBV=%.2fV duty=%.1f%% RPM=%d amps=%.1f altT=%dF mode=%u stage=%u loop=%.1fms heap=%ldKB\n",
                  (unsigned long)(g_blackBoxPrev.upMillis / 1000UL), g_blackBoxPrev.ibv,
                  g_blackBoxPrev.duty, (int)g_blackBoxPrev.rpm, g_blackBoxPrev.measAmps,
                  (int)g_blackBoxPrev.altTempF, g_blackBoxPrev.sysMode, g_blackBoxPrev.chargeStage,
                  g_blackBoxPrev.maxLoopUs / 1000.0f, (long)g_blackBoxPrev.minHeapKB);
  } else {
    Serial.println("BLACKBOX: RTC RAM lost - true power interruption preceded this boot");
  }

  // Save both values: "current,ancient"
  String saveStr = String(LastResetReason) + "," + String(ancientResetReason);
  settingWrite(NK_LastResetReason, saveStr.c_str());
}
void UpdateEngineFuel(unsigned long elapsedMillis) {
  // Only calculate fuel consumption if engine is running. (High-RPM glitch reject is below, scaled
  // to the configured engine — a fixed 6000 ceiling would wrongly cut off high-revving engines.)
  if (RPM <= 0) {
    currentFuelGPH = 0.0f;  // engine off -> no live flow / economy
    currentNMPG = 0.0f;
    fcRun = false;          // break the steady-state run for the session fuel curve
    return;
  }

  float elapsedHours = elapsedMillis / 3600000.0f;

  // Find the number of valid entries (allow first entry to be zero for idle)
  int validEntries = 0;
  for (int i = 0; i < FUEL_TABLE_SIZE; i++) {
    if (fuelTableRPM[i] > 0 || i == 0) {  // Allow zero in first row
      validEntries = i + 1;
    } else {
      break;  // Stop at first zero (after row 0)
    }
  }

  if (validEntries == 0) {
    currentFuelGPH = 0.0f;  // no table configured -> no live flow / economy
    currentNMPG = 0.0f;
    fcRun = false;
    return;
  }

  // Engine RPM range = the configured fuel table. Top breakpoint defines the curve's bin scale and
  // a glitch ceiling that scales with the engine (no fixed 6000 cap, so high-rev engines aren't cut off).
  float fuelTopRPM = fuelTableRPM[validEntries - 1];
  if (fuelTopRPM < 1.0f) fuelTopRPM = 4500.0f;  // guard a degenerate table
  currentFuelTopRPM = fuelTopRPM;               // published for the chart x-axis
  if (RPM >= fuelTopRPM * 1.5f) {               // 50% over redline -> treat as sensor glitch
    currentFuelGPH = 0.0f;
    currentNMPG = 0.0f;
    fcRun = false;
    return;
  }

  // Interpolate fuel consumption based on current RPM
  float fuelRate_GPH = 0.0f;

  if (RPM <= fuelTableRPM[0]) {
    fuelRate_GPH = fuelTableGPH[0];
  } else if (RPM >= fuelTableRPM[validEntries - 1]) {
    fuelRate_GPH = fuelTableGPH[validEntries - 1];
  } else {
    for (int i = 0; i < validEntries - 1; i++) {
      if (RPM >= fuelTableRPM[i] && RPM < fuelTableRPM[i + 1]) {
        float rpmRange = fuelTableRPM[i + 1] - fuelTableRPM[i];
        float fuelRange = fuelTableGPH[i + 1] - fuelTableGPH[i];
        float rpmOffset = RPM - fuelTableRPM[i];
        fuelRate_GPH = fuelTableGPH[i] + (rpmOffset / rpmRange) * fuelRange;
        break;
      }
    }
  }

  // Publish live fuel flow + economy for the dashboard (gal/hr, naut mi/gal).
  // Economy = SOG (knots = naut mi/hr) / flow (gal/hr); 0 when not moving or no GPS speed.
  currentFuelGPH = fuelRate_GPH;
  // Stale gate matches the trip logic: SOGNMEA is never zeroed on GPS loss, so without
  // IS_STALE a frozen speed keeps NMPG live-looking and freezes fuel-curve bins with it.
  currentNMPG = (fuelRate_GPH > 0.0f && SOGNMEA > 0.0f && !IS_STALE(IDX_SOG_NMEA)) ? (SOGNMEA / fuelRate_GPH) : 0.0f;

  // Session fuel-economy curve: settle-then-measure. RPM and boat speed must hold within band
  // (max-min ≤ tol on each) continuously; once they have held for fuelCurveSettleSec (the boat has
  // reached true steady speed for that throttle), mpg is averaged over the next fuelCurveSampleSec and
  // that average freezes the bin (overwriting). Then the settle->sample cycle restarts while still
  // steady, so a long cruise refreshes the bin. ANY band break / stop / no-GPS reseeds from scratch.
  if (currentNMPG > 0.0f && SOGNMEA > 0.0f && !IS_STALE(IDX_SOG_NMEA)) {
    uint32_t nowMs = millis();
    float r = RPM, s = SOGNMEA;
    bool inBand = false;
    if (fcRun) {
      float rMin = min(fcRpmMin, r), rMax = max(fcRpmMax, r);
      float sMin = min(fcSpdMin, s), sMax = max(fcSpdMax, s);
      if ((rMax - rMin) <= fuelCurveRpmTol && (sMax - sMin) <= fuelCurveSpdTol) {
        inBand = true;
        fcRpmMin = rMin; fcRpmMax = rMax; fcSpdMin = sMin; fcSpdMax = sMax;  // extend the run
        uint32_t elapsed = nowMs - fcRunStartMs;
        uint32_t settleMs = (uint32_t)(fuelCurveSettleSec * 1000.0f);
        uint32_t sampleMs = (uint32_t)(fuelCurveSampleSec * 1000.0f);
        if (elapsed >= settleMs) {                  // settled -> accumulate the sample window
          fcSampleMpgSum += currentNMPG;
          fcSampleRpmSum += r;
          fcSampleCount++;
        }
        if (elapsed >= settleMs + sampleMs) {       // sample window complete -> freeze the bin
          if (fcSampleCount > 0) {
            float avgMpg = (float)(fcSampleMpgSum / (double)fcSampleCount);
            float avgRpm = (float)(fcSampleRpmSum / (double)fcSampleCount);  // band centroid picks the bin
            float binW = fuelTopRPM / (float)FUELCURVE_BINS;                 // universal: width scales with engine
            int bin = (binW > 0.0f) ? (int)(avgRpm / binW) : -1;
            if (bin >= FUELCURVE_BINS) bin = FUELCURVE_BINS - 1;             // clamp top bin (RPM at/above redline)
            if (bin >= 0 && bin < FUELCURVE_BINS) fuelCurveNMPG[bin] = avgMpg;  // overwrite bin
          }
          // restart the settle->sample cycle, still steady (reseed band + accumulators at this sample)
          fcRunStartMs = nowMs;
          fcRpmMin = fcRpmMax = r; fcSpdMin = fcSpdMax = s;
          fcSampleMpgSum = 0; fcSampleRpmSum = 0; fcSampleCount = 0;
        }
      }
    }
    if (!inBand) {   // first entry, or band broke -> start a fresh settle, reseeded at this sample
      fcRun = true;
      fcRpmMin = fcRpmMax = r; fcSpdMin = fcSpdMax = s; fcRunStartMs = nowMs;
      fcSampleMpgSum = 0; fcSampleRpmSum = 0; fcSampleCount = 0;
    }
  } else {
    fcRun = false;  // stopped or no GPS speed -> break the run
  }

  float fuelConsumed_Gallons = fuelRate_GPH * elapsedHours;
  float fuelConsumed_Liters = fuelConsumed_Gallons * 3.78541;

  static float engineFuelAccumulator = 0.0f;
  static float engineFuelAccumulator_AllTime = 0.0f;

  engineFuelAccumulator += fuelConsumed_Liters;
  engineFuelAccumulator_AllTime += fuelConsumed_Liters;

  if (engineFuelAccumulator >= 0.01f) {
    float toAdd = floor(engineFuelAccumulator * 100.0f) / 100.0f;  // Round down to nearest 0.01 L
    EngineFuelUsed += toAdd;
    engineFuelAccumulator -= toAdd;  // Keep the fractional remainder
  }

  if (engineFuelAccumulator_AllTime >= 0.01f) {
    float toAdd = floor(engineFuelAccumulator_AllTime * 100.0f) / 100.0f;
    EngineFuelUsed_AllTime += toAdd;
    engineFuelAccumulator_AllTime -= toAdd;  // Keep the fractional remainder
  }
}
void UpdateBatterySOC(unsigned long elapsedMillis) {
  float elapsedSeconds = elapsedMillis / 1000.0f;
  // =================================================================
  //     BATTERY STATE MONITORING
  // =================================================================
  // This section tracks battery state of charge from ALL sources
  float currentBatteryVoltage = getBatteryVoltage();
  Voltage_scaled = currentBatteryVoltage * 100;  // V × 100 for precision
  float batteryCurrentForSoC = getBatteryCurrent();  // dedicated battery source, not alternator
  BatteryCurrent_scaled = batteryCurrentForSoC * 100;  // A × 100 for precision
  // positive = charging, negative = discharging
  BatteryPower_scaled = (Voltage_scaled * BatteryCurrent_scaled) / 100;  // W × 100
  float batteryPower_W = BatteryPower_scaled / 100.0f;
  float energyDelta_Wh = (batteryPower_W * elapsedSeconds) / 3600.0f;
  // Battery-side tracking (charge/discharge energy, coulomb/SoC, capacity tracker,
  // full-charge detection) all need a real Bcur — gate on the shunt. The alternator
  // stats and voltage averaging below run on every config.
  if (HAS_BATT_SHUNT) {
    static float chargedEnergyAccumulator = 0.0f;
    static float dischargedEnergyAccumulator = 0.0f;
    static float chargedEnergyAccumulator_AllTime = 0.0f;
    static float dischargedEnergyAccumulator_AllTime = 0.0f;
    if (BatteryCurrent_scaled > 0) {
      chargedEnergyAccumulator += energyDelta_Wh;
      chargedEnergyAccumulator_AllTime += energyDelta_Wh;
      if (chargedEnergyAccumulator >= 1.0f) {
        ChargedEnergy += (int)chargedEnergyAccumulator;
        chargedEnergyAccumulator -= (int)chargedEnergyAccumulator;
      }
      if (chargedEnergyAccumulator_AllTime >= 1.0f) {
        ChargedEnergy_AllTime += (int)chargedEnergyAccumulator_AllTime;
        chargedEnergyAccumulator_AllTime -= (int)chargedEnergyAccumulator_AllTime;
      }
    } else if (BatteryCurrent_scaled < 0) {
      dischargedEnergyAccumulator += abs(energyDelta_Wh);
      dischargedEnergyAccumulator_AllTime += abs(energyDelta_Wh);
      if (dischargedEnergyAccumulator >= 1.0f) {
        DischargedEnergy += (int)dischargedEnergyAccumulator;
        dischargedEnergyAccumulator -= (int)dischargedEnergyAccumulator;
      }
      if (dischargedEnergyAccumulator_AllTime >= 1.0f) {
        DischargedEnergy_AllTime += (int)dischargedEnergyAccumulator_AllTime;
        dischargedEnergyAccumulator_AllTime -= (int)dischargedEnergyAccumulator_AllTime;
      }
    }

    // =================================================================
    //     COULOMB COUNTING - BATTERY STATE OF CHARGE TRACKING
    // =================================================================
    // Track actual amp-hours into/out of battery with efficiency corrections

    static float coulombAccumulator_Ah = 0.0f;
    float batteryCurrent_A = BatteryCurrent_scaled / 100.0f;
    float deltaAh = (batteryCurrent_A * elapsedSeconds) / 3600.0f;
    if (BatteryCurrent_scaled >= 0) {
      // Charging - apply charge efficiency (not all energy makes it in)
      // ChargeEfficiency_scaled is % × 10 (e.g. 990 = 99.0%), so divide by 1000 to get the multiplier.
      float batteryDeltaAh = deltaAh * (ChargeEfficiency_scaled / 1000.0f);
      coulombAccumulator_Ah += batteryDeltaAh;
    } else {
      // Discharging - apply Peukert compensation for high discharge rates
      float dischargeCurrent_A = abs(batteryCurrent_A);
      float peukertThreshold = BatteryCapacity_Ah / 100.0f;  // C/100 threshold

      if (dischargeCurrent_A > peukertThreshold) {
        float peukertExponent = PeukertExponent_scaled / 100.0f;
        dischargeCurrent_A = constrain(dischargeCurrent_A, 0, BatteryCapacity_Ah);  // Max 1C
        // Peukert: effective drain = I*(I/I_rated)^(k-1) — heavier-than-rated discharge must
        // count FASTER (factor > 1). The ratio was inverted here for years.
        float currentRatio = dischargeCurrent_A / PeukertRatedCurrent_A;
        float peukertFactor = pow(currentRatio, peukertExponent - 1.0f);
        peukertFactor = constrain(peukertFactor, 0.5f, 2.0f);  // Sanity limits
        float batteryDeltaAh = deltaAh * peukertFactor;
        coulombAccumulator_Ah += batteryDeltaAh;
      } else {
        coulombAccumulator_Ah += deltaAh;
      }
    }

    if (abs(coulombAccumulator_Ah) >= 0.01f) {
      int deltaAh_scaled = (int)(coulombAccumulator_Ah * 100.0f);
      CoulombCount_Ah_scaled += deltaAh_scaled;
      shadowCoulombX100 += deltaAh_scaled;  // unsaturated twin — no constrain below
      coulombAccumulator_Ah -= (deltaAh_scaled / 100.0f);  // Keep remainder
    }

    int capAhX100 = BatteryCapacity_Ah * 100;
    if (capAhX100 < 1) capAhX100 = 1;  // guard: 0 Ah capacity (bad import / unset NVS) must not divide-by-zero into SOC
    CoulombCount_Ah_scaled = constrain(CoulombCount_Ah_scaled, 0, capAhX100);
    float SoC_float = (float)CoulombCount_Ah_scaled / (float)capAhX100 * 100.0f;
    SOC_percent = (int)(SoC_float * 100);  // Store as percentage × 100 for 2 decimals
    wmIgnUpdate(wmIgn_SOC, SoC_float);     // ignition-cycle watermark (float percent, 0..100)

    // Capacity tracker (OCV-anchored): rest detection, low-OCV anchor capture, Ah bridge.
    // Uses the FILTERED INA228 voltage (getFiltV) — rest/OCV is a slow read where filtering
    // is correct (not a safety path). Board temp °F → °C for the temp coefficient.
    capTrackTick(BatteryCurrent_scaled / 100.0f, getFiltV(),
                 isnan(ambientTemp) ? NAN : (ambientTemp - 32.0f) / 1.8f, elapsedSeconds);

    // =================================================================
    //     FULL CHARGE DETECTION - WORKS FROM ANY CHARGING SOURCE
    // =================================================================
    // When battery voltage is high AND current is low (tail current),
    // we know it's fully charged regardless of what's charging it
    // Units: BatteryCurrent_scaled is A×100. TailCurrent is % of capacity, so the
    // threshold in A×100 is (TailCurrent/100 × Capacity) × 100 = TailCurrent × Capacity.
    if ((abs(BatteryCurrent_scaled) <= (TailCurrent * BatteryCapacity_Ah)) && (Voltage_scaled >= ChargedVoltage_Scaled)) {
      FullChargeTimer += elapsedSeconds;

      if (FullChargeTimer >= ChargedDetectionTime) {
        // Captured BEFORE the reset below — this is the drift evidence the gain correction reads.
        // The SHADOW count is used, not the live one: unclamped, it retains overshoot above capacity,
        // so drift is visible in both directions (the live counter's top clamp hides over-reading).
        int preResetShadowX100 = shadowCoulombX100;
        SOC_percent = 10000;  // 100.00%
        CoulombCount_Ah_scaled = BatteryCapacity_Ah * 100;
        shadowCoulombX100 = BatteryCapacity_Ah * 100;  // full charge = known anchor for both counters

        static unsigned long lastFullChargeMessage = 0;
        if (!FullChargeDetected || millis() - lastFullChargeMessage > 60000) {
          char msg[128];
          queueConsoleMessageF("BATTERY: Full charge detected - SoC reset to 100%% (V=%.2fV >= %.2fV, I=%.2fA, Timer=%.1fs)",
                               Voltage_scaled / 100.0, ChargedVoltage_Scaled / 100.0,
                               BatteryCurrent_scaled / 100.0, FullChargeTimer);
          lastFullChargeMessage = millis();
        }

        // Capacity measurement at the full anchor (once per event, gated by !FullChargeDetected).
        // OCV-anchored — capTrackOnFull validates the low anchor + span and emits a dated point.
        if (!FullChargeDetected) {
          uint32_t nowEpoch = timeIsSynced ? (timeBase + (millis() - timeBaseMillis) / 1000) : 0;
          capTrackOnFull(nowEpoch, isnan(ambientTemp) ? NAN : (ambientTemp - 32.0f) / 1.8f);
          // Shunt gain correction — once per full-charge event (this block, not the every-2s tick),
          // fed the pre-reset SHADOW count. Battery current always comes from INA228.
          if (AutoShuntGainCorrection == 1) {
            applySocGainCorrection(preResetShadowX100);
          }
        }

        FullChargeDetected = true;
        coulombAccumulator_Ah = 0.0f;
      }
    } else {
      FullChargeTimer = 0;
      FullChargeDetected = false;
    }
  }

  // =================================================================
  //     ALTERNATOR-SPECIFIC TRACKING - ONLY RUNS WHEN ALT IS ON
  // =================================================================
  // These metrics are ONLY about the alternator's contribution

  alternatorIsOn = (MeasuredAmps > CurrentThreshold);

  if (alternatorIsOn) {
    alternatorOnAccumulator += elapsedMillis;
    if (alternatorOnAccumulator >= 1000) {
      int secondsRun = alternatorOnAccumulator / 1000;
      AlternatorOnTime += secondsRun;
      AlternatorOnTime_AllTime += secondsRun;
      alternatorOnAccumulator %= 1000;  // Keep remainder milliseconds
    }

    static float alternatorEnergyAccumulator = 0.0f;
    static float alternatorEnergyAccumulator_AllTime = 0.0f;
    float alternatorPower_W = (currentBatteryVoltage * MeasuredAmps);
    if (alternatorPower_W > alt_power_max_alltime_w) alt_power_max_alltime_w = alternatorPower_W;  // leaderboard: peak alt power
    float altEnergyDelta_Wh = (alternatorPower_W * elapsedSeconds) / 3600.0f;

    if (altEnergyDelta_Wh > 0) {
      alternatorEnergyAccumulator += altEnergyDelta_Wh;
      alternatorEnergyAccumulator_AllTime += altEnergyDelta_Wh;
      if (alternatorEnergyAccumulator >= 1.0f) {
        AlternatorChargedEnergy += (int)alternatorEnergyAccumulator;
        alternatorEnergyAccumulator -= (int)alternatorEnergyAccumulator;
      }
      if (alternatorEnergyAccumulator_AllTime >= 1.0f) {
        AlternatorChargedEnergy_AllTime += (int)alternatorEnergyAccumulator_AllTime;
        alternatorEnergyAccumulator_AllTime -= (int)alternatorEnergyAccumulator_AllTime;
      }
      // =================================================================
      //     FUEL CONSUMPTION CALCULATION - ALTERNATOR SPECIFIC
      // =================================================================
      // Calculate diesel fuel used to produce this electrical energy
      // Chain: Diesel → Engine (30% eff) → Alternator (50% eff) → Electricity

      float energyJoules = altEnergyDelta_Wh * 3600.0f;
      const float engineEfficiency = 0.30f;              // Engine: fuel → mechanical (30%)
      const float alternatorEfficiency = 0.50f;          // Alt: mechanical → electrical (50%)

      float fuelEnergyUsed_J = energyJoules / (engineEfficiency * alternatorEfficiency);
      const float dieselEnergy_J_per_mL = 36000.0f;  // Energy content of diesel

      float fuelUsed_mL = fuelEnergyUsed_J / dieselEnergy_J_per_mL;
      float fuelUsed_L = fuelUsed_mL / 1000.0f;

      static float fuelAccumulator = 0.0f;
      static float fuelAccumulator_AllTime = 0.0f;
      fuelAccumulator += fuelUsed_L;
      fuelAccumulator_AllTime += fuelUsed_L;

      if (fuelAccumulator >= 0.01f) {
        float toAdd = floor(fuelAccumulator * 100.0f) / 100.0f;  // Round down to nearest 0.01 L
        AlternatorFuelUsed += toAdd;
        fuelAccumulator -= toAdd;  // Keep the fractional remainder
      }
      if (fuelAccumulator_AllTime >= 0.01f) {
        float toAdd = floor(fuelAccumulator_AllTime * 100.0f) / 100.0f;
        AlternatorFuelUsed_AllTime += toAdd;
        fuelAccumulator_AllTime -= toAdd;  // Keep the fractional remainder
      }
    }
  }

  // =================================================================
  //     CHARGE CYCLE CALCULATION
  // =================================================================
  // One cycle = one full battery capacity worth of energy charged.
  // Nominal bank class comes from user-entered SYSTEM_VOLTAGE_CLASS, never guessed from measured
  // voltage (a deeply-discharged 24V bank sagging below 16V would mis-bucket as 12V, 2× cycle error).
  float nominalVoltage = (float)SYSTEM_VOLTAGE_CLASS;

  float batteryCapacity_Wh = BatteryCapacity_Ah * nominalVoltage;

  if (batteryCapacity_Wh > 0) {
    ChargeCycles = (ChargedEnergy / batteryCapacity_Wh);  //Units: Wh
    ChargeCycles_AllTime = (ChargedEnergy_AllTime) / batteryCapacity_Wh;
  }

  // Average-SoC bookkeeping is meaningless without a real Bcur-driven SOC_percent.
  if (HAS_BATT_SHUNT) {
    // ===== AVERAGE SOC TRACKING (TIME-WEIGHTED) =====
    static float socAccumulator = 0.0f;
    // socAccumulator_AllTime and totalSocSampleTime_AllTime are globals, not statics here
    static unsigned long totalSocSampleTime = 0;  // Session seconds tracked

    float currentSOC = SOC_percent / 100.0f;  // Convert to actual percentage

    socAccumulator += currentSOC * elapsedSeconds;
    socAccumulator_AllTime += (double)currentSOC * elapsedSeconds;
    totalSocSampleTime += (unsigned long)elapsedSeconds;
    totalSocSampleTime_AllTime += (unsigned long)elapsedSeconds;

    if (totalSocSampleTime > 0) {
      AvgSOC = socAccumulator / totalSocSampleTime;
    }
    if (totalSocSampleTime_AllTime > 0) {
      AvgSOC_AllTime = socAccumulator_AllTime / totalSocSampleTime_AllTime;
    }


    if (totalSocSampleTime_AllTime > 0) {
      float calculatedAvg = socAccumulator_AllTime / totalSocSampleTime_AllTime;
    } else {
      Serial.println("WARNING: totalSocSampleTime_AllTime is ZERO!");
    }
  }


  totalVoltageSampleTime_AllTime += elapsedSeconds;

  // voltageAccumulator_AllTime = 0.0f; don't do this shit anymore
  float currentVoltage = getBatteryVoltage();
  voltageAccumulator_AllTime += currentVoltage * elapsedSeconds;

  if (totalVoltageSampleTime_AllTime > 0) {
    AvgVoltage_AllTime = voltageAccumulator_AllTime / totalVoltageSampleTime_AllTime;
  }
}
void UpdateTravelStatistics(unsigned long elapsedMillis) {
  // ===== DISTANCE CALCULATION - Using GPS position (Haversine) =====

  // gpsValid is shared with trip-tracking below — not stale, not NaN, not (0,0);
  // the (0,0) check rejects the "boot before first fix" sentinel.
  bool gpsValid = !IS_STALE(IDX_LATITUDE_NMEA) && !IS_STALE(IDX_LONGITUDE_NMEA)
                  && !isnan(LatitudeNMEA) && !isnan(LongitudeNMEA)
                  && !(LatitudeNMEA == 0.0 && LongitudeNMEA == 0.0);
  float tripDistanceDelta_nm = 0.0f;  // jump-filtered delta for the trip accumulator

  if (gpsValid) {
    static double lastLat = 0;
    static double lastLon = 0;
    static uint32_t lastFixMs = 0;
    static bool firstRun = true;

    if (firstRun) {
      lastLat = LatitudeNMEA;
      lastLon = LongitudeNMEA;
      lastFixMs = millis();
      firstRun = false;
    } else {
      double distanceDelta_nm = calculateHaversineDistance(lastLat, lastLon, LatitudeNMEA, LongitudeNMEA);

      // Only act when the fix actually moved. Position is re-read every 2 s but only
      // refreshes every 60 s on phone GPS, so most ticks see an identical fix (delta 0).
      if (distanceDelta_nm > 0.0) {
        uint32_t nowMs = millis();
        uint32_t dtMs = nowMs - lastFixMs;
        float impliedKn = (dtMs > 0) ? (float)(distanceDelta_nm / (dtMs / 3600000.0)) : 9999.0f;

        // Implied-speed gate: a <=5 min gap at <150 kt is real motion (covers ~100 mph
        // powerboats; real GPS teleports imply hundreds of kt); anything else rebaselines
        // without polluting the odometer.
        if (dtMs <= 300000UL && impliedKn < 150.0f) {
          static float distanceAccumulator = 0.0f;
          static float distanceAccumulator_AllTime = 0.0f;
          static float sailingDistAccumulator_AllTime = 0.0f;  // engine-off miles (leaderboard)

          distanceAccumulator += (float)distanceDelta_nm;
          distanceAccumulator_AllTime += (float)distanceDelta_nm;
          if (RPM < 50 && Ignition == 0) sailingDistAccumulator_AllTime += (float)distanceDelta_nm;  // under sail = engine off (matches sailing_days gate)

          if (distanceAccumulator >= 0.01f) {
            TotalDistance += distanceAccumulator;
            distanceAccumulator = 0.0f;
          }
          if (distanceAccumulator_AllTime >= 0.01f) {
            TotalDistance_AllTime += distanceAccumulator_AllTime;
            distanceAccumulator_AllTime = 0.0f;
          }
          if (sailingDistAccumulator_AllTime >= 0.01f) {
            sailing_dist_alltime += sailingDistAccumulator_AllTime;
            sailingDistAccumulator_AllTime = 0.0f;
          }

          tripDistanceDelta_nm = (float)distanceDelta_nm;  // reuse validated delta for trip
        }

        lastLat = LatitudeNMEA;
        lastLon = LongitudeNMEA;
        lastFixMs = nowMs;
      }
    }
  }

  // LONGEST SINGLE TRIP TRACKING
  // Trip ends after 60 min continuous (a) GPS invalid OR (b) SOG < 1.5 kn.
  // Either valid GPS + SOG >= 1.5 kn sample resets both timers.
  // Boot recovery: if NVS had an in-progress trip, hold in stasis until time
  // syncs, then resume (epoch < 1 hr old) or finalize (stale). 10-min fallback
  // forces stale-finalize if time never syncs (e.g., no GPS / no NTP).
  {
    if (tripPendingRecovery) {
      if (timeIsSynced && timeBase > 0) {
        uint32_t nowEpoch = timeBase + (millis() - timeBaseMillis) / 1000;
        bool fresh = (currentTripLastUpdateEpoch > 0)
                  && (nowEpoch >= currentTripLastUpdateEpoch)
                  && ((nowEpoch - currentTripLastUpdateEpoch) < 3600UL);
        if (fresh) {
          tripActive = true;
        } else {
          if (currentTripDistanceNm > LongestSingleTrip_Nm_AllTime) LongestSingleTrip_Nm_AllTime = currentTripDistanceNm;
          tripActive = false;
          currentTripDistanceNm = 0.0f;
          currentTripLastUpdateEpoch = 0;
        }
        tripPendingRecovery = false;
      } else if (millis() > 600000UL) {
        // 10 min since boot without a time sync — finalize conservatively so the saved trip isn't held in limbo forever.
        if (currentTripDistanceNm > LongestSingleTrip_Nm_AllTime) LongestSingleTrip_Nm_AllTime = currentTripDistanceNm;
        tripActive = false;
        currentTripDistanceNm = 0.0f;
        currentTripLastUpdateEpoch = 0;
        tripPendingRecovery = false;
      }
    }

    if (!tripPendingRecovery) {
      bool sogValid = !IS_STALE(IDX_SOG_NMEA) && SOGNMEA >= 0;
      float sog_kn = sogValid ? SOGNMEA : 0.0f;  // stale SOG counts as no motion

      if (gpsValid && sog_kn >= 1.5f) {
        timeSinceLastMotion = 0;
        timeSinceLastValidGps = 0;
        if (!tripActive) {
          tripActive = true;
          currentTripDistanceNm = 0.0f;
        }
        currentTripDistanceNm += tripDistanceDelta_nm;
        // Stamp wall-clock epoch (project pattern, since GPS/phone time sources don't touch system clock).
        // Leaving as 0 when time isn't synced means boot recovery conservatively finalizes the trip.
        if (timeIsSynced) currentTripLastUpdateEpoch = timeBase + (millis() - timeBaseMillis) / 1000;
      } else {
        if (!gpsValid)     timeSinceLastValidGps += elapsedMillis;
        else               timeSinceLastValidGps = 0;
        if (sog_kn < 1.5f) timeSinceLastMotion   += elapsedMillis;
        else               timeSinceLastMotion   = 0;

        if (tripActive && (timeSinceLastMotion >= 3600000UL || timeSinceLastValidGps >= 3600000UL)) {
          if (currentTripDistanceNm > LongestSingleTrip_Nm_AllTime) {
            LongestSingleTrip_Nm_AllTime = currentTripDistanceNm;
          }
          tripActive = false;
          currentTripDistanceNm = 0.0f;
        }
      }
    }
  }

  // ROLLING 24-HOUR MAX DISTANCE (lb-max-24hr leaderboard)
  // Ring of 24 hourly buckets; bucket[head] = in-progress hour. Sum = approx
  // last 24 hr of motion (~1 hr undercount at the leading edge).
  {
    uint32_t now = millis();
    if (distHourStartMs == 0) distHourStartMs = now;  // first-tick init
    while ((uint32_t)(now - distHourStartMs) >= 3600000UL) {
      distHourHead = (distHourHead + 1) % 24;
      distHourBuckets[distHourHead] = 0.0f;
      distHourStartMs += 3600000UL;
    }
    distHourBuckets[distHourHead] += tripDistanceDelta_nm;
    float rollingSum = 0.0f;
    for (uint8_t i = 0; i < 24; i++) rollingSum += distHourBuckets[i];
    if (rollingSum > Max24hrDistance_AllTime) Max24hrDistance_AllTime = rollingSum;
  }

  UpdateAnchorageDetection(gpsValid);

  // ===== SPEED CALCULATION - Using SOGNMEA (time-weighted average) - UNCHANGED =====

  if (IS_STALE(IDX_SOG_NMEA) || SOGNMEA < 0) {
    return;
  }

  static float speedAccumulator = 0.0f;
  static unsigned long totalSpeedSampleTime = 0;  // Session seconds

  float elapsedSeconds = elapsedMillis / 1000.0f;

  speedAccumulator += (float)(SOGNMEA * elapsedSeconds);
  speedAccumulator_AllTime += (float)(SOGNMEA * elapsedSeconds);
  totalSpeedSampleTime += (unsigned long)elapsedSeconds;
  totalSpeedSampleTime_AllTime += (unsigned long)elapsedSeconds;

  if (totalSpeedSampleTime > 0) {
    AvgSpeed = speedAccumulator / (float)totalSpeedSampleTime;
  }
  if (totalSpeedSampleTime_AllTime > 0) {
    AvgSpeed_AllTime = speedAccumulator_AllTime / (float)totalSpeedSampleTime_AllTime;
  }
}

// Anchorage detection — pushes one sample per minute into the PSRAM ring while GPS+depth are
// both valid. Each push evaluates the trailing sliding 5-hr span; if continuous (no gaps > 10 min)
// and qualifying (swing < 100 yd, depth varies 2-100 ft), updates DeepestAnchorage_Ft_AllTime
// with the window's average depth.
void UpdateAnchorageDetection(bool gpsValid) {
  if (!anchorageRing) return;
  const uint32_t SAMPLE_INTERVAL_MS  = 60000UL;            // 1-minute sample cadence
  const uint32_t WINDOW_SPAN_MS      = 5UL * 3600UL * 1000UL; // 5 hours
  const uint32_t GAP_TOLERANCE_MS    = 10UL * 60UL * 1000UL;  // 10 min reboot/dropout tolerance
  const double   MAX_SWING_NM        = 0.0494;             // ~100 yards
  const float    MIN_DEPTH_CHANGE_FT = 2.0f;
  const float    MAX_DEPTH_CHANGE_FT = 100.0f;
  uint32_t now = millis();
  if ((uint32_t)(now - lastAnchorageSampleMs) < SAMPLE_INTERVAL_MS) return;
  // Only sample when we have BOTH valid GPS AND fresh depth — otherwise the window can't qualify.
  if (!gpsValid || IS_STALE(IDX_WATER_DEPTH)) {
    lastAnchorageSampleMs = now;  // throttle gate; skip this sample (gap will be detected on resume)
    return;
  }
  anchorageRing[anchorageRingHead].sampleMs = now;
  anchorageRing[anchorageRingHead].lat = LatitudeNMEA;
  anchorageRing[anchorageRingHead].lon = LongitudeNMEA;
  anchorageRing[anchorageRingHead].depth_ft = WaterDepth_m * 3.28084f;
  anchorageRingHead = (anchorageRingHead + 1) % ANCHORAGE_RING_SIZE;
  if (anchorageRingCount < ANCHORAGE_RING_SIZE) anchorageRingCount++;
  lastAnchorageSampleMs = now;
  // Evaluate trailing window. Walk backward from newest, collecting samples until we hit a
  // gap > GAP_TOLERANCE_MS or run out of samples. Need at least WINDOW_SPAN_MS of contiguous coverage.
  if (anchorageRingCount < 2) return;
  uint16_t newestIdx = (anchorageRingHead + ANCHORAGE_RING_SIZE - 1) % ANCHORAGE_RING_SIZE;
  uint32_t newestMs = anchorageRing[newestIdx].sampleMs;
  uint16_t spanCount = 1;
  uint16_t oldestValidIdx = newestIdx;
  for (uint16_t step = 1; step < anchorageRingCount; step++) {
    uint16_t prevIdx = (newestIdx + ANCHORAGE_RING_SIZE - step) % ANCHORAGE_RING_SIZE;
    uint16_t laterIdx = (newestIdx + ANCHORAGE_RING_SIZE - step + 1) % ANCHORAGE_RING_SIZE;
    uint32_t gap = (uint32_t)(anchorageRing[laterIdx].sampleMs - anchorageRing[prevIdx].sampleMs);
    if (gap > GAP_TOLERANCE_MS) break;  // contiguous span ends here
    oldestValidIdx = prevIdx;
    spanCount++;
  }
  uint32_t spanMs = (uint32_t)(newestMs - anchorageRing[oldestValidIdx].sampleMs);
  // Accept a window one sample short: a full 300-slot ring spans only 299 intervals
  // (~17.94 M ms), so the strict < 18 M ms check could essentially never pass.
  if (spanMs + SAMPLE_INTERVAL_MS < WINDOW_SPAN_MS) return;  // not yet 5 contiguous hours
  // Compute centroid + depth stats over the valid trailing span.
  double sumLat = 0, sumLon = 0;
  float depthMin = 1e9f, depthMax = -1e9f, depthSum = 0.0f;
  for (uint16_t step = 0; step < spanCount; step++) {
    uint16_t idx = (newestIdx + ANCHORAGE_RING_SIZE - step) % ANCHORAGE_RING_SIZE;
    sumLat += anchorageRing[idx].lat;
    sumLon += anchorageRing[idx].lon;
    float d = anchorageRing[idx].depth_ft;
    if (d < depthMin) depthMin = d;
    if (d > depthMax) depthMax = d;
    depthSum += d;
  }
  double centroidLat = sumLat / (double)spanCount;
  double centroidLon = sumLon / (double)spanCount;
  float depthAvg = depthSum / (float)spanCount;
  float depthRange = depthMax - depthMin;
  // Max distance from centroid (Haversine).
  double maxSwingNm = 0;
  for (uint16_t step = 0; step < spanCount; step++) {
    uint16_t idx = (newestIdx + ANCHORAGE_RING_SIZE - step) % ANCHORAGE_RING_SIZE;
    double d = calculateHaversineDistance(centroidLat, centroidLon,
                                          anchorageRing[idx].lat, anchorageRing[idx].lon);
    if (d > maxSwingNm) maxSwingNm = d;
  }
  if (maxSwingNm > MAX_SWING_NM) return;
  if (depthRange < MIN_DEPTH_CHANGE_FT || depthRange > MAX_DEPTH_CHANGE_FT) return;
  // Qualifying anchorage — update lifetime watermark.
  if (depthAvg > DeepestAnchorage_Ft_AllTime) DeepestAnchorage_Ft_AllTime = depthAvg;
}

// Single source of truth for "engine is spinning" (tach gate). Feeds run-hour
// accounting AND the sensor-window engine-on-weighted accumulators.
bool engineSpinning() {
  return RPM > 100 && RPM < 20000;
}

void UpdateEngineRuntime(unsigned long elapsedMillis) {
  bool engineIsRunning = engineSpinning();

  if (engineIsRunning) {
    engineRunAccumulator += elapsedMillis;

    if (engineRunAccumulator >= 1000) {
      int secondsRun = engineRunAccumulator / 1000;
      EngineRunTime += secondsRun;
      EngineRunTime_AllTime += secondsRun;

      int cyclesDelta = (RPM * secondsRun) / 60;
      EngineCycles += cyclesDelta;
      EngineCycles_AllTime += cyclesDelta;

      // Keep the remainder milliseconds
      engineRunAccumulator %= 1000;
    }
  }

  // Engine start/stop edge — one console line per transition. Anchors the session
  // timeline so charging events can be read against engine state.
  if (engineIsRunning && !engineWasRunning) {
    queueConsoleMessageF("Engine STARTED (RPM=%d)", (int)RPM);
  } else if (!engineIsRunning && engineWasRunning) {
    queueConsoleMessageF("Engine STOPPED (RPM=%d)", (int)RPM);
  }

  engineWasRunning = engineIsRunning;
}
// 5-sample moving average
void getSmoothedGPS(double &smoothLat, double &smoothLon) {
  if (gpsBufferCount == 0) {
    smoothLat = LatitudeNMEA;
    smoothLon = LongitudeNMEA;
    return;
  }

  double latSum = 0;
  double lonSum = 0;
  int samplesToAverage = min(gpsBufferCount, GPS_SMOOTHING_SAMPLES);

  for (int i = 0; i < samplesToAverage; i++) {
    latSum += latBuffer[i];
    lonSum += lonBuffer[i];
  }

  smoothLat = latSum / samplesToAverage;
  smoothLon = lonSum / samplesToAverage;
}
// Call on each new GPS position
void updateGPSBuffer() {
  if (IS_STALE(IDX_LATITUDE_NMEA) || IS_STALE(IDX_LONGITUDE_NMEA)) {
    return;
  }

  // On first valid reading, pre-fill entire buffer
  if (gpsBufferCount == 0) {
    for (int i = 0; i < GPS_SMOOTHING_SAMPLES; i++) {
      latBuffer[i] = LatitudeNMEA;
      lonBuffer[i] = LongitudeNMEA;
    }
    gpsBufferCount = GPS_SMOOTHING_SAMPLES;
    gpsBufferIndex = 0;
    Serial.println("GPS buffer initialized");
    return;
  }

  latBuffer[gpsBufferIndex] = LatitudeNMEA;
  lonBuffer[gpsBufferIndex] = LongitudeNMEA;

  gpsBufferIndex = (gpsBufferIndex + 1) % GPS_SMOOTHING_SAMPLES;
}
double calculateHaversineDistance(double lat1, double lon1, double lat2, double lon2) {
  const double R = 3440.065;  // Earth's radius in nautical miles

  double lat1_rad = lat1 * PI / 180.0;
  double lat2_rad = lat2 * PI / 180.0;
  double delta_lat = (lat2 - lat1) * PI / 180.0;
  double delta_lon = (lon2 - lon1) * PI / 180.0;

  double a = sin(delta_lat / 2.0) * sin(delta_lat / 2.0) + cos(lat1_rad) * cos(lat2_rad) * sin(delta_lon / 2.0) * sin(delta_lon / 2.0);
  double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));

  return R * c;  // Distance in nautical miles
}

/**
 * UpdateSailingMetrics - Track sailing time and calculate ratio
 * Sailing conditions: SOG > 0.5 knots AND RPM < 50 AND Ignition == 0
 */
void UpdateSailingMetrics(unsigned long elapsedMillis) {
  bool isSailing = false;
  if (!IS_STALE(IDX_SOG_NMEA) && SOGNMEA > 0.5 && RPM < 50 && Ignition == 0) {
    isSailing = true;
  }

  float elapsedSeconds = elapsedMillis / 1000.0f;

  if (isSailing) {
    float elapsedDays = elapsedMillis / (1000.0f * 60.0f * 60.0f * 24.0f);

    static float sailingDaysAccumulator = 0.0f;
    sailingDaysAccumulator += elapsedDays;

    // Update when accumulated at least 0.001 days (~1.4 minutes)
    if (sailingDaysAccumulator >= 0.001f) {
      sailing_days_alltime += sailingDaysAccumulator;
      sailingDaysAccumulator = 0.0f;
      // Will be saved to NVS every 2 minutes
    }
  }

  float total_operational_days = totalSocSampleTime_AllTime / 86400.0f;
  if (total_operational_days > 0) {
    sailing_ratio = (sailing_days_alltime / total_operational_days) * 100.0f;
  } else {
    sailing_ratio = 0.0f;
  }
}
void UpdateWindMaximums() {
  if (!IS_STALE(IDX_APPARENT_WIND_SPEED) && ApparentWindSpeedNMEA > max_wind_speed_apparent_alltime) {
    max_wind_speed_apparent_alltime = ApparentWindSpeedNMEA;
  }
  if (!isnan(TrueWindSpeedNMEA) && TrueWindSpeedNMEA > max_wind_speed_true_alltime) {
    max_wind_speed_true_alltime = TrueWindSpeedNMEA;
  }
  // ignition-cycle watermarks (lo + hi, separate from *_alltime above)
  if (!IS_STALE(IDX_APPARENT_WIND_SPEED)) wmIgnUpdate(wmIgn_AWS, ApparentWindSpeedNMEA);
  if (!isnan(TrueWindSpeedNMEA))          wmIgnUpdate(wmIgn_TWS, TrueWindSpeedNMEA);
}

void UpdateBoardTempPressureMaximums() {
  // Guard per-field — NaN compares false against everything, but watermark updaters
  // shouldn't ingest NaN. A BMP388 read failure could surface NaN on either channel.
  if (!isnan(ambientTemp)) {
    if (ambientTemp > board_temp_max_alltime)        board_temp_max_alltime    = ambientTemp;
    if (ambientTemp < board_temp_min_alltime)        board_temp_min_alltime    = ambientTemp;
    wmIgnUpdate(wmIgn_ambient, ambientTemp);
  }
  if (!isnan(baroPressure)) {
    if (baroPressure > baro_pressure_max_alltime)    baro_pressure_max_alltime = baroPressure;
    if (baroPressure < baro_pressure_min_alltime)    baro_pressure_min_alltime = baroPressure;
    wmIgnUpdate(wmIgn_baro, baroPressure);
  }
}


float getBatteryCurrent() {
  // Source variables are populated by ReadAnalogInputs() or ReadAnalogInputs_Fake()
  switch (BatteryCurrentSource) {
    case 3: return VictronCurrent;  // Victron VE.Direct
    default: return Bcur;           // INA228 (case 0, and fallback)
  }
}
float getBatteryVoltage() {
  // INA228 bus voltage — more accurate (20-bit, no divider drift) and faster in field-on mode.
  // BatteryV (ADS1115) is retained separately for the cross-sensor disagreement check only.
  return IBV;
}
float getTargetAmps() {
  return MeasuredAmps;
}

float getFiltV() {
  // Never use for safety checks — use IBV directly.
  return IBV_filtered;
}

// Channel 3 topology per docs/hardware/analoginputsADS1115.md:
//   3.3 V → Thermistor (R_NTC) → V_node → R_fixed (10 kΩ pulldown) → GND
//   V_node = 3.3 × R_fixed / (R_fixed + R_NTC)  →  R_NTC = R_fixed × (Vcc - V) / V
float thermistorTempC(float V_node) {
  const float Vcc = 3.3f;
  if (V_node <= 0.0f || V_node >= Vcc) return -99.0f;  // unrecoverable: divide-by-zero or rail
  float R_NTC = R_fixed * (Vcc - V_node) / V_node;
  float T0_K = T0_C + 273.15f;
  float tempK = 1.0f / ((1.0f / T0_K) + (1.0f / Beta) * log(R_NTC / R0));
  return tempK - 273.15f;
}

// ===== CheckAlarms() - Alarm monitoring and INA228 hardware protection =====
void CheckAlarms() {
  static unsigned long lastRunTime = 0;
  if (millis() - lastRunTime < 250) return;
  lastRunTime = millis();

  static bool previousAlarmState = false;
  bool currentAlarmCondition = false;
  bool outputAlarmState = false;
  const char *alarmReason = "";

  // ========== ALARM TEST MODE ==========
  if (AlarmTest == 1) {
    if (alarmTestStartTime == 0) {
      alarmTestStartTime = millis();
      queueConsoleMessage("ALARM TEST: Testing buzzer for 2 seconds");
    }

    if (millis() - alarmTestStartTime < ALARM_TEST_DURATION) {
      currentAlarmCondition = true;
      alarmReason = "Alarm test active";
    } else {
      AlarmTest = 0;
      alarmTestStartTime = 0;
      queueConsoleMessage("ALARM TEST: Complete");
    }
  }

  // ========== NORMAL ALARM CHECKING ==========
  if (AlarmActivate == 1) {
    if (TempSource == 0) {
      TempToUse = AlternatorTemperatureF;
    } else if (TempSource == 1) {
      TempToUse = temperatureThermistor;
    }

    static unsigned long lastTempAlarmMsgMs = 0;
    if (TempAlarm > 0 && TempToUse > TempAlarm) {
      currentAlarmCondition = true;
      alarmReason = "High alternator temperature";
      if (millis() - lastTempAlarmMsgMs >= 30000) {
        lastTempAlarmMsgMs = millis();
        queueConsoleMessageF("High alternator temperature: %.1f%s (limit: %.0f%s)",
                             dispTempF(TempToUse), dispTempUnit(), dispTempF((float)TempAlarm), dispTempUnit());
      }
    } else {
      lastTempAlarmMsgMs = 0;  // Reset so it fires immediately when condition returns
    }

    static unsigned long lastTempLowAlarmMsgMs = 0;
    // -99 is the thermistor's disconnected sentinel (Channel3V < 0.05 V — the shipping ground
    // jumper) — the stale-sensor alarm owns that case; without the guard it reads as "cold".
    if (TempAlarmLow > 0 && TempToUse != -99 && TempToUse < TempAlarmLow) {
      currentAlarmCondition = true;
      alarmReason = "Low alternator temperature";
      if (millis() - lastTempLowAlarmMsgMs >= 30000) {
        lastTempLowAlarmMsgMs = millis();
        queueConsoleMessageF("Low alternator temperature: %.1f%s (limit: %.0f%s)",
                             dispTempF(TempToUse), dispTempUnit(), dispTempF((float)TempAlarmLow), dispTempUnit());
      }
    } else {
      lastTempLowAlarmMsgMs = 0;  // Reset so it fires immediately when condition returns
    }

    // Cold-charge lockout alarm — fail-open when no battery temperature source qualifies, matching buildTickSnapshot().
    static unsigned long lastColdChargeAlarmMsgMs = 0;
    if (coldChargeLockoutEnable && isfinite(battTempActiveF) && battTempActiveF < MinChargeTempF) {
      currentAlarmCondition = true;
      alarmReason = "Battery too cold to charge";
      if (millis() - lastColdChargeAlarmMsgMs >= 30000) {
        lastColdChargeAlarmMsgMs = millis();
        queueConsoleMessageF("Cold-charge lockout: battery temperature %.1f%s (%s) below %.0f%s floor — charging disabled",
                             dispTempF(battTempActiveF), dispTempUnit(), battTempSrcName(battTempActiveSrc),
                             dispTempF(MinChargeTempF), dispTempUnit());
      }
    } else {
      lastColdChargeAlarmMsgMs = 0;  // Reset so it fires immediately when condition returns
    }

    // Hot-charge lockout alarm — measured battery source (1..4) only, matching buildTickSnapshot().
    static unsigned long lastHotChargeAlarmMsgMs = 0;
    if (hotChargeLockoutEnable == 1 && battTempActiveSrc >= 1 && battTempActiveSrc <= 4
        && isfinite(battTempActiveF) && battTempActiveF > MaxChargeTempF) {
      currentAlarmCondition = true;
      alarmReason = "Battery too hot to charge";
      if (millis() - lastHotChargeAlarmMsgMs >= 30000) {
        lastHotChargeAlarmMsgMs = millis();
        queueConsoleMessageF("Hot-charge lockout: battery temperature %.1f%s (%s) above %.0f%s ceiling — charging disabled",
                             dispTempF(battTempActiveF), dispTempUnit(), battTempSrcName(battTempActiveSrc),
                             dispTempF(MaxChargeTempF), dispTempUnit());
      }
    } else {
      lastHotChargeAlarmMsgMs = 0;
    }

    // Extra-probe high / low alarms — advisory only, never a field action.
    bool extraTempOk = tempProbeFresh(IDX_EXTRA_TEMP) && isfinite(ExtraTempF);
    static unsigned long lastExtraHiAlarmMsgMs = 0;
    if (extraTempAlarmHiEnable == 1 && extraTempOk && ExtraTempF > extraTempAlarmHiF) {
      currentAlarmCondition = true;
      alarmReason = "Extra temperature high";
      if (millis() - lastExtraHiAlarmMsgMs >= 30000) {
        lastExtraHiAlarmMsgMs = millis();
        queueConsoleMessageF("Extra temperature high: %.1f%s (limit: %.0f%s)",
                             dispTempF(ExtraTempF), dispTempUnit(), dispTempF(extraTempAlarmHiF), dispTempUnit());
      }
    } else {
      lastExtraHiAlarmMsgMs = 0;
    }
    static unsigned long lastExtraLoAlarmMsgMs = 0;
    if (extraTempAlarmLoEnable == 1 && extraTempOk && ExtraTempF < extraTempAlarmLoF) {
      currentAlarmCondition = true;
      alarmReason = "Extra temperature low";
      if (millis() - lastExtraLoAlarmMsgMs >= 30000) {
        lastExtraLoAlarmMsgMs = millis();
        queueConsoleMessageF("Extra temperature low: %.1f%s (limit: %.0f%s)",
                             dispTempF(ExtraTempF), dispTempUnit(), dispTempF(extraTempAlarmLoF), dispTempUnit());
      }
    } else {
      lastExtraLoAlarmMsgMs = 0;
    }

    // (Alternator-health is advisory-only — no audible alarm.)

    // Fast alt-current pulse-pattern fault (rectifier/stator) — opt-in audible alarm.
    // Sounds while a FAULT verdict is fresh (seen within the last 2 min); default OFF until
    // real-capture validation. AlarmActivate-gated like every other condition in this block.
    static unsigned long lastFaFaultAlarmMsgMs = 0;
    if (faAlarmEnable && faLastFaultMs != 0 && (millis() - faLastFaultMs < 120000UL)) {
      currentAlarmCondition = true;
      alarmReason = "Alternator current pulse-pattern fault";
      if (millis() - lastFaFaultAlarmMsgMs >= 30000) {
        lastFaFaultAlarmMsgMs = millis();
        queueConsoleMessageF("Alternator current pulse-pattern fault (class k=%u) -- check rectifier/stator", faDetectLastK);
      }
    } else {
      lastFaFaultAlarmMsgMs = 0;
    }

    float currentVoltage = getBatteryVoltage();

    static unsigned long lastVoltHighMsgMs = 0;
    if (VoltageAlarmHigh > 0 && currentVoltage > VoltageAlarmHigh) {
      currentAlarmCondition = true;
      alarmReason = "High battery voltage";
      if (millis() - lastVoltHighMsgMs >= 30000) {
        lastVoltHighMsgMs = millis();
        queueConsoleMessageF("High battery voltage: %.2fV (limit: %.2fV)",
                             currentVoltage, VoltageAlarmHigh);
      }
    } else {
      lastVoltHighMsgMs = 0;
    }

    static unsigned long lastVoltLowMsgMs = 0;
    // The > floor rejects a disconnected/0V reading; scale it by bank class (8V on 12V → 16/32V on
    // 24/36/48V) so it stays a "sensor disconnected" floor and never sits above a real low-V alarm point.
    if (VoltageAlarmLow > 0 && currentVoltage < VoltageAlarmLow && currentVoltage > 8.0 * SYSTEM_VOLTAGE_CLASS / 12.0f) {
      currentAlarmCondition = true;
      alarmReason = "Low battery voltage";
      if (millis() - lastVoltLowMsgMs >= 30000) {
        lastVoltLowMsgMs = millis();
        queueConsoleMessageF("Low battery voltage: %.2fV (limit: %.2fV)",
                             currentVoltage, VoltageAlarmLow);
      }
    } else {
      lastVoltLowMsgMs = 0;
    }

    static unsigned long lastSocLowMsgMs = 0;
    // SOC_percent is %×100; socInfoAvailable gates this the same way as the rebulk SoC gates
    if (HAS_BATT_SHUNT && SocAlarmLow > 0 && socInfoAvailable && SOC_percent < SocAlarmLow * 100) {
      currentAlarmCondition = true;
      alarmReason = "Low battery state of charge";
      if (millis() - lastSocLowMsgMs >= 30000) {
        lastSocLowMsgMs = millis();
        queueConsoleMessageF("Low battery state of charge: %.1f%% (limit: %d%%)",
                             SOC_percent / 100.0f, SocAlarmLow);
      }
    } else {
      lastSocLowMsgMs = 0;
    }

    static unsigned long lastCurHighMsgMs = 0;
    if (CurrentAlarmHigh > 0 && MeasuredAmps > CurrentAlarmHigh) {
      currentAlarmCondition = true;
      alarmReason = "High alternator current";
      if (millis() - lastCurHighMsgMs >= 30000) {
        lastCurHighMsgMs = millis();
        queueConsoleMessageF("High alternator current: %.1fA (limit: %.0fA)",
                             MeasuredAmps, CurrentAlarmHigh);
      }
    } else {
      lastCurHighMsgMs = 0;
    }

    static unsigned long lastBatCurMsgMs = 0;
    if (HAS_BATT_SHUNT && MaximumAllowedBatteryAmps > 0 && abs(Bcur) > MaximumAllowedBatteryAmps) {
      currentAlarmCondition = true;
      alarmReason = "High battery current";
      if (millis() - lastBatCurMsgMs >= 30000) {
        lastBatCurMsgMs = millis();
        queueConsoleMessageF("High battery current: %.1fA (limit: %.0fA)",
                             abs(Bcur), MaximumAllowedBatteryAmps);
      }
    } else {
      lastBatCurMsgMs = 0;
    }
  }

  // ========== TEMP TASK FAILURE ALARM ==========
  // Safety alarm — fires regardless of AlarmActivate. Throttled console reminder every 30s.
  if (tempTaskAlarm) {
    currentAlarmCondition = true;
    alarmReason = "TempTask failure — temperature monitoring lost";
    static unsigned long lastTempAlarmMsg = 0;
    if (millis() - lastTempAlarmMsg >= 30000) {
      lastTempAlarmMsg = millis();
      queueConsoleMessage("ALARM: TempTask not responding — temperature monitoring lost. Field reduced as safety measure.");
    }
  }

  // ========== TEMP DATA STALE ALARM ==========
  // Fires when no valid reading for 20s (sensor disconnected or persistent comm failure).
  // Gated by AlarmActivate. Complements the field cut that fires at the same threshold.
  if (AlarmActivate == 1) {
    uint32_t tempIdx = (TempSource == 0) ? IDX_ALTERNATOR_TEMP : IDX_THERMISTOR_TEMP;
    uint32_t ts = dataTimestamps[tempIdx];
    bool tempStale = (ts != 0 && (millis() - ts) > 20000) ||
                     (ts == 0 && millis() > 60000);  // never-connected grace period matches tick logic
    static unsigned long lastTempStaleAlarmMsg = 0;
    if (tempStale) {
      currentAlarmCondition = true;
      alarmReason = "Temperature sensor not responding";
      if (millis() - lastTempStaleAlarmMsg >= 30000) {
        lastTempStaleAlarmMsg = millis();
        queueConsoleMessage("ALARM: No temperature reading for 20s — sensor may be disconnected.");
      }
    } else {
      lastTempStaleAlarmMsg = 0;
    }
  }

  // ===== INA228 HARDWARE OVERVOLTAGE PROTECTION =====
  // The ALERT pin pulls GPIO4 low electrically the instant the bus crosses VoltageHardwareLimit —
  // before any software runs. This block only manages the software latch that keeps the field
  // suppressed until the condition is confirmed cleared, plus the messaging around it.
  // SLOW_ALERT is SET, so the comparison uses the averaged ADC value rather than instantaneous
  // reads — a single noise spike cannot assert ALERT. The averaging window follows the chip mode:
  // ~4.3 ms (4-sample) in fast charging mode, ~1054 ms (128-sample) with the field off — so while
  // charging the pin is a near-instant electrical cut. A transparent-mode pin blip that clears
  // before the 250 ms BUSOL status poll still cuts the field but is never logged here.
  // New-event detection is throttled to 5 s (I2C cost); the 3 s / 10 s latch checks run every 250 ms.

  if (INADisconnected == 0) {
    static unsigned long lastINA228Check = 0;
    uint16_t alertStatus = 0;

    // ── Latch management — runs every 250ms while latched ────────────────
    if (inaOvervoltageLatched) {
      uint32_t latchAge = millis() - inaOvervoltageTime;
      static bool earlyCheckDone = false;

      currentAlarmCondition = true;
      alarmReason = "INA228 hardware overvoltage";

      // Periodic reminder while latch is held — every 5s to avoid spam
      static unsigned long lastOVMessage = 0;
      if (millis() - lastOVMessage >= 5000) {
        lastOVMessage = millis();
        float busV = INA.getBusVoltage();
        queueConsoleMessageF(
          "INA228 OV LATCH HELD [%lus]: busV=%.3fV limit=%.3fV | "
          "ALERT pin cut GPIO4 electrically. Field suppressed by SW latch. "
          "Checking for clear at 3s and 10s.",
          latchAge / 1000, busV, VoltageHardwareLimit);
      }

      // ── 3s early check: fast release for genuine transient spikes ────
      if (!earlyCheckDone && latchAge >= 3000) {
        earlyCheckDone = true;
        clearINA228AlertLatch(INA.getAddress());
        alertStatus = readINA228AlertRegister(INA.getAddress());
        float busV = INA.getBusVoltage();

        // I²C error (0xFFFF) → treat as "still set", hold latch (fail-safe)
        if (alertStatus != 0xFFFF && !(alertStatus & 0x0010)) {
          // Cleared — was a transient. Release latch now.
          inaOvervoltageLatched = false;
          inaOvervoltageClearedMs = millis();
          earlyCheckDone = false;
          queueConsoleMessageF(
            "INA228 OV CLEARED [3s early check]: busV=%.3fV limit=%.3fV | "
            "ALERT pin was a transient spike. SW latch released. "
            "Voltage disagreement check suppressed for %ds. "
            "Field control returning to normal path.",
            busV, VoltageHardwareLimit,
            INA_OV_DISAGREE_SUPPRESS_MS / 1000);
        } else {
          // Still set — not a transient. Wait for 10s.
          queueConsoleMessageF(
            "INA228 OV STILL ACTIVE [3s check]: busV=%.3fV limit=%.3fV | "
            "BUSOL bit still set after clear attempt. Sustained condition. "
            "Will recheck at 10s.",
            busV, VoltageHardwareLimit);
        }
      }

      // ── 10s definitive check ─────────────────────────────────────────
      if (inaOvervoltageLatched && latchAge >= 10000) {
        earlyCheckDone = false;
        clearINA228AlertLatch(INA.getAddress());
        alertStatus = readINA228AlertRegister(INA.getAddress());
        float busV = INA.getBusVoltage();

        // I²C error (0xFFFF) → treat as "still set", hold latch (fail-safe)
        if (alertStatus != 0xFFFF && !(alertStatus & 0x0010)) {
          inaOvervoltageLatched = false;
          inaOvervoltageClearedMs = millis();
          queueConsoleMessageF(
            "INA228 OV CLEARED [10s timeout]: busV=%.3fV limit=%.3fV | "
            "BUSOL bit cleared. SW latch released. "
            "Voltage disagreement check suppressed for %ds. "
            "Field control returning to normal path.",
            busV, VoltageHardwareLimit,
            INA_OV_DISAGREE_SUPPRESS_MS / 1000);
        } else {
          // Still present — hold latch, will recheck on next 10s boundary
          // by resetting inaOvervoltageTime so the 10s check re-arms.
          inaOvervoltageTime = millis();
          currentAlarmCondition = true;
          alarmReason = "INA228 hardware overvoltage still present";
          queueConsoleMessageF(
            "INA228 OV SUSTAINED [10s check]: busV=%.3fV limit=%.3fV | "
            "BUSOL bit still set. Latch held. Will recheck in 10s.",
            busV, VoltageHardwareLimit);
        }
      }
    }

    // ── New event detection — throttled to every 5s ───────────────────
    // Hardware ALERT pin already cut GPIO4 before this runs.
    // This block detects the event and engages the software latch
    // so the field stays suppressed even if ALERT deasserts quickly.
    if (millis() - lastINA228Check >= 5000) {
      lastINA228Check = millis();
      alertStatus = readINA228AlertRegister(INA.getAddress());

      // I²C error (0xFFFF) → skip detection this tick (avoid false-positive OV latch)
      if (alertStatus != 0xFFFF && (alertStatus & 0x0010)) {
        if (!inaOvervoltageLatched) {
          inaOvervoltageLatched = true;
          inaOvervoltageTime = millis();
          currentAlarmCondition = true;

          float busV = INA.getBusVoltage();
          alarmReason = "INA228 hardware overvoltage";

          // Note: busV here may be lower than the actual peak because
          // the INA228 averaged value lags the instantaneous reading
          // that triggered the ALERT pin. The hardware caught the real peak;
          // this is a post-event averaged reading.
          queueConsoleMessageF(
            "INA228 HARDWARE ALERT PIN FIRED: busV(avg)=%.3fV limit=%.3fV | "
            "GPIO4 was cut electrically by ALERT pin before this message. "
            "SLOW_ALERT active: trigger used the chip's averaged value (~4.3ms in fast charging mode). "
            "SW latch engaged. Disagreement check suppressed for %ds.",
            busV, VoltageHardwareLimit,
            INA_OV_DISAGREE_SUPPRESS_MS / 1000);
        }
      }
    }
  }

  // ========== MANUAL LATCH RESET ==========
  if (ResetAlarmLatch == 1) {
    alarmLatch = false;
    ResetAlarmLatch = 0;
    queueConsoleMessage("ALARM LATCH: Manually reset");
  }

  // ========== LATCHING LOGIC ==========
  if (AlarmLatchEnabled == 1) {
    if (currentAlarmCondition) {
      alarmLatch = true;
    }
    outputAlarmState = alarmLatch;
  } else {
    outputAlarmState = currentAlarmCondition;
  }

  // ========== FINAL OUTPUT CONTROL ==========
  bool finalOutput = false;
  if (AlarmTest == 1) {
    finalOutput = true;  // Force HIGH regardless of alarm state — test drives output directly
  } else if (AlarmActivate == 1) {
    finalOutput = outputAlarmState;
  }

  digitalWrite(21, finalOutput ? HIGH : LOW);
  alarmOutputState = finalOutput;           // Keep shared state in sync
  Alarm_Status = alarmOutputState ? 1 : 0;  // Set after finalOutput is computed

  // ========== CONSOLE MESSAGING ==========
  if (currentAlarmCondition != previousAlarmState) {
    if (currentAlarmCondition) {
      queueConsoleMessageF("ALARM ACTIVATED: %s", alarmReason);
    } else if (AlarmLatchEnabled == 0) {
      queueConsoleMessage("ALARM CLEARED");
    }
    previousAlarmState = currentAlarmCondition;
  }
}


// Human-readable boot/reset cause for the console boot line. esp_reset_reason()
// is constant for the whole session, so it can be read lazily when the line is emitted.
const char *resetReasonName() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_SW:        return "software";
    case ESP_RST_DEEPSLEEP: return "deep-sleep wake";
    case ESP_RST_EXT:       return "external";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_PANIC:     return "panic/crash";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_WDT:       return "other watchdog";
    case ESP_RST_SDIO:      return "SDIO";
    case ESP_RST_PWR_GLITCH: return "power glitch";
    case ESP_RST_CPU_LOCKUP: return "CPU lockup";
    case ESP_RST_USB:       return "USB";
    case ESP_RST_JTAG:      return "JTAG";
    case ESP_RST_EFUSE:     return "eFuse error";
    case ESP_RST_UNKNOWN:   return "indeterminate";
    default:                return "unrecognized code";
  }
}

void logDashboardValues() {
  static unsigned long lastDashboardLog = 0;
  if (millis() - lastDashboardLog >= 300000) {  // Every 5 min — periodic time/context anchor
    lastDashboardLog = millis();
    // No battery shunt -> Bcur is whatever floats on the INA current input and SOC_percent is a
    // frozen NVS value (coulomb counting is HAS_BATT_SHUNT-gated). Print "n/a" rather than numbers
    // that read like measurements in a log we later analyse as ground truth.
    char socStr[8], bcurStr[12];
    if (HAS_BATT_SHUNT) {
      snprintf(socStr,  sizeof(socStr),  "%d%%",   SOC_percent / 100);
      snprintf(bcurStr, sizeof(bcurStr), "%.1fA", Bcur);
    } else {
      strncpy(socStr,  "n/a", sizeof(socStr));
      strncpy(bcurStr, "n/a", sizeof(bcurStr));
    }
    queueConsoleMessageF("DASHBOARD: IBV=%.2fV SoC=%s AltI=%.1fA BattI=%s AltT=%d°F RPM=%d",
                         IBV, socStr, MeasuredAmps, bcurStr,
                         (int)AlternatorTemperatureF, (int)RPM);
  }
}

void applySocGainCorrection(int preResetShadowX100) {
  // battery current always comes from INA228
  if (AutoShuntGainCorrection == 0) {
    return;
  }

  unsigned long now = millis();
  if (now - lastGainCorrectionTime < MIN_GAIN_CORRECTION_INTERVAL) {
    queueConsoleMessage("SOC Gain: Correction blocked, too soon since last adjustment");
    return;
  }

  float expectedCapacity = BatteryCapacity_Ah;
  // Pre-reset SHADOW coulomb count from the caller (shadowCoulombX100) — unclamped, so it can sit
  // above capacity at the full anchor. Evidence is two-sided: below capacity = counted too little
  // (raise gain), above = counted too much (lower gain). A capacity-setting edit mid-cycle can
  // contaminate one event's evidence; the 5%/event and 0.8-1.2 clamps bound the damage.
  float calculatedCapacity = preResetShadowX100 / 100.0;

  if (calculatedCapacity < 10 || expectedCapacity < 10) {
    queueConsoleMessage("SOC Gain: Invalid capacity values, skipping correction");
    return;
  }

  float errorRatio = abs(expectedCapacity - calculatedCapacity) / expectedCapacity;

  if (errorRatio > MAX_REASONABLE_ERROR) {
    queueConsoleMessageF("SOC Gain: Error too large (%.1f%%), ignoring correction",
                         errorRatio * 100);
    return;
  }

  float desiredCorrectionFactor = expectedCapacity / calculatedCapacity;
  float currentFactor = DynamicShuntGainFactor;
  float newFactor = currentFactor * desiredCorrectionFactor;

  float maxChange = currentFactor * MAX_GAIN_ADJUSTMENT_PER_CYCLE;
  if (newFactor > currentFactor + maxChange) {
    newFactor = currentFactor + maxChange;
    queueConsoleMessage("SOC Gain: Correction limited to maximum change rate");
  } else if (newFactor < currentFactor - maxChange) {
    newFactor = currentFactor - maxChange;
    queueConsoleMessage("SOC Gain: Correction limited to maximum change rate");
  }

  if (newFactor > MAX_DYNAMIC_GAIN_FACTOR) {
    newFactor = MAX_DYNAMIC_GAIN_FACTOR;
    queueConsoleMessageF("SOC Gain: Factor hit maximum limit (%.2f), check system",
                         MAX_DYNAMIC_GAIN_FACTOR);
  } else if (newFactor < MIN_DYNAMIC_GAIN_FACTOR) {
    newFactor = MIN_DYNAMIC_GAIN_FACTOR;
    queueConsoleMessageF("SOC Gain: Factor hit minimum limit (%.2f), check system",
                         MIN_DYNAMIC_GAIN_FACTOR);
  }

  DynamicShuntGainFactor = newFactor;
  lastGainCorrectionTime = now;
  queueConsoleMessageF("SOC Gain: Corrected from %.4f to %.4f (Calc:%.1fAh, Expected:%.1fAh)",
                       currentFactor, newFactor, calculatedCapacity, expectedCapacity);
}
void handleSocGainReset() {
  if (ResetDynamicShuntGain == 1) {
    DynamicShuntGainFactor = 1.0;
    lastGainCorrectionTime = 0;
    ResetDynamicShuntGain = 0;  // Clear the momentary flag
    queueConsoleMessage("SOC Gain: Dynamic gain factor reset to 1.0");
  }
}
// ===== Temperature-compensated zero correction (ZERO_DRIFT_TEMPCOMP_SPEC.md) ==================
// The zero-drift log is the data source; the daily fit builds a line zero(T)=c+b·(T−T_REF),
// EMA-smoothed across days, and the live correction is clamped to ±3 A.

// ZFitResult (one regression's output) is declared in Xregulator.ino so Arduino's auto-generated
// prototype for zeroFitRegress can see the return type.

// Least-squares fit of logged zero (amps) vs one sensor's temperature. Walks the ring newest→oldest,
// x-centered at T_REF so the intercept IS c, and stops the moment it has both enough points AND a
// >30 °F span — that block is the freshest data that reaches the required spread.
static ZFitResult zeroFitRegress(int sensor, uint32_t nowEpoch) {
  ZFitResult res; res.ok = false; res.c = res.b = res.r2 = res.residRms = res.span = 0.0f; res.n = 0;
  if (!zeroLogRing || zeroLogCount == 0) return res;
  double sx = 0, sy = 0, sxx = 0, sxy = 0, syy = 0;
  uint32_t n = 0;
  float tmin = 1e30f, tmax = -1e30f;
  for (uint16_t k = 0; k < zeroLogCount; k++) {
    uint16_t idx = (uint16_t)((zeroLogHead + ZEROLOG_RING_SIZE - 1 - k) % ZEROLOG_RING_SIZE);
    ZeroLogRecord &r = zeroLogRing[idx];
    if (nowEpoch > 0 && r.epoch > 0 && (nowEpoch - r.epoch) > ZFIT_MAX_LOOKBACK_S) break;  // 30-day cap
    int16_t traw = (sensor == ZF_BOARD) ? r.boardTempFx10 : r.altTempFx10;
    if (traw == ZEROLOG_TEMP_BLANK) continue;              // that sensor absent for this record
    if (isnan(r.amps) || isinf(r.amps)) continue;
    float T = traw / 10.0f;
    double x = (double)T - ZFIT_T_REF;
    double y = (double)r.amps;
    sx += x; sy += y; sxx += x * x; sxy += x * y; syy += y * y; n++;
    if (T < tmin) tmin = T;
    if (T > tmax) tmax = T;
    if (n >= ZFIT_MIN_POINTS && (tmax - tmin) >= ZFIT_MIN_SPAN_F) break;  // latest sufficient spread
  }
  if (n < ZFIT_MIN_POINTS || (tmax - tmin) < ZFIT_MIN_SPAN_F) return res;
  double denom = (double)n * sxx - sx * sx;
  if (fabs(denom) < 1e-9) return res;                      // temperature didn't actually vary
  double b = ((double)n * sxy - sx * sy) / denom;
  double c = (sy - b * sx) / (double)n;                    // intercept at x=0 → zero at T_REF
  double ssRes = syy - c * sy - b * sxy;                   // LS identity (normal-equation shortcut)
  if (ssRes < 0) ssRes = 0;
  double ssTot = syy - sy * sy / (double)n;
  double r2 = (ssTot > 1e-12) ? (1.0 - ssRes / ssTot) : 0.0;
  if (r2 < 0) r2 = 0; else if (r2 > 1) r2 = 1;
  res.ok = true;
  res.c = (float)c; res.b = (float)b; res.r2 = (float)r2;
  res.residRms = (float)sqrt(ssRes / (double)n);
  res.span = tmax - tmin; res.n = (uint16_t)n;
  return res;
}

// Median c and b across the daily-fit history ring (for the outlier gate).
static void zeroFitMedians(float *cMed, float *bMed) {
  static float cs[ZFIT_HIST_SIZE], bs[ZFIT_HIST_SIZE];    // static: keep off the loop stack
  uint16_t m = zeroFitHistCount;
  for (uint16_t i = 0; i < m; i++) {
    uint16_t idx = (zeroFitHistCount < ZFIT_HIST_SIZE) ? i
                   : (uint16_t)((zeroFitHistHead + i) % ZFIT_HIST_SIZE);
    cs[i] = zeroFitHist[idx].c; bs[i] = zeroFitHist[idx].b;
  }
  for (uint16_t i = 1; i < m; i++) { float k = cs[i]; int j = i - 1; while (j >= 0 && cs[j] > k) { cs[j + 1] = cs[j]; j--; } cs[j + 1] = k; }
  for (uint16_t i = 1; i < m; i++) { float k = bs[i]; int j = i - 1; while (j >= 0 && bs[j] > k) { bs[j + 1] = bs[j]; j--; } bs[j + 1] = k; }
  *cMed = cs[m / 2]; *bMed = bs[m / 2];
}

static void zeroFitPushHistory(uint32_t epoch, float c, float b, float r2, uint16_t sensor, uint16_t n) {
  if (!zeroFitHist) return;
  ZeroFitRecord &h = zeroFitHist[zeroFitHistHead];
  h.epoch = epoch; h.c = c; h.b = b; h.r2 = r2; h.sensor = sensor; h.n = n;
  zeroFitHistHead = (zeroFitHistHead + 1) % ZFIT_HIST_SIZE;
  if (zeroFitHistCount < ZFIT_HIST_SIZE) zeroFitHistCount++;
}

// Boot: alloc the history ring in PSRAM, restore it from /zerofit.bin (the live equation itself is
// restored separately from NVS in loadNVSData). Called from setup() near zeroLogInit().
void zeroFitInit() {
  if (!zeroFitHist) {
    zeroFitHist = (ZeroFitRecord *)ps_malloc(ZFIT_HIST_SIZE * sizeof(ZeroFitRecord));
    if (!zeroFitHist) { Serial.println("FATAL: zeroFitHist ps_malloc failed"); return; }
    memset(zeroFitHist, 0, ZFIT_HIST_SIZE * sizeof(ZeroFitRecord));
  }
  uint32_t uw = 0;
  uint32_t n = readPsramBlob(ZFIT_PATH, ZFIT_MAGIC, ZFIT_VER,
                             zeroFitHist, sizeof(ZeroFitRecord), ZFIT_HIST_SIZE, &uw, false);
  zeroFitHistCount = (uint16_t)n;
  zeroFitHistHead  = (n >= ZFIT_HIST_SIZE) ? 0 : (uint16_t)n;
  prev_zeroFitHistHead = zeroFitHistHead;
  if (n > 0) Serial.printf("zeroFitInit: restored %u fits\n", (unsigned)n);
}

// Persist the history ring to LittleFS (field-off only — caller guarantees it). Mirrors dumpZeroLog.
void dumpZeroFit() {
  if (!zeroFitHist || zeroFitHistCount == 0) return;
  uint16_t startIdx = (zeroFitHistCount < ZFIT_HIST_SIZE) ? 0 : zeroFitHistHead;
  uint32_t n = writePsramBlob(ZFIT_PATH, ZFIT_MAGIC, ZFIT_VER, 0,
                              zeroFitHist, sizeof(ZeroFitRecord),
                              ZFIT_HIST_SIZE, startIdx, zeroFitHistCount);
  if (n > 0) prev_zeroFitHistHead = zeroFitHistHead;
}

// The once-per-day math. Returns true when a fit was accepted (blended or seeded), false when it
// held the last equation (no sufficient spread / too noisy / outlier). Caller has confirmed off.
bool zeroFitCompute() {
  if (!zeroLogRing || zeroLogCount == 0) return false;
  uint32_t nowEpoch = (uint32_t)getCurrentTimestamp();

  ZFitResult fb = zeroFitRegress(ZF_BOARD, nowEpoch);
  ZFitResult fa = zeroFitRegress(ZF_ALT,   nowEpoch);
  if (!fb.ok && !fa.ok) return false;                     // no >30 °F spread on either sensor → hold

  // Auto-pick the better-correlated sensor among those with a valid block.
  int cand; ZFitResult best;
  if (fb.ok && fa.ok) { if (fa.r2 > fb.r2) { cand = ZF_ALT; best = fa; } else { cand = ZF_BOARD; best = fb; } }
  else if (fb.ok)     { cand = ZF_BOARD; best = fb; }
  else                { cand = ZF_ALT;   best = fa; }

  // Residual-noise gate — passes a clean flat fit (which R² would wrongly reject), blocks scatter.
  if (best.residRms > ZFIT_MAX_RESID_A) {
    queueConsoleMessageF("Zero-fit: held (residual %.2fA > %.2fA)", best.residRms, ZFIT_MAX_RESID_A);
    return false;
  }

  // Sticky sensor: flip only after the other sensor wins by margin for N consecutive fits.
  bool flip = false;
  if (zfValid && cand != zfSensor) {
    ZFitResult cur = (zfSensor == ZF_BOARD) ? fb : fa;
    float curR2 = cur.ok ? cur.r2 : -1.0f;
    if (best.r2 - curR2 > ZFIT_FLIP_MARGIN) {
      if (zfFlipPending == cand) zfFlipStreak++;
      else { zfFlipPending = cand; zfFlipStreak = 1; }
      if (zfFlipStreak >= ZFIT_FLIP_DAYS) flip = true;
    } else {
      zfFlipStreak = 0;
    }
    if (!flip) {
      if (!cur.ok) return false;                          // stay put but current sensor has no spread → hold
      cand = zfSensor; best = cur;
    }
  } else {
    zfFlipStreak = 0; zfFlipPending = cand;
  }

  // Outlier reject vs history median (arms once enough history exists).
  if (zeroFitHistCount >= ZFIT_OUTLIER_MINHIST) {
    float cMed, bMed; zeroFitMedians(&cMed, &bMed);
    if (fabsf(best.c - cMed) > ZFIT_OUTLIER_C_A || fabsf(best.b - bMed) > ZFIT_OUTLIER_B) {
      queueConsoleMessageF("Zero-fit: outlier held (c=%.2f med=%.2f b=%.3f med=%.3f)",
                           best.c, cMed, best.b, bMed);
      return false;
    }
  }

  // Seed (first fit or a sensor flip) or confidence-weighted EMA blend. Weight omits R² on purpose
  // so a flat offset still converges; span + point-count set how hard a good day pulls.
  bool seed = (!zfValid) || flip;
  if (seed) {
    zfC = best.c; zfB = best.b;
    if (flip) { zeroFitHistCount = 0; zeroFitHistHead = 0; prev_zeroFitHistHead = 0xFFFF; }  // history is per-sensor
  } else {
    float w = ZFIT_ALPHA * fminf(1.0f, best.span / 60.0f) * fminf(1.0f, best.n / 100.0f);
    if (w > ZFIT_W_MAX) w = ZFIT_W_MAX;
    zfC = (1.0f - w) * zfC + w * best.c;
    zfB = (1.0f - w) * zfB + w * best.b;
  }
  zfSensor = cand; zfR2 = best.r2; zfValid = 1; zfLastEpoch = nowEpoch;
  if (flip) { zfFlipStreak = 0; zfFlipPending = cand; }

  zeroFitPushHistory(nowEpoch, best.c, best.b, best.r2, (uint16_t)cand, best.n);
  dumpZeroFit();                                           // field-off guaranteed by caller
  queueConsoleMessageF("Zero-fit: %s c=%.3fA b=%.4fA/F R2=%.2f N=%u span=%.0fF",
                       (cand == ZF_ALT ? "alt" : "board"), zfC, zfB, zfR2, (unsigned)best.n, best.span);
  return true;
}

// Called every loop. Recomputes the daily fit at most 1×/24 h when engine+field are both off, and
// updates DynamicAltCurrentZero from the live equation + live temperature every pass.
void zeroFitService() {
  uint32_t now = millis();

  static uint32_t nextComputeMs = 0;                      // 0 → first attempt as soon as everything's off. Under the
                                                          // signed compare below that only holds while uptime < 24.8 d;
                                                          // reachable only if the field never rested once in 24.8 days.
  bool everythingOff = (RPM < 200) && (fieldActiveStatus == 0);
  if ((int32_t)(now - nextComputeMs) >= 0 && everythingOff) {  // signed-delta compare survives the millis() rollover
    bool accepted = zeroFitCompute();
    nextComputeMs = now + (accepted ? ZFIT_INTERVAL_MS : ZFIT_RETRY_MS);  // retry sooner if it held
  }

  // Live correction, recomputed every loop (cheap). Uses the chosen sensor's live temperature.
  float Tlive = (zfSensor == ZF_ALT) ? AlternatorTemperatureF : ambientTemp;
  float corr;
  if (!zfValid)                          corr = 0.0f;
  else if (isnan(Tlive) || isinf(Tlive)) corr = zfC;                       // temp unknown → offset only
  else                                   corr = zfC + zfB * (Tlive - ZFIT_T_REF);
  DynamicAltCurrentZero = constrain(corr, -ZFIT_CLAMP_A, ZFIT_CLAMP_A);
}

void handleAltZeroReset() {
  if (ResetDynamicAltZero == 1) {
    DynamicAltCurrentZero = 0.0f;
    zfValid = 0; zfC = 0.0f; zfB = 0.0f; zfR2 = 0.0f; zfSensor = ZF_BOARD;
    zfLastEpoch = 0; zfFlipStreak = 0; zfFlipPending = ZF_BOARD;
    zeroFitHistCount = 0; zeroFitHistHead = 0; prev_zeroFitHistHead = 0xFFFF;
    fsTakeLock();
    LittleFS.remove(ZFIT_PATH);  // raw call — fsExists() would re-take the non-recursive fsMutex and block 5 s
    fsReleaseLock();
    ResetDynamicAltZero = 0;  // Clear the momentary flag
    queueConsoleMessage("Zero correction: learned fit + history cleared");
  }
}
void calculateChargeTimes() {
  static unsigned long lastCalcTime = 0;
  unsigned long now = millis();
  if (now - lastCalcTime < INA_SLOW_INTERVAL_MS) return;
  lastCalcTime = now;

  float currentAmps = getBatteryCurrent();  // this is for battery state

  if (currentAmps > 0.01) {  // charging
    float currentSoC = SOC_percent / 100.0;
    float remainingCapacity = BatteryCapacity_Ah * (100.0 - currentSoC) / 100.0;
    timeToFullChargeMin = (int)(remainingCapacity / currentAmps * 60.0);
    timeToFullDischargeMin = -999;           // Not applicable while charging
  } else if (currentAmps < -0.01) {          // discharging
    float currentSoC = SOC_percent / 100.0;
    float availableCapacity = BatteryCapacity_Ah * currentSoC / 100.0;
    timeToFullDischargeMin = (int)(availableCapacity / (-currentAmps) * 60.0);
    timeToFullChargeMin = -999;  // Not applicable while discharging
  } else {
    // No significant current flow
    timeToFullChargeMin = -999;
    timeToFullDischargeMin = -999;
  }
}
void calculateThermalStress() {
  unsigned long now = millis();

  if (lastThermalUpdateTime == 0) {
    lastThermalUpdateTime = now;
    return;  // Skip first calculation
  }

  if (now - lastThermalUpdateTime < THERMAL_UPDATE_INTERVAL) {
    return;
  }

  if (RPM < 0.0f || RPM > 10000.0f || isnan(RPM) || isinf(RPM)) {
    return;  // Invalid RPM
  }


  if (now - lastThermalUpdateTime < THERMAL_UPDATE_INTERVAL) {
    return;
  }

  if (isnan(TempToUse) || isinf(TempToUse)) {
    return;  // Temperature sensor not initialized yet
  }

  // DS18B20 valid range: -67°F to +257°F
  if (TempToUse < -67.0f || TempToUse > 257.0f) {
    return;
  }

  float elapsedSeconds = (now - lastThermalUpdateTime) / 1000.0f;
  lastThermalUpdateTime = now;

  float T_winding_F = TempToUse + WindingTempOffset;
  float T_bearing_F = TempToUse + WindingTempOffset;  // temporarily using same offset for simplicity until we see how this thing works
  float T_brush_F = TempToUse + WindingTempOffset;    // temporarily using same offset for simplicity until we see how this thing works

  // Sanity check component temperatures (allow some headroom for offset)
  if (T_winding_F < -100.0f || T_winding_F > 400.0f || T_bearing_F < -100.0f || T_bearing_F > 400.0f || T_brush_F < -100.0f || T_brush_F > 400.0f) {
    return;  // Don't accumulate damage from obviously bad readings
  }

  float Alt_RPM = RPM * PulleyRatio;

  // Calculate individual component lives
  float T_winding_K = (T_winding_F - 32.0f) * 5.0f / 9.0f + 273.15f;
  float L_insul = L_REF_INSUL * exp(EA_INSULATION / BOLTZMANN_K * (1.0f / T_winding_K - 1.0f / T_REF_K));
  L_insul = min(L_insul, 10000.0f);

  float L_grease_base = L_REF_GREASE * pow(0.5f, (T_bearing_F - 158.0f) / 18.0f);
  float L_grease = L_grease_base * (6000.0f / max(Alt_RPM, 100.0f));
  L_grease = min(L_grease, 10000.0f);

  float temp_factor = 1.0f + 0.0025f * (T_brush_F - 150.0f);
  float L_brush = (L_REF_BRUSH * 6000.0f / max(Alt_RPM, 100.0f)) / max(temp_factor, 0.1f);
  L_brush = min(L_brush, 10000.0f);

  // Calculate damage rates (damage per hour)
  float insul_damage_rate = 1.0f / L_insul;
  float grease_damage_rate = 1.0f / L_grease;
  float brush_damage_rate = 1.0f / L_brush;

  float hours_elapsed = elapsedSeconds / 3600.0f;
  CumulativeInsulationDamage += insul_damage_rate * hours_elapsed;
  CumulativeGreaseDamage += grease_damage_rate * hours_elapsed;
  CumulativeBrushDamage += brush_damage_rate * hours_elapsed;

  CumulativeInsulationDamage = constrain(CumulativeInsulationDamage, 0.0f, 1.0f);
  CumulativeGreaseDamage = constrain(CumulativeGreaseDamage, 0.0f, 1.0f);
  CumulativeBrushDamage = constrain(CumulativeBrushDamage, 0.0f, 1.0f);

  InsulationLifePercent = (1.0f - CumulativeInsulationDamage) * 100.0f;
  GreaseLifePercent = (1.0f - CumulativeGreaseDamage) * 100.0f;
  BrushLifePercent = (1.0f - CumulativeBrushDamage) * 100.0f;

  // Calculate predicted life hours (minimum of current rates)
  float min_damage_rate = max({ insul_damage_rate, grease_damage_rate, brush_damage_rate });
  PredictedLifeHours = 1.0f / min_damage_rate;

  if (PredictedLifeHours > 5000.0f) {
    LifeIndicatorColor = 0;  // Green
  } else if (PredictedLifeHours > 1000.0f) {
    LifeIndicatorColor = 1;  // Yellow
  } else {
    LifeIndicatorColor = 2;  // Red
  }
}


//INA228 functions to compensate for lack of library features related to ALERT pin
uint16_t readINA228AlertRegister(uint8_t i2cAddress) {
  Wire.beginTransmission(i2cAddress);
  Wire.write(0x0B);                                     // DIAG_ALRT register
  if (Wire.endTransmission(false) != 0) return 0xFFFF;  // keep repeated start
  if (Wire.requestFrom(i2cAddress, (uint8_t)2) != 2) return 0xFFFF;
  return (Wire.read() << 8) | Wire.read();
}
bool clearINA228AlertLatch(uint8_t i2cAddress) {
  Wire.beginTransmission(i2cAddress);
  Wire.write(0x00);  // CONFIG register
  Wire.endTransmission(false);
  Wire.requestFrom(i2cAddress, (uint8_t)2);
  if (Wire.available() < 2) return false;
  uint16_t config = (Wire.read() << 8) | Wire.read();
  // Set ALERT_LATCH_CLEAR bit (bit 3)
  config |= 0x0008;
  Wire.beginTransmission(i2cAddress);
  Wire.write(0x00);
  Wire.write(config >> 8);
  Wire.write(config & 0xFF);
  return Wire.endTransmission() == 0;
}

// Returns 0..OV_HIST_BINS-1, or -1 if raw bus voltage is at/below Bulk (struct + bin layout
// documented at the OvTelemetry declaration in Xregulator.ino).
int ovHistBin(float rawV) {
  float k = (float)SYSTEM_VOLTAGE_CLASS / 12.0f;
  float v = rawV - BulkVoltage;  // volts above Bulk (raw)
  if (v <= 0.0f) return -1;
  float fineW = 0.2f * k, fineTop = 3.6f * k;  // 18 fine bins; top ≈18V at a 14.4 Bulk
  if (v < fineTop) return (int)(v / fineW);  // 0..17
  float c = (v - fineTop) / (1.0f * k);
  if (c < (float)OV_HIST_COARSE_BINS) return OV_HIST_FINE_BINS + (int)c;  // 18..29
  return OV_HIST_BINS - 1;  // 30 overflow
}

// Called once per control tick in every mode — field state does not gate it, because external
// charge sources (solar, shore) can drive bus OV with the field off and must still be captured.
void updateOvHistogram(float rawV, uint32_t nowMs) {
  static uint32_t lastMs = 0;
  static int lastBin = -1;
  uint32_t dt = (lastMs == 0) ? 0 : (nowMs - lastMs);
  if (dt > 1000) dt = 1000;  // clamp: never attribute a boot/stall gap to a bin
  lastMs = nowMs;
  int bin = ovHistBin(rawV);
  if (bin < 0) {
    lastBin = -1;
    return;
  }
  g_ovTel.timeMs[bin] += dt;
  if (bin != lastBin) g_ovTel.events[bin]++;
  lastBin = bin;
}

// BATTERY_TYPE is free text from Vessel Info, so substring-match the common lithium spellings.
// Shared by the INA228 threshold rule and the SoC voltage seed.
bool batteryIsLithium() {
  String bt = BATTERY_TYPE;
  bt.toLowerCase();
  return bt.indexOf("lifepo") >= 0 || bt.indexOf("lithium") >= 0 ||
         bt.indexOf("li-ion") >= 0 || bt.indexOf("liion") >= 0 || bt.indexOf("lfp") >= 0;
}

void updateINA228OvervoltageThreshold() {
  if (INADisconnected != 0) {
    queueConsoleMessageF("INA228: Cannot update threshold - chip not connected");
    return;
  }

  // Programs the ALERT comparator from VoltageHardwareLimit — a persisted user setting (top rung
  // of the OV ladder), no derivation. Guidance: 0.2 V x class/12 below the battery's BMS
  // charge-disconnect voltage, one rung ABOVE the software instant cut (AlternatorHardShutdownV),
  // so software gets first shot at every rung and the pin is purely the electrical MCU-hang /
  // software-failed-to-protect backstop (~0 trips normally). The compare uses the chip's averaged
  // value (SLOWALERT); while the field is on the chip runs fast mode (4-sample avg, ~4.3 ms), so
  // the pin is a near-instant electrical cut, not a slow layer. It can never be the timed layer:
  // AVG applies to ALL active inputs, so slowing the bus average would drag CURRENT register
  // updates to the same cadence and kill the 5 ms load-dump/CV sampling (TI SLYS021 — the alert
  // path has no independent delay/deglitch feature).

  const double LSB = 0.003125;                                           // 3.125 mV/LSB
  uint16_t thresholdLSB = (uint16_t)(VoltageHardwareLimit / LSB + 0.5);  // Round instead of truncate

  queueConsoleMessageF("INA228 threshold calc: %.3fV / %.6f = %u LSB", VoltageHardwareLimit, LSB, (unsigned)thresholdLSB);
  queueConsoleMessageF("INA228 effective threshold: %.3fV", (double)thresholdLSB * LSB);

  INA.setBusOvervoltageTH(thresholdLSB);
  INA.setBusUndervoltageTH(0x0000);  // under-voltage threshold deliberately disabled

  INA.setDiagnoseAlertBit(INA228_DIAG_SLOW_ALERT);        // SLOW_ALERT: compare uses the averaged value
  INA.clearDiagnoseAlertBit(INA228_DIAG_ALERT_LATCH);     // Transparent mode - alerts clear when condition clears
  INA.clearDiagnoseAlertBit(INA228_DIAG_ALERT_POLARITY);  // Active-low open-drain (default)
  INA.setDiagnoseAlertBit(INA228_DIAG_BUS_OVER_LIMIT);    // Enable BUSOL reporting

  uint16_t readback_BOVL = INA.getBusOvervoltageTH();
  uint16_t readback_BUVL = INA.getBusUndervoltageTH();

  queueConsoleMessageF("INA228 readback: BOVL=0x%04X (%.3fV), BUVL=0x%04X",
                       (unsigned)readback_BOVL, (double)readback_BOVL * LSB, (unsigned)readback_BUVL);

  if (readback_BOVL != thresholdLSB) {
    queueConsoleMessageF("WARNING: BOVL write failed - expected 0x%04X, got 0x%04X",
                         (unsigned)thresholdLSB, (unsigned)readback_BOVL);
  }
  if (readback_BUVL != 0) {
    queueConsoleMessageF("WARNING: BUVL not cleared - still 0x%04X", (unsigned)readback_BUVL);
  }

  queueConsoleMessageF(
    "INA228: Overvoltage threshold=%.2fV | SLOW_ALERT ON (averaged compare; ~4.3ms in fast charging mode)",
    VoltageHardwareLimit);
}

void checkWebFilesExist() {
  const char *gzFiles[] = {
    "/index.html.gz",
    "/styles.css.gz",
    "/script.js.gz",
    "/uPlot.min.css.gz",
    "/uPlot.iife.min.js.gz"
  };

  int missingCount = 0;
  Serial.println("=== CHECKING WEB FILES ===");

  if (!ensureWebFS()) {
    Serial.println("ERROR: Web filesystem not mounted!");
    queueConsoleMessage("CRITICAL: Web filesystem mount failed!");
    return;
  }

  for (size_t i = 0; i < sizeof(gzFiles) / sizeof(gzFiles[0]); i++) {
    if (!webFS.exists(gzFiles[i])) {
      Serial.printf("MISSING: %s\n", gzFiles[i]);
      missingCount++;
    } else {
      Serial.printf("Found: %s\n", gzFiles[i]);
    }
  }

  if (missingCount > 0) {
    Serial.println("===============================================");
    Serial.println("ERROR: Missing web interface files!");
    Serial.printf("Missing %d critical files from web partition\n", missingCount);
    Serial.println("Web files should be in factory_fs or prod_fs partition");
    Serial.println("Currently using: " + String(usingFactoryWebFiles ? "factory_fs" : "prod_fs"));
    Serial.println("===============================================");

    queueConsoleMessage("CRITICAL: " + String(missingCount) + " web files missing from web partition!");

  } else {
    Serial.println("All critical web files found");
    Serial.println("Using web files from: " + String(usingFactoryWebFiles ? "factory_fs" : "prod_fs"));
    queueConsoleMessage("Web interface files verified OK");
  }
}

// Inter-sample gap meter for a slow ADS channel (CH0 battV, CH2 RPM). Called on each
// VALID reading; records the gap since the previous valid reading. "worst" resets on
// Reset Peak Values. ch=0 → battV, ch=2 → RPM. Dashboard: Live Data → ESP32.
static inline void adsGapUpdate(uint8_t ch, uint32_t now) {
  uint32_t *prev  = (ch == 0) ? &ch0GapPrevMs  : &ch2GapPrevMs;
  uint16_t *last  = (ch == 0) ? &ch0GapLastMs  : &ch2GapLastMs;
  uint16_t *worst = (ch == 0) ? &ch0GapWorstMs : &ch2GapWorstMs;
  uint32_t g = (*prev == 0) ? 0 : (now - *prev);
  if (g > 65535u) g = 65535u;                  // clamp (a >65s gap means the channel is dead)
  *last = (uint16_t)g;
  if ((uint16_t)g > *worst) *worst = (uint16_t)g;
  if (!gpio4IsLow) {  // field-on split — excludes field-off background passes that stretch gaps harmlessly
    uint16_t *fw = (ch == 0) ? &ch0GapFieldOnWorstMs : &ch2GapFieldOnWorstMs;
    if ((uint16_t)g > *fw) *fw = (uint16_t)g;
  }
  *prev = now;
}


void ReadAnalogInputs() {
  // Outer wrapper — ft_rai_total captures the true worst-case duration including
  // I2C timeouts. Individual section timers triangulate which sub-block is
  // responsible when ft_rai_total spikes.
  TIMED_CALL(ft_rai_total, _ReadAnalogInputs_inner());
}

void _ReadAnalogInputs_inner() {

  // ── INA228 Battery Monitor ────────────────────────────────────────────────
  static unsigned long lastINARead_local = 0;

  // Speed mode switch — fast (4.3ms) when field gate is open, slow (1054ms) when off.
  // Writes two I2C registers only on transition; no cost on steady state.
  if (INADisconnected == 0) {
    bool fieldGateOpen = !gpio4IsLow;
    if (fieldGateOpen && !inaFastModeActive) {
      INA.setAverage(1);                    // 4 samples (register 1)
      INA.setBusVoltageConversionTime(4);   // 540µs
      INA.setShuntVoltageConversionTime(4); // 540µs — total update: 4×1080µs ≈ 4.3ms
      IBV_filtered = IBV;                   // reseed EMA so CV loop starts clean
      inaReadInterval = INA_FAST_INTERVAL_MS;
      inaFastModeActive = true;
    } else if (!fieldGateOpen && inaFastModeActive) {
      INA.setAverage(4);                    // 128 samples (register 4)
      INA.setBusVoltageConversionTime(7);   // 4120µs
      INA.setShuntVoltageConversionTime(7); // 4120µs — total update: 128×8240µs ≈ 1054ms
      inaReadInterval = INA_SLOW_INTERVAL_MS;
      inaFastModeActive = false;
    }
  }

  if (millis() - lastINARead_local >= inaReadInterval) {
    if (INADisconnected == 0) {
      lastINARead_local = millis();
      // Track intervals only while INA228 is in fast mode (~5ms cadence).
      // Gating on inaFastModeActive (not fieldActiveStatus) avoids logging a
      // slow-mode ~1054ms read as a false outlier during gate transitions.
      // Reset windowed stats on each fast-mode rising edge.
      static bool lastFastMode = false;
      bool nowFastMode = inaFastModeActive;
      if (nowFastMode && !lastFastMode) resetINA228IntervalWindows();
      lastFastMode = nowFastMode;
      if (nowFastMode) recordINA228Interval(lastINARead_local);

      // ft_rai_ina228 / AnalogReadTime aliases: worstWindow + lastCall updated by macro
      TIMED_CALL(ft_rai_ina228, ([&]() {
                   try {
                     uint32_t inaBusT0 = micros();
                     IBV = INA.getBusVoltage();
                     ShuntVoltage_mV = INA.getShuntVoltage() * 1000;
                     uint32_t inaBusDt = micros() - inaBusT0;            // time in JUST the two Wire reads
                     if (inaBusDt > inaBusReadWorstUs) inaBusReadWorstUs = inaBusDt;
                     if (inaBusDt > 15000UL) inaBusSlowCount++;          // ≥1 Wire-timeout's worth = bus stall

                     if (!isnan(IBV) && IBV > 5.0 && IBV < fminf(85.0f, 70.0f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f)) && !isnan(ShuntVoltage_mV)) {  // garbage-reject ceiling scales with class, capped at INA228 85V full scale
                       Bcur = (ShuntResistanceMicroOhm > 0) ? (ShuntVoltage_mV * 1000.0f / ShuntResistanceMicroOhm) : 0.0f;
                       Bcur = Bcur + BatteryCOffset;
                       if (InvertBattAmps == 1) {
                         Bcur = -Bcur;
                       }
                       // battery current always from INA228
                       if (AutoShuntGainCorrection == 1) {
                         Bcur = Bcur * DynamicShuntGainFactor;
                       }
                       BatteryCurrent_scaled = Bcur * 100;
                       MARK_FRESH(IDX_IBV);
                       MARK_FRESH(IDX_BCUR);

                       // Accumulate every valid INA228 sample for the CSV1 window aggregation.
                       aggIbv.add(IBV);
                       aggBcur.add(Bcur);

                       // IBV EMA — used by getFiltV() and CV loop error terms
                       // dBcur/dt — positive value = load dump (loads disconnected, OV risk)
                       {
                         uint32_t nowIna = millis();
                         static bool ibv_ema_init = false;
                         static uint32_t lastIbvEmaMs = 0;
                         if (!ibv_ema_init) {
                           IBV_filtered = IBV;
                           g_cvKdFiltV = IBV;
                           ibv_ema_init = true;
                         } else {
                           float dt_f = fmaxf(1.0f, (float)(nowIna - lastIbvEmaMs));
                           float alpha = dt_f / (VoltageFilterTC + dt_f);
                           IBV_filtered = alpha * IBV + (1.0f - alpha) * IBV_filtered;
                           // Dedicated D-term voltage EMA — separate TC so tuning the D term's noise floor
                           // never shifts the shared IBV_filtered (stage machine). CvKdVoltFiltTC=0 -> raw IBV.
                           float alphaKd = dt_f / (fmaxf(CvKdVoltFiltTC, 1.0f) + dt_f);
                           g_cvKdFiltV = alphaKd * IBV + (1.0f - alphaKd) * g_cvKdFiltV;
                         }
                         lastIbvEmaMs = nowIna;
                         ibvFreshFlag = true;

                         // No shunt -> the derivative is the slope of INA input noise. Leave g_dBcur_dt
                         // at 0 and keep the slew roll empty so neither the CSV2 trace nor the
                         // load-dump gate-tuning readout shows a number that reads like a measurement.
                         static float bcurPrev = 0.0f;
                         static uint32_t bcurPrevMs = 0;
                         if (HAS_BATT_SHUNT) {
                           if (bcurPrevMs > 0) {
                             uint32_t dtBcur = nowIna - bcurPrevMs;
                             if (dtBcur >= 3 && dtBcur < 2000) {
                               g_dBcur_dt = (Bcur - bcurPrev) / ((float)dtBcur * 0.001f);
                               rollUpdate(ROLL_LDSLEW, g_dBcur_dt);   // load-dump slew gate-tuning readout
                             }
                           }
                         } else {
                           g_dBcur_dt = 0.0f;
                         }
                         bcurPrev = Bcur;
                         bcurPrevMs = nowIna;
                       }

                       // Fold this fast Bcur sample (+ co-sampled MeasuredAmps) into the measured
                       // filtered-ripple capture (§3.1). ALWAYS-ON: peak IExcessTau-filtered pk-pk per
                       // FaCell for the map + Protections plot; the 3-level test ring appends only when armed.
                       faFiltRippleUpdate(Bcur, MeasuredAmps, RPM);

                       if (IBV > IBVMax)              { IBVMax              = IBV; }
                       if (IBV > PeakVoltage_AllTime) { PeakVoltage_AllTime = IBV; }
                       if (IBV < MinVoltage)          { MinVoltage          = IBV; }
                       if (IBV < MinVoltage_AllTime)  { MinVoltage_AllTime  = IBV; }
                       wmIgnUpdate(wmIgn_IBV,  IBV);   // ignition-cycle watermarks (lo + hi)
                       // No shunt -> leave the battery-current watermark at NAN so the records row
                       // renders empty instead of the hi/lo excursions of an unconnected input.
                       if (HAS_BATT_SHUNT) wmIgnUpdate(wmIgn_Bcur, Bcur);
                     } else {
                       ina228ErrorCount++;   // implausible read — dropped (no MARK_FRESH)
                     }

                   } catch (...) {
                     ina228ErrorCount++;   // exception path — visible on dashboard "I2C Bus Health"
                     // no MARK_FRESH; one rare Serial line kept for USB debug
                     static unsigned long lastINAFailureWarning = 0;
                     if (millis() - lastINAFailureWarning > 10000) {
                       Serial.println("INA228 read failed");
                       lastINAFailureWarning = millis();
                     }
                   }
                 }()));
    }
  }

  // ── ADS1115 non-blocking state machine ───────────────────────────────────
  // Sequence {1,0,1,2,1,3} → CH1 = 3/6 samples (fresh CH1 measured 5–25ms apart, assume ~30ms)
  // ADS_WAIT uses time-based 3ms delay — no isConversionDone() I²C poll.
  // Back-to-back trigger fires next conversion at end of ADS_READ_RESULT,
  // saving one loop() call per step. Falls back to ADS_IDLE if <2ms elapsed.
  // Full 6-step cycle ≈ 14ms; CH0/CH2/CH3 each update every ~14ms.
  //
  // ft_rai_ads_state measures cost per state step (not a full logical read cycle),
  // which is the correct unit for a non-blocking state machine.
  if (ADS1115Disconnected != 0) {
    static unsigned long lastADSWarning = 0;
    if (millis() - lastADSWarning > 10000) {
      queueConsoleMessage("theADS1115 was not connected and triggered a return");
      lastADSWarning = millis();
    }
    return;  // Early exit - no MARK_FRESH calls
  }

  unsigned long now = millis();

  TIMED_CALL(ft_rai_ads_state, ([&]() {
               switch (adsState) {

                 case ADS_IDLE:
                   adsTriggeredChannel = adsCurrentChannel;
                   adc.setMux(adsMuxCodes[adsTriggeredChannel]);
                   adc.triggerConversion();
                   adsStateEntered = millis();  // capture AFTER triggerConversion() write completes
                   adsState = ADS_WAIT;
                   break;

                 case ADS_WAIT:
                   // Time-based ready check — eliminates isConversionDone() I²C poll (requestFrom blindspot)
                   // 860 SPS = 1.16ms/conversion; 3ms gives millis() granularity margin
                   if (now - adsStateEntered >= ADS_CONVERSION_MS) {
                     adsState = ADS_READ_RESULT;
                   } else if (now - adsStateEntered > ADS_TIMEOUT_MS) {
                     queueConsoleMessage("ADS1115 timeout ch" + String(adsTriggeredChannel));
                     adsState = ADS_IDLE;  // retry same channel
                   }
                   break;

                 case ADS_READ_RESULT:
                   {
                     // Read conversion register directly - we have already confirmed OS bit is set,
                     // so we bypass adc.getConversion() which would busy-wait/block the loop unnecessarily
                     Wire.beginTransmission(0x48);
                     Wire.write(ADS1115_REG_POINTER_CONVERT);
                     uint32_t _ads_t0 = (uint32_t)esp_timer_get_time();
                     uint8_t endStatus = Wire.endTransmission(false);
                     uint32_t _ads_t1 = (uint32_t)esp_timer_get_time();
                     uint8_t bytesReceived = Wire.requestFrom((uint8_t)0x48, (uint8_t)2);
                     uint32_t _ads_t2 = (uint32_t)esp_timer_get_time();

                     // Diagnostic: log when either I2C call stalls >5ms total
                     if (_ads_t2 - _ads_t0 > 5000) {
                       adsSlowReadCount++;
                       adsLastSlowEndTxUs   = _ads_t1 - _ads_t0;
                       adsLastSlowReqFromUs = _ads_t2 - _ads_t1;
                       Serial.printf("ADS slow read #%lu ch%d: endTx=%luus reqFrom=%luus total=%luus\n",
                                     (unsigned long)adsSlowReadCount, adsTriggeredChannel,
                                     (unsigned long)adsLastSlowEndTxUs,
                                     (unsigned long)adsLastSlowReqFromUs,
                                     (unsigned long)(_ads_t2 - _ads_t0));
                     }

                     bool readOK = (endStatus == 0) && (bytesReceived == 2) && (Wire.available() >= 2);

                     if (readOK) {
                       Raw = (int16_t)((Wire.read() << 8) | Wire.read());
                     } else {
                       // Drain whatever is in the buffer to leave I2C bus clean
                       while (Wire.available()) Wire.read();
                       adsI2CErrorCount++;
                       queueConsoleMessage("ADS1115 I2C read error ch" + String(adsTriggeredChannel) + " endStatus=" + String(endStatus) + " bytes=" + String(bytesReceived));
                     }

                     // Track consecutive I²C failures — 5 in a row = chip gone mid-run
                     {
                       static uint8_t adsConsecFails = 0;
                       if (readOK) {
                         adsConsecFails = 0;
                       } else {
                         adsConsecFails++;
                         if (adsConsecFails >= 5) {
                           ADS1115Disconnected = 1;
                           queueConsoleMessage("ADS1115 declared disconnected after 5 consecutive I2C failures");
                           adsConsecFails = 0;
                         }
                       }
                     }

                     // CRITICAL: Use adsTriggeredChannel, NOT adsSequenceIndex!
                     if (readOK) {
                       switch (adsTriggeredChannel) {
                         case 0:
                           Channel0V = Raw / 32768.0 * 6.144 * 21.0401;  // divider 1,000,000Ω / 49.9kΩ, scale ≈21.0401
                           BatteryV = Channel0V;
                           if (BatteryV > 5.0 && BatteryV < fminf(125.0f, 70.0f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f))) {  // Sanity check — ceiling scales with class, capped near the ADS divider range
                             MARK_FRESH(IDX_BATTERY_V);
                             battVFreshFlag = true;
                             adsGapUpdate(0, now);  // CH0 battV inter-sample gap meter
                             aggBattV.add(BatteryV);
                           }
                           break;

                           // case 1:
                           //   //Clamp On Sensor QNHCK1-21
                           //   Channel1V = Raw / 32768.0 * 6.144 * 2.0;  // divider is 768kΩ / 768kΩ, ratio = 0.5, scale = 2.000
                           //   MeasuredAmps = (Channel1V - 2.5) * 100;   // alternator current

                           //   if (InvertAltAmps == 1) {
                           //     MeasuredAmps = MeasuredAmps * -1;  // swap sign if necessary
                           //   }
                           //   MeasuredAmps = MeasuredAmps - AlternatorCOffset;
                           //   // Apply dynamic zero correction only when enabled
                           //   if (AutoAltCurrentZero == 1) {
                           //     MeasuredAmps = MeasuredAmps - DynamicAltCurrentZero;
                           //   }

                           //   if (MeasuredAmps > -500 && MeasuredAmps < 500) {  // Sanity check
                           //     MARK_FRESH(IDX_MEASURED_AMPS);
                           //     ch1FreshFlag = true;  // Signal PID that fresh current data is available
                           //   }


                         case 1:
                           ch1_record(millis());
                           //Clamp On Sensor QNHCK1-21
                           Channel1V = Raw / 32768.0 * 6.144 * 2.0;  // divider is 768kΩ / 768kΩ, ratio = 0.5, scale = 2.000
                           // Scale factor: sensor outputs 2.5V±2V over its rated range
                           // 0=±200A→100 A/V, 1=±300A→150 A/V, 2=±500A→250 A/V
                           {
                             static const float kAmpScale[]  = {100.0f, 150.0f, 250.0f};
                             static const float kSanityLim[] = {250.0f, 370.0f, 600.0f};
                             int rIdx = (AmpSensorRange >= 0 && AmpSensorRange <= 2) ? AmpSensorRange : 1;
                             MeasuredAmps = (Channel1V - 2.5f) * kAmpScale[rIdx];

                           if (InvertAltAmps == 1) {
                             MeasuredAmps = MeasuredAmps * -1;
                           }
                           MeasuredAmps = MeasuredAmps - AlternatorCOffset;
                           if (AutoAltCurrentZero == 1) {
                             MeasuredAmps = MeasuredAmps - DynamicAltCurrentZero;
                           }

                           if (MeasuredAmps > -kSanityLim[rIdx] && MeasuredAmps < kSanityLim[rIdx]) {  // Sanity check
                             MARK_FRESH(IDX_MEASURED_AMPS);
                             wmIgnUpdate(wmIgn_amps, MeasuredAmps);  // ignition-cycle watermark
                             aggAltCur.add(MeasuredAmps);
                             ch1FreshFlag = true;  // Signal PID that fresh current data is available
                             // ── EMA filter ─────────────────────────────────────────────────────────
                             // Output PID EMA (OutputPIDFilterTC → g_pidI_filtered) — the signal the CC
                             // current loop acts on. iExcess uses its own EMA on raw MeasuredAmps into
                             // mExcessEma (see the MA block below), not this one.
                             {
                               static bool amps_filter_init = false;
                               static uint32_t lastAmpsFilterMs = 0;
                               if (!amps_filter_init) {
                                 g_pidI_filtered = MeasuredAmps;
                                 amps_filter_init = true;
                               } else {
                                 float dt_f = fmaxf(1.0f, (float)(now - lastAmpsFilterMs));
                                 float alpha_pid = dt_f / (OutputPIDFilterTC + dt_f);
                                 g_pidI_filtered = alpha_pid * MeasuredAmps + (1.0f - alpha_pid) * g_pidI_filtered;
                               }
                               lastAmpsFilterMs = now;
                             }
                           }

                           // ── Current amplitude ring + moving averages ──────────────────────────────
                           // Runs every confirmed CH1 hit. Feeds MA and dI/dt to the fast current
                           // rise supervisor in AdjustFieldLearnMode.
                           {
                             uint32_t now_i = millis();
                             iAmpRing[iAmpHead] = { now_i, MeasuredAmps };
                             iAmpHead = (iAmpHead + 1) % I_RING_SIZE;
                             if (iAmpCount < I_RING_SIZE) iAmpCount++;

                             // MA(N) for the Output Current PID signal (iExcess no longer uses an MA —
                             // the EMA detector reads raw MeasuredAmps into its own mExcessEma).
                             {
                               int n_pid = OutputPIDMA_N < (int)iAmpCount ? OutputPIDMA_N : (int)iAmpCount;
                               if (n_pid < 1) n_pid = 1;
                               float sum_pid = 0.0f;
                               for (int k = 0; k < n_pid; k++) {
                                 uint8_t idx = (iAmpHead + I_RING_SIZE - 1 - k) % I_RING_SIZE;
                                 sum_pid += iAmpRing[idx].val;
                               }
                               g_pidMA_N = sum_pid / (float)n_pid;
                             }
                           }

                           // ── cvLog: write here, tied to actual CH1 sample arrival ──────────────────
                           // Control-state globals (cv_I, Icv, etc.) reflect the previous control tick —
                           // one-tick lag is acceptable for analysis.
                           cvLog_tick(millis());

                           if (MeasuredAmps > MeasuredAmpsMax)         { MeasuredAmpsMax         = MeasuredAmps; }
                           if (MeasuredAmps > MeasuredAmpsMax_AllTime) { MeasuredAmpsMax_AllTime = MeasuredAmps; }
                           }  // end AmpSensorRange scale block
                           break;

                         case 2:
                           Channel2V = Raw / 32768.0 * 2 * 6.144 * RPMScalingFactor;
                           RPM = Channel2V;
                           // DEBUG — RPM-glitch hunt (the phantom "Engine STARTED RPM=3321" with a clean
                           // plot). A real start ramps; a lone jump from ~0 to a high value in one read is
                           // the glitch. Logs the raw ADS conversion code so we can read the actual bad
                           // value, plus transport status: endStatus=0/bytes=2 means a "clean" read of a
                           // bad conversion (incomplete or cross-channel mux carryover), not a bus error.
                           // Remove once root cause is identified.
                           {
                             static float prevRPMdbg = 0.0f;
                             if (prevRPMdbg < 100.0f && RPM > 1000.0f) {
                               queueConsoleMessageF("RPM GLITCH: RPM=%d rawCode=%d (0x%04X) scale=%d endStatus=%d bytes=%d mux=0x%04X",
                                                    (int)RPM, (int)Raw, (uint16_t)Raw, RPMScalingFactor,
                                                    (int)endStatus, (int)bytesReceived, (unsigned)adsMuxCodes[adsTriggeredChannel]);
                               Serial.printf("RPM GLITCH: RPM=%d rawCode=%d (0x%04X) scale=%d endStatus=%d bytes=%d mux=0x%04X\n",
                                             (int)RPM, (int)Raw, (uint16_t)Raw, RPMScalingFactor,
                                             (int)endStatus, (int)bytesReceived, (unsigned)adsMuxCodes[adsTriggeredChannel]);
                             }
                             prevRPMdbg = RPM;
                           }
                           // Lone-sample glitch rejection: a real start ramps across >=2 samples (~30ms
                           // apart via the ADS round-robin); a single <100 -> >1000 jump is field-PWM
                           // pickup during shutdown or an ADS mux-carryover artifact, never a real engine.
                           // Hold the last accepted value for one sample and accept on the second
                           // consecutive high reading — keeps phantoms out of RPM, RPMMax, watermarks,
                           // aggregates, and CSVs, and stops a lone spike resetting rpmZeroSinceMs.
                           {
                             static float rpmLastAccepted = 0.0f;
                             static bool rpmJumpPending = false;
                             if (rpmLastAccepted < 100.0f && RPM > 1000.0f && !rpmJumpPending) {
                               rpmJumpPending = true;
                               RPM = rpmLastAccepted;
                             } else {
                               rpmJumpPending = false;
                               rpmLastAccepted = RPM;
                             }
                           }
                           if (RPM > RPMMax)         { RPMMax         = RPM; }
                           if (RPM > RPMMax_AllTime) { RPMMax_AllTime = RPM; }
                           if (RPM < 100) {
                             RPM = 0;
                           }
                           if (RPM >= 0 && RPM < 10000) {  // Sanity check
                             MARK_FRESH(IDX_RPM);
                             wmIgnUpdate(wmIgn_RPM, RPM);  // ignition-cycle watermark
                             adsGapUpdate(2, now);  // CH2 RPM inter-sample gap meter
                             aggRpm.add(RPM);
                           }
                           break;

                         case 3:
                           // Channel3V = plain volts at the ADC pin. ADS1115 gain ±6.144V FSR;
                           // real signal range is 0-3.3V per docs/hardware/analoginputsADS1115.md.
                           Channel3V = Raw / 32768.0 * 6.144;

                           // Disconnect detection: per the hardware doc, the lowest legitimate
                           // V_node is 0.134V (NTC at -40°C). The board ships with a ground jumper
                           // installed by default, so most units have Channel 3 tied to GND and
                           // V_node = 0V. A 0.05V floor cleanly distinguishes "no usable sensor"
                           // (grounded or floating-leakage) from any real reading.
                           if (Channel3V < 0.05f) {
                             temperatureThermistor = -99;
                             break;
                           }

                           float thermC = thermistorTempC(Channel3V);
                           if (thermC <= -98.5f) {  // -99.0f error sentinel (°C domain) — converting it would yield -146°F, which passes every != -99 check downstream
                             temperatureThermistor = -99;
                             break;
                           }
                           temperatureThermistor = (int)(thermC * 1.8f + 32.0f);  // °C → °F

                           if (temperatureThermistor > 500) {
                             temperatureThermistor = -99;
                           }
                           if (Channel3V > 0 && Channel3V < 3.3f) {  // legitimate ADC range
                             MARK_FRESH(IDX_CHANNEL3V);
                             aggCh3.add(Channel3V);
                           }
                           if (temperatureThermistor > -58 && temperatureThermistor < 392) {  // °F bounds
                             MARK_FRESH(IDX_THERMISTOR_TEMP);

                             if (temperatureThermistor > MaxTemperatureThermistor)         MaxTemperatureThermistor         = temperatureThermistor;
                             if (temperatureThermistor > MaxTemperatureThermistor_AllTime) MaxTemperatureThermistor_AllTime = temperatureThermistor;
                           }
                           break;
                       }
                     }

                     // Sequence — CH1 gets 3 of 6 slots, worst-case gap = 2 conversion cycles
                     static const uint8_t adsSeq[] = { 1, 0, 1, 2, 1, 3 };
                     static const uint8_t adsSeqLen = 6;

                     static uint8_t adsSeqIdx = 0;
                     adsSeqIdx = (adsSeqIdx + 1) % adsSeqLen;
                     adsCurrentChannel = adsSeq[adsSeqIdx];

                     // Back-to-back trigger: fire next conversion immediately if ≥2ms has
                     // elapsed since this conversion was triggered. At 860SPS (1.16ms) and
                     // ~3ms loop cadence this is always true, saving one loop() call per
                     // channel. Falls back to ADS_IDLE if called too soon (protects WiFi).
                     if (millis() - adsStateEntered >= 2) {
                       adsTriggeredChannel = adsCurrentChannel;
                       adc.setMux(adsMuxCodes[adsTriggeredChannel]);
                       adc.triggerConversion();
                       adsStateEntered = millis();
                       adsState = ADS_WAIT;
                     } else {
                       adsState = ADS_IDLE;
                     }
                     break;
                   }
               }
             }()));


  // ── BMP388 forced-mode non-blocking state machine ────────────────────────
  // x32 pressure, x2 temperature, IIR filter enabled
  // Trigger conversion, return immediately, poll until ready, burst-read once.
  // ft_rai_bmp_state measures cost per state step.
  {
    enum BMPState {
      BMP_IDLE,
      BMP_WAIT_READY
    };

    static BMPState bmpState = BMP_IDLE;
    static uint32_t bmpLastCycleMs = 0;
    static uint32_t bmpTriggerMs = 0;
    static bool bmpFirstReadDone = false;

    float temperature, pressure, altitude;

    TIMED_CALL(ft_rai_bmp_state, ([&]() {
                 switch (bmpState) {

                   case BMP_IDLE:
                     // 4s cycle: max age 4s, one missed read 8s — both inside the 10s DATA_TIMEOUT.
                     // At the old 8s a single missed read hit 16s, which dropped the battery-temp
                     // gain derate to 1.0, skipped the cold-charge lockout check, and grayed the
                     // header board-temp readout until the next good read.
                     if (!BMP388Disconnected && millis() - bmpLastCycleMs >= 4000) {
                       bmp388.startForcedConversion();  // returns immediately if sensor is in sleep
                       bmpTriggerMs = millis();
                       bmpState = BMP_WAIT_READY;
                     }
                     break;

                   case BMP_WAIT_READY:
                     // Non-blocking poll. Returns 0 until data ready, then does the short read.
                     if (bmp388.getMeasurements(temperature, pressure, altitude)) {

                       float newPressure = pressure;  // already hPa / mbar in this library
                       float newTemp = temperature;  // keep in °C; conversion to °F happens at assignment below

                       if (!bmpFirstReadDone) {
                         bmpFirstReadDone = true;  // optional discard of first sample
                       } else {
                         if (isfinite(newPressure) && newPressure > 800.0f && newPressure < 1100.0f) {
                           baroPressure = newPressure;
                           MARK_FRESH(IDX_BARO_PRESSURE);
                         }

                         if (isfinite(newTemp) && newTemp > -40.0f && newTemp < 85.0f) {  // BMP388 rated range in °C
                           ambientTemp = newTemp * 1.8f + 32.0f;  // convert °C to °F for storage
                           MARK_FRESH(IDX_AMBIENT_TEMP);
                           // Board temp drifted → refresh the battery-temp gain derate (the board is the
                           // batteryTempF() stand-in when no battery measurement qualifies; buildTickSnapshot
                           // carries the same trigger for whichever source is active). Gated to a ≥5°F move
                           // (~2.8°C ≈ 6% gain change at 0.024/°C — finer steps are noise vs the board
                           // stand-in's own error) and only when a commissioned reference exists.
                           // recomputeCvGains is already called cross-core from Core-0 web handlers; this
                           // task is also Core 0.
                           static float lastDerateTempF = NAN;
                           if (battTempDerateEnable && !isnan(CommissionTempF) &&
                               (isnan(lastDerateTempF) || fabsf(ambientTemp - lastDerateTempF) >= 5.0f)) {
                             lastDerateTempF = ambientTemp;
                             recomputeCvGains();
                           }
                         }
                       }

                       bmpLastCycleMs = millis();
                       bmpState = BMP_IDLE;
                     } else if (millis() - bmpTriggerMs > 120) {
                       // x32/x2 conversion should be done long before this; timeout just prevents getting stuck
                       static uint32_t lastBMPTimeoutMsg = 0;
                       if (millis() - lastBMPTimeoutMsg > 60000) {
                         Serial.println("BMP388 forced conversion timeout");
                         queueConsoleMessage("BMP388 forced conversion timeout");
                         lastBMPTimeoutMsg = millis();
                       }
                       bmpLastCycleMs = millis();
                       bmpState = BMP_IDLE;
                     }
                     break;
                 }
               }()));
  }

  // IMU FIFO drain runs in drainIMUFifo(), called from loop() after AdjustFieldLearnMode()
}

// ── IMU FIFO Drain ────────────────────────────────────────────────────────────
// Called from loop() AFTER AdjustFieldLearnMode() so a Wire stall on the IMU
// cannot delay control-critical voltage/current reads or the field control decision.
// No collision-avoidance needed here — a slow drain only delays telemetry, not control.
// Increment the shared I2C error counter and auto-disable the IMU if errors
// exceed 10 within any 60-second window.  Called from both error paths inside
// drainIMUFifo() so the threshold is shared across Get_FIFO_Num_Samples and
// Get_FIFO_Sample failures.
static void imuRecordI2CError() {
  static uint32_t windowErrors = 0;
  static unsigned long windowStart = 0;
  imu_i2c_error_count++;
  unsigned long now = millis();
  if (windowStart == 0 || now - windowStart >= 60000) {
    windowStart = now;
    windowErrors = 0;
  }
  windowErrors++;
  if (windowErrors >= 10 && imuEnabled) {
    imuEnabled = false;
    queueConsoleMessageF("IMU disabled: %u I2C errors in 60s (total %u). Check IMU wiring.", windowErrors, imu_i2c_error_count);
  }
}

void drainIMUFifo() {
  if (!imuEnabled || (millis() - lastIMUPoll < IMU_POLL_INTERVAL)) return;
  lastIMUPoll = millis();

  TIMED_CALL(ft_rai_imu, ([&]() {
               uint16_t fifo_samples = 0;

               // Read FIFO status (quick operation, ~50 µs)
               if (imu.Get_FIFO_Num_Samples(&fifo_samples) != LSM6DSOX_OK) {
                 imuRecordI2CError();
                 return;
               }

               uint8_t fifo_ovr = 0;
               if (imu.Get_FIFO_Overrun_Status(&fifo_ovr) == LSM6DSOX_OK && fifo_ovr) {
                 imu_fifo_overrun_count++;
                 static bool first_overrun = true;
                 static unsigned long last_overrun_msg = 0;
                 if (first_overrun) {
                   queueConsoleMessageF("IMU FIFO overrun detected (increase drain rate)");
                   first_overrun = false;
                   last_overrun_msg = millis();
                 } else if (millis() - last_overrun_msg > 300000) {
                   queueConsoleMessageF("IMU FIFO overruns: %u total", imu_fifo_overrun_count);
                   last_overrun_msg = millis();
                 }
               }

               if (fifo_samples == 0) return;

               uint16_t samples_to_read = (fifo_samples > MAX_FIFO_DRAIN_PER_POLL)
                                            ? MAX_FIFO_DRAIN_PER_POLL
                                            : fifo_samples;

               uint32_t imuFetchT0 = micros();
               if (imu.Get_FIFO_Sample(fifoBuffer, samples_to_read) != LSM6DSOX_OK) {
                 imuRecordI2CError();
                 return;
               }
               MARK_FRESH(IDX_IMU);   // stamped HERE, not at poll entry: the age must mean "a sample landed", not "we tried"
               uint32_t imuFetchDt = micros() - imuFetchT0;   // time in JUST the FIFO Wire read
               if (imuFetchDt > imuFifoFetchWorstUs) {
                 imuFifoFetchWorstUs = imuFetchDt;
                 imuFifoWorstSamples = samples_to_read;   // bytes-in-flight at the worst fetch (×7 = bytes)
               }

               // Timestamp the batch end, then backdate each sample by its nominal interval.
               // The sensor's internal timestamp register (0x40-0x42, 25µs resolution) would give
               // true sample times but requires FIFO timestamp batching config. Using nominal ODR
               // intervals instead - error is negligible (<1% at these rates) and avoids the
               // batch_timestamp=same_value problem which caused dt_us=0 for all but the first
               // sample, effectively stalling the complementary filter.
               uint32_t batch_end_us = micros();
               constexpr uint32_t ACCEL_INTERVAL_US = 9615;  // 1,000,000 / 104 Hz
               constexpr uint32_t GYRO_INTERVAL_US = 19231;  // 1,000,000 / 52 Hz

               uint16_t accel_in_batch = 0, gyro_in_batch = 0;
               for (uint16_t i = 0; i < samples_to_read; i++) {
                 uint8_t tag = fifoBuffer[i * 7] >> 3;
                 if (tag == TAG_SENSOR_ACCEL) accel_in_batch++;
                 else if (tag == TAG_SENSOR_GYRO) gyro_in_batch++;
               }

               uint16_t accel_idx = 0, gyro_idx = 0;
               for (uint16_t i = 0; i < samples_to_read; i++) {
                 uint8_t raw_tag = fifoBuffer[i * 7];
                 uint8_t tag_sensor = raw_tag >> 3;

                 int16_t raw_x = (int16_t)((fifoBuffer[i * 7 + 2] << 8) | fifoBuffer[i * 7 + 1]);
                 int16_t raw_y = (int16_t)((fifoBuffer[i * 7 + 4] << 8) | fifoBuffer[i * 7 + 3]);
                 int16_t raw_z = (int16_t)((fifoBuffer[i * 7 + 6] << 8) | fifoBuffer[i * 7 + 5]);

                 if (tag_sensor == TAG_SENSOR_ACCEL) {
                   int16_t x = raw_x * ACCEL_X_SIGN;
                   int16_t y = raw_y * ACCEL_Y_SIGN;
                   int16_t z = raw_z * ACCEL_Z_SIGN;
                   uint32_t ts = batch_end_us - (uint32_t)(accel_in_batch - 1 - accel_idx) * ACCEL_INTERVAL_US;
                   pushAccelSample(x, y, z, ts);
                   accel_idx++;

                 } else if (tag_sensor == TAG_SENSOR_GYRO) {
                   int16_t x = raw_x * GYRO_X_SIGN;
                   int16_t y = raw_y * GYRO_Y_SIGN;
                   int16_t z = raw_z * GYRO_Z_SIGN;
                   uint32_t ts = batch_end_us - (uint32_t)(gyro_in_batch - 1 - gyro_idx) * GYRO_INTERVAL_US;
                   pushGyroSample(x, y, z, ts);
                   gyro_idx++;

                 } else if (tag_sensor == TAG_SENSOR_TEMP) {
                   // Temperature samples explicitly ignored — not batching temp in FIFO

                 } else {
                   imu_unknown_tag_count++;
                   static bool unknown_tag_warned = false;
                   if (!unknown_tag_warned) {
                     queueConsoleMessageF("IMU: unknown tag_sensor=%d (raw=0x%02X)", tag_sensor, raw_tag);
                     unknown_tag_warned = true;
                   }
                 }
               }
             }()));
}

void ReadAnalogInputs_Fake() {
  static unsigned long lastINARead_local2 = 0;

  if (millis() - lastINARead_local2 >= INA_SLOW_INTERVAL_MS) {  // could go down to 600 here, but this logic belongs in Loop anyway
    lastINARead_local2 = millis();

    static unsigned long lastFakeUpdate = 0;
    static float fakeVoltage = 13.2;
    static float fakeCurrent = -85.0;
    static float fakeRPM = 1000;
    static float fakeTemp = 45.0;

    // GPS motion simulation
    static float fakeLat = 0.0;     // Equator
    static float fakeLon = -140.0;  // Open Pacific
    static float fakeHeading = random(0, 360);
    static float fakeCOG = fakeHeading + random(-30, 30);
    static float fakeSOG = 3.0 + (random(-150, 150) / 100.0);  // 1.5–4.5 kt initial band
    static float fakeApparentWindSpeed = 12.0;
    static float fakeApparentWindAngle = 45.0;
    fakeLon += 0.000375f;
    if (fakeLon > 180.0f) fakeLon -= 360.0f;
    // small lat oscillation near equator
    // 0.1 deg amplitude, period controlled by time
    // this uses fakeLon as the "time" driver, no new vars
    fakeLat = 0.1f * sin(fakeLon * 0.1f);

    LatitudeNMEA = fakeLat;
    LongitudeNMEA = fakeLon;
    SatelliteCountNMEA = 6 + random(0, 10);  // 6–15 sats
    MARK_FRESH(IDX_LATITUDE_NMEA);
    MARK_FRESH(IDX_LONGITUDE_NMEA);
    MARK_FRESH(IDX_SATELLITE_COUNT);

    fakeSOG += (random(-80, 80) / 100.0);  // ±0.8 kt per update
    if (fakeSOG < 0.5) fakeSOG = 0.5;
    if (fakeSOG > 12.0) fakeSOG = 12.0;
    SOGNMEA = fakeSOG;
    MARK_FRESH(IDX_SOG_NMEA);

    updateSustainedSpeed(fakeSOG);


    fakeCOG += (random(-40, 40) / 10.0);  // ±4° per update
    if (fakeCOG < 0) fakeCOG += 360;
    if (fakeCOG >= 360) fakeCOG -= 360;
    COGNMEA = fakeCOG;
    MARK_FRESH(IDX_COG_NMEA);

    fakeHeading += (random(-60, 60) / 10.0);  // ±6° per update
    if (fakeHeading < 0) fakeHeading += 360;
    if (fakeHeading >= 360) fakeHeading -= 360;
    HeadingNMEA = fakeHeading;
    MARK_FRESH(IDX_HEADING_NMEA);

    fakeApparentWindSpeed += (random(-40, 40) / 10.0);  // ±4 kt per update
    if (fakeApparentWindSpeed < 0.0) fakeApparentWindSpeed = 0.0;
    if (fakeApparentWindSpeed > 35.0) fakeApparentWindSpeed = 35.0;
    ApparentWindSpeedNMEA = fakeApparentWindSpeed;
    MARK_FRESH(IDX_APPARENT_WIND_SPEED);

    fakeApparentWindAngle += (random(-120, 120) / 10.0);  // ±12° per update
    if (fakeApparentWindAngle < 0.0) fakeApparentWindAngle += 360.0;
    if (fakeApparentWindAngle >= 360.0) fakeApparentWindAngle -= 360.0;
    ApparentWindAngleNMEA = fakeApparentWindAngle;
    MARK_FRESH(IDX_APPARENT_WIND_ANGLE);

    // Fake battery voltage, 11.5–15.0V per-cell-scaled by class (unscaled 12V numbers would
    // fail isVoltageSensorPlausible and trip the disagreement fault on a 24/36/48V-configured unit)
    float simK = (float)SYSTEM_VOLTAGE_CLASS / 12.0f;
    fakeVoltage += (random(-80, 80) / 100.0) * simK;  // ±0.8 V per update at 12V
    if (fakeVoltage < 11.5 * simK) fakeVoltage = 11.5 * simK;
    if (fakeVoltage > 15.0 * simK) fakeVoltage = 15.0 * simK;
    BatteryV = fakeVoltage;
    IBV = fakeVoltage + (random(-30, 30) / 100.0) * simK;             // ±0.30 V at 12V
    VictronVoltage = fakeVoltage + (random(-50, 50) / 100.0) * simK;  // ±0.50 V at 12V
    MARK_FRESH(IDX_BATTERY_V);
    MARK_FRESH(IDX_IBV);
    MARK_FRESH(IDX_VICTRON_VOLTAGE);


    if (IBV > PeakVoltage_AllTime) { PeakVoltage_AllTime = IBV; }
    if (IBV > IBVMax)              { IBVMax              = IBV; }
    if (IBV < MinVoltage)          { MinVoltage          = IBV; }
    if (MinVoltage_AllTime == 0.0 || IBV < MinVoltage_AllTime) { MinVoltage_AllTime = IBV; }


    // Fake alternator current
    //fakeCurrent += (random(-50, 50) / 100.0);  // ±0.5 A per update
    //if (fakeCurrent < -140.0) fakeCurrent = -140.0;
    // if (fakeCurrent > 150.0) fakeCurrent = 150.0;
    fakeCurrent = 10;
    MeasuredAmps = fakeCurrent * (InvertAltAmps ? -1 : 1);
    ch1FreshFlag = true;                                    // Signal PID that fresh current data is available
    MARK_FRESH(IDX_MEASURED_AMPS);

    if (MeasuredAmps > MeasuredAmpsMax)         { MeasuredAmpsMax         = MeasuredAmps; }
    if (MeasuredAmps > MeasuredAmpsMax_AllTime) { MeasuredAmpsMax_AllTime = MeasuredAmps; }


    static float fakeBattCurrent = 0;
    if (SOC_percent > 99) {
      fakeBattCurrent = -100.0;
    }
    if (SOC_percent < 99) {
      fakeBattCurrent = 100.0;
    }
    //fakeBattCurrent += (random(-80, 80) / 10.0);  // ±8 A per update
    //if (fakeBattCurrent < -180.0) fakeBattCurrent = -180.0;
    //if (fakeBattCurrent > 180.0) fakeBattCurrent = 180.0;
    fakeBattCurrent = 100;
    Bcur = fakeBattCurrent * (InvertBattAmps ? -1 : 1);
    BatteryCurrent_scaled = Bcur * 100;
    VictronCurrent = Bcur + (random(-80, 80) / 10.0);  // ±8 A offset
    MARK_FRESH(IDX_BCUR);
    MARK_FRESH(IDX_VICTRON_CURRENT);

    // Fake RPM
    //fakeRPM += (random(-3, 3));  // ±3 rpm per update
    // if (fakeRPM < 800) fakeRPM = 800;
    // if (fakeRPM > 3200) fakeRPM = 3200;
    fakeRPM = 1000;
    RPM = fakeRPM;
    MARK_FRESH(IDX_RPM);

    if (RPM > RPMMax)         { RPMMax         = RPM; }
    if (RPM > RPMMax_AllTime) { RPMMax_AllTime = RPM; }

    // Fake temperatures — fakeTemp walks in °C (10–110), converted to °F at storage to match
    // the °F-native globals (previously stored raw °C, making the alternator read ~100°F
    // hotter than its own thermistor in sim).
    fakeTemp += (random(-30, 30) / 10.0);
    if (fakeTemp < 10) fakeTemp = 10;
    if (fakeTemp > 110) fakeTemp = 110;
    temperatureThermistor = (int)(fakeTemp * 1.8f + 32.0f);
    ambientTemp = (fakeTemp - (5 + random(-20, 20) / 10.0)) * 1.8f + 32.0f;
    // Floored well clear of MinChargeTempF (40°F): the walk above can reach ~37°F, and once
    // IDX_AMBIENT_TEMP is marked fresh the cold-charge lockout acts on it and would kill the
    // field in sim. Without the MARK_FRESH the board-temp and baro readouts stay grayed forever.
    if (ambientTemp < 60.0f) ambientTemp = 60.0f;

    float tempC = fakeTemp + 10 + random(-20, 20) / 10.0;
    AlternatorTemperatureF = tempC * 9.0 / 5.0 + 32.0;
    MARK_FRESH(IDX_THERMISTOR_TEMP);
    MARK_FRESH(IDX_ALTERNATOR_TEMP);
    MARK_FRESH(IDX_AMBIENT_TEMP);

    if (temperatureThermistor > MaxTemperatureThermistor)             MaxTemperatureThermistor             = temperatureThermistor;
    if (temperatureThermistor > MaxTemperatureThermistor_AllTime)     MaxTemperatureThermistor_AllTime     = temperatureThermistor;
    if (AlternatorTemperatureF > MaxAlternatorTemperatureF)           MaxAlternatorTemperatureF            = AlternatorTemperatureF;
    if (AlternatorTemperatureF > MaxAlternatorTemperatureF_AllTime)   MaxAlternatorTemperatureF_AllTime    = AlternatorTemperatureF;


    // Solar energy simulation (fake Victron data)
    static unsigned long lastSolarUpdate = 0;
    if (millis() - lastSolarUpdate >= 2000) {  // Update every 2 seconds like real VE.Direct
      lastSolarUpdate = millis();

      // Solar power 100–800W band
      static float fakeSolarPower = 300.0;
      fakeSolarPower += (random(-300, 300));  // ±300 W per update
      if (fakeSolarPower < 100) fakeSolarPower = 100;
      if (fakeSolarPower > 800) fakeSolarPower = 800;

      float elapsedSeconds = 2.0;  // matches the 2s update gate above
      float solarEnergyDelta_Wh = (fakeSolarPower * elapsedSeconds) / 3600.0f;

      // Mirror the fake solar onto the live dashboard fields + leaderboard watermark
      VictronSolarPower_W = fakeSolarPower;
      VictronSolarVoltage_V = 18.0f + (random(-200, 200) / 100.0f);
      VictronSolarCurrent_A = (VictronSolarVoltage_V > 1.0f) ? (VictronSolarPower_W / VictronSolarVoltage_V) : 0.0f;
      VictronChargeState = 3; VictronMPPTMode = 2; VictronError = 0;   // Bulk / Active MPPT / OK
      VictronYieldToday_kWh += solarEnergyDelta_Wh / 1000.0f;
      if (fakeSolarPower > VictronMaxPowerToday_W) VictronMaxPowerToday_W = fakeSolarPower;
      MARK_FRESH(IDX_VICTRON_SOLAR);
      if (VictronSolarPower_W > solar_power_max_alltime_w) solar_power_max_alltime_w = VictronSolarPower_W;

      static float solarEnergyAccumulator = 0.0f;
      static float solarEnergyAccumulator_AllTime = 0.0f;

      solarEnergyAccumulator += solarEnergyDelta_Wh;
      solarEnergyAccumulator_AllTime += solarEnergyDelta_Wh;

      if (solarEnergyAccumulator >= 1.0f) {
        SolarChargedEnergy += (int)solarEnergyAccumulator;
        solarEnergyAccumulator -= (int)solarEnergyAccumulator;
      }

      if (solarEnergyAccumulator_AllTime >= 1.0f) {
        SolarChargedEnergy_AllTime += (int)solarEnergyAccumulator_AllTime;
        solarEnergyAccumulator_AllTime -= (int)solarEnergyAccumulator_AllTime;
      }
    }

    // Fake barometric pressure (wide, stormy)
    baroPressure = 1013 + (random(-200, 200) / 10.0);  // ±20 hPa
    MARK_FRESH(IDX_BARO_PRESSURE);

    // Fake other channels
    Channel0V = BatteryV;
    Channel1V = 2.5 + (MeasuredAmps / 50.0);
    Channel2V = RPM / RPMScalingFactor;
    Channel3V = 50 + (random(-60, 60));  //  -10–110 range
    MARK_FRESH(IDX_CHANNEL3V);
  }

  // ===== IMU FAKE DATA GENERATION =====
  if (imuEnabled) {
    static float fakeHeelAngle = 0;
    static float fakePitchAngle = 0;
    static float fakeHeelRate = 0;
    static float fakePitchRate = 0;
    static unsigned long lastIMUFake = 0;

    unsigned long now = millis();
    float dt = (now - lastIMUFake) / 1000.0f;
    if (lastIMUFake == 0) dt = 0.1f;  // First call
    lastIMUFake = now;

    // Advance motion phases every call so heel/pitch track real time even when pushes are throttled
    static float rollPhase = 0;
    rollPhase += dt * 0.5f;                                              // ~0.5 rad/s = ~12s period
    fakeHeelAngle = 8.0f * sin(rollPhase) + (random(-30, 30) / 10.0f);  // ±8° ±3° noise
    fakeHeelRate = 8.0f * 0.5f * cos(rollPhase);                        // Derivative

    static float pitchPhase = 0;
    pitchPhase += dt * 0.3f;                                               // ~0.3 rad/s = ~20s period
    fakePitchAngle = 4.0f * sin(pitchPhase) + (random(-20, 20) / 10.0f);  // ±4° ±2° noise
    fakePitchRate = 4.0f * 0.3f * cos(pitchPhase);                        // Derivative

    // Rate-limit sample push to ~10 Hz — avoids flooding ring buffer at fast main loop rates
    static unsigned long lastIMUPush = 0;
    if (now - lastIMUPush < 100) return;
    lastIMUPush = now;

    // Slam event (~2% per push call at 10 Hz = ~1 per 5 seconds, rough conditions)
    // Amplitude kept within ±2g sensor range to avoid int16_t overflow
    static float slamDecay = 0;
    if (random(0, 100) < 2 && slamDecay == 0) {
      slamDecay = 0.6f + (random(0, 30) / 100.0f);  // 0.6–0.9g spike
    }
    if (slamDecay > 0) {
      slamDecay -= dt * 8.0f;
      if (slamDecay < 0) slamDecay = 0;
    }

    // Convert angles to accelerations (sensor frame)
    // When boat heels, gravity vector rotates
    float heel_rad = fakeHeelAngle * PI / 180.0f;
    float pitch_rad = fakePitchAngle * PI / 180.0f;

    // Gravity components in sensor frame (simplified, no yaw)
    float ax_g = -sin(pitch_rad);                 // Forward/aft tilt
    float ay_g = sin(heel_rad) * cos(pitch_rad);  // Port/starboard tilt
    float az_g = cos(heel_rad) * cos(pitch_rad);  // Vertical (1g when level)

    az_g += slamDecay;

    // Add wave-induced vertical motion (~0.3g peak, ~8s period)
    static float wavePhase = 0;
    wavePhase += dt * 0.8f;
    az_g += 0.3f * sin(wavePhase) + (random(-20, 20) / 100.0f);

    // Convert to raw sensor counts — constrain prevents int16_t overflow on spikes
    // ACCEL_SCALE = 0.000061 g/LSB → 1g = 16393 LSB, full range ±32767 = ±2.0g
    int16_t raw_ax = (int16_t)constrain((int32_t)(ax_g / ACCEL_SCALE), -32767, 32767);
    int16_t raw_ay = (int16_t)constrain((int32_t)(ay_g / ACCEL_SCALE), -32767, 32767);
    int16_t raw_az = (int16_t)constrain((int32_t)(az_g / ACCEL_SCALE), -32767, 32767);

    // Gyro rates (already in dps)
    // GYRO_SCALE = 0.070 dps/LSB → 1 dps = 14.3 LSB
    float gx_dps = fakePitchRate + (random(-50, 50) / 100.0f);  // ±0.5 dps noise
    float gy_dps = fakeHeelRate + (random(-50, 50) / 100.0f);
    float gz_dps = (random(-30, 30) / 100.0f);  // Small yaw rate noise

    int16_t raw_gx = (int16_t)(gx_dps / GYRO_SCALE);
    int16_t raw_gy = (int16_t)(gy_dps / GYRO_SCALE);
    int16_t raw_gz = (int16_t)(gz_dps / GYRO_SCALE);

    // Engine + sea-state vibration noise scaled with RPM
    // At 1500 RPM: ±2500 LSB ≈ ±150 mg per axis (≈90 mg RMS)
    // At idle/stopped: minimum 200 LSB keeps background sea-state noise present
    int engineNoise = (int)constrain(RPM / 0.6f, 200.0f, 6000.0f);

    // Push samples to ring buffers — simulates a batch from the real 104 Hz FIFO
    uint32_t timestamp_us = micros();

    // 5 accel samples at ~9.6ms spacing = 104Hz claimed rate
    for (int i = 0; i < 5; i++) {
      int16_t jitter_ax = (int16_t)constrain((int32_t)raw_ax + random(-engineNoise, engineNoise), -32767, 32767);
      int16_t jitter_ay = (int16_t)constrain((int32_t)raw_ay + random(-engineNoise, engineNoise), -32767, 32767);
      int16_t jitter_az = (int16_t)constrain((int32_t)raw_az + random(-engineNoise, engineNoise), -32767, 32767);
      pushAccelSample(jitter_ax, jitter_ay, jitter_az, timestamp_us + i * 9615);
    }

    // Gyro at 52 Hz (one sample per ~20ms)
    pushGyroSample(raw_gx, raw_gy, raw_gz, timestamp_us + 19000);
  }
}


bool validateWebFile(const char *filename) {
  esp_task_wdt_reset();  // Feed watchdog before file operations

  Serial.printf("\n=== VALIDATING WEB FILE: %s ===\n", filename);
  Serial.flush();

  File file = webFS.open(filename, "r");
  if (!file) {
    Serial.printf("ERROR: %s - FILE NOT FOUND\n", filename);
    Serial.flush();
    return false;
  }

  size_t fileSize = file.size();
  file.close();
  Serial.printf("File size: %d bytes\n", fileSize);
  Serial.flush();

  // Size sanity check only — the real end-to-end integrity guarantee is the OTA SHA256
  // verified at download time.
  bool valid = (fileSize >= 8);

  if (valid) {
    Serial.printf("SUCCESS: %s - present (%d bytes) ✓\n", filename, fileSize);
  } else {
    Serial.printf("FAILED: %s - missing or too small (%d bytes) ✗\n", filename, fileSize);
  }
  Serial.printf("=== VALIDATION COMPLETE: %s ===\n\n", filename);
  Serial.flush();

  esp_task_wdt_reset();  // Feed watchdog after file operations

  return valid;
}

bool validateWebFilesystem() {
  Serial.println("\n--- Validating all web files ---");
  Serial.flush();

  bool result = true;
  result = validateWebFile("/index.html.gz") && result;
  result = validateWebFile("/styles.css.gz") && result;
  result = validateWebFile("/script.js.gz") && result;
  result = validateWebFile("/uPlot.min.css.gz") && result;
  result = validateWebFile("/uPlot.iife.min.js.gz") && result;

  if (result) {
    Serial.println("--- All web files validated successfully ---");
  } else {
    Serial.println("--- One or more web files FAILED validation (see above) ---");
  }
  Serial.flush();

  return result;
}
bool ensureWebFS() {
  esp_task_wdt_reset();  // Feed watchdog at start

  static bool webMounted = false;
  if (webMounted) {
    Serial.println("Web filesystem already mounted - skipping");
    return true;
  }

  Serial.println("\n========================================");
  Serial.println("=== MOUNTING WEB FILESYSTEM ===");
  Serial.println("========================================");
  Serial.flush();

  const esp_partition_t *running_partition = esp_ota_get_running_partition();
  const esp_partition_t *factory_partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);

  bool runningFromFactory = (running_partition == factory_partition);

  Serial.printf("Running from: %s\n", runningFromFactory ? "FACTORY partition" : "OTA partition");
  Serial.flush();

  if (runningFromFactory) {
    Serial.println("Attempting to mount factory_fs...");
    Serial.flush();

    if (webFS.begin(true, "/web", 10, "factory_fs")) {
      Serial.println("factory_fs mounted successfully");
      Serial.flush();
      Serial.println("Starting validation of web files...");
      Serial.flush();

      if (validateWebFilesystem()) {
        usingFactoryWebFiles = true;
        Serial.println("\n✓✓✓ SUCCESS: factory_fs validated and mounted ✓✓✓");
        Serial.flush();
        webMounted = true;
        cacheWebAssets();
        if (wdtMainTaskSubscribed) esp_task_wdt_reset();
        return true;
      } else {
        Serial.println("\n✗✗✗ CRITICAL ERROR: Factory web files FAILED validation ✗✗✗");
        Serial.println("This should never happen - factory partition is corrupted!");
        Serial.flush();
        delay(5000);
        return false;
      }
    } else {
      Serial.println("✗✗✗ CRITICAL ERROR: Failed to mount factory_fs ✗✗✗");
      Serial.flush();
      delay(5000);
      return false;
    }

  } else {
    Serial.println("Attempting to mount prod_fs (OTA web files)...");
    Serial.flush();

    if (webFS.begin(true, "/web", 10, "prod_fs")) {
      Serial.println("prod_fs mounted successfully");
      Serial.flush();
      Serial.println("Starting validation of OTA web files...");
      Serial.flush();

      if (validateWebFilesystem()) {
        usingFactoryWebFiles = false;
        Serial.println("\n✓✓✓ SUCCESS: prod_fs validated and mounted ✓✓✓");
        Serial.flush();
        webMounted = true;
        cacheWebAssets();
        if (wdtMainTaskSubscribed) esp_task_wdt_reset();
        return true;

      } else {
        Serial.println("\n✗✗✗ VALIDATION FAILED: prod_fs is corrupted! ✗✗✗");
        Serial.println("OTA web files are corrupted or incomplete");
        Serial.println("Triggering factory rollback in 5 seconds...");
        Serial.println("You will see which file(s) failed validation above ^^^");
        Serial.flush();

        webFS.end();
        queueConsoleMessage("CRITICAL: prod_fs corrupted, forcing factory rollback");
        clearOta0Record();  // the factory boot must not bring this install back

        delay(5000);

        Serial.println("NOW REBOOTING TO FACTORY FIRMWARE...");
        Serial.flush();
        delay(1000);

        esp_ota_mark_app_invalid_rollback_and_reboot();
      }
    }

    Serial.println("\n--- prod_fs mount failed, trying factory_fs fallback ---");
    Serial.flush();

    if (webFS.begin(true, "/web", 10, "factory_fs")) {
      Serial.println("factory_fs mounted successfully (fallback)");
      Serial.flush();
      Serial.println("Starting validation of factory web files (fallback)...");
      Serial.flush();

      if (validateWebFilesystem()) {
        usingFactoryWebFiles = true;
        Serial.println("\n✓✓✓ SUCCESS: factory_fs validated and mounted (fallback from prod_fs) ✓✓✓");
        Serial.flush();
        webMounted = true;
        cacheWebAssets();
        if (wdtMainTaskSubscribed) esp_task_wdt_reset();
        return true;

      } else {
        Serial.println("\n✗✗✗ CATASTROPHIC ERROR: Both web filesystems are corrupted! ✗✗✗");
        Serial.println("prod_fs: FAILED");
        Serial.println("factory_fs: FAILED");
        Serial.println("System cannot continue - halting in 10 seconds");
        Serial.flush();
        webFS.end();
        delay(10000);
        return false;
      }
    }

    Serial.println("✗✗✗ ERROR: Could not mount any web filesystem ✗✗✗");
    Serial.flush();
    delay(5000);
    return false;
  }
}
void switchToFactoryWebFiles() {
  webFS.end();
  usingFactoryWebFiles = true;
  if (!webFS.begin(true, "/web", 10, "factory_fs")) {
    Serial.println("ERROR: Failed to mount factory web files");
    events.send("CRITICAL: Failed to mount factory web files", "console", millis());
  } else {
    Serial.println("Switched to factory web files");
    events.send("Switched to factory web files for recovery", "console", millis());
  }
}
void performFactoryReset() {
  Serial.println("FACTORY RESET: Switching to factory firmware and web files");
  events.send("FACTORY RESET: Switching to factory firmware and web files", "console", millis());

  const esp_partition_t *factory_partition =
    esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);

  if (factory_partition) {
    esp_ota_set_boot_partition(factory_partition);
    switchToFactoryWebFiles();
    Serial.println("Factory reset complete - restarting in 3 seconds");
    events.send("Factory reset complete - restarting in 3 seconds", "console", millis());
    delay(3000);
    ESP.restart();
  } else {
    Serial.println("ERROR: Factory partition not found");
    events.send("ERROR: Factory partition not found", "console", millis());
  }
}

void calculateDerivedMetrics() {
  // ===== TRUE WIND CALCULATION =====
  // Skip if the bus already provided true wind directly (WindSpeed PGN handler populated
  // it from a True_North / Magnetic / True_boat reference). Only derive from apparent
  // when no upstream true source is available.
  if (!IS_STALE(IDX_APPARENT_WIND_SPEED) && !IS_STALE(IDX_APPARENT_WIND_ANGLE) &&
      (!IS_STALE(IDX_STW_NMEA) || !IS_STALE(IDX_SOG_NMEA)) && !IS_STALE(IDX_HEADING_NMEA) &&
      IS_STALE(IDX_TRUE_WIND_SPEED)) {

    float awaRad = ApparentWindAngleNMEA * PI / 180.0;

    // True wind = Apparent wind - Boat motion vector
    float awsX = ApparentWindSpeedNMEA * sin(awaRad);  // Apparent wind X component
    float awsY = ApparentWindSpeedNMEA * cos(awaRad);  // Apparent wind Y component

    // Subtract boat speed (boat speed is in direction of heading, so Y component).
    // Prefer speed-through-water (STW) — it removes current so true wind isn't contaminated;
    // fall back to SOG when no water-speed log is present.
    float boatSpeedTW = (!IS_STALE(IDX_STW_NMEA) && !isnan(STWNMEA)) ? STWNMEA : SOGNMEA;
    float twsX = awsX;
    float twsY = awsY - boatSpeedTW;

    TrueWindSpeedNMEA = sqrt(twsX * twsX + twsY * twsY);
    TrueWindAngleNMEA = atan2(twsX, twsY) * 180.0 / PI;

    if (TrueWindAngleNMEA < 0) TrueWindAngleNMEA += 360.0;

    MARK_FRESH(IDX_TRUE_WIND_SPEED);
    MARK_FRESH(IDX_TRUE_WIND_ANGLE);
  } else if (IS_STALE(IDX_TRUE_WIND_SPEED) && IS_STALE(IDX_TRUE_WIND_ANGLE)) {
    // No upstream true PGN AND no apparent-derived path — clear to NAN.
    TrueWindSpeedNMEA = NAN;
    TrueWindAngleNMEA = NAN;
  }
  // (else: bus is providing true wind directly via WindSpeed PGN handler — leave it alone)

  // ===== LEEWAY CALCULATION =====
  if (!IS_STALE(IDX_HEADING_NMEA) && !IS_STALE(IDX_COG_NMEA)) {
    LeewayNMEA = HeadingNMEA - COGNMEA;

    if (LeewayNMEA > 180.0) LeewayNMEA -= 360.0;
    if (LeewayNMEA < -180.0) LeewayNMEA += 360.0;

    MARK_FRESH(IDX_LEEWAY);
  } else {
    LeewayNMEA = NAN;
  }

  // ===== VELOCITY MADE GOOD (two independent values, both always computed — no mode toggle) =====
  // VMGNMEA   = VMG toward the manual target bearing: SOG·cos(targetBearing − COG).
  // VMGUpwind = VMG to windward: SOG·cos(TWA). TrueWindAngleNMEA is boat-relative, so NO COG term.
  if (!IS_STALE(IDX_SOG_NMEA) && !IS_STALE(IDX_COG_NMEA) && VMGTargetBearing >= 0) {
    float bearingDiff = (VMGTargetBearing - COGNMEA) * PI / 180.0;
    VMGNMEA = SOGNMEA * cos(bearingDiff);
    MARK_FRESH(IDX_VMG);
  } else {
    VMGNMEA = NAN;  // no manual target set, or SOG/COG stale
  }
  if (!IS_STALE(IDX_SOG_NMEA) && !IS_STALE(IDX_TRUE_WIND_ANGLE)) {
    VMGUpwind = SOGNMEA * cos(TrueWindAngleNMEA * PI / 180.0);
  } else {
    VMGUpwind = NAN;  // need boat speed + true wind angle
  }
  // Session min/max watermarks for both VMGs (dashboard ↑/↓ sidebars)
  wmIgnUpdate(wmIgn_VMGman, VMGNMEA);
  wmIgnUpdate(wmIgn_VMGup, VMGUpwind);
  // Lifetime best upwind VMG — only while the engine is effectively off (sailing, RPM<50), so motoring can't pad the record.
  if (isfinite(VMGUpwind) && RPM < 50.0f && VMGUpwind > best_upwind_vmg_alltime) {
    best_upwind_vmg_alltime = VMGUpwind;
  }

  // ===== SUSTAINED TRUE WIND → BEAUFORT & GALE =====
  // 2-min running average of true wind speed (US NWS "sustained wind" window). Time-aware EWMA so the
  // window stays ~120 s regardless of call rate. This sustained value — NOT the instantaneous gust — is
  // the basis for the Beaufort readout and the gale detector: a 34 kt gust isn't a gale, and a brief lull
  // inside a 40 kt blow doesn't end one. When true wind is stale we can't confirm a gale, so the run ends.
  if (!IS_STALE(IDX_TRUE_WIND_SPEED)) {
    static uint32_t lastSustainedMs = 0;
    uint32_t nowMs = millis();
    if (isnan(sustainedTWS) || lastSustainedMs == 0) {
      sustainedTWS = TrueWindSpeedNMEA;                 // seed on first valid sample (or after a data gap)
    } else {
      float dt = (nowMs - lastSustainedMs) / 1000.0f;   // seconds since last update
      float alpha = dt / (120.0f + dt);                 // 120 s time constant
      sustainedTWS += alpha * (TrueWindSpeedNMEA - sustainedTWS);
    }
    lastSustainedMs = nowMs;

    if (sustainedTWS >= 34.0f) {                         // Beaufort 8 — gale, on the sustained average
      if (!galeActive) { galeActive = true; galeStartMs = nowMs; }
      currentGaleMinutes = (nowMs - galeStartMs) / 60000.0f;
      float galeHrs = currentGaleMinutes / 60.0f;
      if (galeHrs > longest_gale_duration_hours_alltime) longest_gale_duration_hours_alltime = galeHrs;
    } else {
      galeActive = false;
      currentGaleMinutes = 0;
    }
  } else {
    galeActive = false;     // true wind unavailable — can't prove a gale; end the run, hold last sustainedTWS for display
    currentGaleMinutes = 0;
  }
}
void saveFuelTableToNVS() {
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("fuel", NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK) {
    queueConsoleMessage("Failed to open NVS for fuel table");
    Serial.println("DEBUG: Failed to open NVS!");
    return;
  }
  nvs_set_blob(nvs_handle, "fuelRPM", fuelTableRPM, sizeof(fuelTableRPM));
  Serial.print("DEBUG: Writing fuelTableGPH blob (");
  nvs_set_blob(nvs_handle, "fuelGPH", fuelTableGPH, sizeof(fuelTableGPH));
  nvs_commit(nvs_handle);
  nvs_close(nvs_handle);
}

void loadFuelTableFromNVS() {
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("fuel", NVS_READONLY, &nvs_handle);
  if (err != ESP_OK) {
    queueConsoleMessage("Fuel: No saved table, using defaults");
    return;
  }

  size_t required_size = sizeof(fuelTableRPM);
  err = nvs_get_blob(nvs_handle, "fuelRPM", fuelTableRPM, &required_size);
  if (err != ESP_OK) {
    Serial.println("DEBUG: Failed to load fuelRPM from NVS");
    nvs_close(nvs_handle);
    return;
  }

  required_size = sizeof(fuelTableGPH);
  err = nvs_get_blob(nvs_handle, "fuelGPH", fuelTableGPH, &required_size);
  if (err != ESP_OK) {
    Serial.println("DEBUG: Failed to load fuelGPH from NVS");
  }
  nvs_close(nvs_handle);
}

bool ensureLittleFS() {
  if (littleFSMounted) {
    return true;
  }

  Serial.println("Initializing LittleFS...");

  // Start clean
  LittleFS.end();

  // Use formatOnFail=TRUE (first parameter)
  if (!LittleFS.begin(true, "/littlefs", 20, "userdata")) {
    Serial.println("CRITICAL: LittleFS begin failed even with formatOnFail=true");
    littleFSMounted = false;
    return false;
  }

  if (!fsMutex) {
    fsMutex = xSemaphoreCreateMutex();  // MUST be a mutex (priority inheritance + ownership)
    if (!fsMutex) {
      Serial.println("CRITICAL: fsMutex creation failed");
      littleFSMounted = false;
      return false;
    }
    // Do NOT xSemaphoreGive() a mutex here.
  }


  Serial.println("LittleFS mounted successfully");
  Serial.printf("Total: %u Used: %u\n",
                (unsigned)fsTotalBytes(),
                (unsigned)fsUsedBytes());
  littleFSMounted = true;
  return true;
}



void ensurePreferredBootPartition() {
  //Boot partition selection. ota_0 is promoted only when it holds the image a completed, RSA-verified
  //install recorded (recordOta0AsVerified): a slot can hold a well-formed image that never passed
  //verification (signature rejected, web bundle failed after the firmware entry streamed), and the
  //bootloader's structural checks cannot tell those apart. GPIO41 held at boot forces factory, clears
  //the staged-update flags and drops the record — the emergency path back to known-good firmware, and
  //the unit stays there until the next verified install.
  const esp_partition_t *ota0_partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
  const esp_partition_t *factory_partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
  const esp_partition_t *current_boot = esp_ota_get_boot_partition();
  const esp_partition_t *running = esp_ota_get_running_partition();

  // Check GPIO41 for manual factory reset
  pinMode(41, INPUT_PULLUP);
  bool forceFactory = (digitalRead(41) == LOW);

  if (forceFactory) {
    // Clear any pending update flags to prevent boot loops. Staged-update flags live in the
    // "update_req" namespace — erasing them from "storage" was a silent no-op that let an
    // armed staged update survive the recovery jumper.
    clearPendingUpdateNVS();
    clearOta0Record();


    if (current_boot != factory_partition) {
      esp_ota_set_boot_partition(factory_partition);
      Serial.println("Boot: GPIO41 forced factory partition - restarting...");
      events.send("Boot: GPIO41 forced factory partition - restarting...", "console", millis());
      Serial.printf("GPIO41 state: %d\n", digitalRead(41));
      delay(1500);
      ESP.restart();
    } else {
      Serial.println("Boot: GPIO41 detected - staying in factory mode");
      events.send("Boot: GPIO41 detected - staying in factory mode", "console", millis());
      Serial.printf("GPIO41 status: %d\n", digitalRead(41));
    }


    return;  // <<<< CRITICAL: Exit function, don't run normal logic
  }

  if (running == ota0_partition) {
    // The bootloader only lands here by a trusted path (verified install, gated promotion, USB flash).
    // Self-record when the record is missing (USB-flashed unit, deep factory reset) or does not match this
    // image (USB reflash of ota_0 over an older record), so a later factory boot can bring this image back
    // after an abandoned update. Appended digest only: the bootloader already validated the running image.
    uint8_t rec[32], cur[32];
    bool haveRec = readOta0Record(rec);
    bool matches = haveRec && appImageAppendedDigest(ota0_partition, cur) && memcmp(rec, cur, 32) == 0;
    if (!matches) {
      Serial.println(haveRec ? "Boot: running ota_0 but the verified-install record is for another image - re-recording"
                             : "Boot: running ota_0 without a verified-install record - recording this image");
      recordOta0AsVerified();
    }
    return;
  }

  esp_app_desc_t app_desc;
  bool ota0_valid = (esp_ota_get_partition_description(ota0_partition, &app_desc) == ESP_OK);
  uint8_t rec[32];
  bool haveRecord = readOta0Record(rec);

  if (ota0_valid && haveRecord && ota0MatchesRecord()) {
    if (current_boot != ota0_partition) {
      esp_err_t err = esp_ota_set_boot_partition(ota0_partition);
      if (err == ESP_OK) {
        Serial.println("Boot: ota_0 holds the recorded verified install - switched to ota_0 partition");
        events.send("Boot: Switched to ota_0 partition", "console", millis());
      } else {
        Serial.printf("Boot: esp_ota_set_boot_partition(ota_0) failed: %s - staying on factory\n", esp_err_to_name(err));
        esp_ota_set_boot_partition(factory_partition);
      }
    }
  } else {
    Serial.println(!ota0_valid ? "Boot: ota_0 empty or invalid - staying on factory"
                   : !haveRecord ? "Boot: ota_0 image has no verified-install record - staying on factory"
                                 : "Boot: ota_0 image does not match the verified-install record - staying on factory");
    if (current_boot != factory_partition) {
      esp_ota_set_boot_partition(factory_partition);
      events.send("Boot: ota_0 not promotable, using factory partition", "console", millis());
    }
  }
}
// Lazy-expiring check for the settings arm gate — every mutating endpoint calls this.
bool settingsArmActive() {
  if (!settingsArmed) return false;
  if (millis() - settingsArmedAtMs > SETTINGS_ARM_TIMEOUT_MS) {
    settingsArmed = false;
    queueConsoleMessage("Settings auto-locked (30 min window expired)");
    return false;
  }
  return true;
}
// Mirror every queued message into the /consolehist.txt history ring (own indices, own short
// critical section — never nested inside the queue's).
static void consoleHistAppend(const char *msg) {
  if (!consoleHist || !msg) return;
  time_t ep = time(nullptr);
  uint32_t nowMs = millis();
  portENTER_CRITICAL(&consoleMux);
  ConsoleHistEntry &h = consoleHist[consoleHistHead];
  h.ms = nowMs;
  h.epoch = ep;
  strncpy(h.msg, msg, CONSOLE_MSG_LEN - 1);
  h.msg[CONSOLE_MSG_LEN - 1] = '\0';
  consoleHistHead = (consoleHistHead + 1) % CONSOLE_HIST_SIZE;
  if (consoleHistCount < CONSOLE_HIST_SIZE) consoleHistCount++;
  portEXIT_CRITICAL(&consoleMux);
}
// printf-style — avoids String heap churn
void queueConsoleMessageF(const char *format, ...) {
  if (otaInProgress) {
    return;  // Skip during OTA
  }
  if (!consoleQueue || !format) return;
  char formattedMsg[CONSOLE_MSG_LEN];
  va_list args;
  va_start(args, format);
  vsnprintf(formattedMsg, sizeof(formattedMsg), format, args);
  va_end(args);

  consoleHistAppend(formattedMsg);
  portENTER_CRITICAL(&consoleMux);
  if (consoleCount >= CONSOLE_QUEUE_SIZE) {
    consoleTail = (consoleTail + 1) % CONSOLE_QUEUE_SIZE;
    consoleCount = CONSOLE_QUEUE_SIZE - 1;
  }
  strncpy(consoleQueue[consoleHead].message, formattedMsg, CONSOLE_MSG_LEN - 1);
  consoleQueue[consoleHead].message[CONSOLE_MSG_LEN - 1] = '\0';
  consoleQueue[consoleHead].timestamp = millis();
  consoleHead = (consoleHead + 1) % CONSOLE_QUEUE_SIZE;
  consoleCount++;
  portEXIT_CRITICAL(&consoleMux);
}
// Legacy c-string variant — no formatting
void queueConsoleMessage(const char *msg) {
  if (otaInProgress) {
    return;  // Skip during OTA
  }
  if (!consoleQueue || !msg) return;

  consoleHistAppend(msg);
  portENTER_CRITICAL(&consoleMux);
  if (consoleCount >= CONSOLE_QUEUE_SIZE) {
    consoleTail = (consoleTail + 1) % CONSOLE_QUEUE_SIZE;
    consoleCount = CONSOLE_QUEUE_SIZE - 1;
  }
  // Copy directly into queue slot
  strncpy(consoleQueue[consoleHead].message, msg, CONSOLE_MSG_LEN - 1);
  consoleQueue[consoleHead].message[CONSOLE_MSG_LEN - 1] = '\0';
  consoleQueue[consoleHead].timestamp = millis();
  consoleHead = (consoleHead + 1) % CONSOLE_QUEUE_SIZE;
  consoleCount++;
  portEXIT_CRITICAL(&consoleMux);
}
// Legacy String overload
void queueConsoleMessage(const String &message) {
  if (otaInProgress) {
    return;  // Skip during OTA
  }
  queueConsoleMessage(message.c_str());
}
// Copy the oldest queued message out under the lock (the SSE send must happen outside it). False when empty.
// One message per call, one caller-side buffer: the old 5-slot variant cost 5×CONSOLE_MSG_LEN of caller stack
// and filled a timestamp array nothing ever read (the client stamps app-received time).
bool popConsoleMessage(char *out) {
  if (!consoleQueue || !out) return false;

  bool got = false;
  portENTER_CRITICAL(&consoleMux);
  if (consoleCount > 0) {
    strncpy(out, consoleQueue[consoleTail].message, CONSOLE_MSG_LEN - 1);
    out[CONSOLE_MSG_LEN - 1] = '\0';
    consoleTail = (consoleTail + 1) % CONSOLE_QUEUE_SIZE;
    consoleCount--;
    got = true;
  }
  portEXIT_CRITICAL(&consoleMux);

  return got;
}
void trySendConsoleSSE(bool &sentSomething, unsigned long now) {
  if (sentSomething) return;
  if (now - lastConsoleMessageTime < CONSOLE_MESSAGE_INTERVAL) return;

  char msg[CONSOLE_MSG_LEN];   // copy under the lock, send outside it — same discipline, one buffer
  int sent = 0;
  while (sent < 5 && popConsoleMessage(msg)) {
    events.send(msg, "console");
    sent++;
  }
  if (sent == 0) return;

  lastConsoleMessageTime = now;
  lastEventSourceSend = now;  // DELETE THIS LINE TO UNTHROTTLE CONSOLE
  sentSomething = true;
}
void initializeNVS() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    Serial.println("NVS init issue: erasing NVS and retrying...");
    err = nvs_flash_erase();
    if (err != ESP_OK) {
      Serial.printf("ERROR: Failed to erase NVS (code %d)\n", err);
      return;
    }
    err = nvs_flash_init();
  }
  if (err != ESP_OK) {
    Serial.printf("ERROR: Failed to initialize NVS (code %d)\n", err);
    return;
  }
  Serial.println("NVS initialized successfully");
  queueConsoleMessage("NVS initialized successfully");
}
// Synchronous full write — for setup() and emergency use only.
// Not safe to call from loop() — blocks for up to 200ms.
void saveNVSDataFull() {
  if (hardwarePresent != 1) return;   // sim mode (HardwarePresent=0): don't persist fake accumulators/extrema
  uint32_t _nvsT0 = millis();
  nvs_handle_t h;
  esp_err_t err = nvs_open("storage", NVS_READWRITE, &h);
  if (err != ESP_OK) {
    queueConsoleMessage("ERROR: Failed to open NVS");
    Serial.println("ERROR: Failed to open NVS");
    return;
  }
  bool chg = false;

  if (prev_ChargedEnergy != (uint32_t)ChargedEnergy)                        { nvs_set_u32(h, "ChargedEnergy",  (uint32_t)ChargedEnergy);                        prev_ChargedEnergy = (uint32_t)ChargedEnergy;                        chg = true; }
  if (prev_DischrgdEnergy != (uint32_t)DischargedEnergy)                    { nvs_set_u32(h, "DischrgdEnergy", (uint32_t)DischargedEnergy);                    prev_DischrgdEnergy = (uint32_t)DischargedEnergy;                    chg = true; }
  if (prev_AltChrgdEnergy != (uint32_t)AlternatorChargedEnergy)             { nvs_set_u32(h, "AltChrgdEnergy", (uint32_t)AlternatorChargedEnergy);             prev_AltChrgdEnergy = (uint32_t)AlternatorChargedEnergy;             chg = true; }
  if (prev_SolarEnergy != (uint32_t)SolarChargedEnergy)                     { nvs_set_u32(h, "SolarEnergy",    (uint32_t)SolarChargedEnergy);                  prev_SolarEnergy = (uint32_t)SolarChargedEnergy;                     chg = true; }
  if (prev_AltFuelUsed != (int32_t)(AlternatorFuelUsed * 10))               { nvs_set_i32(h, "AltFuelUsed",    (int32_t)(AlternatorFuelUsed * 10));             prev_AltFuelUsed = (int32_t)(AlternatorFuelUsed * 10);               chg = true; }
  if (prev_EngineFuel != (int32_t)(EngineFuelUsed * 10))                    { nvs_set_i32(h, "EngineFuel",     (int32_t)(EngineFuelUsed * 10));                 prev_EngineFuel = (int32_t)(EngineFuelUsed * 10);                    chg = true; }
  if (prev_ChargedEnergy_AllTime != (uint32_t)ChargedEnergy_AllTime)        { nvs_set_u32(h, "ChrgdEng_AT",    (uint32_t)ChargedEnergy_AllTime);               prev_ChargedEnergy_AllTime = (uint32_t)ChargedEnergy_AllTime;        chg = true; }
  if (prev_DischrgdEnergy_AllTime != (uint32_t)DischargedEnergy_AllTime)    { nvs_set_u32(h, "DschrgEng_AT",   (uint32_t)DischargedEnergy_AllTime);            prev_DischrgdEnergy_AllTime = (uint32_t)DischargedEnergy_AllTime;    chg = true; }
  if (prev_AltChrgdEnergy_AllTime != (uint32_t)AlternatorChargedEnergy_AllTime) { nvs_set_u32(h, "AltChrgEng_AT", (uint32_t)AlternatorChargedEnergy_AllTime); prev_AltChrgdEnergy_AllTime = (uint32_t)AlternatorChargedEnergy_AllTime; chg = true; }
  if (prev_SolarEnergy_AllTime != (uint32_t)SolarChargedEnergy_AllTime)     { nvs_set_u32(h, "SolarEng_AT",    (uint32_t)SolarChargedEnergy_AllTime);          prev_SolarEnergy_AllTime = (uint32_t)SolarChargedEnergy_AllTime;     chg = true; }
  if (prev_AltFuelUsed_AllTime != (int32_t)(AlternatorFuelUsed_AllTime * 10)) { nvs_set_i32(h, "AltFuel_AT",  (int32_t)(AlternatorFuelUsed_AllTime * 10));    prev_AltFuelUsed_AllTime = (int32_t)(AlternatorFuelUsed_AllTime * 10); chg = true; }
  if (prev_EngineFuel_AllTime != (int32_t)(EngineFuelUsed_AllTime * 10))    { nvs_set_i32(h, "EngFuel_AT",     (int32_t)(EngineFuelUsed_AllTime * 10));        prev_EngineFuel_AllTime = (int32_t)(EngineFuelUsed_AllTime * 10);    chg = true; }
  // Runtime + Cycles
  if (prev_EngineRunTime != (int32_t)EngineRunTime)                         { nvs_set_i32(h, "EngineRunTime",  (int32_t)EngineRunTime);                        prev_EngineRunTime = (int32_t)EngineRunTime;                         chg = true; }
  if (prev_EngineCycles != (int32_t)EngineCycles)                           { nvs_set_i32(h, "EngineCycles",   (int32_t)EngineCycles);                         prev_EngineCycles = (int32_t)EngineCycles;                           chg = true; }
  if (prev_AltOnTime != (int32_t)AlternatorOnTime)                          { nvs_set_i32(h, "AltOnTime",      (int32_t)AlternatorOnTime);                     prev_AltOnTime = (int32_t)AlternatorOnTime;                          chg = true; }
  if (prev_EngineRunTime_AllTime != (int32_t)EngineRunTime_AllTime)         { nvs_set_i32(h, "EngRunTime_AT",  (int32_t)EngineRunTime_AllTime);                prev_EngineRunTime_AllTime = (int32_t)EngineRunTime_AllTime;         chg = true; }
  if (prev_perfSailSeconds != (int32_t)perfSailSeconds)                     { nvs_set_i32(h, "PerfSailSec",    (int32_t)perfSailSeconds);                      prev_perfSailSeconds = (int32_t)perfSailSeconds;                     chg = true; }
  if (prev_perfMotorSeconds != (int32_t)perfMotorSeconds)                   { nvs_set_i32(h, "PerfMotorSec",   (int32_t)perfMotorSeconds);                     prev_perfMotorSeconds = (int32_t)perfMotorSeconds;                   chg = true; }
  if (prev_EngineCycles_AllTime != (int32_t)EngineCycles_AllTime)           { nvs_set_i32(h, "EngCycles_AT",   (int32_t)EngineCycles_AllTime);                 prev_EngineCycles_AllTime = (int32_t)EngineCycles_AllTime;           chg = true; }
  if (prev_AltOnTime_AllTime != (int32_t)AlternatorOnTime_AllTime)          { nvs_set_i32(h, "AltOnTime_AT",   (int32_t)AlternatorOnTime_AllTime);             prev_AltOnTime_AllTime = (int32_t)AlternatorOnTime_AllTime;          chg = true; }
  if (prev_ChargeCycles != (int32_t)ChargeCycles)                           { nvs_set_i32(h, "ChrgCycles",     (int32_t)ChargeCycles);                         prev_ChargeCycles = (int32_t)ChargeCycles;                           chg = true; }
  if (prev_ChargeCycles_AllTime != (int32_t)ChargeCycles_AllTime)           { nvs_set_i32(h, "ChrgCyc_AT",     (int32_t)ChargeCycles_AllTime);                 prev_ChargeCycles_AllTime = (int32_t)ChargeCycles_AllTime;           chg = true; }
  // App-usage lifetime totals (UsgTime_AT is seconds of visible app time). UsageOpenTime_AllTime is a
  // double written on the web-server task under usageMutex; an unguarded cross-core read can tear, and a
  // torn value saved here would be restored as truth at the next boot if power died before the following
  // save. Snapshot under the mutex; on contention skip these rows (they retry at the next save).
  if (usageMutex && xSemaphoreTake(usageMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    int32_t usgTime  = (int32_t)UsageOpenTime_AllTime;
    int32_t usgOpens = (int32_t)UsageOpens_AllTime;
    int32_t usgDays  = (int32_t)UsageDays_AllTime;
    xSemaphoreGive(usageMutex);
    if (prev_UsageOpenTime_AllTime != usgTime)                              { nvs_set_i32(h, "UsgTime_AT",     usgTime);                                       prev_UsageOpenTime_AllTime = usgTime;                                chg = true; }
    if (prev_UsageOpens_AllTime != usgOpens)                                { nvs_set_i32(h, "UsgOpens_AT",    usgOpens);                                      prev_UsageOpens_AllTime = usgOpens;                                  chg = true; }
    if (prev_UsageDays_AllTime != usgDays)                                  { nvs_set_i32(h, "UsgDays_AT",     usgDays);                                       prev_UsageDays_AllTime = usgDays;                                    chg = true; }
  }
  // Travel + SOC session
  if (prev_TotalDist != (int32_t)TotalDistance)                             { nvs_set_i32(h, "TotalDist",      (int32_t)TotalDistance);                        prev_TotalDist = (int32_t)TotalDistance;                             chg = true; }
  if (prev_AvgSpeed != (int32_t)(AvgSpeed * 100))                           { nvs_set_i32(h, "AvgSpeed",       (int32_t)(AvgSpeed * 100));                     prev_AvgSpeed = (int32_t)(AvgSpeed * 100);                           chg = true; }
  if (prev_TotalDist_AllTime != (int32_t)TotalDistance_AllTime)             { nvs_set_i32(h, "TotDist_AT",     (int32_t)TotalDistance_AllTime);                prev_TotalDist_AllTime = (int32_t)TotalDistance_AllTime;             chg = true; }
  if (prev_AvgSpeed_AllTime != (int32_t)(AvgSpeed_AllTime * 100))           { nvs_set_i32(h, "AvgSpd_AT",      (int32_t)(AvgSpeed_AllTime * 100));             prev_AvgSpeed_AllTime = (int32_t)(AvgSpeed_AllTime * 100);           chg = true; }
  if (prev_spdAccum_AllTime  != speedAccumulator_AllTime)                   { nvs_set_blob(h, "SpdAccum_AT",   &speedAccumulator_AllTime,   sizeof(double));   prev_spdAccum_AllTime  = speedAccumulator_AllTime;                   chg = true; }
  if (prev_spdTime_AllTime   != (uint32_t)totalSpeedSampleTime_AllTime)     { nvs_set_u32(h, "SpdTime_AT",     (uint32_t)totalSpeedSampleTime_AllTime);        prev_spdTime_AllTime   = (uint32_t)totalSpeedSampleTime_AllTime;     chg = true; }
  if (prev_AvgSOC != (int32_t)(AvgSOC * 100))                               { nvs_set_i32(h, "AvgSOC",         (int32_t)(AvgSOC * 100));                       prev_AvgSOC = (int32_t)(AvgSOC * 100);                               chg = true; }
  // SOC alltime + voltage AllTime accumulators + battery state
  { uint64_t sc = (uint64_t)(socAccumulator_AllTime * 100.0);
    if (prev_socAccum_AllTime != sc)                                         { nvs_set_u64(h, "SocAccum_AT", sc);                                               prev_socAccum_AllTime = sc;                                          chg = true; } }
  if (prev_socTime_AllTime != (uint32_t)totalSocSampleTime_AllTime)         { nvs_set_u32(h, "SocTime_AT",     (uint32_t)totalSocSampleTime_AllTime);          prev_socTime_AllTime = (uint32_t)totalSocSampleTime_AllTime;         chg = true; }
  if (prev_vltAccum_AllTime  != voltageAccumulator_AllTime)                  { nvs_set_blob(h, "VltAccum_AT",   &voltageAccumulator_AllTime, sizeof(double));   prev_vltAccum_AllTime  = voltageAccumulator_AllTime;                  chg = true; }
  if (prev_vltTime_AllTime   != (uint32_t)totalVoltageSampleTime_AllTime)    { nvs_set_u32(h, "VltTime_AT",     (uint32_t)totalVoltageSampleTime_AllTime);      prev_vltTime_AllTime   = (uint32_t)totalVoltageSampleTime_AllTime;    chg = true; }
  // Don't persist provisional pre-seed SOC/coulomb: a persisted value clears the pending flag next
  // boot and skips the seed forever. Both guards needed — SOC only sticks if coulombSeedPending is
  // still true (UpdateBatterySOC re-derives SOC from CoulombCount each tick).
  if (!socSeedPending && prev_SOC_percent != (int32_t)SOC_percent)          { nvs_set_i32(h, "SOC_percent",    (int32_t)SOC_percent);                          prev_SOC_percent = (int32_t)SOC_percent;                             chg = true; }
  if (!coulombSeedPending && prev_CoulombCount != (int32_t)CoulombCount_Ah_scaled) { nvs_set_i32(h, "CoulombCount",   (int32_t)CoulombCount_Ah_scaled);               prev_CoulombCount = (int32_t)CoulombCount_Ah_scaled;                 chg = true; }
  if (!coulombSeedPending && prev_ShadowCoulomb != (int32_t)shadowCoulombX100)     { nvs_set_i32(h, "ShadowCoulomb",  (int32_t)shadowCoulombX100);                    prev_ShadowCoulomb = (int32_t)shadowCoulombX100;                     chg = true; }
  // Health + Thermal + Learning
  if (prev_SessionDur != (uint32_t)CurrentSessionDuration)                  { nvs_set_u32(h, "SessionDur",     (uint32_t)CurrentSessionDuration);              prev_SessionDur = (uint32_t)CurrentSessionDuration;                  chg = true; }
  if (prev_MaxLoop != (int32_t)MaxLoopTime)                                  { nvs_set_i32(h, "MaxLoop",        (int32_t)MaxLoopTime);                          prev_MaxLoop = (int32_t)MaxLoopTime;                                 chg = true; }
  if (prev_MinHeap != (int32_t)MinFreeHeap)                                  { nvs_set_i32(h, "MinHeap",        (int32_t)MinFreeHeap);                          prev_MinHeap = (int32_t)MinFreeHeap;                                 chg = true; }
  if (prev_PowerCycles != (int32_t)totalPowerCycles)                        { nvs_set_i32(h, "PowerCycles",    (int32_t)totalPowerCycles);                     prev_PowerCycles = (int32_t)totalPowerCycles;                        chg = true; }
  if (prev_InsulDamage != CumulativeInsulationDamage)                       { nvs_set_blob(h, "InsulDamage",   &CumulativeInsulationDamage, sizeof(float));     prev_InsulDamage = CumulativeInsulationDamage;                       chg = true; }
  if (prev_GreaseDamage != CumulativeGreaseDamage)                          { nvs_set_blob(h, "GreaseDamage",  &CumulativeGreaseDamage,     sizeof(float));     prev_GreaseDamage = CumulativeGreaseDamage;                          chg = true; }
  if (prev_BrushDamage != CumulativeBrushDamage)                            { nvs_set_blob(h, "BrushDamage",   &CumulativeBrushDamage,      sizeof(float));     prev_BrushDamage = CumulativeBrushDamage;                            chg = true; }
  if (prev_ShuntGain != DynamicShuntGainFactor)                             { nvs_set_blob(h, "ShuntGain",     &DynamicShuntGainFactor,     sizeof(float));     prev_ShuntGain = DynamicShuntGainFactor;                             chg = true; }
  if (prev_LastGainTime != (uint32_t)lastGainCorrectionTime)                { nvs_set_u32(h, "LastGainTime",   (uint32_t)lastGainCorrectionTime);              prev_LastGainTime = (uint32_t)lastGainCorrectionTime;                chg = true; }
  // Temp-comp zero-correction learned equation (ZERO_DRIFT_TEMPCOMP_SPEC.md). Compare-first → ~1 write/day.
  if (prev_zfValid != zfValid)                                              { nvs_set_i32(h,  "ZFitValid",     zfValid);                                       prev_zfValid = zfValid;                                              chg = true; }
  if (prev_zfSensor != zfSensor)                                            { nvs_set_i32(h,  "ZFitSensor",    zfSensor);                                      prev_zfSensor = zfSensor;                                            chg = true; }
  if (prev_zfC != zfC)                                                      { nvs_set_blob(h, "ZFitC",         &zfC,                        sizeof(float));     prev_zfC = zfC;                                                      chg = true; }
  if (prev_zfB != zfB)                                                      { nvs_set_blob(h, "ZFitB",         &zfB,                        sizeof(float));     prev_zfB = zfB;                                                      chg = true; }
  if (prev_zfR2 != zfR2)                                                    { nvs_set_blob(h, "ZFitR2",        &zfR2,                       sizeof(float));     prev_zfR2 = zfR2;                                                    chg = true; }
  if (prev_zfLastEpoch != zfLastEpoch)                                      { nvs_set_u32(h,  "ZFitEpoch",     zfLastEpoch);                                   prev_zfLastEpoch = zfLastEpoch;                                      chg = true; }
  if (prev_sailing_days_alltime != sailing_days_alltime)                    { nvs_set_blob(h, "SailDays_AT",   &sailing_days_alltime,       sizeof(float));     prev_sailing_days_alltime = sailing_days_alltime;                    chg = true; }
  if (prev_sailing_dist_alltime != sailing_dist_alltime)                    { nvs_set_blob(h, "SailDist_AT",   &sailing_dist_alltime,       sizeof(float));     prev_sailing_dist_alltime = sailing_dist_alltime;                    chg = true; }
  if (prev_alt_power_max_alltime_w != alt_power_max_alltime_w)              { nvs_set_blob(h, "AltPwrMax_AT",  &alt_power_max_alltime_w,    sizeof(float));     prev_alt_power_max_alltime_w = alt_power_max_alltime_w;              chg = true; }
  if (prev_solar_power_max_alltime_w != solar_power_max_alltime_w)          { nvs_set_blob(h, "SolPwrMax_AT",  &solar_power_max_alltime_w,  sizeof(float));     prev_solar_power_max_alltime_w = solar_power_max_alltime_w;          chg = true; }
  if (prev_imu_capsize_count != imu_capsize_count)                          { nvs_set_u32(h,  "IMU_Capsize",   imu_capsize_count);                              prev_imu_capsize_count = imu_capsize_count;                          chg = true; }
  if (prev_imu_pitchpole_count != imu_pitchpole_count)                      { nvs_set_u32(h,  "IMU_Pitchpol",  imu_pitchpole_count);                           prev_imu_pitchpole_count = imu_pitchpole_count;                      chg = true; }
  if (prev_imu_slam_count_lifetime != imu_slam_count_lifetime)              { nvs_set_u32(h,  "IMU_SlamLife",  imu_slam_count_lifetime);                        prev_imu_slam_count_lifetime = imu_slam_count_lifetime;              chg = true; }
  // Fast alt-current detector lifetime FAULT count (fleet scalar)
  if (prev_faAnomalyCount != faAnomalyCount)                                { nvs_set_u32(h,  "faAnomalyCnt",  faAnomalyCount);                                 prev_faAnomalyCount = faAnomalyCount;                                chg = true; }
  // Sea state minute counters (NVS save).
  if (prev_imu_min_moving_gentle != imu_min_moving_gentle)                  { nvs_set_u32(h,  "IMU_MinMvGnt",  imu_min_moving_gentle);                          prev_imu_min_moving_gentle = imu_min_moving_gentle;                  chg = true; }
  if (prev_imu_min_moving_moderate != imu_min_moving_moderate)              { nvs_set_u32(h,  "IMU_MinMvMod",  imu_min_moving_moderate);                        prev_imu_min_moving_moderate = imu_min_moving_moderate;              chg = true; }
  if (prev_imu_min_moving_rough != imu_min_moving_rough)                    { nvs_set_u32(h,  "IMU_MinMvRgh",  imu_min_moving_rough);                           prev_imu_min_moving_rough = imu_min_moving_rough;                    chg = true; }
  if (prev_imu_min_moving_extreme != imu_min_moving_extreme)                { nvs_set_u32(h,  "IMU_MinMvExt",  imu_min_moving_extreme);                         prev_imu_min_moving_extreme = imu_min_moving_extreme;                chg = true; }
  if (prev_imu_min_stat_gentle != imu_min_stat_gentle)                      { nvs_set_u32(h,  "IMU_MinStGnt",  imu_min_stat_gentle);                            prev_imu_min_stat_gentle = imu_min_stat_gentle;                      chg = true; }
  if (prev_imu_min_stat_moderate != imu_min_stat_moderate)                  { nvs_set_u32(h,  "IMU_MinStMod",  imu_min_stat_moderate);                          prev_imu_min_stat_moderate = imu_min_stat_moderate;                  chg = true; }
  if (prev_imu_min_stat_rough != imu_min_stat_rough)                        { nvs_set_u32(h,  "IMU_MinStRgh",  imu_min_stat_rough);                             prev_imu_min_stat_rough = imu_min_stat_rough;                        chg = true; }
  if (prev_imu_min_stat_extreme != imu_min_stat_extreme)                    { nvs_set_u32(h,  "IMU_MinStExt",  imu_min_stat_extreme);                           prev_imu_min_stat_extreme = imu_min_stat_extreme;                    chg = true; }
  if (prev_imu_heel_max_lifetime != imu_heel_max_lifetime)                  { nvs_set_blob(h, "IMU_HeelMax",   &imu_heel_max_lifetime,      sizeof(float));     prev_imu_heel_max_lifetime = imu_heel_max_lifetime;                  chg = true; }
  if (prev_imu_pitch_max_lifetime != imu_pitch_max_lifetime)                { nvs_set_blob(h, "IMU_PitchMax",  &imu_pitch_max_lifetime,     sizeof(float));     prev_imu_pitch_max_lifetime = imu_pitch_max_lifetime;                chg = true; }
  if (prev_imu_slam_peak_lifetime != imu_slam_peak_lifetime)                { nvs_set_blob(h, "IMU_SlamMax",   &imu_slam_peak_lifetime,     sizeof(float));     prev_imu_slam_peak_lifetime = imu_slam_peak_lifetime;                chg = true; }
  // imuMountOrientation / CAPSIZE_THRESHOLD_DEG / PITCHPOLE_THRESHOLD_DEG / SLAM_THRESHOLD_G
  // persist via LittleFS/settings (Pattern B) — user-set form inputs, not written here.

  // Watermarks (session + lifetime peaks)
  if (prev_MaxSpeed != MaxSpeed)                                            { nvs_set_blob(h, "MaxSpd",        &MaxSpeed,                          sizeof(float));    prev_MaxSpeed = MaxSpeed;                                            chg = true; }
  if (prev_MaxSpeed_AllTime != MaxSpeed_AllTime)                            { nvs_set_blob(h, "MaxSpd_AT",     &MaxSpeed_AllTime,                  sizeof(float));    prev_MaxSpeed_AllTime = MaxSpeed_AllTime;                            chg = true; }
  // Longest single trip — distances stored ×100 for 0.01-nm resolution. Epoch is Unix seconds (0 = was unsynced).
  if (prev_LongestTrip_AT != (int32_t)(LongestSingleTrip_Nm_AllTime * 100)) { nvs_set_i32(h, "LongTrip_AT",    (int32_t)(LongestSingleTrip_Nm_AllTime * 100));  prev_LongestTrip_AT = (int32_t)(LongestSingleTrip_Nm_AllTime * 100); chg = true; }
  if (prev_CurrTripDist  != (int32_t)(currentTripDistanceNm * 100))         { nvs_set_i32(h, "CurrTripDist",   (int32_t)(currentTripDistanceNm * 100));         prev_CurrTripDist  = (int32_t)(currentTripDistanceNm * 100);         chg = true; }
  if (prev_CurrTripEpoch != currentTripLastUpdateEpoch)                     { nvs_set_u32(h, "CurrTripEpch",   currentTripLastUpdateEpoch);                     prev_CurrTripEpoch = currentTripLastUpdateEpoch;                     chg = true; }
  if (prev_Max24hrDist_AT != (int32_t)(Max24hrDistance_AllTime * 100))      { nvs_set_i32(h, "Max24h_AT",      (int32_t)(Max24hrDistance_AllTime * 100));       prev_Max24hrDist_AT = (int32_t)(Max24hrDistance_AllTime * 100);      chg = true; }
  if (prev_DeepAnchor_AT != (int32_t)(DeepestAnchorage_Ft_AllTime * 10))    { nvs_set_i32(h, "DeepAnchor_AT",  (int32_t)(DeepestAnchorage_Ft_AllTime * 10));    prev_DeepAnchor_AT = (int32_t)(DeepestAnchorage_Ft_AllTime * 10);    chg = true; }
  if (prev_BestUpVMG_AT != (int32_t)(best_upwind_vmg_alltime * 100))           { nvs_set_i32(h, "BestUpVMG_AT",   (int32_t)(best_upwind_vmg_alltime * 100));           prev_BestUpVMG_AT = (int32_t)(best_upwind_vmg_alltime * 100);           chg = true; }
  if (prev_GaleHrs_AT != (int32_t)(longest_gale_duration_hours_alltime * 100)) { nvs_set_i32(h, "GaleHrs_AT",     (int32_t)(longest_gale_duration_hours_alltime * 100)); prev_GaleHrs_AT = (int32_t)(longest_gale_duration_hours_alltime * 100); chg = true; }
  // Ripple analyzer worsts — persisted so they survive reboot; cleared only by the ripple panel reset
  if (prev_faSesPkpk   != (int32_t)(faSesPkpkWorstA * 100))                    { nvs_set_i32(h, "faSesPkpk",     (int32_t)(faSesPkpkWorstA * 100));                    prev_faSesPkpk   = (int32_t)(faSesPkpkWorstA * 100);                  chg = true; }
  if (prev_faSesPeakA  != (int32_t)(faSesPeakWorstA * 100))                    { nvs_set_i32(h, "faSesPeakA",    (int32_t)(faSesPeakWorstA * 100));                    prev_faSesPeakA  = (int32_t)(faSesPeakWorstA * 100);                  chg = true; }
  if (prev_faSesPeakHz != (int32_t)(faSesPeakWorstHz * 10))                    { nvs_set_i32(h, "faSesPeakHz",   (int32_t)(faSesPeakWorstHz * 10));                    prev_faSesPeakHz = (int32_t)(faSesPeakWorstHz * 10);                  chg = true; }
  // Highest Tone in Map headline — already pre-scaled (amp pk-pk ×100, freq ×10, rpm raw); cleared by Reset Worsts / Clear Map
  if (prev_faDomAmp    != (int32_t)faDomAmpAX100)                              { nvs_set_i32(h, "faDomAmp",      (int32_t)faDomAmpAX100);                              prev_faDomAmp    = (int32_t)faDomAmpAX100;                            chg = true; }
  if (prev_faDomFreq   != (int32_t)faDomFreqHzX10)                             { nvs_set_i32(h, "faDomFreq",     (int32_t)faDomFreqHzX10);                             prev_faDomFreq   = (int32_t)faDomFreqHzX10;                           chg = true; }
  if (prev_faDomRpm    != (int32_t)faDomRpm)                                   { nvs_set_i32(h, "faDomRpm",      (int32_t)faDomRpm);                                   prev_faDomRpm    = (int32_t)faDomRpm;                                 chg = true; }
  // Operating-point context for the two headline worsts (amps ×10, temp ×10 [INT32_MIN if no probe], rpm raw, epoch raw)
  if (prev_faDomAmps   != (int32_t)(faDomAmpsA * 10))                          { nvs_set_i32(h, "faDomAmps",     (int32_t)(faDomAmpsA * 10));                          prev_faDomAmps   = (int32_t)(faDomAmpsA * 10);                        chg = true; }
  if (prev_faDomTmp    != (isnan(faDomTempF) ? INT32_MIN : (int32_t)(faDomTempF * 10)))      { nvs_set_i32(h, "faDomTmp",      (isnan(faDomTempF) ? INT32_MIN : (int32_t)(faDomTempF * 10)));      prev_faDomTmp    = (isnan(faDomTempF) ? INT32_MIN : (int32_t)(faDomTempF * 10));      chg = true; }
  if (prev_faDomEp     != (int32_t)faDomEpoch)                                 { nvs_set_i32(h, "faDomEp",       (int32_t)faDomEpoch);                                 prev_faDomEp     = (int32_t)faDomEpoch;                               chg = true; }
  if (prev_faSPkAmp    != (int32_t)(faSesPkpkAmpsA * 10))                       { nvs_set_i32(h, "faSPkAmp",      (int32_t)(faSesPkpkAmpsA * 10));                       prev_faSPkAmp    = (int32_t)(faSesPkpkAmpsA * 10);                     chg = true; }
  if (prev_faSPkTmp    != (isnan(faSesPkpkTempF) ? INT32_MIN : (int32_t)(faSesPkpkTempF * 10))) { nvs_set_i32(h, "faSPkTmp",      (isnan(faSesPkpkTempF) ? INT32_MIN : (int32_t)(faSesPkpkTempF * 10))); prev_faSPkTmp    = (isnan(faSesPkpkTempF) ? INT32_MIN : (int32_t)(faSesPkpkTempF * 10)); chg = true; }
  if (prev_faSPkRpm    != (int32_t)faSesPkpkRpm)                               { nvs_set_i32(h, "faSPkRpm",      (int32_t)faSesPkpkRpm);                               prev_faSPkRpm    = (int32_t)faSesPkpkRpm;                             chg = true; }
  if (prev_faSPkEp     != (int32_t)faSesPkpkEpoch)                             { nvs_set_i32(h, "faSPkEp",       (int32_t)faSesPkpkEpoch);                             prev_faSPkEp     = (int32_t)faSesPkpkEpoch;                           chg = true; }
  if (prev_MeasAmpsMax != MeasuredAmpsMax)                                  { nvs_set_blob(h, "MAmpsMax",      &MeasuredAmpsMax,                   sizeof(float));    prev_MeasAmpsMax = MeasuredAmpsMax;                                  chg = true; }
  if (prev_MeasAmpsMax_AllTime != MeasuredAmpsMax_AllTime)                  { nvs_set_blob(h, "MAmpsMax_AT",   &MeasuredAmpsMax_AllTime,           sizeof(float));    prev_MeasAmpsMax_AllTime = MeasuredAmpsMax_AllTime;                  chg = true; }
  if (prev_RPMMax != RPMMax)                                                { nvs_set_blob(h, "RPMMax",        &RPMMax,                            sizeof(float));    prev_RPMMax = RPMMax;                                                chg = true; }
  if (prev_RPMMax_AllTime != RPMMax_AllTime)                                { nvs_set_blob(h, "RPMMax_AT",     &RPMMax_AllTime,                    sizeof(float));    prev_RPMMax_AllTime = RPMMax_AllTime;                                chg = true; }
  if (prev_IBVMax != IBVMax)                                                { nvs_set_blob(h, "IBVMax",        &IBVMax,                            sizeof(float));    prev_IBVMax = IBVMax;                                                chg = true; }
  if (prev_PeakV_AllTime != PeakVoltage_AllTime)                            { nvs_set_blob(h, "PeakV_AT",      &PeakVoltage_AllTime,               sizeof(float));    prev_PeakV_AllTime = PeakVoltage_AllTime;                            chg = true; }
  if (prev_MinVoltage != MinVoltage)                                        { nvs_set_blob(h, "MinV",          &MinVoltage,                        sizeof(float));    prev_MinVoltage = MinVoltage;                                        chg = true; }
  if (prev_MinVoltage_AllTime != MinVoltage_AllTime)                        { nvs_set_blob(h, "MinV_AT",       &MinVoltage_AllTime,                sizeof(float));    prev_MinVoltage_AllTime = MinVoltage_AllTime;                        chg = true; }
  if (prev_board_temp_max != board_temp_max_alltime)                        { nvs_set_blob(h, "BdTmpMaxAt",    &board_temp_max_alltime,            sizeof(float));    prev_board_temp_max = board_temp_max_alltime;                        chg = true; }
  if (prev_board_temp_min != board_temp_min_alltime)                        { nvs_set_blob(h, "BdTmpMinAt",    &board_temp_min_alltime,            sizeof(float));    prev_board_temp_min = board_temp_min_alltime;                        chg = true; }
  if (prev_baro_max != baro_pressure_max_alltime)                           { nvs_set_blob(h, "BaroMaxAt",     &baro_pressure_max_alltime,         sizeof(float));    prev_baro_max = baro_pressure_max_alltime;                           chg = true; }
  if (prev_baro_min != baro_pressure_min_alltime)                           { nvs_set_blob(h, "BaroMinAt",     &baro_pressure_min_alltime,         sizeof(float));    prev_baro_min = baro_pressure_min_alltime;                           chg = true; }
  if (prev_MaxTempTherm != MaxTemperatureThermistor)                        { nvs_set_blob(h, "MaxTherm",      &MaxTemperatureThermistor,          sizeof(float));    prev_MaxTempTherm = MaxTemperatureThermistor;                        chg = true; }
  if (prev_MaxTempTherm_AllTime != MaxTemperatureThermistor_AllTime)        { nvs_set_blob(h, "MaxTherm_AT",   &MaxTemperatureThermistor_AllTime,  sizeof(float));    prev_MaxTempTherm_AllTime = MaxTemperatureThermistor_AllTime;        chg = true; }
  if (prev_MaxAltTempF != MaxAlternatorTemperatureF)                        { nvs_set_blob(h, "MaxAltTempF",   &MaxAlternatorTemperatureF,         sizeof(float));    prev_MaxAltTempF = MaxAlternatorTemperatureF;                        chg = true; }
  if (prev_MaxAltTempF_AllTime != MaxAlternatorTemperatureF_AllTime)        { nvs_set_blob(h, "MAltTempF_AT",  &MaxAlternatorTemperatureF_AllTime, sizeof(float));    prev_MaxAltTempF_AllTime = MaxAlternatorTemperatureF_AllTime;        chg = true; }
  if (prev_MaxWindApp != max_wind_speed_apparent_alltime)                   { nvs_set_blob(h, "MaxWApp_AT",    &max_wind_speed_apparent_alltime,   sizeof(float));    prev_MaxWindApp = max_wind_speed_apparent_alltime;                   chg = true; }
  if (prev_MaxWindTrue != max_wind_speed_true_alltime)                      { nvs_set_blob(h, "MaxWTr_AT",     &max_wind_speed_true_alltime,       sizeof(float));    prev_MaxWindTrue = max_wind_speed_true_alltime;                      chg = true; }
  if (prev_UVToday != UVToday)                                              { nvs_set_blob(h, "UVToday",       &UVToday,                           sizeof(float));    prev_UVToday = UVToday;                                              chg = true; }
  if (prev_UVTomorrow != UVTomorrow)                                        { nvs_set_blob(h, "UVTomorrow",    &UVTomorrow,                        sizeof(float));    prev_UVTomorrow = UVTomorrow;                                        chg = true; }
  if (prev_UVDay2 != UVDay2)                                                { nvs_set_blob(h, "UVDay2",        &UVDay2,                            sizeof(float));    prev_UVDay2 = UVDay2;                                                chg = true; }

  // Barometric pressure 14-day history — 8 KB blob + head index + last-sample epoch.
  // Only written when the ring head moved since last save (a new sample landed).
  if (baroPressureHistory && prev_baroHistoryHead != baroHistoryHead) {
    nvs_set_blob(h, "BaroHist",     baroPressureHistory, BARO_HISTORY_SIZE * sizeof(uint16_t));
    nvs_set_u16(h,  "BaroHistHead", baroHistoryHead);
    nvs_set_u32(h,  "BaroHistEpch", (uint32_t)baroHistoryLastEpoch);
    prev_baroHistoryHead = baroHistoryHead;
    chg = true;
  }

  // Soft clock anchor — persist the current wall epoch so a cold power-up can seed a
  // usable timebase before any live source reports (restoreSoftClock, step 2). Written
  // whenever time is known; the field-off-edge/shutdown cadence keeps flash wear low
  // (no periodic commits — same discipline as the rest of this function).
  if (timeIsSynced && timeBase > 0) {
    nvs_set_u32(h, "SoftClockEp", (uint32_t)getCurrentTimestamp());
    chg = true;
  }

  if (chg) nvs_commit(h);
  nvs_close(h);
  lastNVSSaveTime = millis();
  uint32_t _nvsElapsed = lastNVSSaveTime - _nvsT0;
  nvsFullSaveLastMs = (_nvsElapsed > 65535UL) ? 65535 : (uint16_t)_nvsElapsed;
  if (nvsFullSaveLastMs > nvsFullSaveWorstMs) nvsFullSaveWorstMs = nvsFullSaveLastMs;
  nvsFullSaveCount++;
}

// Returns true once fieldActiveStatus has been 0 continuously for 60s + extraMs.
// Resets the moment the field turns on. Use extraMs to stagger callers so they
// don't all fire at once after a long charging session.
bool fieldOffSettled(uint32_t extraMs) {
  static unsigned long fieldOffAt = 0;
  if (fieldActiveStatus > 0) {
    fieldOffAt = 0;
    return false;
  }
  if (fieldOffAt == 0) {
    fieldOffAt = millis();
    return false;
  }
  return (millis() - fieldOffAt >= 60000UL + extraMs);
}

// Hardware-gate variant for flash work on the loop. fieldOffSettled keys off fieldActiveStatus,
// which the control path zeroes whenever applied duty <= 0.01% — true during a live duty-0 CV
// hold with GPIO4 still up and every field-on clock running (2026-08-13 loop-stall incident:
// usedBytes/ledger writes fired mid-control at ~100ms). This one requires the field gate
// physically cut. Anything that touches LittleFS from loop() must gate on THIS.
// 20s baseline = 2x the fast-OV lockout cap (nextFastOvLockoutMs, 10s) — OV cut/retry churn,
// crank dips, and the LM2907 ~4.6s false-zero can never reach flash work; must grow if that
// ladder's cap grows. Long static lockouts (tach-lie 30-120s rungs, cold-charge) DO exceed it
// and may flush mid-lockout — field is hardware-dead there and re-enable is loop-synchronous,
// so that is deliberate, not a hole.
bool fieldCutSettled(uint32_t extraMs) {
  static unsigned long fieldCutAt = 0;
  if (!gpio4IsLow) {
    fieldCutAt = 0;
    return false;
  }
  if (fieldCutAt == 0) {
    fieldCutAt = millis();
    return false;
  }
  return (millis() - fieldCutAt >= 20000UL + extraMs);
}

// Periodic phased NVS save deleted — nvs_commit() blocks Core 1 for hundreds of ms
// during sector erase and can collide with the voltage control loop on a transient.
// All NVS persistence now goes through saveNVSDataFull() at the field-off edge
// (Xregulator.ino loop) and the shutdown sequence — both run with field off so any
// commit duration is safe. See git log for the original 9-phase implementation.

// First-boot SoC seed, deferred from loadNVSData() to the end of setup() so IBV holds a real
// INA228 reading and BATTERY_TYPE / SYSTEM_VOLTAGE_CLASS / BatteryCapacity_Ah hold the vessel-info
// values (all were still defaults/zero at loadNVSData time, which seeded 0% every fresh boot).
// On a factory-fresh device (Vessel Info never saved) it defers further: saveVesselInfoToNvs()
// re-invokes it after the first Vessel Info save, so the estimate never runs on the compile-time
// chemistry/capacity defaults. SOC holds the provisional 50% until then; that provisional state is
// never persisted (saveNVSDataFull guard), so the deferral survives reboots before that save.
void seedSocFromVoltage() {
  if (!socSeedPending) return;
  if (!vesselInfoSaved) return;
  socSeedPending = false;

  float voltage = getBatteryVoltage();
  float socM = SYSTEM_VOLTAGE_CLASS / 12.0f;  // bank-class scale for the 12V-referenced thresholds
  int estimatedSoC = 50;                 // fallback if the INA never produced a sane reading

  // Hoisted so the /socseed snapshot below can record them; snapVlo/plo..vhi/phi are the
  // OCV-table bracket (or lead-acid ladder rung) the lookup actually landed in.
  float iBat = 0.0f, boardTempF = NAN, rTempScale = 1.0f, vOcv = 0.0f;
  float snapVlo = 0.0f, snapVhi = 0.0f;
  int snapPlo = 0, snapPhi = 0;
  bool isLithium = false;

  if (voltage > 5.0f) {  // same sanity floor as the INA read path
    isLithium = batteryIsLithium();

    // Current compensation: back out open-circuit voltage before the table lookup
    // (Vocv ≈ Vterm − I·Reff). The published SoC-vs-V curve families at different C-rates are
    // the rested curve shifted by this term, so one effective resistance per chemistry replaces
    // them. Reff per 100Ah 12V block including polarization: LFP ~6 mΩ, lead-acid ~12 mΩ;
    // parallel Ah divides it, series blocks multiply it. Clamped so a bogus current reading
    // can't wreck the seed.
    // Reff also scales with temperature (~doubles per −20°C, both chemistries). ambientTemp is
    // still NAN here on a cold boot (BMP388 runs a 4s cycle in ReadAnalogInputs and discards
    // its first sample), so take one blocking forced conversion — ~50ms once, only on the
    // fresh-NVS path, and the board hasn't self-heated yet so it's the best proxy it ever is.
    boardTempF = ambientTemp;
    if (isnan(boardTempF) && !BMP388Disconnected) {
      bmp388.startForcedConversion();
      float t, p, a;
      uint32_t t0 = millis();
      while (millis() - t0 < 250) {
        if (bmp388.getMeasurements(t, p, a)) {
          if (isfinite(t) && t > -40.0f && t < 85.0f) boardTempF = t * 1.8f + 32.0f;
          break;
        }
        delay(5);
      }
    }
    if (!isnan(boardTempF)) {
      float tC = (boardTempF - 32.0f) / 1.8f;
      rTempScale = constrain(expf(0.035f * (25.0f - tC)), 0.5f, 3.0f);
    }

    iBat = getBatteryCurrent();  // positive = charging
    float corr = 0.0f;
    if (BatteryCapacity_Ah > 0) {
      float r100 = (isLithium ? 0.006f : 0.012f) * rTempScale;
      corr = iBat * r100 * (100.0f / (float)BatteryCapacity_Ah) * socM;
      corr = constrain(corr, -0.6f * socM, 0.6f * socM);
    }
    vOcv = voltage - corr;

    // One lookup for every chemistry: capOcvVolt[] is chemistry-matched at commissioning
    // (applyChemistryOcvPreset) and honors a hand-tuned curve. ocvToSoC scales by SYSTEM_VOLTAGE_CLASS/12.
    estimatedSoC = (int)(ocvToSoC(vOcv) + 0.5f);
    if (vOcv >= capOcvVolt[0] * socM) {
      snapVlo = snapVhi = capOcvVolt[0] * socM;
      snapPlo = snapPhi = capOcvSocPct[0];
    } else if (vOcv < capOcvVolt[CAP_OCV_ROWS - 1] * socM) {
      snapVlo = snapVhi = capOcvVolt[CAP_OCV_ROWS - 1] * socM;
      snapPlo = snapPhi = capOcvSocPct[CAP_OCV_ROWS - 1];
    } else {
      for (int i = 0; i < CAP_OCV_ROWS - 1; i++) {
        float vHi = capOcvVolt[i] * socM, vLo = capOcvVolt[i + 1] * socM;
        if (vOcv <= vHi && vOcv >= vLo) {
          snapVhi = vHi; snapPhi = capOcvSocPct[i];
          snapVlo = vLo; snapPlo = capOcvSocPct[i + 1];
          break;
        }
      }
    }
    Serial.printf("SOC SEED: estimated %d%% from %.2fV terminal, %.1fA, %.0f°F (Rx%.2f) -> %.2fV OCV (%s curve, %s Reff)\n",
                  estimatedSoC, voltage, iBat, isnan(boardTempF) ? -99.0f : boardTempF, rTempScale,
                  vOcv, BATTERY_TYPE, isLithium ? "Li" : "Pb");
  } else {
    Serial.printf("SOC SEED: no valid voltage (%.2fV) - defaulting to 50%%\n", voltage);
  }

  SOC_percent = estimatedSoC * 100;
  if (coulombSeedPending) {
    // UpdateBatterySOC re-derives SOC_percent from the coulomb count every tick, so this
    // is the assignment that actually sticks.
    CoulombCount_Ah_scaled = (BatteryCapacity_Ah * SOC_percent) / 100;
    shadowCoulombX100 = CoulombCount_Ah_scaled;  // external seed re-anchors the shadow twin
    coulombSeedPending = false;
  }

  // One-shot record served by /socseed for the commissioning popup; ack cleared so it auto-shows once
  String snap = String("{\"fb\":") + ((voltage > 5.0f) ? "0" : "1")
              + ",\"v\":" + String(voltage, 3)
              + ",\"i\":" + String(iBat, 2)
              + ",\"tF\":" + (isnan(boardTempF) ? String("null") : String(boardTempF, 1))
              + ",\"rs\":" + String(rTempScale, 3)
              + ",\"cap\":" + String(BatteryCapacity_Ah)
              + ",\"sysV\":" + String((int)SYSTEM_VOLTAGE_CLASS)
              + ",\"lith\":" + (isLithium ? "1" : "0")
              + ",\"chem\":\"" + String(BATTERY_TYPE) + "\""
              + ",\"vocv\":" + String(vOcv, 3)
              + ",\"vlo\":" + String(snapVlo, 3) + ",\"plo\":" + String(snapPlo)
              + ",\"vhi\":" + String(snapVhi, 3) + ",\"phi\":" + String(snapPhi)
              + ",\"soc\":" + String(estimatedSoC) + "}";
  settingWrite(NK_SocSeedSnap, snap.c_str());
  settingWrite(NK_SocSeedAck, "0");
}

// A boot that carries a persisted SoC forward runs no new estimate, so a leftover un-acked
// commissioning snapshot would re-open the popup on every reboot/OTA. Only ever flips 0 -> 1.
void socSeedAutoAck() {
  if (settingExists(NK_SocSeedAck) && settingRead(NK_SocSeedAck) != "1") settingWrite(NK_SocSeedAck, "1");
}

void loadNVSData() {
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs_handle);
  if (err != ESP_OK) {
    queueConsoleMessage("NVS: No existing data found, using defaults");
    Serial.println("NVS: No existing data found, using defaults");
    return;
  }

  size_t required_size;
  uint32_t temp_uint32;
  int32_t temp_int32;

  // Session Energy Tracking
  if (nvs_get_u32(nvs_handle, "ChargedEnergy", &temp_uint32) == ESP_OK) ChargedEnergy = temp_uint32;
  if (nvs_get_u32(nvs_handle, "DischrgdEnergy", &temp_uint32) == ESP_OK) DischargedEnergy = temp_uint32;
  if (nvs_get_u32(nvs_handle, "AltChrgdEnergy", &temp_uint32) == ESP_OK) AlternatorChargedEnergy = temp_uint32;
  if (nvs_get_u32(nvs_handle, "SolarEnergy", &temp_uint32) == ESP_OK) SolarChargedEnergy = temp_uint32;
  if (nvs_get_i32(nvs_handle, "AltFuelUsed", &temp_int32) == ESP_OK) AlternatorFuelUsed = temp_int32 / 10.0f;
  if (nvs_get_i32(nvs_handle, "EngineFuel", &temp_int32) == ESP_OK) EngineFuelUsed = temp_int32 / 10.0f;

  if (nvs_get_u32(nvs_handle, "ChrgdEng_AT", &temp_uint32) == ESP_OK) ChargedEnergy_AllTime = temp_uint32;
  if (nvs_get_u32(nvs_handle, "DschrgEng_AT", &temp_uint32) == ESP_OK) DischargedEnergy_AllTime = temp_uint32;
  if (nvs_get_u32(nvs_handle, "AltChrgEng_AT", &temp_uint32) == ESP_OK) AlternatorChargedEnergy_AllTime = temp_uint32;
  if (nvs_get_u32(nvs_handle, "SolarEng_AT", &temp_uint32) == ESP_OK) SolarChargedEnergy_AllTime = temp_uint32;
  if (nvs_get_i32(nvs_handle, "AltFuel_AT", &temp_int32) == ESP_OK) AlternatorFuelUsed_AllTime = temp_int32 / 10.0f;
  if (nvs_get_i32(nvs_handle, "EngFuel_AT", &temp_int32) == ESP_OK) EngineFuelUsed_AllTime = temp_int32 / 10.0f;

  // Session Runtime Tracking
  if (nvs_get_i32(nvs_handle, "EngineRunTime", &temp_int32) == ESP_OK) EngineRunTime = temp_int32;
  if (nvs_get_i32(nvs_handle, "EngineCycles", &temp_int32) == ESP_OK) EngineCycles = temp_int32;
  if (nvs_get_i32(nvs_handle, "AltOnTime", &temp_int32) == ESP_OK) AlternatorOnTime = temp_int32;

  if (nvs_get_i32(nvs_handle, "EngRunTime_AT", &temp_int32) == ESP_OK) EngineRunTime_AllTime = temp_int32;
  if (nvs_get_i32(nvs_handle, "PerfSailSec", &temp_int32) == ESP_OK) perfSailSeconds = temp_int32;
  if (nvs_get_i32(nvs_handle, "PerfMotorSec", &temp_int32) == ESP_OK) perfMotorSeconds = temp_int32;
  if (nvs_get_i32(nvs_handle, "EngCycles_AT", &temp_int32) == ESP_OK) EngineCycles_AllTime = temp_int32;
  if (nvs_get_i32(nvs_handle, "AltOnTime_AT", &temp_int32) == ESP_OK) AlternatorOnTime_AllTime = temp_int32;

  if (nvs_get_i32(nvs_handle, "ChrgCycles", &temp_int32) == ESP_OK) ChargeCycles = temp_int32;

  if (nvs_get_i32(nvs_handle, "ChrgCyc_AT", &temp_int32) == ESP_OK) ChargeCycles_AllTime = temp_int32;

  // App-usage lifetime totals
  if (nvs_get_i32(nvs_handle, "UsgTime_AT", &temp_int32) == ESP_OK) UsageOpenTime_AllTime = temp_int32;
  if (nvs_get_i32(nvs_handle, "UsgOpens_AT", &temp_int32) == ESP_OK) UsageOpens_AllTime = temp_int32;
  if (nvs_get_i32(nvs_handle, "UsgDays_AT", &temp_int32) == ESP_OK) UsageDays_AllTime = temp_int32;

  // Session Travel Statistics
  if (nvs_get_i32(nvs_handle, "TotalDist", &temp_int32) == ESP_OK) TotalDistance = temp_int32;
  if (nvs_get_i32(nvs_handle, "AvgSpeed", &temp_int32) == ESP_OK) AvgSpeed = temp_int32 / 100.0f;

  if (nvs_get_i32(nvs_handle, "TotDist_AT", &temp_int32) == ESP_OK) TotalDistance_AllTime = temp_int32;
  if (nvs_get_i32(nvs_handle, "AvgSpd_AT", &temp_int32) == ESP_OK) AvgSpeed_AllTime = temp_int32 / 100.0f;

  // Longest single trip (lifetime + in-progress recovery). Boot recovery resolves once time syncs (see Step 4 hook).
  if (nvs_get_i32(nvs_handle, "LongTrip_AT", &temp_int32) == ESP_OK) LongestSingleTrip_Nm_AllTime = temp_int32 / 100.0f;
  if (nvs_get_i32(nvs_handle, "CurrTripDist", &temp_int32) == ESP_OK) currentTripDistanceNm = temp_int32 / 100.0f;
  if (nvs_get_u32(nvs_handle, "CurrTripEpch", &temp_uint32) == ESP_OK) currentTripLastUpdateEpoch = temp_uint32;
  if (currentTripDistanceNm > 0.0f || currentTripLastUpdateEpoch > 0) tripPendingRecovery = true;
  if (nvs_get_i32(nvs_handle, "Max24h_AT", &temp_int32) == ESP_OK) Max24hrDistance_AllTime = temp_int32 / 100.0f;
  if (nvs_get_i32(nvs_handle, "DeepAnchor_AT", &temp_int32) == ESP_OK) DeepestAnchorage_Ft_AllTime = temp_int32 / 10.0f;
  if (nvs_get_i32(nvs_handle, "BestUpVMG_AT", &temp_int32) == ESP_OK) best_upwind_vmg_alltime = temp_int32 / 100.0f;
  if (nvs_get_i32(nvs_handle, "GaleHrs_AT", &temp_int32) == ESP_OK) longest_gale_duration_hours_alltime = temp_int32 / 100.0f;
  if (nvs_get_i32(nvs_handle, "faSesPkpk", &temp_int32) == ESP_OK) faSesPkpkWorstA = temp_int32 / 100.0f;
  if (nvs_get_i32(nvs_handle, "faSesPeakA", &temp_int32) == ESP_OK) faSesPeakWorstA = temp_int32 / 100.0f;
  if (nvs_get_i32(nvs_handle, "faSesPeakHz", &temp_int32) == ESP_OK) faSesPeakWorstHz = temp_int32 / 10.0f;
  if (nvs_get_i32(nvs_handle, "faDomAmp", &temp_int32) == ESP_OK) faDomAmpAX100 = (uint16_t)temp_int32;     // Highest Tone in Map: pre-scaled, store/restore raw
  if (nvs_get_i32(nvs_handle, "faDomFreq", &temp_int32) == ESP_OK) faDomFreqHzX10 = (uint16_t)temp_int32;
  if (nvs_get_i32(nvs_handle, "faDomRpm", &temp_int32) == ESP_OK) faDomRpm = (uint16_t)temp_int32;
  // Operating-point context (temp sentinel INT32_MIN -> NAN, "no probe at capture")
  if (nvs_get_i32(nvs_handle, "faDomAmps", &temp_int32) == ESP_OK) faDomAmpsA = temp_int32 / 10.0f;
  if (nvs_get_i32(nvs_handle, "faDomTmp", &temp_int32) == ESP_OK) faDomTempF = (temp_int32 == INT32_MIN) ? NAN : temp_int32 / 10.0f;
  if (nvs_get_i32(nvs_handle, "faDomEp", &temp_int32) == ESP_OK) faDomEpoch = (uint32_t)temp_int32;
  if (nvs_get_i32(nvs_handle, "faSPkAmp", &temp_int32) == ESP_OK) faSesPkpkAmpsA = temp_int32 / 10.0f;
  if (nvs_get_i32(nvs_handle, "faSPkTmp", &temp_int32) == ESP_OK) faSesPkpkTempF = (temp_int32 == INT32_MIN) ? NAN : temp_int32 / 10.0f;
  if (nvs_get_i32(nvs_handle, "faSPkRpm", &temp_int32) == ESP_OK) faSesPkpkRpm = (uint16_t)temp_int32;
  if (nvs_get_i32(nvs_handle, "faSPkEp", &temp_int32) == ESP_OK) faSesPkpkEpoch = (uint32_t)temp_int32;

  // Speed AllTime accumulators (restore so AvgSpeed_AllTime continues correctly after reboot)
  required_size = sizeof(double);
  nvs_get_blob(nvs_handle, "SpdAccum_AT", &speedAccumulator_AllTime, &required_size);
  if (nvs_get_u32(nvs_handle, "SpdTime_AT", &temp_uint32) == ESP_OK) totalSpeedSampleTime_AllTime = temp_uint32;
  if (totalSpeedSampleTime_AllTime > 0)
    AvgSpeed_AllTime = speedAccumulator_AllTime / (float)totalSpeedSampleTime_AllTime;

  // Sailing metrics
  required_size = sizeof(float);
  nvs_get_blob(nvs_handle, "SailDays_AT", &sailing_days_alltime, &required_size);
  required_size = sizeof(float);
  nvs_get_blob(nvs_handle, "SailDist_AT", &sailing_dist_alltime, &required_size);
  required_size = sizeof(float);
  nvs_get_blob(nvs_handle, "AltPwrMax_AT", &alt_power_max_alltime_w, &required_size);
  required_size = sizeof(float);
  nvs_get_blob(nvs_handle, "SolPwrMax_AT", &solar_power_max_alltime_w, &required_size);


  // Average SOC (Session)
  if (nvs_get_i32(nvs_handle, "AvgSOC", &temp_int32) == ESP_OK) AvgSOC = temp_int32 / 100.0f;

  // Average SOC (AllTime - load ACCUMULATORS, then calculate average)
  uint64_t temp_uint64;
  if (nvs_get_u64(nvs_handle, "SocAccum_AT", &temp_uint64) == ESP_OK) {
    socAccumulator_AllTime = temp_uint64 / 100.0;
    Serial.printf("NVS LOAD: socAccumulator_AllTime = %.2f\n", socAccumulator_AllTime);
  }

  if (nvs_get_u32(nvs_handle, "SocTime_AT", &temp_uint32) == ESP_OK) {
    totalSocSampleTime_AllTime = temp_uint32;
    Serial.printf("NVS LOAD: totalSocSampleTime_AllTime = %lu sec (%.1f hours)\n",
                  temp_uint32, temp_uint32 / 3600.0f);
  }

  // Voltage AllTime accumulators (restore so AvgVoltage_AllTime continues correctly after reboot)
  required_size = sizeof(double);
  nvs_get_blob(nvs_handle, "VltAccum_AT", &voltageAccumulator_AllTime, &required_size);
  if (nvs_get_u32(nvs_handle, "VltTime_AT", &temp_uint32) == ESP_OK) totalVoltageSampleTime_AllTime = temp_uint32;
  if (totalVoltageSampleTime_AllTime > 0)
    AvgVoltage_AllTime = voltageAccumulator_AllTime / totalVoltageSampleTime_AllTime;

  if (totalSocSampleTime_AllTime > 0) {
    AvgSOC_AllTime = socAccumulator_AllTime / totalSocSampleTime_AllTime;
    Serial.printf("NVS LOAD: Calculated AvgSOC_AllTime = %.2f%%\n", AvgSOC_AllTime);
  } else {
    Serial.println("NVS LOAD: No AllTime SOC history found");
  }

  // Battery State (with initialization if not found)
  if (nvs_get_i32(nvs_handle, "SOC_percent", &temp_int32) == ESP_OK) {
    SOC_percent = temp_int32;
    Serial.printf("NVS LOAD: SOC_percent = %d (%.2f%%)\n", temp_int32, temp_int32 / 100.0f);
  } else {
    // No saved SoC. IBV is still 0 here (first INA228 read is the forced ReadAnalogInputs() at the
    // end of setup) and battery type/voltage/capacity aren't parsed yet (InitSystemSettings), so an
    // estimate now would always be 0%. Defer to seedSocFromVoltage(); provisional 50% until then.
    SOC_percent = 5000;
    socSeedPending = true;
    Serial.println("NVS LOAD: SOC_percent NOT FOUND - voltage-based seed deferred to end of setup");
  }

  if (nvs_get_i32(nvs_handle, "CoulombCount", &temp_int32) == ESP_OK) {
    CoulombCount_Ah_scaled = temp_int32;
    Serial.printf("NVS LOAD: CoulombCount_Ah_scaled = %d\n", temp_int32);
  } else {
    // Initialize based on estimated SoC. If the SoC seed is deferred, this provisional value is
    // re-derived in seedSocFromVoltage() with the real voltage AND the real bank capacity
    // (BatteryCapacity_Ah here is still the compile-time default, not the vessel-info value).
    CoulombCount_Ah_scaled = (BatteryCapacity_Ah * SOC_percent) / 100;
    coulombSeedPending = true;

    Serial.printf("NVS LOAD: CoulombCount NOT FOUND - initialized to %d based on SoC\n",
                  CoulombCount_Ah_scaled);
  }
  // Shadow (unclamped) coulomb twin: restore its own key; absent (pre-shadow firmware, or a
  // pre-seed boot) → re-anchor to the live count, which just means no clamp evidence carried over.
  if (nvs_get_i32(nvs_handle, "ShadowCoulomb", &temp_int32) == ESP_OK) {
    shadowCoulombX100 = temp_int32;
  } else {
    shadowCoulombX100 = CoulombCount_Ah_scaled;
  }

  // Session Health Stats (restore to prior-session variables)
  if (nvs_get_u32(nvs_handle, "SessionDur", &temp_uint32) == ESP_OK) LastSessionDuration = temp_uint32;
  if (nvs_get_i32(nvs_handle, "MaxLoop", &temp_int32) == ESP_OK) LastSessionMaxLoopTime = temp_int32;
  if (nvs_get_i32(nvs_handle, "MinHeap", &temp_int32) == ESP_OK) lastSessionMinHeap = temp_int32;

  // System Health Counters
  if (nvs_get_i32(nvs_handle, "PowerCycles", &temp_int32) == ESP_OK) totalPowerCycles = temp_int32;

  // Thermal Stress
  required_size = sizeof(float);
  nvs_get_blob(nvs_handle, "InsulDamage", &CumulativeInsulationDamage, &required_size);
  nvs_get_blob(nvs_handle, "GreaseDamage", &CumulativeGreaseDamage, &required_size);
  nvs_get_blob(nvs_handle, "BrushDamage", &CumulativeBrushDamage, &required_size);

  nvs_get_blob(nvs_handle, "ShuntGain", &DynamicShuntGainFactor, &required_size);
  if (nvs_get_u32(nvs_handle, "LastGainTime", &temp_uint32) == ESP_OK) lastGainCorrectionTime = temp_uint32;
  // Temp-comp zero-correction learned equation (hold-last-good survives reboot). Old AltZero/LastZeroTime/
  // LastZeroTemp keys are orphaned and intentionally not read.
  if (nvs_get_i32(nvs_handle, "ZFitValid",  &temp_int32) == ESP_OK) zfValid  = temp_int32;
  if (nvs_get_i32(nvs_handle, "ZFitSensor", &temp_int32) == ESP_OK) zfSensor = temp_int32;
  required_size = sizeof(float);
  nvs_get_blob(nvs_handle, "ZFitC",  &zfC,  &required_size);
  nvs_get_blob(nvs_handle, "ZFitB",  &zfB,  &required_size);
  nvs_get_blob(nvs_handle, "ZFitR2", &zfR2, &required_size);
  if (nvs_get_u32(nvs_handle, "ZFitEpoch", &temp_uint32) == ESP_OK) zfLastEpoch = temp_uint32;

  // IMU Lifetime Counters
  if (nvs_get_u32(nvs_handle, "IMU_Capsize", &temp_uint32) == ESP_OK) imu_capsize_count = temp_uint32;
  if (nvs_get_u32(nvs_handle, "IMU_Pitchpol", &temp_uint32) == ESP_OK) imu_pitchpole_count = temp_uint32;
  if (nvs_get_u32(nvs_handle, "IMU_SlamLife", &temp_uint32) == ESP_OK) imu_slam_count_lifetime = temp_uint32;
  if (nvs_get_u32(nvs_handle, "faAnomalyCnt", &temp_uint32) == ESP_OK) faAnomalyCount = temp_uint32;

  // Sea state minute counters (NVS load).
  if (nvs_get_u32(nvs_handle, "IMU_MinMvGnt",  &temp_uint32) == ESP_OK) imu_min_moving_gentle   = temp_uint32;
  if (nvs_get_u32(nvs_handle, "IMU_MinMvMod",  &temp_uint32) == ESP_OK) imu_min_moving_moderate = temp_uint32;
  if (nvs_get_u32(nvs_handle, "IMU_MinMvRgh",  &temp_uint32) == ESP_OK) imu_min_moving_rough    = temp_uint32;
  if (nvs_get_u32(nvs_handle, "IMU_MinMvExt",  &temp_uint32) == ESP_OK) imu_min_moving_extreme  = temp_uint32;
  if (nvs_get_u32(nvs_handle, "IMU_MinStGnt",  &temp_uint32) == ESP_OK) imu_min_stat_gentle     = temp_uint32;
  if (nvs_get_u32(nvs_handle, "IMU_MinStMod",  &temp_uint32) == ESP_OK) imu_min_stat_moderate   = temp_uint32;
  if (nvs_get_u32(nvs_handle, "IMU_MinStRgh",  &temp_uint32) == ESP_OK) imu_min_stat_rough      = temp_uint32;
  if (nvs_get_u32(nvs_handle, "IMU_MinStExt",  &temp_uint32) == ESP_OK) imu_min_stat_extreme    = temp_uint32;

  // IMU Lifetime Maximums
  required_size = sizeof(float);
  nvs_get_blob(nvs_handle, "IMU_HeelMax", &imu_heel_max_lifetime, &required_size);
  nvs_get_blob(nvs_handle, "IMU_PitchMax", &imu_pitch_max_lifetime, &required_size);
  nvs_get_blob(nvs_handle, "IMU_SlamMax", &imu_slam_peak_lifetime, &required_size);

  // imuMountOrientation loads with the rest of the Vessel Info NVS record in InitSystemSettings.
  // CAPSIZE_THRESHOLD_DEG / PITCHPOLE_THRESHOLD_DEG / SLAM_THRESHOLD_G load from
  // their own NVS "settings" keys in InitSystemSettings (Pattern B).

  // Extrema, environment maxima, and UV forecast
  required_size = sizeof(float); nvs_get_blob(nvs_handle, "MaxSpd",      &MaxSpeed,                         &required_size);
  required_size = sizeof(float); nvs_get_blob(nvs_handle, "MaxSpd_AT",   &MaxSpeed_AllTime,                 &required_size);
  required_size = sizeof(float); nvs_get_blob(nvs_handle, "MAmpsMax",    &MeasuredAmpsMax,                  &required_size);
  required_size = sizeof(float); nvs_get_blob(nvs_handle, "MAmpsMax_AT", &MeasuredAmpsMax_AllTime,          &required_size);
  required_size = sizeof(float); nvs_get_blob(nvs_handle, "RPMMax",      &RPMMax,                           &required_size);
  required_size = sizeof(float); nvs_get_blob(nvs_handle, "RPMMax_AT",   &RPMMax_AllTime,                   &required_size);
  required_size = sizeof(float); nvs_get_blob(nvs_handle, "IBVMax",      &IBVMax,                           &required_size);
  required_size = sizeof(float); nvs_get_blob(nvs_handle, "PeakV_AT",    &PeakVoltage_AllTime,              &required_size);
  required_size = sizeof(float); nvs_get_blob(nvs_handle, "MinV",        &MinVoltage,                       &required_size);
  required_size = sizeof(float); nvs_get_blob(nvs_handle, "MinV_AT",     &MinVoltage_AllTime,               &required_size);
  required_size = sizeof(float); nvs_get_blob(nvs_handle, "BdTmpMaxAt",  &board_temp_max_alltime,           &required_size);
  required_size = sizeof(float); nvs_get_blob(nvs_handle, "BdTmpMinAt",  &board_temp_min_alltime,           &required_size);
  required_size = sizeof(float); nvs_get_blob(nvs_handle, "BaroMaxAt",   &baro_pressure_max_alltime,        &required_size);
  required_size = sizeof(float); nvs_get_blob(nvs_handle, "BaroMinAt",   &baro_pressure_min_alltime,        &required_size);
  // Guard against NaN/Inf written by older firmware before the runtime isnan
  // guard existed. Once a watermark turns NaN, comparisons never update it again.
  if (!isfinite(board_temp_max_alltime))    board_temp_max_alltime    = -999.0f;
  if (!isfinite(board_temp_min_alltime))    board_temp_min_alltime    =  999.0f;
  if (!isfinite(baro_pressure_max_alltime)) baro_pressure_max_alltime =    0.0f;
  if (!isfinite(baro_pressure_min_alltime)) baro_pressure_min_alltime = 9999.0f;
  required_size = sizeof(float); nvs_get_blob(nvs_handle, "MaxTherm",    &MaxTemperatureThermistor,         &required_size);
  required_size = sizeof(float); nvs_get_blob(nvs_handle, "MaxTherm_AT", &MaxTemperatureThermistor_AllTime, &required_size);
  required_size = sizeof(float); nvs_get_blob(nvs_handle, "MaxAltTempF", &MaxAlternatorTemperatureF,        &required_size);
  required_size = sizeof(float); nvs_get_blob(nvs_handle, "MAltTempF_AT", &MaxAlternatorTemperatureF_AllTime, &required_size);
  required_size = sizeof(float); nvs_get_blob(nvs_handle, "MaxWApp_AT",  &max_wind_speed_apparent_alltime,  &required_size);
  required_size = sizeof(float); nvs_get_blob(nvs_handle, "MaxWTr_AT",   &max_wind_speed_true_alltime,      &required_size);
  required_size = sizeof(float); nvs_get_blob(nvs_handle, "UVToday",     &UVToday,                          &required_size);
  required_size = sizeof(float); nvs_get_blob(nvs_handle, "UVTomorrow",  &UVTomorrow,                       &required_size);
  required_size = sizeof(float); nvs_get_blob(nvs_handle, "UVDay2",      &UVDay2,                           &required_size);

  // Barometric pressure 14-day history. Buffer is ps_malloc'd in setup() before this
  // function runs, so it's safe to load directly into it. Missing-key paths leave the
  // buffer zeroed (its memset default), which JS treats as "no sample".
  if (baroPressureHistory) {
    required_size = BARO_HISTORY_SIZE * sizeof(uint16_t);
    nvs_get_blob(nvs_handle, "BaroHist", baroPressureHistory, &required_size);
    nvs_get_u16(nvs_handle, "BaroHistHead", (uint16_t *)&baroHistoryHead);
    uint32_t epochTmp = 0;
    nvs_get_u32(nvs_handle, "BaroHistEpch", &epochTmp);
    baroHistoryLastEpoch = (time_t)epochTmp;
    prev_baroHistoryHead = baroHistoryHead;
  }

  nvs_close(nvs_handle);
  //queueConsoleMessage("NVS: Persistent data loaded successfully");
}
void initNVSCache() {
  // Session cache
  prev_ChargedEnergy = (uint32_t)ChargedEnergy;
  prev_DischrgdEnergy = (uint32_t)DischargedEnergy;
  prev_AltChrgdEnergy = (uint32_t)AlternatorChargedEnergy;
  prev_SolarEnergy = (uint32_t)SolarChargedEnergy;
  prev_AltFuelUsed = (int32_t)(AlternatorFuelUsed * 10);
  prev_EngineFuel = (int32_t)(EngineFuelUsed * 10);
  prev_EngineRunTime = (int32_t)EngineRunTime;
  prev_EngineCycles = (int32_t)EngineCycles;
  prev_AltOnTime = (int32_t)AlternatorOnTime;
  prev_ChargeCycles = (int32_t)ChargeCycles;
  prev_TotalDist = (int32_t)TotalDistance;
  prev_AvgSpeed = (int32_t)(AvgSpeed * 100);
  prev_AvgSOC = (int32_t)(AvgSOC * 100);

  prev_ChargedEnergy_AllTime = (uint32_t)ChargedEnergy_AllTime;
  prev_DischrgdEnergy_AllTime = (uint32_t)DischargedEnergy_AllTime;
  prev_AltChrgdEnergy_AllTime = (uint32_t)AlternatorChargedEnergy_AllTime;
  prev_SolarEnergy_AllTime = (uint32_t)SolarChargedEnergy_AllTime;
  prev_AltFuelUsed_AllTime = (int32_t)(AlternatorFuelUsed_AllTime * 10);
  prev_EngineFuel_AllTime = (int32_t)(EngineFuelUsed_AllTime * 10);
  prev_EngineRunTime_AllTime = (int32_t)EngineRunTime_AllTime;
  prev_perfSailSeconds = (int32_t)perfSailSeconds;
  prev_perfMotorSeconds = (int32_t)perfMotorSeconds;
  prev_EngineCycles_AllTime = (int32_t)EngineCycles_AllTime;
  prev_AltOnTime_AllTime = (int32_t)AlternatorOnTime_AllTime;
  prev_ChargeCycles_AllTime = (int32_t)ChargeCycles_AllTime;
  prev_UsageOpenTime_AllTime = (int32_t)UsageOpenTime_AllTime;
  prev_UsageOpens_AllTime = (int32_t)UsageOpens_AllTime;
  prev_UsageDays_AllTime = (int32_t)UsageDays_AllTime;
  prev_TotalDist_AllTime = (int32_t)TotalDistance_AllTime;
  prev_AvgSpeed_AllTime = (int32_t)(AvgSpeed_AllTime * 100);
  prev_spdAccum_AllTime = speedAccumulator_AllTime;
  prev_spdTime_AllTime  = (uint32_t)totalSpeedSampleTime_AllTime;
  prev_vltAccum_AllTime = voltageAccumulator_AllTime;
  prev_vltTime_AllTime  = (uint32_t)totalVoltageSampleTime_AllTime;
  prev_AvgSOC_AllTime = (int32_t)(AvgSOC_AllTime * 100);

  // Battery state
  prev_SOC_percent = (int32_t)SOC_percent;
  prev_CoulombCount = (int32_t)CoulombCount_Ah_scaled;
  prev_ShadowCoulomb = (int32_t)shadowCoulombX100;

  prev_SessionDur = (uint32_t)CurrentSessionDuration;
  prev_MaxLoop = (int32_t)MaxLoopTime;
  prev_MinHeap = (int32_t)MinFreeHeap;

  // System health
  prev_PowerCycles = (int32_t)totalPowerCycles;

  // Thermal stress
  prev_InsulDamage = CumulativeInsulationDamage;
  prev_GreaseDamage = CumulativeGreaseDamage;
  prev_BrushDamage = CumulativeBrushDamage;

  prev_ShuntGain = DynamicShuntGainFactor;
  prev_LastGainTime = (uint32_t)lastGainCorrectionTime;
  prev_zfValid = zfValid;
  prev_zfSensor = zfSensor;
  prev_zfC = zfC;
  prev_zfB = zfB;
  prev_zfR2 = zfR2;
  prev_zfLastEpoch = zfLastEpoch;

  // Sailing metrics cached
  prev_sailing_days_alltime = sailing_days_alltime;
  prev_sailing_dist_alltime = sailing_dist_alltime;
  prev_alt_power_max_alltime_w = alt_power_max_alltime_w;
  prev_solar_power_max_alltime_w = solar_power_max_alltime_w;

  prev_imu_capsize_count = imu_capsize_count;
  prev_imu_pitchpole_count = imu_pitchpole_count;
  prev_imu_slam_count_lifetime = imu_slam_count_lifetime;
  prev_faAnomalyCount = faAnomalyCount;
  prev_imu_heel_max_lifetime = imu_heel_max_lifetime;
  prev_imu_pitch_max_lifetime = imu_pitch_max_lifetime;
  prev_imu_slam_peak_lifetime = imu_slam_peak_lifetime;
  // Sea state minute counters (prev_ shadow sync).
  prev_imu_min_moving_gentle   = imu_min_moving_gentle;
  prev_imu_min_moving_moderate = imu_min_moving_moderate;
  prev_imu_min_moving_rough    = imu_min_moving_rough;
  prev_imu_min_moving_extreme  = imu_min_moving_extreme;
  prev_imu_min_stat_gentle     = imu_min_stat_gentle;
  prev_imu_min_stat_moderate   = imu_min_stat_moderate;
  prev_imu_min_stat_rough      = imu_min_stat_rough;
  prev_imu_min_stat_extreme    = imu_min_stat_extreme;
  prev_MaxSpeed             = MaxSpeed;
  prev_MaxSpeed_AllTime     = MaxSpeed_AllTime;
  prev_LongestTrip_AT       = (int32_t)(LongestSingleTrip_Nm_AllTime * 100);
  prev_CurrTripDist         = (int32_t)(currentTripDistanceNm * 100);
  prev_CurrTripEpoch        = currentTripLastUpdateEpoch;
  prev_Max24hrDist_AT       = (int32_t)(Max24hrDistance_AllTime * 100);
  prev_DeepAnchor_AT        = (int32_t)(DeepestAnchorage_Ft_AllTime * 10);
  prev_BestUpVMG_AT         = (int32_t)(best_upwind_vmg_alltime * 100);
  prev_GaleHrs_AT           = (int32_t)(longest_gale_duration_hours_alltime * 100);
  prev_faSesPkpk            = (int32_t)(faSesPkpkWorstA * 100);
  prev_faSesPeakA           = (int32_t)(faSesPeakWorstA * 100);
  prev_faSesPeakHz          = (int32_t)(faSesPeakWorstHz * 10);
  prev_faDomAmp             = (int32_t)faDomAmpAX100;
  prev_faDomFreq            = (int32_t)faDomFreqHzX10;
  prev_faDomRpm             = (int32_t)faDomRpm;
  prev_faDomAmps            = (int32_t)(faDomAmpsA * 10);
  prev_faDomTmp             = (isnan(faDomTempF) ? INT32_MIN : (int32_t)(faDomTempF * 10));
  prev_faDomEp              = (int32_t)faDomEpoch;
  prev_faSPkAmp             = (int32_t)(faSesPkpkAmpsA * 10);
  prev_faSPkTmp             = (isnan(faSesPkpkTempF) ? INT32_MIN : (int32_t)(faSesPkpkTempF * 10));
  prev_faSPkRpm             = (int32_t)faSesPkpkRpm;
  prev_faSPkEp              = (int32_t)faSesPkpkEpoch;
  prev_MeasAmpsMax          = MeasuredAmpsMax;
  prev_MeasAmpsMax_AllTime  = MeasuredAmpsMax_AllTime;
  prev_RPMMax               = RPMMax;
  prev_RPMMax_AllTime       = RPMMax_AllTime;
  prev_IBVMax               = IBVMax;
  prev_PeakV_AllTime        = PeakVoltage_AllTime;
  prev_MinVoltage           = MinVoltage;
  prev_MinVoltage_AllTime   = MinVoltage_AllTime;
  prev_board_temp_max       = board_temp_max_alltime;
  prev_board_temp_min       = board_temp_min_alltime;
  prev_baro_max             = baro_pressure_max_alltime;
  prev_baro_min             = baro_pressure_min_alltime;
  prev_MaxTempTherm         = MaxTemperatureThermistor;
  prev_MaxTempTherm_AllTime = MaxTemperatureThermistor_AllTime;
  prev_MaxAltTempF          = MaxAlternatorTemperatureF;
  prev_MaxAltTempF_AllTime  = MaxAlternatorTemperatureF_AllTime;
  prev_MaxWindApp           = max_wind_speed_apparent_alltime;
  prev_MaxWindTrue          = max_wind_speed_true_alltime;
  prev_UVToday              = UVToday;
  prev_UVTomorrow           = UVTomorrow;
  prev_UVDay2               = UVDay2;
}
// Helper function to get human-readable subtype names
String getSubtypeString(esp_partition_type_t type, esp_partition_subtype_t subtype) {
  if (type == ESP_PARTITION_TYPE_APP) {
    switch (subtype) {
      case ESP_PARTITION_SUBTYPE_APP_FACTORY: return "factory";
      case ESP_PARTITION_SUBTYPE_APP_OTA_0: return "ota_0";
      case ESP_PARTITION_SUBTYPE_APP_OTA_1: return "ota_1";
      default: return "app_unknown";
    }
  } else if (type == ESP_PARTITION_TYPE_DATA) {
    switch (subtype) {
      case ESP_PARTITION_SUBTYPE_DATA_NVS: return "nvs";
      case ESP_PARTITION_SUBTYPE_DATA_OTA: return "otadata";
      case ESP_PARTITION_SUBTYPE_DATA_LITTLEFS: return "littlefs";
      case ESP_PARTITION_SUBTYPE_DATA_SPIFFS: return "spiffs";
      case ESP_PARTITION_SUBTYPE_DATA_COREDUMP: return "coredump";
      default: return "data_unknown";
    }
  }
  return "unknown";
}
// Helper to verify expected partitions exist with correct sizes
void checkExpectedPartition(const char *name, esp_partition_type_t type, esp_partition_subtype_t subtype, size_t expectedSize) {
  const esp_partition_t *partition = esp_partition_find_first(type, subtype, name);
  if (partition) {
    bool sizeOK = (partition->size == expectedSize);
    Serial.printf("  ✅ %s: Found at 0x%X, size %d bytes %s\n",
                  name, partition->address, partition->size,
                  sizeOK ? "✓" : "❌ SIZE MISMATCH!");
  } else {
    Serial.printf("  ❌ %s: NOT FOUND!\n", name);
  }
}
