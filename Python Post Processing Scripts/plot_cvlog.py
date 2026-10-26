"""
plot_cvlog.py
Diagnostic plotter for CVlog data from the ESP32 alternator regulator.

6 plot windows:
  Plot 1 — Voltage: battV, battV_filt_V, ovFilt_V, targV, vPred | duty% right axis
  Plot 2 — Current command chain (top) + overvoltage protection layers (bottom)
  Plot 3 — Engine RPM + CH1 scheduling jitter | duty% right axis
  Plot 4 — CV PID term decomposition (P / I / D / Icv from logged terms; D detail on Plot 6)
  Plot 5 — Voltage loop firing health (stall diagnostic)
  Plot 6 — Voltage D term: voltage domain + current domain + gate domain (rise rate vs live gate & arm window)

State strip (below each plot), top to bottom:
  - binding-cap track (capReason): which layer actually set the current ceiling each tick
    (blue=G1 KHard, orange=G2 KHard, purple=iExcess, red=loadDump, teal=iExcessBulk;
    blank=unclamped) — answers "is KHard doing anything?". Console also prints the
    per-reason sample share at load time.
  - mode bar (brown=COMMISSIONING stage, green=CV active, grey=CV off)
  - Tick marks: voltLoopFired (grey)
  - Duration bars: recovActive (blue), fastOvActive (cyan; FastOV or iExcess, load dump
    separate), hardClamp (brown), iExcess (purple), loadDumpActive (red)

File picker searches ~/Downloads for *.csv, newest first.
PNGs saved to Downloads alongside source CSV.
"""

import glob
import os
import tkinter as tk
from tkinter import messagebox
from filepicker import pick_file
from plotlayout import tile_figures, enable_pan, block_pan_on

import matplotlib
matplotlib.use("TkAgg")
import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec
from matplotlib.widgets import CheckButtons, TextBox, Button as MplButton
from matplotlib.lines import Line2D
import pandas as pd
import numpy as np

DOWNLOADS = os.path.expanduser("~/Downloads")

plt.rcParams.update({
    "font.size": 14,
    "axes.titlesize": 18,
    "axes.labelsize": 15,
    "xtick.labelsize": 13,
    "ytick.labelsize": 13,
    "legend.fontsize": 12,
    "figure.titlesize": 16,
    "legend.handlelength": 3.5,
    "legend.handleheight": 1.5,
})

# ===== Load and parse =====
path = pick_file(prefix="cvlog_", title="Select CV Log")
if not path:
    raise SystemExit("No file selected.")

basename = os.path.splitext(os.path.basename(path))[0]
print(f"Loading: {path}")

# Header = first line holding battV and a time column (ts_ms tab-separated, or legacy t_s comma-separated).
with open(path, encoding="utf-8", errors="replace") as _f:
    _lines = _f.readlines()

_header_idx  = None
_header_line = None
for _i, _raw in enumerate(_lines):
    if "battV" in _raw and ("ts_ms" in _raw or "t_s" in _raw):
        _header_idx  = _i
        _header_line = _raw.strip()
        break

if _header_idx is None:
    raise SystemExit(
        f"ERROR: No line containing a time column and 'battV' found in {path}.\n"
        "Check that the CVlog header was written correctly by the firmware."
    )

_sep = "\t" if "\t" in _header_line else ","
_col_names = [c.strip() for c in _header_line.split(_sep)]

# Some firmware omits the newline before the header, so the first field arrives as "# cv_I: ... ints_ms".
if _col_names:
    if _col_names[0] != "ts_ms" and "ts_ms" in _col_names[0]:
        _col_names[0] = "ts_ms"
    elif _col_names[0] != "t_s" and _col_names[0].endswith("t_s"):
        _col_names[0] = "t_s"

print(f"Header found at file line {_header_idx} (sep={'TAB' if _sep == chr(9) else 'comma'}): {_header_line[:80]}...")
print(f"Columns ({len(_col_names)}): {_col_names}")

from io import StringIO
_data_text = "".join(_lines[_header_idx + 1:])
df = pd.read_csv(StringIO(_data_text), names=_col_names, sep=_sep, on_bad_lines="skip")

# ===== Rename log columns to the names used below =====
_rename_map = {
    "ts_ms":                   "ts_ms_raw",
    "ChargingVoltageTarget":   "targV",
    "vError":                  "vError_V",
    "Icv":                     "Icv_A",
    "cv_I":                    "cv_I_A",
    "tableThermalLimit":       "uTarget_A",
    "setpointCmd":             "spLimited_A",
    "measAmps":                "iMeas_A",
    "dutyApplied":             "duty_pct",
    "voltageLoopRanThisTick":  "voltLoopFired",
    "dBcur_dt":                "dBcur_dt_Aps",
    "battI":                   "battI_A",
}
df.rename(columns={k: v for k, v in _rename_map.items() if k in df.columns}, inplace=True)

if "ts_ms_raw" in df.columns:
    df["t_s"] = pd.to_numeric(df["ts_ms_raw"], errors="coerce") / 1000.0
elif "t_s" not in df.columns:
    raise SystemExit(f"ERROR: no time column found in parsed columns: {list(df.columns)}")

if "ovFlags" in df.columns:
    _ov = pd.to_numeric(df["ovFlags"], errors="coerce").fillna(0).astype("int64")
    df["fastOvActive"]   = (_ov & 1)
    df["iExcessBulk"]    = ((_ov & 2) > 0).astype("int64")  # bit 1 — iExcess BULK sub-mode (current-control phase)
    df["hardClamp"]      = ((_ov & 4) > 0).astype("int64")
    df["iExcess"]        = ((_ov & 8) > 0).astype("int64")
    df["loadDumpActive"] = ((_ov & 16) > 0).astype("int64")

# flags: 0x02 = CV loop running; 0x20 = inner loop regulating battery current battI_A instead of iMeas_A (§G)
if "flags" in df.columns:
    _fl = pd.to_numeric(df["flags"], errors="coerce").fillna(0).astype("int64")
    if "cvActive" not in df.columns:
        df["cvActive"] = ((_fl & 2) > 0).astype("int64")
    df["cvBattActive"] = ((_fl & 32) > 0).astype("int64")

if "t_s" not in df.columns:
    raise SystemExit(f"ERROR: 't_s' not in parsed columns: {list(df.columns)}")

numeric_cols = [
    "t_s",
    "battV", "targV", "vError_V", "dvdt_Vs", "vPred",
    "fastOvCap_A", "cv_I_A", "Icv_A", "uTarget_A", "spLimited_A",
    "iMeas_A", "duty_pct",
    "fastOvActive", "voltLoopFired", "cvActive",
    "hardClamp",
    "rpm",
    "battV_filt_V",
    "ch1_last_ms", "iExcess", "iExcessBulk",
    "battI_A", "dBcur_dt_Aps", "loadDumpActive",
    "cvDSlope_Vps", "awState",
    "voltLoopInterval_ms", "inaInterval_ms",
    "kdTrim_A",
    "capReason",
    "ovFilt_V",
    "recovActive",
    "targSlewed_V",
    "pTerm_A", "cvKdFiltV_V",
    "kdArmed", "kdActive", "kdDeadband_Vps", "kdArmAboveV",
    "huntDerate", "huntFreq_Hz", "huntState",
]

for col in numeric_cols:
    if col in df.columns:
        df[col] = pd.to_numeric(df[col], errors="coerce")

df.dropna(subset=["t_s"], inplace=True)
df.reset_index(drop=True, inplace=True)

df["t_s"] = df["t_s"] - df["t_s"].iloc[0]   # zero-reference

total_time_s = df["t_s"].iloc[-1]

if total_time_s > 7200:
    df["t_plot"] = df["t_s"] / 3600.0
    time_label = "Time (hours)"
elif total_time_s > 120:
    df["t_plot"] = df["t_s"] / 60.0
    time_label = "Time (minutes)"
else:
    df["t_plot"] = df["t_s"]
    time_label = "Time (seconds)"

def _to_int(col):
    if col not in df.columns:
        return pd.Series(0, index=df.index)
    return pd.to_numeric(df[col], errors="coerce").fillna(0).astype(int)

df["fastOvActive"]   = _to_int("fastOvActive")
df["voltLoopFired"]  = _to_int("voltLoopFired")
df["cvActive"]       = _to_int("cvActive")
df["hardClamp"]      = _to_int("hardClamp")
df["iExcess"]        = _to_int("iExcess")
df["iExcessBulk"]    = _to_int("iExcessBulk")   # Group 3 BULK sub-mode (current-control phase); older logs lack it → all 0
df["loadDumpActive"] = _to_int("loadDumpActive")
df["recovActive"]    = _to_int("recovActive")   # post-protection recovery window (flags b7; blank/0 on pre-ovFilt logs)
df["capReason"]      = _to_int("capReason")   # 0=none 1=KHard_G1 2=KHard_G2 3=iExcess 4=loadDump 5=iExcessBulk (binding cap; older logs lack it → all 0)
df["chargeStageDisplay"] = _to_int("chargeStageDisplay")   # 8 = COMMISSIONING (older logs lack it → all 0, band never drawn)

# capReason = the protection layer that was the binding current cap each tick (1/2 = KHard).
CAP_REASON_LABELS = {0: "none", 1: "KHard_G1", 2: "KHard_G2", 3: "iExcess", 4: "loadDump", 5: "iExcessBulk"}
CAP_REASON_COLORS = {0: "#cccccc", 1: "#1f77b4", 2: "#ff7f0e", 3: "#9467bd", 4: "#d62728", 5: "#26a69a"}  # G1 blue / G2 orange / iExc purple / LD red / iExcBulk teal
CAP_REASON_SHORT  = {1: "G1", 2: "G2", 3: "iExc", 4: "LD", 5: "iExcB"}  # compact on-plot event tags
_have_capreason = "capReason" in _col_names  # present only in newer CV-binary logs
if _have_capreason:
    _n = len(df)
    print("Binding cap reason (share of all samples):")
    for _code in (0, 1, 2, 3, 4, 5):
        _pct = 100.0 * (df["capReason"] == _code).sum() / _n if _n else 0.0
        print(f"    {CAP_REASON_LABELS[_code]:<9} {_pct:5.1f}%")
    _khard_share = 100.0 * df["capReason"].isin([1, 2]).sum() / _n if _n else 0.0
    print(f"  KHard was the binding cap on {_khard_share:.1f}% of samples "
          f"({'meaningful' if _khard_share >= 1.0 else 'negligible — other layers dominate'}).")
else:
    print("Binding cap reason: column 'capReason' not in this log (older firmware) — skipping.")

# Voltage loop Kp/Ki from the per-row voltageKp/Ki columns (Kd comes from the header, below). Old logs'
# gainKp/Ki/Kd are the INNER current-loop gains: never use them as voltage loop gains.
import re
_kp = float("nan")
_ki = float("nan")
if "voltageKp" in df.columns:
    _v = pd.to_numeric(df["voltageKp"], errors="coerce").dropna()
    if len(_v): _kp = float(_v.iloc[0])
