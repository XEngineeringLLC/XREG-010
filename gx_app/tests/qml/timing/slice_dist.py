import os, sys, time, json, statistics
os.environ['QT_QPA_PLATFORM'] = 'offscreen'
from PySide6.QtCore import QUrl, QMetaObject
from PySide6.QtGui import QGuiApplication
from PySide6.QtQml import QQmlEngine, QQmlComponent
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import qmlenv
stubs = qmlenv.stubs('SevenInch', 'Dark', 'timing')
app = QGuiApplication(sys.argv[:1])
eng = QQmlEngine(); eng.addImportPath(stubs)
store = eng.singletonInstance('Victron.VenusOS', 'MockStore')
demo = json.load(open(os.path.join(qmlenv.FIXTURES, 'demo.json')))
store.setProperty('values', {'mock/' + k: v for k, v in demo.items()})
comp = QQmlComponent(eng, QUrl.fromLocalFile(os.path.join(sys.argv[1], 'XregChartPerf.qml')))
res = {}
for view in (0, 1):
    perf = comp.createWithInitialProperties({'svc': 'mock', 'linkUp': True, 'width': 430, 'height': 348, 'pinned': view})
    for _ in range(50): app.processEvents()
    allt, last, first, mid = [], [], [], []
    for i in range(int(sys.argv[2])):
        perf.setProperty('curveKey', '')
        QMetaObject.invokeMethod(perf, 'refresh')
        tt = []
        while perf.property('sweeping'):
            a = time.perf_counter(); app.processEvents(); tt.append((time.perf_counter() - a) * 1e3)
        allt += tt; first.append(tt[0]); last.append(tt[-1]); mid += tt[1:-1]
    q = lambda a, p: round(sorted(a)[min(len(a) - 1, int(p * len(a)))], 2)
    res[view] = {'turns': len(allt), 'median': q(allt, 0.5), 'p95': q(allt, 0.95), 'p99': q(allt, 0.99), 'max': round(max(allt), 2),
                 'over5': sum(1 for t in allt if t > 5), 'first_max': round(max(first), 2), 'last_median': q(last, 0.5), 'last_max': round(max(last), 2), 'mid_max': round(max(mid), 2)}
    perf.deleteLater(); app.processEvents()
print(json.dumps(res))
