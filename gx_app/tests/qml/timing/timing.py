# Offline timing of the Short Term plot page (XregPlotsShort + XregChart) on the Mac, Metal RHI through
# QQuickRenderControl (offscreen, no window). Relative numbers only.
#   python3 timing.py <variant dir> <windowS> [iterations]
import os, sys, time, random, statistics, json
os.environ['QT_QPA_PLATFORM'] = 'offscreen'
from PySide6.QtCore import QUrl, QMetaObject, Qt, QSize, qInstallMessageHandler, QtMsgType, Q_ARG, Q_RETURN_ARG
from PySide6.QtGui import QGuiApplication, QFontDatabase, QRhiTexture, QRhiColorAttachment, \
    QRhiTextureRenderTargetDescription
from PySide6.QtQml import QQmlEngine, QQmlComponent
from PySide6.QtQuick import QQuickWindow, QQuickRenderControl, QSGRendererInterface, QQuickRenderTarget

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import qmlenv
here = os.path.dirname(os.path.abspath(__file__))
stubs = qmlenv.stubs('SevenInch', 'Dark', 'timing')
variant, windowS = sys.argv[1], int(sys.argv[2])
N = int(sys.argv[3]) if len(sys.argv) > 3 else 60
LEN = 300

warns = []
def handler(mode, ctx, msg):
    if mode in (QtMsgType.QtWarningMsg, QtMsgType.QtCriticalMsg) and 'font family' not in msg:
        warns.append(msg)
qInstallMessageHandler(handler)

app = QGuiApplication(sys.argv)
QFontDatabase.addApplicationFont(qmlenv.FONT)
QQuickWindow.setSceneGraphBackend('rhi')
QQuickWindow.setGraphicsApi(QSGRendererInterface.GraphicsApi.Metal)
rc = QQuickRenderControl()
win = QQuickWindow(rc)
W, H = 1024, 480
win.resize(W, H)
assert rc.initialize(), 'render control init failed'
rhi = rc.rhi()
tex = rhi.newTexture(QRhiTexture.RGBA8, QSize(W, H), 1, QRhiTexture.RenderTarget | QRhiTexture.UsedAsTransferSource)
tex.create()
rt = rhi.newTextureRenderTarget(QRhiTextureRenderTargetDescription(QRhiColorAttachment(tex)))
rp = rt.newCompatibleRenderPassDescriptor()
rt.setRenderPassDescriptor(rp)
rt.create()
win.setRenderTarget(QQuickRenderTarget.fromRhiRenderTarget(rt))

eng = QQmlEngine()
eng.addImportPath(stubs)
comp = QQmlComponent(eng, QUrl.fromLocalFile(os.path.join(here, 'bench.qml')))
root = comp.create()
if root is None:
    print(comp.errorString()); sys.exit(2)
root.setParentItem(win.contentItem())

def call(obj, name, *args, ret=True):
    a = [Q_ARG('QVariant', x) for x in args]
    if ret:
        return QMetaObject.invokeMethod(obj, name, Q_RETURN_ARG('QVariant'), *a)
    return QMetaObject.invokeMethod(obj, name, *a)

def frame():
    t0 = time.perf_counter()
    rc.polishItems()
    t1 = time.perf_counter()
    rc.beginFrame()
    rc.sync()
    t2 = time.perf_counter()
    rc.render()
    rc.endFrame()
    t3 = time.perf_counter()
    return (t1 - t0) * 1e3, (t2 - t1) * 1e3, (t3 - t2) * 1e3

# Demo-like series: 1 s samples, formatted as the bridge's fmt_series does
random.seed(1)
SPEC = [('MeasuredAmps', 1, 80, 6), ('Bcur', 1, 60, 6), ('iiout', 2, 3.2, 0.3), ('dutyCycle', 1, 45, 4),
        ('pidAltPV', 1, 80, 3), ('BatteryV', 2, 13.9, 0.05), ('IBV', 2, 13.88, 0.05), ('RPM', 0, 1800, 40),
        ('AlternatorTemperatureC', 1, 70, 0.5), ('chargeStage', 0, 1, 0)]
