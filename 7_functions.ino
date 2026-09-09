
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

// BEST-EVER FRONT — shared engine (Phase A). Generic over an axis count NAXIS so one C++
// instance serves each system: alternator (4-D), sail (3-D), motor (3-D). Templates can't be
// auto-prototyped, so this block must stay ABOVE the alt/boat front code below that instantiates
// it (this project keeps no .h files; the FRONT_* state #defines stay in Xregulator.ino because
// globals there use them). Design contract: BEST_EVER_FRONT_SPEC.md §2/§5 + IMPLEMENTATION_PLAN.md §2.
// 1. Episode<NAXIS>    — sliding-window steady-state detector (per-axis monotonic-deque min/max).
// 2. FrontStore<NAXIS> — sparse best-ever support points (never a grid) + IDW eval + device keep-gate.

// Per-axis steadiness knobs (live-tunable): deviation bound + how long it must hold.
struct EpAxisCfg { float tol; float steadySec; };

// One raw sample fed to the detector. x[] are the steadiness axes (band-checked AND averaged);
// ex[] are extra raw "passenger" inputs that are AVERAGED over the run but NOT band-checked — kept
// so the cloud retains the spec's diagnostic/forensic inputs (alt raw duty; motor raw AWS/AWA) for
// recomputing a derived axis or diagnosing a bad point. out is the measured output, averaged.
template <int NAXIS>
struct RawSample { float x[NAXIS]; float ex[2]; float out; uint32_t tMs; };

// One emitted episode point == one front support point. x[] are the SURFACE coordinates (which may
// be DERIVED from the steadiness axes, e.g. excitation from duty); ex[] are the retained raw extras
// (run-averaged, uploaded to the cloud's raw history, not used by the front eval); y is the output.
template <int NAXIS>
struct FrontPoint { float x[NAXIS]; float ex[2]; float y; uint32_t nSamp; uint32_t tEmit; };

// Steadiness detector — sliding-window form. An axis is "steady NOW" iff the max−min of that axis
// over the trailing steadySec window is ≤ tol. That's the textbook sliding-window-min/max problem,
// solved per axis with a pair of monotonic deques (O(1) amortized; no per-tick rescan, no O(N)
// spike). A brief excursion only widens the window range while it sits
// inside the window, then ages out — it never zeroes accumulated dwell, so a disqualifying sample
// does NOT discard the prior in-band history. A point emits, at most once per EP_EMIT_PERIOD_MS,
// while EVERY axis (and the optional output band) is steady AND data has spanned its dwell since the
// last hard barrier (eligible=false) AND the trailing boxcar (width avgWinMs, per instance) is
// itself clean: it spans a full window and each axis's TRIMMED range over its samples fits that
// axis's band (see trimmedRange). Steadiness + averaging are fed at a decimated EP_FEED_DT_MS
// cadence (the fold may run far faster — 200 Hz on alt). Shared by alt-health (4 axes + amps band,
// avgWinMs = ALT_EMIT_AVG_MS) and vessel-performance (sail/motor, 3 axes, output band disabled,
// default window).

#define EP_FEED_DT_MS      100    // steadiness/average update cadence (10 Hz), decimated from the fold
#define EP_AVG_WIN_MS     2000    // default trailing boxcar width for the emitted average (per-instance avgWinMs overrides)
#define EP_EMIT_PERIOD_MS 1000    // max emit rate while steady (≤ the ~2/s consumer drain; the front's
                                  // max-per-cell dedup collapses repeats in a long hold, so over-emitting is harmless)

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

template <int NAXIS>
struct Episode {
  EpAxisCfg cfg[NAXIS];             // per-axis {tol, steadySec}; synced by the caller each fold
  EpAxisCfg outCfg;                 // optional output-steadiness band (outCfg.tol <= 0 → disabled)
  uint32_t  minRunMs;               // retained for interface compatibility (unused by this detector)

  MonoDeque maxDQ[NAXIS + 1], minDQ[NAXIS + 1];   // per axis + [NAXIS] = output band: sliding window max/min
  int       dqCap[NAXIS + 1];       // each deque ring's capacity (samples), sized from maxDwellSec in init
  uint32_t  dataStartMs;            // time of the first eligible sample after the last hard barrier
  bool      haveData, ready;
  bool      axisSteady[NAXIS + 1];  // per-axis (+ output band) "steady now" — ADDITIVE: lets a caller build a
                                    // lighter gate from a subset of axes (alt-health session gate uses all but temp).

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
  // Gate-blame attribution (boot-cumulative). soleBlock = ticks where exactly ONE
  // axis was out-of-band with its dwell filled while every other axis was steady — during real
  // transients several axes fail together, so the sole blocker is the gate that alone cost a run.
  // emitDirty = candidate emits vetoed by that axis's trimmed boxcar range; emitSpanShort =
  // vetoed because the boxcar hadn't yet spanned avgWinMs. All dumped in /altdebug.csv.
  uint32_t  blameTicks = 0, soleBlock[NAXIS + 1] = {};
  uint32_t  emitDirty[NAXIS + 1] = {}, emitSpanShort = 0;

