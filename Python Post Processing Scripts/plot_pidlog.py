"""
plot_pidlog.py
Diagnostic plotter for TargetVoltageMode instability.

4 plot windows, each with a state strip below:
  Plot 1 — Voltage: what the voltage loop sees (battV, target, vError)
  Plot 2 — Voltage loop command chain (rawVoltageCap → voltageCapAmps → uTarget)
  Plot 3 — Output current PID internals (setpoint, input, terms, saturation)
  Plot 4 — Duty pipeline + actual amps (dutyRequest vs dutyApplied vs measAmps)

State strip shows:
  - Color-coded charge stage bar
  - Orange tick marks where voltageLoopRanThisTick=1 (voltage loop fired)
  - Cyan dashed vlines for enteringCV
  - Blue dashed vlines for enteringTargetVoltageMode

File picker searches ~/Downloads for *.csv, newest first.
PNGs saved to Downloads alongside source CSV.
"""

import glob
import os
import tkinter as tk
from tkinter import messagebox
from filepicker import pick_file
from plotlayout import tile_figures, enable_pan

import matplotlib
matplotlib.use("TkAgg")
import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec
from matplotlib.widgets import CheckButtons, TextBox, Button as MplButton
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
path = pick_file(prefix="pidlog_", title="Select PID Log")
if not path:
    raise SystemExit("No file selected.")

basename = os.path.splitext(os.path.basename(path))[0]
print(f"Loading: {path}")

# A too-small ESP32 line buffer can glue the comment block onto the header line, so the header starts
# at the first "ts_ms" token wherever it appears.

with open(path, encoding="utf-8", errors="replace") as _f:
    _lines = _f.readlines()

_header_idx  = None
_header_line = None
_sep         = "\t"

for _i, _raw in enumerate(_lines):
    if "ts_ms" in _raw:
        _header_idx  = _i
        _header_line = _raw[_raw.find("ts_ms"):].strip()
        break

if _header_idx is None:
    raise SystemExit(
        f"ERROR: No line containing 'ts_ms' found in {path}.\n"
        f"The CSV may be empty, or the header row was never written.\n"
        f"Check that PidDLState.line[] is >= 420 bytes in the ESP32 handler."
    )

print(f"Header found at file line {_header_idx}: {_header_line[:80]}...")

if "\t" in _header_line:
    _sep = "\t"
else:
    _sep = ","

_col_names = [c.strip() for c in _header_line.split(_sep)]
print(f"Delimiter: {repr(_sep)}  |  Columns ({len(_col_names)}): {_col_names}")

from io import StringIO
_data_text = "".join(_lines[_header_idx + 1:])

# Some pidlogs lose the newline after the header, gluing the first data row onto a chopped last column
# name (2N-1 tokens). Restore the canonical header and peel the first row back out.
import re
# Canonical pidlog header — must match the firmware header in 3_functions.ino.
EXPECTED_HEADER = [
    "ts_ms", "chargeStageDisplay", "TargetVoltageMode", "battV",
    "ChargingVoltageTarget", "vError", "Icv", "cv_I", "tableThermalLimit",
    "setpointCmd", "voltageLoopRanThisTick", "pidSetpoint", "pidInput",
    "pidUnsatOutput", "pidOutput", "innerTermP", "innerTermI", "innerTermD",
    "dutyRequest", "dutyApplied", "enteringCV", "enteringTargetVoltageMode",
    "rpm", "measAmps", "innerKp", "innerKi", "innerKd", "voltageKp",
    "voltageKi", "battV_filt_V", "flags",
    "ovFlags", "dBcur_dt", "battI", "ch1IntervalMs", "voltLoopIntervalMs",
    "inaIntervalMs", "mExcessEma", "iExcessThreshold",
]
_N = len(EXPECTED_HEADER)
if _sep == "," and len(_col_names) != _N:
    if len(_col_names) == 2 * _N - 1:
        _garbled = _col_names[_N - 1]            # last header name + glued ts_ms
        _data_tail = _col_names[_N:]             # the remaining N-1 data values
        _m = re.search(r"(\d+)$", _garbled)      # ts_ms is the trailing integer
        if _m:
            _first_row = ",".join([_m.group(1)] + _data_tail)
            _data_text = _first_row + "\n" + _data_text
            print(f"Recovered merged header line: restored canonical {_N}-col "
                  f"header and re-injected first data row (ts_ms={_m.group(1)}).")
            _col_names = list(EXPECTED_HEADER)
        else:
            raise SystemExit(
                f"ERROR: header line looks merged ({len(_col_names)} tokens) but "
                f"could not extract ts_ms from garbled token {_garbled!r}."
            )
    else:
        print(f"WARNING: header has {len(_col_names)} columns, expected {_N}. "
              f"Firmware schema may have changed — using the file's header as-is.")

