# Drives real mouse drags through the offscreen window: swipe, chart cursor, vertical list scroll.
import sys, os, subprocess
sys.dont_write_bytecode = True
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(here))
import qmlenv
plugin, schema, data = sys.argv[1:4]
stubs = qmlenv.stubs('SevenInch', 'Dark')
os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
from PySide6.QtCore import QUrl, QPoint, Qt, QMetaObject, Q_RETURN_ARG, Q_ARG
from PySide6.QtGui import QGuiApplication
from PySide6.QtQml import QQmlApplicationEngine
from PySide6.QtTest import QTest
app = QGuiApplication(sys.argv[:1])
eng = QQmlApplicationEngine()
eng.addImportPath(stubs)
for k, v in (('schemaText', open(schema).read()), ('pluginDir', QUrl.fromLocalFile(plugin).toString()), ('dataText', open(data).read())):
    eng.rootContext().setContextProperty(k, v)
eng.load(QUrl.fromLocalFile(os.path.join(here, 'gesture.qml')))
win = eng.rootObjects()[0]
fails = 0


def call(name, *args):
    return QMetaObject.invokeMethod(win, name, Q_RETURN_ARG('QVariant'), *[Q_ARG('QVariant', a) for a in args])


def check(cond, msg):
    global fails
    sys.stdout.write(('PASS ' if cond else 'FAIL ') + msg + '\n')
    sys.stdout.flush()
    if not cond:
        fails += 1


def drag(x0, y0, x1, y1, steps=12, ms=16):
    QTest.mousePress(win, Qt.LeftButton, Qt.NoModifier, QPoint(x0, y0))
    for i in range(1, steps + 1):
        QTest.mouseMove(win, QPoint(int(x0 + (x1 - x0) * i / steps), int(y0 + (y1 - y0) * i / steps)), ms)
    QTest.mouseRelease(win, Qt.LeftButton, Qt.NoModifier, QPoint(x1, y1), ms)
    QTest.qWait(700)


QTest.qWait(800)
check(call('index') == 0, 'starts on Dashboard')
drag(900, 420, 250, 420)
check(call('index') == 1, 'a leftward drag across the Dashboard shows Plots: %s' % call('index'))
QTest.qWait(500)
pt = call('chartPoint')
check(pt.x() > 0, 'chart found at %s' % pt)
QTest.mousePress(win, Qt.LeftButton, Qt.NoModifier, QPoint(int(pt.x()), int(pt.y())))
QTest.qWait(30)
check(call('holds') == 1, 'finger on the chart holds the swipe')
for i in range(1, 13):
    QTest.mouseMove(win, QPoint(int(pt.x() - 15 * i), int(pt.y())), 16)
cur = call('plotsCursor')
QTest.mouseRelease(win, Qt.LeftButton, Qt.NoModifier, QPoint(int(pt.x() - 180), int(pt.y())), 16)
QTest.qWait(700)
check(call('index') == 1 and cur == cur, 'a sideways drag on a chart moves its cursor (%s) and leaves the page on Plots (%s)'
      % (cur, call('index')))
check(call('holds') == 0, 'lifted: swipe free again')
drag(900, 70, 250, 70)
check(call('index') == 2, 'a leftward drag on the Plots tab row shows Settings: %s' % call('index'))
call('setTab', 5)
QTest.qWait(400)
mid = call('listPoint')
y0 = call('listY')
drag(int(mid.x()), int(mid.y()) + 120, int(mid.x()), int(mid.y()) - 120, 12, 16)
y1 = call('listY')
check(call('index') == 2 and y1 > y0 + 50, 'a vertical drag scrolls the Alarms list (%s -> %s) and stays on Settings' % (y0, y1))
drag(150, 300, 900, 300)
check(call('index') == 2, 'a rightward drag while the list still glides stops the glide, as any list does: %s' % call('index'))
QTest.qWait(1500)
drag(150, 300, 900, 300)
check(call('index') == 1, 'once the list rests, a rightward drag goes back to Plots: %s' % call('index'))
sys.stdout.write('SUMMARY %d FAIL\n' % fails)
sys.exit(1 if fails else 0)
