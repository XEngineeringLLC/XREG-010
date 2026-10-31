// ── 8_functions.ino ── Fast alternator-current failure detector (consumer 2) algorithm bodies,
// Config Sharing manifest, Battery Health monitor (DCIR + capacity), /debug/fillmax endpoints.

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

// Tuning constants (rect_fault_detector.py TUNING — rationale comments live there).
// hump_merge_frac is not listed: the picker rungs below carry their own (window, frac).
#define FAD_SMOOTH_N 3
#define FAD_ROUGH_MS 4.0f
#define FAD_REGIME_CYCLE_MS 2.5f
#define FAD_DUPE_IV_FRAC 0.35f
#define FAD_DUPE_FRAC_MAX 0.08f
#define FAD_HEIGHT_REF_FRAC 0.55f
#define FAD_WEAK_KEEP_FRAC 0.4f
#define FAD_ENV_CYCLES 2.0f
#define FAD_BLOCK_CRESTS 48
#define FAD_F_SIG 2.5f
#define FAD_TH_INT 0.15f
#define FAD_TH_H 0.70f
#define FAD_TREND_INT 0.08f
#define FAD_TREND_H 0.45f
#define FAD_ACF_CONFIRM 1.25f
#define FAD_WINNER_PREF 0.7f
#define FAD_GAP_THR 1.45f
#define FAD_GAP_REF_PCT 25.0f
#define FAD_MIN_GAPS 12
#define FAD_SEG_FRAC_MIN 0.35f
#define FAD_MIN_MODAL_SEGS 20
#define FAD_SYNC_PARTNER_MIN 0.5f

#define FAD_ACF_LAG_CAP 400  // regime/sync ACF lag ceiling
#define FAD_SEG_SAMPLES 8192 // reduction chunk (float partial folded into double): keeps the accumulation order the verdict thresholds were tuned against

// ---- small numeric helpers ----

// k-th smallest (Hoare quickselect, median-of-3 pivot). Leaves a[] partitioned around a[k];
// fadPercentile relies on that to fetch the next order statistic with a linear scan.
static float fadSelect(float *a, int n, int k) {
  int lo = 0, hi = n - 1;
  while (lo < hi) {
    int mid = lo + (hi - lo) / 2;
    float p0 = a[lo], p1 = a[mid], p2 = a[hi];
    float piv = (p0 < p1) ? ((p1 < p2) ? p1 : (p0 < p2 ? p2 : p0))
                          : ((p0 < p2) ? p0 : ((p1 < p2) ? p2 : p1));
    int i = lo, j = hi;
    while (i <= j) {
      while (a[i] < piv) i++;
      while (a[j] > piv) j--;
      if (i <= j) {
        float t = a[i]; a[i] = a[j]; a[j] = t;
        i++; j--;
      }
    }
    if (k <= j) hi = j;
    else if (k >= i) lo = i;
    else return a[k];
  }
  return a[lo];
}

// numpy 'linear' percentile; MUTATES a
static float fadPercentile(float *a, int n, float q) {
  if (n <= 0) return 0.0f;
  if (n == 1) return a[0];
  double pos = (double)(n - 1) * (double)q / 100.0;
  int i = (int)pos;
  if (i >= n - 1) return fadSelect(a, n, n - 1);
  double frac = pos - (double)i;
  float v1 = fadSelect(a, n, i);
  float v2 = a[i + 1];
  for (int t = i + 2; t < n; t++)
    if (a[t] < v2) v2 = a[t];
  if (frac <= 0.0) return v1;
  return (float)((double)v1 + ((double)v2 - (double)v1) * frac);
}
static inline float fadMedian(float *a, int n) { return fadPercentile(a, n, 50.0f); }

// One-way ANOVA by index mod k → (F, bias-corrected RMS effect size).
static void fadAnova(const float *vals, int n, int k, float *Fout, float *effOut) {
  if (n < 2 * k) { *Fout = 0.0f; *effOut = 0.0f; return; }
  double sum[8] = { 0 }, cnt[8] = { 0 };
  for (int i = 0; i < n; i++) {
    sum[i % k] += vals[i];
    cnt[i % k] += 1.0;
  }
  double means[8], grand = 0.0;
  for (int g = 0; g < k; g++) {
    means[g] = sum[g] / cnt[g];
    grand += sum[g];
  }
  grand /= (double)n;
  double ssw = 0.0;
  for (int i = 0; i < n; i++) {
    double d = (double)vals[i] - means[i % k];
    ssw += d * d;
  }
  double msw = ssw / (double)(n - k);
  double ssb = 0.0;
  for (int g = 0; g < k; g++) {
    double d = means[g] - grand;
    ssb += cnt[g] * d * d;
  }
  double msb = ssb / (double)(k - 1);
  double e = (msb - msw) / ((double)n / (double)k);
  *effOut = (float)((e > 0.0) ? sqrt(e) : 0.0);
  *Fout = (float)(msb / ((msw > 1e-12) ? msw : 1e-12));
}

static void fadParabolic(const float *y, int i, float *t, float *h) {
  float den = y[i - 1] - 2.0f * y[i] + y[i + 1];
  if (den < 0.0f) {
    float dt = 0.5f * (y[i - 1] - y[i + 1]) / den;
    if (dt > 0.5f) dt = 0.5f;
    if (dt < -0.5f) dt = -0.5f;
    *t = (float)i + dt;
    *h = y[i] - 0.25f * (y[i - 1] - y[i + 1]) * dt;
  } else {
    *t = (float)i;
    *h = y[i];
  }
}

// centered moving average from the prefix array, numpy edge-shrink semantics
static inline float fadMov(const double *pre, int n, int win, int i) {
  int h = win / 2;
  int lo = i - h;
  if (lo < 0) lo = 0;
  int hi = i + (win - h);
  if (hi > n) hi = n;
  return (float)((pre[hi] - pre[lo]) / (double)(hi - lo));
}

// smallest k with D[k] ≥ 0.7·max, runner = best of the others (floored)
static void fadWinner(const float *D, int *kOut, float *dOut, float *runOut) {
  float dmax = D[2];
  for (int k = 3; k <= 8; k++)
    if (D[k] > dmax) dmax = D[k];
  int kw = 2;
  for (int k = 2; k <= 8; k++)
    if (D[k] >= FAD_WINNER_PREF * dmax) { kw = k; break; }
  float runner = dmax * 1e-3f;
  if (runner < 1e-9f) runner = 1e-9f;
  for (int k = 2; k <= 8; k++)
    if (k != kw && D[k] > runner) runner = D[k];
  *kOut = kw;
  *dOut = D[kw];
  *runOut = runner;
}

// Lay the workspace out inside one externally allocated block (PSRAM on target, malloc on
// the desktop harness). mem == NULL → just return the byte count needed for window n.
static size_t fadCarve(FadJob *J,
                       uint8_t *mem, int n) {
  int crestCap = n / 3 + 16;
  int blocksCap = crestCap / FAD_BLOCK_CRESTS + 2;
  int rowsCap = 32768;
  size_t off = 0;
#define FAD_TAKE(ptr, type, count) \
  do { \
    off = (off + 7u) & ~(size_t)7u; \
    if (mem) J->ptr = (type *)(mem + off); \
    off += sizeof(type) * (size_t)(count); \
  } while (0)
  FAD_TAKE(pre, double, n + 1);
  FAD_TAKE(xf, float, n);
  FAD_TAKE(s, float, n);
  FAD_TAKE(rr, float, n);
  FAD_TAKE(env, float, n);
  FAD_TAKE(acf, float, FAD_ACF_LAG_CAP);
  FAD_TAKE(idx, int32_t, crestCap);
  FAD_TAKE(kept, int32_t, crestCap);
  FAD_TAKE(gpos, int32_t, crestCap);
  FAD_TAKE(instA, int32_t, crestCap);
  FAD_TAKE(instB, int32_t, crestCap);
  FAD_TAKE(cnts, int32_t, crestCap);
  FAD_TAKE(tt, float, crestCap);
  FAD_TAKE(hh, float, crestCap);
  FAD_TAKE(hn, float, crestCap);
  FAD_TAKE(iv, float, crestCap);
  FAD_TAKE(dn, float, crestCap);
  FAD_TAKE(scratch, float, crestCap);
  FAD_TAKE(rowsH, float, rowsCap);
  FAD_TAKE(rowsI, float, rowsCap);
  FAD_TAKE(blkStats, float, 4 * 7 * blocksCap);
#undef FAD_TAKE
  if (mem) {
    J->n = n;
    J->crestCap = crestCap;
    J->blocksCap = blocksCap;
    J->rowsCap = rowsCap;
  }
  return off;
}

// Arm a job. src == NULL means the caller pre-filled J->xf (desktop harness path).
static void fadStart(FadJob *J,
                     const int16_t *src, int n, float fs) {
  J->src = src;
  J->n = n;
  J->fs = fs;
  J->haveBest = 0;
  J->bestSane = 0;
  J->bestHeadroom = 0.0f;
  memset(&J->best, 0, sizeof(J->best));
  J->idxOverflow = 0;
}

// Straight-line detector; runs to completion on the Core-0 worker. Do not flatten the chunked reductions
// (FAD_SEG_SAMPLES): the verdict thresholds were tuned against their exact results. See Fault_Detector_Dev_Summary.md.

// Crest pick: maxima -> local-contrast threshold -> valley-depth merge. -1 = too few maxima.
static int fadPick(FadJob *J, float wMs, float frac) {
  const float *r = J->rr;
  int n = J->n;
  J->nIdx = 0;
  for (int i = 1; i <= n - 2; i++)
    if (r[i] > r[i - 1] && r[i] >= r[i + 1]) {
      if (J->nIdx >= J->crestCap) { J->idxOverflow = 1; return -1; }
      J->idx[J->nIdx++] = i;
    }
  if (J->nIdx < 32) return -1;
  // per-window pk-pk into scratch (numpy reshape semantics)
  int w = (int)llrintf(wMs * 1e-3f * J->fs);
  if (w < 8) w = 8;
  int nw = n / w;
  for (int b = 0; b < nw; b++) {
    const float *seg = r + b * w;
    float mn = seg[0], mx = seg[0];
    for (int q = 1; q < w; q++) { if (seg[q] < mn) mn = seg[q]; if (seg[q] > mx) mx = seg[q]; }
    J->scratch[b] = mx - mn;
  }
  float mergeDelta = frac * fadPercentile(J->scratch, nw, 25.0f);
  // valley-depth merge; vmin/scanFrom maintain an incremental min over [kept[last], i]
  J->nKept = 1;
  J->kept[0] = J->idx[0];
  float vmin = r[J->idx[0]];
  int scanFrom = J->idx[0] + 1;
  for (int p = 1; p < J->nIdx; p++) {
    int i = J->idx[p];
    int j = J->kept[J->nKept - 1];
    for (int q = scanFrom; q <= i; q++) if (r[q] < vmin) vmin = r[q];
    scanFrom = i + 1;
    float lower = (r[j] < r[i]) ? r[j] : r[i];
    if (vmin > lower - mergeDelta) {
      if (r[i] > r[j]) { J->kept[J->nKept - 1] = i; vmin = r[i]; }  // merged: keep the taller
    } else {
      J->kept[J->nKept++] = i; vmin = r[i];  // real valley: new crest
    }
  }
  int niv = J->nKept - 1;
  if (niv < 16) return -1;
  for (int q = 0; q < niv; q++) J->iv[q] = (float)(J->kept[q + 1] - J->kept[q]);
  memcpy(J->scratch, J->iv, sizeof(float) * (size_t)niv);
  float p90 = fadPercentile(J->scratch, niv, 90.0f);
  int dup = 0;
  for (int q = 0; q < niv; q++) if (J->iv[q] < FAD_DUPE_IV_FRAC * p90) dup++;
  J->pickSane = ((float)dup / (float)niv <= FAD_DUPE_FRAC_MAX) ? 1 : 0;
  return 1;
}

// One rung: pick -> weak-filter -> subtrain -> features -> block ANOVA -> sync -> confirm ->
// verdict. Keeps the best rung; a pick fail / too-short train skips it. Updates J->best.
static void fadRung(FadJob *J, float wMs, float frac) {
  const float *r = J->rr;
  int n = J->n;
  if (fadPick(J, wMs, frac) < 0) return;
  J->sane2 = J->pickSane;

  // tall-height reference over ALL raw maxima
  for (int q = 0; q < J->nIdx; q++) J->scratch[q] = r[J->idx[q]];
  float tallThr = FAD_HEIGHT_REF_FRAC * fadPercentile(J->scratch, J->nIdx, 90.0f);
  // weak floor: median of kept crests at/above the tall threshold (fallback: all kept)
  int wkNt = 0;
  for (int q = 0; q < J->nKept; q++) { float kh = r[J->kept[q]]; if (kh >= tallThr) J->scratch[wkNt++] = kh; }
  if (wkNt > 0) {
    J->medTall = fadMedian(J->scratch, wkNt);
  } else {
    for (int q = 0; q < J->nKept; q++) J->scratch[q] = r[J->kept[q]];
    J->medTall = fadMedian(J->scratch, J->nKept);
  }
  // drop crests below the weak-keep floor (in-place forward compaction)
  float floorv = FAD_WEAK_KEEP_FRAC * J->medTall;
  int wkNk = 0;
  for (int q = 0; q < J->nKept; q++) if (r[J->kept[q]] >= floorv) J->kept[wkNk++] = J->kept[q];
  J->nKept = wkNk;
  if (J->nKept < 2 * FAD_BLOCK_CRESTS) return;

  // subtrain: raw maxima not kept, nearest-kept distance via two sorted pointers
  J->subtrain = 0.0f;
  {
    int nSup = J->nIdx - J->nKept;
    if (nSup > 0 && (double)nSup * (double)J->nKept < 4e7) {
      int subCnt = 0, subKp = 0, subQ = 0;
      float hFloor = 0.3f * J->medTall, dFloor = 0.3f * J->P;
      for (int p = 0; p < J->nIdx; p++) {
        int e = J->idx[p];
        if (subQ < J->nKept && J->kept[subQ] == e) { subQ++; continue; }  // member of kept
        while (subKp + 1 < J->nKept && J->kept[subKp + 1] <= e) subKp++;
        int d1 = e - J->kept[subKp]; if (d1 < 0) d1 = -d1;
        int d2 = (subKp + 1 < J->nKept) ? J->kept[subKp + 1] - e : d1; if (d2 < 0) d2 = -d2;
        int dNear = (d1 < d2) ? d1 : d2;
        if (r[e] >= hFloor && (float)dNear >= dFloor) subCnt++;
      }
      J->subtrain = (float)subCnt / (float)J->nKept;
    }
  }

  // parabolic features (tt, hh) -> hn, iv, dn
  for (int q = 0; q < J->nKept; q++) {
    int i = J->kept[q];
    if (i >= 1 && i <= n - 2) fadParabolic(r, i, &J->tt[q], &J->hh[q]);
    else { J->tt[q] = (float)i; J->hh[q] = r[i]; }
    J->hn[q] = J->hh[q] / J->env[i];
  }
  memcpy(J->scratch, J->hn, sizeof(float) * (size_t)J->nKept);
  J->scaleH = fadMedian(J->scratch, J->nKept);
  int niv = J->nKept - 1;
  for (int q = 0; q < niv; q++) J->iv[q] = J->tt[q + 1] - J->tt[q];
  memcpy(J->scratch, J->iv, sizeof(float) * (size_t)niv);
  J->medIv = fadMedian(J->scratch, niv);
  for (int q = 0; q < niv; q++) J->dn[q] = J->iv[q] / J->medIv;

  // modulo-k ANOVA per 48-crest block, medians over blocks
  int nBlkH = J->nKept / FAD_BLOCK_CRESTS;
  int nBlkI = (J->nKept - 1) / FAD_BLOCK_CRESTS;
  if (nBlkH > J->blocksCap) nBlkH = J->blocksCap;
  if (nBlkI > J->blocksCap) nBlkI = J->blocksCap;
  int nBlkMax = (nBlkH > nBlkI) ? nBlkH : nBlkI;
  float sh = (J->scaleH > 1e-9f) ? J->scaleH : 1e-9f;
  int plane = J->blocksCap;
  for (int b = 0; b < nBlkMax; b++)
    for (int k = 2; k <= 8; k++) {
      if (b < nBlkH) {
        float F, e;
        fadAnova(J->hn + b * FAD_BLOCK_CRESTS, FAD_BLOCK_CRESTS, k, &F, &e);
        J->blkStats[((0 * 7) + (k - 2)) * plane + b] = e / sh;
        J->blkStats[((1 * 7) + (k - 2)) * plane + b] = F;
      }
      if (b < nBlkI) {
        float F, e;
        fadAnova(J->dn + b * FAD_BLOCK_CRESTS, FAD_BLOCK_CRESTS, k, &F, &e);
        J->blkStats[((2 * 7) + (k - 2)) * plane + b] = e;
        J->blkStats[((3 * 7) + (k - 2)) * plane + b] = F;
      }
    }
  for (int k = 2; k <= 8; k++) {
    memcpy(J->scratch, J->blkStats + ((0 * 7) + (k - 2)) * plane, sizeof(float) * (size_t)nBlkH);
    J->Dh[k] = fadMedian(J->scratch, nBlkH);
    memcpy(J->scratch, J->blkStats + ((1 * 7) + (k - 2)) * plane, sizeof(float) * (size_t)nBlkH);
    J->Fh[k] = fadMedian(J->scratch, nBlkH);
    memcpy(J->scratch, J->blkStats + ((2 * 7) + (k - 2)) * plane, sizeof(float) * (size_t)nBlkI);
    J->Di[k] = fadMedian(J->scratch, nBlkI);
    memcpy(J->scratch, J->blkStats + ((3 * 7) + (k - 2)) * plane, sizeof(float) * (size_t)nBlkI);
    J->Fi[k] = fadMedian(J->scratch, nBlkI);
  }
  fadWinner(J->Dh, &J->kH, &J->dH, &J->runH);
  fadWinner(J->Di, &J->kI, &J->dI, &J->runI);
  J->syncUsed = 0;

  // gap-anchored sync: any precondition fail -> skip to CONFIRM with syncUsed=0
  float T0 = 0.0f;
  do {
    int nivg = J->nKept - 1;
    memcpy(J->scratch, J->iv, sizeof(float) * (size_t)nivg);
    J->refIv = fadPercentile(J->scratch, nivg, FAD_GAP_REF_PCT);
    J->nGap = 0;
    float thr = FAD_GAP_THR * J->refIv;
    for (int q = 0; q < nivg; q++) if (J->iv[q] > thr) J->gpos[J->nGap++] = q;
    J->lagLo = (int)(1.6f * J->refIv); if (J->lagLo < 4) J->lagLo = 4;
    J->lagHi = n / 4; if (J->lagHi > FAD_ACF_LAG_CAP) J->lagHi = FAD_ACF_LAG_CAP;
    if (J->nGap < FAD_MIN_GAPS || J->lagLo > J->lagHi) break;
    // sync ACF over lags lagLo..lagHi of rn (= J->s)
    const float *rn = J->s;
    float m = (float)J->rnMean;
    if (J->rnVar <= 0.0) {
      for (int l = J->lagLo; l <= J->lagHi; l++) J->acf[l - J->lagLo] = 0.0f;
    } else {
      for (int lag = J->lagLo; lag <= J->lagHi; lag++) {
        int len = n - lag; double acc = 0.0;
        for (int seg = 0; seg < len; seg += FAD_SEG_SAMPLES) {
          int end = seg + FAD_SEG_SAMPLES; if (end > len) end = len;
          float part = 0.0f; const float *a = rn + seg, *b = rn + seg + lag; int cnt = end - seg;
          for (int i = 0; i < cnt; i++) part += (a[i] - m) * (b[i] - m);
          acc += (double)part;
        }
        J->acf[lag - J->lagLo] = (float)(acc / (double)len / J->rnVar);
      }
    }
    // sync ACF peak -> matched period T0
    int mm0 = J->lagHi - J->lagLo + 1;
    int bestP = -1;
    for (int p = 1; p < mm0 - 1; p++)
      if (J->acf[p] > J->acf[p - 1] && J->acf[p] >= J->acf[p + 1])
        if (bestP < 0 || J->acf[p] > J->acf[bestP]) bestP = p;
    if (bestP < 0 || J->acf[bestP] < 0.5f) break;
    T0 = (float)(J->lagLo + bestP);
    // match each gap to its one-period-later partner (+/-10%)
    int nInst = 0, mtOkc = 0;
    for (int j = 0; j < J->nGap; j++) {
      float target = J->tt[J->gpos[j]] + T0;
      int lo = 0, hi = J->nGap;
      while (lo < hi) { int mid = (lo + hi) / 2; if (J->tt[J->gpos[mid]] < target) lo = mid + 1; else hi = mid; }
      int bestC = -1; float bd = T0;
      for (int t = 0; t < 2; t++) {
        int c = lo - 1 + t;
        if (c >= 0 && c < J->nGap && c > j) { float d = fabsf(J->tt[J->gpos[c]] - target); if (d < bd) { bestC = c; bd = d; } }
      }
      if (bestC >= 0 && bd <= 0.10f * T0) { mtOkc++; J->instA[nInst] = j; J->instB[nInst] = bestC; nInst++; }
    }
    if (!((float)mtOkc / (float)J->nGap >= FAD_SYNC_PARTNER_MIN && nInst >= FAD_MIN_MODAL_SEGS)) break;
    // modal segment count via counting sort (reuses J->idx as histogram)
    for (int i = 0; i < nInst; i++) J->cnts[i] = J->gpos[J->instB[i]] - J->gpos[J->instA[i]];
    int vmn = J->cnts[0], vmx = J->cnts[0];
    for (int i = 1; i < nInst; i++) { if (J->cnts[i] < vmn) vmn = J->cnts[i]; if (J->cnts[i] > vmx) vmx = J->cnts[i]; }
    int R = vmx - vmn + 1;
    int32_t *hist = J->idx;
    for (int k = 0; k < R; k++) hist[k] = 0;
    for (int i = 0; i < nInst; i++) hist[J->cnts[i] - vmn]++;
    int mMod = 0, cMax = 0;
    for (int k = 0; k < R; k++) if (hist[k] > cMax) { cMax = hist[k]; mMod = vmn + k; }
    float frc = (float)cMax / (float)nInst;
    if (!(mMod >= 2 && mMod <= 8 && frc >= FAD_SEG_FRAC_MIN)) break;
    int mm = mMod;
    // phase-aligned modal-segment rows -> position-in-period ANOVA
    int nRows = 0;
    for (int i = 0; i < nInst; i++) {
      if (J->cnts[i] != mm) continue;
      int g = J->gpos[J->instA[i]];
      if (g + mm + 1 > nivg) continue;
      if ((nRows + 1) * mm > J->rowsCap) break;
      float hrow[8], irow[8], key[8];
      for (int q = 0; q < mm; q++) {
        hrow[q] = J->hn[g + 1 + q];
        irow[q] = J->iv[g + 1 + q] / J->refIv;
        key[q] = rintf(irow[q] * 2.0f) / 2.0f;
      }
      int rot = 0;  // lexicographically maximal rotation, first wins ties
      for (int s0 = 1; s0 < mm; s0++)
        for (int q = 0; q < mm; q++) {
          float a = key[(s0 + q) % mm], b = key[(rot + q) % mm];
          if (a > b) { rot = s0; break; } if (a < b) break;
        }
      for (int q = 0; q < mm; q++) { J->rowsH[nRows * mm + q] = hrow[(q + rot) % mm]; J->rowsI[nRows * mm + q] = irow[(q + rot) % mm]; }
      nRows++;
    }
    if (nRows >= FAD_MIN_MODAL_SEGS) {
      float fsh, esh, fsi, esi;
      fadAnova(J->rowsH, nRows * mm, mm, &fsh, &esh);
      fadAnova(J->rowsI, nRows * mm, mm, &fsi, &esi);
      float shs = (J->scaleH > 1e-9f) ? J->scaleH : 1e-9f;
      float dSyncH = esh / shs, dSyncI = esi;
      int kSync = (int)llrint((double)T0 / (double)J->refIv);
      if (kSync < 2) kSync = 2; if (kSync > 8) kSync = 8;
      if (dSyncI > J->dI) { J->runI = (J->dI > dSyncI * 1e-3f) ? J->dI : dSyncI * 1e-3f; J->kI = kSync; J->dI = dSyncI; J->Fi[kSync] = fsi; J->syncUsed = 1; }
      if (dSyncH > J->dH) { J->runH = (J->dH > dSyncH * 1e-3f) ? J->dH : dSyncH * 1e-3f; J->kH = kSync; J->dH = dSyncH; J->Fh[kSync] = fsh; J->syncUsed = 1; }
      if (J->syncUsed) J->Tsync = T0;
    }
  } while (0);

  // confirm: T_win from the primary feature pick, then ACF spans aP (pitch) and aT (period)
  float Twin;
  {
    uint8_t trigH = (J->dH >= FAD_TH_H && J->Fh[J->kH] >= FAD_F_SIG) ? 1 : 0;
    uint8_t trigI = (J->dI >= FAD_TH_INT && J->Fi[J->kI] >= FAD_F_SIG) ? 1 : 0;
    if (J->syncUsed) {
      Twin = J->Tsync;
    } else {
      uint8_t featI = (trigI || (!trigH && J->dI / FAD_TH_INT >= J->dH / FAD_TH_H)) ? 1 : 0;
      int kWin = featI ? J->kI : J->kH;
      double sum = 0.0;
      for (int q = 0; q < niv; q++) sum += (double)J->iv[q];
      Twin = (float)kWin * (float)(sum / (double)niv);
    }
  }
  float aP = -1e30f, aT = -1e30f;
  {
    const float *rn = J->s;
    float m = (float)J->rnMean;
    int pLo = (int)(0.75f * J->P); if (pLo < 2) pLo = 2;
    int pHi = (int)(1.3f * J->P);
    int tLo = (int)(0.85f * Twin); if (tLo < 2) tLo = 2;
    int tHi = (int)(1.15f * Twin) + 1;
    if (pHi > n - 2) pHi = n - 2;
    if (tHi > n - 2) tHi = n - 2;
    if (J->rnVar > 0.0) {
      for (int lag = pLo; lag <= pHi; lag++) {
        int len = n - lag; double acc = 0.0;
        for (int seg = 0; seg < len; seg += FAD_SEG_SAMPLES) {
          int end = seg + FAD_SEG_SAMPLES; if (end > len) end = len;
          float part = 0.0f; const float *a = rn + seg, *b = rn + seg + lag; int cnt = end - seg;
          for (int i = 0; i < cnt; i++) part += (a[i] - m) * (b[i] - m);
          acc += (double)part;
        }
        float v = (float)(acc / (double)len / J->rnVar);
        if (v > aP) aP = v;
      }
      for (int lag = tLo; lag <= tHi; lag++) {
        int len = n - lag; double acc = 0.0;
        for (int seg = 0; seg < len; seg += FAD_SEG_SAMPLES) {
          int end = seg + FAD_SEG_SAMPLES; if (end > len) end = len;
          float part = 0.0f; const float *a = rn + seg, *b = rn + seg + lag; int cnt = end - seg;
          for (int i = 0; i < cnt; i++) part += (a[i] - m) * (b[i] - m);
          acc += (double)part;
        }
        float v = (float)(acc / (double)len / J->rnVar);
        if (v > aT) aT = v;
      }
    }
    if (aP < -1e29f) aP = 0.0f;
    if (aT < -1e29f) aT = 0.0f;
  }

  // verdict + keep best
  uint8_t trigH = (J->dH >= FAD_TH_H && J->Fh[J->kH] >= FAD_F_SIG) ? 1 : 0;
  uint8_t trigI = (J->dI >= FAD_TH_INT && J->Fi[J->kI] >= FAD_F_SIG) ? 1 : 0;
  uint8_t featI = (trigI || (!trigH && J->dI / FAD_TH_INT >= J->dH / FAD_TH_H)) ? 1 : 0;
  int kWin = featI ? J->kI : J->kH;
  float dWin = featI ? J->dI : J->dH;
  float runner = featI ? J->runI : J->runH;
  float thr = featI ? FAD_TH_INT : FAD_TH_H;
  float aPd = (aP > 0.05f) ? aP : 0.05f;
  float acfRatio = aT / aPd;
  uint8_t confirmed = (J->syncUsed || acfRatio >= FAD_ACF_CONFIRM) ? 1 : 0;
  uint8_t verdict;
  if ((trigH || trigI) && confirmed) verdict = FADV_FAULT;
  else if (J->dI >= FAD_TREND_INT || J->dH >= FAD_TREND_H) verdict = FADV_TREND;
  else verdict = FADV_HEALTHY;
  float hr1 = J->dI / FAD_TH_INT, hr2 = J->dH / FAD_TH_H;
  float headroom = (hr1 > hr2) ? hr1 : hr2;
  uint8_t better = !J->haveBest || (J->sane2 > J->bestSane) || (J->sane2 == J->bestSane && headroom > J->bestHeadroom);
  if (better) {
    J->haveBest = 1; J->bestSane = J->sane2; J->bestHeadroom = headroom;
    FadResult *Rr = &J->best;
    Rr->verdict = verdict; Rr->winningK = (uint8_t)kWin; Rr->featInterval = featI; Rr->sync = J->syncUsed;
    Rr->score = dWin; Rr->marginThr = dWin / thr; Rr->marginRunner = dWin / ((runner > 1e-12f) ? runner : 1e-12f);
    Rr->Dheight = J->dH; Rr->Dinterval = J->dI; Rr->Fwin = featI ? J->Fi[J->kI] : J->Fh[J->kH];
    Rr->acfRatio = acfRatio; Rr->periodRatio = Twin / J->P; Rr->pitch = J->P; Rr->subtrain = J->subtrain; Rr->nCrests = J->nKept;
  }
}

// Whole-analysis driver; deadline is ignored (runs to completion).
static int fadStep(FadJob *J, int64_t deadline, FadResult *out) {
  (void)deadline;
  int n = J->n;
  if (J->src) for (int i = 0; i < n; i++) J->xf[i] = (float)J->src[i];
  // s = boxcar-3 of xf (numpy clipped-window semantics)
  {
    const float *x = J->xf;
    for (int i = 0; i < n; i++) {
      int lo = i - 1; if (lo < 0) lo = 0;
      int hi = i + 2; if (hi > n) hi = n;
      float acc = 0.0f; for (int q = lo; q < hi; q++) acc += x[q];
      J->s[i] = acc / (float)(hi - lo);
    }
  }
  int nRough = (int)llrintf(FAD_ROUGH_MS * 1e-3f * J->fs); if (nRough < 3) nRough = 3;
  J->pre[0] = 0.0; for (int i = 0; i < n; i++) J->pre[i + 1] = J->pre[i] + (double)J->s[i];  // prefix(s)
  for (int i = 0; i < n; i++) J->rr[i] = J->s[i] - fadMov(J->pre, n, nRough, i);  // r1 = s - movavg(s,nRough)
  // r1 mean / var (chunked float-partial -> double, order preserved)
  { double acc = 0.0; for (int p = 0; p < n; p += FAD_SEG_SAMPLES) { int end = p + FAD_SEG_SAMPLES; if (end > n) end = n; float part = 0.0f; for (int i = p; i < end; i++) part += J->rr[i]; acc += (double)part; } J->r1Mean = acc / (double)n; }
  { float m = (float)J->r1Mean; double acc = 0.0; for (int p = 0; p < n; p += FAD_SEG_SAMPLES) { int end = p + FAD_SEG_SAMPLES; if (end > n) end = n; float part = 0.0f; for (int i = p; i < end; i++) { float d = J->rr[i] - m; part += d * d; } acc += (double)part; } J->r1Var = acc / (double)n; }
  int lagMaxCls = n / 4; if (lagMaxCls > FAD_ACF_LAG_CAP) lagMaxCls = FAD_ACF_LAG_CAP;
  // regime ACF over lags 4..lagMaxCls of r1
  if (J->r1Var <= 0.0) {
    for (int l = 4; l <= lagMaxCls; l++) J->acf[l - 4] = 0.0f;
  } else {
    float m = (float)J->r1Mean;
    for (int lag = 4; lag <= lagMaxCls; lag++) {
      int len = n - lag; double acc = 0.0;
      for (int seg = 0; seg < len; seg += FAD_SEG_SAMPLES) {
        int end = seg + FAD_SEG_SAMPLES; if (end > len) end = len;
        float part = 0.0f; const float *a = J->rr + seg, *b = J->rr + seg + lag; int cnt = end - seg;
        for (int i = 0; i < cnt; i++) part += (a[i] - m) * (b[i] - m);
        acc += (double)part;
      }
      J->acf[lag - 4] = (float)(acc / (double)len / J->r1Var);
    }
  }
  // regime peak -> cycle period -> idle/cruise ladder
  {
    int m = lagMaxCls - 4 + 1;
    int bestP = -1;
    for (int p = 1; p < m - 1; p++)
      if (J->acf[p] > J->acf[p - 1] && J->acf[p] >= J->acf[p + 1])
        if (bestP < 0 || J->acf[p] > J->acf[bestP]) bestP = p;
    if (bestP < 0) goto done;  // no periodicity -> quiet
    J->Tcycle = (float)(4 + bestP);
    J->cruise = (J->Tcycle < FAD_REGIME_CYCLE_MS * 1e-3f * J->fs) ? 1 : 0;
    if (J->cruise) {
      J->nRungs = 3;
      J->rungW[0] = 0.4f;  J->rungF[0] = 0.3f;
      J->rungW[1] = 0.4f;  J->rungF[1] = 0.5f;
      J->rungW[2] = 0.75f; J->rungF[2] = 0.65f;
    } else {
      J->nRungs = 1; J->rungW[0] = 0.75f; J->rungF[0] = 0.65f;
    }
  }
  // bootstrap pitch pick (conservative rung)
  if (fadPick(J, 0.75f, 0.65f) < 0 || J->nKept < 32) goto done;
  {
    int niv = J->nKept - 1;
    for (int q = 0; q < niv; q++) J->scratch[q] = (float)(J->kept[q + 1] - J->kept[q]);
    J->P = fadMedian(J->scratch, niv);
    if (!(J->P >= 4.0f && J->P <= 400.0f)) goto done;
    J->nCyc = (int)llrintf(6.0f * J->P);
    J->envN = (int)llrintf(FAD_ENV_CYCLES * (float)J->nCyc);
  }
  for (int i = 0; i < n; i++) J->rr[i] = J->s[i] - fadMov(J->pre, n, J->nCyc, i);  // r = s - movavg(s,nCyc)
  { double acc = 0.0; for (int p = 0; p < n; p += FAD_SEG_SAMPLES) { int end = p + FAD_SEG_SAMPLES; if (end > n) end = n; float part = 0.0f; for (int i = p; i < end; i++) part += J->rr[i] * J->rr[i]; acc += (double)part; } J->rms = (float)sqrt(acc / (double)n); }
  J->pre[0] = 0.0; for (int i = 0; i < n; i++) J->pre[i + 1] = J->pre[i] + (double)fabsf(J->rr[i]);  // prefix(|r|)
  { float floorv = 0.2f * J->rms; for (int i = 0; i < n; i++) { float e = fadMov(J->pre, n, J->envN, i); J->env[i] = (e > floorv) ? e : floorv; } }
  // rn = r/env (into s) + mean / var
  { double acc = 0.0; for (int p = 0; p < n; p += FAD_SEG_SAMPLES) { int end = p + FAD_SEG_SAMPLES; if (end > n) end = n; float part = 0.0f; for (int i = p; i < end; i++) { J->s[i] = J->rr[i] / J->env[i]; part += J->s[i]; } acc += (double)part; } J->rnMean = acc / (double)n; }
  { float m = (float)J->rnMean; double acc = 0.0; for (int p = 0; p < n; p += FAD_SEG_SAMPLES) { int end = p + FAD_SEG_SAMPLES; if (end > n) end = n; float part = 0.0f; for (int i = p; i < end; i++) { float d = J->s[i] - m; part += d * d; } acc += (double)part; } J->rnVar = acc / (double)n; }
  for (int rung = 0; rung < J->nRungs; rung++) fadRung(J, J->rungW[rung], J->rungF[rung]);
done:
  if (out) { if (J->haveBest) *out = J->best; else memset(out, 0, sizeof(*out)); }
  return 1;
}

// Config Sharing: CONFIG_MANIFEST is the single source of truth for shareable NVS "settings" keys.
// tier 1 = exported + imported; tier 3 = exported, never imported (this unit's cloud bookkeeping; alt-health
// record keys import with that record). Tiers gate IMPORT only: never narrow the export, it doubles as the
// remote-debug record. ALT_SETTINGS/PERF_SETTINGS knobs are emitted programmatically; RPM tables ride the
// "tables" section (exportTablesObject). Only credentials/identity and rings uploaded elsewhere stay out;
// config_drift_check.py fails the build on a settingWrite key in neither this list nor its EXCLUDE list.
// Values are raw NVS strings: import is a byte-identical settingWrite + reboot; absent keys keep their default.
struct ConfigManifestEntry { const char *param; const char *nvsKey; uint8_t tier; };
static const ConfigManifestEntry CONFIG_MANIFEST[] = {
  { "BulkVoltage", NK_BulkVoltage, 1 },
  { "AbsorptionVoltage", NK_AbsorptionVoltage, 1 },
  { "FloatVoltage", NK_FloatVoltage, 1 },
  { "ChargedVoltage", NK_ChargedVoltage, 1 },
  { "AbsorptionTimeoutMs", NK_AbsorptionTimeoutMs, 1 },
  { "absorptionCompleteTime", NK_absorptionCompleteTime, 1 },
  { "FLOAT_DURATION", NK_FLOAT_DURATION, 1 },
  { "MinFloatTime", NK_MinFloatTime, 1 },
  { "ChargedDetectionTime", NK_ChargedDetectionTime, 1 },
  { "bulkVoltageHoldMs", NK_bulkVoltageHoldMs, 1 },
  { "TailCurrent", NK_TailCurrent, 1 },
  { "TailCurrent_A", NK_TailCurrent_A, 1 },
  { "CurrentThreshold", NK_CurrentThreshold, 1 },
  { "RebulkCurrent_A", NK_RebulkCurrent_A, 1 },
  { "RebulkVoltage", NK_RebulkVoltage, 1 },
  { "rebulkDebounceTime", NK_rebulkDebounceTime, 1 },
  { "SOC_AllowRebulk_percent", NK_SOC_AllowRebulk_percent, 1 },
  { "SOC_BlockRebulk_percent", NK_SOC_BlockRebulk_percent, 1 },
  { "MaintainMode", NK_MaintainMode, 1 },
  { "UseFloat", NK_UseFloat, 1 },
  { "TargetVoltageMode", NK_TargetVoltageMode, 1 },
  { "TargetVoltageSetpoint", NK_TargetVoltageSetpoint, 1 },
  { "ChargeEfficiency", NK_ChargeEfficiency, 1 },
  { "PeukertExponent", NK_PeukertExponent, 1 },
  { "BatteryCapacity_Ah", NK_BatteryCapacity_Ah, 1 },
  { "AlternatorNominalAmps", NK_AlternatorNominalAmps, 1 },
  { "SolarWatts", NK_SolarWatts, 1 },
  { "MaximumAllowedBatteryAmps", NK_MaximumAllowedBatteryAmps, 1 },
  { "MaxTableValue", NK_MaxTableValue, 1 },
  { "MaxDuty", NK_MaxDuty, 1 },
  { "MaxFieldVolts", NK_MaxFieldVolts, 1 },
  { "MinDuty", NK_MinDuty, 1 },
  { "AlternatorHardShutdownV", NK_AlternatorHardShutdownV, 1 },
  { "MinRPMForField", NK_MinRPMForField, 1 },
  { "FIELD_COLLAPSE_DELAY", NK_FIELD_COLLAPSE_DELAY, 1 },
  { "DutyRampRate", NK_DutyRampRate, 1 },
  { "DutySlowRampRate", NK_DutySlowRampRate, 1 },
  { "WarmupRampRate", NK_WarmupRampRate, 1 },
  { "StartupRiseRate", NK_StartupRiseRate, 1 },
  { "FieldResistance", NK_FieldResistance, 1 },
  { "FieldResTrefC", NK_FieldResTrefC, 1 },   // written only beside FieldResistance (no /get param of its own)
  { "FieldResDefault", NK_FieldResDefault, 1 },   // written by the test screen's skip (/fieldResTest skip=1) and cleared by any measured value
  { "FieldWiring", NK_FieldWiring, 1 },
  { "FieldWindingV", NK_FieldWindingV, 1 },
  // Travels with the knobs it describes: a full export's field-duty values sit on this winding's scale, and the boot
  // reconcile (fieldWindingReconcile) converts them to the destination's own effective winding.
  { "fieldWindApplied", NK_fieldWindApplied, 1 },
  // Keyed to the exact winding:resistance pair, so it only takes effect when both came from the file too.
  { "fieldWindAck", NK_fieldWindAck, 1 },
  { "PulleyRatio", NK_PulleyRatio, 1 },
  // A changed value is written raw like any row; applyImportConfig then arms the axis wipe the /get guard would
  // have, limited to the speed-indexed data the file does not bring with it.
  { "RPMScalingFactor", NK_RPMScalingFactor, 1 },
  { "SwitchingFrequency", NK_SwitchingFrequency, 1 },
  { "PidKp", NK_PidKp, 1 },
  { "PidKi", NK_PidKi, 1 },
  { "PidKd", NK_PidKd, 1 },
  { "PidSampleDivisor", NK_PidSampleDivisor, 1 },
  { "PIDTrackingGain", NK_PIDTrackingGain, 1 },
  { "OutputPIDFilterTC", NK_OutputPIDFilterTC, 1 },
  { "OutputPIDMA_N", NK_OutputPIDMA_N, 1 },
  { "OutputPIDSigSrc", NK_OutputPIDSigSrc, 1 },
  { "VoltageFilterTC", NK_VoltageFilterTC, 1 },
  { "DvdtTC", NK_DvdtTC, 1 },
  { "VoltageKp", NK_VoltageKp, 1 },
  { "VoltageKi", NK_VoltageKi, 1 },
  { "VoltageLoopInterval", NK_VoltageLoopInterval, 1 },
  { "AwBleedRate", NK_AwBleedRate, 1 },
  { "AwSeedProtectMs", NK_AwSeedProtectMs, 1 },
  { "SetpointRiseRate", NK_SetpointRiseRate, 1 },
  { "SetpointFallRate", NK_SetpointFallRate, 1 },
  { "CvBrakeFallRate", NK_CvBrakeFallRate, 1 },
  { "SetpointBigStepThresh", NK_SetpointBigStepThresh, 1 },
  { "SetpointBigStepRiseRate", NK_SetpointBigStepRiseRate, 1 },
  { "FastSetpointRiseRate", NK_FastSetpointRiseRate, 1 },
  { "FastSetpointRiseHeadroomV", NK_FastSetpointRiseHeadroomV, 1 },
  { "FastSetpointRiseWindowMs", NK_FastSetpointRiseWindowMs, 1 },
  { "cvHelpersEnabled", NK_cvHelpersEnabled, 1 },
  { "cvGainMode", NK_cvGainMode, 1 },
  { "cvCrossover", NK_cvCrossover, 1 },
  { "cvPiZero", NK_cvPiZero, 1 },
  { "cvAlpha", NK_cvAlpha, 1 },
  { "vTgtRampEnable", NK_vTgtRampEnable, 1 },
  { "vTgtRampUp", NK_vTgtRampUp, 1 },
  { "vTgtRampDn", NK_vTgtRampDn, 1 },
  { "setpointSlewEnable", NK_setpointSlewEnable, 1 },
  { "cvRiseGovEnable", NK_cvRiseGovEnable, 1 },
  { "cvRecovEnable", NK_cvRecovEnable, 1 },
  { "loadServeBoostEnable", NK_loadServeBoostEnable, 1 },
  { "reseedCorrEnable", NK_reseedCorrEnable, 1 },
  { "HuntGovEnable", NK_HuntGovEnable, 1 },
  { "HuntCutPct", NK_HuntCutPct, 1 },
  { "HuntVerifyPct", NK_HuntVerifyPct, 1 },
  { "HuntWingPct", NK_HuntWingPct, 1 },
  { "HuntCooldownMin", NK_HuntCooldownMin, 1 },
  { "HuntSteadyPct", NK_HuntSteadyPct, 1 },
  { "HuntQualifyScans", NK_HuntQualifyScans, 1 },
  { "HuntTrigPct", NK_HuntTrigPct, 1 },
  { "cvRecovSec", NK_cvRecovSec, 1 },      // retired, inert — kept so shared configs stay complete
  { "cvRecovEmaxV", NK_cvRecovEmaxV, 1 },  // retired, inert — kept so shared configs stay complete
  { "cvRecovKiMax", NK_cvRecovKiMax, 1 },
  { "cvWindDownEnable", NK_cvWindDownEn, 1 },
  { "cvWindDownRate", NK_cvWindDownRate, 1 },
  { "cvWindDownStopV", NK_cvWindDownStopV, 1 },
  { "cvRecovBoostEnable", NK_cvRecovBoostEnable, 1 },
  { "cvRecovBoostMax", NK_cvRecovBoostMax, 1 },
  { "cvRecovBoostErrV", NK_cvRecovBoostErrV, 1 },
  { "cvRecovBoostFloorV", NK_cvRecovBoostFloorV, 1 },
  { "cvRecovDeepBandV", NK_cvRecovDeepBandV, 1 },
  { "cvRecovDeepMult", NK_cvRecovDeepMult, 1 },
  { "cvRecovFlareBandV", NK_cvRecovFlareBandV, 1 },
  { "cvRecovFlareFrac", NK_cvRecovFlareFrac, 1 },
  { "dutySlewEnable", NK_dutySlewEnable, 1 },
  { "testSlewMode", NK_testSlewMode, 1 },
  { "cvTestSlewMode", NK_cvTestSlewMode, 1 },
  { "coldChargeLockoutEnable", NK_coldChargeLockoutEnable, 1 },
  { "MinChargeTempF", NK_MinChargeTempF, 1 },
  { "battTempProbeEnable", NK_battTempProbeEnable, 1 },
  { "extraTempProbeEnable", NK_extraTempProbeEnable, 1 },
  { "battTempSource", NK_battTempSource, 1 },
  { "hotChargeLockoutEnable", NK_hotChargeLockoutEnable, 1 },
  { "MaxChargeTempF", NK_MaxChargeTempF, 1 },
  { "extraTempAlarmHiEnable", NK_extraTempAlarmHiEnable, 1 },
  { "extraTempAlarmHiF", NK_extraTempAlarmHiF, 1 },
  { "extraTempAlarmLoEnable", NK_extraTempAlarmLoEnable, 1 },
  { "extraTempAlarmLoF", NK_extraTempAlarmLoF, 1 },
  { "battTempDerateEnable", NK_battTempDerateEn, 1 },
  { "battTempCoeff", NK_battTempCoeff, 1 },
  { "VoltageKd", NK_VoltageKd, 1 },
  { "CvKdDeadbandVps", NK_CvKdDeadbandVps, 1 },
  { "CvKdDbSlope", NK_CvKdDbSlope, 1 },
  { "CvKdDbFloor", NK_CvKdDbFloor, 1 },
  { "CvKdDbCeil", NK_CvKdDbCeil, 1 },
  { "CvKdExcessMode", NK_CvKdExcessMode, 1 },
  { "CvKdOneSided", NK_CvKdOneSided, 1 },
  { "CvKdArmV", NK_CvKdArmV, 1 },
  { "CvKdMaxTrimA", NK_CvKdMaxTrimA, 1 },
  { "CvKdVoltFiltTC", NK_CvKdVoltFiltTC, 1 },
  { "CvKdSlopeCeil", NK_CvKdSlopeCeil, 1 },
  { "CvStressDropV", NK_CvStressDropV, 1 },
  { "CvStressFailBandV", NK_CvStressFailBandV, 1 },
  { "CvKdTd", NK_CvKdTd, 1 },
  { "TdPred", NK_TdPred, 1 },
  { "KHard", NK_KHard, 1 },
  { "OvGroup1Enable", NK_OvGroup1Enable, 1 },
  { "OvGroup2Enable", NK_OvGroup2Enable, 1 },
  { "HardOCEnable", NK_HardOCEnable, 1 },
  { "TachLieEnable", NK_TachLieEnable, 1 },
  { "IExcessEnable", NK_IExcessEnable, 1 },
  { "BattLimitEnable", NK_BattLimitEnable, 1 },
  { "LoadDumpEnable", NK_LoadDumpEnable, 1 },
  { "OvMeasMarginV", NK_OvMeasMarginV, 1 },
  { "OvPredMarginV", NK_OvPredMarginV, 1 },
  { "OvTierLoMarginV", NK_OvTierLoMarginV, 1 },
  { "OvTierLoDwellMs", NK_OvTierLoDwellMs, 1 },
  { "OvTierMidMarginV", NK_OvTierMidMarginV, 1 },
  { "OvTierMidDwellMs", NK_OvTierMidDwellMs, 1 },
  { "VoltageHardwareLimit", NK_VoltageHardwareLimit, 1 },
  { "LoadDumpN1", NK_LoadDumpN1, 1 },
  { "LoadDumpN2", NK_LoadDumpN2, 1 },
  { "LoadDumpN3", NK_LoadDumpN3, 1 },
  { "HardOCDebounceMs", NK_HardOCDebounceMs, 1 },
  { "SettleTimeBeforeCut", NK_SettleTimeBeforeCut, 1 },
  { "ShutdownPhase2HoldMs", NK_ShutdownPhase2HoldMs, 1 },
  { "ShutdownRampTauS", NK_ShutdownRampTauS, 1 },
  { "NoOutputAmps", NK_NoOutputAmps, 1 },
  { "ReseedFrac", NK_ReseedFrac, 1 },
  { "ReseedFracNoShunt", NK_ReseedFracNS, 1 },
  { "CvRecovClimbRate", NK_CvRecovClimb, 1 },
  { "IExcessArmMarginV", NK_IExcessArmMarginV, 1 },
  { "IExcessCeilA", NK_IExcessCeilA, 1 },
  { "IExcessFloorA", NK_IExcessFloorA, 1 },
  { "BattCurrentLimitA", NK_BattCurrentLimitA, 1 },
  { "IExcessFrac", NK_IExcessFrac, 1 },
  { "IExcessFracBulk", NK_IExcessFracBulk, 1 },
  { "IExcessBaseA", NK_IExcessBaseA, 1 },
  { "IExcessCcOffsetA", NK_IExcessCcOffsetA, 1 },
  { "IExcessKBleed", NK_IExcessKBleed, 1 },
  { "IExcessRelFrac", NK_IExcessRelFrac, 1 },
  { "IExcessTau", NK_IExcessTau, 1 },
  { "TemperatureLimitF", NK_TemperatureLimitF, 1 },
  { "TempAlarm", NK_TempAlarm, 1 },
  { "TempAlarmLow", NK_TempAlarmLow, 1 },
  { "TempWarnExcess", NK_TempWarnExcess, 1 },
  { "TempCritExcess", NK_TempCritExcess, 1 },
  { "TempSustainedTimeout", NK_TempSustainedTimeout, 1 },
  { "TempPIDKp", NK_TempPIDKp, 1 },
  { "TempPIDKi", NK_TempPIDKi, 1 },
  { "TempPIDKiDownFrac", NK_TempPIDKiDownFrac, 1 },
  { "TempPIDIntervalMs", NK_TempPIDIntervalMs, 1 },
  { "TempPIDFilterAlpha", NK_TempPIDFilterAlpha, 1 },
  { "ThermalLookaheadSec", NK_ThermalLookaheadSec, 1 },
  { "ThermalSlopeWindowSec", NK_ThermalSlopeWindowSec, 1 },
  { "TempSource", NK_TempSource, 1 },
  { "R_fixed", NK_R_fixed, 1 },
  { "Beta", NK_Beta, 1 },
  { "T0_C", NK_T0_C, 1 },
  { "WindingTempOffset", NK_WindingTempOffset, 1 },
  { "displayTempUnit", NK_displayTempUnit, 1 },
  { "displayVolUnit", NK_displayVolUnit, 1 },
  { "LearningUpStep", NK_LearningUpStep, 1 },
  { "LearningDownStep", NK_LearningDownStep, 1 },
  { "LearningSettlingPeriod", NK_LearningSettlingPeriod, 1 },
  { "LearningMemoryDuration", NK_LearningMemoryDuration, 1 },
  { "LearningRPMChangeThreshold", NK_LearningRPMChangeThreshold, 1 },
  { "LearningTempHysteresis", NK_LearningTempHysteresis, 1 },
  { "MinLearningInterval", NK_MinLearningInterval, 1 },
  { "NeighborLearningFactor", NK_NeighborLearningFactor, 1 },
  { "LogAllLearningEvents", NK_LogAllLearningEvents, 1 },
  { "IgnoreLearningDuringPenalty", NK_IgnoreLearningDuringPenalty, 1 },
  { "IgnoreRPM", NK_IgnoreRPM, 1 },
  { "IgnoreTemperature", NK_IgnoreTemperature, 1 },
  { "IgnoreAltCurrent", NK_IgnoreAltCurrent, 1 },
  { "IgnoreBattVMain", NK_IgnoreBattVMain, 1 },
  { "IgnoreBattV2", NK_IgnoreBattV2, 1 },
  { "LimpDutyPct", NK_LimpDutyPct, 1 },
  { "NoRpmCurrentA", NK_NoRpmCurrentA, 1 },
  { "MaxPenaltyDuration", NK_MaxPenaltyDuration, 1 },
  { "MaxPenaltyPercent", NK_MaxPenaltyPercent, 1 },
  { "SafeOperationThreshold", NK_SafeOperationThreshold, 1 },
  { "kneeLearnEnable", NK_kneeLearnEnable, 1 },
  { "kneeMarginPct", NK_kneeMarginPct, 1 },
  { "kneeMaxFloorPct", NK_kneeMaxFloorPct, 1 },
  { "kneeOnsetA", NK_kneeOnsetA, 1 },
  { "kneeReArmA", NK_kneeReArmA, 1 },
  { "kneeStepPct", NK_kneeStepPct, 1 },
  { "kneeDwellSec", NK_kneeDwellSec, 1 },
  { "kneeTempRefF", NK_kneeTempRefF, 1 },
  { "kneeTempComp", NK_kneeTempComp, 1 },
  { "kneeRpmTolPct", NK_kneeRpmTolPct, 1 },
  { "kneeTempTolF", NK_kneeTempTolF, 1 },
  { "kneeDutyTolPct", NK_kneeDutyTolPct, 1 },
  { "faEnabled", NK_faEnabled, 1 },
  { "faAlarmEnable", NK_faAlarmEnable, 1 },
  { "faPeakMinA", NK_faPeakMinA, 1 },
  { "faAmpsDriftPct", NK_faAmpsDriftPct, 1 },
  { "faAmpsDriftFloorA", NK_faAmpsDriftFloorA, 1 },
  { "faAttenUpAmps", NK_faAttenUpAmps, 1 },
  { "faAttenDownAmps", NK_faAttenDownAmps, 1 },
  { "faRpmEdgeMargin", NK_faRpmEdgeMargin, 1 },
  { "faAnomPause", NK_faAnomPause, 1 },
  { "ripWinMs", NK_ripWinMs, 1 },
  { "ripDriftFloorA", NK_ripDriftFloorA, 1 },
  { "ripDriftPct", NK_ripDriftPct, 1 },
  { "SystemIDStabilizeAmps", NK_SystemIDStabilizeAmps, 1 },
  { "SystemIDStepAmplitude", NK_SystemIDStepAmplitude, 1 },
  { "systemIDTestType", NK_systemIDTestType, 1 },
  { "systemIDSineCycles", NK_systemIDSineCycles, 1 },
  { "systemIDSineFreqStart", NK_systemIDSineFreqStart, 1 },
  { "systemIDSineFreqEnd", NK_systemIDSineFreqEnd, 1 },
  { "tuningWaveform", NK_tuningWaveform, 1 },
  { "tuningSineFreq", NK_tuningSineFreq, 1 },
  { "tuningSweepStart", NK_tuningSweepStart, 1 },
  { "tuningSweepEnd", NK_tuningSweepEnd, 1 },
  { "tuningSweepCycles", NK_tuningSweepCycles, 1 },
  { "tuningWaveFloor", NK_tuningWaveFloor, 1 },
  { "waveAmplitude", NK_waveAmplitude, 1 },
  { "wavePeriod", NK_wavePeriod, 1 },
  { "cvWaveAmplitudeV", NK_cvWaveAmplitudeV, 1 },
  { "cvWavePeriodSec", NK_cvWavePeriodSec, 1 },
  { "cvKOvershoot", NK_cvKOvershoot, 1 },
  { "cvConsecutiveReads", NK_cvConsecutiveReads, 1 },
  { "LoadDumpDtThresh", NK_LoadDumpDtThresh, 1 },
  { "LoadDumpDtThresh1", NK_LoadDumpDtThresh1, 1 },
  { "LoadDumpDtThresh3", NK_LoadDumpDtThresh3, 1 },
  { "VoltageDisagreeThreshold", NK_VoltageDisagreeThreshold, 1 },
  { "VoltageDisagreeTimeout", NK_VoltageDisagreeTimeout, 1 },
  { "AlarmActivate", NK_AlarmActivate, 1 },
  { "AlarmLatchEnabled", NK_AlarmLatchEnabled, 1 },
  { "GeneralFaultAlarm", NK_GeneralFaultAlarm, 1 },
  { "CurrentAlarmHigh", NK_CurrentAlarmHigh, 1 },
  { "VoltageAlarmHigh", NK_VoltageAlarmHigh, 1 },
  { "VoltageAlarmLow", NK_VoltageAlarmLow, 1 },
  { "SocAlarmLow", NK_SocAlarmLow, 1 },
  { "UVThresholdHigh", NK_UVThresholdHigh, 1 },
  { "CAPSIZE_THRESHOLD_DEG", NK_CAPSIZE_THRESHOLD_DEG, 1 },
  { "PITCHPOLE_THRESHOLD_DEG", NK_PITCHPOLE_THRESHOLD_DEG, 1 },
  { "SLAM_THRESHOLD_G", NK_SLAM_THRESHOLD_G, 1 },
  { "bmsLogic", NK_bmsLogic, 1 },
  { "bmsLogicLevelOff", NK_bmsLogicLevelOff, 1 },
  { "capLimitMode", NK_capLimitMode, 1 },
  { "NMEA0183Data", NK_NMEA0183Data, 1 },
  { "NMEA0183Baud", NK_NMEA0183Baud, 1 },
  { "NMEA0183Invert", NK_NMEA0183Invert, 1 },
  { "NMEA2KData", NK_NMEA2KData, 1 },
  { "n2kTxEnable", NK_n2kTxEn, 1 },
  { "n2kDeviceInstance", NK_n2kDevInst, 1 },
  { "n2kBattEnable", NK_n2kBattEn, 1 },
  { "n2kBattInstance", NK_n2kBattInst, 1 },
  { "n2kBattCfgEnable", NK_n2kBattCfgEn, 1 },
  { "n2kAltEnable", NK_n2kAltEn, 1 },
  { "n2kAltInstance", NK_n2kAltInst, 1 },
  { "n2kAltTempEnable", NK_n2kAltTempEn, 1 },
  { "n2kTempInstance", NK_n2kTempInst, 1 },
  { "n2kTempSource", NK_n2kTempSrc, 1 },
  { "n2kExtraTempEnable", NK_n2kExtraTempEnable, 1 },
  { "n2kExtraTempInstance", NK_n2kExtraTempInstance, 1 },
  { "n2kExtraTempSource", NK_n2kExtraTempSource, 1 },
  { "n2kChgrEnable", NK_n2kChgrEn, 1 },
  { "n2kChgrInstance", NK_n2kChgrInst, 1 },
  { "n2kChgrCfgEnable", NK_n2kChgrCfgEn, 1 },
  { "n2kChgrMode", NK_n2kChgrMode, 1 },
  { "n2kEngRpmEnable", NK_n2kEngRpmEn, 1 },
  { "n2kEngInstance", NK_n2kEngInst, 1 },
  { "n2kEngDynEnable", NK_n2kEngDynEn, 1 },
  { "n2kEngBitsEnable", NK_n2kEngBitsEn, 1 },
  { "n2kConvStatEnable", NK_n2kConvStatEn, 1 },
  { "n2kRxBattInstance", NK_n2kRxBattInst, 1 },
  { "rvcTxEnable", NK_rvcTxEn, 1 },
  { "rvcChgrEnable", NK_rvcChgrEn, 1 },
  { "rvcDcEnable", NK_rvcDcEn, 1 },
  { "rvcFaultEnable", NK_rvcFaultEn, 1 },
  { "rvcChgrInstance", NK_rvcChgrInst, 1 },
  { "rvcDcInstance", NK_rvcDcInst, 1 },
  { "rvcDevPriority", NK_rvcDevPri, 1 },
  { "dvccEn", NK_dvccEn, 1 },
  { "dvccSrcType", NK_dvccSrcType, 1 },
  { "dvccInst", NK_dvccInst, 1 },
  { "dvccSilenceS", NK_dvccSilenceS, 1 },
  { "dvccVicCutHold", NK_dvccVicCutHold, 1 },
  { "dvccLostStop", NK_dvccLostStop, 1 },
  { "dvccSettleS", NK_dvccSettleS, 1 },
  { "dvccCvlMin", NK_dvccCvlMin, 1 },
  { "dvccCvlMax", NK_dvccCvlMax, 1 },
  { "VeData", NK_VeData, 1 },
  { "weatherModeEnabled", NK_weatherModeEnabled, 1 },
  { "timeSourceMode", NK_timeSourceMode, 1 },
  { "gpsPositionSource", NK_gpsPositionSource, 1 },
  { "speedSourceMode", NK_speedSourceMode, 1 },
  { "wifiNapEnabled", NK_wifiNapEnabled, 1 },
  { "ZeroLogEnable", NK_ZeroLogEnable, 1 },
  { "performanceRatio", NK_performanceRatio, 1 },
  { "solarLearnEnable", NK_solarLearnEnable, 1 },
  { "solarUseConsEnable", NK_solarUseConsEnable, 1 },
  { "solarConsMarginPct", NK_solarConsMarginPct, 1 },
  { "solarLearnRatePct", NK_solarLearnRatePct, 1 },
  { "HiLow", NK_HiLow, 1 },
  { "ManualSOCPoint", NK_ManualSOCPoint, 1 },
  { "ManualLifePercentage", NK_ManualLifePercentage, 1 },
  { "CloudFeatures", NK_CloudFeatures, 1 },
  { "RemoteDiagnostics", NK_RemoteDiagnostics, 1 },
  { "FuelEfficiency", NK_FuelEfficiency, 1 },
  { "WeatherUpdateInterval", NK_WeatherUpdateInterval, 1 },
  { "WeatherTimeoutMs", NK_WeatherTimeoutMs, 1 },
  { "webgaugesinterval", NK_webgaugesinterval, 1 },
  { "plotTimeWindow", NK_plotTimeWindow, 1 },
  { "maxPoints", NK_maxPoints, 1 },
  { "xTime", NK_xTime, 1 },
  { "timeAxisModeChanging", NK_timeAxisModeChanging, 1 },
  { "Ymin1", NK_Ymin1, 1 },
  { "Ymax1", NK_Ymax1, 1 },
  { "Ymin2", NK_Ymin2, 1 },
  { "Ymax2", NK_Ymax2, 1 },
  { "Ymin3", NK_Ymin3, 1 },
  { "Ymax3", NK_Ymax3, 1 },
  { "Ymin4", NK_Ymin4, 1 },
  { "Ymax4", NK_Ymax4, 1 },
  { "yyMin", NK_yyMin, 1 },
  { "yyMax", NK_yyMax, 1 },
  { "bhStepLowA", NK_bhStepLowA, 1 },
  { "bhStepDeltaA", NK_bhStepDeltaA, 1 },
  { "bhDwellMs", NK_bhDwellMs, 1 },   // export key names the STORED unit (ms); the UI param bhDwellSec is seconds
  { "bhNumEdges", NK_bhNumEdges, 1 },
  { "capRestFrac", NK_capRestFrac, 1 },
  { "capRestFloor", NK_capRestFloor, 1 },
  { "capSettleRate", NK_capSettleRate, 1 },
  { "capSocLowMax", NK_capSocLowMax, 1 },
  { "capMinSpan", NK_capMinSpan, 1 },
  { "capFullSoc", NK_capFullSoc, 1 },
  { "capRefMode", NK_capRefMode, 1 },
  { "capTempNorm", NK_capTempNorm, 1 },
  { "capTempCoeff", NK_capTempCoeff, 1 },
  { "capTempRef", NK_capTempRef, 1 },
  { "capOcv", NK_capOcvBlob, 1 },
  { "BattMonSrc", NK_BattMonSrc, 1 },
  { "BatteryVoltageSource", NK_BatteryVoltageSource, 1 },
  { "ShuntResistanceMicroOhm", NK_ShuntResistanceMicroOhm, 1 },
  { "AmpSensorRange", NK_AmpSensorRange, 1 },
  { "InvertAltAmps", NK_InvertAltAmps, 1 },
  { "InvertBattAmps", NK_InvertBattAmps, 1 },
  { "BatteryShuntPresent", NK_BatteryShuntPresent, 1 },
  { "ShuntLead", NK_ShuntLead, 1 },
  { "AlternatorCOffset", NK_AlternatorCOffset, 1 },
  { "BatteryCOffset", NK_BatteryCOffset, 1 },
  { "AutoAltCurrentZero", NK_AutoAltCurrentZero, 1 },
  { "BatteryVoltage", NK_BatteryVoltage, 1 },
  { "cvPlantKa", NK_cvPlantKa, 1 },
  { "cvPlantKb", NK_cvPlantKb, 1 },
  { "CommissionTempF", NK_CommissionTempF, 1 },
  { "CommissionTempSrc", NK_CommissionTempSrc, 1 },   // with CommissionTempF: computeCvTempScale trusts the pair, so the recipient needs the DONOR's provenance
  { "CommissionEpoch", NK_CommissionEpoch, 1 },   // commissioning status: applied by applyImportCommissioning, not the generic loop
  { "maxWorkingRpm", NK_maxWorkingRpm, 1 },
  { "systemIDPlantTauMs", NK_sysidPlantTau, 1 },
  { "fieldDecayTauMs", NK_fieldDecayTau, 1 },
  { "fdDrainLoMs", NK_fdDrainLoMs, 1 },
  { "fdDrainHiMs", NK_fdDrainHiMs, 1 },
  { "fdDrainRpmLo", NK_fdDrainRpmLo, 1 },
  { "fdDrainRpmHi", NK_fdDrainRpmHi, 1 },
  { "ripFitAlt", NK_ripFitAlt, 1 },
  { "slpFitAlt", NK_slpFitAlt, 1 },
  // Level calibration of the board's own bolted-down attitude. Travels with imu_mnt_state (the install verdict
  // judged on this capture); applyImportConfig drops the verdict when the capture or orientation arrives without it.
  { "imu_zero", NK_imu_zero, 1 },
  // Vessel Info: param names match /vessel_info.json. battery_voltage / battery_capacity_ah / solar_watts are
  // already BatteryVoltage / BatteryCapacity_Ah / SolarWatts above.
  { "boat_length_ft", NK_boatLenFt, 1 },
  { "boat_displacement_lbs", NK_boatDispLbs, 1 },
  { "boat_type", NK_boatType, 1 },
  { "boat_make_model", NK_boatMakeModel, 1 },
  { "boat_year", NK_boatYear, 1 },
  { "home_port", NK_homePort, 1 },
  { "engine_make", NK_engineMake, 1 },
  { "engine_hp", NK_engineHp, 1 },
  { "battery_type", NK_batteryType, 1 },
  { "battery_make_model", NK_battMakeModel, 1 },
  { "alternator_brand_model", NK_altBrandModel, 1 },
  { "imu_mount_orientation", NK_imuMountOrient, 1 },
  { "imu_dist_bow_ft", NK_imuDistBowFt, 1 },
  { "imu_dist_cl_ft", NK_imuDistClFt, 1 },
  { "imu_height_wl_ft", NK_imuHtWlFt, 1 },
  { "regulator_name", NK_regName, 1 },
  { "vesselSaved", NK_vesselSaved, 1 },
  // ── Runtime intent, lifecycle and small result blobs (exported so support sees the full device state).
  { "OnOff", NK_OnOff, 1 },
  { "ManualFieldToggle", NK_ManualFieldToggle, 1 },
  { "ManualDutyTarget", NK_ManualDutyTarget, 1 },
  { "IgnitionOverride", NK_IgnitionOverride, 1 },
  { "LimpHome", NK_LimpHome, 1 },
  { "PhysicalPanelOverride", NK_PhysicalPanelOverride, 1 },
  { "TuningMode", NK_TuningMode, 1 },       // both tuning modes are cleared at every boot, so an import of either is inert
  { "CVTuningMode", NK_CVTuningMode, 1 },
  { "battMaxMode", NK_battMaxMode, 1 },
  { "socInfoAvailable", NK_socInfoAvailable, 1 },
  { "totalPowerCycles", NK_totalPowerCycles, 1 },
  { "weatherDataValid", NK_weatherDataValid, 1 },
  // GPS: the manual position, its flag, and the last fix persisted for the weather lookup. A library submission
  // leaves them out (script.js _CFG_GPS_KEYS).
  { "gpsManualActive", NK_gpsManualActive, 1 },
  { "LatitudeManual", NK_LatitudeManual, 1 },
  { "LongitudeManual", NK_LongitudeManual, 1 },
  { "gpsLastFixLat", NK_LatitudeNMEA, 1 },
  { "gpsLastFixLon", NK_LongitudeNMEA, 1 },
  { "VMGTargetBearing", NK_VMGTargetBearing, 1 },
  { "LastResetReason", NK_LastResetReason, 1 },
  // Which one-time settings conversions have run. Imported capped at this firmware's version, so a file saved on older
  // firmware gets the same boot conversions its source got when it upgraded.
  { "cfgSchema", NK_cfgSchema, 1 },
  { "lastAppldCfgId", NK_lastAppldCfgId, 3 },
  // Receipt of an admin config push that applied on the previous boot ("<n>|key,key,..."), held
  // until the dashboard acks it — pairs with lastAppldCfgId to show whether the owner ever saw it.
  { "cfgPushNotify", NK_cfgPushNotify, 3 },
  { "imu_mnt_state", NK_imu_mnt_state, 1 },
  { "RpmAxisWipeLoc", NK_RpmAxisWipeLoc, 1 },   // "1" = a local axis wipe a reboot cut short; the importer re-runs it at boot as the source would
  { "impBootAct", NK_impBootAct, 1 },           // same, for an import's own boot-time wipe (applyImportConfig ORs its own bits in after the loop)
  // Owed cloud wipes act on the records of whichever unit holds them, keyed to its own cloud login.
  { "RpmAxisWipePend", NK_RpmAxisWipePend, 3 },
  { "AltWipePend", NK_AltWipePend, 3 },
  { "PerfWipePend", NK_PerfWipePend, 3 },
  { "recSeedOwed", NK_recSeedOwed, 3 },   // curves this unit still owes its own cloud record (recSeedOwedSet)
  { "SocSeedAck", NK_SocSeedAck, 1 },
  { "SocSeedSnap", NK_SocSeedSnap, 1 },
  // Imported onto a board with no saved WiFi network, client mode still opens the WiFi setup page.
  { "first_config_done", NK_first_config_done, 1 },
  // Commissioning status and the in-flight wizard's restore points: applied by applyImportCommissioning step by
  // step, never by the generic loop (CFG_IMPORT_DEDICATED).
  { "commissionAgeAck", NK_cmAgeAck, 1 },
  { "commissionChangeAck", NK_cmChangeFlag, 1 },
  { "commissionState", NK_commissionState, 1 },
  { "commissionPhase", NK_commissionPhase, 1 },
  { "commissionDoneMask", NK_commissionDoneMask, 1 },
  { "commissionManualMask", NK_commissionManualMask, 1 },
  { "cxLedgerSeq", NK_cxLedgerSeq, 3 },   // this unit's ledger sequence with the cloud
  { "commissionSnap", NK_commissionSnap, 1 },
  { "commissionStepSnap", NK_commissionStepSnap, 1 },
  { "commissionPreRun", NK_commissionPreRun, 1 },
  { "cvStressLast", NK_cvStressLast, 1 },
  // A write-ahead record of a class change or winding rescale a reboot cut short; the importer's boot replays it.
  { "rescaleJournal", NK_rescaleJournal, 1 },
  { "faCalGain", NK_faCalGain, 1 },
  { "faCalOffA", NK_faCalOffA, 1 },
  { "bhBaselineAh", NK_bhBaseline, 1 },
  { "altbaseSec", NK_altbaseSec, 3 },   // imported only with the alt-health record it belongs to (REC_ALT_NVS)
  { "altRefSrc", NK_altRefSrc, 1 },
  { "altWindV", NK_altWindV, 3 },   // winding the alt-health record is on: imported only with that record, so it never hides a winding change
  // Address this node claimed on the NMEA 2000 bus and persisted (5_functions.ino), reloaded as the
  // preferred address at boot. Two units on one address re-arbitrate through the normal address claim.
  { "n2kSrcAddr", NK_n2kSrcAddr, 1 },
  // 126998 installation descriptions as a GX or display last wrote them over the bus (a GX's name for this unit).
  { "n2kInstDesc1", NK_n2kInstDesc1, 1 },
  { "n2kInstDesc2", NK_n2kInstDesc2, 1 },
  // 1-Wire role bindings (16-hex-char ROM codes); a stored alternator probe missing from this bus is a sensor
  // fault, never rebound. Set by the owAssign* /get role loop, which config_drift_check.py's hasParam scan
  // can't see, so these names are in its MANIFEST_NON_PARAM.
  { "owAddrAlt", NK_owAddrAlt, 1 },
  { "owAddrBatt", NK_owAddrBatt, 1 },
  { "owAddrExtra", NK_owAddrExtra, 1 },
  // The boat's other regulators by hardware uid. Imported without this unit's own entry (CFG_IMPORT_DEDICATED).
  { "pairList", NK_pairList, 1 },
};
static const size_t CONFIG_MANIFEST_COUNT = sizeof(CONFIG_MANIFEST)/sizeof(CONFIG_MANIFEST[0]);

// Tier-1 rows applied by dedicated code, not the generic import loop: commissioning status + restore points
// (applyImportCommissioning), cfgSchema (capped), pairList (own entry removed).
static const char *CFG_IMPORT_DEDICATED[] = {
  "commissionState", "commissionPhase", "commissionDoneMask", "commissionManualMask", "CommissionEpoch",
  "commissionAgeAck", "commissionChangeAck", "commissionSnap", "commissionStepSnap", "commissionPreRun",
  "cvStressLast", "cfgSchema", "pairList",
};
static bool cfgImportDedicated(const char *name) {
  for (size_t i = 0; i < sizeof(CFG_IMPORT_DEDICATED)/sizeof(CFG_IMPORT_DEDICATED[0]); i++)
    if (strcmp(name, CFG_IMPORT_DEDICATED[i]) == 0) return true;
  return false;
}

// JSON string literal; a regulator name or another unit's mDNS record can carry any byte.
static void cfgAppendJsonStr(String &out, const String &val) {
  out += '"';
  for (size_t i = 0; i < val.length(); i++) {
    char c = val[i];
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if ((unsigned char)c < 0x20) {
      char e[8];
      snprintf(e, sizeof(e), "\\u%04x", (unsigned)(unsigned char)c);
      out += e;
    } else {
      out += c;
    }
  }
  out += '"';
}

// NaN/Inf -> null: a raw nan makes the whole settings blob invalid and 500s the daily snapshot.
static void cfgAppendNum(String &out, float v, int dec) {
  if (isnan(v) || isinf(v)) { out += "null"; return; }
  out += String(v, dec);
}

static void cfgAppendFloatArr(String &out, const float *a, int n, int dec) {
  out += '[';
  for (int i = 0; i < n; i++) { if (i) out += ','; cfgAppendNum(out, a[i], dec); }
  out += ']';
}

// {"param":"rawNvsStr",...}: the one builder for both /exportConfig and the daily snapshot's "settings"
// (buildConfigPayload), so neither can drift.
String manifestConfigObject() {
  String j;
  j.reserve(18432);   // manifest + registry + learned-state blobs (DCIR up to ~5.6KB)
  j = "{";
  bool first = true;
  for (size_t i = 0; i < CONFIG_MANIFEST_COUNT; i++) {
    if (!settingExists(CONFIG_MANIFEST[i].nvsKey)) continue;   // key never set -> omit, destination keeps its default
    String v = settingRead(CONFIG_MANIFEST[i].nvsKey);
    if (!first) j += ',';
    first = false;
    j += '"'; j += CONFIG_MANIFEST[i].param; j += "\":";
    cfgAppendJsonStr(j, v);
  }
  // A macro, not a helper: a function taking either registry struct would get an Arduino auto-prototype
  // hoisted above the struct definitions in 7_functions.ino. JSON param = full registry name; NVS key = name
  // truncated to 15 chars, mirroring altSettingsLoad/perfSettingsLoad.
  #define CFG_EMIT_REGISTRY(REG, COUNT) \
    for (size_t i = 0; i < COUNT; i++) { \
      char key[16]; \
      snprintf(key, sizeof(key), "%s", REG[i].name); \
      if (!settingExists(key)) continue; \
      String v = settingRead(key); \
      if (!first) j += ','; \
      first = false; \
      j += '"'; j += REG[i].name; j += "\":"; \
      cfgAppendJsonStr(j, v); \
    }
  CFG_EMIT_REGISTRY(ALT_SETTINGS, ALT_SETTING_COUNT)
  CFG_EMIT_REGISTRY(PERF_SETTINGS, PERF_SETTING_COUNT)
  #undef CFG_EMIT_REGISTRY
  // Derived, read-only: the 12V-equiv base CV gains the voltage loop actually runs (recomputeCvGains), so the blob
  // is self-describing. Not in CONFIG_MANIFEST: the destination recomputes its own from its plant fit.
  if (!first) j += ',';
  first = false;
  j += "\"cvComputedKp\":"; cfgAppendJsonStr(j, String(cvComputedKp, 2));
  j += ",\"cvComputedKi\":"; cfgAppendJsonStr(j, String(cvComputedKi, 2));
  j += ",\"cvComputedKd\":"; cfgAppendJsonStr(j, String(cvComputedKd, 2));
  // Learned per-device state (still evolving after commissioning), for fleet monitoring and local archive; frozen
  // per-run evidence goes to the commissioning ledger. Not in CONFIG_MANIFEST, so never imported: a boat must
  // never adopt another's measured results.
  j += ",\"learned_state\":{";
  {
    bool cfirst = true;
    // Battery-health baseline (Ah) only; DCIR results upload as their own "test" ledger rows.
    if (settingExists(NK_bhBaseline)) { if (!cfirst) j += ','; j += "\"bh_baseline_ah\":"; cfgAppendJsonStr(j, settingRead(NK_bhBaseline)); cfirst = false; }
    // Capacity-fade summary only: the full ring (~21 KB at 512 points) would overflow the 32 KB daily upload buffer.
    if (bhCapRing && bhCapCount > 0) {
      if (!cfirst) j += ',';
      float capPct = bhCapRing[(bhCapHead - 1 + bhCapCap) % bhCapCap].capPct;
      j += "\"bh_capfade_pct\":";     cfgAppendJsonStr(j, String(capPct, 1));
      j += ",\"bh_capfade_points\":"; cfgAppendJsonStr(j, String(bhCapCount));
      cfirst = false;
    }
    // ── Adaptive tables + sensor calibration: monitored, never imported. Real JSON numbers (not the manifest's
    //    quoted strings) so the fleet DB can query them; every per-RPM array shares rpm_axis. ──
    if (!cfirst) j += ',';
    cfirst = false;
    j += "\"rpm_axis\":[";
    for (int i = 0; i < RPM_TABLE_SIZE; i++) { if (i) j += ','; j += String(rpmTableRPMPoints[i]); }
    j += ']';
    // Min% floor in effect (kneeFloor when knee-learning is on), detected knee, frozen flags (1 = converged).
    j += ",\"min_duty_floor\":"; cfgAppendFloatArr(j, rpmMinDutyTable, RPM_TABLE_SIZE, 2);
    j += ",\"knee_detected\":";  cfgAppendFloatArr(j, kneeKnee, RPM_TABLE_SIZE, 2);
    j += ",\"knee_frozen\":[";
    for (int i = 0; i < RPM_TABLE_SIZE; i++) { if (i) j += ','; j += (kneeFrozen[i] ? '1' : '0'); }
    j += "],\"knee_fit_a\":"; cfgAppendNum(j, kneeFitA, 3);
    // Hi and Lo cap tables read from the "learning" NVS namespace so neither live table is disturbed; a mode with
    // no saved blob emits null (it runs computed defaults). cap_mode: 1 = Normal/High, 0 = Low.
    j += ",\"cap_mode\":"; j += String((int)HiLow);
    {
      nvs_handle_t ch; float tmp[RPM_TABLE_SIZE];
      bool haveNvs = (nvs_open("learning", NVS_READONLY, &ch) == ESP_OK);
      const char *capKeys[4] = { "capTable", "capPowerTable", "capTableLo", "capPowerTableLo" };
      const char *capJson[4] = { "cap_current_hi", "cap_power_hi", "cap_current_lo", "cap_power_lo" };
      const int   capDec[4]  = { 1, 0, 1, 0 };
      for (int m = 0; m < 4; m++) {
        j += ",\""; j += capJson[m]; j += "\":";
        size_t sz = sizeof(tmp);
        if (haveNvs && nvs_get_blob(ch, capKeys[m], tmp, &sz) == ESP_OK && sz == sizeof(tmp))
          cfgAppendFloatArr(j, tmp, RPM_TABLE_SIZE, capDec[m]);
        else
          j += "null";
      }
      if (haveNvs) nvs_close(ch);
    }
    // Current-sensor zero-drift fit (zero-offset = offset + slope·(T − T_ref); slope in A/°F). valid=0 until the first
    // accepted fit.
    j += ",\"zero_fit\":{\"valid\":"; j += String((int)zfValid);
    j += ",\"sensor\":"; j += String((int)zfSensor);
    j += ",\"offset\":"; cfgAppendNum(j, zfC, 6);
    j += ",\"slope\":";  cfgAppendNum(j, zfB, 6);
    j += ",\"r2\":";     cfgAppendNum(j, zfR2, 4);
    j += ",\"epoch\":";  j += String((unsigned long)zfLastEpoch); j += "}";
  }
  j += "}";
  j += "}";
  return j;
}

// Every "tables" entry, in export order: data the string-settings manifest can't carry. Values are comma-joined raw
// numbers (amps / watts / duty-% / counts); ripTab and huntPockets join their records with ';'. Both the Normal and
// Low cap tables are emitted regardless of the active HiLow mode. A table with nothing stored is omitted.
static const char *const CFG_TABLE_NAMES[] = {
  "rpmPoints", "capTable", "capPowerTable", "capTableLo", "capPowerTableLo", "minDutyTable",
  "kneeKnee", "kneeFitA", "kneeFloor", "kneeFrozen",
  "bk_minDuty", "bk_kneeFlr", "bk_kneeKn", "bk_kneeFrz", "bk_kneeFitA",
  "overheatCount", "lastOverheat", "cumulativeTime", "totalEvents", "totalOverheats", "totalSafeMs",
  "fuelRPM", "fuelGPH", "ripTab", "huntPockets", "zeroFit",
};

static void cfgJoinFloats(String &o, const float *a, int n, int dec) {
  for (int i = 0; i < n; i++) { if (i) o += ','; o += String(a[i], dec); }
}
static void cfgJoinBools(String &o, const bool *a, int n) {
  for (int i = 0; i < n; i++) { if (i) o += ','; o += (a[i] ? '1' : '0'); }
}

// Table `name` formatted as this regulator holds it now — the exported value, and the import's before/after
// comparison. "learning"/"fuel"-namespace blobs come from NVS; the ripple map, damper pockets, zero-drift fit and
// shunt gain from the live values their files and "storage" keys boot into. False = nothing stored.
static bool cfgTableValue(const char *name, String &o) {
  o = "";
  if (strcmp(name, "ripTab") == 0) {   // an empty map is a value too ("" = none): the importer ends up with none
    bool any = (ripTab.sess.levelA != 0.0f);
    for (int r = 0; r < RIPTAB_BINS && !any; r++) any = (ripTab.cell[r].state != 0);
    if (!any) return true;
    o = String(ripTab.sess.levelA, 2); o += ','; o += String(ripTab.sess.ibvMinV, 3); o += ','; o += String(ripTab.sess.ibvMaxV, 3);
    o += ','; o += String((unsigned)ripTab.sess.idleRpm); o += ','; o += String((unsigned long)ripTab.sess.epoch);
    for (int r = 0; r < RIPTAB_BINS; r++) {
      const RipTabCell &c = ripTab.cell[r];
      o += ';'; o += String((unsigned)c.altPkX100); o += ','; o += String((unsigned)c.battPkX100);
      o += ','; o += String((unsigned)c.altPendX100); o += ','; o += String((unsigned)c.battPendX100); o += ','; o += String((unsigned)c.state);
    }
    return true;
  }
  if (strcmp(name, "huntPockets") == 0) {   // an empty map is a value too: the importer ends up with none
    for (uint8_t i = 0; i < hgPocketN; i++) {
      if (i) o += ';';
      o += String(hgPockets[i].lo, 0); o += ','; o += String(hgPockets[i].hi, 0);
      o += ','; o += String((unsigned long)hgPockets[i].epoch); o += ','; o += String((unsigned)hgPockets[i].lever);
    }
    return true;
  }
  if (strcmp(name, "zeroFit") == 0) {
    o = String(zfValid); o += ','; o += String(zfSensor); o += ','; o += String(zfC, 6); o += ',';
    o += String(zfB, 6); o += ','; o += String(zfR2, 4); o += ','; o += String((unsigned long)zfLastEpoch);
    return true;
  }
  const bool fuel = (strcmp(name, "fuelRPM") == 0 || strcmp(name, "fuelGPH") == 0);
  nvs_handle_t h;
  if (nvs_open(fuel ? "fuel" : "learning", NVS_READONLY, &h) != ESP_OK) return false;
  bool ok = false;
  size_t sz;
  if (fuel) {
    float f[FUEL_TABLE_SIZE]; sz = sizeof(f);
    if ((ok = (nvs_get_blob(h, name, f, &sz) == ESP_OK && sz == sizeof(f)))) cfgJoinFloats(o, f, FUEL_TABLE_SIZE, name[4] == 'R' ? 1 : 3);
  } else if (strcmp(name, "rpmPoints") == 0 || strcmp(name, "overheatCount") == 0) {
    int v[RPM_TABLE_SIZE]; sz = sizeof(v);
    if ((ok = (nvs_get_blob(h, name, v, &sz) == ESP_OK && sz == sizeof(v))))
      for (int i = 0; i < RPM_TABLE_SIZE; i++) { if (i) o += ','; o += String(v[i]); }
  } else if (strcmp(name, "lastOverheat") == 0 || strcmp(name, "cumulativeTime") == 0) {
    unsigned long v[RPM_TABLE_SIZE]; sz = sizeof(v);
    if ((ok = (nvs_get_blob(h, name, v, &sz) == ESP_OK && sz == sizeof(v))))
      for (int i = 0; i < RPM_TABLE_SIZE; i++) { if (i) o += ','; o += String(v[i]); }
  } else if (strcmp(name, "totalEvents") == 0 || strcmp(name, "totalOverheats") == 0) {
    uint32_t v;
    if ((ok = (nvs_get_u32(h, name, &v) == ESP_OK))) o = String((unsigned long)v);
  } else if (strcmp(name, "totalSafeMs") == 0) {
    uint64_t v; sz = sizeof(v);
    if ((ok = (nvs_get_blob(h, name, &v, &sz) == ESP_OK && sz == sizeof(v)))) { char b[24]; snprintf(b, sizeof(b), "%llu", (unsigned long long)v); o = b; }
  } else if (strcmp(name, "kneeFitA") == 0 || strcmp(name, "bk_kneeFitA") == 0) {
    float v; sz = sizeof(v);
    if ((ok = (nvs_get_blob(h, name, &v, &sz) == ESP_OK && sz == sizeof(v)))) o = String(v, 3);
  } else if (strcmp(name, "kneeFrozen") == 0 || strcmp(name, "bk_kneeFrz") == 0) {
    bool v[RPM_TABLE_SIZE]; sz = sizeof(v);
    if ((ok = (nvs_get_blob(h, name, v, &sz) == ESP_OK && sz == sizeof(v)))) cfgJoinBools(o, v, RPM_TABLE_SIZE);
  } else {   // float[RPM_TABLE_SIZE] columns: cap tables, Keep-Alive column, knee floors and knees, their backups
    float v[RPM_TABLE_SIZE]; sz = sizeof(v);
    if ((ok = (nvs_get_blob(h, name, v, &sz) == ESP_OK && sz == sizeof(v)))) cfgJoinFloats(o, v, RPM_TABLE_SIZE, 2);
  }
  nvs_close(h);
  return ok;
}

String exportTablesObject() {
  String j;
  j.reserve(4096);
  j = "{";
  bool first = true;
  String v;
  for (size_t k = 0; k < sizeof(CFG_TABLE_NAMES) / sizeof(CFG_TABLE_NAMES[0]); k++) {
    if (!cfgTableValue(CFG_TABLE_NAMES[k], v)) continue;
    if (!first) j += ',';
    first = false;
    j += '"'; j += CFG_TABLE_NAMES[k]; j += "\":";
    cfgAppendJsonStr(j, v);
  }
  j += "}";
  return j;
}

// ── Commissioning status on import ───────────────────────────────────────────
// Values each wizard step writes or was measured against (stage -1 = input of every RPM-binned step 1..7). Per step:
// all match the file -> file's done/hand-set flags; all untouched -> keep ours; a mix -> not done. A value absent
// from the file matches only an absent value here. Prep (0) and the Stress test (8) have no rows.
struct CxImportKey { int8_t stage; const char *name; bool table; };
static const CxImportKey CX_IMPORT_KEYS[] = {
  { -1, "RPMScalingFactor", false },
  { 1, "SystemIDStabilizeAmps", false }, { 1, "SystemIDStepAmplitude", false },
  { 2, "PidKp", false }, { 2, "PidKi", false }, { 2, "OutputPIDFilterTC", false }, { 2, "VoltageFilterTC", false },
  { 2, "systemIDPlantTauMs", false },
  { 3, "PidKp", false }, { 3, "PidKi", false }, { 3, "PidKd", false },
  { 4, "ripFitAlt", false }, { 4, "slpFitAlt", false }, { 4, "IExcessTau", false }, { 4, "ripTab", true },
  { 5, "IExcessFrac", false }, { 5, "IExcessFracBulk", false }, { 5, "IExcessBaseA", false }, { 5, "IExcessCcOffsetA", false },
  { 5, "IExcessFloorA", false }, { 5, "IExcessCeilA", false }, { 5, "CvKdDeadbandVps", false }, { 5, "CvKdDbSlope", false },
  { 6, "cvPlantKa", false }, { 6, "cvPlantKb", false }, { 6, "cvGainMode", false }, { 6, "CommissionTempF", false },
  { 6, "CommissionTempSrc", false }, { 6, "BatteryVoltage", false }, { 6, "BatteryCapacity_Ah", false }, { 6, "battery_type", false },
  { 7, "rpmPoints", true }, { 7, "minDutyTable", true }, { 7, "fieldDecayTauMs", false }, { 7, "fdDrainLoMs", false },
  { 7, "fdDrainHiMs", false }, { 7, "fdDrainRpmLo", false }, { 7, "fdDrainRpmHi", false },
  { 9, "altLeadSec", false },
};
static const size_t CX_IMPORT_KEY_COUNT = sizeof(CX_IMPORT_KEYS) / sizeof(CX_IMPORT_KEYS[0]);
// Also sent in "cx_basis": speed-indexed tables an RPMScalingFactor change must wipe unless the file brings them.
static const char *const CFG_AXIS_BASIS_TABLES[] = { "huntPockets", "overheatCount" };

// /vessel_info.json view, built from the live globals so it matches what the loops run. Numbers stay JSON
// numbers: the dashboard form radio-selects on a numeric compare, so a quoted "0" orientation would not select.
String vesselInfoJson() {
  String j;
  j.reserve(768);
  j = "{\"regulator_name\":";          cfgAppendJsonStr(j, String(REGULATOR_NAME));
  j += ",\"boat_length_ft\":";          j += String(BOAT_LENGTH_FT, 2);
  j += ",\"boat_displacement_lbs\":";   j += String(BOAT_DISPLACEMENT_LBS, 0);
  j += ",\"boat_type\":";               cfgAppendJsonStr(j, BOAT_TYPE);
  j += ",\"boat_make_model\":";         cfgAppendJsonStr(j, BOAT_MAKE_MODEL);
  j += ",\"boat_year\":";               j += String((unsigned)BOAT_YEAR);
  j += ",\"home_port\":";               cfgAppendJsonStr(j, String(HOME_PORT));
  j += ",\"engine_make\":";             cfgAppendJsonStr(j, ENGINE_MAKE);
  j += ",\"engine_hp\":";               j += String((unsigned)ENGINE_HP);
  j += ",\"battery_voltage\":";         j += String((unsigned)SYSTEM_VOLTAGE_CLASS);
  j += ",\"field_winding_v\":";         j += String((unsigned)FieldWindingV);
  j += ",\"battery_capacity_ah\":";     j += String(BatteryCapacity_Ah);
  j += ",\"battery_type\":";            cfgAppendJsonStr(j, BATTERY_TYPE);
  j += ",\"battery_make_model\":";      cfgAppendJsonStr(j, BATTERY_MAKE_MODEL);
  j += ",\"alternator_brand_model\":";  cfgAppendJsonStr(j, ALTERNATOR_BRAND_MODEL);
  j += ",\"solar_watts\":";             j += String(SolarWatts);
  j += ",\"imu_mount_orientation\":";   j += String((unsigned)imuMountOrientation);
  j += ",\"imu_dist_bow_ft\":";         j += String(IMU_DIST_BOW_FT, 2);
  j += ",\"imu_dist_cl_ft\":";          j += String(IMU_DIST_CL_FT, 2);
  j += ",\"imu_height_wl_ft\":";        j += String(IMU_HEIGHT_WL_FT, 2);
  j += "}";
  return j;
}

String exportConfigJson() {
  String j;
  j.reserve(20480);   // config (manifest + learned-state blobs) + tables
  j = "{\"fw_version\":\"";
  j += FIRMWARE_VERSION;
  j += "\",\"source_uid\":\"";   // which regulator saved it: an import onto a unit the source lists as its partner pairs them back
  j += device_id_hex;
  j += "\",\"payload_v\":4,\"config\":";
  j += manifestConfigObject();
  j += ",\"tables\":";
  j += exportTablesObject();
  // Tier-3 rows: cfgDiffPreview (script.js) uses this so device state can't pose as an importable change.
  // Names only, never read by applyImportConfig, so inert to every importer.
  j += ",\"export_only\":[";
  bool first = true;
  for (size_t i = 0; i < CONFIG_MANIFEST_COUNT; i++) {
    if (CONFIG_MANIFEST[i].tier != 3) continue;
    if (!first) j += ',';
    first = false;
    j += '"'; j += CONFIG_MANIFEST[i].param; j += '"';
  }
  // The values applyImportCommissioning compares step by step, and the speed-indexed tables an engine-speed rescale
  // checks: the preview sends the file's value of each in "cx_basis", ticked or not. Names only, like export_only.
  j += "],\"cx_basis_keys\":[";
  first = true;
  for (size_t i = 0; i < CX_IMPORT_KEY_COUNT; i++) {
    bool dup = false;
    for (size_t k = 0; k < i; k++) if (strcmp(CX_IMPORT_KEYS[k].name, CX_IMPORT_KEYS[i].name) == 0) dup = true;
    if (dup) continue;
    if (!first) j += ',';
    first = false;
    j += '"'; j += CX_IMPORT_KEYS[i].name; j += '"';
  }
  for (size_t i = 0; i < sizeof(CFG_AXIS_BASIS_TABLES) / sizeof(CFG_AXIS_BASIS_TABLES[0]); i++) {
    j += ",\""; j += CFG_AXIS_BASIS_TABLES[i]; j += '"';
  }
  j += ",\"AlternatorCOffset\",\"zeroFit\"";   // and the zero-drift fit with the offset it is on: which of the pair the file brought
  j += "]}";
  return j;
}

// Whole trimmed string parses with strtof and is finite in float precision (rejects "nan", "infx", "1e39", "").
// Use only where the consumer is a float, never on ids or names.
bool floatTokenValid(const char *s) {
  if (!s) return false;
  while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
  if (!*s) return false;
  char *end = nullptr;
  float v = strtof(s, &end);
  if (end == s) return false;
  while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
  if (*end != '\0') return false;
  return isfinite(v) != 0;
}
// A config-import row is numeric when the value it REPLACES parses as a number; a text row (a boat named
// "Infinity", an SSID, an id) is never judged. New keys with nothing stored are left alone as well.
static bool importRowIsNumeric(const char *nvsKey) {
  String cur = settingRead(nvsKey);
  return cur.length() > 0 && floatTokenValid(cur.c_str());
}

// Number-shaped but not finite in float precision ("1e39", "nan", "-inf", "nanx"). Text and blanks are not
// number-shaped, so a text row that happens to hold "1998" still accepts a name or an empty value.
static bool importValueNonFinite(const char *s) {
  while (*s == ' ' || *s == '\t') s++;
  const char *t = (*s == '+' || *s == '-') ? s + 1 : s;
  if (strncasecmp(t, "nan", 3) == 0 || strncasecmp(t, "inf", 3) == 0) return true;
  char *end = nullptr;
  double v = strtod(s, &end);
  if (end == s) return false;
  while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
  return *end == '\0' && !isfinite((float)v);
}

// Decode a JSON string body starting just after its opening quote into val; returns the character after the
// closing quote. cfgAppendJsonStr produces \" \\ and \u00XX — e.g. the imu_zero value is itself a JSON string with
// quotes, and the pair list separates records with control characters.
static const char *cfgJsonStrDecode(const char *p, String &val) {
  val = "";
  while (*p && *p != '"') {
    if (*p != '\\' || !p[1]) { val += *p++; continue; }
    p++;
    switch (*p) {
      case 'n': val += '\n'; p++; break;
      case 'r': val += '\r'; p++; break;
      case 't': val += '\t'; p++; break;
      case 'b': val += '\b'; p++; break;
      case 'f': val += '\f'; p++; break;
      case 'u': {
        unsigned cp = 0;
        int i = 1;
        for (; i <= 4 && isxdigit((unsigned char)p[i]); i++) cp = cp * 16 + (unsigned)(isdigit((unsigned char)p[i]) ? p[i] - '0' : (tolower((unsigned char)p[i]) - 'a' + 10));
        if (i <= 4) { val += 'u'; p++; break; }   // malformed: keep the letter
        p += 5;
        if (cp == 0) break;                       // a NUL cannot live inside a String
        if (cp < 0x80) val += (char)cp;
        else if (cp < 0x800) { val += (char)(0xC0 | (cp >> 6)); val += (char)(0x80 | (cp & 0x3F)); }
        else { val += (char)(0xE0 | (cp >> 12)); val += (char)(0x80 | ((cp >> 6) & 0x3F)); val += (char)(0x80 | (cp & 0x3F)); }
        break;
      }
      default: val += *p++; break;                // \" \\ \/
    }
  }
  return *p ? p + 1 : p;
}

static bool cfgJsonExtract(const char *from, const char *key, String &val) {
  String needle = "\"";
  needle += key;
  needle += "\"";
  // Only a match followed by ':' is the key: a text value spelled like a key name ("engine_make":"Beta") is not.
  const char *p = nullptr;
  for (const char *q = strstr(from, needle.c_str()); q && !p; q = strstr(q + 1, needle.c_str())) {
    const char *c = q + needle.length();
    while (*c == ' ' || *c == '\t' || *c == '\r' || *c == '\n') c++;
    if (*c == ':') p = c;
  }
  if (!p) return false;
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':') p++;
  if (*p == '"') {
    cfgJsonStrDecode(p + 1, val);
    return true;
  }
  const char *e = p;
  while (*e && *e != ',' && *e != '}' && *e != ' ' && *e != '\n' && *e != '\r' && *e != '\t') e++;
  val = ""; val.concat(p, e - p);
  return true;
}

// A copy of the JSON object that is the value of key `name` (first occurrence), so lookups stay inside it: a key
// missing from "config" must never be picked up from "tables", "cx_basis" or a later array. Caller frees.
// An admin push wraps a whole export blob in its own "config", so a "config" holding a "config" object is descended.
static char *cfgObjectDup(const char *body, const char *name) {
  String needle = "\"";
  needle += name;
  needle += "\"";
  const char *start = nullptr;
  for (const char *p = strstr(body, needle.c_str()); p && !start; p = strstr(p + 1, needle.c_str())) {
    const char *q = p + needle.length();
    while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
    if (*q != ':') continue;
    q++;
    while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
    if (*q == '{') start = q;
  }
  if (!start) return nullptr;
  int depth = 0;
  bool inStr = false;
  const char *e = start;
  for (; *e; e++) {
    if (inStr) {
      if (*e == '\\' && e[1]) e++;
      else if (*e == '"') inStr = false;
      continue;
    }
    if (*e == '"') inStr = true;
    else if (*e == '{') depth++;
    else if (*e == '}' && --depth == 0) { e++; break; }
  }
  if (depth != 0) return nullptr;
  size_t n = (size_t)(e - start);
  char *buf = (char *)ps_malloc(n + 1);
  if (!buf) return nullptr;
  memcpy(buf, start, n);
  buf[n] = '\0';
  if (strcmp(name, "config") == 0) {
    char *inner = cfgObjectDup(buf + 1, "config");
    if (inner) { free(buf); return inner; }
  }
  return buf;
}

// Strict comma-joined parsers for the "tables" section — exactly n values or reject.
static bool cfgCsvToFloat(const String &s, float *out, int n) {
  int pos = 0;
  for (int i = 0; i < n; i++) {
    if (pos >= (int)s.length()) return false;
    out[i] = atof(s.c_str() + pos);
    int c = s.indexOf(',', pos);
    if (c < 0) return i == n - 1;
    if (i == n - 1) return false;
    pos = c + 1;
  }
  return true;
}
static bool cfgCsvToInt(const String &s, int *out, int n) {
  int pos = 0;
  for (int i = 0; i < n; i++) {
    if (pos >= (int)s.length()) return false;
    out[i] = atoi(s.c_str() + pos);
    int c = s.indexOf(',', pos);
    if (c < 0) return i == n - 1;
    if (i == n - 1) return false;
    pos = c + 1;
  }
  return true;
}

// What this import wrote into each table, by CFG_TABLE_NAMES index — the "after" side of the commissioning step
// comparison for tables whose live value only reloads at the next boot (staged files, "storage" keys).
static String (&cfgImpTblVal)[sizeof(CFG_TABLE_NAMES) / sizeof(CFG_TABLE_NAMES[0])] = PsramPlace::make<String[sizeof(CFG_TABLE_NAMES) / sizeof(CFG_TABLE_NAMES[0])]>();
static bool cfgImpTblSet[sizeof(CFG_TABLE_NAMES) / sizeof(CFG_TABLE_NAMES[0])];
static void cfgImpNoteTable(const char *name, const String &v) {
  for (size_t k = 0; k < sizeof(CFG_TABLE_NAMES) / sizeof(CFG_TABLE_NAMES[0]); k++)
    if (strcmp(CFG_TABLE_NAMES[k], name) == 0) { cfgImpTblVal[k] = v; cfgImpTblSet[k] = true; return; }
}
// Table `name` as this regulator will hold it after the import's reboot.
static bool cfgTableValueAfter(const char *name, String &o) {
  for (size_t k = 0; k < sizeof(CFG_TABLE_NAMES) / sizeof(CFG_TABLE_NAMES[0]); k++)
    if (strcmp(CFG_TABLE_NAMES[k], name) == 0 && cfgImpTblSet[k]) { o = cfgImpTblVal[k]; return true; }
  return cfgTableValue(name, o);
}

static bool cfgCsvToULong(const String &s, unsigned long *out, int n) {
  int pos = 0;
  for (int i = 0; i < n; i++) {
    if (pos >= (int)s.length()) return false;
    out[i] = strtoul(s.c_str() + pos, nullptr, 10);
    int c = s.indexOf(',', pos);
    if (c < 0) return i == n - 1;
    if (i == n - 1) return false;
    pos = c + 1;
  }
  return true;
}
static bool cfgCsvToBool(const String &s, bool *out, int n) {
  int v[RPM_TABLE_SIZE];
  if (n > RPM_TABLE_SIZE || !cfgCsvToInt(s, v, n)) return false;
  for (int i = 0; i < n; i++) out[i] = (v[i] != 0);
  return true;
}

// "levelA,ibvMin,ibvMax,idleRpm,epoch;" then RIPTAB_BINS cells "altPk,battPk,altPend,battPend,state" — cfgTableValue's layout.
// `out` is a RipTab: a struct-typed parameter would trip the Arduino auto-prototype hoisting above its definition.
static bool cfgParseRipTab(const String &s, void *out) {
  RipTab &t = *(RipTab *)out;
  memset(&t, 0, sizeof(t));
  int semi = s.indexOf(';');
  if (semi < 0) return false;
  float f[3];
  unsigned long u[2];
  if (sscanf(s.substring(0, semi).c_str(), "%f,%f,%f,%lu,%lu", &f[0], &f[1], &f[2], &u[0], &u[1]) != 5) return false;
  t.sess.levelA = f[0]; t.sess.ibvMinV = f[1]; t.sess.ibvMaxV = f[2];
  t.sess.idleRpm = (uint16_t)u[0]; t.sess.epoch = (uint32_t)u[1];
  int pos = semi + 1;
  for (int r = 0; r < RIPTAB_BINS; r++) {
    int end = s.indexOf(';', pos);
    if (end < 0) end = s.length();
    unsigned a, b, c, d, st;
    if (sscanf(s.substring(pos, end).c_str(), "%u,%u,%u,%u,%u", &a, &b, &c, &d, &st) != 5) return false;
    t.cell[r].altPkX100 = (uint16_t)a; t.cell[r].battPkX100 = (uint16_t)b;
    t.cell[r].altPendX100 = (uint16_t)c; t.cell[r].battPendX100 = (uint16_t)d; t.cell[r].state = (uint8_t)st;
    pos = end + 1;
  }
  return true;
}

// Files the loop rewrites from RAM, so an import writes these copies instead; importStagedCommit moves them over the
// live files at the next boot, before any loader opens them.
static const struct { const char *staged; const char *live; } CFG_STAGED_FILES[] = {
  { "/imp_riptab.bin", RIPTAB_PATH },
  { "/imp_huntpockets.csv", "/huntpockets.csv" },
};

static void cfgImportNoteChanged(const char *name);

// Push receipt: the tables an owner sets by hand. Learned or accumulated ones (knee state, overheat history, ripple map,
// damper pockets, zero fit, bk_* backups) move on their own and would raise the popup on every push.
static const char *const CFG_OWNER_TABLES[] = {
  "rpmPoints", "capTable", "capPowerTable", "capTableLo", "capPowerTableLo", "minDutyTable", "fuelRPM", "fuelGPH",
};
static const size_t CFG_OWNER_TABLE_COUNT = sizeof(CFG_OWNER_TABLES) / sizeof(CFG_OWNER_TABLES[0]);
static String (&cfgOwnerTblPre)[CFG_OWNER_TABLE_COUNT] = PsramPlace::make<String[CFG_OWNER_TABLE_COUNT]>();
static bool cfgOwnerTblPreOk[CFG_OWNER_TABLE_COUNT];

// Before anything in the import writes (the class conversion resets the Keep-Alive column). Only while collecting.
static void cfgOwnerTablesCapturePre() {
  if (!cfgImportChangedNames) return;
  for (size_t k = 0; k < CFG_OWNER_TABLE_COUNT; k++) cfgOwnerTblPreOk[k] = cfgTableValue(CFG_OWNER_TABLES[k], cfgOwnerTblPre[k]);
}

// `fileVal` is the parsed table re-joined the way cfgTableValue formats it, so a re-pushed export reads unchanged.
static void cfgImportNoteTableChanged(const char *name, const String &fileVal) {
  if (!cfgImportChangedNames) return;
  // With the learner on, the Keep-Alive column is its learned floors, not a hand-set table.
  if (!strcmp(name, "minDutyTable") && settingRead(NK_kneeLearnEnable).toInt() != 0) return;
  for (size_t k = 0; k < CFG_OWNER_TABLE_COUNT; k++) {
    if (strcmp(CFG_OWNER_TABLES[k], name) != 0) continue;
    if (!cfgOwnerTblPreOk[k] || cfgOwnerTblPre[k] != fileVal) cfgImportNoteChanged(name);
    return;
  }
}

// Apply the "tables" section (`tbl` = that object alone). Moved rpmPoints reset the knee tracker and the moved bins'
// overheat history as a manual breakpoint edit does, BEFORE the imported minDutyTable lands. bk_* blobs belong to
// applyImportCommissioning. Live arrays reload at the end for noReboot=1.
static int applyImportTables(const char *tbl, float zfShiftA) {
  if (!tbl) return 0;
  String val;
  int pts[RPM_TABLE_SIZE];
  bool havePts = cfgJsonExtract(tbl, "rpmPoints", val) && cfgCsvToInt(val, pts, RPM_TABLE_SIZE);
  if (havePts && cfgImportChangedNames) {
    String o;
    for (int i = 0; i < RPM_TABLE_SIZE; i++) { if (i) o += ','; o += String(pts[i]); }
    cfgImportNoteTableChanged("rpmPoints", o);
  }
  bool moved = false;
  if (havePts && clearMovedRpmBins(rpmTableRPMPoints, pts)) {   // imported breakpoints moved → Min% floor + Field decay stage stale
    moved = true;
    kneeLearnResetDefaults();
    commissionClearStage(7);
  }
  int applied = 0;
  float impMinDuty[RPM_TABLE_SIZE];
  bool haveMinDuty = false;
  bool haveOverheat = false;
  nvs_handle_t h;
  if (nvs_open("learning", NVS_READWRITE, &h) == ESP_OK) {
    if (havePts) { nvs_set_blob(h, "rpmPoints", pts, sizeof(pts)); applied++; }
    static const char *CFG_TABLE_FKEYS[] = { "capTable", "capPowerTable", "capTableLo", "capPowerTableLo", "minDutyTable" };
    for (size_t k = 0; k < sizeof(CFG_TABLE_FKEYS)/sizeof(CFG_TABLE_FKEYS[0]); k++) {
      float f[RPM_TABLE_SIZE];
      if (!cfgJsonExtract(tbl, CFG_TABLE_FKEYS[k], val) || !cfgCsvToFloat(val, f, RPM_TABLE_SIZE)) continue;
      if (cfgImportChangedNames) { String o; cfgJoinFloats(o, f, RPM_TABLE_SIZE, 2); cfgImportNoteTableChanged(CFG_TABLE_FKEYS[k], o); }
      nvs_set_blob(h, CFG_TABLE_FKEYS[k], f, sizeof(f));
      if (!strcmp(CFG_TABLE_FKEYS[k], "minDutyTable")) { memcpy(impMinDuty, f, sizeof(f)); haveMinDuty = true; }
      applied++;
    }
    // Per-speed overheat history — one record, all six parts or none.
    int oc[RPM_TABLE_SIZE];
    unsigned long lo[RPM_TABLE_SIZE], ct[RPM_TABLE_SIZE];
    String ev, to, sm;
    if (cfgJsonExtract(tbl, "overheatCount", val) && cfgCsvToInt(val, oc, RPM_TABLE_SIZE)
        && cfgJsonExtract(tbl, "lastOverheat", val) && cfgCsvToULong(val, lo, RPM_TABLE_SIZE)
        && cfgJsonExtract(tbl, "cumulativeTime", val) && cfgCsvToULong(val, ct, RPM_TABLE_SIZE)
        && cfgJsonExtract(tbl, "totalEvents", ev) && cfgJsonExtract(tbl, "totalOverheats", to) && cfgJsonExtract(tbl, "totalSafeMs", sm)) {
      memcpy(overheatCount, oc, sizeof(oc));
      memcpy(lastOverheatTime, lo, sizeof(lo));
      memcpy(cumulativeNoOverheatTime, ct, sizeof(ct));
      totalLearningEvents = strtoul(ev.c_str(), nullptr, 10);
      totalOverheats = strtoul(to.c_str(), nullptr, 10);
      totalSafeMs = strtoull(sm.c_str(), nullptr, 10);
      haveOverheat = true;
    }
    if (applied) nvs_commit(h);
    nvs_close(h);
  }
  if (haveOverheat) { saveHistoricalDataImmediate(); applied++; }
  else if (moved) saveHistoricalDataImmediate();   // the cleared bins must survive the import's reboot
  if (haveMinDuty) {
    // The learner's own per-bin state, when the file has it, installs as-is; an older file without it gets the
    // column as commissioning would leave it. Without the latter the boot rebuild restores the learner's old column.
    float kn[RPM_TABLE_SIZE], fl[RPM_TABLE_SIZE];
    bool fz[RPM_TABLE_SIZE];
    bool haveKnee = cfgJsonExtract(tbl, "kneeKnee", val) && cfgCsvToFloat(val, kn, RPM_TABLE_SIZE);
    float fitA = cfgJsonExtract(tbl, "kneeFitA", val) ? val.toFloat() : 0.0f;
    bool haveLearner = haveKnee && cfgJsonExtract(tbl, "kneeFloor", val) && cfgCsvToFloat(val, fl, RPM_TABLE_SIZE)
                       && cfgJsonExtract(tbl, "kneeFrozen", val) && cfgCsvToBool(val, fz, RPM_TABLE_SIZE);
    if (haveLearner) kneeAdoptLearnedState(impMinDuty, fl, kn, fz, fitA);
    else kneeAdoptCommittedTable(impMinDuty, haveKnee ? kn : nullptr, fitA);
  }
  // Fuel curve — separate "fuel" namespace + FUEL_TABLE_SIZE arrays. Both halves must
  // parse or neither is written (a half-updated curve would interpolate wrong).
  float fr[FUEL_TABLE_SIZE], fg[FUEL_TABLE_SIZE];
  if (cfgJsonExtract(tbl, "fuelRPM", val) && cfgCsvToFloat(val, fr, FUEL_TABLE_SIZE) &&
      cfgJsonExtract(tbl, "fuelGPH", val) && cfgCsvToFloat(val, fg, FUEL_TABLE_SIZE)) {
    nvs_handle_t fh;
    if (nvs_open("fuel", NVS_READWRITE, &fh) == ESP_OK) {
      if (cfgImportChangedNames) {
        String o; cfgJoinFloats(o, fr, FUEL_TABLE_SIZE, 1); cfgImportNoteTableChanged("fuelRPM", o);
        o = "";   cfgJoinFloats(o, fg, FUEL_TABLE_SIZE, 3); cfgImportNoteTableChanged("fuelGPH", o);
      }
      nvs_set_blob(fh, "fuelRPM", fr, sizeof(fr));
      nvs_set_blob(fh, "fuelGPH", fg, sizeof(fg));
      nvs_commit(fh);
      nvs_close(fh);
      memcpy(fuelTableRPM, fr, sizeof(fr));   // live arrays for the noReboot=1 path
      memcpy(fuelTableGPH, fg, sizeof(fg));
      applied += 2;
    }
  }
  // Ripple map (Disturbances step evidence) and damper pockets: staged files.
  if (cfgJsonExtract(tbl, "ripTab", val)) {
    RipTab t;
    if (val.length() == 0) memset(&t, 0, sizeof(t));
    if ((val.length() == 0 || cfgParseRipTab(val, &t)) && writePsramBlob(CFG_STAGED_FILES[0].staged, RIPTAB_MAGIC, RIPTAB_VER, 0, &t, sizeof(RipTab), 1, 0, 1) == 1) {
      cfgImpNoteTable("ripTab", val);
      applied++;
    }
  }
  if (cfgJsonExtract(tbl, "huntPockets", val)) {
    fsTakeLock();
    File f = LittleFS.open(CFG_STAGED_FILES[1].staged, "w");
    if (f) {
      int pos = 0;
      while (pos < (int)val.length()) {
        int end = val.indexOf(';', pos);
        if (end < 0) end = val.length();
        f.print(val.substring(pos, end));
        f.print('\n');
        pos = end + 1;
      }
      f.close();
      fsFreeDirty = true;
      cfgImpNoteTable("huntPockets", val);
      applied++;
    }
    fsReleaseLock();
  }
  // Zero-drift fit: "storage"-namespace keys written behind the live values. saveNVSDataFull only writes a key whose
  // live value moved, so these survive to the reboot that loads them.
  {
    String zf;
    int zv = 0, zs = 0;
    float zc = 0, zb = 0, zr = 0;
    unsigned long ze = 0;
    const bool haveZf = cfgJsonExtract(tbl, "zeroFit", zf) && sscanf(zf.c_str(), "%d,%d,%f,%f,%f,%lu", &zv, &zs, &zc, &zb, &zr, &ze) == 6
                        && isfinite(zc) && isfinite(zb) && isfinite(zr);
    nvs_handle_t sh;
    if (haveZf && nvs_open("storage", NVS_READWRITE, &sh) == ESP_OK) {
      if (zv != 0 && zfShiftA != 0.0f) {   // zfShiftA: see applyImportConfig
        zc += zfShiftA;
        zf = String(zv) + ',' + String(zs) + ',' + String(zc, 6) + ',' + String(zb, 6) + ',' + String(zr, 4) + ',' + String(ze);
        queueConsoleMessageF("IMPORT: the imported zero correction moved %+.2f A onto this regulator's current offset", zfShiftA);
      }
      uint32_t ze32 = (uint32_t)ze;
      nvs_set_i32(sh, "ZFitValid", zv);
      nvs_set_i32(sh, "ZFitSensor", zs);
      nvs_set_blob(sh, "ZFitC", &zc, sizeof(float));
      nvs_set_blob(sh, "ZFitB", &zb, sizeof(float));
      nvs_set_blob(sh, "ZFitR2", &zr, sizeof(float));
      nvs_set_u32(sh, "ZFitEpoch", ze32);
      cfgImpNoteTable("zeroFit", zf);
      applied++;
      nvs_commit(sh);
      nvs_close(sh);
    }
  }
  if (applied) loadLearningTableFromNVS();
  return applied;
}

// Battery class / capacity / chemistry changed: the commissioned tune no longer describes the bank. Shared by
// /saveVesselInfo and applyImportConfig; each caller does its own change detection.
void raiseRecommissionNag() {
  commissionChangeFlag = true;
  settingWrite(NK_cmChangeFlag, "1");
}

static const size_t CFGPUSH_KEYS_CAP = 240;   // bounds the NVS receipt + the /configPush body

static void cfgImportNoteChanged(const char *name) {
  if (!cfgImportChangedNames) return;
  cfgImportChangedCount++;
  if (cfgImportChangedNames->length() + strlen(name) + 1 > CFGPUSH_KEYS_CAP) return;   // silently stop at the cap; the count is still exact
  if (cfgImportChangedNames->length()) *cfgImportChangedNames += ',';
  *cfgImportChangedNames += name;
}

// Asked BEFORE the write: settingWrite returns true for an identical value too. No NVS read unless names are being collected.
static bool cfgImportWillChange(const char *key, const String &val) {
  if (!cfgImportChangedNames) return false;
  const String cur = settingRead(key);
  return cur != val || (cur.length() == 0 && !settingExists(key));
}

// Renamed params: import matches the current manifest name only, so an old-name blob would silently drop the
// setting. One row per rename; the NVS key never changes, only the JSON param name.
static const struct { const char *now; const char *legacy; } CFG_PARAM_ALIASES[] = {
  { "timeSourceMode", "gpsTimeSourceMode" },   // position/time split
};


static String (&cxImpPre)[CX_IMPORT_KEY_COUNT] = PsramPlace::make<String[CX_IMPORT_KEY_COUNT]>();
static String cxImpPreAxis[sizeof(CFG_AXIS_BASIS_TABLES) / sizeof(CFG_AXIS_BASIS_TABLES[0])];
static uint16_t cxImpPreDone, cxImpPreMan;
static uint8_t cxImpPreState;
static time_t cxImpPreEpoch;
static bool cxImpPreAgeAck, cxImpPreChange;
static String cxImpPreStress;

static const char *cfgManifestNvsKey(const char *param) {
  for (size_t i = 0; i < CONFIG_MANIFEST_COUNT; i++)
    if (strcmp(CONFIG_MANIFEST[i].param, param) == 0) return CONFIG_MANIFEST[i].nvsKey;
  return nullptr;
}

// A step value as this regulator holds it: before the import (after=false) or as it will be after its reboot.
static String cxImportValue(const char *name, bool table, bool after) {
  String o;
  if (table) {
    if (after) cfgTableValueAfter(name, o);
    else cfgTableValue(name, o);
    return o;
  }
  const char *nk = cfgManifestNvsKey(name);
  if (nk) return settingRead(nk);
  char key[16];
  snprintf(key, sizeof(key), "%s", name);   // registry knob: NVS key = name truncated to the 15-char cap
  return settingRead(key);
}

// The file's value: the preview sends every step value the file holds in "cx_basis", ticked or not, so an unticked
// row still counts as the file's. A body without it (bench POST of a whole file, admin push) is its own basis.
static bool cxImportBasis(const char *basis, const char *cfg, const char *tbl, const char *name, bool table, String &v) {
  if (basis && cfgJsonExtract(basis, name, v)) return true;
  if (table) return tbl && cfgJsonExtract(tbl, name, v);
  return cfgJsonExtract(cfg, name, v);
}

// Same value: exact text, or the same finite number written with different precision.
static bool cxImportSame(const String &a, const String &b) {
  if (a == b) return true;
  if (!floatTokenValid(a.c_str()) || !floatTokenValid(b.c_str())) return false;
  float x = a.toFloat(), y = b.toFloat();
  return fabsf(x - y) <= 1e-5f * fmaxf(1.0f, fmaxf(fabsf(x), fabsf(y)));
}

static void cxImportCapturePre() {
  for (size_t i = 0; i < CX_IMPORT_KEY_COUNT; i++) cxImpPre[i] = cxImportValue(CX_IMPORT_KEYS[i].name, CX_IMPORT_KEYS[i].table, false);
  for (size_t i = 0; i < sizeof(CFG_AXIS_BASIS_TABLES) / sizeof(CFG_AXIS_BASIS_TABLES[0]); i++)
    cxImpPreAxis[i] = cxImportValue(CFG_AXIS_BASIS_TABLES[i], true, false);
  cxImpPreDone = commissionDoneMask;
  cxImpPreMan = commissionManualMask;
  cxImpPreState = commissionState;
  cxImpPreEpoch = CommissionEpoch;
  cxImpPreAgeAck = commissionAgeAck;
  cxImpPreChange = commissionChangeFlag;
  cxImpPreStress = String(cvsLastBlob);
}

static void cxImportSetStress(const String &blob) {
  if (blob.length() && blob.length() < sizeof(cvsLastBlob)) {
    strlcpy(cvsLastBlob, blob.c_str(), sizeof(cvsLastBlob));
    settingWrite(NK_cvStressLast, cvsLastBlob);
  } else {
    cvsLastBlob[0] = '\0';
    settingRemove(NK_cvStressLast);
  }
}

static void cxImportSetEpoch(time_t e) {
  CommissionEpoch = e;
  if (e > 0) {
    char b[24];
    snprintf(b, sizeof(b), "%lld", (long long)e);
    settingWrite(NK_CommissionEpoch, b);
  } else {
    settingRemove(NK_CommissionEpoch);
  }
}

static void cxLedgerAppend(const char *eventType, int stage, const String &dataJson);

// Applies the file's commissioning status when the body carries it (the preview's "Commissioning status" row).
// Without it the regulator keeps its own status and the import's own change checks apply as before.
static bool applyImportCommissioning(const char *cfg, const char *tbl, const char *basis) {
  String v;
  if (!cfgJsonExtract(cfg, "commissionDoneMask", v)) return false;
  const uint16_t fDone = (uint16_t)v.toInt();
  const uint16_t fMan = cfgJsonExtract(cfg, "commissionManualMask", v) ? (uint16_t)v.toInt() : 0;
  enum { CX_KEEP = 0, CX_FILE = 1, CX_NONE = 2, CX_EITHER = 3 };
  uint8_t src[COMMISSION_STAGE_COUNT];
  bool fileIsPre[COMMISSION_STAGE_COUNT];   // the file's values for this step equal this regulator's pre-import ones
  bool keyed[COMMISSION_STAGE_COUNT];       // the step has values of its own in CX_IMPORT_KEYS
  for (int s = 0; s < COMMISSION_STAGE_COUNT; s++) {
    bool matchFile = true, unchanged = true, same = true, any = false;
    for (size_t i = 0; i < CX_IMPORT_KEY_COUNT; i++) {
      const CxImportKey &k = CX_IMPORT_KEYS[i];
      if (!(k.stage == s || (k.stage == -1 && s >= 1 && s <= 7))) continue;
      any = true;
      String now = cxImportValue(k.name, k.table, true), b;
      if (!cxImportBasis(basis, cfg, tbl, k.name, k.table, b)) b = "";   // not in the file: only an absent value here matches it
      if (!cxImportSame(now, b)) matchFile = false;
      if (!cxImportSame(now, cxImpPre[i])) unchanged = false;
      if (!cxImportSame(b, cxImpPre[i])) same = false;
    }
    src[s] = matchFile ? CX_FILE : (unchanged ? CX_KEEP : CX_NONE);
    fileIsPre[s] = same;
    keyed[s] = any;
  }
  // A step measured on an upstream step's values is only good while those are in force: an upstream from the other
  // source (that actually differs) leaves the dependent not done. A dependent whose own values match in both sources
  // follows the source of its first such upstream; a later one from the other source leaves it not done.
  uint16_t depPinned = 0;
  for (int u = 0; u < COMMISSION_STAGE_COUNT; u++) {
    const uint16_t deps = commissionDependentsMask(u);
    for (int d = 0; d < COMMISSION_STAGE_COUNT; d++) {
      const uint16_t bd = (uint16_t)(1u << d);
      if (!keyed[d] || !(deps & bd) || fileIsPre[u] || src[d] == CX_NONE) continue;
      if (src[u] == src[d]) { depPinned |= bd; continue; }
      if (fileIsPre[d] && !(depPinned & bd) && src[u] != CX_NONE) { src[d] = src[u]; depPinned |= bd; }
      else src[d] = CX_NONE;
    }
  }
  // A step with no values of its own follows the steps it rests on: the Stress test its upstream steps, Prep every
  // step. An upstream whose file values equal this regulator's fits either source. Upstreams from both sources leave
  // the Stress test not done; Prep stays done if the source whose status it takes had it done (CX_EITHER).
  bool hasUp[COMMISSION_STAGE_COUNT];
  for (int s = 0; s < COMMISSION_STAGE_COUNT; s++) {
    hasUp[s] = false;
    for (int u = 0; u < COMMISSION_STAGE_COUNT; u++) if (commissionDependentsMask(u) & (1u << s)) hasUp[s] = true;
  }
  for (int s = 0; s < COMMISSION_STAGE_COUNT; s++) {
    if (keyed[s]) continue;
    bool fileOk = true, keepOk = true;
    for (int u = 0; u < COMMISSION_STAGE_COUNT; u++) {
      if (u == s || !keyed[u] || (hasUp[s] && !(commissionDependentsMask(u) & (1u << s)))) continue;
      if (src[u] == CX_FILE && fileIsPre[u]) continue;
      if (src[u] != CX_FILE) fileOk = false;
      if (src[u] != CX_KEEP) keepOk = false;
    }
    src[s] = fileOk ? CX_FILE : keepOk ? CX_KEEP : hasUp[s] ? CX_NONE : CX_EITHER;
  }
  uint16_t done = 0, man = 0, fromFile = 0;
  bool allFile = true, allKeep = true, anyFileDone = false, anyKeepDone = false;
  for (int s = 0; s < COMMISSION_STAGE_COUNT; s++) {
    const uint16_t bit = (uint16_t)(1u << s);
    const bool measured = keyed[s] || hasUp[s];   // Prep measures nothing, so it never dates the reminder
    if (src[s] == CX_FILE) { done |= fDone & bit; man |= fMan & bit; fromFile |= bit; if ((fDone & bit) && measured) anyFileDone = true; }
    else if (src[s] == CX_KEEP) { done |= cxImpPreDone & bit; man |= cxImpPreMan & bit; if ((cxImpPreDone & bit) && measured) anyKeepDone = true; }
    else if (src[s] == CX_EITHER) {
      if (fDone & bit) { done |= bit; man |= fMan & bit; }
      else if (cxImpPreDone & bit) { done |= bit; man |= cxImpPreMan & bit; }
    }
    if (src[s] != CX_FILE) allFile = false;
    if (src[s] != CX_KEEP) allKeep = false;
  }
  commissionDoneMask = done; commissionWriteDoneMask();
  commissionManualMask = man; commissionWriteManualMask();
  const time_t fEpoch = cfgJsonExtract(cfg, "CommissionEpoch", v) ? (time_t)strtoll(v.c_str(), nullptr, 10) : 0;
  const bool fAgeAck = cfgJsonExtract(cfg, "commissionAgeAck", v) && v.toInt() != 0;
  const bool fChange = cfgJsonExtract(cfg, "commissionChangeAck", v) && v.toInt() != 0;
  String fStress;
  cfgJsonExtract(cfg, "cvStressLast", fStress);
  static const struct { const char *param; const char *nvsKey; } RESTORE_POINTS[] = {
    { "commissionSnap", NK_commissionSnap }, { "commissionStepSnap", NK_commissionStepSnap }, { "commissionPreRun", NK_commissionPreRun },
  };
  if (allFile) {
    // Every step is the file's: the source regulator's lifecycle exactly, a wizard run in flight and its restore
    // points included (the next boot undoes the interrupted step as the source's would, Abort reverts to its tune).
    if (cfgJsonExtract(cfg, "commissionState", v)) commissionSetState((uint8_t)constrain((int)v.toInt(), 0, 2));
    else commissionRecomputeState();
    commissionSetPhase(cfgJsonExtract(cfg, "commissionPhase", v) ? (uint8_t)constrain((int)v.toInt(), 0, COMMISSION_STAGE_COUNT) : COMMISSION_STAGE_COUNT);
    for (auto &rp : RESTORE_POINTS) {
      if (cfgJsonExtract(cfg, rp.param, v) && v.length()) settingWrite(rp.nvsKey, v.c_str());
      else settingRemove(rp.nvsKey);
    }
    float m[RPM_TABLE_SIZE], f[RPM_TABLE_SIZE], kn[RPM_TABLE_SIZE], fa = 0.0f;
    bool z[RPM_TABLE_SIZE];
    if (tbl && cfgJsonExtract(tbl, "bk_minDuty", v) && cfgCsvToFloat(v, m, RPM_TABLE_SIZE)
        && cfgJsonExtract(tbl, "bk_kneeFlr", v) && cfgCsvToFloat(v, f, RPM_TABLE_SIZE)
        && cfgJsonExtract(tbl, "bk_kneeKn", v) && cfgCsvToFloat(v, kn, RPM_TABLE_SIZE)
        && cfgJsonExtract(tbl, "bk_kneeFrz", v) && cfgCsvToBool(v, z, RPM_TABLE_SIZE)
        && cfgJsonExtract(tbl, "bk_kneeFitA", v) && floatTokenValid(v.c_str())) {
      fa = v.toFloat();
      commissionBackupMinPct(m, f, kn, z, fa);
    } else {
      commissionClearMinPctBackup();
    }
    cxImportSetEpoch(fEpoch);
    commissionAgeAck = fAgeAck;
    commissionChangeFlag = fChange;
    cxImportSetStress(fStress);
  } else if (allKeep) {
    // No step took the file's status: this regulator's own lifecycle stands, a wizard run in flight included.
  } else {
    // A mix of the file's steps and this regulator's: neither run's restore points describe the result, the
    // reminder dates from the older of the two measurements in force, and either source's change flag stands.
    commissionRecomputeState();
    commissionSetPhase(COMMISSION_STAGE_COUNT);
    for (auto &rp : RESTORE_POINTS) settingRemove(rp.nvsKey);
    commissionClearMinPctBackup();
    time_t e = 0;
    if (anyFileDone && fEpoch > 0) e = fEpoch;
    if (anyKeepDone && cxImpPreEpoch > 0 && (e == 0 || cxImpPreEpoch < e)) e = cxImpPreEpoch;
    cxImportSetEpoch(e);
    commissionAgeAck = false;
    commissionChangeFlag = (anyFileDone && fChange) || (anyKeepDone && cxImpPreChange);
    if (src[8] == CX_FILE && (done & (1u << 8))) cxImportSetStress(fStress);
    else if (src[8] == CX_KEEP && (done & (1u << 8))) cxImportSetStress(cxImpPreStress);
    else cxImportSetStress(String());
  }
  settingWrite(NK_cmAgeAck, commissionAgeAck ? "1" : "0");
  settingWrite(NK_cmChangeFlag, commissionChangeFlag ? "1" : "0");
  String d = "{\"done_mask\":";
  d += String((int)done);
  d += ",\"manual_mask\":"; d += String((int)man);
  d += ",\"state\":"; d += String((int)commissionState);
  d += ",\"from_file_mask\":"; d += String((int)fromFile);
  d += ",\"file_done_mask\":"; d += String((int)fDone);
  d += '}';
  cxLedgerAppend("import", -1, d);
  if (done != cxImpPreDone || man != cxImpPreMan || commissionState != cxImpPreState || CommissionEpoch != cxImpPreEpoch)
    cfgImportNoteChanged("commissioning status");
  queueConsoleMessageF("IMPORT: commissioning status %s (steps done mask 0x%03X, %s)",
                       allFile ? "copied from the file" : allKeep ? "kept, no step came from the file" : "taken step by step", (unsigned)done,
                       commissionState == 2 ? "COMMISSIONED" : commissionState == 1 ? "in progress" : "not commissioned");
  return true;
}

// Boot-time work an import leaves behind (NK_impBootAct, hex IBA_*/AXW_* bits), ORed into whatever is already owed.
static void importBootActAdd(uint16_t bits) {
  uint16_t cur = (uint16_t)strtoul(settingRead(NK_impBootAct).c_str(), nullptr, 16);
  char b[8];
  snprintf(b, sizeof(b), "%x", (unsigned)(cur | bits));
  settingWrite(NK_impBootAct, b);
}

// setup(), once LittleFS is mounted and before any loader: move each file an import staged over its live copy.
static void recordsStagedCommit();
void importStagedCommit() {
  recordsStagedCommit();
  for (auto &sf : CFG_STAGED_FILES) {
    fsTakeLock();
    if (LittleFS.exists(sf.staged)) {
      LittleFS.remove(sf.live);
      if (!LittleFS.rename(sf.staged, sf.live)) LittleFS.remove(sf.staged);
      fsFreeDirty = true;
    }
    fsReleaseLock();
  }
}

// setup(), after the last loader: hand the import's boot-time work to loop() (importBootActRun).
void importBootActionsLoad() {
  if (!settingExists(NK_impBootAct)) return;
  pendingImportBootAct = (uint16_t)strtoul(settingRead(NK_impBootAct).c_str(), nullptr, 16);
  if (!pendingImportBootAct) settingRemove(NK_impBootAct);
}

// loop(), first pass after the import's reboot; a reboot before this repeats it (every part is idempotent). The record
// goes here, while the matrix, ripple-map and flipbook wipes are still flags their own services act on shortly after.
void importBootActRun(uint16_t act) {
  if (act & AXW_ALL) rpmAxisWipeLocal(act & AXW_ALL);
  if (act & IBA_PERF) pendingResetBoatPerformance = true;
  settingRemove(NK_impBootAct);
}

// ── Equipment records ─────────────────────────────────────────────────────────
// The boat's own history (/exportRecords, /importRecords): record files byte for byte, their NVS keys and the "storage"
// namespace. Loaders check each file's magic/version header, so a foreign layout isn't loaded. Imports stage until boot.
static const char *const REC_ALT_FILES[]  = { "/alttrend.bin", "/altfront.bin", "/altfrontup.bin" };
static const char *const REC_ALT_NVS[]    = { NK_altbaseSec, NK_altWindV, "altBkSum", "altBkN", "altBkWst", "altBkHr", "altBkHist" };
static const char *const REC_PERF_FILES[] = { "/sailfront.bin", "/motorfront.bin" };
static const char *const REC_BH_FILES[]   = { BHRES_PATH, BHCAP_PATH };
static const char *const REC_SLED_FILES[] = { SLED_PATH, SLED_LIVE_PATH };
static const char *const REC_ZF_FILES[]   = { ZFIT_PATH, ZEROLOG_PATH };
static const char *const REC_FA_FILES[]   = { FA_MATRIX_PATH, FA_FLIP_PATH };
static const char *const REC_HUNT_FILES[] = { "/huntledger.csv" };
#define REC_N(a) (uint8_t)(sizeof(a) / sizeof(a[0]))
struct RecGroup { const char *name; const char *const *files; uint8_t nFiles; const char *const *nvs; uint8_t nNvs; bool storage; };
static const RecGroup REC_GROUPS[] = {
  { "altHealth",      REC_ALT_FILES,  REC_N(REC_ALT_FILES),  REC_ALT_NVS, REC_N(REC_ALT_NVS), false },
  { "boatPerf",       REC_PERF_FILES, REC_N(REC_PERF_FILES), nullptr, 0, false },
  { "battHealth",     REC_BH_FILES,   REC_N(REC_BH_FILES),   nullptr, 0, false },
  { "solarLedger",    REC_SLED_FILES, REC_N(REC_SLED_FILES), nullptr, 0, false },
  { "zeroFitHistory", REC_ZF_FILES,   REC_N(REC_ZF_FILES),   nullptr, 0, false },
  { "resonanceMap",   REC_FA_FILES,   REC_N(REC_FA_FILES),   nullptr, 0, false },
  { "damperRecord",   REC_HUNT_FILES, REC_N(REC_HUNT_FILES), nullptr, 0, false },
  { "lifetime",       nullptr, 0, nullptr, 0, true },
};
static const size_t REC_GROUP_COUNT = sizeof(REC_GROUPS) / sizeof(REC_GROUPS[0]);
// Lifetime keys the record leaves alone: the zero-drift fit imports as its own tables row, so unticking that row keeps
// this unit's. The barometer ring is this unit's own weather history and never imports.
static const char *const REC_STORAGE_KEEP[] = {
  "ZFitValid", "ZFitSensor", "ZFitC", "ZFitB", "ZFitR2", "ZFitEpoch",
  "BaroHist", "BaroHistHead", "BaroHistEpch",
};
static bool recStorageKept(const char *key) {
  for (size_t i = 0; i < sizeof(REC_STORAGE_KEEP) / sizeof(REC_STORAGE_KEEP[0]); i++)
    if (strcmp(REC_STORAGE_KEEP[i], key) == 0) return true;
  return false;
}

static String recStagedPath(const char *live) { return String("/imp_") + (live[0] == '/' ? live + 1 : live); }

// The records JSON as a chain of fixed 16 KB PSRAM blocks, served block by block (hundreds of KB of base64 must stay
// off the internal heap). Never reallocated: tlsf_realloc copies inside the PSRAM heap lock, which Core 1 takes.
class RecOut {
 public:
  struct Blk { Blk *next; size_t used; char d[16384]; };
  Blk *head = nullptr, *tail = nullptr;
  size_t len = 0;
  bool ok = true;
  Blk *cur = nullptr;   // read cursor: the block holding byte curAt
  size_t curAt = 0;
  RecOut() = default;
  RecOut(const RecOut &) = delete;
  RecOut &operator=(const RecOut &) = delete;
  ~RecOut() { while (head) { Blk *n = head->next; free(head); head = n; } }
  bool grow() {
    Blk *b = (Blk *)ps_malloc(sizeof(Blk));
    if (!b) { ok = false; return false; }
    b->next = nullptr;
    b->used = 0;
    if (tail) tail->next = b; else head = b;
    tail = b;
    return true;
  }
  void add(const char *s, size_t n) {
    while (n && ok) {
      if (!tail || tail->used == sizeof(tail->d)) { if (!grow()) return; }
      size_t room = sizeof(tail->d) - tail->used;
      size_t k = (n < room) ? n : room;
      memcpy(tail->d + tail->used, s, k);
      tail->used += k; len += k; s += k; n -= k;
    }
  }
  void add(const char *s) { add(s, strlen(s)); }
  void add(const String &s) { add(s.c_str(), s.length()); }
  void addStr(const String &v) { String t; cfgAppendJsonStr(t, v); add(t); }
  void addB64(const uint8_t *d, size_t n) {
    while (n && ok) {
      size_t room = tail ? sizeof(tail->d) - tail->used : 0;
      if (room < 5) { if (!grow()) return; room = sizeof(tail->d); }
      // Whole 3-byte groups until the last piece, so the pieces concatenate to the one-shot encoding. The -1 leaves
      // room for the NUL mbedtls writes after its output.
      size_t take = (room - 1) / 4 * 3;
      if (take > n) take = n;
      size_t w = 0;
      if (mbedtls_base64_encode((unsigned char *)tail->d + tail->used, room, &w, d, take) != 0) { ok = false; return; }
      tail->used += w; len += w; d += take; n -= take;
    }
  }
  size_t read(uint8_t *out, size_t maxLen, size_t index) {
    if (!cur || index < curAt) { cur = head; curAt = 0; }
    while (cur && index >= curAt + cur->used) { curAt += cur->used; cur = cur->next; }
    size_t w = 0;
    while (cur && w < maxLen) {
      size_t off = index + w - curAt;
      size_t k = cur->used - off;
      if (k > maxLen - w) k = maxLen - w;
      memcpy(out + w, cur->d + off, k);
      w += k;
      if (off + k == cur->used) { curAt += cur->used; cur = cur->next; }
    }
    return w;
  }
};

static uint32_t recFnv(const uint8_t *d, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) { h ^= d[i]; h *= 16777619u; }
  return h;
}

// Whole file into PSRAM (caller frees); nullptr when absent, or with *failed set when it could not be read whole.
static uint8_t *recReadFile(const char *path, size_t *n, bool *failed) {
  *n = 0;
  fsTakeLock();
  File f = LittleFS.open(path, "r");
  if (!f) { fsReleaseLock(); return nullptr; }
  size_t sz = f.size();
  uint8_t *buf = (uint8_t *)ps_malloc(sz ? sz : 1);
  if (buf) *n = f.read(buf, sz);
  f.close();
  fsReleaseLock();
  if (!buf || *n != sz) { free(buf); *n = 0; *failed = true; return nullptr; }
  return buf;
}

// One "storage" entry as "type:value" (BLOB as hex). False for a type this never writes.
static bool recStorageValue(nvs_handle_t h, const nvs_entry_info_t &info, String &o) {
  char b[32];
  switch (info.type) {
    case NVS_TYPE_U8:  { uint8_t v;  if (nvs_get_u8(h, info.key, &v) != ESP_OK) return false;  snprintf(b, sizeof(b), "u8:%u", (unsigned)v); break; }
    case NVS_TYPE_I8:  { int8_t v;   if (nvs_get_i8(h, info.key, &v) != ESP_OK) return false;  snprintf(b, sizeof(b), "i8:%d", (int)v); break; }
    case NVS_TYPE_U16: { uint16_t v; if (nvs_get_u16(h, info.key, &v) != ESP_OK) return false; snprintf(b, sizeof(b), "u16:%u", (unsigned)v); break; }
    case NVS_TYPE_I16: { int16_t v;  if (nvs_get_i16(h, info.key, &v) != ESP_OK) return false; snprintf(b, sizeof(b), "i16:%d", (int)v); break; }
    case NVS_TYPE_U32: { uint32_t v; if (nvs_get_u32(h, info.key, &v) != ESP_OK) return false; snprintf(b, sizeof(b), "u32:%lu", (unsigned long)v); break; }
    case NVS_TYPE_I32: { int32_t v;  if (nvs_get_i32(h, info.key, &v) != ESP_OK) return false; snprintf(b, sizeof(b), "i32:%ld", (long)v); break; }
    case NVS_TYPE_U64: { uint64_t v; if (nvs_get_u64(h, info.key, &v) != ESP_OK) return false; snprintf(b, sizeof(b), "u64:%llu", (unsigned long long)v); break; }
    case NVS_TYPE_I64: { int64_t v;  if (nvs_get_i64(h, info.key, &v) != ESP_OK) return false; snprintf(b, sizeof(b), "i64:%lld", (long long)v); break; }
    case NVS_TYPE_STR: {
      size_t sz = 0;
      if (nvs_get_str(h, info.key, nullptr, &sz) != ESP_OK || sz > 512) return false;
      char s[513];
      if (nvs_get_str(h, info.key, s, &sz) != ESP_OK) return false;
      o = "str:"; o += s;
      return true;
    }
    case NVS_TYPE_BLOB: {
      size_t sz = 0;
      if (nvs_get_blob(h, info.key, nullptr, &sz) != ESP_OK || sz > 256) return false;
      uint8_t d[256];
      if (nvs_get_blob(h, info.key, d, &sz) != ESP_OK) return false;
      o = "blob:";
      for (size_t i = 0; i < sz; i++) { char x[3]; snprintf(x, sizeof(x), "%02x", d[i]); o += x; }
      return true;
    }
    default: return false;
  }
  o = b;
  return true;
}

// Records JSON in PSRAM as an opaque RecOut (void*: a sketch type in a free-function signature breaks the Arduino
// auto-prototype); serve it with recordsExportRead, release it with recordsExportFree. nullptr on OOM or an unreadable
// record file. Every group is present, empty ones too, so importing it leaves none. summary = names, sizes and an
// order-independent content hash (sum of per-item hashes) for the import preview, excluding lifetime keys an import
// never writes.
void *recordsExportJson(bool summary, size_t *outLen) {
  RecOut *o = new (std::nothrow) RecOut();
  if (!o) return nullptr;
  o->add("{\"records_v\":1,\"fw_version\":");
  o->addStr(String(FIRMWARE_VERSION));
  o->add(",\"records\":{");
  for (size_t gi = 0; gi < REC_GROUP_COUNT; gi++) {
    const char *const *files = REC_GROUPS[gi].files;
    const char *const *nvs = REC_GROUPS[gi].nvs;
    uint32_t hash = 0;
    size_t bytes = 0, items = 0;
    bool readFail = false;
    if (gi) o->add(",");
    o->addStr(String(REC_GROUPS[gi].name));
    o->add(":{");
    const size_t body = o->len;   // the group body is written in place; a non-empty one is followed by ","
    if (REC_GROUPS[gi].nFiles) {
      o->add("\"files\":{");
      bool first = true;
      for (uint8_t f = 0; f < REC_GROUPS[gi].nFiles; f++) {
        size_t n = 0;
        uint8_t *buf = recReadFile(files[f], &n, &readFail);
        if (!buf) continue;
        hash += recFnv(buf, n) ^ recFnv((const uint8_t *)files[f], strlen(files[f]));
        bytes += n; items++;
        if (!summary) {
          if (!first) o->add(",");
          first = false;
          o->addStr(String(files[f])); o->add(":\"");
          o->addB64(buf, n);
          o->add("\"");
        }
        free(buf);
      }
      o->add("}");
    }
    if (REC_GROUPS[gi].nNvs) {
      o->add(o->len > body ? ",\"nvs\":{" : "\"nvs\":{");
      bool first = true;
      for (uint8_t k = 0; k < REC_GROUPS[gi].nNvs; k++) {
        if (!settingExists(nvs[k])) continue;
        String v = settingRead(nvs[k]);
        String kv = String(nvs[k]) + "=" + v;
        hash += recFnv((const uint8_t *)kv.c_str(), kv.length());
        items++;
        if (!summary) {
          if (!first) o->add(",");
          first = false;
          o->addStr(String(nvs[k])); o->add(":"); o->addStr(v);
        }
      }
      o->add("}");
    }
    if (REC_GROUPS[gi].storage) {
      o->add(o->len > body ? ",\"storage\":{" : "\"storage\":{");
      nvs_handle_t h;
      if (nvs_open("storage", NVS_READONLY, &h) == ESP_OK) {
        nvs_iterator_t it = nullptr;
        esp_err_t e = nvs_entry_find(NVS_DEFAULT_PART_NAME, "storage", NVS_TYPE_ANY, &it);
        bool first = true;
        while (e == ESP_OK) {
          nvs_entry_info_t info;
          nvs_entry_info(it, &info);
          String v;
          if (recStorageValue(h, info, v)) {
            if (!recStorageKept(info.key)) {
              String kv = String(info.key) + "=" + v;
              hash += recFnv((const uint8_t *)kv.c_str(), kv.length());
              items++;
            }
            if (!summary) {
              if (!first) o->add(",");
              first = false;
              o->addStr(String(info.key)); o->add(":"); o->addStr(v);
            }
          }
          e = nvs_entry_next(&it);
        }
        nvs_release_iterator(it);
        nvs_close(h);
      }
      o->add("}");
    }
    if (strcmp(REC_GROUPS[gi].name, "altHealth") == 0) {
      char es[32];
      snprintf(es, sizeof(es), ",\"eng_s\":%ld", (long)EngineRunTime_AllTime);
      o->add(es);
    }
    if (!o->ok || readFail) { delete o; return nullptr; }
    if (o->len > body) o->add(",");
    char tail[64];
    snprintf(tail, sizeof(tail), "\"bytes\":%u,\"items\":%u,\"hash\":\"%08lx\"}", (unsigned)bytes, (unsigned)items, (unsigned long)hash);
    o->add(tail);
  }
  o->add("}}");
  if (!o->ok) { delete o; return nullptr; }
  *outLen = o->len;
  return o;
}

// AsyncWebServer filler for a recordsExportJson result: bytes from index on, sequential calls walk the blocks in order.
size_t recordsExportRead(void *rec, uint8_t *out, size_t maxLen, size_t index) {
  return rec ? ((RecOut *)rec)->read(out, maxLen, index) : 0;
}

void recordsExportFree(void *rec) { delete (RecOut *)rec; }

// Span of the string value of "key" inside obj (no escapes are expected — base64 and hex never carry any).
static bool recJsonStrSpan(const char *obj, const char *key, const char **start, size_t *len) {
  String needle = "\"";
  needle += key;
  needle += "\"";
  const char *p = strstr(obj, needle.c_str());
  if (!p) return false;
  p += needle.length();
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':') p++;
  if (*p != '"') return false;
  const char *e = strchr(p + 1, '"');
  if (!e) return false;
  *start = p + 1;
  *len = (size_t)(e - p - 1);
  return true;
}

// One "type:value" lifetime entry back into the open "storage" handle.
static bool recStorageWrite(nvs_handle_t h, const char *key, const String &tv) {
  int c = tv.indexOf(':');
  if (c < 1) return false;
  String t = tv.substring(0, c);
  const char *v = tv.c_str() + c + 1;
  if (t == "u8")  return nvs_set_u8(h, key, (uint8_t)strtoul(v, nullptr, 10)) == ESP_OK;
  if (t == "i8")  return nvs_set_i8(h, key, (int8_t)strtol(v, nullptr, 10)) == ESP_OK;
  if (t == "u16") return nvs_set_u16(h, key, (uint16_t)strtoul(v, nullptr, 10)) == ESP_OK;
  if (t == "i16") return nvs_set_i16(h, key, (int16_t)strtol(v, nullptr, 10)) == ESP_OK;
  if (t == "u32") return nvs_set_u32(h, key, (uint32_t)strtoul(v, nullptr, 10)) == ESP_OK;
  if (t == "i32") return nvs_set_i32(h, key, (int32_t)strtol(v, nullptr, 10)) == ESP_OK;
  if (t == "u64") return nvs_set_u64(h, key, (uint64_t)strtoull(v, nullptr, 10)) == ESP_OK;
  if (t == "i64") return nvs_set_i64(h, key, (int64_t)strtoll(v, nullptr, 10)) == ESP_OK;
  if (t == "str") return nvs_set_str(h, key, v) == ESP_OK;
  if (t == "blob") {
    size_t n = strlen(v) / 2;
    if (n > 256) return false;
    uint8_t d[256];
    for (size_t i = 0; i < n; i++) { char x[3] = { v[2 * i], v[2 * i + 1], 0 }; d[i] = (uint8_t)strtoul(x, nullptr, 16); }
    return nvs_set_blob(h, key, d, n) == ESP_OK;
  }
  return false;
}

// Apply the `records` groups the body carries; whatever a group lacks is removed so the importer holds what the source
// held. Files stage first: any decode/write failure removes every staged file and touches nothing else (-2); -1 =
// malformed. On success sets importRebootPending so this unit's savers don't overwrite the imported keys before reboot.
int recordsImport(const char *body) {
  char *rec = cfgObjectDup(body, "records");
  if (!rec) return -1;
  char *grp[REC_GROUP_COUNT] = {}, *fo[REC_GROUP_COUNT] = {}, *no[REC_GROUP_COUNT] = {}, *so[REC_GROUP_COUNT] = {};
  int applied = 0;
  bool bad = false;
  // Phase 1: every group's sections, then every file decoded and staged.
  for (size_t gi = 0; gi < REC_GROUP_COUNT && !bad; gi++) {
    const RecGroup &G = REC_GROUPS[gi];
    grp[gi] = cfgObjectDup(rec, G.name);
    if (!grp[gi]) continue;   // not picked: this regulator keeps its own
    if (G.nFiles && !(fo[gi] = cfgObjectDup(grp[gi], "files"))) bad = true;
    if (G.nNvs && !(no[gi] = cfgObjectDup(grp[gi], "nvs"))) bad = true;
    if (G.storage && !(so[gi] = cfgObjectDup(grp[gi], "storage"))) bad = true;
    for (uint8_t f = 0; f < G.nFiles && !bad; f++) {
      const char *live = G.files[f];
      const char *b64 = nullptr;
      size_t bl = 0, n = 0;
      uint8_t *buf = nullptr;
      if (recJsonStrSpan(fo[gi], live, &b64, &bl)) {
        mbedtls_base64_decode(nullptr, 0, &n, (const unsigned char *)b64, bl);
        buf = (uint8_t *)ps_malloc(n ? n : 1);
        if (!buf || mbedtls_base64_decode(buf, n, &n, (const unsigned char *)b64, bl) != 0) { free(buf); bad = true; break; }
      }
      String staged = recStagedPath(live);
      fsTakeLock();
      File out = LittleFS.open(staged.c_str(), "w");   // zero bytes = the source had no such file
      bool ok = (bool)out;
      if (out) {
        if (buf && n) ok = (out.write(buf, n) == n);
        out.close();
        fsFreeDirty = true;
      }
      fsReleaseLock();
      free(buf);
      if (!ok) bad = true;
      else applied++;
    }
  }
  if (bad) {
    fsTakeLock();
    for (size_t gi = 0; gi < REC_GROUP_COUNT; gi++)
      if (grp[gi]) for (uint8_t f = 0; f < REC_GROUPS[gi].nFiles; f++) LittleFS.remove(recStagedPath(REC_GROUPS[gi].files[f]).c_str());
    fsFreeDirty = true;
    fsReleaseLock();
    applied = -2;
  } else {
    // Phase 2: the NVS halves.
    bool lifeIn = false, altIn = false;
    for (size_t gi = 0; gi < REC_GROUP_COUNT; gi++) {
      if (REC_GROUPS[gi].storage && grp[gi]) lifeIn = true;
      if (strcmp(REC_GROUPS[gi].name, "altHealth") == 0 && grp[gi]) altIn = true;
    }
    // The other way round: the file's engine hours replace this regulator's under its own alt-health record.
    if (lifeIn && !altIn && settingExists(NK_altbaseSec)) {
      for (size_t gi = 0; gi < REC_GROUP_COUNT; gi++) {
        String fe;
        if (!REC_GROUPS[gi].storage || !grp[gi] || !cfgJsonExtract(so[gi], "EngRunTime_AT", fe) || !fe.startsWith("i32:")) continue;
        int32_t ownEng = (int32_t)EngineRunTime_AllTime;
        nvs_handle_t eh;
        if (nvs_open("storage", NVS_READONLY, &eh) == ESP_OK) { nvs_get_i32(eh, "EngRunTime_AT", &ownEng); nvs_close(eh); }
        const double base = settingRead(NK_altbaseSec).toDouble() + (double)strtol(fe.c_str() + 4, nullptr, 10) - (double)ownEng;
        settingWrite(NK_altbaseSec, String(base, 1).c_str());
      }
    }
    for (size_t gi = 0; gi < REC_GROUP_COUNT; gi++) {
      if (!grp[gi]) continue;
      const RecGroup &G = REC_GROUPS[gi];
      importRebootPending = true;
      for (uint8_t k = 0; k < G.nNvs; k++) {
        String v;
        if (cfgJsonExtract(no[gi], G.nvs[k], v)) settingWrite(G.nvs[k], v.c_str());
        else settingRemove(G.nvs[k]);
        applied++;
      }
      // The trend axis counts engine seconds from altbaseSec. Without the lifetime totals this regulator keeps its own
      // engine hours, so the baseline moves by the difference and new trend points continue where the file's stopped.
      String es;
      if (strcmp(G.name, "altHealth") == 0 && !lifeIn && cfgJsonExtract(grp[gi], "eng_s", es) && settingExists(NK_altbaseSec)) {
        int32_t ownEng = (int32_t)EngineRunTime_AllTime;
        nvs_handle_t eh;
        if (nvs_open("storage", NVS_READONLY, &eh) == ESP_OK) { nvs_get_i32(eh, "EngRunTime_AT", &ownEng); nvs_close(eh); }
        const double base = (double)ownEng - (double)es.toInt() + settingRead(NK_altbaseSec).toDouble();
        settingWrite(NK_altbaseSec, String(base, 1).c_str());
      }
      if (G.storage) {
        nvs_handle_t h;
        if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK) {
          // This regulator's lifetime keys the file does not hold go, as its files do: every key the export would
          // carry (recStorageValue), less the kept ones. Collected first; erasing under the iterator is not allowed.
          size_t nAll = 0;
          nvs_iterator_t it = nullptr;
          for (esp_err_t e = nvs_entry_find(NVS_DEFAULT_PART_NAME, "storage", NVS_TYPE_ANY, &it); e == ESP_OK; e = nvs_entry_next(&it)) nAll++;
          nvs_release_iterator(it);
          char (*gone)[NVS_KEY_NAME_MAX_SIZE] = (char (*)[NVS_KEY_NAME_MAX_SIZE])ps_malloc((nAll + 1) * NVS_KEY_NAME_MAX_SIZE);
          size_t nGone = 0;
          it = nullptr;
          for (esp_err_t e = nvs_entry_find(NVS_DEFAULT_PART_NAME, "storage", NVS_TYPE_ANY, &it); gone && e == ESP_OK && nGone < nAll; e = nvs_entry_next(&it)) {
            nvs_entry_info_t info;
            nvs_entry_info(it, &info);
            String needle = "\"", v;
            needle += info.key;
            needle += "\":";
            if (!recStorageKept(info.key) && strcmp(info.key, "SoftClockEp") != 0 && !strstr(so[gi], needle.c_str()) && recStorageValue(h, info, v))
              strlcpy(gone[nGone++], info.key, NVS_KEY_NAME_MAX_SIZE);
          }
          nvs_release_iterator(it);
          for (size_t i = 0; i < nGone; i++) nvs_erase_key(h, gone[i]);
          free(gone);
          const char *p = so[gi] + 1;
          while (true) {
            p = strchr(p, '"');
            if (!p) break;
            String key, tv;
            p = cfgJsonStrDecode(p + 1, key);
            while (*p == ' ' || *p == ':') p++;
            if (*p != '"') break;
            p = cfgJsonStrDecode(p + 1, tv);
            if (!key.length() || key.length() >= 16 || recStorageKept(key.c_str())) continue;
            if (key == "SoftClockEp" && tv.startsWith("u32:")) {   // a clock estimate: moved forward only, never back to the file's save time
              uint32_t own = 0;
              if (nvs_get_u32(h, "SoftClockEp", &own) == ESP_OK && own >= (uint32_t)strtoul(tv.c_str() + 4, nullptr, 10)) continue;
            }
            if (recStorageWrite(h, key.c_str(), tv)) applied++;
          }
          nvs_commit(h);
          nvs_close(h);
        }
      }
    }
    if (applied) cfgImportNoteChanged("equipment records");
  }
  for (size_t gi = 0; gi < REC_GROUP_COUNT; gi++) { free(grp[gi]); free(fo[gi]); free(no[gi]); free(so[gi]); }
  free(rec);
  return applied;
}

// The records' staged files, swapped in by importStagedCommit like the config's own.
static void recordsStagedCommit() {
  uint8_t seed = 0, wipe = 0;
  for (size_t gi = 0; gi < REC_GROUP_COUNT; gi++) {
    for (uint8_t f = 0; f < REC_GROUPS[gi].nFiles; f++) {
      const char *live = REC_GROUPS[gi].files[f];
      String staged = recStagedPath(live);
      fsTakeLock();
      if (LittleFS.exists(staged.c_str())) {
        if (strcmp(live, "/altfront.bin") == 0) wipe |= 1;
        else if (strcmp(live, "/sailfront.bin") == 0 || strcmp(live, "/motorfront.bin") == 0) wipe |= 2;
        File sf = LittleFS.open(staged.c_str(), "r");
        size_t sz = sf ? sf.size() : 0;
        if (sf) sf.close();
        LittleFS.remove(live);
        if (sz == 0 || !LittleFS.rename(staged.c_str(), live)) LittleFS.remove(staged.c_str());
        else if (strcmp(live, "/altfront.bin") == 0) seed |= 1;
        else if (strcmp(live, "/sailfront.bin") == 0 || strcmp(live, "/motorfront.bin") == 0) seed |= 2;
        fsFreeDirty = true;
      }
      fsReleaseLock();
    }
  }
  if (seed) recSeedOwedSet(seed, true);
  // The imported curve replaces this unit's, an empty one too, but the cloud rebuilds each curve from every row this
  // unit holds there. Its rows go first; the seeded upload waits for the wipe.
  if (wipe & 1) { altCloudWipePending = true; settingWrite(NK_AltWipePend, "1"); }
  if (wipe & 2) { perfCloudWipePending = true; settingWrite(NK_PerfWipePend, "1"); }
  // The replaced curve's unsynced points go with it, or the boot would reload and upload them after the wipe.
  fsTakeLock();
  if (wipe & 1) LittleFS.remove("/altpend.bin");
  if (wipe & 2) { LittleFS.remove("/perfpends.bin"); LittleFS.remove("/perfpendsa.bin"); LittleFS.remove("/perfpendm.bin"); LittleFS.remove("/perfpendma.bin"); }
  fsReleaseLock();
}

int applyImportConfig(const char *body, bool cloudPush) {
  if (!body) return -1;
  char *cfgBuf = cfgObjectDup(body, "config");
  if (!cfgBuf) return -1;   // malformed / wrong shape — reject, don't half-apply
  const char *cfg = cfgBuf;
  char *tblBuf = cfgObjectDup(body, "tables");
  char *basisBuf = cfgObjectDup(body, "cx_basis");
  for (size_t k = 0; k < sizeof(CFG_TABLE_NAMES) / sizeof(CFG_TABLE_NAMES[0]); k++) cfgImpTblSet[k] = false;
  String tmp;
  const bool cxInFile = cfgJsonExtract(cfg, "commissionDoneMask", tmp);   // the file's commissioning status was taken
  cxImportCapturePre();
  cfgOwnerTablesCapturePre();
  const String preRpmScale = settingRead(NK_RPMScalingFactor);
  const float prePerfSrc = settingRead("perfSpeedSrc").toFloat();
  const float preCOffset = settingRead(NK_AlternatorCOffset).toFloat();
  // Class, capacity and chemistry move the CV plant gain the commissioning fit measured. An import writes NVS raw,
  // so it must detect these changes itself to raise the re-commission nag /saveVesselInfo would.
  String preClass = settingRead(NK_BatteryVoltage);
  String preCap   = settingRead(NK_BatteryCapacity_Ah);
  String preChem  = settingRead(NK_batteryType);
  String preWinding = settingRead(NK_FieldWindingV);
  // The voltage class is the MEANING of every volt/duty value, so it converts before the manifest loop: a class-only
  // push gets the conversion /saveVesselInfo runs (else 12 V pushed onto a 48V boat keeps a 57 V bulk target over a
  // 12 V bank); a full export then overwrites each converted key. applyNominalVoltageChange also resets Min% floors and hunt pockets.
  bool cfgClassBad = false;
  {
    String cv;
    if (cfgJsonExtract(cfg, "BatteryVoltage", cv)) {
      int newV = cv.toInt();
      if (newV != 12 && newV != 24 && newV != 36 && newV != 48) {
        // Dropped here rather than written: the boot loader silently coerces a bad value to 12, which on a
        // 48V boat is the same catastrophe by a slower route.
        cfgClassBad = true;
        queueConsoleMessageF("IMPORT WARNING: ignoring invalid system voltage \"%s\" in the imported config", cv.c_str());
      } else if (newV != (int)SYSTEM_VOLTAGE_CLASS) {
        int oldV = (int)SYSTEM_VOLTAGE_CLASS;
        SYSTEM_VOLTAGE_CLASS = (uint8_t)newV;
        applyNominalVoltageChange(oldV, newV);   // persists the class + every converted setting, synchronously
      }
    }
  }
  // The field winding is the other half of what every field-duty value means; it converts after the class (which bounds
  // it). A full export's fieldWindApplied + knobs overwrite this; fieldSideConvertUnwritten puts any value the file left
  // onto the file's scale, and the boot reconcile moves all onto this device's effective winding.
  bool cfgWindBad = false;
  {
    String wv;
    if (cfgJsonExtract(cfg, "FieldWindingV", wv)) {
      int w = wv.toInt();
      if (w != 12 && w != 24 && w != 36 && w != 48) {
        cfgWindBad = true;
        queueConsoleMessageF("IMPORT WARNING: ignoring invalid field winding voltage \"%s\" in the imported config", wv.c_str());
      } else if (w != (int)FieldWindingV) {
        fieldWindingSet(w, nullptr);
      }
    }
  }
  // The scale the stored field-side values are on once the winding step above has run, and the field-side NVS keys the
  // loops below write (";key;key;"), for fieldSideConvertUnwritten after them.
  const int preWindApplied = settingRead(NK_fieldWindApplied).toInt();
  String cfgWritten = ";";
  // Field resistance and the alternator temperature it was measured at import only as a pair (old exports carry a bare
  // 2-ohm seed); otherwise the Prerequisites screen asks for a measurement. Interim until a cloud-side strip step.
  bool fieldResPair;
  {
    String r, t;
    const bool hasR = cfgJsonExtract(cfg, "FieldResistance", r);
    const bool hasT = cfgJsonExtract(cfg, "FieldResTrefC", t);
    fieldResPair = hasR && hasT;
    if (hasR != hasT)
      queueConsoleMessage("IMPORT: Field Resistance not imported - it travels only with the alternator temperature it was measured at; enter it on this boat");
  }
  int applied = 0;
  String wrote = ";";   // params the loop below wrote (";name;")
  for (size_t i = 0; i < CONFIG_MANIFEST_COUNT; i++) {
    if (CONFIG_MANIFEST[i].tier == 3) continue;   // export-only: this device's own history, never adopted
    if (cfgImportDedicated(CONFIG_MANIFEST[i].param)) continue;
    if (cfgClassBad && strcmp(CONFIG_MANIFEST[i].param, "BatteryVoltage") == 0) continue;
    if (cfgWindBad && strcmp(CONFIG_MANIFEST[i].param, "FieldWindingV") == 0) continue;
    if (!fieldResPair && (strcmp(CONFIG_MANIFEST[i].param, "FieldResistance") == 0 || strcmp(CONFIG_MANIFEST[i].param, "FieldResTrefC") == 0)) continue;
    String val;
    bool found = cfgJsonExtract(cfg, CONFIG_MANIFEST[i].param, val);
    if (!found) {
      for (size_t a = 0; a < sizeof(CFG_PARAM_ALIASES) / sizeof(CFG_PARAM_ALIASES[0]); a++) {
        if (strcmp(CFG_PARAM_ALIASES[a].now, CONFIG_MANIFEST[i].param) == 0
            && cfgJsonExtract(cfg, CFG_PARAM_ALIASES[a].legacy, val)) { found = true; break; }
      }
    }
    if (found) {
      if (importRowIsNumeric(CONFIG_MANIFEST[i].nvsKey) && importValueNonFinite(val.c_str())) {   // "nan"/"inf"/"1e39" would be stored and read back as a real value
        queueConsoleMessageF("IMPORT WARNING: ignoring non-finite value \"%s\" for %s", val.c_str(), CONFIG_MANIFEST[i].param);
        continue;
      }
      if (strcmp(CONFIG_MANIFEST[i].param, "MinRPMForField") == 0 && val.toInt() < 0) val = "0";
      // The class and winding steps above already stored those two: they are judged against the pre-import value.
      const bool changed = !strcmp(CONFIG_MANIFEST[i].param, "BatteryVoltage") ? (cfgImportChangedNames && val != preClass)
                         : !strcmp(CONFIG_MANIFEST[i].param, "FieldWindingV")  ? (cfgImportChangedNames && val != preWinding)
                         : cfgImportWillChange(CONFIG_MANIFEST[i].nvsKey, val);
      if (settingWrite(CONFIG_MANIFEST[i].nvsKey, val.c_str())) {
        applied++;
        if (changed) cfgImportNoteChanged(CONFIG_MANIFEST[i].param);
        wrote += CONFIG_MANIFEST[i].param; wrote += ';';
        if (fieldSideKey(CONFIG_MANIFEST[i].nvsKey)) { cfgWritten += CONFIG_MANIFEST[i].nvsKey; cfgWritten += ';'; }
      }
    }
  }
  // The live power-cycle count is storage "PowerCycles" (what loadNVSData reads); NK_totalPowerCycles is only the /get
  // handler's mirror, so the imported count has to land in storage too.
  if (wrote.indexOf(";totalPowerCycles;") >= 0) {
    totalPowerCycles = settingRead(NK_totalPowerCycles).toInt();   // as the /get handler does (covers noReboot=1)
    nvs_handle_t sh;
    if (nvs_open("storage", NVS_READWRITE, &sh) == ESP_OK) {
      nvs_set_i32(sh, "PowerCycles", (int32_t)totalPowerCycles);
      nvs_commit(sh);
      nvs_close(sh);
    }
  }
  // Schema number capped at this firmware's own: the boot then runs exactly the conversions the file's values still
  // need, and a file from newer firmware cannot make this one skip a future conversion.
  if (cfgJsonExtract(cfg, "cfgSchema", tmp) && floatTokenValid(tmp.c_str())) {
    const String sv = String(constrain((int)tmp.toInt(), 0, SETTINGS_SCHEMA_VERSION));
    const bool changed = cfgImportWillChange(NK_cfgSchema, sv);
    if (settingWrite(NK_cfgSchema, sv.c_str())) { applied++; if (changed) cfgImportNoteChanged("cfgSchema"); }
  }
  // The other regulators on the source's boat, less this unit itself (copying a partner's list here would name this
  // unit as its own partner). A source that listed this unit is its partner on the same boat, so the source takes
  // this unit's place in the list.
  if (cfgJsonExtract(cfg, "pairList", tmp)) {
    String keep, srcUid;
    const bool haveSrc = cfgJsonExtract(body, "source_uid", srcUid) && srcUid.length() == 16 && !srcUid.equalsIgnoreCase(device_id_hex);
    bool listedHere = false, srcListed = false;
    int pos = 0;
    while (pos < (int)tmp.length()) {
      int end = tmp.indexOf('\x1e', pos);
      if (end < 0) end = tmp.length();
      String rec = tmp.substring(pos, end);
      int us = rec.indexOf('\x1f');
      String uid = us < 0 ? rec : rec.substring(0, us);
      if (uid.equalsIgnoreCase(device_id_hex)) listedHere = true;
      else if (uid.length()) {
        if (haveSrc && uid.equalsIgnoreCase(srcUid)) srcListed = true;
        if (keep.length()) keep += '\x1e';
        keep += rec;
      }
      pos = end + 1;
    }
    if (listedHere && haveSrc && !srcListed) {
      String srcName;
      cfgJsonExtract(cfg, "regulator_name", srcName);
      if (keep.length()) keep += '\x1e';
      keep += srcUid; keep += '\x1f'; keep += srcName;
    }
    const bool changed = cfgImportWillChange(NK_pairList, keep);
    if (settingWrite(NK_pairList, keep.c_str())) { applied++; if (changed) cfgImportNoteChanged("pairList"); }
  }
  // The install verdict was judged on one level capture in one orientation. A capture or orientation that arrives
  // without its verdict leaves none (the next Level Zero re-latches it), as /saveVesselInfo does on an orientation change.
  if ((wrote.indexOf(";imu_zero;") >= 0 || wrote.indexOf(";imu_mount_orientation;") >= 0) && wrote.indexOf(";imu_mnt_state;") < 0)
    settingRemove(NK_imu_mnt_state);
  // Only nag once a pass has actually been finished (epoch stamped) — a fresh device has
  // nothing to invalidate, and every key looks "changed" there because none existed. When the file's commissioning
  // status came along, applyImportCommissioning decides each step from what is now in force instead.
  if (CommissionEpoch > 0 && !cxInFile) {
    bool classChanged = (settingRead(NK_BatteryVoltage) != preClass);
    if (classChanged || settingRead(NK_BatteryCapacity_Ah) != preCap
        || settingRead(NK_batteryType) != preChem) {
      raiseRecommissionNag();
    }
    // The class conversion above already reset the knee-learned Min% floors (duty learned at the OLD class); only the
    // wizard stage clear is left. applyImportTables runs last, so an imported table wins.
    if (classChanged) {
      commissionClearStage(7);
      queueConsoleMessage("IMPORT WARNING: battery voltage class changed - charge profile, protection trips and field-duty limits converted to the new class for the field winding set in Vessel Info; learned tachometer keep-alive floors reset and learned oscillation speed ranges cleared; recommissioning recommended");
    }
  } else if (cxInFile && settingRead(NK_BatteryVoltage) != preClass) {
    queueConsoleMessage("IMPORT: battery voltage class changed - charge profile, protection trips and field-duty limits converted to the new class for the field winding set in Vessel Info");
  }
  if (settingRead(NK_FieldWindingV) != preWinding)
    queueConsoleMessageF("IMPORT WARNING: field winding voltage changed to %sV - field-duty settings and Max Field Volts move onto the new winding's scale, and the alternator-health history is cleared. Confirm the rating on the alternator's datasheet.",
                         settingRead(NK_FieldWindingV).c_str());
  // Registry knobs — generic import mirroring the export side.
  // Macro for the same auto-prototype reason as the emit.
  #define CFG_IMPORT_REGISTRY(REG, COUNT) \
    for (size_t i = 0; i < COUNT; i++) { \
      String val; \
      if (cfgJsonExtract(cfg, REG[i].name, val)) { \
        char key[16]; \
        snprintf(key, sizeof(key), "%s", REG[i].name); \
        if (!floatTokenValid(val.c_str())) { queueConsoleMessageF("IMPORT WARNING: ignoring non-finite value \"%s\" for %s", val.c_str(), REG[i].name); continue; } \
        const bool changed = cfgImportWillChange(key, val); \
        if (settingWrite(key, val.c_str())) { applied++; if (changed) cfgImportNoteChanged(REG[i].name); if (fieldSideKey(key)) { cfgWritten += key; cfgWritten += ';'; } } \
      } \
    }
  CFG_IMPORT_REGISTRY(ALT_SETTINGS, ALT_SETTING_COUNT)
  CFG_IMPORT_REGISTRY(PERF_SETTINGS, PERF_SETTING_COUNT)
  #undef CFG_IMPORT_REGISTRY
  fieldSideConvertUnwritten(preWindApplied, settingRead(NK_fieldWindApplied).toInt(), cfgWritten);
  // The learned zero correction (zfC) is on the current offset it was learned against, so the two land as a pair, both
  // written here before the import's reboot. A fit that comes without its offset moves onto this regulator's offset. An
  // offset that comes without the fit moves this regulator's fit with it, as a typed offset does with auto zero
  // correction off; with it on, a cloud push is an admin correcting a reading seen in telemetry and the fit stays (the
  // /get rule), while a config import is a restore and the fit moves.
  const bool offWritten = (wrote.indexOf(";AlternatorCOffset;") >= 0);
  const float postCOffset = settingRead(NK_AlternatorCOffset).toFloat();
  float zfShiftA = 0.0f;
  if (!offWritten) {
    String fo;
    if (cxImportBasis(basisBuf, cfg, tblBuf, "AlternatorCOffset", false, fo) && floatTokenValid(fo.c_str()))
      zfShiftA = fo.toFloat() - postCOffset;
  }
  applied += applyImportTables(tblBuf, zfShiftA);
  bool zfFromFile = false;
  for (size_t k = 0; k < sizeof(CFG_TABLE_NAMES) / sizeof(CFG_TABLE_NAMES[0]); k++)
    if (strcmp(CFG_TABLE_NAMES[k], "zeroFit") == 0) zfFromFile = cfgImpTblSet[k];
  // A file fit equal to this regulator's has no preview row, yet the pair is still the file's.
  String zfFile, zfLive;
  if (!zfFromFile && cxImportBasis(basisBuf, cfg, tblBuf, "zeroFit", true, zfFile) && cfgTableValue("zeroFit", zfLive))
    zfFromFile = (zfFile == zfLive);
  const float zfFollowA = preCOffset - postCOffset;
  nvs_handle_t zh;
  if (offWritten && !zfFromFile && zfFollowA != 0.0f && isfinite(zfFollowA) && (!cloudPush || AutoAltCurrentZero != 1)
      && nvs_open("storage", NVS_READWRITE, &zh) == ESP_OK) {
    int32_t zv = 0;
    float zc = 0.0f;
    size_t zsz = sizeof(float);
    if (nvs_get_i32(zh, "ZFitValid", &zv) == ESP_OK && zv != 0
        && nvs_get_blob(zh, "ZFitC", &zc, &zsz) == ESP_OK && isfinite(zc)) {
      zc += zfFollowA;
      nvs_set_blob(zh, "ZFitC", &zc, sizeof(float));
      nvs_commit(zh);
      queueConsoleMessageF("IMPORT: the learned zero correction moved %+.2f A with the imported current offset", zfFollowA);
    }
    nvs_close(zh);
  }
  if (applyImportCommissioning(cfg, tblBuf, basisBuf)) applied++;
  // A new tach scale moves the engine-RPM axis under every speed-indexed artifact. The ones this file brought are on
  // its own axis already; the rest of this regulator's are wiped at the next boot, and its cloud records with them.
  if (settingRead(NK_RPMScalingFactor) != preRpmScale) {
    String now, b;
    auto fromFile = [&](const char *name, bool table) {
      now = cxImportValue(name, table, true);
      return cxImportBasis(basisBuf, cfg, tblBuf, name, table, b) && cxImportSame(now, b);
    };
    uint16_t parts = AXW_ALL;
    // Records the dashboard posted just before this import are staged on the file's axis too.
    auto staged = [](const char *live) { fsTakeLock(); bool e = LittleFS.exists(recStagedPath(live).c_str()); fsReleaseLock(); return e; };
    if (staged("/motorfront.bin")) parts &= ~AXW_MOTORFRONT;
    if (staged("/altfront.bin")) parts &= ~AXW_ALTHEALTH;
    if (staged(FA_MATRIX_PATH)) parts &= ~AXW_FAMATRIX;
    if (staged(FA_FLIP_PATH)) parts &= ~AXW_FAFLIP;
    if (staged("/huntledger.csv")) parts &= ~AXW_HUNTLOG;
    if (fromFile("minDutyTable", true)) parts &= ~AXW_KNEE;
    if (fromFile("huntPockets", true)) parts &= ~AXW_HUNT;
    if (cxInFile) parts &= ~AXW_CXBITS;   // applyImportCommissioning judged the speed-binned steps against the new scale
    if (fromFile("overheatCount", true)) parts &= ~AXW_OVERHEAT;
    if (fromFile("ripTab", true)) parts &= ~AXW_RIPTAB;
    if (fromFile("ripFitAlt", false)) parts &= ~AXW_RIPFIT_ALT;
    if (fromFile("slpFitAlt", false)) parts &= ~AXW_RIPFIT_SLP;
    importBootActAdd(parts);
    settingWrite(NK_RpmAxisWipePend, "1");
    rpmAxisWipePending = true;
    queueConsoleMessage("IMPORT: engine speed scaling changed - this regulator's own speed-indexed learning not in the file is wiped at the next boot");
  }
  // A new boat-speed source invalidates every learned performance point, as perfSettingsHandle's Clear All does —
  // unless the file's own performance curves, learned on that source, came with it.
  if (settingRead("perfSpeedSrc").toFloat() != prePerfSrc) {
    fsTakeLock();
    const bool perfStaged = LittleFS.exists(recStagedPath("/sailfront.bin").c_str());
    fsReleaseLock();
    if (!perfStaged) importBootActAdd(IBA_PERF);
  }
  free(cfgBuf);
  free(tblBuf);
  free(basisBuf);
  return applied;
}

// BATTERY HEALTH MONITOR — active DCIR test + capacity-vs-cycles trend.
// Step generator: 6_functions.ino.  Globals/structs: Xregulator.ino.

static uint32_t      bhSampleLastMs   = 0;
static const uint32_t BH_SAMPLE_INTERVAL_MS = 15;   // ~INA228 cadence; bounds buffer fill

// Per-tick sampler (control loop). INA228 IBV/Bcur directly — Victron's ~1 Hz is useless
// for a step edge, so bhStartTest() requires the INA source.
void bhSample(uint32_t nowMs) {
  if (!bhSamples || bhSampleCount >= bhSampleCap) return;
  if (nowMs - bhSampleLastMs < BH_SAMPLE_INTERVAL_MS) return;
  bhSampleLastMs = nowMs;
  bhSamples[bhSampleCount].tMs = nowMs;
  bhSamples[bhSampleCount].v   = IBV;
  bhSamples[bhSampleCount].i   = Bcur;
  bhSampleCount++;
}

// Floor for bhDwellMs: the fit averages the last third of each dwell, so the first two thirds must cover the slewed
// traverse (bhStepDeltaA / TEST_ENTRY_RATE_A) plus 1 s settle, or no edge passes the ΔI gate.
uint32_t bhMinDwellMs() {
  float traverseMs = (bhStepDeltaA / TEST_ENTRY_RATE_A) * 1000.0f;
  return (uint32_t)(1.5f * (traverseMs + 1000.0f));
}

void bhAbort(const char *reason) {
  bhTestState = 3;                    // state first: core 1 reads (state 1 && !active) as "run finished, fit it"
  batteryHealthTestActive = false;
  bhAbortReason = reason;
  queueConsoleMessageF("BATT HEALTH: test aborted — %s", reason);
  char ev[192];
  snprintf(ev, sizeof(ev), "{\"test\":\"batt_health\",\"ok\":0,\"abort\":\"%s\"}", reason);
  cxLedgerLogTest(ev);
}

bool bhStartTest() {
  if (bhTestState == 1)        { bhAbortReason = "already running";          return false; }
  if (!bhSamples || !bhResults){ bhAbortReason = "buffers unallocated";      return false; }
  if (const char *rr = rpmTestRefuseReason()) { bhAbortReason = rr;         return false; }   // engine running is judged on the tach
  if (RPM < 100)               { bhAbortReason = "engine not running";       return false; }
  if (sysMode != SYS_MODE_AUTO){ bhAbortReason = "must be charging in Auto mode";     return false; }   // generator only runs in the AUTO control path
  // altSweepRequested too: a sweep start is two-stage (the HTTP press arms the request, the next
  // control tick promotes it to altSweepActive), so active alone leaves a one-tick window.
  if (TuningMode || CVTuningMode || systemIDActive || fieldCurveActive || fieldCutActive || cvPlantFitActive || resTestActive || cvStressActive || protTestActive || (altSweepActive != 0) || altSweepRequested || (chcActive != 0) || chcRequested) { bhAbortReason = "another test active"; return false; }
  if (!HAS_BATT_SHUNT){ bhAbortReason = "needs INA228 battery shunt"; return false; }
  if (bhNumEdges < 3) bhNumEdges = 3;
  if (bhNumEdges > BH_MAX_TOGGLES - 3) bhNumEdges = BH_MAX_TOGGLES - 3;
  if (bhDwellMs < bhMinDwellMs()) bhDwellMs = bhMinDwellMs();   // belt-and-braces; set paths already clamp
  // Total run must fit the sample buffer (~123 s at 8192×15 ms): bhSample stops appending when
  // full, edges past that point score nb<2/na<2, and the abort blames protections misleadingly.
  {
    uint32_t maxRunMs = (uint32_t)bhSampleCap * BH_SAMPLE_INTERVAL_MS;
    uint32_t needMs   = (uint32_t)(bhNumEdges + 3) * bhDwellMs;
    if (needMs > maxRunMs) {
      uint8_t fitEdges = (uint8_t)(maxRunMs / bhDwellMs);
      fitEdges = (fitEdges > 3) ? (uint8_t)(fitEdges - 3) : 0;
      if (fitEdges < 3) { bhAbortReason = "dwell too long for sample buffer"; return false; }
      queueConsoleMessageF("BATT HEALTH: %u edges x %lus exceeds the ~%lus sample buffer - running %u edges",
                           bhNumEdges, (unsigned long)(bhDwellMs / 1000), (unsigned long)(maxRunMs / 1000), fitEdges);
      bhNumEdges = fitEdges;
    }
  }
  bhSampleCount  = 0;
  bhSampleLastMs = 0;
  bhWaveHigh     = false;
  bhEdgeCount    = 0;
  bhToggleCount  = 0;
  bhTestStartMs  = millis();
  bhLastToggleMs = bhTestStartMs;
  bhAbortReason  = "";
  // Active flag FIRST: bhServiceCompletion (Core 1) reads (bhTestState==1 && !active) as run-complete.
  batteryHealthTestActive = true;
  bhTestState    = 1;
  queueConsoleMessageF("BATT HEALTH: DCIR test started (%.1f<->%.1fA, %lus dwell, %u edges)",
                       bhStepLowA, bhStepLowA + bhStepDeltaA, (unsigned long)(bhDwellMs / 1000), bhNumEdges);
  return true;
}

// A macro, not a helper: a free function taking BattHealthResult would get an Arduino auto-prototype hoisted above
// the struct definition. Keep ALL struct types out of free-function signatures.
#define BH_APPEND_RESULT(r) do {                         \
    bhResults[bhResultHead] = (r);                       \
    bhResultHead = (bhResultHead + 1) % bhResultCap;     \
    if (bhResultCount < bhResultCap) bhResultCount++;    \
  } while (0)

// Resistance at the dwell timescale (ohmic + polarization): the absolute value depends on dwell, the trend is what
// matters. A 3-level linearity sweep needs only generator + reporting changes; the per-edge math handles any ΔI.
void bhComputeDcir() {
  uint32_t bhT0 = micros();   // this fit runs in a field-on loop pass — report its cost so a spike is attributable
  if (bhSampleCount < 8 || bhToggleCount < (int)bhNumEdges + 3) { bhAbort("not enough samples"); return; }

  // Average the settled tail of each level (last `win` ms before its end toggle), so slew time doesn't matter.
  uint32_t win = bhDwellMs / 3; if (win < 200) win = 200;

  float Rsum = 0.0f, Rsq = 0.0f; int Rn = 0;   // Rsq → per-edge std-dev (fit consistency)
  const float rK = (float)SYSTEM_VOLTAGE_CLASS / 12.0f;   // mΩ is V/A: sanity bound scales with series blocks
  // Score toggle numbers k = 3 .. bhNumEdges+2 (1-indexed). Array index = k-1.
  for (int k = 3; k <= (int)bhNumEdges + 2; k++) {
    if (k >= bhToggleCount) break;
    uint32_t tEdge = bhToggleMs[k - 1];   // end of prior level
    uint32_t tNext = bhToggleMs[k];       // end of new level
    float vb = 0, ib = 0, va = 0, ia = 0;
    int   nb = 0, na = 0;
    for (int s = 0; s < bhSampleCount; s++) {
      uint32_t t = bhSamples[s].tMs;
      if (t + win >= tEdge && t < tEdge)        { vb += bhSamples[s].v; ib += bhSamples[s].i; nb++; }   // settled tail, prior level
      else if (t + win >= tNext && t < tNext)   { va += bhSamples[s].v; ia += bhSamples[s].i; na++; }   // settled tail, new level
    }
    if (nb < 2 || na < 2) continue;
    vb /= nb; ib /= nb; va /= na; ia /= na;
    float dV = va - vb, dI = ia - ib;
    if (fabsf(dI) < 0.5f * bhStepDeltaA) continue;     // step never manifested (protection clamp / field cut)
    float R = (dV / dI) * 1000.0f;                     // mΩ
    if (R < 0.1f * rK || R > 500.0f * rK) continue;    // sanity bound
    Rsum += R; Rsq += R * R; Rn++;
  }
  if (Rn < 2) { bhAbort("insufficient valid steps (protections firing or step too small?)"); return; }

  BattHealthResult r = {};
  r.epoch      = clockIsReal() ? (timeBase + (millis() - timeBaseMillis) / 1000) : 0;
  r.dcir_mOhm  = Rsum / Rn;
  r.soh_pct    = (bhCapCount > 0) ? bhCapRing[(bhCapHead - 1 + bhCapCap) % bhCapCap].capPct : NAN;
  r.battTempF  = battTempActiveF;   // measured battery source only (°F); NAN when none — the board temperature is NOT a substitute
  r.soc_pct    = socShownX100() / 100.0f;   // the SoC shown: the owner's selected monitor, else this shunt's count
  r.battV      = IBV;
  r.stepLowA   = bhStepLowA;
  r.stepDeltaA = bhStepDeltaA;
  r.edgesUsed  = (uint8_t)Rn;
  r.dwellMsUsed = (uint16_t)min(bhDwellMs, (uint32_t)65535);
  // population std-dev of the per-edge DCIR (r.dcir_mOhm is already the mean = Rsum/Rn)
  float bhVar = Rsq / Rn - r.dcir_mOhm * r.dcir_mOhm; if (bhVar < 0.0f) bhVar = 0.0f;
  r.fitSpread_mOhm = sqrtf(bhVar);
  BH_APPEND_RESULT(r);
  bhLastResultDcir = r.dcir_mOhm;
  bhTestState = 2;
  bhResultsDirty = true;   // NVS persist deferred to field-off — a test always ends field-on
  char tbuf[24];
  if (isfinite(r.battTempF)) snprintf(tbuf, sizeof(tbuf), "%.1f%s", dispTempF(r.battTempF), dispTempUnit());
  else                       snprintf(tbuf, sizeof(tbuf), "no batt temp");
  queueConsoleMessageF("BATT HEALTH: DCIR = %.2f mOhm (%d/%u edges, SoC %.0f%%, %s, fit %luus)",
                       r.dcir_mOhm, Rn, bhNumEdges, r.soc_pct, tbuf, (unsigned long)(micros() - bhT0));
  char tjson[16];
  if (isfinite(r.battTempF)) snprintf(tjson, sizeof(tjson), "%.1f", r.battTempF);
  else                       strcpy(tjson, "null");   // %.1f of NAN would emit bare nan and break the ledger row's JSON
  char ev[288];
  snprintf(ev, sizeof(ev),
           "{\"test\":\"batt_health\",\"ok\":1,\"dcir_mohm\":%.2f,\"spread_mohm\":%.2f,\"edges_used\":%d,"
           "\"edges_cfg\":%u,\"soc_pct\":%.1f,\"batt_temp_f\":%s,\"batt_v\":%.2f,"
           "\"low_a\":%.1f,\"delta_a\":%.1f,\"dwell_ms\":%u}",
           r.dcir_mOhm, r.fitSpread_mOhm, Rn, (unsigned)bhNumEdges, r.soc_pct, tjson, r.battV,
           r.stepLowA, r.stepDeltaA, (unsigned)r.dwellMsUsed);
  cxLedgerLogTest(ev);
}

// Called every loop() iteration. Cheap: a couple of compares unless a test is live.
void bhServiceCompletion() {
  if (bhTestState != 1) return;
  if (!batteryHealthTestActive) {           // control loop finished the last edge
    bhComputeDcir();
    return;
  }
  // Watchdog: if the engine stops mid-test the health branch stops toggling and the
  // test would hang RUNNING forever.
  uint32_t budget = (uint32_t)(bhNumEdges + 4) * bhDwellMs + 5000;
  if (millis() - bhTestStartMs > budget) bhAbort("timed out (engine stopped or left Auto mode?)");
}

// ════════════════════════ CV PLANT FIT (firmware voltage-loop identification) ════════════════════════
// Ohmic-anchor pulse train (CV_Gain_Ohmic_Anchor_Redesign_Spec.md): SETTLE → PILOT → HOLD (stepDuty) → REBASE
// (baseDuty) → 4× base/step PULSES → RELEASE; cvpfKa = median over 8 edges of settled ΔV/ΔI. Keep baseDuty captured at
// REBASE, one settle after the crest: captured at SETTLE, engine speed moves between the two and flattens every edge.
static const uint32_t CVPF_SAMPLE_INTERVAL_MS = 40;    // CC phases — ample for settled means; bounds buffer fill
static const uint32_t CVPF_SAMPLE_FAST_MS = 10;        // PULSES/RELEASE — tick-cadence sampling across the edges
static const uint32_t CVPF_SETTLE_MIN_MS = 2000;
static const uint32_t CVPF_SETTLE_TIMEOUT_MS = 15000;  // proceed with what settled — the fit divides by MEASURED ΔI
static const uint32_t CVPF_T_PILOT_MS  = 3000;
static const uint32_t CVPF_INBAND_MS   = 1000;         // measured-current dwell inside the band that counts as settled
static const uint32_t CVPF_HOLD_TIMEOUT_MS = 10000;
static const uint32_t CVPF_REBASE_SETTLE_MS = 6000;     // return-to-base settle allowance ON TOP of the fall-rate ramp (see the REBASE budget)
static const uint32_t CVPF_PULSE_SEG_MS = 2000;
static const uint32_t CVPF_COND_SKIP_MS = 900;         // wait out the post-edge reaction before averaging result-card conditions in a segment
static const uint32_t CVPF_EDGE_V0_MS = 550;           // ΔV readout ~600 ms after the edge — the timescale the CV loop
static const uint32_t CVPF_EDGE_V1_MS = 650;           //   reacts on; matched pair with cvAlpha. Inside the 2 s pulse hold
static const float    CVPF_DI_MAX_DEFAULT = 40.0f;     // baseline current-step ceiling; cvpfDiMaxA overrides per-run on a weak-signal re-run
static const float    CVPF_DI_MAX_CEIL    = 100.0f;    // absolute backstop for the operator-boosted step (cap-table clamp usually binds first)
static const float    CVPF_CAP_MARGIN_A   = 5.0f;      // keep the commanded step this far under the live cap-table ceiling (g_I_cap)
static const float    CVPF_V_RESERVE = 0.15f;          // keep the projected step peak this far below the hard-OV cut
static const float    CVPF_GASSING_V_12V = 14.2f;      // ~2.37 V/cell — oxygen-evolution onset on lead-acid

// Volt-domain class scale: everything this test measures in VOLTS (ΔV target, OV cushions) is per-cell and scales
// with the bus; everything it commands in AMPS (cvpfBaseA/PilotA, CVPF_DI_MAX_DEFAULT) is class-invariant.
static inline float cvpfVScale() { return (float)SYSTEM_VOLTAGE_CLASS / 12.0f; }

// Lithium does not evolve gas at any charge voltage we permit; every other chemistry does. "other" is treated
// as gassing: a false refusal costs the user a retry, a false pass ships an over-gained loop.
static bool cvpfChemGasses() {
  return strcasecmp(BATTERY_TYPE, "lifepo4") != 0;
}

void cvpfSample(uint32_t nowMs) {
  if (!cvpfBuf || cvpfBufCount >= cvpfBufCap) return;
  uint32_t interval = (cvpfPhase >= 4) ? CVPF_SAMPLE_FAST_MS : CVPF_SAMPLE_INTERVAL_MS;
  if (nowMs - cvpfSampleLastMs < interval) return;
  cvpfSampleLastMs = nowMs;
  cvpfBuf[cvpfBufCount].tMs  = nowMs;
  cvpfBuf[cvpfBufCount].v    = IBV;
  cvpfBuf[cvpfBufCount].iAlt = MeasuredAmps;
  cvpfBuf[cvpfBufCount].iBat = Bcur;
  cvpfBufCount++;
}

// Run-mean of conditions for the result card; called only from the settled tails of the two middle pulse segments
// (one step, one base), so it centers on the operating point.
static void cvpfAccumConditions() {
  float r = (float)RPM;
  if (cvpfCondN == 0) { cvpfRpmMinAtFit = r; cvpfRpmMaxAtFit = r; }
  else {
    if (r < cvpfRpmMinAtFit) cvpfRpmMinAtFit = r;
    if (r > cvpfRpmMaxAtFit) cvpfRpmMaxAtFit = r;
  }
  cvpfRpmSum += RPM; cvpfBattVSum += IBV; cvpfSocSum += socShownX100() / 100.0f; cvpfCondN++;
}

// One-pass stats over the sample buffer for [t0,t1): mean, linear slope (units per ms), RMS residual about
// the fitted line (ripple proxy). field: 0=v 1=iAlt 2=iBat. Returns sample count (0 → mean=NAN).
static int cvpfWinStats(uint32_t t0, uint32_t t1, int field, float &mean, float &slope, float &resid) {
  double sx = 0, sy = 0, sxx = 0, sxy = 0; int n = 0;
  for (int i = 0; i < cvpfBufCount; i++) {
    uint32_t t = cvpfBuf[i].tMs;
    if (t < t0 || t >= t1) continue;
    double x = (double)(t - t0);
    double y = (field == 0) ? cvpfBuf[i].v : (field == 1) ? cvpfBuf[i].iAlt : cvpfBuf[i].iBat;
    sx += x; sy += y; sxx += x * x; sxy += x * y; n++;
  }
  if (n == 0) { mean = NAN; slope = 0; resid = 0; return 0; }
  mean = (float)(sy / n);
  if (n >= 2) {
    double d = n * sxx - sx * sx;
    double b = (d > 1e-9) ? (n * sxy - sx * sy) / d : 0.0;
    double a = (sy - b * sx) / n;
    slope = (float)b;
    double ss = 0;
    for (int i = 0; i < cvpfBufCount; i++) {
      uint32_t t = cvpfBuf[i].tMs; if (t < t0 || t >= t1) continue;
      double x = (double)(t - t0);
      double y = (field == 0) ? cvpfBuf[i].v : (field == 1) ? cvpfBuf[i].iAlt : cvpfBuf[i].iBat;
      double e = y - (a + b * x); ss += e * e;
    }
    resid = (float)sqrt(ss / n);
  } else { slope = 0; resid = 0; }
  return n;
}

// Size the main step for cvpfTargetDV, capped by OV headroom (never below the pilot — need signal).
static void cvpfSizeStep() {
  // The pilot chord (~2-3 s) matches the 2 s pulse holds, so it sizes the step peak directly.
  float Klong    = cvpfPilotK;
  float Kupper   = Klong * 1.5f;                                             // pessimistic (bigger K → smaller ΔI)
  // Cushions scale with class (cvpfVScale): cvpfSNR divides by the pre-edge residual, which is bus RIPPLE (scales with
  // the bus), not sensor resolution. A fixed 300 mV would shrink the chord by 12/class (reads K high on lead-acid), and
  // an unscaled CVPF_V_RESERVE puts the step peak 4x closer to the hard-OV cut per cell at 48V.
  float headroom = fmaxf(0.05f, AlternatorHardShutdownV - cvpfPilotVbase - CVPF_V_RESERVE * cvpfVScale());
  float di = cvpfDiMaxA;
  di = fminf(di, headroom / fmaxf(1e-3f, Kupper));
  // Target-ΔV sizing — skipped on an operator-boosted re-run (cvpfBoosted), whose whole point is a
  // bigger swing for SNR; the OV-headroom and cap-table clamps still bound it, and the gassing gate
  // judges the projected peak. Never skipped after an OV fallback re-size (headroom already proved tight).
  if (!cvpfBoosted || cvpfFellBack)
    di = fminf(di, cvpfTargetDV / fmaxf(1e-3f, Klong));
  // Never command past what the alternator cap table allows at this RPM (the step rides on top of cvpfBaseA).
  // Skip when g_I_cap is unset/tiny (low RPM) — the delivery-short warn catches under-delivery instead.
  if (g_I_cap > cvpfBaseA + cvpfPilotA + CVPF_CAP_MARGIN_A)
    di = fminf(di, g_I_cap - cvpfBaseA - CVPF_CAP_MARGIN_A);
  di = roundf(di);
  if (di < cvpfPilotA) di = cvpfPilotA;
  if (di > cvpfDiMaxA) di = cvpfDiMaxA;
  cvpfStepA = di;
}

// Pulse-train settle/segment trackers (reset in cvpfStartTest)
static float    cvpfAmpsEma = 0.0f;
static uint32_t cvpfInBandMs = 0;
static int8_t   cvpfSeg = -1;      // current pulse segment; even = base duty, odd = step duty
static float    cvpfBaseAmps = 0.0f;    // settled output at SETTLE exit — pinned-output diagnostics reference
static float    cvpfSettleRpm = 0.0f;   // RPM at SETTLE exit — base-load anchor for the setup-drift advisory
static float    cvpfHoldAmps = 0.0f;    // settled output at HOLD exit (crest level) — pairs with the REBASE floor level
// RPM over the settled tails of the FIRST and LAST base segments of the train. Same commanded duty at both
// ends, so their difference is pure engine drift with the test's own load sag cancelled out.
static double   cvpfSeg0RpmSum = 0.0, cvpfSeg6RpmSum = 0.0;
static uint32_t cvpfSeg0RpmN = 0, cvpfSeg6RpmN = 0;
static char     (&cvpfDiagBuf)[256] = PsramPlace::make<char[256]>();       // composed diagnostic aborts (cvpfAbortMsg points here); longest message + UTF-8 dashes reaches ~231 bytes

// Called every control tick from the duty-override block in AdjustField (like fieldCut_tick). Returns
// true only while PULSES/RELEASE own duty; CC phases return false. Ends by clearing cvPlantFitActive.
bool cvpf_tick(float &dutyOut, float measA, uint32_t nowMs) {
  if (!cvPlantFitActive) return false;
  cvpfSample(nowMs);
  uint32_t elapsed = nowMs - cvpfPhaseStartMs;
  switch (cvpfPhase) {
    case 0: {  // SETTLE — current-PID at the base level; establishes the reference output and speed (REBASE captures the floor duty)
      cvpfCmdA = cvpfBaseA;
      cvpfAmpsEma += 0.05f * (measA - cvpfAmpsEma);
      bool arrived = fabsf(setpointLimited - cvpfBaseA) < 0.5f;
      bool inBand = arrived && fabsf(cvpfAmpsEma - cvpfBaseA) < fmaxf(2.0f, 0.15f * cvpfBaseA);
      if (inBand) { if (cvpfInBandMs == 0) cvpfInBandMs = nowMs; } else cvpfInBandMs = 0;
      bool settled = (elapsed >= CVPF_SETTLE_MIN_MS) && cvpfInBandMs && (nowMs - cvpfInBandMs >= CVPF_INBAND_MS);
      if (settled || elapsed >= CVPF_SETTLE_TIMEOUT_MS) {
        cvpfBaseAmps = cvpfAmpsEma; cvpfSettleRpm = (float)RPM;
        cvpfPhase = 1; cvpfPhaseStartMs = nowMs;
      }
      break; }
    case 1: {  // PILOT — probe the fast stiffness, then gassing-gate and size the main step
      cvpfCmdA = cvpfBaseA + cvpfPilotA;
      if (elapsed >= CVPF_T_PILOT_MS) {
        float vB, vP, iBn, iPn, sl, rs;
        cvpfWinStats(cvpfPhaseStartMs - 2500, cvpfPhaseStartMs - 200, 0, vB,  sl, rs);   // last 2.3s of SETTLE
        cvpfWinStats(cvpfPhaseStartMs - 2500, cvpfPhaseStartMs - 200, 1, iBn, sl, rs);
        cvpfWinStats(nowMs - 1500, nowMs - 200, 0, vP,  sl, rs);                          // last ~1.3s of PILOT
        cvpfWinStats(nowMs - 1500, nowMs - 200, 1, iPn, sl, rs);
        float dIp = iPn - iBn, dVp = vP - vB;
        // Pilot moved (almost) no current, so the main step can't either. Abort now: a fallback K would size a tiny
        // step and doom the whole train to flat, all-dropped edges.
        if (dIp < 2.0f) {
          if (boardCeilHoldsField())
            snprintf(cvpfDiagBuf, sizeof(cvpfDiagBuf),
                     "the +%.0f A pilot pulse moved output only %.1f A — the field sat at %s, which no setting raises; re-run at a higher engine speed, where the same output needs less field",
                     cvpfPilotA, dIp, fieldCeilBindName(g_fieldCeilBind));
          else
            snprintf(cvpfDiagBuf, sizeof(cvpfDiagBuf),
                     "the +%.0f A pilot pulse moved output only %.1f A — alternator at its output ceiling for this RPM, a limiter capping current, or the field circuit not carrying the step; fix and re-run",
                     cvpfPilotA, dIp);
          cvpfAbort(cvpfDiagBuf);
          break;
        }
        cvpfPilotK     = (dVp > 0.0f) ? (dVp / dIp) : (cvpfTargetDV / fmaxf(1.0f, cvpfPilotA));
        cvpfPilotVbase = isnan(vB) ? IBV : vB;
        cvpfSizeStep();
        // Gassing gate: past ~2.37 V/cell part of the current evolves oxygen, dragging dV/dI DOWN → over-gained loop.
        // Judged on baseline + projected peak of the step actually sized (stepA·pilotK), which tracks boosted and
        // cap-clamped steps.
        if (cvpfChemGasses() && isfinite(cvpfPilotVbase)
            && cvpfPilotVbase + cvpfStepA * cvpfPilotK > CVPF_GASSING_V_12V * (float)SYSTEM_VOLTAGE_CLASS / 12.0f) {
          cvpfAbort("bank too full to measure — this close to full, extra current stops raising the voltage in proportion, which reads the battery's resistance low and over-tunes the loop. Draw the bank down with some loads, then re-run.");
          break;
        }
        cvpfPhase = 2; cvpfPhaseStartMs = nowMs; cvpfInBandMs = 0;
        queueConsoleMessageF("CV plant-fit: pilotK=%.1f mV/A -> pulse step %.0f A; practice run through the current loop",
                             cvpfPilotK * 1000.0f, cvpfStepA);
      }
      break; }
    case 2: {  // HOLD — practice run at the sized step; capture the settled duty as the pulse crest
      float target = cvpfBaseA + cvpfStepA;
      cvpfCmdA = target;
      // Runtime OV guard: first trip → re-hold at a 180 mV (12V-equiv) step; second → abort. Both class-scaled: the
      // bus crosses a fixed cushion 4x faster per cell at 48V.
      if (IBV > AlternatorHardShutdownV - 0.10f * cvpfVScale()) {
        if (!cvpfFellBack) {
          cvpfFellBack = true; cvpfTargetDV = 0.18f * cvpfVScale(); cvpfWarn |= 0x10;
          cvpfSizeStep();
          cvpfPhaseStartMs = nowMs; cvpfInBandMs = 0;
          queueConsoleMessageF("CV plant-fit: over-voltage headroom tight — step reduced to %.0f A, re-testing", cvpfStepA);
        } else {
          cvpfAbort("over-voltage during fit even at the reduced step — lower the charge target or raise OV headroom, then re-run");
        }
        break;
      }
      cvpfAmpsEma += 0.05f * (measA - cvpfAmpsEma);
      bool arrived = fabsf(setpointLimited - target) < 0.5f;
      bool inBand = arrived && fabsf(cvpfAmpsEma - target) < fmaxf(3.0f, 0.08f * target);
      if (inBand) { if (cvpfInBandMs == 0) cvpfInBandMs = nowMs; } else cvpfInBandMs = 0;
      bool settled = (elapsed >= CVPF_SETTLE_MIN_MS) && cvpfInBandMs && (nowMs - cvpfInBandMs >= CVPF_INBAND_MS);
      // Timeout = step unreachable (railed PID): proceed, the fit divides by MEASURED ΔI — unless nothing was delivered
      // above base, when every replayed edge would drop (only reachable on the timeout path).
      if (settled || elapsed >= CVPF_HOLD_TIMEOUT_MS) {
        if (cvpfAmpsEma - cvpfBaseAmps < 2.0f) {
          if (boardCeilHoldsField())
            snprintf(cvpfDiagBuf, sizeof(cvpfDiagBuf),
                     "practice step didn't raise output (%.1f -> %.1f A with +%.0f A commanded) — the field sat at %s, which no setting raises; re-run at a higher engine speed, where the same output needs less field",
                     cvpfBaseAmps, cvpfAmpsEma, cvpfStepA, fieldCeilBindName(g_fieldCeilBind));
          else
            snprintf(cvpfDiagBuf, sizeof(cvpfDiagBuf),
                     "practice step didn't raise output (%.1f -> %.1f A with +%.0f A commanded) — alternator at its output ceiling for this RPM, or the field circuit didn't carry it; re-run once output can move",
                     cvpfBaseAmps, cvpfAmpsEma, cvpfStepA);
          cvpfAbort(cvpfDiagBuf);
          break;
        }
        cvpfStepDuty = lastAppliedDuty;
        cvpfHoldAmps = cvpfAmpsEma;
        cvpfPhase = 3; cvpfPhaseStartMs = nowMs; cvpfInBandMs = 0;
        queueConsoleMessageF("CV plant-fit: crest level captured (%.1f%% at %.1f A) — returning to base to capture its duty alongside it",
                             cvpfStepDuty, cvpfHoldAmps);
      }
      break; }
    case 3: {  // REBASE — current-PID back down to the base level; the floor duty is captured HERE, one
               // settle after the crest, so both replayed levels carry the same engine speed and plant gain
      cvpfCmdA = cvpfBaseA;
      if (IBV > AlternatorHardShutdownV - 0.10f * cvpfVScale()) {
        cvpfAbort("over-voltage while returning to the baseline current — lower the charge target or raise OV headroom, then re-run");
        break;
      }
      cvpfAmpsEma += 0.05f * (measA - cvpfAmpsEma);
      bool arrived = fabsf(setpointLimited - cvpfBaseA) < 0.5f;
      bool inBand = arrived && fabsf(cvpfAmpsEma - cvpfBaseA) < fmaxf(2.0f, 0.15f * cvpfBaseA);
      if (inBand) { if (cvpfInBandMs == 0) cvpfInBandMs = nowMs; } else cvpfInBandMs = 0;
      bool settled = (elapsed >= CVPF_SETTLE_MIN_MS) && cvpfInBandMs && (nowMs - cvpfInBandMs >= CVPF_INBAND_MS);
      // The walk down is fall-rate limited (TEST_ENTRY_RATE_A; 12.5 s for a boosted 100 A step), so the budget covers
      // the ramp first; a fixed timeout would capture a mid-ramp duty as the pulse floor.
      uint32_t budget = CVPF_REBASE_SETTLE_MS + (uint32_t)(1000.0f * cvpfStepA / TEST_ENTRY_RATE_A);
      if (elapsed >= budget && !arrived) {
        cvpfAbort("the current loop never walked back down to the baseline inside its time budget — re-run");
        break;
      }
      if (settled || elapsed >= budget) {
        cvpfBaseDuty = lastAppliedDuty;
        cvpfRebaseRpm = (float)RPM;
        // The train replays exactly this pair, so this gap IS every edge's ΔI; below 2 A the per-edge |dI| test rejects
        // all 8. Do not gate on RPM change here: the two speeds sit at different loads, so it rejects the test's own sag.
        float dIpair = cvpfHoldAmps - cvpfAmpsEma;
        if (dIpair < 2.0f) {
          snprintf(cvpfDiagBuf, sizeof(cvpfDiagBuf),
                   "the two field levels came out only %.1f A apart (%.1f%% / %.1f%% duty, %.1f / %.1f A) — the pulses would have no step to measure; re-run where the alternator has room to move",
                   dIpair, cvpfBaseDuty, cvpfStepDuty, cvpfAmpsEma, cvpfHoldAmps);
          cvpfAbort(cvpfDiagBuf);
          break;
        }
        // Restore the pre-test setpoint BEFORE first returning true — the SystemID resume snapshot fires
        // on this tick's rising edge and must not capture the test current (same trick as fieldCut_tick).
        setpointLimited = cvpfPreSetpoint;
        cvpfCcActive = false;
        cvpfSeg = 0;
        cvpfPhase = 4; cvpfPhaseStartMs = nowMs;
        queueConsoleMessageF("CV plant-fit: duties captured (base %.1f%% / step %.1f%%, %.1f A apart) — pulsing",
                             cvpfBaseDuty, cvpfStepDuty, dIpair);
        dutyOut = cvpfBaseDuty;
        return true;
      }
      break; }
    case 4: {  // PULSES — abrupt duty squares; every segment boundary is a measured edge
      // Same class-scaled cushion as HOLD/REBASE; matters most here since direct-duty edges have no rise pacing.
      if (IBV > AlternatorHardShutdownV - 0.10f * cvpfVScale()) {
        cvpfAbort("over-voltage during the pulse train — re-run once charging is steady");
        break;
      }
      int seg = (int)(elapsed / CVPF_PULSE_SEG_MS);   // 0,2,4,6 = base; 1,3,5,7 = step
      if (seg > 7) {
        if (cvpfEdgeCount < 8) cvpfEdgeMs[cvpfEdgeCount++] = nowMs;   // final step→base edge opens RELEASE
        cvpfPhase = 5; cvpfPhaseStartMs = nowMs;
        dutyOut = cvpfBaseDuty;
        return true;
      }
      if (seg != cvpfSeg) {
        if (seg > 0 && cvpfEdgeCount < 8) cvpfEdgeMs[cvpfEdgeCount++] = nowMs;   // this tick writes the new duty
        cvpfSeg = seg;
      }
      bool tail = (elapsed - (uint32_t)seg * CVPF_PULSE_SEG_MS) >= CVPF_COND_SKIP_MS;   // past the post-edge reaction
      // Result-card conditions: the two middle segments (3 = high step, 4 = low base), settled tail only —
      // one high + one low centers the reported voltage on the operating point being characterized.
      if ((seg == 3 || seg == 4) && tail) cvpfAccumConditions();
      // Drift readout: first vs last BASE segment. Identical commanded duty at both ends, so whatever
      // separates them is real engine drift across the measurement — not the step's load sag.
      if (seg == 0 && tail) { cvpfSeg0RpmSum += RPM; cvpfSeg0RpmN++; }
      if (seg == 6 && tail) { cvpfSeg6RpmSum += RPM; cvpfSeg6RpmN++; }
      dutyOut = (seg & 1) ? cvpfStepDuty : cvpfBaseDuty;
      return true; }
    case 5: {  // RELEASE — settled base hold that closes the final edge's ΔI window, then hand back
      dutyOut = cvpfBaseDuty;
      if (elapsed >= CVPF_PULSE_SEG_MS) {
        cvPlantFitActive = false;   // resume block reseeds the PID bumplessly at the applied (base) duty
        return false;
      }
      return true; }
  }
  return false;   // current-PID phases — the cvpfCcActive branch drives
}

// Median over n floats (n ≤ 8) — insertion sort on a copy.
static float cvpfMedian(const float *v, int n) {
  float s[8];
  for (int i = 0; i < n; i++) s[i] = v[i];
  for (int i = 1; i < n; i++) {
    float x = s[i]; int j = i - 1;
    while (j >= 0 && s[j] > x) { s[j + 1] = s[j]; j--; }
    s[j + 1] = x;
  }
  return (n & 1) ? s[n / 2] : 0.5f * (s[n / 2 - 1] + s[n / 2]);
}

// Per edge: ΔV = mean[tE+550,650 ms] − pre-edge baseline; ΔI = settled means either side (so current-
// sensor SPEED is irrelevant — the fast transient is all on the INA228 voltage). K = ΔV/ΔI, median over
// edges: a stretched tick or a load switch under one edge drops that edge instead of biasing the fit.
void cvpfProcess() {
  uint32_t tP = micros();
  cvpfReady = true;
  if (cvpfEdgeCount < 4 || cvpfBufCount < 50) { cvpfOk = false; cvpfState = 3; cvpfAbortMsg = "too few usable samples — re-run"; return; }
  bool noShunt = !HAS_BATT_SHUNT;
  int iField = noShunt ? 1 : 2;   // settled ΔI source: calibrated ADS1115 alt current, or the battery shunt
  float Ks[8], dVs[8], dIs[8], dIalts[8];
  float ripSum = 0.0f; int nK = 0, nAlt = 0, ripN = 0;
  for (int i = 0; i < 8; i++) { cvpfEdgeK[i] = 0.0f; cvpfEdgeStat[i] = 4; }   // 4 = not fired (< 8 edges recorded)
  for (int e = 0; e < cvpfEdgeCount; e++) {
    uint32_t tE = cvpfEdgeMs[e];
    float vPre, vPost, iPre, iPost, iaPre, iaPost, sl, rip, rs;
    int n1 = cvpfWinStats(tE - 600, tE - 100, 0, vPre, sl, rip);
    int n2 = cvpfWinStats(tE + CVPF_EDGE_V0_MS, tE + CVPF_EDGE_V1_MS, 0, vPost, sl, rs);
    int n3 = cvpfWinStats(tE - 1100, tE - 100, iField, iPre, sl, rs);
    int n4 = cvpfWinStats(tE + 500, tE + 1900, iField, iPost, sl, rs);
    int n5 = cvpfWinStats(tE - 1100, tE - 100, 1, iaPre, sl, rs);
    int n6 = cvpfWinStats(tE + 500, tE + 1900, 1, iaPost, sl, rs);
    if (n5 >= 3 && n6 >= 5) { dIalts[nAlt++] = fabsf(iaPost - iaPre); }   // delivery/gap-drift reference
    if (n1 < 3 || n2 < 2 || n3 < 3 || n4 < 5) { cvpfEdgeStat[e] = 3; continue; }   // a stretched tick starved a window — drop the edge
    float dV = vPost - vPre, dI = iPost - iPre;
    if (fabsf(dI) < 1.0f) { cvpfEdgeStat[e] = 1; continue; }               // no real current edge under this one
    float K = dV / dI;
    cvpfEdgeK[e] = K * 1000.0f;                                            // report the signed reading even when rejected
    if (!(K > 1e-5f)) { cvpfEdgeStat[e] = 2; continue; }                   // wrong-way edge — a load switched under it
    cvpfEdgeStat[e] = 0;
    Ks[nK] = K; dVs[nK] = fabsf(dV); dIs[nK] = fabsf(dI);
    ripSum += rip; ripN++;
    nK++;
  }
  float dIaltMed = (nAlt > 0) ? cvpfMedian(dIalts, nAlt) : 0.0f;
  if (nK < 3) {
    cvpfOk = false; cvpfState = 3;
    // Diagnose from what the run recorded instead of guessing: identical captured duties = the
    // setup phases were pinned (floor/ceiling/limiter); duty stepped but current didn't = ceiling
    // or field circuit; current stepped but edges still dropped = disturbed/starved voltage windows.
    float dStepPct = cvpfStepDuty - cvpfBaseDuty;
    if (dStepPct < 1.5f * fieldScale()) {   // 1.5 % is the 12 V-winding-on-12 V gap
      snprintf(cvpfDiagBuf, sizeof(cvpfDiagBuf),
               "pulse edges unusable — the two field levels came out nearly identical (%.1f%% / %.1f%%) even though they were learned back to back: output was pinned at a floor, a ceiling, or a limiter. Re-run where the alternator has room to move",
               cvpfBaseDuty, cvpfStepDuty);
      cvpfAbortMsg = cvpfDiagBuf;
    } else if (dIaltMed < 1.0f) {
      snprintf(cvpfDiagBuf, sizeof(cvpfDiagBuf),
               "pulse edges unusable — field duty stepped %.1f%% -> %.1f%% but output current moved only %.1f A: alternator at its output ceiling for this RPM, or the field circuit didn't carry the step. Check the field connection and re-run",
               cvpfBaseDuty, cvpfStepDuty, dIaltMed);
      cvpfAbortMsg = cvpfDiagBuf;
    } else {
      cvpfAbortMsg = noShunt ? "pulse edges unusable — current stepped but the voltage read windows were disturbed; hold RPM and loads steady and re-run"
                             : "pulse edges unusable — no clean battery-current step; re-run with loads steady";
    }
    return;
  }
  cvpfKa = cvpfMedian(Ks, nK);
  cvpfKb = 0.0f;
  cvpfK  = cvpfKa;
  cvpfDV = cvpfMedian(dVs, nK);
  cvpfDI = cvpfMedian(dIs, nK);
  float ripple = (ripN > 0) ? (ripSum / ripN) : 0.0f;
  cvpfSNR = cvpfDV / fmaxf(1e-4f, ripple);
  // advisory gates (never block — the wizard shows them and the user still chooses Apply)
  if (nK < 5) cvpfWarn |= 0x04;                                          // edges dropped (stretched ticks / disturbances)
  if (dIaltMed / fmaxf(1e-3f, cvpfStepA) < 0.6f) cvpfWarn |= 0x01;       // delivery came up short
  if (cvpfSNR < 12.0f) cvpfWarn |= 0x02;                                 // weak signal vs ripple
  if (!noShunt && dIaltMed > 0.5f
      && fabsf(dIaltMed - cvpfDI) / dIaltMed > 0.15f) cvpfWarn |= 0x08;  // alt vs battery step disagree ⇒ a load moved
  // Engine drift between LIKE operating points only (SETTLE vs REBASE exit = base current; seg 0 vs 6 = base duty);
  // across the step it would read the test's own load sag. Advisory: the median fit makes drift second-order.
  float r0 = cvpfSeg0RpmN ? (float)(cvpfSeg0RpmSum / cvpfSeg0RpmN) : 0.0f;
  float r6 = cvpfSeg6RpmN ? (float)(cvpfSeg6RpmSum / cvpfSeg6RpmN) : 0.0f;
  cvpfDriftSetupPct = (cvpfSettleRpm > 100.0f && cvpfRebaseRpm > 0.0f)
                        ? 100.0f * (cvpfRebaseRpm - cvpfSettleRpm) / cvpfSettleRpm : 0.0f;
  cvpfDriftTrainPct = (r0 > 100.0f && r6 > 0.0f) ? 100.0f * (r6 - r0) / r0 : 0.0f;
  if (fabsf(cvpfDriftSetupPct) > 10.0f || fabsf(cvpfDriftTrainPct) > 10.0f) cvpfWarn |= 0x20;
  // display Kp/Ki (mirrors recomputeCvGains; recomputeCvGains is authoritative when the user Applies Ka)
  float vNorm = 12.0f / (float)SYSTEM_VOLTAGE_CLASS;
  float kp = cvAlpha / fmaxf(1e-6f, cvpfKa * vNorm);
  float ki = cvPiZero * kp;                   // from the UN-clamped Kp, then clamp both — matches recomputeCvGains
  cvpfKp = clamp_f(kp, 2.0f, 120.0f);
  cvpfKi = clamp_f(ki, 1.0f, 80.0f);
  // Result-card conditions: mean over the settled tail of the two middle pulse segments (~1 s each). Fall back
  // to the instantaneous read only if the pulse train never reached those segments (aborted mid-run).
  cvpfRpmAtFit   = cvpfCondN ? (float)(cvpfRpmSum   / cvpfCondN) : RPM;
  cvpfBattVAtFit = cvpfCondN ? (float)(cvpfBattVSum / cvpfCondN) : IBV;
  cvpfSocAtFit   = (socInfoAvailable && socShownOk() && cvpfCondN) ? (float)(cvpfSocSum / cvpfCondN) : -1.0f;  // −1 → SOC untrustworthy, UI blanks it
  cvpfCapHeadroomA = fmaxf(0.0f, g_I_cap - cvpfBaseA - CVPF_CAP_MARGIN_A);   // same clamp cvpfSizeStep applies — the step a re-run can actually get
  cvpfOk = true; cvpfState = 2;
  queueConsoleMessageF("CV plant-fit: K=%.1f mV/A (median of %d/%d edges) dV=%.0f mV dI=%.2f A SNR=%.0f -> Kp %.1f Ki %.1f warn=%d drift setup %+.1f%% train %+.1f%% (%luus)",
                       cvpfKa * 1000.0f, nK, (int)cvpfEdgeCount, cvpfDV * 1000.0f, cvpfDI, cvpfSNR, cvpfKp, cvpfKi, (int)cvpfWarn,
                       cvpfDriftSetupPct, cvpfDriftTrainPct, (unsigned long)(micros() - tP));
}

bool cvpfStartTest(float diMaxReq) {
  if (cvpfState == 1)           { cvpfAbortMsg = "already running";        return false; }
  const char *refused = nullptr;
  if (!cvpfBuf)                      refused = "buffer unallocated";
  else if (rpmTestRefuseReason())    refused = rpmTestRefuseReason();   // engine running, the drift advisory and the result's speed
  else if (RPM < 100)                refused = "engine not running";
  else if (sysMode != SYS_MODE_AUTO) refused = "must be charging in Auto mode";
  else if (TuningMode || CVTuningMode || systemIDActive || batteryHealthTestActive || resTestActive || fieldCurveActive || fieldCutActive || cvStressActive || protTestActive || (altSweepActive != 0) || altSweepRequested || (chcActive != 0) || chcRequested)
    refused = "another test active";
  if (refused) {
    // Published as a failed run: the wizard polls /cvplantfit.json after Start, and a stale ready/ok result would
    // read as this run's (and Apply would write its gains).
    cvpfAbortMsg = refused;
    cvpfOk = false; cvpfReady = true; cvpfState = 3;
    for (int i = 0; i < 8; i++) { cvpfEdgeK[i] = 0.0f; cvpfEdgeStat[i] = 4; }
    return false;
  }
  cvpfBufCount = 0; cvpfSampleLastMs = 0;
  cvpfPhase = 0;
  cvpfPhaseStartMs = millis(); cvpfTestStartMs = cvpfPhaseStartMs;
  cvpfBaseA = 10.0f; cvpfPilotA = 6.0f; cvpfStepA = 6.0f;
  cvpfTargetDV = 0.30f * cvpfVScale(); cvpfFellBack = false;  // 300 mV per 12V block — ripple-limited SNR, so per-cell (see cvpfVScale)
  cvpfCmdA = cvpfBaseA;
  // Any request is a boost, including ones under the 40 A default: the default is a ceiling the 300 mV target
  // or the cap table almost always undercuts, so gating the boost on >40 A silently re-ran the same step.
  cvpfBoosted = (diMaxReq > 0.0f);
  cvpfDiMaxA = cvpfBoosted ? fminf(fmaxf(diMaxReq, cvpfPilotA), CVPF_DI_MAX_CEIL) : CVPF_DI_MAX_DEFAULT;
  if (cvpfBoosted)
    queueConsoleMessageF("CV plant-fit: stronger step requested — up to %.0f A (weak-signal re-run)", cvpfDiMaxA);
  cvpfBaseDuty = 0.0f; cvpfStepDuty = 0.0f;
  cvpfEdgeCount = 0;
  for (int i = 0; i < 8; i++) { cvpfEdgeK[i] = 0.0f; cvpfEdgeStat[i] = 4; }   // clear the per-edge table; an early abort shows none
  cvpfPreSetpoint = setpointLimited;
  cvpfAmpsEma = MeasuredAmps; cvpfInBandMs = 0; cvpfSeg = -1;
  cvpfHorizonS = (CVPF_EDGE_V0_MS + CVPF_EDGE_V1_MS) / 2000.0f;   // the ~0.6 s edge readout (display only)
  cvpfReady = false; cvpfOk = false; cvpfWarn = 0; cvpfAbortMsg = "";
  cvpfK = cvpfDV = cvpfDI = cvpfSNR = 0.0f; cvpfKp = cvpfKi = 0.0f;
  cvpfRpmSum = cvpfBattVSum = cvpfSocSum = 0.0; cvpfCondN = 0;
  cvpfRpmMinAtFit = cvpfRpmMaxAtFit = 0.0f;
  cvpfSeg0RpmSum = cvpfSeg6RpmSum = 0.0; cvpfSeg0RpmN = cvpfSeg6RpmN = 0;
  cvpfSettleRpm = cvpfRebaseRpm = 0.0f; cvpfHoldAmps = 0.0f;
  cvpfDriftSetupPct = cvpfDriftTrainPct = 0.0f;
  cvpfCcActive = true;
  // Active flag before state=1: cvpfServiceCompletion on Core 1 reads (cvpfState==1 && !active)
  // as run-complete, so state must never be 1 while the active flag is still false.
  cvPlantFitActive = true;
  cvpfState = 1;
  queueConsoleMessage("CV plant-fit: started (settle -> size -> practice run -> return to base -> 4 abrupt duty pulses, ~0.6s stiffness read)");
  return true;
}

void cvpfAbort(const char *reason) {
  cvpfState = 3; cvpfReady = true; cvpfOk = false;   // state first, as in bhAbort
  cvPlantFitActive = false; cvpfCcActive = false;
  cvpfAbortMsg = reason;
  queueConsoleMessageF("CV plant-fit: aborted — %s", reason);
  cxLedgerLogCvpf();
}

// Called every loop(): runs the fit once the control-loop branch finishes, plus a hang watchdog.
void cvpfServiceCompletion() {
  if (cvpfState != 1) return;
  if (!cvPlantFitActive) { cvpfProcess(); cxLedgerLogCvpf(); return; }   // tick machine finished the RELEASE phase
  // Worst case: settle timeout + pilot + two hold timeouts (OV re-size) + REBASE at its full budget
  // (fall-rate ramp of the largest boosted step, plus its settle allowance) + 9 segments + slack.
  uint32_t budget = CVPF_SETTLE_TIMEOUT_MS + CVPF_T_PILOT_MS + 2 * CVPF_HOLD_TIMEOUT_MS
                    + CVPF_REBASE_SETTLE_MS + (uint32_t)(1000.0f * CVPF_DI_MAX_CEIL / TEST_ENTRY_RATE_A)
                    + 9 * CVPF_PULSE_SEG_MS + 8000;
  if (millis() - cvpfTestStartMs > budget) cvpfAbort("timed out (engine stopped or left Auto mode?)");
}

// ═══════════════ CV STRESS TEST (commissioning stage 8 / standalone Tuning ▸ Stress Test) ═══════════════
// Provokes OV protection at any SoC: park the bus at an ACHIEVABLE CV target (idle mean − CvStressDropV, reachable
// since idle held above it), then the operator snaps the throttle. Phases: 1 STAB_IDLE (steady + settled-up),
// 2 STAB_CV (re-settled within half the headroom, so the approach is over before arming), 3 ARMED (count EVERY
// protection edge: rapid re-clamping IS the defect), 4 DONE (firmware ends it 5 s after re-settle). The target is driven
// via the cvStressForceCV hook; every protection stays armed. Every ending queues a "test" ledger row.
static const uint32_t CVS_IDLE_MIN_MS       = 10000;   // min idle charge before the stability gate opens
static const uint32_t CVS_STAB_WIN_MS       = 3000;    // stationarity window (idle and CV)
static const float    CVS_STAB_BAND_V       = 0.10f;   // max−min of the filtered bus over the window (×V/12) — setup gate, not the graded subject; keep loose enough that normal idle ripple passes
// The fail band (CvStressFailBandV) and target headroom (CvStressDropV) are user settings — Xregulator.ino globals.
static const uint32_t CVS_TARGET_AVG_MS     = 2000;    // averaging window for the achievable target
// Settle-up gate: charging keeps nudging the bus up after the 3 s window looks flat; a target fixed against a rising
// average reads low and the parked current dies away, leaving the current limit (not CV) in command.
static const float    CVS_SETTLEUP_BAND_V   = 0.10f;   // max drift (×V/12) of IBV_filtered around the settle reference
static const uint32_t CVS_SETTLEUP_WIN_MS   = 10000;   // drift-free hold required before the target is fixed
static const uint32_t CVS_SETTLEUP_MAX_MS   = 30000;   // settle-up wait cap — proceed anyway so a slow bank can't stall the test
static const float    CVS_RESETTLE_BAND_V   = 0.15f;   // |bus − target| counted as at setpoint (×V/12), post-snap; the ARM gate uses min(this, ½ CvStressDropV)
static const uint32_t CVS_SETTLE_MS         = 5000;    // sustained in-band + stable = recovered / end
static const uint32_t CVS_IDLESTAB_TO_MS    = 45000;   // idle stability deadline — tight band missed: proceed wobbly (≤ fail band) or basic-stability fail
static const uint32_t CVS_CVSTAB_TO_MS      = 90000;   // CV settle deadline: the Ki-paced approach has τ ~25 s, so allow several time constants; tiered rule at the deadline like idle
static const uint32_t CVS_WATCH_MAX_MS      = 45000;   // armed watch backstop (never re-settles → no-recovery)
static const uint32_t CVS_POLL_DEADMAN_MS   = 20000;   // no /cvstress.json poll while active = browser gone
static const float    CVS_SNAP_RPM_DELTA    = 300.0f;  // rpmMax over arm RPM that counts as "a snap happened"
static const float    CVS_SLEW_SOFT_RPMPS   = 1000.0f; // no trip + slew below this = soft stimulus (snap harder)
static const uint32_t CVS_EVENT_MERGE_MS    = 300;     // re-trip this soon after a release = same surge, not a new event; logged chatter laps are ≥1.5 s

// Filtered-bus sample ring for the CVS_STAB_BAND_V / CVS_STAB_WIN_MS stationarity gate + the 2 s target average.
static const int CVS_RING_N = 64;
static float    cvsRingV[CVS_RING_N];
static uint32_t cvsRingMs[CVS_RING_N];
static int      cvsRingHead = 0, cvsRingCount = 0;
static uint32_t cvsRingStartMs = 0, cvsRingLastMs = 0;
static void cvsStabReset(uint32_t nowMs) { cvsRingHead = cvsRingCount = 0; cvsRingStartMs = nowMs; cvsRingLastMs = 0; }
static void cvsStabPush(uint32_t nowMs, float v) {
  if (cvsRingLastMs != 0 && (uint32_t)(nowMs - cvsRingLastMs) < 100UL) return;   // ~10 Hz
  cvsRingLastMs = nowMs;
  cvsRingV[cvsRingHead] = v; cvsRingMs[cvsRingHead] = nowMs;
  cvsRingHead = (cvsRingHead + 1) % CVS_RING_N;
  if (cvsRingCount < CVS_RING_N) cvsRingCount++;
}
static bool cvsStabReady(uint32_t nowMs, uint32_t winMs) { return (uint32_t)(nowMs - cvsRingStartMs) >= winMs && cvsRingCount >= 3; }
static float cvsStabRange(uint32_t nowMs, uint32_t winMs) {   // max−min over the last winMs, order-independent
  float lo = 1e9f, hi = -1e9f;
  for (int i = 0; i < cvsRingCount; i++)
    if ((uint32_t)(nowMs - cvsRingMs[i]) <= winMs) { if (cvsRingV[i] < lo) lo = cvsRingV[i]; if (cvsRingV[i] > hi) hi = cvsRingV[i]; }
  return (hi >= lo) ? (hi - lo) : 0.0f;
}
static float cvsStabMean(uint32_t nowMs, uint32_t winMs) {
  float sum = 0.0f; int n = 0;
  for (int i = 0; i < cvsRingCount; i++)
    if ((uint32_t)(nowMs - cvsRingMs[i]) <= winMs) { sum += cvsRingV[i]; n++; }
  return n ? sum / n : IBV_filtered;
}

// Module state (all consumed within one core; result fields read by the JSON builder)
static uint32_t cvsPhaseStartMs = 0, cvsTestStartMs = 0, cvsLastEndMs = 0;
static volatile uint32_t cvsLastPollMs = 0;
static uint32_t cvsRpmLowSinceMs = 0;
static float    cvsIdleAvgV = 0.0f, cvsBattStartA = 0.0f, cvsArmRpm = 0.0f;
// Alternator amps at arm — the standing current the snap must shed. Battery current (cvsBattStartA)
// can read ~0 there while the alternator still carries house loads, so it can't be the discriminator.
static float    cvsArmAltA = 0.0f;
// Idle-wait charge-current steadiness (report-only): alternator amps at the stability-ring cadence.
static float    cvsCcSumA = 0.0f, cvsCcMinA = 0.0f, cvsCcMaxA = 0.0f, cvsCcAvgA = 0.0f, cvsCcRipA = 0.0f;
static int      cvsCcN = 0;
// settle-up gate state (phase 1): reference re-anchors whenever the filtered bus drifts beyond the band
static float    cvsSettleRefV = 0.0f;
static uint32_t cvsSettleRefMs = 0;
// arm baselines (raw protection counters)
static uint32_t cvsClamp0 = 0, cvsClampPrev = 0, cvsHoc0 = 0, cvsLd0 = 0, cvsIx0 = 0;
static bool     cvsClampWas = false;   // merged protection flag as last seen by the watcher — release detection
static uint32_t cvsReleaseMs = 0;      // last merged-flag release (0 = none since arm)
// watch-window trackers
static float    cvsPeakV = 0.0f, cvsMinV = 0.0f, cvsOvershootV = 0.0f, cvsValleyV = 0.0f;
static float    cvsRpmMax = 0.0f, cvsRpmSlewMax = 0.0f, cvsRpmPrev = 0.0f;
static uint32_t cvsRpmPrevMs = 0;
static uint16_t cvsEvents = 0, cvsHardCuts = 0, cvsIxTrips = 0;
static float    cvsWobbleIdleV = 0.0f, cvsWobbleCvV = 0.0f, cvsWobblePostV = 0.0f;  // measured 3 s swing (max−min) at each phase's settle, clean or loose; 0 = that phase never completed
static uint32_t cvsMaxLockoutMs = 0, cvsFirstEventMs = 0, cvsResettleSinceMs = 0, cvsRecoveryMs = 0;
static uint32_t cvsProvokedMs = 0;   // first provocation (event or snap) — the recovery watch runs from here
static bool     cvsSnapDetected = false, cvsRecovered = false, cvsSettled = false, cvsSoftStimulus = false;
static bool     cvsLoosePost = false;   // post-snap settle was outside the tight band — drives the marginal stability grade (the wobble V is recorded even on a clean settle)
static uint8_t  cvsFailPhase = 0;       // basic-stability fail kind: 1 idle swing / 2 CV approach / 3 target unreachable (bank too stiff); 0 = armed. Named by the web card
// grade: 0 pass, 1 marginal, 2 fail, 3 n/a. outcome: 1 rode-it-out, 2 event-graded, 3 stability-fail
static uint8_t  cvsOutcome = 0;
static uint8_t  cvsGPreStab = 0, cvsGEvents = 3, cvsGHardCut = 3, cvsGRecovery = 3, cvsGStab = 3, cvsGOverall = 3;
static bool     cvsReady = false, cvsOk = false, cvsAborted = false;

bool cvStressStartTest() {
  if (cvStressActive)           { cvStressAbortMsg = "already running";      return false; }
  if (const char *rr = rpmTestRefuseReason()) { cvStressAbortMsg = rr;      return false; }   // stall and throttle-snap detection read the tach
  if (RPM < 100)                { cvStressAbortMsg = "engine not running";   return false; }
  if (sysMode != SYS_MODE_AUTO) { cvStressAbortMsg = "must be charging in Auto mode"; return false; }
  if (TuningMode || CVTuningMode || systemIDActive || batteryHealthTestActive || resTestActive || fieldCurveActive || fieldCutActive || cvPlantFitActive || protTestActive || (altSweepActive != 0) || altSweepRequested || (chcActive != 0) || chcRequested) {
    cvStressAbortMsg = "another test active"; return false;
  }
  if (millis() - cvsLastEndMs < 2000UL) { cvStressAbortMsg = "cooling down — retry in a moment"; return false; }
  uint32_t now = millis();
  cvStressPhase = 1;
  cvsPhaseStartMs = cvsTestStartMs = now;
  cvsLastPollMs = 0; cvsRpmLowSinceMs = 0;
  cvStressForceCV = false;
  cvStressTargetV = 0.0f; cvsIdleAvgV = 0.0f; cvsArmRpm = 0.0f;
  cvsEvents = cvsHardCuts = cvsIxTrips = 0;
  cvsMaxLockoutMs = 0; cvsFirstEventMs = 0; cvsResettleSinceMs = 0; cvsRecoveryMs = 0;
  cvsProvokedMs = 0;
  cvsArmAltA = 0.0f;
  cvsCcSumA = cvsCcMinA = cvsCcMaxA = cvsCcAvgA = cvsCcRipA = 0.0f; cvsCcN = 0;
  cvsSnapDetected = cvsRecovered = cvsSettled = cvsSoftStimulus = cvsLoosePost = false;
  cvsOvershootV = cvsValleyV = 0.0f;
  cvsWobbleIdleV = cvsWobbleCvV = cvsWobblePostV = 0.0f;
  cvsPeakV = cvsMinV = IBV; cvsRpmMax = RPM; cvsRpmSlewMax = 0.0f;
  cvsOutcome = 0; cvsFailPhase = 0;
  cvsGPreStab = 0; cvsGEvents = cvsGHardCut = cvsGRecovery = cvsGStab = cvsGOverall = 3;
  cvsReady = cvsOk = cvsAborted = false;
  cvStressAbortMsg = "";
  cvStressAbortRequested = false;
  cvsStabReset(now);
  cvsSettleRefV = IBV_filtered; cvsSettleRefMs = now;
  cvStressActive = true;
  queueConsoleMessage("CV stress test: started — charging at idle while the battery settles (typically 10-45 s)");
  return true;
}

void cvStressAbort(const char *reason) {
  cvStressForceCV = false;
  cvStressActive = false;
  cvsReady = true; cvsOk = false; cvsAborted = true;
  cvStressAbortMsg = reason;
  cvsLastEndMs = millis();
  queueConsoleMessageF("CV stress test: aborted — %s", reason);
  // Aborts are fleet data too — Cancel/X, lost browser, engine stall all land here.
  char ev[224];
  snprintf(ev, sizeof(ev), "{\"test\":\"cv_stress\",\"abort\":\"%s\",\"phase\":%d,\"target_v\":%.2f}",
           reason, (int)cvStressPhase, cvStressTargetV);
  cxLedgerLogTest(ev);
}

// Graded result persisted (survives reload/reboot) and queued as a "test" ledger row. Positional ver-2 blob (V×100,
// A×10); the web parser reads the leading version and missing trailing fields as 0, so new fields go at the end.
// Bands = 12V-equiv constant × V/12 at grade time.
static void cvStressPersistResult() {
  const float k = (float)SYSTEM_VOLTAGE_CLASS / 12.0f;
  snprintf(cvsLastBlob, sizeof(cvsLastBlob),
           "2,%d,%d,%d,%d,%d,%d,%d,%lu,%lu,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
           (int)cvsOutcome, (int)cvsGOverall, (int)cvsGPreStab, (int)cvsGEvents, (int)cvsGHardCut,
           (int)cvsGRecovery, (int)cvsGStab, (unsigned long)cvsRecoveryMs, (unsigned long)cvsMaxLockoutMs,
           (int)cvsEvents, (int)cvsHardCuts, (int)cvsIxTrips,
           (int)lroundf(cvsOvershootV * 100.0f), (int)lroundf(cvsValleyV * 100.0f),
           (int)lroundf(cvsPeakV * 100.0f), (int)lroundf(cvsMinV * 100.0f),
           (int)lroundf(cvsRpmSlewMax), (int)lroundf(cvsRpmMax),
           (int)lroundf(cvsBattStartA * 10.0f), (int)lroundf(cvStressTargetV * 100.0f),
           (int)(cvsSoftStimulus ? 1 : 0),
           (int)lroundf(cvsWobbleIdleV * 100.0f), (int)lroundf(cvsWobbleCvV * 100.0f),
           (int)lroundf(cvsWobblePostV * 100.0f),
           (int)lroundf(CVS_STAB_BAND_V * k * 100.0f), (int)lroundf(CvStressFailBandV * k * 100.0f),
           (int)cvsFailPhase,
           (int)lroundf(cvsArmAltA * 10.0f), (int)lroundf(cvsCcAvgA * 10.0f), (int)lroundf(cvsCcRipA * 10.0f));
  settingWrite(NK_cvStressLast, cvsLastBlob);
  char ev[280];
  snprintf(ev, sizeof(ev), "{\"test\":\"cv_stress\",\"result\":\"%s\"}", cvsLastBlob);
  cxLedgerLogTest(ev);
}

// Grade a run that reached ARMED (armed → snapped). Overshoot/valley are reported, never gated.
static void cvStressGrade() {
  cvsGEvents  = (cvsEvents <= 1) ? 0 : (cvsEvents == 2) ? 1 : 2;   // 1 ideal, 2 marginal, 3+ fail
  cvsGHardCut = (cvsHardCuts > 0) ? 2 : 0;                          // any hard current cut (hard over-current or load dump) = fail
  if (cvsFirstEventMs == 0) {
    cvsOutcome = 1;                                                 // rode it out — no protection event
    cvsGRecovery = 3;                                               // n/a
    cvsSoftStimulus = (cvsRpmSlewMax < CVS_SLEW_SOFT_RPMPS);        // weak snap → recommend a harder one
  } else {
    cvsOutcome = 2;
    cvsGRecovery = !cvsRecovered ? 2 : (cvsRecoveryMs <= 12000) ? 0 : (cvsRecoveryMs <= 20000) ? 1 : 2;
    cvsSoftStimulus = false;
  }
  cvsGStab = !cvsSettled ? 2 : cvsLoosePost ? 1 : 0;                // clean stationarity / settled-but-wobbling / never re-settled
  uint8_t worst = cvsGPreStab;                                      // 0 for any run that armed
  uint8_t gs[4] = { cvsGEvents, cvsGHardCut, cvsGRecovery, cvsGStab };
  for (int i = 0; i < 4; i++) if (gs[i] != 3 && gs[i] > worst) worst = gs[i];
  cvsGOverall = worst;
  cvsReady = true; cvsOk = true;
  queueConsoleMessageF("CV stress test: %s — events=%d hardCut=%d recov=%lums overshoot=%.2fV valley=%.2fV rpmSlew=%.0f/s%s",
                       cvsGOverall == 0 ? "PASS" : cvsGOverall == 1 ? "MARGINAL" : "FAIL",
                       (int)cvsEvents, (int)cvsHardCuts, (unsigned long)cvsRecoveryMs,
                       cvsOvershootV, cvsValleyV, cvsRpmSlewMax, cvsSoftStimulus ? " (soft stimulus — snap harder)" : "");
}

// Basic-stability failure: bus swinging beyond CvStressFailBandV at a stabilization deadline. Recorded as a graded
// FAIL (not a silent abort) so the card shows why the test couldn't run.
static void cvStressStabilityFail(const char *where) {
  cvStressForceCV = false;
  cvsOutcome = 3; cvsGPreStab = 2;
  if (cvsFailPhase == 0) cvsFailPhase = cvStressPhase;   // a caller may pre-stamp a specific kind (3 = unreachable)
  cvsGEvents = cvsGHardCut = cvsGRecovery = cvsGStab = 3;
  cvsGOverall = 2;
  cvStressPersistResult();
  cvsReady = true; cvsOk = true; cvsAborted = false;
  cvStressActive = false; cvStressPhase = 4; cvsLastEndMs = millis();
  queueConsoleMessageF("CV stress test: FAIL — %s", where);
}

// ARMED-phase extremes, every AdjustField pass: cvStress_tick skips the hard-OV-cut and lockout passes. Distances
// reference the COMMANDED cvStressTargetV, not the slewed ChargingVoltageTarget, so a moving target can't rescale them.
void cvStressTrackEveryPass() {
  if (!cvStressActive || cvStressPhase != 3) return;
  if (IBV > cvsPeakV) cvsPeakV = IBV;
  if (IBV - cvStressTargetV > cvsOvershootV) cvsOvershootV = IBV - cvStressTargetV;   // peak overshoot (even a no-trip near-miss)
  if (cvsFirstEventMs != 0) {
    if (IBV < cvsMinV) cvsMinV = IBV;
    cvsValleyV = fmaxf(0.0f, cvStressTargetV - cvsMinV);                 // valley depth below target (report)
  }
  if (fieldCollapseTime != 0 && activeCollapseDelay > cvsMaxLockoutMs) cvsMaxLockoutMs = activeCollapseDelay;
}

// Called from AdjustField on normal passes (the immediate-cut and lockout passes return before it).
// Phases 1-2 wait for stationarity; phase 2-3 drive CV at cvStressTargetV via the cvStressForceCV hook.
void cvStress_tick(uint32_t nowMs) {
  if (!cvStressActive) return;
  float    k       = (float)SYSTEM_VOLTAGE_CLASS / 12.0f;
  uint32_t elapsed = nowMs - cvsPhaseStartMs;

  if (cvStressAbortRequested) { cvStressAbortRequested = false; cvStressAbort("cancelled by user"); return; }
  if (sysMode != SYS_MODE_AUTO) { cvStressAbort("left Auto mode"); return; }
  // Signed compare: a poll can land between this pass's nowMs capture and this check, stamping
  // cvsLastPollMs a hair AHEAD of nowMs — unsigned subtraction would wrap to ~4.3e9 and abort.
  if (cvsLastPollMs != 0 && (int32_t)(nowMs - cvsLastPollMs) > (int32_t)CVS_POLL_DEADMAN_MS) {
    cvStressAbort("browser connection lost"); return;
  }
  // Engine-stall guard, suppressed inside the post-cut tach false-zero window (a hard cut collapses
  // the LM2907 pickup to a ~4.6 s false zero, exactly when the ARMED watcher is busiest).
  bool protGrace = (g_lastProtClampMs != 0) && ((uint32_t)(nowMs - g_lastProtClampMs) < PROT_RPM_GRACE_MS + 2000UL);
  if (RPM < 80.0f && !protGrace) { if (cvsRpmLowSinceMs == 0) cvsRpmLowSinceMs = nowMs; } else cvsRpmLowSinceMs = 0;
  if (cvsRpmLowSinceMs != 0 && (uint32_t)(nowMs - cvsRpmLowSinceMs) > 1500UL) { cvStressAbort("engine stopped"); return; }
  // Watch max counted twice: up to CVS_WATCH_MAX_MS waiting for the snap, then a fresh
  // CVS_WATCH_MAX_MS of recovery from a provocation at the last moment.
  if (nowMs - cvsTestStartMs > CVS_IDLESTAB_TO_MS + CVS_CVSTAB_TO_MS + 2UL * CVS_WATCH_MAX_MS + 20000UL) {
    cvStressAbort("timed out"); return;
  }

  cvsStabPush(nowMs, IBV_filtered);
  bool ready  = cvsStabReady(nowMs, CVS_STAB_WIN_MS);
  bool stable = ready && (cvsStabRange(nowMs, CVS_STAB_WIN_MS) <= CVS_STAB_BAND_V * k);

  switch (cvStressPhase) {
    case 1: {  // STAB_IDLE — charge normally ≥10 s, wait for the bus to hold steady AND stop settling up, then fix the target
      if (cvsRingLastMs == nowMs) {   // a ring sample was just accepted — same ~10 Hz cadence
        if (cvsCcN == 0) { cvsCcMinA = cvsCcMaxA = MeasuredAmps; }
        else if (MeasuredAmps < cvsCcMinA) cvsCcMinA = MeasuredAmps;
        else if (MeasuredAmps > cvsCcMaxA) cvsCcMaxA = MeasuredAmps;
        cvsCcSumA += MeasuredAmps; cvsCcN++;
      }
      if (fabsf(IBV_filtered - cvsSettleRefV) > CVS_SETTLEUP_BAND_V * k) { cvsSettleRefV = IBV_filtered; cvsSettleRefMs = nowMs; }
      bool  settledUp = (uint32_t)(nowMs - cvsSettleRefMs) >= CVS_SETTLEUP_WIN_MS || elapsed >= CVS_SETTLEUP_MAX_MS;
      bool  timedOut = elapsed > CVS_IDLESTAB_TO_MS;
      float range    = ready ? cvsStabRange(nowMs, CVS_STAB_WIN_MS) : 1e9f;
      bool  wobblyOk = timedOut && range <= CvStressFailBandV * k;  // tight band missed by the deadline but the swing is workable — proceed and report it
      if (elapsed >= CVS_IDLE_MIN_MS && settledUp && (stable || wobblyOk)) {
        if ((uint32_t)(nowMs - cvsSettleRefMs) < CVS_SETTLEUP_WIN_MS)
          queueConsoleMessage("CV stress test: bus still creeping at the settle-up cap — target may read slightly low");
        cvsWobbleIdleV = range;
        if (!stable) cvsGPreStab = 1;
        if (cvsCcN > 0) { cvsCcAvgA = cvsCcSumA / cvsCcN; cvsCcRipA = cvsCcMaxA - cvsCcMinA; }
        cvsIdleAvgV = cvsStabMean(nowMs, CVS_TARGET_AVG_MS);
        cvStressTargetV = cvsIdleAvgV - CvStressDropV * k;   // achievable: idle already held above it
        cvStressForceCV = true;                                   // AdjustField now parks CV at cvStressTargetV
        if (!stable)
          queueConsoleMessageF("CV stress test: idle voltage still swinging %.2f V (within the %.2f V limit) — proceeding, target %.2f V; stability graded marginal", range, CvStressFailBandV * k, cvStressTargetV);
        else
          queueConsoleMessageF("CV stress test: battery stabilized at %.2f V — target set to %.2f V, entering CV", cvsIdleAvgV, cvStressTargetV);
        cvStressPhase = 2; cvsPhaseStartMs = nowMs;
        cvsStabReset(nowMs);
      } else if (timedOut) {
        cvStressStabilityFail("battery voltage swinging beyond the fail limit at idle");
      }
      break; }
    case 2: {  // STAB_CV — CV forced at the new target; wait for the bus to re-settle onto it, then arm
      // Arm band = a FRACTION of the commanded headroom around cvStressTargetV (not the slewed target), else a band wider
      // than CvStressDropV arms at idle level. Window mean, so ripple inside the stability band can't defeat it.
      float armBand  = fminf(CVS_RESETTLE_BAND_V, 0.5f * CvStressDropV) * k;
      float busAvg   = ready ? cvsStabMean(nowMs, CVS_STAB_WIN_MS) : IBV_filtered;
      bool  atTarget = fabsf(busAvg - cvStressTargetV) <= armBand;
      bool  timedOut = elapsed > CVS_CVSTAB_TO_MS;
      float range    = ready ? cvsStabRange(nowMs, CVS_STAB_WIN_MS) : 1e9f;
      // Deadline tier: the Ki-paced approach can park a hair outside the arm band; within the full re-settle band it arms
      // marginal instead of failing (CV stays forced through ARMED, so the bus keeps closing).
      bool  nearTarget = fabsf(busAvg - cvStressTargetV) <= CVS_RESETTLE_BAND_V * k;
      bool  wobblyOk = timedOut && nearTarget && range <= CvStressFailBandV * k;
      if ((stable && atTarget) || wobblyOk) {
        cvsWobbleCvV = range;
        if (!stable) {
          if (cvsGPreStab < 1) cvsGPreStab = 1;
          queueConsoleMessageF("CV stress test: CV holding target but swinging %.2f V — arming anyway; stability graded marginal", range);
        } else if (!atTarget) {
          if (cvsGPreStab < 1) cvsGPreStab = 1;
          queueConsoleMessageF("CV stress test: bus steady but %.2f V from the %.2f V target at the deadline — arming anyway; stability graded marginal", fabsf(busAvg - cvStressTargetV), cvStressTargetV);
        }
        cvsClamp0 = cvsClampPrev = g_fastOvClampCount;
        cvsClampWas = g_fastOvClampActive; cvsReleaseMs = 0;
        cvsHoc0 = g_hardOCCount; cvsLd0 = g_loadDumpCount; cvsIx0 = g_iExcessCount;
        cvsBattStartA = Bcur;
        cvsArmAltA = MeasuredAmps;
        cvsArmRpm = RPM;
        cvsPeakV = cvsMinV = IBV;
        cvsRpmMax = cvsRpmPrev = RPM; cvsRpmPrevMs = nowMs;
        cvStressPhase = 3; cvsPhaseStartMs = nowMs;
        cvsStabReset(nowMs);
        queueConsoleMessageF("CV stress test: ARMED at %.2f V target, bus settled to %.2f V — snap the throttle to ~half max RPM, then back to idle", cvStressTargetV, busAvg);
      } else if (timedOut) {
        if (!nearTarget && busAvg > cvStressTargetV && MeasuredAmps < 2.0f) {
          // The loop shed everything and the bus still sits above target: the bank is too stiff
          // to pull down by the commanded headroom. Setup condition, not a loop defect.
          cvsFailPhase = 3;
          cvStressStabilityFail("bank too stiff to pull the bus down by the Target Headroom — reduce it and re-run");
        } else {
          cvStressStabilityFail(nearTarget ? "voltage swinging beyond the fail limit in CV mode"
                                           : "voltage would not settle onto the CV target");
        }
      }
      break; }
    case 3: {  // ARMED — CV still forced; the watcher observes. Firmware ends the run itself.
      // Peak / overshoot / valley / max lockout are tracked by cvStressTrackEveryPass().
      if (RPM > cvsRpmMax) cvsRpmMax = RPM;
      if (cvsRpmMax >= cvsArmRpm + CVS_SNAP_RPM_DELTA) cvsSnapDetected = true;
      // Stimulus slew — skipped through the tach false-zero grace so the 0→real re-bias jump after a
      // cut is not read as a huge fake positive slew. The real ramp happens BEFORE the trip.
      if ((uint32_t)(nowMs - cvsRpmPrevMs) >= 100UL) {
        if (!protGrace && RPM > 100.0f && cvsRpmPrev > 100.0f) {
          float slew = (RPM - cvsRpmPrev) / ((nowMs - cvsRpmPrevMs) * 0.001f);
          if (slew > cvsRpmSlewMax) cvsRpmSlewMax = slew;
        }
        cvsRpmPrev = RPM; cvsRpmPrevMs = nowMs;
      }
      // OV events = merged-flag trips (G1/G2 OV, iExcess, LoadDump share one flag). Chatter is the defect, but a re-trip
      // within CVS_EVENT_MERGE_MS of the release is one surge handed between detectors; it still restarts the re-settle clock.
      bool clampOn = g_fastOvClampActive;
      if (cvsClampWas && !clampOn) cvsReleaseMs = nowMs;
      cvsClampWas = clampOn;
      uint32_t clampNow = g_fastOvClampCount;
      if (clampNow != cvsClampPrev) {
        uint32_t gapMs = nowMs - cvsReleaseMs;
        if (cvsEvents > 0 && cvsReleaseMs != 0 && gapMs < CVS_EVENT_MERGE_MS) {
          queueConsoleMessageF("CV stress test: re-trip %lu ms after release counted with the previous event", (unsigned long)gapMs);
        } else if (cvsEvents < 65535) {
          cvsEvents++;
        }
        cvsClampPrev = clampNow;
        if (cvsFirstEventMs == 0) cvsFirstEventMs = nowMs;
        cvsResettleSinceMs = 0;   // a fresh event restarts the re-settle clock
      }
      // Hard current cut = hard over-current (alternator current sensor) OR load dump (INA228 battery current), NOT the OV clamp.
      cvsHardCuts = (uint16_t)((g_hardOCCount - cvsHoc0) + (g_loadDumpCount - cvsLd0));
      cvsIxTrips  = (uint16_t)(g_iExcessCount - cvsIx0);
      // Re-settle detector = provoked (an event fired, or a snap happened) then held in-band + stable
      // for 5 s. Works from BELOW the target (the field-dump valley), which is the real recovery path.
      bool provoked = (cvsFirstEventMs != 0) || cvsSnapDetected;
      if (provoked && cvsProvokedMs == 0) cvsProvokedMs = nowMs;
      bool inBand    = fabsf(IBV_filtered - cvStressTargetV) <= CVS_RESETTLE_BAND_V * k;
      if (provoked && inBand) { if (cvsResettleSinceMs == 0) cvsResettleSinceMs = nowMs; }
      else cvsResettleSinceMs = 0;
      if (provoked && !cvsSettled && cvsResettleSinceMs != 0
          && (uint32_t)(nowMs - cvsResettleSinceMs) >= CVS_SETTLE_MS) {
        // Tight band settles clean at 5 s. A wobble within the fail band gets a second CVS_SETTLE_MS, then settles
        // "loose" (graded marginal); beyond the fail band it waits out WATCH_MAX (graded fail).
        float range = ready ? cvsStabRange(nowMs, CVS_STAB_WIN_MS) : 1e9f;
        bool  loose = ((uint32_t)(nowMs - cvsResettleSinceMs) >= 2UL * CVS_SETTLE_MS)
                      && range <= CvStressFailBandV * k;
        if (stable || loose) {
          cvsWobblePostV = range;
          if (!stable) cvsLoosePost = true;
          cvsSettled = true;
          if (cvsFirstEventMs != 0) { cvsRecovered = true; cvsRecoveryMs = cvsResettleSinceMs - cvsFirstEventMs; }
        }
      }
      if (!cvsSettled && elapsed >= CVS_WATCH_MAX_MS && !provoked) {
        cvStressAbort("no throttle snap detected — re-run and snap to ~half max RPM"); return;
      }
      // Recovery deadline runs from the PROVOCATION, not arming, so a late snap can't starve a healthy recovery.
      if (cvsSettled || (provoked && (uint32_t)(nowMs - cvsProvokedMs) >= CVS_WATCH_MAX_MS)) {   // provoked but never re-settled = graded fail
        cvStressForceCV = false;
        cvStressGrade();
        cvStressPersistResult();
        cvStressActive = false;
        cvStressPhase = 4;
        cvsLastEndMs = nowMs;
      }
      break; }
    default: break;
  }
}

// /cvstress.json builder — also stamps the poll deadman.
int cvStressJsonBuild(char *buf, int cap) {
  cvsLastPollMs = millis();
  const float k = (float)SYSTEM_VOLTAGE_CLASS / 12.0f;   // graded bands are 12V-equiv constants × V/12
  return snprintf(buf, cap,
                  "{\"active\":%d,\"phase\":%d,\"ready\":%d,\"ok\":%d,\"aborted\":%d,\"abort\":\"%s\","
                  "\"targetV\":%.2f,\"idleAvgV\":%.2f,"
                  "\"events\":%d,\"ix\":%d,\"hardCuts\":%d,\"lockMs\":%lu,"
                  "\"overV\":%.2f,\"valleyV\":%.2f,\"peakV\":%.2f,\"minV\":%.2f,\"rpmMax\":%.0f,\"rpmSlew\":%.0f,\"battA\":%.1f,"
                  "\"recovMs\":%lu,\"recovered\":%d,\"settled\":%d,\"softStim\":%d,"
                  "\"outcome\":%d,\"overall\":%d,\"gPreStab\":%d,\"gEvents\":%d,\"gHardCut\":%d,\"gRecov\":%d,\"gStab\":%d,\"failPhase\":%d,"
                  "\"wobIdleV\":%.2f,\"wobCvV\":%.2f,\"wobPostV\":%.2f,"
                  "\"stabBandV\":%.2f,\"failBandV\":%.2f,"
                  "\"armAltA\":%.1f,\"ccAvgA\":%.1f,\"ccRipA\":%.1f,"
                  "\"tune\":{\"gainMode\":%d,\"alpha\":%.3f,\"td\":%.2f,\"kp\":%.2f,\"ki\":%.2f,\"kd\":%.1f,\"recovEn\":%d},"
                  "\"last\":\"%s\"}",
                  cvStressActive ? 1 : 0, (int)cvStressPhase, cvsReady ? 1 : 0, cvsOk ? 1 : 0, cvsAborted ? 1 : 0, cvStressAbortMsg,
                  cvStressTargetV, cvsIdleAvgV,
                  (int)cvsEvents, (int)cvsIxTrips, (int)cvsHardCuts, (unsigned long)cvsMaxLockoutMs,
                  cvsOvershootV, cvsValleyV, cvsPeakV, cvsMinV, cvsRpmMax, cvsRpmSlewMax, cvsBattStartA,
                  (unsigned long)cvsRecoveryMs, cvsRecovered ? 1 : 0, cvsSettled ? 1 : 0, cvsSoftStimulus ? 1 : 0,
                  (int)cvsOutcome, (int)cvsGOverall, (int)cvsGPreStab, (int)cvsGEvents, (int)cvsGHardCut, (int)cvsGRecovery, (int)cvsGStab, (int)cvsFailPhase,
                  cvsWobbleIdleV, cvsWobbleCvV, cvsWobblePostV,
                  CVS_STAB_BAND_V * k, CvStressFailBandV * k,
                  cvsArmAltA, cvsCcAvgA, cvsCcRipA,
                  (int)cvGainMode, cvAlpha, CvKdTd, VoltageKp, VoltageKi, VoltageKd, (int)cvRecovEnable,
                  cvsLastBlob);
}

// ── Capacity tracker (OCV-anchored) — see Xregulator.ino cap globals + dev doc ──
// Rested voltage → SoC% via capOcvVolt[] (12V-referenced, descending): the INDEPENDENT low anchor (no coulomb
// count) that makes fade measurable.
float ocvToSoC(float restedV) {
  float vScale = (float)SYSTEM_VOLTAGE_CLASS / 12.0f;
  if (restedV >= capOcvVolt[0] * vScale) return 100.0f;
  for (int i = 0; i < CAP_OCV_ROWS - 1; i++) {
    float vHi = capOcvVolt[i]     * vScale;   // higher SoC, higher V
    float vLo = capOcvVolt[i + 1] * vScale;   // lower SoC, lower V
    if (restedV <= vHi && restedV >= vLo) {
      float frac = (vHi > vLo) ? (restedV - vLo) / (vHi - vLo) : 0.0f;
      return capOcvSocPct[i + 1] + frac * (capOcvSocPct[i] - capOcvSocPct[i + 1]);
    }
  }
  return 0.0f;
}

static void capAppendPoint(uint32_t epoch, float capAh, float socLow, float tempC, uint8_t conf) {
  if (!bhCapRing) return;
  if (capRefMode == 1 && bhBaselineCapacityAh <= 0.0f) bhBaselineCapacityAh = capAh;  // first = 100%
  float refAh = (capRefMode == 1) ? bhBaselineCapacityAh : (float)BatteryCapacity_Ah;
  BattCapPoint p = {};
  p.epoch = epoch; p.capacityAh = capAh;
  p.capPct = (refAh > 0.0f) ? (capAh / refAh * 100.0f) : 100.0f;
  p.socLow = socLow; p.tempC = tempC; p.conf = conf;
  bhCapRing[bhCapHead] = p;
  bhCapHead = (bhCapHead + 1) % bhCapCap;
  if (bhCapCount < bhCapCap) bhCapCount++;
  capLastPct = p.capPct;
  capLastUpdateEpoch = epoch;
  bhCapDirty = true;   // field-off persist
  queueConsoleMessageF("BATT CAP: %.1f Ah = %.0f%% (from rested SoC %.0f%%, %s)",
                       capAh, p.capPct, socLow, conf ? "high conf" : "LOW conf");
}

// Per battery-update tick. Rest detection (dV/dt-settled + time floor) on the FILTERED voltage,
// low-OCV anchor capture, and an independent unclamped Ah bridge from the anchor.
void capTrackTick(float I_batt, float V_filt, float tempC, float dtSec) {
  if (!bhCapRing || dtSec <= 0.0f || demoSeal) return;   // demo mode: the simulated battery never feeds the capacity record
  uint32_t nowMs = millis();
  float restThresh = capRestCurrentFrac * (float)BatteryCapacity_Ah;
  if (restThresh < 0.2f) restThresh = 0.2f;

  // dV/dt of the FILTERED voltage over a ≥2-min window → mV per 10 min (short windows would be all noise)
  // NAN V_filt = no trustworthy voltage: no rest timing and no anchor, the Ah bridge below still runs.
  const bool vOk = isfinite(V_filt);
  if (!vOk) capPrevVForRate = NAN;
  else if (isnan(capPrevVForRate)) { capPrevVForRate = V_filt; capPrevVRateMs = nowMs; }
  else {
    float dtMin = (nowMs - capPrevVRateMs) / 60000.0f;
    if (dtMin >= 2.0f) {
      capLastDvdtMv10 = fabsf(V_filt - capPrevVForRate) * 1000.0f / dtMin * 10.0f;
      capPrevVForRate = V_filt; capPrevVRateMs = nowMs;
    }
  }

  if (vOk && fabsf(I_batt) < restThresh) capRestTimerMs += (uint32_t)(dtSec * 1000.0f);
  else                                   capRestTimerMs = 0;

  bool rested = (capRestTimerMs >= (uint32_t)capRestFloorMin * 60000UL) &&
                (capLastDvdtMv10 < capSettleRateMv10);
  if (rested) {
    float soc = ocvToSoC(V_filt);
    if (soc <= capSocLowMax && (!capLowAnchorValid || soc < capLowAnchorSoC)) {  // bottom knee, keep the lowest
      capLowAnchorValid = true;
      capLowAnchorSoC   = soc;
      capLowAnchorTempC = tempC;
      capBridgeAh = 0.0f; capBridgeMinAh = 0.0f;
    }
  }

  if (capLowAnchorValid) {
    float dAh = I_batt * dtSec / 3600.0f;
    if (dAh > 0.0f) dAh *= (ChargeEfficiency_scaled / 1000.0f);   // single source of truth = Battery Monitor's ChargeEfficiency_scaled (%×10)
    capBridgeAh += dAh;
    if (capBridgeAh < capBridgeMinAh) capBridgeMinAh = capBridgeAh;
    if (capBridgeMinAh < -0.05f * (float)BatteryCapacity_Ah) capLowAnchorValid = false;  // re-discharged → stale
  }
}

// At the full-charge anchor. Emits a dated capacity point if the span is deep enough.
void capTrackOnFull(uint32_t epoch, float tempC) {
  if (!capLowAnchorValid || demoSeal) return;                 // no independent low anchor → can't measure
  float span = capFullSoc - capLowAnchorSoC;
  if (span < capMinSpan || capBridgeAh <= 0.0f) { capLowAnchorValid = false; return; }

  float measuredAh = capBridgeAh / (span / 100.0f);   // no coulomb-derived SoC → actually shows fade
  if (capTempNormEnable && !isnan(tempC) && !isnan(capLowAnchorTempC)) {
    float tAvg = 0.5f * (tempC + capLowAnchorTempC);
    measuredAh *= (1.0f + (capTempCoeffPctC / 100.0f) * (capTempRefC - tAvg));
  }
  if (measuredAh < 0.3f * BatteryCapacity_Ah || measuredAh > 1.5f * BatteryCapacity_Ah) {
    capLowAnchorValid = false; return;            // implausible vs rated → discard
  }
  uint8_t conf = (capLastDvdtMv10 < 0.5f * capSettleRateMv10 && span >= capMinSpan + 10.0f) ? 1 : 0;
  capAppendPoint(epoch, measuredAh, capLowAnchorSoC, tempC, conf);   // NAN stays NAN: 0 °C is a real temperature
  capLowAnchorValid = false;                       // consume the anchor
}

// Persist DCIR results + capacity ring to LittleFS blobs. Caller gates on field-off.
// Dirty flags stay set on a failed write so a later flush retries instead of losing history;
// the caller runs on the 2 s SOC tick, so failures back off 10 min (one warning per retry).
void bhFlushCapNVS() {
  if (dbgRingsSynthetic) return;   // fillmax/clearmax: RAM ring is synthetic/empty — keep the real flash blob
  static uint32_t bhFlushRetryAtMs = 0;
  if (bhFlushRetryAtMs != 0 && (int32_t)(millis() - bhFlushRetryAtMs) < 0) return;
  bool failed = false;
  if (bhCapDirty) {
    uint32_t start = (bhCapCount < bhCapCap) ? 0 : (uint32_t)bhCapHead;
    uint32_t n = writePsramBlob(BHCAP_PATH, BHCAP_MAGIC, BHCAP_VER, 0,
                                bhCapRing, sizeof(BattCapPoint), bhCapCap, start, bhCapCount);
    settingWrite(NK_bhBaseline, String(bhBaselineCapacityAh, 3).c_str());
    if (n == (uint32_t)bhCapCount) {
      bhCapDirty = false;
      // Legacy NVS string is only dropped once its content is safely in the blob (migration in bhInitSettings)
      if (settingExists(NK_bhCapBlob)) settingRemove(NK_bhCapBlob);
    } else failed = true;
  }
  if (bhResultsDirty) {
    uint32_t start = (bhResultCount < bhResultCap) ? 0 : (uint32_t)bhResultHead;
    uint32_t n = writePsramBlob(BHRES_PATH, BHRES_MAGIC, BHRES_VER, 0,
                                bhResults, sizeof(BattHealthResult), bhResultCap, start, bhResultCount);
    if (n == (uint32_t)bhResultCount) {
      bhResultsDirty = false;
      if (settingExists(NK_bhResults)) settingRemove(NK_bhResults);
    } else failed = true;
  }
  if (failed) {
    bhFlushRetryAtMs = millis() + 600000UL;
    queueConsoleMessage("Battery health: history save failed (filesystem?) - retrying every 10 min");
  } else {
    bhFlushRetryAtMs = 0;
  }
}

// ── Deserialize (legacy NVS-string migration only; records joined by ';') ────────
static float bhTok(const String &s, int &pos) {   // next comma/semicolon-delimited float
  int e = pos;
  while (e < (int)s.length() && s[e] != ',' && s[e] != ';') e++;
  String t = s.substring(pos, e);
  pos = e + 1;
  if (t == "nan") return NAN;
  return t.toFloat();
}
// Integer fields must not round-trip through bhTok's float (24-bit mantissa): current epochs
// (~1.75e9) land on a 128 s float grid, shifting every timestamp up to ~64 s per reboot.
static uint32_t bhTokU32(const String &s, int &pos) {
  int e = pos;
  while (e < (int)s.length() && s[e] != ',' && s[e] != ';') e++;
  String t = s.substring(pos, e);
  pos = e + 1;
  return (uint32_t)strtoul(t.c_str(), nullptr, 10);
}

void bhDeserializeResults(const String &blob) {
  bhResultCount = 0; bhResultHead = 0;
  if (blob.length() == 0) return;
  int pos = 0;
  while (pos < (int)blob.length()) {
    BattHealthResult r = {};
    r.epoch      = bhTokU32(blob, pos);
    r.dcir_mOhm  = bhTok(blob, pos);
    r.soh_pct    = bhTok(blob, pos);
    bhTok(blob, pos); r.battTempF = NAN;   // legacy slot held the BOARD temperature — consumed, not carried forward
    r.soc_pct    = bhTok(blob, pos);
    r.battV      = bhTok(blob, pos);
    r.stepLowA   = bhTok(blob, pos);
    r.stepDeltaA = bhTok(blob, pos);
    r.edgesUsed  = (uint8_t)bhTok(blob, pos);
    r.dwellMsUsed    = (uint16_t)bhTokU32(blob, pos);   // 0 for pre-upgrade rows
    r.fitSpread_mOhm = bhTok(blob, pos);
    BH_APPEND_RESULT(r);
  }
}

void bhDeserializeCap(const String &blob) {
  bhCapCount = 0; bhCapHead = 0;
  if (blob.length() == 0) return;
  int pos = 0;
  while (pos < (int)blob.length()) {
    BattCapPoint p = {};
    p.epoch      = bhTokU32(blob, pos);
    p.capacityAh = bhTok(blob, pos);
    p.capPct     = bhTok(blob, pos);
    p.socLow     = bhTok(blob, pos);
    p.tempC      = bhTok(blob, pos);
    p.conf       = (uint8_t)bhTok(blob, pos);
    bhCapRing[bhCapHead] = p;
    bhCapHead = (bhCapHead + 1) % bhCapCap;
    if (bhCapCount < bhCapCap) bhCapCount++;
  }
}

// OCV table blob — CAP_OCV_ROWS rested-voltage values, comma-joined.
String capSerializeOcv() {
  String out;
  for (int i = 0; i < CAP_OCV_ROWS; i++) { if (i) out += ','; out += String(capOcvVolt[i], 3); }
  return out;
}
void capDeserializeOcv(const String &blob) {
  if (blob.length() == 0) return;
  int pos = 0;
  for (int i = 0; i < CAP_OCV_ROWS && pos < (int)blob.length(); i++) capOcvVolt[i] = bhTok(blob, pos);
}

static bool ocvMatchesPreset(const float *p) {
  for (int i = 0; i < CAP_OCV_ROWS; i++) if (fabsf(capOcvVolt[i] - p[i]) > 0.005f) return false;
  return true;
}
// True while capOcvVolt[] still equals one of the chemistry presets — i.e. the user has NOT hand-tuned it.
bool ocvIsAnyPreset() {
  return ocvMatchesPreset(capOcvPresetLifepo4) || ocvMatchesPreset(capOcvPresetAgm) || ocvMatchesPreset(capOcvPresetLead);
}
// Bind the rested-voltage curve to the selected chemistry unless the user hand-tuned it. Runs at
// commissioning (vessel-info save) and at boot, before the SoC seed reads the curve, so voltage→SoC
// matches the bank. Idempotent: no NVS write when the curve is already the right preset.
void applyChemistryOcvPreset() {
  if (!ocvIsAnyPreset()) return;  // custom curve — never clobber
  String bt = BATTERY_TYPE; bt.toLowerCase();
  const float *p = nullptr;
  if (bt.indexOf("lifepo") >= 0 || bt.indexOf("lithium") >= 0 || bt.indexOf("li-ion") >= 0 ||
      bt.indexOf("liion") >= 0 || bt.indexOf("lfp") >= 0)  p = capOcvPresetLifepo4;
  else if (bt.indexOf("agm") >= 0)                         p = capOcvPresetAgm;
  else if (bt.indexOf("lead") >= 0)                        p = capOcvPresetLead;
  if (!p || ocvMatchesPreset(p)) return;  // "other"/unknown chemistry, or already the right curve
  for (int i = 0; i < CAP_OCV_ROWS; i++) capOcvVolt[i] = p[i];
  settingWrite(NK_capOcvBlob, capSerializeOcv().c_str());
}

// Call after InitSystemSettings() — needs the NVS settings layer up.
void bhInitSettings() {
  if (!settingExists(NK_bhStepLowA))   settingWrite(NK_bhStepLowA, String(bhStepLowA, 1).c_str());   else bhStepLowA   = settingRead(NK_bhStepLowA).toFloat();
  if (!settingExists(NK_bhStepDeltaA)) settingWrite(NK_bhStepDeltaA, String(bhStepDeltaA, 1).c_str()); else bhStepDeltaA = settingRead(NK_bhStepDeltaA).toFloat();
  if (!settingExists(NK_bhDwellMs))    settingWrite(NK_bhDwellMs, String(bhDwellMs).c_str());         else bhDwellMs    = (uint32_t)settingRead(NK_bhDwellMs).toInt();
  if (!settingExists(NK_bhNumEdges))   settingWrite(NK_bhNumEdges, String(bhNumEdges).c_str());       else bhNumEdges   = (uint8_t)settingRead(NK_bhNumEdges).toInt();
  if (settingExists(NK_bhBaseline))    bhBaselineCapacityAh = settingRead(NK_bhBaseline).toFloat();
  if (bhNumEdges < 3) bhNumEdges = 3;
  if (bhNumEdges > BH_MAX_TOGGLES - 3) bhNumEdges = BH_MAX_TOGGLES - 3;
  if (bhStepDeltaA < 5.0f) bhStepDeltaA = 5.0f;   // ΔV must clear ripple/noise; also guards the ΔI validity gate
  if (bhDwellMs < bhMinDwellMs()) bhDwellMs = bhMinDwellMs();
  // Rings restore from LittleFS blobs; a legacy NVS string migrates once (dirty set). The legacy key is removed only
  // after the blob write succeeds (bhFlushCapNVS), so a reboot before the first flush can't lose history.
  {
    // v1 rows hold the BOARD temperature in battTempF. Peek the version first (readPsramBlob deletes a version
    // mismatch), then blank that field; the next field-off flush rewrites the ring as v2.
    bool bhResLegacy = (psramBlobVersion(BHRES_PATH, BHRES_MAGIC) == BHRES_VER_BOARDTEMP);
    uint32_t nRes = readPsramBlob(BHRES_PATH, BHRES_MAGIC,
                                  bhResLegacy ? BHRES_VER_BOARDTEMP : BHRES_VER,
                                  bhResults, sizeof(BattHealthResult), bhResultCap, nullptr, false);
    if (bhResLegacy && nRes > 0) {
      for (uint32_t i = 0; i < nRes; i++) bhResults[i].battTempF = NAN;
      bhResultsDirty = true;
      Serial.printf("bhInit: migrated %u DCIR rows to v2 (board-temperature column blanked)\n", (unsigned)nRes);
    }
    bhResultCount = (int)nRes;
    bhResultHead  = (nRes >= (uint32_t)bhResultCap) ? 0 : (int)nRes;
    if (nRes == 0 && settingExists(NK_bhResults)) {
      bhDeserializeResults(settingRead(NK_bhResults));
      bhResultsDirty = true;
    }
    uint32_t nCap = readPsramBlob(BHCAP_PATH, BHCAP_MAGIC, BHCAP_VER,
                                  bhCapRing, sizeof(BattCapPoint), bhCapCap, nullptr, false);
    bhCapCount = (int)nCap;
    bhCapHead  = (nCap >= (uint32_t)bhCapCap) ? 0 : (int)nCap;
    if (nCap == 0 && settingExists(NK_bhCapBlob)) {
      bhDeserializeCap(settingRead(NK_bhCapBlob));
      bhCapDirty = true;
    }
  }

  // Capacity tracker config (Pattern B) + the editable OCV table
  if (!settingExists(NK_capRestFrac))   settingWrite(NK_capRestFrac, String(capRestCurrentFrac, 4).c_str());   else capRestCurrentFrac = settingRead(NK_capRestFrac).toFloat();
  if (!settingExists(NK_capRestFloor))  settingWrite(NK_capRestFloor, String(capRestFloorMin).c_str());        else capRestFloorMin   = (uint16_t)settingRead(NK_capRestFloor).toInt();
  // capSettleRate is volt-domain (mV/10min): first creation scales the 12V default ×(V/12), same rule as InitSystemSettings seeds
  if (!settingExists(NK_capSettleRate)) { capSettleRateMv10 *= (float)SYSTEM_VOLTAGE_CLASS / 12.0f; settingWrite(NK_capSettleRate, String(capSettleRateMv10, 2).c_str()); }  else capSettleRateMv10 = settingRead(NK_capSettleRate).toFloat();
  if (!settingExists(NK_capSocLowMax))  settingWrite(NK_capSocLowMax, String(capSocLowMax, 1).c_str());        else capSocLowMax      = settingRead(NK_capSocLowMax).toFloat();
  if (!settingExists(NK_capMinSpan))    settingWrite(NK_capMinSpan, String(capMinSpan, 1).c_str());            else capMinSpan        = settingRead(NK_capMinSpan).toFloat();
  if (!settingExists(NK_capFullSoc))    settingWrite(NK_capFullSoc, String(capFullSoc, 1).c_str());            else capFullSoc        = settingRead(NK_capFullSoc).toFloat();
  if (!settingExists(NK_capRefMode))    settingWrite(NK_capRefMode, String(capRefMode).c_str());               else capRefMode        = (uint8_t)settingRead(NK_capRefMode).toInt();
  if (!settingExists(NK_capTempNorm))   settingWrite(NK_capTempNorm, String(capTempNormEnable).c_str());       else capTempNormEnable = (uint8_t)settingRead(NK_capTempNorm).toInt();
  if (!settingExists(NK_capTempCoeff))  settingWrite(NK_capTempCoeff, String(capTempCoeffPctC, 3).c_str());    else capTempCoeffPctC  = settingRead(NK_capTempCoeff).toFloat();
  if (!settingExists(NK_capTempRef))    settingWrite(NK_capTempRef, String(capTempRefC, 1).c_str());           else capTempRefC       = settingRead(NK_capTempRef).toFloat();
  if (settingExists(NK_capOcvBlob))     capDeserializeOcv(settingRead(NK_capOcvBlob));
}

// Bounded append for bhBuildStatusJson: n parks at cap-1 once the buffer is full, which the caller treats as overflow.
static void bhCat(char *out, size_t cap, size_t &n, const char *fmt, ...) {
  if (n >= cap - 1) return;
  va_list ap;
  va_start(ap, fmt);
  int w = vsnprintf(out + n, cap - n, fmt, ap);
  va_end(ap);
  if (w < 0) return;
  n = ((size_t)w >= cap - n) ? cap - 1 : n + (size_t)w;
}

// GET /batteryHealth body, formatted once into a PSRAM buffer the caller streams (sendPsramText) and frees. Polled at
// 2 Hz during a test with the field live: the Arduino String this replaced regrew under the PSRAM heap lock on every
// append, up to ~50 KB with both rings full. Returns nullptr, nothing allocated, when the buffer could not be had or the
// body did not fit its per-entry budget (never a truncated body).
char *bhBuildStatusJson(size_t *len) {
  *len = 0;
  const int rc = bhResultCount, cc = bhCapCount;   // taken once: Core 1 appends a result while a test runs
  const size_t cap = 1024 + (size_t)CAP_OCV_ROWS * 24 + (size_t)(rc + 1) * 200 + (size_t)(cc + 1) * 110;
  char *j = (char *)ps_malloc(cap);
  if (!j) return nullptr;
  size_t n = 0;
  char numA[16], numB[16];
  bhCat(j, cap, n, "{\"state\":%u,\"reason\":\"%s\",\"lastDcir\":%.2f,\"baselineAh\":%.1f,\"low\":%.1f,\"delta\":%.1f,\"dwellMs\":%lu,\"edges\":%u",
        (unsigned)bhTestState, bhAbortReason, bhLastResultDcir, bhBaselineCapacityAh, bhStepLowA, bhStepDeltaA,
        (unsigned long)bhDwellMs, (unsigned)bhNumEdges);
  // Wizard countdown: (2 ring-in + scored + 1 trailing) toggles × dwell, mirroring bhStep's bound. elapsedMs is live
  // only while running, so a reopened modal can recompute what remains.
  bhCat(j, cap, n, ",\"totalMs\":%lu,\"elapsedMs\":%lu,\"results\":[",
        (unsigned long)((uint32_t)(bhNumEdges + 3) * bhDwellMs),
        (unsigned long)(bhTestState == 1 ? (millis() - bhTestStartMs) : 0UL));
  int start = (rc < bhResultCap) ? 0 : bhResultHead;
  for (int k = 0; k < rc; k++) {
    BattHealthResult &r = bhResults[(start + k) % bhResultCap];
    bhCat(j, cap, n, "%s{\"epoch\":%lu,\"dcir\":%.2f,\"soh\":%s,\"tF\":%s,\"soc\":%.1f,\"v\":%.2f,\"low\":%.1f,\"delta\":%.1f,\"edges\":%d,\"dwell\":%u,\"spread\":%.3f}",
          k ? "," : "", (unsigned long)r.epoch, r.dcir_mOhm,
          ltJsonNumP(numA, sizeof(numA), r.soh_pct, !isnan(r.soh_pct), 1),
          ltJsonNumP(numB, sizeof(numB), r.battTempF, !isnan(r.battTempF), 1),
          r.soc_pct, r.battV, r.stepLowA, r.stepDeltaA, (int)r.edgesUsed, (unsigned)r.dwellMsUsed, r.fitSpread_mOhm);
  }
  // Capacity: live status + config echoes + editable OCV table + the dated points. capRestMin = current rest accumulation
  // (min); chgEff and capacityAh are read-only mirrors of the Battery Monitor tab (capacityAh drives % vs rated).
  bhCat(j, cap, n, "],\"capLastPct\":%s,\"capLastUpd\":%lu,\"capAnchored\":%d,\"capAnchorSoC\":%.1f,\"capRestMin\":%lu,\"capDvdt\":%.1f",
        ltJsonNumP(numA, sizeof(numA), capLastPct, !isnan(capLastPct), 1), (unsigned long)capLastUpdateEpoch,
        capLowAnchorValid ? 1 : 0, capLowAnchorSoC, (unsigned long)(capRestTimerMs / 60000), capLastDvdtMv10);
  bhCat(j, cap, n, ",\"capCfg\":{\"restFrac\":%.4f,\"restFloor\":%u,\"settleRate\":%.2f,\"socLowMax\":%.1f,\"minSpan\":%.1f,\"chgEff\":%.4f,\"capacityAh\":%d,\"fullSoc\":%.1f,\"refMode\":%u,\"tempNorm\":%u,\"tempCoeff\":%.3f,\"tempRef\":%.1f}",
        capRestCurrentFrac, (unsigned)capRestFloorMin, capSettleRateMv10, capSocLowMax, capMinSpan,
        (double)(ChargeEfficiency_scaled / 1000.0f), BatteryCapacity_Ah, capFullSoc, (unsigned)capRefMode,
        (unsigned)capTempNormEnable, capTempCoeffPctC, capTempRefC);
  bhCat(j, cap, n, ",\"ocv\":[");
  for (int i = 0; i < CAP_OCV_ROWS; i++) bhCat(j, cap, n, "%s%.2f", i ? "," : "", capOcvVolt[i]);
  bhCat(j, cap, n, "],\"ocvSoc\":[");
  for (int i = 0; i < CAP_OCV_ROWS; i++) bhCat(j, cap, n, "%s%d", i ? "," : "", (int)capOcvSocPct[i]);
  bhCat(j, cap, n, "],\"cap\":[");
  int cstart = (cc < bhCapCap) ? 0 : bhCapHead;
  for (int k = 0; k < cc; k++) {
    BattCapPoint &p = bhCapRing[(cstart + k) % bhCapCap];
    bhCat(j, cap, n, "%s{\"epoch\":%lu,\"ah\":%.1f,\"pct\":%.1f,\"socLow\":%.1f,\"conf\":%d}",
          k ? "," : "", (unsigned long)p.epoch, p.capacityAh, p.capPct, p.socLow, (int)p.conf);
  }
  bhCat(j, cap, n, "]}");
  if (n >= cap - 1) {
    free(j);
    return nullptr;
  }
  *len = n;
  return j;
}

// /debug/fillmax + /debug/clearmax (bench only): fill every fixed-cap ring to cap with DISPERSED points and time the
// O(count) worst cases. Zero-drift span < ZFIT_MIN_SPAN_F on purpose (no early exit, fit never accepted). Default is
// RAM-only (dbgRingsSynthetic freezes the savers until reboot); &persist=yes writes synthetic data that survives reflash.
static uint32_t dbgLcg = 0x1234567u;               // deterministic LCG → identical fill every run
static inline float dbgRand(float lo, float hi) {
  dbgLcg = dbgLcg * 1664525u + 1013904223u;
  return lo + (hi - lo) * ((float)(dbgLcg >> 8) / 16777216.0f);
}

void debugFillMax(AsyncWebServerRequest *request) {
  if (!request->hasParam("confirm") || request->getParam("confirm")->value() != "yes") {
    request->send(200, "text/plain",
      "REFUSED. /debug/fillmax fills EVERY ring to cap with synthetic data and\n"
      "OVERWRITES the learned alt-health + boat-perf surfaces in RAM (not flash — a\n"
      "reboot restores them; persistence is frozen until then). Bench units only.\n"
      "Re-send as /debug/fillmax?confirm=yes to proceed.\n"
      "Add &persist=yes to ALSO run+time the real field-off savers (writes the synthetic\n"
      "data to LittleFS/NVS; survives reboot AND reflash — recover with Erase All Flash).\n");
    return;
  }
  bool persist = request->hasParam("persist") && request->getParam("persist")->value() == "yes";
  dbgRingsSynthetic = !persist;                     // default: freeze ALL field-off persistence until reboot (flash stays real)
  dbgLcg = 0x1234567u;                              // reseed
  String out;
  frontWriteBegin();

  // 4-D alternator best-ever front → cap, dispersed across the real operating box (axisScale set at init)
  if (altFrontBuf) {
    for (int i = 0; i < ALT_FRONT_CAP; i++) {
      FrontPoint<ALT_NAXIS> &p = altFrontBuf[i];
      p.x[0] = dbgRand(800.0f, 6000.0f);            // RPM
      p.x[1] = dbgRand(0.0f, 3.0f);                 // excitation
      p.x[2] = dbgRand(12.0f, 15.0f);               // Vbus
      p.x[3] = dbgRand(40.0f, 250.0f);              // tempF
      p.ex[0] = 0; p.ex[1] = 0; p.y = dbgRand(20.0f, 150.0f); p.nSamp = 120; p.tEmit = (uint32_t)i;
    }
    altFront2.count = ALT_FRONT_CAP; altFront2.source = 0;
  }

  // 3-D boat-perf fronts (sail + motor) → cap (blob + scan cost; not timed here)
  if (sailFrontBuf) {
    for (int i = 0; i < PERF_FRONT_CAP; i++) {
      FrontPoint<PERF_NAXIS> &p = sailFrontBuf[i];
      for (int a = 0; a < PERF_NAXIS; a++) p.x[a] = dbgRand(0.0f, 60.0f);
      p.ex[0] = 0; p.ex[1] = 0; p.y = dbgRand(2.0f, 12.0f); p.nSamp = 60; p.tEmit = (uint32_t)i;
    }
    sailFront.count = PERF_FRONT_CAP; sailFront.source = 0;
  }
  if (motorFrontBuf) {
    for (int i = 0; i < PERF_FRONT_CAP; i++) {
      FrontPoint<PERF_NAXIS> &p = motorFrontBuf[i];
      for (int a = 0; a < PERF_NAXIS; a++) p.x[a] = dbgRand(0.0f, 60.0f);
      p.ex[0] = 0; p.ex[1] = 0; p.y = dbgRand(2.0f, 12.0f); p.nSamp = 60; p.tEmit = (uint32_t)i;
    }
    motorFront.count = PERF_FRONT_CAP; motorFront.source = 0;
  }
  frontWriteEnd();

  // Alt-health engine-hour trend → cap (drives the /alttrend.csv scan + ~52 KB /alttrend.bin)
  if (altTrend) {
    for (int i = 0; i < ALT_TREND_CAP; i++) {
      altTrend[i].engHour    = (uint16_t)i;
      altTrend[i].worstPct   = (int16_t)lroundf(dbgRand(600.0f, 950.0f));    // %×10
      altTrend[i].overallPct = (int16_t)lroundf(dbgRand(800.0f, 1000.0f));
    }
    altTrendCount = ALT_TREND_CAP; altTrendFlushed = 0; altTrendRewrite = true;  // next field-off save rewrites whole log
  }

  // 30-day long-term ring → cap
  if (longTermRing) {
    uint32_t base = (uint32_t)time(NULL); if (base < 1700000000u) base = 1700000000u;
    for (int i = 0; i < LONGTERM_RING_SIZE; i++) {
      LongTermRecord &r = longTermRing[i];
      r.timestamp = base - (uint32_t)(LONGTERM_RING_SIZE - i) * 600u;         // 10-min cadence
      r.validMask = 0xFFFFFFFFu; r.chargeStage = 1;
    }
    longTermCount = LONGTERM_RING_SIZE; longTermHead = 0;
    // Without this, dumpLongTermRing early-outs (delta == 0 && FileRecords > 0) and &persist=yes silently
    // writes nothing — the timing report then shows ~0 ms for a saver that never ran.
    longTermPushSeq = longTermCount; longTermFlushedSeq = 0; longTermFileRecords = 0;
  }

  // Zero-drift log → cap, temp span < ZFIT_MIN_SPAN_F so zeroFitRegress can't early-exit (true worst case)
  if (zeroLogRing) {
    for (int i = 0; i < ZEROLOG_RING_SIZE; i++) {
      ZeroLogRecord &r = zeroLogRing[i];
      r.epoch = (uint32_t)i; r.amps = dbgRand(-0.5f, 0.5f); r.p2pAmps = 0.1f; r.rpm = 0; r.battVx100 = 1280;
      r.altTempFx10   = (int16_t)lroundf(dbgRand(700.0f, 720.0f));            // ~2 °F span → never satisfies the 30 °F gate
      r.boardTempFx10 = (int16_t)lroundf(dbgRand(700.0f, 720.0f));
    }
    zeroLogCount = ZEROLOG_RING_SIZE; zeroLogHead = 0;
    zeroLogPushSeq = zeroLogCount; zeroLogFlushedSeq = 0; zeroLogFileRecords = 0;  // same early-out as above
  }

  // Battery-capacity ring → cap. The sensor upload ring is NOT faked: it would queue junk rows to the cloud and feeds
  // only the Core-0 uploader, no O(count) control-path scan.
  if (bhCapRing) {
    for (int i = 0; i < bhCapCap; i++) {
      BattCapPoint &p = bhCapRing[i];
      p.epoch = 1700000000u + (uint32_t)i * 86400u; p.capacityAh = dbgRand(180.0f, 210.0f);
      p.capPct = dbgRand(85.0f, 100.0f); p.socLow = dbgRand(8.0f, 15.0f); p.tempC = 25.0f; p.conf = 1;
    }
    bhCapCount = bhCapCap; bhCapHead = 0; bhCapDirty = true;   // dirty so a persist-mode bhFlushCapNVS actually writes
  }

  // Measure the two worst-case data-scaling scans on THIS board (not estimates):
  // 1) front classify() at cap = the real 1 Hz alt-health cost (IDW eval + LWLR select + 48-pt solve)
  float pred = 0; volatile long sink = 0;
  float surf[ALT_NAXIS] = { 3000.0f, 1.5f, 13.5f, 150.0f };
  uint32_t t0 = micros();
  for (int r = 0; r < 20; r++) sink += altFront2.classify(surf, altRefRadius, altIdwPower, altRidgeFrac, altRiskThresh, &pred);
  uint32_t classifyUs = (micros() - t0) / 20;
  // 2) trend drop-oldest memmove at cap = the "once per decade" event on real PSRAM
  // altTrend can be NULL (initAlternatorHealth returns early on ps_malloc failure) — same guard as the fill above
  uint32_t memmoveUs = 0;
  if (altTrend) {
    uint32_t t1 = micros();
    memmove(altTrend, altTrend + 1, (size_t)(ALT_TREND_CAP - 1) * sizeof(AltTrendPt));
    memmoveUs = micros() - t1;
    altTrendCount = ALT_TREND_CAP;                  // memmove left a dup in the last slot; keep count at cap
  }

  out  = "FILLMAX done — every ring at cap (bench worst-case)\n";
  out += "altFront2="   + String(altFront2.count) + "/" + String(ALT_FRONT_CAP);
  out += "  sail/motor=" + String(sailFront.count) + "/" + String(motorFront.count) + "\n";
  out += "altTrend="    + String(altTrendCount) + "/" + String(ALT_TREND_CAP);
  out += "  longTerm="  + String((unsigned)longTermCount) + "/" + String((unsigned)LONGTERM_RING_SIZE) + "\n";
  out += "zeroLog="     + String((unsigned)zeroLogCount) + "/" + String((unsigned)ZEROLOG_RING_SIZE);
  out += "  bhCap="     + String(bhCapCount) + "/" + String(bhCapCap) + "\n";
  out += "---- measured ceilings ----\n";
  out += "classify() @ " + String(ALT_FRONT_CAP) + " pts (1 Hz alt-health tick): " + String(classifyUs) + " us\n";
  out += "trend memmove @ cap (once-per-decade drop-oldest): " + String(memmoveUs) + " us\n";
  out += "free heap=" + String(ESP.getFreeHeap()) + " maxBlock=" + String(ESP.getMaxAllocHeap());
  out += " freePSRAM=" + String(ESP.getFreePsram()) + "\n";
  out += "sink=" + String(sink) + "\n\n";
  if (persist) {
    struct { const char *name; void (*fn)(); } savers[] = {
      { "altHealthSave (fronts + trend)", altHealthSave },
      { "boatPerfSave (sail + motor fronts)", boatPerfSave },
      { "dumpLongTermRing", dumpLongTermRing },
      { "dumpZeroLog", dumpZeroLog },
      { "bhFlushCapNVS (capacity blob, NVS)", bhFlushCapNVS },
    };
    out += "---- persist=yes: real field-off savers run inline, timed ----\n";
    for (auto &s : savers) {
      uint32_t w0 = millis();
      s.fn();
      out += String(s.name) + ": " + String(millis() - w0) + " ms\n";
    }
    out += "Flash/NVS now hold SYNTHETIC data (survives reboot AND reflash).\n";
    out += "Reboot to test the load-at-cap boot path; recover with Erase All Flash / factory reset.\n";
  } else {
    out += "Persistence is FROZEN until reboot (flash untouched) — reboot restores real data.\n";
  }
  out += "LittleFS used/total=" + String((unsigned)LittleFS.usedBytes()) + "/" + String((unsigned)LittleFS.totalBytes()) + "\n";
  out += "Next: watch ft_altFold / ft_altHealth / ft_dumpLongTermRing in /debug for worst-pass loop time.\n";
  request->send(200, "text/plain", out);
}

// Zero the in-RAM rings after a fillmax. Persistence stays FROZEN — empty is as un-real as synthetic;
// reboot restores the real data. sensorRing untouched (never faked; zeroing it would drop real pending uploads).
void debugClearMax(AsyncWebServerRequest *request) {
  if (!request->hasParam("confirm") || request->getParam("confirm")->value() != "yes") {
    request->send(200, "text/plain",
      "REFUSED. /debug/clearmax zeroes the in-RAM learned rings (alt-health + boat-perf fronts, trend,\n"
      "long-term, zero-drift and capacity logs) and freezes every field-off saver until reboot.\n"
      "Bench units only. Re-send as /debug/clearmax?confirm=yes to proceed.\n");
    return;
  }
  dbgRingsSynthetic = true;                         // RAM empty ≠ real → keep every field-off persister frozen until reboot
  frontWriteBegin();
  if (altFront2.source == 0) altFront2.count = 0;   // only clear a LEARNED surface, never an uploaded one
  sailFront.count = 0; motorFront.count = 0;
  frontWriteEnd();
  altTrendCount = 0; altTrendFlushed = 0; altTrendRewrite = true;
  longTermCount = 0; longTermHead = 0;
  zeroLogCount = 0; zeroLogHead = 0;
  bhCapCount = 0; bhCapHead = 0;
  request->send(200, "text/plain",
    "CLEARMAX done — in-RAM rings zeroed; persistence frozen. REBOOT to reload from flash\n"
    "(real data — unless a fillmax?persist=yes wrote synthetic there; then Erase All Flash).\n");
}

// /debug/frontcheck (bench only): proves the record-book lookup engine (FrontQuery) returns what the inline reads
// (classify, eval, evalLWLR, hasLocalSupport, cellOf, nearestNormDist, pushesHybrid) return, bit for bit, on this
// board's own fronts, and times both. Runs on async_tcp; field cut only, so no alternator runs are emitted meanwhile.
//   ?confirm=yes[&n=16]   per front: n stored records exactly, n moved 0.3 cell (in-cell), n moved 0.6 cell
//                         (estimate path), n random points in the records' bounding box; then a second's work, inline
//                         vs engine, min of 5 (async_tcp is preempted by WiFi/lwIP, so single samples run long)
//   ?drive=S[&mode=old]   for S s (1-300) with the field cut, a two-run second's lookups each second through the live
//                         Core 1 path, results unused (mode=old: the inline reads, all in the 1 Hz pass); read the
//                         Record-book engine line on /debug. ?drive=0 stops.
struct FrontCheck {
  static uint32_t bits(float v) { uint32_t u; memcpy(&u, &v, 4); return u; }
  template <int N>
  static void run(FrontStore<N> &fs, const char *name, int nPer, float idwP, float ridge, float radius, float risk,
                  float margin, String &out) {
    char line[200];
    if (fs.count <= 0) { out += String(name) + ": empty, skipped\n"; return; }
    const uint32_t g0 = frontGenNow();
    const int c0 = fs.count;
    float lo[N], hi[N];
    for (int a = 0; a < N; a++) { lo[a] = 1e30f; hi[a] = -1e30f; }
    for (int i = 0; i < fs.count; i++)
      for (int a = 0; a < N; a++) { if (fs.pts[i].x[a] < lo[a]) lo[a] = fs.pts[i].x[a]; if (fs.pts[i].x[a] > hi[a]) hi[a] = fs.pts[i].x[a]; }
    FrontQuery<N> q;
    int tested = 0, matched = 0, byState[4] = { 0, 0, 0, 0 }, shown = 0;
    uint32_t lcg = 0x9E3779B9u;
    for (int set = 0; set < 4; set++) {
      for (int k = 0; k < nPer; k++) {
        float x[N], yTest;
        if (set == 0) {
          const FrontPoint<N> &p = fs.pts[((uint32_t)k * 7919u) % (uint32_t)fs.count];
          for (int a = 0; a < N; a++) x[a] = p.x[a];
          yTest = p.y;
        } else if (set < 3) {
          FrontQuery<N>::probe(fs, (uint32_t)k + 101u * set, set == 1 ? 0.3f : 0.6f, x);
          yTest = fs.pts[(((uint32_t)k + 101u * set) * 7919u) % (uint32_t)fs.count].y;
        } else {
          for (int a = 0; a < N; a++) {
            lcg = lcg * 1664525u + 1013904223u;
            float span = hi[a] - lo[a];
            x[a] = lo[a] - 0.1f * span + 1.2f * span * ((float)(lcg >> 8) / 16777216.0f);
          }
          yTest = fs.pts[k % fs.count].y;
        }
        float oPred = 0, oBar = 0, oLp = 0;
        int oSt = fs.classify(x, radius, idwP, ridge, risk, &oPred, &oBar);
        float oNear = fs.nearestNormDist(x);
        int oCell = fs.cellOf(x);
        bool oLocal = fs.hasLocalSupport(x);
        float oIdw = fs.eval(x, idwP);
        bool oOk = fs.evalLWLR(x, ridge, &oLp, nullptr);
        bool oPush = fs.pushesHybrid(x, yTest, margin, idwP, ridge);
        q.begin(fs, x, idwP, ridge, radius, risk, frontGenNow());
        while (!q.step(INT64_MAX)) {}
        bool nPush = (fs.count <= 0) ? true : (yTest > q.bar - margin);
        bool ok = q.phase == FrontQuery<N>::DONE && oSt == q.state && bits(oPred) == bits(q.pred)
                  && bits(oBar) == bits(q.bar) && bits(oNear) == bits(q.nearest) && oCell == q.cell
                  && oLocal == (q.cell >= 0) && bits(oIdw) == bits(q.idw) && oOk == q.lwlrOk
                  && (!oOk || bits(oLp) == bits(q.lp)) && oPush == nPush;
        tested++;
        if (ok) matched++;
        if (q.state >= 0 && q.state < 4) byState[q.state]++;
        if (!ok && shown < 3) {
          shown++;
          snprintf(line, sizeof(line), "  MISMATCH set %d #%d: state %d/%d pred %08lx/%08lx bar %08lx/%08lx near %08lx/%08lx cell %d/%d lwlr %d/%d\n",
                   set, k, oSt, q.state, (unsigned long)bits(oPred), (unsigned long)bits(q.pred),
                   (unsigned long)bits(oBar), (unsigned long)bits(q.bar), (unsigned long)bits(oNear),
                   (unsigned long)bits(q.nearest), oCell, q.cell, (int)oOk, (int)q.lwlrOk);
          out += line;
        }
      }
    }
    // A second's work at this count, near stored records on the estimate path (the inline reads' most passes).
    uint32_t oldLive = UINT32_MAX, oldRun = UINT32_MAX, eng = UINT32_MAX;
    volatile float sink = 0;
    for (int r = 0; r < 5; r++) {
      float x[N], pr = 0, br = 0;
      FrontQuery<N>::probe(fs, 7u + (uint32_t)r, 0.6f, x);
      uint32_t t0 = micros();
      if (N == ALT_NAXIS) sink = sink + fs.nearestNormDist(x);           // the alt live grade's extra read
      sink = sink + (float)fs.classify(x, radius, idwP, ridge, risk, &pr);
      uint32_t t1 = micros();
      if (N == ALT_NAXIS) {                                               // altProcessEmits, My History active
        sink = sink + (float)fs.classify(x, radius, idwP, ridge, risk, &pr);
        sink = sink + (float)fs.classify(x, radius, idwP, ridge, risk, &pr, &br);
      } else {                                                            // the inline perf fold
        sink = sink + (fs.pushesHybrid(x, 0.0f, margin, idwP, ridge) ? 1.0f : 0.0f);
      }
      sink = sink + (fs.hasLocalSupport(x) ? 1.0f : 0.0f) + (float)fs.cellOf(x);
      uint32_t t2 = micros();
      q.begin(fs, x, idwP, ridge, radius, risk, frontGenNow());
      while (!q.step(INT64_MAX)) {}
      uint32_t t3 = micros();
      sink = sink + q.pred;
      if (t1 - t0 < oldLive) oldLive = t1 - t0;
      if (t2 - t1 < oldRun) oldRun = t2 - t1;
      if (t3 - t2 < eng) eng = t3 - t2;
    }
    const bool moved = !frontGenSame(g0) || fs.count != c0;
    snprintf(line, sizeof(line), "%s (%d pts): %d/%d identical%s; engine states measured %d, estimated %d, learning %d, no reference %d\n",
             name, c0, matched, tested, moved ? " (FRONT CHANGED DURING THE CHECK: rerun)" : "",
             byState[0], byState[1], byState[2], byState[3]);
    out += line;
    snprintf(line, sizeof(line), "  one second's work, inline vs engine (us, min of 5): live grade %lu vs %lu; with %s %lu vs %lu\n",
             (unsigned long)oldLive, (unsigned long)eng, N == ALT_NAXIS ? "two runs" : "one run",
             (unsigned long)(N == ALT_NAXIS ? oldLive + 2 * oldRun : oldLive + oldRun),
             (unsigned long)(N == ALT_NAXIS ? 3 * eng : 2 * eng));
    out += line;
  }
};

void debugFrontCheck(AsyncWebServerRequest *request) {
  if (request->hasParam("drive")) {
    long sec = request->getParam("drive")->value().toInt();
    if (sec <= 0) { frontDriveUntilMs = 0; request->send(200, "text/plain", "frontcheck drive off\n"); return; }
    if (!gpio4IsLow) { request->send(200, "text/plain", "REFUSED: the field is on. Drive runs only with the field cut.\n"); return; }
    if (sec > 300) sec = 300;
    frontDriveOld = request->hasParam("mode") && request->getParam("mode")->value() == "old";
    uint32_t until = millis() + (uint32_t)sec * 1000u;
    frontDriveUntilMs = until ? until : 1;
    String m = "frontcheck drive " + String(frontDriveOld ? "OLD (inline reads, one pass per second)" : "NEW (lookup engine)")
             + " for " + String(sec) + " s, field cut only (stops when the field turns on).\n"
             + "Reset Peak Values first, then read the Record-book engine line and ft_altHealth / ft_boatPerf on /debug.\n";
    request->send(200, "text/plain", m);
    return;
  }
  if (!request->hasParam("confirm") || request->getParam("confirm")->value() != "yes") {
    request->send(200, "text/plain",
      "REFUSED. /debug/frontcheck?confirm=yes[&n=16] compares the lookup engine with the inline reads on every front\n"
      "and times both (blocks the web server for seconds at 4096 points). Field cut only. Bench units only.\n"
      "/debug/frontcheck?drive=60[&mode=old] runs a two-run second's lookups each second on Core 1; drive=0 stops.\n");
    return;
  }
  if (!gpio4IsLow) { request->send(200, "text/plain", "REFUSED: the field is on. Cut the field and wait for it to settle.\n"); return; }
  if (frontDriveUntilMs) { request->send(200, "text/plain", "REFUSED: a drive is running; /debug/frontcheck?drive=0 first.\n"); return; }
  int nPer = request->hasParam("n") ? request->getParam("n")->value().toInt() : 16;
  if (nPer < 1) nPer = 1;
  if (nPer > 256) nPer = 256;
  String out = "FRONTCHECK: lookup engine vs inline reads, bit for bit (state, pred, bar, idw, lwlr, cell, nearest, admit)\n";
  if (altFrontBuf) {
    FrontCheck::run(altFront2, "alt My History", nPer, altIdwPower, altRidgeFrac, altRefRadius, altRiskThresh, altSafetyMargin, out);
    if (altHaveUpload) FrontCheck::run(altFrontUp, "alt Uploaded", nPer, altIdwPower, altRidgeFrac, altRefRadius, altRiskThresh, altSafetyMargin, out);
  }
  if (sailFrontBuf) {
    FrontCheck::run(sailFront, "sail", nPer, perfIdwPower, perfRidgeFrac, perfRefRadius, perfRiskThresh, perfSafetyMargin, out);
    FrontCheck::run(motorFront, "motor", nPer, perfIdwPower, perfRidgeFrac, perfRefRadius, perfRiskThresh, perfSafetyMargin, out);
  }
  out += "Engine work per loop pass is capped at " + String(FRONT_STEP_US) + " us per engine; live per-pass and per-second figures are on /debug.\n";
  request->send(200, "text/plain", out);
}

// ════════════════ Commissioning Ledger (COMMISSIONING_LEDGER_SPEC.md) ════════════════
// Append-only per-run evidence (stage completions, skips/hand-sets, finishes, Bode sweeps). Stages in PSRAM (any core),
// flushes to /cxledger.bin on Core 1, drains to log-commissioning-event under the field-off cloud gates. Cloud dedupes
// on (device_uid, seq), so retries are harmless. The daily snapshot keeps the LIVE values; this keeps frozen evidence.

void cxLedgerInit() {
  cxPendBuf  = (char *)ps_malloc(CX_LEDGER_PEND_CAP);
  cxPendSwap = (char *)ps_malloc(CX_LEDGER_PEND_CAP);
  if (settingExists(NK_cxLedgerSeq)) cxLedgerSeq = (uint32_t)settingRead(NK_cxLedgerSeq).toInt();
}

// Stage one event row in PSRAM; safe from either core (spinlock), no flash I/O. dataJson must be a complete {...}.
// Rows that don't fit are counted and reported as data.dropped_prior on the next row that does.
static void cxLedgerAppend(const char *eventType, int stage, const String &dataJson) {
  if (!cxPendBuf) return;
  uint32_t seq, dropped;
  portENTER_CRITICAL(&cxPendMux);
  seq = ++cxLedgerSeq;
  dropped = cxLedgerDroppedRows;
  cxLedgerDroppedRows = 0;
  portEXIT_CRITICAL(&cxPendMux);

  String row;
  row.reserve(dataJson.length() + 160);
  row = "{\"seq\":";
  row += String((unsigned long)seq);
  row += ",\"event_type\":\"";
  row += eventType;
  row += '"';
  if (stage >= 0) { row += ",\"stage\":"; row += String(stage); }
  row += ",\"fw_version\":\"";
  row += FIRMWARE_VERSION;
  row += "\",\"device_epoch\":";
  row += String((unsigned long)getCurrentTimestamp());
  row += ",\"data\":";
  if (dropped > 0 && dataJson.length() >= 2) {
    row += "{\"dropped_prior\":";
    row += String((unsigned long)dropped);
    if (dataJson.length() > 2) { row += ','; row += dataJson.substring(1); }
    else row += '}';
  } else {
    row += dataJson;
  }
  row += '}';

  uint32_t need = (uint32_t)row.length() + 2;
  bool ok = false;
  portENTER_CRITICAL(&cxPendMux);
  if (row.length() <= CX_LEDGER_ROW_MAX && cxPendLen + need <= CX_LEDGER_PEND_CAP) {
    uint16_t len = (uint16_t)row.length();
    memcpy(cxPendBuf + cxPendLen, &len, 2);
    memcpy(cxPendBuf + cxPendLen + 2, row.c_str(), len);
    cxPendLen += need;
    ok = true;
  } else {
    cxLedgerDroppedRows += dropped + 1;  // this row plus the earlier drops it would have confessed
  }
  portEXIT_CRITICAL(&cxPendMux);
  if (ok) cxLedgerFlushPending = true;
}

// Stage-specific evidence, read when the stage is marked complete. "applied" carries AFTER values only; the daily
// snapshot history covers earlier states.
static String cxLedgerStageData(int stage) {
  String d;
  d.reserve(1536);
  d = "{";
  switch (stage) {
    case 1: {  // Field curve: raw duty→amps ramp — RAM-only elsewhere, this row is its only persistence
      d += "\"curve\":[";
      for (int i = 0; fieldCurveBuf && i < fieldCurveCount; i++) {
        if (i) d += ',';
        d += '[';
        cfgAppendNum(d, fieldCurveBuf[i].duty, 2);
        d += ',';
        cfgAppendNum(d, fieldCurveBuf[i].amps, 1);
        d += ']';
      }
      d += "],\"applied\":{\"SystemIDStabilizeAmps\":";
      cfgAppendNum(d, SystemIDStabilizeAmps, 1);
      d += ",\"SystemIDStepAmplitude\":";
      cfgAppendNum(d, SystemIDStepAmplitude, 2);
      d += '}';
      break;
    }
    case 2: {  // Current-loop plant fit
      d += "\"plant_tau_ms\":";
      d += String((int)systemIDPlantTauMs);
      d += ",\"applied\":{\"PidKp\":"; cfgAppendNum(d, PidKp, 4);
      d += ",\"PidKi\":"; cfgAppendNum(d, PidKi, 4);
      d += ",\"OutputPIDFilterTC\":"; cfgAppendNum(d, OutputPIDFilterTC, 1);
      d += ",\"VoltageFilterTC\":"; cfgAppendNum(d, VoltageFilterTC, 1);
      d += '}';
      break;
    }
    case 3: {  // Verify: gains as left in force (an accepted "Soften 20%" lands here too)
      d += "\"kp\":"; cfgAppendNum(d, PidKp, 4);
      d += ",\"ki\":"; cfgAppendNum(d, PidKi, 4);
      d += ",\"kd\":"; cfgAppendNum(d, PidKd, 4);
      break;
    }
    case 4: {  // Disturbances: committed per-RPM map + the 3-level fit projections (raw points inside)
      d += "\"level_a\":"; cfgAppendNum(d, ripTab.sess.levelA, 1);
      d += ",\"ibv_min\":"; cfgAppendNum(d, ripTab.sess.ibvMinV, 2);
      d += ",\"ibv_max\":"; cfgAppendNum(d, ripTab.sess.ibvMaxV, 2);
      d += ",\"idle_rpm\":"; d += String((int)ripTab.sess.idleRpm);
      d += ",\"map\":[";
      {
        bool first = true;
        for (int r = 0; r < RIPTAB_BINS; r++) {
          const RipTabCell *c = &ripTab.cell[r];
          if (c->state == 0) continue;
          int altSt  = (c->state & RIPTAB_ALT_DONE)  ? 2 : (c->state & RIPTAB_ALT_PEND)  ? 1 : 0;
          int battSt = (c->state & RIPTAB_BATT_DONE) ? 2 : (c->state & RIPTAB_BATT_PEND) ? 1 : 0;
          uint16_t aV = (altSt == 2)  ? c->altPkX100  : c->altPendX100;
          uint16_t bV = (battSt == 2) ? c->battPkX100 : c->battPendX100;
          if (!first) d += ',';
          first = false;
          d += '[';
          d += String(r * FA_RPM_BIN_W); d += ',';
          cfgAppendNum(d, aV / 100.0f, 2); d += ',';
          cfgAppendNum(d, bV / 100.0f, 2); d += ',';
          d += String(altSt); d += ',';
          d += String(battSt);
          d += ']';
        }
      }
      d += "],\"rip_fit\":";
      cfgAppendJsonStr(d, ripFitEncode(ripFitAlt));
      d += ",\"slp_fit\":";
      cfgAppendJsonStr(d, ripFitEncode(slpFitAlt));
      break;
    }
    case 5: {  // Fault thresholds: the affine trip line + D-term deadband line as applied
      d += "\"applied\":{\"IExcessFrac\":"; cfgAppendNum(d, IExcessFrac, 4);
      d += ",\"IExcessFracBulk\":"; cfgAppendNum(d, IExcessFracBulk, 4);
      d += ",\"IExcessBaseA\":"; cfgAppendNum(d, IExcessBaseA, 2);
      d += ",\"CvKdDeadbandVps\":"; cfgAppendNum(d, CvKdDeadbandVps, 3);
      d += ",\"CvKdDbSlope\":"; cfgAppendNum(d, CvKdDbSlope, 5);
      d += '}';
      break;
    }
    case 6: {  // CV plant fit: stiffness + the 8-edge table behind the median + gains it computed
      d += "\"plant_ka\":"; cfgAppendNum(d, cvPlantKa, 5);
      d += ",\"edges_k\":[";
      for (int i = 0; i < 8; i++) { if (i) d += ','; cfgAppendNum(d, cvpfEdgeK[i], 2); }
      d += "],\"edges_stat\":[";  // 0 used · 1 weak step · 2 wrong-way · 3 window starved · 4 not fired
      for (int i = 0; i < 8; i++) { if (i) d += ','; d += String((int)cvpfEdgeStat[i]); }
      d += "],\"computed\":{\"kp\":"; cfgAppendNum(d, cvComputedKp, 2);
      d += ",\"ki\":"; cfgAppendNum(d, cvComputedKi, 2);
      d += ",\"kd\":"; cfgAppendNum(d, cvComputedKd, 2);
      d += "},\"cond\":{\"base_a\":"; cfgAppendNum(d, cvpfBaseA, 1);
      d += ",\"step_a\":"; cfgAppendNum(d, cvpfStepA, 1);
      d += ",\"batt_v\":"; cfgAppendNum(d, cvpfBattVAtFit, 3);
      d += "},\"commission_temp_f\":"; cfgAppendNum(d, CommissionTempF, 1);
      break;
    }
    case 7: {  // Min% floor + field decay: as-commissioned tables + the drain-vs-RPM line
      d += "\"rpm_axis\":[";
      for (int i = 0; i < RPM_TABLE_SIZE; i++) { if (i) d += ','; d += String(rpmTableRPMPoints[i]); }
      d += "],\"min_duty_floor\":"; cfgAppendFloatArr(d, rpmMinDutyTable, RPM_TABLE_SIZE, 2);
      d += ",\"knee_detected\":"; cfgAppendFloatArr(d, kneeKnee, RPM_TABLE_SIZE, 2);
      d += ",\"knee_frozen\":[";
      for (int i = 0; i < RPM_TABLE_SIZE; i++) { if (i) d += ','; d += (kneeFrozen[i] ? '1' : '0'); }
      d += "],\"drain\":{\"lo_ms\":"; d += String((int)fdDrainLoMs);
      d += ",\"hi_ms\":"; d += String((int)fdDrainHiMs);
      d += ",\"rpm_lo\":"; d += String((int)fdDrainRpmLo);
      d += ",\"rpm_hi\":"; d += String((int)fdDrainRpmHi);
      d += ",\"tau_ms\":"; d += String((int)fieldDecayTauMs);
      d += '}';
      break;
    }
    case 8: {  // Stress test: the full graded record, verbatim (layout at cvStressPersistResult())
      d += "\"stress\":";
      cfgAppendJsonStr(d, String(cvsLastBlob));
      break;
    }
    default: break;  // Prep (0): the row's existence + timestamp is the content
  }
  d += '}';
  return d;
}

// action: "stage" (measured completion — carries evidence), "manual", or "skip".
void cxLedgerLogStage(int stage, const char *action) {
  cxLedgerAppend(action, stage,
                 (strcmp(action, "stage") == 0) ? cxLedgerStageData(stage) : String("{}"));
}

void cxLedgerLogFinish() {
  String d;
  d.reserve(192);
  d = "{\"done_mask\":";
  d += String((int)commissionDoneMask);
  d += ",\"manual_mask\":";
  d += String((int)commissionManualMask);
  d += ",\"state\":";
  d += String((int)commissionState);
  d += ",\"commission_temp_f\":";
  cfgAppendNum(d, CommissionTempF, 1);
  d += ",\"commission_epoch\":";
  {
    char eb[24];
    snprintf(eb, sizeof(eb), "%lld", (long long)CommissionEpoch);  // time_t is 64-bit
    d += eb;
  }
  d += '}';
  cxLedgerAppend("finish", -1, d);
}

// which: 0 = open-loop plant sweep (SystemID), 1 = closed-loop tuning sweep.
// Reads the record just committed into the respective 50-slot ring.
void cxLedgerLogSweep(int which) {
  String d;
  d.reserve(1024);
  if (which == 0) {
    if (!sysidSweepLog || sysidSweepLogCount == 0) return;
    const SysIDSweepRecord *r = &sysidSweepLog[(sysidSweepLogHead + 49) % 50];
    d = "{\"run\":"; d += String(r->runNumber);
    d += ",\"rolloff_hz\":"; cfgAppendNum(d, r->rolloffHz, 2);
    d += ",\"dc_gain_a_per_pct\":"; cfgAppendNum(d, r->dcGainApPct, 3);
    d += ",\"worst_phase_deg\":"; cfgAppendNum(d, r->worstPhaseDeg, 1);
    d += ",\"worst_phase_hz\":"; cfgAppendNum(d, r->worstPhaseFreqHz, 2);
    d += ",\"amp_pct\":"; cfgAppendNum(d, r->setupAmplitude, 2);
    d += ",\"floor_a\":"; cfgAppendNum(d, r->stabilizeAmps, 1);
    d += ",\"rpm\":"; cfgAppendNum(d, r->avgRPM, 0);
    d += ",\"alt_temp_f\":"; cfgAppendNum(d, r->avgAltTempF, 1);
    d += ",\"batt_v\":"; cfgAppendNum(d, r->battV, 2);
    d += ",\"stage_code\":"; d += String((int)r->chargeStage);
    d += ",\"curve\":[";
    for (int i = 0; i < r->nPoints; i++) {
      if (i) d += ',';
      d += '[';
      cfgAppendNum(d, r->curve[i].freqHz, 3); d += ',';
      cfgAppendNum(d, r->curve[i].gainApPct, 3); d += ',';
      cfgAppendNum(d, r->curve[i].phaseDeg, 1);
      d += ']';
    }
    d += "]}";
    cxLedgerAppend("bode_sysid", -1, d);
  } else {
    if (!tuningSweepLog || tuningSweepLogCount == 0) return;
    const TuningSweepRecord *r = &tuningSweepLog[(tuningSweepLogHead + 49) % 50];
    d = "{\"run\":"; d += String(r->runNumber);
    d += ",\"bandwidth_hz\":"; cfgAppendNum(d, r->bandwidthHz, 2);
    d += ",\"peak_gain\":"; cfgAppendNum(d, r->peakGain, 3);
    d += ",\"peak_gain_hz\":"; cfgAppendNum(d, r->peakGainFreqHz, 2);
    d += ",\"worst_phase_deg\":"; cfgAppendNum(d, r->worstPhaseDeg, 1);
    d += ",\"worst_phase_hz\":"; cfgAppendNum(d, r->worstPhaseFreqHz, 2);
    d += ",\"kp\":"; cfgAppendNum(d, r->kp, 4);
    d += ",\"ki\":"; cfgAppendNum(d, r->ki, 4);
    d += ",\"kd\":"; cfgAppendNum(d, r->kd, 4);
    d += ",\"sine_amp_a\":"; cfgAppendNum(d, r->sineAmpA, 1);
    d += ",\"base_a\":"; cfgAppendNum(d, r->baseA, 1);
    d += ",\"batt_v\":"; cfgAppendNum(d, r->battV, 2);
    d += ",\"rpm_min\":"; cfgAppendNum(d, r->rpmMin, 0);
    d += ",\"rpm_max\":"; cfgAppendNum(d, r->rpmMax, 0);
    d += ",\"worst_coherence\":"; cfgAppendNum(d, r->worstCoherence, 3);
    d += ",\"duty_railed\":"; d += String((int)r->dutyRailed);
    d += ",\"stage_code\":"; d += String((int)r->chargeStage);
    d += ",\"curve\":[";
    for (int i = 0; i < r->nPoints; i++) {
      if (i) d += ',';
      d += '[';
      cfgAppendNum(d, r->curve[i].freqHz, 3); d += ',';
      cfgAppendNum(d, r->curve[i].gain, 3); d += ',';
      cfgAppendNum(d, r->curve[i].phaseDeg, 1);
      d += ']';
    }
    d += "]}";
    cxLedgerAppend("bode_tuning", -1, d);
  }
}

// One "test" row per test ENDING (graded, failed or aborted), any entry point. Plain char*: callers in earlier .ino
// files can't see a String-arg prototype under Arduino auto-prototype ordering.
void cxLedgerLogTest(const char *dataJson) {
  cxLedgerAppend("test", -1, String(dataJson));
}

// Serialize the LAST COMMITTED record of a scored tuning run (called right after the ring write) into a "test" row.
// which: 0 = CC square wave (tuningLog), 1 = CV square wave (cvTuningLog), 2 = SystemID step (systemIDLog).
void cxLedgerLogTuneRun(int which) {
  String d;
  d.reserve(768);
  if (which == 0) {
    if (!tuningLog || tuningLogCount == 0) return;
    const TuningRecord *r = &tuningLog[(tuningLogHead + 49) % 50];
    d = "{\"test\":\"cc_square\",\"run\":"; d += String(r->runNumber);
    d += ",\"score\":"; cfgAppendNum(d, r->score, 2);
    d += ",\"worst_err_a\":"; cfgAppendNum(d, r->worstErrorA, 1);
    d += ",\"active_s\":"; cfgAppendNum(d, r->activeTimeSec, 1);
    d += ",\"kp\":"; cfgAppendNum(d, r->kp, 4);
    d += ",\"ki\":"; cfgAppendNum(d, r->ki, 4);
    d += ",\"kd\":"; cfgAppendNum(d, r->kd, 4);
    d += ",\"sample_div\":"; d += String((int)r->sampleDivisor);
    d += ",\"tracking_gain\":"; cfgAppendNum(d, r->trackingGain, 3);
    d += ",\"duty_ramp\":"; cfgAppendNum(d, r->dutyRampRate, 2);
    d += ",\"amp_a\":"; d += String((int)r->waveAmplitude);
    d += ",\"period_s\":"; d += String((int)r->wavePeriod);
    d += ",\"floor_a\":"; d += String((int)r->waveFloor);
    d += ",\"rpm\":"; cfgAppendNum(d, r->avgRPM, 0);
    d += ",\"alt_temp_f\":"; cfgAppendNum(d, r->avgAltTempF, 1);
    d += ",\"batt_v\":"; cfgAppendNum(d, r->battV, 2);
    d += ",\"stage_code\":"; d += String((int)r->chargeStage);
    if (r->note[0]) { d += ",\"note\":\""; d += r->note; d += '"'; }   // sanitizeTuningNote strips " \ < >
  } else if (which == 1) {
    if (!cvTuningLog || cvTuningLogCount == 0) return;
    const CVTuningRecord *r = &cvTuningLog[(cvTuningLogHead + 49) % 50];
    d = "{\"test\":\"cv_square\",\"run\":"; d += String(r->runNumber);
    d += ",\"score\":"; cfgAppendNum(d, r->score, 2);
    d += ",\"low_score\":"; cfgAppendNum(d, r->lowScore, 2);
    d += ",\"avg_settle_s\":"; cfgAppendNum(d, r->avgSettlingTimeSec, 2);
    d += ",\"avg_low_settle_s\":"; cfgAppendNum(d, r->avgLowSettlingTimeSec, 2);
    d += ",\"worst_ov_v\":"; cfgAppendNum(d, r->worstOvershootV, 3);
    d += ",\"worst_low_ov_v\":"; cfgAppendNum(d, r->worstLowOvV, 3);
    d += ",\"worst_low_under_v\":"; cfgAppendNum(d, r->worstLowUndershootV, 3);
    d += ",\"steady_p2p_v\":"; cfgAppendNum(d, r->steadyP2PV, 3);
    d += ",\"active_s\":"; cfgAppendNum(d, r->activeTimeSec, 1);
    d += ",\"fires\":{\"fast_ov\":"; d += String((int)r->fastOvFires);
    d += ",\"i_excess\":"; d += String((int)r->iExcessFires);
    d += ",\"load_dump\":"; d += String((int)r->loadDumpFires);
    d += ",\"hard_oc\":"; d += String((int)r->hardOcFires);
    d += "},\"kp\":"; cfgAppendNum(d, r->voltageKp, 2);
    d += ",\"ki\":"; cfgAppendNum(d, r->voltageKi, 2);
    d += ",\"kd\":"; cfgAppendNum(d, r->voltageKd, 1);
    d += ",\"amp_v\":"; cfgAppendNum(d, r->waveAmplitudeV, 2);
    d += ",\"period_s\":"; d += String((int)r->wavePeriodSec);
    d += ",\"target_v\":"; cfgAppendNum(d, r->chargingVoltageTarget, 2);
    d += ",\"rpm\":"; cfgAppendNum(d, r->avgRPM, 0);
    d += ",\"alt_temp_f\":"; cfgAppendNum(d, r->avgAltTempF, 1);
    d += ",\"batt_v\":"; cfgAppendNum(d, r->battVAtStart, 2);
    d += ",\"soc\":"; cfgAppendNum(d, r->socAtStart, 1);
    d += ",\"stage_code\":"; d += String((int)r->chargeStage);
    if (r->note[0]) { d += ",\"note\":\""; d += r->note; d += '"'; }
  } else {
    if (!systemIDLog || systemIDLogCount == 0) return;
    const SystemIDRecord *r = &systemIDLog[(systemIDLogHead + 49) % 50];
    d = "{\"test\":\"sysid_step\",\"run\":"; d += String(r->runNumber);
    d += ",\"ok\":"; d += (r->abortReason == 0) ? '1' : '0';
    d += ",\"abort_reason\":"; d += String((int)r->abortReason);
    d += ",\"abort_phase\":"; d += String((int)r->abortPhase);
    d += ",\"score\":"; cfgAppendNum(d, r->score, 0);
    d += ",\"rise_avg_ms\":"; cfgAppendNum(d, r->riseAvg_ms, 0);
    d += ",\"fall_avg_ms\":"; cfgAppendNum(d, r->fallAvg_ms, 0);
    d += ",\"rise_ms\":[";
    for (int i = 0; i < 3; i++) { if (i) d += ','; cfgAppendNum(d, r->riseDelays[i], 0); }
    d += "],\"fall_ms\":[";
    for (int i = 0; i < 3; i++) { if (i) d += ','; cfgAppendNum(d, r->fallDelays[i], 0); }
    d += "],\"step_a\":[";
    for (int i = 0; i < 3; i++) { if (i) d += ','; cfgAppendNum(d, r->stepAmps[i], 1); }
    d += "],\"quiet_pp_a\":[";
    for (int i = 0; i < 3; i++) { if (i) d += ','; cfgAppendNum(d, r->quietPP[i], 1); }
    d += "],\"amp_pct\":"; cfgAppendNum(d, r->setupStepAmplitude, 2);
    d += ",\"rpm\":"; cfgAppendNum(d, r->avgRPM, 0);
    d += ",\"alt_temp_f\":"; cfgAppendNum(d, r->avgAltTempF, 1);
    d += ",\"batt_v\":"; cfgAppendNum(d, r->battV, 2);
    d += ",\"stage_code\":"; d += String((int)r->chargeStage);
  }
  d += '}';
  cxLedgerAppend("test", -1, d);
}

// CV plant-fit "test" row: the fit on success (stage-6 shape plus conditions), else the abort text. Called once
// cvpfState settles (cvpfAbort, cvpfServiceCompletion).
void cxLedgerLogCvpf() {
  String d;
  d.reserve(512);
  if (cvpfOk) {
    d = "{\"test\":\"cv_plant_fit\",\"ok\":1,\"plant_ka\":"; cfgAppendNum(d, cvpfKa, 5);
    d += ",\"edges_k\":[";
    for (int i = 0; i < 8; i++) { if (i) d += ','; cfgAppendNum(d, cvpfEdgeK[i], 2); }
    d += "],\"edges_stat\":[";
    for (int i = 0; i < 8; i++) { if (i) d += ','; d += String((int)cvpfEdgeStat[i]); }
    d += "],\"kp\":"; cfgAppendNum(d, cvpfKp, 2);
    d += ",\"ki\":"; cfgAppendNum(d, cvpfKi, 2);
    d += ",\"dv_mv\":"; cfgAppendNum(d, cvpfDV * 1000.0f, 0);
    d += ",\"di_a\":"; cfgAppendNum(d, cvpfDI, 2);
    d += ",\"snr\":"; cfgAppendNum(d, cvpfSNR, 1);
    d += ",\"warn\":"; d += String((int)cvpfWarn);
    d += ",\"base_a\":"; cfgAppendNum(d, cvpfBaseA, 1);
    d += ",\"step_a\":"; cfgAppendNum(d, cvpfStepA, 1);
    d += ",\"rpm\":"; cfgAppendNum(d, cvpfRpmAtFit, 0);
    d += ",\"drift_setup_pct\":"; cfgAppendNum(d, cvpfDriftSetupPct, 1);
    d += ",\"drift_train_pct\":"; cfgAppendNum(d, cvpfDriftTrainPct, 1);
    d += ",\"batt_v\":"; cfgAppendNum(d, cvpfBattVAtFit, 3);
    d += ",\"soc\":"; cfgAppendNum(d, cvpfSocAtFit, 1);
    d += '}';
  } else {
    d = "{\"test\":\"cv_plant_fit\",\"ok\":0,\"abort\":\"";
    d += cvpfAbortMsg;   // fixed literals or cvpfDiagBuf-composed diagnostics — neither contains quote characters
    d += "\"}";
  }
  cxLedgerAppend("test", -1, d);
}

static uint32_t cxHeldLen = 0;   // bytes of rows parked in cxPendSwap after a failed compaction buffer (0 = none)

// Core 1 only (loop() deferred-writes cluster): staged rows → /cxledger.bin. Every row comes from a user-driven
// wizard or tuning action, so this flash write never fires autonomously; the paced retry of held rows waits for the field cut.
void cxLedgerFlush() {
  static uint32_t heldAtMs = 0;
  static bool noMemNoted = false;      // one console line per run of failed compaction buffers
  if (!cxPendBuf || !cxPendSwap) return;
  char *rows;
  uint32_t n;
  if (cxHeldLen) {
    if (millis() - heldAtMs < 60000UL || !fieldCutSettled(10000)) {
      cxLedgerFlushPending = true;
      return;
    }
    rows = cxPendSwap;
    n = cxHeldLen;
  } else {
    portENTER_CRITICAL(&cxPendMux);
    n = cxPendLen;
    if (n == 0) { portEXIT_CRITICAL(&cxPendMux); return; }
    rows = cxPendBuf;
    cxPendBuf = cxPendSwap;   // producers keep appending into the other buffer
    cxPendSwap = rows;
    cxPendLen = 0;
    portEXIT_CRITICAL(&cxPendMux);
  }

  // Seq persists BEFORE the rows land: a crash between the two burns a gap in the sequence
  // (harmless) instead of reusing numbers (the cloud would silently drop the reuse as dupes).
  settingWrite(NK_cxLedgerSeq, String((unsigned long)cxLedgerSeq).c_str());

  fsTakeLock();
  uint32_t cur = 0;
  {
    File f = LittleFS.open(CX_LEDGER_PATH, "r");
    if (f) { cur = f.size(); f.close(); }
  }
  if (cur + n > CX_LEDGER_FILE_CAP) {
    // Compact: drop oldest rows until the incoming batch fits (newest evidence wins).
    char *tmp = (char *)ps_malloc(CX_LEDGER_FILE_CAP);
    if (tmp) {
      uint32_t sz = 0;
      File f = LittleFS.open(CX_LEDGER_PATH, "r");
      if (f) { sz = f.read((uint8_t *)tmp, CX_LEDGER_FILE_CAP); f.close(); }
      uint32_t off = 0, droppedRows = 0;
      while (off < sz) {
        uint16_t len;
        memcpy(&len, tmp + off, 2);
        if (len == 0 || len > CX_LEDGER_ROW_MAX || off + 2 + len > sz) { sz = off; break; }  // corrupt tail: discard the rest
        if (sz - off + n <= CX_LEDGER_FILE_CAP) break;
        off += 2 + (uint32_t)len;
        droppedRows++;
      }
      uint32_t kept = (off <= sz) ? sz - off : 0;
      File w = LittleFS.open(CX_LEDGER_PATH, "w");
      if (w) {
        if (kept) w.write((uint8_t *)tmp + off, kept);
        w.close();
      }
      free(tmp);
      // The dropped head rows may be the batch in flight: shrink the post-send trim so it never cuts unsent rows.
      if (cxLedgerUpState == 1 || cxLedgerUpState == 2) cxLedgerUpBytes = (cxLedgerUpBytes > off) ? cxLedgerUpBytes - off : 0;
      if (droppedRows) {
        portENTER_CRITICAL(&cxPendMux);
        cxLedgerDroppedRows += droppedRows;
        portEXIT_CRITICAL(&cxPendMux);
      }
    } else {
      // No PSRAM for compaction: leave the file whole and park this batch in cxPendSwap. Producers keep filling
      // cxPendBuf meanwhile (overflow is counted into dropped_prior); no swap happens until the held rows land.
      fsReleaseLock();
      cxHeldLen = n;
      heldAtMs = millis();
      cxLedgerFlushPending = true;
      if (!noMemNoted) {
        noMemNoted = true;
        queueConsoleMessageF("Commissioning ledger: no %u KB PSRAM block to make room in the queue file - file kept, new rows held in RAM, retrying each minute with the field off",
                             (unsigned)(CX_LEDGER_FILE_CAP / 1024));
      }
      return;
    }
  }
  File f = LittleFS.open(CX_LEDGER_PATH, "a");
  if (f) {
    f.write((uint8_t *)rows, n);
    f.close();
  }
  fsFreeDirty = true;
  fsReleaseLock();
  if (cxHeldLen) {
    cxHeldLen = 0;
    noMemNoted = false;
    if (cxPendLen) cxLedgerFlushPending = true;   // rows staged while this batch was held
  }
}

// Core 1 (cloud-features block; caller gates hardware + field-off — the post-send trim is a
// flash write). Network gates live here. One batch in flight at a time; sent bytes trim off
// the file head only after the cloud confirms.
void cxLedgerDrainService() {
  if (demoSeal) return;   // commissioning is refused in demo; anything queued before entry waits
  static uint32_t trimRetryAtMs = 0;   // 0 = no trim waiting on PSRAM
  static bool noMemNoted = false;      // one console line per run of failed buffer allocations
  if (cxLedgerUpState == 1) return;                            // batch in flight
  if (cxLedgerUpState == -1) { cxLedgerUpState = 0; return; }  // failed: retry paced by the 60s guard below
  if (cxLedgerUpState == 2) {                                  // cloud has the rows → drop the sent prefix
    if (trimRetryAtMs && millis() - trimRetryAtMs < 60000UL) return;
    bool trimmed = true;
    fsTakeLock();
    File f = (cxLedgerUpBytes > 0) ? LittleFS.open(CX_LEDGER_PATH, "r") : File();   // 0 = compaction already dropped the whole batch
    if (f) {
      uint32_t sz = f.size();
      if (cxLedgerUpBytes >= sz) {
        f.close();
        LittleFS.remove(CX_LEDGER_PATH);
      } else {
        uint32_t rem = sz - cxLedgerUpBytes;
        char *tmp = (char *)ps_malloc(rem);
        uint32_t kept = 0;
        if (tmp) {
          f.seek(cxLedgerUpBytes);
          kept = f.read((uint8_t *)tmp, rem);
        }
        f.close();
        // No buffer or a short read: leave the file whole (the sent rows stay at its head) and retry the trim in a minute;
        // state stays 2, so nothing is re-sent meanwhile.
        if (tmp && kept == rem) {
          File w = LittleFS.open(CX_LEDGER_PATH, "w");
          if (w) {
            w.write((uint8_t *)tmp, kept);
            w.close();
          }
        } else {
          trimmed = false;
        }
        if (tmp) free(tmp);
        if (!tmp && !noMemNoted) {
          noMemNoted = true;
          queueConsoleMessageF("Commissioning ledger: no %u KB PSRAM block to trim the uploaded rows - file kept, retrying each minute",
                               (unsigned)((rem + 1023) / 1024));
        } else if (tmp) {
          noMemNoted = false;
        }
      }
    }
    if (trimmed) fsFreeDirty = true;
    fsReleaseLock();
    if (!trimmed) {
      trimRetryAtMs = millis() ? millis() : 1;
      return;
    }
    trimRetryAtMs = 0;
    cxLedgerUpBytes = 0;
    cxLedgerUpState = 0;
    return;
  }
  if (millis() - cxLedgerLastAttemptMs < 60000UL) return;
  if (!(currentMode == MODE_CLIENT && WiFi.status() == WL_CONNECTED && isRegistered && WiFi.RSSI() >= -80)) return;
  // Held rows (no compaction buffer) may need this drain's trim to make room for them, so it must not wait on them.
  if ((cxPendLen > 0 || cxLedgerFlushPending) && !cxHeldLen) return;  // let staged rows reach the file first — one batch, not two

  fsTakeLock();
  File f = LittleFS.open(CX_LEDGER_PATH, "r");
  if (!f || f.size() == 0) {
    if (f) f.close();
    fsReleaseLock();
    return;
  }
  uint32_t sz = f.size();
  char *body = (char *)ps_malloc(CX_LEDGER_BATCH_CAP);
  if (!body) {
    f.close();
    fsReleaseLock();
    cxLedgerLastAttemptMs = millis();
    if (!noMemNoted) {
      noMemNoted = true;
      queueConsoleMessageF("Commissioning ledger: no %u KB PSRAM block for the upload batch - rows kept, retrying",
                           (unsigned)(CX_LEDGER_BATCH_CAP / 1024));
    }
    return;
  }
  noMemNoted = false;
  int off = snprintf(body, CX_LEDGER_BATCH_CAP,
                     "{\"device_uid\":\"%s\",\"token\":\"%s\",\"events\":[",
                     device_id_hex, authToken.c_str());
  uint32_t consumed = 0;
  int rows = 0;
  bool corrupt = false;
  while (consumed < sz && rows < CX_LEDGER_BATCH_ROWS) {
    uint16_t len = 0;
    f.seek(consumed);
    if (f.read((uint8_t *)&len, 2) != 2 || len == 0 || len > CX_LEDGER_ROW_MAX || consumed + 2 + len > sz) {
      corrupt = true;
      break;
    }
    if ((uint32_t)off + len + 4 > CX_LEDGER_BATCH_CAP) break;  // batch full — next pass takes the rest
    if (rows) body[off++] = ',';
    if (f.read((uint8_t *)body + off, len) != (int)len) { corrupt = true; break; }
    off += len;
    consumed += 2 + (uint32_t)len;
    rows++;
  }
  f.close();
  if (corrupt && rows == 0) {
    // Unreadable from the first row: the queue file is garbage — drop it rather than wedge.
    LittleFS.remove(CX_LEDGER_PATH);
    fsReleaseLock();
    free(body);
    queueConsoleMessage("Commissioning ledger: corrupt queue discarded");
    return;
  }
  fsReleaseLock();
  if (rows == 0) { free(body); return; }
  off += snprintf(body + off, CX_LEDGER_BATCH_CAP - off, "]}");

  HttpsRequest req = {};
  req.type = HTTPS_UPLOAD_CX_LEDGER;
  req.payloadCap = CX_LEDGER_BATCH_CAP;
  req.payload = body;
  cxLedgerLastAttemptMs = millis();
  cxLedgerUpBytes = consumed;
  cxLedgerUpState = 1;
  if (xQueueSend(httpsQueue, &req, 0) != pdTRUE) {
    free(req.payload);
    cxLedgerUpState = 0;
    cxLedgerUpBytes = 0;
  }
}