df = pd.read_csv(
    StringIO(_data_text),
    sep=_sep,
    names=_col_names,
    on_bad_lines="skip"
)

if "ts_ms" not in df.columns:
    raise SystemExit(
        f"ERROR: 'ts_ms' not in parsed columns: {list(df.columns)}"
    )

_rename = {}
for _col in list(df.columns):
    if _col == "battV_filt_V":
        _rename["battV_filt_V"] = "battV_filt"
if _rename:
    df.rename(columns=_rename, inplace=True)
    print(f"Renamed columns: {_rename}")

numeric_cols = [
    "ts_ms",
    "chargeStageDisplay", "TargetVoltageMode",
    "battV", "ChargingVoltageTarget", "vError",
    "Icv", "cv_I",
    "tableThermalLimit", "setpointCmd", "voltageLoopRanThisTick",
    "pidSetpoint", "pidInput", "pidUnsatOutput", "pidOutput",
    "innerTermP", "innerTermI", "innerTermD",
    "dutyRequest", "dutyApplied",
    "enteringCV", "enteringTargetVoltageMode",
    "rpm", "measAmps",
    "gainKp", "gainKi", "gainKd",   # old log format — inner current-loop gains
    "innerKp", "innerKi", "innerKd",  # new log format — inner current-loop gains
    "voltageKp", "voltageKi",  # new log format — outer voltage loop gains (no D term)
    "flags",
    "battV_filt",
    "ovFlags", "dBcur_dt", "battI",
    "ch1IntervalMs", "voltLoopIntervalMs", "inaIntervalMs",
    "mExcessEma", "iExcessThreshold",   # iExcess detector: averaged excess vs fire threshold E (A)
]

for col in numeric_cols:
    if col in df.columns:
        df[col] = pd.to_numeric(df[col], errors="coerce")

df.dropna(subset=["ts_ms"], inplace=True)
df.reset_index(drop=True, inplace=True)
df["t_ms"] = df["ts_ms"] - df["ts_ms"].iloc[0]
df["t_s"] = df["t_ms"] / 1000.0
if df["t_ms"].iloc[-1] > 5000:
    df["t_plot"] = df["t_ms"] / 1000.0
    time_label = "Time (s)"
else:
    df["t_plot"] = df["t_ms"]
    time_label = "Time (ms)"

df["pid_saturation"] = (df["pidUnsatOutput"] - df["pidOutput"]).abs()
df["duty_clamp"]     = (df["dutyRequest"] - df["dutyApplied"]).abs()

# innerKp/Ki/Kd are the EFFECTIVE gains and innerKi moves when the oscillation damper derates: report
# the span, never row 1 alone. Old logs name them gainKp/Ki/Kd.
def _gain_label(cols):
    for c in cols:
        if c in df.columns:
            v = pd.to_numeric(df[c], errors="coerce").dropna()
            if v.empty: continue
            lo, hi = float(v.min()), float(v.max())
            return f"{lo:.4g}" if (hi - lo) <= 1e-4 * max(abs(hi), 1.0) else f"{lo:.4g}->{hi:.4g}"
    return "n/a"
kp_inner = _gain_label(["innerKp", "gainKp"])
ki_inner = _gain_label(["innerKi", "gainKi"])
kd_inner = _gain_label(["innerKd", "gainKd"])
inner_label = f"Output Current PID  Kp={kp_inner}  Ki={ki_inner}  Kd={kd_inner}"
print(f"Gains: {inner_label}")

def _to_int(col):
    return pd.to_numeric(df[col], errors="coerce").fillna(0).astype(int)

df["chargeStageDisplay"]        = _to_int("chargeStageDisplay")
df["voltageLoopRanThisTick"]    = _to_int("voltageLoopRanThisTick")
df["enteringCV"]                = _to_int("enteringCV")
df["enteringTargetVoltageMode"] = _to_int("enteringTargetVoltageMode")
df["TargetVoltageMode"]         = _to_int("TargetVoltageMode")

