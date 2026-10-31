# Health with change-only session chart inputs vs the old page: python3 health_test.py <old plugin dir> <new plugin dir> <health json>
# The first speed set's pages (2026-10-30), the old side of the comparison, are kept in ref_speed1/: the files these
# tests create from it (XregChartPerf with the inline sweep, XregPlotsLong, XregPlotsHealth, XregChart, XregNum, XregSync).
import sys, os, subprocess, shutil, json
sys.dont_write_bytecode = True
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(here))
import qmlenv
old, new = sys.argv[1], sys.argv[2]
stubs = qmlenv.stubs('SevenInch', 'Dark')
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
eng.rootContext().setContextProperty('oldDir', QUrl.fromLocalFile(old).toString())
eng.rootContext().setContextProperty('newDir', QUrl.fromLocalFile(new).toString())
eng.rootContext().setContextProperty('healthText', open(sys.argv[3]).read())
eng.load(QUrl.fromLocalFile(os.path.join(here, 'health_test.qml')))
if not eng.rootObjects():
    print('HARNESS LOAD FAILED'); sys.exit(2)
QTimer.singleShot(120000, app.quit)
app.exec()
warn = [m for m in msgs if m[0] in (QtMsgType.QtWarningMsg, QtMsgType.QtCriticalMsg, QtMsgType.QtFatalMsg)]
fails = [m for m in msgs if m[1].startswith('FAIL')]
passes = [m for m in msgs if m[1].startswith('PASS')]
print(('PASS' if not warn else 'FAIL') + ' no Qt warnings: %d' % len(warn))
print('SUMMARY [health-q]: %d PASS, %d FAIL, %d Qt warnings' % (len(passes) + (not warn), len(fails) + bool(warn), len(warn)))
sys.exit(1 if fails or warn or not any(m[1].startswith('DONE') for m in msgs) else 0)