if "voltageKi" in df.columns:
    _v = pd.to_numeric(df["voltageKi"], errors="coerce").dropna()
    if len(_v): _ki = float(_v.iloc[0])
# Header VoltageKp= fallback; no firmware has emitted it
if np.isnan(_kp):
    for _raw in _lines[:_header_idx]:
        if "VoltageKp" in _raw:
            m = re.search(r"VoltageKp=([\d.]+)", _raw)
            if m: _kp = float(m.group(1))
            m = re.search(r"VoltageKi=([\d.]+)", _raw)
            if m: _ki = float(m.group(1))
            break
if np.isnan(_kp) and ("gainKp" in df.columns or "innerKp" in df.columns):
    print("WARNING: old log format — voltage loop gains unavailable. "
          "gainKp/Ki/Kd (if present) are inner current-loop gains, not VoltageKp/Ki/Kd.")

outer_label = f"VoltageKp={_kp:.4g}  VoltageKi={_ki:.4g}"
print(f"Voltage loop gains: {outer_label}")

# CV D-term constants from the cvBinToCsv header line, matched by KEY. Old logs carry only "VoltageKd=",
# the TYPED knob (in Auto mode not the gain that ran), so VoltageKdActive wins when present.
_brk_thresh    = float("nan")   # V/s — deadband line BASE (b of clamp(floor, b + m·I, ceil))
_brk_k         = float("nan")   # A/(V/s) — D gain; ACTIVE gain on new logs, typed knob on old ones
_brk_k_typed   = False          # True when _brk_k came from the old typed-knob key (Auto logs: distrust it)
_brk_armv      = float("nan")   # V — max distance under target for the D term to act (0 = always)
_brk_onesided  = float("nan")   # 1 = removes current only; 0 = two-sided (adds below target)
_brk_voltfilt  = float("nan")   # ms — CvKdVoltFiltTC, the dedicated voltage EMA the D term differentiates
_brk_dbslope   = float("nan")   # V/s per A — deadband line slope m, evaluated at spLimited_A
_brk_dbfloor   = float("nan")   # V/s — deadband line clamp floor
_brk_dbceil    = float("nan")   # V/s — deadband line clamp ceiling
_brk_slopeceil = float("nan")   # V/s — slope ceiling; cvDSlope clamps to ±this
_brk_maxtrim   = float("nan")   # A — flat trim cap
_brk_excess    = float("nan")   # 1 = trim = Kd×(slope − band) continuous; 0 = legacy Kd×slope latch
_brk_fallrate  = float("nan")   # A/s — CvBrakeFallRate: brake-tier setpoint fall rate while D-term removes current
for _raw in _lines[:_header_idx]:
    if "CvKdDeadbandVps" not in _raw and "VoltageKd" not in _raw:
        continue
    m = re.search(r"CvKdDeadbandVps=([\d.]+)", _raw)
    if m: _brk_thresh = float(m.group(1))
    m = re.search(r"VoltageKdActive=([\d.]+)", _raw)
    if m:
        _brk_k = float(m.group(1))
    else:
        m = re.search(r"VoltageKd=([\d.]+)", _raw)
        if m: _brk_k = float(m.group(1)); _brk_k_typed = True
    m = re.search(r"CvKdArmV=([\d.]+)", _raw)
    if m: _brk_armv = float(m.group(1))
    m = re.search(r"CvKdOneSided=([\d.]+)", _raw)
    if m: _brk_onesided = float(m.group(1))
    m = re.search(r"CvKdVoltFiltTC=([\d.]+)", _raw)
    if m: _brk_voltfilt = float(m.group(1))
    m = re.search(r"CvKdDbSlope=([\d.]+)", _raw)
    if m: _brk_dbslope = float(m.group(1))
    m = re.search(r"CvKdDbFloor=([\d.]+)", _raw)
    if m: _brk_dbfloor = float(m.group(1))
    m = re.search(r"CvKdDbCeil=([\d.]+)", _raw)
    if m: _brk_dbceil = float(m.group(1))
    m = re.search(r"CvKdSlopeCeil=([\d.]+)", _raw)
    if m: _brk_slopeceil = float(m.group(1))
    m = re.search(r"CvKdMaxTrimA=([\d.]+)", _raw)
    if m: _brk_maxtrim = float(m.group(1))
    m = re.search(r"CvKdExcessMode=([\d.]+)", _raw)
    if m: _brk_excess = float(m.group(1))
    m = re.search(r"CvBrakeFallRate=([\d.]+)", _raw)
    if m: _brk_fallrate = float(m.group(1))
    break
_brk_label = (
    (f"VoltageKd(TYPED — Auto logs ran Td·Kp, not this)={_brk_k:.4g}A/(V/s)  " if _brk_k_typed
     else f"VoltageKdActive={_brk_k:.4g}A/(V/s)  ")
    + f"CvKdDeadbandVps={_brk_thresh:.3g}V/s  "
    + (f"CvKdDbSlope={_brk_dbslope:.4g}V/s/A  floor/ceil={_brk_dbfloor:.3g}/{_brk_dbceil:.3g}V/s  "
       if not np.isnan(_brk_dbslope) else "")
    + f"CvKdArmV={_brk_armv:.3g}V  "
    + f"CvKdOneSided={'1' if _brk_onesided == 1 else '0' if _brk_onesided == 0 else '?'}"
    + (f"  CvKdExcessMode={'EXCESS-over-line' if _brk_excess == 1 else 'LEGACY full-slope latch'}"
       if not np.isnan(_brk_excess) else "")
    + (f"  CvKdSlopeCeil={_brk_slopeceil:.4g}V/s" if not np.isnan(_brk_slopeceil) else "")
    + (f"  CvKdMaxTrimA={_brk_maxtrim:.4g}A" if not np.isnan(_brk_maxtrim) else "")
    + (f"  CvKdVoltFiltTC={_brk_voltfilt:.4g}ms" if not np.isnan(_brk_voltfilt) else "")
    + (f"  CvBrakeFallRate={_brk_fallrate:.4g}A/s" if not np.isnan(_brk_fallrate) else "")
    if not np.isnan(_brk_thresh) else "CV D-term params not in header (older log)"
)
print(f"CV D-term params: {_brk_label}")

# Trim-law self-check on rows where a trim landed: expected = Kd×(slope − band) in excess mode, Kd×slope
# in legacy mode. Ratio ≈1 = healthy; otherwise the header params differ from what ran.
def _kd_law_check(_df):
    if _brk_k_typed or np.isnan(_brk_k) or np.isnan(_brk_thresh): return
    need = ("kdTrim_A", "cvDSlope_Vps", "kdDeadband_Vps")
    if not all(c in _df.columns for c in need): return
    _rows = _df[(_df["kdTrim_A"] != 0) & (_df["kdDeadband_Vps"] > 0)]
    if len(_rows) < 3: return
    _band = _rows["kdDeadband_Vps"]
    _slp = _rows["cvDSlope_Vps"]
    if _brk_excess == 1:
        _exp = np.where(_slp > _band, _slp - _band, np.where(_slp < -_band, _slp + _band, 0.0)) * _brk_k
    else:
        _exp = _slp * _brk_k
    if not np.isnan(_brk_maxtrim):
        _exp = np.clip(_exp, -_brk_maxtrim, _brk_maxtrim)
    _mask = np.abs(_exp) > 0.05
    if _mask.sum() < 3: return
    _ratio = (_rows["kdTrim_A"].values[_mask] / _exp[_mask])
    print(f"D-term law check: kdTrim / header-equation ratio median {np.median(_ratio):.2f} "
          f"(p10 {np.percentile(_ratio, 10):.2f}, p90 {np.percentile(_ratio, 90):.2f}) over {_mask.sum()} trim rows"
          + ("" if 0.8 <= np.median(_ratio) <= 1.25 else "  *** MISMATCH — header params or law differ from what ran ***"))
_kd_law_check(df)

# Loop-shape toggles (cvBinToCsv "# Toggles:" line). cvHelpersEnabled=0 means the D term never ran at
# all — without this a reader cannot tell "never triggered" from "switched off".
_helpers_en = None
_risegov_en = None
for _raw in _lines[:_header_idx]:
    if "cvHelpersEnabled" in _raw:
        m = re.search(r"cvHelpersEnabled=(\d+)", _raw)
        if m: _helpers_en = int(m.group(1))
        m = re.search(r"cvRiseGovEnable=(\d+)", _raw)
        if m: _risegov_en = int(m.group(1))
        break
if _helpers_en is not None:
    print(f"Toggles: cvHelpersEnabled={_helpers_en} cvRiseGovEnable={_risegov_en}"
          + ("   *** HELPERS OFF — CV D term did not run ***" if _helpers_en == 0 else ""))

# kdTrim_A != 0 is ground truth that the D term acted; do NOT re-derive it from the gate formula (its inputs
# can be against you in any log). Hold it until the next VL tick: the gate runs at 10 Hz, the log at ~100 Hz.
# Same derivation as cvBinToCsv's kdActive, for CSVs predating that column.
if "kdActive" not in df.columns:
    if all(c in df.columns for c in ("kdTrim_A", "voltLoopFired")):
        _held, _vals = 0, []
        for _kt, _vl in zip(df["kdTrim_A"], df["voltLoopFired"]):
            if _kt != 0:  _held = 1
            elif _vl:     _held = 0
            _vals.append(_held)
        df["kdActive"] = _vals
    else:
        df["kdActive"] = 0

# Arm floor = the voltage above which the distance half of the gate opens. Prefer the exported column
# (already computed off targSlewed), then targSlewed, then targV — the last is exact only while the rise
# governor is not clamping a rising target. CvKdArmV=0 means always-armed (no floor line).
if "kdArmAboveV" in df.columns and df["kdArmAboveV"].notna().any() and (df["kdArmAboveV"] > -90).any():
    df["armFloor_V"] = df["kdArmAboveV"].where(df["kdArmAboveV"] > -90)
elif not np.isnan(_brk_armv) and _brk_armv > 0:
    _arm_ref = "targSlewed_V" if ("targSlewed_V" in df.columns and df["targSlewed_V"].notna().any()) else "targV"
    df["armFloor_V"] = df[_arm_ref] - _brk_armv

# The setpoint the CV PI actually tracks. targV is the RAW target and differs whenever the rise
# governor clamps; vError_V is logged as (targV − battV), which is NOT the PI's error.
_targ_pi = "targSlewed_V" if ("targSlewed_V" in df.columns and df["targSlewed_V"].notna().any()) else "targV"
_targ_pi_exact = (_targ_pi == "targSlewed_V")

# ===== Shared drawing helpers =====
GRID_KW    = dict(alpha=0.4, linewidth=0.7)
DUTY_COLOR = "#78909c"   # field duty % line — neutral grey-blue on all plots

