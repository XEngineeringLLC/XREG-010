"""
plot_thermallog.py
- File selector GUI (tkinter) — pick any *.csv from Downloads, newest first
- 3 plot windows, each with a state strip below for binary/low-integer signals
- PNGs saved to Downloads alongside the source CSV
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
path = pick_file(prefix="thermallog_", title="Select Thermal Log")
if not path:
    raise SystemExit("No file selected.")

basename = os.path.splitext(os.path.basename(path))[0]
print(f"Loading: {path}")

constants = {}
with open(path) as f:
    for line in f:
        if line.startswith("CONST"):
            for token in line.strip().split(",")[1:]:
                if "=" in token:
                    k, v = token.split("=")
                    constants[k.strip()] = float(v.strip())
            break

kp       = constants.get("kp",       float("nan"))
ki       = constants.get("ki",       float("nan"))
lookahead = constants.get("lookahead", float("nan"))
# CONST keys absent from older logs read as NaN; the code below skips on NaN.
temp_limit = constants.get("limit", float("nan"))
temp_warn  = constants.get("warn",  float("nan"))
temp_crit  = constants.get("crit",  float("nan"))
# kidownfrac: hybrid asymmetric below-setpoint integral bleed ratio.
kidownfrac = constants.get("kidownfrac", float("nan"))
# slopewin: slope backward-difference window (s).
slopewin = constants.get("slopewin", float("nan"))
const_label = f"Kp={kp}  Ki={ki}  Lookahead={lookahead}s"
if slopewin == slopewin:  # not NaN
    const_label += f"  SlopeWin={slopewin:g}s"
if kidownfrac == kidownfrac:  # not NaN
    const_label += f"  KiDownFrac={kidownfrac:g}"
if temp_limit == temp_limit:  # not NaN
    const_label += f"  Limit={temp_limit:.0f}°F"
print(f"Constants: {const_label}")

# Load data rows — parse raw lines first so the trimmer has _lines/_header_idx/_col_names
with open(path, encoding="utf-8", errors="replace") as _f:
    _lines = _f.readlines()
_header_idx  = None
_header_line = None
for _i, _raw in enumerate(_lines):
    if _raw.strip().startswith("ts_ms"):
        _header_idx  = _i
        _header_line = _raw.strip()
        break
if _header_idx is None:
    raise SystemExit(f"ERROR: No 'ts_ms' header row found in {path}.")
_sep_th = "," if "\t" not in _header_line else "\t"
_col_names = [c.strip() for c in _header_line.split(_sep_th)]
from io import StringIO as _SI_th
_th_data = "".join(_lines[_header_idx + 1:])
df = pd.read_csv(_SI_th(_th_data), sep=_sep_th, names=_col_names, on_bad_lines="skip")

numeric_cols = [
    "ts_ms", "tempFilt_F", "tempProj_F", "nominalTarget_A", "rpmCap_A",
    "voltCap_A", "uTarget_A", "spLimited_A", "pidErr_A", "pidOut_pct",
    "duty_pct", "RPM", "battV", "measAmps_A", "penaltyAmps_A", "flags",
    "chargeStageDisplay", "outerP", "outerI", "lookahead", "impliedPenalty",
    "antiWindupFired", "thermalSlope_F_sec",
    "freezeWhy", "penaltyRaw_A", "iCeil_A"  # iCeil<outerI = clamp deleting earned hold
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

flags = pd.to_numeric(df["flags"], errors="coerce").fillna(0).astype("int64")
df["chargeStageDisplay"] = pd.to_numeric(df["chargeStageDisplay"], errors="coerce").fillna(0).astype("int64")

df["f_tempPIDActive"] = ((flags // (2**0)) % 2).astype(int)
df["f_autoMode"]      = ((flags // (2**4)) % 2).astype(int)
df["f_shutdown"]      = ((flags // (2**5)) % 2).astype(int)

# ===== Mode-change events =====
THERMAL_MODE_COLORS = {
    "shutdown":   "#c62828",
    "bulk":       "#2e7d32",
    "absorption": "#6a1b9a",
    "float":      "#e91e63",
    "manual":     "#c62828",
    "maintain":   "#388e3c",
    "targetV":    "#1565c0",
    "idle":       "#777777",
    "commissioning": "#795548",
    "antiWindup": "#c62828",
}

state_changes = df.index[
    (df["flags"] != df["flags"].shift()) |
    (df["chargeStageDisplay"] != df["chargeStageDisplay"].shift())
].tolist()
if state_changes and state_changes[0] == 0:
    state_changes = state_changes[1:]

def mode_label_color(row):
    if row["f_shutdown"]:
        return "SHUTDOWN", THERMAL_MODE_COLORS["shutdown"]

    stage = int(row["chargeStageDisplay"])

    if stage == 1:
        return "BULK", THERMAL_MODE_COLORS["bulk"]
    if stage == 2:
        return "ABSORPTION", THERMAL_MODE_COLORS["absorption"]
    if stage == 3:
        return "FLOAT", THERMAL_MODE_COLORS["float"]
    if stage == 4:
        return "MANUAL", THERMAL_MODE_COLORS["manual"]
    if stage == 5:
        return "MAINTAIN", THERMAL_MODE_COLORS["maintain"]
    if stage == 6:
        return "TARGET V", THERMAL_MODE_COLORS["targetV"]
    if stage == 8:
        return "COMMISSIONING", THERMAL_MODE_COLORS["commissioning"]

    return "IDLE", THERMAL_MODE_COLORS["idle"]

# ===== State strip: mode timeline bar + antiWindupFired red ticks =====
def draw_state_strip(ax, df, state_changes):
    ax.set_ylim(0, 3)
    ax.set_yticks([])
    ax.set_xlim(df["t_plot"].iloc[0], df["t_plot"].iloc[-1])
    ax.set_xlabel(time_label, fontsize=13)

    boundaries = state_changes + [len(df) - 1]
    prev_t = df["t_plot"].iloc[0]
    prev_row = df.iloc[0]

    for idx in boundaries:
        t = df.loc[idx, "t_plot"] if idx < len(df) else df["t_plot"].iloc[-1]
        label, color = mode_label_color(prev_row)
        width = t - prev_t
        ax.barh(2, width, left=prev_t, height=0.7, color=color, alpha=0.85, align="center")
        if width > (df["t_plot"].iloc[-1] - df["t_plot"].iloc[0]) * 0.03:
            ax.text(prev_t + width / 2, 2, label,
                    ha="center", va="center",
                    fontsize=8, color="white", fontweight="bold", clip_on=True)
        if idx < len(df):
            prev_row = df.loc[idx]
        prev_t = t

    aw = df[df["antiWindupFired"] == 1]
    for t in aw["t_plot"]:
        ax.axvline(x=t, ymin=0, ymax=0.38, color=THERMAL_MODE_COLORS["antiWindup"], linewidth=1.5, alpha=0.9)

    if not aw.empty:
        span = df["t_plot"].iloc[-1] - df["t_plot"].iloc[0]
        ax.text(df["t_plot"].iloc[0] + span * 0.01, 0.5, "antiWindup ▲",
                fontsize=8, color=THERMAL_MODE_COLORS["antiWindup"], va="center")

    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.spines["left"].set_visible(False)


def add_mode_vlines(ax, df, state_changes):
    for idx in state_changes:
        t = df.loc[idx, "t_plot"]
        _, color = mode_label_color(df.loc[idx])
        ax.axvline(x=t, color=color, linewidth=1.0, linestyle="--", alpha=0.5)


def save_fig(fig, suffix):
    out = os.path.join(DOWNLOADS, f"{basename}_{suffix}.png")
    fig.savefig(out, dpi=150, bbox_inches="tight")
    print(f"Saved: {out}")


# ===== Shared style =====
GRID_KW = dict(alpha=0.4, linewidth=0.7)

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

# ===== PLOT 1 — Temperature control & penalty output =====
fig1 = plt.figure(figsize=(16, 8), num="Plot 1 — Temp Control & Penalty")
gs1  = gridspec.GridSpec(2, 1, height_ratios=[5, 1], hspace=0.08)
ax1a = fig1.add_subplot(gs1[0])
ax1b = ax1a.twinx()
ax1s = fig1.add_subplot(gs1[1], sharex=ax1a)
fig1.subplots_adjust(right=0.80, bottom=0.10)

ax1a.plot(df["t_plot"], df["tempFilt_F"],  color="#c62828", lw=2.5, label="tempFilt_F (measured)")
ax1a.plot(df["t_plot"], df["tempProj_F"], color="#e91e63", lw=2.2, linestyle="--", label="tempProj_F (PID input)")
# nominalTarget column carries the live thermal regulation setpoint in °F (limit−5 steady /
# limit−20 warmup) despite its legacy "_A" header name — plot it on the temperature axis.
if "nominalTarget_A" in df.columns:
    ax1a.plot(df["t_plot"], df["nominalTarget_A"], color="#1565c0", lw=2.0, linestyle=":",
              label="regulation target (°F)")
# Protection reference lines from the CONST row (skipped if the log predates limit logging).
if temp_limit == temp_limit:
    ax1a.axhline(temp_limit, color="#ef6c00", lw=1.8, linestyle="-",
                 label=f"limit ({temp_limit:.0f}°F)")
    if temp_warn == temp_warn:
        ax1a.axhline(temp_limit + temp_warn, color="#b71c1c", lw=1.6, linestyle="--",
                     label=f"warning trip (+{temp_warn:.0f}°F)")
    if temp_crit == temp_crit:
        ax1a.axhline(temp_limit + temp_crit, color="#4a148c", lw=1.4, linestyle="-.",
                     label=f"critical trip (+{temp_crit:.0f}°F)")
ax1b.plot(df["t_plot"], df["penaltyAmps_A"], color="#2e7d32", lw=2.2, label="penaltyAmps_A")

ax1a.set_ylabel("Temperature (°F)", color="#c62828")
ax1b.set_ylabel("Penalty Amps (A)", color="#2e7d32")
ax1a.grid(**GRID_KW)
plt.setp(ax1a.get_xticklabels(), visible=False)

lines1  = ax1a.get_lines() + ax1b.get_lines()
labels1 = [l.get_label() for l in lines1]
_leg1 = ax1b.legend(lines1, labels1, loc="upper left")
_leg1.set_draggable(True)
_cb1 = _make_checkbox_panel(fig1, lines1)

add_mode_vlines(ax1a, df, state_changes)
draw_state_strip(ax1s, df, state_changes)
fig1.suptitle(f"Temp Control & Penalty  |  {const_label}")
# save_fig(fig1, "plot1_temp")

# ===== PLOT 2 — PID term decomposition =====
fig2 = plt.figure(figsize=(16, 8), num="Plot 2 — PID Term Decomposition")
gs2  = gridspec.GridSpec(2, 1, height_ratios=[5, 1], hspace=0.08)
ax2  = fig2.add_subplot(gs2[0])
ax2s = fig2.add_subplot(gs2[1], sharex=ax2)
fig2.subplots_adjust(right=0.80)

# Sign flipped to match the dashboard: CSV + = cut amps, plotted + = more amps. Logged P includes the
# look-ahead, so subtracting it makes present + lookahead + integrator = total. thermalSlope (°F/s) keeps its sign.
ax2.plot(df["t_plot"], -(df["outerP"] - df["lookahead"]), color="#1565c0", lw=2.2, label="present temp (P)")
ax2.plot(df["t_plot"], -df["lookahead"],       color="#2e7d32", lw=2.2, label="look-ahead")
ax2.plot(df["t_plot"], -df["outerI"],          color="#f9a825", lw=2.2, label="integrator (I)")
# Integrator pushed DOWN onto this line = the I<=cap−FF clamp deleting earned holding integral (RPM dip /
# FF spike); expect a reheat on recovery.
if "iCeil_A" in df.columns and df["iCeil_A"].notna().any():
    ax2.plot(df["t_plot"], -df["iCeil_A"], color="#9c27b0", lw=1.3, linestyle="--",
             alpha=0.8, label="integral ceiling (cap−FF)")
ax2.plot(df["t_plot"], df["thermalSlope_F_sec"], color="#00838f", lw=2.0, linestyle=":", label="thermalSlope_F_sec")

ax2.set_ylabel("Effect on Current Target (A)")
ax2.grid(**GRID_KW)

# Direction bands + corner watermarks, same scheme as the dashboard: teal above zero =
# more amps allowed, red below = amps being cut. axhspan joins autoscale, so re-pin ylim.
_y0, _y1 = ax2.get_ylim()
ax2.axhspan(0, max(_y1, 0), facecolor="#00a19a", alpha=0.06, zorder=0)
ax2.axhspan(min(_y0, 0), 0, facecolor="#d62728", alpha=0.06, zorder=0)
ax2.set_ylim(_y0, _y1)
ax2.text(0.99, 0.97, "↑ MORE AMPS", transform=ax2.transAxes, ha="right", va="top",
         color="#666", alpha=0.6, fontsize=12, fontweight="bold", family="monospace")
ax2.text(0.99, 0.03, "↓ LESS AMPS", transform=ax2.transAxes, ha="right", va="bottom",
         color="#666", alpha=0.6, fontsize=12, fontweight="bold", family="monospace")
_h2, _ = ax2.get_legend_handles_labels()
_leg2 = ax2.legend(loc="upper left")
_leg2.set_draggable(True)
_h2_lines = [l for l in ax2.get_lines() if not l.get_label().startswith("_")]
_cb2 = _make_checkbox_panel(fig2, _h2_lines)
plt.setp(ax2.get_xticklabels(), visible=False)

add_mode_vlines(ax2, df, state_changes)
draw_state_strip(ax2s, df, state_changes)
fig2.suptitle(f"PID Term Decomposition  |  {const_label}")
# save_fig(fig2, "plot2_pid")

# ===== PLOT 3 — Constraint context =====
fig3 = plt.figure(figsize=(16, 8), num="Plot 3 — Constraint Context")
gs3  = gridspec.GridSpec(2, 1, height_ratios=[5, 1], hspace=0.08)
ax3  = fig3.add_subplot(gs3[0])
ax3s = fig3.add_subplot(gs3[1], sharex=ax3)
fig3.subplots_adjust(right=0.80)

ax3.plot(df["t_plot"], df["measAmps_A"],  color="#c62828", lw=2.5, label="measAmps_A")
ax3.plot(df["t_plot"], df["uTarget_A"],   color="#1565c0", lw=2.2, label="uTarget_A")
ax3.plot(df["t_plot"], df["spLimited_A"], color="#e91e63", lw=2.2, linestyle="--", label="spLimited_A")
ax3.plot(df["t_plot"], df["voltCap_A"],   color="#6a1b9a", lw=2.0, linestyle=":", label="voltCap_A")
ax3.plot(df["t_plot"], df["duty_pct"],
         color="#455a64", lw=1.8, linestyle="-.", label="duty_pct")

ax3.set_ylabel("Amps (A)")
ax3.grid(**GRID_KW)
_h3, _ = ax3.get_legend_handles_labels()
_leg3 = ax3.legend(loc="upper left")
_leg3.set_draggable(True)
_h3_lines = [l for l in ax3.get_lines() if not l.get_label().startswith("_")]
_cb3 = _make_checkbox_panel(fig3, _h3_lines)
plt.setp(ax3.get_xticklabels(), visible=False)

add_mode_vlines(ax3, df, state_changes)
draw_state_strip(ax3s, df, state_changes)
fig3.suptitle(f"Constraint Context  |  {const_label}")
# save_fig(fig3, "plot3_constraints")

# ===== PLOT 4 — Battery Voltage & RPM =====
fig4 = plt.figure(figsize=(16, 8), num="Plot 4 — Battery Voltage & RPM")
gs4  = gridspec.GridSpec(2, 1, height_ratios=[5, 1], hspace=0.08)
ax4a = fig4.add_subplot(gs4[0])
ax4b = ax4a.twinx()
ax4s = fig4.add_subplot(gs4[1], sharex=ax4a)
fig4.subplots_adjust(right=0.80)

ax4a.plot(df["t_plot"], df["battV"],
          color="#1565c0", lw=2.5, label="battV")

ax4b.plot(df["t_plot"], df["RPM"] / 100.0,
          color="#f9a825", lw=2.2, label="RPM/100")

ax4a.set_ylabel("Battery Voltage (V)", color="#1565c0")
ax4b.set_ylabel("RPM / 100", color="#f9a825")

ax4a.grid(**GRID_KW)
plt.setp(ax4a.get_xticklabels(), visible=False)

lines4  = ax4a.get_lines() + ax4b.get_lines()
labels4 = [l.get_label() for l in lines4]
_leg4 = ax4b.legend(lines4, labels4, loc="upper left")
_leg4.set_draggable(True)
_cb4 = _make_checkbox_panel(fig4, lines4)

add_mode_vlines(ax4a, df, state_changes)
draw_state_strip(ax4s, df, state_changes)

fig4.suptitle(f"Battery Voltage & RPM  |  {const_label}")
# save_fig(fig4, "plot4_batt_rpm")

# ===== Linked x-axis zoom (one primary axis per figure; sub-axes already sharex) =====
_all_primary_axes = [ax1a, ax2, ax3, ax4a]
_all_figs         = [fig1, fig2, fig3, fig4]
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
        # The firmware writes CONST on the line AFTER the header, so the parse above read it as
        # a data row (ts_ms "CONST" -> NaN) and dropped it.
        for _tln in _lines[_header_idx + 1:]:
            if _tln.startswith("CONST"):
                _tf.write(_tln.rstrip("\r\n") + "\n")
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
