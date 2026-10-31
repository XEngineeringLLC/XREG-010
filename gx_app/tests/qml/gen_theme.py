# Theme.qml for the stub Victron.VenusOS module, from gui-v2 1.3.22's own theme JSON (screen size and colour scheme).
import json, os, re, sys
sys.dont_write_bytecode = True
import qmlenv
G = os.path.join(qmlenv.GUIV2, 'themes')
size, scheme, out = sys.argv[1], sys.argv[2], sys.argv[3]
d = {}
for f in ('color/ColorDesign.json', 'color/%s.json' % scheme, 'geometry/%s.json' % size, 'typography/TypographyDesign.json',
          'typography/%s.json' % size, 'animation/Animation.json'):
    d.update(json.load(open(os.path.join(G, f))))
def res(v, depth=0):
    while isinstance(v, str) and v in d and depth < 10:
        v, depth = d[v], depth + 1
    return v
W, H = {'FiveInch': (800, 480), 'SevenInch': (1024, 600), 'Portrait': (383, 793)}[size]
lines = ['pragma Singleton', 'import QtQuick', 'QtObject {',
         '\tenum ScreenSize { FiveInch, SevenInch, Portrait }', '\tenum ColorScheme { Dark, Light }',
         '\tproperty int screenSize: %d' % {'FiveInch': 0, 'SevenInch': 1, 'Portrait': 2}[size],
         '\tproperty int colorScheme: %d' % (0 if scheme == 'Dark' else 1),
         '\tproperty real geometry_screen_width: %d' % W, '\tproperty real geometry_screen_height: %d' % H,
         '\tproperty bool virtualKeyboardOpened: false',
         '\tfunction objectHasQObjectParent(o) { return false }']
for k in sorted(d):
    v = res(d[k])
    if k.startswith('color_') and isinstance(v, str):
        lines.append('\tproperty color %s: "%s"' % (k, v))
    elif isinstance(v, (int, float)) and not isinstance(v, bool):
        lines.append('\tproperty real %s: %s' % (k, v))
    elif isinstance(v, str):
        lines.append('\tproperty string %s: %s' % (k, json.dumps(v)))
lines.append('}')
open(out, 'w').write('\n'.join(lines) + '\n')