EV_COLOR_VLOOP = "#757575"   # voltLoopFired ticks  (neutral grey)
EV_COLOR_HARD  = "#6a1b9a"   # hardClamp ticks      (purple)
EV_COLOR_FAST  = "#9467bd"   # iExcess ticks        (purple, matches capReason iExc)
EV_COLOR_LDUMP = "#f57c00"   # loadDumpActive ticks (orange)

_checkbox_refs = []  # keep CheckButtons alive — GC drops them without this

def _make_checkbox_panel(fig, lines):
    """Add a CheckButtons panel on the right margin to toggle line visibility."""
    lines = [l for l in lines if not l.get_label().startswith("_")]
    if not lines:
        return None
    labels = [l.get_label() for l in lines]
    n = len(labels)
    panel_h = min(0.85, max(0.12, n * 0.10))
    y0 = max(0.05, 0.52 - panel_h / 2)
    ax_cb = fig.add_axes([0.82, y0, 0.16, panel_h])
    ax_cb.set_frame_on(False)
    ax_cb.set_navigate(False)   # widget axes: never pan or rubber-band zoom it
    check = CheckButtons(ax_cb, labels, [True] * n)
    for lbl_obj, line in zip(check.labels, lines):
        lbl_obj.set_color(line.get_color())
        lbl_obj.set_fontsize(8)
    def toggle(label):
        for line in lines:
            if line.get_label() == label:
                line.set_visible(not line.get_visible())
        fig.canvas.draw_idle()
    check.on_clicked(toggle)
    _checkbox_refs.append(check)
    return check


def draw_state_strip(ax, df):
    ax._pan_x_only = True   # y here is bar layout, not data
    ax.set_ylim(0, 1)
    ax.set_yticks([])
    ax.set_xlim(df["t_plot"].iloc[0], df["t_plot"].iloc[-1])
    ax.set_xlabel(time_label, fontsize=13)

    span = df["t_plot"].iloc[-1] - df["t_plot"].iloc[0]

    prev_t   = df["t_plot"].iloc[0]
    prev_val = df["cvActive"].iloc[0]
    for i in range(1, len(df)):
        val = df["cvActive"].iloc[i]
        if val != prev_val or i == len(df) - 1:
            t     = df["t_plot"].iloc[i]
            color = "#2e7d32" if prev_val else "#cccccc"
            width = t - prev_t
            ax.barh(0.5, width, left=prev_t, height=0.82,
                    color=color, alpha=0.85, align="center")
            if prev_val and width > span * 0.04:
                ax.text(prev_t + width / 2, 0.5, "CV",
                        ha="center", va="center",
                        fontsize=8, color="white", fontweight="bold", clip_on=True)
            prev_t  = t
            prev_val = val

    for t in df.loc[df["voltLoopFired"] == 1, "t_plot"]:
        ax.axvline(x=t, ymin=0.09, ymax=0.91, color=EV_COLOR_VLOOP, linewidth=1.0, alpha=0.9)

    ax.text(df["t_plot"].iloc[0] + span * 0.005, 0.88, "cvActive", fontsize=7, color="#1b5e20", va="center")
    ax.text(df["t_plot"].iloc[0] + span * 0.005, 0.12, "VLoop▲",   fontsize=7, color=EV_COLOR_VLOOP, va="center")

    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.spines["left"].set_visible(False)


def add_ov_shading(ax, df):
    """Light red background fill where fastOvActive=1."""
    in_ov    = False
    ov_start = None
    for i, row in df.iterrows():
        if row["fastOvActive"] and not in_ov:
            ov_start = row["t_plot"]
            in_ov    = True
        elif not row["fastOvActive"] and in_ov:
            ax.axvspan(ov_start, row["t_plot"], color="#c62828", alpha=0.08)
            in_ov = False
    if in_ov:
        ax.axvspan(ov_start, df["t_plot"].iloc[-1], color="#c62828", alpha=0.08)


def add_cvbatt_shading(ax, df):
    """Light amber background where cvBattActive=1 (§G): the CV loop is regulating BATTERY current,
    so the PID command (spLimited_A) is tracked against battI_A, NOT iMeas_A (alternator). Outside the
    bands the loop is on alternator current as before."""
    if "cvBattActive" not in df.columns:
        return
    in_b, b_start = False, None
    for _, row in df.iterrows():
        if row["cvBattActive"] and not in_b:
            b_start, in_b = row["t_plot"], True
        elif not row["cvBattActive"] and in_b:
            ax.axvspan(b_start, row["t_plot"], color="#f9a825", alpha=0.07)
            in_b = False
    if in_b:
        ax.axvspan(b_start, df["t_plot"].iloc[-1], color="#f9a825", alpha=0.07)


KD_COLOR = "#00838f"   # teal — CV D term; distinct from loadDump orange and hardClamp purple


def _bool_spans(t, mask, bridge_s=0.0):
    """Contiguous (t_start, t_end) spans where mask is True. Samples merge when index-adjacent, or
    when the time gap between consecutive True samples is ≤ bridge_s. The bridge stitches legacy
    VL-tick logs (trim landed at 10 Hz) into a solid band, but never EXTENDS past the last True
    sample — so a bridged band still ends exactly where the signal does (no trailing overhang)."""
    idx = np.where(mask)[0]
    if len(idx) == 0:
        return []
    spans = []
    start_i = prev_i = idx[0]
    for k in idx[1:]:
        if k == prev_i + 1 or (t[k] - t[prev_i]) <= bridge_s:
            prev_i = k
        else:
            spans.append((t[start_i], t[prev_i]))
            start_i = prev_i = k
    spans.append((t[start_i], t[prev_i]))
    return spans


def add_kd_shading(ax, df, label=False):
    """Teal background where the CV D term ACTUALLY removed current (kdTrim_A != 0), sub-VL-interval
    gaps bridged. This is the true trim extent: it ends the instant D releases. The firmware kdActive
    latch is NOT used for the band — it stays set ~1 voltage-loop interval past the last trim, which
    paints a misleading trailing tail. Falls back to kdActive only on logs without kdTrim_A."""
    t = df["t_plot"].values
    if "kdTrim_A" in df.columns and (df["kdTrim_A"].values != 0).any():
        spans = _bool_spans(t, df["kdTrim_A"].values != 0, bridge_s=0.12)   # > one 100 ms VL interval
    elif "kdActive" in df.columns:
        spans = _bool_spans(t, df["kdActive"].values.astype(bool))
    else:
        return
    first = True
    for a, b in spans:
        ax.axvspan(a, b, color=KD_COLOR, alpha=0.16,
                   label="CV D term trimming" if (label and first) else None)
        first = False


COMMISSIONING_COLOR = "#795548"   # brown — matches plot_pidlog / plot_thermallog commissioning mode


def add_voltloop_vlines(ax, df):
    """Faint grey vlines where voltage loop fired."""
    for t in df.loc[df["voltLoopFired"] == 1, "t_plot"]:
        ax.axvline(x=t, color=EV_COLOR_VLOOP, linewidth=0.6, alpha=0.35)


def add_duty_axis(ax):
    """Add field duty % as a secondary right axis. Returns the twin axis."""
    ax_d = ax.twinx()
    if "duty_pct" in df.columns:
        ax_d.plot(df["t_plot"], df["duty_pct"],
                  color=DUTY_COLOR, lw=1.3, alpha=0.55, linestyle="-.",
                  label="Field duty (%)")
    ax_d.set_ylabel("Field duty (%)", color=DUTY_COLOR, fontsize=12)
    ax_d.set_ylim(0, 115)
    ax_d.tick_params(axis="y", colors=DUTY_COLOR, labelsize=11)
    return ax_d


def add_subtitle(fig, text, y=0.955):
    """Small italic subtitle below suptitle."""
    fig.text(0.5, y, text, ha="center", va="top",
             fontsize=9.5, color="#555555", style="italic")


def save_fig(fig, suffix):
    out = os.path.join(DOWNLOADS, f"{basename}_{suffix}.png")
    fig.savefig(out, dpi=150, bbox_inches="tight")
    print(f"Saved: {out}")


