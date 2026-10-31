# Cost of XregChartPerf.parseCurve() and refresh() (the curve sweep) with the demo /Perf/CurveCsv, offline on the Mac.
import os, sys, time, json, statistics
os.environ['QT_QPA_PLATFORM'] = 'offscreen'
from PySide6.QtCore import QUrl, QMetaObject
from PySide6.QtGui import QGuiApplication
from PySide6.QtQml import QQmlEngine, QQmlComponent

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import qmlenv
stubs = qmlenv.stubs('SevenInch', 'Dark', 'timing')
app = QGuiApplication(sys.argv)
eng = QQmlEngine()
eng.addImportPath(stubs)
store = eng.singletonInstance('Victron.VenusOS', 'MockStore')
demo = json.load(open(os.path.join(qmlenv.FIXTURES, 'demo.json')))
store.setProperty('values', {'mock/' + k: v for k, v in demo.items()})
comp = QQmlComponent(eng, QUrl.fromLocalFile(os.path.join(sys.argv[1], 'XregChartPerf.qml')))
for link in (True, False):
    perf = comp.createWithInitialProperties({'svc': 'mock', 'linkUp': link, 'width': 430, 'height': 348})
    L = lambda v: v.property('length').toInt() if hasattr(v, 'property') else len(v or [])
    cv = perf.property('curve')
    out = {'linkUp': link, 'view': perf.property('view'),
           'sail_pts': L(perf.property('sailFront')), 'motor_pts': L(perf.property('motorFront')),
           'curve_pts': cv.property('pts').property('length').toInt() if hasattr(cv, 'property') else len(cv['pts'])}
    ts = []
    for i in range(10):
        perf.setProperty('curveKey', '')
        t0 = time.perf_counter(); QMetaObject.invokeMethod(perf, 'refresh'); ts.append((time.perf_counter() - t0) * 1e3)
    out['refresh_ms_median'] = round(statistics.median(ts), 1)
    ts = []
    for i in range(5):
        t0 = time.perf_counter(); QMetaObject.invokeMethod(perf, 'parseCurve'); ts.append((time.perf_counter() - t0) * 1e3)
    out['parseCurve_incl_refresh_ms_median'] = round(statistics.median(ts), 1)
    print(json.dumps(out))
    perf.deleteLater()
