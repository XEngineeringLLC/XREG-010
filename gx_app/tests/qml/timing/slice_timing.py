# Sliced sweep on the Mac: each event-loop turn's GUI-thread time while a sweep runs (perf_counter), total, steps per
# slice; inline sweep for comparison. python3 slice_timing.py <inline dir> <sliced dir> [runs]
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
eng = QQmlEngine()
eng.addImportPath(stubs)
store = eng.singletonInstance('Victron.VenusOS', 'MockStore')
demo = json.load(open(os.path.join(qmlenv.FIXTURES, 'demo.json')))
store.setProperty('values', {'mock/' + k: v for k, v in demo.items()})
runs = int(sys.argv[3]) if len(sys.argv) > 3 else 10
out = {}
for label, d in (('inline', sys.argv[1]), ('sliced', sys.argv[2])):
    comp = QQmlComponent(eng, QUrl.fromLocalFile(os.path.join(d, 'XregChartPerf.qml')))
    for view in (0, 1):
        perf = comp.createWithInitialProperties({'svc': 'mock', 'linkUp': True, 'width': 430, 'height': 348, 'pinned': view})
        for _ in range(50):
            app.processEvents()
        turns, totals, nturns = [], [], []
        for i in range(runs):
            perf.setProperty('curveKey', '')
            t0 = time.perf_counter()
            QMetaObject.invokeMethod(perf, 'refresh')
            t1 = time.perf_counter()
            tt = [(t1 - t0) * 1e3]
            n = 0
            while label == 'sliced' and perf.property('sweeping') and n < 10000:
                a = time.perf_counter(); app.processEvents(); tt.append((time.perf_counter() - a) * 1e3); n += 1
            turns += tt
            totals.append(sum(tt))
            nturns.append(n)
        pts = perf.property('curve').property('pts').property('length').toInt() if hasattr(perf.property('curve'), 'property') else None
        out['%s view %d' % (label, view)] = {
            'gui_ms_total_median': round(statistics.median(totals), 2),
            'longest_turn_ms': round(max(turns), 2), 'turn_ms_median': round(statistics.median(turns[1:] if label == 'sliced' else turns), 2),
            'refresh_call_ms_median': round(statistics.median(t for t in turns[::(len(turns) // runs)]), 3),
            'slices_per_sweep': statistics.median(nturns) if label == 'sliced' else 0}
        perf.deleteLater(); app.processEvents()
print(json.dumps(out, indent=1))