def draw_flag_bars(ax, df):
    """Mode bar + duration bars + VLoop ticks — replaces state strip on all plots."""
    ax._pan_x_only = True   # y here is row layout, not data — dragging it vertically only hides rows
    flag_h  = 0.18
    spacing = 0.26
    n_rows  = 8   # capReason + cvActive + voltLoopFired + recovery + 4 protection flags
    top     = (n_rows - 1) * spacing + flag_h + 0.06

    span = df["t_plot"].iloc[-1] - df["t_plot"].iloc[0]

    # --- Row 7 (top): capReason — which layer was the BINDING cap (colored segments) ---
    cr_offset = 7 * spacing
    if "capReason" in df.columns:
        cr_vals = df["capReason"].values
        t_arr   = df["t_plot"].values
        seg_start = 0
        episodes  = []        # [t_start, [codes in order], t_end] — merged protection bursts
        EPISODE_GAP_S = 0.20  # bursts closer than this merge into one labelled episode
        for i in range(1, len(cr_vals) + 1):
            last = (i == len(cr_vals))
            if last or cr_vals[i] != cr_vals[seg_start]:
                code = int(cr_vals[seg_start])
                if code != 0:  # skip "none" — leave the row blank when nothing bound
                    seg_end = t_arr[-1] if last else t_arr[i]  # transition time = right edge (matches other bars)
                    ax.barh(cr_offset + flag_h / 2, seg_end - t_arr[seg_start],
                            left=t_arr[seg_start], height=flag_h,
                            color=CAP_REASON_COLORS.get(code, "#000000"), alpha=0.85, align="center")
                    # accumulate into an episode (merge tightly-spaced bursts so one event = one label)
                    if episodes and (t_arr[seg_start] - episodes[-1][2]) < EPISODE_GAP_S:
                        if code not in episodes[-1][1]:
                            episodes[-1][1].append(code)
                        episodes[-1][2] = seg_end
                    else:
                        episodes.append([t_arr[seg_start], [code], seg_end])
                if not last:
                    seg_start = i
        for ep_t, codes, ep_end in episodes:
            tag    = ">".join(CAP_REASON_SHORT.get(c, str(c)) for c in codes)
            ep_mid = 0.5 * (ep_t + ep_end)
            tcol   = CAP_REASON_COLORS.get(codes[-1], "#000000")
            ax.plot([ep_mid, ep_mid], [cr_offset + flag_h, cr_offset + flag_h + 0.10],
                    color=tcol, lw=0.8, alpha=0.7, clip_on=False)
            ax.text(ep_mid, cr_offset + flag_h + 0.12, tag,
                    va="bottom", ha="center", fontsize=7, fontweight="bold",
                    color=tcol, clip_on=False)

    # --- Row 6: mode bar — brown=COMMISSIONING (stage 8), green=CV active, grey=CV off ---
    # COMMISSIONING overrides CV on/off in this row, as in plot_pidlog / plot_thermallog.
    cv_offset = 6 * spacing
    _cv  = df["cvActive"].values
    _stg = (df["chargeStageDisplay"].values if "chargeStageDisplay" in df.columns
            else np.zeros(len(df), dtype="int64"))
    def _mode_color(i):
        if _stg[i] == 8:
            return COMMISSIONING_COLOR
        return "#2e7d32" if _cv[i] else "#aaaaaa"
    prev_t   = df["t_plot"].iloc[0]
    prev_col = _mode_color(0)
    for i in range(1, len(df)):
        col = _mode_color(i)
        if col != prev_col or i == len(df) - 1:
            t     = df["t_plot"].iloc[i]
            width = t - prev_t
            ax.barh(cv_offset + flag_h / 2, width, left=prev_t, height=flag_h,
                    color=prev_col, alpha=0.85, align="center")
            prev_t   = t
            prev_col = col

    # --- Row 5: voltLoopFired — tick marks (fires every loop tick) ---
    vl_offset = 5 * spacing
    for t in df.loc[df["voltLoopFired"] == 1, "t_plot"]:
        ax.axvline(x=t, ymin=vl_offset / top, ymax=(vl_offset + flag_h) / top,
                   color=EV_COLOR_VLOOP, linewidth=0.8, alpha=0.7)

    # --- Rows 4–0: recovery window + protection duration bars ---
    bar_rows = [
        (4 * spacing, "recovActive",    "#2e96d1", "recovActive   (post-protection recovery window)"),
        (3 * spacing, "fastOvActive",   "#17becf", "fastOvActive  (FastOV or iExcess; load dump separate)"),
        (2 * spacing, "hardClamp",      "#8c564b", "hardClamp     (layer 2/3 — hard ceiling)"),
        (1 * spacing, "iExcess",        "#9467bd", "iExcess       (current excess protection)"),
        (0 * spacing, "loadDumpActive", "#d62728", "loadDumpActive (sudden load drop detected)"),
    ]
    for offset, col, color, label in bar_rows:
        vals = df[col].values
        t    = df["t_plot"].values
        in_flag    = False
        flag_start = None
        for i in range(len(vals)):
            if vals[i] and not in_flag:
                flag_start = t[i]
                in_flag    = True
            elif not vals[i] and in_flag:
                ax.barh(offset + flag_h / 2, t[i] - flag_start,
                        left=flag_start, height=flag_h,
                        color=color, alpha=0.80, align="center")
                in_flag = False
        if in_flag:
            ax.barh(offset + flag_h / 2, t[-1] - flag_start,
                    left=flag_start, height=flag_h,
                    color=color, alpha=0.80, align="center")

    ax.set_ylim(-0.03, top + 0.30)   # headroom for inline capReason episode tags
    # Row names in the left gutter (y-tick labels) — never painted over the data.
    row_centers = [k * spacing + flag_h / 2 for k in range(n_rows)]
    row_names   = ["loadDump", "iExcess", "hardClamp", "fastOV", "recovery",
                   "voltLoop", "mode", "binding cap"]   # rows 0..7, bottom -> top (row 6 = CV on/off + COMMISSIONING)
    ax.set_yticks(row_centers)
    ax.set_yticklabels(row_names, fontsize=7.5)
    ax.tick_params(axis="y", length=0, pad=2)

    # Floating, draggable colour key for the protection-event tags (drag it off the data).
    from matplotlib.patches import Patch
    _key = [
        Patch(color=CAP_REASON_COLORS[1], label="G1 KHard predictive"),
        Patch(color=CAP_REASON_COLORS[2], label="G2 KHard measured"),
        Patch(color=CAP_REASON_COLORS[3], label="iExcess (current excess)"),
        Patch(color=CAP_REASON_COLORS[4], label="LD load dump"),
        Patch(color="#17becf",            label="fastOvActive"),
        Patch(color="#2e96d1",            label="recovActive (recovery window)"),
        Patch(color="#8c564b",            label="hardClamp"),
        Patch(color="#2e7d32",            label="cvActive (CV on)"),
        Patch(color=EV_COLOR_VLOOP,       label="voltLoop tick"),
        Patch(color=COMMISSIONING_COLOR,  label="COMMISSIONING (mode)"),
    ]
    _evleg = ax.figure.legend(handles=_key, loc="upper right", bbox_to_anchor=(0.995, 0.995),
                              ncol=2, fontsize=7, framealpha=0.9, borderpad=0.4,
                              handlelength=1.3, columnspacing=1.2, title="Events key")
    _evleg.get_title().set_fontsize(7)
    _evleg.set_draggable(True)

    # Hover read-out: name the row (and nearest capReason tag) under the cursor.
    try:
        _annot = ax.annotate("", xy=(0, 0), xytext=(12, 14), textcoords="offset points",
                             fontsize=8, fontweight="bold", zorder=30,
                             bbox=dict(boxstyle="round,pad=0.3", fc="#ffffcc", ec="#888888", alpha=0.95))
        _annot.set_visible(False)
        _eps = episodes if "capReason" in df.columns else []
        def _hover(event, _ax=ax, _ann=_annot, _centers=row_centers, _names=row_names,
                   _eps=_eps, _sp=spacing, _ntop=n_rows - 1):
            if event.inaxes is not _ax or event.ydata is None:
                if _ann.get_visible():
                    _ann.set_visible(False); _ax.figure.canvas.draw_idle()
                return
            row = min(range(len(_centers)), key=lambda k: abs(_centers[k] - event.ydata))
            if abs(_centers[row] - event.ydata) > _sp * 0.6:
                if _ann.get_visible():
                    _ann.set_visible(False); _ax.figure.canvas.draw_idle()
                return
            txt = _names[row]
            if row == _ntop and _eps:
                near = min(_eps, key=lambda e: abs(0.5 * (e[0] + e[2]) - (event.xdata or 0)))
                if near[0] - _sp <= (event.xdata or -1e9) <= near[2] + _sp:
                    txt += ": " + ">".join(CAP_REASON_SHORT.get(c, str(c)) for c in near[1])
            _ann.xy = (event.xdata, event.ydata)
            _ann.set_text(txt); _ann.set_visible(True); _ax.figure.canvas.draw_idle()
        ax.figure.canvas.mpl_connect("motion_notify_event", _hover)
    except Exception:
        pass
    ax.set_xlim(df["t_plot"].iloc[0], df["t_plot"].iloc[-1])
    ax.set_xlabel(time_label, fontsize=13)
    ax.set_ylabel("Events", fontsize=9)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.spines["left"].set_visible(False)
    ax.grid(**GRID_KW)


# ===== PLOT 1 — Voltage =====
fig1 = plt.figure(figsize=(18, 8), num="Plot 1 — Voltage")
gs1  = gridspec.GridSpec(2, 1, height_ratios=[5, 0.9], hspace=0.08)
ax1  = fig1.add_subplot(gs1[0])
ax1s = fig1.add_subplot(gs1[1], sharex=ax1)
plt.setp(ax1.get_xticklabels(), visible=False)
fig1.suptitle(f"Plot 1 — Voltage  |  {outer_label}", fontsize=14, y=0.99)
add_subtitle(fig1,
    "Was the voltage setpoint reached? Did the regulator approach it cleanly, "
    "or did it overshoot? vPred shows how far ahead the loop was looking.")
fig1.subplots_adjust(top=0.90, right=0.80, bottom=0.10)

ax1.plot(df["t_plot"], df["battV"],
         color="#1565c0", lw=2.5, label="battV (measured)")
ax1.plot(df["t_plot"], df["battV_filt_V"],
         color="#90caf9", lw=1.6, linestyle="--", alpha=0.85, label="battV_filt_V (EMA)")
if "cvKdFiltV_V" in df.columns and df["cvKdFiltV_V"].notna().any():
    # The dedicated CvKdVoltFiltTC EMA the D term differentiates — distinct from battV_filt_V (VoltageFilterTC)
    ax1.plot(df["t_plot"], df["cvKdFiltV_V"],
             color="#8e24aa", lw=1.6, alpha=0.85, label="Voltage for D term  (cvKdFiltV_V)")
if "ovFilt_V" in df.columns and df["ovFilt_V"].notna().any():
    # Group 2's actual comparator input (plant-tau EMA) — the trip fires on THIS vs targV+OvMeasMarginV
    ax1.plot(df["t_plot"], df["ovFilt_V"],
             color="#6a1b9a", lw=1.8, alpha=0.90, label="ovFilt_V (G2 comparator input)")
ax1.plot(df["t_plot"], df["targV"],
         color="#e91e63", lw=2.2, linestyle="--", label="targV (setpoint)")
if "vPred" in df.columns:
    ax1.plot(df["t_plot"], df["vPred"],
             color="#00838f", lw=2.0, linestyle="-.", label="vPred (predicted)", alpha=0.85)
ax1.set_ylabel("Voltage (V)")
ax1.grid(**GRID_KW)
ax1_d = add_duty_axis(ax1)

_h1 = [l for l in ax1.get_lines() if not l.get_label().startswith("_")]
_h1_duty = [l for l in ax1_d.get_lines() if not l.get_label().startswith("_")]
_leg1 = ax1_d.legend(_h1 + _h1_duty, [l.get_label() for l in _h1 + _h1_duty], loc="upper left")
_leg1.set_draggable(True)
_cb1 = _make_checkbox_panel(fig1, _h1 + _h1_duty)

add_kd_shading(ax1, df)   # teal = D term removing current — read the wander/rise against where D braked
add_ov_shading(ax1, df)
add_voltloop_vlines(ax1, df)
draw_flag_bars(ax1s, df)
# save_fig(fig1, "plot1_voltage")


# ===== PLOT 2 — Current command chain (top) + protection layers (bottom) =====
fig2 = plt.figure(figsize=(18, 12), num="Plot 2 — Command Chain & Protections")
gs2  = gridspec.GridSpec(2, 1, height_ratios=[4, 0.9], hspace=0.12)
ax2a = fig2.add_subplot(gs2[0])
ax2b = fig2.add_subplot(gs2[1], sharex=ax2a)
plt.setp(ax2a.get_xticklabels(), visible=False)
fig2.suptitle(f"Plot 2 — Current Command Chain & Protections  |  {outer_label}", fontsize=14, y=0.99)
add_subtitle(fig2,
    "Top: which limit is dominating the current setpoint, and did the alternator deliver it?  "
    "Bottom: when each overvoltage protection layer fired and for how long.")
fig2.subplots_adjust(top=0.92, right=0.80)

# --- 2a: command chain ---
if "uTarget_A" in df.columns:
    ax2a.plot(df["t_plot"], df["uTarget_A"],
              color="#1565c0", lw=2.5, label="Current ceiling after all caps  (uTarget_A)")
if "Icv_A" in df.columns:
    ax2a.plot(df["t_plot"], df["Icv_A"],
              color="#e91e63", lw=2.2, linestyle="--", label="CV setpoint  (Icv_A)")
if "fastOvCap_A" in df.columns:
    ax2a.plot(df["t_plot"], df["fastOvCap_A"],
              color="#455a64", lw=1.6, linestyle=":", label="FastOV voltage ceiling  (fastOvCap_A)", alpha=0.85)
