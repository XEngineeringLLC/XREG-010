# Numeric checks of the GX app page (XregGxApp.qml) and the rows that open it, at one GX layout and colour scheme:
#   python3 gxapp_test.py <plugin dir> <schema json> <FiveInch|SevenInch|Portrait> <Dark|Light>
import sys, os, subprocess, shutil
sys.dont_write_bytecode = True
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(here))
import qmlenv
plugin, schema, size, scheme = sys.argv[1:5]
W, H = {'FiveInch': (800, 424), 'SevenInch': (1024, 544), 'Portrait': (383, 741)}[size]
stubs = qmlenv.stubs(size, scheme)
os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
from PySide6.QtCore import QUrl, qInstallMessageHandler, QtMsgType, QTimer
from PySide6.QtGui import QGuiApplication, QFontDatabase
from PySide6.QtQml import QQmlApplicationEngine
msgs = []
def handler(mode, ctx, msg):
    if 'Populating font' in msg:
        return
    msgs.append((mode, msg))
    warn = mode in (QtMsgType.QtWarningMsg, QtMsgType.QtCriticalMsg, QtMsgType.QtFatalMsg)
    print(('W ' if warn else '') + msg + ((' (%s:%s)' % (ctx.file, ctx.line)) if warn else ''), flush=True)
qInstallMessageHandler(handler)
app = QGuiApplication(sys.argv[:1])
QFontDatabase.addApplicationFont(qmlenv.FONT)
eng = QQmlApplicationEngine()
eng.addImportPath(stubs)
for k, v in (('schemaText', open(schema).read()), ('pluginDir', QUrl.fromLocalFile(plugin).toString()),
             ('pageW', W), ('pageH', H), ('layoutName', size + ' ' + scheme)):
    eng.rootContext().setContextProperty(k, v)
eng.load(QUrl.fromLocalFile(os.path.join(here, 'gxapp_test.qml')))
if not eng.rootObjects():
    print('HARNESS LOAD FAILED'); sys.exit(2)
QTimer.singleShot(120000, app.quit)
app.exec()
warn = [m for m in msgs if m[0] in (QtMsgType.QtWarningMsg, QtMsgType.QtCriticalMsg, QtMsgType.QtFatalMsg)]
fails = [m for m in msgs if m[1].startswith('FAIL')]
passes = [m for m in msgs if m[1].startswith('PASS')]
print(('PASS' if not warn else 'FAIL') + ' no Qt warnings: %d' % len(warn))
print('SUMMARY [%s %s]: %d PASS, %d FAIL, %d Qt warnings' % (size, scheme, len(passes) + (not warn), len(fails) + bool(warn), len(warn)))
sys.exit(1 if fails or warn or not any(m[1].startswith('DONE') for m in msgs) else 0)
