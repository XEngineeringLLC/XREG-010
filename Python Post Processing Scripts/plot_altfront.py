"""
plot_altfront.py
Alternator steady-state operating-point viewer for Xregulator.

Replaces plot_altmatrix.py. The old dense bucket-matrix export
(AltHealthMatrix_*.csv) is gone — the firmware now keeps a SPARSE set of
operating points instead of a fixed RPM x temp x field grid. Each point is
admitted only after the system reaches steady state, so every dot is a real
equilibrium measurement. This viewer reads that artifact: the "Alternator
Health Data *.csv" file the dashboard's Download CSV button drops in ~/Downloads.

File format (BEFRONT1, alternator, 4-axis):
  line 1:  BEFRONT1,ALT,4,<source>,rpm,exc,V,F,amps     source 0=LEARNED, 1=FIXED
  rows  :  rpm, excitation, vbus, tempF, amps, nSamp, tEmit
           rpm        engine RPM at the point
           excitation excitation proxy = (duty*Vbus)/(1+a(T_C-25)), a=0.00393
           vbus       bus voltage (V)
           tempF      alternator temperature (deg F)
           amps       steady-state output current at this condition
           nSamp      samples averaged into this point (confidence)
           tEmit      device-uptime millis when emitted (wraps ~49 days; NOT wall clock)

Two tabs (scatter + curves only — every dot is a real steady-state
measurement, nothing interpolated):

  Tab 1 - Overview
    Summary stats, coverage scatter (RPM vs temp, colour = amps, size = nSamp),
    and excitation-vs-amps scatter coloured by RPM.

  Tab 2 - Output Curve
    Output amps (Y) vs a selectable X axis (RPM / excitation / bus voltage).
    Points grouped into temperature bands; one colour + connecting line per band.
    Sliders filter RPM range, temperature range, and minimum nSamp confidence.

Run:
  cd /Users/joeceo/Documents/Arduino/Xregulator && \\
  "$HOME/Documents/Xeng_Python_Venvs/tk/bin/python" \\
  "Python Post Processing Scripts/plot_altfront.py"
"""

import glob
import os
import datetime
import tkinter as tk
from tkinter import messagebox, ttk
from filepicker import pick_file

import matplotlib
matplotlib.use("TkAgg")
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg, NavigationToolbar2Tk
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

plt.rcParams.update({
    "font.size":       9,
    "axes.titlesize":  9,
    "axes.labelsize":  8,
    "xtick.labelsize": 7,
    "ytick.labelsize": 7,
})

DOWNLOADS    = os.path.expanduser("~/Downloads")
# The dashboard Download CSV button names the file "Alternator Health Data <stamp>.csv".
FILE_KEYWORD = "Alternator Health Data"

AMP_CMAP  = matplotlib.cm.plasma
RPM_CMAP  = matplotlib.colormaps["RdYlBu_r"]
BAND_CMAP = matplotlib.colormaps["RdYlBu_r"]   # temperature bands: cool blue -> hot red
BAND_MARKERS = ['o', 's', '^', 'D', 'v', 'P', '*']

# X-axis choices for the curve tab: (column, label).
X_AXES = {
    "RPM":          ("rpm", "Engine RPM"),
    "Excitation":   ("exc", "Excitation proxy"),
    "Bus Voltage":  ("vbus", "Bus Voltage (V)"),
}


# ===== Data loader =====
def load_front(path):
    """Returns (df, source) where source is 0=LEARNED, 1=FIXED.

    Line 1 is the BEFRONT1 header (variable column count), not a data header,
    so we parse it by hand and read the rows with explicit column names.
    """
    with open(path, "r") as fh:
        first = fh.readline().strip()

    source = -1
    parts = first.split(",")
    if len(parts) >= 4 and parts[0] == "BEFRONT1":
        try:
            source = int(parts[3])
        except ValueError:
            source = -1

    cols = ["rpm", "exc", "vbus", "tempF", "amps", "nSamp", "tEmit"]
    rows = []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        fh.readline()
        for line in fh:
            p = line.strip().split(",")
            if not p[0]:
                continue
            try:
                float(p[0])
            except ValueError:
                break  # end of the BEFRONT1 block; the export appends /altdebug.csv, DEVSESS, SESSPT after it
            rows.append((p + [None] * 7)[:7])
    df = pd.DataFrame(rows, columns=cols).apply(pd.to_numeric, errors="coerce")
    # Older/cloud-pruned files may omit tEmit; fill so downstream code is uniform.
    if df["tEmit"].isna().all():
        df["tEmit"] = 0
    df = df.dropna(subset=["rpm", "exc", "vbus", "tempF", "amps"]).reset_index(drop=True)
    return df, source


