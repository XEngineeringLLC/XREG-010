"""
Plots code growth velocity from git commit history in the public repo.

Shows:
  - Top: bars = lines inserted per commit, colored by net change (green=add, red=delete-heavy)
  - Bottom: cumulative net lines of code over time
"""

import subprocess
import sys
from datetime import datetime
import matplotlib.pyplot as plt
import matplotlib.dates as mdates
import numpy as np

REPO = "/Users/joeceo/Projects/Regulator2026-public"

# ── Pull raw log ────────────────────────────────────────────────────────────
result = subprocess.run(
    ["git", "log", "--reverse", "--format=%H|%ad|%s", "--date=short", "--numstat"],
    cwd=REPO, capture_output=True, text=True
)
if result.returncode != 0:
    print("git log failed:", result.stderr)
    sys.exit(1)

# ── Parse commits ────────────────────────────────────────────────────────────
commits = []
current = None

for line in result.stdout.splitlines():
    if "|" in line and len(line.split("|")) == 3 and len(line.split("|")[0]) == 40:
        sha, date_str, subject = line.split("|", 2)
        current = {
            "sha": sha[:8],
            "date": datetime.strptime(date_str, "%Y-%m-%d"),
            "subject": subject.strip(),
            "insertions": 0,
            "deletions": 0,
        }
        commits.append(current)
    elif current and line.strip() and line[0].isdigit():
        parts = line.split("\t")
        if len(parts) == 3:
            try:
                current["insertions"] += int(parts[0])
                current["deletions"] += int(parts[1])
            except ValueError:
                pass  # binary files show "-"

if not commits:
    print("No commits parsed.")
    sys.exit(1)

# ── Build series ─────────────────────────────────────────────────────────────
dates      = [c["date"] for c in commits]
insertions = [c["insertions"] for c in commits]
deletions  = [c["deletions"] for c in commits]
net        = [c["insertions"] - c["deletions"] for c in commits]
cumulative = list(np.cumsum(net))

bar_colors = ["#2ecc71" if n >= 0 else "#e74c3c" for n in net]

# ── Plot ──────────────────────────────────────────────────────────────────────
fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(13, 8), sharex=True,
                                gridspec_kw={"height_ratios": [2, 1]})
fig.suptitle("Code Growth — Xregulator/Xregulator", fontsize=14, fontweight="bold", y=0.98)

# Top: per-commit insertions as bars
ax1.bar(dates, insertions, color=bar_colors, width=0.6, alpha=0.85, label="Lines inserted")
ax1.bar(dates, [-d for d in deletions], color="#e74c3c", width=0.6,
        alpha=0.4, label="Lines deleted (inverted)")
ax1.axhline(0, color="#555", linewidth=0.6)
ax1.set_ylabel("Lines changed per commit")
ax1.legend(fontsize=9, loc="upper left")
ax1.grid(axis="y", alpha=0.3)

# Annotate commit subjects on bars (only commits with significant changes)
threshold = max(insertions) * 0.15 if max(insertions) > 0 else 1
for c, ins in zip(commits, insertions):
    if ins >= threshold:
        ax1.annotate(
            c["subject"][:28],
            xy=(c["date"], ins),
            xytext=(4, 4), textcoords="offset points",
            fontsize=7, color="#333", rotation=30, ha="left", va="bottom",
        )

# Bottom: cumulative net LOC
ax2.fill_between(dates, cumulative, alpha=0.25, color="#3498db")
ax2.plot(dates, cumulative, color="#2980b9", linewidth=1.8, marker="o", markersize=4)
ax2.set_ylabel("Cumulative net lines")
ax2.set_xlabel("Commit date")
ax2.grid(alpha=0.3)

ax2.xaxis.set_major_formatter(mdates.DateFormatter("%b %d"))
ax2.xaxis.set_major_locator(mdates.AutoDateLocator())
plt.setp(ax2.xaxis.get_majorticklabels(), rotation=35, ha="right", fontsize=9)

total_ins = sum(insertions)
total_del = sum(deletions)
summary = (
    f"Commits: {len(commits)}  |  "
    f"Total inserted: {total_ins:,}  |  "
    f"Total deleted: {total_del:,}  |  "
    f"Net: {total_ins - total_del:+,}"
)
fig.text(0.5, 0.01, summary, ha="center", fontsize=9, color="#555")

plt.tight_layout(rect=[0, 0.03, 1, 0.97])
plt.show()
