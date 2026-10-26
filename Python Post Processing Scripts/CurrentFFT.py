import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
from matplotlib.widgets import Cursor
import os
from tkinter import Tk, filedialog
from pathlib import Path


AMPS_PER_VOLT = 250.0   # sensor scale factor — adjust to match your current probe

# ===== Sensor conversion =====
def volts_to_amps(volts: np.ndarray) -> np.ndarray:
    """
    AC-coupled current signal:
    no 2.5 V offset subtraction
    """
    return volts * AMPS_PER_VOLT


# ===== File picker =====
def pick_csv_file() -> str:
    root = Tk()
    root.withdraw()
    root.attributes("-topmost", True)   # native dialog otherwise opens behind the terminal on macOS
    root.update()
    try:
        os.system(
            "osascript -e 'tell application \"System Events\" to set frontmost of "
            "first process whose unix id is %d to true' >/dev/null 2>&1" % os.getpid()
        )
    except Exception:
        pass
    file_path = filedialog.askopenfilename(
        title="Select CSV file",
        initialdir=os.path.expanduser("~/Downloads"),
        filetypes=[("CSV or text files", "*.csv *.txt"), ("All files", "*.*")]
    )
    root.destroy()
    return file_path


# ===== CSV loader =====
def load_csv_auto(csv_path: str) -> pd.DataFrame:
    for sep in ["\t", ",", ";"]:
        try:
            df = pd.read_csv(csv_path, sep=sep)
            df.columns = [c.strip() for c in df.columns]
            if "time_s" in df.columns and "volts" in df.columns:
                return df
        except Exception:
            pass

    df = pd.read_csv(csv_path, sep=None, engine="python")
    df.columns = [c.strip() for c in df.columns]

    if "time_s" not in df.columns or "volts" not in df.columns:
        raise ValueError(
            f"Could not find required columns 'time_s' and 'volts'. Found: {list(df.columns)}"
        )
    return df


# ===== FFT =====
def compute_fft(time_s: np.ndarray, amps: np.ndarray):
    mask = np.isfinite(time_s) & np.isfinite(amps)
    time_s = time_s[mask]
    amps = amps[mask]

    if len(time_s) < 16:
        raise ValueError("Not enough valid samples for FFT.")

    dt = np.diff(time_s)
    dt = dt[np.isfinite(dt) & (dt > 0)]

    if len(dt) == 0:
        raise ValueError("Invalid time base.")

    dt_median = np.median(dt)
    fs = 1.0 / dt_median

    amps_detrended = amps - np.mean(amps)

    n = len(amps_detrended)
    window = np.hanning(n)
    amps_windowed = amps_detrended * window

    fft_vals = np.fft.rfft(amps_windowed)
    freqs = np.fft.rfftfreq(n, d=dt_median)

    coherent_gain = np.sum(window) / n
    amplitudes = (2.0 / (n * coherent_gain)) * np.abs(fft_vals)

    freqs = freqs[1:]
    amplitudes = amplitudes[1:]

    return freqs, amplitudes, fs, dt_median


def find_top_frequencies(freqs: np.ndarray, amplitudes: np.ndarray, fmax: float, top_n: int = 10):
    mask = freqs <= fmax
    freqs_limited = freqs[mask]
    amplitudes_limited = amplitudes[mask]

    if len(freqs_limited) == 0:
        return pd.DataFrame(columns=["frequency_hz", "amplitude_A"])

    idx = np.argsort(amplitudes_limited)[::-1][:top_n]

    return pd.DataFrame({
        "frequency_hz": freqs_limited[idx],
        "amplitude_A": amplitudes_limited[idx]
    }).sort_values("frequency_hz").reset_index(drop=True)


# ===== Plotting =====
def plot_time_domain(time_s: np.ndarray, amps: np.ndarray, file_name: str):
    fig, ax = plt.subplots(figsize=(14, 6))
    ax.plot(time_s, amps, linewidth=1)
    ax.set_title(f"Current vs Time\n{file_name}")
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Current (A)")
    ax.grid(True)
    Cursor(ax, useblit=True, color="red", linewidth=1)
    fig.tight_layout()
    return fig, ax