if "spLimited_A" in df.columns:
    ax2a.plot(df["t_plot"], df["spLimited_A"],
              color="#2e7d32", lw=2.0, label="PID command  (spLimited_A)")
if "iMeas_A" in df.columns:
    ax2a.plot(df["t_plot"], df["iMeas_A"],
              color="#c62828", lw=2.0, label="Actual current  (iMeas_A)", alpha=0.90)
if "battI_A" in df.columns:
    # In amber-shaded spans (cvBattActive) THIS is the regulated PV the loop tracks against spLimited_A.
    _batt_lbl = "Battery current  (battI_A — tracked PV in shaded spans)" if "cvBattActive" in df.columns else "Battery current  (battI_A)"
    ax2a.plot(df["t_plot"], df["battI_A"],
              color="#f9a825", lw=1.8, alpha=0.80, label=_batt_lbl)

ax2a.set_ylabel("Current (A)")
ax2a.grid(**GRID_KW)
ax2a.set_title("Command chain — which limit is lowest wins; field duty on right axis",
               fontsize=10, color="#444444", style="italic", pad=4)
ax2a_d = add_duty_axis(ax2a)

_h2a = [l for l in ax2a.get_lines() if not l.get_label().startswith("_")]
_h2a_duty = [l for l in ax2a_d.get_lines() if not l.get_label().startswith("_")]
_leg2a = ax2a_d.legend(_h2a + _h2a_duty, [l.get_label() for l in _h2a + _h2a_duty], loc="upper left", fontsize=11)
_leg2a.set_draggable(True)

add_kd_shading(ax2a, df)   # teal = D term trimming Icv down — ties a dip in the command chain to D
add_ov_shading(ax2a, df)
add_cvbatt_shading(ax2a, df)   # §G: amber band = CV regulating battery current (track spLimited vs battI)
add_voltloop_vlines(ax2a, df)

_p2_key = (
    "  Current ceiling/all caps =  uTarget_A      (A)\n"
    "  CV setpoint              =  Icv_A          (A)\n"
    "  FastOV voltage ceiling   =  fastOvCap_A    (A)\n"
    "  PID command              =  spLimited_A    (A)\n"
    "  Actual current (raw)     =  iMeas_A        (A)\n"
    "  Battery current          =  battI_A        (A)\n"
    "  Field duty               =  duty_pct       (%)\n"
    "  Amber band = CV regulating BATTERY current (§G):\n"
    "    track spLimited_A vs battI_A there, not iMeas_A"
)
_key_text = fig2.text(
    0.01, 0.30, _p2_key,
    fontsize=8, family="monospace",
    va="top", ha="left",
    bbox=dict(boxstyle="round,pad=0.35", fc="#f9f9f9", ec="#cccccc", alpha=0.92),
    zorder=10,
)

_key_drag = {"on": False, "x": 0.0, "y": 0.0, "pos": (0.01, 0.30)}

def _kpress(ev):
    if ev.button != 1:
        return
    try:
        bb = _key_text.get_window_extent(fig2.canvas.get_renderer())
    except Exception:
        return
    if bb.contains(ev.x, ev.y):
        _key_drag["on"] = True
        _key_drag["x"] = ev.x
        _key_drag["y"] = ev.y
        _key_drag["pos"] = _key_text.get_position()

def _kmove(ev):
    if not _key_drag["on"]:
        return
    fw = fig2.get_figwidth() * fig2.dpi
    fh = fig2.get_figheight() * fig2.dpi
    nx = _key_drag["pos"][0] + (ev.x - _key_drag["x"]) / fw
    ny = _key_drag["pos"][1] + (ev.y - _key_drag["y"]) / fh
    _key_text.set_position((nx, ny))
    fig2.canvas.draw_idle()

def _krelease(ev):
    _key_drag["on"] = False

block_pan_on(fig2, _key_text)   # the key box owns its own left-drag; don't pan under it

fig2.canvas.mpl_connect("button_press_event", _kpress)
fig2.canvas.mpl_connect("motion_notify_event", _kmove)
fig2.canvas.mpl_connect("button_release_event", _krelease)

# --- 2b: mode + protection events panel ---
draw_flag_bars(ax2b, df)

_cb2 = _make_checkbox_panel(fig2, _h2a + _h2a_duty)

# save_fig(fig2, "plot2_command_chain_protections")


# ===== PLOT 3 — Engine & scheduling context =====
fig3 = plt.figure(figsize=(18, 9), num="Plot 3 — RPM & Context")
gs3  = gridspec.GridSpec(3, 1, height_ratios=[2, 1.5, 0.9], hspace=0.10)
ax3  = fig3.add_subplot(gs3[0])
ax3b = fig3.add_subplot(gs3[1], sharex=ax3)
ax3s = fig3.add_subplot(gs3[2], sharex=ax3)
plt.setp(ax3.get_xticklabels(),  visible=False)
plt.setp(ax3b.get_xticklabels(), visible=False)
fig3.suptitle(f"Plot 3 — Engine & Scheduling Context  |  {outer_label}", fontsize=14, y=0.99)
add_subtitle(fig3,
    "RPM, ADC scheduling jitter, and field duty — the mechanical and timing backdrop "
    "for everything in Plots 1 and 2.")
fig3.subplots_adjust(top=0.90, right=0.80)

if "rpm" in df.columns:
    ax3.plot(df["t_plot"], df["rpm"],
             color="#f9a825", lw=2.2, label="RPM")
else:
    ax3.text(0.5, 0.5, "rpm not present in this log",
             ha="center", va="center", transform=ax3.transAxes,
             color="#888888", fontsize=13)
ax3.set_ylabel("RPM")
ax3.grid(**GRID_KW)
ax3_d = add_duty_axis(ax3)
_h3 = [l for l in ax3.get_lines() if not l.get_label().startswith("_")]
_h3_duty = [l for l in ax3_d.get_lines() if not l.get_label().startswith("_")]
_leg3 = ax3_d.legend(_h3 + _h3_duty, [l.get_label() for l in _h3 + _h3_duty], loc="upper left")
_leg3.set_draggable(True)
add_voltloop_vlines(ax3, df)

if "ch1_last_ms" in df.columns:
    ax3b.plot(df["t_plot"], df["ch1_last_ms"],
              color="#00838f", lw=1.8, label="ch1_last_ms (CH1)", zorder=2)
    ax3b.axhline(5,  color="#2e7d32", linewidth=0.8, linestyle=":", alpha=0.70, label="5 ms (CH1 nominal)")
    ax3b.axhline(15, color="#c62828", linewidth=0.8, linestyle=":", alpha=0.70,
                 label="15 ms (CH1 3× — stale reading risk)")
else:
    ax3b.text(0.5, 0.5, "ch1_last_ms not present in this log (older firmware)",
              ha="center", va="center", transform=ax3b.transAxes,
              color="#888888", fontsize=12)

if "voltLoopInterval_ms" in df.columns:
    vl_rows = df[df["voltLoopFired"] == 1].copy()
    if not vl_rows.empty:
        ax3b.scatter(vl_rows["t_plot"], vl_rows["voltLoopInterval_ms"],
                     s=18, color="#ef5350", zorder=4, label="voltLoop actual interval (fired ticks only)")
    ax3b.axhline(100, color="#ef5350", linewidth=0.8, linestyle=":",  alpha=0.70, label="100 ms (vLoop target)")
    ax3b.axhline(200, color="#b71c1c", linewidth=0.8, linestyle="--", alpha=0.70, label="200 ms (vLoop 2×)")

if "inaInterval_ms" in df.columns:
    ax3b.plot(df["t_plot"], df["inaInterval_ms"],
              color="#42a5f5", lw=1.0, alpha=0.8, label="inaInterval_ms (INA228)", zorder=1)
    ax3b.axhline(10, color="#1565c0", linewidth=0.8, linestyle=":", alpha=0.70, label="10 ms (INA228 2×)")

ax3b.set_ylabel("Intervals (ms)")
ax3b.grid(**GRID_KW)
_h3b = [l for l in ax3b.get_lines() if not l.get_label().startswith("_")]
_leg3b = ax3b.legend(loc="upper right", fontsize=10)
_leg3b.set_draggable(True)

_cb3 = _make_checkbox_panel(fig3, _h3 + _h3_duty + _h3b)

draw_flag_bars(ax3s, df)
# save_fig(fig3, "plot3_rpm_context")


# ===== PLOT 4 — CV PID term decomposition =====
# Icv_A = pTerm_A + cv_I_A − kdTrim_A before the [0, ceiling] clamp. Logs without pTerm_A/kdTrim_A fall
# back to a Kp×error P (no D).
fig4 = plt.figure(figsize=(18, 9), num="Plot 4 — PID Term Decomposition")
gs4  = gridspec.GridSpec(3, 1, height_ratios=[4, 1.5, 0.9], hspace=0.10)
ax4  = fig4.add_subplot(gs4[0])
ax4b = fig4.add_subplot(gs4[1], sharex=ax4)
ax4s = fig4.add_subplot(gs4[2], sharex=ax4)
plt.setp(ax4.get_xticklabels(),  visible=False)
plt.setp(ax4b.get_xticklabels(), visible=False)
fig4.suptitle(f"Plot 4 — CV PID Term Decomposition  |  {outer_label}", fontsize=14, y=0.99)
add_subtitle(fig4,
    "What each term of the voltage loop contributes to the current setpoint Icv. P reacts to error, "
    "I holds the running correction, D (−kdTrim_A) brakes on a rising voltage by removing current. "
    "Icv = P + I − D before the [0, ceiling] clamp; teal band = D actually trimming.\n"
    f"CV D term: {_brk_label}")
fig4.subplots_adjust(top=0.90, right=0.80)

# The Kp×error fallback uses targSlewed (what the PI tracks); off the raw target it is wrong wherever the
# rise governor clamped.
_have_pterm = "pTerm_A" in df.columns and df["pTerm_A"].notna().any()
if _have_pterm:
    df["pid_P"] = df["pTerm_A"]
    _p_lbl = "P term  =  pTerm_A  (logged, A)"
elif not np.isnan(_kp):
    df["pid_P"] = _kp * (df[_targ_pi] - df["battV"])
    _src = "targSlewed_V" if _targ_pi_exact else "targV approx"
    _p_lbl = f"P term  ≈  Kp({_kp:.4g}) × ({_src} − battV)  (reconstructed, A)"
else:
    df["pid_P"] = np.nan
    _p_lbl = "P term  (unavailable — no pTerm_A, no VoltageKp)"
    print("WARNING: neither pTerm_A nor VoltageKp available — P term cannot be shown")

ax4.plot(df["t_plot"], df["pid_P"], color="#1565c0", lw=2.0, label=_p_lbl)
if "cv_I_A" in df.columns:
    ax4.plot(df["t_plot"], df["cv_I_A"],
             color="#2e7d32", lw=2.0, label="I term  =  cv_I_A  (running integral, A)")
