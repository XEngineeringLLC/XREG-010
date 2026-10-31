# Real pointer input on the plugin's switches through an offscreen window (Qt Quick Templates, the stub Victron.VenusOS
# with gui-v2 1.3.22's own ListSwitch.qml): taps, slides, holds, releases outside, cancels, refused and slow answers,
# by mouse and by touch. Every case asserts the state each write asked for and what the switch shows afterwards.
#   python3 toggle_test.py <plugin dir> <schema json> [case filter]
import sys, os, subprocess, json
sys.dont_write_bytecode = True
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(here))
import qmlenv
plugin, schema = sys.argv[1:3]
only = sys.argv[3] if len(sys.argv) > 3 else ''
stubs = qmlenv.stubs('SevenInch', 'Dark', 'switch')
os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
from PySide6.QtCore import QUrl, QPoint, Qt, QMetaObject, Q_RETURN_ARG, Q_ARG, qInstallMessageHandler, QtMsgType
from PySide6.QtGui import QGuiApplication
from PySide6.QtQml import QQmlApplicationEngine
from PySide6.QtTest import QTest
from PySide6.QtQuick import QQuickItem
import shiboken6

warnings = []


def handler(mode, ctx, msg):
    if mode in (QtMsgType.QtWarningMsg, QtMsgType.QtCriticalMsg, QtMsgType.QtFatalMsg) and 'Populating font' not in msg:
        warnings.append(msg)
        print('W ' + msg, flush=True)
    elif msg.startswith('FAIL'):
        print(msg, flush=True)


qInstallMessageHandler(handler)
app = QGuiApplication(sys.argv[:1])
eng = QQmlApplicationEngine()
eng.addImportPath(stubs)
schema_text = open(schema).read()
s = json.loads(schema_text)
for n in s['outline']:
    if n['id'] == 'engine':
        for c in n['children']:
            if c.get('key') == 'IgnitionOverride':
                c['confirm'] = 'Ignition Override changes when the field may come on.'
for k, v in (('schemaText', schema_text), ('confirmSchemaText', json.dumps(s)),
             ('pluginDir', QUrl.fromLocalFile(plugin).toString())):
    eng.rootContext().setContextProperty(k, v)
eng.load(QUrl.fromLocalFile(os.path.join(here, 'toggle.qml')))
win = eng.rootObjects()[0]
touch = QTest.createTouchDevice()
fails = 0
passes = 0
CONFIRM_WAIT = 2400   # past the switch's 2 s confirm window


def call(name, *args):
    return QMetaObject.invokeMethod(win, name, Q_RETURN_ARG('QVariant'), *[Q_ARG('QVariant', a) for a in args])


def check(cond, msg):
    global fails, passes
    if cond:
        passes += 1
    else:
        fails += 1
    print(('PASS ' if cond else 'FAIL ') + msg, flush=True)


def P(where, dx=0, dy=0):
    p = call('pt', where)
    return QPoint(min(win.width() - 2, max(1, round(p.x() + dx))), round(p.y() + dy))


class Pointer:
    def __init__(self, kind):
        self.kind = kind
        self.seq = None

    def press(self, p):
        if self.kind == 'mouse':
            QTest.mousePress(win, Qt.LeftButton, Qt.NoModifier, p)
        else:
            QTest.touchEvent(win, touch, False).press(0, p, win).commit()
        QTest.qWait(16)

    def move(self, p):
        if self.kind == 'mouse':
            QTest.mouseMove(win, p, 16)
        else:
            QTest.touchEvent(win, touch, False).move(0, p, win).commit()
            QTest.qWait(16)

    def release(self, p):
        if self.kind == 'mouse':
            QTest.mouseRelease(win, Qt.LeftButton, Qt.NoModifier, p)
        else:
            QTest.touchEvent(win, touch, False).release(0, p, win).commit()
        QTest.qWait(60)


def glide(ptr, a, b, step=5):
    n = max(1, round(max(abs(b.x() - a.x()), abs(b.y() - a.y())) / step))
    for i in range(1, n + 1):
        ptr.move(QPoint(round(a.x() + (b.x() - a.x()) * i / n), round(a.y() + (b.y() - a.y()) * i / n)))


def gesture(ptr, path, hold=0, mid=None):
    """press at path[0], hold, glide through path, call mid() before the release, release at the last point"""
    ptr.press(path[0])
    if hold:
        QTest.qWait(hold)
    for a, b in zip(path, path[1:]):
        glide(ptr, a, b)
    if mid:
        mid()
    ptr.release(path[-1])


def state():
    st = {'writes': call('writeCount'), 'sent': call('lastSent'), 'shown': call('shown'), 'knob': call('knobOn'),
          'device': call('device')}
    if st['knob'] != st['shown']:
        # the knob's 200 ms animation can finish late on a loaded machine; checked is what it animates to
        QTest.qWait(300)
        st['knob'] = call('knobOn')
    return st


