"""
plot_altcapture.py
- Alternator best-ever-front CAPTURE-CRITERIA tuner.
- File selector GUI (tkinter) — pick a THERMAL LOG csv from Downloads, newest first.
- Replays the on-device alt-health steady-state detector over the log and shows,
  with live sliders, where a surface point WOULD be captured under any criteria set.
  Three panels:
    1. Amps vs time   (green = full capture, orange = temperature steady only)
    2. Temp  vs time   (same shading)
    3. Blocker map     — for each non-temp axis, lit where temp IS steady but THAT
                         axis is what's stopping the capture (why orange isn't green).
- Slider start values are the study working-points (see SLIDERS below for the live numbers —
  not restated here so they can't drift). Each maps back to a firmware knob:
  altThermDegF/altThermSec, altRpmTol/altRpmSec, altDutyTolPct/altDutySec,
  altVbusTol/altVbusSec, altAmpsTolPct/altAmpsSec, altMinAmps.

Detector replication notes (must track 7_functions.ino if those change):
- Inputs RPM/duty/Vbus/amps get a 0.5 s EMA (altEmaSec); temperature uses tempFilt_F raw.
- Eligibility barrier (Vbus>=8, amps>=altMinAmps, duty>=5) RESETS the dwell windows —
  a load lull that drops amps below the floor restarts every axis's steady clock.
- An axis is steady when the run has been eligible >= its dwell AND the trailing-window
  (max-min) is within its band. Capture = all 5 axes steady at once.
"""

import csv
import glob
import os
from collections import deque
import tkinter as tk
from tkinter import messagebox
from filepicker import pick_file

import matplotlib
matplotlib.use("TkAgg")
import matplotlib.pyplot as plt
from matplotlib.widgets import Slider

DOWNLOADS = os.path.expanduser("~/Downloads")

# ---- firmware constants (keep in sync with 7_functions.ino / Xregulator.ino) ----
EMA_SEC    = 0.5     # altEmaSec
AMPS_FLOOR = 1.5     # altAmpsFloorA (A) — floor under the output band
MIN_DUTY   = 5.0     # altMinDuty (eligibility)
MIN_BATT_V = 8.0     # ALT_MIN_BATT_V (eligibility)

# ===== Load thermal log (REMINDER: select THERMAL LOGS as input) =====
def load(path):
    ts=[]; tFa=[]; dutya=[]; rpma=[]; vbusa=[]; ampsa=[]
    with open(path, encoding="utf-8", errors="replace") as f:
        r = csv.reader(f)
        header = None
        for line in f:  # find ts_ms header, skip CONST rows
            if line.strip().startswith("ts_ms"):
                header = line.strip().split(","); break
        if header is None:
            raise SystemExit("No ts_ms header row — is this a thermal log?")
        idx = {name: i for i, name in enumerate(header)}
        need = ["ts_ms","tempFilt_F","duty_pct","RPM","battV","measAmps_A"]
        for n in need:
            if n not in idx:
                raise SystemExit(f"Column '{n}' missing — wrong log type?")
        for line in f:
            p = line.strip().split(",")
            if not p or p[0]=="CONST" or len(p) <= idx["measAmps_A"]:
                continue
            try:
                ts.append(int(p[idx["ts_ms"]]))
                tFa.append(float(p[idx["tempFilt_F"]]))
                dutya.append(float(p[idx["duty_pct"]]))
                rpma.append(float(p[idx["RPM"]]))
                vbusa.append(float(p[idx["battV"]]))
                ampsa.append(float(p[idx["measAmps_A"]]))
            except (ValueError, IndexError):
                continue
    return ts, tFa, dutya, rpma, vbusa, ampsa


path = pick_file(
    prefix="thermallog_",
    title="Alt-Capture Tuner — Select Thermal Log",
    reminder="Select THERMAL LOGS as input:",
    subreminder="(the same csv plot_thermallog.py reads — has ts_ms, tempFilt_F, RPM, battV, duty_pct, measAmps_A)")
if not path:
    raise SystemExit("No file selected.")
