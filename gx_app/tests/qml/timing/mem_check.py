# Items and resident memory of the Plots page with one tab vs all four kept: python3 mem_check.py <plugin dir>
import sys, os, json, subprocess
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rcwin import app, eng, win, frame, seed, warns
from PySide6.QtCore import QUrl
from PySide6.QtQml import QQmlComponent

def rss_kb():
    return int(subprocess.check_output(['ps', '-o', 'rss=', '-p', str(os.getpid())]).strip())
def settle(n=6):
    for _ in range(n):
        eng.collectGarbage(); app.processEvents(); frame()
def count_items(it):
    return sum(1 + count_items(c) for c in it.childItems())

seed()
comp = QQmlComponent(eng, QUrl.fromLocalFile(os.path.join(sys.argv[1], 'XregPlots.qml')))
# warm: create and drop one page with every tab visited, so the comparison is not the first load of each file
p = comp.createWithInitialProperties({'svc': 'mock', 'width': 1024, 'height': 544}); p.setParentItem(win.contentItem())
for t in (1, 2, 3, 0):
    p.setProperty('tab', t); settle(2)
p.setParentItem(None); p.deleteLater(); settle()
r0 = rss_kb()
page = comp.createWithInitialProperties({'svc': 'mock', 'width': 1024, 'height': 544}); page.setParentItem(win.contentItem())
settle()
r1, i1 = rss_kb(), count_items(page)
for c in range(3):
    for t in (1, 2, 3, 0):
        page.setProperty('tab', t); settle(2)
settle()
r2, i2 = rss_kb(), count_items(page)
print(json.dumps({'dir': os.path.basename(sys.argv[1].rstrip('/')), 'rss_before_page_kb': r0, 'page_open_short_only_kb': r1 - r0,
                  'after_3_cycles_back_on_short_kb': r2 - r0, 'items_short_only': i1, 'items_after_cycles': i2, 'warnings': len(warns)}))