_flags_num = pd.to_numeric(df["flags"], errors="coerce").fillna(0)
df["f_govBypass"] = (_flags_num // 16 % 2).astype(int)
df["f_cvBatt"]    = (_flags_num // 32 % 2).astype(int)   # bit5 (§G): inner loop regulating BATTERY current (pidInput = battI)

_ov_num = pd.to_numeric(df["ovFlags"] if "ovFlags" in df.columns else pd.Series(0, index=df.index), errors="coerce").fillna(0)
df["f_fastOvActive"]  = (_ov_num       % 2).astype(int)   # bit 0
df["f_iExcessBulk"]   = (_ov_num // 2  % 2).astype(int)   # bit 1 — iExcess BULK sub-mode (current-control phase)
df["f_hardClamp"]     = (_ov_num // 4  % 2).astype(int)   # bit 2
df["f_iExcess"]       = (_ov_num // 8  % 2).astype(int)   # bit 3
df["f_loadDump"]      = (_ov_num // 16 % 2).astype(int)   # bit 4

# ===== Stage colors and mode helpers =====
STAGE_COLORS = {
    0: ("#777777", "NONE"),
    1: ("#2e7d32", "BULK"),
    2: ("#6a1b9a", "ABSORPTION"),
    3: ("#e91e63", "FLOAT"),
    4: ("#c62828", "MANUAL"),
    5: ("#388e3c", "MAINTAIN"),
    6: ("#1565c0", "TARGET-V"),
    7: ("#888888", "IDLE"),
    8: ("#795548", "COMMISSIONING"),
}

EV_COLOR_CV     = "#00838f"   # enteringCV vlines
EV_COLOR_TVM    = "#1565c0"   # enteringTargetVoltageMode vlines
EV_COLOR_VLOOP  = "#aeea00"   # voltageLoopRanThisTick ticks (lime — distinct from red stages)

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


def stage_color_label(stage_int):
    return STAGE_COLORS.get(int(stage_int), ("#777777", "?"))

state_changes = df.index[
    (df["chargeStageDisplay"] != df["chargeStageDisplay"].shift()) |
    (df["flags"] != df["flags"].shift())
].tolist()
if state_changes and state_changes[0] == 0:
    state_changes = state_changes[1:]

# ===== Reusable drawing helpers =====
GRID_KW = dict(alpha=0.4, linewidth=0.7)


def draw_state_strip(ax, df, state_changes):
    ax.set_ylim(0, 1)
    ax.set_yticks([])
    ax.set_xlim(df["t_plot"].iloc[0], df["t_plot"].iloc[-1])
    ax.set_xlabel(time_label, fontsize=13)

    span = df["t_plot"].iloc[-1] - df["t_plot"].iloc[0]

    boundaries = state_changes + [len(df) - 1]
    prev_t   = df["t_plot"].iloc[0]
    prev_row = df.iloc[0]
    for idx in boundaries:
        t = df.loc[idx, "t_plot"] if idx < len(df) else df["t_plot"].iloc[-1]
        color, label = stage_color_label(prev_row["chargeStageDisplay"])
        width = t - prev_t
        ax.barh(0.72, width, left=prev_t, height=0.42,
                color=color, alpha=0.85, align="center")
        if width > span * 0.03:
            ax.text(prev_t + width / 2, 0.72, label,
                    ha="center", va="center",
                    fontsize=8, color="white", fontweight="bold", clip_on=True)
        if idx < len(df):
            prev_row = df.loc[idx]
        prev_t = t

    vloop_rows = df[df["voltageLoopRanThisTick"] == 1]
    for t in vloop_rows["t_plot"]:
        ax.axvline(x=t, ymin=0.0, ymax=0.30,
                   color=EV_COLOR_VLOOP, linewidth=1.0, alpha=0.85)
    if not vloop_rows.empty:
        ax.text(df["t_plot"].iloc[0] + span * 0.005, 0.13,
                "VLoop ▲", fontsize=7, color="#212121",
                fontweight="bold", va="center")

    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.spines["left"].set_visible(False)


def add_event_vlines(ax, df):
    for t in df.loc[df["enteringCV"] == 1, "t_plot"]:
        ax.axvline(x=t, color=EV_COLOR_CV, linewidth=1.2,
                   linestyle="--", alpha=0.8, label="_nolegend_")
    for t in df.loc[df["enteringTargetVoltageMode"] == 1, "t_plot"]:
        ax.axvline(x=t, color=EV_COLOR_TVM, linewidth=1.4,
                   linestyle="--", alpha=0.9, label="_nolegend_")


def add_mode_vlines(ax, df, state_changes):
    for idx in state_changes:
        t = df.loc[idx, "t_plot"]
        color, _ = stage_color_label(df.loc[idx, "chargeStageDisplay"])
        ax.axvline(x=t, color=color, linewidth=1.0,
                   linestyle="--", alpha=0.4)


def save_fig(fig, suffix):
    out = os.path.join(DOWNLOADS, f"{basename}_{suffix}.png")
    fig.savefig(out, dpi=150, bbox_inches="tight")
    print(f"Saved: {out}")


def make_fig(title_suffix, num):
    fig = plt.figure(figsize=(18, 8), num=num)
    gs  = gridspec.GridSpec(2, 1, height_ratios=[5, 0.5], hspace=0.08)
    ax  = fig.add_subplot(gs[0])
    axs = fig.add_subplot(gs[1], sharex=ax)
    plt.setp(ax.get_xticklabels(), visible=False)
    fig.suptitle(f"{title_suffix}  |  {inner_label}", fontsize=14)
    fig.subplots_adjust(right=0.80)
    return fig, ax, axs


# ===== PLOT 1 — Voltage: what the voltage loop sees =====
fig1, ax1, ax1s = make_fig("Plot 1 — Voltage Loop Input", "Plot 1 — Voltage")
ax1b = ax1.twinx()

ax1.plot(df["t_plot"], df["battV"],
         color="#1565c0", lw=2.5, label="battV")
ax1.plot(df["t_plot"], df["battV_filt"],
         color="#64b5f6", lw=2.0, linestyle="--", label="battV_filt", alpha=0.85)
ax1.plot(df["t_plot"], df["ChargingVoltageTarget"],
         color="#e91e63", lw=2.2, linestyle="--", label="ChargingVoltageTarget")
ax1b.plot(df["t_plot"], df["vError"],
          color="#c62828", lw=2.0, label="vError", alpha=0.9)
ax1b.axhline(0, color="#c62828", linewidth=0.6, linestyle=":", alpha=0.4)

ax1.set_ylabel("Voltage (V)", color="#1565c0")
ax1b.set_ylabel("vError (V)", color="#c62828")
ax1.grid(**GRID_KW)

lines1  = ax1.get_lines() + ax1b.get_lines()
labels1 = [l.get_label() for l in lines1 if not l.get_label().startswith("_")]
_leg1 = ax1b.legend(lines1[:len(labels1)], labels1, loc="upper left")
_leg1.set_draggable(True)
_cb1 = _make_checkbox_panel(fig1, lines1)

add_event_vlines(ax1, df)
add_mode_vlines(ax1, df, state_changes)
draw_state_strip(ax1s, df, state_changes)

for t in df.loc[df["enteringCV"] == 1, "t_plot"]:
    ax1s.axvline(x=t, color=EV_COLOR_CV, linewidth=1.2,
                 linestyle="--", alpha=0.8)
for t in df.loc[df["enteringTargetVoltageMode"] == 1, "t_plot"]:
    ax1s.axvline(x=t, color=EV_COLOR_TVM, linewidth=1.4,
                 linestyle="--", alpha=0.9)

# save_fig(fig1, "plot1_voltage")

# ===== PLOT 2 — CV loop =====
fig2, ax2, ax2s = make_fig("Plot 2 — CV Loop (Icv, cv_I, limits)  [NOTE: post-OV-cap uTargetAmps not logged]", "Plot 2 — CV Loop")
ax2b = ax2.twinx()

ax2.plot(df["t_plot"], df["tableThermalLimit"],
         color="#1565c0", lw=2.5, label="tableThermalLimit (RPM/thermal ceiling — pre-OV cap)")
ax2.plot(df["t_plot"], df["setpointCmd"],
         color="#2e7d32", lw=2.5, label="setpointCmd (=Icv in CV, =uTargetAmps in bulk)")
ax2.plot(df["t_plot"], df["Icv"],
         color="#e91e63", lw=2.2, linestyle="--", label="Icv (CV setpoint)")

ax2b.plot(df["t_plot"], df["cv_I"],
          color="#6a1b9a", lw=2.0, label="cv_I (integrator state, A)", alpha=0.85)
ax2b.set_ylabel("cv_I (A) — integrator state", color="#6a1b9a")
ax2.grid(**GRID_KW)

lines2  = ax2.get_lines() + ax2b.get_lines()
labels2 = [l.get_label() for l in lines2 if not l.get_label().startswith("_")]
_leg2 = ax2b.legend(lines2[:len(labels2)], labels2, loc="upper left")
_leg2.set_draggable(True)
_cb2 = _make_checkbox_panel(fig2, lines2)

add_event_vlines(ax2, df)
add_mode_vlines(ax2, df, state_changes)
draw_state_strip(ax2s, df, state_changes)

for t in df.loc[df["voltageLoopRanThisTick"] == 1, "t_plot"]:
    ax2.axvline(x=t, color=EV_COLOR_VLOOP, linewidth=0.7, alpha=0.4)

# save_fig(fig2, "plot2_outer_loop")

# ===== PLOT 3 — Output current PID internals =====
fig3 = plt.figure(figsize=(18, 10), num="Plot 3 — Output Current PID")
gs3  = gridspec.GridSpec(3, 1, height_ratios=[3, 2, 0.5], hspace=0.10)
ax3a = fig3.add_subplot(gs3[0])
ax3b = fig3.add_subplot(gs3[1], sharex=ax3a)
ax3s = fig3.add_subplot(gs3[2], sharex=ax3a)

plt.setp(ax3a.get_xticklabels(), visible=False)
plt.setp(ax3b.get_xticklabels(), visible=False)
fig3.suptitle(f"Plot 3 — Output Current PID Internals  |  {inner_label}", fontsize=14)
fig3.subplots_adjust(right=0.80)

ax3a.plot(df["t_plot"], df["pidSetpoint"],
          color="#e91e63", lw=2.5, linestyle="--", label="pidSetpoint")
ax3a.plot(df["t_plot"], df["pidInput"],
          color="#c62828", lw=2.2, label="pidInput (loop PV: battery current in §G amber spans, else alternator measAmps)")
ax3a.plot(df["t_plot"], df["pidOutput"],
          color="#2e7d32", lw=2.2, label="pidOutput (→ dutyReq)")
ax3a.plot(df["t_plot"], df["pidUnsatOutput"],
          color="#455a64", lw=1.8, linestyle=":", label="pidUnsatOutput", alpha=0.85)

ax3a.fill_between(df["t_plot"], df["pidOutput"], df["pidUnsatOutput"],
                  where=(df["pid_saturation"] > 0.1),
                  color="#455a64", alpha=0.18, label="saturation zone")

# §G: amber band where the inner loop regulates BATTERY current (pidInput = battI, not alternator measAmps).
if "f_cvBatt" in df.columns and df["f_cvBatt"].any():
    _cb_in, _cb_start = False, None
    for _i, _r in df.iterrows():
        if _r["f_cvBatt"] and not _cb_in:
            _cb_start, _cb_in = _r["t_plot"], True
        elif not _r["f_cvBatt"] and _cb_in:
            ax3a.axvspan(_cb_start, _r["t_plot"], color="#f9a825", alpha=0.07)
            _cb_in = False
    if _cb_in:
        ax3a.axvspan(_cb_start, df["t_plot"].iloc[-1], color="#f9a825", alpha=0.07)

ax3a.set_ylabel("Amps / Duty %")
ax3a.grid(**GRID_KW)
_h3a, _ = ax3a.get_legend_handles_labels()
_leg3a = ax3a.legend(loc="upper left")
_leg3a.set_draggable(True)

ax3b.plot(df["t_plot"], df["innerTermP"],
          color="#1565c0", lw=2.2, label="innerTermP")
ax3b.plot(df["t_plot"], df["innerTermI"],
          color="#f9a825", lw=2.2, label="innerTermI")
ax3b.plot(df["t_plot"], df["innerTermD"],
          color="#6a1b9a", lw=2.0, label="innerTermD")
ax3b.axhline(0, color="#888888", linewidth=0.5, alpha=0.5)
ax3b.set_ylabel("PID Term Value")
ax3b.grid(**GRID_KW)
_h3b, _ = ax3b.get_legend_handles_labels()
_leg3b = ax3b.legend(loc="upper left")
_leg3b.set_draggable(True)

_h3a_lines = [l for l in ax3a.get_lines() if not l.get_label().startswith("_")]
_h3b_lines = [l for l in ax3b.get_lines() if not l.get_label().startswith("_")]
_cb3 = _make_checkbox_panel(fig3, _h3a_lines + _h3b_lines)

add_event_vlines(ax3a, df)
add_event_vlines(ax3b, df)
add_mode_vlines(ax3a, df, state_changes)
add_mode_vlines(ax3b, df, state_changes)
draw_state_strip(ax3s, df, state_changes)

# save_fig(fig3, "plot3_inner_pid")

# ===== PLOT 4 — Duty pipeline + actual amps =====
fig4, ax4, ax4s = make_fig("Plot 4 — Duty Pipeline & Measured Amps", "Plot 4 — Duty")
ax4b = ax4.twinx()

ax4.plot(df["t_plot"], df["dutyRequest"],
         color="#f9a825", lw=2.2, linestyle="--", label="dutyRequest")
ax4.plot(df["t_plot"], df["dutyApplied"],
         color="#2e7d32", lw=2.5, label="dutyApplied")

ax4.fill_between(df["t_plot"], df["dutyRequest"], df["dutyApplied"],
                 where=(df["duty_clamp"] > 0.5),
                 color="#f9a825", alpha=0.22, label="governor clip zone")

ax4b.plot(df["t_plot"], df["measAmps"],
          color="#c62828", lw=2.2, label="measAmps", alpha=0.9)

ax4.set_ylabel("Duty (%)")
ax4b.set_ylabel("Measured Amps (A)", color="#c62828")
ax4.grid(**GRID_KW)

lines4  = ax4.get_lines() + ax4b.get_lines()
labels4 = [l.get_label() for l in lines4 if not l.get_label().startswith("_")]
_leg4 = ax4b.legend(lines4[:len(labels4)], labels4, loc="upper left")
_leg4.set_draggable(True)
_cb4 = _make_checkbox_panel(fig4, lines4)

add_event_vlines(ax4, df)
add_mode_vlines(ax4, df, state_changes)
draw_state_strip(ax4s, df, state_changes)

# save_fig(fig4, "plot4_duty")

# ===== PLOT 5 — Engine RPM =====
fig5, ax5, ax5s = make_fig("Plot 5 — Engine RPM", "Plot 5 — RPM")

if "rpm" in df.columns:
    ax5.plot(df["t_plot"], df["rpm"],
             color="#f9a825", lw=2.2, label="rpm")
else:
    ax5.text(0.5, 0.5, "rpm column not present in this log",
             ha="center", va="center", transform=ax5.transAxes,
             color="#888888", fontsize=13)

ax5.set_ylabel("RPM")
ax5.grid(**GRID_KW)
_h5, _ = ax5.get_legend_handles_labels()
_leg5 = ax5.legend(loc="upper left")
_leg5.set_draggable(True)
_h5_lines = [l for l in ax5.get_lines() if not l.get_label().startswith("_")]
_cb5 = _make_checkbox_panel(fig5, _h5_lines)
add_event_vlines(ax5, df)
add_mode_vlines(ax5, df, state_changes)
draw_state_strip(ax5s, df, state_changes)

# save_fig(fig5, "plot5_rpm")

# ===== PLOT 6 — Protection flags + battery current =====
fig6 = plt.figure(figsize=(18, 9), num="Plot 6 — Protection Flags & Battery Current")
gs6  = gridspec.GridSpec(3, 1, height_ratios=[3, 1.0, 0.5], hspace=0.10)
ax6a = fig6.add_subplot(gs6[0])
ax6b = fig6.add_subplot(gs6[1], sharex=ax6a)
ax6s = fig6.add_subplot(gs6[2], sharex=ax6a)

plt.setp(ax6a.get_xticklabels(), visible=False)
plt.setp(ax6b.get_xticklabels(), visible=False)
fig6.suptitle(f"Plot 6 — Protection Flags & Battery Current", fontsize=14)
fig6.subplots_adjust(right=0.80)

# 6a: battery current + dBcur_dt
ax6a_r = ax6a.twinx()
if "battI" in df.columns:
    ax6a.plot(df["t_plot"], df["battI"],
              color="#f9a825", lw=2.0, label="battI (A)")
    ax6a.axhline(0, color="#888888", linewidth=0.7, linestyle=":", alpha=0.6)
else:
    ax6a.text(0.5, 0.7, "battI not present (older firmware)",
              ha="center", va="center", transform=ax6a.transAxes,
              color="#888888", fontsize=12)
if "dBcur_dt" in df.columns:
    ax6a_r.plot(df["t_plot"], df["dBcur_dt"],
                color="#e91e63", lw=1.8, linestyle="--", label="dBcur_dt (A/s)", alpha=0.80)
    ax6a_r.axhline(0, color="#e91e63", linewidth=0.5, linestyle=":", alpha=0.4)

ax6a.set_ylabel("Battery Current (A)", color="#f9a825")
ax6a_r.set_ylabel("dBcur/dt (A/s)", color="#e91e63")
ax6a.grid(**GRID_KW)

_h6a = [l for l in ax6a.get_lines() + ax6a_r.get_lines() if not l.get_label().startswith("_")]
_leg6a = ax6a_r.legend(_h6a, [l.get_label() for l in _h6a], loc="upper left")
_leg6a.set_draggable(True)
_cb6 = _make_checkbox_panel(fig6, _h6a)

_in_ov = False
_ov_start = None
for _i, _row in df.iterrows():
    if _row["f_fastOvActive"] and not _in_ov:
        _ov_start = _row["t_plot"]
        _in_ov = True
    elif not _row["f_fastOvActive"] and _in_ov:
        ax6a.axvspan(_ov_start, _row["t_plot"], color="#c62828", alpha=0.08)
        _in_ov = False
if _in_ov:
    ax6a.axvspan(_ov_start, df["t_plot"].iloc[-1], color="#c62828", alpha=0.08)

# 6b: flag lanes — tight style
_flag_h6  = 0.18
_spacing6 = 0.26
for _i6, (_col, _color, _lbl) in enumerate([
    ("f_fastOvActive", "#c62828", "fastOvActive  (FastOV or iExcess)"),
    ("f_hardClamp",    "#6a1b9a", "hardClamp     (layer 2/3 — hard ceiling)"),
    ("f_iExcess",      "#00838f", "iExcess       (current excess — near target)"),
    ("f_iExcessBulk",  "#26a69a", "iExcessBulk   (current excess — bulk/CC phase)"),
    ("f_loadDump",     "#f57c00", "loadDumpActive (sudden load drop detected)"),
]):
    _off = (4 - _i6) * _spacing6
    if _col not in df.columns:
        continue
    _vals = df[_col].values
    _t    = df["t_plot"].values
    _in_f = False
    _fs   = None
    for _i in range(len(_vals)):
        if _vals[_i] and not _in_f:
            _fs = _t[_i]
            _in_f = True
        elif not _vals[_i] and _in_f:
            ax6b.barh(_off + _flag_h6 / 2, _t[_i] - _fs,
                      left=_fs, height=_flag_h6, color=_color, alpha=0.80, align="center")
            _in_f = False
    if _in_f:
        ax6b.barh(_off + _flag_h6 / 2, _t[-1] - _fs,
                  left=_fs, height=_flag_h6, color=_color, alpha=0.80, align="center")
    ax6b.text(df["t_plot"].iloc[0], _off + _flag_h6 / 2, f"  {_lbl}",
              va="center", fontsize=7, color=_color)

ax6b.set_ylim(-0.03, 4 * _spacing6 + _flag_h6 + 0.08)
ax6b.set_yticks([])
ax6b.set_ylabel("Flags", fontsize=9)
ax6b.grid(**GRID_KW)

draw_state_strip(ax6s, df, state_changes)
# save_fig(fig6, "plot6_protection")

# ===== PLOT 7 — Loop timing diagnostics (only when the timing columns are present) =====
_timing_cols = [c for c in ("ch1IntervalMs", "voltLoopIntervalMs", "inaIntervalMs") if c in df.columns]

if _timing_cols:
    fig7, (ax7, ax7s) = plt.subplots(2, 1, figsize=(16, 6),
                                     gridspec_kw={"height_ratios": [5, 1]},
                                     sharex=True)
    fig7.canvas.manager.set_window_title("Plot 7 — Loop Timing")
    fig7.suptitle("Loop Timing Diagnostics", fontsize=14, fontweight="bold")
    plt.subplots_adjust(hspace=0.05)

    if "ch1IntervalMs" in df.columns:
        ax7.plot(df["t_plot"], df["ch1IntervalMs"],
                 color="#00838f", lw=1.5, label="ch1IntervalMs (CH1 inter-sample, ms)", zorder=2)
        ax7.axhline(5,  color="#2e7d32", lw=0.8, ls=":", alpha=0.7, label="5 ms (CH1 nominal)")
        ax7.axhline(15, color="#c62828", lw=0.8, ls=":", alpha=0.7, label="15 ms (CH1 3× — stale risk)")

    if "voltLoopIntervalMs" in df.columns:
        vl_rows = df[df["voltageLoopRanThisTick"] == 1].copy()
        if not vl_rows.empty:
            ax7.scatter(vl_rows["t_plot"], vl_rows["voltLoopIntervalMs"],
                        s=20, color="#ef5350", zorder=4, label="voltLoop actual interval (fired ticks only, ms)")
        ax7.axhline(100, color="#ef5350", lw=0.8, ls=":",  alpha=0.7, label="100 ms (vLoop target)")
        ax7.axhline(200, color="#b71c1c", lw=0.8, ls="--", alpha=0.7, label="200 ms (vLoop 2×)")

    if "inaIntervalMs" in df.columns:
        ax7.plot(df["t_plot"], df["inaIntervalMs"],
                 color="#42a5f5", lw=1.0, alpha=0.8, label="inaIntervalMs (INA228 read gap, ms)", zorder=1)
        ax7.axhline(10, color="#1565c0", lw=0.8, ls=":", alpha=0.7, label="10 ms (INA228 2×)")

    ax7.set_ylabel("Interval (ms)")
    ax7.grid(True, linestyle=":", alpha=0.4)
    _leg7 = ax7.legend(loc="upper right", fontsize=10)
    _leg7.set_draggable(True)
    for t in df.loc[df["voltageLoopRanThisTick"] == 1, "t_plot"]:
        ax7s.axvline(x=t, color="#aeea00", linewidth=0.8, alpha=0.6)
    draw_state_strip(ax7s, df, state_changes)
    ax7.set_xlabel(time_label)

# ===== PLOT 8 — iExcess (Group 3) detector: averaged excess vs fire threshold E =====
# Validates IExcessTau / IExcessFloorA: mExcessEma should stay well below E on a clean high-current
# hold and cross it promptly on a real over-current.
_have_iex = ("mExcessEma" in df.columns and "iExcessThreshold" in df.columns)
if _have_iex:
    fig8, (ax8, ax8s) = plt.subplots(2, 1, figsize=(16, 6),
                                     gridspec_kw={"height_ratios": [5, 1]},
                                     sharex=True)
    fig8.canvas.manager.set_window_title("Plot 8 — iExcess Detector")
    fig8.suptitle("iExcess (Group 3) Detector — Averaged Excess vs Fire Threshold",
                  fontsize=14, fontweight="bold")
    plt.subplots_adjust(hspace=0.05)

    ax8.plot(df["t_plot"], df["mExcessEma"], color="#1565c0", lw=1.6,
             label="mExcessEma — averaged current excess (A)", zorder=3)
    ax8.plot(df["t_plot"], df["iExcessThreshold"], color="#c62828", lw=1.4, ls="--",
             label="E — fire threshold (A)", zorder=2)
    ax8.axhline(0, color="#888", lw=0.6, ls=":", alpha=0.6)

    _fire = ((df.get("f_iExcess", 0) == 1) | (df.get("f_iExcessBulk", 0) == 1))
    if hasattr(_fire, "any") and _fire.any():
        ax8.fill_between(df["t_plot"], 0, 1, where=_fire,
                         transform=ax8.get_xaxis_transform(),
                         color="#ff8a80", alpha=0.25, step="pre",
                         label="iExcess fired/latched", zorder=1)

    ax8.set_ylabel("Amps")
    ax8.grid(True, linestyle=":", alpha=0.4)
    _leg8 = ax8.legend(loc="upper right", fontsize=10)
    _leg8.set_draggable(True)
    draw_state_strip(ax8s, df, state_changes)
    ax8.set_xlabel(time_label)

# ===== Linked x-axis zoom (one primary axis per figure; sub-axes already sharex) =====
_all_primary_axes = [ax1, ax2, ax3a, ax4, ax5, ax6a]
_all_figs         = [fig1, fig2, fig3, fig4, fig5, fig6]
if _timing_cols:
    _all_primary_axes.append(ax7)
    _all_figs.append(fig7)
if _have_iex:
    _all_primary_axes.append(ax8)
    _all_figs.append(fig8)
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

fig1.subplots_adjust(bottom=0.10)

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
for _pfig in _all_figs:
    enable_pan(_pfig)

tile_figures()
plt.show()