if "kdTrim_A" in df.columns:
    # D contribution to Icv is −kdTrim_A: kdTrim_A is signed (≥0 = removed on a rise; <0 = added on a below-target fall in two-sided mode).
    ax4.plot(df["t_plot"], -df["kdTrim_A"],
             color="#8e24aa", lw=2.0, label="D term  =  −kdTrim_A  (removes on a rise; adds below target, two-sided)")
if "Icv_A" in df.columns:
    ax4.plot(df["t_plot"], df["Icv_A"],
             color="#c62828", lw=2.2, linestyle="-.",
             label="Icv_A  (CV output = P + I − D, clamped)", alpha=0.90)

ax4.axhline(0, color="#999999", linewidth=0.7, linestyle=":", alpha=0.5)
ax4.set_ylabel("Current contribution (A)")
ax4.grid(**GRID_KW)
ax4_d = add_duty_axis(ax4)
_h4 = [l for l in ax4.get_lines() if not l.get_label().startswith("_")]
_h4_duty = [l for l in ax4_d.get_lines() if not l.get_label().startswith("_")]
_leg4 = ax4_d.legend(_h4 + _h4_duty, [l.get_label() for l in _h4 + _h4_duty], loc="upper left", fontsize=10)
_leg4.set_draggable(True)
add_kd_shading(ax4, df)
add_ov_shading(ax4, df)
add_voltloop_vlines(ax4, df)

# vError_V is logged as targV − battV, not the PI's error (targSlewed − battV, plotted when available).
df["filt_error_V"] = df[_targ_pi] - df["battV_filt_V"]
ax4b.plot(df["t_plot"], df["vError_V"],
          color="#212121", lw=1.8, alpha=0.90, label="vError_V  (logged: targV − battV)")
if _targ_pi_exact:
    df["pi_error_V"] = df["targSlewed_V"] - df["battV"]
    ax4b.plot(df["t_plot"], df["pi_error_V"],
              color="#c62828", lw=1.4, alpha=0.85,
              label="targSlewed_V − battV  (the error the PI actually runs on)")
ax4b.plot(df["t_plot"], df["filt_error_V"],
          color="#9e9e9e", lw=1.2, linestyle="--", alpha=0.70, label="targV − battV_filt_V  (display EMA, reference)")
ax4b.axhline(0, color="#999999", linewidth=0.7, linestyle=":", alpha=0.5)
ax4b.set_ylabel("vError (V)")

ax4b.grid(**GRID_KW)
_h4b = [l for l in ax4b.get_lines() if not l.get_label().startswith("_")]
_leg4b = ax4b.legend(_h4b, [l.get_label() for l in _h4b], loc="upper left", fontsize=10)
_leg4b.set_draggable(True)

_cb4 = _make_checkbox_panel(fig4, _h4 + _h4_duty + _h4b)

draw_flag_bars(ax4s, df)
# save_fig(fig4, "plot4_pid_decomposition")


# ===== PLOT 5 — Voltage loop firing health (stall diagnostic) =====
# Most OV protections sit downstream of the 100 ms CV loop, so a stall here stalls them too.
fig5 = plt.figure(figsize=(18, 8), num="Plot 5 — Voltage Loop Health")
gs5  = gridspec.GridSpec(2, 1, height_ratios=[3, 1.6], hspace=0.12)
ax5  = fig5.add_subplot(gs5[0])
ax5b = fig5.add_subplot(gs5[1], sharex=ax5)
plt.setp(ax5.get_xticklabels(), visible=False)
fig5.suptitle(f"Plot 5 — Voltage Loop Firing Health  |  {outer_label}", fontsize=14, y=0.99)
add_subtitle(fig5,
    "Top: ms since previous voltage loop fire. Should reset to ~0 every 100 ms. "
    "Long flat ramps = the loop stalled — P and I stayed frozen (the D term recomputes every output "
    "tick, so it keeps braking through a stall). "
    "Bottom: battV during the same window. Stall + rising RPM = uncontrolled overshoot.")
fig5.subplots_adjust(top=0.90, right=0.80)

_last_fire_t = None
_t_since = []
for _i, _row in df.iterrows():
    if _row["voltLoopFired"] == 1:
        _last_fire_t = _row["t_plot"]
    if _last_fire_t is None:
        _t_since.append(float("nan"))
    else:
        _t_since.append((_row["t_plot"] - _last_fire_t) * 1000.0)
df["t_since_vloop_ms"] = _t_since

_fires = df.loc[df["voltLoopFired"] == 1]
_last_fire_t_global = _fires["t_plot"].iloc[-1] if len(_fires) > 0 else None
_file_end_t = df["t_plot"].iloc[-1]

ax5.plot(df["t_plot"], df["t_since_vloop_ms"],
         color="#1565c0", lw=1.6, label="ms since previous voltage loop fire")

if len(_fires) > 0 and "voltLoopInterval_ms" in df.columns:
    _intervals = _fires["voltLoopInterval_ms"].astype(float)
    ax5.scatter(_fires["t_plot"], _intervals,
                s=30, color="#0d47a1", zorder=5, marker="o",
                label="Fire events (interval since previous fire)")

ax5.axhline(y=100, color="#2e7d32", linestyle="--", lw=1.0, alpha=0.65,
            label="Expected (100 ms)")
ax5.axhline(y=200, color="#f57c00", linestyle=":",  lw=1.0, alpha=0.65,
            label="2× expected (200 ms)")
ax5.axhline(y=300, color="#c62828", linestyle=":",  lw=1.0, alpha=0.65,
            label="3× expected (300 ms)")

if _last_fire_t_global is not None:
    _silence_ms = (_file_end_t - _last_fire_t_global) * 1000.0
    if _silence_ms > 200:
        ax5.axvspan(_last_fire_t_global, _file_end_t,
                    color="#c62828", alpha=0.12,
                    label=f"Silent zone — no fire for {_silence_ms:.0f} ms")

ax5.set_ylabel("ms since last voltage loop fire", fontsize=12)
ax5.grid(**GRID_KW)
_leg5 = ax5.legend(loc="upper left", fontsize=10)
_leg5.set_draggable(True)

ax5b.plot(df["t_plot"], df["battV"],
          color="#1565c0", lw=2.0, label="battV (measured)")
ax5b.plot(df["t_plot"], df["targV"],
          color="#e91e63", lw=1.6, linestyle="--", label="targV (setpoint)")
ax5b.set_xlabel(time_label, fontsize=13)
ax5b.set_ylabel("Voltage (V)", fontsize=12)
ax5b.grid(**GRID_KW)
ax5b.legend(loc="upper left", fontsize=10)
add_ov_shading(ax5b, df)

if len(_fires) > 1 and "voltLoopInterval_ms" in df.columns:
    _ivals = _fires["voltLoopInterval_ms"].astype(float)
    _worst = _ivals.max()
    _over_200 = int((_ivals > 200).sum())
    _over_300 = int((_ivals > 300).sum())
    print(f"Voltage loop: {len(_fires)} fires | worst interval = {_worst:.0f} ms | "
          f"{_over_200} fires with >200 ms gap | {_over_300} with >300 ms gap")
    if _last_fire_t_global is not None:
        _silence_ms = (_file_end_t - _last_fire_t_global) * 1000.0
        if _silence_ms > 200:
            print(f"  TRAILING SILENCE: last fire at t={_last_fire_t_global:.3f}s, "
                  f"no further fires for {_silence_ms:.0f} ms before end of file")


# ===== PLOT 6 — Voltage D term =====
# Row 1 is split left/right because amps and V/s are unrelated scales; both sharex the voltage panel.
fig6 = plt.figure(figsize=(18, 10), num="Plot 6 — Voltage D Term")
gs6  = gridspec.GridSpec(3, 2, height_ratios=[3.2, 2.6, 0.9], hspace=0.13, wspace=0.16)
ax6  = fig6.add_subplot(gs6[0, :])
ax6b = fig6.add_subplot(gs6[1, 0], sharex=ax6)   # current domain
ax6c = fig6.add_subplot(gs6[1, 1], sharex=ax6)   # slope domain (the gate signal)
ax6s = fig6.add_subplot(gs6[2, :], sharex=ax6)
plt.setp(ax6.get_xticklabels(), visible=False)
fig6.suptitle(f"Plot 6 — Voltage D Term  |  {outer_label}", fontsize=14, y=0.99)
add_subtitle(fig6,
    f"{_brk_label}\n"
    "Teal band = D term actually trimming the setpoint (ground truth).  "
    "Orange scatter = the trim itself.")
fig6.subplots_adjust(top=0.88, right=0.80)

# --- 6a: voltage domain ---
ax6.plot(df["t_plot"], df["battV"],
         color="#1565c0", lw=2.5, label="battV (measured)")
ax6.plot(df["t_plot"], df["battV_filt_V"],
         color="#90caf9", lw=1.8, linestyle="--", alpha=0.90, label="Smoothed display voltage (battV_filt_V — VoltageFilterTC EMA)")
if "cvKdFiltV_V" in df.columns and df["cvKdFiltV_V"].notna().any():
    # The signal the firmware actually differentiates AND arms on (CvKdVoltFiltTC EMA); coincides with
    # battV_filt_V only while the two TCs are set equal
    ax6.plot(df["t_plot"], df["cvKdFiltV_V"],
             color="#8e24aa", lw=1.8, alpha=0.90,
             label="Voltage for D term (cvKdFiltV_V — its slope AND arm distance)")
if "ovFilt_V" in df.columns and df["ovFilt_V"].notna().any():
    ax6.plot(df["t_plot"], df["ovFilt_V"],
             color="#6a1b9a", lw=1.8, alpha=0.90, label="ovFilt_V (G2 comparator input)")
# targSlewed (the setpoint the PI tracks) diverges from targV only on a rising target. Drawn thick and
# UNDER the dashed targV so neither hides the other where they coincide.
if _targ_pi_exact:
    ax6.plot(df["t_plot"], df["targSlewed_V"],
             color="#ff8f00", lw=3.0, alpha=0.90,
             label="targSlewed_V (governor output — what the PI tracks)")
ax6.plot(df["t_plot"], df["targV"],
         color="#e91e63", lw=1.8, linestyle="--", label="targV (raw setpoint)")
# Arm floor: at/above it the D term is close enough to target to act (absent when CvKdArmV=0).
if "armFloor_V" in df.columns and df["armFloor_V"].notna().any():
    _af_ref = "targSlewed" if _targ_pi_exact else "targV"
    ax6.plot(df["t_plot"], df["armFloor_V"],
             color="#00838f", lw=1.4, linestyle=":", alpha=0.85,
             label=f"{_af_ref} − CvKdArmV ({_brk_armv:.3g} V) — arm floor")
ax6.set_ylabel("Voltage (V)")
ax6.grid(**GRID_KW)
ax6_d = add_duty_axis(ax6)
_h6 = [l for l in ax6.get_lines() if not l.get_label().startswith("_")]
_h6_duty = [l for l in ax6_d.get_lines() if not l.get_label().startswith("_")]
# Legends sit lower-right: on a D-term log the episode is a rise INTO target, so the upper-left
# quadrant every other plot uses is exactly where the event is. Draggable if a log breaks that.
_leg6 = ax6_d.legend(_h6 + _h6_duty, [l.get_label() for l in _h6 + _h6_duty], loc="lower right", fontsize=9)
_leg6.set_draggable(True)
add_kd_shading(ax6, df)
add_ov_shading(ax6, df)

