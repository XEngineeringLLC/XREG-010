# Plots tab-switch cost, offline on the Mac (Metal via QQuickRenderControl), relative only: python3 switch_timing.py <plugin dir> [cycles]
import sys, os, json, time, statistics, resource
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rcwin
from rcwin import app, eng, win, frame, seed, warns, shapes
from PySide6.QtCore import QUrl
from PySide6.QtQml import QQmlComponent

d = sys.argv[1]
cycles = int(sys.argv[2]) if len(sys.argv) > 2 else 6
seed()
comp = QQmlComponent(eng, QUrl.fromLocalFile(os.path.join(d, 'XregPlots.qml')))
if comp.status() != QQmlComponent.Ready:
    print(comp.errorString()); sys.exit(2)

def count_items(it):
    n = 0
    for c in it.childItems():
        n += 1 + count_items(c)
    return n

def rss_mb():
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1048576

helper_c = QQmlComponent(eng)
helper_c.setData(b"""import QtQuick
QtObject {
	function busy(it) {
		for (const c of it.children) {
			if (c.rendererType !== undefined && c.asynchronous !== undefined && c.visible && c.status === 2) return true
			if (busy(c)) return true
		}
		return false
	}
}""", QUrl.fromLocalFile(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'helper.qml')))
helper = helper_c.create()
from PySide6.QtCore import QMetaObject, Q_ARG, Q_RETURN_ARG
def busy(it):
    return QMetaObject.invokeMethod(helper, 'busy', Q_RETURN_ARG('QVariant'), Q_ARG('QVariant', it))
page = comp.createWithInitialProperties({'svc': 'mock', 'width': 1024, 'height': 544})
page.setParentItem(win.contentItem())
for _ in range(3):
    app.processEvents(); frame()
out = {'dir': os.path.basename(d.rstrip('/')), 'items_open': count_items(page)}
per = {1: [], 2: [], 3: [], 0: []}
first = {}
items_after = []
for c in range(cycles):
    for t in (1, 2, 3, 0):
        # from the tap to the frame that shows the tab's charts with data: the switch, the queued first redraw (the
        # old tab's Timer triggeredOnStart, posted), frames until no shown Shape is still processing
        t0 = time.perf_counter()
        page.setProperty('tab', t)
        t1 = time.perf_counter()
        app.processEvents()
        p, s, r = frame()
        n = 1
        while busy(page) and n < 30:
            app.processEvents(); frame(); n += 1
        t2 = time.perf_counter()
        rec = {'set_ms': (t1 - t0) * 1e3, 'first_frame_gui_ms': p + s, 'total_gui_ms': (t2 - t0) * 1e3, 'frames': n}
        if c == 0:
            first[t] = rec
        else:
            per[t].append(rec)
        for _ in range(2):
            app.processEvents(); frame()
    items_after.append(count_items(page))
names = {0: 'short', 1: 'long', 2: 'health', 3: 'wind'}
med = lambda a, k: round(statistics.median(x[k] for x in a), 2)
out['first_visit_total_gui_ms'] = {names[t]: round(first[t]['total_gui_ms'], 2) for t in first}
out['return_total_gui_ms_median'] = {names[t]: med(per[t], 'total_gui_ms') for t in per if per[t]}
out['return_set_ms_median'] = {names[t]: med(per[t], 'set_ms') for t in per if per[t]}
out['return_frames_to_ready_max'] = {names[t]: max(x['frames'] for x in per[t]) for t in per if per[t]}
out['items_after_each_cycle'] = items_after
out['warnings'] = len(warns)
print(json.dumps(out))