def fmt(st):
    return 'writes %d, sent %d, shows %d (knob %d), device %d' % (st['writes'], st['sent'], st['shown'], st['knob'], st['device'])


def expect(tag, st, writes, sent, shown):
    # shown also checks the device: the switch at rest always shows the device's state
    ok = st['writes'] == writes and (writes == 0 or st['sent'] == sent) and st['shown'] == shown and st['knob'] == shown
    check(ok, '%s: %s (want writes %d%s, shows %d)' % (tag, fmt(st), writes, '' if writes == 0 else ', sent %d' % sent, shown))


def side(init):
    # the far end of the groove from the knob, and past the knob's own end
    return ('right', 20) if init == 0 else ('left', -20)


def own(init):
    return ('left', -30) if init == 0 else ('right', 30)


SWITCHES = ['header', 'row', 'draws', 'help']


def setup(kind, init, scroll=False):
    ok = call('setup', kind, init, scroll)
    QTest.qWait(250)   # past the knob's 200 ms animation
    return ok


def run_basic(kind, init, inp):
    tag = '%s %s %s' % (kind, inp, 'ON' if init else 'OFF')
    ptr = Pointer(inp)
    other, own_end = side(init), own(init)

    setup(kind, init)
    gesture(ptr, [P('knob')])
    QTest.qWait(250)
    expect(tag + ' tap', state(), 1, 1 - init, 1 - init)

    setup(kind, init)
    gesture(ptr, [P('knob'), P(other[0], other[1])])
    QTest.qWait(250)
    expect(tag + ' slide to the other side', state(), 1, 1 - init, 1 - init)

    setup(kind, init)
    gesture(ptr, [P('knob'), P(own_end[0], own_end[1])])
    QTest.qWait(250)
    expect(tag + ' slide toward its own side', state(), 0, -1, init)

    setup(kind, init)
    gesture(ptr, [P('knob'), P(other[0], other[1]), P(own_end[0], own_end[1] // 3)])
    QTest.qWait(250)
    expect(tag + ' slide across and back', state(), 0, -1, init)

    setup(kind, init)
    gesture(ptr, [P('knob')], hold=1000)
    QTest.qWait(250)
    expect(tag + ' hold 1 s, then release', state(), 1, 1 - init, 1 - init)

    setup(kind, init)
    gesture(ptr, [P('knob'), P(other[0], other[1])], hold=1000)
    QTest.qWait(250)
    expect(tag + ' hold 1 s, then slide across', state(), 1, 1 - init, 1 - init)

    setup(kind, init)
    k = P('knob')
    o = P(other[0], other[1])
    gesture(ptr, [k, o, QPoint(min(win.width() - 2, o.x() + (40 if init == 0 else -40)), o.y() + 70)])
    QTest.qWait(250)
    expect(tag + ' slide across, release outside the control', state(), 1, 1 - init, 1 - init)

    setup(kind, init)
    k = P('knob')
    e = P(own_end[0], own_end[1])
    gesture(ptr, [k, e, QPoint(e.x(), e.y() + 70)])
    QTest.qWait(250)
    expect(tag + ' slide toward its own side, release outside the control', state(), 0, -1, init)


def run_refused(kind, init, inp):
    tag = '%s %s %s' % (kind, inp, 'ON' if init else 'OFF')
    ptr = Pointer(inp)
    other = side(init)
    # the write is refused (no answer ever comes): shows the state asked for, then the device's again
    setup(kind, init)
    call('setNoEcho', True)
    gesture(ptr, [P('knob'), P(other[0], other[1])])
    QTest.qWait(250)
    st = state()
    expect(tag + ' slide across, no answer: at first (0.25 s)', st, 1, 1 - init, 1 - init)
    QTest.qWait(CONFIRM_WAIT - 250)
    expect(tag + ' slide across, no answer: after 2.4 s', state(), 1, 1 - init, init)

    # a slow answer (600 ms): shows the new state the whole time, no flicker on the way
    setup(kind, init)
    call('setNoEcho', True)
    gesture(ptr, [P('knob')])
    samples = []
    for i in range(int(3000 / 25)):
        if i == int(600 / 25):
            call('setNoEcho', False)
            call('putDevice', 1 - init)
        samples.append((call('shown'), call('device')))
        QTest.qWait(25)
    flips = sum(1 for a, b in zip(samples, samples[1:]) if a[0] != b[0])
    check(all(x[0] == 1 - init for x in samples) and samples[-1][1] == 1 - init and flips == 0,
          '%s tap, answer after 600 ms: shows %d in %d of %d samples over 3 s, %d flips, device %d'
          % (tag, 1 - init, sum(1 for x in samples if x[0] == 1 - init), len(samples), flips, samples[-1][1]))


def run_gated(kind, init, inp):
    # accepted, then put back by the device 1 s later (the bridge's hold ends and the regulator's state shows): follows it
    tag = '%s %s %s' % (kind, inp, 'ON' if init else 'OFF')
    ptr = Pointer(inp)
    other = side(init)
    setup(kind, init)
    gesture(ptr, [P('knob'), P(other[0], other[1])])
    QTest.qWait(1000)
    a = state()
    call('putDevice', init)
    QTest.qWait(250)
    b = state()
    check(a['shown'] == 1 - init and b['shown'] == init and b['knob'] == init and b['writes'] == 1,
          '%s slide across, device puts it back after 1 s: shows %d then %d (%s)' % (tag, a['shown'], b['shown'], fmt(b)))


def run_cancel(kind, init, inp):
    tag = '%s %s %s' % (kind, inp, 'ON' if init else 'OFF')
    ptr = Pointer(inp)
    other = side(init)
    if kind == 'header':
        # the link drops with the finger halfway across: the switch is disabled, Qt cancels the press
        setup(kind, init)
        k, o = P('knob'), P(other[0], other[1])
        half = QPoint((k.x() + o.x()) // 2, k.y())
        gesture(ptr, [k, half], mid=lambda: call('cancelLink'))
        QTest.qWait(100)
        call('restoreLink')
        QTest.qWait(250)
        expect(tag + ' halfway across, link drops, then release', state(), 0, -1, init)
    # (Mark, 2026-10-29) a press that leaves the switch without sliding and lifts outside it asks for nothing; Qt alone
    # counts it as a tap (Switch keeps the press, keepPressed). No list scrolls here, so nothing takes the drag.
    setup(kind, init)
    k = P('knob')
    gesture(ptr, [k, QPoint(k.x(), k.y() + 70)])
    QTest.qWait(250)
    expect(tag + ' press, leave downward without sliding, lift outside', state(), 0, -1, init)
    if kind in ('row', 'help', 'draws'):
        # a vertical drag from the switch in a list that scrolls: the list takes it, Qt cancels the press
        setup(kind, init, True)
        d = call('scrollToRow')
        QTest.qWait(250)
        y0 = call('listY')
        k = P('knob')
        gesture(ptr, [k, QPoint(k.x(), k.y() + 80 * d)])
        QTest.qWait(250)
        y1 = call('listY')
        st = state()
        check(d != 0 and abs(y1 - y0) > 20 and st['writes'] == 0 and st['shown'] == init and st['knob'] == init,
              '%s vertical drag from the switch, the list scrolls (%.0f -> %.0f): %s' % (tag, y0, y1, fmt(st)))
    # the grab taken away mid-slide (as a Flickable or a popup taking it), then release outside
    setup(kind, init)
    k, o = P('knob'), P(other[0], other[1])
    half = QPoint((k.x() + o.x()) // 2, k.y())
    ptr.press(k)
    glide(ptr, k, half)
    pos = call('position')
    item = shiboken6.wrapInstance(shiboken6.getCppPointer(call('sw'))[0], QQuickItem)
    item.ungrabMouse()
    item.ungrabTouchPoints()
    QTest.qWait(30)
    pressed = call('swPressed')
    ptr.release(QPoint(half.x(), half.y() + 120))
    QTest.qWait(250)
    st = state()
    check(not pressed and st['writes'] == 0 and st['shown'] == init and st['knob'] == init,
          '%s grab taken halfway (position %.2f): cancelled, %s' % (tag, pos, fmt(st)))


def run_confirm(inp):
    ptr = Pointer(inp)
    for init in (0, 1):
        tag = 'confirmrow %s %s' % (inp, 'ON' if init else 'OFF')
        other = side(init)
        setup('confirmrow', init)
        gesture(ptr, [P('knob'), P(other[0], other[1])])
        QTest.qWait(150)
        d = call('dialogCount')
        st = state()
        call('rejectTop')
        QTest.qWait(250)
        st2 = state()
        check(d == 1 and st['writes'] == 0 and st2['writes'] == 0 and st2['shown'] == init and st2['knob'] == init,
              '%s slide across, asked, Cancel: dialog %d, while asking shows %d, after %s' % (tag, d, st['shown'], fmt(st2)))
        setup('confirmrow', init)
        gesture(ptr, [P('knob'), P(other[0], other[1])])
        QTest.qWait(150)
        call('acceptTop')
        QTest.qWait(250)
        expect(tag + ' slide across, asked, OK', state(), 1, 1 - init, 1 - init)


def run_bare(inp):
    # Qt itself: a Templates Switch with checkable false and checked bound to a value, as gui-v2's Switch is used
    ptr = Pointer(inp)
    setup('bare', 0)
    gesture(ptr, [P('knob'), P('right', 20)])
    QTest.qWait(250)
    a = (call('shown'), call('device'), call('writeCount'))
    call('putDevice', 1)
    call('putDevice', 0)
    QTest.qWait(250)
    b = call('shown')
    check(a == (1, 0, 1) and b == 0,
          'Qt %s: checkable false, slid right: checked %d with the bound value still %d, clicked %d time(s); the binding '
          'was left in place, stale (it applied again when the value changed: checked %d)' % (inp, a[0], a[1], a[2], b))
    setup('bare', 0)
    gesture(ptr, [P('knob')])
    QTest.qWait(250)
    check(call('shown') == 0 and call('writeCount') == 1, 'Qt %s: checkable false, tapped: checked stays %d, clicked %d'
          % (inp, call('shown'), call('writeCount')))
    setup('bare', 0)
    gesture(ptr, [P('knob'), P('left', -30)])
    QTest.qWait(250)
    check(call('shown') == 0 and call('writeCount') == 1, 'Qt %s: checkable false, slid left from off: checked %d, clicked %d '
          '(clicked fires for a slide that changes nothing)' % (inp, call('shown'), call('writeCount')))


def run_keys(kind, init):
    tag = '%s %s' % (kind, 'ON' if init else 'OFF')
    setup(kind, init)
    call('codeClick')
    QTest.qWait(250)
    expect(tag + ' click() from code', state(), 1, 1 - init, 1 - init)
    setup(kind, init)
    focused = call('focusForKeys')
    QTest.keyClick(win, Qt.Key_Space)
    QTest.qWait(250)
    st = state()
    ok = focused and st['writes'] == 1 and st['sent'] == 1 - init and st['shown'] == 1 - init and st['knob'] == 1 - init
    check(ok, '%s space key (focused %s): %s (want writes 1, sent %d, shows %d)' % (tag, focused, fmt(st), 1 - init, 1 - init))


def run_range():
    ptr = Pointer('mouse')
    call('setupRange', '24h')
    QTest.qWait(200)
    p = call('rangePt', 3)
    gesture(ptr, [QPoint(round(p.x()), round(p.y()))])
    QTest.qWait(100)
    i1 = call('rangeIndex')
    call('putDevice', '1h')    # another screen picks 1 h (/Ui/LongRange is the bridge's, one for every screen)
    QTest.qWait(100)
    i2 = call('rangeIndex')
    check(call('lastRange') == '7d' and i1 == 3 and i2 == 0,
          'Long Term range: 7d tapped, shows %d; another screen picks 1h, shows %d (want 3 then 0)' % (i1, i2))
    call('setupRange', '24h')
    call('setNoEcho', True)
    QTest.qWait(200)
    p = call('rangePt', 1)
    gesture(ptr, [QPoint(round(p.x()), round(p.y()))])
    QTest.qWait(100)
    i1 = call('rangeIndex')
    QTest.qWait(CONFIRM_WAIT)
    i2 = call('rangeIndex')
    check(call('lastRange') == '6h' and i1 == 1 and i2 == 2,
          'Long Term range: 6h tapped, no answer: shows %d, after 2.4 s %d (want 1 then 2, the device\'s 24h)' % (i1, i2))


QTest.qWait(300)
groups = []
for kind in SWITCHES:
    for inp in ('mouse', 'touch'):
        for init in (0, 1):
            groups.append(('basic', kind, inp, init))
for kind in ('header', 'row', 'draws'):
    for inp in ('mouse', 'touch'):
        for init in (0, 1):
            groups.append(('refused', kind, inp, init))
            groups.append(('gated', kind, inp, init))
for kind in SWITCHES:
    for inp in ('mouse', 'touch'):
        for init in (0, 1):
            groups.append(('cancel', kind, inp, init))
for inp in ('mouse', 'touch'):
    groups.append(('confirm', '', inp, 0))
    groups.append(('bare', '', inp, 0))
groups.append(('range', '', 'mouse', 0))
for kind in SWITCHES:
    for init in (0, 1):
        groups.append(('keys', kind, 'keyboard', init))
for g, kind, inp, init in groups:
    if only and only not in '%s %s %s' % (g, kind, inp):
        continue
    if g == 'basic':
        run_basic(kind, init, inp)
    elif g == 'refused':
        run_refused(kind, init, inp)
    elif g == 'gated':
        run_gated(kind, init, inp)
    elif g == 'cancel':
        run_cancel(kind, init, inp)
    elif g == 'confirm':
        run_confirm(inp)
    elif g == 'bare':
        run_bare(inp)
    elif g == 'range':
        run_range()
    elif g == 'keys':
        run_keys(kind, init)
check(not warnings, 'no Qt warnings (%d)' % len(warnings))
print('SUMMARY: %d PASS, %d FAIL, %d Qt warnings' % (passes, fails, len(warnings)), flush=True)
sys.exit(1 if fails else 0)
