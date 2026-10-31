# Offscreen Qt Quick window through QQuickRenderControl (Metal), as agentB's timing.py; no image is read back.
import os, sys, time, json
os.environ['QT_QPA_PLATFORM'] = 'offscreen'
from PySide6.QtCore import QUrl, QSize, qInstallMessageHandler, QtMsgType
from PySide6.QtGui import QGuiApplication, QFontDatabase, QRhiTexture, QRhiColorAttachment, \
    QRhiTextureRenderTargetDescription
from PySide6.QtQml import QQmlEngine, QQmlComponent
from PySide6.QtQuick import QQuickWindow, QQuickRenderControl, QSGRendererInterface, QQuickRenderTarget

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import qmlenv
stubs = qmlenv.stubs('SevenInch', 'Dark', 'timing')
warns = []
def _handler(mode, ctx, msg):
    if mode in (QtMsgType.QtWarningMsg, QtMsgType.QtCriticalMsg, QtMsgType.QtFatalMsg) and 'font family' not in msg:
        warns.append(msg)
    elif mode in (QtMsgType.QtDebugMsg, QtMsgType.QtInfoMsg) and msg.startswith(('PASS', 'FAIL', 'INFO')):
        print(msg, flush=True)
qInstallMessageHandler(_handler)
app = QGuiApplication(sys.argv[:1])
QFontDatabase.addApplicationFont(qmlenv.FONT)
QQuickWindow.setSceneGraphBackend('rhi')
QQuickWindow.setGraphicsApi(QSGRendererInterface.GraphicsApi.Metal)
rc = QQuickRenderControl()
win = QQuickWindow(rc)
W, H = 1024, 544
win.resize(W, H)
assert rc.initialize(), 'render control init failed'
rhi = rc.rhi()
tex = rhi.newTexture(QRhiTexture.RGBA8, QSize(W, H), 1, QRhiTexture.RenderTarget)
tex.create()
rt = rhi.newTextureRenderTarget(QRhiTextureRenderTargetDescription(QRhiColorAttachment(tex)))
rp = rt.newCompatibleRenderPassDescriptor()
rt.setRenderPassDescriptor(rp)
rt.create()
win.setRenderTarget(QQuickRenderTarget.fromRhiRenderTarget(rt))
eng = QQmlEngine()
eng.addImportPath(stubs)
store = eng.singletonInstance('Victron.VenusOS', 'MockStore')

def frame():
    """polish (GUI thread), sync, render: ms each"""
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

def seed(extra=None):
    demo = json.load(open(os.path.join(qmlenv.FIXTURES, 'demo.json')))
    vals = {'mock/' + k: v for k, v in demo.items()}
    now = int(time.time())
    vals.update({'mock/Ui/LongRange': '24h', 'mock/Plot/Long/Range': '24h', 'mock/Plot/Long/Fields': 'battVolt,altCurr,rpm,duty',
                 'mock/Plot/Long/StartEpoch': now - 86400, 'mock/Plot/Long/StepS': 300})
    for f, base, amp in (('battVolt', 13.2, 0.4), ('altCurr', 60, 20), ('rpm', 1800, 300), ('duty', 50, 20)):
        vals['mock/Plot/Long/' + f] = ','.join('%.2f' % (base + amp * ((i * 37) % 100) / 100) for i in range(288))
        vals['mock/Plot/Long/' + f + 'Min'] = ','.join('%.2f' % (base - amp / 4 + amp * ((i * 37) % 100) / 100) for i in range(288))
        vals['mock/Plot/Long/' + f + 'Max'] = ','.join('%.2f' % (base + amp / 4 + amp * ((i * 37) % 100) / 100) for i in range(288))
    vals['mock/Health/TrendCsv/0'] = 'eng,worst,overall\n' + '\n'.join('%.1f,%d,%d' % (i * 0.5, 90 + i % 4, 95 + i % 3) for i in range(200))
    vals['mock/Health/Live/valid'] = 1
    if extra:
        vals.update(extra)
    store.setProperty('values', vals)

def shapes(root):
    """every Shape under root (QQuickShape has 'asynchronous' and 'rendererType')"""
    out = []
    def walk(it):
        for c in it.childItems():
            mo = c.metaObject()
            if mo.indexOfProperty('rendererType') >= 0 and mo.indexOfProperty('asynchronous') >= 0:
                out.append(c)
            walk(c)
    walk(root)
    return out