def plot_frequency_domain(freqs: np.ndarray, amplitudes: np.ndarray, top_df: pd.DataFrame, file_name: str, fmax: float):
    mask = freqs <= fmax
    freqs_plot = freqs[mask]
    amplitudes_plot = amplitudes[mask]

    fig, ax = plt.subplots(figsize=(14, 6))
    ax.plot(freqs_plot, amplitudes_plot, linewidth=1)
    ax.set_xlim(0, fmax)
    ax.set_title(f"FFT of Current (0 to {fmax:.0f} Hz)\n{file_name}")
    ax.set_xlabel("Frequency (Hz)")
    ax.set_ylabel("Amplitude (A)")
    ax.grid(True)

    for _, row in top_df.iterrows():
        f = row["frequency_hz"]
        a = row["amplitude_A"]
        ax.plot(f, a, "ro")
        ax.annotate(f"{f:.2f} Hz", (f, a), textcoords="offset points", xytext=(5, 5))

    Cursor(ax, useblit=True, color="red", linewidth=1)
    fig.tight_layout()
    return fig, ax
def plot_frequency_domain_full(freqs: np.ndarray, amplitudes: np.ndarray, file_name: str):
    fig, ax = plt.subplots(figsize=(14, 6))
    ax.plot(freqs, amplitudes, linewidth=1)
    ax.set_title(f"FFT of Current (full frequency range)\n{file_name}")
    ax.set_xlabel("Frequency (Hz)")
    ax.set_ylabel("Amplitude (A)")
    ax.grid(True)
    Cursor(ax, useblit=True, color="red", linewidth=1)
    fig.tight_layout()
    return fig, ax

# ===== Main =====
def main():
    csv_path = pick_csv_file()
    if not csv_path:
        print("No file selected.")
        return

    df = load_csv_auto(csv_path)

    time_s = pd.to_numeric(df["time_s"], errors="coerce").to_numpy(dtype=float)
    volts = pd.to_numeric(df["volts"], errors="coerce").to_numpy(dtype=float)
    amps = volts_to_amps(volts)

    valid = np.isfinite(time_s) & np.isfinite(amps)
    time_s = time_s[valid]
    amps = amps[valid]

    if len(time_s) < 2:
        print("Not enough valid data.")
        return

    freqs, amplitudes, fs, dt_median = compute_fft(time_s, amps)

    top_df_1200 = find_top_frequencies(freqs, amplitudes, fmax=1200.0, top_n=10)
    top_df_130 = find_top_frequencies(freqs, amplitudes, fmax=130.0, top_n=10)

    file_name = Path(csv_path).name

    print(f"\nLoaded file: {csv_path}")
    print(f"Samples: {len(time_s)}")
    print(f"Estimated sample rate: {fs:.3f} Hz")
    print(f"Median dt: {dt_median:.9e} s")
    print(f"Current min/max: {np.min(amps):.3f} A / {np.max(amps):.3f} A")

    print("\nTop dominant frequencies up to 1200 Hz:")
    if len(top_df_1200) == 0:
        print("None found.")
    else:
        print(top_df_1200.to_string(index=False))

    print("\nTop dominant frequencies up to 130 Hz:")
    if len(top_df_130) == 0:
        print("None found.")
    else:
        print(top_df_130.to_string(index=False))

    plot_time_domain(time_s, amps, file_name)
    plot_frequency_domain(freqs, amplitudes, top_df_1200, file_name, 1200.0)
    plot_frequency_domain(freqs, amplitudes, top_df_130, file_name, 130.0)
    plot_frequency_domain_full(freqs, amplitudes, file_name)
    print("\nUse matplotlib toolbar for zoom/pan/reset.")

    plt.show()


if __name__ == "__main__":
    main()