# --- 6b: current domain + the gate signal ---
if "iMeas_A" in df.columns:
    ax6b.plot(df["t_plot"], df["iMeas_A"],
              color="#c62828", lw=2.0, alpha=0.90, label="Actual current  (iMeas_A)")
if "spLimited_A" in df.columns:
    ax6b.plot(df["t_plot"], df["spLimited_A"],
              color="#2e7d32", lw=2.0, label="Current target — PID command  (spLimited_A)")
if "Icv_A" in df.columns:
    ax6b.plot(df["t_plot"], df["Icv_A"],
              color="#e91e63", lw=1.8, linestyle="--", alpha=0.85, label="CV setpoint  (Icv_A — what the D term trims)")
# kdTrim_A is recomputed every output tick, so every nonzero row is a real trim: do not filter by
# voltLoopFired (hides ~95% of trims). No scatter = D never acted. Signed: <0 = added (two-sided mode).
if "kdTrim_A" in df.columns:
    _kt6 = df[df["kdTrim_A"] != 0]
    if not _kt6.empty:
        ax6b.scatter(_kt6["t_plot"], _kt6["kdTrim_A"],
                     s=42, color="#f57c00", zorder=6, edgecolor="#e65100", linewidth=0.6,
                     label="D-term trim (A off Icv — ground truth; <0 = added, two-sided mode)")
ax6b.axhline(0, color="#999999", linewidth=0.7, linestyle=":", alpha=0.5)
ax6b.set_ylabel("Current (A)")
ax6b.set_xlabel(time_label, fontsize=11)
ax6b.grid(**GRID_KW)
ax6b.set_title("Current domain — what the D term trims", fontsize=10, color="#444444", style="italic", pad=4)
add_kd_shading(ax6b, df)
_h6b = [l for l in ax6b.get_lines() if not l.get_label().startswith("_")]
_sc6 = [c for c in ax6b.collections if not c.get_label().startswith("_")]
_leg6b = ax6b.legend(_h6b + _sc6, [a.get_label() for a in _h6b + _sc6], loc="lower right", fontsize=9)
_leg6b.set_draggable(True)

# --- 6c: the gate signal vs the LIVE gate + arm window (V/s never shares a frame with amps) ---
# cvDSlope_Vps IS the gated signal: a ~100 ms sliding-window backward diff of cvKdFiltV_V compared directly
# to the live deadband. No slope EMA (its lag released the brake past the crest).
if "cvDSlope_Vps" in df.columns:
    ax6c.plot(df["t_plot"], df["cvDSlope_Vps"],
              color="#00838f", lw=1.8, alpha=0.95,
              label="Rise rate — the gated signal (cvDSlope_Vps)")
# Logged live gate clamp(floor, base + CvKdDbSlope·I, ceil), not the header base, which rides below it as
# current rises. Non-positive = not-evaluated sentinel, masked out.
if "kdDeadband_Vps" in df.columns and (df["kdDeadband_Vps"] > 0).any():
    _gate6c = df["kdDeadband_Vps"].where(df["kdDeadband_Vps"] > 0)
    ax6c.plot(df["t_plot"], _gate6c, color="#f57c00", lw=1.4, linestyle="--",
              alpha=0.90, label="Live deadband gate (kdDeadband_Vps = base + slope·I)")
    if _brk_onesided == 0:
        ax6c.plot(df["t_plot"], -_gate6c, color="#f57c00", lw=1.0, linestyle=":",
                  alpha=0.7, label="−live gate (two-sided mode)")
ax6c.axhline(0, color="#999999", linewidth=0.7, linestyle=":", alpha=0.5)
# Distance-gate window: the D term is eligible ONLY inside these bands (voltage within CvKdArmV of target).
# A rise-rate crossing OUTSIDE a band cannot trim — that is the second reason 6b shows no dot there.
_armed_shaded = False
if "kdArmed" in df.columns and (df["kdArmed"] == 1).any():
    for _a6, _b6 in _bool_spans(df["t_plot"].values, df["kdArmed"].values.astype(bool), bridge_s=0.12):
        ax6c.axvspan(_a6, _b6, color="#43a047", alpha=0.10, zorder=0)
    _armed_shaded = True
ax6c.set_ylabel("Rise slope (V/s)")
ax6c.set_xlabel(time_label, fontsize=11)
ax6c.grid(**GRID_KW)
ax6c.set_title("Gate domain — rise rate vs live gate & arm window", fontsize=10, color="#444444", style="italic", pad=4)
add_kd_shading(ax6c, df)
_h6c = [l for l in ax6c.get_lines() if not l.get_label().startswith("_")]
# Armed band and teal trim band are axvspans, not lines — add proxy patches so the legend explains them.
from matplotlib.patches import Patch
_p6c = []
if _armed_shaded:
    _p6c.append(Patch(color="#43a047", alpha=0.10, label="D-term armed (distance gate open)"))
if ("kdTrim_A" in df.columns and (df["kdTrim_A"].values != 0).any()) or \
   ("kdActive" in df.columns and df["kdActive"].any()):
    _p6c.append(Patch(color=KD_COLOR, alpha=0.16, label="CV D term trimming"))
_leg6c = ax6c.legend(_h6c + _p6c, [a.get_label() for a in _h6c + _p6c], loc="lower right", fontsize=9)
_leg6c.set_draggable(True)

_cb6 = _make_checkbox_panel(fig6, _h6 + _h6_duty + _h6b + _h6c)

draw_flag_bars(ax6s, df)

# Episode summary. Contiguous kdActive runs = one engagement, matching the firmware's ≥1s-quiet
# re-arm rule closely enough for counts to line up with the console's "CV D term #N" messages.
if "kdActive" in df.columns and df["kdActive"].any():
    _bg = (df["kdActive"].diff() == 1).cumsum()
    _eps6 = [g for _, g in df[df["kdActive"] == 1].groupby(_bg)]
    print(f"CV D term: {len(_eps6)} engagement(s)")
    for _i6, _g6 in enumerate(_eps6, 1):
        # kdTrim lands on every output tick: measure over all rows, not voltLoopFired rows; a summed "net A" is not physical.
        _nz = _g6.loc[_g6["kdTrim_A"] != 0, "kdTrim_A"].abs()
        _peak = _nz.max()  if len(_nz) else 0.0
        _mean = _nz.mean() if len(_nz) else 0.0
        # Signed larger-magnitude extreme, so a two-sided add episode (falling → negative slope) reports
        # its true fall rate rather than the ~0 that .max() would pick off the negative excursion.
        _slp6 = _g6["cvDSlope_Vps"]
        _pk_slope = _slp6.max() if abs(_slp6.max()) >= abs(_slp6.min()) else _slp6.min()
        print(f"  #{_i6}: t={_g6['t_s'].iloc[0]:.3f}–{_g6['t_s'].iloc[-1]:.3f}s  "
              f"peak slope={_pk_slope:.3f} V/s  "
              f"peak trim {_peak:.2f} A, mean {_mean:.2f} A over {len(_nz)} trim tick(s)")
elif _helpers_en == 0:
    print("CV D term: never engaged — cvHelpersEnabled=0, the D term was switched OFF")
else:
    # Nearest miss vs the LIVE per-row gate (kdDeadband_Vps): the header base alone would call a sub-gate
    # slope a "crossing" and wrongly blame the arm gate.
    _msg = "CV D term: never engaged in this log"
    if "cvDSlope_Vps" in df.columns and "kdDeadband_Vps" in df.columns and (df["kdDeadband_Vps"] > 0).any():
        _pk = df["cvDSlope_Vps"].max()
        _cleared = ((df["cvDSlope_Vps"] > df["kdDeadband_Vps"]) & (df["kdDeadband_Vps"] > 0)).any()
        _gmin = df.loc[df["kdDeadband_Vps"] > 0, "kdDeadband_Vps"].min()
        _msg += f"  (peak rise rate {_pk:.3f} V/s vs live gate {_gmin:.3g}+ V/s"
        _msg += " — never cleared it)" if not _cleared else " — cleared it, so the arm-distance gate held it off)"
    elif "cvDSlope_Vps" in df.columns and not np.isnan(_brk_thresh):
        _pk = df["cvDSlope_Vps"].max()
        _msg += f"  (peak rise rate {_pk:.3f} V/s vs deadband {_brk_thresh:.3g} V/s"
        _msg += " — never crossed)" if _pk <= _brk_thresh else " — crossed, so the arm-distance gate held it off)"
    print(_msg)


# ===== PLOT 7 — Oscillation Damper (hunt governor) =====
# The damper scales the INNER current-loop Ki, which no header gain reflects. Skipped (not drawn empty)
# on logs without huntDerate.
_have_hunt = "huntDerate" in df.columns and df["huntDerate"].notna().any()