def source_label(source):
    return {0: "LEARNED (device-trained)",
            1: "FIXED (loaded curve)"}.get(source, "unknown")


# ===== Tab 1 - Overview =====
def build_overview_figure(df, source, fname):
    fig = plt.figure(figsize=(13, 8))
    fig.suptitle(f"Alternator Steady-State Operating Points - {os.path.basename(fname)}",
                 fontsize=11, y=0.995)

    gs = fig.add_gridspec(1, 3, width_ratios=[1.05, 1.5, 1.5], wspace=0.30)

    # --- summary text -------------------------------------------------------
    def rng(col, unit="", fmt="{:.0f}"):
        lo, hi = df[col].min(), df[col].max()
        return f"{fmt.format(lo)} - {fmt.format(hi)}{unit}"

    lines = [
        f"File:        {os.path.basename(fname)}",
        f"Source:      {source_label(source)}",
        f"Operating points: {len(df)}",
        f"Total samples: {int(df['nSamp'].sum())}",
        "",
        f"RPM:         {rng('rpm', ' rpm')}",
        f"Temp:        {rng('tempF', ' F', '{:.0f}')}",
        f"Bus V:       {rng('vbus', ' V', '{:.2f}')}",
        f"Excitation:  {rng('exc', '', '{:.2f}')}",
        f"Output:      {rng('amps', ' A', '{:.1f}')}",
        "",
        "Each dot is a measured steady-state",
        "operating point (nothing interpolated).",
        "Larger dot = more samples (higher",
        "confidence) in that point.",
    ]
    ax_txt = fig.add_subplot(gs[0]); ax_txt.axis("off")
    ax_txt.text(0.0, 0.98, "\n".join(lines), transform=ax_txt.transAxes,
                fontsize=8, va="top", family="monospace",
                bbox=dict(boxstyle="round", facecolor="#f0f0f0", alpha=0.85))

    # --- coverage scatter: RPM vs temp, colour = amps, size = nSamp ---------
    ax1 = fig.add_subplot(gs[1])
    ns = df["nSamp"].clip(lower=1)
    sizes = 18 + 90 * (ns / ns.max())
    sc1 = ax1.scatter(df["rpm"], df["tempF"], c=df["amps"], s=sizes,
                      cmap=AMP_CMAP, edgecolor="#333", linewidth=0.4, alpha=0.9)
    ax1.set_xlabel("Engine RPM"); ax1.set_ylabel("Alternator Temp (F)")
    ax1.set_title("Coverage - where the alternator is characterized")
    ax1.grid(True, linestyle="--", alpha=0.4)
    fig.colorbar(sc1, ax=ax1, label="Output (A)", fraction=0.046, pad=0.04)

    # --- excitation vs amps, colour = RPM -----------------------------------
    ax2 = fig.add_subplot(gs[2])
    sc2 = ax2.scatter(df["exc"], df["amps"], c=df["rpm"], s=sizes,
                      cmap=RPM_CMAP, edgecolor="#333", linewidth=0.4, alpha=0.9)
    ax2.set_xlabel("Excitation proxy"); ax2.set_ylabel("Output (A)")
    ax2.set_title("Output vs excitation (colour = RPM)")
    ax2.grid(True, linestyle="--", alpha=0.4)
    fig.colorbar(sc2, ax=ax2, label="RPM", fraction=0.046, pad=0.04)

    fig.tight_layout(rect=[0, 0, 1, 0.97])
    return fig


