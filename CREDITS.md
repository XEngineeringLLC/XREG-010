# Credits & Third-Party Assets

## Images

### Loading-screen sailboat — `web_assets/boat_sloop.png`

- **Title:** "Sloop under full sail, close reaching, showing its fractional rig"
- **Author:** Stefan Ivanovich
- **Source:** <https://commons.wikimedia.org/wiki/File:Sloop_under_full_sail,_close_reaching,_showing_its_fractional_rig.jpg>
- **License:** CC BY-SA 3.0 — <https://creativecommons.org/licenses/by-sa/3.0/>
- **Modifications:** Background removed (cut out for use as the app's loading-screen graphic).

This project is open source. Per the Share-Alike (SA) term of CC BY-SA 3.0, this
derived image asset is likewise made available under CC BY-SA 3.0.

## Firmware libraries (custom `_xeng` forks, in `~/Documents/Arduino/libraries/`)

### VE.Direct frame handler — `VeDirectFrameHandler_xeng`

- **Origin:** VeDirectFrameHandler by Chris Terwilliger, derived from the Victron Energy
  reference implementation
- **Source:** <https://github.com/cterwilliger/VeDirectFrameHandler>
- **License:** MIT (Copyright (c) 2019 Victron Energy BV; portions Copyright (c) 2020
  Chris Terwilliger) — full notice kept in the library source and LICENSE file.
- **Modifications:** frameCounter for checksum-valid new-frame gating; corrupted-stream
  bounds fixes (frameIndex clamp in textRxEvent, overlong-name truncate-terminate,
  mName/mValue zero-init).

### PID controller — `PID_v1_xeng`

- **Origin:** Arduino PID Library v1.2.1 by Brett Beauregard
- **Source:** <https://github.com/br3ttb/Arduino-PID-Library>
- **License:** MIT — notice kept in the library source.
- **Modifications:** tracking anti-windup and integral up-restraint methods added;
  `Compute()` reworked to run on every call and scale its integral and derivative gains to
  the measured interval (described in the library source).

### NMEA2000 ESP32-S3 CAN driver — `NMEA2000_esp32_xeng`

- **Origin:** NMEA2000_twai by Svante Karlsson (TWAI driver for the NMEA2000 library by
  Timo Lappalainen, which is used unmodified alongside it)
- **Source:** NMEA2000_twai (GitHub; installed copy carries no upstream URL in its manifest)
- **License:** MIT (Copyright (c) 2024 Svante Karlsson) — notice kept in the library LICENSE file.
- **Modifications:** transmit is unconditionally non-blocking (stock waited up to 100 ms
  per fast-packet frame when the TWAI queue was full, e.g. with no bus attached); failed
  frames fall through to the core library's send-frame retry ring. A raw-RX tap hook
  (`SetRawRxHook`) surfaces every received frame to the sketch for the RV-C / proprietary
  decodes the core library's dispatch cannot deliver, and a raw-TX hook (`SetRawTxHook`) does
  the same for every frame we queue. The driver publishes a bus-health snapshot (`BusStats`:
  the TWAI status fields once a second, lifetime error counters, frame counts, bus-off count).
  Bus-off is recovered with the IDF's documented `twai_initiate_recovery` path instead of the
  stock driver reinstall, which freed the driver object while the receive path could still be
  using it.
  The error-monitor task's stack is allocated in PSRAM.

### Async web server — `ESP_Async_WebServer_xeng`

- **Origin:** ESP Async WebServer 3.11.0 by ESP32Async (Hristo Gochkov, Mathieu Carbou, Emil
  Muratov, Will Miles)
- **Source:** <https://github.com/ESP32Async/ESPAsyncWebServer>
- **License:** LGPL-3.0-or-later. Notice kept in every source file and the library LICENSE file.
- **Modifications:** server-sent event messages are held in PSRAM. Each message's text and its
  reference count share one PSRAM block, and each client's queue is a ring of slots in PSRAM.
  Stock kept the text, the reference count, and the queue node in internal RAM. A message that
  finds no PSRAM is dropped. The
  sender's task drives the socket only while a client's queue is under 4 messages, however deep
  the queue may grow. Main header renamed `ESPAsyncWebServer_xeng.h`; class names and API
  otherwise unchanged. Only `AsyncEventSource.h` and `AsyncEventSource.cpp` differ from upstream.

### LSM6DSOX IMU driver — `LSM6DSOX_xeng`

- **Origin:** STM32duino LSM6DSOX 2.3.4 by SRA / STMicroelectronics
- **Source:** <https://github.com/stm32duino/LSM6DSOX>
- **License:** BSD 3-Clause (Copyright (c) 2019 STMicroelectronics) — full notice kept at the
  top of the library source.
- **Modifications:** the I2C branches of `IO_Read` / `IO_Write` now return the real transfer
  result. Upstream returned 0 (OK) on that path regardless of what the bus did — it discarded
  `endTransmission()` and never checked how many bytes `requestFrom()` delivered — so every
  `LSM6DSOX_OK` test in a caller was vacuous over I2C, and a failed read silently left the
  caller's buffer holding the previous contents. Also bounds the receive loop to the requested
  length. SPI path, class name and API untouched.

## Design prior art (patterns, not code)

### DVCC-style charge-limit follow (CVL/CCL) — trust state machine and arbitration

- The authority arbitration, settling period, silence revert, and "ignore an authority
  publishing implausible values" latch follow the Remote Battery Master handling that
  **Al Thomason** designed and field-proved in the open-source VSR / OSEnergy alternator
  regulator (`OSEnergy_CAN.cpp`, the ancestor of the Wakespeed WS500) —
  <https://github.com/OSEnergy/OSEnergy>. Patterns reimplemented, no code copied.
- The RV-C message layouts come from the published RV-C application layer (RVIA) via the
  OSEnergy design guide. The Victron VREG carrier framing (VREGs over 0xEF00 proprietary
  frames) was confirmed against the archived **Revatek** alternator regulator source
  (Apache 2.0, Copyright 2026 Revatek LLC) —
  <https://github.com/grevelle/revatek-alternator-regulator>. Protocol facts only, no code
  copied. (Note: that repository's PATENTS.md reserves patent rights over its *cloud
  connectivity / dual-alternator / multi-source coordination* features — none of which this
  decode uses.)

### RV-C producer (transmit)

- Every DGN composer is written from the published RV-C application layer (RVIA, Full
  Application Layer revision of 31 July 2025). **Al Thomason**'s `RV-C` library
  (<https://github.com/thomasonw/RV-C>) is GPL-3.0 and was read as *documentation only*, for two
  conventions it proposed ahead of ratification: the charger-type nibble in the upper 4 bits of
  the charger instance byte (3 = engine alternator, which is what a WS500 follows), and the
  CHARGER_STATUS_2 extension that the 2025 revision has since ratified as DGN 1FEA3h. No code
  copied — the licenses are incompatible, and the byte layouts come from the specification.