print(f"Loading thermal log: {path}")
ts, tFa, dutya, rpma, vbusa, ampsa = load(path)
N = len(ts)
if N < 10:
    raise SystemExit("Too few rows parsed.")
t0 = ts[0]
trel = [(t - t0) / 1000.0 for t in ts]
basename = os.path.splitext(os.path.basename(path))[0]

# ---- optional reference surface: an exported Alternator Health Data (BEFRONT1) file ----
# Lets the tool report min/max/avg HEALTH % (measured / LWLR-predicted best-ever) over the
# captured points. Skip → no health readout, capture tuning only.
AXSCALE = [25.0, 0.2, 0.1, 5.0]   # rpm, excitation, Vbus, tempF (altFront2.axisScale)
REF_RADIUS = 2.0                  # altRefRadius — beyond this normalized dist → "no reference"
RIDGE = 0.10                      # altRidgeFrac
ALT_ALPHA = 0.00393; ALT_TREF_C = 25.0   # excitation proxy (altExcitation)

def excitation(duty, vbus, tF):
    tc = (tF - 32.0) / 1.8
    den = 1.0 + ALT_ALPHA * (tc - ALT_TREF_C)
    if den < 0.5: den = 0.5
    return (duty / 100.0) * vbus / den

refpath = pick_file(
    prefix="Alternator Health Data",
    title="Alt-Capture Tuner — Optional Reference Surface",
    reminder="Optionally select an ALTERNATOR HEALTH DATA file (reference):",
    subreminder="(a BEFRONT1 export — gives min/max/avg health %. Skip for capture-tuning only.)",
    optional=True)
