# Terminal command:  RunAltSpeed
# Launch script for alternator stator pulse RPM + FFT analysis

import math
import sys
import subprocess
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
from collections import Counter


# ===== User-tunable assumptions =====
ALT_POLE_PAIRS = 6          # bog-standard 12-pole automotive alternator
PLOT_SAMPLES = 5000         # first N raw samples to show in sanity-check plot
FLAT_KEEP_FRACTION = 0.90   # use middle 90% of each flat
EDGE_HYST_FRACTION = 0.20   # hysteresis band as fraction of span
MIN_HIGH_SAMPLES = 8        # reject pulses with tiny high flat
MIN_LOW_SAMPLES = 8         # reject pulses with tiny low flat
MIN_PULSE_SAMPLES = 8       # reject absurdly short full pulses
MAX_PULSE_SAMPLES = None    # None = no upper bound
SHOW_DEBUG_PRINTS = True


# ===== File selection (macOS) =====
def pick_csv_file() -> str:
    script = """
    set theFile to choose file with prompt "Select CSV file"
    POSIX path of theFile
    """
    result = subprocess.run(
        ["osascript", "-e", script],
        capture_output=True,
        text=True
    )
    if result.returncode != 0:
        return ""
    return result.stdout.strip()


# ===== Helpers =====
def robust_mode(values: np.ndarray) -> float:
    """
    Return the most common exact sample value.

    This exploits coarse ADC quantization intentionally.  The stator signal
    sits on one or two repeated ADC codes during each flat.  The mode directly
    answers "what code did the ADC report most of the time here?" which is the
    most physically honest plateau estimate for this kind of waveform.  The
    mean gets pulled by edge samples and any residual ringing leaking into the
    window; the median reflects the full distribution rather than the dominant
    code.  For timing extraction from a quantized pulse train the mode is the
    right choice.
    """
    if len(values) == 0:
        return np.nan
    counts = Counter(values.tolist())
    mode_value, _ = counts.most_common(1)[0]
    return float(mode_value)


def middle_fraction_indices(n: int, keep_fraction: float) -> tuple[int, int]:
    """
    Return [start, end) indices for the centered middle keep_fraction of a segment.
    Example: n=100, keep=0.90 -> keep indices 5..94
    """
    if n <= 0:
        return 0, 0
    trim_each_side = (1.0 - keep_fraction) / 2.0
    start = int(math.floor(n * trim_each_side))
    end = int(math.ceil(n * (1.0 - trim_each_side)))
    start = max(0, min(start, n))
    end = max(start, min(end, n))
    return start, end


def interp_crossing_time(t1: float, v1: float, t2: float, v2: float, threshold: float) -> float:
    if v2 == v1:
        return 0.5 * (t1 + t2)
    return t1 + (threshold - v1) * (t2 - t1) / (v2 - v1)


# ===== Pulse segmentation =====
def segment_signal_into_states(time_s: np.ndarray, volts: np.ndarray):
    """
    Segment waveform into alternating low/high flats using Schmitt-style hysteresis.
    Thresholds are global only for coarse segmentation.
    Per-pulse thresholds are computed later from local flats.

    Returns:
      segments: list of dicts with keys state, start_idx, end_idx
      coarse_low, coarse_high, coarse_mid, low_thresh, high_thresh
    """
    coarse_low = np.percentile(volts, 5)
    coarse_high = np.percentile(volts, 95)
    coarse_mid = 0.5 * (coarse_low + coarse_high)
    span = coarse_high - coarse_low
    if span <= 0:
        raise ValueError("Signal span is zero or negative. Cannot segment waveform.")

    band = EDGE_HYST_FRACTION * span
    low_thresh = coarse_mid - 0.5 * band
    high_thresh = coarse_mid + 0.5 * band

    state = "high" if volts[0] >= high_thresh else "low"
    segments = []
    seg_start = 0

    for i in range(1, len(volts)):
        v = volts[i]
        if state == "low":
            if v >= high_thresh:
                segments.append({"state": "low", "start_idx": seg_start, "end_idx": i})
                seg_start = i
                state = "high"
        else:
            if v <= low_thresh:
                segments.append({"state": "high", "start_idx": seg_start, "end_idx": i})
                seg_start = i
                state = "low"

    segments.append({"state": state, "start_idx": seg_start, "end_idx": len(volts)})
    return segments, coarse_low, coarse_high, coarse_mid, low_thresh, high_thresh