# ===== Main window =====
class FrontViewer:
    def __init__(self, df, source, fname):
        self.df     = df
        self.source = source
        self.fname  = fname

        self.rpm_min, self.rpm_max   = float(df["rpm"].min()),   float(df["rpm"].max())
        self.temp_min, self.temp_max = float(df["tempF"].min()), float(df["tempF"].max())
        self.ns_max  = int(df["nSamp"].max())

        self.root = tk.Tk()
        try:
            self.root.tk.call("::tk::unsupported::MacWindowStyle", "appearance",
                              self.root._w, "NSAppearanceNameAqua")
        except Exception:
            pass
        self.root.title(f"Alternator Steady-State Operating Points - {os.path.basename(fname)}")
        self.root.geometry("1400x900")
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

        nb = ttk.Notebook(self.root)
        nb.pack(fill="both", expand=True)

        tab1 = ttk.Frame(nb); nb.add(tab1, text="   Overview   ")
        self._build_overview_tab(tab1)

        tab2 = ttk.Frame(nb); nb.add(tab2, text="   Output Curve   ")
        self._build_curve_tab(tab2)

        self.root.mainloop()

    def _on_close(self):
        plt.close("all")
        self.root.destroy()

    # --- Tab 1 --------------------------------------------------------------
    def _build_overview_tab(self, parent):
        fig    = build_overview_figure(self.df, self.source, self.fname)
        canvas = FigureCanvasTkAgg(fig, master=parent)
        canvas.draw()
        tb = NavigationToolbar2Tk(canvas, parent); tb.update()
        tb.pack(side="top", fill="x")
        canvas.get_tk_widget().pack(fill="both", expand=True)

    # --- Tab 2 --------------------------------------------------------------
    def _build_curve_tab(self, parent):
        fig_frame = tk.Frame(parent)
        fig_frame.pack(fill="both", expand=True)

        self.fc_fig, self.fc_ax = plt.subplots(figsize=(13, 6))
        self.fc_canvas = FigureCanvasTkAgg(self.fc_fig, master=fig_frame)
        self.fc_canvas.draw()
        tb = NavigationToolbar2Tk(self.fc_canvas, fig_frame); tb.update()
        tb.pack(side="top", fill="x")
        self.fc_canvas.get_tk_widget().pack(fill="both", expand=True)

        ctrl = tk.Frame(parent, bg="#ebebeb", relief="ridge", bd=1)
        ctrl.pack(fill="x", padx=6, pady=(2, 6))

        tk.Label(ctrl, text="Filters", bg="#ebebeb",
                 font=("Helvetica", 9, "bold")).grid(
            row=0, column=0, columnspan=12, sticky="w", padx=8, pady=(5, 2))

        self.x_choice  = tk.StringVar(value="RPM")
        self.n_bands   = tk.IntVar(value=5)
        self.rpm_lo    = tk.DoubleVar(value=self.rpm_min)
        self.rpm_hi    = tk.DoubleVar(value=self.rpm_max)
        self.temp_lo   = tk.DoubleVar(value=self.temp_min)
        self.temp_hi   = tk.DoubleVar(value=self.temp_max)
        self.min_ns    = tk.IntVar(value=1)
        self.connect   = tk.BooleanVar(value=True)

        def redraw(*_):
            self._redraw_curve()

        tk.Label(ctrl, text="X axis:", bg="#ebebeb", font=("Helvetica", 8),
                 anchor="e", width=11).grid(row=1, column=0, sticky="e", padx=(8, 2))
        xrow = tk.Frame(ctrl, bg="#ebebeb")
        xrow.grid(row=1, column=1, columnspan=3, sticky="w")
        for name in X_AXES:
            tk.Radiobutton(xrow, text=name, variable=self.x_choice, value=name,
                           command=redraw, bg="#ebebeb",
                           font=("Helvetica", 8)).pack(side="left", padx=3)

        def slider(row, col, label, var, lo, hi, res=1):
            tk.Label(ctrl, text=label, bg="#ebebeb", font=("Helvetica", 8),
                     anchor="e", width=13).grid(row=row, column=col, sticky="e", padx=(8, 2))
            tk.Scale(ctrl, variable=var, from_=lo, to=hi, orient="horizontal",
                     length=170, showvalue=True, command=redraw, resolution=res,
                     bg="#ebebeb", highlightthickness=0,
                     font=("Helvetica", 7)).grid(row=row, column=col+1, padx=2, pady=2)

        slider(2, 0, "RPM min:",  self.rpm_lo,  self.rpm_min,  self.rpm_max, 10)
        slider(2, 2, "RPM max:",  self.rpm_hi,  self.rpm_min,  self.rpm_max, 10)
        slider(3, 0, "Temp min F:", self.temp_lo, self.temp_min, self.temp_max, 1)
        slider(3, 2, "Temp max F:", self.temp_hi, self.temp_min, self.temp_max, 1)
        slider(2, 4, "Temp bands:", self.n_bands, 1, min(7, max(1, len(self.df))), 1)
        slider(3, 4, "Min nSamp:",  self.min_ns,  1, max(1, self.ns_max), 1)

        tk.Checkbutton(ctrl, text="Connect points", variable=self.connect,
                       command=redraw, bg="#ebebeb", font=("Helvetica", 8)).grid(
            row=1, column=4, columnspan=2, sticky="w", padx=12)

        self._redraw_curve()

    def _redraw_curve(self):
        xcol, xlabel = X_AXES[self.x_choice.get()]
        rpm_lo  = self.rpm_lo.get();  rpm_hi  = max(rpm_lo, self.rpm_hi.get())
        temp_lo = self.temp_lo.get(); temp_hi = max(temp_lo, self.temp_hi.get())
        nbands  = max(1, self.n_bands.get())
        min_ns  = max(1, self.min_ns.get())
        connect = self.connect.get()

        ax = self.fc_ax; ax.cla()

        sel = self.df[
            (self.df["rpm"]   >= rpm_lo)  & (self.df["rpm"]   <= rpm_hi) &
            (self.df["tempF"] >= temp_lo) & (self.df["tempF"] <= temp_hi) &
            (self.df["nSamp"] >= min_ns)
        ].copy()

        n_lines = 0
        handles = []
        if not sel.empty:
            # Split the selected temperature span into equal-width bands.
            tmin, tmax = sel["tempF"].min(), sel["tempF"].max()
            if nbands == 1 or tmax <= tmin:
                edges = np.array([tmin, tmax + 1e-6])
                nbands = 1
            else:
                edges = np.linspace(tmin, tmax, nbands + 1)

            for b in range(nbands):
                lo, hi = edges[b], edges[b + 1]
                if b == nbands - 1:
                    band = sel[(sel["tempF"] >= lo) & (sel["tempF"] <= hi)]
                else:
                    band = sel[(sel["tempF"] >= lo) & (sel["tempF"] < hi)]
                if band.empty:
                    continue
                band = band.sort_values(xcol)
                color  = BAND_CMAP(b / max(nbands - 1, 1))
                marker = BAND_MARKERS[b % len(BAND_MARKERS)]
                ns = band["nSamp"].clip(lower=1)
                sizes = 24 + 70 * (ns / max(1, self.ns_max))
                lbl = f"{lo:.0f}-{hi:.0f} F  (n={len(band)})"
                ax.scatter(band[xcol], band["amps"], color=color, marker=marker,
                           s=sizes, edgecolor="#222", linewidth=0.4,
                           zorder=3, label=lbl)
                if connect and len(band) > 1:
                    ln, = ax.plot(band[xcol], band["amps"], color=color,
                                  linewidth=1.6, alpha=0.85, zorder=2, label="_nolegend_")
                # one legend handle per band (use a proxy line for clarity)
                handles.append(plt.Line2D([0], [0], color=color, marker=marker,
                                          linewidth=1.6, markersize=7, label=lbl))
                n_lines += 1

        ax.set_xlabel(xlabel, fontsize=11)
        ax.set_ylabel("Output (A)", fontsize=11)
        ax.set_title(f"Output Amps vs {self.x_choice.get()}  -  "
                     f"{source_label(self.source)}", fontsize=10, pad=18)
        ax.text(0.5, 1.005,
                "Each marker is a real steady-state point  -  colour = temperature band  -  "
                "dot size = sample count (confidence)",
                transform=ax.transAxes, ha="center", va="bottom",
                fontsize=7.5, color="#555555", style="italic")

        if n_lines > 0:
            ax.legend(handles=handles, loc="best", fontsize=7,
                      ncol=max(1, n_lines // 6), framealpha=0.9,
                      title="temperature band", title_fontsize=7)
        else:
            ax.text(0.5, 0.5,
                    "No points in selected range.\n"
                    "Widen the RPM / Temp sliders or lower Min nSamp.",
                    ha="center", va="center", transform=ax.transAxes,
                    fontsize=11, color="#888888")

        ax.grid(True, linestyle="--", alpha=0.45)
        ax.set_xlim(left=0)
        ax.set_ylim(bottom=0)
        self.fc_fig.subplots_adjust(top=0.88, bottom=0.12)
        self.fc_canvas.draw_idle()


# ===== Entry point =====
if __name__ == "__main__":
    path = pick_file(prefix="Alternator Health Data", title="Select Alternator Health Export", reminder="Select health export to view:")
    if not path:
        raise SystemExit

    df, source = load_front(path)
    if df.empty:
        print("File loaded but contains no operating points.")
        raise SystemExit

    FrontViewer(df, source, path)