  // ringBuf/ringCap = the caller's PSRAM boxcar ring; maxDwellSec[NAXIS+1] sizes each axis's (and the
  // output band's) deque to its longest expected steady time. Deque storage is ps_malloc'd here.
  void init(RawSample<NAXIS> *ringBuf, int ringCap, const float *maxDwellSec) {
    avgRing = ringBuf; avgCap = ringCap;
    outCfg = { 0, 0 }; minRunMs = 0;
    ready = true;
    for (int a = 0; a < NAXIS + 1; a++) {
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

  // Reset all detector state (sliding-window history, boxcar, dwell origin). Called on a hard barrier
  // and by the caller's "Start Over". Safe even if init's alloc failed (clear() only zeroes indices).
  void clearRun() {
    for (int a = 0; a < NAXIS + 1; a++) { maxDQ[a].clear(); minDQ[a].clear(); }
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
    // A feed gap is a hard barrier too: several caller paths stop feeding WITHOUT clearRun
    // (stale NMEA, pause, IgnoreTemperature toggle). After a gap the window deques evict to a
    // single sample (trivially in-band) while dwell still measures from the pre-gap dataStartMs,
    // so one instantaneous post-gap reading would emit as a full steady run.
    if (haveData && (uint32_t)(s.tMs - lastFeedMs) > 5u * EP_FEED_DT_MS) clearRun();
    lastFeedMs = s.tMs;
    if (!haveData) { dataStartMs = s.tMs; haveData = true; }

    // Per-axis sliding-window min/max over each axis's own trailing dwell window (clamped to storage).
    feedTicks++;
    int nBad = 0, badAxis = -1; bool badDwellOk = false;
    bool qualified = true;
    for (int a = 0; a < NAXIS; a++) {
      uint32_t win = (uint32_t)(cfg[a].steadySec * 1000.0f);
      uint32_t winMax = (uint32_t)((dqCap[a] - 2) * EP_FEED_DT_MS);   // can't window more than the ring holds
      if (win > winMax) win = winMax;
      maxDQ[a].push(s.tMs, s.x[a], win);
      minDQ[a].push(s.tMs, s.x[a], win);
      bool dwellOk = ((uint32_t)(s.tMs - dataStartMs) >= win);
      bool inBand  = (maxDQ[a].front() - minDQ[a].front() <= cfg[a].tol);
      if (dwellOk) {
        if (!inBand) { axUnqual[a]++; if (!axWasUnq[a]) axExcur[a]++; }
        axWasUnq[a] = !inBand;
      }
      axisSteady[a] = dwellOk && inBand;
      if (!axisSteady[a]) { qualified = false; nBad++; badAxis = a; badDwellOk = dwellOk; }
    }
    if (outCfg.tol > 0) {
      uint32_t win = (uint32_t)(outCfg.steadySec * 1000.0f);
      uint32_t winMax = (uint32_t)((dqCap[NAXIS] - 2) * EP_FEED_DT_MS);
      if (win > winMax) win = winMax;
      maxDQ[NAXIS].push(s.tMs, s.out, win);
      minDQ[NAXIS].push(s.tMs, s.out, win);
      bool dwellOk = ((uint32_t)(s.tMs - dataStartMs) >= win);
      bool inBand  = (maxDQ[NAXIS].front() - minDQ[NAXIS].front() <= outCfg.tol);
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
    avgRing[ringHead] = s;
    ringHead = (ringHead + 1) % avgCap;
    if (ringCount < avgCap) ringCount++;
    for (int a = 0; a < NAXIS; a++) avgSumX[a] += s.x[a];
    avgSumEx[0] += s.ex[0]; avgSumEx[1] += s.ex[1]; avgSumOut += s.out;
    while (ringCount > 1) {
      int tail = (ringHead - ringCount + avgCap) % avgCap;
      if ((uint32_t)(s.tMs - avgRing[tail].tMs) <= avgWinMs) break;
      for (int a = 0; a < NAXIS; a++) avgSumX[a] -= avgRing[tail].x[a];
      avgSumEx[0] -= avgRing[tail].ex[0]; avgSumEx[1] -= avgRing[tail].ex[1]; avgSumOut -= avgRing[tail].out;
      ringCount--;
    }
    count = qualified ? (uint32_t)ringCount : 0;            // caller reads count>0 as "steady now"

    // Emit a steady point, rate-limited. Beyond instant steadiness (qualified), the BOXCAR itself
    // must be clean: full avgWinMs span, and each axis's trimmed range over its samples within that
    // axis's band. This replaced the 2026-08-14 "steady continuously for a full avgWinMs" clock:
    // one 100 ms blip poisons the 3 s instant windows for 3 s, so with blips every few seconds the
    // clock never reached 8 s and the record book starved (bench 2026-08-20: 12 min steady run,
    // ONE record, live % unmoored). Trimming lets isolated blips ride in the average (unbiased,
    // band-bounded smear) instead of vetoing the emit; a sustained shift still fails the range.
    if (qualified && (uint32_t)(s.tMs - lastEmitMs) >= EP_EMIT_PERIOD_MS && ringCount > 2) {
      int tail = (ringHead - ringCount + avgCap) % avgCap;
      bool clean = ((uint32_t)(s.tMs - avgRing[tail].tMs) + 2u * EP_FEED_DT_MS >= avgWinMs);
      if (!clean) emitSpanShort++;
      for (int a = 0; clean && a < NAXIS; a++)
        if (trimmedRange(a) > cfg[a].tol) { clean = false; emitDirty[a]++; }
      if (clean && outCfg.tol > 0 && trimmedRange(NAXIS) > outCfg.tol) { clean = false; emitDirty[NAXIS]++; }
      if (clean) {
        lastEmitMs = s.tMs;
        if (out) {
          for (int a = 0; a < NAXIS; a++) out->x[a] = (float)(avgSumX[a] / (double)ringCount);
          out->ex[0] = (float)(avgSumEx[0] / (double)ringCount);
          out->ex[1] = (float)(avgSumEx[1] / (double)ringCount);
          out->y = (float)(avgSumOut / (double)ringCount);
          out->nSamp = (uint32_t)ringCount;
          out->tEmit = s.tMs;
          return true;
        }
      }
    }
    return false;
  }

  // Trimmed max−min over the boxcar ring for one axis (axis == NAXIS → the output). Drops the
  // ~5% most extreme samples per tail (ringCount/20, min 1, cap 8) before ranging, so a few
  // isolated 100 ms blips can't veto an emit whose core is genuinely steady — the blips stay IN
  // the emitted average. A sustained excursion (> the trim count) still fails. O(n·trim) once
  // per candidate emit; also read (unlocked, diagnostic) by altDebugCsvSend.
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

  // Dwell seconds still needed before this episode can go steady: the largest per-axis shortfall of
  // in-band data since dataStartMs. 0 once every axis has spanned its window (steadiness is then purely
  // the in-band test); -1 with no eligible data. Only meaningful during the initial fill of a fresh run.
  float settleRemainSec(uint32_t nowMs) {
    if (!haveData) return -1.0f;
    float rem = 0.0f;
    for (int a = 0; a < NAXIS; a++) {
      uint32_t win = (uint32_t)(cfg[a].steadySec * 1000.0f);
      uint32_t winMax = (uint32_t)((dqCap[a] - 2) * EP_FEED_DT_MS);
      if (win > winMax) win = winMax;
      uint32_t el = (uint32_t)(nowMs - dataStartMs);
      if (el < win) { float r = (win - el) / 1000.0f; if (r > rem) rem = r; }
    }
    return rem;
  }
};

// Sparse support points (never a grid). Memory scales with the data, not the input volume —
// what makes the 4-D alternator affordable. axisScale[] normalizes each dimension's distance
// for IDW (≈ that axis's tol or characteristic span).
// Hard cap on neighbors entering an LWLR solve — bounds the software-double accumulation cost in
// dense neighborhoods (~48 pts ≈ 1–2 ms worst case) independent of front size. See evalLWLR.
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
  // Max-per-cell insert: keep the store a true upper ENVELOPE, not a point cloud. If an incoming run
  // lands in the same operating cell as an existing point (within half an axisScale on EVERY axis),
  // keep only the higher-y one — so eval() interpolates between bests instead of averaging a best with
  // also-rans. Returns true ONLY on a meaningful improvement (new cell, or beating the cell's record
  // by ≥0.5% — a bare float compare re-admitted every +0.01A creep at steady state, churning the cloud
  // upload queue; rationale in BEST_EVER_FRONT_SPEC.md §5b); callers use that to gate cloud upload.
  bool add(const FrontPoint<NAXIS> &p) {
    for (int i = 0; i < count; i++) {
      bool sameCell = true;
      for (int a = 0; a < NAXIS; a++) {
        float sc = (axisScale[a] > 1e-9f) ? axisScale[a] : 1.0f;
        if (fabsf(p.x[a] - pts[i].x[a]) > 0.5f * sc) { sameCell = false; break; }
      }
      if (sameCell) {
        if (p.y > pts[i].y * 1.005f + 0.01f) { pts[i] = p; return true; }   // meaningfully better at this cell
        return false;                                                        // the existing record stands
      }
    }
    if (count >= cap) return false;
    pts[count++] = p; return true;
  }
  // IDW surface evaluation, O(count) — the spec's front_eval(). Float + precomputed reciprocal axis
  // scales (4 mults/point, not 4 divides). Reached only via classify()/pushesHybrid() at 1 Hz
  // (altHealth_tick, behind gHeavyRanThisPass) — never from the 200 Hz altFold_tick, so front size
  // has no control-loop cost.
  // A convex blend of the support points → the result is ALWAYS within their y-range: never extrapolates.
  // d_i = sqrt(Σ_a ((x[a]-pts.x[a])*invSc[a])^2); exact hit → that point's y; else Σ w_i y_i / Σ w_i.
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
  // Device keep-gate, IDW-bar-only form — SUPERSEDED by pushesHybrid at every call site; kept as
  // the simple reference (unused template methods cost no flash). Only apply either gate when
  // hasLocalSupport() is true.
  bool pushes(const float x[NAXIS], float y, float safetyMargin, float idwPower) const {
    if (count <= 0) return true;
    return y > eval(x, idwPower) - safetyMargin;
  }
  // Any support point within the same cell (the half-axisScale box add() dedupes in)? A run landing
  // in an unvisited cell is admitted unconditionally — it opens that region at its true value, and
  // add()'s max-per-cell keeps later, better runs.
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
  // Normalized (axis-scaled) distance to the nearest support point — the "is the reference
  // trustworthy here" test. Beyond the caller's radius the live % and trend report no reference
  // instead of a ratio against a blend of faraway points. Empty front → huge distance.
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
  // Locally weighted linear regression (LWLR) — the display/trend evaluator. Fits the local slope
  // y ≈ b0 + Σ slope_a·dx_a (normalized dx, same 1/(d²+1e-9) weights as eval); prediction = b0,
  // which CAN sit below the lowest neighbor — removes IDW's edge bias. Ridge on the slope diagonal
  // only; prediction clamped to [0.1, 1.25 × max stored y]; slopesOut feeds the slope-gap risk
  // test. Returns false on an empty front or degenerate solve. Solve runs over only the nearest
  // ≤ FRONT_LWLR_KMAX points — a hard radius cut was tried and REJECTED. Full rationale, validation
  // numbers, and cost model: ALT_HEALTH_LWLR_ENGINE_SPEC.md.
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
    float hi = 1.25f * maxY;                                  // clamp: NO neighbor-range clamp (that
    if (pred < 0.1f) pred = 0.1f;                             // reintroduces the edge bias) — just a
    if (pred > hi) pred = hi;                                 // sanity ceiling over the record book
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
  // Confidence-state classifier + display prediction (the 1 Hz evaluator). Test order: MEASURED
  // (same-cell record) → NO_REFERENCE (beyond refRadius) → risk gate (slope-gap / curvature) →
  // LEARNING_EDGE or ESTIMATED. KEY INVARIANT: OUTPUT-BLIND — measured output never enters the
  // state decision, so degradation can't relabel itself "learning". predOut = LWLR (IDW fallback
  // for MEASURED on a degenerate solve; 0 = show no number). barOut = min(IDW, LWLR) admission bar
  // from the same solve. Full design: ALT_HEALTH_LWLR_ENGINE_SPEC.md.
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

// ALTERNATOR HEALTH — Best-Ever Front (device side). Folds one sample per control tick (~200 Hz,
// via the pidLog hook) into the Episode detector; an emitted steady-run average pushes the sparse
// best-ever output-amps front (FrontStore). The cloud prunes dominated points + retains raw
// history. Surface axes {RPM, excitation, Vbus, temp}; excitation derived from run-average
// duty/Vbus/temp. Generic engine + globals: Xregulator.ino. Design:
// BEST_EVER_FRONT_SPEC.md / BEST_EVER_IMPLEMENTATION_PLAN.md.

#define ALT_VER          4u
#define ALT_FRONT_MAGIC  0x414C4652u  // 'ALFR' — best-ever front point blob
#define ALT_TREND_MAGIC  0x414C5452u  // 'ALTR' (legacy whole-blob trend; superseded by the append log)
#define ALT_TRENDLOG_MAGIC 0x414C544Cu // 'ALTL' — append-only engine-hour trend log

// Temp-normalized field drive ("excitation proxy"): (dutyFrac × Vbus) / (1 + α(Tc − Tref)).
static inline float altExcitation(float duty, float vbus, float tF) {
  float tc = (tF - 32.0f) / 1.8f;
  float denom = 1.0f + ALT_ALPHA_PER_C * (tc - ALT_TREF_C);
  if (denom < 0.5f) denom = 0.5f;
  return (duty / 100.0f) * vbus / denom;
}

// ---- Best-Ever Front engine instance (alternator 4-D) ----
// Steadiness/averaging axes: {RPM, field-duty %, Vbus, tempF}. Defined here (before every function
// that references them) so the rest of the module can use the front. Generic engine: Xregulator.ino.
#define ALT_NAXIS        4
#define ALT_EMIT_AVG_MS  2000     // record/grade averaging window. Sized from /altwinstats.csv on a real
                                  // idle hour (2026-08-21): luckiest-2s-window inflation 0.25% vs 0.13%
                                  // at 8 s, but 8 s passed only 11 of 53 ≥2 s steady runs — the old 8 s
                                  // guess starved harvest (and all transient capture) for ~0.1% inflation
#define ALT_FRONT_CAP    4096     // sparse support points (PSRAM); sized to be unreachable even AP-mode/no-prune — cost scales with count, not cap (see ALT_HEALTH_LWLR_ENGINE_SPEC.md)
#define ALT_EP_RING_CAP  1024     // Episode trailing-boxcar buffer (PSRAM). Only ~ALT_EMIT_AVG_MS of
                                  // decimated samples are ever live in it (~80 at 10 Hz); generously
                                  // sized. The steadiness windows live in the per-axis monotonic
                                  // deques (allocated in Episode::init), NOT here.
#define ALT_PENDING_CAP  4096     // = front cap: holds every unsynced point through weeks offline (PSRAM)

static Episode<ALT_NAXIS>     altEpisode;
static FrontStore<ALT_NAXIS>  altFront2;                // "My History" — the LEARNED surface (always the learn target)
static FrontStore<ALT_NAXIS>  altFrontUp;               // "Uploaded File" — a borrowed surface, resident alongside My History
static RawSample<ALT_NAXIS>  *altEpRing   = nullptr;
static FrontPoint<ALT_NAXIS> *altFrontBuf = nullptr;
static FrontPoint<ALT_NAXIS> *altFrontUpBuf = nullptr;  // backing store for the Uploaded surface
static bool altHaveUpload     = false;                  // true once a file has been uploaded (Uploaded surface populated)
static FrontPoint<ALT_NAXIS> *altPending  = nullptr;   // accepted-since-last-upload (raw points out)
static int altPendingCount   = 0;

// ---- session-plot series (PSRAM ring, RAM-only) ----------------------------------------------
// The session % plot used to exist ONLY in the browser tab: a page opened mid-session drew an empty
// chart, and a WiFi drop left a permanent hole (the live stream never backfills). The device now
// keeps the same 5 s series itself and serves it at /altsess.csv, so any page — fresh, reloaded, or
// reconnecting — recovers the whole run. Deliberately never persisted: a session ends at field-off,
// and writing it would put flash in the control path.
#define ALT_SESS_DT_MS    5000u   // MUST match the dashboard's dot cadence (altSessionPush in script.js)
#define ALT_SESS_RING_N   1440    // 2 h at ALT_SESS_DT_MS, ~32 KB PSRAM. Oldest rolls off. This only
                                  // has to cover "how long was I away / disconnected" — an open page
                                  // keeps ~40 h of its own. Uniform cadence on purpose: the plot's
                                  // information IS dot density, so decimating old data would read as
                                  // a change in the machine rather than a change in sampling.
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

// ---- session-temp gate: a lighter temp dwell (half of altThermSec) than the Episode's full temp dwell ----
// Same monotonic-deque sliding-window min/max as Episode, fed the same decimated tF. Lets the Session
// plot show a dot once temp has held for HALF the full steady time, while the surface/trend still wait
// for the full dwell. PSRAM-backed (cap sized in init for ample headroom past any half-dwell).
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
  void feed(bool eligible, float tF, uint32_t nowMs, float tol, float secs) {
    if (!eligible) { clear(); return; }
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
static AltTempGate altSessTempGate;
static String altPendingSeededFrom = "";   // non-empty → this pending batch is an adopted import (provenance tag)
static int altFrontEmitCount = 0;        // episode points emitted (whether or not they pushed the front)

// Per-axis steady-time knobs + front/eval knobs (registry-wired below; per-axis tol + floors are in
// Xregulator.ino). altPruneK is echoed to the cloud but applied cloud-side.
float altRpmSec       = 3.0f;    // RPM steady time (s)
float altDutySec      = 3.0f;    // field-duty % steady time (s)
float altVbusSec      = 3.0f;    // bus-voltage steady time (s)
float altThermDegF    = 4.0f;    // temperature deviation bound (°F) — record only at thermal equilibrium. 0.105 A/°F at cruise; also gates the altThermSec dwell, so it is the biggest yield knob
float altThermSec     = 40.0f;   // STEADY_TEMP_SEC — FULL-steady temp dwell (s). Feeds the surface + trend + orange ring.
                                 // 80→40 2026-08-20: at 80 the normal warm-up ramp (~1.5-2 °F/min) ate half the 4 °F band
                                 // and starved records for whole sessions. NVS-held — bench device needs a live set too.
// SESSION-steady temp dwell is DERIVED as half of altThermSec (see altSessTempDwell()) — no separate
// knob, so the session gate auto-tracks whenever the full dwell above is changed.
static inline float altSessTempDwell() { return altThermSec * 0.5f; }
// ---- engine-hours trend knobs (spec §5) ----
float altTrendBucketSec = 3600.0f;  // TREND_BUCKET_SEC — engine-seconds per trend bucket (production 3600 = 1 h; testing 600)
float altTrendFeedSec   = 10.0f;    // TREND_FEED_SEC — min spacing between graded samples entering a bucket (intake throttle)
float altTrendMinSamp   = 2.0f;     // MIN_SAMPLES — a bucket needs ≥ this many graded steady-run samples before it commits
// Output-steadiness band (5th criterion: the measured amps themselves must hold steady — directly
// guards what gets recorded, letting the input bands stay tight) + detector signal conditioning:
float altAmpsTolPct   = 2.5f;    // output-amps band, % of the filtered reading — backstop sitting above the ~1.9% p-p the four input bands can pass
float altAmpsFloorA   = 0.5f;    // output-amps band floor (A) — governs below ~20 A (2.5% takes over above). Was 1.0,
                                 // halved 2026-08-27: measured steady 3 s amps range at quiet idle was 0.35 A median /
                                 // 0.42 A p90, and 1.0 A let a "steady" record hide 13% of output at a 7.5 A cell
float altAmpsSec      = 3.0f;    // output-amps steady time (s)
float altEmaSec       = 0.5f;    // EMA time constant (s) on detector inputs RPM/duty/Vbus/amps (0 = off)
float altMinRunSec    = 2.0f;    // minimum steady-run length to emit a point (s)
float altRefRadius    = 2.0f;    // normalized nearest-support distance beyond which live % + trend report no reference
float altSafetyMargin = 0.0f;    // amps — gate keeps only runs that strictly beat the front (no keep-bias: the cloud only prunes, so sub-front samples were pure pollution of the local eval surface)
float altIdwPower     = 2.0f;    // IDW power for the admission-bar/curvature IDW (display prediction is LWLR)
float altPruneK       = 6.0f;    // cloud prune neighbor count (echoed; applied cloud-side)
float altRidgeFrac    = 0.10f;   // LWLR ridge fraction (× slope-block trace/NAXIS) — fit stability vs slope fidelity
float altRiskThresh   = 0.15f;   // classifier risk above which an in-radius point shows "learning" instead of a %
// High-field-low-output alert thresholds (independent safety net — see altHealth_tick):
float altHiFieldPct   = 80.0f;   // high-drive threshold as % of the live field ceiling (ccDutyCeiling), so it works on 24/36/48V compressed spans
float altLowOutAmps   = 10.0f;   // output at/below this counts as low (A)
float altHiFieldSec   = 30.0f;   // both conditions must persist this long before the alert fires (s)

// GUI-adjustable settings registry (defined here, before altDebugCsvSend uses it; handlers/echo live
// further down). One float registry → one /get handler loop + boot-load loop + "AltSettings" echo.
struct AltSetting { const char *name; float *ptr; };
static AltSetting ALT_SETTINGS[] = {
  {"altRpmTol", &altRpmTol},   {"altRpmSec", &altRpmSec},
  {"altDutyTolPct", &altDutyTolPct}, {"altDutySec", &altDutySec},
  {"altVbusTol", &altVbusTol}, {"altVbusSec", &altVbusSec},
  {"altThermDegF", &altThermDegF}, {"altThermSec", &altThermSec},
  {"altTrendBuckSec", &altTrendBucketSec},                             // TREND_BUCKET_SEC (name ≤15 chars; var is altTrendBucketSec)
  {"altTrendFeedSec", &altTrendFeedSec},                               // TREND_FEED_SEC (trend intake throttle)
  {"altTrendMinSamp", &altTrendMinSamp},                               // MIN_SAMPLES (bucket commit gate)
  {"altAmpsTolPct", &altAmpsTolPct}, {"altAmpsFloorA", &altAmpsFloorA}, {"altAmpsSec", &altAmpsSec},
  {"altEmaSec", &altEmaSec}, {"altMinRunSec", &altMinRunSec}, {"altRefRadius", &altRefRadius},
  {"altMinAmps", &altMinAmps}, {"altMinDuty", &altMinDuty},
  {"altSafetyMargin", &altSafetyMargin}, {"altIdwPower", &altIdwPower}, {"altPruneK", &altPruneK},
  {"altRidgeFrac", &altRidgeFrac}, {"altRiskThresh", &altRiskThresh},
  {"altHiFieldPct", &altHiFieldPct}, {"altLowOutAmps", &altLowOutAmps}, {"altHiFieldSec", &altHiFieldSec},
  {"altPaused", &altPaused},
};
static const size_t ALT_SETTING_COUNT = sizeof(ALT_SETTINGS) / sizeof(ALT_SETTINGS[0]);

// ---- performance-vs-engine-hours trend (engine-hour buckets: average + P10 output-%) ----
// Source = FULL-steady runs only, graded against the active surface, throttled to one per
// altTrendFeedSec (fed from altProcessEmits). A bucket spans altTrendBucketSec of engine-time
// (production 3600 = 1 h; testing 600) and stores its samples' average + low-decile (P10) output-%
// (from a 2%-bin histogram — the raw min made one unlucky sample define the hour's lower line; real
// degradation drags a tenth of the hour down and the P10 with it). It
// commits when the engine-time bucket index advances AND it holds ≥ altTrendMinSamp samples; a bucket
// short of that many qualifying steady samples shows a gap, by design (spec §2/§5). The in-progress
// bucket (sum/count/hist/index) PERSISTS across reboot, so a 50-min partial hour commits after only
// 10 more minutes of the next session — the commit fires on the real engine-time rollover regardless
// of how many reboots happen inside the bucket. NO clamp — % may exceed 100 vs a stale reference.
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
static void altTrendAdd(float perfFrac) {
  int eh = altTrendBucketIndex();
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
  // NOTE: the partial bucket is NOT written here. It is persisted only at the ignition-off shutdown
  // flush (altHealthSave → altPersistTrendBucket, Phase 2 after the field cuts) and on Reset. Every
  // shutdown goes through field-off while the device is still powered, so that one write always lands —
  // no per-tick NVS churn needed.
  // (deliberately no healthy/drifting verdict here — trends are read from the plots)
}
// Persist the in-progress bucket to NVS (4 scalars + the P10 histogram as sparse "bin:count" pairs).
// The committed buckets live in /alttrend.bin; this covers ONLY the partial current bucket so a
// reboot mid-bucket keeps its accumulated samples.
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

// ---- NMEA-style bench simulator (no engine) ----
// Sweeps RPM × excitation points, synthesizes amps from a near-deterministic model × a slow
// degradation ramp + noise so the curve fills and the trend declines. Writes only alt sim vars.
static float altSimRpm = 0, altSimExc = 0, altSimT = 80, altSimV = 13.6f, altSimAmps = 0;
static float altSimDuty = 50.0f;   // synthetic field duty % consistent with altSimExc (spreads the front's excitation axis)
static int   altSimIdx = 0, altSimLoops = 0;
static uint32_t altSimPtStartMs = 0;
static float altSimDeg = 1.0f;
#define ALT_SIM_NRPM 8
#define ALT_SIM_NEXC 5
#define ALT_SIM_NPTS (ALT_SIM_NRPM * ALT_SIM_NEXC)
#define ALT_SIM_HOLD_MS 40000u    // floor on the per-point hold; altSimTick extends it to altThermSec+30 s
static void altSimTick(uint32_t nowMs) {
  // Hold each point longer than the temperature dwell (+margin) so the sliding-window temp band
  // clears the inter-point temp jump (tF varies per point below) and the run can qualify + emit.
  uint32_t hold = (uint32_t)(altThermSec * 1000.0f) + 30000u;
  if (hold < ALT_SIM_HOLD_MS) hold = ALT_SIM_HOLD_MS;
  if (altSimPtStartMs == 0) altSimPtStartMs = nowMs;
  if (nowMs - altSimPtStartMs >= hold) {
    altSimPtStartMs = nowMs;
    if (++altSimIdx >= ALT_SIM_NPTS) { altSimIdx = 0; altSimLoops++; }
  }
  int ri = altSimIdx / ALT_SIM_NEXC, ei = altSimIdx % ALT_SIM_NEXC;
  float rpm = 800.0f + ri * 400.0f;             // 800..3600
  float exc = 3.0f + ei * 2.5f;                 // 3..13
  float tF = 80.0f + 0.02f * rpm;               // hotter at higher RPM
  float vbus = 13.6f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f);
  float a = 1.4f * exc * (1.0f - expf(-rpm / 1200.0f));   // rises with rpm knee + excitation
  a *= 1.0f - 0.0008f * (tF - 77.0f);                     // derate with temp
  if (altSimLoops >= 2) altSimDeg -= 0.0006f;             // after 2 full sweeps, degrade
  if (altSimDeg < 0.80f) altSimDeg = 0.80f;
  a *= altSimDeg;
  a *= 1.0f + (float)random(-20, 21) / 1000.0f;          // ±2% noise
  if (a < 0.5f) a = 0.5f;
  altSimRpm = rpm; altSimExc = exc; altSimT = tF; altSimV = vbus; altSimAmps = a;
  altSimDuty = exc * 100.0f / vbus;             // invert excitation → duty so the duty axis tracks exc
}

// Per-axis tol from the deviation-bound knobs, steady time from the *Sec knobs. Resynced every
// fold so live knob edits take effect immediately. ampsFilt sizes the relative output band
// (percent-of-reading with an absolute floor — one knob pair works at 5 A float and 150 A bulk).
static void altEpisodeSyncCfg(float ampsFilt) {
  altEpisode.cfg[0] = { altRpmTol,     altRpmSec  };   // RPM (filtered)
  altEpisode.cfg[1] = { altDutyTolPct, altDutySec };   // field duty % (filtered, absolute % points)
  altEpisode.cfg[2] = { altVbusTol,    altVbusSec };   // Vbus (V, filtered)
  altEpisode.cfg[3] = { altThermDegF,  altThermSec };  // temp (°F)
  float aTol = altAmpsTolPct * 0.01f * ampsFilt;
  altEpisode.outCfg = { (aTol > altAmpsFloorA) ? aTol : altAmpsFloorA, altAmpsSec };
  altEpisode.minRunMs = (altMinRunSec > 0) ? (uint32_t)(altMinRunSec * 1000.0f) : 0;
}

// Emitted-run hand-off: the ~200 Hz fold only stashes here; grading/admission run in the 1 Hz
// altProcessEmits so the control path never pays a solve.
#define ALT_EMIT_QUEUE 8
struct AltEmitPending { FrontPoint<ALT_NAXIS> sp; };
static AltEmitPending altEmitQ[ALT_EMIT_QUEUE];
static int altEmitQCount = 0;
static bool altCapWarned = false;   // once per boot OR per Start Over (cleared in resetAlternatorHealth)

// ---- per-control-tick fold (THE canonical cadence) ----
// ---- emit-window sizing probe — TEMPORARY DIAGNOSTIC, REMOVE AFTER AUGUST 2026 ----
// Not a feature. It exists to pick ONE constant (ALT_EMIT_AVG_MS) and is user-facing only via a raw
// CSV. To remove: this block, the /altwinstats.csv route in 3_functions.ino, and the altWinProbeFeed
// call in altFold_tick. Costs ~4.8 KB PSRAM and one ring push per 10 Hz probe tick; nothing else
// reads it, so deleting it cannot affect grading, records, or the trend.
//
// Measures, during FULL-steady runs, how far a trailing W-second average of the graded amps signal
// rides above the run's overall mean — exactly the inflation a max-per-cell record book harvests at
// that window width. One steady run = one committed sample per window it spans ≥2×. Serves
// /altwinstats.csv. Answer so far, from two independent sessions (idle hour 2026-08-21, driveway idle
// 2026-08-21 pm): inflation is 0.12-0.31% at EVERY width from 1 s to 60 s — immaterial against the
// 2.5% amps band — while harvest collapses at the long end (2 s caught 47 of 149 runs, 8 s caught 3).
// ALT_EMIT_AVG_MS is hard-set to 2000 on that basis. Kept installed only because the user is still
// running it; it has no remaining question to answer.
#define AWP_NWIN 7
static const float AWP_WIN_S[AWP_NWIN] = { 1, 2, 4, 8, 15, 30, 60 };
#define AWP_RING_N 620                     // 62 s @ the 10 Hz probe cadence — covers the 60 s window
static float    *awpAmps = nullptr;        // PSRAM rings (allocated in altFrontInit)
static uint32_t *awpT = nullptr;
static int      awpHead = 0, awpN = 0;
static uint32_t awpLastFeedMs = 0, awpLastCalcMs = 0;
static bool     awpInRun = false;
static uint32_t awpRunStartMs = 0, awpRunLastMs = 0;
static double   awpRunSum = 0;
static uint32_t awpRunN = 0;
static float    awpRunMax[AWP_NWIN], awpRunMin[AWP_NWIN];
static bool     awpRunHave[AWP_NWIN];
static uint32_t awpCnt[AWP_NWIN];
static double   awpSumInfl[AWP_NWIN], awpSumInflPct[AWP_NWIN], awpSumRange[AWP_NWIN];
static float    awpMaxInfl[AWP_NWIN];
static uint32_t awpRunsTotal = 0;
static double   awpSteadySec = 0, awpAmpSum = 0;
static uint32_t awpAmpN = 0;
static void awpCloseRun() {
  if (!awpInRun) return;
  awpInRun = false;
  if (awpRunN < 5) return;
  float runMean = (float)(awpRunSum / awpRunN);
  float durS = (float)((uint32_t)(awpRunLastMs - awpRunStartMs)) / 1000.0f;
  awpRunsTotal++; awpSteadySec += durS;
  if (runMean < 1.0f) return;              // ratio meaningless at near-zero output
  for (int w = 0; w < AWP_NWIN; w++) {
    if (!awpRunHave[w] || durS < 2.0f * AWP_WIN_S[w]) continue;   // ≥2 window-lengths of run for a fair max
    float infl = awpRunMax[w] - runMean;
    awpCnt[w]++;
    awpSumInfl[w] += infl;
    awpSumInflPct[w] += 100.0f * infl / runMean;
    awpSumRange[w] += (awpRunMax[w] - awpRunMin[w]);
    if (infl > awpMaxInfl[w]) awpMaxInfl[w] = infl;
  }
}
static void awpReset() {
  awpHead = awpN = 0; awpInRun = false;
  memset(awpCnt, 0, sizeof(awpCnt));
  memset(awpMaxInfl, 0, sizeof(awpMaxInfl));
  for (int w = 0; w < AWP_NWIN; w++) awpSumInfl[w] = awpSumInflPct[w] = awpSumRange[w] = 0;
  awpRunsTotal = 0; awpSteadySec = 0; awpAmpSum = 0; awpAmpN = 0;
}
static void altWinProbeFeed(uint32_t nowMs, float amps, bool steady) {
  if (!awpAmps || !awpT) return;
  if (awpLastFeedMs != 0 && (uint32_t)(nowMs - awpLastFeedMs) < 100u) return;
  bool gap = (awpLastFeedMs != 0) && ((uint32_t)(nowMs - awpLastFeedMs) > 1000u);
  awpLastFeedMs = nowMs;
  if (gap || !steady) {
    awpCloseRun(); awpN = 0; awpHead = 0;  // ring holds in-run data only; a gap can't bridge runs
    if (!steady) return;
  }
  if (!awpInRun) {
    awpInRun = true; awpRunStartMs = nowMs; awpRunSum = 0; awpRunN = 0;
    for (int w = 0; w < AWP_NWIN; w++) { awpRunHave[w] = false; awpRunMax[w] = -1e30f; awpRunMin[w] = 1e30f; }
  }
  awpRunLastMs = nowMs;
  awpAmps[awpHead] = amps; awpT[awpHead] = nowMs;
  awpHead = (awpHead + 1) % AWP_RING_N;
  if (awpN < AWP_RING_N) awpN++;
  awpRunSum += amps; awpRunN++;
  awpAmpSum += amps; awpAmpN++;
  if ((uint32_t)(nowMs - awpLastCalcMs) < 1000u) return;   // trailing averages sampled 1/s, like emits
  awpLastCalcMs = nowMs;
  double sum = 0; int cnt = 0, wi = 0;
  for (int back = 0; back < awpN && wi < AWP_NWIN; back++) {
    int idx = (awpHead - 1 - back + AWP_RING_N) % AWP_RING_N;
    uint32_t age = (uint32_t)(nowMs - awpT[idx]);
    while (wi < AWP_NWIN && age > (uint32_t)(AWP_WIN_S[wi] * 1000.0f)) {
      // window wi is complete (this sample is already older than it) → record its trailing average,
      // but only once the run itself has spanned the window (no partial-window impostors)
      if (cnt > 0 && (uint32_t)(nowMs - awpRunStartMs) >= (uint32_t)(AWP_WIN_S[wi] * 1000.0f)) {
        float avg = (float)(sum / cnt);
        if (avg > awpRunMax[wi]) awpRunMax[wi] = avg;
        if (avg < awpRunMin[wi]) awpRunMin[wi] = avg;
        awpRunHave[wi] = true;
      }
      wi++;
    }
    if (wi >= AWP_NWIN) break;
    sum += awpAmps[idx]; cnt++;
  }
  for (; wi < AWP_NWIN; wi++) {            // ring exhausted before these windows aged out
    if (cnt > 0 && (uint32_t)(nowMs - awpRunStartMs) >= (uint32_t)(AWP_WIN_S[wi] * 1000.0f)) {
      float avg = (float)(sum / cnt);
      if (avg > awpRunMax[wi]) awpRunMax[wi] = avg;
      if (avg < awpRunMin[wi]) awpRunMin[wi] = avg;
      awpRunHave[wi] = true;
    }
  }
}
// /altwinstats.csv — one row per candidate window width. avgInflA/avgInflPct = how far that window's
// best trailing average sat above its run's mean (averaged over runs); maxInflA = worst single run;
// avgRangeA = mean max-min spread of the window averages within a run. ?reset=1 clears.
void altWinStatsCsvSend(AsyncWebServerRequest *request) {
  if (request->hasParam("reset")) { awpReset(); request->send(200, "text/plain", "reset\n"); return; }
  String out;
  out.reserve(900);
  if (!awpAmps || !awpT) { request->send(200, "text/plain", "probe unavailable (alloc failed)\n"); return; }
  out += "# steadyRuns=" + String(awpRunsTotal) + " steadySec=" + String(awpSteadySec, 0);
  out += " meanAmps=" + String((awpAmpN > 0) ? (float)(awpAmpSum / awpAmpN) : 0.0f, 1);
  out += " prodWinMs=" + String((unsigned)ALT_EMIT_AVG_MS) + "\n";
  out += "win_s,runs,avgInflA,avgInflPct,maxInflA,avgRangeA\n";
  for (int w = 0; w < AWP_NWIN; w++) {
    out += String(AWP_WIN_S[w], 0); out += ',';
    out += String(awpCnt[w]);
    if (awpCnt[w] > 0) {
      out += ',';
      out += String((float)(awpSumInfl[w] / awpCnt[w]), 2); out += ',';
      out += String((float)(awpSumInflPct[w] / awpCnt[w]), 2); out += ',';
      out += String(awpMaxInfl[w], 2); out += ',';
      out += String((float)(awpSumRange[w] / awpCnt[w]), 2);
    } else out += ",0,0,0,0";
    out += '\n';
  }
  request->send(200, "text/csv", out);
}

// ══ GATE-TUNING CAPTURE ═══════════════════════════════════════════════════════════════════════
// Manual session logger (this block) + field sweeper (altSweep_tick, next to fieldCurve_tick).
// Spec: Working Markdown Docs/ALT_GATE_TUNING_CAPTURE_SPEC.md. Replaces the gate-replay ring
// (/altraw.csv + /altemits.csv), removed 2026-08-23.
//
// WHY IT EXISTS: the record book's admission gate keeps only runs that beat the local surface
// estimate, so any upward bias in that surface reads as a healthy machine grading below 100%. Sizing
// the gates needs per-axis output sensitivities, and the closed-loop operating data cannot give them:
// the regulator holds a target by trading field against engine speed, so those two never move
// independently and a fit on ordinary running reports "field barely matters" when the truth is "the
// regulator was cancelling it". This logs a hand-run experiment where they DO move independently.
//
// 10 Hz, and 10 Hz is not a compromise: EP_FEED_DT_MS = 100, so the Episode detector already
// decimates the ~200 Hz fold to 10 Hz before any steadiness decision — an offline replay is
// sample-for-sample aligned with what the live gate would have done. It also clears the fixed
// 1.5006 Hz bus disturbance (5 Hz Nyquist), which a 2 Hz log would fold straight into the band the
// smear measurement lives in.
#define ALTLOG_DT_MS       100u    // 10 Hz — the cadence the gate itself evaluates at (EP_FEED_DT_MS)
#define ALTLOG_BLOCKS      3
#define ALTLOG_BLOCK_ROWS  3855    // rows per 64 KB block at 17 B/row (65,535 B — the 1 B remainder
                                   // is left unused rather than splitting a row across two blocks)
#define ALTLOG_CAP_ROWS    ((uint32_t)ALTLOG_BLOCKS * ALTLOG_BLOCK_ROWS)   // 11565 rows = 19.3 min
#define ALTLOG_STALE_MS    300u    // fold older than this → field is off / control path not running

// 17 B/row. Every axis is stored 20-100x finer than the band it gates, so quantization can never be
// mistaken for movement. Saturating stores (altQI16/altQU16): amps clip at ±327.67 A (a 500 A
// install running above that logs a flat line, which is visible; it does not wrap), bus volts at
// 65.535 V (above any 48 V-class cut), temp at ±3276.7 °F. A NaN case temp stores 0 — implausible on
// a running alternator, so the analysis treats 0 °F as "no reading" rather than as data.
struct __attribute__((packed)) AltLogRow {
  uint16_t dtMs;      // ms since the previous row (uint16 caps an in-row gap at 65 s; longer ends the segment)
  int16_t  rpm;
  uint16_t duty;      // field % ×100
  uint16_t vbus;      // mV
  int16_t  tempF;     // °F ×10 — RAW: the health tool never filters temp (slow, quantized, noise-free)
  int16_t  tSlope;    // case-temp rate of change, °F/min ×100 — the device's own altTempSlopeFMin
                      // (fast-minus-slow EMA pair in the fold), NOT recomputed offline: it needs
                      // filter history from before Record was pressed, and it is the same number the
                      // record book stamps on every point (ex[1]), so a replay must see the same one.
                      // Full resolution on purpose — near-zero versus a couple of °F/min is the whole
                      // settled-vs-warming discriminator, and warm-up sessions grade ~10% low
  int16_t  amps;      // A ×100, EMA-filtered (altEmaSec) — the signal the detector acts on
  int16_t  ampsRaw;   // A ×100, MEAN of the fresh CH1 conversions since the previous row (see below)
  uint8_t  flags;     // b0 field on, b1 manual mode, b2 sweeper active, b3 sweep direction (1=down),
                      // b4 protection binding, b5 detector eligible, b6 detector full-steady,
                      // b7 this sweep reversed early — a control limit or Reverse now, not its ceiling
  // No segment index. A recording is deliberately FLUID — one press of Record, then whatever the
  // operator does: a sweep, throttle movements, ordinary driving, in any order. Nothing on the device
  // knows where one "run" ends and the next begins, and nothing needs to: these flag bits already mark
  // every transition the analysis can split on (sweeper on/off, up leg vs down, manual vs auto, field
  // on vs off), and dtMs marks any real gap. A counter on top of that was a second, weaker copy.
};
static_assert(sizeof(AltLogRow) == 17, "AltLogRow lost its packing — the 192 KB budget assumes 17 B");

// 192 KB as 3 × 64 KB blocks, NOT one allocation. Free PSRAM measured 848 KB (2026-08-23) and a
// single contiguous request that size is not safe at that level: cvLog's 334 KB is one
// heap_caps_malloc and it silently lost its boot allocation once already, serving empty files for
// days, at a HIGHER free figure than this. Chunking removes the contiguity question entirely.
// Allocated on Record, freed on dump-and-clear — steady-state cost is zero.
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
static bool     altEligibleNow = false;   // detector eligibility, exported from altFold_tick for flags b5
// Every button press is a REQUEST consumed on the control loop, never an action taken on the web
// task: allocation, freeing and the state machine all run on the one task that also writes rows, so
// a Discard can never free the buffer out from under a row write. Same reason the field curve is
// request-driven. The dump lambda reads the blocks from the web task, so a release also waits for
// any transfer in flight (with a staleness escape, since an abandoned connection may never
// disconnect cleanly).
// (the request flags themselves live in Xregulator.ino — the /get handlers are compiled before this file)
static volatile bool altLogDlBusy = false;
static volatile uint32_t altLogDlStartMs = 0;   // refreshed on every chunk the TCP stack pulls, so the
                                                // 30 s escape fires only when the transfer truly stalls —
                                                // a slow-but-live link must keep holding Discard back
static inline bool altLogDlActive() {
  return altLogDlBusy && ((uint32_t)(millis() - altLogDlStartMs) < 30000u);
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
  queueConsoleMessageF("Gate capture: %s at %lu rows (%.0f s) — Dump to keep it",
                       (reason == 2) ? "BUFFER FULL, recording stopped" : "stopped",
                       (unsigned long)altLogRows, secs);
}

static bool altLogStartRec() {
  if (altLogState == 1) { queueConsoleMessage("Gate capture: already recording"); return false; }
  if (IgnoreTemperature) {   // temp is one of the four axes AND the gate that disables the whole fold
    queueConsoleMessage("Gate capture: blocked — Ignore Temperature is on, so there is no case-temp axis to log");
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

// Called from loop() every pass. Accumulates raw CH1 amps continuously and writes one row per
// ALTLOG_DT_MS. Idle cost is a single compare.
void altLog_tick(uint32_t nowMs) {
  altLogService();
  if (altLogState != 1) return;

  // ampsRaw MUST be a mean, not a point sample. CH1 delivers fresh amps roughly every 30 ms, so at
  // 10 Hz a point sample would take one conversion in three and alias current ripple into the one
  // column intended for re-deriving the EMA offline — which defeats its purpose. ch1AtCount ticks
  // once per confirmed CH1 read, so this edge-detects every fresh conversion and averages them.
  // The other channels update slower than 10 Hz, so point samples are correct for them.
  if (ch1AtCount != altLogCh1Seen) {
    altLogCh1Seen = ch1AtCount;
    if (!isnan(MeasuredAmps)) { altLogRawSum += MeasuredAmps; altLogRawN++; }
  }

  if ((uint32_t)(nowMs - altLogLastRowMs) < ALTLOG_DT_MS) return;

  // Auto-stop: the next write would exceed capacity. Stop, keep every row, say so loudly.
  if (altLogRows >= ALTLOG_CAP_ROWS) { altLogStopRec(2); return; }

  uint32_t dt = nowMs - altLogLastRowMs;
  altLogLastRowMs = nowMs;

  // The fold exports the EMA-filtered detector inputs at ~200 Hz but only on a normal field-on
  // control tick. A stale fold means the field is off (or a fault path is running): log the raw
  // live values with the field-on bit clear, so an off stretch reads as an off stretch instead of
  // as a hole in the record.
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
                                   fl |= 0x80;   // this sweep turned around early — a control limit or the
                                                 // Reverse now button, not its commanded ceiling. Gated on the
                                                 // sweep being live so the bit describes a run, not a leftover.


  AltLogRow *r = ALTLOG_AT(altLogRows);
  r->dtMs    = (dt > 65535u) ? 65535u : (uint16_t)dt;
  r->rpm     = altQI16(rpm, 1.0f);
  r->duty    = altQU16(duty, 100.0f);
  r->vbus    = altQU16(vbus, 1000.0f);
  r->tempF   = altQI16(TempToUse, 10.0f);
  r->tSlope  = altQI16(altTempSlopeFMin, 100.0f);
  r->amps    = altQI16(amps, 100.0f);
  r->ampsRaw = altQI16(rawAmps, 100.0f);
  r->flags   = fl;
  altLogRows++;

  altLogSecLeft = (float)(ALTLOG_CAP_ROWS - altLogRows) / 10.0f;
  if (!altLogWarned60 && altLogSecLeft <= 60.0f) {
    altLogWarned60 = true;
    queueConsoleMessage("Gate capture: 60 s of buffer left — wrap up the sweep in progress");
  }
}

// ---- dump header (128+ B, little-endian, mirrored by parseAltLogBin() in script.js) ----
// Dump-and-restart produces many small files instead of one large one, so each file has to carry the
// settings that shaped it: a mid-campaign gate change would otherwise make two files silently
// incomparable. Session id + start epoch keep them in order.
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
  uint8_t  pad0[3];      // keeps the floats below 4-byte aligned (one byte was the retired segment index)
  float    startTempF, startVbus;
  float    emaSec, rpmTol, dutyTolPct, vbusTol, thermDegF, thermSec, ampsTolPct, ampsFloorA;
  float    rpmSec, dutySec, vbusSec, ampsSec, minRunSec;
  uint32_t emitAvgMs;
  float    axisScale[4];
  char     fw[16];
};
static_assert(sizeof(AltLogHdr) == 136, "AltLogHdr size changed — parseAltLogBin() in script.js must match");

// /altlog.bin — header then rows, streamed. Refused while recording: a growing row count under a
// chunked reader would hand out a file whose header disagrees with its body.
void altLogBinSend(AsyncWebServerRequest *request) {
  // Claim the transfer interlock BEFORE reading any of the state it protects: altLogService only
  // holds a Discard back while altLogDlActive(), so a guard that ran first could pass on a live
  // buffer and then have the control loop free it under the streaming lambda. Every early return
  // below releases it by hand — onDisconnect is not registered yet at that point.
  altLogDlStartMs = millis();   // before the flag: altLogDlActive() reads both, and only when busy
  altLogDlBusy = true;
  if (altLogState == 1) {
    altLogDlBusy = false;
    request->send(409, "text/plain", "recording — press Stop first\n");
    return;
  }
  if (altLogState == 0 || altLogRows == 0 || !altLogBlk[0]) {
    altLogDlBusy = false;
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

  request->onDisconnect([]() { altLogDlBusy = false; });   // Safari tears connections down abruptly

  AltLogHdr h;
  memset(&h, 0, sizeof(h));
  h.magic = 0x414C474Cu;   // 'ALGL'
  h.ver = 1;
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
  h.emaSec = altEmaSec;       h.rpmTol = altRpmTol;       h.dutyTolPct = altDutyTolPct;
  h.vbusTol = altVbusTol;     h.thermDegF = altThermDegF; h.thermSec = altThermSec;
  h.ampsTolPct = altAmpsTolPct; h.ampsFloorA = altAmpsFloorA;
  h.rpmSec = altRpmSec;       h.dutySec = altDutySec;     h.vbusSec = altVbusSec;
  h.ampsSec = altAmpsSec;     h.minRunSec = altMinRunSec;
  h.emitAvgMs = (uint32_t)ALT_EMIT_AVG_MS;
  for (int a = 0; a < ALT_NAXIS; a++) h.axisScale[a] = altFront2.axisScale[a];
  strncpy(h.fw, FIRMWARE_VERSION, sizeof(h.fw) - 1);
  memcpy(st.hdr, &h, sizeof(h));

  AsyncWebServerResponse *response = request->beginChunkedResponse(
    "application/octet-stream",
    [st](uint8_t *buf, size_t maxLen, size_t) mutable -> size_t {
      if (st.done) return 0;
      altLogDlStartMs = millis();   // progress = alive; see altLogDlActive()
      // If the 30 s escape did lapse during a stall and a Clear freed the blocks, end the stream
      // instead of reading a null block (all three are freed together).
      if (!altLogBlk[0]) { st.done = true; altLogDlBusy = false; return 0; }
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
          if (st.row >= st.total) { st.done = true; altLogDlBusy = false; return written; }
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

// Live: called from the pidLog hook (~200 Hz). Bench-sim: called at 1 Hz from altHealth_tick.
// Reads the final control state, EMA-filters the detector inputs, feeds the Episode detector, and on a
// steady-run emit builds the surface point and STASHES it into altEmitQ — the O(count) work (front
// gate, push-if-best, trend feed) all happens at 1 Hz in altProcessEmits/altHealth_tick, never in this
// ~200 Hz tick. The off/fault/shutdown paths early-return before the live pidLog hook, so field-off
// cases exclude themselves with no mode check.
void altFold_tick(uint32_t nowMs) {
  if (!altFrontBuf || !altEpRing) return;

  // IgnoreTemperature → the ENTIRE alt-health system is disabled: no live, no points, no trend.
  if (IgnoreTemperature) { altLiveValid = false; altSteady = false; altSessSteady = false; altStatusCode = 3; return; }
  if (altStatusCode == 3) altStatusCode = 0;   // temp re-enabled → clear the "disabled" status

  float rpm, tF, vbus, amps, duty;
  if (altSimMode >= 0.5f) {
    rpm = altSimRpm; tF = altSimT; vbus = altSimV; amps = altSimAmps; duty = altSimDuty;
  } else {
    vbus = getBatteryVoltage();
    amps = isnan(MeasuredAmps) ? 0.0f : MeasuredAmps;
    tF = TempToUse; duty = dutyCycle; rpm = RPM;
  }

  // Detector-input EMA (altEmaSec): strips inner-loop duty dither, RPM jitter, and current ripple
  // so the steadiness bands can be sized purely for real operating-point movement (= transient
  // rejection). Temp (DS18B20) is DELIBERATELY raw — slow, quantized, noise-free (user, 2026-08-20),
  // so filtering it adds only lag. The control loops never see these. A long gap between folds
  // (field off, boot) reseeds the filters from raw so stale state can't bridge it.
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

  // Export the filtered live point for the 1 Hz evaluator (altHealth_tick); the fold itself keeps
  // only the EMA filters + the episode detector feed.
  altLive_rpm = fRpm; altLive_exc = exc; altLive_amps = fAmps;
  altLive_vbus = fVbus; altLive_tF = tF; altLive_duty = fDuty;

  // Case-temp slope (F/min), fast-minus-slow EMA pair: for a ramp input emaFast-emaSlow settles at
  // rate*(tauSlow-tauFast). Thermal-transient tag for the 2026-08-21 lag finding (case sensor lags
  // windings; warm-up sessions grade ~10% low, and phase offsets at like tags track thermal state):
  // rides the AltLive SSE (session dots latch it) and every emitted record's ex[1].
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
    altSessTempGate.clear(); altEpisode.clearRun();
    return;
  }

  // Feed the Episode detector (steadiness/averaging axes {RPM, duty %, Vbus, tempF} + the
  // output-amps band) — all filtered, including the admission floors, so a single noise dip
  // can't act as a barrier that wipes the look-back ring mid-run.
  altEpisodeSyncCfg(fAmps);
  bool eligible = (!isnan(fVbus) && fVbus >= ALT_MIN_BATT_V * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f) && fAmps >= altMinAmps && fDuty >= altMinDuty && fRpm >= 0);
  altEligibleNow = eligible;                                                    // exported for the gate-capture log's flags byte
  altSessTempGate.feed(eligible, tF, nowMs, altThermDegF, altSessTempDwell());   // lighter (half-dwell) temp gate for the session plot
  RawSample<ALT_NAXIS> s;
  s.x[0] = fRpm; s.x[1] = fDuty; s.x[2] = fVbus; s.x[3] = tF; s.out = fAmps; s.tMs = nowMs;
  s.ex[0] = fDuty; s.ex[1] = altTempSlopeFMin;   // retain run duty + case-temp slope (F/min) for diagnosis;
                                                 // the emit averages both, so ex[1] = the run's mean thermal transient
  FrontPoint<ALT_NAXIS> ep;
  bool emitted = altEpisode.feed(eligible, s, &ep);
  altSteady = (altEpisode.count > 0);                              // FULL steady (temp held altThermSec) → ring + surface + trend
  altWinProbeFeed(nowMs, fAmps, altSteady);                        // emit-window sizing probe (10 Hz internal decimation)
  // SESSION steady = all axes EXCEPT temperature steady on their ~3 s windows (Episode per-axis flags for
  // {RPM,duty,Vbus} + the amps band) AND temperature steady for the HALF dwell (altSessTempGate). Lets a
  // Session-plot dot appear before the full record-book dwell is reached.
  altSessSteady = eligible
                  && altEpisode.axisSteady[0] && altEpisode.axisSteady[1] && altEpisode.axisSteady[2]
                  && altEpisode.axisSteady[ALT_NAXIS] && altSessTempGate.steady;
  if (!emitted) return;

  // Full steady run completed → build the surface point (excitation derived from run averages) and STASH
  // it UNCONDITIONALLY (even when paused) so the trend still sees it; altProcessEmits grades it against the
  // active surface (→ trend) and admits it into My History only when learning is active. Solves run there
  // at 1 Hz, never in this ~200 Hz control tick.
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

// Drain the emit queue (1 Hz, from altHealth_tick; same task as the fold — no lock). At most 2
// emits per tick so a single loop() pass never blocks more than a couple ms; emits arrive at least
// one steady-run apart, so the queue drains faster than it fills.
static void altProcessEmits() {
  int n = (altEmitQCount < 2) ? altEmitQCount : 2;
  bool learn = (altPaused < 0.5f);   // learning active → also admit into My History; else trend-only
  for (int k = 0; k < n; k++) {
    FrontPoint<ALT_NAXIS> &sp = altEmitQ[k].sp;

    // (1) TREND FEED — every FULL-steady run is graded against the ACTIVE surface (My History or Uploaded)
    // and fed to the engine-hours trend, throttled to one sample per altTrendFeedSec. This is the trend's
    // ONLY source: the spec wants the trend built purely from
    // full steady runs. MEASURED runs only (session plot: green dot + orange ring) — Estimated grades
    // lean on interpolation and would blur the trend; No-reference/risky-fit runs feed nothing.
    {
      static uint32_t lastTrendFeedMs = 0;
      float pred = 0;
      int st = altGradeFront().classify(sp.x, altRefRadius, altIdwPower, altRidgeFrac, altRiskThresh, &pred);
      bool graded = (st == FRONT_MEASURED) && pred > 0.1f;
      uint32_t nowMs = millis();
      if (graded && (lastTrendFeedMs == 0 || (uint32_t)(nowMs - lastTrendFeedMs) >= (uint32_t)(altTrendFeedSec * 1000.0f))) {
        lastTrendFeedMs = nowMs;
        altTrendAdd((sp.y / pred));   // output-% as a fraction; altTrendAdd scales ×100 + buckets it
      }
    }

    // (2) SURFACE ADMISSION — into My History only, and only while learning is active. The Uploaded surface
    // is never modified by learning. Admission grades against My History (the learn target), not the active
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
    // No console message on accept — the dashboard front-point counters show growth; logging every
    // accept spammed the console with same-cell improvements at steady state.
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
static float alf_valid()     { return (float)altLiveValid; }
static float alf_rpm()       { return altLive_rpm; }
static float alf_exc()       { return altLive_exc; }
static float alf_amps()      { return altLive_amps; }
static float alf_pred()      { return altLive_pred; }
static float alf_pct()       { return altLive_pct; }
static float alf_worst()     { return altWorstPctLive; }
static float alf_overall()   { return altOverallPctLive; }
static float alf_status()    { return (float)altStatusCode; }
static float alf_steady()    { return (float)altSteady; }                        // FULL steady run (temp held STEADY_TEMP_SEC) → orange ring
static float alf_sessSteady(){ return (float)altSessSteady; }                    // SESSION steady (temp held SESSION_TEMP_SEC) → session-plot dot gate
static float alf_engHours()  { return altEngineHoursSinceBaseline(); }
static float alf_coverage()  { return altFrontBuf ? (100.0f * (float)altGradeFront().count / (float)ALT_FRONT_CAP) : 0.0f; }
static float alf_haveCurve() { return (float)(altGradeFront().count > 0 ? 1 : 0); }   // active surface has a usable front
static float alf_ptCount()   { return (float)altGradeFront().count; }           // active-surface support points
static float alf_source()    { return (float)altRefSource; }                    // 0 = My History, 1 = Uploaded File (active grading surface)
static float alf_haveUpload(){ return altHaveUpload ? 1.0f : 0.0f; }            // an Uploaded surface is resident
static float alf_paused()    { return (altPaused >= 0.5f) ? 1.0f : 0.0f; }
static float alf_refOk()     { return altRefOk ? 1.0f : 0.0f; }          // state is MEASURED/ESTIMATED → % shown
static float alf_refDist()   { return altRefDist; }                       // normalized distance to nearest support
static float alf_state()     { return (float)altState; }                  // 0 MEASURED, 1 ESTIMATED, 2 LEARNING_EDGE, 3 NO_REFERENCE
static float alf_sessMean()  { return (altSessN > 0) ? (float)(altSessSum / (double)altSessN) : 0.0f; }
static float alf_sessP10()   {                                            // P10 of the session histogram (≥10 samples)
  if (altSessN < 10) return 0.0f;
  uint32_t target = (altSessN + 9) / 10, cum = 0;
  for (int i = 0; i < 60; i++) { cum += altSessHist[i]; if (cum >= target) return (float)(i * 2 + 1); }
  return 0.0f;
}
static float alf_sessN()     { return (float)altSessN; }
static float alf_hiField()   { return altHiFieldAlert ? 1.0f : 0.0f; }    // high-field-low-output alert active
static float alf_sim()       { return (altSimMode >= 0.5f) ? 1.0f : 0.0f; }
static float alf_gAmps()     { return altLive_gAmps; }                    // graded boxcar-average amps (the % numerator)
static float alf_tSlope()    { return altTempSlopeFMin; }                 // case-temp slope (F/min) — thermal-transient tag
// Gate-tuning capture state. It rides AltLive rather than a CSV channel: AltLive is the alt-health
// module's own 1 Hz frame, the recorder UI lives on the same page, and the frame is schema-zipped
// (/altschema) so appending a field cannot desynchronize a stale-open dashboard the way a CSV
// index map can. A 1 Hz countdown is also all a seconds-remaining readout needs.
static float alf_logState()  { return (float)altLogState; }               // 0 idle, 1 recording, 2 stopped, 3 full
static float alf_logRows()   { return (float)altLogRows; }
static float alf_logSecLeft(){ return altLogSecLeft; }                    // capacity remaining at the fixed 10 Hz rate
static float alf_logCap()    { return (float)ALTLOG_CAP_ROWS; }
static float alf_sweepState(){ return (float)altSweepActive; }            // 0 idle, 1 ramp up, 2 ramp down, 3 easing out
static float alf_sweepRev()  { return (float)altSweepRevWhy; }            // 0 ceiling, 1 a control limit, 2 Reverse now
static float alf_syncAgo()   { if (lastAltHealthSyncEpoch <= 0 || !timeIsSynced) return -1.0f;
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
};
static const size_t ALT_LIVE_COUNT = sizeof(ALT_LIVE) / sizeof(ALT_LIVE[0]);
static void altSendLive() {
  char buf[512];   // truncates rather than overruns (see the break below); sized well past the field list
  int off = 0;
  for (size_t i = 0; i < ALT_LIVE_COUNT; i++) {
    int n = snprintf(buf + off, sizeof(buf) - off, (i ? ",%.3f" : "%.3f"), ALT_LIVE[i].get());
    if (n < 0 || off + n >= (int)sizeof(buf)) break;   // truncated echo beats stack overrun (snprintf returns would-be length)
    off += n;
  }
  events.send(buf, "AltLive");
}

// ---- coverage / status helpers (CSV2 + dashboard) ----
float altCoveragePct() { return altFrontBuf ? (100.0f * (float)altFront2.count / (float)ALT_FRONT_CAP) : 0.0f; }
float altWorstPct()    { return altWorstPctLive; }
int   altStatus()      { return (int)altStatusCode; }
int   altHaveFront()   { return (altFront2.count > 0) ? 1 : 0; }   // CSV2: have a usable best-ever front
int   altFrontCount()  { return altFront2.count; }                 // CSV2: front support-point count
void  altClearPending() { altPendingCount = 0; altPendingSeededFrom = ""; }   // cloud accepted the batch → drop pending + tag

// ---- front CSV (the artifact): BEFRONT1,<sys>,<naxis>,<source>,<units…> then x0..xN,y,nSamp,tEmit ----
// Serves /altcurve.csv (dashboard), the cloud sync-back, and Save/Load to file (spec §2.2/§8).
// Streamed row-by-row (beginChunkedResponse, same idiom as /alttrend.csv) so a front grown to its
// 4096-point cap can no longer build a single ~150 KB std::String on the internal heap — that
// fragmented the heap TLS needs ~32–40 KB contiguous from, a genuine fill-driven failure mode.
// Only one row (line[]) is ever materialized; CONSTANT internal RAM at any count. total/source are
// snapshotted so a concurrent Core-1 front edit can't run past the snapshot — an in-place same-cell
// improvement just shows a fresher value for one row (harmless for a diagnostic CSV).
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
// Session series for the dashboard's session-% plot (/altsess.csv). Streamed oldest-first, constant
// RAM. The header carries the device's current millis so the client can map tMs onto its own wall
// clock. A push can land mid-stream and overwrite the oldest slot we have not reached yet, so tMs is
// self-describing and the client drops any row whose tMs goes backwards.
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
// Full alt-health state dump for offline / AI debugging (spec §4.5 "Download Debug CSV"). Streamed
// row-by-row (constant internal RAM at any front size). Self-describing "section,key,v0…" rows, in
// phases: PARAM (every registry knob), LIVE/GATE/BUCKET/SESSION scalars, then MYHIST + UPLOAD front
// points and the committed TREND buckets. Served at /altdebug.csv.
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
              case 9: {   // elapsed steadiness dwell (ms): session-temp gate + episode (full) data window
                uint32_t now = millis();
                uint32_t sessTempMs = altSessTempGate.have ? (now - altSessTempGate.startMs) : 0;
                uint32_t epDataMs   = altEpisode.haveData  ? (now - altEpisode.dataStartMs) : 0;
                st.len = snprintf(st.line, sizeof(st.line), "GATE,sessTempDwellMs_epDataMs,%u,%u\n", (unsigned)sessTempMs, (unsigned)epDataMs);
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

// Parse a BEFRONT1 CSV (the cloud's pruned front, or a saved/uploaded file) into a TARGET surface +
// its backing buffer, replacing it. toUploaded=false → My History (cloud sync-back); true → Uploaded
// surface (browser Load CSV). Uses a bool (not a FrontStore<> reference) so Arduino's auto-prototype
// generator — which can't parse template types in signatures — leaves it alone.
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
  int newCount = 0;
  char *line = nl + 1;
  while (line && *line && newCount < ALT_FRONT_CAP) {
    char *eol = strchr(line, '\n');
    if (eol) *eol = '\0';
    float x0, x1, x2, x3, y; unsigned ns = 0, te = 0;
    if (*line && sscanf(line, "%f,%f,%f,%f,%f,%u,%u", &x0, &x1, &x2, &x3, &y, &ns, &te) >= 5) {
      FrontPoint<ALT_NAXIS> q;
      q.x[0] = x0; q.x[1] = x1; q.x[2] = x2; q.x[3] = x3; q.y = y;
      q.ex[0] = 0; q.ex[1] = 0;   // raw extras live in the cloud table, not the front CSV
      q.nSamp = (uint32_t)ns; q.tEmit = (uint32_t)te;
      buf[newCount++] = q;
    }
    if (!eol) break;
    line = eol + 1;
  }
  if (line && *line && newCount >= ALT_FRONT_CAP)   // source sent more points than we can hold
    queueConsoleMessageF("WARN: alt front truncated at %d pts — raise ALT_FRONT_CAP", ALT_FRONT_CAP);
  fs.count = newCount;
  fs.source = newSource;
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
  if (!altFrontBuf || hardwarePresent != 1) return;
  uint32_t uw = ((uint32_t)altFront2.source << 8) | (uint32_t)ALT_NAXIS;   // stash source + naxis
  writePsramBlob("/altfront.bin", ALT_FRONT_MAGIC, ALT_VER, uw, altFrontBuf, sizeof(FrontPoint<ALT_NAXIS>), ALT_FRONT_CAP, 0, altFront2.count);
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

// Ingest a BEFRONT1 front UPLOADED from the browser (Load CSV) into the UPLOADED surface — resident
// ALONGSIDE My History, never overwriting it (spec §4). Switches the active Reference Source to Uploaded
// and defaults Pause per the user's choice: fixed=true → Pause ON (just grade against the borrowed curve);
// fixed=false → keep learning My History while graded against Uploaded. My History (and its cloud upload
// pending) is untouched. Persists immediately so the upload survives reboot. Mutates the body buffer.
bool altUploadFrontCsv(char *body, bool fixed) {
  if (!altFrontUpBuf || !body) return false;
  bool ok = altIngestFrontCsvInto(body, true);
  if (!ok) return false;
  altFrontUp.source = 1;                        // borrowed surface
  altHaveUpload = true;
  altRefSource = 1;                             // grade session + trend against the uploaded surface
  altPaused = fixed ? 1.0f : 0.0f;              // Uploaded defaults Pause ON; user may keep learning My History
  settingWrite(NK_altPaused, fixed ? "1.0000" : "0.0000");
  settingWrite(NK_altRefSrc, "1");
  queueConsoleMessageF("AltFront: UPLOADED %d pts to Uploaded surface (%s); My History kept",
                       altFrontUp.count, fixed ? "Pause ON" : "still learning My History");
  // Immediate persist even field-on: user-initiated import, the brief flash-write control
  // stall is accepted for user actions (owner decision 2026-08-14).
  altHealthSave();
  return true;
}

// ---- cloud upload: batch of accepted episode points since the last upload (raw out; pruned front in) ----
// Schema: {device_uid,token,ts,sys,pruneK,idwPower, pts:[[rpm,exc,vbus,tF,amps,nSamp], ...]}.
// Pending cleared on a successful response (executeUploadAltHealth → altIngestFrontCsv).
bool buildAltHealthPayload(char *buf, size_t size) {
  if (!altPending || altPendingCount == 0 || authToken.isEmpty()) return false;
  time_t now_ts = time(NULL);
  int off = snprintf(buf, size,
    "{\"device_uid\":\"%s\",\"token\":\"%s\",\"ts\":\"%s\",\"sys\":\"ALT\",\"pruneK\":%d,\"idwPower\":%.2f,\"sysV\":%u,",
    device_id_hex, authToken.c_str(), formatTimestamp(now_ts), (int)altPruneK, altIdwPower, (unsigned)SYSTEM_VOLTAGE_CLASS);
  if (off < 0 || (size_t)off >= size) return false;
  if (altPendingSeededFrom.length()) {   // adopted import: tag the whole batch as borrowed provenance
    off += snprintf(buf + off, size - off, "\"seededFrom\":\"%s\",", altPendingSeededFrom.c_str());
    if (off < 0 || (size_t)off >= size) return false;
  }
  off += snprintf(buf + off, size - off, "\"pts\":[");
  bool first = true;
  for (int k = 0; k < altPendingCount; k++) {
    if (size - (size_t)off < 80) break;   // margin for the closer
    FrontPoint<ALT_NAXIS> &p = altPending[k];
    off += snprintf(buf + off, size - off, "%s[%.0f,%.3f,%.2f,%.1f,%.2f,%u,%.1f]",
                    first ? "" : ",", p.x[0], p.x[1], p.x[2], p.x[3], p.y, p.nSamp, p.ex[0]);  // ex[0]=raw duty
    first = false;
  }
  off += snprintf(buf + off, size - off, "]}");
  if (off < 0 || (size_t)off >= size - 1) return false;
  return true;
}

// ---- lifecycle ----
void initAlternatorHealth() {
  altTrend = (AltTrendPt *)ps_malloc((size_t)ALT_TREND_CAP * sizeof(AltTrendPt));
  if (!altTrend) { queueConsoleMessage("ERROR: AltHealth trend ps_malloc failed"); return; }
  memset(altTrend, 0, (size_t)ALT_TREND_CAP * sizeof(AltTrendPt));
  altTrendCount = 0; altCurEngHour = -1;
  altFrontInit();          // ps_malloc front + episode ring + pending; init the engine instance
  altLoad();               // restore front + trend + baseline (after the buffers exist)
  queueConsoleMessageF("AltHealth init: My History %d pts, Uploaded %d pts, source=%s, %d trend pts",
                       altFront2.count, altFrontUp.count, altRefSource ? "Uploaded" : "MyHistory", altTrendCount);
}
void resetAlternatorHealth() {
  // The Start Over confirm says "after replacing the alternator, regulator, or belt" — exactly the
  // events that invalidate learned hunt pockets, so the damper map goes with the health record.
  // Called here, before this function takes the fs lock itself (huntMapClearAll takes it too).
  huntMapClearAll("alternator replaced / health record reset");
  if (!altFrontBuf) return;
  altFront2.count = 0; altFront2.source = 0;
  altPendingCount = 0; altFrontEmitCount = 0; altEmitQCount = 0; altCapWarned = false;
  altEpisode.clearRun(); altEpisode.ringHead = 0; altEpisode.ringCount = 0;
  altTrendCount = 0; altTrendFlushed = 0; altTrendRewrite = true;   // /alttrend.bin removed below → fresh log
  altBucket_sum = 0; altBucket_n = 0; altBucket_worst = 0; altCurEngHour = -1;
  memset(altBucketHist, 0, sizeof(altBucketHist));
  altPersistTrendBucket();                       // overwrite the persisted partial bucket with the cleared one
  altWorstPctLive = 0; altOverallPctLive = 0; altStatusCode = 0; altLive_pct = 0;
  altState = FRONT_NO_REFERENCE; altHiFieldAlert = false;
  altSteady = false; altSessSteady = false; altSessTempGate.clear();
  altRefSource = 0;                              // revert active reference to My History (Uploaded surface kept, just inactive)
  memset(altSessHist, 0, sizeof(altSessHist)); altSessN = 0; altSessSum = 0;   // session stats restart with the data
  altSessRingHead = 0; altSessRingCount = 0; altSessRingLastMs = 0;              // and the served session series
  altTrendBaselineSec = EngineRunTime_AllTime;   // new baseline → trend X-axis restarts at 0
  fsTakeLock();
  LittleFS.remove("/altfront.bin");
  LittleFS.remove("/alttrend.bin");
  fsReleaseLock();
  settingWrite(NK_altbaseSec, String(altTrendBaselineSec, 1).c_str());
  settingWrite(NK_altRefSrc, "0");
  queueConsoleMessage("AltHealth: full reset (Reset / Start Over) — My History + trend + session cleared, baseline restarted");
}

// ---- engine allocation (PSRAM): front points + episode reseed ring + pending-upload buffer ----
void altFrontInit() {
  altEpRing     = (RawSample<ALT_NAXIS>  *)ps_malloc((size_t)ALT_EP_RING_CAP * sizeof(RawSample<ALT_NAXIS>));
  altFrontBuf   = (FrontPoint<ALT_NAXIS> *)ps_malloc((size_t)ALT_FRONT_CAP   * sizeof(FrontPoint<ALT_NAXIS>));
  altFrontUpBuf = (FrontPoint<ALT_NAXIS> *)ps_malloc((size_t)ALT_FRONT_CAP   * sizeof(FrontPoint<ALT_NAXIS>));
  altPending  = (FrontPoint<ALT_NAXIS> *)ps_malloc((size_t)ALT_PENDING_CAP * sizeof(FrontPoint<ALT_NAXIS>));
  altSessRing = (AltSessPt *)ps_malloc((size_t)ALT_SESS_RING_N * sizeof(AltSessPt));   // session series (~32 KB)
  if (altSessRing) memset(altSessRing, 0, (size_t)ALT_SESS_RING_N * sizeof(AltSessPt));
  else queueConsoleMessage("AltHealth: session-series ps_malloc failed — /altsess.csv empty, plots stay browser-only");
  awpAmps = (float *)ps_malloc((size_t)AWP_RING_N * sizeof(float));         // TEMPORARY probe rings (~4.8 KB) — REMOVE AFTER AUGUST 2026
  awpT    = (uint32_t *)ps_malloc((size_t)AWP_RING_N * sizeof(uint32_t));
  if (!altEpRing || !altFrontBuf || !altFrontUpBuf || !altPending) { queueConsoleMessage("ERROR: AltFront ps_malloc failed"); return; }
  memset(altEpRing,     0, (size_t)ALT_EP_RING_CAP * sizeof(RawSample<ALT_NAXIS>));
  memset(altFrontBuf,   0, (size_t)ALT_FRONT_CAP   * sizeof(FrontPoint<ALT_NAXIS>));
  memset(altFrontUpBuf, 0, (size_t)ALT_FRONT_CAP   * sizeof(FrontPoint<ALT_NAXIS>));
  memset(altPending,  0, (size_t)ALT_PENDING_CAP * sizeof(FrontPoint<ALT_NAXIS>));
  altPendingCount = 0;
  // Per-axis deque window caps (max steady time the axis can be set to): RPM/duty/Vbus/amps ~30 s
  // headroom, temperature 240 s (covers the full temp dwell + room). Index 4 = the output (amps) band.
  static const float ALT_MAXDWELL[ALT_NAXIS + 1] = { 30.0f, 30.0f, 30.0f, 240.0f, 30.0f };
  altEpisode.init(altEpRing, ALT_EP_RING_CAP, ALT_MAXDWELL);
  altEpisode.avgWinMs = ALT_EMIT_AVG_MS;   // sail/motor keep the 2 s default; alt records are sustained averages
  altEpisodeSyncCfg(0.0f);   // amps band starts at the floor; resized from filtered amps every fold
  altSessTempGate.init(2600);   // session-temp (half-dwell) gate, ~240 s of 10 Hz headroom (matches temp axis maxdwell)
  altFront2.init(altFrontBuf, ALT_FRONT_CAP);
  altFrontUp.init(altFrontUpBuf, ALT_FRONT_CAP);
  // axisScale ≈ the span of each axis that moves output a comparable amount (rationale + rebalance
  // history: ALT_HEALTH_LWLR_ENGINE_SPEC.md). MUST match AXIS_SCALE in the update-alt-health edge fn
  // — the Vbus scale is class-aware there via the payload's sysV field (excitation needs no scaling:
  // it is physical field volts, and the class scaling of the field ceiling keeps its range identical
  // on any bank).
  altFront2.axisScale[0] = 25.0f;   // RPM
  altFront2.axisScale[1] = 0.10f;   // excitation (temp-normalized field volts). 2026-08-21 sensitivity study:
                                    // the old 0.2 cell spanned ~2.3% field ≈ ~5% output at measured g≈2 —
                                    // coarser than the 2.5% amps steadiness band — so records within one cell
                                    // differed by commanded field alone. Halved.
  altFront2.axisScale[2] = 0.1f;    // Vbus at 12V — class-corrected by altApplyClassScales() once SYSTEM_VOLTAGE_CLASS is loaded
  altFront2.axisScale[3] = 10.0f;   // tempF. Same study: ≤0.32%/F apparent slope → a 5F cell stays ≤1.6% output,
                                    // inside the amps band; opened from 5.0 for 2x faster coverage per temp span.
  for (int a = 0; a < ALT_NAXIS; a++) altFrontUp.axisScale[a] = altFront2.axisScale[a];   // same metric for the borrowed surface
  queueConsoleMessageF("AltFront init: cap %d pts, ring %d, %.1fKB PSRAM",
    ALT_FRONT_CAP, ALT_EP_RING_CAP,
    (float)((size_t)ALT_EP_RING_CAP * sizeof(RawSample<ALT_NAXIS>) +
            (size_t)(ALT_FRONT_CAP + ALT_PENDING_CAP) * sizeof(FrontPoint<ALT_NAXIS>)) / 1024.0f);
}

// Vbus cell size per-cell-equivalent across bank classes (0.4V @48V ≡ 0.1V @12V). Separate from
// altFrontInit because that runs BEFORE InitSystemSettings loads SYSTEM_VOLTAGE_CLASS; also re-run on a
// live class change (applyNominalVoltageChange). Scales are interpretation-only — stored points keep
// raw coordinates. The update-alt-health edge fn mirrors this via the upload payload's sysV field.
void altApplyClassScales() {
  altFront2.axisScale[2] = 0.1f * ((float)SYSTEM_VOLTAGE_CLASS / 12.0f);
  altFrontUp.axisScale[2] = altFront2.axisScale[2];
}

// One entry per ALT_SESS_DT_MS while the alternator is live, graded or not — an ungraded entry
// (state 2) is the dashboard's grey gap. Nothing is pushed while the field is down, so an overnight
// stop is a time jump in the series rather than thousands of gap rows evicting the real data.
// Cost: one 28-byte PSRAM store every 5 s inside the existing 1 Hz evaluator — never in the fold.
static void altSessSeriesPush(uint32_t nowMs, bool graded) {
  if (!altSessRing) return;
  if (altSessRingLastMs && (uint32_t)(nowMs - altSessRingLastMs) < ALT_SESS_DT_MS) return;
  altSessRingLastMs = nowMs;
  AltSessPt &p = altSessRing[altSessRingHead];
  p.tMs    = nowMs;
  p.flags  = graded ? (uint8_t)((altState == FRONT_ESTIMATED) ? 1 : 0) : 2;
  if (graded && altSteady) p.flags |= 0x04;
  p.gAmps  = altQU16(altLive_gAmps, 100.0f);
  p.measA  = altQU16(altLive_amps,  100.0f);
  p.pred   = altQU16(altLive_pred,  100.0f);
  p.rpm    = altQU16(altLive_rpm,     1.0f);
  p.duty   = altQU16(altLive_duty,  100.0f);
  p.vbus   = altQU16(altLive_vbus,  100.0f);
  p.tF     = altQI16(altLive_tF,     10.0f);
  p.tSlope = altQI16(altTempSlopeFMin, 100.0f);
  altSessRingHead = (altSessRingHead + 1) % ALT_SESS_RING_N;
  if (altSessRingCount < ALT_SESS_RING_N) altSessRingCount++;
}

// ---- 1 Hz tick (NOT the fold) — THE evaluator/classifier cadence, plus live telemetry + settings
//      echo. In bench-sim it also advances the simulator and folds at 1 Hz; live, the fold runs in
//      the ~200 Hz pidLog hook (altFold_tick is called from there). ----
void altHealth_tick(uint32_t nowMs) {
  static uint32_t lastMs = 0;
  if (!altFrontBuf) return;
  if (nowMs - lastMs < 1000) return;
  if (gHeavyRanThisPass) return;      // one-heavy-per-pass gate; defer (lastMs unchanged → still due next pass)
  gHeavyRanThisPass = true;
  lastMs = nowMs;
  if (altSimMode >= 0.5f) {           // bench simulator: advance synthetic point + fold at 1 Hz
    altSimTick(nowMs);
    altFold_tick(nowMs);
  }
  altProcessEmits();                  // grade + admit queued steady runs FIRST (front fresh for the live classify)
  // 1 Hz evaluator + OUTPUT-BLIND state classifier. A stale fold (field off) → nothing to grade.
  bool foldFresh = (altLastFoldMs != 0) && ((uint32_t)(nowMs - altLastFoldMs) < 3000u);
  if (!foldFresh) altLiveValid = false;
  if (foldFresh && altLiveValid) {
    // Grade the SAME statistic the records store: the episode's trailing-boxcar averages (coords AND
    // amps). A half-second snapshot divided by a best-ever boxcar reads ~95% on a healthy machine
    // (middle-of-wiggle ÷ top-of-wiggle); boxcar-vs-boxcar reads ~100. Ring empty (just became
    // eligible / output below the admission floor) → fall back to the fast EMA point.
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
    FrontStore<ALT_NAXIS> &gf = altGradeFront();
    altRefDist = gf.nearestNormDist(surf);
    if (altRefDist > 999.0f) altRefDist = 999.0f;
    float pred = 0;
    altState = (uint8_t)gf.classify(surf, altRefRadius, altIdwPower, altRidgeFrac, altRiskThresh, &pred);
    altLive_pred = pred;
    altRefOk = (altState == FRONT_MEASURED || altState == FRONT_ESTIMATED);
    altLive_pct = (altRefOk && pred > 0.1f) ? (altLive_gAmps / pred * 100.0f) : 0.0f;
    // Session stats = the SESSION-PLOTTED points: session-steady (lighter gate) AND graded. The trend is
    // NOT fed here — it is built purely from FULL-steady runs in altProcessEmits (spec §2).
    if (altSessSteady && altRefOk && altLive_pct > 0.0f) {
      altSessSum += altLive_pct; altSessN++;
      int bin = (int)(altLive_pct * 0.5f);    // 2%-wide histogram bins, 0..120%
      if (bin < 0) bin = 0;
      if (bin > 59) bin = 59;
      if (altSessHist[bin] < 65535) altSessHist[bin]++;
    }
    altSessSeriesPush(nowMs, altSessSteady && altRefOk && altLive_pct > 0.0f);
  } else {
    altRefOk = false; altLive_pct = 0;        // nothing running → no grade (state holds its last value)
  }
  // High-field-low-output alert — independent of the record book (covers degradation pushed into
  // unlearned territory where the gauge says "learning"). Self-clears when either condition lifts.
  {
    static uint32_t hiFieldSinceMs = 0;
    // altHiFieldPct reads as % of the live field ceiling (ccDutyCeiling: the lower of Max Field % and
    // the Max Field Volts term), not absolute duty — absolute 80% is unreachable on 24/36/48V banks
    // where the ceiling is ~50/33/25%, deadening the alert.
    bool cond = foldFresh && altLiveValid
                && altLive_duty >= altHiFieldPct * (ccDutyCeiling() / 100.0f) && altLive_amps <= altLowOutAmps;
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
  altSendLive();
  static uint8_t settCtr = 0;          // resend settings ~every 5s so reconnects get echoes
  if (++settCtr >= 5) { settCtr = 0; sendAltSettings(); }
}


// ALTERNATOR HEALTH — GUI-adjustable settings (registry-driven)
// One float registry → one /get handler loop + one boot-load loop +
// one "AltSettings" SSE echo. (ALT_SETTINGS[] is defined up with the param
// declarations so altDebugCsvSend can iterate it.) Avoids fragile CSV3 plumbing.
void altSettingsLoad() {
  // Three knobs are class-dependent 12V-value defaults, and this loader runs BEFORE InitSystemSettings
  // resolves SYSTEM_VOLTAGE_CLASS (NVS else vessel-JSON mirror). Their first creation is DEFERRED to
  // altSeedClassKnobs() right after it; existing keys still load verbatim here. The knobs have no
  // consumer until the runtime fold (altEpisodeSyncCfg), so the gap is safe.
  for (size_t i = 0; i < ALT_SETTING_COUNT; i++) {
    char key[16];
    snprintf(key, sizeof(key), "%s", ALT_SETTINGS[i].name);  // NVS key = registry name (15-char cap)
    if (!settingExists(key)) {
      if (strcmp(key, "altVbusTol") == 0 || strcmp(key, "altDutyTolPct") == 0 || strcmp(key, "altMinDuty") == 0) continue;
      settingWrite(key, String(*ALT_SETTINGS[i].ptr, 4).c_str());
    }
    else *ALT_SETTINGS[i].ptr = settingRead(key).toFloat();
  }
}

// Deferred first-creation seeding for the class-dependent registry knobs — call AFTER InitSystemSettings
// so SYSTEM_VOLTAGE_CLASS is authoritative. Volt-domain ×(V/12), duty-domain ×(12/V), identity at 12V; a live
// class change rescales them in applyNominalVoltageChange like every other class-scaled setting.
void altSeedClassKnobs() {
  if (!settingExists("altVbusTol"))    { altVbusTol    *= (float)SYSTEM_VOLTAGE_CLASS / 12.0f; settingWrite("altVbusTol",    String(altVbusTol, 4).c_str()); }
  if (!settingExists("altDutyTolPct")) { altDutyTolPct *= 12.0f / (float)SYSTEM_VOLTAGE_CLASS; settingWrite("altDutyTolPct", String(altDutyTolPct, 4).c_str()); }
  if (!settingExists("altMinDuty"))    { altMinDuty    *= 12.0f / (float)SYSTEM_VOLTAGE_CLASS; settingWrite("altMinDuty",    String(altMinDuty, 4).c_str()); }
}
bool altSettingsHandle(AsyncWebServerRequest *request) {
  bool handled = false;
  for (size_t i = 0; i < ALT_SETTING_COUNT; i++) {
    if (request->hasParam(ALT_SETTINGS[i].name)) {
      *ALT_SETTINGS[i].ptr = request->getParam(ALT_SETTINGS[i].name)->value().toFloat();
      char key[16];
      snprintf(key, sizeof(key), "%s", ALT_SETTINGS[i].name);  // NVS key = registry name (15-char cap)
      settingWrite(key, String(*ALT_SETTINGS[i].ptr, 4).c_str());
      handled = true;
    }
  }
  // Action (not a float knob): Reference Source selector — My History (0) | Uploaded File (1).
  // INDEPENDENT of Pause (altPaused is its own registry knob, handled above). Per spec §4.3, selecting
  // Uploaded defaults Pause = ON (the user may then flip Continue); switching back to My History leaves
  // Pause untouched. Selecting Uploaded with no uploaded surface present is ignored.
  if (request->hasParam("altSource")) {
    int src = request->getParam("altSource")->value().toInt();
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
    handled = true;
  }
  return handled;
}
void sendAltSettings() {
  char buf[320];
  int off = 0;
  for (size_t i = 0; i < ALT_SETTING_COUNT; i++) {
    int n = snprintf(buf + off, sizeof(buf) - off, (i ? ",%.4f" : "%.4f"), *ALT_SETTINGS[i].ptr);
    if (n < 0 || off + n >= (int)sizeof(buf)) break;   // truncated echo beats stack overrun (snprintf returns would-be length)
    off += n;
  }
  events.send(buf, "AltSettings");
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



// BOAT PERFORMANCE — Best-Ever Front engine instances (sail 3-D + motor 3-D). Mirrors the
// alternator module. Folds on a fast internal tick (~10 Hz, from boatPerf_tick). Sail axes
// {AWS, AWA, sea-state}; motor axes {RPM, headwind = AWS·cosAWA, sea-state}. Best-ever output =
// the user-selected STW or SOG. Generic engine (Episode/FrontStore): top of Xregulator.ino.
#define PERF_VER         3u
#define PERF_SAILF_MAGIC 0x50534652u  // 'PSFR' sail front blob
#define PERF_MOTF_MAGIC  0x504D4652u  // 'PMFR' motor front blob
#define PERF_NAXIS       3
#define PERF_FRONT_CAP   4096     // sparse support points (PSRAM); sized to be unreachable even for fast hulls AP-mode/no-prune (see ALT_HEALTH_LWLR_ENGINE_SPEC.md)
#define PERF_EP_RING_CAP 2048    // Episode trailing-boxcar buffer (PSRAM); only ~EP_AVG_WIN_MS of decimated samples are live (steadiness windows are in the per-axis deques)
#define PERF_PENDING_CAP 4096     // = front cap (PSRAM). Covers weeks offline because non-admitted raw
                                  // points are per-cell throttled at queue time (PerfPendingQ::push);
                                  // admitted best-ever points always get a slot.

static Episode<PERF_NAXIS>    sailEpisode,  motorEpisode;
static FrontStore<PERF_NAXIS> sailFront,    motorFront;
static RawSample<PERF_NAXIS>  *sailRing = nullptr,     *motorRing = nullptr;
static FrontPoint<PERF_NAXIS> *sailFrontBuf = nullptr, *motorFrontBuf = nullptr;
static FrontPoint<PERF_NAXIS> *sailPending = nullptr,  *motorPending = nullptr;
static int sailPendingCount = 0, motorPendingCount = 0;
static uint8_t *sailPendingAdm = nullptr, *motorPendingAdm = nullptr;   // 1 = admitted into the local front (never evicted/dropped)
static bool perfPendingOverflowWarned = false;   // one-shot until the next successful cloud sync
static String perfPendingSeededFrom = "";   // non-empty → this pending batch is an adopted import (provenance tag)
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
float perfSafetyMargin = 0.0f;  // kt — gate keeps only runs that strictly beat the front (no keep-bias: cloud gets raw episodes regardless, so sub-front samples only dragged down the local eval surface)
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
void  perfClearPending() { sailPendingCount = 0; motorPendingCount = 0; perfPendingSeededFrom = ""; perfPendingOverflowWarned = false; }   // cloud accepted the batch + tag

// Pending-queue admission (policy 2026-08-14). ADMITTED points (FrontStore::add returned true — they
// opened or beat a local best-ever cell) always queue: on overflow they evict the oldest non-admitted
// raw point, so the "front is built locally, cloud only prunes" invariant survives any offline
// stretch. Non-admitted raw points are per-cell rate limited (same half-axisScale cell as
// FrontStore::add dedupes in) so PERF_PENDING_CAP genuinely covers weeks, and drop with a one-shot
// warning when the queue is full. Template so Arduino's auto-prototype generator (which can't parse
// template types in signatures) leaves it alone.
#define PERF_RAW_CELL_THROTTLE_MS 600000u   // one non-admitted raw push per conditions-cell per 10 min
// Struct wrapper (not a free function): Arduino's auto-prototype generator emits a broken prototype
// for any free function with a template type in its signature; struct members are left alone.
struct PerfPendingQ {
  template <int N>
  static void push(FrontPoint<N> *q, uint8_t *adm, int &n, const FrontPoint<N> &p,
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
      memmove(&q[i], &q[i + 1], (size_t)(PERF_PENDING_CAP - 1 - i) * sizeof(FrontPoint<N>));
      memmove(&adm[i], &adm[i + 1], (size_t)(PERF_PENDING_CAP - 1 - i));
      q[PERF_PENDING_CAP - 1] = p; adm[PERF_PENDING_CAP - 1] = 1;
      return;
    }
  }
  // Queue holds 4096 unsynced ADMITTED records (needs that many distinct best-ever cells) — drop.
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
static float    perfPitchVal[PERF_PITCH_RING];
static uint32_t perfPitchMs[PERF_PITCH_RING];
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

// FIXME (sail/motor steady-state — dedicated rework needed): the timescales
// here are TOTALLY INADEQUATE for boat-speed/polar work and must be redone in a focused session.
//  - Dwells (AWS/AWA/RPM/headwind 3 s, sea 5 s) are far too short: a displacement hull takes tens of
//    seconds to minutes to settle to its polar speed after conditions change, so a 3 s "steady" window
//    records the boat mid-acceleration, not an equilibrium point. These need to be MINUTES.
//  - Averaging is wrong: this detector emits a short ~2 s boxcar (EP_AVG_WIN_MS, shared with alt where
//    it's correct). Boat speed is surgy (waves), so a polar point needs a MINUTES-scale average over
//    several wave cycles to capture sustained speed, not a single surf peak. Make avgWin per-instance.
//  - Acceptance model is too brittle: a steady interval should be ACCEPTED on its average behaviour,
//    not excluded outright because of a brief excursion. The sliding-window engine helps (an excursion
//    ages out instead of zeroing the dwell), but the accept/emit policy still needs to be rethought
//    around long-window averages rather than instantaneous in-band/out-of-band gating.
//  - Cascade when fixed: minutes-scale dwells also need PERF_MAXDWELL (the deque caps) raised to match.
// Alt-health is correct as-is (fast, well-filtered, short windows); this note is sail/motor ONLY.
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
// Snapshots the latest axis values, updates the live %, feeds the SAIL or MOTOR Episode (the other
// is fed an ineligible break so a run can't span a sail↔motor transition), and on a steady-run emit
// derives the surface point, gates against the front, pushes if best-ever, and queues it for upload.
// Wind axes are APPARENT; AWA is stored RAW (both-sided) to the front/pending — folded to |AWA| only
// for eval + the device front when perfFoldSymmetric. Speed = STW or SOG per perfSpeedSrc.
// Bench simulator state (perfSim* — written ONLY by perfSimTick, never the real NMEA/IMU globals).
static float perfSimTws = 0, perfSimTwa = 0, perfSimStw = 0, perfSimSog = 0;
static float perfSimHdg = 90, perfSimPitch = 0, perfSimRpm = 0;
static int   perfSimLoops = 0;
static uint32_t perfSimPtStartMs = 0;
static float perfSimFoul = 1.0f;

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

  // ── live point export (10 Hz): conditions only; prediction/state/% run at 1 Hz in boatPerf_tick.
  //    valid = "inputs usable" — the confidence state decides whether a % is shown. ──
  if (motoring) {
    motorLive_rpm = rpm; motorLive_hw = headwind; motorLive_spd = spd; motorLive_pitch = sea;
    motorLiveSrc = src; motorLiveValid = haveSpd; perfLiveValid = false;
  } else {
    perfLive_ws = aws; perfLive_wa = awa; perfLive_spd = spd; perfLive_pitch = sea;
    perfLiveSrc = src; perfLiveValid = (haveSpd && aws >= perfMinWindSpeed); motorLiveValid = false;
  }

  if (hardwarePresent != 1 && perfSimMode < 0.5f) { perfSteady = false; perfSettleSec = -1.0f; return; }   // display only
  if (perfPaused >= 0.5f) { perfSteady = false; perfSettleSec = -1.0f; return; }                           // paused: no learning

  // ── data-maturity hours: time spent actually moving in each mode (only while learning) ──
  if (perfDtMs > 0 && spd >= perfMinBoatSpeed) {
    if (motoring) perfMotorSeconds += perfDtMs / 1000.0;
    else          perfSailSeconds  += perfDtMs / 1000.0;
  }

  // ── feed both Episodes; the inactive one gets an ineligible break so runs don't span a mode change ──
  FrontPoint<PERF_NAXIS> ep;

  bool sailLearn = (sailFront.source != 1);
  bool sailElig = sailLearn && !motoring && haveSpd && spd >= perfMinBoatSpeed && aws >= perfMinWindSpeed;
  RawSample<PERF_NAXIS> ss; ss.x[0] = aws; ss.x[1] = awa; ss.x[2] = sea; ss.out = spd; ss.tMs = nowMs;
  ss.ex[0] = 0; ss.ex[1] = 0;   // sail keeps raw AWS/AWA as surface axes x0/x1 already — no extras needed
  if (sailEpisode.feed(sailElig, ss, &ep)) {
    FrontPoint<PERF_NAXIS> raw = ep;                                  // raw both-sided AWA → cloud
    FrontPoint<PERF_NAXIS> sp = ep; sp.x[1] = perfFoldAwa(ep.x[1]);   // device front: folded AWA (gate operates on folded coords)
    // Cell-local admit gate (alt pattern): unvisited cell → unconditional; local support → beat
    // the min(IDW, LWLR) hybrid bar − margin.
    bool sailLocal = sailFront.hasLocalSupport(sp.x);
    bool sailAdmitted = false;
    if (!sailLocal || sailFront.pushesHybrid(sp.x, sp.y, perfSafetyMargin, perfIdwPower, perfRidgeFrac)) {
      if (!sailLocal && sailFront.count >= PERF_FRONT_CAP) {
        // a genuinely NEW conditions cell dropped at capacity (in-cell improvements still land)
        if (!sailCapWarned) {
          sailCapWarned = true;
          queueConsoleMessage("WARN: sail front at capacity — new conditions are no longer recorded");
        }
      } else sailAdmitted = sailFront.add(sp);   // no console message on accept (counter on the dashboard)
    }
    // Queue AFTER the admit gate so admitted points carry their never-drop flag (sim never uploads).
    if (perfSimMode < 0.5f) PerfPendingQ::push(sailPending, sailPendingAdm, sailPendingCount, raw, sailAdmitted, sailFront.axisScale, nowMs);
  }

  bool motorLearn = (motorFront.source != 1);
  bool motorElig = motorLearn && motoring && haveSpd && spd >= perfMinBoatSpeed;
  RawSample<PERF_NAXIS> ms; ms.x[0] = rpm; ms.x[1] = headwind; ms.x[2] = sea; ms.out = spd; ms.tMs = nowMs;
  ms.ex[0] = aws; ms.ex[1] = awa;   // retain raw AWS/AWA (headwind is derived from them) for cloud diagnosis
  if (motorEpisode.feed(motorElig, ms, &ep)) {
    // Cell-local admit gate — same pattern as the sail site above.
    bool motorLocal = motorFront.hasLocalSupport(ep.x);
    bool motorAdmitted = false;
    if (!motorLocal || motorFront.pushesHybrid(ep.x, ep.y, perfSafetyMargin, perfIdwPower, perfRidgeFrac)) {
      if (!motorLocal && motorFront.count >= PERF_FRONT_CAP) {
        // a genuinely NEW conditions cell dropped at capacity (in-cell improvements still land)
        if (!motorCapWarned) {
          motorCapWarned = true;
          queueConsoleMessage("WARN: motor front at capacity — new conditions are no longer recorded");
        }
      } else motorAdmitted = motorFront.add(ep);   // no console message on accept (counter on the dashboard)
    }
    // Queue AFTER the admit gate so admitted points carry their never-drop flag (sim never uploads).
    if (perfSimMode < 0.5f) PerfPendingQ::push(motorPending, motorPendingAdm, motorPendingCount, ep, motorAdmitted, motorFront.axisScale, nowMs);
  }

  // steady-run indicator: the active mode's Episode is currently accumulating eligible samples
  perfSteady = motoring ? (motorEpisode.count > 0) : (sailEpisode.count > 0);
  {
    bool aelig = motoring ? motorElig : sailElig;
    perfSettleSec = !aelig ? -1.0f
                   : perfSteady ? 0.0f
                   : (motoring ? motorEpisode.settleRemainSec(nowMs) : sailEpisode.settleRemainSec(nowMs));
  }
}
// ---- NMEA SIMULATOR (bench testing, no boat) ----
// Writes ONLY to dedicated perfSim* vars (never the real NMEA/RPM/IMU globals) so it can't
// touch the control loop or other subsystems. Models a realistic sailboat: each "leg" picks a
// random operating point (weighted toward how a boat actually sails), holds it steady within the
// episode bands long enough to bank ONE record, then jumps to a new one — so the best-ever front
// fills ORGANICALLY over time (sparse → dense → converged) instead of re-tracing a fixed grid.
// Speed = hull × wind-response(TWS) × polar-shape(TWA) × sea derate × slow fouling × noise.
// (perfSim* vars declared above the fold.)
#define PERF_SIM_HOLD_MS 14000u    // hold each leg > max steady time so one run forms + emits

// Normalized best-speed fraction vs TWA (0..180, 10° steps): zero in the no-go zone, peak on the
// reach (~100-110°), still ~0.72 of peak dead downwind — matches the dashboard's render shape.
static float perfSimShape(float pa) {
  static const float S[19] = {0,0,0,0.42f,0.60f,0.74f,0.84f,0.90f,0.95f,0.98f,1.00f,1.00f,0.98f,0.94f,0.89f,0.85f,0.81f,0.78f,0.72f};
  if (pa < 0) pa = -pa; if (pa > 180) pa = 360 - pa; if (pa < 0) pa = 0;
  float f = pa / 10.0f; int i = (int)f; if (i > 17) i = 17; float t = f - i;
  return S[i] * (1.0f - t) + S[i + 1] * t;
}
static void perfSimTick(uint32_t nowMs) {
  const float HULL = 7.6f;                  // typical ~35-40 ft sailboat hull speed (kt)
  // Current leg's target — persists across ticks until the hold expires, then a new one is drawn.
  static float legTws = 12, legTwa = 95, legRpm = 0, legSea = 1.0f;
  static uint8_t legMotor = 0; static bool legInit = false;
  if (perfSimPtStartMs == 0) perfSimPtStartMs = nowMs;
  if (!legInit || nowMs - perfSimPtStartMs >= PERF_SIM_HOLD_MS) {
    legInit = true; perfSimPtStartMs = nowMs; perfSimLoops++;          // count legs (drives fouling)
    legMotor = (random(0, 100) < 20) ? 1 : 0;                          // ~20% of legs under engine
    if (legMotor) {
      legRpm = 1000.0f + (float)random(0, 2401);                       // 1000..3400 RPM
      legTws = (float)random(0, 121) / 10.0f;                          // 0..12 kt ambient wind
      legTwa = (float)random(0, 181);
      legSea = 0.3f + (float)random(0, 151) / 100.0f;                  // 0.3..1.8
    } else {
      // wind speed: triangular (avg of two uniforms) → peaks ~13 kt, spans ~4..23
      legTws = 4.0f + (float)(random(0, 191) + random(0, 191)) / 20.0f;
      // wind angle: triangular → peaks ~105°, spans ~30..176 (full range incl. downwind)
      legTwa = 30.0f + (float)(random(0, 1461) + random(0, 1461)) / 20.0f;
      legSea = 0.3f + legTws * 0.12f + (float)random(-30, 31) / 100.0f;// chop grows with wind
      if (legSea < 0) legSea = 0; if (legSea > 5) legSea = 5;
      legRpm = 0;
    }
  }
  // Small in-band jitter so the run looks "live" but stays steady within the episode tolerances.
  uint8_t motor = legMotor;
  float tws = legTws + (float)random(-20, 21) / 100.0f;               // ±0.2 kt
  float twa = legTwa + (float)random(-10, 11) / 10.0f;               // ±1.0°
  float rpm = legMotor ? (legRpm + (float)random(-15, 16)) : 0.0f;    // ±15 RPM
  float sea = legSea;
  float v;
  if (motor) {
    v = HULL * (1.0f - expf(-(rpm - 600.0f) / 1300.0f));              // idle ~2kt, cruise ~6.5, → hull
    if (v < 0) v = 0;
    float hw = tws * cosf(twa * (float)PI / 180.0f);                  // ambient headwind while motoring
    v -= 0.015f * (hw > 0 ? hw : 0);
  } else {
    float resp = 1.0f - expf(-tws / 8.0f);                           // wind response: ~.63@8 .86@16 .94@22
    v = HULL * resp * perfSimShape(twa);                              // × normalized polar shape
  }
  float seaStd = sea / 1.414f;
  v *= (1.0f - 0.025f * seaStd);                                      // rougher seas → slower
  if (perfSimLoops >= 40) perfSimFoul -= 0.0002f;                     // gentle fouling after ~40 legs (~9 min)
  if (perfSimFoul < 0.85f) perfSimFoul = 0.85f;
  v *= perfSimFoul;
  v *= 1.0f + (float)random(-25, 26) / 1000.0f;                      // ±2.5% measurement noise
  if (v < 0.2f) v = 0.2f;
  perfSimPitch = sea * sinf(2.0f * (float)PI * (float)(nowMs % 4000) / 4000.0f);  // wave-like pitch
  perfSimTws = tws; perfSimTwa = twa; perfSimHdg = 90.0f;
  perfSimRpm = motor ? rpm : 0.0f;
  perfSimStw = v; perfSimSog = v;                            // no current; SOG == STW
}

// ---- live telemetry registry (PILOT: single source of truth for payload + schema) ----
// One {name, getter} table builds BOTH the PerfLive payload AND the /perfschema field list,
// so the firmware↔dashboard field contract cannot desync (no hand-kept parallel JS array,
// no format-string/count drift). Adding a field = add ONE row. All values sent as floats.
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
static float plf_syncAgo()  { if (lastBoatPerfSyncEpoch <= 0 || !timeIsSynced) return -1.0f;
                              time_t n = time(NULL); return (n > (time_t)lastBoatPerfSyncEpoch) ? (float)(n - (time_t)lastBoatPerfSyncEpoch) : 0.0f; }
static float plf_sailHours(){ return (float)(perfSailSeconds / 3600.0); }   // data-maturity hours
static float plf_steady()   { return (float)perfSteady; }                   // in a steady-run right now
static float plf_settleSec(){ return perfSettleSec; }                       // dwell s remaining; 0 settled, -1 idle
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
  char buf[256];
  int off = 0;
  for (size_t i = 0; i < PERF_LIVE_COUNT; i++) {
    int n = snprintf(buf + off, sizeof(buf) - off, (i ? ",%.3f" : "%.3f"), PERF_LIVE[i].get());
    if (n < 0 || off + n >= (int)sizeof(buf)) break;   // truncated echo beats stack overrun (snprintf returns would-be length)
    off += n;
  }
  events.send(buf, "PerfLive");
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
static float pmlf_steady()     { return (float)perfSteady; }                    // in a steady-run right now
static float pmlf_settleSec()  { return perfSettleSec; }                        // dwell s remaining; 0 settled, -1 idle
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
  char buf[256];
  int off = 0;
  for (size_t i = 0; i < PERF_MOTOR_LIVE_COUNT; i++) {
    int n = snprintf(buf + off, sizeof(buf) - off, (i ? ",%.3f" : "%.3f"), PERF_MOTOR_LIVE[i].get());
    if (n < 0 || off + n >= (int)sizeof(buf)) break;   // truncated echo beats stack overrun (snprintf returns would-be length)
    off += n;
  }
  events.send(buf, "MotorLive");
}

// ---- persistence (Phase-0 scaffold; field-off-gated by caller) ----
void boatPerfSave() {
  if (dbgRingsSynthetic) return;   // fillmax/clearmax: RAM rings are synthetic/empty — keep the real flash blobs
  if (!sailFrontBuf || hardwarePresent != 1) return;
  uint32_t suw = ((uint32_t)sailFront.source  << 8) | (uint32_t)PERF_NAXIS;
  uint32_t muw = ((uint32_t)motorFront.source << 8) | (uint32_t)PERF_NAXIS;
  writePsramBlob("/sailfront.bin",  PERF_SAILF_MAGIC, PERF_VER, suw, sailFrontBuf,  sizeof(FrontPoint<PERF_NAXIS>), PERF_FRONT_CAP, 0, sailFront.count);
  writePsramBlob("/motorfront.bin", PERF_MOTF_MAGIC,  PERF_VER, muw, motorFrontBuf, sizeof(FrontPoint<PERF_NAXIS>), PERF_FRONT_CAP, 0, motorFront.count);
}
static void boatPerfLoad() {
  uint32_t uw = 0;
  if (sailFrontBuf)  { sailFront.count  = (int)readPsramBlob("/sailfront.bin",  PERF_SAILF_MAGIC, PERF_VER, sailFrontBuf,  sizeof(FrontPoint<PERF_NAXIS>), PERF_FRONT_CAP, &uw, false); sailFront.source  = (uint8_t)((uw >> 8) & 0xFF); }
  if (motorFrontBuf) { motorFront.count = (int)readPsramBlob("/motorfront.bin", PERF_MOTF_MAGIC,  PERF_VER, motorFrontBuf, sizeof(FrontPoint<PERF_NAXIS>), PERF_FRONT_CAP, &uw, false); motorFront.source = (uint8_t)((uw >> 8) & 0xFF); }
}

// ---- front CSV (the artifact). A boat "curve" = the PAIR of sail + motor BEFRONT1 blocks, mode-
//      tagged. target: 0 = sail, 1 = motor. (int target, not a template-typed param — keeps
//      Arduino's auto-prototype pass from referencing FrontStore<3> before PERF_NAXIS is defined.) ----
// /perfcurve.csv — held best-ever fronts as the BEFRONT1 artifact pair (SAIL block then MOTOR block).
// Streamed row-by-row: logical lines are SAIL header, sail rows, MOTOR header, motor rows — so two
// 4096-cap fronts never concatenate into one huge heap String. Same constant-RAM chunker as the alt
// senders. Sail x0 is 2dp, motor x0 is 0dp.
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
  if (sp) perfIngestBlock(sp, 0);
  if (mp) perfIngestBlock(mp, 1);
  return (sp || mp);
}

// ---- cloud upload: batch of accepted points since last upload (sail + motor); pruned fronts back ----
// Schema: {device_uid,token,ts,sys:"BOAT",speedSrc,foldSym,pruneK,idwPower,
//          sail:[[aws,awa,sea,spd,nSamp]...], motor:[[rpm,hw,sea,spd,nSamp,aws,awa]...]}. AWA raw both-sided;
//          motor also carries raw AWS/AWA (headwind is derived from them) for cloud diagnosis.
// Pending cleared on a successful response (executeUploadBoatPerf → perfIngestFrontCsv → perfClearPending).
bool buildBoatPerfPayload(char *buf, size_t size) {
  if ((sailPendingCount == 0 && motorPendingCount == 0) || authToken.isEmpty()) return false;
  time_t now_ts = time(NULL);
  int off = snprintf(buf, size,
    "{\"device_uid\":\"%s\",\"token\":\"%s\",\"ts\":\"%s\",\"sys\":\"BOAT\",\"speedSrc\":%d,\"foldSym\":%d,\"pruneK\":%d,\"idwPower\":%.2f,",
    device_id_hex, authToken.c_str(), formatTimestamp(now_ts),
    (int)perfSpeedSrc, (perfFoldSymmetric >= 0.5f) ? 1 : 0, (int)perfPruneK, perfIdwPower);
  if (off < 0 || (size_t)off >= size) return false;
  if (perfPendingSeededFrom.length()) {   // adopted import: tag the whole batch as borrowed provenance
    off += snprintf(buf + off, size - off, "\"seededFrom\":\"%s\",", perfPendingSeededFrom.c_str());
    if (off < 0 || (size_t)off >= size) return false;
  }
  off += snprintf(buf + off, size - off, "\"sail\":[");
  bool first = true;
  for (int k = 0; k < sailPendingCount; k++) {
    if (size - (size_t)off < 64) break;
    FrontPoint<PERF_NAXIS> &p = sailPending[k];
    off += snprintf(buf + off, size - off, "%s[%.2f,%.1f,%.3f,%.2f,%u]", first ? "" : ",", p.x[0], p.x[1], p.x[2], p.y, p.nSamp);
    first = false;
  }
  off += snprintf(buf + off, size - off, "],\"motor\":[");
  first = true;
  for (int k = 0; k < motorPendingCount; k++) {
    if (size - (size_t)off < 64) break;
    FrontPoint<PERF_NAXIS> &p = motorPending[k];
    off += snprintf(buf + off, size - off, "%s[%.0f,%.1f,%.3f,%.2f,%u,%.2f,%.1f]",
                    first ? "" : ",", p.x[0], p.x[1], p.x[2], p.y, p.nSamp, p.ex[0], p.ex[1]);  // ex=raw AWS,AWA
    first = false;
  }
  off += snprintf(buf + off, size - off, "]}");
  if (off < 0 || (size_t)off >= size - 1) return false;
  return true;
}

// Ingest a BEFRONT1 pair UPLOADED from the browser (Load CSV) — replace both fronts, then set the
// mode the user chose at import: fixed=true → FIXED + paused (hold the borrowed polar exactly as-is);
// fixed=false → LEARNED + resumed (use it as a starting point, keep refining from your own sailing).
// Persists immediately so the import survives reboot (the file isn't on the device to re-load).
// Non-static so the /perfUploadFront handler in 3_functions.ino can call it (sailFront/motorFront are
// file-static here). Mutates the body buffer.
bool perfUploadFrontCsv(char *body, bool fixed) {
  if (!sailFrontBuf || !body) return false;
  bool ok = perfIngestFrontCsv(body);
  if (!ok) return false;
  uint8_t src = fixed ? 1 : 0;
  sailFront.source = src; motorFront.source = src;
  perfPaused = fixed ? 1.0f : 0.0f;
  settingWrite(NK_perfPaused, fixed ? "1.0000" : "0.0000");
  if (fixed) {                                  // FREEZE — local only, never uploaded
    sailPendingCount = motorPendingCount = 0; perfPendingSeededFrom = "";
  } else {                                      // LEARN — adopt to cloud: stage both fronts (tagged), keep refining
    sailPendingCount = motorPendingCount = 0;   // replace pending with the imported set (pure seeded batch)
    for (int i = 0; i < sailFront.count  && sailPendingCount  < PERF_PENDING_CAP; i++) { if (sailPendingAdm)  sailPendingAdm[sailPendingCount]   = 1; sailPending[sailPendingCount++]   = sailFrontBuf[i]; }
    for (int i = 0; i < motorFront.count && motorPendingCount < PERF_PENDING_CAP; i++) { if (motorPendingAdm) motorPendingAdm[motorPendingCount] = 1; motorPending[motorPendingCount++] = motorFrontBuf[i]; }
    perfPendingSeededFrom = "import";
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
  if (!sailRing || !motorRing || !sailFrontBuf || !motorFrontBuf || !sailPending || !motorPending
      || !sailPendingAdm || !motorPendingAdm) {
    queueConsoleMessage("ERROR: BoatPerf ps_malloc failed"); return;
  }
  memset(sailFrontBuf,  0, (size_t)PERF_FRONT_CAP * sizeof(FrontPoint<PERF_NAXIS>));
  memset(motorFrontBuf, 0, (size_t)PERF_FRONT_CAP * sizeof(FrontPoint<PERF_NAXIS>));
  // Per-axis deque window caps. Sail axes {AWS, AWA, sea}, motor axes {RPM, headwind, sea};
  // longest dwell is sea-state (~5 s default), 60 s headroom. Index 3 = output band (unused; perf
  // leaves outCfg disabled), small.
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
  queueConsoleMessageF("BoatPerf init: sail %d pts (%s), motor %d pts (%s)",
                       sailFront.count, sailFront.source ? "FIXED" : "LEARNED",
                       motorFront.count, motorFront.source ? "FIXED" : "LEARNED");
}
void resetBoatPerformance() {
  if (!sailFrontBuf) return;
  sailFront.count = 0; sailFront.source = 0; motorFront.count = 0; motorFront.source = 0;
  sailPendingCount = motorPendingCount = 0; sailCapWarned = motorCapWarned = false;
  sailEpisode.clearRun();  sailEpisode.ringHead = 0;  sailEpisode.ringCount = 0;
  motorEpisode.clearRun(); motorEpisode.ringHead = 0; motorEpisode.ringCount = 0;
  perfPitchHead = 0; perfPitchCount = 0;
  perfSailSeconds = 0.0; perfMotorSeconds = 0.0;   // data-maturity hours reset with the learned data
  perfLiveValid = false; motorLiveValid = false; perfLive_pct = 0; motorLive_pct = 0;
  fsTakeLock();
  LittleFS.remove("/sailfront.bin");
  LittleFS.remove("/motorfront.bin");
  fsReleaseLock();
  queueConsoleMessage("BoatPerf: full reset (Clear All)");
}

// Motor-only counterpart of resetBoatPerformance(). The motoring front's axis 0 is engine RPM, so a
// tach rescale invalidates it; the sail front (AWS/AWA/sea-state) has no RPM axis and must survive.
void resetMotorFrontOnly() {
  if (!sailFrontBuf) return;  // shared PSRAM arena backs both fronts
  motorFront.count = 0; motorFront.source = 0;
  motorPendingCount = 0; motorCapWarned = false;
  motorEpisode.clearRun(); motorEpisode.ringHead = 0; motorEpisode.ringCount = 0;
  perfMotorSeconds = 0.0;
  motorLiveValid = false; motorLive_pct = 0;
  fsTakeLock();
  LittleFS.remove("/motorfront.bin");
  fsReleaseLock();
  queueConsoleMessage("BoatPerf: motoring front cleared (engine-RPM axis rescaled); sail front kept");
}

// ---- tick (call from loop()): fold at ~10 Hz, send live telemetry + settings echo at ~1 Hz ----
void boatPerf_tick(uint32_t nowMs) {
  static uint32_t lastFold = 0, lastSse = 0;
  static uint8_t  perfSseStep = 0;   // 0 = idle; 1..4 = the 1 Hz SSE burst, SPREAD one step per loop pass
  static uint8_t  sc = 0;            // sendPerfSettings cadence: every 5th SSE cycle
  if (!sailFrontBuf) return;
  // One-heavy-per-pass gate: defer the whole tick if any sub-op is due (or we're mid-SSE-spread) but
  // another heavy ran this pass (lastFold/lastSse/perfSseStep unchanged → still due next pass).
  // The 1 Hz SSE burst (classify + 2-3 SSE pushes, ~7 ms if stacked) is split one step per pass so it
  // never lands as a single fat tick — it was the tallest loop pole and bounded CH1/Vbus worst spacing.
  bool foldDue  = ((uint32_t)(nowMs - lastFold) >= 100);
  bool sseStart = ((uint32_t)(nowMs - lastSse) >= 1000);
  if (foldDue || sseStart || perfSseStep) {
    if (gHeavyRanThisPass) return;
    gHeavyRanThisPass = true;
  } else {
    if (perfSimMode >= 0.5f) perfSimTick(nowMs);        // keep the sim feed alive on idle passes
    return;
  }
  if (perfSimMode >= 0.5f) perfSimTick(nowMs);          // bench simulator feeds the sim vars first
  if (sseStart && perfSseStep == 0) { lastSse = nowMs; perfSseStep = 1; }   // arm the 1 Hz burst
  // Do exactly ONE heavy sub-op per pass: advance the SSE burst if one is pending, else run the fold.
  // (Prioritising the burst defers the 10 Hz fold by at most a few passes — jitter, not loss.)
  if (perfSseStep) {
    switch (perfSseStep) {
      case 1:
        // step 1: 1 Hz evaluator + OUTPUT-BLIND state classifier for the active mode (mirrors
        // altHealth_tick). NO clamp on the % — it may exceed 100 vs a stale front.
        if (motorLiveValid) {
          float surf[PERF_NAXIS] = { motorLive_rpm, motorLive_hw, motorLive_pitch };
          float pred = 0;
          motorState = (uint8_t)motorFront.classify(surf, perfRefRadius, perfIdwPower, perfRidgeFrac, perfRiskThresh, &pred);
          motorLive_best = pred;
          bool graded = (motorState == FRONT_MEASURED || motorState == FRONT_ESTIMATED);
          motorLive_pct = (graded && pred > 0.1f) ? (motorLive_spd / pred * 100.0f) : 0.0f;
        } else if (perfLiveValid) {
          float surf[PERF_NAXIS] = { perfLive_ws, perfFoldAwa(perfLive_wa), perfLive_pitch };
          float pred = 0;
          perfState = (uint8_t)sailFront.classify(surf, perfRefRadius, perfIdwPower, perfRidgeFrac, perfRiskThresh, &pred);
          perfLive_best = pred;
          bool graded = (perfState == FRONT_MEASURED || perfState == FRONT_ESTIMATED);
          perfLive_pct = (graded && pred > 0.1f) ? (perfLive_spd / pred * 100.0f) : 0.0f;
        }
        perfSseStep = 2;
        break;
      case 2: perfSendLive();      perfSseStep = 3; break;                  // step 2: sailing live SSE
      case 3: perfSendMotorLive(); perfSseStep = (++sc >= 5) ? 4 : 0; break; // step 3: motoring live SSE
      case 4: sc = 0; sendPerfSettings(); perfSseStep = 0; break;           // step 4: settings echo (1-in-5)
    }
  } else if (foldDue) {
    lastFold = nowMs; perfFold_tick(nowMs);                                 // ~10 Hz fold
  }
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
    if (request->hasParam(PERF_SETTINGS[i].name)) {
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
    resetBoatPerformance();
    queueConsoleMessage("BoatPerf: speed source changed → Clear All (front reset)");
  }
  // Action (not a float knob): LEARNED↔FIXED source toggle.
  if (request->hasParam("perfSource")) {   // 1 = FIXED (freeze + pause), 0 = LEARNED (resume)
    int src = request->getParam("perfSource")->value().toInt();
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
  events.send(buf, "PerfSettings");
}

// Self-describing schema (served at /perfschema). The dashboard fetches this ONCE on load
// and zips these names against the PerfLive / PerfSettings payload values — so it never keeps
// its own field array and can't fall out of sync with the firmware tables above.
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
  if (!loggingActive) return;  // Stop Logs: skip append, freeze buffer
  if (!cvLogReady || !cvLog) return;
  // if (sysMode != SYS_MODE_AUTO) return;  // this was dumb, probably remove later

  // Pause watchdog — same pattern as thermalLog
  if (cvLogPaused) {
    if ((uint32_t)(nowMs - cvLogPausedAtMs) > THERMAL_LOG_PAUSE_TIMEOUT_MS) {
      Serial.println("cvLog: pause watchdog triggered - resuming");
      cvLogPaused = false;
    } else {
      return;
    }
  }

  CvLogEntry &e = cvLog[cvLogHead];

  e.ts = nowMs;
  e.battV = (int16_t)(IBV * 100.0f);
  e.targV = (int16_t)(ChargingVoltageTarget * 100.0f);
  e.vErrorMv = (int16_t)((ChargingVoltageTarget - IBV) * 1000.0f);
  e.dvdt_x1000 = (int16_t)clamp_f(g_fastOvDvdt * 1000.0f, -32767.0f, 32767.0f);
  e.vPred = (int16_t)(g_fastOvVpred * 100.0f);
  e.fastOvCap = (int16_t)(g_fastOvCurrentCap * 10.0f);
  e.cv_I_x10 = (int16_t)clamp_f(cv_I * 10.0f, -32767.0f, 32767.0f);
  e.Icv_x10 = (int16_t)clamp_f(Icv * 10.0f, -32767.0f, 32767.0f);
  e.uTarget = (int16_t)clamp_f((float)uTargetAmps * 10.0f, -32767.0f, 32767.0f);
  e.spLimited = (int16_t)clamp_f(setpointLimited * 10.0f, -32767.0f, 32767.0f);
  e.iMeas = (int16_t)clamp_f(MeasuredAmps * 10.0f, -32767.0f, 32767.0f);
  e.duty = (int16_t)(dutyCycle * 10.0f);

  e.flags = 0;
  if (g_fastOvClampActive) e.flags |= (1 << 0);
  if (pidLog_voltageLoopRanThisTick) e.flags |= (1 << 1);
  if (voltageControlActive) e.flags |= (1 << 2);
  // bit 3 assigned below (iExcess BULK sub-mode)
  if (g_fastOvHardActive) e.flags |= (1 << 4);

  e.awState = g_awState;
  e.rpm = (int16_t)constrain((int)RPM, -32768, 32767);
  e.battV_filt_x100 = (int16_t)clamp_f(IBV_filtered * 100.0f, -32767.0f, 32767.0f);
  e.cvDSlope_x10000 = (int16_t)clamp_f(cvDSlope * 10000.0f, -32767.0f, 32767.0f);
  e.ch1IntervalMs = (int16_t)g_ch1LastIntervalMs;
  e.battI_x10 = (int16_t)clamp_f(getBatteryCurrent() * 10.0f, -32767.0f, 32767.0f);
  e.dBcur_dt_Aps = (int16_t)clamp_f(g_dBcur_dt, -32767.0f, 32767.0f);
  e.voltLoopIntervalMs = pidLog_voltageLoopRanThisTick ? (int16_t)g_voltLoopActualIntervalMs : 0;
  e.inaIntervalMs = (int16_t)ina_last_ms;
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

// Effective alternator-current sample rate (Hz). Every swept-sine measurement is capped by this,
// not by how fast the control tick or the sine generator runs — CH1 is where new information
// enters. 0 before the first read.
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
// Deliberately no inaAtWorst intermediate — worst writes directly to the public ina_worst_at.
// An intermediate once produced ina_worst_at=0 alongside nonzero over2x/avg (cause never found),
// so the published variable is the single source of truth. Same for ina_worst_2m.
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

// Full reset including all-time accumulators — called by the web-side
// Reset Peaks button. resetINA228IntervalWindows() above only clears the
// 10s/2m windows; this also wipes ina_worst_at / ina_avg_at / ina_over2x_at
// and the static counters that feed them.
void resetINA228AllStats() {
  resetINA228IntervalWindows();
  inaAtSum     = 0;
  inaAtCount   = 0;
  inaAtOver2x  = 0;
  ina_avg_at   = 0.0f;
  ina_worst_at = 0;
  ina_over2x_at = 0;
}

// Drop the previous-read timestamp from outside this file. loop() calls this every
// pass the field gate is low — the INA equivalent of the pf tracker's per-field-off-pass
// pfHasPrev reset. Without it, a field-off flash write that stalls the whole loop (so
// _ReadAnalogInputs_inner never runs to flip inaFastModeActive to slow mode) gets logged
// as a fast-mode (field-on) read interval the next time a read fires, inflating ina_worst_at.
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
  // The "2m" worst is also written directly, so ina_worst_2m means
  // "max iv since last fast-mode rising edge" rather than a strict
  // 2m rolling window. The bucket-based avg + over2x for 2m
  // remain rolling. Tooltips should say "since fast-mode start" for these.
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

  // Publish 2m stats. ina_worst_2m is updated DIRECTLY on every sample
  // (above), so this block does NOT touch it — only avg + over2x come from
  // the bucket ring.
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

  // Publish all-time stats. ina_worst_at is also updated directly above,
  // so the publish only handles avg + over2x.
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
    // index.html must revalidate on every load. Cached without a network round trip it renders a
    // complete, live-looking dashboard off a device that is unreachable (wrong network, still in
    // AP/config mode) — every reading dashed out and vesselInfoComplete stuck false.
    // The rest of the bundle revalidates too (was max-age=3600): the five assets are a MATCHED SET,
    // and a browser holding an hour-old styles.css against a just-updated index.html renders new
    // markup with no rules for it — a viewBox-only <svg> with no matching CSS inflates to its
    // container. loadFileToRAM always mints an ETag alongside the data, so every one of these is a
    // 304 with no body, not a re-download.
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
// systemID_tick() — plant delay measurement step test. 8 phases: BASELINE + 3x (UP/DOWN), each held
// SYSID_STEP_HOLD_MS to let the plant settle. Called on every fresh CH1 hit from AdjustFieldLearnMode;
// the caller MUST force govMode = GOV_BYPASS_SLEW, put the PID in MANUAL with its integrator reset to
// dutyOut, and drive duty from dutyOut. PSRAM buffer allocated once on first run, never freed.
// The fall threshold is step-RELATIVE, not a fixed percentage: stepAmp = upMin - quietMax, fall at
// upMin - 0.3*stepAmp (rise at quietMax * 1.2). The scan is bounded by t_down_end so it cannot bleed
// into later phases. A reported delay of -1 ms means the crossing was never found in the buffer.

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

  // ── EASE-OUT: after a run completes, ramp the field down to baseDuty over ~1.5 s before handing
  // back to normal control. The override (and thus the protection gate) stays held until the field
  // has settled, so the fast current collapse doesn't trip a protection. ──
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

  // ── STABILIZE phase: P-control to SystemIDStabilizeAmps before baseline ──
  // Adjust duty once per second. Once the 5-second rolling average is within
  // ±3A of the target, advance. Abort if timeout exceeded.
  if (phase == SYSID_STABILIZE) {
    if (nowMs - stabilizeLastAdjMs >= 1000) {
      float err = SystemIDStabilizeAmps - ampsRaw;
      baseDuty = constrain(baseDuty + err * 0.5f, 5.0f, 80.0f);
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

  // ── SINE SWEEP phase (open-loop plant Bode): swept sine on duty, PID off ──
  // dutyOut = baseDuty + amp·(1+sin(2π f t)) — baseDuty is the trough, so the swing is entirely
  // upward and the current can't clip at the bottom. Record (ts,duty,amps) each tick; advance to
  // the next log-spaced frequency after (1 settle + N analysed) cycles. Lock-in DFT in PROCESSING
  // extracts per-frequency gain and phase (AC amplitude still SystemIDStepAmplitude).
  if (phase == SYSID_SINE) {
    if (!sineSegStarted) {
      sineSegStartMs[sineIdx] = nowMs;
      sineSegStarted = true;
    }
    float f = sineFreq[sineIdx];
    float tSec = (float)(nowMs - sineSegStartMs[sineIdx]) / 1000.0f;
    // (1+sin) offset: baseDuty is the wave TROUGH, not the midpoint (no-clip rationale in block header).
    float drive = SystemIDStepAmplitude * (1.0f + sinf(2.0f * (float)M_PI * f * tSec));
    float d = constrain(baseDuty + drive, 0.0f, 100.0f);
    dutyOut = d;

    // Engine-speed stats over the sine phase — advisory context for /sysidbode, never gated on.
    float rNow = (float)RPM;
    if (sidRpmN == 0) { systemIDRpmMin = rNow; systemIDRpmMax = rNow; }
    else {
      if (rNow < systemIDRpmMin) systemIDRpmMin = rNow;
      if (rNow > systemIDRpmMax) systemIDRpmMax = rNow;
    }
    sidRpmSum += rNow; sidRpmN++;
    systemIDRpmAvg = (float)(sidRpmSum / sidRpmN);

    // Frequency-adaptive decimation: record ~24 samples per cycle of the current tone.
    // At high frequencies this is essentially every CH1 sample (interval below the ~3ms
    // sample period); at low frequencies it throttles hard so a multi-minute, low-start
    // sweep can't exhaust the 15000-sample buffer and starve the later (high-freq) points.
    // 24/cycle is far above Nyquist, so the per-frequency lock-in stays clean. The DFT uses
    // each sample's real timestamp, so the non-uniform rate across segments is harmless.
    uint32_t sineSampleIntervalMs = (uint32_t)(1000.0f / (f * 24.0f));  // period/24
    if ((uint32_t)(nowMs - lastSineSampleMs) >= sineSampleIntervalMs) {
      lastSineSampleMs = nowMs;
      if (sysIDSampleCount < SYSID_BUF_SIZE) {
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
  // UP phases command baseDuty + amplitude; all others hold baseDuty.
  bool isUpPhase = (phase == SYSID_UP_1 || phase == SYSID_UP_2 || phase == SYSID_UP_3);
  float phaseDuty = isUpPhase
                      ? constrain(baseDuty + SystemIDStepAmplitude, 0.0f, 100.0f)
                      : baseDuty;
  dutyOut = phaseDuty;

  // ── Record sample ────────────────────────────────────────────────────────
  if (sysIDSampleCount < SYSID_BUF_SIZE) {
    sysIDBuffer[sysIDSampleCount++] = { nowMs, phaseDuty, ampsRaw };
  } else {
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
  // phaseStartMs layout (with STABILIZE prefix):
  //   [0] STABILIZE start  [1] BASELINE start  [2] UP_1 start   [3] DOWN_1 start
  //   [4] UP_2 start       [5] DOWN_2 start    [6] UP_3 start   [7] DOWN_3 start
  //   [8] test end (DOWN_3 end)
  //
  // Preceding quiet phase for each rise: BASELINE[1], DOWN_1[3], DOWN_2[5]
  // UP phase starts:                     UP_1[2],     UP_2[4],   UP_3[6]
  // UP phase ends (= next phase start):  DOWN_1[3],   DOWN_2[5], DOWN_3[7]
  //
  // Preceding UP phase for each fall:    UP_1[2],  UP_2[4],  UP_3[6]
  // DOWN phase starts:                   DOWN_1[3],DOWN_2[5],DOWN_3[7]

  // ── Sine-sweep post-process (lock-in DFT per frequency) ──────────────────
  // For each swept frequency, correlate (amps − mean) against sin/cos at that exact
  // frequency over its analysis window. Rejects all other tones (incl. belt ripple)
  // to ~0. gain = output A per %duty; phase = output lag in degrees.
  if (phase == SYSID_PROCESSING && curTestType == 1) {
    systemIDBodeCount = 0;
    float prevLagDeg = 0.0f;   // anchors the ±180° unwrap; lag only grows across a sweep
    for (int p = 0; p < SYSID_SINE_NPOINTS; p++) {
      float f = sineFreq[p];
      uint32_t segStart = sineSegStartMs[p];
      uint32_t segEnd   = (p + 1 < SYSID_SINE_NPOINTS) ? sineSegStartMs[p + 1] : sineSweepEndMs;
      uint32_t anaStart = segStart + (uint32_t)(1000.0f / f);  // skip 1 settle cycle
      // Integer-cycle window: truncate the analysis span to a WHOLE number of drive periods
      // so the lock-in's negative-frequency leakage term cancels exactly — the window edge
      // lands on a cycle boundary instead of wherever the next segment happened to start.
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

    // Reference statistics saved between rise and fall loops.
    // Mean anchors the threshold to a stable baseline; max/min measures the local noise
    // half-amplitude. Threshold = mean ± 2× noise_half_amplitude, so the detection point
    // stays just above the noise floor — close to the true transport delay — while a single
    // spike can no longer move the threshold the way raw max/min could.
    float quietMeanArr[3] = { 0.0f, 0.0f, 0.0f };
    float quietMaxArr[3]  = { 0.0f, 0.0f, 0.0f };
    float upMeanArr[3]    = { 0.0f, 0.0f, 0.0f };
    float upMinArr[3]     = { 0.0f, 0.0f, 0.0f };

    // ── Rise delays ─────────────────────────────────────────────────────
    for (int i = 0; i < 3; i++) {
      uint32_t t_quiet_start = phaseStartMs[quietIdx[i]];
      uint32_t t_up_start    = phaseStartMs[upIdx[i]];
      uint32_t t_up_end      = phaseStartMs[upEndIdx[i]];

      // Mean + max of the last 2 seconds of the quiet phase.
      // Mean = stable baseline; max - mean = noise half-amplitude.
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

      // Mean + min of the last 2 seconds of the UP phase.
      // Mean = stable settled high level; mean - min = noise half-amplitude (downward).
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

      // Rise threshold: quiet mean + 2× noise half-amplitude (floored at 0.5A).
      // This sits just above the noise ceiling while the mean anchor keeps it stable
      // across trials — matching the earliest reliable detection of field response.
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

      // Fall threshold: UP mean − 2× noise half-amplitude (floored at 0.5A).
      // Symmetric to the rise: sits just below the noise floor of the settled high level.
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
//
// Sequence: (1) RAMP — the normal AUTO path drives the REAL current PID to SystemIDStabilizeAmps
// (the level commissioned by the field-curve step) through the fieldCutCcActive branch: manually
// slewed setpoint at TEST_ENTRY_RATE_A, duty slew bypassed, protections live. This phase returns
// false — duty belongs to the real loop. (2) HOLD — duty FROZEN at the settled value for
// FIELDCUT_HOLD_MS; baseline + fast-channel high calibration point averaged over the tail of the
// hold. From here on the tick returns true (SystemID duty-override path). (3) CUT — duty steps to
// MinDuty, the exact floor a real OV clamp drives to, and stays there FIELDCUT_MEAS_MS. The decay
// lives in the 20 kSPS fast alt-current ring — grabbed FIELDCUT_SNAP_MS after the cut; ADS samples
// are recorded as a fallback, and the tail of the window gives the residual floor + low calibration
// point. (4) PROCESS — two-point calibrate the fast channel against the ADS, boxcar+zero-phase-EMA
// filter, log-linear fit of ln(I − floor) for τ, direct 90→10% fall time. Results publish HERE, so
// a late abort can't discard a finished measurement. (5) EASE back to the pre-test duty.
// One run per trigger; the wizard fires one run per Min%-floor speed (max/mid/idle) because the
// drain time IS RPM-dependent (bench 07-20), and fits drain-vs-RPM as a line across the runs.
// fieldCutRpm (this run's x-value) averages over the hold tail, BEFORE the cut — the cut corrupts
// the LM2907 tach signal (spike/false-zero/garbage ramp for ~4.6 s), so BOTH RPM gates are masked
// for the whole test + FIELDCUT_RPM_GRACE_MS tail (fieldCutRpmGrace in the tick builder).
// Field-at-MinDuty is the SAFE direction.

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

// Shared τ fit + fall-time on a filtered, floor-referenced decay trace (t in ms, amps). tCmd = the cut
// command instant in THIS trace's time base (NaN → unknown). Band: 85% down to 10% of (base − floor),
// starting at the first 85% crossing. Also fills the wizard plot arrays (t re-zeroed to tCmd — the cut
// command — so the plot origin matches the drain number). Returns true on a clean fit.
static bool fcFitDecay(const float *tMs, const float *amps, int n, int minFitPts, float tCmd) {
  float base = fieldCutBaseA, floorA = fieldCutFloorA;
  float dA = base - floorA;
  if (dA < FIELDCUT_MIN_BASE_A) {
    snprintf(fieldCutAbortMsg, sizeof(fieldCutAbortMsg), "output too low to measure — raise engine speed a little and re-run");
    return false;
  }
  const float lv95 = floorA + 0.95f * dA, lv90 = floorA + 0.90f * dA,
              lv85 = floorA + 0.85f * dA, lv50 = floorA + 0.50f * dA, lv10 = floorA + 0.10f * dA;
  // All crossings anchor around the 50% level: plateau rectifier ripple (±20% seen on the bench)
  // can dip through 85/90/95% and fake an early edge, but it can never reach 50%. The 85/90/95
  // crossings are the LAST ones before the 50% point (walk backward), the 10% crossing the first
  // one after it.
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
  // Drain + plot zero anchor to the cut COMMAND (tCmd): the OV release waits from when IT commands the
  // cut, so the command→response dead time belongs inside the number. Fall back to the 95% crossing
  // (then the 85% index) when tCmd is missing or out of range. Drain = anchor → 10%-of-baseline, read
  // directly off the trace — the model-free "field no longer makes meaningful current" number the OV
  // release needs. For a clean coil drain ≈ dead-time + 2.3τ, so the fit still cross-checks it.
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

  // ── Fast path: 20 kSPS ring copy → boxcar-16 decimate (0.8 ms) → zero-phase EMA (τ≈4 ms,
  //   forward+backward so the smoothing adds no lag bias to the fitted slope). The heavy filter
  //   kills the HF hash; the ~tens-of-Hz rectifier ripple stays and averages out across the
  //   hundreds of fit points (it is multiplicative → a zero-mean sinusoid in log space). ──
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
  // Calibration points are averaged over MATCHED multi-second windows on both sensors — four
  // spaced 500 ms ring grabs (~2 s of 20 kSPS data) against the ADS mean over the same span, all
  // in steady conditions (frozen duty / settled floor) — so ripple, noise, and any fixed pipeline
  // delay average out instead of landing in the calibration.
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
    entryDuty = (dutyOut > 1.0f) ? dutyOut : 1.0f;   // where the field sat pre-test — eased back to on exit
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

  // ── RAMP/SETTLE: the fieldCutCcActive branch slews the setpoint to fieldCutCmdA at
  //   TEST_ENTRY_RATE_A and the real PID drives duty (slew bypassed, protections live). Here we only
  //   WATCH: once the slewed setpoint has arrived and the measured current has sat inside the band
  //   for FIELDCUT_SETTLE_MS, capture the applied duty and freeze it. ──
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
      // Restore the pre-test setpoint BEFORE returning true: the SystemID resume snapshot fires on
      // this tick's rising edge (after this call) and must capture the pre-test value, not the 30 A
      // test target — otherwise test end would re-assert the test current.
      setpointLimited = fcPreSetpoint;
      queueConsoleMessageF("Field-cut: settled at %.1f A — freezing duty %.1f%%", ampsEma, freezeDuty);
      phase = 2; phaseStartMs = nowMs;
      fieldCutPhase = 1;
      dutyOut = freezeDuty;
      return true;
    }
    return false;
  }

  // ── HOLD: duty FROZEN at freezeDuty for FIELDCUT_HOLD_MS — a constant, settled operating point.
  //   Baseline = ADS average over the last 2.5 s; high calibration point = four spaced 500 ms ring
  //   grabs across the SAME 2.5 s (windows [2.0-2.5] [2.8-3.3] [3.6-4.1] [4.4-4.9] s). ──
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

  // ── CUT + TAIL: duty steps to MinDuty (the real OV-clamp floor) and stays FIELDCUT_MEAS_MS.
  //   Decay: fast ring grabbed FIELDCUT_SNAP_MS after the cut; ADS samples recorded for fallback.
  //   Tail: residual floor (ADS) + low calibration point (four spaced ring grabs) over the
  //   final 2.5 s — starts ~1.3 s after the drain has fully settled, pure floor. ──
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
//
// Reproduces each field-collapse waveform on demand to hunt an electrical fault (a suspected field
// cut). Same mutex family + duty-override contract as fieldCut_tick: returns TRUE only while it OWNS
// duty (via dutyOut); returns FALSE during the energize ramp (the protTestCcActive branch in
// AdjustField drives the real current PID to protTestCmdA) and during the mode-1/4 pre-gate cut +
// ladder recovery. NO capture — the normal CSV / cvlog / console telemetry records the transient.
// mode 1 instant cut : arms protTestCutPending → the pre-gate fires applyImmediateCut(fast-OV)
// mode 2 load-dump    : owns duty at MinDuty + dumps the inner PID integrator (fastest commanded collapse)
// mode 3 graceful     : owns duty, ramps down at DutyRampRate (the clean control case)
// mode 4 ladder walk  : the mode-1 cut repeated protTestReps× — each climbs the fast-OV lockout ladder
// Guards (AUTO / RPM / charging / no other test) live in the /protTestStart handler, not here.
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
    entryDuty = (dutyOut > 1.0f) ? dutyOut : 1.0f;
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
  // Only normal-path ticks reach this function: from the cut landing until the ladder lockout
  // expires, AdjustField takes the shutdown path (MODE_LOCKOUT_RAMP) and never calls protTest_tick,
  // so gpio4IsLow / fieldCollapseTime are NEVER observably true here. Detect the fire by the
  // pre-gate having CONSUMED protTestCutPending — the first tick that runs after that consumption
  // is by definition post-recovery.
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

  // ── MANUAL HARD-CUT HOLD (mode 5, phase 6): hold the field-enable line open for protTestCutMs
  //   against the field the user already set. protTestManualCutActive gates the enable HIGH in
  //   AdjustField; here we also command PWM 0 and own duty (return true). ──
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
// Quasi-static duty→amps ramp. Like systemID_tick it returns true while active and
// the caller must force GOV_BYPASS_SLEW + MANUAL PID and use dutyOut as the command.
// Ramps duty FIELDCURVE_DUTY_START → (table limit reached | FIELDCURVE_DUTY_MAX),
// dwelling FIELDCURVE_DWELL_MS per step and averaging amps over the settled (2nd) half
// of each dwell so the field L/R lag doesn't bias the curve. Post-ramp it locates the
// saturation knee and proposes SystemIDStabilizeAmps / SystemIDStepAmplitude placed in
// the linear region below the knee. Results are read by the dashboard via /fieldcurve.csv;
// the user clicks Apply (show-before-write) — the firmware does NOT auto-write the settings.
// Whole-run engine-speed stats for the last ramp (fed at the same settled samples as amps,
// so they describe the data actually captured). Advisory only — served, never gated on.
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
    // Warm-start (Min% onset only): commissioning sweeps run in DESCENDING RPM order, so each point's
    // onset duty is strictly HIGHER than the previous (lower RPM needs more field — the 1/RPM law).
    // Start the ramp one step below the most recent committed anchor's duty instead of from 5%, which
    // skips re-walking duties we already know are below onset. Safe by construction: the previous
    // (higher-RPM) onset is always below this point's onset, so we never start past the knee; the
    // one-step backoff keeps a pre-onset baseline so the detector still sees current "begin".
    if (fieldCurveOnsetMode && kneeAnchorN > 0) {
      float warm = kneeAnchorDuty[kneeAnchorN - 1] - FIELDCURVE_DUTY_STEP;
      if (warm > stepDuty) stepDuty = warm;
    }
    stepStartMs = nowMs;
    ampSum = 0.0;
    ampN = 0;
    rpmSum = 0.0;
    // Ease the field DOWN from the live charging duty to the sweep floor over FIELDCURVE_EASE_MS instead of
    // snapping (the ramp runs under GOV_BYPASS_SLEW, so an un-eased entry is an instant drop). Only when
    // coming down — a start at/below the floor (already rested) enters the sweep directly.
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
    // Over-voltage during the ramp is handled by the canonical fast-OV layer on the main
    // control path (REASON_FAST_OVERVOLTAGE: live bus > AlternatorHardShutdownV → immediate
    // field cut, fires before this override runs). That cut also flags fieldCurveAbortRequested,
    // so the ramp tears down cleanly. No bespoke per-target headroom abort is needed here.
    //
    // Effective ramp ceiling: Max Field % (MaxDuty) is the real per-bus field-duty cap and setDutyPercent
    // clamps the APPLIED duty down to it on a 24/36/48V bank. Cap the ramp at that same ceiling so we STOP
    // there instead of marching stepDuty up into a frozen-duty region that records flat/false amps and a
    // bogus curve. If the amp target isn't reached by then, we flag fieldCurveCeilingLimited so the
    // dashboard can tell the user to raise Max Field % and re-run for the full curve.
    // Taken through ccDutyCeiling() so a Max Field Volts cap stops the ramp too — it clamps applied duty
    // in setDutyPercent identically, so a raw MaxDuty read here would walk into that same frozen region.
    float effDutyCeil = FIELDCURVE_DUTY_MAX;
    if (ccDutyCeiling() < effDutyCeil) effDutyCeil = ccDutyCeiling();
    dutyOut = stepDuty;

    // Accumulate amps (and RPM, for the onset knee) only over the settled second half of the dwell.
    if ((nowMs - stepStartMs) >= (FIELDCURVE_DWELL_MS / 2)) {
      ampSum += ampsRaw;
      rpmSum += RPM;
      ampN++;
      fcRunRpmSample();
    }

    if ((nowMs - stepStartMs) >= FIELDCURVE_DWELL_MS) {
      float avgAmps = (ampN > 0) ? (float)(ampSum / ampN) : ampsRaw;
      float avgRpm  = (ampN > 0) ? (float)(rpmSum / ampN) : RPM;
      if (fieldCurveCount < FIELDCURVE_MAX_PTS) {
        fieldCurveBuf[fieldCurveCount++] = { stepDuty, avgAmps };
      }
      // ── Onset-stop mode (Min% knee) ─────────────────────────────────────────
      // First settled step = baseline (below the knee). Stop the instant a later step lifts
      // kneeOnsetA above it — that duty is the onset knee, and we've forced almost no current.
      // No onset by kneeMaxFloorPct+margin → record the ceiling, flag "no clean knee".
      if (fieldCurveOnsetMode) {
        if (fieldCurveCount == 1) {
          onsetBaseline = avgAmps;
        } else if ((avgAmps - onsetBaseline) >= kneeOnsetA) {
          // Coarse onset found (within one FIELDCURVE_DUTY_STEP). Refine by backing DOWN in fine
          // steps until amps fall back below onset — the lowest duty still above onset is the knee.
          queueConsoleMessageF("Min%% knee: coarse onset %.2fA at %.1f%% duty @ %.0f RPM, refining down...",
                               avgAmps - onsetBaseline, stepDuty, avgRpm);
          // Pair the knee with the RPM/temp measured AT this step (co-temporal), not a late read —
          // the engine drifts hundreds of RPM across a sweep. The fine-down phase overwrites these
          // with each lower step that still shows onset, so the final pair is the knee-defining step.
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
            fieldCurveCeilingLimited = true;
            queueConsoleMessageF("Min%% knee: no onset below %.1f%% duty (Max Field %% limit) @ %.0f RPM — "
                                 "raise Max Field %% and re-run if onset is higher", stepDuty, avgRpm);
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
        // Stopped at the duty-limit ceiling (not the natural 92% backstop) without hitting the amp
        // target → the curve is truncated by the 24/36/48V field-duty limit. Flag it + tell the user.
        if (reachedCeil && !reachedLimit && effDutyCeil < FIELDCURVE_DUTY_MAX) {
          fieldCurveCeilingLimited = true;
          queueConsoleMessageF("Field curve: stopped at %.0f%% duty (Max Field %% limit) before reaching %.0fA — "
                               "raise Max Field %% and re-run for the full curve",
                               effDutyCeil, fieldCurveTargetLimitA);
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
        fieldCurvePropStepPct = constrain((duty75 - duty50) * 0.5f, 1.0f, 25.0f);
        fieldCurveOk = (fieldCurveCount >= 4 && duty75 > duty50 && amps50 > 1.0f);

        queueConsoleMessageF("Field curve: %d pts, knee %.1fA@%.1f%% — propose Stabilize=%.0fA Step=%.1f%% (%s)",
                             fieldCurveCount, fieldCurveKneeAmps, fieldCurveKneeDuty,
                             fieldCurvePropStabA, fieldCurvePropStepPct,
                             fieldCurveOk ? "ok" : "review");

        fieldCurveResultsReady = true;
        // Ease the field down over ~1.5 s instead of snapping from the ramp peak to the start duty —
        // a one-tick collapse trips a protection when the override releases. Keep fieldCurveActive
        // non-zero so the dashboard poll waits for the ease to finish.
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
      ampN = 0;
      rpmSum = 0.0;
    }
    return true;
  }

  // ── ONSET FINE-DOWN (refine the coarse knee, onset mode only) ───────────────
  // Back down in FIELDCURVE_ONSET_FINE_STEP steps from the coarse onset. Each settled step still
  // >= kneeOnsetA over baseline becomes the new lowest-above; the first step that falls below means
  // we've passed under the knee, so the lowest-above duty is the refined knee (rounded conservatively
  // high = field stays primed). Bounded: at most one coarse step's worth of fine steps.
  if (phase == 3) {
    dutyOut = stepDuty;
    if ((nowMs - stepStartMs) >= (FIELDCURVE_DWELL_MS / 2)) { ampSum += ampsRaw; rpmSum += RPM; ampN++; fcRunRpmSample(); }
    if ((nowMs - stepStartMs) >= FIELDCURVE_DWELL_MS) {
      float avgAmps = (ampN > 0) ? (float)(ampSum / ampN) : ampsRaw;
      float avgRpm  = (ampN > 0) ? (float)(rpmSum / ampN) : RPM;
      bool stillAbove = ((avgAmps - onsetBaseline) >= kneeOnsetA);
      bool atFloor = (stepDuty <= FIELDCURVE_DUTY_START + 0.001f);
      if (stillAbove) {
        // This step still produces onset current → it defines the knee so far. Capture its duty
        // AND the RPM/temp measured during this same dwell, so the pair is co-temporal at the knee.
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
  // Gentle ramp from the live operating duty down to the sweep floor before the first dwell begins.
  // The dwell clock (stepStartMs) is (re)started only when the ease finishes, so no settle window
  // opens until the field has arrived.
  if (phase == 4) {
    float frac = (float)(nowMs - fcEaseInStartMs) / FIELDCURVE_EASE_MS;
    if (frac >= 1.0f) {
      dutyOut = stepDuty;
      stepStartMs = nowMs;
      ampSum = 0.0; ampN = 0; rpmSum = 0.0;
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
// A continuous duty ramp, up and back down, for the held-speed half of the sensitivity experiment.
// Same contract as fieldCurve_tick: returns true while active, and the caller must force
// GOV_BYPASS_SLEW + MANUAL PID and use dutyOut as the command.
//
// Modelled on fieldCurve_tick because that already solves the hard parts — easing in from the live
// operating duty instead of snapping, easing out so a fast current collapse does not trip a
// protection, and tearing down cleanly when the canonical fast-OV layer cuts the field. What differs:
//   - a continuous ramp, not step-and-dwell (default 1 %/s);
//   - up THEN down in one run. At 1 %/s against a field time constant near 0.7 s the output lags
//     duty by roughly 0.7% of duty, and averaging the two directions cancels a symmetric lag
//     exactly. The reversal also decorrelates the two monotone confounders — speed sag under rising
//     field load, case temperature rising through the sweep — from the field axis;
//   - no knee detection and no proposed settings. It is a stimulus generator; the analysis is offline.
// Bounded by MaxDuty, the real per-bus field ceiling, exactly as the field curve is: ramping past it
// records flat, false amps. Over-voltage is not handled here — the fast-OV layer on the control path
// fires before this override runs and latches altSweepAbortRequested (6_functions.ino).
// Turn around SHORT of the current and voltage limits, never on them. The soft layers that normally
// keep the machine off its limits — Group 1/2 over-voltage, iExcess, load dump, the thermal derate, the whole min-select
// ceiling chain — are all inside the !sysIDRunning body and therefore bypassed for the entire sweep.
// Only the hard-cut layer is left, so arriving exactly at a limit with 1 %/s of field still building
// and a ~0.7 s field lag behind it is how a sweep ends in a protection cut, and a cut throws away the
// down leg the lag cancellation depends on. These margins buy back the overshoot the missing soft
// layers would have absorbed.
// Both margins are panel controls (altSweepMarginA / altSweepMarginV, defaults 10 A and 0.20 V),
// posted with the start request exactly like rate/from/to.
// No temperature margin, on the user's call. Case temperature moves on a thermal time constant, not
// a field one: it cannot overshoot the way current and bus volts do behind the ramp's ~0.7 s field
// lag, so there is nothing for a margin to buy back, and taking one would end sweeps early.
// A 10 A margin would swallow a small cap whole (25 A at idle leaves 15 A of usable span, and a 12 A
// cap would refuse to sweep at all), so the current margin also never takes more than a quarter of
// the limit it guards — which is a clamp on the user's number too, not just on the default.
static inline float altSweepAmpMargin(float limitA) { return fminf(altSweepMarginA, 0.25f * limitA); }

// Which control limit, if any, the machine is approaching right now — evaluated from live
// measurements against the CONFIGURED limits, deliberately not from ctrlLimiter: the sweep owns duty
// through the same override the field curve uses, so the normal min-select chain that publishes
// ctrlLimiter is bypassed for the whole run (and is forced to 0 while it runs). Each test therefore
// has to repeat the enable gates the min-select chain applies, or the sweep turns around on a limit
// the regulator itself would not have honoured.
static const char *altSweepLimitNow() {
  float v = getBatteryVoltage();
  if (ChargingVoltageTarget > 0.5f && v >= ChargingVoltageTarget - altSweepMarginV)
                                                                           return "charge-voltage target";
  // DVCC limits count only while actually following an external authority — dvccCvlV/dvccCclA hold
  // their last decoded values when the link drops or the feature is off, and a stale one would end
  // every sweep. "external" not "BMS": the publisher is as often a gateway (Cerbo GX) as a battery.
  bool dvccFollowing = (dvccEn == 1 && dvccState == 3);
  if (dvccFollowing && !isnan(dvccCvlV) && v >= dvccCvlV - altSweepMarginV)
                                                                           return "external voltage limit";
  float amps = isnan(MeasuredAmps) ? 0.0f : MeasuredAmps;
  // capLimitMode 1 = the cap table is watts, not amps (the min-select chain divides it by bus volts).
  float capA = (capLimitMode == 1 && v > 0.5f) ? (interpolateRPMTable(RPM, rpmCapPowerTable) / v)
                                               : interpolateRPMTable(RPM, rpmCapCurrentTable);
  capA = fminf(capA, (float)MaxTableValue);
  if (capA > 0.5f && amps >= capA - altSweepAmpMargin(capA))               return "current limit at this engine speed";
  if (dvccFollowing && !isnan(dvccCclA) && dvccCclA > 0.5f && amps >= dvccCclA - altSweepAmpMargin(dvccCclA))
                                                                           return "external current limit";
  // Same enable gate the battery-limit ceiling uses in the min-select chain (BattLimitEnable +
  // a configured INA228 shunt), so a disabled limit cannot cut the sweep short.
  if (BattLimitEnable && HAS_BATT_SHUNT && BatteryCurrentSource == 0 && BattCurrentLimitA > 0.5f
      && !isnan(Bcur) && Bcur >= BattCurrentLimitA - altSweepAmpMargin(BattCurrentLimitA))
                                                                           return "battery current limit";
  if (TemperatureLimitF > 0 && !isnan(TempToUse) && TempToUse >= TemperatureLimitF)
                                                                           return "alternator temperature limit";
  return nullptr;
}
#define ALTSWEEP_LIMIT_CONFIRM_MS 0u     // reverse on the FIRST sample at a limit. Was 250 ms (ripple
                                         // debounce), but the failure costs are asymmetric: a ripple-early
                                         // reversal just shortens the sweep (bit 7 marks it), while a late
                                         // one at a steep battery acceptance knee ends in a hard OV cut and
                                         // loses the down leg — measured 2026-08-27, both float-stage sweeps
                                         // cut ~1.5 s after turnaround with the bus running >=1.2 V/s, where
                                         // 250 ms alone cost 0.3 V of the 0.15 V margin

bool altSweep_tick(float &dutyOut, float ampsRaw, uint32_t nowMs) {
  static uint8_t  phase = 0;        // 0 idle, 1 ease-in, 2 ramp, 3 ease-out
  static float    fromDuty = 0.0f, toDuty = 0.0f, curDuty = 0.0f, ratePctS = 1.0f;
  static float    easeFromDuty = 0.0f;
  static uint32_t easeStartMs = 0, lastRampMs = 0, limSinceMs = 0;
  static bool     goingDown = false;

  if (phase != 0 && altSweepAbortRequested) {
    altSweepAbortRequested = false;
    // A teardown outside this function (protection cut, or the field leaving normal AUTO — both in
    // 6_functions.ino) has already cleared altSweepActive and said why on the console. Only the plain
    // Stop-sweep press arrives here with the run still marked live, so only that one announces.
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

    // The two field caps are the ONLY ceilings: Max Field % and Max Field Volts, whichever binds first
    // (ccDutyCeiling). Both are named on the panel and both are lookupable, which is what the earlier
    // "only MaxDuty" rule was protecting — unlike FIELDCURVE_DUTY_MAX, which the field curve stops at
    // as its own knee-search bound and which is deliberately still NOT carried here. Reaching the
    // ceiling is nothing new: governor_apply clamps every duty the regulator commands to it every tick.
    float dutyCeil = ccDutyCeiling();
    fromDuty = constrain(altSweepFromPct, 0.0f, dutyCeil);
    toDuty   = (altSweepToPct <= 0.0f) ? dutyCeil : constrain(altSweepToPct, 0.0f, dutyCeil);
    ratePctS = constrain(altSweepRatePctS, 0.1f, 10.0f);
    if (toDuty - fromDuty < 1.0f) {
      queueConsoleMessageF("Field sweep: refused — span %.1f%%→%.1f%% is under 1%% after clamping to the field ceiling %.1f%% (set by %s)",
                           fromDuty, toDuty, dutyCeil,
                           (dutyCeil < MaxDuty - 0.05f) ? "Max Field Volts" : "Max Field %");
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
    // Turn around at the first control limit the machine reaches — voltage target, current cap at
    // this speed, battery or BMS limit, temperature — instead of driving into it. Past a limit the
    // rows stop measuring what the sweep is for (output as a function of field) and start measuring
    // whatever is holding the machine back, and at a high state of charge driving on ends in a
    // protection cut, which throws away the down leg the lag cancellation needs. The down leg runs
    // at the SAME rate, so the pair stays symmetric and still averages the field lag out.
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
// Emits a sine setpoint reference into *out (centred on baseA, amplitude ampA). For
// waveform==2 (auto-sweep) it also runs an online single-bin lock-in of the MEASURED
// current against the reference, per log-spaced frequency, filling tuningBode[]. The
// phase accumulator is owned by the caller (passed by ref) so it persists across ticks.
// Called every control tick from the TuningMode block while a sine waveform is selected.
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
        // Post-sweep: ease the setpoint all the way down to a low rest current over ~2 s (easing only to
        // the wave floor left the floor→release drop abrupt). A still-high current when TuningMode releases
        // reads as an over-current once protections re-arm. tuningSweepDone stays false until the ease ends.
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
    // High rail is the live ceiling (MaxDuty ~50%@24V / ~25%@48V) — a fixed 99.5 could never
    // fire there and a clipped sweep would be accepted as a valid plant fit.
    if (dutyCycle <= MinDuty + 0.5f || dutyCycle >= ccDutyCeiling() - 0.5f) tuningSweepDutyRailed = true;
    // Integer-cycle window: settle 1 cycle, then accumulate over a WHOLE number of drive
    // periods so the lock-in's negative-frequency leakage term cancels exactly (and the
    // reference sin/cos sums go to ~0, keeping the DC correction clean). The raw hold
    // (segMs, floored at 1.5 s for fast tones) is truncated down to whole cycles.
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
        // Only in-band points (within 6 dB of the strongest response) feed worst-coherence.
        // Past roll-off the current barely moves while the drive keeps swinging, so a low
        // coherence there is dead signal, not a bad run — and that is exactly where slow
        // alternators sit, so folding those points in would false-fail otherwise-clean sweeps.
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
  if (result.data) {
    f.read(result.data, result.size);
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
// CSV emitter for a watermark. A sensor absent for the whole ignition cycle leaves the pair NAN;
// a 0 there was rendered as a measured low/high (0 F, 0 kt, 0 deg heel). ROLL_EMPTY -> "—".
inline int wmIgnCsv(float v, int scale) {
  if (!isfinite(v)) return ROLL_EMPTY;
  float x = v * (float)scale;   // lroundf on a non-finite or out-of-range float is undefined
  if (x >  2000000000.0f) return  2000000000;
  if (x < -1000000000.0f) return -1000000000;   // stays clear of ROLL_EMPTY, which must stay unreachable
  return (int)lroundf(x);
}
