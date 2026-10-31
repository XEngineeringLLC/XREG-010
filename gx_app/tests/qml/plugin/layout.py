# Geometry checks for the 2026-10-28 5-inch layout defects at one GX layout:
#   python3 layout.py <plugin> <schema> <FiveInch|SevenInch|Portrait> <name=data.json>...
import sys, os, subprocess, shutil, json
sys.dont_write_bytecode = True
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(here))
import qmlenv
plugin, schema, size = sys.argv[1:4]
datas = dict(a.split('=', 1) for a in sys.argv[4:])
W, H = {'FiveInch': (800, 424), 'SevenInch': (1024, 544), 'Portrait': (383, 741)}[size]
stubs = qmlenv.stubs(size, 'Dark')
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
    print(('W ' if warn else '') + msg, flush=True)
qInstallMessageHandler(handler)
app = QGuiApplication(sys.argv[:1])
QFontDatabase.addApplicationFont(qmlenv.FONT)
eng = QQmlApplicationEngine()
eng.addImportPath(stubs)
for k, v in (('schemaText', open(schema).read()), ('pluginDir', QUrl.fromLocalFile(plugin).toString()),
             ('dataFiles', {n: open(p).read() for n, p in datas.items()}), ('setsText', json.dumps(list(datas))),
             ('pageW', W), ('pageH', H), ('layoutName', size)):
    eng.rootContext().setContextProperty(k, v)
eng.load(QUrl.fromLocalFile(os.path.join(here, 'layout.qml')))
QTimer.singleShot(180000, app.quit)
app.exec()
warn = [m for m in msgs if m[0] in (QtMsgType.QtWarningMsg, QtMsgType.QtCriticalMsg, QtMsgType.QtFatalMsg)]
fails = [m for m in msgs if m[1].startswith('FAIL')]
passes = [m for m in msgs if m[1].startswith('PASS')]
print(('PASS' if not warn else 'FAIL') + ' [%s] no Qt warnings: %d' % (size, len(warn)))
print('SUMMARY [%s]: %d PASS, %d FAIL, %d Qt warnings' % (size, len(passes) + (not warn), len(fails) + bool(warn), len(warn)))
sys.exit(1 if fails or warn or not any(m[1].startswith('DONE') for m in msgs) else 0)
