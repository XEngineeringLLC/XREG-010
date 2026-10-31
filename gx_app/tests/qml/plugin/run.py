# Offline QML harness: loads the plugin QML against the stub Victron.VenusOS module and runs harness.qml's checks.
#   python3 run.py <plugin dir> <schema json> [SevenInch|FiveInch|Portrait] [Dark|Light]
import sys, os, subprocess
sys.dont_write_bytecode = True
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(here))
import qmlenv
plugin_dir, schema_path = sys.argv[1], sys.argv[2]
size = sys.argv[3] if len(sys.argv) > 3 else 'SevenInch'
scheme = sys.argv[4] if len(sys.argv) > 4 else 'Dark'
stubs = qmlenv.stubs(size, scheme)
os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
from PySide6.QtCore import QUrl, qInstallMessageHandler, QtMsgType, QTimer
from PySide6.QtGui import QGuiApplication, QFontDatabase
from PySide6.QtQml import QQmlApplicationEngine
msgs = []
def handler(mode, ctx, msg):
    msgs.append((mode, msg, ctx.file, ctx.line))
    print(('W ' if mode not in (QtMsgType.QtInfoMsg, QtMsgType.QtDebugMsg) else '') + msg
          + ((' (%s:%s)' % (ctx.file, ctx.line)) if mode not in (QtMsgType.QtInfoMsg, QtMsgType.QtDebugMsg) else ''), flush=True)
qInstallMessageHandler(handler)
app = QGuiApplication(sys.argv)
QFontDatabase.addApplicationFont(qmlenv.FONT)
eng = QQmlApplicationEngine()
eng.addImportPath(stubs)
eng.rootContext().setContextProperty('schemaText', open(schema_path, encoding='utf-8').read())
eng.rootContext().setContextProperty('pluginDir', QUrl.fromLocalFile(plugin_dir).toString())
eng.load(QUrl.fromLocalFile(os.path.join(here, 'harness.qml')))
if not eng.rootObjects():
    print('HARNESS LOAD FAILED'); sys.exit(2)
QTimer.singleShot(120000, app.quit)
app.exec()
warn = [m for m in msgs if m[0] in (QtMsgType.QtWarningMsg, QtMsgType.QtCriticalMsg, QtMsgType.QtFatalMsg)
        and not m[1].startswith('Populating font family aliases')]
fails = [m for m in msgs if m[1].startswith('FAIL')]
passes = [m for m in msgs if m[1].startswith('PASS')]
# A gui-v2 warning (a binding loop, a type error) fails the run like a failed check
print(('PASS' if not warn else 'FAIL') + ' no Qt warnings' + ('' if not warn else ': ' + ' | '.join(sorted(set(m[1][:160] for m in warn)))[:600]))
print('SUMMARY: %d PASS, %d FAIL, %d Qt warnings' % (len(passes) + (not warn), len(fails) + bool(warn), len(warn)))
sys.exit(1 if fails or warn or not any(m[1].startswith('DONE') for m in msgs) else 0)