REFX = []; REFY = []
if refpath:
    with open(refpath, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("BEFRONT1"): continue
            p = line.strip().split(",")
            if len(p) < 5: continue
            try:
                REFX.append([float(p[0]), float(p[1]), float(p[2]), float(p[3])])
                REFY.append(float(p[4]))
            except (ValueError, IndexError):
                continue
    print(f"Reference surface: {len(REFX)} points from {os.path.basename(refpath)}")
REF_MAXY = max(REFY) if REFY else 0.0

def near_dist(x):
    best = 1e30
    for q in REFX:
        d2 = sum(((x[a]-q[a])/AXSCALE[a])**2 for a in range(4))
        if d2 < best: best = d2
    return best**0.5

def lwlr_pred(x):
    # weighted linear regression over all ref points; prediction = intercept b0 (matches evalLWLR)
    M = 5
    A = [[0.0]*M for _ in range(M)]; b = [0.0]*M
    for q, y in zip(REFX, REFY):
        phi = [1.0] + [(q[a]-x[a])/AXSCALE[a] for a in range(4)]
        d2 = sum(phi[a+1]**2 for a in range(4)); w = 1.0/(d2+1e-9)
        for r_ in range(M):
            for c_ in range(M): A[r_][c_] += w*phi[r_]*phi[c_]
            b[r_] += w*phi[r_]*y
    tr = sum(A[a][a] for a in range(1, M)); ridge = RIDGE*tr/4.0
    for a in range(1, M): A[a][a] += ridge
    for col in range(M):
        pr = max(range(col, M), key=lambda r: abs(A[r][col]))
        if abs(A[pr][col]) < 1e-12: return None
        A[col], A[pr] = A[pr], A[col]; b[col], b[pr] = b[pr], b[col]
        for r in range(col+1, M):
            fac = A[r][col]/A[col][col]
            for c in range(col, M): A[r][c] -= fac*A[col][c]
            b[r] -= fac*b[col]
    sol = [0.0]*M
    for r in range(M-1, -1, -1):
        s = b[r]
        for c in range(r+1, M): s -= A[r][c]*sol[c]
        sol[r] = s/A[r][r]
    pred = sol[0]; hi = 1.25*REF_MAXY
    return max(0.1, min(pred, hi))

def health_stats(full):
    # health % over captured points within refRadius of the reference surface
    if not REFX: return None
    pcts = []; noref = 0
    for i in range(N):
        if not full[i]: continue
        x = [fRpm[i], excitation(fDuty[i], fVbus[i], tFa[i]), fVbus[i], tFa[i]]
        if near_dist(x) > REF_RADIUS: noref += 1; continue
        pred = lwlr_pred(x)
        if pred and pred > 0.1: pcts.append(100.0*fAmps[i]/pred)
    return pcts, noref

# ---- fixed EMA on rpm/duty/vbus/amps (temperature used raw-filtered) ----
fRpm=[0.0]*N; fDuty=[0.0]*N; fVbus=[0.0]*N; fAmps=[0.0]*N
last=None; init=False; r=d=v=a=0.0
for i in range(N):
    if not init or last is None:
        r,d,v,a = rpma[i],dutya[i],vbusa[i],ampsa[i]; init=True
    else:
        dt=ts[i]-last; tau=EMA_SEC*1000
        if dt > (5*tau)+1000:
            r,d,v,a = rpma[i],dutya[i],vbusa[i],ampsa[i]
        else:
            k=dt/(tau+dt)
            r+=k*(rpma[i]-r); d+=k*(dutya[i]-d); v+=k*(vbusa[i]-v); a+=k*(ampsa[i]-a)
    last=ts[i]; fRpm[i],fDuty[i],fVbus[i],fAmps[i]=r,d,v,a


# ===== Detector replay =====
NONTEMP = ("rpm","duty","vbus","amps")
def compute(p):
    full=[False]*N; tempok=[False]*N
    axisok={k:[False]*N for k in NONTEMP}
    dataStart=None
    dq={k:deque() for k in ("rpm","duty","vbus","temp","amps")}
    axes=[("rpm",fRpm,p["rpm_tol"],p["rpm_sec"]),
          ("duty",fDuty,p["duty_tol"],p["duty_sec"]),
          ("vbus",fVbus,p["vbus_tol"],p["vbus_sec"]),
          ("temp",tFa,p["temp_tol"],p["temp_sec"]),
          ("amps",fAmps,None,p["amps_sec"])]
    for i in range(N):
        elig=(fVbus[i]>=MIN_BATT_V and fAmps[i]>=p["min_amps"] and fDuty[i]>=MIN_DUTY and fRpm[i]>=0)
        if not elig:
            dataStart=None
            for k in dq: dq[k].clear()
            continue
        if dataStart is None: dataStart=ts[i]
        elapsed=ts[i]-dataStart
        allok=True
        for name,sig,tol,sec in axes:
            if name=="amps":
                tol=max(p["amps_pct"]*0.01*fAmps[i], AMPS_FLOOR)
            win=int(sec*1000)
            q=dq[name]; q.append((ts[i],sig[i]))
            while q and q[0][0] <= ts[i]-win: q.popleft()
            vals=[x for _,x in q]; rng=max(vals)-min(vals)
            ok=(elapsed>=win) and (rng<=tol)
            if name=="temp": tempok[i]=ok
            else: axisok[name][i]=ok
            if not ok: allok=False
        full[i]=allok
    return full, tempok, axisok


# ===== Figure + sliders =====
fig,(ax1,ax2,ax3)=plt.subplots(3,1,figsize=(15,9.5),sharex=True,
                               gridspec_kw={"height_ratios":[3,3,2]})
plt.subplots_adjust(left=0.07,right=0.99,top=0.91,bottom=0.42,hspace=0.25)
fig.canvas.manager.set_window_title(f"Alt-Capture Tuner — {basename}")
ax1.plot(trel,ampsa,color="#1f77b4",lw=0.8); ax1.set_ylabel("Amps (A)"); ax1.grid(alpha=0.3)
ax2.plot(trel,tFa,color="#d62728",lw=0.8); ax2.set_ylabel("Temp (°F)"); ax2.grid(alpha=0.3)
ax3.set_xlabel("Time from start (s)")

ROWCOL={"rpm":"#9467bd","duty":"#8c564b","vbus":"#1f77b4","amps":"#7f7f7f"}
ROWS=["amps","vbus","duty","rpm"]   # bottom-to-top
ax3.set_ylim(-0.5,len(ROWS)-0.5)
ax3.set_yticks(range(len(ROWS)))
ax3.set_yticklabels(ROWS)
ax3.set_title("Blocker map — lit = temp steady but THIS axis is blocking (why orange isn't green)")
ax3.grid(alpha=0.2,axis="x")

spans1=[]; spans2=[]; bars=[]
title=ax1.set_title("",fontsize=12,linespacing=1.4)

def spanlist(mask):
    out=[]; start=None
    for i in range(N):
        if mask[i] and start is None: start=trel[i]
        if (not mask[i] or i==N-1) and start is not None:
            out.append((start,trel[i])); start=None
    return out

def redraw(p):
    global spans1,spans2,bars
    for s in spans1+spans2: s.remove()
    for b in bars: b.remove()
    spans1=[]; spans2=[]; bars=[]
    full,tempok,axisok=compute(p)
    for a,b in spanlist(tempok):
        spans1.append(ax1.axvspan(a,b,color="#ff7f0e",alpha=0.15,lw=0))
        spans2.append(ax2.axvspan(a,b,color="#ff7f0e",alpha=0.15,lw=0))
    for a,b in spanlist(full):
        spans1.append(ax1.axvspan(a,b,color="#2ca02c",alpha=0.35,lw=0))
        spans2.append(ax2.axvspan(a,b,color="#2ca02c",alpha=0.35,lw=0))
    counts={}
    for yi,name in enumerate(ROWS):
        blk=[tempok[i] and (not axisok[name][i]) for i in range(N)]
        counts[name]=sum(blk)
        xr=[(a,b-a) for a,b in spanlist(blk)]
        if xr:
            bars.append(ax3.broken_barh(xr,(yi-0.4,0.8),facecolors=ROWCOL[name]))
    ax3.set_yticklabels([f"{r}  ({counts[r]} s)" for r in ROWS])
    capN=sum(full); tN=sum(tempok)
    txt=(f"GREEN=capture (all axes)  ORANGE=temp-steady only   |   "
         f"temp-steady {tN} s · capture {capN} s")
    hs=health_stats(full)
    if hs is not None:
        pcts,noref=hs
        if pcts:
            txt+=(f"\nHEALTH % over {len(pcts)} graded captures "
                  f"(+{noref} beyond reference):  "
                  f"min {min(pcts):.1f}  ·  avg {sum(pcts)/len(pcts):.1f}  ·  max {max(pcts):.1f}")
        else:
            txt+=f"\nHEALTH %: no captures within reference range ({noref} beyond ref, {capN} total)"
    title.set_text(txt)
    fig.canvas.draw_idle()

# SLIDERS — slider specs: key, label, min, max, init, valfmt. init = study working-points; the temp
# dwell init here tracks the firmware altThermSec default (keep them in step when either changes).
specs=[
 ("temp_tol","temp ±°F",0.5,15,2.0,"%.1f"),   ("temp_sec","temp dwell s",2,180,80,"%.0f"),
 ("rpm_tol","RPM ±",5,200,30,"%.0f"),         ("rpm_sec","RPM dwell s",1,30,3,"%.0f"),
 ("duty_tol","duty ±%",0.2,10,1.5,"%.2f"),    ("duty_sec","duty dwell s",1,30,3,"%.0f"),
 ("vbus_tol","Vbus ±V",0.01,1.0,0.055,"%.3f"),("vbus_sec","Vbus dwell s",1,30,3,"%.0f"),
 ("amps_pct","amps ±%",0.5,50,5,"%.1f"),      ("amps_sec","amps dwell s",1,30,2,"%.0f"),
 ("min_amps","elig min A",0,20,2,"%.1f"),
]
sliders={}
rows=6
for idx,(key,label,lo,hi,ini,fmt) in enumerate(specs):
    col=idx//rows; row=idx%rows
    x=0.10+col*0.50; y=0.30-row*0.045
    sax=fig.add_axes([x,y,0.32,0.025])
    sliders[key]=Slider(sax,label,lo,hi,valinit=ini,valfmt=fmt)

def on_change(_):
    redraw({k:s.val for k,s in sliders.items()})
for s in sliders.values():
    s.on_changed(on_change)

redraw({k:s.val for k,s in sliders.items()})
plt.show()