if _have_hunt:
    from matplotlib.patches import Patch  # also imported by Plot 6; this figure must not depend on that
    _hs = df["huntState"].fillna(0).astype(int) if "huntState" in df.columns else pd.Series(0, index=df.index)
    _hd_pct = df["huntDerate"] * 100.0

    fig7 = plt.figure(figsize=(18, 9), num="Plot 7 — Oscillation Damper")
    gs7  = gridspec.GridSpec(3, 1, height_ratios=[3.0, 2.0, 0.9], hspace=0.12)
    ax7  = fig7.add_subplot(gs7[0])
    ax7b = fig7.add_subplot(gs7[1], sharex=ax7)
    ax7s = fig7.add_subplot(gs7[2], sharex=ax7)
    plt.setp(ax7.get_xticklabels(), visible=False)
    plt.setp(ax7b.get_xticklabels(), visible=False)
    fig7.suptitle(f"Plot 7 — Oscillation Damper  |  {outer_label}", fontsize=14, y=0.99)
    add_subtitle(fig7,
        "Inner current-loop Ki as a share of the configured PID Ki. "
        "Red band = damping episode in progress; grey band = standing down after a wobble that did not respond.")
    fig7.subplots_adjust(top=0.90, right=0.80)

    # --- 7a: the action (derate) against the signal it reacts to (field duty) ---
    ax7.plot(df["t_plot"], _hd_pct,
             color="#c62828", lw=2.4, label="Inner-loop Ki derate (% of configured PID Ki)")
    ax7.axhline(y=100, color="#9e9e9e", linestyle="--", lw=1.0, alpha=0.7)
    ax7.axhline(y=25, color="#c62828", linestyle=":", lw=1.0, alpha=0.5,
                label="Derate floor (25% — stacked episodes stop here)")
    ax7.set_ylabel("Inner Ki (% of setting)")
    ax7.set_ylim(0, 110)
    ax7.grid(**GRID_KW)
    ax7.set_title("Field duty is the detector's own input — the wobble it reacts to is visible on the right axis",
                  fontsize=10, color="#444444", style="italic", pad=4)
    ax7_d = add_duty_axis(ax7)
    # Duty axis zoomed to the data here (not 0-115%): the detector fires on a ~0.5% peak wobble.
    if "duty_pct" in df.columns and df["duty_pct"].notna().any():
        _dlo, _dhi = df["duty_pct"].min(), df["duty_pct"].max()
        _pad = max(0.5, (_dhi - _dlo) * 0.35)
        ax7_d.set_ylim(_dlo - _pad, _dhi + _pad)
        ax7_d.set_ylabel("Field duty (%) — zoomed to show shape", color=DUTY_COLOR, fontsize=11)

    # Episode / stand-down bands. bridge_s covers the 1.6 s evaluation cadence: state only updates on
    # an eval, so consecutive samples inside one episode are contiguous but a slow log can gap.
    _t7 = df["t_plot"].values
    _ep_spans = _bool_spans(_t7, (_hs == 1).values, bridge_s=2.0)
    _sd_spans = _bool_spans(_t7, (_hs == 3).values, bridge_s=2.0)
    for _a7, _b7 in _ep_spans:
        for _axx in (ax7, ax7b):
            _axx.axvspan(_a7, _b7, color="#c62828", alpha=0.12, zorder=0)
    for _a7, _b7 in _sd_spans:
        for _axx in (ax7, ax7b):
            _axx.axvspan(_a7, _b7, color="#616161", alpha=0.12, zorder=0)

    _h7 = [l for l in ax7.get_lines() if not l.get_label().startswith("_")]
    _h7_duty = [l for l in ax7_d.get_lines() if not l.get_label().startswith("_")]
    _p7 = []
    if _ep_spans:
        _p7.append(Patch(color="#c62828", alpha=0.12, label="Damping episode"))
    if _sd_spans:
        _p7.append(Patch(color="#616161", alpha=0.12, label="Standing down (wobble was not ours)"))
    _leg7 = ax7.legend(_h7 + _h7_duty + _p7,
                       [a.get_label() for a in _h7 + _h7_duty + _p7],
                       loc="lower left", fontsize=9)
    _leg7.set_draggable(True)

    # --- 7b: the speed-coupled side — engine speed against the confirmed wobble frequency ---
    ax7b.plot(df["t_plot"], df["rpm"], color="#1565c0", lw=1.8, label="Engine speed (rpm)")
    ax7b.set_ylabel("Engine speed (rpm)")
    ax7b.grid(**GRID_KW)
    ax7b.set_title("The idle-knee hunt is speed-coupled — read the confirmed frequency against the rpm it appeared at",
                   fontsize=10, color="#444444", style="italic", pad=4)
    ax7b_f = ax7b.twinx()
    _hf = df["huntFreq_Hz"] if "huntFreq_Hz" in df.columns else pd.Series(np.nan, index=df.index)
    # 0 means "nothing confirmed yet this session" — plotting it as a real 0 Hz reading would invent
    # a measurement the firmware never made.
    ax7b_f.plot(df["t_plot"], _hf.where(_hf > 0),
                color="#6a1b9a", lw=1.8, linestyle="--", alpha=0.9,
                label="Confirmed wobble frequency (Hz)")
    ax7b_f.set_ylabel("Wobble frequency (Hz)", color="#6a1b9a", fontsize=12)
    ax7b_f.set_ylim(0, 2.6)
    ax7b_f.tick_params(axis="y", colors="#6a1b9a", labelsize=11)
    _h7b = [l for l in ax7b.get_lines() + ax7b_f.get_lines() if not l.get_label().startswith("_")]
    _leg7b = ax7b.legend(_h7b, [a.get_label() for a in _h7b], loc="upper right", fontsize=9)
    _leg7b.set_draggable(True)

    _cb7 = _make_checkbox_panel(fig7, _h7 + _h7_duty + _h7b)
    draw_flag_bars(ax7s, df)

    # Episode summary — the console prints a verdict per episode, but this recovers the same story
    # from the log alone, including episodes whose verdict never reached flash.
    if _ep_spans:
        print(f"Oscillation damper: {len(_ep_spans)} episode(s) in this log")
        for _i7, (_a7, _b7) in enumerate(_ep_spans, 1):
            _m7 = (df["t_plot"] >= _a7) & (df["t_plot"] <= _b7)
            _g7 = df[_m7]
            _f7 = _g7.loc[_g7["huntFreq_Hz"] > 0, "huntFreq_Hz"]
            _lo = _hd_pct[_m7].min()          # deepest cut — matches the ledger's derateExit
            # Recovery ceiling: highest the creep got back to before the next episode (or log end); not in the ledger.
            _nxt = next((a for a, _ in _ep_spans if a > _b7), None)
            _tail = _hd_pct[(df["t_plot"] > _b7) & ((df["t_plot"] < _nxt) if _nxt is not None else True)]
            _ceil = _tail.max() if len(_tail) else _lo
            _fs = f"{_f7.iloc[0]:.2f} Hz" if len(_f7) else "frequency not logged"
            print(f"  #{_i7}: t={_g7['t_s'].iloc[0]:.1f}–{_g7['t_s'].iloc[-1]:.1f}s  {_fs}  "
                  f"rpm {_g7['rpm'].min():.0f}–{_g7['rpm'].max():.0f}  "
                  f"Ki cut to {_lo:.0f}%, recovered to {_ceil:.0f}% by "
                  f"{'the next episode' if _nxt is not None else 'end of log'}")
        if _sd_spans:
            print(f"  {len(_sd_spans)} stand-down window(s) — those episodes reverted to full gain "
                  f"(external cyclic load or engine governor suspected)")
    elif (_hd_pct < 99.5).any():
        print(f"Oscillation damper: no episode opened in this log, but Ki was already held at "
              f"{_hd_pct.min():.0f}% — an episode earlier in the session set a recovery ceiling")
    else:
        print("Oscillation damper: never engaged in this log — inner Ki stayed at 100% throughout")
else:
    fig7 = None
    print("Oscillation damper: no huntDerate column — log predates the damper telemetry")


# ===== Linked x-axis zoom across all plot windows =====
_all_primary_axes = [ax1, ax2a, ax3, ax4, ax5, ax6]
_all_figs         = [fig1, fig2, fig3, fig4, fig5, fig6]
if _have_hunt:
    _all_primary_axes.append(ax7)
    _all_figs.append(fig7)
_syncing = [False]

def _on_xlim_changed(changed_ax):
    if _syncing[0]:
        return
    _syncing[0] = True
    try:
        new_xlim = changed_ax.get_xlim()
        for ax in _all_primary_axes:
            if ax is not changed_ax:
                ax.set_xlim(new_xlim)
        for fig in _all_figs:
            fig.canvas.draw_idle()
    finally:
        _syncing[0] = False

for _ax in _all_primary_axes:
    _ax.callbacks.connect("xlim_changed", _on_xlim_changed)

# Grab-and-drag panning on every window. Panning x fires the xlim_changed sync above, so a drag in
# one figure carries the others with it — same as a zoom.
for _fig in _all_figs:
    enable_pan(_fig)

# ===== File Trimmer =====
_trim_total_s = df["t_s"].iloc[-1]

def _fmt_trim_t(t):
    return f"{int(t)}" if t == int(t) else f"{t:.3f}".rstrip("0").rstrip(".")

def _do_trim(event=None):
    try:
        _s = float(_tb_trim_start.text)
        _e = float(_tb_trim_end.text)
    except ValueError:
        _trim_status_lbl.set_text("Bad input — start/end must be numbers")
        fig1.canvas.draw_idle()
        return
    if _s < 0 or _s >= _e or _e > _trim_total_s * 1.001:
        _trim_status_lbl.set_text(f"Range error  (file is 0 – {_trim_total_s:.1f} s)")
        fig1.canvas.draw_idle()
        return
    from io import StringIO as _TrimSI
    _tsep = "\t" if "\t" in _header_line else ","
    _trdf = pd.read_csv(
        _TrimSI("".join(_lines[_header_idx + 1:])),
        sep=_tsep, names=_col_names, on_bad_lines="skip"
    )
    _tcol = next((c for c in ("ts_ms", "t_s") if c in _trdf.columns), None)
    if _tcol is None:
        _trim_status_lbl.set_text("No time column found")
        fig1.canvas.draw_idle()
        return
    _trdf[_tcol] = pd.to_numeric(_trdf[_tcol], errors="coerce")
    _trdf.dropna(subset=[_tcol], inplace=True)
    _t_zero = _trdf[_tcol].iloc[0]
    _t_s_col = (_trdf[_tcol] - _t_zero) / (1000.0 if _tcol == "ts_ms" else 1.0)
    _tmask = (_t_s_col >= _s) & (_t_s_col <= _e)
    _tout  = _trdf[_tmask]
    if _tout.empty:
        _trim_status_lbl.set_text("No rows in that range")
        fig1.canvas.draw_idle()
        return
    _tname = f"{basename}_{_fmt_trim_t(_s)}s_{_fmt_trim_t(_e)}s.csv"
    _tpath = os.path.join(DOWNLOADS, _tname)
    with open(_tpath, "w", encoding="utf-8", newline="") as _tf:
        for _tln in _lines[:_header_idx]:
            _tf.write(_tln)
        _tf.write(_header_line + "\n")
        _tout.to_csv(_tf, index=False, header=False, columns=_col_names)
    _trim_status_lbl.set_text(f"Saved: {_tname}  ({len(_tout)} rows)")
    fig1.canvas.draw_idle()

_ax_trim_s   = fig1.add_axes([0.12, 0.018, 0.10, 0.05])
_ax_trim_e   = fig1.add_axes([0.28, 0.018, 0.10, 0.05])
_ax_trim_go  = fig1.add_axes([0.40, 0.018, 0.04, 0.05])
for _ax_w in (_ax_trim_s, _ax_trim_e, _ax_trim_go):
    _ax_w.set_navigate(False)   # widget axes: never pan or rubber-band zoom them
_tb_trim_start = TextBox(_ax_trim_s, "Start (s) ", initial="0")
_tb_trim_end   = TextBox(_ax_trim_e, "End (s) ",   initial=str(round(_trim_total_s, 1)))
_btn_trim_go   = MplButton(_ax_trim_go, "Go", color="#1565c0", hovercolor="#0d47a1")
_btn_trim_go.label.set_color("white")
_btn_trim_go.label.set_fontsize(10)
_btn_trim_go.on_clicked(_do_trim)
_trim_status_lbl = fig1.text(
    0.46, 0.043,
    f"File: {basename}  ({_trim_total_s:.1f} s)",
    fontsize=9, color="#555555", verticalalignment="center"
)
fig1.text(0.03, 0.043, "Trim file:", fontsize=10, color="#1a1a1a",
          fontweight="bold", verticalalignment="center")

print("Tip: use the checkboxes on the right of each plot to show/hide series.")
tile_figures()
plt.show()
