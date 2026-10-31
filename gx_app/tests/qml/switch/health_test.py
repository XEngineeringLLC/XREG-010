# Two screens on the Health tab at different windows: each must show only its own window's trend.
#   python3 health_test.py <plugin dir>
import sys, os, subprocess
sys.dont_write_bytecode = True
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(here))
import qmlenv
plugin = sys.argv[1]
stubs = qmlenv.stubs('SevenInch', 'Dark', 'switch')
os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
from PySide6.QtCore import QUrl, QMetaObject, Q_RETURN_ARG, Q_ARG, qInstallMessageHandler, QtMsgType
from PySide6.QtGui import QGuiApplication
from PySide6.QtQml import QQmlApplicationEngine
from PySide6.QtTest import QTest

warnings = []


def handler(mode, ctx, msg):
    if mode in (QtMsgType.QtWarningMsg, QtMsgType.QtCriticalMsg, QtMsgType.QtFatalMsg) and 'Populating font' not in msg:
        warnings.append(msg)
        print('W ' + msg, flush=True)


qInstallMessageHandler(handler)
app = QGuiApplication(sys.argv[:1])
eng = QQmlApplicationEngine()
eng.addImportPath(stubs)
eng.rootContext().setContextProperty('pluginDir', QUrl.fromLocalFile(plugin).toString())
eng.load(QUrl.fromLocalFile(os.path.join(here, 'health.qml')))
win = eng.rootObjects()[0]
fails = passes = 0


def call(name, *args):
    return QMetaObject.invokeMethod(win, name, Q_RETURN_ARG('QVariant'), *[Q_ARG('QVariant', a) for a in args])


def check(cond, msg):
    global fails, passes
    passes += bool(cond)
    fails += not cond
    print(('PASS ' if cond else 'FAIL ') + msg, flush=True)


QTest.qWait(200)
for w1, w2 in ((10, 0), (100, 10), (0, 100)):
    call('setup', w1, w2)
    QTest.qWait(100)
    seen = []
    for _ in range(10):
        for i in (0, 1):
            call('ask', i)
            QTest.qWait(30)
            seen.append((call('shows', 0), call('shows', 1)))
    bad0 = sum(1 for a, _ in seen if a != w1)
    bad1 = sum(1 for _, b in seen if b != w2)
    name = lambda w: '%d h' % w if w else 'All'
    check(bad0 == 0 and bad1 == 0, 'screens at %s and %s, 10 asks each: %d samples, other window shown %d and %d times'
          % (name(w1), name(w2), len(seen), bad0, bad1))
    paths = call('writePaths')
    check('/Ui/HealthHours' not in paths and '/Ui/Want/HealthTrend/%d' % w1 in paths and '/Ui/Want/HealthTrend/%d' % w2 in paths,
          'writes went to %s' % paths)
# one screen changes window while the other stays: from the change on, never the old window under the new button
call('setup', 10, 0)
QTest.qWait(100)
call('setWindow', 0, 100)
seen = []
for _ in range(20):
    call('ask', 1)
    QTest.qWait(25)
    seen.append((call('shows', 0), call('shows', 1)))
check(all(a in (-1, 100) for a, _ in seen) and seen[-1][0] == 100 and all(b == 0 for _, b in seen),
      '10 h -> 100 h with the other screen at All: windows shown %s and %s' % (sorted({a for a, _ in seen}), sorted({b for _, b in seen})))
check(not warnings, 'no Qt warnings (%d)' % len(warnings))
print('SUMMARY: %d PASS, %d FAIL, %d Qt warnings' % (passes, fails, len(warnings)), flush=True)
sys.exit(1 if fails else 0)
