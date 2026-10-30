
// XREG-010
// Copyright (C) 2026 X Engineering LLC
// Contact: joe@xengineering.net

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 3 of the License.

// This program is distributed WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

// BEST-EVER FRONT shared engine, generic over NAXIS (alt 4-D, sail 3-D, motor 3-D). Templates can't be
// auto-prototyped: this block must stay ABOVE the alt/boat front code that instantiates it. FRONT_* #defines
// stay in Xregulator.ino (globals there use them). Spec: BEST_EVER_FRONT_SPEC.md §2/§5, IMPLEMENTATION_PLAN.md §2.

// Per-axis steadiness knobs (live-tunable): deviation bound + how long it must hold.
struct EpAxisCfg { float tol; float steadySec; };

// x[] = steadiness axes (band-checked and averaged); ex[] = passenger inputs averaged but NOT band-checked,
// kept for cloud forensics (alt raw duty; motor raw AWS/AWA); out = measured output, averaged.
template <int NAXIS>
struct RawSample { float x[NAXIS]; float ex[2]; float out; uint32_t tMs; };

// One episode point == one front support point. x[] are SURFACE coords (may be derived from the steadiness
// axes, e.g. excitation from duty); ex[] are run-averaged raw extras for the cloud, unused by eval; y = output.
template <int NAXIS>
struct FrontPoint { float x[NAXIS]; float ex[2]; float y; uint32_t nSamp; uint32_t tEmit; };

// Steadiness detector: an axis is steady when max-min over its trailing steadySec window <= tol (EP_BAND,
// monotonic deques), so a brief excursion ages out instead of zeroing dwell. Alt-health uses
// EP_STRAIGHT/EP_EXTERNAL axes (altEpisodeSyncCfg); vessel-performance stays EP_BAND.

#define EP_FEED_DT_MS      100    // steadiness/average update cadence (10 Hz), decimated from the fold
#define EP_AVG_WIN_MS     2000    // default trailing boxcar width for the emitted average (per-instance avgWinMs overrides)
#define EP_EMIT_PERIOD_MS 1000    // max emit rate while steady; keep <= the ~2/s consumer drain (max-per-cell dedup absorbs repeats)

// Sliding-window extremum over a trailing time window (monotonic / "ascending-minima" deque).
// keepMax=true → front() is the window max; false → the window min. Ring storage is bound in init.
struct MonoDeque {
  uint32_t *ts; float *val; int cap, head, tail; bool keepMax;
  void bind(uint32_t *t, float *v, int c, bool isMax) { ts = t; val = v; cap = c; head = tail = 0; keepMax = isMax; }
  void clear() { head = tail = 0; }
  bool empty() const { return head == tail; }
  float front() const { return val[head]; }
  void push(uint32_t t, float v, uint32_t windowMs) {
    while (head != tail) {                                       // drop back entries this one dominates
      int b = (tail - 1 + cap) % cap;
      if (keepMax ? (val[b] <= v) : (val[b] >= v)) tail = b; else break;
    }
    ts[tail] = t; val[tail] = v; tail = (tail + 1) % cap;
    if (tail == head) head = (head + 1) % cap;                  // ring guard (sized to window; shouldn't trigger)
    while (head != tail && (uint32_t)(t - ts[head]) > windowMs) head = (head + 1) % cap;   // evict stale front
  }
};

// BAND: max-min within tol. STRAIGHT: rms departure from a fitted line, so a smooth ramp (boat accelerating)
// qualifies. EXTERNAL: caller supplies the verdict, for a test that must survive the eligibility barrier.
enum EpGateMode : uint8_t { EP_BAND = 0, EP_STRAIGHT = 1, EP_EXTERNAL = 2 };

#define EP_FIT_N     64    // straightness ring depth = 6.4 s at EP_FEED_DT_MS; power of two so indexing is a mask
#define EP_LEAD_MAX  11    // lead-pairing delay depth (samples) — 0..10 × EP_FEED_DT_MS, i.e. up to 1.0 s

// Least-squares straightness over a trailing window: rms departure from the best-fit line, plus its slope.
// Internal-RAM ring (runs in the control tick; PSRAM would cost more than the math); refit each tick so it
// cannot drift. Values centred on the window's first sample for float32 headroom; double is software-emulated on S3.
struct LineFit {
  float    v[EP_FIT_N];
  uint32_t t[EP_FIT_N];
  int      head, n;
  float    rms, slope, mean;        // last fit: rms departure, slope per SECOND, window mean
  void clear() { head = 0; n = 0; rms = 0; slope = 0; mean = 0; }
  void push(uint32_t tMs, float x) {
    v[head] = x; t[head] = tMs; head = (head + 1) & (EP_FIT_N - 1);
    if (n < EP_FIT_N) n++;
  }
  // Fit the samples within the trailing winMs. Returns how many were used (< 3 → rms/slope invalid).
  int fit(uint32_t nowMs, uint32_t winMs) {
    int cnt = 0;
    while (cnt < n) {
      int idx = (head - 1 - cnt) & (EP_FIT_N - 1);
      if ((uint32_t)(nowMs - t[idx]) > winMs) break;
      cnt++;
    }
    if (cnt < 3) {
      rms = 0.0f; slope = 0.0f;
      mean = (n > 0) ? v[(head - 1) & (EP_FIT_N - 1)] : 0.0f;
      return cnt;
    }
    int      oldest = (head - cnt) & (EP_FIT_N - 1);
    uint32_t t0 = t[oldest];
    float    y0 = v[oldest];                 // centring reference: keeps every accumulator O(band), not O(rpm²)
    float sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (int i = 0; i < cnt; i++) {
      int   idx = (oldest + i) & (EP_FIT_N - 1);
      float x = (float)((uint32_t)(t[idx] - t0)) * 0.001f;    // seconds, so the slope is per second
      float y = v[idx] - y0;
      sx += x; sy += y; sxx += x * x; sxy += x * y;
    }
    float fn = (float)cnt;
    float den = fn * sxx - sx * sx;
    float b = (fabsf(den) > 1e-9f) ? ((fn * sxy - sx * sy) / den) : 0.0f;
    float a = (sy - b * sx) / fn;
    float sse = 0;                            // residuals in a second pass rather than by the
    for (int i = 0; i < cnt; i++) {           // Syy − a·Sy − b·Sxy identity, which cancels catastrophically
      int   idx = (oldest + i) & (EP_FIT_N - 1);
      float x = (float)((uint32_t)(t[idx] - t0)) * 0.001f;
      float r = (v[idx] - y0) - (a + b * x);
      sse += r * r;
    }
    slope = b; rms = sqrtf(sse / fn); mean = y0 + sy / fn;
    return cnt;
  }
};

// Lead pairing: alternator output answers where the engine is GOING, so delay the responding channels by a
// fixed number of feed ticks. Fed only behind the EP_FEED_DT_MS decimation, never the ~75 Hz fold path.
struct LeadLine {
  float v[EP_LEAD_MAX];
  int   head, n, delay;
  void clear() { head = 0; n = 0; }
  void setDelay(int d) {
    if (d < 0) d = 0;
    if (d > EP_LEAD_MAX - 1) d = EP_LEAD_MAX - 1;
    if (d != delay) { delay = d; clear(); }
  }
  // Push x and return the sample `delay` ticks old — or the OLDEST one held while the line is still
  // filling, so a fresh run pairs like with like instead of holding a zero.
  float push(float x) {
    v[head] = x; head = (head + 1) % EP_LEAD_MAX;
    if (n < EP_LEAD_MAX) n++;
    int back = (delay < n) ? delay : (n - 1);
    return v[(head - 1 - back + EP_LEAD_MAX) % EP_LEAD_MAX];
  }
};

template <int NAXIS>
struct Episode {
  EpAxisCfg cfg[NAXIS];             // per-axis {tol, steadySec}; synced by the caller each fold
  EpAxisCfg outCfg;                 // optional output-steadiness band (outCfg.tol <= 0 → disabled)
  uint32_t  minRunMs;               // emit floor: eligible data must span this long since the last hard barrier (0 = off)

  // Per-axis qualification, index [NAXIS] = output. Defaults are EP_BAND (sail/motor set none). Set
  // gateMode[] and fit[] BEFORE init(): deques are allocated only for band axes.
  uint8_t   gateMode[NAXIS + 1] = {};      // EpGateMode
  LineFit  *fit[NAXIS + 1] = {};           // EP_STRAIGHT axes: caller-owned fit state (internal RAM)
  LeadLine *lead[NAXIS + 1] = {};          // optional per-axis feed delay (lead pairing)
  bool      extOk[NAXIS + 1] = {};         // EP_EXTERNAL axes: the caller's verdict for this tick
  float     slopeMax[NAXIS + 1] = {};      // EP_STRAIGHT: |slope| cap per second (≤ 0 = uncapped)
  float     tolPct[NAXIS + 1] = {};        // EP_STRAIGHT: tolerance as % of the window mean, floored at cfg tol (0 = absolute only)
  float     slopePct[NAXIS + 1] = {};      // EP_STRAIGHT: |slope| cap as % of the window mean per second, floored at slopeMax (0 = absolute only)
  bool      emitTrimSkip[NAXIS + 1] = {};  // axes the emit-time trimmed-range re-test does NOT apply to

  MonoDeque maxDQ[NAXIS + 1], minDQ[NAXIS + 1];   // per axis + [NAXIS] = output band: sliding window max/min
  int       dqCap[NAXIS + 1];       // each deque ring's capacity (samples), sized from maxDwellSec in init
  uint32_t  dataStartMs;            // time of the first eligible sample after the last hard barrier
  bool      haveData, ready;
  bool      axisSteady[NAXIS + 1];  // per-axis (+ output band) "steady now"; alt-health session gate uses all but temp

  RawSample<NAXIS> *avgRing; int avgCap;          // trailing boxcar buffer (the caller's PSRAM ring)
  int       ringHead, ringCount;    // boxcar head/count (named ringHead/ringCount for caller compatibility)
  double    avgSumX[NAXIS], avgSumEx[2], avgSumOut;
  uint32_t  count;                  // boxcar sample count while steady, else 0 (caller reads count>0 as "steady")
  uint32_t  lastFeedMs, lastEmitMs;
  uint32_t  avgWinMs = EP_AVG_WIN_MS;   // boxcar width; an emit requires the boxcar itself to be clean (span + trimmed ranges)
  // Emit-starvation triage (boot-cumulative, NOT reset by clearRun): per axis, out-of-band ticks and
  // excursion starts, counted only once that axis's dwell has filled. Mean excursion length =
  // axUnqual/axExcur × EP_FEED_DT_MS. Dumped in /altdebug.csv.
  uint32_t  axUnqual[NAXIS + 1] = {}, axExcur[NAXIS + 1] = {}, feedTicks = 0;
  bool      axWasUnq[NAXIS + 1] = {};
  // Gate-blame (boot-cumulative, in /altdebug.csv): soleBlock = ticks where only this axis was out-of-band
  // with dwell filled; emitDirty = emits vetoed by its trimmed range; emitSpanShort = boxcar not yet avgWinMs.
  uint32_t  blameTicks = 0, soleBlock[NAXIS + 1] = {};
  uint32_t  emitDirty[NAXIS + 1] = {}, emitSpanShort = 0;

  // maxDwellSec[NAXIS+1] sizes each band axis's deque (ps_malloc'd here, EP_BAND axes only, so set
  // gateMode[] first). ringBuf is the caller's PSRAM boxcar ring.
  void init(RawSample<NAXIS> *ringBuf, int ringCap, const float *maxDwellSec) {
    avgRing = ringBuf; avgCap = ringCap;
    outCfg = { 0, 0 }; minRunMs = 0;
    ready = true;
    for (int a = 0; a < NAXIS + 1; a++)
      if (gateMode[a] == EP_STRAIGHT && !fit[a]) gateMode[a] = EP_BAND;   // no fit state → band rule, and a deque below
    for (int a = 0; a < NAXIS + 1; a++) {
      if (gateMode[a] != EP_BAND) { dqCap[a] = 0; continue; }
      int cap = (int)(maxDwellSec[a] * (1000.0f / EP_FEED_DT_MS)) + 4;
      if (cap < 4) cap = 4;
      dqCap[a] = cap;
      uint32_t *tMax = (uint32_t *)ps_malloc((size_t)cap * sizeof(uint32_t));
      float    *vMax = (float    *)ps_malloc((size_t)cap * sizeof(float));
      uint32_t *tMin = (uint32_t *)ps_malloc((size_t)cap * sizeof(uint32_t));
      float    *vMin = (float    *)ps_malloc((size_t)cap * sizeof(float));
      if (!tMax || !vMax || !tMin || !vMin) { ready = false; return; }
      maxDQ[a].bind(tMax, vMax, cap, true);
      minDQ[a].bind(tMin, vMin, cap, false);
    }
    clearRun();
  }

  // Reset all detector state. Safe even if init's alloc failed (clear() only zeroes indices).
  void clearRun() {
    for (int a = 0; a < NAXIS + 1; a++) {
      if (gateMode[a] == EP_BAND) { maxDQ[a].clear(); minDQ[a].clear(); }
      if (fit[a]) fit[a]->clear();
      if (lead[a]) lead[a]->clear();
    }
    ringHead = 0; ringCount = 0; count = 0;
    for (int a = 0; a < NAXIS; a++) avgSumX[a] = 0;
    for (int a = 0; a < NAXIS + 1; a++) axisSteady[a] = false;
    avgSumEx[0] = avgSumEx[1] = 0; avgSumOut = 0;
    dataStartMs = 0; haveData = false; lastFeedMs = 0; lastEmitMs = 0;
  }

  // Feed one sample. eligible=false → hard barrier (drop all in-band history; a run can't span the
  // gap). Returns true + fills `out` when a steady point should be recorded (rate-limited).
  bool feed(bool eligible, const RawSample<NAXIS> &s, FrontPoint<NAXIS> *out) {
    if (!ready) return false;
    if (!eligible) { clearRun(); return false; }

    // Decimate the steadiness/averaging update to EP_FEED_DT_MS (the fold may run much faster).
    if (haveData && (uint32_t)(s.tMs - lastFeedMs) < EP_FEED_DT_MS) return false;
    // A feed gap is a hard barrier: some callers stop feeding without clearRun (stale NMEA, pause,
    // IgnoreTemperature), and post-gap deques hold one trivially in-band sample against pre-gap dwell.
    if (haveData && (uint32_t)(s.tMs - lastFeedMs) > 5u * EP_FEED_DT_MS) clearRun();
    lastFeedMs = s.tMs;
    if (!haveData) { dataStartMs = s.tMs; haveData = true; }

    // Lead pairing only here: the delay is counted in decimated ticks; everything downstream sees delayed channels.
    RawSample<NAXIS> ls = s;
    for (int a = 0; a < NAXIS; a++) if (lead[a]) ls.x[a] = lead[a]->push(s.x[a]);
    if (lead[NAXIS]) ls.out = lead[NAXIS]->push(s.out);

    // Per-axis qualification over each axis's own trailing window (clamped to storage).
    feedTicks++;
    int nBad = 0, badAxis = -1; bool badDwellOk = false;
    bool qualified = true;
    for (int a = 0; a < NAXIS; a++) {
      bool dwellOk, inBand;
      if (gateMode[a] == EP_EXTERNAL) {
        dwellOk = true; inBand = extOk[a];          // the caller owns this axis's window entirely
      } else if (gateMode[a] == EP_STRAIGHT && fit[a]) {
        uint32_t win = (uint32_t)(cfg[a].steadySec * 1000.0f);
        uint32_t winMax = (uint32_t)((EP_FIT_N - 2) * EP_FEED_DT_MS);
        if (win > winMax) win = winMax;
        fit[a]->push(ls.tMs, ls.x[a]);
        int nf = fit[a]->fit(ls.tMs, win);
        float tol = cfg[a].tol;                     // absolute floor…
        if (tolPct[a] > 0) {                        // …raised to a share of the reading where that is larger
          float rel = tolPct[a] * 0.01f * fabsf(fit[a]->mean);
          if (rel > tol) tol = rel;
        }
        float slim = slopeMax[a];                   // absolute cap…
        if (slopePct[a] > 0) {                      // …raised to a share of the reading per second where that is larger
          float rels = slopePct[a] * 0.01f * fabsf(fit[a]->mean);
          if (rels > slim) slim = rels;
        }
        dwellOk = ((uint32_t)(ls.tMs - dataStartMs) >= win);
        inBand  = (nf >= 3) && (fit[a]->rms <= tol)
                  && (slim <= 0.0f || fabsf(fit[a]->slope) <= slim);
      } else {
        uint32_t win = (uint32_t)(cfg[a].steadySec * 1000.0f);
        uint32_t winMax = (uint32_t)((dqCap[a] - 2) * EP_FEED_DT_MS);   // can't window more than the ring holds
        if (win > winMax) win = winMax;
        maxDQ[a].push(ls.tMs, ls.x[a], win);
        minDQ[a].push(ls.tMs, ls.x[a], win);
        dwellOk = ((uint32_t)(ls.tMs - dataStartMs) >= win);
        inBand  = (maxDQ[a].front() - minDQ[a].front() <= cfg[a].tol);
      }
      if (dwellOk) {
        if (!inBand) { axUnqual[a]++; if (!axWasUnq[a]) axExcur[a]++; }
        axWasUnq[a] = !inBand;
      }
      axisSteady[a] = dwellOk && inBand;
      if (!axisSteady[a]) { qualified = false; nBad++; badAxis = a; badDwellOk = dwellOk; }
    }
    if (outCfg.tol > 0) {
      bool dwellOk, inBand;
      if (gateMode[NAXIS] == EP_STRAIGHT && fit[NAXIS]) {
        uint32_t win = (uint32_t)(outCfg.steadySec * 1000.0f);
        uint32_t winMax = (uint32_t)((EP_FIT_N - 2) * EP_FEED_DT_MS);
        if (win > winMax) win = winMax;
        fit[NAXIS]->push(ls.tMs, ls.out);
        int nf = fit[NAXIS]->fit(ls.tMs, win);
        float tol = outCfg.tol;
        if (tolPct[NAXIS] > 0) {
          float rel = tolPct[NAXIS] * 0.01f * fabsf(fit[NAXIS]->mean);
          if (rel > tol) tol = rel;
        }
        float slim = slopeMax[NAXIS];
        if (slopePct[NAXIS] > 0) {
          float rels = slopePct[NAXIS] * 0.01f * fabsf(fit[NAXIS]->mean);
          if (rels > slim) slim = rels;
        }
        dwellOk = ((uint32_t)(ls.tMs - dataStartMs) >= win);
        inBand  = (nf >= 3) && (fit[NAXIS]->rms <= tol)
                  && (slim <= 0.0f || fabsf(fit[NAXIS]->slope) <= slim);
      } else {
        uint32_t win = (uint32_t)(outCfg.steadySec * 1000.0f);
        uint32_t winMax = (uint32_t)((dqCap[NAXIS] - 2) * EP_FEED_DT_MS);
        if (win > winMax) win = winMax;
        maxDQ[NAXIS].push(ls.tMs, ls.out, win);
        minDQ[NAXIS].push(ls.tMs, ls.out, win);
        dwellOk = ((uint32_t)(ls.tMs - dataStartMs) >= win);
        inBand  = (maxDQ[NAXIS].front() - minDQ[NAXIS].front() <= outCfg.tol);
      }
      if (dwellOk) {
        if (!inBand) { axUnqual[NAXIS]++; if (!axWasUnq[NAXIS]) axExcur[NAXIS]++; }
        axWasUnq[NAXIS] = !inBand;
      }
      axisSteady[NAXIS] = dwellOk && inBand;
      if (!axisSteady[NAXIS]) { qualified = false; nBad++; badAxis = NAXIS; badDwellOk = dwellOk; }
    } else {
      axisSteady[NAXIS] = true;   // output band disabled (vessel-perf) → never blocks
    }
    blameTicks++;
    if (nBad == 1 && badDwellOk) soleBlock[badAxis]++;   // dwell-fill misses aren't band violations — skip

    // Trailing boxcar average (the emitted value): push, then evict samples older than avgWinMs.
    avgRing[ringHead] = ls;
    ringHead = (ringHead + 1) % avgCap;
    if (ringCount < avgCap) ringCount++;
    for (int a = 0; a < NAXIS; a++) avgSumX[a] += ls.x[a];
    avgSumEx[0] += ls.ex[0]; avgSumEx[1] += ls.ex[1]; avgSumOut += ls.out;
    while (ringCount > 1) {
      int tail = (ringHead - ringCount + avgCap) % avgCap;
      if ((uint32_t)(ls.tMs - avgRing[tail].tMs) <= avgWinMs) break;
      for (int a = 0; a < NAXIS; a++) avgSumX[a] -= avgRing[tail].x[a];
      avgSumEx[0] -= avgRing[tail].ex[0]; avgSumEx[1] -= avgRing[tail].ex[1]; avgSumOut -= avgRing[tail].out;
      ringCount--;
    }
    count = qualified ? (uint32_t)ringCount : 0;            // caller reads count>0 as "steady now"

    // Emit needs a clean BOXCAR: full avgWinMs span and each axis's trimmed range within emitTrimTol, so
    // isolated blips ride in the average instead of vetoing (a continuous-steady clock starved emits).
    // emitTrimSkip exempts straightness axes. minRunMs floors the ELIGIBLE span, not continuous qualification.
    if (qualified && (uint32_t)(ls.tMs - lastEmitMs) >= EP_EMIT_PERIOD_MS && ringCount > 2
        && (minRunMs == 0 || (uint32_t)(ls.tMs - dataStartMs) >= minRunMs)) {
      int tail = (ringHead - ringCount + avgCap) % avgCap;
      bool clean = ((uint32_t)(ls.tMs - avgRing[tail].tMs) + 2u * EP_FEED_DT_MS >= avgWinMs);
      if (!clean) emitSpanShort++;
      for (int a = 0; clean && a < NAXIS; a++)
        if (!emitTrimSkip[a] && trimmedRange(a) > emitTrimTol(a, cfg[a].tol)) { clean = false; emitDirty[a]++; }
      if (clean && outCfg.tol > 0 && !emitTrimSkip[NAXIS] && trimmedRange(NAXIS) > emitTrimTol(NAXIS, outCfg.tol)) { clean = false; emitDirty[NAXIS]++; }
      if (clean) {
        lastEmitMs = ls.tMs;
        if (out) {
          for (int a = 0; a < NAXIS; a++) out->x[a] = (float)(avgSumX[a] / (double)ringCount);
          out->ex[0] = (float)(avgSumEx[0] / (double)ringCount);
          out->ex[1] = (float)(avgSumEx[1] / (double)ringCount);
          out->y = (float)(avgSumOut / (double)ringCount);
          out->nSamp = (uint32_t)ringCount;
          out->tEmit = ls.tMs;
          return true;
        }
      }
    }
    return false;
  }

  // Allowed boxcar trimmed range: the axis's tolerance plus the travel its slope cap permits over avgWinMs.
  float emitTrimTol(int axis, float baseTol) const {
    if (slopeMax[axis] <= 0.0f) return baseTol;
    return baseTol + slopeMax[axis] * ((float)avgWinMs * 0.001f);
  }

  // Trimmed max-min over the boxcar for one axis (NAXIS = output): drops the ~5% most extreme samples per tail
  // (n/20, 1..8) so isolated blips can't veto an emit. Also read unlocked (diagnostic) by altDebugCsvSend.
  float trimmedRange(int axis) const {
    int n = ringCount;
    if (n < 3) return 0.0f;
    int k = n / 20;
    if (k < 1) k = 1;
    if (k > 8) k = 8;
    float lo[9], hi[9];                       // ascending k+1 smallest / descending k+1 largest
    int m = k + 1, nlo = 0, nhi = 0;
    for (int i = 0; i < n; i++) {
      int idx = (ringHead - 1 - i + avgCap) % avgCap;
      float v = (axis < NAXIS) ? avgRing[idx].x[axis] : avgRing[idx].out;
      if (nlo < m) { int j = nlo++; while (j > 0 && lo[j - 1] > v) { lo[j] = lo[j - 1]; j--; } lo[j] = v; }
      else if (v < lo[m - 1]) { int j = m - 1; while (j > 0 && lo[j - 1] > v) { lo[j] = lo[j - 1]; j--; } lo[j] = v; }
      if (nhi < m) { int j = nhi++; while (j > 0 && hi[j - 1] < v) { hi[j] = hi[j - 1]; j--; } hi[j] = v; }
      else if (v > hi[m - 1]) { int j = m - 1; while (j > 0 && hi[j - 1] < v) { hi[j] = hi[j - 1]; j--; } hi[j] = v; }
    }
    return hi[k] - lo[k];
  }

  // Dwell seconds still needed before steady: largest per-axis shortfall since dataStartMs; 0 once every
  // window has spanned, -1 with no eligible data.
  float settleRemainSec(uint32_t nowMs) {
    if (!haveData) return -1.0f;
    float rem = 0.0f;
    for (int a = 0; a < NAXIS; a++) {
      if (gateMode[a] == EP_EXTERNAL) continue;    // its window is the caller's, and does not start at the run
      uint32_t win = (uint32_t)(cfg[a].steadySec * 1000.0f);
      uint32_t winMax = (uint32_t)(((gateMode[a] == EP_STRAIGHT ? EP_FIT_N : dqCap[a]) - 2) * EP_FEED_DT_MS);
      if (win > winMax) win = winMax;
      uint32_t el = (uint32_t)(nowMs - dataStartMs);
      if (el < win) { float r = (win - el) / 1000.0f; if (r > rem) rem = r; }
    }
    return rem;
  }
};

// Sparse support points (never a grid), so memory scales with the data; axisScale[] normalizes each axis's
// IDW distance (~ that axis's tol or span).
// Cap on LWLR solve neighbors: bounds software-double cost in dense neighborhoods (~48 pts = 1-2 ms worst).
#define FRONT_LWLR_KMAX 48
template <int NAXIS>
struct FrontStore {
  FrontPoint<NAXIS> *pts; int count, cap;
  uint8_t source;                                   // 0 = LEARNED, 1 = FIXED (loaded curve)
  float   axisScale[NAXIS];

  void init(FrontPoint<NAXIS> *buf, int c) {
    pts = buf; cap = c; count = 0; source = 0;
    for (int a = 0; a < NAXIS; a++) axisScale[a] = 1.0f;
  }
  // Max-per-cell insert (cell = within half an axisScale on EVERY axis) keeps a true upper envelope.
  // Returns true only for a new cell or a >=0.5% record beat (bare compare churned the cloud upload queue;
  // BEST_EVER_FRONT_SPEC.md §5b); callers gate cloud upload on it.
  bool add(const FrontPoint<NAXIS> &p) { return addAt(p, cellOf(p.x)); }
  // add() with its cell already found: cellOf(p.x), or a FrontQuery over this store with no write since.
  bool addAt(const FrontPoint<NAXIS> &p, int cell) {
    if (cell >= 0) {
      if (p.y > pts[cell].y * 1.005f + 0.01f) { pts[cell] = p; return true; }   // meaningfully better at this cell
      return false;                                                              // the existing record stands
    }
    if (count >= cap) return false;
    pts[count++] = p; return true;
  }
  // First record in x's half-axisScale cell, or -1: the record add() would beat (hasLocalSupport() is cellOf() >= 0).
  int cellOf(const float x[NAXIS]) const {
    for (int i = 0; i < count; i++) {
      bool sameCell = true;
      for (int a = 0; a < NAXIS; a++) {
        float sc = (axisScale[a] > 1e-9f) ? axisScale[a] : 1.0f;
        if (fabsf(x[a] - pts[i].x[a]) > 0.5f * sc) { sameCell = false; break; }
      }
      if (sameCell) return i;
    }
    return -1;
  }
  // IDW evaluation, O(count). Reached only at 1 Hz via classify()/pushesHybrid(), never from the ~75 Hz
  // altFold_tick. A convex blend, so the result never leaves the support points' y-range.
  float eval(const float x[NAXIS], float idwPower) const {
    if (count <= 0) return 0.0f;                     // bootstrap: no surface yet
    float invSc[NAXIS];
    for (int a = 0; a < NAXIS; a++) invSc[a] = (axisScale[a] > 1e-9f) ? (1.0f / axisScale[a]) : 1.0f;
    float wsum = 0, num = 0;
    for (int i = 0; i < count; i++) {
      float d2 = 0;
      for (int a = 0; a < NAXIS; a++) {
        float dx = (x[a] - pts[i].x[a]) * invSc[a];
        d2 += dx * dx;
      }
      if (d2 < 1e-12f) return pts[i].y;              // exact hit
      // dᵢ^power. Fast-path power 2 (the default) — d2 already is dᵢ²; skip sqrt+pow (1 Hz evaluator).
      float dp = (idwPower == 2.0f) ? d2 : powf(sqrtf(d2), idwPower);
      float w = 1.0f / (dp + 1e-9f);
      wsum += w; num += w * pts[i].y;
    }
    return (wsum > 0) ? (num / wsum) : 0.0f;
  }
  // IDW-bar-only keep-gate, superseded by pushesHybrid everywhere; kept as reference. Apply only when hasLocalSupport().
  bool pushes(const float x[NAXIS], float y, float safetyMargin, float idwPower) const {
    if (count <= 0) return true;
    return y > eval(x, idwPower) - safetyMargin;
  }
  // Any support point in the same half-axisScale cell? A run in an unvisited cell is admitted unconditionally.
  bool hasLocalSupport(const float x[NAXIS]) const {
    for (int i = 0; i < count; i++) {
      bool sameCell = true;
      for (int a = 0; a < NAXIS; a++) {
        float sc = (axisScale[a] > 1e-9f) ? axisScale[a] : 1.0f;
        if (fabsf(x[a] - pts[i].x[a]) > 0.5f * sc) { sameCell = false; break; }
      }
      if (sameCell) return true;
    }
    return false;
  }
  // Normalized distance to the nearest support point; beyond the caller's radius live % and trend report no
  // reference.
  float nearestNormDist(const float x[NAXIS]) const {
    if (count <= 0) return 1e9f;
    float invSc[NAXIS];
    for (int a = 0; a < NAXIS; a++) invSc[a] = (axisScale[a] > 1e-9f) ? (1.0f / axisScale[a]) : 1.0f;
    float best = 1e30f;
    for (int i = 0; i < count; i++) {
      float d2 = 0;
      for (int a = 0; a < NAXIS; a++) {
        float dx = (x[a] - pts[i].x[a]) * invSc[a];
        d2 += dx * dx;
      }
      if (d2 < best) best = d2;
    }
    return sqrtf(best);
  }
  // LWLR display/trend evaluator: local linear fit with eval's weights; prediction b0 can sit below the
  // lowest neighbor (removes IDW edge bias). Nearest <= FRONT_LWLR_KMAX points only (a hard radius cut was
  // REJECTED). Rationale and validation: ALT_HEALTH_LWLR_ENGINE_SPEC.md.
  bool evalLWLR(const float x[NAXIS], float ridgeFrac, float *predOut, float *slopesOut) const {
    if (count <= 0) return false;
    float invSc[NAXIS];
    for (int a = 0; a < NAXIS; a++) invSc[a] = (axisScale[a] > 1e-9f) ? (1.0f / axisScale[a]) : 1.0f;
    // selection pass: nearest ≤ KMAX neighbors (bounded insertion, ascending d²) + whole-front max y
    int   selIdx[FRONT_LWLR_KMAX];
    float selD2[FRONT_LWLR_KMAX];
    int   nSel = 0;
    float maxY = 0;
    for (int i = 0; i < count; i++) {
      float d2 = 0;
      for (int a = 0; a < NAXIS; a++) {
        float dx = (x[a] - pts[i].x[a]) * invSc[a];
        d2 += dx * dx;
      }
      if (pts[i].y > maxY) maxY = pts[i].y;
      if (nSel < FRONT_LWLR_KMAX) {
        int j = nSel++;
        while (j > 0 && selD2[j - 1] > d2) { selD2[j] = selD2[j - 1]; selIdx[j] = selIdx[j - 1]; j--; }
        selD2[j] = d2; selIdx[j] = i;
      } else if (d2 < selD2[FRONT_LWLR_KMAX - 1]) {
        int j = FRONT_LWLR_KMAX - 1;
        while (j > 0 && selD2[j - 1] > d2) { selD2[j] = selD2[j - 1]; selIdx[j] = selIdx[j - 1]; j--; }
        selD2[j] = d2; selIdx[j] = i;
      }
    }
    const int M = NAXIS + 1;                      // unknowns: [b0, slope_0..slope_{NAXIS-1}]
    double A[NAXIS + 1][NAXIS + 1] = {};
    double b[NAXIS + 1] = {};
    for (int k = 0; k < nSel; k++) {
      const FrontPoint<NAXIS> &pt = pts[selIdx[k]];
      double phi[NAXIS + 1];
      phi[0] = 1.0;
      double d2 = 0;
      for (int a = 0; a < NAXIS; a++) {
        double dx = (double)(pt.x[a] - x[a]) * invSc[a];
        phi[a + 1] = dx;
        d2 += dx * dx;
      }
      double w = 1.0 / (d2 + 1e-9);
      for (int r = 0; r < M; r++) {
        for (int c = r; c < M; c++) A[r][c] += w * phi[r] * phi[c];
        b[r] += w * phi[r] * (double)pt.y;
      }
    }
    for (int r = 1; r < M; r++)
      for (int c = 0; c < r; c++) A[r][c] = A[c][r];          // mirror the symmetric upper triangle
    double tr = 0;
    for (int a = 1; a < M; a++) tr += A[a][a];
    double ridge = (double)ridgeFrac * tr / (double)NAXIS;    // slope block only — never b0
    for (int a = 1; a < M; a++) A[a][a] += ridge;
    for (int col = 0; col < M; col++) {                       // forward elimination, partial pivot
      int p = col;
      for (int r = col + 1; r < M; r++)
        if (fabs(A[r][col]) > fabs(A[p][col])) p = r;
      if (fabs(A[p][col]) < 1e-12) return false;              // degenerate geometry (e.g. 1 point)
      if (p != col) {
        for (int c = 0; c < M; c++) { double t = A[col][c]; A[col][c] = A[p][c]; A[p][c] = t; }
        double t = b[col]; b[col] = b[p]; b[p] = t;
      }
      for (int r = col + 1; r < M; r++) {
        double f = A[r][col] / A[col][col];
        for (int c = col; c < M; c++) A[r][c] -= f * A[col][c];
        b[r] -= f * b[col];
      }
    }
    double sol[NAXIS + 1];
    for (int r = M - 1; r >= 0; r--) {                        // back substitution
      double s = b[r];
      for (int c = r + 1; c < M; c++) s -= A[r][c] * sol[c];
      sol[r] = s / A[r][r];
    }
    float pred = (float)sol[0];
    float hi = 1.25f * maxY;                                  // sanity ceiling only; a neighbor-range clamp reintroduces edge bias
    if (pred < 0.1f) pred = 0.1f;
    if (pred > hi) pred = hi;
    if (predOut) *predOut = pred;
    if (slopesOut)
      for (int a = 0; a < NAXIS; a++) slopesOut[a] = (float)sol[a + 1];
    return true;
  }
  // Slope-gap risk numerator: per axis, if every in-radius neighbor lies on ONE side of the query
  // (beyond ±deadBand), contribution = |slope_a| × gap to the nearest populated side. OUTPUT-BLIND.
  // Returns Σ in output units (caller divides by the prediction).
  float slopeGapAmps(const float x[NAXIS], float radius, float deadBand, const float slopes[NAXIS]) const {
    float invSc[NAXIS];
    for (int a = 0; a < NAXIS; a++) invSc[a] = (axisScale[a] > 1e-9f) ? (1.0f / axisScale[a]) : 1.0f;
    float minPos[NAXIS], minNeg[NAXIS];
    bool hasPos[NAXIS], hasNeg[NAXIS], hasMid[NAXIS];
    for (int a = 0; a < NAXIS; a++) { minPos[a] = minNeg[a] = 1e30f; hasPos[a] = hasNeg[a] = hasMid[a] = false; }
    float r2 = radius * radius;
    for (int i = 0; i < count; i++) {
      float dx[NAXIS], d2 = 0;
      for (int a = 0; a < NAXIS; a++) { dx[a] = (pts[i].x[a] - x[a]) * invSc[a]; d2 += dx[a] * dx[a]; }
      if (d2 > r2) continue;                                  // risk is judged over in-radius neighbors only
      for (int a = 0; a < NAXIS; a++) {
        if (dx[a] > deadBand)       { hasPos[a] = true; if (dx[a]  < minPos[a]) minPos[a] = dx[a]; }
        else if (dx[a] < -deadBand) { hasNeg[a] = true; if (-dx[a] < minNeg[a]) minNeg[a] = -dx[a]; }
        else hasMid[a] = true;                                // a neighbor sits AT the query on this axis
      }
    }
    float sum = 0;
    for (int a = 0; a < NAXIS; a++) {
      if (hasMid[a] || (hasPos[a] && hasNeg[a])) continue;    // two-sided (or on-point) support → no risk
      float gap = hasPos[a] ? minPos[a] : (hasNeg[a] ? minNeg[a] : 0.0f);
      sum += fabsf(slopes[a]) * gap;
    }
    return sum;
  }
  // Confidence state + display prediction (1 Hz). Order: MEASURED -> NO_REFERENCE -> risk gate ->
  // LEARNING_EDGE or ESTIMATED. OUTPUT-BLIND: measured output never enters the state decision, so
  // degradation can't relabel itself "learning". barOut = min(IDW, LWLR). See ALT_HEALTH_LWLR_ENGINE_SPEC.md.
  int classify(const float x[NAXIS], float refRadius, float idwPower, float ridgeFrac,
               float riskThresh, float *predOut, float *barOut = nullptr) const {
    if (predOut) *predOut = 0;
    if (barOut) *barOut = 0;
    if (count <= 0) return 3;                                 // FRONT_NO_REFERENCE
    float idw = eval(x, idwPower);                            // float whole-front pass — cheap (FPU)
    float lp = 0, sl[NAXIS];
    bool lwlrOk = evalLWLR(x, ridgeFrac, &lp, sl);
    if (barOut) *barOut = (lwlrOk && lp < idw) ? lp : idw;    // conservative hybrid admission bar
    if (hasLocalSupport(x)) {                                 // 1. MEASURED — show %
      if (predOut) *predOut = lwlrOk ? lp : idw;              // IDW fallback ≈ the cell record itself
      return 0;
    }
    if (nearestNormDist(x) > refRadius) return 3;             // 2. NO_REFERENCE — "no reference here yet"
    if (!lwlrOk || lp <= 0.1f) return 2;                      // degenerate solve → LEARNING_EDGE
    float slopeGapRisk = slopeGapAmps(x, refRadius, 0.05f, sl) / lp;
    float curvRisk = fabsf(lp - idw) / lp;                    // IDW≠LWLR = local curvature the fit can't see
    float risk = (slopeGapRisk > curvRisk) ? slopeGapRisk : curvRisk;
    if (risk > riskThresh) return 2;                          // 3. LEARNING_EDGE — no number shown
    if (predOut) *predOut = lp;
    return 1;                                                 // 4. ESTIMATED — show %
  }
  // Cell-local admission bar for cells WITH local support: beat min(IDW, LWLR) − margin. Callers
  // admit unconditionally when hasLocalSupport() is false (unvisited cell opens at its true value).
  bool pushesHybrid(const float x[NAXIS], float y, float safetyMargin, float idwPower, float ridgeFrac) const {
    if (count <= 0) return true;
    float bar = eval(x, idwPower);
    float lp;
    if (evalLWLR(x, ridgeFrac, &lp, nullptr) && lp < bar) bar = lp;
    return y > bar - safetyMargin;
  }
};

// Front writes outside the lookup engine (reset, import, cloud sync, Uploaded load, class change, demo swap, fillmax)
// bracket themselves with frontWriteBegin/End; a FrontQuery that sees either counter move drops its result. The
// writer count covers async_tcp loading the Uploaded surface while Core 1 is mid-lookup.
static volatile uint32_t frontGen = 0;
static volatile int32_t  frontWriters = 0;
static inline void frontWriteBegin() {
  __atomic_add_fetch(&frontWriters, 1, __ATOMIC_SEQ_CST);
  __atomic_add_fetch(&frontGen, 1, __ATOMIC_SEQ_CST);
}
static inline void frontWriteEnd() {
  __atomic_add_fetch(&frontGen, 1, __ATOMIC_SEQ_CST);
  __atomic_sub_fetch(&frontWriters, 1, __ATOMIC_SEQ_CST);
}
static inline uint32_t frontGenNow() { return __atomic_load_n(&frontGen, __ATOMIC_SEQ_CST); }
static inline bool frontGenSame(uint32_t g) {
  return __atomic_load_n(&frontWriters, __ATOMIC_SEQ_CST) == 0 && __atomic_load_n(&frontGen, __ATOMIC_SEQ_CST) == g;
}

#define FRONT_STEP_US 500   // lookup work per loop() pass for each engine (alt-health job, boat-perf queue)

// One record-book lookup as ONE pass over the store, resumable across loop() passes so a pass pays at most
// FRONT_STEP_US at any count. The pass gathers what classify(), cellOf() and nearestNormDist() otherwise read the
// store five to seven times for, keeping each one's comparisons and summation order, so every result is bit-identical
// to theirs (/debug/frontcheck compares them on the board). pts/count/axisScale are taken at begin(); a front write
// since then (frontGen) ends the lookup DROPPED.
template <int NAXIS>
struct FrontQuery {
  enum : uint8_t { IDLE = 0, SCAN = 1, SOLVE = 2, DONE = 3, DROPPED = 4 };
  uint8_t phase;
  const FrontPoint<NAXIS> *pts;
  int   n, i, k;
  uint32_t gen;
  float x[NAXIS], invSc[NAXIS], half[NAXIS];
  float idwPower, ridgeFrac, refRadius, riskThresh, r2;
  float wsum, num, maxY, best;                         // eval() sums, evalLWLR() max y, nearest d²
  int   exact, nSel;                                   // eval() exact hit, evalLWLR() neighbor count
  int   selIdx[FRONT_LWLR_KMAX];
  float selD2[FRONT_LWLR_KMAX];
  float minPos[NAXIS], minNeg[NAXIS];                  // slopeGapAmps() per-axis side state
  bool  hasPos[NAXIS], hasNeg[NAXIS], hasMid[NAXIS];
  double A[NAXIS + 1][NAXIS + 1], b[NAXIS + 1];
  float sl[NAXIS];
  // Results, valid at DONE
  int   state;                  // classify()
  float pred, bar;              // classify() predOut / barOut (bar = pushesHybrid()'s bar)
  float idw, lp;                // eval(); evalLWLR() prediction (0 unless lwlrOk)
  bool  lwlrOk;
  int   cell;                   // cellOf()
  float nearest;                // nearestNormDist()

  void begin(const FrontStore<NAXIS> &fs, const float q[NAXIS], float idwP, float ridge, float radius, float risk,
             uint32_t g) {
    pts = fs.pts; n = fs.count; i = 0; k = 0; gen = g;
    for (int a = 0; a < NAXIS; a++) {
      x[a] = q[a];
      invSc[a] = (fs.axisScale[a] > 1e-9f) ? (1.0f / fs.axisScale[a]) : 1.0f;
      float sc = (fs.axisScale[a] > 1e-9f) ? fs.axisScale[a] : 1.0f;
      half[a] = 0.5f * sc;
      minPos[a] = minNeg[a] = 1e30f; hasPos[a] = hasNeg[a] = hasMid[a] = false;
    }
    idwPower = idwP; ridgeFrac = ridge; refRadius = radius; riskThresh = risk; r2 = radius * radius;
    wsum = 0; num = 0; maxY = 0; best = 1e30f; exact = -1; nSel = 0; cell = -1;
    state = FRONT_NO_REFERENCE; pred = 0; bar = 0; idw = 0; lp = 0; lwlrOk = false; nearest = 1e9f;
    phase = SCAN;
  }

  // Runs until the lookup ends (true: DONE or DROPPED) or untilUs passes (false: call again next pass).
  bool step(int64_t untilUs) {
    if (phase != SCAN && phase != SOLVE) return true;
    if (!frontGenSame(gen)) { phase = DROPPED; return true; }
    if (phase == SCAN) {
      float ws = wsum, nm = num, my = maxY, bs = best;
      int ex = exact, cl = cell;
      while (i < n) {
        int end = (n - i > 32) ? i + 32 : n;
        for (; i < end; i++) {
          const FrontPoint<NAXIS> &p = pts[i];
          float d2 = 0;
          bool inCell = true;
          for (int a = 0; a < NAXIS; a++) {
            float diff = x[a] - p.x[a];
            float dx = diff * invSc[a];
            d2 += dx * dx;
            if (fabsf(diff) > half[a]) inCell = false;
          }
          if (ex < 0) {                                                // eval()
            if (d2 < 1e-12f) ex = i;
            else {
              float dp = (idwPower == 2.0f) ? d2 : powf(sqrtf(d2), idwPower);
              float w = 1.0f / (dp + 1e-9f);
              ws += w; nm += w * p.y;
            }
          }
          if (p.y > my) my = p.y;                                      // evalLWLR() selection
          if (nSel < FRONT_LWLR_KMAX) {
            int j = nSel++;
            while (j > 0 && selD2[j - 1] > d2) { selD2[j] = selD2[j - 1]; selIdx[j] = selIdx[j - 1]; j--; }
            selD2[j] = d2; selIdx[j] = i;
          } else if (d2 < selD2[FRONT_LWLR_KMAX - 1]) {
            int j = FRONT_LWLR_KMAX - 1;
            while (j > 0 && selD2[j - 1] > d2) { selD2[j] = selD2[j - 1]; selIdx[j] = selIdx[j - 1]; j--; }
            selD2[j] = d2; selIdx[j] = i;
          }
          if (d2 < bs) bs = d2;                                        // nearestNormDist()
          if (cl < 0 && inCell) cl = i;                                // cellOf()
          if (!(d2 > r2)) {                                            // slopeGapAmps(x, refRadius, 0.05f)
            for (int a = 0; a < NAXIS; a++) {
              float g = (p.x[a] - x[a]) * invSc[a];
              if (g > 0.05f)       { hasPos[a] = true; if (g  < minPos[a]) minPos[a] = g; }
              else if (g < -0.05f) { hasNeg[a] = true; if (-g < minNeg[a]) minNeg[a] = -g; }
              else hasMid[a] = true;
            }
          }
        }
        if (i < n && esp_timer_get_time() >= untilUs) break;
      }
      wsum = ws; num = nm; maxY = my; best = bs; exact = ex; cell = cl;
      if (i < n) return false;
      if (n <= 0) { finish(); return true; }
      idw = (exact >= 0) ? pts[exact].y : ((wsum > 0) ? (num / wsum) : 0.0f);
      nearest = sqrtf(best);
      for (int r = 0; r <= NAXIS; r++) { b[r] = 0; for (int c = 0; c <= NAXIS; c++) A[r][c] = 0; }
      phase = SOLVE;
      if (esp_timer_get_time() >= untilUs) return false;
    }
    while (k < nSel) {                                                 // evalLWLR() normal equations
      const FrontPoint<NAXIS> &pt = pts[selIdx[k]];
      const int M = NAXIS + 1;
      double phi[NAXIS + 1];
      phi[0] = 1.0;
      double d2 = 0;
      for (int a = 0; a < NAXIS; a++) {
        double dx = (double)(pt.x[a] - x[a]) * invSc[a];
        phi[a + 1] = dx;
        d2 += dx * dx;
      }
      double w = 1.0 / (d2 + 1e-9);
      for (int r = 0; r < M; r++) {
        for (int c = r; c < M; c++) A[r][c] += w * phi[r] * phi[c];
        b[r] += w * phi[r] * (double)pt.y;
      }
      k++;
      if ((k & 3) == 0 && k < nSel && esp_timer_get_time() >= untilUs) return false;
    }
    finish();
    if (!frontGenSame(gen)) phase = DROPPED;                           // an async_tcp write landed mid-lookup
    return true;
  }

  // evalLWLR()'s solve, then classify()'s decision, as written there.
  void finish() {
    if (n > 0) {
      const int M = NAXIS + 1;
      for (int r = 1; r < M; r++)
        for (int c = 0; c < r; c++) A[r][c] = A[c][r];
      double tr = 0;
      for (int a = 1; a < M; a++) tr += A[a][a];
      double ridge = (double)ridgeFrac * tr / (double)NAXIS;
      for (int a = 1; a < M; a++) A[a][a] += ridge;
      bool ok = true;
      for (int col = 0; ok && col < M; col++) {
        int p = col;
        for (int r = col + 1; r < M; r++)
          if (fabs(A[r][col]) > fabs(A[p][col])) p = r;
        if (fabs(A[p][col]) < 1e-12) { ok = false; break; }
        if (p != col) {
          for (int c = 0; c < M; c++) { double t = A[col][c]; A[col][c] = A[p][c]; A[p][c] = t; }
          double t = b[col]; b[col] = b[p]; b[p] = t;
        }
        for (int r = col + 1; r < M; r++) {
          double f = A[r][col] / A[col][col];
          for (int c = col; c < M; c++) A[r][c] -= f * A[col][c];
          b[r] -= f * b[col];
        }
      }
      if (ok) {
        double sol[NAXIS + 1];
        for (int r = M - 1; r >= 0; r--) {
          double s = b[r];
          for (int c = r + 1; c < M; c++) s -= A[r][c] * sol[c];
          sol[r] = s / A[r][r];
        }
        float pr = (float)sol[0];
        float hi = 1.25f * maxY;
        if (pr < 0.1f) pr = 0.1f;
        if (pr > hi) pr = hi;
        lp = pr;
        for (int a = 0; a < NAXIS; a++) sl[a] = (float)sol[a + 1];
        lwlrOk = true;
      }
    }
    pred = 0; bar = 0;
    if (n <= 0) state = FRONT_NO_REFERENCE;
    else {
      bar = (lwlrOk && lp < idw) ? lp : idw;
      if (cell >= 0) { pred = lwlrOk ? lp : idw; state = FRONT_MEASURED; }
      else if (nearest > refRadius) state = FRONT_NO_REFERENCE;
      else if (!lwlrOk || lp <= 0.1f) state = FRONT_LEARNING_EDGE;
      else {
        float sum = 0;
        for (int a = 0; a < NAXIS; a++) {
          if (hasMid[a] || (hasPos[a] && hasNeg[a])) continue;
          float gap = hasPos[a] ? minPos[a] : (hasNeg[a] ? minNeg[a] : 0.0f);
          sum += fabsf(sl[a]) * gap;
        }
        float slopeGapRisk = sum / lp;
        float curvRisk = fabsf(lp - idw) / lp;
        float risk = (slopeGapRisk > curvRisk) ? slopeGapRisk : curvRisk;
        if (risk > riskThresh) state = FRONT_LEARNING_EDGE;
        else { pred = lp; state = FRONT_ESTIMATED; }
      }
    }
    phase = DONE;
  }

  // Bench probe near a stored record (/debug/frontcheck): record (seq·7919 mod count), moved off * axisScale per axis.
  static void probe(const FrontStore<NAXIS> &fs, uint32_t seq, float off, float out[NAXIS]) {
    if (fs.count <= 0) { for (int a = 0; a < NAXIS; a++) out[a] = 0; return; }
    const FrontPoint<NAXIS> &p = fs.pts[(seq * 7919u) % (uint32_t)fs.count];
    for (int a = 0; a < NAXIS; a++) out[a] = p.x[a] + off * ((fs.axisScale[a] > 1e-9f) ? fs.axisScale[a] : 1.0f);
  }
};

// ALTERNATOR HEALTH, Best-Ever Front (device side). Folds one sample per control tick (~75 Hz, pidLog hook)
// into Episode; emitted steady-run averages push the output-amps front; the cloud prunes and keeps raw history.
// Generic engine: top of this file; globals: Xregulator.ino. Spec: BEST_EVER_FRONT_SPEC.md / BEST_EVER_IMPLEMENTATION_PLAN.md.

#define ALT_VER          4u
#define ALT_FRONT_MAGIC  0x414C4652u  // 'ALFR' — best-ever front point blob
#define ALT_TREND_MAGIC  0x414C5452u  // 'ALTR' (legacy whole-blob trend; superseded by the append log)
#define ALT_TRENDLOG_MAGIC 0x414C544Cu // 'ALTL' — append-only engine-hour trend log
#define ALT_PEND_MAGIC   0x414C5044u  // 'ALPD' — unsynced cloud-pending points

// Temp-normalized field drive (excitation proxy) in 12 V-winding equivalent volts so windings compare on one
// axis; fieldWindApplied is 12 while the resistance check holds 12 V winding values.
static inline float altExcitation(float duty, float vbus, float tF) {
  float tc = (tF - 32.0f) / 1.8f;
  float denom = 1.0f + ALT_ALPHA_PER_C * (tc - ALT_TREF_C);
  if (denom < 0.5f) denom = 0.5f;
  return (duty / 100.0f) * vbus / denom * (12.0f / (float)fieldWindApplied);
}

// ---- Best-Ever Front engine instance (alternator 4-D): axes {RPM, field-duty %, Vbus, tempF}. Generic engine: top of this file ----
#define ALT_NAXIS        4
#define ALT_EMIT_AVG_MS  2000     // record/grade averaging window; 8 s starved harvest (11 of 53 steady runs) for ~0.1% less inflation
#define ALT_FRONT_CAP    4096     // sparse support points (PSRAM); sized to be unreachable even AP-mode/no-prune — cost scales with count, not cap (see ALT_HEALTH_LWLR_ENGINE_SPEC.md)
#define ALT_EP_RING_CAP  1024     // Episode boxcar buffer (PSRAM); only ~80 samples live at 10 Hz, generously sized
#define ALT_PENDING_CAP  4096     // = front cap: holds every unsynced point through weeks offline (PSRAM)

static Episode<ALT_NAXIS>     altEpisode;
// Straightness + lead-pairing state: INTERNAL RAM on purpose (~1.7 KB), read and written in the 10 Hz
// control tick where a PSRAM sweep would cost more than the arithmetic.
static LineFit  altFitRpm, altFitDuty, altFitAmps;
static LeadLine altLeadVbus, altLeadAmps;
static FrontStore<ALT_NAXIS>  altFront2;                // "My History" — the LEARNED surface (always the learn target)
static FrontStore<ALT_NAXIS>  altFrontUp;               // "Uploaded File" — a borrowed surface, resident alongside My History
static RawSample<ALT_NAXIS>  *altEpRing   = nullptr;
static FrontPoint<ALT_NAXIS> *altFrontBuf = nullptr;
static FrontPoint<ALT_NAXIS> *altFrontUpBuf = nullptr;  // backing store for the Uploaded surface
static bool altHaveUpload     = false;                  // true once a file has been uploaded (Uploaded surface populated)
static FrontPoint<ALT_NAXIS> *altPending  = nullptr;   // accepted-since-last-upload (raw points out)
static int altPendingCount   = 0;
static int altSentCount      = 0;          // pending prefix the last built upload carried (Core 1 only)
static volatile bool altSentSeeded = false;   // that upload was the seeded batch, import or reregister (read by Core 0 on success)

// Front-sync result from httpsTask (Core 0) to the learning tick (Core 1), which alone writes the pending queues
// and fronts (else in-flight points drop and FrontStore::add races). body = pruned BEFRONT1 CSV when ingest: the
// upload's own PSRAM block, which the reply was read into, owned by the hand-off from make() on (no second copy).
// make() is a member because Arduino's auto-prototype generator breaks on sketch types in free-function signatures.
struct FrontSyncHandoff {
  bool ingest;
  char *body;
  static FrontSyncHandoff *make(bool ingest, char *body) {
    if (!ingest) { free(body); body = nullptr; }
    FrontSyncHandoff *h = (FrontSyncHandoff *)ps_malloc(sizeof(FrontSyncHandoff));
    if (!h) { free(body); return nullptr; }
    h->ingest = ingest && body;
    h->body = body;
    return h;
  }
  static void drop(FrontSyncHandoff *h) {
    if (!h) return;
    free(h->body);
    free(h);
  }
};
static FrontSyncHandoff *altSyncHandoff = nullptr, *perfSyncHandoff = nullptr;
// Either reply still waiting for Core 1 holds its block; the uploads take turns on that too (see the producers in loop()).
bool frontSyncHandoffPending() {
  return __atomic_load_n(&altSyncHandoff, __ATOMIC_ACQUIRE) || __atomic_load_n(&perfSyncHandoff, __ATOMIC_ACQUIRE);
}

// ---- session-plot series (PSRAM ring, RAM-only) ----------------------------------------------
// Device-side copy of the dashboard's 5 s series, served at /altsess.csv so a fresh or reconnecting page
// recovers the whole run. Never persisted: a session ends at field-off, and writing it puts flash in the control path.
#define ALT_SESS_DT_MS    5000u   // MUST match the dashboard's dot cadence (altSessionPush in script.js)
#define ALT_SESS_RING_N   1440    // 2 h at ALT_SESS_DT_MS, ~30 KB PSRAM; only covers "how long was I away"
                                  // (an open page keeps ~40 h). Never decimate: the plot's information IS dot density.
struct __attribute__((packed)) AltSessPt {   // 21 B — packed + fixed-point; 2 h = 30 KB, not 200 KB
  uint32_t tMs;                   // device millis() at capture — client maps to wall clock via the header's nowMs
  uint16_t gAmps;                 // graded (boxcar) output A ×100 — the % numerator
  uint16_t pred;                  // expected A ×100 — pct is gAmps/pred, so it is NOT stored
  uint16_t measA;                 // fast live output A ×100
  uint16_t rpm;
  uint16_t duty;                  // field % ×100
  uint16_t vbus;                  // V ×100
  int16_t  tF;                    // alternator case °F ×10
  int16_t  tSlope;                // case-temp slope °F/min ×100 — full resolution: near-zero vs a few
                                  // °F/min is the settled-vs-warm-up discriminator, so it can't be coarsened
  uint8_t  flags;                 // bits 0-1 state (0 measured, 1 estimated, 2 ungraded), bit 2 full steady run
};
static_assert(sizeof(AltSessPt) == 21, "AltSessPt lost its packing — the PSRAM budget assumes 21 B");
static AltSessPt *altSessRing      = nullptr;
static uint32_t   altSessRingHead  = 0, altSessRingCount = 0, altSessRingLastMs = 0;
static AltSessPt  altSessLastGraded = {};   // demo only: the row repeated while nothing is graded (9_demo.ino seeds it)
static inline uint16_t altQU16(float v, float scale) {          // saturating, NaN→0
  if (!(v > 0.0f)) return 0;
  float q = v * scale;
  return (q >= 65535.0f) ? 65535u : (uint16_t)(q + 0.5f);
}
static inline int16_t altQI16(float v, float scale) {
  if (isnan(v)) return 0;
  float q = v * scale;
  if (q >  32767.0f) return  32767;
  if (q < -32768.0f) return -32768;
  return (int16_t)(q + (q >= 0.0f ? 0.5f : -0.5f));
}

// Active grading surface = the one altRefSource selects. Learning ALWAYS writes altFront2 (My History);
// grading (session % + trend) reads whichever this returns. Uploaded falls back to My History if empty.
static inline FrontStore<ALT_NAXIS> &altGradeFront() {
  return (altRefSource == 1 && altHaveUpload) ? altFrontUp : altFront2;
}

// ---- trailing case-temperature gates (full dwell + the lighter half dwell for the Session plot) ----
// Same deque sliding-window min/max as Episode, fed the same decimated tF; PSRAM-backed.
// NOT reset by the detector's eligibility barrier: it is a physical thermal-equilibrium test, and clearing it
// on every output dip was the largest cause of a starved record book. Only a feed gap clears it.
struct AltTempGate {
  MonoDeque maxDQ, minDQ;
  uint32_t  startMs, lastFeedMs; bool have, steady; int cap;
  void init(int c) {
    cap = c;
    uint32_t *tA = (uint32_t *)ps_malloc((size_t)cap * sizeof(uint32_t)); float *vA = (float *)ps_malloc((size_t)cap * sizeof(float));
    uint32_t *tB = (uint32_t *)ps_malloc((size_t)cap * sizeof(uint32_t)); float *vB = (float *)ps_malloc((size_t)cap * sizeof(float));
    if (!tA || !vA || !tB || !vB) { steady = false; have = false; return; }
    maxDQ.bind(tA, vA, cap, true); minDQ.bind(tB, vB, cap, false);
    clear();
  }
  void clear() { maxDQ.clear(); minDQ.clear(); startMs = 0; lastFeedMs = 0; have = false; steady = false; }
  void feed(float tF, uint32_t nowMs, float tol, float secs) {
    if (!maxDQ.ts || !minDQ.ts) return;   // init's ps_malloc failed — stay un-steady rather than deref null
    if (have && (uint32_t)(nowMs - lastFeedMs) < EP_FEED_DT_MS) return;   // decimate to match the fold's Episode feed
    if (have && (uint32_t)(nowMs - lastFeedMs) > 5u * EP_FEED_DT_MS) clear();  // feed gap = hard barrier, same as Episode::feed
    lastFeedMs = nowMs;
    if (!have) { startMs = nowMs; have = true; }
    uint32_t win = (uint32_t)(secs * 1000.0f);
    uint32_t winMax = (uint32_t)((cap - 2) * EP_FEED_DT_MS);
    if (win > winMax) win = winMax;
    maxDQ.push(nowMs, tF, win); minDQ.push(nowMs, tF, win);
    steady = ((uint32_t)(nowMs - startMs) >= win) && (maxDQ.front() - minDQ.front() <= tol);
  }
};
static AltTempGate altThermGate;      // FULL dwell (altThermSec) — the detector's temperature axis
static AltTempGate altSessTempGate;   // HALF dwell — the Session plot's lighter tier
static String altPendingSeededFrom = "";   // non-empty → this pending batch is a seeded curve: "import" or "reregister" (provenance tag)
static int altFrontEmitCount = 0;        // episode points emitted (whether or not they pushed the front)

// Per-axis steady-time knobs + front/eval knobs (registry-wired below; per-axis tol + floors are in
// Xregulator.ino). altPruneK is echoed to the cloud but applied cloud-side.
float altRpmSec       = 3.0f;    // RPM steady time (s)
float altDutySec      = 3.0f;    // field-duty % steady time (s)
float altVbusSec      = 3.0f;    // bus-voltage steady time (s)
float altThermDegF    = 4.0f;    // temperature deviation bound (°F) — record only at thermal equilibrium. 0.105 A/°F at cruise; also gates the altThermSec dwell, so it is the biggest yield knob
float altThermSec     = 20.0f;   // STEADY_TEMP_SEC — FULL-steady temp dwell (s). Feeds the surface + trend + orange ring.
                                 // Longer dwell costs records without reducing smear; at 20 s a 5 °F/min warm-up spans 1.7 °F.
                                 // A warming machine is still protected by tempF being a book axis and tSlope on every record.
static inline float altSessTempDwell() { return altThermSec * 0.5f; }
// ---- engine-hours trend knobs (spec §5) ----
float altTrendBucketSec = 3600.0f;  // TREND_BUCKET_SEC — engine-seconds per trend bucket (production 3600 = 1 h; testing 600)
float altTrendFeedSec   = 10.0f;    // TREND_FEED_SEC — min spacing between graded samples entering a bucket (intake throttle)
float altTrendMinSamp   = 2.0f;     // MIN_SAMPLES — a bucket needs ≥ this many graded steady-run samples before it commits
// Output straightness (5th criterion: the measured amps must themselves lie on a straight line —
// directly guards what gets recorded, letting the input limits stay tight) + signal conditioning.
// The output may slope (up to the slope cap below): while the machine ramps smoothly up a straight speed line, its
// output rides a straight line of its own, and that pair is exactly the measurement worth banking.
float altAmpsLinePct  = 0.5f;    // output-amps rms departure from its own line, % of the window mean
float altAmpsLineFloorA = 0.4f;  // …with this absolute floor (A), which governs below ~80 A; registry name altAmpsLineFlrA (15-char NVS key cap)
// Output SLOPE cap: smear from a moving operating point is (output ramp rate) x (residual lead error), so
// bounding the ramp bounds smear at its source. Relative with a floor: an absolute A/s cap sized at a 20 A
// float would strangle a 90 A bulk.
float altAmpsSlewPct  = 1.0f;    // output-line slope cap, % of the window mean per second
float altAmpsSlewFloorA = 0.15f; // …with this absolute floor (A/s), which governs below 15 A; registry name altAmpsSlewFlrA (15-char NVS key cap)
float altAmpsSec      = 3.0f;    // output-amps straightness window (s)
float altEmaSec       = 0.5f;    // EMA time constant (s) on detector inputs RPM/duty/Vbus/amps (0 = off)
float altLeadSec      = 0.40f;   // output lead: amps and bus volts delayed this long relative to RPM, field and temp before
                                 // window stats (0.4 s aligns accel/decel passes to 0.1-0.6 A). Quantized to EP_FEED_DT_MS ticks, 0..10
float altMinRunSec    = 2.0f;    // minimum steady-run length to emit a point (s)
float altRefRadius    = 2.0f;    // normalized nearest-support distance beyond which live % + trend report no reference
float altSafetyMargin = 0.0f;    // amps; 0 = keep only runs that strictly beat the front (the cloud only prunes, so sub-front samples pollute the local surface)
float altIdwPower     = 2.0f;    // IDW power for the admission-bar/curvature IDW (display prediction is LWLR)
float altPruneK       = 6.0f;    // cloud prune neighbor count (echoed; applied cloud-side)
float altRidgeFrac    = 0.10f;   // LWLR ridge fraction (× slope-block trace/NAXIS) — fit stability vs slope fidelity
float altRiskThresh   = 0.15f;   // classifier risk above which an in-radius point shows "learning" instead of a %
// High-field-low-output alert thresholds (independent safety net — see altHealth_tick):
float altHiFieldPct   = 80.0f;   // high-drive threshold as % of the alternator's live field ceiling (altFieldCeiling), so it works on 24/36/48V compressed spans
float altLowOutAmps   = 10.0f;   // output at/below this counts as low (A)
float altHiFieldSec   = 30.0f;   // both conditions must persist this long before the alert fires (s)

// GUI-adjustable settings registry (defined here, before altDebugCsvSend uses it; handlers/echo live
// further down). One float registry → one /get handler loop + boot-load loop + "AltSettings" echo.
struct AltSetting { const char *name; float *ptr; };
static AltSetting ALT_SETTINGS[] = {
  {"altRpmLineRms", &altRpmLineRms}, {"altRpmRateMax", &altRpmRateMax}, {"altRpmSec", &altRpmSec},
  {"altDutyLineRms", &altDutyLineRms}, {"altDutySlewMax", &altDutySlewMax}, {"altDutySec", &altDutySec},
  {"altVbusTol", &altVbusTol}, {"altVbusSec", &altVbusSec},
  {"altThermDegF", &altThermDegF}, {"altThermSec", &altThermSec},
  {"altTrendBuckSec", &altTrendBucketSec},                             // TREND_BUCKET_SEC (name ≤15 chars; var is altTrendBucketSec)
  {"altTrendFeedSec", &altTrendFeedSec},                               // TREND_FEED_SEC (trend intake throttle)
  {"altTrendMinSamp", &altTrendMinSamp},                               // MIN_SAMPLES (bucket commit gate)
  {"altAmpsLinePct", &altAmpsLinePct},
  {"altAmpsLineFlrA", &altAmpsLineFloorA},                             // 15-char NVS cap (var is altAmpsLineFloorA)
  {"altAmpsSlewPct", &altAmpsSlewPct},
  {"altAmpsSlewFlrA", &altAmpsSlewFloorA},                             // 15-char NVS cap (var is altAmpsSlewFloorA)
  {"altAmpsSec", &altAmpsSec},
  {"altEmaSec", &altEmaSec}, {"altLeadSec", &altLeadSec},
  {"altMinRunSec", &altMinRunSec}, {"altRefRadius", &altRefRadius},
  {"altMinAmps", &altMinAmps}, {"altMinDuty", &altMinDuty},
  {"altSafetyMargin", &altSafetyMargin}, {"altIdwPower", &altIdwPower}, {"altPruneK", &altPruneK},
  {"altRidgeFrac", &altRidgeFrac}, {"altRiskThresh", &altRiskThresh},
  {"altHiFieldPct", &altHiFieldPct}, {"altLowOutAmps", &altLowOutAmps}, {"altHiFieldSec", &altHiFieldSec},
  {"altPaused", &altPaused},
};
static const size_t ALT_SETTING_COUNT = sizeof(ALT_SETTINGS) / sizeof(ALT_SETTINGS[0]);

// ---- performance-vs-engine-hours trend (engine-hour buckets: average + P10 output-%) ----
// Source: FULL-steady runs graded against the active surface, one per altTrendFeedSec. P10 from a 2%-bin
// histogram (raw min let one unlucky sample define the hour). Commits on engine-time rollover with >=
// altTrendMinSamp samples, else a gap; partial bucket persists across reboot. NO clamp: % may exceed 100.
static float altEngineHoursSinceBaseline() {        // engine-HOURS since Start Over (live display)
  double s = EngineRunTime_AllTime - altTrendBaselineSec;
  if (s < 0) s = 0;
  return (float)(s / 3600.0);
}
static int altTrendBucketIndex() {                  // which bucket the current engine-time falls in
  double s = EngineRunTime_AllTime - altTrendBaselineSec;
  if (s < 0) s = 0;
  double bsec = (altTrendBucketSec > 1.0f) ? (double)altTrendBucketSec : 1.0;
  return (int)(s / bsec);
}
// Bucket P10 from the 2%-bin histogram, linearly interpolated inside the crossing bin. With n < 10
// the target lands in the lowest occupied bin, so tiny buckets degrade gracefully toward the min.
static float altBucketP10() {
  uint32_t n = (uint32_t)altBucket_n;
  if (n < 1) return 0.0f;
  uint32_t target = (n + 9) / 10, cum = 0;
  for (int i = 0; i < 60; i++) {
    if (altBucketHist[i] == 0) continue;
    if (cum + altBucketHist[i] >= target)
      return (float)(i * 2) + 2.0f * ((float)(target - cum) / (float)altBucketHist[i]);
    cum += altBucketHist[i];
  }
  return altBucket_worst * 100.0f;   // histogram/count mismatch (shouldn't happen) → raw min
}
static void altCommitTrendBucket() {
  // sample gate ≥ altTrendMinSamp: a too-thin bucket must not commit (gap, not a single-sample artifact)
  if (altCurEngHour < 0 || altBucket_n < (double)altTrendMinSamp || !altTrend) return;
  float overall = (float)(altBucket_sum / altBucket_n) * 100.0f;
  float worst   = altBucketP10();
  if (altTrendCount >= ALT_TREND_CAP) {  // window full → evict the oldest ALT_TREND_DROP block in one
    // shift; indices moved → next save rewrites the whole log (once per ~DROP hours, not per hour)
    memmove(altTrend, altTrend + ALT_TREND_DROP, (ALT_TREND_CAP - ALT_TREND_DROP) * sizeof(AltTrendPt));
    altTrendCount = ALT_TREND_CAP - ALT_TREND_DROP;
    altTrendRewrite = true;
  }
  AltTrendPt &p = altTrend[altTrendCount++];
  p.engHour = (uint16_t)altCurEngHour;
  p.worstPct = (int16_t)lroundf(worst * 10.0f);
  p.overallPct = (int16_t)lroundf(overall * 10.0f);
}
// eh = altTrendBucketIndex() when the run was graded (the 1 Hz tick, which the job may apply a few passes later).
static void altTrendAdd(float perfFrac, int eh) {
  if (altCurEngHour < 0) altCurEngHour = eh;
  if (eh != altCurEngHour) {             // engine-time bucket advanced → commit the prior bucket + reset
    altCommitTrendBucket();
    altBucket_sum = 0; altBucket_n = 0; altBucket_worst = perfFrac;
    memset(altBucketHist, 0, sizeof(altBucketHist));
    altCurEngHour = eh;
  }
  if (altBucket_n < 1 || perfFrac < altBucket_worst) altBucket_worst = perfFrac;
  altBucket_sum += perfFrac; altBucket_n += 1;
  int bin = (int)(perfFrac * 100.0f * 0.5f);   // same 2%-wide binning as the session histogram
  if (bin < 0) bin = 0;
  if (bin > 59) bin = 59;
  if (altBucketHist[bin] < 65535) altBucketHist[bin]++;
  altOverallPctLive = (float)(altBucket_sum / altBucket_n) * 100.0f;
  altWorstPctLive   = altBucketP10();
  // Partial bucket is persisted only at the ignition-off flush (altHealthSave -> altPersistTrendBucket, after the
  // field cuts) and on Reset; every shutdown passes through field-off while powered, so no per-tick NVS write.
  // (deliberately no healthy/drifting verdict here — trends are read from the plots)
}
// Persist only the partial current bucket to NVS (committed buckets live in /alttrend.bin); histogram as "bin:count" pairs.
static void altPersistTrendBucket() {
  settingWrite("altBkSum", String(altBucket_sum, 4).c_str());
  settingWrite("altBkN",   String(altBucket_n, 0).c_str());
  settingWrite("altBkWst", String(altBucket_worst, 5).c_str());
  settingWrite("altBkHr",  String(altCurEngHour).c_str());
  String h;
  for (int i = 0; i < 60; i++)
    if (altBucketHist[i] > 0) { if (h.length()) h += ','; h += String(i); h += ':'; h += String(altBucketHist[i]); }
  settingWrite("altBkHist", h.c_str());
}
static void altRestoreTrendBucket() {    // boot restore — resume the partial bucket exactly where it stopped
  if (settingExists("altBkHr")) altCurEngHour   = settingRead("altBkHr").toInt();
  if (settingExists("altBkSum")) altBucket_sum  = settingRead("altBkSum").toDouble();
  if (settingExists("altBkN"))   altBucket_n    = settingRead("altBkN").toDouble();
  if (settingExists("altBkWst")) altBucket_worst = settingRead("altBkWst").toFloat();
  if (settingExists("altBkHist")) {
    String h = settingRead("altBkHist");
    int pos = 0;
    while (pos < (int)h.length()) {
      int colon = h.indexOf(':', pos);
      if (colon < 0) break;
      int comma = h.indexOf(',', colon);
      if (comma < 0) comma = h.length();
      int bin = h.substring(pos, colon).toInt();
      long cnt = h.substring(colon + 1, comma).toInt();
      if (bin >= 0 && bin < 60 && cnt > 0) altBucketHist[bin] = (uint16_t)((cnt > 65535) ? 65535 : cnt);
      pos = comma + 1;
    }
  }
}

// ---- Demo-mode feed (altSimMode follows Hardware Present = No, 9_demo.ino) ----
// The record book grades the simulated boat through its own sensors, so the live dot, the session plot and the
// header chips all describe one alternator. Writes only the altSim* vars the fold reads in sim mode.
static float altSimRpm = 0, altSimExc = 0, altSimT = 80, altSimV = 13.6f, altSimAmps = 0;
static float altSimDuty = 50.0f;
static void altSimTick(uint32_t nowMs) {
  (void)nowMs;
  altSimRpm = RPM;
  altSimT = TempToUse;
  altSimV = getBatteryVoltage();
  altSimAmps = isnan(MeasuredAmps) ? 0.0f : MeasuredAmps;
  altSimDuty = gpio4IsLow ? 0.0f : dutyCycle;
  altSimExc = altExcitation(altSimDuty, altSimV, altSimT);
}

// Per-axis limits from the knobs, resynced every fold so live edits apply immediately. Axis MODES are set once
// in altFrontInit, before Episode::init decides which axes need deques. cfg[a].tol is both the axis limit and
// the emit-time trimmed-range tolerance (field, Vbus, temp).
static void altEpisodeSyncCfg(float ampsFilt) {
  (void)ampsFilt;                                                   // the relative amps limit now scales off the fit's window mean
  altEpisode.cfg[0] = { altRpmLineRms,  altRpmSec  };   // RPM (filtered) — straightness
  altEpisode.slopeMax[0] = altRpmRateMax;
  altEpisode.cfg[1] = { altDutyLineRms, altDutySec };   // field duty % (filtered, absolute % points) — straightness + slew cap
  altEpisode.slopeMax[1] = altDutySlewMax;
  altEpisode.cfg[2] = { altVbusTol,     altVbusSec };   // Vbus (V, filtered) — unchanged max−min band
  altEpisode.cfg[3] = { altThermDegF,   altThermSec };  // temp (°F) — verdict from altThermGate; tol still gates the emit trim test
  altEpisode.outCfg = { altAmpsLineFloorA, altAmpsSec };// output amps — straightness, floor…
  altEpisode.tolPct[ALT_NAXIS] = altAmpsLinePct;        // …raised to this share of the window mean where that is larger
  altEpisode.slopeMax[ALT_NAXIS] = altAmpsSlewFloorA;   // output slope cap — absolute A/s floor…
  altEpisode.slopePct[ALT_NAXIS] = altAmpsSlewPct;      // …raised to this share of the window mean per second
  altEpisode.minRunMs = (altMinRunSec > 0) ? (uint32_t)(altMinRunSec * 1000.0f) : 0;
  // Lead pairing in whole feed ticks. Clamped to 0..EP_LEAD_MAX-1 by setDelay; a changed delay
  // restarts the line rather than mixing two alignments in one window.
  int leadN = (int)lroundf(altLeadSec * (1000.0f / (float)EP_FEED_DT_MS));
  if (altEpisode.lead[2])         altEpisode.lead[2]->setDelay(leadN);
  if (altEpisode.lead[ALT_NAXIS]) altEpisode.lead[ALT_NAXIS]->setDelay(leadN);
}

// Emitted-run hand-off: the ~75 Hz fold only stashes here; grading/admission run in the 1 Hz
// altProcessEmits so the control path never pays a solve.
#define ALT_EMIT_QUEUE 8
struct AltEmitPending { FrontPoint<ALT_NAXIS> sp; };
static AltEmitPending altEmitQ[ALT_EMIT_QUEUE];
static int altEmitQCount = 0;
static uint32_t altLastTrendFeedMs = 0;   // altTrendFeedSec throttle, shared by altProcessEmits() and the 1 Hz job

// ---- 1 Hz record-book job (live alternator; demo mode keeps altTickInline) ----
// The tick takes what altTickInline() reads (up to two finished steady runs, the live point, the knobs, the active
// surface, the trend clock); then FrontQuery lookups run in the inline order within FRONT_STEP_US per loop pass: each
// run's grade (trend) and, only when Uploaded is the active surface, its admission lookup against My History (otherwise
// one lookup serves both); then the live grade; then AltLive and the settings echo on passes of their own. Same grades
// as the inline tick, landing a few passes after it. A front write after the tick drops the job's remaining lookups.
enum : uint8_t { ALTJ_GRADE = 0, ALTJ_ADMIT = 1, ALTJ_LIVE = 2, ALTJ_DRY = 3 };
#define ALTJ_ITEMS 10
struct AltJobItem { uint8_t kind, emit, uploaded; float x[ALT_NAXIS]; };
struct AltJob {
  bool     active;
  uint8_t  stage;                               // 0 lookups, 1 AltLive, 2 settings echo
  uint8_t  gradeUp;                             // altGradeFront() at the tick: 1 = Uploaded
  bool     learn, liveWanted, liveSessSteady, liveSteady;
  uint32_t tickMs, gen, trendFeedMs, workUs;
  int      trendEh;
  float    refRadius, idwPower, ridgeFrac, riskThresh, safetyMargin;
  float    liveSurf[ALT_NAXIS], liveGAmps;
  float    liveIn[6];                           // altSessSeriesPush() inputs at the tick
  FrontPoint<ALT_NAXIS> emit[2];
  AltJobItem item[ALTJ_ITEMS];
  uint8_t  nItem, iItem;
  uint16_t passes, lookups;
  FrontQuery<ALT_NAXIS> q;
};
static AltJob *altJob = nullptr;                // PSRAM (altFrontInit); null → altTickInline()

static bool altCapWarned = false;   // once per boot OR per Start Over (cleared in resetAlternatorHealth)

// ---- per-control-tick fold (THE canonical cadence) ----
// ══ GATE-TUNING CAPTURE ═══════════════════════════════════════════════════════════════════════
// Manual session logger (this block) + field sweeper (altSweep_tick, next to fieldCurve_tick).
// Spec: Working Markdown Docs/ALT_GATE_TUNING_CAPTURE_SPEC.md.
// Why: closed-loop data can't give per-axis output sensitivities (the regulator trades field against speed, so
// they never move independently); this logs a hand-run experiment where they do.
// 10 Hz matches EP_FEED_DT_MS, so an offline replay aligns sample-for-sample with the live gate, and clears the
// fixed 1.5006 Hz bus disturbance that a 2 Hz log would alias into the smear band.
#define ALTLOG_DT_MS       100u    // 10 Hz — the cadence the gate itself evaluates at (EP_FEED_DT_MS)
// 3 blocks, deliberately: 192 KB is a known-safe PSRAM footprint. A full capture just ends; Transfer and Record again.
#define ALTLOG_BLOCKS      3
#define ALTLOG_BLOCK_ROWS  3640    // rows per 64 KB block at 18 B/row; a row never splits across blocks
#define ALTLOG_CAP_ROWS    ((uint32_t)ALTLOG_BLOCKS * ALTLOG_BLOCK_ROWS)   // 10920 rows = 18.2 min
#define ALTLOG_STALE_MS    300u    // fold older than this → field is off / control path not running

// 18 B/row. Every axis is stored 20-100x finer than the band it gates, so quantization can never be
// mistaken for movement. Saturating stores (altQI16/altQU16): amps clip at ±327.67 A (a 500 A
// install running above that logs a flat line, which is visible; it does not wrap), bus volts at
// 65.535 V (above any 48 V-class cut), temp at ±3276.7 °F. A NaN case temp stores -32768, which the
// parser reads as "no reading" (blank) rather than as data.
struct __attribute__((packed)) AltLogRow {
  uint16_t dtMs;      // ms since the previous row (uint16 caps an in-row gap at 65 s; longer ends the segment)
  int16_t  rpm;
  uint16_t duty;      // field % ×100
  uint16_t vbus;      // mV
  int16_t  tempF;     // °F ×10 — RAW: the health tool never filters temp (slow, quantized, noise-free)
  int16_t  tSlope;    // case-temp rate, °F/min ×100 — the device's own altTempSlopeFMin, NOT recomputable offline
                      // (needs pre-Record filter history; replay must see the number stamped in ex[1]). Full
                      // resolution: near-zero vs a few °F/min is the settled-vs-warming discriminator.
  int16_t  amps;      // A ×100, EMA-filtered (altEmaSec) — the signal the detector acts on
  int16_t  ampsRaw;   // A ×100, MEAN of the fresh CH1 conversions since the previous row (see below)
  uint8_t  flags;     // b0 field on, b1 manual mode, b2 sweeper active, b3 sweep direction (1=down),
                      // b4 protection binding, b5 detector eligible, b6 detector full-steady,
                      // b7 this sweep reversed early — a control limit or Reverse now, not its ceiling
  uint8_t  gate;      // detector's PER-AXIS verdict: b0 speed, b1 field, b2 bus volts, b3 temp, b4 output amps,
                      // b5 record emitted since last row, b6 stage-9 charge-health calibration driving the field
                      // (the only capture marker for output-lead re-measurement; flags b6 is just the AND of b0..b4).
                      // b0..b4 zero while the fold is stale (field off): the axes hold pre-shutdown values there
  // No segment index, by design: the flag bits mark every transition the analysis splits on, and dtMs marks gaps.
};
static_assert(sizeof(AltLogRow) == 18, "AltLogRow lost its packing — ALTLOG_BLOCK_ROWS assumes 18 B");

// 192 KB as 3 x 64 KB blocks, NOT one allocation: a single contiguous PSRAM request this size is unsafe (cvLog's
// one 334 KB malloc has silently failed at boot). Allocated on Record, freed on dump-and-clear.
static AltLogRow *altLogBlk[ALTLOG_BLOCKS] = { nullptr, nullptr, nullptr };
static uint32_t altLogSessionId = 0;      // millis() at Record — files from one campaign sort by it
static uint32_t altLogStartMs = 0, altLogLastRowMs = 0;
static uint32_t altLogEpoch = 0;          // wall-clock seconds at session start (0 = clock never synced)
static uint8_t  altLogEndReason = 0, altLogChargeStage = 0;
static float    altLogStartTempF = 0.0f, altLogStartVbus = 0.0f;
static bool     altLogWarned60 = false;   // one-shot 60-s-remaining console warning
static double   altLogRawSum = 0.0;       // CH1 accumulator for THIS row's ampsRaw mean
static uint32_t altLogRawN = 0;
static uint32_t altLogCh1Seen = 0;        // last ch1AtCount consumed — edge-detects a fresh conversion
static int      altLogEmitSeen = 0;       // last altFrontEmitCount consumed — edge-detects a banked record
static bool     altEligibleNow = false;   // detector eligibility, exported from altFold_tick for flags b5
// Every button press is a REQUEST consumed on the control loop (the task that writes rows), so a Discard can
// never free the buffer under a row write. Release also waits for a dump in flight (web task reads the blocks),
// with a staleness escape for abandoned connections. Request flags live in Xregulator.ino (/get compiles first).
static volatile int altLogDlCount = 0;          // streams holding the blocks; changed only on the web task
static volatile uint32_t altLogDlStartMs = 0;   // refreshed per chunk, so the 30 s escape fires only on a true stall
static inline bool altLogDlActive() {
  return altLogDlCount > 0 && ((uint32_t)(millis() - altLogDlStartMs) < 30000u);
}

// A macro, not an inline function: Arduino hoists every function prototype to the top of the merged
// sketch, ahead of this struct, so anything returning an AltLogRow* would not compile.
#define ALTLOG_AT(row) (&altLogBlk[(row) / ALTLOG_BLOCK_ROWS][(row) % ALTLOG_BLOCK_ROWS])
static void altLogFree() {
  for (int i = 0; i < ALTLOG_BLOCKS; i++) { if (altLogBlk[i]) { free(altLogBlk[i]); altLogBlk[i] = nullptr; } }
}
// All-or-nothing: a partial allocation would record a short session nobody notices is short.
static bool altLogAlloc() {
  for (int i = 0; i < ALTLOG_BLOCKS; i++) {
    altLogBlk[i] = (AltLogRow *)ps_malloc((size_t)ALTLOG_BLOCK_ROWS * sizeof(AltLogRow));
    if (!altLogBlk[i]) { altLogFree(); return false; }
  }
  return true;
}

static void altLogClear() {
  altLogFree();
  altLogState = 0; altLogRows = 0; altLogSecLeft = 0; altLogEndReason = 0;
  altLogRawSum = 0.0; altLogRawN = 0; altLogWarned60 = false;
}

// reason: 1 = user Stop, 2 = capacity reached (FULL). The buffer NEVER wraps — a deliberate capture
// that silently ate its own beginning would be worse than one that stopped.
static void altLogStopRec(uint8_t reason) {
  if (altLogState != 1) return;
  altLogState = (reason == 2) ? 3 : 2;
  altLogEndReason = reason;
  float secs = (float)(millis() - altLogStartMs) / 1000.0f;
  queueConsoleMessageF("Gate capture: %s at %lu rows (%.0f s) — Transfer to keep it",
                       (reason == 2) ? "BUFFER FULL, recording stopped" : "stopped",
                       (unsigned long)altLogRows, secs);
}

static bool altLogStartRec() {
  if (altLogState == 1) { queueConsoleMessage("Gate capture: already recording"); return false; }
  if (IgnoreTemperature) {   // temp is one of the four axes AND the gate that disables the whole fold
    queueConsoleMessage("Gate capture: blocked — Ignore Temperature is on, so there is no case-temp axis to log");
    return false;
  }
  if (const char *rr = rpmTestRefuseReason()) {   // engine speed is another axis, and an ignored tach stops the fold too
    queueConsoleMessageF("Gate capture: blocked — %s", rr);
    return false;
  }
  if (altLogState != 0) {    // STOPPED or FULL, never dumped — the browser confirms before we get here
    queueConsoleMessageF("Gate capture: discarding the previous %lu-row session (never dumped)", (unsigned long)altLogRows);
    altLogClear();
  }
  if (!altLogAlloc()) {
    queueConsoleMessageF("Gate capture: PSRAM alloc FAILED for 192 KB in 3 blocks (%u KB free) — not recording",
                         (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    return false;
  }
  altLogSessionId = millis();
  altLogStartMs = altLogSessionId;
  altLogLastRowMs = altLogSessionId;
  altLogEpoch = timeIsSynced ? (uint32_t)time(NULL) : 0;
  altLogRows = 0; altLogEndReason = 0; altLogWarned60 = false;
  altLogRawSum = 0.0; altLogRawN = 0; altLogCh1Seen = ch1AtCount;
  altLogEmitSeen = altFrontEmitCount;   // so the first row does not claim every record banked before Record
  altLogStartTempF = TempToUse;
  altLogStartVbus = getBatteryVoltage();
  altLogChargeStage = (uint8_t)chargeStageDisplay;
  altLogSecLeft = (float)ALTLOG_CAP_ROWS / 10.0f;
  altLogState = 1;
  queueConsoleMessageF("Gate capture: RECORDING — %lu rows capacity, %.1f min at 10 Hz",
                       (unsigned long)ALTLOG_CAP_ROWS, altLogSecLeft / 60.0f);
  return true;
}

// Consume the button requests. Runs on the control loop, so it is the only place the 192 KB is
// allocated, freed, or handed to the state machine.
static void altLogService() {
  if (!altLogStartReq && !altLogStopReq && !altLogClearReq) return;
  if (altLogDlActive()) return;   // a dump is streaming these blocks — hold every request until it ends
  if (altLogStopReq)  { altLogStopReq = false;  altLogStopRec(1); }
  if (altLogClearReq) {
    altLogClearReq = false;
    if (altLogState == 1) queueConsoleMessage("Gate capture: clear ignored — still recording");
    else if (altLogState == 0) queueConsoleMessage("Gate capture: nothing to clear");
    else { altLogClear(); queueConsoleMessage("Gate capture: buffer cleared, 192 KB PSRAM released"); }
  }
  if (altLogStartReq) { altLogStartReq = false; altLogStartRec(); }
}

// Accumulates raw CH1 amps every loop() pass; writes one row per ALTLOG_DT_MS.
void altLog_tick(uint32_t nowMs) {
  altLogService();
  if (altLogState != 1) return;

  // ampsRaw MUST be a mean of every fresh CH1 conversion (~13 ms apart, edge-detected via ch1AtCount); a 10 Hz
  // point sample would alias current ripple. Other columns are already EMA-filtered, so point samples suit them.
  if (ch1AtCount != altLogCh1Seen) {
    altLogCh1Seen = ch1AtCount;
    if (!isnan(MeasuredAmps)) { altLogRawSum += MeasuredAmps; altLogRawN++; }
  }

  if ((uint32_t)(nowMs - altLogLastRowMs) < ALTLOG_DT_MS) return;

  // Auto-stop: the next write would exceed capacity. Stop, keep every row, say so loudly.
  if (altLogRows >= ALTLOG_CAP_ROWS) { altLogStopRec(2); return; }

  uint32_t dt = nowMs - altLogLastRowMs;
  altLogLastRowMs = nowMs;

  // Stale fold = field off or fault path: log raw live values with the field-on bit clear, so an off stretch
  // reads as off instead of as a hole.
  bool foldFresh = (altLastFoldMs != 0) && ((uint32_t)(nowMs - altLastFoldMs) < ALTLOG_STALE_MS);
  float rpm, duty, vbus, amps;
  if (foldFresh) {
    rpm = altLive_rpm; duty = altLive_duty; vbus = altLive_vbus; amps = altLive_amps;
  } else {
    rpm = RPM; duty = dutyCycle; vbus = getBatteryVoltage();
    amps = isnan(MeasuredAmps) ? 0.0f : MeasuredAmps;
    altEligibleNow = false;
  }
  float rawAmps = (altLogRawN > 0) ? (float)(altLogRawSum / (double)altLogRawN) : amps;
  altLogRawSum = 0.0; altLogRawN = 0;

  // Per-axis verdict only while the fold is fresh (field off leaves pre-shutdown flags). Emits are >= 1 s apart
  // and rows 100 ms, so the emit bit never merges two.
  uint8_t gt = 0;
  if (foldFresh) {
    if (altEpisode.axisSteady[0])         gt |= 0x01;
    if (altEpisode.axisSteady[1])         gt |= 0x02;
    if (altEpisode.axisSteady[2])         gt |= 0x04;
    if (altEpisode.axisSteady[3])         gt |= 0x08;
    if (altEpisode.axisSteady[ALT_NAXIS]) gt |= 0x10;
  }
  if (altFrontEmitCount != altLogEmitSeen) { gt |= 0x20; altLogEmitSeen = altFrontEmitCount; }
  if (chcActive != 0)                      gt |= 0x40;

  uint8_t fl = 0;
  bool manualNow = (sysMode == SYS_MODE_MANUAL);
  if (!gpio4IsLow)                 fl |= 0x01;
  if (manualNow)                   fl |= 0x02;
  if (altSweepActive != 0 && altSweepMeasuredLeg) fl |= 0x04;   // constant-rate ramp only, never the eases
  if (altSweepActive == 2)         fl |= 0x08;   // down leg
  if (ctrlLimiter == 6)            fl |= 0x10;   // a protection is binding or a recovery window is open
  if (altEligibleNow)              fl |= 0x20;
  if (altSteady)                   fl |= 0x40;
  if (altSweepActive != 0 && altSweepLimitRev)
                                   fl |= 0x80;   // sweep turned around early (control limit or Reverse now), gated on a live sweep


  AltLogRow *r = ALTLOG_AT(altLogRows);
  r->dtMs    = (dt > 65535u) ? 65535u : (uint16_t)dt;
  r->rpm     = altQI16(rpm, 1.0f);
  r->duty    = altQU16(duty, 100.0f);
  r->vbus    = altQU16(vbus, 1000.0f);
  r->tempF   = isnan(TempToUse) ? (int16_t)-32768 : altQI16(TempToUse, 10.0f);   // absent probe = blank in the dump, never 0.0 °F (§10.4)
  r->tSlope  = altQI16(altTempSlopeFMin, 100.0f);
  r->amps    = altQI16(amps, 100.0f);
  r->ampsRaw = altQI16(rawAmps, 100.0f);
  r->flags   = fl;
  r->gate    = gt;
  altLogRows++;

  altLogSecLeft = (float)(ALTLOG_CAP_ROWS - altLogRows) / 10.0f;
  if (!altLogWarned60 && altLogSecLeft <= 60.0f) {
    altLogWarned60 = true;
    queueConsoleMessage("Gate capture: 60 s of buffer left — wrap up the sweep in progress");
  }
}

// ---- dump header (128+ B, little-endian, mirrored by parseAltLogBin() in script.js) ----
// Each dump file carries the settings that shaped it, so a mid-campaign gate change can't make files silently incomparable.
struct __attribute__((packed)) AltLogHdr {
  uint32_t magic;        // 'ALGL'
  uint16_t ver;
  uint16_t rowBytes;
  uint32_t rowCount;
  uint32_t capRows;
  uint32_t sessionId;
  uint32_t epoch;        // wall clock at session start (0 = never synced)
  uint32_t startMs;
  uint32_t durMs;
  uint16_t dtMs;         // nominal sample interval
  uint8_t  voltClass;
  uint8_t  endReason;    // 0 still recording, 1 Stop, 2 FULL
  uint8_t  chargeStage;
  uint8_t  pad0[3];      // keeps the floats below 4-byte aligned
  float    startTempF, startVbus;
  float    emaSec;
  // ver 1 only: max-min band widths, written 0 since ver 2. Slots stay so one parser reads all versions at fixed offsets.
  float    v1RpmTol, v1DutyTolPct;
  float    vbusTol, thermDegF, thermSec;
  float    v1AmpsTolPct, v1AmpsFloorA;
  float    rpmSec, dutySec, vbusSec, ampsSec, minRunSec;
  uint32_t emitAvgMs;
  float    axisScale[4];
  char     fw[16];
  // ---- appended in ver 2 (straightness gate) ----
  float    rpmLineRms, rpmRateMax, dutyLineRms, dutySlewMax, ampsLinePct, ampsLineFloorA, leadSec;
  // ---- appended in ver 3 (output slope cap + the per-axis gate byte on every row) ----
  float    ampsSlewPct, ampsSlewFloorA;
};
static_assert(sizeof(AltLogHdr) == 172, "AltLogHdr size changed — parseAltLogBin() in script.js must match");

// /altlog.bin — header then rows, streamed. Refused while recording: a growing row count under a
// chunked reader would hand out a file whose header disagrees with its body.
void altLogBinSend(AsyncWebServerRequest *request) {
  // Claim the transfer interlock BEFORE reading the state it protects, or the control loop could free the
  // buffer under the streaming lambda. Early returns release it by hand (onDisconnect isn't registered yet).
  altLogDlStartMs = millis();   // before the count: altLogDlActive() reads both, and only when busy
  altLogDlCount++;
  if (altLogState == 1) {
    altLogDlCount--;
    request->send(409, "text/plain", "recording — press Stop first\n");
    return;
  }
  if (altLogState == 0 || altLogRows == 0 || !altLogBlk[0]) {
    altLogDlCount--;
    request->send(200, "application/octet-stream", "");
    return;
  }

  struct AltLogDL {
    uint8_t  hdr[sizeof(AltLogHdr)];
    int      hdrPos;
    uint32_t row, total;
    uint8_t  rowBuf[sizeof(AltLogRow)];
    int      rowPos, rowLen;
    bool     done;
  };
  AltLogDL st;
  memset(&st, 0, sizeof(st));
  st.total = altLogRows;

  // Each stream releases its own hold exactly once (done or disconnect); a shared flag lets overlapping downloads release each other's.
  auto held = std::make_shared<bool>(true);
  auto release = [held]() { if (*held) { *held = false; if (altLogDlCount > 0) altLogDlCount--; } };
  request->onDisconnect([release]() { release(); });   // Safari tears connections down abruptly

  AltLogHdr h;
  memset(&h, 0, sizeof(h));
  h.magic = 0x414C474Cu;   // 'ALGL'
  h.ver = 3;               // 3 = output slope cap + per-axis gate byte (2 = straightness gate, 1 = max−min bands)
  h.rowBytes = (uint16_t)sizeof(AltLogRow);
  h.rowCount = altLogRows;
  h.capRows = ALTLOG_CAP_ROWS;
  h.sessionId = altLogSessionId;
  h.epoch = altLogEpoch;
  h.startMs = altLogStartMs;
  h.durMs = altLogLastRowMs - altLogStartMs;
  h.dtMs = (uint16_t)ALTLOG_DT_MS;
  h.voltClass = (uint8_t)SYSTEM_VOLTAGE_CLASS;
  h.endReason = altLogEndReason;
  h.chargeStage = altLogChargeStage;
  h.startTempF = altLogStartTempF;
  h.startVbus = altLogStartVbus;
  h.emaSec = altEmaSec;
  h.vbusTol = altVbusTol;     h.thermDegF = altThermDegF; h.thermSec = altThermSec;
  h.rpmSec = altRpmSec;       h.dutySec = altDutySec;     h.vbusSec = altVbusSec;
  h.ampsSec = altAmpsSec;     h.minRunSec = altMinRunSec;
  h.rpmLineRms = altRpmLineRms;   h.rpmRateMax = altRpmRateMax;
  h.dutyLineRms = altDutyLineRms; h.dutySlewMax = altDutySlewMax;
  h.ampsLinePct = altAmpsLinePct; h.ampsLineFloorA = altAmpsLineFloorA;
  h.leadSec = altLeadSec;
  h.ampsSlewPct = altAmpsSlewPct; h.ampsSlewFloorA = altAmpsSlewFloorA;
  // v1RpmTol / v1DutyTolPct / v1AmpsTolPct / v1AmpsFloorA stay at the memset 0 — those settings no
  // longer exist, and a file must not claim a gate that did not shape it.
  h.emitAvgMs = (uint32_t)ALT_EMIT_AVG_MS;
  for (int a = 0; a < ALT_NAXIS; a++) h.axisScale[a] = altFront2.axisScale[a];
  strncpy(h.fw, FIRMWARE_VERSION, sizeof(h.fw) - 1);
  memcpy(st.hdr, &h, sizeof(h));

  AsyncWebServerResponse *response = request->beginChunkedResponse(
    "application/octet-stream",
    [st, release](uint8_t *buf, size_t maxLen, size_t) mutable -> size_t {
      if (st.done) return 0;
      altLogDlStartMs = millis();   // progress = alive; see altLogDlActive()
      // If the 30 s escape did lapse during a stall and a Clear freed the blocks, end the stream
      // instead of reading a null block (all three are freed together).
      if (!altLogBlk[0]) { st.done = true; release(); return 0; }
      size_t written = 0;
      while (written < maxLen) {
        if (st.hdrPos < (int)sizeof(AltLogHdr)) {
          size_t canSend = min(maxLen - written, (size_t)((int)sizeof(AltLogHdr) - st.hdrPos));
          memcpy(buf + written, st.hdr + st.hdrPos, canSend);
          written += canSend;
          st.hdrPos += (int)canSend;
          continue;
        }
        if (st.rowPos >= st.rowLen) {
          if (st.row >= st.total) { st.done = true; release(); return written; }
          memcpy(st.rowBuf, ALTLOG_AT(st.row), sizeof(AltLogRow));
          st.rowLen = (int)sizeof(AltLogRow);
          st.rowPos = 0;
          st.row++;
        }
        size_t canSend = min(maxLen - written, (size_t)(st.rowLen - st.rowPos));
        memcpy(buf + written, st.rowBuf + st.rowPos, canSend);
        written += canSend;
        st.rowPos += (int)canSend;
      }
      return written;
    });
  response->addHeader("Cache-Control", "no-cache");
  request->send(response);
}

// Live: called from the pidLog hook (~75 Hz); bench-sim: at EP_FEED_DT_MS from altHealth_tick. EMA-filters
// the detector inputs, feeds Episode, and only STASHES emits into altEmitQ; O(count) work runs at 1 Hz in
// altProcessEmits. Off/fault/shutdown paths return before the pidLog hook, so no mode check is needed.
void altFold_tick(uint32_t nowMs) {
  if (!altFrontBuf || !altEpRing) return;

  // IgnoreTemperature or IgnoreRPM disables ALL of alt-health (both are surface axes, §4.8); altHealth_tick reports status 3.
  if (IgnoreTemperature || IgnoreRPM) { altLiveValid = false; altSteady = false; altSessSteady = false; return; }

  float rpm, tF, vbus, amps, duty;
  if (altSimMode >= 0.5f) {
    rpm = altSimRpm; tF = altSimT; vbus = altSimV; amps = altSimAmps; duty = altSimDuty;
  } else {
    vbus = getBatteryVoltage();
    amps = isnan(MeasuredAmps) ? 0.0f : MeasuredAmps;
    tF = TempToUse; duty = dutyCycle; rpm = RPM;
  }

  // Detector-input EMA (altEmaSec) strips duty dither, RPM jitter and current ripple so bands size for real
  // operating-point movement; control loops never see it. Temp stays raw (DS18B20: slow, quantized, noise-free).
  // A long fold gap (field off, boot) reseeds from raw.
  static float fRpm = 0, fDuty = 0, fVbus = 0, fAmps = 0;
  static uint32_t lastFoldMs = 0;
  static bool fInit = false;
  float tauMs = altEmaSec * 1000.0f;
  uint32_t dtMs = nowMs - lastFoldMs;
  lastFoldMs = nowMs;
  if (!fInit || tauMs < 1.0f || dtMs > (uint32_t)(5.0f * tauMs) + 1000u) {
    fRpm = rpm; fDuty = duty; fVbus = vbus; fAmps = amps; fInit = true;
  } else {
    float a = (float)dtMs / (tauMs + (float)dtMs);   // irregular-interval EMA
    fRpm  += a * (rpm  - fRpm);  fDuty += a * (duty - fDuty);
    fVbus += a * (vbus - fVbus); fAmps += a * (amps - fAmps);
  }
  // Bench-sim injects its own consistent excitation; live derives it from the filtered drive.
  float exc = (altSimMode >= 0.5f) ? altSimExc : altExcitation(fDuty, fVbus, tF);

  // Exported for the 1 Hz evaluator (altHealth_tick).
  altLive_rpm = fRpm; altLive_exc = exc; altLive_amps = fAmps;
  altLive_vbus = fVbus; altLive_tF = tF; altLive_duty = fDuty;

  // Case-temp slope (°F/min) from a fast-minus-slow EMA pair (settles at rate*(tauSlow-tauFast)). The case sensor
  // lags the windings, so this tags thermal transients: rides AltLive SSE and every record's ex[1].
  {
    static float tsF = 0, tsS = 0; static uint32_t tsLast = 0;
    if (!isnan(tF)) {
      if (!tsLast) { tsF = tsS = tF; tsLast = nowMs; }
      float dt = (nowMs - tsLast) * 0.001f;
      if (dt > 0.004f) {
        tsLast = nowMs;
        if (dt > 5.0f) { tsF = tsS = tF; }          // feed gap: reseed instead of a spike
        else {
          tsF += (tF - tsF) * (dt / (5.0f + dt));   // tau 5 s
          tsS += (tF - tsS) * (dt / (60.0f + dt));  // tau 60 s
          altTempSlopeFMin = (tsF - tsS) / 55.0f * 60.0f;
        }
      }
    }
  }
  altLiveValid = (!isnan(fRpm) && !isnan(exc) && !isnan(fVbus) && fVbus >= ALT_MIN_BATT_V * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f));
  altLastFoldMs = nowMs;

  // Detection runs regardless of Pause / Reference Source: the trend (full-steady runs graded against
  // the ACTIVE surface) and the Session orange ring must work even while learning is paused or grading
  // against an Uploaded surface. Only surface ADMISSION into My History is gated by Pause (altProcessEmits).
  if (hardwarePresent != 1 && altSimMode < 0.5f) {                                   // no real hardware → display only
    altSteady = false; altSessSteady = false;
    altThermGate.clear(); altSessTempGate.clear(); altEpisode.clearRun();
    return;
  }

  // Everything fed filtered, including the admission floors, so one noise dip can't act as a barrier mid-run.
  altEpisodeSyncCfg(fAmps);
  bool eligible = (!isnan(fVbus) && fVbus >= ALT_MIN_BATT_V * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f) && fAmps >= altMinAmps && fDuty >= altMinDuty && fRpm >= 0);
  altEligibleNow = eligible;                                                    // exported for the gate-capture log's flags byte
  // Both temp gates are fed every fold, eligible or not: trailing thermal windows, not run timers (see AltTempGate).
  altThermGate.feed(tF, nowMs, altThermDegF, altThermSec);
  altSessTempGate.feed(tF, nowMs, altThermDegF, altSessTempDwell());
  altEpisode.extOk[3] = altThermGate.steady;
  RawSample<ALT_NAXIS> s;
  s.x[0] = fRpm; s.x[1] = fDuty; s.x[2] = fVbus; s.x[3] = tF; s.out = fAmps; s.tMs = nowMs;
  s.ex[0] = fDuty; s.ex[1] = altTempSlopeFMin;   // retain run duty + case-temp slope (F/min) for diagnosis;
                                                 // the emit averages both, so ex[1] = the run's mean thermal transient
  FrontPoint<ALT_NAXIS> ep;
  bool emitted = altEpisode.feed(eligible, s, &ep);
  altSteady = (altEpisode.count > 0);                              // FULL steady (temp held altThermSec) → ring + surface + trend
  // SESSION steady = all non-temp axes qualifying plus temp steady for HALF the dwell, so a Session-plot dot can
  // appear before the record-book dwell; it uses the same per-axis verdicts as the record book.
  altSessSteady = eligible
                  && altEpisode.axisSteady[0] && altEpisode.axisSteady[1] && altEpisode.axisSteady[2]
                  && altEpisode.axisSteady[ALT_NAXIS] && altSessTempGate.steady;
  if (!emitted) return;

  // Stash UNCONDITIONALLY (even paused) so the trend still sees it; altProcessEmits grades and admits at 1 Hz.
  altFrontEmitCount++;
  FrontPoint<ALT_NAXIS> sp;
  sp.x[0] = ep.x[0];                                    // RPM
  sp.x[1] = altExcitation(ep.x[1], ep.x[2], ep.x[3]);  // excitation
  sp.x[2] = ep.x[2];                                    // Vbus
  sp.x[3] = ep.x[3];                                    // tempF
  sp.ex[0] = ep.x[1];                                   // raw run-avg duty (retained for cloud diagnosis)
  sp.ex[1] = ep.ex[1];                                  // run-avg case-temp slope (F/min) — thermal-transient tag
  sp.y = ep.y; sp.nSamp = ep.nSamp; sp.tEmit = ep.tEmit;
  if (altEmitQCount < ALT_EMIT_QUEUE) altEmitQ[altEmitQCount++].sp = sp;
}

// Learning arms only when COMMISSIONED with stage 9 (Charge Health Calibration) done: that stage measures
// altLeadSec, and wizard runs must never set records (records only ratchet up). Re-commission pauses learning
// with history kept; "Mark done manually" on stage 9 arms it with the default lead.
#define ALT_STATUS_WAIT_CX  4   // altStatusCode: not commissioned
#define ALT_STATUS_WAIT_CHC 5   // altStatusCode: commissioned, stage 9 (Charge Health Calibration) not done
static bool altLearnArmed() { return commissionState == 2 && (commissionDoneMask & (1u << 9)) != 0; }

// Drain the emit queue (1 Hz, same task as the fold, no lock); at most 2 per tick to bound the loop() pass.
static void altProcessEmits() {
  if (!altLearnArmed()) { altEmitQCount = 0; return; }   // not armed: no record, no trend sample, no upload
  int n = (altEmitQCount < 2) ? altEmitQCount : 2;
  bool learn = (altPaused < 0.5f) && altSimMode < 0.5f;   // learning active → also admit into My History; else trend-only
                                                           // (demo: the record stays the seeded one, 9_demo.ino)
  for (int k = 0; k < n; k++) {
    FrontPoint<ALT_NAXIS> &sp = altEmitQ[k].sp;

    // (1) TREND FEED: the trend's ONLY source. FULL-steady MEASURED runs graded against the ACTIVE surface, one per
    // altTrendFeedSec; Estimated grades lean on interpolation and would blur it.
    {
      float pred = 0;
      int st = (altSimMode >= 0.5f) ? demoAltGrade(sp.x, sp.y, millis(), &pred)
                                    : altGradeFront().classify(sp.x, altRefRadius, altIdwPower, altRidgeFrac, altRiskThresh, &pred);
      bool graded = (st == FRONT_MEASURED) && pred > 0.1f;
      uint32_t nowMs = millis();
      if (graded && (altLastTrendFeedMs == 0 || (uint32_t)(nowMs - altLastTrendFeedMs) >= (uint32_t)(altTrendFeedSec * 1000.0f))) {
        altLastTrendFeedMs = nowMs;
        altTrendAdd((sp.y / pred), altTrendBucketIndex());   // output-% as a fraction; altTrendAdd scales ×100 + buckets it
      }
    }

    // (2) SURFACE ADMISSION into My History only, while learning. Grades against My History, not the active
    // surface, so a borrowed reference can't gate what you record.
    if (!learn) continue;
    float yref = 0, bar = 0;
    altFront2.classify(sp.x, altRefRadius, altIdwPower, altRidgeFrac, altRiskThresh, &yref, &bar);
    // Cell-local admit gate: unvisited cell → unconditional; same-cell support → beat bar − margin.
    bool localSup = altFront2.hasLocalSupport(sp.x);
    if (localSup && !(sp.y > bar - altSafetyMargin)) continue;
    if (!localSup && altFront2.count >= ALT_FRONT_CAP) {
      if (!altCapWarned) {   // a genuinely NEW cell dropped at capacity (in-cell improvements still land)
        altCapWarned = true;
        queueConsoleMessage("WARN: alt front at capacity — new operating cells are no longer recorded");
      }
      continue;
    }
    // No console message on accept: it spams on same-cell improvements at steady state.
    if (altFront2.add(sp)) {                                              // optimistic local front (cloud re-prunes)
      // Sim-folded points stay local (My History) — never queued, so synthetic data can't reach the cloud front
      if (altSimMode < 0.5f && altPending && altPendingCount < ALT_PENDING_CAP) altPending[altPendingCount++] = sp;
    }
  }
  if (altEmitQCount > n) memmove(altEmitQ, altEmitQ + n, (size_t)(altEmitQCount - n) * sizeof(AltEmitPending));
  altEmitQCount -= n;
}

// ---- live telemetry registry (schema-driven: one source for the AltLive payload + /altschema) ----
// One {name, getter} table builds BOTH the AltLive SSE payload AND the /altschema field list, so
// the firmware↔dashboard contract can't desync. Adding a field = add ONE row. All values floats.
struct AltLiveField { const char *name; float (*get)(); };
// Demo mode never serves an ungraded tick (field off, engine stopped, between steady points): it serves the last grade
static inline bool alfDemoHold() { return altSimMode >= 0.5f && !(altRefOk && altLive_pct > 0.0f); }
static float alf_valid()     { return (altSimMode >= 0.5f) ? 1.0f : (float)altLiveValid; }
static float alf_rpm()       { return altLive_rpm; }
static float alf_exc()       { return altLive_exc; }
static float alf_amps()      { return altLive_amps; }
static float alf_pred()      { return altLive_pred; }
static float alf_pct()       { return alfDemoHold() ? altLatchPct : altLive_pct; }
static float alf_latchPct()  { return altLatchPct; }                             // header Health chip (device-side latch)
static float alf_worst()     { return altWorstPctLive; }
static float alf_overall()   { return altOverallPctLive; }
static float alf_status()    { return (float)altStatusCode; }
static float alf_steady()    { return (altSimMode >= 0.5f) ? 1.0f : (float)altSteady; }      // FULL steady run (temp held STEADY_TEMP_SEC) → orange ring
static float alf_sessSteady(){ return (altSimMode >= 0.5f) ? 1.0f : (float)altSessSteady; }  // SESSION steady (temp held SESSION_TEMP_SEC) → session-plot dot gate
static float alf_engHours()  { return altEngineHoursSinceBaseline(); }
static float alf_coverage()  { return altFrontBuf ? (100.0f * (float)altGradeFront().count / (float)ALT_FRONT_CAP) : 0.0f; }
static float alf_haveCurve() { return (float)(altGradeFront().count > 0 ? 1 : 0); }   // active surface has a usable front
static float alf_ptCount()   { return (float)altGradeFront().count; }           // active-surface support points
static float alf_source()    { return (float)altRefSource; }                    // 0 = My History, 1 = Uploaded File (active grading surface)
static float alf_haveUpload(){ return altHaveUpload ? 1.0f : 0.0f; }            // an Uploaded surface is resident
static float alf_paused()    { return (altPaused >= 0.5f) ? 1.0f : 0.0f; }
static float alf_refOk()     { return (altRefOk || altSimMode >= 0.5f) ? 1.0f : 0.0f; }   // state is MEASURED/ESTIMATED → % shown
static float alf_refDist()   { return altRefDist; }                       // normalized distance to nearest support
static float alf_state()     { return alfDemoHold() ? (float)FRONT_MEASURED : (float)altState; }   // 0 MEASURED, 1 ESTIMATED, 2 LEARNING_EDGE, 3 NO_REFERENCE
static float alf_sessMean()  { return (altSessN > 0) ? (float)(altSessSum / (double)altSessN) : 0.0f; }
static float alf_sessP10()   {                                            // P10 of the session histogram (≥10 samples), interpolated
  if (altSessN < 10) return 0.0f;                                          // inside its 2 % bin as altBucketP10 does
  uint32_t target = (altSessN + 9) / 10, cum = 0;
  for (int i = 0; i < 60; i++) {
    if (altSessHist[i] == 0) continue;
    if (cum + altSessHist[i] >= target) return (float)(i * 2) + 2.0f * ((float)(target - cum) / (float)altSessHist[i]);
    cum += altSessHist[i];
  }
  return 0.0f;
}
static float alf_sessN()     { return (float)altSessN; }
static float alf_hiField()   { return altHiFieldAlert ? 1.0f : 0.0f; }    // high-field-low-output alert active
static float alf_sim()       { return (altSimMode >= 0.5f) ? 1.0f : 0.0f; }
static float alf_gAmps()     { return altLive_gAmps; }                    // graded boxcar-average amps (the % numerator)
static float alf_tSlope()    { return altTempSlopeFMin; }                 // case-temp slope (F/min) — thermal-transient tag
// Gate-capture state rides AltLive, not a CSV channel: the frame is schema-zipped (/altschema), so appending a
// field can't desync a stale-open dashboard the way a CSV index map can.
static float alf_logState()  { return (float)altLogState; }               // 0 idle, 1 recording, 2 stopped, 3 full
static float alf_logRows()   { return (float)altLogRows; }
static float alf_logSecLeft(){ return altLogSecLeft; }                    // capacity remaining at the fixed 10 Hz rate
static float alf_logCap()    { return (float)ALTLOG_CAP_ROWS; }
static float alf_sweepState(){ return (float)altSweepActive; }            // 0 idle, 1 ramp up, 2 ramp down, 3 easing out
static float alf_sweepRev()  { return (float)altSweepRevWhy; }            // 0 ceiling, 1 a control limit, 2 Reverse now
static float alf_syncAgo()   { if (demoSeal) return demoSyncAgoS(420.0f);   // demo mode: a believable sync age, nothing is uploaded
                               if (lastAltHealthSyncEpoch <= 0 || !timeIsSynced) return -1.0f;
                               time_t n = time(NULL); return (n > (time_t)lastAltHealthSyncEpoch) ? (float)(n - (time_t)lastAltHealthSyncEpoch) : 0.0f; }
// fold timing lives in the Function Timing table (ft_altHealth / ft_altFold rows) — not in this live stream
static AltLiveField ALT_LIVE[] = {
  {"valid", alf_valid}, {"rpm", alf_rpm}, {"exc", alf_exc}, {"amps", alf_amps},
  {"pred", alf_pred}, {"pct", alf_pct}, {"worstPct", alf_worst}, {"overallPct", alf_overall},
  {"status", alf_status}, {"steady", alf_steady}, {"sessSteady", alf_sessSteady}, {"engHours", alf_engHours},
  {"coverage", alf_coverage}, {"haveCurve", alf_haveCurve}, {"ptCount", alf_ptCount},
  {"source", alf_source}, {"haveUpload", alf_haveUpload}, {"paused", alf_paused},
  {"refOk", alf_refOk}, {"refDist", alf_refDist},
  {"state", alf_state},
  {"sessionMean", alf_sessMean}, {"sessionP10", alf_sessP10}, {"sessionN", alf_sessN},
  {"hiFieldAlert", alf_hiField},
  {"sim", alf_sim}, {"syncAgoS", alf_syncAgo},
  {"gAmps", alf_gAmps},
  {"tSlope", alf_tSlope},   // appended at END — stale-open dashboards zip fewer schema names than values, extras ignored
  {"logState", alf_logState}, {"logRows", alf_logRows}, {"logSecLeft", alf_logSecLeft},
  {"logCap", alf_logCap},
  {"sweepState", alf_sweepState}, {"sweepRev", alf_sweepRev},
  {"latchPct", alf_latchPct},
};
static const size_t ALT_LIVE_COUNT = sizeof(ALT_LIVE) / sizeof(ALT_LIVE[0]);
static void altSendLive() {
  // periodic; the next tick resends current state. A backlogged phone link skips only the phone copy.
  const bool phoneOk = !sseBacklogged();
  if (!phoneOk && !gxLiveNow) return;
  char buf[512];   // truncates rather than overruns (see the break below); sized well past the field list
  int off = 0;
  for (size_t i = 0; i < ALT_LIVE_COUNT; i++) {
    int n = snprintf(buf + off, sizeof(buf) - off, (i ? ",%.3f" : "%.3f"), ALT_LIVE[i].get());
    if (n < 0 || off + n >= (int)sizeof(buf)) break;   // truncated echo beats stack overrun (snprintf returns would-be length)
    off += n;
  }
  if (phoneOk) sseSend(buf, "AltLive");
  gxSideOffer(GX_CH_ALTLIVE, buf, off);
}

// ---- coverage / status helpers (CSV2 + dashboard) ----
float altCoveragePct() { return altFrontBuf ? (100.0f * (float)altFront2.count / (float)ALT_FRONT_CAP) : 0.0f; }
float altWorstPct()    { return altWorstPctLive; }
int   altStatus()      { return (int)altStatusCode; }
int   altHaveFront()   { return (altFront2.count > 0) ? 1 : 0; }   // CSV2: have a usable best-ever front
int   altFrontCount()  { return altFront2.count; }                 // CSV2: front support-point count
// Curves owed to this unit's own cloud record (NK_recSeedOwed): an import (bits 1/2) or a new record after re-registration
// (4/8). The cloud rebuilds each curve from this unit's rows each sync, so until it holds the curve, each boot re-queues
// the whole curve as one seeded batch labeled "import" or "reregister".
void recSeedOwedSet(uint8_t bits, bool on) {
  uint8_t cur = settingExists(NK_recSeedOwed) ? (uint8_t)strtoul(settingRead(NK_recSeedOwed).c_str(), nullptr, 16) : 0;
  uint8_t nv = on ? (cur | bits) : (cur & ~bits);
  if (nv == cur && (nv || !settingExists(NK_recSeedOwed))) return;
  if (!nv) { settingRemove(NK_recSeedOwed); return; }
  char b[4];
  snprintf(b, sizeof(b), "%x", (unsigned)nv);
  settingWrite(NK_recSeedOwed, b);
}
static bool recSeedOwed(uint8_t bit) {
  return settingExists(NK_recSeedOwed) && ((uint8_t)strtoul(settingRead(NK_recSeedOwed).c_str(), nullptr, 16) & bit);
}
// A new cloud record (re-registration after the old one was deleted) holds none of this unit's points, so its first
// sync reply would collapse the local curves. Core 0 owes both curves to it (bits 4/8 survive a reboot) and asks
// Core 1 to seed the pending queues now; the boot path seeds from the same bits.
static volatile uint8_t regSeedRequest = 0;
void recSeedAfterNewRegistration() {
  recSeedOwedSet(4 | 8, true);
  __atomic_fetch_or(&regSeedRequest, (uint8_t)(4 | 8), __ATOMIC_ACQ_REL);
}
// Core 0, on a successful alt-health upload; takes body (freed here unless the hand-off keeps it). False without
// PSRAM for the hand-off: the batch then stays pending and is re-sent, which the cloud absorbs (resent points are no-ops there).
bool altHandSync(bool ingest, char *body) {
  if (altSentSeeded) recSeedOwedSet(1 | 4, false);   // the seeded curve reached the cloud
  FrontSyncHandoff *h = FrontSyncHandoff::make(ingest, body);
  if (!h) return false;
  FrontSyncHandoff *old = __atomic_exchange_n(&altSyncHandoff, h, __ATOMIC_ACQ_REL);
  FrontSyncHandoff::drop(old);
  return true;
}
// Core 1: drop only the points that batch carried (pending kept growing while it was in flight), replace My
// History with the cloud's pruned front, then re-add the still-unsent points, which the cloud has not seen.
static void altApplySync() {
  FrontSyncHandoff *h = __atomic_exchange_n(&altSyncHandoff, (FrontSyncHandoff *)nullptr, __ATOMIC_ACQ_REL);
  if (!h) return;
  int d = (altSentCount < altPendingCount) ? altSentCount : altPendingCount;
  if (d > 0) {
    memmove(altPending, altPending + d, (size_t)(altPendingCount - d) * sizeof(FrontPoint<ALT_NAXIS>));
    altPendingCount -= d;
    altPendingSeededFrom = "";   // a batch built before a seed carried none of it: the label stays for the seeded batch
  }
  altSentCount = 0;
  if (h->ingest && !rpmAxisWipePending && !altCloudWipePending) {
    if (altIngestFrontCsv(h->body)) {
      for (int i = 0; i < altPendingCount; i++) altFront2.add(altPending[i]);
      queueConsoleMessage("Alternator health: front updated from cloud");
    } else {
      queueConsoleMessage("Alternator health uploaded (no front in response)");
    }
  }
  FrontSyncHandoff::drop(h);
  altHealthSave();   // caller runs this only with the field physically cut (a LittleFS write stalls the flash cache)
}

// ---- front CSV (the artifact): BEFRONT1,<sys>,<naxis>,<source>,<units…> then x0..xN,y,nSamp,tEmit ----
// Serves /altcurve.csv, the cloud sync-back, and Save/Load (spec §2.2/§8). Streamed row-by-row: a ~150 KB
// String at the 4096-point cap fragments the internal heap TLS needs 32-40 KB contiguous from. total/source are
// snapshotted; a concurrent Core-1 same-cell edit just shows a fresher value for one row.
void altCurveCsvSend(AsyncWebServerRequest *request) {
  struct St { int total, source, idx; bool done; char line[96]; int len, pos; };
  St st; st.total = altFrontBuf ? altFront2.count : 0; st.source = altFront2.source;
  st.idx = 0; st.done = false; st.len = 0; st.pos = 0;
  AsyncWebServerResponse *response = request->beginChunkedResponse("text/plain",
    [st](uint8_t *buf, size_t maxLen, size_t) mutable -> size_t {
      size_t written = 0;
      while (written < maxLen) {
        if (st.pos >= st.len) {
          if (st.done) return written;
          if (st.idx == 0) {
            st.len = snprintf(st.line, sizeof(st.line), "BEFRONT1,ALT,%d,%d,rpm,exc,V,F,amps\n", ALT_NAXIS, st.source);
          } else {
            int i = st.idx - 1;
            if (i >= st.total) { st.done = true; return written; }
            FrontPoint<ALT_NAXIS> &p = altFrontBuf[i];
            st.len = snprintf(st.line, sizeof(st.line), "%.0f,%.3f,%.2f,%.1f,%.2f,%u,%u\n",
                              p.x[0], p.x[1], p.x[2], p.x[3], p.y, (unsigned)p.nSamp, (unsigned)p.tEmit);
          }
          if (st.len > (int)sizeof(st.line) - 1) st.len = sizeof(st.line) - 1;  // clamp snprintf's intended-len to the buffer (defensive vs a future wider field)
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
// Plain front-points table for the dashboard scatter view (/altrecords.csv). Distinct from the
// BEFRONT1 artifact above; streamed row-by-row (constant RAM at any count ≤ ALT_FRONT_CAP).
void altFrontRecordsCsvSend(AsyncWebServerRequest *request) {
  struct St { int total, idx; bool done; char line[96]; int len, pos; };
  St st; st.total = altFrontBuf ? altFront2.count : 0;
  st.idx = 0; st.done = false; st.len = 0; st.pos = 0;
  AsyncWebServerResponse *response = request->beginChunkedResponse("text/csv",
    [st](uint8_t *buf, size_t maxLen, size_t) mutable -> size_t {
      size_t written = 0;
      while (written < maxLen) {
        if (st.pos >= st.len) {
          if (st.done) return written;
          if (st.idx == 0) {
            st.len = snprintf(st.line, sizeof(st.line), "rpm,exc,vbus,tF,amps,nSamp\n");
          } else {
            int i = st.idx - 1;
            if (i >= st.total) { st.done = true; return written; }
            FrontPoint<ALT_NAXIS> &p = altFrontBuf[i];
            st.len = snprintf(st.line, sizeof(st.line), "%.0f,%.3f,%.2f,%.1f,%.2f,%u\n",
                              p.x[0], p.x[1], p.x[2], p.x[3], p.y, (unsigned)p.nSamp);
          }
          if (st.len > (int)sizeof(st.line) - 1) st.len = sizeof(st.line) - 1;  // clamp snprintf's intended-len to the buffer (defensive vs a future wider field)
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
// /altsess.csv, streamed oldest-first. Header carries device millis so the client maps tMs to wall clock; a push
// can overwrite the oldest unread slot mid-stream, so the client drops any row whose tMs goes backwards.
void altSessCsvSend(AsyncWebServerRequest *request) {
  struct St { uint32_t total, base, idx, nowMs; bool done; char line[144]; int len, pos; };
  St st;
  st.total = altSessRing ? altSessRingCount : 0;
  st.base  = (altSessRingHead + ALT_SESS_RING_N - st.total) % ALT_SESS_RING_N;   // oldest live slot
  st.nowMs = millis();
  st.idx = 0; st.done = false; st.len = 0; st.pos = 0;
  AsyncWebServerResponse *response = request->beginChunkedResponse("text/csv",
    [st](uint8_t *buf, size_t maxLen, size_t) mutable -> size_t {
      size_t written = 0;
      while (written < maxLen) {
        if (st.pos >= st.len) {
          if (st.done) return written;
          if (st.idx == 0) {
            st.len = snprintf(st.line, sizeof(st.line), "# altsess v1 nowMs=%lu dtMs=%lu n=%lu\n",
                              (unsigned long)st.nowMs, (unsigned long)ALT_SESS_DT_MS, (unsigned long)st.total);
          } else if (st.idx == 1) {
            st.len = snprintf(st.line, sizeof(st.line),
                              "tMs,pct,gAmps,measAmps,expected,rpm,fieldPct,tempF,vbus,tSlopeFmin,state,ring\n");
          } else {
            uint32_t i = st.idx - 2;
            if (i >= st.total) { st.done = true; return written; }
            const AltSessPt &q = altSessRing[(st.base + i) % ALT_SESS_RING_N];
            uint8_t state = q.flags & 0x03;
            // pct is reconstructed, not stored: altLive_pct IS gAmps/pred×100, so the only loss is the
            // 0.01 A quantisation of the two operands (~0.08% at 18 A).
            float pct = (state <= 1 && q.pred > 0) ? ((float)q.gAmps * 100.0f / (float)q.pred) : 0.0f;
            st.len = snprintf(st.line, sizeof(st.line), "%lu,%.2f,%.2f,%.2f,%.2f,%u,%.2f,%.1f,%.2f,%.2f,%u,%u\n",
                              (unsigned long)q.tMs, pct, q.gAmps / 100.0f, q.measA / 100.0f,
                              q.pred / 100.0f, (unsigned)q.rpm, q.duty / 100.0f, q.tF / 10.0f,
                              q.vbus / 100.0f, q.tSlope / 100.0f, (unsigned)state,
                              (unsigned)((q.flags >> 2) & 0x01));
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
// Full alt-health state dump (/altdebug.csv, spec §4.5), streamed. Self-describing "section,key,v0…" rows:
// PARAM, LIVE/GATE/BUCKET/SESSION scalars, MYHIST + UPLOAD points, TREND buckets.
void altDebugCsvSend(AsyncWebServerRequest *request) {
  struct St { int phase, idx, myN, upN, trN; bool done; char line[176]; int len, pos; };
  St st; st.phase = 0; st.idx = 0; st.done = false; st.len = 0; st.pos = 0;
  st.myN = altFrontBuf ? altFront2.count : 0;
  st.upN = altFrontUpBuf ? altFrontUp.count : 0;
  st.trN = altTrend ? altTrendCount : 0;
  AsyncWebServerResponse *response = request->beginChunkedResponse("text/csv",
    [st](uint8_t *buf, size_t maxLen, size_t) mutable -> size_t {
      size_t written = 0;
      while (written < maxLen) {
        if (st.pos >= st.len) {
          if (st.done) return written;
          st.len = 0;
          if (st.phase == 0) {                                   // header
            st.len = snprintf(st.line, sizeof(st.line), "section,key,v0,v1,v2,v3,v4,v5,v6\n");
            st.phase = 1; st.idx = 0;
          } else if (st.phase == 1) {                            // PARAM — every registry knob
            if (st.idx < (int)ALT_SETTING_COUNT) {
              st.len = snprintf(st.line, sizeof(st.line), "PARAM,%s,%.4f\n", ALT_SETTINGS[st.idx].name, *ALT_SETTINGS[st.idx].ptr);
              st.idx++;
            } else { st.phase = 2; st.idx = 0; }
          } else if (st.phase == 2) {                            // scalar state
            switch (st.idx) {
              case 0: st.len = snprintf(st.line, sizeof(st.line), "LIVE,rpm_exc_vbus_tF_amps,%.0f,%.3f,%.2f,%.1f,%.2f\n", altLive_rpm, altLive_exc, altLive_vbus, altLive_tF, altLive_amps); break;
              case 1: st.len = snprintf(st.line, sizeof(st.line), "LIVE,duty_pred_pct_tslope,%.1f,%.2f,%.1f,%.2f\n", altLive_duty, altLive_pred, altLive_pct, altTempSlopeFMin); break;
              case 2: st.len = snprintf(st.line, sizeof(st.line), "LIVE,state_refOk_refDist,%d,%d,%.3f\n", (int)altState, (int)altRefOk, altRefDist); break;
              case 3: st.len = snprintf(st.line, sizeof(st.line), "GATE,sessSteady_fullSteady_valid,%d,%d,%d\n", (int)altSessSteady, (int)altSteady, (int)altLiveValid); break;
              case 4: st.len = snprintf(st.line, sizeof(st.line), "GATE,ignoreTemp_paused_refSrc_haveUp_sim,%d,%.0f,%d,%d,%.0f\n", (int)IgnoreTemperature, altPaused, (int)altRefSource, (int)altHaveUpload, altSimMode); break;
              case 5: st.len = snprintf(st.line, sizeof(st.line), "BUCKET,sum_n_min_p10_idx,%.4f,%.0f,%.4f,%.1f,%d\n", altBucket_sum, altBucket_n, altBucket_worst, altBucketP10(), altCurEngHour); break;
              case 6: st.len = snprintf(st.line, sizeof(st.line), "BUCKET,baseline_engNow_bucketSec,%.1f,%.1f,%.0f\n", altTrendBaselineSec, EngineRunTime_AllTime, altTrendBucketSec); break;
              case 7: st.len = snprintf(st.line, sizeof(st.line), "SESSION,mean_p10_n,%.1f,%.1f,%lu\n", alf_sessMean(), alf_sessP10(), (unsigned long)altSessN); break;
              case 8: st.len = snprintf(st.line, sizeof(st.line), "COUNT,myHist_upload_trend_myHistSrc_upSrc,%d,%d,%d,%d,%d\n", st.myN, st.upN, st.trN, (int)altFront2.source, (int)altFrontUp.source); break;
              case 9: {   // elapsed history (ms): both temperature gates (trailing, survive the barrier)
                          // and the episode's own data window (restarts at every barrier)
                uint32_t now = millis();
                uint32_t thermMs    = altThermGate.have    ? (now - altThermGate.startMs) : 0;
                uint32_t sessTempMs = altSessTempGate.have ? (now - altSessTempGate.startMs) : 0;
                uint32_t epDataMs   = altEpisode.haveData  ? (now - altEpisode.dataStartMs) : 0;
                st.len = snprintf(st.line, sizeof(st.line), "GATE,thermTrailMs_sessTempMs_epDataMs,%u,%u,%u\n",
                                  (unsigned)thermMs, (unsigned)sessTempMs, (unsigned)epDataMs);
                break;
              }
              case 10:   // per-axis steadiness at this instant — names which axis starves emits
                st.len = snprintf(st.line, sizeof(st.line), "GATE,axSteady_rpm_duty_vbus_temp_amps,%d,%d,%d,%d,%d\n",
                                  (int)altEpisode.axisSteady[0], (int)altEpisode.axisSteady[1], (int)altEpisode.axisSteady[2],
                                  (int)altEpisode.axisSteady[3], (int)altEpisode.axisSteady[ALT_NAXIS]);
                break;
              case 11: {  // emit starvation triage: total emits + how long since the last emit
                uint32_t now = millis();
                float emitAgo = altEpisode.lastEmitMs ? (float)((uint32_t)(now - altEpisode.lastEmitMs)) / 1000.0f : -1.0f;
                st.len = snprintf(st.line, sizeof(st.line), "GATE,emitCount_lastEmitAgoS,%d,%.1f\n",
                                  altFrontEmitCount, emitAgo);
                break;
              }
              case 12:   // boot-cumulative out-of-band ticks per axis (+ total feeds) — which band interrupts
                st.len = snprintf(st.line, sizeof(st.line), "GATE,axUnqual_rpm_duty_vbus_temp_amps_feeds,%lu,%lu,%lu,%lu,%lu,%lu\n",
                                  (unsigned long)altEpisode.axUnqual[0], (unsigned long)altEpisode.axUnqual[1],
                                  (unsigned long)altEpisode.axUnqual[2], (unsigned long)altEpisode.axUnqual[3],
                                  (unsigned long)altEpisode.axUnqual[ALT_NAXIS], (unsigned long)altEpisode.feedTicks);
                break;
              case 13:   // excursion starts per axis — with axUnqual gives mean excursion length (×0.1 s)
                st.len = snprintf(st.line, sizeof(st.line), "GATE,axExcur_rpm_duty_vbus_temp_amps,%lu,%lu,%lu,%lu,%lu\n",
                                  (unsigned long)altEpisode.axExcur[0], (unsigned long)altEpisode.axExcur[1],
                                  (unsigned long)altEpisode.axExcur[2], (unsigned long)altEpisode.axExcur[3],
                                  (unsigned long)altEpisode.axExcur[ALT_NAXIS]);
                break;
              case 14:   // live trimmed boxcar range per axis — compare directly to the PARAM band tols
                st.len = snprintf(st.line, sizeof(st.line), "GATE,boxTrimRange_rpm_duty_vbus_temp_amps,%.1f,%.3f,%.3f,%.2f,%.2f\n",
                                  altEpisode.trimmedRange(0), altEpisode.trimmedRange(1), altEpisode.trimmedRange(2),
                                  altEpisode.trimmedRange(3), altEpisode.trimmedRange(ALT_NAXIS));
                break;
              case 15:   // sole-blocker ticks — the one axis out-of-band while every other axis was steady
                st.len = snprintf(st.line, sizeof(st.line), "GATE,soleBlk_rpm_duty_vbus_temp_amps_ticks,%lu,%lu,%lu,%lu,%lu,%lu\n",
                                  (unsigned long)altEpisode.soleBlock[0], (unsigned long)altEpisode.soleBlock[1],
                                  (unsigned long)altEpisode.soleBlock[2], (unsigned long)altEpisode.soleBlock[3],
                                  (unsigned long)altEpisode.soleBlock[ALT_NAXIS], (unsigned long)altEpisode.blameTicks);
                break;
              case 16:   // candidate emits vetoed per axis by the trimmed boxcar range (+ span-short)
                st.len = snprintf(st.line, sizeof(st.line), "GATE,emitDirty_rpm_duty_vbus_temp_amps_span,%lu,%lu,%lu,%lu,%lu,%lu\n",
                                  (unsigned long)altEpisode.emitDirty[0], (unsigned long)altEpisode.emitDirty[1],
                                  (unsigned long)altEpisode.emitDirty[2], (unsigned long)altEpisode.emitDirty[3],
                                  (unsigned long)altEpisode.emitDirty[ALT_NAXIS], (unsigned long)altEpisode.emitSpanShort);
                break;
              case 17:   // live straightness fits — rms departure from the line, and the line's slope per second.
                         // Compare rms to the PARAM line limits and slope to the rate/slew caps.
                st.len = snprintf(st.line, sizeof(st.line), "GATE,lineRms_rpm_duty_amps_slope_rpm_duty_amps,%.2f,%.3f,%.3f,%.1f,%.3f,%.3f\n",
                                  altFitRpm.rms, altFitDuty.rms, altFitAmps.rms,
                                  altFitRpm.slope, altFitDuty.slope, altFitAmps.slope);
                break;
              default: st.phase = 3; st.idx = 0; break;
            }
            if (st.len > 0) st.idx++;
          } else if (st.phase == 3) {                            // My History front points (x0..x3,y,nSamp,tEmit,dutyPct,tSlopeFmin)
            if (st.idx < st.myN) {
              FrontPoint<ALT_NAXIS> &p = altFrontBuf[st.idx];
              st.len = snprintf(st.line, sizeof(st.line), "MYHIST,%d,%.0f,%.3f,%.2f,%.1f,%.2f,%u,%u,%.1f,%.2f\n", st.idx, p.x[0], p.x[1], p.x[2], p.x[3], p.y, (unsigned)p.nSamp, (unsigned)p.tEmit, p.ex[0], p.ex[1]);
              st.idx++;
            } else { st.phase = 4; st.idx = 0; }
          } else if (st.phase == 4) {                            // Uploaded front points (x0..x3,y,nSamp,tEmit)
            if (st.idx < st.upN) {
              FrontPoint<ALT_NAXIS> &p = altFrontUpBuf[st.idx];
              st.len = snprintf(st.line, sizeof(st.line), "UPLOAD,%d,%.0f,%.3f,%.2f,%.1f,%.2f,%u,%u\n", st.idx, p.x[0], p.x[1], p.x[2], p.x[3], p.y, (unsigned)p.nSamp, (unsigned)p.tEmit);
              st.idx++;
            } else { st.phase = 5; st.idx = 0; }
          } else if (st.phase == 5) {                            // committed trend buckets
            if (st.idx < st.trN) {
              AltTrendPt &p = altTrend[st.idx];
              st.len = snprintf(st.line, sizeof(st.line), "TREND,%u,%.1f,%.1f\n", (unsigned)p.engHour, p.worstPct / 10.0f, p.overallPct / 10.0f);
              st.idx++;
            } else { st.phase = 6; st.idx = 0; }
          } else {                                               // phase 6: session % histogram (60 × 2%-wide bins; key = bin low %)
            if (st.idx < 60) {
              if (altSessHist[st.idx] > 0)
                st.len = snprintf(st.line, sizeof(st.line), "SESSHIST,%d,%u\n", st.idx * 2, (unsigned)altSessHist[st.idx]);
              st.idx++;
            } else { st.done = true; return written; }
          }
          if (st.len <= 0) continue;                             // phase transition produced no line → loop to next phase
          if (st.len > (int)sizeof(st.line) - 1) st.len = sizeof(st.line) - 1;
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
}

// Parse a BEFRONT1 CSV into a TARGET surface, replacing it: false -> My History (cloud sync-back), true ->
// Uploaded (browser Load CSV). A bool, not a FrontStore<> reference, because Arduino's auto-prototype
// generator can't parse template types in signatures.
static bool altIngestFrontCsvInto(char *body, bool toUploaded) {
  FrontStore<ALT_NAXIS> &fs  = toUploaded ? altFrontUp : altFront2;
  FrontPoint<ALT_NAXIS> *buf = toUploaded ? altFrontUpBuf : altFrontBuf;
  if (!buf) return false;
  char *p = strstr(body, "BEFRONT1");
  if (!p) return false;
  char *nl = strchr(p, '\n');
  if (!nl) return false;
  uint8_t newSource = 0;
  { char saved = *nl; *nl = '\0';                       // header: BEFRONT1,sys,naxis,source,units…
    char *t = strtok(p, ",");                           // BEFRONT1
    t = strtok(NULL, ",");                              // sys — must be ALT: a boat-polar BEFRONT1 (SAIL/MOTOR)
    if (!t || strcmp(t, "ALT") != 0) {                  // parses column-shifted into garbage alt points otherwise
      *nl = saved;
      queueConsoleMessageF("AltFront: rejected import — header sys '%s' is not ALT", t ? t : "?");
      return false;
    }
    t = strtok(NULL, ",");                              // naxis
    if (!t || atoi(t) != ALT_NAXIS) {
      *nl = saved;
      queueConsoleMessageF("AltFront: rejected import — %d axes, expected %d", t ? atoi(t) : 0, ALT_NAXIS);
      return false;
    }
    t = strtok(NULL, ",");                              // source
    if (t) newSource = (uint8_t)atoi(t);
    *nl = saved; }
  frontWriteBegin();
  int newCount = 0;
  char *line = nl + 1;
  while (line && *line && newCount < ALT_FRONT_CAP) {
    char *eol = strchr(line, '\n');
    if (eol) *eol = '\0';
    float x0, x1, x2, x3, y; unsigned ns = 0, te = 0;
    // The block ends at the first line that is not a row, so a later bare-numeric section is never read as points
    // (same rule as altFrontBlockOnly in script.js).
    if (sscanf(line, "%f,%f,%f,%f,%f,%u,%u", &x0, &x1, &x2, &x3, &y, &ns, &te) < 5) break;
    FrontPoint<ALT_NAXIS> q;
    q.x[0] = x0; q.x[1] = x1; q.x[2] = x2; q.x[3] = x3; q.y = y;
    q.ex[0] = 0; q.ex[1] = 0;   // raw extras live in the cloud table, not the front CSV
    q.nSamp = (uint32_t)ns; q.tEmit = (uint32_t)te;
    buf[newCount++] = q;
    if (!eol) break;
    line = eol + 1;
  }
  if (line && *line && newCount >= ALT_FRONT_CAP)   // source sent more points than we can hold
    queueConsoleMessageF("WARN: alt front truncated at %d pts — raise ALT_FRONT_CAP", ALT_FRONT_CAP);
  fs.count = newCount;
  fs.source = newSource;
  frontWriteEnd();
  return true;
}
// Back-compat: cloud sync-back replaces My History (the learned surface the cloud prunes).
bool altIngestFrontCsv(char *body) { return altIngestFrontCsvInto(body, false); }

// Append-only engine-hour trend log: 8-byte {magic,ver} header + AltTrendPt records. Each field-off
// appends only the newly committed buckets (~6 B/hour) instead of rewriting the whole ~52 KB ring;
// a full rewrite happens only on a load-miss or ring eviction (altTrendRewrite).
static void altTrendPersist() {
  if (!altTrend) return;
  bool full = altTrendRewrite || altTrendFlushed > (uint32_t)altTrendCount || !fsExists("/alttrend.bin");
  fsTakeLock();
  if (full) {
    File f = LittleFS.open("/alttrend.bin", "w");
    if (f) {
      uint32_t hdr[2] = { ALT_TRENDLOG_MAGIC, ALT_VER };
      f.write((const uint8_t *)hdr, sizeof(hdr));
      if (altTrendCount > 0) f.write((const uint8_t *)altTrend, (size_t)altTrendCount * sizeof(AltTrendPt));
      f.close();
      altTrendFlushed = altTrendCount; altTrendRewrite = false;
    }
  } else if ((uint32_t)altTrendCount > altTrendFlushed) {   // append only the new buckets
    File f = LittleFS.open("/alttrend.bin", "a");
    if (f) {
      f.write((const uint8_t *)(altTrend + altTrendFlushed),
              (size_t)((uint32_t)altTrendCount - altTrendFlushed) * sizeof(AltTrendPt));
      f.close();
      altTrendFlushed = altTrendCount;
    }
  }
  fsFreeDirty = true;
  fsReleaseLock();
}
static void altTrendLoad() {
  altTrendCount = 0; altTrendFlushed = 0; altTrendRewrite = true;   // default: log missing/invalid → rewrite next save
  if (!altTrend || !fsExists("/alttrend.bin")) return;
  fsTakeLock();
  File f = LittleFS.open("/alttrend.bin", "r");
  if (f) {
    uint32_t hdr[2] = { 0, 0 };
    size_t sz = f.size();
    if (sz >= sizeof(hdr) && f.read((uint8_t *)hdr, sizeof(hdr)) == sizeof(hdr)
        && hdr[0] == ALT_TRENDLOG_MAGIC && hdr[1] == ALT_VER) {
      uint32_t n = (sz - sizeof(hdr)) / sizeof(AltTrendPt);
      if (n > (uint32_t)ALT_TREND_CAP) {                  // keep most recent CAP (far-future overflow)
        f.seek(sizeof(hdr) + (size_t)(n - ALT_TREND_CAP) * sizeof(AltTrendPt));
        n = ALT_TREND_CAP;
      }
      size_t got = f.read((uint8_t *)altTrend, (size_t)n * sizeof(AltTrendPt));
      altTrendCount = (int)(got / sizeof(AltTrendPt));
      altTrendFlushed = altTrendCount; altTrendRewrite = false;
    }
    f.close();
  }
  fsReleaseLock();
}

// ---- persistence (field-off-gated by caller) — front + trend survive reboot (cloud authoritative) ----
void altHealthSave() {
  if (dbgRingsSynthetic) return;   // fillmax/clearmax: RAM rings are synthetic/empty — keep the real flash blobs
  if (importRebootPending) return; // a records import just wrote this record's keys; the reboot loads them
  if (!altFrontBuf || hardwarePresent != 1 || demoSeal) return;   // demo mode: the stores in RAM are the demo copies
  uint32_t uw = ((uint32_t)altFront2.source << 8) | (uint32_t)ALT_NAXIS;   // stash source + naxis
  writePsramBlob("/altfront.bin", ALT_FRONT_MAGIC, ALT_VER, uw, altFrontBuf, sizeof(FrontPoint<ALT_NAXIS>), ALT_FRONT_CAP, 0, altFront2.count);
  // Unsynced points too: else a reboot loses them and the next sync-back erases them from My History.
  if (altPending) writePsramBlob("/altpend.bin", ALT_PEND_MAGIC, ALT_VER, 0, altPending, sizeof(FrontPoint<ALT_NAXIS>), ALT_PENDING_CAP, 0, altPendingCount);
  if (altHaveUpload && altFrontUpBuf)                                       // Uploaded surface (separate file)
    writePsramBlob("/altfrontup.bin", ALT_FRONT_MAGIC, ALT_VER, (uint32_t)ALT_NAXIS, altFrontUpBuf, sizeof(FrontPoint<ALT_NAXIS>), ALT_FRONT_CAP, 0, altFrontUp.count);
  altTrendPersist();                                                       // append-only committed-bucket log
  altPersistTrendBucket();                                                 // partial in-progress bucket (4 scalars)
  settingWrite(NK_altbaseSec, String(altTrendBaselineSec, 1).c_str());   // trend X-axis origin
  settingWrite(NK_altRefSrc, String((int)altRefSource).c_str());          // active reference source
}
static void altLoad() {
  uint32_t uw = 0;
  if (altFrontBuf) {
    altFront2.count = (int)readPsramBlob("/altfront.bin", ALT_FRONT_MAGIC, ALT_VER, altFrontBuf, sizeof(FrontPoint<ALT_NAXIS>), ALT_FRONT_CAP, &uw, false);
    altFront2.source = (uint8_t)((uw >> 8) & 0xFF);
  }
  if (altPending && fsExists("/altpend.bin"))
    altPendingCount = (int)readPsramBlob("/altpend.bin", ALT_PEND_MAGIC, ALT_VER, altPending, sizeof(FrontPoint<ALT_NAXIS>), ALT_PENDING_CAP, nullptr, false);
  if (altFrontUpBuf && fsExists("/altfrontup.bin")) {                       // restore the Uploaded surface if present
    uint32_t uw2 = 0;
    altFrontUp.count = (int)readPsramBlob("/altfrontup.bin", ALT_FRONT_MAGIC, ALT_VER, altFrontUpBuf, sizeof(FrontPoint<ALT_NAXIS>), ALT_FRONT_CAP, &uw2, false);
    altFrontUp.source = 1;
    altHaveUpload = (altFrontUp.count > 0);
  }
  altTrendLoad();                                                          // committed buckets
  altRestoreTrendBucket();                                                 // partial in-progress bucket
  if (settingExists(NK_altbaseSec)) altTrendBaselineSec = settingRead(NK_altbaseSec).toFloat();
  if (settingExists(NK_altRefSrc)) altRefSource = (uint8_t)settingRead(NK_altRefSrc).toInt();
  if (altRefSource == 1 && !altHaveUpload) altRefSource = 0;               // no uploaded surface → fall back to My History
}

// Load CSV from the browser into the UPLOADED surface, alongside My History (never overwriting it, spec §4).
// Switches Reference Source to Uploaded; fixed=true -> Pause ON, false -> keep learning My History. Persists
// immediately. Mutates body.
bool altUploadFrontCsv(char *body, bool fixed) {
  if (!altFrontUpBuf || !body) return false;
  bool ok = altIngestFrontCsvInto(body, true);
  if (!ok) return false;
  altFrontUp.source = 1;                        // borrowed surface
  altHaveUpload = true;
  altRefSource = 1;                             // grade session + trend against the uploaded surface
  altLatchPct = 0;                              // the latched % was graded against the old reference
  altPaused = fixed ? 1.0f : 0.0f;              // Uploaded defaults Pause ON; user may keep learning My History
  settingWrite(NK_altPaused, fixed ? "1.0000" : "0.0000");
  settingWrite(NK_altRefSrc, "1");
  queueConsoleMessageF("AltFront: UPLOADED %d pts to Uploaded surface (%s); My History kept",
                       altFrontUp.count, fixed ? "Pause ON" : "still learning My History");
  // Persist even field-on: the brief flash-write control stall is accepted for user actions.
  altHealthSave();
  return true;
}

// ---- cloud upload: batch of accepted episode points since the last upload (raw out; pruned front in) ----
// Schema: {device_uid,token,ts,sys,pruneK,idwPower, pts:[[rpm,exc,vbus,tF,amps,nSamp], ...]}.
// The batch's prefix leaves pending on a successful response (executeUploadAltHealth → altHandSync → altApplySync on Core 1).
bool buildAltHealthPayload(char *buf, size_t size) {
  if (!altPending || altPendingCount == 0 || authToken.isEmpty()) return false;
  if (altSyncHandoff) return false;   // last sync not applied yet: its sent prefix is still in the queue
  time_t now_ts = time(NULL);
  int off = snprintf(buf, size,
    "{\"device_uid\":\"%s\",\"token\":\"%s\",\"ts\":\"%s\",\"sys\":\"ALT\",\"pruneK\":%d,\"idwPower\":%.2f,\"sysV\":%u,",
    device_id_hex, authToken.c_str(), formatTimestamp(now_ts), (int)altPruneK, altIdwPower, (unsigned)SYSTEM_VOLTAGE_CLASS);
  if (off < 0 || (size_t)off >= size) return false;
  if (altPendingSeededFrom.length()) {   // seeded curve: tag the whole batch with its provenance label
    off += snprintf(buf + off, size - off, "\"seededFrom\":\"%s\",", altPendingSeededFrom.c_str());
    if (off < 0 || (size_t)off >= size) return false;
  }
  off += snprintf(buf + off, size - off, "\"pts\":[");
  bool first = true;
  int k = 0;
  for (; k < altPendingCount; k++) {
    if (size - (size_t)off < 80) break;   // margin for the closer
    FrontPoint<ALT_NAXIS> &p = altPending[k];
    off += snprintf(buf + off, size - off, "%s[%.0f,%.3f,%.2f,%.1f,%.2f,%u,%.1f]",
                    first ? "" : ",", p.x[0], p.x[1], p.x[2], p.x[3], p.y, p.nSamp, p.ex[0]);  // ex[0]=raw duty
    first = false;
  }
  off += snprintf(buf + off, size - off, "]}");
  if (off < 0 || (size_t)off >= size - 1) return false;
  altSentCount = k;
  altSentSeeded = (altPendingSeededFrom.length() > 0);
  return true;
}
// The upload block carries this batch out and the cloud's front back (executeUploadAltHealth reads the reply into it), so
// it is sized for the larger: the JSON at the builder's 80 B per-point margin, or the reply at its widest row. The reply is
// the cloud's pruned front, close to My History plus this batch; a bigger one grows the block up to ALT_UPLOAD_BUF_SIZE.
// 0 = nothing to send (no allocation).
#define ALT_CSV_ROW_MAX 56   // widest update-alt-health front row: rpm, exc 3dp, V 2dp, F 1dp, amps 2dp, nSamp, 0
size_t altSyncBufBytes() {
  if (!altPending || altPendingCount == 0 || authToken.isEmpty()) return 0;
  size_t out = 512 + authToken.length() + altPendingSeededFrom.length() + ((size_t)altPendingCount + 2) * 80;
  size_t rows = (size_t)altFront2.count + (size_t)altPendingCount;
  size_t back = 128 + (rows + rows / 8 + 32) * ALT_CSV_ROW_MAX;
  size_t n = (out > back) ? out : back;
  return (n < (size_t)ALT_UPLOAD_BUF_SIZE) ? n : (size_t)ALT_UPLOAD_BUF_SIZE;
}

// ---- lifecycle ----
void initAlternatorHealth() {
  altTrend = (AltTrendPt *)ps_malloc((size_t)ALT_TREND_CAP * sizeof(AltTrendPt));
  if (!altTrend) { queueConsoleMessage("ERROR: AltHealth trend ps_malloc failed"); return; }
  memset(altTrend, 0, (size_t)ALT_TREND_CAP * sizeof(AltTrendPt));
  altTrendCount = 0; altCurEngHour = -1;
  altFrontInit();          // ps_malloc front + episode ring + pending; init the engine instance
  altLoad();               // restore front + trend + baseline (after the buffers exist)
  if (recSeedOwed(1 | 4)) {    // an imported (1) or pre-registration (4) My History the cloud does not hold yet
    if (altPending && altFront2.count > 0) {
      altPendingCount = 0;
      for (int i = 0; i < altFront2.count && altPendingCount < ALT_PENDING_CAP; i++) altPending[altPendingCount++] = altFrontBuf[i];
      altPendingSeededFrom = recSeedOwed(1) ? "import" : "reregister";
    } else if (altPending) {
      recSeedOwedSet(1 | 4, false);
    }
  }
  queueConsoleMessageF("AltHealth init: My History %d pts, Uploaded %d pts, source=%s, %d trend pts",
                       altFront2.count, altFrontUp.count, altRefSource ? "Uploaded" : "MyHistory", altTrendCount);
}
void resetAlternatorHealth() {
  // Start Over follows alternator/regulator/belt replacement, which also invalidates learned hunt pockets.
  // Runs before the fs lock is taken below (huntMapClearAll takes it too).
  huntMapClearAll("alternator replaced / health record reset");
  altHealthRecordClear("Reset / Start Over");
}
// The health record itself, local and cloud. The Uploaded surface stays: it was scaled by its own boat's winding.
void altHealthRecordClear(const char *why) {
  if (!altFrontBuf) return;
  frontWriteBegin(); altFront2.count = 0; altFront2.source = 0; frontWriteEnd();
  altPendingCount = 0; altSentCount = 0; altFrontEmitCount = 0; altEmitQCount = 0; altCapWarned = false;
  altEpisode.clearRun(); altEpisode.ringHead = 0; altEpisode.ringCount = 0;
  altTrendCount = 0; altTrendFlushed = 0; altTrendRewrite = true;   // /alttrend.bin removed below → fresh log
  altBucket_sum = 0; altBucket_n = 0; altBucket_worst = 0; altCurEngHour = -1;
  memset(altBucketHist, 0, sizeof(altBucketHist));
  altPersistTrendBucket();                       // overwrite the persisted partial bucket with the cleared one
  altWorstPctLive = 0; altOverallPctLive = 0; altStatusCode = 0; altLive_pct = 0; altLatchPct = 0;
  altState = FRONT_NO_REFERENCE; altHiFieldAlert = false;
  altSteady = false; altSessSteady = false; altThermGate.clear(); altSessTempGate.clear();
  altRefSource = 0;                              // revert active reference to My History (Uploaded surface kept, just inactive)
  memset(altSessHist, 0, sizeof(altSessHist)); altSessN = 0; altSessSum = 0;   // session stats restart with the data
  altSessRingHead = 0; altSessRingCount = 0; altSessRingLastMs = 0;              // and the served session series
  altTrendBaselineSec = EngineRunTime_AllTime;   // new baseline → trend X-axis restarts at 0
  fsTakeLock();
  LittleFS.remove("/altfront.bin");
  LittleFS.remove("/alttrend.bin");
  LittleFS.remove("/altpend.bin");
  fsReleaseLock();
  settingWrite(NK_altbaseSec, String(altTrendBaselineSec, 1).c_str());
  settingWrite(NK_altRefSrc, "0");
  // Cloud half: the cloud rebuilds the front from every raw point, so without this the next sync-back restores
  // the cleared record. Persisted so an offline reset still lands; executeResetAltHealthCloud retries until 200.
  altCloudWipePending = true;
  settingWrite(NK_AltWipePend, "1");
  queueConsoleMessageF("AltHealth: full reset (%s) — My History + trend + session cleared, baseline restarted", why);
}

// Excitation is divided by fieldWindApplied, so points recorded under another winding sit on another scale.
// altHealthWindV is the winding the record was built on; seeded after InitSystemSettings, imported only with the record.
void altHealthWindingInit() {
  const int w = settingExists(NK_altWindV) ? settingRead(NK_altWindV).toInt() : 0;
  altHealthWindV = (w == 12 || w == 24 || w == 36 || w == 48) ? (uint8_t)w : fieldWindApplied;
  settingWrite(NK_altWindV, String((int)altHealthWindV).c_str());
}
// Loop-side, so every move of fieldWindApplied is caught: a winding or resistance change, the owner's answer to the
// resistance check, the class clamp, and an import (at the next boot).
void altHealthWindingCheck() {
  if (demoSeal) return;   // demo mode: a winding changed in demo reverts at exit, so the record must not be cleared for it
  if (altHealthWindV == 0 || altHealthWindV == fieldWindApplied) return;
  if (importRebootPending) return;   // a records import just wrote the record's keys; the boot after it judges the winding
  char why[48];
  snprintf(why, sizeof(why), "field winding values %uV -> %uV", (unsigned)altHealthWindV, (unsigned)fieldWindApplied);
  altHealthWindV = fieldWindApplied;
  settingWrite(NK_altWindV, String((int)altHealthWindV).c_str());
  altHealthRecordClear(why);
}

// ---- engine allocation (PSRAM): front points + episode reseed ring + pending-upload buffer ----
void altFrontInit() {
  altEpRing     = (RawSample<ALT_NAXIS>  *)ps_malloc((size_t)ALT_EP_RING_CAP * sizeof(RawSample<ALT_NAXIS>));
  altFrontBuf   = (FrontPoint<ALT_NAXIS> *)ps_malloc((size_t)ALT_FRONT_CAP   * sizeof(FrontPoint<ALT_NAXIS>));
  altFrontUpBuf = (FrontPoint<ALT_NAXIS> *)ps_malloc((size_t)ALT_FRONT_CAP   * sizeof(FrontPoint<ALT_NAXIS>));
  altPending  = (FrontPoint<ALT_NAXIS> *)ps_malloc((size_t)ALT_PENDING_CAP * sizeof(FrontPoint<ALT_NAXIS>));
  altSessRing = (AltSessPt *)ps_malloc((size_t)ALT_SESS_RING_N * sizeof(AltSessPt));   // session series (~30 KB)
  if (altSessRing) memset(altSessRing, 0, (size_t)ALT_SESS_RING_N * sizeof(AltSessPt));
  else queueConsoleMessage("AltHealth: session-series ps_malloc failed — /altsess.csv empty, plots stay browser-only");
  altJob = (AltJob *)ps_malloc(sizeof(AltJob));   // null leaves the 1 Hz evaluator inline (altTickInline)
  if (altJob) memset(altJob, 0, sizeof(AltJob));
  if (!altEpRing || !altFrontBuf || !altFrontUpBuf || !altPending) { queueConsoleMessage("ERROR: AltFront ps_malloc failed"); return; }
  memset(altEpRing,     0, (size_t)ALT_EP_RING_CAP * sizeof(RawSample<ALT_NAXIS>));
  memset(altFrontBuf,   0, (size_t)ALT_FRONT_CAP   * sizeof(FrontPoint<ALT_NAXIS>));
  memset(altFrontUpBuf, 0, (size_t)ALT_FRONT_CAP   * sizeof(FrontPoint<ALT_NAXIS>));
  memset(altPending,  0, (size_t)ALT_PENDING_CAP * sizeof(FrontPoint<ALT_NAXIS>));
  altPendingCount = 0;
  // Axis modes are structural: set BEFORE init(), which allocates deques only for band axes. Temperature's
  // verdict comes from altThermGate (its window must outlive the eligibility barrier). RPM and output amps
  // skip the emit-time trimmed-range test, which would re-impose stillness (26 vs 209 points on THEDRIVE replay).
  altEpisode.gateMode[0] = EP_STRAIGHT;  altEpisode.fit[0] = &altFitRpm;
  altEpisode.gateMode[1] = EP_STRAIGHT;  altEpisode.fit[1] = &altFitDuty;
  altEpisode.gateMode[2] = EP_BAND;      altEpisode.lead[2] = &altLeadVbus;
  altEpisode.gateMode[3] = EP_EXTERNAL;
  altEpisode.gateMode[ALT_NAXIS] = EP_STRAIGHT;
  altEpisode.fit[ALT_NAXIS] = &altFitAmps;
  altEpisode.lead[ALT_NAXIS] = &altLeadAmps;
  altEpisode.emitTrimSkip[0] = true;
  altEpisode.emitTrimSkip[ALT_NAXIS] = true;
  // Per-axis deque window caps (max steady time the axis can be set to): only the Vbus axis still
  // holds a deque, but the array keeps one entry per axis so the indices stay readable.
  static const float ALT_MAXDWELL[ALT_NAXIS + 1] = { 30.0f, 30.0f, 30.0f, 240.0f, 30.0f };
  altEpisode.init(altEpRing, ALT_EP_RING_CAP, ALT_MAXDWELL);
  altEpisode.avgWinMs = ALT_EMIT_AVG_MS;   // sail/motor keep the 2 s default; alt records are sustained averages
  altEpisodeSyncCfg(0.0f);   // per-axis limits + the lead delay; resynced every fold so live edits land
  altThermGate.init(2600);      // full-dwell temperature gate, ~240 s of 10 Hz headroom
  altSessTempGate.init(2600);   // session-temp (half-dwell) gate, same headroom
  altFront2.init(altFrontBuf, ALT_FRONT_CAP);
  altFrontUp.init(altFrontUpBuf, ALT_FRONT_CAP);
  // axisScale ~ the span of each axis that moves output a comparable amount (ALT_HEALTH_LWLR_ENGINE_SPEC.md).
  // MUST match AXIS_SCALE in the update-alt-health edge fn (Vbus class-aware there via payload sysV;
  // excitation is 12 V-winding-equivalent so it needs no scaling).
  altFront2.axisScale[0] = 25.0f;   // RPM
  altFront2.axisScale[1] = 0.10f;   // excitation (temp-normalized field volts); 0.2 cells were coarser than the amps
                                    // band, so in-cell records differed by commanded field alone
  altFront2.axisScale[2] = 0.1f;    // Vbus at 12V — class-corrected by altApplyClassScales() once SYSTEM_VOLTAGE_CLASS is loaded
  altFront2.axisScale[3] = 10.0f;   // tempF; <=0.32%/F apparent slope keeps 5F (half a cell) <=1.6% output, inside the amps band
  for (int a = 0; a < ALT_NAXIS; a++) altFrontUp.axisScale[a] = altFront2.axisScale[a];   // same metric for the borrowed surface
  queueConsoleMessageF("AltFront init: cap %d pts, ring %d, %.1fKB PSRAM",
    ALT_FRONT_CAP, ALT_EP_RING_CAP,
    (float)((size_t)ALT_EP_RING_CAP * sizeof(RawSample<ALT_NAXIS>) +
            (size_t)(ALT_FRONT_CAP + ALT_PENDING_CAP) * sizeof(FrontPoint<ALT_NAXIS>)) / 1024.0f);
}

// Vbus cell size per-cell-equivalent across classes (0.4V @48V = 0.1V @12V). Separate from altFrontInit, which
// runs BEFORE InitSystemSettings loads SYSTEM_VOLTAGE_CLASS; also re-run on a live class change. Interpretation
// only (stored points keep raw coords); the update-alt-health edge fn mirrors it via payload sysV.
void altApplyClassScales() {
  frontWriteBegin();   // callers include the web task
  altFront2.axisScale[2] = 0.1f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f);
  altFrontUp.axisScale[2] = altFront2.axisScale[2];
  frontWriteEnd();
}

// One entry per ALT_SESS_DT_MS while the alternator is live, graded or not (state 2 = the dashboard's grey gap).
// Nothing is pushed while the field is down, so an overnight stop is a time jump, not gap rows evicting real data.
// Cost: one 21-byte PSRAM store every 5 s inside the existing 1 Hz evaluator — never in the fold.
// in[] = the fold outputs at the grading tick {amps, rpm, duty, vbus, tF, tSlope}; steady = altSteady then.
static void altSessSeriesPush(uint32_t nowMs, bool graded, const float *in, bool steady) {
  if (!altSessRing) return;
  if (altSessRingLastMs && (uint32_t)(nowMs - altSessRingLastMs) < ALT_SESS_DT_MS) return;
  altSessRingLastMs = nowMs;
  AltSessPt &p = altSessRing[altSessRingHead];
  if (!graded && altSimMode >= 0.5f && altSessLastGraded.pred > 0) {
    // Demo: the plot stays populated with no grade (field off, engine stopped): the last graded conditions at the held grade
    float pct = constrain(altLatchPct + 0.25f * demoWander((float)nowMs / 30000.0f, 414), 97.65f, 99.35f);
    bool est = demoHash01(nowMs / ALT_SESS_DT_MS) < 0.12f;
    p = altSessLastGraded;
    p.tMs = nowMs;
    p.flags = est ? 1 : 0x04;
    p.gAmps = altQU16(altSessLastGraded.pred / 100.0f * (est ? pct - 0.5f : pct), 1.0f);
  } else {
    p.tMs    = nowMs;
    p.flags  = graded ? (uint8_t)((altState == FRONT_ESTIMATED) ? 1 : 0) : 2;
    if (graded && (steady || (altSimMode >= 0.5f && altState == FRONT_MEASURED))) p.flags |= 0x04;
    p.gAmps  = altQU16(altLive_gAmps, 100.0f);
    p.measA  = altQU16(in[0], 100.0f);
    p.pred   = altQU16(altLive_pred,  100.0f);
    p.rpm    = altQU16(in[1],   1.0f);
    p.duty   = altQU16(in[2], 100.0f);
    p.vbus   = altQU16(in[3], 100.0f);
    p.tF     = altQI16(in[4],  10.0f);
    p.tSlope = altQI16(in[5], 100.0f);
    if (graded) altSessLastGraded = p;
  }
  altSessRingHead = (altSessRingHead + 1) % ALT_SESS_RING_N;
  if (altSessRingCount < ALT_SESS_RING_N) altSessRingCount++;
}

// High-field-low-output alert — independent of the record book (covers degradation pushed into
// unlearned territory where the gauge says "learning"). Self-clears when either condition lifts.
static void altHiFieldTick(uint32_t nowMs, bool foldFresh) {
  static uint32_t hiFieldSinceMs = 0;
  // altHiFieldPct is % of altFieldCeiling (lower of Max Field % and the Max Field Volts term), not absolute duty:
  // 80% absolute is unreachable on 24/36/48V banks with a 12 V winding. Never ccDutyCeiling(): a hot regulator
  // holding the field down must not read as a weak alternator.
  bool cond = foldFresh && altLiveValid
              && altLive_duty >= altHiFieldPct * (altFieldCeiling() / 100.0f) && altLive_amps <= altLowOutAmps;
  if (!cond) {
    hiFieldSinceMs = 0; altHiFieldAlert = false;
  } else {
    if (hiFieldSinceMs == 0) hiFieldSinceMs = nowMs;
    if (!altHiFieldAlert && (uint32_t)(nowMs - hiFieldSinceMs) >= (uint32_t)(altHiFieldSec * 1000.0f)) {
      altHiFieldAlert = true;
      queueConsoleMessageF("ALERT: Low output despite high field drive (duty %.0f%%, only %.1f A for %.0f s) — check alternator, belt, wiring",
                           altLive_duty, altLive_amps, altHiFieldSec);
    }
  }
}

static uint8_t altSettCtr = 0;   // resend settings ~every 5s so reconnects get echoes

// The whole 1 Hz evaluator in one pass: demo mode (its grades come from the simulated alternator, no record-book
// lookups) and a unit whose job memory failed to allocate.
static void altTickInline(uint32_t nowMs) {
  altProcessEmits();                  // grade + admit queued steady runs FIRST (front fresh for the live classify)
  altStatusCode = (IgnoreTemperature || IgnoreRPM) ? 3 : altLearnArmed() ? 0 : (commissionState == 2 ? ALT_STATUS_WAIT_CHC : ALT_STATUS_WAIT_CX);
  // 1 Hz evaluator + OUTPUT-BLIND state classifier. A stale fold (field off) → nothing to grade.
  bool foldFresh = (altLastFoldMs != 0) && ((uint32_t)(nowMs - altLastFoldMs) < 3000u);
  if (!foldFresh) altLiveValid = false;
  if (foldFresh && altLiveValid) {
    // Grade the SAME statistic the records store (trailing-boxcar averages): a fast snapshot vs a best-ever boxcar
    // reads ~95% on a healthy machine. Empty ring -> fall back to the fast EMA point.
    float surf[ALT_NAXIS] = { altLive_rpm, altLive_exc, altLive_vbus, altLive_tF };
    altLive_gAmps = altLive_amps;
    if (altEpisode.ringCount > 0) {
      double rc = (double)altEpisode.ringCount;
      surf[0] = (float)(altEpisode.avgSumX[0] / rc);                      // RPM
      surf[2] = (float)(altEpisode.avgSumX[2] / rc);                      // Vbus
      surf[3] = (float)(altEpisode.avgSumX[3] / rc);                      // tempF
      surf[1] = altExcitation((float)(altEpisode.avgSumX[1] / rc), surf[2], surf[3]);   // exc, same derivation as emits
      altLive_gAmps = (float)(altEpisode.avgSumOut / rc);
    }
    // Grade against the ACTIVE reference surface (My History or Uploaded). The trend uses the same
    // surface (in altProcessEmits) — session % and trend are graded identically (spec §1/§4.2).
    float pred = 0;
    if (altSimMode >= 0.5f) {                 // demo: graded against the simulated alternator's own best (9_demo.ino)
      altRefDist = 0;
      altState = demoAltGrade(surf, altLive_gAmps, nowMs, &pred);
    } else {
      FrontStore<ALT_NAXIS> &gf = altGradeFront();
      altRefDist = gf.nearestNormDist(surf);
      if (altRefDist > 999.0f) altRefDist = 999.0f;
      altState = (uint8_t)gf.classify(surf, altRefRadius, altIdwPower, altRidgeFrac, altRiskThresh, &pred);
    }
    altLive_pred = pred;
    altRefOk = (altState == FRONT_MEASURED || altState == FRONT_ESTIMATED);
    altLive_pct = (altRefOk && pred > 0.1f) ? (altLive_gAmps / pred * 100.0f) : 0.0f;
    if (altRefOk && altLive_pct > 0.0f) altLatchPct = altLive_pct;   // same gate as the Diag headline; only this survives an ungraded tick
    // Session stats = the SESSION-PLOTTED points: session-steady (lighter gate) AND graded. The trend is
    // NOT fed here — it is built purely from FULL-steady runs in altProcessEmits (spec §2). Demo: every graded tick.
    const bool sessGraded = (altSessSteady || altSimMode >= 0.5f) && altRefOk && altLive_pct > 0.0f;
    if (sessGraded) {
      altSessSum += altLive_pct; altSessN++;
      int bin = (int)(altLive_pct * 0.5f);    // 2%-wide histogram bins, 0..120%
      if (bin < 0) bin = 0;
      if (bin > 59) bin = 59;
      if (altSessHist[bin] < 65535) altSessHist[bin]++;
    }
    const float in[6] = { altLive_amps, altLive_rpm, altLive_duty, altLive_vbus, altLive_tF, altTempSlopeFMin };
    altSessSeriesPush(nowMs, sessGraded, in, altSteady);
  } else {
    altRefOk = false; altLive_pct = 0;        // nothing running → no grade (state holds its last value)
    if (altSimMode >= 0.5f) {                 // demo: the session plot keeps going (altSessSeriesPush)
      static const float none[6] = { 0, 0, 0, 0, 0, 0 };
      altSessSeriesPush(nowMs, false, none, false);
    }
  }
  altHiFieldTick(nowMs, foldFresh);
  altSendLive();
  if (++altSettCtr >= 5) { altSettCtr = 0; sendAltSettings(); }
}

static uint32_t frontDriveSeq = 0;              // /debug/frontcheck?drive= probe sequence

// /debug/frontcheck?drive=: synthetic lookups each second while the field stays cut (bench timing only).
static inline bool frontDriveOn() {
  if (!frontDriveUntilMs) return false;
  if (!gpio4IsLow || demoSeal || (int32_t)(millis() - frontDriveUntilMs) >= 0) { frontDriveUntilMs = 0; return false; }
  return true;
}

static void altJobItemAdd(uint8_t kind, uint8_t e, uint8_t uploaded, const float *x) {
  if (altJob->nItem >= ALTJ_ITEMS) return;
  AltJobItem &it = altJob->item[altJob->nItem++];
  it.kind = kind; it.emit = e; it.uploaded = uploaded;
  for (int a = 0; a < ALT_NAXIS; a++) it.x[a] = x[a];
}

static void altJobBegin(uint32_t nowMs) {
  AltJob &J = *altJob;
  J.active = true; J.stage = 0; J.tickMs = nowMs; J.gen = frontGenNow();
  J.workUs = 0; J.passes = 0; J.lookups = 0; J.nItem = 0; J.iItem = 0;
  J.q.phase = FrontQuery<ALT_NAXIS>::IDLE;
  J.trendEh = altTrendBucketIndex();
  J.trendFeedMs = (uint32_t)(altTrendFeedSec * 1000.0f);
  J.learn = (altPaused < 0.5f);
  J.gradeUp = (altRefSource == 1 && altHaveUpload) ? 1 : 0;
  J.refRadius = altRefRadius; J.idwPower = altIdwPower; J.ridgeFrac = altRidgeFrac;
  J.riskThresh = altRiskThresh; J.safetyMargin = altSafetyMargin;
  int nE = 0;                                   // altProcessEmits(): at most 2 per tick, none unless armed
  if (!altLearnArmed()) altEmitQCount = 0;
  else {
    nE = (altEmitQCount < 2) ? altEmitQCount : 2;
    for (int k = 0; k < nE; k++) J.emit[k] = altEmitQ[k].sp;
    if (altEmitQCount > nE) memmove(altEmitQ, altEmitQ + nE, (size_t)(altEmitQCount - nE) * sizeof(AltEmitPending));
    altEmitQCount -= nE;
  }
  altStatusCode = (IgnoreTemperature || IgnoreRPM) ? 3 : altLearnArmed() ? 0 : (commissionState == 2 ? ALT_STATUS_WAIT_CHC : ALT_STATUS_WAIT_CX);
  bool foldFresh = (altLastFoldMs != 0) && ((uint32_t)(nowMs - altLastFoldMs) < 3000u);
  if (!foldFresh) altLiveValid = false;
  J.liveWanted = foldFresh && altLiveValid;
  if (J.liveWanted) {                           // as altTickInline(): the boxcar averages, else the fast EMA point
    float *surf = J.liveSurf;
    surf[0] = altLive_rpm; surf[1] = altLive_exc; surf[2] = altLive_vbus; surf[3] = altLive_tF;
    J.liveGAmps = altLive_amps;
    if (altEpisode.ringCount > 0) {
      double rc = (double)altEpisode.ringCount;
      surf[0] = (float)(altEpisode.avgSumX[0] / rc);
      surf[2] = (float)(altEpisode.avgSumX[2] / rc);
      surf[3] = (float)(altEpisode.avgSumX[3] / rc);
      surf[1] = altExcitation((float)(altEpisode.avgSumX[1] / rc), surf[2], surf[3]);
      J.liveGAmps = (float)(altEpisode.avgSumOut / rc);
    }
    J.liveSessSteady = altSessSteady; J.liveSteady = altSteady;
    J.liveIn[0] = altLive_amps; J.liveIn[1] = altLive_rpm; J.liveIn[2] = altLive_duty;
    J.liveIn[3] = altLive_vbus; J.liveIn[4] = altLive_tF; J.liveIn[5] = altTempSlopeFMin;
  }
  altHiFieldTick(nowMs, foldFresh);
  for (int k = 0; k < nE; k++) {
    altJobItemAdd(ALTJ_GRADE, k, J.gradeUp, J.emit[k].x);
    if (J.learn && J.gradeUp) altJobItemAdd(ALTJ_ADMIT, k, 0, J.emit[k].x);
  }
  if (J.liveWanted) altJobItemAdd(ALTJ_LIVE, 0, J.gradeUp, J.liveSurf);
  if (frontDriveOn()) {                         // bench: a two-run second's lookups near stored records, results unused
    FrontStore<ALT_NAXIS> &gf = J.gradeUp ? altFrontUp : altFront2;
    float px[ALT_NAXIS];
    if (frontDriveOld) {                        // the inline tick's reads for the same second, all in this pass
      volatile float sink = 0;
      for (int d = 0; d < 2; d++) {
        FrontQuery<ALT_NAXIS>::probe(altFront2, frontDriveSeq++, 0.6f, px);
        float pr = 0, yr = 0, br = 0;
        sink = sink + (float)gf.classify(px, J.refRadius, J.idwPower, J.ridgeFrac, J.riskThresh, &pr) + pr;
        sink = sink + (float)altFront2.classify(px, J.refRadius, J.idwPower, J.ridgeFrac, J.riskThresh, &yr, &br) + br;
        sink = sink + (altFront2.hasLocalSupport(px) ? 1.0f : 0.0f) + (float)altFront2.cellOf(px);
      }
      FrontQuery<ALT_NAXIS>::probe(gf, frontDriveSeq++, 0.6f, px);
      float pr = 0;
      sink = sink + gf.nearestNormDist(px);
      sink = sink + (float)gf.classify(px, J.refRadius, J.idwPower, J.ridgeFrac, J.riskThresh, &pr) + pr;
      J.lookups += 10;                          // calls, each one or more passes over the store
    } else {
      for (int d = 0; d < 2; d++) {
        FrontQuery<ALT_NAXIS>::probe(altFront2, frontDriveSeq++, 0.6f, px);
        altJobItemAdd(ALTJ_DRY, 0, J.gradeUp, px);
        if (J.gradeUp) altJobItemAdd(ALTJ_DRY, 0, 0, px);
      }
      FrontQuery<ALT_NAXIS>::probe(gf, frontDriveSeq++, 0.6f, px);
      altJobItemAdd(ALTJ_DRY, 0, J.gradeUp, px);
    }
  }
}

// Admission into My History, as altProcessEmits(): cell-local gate against the lookup's bar, then add at its cell.
static void altJobAdmit(uint8_t e) {
  AltJob &J = *altJob;
  const FrontQuery<ALT_NAXIS> &q = J.q;
  const FrontPoint<ALT_NAXIS> &sp = J.emit[e];
  bool localSup = (q.cell >= 0);
  if (localSup && !(sp.y > q.bar - J.safetyMargin)) return;
  if (!localSup && altFront2.count >= ALT_FRONT_CAP) {
    if (!altCapWarned) {   // a genuinely NEW cell dropped at capacity (in-cell improvements still land)
      altCapWarned = true;
      queueConsoleMessage("WARN: alt front at capacity — new operating cells are no longer recorded");
    }
    return;
  }
  if (altFront2.addAt(sp, q.cell)) {
    if (altPending && altPendingCount < ALT_PENDING_CAP) altPending[altPendingCount++] = sp;
  }
}

// The live grade, as altTickInline() applies it.
static void altJobLive() {
  AltJob &J = *altJob;
  const FrontQuery<ALT_NAXIS> &q = J.q;
  altLive_gAmps = J.liveGAmps;
  altRefDist = q.nearest;
  if (altRefDist > 999.0f) altRefDist = 999.0f;
  altState = (uint8_t)q.state;
  float pred = q.pred;
  altLive_pred = pred;
  altRefOk = (altState == FRONT_MEASURED || altState == FRONT_ESTIMATED);
  altLive_pct = (altRefOk && pred > 0.1f) ? (altLive_gAmps / pred * 100.0f) : 0.0f;
  if (altRefOk && altLive_pct > 0.0f) altLatchPct = altLive_pct;
  const bool sessGraded = J.liveSessSteady && altRefOk && altLive_pct > 0.0f;
  if (sessGraded) {
    altSessSum += altLive_pct; altSessN++;
    int bin = (int)(altLive_pct * 0.5f);
    if (bin < 0) bin = 0;
    if (bin > 59) bin = 59;
    if (altSessHist[bin] < 65535) altSessHist[bin]++;
  }
  altSessSeriesPush(J.tickMs, sessGraded, J.liveIn, J.liveSteady);
}

static void altJobApply(uint8_t kind, uint8_t e) {
  AltJob &J = *altJob;
  if (kind == ALTJ_GRADE) {                     // altProcessEmits() (1) TREND FEED
    const FrontPoint<ALT_NAXIS> &sp = J.emit[e];
    bool graded = (J.q.state == FRONT_MEASURED) && J.q.pred > 0.1f;
    if (graded && (altLastTrendFeedMs == 0 || (uint32_t)(J.tickMs - altLastTrendFeedMs) >= J.trendFeedMs)) {
      altLastTrendFeedMs = J.tickMs;
      altTrendAdd((sp.y / J.q.pred), J.trendEh);
    }
    if (J.learn && !J.gradeUp) altJobAdmit(e);  // My History is the active surface: this lookup is the admission lookup too
  } else if (kind == ALTJ_ADMIT) {
    altJobAdmit(e);
  } else if (kind == ALTJ_LIVE) {
    altJobLive();
  }
}

// One pass of the job; true when it has finished. Lookups first; AltLive and the settings echo each wait for a pass
// with no lookup work and the heavy slot free.
static bool altJobStep() {
  AltJob &J = *altJob;
  if (J.stage == 0) {
    int64_t until = esp_timer_get_time() + FRONT_STEP_US;
    while (J.iItem < J.nItem) {
      const AltJobItem &it = J.item[J.iItem];
      if (J.q.phase == FrontQuery<ALT_NAXIS>::IDLE) {
        J.q.begin(it.uploaded ? altFrontUp : altFront2, it.x, J.idwPower, J.ridgeFrac, J.refRadius, J.riskThresh, J.gen);
        J.lookups++;
      }
      if (!J.q.step(until)) return false;
      bool dropped = (J.q.phase == FrontQuery<ALT_NAXIS>::DROPPED);
      J.q.phase = FrontQuery<ALT_NAXIS>::IDLE;
      if (dropped) { frontDropCount++; J.iItem = J.nItem; break; }
      altJobApply(it.kind, it.emit);
      J.iItem++;
      if (esp_timer_get_time() >= until) return false;
    }
    if (!J.liveWanted) { altRefOk = false; altLive_pct = 0; }   // nothing running → no grade (state holds its last value)
    J.stage = 1;
    return false;
  }
  if (gHeavyRanThisPass) return false;
  gHeavyRanThisPass = true;
  if (J.stage == 1) {
    altSendLive();
    if (++altSettCtr >= 5) { J.stage = 2; return false; }
    return true;
  }
  altSettCtr = 0;
  sendAltSettings();
  return true;
}

// Runs the job for this pass and books its time: ft_altEngine per pass, altJob* per finished job (/debug).
static void altJobPass(uint32_t nowMs, bool begin) {
  AltJob &J = *altJob;
  uint32_t t0 = (uint32_t)esp_timer_get_time();
  if (begin) altJobBegin(nowMs);
  bool ended = altJobStep();
  uint32_t dt = (uint32_t)esp_timer_get_time() - t0;
  ft_altEngine.lastCall = dt;
  ft_altEngine.passSeq = ftLoopPassSeq;
  if (dt > ft_altEngine.worstWindow) ft_altEngine.worstWindow = dt;
  if (dt > ft_altEngine.worstSession) ft_altEngine.worstSession = dt;
  J.workUs += dt; J.passes++;
  if (!ended) return;
  J.active = false;
  uint32_t span = millis() - J.tickMs;
  altJobLastUs = J.workUs; altJobLastPasses = J.passes; altJobLastLookups = J.lookups; altJobLastSpanMs = span;
  if (J.workUs > altJobWorstUs) { altJobWorstUs = J.workUs; altJobWorstLookups = J.lookups; }
  if (span > altJobWorstSpanMs) altJobWorstSpanMs = span;
}

// ---- 1 Hz tick (NOT the fold): evaluator/classifier, live telemetry, settings echo. In bench-sim it also
//      advances the simulator and folds at EP_FEED_DT_MS; live, the fold runs in the ~75 Hz pidLog hook. ----
void altHealth_tick(uint32_t nowMs) {
  static uint32_t lastMs = 0;
  if (!altFrontBuf || demoStoresBusy()) return;   // demo mode switching: the stores are mid-swap
  const bool jobIdle = !(altJob && altJob->active);   // the job's tick-time order holds until it finishes
  // A finished cloud sync (heavy: front parse + re-adds + flash write) — one heavy op per pass, and only with
  // the field physically cut (gpio4, not the duty-derived fieldActiveStatus, which reads off in a duty-0 CV hold).
  if (jobIdle && altSyncHandoff && !demoSeal && !gHeavyRanThisPass && fieldCutSettled()) {   // demo: held, the front in RAM is the demo copy
    gHeavyRanThisPass = true;
    altApplySync();
    return;
  }
  if (jobIdle && (regSeedRequest & 4) && !demoSeal && !gHeavyRanThisPass && fieldCutSettled()) {
    gHeavyRanThisPass = true;
    __atomic_fetch_and(&regSeedRequest, (uint8_t)~4, __ATOMIC_ACQ_REL);
    if (altPending && altFront2.count > 0) {
      altPendingCount = 0;
      for (int i = 0; i < altFront2.count && altPendingCount < ALT_PENDING_CAP; i++) altPending[altPendingCount++] = altFrontBuf[i];
      altSentCount = 0;   // a batch already in flight carried none of these
      altPendingSeededFrom = "reregister";
    } else {
      recSeedOwedSet(4, false);
    }
    return;
  }
  // Bench simulator: advance + fold at the Episode feed cadence (at 1 Hz every feed would trip the 5x feed-gap barrier).
  // Field gate open only, as the live fold runs only on a field-on control tick.
  if (altSimMode >= 0.5f && !gpio4IsLow) {
    static uint32_t lastSimMs = 0;
    if (nowMs - lastSimMs >= EP_FEED_DT_MS) {
      lastSimMs = nowMs;
      altSimTick(nowMs);
      altFold_tick(nowMs);
    }
  }
  if (!jobIdle) { altJobPass(nowMs, false); return; }
  if (nowMs - lastMs < 1000) return;
  if (!altJob || altSimMode >= 0.5f) {
    if (gHeavyRanThisPass) return;      // one-heavy-per-pass gate; defer (lastMs unchanged → still due next pass)
    gHeavyRanThisPass = true;
    lastMs = nowMs;
    altTickInline(nowMs);
    return;
  }
  lastMs = nowMs;
  altJobPass(nowMs, true);
}


// ALTERNATOR HEALTH — GUI-adjustable settings (registry-driven): one /get handler loop, one boot-load loop,
// one "AltSettings" SSE echo. ALT_SETTINGS[] is defined with the params so altDebugCsvSend can iterate it.
void altSettingsLoad() {
  // Four knobs are class-dependent 12V-value defaults, and this loader runs BEFORE InitSystemSettings
  // resolves SYSTEM_VOLTAGE_CLASS (NVS else vessel-JSON mirror). Their first creation is DEFERRED to
  // altSeedClassKnobs() right after it; existing keys still load verbatim here. The knobs have no
  // consumer until the runtime fold (altFold_tick, altEpisodeSyncCfg), so the gap is safe.
  for (size_t i = 0; i < ALT_SETTING_COUNT; i++) {
    char key[16];
    snprintf(key, sizeof(key), "%s", ALT_SETTINGS[i].name);  // NVS key = registry name (15-char cap)
    if (!settingExists(key)) {
      if (strcmp(key, "altVbusTol") == 0 || strcmp(key, "altDutyLineRms") == 0
          || strcmp(key, "altDutySlewMax") == 0 || strcmp(key, "altMinDuty") == 0) continue;
      settingWrite(key, String(*ALT_SETTINGS[i].ptr, 4).c_str());
    }
    else *ALT_SETTINGS[i].ptr = settingRead(key).toFloat();
  }
}

// Deferred first-creation seeding for the class-dependent registry knobs — call AFTER InitSystemSettings
// so SYSTEM_VOLTAGE_CLASS and the winding are authoritative. Volt-domain ×(V/12), duty-domain × fieldScale(),
// identity at 12V; a live class or winding change rescales them like every other class-scaled setting.
void altSeedClassKnobs() {
  if (!settingExists("altVbusTol"))     { altVbusTol     *= (float)SYSTEM_VOLTAGE_CLASS / 12.0f; settingWrite("altVbusTol",     String(altVbusTol, 4).c_str()); }
  if (!settingExists("altDutyLineRms")) { altDutyLineRms *= fieldScale(); settingWrite("altDutyLineRms", String(altDutyLineRms, 4).c_str()); }
  if (!settingExists("altDutySlewMax")) { altDutySlewMax *= fieldScale(); settingWrite("altDutySlewMax", String(altDutySlewMax, 4).c_str()); }
  if (!settingExists("altMinDuty"))     { altMinDuty     *= fieldScale(); settingWrite("altMinDuty",     String(altMinDuty, 4).c_str()); }
}
bool altSettingsHandle(AsyncWebServerRequest *request) {
  bool handled = false;
  for (size_t i = 0; i < ALT_SETTING_COUNT; i++) {
    if (paramOk(request, ALT_SETTINGS[i].name)) {   // every registry knob is a float: the §10.9 gate applies
      *ALT_SETTINGS[i].ptr = request->getParam(ALT_SETTINGS[i].name)->value().toFloat();
      char key[16];
      snprintf(key, sizeof(key), "%s", ALT_SETTINGS[i].name);  // NVS key = registry name (15-char cap)
      settingWrite(key, String(*ALT_SETTINGS[i].ptr, 4).c_str());
      handled = true;
    }
  }
  // Reference Source selector: My History (0) | Uploaded File (1), independent of Pause. Switching INTO Uploaded
  // defaults Pause ON (spec §4.3); back to My History leaves Pause; Uploaded with none resident is ignored.
  if (request->hasParam("altSource")) {
    int src = request->getParam("altSource")->value().toInt();
    uint8_t prevRefSource = altRefSource;
    if (src == 1 && altHaveUpload) {
      if (altRefSource != 1) {                              // default Pause ON only when SWITCHING into Uploaded
        altPaused = 1.0f;                                   // (spec §4.3 "on select") — a repeat click won't clobber a deliberate Continue
        settingWrite(NK_altPaused, "1.0000");
      }
      altRefSource = 1;
      settingWrite(NK_altRefSrc, "1");
    } else {
      altRefSource = 0;
      settingWrite(NK_altRefSrc, "0");
    }
    if (altRefSource != prevRefSource) altLatchPct = 0;   // the latched % was graded against the old reference
    handled = true;
  }
  return handled;
}
void sendAltSettings() {
  char buf[448];   // ~230 B at the current 33 knobs and their defaults; sized so a few large values
                   // (rate caps, trend bucket seconds) cannot reach the truncation guard below
  int off = 0;
  for (size_t i = 0; i < ALT_SETTING_COUNT; i++) {
    int n = snprintf(buf + off, sizeof(buf) - off, (i ? ",%.4f" : "%.4f"), *ALT_SETTINGS[i].ptr);
    if (n < 0 || off + n >= (int)sizeof(buf)) break;   // truncated echo beats stack overrun (snprintf returns would-be length)
    off += n;
  }
  sseSend(buf, "AltSettings");
  gxSideOffer(GX_CH_ALTSET, buf, off);
}

// Self-describing schema (served at /altschema). The dashboard fetches this ONCE and zips these
// names against the AltLive / AltSettings payload values — so it never keeps its own field array.
String altSchemaJson() {
  String s = "{\"live\":[";
  for (size_t i = 0; i < ALT_LIVE_COUNT; i++) { if (i) s += ","; s += "\""; s += ALT_LIVE[i].name; s += "\""; }
  s += "],\"settings\":[";
  for (size_t i = 0; i < ALT_SETTING_COUNT; i++) { if (i) s += ","; s += "\""; s += ALT_SETTINGS[i].name; s += "\""; }
  s += "]}";
  return s;
}



// BOAT PERFORMANCE — Best-Ever Front instances (sail 3-D + motor 3-D), mirroring the alternator module; folds at
// ~10 Hz from boatPerf_tick. Sail axes {AWS, AWA, sea-state}; motor {RPM, headwind = AWS·cosAWA, sea-state};
// output = user-selected STW or SOG. Generic engine (Episode/FrontStore): top of this file.
#define PERF_VER         3u
#define PERF_SAILF_MAGIC 0x50534652u  // 'PSFR' sail front blob
#define PERF_MOTF_MAGIC  0x504D4652u  // 'PMFR' motor front blob
#define PERF_PEND_MAGIC  0x5050444Eu  // 'PPDN' unsynced cloud-pending points (sail / motor files)
#define PERF_PENDA_MAGIC 0x50504441u  // 'PPDA' their admitted flags, same count
#define PERF_NAXIS       3
#define PERF_FRONT_CAP   4096     // sparse support points (PSRAM); sized to be unreachable even for fast hulls AP-mode/no-prune (see ALT_HEALTH_LWLR_ENGINE_SPEC.md)
#define PERF_EP_RING_CAP 2048    // Episode trailing-boxcar buffer (PSRAM); only ~EP_AVG_WIN_MS of decimated samples are live (steadiness windows are in the per-axis deques)
#define PERF_PENDING_CAP 4096     // = front cap (PSRAM); covers weeks offline because non-admitted raw points are
                                  // per-cell throttled (PerfPendingQ::push) and admitted ones always get a slot

static Episode<PERF_NAXIS>    &sailEpisode  = PsramPlace::make<Episode<PERF_NAXIS>>();
static Episode<PERF_NAXIS>    &motorEpisode = PsramPlace::make<Episode<PERF_NAXIS>>();
static FrontStore<PERF_NAXIS> sailFront,    motorFront;
static RawSample<PERF_NAXIS>  *sailRing = nullptr,     *motorRing = nullptr;
static FrontPoint<PERF_NAXIS> *sailFrontBuf = nullptr, *motorFrontBuf = nullptr;
static FrontPoint<PERF_NAXIS> *sailPending = nullptr,  *motorPending = nullptr;
static int sailPendingCount = 0, motorPendingCount = 0;
static uint8_t *sailPendingAdm = nullptr, *motorPendingAdm = nullptr;   // 1 = admitted into the local front (never evicted/dropped)
static bool perfPendingOverflowWarned = false;   // one-shot until the next successful cloud sync
static int perfSentSail = 0, perfSentMotor = 0;   // pending prefixes the last built upload carried (Core 1 only)
static volatile bool perfSentSeeded = false;      // that upload was the seeded batch, import or reregister (read by Core 0)
static uint32_t perfLearnSerial = 0;              // bumps on every emitted episode; the periodic save watches it
static String perfPendingSeededFrom = "";   // non-empty → this pending batch is a seeded curve: "import" or "reregister" (provenance tag)
static bool sailCapWarned = false, motorCapWarned = false;   // once per boot OR per Clear All (cleared in resetBoatPerformance)

// Steady-time + sea-state-window + headwind + front/eval + cloud-prune knobs (registry-wired below;
// per-axis deviation bounds + floors + mode flags are in Xregulator.ino).
float perfWsSec  = 3.0f;     // AWS steady time (s)
float perfWaSec  = 3.0f;     // AWA steady time (s)
float perfSeaTol = 1.0f;     // sea-state (pitch-std) deviation band (deg)
float perfSeaSec = 5.0f;     // sea-state steady time (s)
float perfSeaWinSec = 20.0f; // rolling window the pitch-std NUMBER is computed over (≠ perfSeaSec)
float perfRpmSec = 3.0f;     // motoring RPM steady time (s)
float perfHwTol  = 2.0f;     // headwind deviation band (kt)
float perfHwSec  = 3.0f;     // headwind steady time (s)
float perfSafetyMargin = 0.0f;  // kt; 0 = keep only runs that strictly beat the front (sub-front samples only drag down the local surface)
float perfIdwPower     = 2.0f;  // IDW power for the admission-bar/curvature IDW (display prediction is LWLR)
float perfPruneK       = 6.0f;  // cloud prune neighbor count (echoed; applied cloud-side)
// Evaluator/classifier knobs (mirror the alternator's altRefRadius/altRidgeFrac/altRiskThresh):
float perfRefRadius    = 2.0f;   // normalized nearest-support distance beyond which the live % reports no reference
float perfRidgeFrac    = 0.10f;  // LWLR ridge fraction (× slope-block trace/NAXIS)
float perfRiskThresh   = 0.15f;  // classifier risk above which an in-radius point shows "learning" instead of a %

// Coverage / count accessors (CSV2 + dashboard).
float perfCoveragePct()      { return sailFrontBuf  ? (100.0f * (float)sailFront.count  / (float)PERF_FRONT_CAP) : 0.0f; }
float perfMotorCoveragePct() { return motorFrontBuf ? (100.0f * (float)motorFront.count / (float)PERF_FRONT_CAP) : 0.0f; }
int   perfSailCount()  { return sailFront.count; }
int   perfMotorCount() { return motorFront.count; }
// Core 0, on a successful boat-perf upload; same contract as altHandSync.
bool perfHandSync(bool ingest, char *body) {
  if (perfSentSeeded) recSeedOwedSet(2 | 8, false);   // the seeded curve reached the cloud
  FrontSyncHandoff *h = FrontSyncHandoff::make(ingest, body);
  if (!h) return false;
  FrontSyncHandoff *old = __atomic_exchange_n(&perfSyncHandoff, h, __ATOMIC_ACQ_REL);
  FrontSyncHandoff::drop(old);
  return true;
}
// Both fronts as one pending batch, every point admitted, labeled "import" (re-registration relabels it "reregister");
// the cloud stores them as this unit's own rows with that provenance.
static void perfSeedPendingFromFronts() {
  sailPendingCount = motorPendingCount = 0;
  for (int i = 0; i < sailFront.count  && sailPendingCount  < PERF_PENDING_CAP; i++) { if (sailPendingAdm)  sailPendingAdm[sailPendingCount]   = 1; sailPending[sailPendingCount++]   = sailFrontBuf[i]; }
  for (int i = 0; i < motorFront.count && motorPendingCount < PERF_PENDING_CAP; i++) { if (motorPendingAdm) motorPendingAdm[motorPendingCount] = 1; motorPending[motorPendingCount++] = motorFrontBuf[i]; }
  perfPendingSeededFrom = "import";
}

// Pending-queue admission. ADMITTED points (opened or beat a local cell) always queue, evicting the oldest
// non-admitted raw point on overflow, so "front built locally, cloud only prunes" survives any offline stretch.
// Non-admitted raw points are per-cell rate limited and drop with a one-shot warning when full.
#define PERF_RAW_CELL_THROTTLE_MS 600000u   // one non-admitted raw push per conditions-cell per 10 min
// Struct wrapper (not a free function): Arduino's auto-prototype generator emits a broken prototype
// for any free function with a template type in its signature; struct members are left alone.
struct PerfPendingQ {
  template <int N>
  static void push(FrontPoint<N> *q, uint8_t *adm, int &n, int &sent, const FrontPoint<N> &p,
                   bool admitted, const float axisScale[N], uint32_t nowMs) {
    if (!q || !adm) return;
  if (!admitted) {
    // Newest → oldest, bounded by the throttle window (tEmit is nondecreasing for live emits).
    for (int i = n - 1; i >= 0; i--) {
      if ((uint32_t)(nowMs - q[i].tEmit) >= PERF_RAW_CELL_THROTTLE_MS) break;
      bool sameCell = true;
      for (int a = 0; a < N; a++) {
        float sc = (axisScale[a] > 1e-9f) ? axisScale[a] : 1.0f;
        if (fabsf(p.x[a] - q[i].x[a]) > 0.5f * sc) { sameCell = false; break; }
      }
      if (sameCell) return;   // this cell already queued inside the window
    }
  }
  if (n < PERF_PENDING_CAP) { adm[n] = admitted ? 1 : 0; q[n] = p; n++; return; }
  if (!admitted) {
    if (!perfPendingOverflowWarned) {
      perfPendingOverflowWarned = true;
      queueConsoleMessage("WARN: perf pending queue full — raw episode points dropped until next cloud sync");
    }
    return;
  }
  for (int i = 0; i < PERF_PENDING_CAP; i++) {
    if (!adm[i]) {   // evict the oldest non-admitted raw point
      if (i < sent) sent--;   // it was in the in-flight batch: that batch's prefix is one shorter now
      memmove(&q[i], &q[i + 1], (size_t)(PERF_PENDING_CAP - 1 - i) * sizeof(FrontPoint<N>));
      memmove(&adm[i], &adm[i + 1], (size_t)(PERF_PENDING_CAP - 1 - i));
      q[PERF_PENDING_CAP - 1] = p; adm[PERF_PENDING_CAP - 1] = 1;
      return;
    }
  }
  // Queue holds 4096 unsynced ADMITTED records (needs that many distinct best-ever cells) — drop.
  }
};

// ---- boat-performance lookup queue ----
// perfFold_tick() queues each finished steady run instead of admitting it inline; PerfEng::step() admits them in emit
// order (FrontQuery within FRONT_STEP_US per loop pass), and the 1 Hz live grade (SSE step 1) takes its turn behind the
// runs queued before it. Same admissions, pending-queue pushes and grades as the inline fold and step 1 made, from the
// knobs as they were when each was queued; a front write since then drops it.
#define PERF_EMIT_Q 8
struct PerfEmit {
  FrontPoint<PERF_NAXIS> sp, raw;   // sp: device-front coords (sail AWA folded); raw: as emitted, for the pending queue
  uint32_t nowMs, gen;
  float    idwPower, ridgeFrac, safetyMargin;
  bool     sail, dry;               // dry: /debug/frontcheck?drive= lookup, result unused
};
static PerfEmit *perfEmitQ = nullptr;        // PSRAM, PERF_EMIT_Q (initBoatPerformance)
static int       perfEmitN = 0;
static uint32_t  perfEmitDropped = 0;        // queue full: the engine fell PERF_EMIT_Q runs behind
static FrontQuery<PERF_NAXIS> *perfQ = nullptr;   // PSRAM
static uint8_t   perfQKind = 0;              // 0 idle, 1 the head run, 2 the live grade
static bool      perfLiveArmed = false, perfLiveReady = false, perfLiveMotor = false, perfLiveDry = false;
static int       perfLiveBefore = 0;         // runs queued ahead of the live grade
static float     perfLiveX[PERF_NAXIS], perfLiveSpd = 0, perfLiveK[4];   // point, speed, {radius, idw, ridge, risk}
static uint32_t  perfLiveGen = 0;
static uint32_t  perfEngAccUs = 0, perfEngWinMs = 0;

// Members, not free functions: Arduino's auto-prototype generator breaks on template types in free-function signatures.
struct PerfEng {
  static bool idle() { return perfQKind == 0 && perfEmitN == 0 && !perfLiveArmed; }
  static void push(bool sail, const FrontPoint<PERF_NAXIS> &sp, const FrontPoint<PERF_NAXIS> &raw, uint32_t nowMs, bool dry) {
    if (!perfEmitQ) return;
    if (perfEmitN >= PERF_EMIT_Q) { perfEmitDropped++; return; }
    PerfEmit &e = perfEmitQ[perfEmitN++];
    e.sp = sp; e.raw = raw; e.nowMs = nowMs; e.gen = frontGenNow();
    e.idwPower = perfIdwPower; e.ridgeFrac = perfRidgeFrac; e.safetyMargin = perfSafetyMargin;
    e.sail = sail; e.dry = dry;
  }
  // SSE step 1's live grade, as that step read it, queued behind the runs already waiting.
  static void arm(bool motor, const float *x, float spd, bool dry) {
    perfLiveArmed = true; perfLiveReady = false; perfLiveMotor = motor; perfLiveDry = dry;
    for (int a = 0; a < PERF_NAXIS; a++) perfLiveX[a] = x[a];
    perfLiveSpd = spd;
    perfLiveK[0] = perfRefRadius; perfLiveK[1] = perfIdwPower; perfLiveK[2] = perfRidgeFrac; perfLiveK[3] = perfRiskThresh;
    perfLiveGen = frontGenNow();
    perfLiveBefore = perfEmitN;
  }
  static void pop() {
    if (perfEmitN > 1) memmove(perfEmitQ, perfEmitQ + 1, (size_t)(perfEmitN - 1) * sizeof(PerfEmit));
    perfEmitN--;
    if (perfLiveArmed && !perfLiveReady && perfLiveBefore > 0) perfLiveBefore--;
  }
  // Admission, as the inline fold did it: cell-local gate (unvisited cell → unconditional; local support → beat the
  // min(IDW, LWLR) bar − margin), then the pending queue with the admitted flag.
  static void admit(const PerfEmit &e) {
    const FrontQuery<PERF_NAXIS> &q = *perfQ;
    FrontStore<PERF_NAXIS> &fs = e.sail ? sailFront : motorFront;
    bool local = (q.cell >= 0);
    bool admitted = false;
    if (!local || e.sp.y > q.bar - e.safetyMargin) {
      if (!local && fs.count >= PERF_FRONT_CAP) {
        // a genuinely NEW conditions cell dropped at capacity (in-cell improvements still land)
        bool &warned = e.sail ? sailCapWarned : motorCapWarned;
        if (!warned) {
          warned = true;
          queueConsoleMessage(e.sail ? "WARN: sail front at capacity — new conditions are no longer recorded"
                                     : "WARN: motor front at capacity — new conditions are no longer recorded");
        }
      } else admitted = fs.addAt(e.sp, q.cell);   // no console message on accept (counter on the dashboard)
    }
    if (perfSimMode < 0.5f) {
      if (e.sail) PerfPendingQ::push(sailPending, sailPendingAdm, sailPendingCount, perfSentSail, e.raw, admitted, sailFront.axisScale, e.nowMs);
      else        PerfPendingQ::push(motorPending, motorPendingAdm, motorPendingCount, perfSentMotor, e.raw, admitted, motorFront.axisScale, e.nowMs);
      perfLearnSerial++;
    }
  }
  static void applyLive() {
    float pred = perfQ->pred;
    if (perfLiveMotor) {
      motorState = (uint8_t)perfQ->state;
      motorLive_best = pred;
      bool graded = (motorState == FRONT_MEASURED || motorState == FRONT_ESTIMATED);
      motorLive_pct = (graded && pred > 0.1f) ? (perfLiveSpd / pred * 100.0f) : 0.0f;
    } else {
      perfState = (uint8_t)perfQ->state;
      perfLive_best = pred;
      bool graded = (perfState == FRONT_MEASURED || perfState == FRONT_ESTIMATED);
      perfLive_pct = (graded && pred > 0.1f) ? (perfLiveSpd / pred * 100.0f) : 0.0f;
    }
  }
  // ft_perfEngine per pass, plus the work per wall-clock second (/debug).
  static void book(uint32_t dt) {
    ft_perfEngine.lastCall = dt;
    ft_perfEngine.passSeq = ftLoopPassSeq;
    if (dt > ft_perfEngine.worstWindow) ft_perfEngine.worstWindow = dt;
    if (dt > ft_perfEngine.worstSession) ft_perfEngine.worstSession = dt;
    perfEngAccUs += dt;
    uint32_t now = millis();
    if (perfEngWinMs == 0) perfEngWinMs = now;
    if ((uint32_t)(now - perfEngWinMs) >= 1000u) {
      perfEngLastSecUs = perfEngAccUs;
      if (perfEngAccUs > perfEngWorstSecUs) perfEngWorstSecUs = perfEngAccUs;
      perfEngAccUs = 0; perfEngWinMs = now;
    }
  }
  // Every boatPerf_tick pass: lookups within FRONT_STEP_US, in queue order with the live grade in its place.
  static void step() {
    if (!perfQ || !perfEmitQ) return;
    uint32_t t0 = (uint32_t)esp_timer_get_time();
    int64_t until = (int64_t)t0 + FRONT_STEP_US;
    for (;;) {
      if (perfQKind == 0) {
        if (perfLiveArmed && !perfLiveReady && perfLiveBefore == 0) {
          perfQ->begin(perfLiveMotor ? motorFront : sailFront, perfLiveX, perfLiveK[1], perfLiveK[2], perfLiveK[0], perfLiveK[3], perfLiveGen);
          perfQKind = 2;
        } else if (perfEmitN > 0) {
          const PerfEmit &e = perfEmitQ[0];
          perfQ->begin(e.sail ? sailFront : motorFront, e.sp.x, e.idwPower, e.ridgeFrac, perfRefRadius, perfRiskThresh, e.gen);
          perfQKind = 1;
        } else break;
      }
      if (!perfQ->step(until)) break;
      bool dropped = (perfQ->phase == FrontQuery<PERF_NAXIS>::DROPPED);
      perfQ->phase = FrontQuery<PERF_NAXIS>::IDLE;
      if (dropped) frontDropCount++;
      if (perfQKind == 2) {
        if (!dropped && !perfLiveDry) applyLive();
        perfLiveReady = true;
      } else {
        if (!dropped && !perfEmitQ[0].dry) admit(perfEmitQ[0]);
        pop();
      }
      perfQKind = 0;
      if (esp_timer_get_time() >= until) break;
    }
    book((uint32_t)esp_timer_get_time() - t0);
  }
};

// Fold |AWA| to [0,180] when symmetric (eval/display only — raw AWA is stored to pending/cloud).
static inline float perfFoldAwa(float a) {
  if (perfFoldSymmetric < 0.5f) return a;
  float pa = a; while (pa < 0) pa += 360.0f; while (pa >= 360.0f) pa -= 360.0f;
  if (pa > 180.0f) pa = 360.0f - pa;
  return pa;
}

// ---- rolling sea-state: pitch standard deviation over the last perfSeaWinSec (default 20 s) ----
#define PERF_PITCH_RING 256
static float    (&perfPitchVal)[PERF_PITCH_RING] = PsramPlace::make<float[PERF_PITCH_RING]>();
static uint32_t (&perfPitchMs)[PERF_PITCH_RING] = PsramPlace::make<uint32_t[PERF_PITCH_RING]>();
static int      perfPitchHead = 0, perfPitchCount = 0;
static void perfPitchPush(float pitch, uint32_t nowMs) {
  perfPitchVal[perfPitchHead] = pitch; perfPitchMs[perfPitchHead] = nowMs;
  perfPitchHead = (perfPitchHead + 1) % PERF_PITCH_RING;
  if (perfPitchCount < PERF_PITCH_RING) perfPitchCount++;
}
static float perfSeaState(uint32_t nowMs) {
  uint32_t win = (uint32_t)(perfSeaWinSec * 1000.0f);
  double s = 0, s2 = 0; int n = 0;
  for (int k = 0; k < perfPitchCount; k++) {
    int idx = ((perfPitchHead - 1 - k) % PERF_PITCH_RING + PERF_PITCH_RING) % PERF_PITCH_RING;
    if ((uint32_t)(nowMs - perfPitchMs[idx]) > win) break;       // older than the window → stop
    s += perfPitchVal[idx]; s2 += (double)perfPitchVal[idx] * perfPitchVal[idx]; n++;
  }
  if (n < 2) return 0.0f;
  double var = s2 / n - (s / n) * (s / n);
  return (var > 0) ? (float)sqrt(var) : 0.0f;
}

// FIXME (sail/motor steady-state, needs a dedicated rework; alt-health is correct as-is): dwells (3-5 s) must be
// MINUTES (a displacement hull takes that long to settle); the ~2 s boxcar must be minutes over several wave
// cycles (set sail/motor avgWinMs); accept on long-window averages, not instantaneous in/out-of-band gating;
// then raise PERF_MAXDWELL (deque caps) to match.
// Per-axis tol from the deviation-bound knobs, steady time from the *Sec knobs. Resynced each fold.
static void perfEpisodeSyncCfg() {
  sailEpisode.cfg[0]  = { perfWsTol,  perfWsSec  };   // AWS
  sailEpisode.cfg[1]  = { perfWaTol,  perfWaSec  };   // AWA (raw, band-checked)
  sailEpisode.cfg[2]  = { perfSeaTol, perfSeaSec };   // sea-state
  motorEpisode.cfg[0] = { perfRpmTol, perfRpmSec };   // RPM
  motorEpisode.cfg[1] = { perfHwTol,  perfHwSec  };   // headwind
  motorEpisode.cfg[2] = { perfSeaTol, perfSeaSec };   // sea-state
}

// ---- per-tick fold (fast internal cadence, ~10 Hz from boatPerf_tick) ----
// Feeds the SAIL or MOTOR Episode (the other gets an ineligible break so a run can't span a sail<->motor change).
// Wind axes are APPARENT; AWA is stored RAW (both-sided) to pending/cloud, folded to |AWA| only for eval + the
// device front when perfFoldSymmetric.
// Demo-mode feed (perfSim* — written ONLY by perfSimTick, never the real NMEA/IMU globals).
static float perfSimTws = 0, perfSimTwa = 0, perfSimStw = 0, perfSimSog = 0;
static float perfSimPitch = 0, perfSimRpm = 0;

void perfFold_tick(uint32_t nowMs) {
  if (!sailFrontBuf || !motorFrontBuf) return;

  // elapsed since last tick — for sailing/motoring hour accumulators (added once gated, below)
  static uint32_t perfHoursLastMs = 0;
  uint32_t perfDtMs = (perfHoursLastMs == 0) ? 0 : (nowMs - perfHoursLastMs);
  perfHoursLastMs = nowMs;
  if (perfDtMs > 2000) perfDtMs = 0;   // skip gaps (backgrounded tab, reboot, stalls)

  float aws, awa, pitch, rpm, spd; bool haveSpd;
  if (perfSimMode >= 0.5f) {
    aws = perfSimTws; awa = perfSimTwa; pitch = perfSimPitch; rpm = perfSimRpm;
    spd = (perfSpeedSrc >= 1.5f) ? perfSimSog : perfSimStw; haveSpd = true;
  } else {
    if (IS_STALE(IDX_APPARENT_WIND_SPEED) || isnan(ApparentWindSpeedNMEA) || isnan(ApparentWindAngleNMEA)) { perfSteady = false; perfSettleSec = -1.0f; return; }
    aws = ApparentWindSpeedNMEA; awa = ApparentWindAngleNMEA;
    pitch = imu_pitch_deg; rpm = RPM;
    if (perfSpeedSrc >= 1.5f) { haveSpd = !IS_STALE(IDX_SOG_NMEA); spd = SOGNMEA; }
    else                     { haveSpd = (!IS_STALE(IDX_STW_NMEA) && !isnan(STWNMEA)); spd = STWNMEA; }
    if (!haveSpd || isnan(spd)) spd = 0;
  }
  perfPitchPush(pitch, nowMs);
  float sea = perfSeaState(nowMs);
  perfEpisodeSyncCfg();
  uint8_t src = (perfSpeedSrc >= 1.5f) ? 2 : 1;
  float headwind = aws * cosf(awa * (float)PI / 180.0f);   // fore-aft apparent component (+ = headwind)
  bool motoring = (rpm > perfRpmFloor);
  bool sailBoat = boatIsSail();   // a motorboat below the RPM floor is drifting: neither front learns

  // ── live point export (10 Hz): conditions only; prediction/state/% run at 1 Hz in boatPerf_tick.
  //    valid = "inputs usable" — the confidence state decides whether a % is shown. ──
  if (motoring) {
    motorLive_rpm = rpm; motorLive_hw = headwind; motorLive_spd = spd; motorLive_pitch = sea;
    motorLiveSrc = src; motorLiveValid = haveSpd; perfLiveValid = false;
    if (perfSimMode >= 0.5f && sailBoat) {   // demo: the sailing view keeps a point while the engine runs (9_demo.ino)
      demoSailShadow(perfLive_ws, perfLive_wa, perfLive_spd);
      perfLive_pitch = sea; perfLiveSrc = src; perfLiveValid = true;
    }
  } else {
    perfLive_ws = aws; perfLive_wa = awa; perfLive_spd = spd; perfLive_pitch = sea;
    perfLiveSrc = src; perfLiveValid = (sailBoat && haveSpd && aws >= perfMinWindSpeed); motorLiveValid = false;
    if (perfSimMode >= 0.5f) {               // demo: and the motoring view while the engine is stopped
      demoMotorShadow(motorLive_rpm, motorLive_hw, motorLive_spd);
      motorLive_pitch = sea; motorLiveSrc = src; motorLiveValid = true;
    }
  }

  if (hardwarePresent != 1 && perfSimMode < 0.5f) { perfSteady = false; perfSettleSec = -1.0f; return; }   // display only
  if (perfPaused >= 0.5f) { perfSteady = false; perfSettleSec = -1.0f; return; }                           // paused: no learning
  if (IgnoreRPM && perfSimMode < 0.5f) { perfSteady = false; perfSettleSec = -1.0f; return; }             // no engine speed: motoring cannot be told from sailing, neither front learns (§4.8)

  // ── data-maturity hours: time spent actually moving in each mode (only while learning) ──
  if (perfDtMs > 0 && spd >= perfMinBoatSpeed) {
    if (motoring) perfMotorSeconds += perfDtMs / 1000.0;
    else if (sailBoat) perfSailSeconds += perfDtMs / 1000.0;
  }

  // ── feed both Episodes; the inactive one gets an ineligible break so runs don't span a mode change ──
  FrontPoint<PERF_NAXIS> ep;

  bool sailLearn = sailBoat && (sailFront.source != 1) && perfSimMode < 0.5f;   // demo: the seeded record stays as seeded
  bool sailElig = sailLearn && !motoring && haveSpd && spd >= perfMinBoatSpeed && aws >= perfMinWindSpeed;
  RawSample<PERF_NAXIS> ss; ss.x[0] = aws; ss.x[1] = awa; ss.x[2] = sea; ss.out = spd; ss.tMs = nowMs;
  ss.ex[0] = 0; ss.ex[1] = 0;   // sail keeps raw AWS/AWA as surface axes x0/x1 already — no extras needed
  if (sailEpisode.feed(sailElig, ss, &ep)) {
    FrontPoint<PERF_NAXIS> raw = ep;                                  // raw both-sided AWA → cloud
    FrontPoint<PERF_NAXIS> sp = ep; sp.x[1] = perfFoldAwa(ep.x[1]);   // device front: folded AWA (gate operates on folded coords)
    PerfEng::push(true, sp, raw, nowMs, false);   // admit gate + pending queue run in PerfEng::step(), in emit order
  }

  bool motorLearn = (motorFront.source != 1) && perfSimMode < 0.5f;
  bool motorElig = motorLearn && motoring && haveSpd && spd >= perfMinBoatSpeed;
  RawSample<PERF_NAXIS> ms; ms.x[0] = rpm; ms.x[1] = headwind; ms.x[2] = sea; ms.out = spd; ms.tMs = nowMs;
  ms.ex[0] = aws; ms.ex[1] = awa;   // retain raw AWS/AWA (headwind is derived from them) for cloud diagnosis
  if (motorEpisode.feed(motorElig, ms, &ep)) PerfEng::push(false, ep, ep, nowMs, false);   // same as the sail site

  // steady-run indicator: the active mode's Episode is currently accumulating eligible samples
  perfSteady = motoring ? (motorEpisode.count > 0) : (sailEpisode.count > 0);
  {
    bool aelig = motoring ? motorElig : sailElig;
    perfSettleSec = !aelig ? -1.0f
                   : perfSteady ? 0.0f
                   : (motoring ? motorEpisode.settleRemainSec(nowMs) : sailEpisode.settleRemainSec(nowMs));
  }
}
// ---- Demo-mode feed (perfSimMode follows Hardware Present = No, 9_demo.ino) ----
// Copies the simulated boat's own wind, speed, engine and pitch, so the polar dot agrees with the dashboards.
// perfSimTws / perfSimTwa carry APPARENT wind: the sail front's wind axes are apparent.
static void perfSimTick(uint32_t nowMs) {
  (void)nowMs;
  demoPerfSimFeed(perfSimTws, perfSimTwa, perfSimStw, perfSimSog, perfSimRpm, perfSimPitch);
}

// ---- live telemetry registry (single source for the PerfLive payload + /perfschema) ----
// Adding a field = add ONE row; the dashboard zips names from the schema, so they can't desync. All floats.
struct PerfLiveField { const char *name; float (*get)(); };
static float plf_valid()    { return (float)perfLiveValid; }
static float plf_ws()       { return perfLive_ws; }
static float plf_wa()       { return perfLive_wa; }
static float plf_spd()      { return perfLive_spd; }
static float plf_best()     { return perfLive_best; }
static float plf_pct()      { return perfLive_pct; }
static float plf_pitchStd() { return perfLive_pitch; }
static float plf_src()      { return (float)perfLiveSrc; }
static float plf_coverage() { return perfCoveragePct(); }
static float plf_ptCount()  { return (float)sailFront.count; }
static float plf_source()   { return (float)sailFront.source; }    // 0 LEARNED, 1 FIXED
static float plf_paused()   { return (perfPaused >= 0.5f) ? 1.0f : 0.0f; }
// fold timing lives in the Function Timing table (ft_boatPerf row) — not in this live stream
static float plf_sim()      { return (perfSimMode >= 0.5f) ? 1.0f : 0.0f; }
static float plf_syncAgo()  { if (demoSeal) return demoSyncAgoS(1260.0f);
                              if (lastBoatPerfSyncEpoch <= 0 || !timeIsSynced) return -1.0f;
                              time_t n = time(NULL); return (n > (time_t)lastBoatPerfSyncEpoch) ? (float)(n - (time_t)lastBoatPerfSyncEpoch) : 0.0f; }
static float plf_sailHours(){ return (float)(perfSailSeconds / 3600.0); }   // data-maturity hours
static float plf_steady()   { return (perfSimMode >= 0.5f && perfLiveValid) ? 1.0f : (float)perfSteady; }   // in a steady-run right now
static float plf_settleSec(){ return (perfSimMode >= 0.5f && perfLiveValid) ? 0.0f : perfSettleSec; }      // dwell s remaining; 0 settled, -1 idle
static float plf_state()    { return (float)perfState; }    // 0 MEASURED, 1 ESTIMATED, 2 LEARNING_EDGE, 3 NO_REFERENCE
static PerfLiveField PERF_LIVE[] = {
  {"valid", plf_valid}, {"ws", plf_ws}, {"wa", plf_wa}, {"spd", plf_spd},
  {"best", plf_best}, {"pct", plf_pct}, {"pitchStd", plf_pitchStd}, {"src", plf_src},
  {"coverage", plf_coverage}, {"ptCount", plf_ptCount}, {"source", plf_source},
  {"paused", plf_paused},
  {"sim", plf_sim}, {"syncAgoS", plf_syncAgo},
  {"sailHours", plf_sailHours}, {"steady", plf_steady}, {"settleSec", plf_settleSec},
  {"state", plf_state},
};
static const size_t PERF_LIVE_COUNT = sizeof(PERF_LIVE) / sizeof(PERF_LIVE[0]);

static void perfSendLive() {
  // periodic; the next tick resends current state. A backlogged phone link skips only the phone copy.
  const bool phoneOk = !sseBacklogged();
  if (!phoneOk && !gxLiveNow) return;
  char buf[256];
  int off = 0;
  for (size_t i = 0; i < PERF_LIVE_COUNT; i++) {
    int n = snprintf(buf + off, sizeof(buf) - off, (i ? ",%.3f" : "%.3f"), PERF_LIVE[i].get());
    if (n < 0 || off + n >= (int)sizeof(buf)) break;   // truncated echo beats stack overrun (snprintf returns would-be length)
    off += n;
  }
  if (phoneOk) sseSend(buf, "PerfLive");
  gxSideOffer(GX_CH_PERFLIVE, buf, off);
}

// Motoring live registry (same {name,getter} pattern; schema served alongside under "motorLive").
static float pmlf_valid()    { return (float)motorLiveValid; }
static float pmlf_rpm()      { return motorLive_rpm; }
static float pmlf_headwind() { return motorLive_hw; }
static float pmlf_spd()      { return motorLive_spd; }
static float pmlf_best()     { return motorLive_best; }
static float pmlf_pct()      { return motorLive_pct; }
static float pmlf_src()      { return (float)motorLiveSrc; }
static float pmlf_coverage() { return perfMotorCoveragePct(); }
static float pmlf_ptCount()  { return (float)motorFront.count; }
static float pmlf_source()   { return (float)motorFront.source; }
static float pmlf_paused()   { return (perfPaused >= 0.5f) ? 1.0f : 0.0f; }
static float pmlf_pitchStd() { return motorLive_pitch; }
static float pmlf_motorHours() { return (float)(perfMotorSeconds / 3600.0); }   // data-maturity hours
static float pmlf_steady()     { return (perfSimMode >= 0.5f && motorLiveValid) ? 1.0f : (float)perfSteady; }   // in a steady-run right now
static float pmlf_settleSec()  { return (perfSimMode >= 0.5f && motorLiveValid) ? 0.0f : perfSettleSec; }      // dwell s remaining; 0 settled, -1 idle
static float pmlf_state()      { return (float)motorState; }   // 0 MEASURED, 1 ESTIMATED, 2 LEARNING_EDGE, 3 NO_REFERENCE
static PerfLiveField PERF_MOTOR_LIVE[] = {
  {"valid", pmlf_valid}, {"rpm", pmlf_rpm}, {"headwind", pmlf_headwind}, {"spd", pmlf_spd},
  {"best", pmlf_best}, {"pct", pmlf_pct}, {"src", pmlf_src},
  {"coverage", pmlf_coverage}, {"ptCount", pmlf_ptCount}, {"source", pmlf_source}, {"paused", pmlf_paused},
  {"pitchStd", pmlf_pitchStd},
  {"motorHours", pmlf_motorHours}, {"steady", pmlf_steady}, {"settleSec", pmlf_settleSec},
  {"state", pmlf_state},
};
static const size_t PERF_MOTOR_LIVE_COUNT = sizeof(PERF_MOTOR_LIVE) / sizeof(PERF_MOTOR_LIVE[0]);
static void perfSendMotorLive() {
  // periodic; the next tick resends current state. A backlogged phone link skips only the phone copy.
  const bool phoneOk = !sseBacklogged();
  if (!phoneOk && !gxLiveNow) return;
  char buf[256];
  int off = 0;
  for (size_t i = 0; i < PERF_MOTOR_LIVE_COUNT; i++) {
    int n = snprintf(buf + off, sizeof(buf) - off, (i ? ",%.3f" : "%.3f"), PERF_MOTOR_LIVE[i].get());
    if (n < 0 || off + n >= (int)sizeof(buf)) break;   // truncated echo beats stack overrun (snprintf returns would-be length)
    off += n;
  }
  if (phoneOk) sseSend(buf, "MotorLive");
  gxSideOffer(GX_CH_MOTORLIVE, buf, off);
}

// ---- persistence (field-off-gated by caller) ----
void boatPerfSave() {
  if (dbgRingsSynthetic) return;   // fillmax/clearmax: RAM rings are synthetic/empty — keep the real flash blobs
  if (!sailFrontBuf || hardwarePresent != 1 || demoSeal) return;
  uint32_t suw = ((uint32_t)sailFront.source  << 8) | (uint32_t)PERF_NAXIS;
  uint32_t muw = ((uint32_t)motorFront.source << 8) | (uint32_t)PERF_NAXIS;
  writePsramBlob("/sailfront.bin",  PERF_SAILF_MAGIC, PERF_VER, suw, sailFrontBuf,  sizeof(FrontPoint<PERF_NAXIS>), PERF_FRONT_CAP, 0, sailFront.count);
  writePsramBlob("/motorfront.bin", PERF_MOTF_MAGIC,  PERF_VER, muw, motorFrontBuf, sizeof(FrontPoint<PERF_NAXIS>), PERF_FRONT_CAP, 0, motorFront.count);
  // Unsynced points too: else a reboot loses them and the next sync-back erases them from the fronts.
  if (sailPending && sailPendingAdm && motorPending && motorPendingAdm) {
    writePsramBlob("/perfpends.bin",  PERF_PEND_MAGIC,  PERF_VER, 0, sailPending,     sizeof(FrontPoint<PERF_NAXIS>), PERF_PENDING_CAP, 0, sailPendingCount);
    writePsramBlob("/perfpendsa.bin", PERF_PENDA_MAGIC, PERF_VER, 0, sailPendingAdm,  1,                              PERF_PENDING_CAP, 0, sailPendingCount);
    writePsramBlob("/perfpendm.bin",  PERF_PEND_MAGIC,  PERF_VER, 0, motorPending,    sizeof(FrontPoint<PERF_NAXIS>), PERF_PENDING_CAP, 0, motorPendingCount);
    writePsramBlob("/perfpendma.bin", PERF_PENDA_MAGIC, PERF_VER, 0, motorPendingAdm, 1,                              PERF_PENDING_CAP, 0, motorPendingCount);
  }
}
static void boatPerfLoad() {
  uint32_t uw = 0;
  if (sailFrontBuf)  { sailFront.count  = (int)readPsramBlob("/sailfront.bin",  PERF_SAILF_MAGIC, PERF_VER, sailFrontBuf,  sizeof(FrontPoint<PERF_NAXIS>), PERF_FRONT_CAP, &uw, false); sailFront.source  = (uint8_t)((uw >> 8) & 0xFF); }
  if (motorFrontBuf) { motorFront.count = (int)readPsramBlob("/motorfront.bin", PERF_MOTF_MAGIC,  PERF_VER, motorFrontBuf, sizeof(FrontPoint<PERF_NAXIS>), PERF_FRONT_CAP, &uw, false); motorFront.source = (uint8_t)((uw >> 8) & 0xFF); }
  if (sailPending && sailPendingAdm && motorPending && motorPendingAdm && fsExists("/perfpends.bin")) {
    // Points and flags were written together; a count mismatch (torn save) keeps only the common prefix.
    int sp = (int)readPsramBlob("/perfpends.bin",  PERF_PEND_MAGIC,  PERF_VER, sailPending,     sizeof(FrontPoint<PERF_NAXIS>), PERF_PENDING_CAP, nullptr, false);
    int sa = (int)readPsramBlob("/perfpendsa.bin", PERF_PENDA_MAGIC, PERF_VER, sailPendingAdm,  1,                              PERF_PENDING_CAP, nullptr, false);
    int mp = (int)readPsramBlob("/perfpendm.bin",  PERF_PEND_MAGIC,  PERF_VER, motorPending,    sizeof(FrontPoint<PERF_NAXIS>), PERF_PENDING_CAP, nullptr, false);
    int ma = (int)readPsramBlob("/perfpendma.bin", PERF_PENDA_MAGIC, PERF_VER, motorPendingAdm, 1,                              PERF_PENDING_CAP, nullptr, false);
    sailPendingCount  = (sp < sa) ? sp : sa;
    motorPendingCount = (mp < ma) ? mp : ma;
  }
}

// ---- front CSV (the artifact): a boat "curve" = the PAIR of sail + motor BEFRONT1 blocks. target 0 = sail,
//      1 = motor: an int, not FrontStore<3>&, so auto-prototypes don't reference it before PERF_NAXIS exists. ----
// /perfcurve.csv, streamed row-by-row so two 4096-cap fronts never build one huge heap String.
void perfCurveCsvSend(AsyncWebServerRequest *request) {
  struct St { int sc, mc, ssrc, msrc, idx; bool done; char line[96]; int len, pos; };
  St st; st.sc = sailFrontBuf ? sailFront.count : 0; st.mc = motorFrontBuf ? motorFront.count : 0;
  st.ssrc = sailFront.source; st.msrc = motorFront.source;
  st.idx = 0; st.done = false; st.len = 0; st.pos = 0;
  AsyncWebServerResponse *response = request->beginChunkedResponse("text/plain",
    [st](uint8_t *buf, size_t maxLen, size_t) mutable -> size_t {
      size_t written = 0;
      while (written < maxLen) {
        if (st.pos >= st.len) {
          if (st.done) return written;
          if (st.idx == 0) {                                               // SAIL header
            st.len = snprintf(st.line, sizeof(st.line), "BEFRONT1,SAIL,%d,%d,aws,awa,sea,spd\n", PERF_NAXIS, st.ssrc);
          } else if (st.idx <= st.sc) {                                    // sail rows
            FrontPoint<PERF_NAXIS> &p = sailFrontBuf[st.idx - 1];
            st.len = snprintf(st.line, sizeof(st.line), "%.2f,%.1f,%.3f,%.2f,%u,%u\n",
                              p.x[0], p.x[1], p.x[2], p.y, (unsigned)p.nSamp, (unsigned)p.tEmit);
          } else if (st.idx == st.sc + 1) {                                // MOTOR header
            st.len = snprintf(st.line, sizeof(st.line), "BEFRONT1,MOTOR,%d,%d,rpm,hw,sea,spd\n", PERF_NAXIS, st.msrc);
          } else {
            int k = st.idx - st.sc - 2;                                    // motor rows
            if (k >= st.mc) { st.done = true; return written; }
            FrontPoint<PERF_NAXIS> &p = motorFrontBuf[k];
            st.len = snprintf(st.line, sizeof(st.line), "%.0f,%.1f,%.3f,%.2f,%u,%u\n",
                              p.x[0], p.x[1], p.x[2], p.y, (unsigned)p.nSamp, (unsigned)p.tEmit);
          }
          if (st.len > (int)sizeof(st.line) - 1) st.len = sizeof(st.line) - 1;  // clamp snprintf's intended-len to the buffer (defensive vs a future wider field)
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
// /perfrecords.csv — scatter table, sail rows then motor rows, mode-tagged. Streamed row-by-row.
void perfRecordsCsvSend(AsyncWebServerRequest *request) {
  struct St { int sc, mc, idx; bool done; char line[96]; int len, pos; };
  St st; st.sc = sailFrontBuf ? sailFront.count : 0; st.mc = motorFrontBuf ? motorFront.count : 0;
  st.idx = 0; st.done = false; st.len = 0; st.pos = 0;
  AsyncWebServerResponse *response = request->beginChunkedResponse("text/csv",
    [st](uint8_t *buf, size_t maxLen, size_t) mutable -> size_t {
      size_t written = 0;
      while (written < maxLen) {
        if (st.pos >= st.len) {
          if (st.done) return written;
          if (st.idx == 0) {
            st.len = snprintf(st.line, sizeof(st.line), "mode,a0,a1,sea,spd,nSamp\n");
          } else if (st.idx <= st.sc) {                                    // sail rows
            FrontPoint<PERF_NAXIS> &p = sailFrontBuf[st.idx - 1];
            st.len = snprintf(st.line, sizeof(st.line), "sail,%.2f,%.1f,%.3f,%.2f,%u\n",
                              p.x[0], p.x[1], p.x[2], p.y, (unsigned)p.nSamp);
          } else {
            int k = st.idx - st.sc - 1;                                    // motor rows
            if (k >= st.mc) { st.done = true; return written; }
            FrontPoint<PERF_NAXIS> &p = motorFrontBuf[k];
            st.len = snprintf(st.line, sizeof(st.line), "motor,%.0f,%.1f,%.3f,%.2f,%u\n",
                              p.x[0], p.x[1], p.x[2], p.y, (unsigned)p.nSamp);
          }
          if (st.len > (int)sizeof(st.line) - 1) st.len = sizeof(st.line) - 1;  // clamp snprintf's intended-len to the buffer (defensive vs a future wider field)
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
// Parse one BEFRONT1 block (starting at p) into the sail (target 0) or motor (target 1) front.
static int perfIngestBlock(char *p, int target) {
  FrontStore<PERF_NAXIS> &f = target ? motorFront : sailFront;
  FrontPoint<PERF_NAXIS> *buf = target ? motorFrontBuf : sailFrontBuf;
  char *nl = strchr(p, '\n'); if (!nl) return 0;
  uint8_t src = 0;
  { char saved = *nl; *nl = '\0'; char *t = strtok(p, ",");   // BEFRONT1
    t = strtok(NULL, ","); t = strtok(NULL, ","); t = strtok(NULL, ",");   // sys, naxis, source
    if (t) src = (uint8_t)atoi(t); *nl = saved; }
  int n = 0; char *line = nl + 1;
  while (line && *line && n < PERF_FRONT_CAP) {
    if (strncmp(line, "BEFRONT1", 8) == 0) break;             // next block begins
    char *eol = strchr(line, '\n'); if (eol) *eol = '\0';
    float x0, x1, x2, y; unsigned ns = 0, te = 0;
    if (*line && sscanf(line, "%f,%f,%f,%f,%u,%u", &x0, &x1, &x2, &y, &ns, &te) >= 4) {
      FrontPoint<PERF_NAXIS> q; q.x[0] = x0; q.x[1] = x1; q.x[2] = x2; q.y = y;
      q.ex[0] = 0; q.ex[1] = 0;   // raw extras live in the cloud table, not the front CSV
      q.nSamp = (uint32_t)ns; q.tEmit = (uint32_t)te;
      buf[n++] = q;
    }
    if (!eol) break;
    line = eol + 1;
  }
  if (line && *line && strncmp(line, "BEFRONT1", 8) != 0 && n >= PERF_FRONT_CAP)   // more than we can hold
    queueConsoleMessageF("WARN: %s front truncated at %d pts — raise PERF_FRONT_CAP", target ? "motor" : "sail", PERF_FRONT_CAP);
  f.count = n; f.source = src;
  return n;
}
// The cloud reply (or a saved file) carries both blocks (SAIL then MOTOR). Replace both held fronts.
bool perfIngestFrontCsv(char *body) {
  char *sp = strstr(body, "BEFRONT1,SAIL");
  char *mp = strstr(body, "BEFRONT1,MOTOR");
  frontWriteBegin();
  if (sp) perfIngestBlock(sp, 0);
  if (mp) perfIngestBlock(mp, 1);
  frontWriteEnd();
  return (sp || mp);
}

// ---- cloud upload: batch of accepted points since last upload (sail + motor); pruned fronts back ----
// Schema: {device_uid,token,ts,sys:"BOAT",speedSrc,foldSym,pruneK,idwPower,
//          sail:[[aws,awa,sea,spd,nSamp]...], motor:[[rpm,hw,sea,spd,nSamp,aws,awa]...]}. AWA raw both-sided;
//          motor also carries raw AWS/AWA (headwind is derived from them) for cloud diagnosis.
// The batch's prefix leaves pending on a successful response (executeUploadBoatPerf → perfHandSync → perfApplySync on Core 1).
bool buildBoatPerfPayload(char *buf, size_t size) {
  if ((sailPendingCount == 0 && motorPendingCount == 0) || authToken.isEmpty()) return false;
  if (perfSyncHandoff) return false;   // last sync not applied yet: its sent prefixes are still in the queues
  time_t now_ts = time(NULL);
  int off = snprintf(buf, size,
    "{\"device_uid\":\"%s\",\"token\":\"%s\",\"ts\":\"%s\",\"sys\":\"BOAT\",\"speedSrc\":%d,\"foldSym\":%d,\"pruneK\":%d,\"idwPower\":%.2f,",
    device_id_hex, authToken.c_str(), formatTimestamp(now_ts),
    (int)perfSpeedSrc, (perfFoldSymmetric >= 0.5f) ? 1 : 0, (int)perfPruneK, perfIdwPower);
  if (off < 0 || (size_t)off >= size) return false;
  if (perfPendingSeededFrom.length()) {   // seeded curve: tag the whole batch with its provenance label
    off += snprintf(buf + off, size - off, "\"seededFrom\":\"%s\",", perfPendingSeededFrom.c_str());
    if (off < 0 || (size_t)off >= size) return false;
  }
  off += snprintf(buf + off, size - off, "\"sail\":[");
  bool first = true;
  int ks = 0, km = 0;
  for (; ks < sailPendingCount; ks++) {
    if (size - (size_t)off < 64) break;
    FrontPoint<PERF_NAXIS> &p = sailPending[ks];
    off += snprintf(buf + off, size - off, "%s[%.2f,%.1f,%.3f,%.2f,%u]", first ? "" : ",", p.x[0], p.x[1], p.x[2], p.y, p.nSamp);
    first = false;
  }
  off += snprintf(buf + off, size - off, "],\"motor\":[");
  first = true;
  for (; km < motorPendingCount; km++) {
    if (size - (size_t)off < 64) break;
    FrontPoint<PERF_NAXIS> &p = motorPending[km];
    off += snprintf(buf + off, size - off, "%s[%.0f,%.1f,%.3f,%.2f,%u,%.2f,%.1f]",
                    first ? "" : ",", p.x[0], p.x[1], p.x[2], p.y, p.nSamp, p.ex[0], p.ex[1]);  // ex=raw AWS,AWA
    first = false;
  }
  off += snprintf(buf + off, size - off, "]}");
  if (off < 0 || (size_t)off >= size - 1) return false;
  perfSentSail = ks; perfSentMotor = km;
  perfSentSeeded = (perfPendingSeededFrom.length() > 0);
  return true;
}
// Same contract as altSyncBufBytes: the larger of this batch's JSON (64 B per-point margin) and the SAIL + MOTOR reply,
// growing up to PERF_UPLOAD_BUF_SIZE when the cloud's fronts outrun the local ones.
#define PERF_CSV_ROW_MAX 48  // widest update-boat-performance front row: x0, x1 1dp, x2 3dp, spd 2dp, nSamp, 0
size_t perfSyncBufBytes() {
  size_t pend = (size_t)sailPendingCount + (size_t)motorPendingCount;
  if (pend == 0 || authToken.isEmpty()) return 0;
  size_t out = 512 + authToken.length() + perfPendingSeededFrom.length() + (pend + 2) * 64;
  size_t rows = (size_t)sailFront.count + (size_t)motorFront.count + pend;
  size_t back = 256 + (rows + rows / 8 + 64) * PERF_CSV_ROW_MAX;
  size_t n = (out > back) ? out : back;
  return (n < (size_t)PERF_UPLOAD_BUF_SIZE) ? n : (size_t)PERF_UPLOAD_BUF_SIZE;
}

// Load CSV pair from the browser: replace both fronts, then fixed=true -> FIXED + paused (hold the borrowed polar
// as-is), false -> LEARNED + resumed (refine from your own sailing). Persists immediately (the file isn't on the
// device to re-load). Non-static for the /perfUploadFront handler in 3_functions.ino. Mutates body.
bool perfUploadFrontCsv(char *body, bool fixed) {
  if (!sailFrontBuf || !body) return false;
  bool ok = perfIngestFrontCsv(body);
  if (!ok) return false;
  uint8_t src = fixed ? 1 : 0;
  sailFront.source = src; motorFront.source = src;
  perfPaused = fixed ? 1.0f : 0.0f;
  settingWrite(NK_perfPaused, fixed ? "1.0000" : "0.0000");
  if (fixed) {                                  // FREEZE — local only, never uploaded
    sailPendingCount = motorPendingCount = 0; perfSentSail = perfSentMotor = 0; perfPendingSeededFrom = "";
  } else {                                      // LEARN — adopt to cloud: stage both fronts (tagged), keep refining
    perfSeedPendingFromFronts();                // replace pending with the imported set (pure seeded batch)
    perfSentSail = perfSentMotor = 0;
    // The loaded curve replaces this unit's, as a records import does: the cloud rebuilds the fronts from every row
    // it holds, so its old rows go first, and the seeded upload waits for the wipe (and survives a reboot).
    recSeedOwedSet(2, true);
    perfCloudWipePending = true;
    settingWrite(NK_PerfWipePend, "1");
  }
  // Immediate persist even field-on: user-initiated import — see altUploadFrontCsv.
  boatPerfSave();
  queueConsoleMessageF("PerfFront: UPLOADED sail %d + motor %d (%s)",
                       sailFront.count, motorFront.count, fixed ? "FIXED, paused" : "LEARNED, adopting to cloud");
  return true;
}

// ---- lifecycle ----
void initBoatPerformance() {
  sailRing      = (RawSample<PERF_NAXIS>  *)ps_malloc((size_t)PERF_EP_RING_CAP * sizeof(RawSample<PERF_NAXIS>));
  motorRing     = (RawSample<PERF_NAXIS>  *)ps_malloc((size_t)PERF_EP_RING_CAP * sizeof(RawSample<PERF_NAXIS>));
  sailFrontBuf  = (FrontPoint<PERF_NAXIS> *)ps_malloc((size_t)PERF_FRONT_CAP   * sizeof(FrontPoint<PERF_NAXIS>));
  motorFrontBuf = (FrontPoint<PERF_NAXIS> *)ps_malloc((size_t)PERF_FRONT_CAP   * sizeof(FrontPoint<PERF_NAXIS>));
  sailPending   = (FrontPoint<PERF_NAXIS> *)ps_malloc((size_t)PERF_PENDING_CAP * sizeof(FrontPoint<PERF_NAXIS>));
  motorPending  = (FrontPoint<PERF_NAXIS> *)ps_malloc((size_t)PERF_PENDING_CAP * sizeof(FrontPoint<PERF_NAXIS>));
  sailPendingAdm  = (uint8_t *)ps_malloc(PERF_PENDING_CAP);
  motorPendingAdm = (uint8_t *)ps_malloc(PERF_PENDING_CAP);
  perfEmitQ = (PerfEmit *)ps_malloc((size_t)PERF_EMIT_Q * sizeof(PerfEmit));
  perfQ     = (FrontQuery<PERF_NAXIS> *)ps_malloc(sizeof(FrontQuery<PERF_NAXIS>));
  if (!sailRing || !motorRing || !sailFrontBuf || !motorFrontBuf || !sailPending || !motorPending
      || !sailPendingAdm || !motorPendingAdm || !perfEmitQ || !perfQ) {
    queueConsoleMessage("ERROR: BoatPerf ps_malloc failed"); return;
  }
  memset(perfQ, 0, sizeof(FrontQuery<PERF_NAXIS>));
  memset(sailFrontBuf,  0, (size_t)PERF_FRONT_CAP * sizeof(FrontPoint<PERF_NAXIS>));
  memset(motorFrontBuf, 0, (size_t)PERF_FRONT_CAP * sizeof(FrontPoint<PERF_NAXIS>));
  // Deque window caps {AWS|RPM, AWA|headwind, sea, output}: sea-state has the longest dwell (60 s headroom);
  // index 3 is the unused output band (perf leaves outCfg disabled).
  static const float PERF_MAXDWELL[PERF_NAXIS + 1] = { 30.0f, 30.0f, 60.0f, 30.0f };
  sailEpisode.init(sailRing, PERF_EP_RING_CAP, PERF_MAXDWELL);
  motorEpisode.init(motorRing, PERF_EP_RING_CAP, PERF_MAXDWELL);
  perfEpisodeSyncCfg();
  sailFront.init(sailFrontBuf, PERF_FRONT_CAP);
  motorFront.init(motorFrontBuf, PERF_FRONT_CAP);
  // axisScale: balanced against synthesized polar fronts (rationale: ALT_HEALTH_LWLR_ENGINE_SPEC.md
  // item 17). MUST match SAIL_SCALE/MOTOR_SCALE in the update-boat-performance edge fn.
  sailFront.axisScale[0]  = 2.0f;   sailFront.axisScale[1]  = 12.0f; sailFront.axisScale[2]  = 1.0f;  // AWS, AWA, sea
  motorFront.axisScale[0] = 100.0f; motorFront.axisScale[1] = 4.0f;  motorFront.axisScale[2] = 1.0f;  // RPM, headwind, sea
  sailPendingCount = motorPendingCount = 0;
  boatPerfLoad();
  if (recSeedOwed(2 | 8)) {    // imported (2) or pre-registration (8) fronts the cloud does not hold yet; a FIXED (borrowed, paused) pair is never uploaded
    if (sailFront.source == 0 && motorFront.source == 0 && (sailFront.count > 0 || motorFront.count > 0)) {
      const bool imported = recSeedOwed(2);
      perfSeedPendingFromFronts();
      if (!imported) perfPendingSeededFrom = "reregister";
    }
    else recSeedOwedSet(2 | 8, false);
  }
  queueConsoleMessageF("BoatPerf init: sail %d pts (%s), motor %d pts (%s)",
                       sailFront.count, sailFront.source ? "FIXED" : "LEARNED",
                       motorFront.count, motorFront.source ? "FIXED" : "LEARNED");
}
void resetBoatPerformance() {
  if (!sailFrontBuf) return;
  frontWriteBegin(); sailFront.count = 0; sailFront.source = 0; motorFront.count = 0; motorFront.source = 0; frontWriteEnd();
  sailPendingCount = motorPendingCount = 0; perfSentSail = perfSentMotor = 0; sailCapWarned = motorCapWarned = false;
  sailEpisode.clearRun();  sailEpisode.ringHead = 0;  sailEpisode.ringCount = 0;
  motorEpisode.clearRun(); motorEpisode.ringHead = 0; motorEpisode.ringCount = 0;
  perfPitchHead = 0; perfPitchCount = 0;
  perfSailSeconds = 0.0; perfMotorSeconds = 0.0;   // data-maturity hours reset with the learned data
  perfLiveValid = false; motorLiveValid = false; perfLive_pct = 0; motorLive_pct = 0;
  fsTakeLock();
  LittleFS.remove("/sailfront.bin");
  LittleFS.remove("/motorfront.bin");
  LittleFS.remove("/perfpends.bin");  LittleFS.remove("/perfpendsa.bin");
  LittleFS.remove("/perfpendm.bin");  LittleFS.remove("/perfpendma.bin");
  fsReleaseLock();
  // Cloud half, as resetAlternatorHealth: the cloud rebuilds both fronts from every raw point it holds, so without this
  // the next sync-back would hand the cleared fronts straight back. executeResetBoatPerfCloud retries until 200.
  perfCloudWipePending = true;
  settingWrite(NK_PerfWipePend, "1");
  queueConsoleMessage("BoatPerf: full reset (Clear All)");
}

// Motor-only counterpart of resetBoatPerformance(). The motoring front's axis 0 is engine RPM, so a
// tach rescale invalidates it; the sail front (AWS/AWA/sea-state) has no RPM axis and must survive.
void resetMotorFrontOnly() {
  if (!sailFrontBuf) return;  // shared PSRAM arena backs both fronts
  frontWriteBegin(); motorFront.count = 0; motorFront.source = 0; frontWriteEnd();
  motorPendingCount = 0; perfSentMotor = 0; motorCapWarned = false;
  motorEpisode.clearRun(); motorEpisode.ringHead = 0; motorEpisode.ringCount = 0;
  perfMotorSeconds = 0.0;
  motorLiveValid = false; motorLive_pct = 0;
  fsTakeLock();
  LittleFS.remove("/motorfront.bin");
  LittleFS.remove("/perfpendm.bin");  LittleFS.remove("/perfpendma.bin");
  fsReleaseLock();
  queueConsoleMessage("BoatPerf: motoring front cleared (engine-RPM axis rescaled); sail front kept");
}

// ---- tick (call from loop()): fold at ~10 Hz, send live telemetry + settings echo at ~1 Hz ----
// Core 1: drop only the points the synced batch carried (the queues kept growing while it was in flight), replace
// both fronts with the cloud's pruned ones, then re-add the still-unsent admitted points, which the cloud has not
// seen. Pending holds raw AWA; the sail front works in folded coordinates.
static void perfApplySync() {
  FrontSyncHandoff *h = __atomic_exchange_n(&perfSyncHandoff, (FrontSyncHandoff *)nullptr, __ATOMIC_ACQ_REL);
  if (!h) return;
  int ds = (perfSentSail  < sailPendingCount)  ? perfSentSail  : sailPendingCount;
  int dm = (perfSentMotor < motorPendingCount) ? perfSentMotor : motorPendingCount;
  if (ds > 0) {
    memmove(sailPending, sailPending + ds, (size_t)(sailPendingCount - ds) * sizeof(FrontPoint<PERF_NAXIS>));
    memmove(sailPendingAdm, sailPendingAdm + ds, (size_t)(sailPendingCount - ds));
    sailPendingCount -= ds;
  }
  if (dm > 0) {
    memmove(motorPending, motorPending + dm, (size_t)(motorPendingCount - dm) * sizeof(FrontPoint<PERF_NAXIS>));
    memmove(motorPendingAdm, motorPendingAdm + dm, (size_t)(motorPendingCount - dm));
    motorPendingCount -= dm;
  }
  if (ds > 0 || dm > 0) perfPendingSeededFrom = "";   // a batch built before a seed carried none of it: the label stays
  perfSentSail = perfSentMotor = 0; perfPendingOverflowWarned = false;
  // A Frozen (source 1) reference is held as loaded; the cloud's rebuild would replace it and flip it to Learning.
  if (h->ingest && !rpmAxisWipePending && !perfCloudWipePending && sailFront.source != 1 && motorFront.source != 1) {
    if (perfIngestFrontCsv(h->body)) {
      for (int i = 0; i < sailPendingCount; i++) {
        if (!sailPendingAdm[i]) continue;
        FrontPoint<PERF_NAXIS> p = sailPending[i];
        p.x[1] = perfFoldAwa(p.x[1]);
        sailFront.add(p);
      }
      for (int i = 0; i < motorPendingCount; i++) if (motorPendingAdm[i]) motorFront.add(motorPending[i]);
      queueConsoleMessage("Boat performance: fronts updated from cloud");
    } else {
      queueConsoleMessage("Boat performance uploaded (no front in response)");
    }
  }
  FrontSyncHandoff::drop(h);
  boatPerfSave();   // caller runs this only with the field physically cut (a LittleFS write stalls the flash cache)
}

void boatPerf_tick(uint32_t nowMs) {
  if (demoStoresBusy()) return;   // demo mode switching: the stores are mid-swap
  static uint32_t lastFold = 0, lastSse = 0;
  static uint8_t  perfSseStep = 0;   // 0 = idle; 1..4 = the 1 Hz SSE burst, SPREAD one step per loop pass
  static uint8_t  sc = 0;            // sendPerfSettings cadence: every 5th SSE cycle
  static uint32_t lastPeriodicSave = 0, savedLearnSerial = 0;
  if (!sailFrontBuf) return;
  PerfEng::step();   // queued runs and the live grade, within FRONT_STEP_US; no heavy slot
  // Heavy one-offs, one per pass and only with the field physically cut (gpio4, not duty-derived fieldActiveStatus):
  // a finished cloud sync, and a 15 min save of new learning (sailing engine-off has no field-off edge to persist on).
  // Held in demo mode: the fronts in RAM are the demo copies. All wait for the lookup queue to empty, so every run
  // emitted before them is admitted first, as the inline fold did.
  if (!gHeavyRanThisPass && !demoSeal && fieldCutSettled() && PerfEng::idle()) {
    if (perfSyncHandoff) {
      gHeavyRanThisPass = true;
      perfApplySync();
      return;
    }
    if (regSeedRequest & 8) {   // a FIXED (borrowed, paused) pair stays local-only, as at boot
      gHeavyRanThisPass = true;
      __atomic_fetch_and(&regSeedRequest, (uint8_t)~8, __ATOMIC_ACQ_REL);
      if (sailFront.source == 0 && motorFront.source == 0 && (sailFront.count > 0 || motorFront.count > 0)) {
        perfSeedPendingFromFronts();
        perfSentSail = perfSentMotor = 0;   // a batch already in flight carried none of these
        perfPendingSeededFrom = "reregister";
      } else {
        recSeedOwedSet(8, false);
      }
      return;
    }
    if (perfLearnSerial != savedLearnSerial && (uint32_t)(nowMs - lastPeriodicSave) >= 900000u) {
      gHeavyRanThisPass = true;
      lastPeriodicSave = nowMs;
      savedLearnSerial = perfLearnSerial;
      boatPerfSave();
      return;
    }
  }
  // One-heavy-per-pass gate: defer if any sub-op is due but another heavy ran (timers unchanged -> due next pass).
  // The 1 Hz SSE burst (~7 ms stacked) is split one step per pass so it never becomes the tallest loop pole.
  bool foldDue  = ((uint32_t)(nowMs - lastFold) >= 100);
  bool sseStart = ((uint32_t)(nowMs - lastSse) >= 1000);
  // Step 1 waiting on its queued lookup takes no slot, so the 10 Hz fold keeps its cadence meanwhile.
  bool sseWait  = (perfSseStep == 1 && perfLiveArmed && !perfLiveReady);
  if (foldDue || sseStart || (perfSseStep && !sseWait)) {
    if (gHeavyRanThisPass) return;
    gHeavyRanThisPass = true;
  } else {
    if (perfSimMode >= 0.5f) perfSimTick(nowMs);        // keep the sim feed alive on idle passes
    return;
  }
  if (perfSimMode >= 0.5f) perfSimTick(nowMs);          // demo feed first
  if (sseStart && perfSseStep == 0) { lastSse = nowMs; perfSseStep = 1; }   // arm the 1 Hz burst
  // Do exactly ONE heavy sub-op per pass: advance the SSE burst if one is pending, else run the fold.
  // (Prioritising the burst defers the 10 Hz fold by at most a few passes — jitter, not loss.)
  if (perfSseStep && !(perfSseStep == 1 && perfLiveArmed && !perfLiveReady)) {
    switch (perfSseStep) {
      case 1:
        // step 1: 1 Hz evaluator + OUTPUT-BLIND state classifier for the active mode (mirrors
        // altHealth_tick). NO clamp on the % — it may exceed 100 vs a stale front. Demo: both views, from the model.
        // Live: the point is read here and graded by PerfEng::step() on the passes after (PerfEng::applyLive).
        if (perfLiveArmed) { perfLiveArmed = false; perfSseStep = 2; break; }   // graded and applied
        if (perfSimMode >= 0.5f) { demoPerfGrade(); perfSseStep = 2; break; }
        {
          const bool drive = frontDriveOn();
          float px[PERF_NAXIS];
          if (drive && frontDriveOld) {   // bench: the inline reads for a second with one run, in this pass
            uint32_t d0 = (uint32_t)esp_timer_get_time();
            volatile float sink = 0;
            float pr = 0;
            FrontQuery<PERF_NAXIS>::probe(sailFront, frontDriveSeq++, 0.6f, px);
            sink = sink + (float)sailFront.classify(px, perfRefRadius, perfIdwPower, perfRidgeFrac, perfRiskThresh, &pr) + pr;
            FrontQuery<PERF_NAXIS>::probe(sailFront, frontDriveSeq++, 0.6f, px);
            sink = sink + (sailFront.hasLocalSupport(px) ? 1.0f : 0.0f)
                 + (sailFront.pushesHybrid(px, 0.0f, perfSafetyMargin, perfIdwPower, perfRidgeFrac) ? 1.0f : 0.0f)
                 + (float)sailFront.cellOf(px);
            PerfEng::book((uint32_t)esp_timer_get_time() - d0);
          } else if (drive) {             // bench: the same second through the queue, results unused
            FrontPoint<PERF_NAXIS> dp = {};
            FrontQuery<PERF_NAXIS>::probe(sailFront, frontDriveSeq++, 0.6f, dp.x);
            PerfEng::push(true, dp, dp, nowMs, true);
          }
          if (motorLiveValid) {
            float surf[PERF_NAXIS] = { motorLive_rpm, motorLive_hw, motorLive_pitch };
            PerfEng::arm(true, surf, motorLive_spd, false);
          } else if (perfLiveValid) {
            float surf[PERF_NAXIS] = { perfLive_ws, perfFoldAwa(perfLive_wa), perfLive_pitch };
            PerfEng::arm(false, surf, perfLive_spd, false);
          } else if (drive && !frontDriveOld) {
            FrontQuery<PERF_NAXIS>::probe(sailFront, frontDriveSeq++, 0.6f, px);
            PerfEng::arm(false, px, 0.0f, true);
          } else {
            perfSseStep = 2;
          }
        }
        break;
      case 2: perfSendLive();      perfSseStep = 3; break;                  // step 2: sailing live SSE
      case 3: perfSendMotorLive(); perfSseStep = (++sc >= 5) ? 4 : 0; break; // step 3: motoring live SSE
      case 4: sc = 0; sendPerfSettings(); perfSseStep = 0; break;           // step 4: settings echo (1-in-5)
    }
  } else if (foldDue) {
    lastFold = nowMs; perfFold_tick(nowMs);                                 // ~10 Hz fold
  }
}

// The /debug "Record-book engine" line (3_functions.ino cannot see this file's statics).
int frontEngineDebugLine(char *buf, size_t cap) {
  uint32_t now = millis();
  int32_t left = frontDriveUntilMs ? (int32_t)(frontDriveUntilMs - now) : 0;
  char drive[40];
  if (left > 0) snprintf(drive, sizeof(drive), "%s, %lds left", frontDriveOld ? "OLD inline reads" : "NEW engine", (long)(left / 1000));
  else snprintf(drive, sizeof(drive), "off");
  return snprintf(buf, cap,
    "Record-book engine (Core 1, <=%u us of lookups per loop pass each): alt pass last %.2f ms, worst %.2f (10 s) / %.2f ms;"
    " 1 Hz job last %u lookups, %.2f ms work over %u passes in %lu ms, worst %.2f ms (%u lookups), longest %lu ms;"
    " boat perf pass last %.2f ms, worst %.2f / %.2f ms, work last second %.2f ms, worst %.2f ms, queue %d, full-drops %lu;"
    " fronts alt %d, uploaded %d, sail %d, motor %d; lookups dropped by a front write %lu; drive %s\n",
    (unsigned)FRONT_STEP_US, ft_altEngine.lastCall / 1000.0f, ft_altEngine.worstWindow / 1000.0f, ft_altEngine.worstSession / 1000.0f,
    (unsigned)altJobLastLookups, altJobLastUs / 1000.0f, (unsigned)altJobLastPasses, (unsigned long)altJobLastSpanMs,
    altJobWorstUs / 1000.0f, (unsigned)altJobWorstLookups, (unsigned long)altJobWorstSpanMs,
    ft_perfEngine.lastCall / 1000.0f, ft_perfEngine.worstWindow / 1000.0f, ft_perfEngine.worstSession / 1000.0f,
    perfEngLastSecUs / 1000.0f, perfEngWorstSecUs / 1000.0f, perfEmitN, (unsigned long)perfEmitDropped,
    altFront2.count, altHaveUpload ? altFrontUp.count : 0, sailFront.count, motorFront.count,
    (unsigned long)frontDropCount, drive);
}

// ===== BOAT PERFORMANCE — GUI-adjustable settings (registry-driven, like AltSettings). =====
struct PerfSetting { const char *name; float *ptr; };
static PerfSetting PERF_SETTINGS[] = {
  {"perfWsTol", &perfWsTol}, {"perfWsSec", &perfWsSec},
  {"perfWaTol", &perfWaTol}, {"perfWaSec", &perfWaSec},
  {"perfSeaTol", &perfSeaTol}, {"perfSeaSec", &perfSeaSec}, {"perfSeaWinSec", &perfSeaWinSec},
  {"perfRpmTol", &perfRpmTol}, {"perfRpmSec", &perfRpmSec},
  {"perfHwTol", &perfHwTol}, {"perfHwSec", &perfHwSec},
  {"perfMinBoatSpeed", &perfMinBoatSpeed}, {"perfMinWindSpeed", &perfMinWindSpeed},
  {"perfRpmFloor", &perfRpmFloor},
  {"perfSafetyMargin", &perfSafetyMargin}, {"perfIdwPower", &perfIdwPower}, {"perfPruneK", &perfPruneK},
  {"perfRefRadius", &perfRefRadius}, {"perfRidgeFrac", &perfRidgeFrac}, {"perfRiskThresh", &perfRiskThresh},
  {"perfSpeedSrc", &perfSpeedSrc}, {"perfFoldSymmetric", &perfFoldSymmetric}, {"perfPaused", &perfPaused},
};
static const size_t PERF_SETTING_COUNT = sizeof(PERF_SETTINGS) / sizeof(PERF_SETTINGS[0]);

void perfSettingsLoad() {
  for (size_t i = 0; i < PERF_SETTING_COUNT; i++) {
    char key[16];
    snprintf(key, sizeof(key), "%s", PERF_SETTINGS[i].name);  // NVS key = registry name truncated to the 15-char cap
    if (!settingExists(key)) settingWrite(key, String(*PERF_SETTINGS[i].ptr, 4).c_str());
    else *PERF_SETTINGS[i].ptr = settingRead(key).toFloat();
  }
}
bool perfSettingsHandle(AsyncWebServerRequest *request) {
  bool handled = false;
  float oldSpeedSrc = perfSpeedSrc;
  for (size_t i = 0; i < PERF_SETTING_COUNT; i++) {
    if (paramOk(request, PERF_SETTINGS[i].name)) {   // every registry knob is a float: the §10.9 gate applies
      *PERF_SETTINGS[i].ptr = request->getParam(PERF_SETTINGS[i].name)->value().toFloat();
      char key[16];
      snprintf(key, sizeof(key), "%s", PERF_SETTINGS[i].name);  // NVS key = registry name truncated to the 15-char cap
      settingWrite(key, String(*PERF_SETTINGS[i].ptr, 4).c_str());
      handled = true;
    }
  }
  // Changing the speed source (STW↔SOG) invalidates every learned point → Clear-All (spec §1/§4;
  // the dashboard warns first). Both fronts + raw history reset.
  if (request->hasParam("perfSpeedSrc") && perfSpeedSrc != oldSpeedSrc) {
    pendingResetBoatPerformance = true;   // core 1, like the Clear All button: perfFold_tick adds to the fronts there
    queueConsoleMessage("BoatPerf: speed source changed → Clear All (front reset)");
  }
  // Action (not a float knob): LEARNED↔FIXED source toggle.
  if (request->hasParam("perfSource")) {   // 1 = FIXED (freeze + pause), 0 = LEARNED (resume)
    int src = request->getParam("perfSource")->value().toInt();
    // Frozen -> Learning: a frozen curve was never uploaded, so the first sync-back (rebuilt from this unit's
    // cloud rows) would erase it; stage it like an import. Staged while still FIXED so Core 1's fold isn't
    // pushing into the queues being rewritten.
    bool wasFixed = (sailFront.source == 1 || motorFront.source == 1);
    if (wasFixed && !src && (sailFront.count > 0 || motorFront.count > 0)) {
      perfSeedPendingFromFronts();
      perfSentSail = perfSentMotor = 0;
      recSeedOwedSet(2, true);
    }
    sailFront.source = motorFront.source = (uint8_t)(src ? 1 : 0);
    perfPaused = src ? 1.0f : 0.0f;
    settingWrite(NK_perfPaused, String(perfPaused, 4).c_str());
    handled = true;
  }
  return handled;
}
void sendPerfSettings() {
  char buf[320];   // 23 registry floats at %.4f — headroom over the worst case
  int off = 0;
  for (size_t i = 0; i < PERF_SETTING_COUNT; i++) {
    int n = snprintf(buf + off, sizeof(buf) - off, (i ? ",%.4f" : "%.4f"), *PERF_SETTINGS[i].ptr);
    if (n < 0 || off + n >= (int)sizeof(buf)) break;   // truncated echo beats stack overrun (snprintf returns would-be length)
    off += n;
  }
  sseSend(buf, "PerfSettings");
  gxSideOffer(GX_CH_PERFSET, buf, off);
}

// Self-describing schema (/perfschema): the dashboard fetches it ONCE and zips these names against the
// PerfLive / PerfSettings values, so it keeps no field array of its own.
String perfSchemaJson() {
  String s = "{\"live\":[";
  for (size_t i = 0; i < PERF_LIVE_COUNT; i++) {
    if (i) s += ",";
    s += "\""; s += PERF_LIVE[i].name; s += "\"";
  }
  s += "],\"motorLive\":[";
  for (size_t i = 0; i < PERF_MOTOR_LIVE_COUNT; i++) {
    if (i) s += ",";
    s += "\""; s += PERF_MOTOR_LIVE[i].name; s += "\"";
  }
  s += "],\"settings\":[";
  for (size_t i = 0; i < PERF_SETTING_COUNT; i++) {
    if (i) s += ",";
    s += "\""; s += PERF_SETTINGS[i].name; s += "\"";
  }
  s += "]}";
  return s;
}


// ===== VOLTAGE MODE SUPPORT FUNCTIONS =====
void cvLog_init() {
  if (!psramFound()) {
    Serial.println("cvLog: PSRAM not found, disabled");
    cvLogReady = false;
    cvLog = nullptr;
    return;
  }

  cvLog = (CvLogEntry *)heap_caps_malloc(
    CV_LOG_SIZE * sizeof(CvLogEntry),
    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

  if (!cvLog) {
    Serial.println("cvLog: PSRAM malloc failed");
    cvLogReady = false;
    return;
  }

  cvLogHead = 0;
  cvLogCount = 0;
  cvLogReady = true;

  Serial.printf("cvLog: %d entries × %u bytes = %u KB PSRAM\n",
                CV_LOG_SIZE,
                (unsigned)sizeof(CvLogEntry),
                (unsigned)(CV_LOG_SIZE * sizeof(CvLogEntry) / 1024));
}

void cvLog_tick(uint32_t nowMs) {
  // Per-row latches are dropped while no row is written, so the first row after a stop or pause
  // shows only what happened since the previous CH1 read.
  if (!loggingActive || !cvLogReady || !cvLog) {  // Stop Logs: skip append, freeze buffer
    g_cvLogVoltLoopFired = false;
    g_kdTrimThisTick = 0.0f;
    return;
  }
  // if (sysMode != SYS_MODE_AUTO) return;  // this was dumb, probably remove later

  // Pause watchdog — same pattern as thermalLog
  if (cvLogPaused) {
    if ((uint32_t)(nowMs - cvLogPausedAtMs) > THERMAL_LOG_PAUSE_TIMEOUT_MS) {
      Serial.println("cvLog: pause watchdog triggered - resuming");
      cvLogPaused = false;
    } else {
      g_cvLogVoltLoopFired = false;
      g_kdTrimThisTick = 0.0f;
      return;
    }
  }

  CvLogEntry &e = cvLog[cvLogHead];

  e.ts = nowMs;
  // Every narrowed field is clamped (§10.7): (int16_t) of an out-of-range float is undefined, and
  // vErrorMv overflows at 36/48 V whenever the error passes 32.7 V (IBV 0 after boot).
  e.battV = (int16_t)clamp_f(IBV * 100.0f, -32767.0f, 32767.0f);
  e.targV = (int16_t)clamp_f(ChargingVoltageTarget * 100.0f, -32767.0f, 32767.0f);
  e.vErrorMv = (int16_t)clamp_f((ChargingVoltageTarget - IBV) * 1000.0f, -32767.0f, 32767.0f);
  e.dvdt_x1000 = (int16_t)clamp_f(g_fastOvDvdt * 1000.0f, -32767.0f, 32767.0f);
  e.vPred = (int16_t)clamp_f(g_fastOvVpred * 100.0f, -32767.0f, 32767.0f);
  e.fastOvCap = (int16_t)clamp_f(g_fastOvCurrentCap * 10.0f, -32767.0f, 32767.0f);
  e.cv_I_x10 = (int16_t)clamp_f(cv_I * 10.0f, -32767.0f, 32767.0f);
  e.Icv_x10 = (int16_t)clamp_f(Icv * 10.0f, -32767.0f, 32767.0f);
  e.uTarget = (int16_t)clamp_f((float)uTargetAmps * 10.0f, -32767.0f, 32767.0f);
  e.spLimited = (int16_t)clamp_f(setpointLimited * 10.0f, -32767.0f, 32767.0f);
  e.iMeas = (int16_t)clamp_f(MeasuredAmps * 10.0f, -32767.0f, 32767.0f);
  e.duty = (int16_t)clamp_f(dutyCycle * 10.0f, -32767.0f, 32767.0f);

  e.flags = 0;
  if (g_fastOvClampActive) e.flags |= (1 << 0);
  if (g_cvLogVoltLoopFired) e.flags |= (1 << 1);
  if (voltageControlActive) e.flags |= (1 << 2);
  // bit 3 assigned below (iExcess BULK sub-mode)
  if (g_fastOvHardActive) e.flags |= (1 << 4);

  e.awState = g_awState;
  e.rpm = (int16_t)constrain((int)RPM, -32768, 32767);
  e.battV_filt_x100 = (int16_t)clamp_f(IBV_filtered * 100.0f, -32767.0f, 32767.0f);
  e.cvDSlope_x10000 = (int16_t)clamp_f(cvDSlope * 10000.0f, -32767.0f, 32767.0f);
  e.ch1IntervalMs = (int16_t)min((int)g_ch1LastIntervalMs, 32767);   // uint16 sources: clamp, or a >32.7 s gap logs negative
  e.battI_x10 = (int16_t)clamp_f(getBatteryCurrent() * 10.0f, -32767.0f, 32767.0f);
  e.dBcur_dt_Aps = (int16_t)clamp_f(g_dBcur_dt, -32767.0f, 32767.0f);
  e.voltLoopIntervalMs = g_cvLogVoltLoopFired ? (int16_t)min((int)g_voltLoopLogIntervalMs, 32767) : 0;
  g_cvLogVoltLoopFired = false;
  e.inaIntervalMs = (int16_t)min((int)ina_last_ms, 32767);
  e.kdTrim_x1000 = (int16_t)clamp_f(g_kdTrimThisTick * 1000.0f, -32767.0f, 32767.0f);  // signed: <0 only in symmetric mode
  g_kdTrimThisTick = 0.0f;  // clear after logging so a no-trim interval logs 0

  if (g_iExcessBulkActive) e.flags |= (1 << 3);  // iExcess BULK sub-mode (current-control phase)
  if (g_iExcessActive)   e.flags |= (1 << 5);
  if (g_loadDumpActive)  e.flags |= (1 << 6);
  if (g_cvRecovActive)   e.flags |= (1 << 7);    // post-protection integrator refill

  e.capReason = g_fastOvCapReason;  // which layer set the binding fastOvCap this tick
  e.ovFilt_x100 = (int16_t)clamp_f(g_ovIbvFilt * 100.0f, -32767.0f, 32767.0f);
  e.targSlewed_x100 = (int16_t)clamp_f(voltageTargetSlewed * 100.0f, -32767.0f, 32767.0f);
  e.pTerm_x10 = (int16_t)clamp_f(g_cvPTerm * 10.0f, -32767.0f, 32767.0f);      // P contribution to Icv (A); I=cv_I_x10, D=kdTrim_x1000
  e.cvKdFiltV_x100 = (int16_t)clamp_f(g_cvKdFiltV * 100.0f, -32767.0f, 32767.0f);  // D term's dedicated filtered voltage

  // Oscillation damper: the inner-Ki derate is the reason a CV transient can look sluggish here
  // without any gain setting having changed, so it belongs on the same timeline as the loop it moves.
  e.huntDerate_pct = (uint8_t)clamp_f(g_huntDerate * 100.0f, 0.0f, 255.0f);
  e.huntFreq_x100 = (uint8_t)clamp_f(g_huntFreqHz * 100.0f, 0.0f, 255.0f);
  e.huntState = g_huntState;

  cvLogHead = (cvLogHead + 1) % CV_LOG_SIZE;
  if (cvLogCount < CV_LOG_SIZE) cvLogCount++;
}

// Call once per confirmed good CH1 read, at the very top of case 1.
// Runs in the ADC hot path — no Serial, no allocation.
void ch1_record(uint32_t now) {
  if (!ch1HasPrev) {
    ch1PrevTs = now;
    ch1BktStart = now;
    ch1HasPrev = true;
    return;
  }

  uint32_t diff = now - ch1PrevTs;
  ch1PrevTs = now;
  uint16_t iv = (diff > 65535u) ? 65535u : (uint16_t)diff;
  g_ch1LastIntervalMs = iv;  // export to cvLog


  // ── 10s ring ──────────────────────────────────────────────────────────
  ch1Ring[ch1Head] = { now, iv };
  ch1Head = (ch1Head + 1) % CH1_RING;
  if (ch1Count < CH1_RING) ch1Count++;

  // ── All-time accumulators ─────────────────────────────────────────────
  ch1AtCount++;
  ch1AtSum += iv;
  if (iv > ch1AtWorst) ch1AtWorst = iv;
  if (!gpio4IsLow && iv > ch1_worst_fieldon) ch1_worst_fieldon = iv;  // field-on split — the control-relevant worst
  // over2x is per-sample vs the running mean — the SAME test feeds the 1s mini-bucket
  // below, so the 10s / 2m / all-time over-counts are all on the same footing.
  bool isOver2x = false;
  if (ch1AtCount > 1) {
    float runMean = (float)((double)ch1AtSum / ch1AtCount);
    if ((float)iv > runMean * 2.0f) { ch1AtOver2x++; isOver2x = true; }
  }

  // ── 1s mini-bucket: incremental update, O(1), no ring scan ────────────
  ch1Bkt1sCurrent.sum += iv;
  ch1Bkt1sCurrent.count++;
  if (iv > ch1Bkt1sCurrent.worst) ch1Bkt1sCurrent.worst = iv;
  if (isOver2x) ch1Bkt1sCurrent.over2x++;

  // 1s rollover: close current mini-bucket, open a new one
  if (now - ch1Bkt1sStart >= 1000UL) {
    ch1Bkt1s[ch1Bkt1sHead] = ch1Bkt1sCurrent;
    ch1Bkt1sHead = (ch1Bkt1sHead + 1) % CH1_1S_BUCKETS;
    if (ch1Bkt1sCount < CH1_1S_BUCKETS) ch1Bkt1sCount++;
    ch1Bkt1sCurrent = { 0, 0, 0, 0 };
    ch1Bkt1sStart = now;
  }

  // ── 10s→2m bucket rollover: O(10) mini-bucket sum, no ring scan ────────
  // over2x carries forward the per-sample counts accumulated in the 1s mini-buckets
  if (now - ch1BktStart >= 10000UL) {
    Ch1Bucket bkt = { 0, 0, 0, 0 };

    // Sum all closed 1s mini-buckets
    for (uint8_t i = 0; i < ch1Bkt1sCount; i++) {
      uint8_t idx = (ch1Bkt1sHead + CH1_1S_BUCKETS - 1 - i) % CH1_1S_BUCKETS;
      bkt.sum += ch1Bkt1s[idx].sum;
      bkt.count += ch1Bkt1s[idx].count;
      if (ch1Bkt1s[idx].worst > bkt.worst) bkt.worst = ch1Bkt1s[idx].worst;
      bkt.over2x += ch1Bkt1s[idx].over2x;  // per-sample over-2× counts
    }
    // Include currently open mini-bucket
    bkt.sum += ch1Bkt1sCurrent.sum;
    bkt.count += ch1Bkt1sCurrent.count;
    if (ch1Bkt1sCurrent.worst > bkt.worst) bkt.worst = ch1Bkt1sCurrent.worst;
    bkt.over2x += ch1Bkt1sCurrent.over2x;

    ch1Buckets[ch1BktHead] = bkt;
    ch1BktHead = (ch1BktHead + 1) % CH1_BUCKETS;
    if (ch1BktCount < CH1_BUCKETS) ch1BktCount++;
    ch1BktStart = now;
  }
}
void ch1_compute_stats() {
  if (ch1Count == 0) return;

  uint32_t now = millis();
  // ── 10s: O(10) mini-bucket scan — no ring access, no PSRAM thrash ────
  ch1_last_ms = ch1Ring[(ch1Head + CH1_RING - 1) % CH1_RING].iv;  // O(1) single element
  ch1_n_10s = ch1Bkt1sCurrent.count;                              // start with open bucket
  ch1_worst_10s = ch1Bkt1sCurrent.worst;
  uint32_t ch1_o10 = ch1Bkt1sCurrent.over2x;
  uint32_t sum10 = ch1Bkt1sCurrent.sum;

  for (uint8_t i = 0; i < ch1Bkt1sCount; i++) {
    uint8_t idx = (ch1Bkt1sHead + CH1_1S_BUCKETS - 1 - i) % CH1_1S_BUCKETS;
    sum10 += ch1Bkt1s[idx].sum;
    ch1_n_10s += ch1Bkt1s[idx].count;
    ch1_o10 += ch1Bkt1s[idx].over2x;
    if (ch1Bkt1s[idx].worst > ch1_worst_10s) ch1_worst_10s = ch1Bkt1s[idx].worst;
  }
  ch1_over2x_10s = (ch1_o10 > 65535u) ? 65535u : (uint16_t)ch1_o10;  // per-sample count over the 10s window
  if (ch1_n_10s > 0) ch1_avg_10s = (float)sum10 / (float)ch1_n_10s;
  else ch1_avg_10s = 0.0f;

  // ── 2m ───────────────────────────────────────────────────────────────
  ch1_n_2m = 0;
  ch1_worst_2m = 0;
  ch1_over2x_2m = 0;
  ch1_avg_2m = 0;

  uint64_t sum2m = 0;
  for (uint8_t i = 0; i < ch1BktCount; i++) {
    uint8_t idx = (ch1BktHead + CH1_BUCKETS - 1 - i) % CH1_BUCKETS;
    ch1_n_2m += ch1Buckets[idx].count;
    sum2m += ch1Buckets[idx].sum;
    ch1_over2x_2m += ch1Buckets[idx].over2x;
    if (ch1Buckets[idx].worst > ch1_worst_2m) ch1_worst_2m = ch1Buckets[idx].worst;
  }
  if (ch1_n_2m > 0) ch1_avg_2m = (float)sum2m / (float)ch1_n_2m;

  // ── All-time ──────────────────────────────────────────────────────────
  ch1_n_at = ch1AtCount;
  ch1_worst_at = ch1AtWorst;
  ch1_avg_at = ch1AtCount > 0 ? (float)((double)ch1AtSum / ch1AtCount) : 0.0f;
  ch1_over2x_at = ch1AtOver2x;
}

// Effective alternator-current sample rate (Hz): CH1 is where new information enters, so it caps every
// swept-sine measurement. 0 before the first read.
float ch1SampleHz() {
  float avgMs = (ch1_avg_2m > 0.5f) ? ch1_avg_2m : ch1_avg_at;
  return (avgMs > 0.5f) ? (1000.0f / avgMs) : 0.0f;
}

// Highest sweep frequency worth driving: 3 samples/cycle. Above it the lock-in reports the
// alias instead of the plant — repeatably, so it reads as a real high-frequency feature.
float sweepAliasLimitHz() {
  float fs = ch1SampleHz();
  return (fs > 0.0f) ? (fs / 3.0f) : 0.0f;
}

// Inner Current PID Firing Interval — field-on-gated clone of ch1_record/_compute_stats.
// Called once per normal control tick (field driven). Globals live in Xregulator.ino.
void pidFire_record(uint32_t now) {
  if (!pfHasPrev) {
    pfPrevTs = now;
    pfBktStart = now;
    pfBkt1sStart = now;
    pfHasPrev = true;
    return;
  }

  uint32_t diff = now - pfPrevTs;
  pfPrevTs = now;
  uint16_t iv = (diff > 65535u) ? 65535u : (uint16_t)diff;
  pf_last_ms = iv;

  // ── All-time accumulators ─────────────────────────────────────────────
  pfAtCount++;
  pfAtSum += iv;
  if (iv > pfAtWorst) pfAtWorst = iv;
  // over-2× is per-sample vs the running mean — the SAME test feeds the 1s mini-bucket
  // below, so the 10s / 2m / all-time over-counts are all on the same footing.
  bool isOver2x = false;
  if (pfAtCount > 1) {
    float runMean = (float)((double)pfAtSum / pfAtCount);
    if ((float)iv > runMean * 2.0f) { pfAtOver2x++; isOver2x = true; }
  }

  // ── 1s mini-bucket ────────────────────────────────────────────────────
  pfBkt1sCurrent.sum += iv;
  pfBkt1sCurrent.count++;
  if (iv > pfBkt1sCurrent.worst) pfBkt1sCurrent.worst = iv;
  if (isOver2x) pfBkt1sCurrent.over2x++;
  if (now - pfBkt1sStart >= 1000UL) {
    pfBkt1s[pfBkt1sHead] = pfBkt1sCurrent;
    pfBkt1sHead = (pfBkt1sHead + 1) % PF_1S_BUCKETS;
    if (pfBkt1sCount < PF_1S_BUCKETS) pfBkt1sCount++;
    pfBkt1sCurrent = { 0, 0, 0, 0 };
    pfBkt1sStart = now;
  }

  // ── 10s→2m bucket rollover ────────────────────────────────────────────
  if (now - pfBktStart >= 10000UL) {
    Ch1Bucket bkt = { 0, 0, 0, 0 };
    for (uint8_t i = 0; i < pfBkt1sCount; i++) {
      uint8_t idx = (pfBkt1sHead + PF_1S_BUCKETS - 1 - i) % PF_1S_BUCKETS;
      bkt.sum += pfBkt1s[idx].sum;
      bkt.count += pfBkt1s[idx].count;
      if (pfBkt1s[idx].worst > bkt.worst) bkt.worst = pfBkt1s[idx].worst;
      bkt.over2x += pfBkt1s[idx].over2x;
    }
    bkt.sum += pfBkt1sCurrent.sum;
    bkt.count += pfBkt1sCurrent.count;
    if (pfBkt1sCurrent.worst > bkt.worst) bkt.worst = pfBkt1sCurrent.worst;
    bkt.over2x += pfBkt1sCurrent.over2x;
    pfBuckets[pfBktHead] = bkt;
    pfBktHead = (pfBktHead + 1) % PF_BUCKETS;
    if (pfBktCount < PF_BUCKETS) pfBktCount++;
    pfBktStart = now;
  }
}

void pidFire_compute_stats() {
  if (pfAtCount == 0 && pfBkt1sCount == 0 && pfBkt1sCurrent.count == 0) return;

  // ── 10s window: open 1s bucket + closed 1s buckets ────────────────────
  pf_worst_10s = pfBkt1sCurrent.worst;
  uint32_t n10 = pfBkt1sCurrent.count;
  uint32_t sum10 = pfBkt1sCurrent.sum;
  uint32_t o10 = pfBkt1sCurrent.over2x;
  for (uint8_t i = 0; i < pfBkt1sCount; i++) {
    uint8_t idx = (pfBkt1sHead + PF_1S_BUCKETS - 1 - i) % PF_1S_BUCKETS;
    sum10 += pfBkt1s[idx].sum;
    n10 += pfBkt1s[idx].count;
    o10 += pfBkt1s[idx].over2x;
    if (pfBkt1s[idx].worst > pf_worst_10s) pf_worst_10s = pfBkt1s[idx].worst;
  }
  pf_avg_10s = (n10 > 0) ? (float)sum10 / (float)n10 : 0.0f;
  pf_over2x_10s = (o10 > 65535u) ? 65535u : (uint16_t)o10;  // per-sample count over the 10s window

  // ── 2m window: closed 10s buckets ─────────────────────────────────────
  pf_worst_2m = 0;
  pf_over2x_2m = 0;
  uint32_t n2m = 0;
  uint64_t sum2m = 0;
  for (uint8_t i = 0; i < pfBktCount; i++) {
    uint8_t idx = (pfBktHead + PF_BUCKETS - 1 - i) % PF_BUCKETS;
    n2m += pfBuckets[idx].count;
    sum2m += pfBuckets[idx].sum;
    pf_over2x_2m += pfBuckets[idx].over2x;
    if (pfBuckets[idx].worst > pf_worst_2m) pf_worst_2m = pfBuckets[idx].worst;
  }
  pf_avg_2m = (n2m > 0) ? (float)sum2m / (float)n2m : 0.0f;

  // ── All-time ──────────────────────────────────────────────────────────
  pf_worst_at = pfAtWorst;
  pf_avg_at = pfAtCount > 0 ? (float)((double)pfAtSum / pfAtCount) : 0.0f;
  pf_over2x_at = pfAtOver2x;
}

// CV Voltage Loop Firing Interval — CV-mode-gated clone of pidFire_record/_compute_stats.
// Called once per CV fire. Globals live in Xregulator.ino. The control loop clears
// vlHasPrev when CV is inactive, so re-entering CV re-baselines instead of logging the gap.
void voltLoop_record(uint32_t now) {
  if (!vlHasPrev) {
    vlPrevTs = now;
    vlBktStart = now;
    vlBkt1sStart = now;
    vlHasPrev = true;
    return;
  }

  uint32_t diff = now - vlPrevTs;
  vlPrevTs = now;
  uint16_t iv = (diff > 65535u) ? 65535u : (uint16_t)diff;
  vl_last_ms = iv;

  // ── All-time accumulators ─────────────────────────────────────────────
  vlAtCount++;
  vlAtSum += iv;
  if (iv > vlAtWorst) vlAtWorst = iv;
  bool isOver2x = false;
  if (vlAtCount > 1) {
    float runMean = (float)((double)vlAtSum / vlAtCount);
    if ((float)iv > runMean * 2.0f) { vlAtOver2x++; isOver2x = true; }
  }

  // ── 1s mini-bucket ────────────────────────────────────────────────────
  vlBkt1sCurrent.sum += iv;
  vlBkt1sCurrent.count++;
  if (iv > vlBkt1sCurrent.worst) vlBkt1sCurrent.worst = iv;
  if (isOver2x) vlBkt1sCurrent.over2x++;
  if (now - vlBkt1sStart >= 1000UL) {
    vlBkt1s[vlBkt1sHead] = vlBkt1sCurrent;
    vlBkt1sHead = (vlBkt1sHead + 1) % VL_1S_BUCKETS;
    if (vlBkt1sCount < VL_1S_BUCKETS) vlBkt1sCount++;
    vlBkt1sCurrent = { 0, 0, 0, 0 };
    vlBkt1sStart = now;
  }

  // ── 10s→2m bucket rollover ────────────────────────────────────────────
  if (now - vlBktStart >= 10000UL) {
    Ch1Bucket bkt = { 0, 0, 0, 0 };
    for (uint8_t i = 0; i < vlBkt1sCount; i++) {
      uint8_t idx = (vlBkt1sHead + VL_1S_BUCKETS - 1 - i) % VL_1S_BUCKETS;
      bkt.sum += vlBkt1s[idx].sum;
      bkt.count += vlBkt1s[idx].count;
      if (vlBkt1s[idx].worst > bkt.worst) bkt.worst = vlBkt1s[idx].worst;
      bkt.over2x += vlBkt1s[idx].over2x;
    }
    bkt.sum += vlBkt1sCurrent.sum;
    bkt.count += vlBkt1sCurrent.count;
    if (vlBkt1sCurrent.worst > bkt.worst) bkt.worst = vlBkt1sCurrent.worst;
    bkt.over2x += vlBkt1sCurrent.over2x;
    vlBuckets[vlBktHead] = bkt;
    vlBktHead = (vlBktHead + 1) % VL_BUCKETS;
    if (vlBktCount < VL_BUCKETS) vlBktCount++;
    vlBktStart = now;
  }
}

void voltLoop_compute_stats() {
  if (vlAtCount == 0 && vlBkt1sCount == 0 && vlBkt1sCurrent.count == 0) return;

  // ── 10s window: open 1s bucket + closed 1s buckets ────────────────────
  vl_worst_10s = vlBkt1sCurrent.worst;
  uint32_t n10 = vlBkt1sCurrent.count;
  uint32_t sum10 = vlBkt1sCurrent.sum;
  uint32_t o10 = vlBkt1sCurrent.over2x;
  for (uint8_t i = 0; i < vlBkt1sCount; i++) {
    uint8_t idx = (vlBkt1sHead + VL_1S_BUCKETS - 1 - i) % VL_1S_BUCKETS;
    sum10 += vlBkt1s[idx].sum;
    n10 += vlBkt1s[idx].count;
    o10 += vlBkt1s[idx].over2x;
    if (vlBkt1s[idx].worst > vl_worst_10s) vl_worst_10s = vlBkt1s[idx].worst;
  }
  vl_avg_10s = (n10 > 0) ? (float)sum10 / (float)n10 : 0.0f;
  vl_over2x_10s = (o10 > 65535u) ? 65535u : (uint16_t)o10;

  // ── 2m window: closed 10s buckets ─────────────────────────────────────
  vl_worst_2m = 0;
  vl_over2x_2m = 0;
  uint32_t n2m = 0;
  uint64_t sum2m = 0;
  for (uint8_t i = 0; i < vlBktCount; i++) {
    uint8_t idx = (vlBktHead + VL_BUCKETS - 1 - i) % VL_BUCKETS;
    n2m += vlBuckets[idx].count;
    sum2m += vlBuckets[idx].sum;
    vl_over2x_2m += vlBuckets[idx].over2x;
    if (vlBuckets[idx].worst > vl_worst_2m) vl_worst_2m = vlBuckets[idx].worst;
  }
  vl_avg_2m = (n2m > 0) ? (float)sum2m / (float)n2m : 0.0f;

  // ── All-time ──────────────────────────────────────────────────────────
  vl_worst_at = vlAtWorst;
  vl_avg_at = vlAtCount > 0 ? (float)((double)vlAtSum / vlAtCount) : 0.0f;
  vl_over2x_at = vlAtOver2x;
}

// INA228 fast-mode interval tracking
// Mirrors CH1 interval stats. Only updated when inaFastModeActive.
// resetINA228IntervalWindows() clears 10s/2m windows; all-time persists.
struct InaMiniB  { uint32_t sum; uint32_t count; uint16_t worst; uint16_t over2x; };
struct InaBucket { uint32_t sum; uint32_t count; uint16_t worst; uint16_t over2x; };

#define INA_1S_BUCKETS 11
#define INA_BUCKETS    12

static InaMiniB  ina1sB[INA_1S_BUCKETS];
static uint8_t   ina1sHead  = 0;
static uint8_t   ina1sCount = 0;
static InaMiniB  ina1sCur   = {0, 0, 0};
static uint32_t  ina1sStart = 0;

static InaBucket ina2mB[INA_BUCKETS];
static uint8_t   ina2mHead  = 0;
static uint8_t   ina2mCount = 0;
static uint32_t  ina2mStart = 0;

static uint64_t  inaAtSum   = 0;
static uint32_t  inaAtCount = 0;
// Deliberately no inaAtWorst intermediate: one once yielded ina_worst_at=0 beside nonzero over2x/avg (cause
// never found), so worst writes directly to ina_worst_at. Same for ina_worst_2m.
static uint32_t  inaAtOver2x = 0;
static uint32_t  inaPrevRead = 0;

void resetINA228IntervalWindows() {
  memset(ina1sB, 0, sizeof(ina1sB));
  ina1sHead = 0; ina1sCount = 0;
  ina1sCur  = {0, 0, 0};
  ina1sStart = millis();
  memset(ina2mB, 0, sizeof(ina2mB));
  ina2mHead = 0; ina2mCount = 0;
  ina2mStart = millis();
  inaPrevRead = 0;
  ina_last_ms = 0;
  ina_avg_10s = 0.0f; ina_worst_10s = 0; ina_over2x_10s = 0;
  ina_avg_2m  = 0.0f; ina_worst_2m  = 0; ina_over2x_2m  = 0;
}

// Full reset incl. all-time accumulators (web Reset Peaks); resetINA228IntervalWindows() clears only 10s/2m.
void resetINA228AllStats() {
  resetINA228IntervalWindows();
  inaAtSum     = 0;
  inaAtCount   = 0;
  inaAtOver2x  = 0;
  ina_avg_at   = 0.0f;
  ina_worst_at = 0;
  ina_over2x_at = 0;
}

// loop() calls this every field-gate-low pass (INA twin of the pf tracker's pfHasPrev reset): a field-off flash
// write can stall the loop before inaFastModeActive flips to slow, and that gap would log as a fast-mode interval.
void inaResetIntervalBaseline() { inaPrevRead = 0; }

void recordINA228Interval(uint32_t now) {
  if (inaPrevRead == 0) { inaPrevRead = now; return; }

  uint32_t diff = now - inaPrevRead;
  inaPrevRead = now;
  uint16_t iv = (diff > 65535u) ? 65535u : (uint16_t)diff;
  ina_last_ms = iv;

  // All-time accumulators (avg + over2x via running mean).
  // Worst is written DIRECTLY to the published variable — no intermediate.
  inaAtCount++;
  inaAtSum += iv;
  if (iv > ina_worst_at) ina_worst_at = iv;
  // ina_worst_2m is also written directly, so it means "max since last fast-mode rising edge", not a rolling
  // 2m window (avg + over2x stay rolling). Tooltips should say "since fast-mode start".
  if (iv > ina_worst_2m) ina_worst_2m = iv;
  bool isOver2x = false;
  if (inaAtCount > 1) {
    // Bias correction: compute mean of prior samples only, otherwise a huge
    // outlier inflates its own mean and fails the > 2× test against itself.
    float runMean = (float)((double)(inaAtSum - iv) / (inaAtCount - 1));
    if ((float)iv > runMean * 2.0f) { inaAtOver2x++; isOver2x = true; }
  }

  // 1s mini-bucket (over2x counted per-sample so 10s / 2m / all-time agree)
  ina1sCur.sum += iv;
  ina1sCur.count++;
  if (iv > ina1sCur.worst) ina1sCur.worst = iv;
  if (isOver2x) ina1sCur.over2x++;

  if (now - ina1sStart >= 1000UL) {
    ina1sB[ina1sHead] = ina1sCur;
    ina1sHead = (ina1sHead + 1) % INA_1S_BUCKETS;
    if (ina1sCount < INA_1S_BUCKETS) ina1sCount++;
    ina1sCur  = {0, 0, 0};
    ina1sStart = now;
  }

  // 10s→2m bucket rollover
  if (now - ina2mStart >= 10000UL) {
    InaBucket bkt = {0, 0, 0, 0};
    for (uint8_t i = 0; i < ina1sCount; i++) {
      uint8_t idx = (ina1sHead + INA_1S_BUCKETS - 1 - i) % INA_1S_BUCKETS;
      bkt.sum   += ina1sB[idx].sum;
      bkt.count += ina1sB[idx].count;
      if (ina1sB[idx].worst > bkt.worst) bkt.worst = ina1sB[idx].worst;
      bkt.over2x += ina1sB[idx].over2x;  // per-sample over-2× counts
    }
    bkt.sum   += ina1sCur.sum;
    bkt.count += ina1sCur.count;
    if (ina1sCur.worst > bkt.worst) bkt.worst = ina1sCur.worst;
    bkt.over2x += ina1sCur.over2x;
    ina2mB[ina2mHead] = bkt;
    ina2mHead = (ina2mHead + 1) % INA_BUCKETS;
    if (ina2mCount < INA_BUCKETS) ina2mCount++;
    ina2mStart = now;
  }

  // Publish 10s stats
  uint32_t sum10 = ina1sCur.sum, n10 = ina1sCur.count, o10 = ina1sCur.over2x;
  ina_worst_10s = ina1sCur.worst;
  for (uint8_t i = 0; i < ina1sCount; i++) {
    uint8_t idx = (ina1sHead + INA_1S_BUCKETS - 1 - i) % INA_1S_BUCKETS;
    sum10 += ina1sB[idx].sum;
    n10   += ina1sB[idx].count;
    o10   += ina1sB[idx].over2x;
    if (ina1sB[idx].worst > ina_worst_10s) ina_worst_10s = ina1sB[idx].worst;
  }
  ina_avg_10s    = (n10 > 0) ? (float)sum10 / (float)n10 : 0.0f;
  ina_over2x_10s = (o10 > 65535u) ? 65535u : (uint16_t)o10;  // per-sample count over the 10s window

  // Publish 2m avg + over2x only; ina_worst_2m is updated directly above.
  uint32_t n2m = 0;
  uint64_t sum2m = 0;
  ina_over2x_2m  = 0;
  for (uint8_t i = 0; i < ina2mCount; i++) {
    uint8_t idx = (ina2mHead + INA_BUCKETS - 1 - i) % INA_BUCKETS;
    n2m          += ina2mB[idx].count;
    sum2m        += ina2mB[idx].sum;
    ina_over2x_2m += ina2mB[idx].over2x;
  }
  ina_avg_2m = (n2m > 0) ? (float)sum2m / (float)n2m : 0.0f;

  // Publish all-time avg + over2x (ina_worst_at is updated directly above).
  ina_avg_at    = (inaAtCount > 0) ? (float)((double)inaAtSum / inaAtCount) : 0.0f;
  ina_over2x_at = inaAtOver2x;
}

// Web assets are gzip, named *.gz, served Content-Encoding: gzip. NEVER brotli —
// Chrome/Firefox/Android WebView reject brotli over plain HTTP (ERR_CONTENT_DECODING_FAILED)
// and the LAN UI is plain-HTTP.
void cacheWebAssets() {
  cachedIndex    = loadFileToRAM("/index.html.gz");
  cachedCss      = loadFileToRAM("/styles.css.gz");
  cachedJs       = loadFileToRAM("/script.js.gz");
  cachedUplotCss = loadFileToRAM("/uPlot.min.css.gz");
  cachedUplotJs  = loadFileToRAM("/uPlot.iife.min.js.gz");
}
bool serveCachedAsset(AsyncWebServerRequest *request, const String &path, const String &contentType) {
  CachedAsset *cf = nullptr;
  if (path == "/index.html") cf = &cachedIndex;
  else if (path == "/styles.css") cf = &cachedCss;
  else if (path == "/script.js") cf = &cachedJs;
  else if (path == "/uPlot.min.css") cf = &cachedUplotCss;
  else if (path == "/uPlot.iife.min.js") cf = &cachedUplotJs;

  if (cf && cf->data && cf->size > 0) {
    // Every asset revalidates (no-cache): a cached index.html renders a live-looking dashboard off an unreachable
    // device, and the five assets are a MATCHED SET (stale CSS against new markup breaks layout). loadFileToRAM
    // always mints an ETag, so revalidation is a bodyless 304.
    const char *cacheHdr = "no-cache";

    if (cf->etag[0] && request->header("If-None-Match").indexOf(cf->etag) >= 0) {
      AsyncWebServerResponse *r304 = request->beginResponse(304);
      r304->addHeader("ETag", cf->etag);
      r304->addHeader("Cache-Control", cacheHdr);
      request->send(r304);
      return true;
    }

    uint8_t *data = cf->data;
    size_t len = cf->size;
    AsyncWebServerResponse *resp = request->beginResponse(
      contentType, len,
      [data, len](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
        size_t remaining = len - index;
        size_t toSend = min(maxLen, remaining);
        memcpy(buffer, data + index, toSend);
        return toSend;
      });
    resp->addHeader("Content-Encoding", "gzip");
    resp->addHeader("Cache-Control", cacheHdr);
    if (cf->etag[0]) resp->addHeader("ETag", cf->etag);
    request->send(resp);
    return true;
  }
  return false;
}

// The duty setDutyPercent writes for a commissioning override's command this tick. The field ceilings are
// re-solved in buildTickSnapshot before any override runs and the overrides bypass the floor, so this IS the clamp.
static float dutyAfterCeiling(float cmd) {
  return fminf(cmd, fminf(ccDutyCeiling(), 99.0f));
}

// A g_fieldCeilBind term as the owner sees it: a setting by its Setup label, or the regulator's own limit.
static const char *fieldCeilBindName(uint8_t bind) {
  switch (bind) {
    case 1:  return "Max Field Volts";
    case 2:  return "the regulator's 10 A field-current limit";
    case 3:
    case 4:  return "the regulator's field-drive heat limit";
    default: return "Max Field (%)";
  }
}
// Short form for chcAbortMsg's 128 bytes.
static const char *fieldCeilBindBrief(uint8_t bind) {
  switch (bind) {
    case 1:  return "Max Field Volts";
    case 2:  return "regulator's own 10 A limit";
    case 3:
    case 4:  return "regulator's own heat limit";
    default: return "Max Field (%)";
  }
}
static const char *fieldCeilBindRemedy(uint8_t bind) {
  switch (bind) {
    case 1:  return "Check Field Winding Voltage in Vessel Info against the alternator's datasheet. If Max Field Volts is under the rated field voltage, raise it (Setup > Alternator > Basic > Output Control) and re-run.";
    case 2:  return "It protects the regulator, and no setting raises it.";
    case 3:
    case 4:  return "It protects the regulator, no setting raises it, and it recovers as the regulator cools.";
    default: return "Raise Max Field (%) (Setup > Alternator > Basic > Output Control) and re-run.";
  }
}

// True while one of the regulator's own ceilings (not an owner setting) holds the field: duty within a point of
// ccDutyCeiling(), the same "pegged" test the limiter code uses.
static bool boardCeilHoldsField() {
  return g_fieldCeilBind >= 2 && lastAppliedDuty >= ccDutyCeiling() - 1.0f;
}

// Plant delay step test: BASELINE + 3x (UP/DOWN), each held SYSID_STEP_HOLD_MS; PSRAM buffer allocated at test start,
// freed back at IDLE. Called per fresh CH1 hit from AdjustFieldLearnMode; caller MUST force govMode = GOV_BYPASS_SLEW and the PID
// to MANUAL with its integrator reset to dutyOut. Fall threshold is step-relative; a -1 ms delay = no crossing found.

bool systemID_tick(float &dutyOut, float ampsRaw, uint32_t nowMs) {
  // Phase enum. Values 1–9 map to phaseStartMs indices 0–8 via (phase – 1).
  // STABILIZE(1) runs first and is handled with its own early-return block.
  enum SysIDPhase : uint8_t {
    SYSID_IDLE        = 0,
    SYSID_STABILIZE   = 1,
    SYSID_BASELINE    = 2,
    SYSID_UP_1        = 3,
    SYSID_DOWN_1      = 4,
    SYSID_UP_2        = 5,
    SYSID_DOWN_2      = 6,
    SYSID_UP_3        = 7,
    SYSID_DOWN_3      = 8,
    SYSID_PROCESSING  = 9,
    SYSID_SINE        = 10,  // open-loop sine sweep (plant Bode) — replaces the UP/DOWN sequence
    SYSID_EASE        = 11   // ramp duty down gently on exit so protections don't trip on a fast collapse
  };

  static SysIDPhase phase = SYSID_IDLE;
  static float baseDuty = 0.0f;
  static uint32_t holdMs = 0;
  static bool bufFullWarned = false;
  static uint32_t stabilizeLastAdjMs = 0;   // last 1Hz duty adjustment in STABILIZE
  // Rolling 5-sample ring buffer for average-based settle check (1Hz sampling)
  static float stabRing[SYSID_STABILIZE_SAMPLES];
  static uint8_t stabRingIdx = 0;
  static uint8_t stabRingCount = 0;

  // phaseStartMs[0..8]: STABILIZE[0] BASELINE[1] UP_1[2] DOWN_1[3]
  //                     UP_2[4] DOWN_2[5] UP_3[6] DOWN_3[7] test-end[8]
  static uint32_t phaseStartMs[9] = { 0 };

  // Sine-sweep state (open-loop plant Bode). curTestType is captured at start so a
  // mid-test settings change can't switch the running test.
  static uint8_t  curTestType = 0;
  static float    sineFreq[SYSID_SINE_NPOINTS];
  static uint32_t sineSegStartMs[SYSID_SINE_NPOINTS];
  static uint32_t sineSweepEndMs = 0;
  static uint8_t  sineIdx = 0;
  static bool     sineSegStarted = false;
  static uint32_t lastSineSampleMs = 0;   // decimation clock so a long/low-freq sweep can't overflow the buffer
  static uint32_t sysidEaseStartMs = 0;   // EASE phase: gentle field ramp-down on exit
  static float    sysidEaseFromDuty = 0.0f;
  static double   sidRpmSum = 0.0;        // engine-speed stats over the sine phase → systemIDRpm* globals
  static uint32_t sidRpmN   = 0;

  // One-shot debug on request arrival
  static bool lastReqState = false;
  if (systemIDRequested && !lastReqState) {
    Serial.printf("SystemID: REQUEST SEEN | phase=%d sysMode=%d lastAppliedDuty=%.1f\n",
                  phase, sysMode, lastAppliedDuty);
  }
  lastReqState = systemIDRequested;

  // ── EASE-OUT: ramp to baseDuty over ~1.5 s, holding the override (and protection gate) until settled so the
  // fast current collapse doesn't trip a protection. ──
  if (phase == SYSID_EASE) {
    float frac = (float)(nowMs - sysidEaseStartMs) / 1500.0f;
    if (frac >= 1.0f) {
      systemIDLastEndMs = millis();
      phase = SYSID_IDLE;
      dutyOut = baseDuty;
      return false;
    }
    dutyOut = sysidEaseFromDuty + (baseDuty - sysidEaseFromDuty) * frac;
    return true;
  }

  // ── Ignore re-triggers while a test is already running ──────────────────
  if (phase != SYSID_IDLE && systemIDRequested) {
    systemIDRequested = false;
    queueConsoleMessage("SystemID: re-trigger ignored — test already in progress");
  }

  // ── Abort check ─────────────────────────────────────────────────────────
  if (phase != SYSID_IDLE && systemIDAbortRequested) {
    systemIDAbortRequested = false;
    queueConsoleMessage("SystemID: test aborted");
    commitSystemIDRecord(true);  // log aborted run to ring buffer for fleet visibility
    systemIDActive = 0;
    phase = SYSID_IDLE;
    stabilizeLastAdjMs = 0;
    stabRingIdx = 0;
    stabRingCount = 0;
    dutyOut = lastAppliedDuty;
    return false;
  }

  // ── IDLE: wait for trigger ───────────────────────────────────────────────
  if (phase == SYSID_IDLE) {
    if (!systemIDRequested) {
      if (sysIDBuffer) {   // results live in the record rings and systemIDBode; nothing reads raw samples after the run
        free(sysIDBuffer);
        sysIDBuffer = nullptr;
        sysIDSampleCount = 0;
      }
      dutyOut = lastAppliedDuty;
      return false;
    }
    systemIDRequested = false;
    systemIDAbortRequested = false;   // clear any stale abort that arrived after previous test ended

    Serial.printf("SystemID: starting | sysMode=%d lastAppliedDuty=%.1f\n",
                  sysMode, lastAppliedDuty);

    // Allocate PSRAM buffer on first use
    if (sysIDBuffer == nullptr) {
      sysIDBuffer = (SystemIDSample *)ps_malloc(SYSID_BUF_SIZE * sizeof(SystemIDSample));
      if (sysIDBuffer == nullptr) {
        Serial.println("SystemID: ABORTED — PSRAM alloc failed");
        queueConsoleMessage("SystemID: ABORTED — PSRAM alloc failed");
        dutyOut = lastAppliedDuty;
        return false;
      }
      Serial.printf("SystemID: PSRAM alloc OK — %d bytes\n",
                    SYSID_BUF_SIZE * (int)sizeof(SystemIDSample));
    }

    // Initialise test state
    memset(phaseStartMs, 0, sizeof(phaseStartMs));
    sysIDSampleCount = 0;
    bufFullWarned = false;
    stabilizeLastAdjMs = 0;
    stabRingIdx = 0;
    stabRingCount = 0;
    systemIDResultsReady = false;
    sidRpmSum = 0.0; sidRpmN = 0;
    systemIDRpmMin = systemIDRpmMax = systemIDRpmAvg = 0.0f;
    baseDuty = lastAppliedDuty;
    holdMs = SYSID_STEP_HOLD_MS;

    // Capture test type for the whole run; build the log-spaced sweep frequency list.
    curTestType = systemIDTestType;
    if (curTestType == 1) {
      float fLo = fmaxf(0.1f, systemIDSineFreqStart);
      float fHi = fmaxf(fLo + 0.1f, systemIDSineFreqEnd);
      // 24 samples/cycle off sysIDBuffer only re-reads the same held CH1 value — the sensor,
      // not the sampler, sets what this sweep can resolve.
      systemIDFsHz = ch1SampleHz();
      float fAlias = sweepAliasLimitHz();
      if (fAlias > 0.5f && fHi > fAlias) {
        queueConsoleMessageF("SystemID sweep: top trimmed %.1f→%.1f Hz — current sensor only samples at %.0f Hz",
                             fHi, fAlias, systemIDFsHz);
        fHi = fmaxf(fLo + 0.1f, fAlias);
      }
      for (int i = 0; i < SYSID_SINE_NPOINTS; i++) {
        float frac = (float)i / (float)(SYSID_SINE_NPOINTS - 1);
        sineFreq[i] = fLo * powf(fHi / fLo, frac);
      }
      sineIdx = 0;
      sineSegStarted = false;
      systemIDBodeCount = 0;
      lastSineSampleMs = 0;
    }

    queueConsoleMessageF(
      "SystemID: stabilizing to %.0fA | step=+%.1f%% holdMs=%u",
      SystemIDStabilizeAmps, SystemIDStepAmplitude, holdMs);

    phaseStartMs[0] = nowMs;  // STABILIZE start
    phase = SYSID_STABILIZE;
    systemIDActive = (uint8_t)SYSID_STABILIZE;
  }

  // ── STABILIZE phase: 1 Hz P-control to SystemIDStabilizeAmps until the 5 s rolling average is within band ──
  if (phase == SYSID_STABILIZE) {
    if (nowMs - stabilizeLastAdjMs >= 1000) {
      float err = SystemIDStabilizeAmps - ampsRaw;
      baseDuty = constrain(baseDuty + err * 0.5f * fieldScale(), 5.0f * fieldScale(), 80.0f);   // 0.5 %/A and the 5 % floor are 12 V values
      // Push ampsRaw into the ring buffer on each 1Hz duty update
      stabRing[stabRingIdx] = ampsRaw;
      stabRingIdx = (stabRingIdx + 1) % SYSID_STABILIZE_SAMPLES;
      if (stabRingCount < SYSID_STABILIZE_SAMPLES) stabRingCount++;
      stabilizeLastAdjMs = nowMs;

      // Once we have a full 5-second window, check if the average is within band
      if (stabRingCount >= SYSID_STABILIZE_SAMPLES) {
        float sum = 0;
        for (uint8_t i = 0; i < SYSID_STABILIZE_SAMPLES; i++) sum += stabRing[i];
        float avg = sum / SYSID_STABILIZE_SAMPLES;
        if (fabsf(avg - SystemIDStabilizeAmps) < SYSID_STABILIZE_BAND_A) {
          stabRingIdx = 0;
          stabRingCount = 0;
          stabilizeLastAdjMs = 0;
          if (curTestType == 1) {
            // Sine sweep: skip the UP/DOWN sequence, go straight to the swept-sine phase.
            sineIdx = 0;
            sineSegStarted = false;
            phase = SYSID_SINE;
            systemIDActive = (uint8_t)SYSID_SINE;
            queueConsoleMessageF(
              "SystemID: sine sweep — %d pts %.1f–%.1f Hz, %d cycles/pt, amp=%.1f%% duty",
              SYSID_SINE_NPOINTS, sineFreq[0], sineFreq[SYSID_SINE_NPOINTS - 1],
              systemIDSineCycles, SystemIDStepAmplitude);
            Serial.printf("SystemID: SINE SWEEP\n");
          } else {
            phaseStartMs[1] = nowMs;  // BASELINE start
            phase = SYSID_BASELINE;
            systemIDActive = (uint8_t)SYSID_BASELINE;
            queueConsoleMessageF(
              "SystemID: 5s avg=%.1fA (duty=%.1f%%) within %.0fA of target — starting baseline | holdMs=%u",
              avg, baseDuty, SYSID_STABILIZE_BAND_A, holdMs);
            Serial.printf("SystemID: BASELINE\n");
          }
        }
      }
    }
    dutyOut = baseDuty;

    if ((nowMs - phaseStartMs[0]) >= SYSID_STABILIZE_TIMEOUT_MS) {
      stabilizeLastAdjMs = 0;
      stabRingIdx = 0;
      stabRingCount = 0;
      queueConsoleMessageF(
        "SystemID: ABORTED — could not stabilize at %.0fA within %us "
        "(last reading: %.1fA duty=%.1f%%)",
        SystemIDStabilizeAmps, SYSID_STABILIZE_TIMEOUT_MS / 1000,
        ampsRaw, baseDuty);
      systemIDAbortReason = 254;             // sentinel: stabilize-phase timeout (outside FieldEventReason enum)
      systemIDAbortPhase  = systemIDActive;  // current phase before we clear it
      commitSystemIDRecord(true);            // log the timeout-abort to ring buffer
      systemIDActive = 0;
      phase = SYSID_IDLE;
      dutyOut = baseDuty;
      return false;
    }

    return true;
  }

  // ── SINE SWEEP phase (open-loop plant Bode), PID off ──
  // baseDuty is the TROUGH of amp·(1+sin), so the swing is entirely upward and current can't clip at the bottom.
  // Each tone runs 1 settle + N analysed cycles; the PROCESSING lock-in DFT extracts gain and phase.
  if (phase == SYSID_SINE) {
    if (!sineSegStarted) {
      sineSegStartMs[sineIdx] = nowMs;
      sineSegStarted = true;
    }
    float f = sineFreq[sineIdx];
    float tSec = (float)(nowMs - sineSegStartMs[sineIdx]) / 1000.0f;
    float drive = SystemIDStepAmplitude * (1.0f + sinf(2.0f * (float)M_PI * f * tSec));
    float d = constrain(baseDuty + drive, 0.0f, 100.0f);
    dutyOut = d;

    // A field ceiling under the crest clips the wave: the fundamental shrinks, K reads low and the proposed
    // gains run hot. A clip deeper than 5% of the amplitude (under 1% gain error) ends the run unfitted.
    const float dApplied = dutyAfterCeiling(d);
    if (d - dApplied > fmaxf(0.05f, 0.05f * SystemIDStepAmplitude)) {
      queueConsoleMessageF("SystemID: ABORTED at %.2f Hz: %s (%.1f%% duty) clipped the sine peak (%.1f%% commanded). A clipped wave reads as low plant gain, so nothing was fitted.",
                           f, fieldCeilBindName(g_fieldCeilBind), dApplied, d);
      systemIDAbortReason = 253;             // sentinel: a field ceiling clipped the sine (outside FieldEventReason enum)
      systemIDAbortPhase  = systemIDActive;
      commitSystemIDRecord(true);
      systemIDActive = 0;
      sysidEaseFromDuty = dApplied;
      sysidEaseStartMs = nowMs;
      phase = SYSID_EASE;
      dutyOut = dApplied;
      return true;
    }

    // Engine-speed stats over the sine phase — advisory context for /sysidbode, never gated on.
    float rNow = (float)RPM;
    if (sidRpmN == 0) { systemIDRpmMin = rNow; systemIDRpmMax = rNow; }
    else {
      if (rNow < systemIDRpmMin) systemIDRpmMin = rNow;
      if (rNow > systemIDRpmMax) systemIDRpmMax = rNow;
    }
    sidRpmSum += rNow; sidRpmN++;
    systemIDRpmAvg = (float)(sidRpmSum / sidRpmN);

    // Frequency-adaptive decimation, ~24 samples per cycle, so a long low-frequency start can't exhaust the
    // 15000-sample buffer and starve the high tones. The DFT uses real timestamps, so a non-uniform rate is harmless.
    uint32_t sineSampleIntervalMs = (uint32_t)(1000.0f / (f * 24.0f));  // period/24
    if ((uint32_t)(nowMs - lastSineSampleMs) >= sineSampleIntervalMs) {
      lastSineSampleMs = nowMs;
      if (sysIDBuffer && sysIDSampleCount < SYSID_BUF_SIZE) {
        sysIDBuffer[sysIDSampleCount++] = { nowMs, d, ampsRaw };
      }
    }

    // Hold = 1 settle cycle + N analysed cycles, floored at 1.5 s for the fastest tones.
    float segMs = (1.0f + (float)systemIDSineCycles) * 1000.0f / f;
    if (segMs < 1500.0f) segMs = 1500.0f;
    if ((uint32_t)(nowMs - sineSegStartMs[sineIdx]) >= (uint32_t)segMs) {
      sineIdx++;
      sineSegStarted = false;
      if (sineIdx >= SYSID_SINE_NPOINTS) {
        sineSweepEndMs = nowMs;
        phase = SYSID_PROCESSING;
        systemIDActive = (uint8_t)SYSID_PROCESSING;
        queueConsoleMessageF("SystemID: sine sweep complete — %d samples, post-processing", sysIDSampleCount);
        Serial.println("SystemID: sine sweep complete — post-processing");
      }
    }
    return true;
  }

  // ── Determine commanded duty for current phase ───────────────────────────
  bool isUpPhase = (phase == SYSID_UP_1 || phase == SYSID_UP_2 || phase == SYSID_UP_3);
  float phaseDuty = isUpPhase
                      ? constrain(baseDuty + SystemIDStepAmplitude, 0.0f, 100.0f)
                      : baseDuty;
  dutyOut = phaseDuty;

  // ── Record sample ────────────────────────────────────────────────────────
  if (sysIDBuffer && sysIDSampleCount < SYSID_BUF_SIZE) {
    sysIDBuffer[sysIDSampleCount++] = { nowMs, phaseDuty, ampsRaw };
  } else if (sysIDBuffer) {
    if (!bufFullWarned) {
      bufFullWarned = true;
      queueConsoleMessageF("SystemID: WARNING — sample buffer full at %d samples. "
                           "Post-processing will use truncated data. "
                           "Increase SYSID_BUF_SIZE or reduce call rate.",
                           SYSID_BUF_SIZE);
      Serial.printf("SystemID: buffer full at %d samples\n", SYSID_BUF_SIZE);
    }
  }

  // ── Phase advance ────────────────────────────────────────────────────────
  // phase enum starts at 1, phaseStartMs index = phase - 1.
  uint32_t elapsed = nowMs - phaseStartMs[phase - 1];
  if (elapsed >= holdMs) {
    switch (phase) {
      case SYSID_BASELINE:
        phaseStartMs[2] = nowMs;
        phase = SYSID_UP_1;
        systemIDActive = (uint8_t)SYSID_UP_1;
        queueConsoleMessageF("SystemID: BASELINE complete (%ums) — entering UP 1 | duty=%.1f%% amps=%.1fA",
                             elapsed, phaseDuty + SystemIDStepAmplitude, ampsRaw);
        Serial.printf("SystemID: UP 1\n");
        break;
      case SYSID_UP_1:
        phaseStartMs[3] = nowMs;
        phase = SYSID_DOWN_1;
        systemIDActive = (uint8_t)SYSID_DOWN_1;
        queueConsoleMessageF("SystemID: UP 1 complete (%ums) — entering DOWN 1 | amps=%.1fA",
                             elapsed, ampsRaw);
        Serial.printf("SystemID: DOWN 1\n");
        break;
      case SYSID_DOWN_1:
        phaseStartMs[4] = nowMs;
        phase = SYSID_UP_2;
        systemIDActive = (uint8_t)SYSID_UP_2;
        queueConsoleMessageF("SystemID: DOWN 1 complete (%ums) — entering UP 2 | amps=%.1fA",
                             elapsed, ampsRaw);
        Serial.printf("SystemID: UP 2\n");
        break;
      case SYSID_UP_2:
        phaseStartMs[5] = nowMs;
        phase = SYSID_DOWN_2;
        systemIDActive = (uint8_t)SYSID_DOWN_2;
        queueConsoleMessageF("SystemID: UP 2 complete (%ums) — entering DOWN 2 | amps=%.1fA",
                             elapsed, ampsRaw);
        Serial.printf("SystemID: DOWN 2\n");
        break;
      case SYSID_DOWN_2:
        phaseStartMs[6] = nowMs;
        phase = SYSID_UP_3;
        systemIDActive = (uint8_t)SYSID_UP_3;
        queueConsoleMessageF("SystemID: DOWN 2 complete (%ums) — entering UP 3 | amps=%.1fA",
                             elapsed, ampsRaw);
        Serial.printf("SystemID: UP 3\n");
        break;
      case SYSID_UP_3:
        phaseStartMs[7] = nowMs;
        phase = SYSID_DOWN_3;
        systemIDActive = (uint8_t)SYSID_DOWN_3;
        queueConsoleMessageF("SystemID: UP 3 complete (%ums) — entering DOWN 3 | amps=%.1fA",
                             elapsed, ampsRaw);
        Serial.printf("SystemID: DOWN 3\n");
        break;
      case SYSID_DOWN_3:
        phaseStartMs[8] = nowMs;  // test end timestamp
        phase = SYSID_PROCESSING;
        systemIDActive = (uint8_t)SYSID_PROCESSING;
        queueConsoleMessageF("SystemID: DOWN 3 complete (%ums) — %d samples collected, post-processing",
                             elapsed, sysIDSampleCount);
        Serial.println("SystemID: data collection complete — post-processing");
        break;
      default:
        break;
    }
  }

  // ── Post-processing (runs immediately when PROCESSING is entered) ────────
  // Rise i: quiet phase = BASELINE[1]/DOWN_1[3]/DOWN_2[5], UP start = [2]/[4]/[6], UP end = [3]/[5]/[7].
  // Fall i: preceding UP = [2]/[4]/[6], DOWN start = [3]/[5]/[7]; [8] = test end.

  // ── Sine-sweep post-process (lock-in DFT per frequency) ──────────────────
  // Correlating (amps - mean) against sin/cos at each exact frequency rejects other tones (incl. belt ripple).
  // gain = output A per %duty; phase = output lag in degrees.
  if (phase == SYSID_PROCESSING && curTestType == 1) {
    systemIDBodeCount = 0;
    float prevLagDeg = 0.0f;   // anchors the ±180° unwrap; lag only grows across a sweep
    for (int p = 0; p < SYSID_SINE_NPOINTS; p++) {
      float f = sineFreq[p];
      uint32_t segStart = sineSegStartMs[p];
      uint32_t segEnd   = (p + 1 < SYSID_SINE_NPOINTS) ? sineSegStartMs[p + 1] : sineSweepEndMs;
      uint32_t anaStart = segStart + (uint32_t)(1000.0f / f);  // skip 1 settle cycle
      // Truncate the analysis span to a WHOLE number of drive periods so the lock-in's negative-frequency
      // leakage term cancels exactly.
      float periodMs = 1000.0f / f;
      uint32_t availMs = (segEnd > anaStart) ? (segEnd - anaStart) : 0;
      int nCyc = (int)floorf((float)availMs / periodMs);
      uint32_t anaEnd = (nCyc >= 1) ? (anaStart + (uint32_t)((float)nCyc * periodMs)) : segEnd;

      // Pass 1: window mean (removes DC so a partial trailing cycle can't bias the fit).
      double mean = 0.0; int nMean = 0;
      for (int s = 0; s < sysIDSampleCount; s++) {
        uint32_t ts = sysIDBuffer[s].ts;
        if (ts < anaStart) continue;
        if (ts >= anaEnd) break;
        mean += sysIDBuffer[s].amps; nMean++;
      }
      if (nMean > 0) mean /= (double)nMean;

      // Pass 2: single-bin DFT (lock-in) of (amps − mean) against the drive frequency.
      double I = 0.0, Q = 0.0; int n = 0;
      for (int s = 0; s < sysIDSampleCount; s++) {
        uint32_t ts = sysIDBuffer[s].ts;
        if (ts < anaStart) continue;
        if (ts >= anaEnd) break;
        float t = (float)(ts - segStart) / 1000.0f;
        float ang = 2.0f * (float)M_PI * f * t;
        double y = (double)sysIDBuffer[s].amps - mean;
        I += y * sinf(ang);
        Q += y * cosf(ang);
        n++;
      }
      float gain = 0.0f, phaseDeg = 0.0f;
      if (n > 0 && SystemIDStepAmplitude > 0.01f) {
        float B = 2.0f * (float)sqrt(I * I + Q * Q) / (float)n;   // output amplitude (A)
        gain = B / SystemIDStepAmplitude;                         // A per %duty
        phaseDeg = atan2f(-(float)Q, (float)I) * 180.0f / (float)M_PI;  // output lag (deg)
        // Unfold atan2's ±180° branch cut — the dashboard fits dead time off this number, and a
        // folded point carries the largest ω weight in that fit.
        while (phaseDeg < prevLagDeg - 180.0f) phaseDeg += 360.0f;
        while (phaseDeg > prevLagDeg + 180.0f) phaseDeg -= 360.0f;
        prevLagDeg = phaseDeg;
      }
      systemIDBode[p].freqHz    = f;
      systemIDBode[p].gainApPct = gain;
      systemIDBode[p].phaseDeg  = phaseDeg;
      systemIDBodeCount++;
      queueConsoleMessageF("Bode %d: %.2f Hz gain=%.3f A/%% lag=%.0f deg (n=%d)", p + 1, f, gain, phaseDeg, n);
    }
    commitSysidSweepRecord();
    systemIDResultsReady = true;
    systemIDActive = 0;
    systemIDLastEndMs = millis();
    phase = SYSID_IDLE;
    dutyOut = baseDuty;
    return false;
  }

  if (phase == SYSID_PROCESSING) {

    const uint8_t quietIdx[3] = { 1, 3, 5 };  // phaseStartMs index of pre-rise quiet phase
    const uint8_t upIdx[3] = { 2, 4, 6 };     // phaseStartMs index of UP phase start
    const uint8_t upEndIdx[3] = { 3, 5, 7 };  // phaseStartMs index of UP phase end
    const uint8_t downIdx[3] = { 3, 5, 7 };   // phaseStartMs index of DOWN phase start

    const uint32_t REF_WINDOW_MS = 2000;  // last 2 seconds of each phase used as reference

    // Threshold = reference mean ± 2x noise half-amplitude (max/min vs mean): just outside the noise floor,
    // close to the true transport delay, and a single spike can't move it the way raw max/min could.
    float quietMeanArr[3] = { 0.0f, 0.0f, 0.0f };
    float quietMaxArr[3]  = { 0.0f, 0.0f, 0.0f };
    float upMeanArr[3]    = { 0.0f, 0.0f, 0.0f };
    float upMinArr[3]     = { 0.0f, 0.0f, 0.0f };

    // ── Rise delays ─────────────────────────────────────────────────────
    for (int i = 0; i < 3; i++) {
      uint32_t t_quiet_start = phaseStartMs[quietIdx[i]];
      uint32_t t_up_start    = phaseStartMs[upIdx[i]];
      uint32_t t_up_end      = phaseStartMs[upEndIdx[i]];

      uint32_t quietRefStart = (t_up_start > REF_WINDOW_MS)
                                 ? (t_up_start - REF_WINDOW_MS)
                                 : t_quiet_start;
      float quietSum = 0.0f;
      float quietMax = -1.0e9f;
      float quietMin =  1.0e9f;
      int quietSamples = 0;
      for (int s = 0; s < sysIDSampleCount; s++) {
        if (sysIDBuffer[s].ts < quietRefStart) continue;
        if (sysIDBuffer[s].ts >= t_up_start) break;
        quietSum += sysIDBuffer[s].amps;
        if (sysIDBuffer[s].amps > quietMax) quietMax = sysIDBuffer[s].amps;
        if (sysIDBuffer[s].amps < quietMin) quietMin = sysIDBuffer[s].amps;
        quietSamples++;
      }

      if (quietSamples == 0) {
        queueConsoleMessageF("SystemID rise %d: WARNING — no samples in 2s quiet ref window "
                             "(quietRefStart=%u t_up_start=%u). threshold unreliable.",
                             i + 1, quietRefStart, t_up_start);
        Serial.printf("SystemID rise %d: no samples in quiet ref window\n", i + 1);
      }
      float quietMean = (quietSamples > 0) ? (quietSum / quietSamples) : 0.0f;
      quietMeanArr[i] = quietMean;
      quietMaxArr[i]  = quietMax;

      uint32_t upRefStart = (t_up_end > REF_WINDOW_MS)
                              ? (t_up_end - REF_WINDOW_MS)
                              : t_up_start;
      float upSum = 0.0f;
      float upMin = 1.0e9f;
      int upSamples = 0;
      for (int s = 0; s < sysIDSampleCount; s++) {
        if (sysIDBuffer[s].ts < upRefStart) continue;
        if (sysIDBuffer[s].ts >= t_up_end) break;
        upSum += sysIDBuffer[s].amps;
        if (sysIDBuffer[s].amps < upMin) upMin = sysIDBuffer[s].amps;
        upSamples++;
      }

      float upMean = (upSamples > 0) ? (upSum / upSamples) : quietMean;
      upMeanArr[i] = upMean;
      upMinArr[i]  = upMin;

      // Signal quality metrics sent to UI for noise / amplitude quality check.
      systemIDStepAmp_A[i] = fmaxf(0.0f, upMean - quietMean);
      systemIDQuietPP_A[i] = (quietSamples > 0) ? fmaxf(0.0f, quietMax - quietMin) : 0.0f;

      // Rise threshold: quiet mean + 2x noise half-amplitude (noise floored at 0.5 A).
      float quietNoise = fmaxf(quietMax - quietMean, 0.5f);
      float riseThresh = quietMean + 2.0f * quietNoise;

      // First sample after UP command that crosses above threshold.
      systemIDRiseDelay_ms[i] = -1.0f;
      for (int s = 0; s < sysIDSampleCount; s++) {
        if (sysIDBuffer[s].ts < t_up_start) continue;
        if (sysIDBuffer[s].ts >= t_up_end) break;
        if (sysIDBuffer[s].amps > riseThresh) {
          systemIDRiseDelay_ms[i] = (float)(sysIDBuffer[s].ts - t_up_start);
          break;
        }
      }

      if (systemIDRiseDelay_ms[i] < 0.0f) {
        queueConsoleMessageF("SystemID rise %d: NOT FOUND | quietMean=%.1fA quietMax=%.1fA noise=%.2fA thresh=%.1fA — "
                             "current never crossed threshold during UP phase. "
                             "Step amplitude may be too small or sensor not responding.",
                             i + 1, quietMean, quietMax, quietNoise, riseThresh);
      } else {
        queueConsoleMessageF("SystemID rise %d | quietMean=%.1fA quietMax=%.1fA noise=%.2fA thresh=%.1fA delay=%.0f ms",
                             i + 1, quietMean, quietMax, quietNoise, riseThresh, systemIDRiseDelay_ms[i]);
      }
      Serial.printf("SystemID rise %d | quietMean=%.1fA quietMax=%.1fA noise=%.2fA thresh=%.1fA delay=%.0f ms\n",
                    i + 1, quietMean, quietMax, quietNoise, riseThresh, systemIDRiseDelay_ms[i]);
    }

    // ── Fall delays ─────────────────────────────────────────────────────
    for (int i = 0; i < 3; i++) {
      uint32_t t_down_start = phaseStartMs[downIdx[i]];
      uint32_t t_down_end   = phaseStartMs[downIdx[i] + 1];  // bounded: [4],[6],[8]=test-end

      // Reuse statistics computed in the rise loop — same reference windows.
      float upMean = upMeanArr[i];
      float upMin  = upMinArr[i];

      // Fall threshold: UP mean - 2x noise half-amplitude (floored at 0.5 A), symmetric to the rise.
      float upNoise  = fmaxf(upMean - upMin, 0.5f);
      float fallThresh = upMean - 2.0f * upNoise;

      // First sample after DOWN command that crosses below threshold.
      // Scan bounded by t_down_end so it cannot bleed into the next UP phase or beyond.
      systemIDFallDelay_ms[i] = -1.0f;
      for (int s = 0; s < sysIDSampleCount; s++) {
        if (sysIDBuffer[s].ts < t_down_start) continue;
        if (sysIDBuffer[s].ts >= t_down_end) break;
        if (sysIDBuffer[s].amps < fallThresh) {
          systemIDFallDelay_ms[i] = (float)(sysIDBuffer[s].ts - t_down_start);
          break;
        }
      }

      if (systemIDFallDelay_ms[i] < 0.0f) {
        queueConsoleMessageF("SystemID fall %d: NOT FOUND | upMean=%.1fA upMin=%.1fA noise=%.2fA thresh=%.1fA — "
                             "current never dropped below threshold in DOWN window. "
                             "Step amplitude may be too small or sensor not responding.",
                             i + 1, upMean, upMin, upNoise, fallThresh);
      } else {
        queueConsoleMessageF("SystemID fall %d | upMean=%.1fA upMin=%.1fA noise=%.2fA thresh=%.1fA delay=%.0f ms",
                             i + 1, upMean, upMin, upNoise, fallThresh, systemIDFallDelay_ms[i]);
      }
      Serial.printf("SystemID fall %d | upMean=%.1fA upMin=%.1fA noise=%.2fA thresh=%.1fA delay=%.0f ms\n",
                    i + 1, upMean, upMin, upNoise, fallThresh, systemIDFallDelay_ms[i]);
    }

    // ── Averages (skip -1 not-found entries) ────────────────────────────
    float riseSum = 0.0f;
    int riseN = 0;
    float fallSum = 0.0f;
    int fallN = 0;
    for (int i = 0; i < 3; i++) {
      if (systemIDRiseDelay_ms[i] >= 0.0f) {
        riseSum += systemIDRiseDelay_ms[i];
        riseN++;
      }
      if (systemIDFallDelay_ms[i] >= 0.0f) {
        fallSum += systemIDFallDelay_ms[i];
        fallN++;
      }
    }
    systemIDRiseAvg_ms = (riseN > 0) ? riseSum / (float)riseN : -1.0f;
    systemIDFallAvg_ms = (fallN > 0) ? fallSum / (float)fallN : -1.0f;

    // ── Summarise outcome ────────────────────────────────────────────────
    if (riseN == 0 && fallN == 0) {
      queueConsoleMessage("SystemID: FAILED — no threshold crossings detected for rise or fall. "
                          "Check ampsRaw signal, SystemIDStepAmplitude, and sensor wiring.");
    } else if (riseN == 0) {
      queueConsoleMessageF("SystemID: WARNING — rise crossings not found (%d/3). "
                           "Fall avg=%.0f ms. Results partial.",
                           3 - riseN, systemIDFallAvg_ms);
    } else if (fallN == 0) {
      queueConsoleMessageF("SystemID: WARNING — fall crossings not found (%d/3). "
                           "Rise avg=%.0f ms. Results partial.",
                           3 - fallN, systemIDRiseAvg_ms);
    }

    queueConsoleMessageF(
      "SystemID results | Rise: %.0f %.0f %.0f avg=%.0f ms | Fall: %.0f %.0f %.0f avg=%.0f ms | samples=%d",
      systemIDRiseDelay_ms[0], systemIDRiseDelay_ms[1], systemIDRiseDelay_ms[2], systemIDRiseAvg_ms,
      systemIDFallDelay_ms[0], systemIDFallDelay_ms[1], systemIDFallDelay_ms[2], systemIDFallAvg_ms,
      sysIDSampleCount);
    Serial.printf(
      "SystemID results | Rise: %.0f %.0f %.0f avg=%.0f ms | Fall: %.0f %.0f %.0f avg=%.0f ms | samples=%d\n",
      systemIDRiseDelay_ms[0], systemIDRiseDelay_ms[1], systemIDRiseDelay_ms[2], systemIDRiseAvg_ms,
      systemIDFallDelay_ms[0], systemIDFallDelay_ms[1], systemIDFallDelay_ms[2], systemIDFallAvg_ms,
      sysIDSampleCount);

    commitSystemIDRecord(false);  // log successful run to ring buffer
    systemIDResultsReady = true;
    systemIDActive = 0;
    // Don't snap back to normal control — ease the field down first (see SYSID_EASE above). The
    // override stays held during the ramp so protections re-arm only once the current has settled.
    sysidEaseFromDuty = dutyOut;   // dutyOut entered as lastAppliedDuty (the last sweep/step value)
    sysidEaseStartMs = nowMs;
    phase = SYSID_EASE;
    return true;
  }

  return true;  // test still in progress
}


// Invert the field-% curve: given a target current (A), linear-interpolate the duty (%).
// Assumes fieldCurveBuf[] is ascending in amps (it is — a monotonic duty ramp).
static float fieldCurveInvert(float targetA) {
  if (fieldCurveCount <= 0) return FIELDCURVE_DUTY_START;
  if (targetA <= fieldCurveBuf[0].amps) return fieldCurveBuf[0].duty;
  for (int i = 1; i < fieldCurveCount; i++) {
    if (targetA <= fieldCurveBuf[i].amps) {
      float da = fieldCurveBuf[i].amps - fieldCurveBuf[i - 1].amps;
      if (da <= 0.001f) return fieldCurveBuf[i].duty;
      float t = (targetA - fieldCurveBuf[i - 1].amps) / da;
      return fieldCurveBuf[i - 1].duty + t * (fieldCurveBuf[i].duty - fieldCurveBuf[i - 1].duty);
    }
  }
  return fieldCurveBuf[fieldCurveCount - 1].duty;
}

// fieldCut_tick() — commissioning stage 7: field de-energize τ
// RAMP (real current PID, returns false) -> HOLD (frozen duty, high cal point) -> CUT to MinDuty, the floor a
// real OV clamp drives to (decay from the 20 kSPS ring, ADS fallback) -> PROCESS (results publish HERE so a late
// abort can't discard them) -> EASE. Drain time IS RPM-dependent, so the wizard runs one per Min%-floor speed.
// fieldCutRpm averages the hold tail: the cut corrupts the LM2907 tach ~4.6 s, so both RPM gates are masked
// through FIELDCUT_RPM_GRACE_MS. Field-at-MinDuty is the SAFE direction.

static float fcMeanMv(const int16_t *b, int n) {
  if (!b || n <= 0) return NAN;
  double s = 0;
  for (int i = 0; i < n; i++) s += b[i];
  return (float)(s / n);
}

// Two-point fast-channel calibration from this run's own settled levels. ADS1115 is the reference
// (hold ≈ stabilize amps, tail ≈ residual at MinDuty). Solves ampsCal = gain × nominal + off and
// persists it. Rejects a degenerate span or an implausible solution and keeps the previous cal.
static void fcCalibrateFast(float faHighMv, float faLowMv) {
  if (isnan(faHighMv) || isnan(faLowMv)) return;
  float nomHigh = faMvToAmpsNominal(faHighMv), nomLow = faMvToAmpsNominal(faLowMv);
  float span = nomHigh - nomLow;
  if (span < 5.0f) {
    queueConsoleMessageF("Field-cut: fast-channel cal skipped (span %.1f A too small)", span);
    return;
  }
  float g = (fieldCutBaseA - fieldCutFloorA) / span;
  float o = fieldCutBaseA - g * nomHigh;
  if (g < 0.5f || g > 2.0f || fabsf(o) > 30.0f) {
    queueConsoleMessageF("Field-cut: fast-channel cal rejected (gain %.3f off %.2f A) — keeping previous", g, o);
    return;
  }
  faCalGain = g; faCalOffA = o;
  settingWrite(NK_faCalGain, String(faCalGain, 4).c_str());
  settingWrite(NK_faCalOffA, String(faCalOffA, 3).c_str());
  queueConsoleMessageF("Field-cut: fast-channel calibrated — gain %.3f, offset %.2f A (vs ADS %.1f/%.1f A)",
                       g, o, fieldCutBaseA, fieldCutFloorA);
}

// Shared τ fit + fall-time on a filtered, floor-referenced decay trace (t ms, amps). tCmd = cut command instant in
// THIS trace's time base (NaN = unknown). Fit band 85% -> 10% of (base - floor). Also fills the plot arrays, t
// re-zeroed to tCmd so the plot origin matches the drain number.
static bool fcFitDecay(const float *tMs, const float *amps, int n, int minFitPts, float tCmd) {
  float base = fieldCutBaseA, floorA = fieldCutFloorA;
  float dA = base - floorA;
  if (dA < FIELDCUT_MIN_BASE_A) {
    snprintf(fieldCutAbortMsg, sizeof(fieldCutAbortMsg), "output too low to measure — raise engine speed a little and re-run");
    return false;
  }
  const float lv95 = floorA + 0.95f * dA, lv90 = floorA + 0.90f * dA,
              lv85 = floorA + 0.85f * dA, lv50 = floorA + 0.50f * dA, lv10 = floorA + 0.10f * dA;
  // Crossings anchor on the 50% level: plateau rectifier ripple (±20% on the bench) can fake an early 85/90/95%
  // edge but never reaches 50%. So 85/90/95 are the LAST crossings before i50, 10% the first after.
  int i50 = -1;
  for (int i = 0; i < n; i++) { if (amps[i] < lv50) { i50 = i; break; } }
  if (i50 <= 0) {
    snprintf(fieldCutAbortMsg, sizeof(fieldCutAbortMsg), "no clean decay found (field may not have cut) — re-run");
    return false;
  }
  float t95 = NAN, t90 = NAN, t10 = NAN;
  int i85 = 0, i10 = n - 1;
  for (int i = i50; i >= 1; i--) {
    float a0 = amps[i - 1], a1 = amps[i];
    if (isnan(t90) && a0 >= lv90 && a1 < lv90) t90 = tMs[i - 1] + (tMs[i] - tMs[i - 1]) * (a0 - lv90) / fmaxf(1e-6f, a0 - a1);
    if (isnan(t95) && a0 >= lv95 && a1 < lv95) { t95 = tMs[i - 1] + (tMs[i] - tMs[i - 1]) * (a0 - lv95) / fmaxf(1e-6f, a0 - a1); break; }
  }
  for (int i = i50; i >= 1; i--) { if (amps[i - 1] >= lv85) { i85 = i; break; } }
  bool found10 = false;
  for (int i = i50 + 1; i < n; i++) {
    float a0 = amps[i - 1], a1 = amps[i];
    if (a0 >= lv10 && a1 < lv10) { t10 = tMs[i - 1] + (tMs[i] - tMs[i - 1]) * (a0 - lv10) / fmaxf(1e-6f, a0 - a1); i10 = i; found10 = true; break; }
  }
  if (!found10) {
    snprintf(fieldCutAbortMsg, sizeof(fieldCutAbortMsg), "no clean decay found (field may not have cut) — re-run");
    return false;
  }
  double sx = 0, sy = 0, sxx = 0, sxy = 0; int nf = 0;
  for (int i = i85; i < n; i++) {
    float ex = amps[i] - floorA;
    if (ex < 0.10f * dA) break;          // below the fit band — ripple around the floor is not decay
    if (ex > 0.85f * dA) continue;
    double x = (double)tMs[i] / 1000.0, y = log((double)ex);
    sx += x; sy += y; sxx += x * x; sxy += x * y; nf++;
  }
  if (nf < minFitPts) {
    snprintf(fieldCutAbortMsg, sizeof(fieldCutAbortMsg), "decay too fast for this sensor to resolve — re-run");
    return false;
  }
  double d = (double)nf * sxx - sx * sx;
  if (fabs(d) < 1e-12) { snprintf(fieldCutAbortMsg, sizeof(fieldCutAbortMsg), "fit failed — re-run"); return false; }
  double slope = ((double)nf * sxy - sx * sy) / d;   // d(ln(I−floor))/dt (1/s); a real decay is negative
  double icept = (sy - slope * sx) / nf;
  double tauMs = (slope < 0.0) ? (-1000.0 / slope) : -1.0;
  if (tauMs < 2.0 || tauMs > 500.0) {
    snprintf(fieldCutAbortMsg, sizeof(fieldCutAbortMsg), "no clean decay found (field may not have cut) — re-run");
    return false;
  }
  double ss = 0; int nr = 0;
  for (int i = i85; i < n; i++) {
    float ex = amps[i] - floorA;
    if (ex < 0.10f * dA) break;
    if (ex > 0.85f * dA) continue;
    double x = (double)tMs[i] / 1000.0;
    double e = log((double)ex) - (icept + slope * x);
    ss += e * e; nr++;
  }
  fieldCutResidPct = nr ? (float)((exp(sqrt(ss / nr)) - 1.0) * 100.0) : -1.0f;
  fieldCutTauMs = (float)tauMs;
  fieldCutFallMs = (!isnan(t90)) ? (t10 - t90) : -1.0f;
  // Drain + plot zero anchor to the cut COMMAND (tCmd): the OV release waits from its own cut command, so dead time
  // belongs in the number (fallback: 95% crossing, then i85). Drain = anchor -> 10% read off the trace, model-free;
  // for a clean coil drain ~ dead-time + 2.3τ, which cross-checks the fit.
  float t0 = !isnan(t95) ? t95 : tMs[i85];
  if (!isnan(tCmd) && tCmd >= tMs[0] && tCmd < t10) t0 = tCmd;
  fieldCutDrainMs = t10 - t0;
  int iStart = 0;
  while (iStart < i85 && tMs[iStart] < t0 - 50.0f) iStart++;
  int iEnd = (i10 < n) ? i10 : n - 1;
  float tEnd = tMs[iEnd] + 100.0f;
  while (iEnd < n - 1 && tMs[iEnd + 1] <= tEnd) iEnd++;
  int range = iEnd - iStart + 1;
  int stride = (range + FIELDCUT_PLOT_N - 1) / FIELDCUT_PLOT_N;
  if (stride < 1) stride = 1;
  fcPlotN = 0;
  for (int i = iStart; i <= iEnd && fcPlotN < FIELDCUT_PLOT_N; i += stride) {
    fcPlotMs[fcPlotN] = tMs[i] - t0;
    fcPlotA[fcPlotN] = amps[i];
    fcPlotN++;
  }
  return true;
}

// Post-capture processing: calibrate, filter, fit. faHighMv/faLowMv are the hold-end / tail-end
// fast-ring means (NAN when the channel wasn't available).
static void fieldCutProcess(float faHighMv, float faLowMv) {
  fieldCutOk = false;
  fcPlotN = 0;
  fcCalibrateFast(faHighMv, faLowMv);

  // ── Fast path: 20 kSPS ring -> boxcar-16 decimate (0.8 ms) -> zero-phase EMA (τ≈4 ms, fwd+back so no lag bias
  //   in the fitted slope). Residual rectifier ripple is multiplicative, so it averages out in log space. ──
  if (fcFastCount >= 4000 && fcPlotMs && fcPlotA) {
    int nD = fcFastCount / 16;
    static float *dT = nullptr, *dA = nullptr;   // decimated workspace, PSRAM, allocated once
    if (!dT) dT = (float *)ps_malloc((FIELDCUT_FAST_N / 16 + 1) * sizeof(float));
    if (!dA) dA = (float *)ps_malloc((FIELDCUT_FAST_N / 16 + 1) * sizeof(float));
    if (dT && dA) {
      const float dtMs = 16.0f * 1000.0f / 20000.0f;   // 0.8 ms per decimated sample
      for (int i = 0; i < nD; i++) {
        double s = 0;
        const int16_t *p = fcFastBuf + i * 16;
        for (int k = 0; k < 16; k++) s += p[k];
        dA[i] = faMvToAmps((float)(s / 16.0));
        dT[i] = i * dtMs;
      }
      const float alpha = 0.1667f;   // EMA τ ≈ 4 ms at 0.8 ms samples — well under the ~40 ms decay
      for (int i = 1; i < nD; i++) dA[i] = dA[i - 1] + alpha * (dA[i] - dA[i - 1]);
      for (int i = nD - 2; i >= 0; i--) dA[i] = dA[i + 1] + alpha * (dA[i] - dA[i + 1]);
      // Last decimated sample = the grab moment; walk back the measured grab offset to the cut command.
      float tCmd = (nD > 0) ? (dT[nD - 1] - fcSnapElapsedMs) : NAN;
      if (fcFitDecay(dT, dA, nD, 20, tCmd)) {
        fieldCutSrc = 1;
        fieldCutOk = true;
      }
    }
  }

  // ── ADS fallback: channel absent/dormant (or the fast fit failed) — fit the ~10-30 ms control-tick
  //   samples instead. Far fewer points; minFitPts drops accordingly. ──
  if (!fieldCutOk && fieldCutCount >= 4) {
    static float *aT = nullptr, *aA = nullptr;
    if (!aT) aT = (float *)ps_malloc(FIELDCUT_BUF_SIZE * sizeof(float));
    if (!aA) aA = (float *)ps_malloc(FIELDCUT_BUF_SIZE * sizeof(float));
    if (aT && aA) {
      for (int i = 0; i < fieldCutCount; i++) { aT[i] = (float)fieldCutBuf[i].tMs; aA[i] = fieldCutBuf[i].amps; }
      if (fcFitDecay(aT, aA, fieldCutCount, 4, 0.0f)) {   // ADS tMs is already cut-relative (el = 0 at the cut)
        fieldCutSrc = 0;
        fieldCutOk = true;
        queueConsoleMessage("Field-cut: fast channel unavailable — fitted on ADS samples (coarse)");
      }
    }
  }

  if (fieldCutOk) {
    fieldCutAbortMsg[0] = '\0';   // a failed fast fit may have latched a reason before the fallback succeeded
    queueConsoleMessageF("Field-cut: drain(cut->10%%)=%.0f ms @ %.0f RPM, tau=%.0f ms, 90-10 fall=%.0f ms (base %.1f A, floor %.1f A, resid %.0f%%, %s)",
                         fieldCutDrainMs, fieldCutRpm, fieldCutTauMs, fieldCutFallMs, fieldCutBaseA, fieldCutFloorA, fieldCutResidPct,
                         fieldCutSrc ? "20k channel" : "ADS");
  }
}

bool fieldCut_tick(float &dutyOut, float ampsRaw, uint32_t nowMs) {
  static uint8_t  phase = 0;             // 0=idle 1=CC ramp/settle 2=hold(frozen duty) 3=cut+tail 4=ease
  static uint32_t phaseStartMs = 0;
  static float    entryDuty = 0.0f;      // pre-test operating duty (rest floor / AUTO) — eased back to on exit
  static float    freezeDuty = 0.0f;     // duty captured when the real loop settled; FROZEN through hold + snapshot
  static float    fcPreSetpoint = 0.0f;  // setpointLimited before the test — restored so the resume snapshot is clean
  static float    ampsEma = 0.0f;        // settle detector input
  static uint32_t arriveMs = 0, inBandSinceMs = 0;
  static double   adsHoldSum = 0.0;  static uint32_t adsHoldN = 0;   // hold-tail baseline (high cal ref)
  static double   rpmHoldSum = 0.0;  static uint32_t rpmHoldN = 0;   // hold-tail RPM (pre-cut — the cut corrupts the tach)
  static double   adsTailSum = 0.0;  static uint32_t adsTailN = 0;   // cut-tail floor (low cal ref)
  // Cal points average MATCHED multi-second windows on both sensors (four spaced 500 ms ring grabs vs the ADS mean
  // over the same span, steady conditions) so ripple, noise and fixed pipeline delay average out.
  static double   faHiSum = 0.0, faLoSum = 0.0;
  static uint8_t  faHiN = 0, faLoN = 0, holdGrabs = 0, tailGrabs = 0;
  static bool     snapped = false;

  // ── Abort ────────────────────────────────────────────────────────────────
  if (phase != 0 && fieldCutAbortRequested) {
    fieldCutAbortRequested = false;
    fieldCutCcActive = false;
    if (fieldCutResultsReady) {
      // Fit already published (abort landed in the ease) — a finished measurement is kept, the
      // abort only ends the wind-down early. Clear any protection reason so the UI shows the result.
      fieldCutAbortMsg[0] = '\0';
    } else if (fieldCutAbortMsg[0] == '\0') {
      // A protection abort latches its reason into the msg before this tick runs — don't overwrite it.
      snprintf(fieldCutAbortMsg, sizeof(fieldCutAbortMsg), "aborted");
    }
    fieldCutActive = 0; fieldCutPhase = 0; fieldCutLastEndMs = millis();
    phase = 0; dutyOut = entryDuty;
    return false;
  }

  // ── IDLE: wait for trigger (do NOT touch dutyOut — SystemID may own it) ───
  if (phase == 0) {
    if (!fieldCutRequested) return false;
    fieldCutRequested = false;
    fieldCutAbortRequested = false;
    if (fieldCutBuf == nullptr) fieldCutBuf = (FieldCutSample *)ps_malloc(FIELDCUT_BUF_SIZE * sizeof(FieldCutSample));
    if (fcFastBuf == nullptr) fcFastBuf = (int16_t *)ps_malloc(FIELDCUT_FAST_N * sizeof(int16_t));
    if (fcScratch == nullptr) fcScratch = (int16_t *)ps_malloc(FIELDCUT_FAST_N * sizeof(int16_t));
    if (fcPlotMs == nullptr) fcPlotMs = (float *)ps_malloc(FIELDCUT_PLOT_N * sizeof(float));
    if (fcPlotA == nullptr) fcPlotA = (float *)ps_malloc(FIELDCUT_PLOT_N * sizeof(float));
    if (!fieldCutBuf || !fcFastBuf || !fcScratch || !fcPlotMs || !fcPlotA) {
      snprintf(fieldCutAbortMsg, sizeof(fieldCutAbortMsg), "PSRAM alloc failed");
      fieldCutOk = false; fieldCutResultsReady = true;
      return false;
    }
    entryDuty = fmaxf(dutyOut, fieldScale());   // where the field sat pre-test, floored at 1 % x fieldScale() — eased back to on exit
    fcPreSetpoint = setpointLimited;
    fieldCutCmdA = constrain(SystemIDStabilizeAmps, 10.0f, 100.0f);
    fieldCutCount = 0; fcFastCount = 0; fcPlotN = 0;
    adsHoldSum = 0.0; adsHoldN = 0; rpmHoldSum = 0.0; rpmHoldN = 0; adsTailSum = 0.0; adsTailN = 0;
    faHiSum = 0.0; faLoSum = 0.0; faHiN = 0; faLoN = 0; holdGrabs = 0; tailGrabs = 0; snapped = false;
    ampsEma = ampsRaw; arriveMs = 0; inBandSinceMs = 0;
    fieldCutTauMs = -1.0f; fieldCutFallMs = -1.0f; fieldCutDrainMs = -1.0f; fieldCutBaseA = 0.0f; fieldCutFloorA = 0.0f;
    fieldCutRpm = 0.0f;
    fieldCutResidPct = -1.0f; fieldCutSrc = 0;
    fieldCutResultsReady = false; fieldCutOk = false; fieldCutAbortMsg[0] = '\0';
    fieldCutActive = 1; fieldCutPhase = 0;
    fieldCutCcActive = true;   // hand the setpoint to the real current loop (branch in AdjustField)
    phase = 1; phaseStartMs = nowMs;
    queueConsoleMessageF("Field-cut: ramping to %.0f A through the current loop", fieldCutCmdA);
    return false;   // ramp phase does NOT own duty — the real PID does
  }

  // ── RAMP/SETTLE: the fieldCutCcActive branch and real PID own duty; here we only WATCH until the setpoint has
  //   arrived and current has sat in band for FIELDCUT_SETTLE_MS, then freeze the applied duty. ──
  if (phase == 1) {
    fieldCutPhase = 0;
    ampsEma += 0.05f * (ampsRaw - ampsEma);
    bool arrived = fabsf(setpointLimited - fieldCutCmdA) < 0.5f;
    if (arrived && arriveMs == 0) arriveMs = nowMs;
    float band = fmaxf(3.0f, 0.08f * fieldCutCmdA);
    bool settled = false;
    if (arrived && fabsf(ampsEma - fieldCutCmdA) < band) {
      if (inBandSinceMs == 0) inBandSinceMs = nowMs;
      settled = (nowMs - inBandSinceMs >= FIELDCUT_SETTLE_MS);
    } else {
      inBandSinceMs = 0;
    }
    // Target unreachable (e.g. commissioned level above what the alternator makes at idle): the PID
    // rails, current plateaus below band. Proceed with whatever real output we got — the fit only
    // needs a baseline clear of the floor. Truly no output → fail with a plain reason.
    if (!settled && (nowMs - phaseStartMs >= FIELDCUT_SETTLE_TIMEOUT_MS)) {
      if (ampsEma >= FIELDCUT_MIN_BASE_A) {
        settled = true;
        queueConsoleMessageF("Field-cut: %.0f A not reachable here — proceeding at %.1f A", fieldCutCmdA, ampsEma);
      } else {
        fieldCutCcActive = false;
        snprintf(fieldCutAbortMsg, sizeof(fieldCutAbortMsg), "no charging output at this speed — re-run once the alternator is producing");
        fieldCutOk = false; fieldCutResultsReady = true;
        fieldCutActive = 0; fieldCutPhase = 0; fieldCutLastEndMs = millis();
        phase = 0;
        return false;
      }
    }
    if (settled) {
      freezeDuty = lastAppliedDuty;
      fieldCutCcActive = false;
      // Restore the pre-test setpoint BEFORE returning true: the SystemID resume snapshot fires on this tick's
      // rising edge and must not capture the test target, or test end would re-assert the test current.
      setpointLimited = fcPreSetpoint;
      queueConsoleMessageF("Field-cut: settled at %.1f A — freezing duty %.1f%%", ampsEma, freezeDuty);
      phase = 2; phaseStartMs = nowMs;
      fieldCutPhase = 1;
      dutyOut = freezeDuty;
      return true;
    }
    return false;
  }

  // ── HOLD: duty FROZEN at freezeDuty for FIELDCUT_HOLD_MS. Baseline = ADS mean over the last 2.5 s; high cal
  //   point = four spaced 500 ms ring grabs across the same span. ──
  if (phase == 2) {
    dutyOut = freezeDuty; fieldCutPhase = 1;
    uint32_t el = nowMs - phaseStartMs;
    if (el >= FIELDCUT_HOLD_MS - 2500UL) {
      adsHoldSum += ampsRaw; adsHoldN++;
      if (RPM > 0.0f) { rpmHoldSum += RPM; rpmHoldN++; }
    }
    if (holdGrabs < 4 && el >= 2500UL + 800UL * holdGrabs) {
      int n = faGrabRecent(fcScratch, FIELDCUT_FAST_N);
      if (n >= 2000) { faHiSum += fcMeanMv(fcScratch, n); faHiN++; }
      holdGrabs++;
    }
    if (el >= FIELDCUT_HOLD_MS) {
      fieldCutBaseA = (adsHoldN > 0) ? (float)(adsHoldSum / adsHoldN) : ampsRaw;
      fieldCutRpm = (rpmHoldN > 0) ? (float)(rpmHoldSum / rpmHoldN) : 0.0f;
      phase = 3; phaseStartMs = nowMs;
    }
    return true;
  }

  // ── CUT + TAIL: duty at MinDuty (the real OV-clamp floor) for FIELDCUT_MEAS_MS. Fast ring grabbed FIELDCUT_SNAP_MS
  //   after the cut (ADS recorded as fallback); floor + low cal point from the final 2.5 s, well after the drain. ──
  if (phase == 3) {
    dutyOut = MinDuty; fieldCutPhase = 2;
    uint32_t el = nowMs - phaseStartMs;
    if (el <= 2500 && fieldCutCount < FIELDCUT_BUF_SIZE) {
      fieldCutBuf[fieldCutCount].tMs = el;
      fieldCutBuf[fieldCutCount].amps = ampsRaw;
      fieldCutCount++;
    }
    if (!snapped && el >= FIELDCUT_SNAP_MS) {
      fcFastCount = faGrabRecent(fcFastBuf, FIELDCUT_FAST_N);
      fcSnapElapsedMs = (float)el;   // grab lands one tick past FIELDCUT_SNAP_MS — record the true offset so the command maps exactly into the ring copy
      snapped = true;
    }
    if (el >= FIELDCUT_MEAS_MS - 2500UL) { adsTailSum += ampsRaw; adsTailN++; }
    // Four non-overlapping 500 ms windows at 2.0/2.65/3.3/3.95 s — [1.5-2.0][2.15-2.65][2.8-3.3][3.45-3.95]
    if (tailGrabs < 4 && el >= (FIELDCUT_MEAS_MS - 2000UL) + 650UL * tailGrabs) {
      int n = faGrabRecent(fcScratch, FIELDCUT_FAST_N);
      if (n >= 2000) { faLoSum += fcMeanMv(fcScratch, n); faLoN++; }
      tailGrabs++;
    }
    if (el >= FIELDCUT_MEAS_MS) {
      fieldCutFloorA = (adsTailN > 0) ? (float)(adsTailSum / adsTailN) : ampsRaw;
      fieldCutActive = 2;
      fieldCutProcess(faHiN ? (float)(faHiSum / faHiN) : NAN,
                      faLoN ? (float)(faLoSum / faLoN) : NAN);
      fieldCutResultsReady = true;   // published BEFORE the ease — a late abort keeps the result
      phase = 4; phaseStartMs = nowMs;
      fieldCutPhase = 3;
    }
    return true;
  }

  // ── EASE-OUT: ramp duty MinDuty → entryDuty so re-engagement can't trip a protection ──
  if (phase == 4) {
    fieldCutPhase = 3;
    float frac = (float)(nowMs - phaseStartMs) / FIELDCUT_EASE_MS;
    if (frac >= 1.0f) {
      fieldCutActive = 0; fieldCutPhase = 0; fieldCutLastEndMs = millis();
      phase = 0; dutyOut = entryDuty;
      return false;
    }
    dutyOut = MinDuty + (entryDuty - MinDuty) * frac;
    return true;
  }
  return false;
}

// protTest_tick() — manual protection-trigger test (Settings → Emergency & Troubleshooting)
// Reproduces each field-collapse waveform on demand to hunt an electrical fault. Same duty-override contract as
// fieldCut_tick: TRUE only while it OWNS duty; FALSE during the energize ramp (protTestCcActive branch drives the
// real PID) and the mode-1/4 pre-gate cut + ladder recovery. NO capture: normal telemetry records the transient.
// Modes: 1 instant cut (pre-gate applyImmediateCut, fast-OV), 2 load-dump (MinDuty + inner integrator dump),
// 3 graceful ramp at DutyRampRate, 4 ladder walk (mode-1 cut x protTestReps up the lockout ladder).
// Guards (AUTO / RPM / charging / no other test) live in the /protTestStart handler.
bool protTest_tick(float &dutyOut, float ampsRaw, uint32_t nowMs) {
  static uint8_t  phase = 0;          // 0=idle 1=ramp/settle 2=fire-active 3=ease-out 5=ladder recovery wait
  static uint32_t phaseStartMs = 0;
  static float    entryDuty = 1.0f;   // pre-test operating duty — eased back to on exit
  static float    freezeDuty = 0.0f;  // applied duty at settle (mode-3 ramp start)
  static float    ptPreSetpoint = 0.0f;
  static float    ampsEma = 0.0f;
  static uint32_t inBandSinceMs = 0;
  static bool     ptCutSeen = false;  // mode 4: has the armed cut actually landed yet

  // ── Abort ──
  if (phase != 0 && protTestAbortRequested) {
    protTestAbortRequested = false;
    protTestCcActive = false;
    protTestCutPending = false;
    protTestManualCutActive = false;   // release the manual hard-cut hold — never leave the field open on abort
    if (protTestMode == 2) g_loadDumpActive = false;   // mode-2 sets it for the hold; the real detector (which would clear it) is bypassed while the test owns duty
    queueConsoleMessage("Protection test: aborted");
    protTestActive = 0; protTestLastEndMs = millis();
    phase = 0; dutyOut = entryDuty;
    return false;
  }

  // ── IDLE: wait for the /protTestStart trigger (do NOT touch dutyOut) ──
  if (phase == 0) {
    // Cancel must beat a queued start: this tick can be starved (shutdown/lockout path) between the
    // start request and the first idle tick, so a pending request is abortable before it ever runs.
    if (protTestAbortRequested) { protTestAbortRequested = false; protTestRequested = false; return false; }
    if (!protTestRequested) return false;
    protTestRequested = false;
    protTestCutPending = false;
    entryDuty = fmaxf(dutyOut, fieldScale());
    ptPreSetpoint = setpointLimited;
    ampsEma = ampsRaw; inBandSinceMs = 0; ptCutSeen = false;
    protTestRepDone = 0;
    protTestActive = 1;
    if (protTestMode == 5) {
      // Manual bench cut: the field is ALREADY set by the user (Manual Field into a resistor). No
      // energize ramp — go straight to the hard-cut hold against the field they dialed in.
      protTestCcActive = false;
      phase = 6; phaseStartMs = nowMs;
      queueConsoleMessageF("Protection test: MANUAL hard cut — %u ms x%u (gap %u ms)", protTestCutMs, protTestReps, protTestGapMs);
      return false;
    }
    protTestCmdA = constrain(protTestCmdA, 5.0f, 100.0f);
    protTestCcActive = true;   // hand the setpoint to the real current loop (branch in AdjustField)
    phase = 1; phaseStartMs = nowMs;
    queueConsoleMessageF("Protection test mode %u: ramping to %.0f A through the current loop", protTestMode, protTestCmdA);
    return false;
  }

  // ── RAMP/SETTLE: the protTestCcActive branch slews the setpoint to protTestCmdA and the real PID
  //   drives duty. Watch for the measured current to sit in-band FIELDCUT_SETTLE_MS, then fire. ──
  if (phase == 1) {
    ampsEma += 0.05f * (ampsRaw - ampsEma);
    bool arrived = fabsf(setpointLimited - protTestCmdA) < 0.5f;
    float band = fmaxf(3.0f, 0.08f * protTestCmdA);
    bool inBand = arrived && fabsf(ampsEma - protTestCmdA) < band;
    if (inBand) { if (inBandSinceMs == 0) inBandSinceMs = nowMs; }
    else inBandSinceMs = 0;
    bool settled = inBand && (nowMs - inBandSinceMs >= FIELDCUT_SETTLE_MS);
    if (!settled && (nowMs - phaseStartMs >= FIELDCUT_SETTLE_TIMEOUT_MS)) {
      if (ampsEma >= FIELDCUT_MIN_BASE_A) {
        settled = true;
        queueConsoleMessageF("Protection test: %.0f A not reachable here — firing at %.1f A", protTestCmdA, ampsEma);
      } else {
        protTestCcActive = false;
        queueConsoleMessage("Protection test: aborted — no charging output; raise RPM and retry");
        protTestActive = 0; protTestLastEndMs = millis(); phase = 0;
        return false;
      }
    }
    if (!settled) return false;

    // ── FIRE: field is settled at target. Diverge by mode. ──
    freezeDuty = lastAppliedDuty;
    protTestCcActive = false;
    setpointLimited = ptPreSetpoint;   // the bumpless-resume snapshot must capture the pre-test value

    if (protTestMode == 1) {
      protTestCutPending = true;       // pre-gate fires the fast-OV cut next tick
      queueConsoleMessageF("Protection test: INSTANT FIELD CUT armed at %.1f A (fast-OV path + adaptive lockout)", ampsEma);
      protTestActive = 0; protTestLastEndMs = millis(); phase = 0;
      return false;
    }
    if (protTestMode == 4) {
      protTestCutPending = true;
      protTestRepDone++;
      ptCutSeen = false;
      queueConsoleMessageF("Protection test: ladder cut %u/%u armed at %.1f A", protTestRepDone, protTestReps, ampsEma);
      if (protTestRepDone >= protTestReps) { protTestActive = 0; protTestLastEndMs = millis(); phase = 0; return false; }
      phase = 5; phaseStartMs = nowMs;   // wait for the cut to land + lockout to clear, then re-energize
      return false;
    }
    if (protTestMode == 2) {
      // Load-dump collapse: own duty at MinDuty and dump the inner PID integrator — the real load-dump
      // signature (fastest commanded field collapse; GPIO4 stays HIGH, not a hard open).
      currentPID.ResetIntegratorTo(0.0);
      g_loadDumpCount++;
      g_loadDumpActive = true;
      queueConsoleMessageF("Protection test: LOAD-DUMP collapse from %.1f%% — duty->MinDuty, inner PID reset", freezeDuty);
      phase = 2; phaseStartMs = nowMs;
      dutyOut = MinDuty;
      return true;
    }
    // mode 3 graceful ramp-to-zero (the clean control case)
    queueConsoleMessageF("Protection test: GRACEFUL RAMP-DOWN from %.1f%% at %.0f%%/s", freezeDuty, DutyRampRate);
    phase = 2; phaseStartMs = nowMs;
    dutyOut = freezeDuty;
    return true;
  }

  // ── FIRE-ACTIVE: mode 2 holds MinDuty; mode 3 ramps duty down at DutyRampRate ──
  if (phase == 2) {
    if (protTestMode == 2) {
      dutyOut = MinDuty;
      if (nowMs - phaseStartMs >= PROT_LOADDUMP_HOLD_MS) {
        g_loadDumpActive = false;
        phase = 3; phaseStartMs = nowMs;
      }
      return true;
    }
    float sec = (float)(nowMs - phaseStartMs) / 1000.0f;
    float d = freezeDuty - DutyRampRate * sec;
    if (d <= MinDuty) {
      dutyOut = MinDuty;
      phase = 3; phaseStartMs = nowMs;
      return true;
    }
    dutyOut = d;
    return true;
  }

  // ── EASE-OUT: ramp MinDuty → entryDuty so re-engagement can't itself trip a protection ──
  if (phase == 3) {
    float frac = (float)(nowMs - phaseStartMs) / FIELDCUT_EASE_MS;
    if (frac >= 1.0f) {
      protTestActive = 0; protTestLastEndMs = millis();
      phase = 0; dutyOut = entryDuty;
      return false;
    }
    dutyOut = MinDuty + (entryDuty - MinDuty) * frac;
    return true;
  }

  // ── LADDER RECOVERY WAIT (mode 4): wait out the armed cut + its lockout, then re-energize ──
  // During the cut + lockout AdjustField takes the shutdown path and never calls this, so gpio4IsLow /
  // fieldCollapseTime are never observably true here; the fire is detected by the pre-gate CONSUMING protTestCutPending.
  if (phase == 5) {
    if (nowMs - phaseStartMs >= PROT_RECOVER_TIMEOUT_MS) {
      queueConsoleMessage("Protection test: aborted — ladder recovery timed out");
      protTestCutPending = false;
      protTestActive = 0; protTestLastEndMs = millis(); phase = 0;
      return false;
    }
    if (!ptCutSeen) {
      if (protTestCutPending) return false;   // still armed — pre-gate found the field already low
      ptCutSeen = true;
    }
    if (fieldCollapseTime == 0 && !gpio4IsLow) {
      // lockout cleared and the field can re-energize — re-arm the ramp for the next rung
      protTestCcActive = true;
      ampsEma = ampsRaw; inBandSinceMs = 0;
      phase = 1; phaseStartMs = nowMs;
      queueConsoleMessageF("Protection test: re-energizing for ladder cut %u/%u", protTestRepDone + 1, protTestReps);
    }
    return false;
  }

  // ── MANUAL HARD-CUT HOLD (mode 5, phase 6): field-enable held open for protTestCutMs against the user's field;
  //   protTestManualCutActive gates the enable in AdjustField, and here we own duty at PWM 0. ──
  if (phase == 6) {
    if (!protTestManualCutActive) {
      protTestManualCutActive = true;
      digitalWrite(4, LOW);            // immediate cut on the entry tick; the gate holds it thereafter
      gpio4IsLow = true;
      protTestRepDone++;
      queueConsoleMessageF("Protection test: manual hard cut %u/%u — field open %u ms", protTestRepDone, protTestReps, protTestCutMs);
    }
    dutyOut = 0.0f;
    if (nowMs - phaseStartMs >= protTestCutMs) {
      protTestManualCutActive = false;   // release — the gate re-enables the field; manual duty ramps back at DutyRampRate
      if (protTestRepDone >= protTestReps) {
        protTestActive = 0; protTestLastEndMs = millis(); phase = 0;
        queueConsoleMessage("Protection test: manual cut sequence complete");
        return false;
      }
      phase = 7; phaseStartMs = nowMs;   // gap: let the field ramp back up before the next cut
    }
    return true;
  }

  // ── MANUAL GAP (mode 5, phase 7): field restored, wait protTestGapMs, then cut again ──
  if (phase == 7) {
    if (ManualFieldToggle != 1) {
      // The start guard only checks Manual Field ON; turning it off mid-sequence must not leave the
      // remaining cuts firing into whatever mode the system fell into.
      queueConsoleMessage("Protection test: manual cut sequence aborted — Manual Field turned off");
      protTestActive = 0; protTestLastEndMs = millis(); phase = 0;
      return false;
    }
    if (nowMs - phaseStartMs >= protTestGapMs) {
      phase = 6; phaseStartMs = nowMs;
    }
    return false;   // normal manual control drives the field back up during the gap
  }

  return false;
}

// fieldCurve_tick() — auto-commissioning Phase 1a field-% sweep
//
// Quasi-static duty->amps ramp; like systemID_tick, returns true while active and the caller must force
// GOV_BYPASS_SLEW + MANUAL PID with dutyOut as the command. Amps are averaged over the settled 2nd half of each
// dwell so field L/R lag doesn't bias the curve. Proposals (SystemIDStabilizeAmps / StepAmplitude below the knee)
// are show-before-write: the dashboard reads /fieldcurve.csv and the user clicks Apply.
// Whole-run RPM stats over the same settled samples; advisory only, never gated on.
static double   fcRunRpmSum = 0.0;
static uint32_t fcRunRpmN   = 0;
static void fcRunRpmSample() {
  float r = (float)RPM;
  if (fcRunRpmN == 0) { fieldCurveRpmMin = r; fieldCurveRpmMax = r; }
  else {
    if (r < fieldCurveRpmMin) fieldCurveRpmMin = r;
    if (r > fieldCurveRpmMax) fieldCurveRpmMax = r;
  }
  fcRunRpmSum += r; fcRunRpmN++;
  fieldCurveRpmAvg = (float)(fcRunRpmSum / fcRunRpmN);
}

bool fieldCurve_tick(float &dutyOut, float ampsRaw, uint32_t nowMs) {
  static uint8_t  phase = 0;          // 0=idle, 1=ramp, 2=ease-out
  static float    stepDuty = 0.0f;
  static uint32_t stepStartMs = 0;
  static double   ampSum = 0.0;
  static double   dutySum = 0.0;      // APPLIED duty over the same settled window as amps (a step past the ceiling is clamped)
  static uint32_t ampN = 0;
  static uint32_t fcEaseStartMs = 0;  // EASE phase: gentle field ramp-down on exit
  static float    fcEaseFromDuty = 0.0f;
  static uint32_t fcEaseInStartMs = 0;  // EASE-IN phase: gentle ramp from the operating duty to the sweep floor on entry
  static float    fcEaseInFromDuty = 0.0f;
  static float    onsetBaseline = 0.0f;  // onset-mode: amps at the first settled step (below the knee)
  static float    onsetLastAbove = 0.0f; // onset fine-down: lowest duty still producing onset current
  static double   rpmSum = 0.0;          // onset-mode: RPM accumulated over the same settled window as amps
  static float    onsetLastAboveRPM = 0.0f;  // RPM sampled AT the knee-defining step (engine drifts across a sweep)
  static float    onsetLastAboveTempF = 0.0f;// case temp sampled AT the knee-defining step

  // ── Abort ────────────────────────────────────────────────────────────────
  if (phase != 0 && fieldCurveAbortRequested) {
    fieldCurveAbortRequested = false;
    queueConsoleMessage("Field curve: aborted");
    fieldCurveActive = 0;
    fieldCurveLastEndMs = millis();
    phase = 0;
    return false;
  }

  // ── IDLE: wait for trigger (do NOT touch dutyOut — SystemID may own it) ───
  if (phase == 0) {
    if (!fieldCurveRequested) return false;
    fieldCurveRequested = false;
    fieldCurveAbortRequested = false;

    if (fieldCurveBuf == nullptr) {
      fieldCurveBuf = (FieldCurvePoint *)ps_malloc(FIELDCURVE_MAX_PTS * sizeof(FieldCurvePoint));
      if (fieldCurveBuf == nullptr) {
        queueConsoleMessage("Field curve: ABORTED — PSRAM alloc failed");
        return false;
      }
    }

    // Table limit-at-speed: the per-RPM current cap, clamped to the rated max. The saturation
    // sweep needs real headroom to reach the knee; the onset sweep stops at the first amps and
    // needs none, so it skips this abort (it must run at low RPM where the cap is small).
    fieldCurveTargetLimitA = fminf(interpolateRPMTable(RPM, rpmCapCurrentTable), (float)MaxTableValue);
    if (!fieldCurveOnsetMode && fieldCurveTargetLimitA < 5.0f) {
      queueConsoleMessageF("Field curve: ABORTED — table limit-at-speed only %.0fA (raise RPM)", fieldCurveTargetLimitA);
      return false;
    }

    fieldCurveCount = 0;
    fieldCurveResultsReady = false;
    fieldCurveOk = false;
    fieldCurveCeilingLimited = false;
    fieldCurveKneeDuty = -1.0f;
    fieldCurveKneeAmps = -1.0f;
    fcRunRpmSum = 0.0; fcRunRpmN = 0;
    fieldCurveRpmMin = fieldCurveRpmMax = fieldCurveRpmAvg = 0.0f;
    onsetBaseline = 0.0f;
    if (fieldCurveOnsetMode) { kneeSweepKneeDuty = -1.0f; kneeSweepOk = false; }
    stepDuty = FIELDCURVE_DUTY_START;
    // Warm-start (Min% onset only): commissioning sweeps run in DESCENDING RPM order, so each onset duty is higher
    // than the previous (1/RPM law). Start one step below the last anchor's duty; the backoff keeps a pre-onset
    // baseline so the detector still sees current "begin".
    if (fieldCurveOnsetMode && kneeAnchorN > 0) {
      float warm = kneeAnchorDuty[kneeAnchorN - 1] - FIELDCURVE_DUTY_STEP;
      if (warm > stepDuty) stepDuty = warm;
    }
    stepStartMs = nowMs;
    ampSum = 0.0;
    dutySum = 0.0;
    ampN = 0;
    rpmSum = 0.0;
    // Ease DOWN from the live duty to the sweep floor over FIELDCURVE_EASE_MS (the ramp runs under GOV_BYPASS_SLEW,
    // so an un-eased entry is an instant drop). A start at/below the floor enters the sweep directly.
    if (lastAppliedDuty > stepDuty) {
      fcEaseInFromDuty = lastAppliedDuty;
      fcEaseInStartMs = nowMs;
      phase = 4;
    } else {
      phase = 1;
    }
    fieldCurveActive = 1;
    if (fieldCurveOnsetMode)
      queueConsoleMessageF("Min%% knee: ramping from %.1f%% @ %.0f RPM, stop at first onset (step %.1f%%, dwell %lums)",
                           stepDuty, RPM, FIELDCURVE_DUTY_STEP, FIELDCURVE_DWELL_MS);
    else
      queueConsoleMessageF("Field curve: ramping to %.0fA limit @ %.0f RPM (step %.1f%%, dwell %lums)",
                           fieldCurveTargetLimitA, RPM, FIELDCURVE_DUTY_STEP, FIELDCURVE_DWELL_MS);
  }

  // ── RAMP ──────────────────────────────────────────────────────────────────
  if (phase == 1) {
    // OV during the ramp is handled by the fast-OV layer on the main path (fires before this override and flags
    // fieldCurveAbortRequested), so no bespoke headroom abort here.
    // Cap the ramp at ccDutyCeiling() (Max Field % / Max Field Volts, which setDutyPercent clamps APPLIED duty to)
    // so it stops there instead of recording a frozen-duty region as a bogus curve; fieldCurveCeilingLimited names it.
    float effDutyCeil = FIELDCURVE_DUTY_MAX;
    if (ccDutyCeiling() < effDutyCeil) effDutyCeil = ccDutyCeiling();
    dutyOut = stepDuty;

    // Accumulate amps (and RPM, for the onset knee) only over the settled second half of the dwell.
    if ((nowMs - stepStartMs) >= (FIELDCURVE_DWELL_MS / 2)) {
      ampSum += ampsRaw;
      dutySum += dutyAfterCeiling(stepDuty);
      rpmSum += RPM;
      ampN++;
      fcRunRpmSample();
    }

    if ((nowMs - stepStartMs) >= FIELDCURVE_DWELL_MS) {
      float avgAmps = (ampN > 0) ? (float)(ampSum / ampN) : ampsRaw;
      float avgRpm  = (ampN > 0) ? (float)(rpmSum / ampN) : RPM;
      // Recorded at the APPLIED duty: the last step usually lands past the ceiling, and its command would draw
      // a false slope drop (a "knee") into the final segment.
      float avgDuty = (ampN > 0) ? (float)(dutySum / ampN) : dutyAfterCeiling(stepDuty);
      // A clamped point under half a step above the previous one is dropped: the slope over it is noise, and
      // one at or below the previous duty breaks the ascending order fieldCurveInvert relies on. Saturation
      // mode only, because the onset baseline keys off fieldCurveCount == 1.
      bool clampedTooClose = !fieldCurveOnsetMode && fieldCurveCount > 0 && avgDuty < stepDuty - 0.01f
                             && avgDuty < fieldCurveBuf[fieldCurveCount - 1].duty + 0.5f * FIELDCURVE_DUTY_STEP;
      if (fieldCurveCount < FIELDCURVE_MAX_PTS && !clampedTooClose) {
        fieldCurveBuf[fieldCurveCount++] = { avgDuty, avgAmps };
      }
      // ── Onset-stop mode (Min% knee) ─────────────────────────────────────────
      // First settled step = baseline; stop when a later step lifts kneeOnsetA above it. No onset by
      // kneeMaxFloorPct+margin -> record the ceiling, flag "no clean knee".
      if (fieldCurveOnsetMode) {
        if (fieldCurveCount == 1) {
          onsetBaseline = avgAmps;
        } else if ((avgAmps - onsetBaseline) >= kneeOnsetA) {
          // Coarse onset found (within one FIELDCURVE_DUTY_STEP). Refine by backing DOWN in fine
          // steps until amps fall back below onset — the lowest duty still above onset is the knee.
          queueConsoleMessageF("Min%% knee: coarse onset %.2fA at %.1f%% duty @ %.0f RPM, refining down...",
                               avgAmps - onsetBaseline, stepDuty, avgRpm);
          // Pair the knee with RPM/temp measured AT this step (the engine drifts hundreds of RPM across a sweep);
          // fine-down overwrites them per lower step still showing onset.
          onsetLastAbove = stepDuty;
          onsetLastAboveRPM = avgRpm;
          onsetLastAboveTempF = isnan(AlternatorTemperatureF) ? kneeTempRefF : AlternatorTemperatureF;
          stepDuty -= FIELDCURVE_ONSET_FINE_STEP;
          if (stepDuty < FIELDCURVE_DUTY_START) stepDuty = FIELDCURVE_DUTY_START;
          stepStartMs = nowMs; ampSum = 0.0; ampN = 0; rpmSum = 0.0;
          phase = 3;
          return true;
        }
        // Stop ceiling for "no onset found": the configured Min%-floor ceiling, but never command above
        // the 24/36/48V field-duty limit (a real onset above that limit can't be reached anyway).
        float onsetCeil = kneeMaxFloorPct + kneeMarginPct;
        bool  onsetCeilDutyCapped = false;
        if (effDutyCeil < onsetCeil) { onsetCeil = effDutyCeil; onsetCeilDutyCapped = true; }
        if (stepDuty >= onsetCeil) {
          kneeSweepKneeDuty = stepDuty;
          kneeSweepRPM      = avgRpm;
          kneeSweepTempF    = isnan(AlternatorTemperatureF) ? kneeTempRefF : AlternatorTemperatureF;
          kneeSweepOk       = false;
          fieldCurveResultsReady = true;
          if (onsetCeilDutyCapped) {
            fieldCurveCeilingLimited = true; fieldCurveCeilBind = g_fieldCeilBind;
            queueConsoleMessageF("Min%% knee: no onset below %.1f%% duty @ %.0f RPM, capped by %s; onset may be higher. %s",
                                 avgDuty, avgRpm, fieldCeilBindName(g_fieldCeilBind), fieldCeilBindRemedy(g_fieldCeilBind));
          } else {
            queueConsoleMessageF("Min%% knee: no onset below %.1f%% duty @ %.0f RPM (using ceiling)", stepDuty, avgRpm);
          }
          fcEaseFromDuty = stepDuty; fcEaseStartMs = nowMs; phase = 2; fieldCurveActive = 2;
          return true;
        }
        // no onset yet → fall through to the next step
      } else {
      bool reachedLimit = (avgAmps >= fieldCurveTargetLimitA);
      bool reachedCeil  = (stepDuty >= effDutyCeil);
      bool bufFull      = (fieldCurveCount >= FIELDCURVE_MAX_PTS);

      if (reachedLimit || reachedCeil || bufFull) {
        if (bufFull && !reachedLimit && !reachedCeil)
          queueConsoleMessageF("Field curve: stopped at %d points and %.1f%% duty before reaching %.0fA or the %.1f%% field ceiling, so the curve is cut short",
                               fieldCurveCount, stepDuty, fieldCurveTargetLimitA, effDutyCeil);
        // Stopped at the duty-limit ceiling (not the natural 92% backstop) without hitting the amp
        // target → the curve is truncated by whichever field ceiling binds. Flag it + name it.
        if (reachedCeil && !reachedLimit && effDutyCeil < FIELDCURVE_DUTY_MAX) {
          fieldCurveCeilingLimited = true; fieldCurveCeilBind = g_fieldCeilBind;
          queueConsoleMessageF("Field curve: stopped at %.1f%% duty before reaching %.0fA, capped by %s, so the curve is cut short. %s",
                               effDutyCeil, fieldCurveTargetLimitA,
                               fieldCeilBindName(g_fieldCeilBind), fieldCeilBindRemedy(g_fieldCeilBind));
        }
        // ── Post-process: knee + proposals ──────────────────────────────────
        fieldCurveActive = 2;
        // Max early slope over the lower third of the captured range.
        float maxSlope = 0.0f;
        int lowThird = (fieldCurveCount / 3 > 1) ? fieldCurveCount / 3 : (fieldCurveCount > 1 ? 2 : 1);
        for (int i = 1; i < lowThird && i < fieldCurveCount; i++) {
          float dd = fieldCurveBuf[i].duty - fieldCurveBuf[i - 1].duty;
          if (dd > 0.001f) {
            float s = (fieldCurveBuf[i].amps - fieldCurveBuf[i - 1].amps) / dd;
            if (s > maxSlope) maxSlope = s;
          }
        }
        // Knee = first point where slope falls below 40% of the early max.
        for (int i = 1; i < fieldCurveCount; i++) {
          float dd = fieldCurveBuf[i].duty - fieldCurveBuf[i - 1].duty;
          if (dd <= 0.001f) continue;
          float s = (fieldCurveBuf[i].amps - fieldCurveBuf[i - 1].amps) / dd;
          if (maxSlope > 0.0f && s < 0.40f * maxSlope && fieldCurveBuf[i].amps > 0.3f * fieldCurveTargetLimitA) {
            fieldCurveKneeDuty = fieldCurveBuf[i].duty;
            fieldCurveKneeAmps = fieldCurveBuf[i].amps;
            break;
          }
        }

        // Targets: 50% / 75% of the table limit, slid below the knee if needed.
        float amps75 = 0.75f * fieldCurveTargetLimitA;
        if (fieldCurveKneeAmps > 0.0f && amps75 > fieldCurveKneeAmps) amps75 = fieldCurveKneeAmps;
        float amps50 = amps75 * (0.50f / 0.75f);

        float duty50 = fieldCurveInvert(amps50);
        float duty75 = fieldCurveInvert(amps75);

        fieldCurvePropStabA   = amps50;
        fieldCurvePropStepPct = constrain((duty75 - duty50) * 0.5f, 1.0f * fieldScale(), 25.0f);
        fieldCurveOk = (fieldCurveCount >= 4 && duty75 > duty50 && amps50 > 1.0f);

        char kneeTxt[40];
        if (fieldCurveKneeAmps > 0.0f) snprintf(kneeTxt, sizeof(kneeTxt), "knee %.1fA@%.1f%%", fieldCurveKneeAmps, fieldCurveKneeDuty);
        else                           snprintf(kneeTxt, sizeof(kneeTxt), "no knee in range");
        queueConsoleMessageF("Field curve: %d pts, %s — propose Stabilize=%.0fA Step=%.1f%% (%s)",
                             fieldCurveCount, kneeTxt,
                             fieldCurvePropStabA, fieldCurvePropStepPct,
                             fieldCurveOk ? "ok" : "review");

        fieldCurveResultsReady = true;
        // Ease down over ~1.5 s: a one-tick collapse trips a protection when the override releases. fieldCurveActive
        // stays non-zero so the dashboard poll waits for the ease.
        fcEaseFromDuty = stepDuty;
        fcEaseStartMs = nowMs;
        phase = 2;
        fieldCurveActive = 2;
        return true;
      }
      }  // end !fieldCurveOnsetMode (saturation) branch

      stepDuty += FIELDCURVE_DUTY_STEP;
      stepStartMs = nowMs;
      ampSum = 0.0;
      dutySum = 0.0;
      ampN = 0;
      rpmSum = 0.0;
    }
    return true;
  }

  // ── ONSET FINE-DOWN (refine the coarse knee, onset mode only) ───────────────
  // Back down in FIELDCURVE_ONSET_FINE_STEP steps; each settled step still >= kneeOnsetA over baseline becomes the
  // new lowest-above, and the first below ends it (knee rounded conservatively high = field stays primed).
  if (phase == 3) {
    dutyOut = stepDuty;
    if ((nowMs - stepStartMs) >= (FIELDCURVE_DWELL_MS / 2)) { ampSum += ampsRaw; rpmSum += RPM; ampN++; fcRunRpmSample(); }
    if ((nowMs - stepStartMs) >= FIELDCURVE_DWELL_MS) {
      float avgAmps = (ampN > 0) ? (float)(ampSum / ampN) : ampsRaw;
      float avgRpm  = (ampN > 0) ? (float)(rpmSum / ampN) : RPM;
      bool stillAbove = ((avgAmps - onsetBaseline) >= kneeOnsetA);
      bool atFloor = (stepDuty <= FIELDCURVE_DUTY_START + 0.001f);
      if (stillAbove) {
        // Still onset current: this step defines the knee so far, with its co-temporal RPM/temp.
        onsetLastAbove = stepDuty;
        onsetLastAboveRPM = avgRpm;
        onsetLastAboveTempF = isnan(AlternatorTemperatureF) ? kneeTempRefF : AlternatorTemperatureF;
      }
      if (!stillAbove || atFloor) {
        kneeSweepKneeDuty = onsetLastAbove;
        kneeSweepRPM      = onsetLastAboveRPM;     // RPM AT the knee-defining step, not a late read
        kneeSweepTempF    = onsetLastAboveTempF;
        kneeSweepOk       = true;
        fieldCurveResultsReady = true;
        queueConsoleMessageF("Min%% knee: refined onset at %.2f%% duty @ %.0f RPM", onsetLastAbove, onsetLastAboveRPM);
        fcEaseFromDuty = stepDuty; fcEaseStartMs = nowMs; phase = 2; fieldCurveActive = 2;
        return true;
      }
      stepDuty -= FIELDCURVE_ONSET_FINE_STEP;
      if (stepDuty < FIELDCURVE_DUTY_START) stepDuty = FIELDCURVE_DUTY_START;
      stepStartMs = nowMs; ampSum = 0.0; ampN = 0; rpmSum = 0.0;
    }
    return true;
  }

  // ── EASE-OUT ──────────────────────────────────────────────────────────────
  if (phase == 2) {
    float frac = (float)(nowMs - fcEaseStartMs) / FIELDCURVE_EASE_MS;
    if (frac >= 1.0f) {
      fieldCurveActive = 0;
      fieldCurveLastEndMs = millis();
      phase = 0;
      dutyOut = FIELDCURVE_DUTY_START;
      return false;
    }
    dutyOut = fcEaseFromDuty + (FIELDCURVE_DUTY_START - fcEaseFromDuty) * frac;
    return true;
  }

  // ── EASE-IN ────────────────────────────────────────────────────────────────
  // Live duty -> sweep floor; the dwell clock starts only when the ease finishes, so no settle window opens early.
  if (phase == 4) {
    float frac = (float)(nowMs - fcEaseInStartMs) / FIELDCURVE_EASE_MS;
    if (frac >= 1.0f) {
      dutyOut = stepDuty;
      stepStartMs = nowMs;
      ampSum = 0.0; dutySum = 0.0; ampN = 0; rpmSum = 0.0;
      phase = 1;
      return true;
    }
    dutyOut = fcEaseInFromDuty + (stepDuty - fcEaseInFromDuty) * frac;
    return true;
  }

  return false;
}


// altSweep_tick() — gate-tuning field sweeper (ALT_GATE_TUNING_CAPTURE_SPEC.md §8)
//
// Continuous duty ramp up THEN down (same contract as fieldCurve_tick): averaging both legs cancels the symmetric
// ~0.7 s field lag. Stimulus only. Bounded by ccDutyCeiling(), the enforced field ceiling, as the field curve is.
// Fast-OV fires before this override and latches altSweepAbortRequested (6_functions.ino).
// Turn around SHORT of current/voltage limits: every soft layer (Group 1/2 OV, iExcess, load dump, thermal derate,
// min-select ceilings) is bypassed inside !sysIDRunning, so arriving at a limit ends in a hard cut and loses the
// down leg. No temperature margin (case temp can't overshoot behind the field lag). The amps margin never exceeds a
// quarter of the limit it guards, so a small cap still sweeps.
static inline float altSweepAmpMargin(float limitA) { return fminf(altSweepMarginA, 0.25f * limitA); }

// Which control limit the machine is approaching, from live measurements vs CONFIGURED limits, not ctrlLimiter
// (the min-select chain is bypassed and ctrlLimiter forced 0 during the sweep). Each test repeats that chain's
// enable gates, or the sweep would turn around on a limit the regulator itself would not honour.
static const char *altSweepLimitNow() {
  float v = getBatteryVoltage();
  if (ChargingVoltageTarget > 0.5f && v >= ChargingVoltageTarget - altSweepMarginV)
                                                                           return "charge-voltage target";
  // DVCC limits count only while following an external authority: dvccCvlV/dvccCclA hold stale values when the link
  // drops or the feature is off. "external", not "BMS": the publisher is often a gateway (Cerbo GX).
  bool dvccFollowing = (dvccEn == 1 && dvccState == 3);
  if (dvccFollowing && !isnan(dvccCvlV) && v >= dvccCvlV - altSweepMarginV)
                                                                           return "external voltage limit";
  float amps = isnan(MeasuredAmps) ? 0.0f : MeasuredAmps;
  // capLimitMode 1 = the cap table is watts, not amps (the min-select chain divides it by bus volts). While engine speed
  // is ignored the chain uses the owner's one limit instead (rpmCapAmpsNow), so the sweep does too.
  float capA;
  if (IgnoreRPM) capA = (NoRpmCurrentA > 0.0f) ? NoRpmCurrentA : smallestRpmTableAmps();
  else           capA = (capLimitMode == 1 && v > 0.5f) ? (interpolateRPMTable(RPM, rpmCapPowerTable) / v)
                                                        : interpolateRPMTable(RPM, rpmCapCurrentTable);
  capA = fminf(capA, (float)MaxTableValue);
  if (capA > 0.5f && amps >= capA - altSweepAmpMargin(capA))               return "current limit at this engine speed";
  // Measured on the basis the min-select chain applies the CCL: battery current on a live shunt, unless it is the WiFi
  // carrier's alternator ceiling (dvccCclAltBasis) or there is no live shunt (then it caps alternator amps directly).
  if (dvccFollowing && !isnan(dvccCclA) && dvccCclA > 0.5f) {
    float cclMeas = (!dvccCclAltBasis && HAS_BATT_SHUNT && bcurFresh() && !isnan(Bcur)) ? Bcur : amps;
    if (cclMeas >= dvccCclA - altSweepAmpMargin(dvccCclA))                return "external current limit";
  }
  // Same enable gate the battery-limit ceiling uses in the min-select chain (BattLimitEnable +
  // a configured INA228 shunt), so a disabled limit cannot cut the sweep short.
  if (BattLimitEnable && HAS_BATT_SHUNT && BattCurrentLimitA > 0.5f
      && !isnan(Bcur) && Bcur >= BattCurrentLimitA - altSweepAmpMargin(BattCurrentLimitA))
                                                                           return "battery current limit";
  if (!IgnoreTemperature && TemperatureLimitF > 0 && !isnan(TempToUse) && TempToUse >= TemperatureLimitF)   // the thermal loop parks while ignored
                                                                           return "alternator temperature limit";
  return nullptr;
}
#define ALTSWEEP_LIMIT_CONFIRM_MS 0u     // reverse on the FIRST sample at a limit: an early ripple reversal only
                                         // shortens the sweep (bit 7 marks it), a late one at a steep acceptance knee
                                         // ends in a hard OV cut (a 250 ms debounce cost 0.3 V of a 0.15 V margin)

bool altSweep_tick(float &dutyOut, float ampsRaw, uint32_t nowMs) {
  static uint8_t  phase = 0;        // 0 idle, 1 ease-in, 2 ramp, 3 ease-out
  static float    fromDuty = 0.0f, toDuty = 0.0f, curDuty = 0.0f, ratePctS = 1.0f;
  static float    easeFromDuty = 0.0f;
  static uint32_t easeStartMs = 0, lastRampMs = 0, limSinceMs = 0;
  static bool     goingDown = false;

  if (phase != 0 && altSweepAbortRequested) {
    altSweepAbortRequested = false;
    // External teardowns (protection cut, leaving normal AUTO; 6_functions.ino) already cleared altSweepActive and
    // announced; only a plain Stop press arrives here still live.
    if (altSweepActive != 0) queueConsoleMessage("Field sweep: aborted");
    altSweepActive = 0;
    altSweepMeasuredLeg = false;
    altSweepLastEndMs = millis();
    phase = 0;
    return false;
  }

  // ── IDLE: wait for trigger (do NOT touch dutyOut — another override may own it) ──
  if (phase == 0) {
    if (!altSweepRequested) return false;
    // A request is only consumed on a normal running control tick, so one pressed with the engine
    // stopped (or during a fault lockout) would otherwise sit armed and start a field ramp by itself
    // whenever running resumed. Expire it instead — the button is a metre away from the throttle.
    if ((uint32_t)(nowMs - altSweepReqMs) > 30000u) {
      altSweepRequested = false;
      queueConsoleMessage("Field sweep: start request expired — the control loop was not running (engine stopped or field off). Press Start again.");
      return false;
    }
    altSweepRequested = false;
    altSweepAbortRequested = false;

    // The field caps (ccDutyCeiling: Max Field %, Max Field Volts, the board's 10 A and drive-heat ceilings) are the
    // ONLY ceilings; FIELDCURVE_DUTY_MAX (the field curve's own knee-search bound) is deliberately NOT carried here.
    float dutyCeil = ccDutyCeiling();
    fromDuty = constrain(altSweepFromPct, 0.0f, dutyCeil);
    toDuty   = (altSweepToPct <= 0.0f) ? dutyCeil : constrain(altSweepToPct, 0.0f, dutyCeil);
    ratePctS = constrain(altSweepRatePctS, 0.1f, 10.0f);
    if (toDuty - fromDuty < 1.0f) {
      queueConsoleMessageF("Field sweep: refused — span %.1f%%→%.1f%% is under 1%% after clamping to the field ceiling %.1f%% (set by %s)",
                           fromDuty, toDuty, dutyCeil,
                           (g_fieldCeilBind == 1) ? "Max Field Volts" : (g_fieldCeilBind == 2) ? "the board's 10 A field-current ceiling"
                           : (g_fieldCeilBind >= 3) ? "the field-drive heat ceiling" : "Max Field %");
      return false;
    }
    curDuty = fromDuty;
    goingDown = false;
    limSinceMs = 0;
    altSweepLimitRev = false;
    altSweepRevWhy = 0;
    altSweepReverseReq = false;   // a press from before this run must not turn the new one around
    lastRampMs = nowMs;
    altSweepActive = 1;
    altSweepMeasuredLeg = false;
    // Ease from the live operating duty to the ramp floor over FIELDCURVE_EASE_MS. Under
    // GOV_BYPASS_SLEW an un-eased entry is an instant step in either direction, so ease both ways.
    easeFromDuty = lastAppliedDuty;
    easeStartMs = nowMs;
    phase = 1;
    queueConsoleMessageF("Field sweep: %.1f%% → %.1f%% → %.1f%% at %.2f%%/s (~%.0f s), Max Field %% %.0f%%",
                         fromDuty, toDuty, fromDuty, ratePctS, 2.0f * (toDuty - fromDuty) / ratePctS, dutyCeil);
  }

  // ── EASE-IN ───────────────────────────────────────────────────────────────
  if (phase == 1) {
    float frac = (float)(nowMs - easeStartMs) / FIELDCURVE_EASE_MS;
    if (frac >= 1.0f) {
      dutyOut = curDuty;
      lastRampMs = nowMs;      // ramp clock starts only once the field has arrived
      altSweepMeasuredLeg = true;
      phase = 2;
      return true;
    }
    dutyOut = easeFromDuty + (curDuty - easeFromDuty) * frac;
    return true;
  }

  // ── RAMP (up, then down) ──────────────────────────────────────────────────
  if (phase == 2) {
    float dt = (float)(nowMs - lastRampMs) * 0.001f;
    lastRampMs = nowMs;
    if (dt > 0.5f) dt = 0.5f;                 // a loop stall must not teleport the command
    curDuty += (goingDown ? -1.0f : 1.0f) * ratePctS * dt;
    // Turn around at the first control limit instead of driving into it: past a limit the rows measure whatever holds
    // the machine back, not output vs field, and at high SOC it ends in a protection cut. The down leg runs at the
    // SAME rate so the pair still averages the field lag out.
    const char *lim = goingDown ? nullptr : altSweepLimitNow();
    if (lim) { if (limSinceMs == 0) limSinceMs = nowMs; } else limSinceMs = 0;
    bool limReached = (limSinceMs != 0) && ((uint32_t)(nowMs - limSinceMs) >= ALTSWEEP_LIMIT_CONFIRM_MS);
    // Reverse now: the operator sees something the limit tests cannot — a noise, a smell, a gauge —
    // and turns the ramp around by hand. No confirmation dwell; the press IS the confirmation.
    bool manualRev = altSweepReverseReq && !goingDown;
    if (altSweepReverseReq) altSweepReverseReq = false;
    if (limReached || manualRev) {
      goingDown = true;
      altSweepActive = 2;
      altSweepLimitRev = true;      // rides flags bit 7 on every row from here to the end of the run
      altSweepRevWhy = manualRev ? 2 : 1;
      limSinceMs = 0;
      if (manualRev)
        queueConsoleMessageF("Field sweep: reversed by hand at %.1f%% field (%.1f A, %.2f V) — coming back down at the same rate",
                             curDuty, ampsRaw, getBatteryVoltage());
      else
        queueConsoleMessageF("Field sweep: %s reached at %.1f%% field (%.1f A, %.2f V) — reversing at the same rate",
                             lim, curDuty, ampsRaw, getBatteryVoltage());
    } else if (!goingDown && curDuty >= toDuty) {
      curDuty = toDuty;
      goingDown = true;
      altSweepActive = 2;
      queueConsoleMessageF("Field sweep: top of ramp %.1f%% (%.1f A) — returning down", curDuty, ampsRaw);
    } else if (goingDown && curDuty <= fromDuty) {
      curDuty = fromDuty;
      easeFromDuty = curDuty;
      easeStartMs = nowMs;
      altSweepActive = 3;
      altSweepMeasuredLeg = false;
      phase = 3;
      queueConsoleMessage("Field sweep: complete — easing the field out");
    }
    dutyOut = curDuty;
    return true;
  }

  // ── EASE-OUT ──────────────────────────────────────────────────────────────
  if (phase == 3) {
    float frac = (float)(nowMs - easeStartMs) / FIELDCURVE_EASE_MS;
    if (frac >= 1.0f) {
      altSweepActive = 0;
      altSweepLastEndMs = millis();
      phase = 0;
      dutyOut = FIELDCURVE_DUTY_START;
      return false;
    }
    dutyOut = easeFromDuty + (FIELDCURVE_DUTY_START - easeFromDuty) * frac;
    return true;
  }

  return false;
}


// tuningSineStep() — Tuning→Current closed-loop sine generator (Stage 2)
//
// Sine setpoint into *out (baseA ± ampA). waveform==2 (auto-sweep) also runs an online single-bin lock-in of the
// MEASURED current per log-spaced frequency into tuningBode[]. phase is caller-owned so it persists across ticks.
void tuningSineStep(uint32_t nowMs, float dt, float &phase, float baseA, float ampA,
                    float measAmps, float &out) {
  static uint8_t  segIdx = 0;
  static uint32_t segStartMs = 0;
  static bool     segStarted = false;
  static double   sAmpsSin, sAmpsCos, sAmps, sSin, sCos, sAmpsSq;
  static uint32_t nAcc;
  static float    gMaxSeen;   // peak |gain| seen so far this sweep — gates which points feed worst-coherence
  static double   sFsSamples, sFsWindowMs;  // sweep-wide totals — their ratio is the measured tick rate
  static float    prevLagDeg;               // last point's unwrapped lag, anchors the ±180 unwrap
  static float    freqList[TUNING_SWEEP_NPOINTS];
  static bool     tuningEaseActive = false;   // post-sweep setpoint ease-down
  static uint32_t tuningEaseStartMs = 0;
  static float    tuningEaseFromA = 0.0f;
  const float     TUNING_EASE_REST_A = 3.0f;  // sweep-exit ease target: low rest current the field glides to before release

  float f;
  if (tuningWaveform == 2) {
    // Start / restart the sweep on UI request.
    if (tuningSweepRequested) {
      tuningSweepRequested = false;
      float fLo = fmaxf(0.1f, tuningSweepStart);
      float fHi = fmaxf(fLo + 0.1f, tuningSweepEnd);
      float fAlias = sweepAliasLimitHz();
      if (fAlias > 0.5f && fHi > fAlias) {
        queueConsoleMessageF("Tuning sine sweep: top trimmed %.1f→%.1f Hz — current sensor only samples at %.0f Hz",
                             fHi, fAlias, ch1SampleHz());
        fHi = fmaxf(fLo + 0.1f, fAlias);
      }
      for (int i = 0; i < TUNING_SWEEP_NPOINTS; i++)
        freqList[i] = fLo * powf(fHi / fLo, (float)i / (float)(TUNING_SWEEP_NPOINTS - 1));
      segIdx = 0; segStarted = false; tuningBodeCount = 0;
      sFsSamples = 0.0; sFsWindowMs = 0.0; prevLagDeg = 0.0f; tuningSweepFsHz = 0.0f;
      tuningSweepActive = true; tuningSweepDone = false; tuningEaseActive = false;
      // Reset whole-sweep run-condition trackers (captured into the record at commit).
      tuningSweepBaseA = baseA; tuningSweepAmpA = ampA;
      tuningSweepRpmMin = (float)RPM; tuningSweepRpmMax = (float)RPM;
      tuningSweepRpmSum = 0.0; tuningSweepRpmN = 0;
      tuningSweepBattV = BatteryV;
      tuningSweepDutyRailed = false;
      tuningSweepAbortReason = 0; tuningSweepAbortVolts = 0.0f; tuningSweepAbortDuty = 0.0f;
      tuningSweepWorstCoh = 1.0f; gMaxSeen = 0.0f;
      queueConsoleMessageF("Tuning sine sweep: %d pts %.1f-%.1f Hz, %d cycles/pt, amp=%.1f A",
                           TUNING_SWEEP_NPOINTS, freqList[0], freqList[TUNING_SWEEP_NPOINTS - 1],
                           tuningSweepCycles, ampA);
    }
    if (!tuningSweepActive) {
      if (tuningEaseActive) {
        // Post-sweep: ease the setpoint down to a low rest current over ~2 s; a still-high current when TuningMode
        // releases reads as over-current once protections re-arm. tuningSweepDone stays false until the ease ends.
        float frac = (float)(nowMs - tuningEaseStartMs) / 2000.0f;
        if (frac >= 1.0f) { tuningEaseActive = false; tuningSweepDone = true; out = TUNING_EASE_REST_A; }
        else out = tuningEaseFromA + (TUNING_EASE_REST_A - tuningEaseFromA) * frac;
        return;
      }
      // Hold at the eased rest current after a sweep (not back at the center) so it doesn't pop up before
      // the dashboard releases TuningMode; hold at center before any sweep so a Run swings cleanly.
      out = tuningSweepDone ? TUNING_EASE_REST_A : baseA;
      return;
    }
    // Protection cut mid-sweep: stop and discard. Points spanning the cut + lockout measure the
    // outage, not the plant. No ease-down (field is already dead), no record commit; done=true so
    // pollers that predate the aborted flag still terminate.
    if (tuningSweepAbortReason != 0) {
      tuningSweepActive = false;
      tuningSweepDone = true;
      out = TUNING_EASE_REST_A;
      return;
    }
    f = freqList[segIdx];
    if (!segStarted) {
      segStartMs = nowMs; segStarted = true; phase = 0.0f;
      sAmpsSin = sAmpsCos = sAmps = sSin = sCos = sAmpsSq = 0.0; nAcc = 0;
    }
  } else {
    f = fmaxf(0.1f, tuningSineFreq);   // manual sine (waveform==1)
  }

  // Advance phase, emit the sine reference.
  phase += 2.0f * (float)M_PI * f * dt;
  if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
  float s = sinf(phase);
  out = baseA + ampA * s;

  if (tuningWaveform == 2 && tuningSweepActive) {
    uint32_t tElapsed = nowMs - segStartMs;
    // Whole-sweep run-condition tracking: RPM range and field-duty rail (clipped sine).
    float rpmNow = (float)RPM;
    if (rpmNow < tuningSweepRpmMin) tuningSweepRpmMin = rpmNow;
    if (rpmNow > tuningSweepRpmMax) tuningSweepRpmMax = rpmNow;
    tuningSweepRpmSum += rpmNow; tuningSweepRpmN++;
    // High rail is the live ceiling (Max Field Volts works out to ~52%@24V / ~26%@48V at its default) — a fixed 99.5 could never
    // fire there and a clipped sweep would be accepted as a valid plant fit.
    if (dutyCycle <= MinDuty + 0.5f || dutyCycle >= ccDutyCeiling() - 0.5f) tuningSweepDutyRailed = true;
    // Integer-cycle window: settle 1 cycle, then accumulate over a WHOLE number of periods so the lock-in's
    // negative-frequency leakage cancels and the reference sin/cos sums go to ~0 (clean DC correction).
    float periodMs = 1000.0f / f;
    float segMs = (1.0f + (float)tuningSweepCycles) * periodMs;
    if (segMs < 1500.0f) segMs = 1500.0f;
    uint32_t accStartMs = (uint32_t)periodMs;                       // 1-cycle settle
    int nCyc = (int)floorf((segMs - periodMs) / periodMs);          // whole cycles after settle
    if (nCyc < 1) nCyc = 1;
    uint32_t accEndMs = accStartMs + (uint32_t)((float)nCyc * periodMs);
    if (tElapsed >= accStartMs && tElapsed < accEndMs) {   // accumulate over whole cycles only
      float c = cosf(phase);
      sAmpsSin += (double)measAmps * s;
      sAmpsCos += (double)measAmps * c;
      sAmps += measAmps; sSin += s; sCos += c; nAcc++;
      sAmpsSq += (double)measAmps * measAmps;   // for per-point coherence
    }
    if (tElapsed >= accEndMs) {
      // Samples actually landed per whole-cycle window = the live control-tick rate. Measured
      // rather than assumed: it sets the alias limit and is served to the dashboard fit.
      sFsSamples += (double)nAcc; sFsWindowMs += (double)((float)nCyc * periodMs);
      if (sFsWindowMs > 1.0) tuningSweepFsHz = (float)(sFsSamples * 1000.0 / sFsWindowMs);
      float gain = 0.0f, phaseDeg = 0.0f;
      if (nAcc > 0 && ampA > 0.01f) {
        double meanA = sAmps / (double)nAcc;
        double Ic = sAmpsSin - meanA * sSin;   // DC-corrected in-phase
        double Qc = sAmpsCos - meanA * sCos;   // DC-corrected quadrature
        float B = 2.0f * (float)sqrt(Ic * Ic + Qc * Qc) / (float)nAcc;
        gain = B / ampA;                                              // measured / reference
        phaseDeg = atan2f(-(float)Qc, (float)Ic) * 180.0f / (float)M_PI;  // output lag (deg)
        // atan2 folds at ±180°; lag only grows across a sweep, so re-add the turns. Unfolded, a
        // >180° point plots as a cliff and drags the dashboard's dead-time fit negative.
        while (phaseDeg < prevLagDeg - 180.0f) phaseDeg += 360.0f;
        while (phaseDeg > prevLagDeg + 180.0f) phaseDeg -= 360.0f;
        prevLagDeg = phaseDeg;
        // Coherence: fraction of the measured AC power explained by the drive-frequency sinusoid.
        // fitted power = B²/2; total AC power = variance of measAmps. Low = noisy/unreliable point.
        double varA = sAmpsSq / (double)nAcc - meanA * meanA;
        float coh = 1.0f;
        if (varA > 1e-9) {
          coh = (float)((0.5 * (double)B * (double)B) / varA);
          if (coh > 1.0f) coh = 1.0f;
          if (coh < 0.0f) coh = 0.0f;
        }
        if (gain > gMaxSeen) gMaxSeen = gain;
        // Only in-band points (within 6 dB of the strongest) feed worst-coherence: past roll-off low coherence is
        // dead signal, exactly where slow alternators sit, and would false-fail clean sweeps.
        if (gMaxSeen > 0.01f && gain >= 0.5f * gMaxSeen && coh < tuningSweepWorstCoh)
          tuningSweepWorstCoh = coh;
      }
      tuningBode[segIdx].freqHz   = f;
      tuningBode[segIdx].gain     = gain;
      tuningBode[segIdx].phaseDeg = phaseDeg;
      tuningBodeCount = segIdx + 1;
      queueConsoleMessageF("Tuning Bode %d: %.2f Hz gain=%.2f lag=%.0f deg (n=%lu)",
                           segIdx + 1, f, gain, phaseDeg, (unsigned long)nAcc);
      segIdx++; segStarted = false;
      if (segIdx >= TUNING_SWEEP_NPOINTS) {
        tuningSweepActive = false;   // tuningSweepDone set after the ease-down below completes
        queueConsoleMessage("Tuning sine sweep complete.");
        commitTuningSweepRecord();
        tuningEaseActive = true; tuningEaseStartMs = nowMs; tuningEaseFromA = baseA;
      }
    }
  }
}


// ===== SMALL SHARED HELPERS (prototypes live in Xregulator.ino). =====

// Preload a gzip-compressed web asset from LittleFS into PSRAM so the dashboard serves from RAM.
CachedAsset loadFileToRAM(const char *path) {
  CachedAsset result;
  File f = webFS.open(path, "r");
  if (!f) {
    Serial.printf("preload FAILED: %s\n", path);
    return result;
  }
  result.size = f.size();
  // PSRAM only — no internal-heap fallback: ~300KB of web bundle on the internal
  // heap would destroy the contiguous block TLS handshakes need. On failure
  // serveCachedAsset() returns false and the file is served from flash instead.
  result.data = (uint8_t *)ps_malloc(result.size);
  if (result.data && f.read(result.data, result.size) != result.size) {
    Serial.printf("preload short read: %s - serving from flash\n", path);
    free(result.data);
    result.data = nullptr;
    result.size = 0;
    f.close();
    return result;
  }
  if (result.data) {
    // FNV-1a over the loaded bytes — recomputed on every cacheWebAssets(), so a web-bundle
    // update mints a new ETag and browsers can never revalidate onto the old bundle.
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < result.size; i++) h = (h ^ result.data[i]) * 16777619u;
    snprintf(result.etag, sizeof(result.etag), "\"%08x-%x\"", h, (unsigned)result.size);
    Serial.printf("Preloaded %s into RAM (%d bytes)\n", path, result.size);
  } else {
    Serial.printf("preload malloc FAILED: %s\n", path);
    result.size = 0;
  }
  f.close();
  return result;
}

// Thermal-log scaling helpers (6_functions thermal logger packs floats into int16 ×10).
static int16_t thermalLogScale10(float v) {
  if (isnan(v) || isinf(v)) return 0;
  float scaled = v * 10.0f;
  if (scaled > 32767.0f) return 32767;
  if (scaled < -32767.0f) return -32767;   // NOT -32768: INT16_MIN is the log's "no reading" blank
  return (int16_t)lroundf(scaled);
}

static int16_t thermalLogScaleRPM(float v) {
  if (isnan(v) || isinf(v)) return 0;
  if (v > 32767.0f) return 32767;
  if (v < -32768.0f) return -32768;
  return (int16_t)lroundf(v);
}

static uint8_t thermalLogGetStageCode() {
  return getChargeStageDisplayCode();
}

// Ignition-cycle watermark helpers (IgnWatermark struct + wmIgn_* globals live in Xregulator.ino).
inline void wmIgnUpdate(IgnWatermark &w, float v) {
  if (!isfinite(v)) return;
  if (isnan(w.lo) || v < w.lo) w.lo = v;
  if (isnan(w.hi) || v > w.hi) w.hi = v;
}
// CSV emitter for a watermark: a sensor absent all ignition cycle stays NAN -> ROLL_EMPTY ("—"), never a fake 0 low/high.
inline int wmIgnCsv(float v, int scale) {
  if (!isfinite(v)) return ROLL_EMPTY;
  float x = v * (float)scale;   // lroundf on a non-finite or out-of-range float is undefined
  if (x >  2000000000.0f) return  2000000000;
  if (x < -1000000000.0f) return -1000000000;   // stays clear of ROLL_EMPTY, which must stay unreachable
  return (int)lroundf(x);
}

// ───── Charge health system calibration (commissioning stage 9): the alternator's speed lead ─────
// At fixed field, output answers where the engine is GOING (rotor flux lag), so the health tracker pairs output
// with speed via altLeadSec. Four throttle cycles on one clock (idle -> climb -> top hold -> idle). Cycle 1 SIZES
// the field AT THE TOP, the fastest the machine turns all run, so nothing later exceeds what sizing proved safe;
// cycles 2-4 run at that frozen duty, each fitted alone, chained with no button press (a button would pin the
// frozen field while nobody drives). Duty-override contract as fieldCut_tick; sizing drives the real PID via chcCcActive.

// Estimator over one capture span [lo,hi): pair amps[i] with rpm[i+k] (k = delay in CHC_SAMP_MS ticks) and score the
// count-weighted mean |rising - falling| gap over speed bins (still samples dropped); parabolic refine around the min.
// Returns lead s (0.0 is real), -1 if too few bins scored, -2 if the min sits at CHC_KMAX (lead out of range).
static float chcFitLead(int lo, int hi, float *gapBefore, float *gapAfter) {
  const int n = hi - lo;
  if (gapBefore) *gapBefore = -1.0f;
  if (gapAfter)  *gapAfter  = -1.0f;
  if (!chcBuf || lo < 0 || hi > chcCount || n < 40) return -1.0f;
  static float (&sumUp)[128] = PsramPlace::make<float[128]>();
  static float (&sumDn)[128] = PsramPlace::make<float[128]>();
  static uint16_t (&nUp)[128] = PsramPlace::make<uint16_t[128]>();
  static uint16_t (&nDn)[128] = PsramPlace::make<uint16_t[128]>();
  float cost[CHC_KMAX + 1];
  for (int k = 0; k <= CHC_KMAX; k++) {
    for (int b = 0; b < 128; b++) { sumUp[b] = sumDn[b] = 0.0f; nUp[b] = nDn[b] = 0; }
    for (int i = 0; i + k < n; i++) {
      int a = i + k - 3, c = i + k + 3;
      if (a < 0) a = 0;
      if (c > n - 1) c = n - 1;
      if (c <= a) continue;
      float rate = ((float)chcBuf[lo + c].rpm - (float)chcBuf[lo + a].rpm) / ((float)(c - a) * (CHC_SAMP_MS / 1000.0f));
      if (fabsf(rate) < CHC_RATE_MIN) continue;
      int b = (int)((float)chcBuf[lo + i + k].rpm / CHC_BIN_RPM);
      if (b < 0 || b > 127) continue;
      float amps = (float)chcBuf[lo + i].amps10 * 0.1f;
      if (rate > 0) { sumUp[b] += amps; nUp[b]++; } else { sumDn[b] += amps; nDn[b]++; }
    }
    float sw = 0.0f, sg = 0.0f; int nb = 0;
    for (int b = 0; b < 128; b++) {
      if (!nUp[b] || !nDn[b]) continue;
      float w = (float)((nUp[b] < nDn[b]) ? nUp[b] : nDn[b]);
      sg += w * fabsf(sumUp[b] / nUp[b] - sumDn[b] / nDn[b]);
      sw += w; nb++;
    }
    cost[k] = (nb >= CHC_MIN_BINS && sw > 0.0f) ? (sg / sw) : NAN;
  }
  int bi = -1;
  for (int k = 0; k <= CHC_KMAX; k++)
    if (!isnan(cost[k]) && (bi < 0 || cost[k] < cost[bi])) bi = k;
  if (bi < 0) return -1.0f;
  if (bi == 0) {
    // Zero delay IS the answer for a machine with no lead, but only when the next candidate is
    // worse — a lone scored point at k=0 is a fit that found nothing, not a minimum.
    if (isnan(cost[1]) || !(cost[1] > cost[0])) return -1.0f;
    if (gapBefore) *gapBefore = cost[0];
    if (gapAfter)  *gapAfter  = cost[0];
    return 0.0f;
  }
  if (bi >= CHC_KMAX) return -2.0f;
  if (isnan(cost[bi - 1]) || isnan(cost[bi + 1])) return -1.0f;
  float den = cost[bi - 1] - 2.0f * cost[bi] + cost[bi + 1];
  float d = (fabsf(den) > 1e-9f) ? (0.5f * (cost[bi - 1] - cost[bi + 1]) / den) : 0.0f;
  if (d < -1.0f) d = -1.0f;
  if (d >  1.0f) d =  1.0f;
  if (gapBefore) *gapBefore = isnan(cost[0]) ? -1.0f : cost[0];
  if (gapAfter)  *gapAfter  = cost[bi];
  return ((float)bi + d) * (CHC_SAMP_MS / 1000.0f);
}

// Delivered throttle pace over the samples the fit could use (|rate| >= CHC_RATE_MIN); reported only, nothing consumes it.
static void chcRateStats(int lo, int hi) {
  chcRateMin = chcRateMax = chcRateAvg = -1.0f;
  const int n = hi - lo;
  if (!chcBuf || lo < 0 || hi > chcCount || n < 8) return;
  double sum = 0.0; int k = 0;
  for (int i = 0; i < n; i++) {
    int a = i - 3, c = i + 3;
    if (a < 0) a = 0;
    if (c > n - 1) c = n - 1;
    if (c <= a) continue;
    float rate = fabsf(((float)chcBuf[lo + c].rpm - (float)chcBuf[lo + a].rpm)
                       / ((float)(c - a) * (CHC_SAMP_MS / 1000.0f)));
    if (rate < CHC_RATE_MIN) continue;
    if (chcRateMin < 0.0f || rate < chcRateMin) chcRateMin = rate;
    if (rate > chcRateMax) chcRateMax = rate;
    sum += rate; k++;
  }
  if (k > 0) chcRateAvg = (float)(sum / (double)k);
}

// Read one saved charge-rate column from NVS: the test is SIZED from one chosen by bank size (CHC_LOHI_AH), the HIGH
// column is always the guard. Web handler only: the control path never touches flash. capLimitMode 1 = columns are
// WATTS (divided by bus volts at use, as chcCapAmpsAt does); a missing column falls back to loadCapTablesForMode's default.
void chcLoadCapColumn(bool high, float *out) {
  const bool kw = (capLimitMode == 1);
  for (int i = 0; i < RPM_TABLE_SIZE; i++)
    out[i] = kw ? defaultCapPowerW(i, high)
                : (high ? defaultCapCurrentValues[i] : defaultCapCurrentValues[i] * LOW_MODE_CAP_FRACTION);
  nvs_handle_t h;
  if (nvs_open("learning", NVS_READONLY, &h) != ESP_OK) return;
  size_t sz = (size_t)RPM_TABLE_SIZE * sizeof(float);
  float tmp[RPM_TABLE_SIZE];
  const char *key = kw ? (high ? "capPowerTable" : "capPowerTableLo") : (high ? "capTable" : "capTableLo");
  if (nvs_get_blob(h, key, tmp, &sz) == ESP_OK && sz == (size_t)RPM_TABLE_SIZE * sizeof(float))
    for (int i = 0; i < RPM_TABLE_SIZE; i++) out[i] = tmp[i];
  nvs_close(h);
}

// One column's ceiling in AMPS at one speed, read as the control path does (W / bus V in kW mode, <= MaxTableValue).
// The bus for the charge-test backstops: the alarms' source (main reading, else the second one, each only if fresh
// and not ignored by the owner; none in Limp Home), else NAN, so the "bus reached the charge target" abort never
// silently drops out.
static float chcBusV() {
  float v;
  return alarmBatteryVoltage(v) ? v : NAN;
}

static float chcCapAmpsAt(float rpm, const float *table) {
  float a = interpolateRPMTable(rpm, table);
  if (capLimitMode == 1) {
    const float v = chcBusV();
    a = (v > 0.5f) ? a / v : 0.0f;   // no fresh voltage = a zero ceiling (fails safe): a held value must not size the run
  }
  return fminf(a, (float)MaxTableValue);
}

// The charge-rate limit a run would size from if Start were pressed now — the setup screen shows the
// test current in amps before any run exists. Reads flash (chcLoadCapColumn): web task only.
float chcPreviewCapA() {
  if (maxWorkingRpm <= 0) return 0.0f;
  float col[RPM_TABLE_SIZE];
  chcLoadCapColumn(BatteryCapacity_Ah >= CHC_LOHI_AH, col);
  return chcCapAmpsAt(CHC_TOP_FRAC * (float)maxWorkingRpm, col);
}

static void chcFinish(bool ok, const char *why) {
  chcOk = ok;
  chcResultsReady = true;
  // why == chcAbortMsg on the ready-phase backstops, which hand the buffer back to itself.
  if (why && why[0] && why != chcAbortMsg) {
    strncpy(chcAbortMsg, why, sizeof(chcAbortMsg) - 1);
    chcAbortMsg[sizeof(chcAbortMsg) - 1] = '\0';
  }
}

// Where the clock is. Cycle 1 = settle, climb, sizing hold, descent; cycles 2…1+CHC_MEAS_CYCLES =
// climb, hold, descent. Returns false once the schedule has run out.
static bool chcSchedule(uint32_t runMs, uint8_t &cyc, uint8_t &seg) {
  const uint32_t c1 = CHC_SETTLE_IDLE_MS + chcRampMs + CHC_SIZE_HOLD_MS + chcRampMs;
  const uint32_t cn = chcRampMs + CHC_TOP_HOLD_MS + chcRampMs;
  uint32_t t = runMs;
  if (t < c1) {
    cyc = 1;
    seg = (t < CHC_SETTLE_IDLE_MS) ? 0
        : (t < CHC_SETTLE_IDLE_MS + chcRampMs) ? 1
        : (t < CHC_SETTLE_IDLE_MS + chcRampMs + CHC_SIZE_HOLD_MS) ? 2 : 3;
    return true;
  }
  t -= c1;
  const uint32_t k = t / cn;
  if (k >= CHC_MEAS_CYCLES) { cyc = 1 + CHC_MEAS_CYCLES; seg = 3; return false; }
  cyc = (uint8_t)(2 + k);
  t -= k * cn;
  seg = (t < chcRampMs) ? 1 : (t < chcRampMs + CHC_TOP_HOLD_MS) ? 2 : 3;
  return true;
}

// Lowest and highest applied duty inside each measured cycle's window, and the ceiling term that first moved one.
// The estimator assumes a fixed field: a ceiling stepping the frozen field mid-cycle reads as up/down hysteresis
// and biases the lead, so that cycle is discarded. A clamp that holds steady across a whole cycle is a fixed field.
static float   chcCycDutyLo[CHC_MEAS_CYCLES], chcCycDutyHi[CHC_MEAS_CYCLES];
static uint8_t chcClampBind = 0;
static bool chcCycMoved(int m) { return chcCycDutyHi[m] - chcCycDutyLo[m] > 0.05f; }

// Fit one measured cycle as soon as its clock window closes, so the work is spread across the run
// instead of landing on one control tick at the end, and the wizard can show each answer as it lands.
static void chcFitCycle(int m) {
  if (chcCycMoved(m)) {
    chcLead[m] = chcGapB[m] = chcGapA[m] = -1.0f;
    queueConsoleMessageF("Charge health calibration: cycle %d discarded, %s moved the frozen field during it",
                         m + 2, fieldCeilBindName(chcClampBind));
    return;
  }
  chcLead[m] = chcFitLead(chcCycStart[m], chcCycEnd[m], &chcGapB[m], &chcGapA[m]);
  queueConsoleMessageF("Charge health calibration: cycle %d fit %s (%.3f s, gap %.1f -> %.1f A)",
                       m + 2, (chcLead[m] >= 0.0f) ? "ok" : (chcLead[m] <= -1.5f) ? "past 0.9 s" : "no answer",
                       chcLead[m], chcGapB[m], chcGapA[m]);
}

bool chc_tick(float &dutyOut, float ampsRaw, uint32_t nowMs) {
  static uint8_t  phase = 0;          // 0 idle, 1 settle, 2 sizing (climb + hold), 4 frozen cycles, 7 ease-out
  static uint32_t runStartMs = 0, phaseStartMs = 0, lastSampMs = 0, inBandSinceMs = 0;
  static float    entryDuty = 1.0f, holdDuty = 0.0f, preSetpoint = 0.0f, ampsEma = 0.0f;
  static uint8_t  lastCyc = 0;

  // A teardown outside this function (6_functions.ino, when the field leaves normal AUTO) zeroes
  // chcActive while chc_tick is not being called; without this the run would resume mid-phase.
  if (phase != 0 && chcActive == 0) { phase = 0; chcCcActive = false; return false; }

  // ── Abort — a cancel from the wizard or a protection cut latched into the same flag ──
  if (phase != 0 && chcAbortRequested) {
    chcAbortRequested = false;
    if (chcCcActive) { chcCcActive = false; setpointLimited = preSetpoint; }
    if (!chcResultsReady) chcFinish(false, chcAbortMsg[0] ? chcAbortMsg : "stopped");
    chcActive = 0; chcPhase = 0; chcLastEndMs = millis();
    phase = 0;
    return false;   // no ease-out: the caller ignores dutyOut on false and the normal path resumes bumpless from lastAppliedDuty
  }

  // ── IDLE ──
  if (phase == 0) {
    if (!chcRequested) return false;
    // A request is only consumed on a normal running control tick, so one pressed with the engine
    // stopped (or during a fault lockout) would otherwise sit armed and pin the field by itself
    // whenever running resumed. Expire it instead — the button is a metre away from the throttle.
    if ((uint32_t)(nowMs - chcReqMs) > 30000u) {
      chcRequested = false;
      chcFinish(false, "the engine stopped, or the field was switched off");
      queueConsoleMessage("Charge health calibration: start request expired — the control loop was not running (engine stopped or field off). Press Start again.");
      return false;
    }
    chcRequested = false;
    chcAbortRequested = false;   // a Cancel from after the last run must not eat this Start
    if (chcBuf == nullptr) chcBuf = (ChcSample *)ps_malloc(CHC_BUF_N * sizeof(ChcSample));
    if (!chcBuf) { chcFinish(false, "PSRAM alloc failed"); return false; }
    chcIdleRpm = (RPM > 0) ? RPM : 0.0f;
    chcTopRpm = CHC_TOP_FRAC * (float)maxWorkingRpm;
    chcRampMs = 0; chcRunMs = 0; chcCycle = 0; chcSeg = 0; chcFrozen = false;
    chcSizeCapA = 0.0f; chcTargetA = 0.0f; chcHoldDuty = 0.0f; chcCmdA = 0.0f;
    chcMinA = 0.0f; chcPeakA = 0.0f; chcCount = 0;
    chcLeadSec = -1.0f;
    for (int i = 0; i < 3; i++) { chcLead[i] = chcGapB[i] = chcGapA[i] = -1.0f; chcCycStart[i] = 0; chcCycEnd[i] = 0; }
    for (int i = 0; i < CHC_MEAS_CYCLES; i++) { chcCycDutyLo[i] = 1000.0f; chcCycDutyHi[i] = -1000.0f; }
    chcRateMin = chcRateMax = chcRateAvg = -1.0f;
    chcResultsReady = false; chcOk = false; chcAbortMsg[0] = '\0';
    // The same range guard the setup screen applies to the entered maximum, in case Start arrived
    // from a browser that did not check, or idle is higher today than when the number was entered.
    if (!(chcIdleRpm > 0.0f)) { chcFinish(false, "no engine speed"); return false; }
    if (chcTopRpm < chcIdleRpm + CHC_HOLD_ABOVE_IDLE) {
      snprintf(chcAbortMsg, sizeof(chcAbortMsg), "%d%% of %d rpm is %.0f rpm — too close to idle (%.0f) to leave the passes any range",
               (int)lroundf(CHC_TOP_FRAC * 100.0f), maxWorkingRpm, chcTopRpm, chcIdleRpm);
      chcFinish(false, chcAbortMsg);
      return false;
    }
    chcSizeCapA = chcCapAmpsAt(chcTopRpm, (BatteryCapacity_Ah >= CHC_LOHI_AH) ? chcHiTable : chcLoTable);
    chcTargetA = chcSizeCapA * (float)chcTestPct * 0.01f;
    if (chcTargetA < CHC_MIN_TEST_A) { chcFinish(false, "the charge-rate limit at the test speed is too small to measure"); return false; }
    chcRampMs = (uint32_t)((chcTopRpm - chcIdleRpm) / CHC_PACE_RPM_S * 1000.0f);
    if (chcRampMs < CHC_MIN_RAMP_MS) chcRampMs = CHC_MIN_RAMP_MS;
    entryDuty = fmaxf(lastAppliedDuty, fieldScale());
    chcActive = 1; chcPhase = 1; chcCycle = 1; lastCyc = 1;
    runStartMs = nowMs; phase = 1; phaseStartMs = nowMs;
    queueConsoleMessageF("Charge health calibration: started — idle %.0f, top %.0f rpm (%d%% of %d), %.1f s ramps; sizing for %.1f A (%d%% of the %.0f A %s column)",
                         chcIdleRpm, chcTopRpm, (int)lroundf(CHC_TOP_FRAC * 100.0f), maxWorkingRpm, chcRampMs / 1000.0f,
                         chcTargetA, (int)chcTestPct, chcSizeCapA, (BatteryCapacity_Ah >= CHC_LOHI_AH) ? "High" : "Low");
    return false;   // no field driven yet: normal charging continues through the settle
  }

  const float vTarget = (ChargingVoltageTarget > 1.0f) ? ChargingVoltageTarget : BulkVoltage;
  chcGuardCapA = chcCapAmpsAt((RPM > 0) ? RPM : chcIdleRpm, chcHiTable);

  if (phase != 7) {
    chcRunMs = nowMs - runStartMs;
    uint8_t cyc = 0, seg = 0;
    const bool onSchedule = chcSchedule(chcRunMs, cyc, seg);
    chcCycle = cyc; chcSeg = seg;

    // ── SETTLE: the run's first ten seconds at idle. Nothing is driven; normal charging continues. ──
    if (phase == 1) {
      chcPhase = 1;
      if (seg != 0) {
        preSetpoint = setpointLimited;
        ampsEma = ampsRaw; inBandSinceMs = 0;
        chcCmdA = chcCapAmpsAt(fminf((RPM > 0) ? RPM : chcIdleRpm, chcTopRpm), (BatteryCapacity_Ah >= CHC_LOHI_AH) ? chcHiTable : chcLoTable) * (float)chcTestPct * 0.01f;
        chcCcActive = true;   // hand the setpoint to the real current loop (branch in AdjustField)
        phase = 2; phaseStartMs = nowMs;
        queueConsoleMessage("Charge health calibration: climbing — the current loop follows the charge-rate column up to the top");
      }
      return false;
    }

    // ── SIZE (cycle 1 climb + top hold): the chcCcActive branch and real PID own duty; chcCmdA is chcTestPct of the
    //   cap column at the PRESENT speed (never above the top's), so the command is bounded on the way up. Freeze the
    //   applied duty once at the top, arrived, and in band for CHC_SETTLE_MS. ──
    if (phase == 2) {
      chcPhase = (seg >= 2) ? 3 : 2;
      const float *col = (BatteryCapacity_Ah >= CHC_LOHI_AH) ? chcHiTable : chcLoTable;
      chcCmdA = chcCapAmpsAt(fminf((RPM > 0) ? RPM : chcIdleRpm, chcTopRpm), col) * (float)chcTestPct * 0.01f;
      ampsEma += 0.05f * (ampsRaw - ampsEma);
      // Both run-ending ceilings apply here too: the bank charges under the sizing current just as it
      // does under a pass, and neither ceiling is survivable by continuing.
      if (ampsRaw > chcGuardCapA * CHC_I_ABORT_FRAC || !(chcBusV() <= vTarget - CHC_V_ABORT_MARGIN)) {
        chcCcActive = false;
        setpointLimited = preSetpoint;
        chcFinish(false, (ampsRaw > chcGuardCapA * CHC_I_ABORT_FRAC)
                         ? "output reached the charge-rate limit for this speed"
                         : (isnan(chcBusV()) ? "no fresh battery voltage" : "the bus reached the charge target"));
        chcActive = 0; chcPhase = 0; chcLastEndMs = millis(); phase = 0;
        return false;
      }
      const bool atTop = RPM >= chcTopRpm - fmaxf(CHC_TOL_FLOOR_RPM, CHC_TOL_FRAC * chcTopRpm);
      const bool arrived = fabsf(setpointLimited - chcCmdA) < 0.5f;
      const float band = fmaxf(2.0f, 0.08f * chcCmdA);
      bool settled = false;
      if (atTop && arrived && fabsf(ampsEma - chcCmdA) < band) {
        if (inBandSinceMs == 0) inBandSinceMs = nowMs;
        settled = (nowMs - inBandSinceMs >= CHC_SETTLE_MS);
      } else {
        inBandSinceMs = 0;
      }
      // Hold window closed without a settle: at the top with real output, proceed (the field railed first; the cycles
      // only need something to rise and fall). Off the top the run ends: a field sized lower leaves the top unbounded.
      if (!settled && (seg == 3 || !onSchedule)) {
        const bool boardHeld = boardCeilHoldsField();
        if (atTop && ampsEma >= CHC_MIN_TEST_A) {
          settled = true;
          queueConsoleMessageF("Charge health calibration: %.1f A not reachable here%s%s — proceeding at %.1f A", chcCmdA,
                               boardHeld ? ", field held by " : "", boardHeld ? fieldCeilBindName(g_fieldCeilBind) : "", ampsEma);
        } else {
          chcCcActive = false;
          setpointLimited = preSetpoint;
          if (!atTop) snprintf(chcAbortMsg, sizeof(chcAbortMsg), "the top speed was not reached — hold %.0f rpm through the marked stretch", chcTopRpm);
          else if (boardHeld) snprintf(chcAbortMsg, sizeof(chcAbortMsg), "the field reached its ceiling (%s) before making the test current",
                                       fieldCeilBindBrief(g_fieldCeilBind));
          chcFinish(false, (atTop && !boardHeld) ? "the field reached its ceiling before making the test current" : chcAbortMsg);
          chcActive = 0; chcPhase = 0; chcLastEndMs = millis(); phase = 0;
          return false;
        }
      }
      if (settled) {
        holdDuty = (lastAppliedDuty > 0.0f) ? lastAppliedDuty : MinDuty;
        chcHoldDuty = holdDuty;
        chcCcActive = false;
        // Restore the pre-test setpoint BEFORE returning true: the SystemID resume snapshot fires on this tick's
        // rising edge and must not capture the test target, or test end would re-assert the test current.
        setpointLimited = preSetpoint;
        chcMinA = ampsRaw; chcPeakA = ampsRaw;
        chcCount = 0; lastSampMs = 0;
        chcFrozen = true;
        phase = 4; phaseStartMs = nowMs; chcPhase = 4;
        queueConsoleMessageF("Charge health calibration: settled at %.1f A at %.0f rpm — field frozen at %.1f%%, %d measured cycles between %.0f and %.0f rpm",
                             ampsEma, RPM, holdDuty, CHC_MEAS_CYCLES, chcIdleRpm, chcTopRpm);
        dutyOut = holdDuty;
        return true;
      }
      return false;
    }

    // ── FROZEN: field pinned, one continuous recording across the rest of cycle 1 and the measured
    //   cycles. Each measured cycle is fitted the tick its clock window closes. ──
    if (phase == 4) {
      chcPhase = 4;
      dutyOut = holdDuty;
      if (ampsRaw > chcPeakA) chcPeakA = ampsRaw;
      if (ampsRaw < chcMinA)  chcMinA  = ampsRaw;
      if (lastSampMs == 0 || (uint32_t)(nowMs - lastSampMs) >= CHC_SAMP_MS) {
        lastSampMs = nowMs;
        if (chcCount < CHC_BUF_N) {
          chcBuf[chcCount].rpm = (uint16_t)constrain((int)lroundf(RPM), 0, 6400);
          chcBuf[chcCount].amps10 = (int16_t)constrain((int)lroundf(ampsRaw * 10.0f), -32000, 32000);
          chcCount++;
        }
      }
      // The two real ceilings. The field is open-loop here, so these are the run's own backstops —
      // the operator is never the protection.
      if (ampsRaw > chcGuardCapA * CHC_I_ABORT_FRAC) {
        snprintf(chcAbortMsg, sizeof(chcAbortMsg), "output reached the charge-rate limit for this speed");
        chcAbortRequested = true;
        return true;
      }
      const float busV = chcBusV();
      if (!(busV <= vTarget - CHC_V_ABORT_MARGIN)) {   // NAN (no fresh voltage at all) aborts too
        snprintf(chcAbortMsg, sizeof(chcAbortMsg), isnan(busV) ? "no fresh battery voltage" : "the bus reached the charge target");
        chcAbortRequested = true;
        return true;
      }
      // A ceiling crossing the frozen field (die warming, or the bus rising under the 10 A law) steps it mid-cycle.
      // lastCyc, not cyc: this tick's sample closes lastCyc's window on a boundary tick.
      if (lastCyc >= 2 && lastCyc - 2 < CHC_MEAS_CYCLES) {
        const int m = lastCyc - 2;
        const float applied = dutyAfterCeiling(fminf(holdDuty, 99.0f));
        const bool wasMoved = chcCycMoved(m);
        chcCycDutyLo[m] = fminf(chcCycDutyLo[m], applied);
        chcCycDutyHi[m] = fmaxf(chcCycDutyHi[m], applied);
        if (!wasMoved && chcCycMoved(m)) {
          chcClampBind = g_fieldCeilBind;
          queueConsoleMessageF("Charge health calibration: %s moved the frozen field between %.1f%% and %.1f%% in cycle %d, so that cycle will not be fitted",
                               fieldCeilBindName(chcClampBind), chcCycDutyLo[m], chcCycDutyHi[m], (int)lastCyc);
        }
      }
      // Cycle bookkeeping off the clock: a measured cycle's window is [first sample in it, first
      // sample of the next), and it is fitted the moment it closes.
      if (cyc != lastCyc || !onSchedule) {
        if (lastCyc >= 2) { const int m = lastCyc - 2; if (m < CHC_MEAS_CYCLES) { chcCycEnd[m] = chcCount; chcFitCycle(m); } }
        if (onSchedule && cyc >= 2 && cyc - 2 < CHC_MEAS_CYCLES) chcCycStart[cyc - 2] = chcCount;
        lastCyc = cyc;
      }
      if (onSchedule) return true;

      // Schedule over. Closest pair of the fits that produced an answer is the run's self-check;
      // the browser recomputes the same pair and stores the mean.
      chcRateStats(chcCycStart[0], chcCycEnd[CHC_MEAS_CYCLES - 1]);
      int nGood = 0, nMoved = 0; bool pastRange = false;
      for (int i = 0; i < CHC_MEAS_CYCLES; i++) {
        if (chcLead[i] >= 0.0f) nGood++;
        if (chcLead[i] <= -1.5f) pastRange = true;
        if (chcCycMoved(i)) nMoved++;
      }
      const float span = chcTopRpm - chcIdleRpm;
      if (nGood < 2 && nMoved > 0) {
        snprintf(chcAbortMsg, sizeof(chcAbortMsg), "the field ceiling (%s) moved the frozen field in %d of %d cycles",
                 fieldCeilBindBrief(chcClampBind), nMoved, CHC_MEAS_CYCLES);
        chcFinish(false, chcAbortMsg);
      } else if (nGood == 0) {
        if (pastRange) chcFinish(false, "the lead is past the 0.9 s the fit can see");
        else {
          snprintf(chcAbortMsg, sizeof(chcAbortMsg), "no usable measurement — %.0f to %.0f A over %.0f rpm of range gave the fit nothing to read",
                   chcMinA, chcPeakA, span);
          chcFinish(false, chcAbortMsg);
        }
      } else if (nGood == 1) {
        chcFinish(false, "only one cycle gave an answer — nothing to check it against");
      } else {
        int a = -1, b = -1; float best = 1e9f;
        for (int i = 0; i < CHC_MEAS_CYCLES; i++) {
          if (chcLead[i] < 0.0f) continue;
          for (int k = i + 1; k < CHC_MEAS_CYCLES; k++) {
            if (chcLead[k] < 0.0f) continue;
            const float d = fabsf(chcLead[i] - chcLead[k]);
            if (d < best) { best = d; a = i; b = k; }
          }
        }
        chcLeadSec = 0.5f * (chcLead[a] + chcLead[b]);
        chcFinish(true, "");
        queueConsoleMessageF("Charge health calibration: lead %.3f s (cycles %d+%d of %.3f / %.3f / %.3f s) over %.0f rpm; pace min %.0f avg %.0f max %.0f rpm/s",
                             chcLeadSec, a + 2, b + 2, chcLead[0], chcLead[1], chcLead[2], span, chcRateMin, chcRateAvg, chcRateMax);
      }
      phase = 7; phaseStartMs = nowMs; chcPhase = 6;
      return true;
    }
    return false;
  }

  // ── EASE back to the pre-test operating point ──
  if (phase == 7) {
    float frac = (float)(nowMs - phaseStartMs) / CHC_EASE_MS;
    if (frac >= 1.0f) {
      chcActive = 0; chcPhase = 0; chcLastEndMs = millis();
      phase = 0; dutyOut = entryDuty;
      return false;
    }
    dutyOut = holdDuty + (entryDuty - holdDuty) * frac;
    return true;
  }
  return false;
}
