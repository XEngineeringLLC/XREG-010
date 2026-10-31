# Shape asynchronous: same drawn input and final state as synchronous, numbers only. python3 async_check.py <async dir> <sync dir>
import sys, os, json, time, random, statistics
sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.environ.get('XREG_BRIDGE') or os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(HERE))), 'xregulator', 'bridge'))
import xreg_core as xc
import rcwin
from rcwin import app, eng, win, frame, seed, store, warns
from PySide6.QtCore import QUrl, QMetaObject, Q_ARG, Q_RETURN_ARG
from PySide6.QtQml import QQmlComponent

def call(obj, name, *args, ret=True):
    a = [Q_ARG('QVariant', x) for x in args]
    return QMetaObject.invokeMethod(obj, name, Q_RETURN_ARG('QVariant'), *a) if ret else QMetaObject.invokeMethod(obj, name, *a)

comp = QQmlComponent(eng, QUrl.fromLocalFile(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'shapes.qml')))
root = comp.create()
root.setParentItem(win.contentItem())

def until_ready(maxf=60):
    n = 0
    page = root.property('page')
    k = 0
    while page is not None and page.metaObject().indexOfProperty('sweeping') >= 0 and page.property('sweeping') and k < 5000:
        app.processEvents(); k += 1
    while call(root, 'busy') and k < 5000:
        app.processEvents(); frame(); k += 1
    while n < maxf:
        app.processEvents()
        frame()
        n += 1
        info = json.loads(call(root, 'pathInfo'))
        if all(s['status'] != 2 for s in info):
            return n, info
        time.sleep(0.002)
    return n, info

def run(dirpath, page, props, updates, start=None):
    seed()
    call(root, 'load', QUrl.fromLocalFile(os.path.join(dirpath, page)).toString(), props, ret=False)
    app.processEvents()
    if start:
        start(root.property('page'))
        app.processEvents()
    first_n, first = until_ready()
    steps = []
    pol = []
    for u in updates:
        u(root.property('page'))
        app.processEvents()
        t0 = time.perf_counter(); rcwin.rc.polishItems(); pol.append((time.perf_counter() - t0) * 1e3)
        n, info = until_ready()
        steps.append((n, info))
    call(root, 'load', '', {}, ret=False)
    app.processEvents(); frame()
    return first_n, first, steps, pol

def short_feed(n):
    # Short Term as the bridge feeds it: /Fast/Short from xreg_core's ShortRecorder, 320 s of 100 ms frames read whole at
    # open, then n increments of one frame. The same seed for the async and sync runs.
    rnd = random.Random(7)
    rec = xc.ShortRecorder()
    s = {'at': 0.0, 'sms': 1000, 'since': None, 'w': 0.0}

    def add(k):
        for _ in range(k):
            s['at'] += 0.1
            s['sms'] += 100
            s['w'] += rnd.gauss(0, 1)
            rec.frame(s['at'], 1, {'sendMs': s['sms'], 'sessionId': 1, 'MeasuredAmps': int(6000 + 300 * s['w']),
                                   'Bcur': int(2500 + 200 * s['w']), 'iiout': 350 + rnd.randint(-5, 5),
                                   'dutyCycle': int(4500 + 100 * s['w']), 'pidAltPV': int(6000 + 250 * s['w']),
                                   'BatteryV': 1385 + rnd.randint(-3, 3), 'IBV': 1380 + rnd.randint(-3, 3),
                                   'RPM': 1800 + rnd.randint(-20, 20), 'AlternatorTemperatureF': 15000 + rnd.randint(-20, 20)},
                      1 if rec.n % 2000 < 1500 else 2)

    def put(msg):
        QMetaObject.invokeMethod(store, 'put', Q_ARG('QVariant', 'mock/Fast/Short'), Q_ARG('QVariant', msg))
    add(3200)

    def start(page):
        put(rec.message(None))
        s['since'] = rec.n
    out = []
    for i in range(n):
        def u(page):
            add(1)
            put(rec.message(s['since']))
            s['since'] = rec.n
        out.append(u)
    return start, out

def perf_updates(page_views):
    out = []
    for v, frame_true in page_views:
        def u(page, v=v, t=frame_true):
            page.setProperty('pinned', v)
            page.setProperty('trueFrame', t)
        out.append(u)
    return out

res = {}
for label, d in (('async', sys.argv[1]), ('sync', sys.argv[2])):
    r = {}
    start, ups = short_feed(20)
    r['short300'] = run(d, 'XregPlotsShort.qml', {'svc': 'mock', 'toolX': 500, 'toolY': 8, 'bodyY': 58, 'windowS': 300}, ups, start)
    r['perf'] = run(d, 'XregChartPerf.qml', {'svc': 'mock', 'linkUp': True, 'active': True, 'width': 430, 'height': 348},
                    perf_updates([(0, False), (0, True), (1, False), (0, False)]))
    res[label] = r

ok_all = True
for k in ('short300', 'perf'):
    a, s = res['async'][k], res['sync'][k]
    strip = lambda info: [{kk: vv for kk, vv in x.items() if kk != 'async'} for x in info]
    same_first = strip(a[1]) == strip(s[1])
    same_steps = all(strip(x[1]) == strip(y[1]) for x, y in zip(a[2], s[2]))
    shapes_n = len(a[1])
    asyncs = sum(1 for x in a[1] if x['async'])
    ready = all(x['status'] == 1 or not x['visible'] for x in a[1]) and all(all(y['status'] == 1 or not y['visible'] for y in st[1]) for st in a[2])
    pts = sum(e.get('points', 0) for x in a[1] for p in x['paths'] for e in p['els'])
    renderers = sorted(set(x['renderer'] for x in a[1]))
    print('%s %s: %d Shapes (%d asynchronous), %d points drawn at open; open and %d updates identical to synchronous: %s; '
          'all visible Shapes Ready: %s; frames to Ready, async open %d / sync open %d, updates async max %d / sync max %d; renderer %s'
          % ('PASS' if same_first and same_steps and ready else 'FAIL', k, shapes_n, asyncs, pts, len(a[2]), same_first and same_steps,
             ready, a[0], s[0], max(x[0] for x in a[2]), max(x[0] for x in s[2]), renderers))
    if k == 'short300':
        print('INFO short300 polish ms (GUI thread, one update), async median %.3f max %.3f; sync median %.3f max %.3f'
              % (statistics.median(a[3]), max(a[3]), statistics.median(s[3]), max(s[3])))
    ok_all &= same_first and same_steps and ready
print(('PASS' if not warns else 'FAIL') + ' Qt warnings: %d %s' % (len(warns), warns[:3]))
print('SUMMARY async_check: %s' % ('PASS' if ok_all and not warns else 'FAIL'))
