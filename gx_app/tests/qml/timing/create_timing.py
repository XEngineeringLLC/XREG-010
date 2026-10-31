# Compile vs instantiate cost of one plugin page, offline on the Mac (Metal RHI via QQuickRenderControl). Relative only.
#   QML_DISABLE_DISK_CACHE=1 python3 create_timing.py <plugin dir> <Page.qml> [extra props json]
import os, sys, time, statistics, json
os.environ['QT_QPA_PLATFORM'] = 'offscreen'
from PySide6.QtCore import QUrl, QSize, qInstallMessageHandler, QtMsgType
from PySide6.QtGui import QGuiApplication, QFontDatabase, QRhiTexture, QRhiColorAttachment, \
    QRhiTextureRenderTargetDescription
from PySide6.QtQml import QQmlEngine, QQmlComponent
from PySide6.QtQuick import QQuickWindow, QQuickRenderControl, QSGRendererInterface, QQuickRenderTarget, QQuickItem

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import qmlenv
here = os.path.dirname(os.path.abspath(__file__))
stubs = qmlenv.stubs('SevenInch', 'Dark', 'timing')
plugin, page = sys.argv[1], sys.argv[2]
props = json.loads(sys.argv[3]) if len(sys.argv) > 3 else {}
warns = []
qInstallMessageHandler(lambda m, c, s: warns.append(s) if m in (QtMsgType.QtWarningMsg, QtMsgType.QtCriticalMsg) else None)
app = QGuiApplication(sys.argv)
QFontDatabase.addApplicationFont(qmlenv.FONT)
QQuickWindow.setSceneGraphBackend('rhi')
QQuickWindow.setGraphicsApi(QSGRendererInterface.GraphicsApi.Metal)
rc = QQuickRenderControl()
win = QQuickWindow(rc)
W, H = 1024, 544
win.resize(W, H)
assert rc.initialize()
rhi = rc.rhi()
tex = rhi.newTexture(QRhiTexture.RGBA8, QSize(W, H), 1, QRhiTexture.RenderTarget | QRhiTexture.UsedAsTransferSource)
tex.create()
rt = rhi.newTextureRenderTarget(QRhiTextureRenderTargetDescription(QRhiColorAttachment(tex)))
rp = rt.newCompatibleRenderPassDescriptor(); rt.setRenderPassDescriptor(rp); rt.create()
win.setRenderTarget(QQuickRenderTarget.fromRhiRenderTarget(rt))

def frame():
    t0 = time.perf_counter(); rc.polishItems(); rc.beginFrame(); rc.sync(); t1 = time.perf_counter()
    rc.render(); rc.endFrame()
    return (t1 - t0) * 1e3

eng = QQmlEngine()
eng.addImportPath(stubs)

# Warm the stub module (gui-v2 has its own types loaded long before a plugin page opens)
qmldir = open(os.path.join(stubs, 'Victron/VenusOS/qmldir')).read().split('\n')
types = [l.split()[0] for l in qmldir if l and not l.startswith(('module', 'singleton')) and len(l.split()) == 3]
warm_src = 'import QtQuick\nimport QtQuick.Shapes\nimport Victron.VenusOS\nItem {\n' + ''.join(
    '\tComponent { %s {} }\n' % t for t in types) + '\tShape { ShapePath { PathMultiline {} } }\n' \
    '\tText { text: Theme.color_font_primary + Global.fontFamily + MockStore.created + Units + ScreenBlanker.blanked }\n}\n'
warm = QQmlComponent(eng)
warm.setData(warm_src.encode(), QUrl.fromLocalFile(os.path.join(here, 'warm.qml')))
w = warm.create()
if w is None:
    print('warm failed', warm.errorString()); sys.exit(2)

# Seed the item store with demo data, as the bridge would have published it
store = eng.singletonInstance('Victron.VenusOS', 'MockStore')
demo = json.load(open(os.path.join(qmlenv.FIXTURES, 'demo.json')))
vals = {'mock/' + k: v for k, v in demo.items()}
vals['mock/Settings/Schema'] = open(qmlenv.SCHEMA).read()
store.setProperty('values', vals)

url = QUrl.fromLocalFile(os.path.join(plugin, page))
t0 = time.perf_counter()
comp = QQmlComponent(eng, url)
t1 = time.perf_counter()
if comp.status() != QQmlComponent.Ready:
    print(comp.errorString()); sys.exit(2)
p = dict(svc='mock', **props)
runs = []
for i in range(6):
    t2 = time.perf_counter()
    obj = comp.createWithInitialProperties(p)
    obj.setParentItem(win.contentItem()); obj.setWidth(W); obj.setHeight(H)
    t3 = time.perf_counter()
    f = frame()
    runs.append(((t3 - t2) * 1e3, f))
    obj.setParentItem(None); obj.deleteLater(); app.processEvents(); frame()
print(json.dumps({'page': page, 'compile_ms': round((t1 - t0) * 1e3, 1),
                  'first_create_ms': round(runs[0][0], 1), 'first_frame_gui_ms': round(runs[0][1], 1),
                  'warm_create_ms': round(statistics.median(r[0] for r in runs[1:]), 1),
                  'warm_frame_gui_ms': round(statistics.median(r[1] for r in runs[1:]), 1),
                  'warnings': len(warns), 'w1': warns[:2]}))