# ===== Pulse extraction =====
def extract_rising_pulses(time_s: np.ndarray, volts: np.ndarray, segments: list[dict]):
    """
    Use low/high/low triplets.
    For each pulse:
      - use middle 90% of preceding low flat
      - use middle 90% of high flat
      - low level  = mode(low flat kept samples)   -- see robust_mode() docstring
      - high level = mode(high flat kept samples)
      - midpoint   = (low + high) / 2
      - crossing   = interpolated time where rising edge crosses midpoint

    Returns list of pulse dicts.
    """
    pulses = []

    for i in range(len(segments) - 2):
        seg_low  = segments[i]
        seg_high = segments[i + 1]
        seg_low2 = segments[i + 2]

        if not (seg_low["state"] == "low" and
                seg_high["state"] == "high" and
                seg_low2["state"] == "low"):
            continue

        low_n          = seg_low["end_idx"]  - seg_low["start_idx"]
        high_n         = seg_high["end_idx"] - seg_high["start_idx"]
        full_pulse_n   = seg_low2["start_idx"] - seg_low["start_idx"]

        if low_n        < MIN_LOW_SAMPLES:    continue
        if high_n       < MIN_HIGH_SAMPLES:   continue
        if full_pulse_n < MIN_PULSE_SAMPLES:  continue
        if MAX_PULSE_SAMPLES is not None and full_pulse_n > MAX_PULSE_SAMPLES:
            continue

        l0 = seg_low["start_idx"]
        low_start_rel, low_end_rel = middle_fraction_indices(low_n, FLAT_KEEP_FRACTION)
        low_keep_start = l0 + low_start_rel
        low_keep_end   = l0 + low_end_rel

        h0 = seg_high["start_idx"]
        high_start_rel, high_end_rel = middle_fraction_indices(high_n, FLAT_KEEP_FRACTION)
        high_keep_start = h0 + high_start_rel
        high_keep_end   = h0 + high_end_rel

        low_flat_vals  = volts[low_keep_start:low_keep_end]
        high_flat_vals = volts[high_keep_start:high_keep_end]

        if len(low_flat_vals) == 0 or len(high_flat_vals) == 0:
            continue

        low_level  = robust_mode(low_flat_vals)
        high_level = robust_mode(high_flat_vals)
        midpoint   = 0.5 * (low_level + high_level)

        # Find rising crossing between end of low flat and early part of high segment.
        search_start = max(seg_low["start_idx"], seg_low["end_idx"] - 3)
        search_end   = min(len(volts) - 1, seg_high["start_idx"] + max(3, high_n // 3))

        crossing_idx  = None
        crossing_time = np.nan

        for k in range(search_start, search_end):
            if volts[k] < midpoint <= volts[k + 1]:
                crossing_idx  = k
                crossing_time = interp_crossing_time(
                    time_s[k], volts[k], time_s[k + 1], volts[k + 1], midpoint
                )
                break

        if crossing_idx is None:
            # Broader fallback across the low->high boundary
            search_start = max(0, seg_low["end_idx"] - 10)
            search_end   = min(len(volts) - 1, seg_high["end_idx"])
            for k in range(search_start, search_end):
                if volts[k] < midpoint <= volts[k + 1]:
                    crossing_idx  = k
                    crossing_time = interp_crossing_time(
                        time_s[k], volts[k], time_s[k + 1], volts[k + 1], midpoint
                    )
                    break

        if crossing_idx is None:
            continue

        pulses.append({
            "pulse_index":        len(pulses) + 1,
            "low_seg_start_idx":  seg_low["start_idx"],
            "low_seg_end_idx":    seg_low["end_idx"],
            "high_seg_start_idx": seg_high["start_idx"],
            "high_seg_end_idx":   seg_high["end_idx"],
            "low_keep_start_idx": low_keep_start,
            "low_keep_end_idx":   low_keep_end,
            "high_keep_start_idx":high_keep_start,
            "high_keep_end_idx":  high_keep_end,
            "low_level":          low_level,
            "high_level":         high_level,
            "midpoint":           midpoint,
            "crossing_idx":       crossing_idx,
            "crossing_time_s":    crossing_time,
        })

    return pulses


# ===== RPM conversion =====
def pulses_to_rpm(pulses: list[dict], pole_pairs: int) -> pd.DataFrame:
    """
    For a single stator tap on a typical alternator phase:
      electrical cycles per mechanical rev = pole pairs
      RPM = 60 * f_pulse / pole_pairs
    """
    if len(pulses) < 2:
        return pd.DataFrame(columns=["time_s", "dt_s", "pulse_freq_hz", "alt_rpm"])

    crossing_times = np.array([p["crossing_time_s"] for p in pulses], dtype=float)
    dt             = np.diff(crossing_times)
    valid          = dt > 0
    time_mid       = 0.5 * (crossing_times[:-1] + crossing_times[1:])

    pulse_freq = np.full_like(dt, np.nan)
    alt_rpm    = np.full_like(dt, np.nan)
    pulse_freq[valid] = 1.0 / dt[valid]
    alt_rpm[valid]    = 60.0 * pulse_freq[valid] / pole_pairs

    return pd.DataFrame({
        "time_s":       time_mid,
        "dt_s":         dt,
        "pulse_freq_hz":pulse_freq,
        "alt_rpm":      alt_rpm,
    })


# ===== Plotting =====
def plot_sanity_check(time_s: np.ndarray, volts: np.ndarray,
                      pulses: list[dict], plot_samples: int):
    n           = min(plot_samples, len(time_s))
    time_plot   = time_s[:n]
    volts_plot  = volts[:n]

    fig, ax = plt.subplots(figsize=(14, 7))
    ax.plot(time_plot, volts_plot, linewidth=1)

    used = {k: False for k in ("low_band", "high_band", "low_line", "high_line", "mid")}

    for p in pulses:
        if (p["low_keep_start_idx"] >= n and
                p["high_keep_start_idx"] >= n and
                p["crossing_idx"] >= n):
            continue

        ls0, le0 = p["low_keep_start_idx"],  p["low_keep_end_idx"]
        hs0, he0 = p["high_keep_start_idx"], p["high_keep_end_idx"]
        low_w  = le0 - ls0
        high_w = he0 - hs0

        low_trim  = int(round(low_w  * 0.10))
        high_trim = int(round(high_w * 0.10))

        ls = max(0, min(n, ls0 + low_trim))
        le = max(0, min(n, le0 - low_trim))
        hs = max(0, min(n, hs0 + high_trim))
        he = max(0, min(n, he0 - high_trim))

        if le <= ls:   ls, le = max(0, ls0), min(n, le0)
        if he <= hs:   hs, he = max(0, hs0), min(n, he0)

        if ls < le:
            ax.axvspan(time_s[ls], time_s[le - 1], alpha=0.18, color="tab:blue",
                       label="low flat window" if not used["low_band"] else None)
            used["low_band"] = True
            ax.hlines(p["low_level"], time_s[ls], time_s[le - 1],
                      linewidth=1.8, linestyles="--", colors="orange",
                      label="low flat level" if not used["low_line"] else None)
            used["low_line"] = True

        if hs < he:
            ax.axvspan(time_s[hs], time_s[he - 1], alpha=0.18, color="tab:green",
                       label="high flat window" if not used["high_band"] else None)
            used["high_band"] = True
            ax.hlines(p["high_level"], time_s[hs], time_s[he - 1],
                      linewidth=1.8, linestyles="--", colors="orange",
                      label="high flat level" if not used["high_line"] else None)
            used["high_line"] = True

        if p["crossing_idx"] < n:
            ax.plot(p["crossing_time_s"], p["midpoint"],
                    marker="o", linestyle="None", markersize=5, color="red",
                    label="midpoint crossing" if not used["mid"] else None)
            used["mid"] = True

    ax.set_title(
        f"Original signal with low flat, high flat, and midpoint crossing overlays"
        f" (first {n} samples)"
    )
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Voltage (V)")
    ax.grid(True)
    ax.legend()
    fig.tight_layout()
    return fig


def plot_rpm(rpm_df: pd.DataFrame):
    fig, ax = plt.subplots(figsize=(14, 5))
    ax.plot(rpm_df["time_s"], rpm_df["alt_rpm"], linewidth=1)
    ax.scatter(rpm_df["time_s"], rpm_df["alt_rpm"], s=6)
    ax.set_title("Alternator RPM vs time")
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Alternator RPM")
    ax.grid(True)
    fig.tight_layout()
    return fig


def plot_rpm_fft(rpm_df: pd.DataFrame):
    """
    FFT of alternator RPM using a Hanning window.

    Time base uses median inter-pulse dt for robustness against jitter/outliers.
    Amplitude is corrected for the Hanning window coherent gain so the y-axis
    is in meaningful RPM units.
    """
    if rpm_df.empty or len(rpm_df) < 16:
        print("Not enough RPM data for FFT.")
        return None

    t   = rpm_df["time_s"].to_numpy(dtype=float)
    rpm = rpm_df["alt_rpm"].to_numpy(dtype=float)

    mask = np.isfinite(t) & np.isfinite(rpm)
    t    = t[mask]
    rpm  = rpm[mask]
    if len(rpm) < 16:
        print("Not enough valid RPM samples for FFT.")
        return None

    # Median dt is more robust than mean against jitter outliers
    dt_vals = np.diff(t)
    dt_vals = dt_vals[np.isfinite(dt_vals) & (dt_vals > 0)]
    if len(dt_vals) == 0:
        print("Invalid time base for FFT.")
        return None
    dt_med = np.median(dt_vals)

    # Remove DC so modulation is visible
    rpm_detrended = rpm - np.mean(rpm)

    N      = len(rpm_detrended)
    window = np.hanning(N)
    xw     = rpm_detrended * window

    fft_vals = np.fft.rfft(xw)
    freqs    = np.fft.rfftfreq(N, d=dt_med)

    # Coherent-gain correction so amplitude is in RPM
    coherent_gain = np.sum(window) / N
    mag = (2.0 / (N * coherent_gain)) * np.abs(fft_vals)

    # Skip DC bin for display
    freqs_plot = freqs[1:]
    mag_plot   = mag[1:]
    f_nyq      = 0.5 / dt_med

    fig, ax = plt.subplots(figsize=(14, 5))

    step = max(1, len(freqs_plot) // 2000)
    ax.plot(freqs_plot, mag_plot, linewidth=1)
    ax.scatter(freqs_plot[::step], mag_plot[::step], s=6)

    ax.set_xlim(0, f_nyq)
    ax.set_xlabel("Frequency (Hz)")
    ax.set_ylabel("Amplitude (RPM)")
    ax.set_title("FFT of Alternator RPM (Hanning window)")
    ax.grid(True)

    secax = ax.secondary_xaxis(
        "top",
        functions=(lambda x: x * 60.0, lambda x: x / 60.0)
    )
    secax.set_xlabel("Frequency (RPM-equivalent)")

    fig.tight_layout()
    return fig


# ===== Main =====
def main():
    csv_path = pick_csv_file()
    if not csv_path:
        print("No file selected.")
        return

    df = pd.read_csv(csv_path)
    expected_cols = {"time_s", "volts"}
    if not expected_cols.issubset(df.columns):
        print(f"CSV must contain columns: {expected_cols}")
        print(f"Found columns: {list(df.columns)}")
        return

    time_s = df["time_s"].to_numpy(dtype=float)
    volts  = df["volts"].to_numpy(dtype=float)

    if len(time_s) < 10:
        print("File too short.")
        return

    if SHOW_DEBUG_PRINTS:
        dt        = np.diff(time_s)
        median_dt = np.median(dt)
        fs        = 1.0 / median_dt if median_dt > 0 else float("nan")
        print(f"Loaded: {csv_path}")
        print(f"Samples: {len(time_s)}")
        print(f"Estimated sample rate: {fs:.3f} Hz")
        print(f"Estimated dt: {median_dt * 1e6:.3f} us")

    segments, coarse_low, coarse_high, coarse_mid, low_thresh, high_thresh = \
        segment_signal_into_states(time_s, volts)

    if SHOW_DEBUG_PRINTS:
        print(f"Coarse low:       {coarse_low:.6f} V")
        print(f"Coarse high:      {coarse_high:.6f} V")
        print(f"Coarse midpoint:  {coarse_mid:.6f} V")
        print(f"Hysteresis thresholds: low={low_thresh:.6f} V, high={high_thresh:.6f} V")
        print(f"Segments found:   {len(segments)}")

    pulses = extract_rising_pulses(time_s, volts, segments)

    if SHOW_DEBUG_PRINTS:
        print(f"Rising pulses found: {len(pulses)}")
        for p in pulses[:10]:
            print(
                f"  Pulse {p['pulse_index']:3d}: "
                f"low={p['low_level']:.6f} V, "
                f"high={p['high_level']:.6f} V, "
                f"mid={p['midpoint']:.6f} V, "
                f"t_cross={p['crossing_time_s']:.9f} s"
            )

    rpm_df = pulses_to_rpm(pulses, ALT_POLE_PAIRS)

    if rpm_df.empty:
        print("Fewer than 2 valid pulses found — cannot compute RPM. Exiting.")
        return

    if SHOW_DEBUG_PRINTS:
        print("\nFirst 10 RPM rows:")
        print(rpm_df.head(10).to_string(index=False))
        print(f"\nMean alternator RPM: {rpm_df['alt_rpm'].mean():.3f}")

    fig1 = plot_sanity_check(time_s, volts, pulses, PLOT_SAMPLES)
    fig2 = plot_rpm(rpm_df)
    fig3 = plot_rpm_fft(rpm_df)

    # Link fig1 (raw waveform) and fig2 (RPM vs time) — both share a time (s)
    # x-axis. fig3 is frequency domain so it is intentionally excluded.
    _ax_sanity = fig1.axes[0]
    _ax_rpm    = fig2.axes[0]
    _syncing   = [False]

    def _on_xlim_changed(changed_ax):
        if _syncing[0]:
            return
        _syncing[0] = True
        try:
            new_xlim = changed_ax.get_xlim()
            for ax in (_ax_sanity, _ax_rpm):
                if ax is not changed_ax:
                    ax.set_xlim(new_xlim)
            for fig in (fig1, fig2):
                fig.canvas.draw_idle()
        finally:
            _syncing[0] = False

    _ax_sanity.callbacks.connect("xlim_changed", _on_xlim_changed)
    _ax_rpm.callbacks.connect("xlim_changed", _on_xlim_changed)

    plt.show()


if __name__ == "__main__":
    main()