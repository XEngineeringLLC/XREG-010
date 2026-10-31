# New-behaviour checks for the 2026-10-30 speed set: python3 new_test.py <plugin dir> [Dark|Light]
import sys, os, subprocess, shutil, json
sys.dont_write_bytecode = True
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(here))
import qmlenv
plugin = sys.argv[1]
scheme = sys.argv[2] if len(sys.argv) > 2 else 'Dark'
stubs = qmlenv.stubs('SevenInch', scheme)
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
demo = json.load(open(os.path.join(qmlenv.FIXTURES, 'demo.json')))
eng = QQmlApplicationEngine()
eng.addImportPath(stubs)
eng.rootContext().setContextProperty('pluginDir', QUrl.fromLocalFile(plugin).toString())
eng.rootContext().setContextProperty('demoText', json.dumps(demo))
eng.load(QUrl.fromLocalFile(os.path.join(here, 'new_test.qml')))
if not eng.rootObjects():
    print('HARNESS LOAD FAILED'); sys.exit(2)
QTimer.singleShot(60000, app.quit)
app.exec()
logline = [m[1] for m in msgs if m[1].startswith('xreg timing: ')]
ok = logline == ['xreg timing: probe n=2 avg=7.0 max=10 ms, other n=1 avg=1.0 max=1 ms']
print(('PASS' if ok else 'FAIL') + ' timing log line: ' + repr(logline))
warn = [m for m in msgs if m[0] in (QtMsgType.QtWarningMsg, QtMsgType.QtCriticalMsg, QtMsgType.QtFatalMsg)]
fails = [m for m in msgs if m[1].startswith('FAIL')] + ([] if ok else ['log'])
passes = [m for m in msgs if m[1].startswith('PASS')] + ([1] if ok else [])
print(('PASS' if not warn else 'FAIL') + ' no Qt warnings: %d' % len(warn))
print('SUMMARY [new %s]: %d PASS, %d FAIL, %d Qt warnings' % (scheme, len(passes) + (not warn), len(fails) + bool(warn), len(warn)))
sys.exit(1 if fails or warn or not any(m[1].startswith('DONE') for m in msgs) else 0)
