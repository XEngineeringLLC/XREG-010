# python3 flow_run.py <plugin dir> <scenarios.json> <Dark|Light> [render dir]
import sys, os, subprocess
sys.dont_write_bytecode = True
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(here))
import qmlenv
plugin, scen, scheme = sys.argv[1:4]
render = sys.argv[4] if len(sys.argv) > 4 else ''
stubs = qmlenv.stubs('SevenInch', scheme)
os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
from PySide6.QtCore import QUrl, qInstallMessageHandler, QtMsgType
from PySide6.QtGui import QGuiApplication, QFontDatabase
from PySide6.QtQml import QQmlApplicationEngine
msgs = []
def handler(mode, ctx, msg):
    msgs.append((mode, msg))
    print(('W ' if mode not in (QtMsgType.QtInfoMsg, QtMsgType.QtDebugMsg) else '') + msg + ((' (%s:%s)' % (ctx.file, ctx.line)) if mode not in (QtMsgType.QtInfoMsg, QtMsgType.QtDebugMsg) else ''), flush=True)
qInstallMessageHandler(handler)
app = QGuiApplication(sys.argv[:1])
QFontDatabase.addApplicationFont(qmlenv.FONT)
eng = QQmlApplicationEngine()
eng.addImportPath(stubs)
for k, v in (('scenariosText', open(scen).read()), ('pluginDir', QUrl.fromLocalFile(plugin).toString()), ('renderDir', render), ('scheme', scheme.lower())):
    eng.rootContext().setContextProperty(k, v)
eng.load(QUrl.fromLocalFile(os.path.join(here, 'flow_harness.qml')))
app.exec()
warn = [m for m in msgs if m[0] in (QtMsgType.QtWarningMsg, QtMsgType.QtCriticalMsg, QtMsgType.QtFatalMsg) and not m[1].startswith('Populating font family aliases')]
fails = [m for m in msgs if m[1].startswith('FAIL')]
passes = [m for m in msgs if m[1].startswith('PASS')]
print('SUMMARY: %d PASS, %d FAIL, %d Qt warnings' % (len(passes), len(fails), len(warn)))
sys.exit(1 if fails or warn or not any(m[1].startswith('DONE') for m in msgs) else 0)
