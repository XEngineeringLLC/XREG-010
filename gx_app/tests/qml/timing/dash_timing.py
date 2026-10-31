# Dashboard opening on the Mac (Metal via QQuickRenderControl), relative only: creation (one GUI-thread block), first
# frame, then until the speed tile shows its curve; longest single GUI-thread block. python3 dash_timing.py <plugin dir> [runs]
import sys, os, json, time, statistics
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rcwin import app, eng, win, frame, seed, warns
from PySide6.QtCore import QUrl
from PySide6.QtQml import QQmlComponent
seed()
d = sys.argv[1]
runs = int(sys.argv[2]) if len(sys.argv) > 2 else 6
comp = QQmlComponent(eng, QUrl.fromLocalFile(os.path.join(d, 'XregDash.qml')))
if comp.status() != QQmlComponent.Ready:
    print(comp.errorString()); sys.exit(2)
def find_perf(it):
    for c in it.childItems():
        mo = c.metaObject()
        if mo.indexOfProperty('curveKey') >= 0:
            return c
        r = find_perf(c)
        if r: return r
    return None
rec = []
for i in range(runs + 1):
    t0 = time.perf_counter()
    dash = comp.createWithInitialProperties({'svc': 'mock', 'width': 1024, 'height': 544})
    dash.setParentItem(win.contentItem())
    t1 = time.perf_counter()
    blocks = [(t1 - t0) * 1e3]
    a = time.perf_counter(); p, s, r = frame(); blocks.append((time.perf_counter() - a) * 1e3)
    perf = find_perf(dash)
    n = 0
    while perf.property('sweeping') if perf.metaObject().indexOfProperty('sweeping') >= 0 else False:
        a = time.perf_counter(); app.processEvents(); blocks.append((time.perf_counter() - a) * 1e3)
        a = time.perf_counter(); frame(); blocks.append((time.perf_counter() - a) * 1e3)
        n += 1
        if n > 5000: break
    t2 = time.perf_counter()
    pts = perf.property('curve').property('pts').property('length').toInt()
    if i:
        rec.append({'create': blocks[0], 'to_curve': (t2 - t0) * 1e3, 'longest_block': max(blocks), 'pts': pts, 'turns': n})
    dash.setParentItem(None); dash.deleteLater()
    for _ in range(3):
        app.processEvents(); frame()
m = lambda k: round(statistics.median(x[k] for x in rec), 1)
print(json.dumps({'dir': os.path.basename(d.rstrip('/')), 'create_ms': m('create'), 'open_to_curve_ms': m('to_curve'),
                  'longest_gui_block_ms': m('longest_block'), 'curve_pts': rec[0]['pts'], 'turns': m('turns'), 'warnings': len(warns)}))
