# Synthetic /Plot/Long/* as the bridge publishes it (its own LT_* tables, fmt_series, lt_decimals): longdata.py <out.json> [seed] [range]
import sys, os, json, math, random, time
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'xregulator', 'bridge'))
import xreg_core as xc

out, seed = sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 1
rng = sys.argv[3] if len(sys.argv) > 3 else '30d'
random.seed(seed)
span = xc.LONG_RANGES[rng]
n = xc.LONG_POINTS
step = span // n
start = int(time.time()) - span
base = {'battVolt': (13.4, 0.5), 'battCurr': (5, 40), 'altCurr': (40, 50), 'victronCurr': (3, 8), 'rpm': (1500, 500),
        'duty': (40, 30), 'altTemp': (60, 20), 'tempTherm': (55, 15), 'sog': (5, 2), 'tws': (12, 6), 'vmg': (3, 2),
        'aws': (14, 6), 'awa': (60, 50), 'twa': (90, 60), 'heel': (8, 6), 'pitch': (1, 2), 'soc': (80, 15),
        'baro': (1013, 8), 'ambTemp': (30, 5), 'cog': (180, 170), 'heading': (180, 170), 'leeway': (2, 2),
        'altZero': (0.1, 0.05), 'battTemp': (25, 5)}
first = int(n * 0.12)   # ring shorter than the window: the oldest steps hold no record
gap = set(i for i in range(n) if random.random() < 0.06)
eng = [random.random() < 0.55 for _ in range(n)]
series = {}
for name in xc.LT_FIELDS:
    b, a = base[name]
    ph = random.random() * 6
    av, mn, mx, on = [], [], [], []
    for i in range(n):
        if i < first or i in gap:
            av.append(None); mn.append(None); mx.append(None); on.append(None)
            continue
        v = b + a * math.sin(i / 17.0 + ph) * 0.8 + random.gauss(0, a * 0.1)
        if name in xc.LT_ANGLES:
            v = v % 360 if name in ('cog', 'heading') else max(-180, min(180, v))
        av.append(v); mn.append(v - abs(random.gauss(a * 0.2, a * 0.05))); mx.append(v + abs(random.gauss(a * 0.2, a * 0.05)))
        on.append(v * 1.05 if eng[i] else None)
    series[name], series[name + 'Min'], series[name + 'Max'], series[name + 'OnAvg'] = av, mn, mx, on
series['chargeStage'] = [None if i < first else (1 + (i // 23) % 3) for i in range(n)]
series['cover'] = [None if i < first else random.random() for i in range(n)]
pub = {'Ui/LongRange': rng, 'Plot/Long/Range': rng, 'Plot/Long/Fields': ','.join(xc.LT_FIELDS),
       'Plot/Long/StartEpoch': start, 'Plot/Long/StepS': step}
for name in xc.Bridge._long_series_names() if hasattr(xc, 'Bridge') else []:
    pass
names = []
for nm, *_ in xc.LT_ENV + xc.LT_TAIL_ENV:
    names += [nm, nm + 'Min', nm + 'Max']
names += [nm for nm, _, _ in xc.LT_AVG]
names += [nm + 'OnAvg' for nm, _ in xc.LT_ONAVG]
names += ['chargeStage', 'cover']
for name in names:
    pub['Plot/Long/' + name] = xc.fmt_series(series.get(name, []), xc.lt_decimals(name))
pub['Plot/Long/Last'] = json.dumps({k: round(series[k][-1], xc.lt_decimals(k)) for k in xc.LT_FIELDS if series[k][-1] is not None},
                                   separators=(',', ':'))
json.dump(pub, open(out, 'w'))
print(len(pub), 'paths,', sum(len(str(v)) for v in pub.values()), 'bytes')
