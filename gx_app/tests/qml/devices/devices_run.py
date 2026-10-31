import sys, os, subprocess
sys.dont_write_bytecode = True
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(here))
import qmlenv
# devices_run.py <plugin dir> [SevenInch|FiveInch] [Dark|Light] [render dir]
size = sys.argv[2] if len(sys.argv) > 2 else 'SevenInch'
scheme = sys.argv[3] if len(sys.argv) > 3 else 'Dark'
render = sys.argv[4] if len(sys.argv) > 4 else ''
stubs = qmlenv.stubs(size, scheme)
os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
from PySide6.QtCore import QUrl, qInstallMessageHandler, QtMsgType
from PySide6.QtGui import QGuiApplication, QFontDatabase
from PySide6.QtQml import QQmlApplicationEngine
msgs = []
def h(mode, ctx, msg):
    msgs.append((mode, msg)); print(('W ' if mode not in (QtMsgType.QtInfoMsg, QtMsgType.QtDebugMsg) else '') + msg, flush=True)
qInstallMessageHandler(h)
app = QGuiApplication(sys.argv[:1])
QFontDatabase.addApplicationFont(qmlenv.FONT)
eng = QQmlApplicationEngine()
eng.addImportPath(stubs)
eng.rootContext().setContextProperty('pluginDir', QUrl.fromLocalFile(sys.argv[1]).toString())
eng.rootContext().setContextProperty('renderFile', os.path.join(render, 'devices_%s_%s.png' % (size, scheme.lower())) if render else '')
eng.rootContext().setContextProperty('pageW', 1024 if size == 'SevenInch' else 800)
eng.rootContext().setContextProperty('pageH', 544 if size == 'SevenInch' else 424)
eng.load(QUrl.fromLocalFile(os.path.join(here, 'devices_check.qml')))
app.exec()
warn = [m for m in msgs if m[0] in (QtMsgType.QtWarningMsg, QtMsgType.QtCriticalMsg) and not m[1].startswith('Populating font')]
print('SUMMARY: %d PASS, %d FAIL, %d Qt warnings' % (sum(m[1].startswith('PASS') for m in msgs), sum(m[1].startswith('FAIL') for m in msgs), len(warn)))
sys.exit(1 if warn or any(m[1].startswith('FAIL') for m in msgs) or not any(m[1].startswith('DONE') for m in msgs) else 0)