hist = {k: [base + random.gauss(0, sd) for _ in range(LEN)] for k, _d, base, sd in SPEC}
def step():
    for k, _d, base, sd in SPEC:
        h = hist[k]
        h.append(base + random.gauss(0, sd))
        del h[0]
def strings():
    return {k: ','.join('%.*f' % (d, v) for v in hist[k]) for k, d, _b, _s in SPEC}

out = {'variant': os.path.basename(variant.rstrip('/')), 'windowS': windowS}
src = QUrl.fromLocalFile(os.path.join(variant, 'XregPlotsShort.qml')).toString()

# Page creation with no data, then with data (the tab-switch case, data already in the item tree)
t0 = time.perf_counter(); call(root, 'load', src, ret=False); t1 = time.perf_counter()
page = root.property('page')
page.setProperty('windowS', windowS)
out['create_first_ms'] = (t1 - t0) * 1e3
out['first_frame_ms'] = sum(frame())
call(root, 'feed', strings(), ret=False)
c0 = call(root, 'created')
t0 = time.perf_counter(); call(page, 'snap', ret=False); t1 = time.perf_counter()
out['first_snap_ms'] = (t1 - t0) * 1e3
out['first_snap_frame_ms'] = sum(frame())
out['first_snap_delegates'] = call(root, 'created') - c0

recreate = []
for i in range(8):
    call(root, 'unload', ret=False); app.processEvents(); frame()
    t0 = time.perf_counter()
    call(root, 'load', src, ret=False)
    page = root.property('page'); page.setProperty('windowS', windowS)
    call(page, 'snap', ret=False)
    t1 = time.perf_counter()
    p, s, r = frame()
    recreate.append(((t1 - t0) * 1e3, p, s, r))
out['recreate_with_data_ms'] = {'create+snap': statistics.median(x[0] for x in recreate),
                                'polish': statistics.median(x[1] for x in recreate),
                                'sync': statistics.median(x[2] for x in recreate),
                                'render': statistics.median(x[3] for x in recreate)}

# Steady state: one new sample per update in every series, as the bridge's 1 s publish
snap, pol, syn, ren, ev, dl = [], [], [], [], [], []
for i in range(N):
    step()
    call(root, 'feed', strings(), ret=False)
    c0 = call(root, 'created')
    t0 = time.perf_counter(); call(page, 'snap', ret=False); t1 = time.perf_counter()
    p, s, r = frame()
    t2 = time.perf_counter(); app.processEvents(); t3 = time.perf_counter()
    snap.append((t1 - t0) * 1e3); pol.append(p); syn.append(s); ren.append(r); ev.append((t3 - t2) * 1e3)
    dl.append(call(root, 'created') - c0)
med = lambda a: round(statistics.median(a), 3)
p90 = lambda a: round(sorted(a)[int(0.9 * len(a))], 3)
out['steady_ms'] = {'snap_js+bindings': (med(snap), p90(snap)), 'polish(shapes,layout)': (med(pol), p90(pol)),
                    'sync': (med(syn), p90(syn)), 'render': (med(ren), p90(ren)), 'events(deletes)': (med(ev), p90(ev))}
out['steady_delegates_created_per_snap'] = (min(dl), med(dl), max(dl))
m = strings()
out['parse_like_snap_ms'] = call(root, 'parseLikeSnap', m, windowS, 200)
out['parse_tail_ms'] = call(root, 'parseTail', m, windowS, 200)
out['snap_js_loop_ms'] = call(root, 'snapLoop', 50)
out['string_bytes_total'] = sum(len(v) for v in m.values())
out['warnings'] = sorted(set(w[:140] for w in warns))[:6]
print(json.dumps(out))
