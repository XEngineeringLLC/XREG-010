# The larger views, by real pointer input on the offscreen window: Short Term's charts (padded autoscale that keeps every
# line inside the plot, tap to open, Autoscale, Lock axes, the limit box, axis limits shared with the web app through
# /Plot/YLim/*, settings kept across a restart), a Long Term chart (limits kept on the GX), the Dashboard's chips (each
# panel as big as its content, highs and lows) and its barometer tile (the tile follows its larger view); then the Health
# and Wind tabs' charts and the Dashboard's wind, power-flow and speed tiles; the speed tile's layout at each GX's size;
# touch input, as the GX's touchscreen sends it, on everything that opens a larger view (test_* below).
#   python3 pop_test.py <plugin dir>
import json, sys, os
sys.dont_write_bytecode = True
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(here))
import qmlenv
plugin = sys.argv[1]
stubs = qmlenv.stubs('SevenInch', 'Dark')
os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
from PySide6.QtCore import QUrl, QPoint, Qt, QMetaObject, Q_RETURN_ARG, Q_ARG, qInstallMessageHandler, QtMsgType
from PySide6.QtGui import QGuiApplication
from PySide6.QtQml import QQmlApplicationEngine
from PySide6.QtTest import QTest

warnings = []


def handler(mode, ctx, msg):
    if mode in (QtMsgType.QtWarningMsg, QtMsgType.QtCriticalMsg, QtMsgType.QtFatalMsg) and 'Populating font' not in msg:
        warnings.append(msg)
        print('W ' + msg, flush=True)
    elif msg.startswith('FAIL'):
        warnings.append(msg)
        print(msg, flush=True)


qInstallMessageHandler(handler)
app = QGuiApplication(sys.argv[:1])
eng = QQmlApplicationEngine()
eng.addImportPath(stubs)
eng.rootContext().setContextProperty('pluginDir', QUrl.fromLocalFile(plugin).toString())
eng.load(QUrl.fromLocalFile(os.path.join(here, 'pop.qml')))
win = eng.rootObjects()[0]
fails = passes = 0


def call(name, *args):
    return QMetaObject.invokeMethod(win, name, Q_RETURN_ARG('QVariant'), *[Q_ARG('QVariant', a) for a in args])


def js(name, *args):
    return json.loads(call(name, *args))


def check(cond, msg):
    global fails, passes
    passes += bool(cond)
    fails += not cond
    print(('PASS ' if cond else 'FAIL ') + msg, flush=True)


def click(pt):
    p = QPoint(int(pt.x()), int(pt.y()))
    QTest.mouseClick(win, Qt.LeftButton, Qt.NoModifier, p)
    QTest.qWait(150)


def type_text(text):
    # QTest.keyClicks takes only a QWidget
    for ch in text:
        QTest.keyClick(win, ch)
    QTest.qWait(50)


def drag(pt, dx):
    x0, y0 = int(pt.x()), int(pt.y())
    QTest.mousePress(win, Qt.LeftButton, Qt.NoModifier, QPoint(x0, y0))
    for i in range(1, 13):
        QTest.mouseMove(win, QPoint(int(x0 + dx * i / 12), y0), 16)
    QTest.mouseRelease(win, Qt.LeftButton, Qt.NoModifier, QPoint(x0 + dx, y0), 16)
    QTest.qWait(300)


QTest.qWait(100)
check(call('start', False), 'Short Term page created')
QTest.qWait(200)


def feed(first, n, full, alt_fn):
    # alt_fn(i) -> alternator amps; the rest follow it
    rows = {}
    for i in range(first, n + 1):
        a = alt_fn(i)
        rows[i] = {'MeasuredAmps': a, 'Bcur': round(a * 0.5 - 5, 2), 'iiout': 3, 'dutyCycle': 50, 'pidAltPV': a,
                   'BatteryV': 13.5, 'IBV': 13.6, 'RPM': 1500, 'AlternatorTemperatureC': 60, 'chargeStage': 1}
    call('feedRows', 'e1', first, n, full, json.dumps(rows))
    QTest.qWait(700 if full else 300)


# Samples 1..600, 100 ms apart: alternator 20 + 30 sin, battery half of it less 5, field 3 A at 50 %, and one 90 A
# alternator reading at sample 299, 100 ms older than the 30 s window's left edge: inside what the smooth scroll still
# shows there (it trails the newest sample by at least 0.1 s)
import math
base = lambda i: round(20 + 30 * math.sin(i / 15.0), 2)
feed(1, 600, True, lambda i: 90 if i == 299 else base(i))

s = 'small'
check(call('has', 'small'), 'the current chart opens a larger view')
la, ra = js('ax', s, False), js('ax', s, True)
vis = [base(i) for i in range(300, 601)] + [round(base(i) * 0.5 - 5, 2) for i in range(300, 601)]
lo_d, hi_d = min(min(vis), 0), 90
check(la['hi'] >= hi_d + 0.1 * (hi_d - lo_d) - 1e-9 and la['lo'] <= lo_d - 0.1 * (hi_d - lo_d) + 1e-9,
      'left axis %s..%s holds the readings %s..%s (90 A at the left edge counted) with 10%% of their span above and below'
      % (la['lo'], la['hi'], lo_d, hi_d))
check(ra['lo'] == 0 and ra['hi'] >= 55, 'Field %% autoscaled from 0 with room above 50 %%: %s..%s' % (ra['lo'], ra['hi']))
h = call('plotH', s)
y_hi, y_lo = call('yAt', s, la['hi'], False), call('yAt', s, la['lo'], False)
check(abs(y_hi - 3) < 1e-6 and abs(y_lo - (h - 3)) < 1e-6, 'axis limits drawn 3 px inside the plot: y %s and %s of %s' % (y_hi, y_lo, h))
v = js('ax', 'volts', False)
vr = js('ax', 'volts', True)
# 13.5 and 13.6 V steady: the 1 V least span 13.05..14.05, padded 10 % to 12.95..14.15, rounded to 0.5 V
check(abs(v['lo'] - 12.5) < 1e-9 and abs(v['hi'] - 14.5) < 1e-9 and vr['lo'] == 0 and vr['hi'] == 60,
      'the voltage chart padded the same way: %s..%s, Field %% %s..%s' % (v['lo'], v['hi'], vr['lo'], vr['hi']))

drag(call('smallPoint'), -220)
check(not call('popShown'), 'a sideways drag on the current chart opens nothing (the page gets the drag)')
click(call('smallPoint'))
check(call('popShown'), 'a tap on the current chart opens its larger view')
b = 'big'
check(call('has', 'big') and call('bigTitle') == 'Current History' and js('ax', b, False)['lo'] == la['lo']
      and js('ax', b, False)['hi'] == la['hi'], 'the larger view shows the same chart and left axis: %s' % js('ax', b, False))
bx = js('box')
check(not bx['fits'] and bx['w'] == round(1024 * 0.8) and bx['h'] == round(600 * 0.84) and abs(bx['dim'] - 0.3) < 1e-9,
      'a chart panel is 80 %% by 84 %% of the page, over a lighter dim (0.3): %s' % bx)
check(js('tickState', 'Autoscale') == {'checked': True, 'enabled': True}, 'Autoscale ticked by default')

click(call('tickPoint', 'Lock axes'))
vw = json.loads(call('views') or '{}').get('0000FEEDC0DE1234/shortCurrent', {})
check(vw.get('lock') is True and vw.get('lockL') == [la['lo'], la['hi']], 'Lock axes keeps the ranges shown, per regulator: %s' % vw)
feed(601, 620, False, lambda i: 150)
l2, b2 = js('ax', 'small', False), js('ax', 'big', False)
check(l2['lo'] == la['lo'] and l2['hi'] == la['hi'] and b2['hi'] == la['hi'], 'locked: 150 A arrives and both charts stay at %s..%s' % (l2['lo'], l2['hi']))
click(call('tickPoint', 'Lock axes'))
l3 = js('ax', 'small', False)
check(l3['hi'] >= 165, 'unlocked: autoscale takes 150 A in at once, with room above: %s' % l3['hi'])
click(call('tickPoint', 'Lock axes'))
locked = js('ax', 'big', False)

click(QPoint(5, 5))
check(not call('popShown'), 'a tap on the page around the larger view closes it')
check(js('ax', 'small', False)['hi'] == locked['hi'], 'the small chart keeps the lock after the larger view closes')

# a restart: the same settings from localsettings
call('start', True)
QTest.qWait(200)
feed(1, 600, True, lambda i: base(i))
r = js('ax', 'small', False)
check(r['lo'] == locked['lo'] and r['hi'] == locked['hi'], 'after a restart the chart is still locked at %s..%s' % (r['lo'], r['hi']))

click(call('smallPoint'))
check(call('popShown'), 'opens again')
click(call('axisZone', False, True))
d = js('sheetInfo')
check(d and d['title'] == 'Current maximum' and d['value'] == round(locked['hi']) and d['suffix'] == ' A',
      'a tap at the top of the left axis opens the limit box at its maximum: %s' % d)
check(d and d['w'] == 312 and 150 <= d['h'] <= 184, 'the limit box is half gui-v2\'s 624 x 368 number dialog each way: %s x %s'
      % (d['w'], d['h']))
step = max(1, 10 ** math.floor(math.log10((locked['hi'] - locked['lo']) / 30) + 1e-9))
click(call('sheetPoint', '+'))
check(js('sheetInfo')['value'] == round(locked['hi']) + step, 'a press on + raises it one step (%s A): %s' % (step, js('sheetInfo')['value']))
click(call('sheetPoint', 'Cancel'))
check(js('sheetInfo') is None and call('popShown') and js('writes', '/Plot/YLim/Current') == [],
      'Cancel closes the box, the larger view stays, nothing written')
click(call('axisZone', False, True))
click(QPoint(5, 5))
check(js('sheetInfo') is None and call('popShown'), 'a tap around the limit box closes only the box')
# typed entry: a tap on the number types one (it comes up selected, so typing replaces it); Enter applies it as Set does;
# Cancel drops it; a maximum typed below the minimum is held one step inside it
click(call('axisZone', False, True))
click(call('sheetNumber'))
d = js('sheetInfo')
check(d['typing'] is True and d['focus'] is True and d['field'] == str(round(locked['hi'])) and d['selected'] == d['field'],
      'a tap on the number starts typing, the number selected: %s' % d)
type_text('99')
click(call('sheetPoint', 'Cancel'))
check(js('sheetInfo') is None and js('writes', '/Plot/YLim/Current') == [], 'typed, then Cancel: nothing written')
click(call('axisZone', False, True))
click(call('sheetNumber'))
type_text('65')
QTest.keyClick(win, Qt.Key_Return)
QTest.qWait(150)
w = js('writes', '/Plot/YLim/Current')
check(js('sheetInfo') is None and w[-1:] == ['%d,65' % locked['lo']], 'typed 65 and Enter: written %s' % w)
click(call('axisZone', False, True))
click(call('sheetNumber'))
type_text(str(locked['lo'] - 50))
click(call('sheetPoint', 'Set'))
QTest.qWait(150)
w = js('writes', '/Plot/YLim/Current')
check(w[-1:] == ['%d,%d' % (locked['lo'], locked['lo'] + 1)], 'a maximum typed below the minimum is held one step above it: %s' % w)
click(call('axisZone', False, True))
call('setSheet', 70)
QTest.qWait(150)
w = js('writes', '/Plot/YLim/Current')
l4, r4 = js('ax', 'small', False), js('ax', 'small', True)
check(w[-1:] == ['%d,70' % locked['lo']], 'the regulator gets Ymin1/Ymax1 %s (shared with the web app): %s' % ('%d,70' % locked['lo'], w))
check(js('tickState', 'Autoscale')['checked'] is False and js('tickState', 'Lock axes')['enabled'] is False,
      'a limit set by hand turns Autoscale off; Lock axes is for autoscale only')
check(l4['lo'] == locked['lo'] and l4['hi'] == 70 and l4['ticks'][0] == locked['lo'] and l4['ticks'][-1] == 70,
      'left axis %s..70, both limits labelled: %s' % (locked['lo'], l4['ticks']))
click(call('axisZone', True, True))
check(js('sheetInfo')['title'] == 'Field maximum', 'the right axis\'s box: %s' % js('sheetInfo')['title'])
call('setSheet', 120)
QTest.qWait(150)
r5, w5 = js('ax', 'small', True), js('writes', '/Plot/YLim/Current')
check(r5['lo'] == r4['lo'] and r5['hi'] == 120 and len(w5) == len(w), 'Field %% maximum 120, kept on the GX only: %s..%s, %d regulator writes'
      % (r5['lo'], r5['hi'], len(w5)))
click(call('tickPoint', 'Autoscale'))
l6 = js('ax', 'small', False)
check(l6['hi'] != 70 and js('tickState', 'Autoscale')['checked'] is True, 'Autoscale ticked again fits the data: %s..%s' % (l6['lo'], l6['hi']))

click(call('legendPoint', 'big', 'Battery Current'))
check(call('hidden', 'small', 'bcur') is True and call('hidden', 'big', 'bcur') is True,
      'a legend tap in the larger view hides the line in both charts')
vw = json.loads(call('views')).get('0000FEEDC0DE1234/shortCurrent', {})
check(vw.get('hidden', {}).get('bcur') is True, 'and is kept: %s' % vw.get('hidden'))
click(QPoint(5, 5))

# the other Short Term charts: their own larger views and the regulator's Ymin2-4/Ymax2-4
click(call('point', 'volts'))
check(call('popShown') and call('bigTitle') == 'Battery Voltage History', 'a tap on the voltage chart opens its own larger view')
click(call('axisZone', False, True))
d = js('sheetInfo')
check(d['title'] == 'Voltage maximum' and abs(d['value'] - 14.5) < 1e-9 and d['suffix'] == ' V', 'its limit box: %s' % d)
click(call('sheetPoint', '+'))
check(abs(js('sheetInfo')['value'] - 14.6) < 1e-9, 'volts step 0.1 V: %s' % js('sheetInfo')['value'])
click(call('sheetPoint', 'Set'))
QTest.qWait(150)
wv = js('writes', '/Plot/YLim/Volts')
check(wv == ['12.5,14.6'], 'the regulator gets Ymin2/Ymax2 in volts, one decimal: %s' % wv)
lv = js('ax', 'volts', False)
check(abs(lv['lo'] - 12.5) < 1e-9 and abs(lv['hi'] - 14.6) < 1e-9, 'the small voltage chart shows 12.5..14.6: %s..%s' % (lv['lo'], lv['hi']))
check(js('writes', '/Plot/YLim/Current') == w, 'the current chart\'s limits untouched')
click(QPoint(5, 5))

click(call('point', 'temp'))
check(call('bigTitle') == 'Alternator Temperature History', 'the temperature chart opens its own larger view')
tl = js('ax', 'big', False)
click(call('axisZone', False, True))
d = js('sheetInfo')
# the stand-in GX shows Fahrenheit: 60 C reads 140 F
check(d['title'] == 'Temperature maximum' and d['suffix'] == ' °F' and d['value'] == round(tl['hi']),
      'its limit box, in the GX\'s unit: %s' % d)
call('setSheet', 200)
QTest.qWait(150)


def wire_f(f):
    # a limit as the bridge publishes it: Celsius of the whole degree F the regulator keeps
    return ('%.2f' % ((math.floor(f + 0.5) - 32) * 5 / 9)).rstrip('0').rstrip('.')


wt = js('writes', '/Plot/YLim/Temp')
want_t = '%s,%s' % (wire_f(tl['lo']), wire_f(200))
check(wt == [want_t], 'the regulator gets Ymin4/Ymax4 as Celsius of whole degrees F (%s): %s' % (want_t, wt))
tt = js('ax', 'temp', False)
check(round(tt['lo']) == tl['lo'] and round(tt['hi']) == 200, 'the small chart shows %s..200 F: %s..%s' % (tl['lo'], tt['lo'], tt['hi']))
click(call('axisZone', False, False))
check(js('sheetInfo')['value'] == tl['lo'], 'the minimum comes back as set, not a degree out: %s' % js('sheetInfo')['value'])
call('setSheet', tl['lo'] - 10)
QTest.qWait(150)
wt = js('writes', '/Plot/YLim/Temp')
check(wt[-1] == '%s,%s' % (wire_f(tl['lo'] - 10), wire_f(200)), 'and the maximum stays at 200 F: %s' % wt[-1])
click(QPoint(5, 5))

# Long Term: limits kept on the GX
check(call('startLong'), 'Long Term page created')
QTest.qWait(500)
lc = js('ax', 'longCur', False)
check(lc['hi'] >= 56 + 0.1 * 56 - 1e-9 and lc['lo'] == 0, 'Long Term currents padded from 0: %s..%s' % (lc['lo'], lc['hi']))
click(call('point', 'longCur'))
check(call('popShown') and call('bigTitle') == 'Currents', 'a tap on a Long Term chart opens its larger view')
click(call('axisZone', False, True))
check(js('sheetInfo')['title'] == 'Current maximum', 'its limit box: %s' % js('sheetInfo'))
nw = len(json.loads(call('views') or '{}'))
call('setSheet', 80)
QTest.qWait(150)
vw = json.loads(call('views')).get('0000FEEDC0DE1234/longCur', {})
lc2 = js('ax', 'longCur', False)
check(vw.get('auto') is False and vw.get('left') == [lc['lo'], 80] and lc2['hi'] == 80,
      'kept on the GX (the web app keeps Long Term limits in the browser): %s, small chart %s..%s' % (vw, lc2['lo'], lc2['hi']))
check(all(js('writes', '/Plot/YLim/' + n)[-1:] != ['0,80'] for n in ('Current', 'Volts', 'Rpm', 'Temp')), 'nothing written to the regulator')
click(call('tickPoint', 'Autoscale'))
click(call('tickPoint', 'Autoscale'))
vw = json.loads(call('views')).get('0000FEEDC0DE1234/longCur', {})
check(vw.get('auto') is False and vw.get('left') == [lc['lo'], 80], 'Autoscale off again goes back to the limits set: %s' % vw)
click(call('tickPoint', 'Autoscale'))
click(QPoint(5, 5))

# Dashboard
check(call('startDash'), 'Dashboard created')
QTest.qWait(400)
chips = [
    ('Batt', 'SOC', 'Battery', ['Voltage', '13.62 V', '14.42 V', '12.81 V', 'State of charge', '85.0 %', '97.3 %', '64.0 %', 'High', 'Low']),
    ('Alt', 'Batt', 'Alternator and Battery Current', ['Alternator', 'Battery', '42.5 A', '−3.3 A', '88.3 A', '0.0 A', '61.3 A', '−40.5 A']),
    ('Alt', '', 'Temperatures', ['Alternator', '140.0 °F', '176.9 °F', '68.0 °F']),
    ('RPM', 'IGN', 'Engine', ['RPM', '1500', '2800', '0', 'Ignition', 'ON']),
    ('Health', '', 'Alternator Health', ['Health', '97 %', 'Mean', '96 %', '10th percentile', '93 %', '12 graded points this session']),
]
for l1, l2, title, want in chips:
    click(call('chipPoint', l1, l2))
    t = js('texts')
    ok = call('popShown') and title in t
    check(ok, 'a tap on the %s chip opens "%s"' % (l1 + ('/' + l2 if l2 else ''), title))
    miss = [x for x in want if x not in t]
    check(not miss, '%s: shows %s' % (title, 'every reading' if not miss else 'all but %s' % miss))
    bx = js('box')
    check(bx['fits'] and bx['w'] == math.ceil(bx['cw']) + 28 and bx['h'] == math.ceil(bx['ch']) + 28 and bx['w'] < 1024 * 0.8,
          '%s: the panel is as big as its content and no bigger: %s' % (title, bx))
    click(QPoint(5, 5))
    check(not call('popShown'), '%s: closed by a tap around it' % title)

# highs and lows: the panel's "since" note (a pair started at another time named) and its two-tap Reset
now = call('nowS')
for n in ('IBV', 'SOC', 'amps', 'Bcur', 'RPM'):
    call('put', 'Live/WmSince/' + n, now - 3600)
click(call('chipPoint', 'Batt', 'SOC'))
t = js('texts')
note = [x for x in t if x.startswith('Highs and lows')]
check('Reset' in t and len(note) == 1 and note[0].startswith('Highs and lows since ') and 'since' not in note[0][21:],
      'Battery panel: Reset and one shared since time: %s' % note)
call('put', 'Live/WmSince/SOC', now - 86400 * 2)
QTest.qWait(100)
note = [x for x in js('texts') if x.startswith('Highs and lows')]
check(len(note) == 1 and '. State of charge since ' in note[0], 'a pair reset at another time is named: %s' % note)
click(call('textPoint', 'Reset'))
check('Tap again to reset' in js('texts') and js('writes', '/Live/WmReset') == [], 'the first tap only asks again')
QTest.qWait(3300)
check('Reset' in js('texts') and 'Tap again to reset' not in js('texts'), 'after 3 s the first tap is forgotten')
click(call('textPoint', 'Reset'))
click(call('textPoint', 'Tap again to reset'))
check(js('writes', '/Live/WmReset') == ['IBV,SOC'], 'the second tap resets this panel\'s pairs: %s' % js('writes', '/Live/WmReset'))
call('put', 'Settings/Result', json.dumps({'seq': 41, 'key': 'wmReset', 'ok': False, 'message': 'x'}))
QTest.qWait(100)
check('Could not reset' in js('texts'), 'a refused reset says so')
click(QPoint(5, 5))
click(call('chipPoint', 'Alt', 'Batt'))
click(call('textPoint', 'Reset'))
click(call('textPoint', 'Tap again to reset'))
check(js('writes', '/Live/WmReset')[-1:] == ['amps,Bcur'], 'Current panel resets alternator and battery current')
click(QPoint(5, 5))
click(call('chipPoint', 'RPM', 'IGN'))
click(call('textPoint', 'Reset'))
click(call('textPoint', 'Tap again to reset'))
check(js('writes', '/Live/WmReset')[-1:] == ['RPM'], 'Engine panel resets RPM')
click(QPoint(5, 5))
click(call('chipPoint', 'Health', ''))
check('Reset' not in js('texts'), 'Health panel has no Reset')
click(QPoint(5, 5))

# the barometer tile
bi = js('baroInfo')
check(bi['hi'] - bi['lo'] >= 10 and min(bi['n']) > 0, 'barometer tile: three days drawn on at least 10 mbar: %s' % bi)
click(call('baroPoint'))
check(call('popShown') and call('bigTitle') == 'Barometer', 'a tap on the barometer tile opens its larger view')
bb = js('ax', 'big', False)
click(call('axisZone', False, True))
d = js('sheetInfo')
check(d['title'] == 'Pressure maximum' and d['suffix'] == ' mbar', 'its limit box: %s' % d)
call('setSheet', 1030)
QTest.qWait(150)
bi2 = js('baroInfo')
check(bi2['hi'] == 1030 and bi2['lo'] == bb['lo'], 'the tile follows the limit set: %s..%s' % (bi2['lo'], bi2['hi']))
click(call('legendPoint', 'big', 'Yesterday'))
check(js('baroInfo')['n'][1] == 0 and js('baroInfo')['n'][0] > 0, 'a day hidden in the larger view leaves the tile too: %s' % js('baroInfo'))
click(QPoint(5, 5))



def no_regulator_writes():
    return all(js('writes', '/Plot/YLim/' + n) == [] for n in ('Current', 'Volts', 'Rpm', 'Temp'))


def view_of(name):
    return json.loads(call('views') or '{}').get('0000FEEDC0DE1234/' + name, {})


def padded(ax, lo_d, hi_d, min_span, from_zero=False):
    # an autoscaled axis: the readings lo_d..hi_d, widened to min_span, with 10 % of that span above and below (not
    # below 0 on an axis from zero)
    span = max(hi_d - lo_d, min_span)
    lo_w = 0 if from_zero else (lo_d + hi_d) / 2 - span / 2 if hi_d - lo_d < min_span else lo_d
    hi_w = lo_w + span
    lo_need = 0 if from_zero and lo_w == 0 else lo_w - 0.1 * span
    return ax['lo'] <= lo_need + 1e-9 and ax['hi'] >= hi_w + 0.1 * span - 1e-9 and (not from_zero or ax['lo'] == 0)


def test_health_larger_views():
    check(call('startHealth', False), 'Health page created')
    QTest.qWait(800)
    t = js('ax', 'trend', False)
    # the trend's low 10 % line 88-93, average 94-98, and the 100 % reference line
    check(padded(t, 88, 100, 10), 'Health trend: padded autoscale holds 88..100 %% (the 100 %% line counted): %s..%s' % (t['lo'], t['hi']))
    h = call('plotH', 'trend')
    check(abs(call('yAt', 'trend', t['hi'], False) - 3) < 1e-6 and abs(call('yAt', 'trend', t['lo'], False) - (h - 3)) < 1e-6,
          'Health trend: axis limits 3 px inside the plot')
    se = js('ax', 'sess', False)
    check(padded(se, 87, 93, 10) and se['hi'] - se['lo'] >= 10, 'This Session: 87..93 %% widened to 10 points and padded: %s..%s'
          % (se['lo'], se['hi']))
    drag(call('point', 'sess'), -200)
    check(not call('popShown') and call('cursorOf', 'sess') != call('cursorOf', 'sess'),
          'a drag on the session chart opens nothing and moves no cursor')
    lt = call('latestLine')
    check(lt.startswith('92% · Estimated · full steady run'), 'the line under it reads the newest graded dot: "%s"' % lt)
    click(call('point', 'sess'))
    bx = js('box')
    check(call('popShown') and call('bigTitle') == 'This Session' and not bx['fits'] and bx['w'] == round(1024 * 0.8)
          and bx['h'] == round(600 * 0.84), 'a tap on the session chart opens its larger view, 80 %% by 84 %%: %s' % bx)
    check(lt in js('texts'), 'the larger view shows the newest graded dot too')
    click(call('tickPoint', 'Lock axes'))
    vw = view_of('healthSession')
    check(vw.get('lock') is True and vw.get('lockL') == [se['lo'], se['hi']], 'Lock axes keeps the session axis shown: %s' % vw)
    click(call('axisZone', False, True))
    d = js('sheetInfo')
    check(d and d['title'] == 'Health maximum' and d['suffix'] == '%' and d['value'] == se['hi'], 'its limit box: %s' % d)
    call('setSheet', 98)
    QTest.qWait(150)
    s2 = js('ax', 'sess', False)
    vw = view_of('healthSession')
    check(vw.get('auto') is False and vw.get('left') == [se['lo'], 98] and s2['hi'] == 98 and s2['lo'] == se['lo'],
          'the session maximum 98 %%, kept on the GX, and the small chart follows: %s, %s..%s' % (vw, s2['lo'], s2['hi']))
    check(no_regulator_writes(), 'nothing written to the regulator')
    click(call('legendPoint', 'big', 'Estimated'))
    check(call('hidden', 'sess', 'est') is True, 'a legend tap in the larger view hides the estimated dots in the small chart too')
    click(QPoint(5, 5))
    check(not call('popShown'), 'closed by a tap around it')
    click(call('point', 'trend'))
    check(call('popShown') and call('bigTitle') == 'Charging System Health vs Engine Hours', 'a tap on the trend chart opens its larger view')
    click(QPoint(5, 5))
    call('startHealth', True)
    QTest.qWait(800)
    s3 = js('ax', 'sess', False)
    check(s3['hi'] == 98 and s3['lo'] == se['lo'] and call('hidden', 'sess', 'est') is True,
          'after a restart the session chart keeps 98 %% and the hidden dots: %s..%s' % (s3['lo'], s3['hi']))


def test_wind_tab_larger_view():
    check(call('startWind', False, False), 'Wind tab created (apparent)')
    QTest.qWait(500)
    r = js('ax', 'wind', True)
    # the last 30 min of apparent wind speed: 17 + 3 sin(i / 20) over samples 180..359
    spd = [round(17 + 3 * math.sin(i / 20), 1) for i in range(180, 360)]
    check(padded(r, 0, max(spd), 10, True), 'Wind tab speed axis padded from 0: %s..%s (readings to %s)' % (r['lo'], r['hi'], max(spd)))
    st, c0 = js('windStore', False), js('streamState', 'wind')
    check(st['n'] == 360 and st['len'] == 360 and st['breaks'] == 0 and c0['has'] and not c0['building'] and c0['smooth'] is False,
          'the whole hour held, drawn as streamed lines that step (no scroll): %s, %s' % (st, c0))
    # each new reading is one step of the chart, no rebuild; an apparent angle crossing 0/360 breaks the line there
    for k, (aws, awa) in enumerate(((21.5, 355), (21.0, 5), (20.5, 12))):
        call('windStep', 'pw1', 360 + k, 13.0, 220, aws, awa)
        QTest.qWait(60)
    st2, c2 = js('windStore', False), js('streamState', 'wind')
    check(st2['n'] == 363 and st2['breaks'] == 2 and st2['last'] == [13.0, 220, 20.5, 12],
          'three readings held as they came, a break sample at each of the two wraps (40 to 355, 355 to 5): %s' % st2)
    check(c2['sVer'] - c0['sVer'] == 3 and not c2['building'], 'the chart stepped once per reading, nothing rebuilt: %s -> %s'
          % (c0['sVer'], c2['sVer']))
    r1 = js('ax', 'wind', True)
    check(r1['hi'] >= 21.5 * 1.1 - 1e-9, 'the speed axis takes the new 21.5 kn reading at once: %s..%s' % (r1['lo'], r1['hi']))
    drag(call('point', 'wind'), -200)
    check(not call('popShown'), 'a drag on the Wind chart opens nothing')
    click(call('point', 'wind'))
    bx = js('box')
    check(call('popShown') and call('bigTitle') == 'Apparent wind' and bx['w'] == round(1024 * 0.8) and bx['h'] == round(600 * 0.84),
          'a tap opens the apparent wind\'s larger view, 80 %% by 84 %%: %s' % bx)
    click(call('axisZone', False, True))
    d = js('sheetInfo')
    check(d and d['title'] == 'Angle maximum' and d['suffix'] == '°', 'the angle\'s limit box, the degree sign as the chart writes it: %s' % d)
    click(call('sheetPoint', 'Cancel'))
    click(call('axisZone', True, True))
    d = js('sheetInfo')
    check(d and d['title'] == 'Speed maximum' and d['suffix'] == ' kn', 'the speed\'s limit box: %s' % d)
    call('setSheet', 30)
    QTest.qWait(150)
    r2 = js('ax', 'wind', True)
    vw = view_of('windApp')
    check(vw.get('auto') is False and vw.get('right') == [0, 30] and r2['lo'] == 0 and r2['hi'] == 30,
          'speed maximum 30 kn, kept on the GX for apparent wind, and the small chart follows: %s, %s..%s' % (vw, r2['lo'], r2['hi']))
    check(no_regulator_writes(), 'nothing written to the regulator')
    click(QPoint(5, 5))
    call('setTrueWind', True)
    QTest.qWait(300)
    l3, r3 = js('ax', 'wind', False), js('ax', 'wind', True)
    check(l3['lo'] == 0 and l3['hi'] == 360 and r3['hi'] != 30 and padded(r3, 0, 15, 10, True),
          'true wind keeps its own settings: direction 0..360, speed autoscaled %s..%s' % (r3['lo'], r3['hi']))
    call('startWind', True, False)
    QTest.qWait(500)
    r4 = js('ax', 'wind', True)
    check(r4['lo'] == 0 and r4['hi'] == 30, 'after a restart apparent wind keeps 0..30 kn: %s..%s' % (r4['lo'], r4['hi']))


demo = json.load(open(os.path.join(os.path.dirname(here), 'fixtures', 'demo.json')))
perf_json = json.dumps({k: v for k, v in demo.items() if k.startswith('Perf/') or k == 'Live/Rpm'})


def test_dash_wind_tile_larger_view():
    check(call('startDash2', False, perf_json), 'Dashboard created with wind, power flow and speed data')
    QTest.qWait(600)
    w = js('windInfo')
    app = [round(18 + 3 * math.sin(i / 18), 1) for i in range(240)]
    check(padded(w, 0, max(app), 10, True) and w['app'] == 240 and w['tru'] == 0 and not w['trueOn'],
          'wind tile: apparent wind only, one point per 15 s column, padded from 0: %s..%s (readings to %s, %d points)'
          % (w['lo'], w['hi'], max(app), w['app']))
    c0 = js('dashWindCols')
    check(c0['app']['n'] == 240 and c0['app']['lastX'] == 0 and c0['app']['firstX'] == -3585,
          'the tile\'s columns span the hour to the newest reading: %s' % c0['app'])
    # the next reading lands in the newest column: the line's end moves at once, the rest stays
    call('windStep', 'pd1', 240, 25.0, 200, 30.0, 40)
    QTest.qWait(100)
    c1 = js('dashWindCols')
    last = round(18 + 3 * math.sin(239 / 18), 1)
    check(c1['app']['n'] == 240 and abs(c1['app']['last'] - (last + 30.0) / 2) < 0.051 and c1['app']['lastX'] == -0.5
          and c1['app']['firstX'] == -3586, 'a new reading averages into the newest column, the hour slides 1 s: %s' % c1['app'])
    w = js('windInfo')
    check(min(w['ys']) >= 3 - 1e-6 and max(w['ys']) <= w['h'] - 3 + 1e-6, 'its line 3 px inside the plot: y %.1f..%.1f of %s'
          % (min(w['ys']), max(w['ys']), w['h']))
    drag(call('windPoint'), -200)
    check(not call('popShown'), 'a drag on the wind tile opens nothing')
    click(call('windPoint'))
    bx = js('box')
    check(call('popShown') and call('bigTitle') == 'Wind' and bx['w'] == round(1024 * 0.8) and bx['h'] == round(600 * 0.84),
          'a tap on the wind tile opens its larger view, 80 %% by 84 %%: %s' % bx)
    b = js('ax', 'big', False)
    shown = [k for k in ('true', 'app') if not call('hidden', 'big', k)]
    tru = [round(25 + 3 * math.sin(i / 18), 1) for i in range(240)]
    check(shown == ['true', 'app'] and padded(b, 0, max(tru), 10, True),
          'the larger view: true and apparent wind together, padded from 0: %s, %s..%s' % (shown, b['lo'], b['hi']))
    click(call('axisZone', False, True))
    d = js('sheetInfo')
    check(d and d['title'] == 'Speed maximum' and d['suffix'] == ' kn', 'its limit box: %s' % d)
    call('setSheet', 35)
    QTest.qWait(150)
    w2 = js('windInfo')
    check(w2['lo'] == 0 and w2['hi'] == 35 and view_of('dashWind').get('left') == [0, 35],
          'the tile follows the 35 kn maximum, kept on the GX: %s..%s' % (w2['lo'], w2['hi']))
    click(call('legendPoint', 'big', 'True'))
    check(not js('windInfo')['trueOn'], 'true wind tapped off in the larger view: still off the tile')
    click(call('legendPoint', 'big', 'True'))
    w3 = js('windInfo')
    check(w3['trueOn'] and w3['tru'] == 240, 'tapped on again: the tile draws true wind too (%d points)' % w3['tru'])
    click(call('legendPoint', 'big', 'Apparent'))
    w4 = js('windInfo')
    check(not w4['appOn'] and w4['app'] == 0 and w4['tru'] == 240, 'apparent wind tapped off: it leaves the tile too')
    click(QPoint(5, 5))
    check(not call('popShown'), 'closed by a tap around it')
    call('startDash2', True, perf_json)
    QTest.qWait(600)
    w5 = js('windInfo')
    check(w5['hi'] == 35 and w5['trueOn'] and not w5['appOn'], 'after a restart the tile keeps 35 kn and the lines set: %s' % {
        k: w5[k] for k in ('lo', 'hi', 'appOn', 'trueOn')})


def test_dash_flow_larger_view():
    check(call('startDash2', False, perf_json), 'Dashboard created again')
    QTest.qWait(600)
    drag(call('flowPoint'), -200)
    check(not call('popShown'), 'a drag on the power flow opens nothing')
    click(call('flowPoint'))
    f = js('flowInfo')
    bx = js('box')
    check(call('popShown') and 'Power flow' in js('texts'), 'a tap on the power flow opens its larger view')
    check(bx['fits'] and bx['w'] == math.ceil(bx['cw']) + 28 and bx['h'] == math.ceil(bx['ch']) + 28 and bx['h'] <= 600 - 16,
          'the panel is as big as its content: %s' % bx)
    check(f['big'] == f['small'] and f['mode'] == 'normal' and f['grow'],
          'the same rows and numbers in the full layout (%s; the tile: %s): %s' % (f['mode'], f['smallMode'], f['big']))
    check(1 < f['zoom'] <= 1.5 and abs(bx['w'] - 28 - math.ceil(f['bigW'] * f['zoom'])) <= 1,
          'scaled %.2f times on a 1024 x 600 page: %.0f px wide' % (f['zoom'], f['bigW'] * f['zoom']))
    check('Shore charger' in f['names'] and 'Inverter' in f['names'], 'full row names: %s' % f['names'])
    click(QPoint(5, 5))
    check(not call('popShown'), 'closed by a tap around it')


def test_dash_perf_larger_view():
    QTest.qWait(800)
    p = js('perfInfo', 'small')
    want_s = 4 * math.ceil(max(10, 1.1 * max(p['curveS'], p['spd'])) / 4)
    check(p['view'] == 1 and p['maxS'] == want_s and p['maxR'] % 500 == 0 and p['maxR'] >= max(3000, 1.1 * p['curveR']) - 1e-9
          and p['maxR'] - 500 < max(3000, 1.1 * p['curveR']), 'speed tile (motoring): padded autoscale, %s kn (curve to %.2f, live %.2f) by %s RPM '
          '(curve to %s)' % (p['maxS'], p['curveS'], p['spd'], p['maxR'], p['curveR']))
    drag(call('perfPoint'), -200)
    check(not call('popShown'), 'a drag on the speed tile opens nothing')
    click(call('perfModePoint', 0, 'small'))
    QTest.qWait(300)
    check(not call('popShown') and js('perfInfo', 'small')['pinned'] == 0, 'a tap on the tile\'s own Sailing button picks sailing and opens nothing')
    click(call('perfModePoint', 1, 'small'))
    QTest.qWait(600)
    check(not call('popShown') and js('perfInfo', 'small')['view'] == 1, 'Motoring again, still no larger view')
    click(call('perfPoint'))
    bx = js('box')
    b = js('perfInfo', 'big')
    check(call('popShown') and b and bx['w'] == round(1024 * 0.8) and bx['h'] == round(600 * 0.84),
          'a tap on the speed tile opens its larger view, 80 %% by 84 %%: %s' % bx)
    check(b['view'] == 1 and b['sameShown'] and not b['job'] and b['maxS'] == p['maxS'] and b['maxR'] == p['maxR'],
          'it draws the small tile\'s curve and axes, with no sweep of its own')
    check(b['k'] > 1 and b['plotS'] > p['plotS'], 'bigger: type %.2f times, plot %.0f px against %.0f' % (b['k'], b['plotS'], p['plotS']))
    click(call('perfModePoint', 0, 'big'))
    QTest.qWait(600)
    p, b = js('perfInfo', 'small'), js('perfInfo', 'big')
    check(p['pinned'] == 0 and p['view'] == 0 and b['view'] == 0, 'Sailing tapped in the larger view: the small tile shows sailing too')
    click(call('perfFramePoint', 'True', 'big'))
    QTest.qWait(300)
    check(js('perfInfo', 'small')['trueFrame'] and view_of('dashPerf').get('trueFrame') is True,
          'True tapped in the larger view: the small tile\'s polar turns to true wind, kept on the GX')
    click(call('perfZone', False))
    d = js('sheetInfo')
    s_now = js('perfInfo', 'small')['maxS']
    check(d and d['title'] == 'Speed maximum' and d['suffix'] == ' kn' and d['value'] == s_now, 'a tap on the outer ring\'s speed: %s' % d)
    call('setSheet', 20)
    QTest.qWait(150)
    vw = view_of('dashPerf')
    check(js('perfInfo', 'small')['maxS'] == 20 and vw.get('auto') is False and vw.get('left') == [0, 20],
          'speed maximum 20 kn: the small tile follows, kept on the GX: %s' % vw)
    click(call('perfModePoint', 1, 'big'))
    QTest.qWait(600)
    click(call('perfZone', True))
    d = js('sheetInfo')
    check(d and d['title'] == 'RPM maximum' and d['suffix'] == '', 'motoring, a tap on the last RPM: %s' % d)
    call('setSheet', 4000)
    QTest.qWait(150)
    p = js('perfInfo', 'small')
    check(p['view'] == 1 and p['maxR'] == 4000 and p['maxS'] == 20, 'RPM maximum 4000: the small tile follows: %s RPM, %s kn' % (p['maxR'], p['maxS']))
    click(call('perfModePoint', 0, 'big'))
    QTest.qWait(600)
    for kn in (21, 20):   # Sailing shows no RPM axis
        click(call('perfZone', False))
        call('setSheet', kn)
        QTest.qWait(150)
    vw = view_of('dashPerf')
    check(vw.get('left') == [0, 20] and (vw.get('right') or [0, 0])[1] == 4000, 'a speed limit set on Sailing keeps the RPM limit: %s' % vw)
    check(no_regulator_writes(), 'nothing written to the regulator')
    click(QPoint(5, 5))
    check(not call('popShown'), 'closed by a tap around it')
    call('startDash2', True, perf_json)
    QTest.qWait(900)
    p = js('perfInfo', 'small')
    check(p['maxS'] == 20 and p['maxR'] == 4000 and p['trueFrame'] and p['pinned'] == -1,
          'after a restart: 20 kn, 4000 RPM and true wind kept; Sailing/Motoring back to following the boat: %s' % p)


# Touch input, as the GX's touchscreen sends it (QTest's touch device), beside the mouse clicks above
touch = QTest.createTouchDevice()


def ttap(pt, dx=0, dy=0):
    p = QPoint(int(round(pt.x() + dx)), int(round(pt.y() + dy)))
    QTest.touchEvent(win, touch, False).press(0, p, win).commit()
    QTest.qWait(40)
    QTest.touchEvent(win, touch, False).release(0, p, win).commit()
    QTest.qWait(200)


def tdrag(pt, dx):
    x0, y0 = int(pt.x()), int(pt.y())
    QTest.touchEvent(win, touch, False).press(0, QPoint(x0, y0), win).commit()
    for i in range(1, 13):
        QTest.qWait(16)
        QTest.touchEvent(win, touch, False).move(0, QPoint(int(x0 + dx * i / 12), y0), win).commit()
    QTest.qWait(16)
    QTest.touchEvent(win, touch, False).release(0, QPoint(x0 + dx, y0), win).commit()
    QTest.qWait(300)


# The speed tile at the Dashboard's sizes and its larger view's (80 % x 84 % of the page, less the panel's head), in both
# views: the polar's circle and the motoring plot at least as big as the old layout made them, and no text or control
# outside the tile, over another, or crossed by the circle
def test_perf_layout():
    check(call('startDash2', False, perf_json), 'Dashboard created for the speed tile\'s layout')
    QTest.qWait(800)
    sizes = [('Dashboard tile, 7-inch GX', 430, 298, 1), ('Dashboard tile, 5-inch GX', 336, 224, 1),
             ('Dashboard tile, phone upright', 355, 340, 1), ('larger view, 7-inch GX', 791, 362, 1.6),
             ('larger view, 5-inch GX', 612, 261, 1.6), ('larger view, phone upright', 332, 527, 1.6)]
    for name, w, h, k in sizes:
        call('perfLayoutStart', w, h, k)
        for v in (0, 1):
            call('perfPin', v)
            QTest.qWait(400)
            L = js('perfLayout')
            tag = '%s (%d x %d)' % (name, w, h)
            if v == 0:
                check(L['view'] == 0 and L['r'] >= L['oldR'] and not L['problems'],
                      '%s, sailing polar: radius %.0f px (the old layout: %.0f); %d texts and controls clear of each other and '
                      'of the circle, inside the tile%s' % (tag, L['r'], L['oldR'], L['texts'],
                                                            '' if not L['problems'] else ': ' + '; '.join(L['problems'])))
            else:
                check(L['view'] == 1 and L['cw'] >= L['oldCw'] and L['ch'] >= L['oldCh'] and not L['problems'],
                      '%s, motoring map: plot %.0f x %.0f px (the old layout: %.0f x %.0f); %d texts and controls clear, inside '
                      'the tile%s' % (tag, L['cw'], L['ch'], L['oldCw'], L['oldCh'], L['texts'],
                                      '' if not L['problems'] else ': ' + '; '.join(L['problems'])))
    # the plot holds its size as the readings change: grading words that wrap to 1 or 3 lines, sea state "calm" or
    # "very rough"
    call('perfLayoutStart', 430, 298, 1)
    call('perfPin', 0)
    QTest.qWait(400)
    sizes0 = []
    for state, pitch in ((0, 0.2), (2, 6.0), (3, 2.0), (1, 0.2)):
        call('put', 'Perf/Live/state', state)
        call('put', 'Perf/Live/pitchStd', pitch)
        QTest.qWait(250)
        L = js('perfLayout')
        sizes0.append((L['r'], L['cy'], len(L['problems'])))
    check(len(set(sizes0)) == 1 and sizes0[0][2] == 0, 'Dashboard tile, 7-inch GX: the polar keeps its radius and place through four grading '
          'states and sea states from calm to very rough, nothing overlapping: %s' % sizes0)
    call('perfLayoutEnd')
    call('perfPin', -1)


# Touch on the speed tile: its buttons do only what they say, a finger landing just off one (within the touch margin)
# counts as on it, the rest of the tile opens the larger view, a drag opens nothing; the larger view's controls too
def test_perf_touch():
    check(call('startDash2', False, perf_json), 'Dashboard created for touch input')
    QTest.qWait(800)
    for i in (0, 1, 0):
        ttap(call('perfModePoint', i, 'small'))
        p = js('perfInfo', 'small')
        check(not call('popShown') and p['pinned'] == i and p['view'] == i,
              'touch: a tap on the tile\'s %s button picks it and opens nothing' % ['Sailing', 'Motoring'][i])
    for i, edge, dy, where in ((1, True, 10, '10 px under'), (0, True, 10, '10 px under'), (1, False, -4, 'above (by the tile\'s top)'),
                               (0, False, -4, 'above (by the tile\'s top)')):
        ttap(call('perfEdge', 'small', i, '', edge), 0, dy)
        p = js('perfInfo', 'small')
        check(not call('popShown') and p['pinned'] == i, 'touch: %s the %s button picks it and opens nothing' % (where, ['Sailing', 'Motoring'][i]))
    ttap(call('perfFramePoint', 'True', 'small'))
    check(not call('popShown') and js('perfInfo', 'small')['trueFrame'], 'touch: a tap on the tile\'s True picks true wind and opens nothing')
    ttap(call('perfEdge', 'small', 0, 'Apparent', True), 0, 8)
    check(not call('popShown') and not js('perfInfo', 'small')['trueFrame'], 'touch: 8 px under Apparent picks it and opens nothing')
    ttap(call('perfEdge', 'small', 0, 'True', False), 0, -8)
    check(not call('popShown') and js('perfInfo', 'small')['trueFrame'], 'touch: 8 px over True picks it and opens nothing')
    ttap(call('perfFramePoint', 'Apparent', 'small'))
    tdrag(call('perfPoint'), -200)
    check(not call('popShown'), 'touch: a drag on the speed tile opens nothing')
    ttap(call('perfPoint'))
    check(call('popShown') and js('perfInfo', 'big'), 'touch: a tap on the polar opens the larger view')
    ttap(call('perfModePoint', 1, 'big'))
    check(call('popShown') and js('perfInfo', 'small')['view'] == 1, 'touch: Motoring in the larger view, which stays open')
    ttap(call('perfEdge', 'big', 0, '', True), 0, 10)
    check(call('popShown') and js('perfInfo', 'small')['view'] == 0, 'touch: 10 px under the larger view\'s Sailing picks it')
    ttap(call('perfFramePoint', 'True', 'big'))
    check(js('perfInfo', 'small')['trueFrame'], 'touch: True in the larger view')
    ttap(call('perfZone', False))
    d = js('sheetInfo')
    check(d and d['title'] == 'Speed maximum', 'touch: the outer ring\'s speed opens its limit box: %s' % d)
    ttap(call('sheetPoint', 'Cancel'))
    check(js('sheetInfo') is None and call('popShown'), 'touch: Cancel closes the box, the larger view stays')
    ttap(QPoint(5, 5))
    check(not call('popShown'), 'touch: a tap around the larger view closes it')



# Demo mode (/Perf/Live/sim): the "now" dot never outside the curve the tile draws, measured on the drawn items; a curve
# swept at another wind (the 15 s hold) gives way to a new sweep at once; the speed readout keeps the speed sent. Off
# demo mode (a real boat can beat its record) the dot sits at its speed and the hold stands.
def test_perf_demo_dot():
    check(call('startDash2', False, perf_json), 'Dashboard created for the demo-mode dot')
    QTest.qWait(900)

    def until_swept(ms=4000):
        t = 0
        while t < ms and js('perfDot')['sweeping']:
            QTest.qWait(50)
            t += 50

    until_swept()
    d = js('perfDot')
    check(d['demo'] and d['view'] == 1 and d['dotR'] >= d['curveR'] - 0.5, 'motoring, demo mode, at its speed: the dot on or under the '
          'curve (y %.1f, curve %.1f px)' % (d['dotR'], d['curveR']))
    call('put', 'Perf/Motor/spd', round(1.5 * d['curveAtDot'], 3))
    QTest.qWait(200)
    d = js('perfDot')
    check(d['dotR'] >= d['curveR'] - 0.5 and d['readout'] == '%.1f' % d['lSpd'], 'a speed 1.5 times the curve: the dot still on or under '
          'it (y %.1f, curve %.1f px), the readout at the speed sent (%s)' % (d['dotR'], d['curveR'], d['readout']))
    k0 = d['key']
    call('put', 'Perf/Motor/headwind', 4.0)          # a new headwind bin, within the 15 s hold
    QTest.qWait(2300)
    d = js('perfDot')
    check(d['key'] != k0 and d['key'].endswith('|8'), 'a headwind change with the dot outside the curve shown: a new sweep at the '
          '2 s check, not after the 15 s hold (%s to %s)' % (k0.split('|')[-1], d['key'].split('|')[-1]))
    until_swept()
    d = js('perfDot')
    check(d['dotR'] >= d['curveR'] - 0.5, 'and on the new curve the dot is on or under it (y %.1f, curve %.1f px)' % (d['dotR'], d['curveR']))
    # sailing: a point to windward and one off the wind, apparent and true wind
    for k, v in (('Perf/Live/valid', 1), ('Perf/Live/ws', 15.0), ('Perf/Live/pitchStd', 1.0), ('Perf/Live/state', 0)):
        call('put', k, v)
    call('perfPin', 0)
    QTest.qWait(300)
    until_swept()
    for wa, tf in ((40.0, False), (120.0, False), (320.0, True), (120.0, True)):
        call('perfFrame', tf)
        call('put', 'Perf/Live/wa', wa)
        call('put', 'Perf/Live/spd', 20.0)
        QTest.qWait(300)
        until_swept()
        d = js('perfDot')
        check(d['view'] == 0 and 0 < d['dotR'] <= d['curveR'] + 0.5, 'sailing at %g° apparent, %s frame, 20 kn sent: the dot on or inside '
              'the curve (%.1f px from the centre, the curve %.1f px)' % (wa, 'true' if tf else 'apparent', d['dotR'], d['curveR']))
    call('perfFrame', False)
    # off demo mode: the dot where its speed is, and the hold stands
    call('put', 'Perf/Live/sim', 0)
    call('put', 'Perf/Live/wa', 120.0)
    QTest.qWait(300)
    d = js('perfDot')
    check(not d['demo'] and abs(d['dotSpd'] - d['lSpd']) < 1e-6 and d['dotR'] > d['curveR'] + 5, 'off demo mode the dot sits at its speed, '
          'outside the curve (%.1f px from the centre, the curve %.1f px)' % (d['dotR'], d['curveR']))
    k0 = d['key']
    call('put', 'Perf/Live/ws', 22.0)
    QTest.qWait(2500)
    d = js('perfDot')
    check(d['key'] == k0 and not d['sweeping'], 'and a wind change within the 15 s hold waits for it (%s)' % d['key'].split('|')[-1])
    call('perfPin', -1)

# Touch on everything else that opens a larger view: none has a control of its own on the small tile or chart (a small
# chart's legend opens too), so a tap opens and a drag does not; in the larger views the legend, Lock axes and the axis
# zones take touch
def test_touch_elsewhere():
    call('fresh', False)
    check(call('start', False), 'Short Term page created for touch input')
    QTest.qWait(200)
    feed(1, 600, True, lambda i: base(i))
    tdrag(call('smallPoint'), -220)
    check(not call('popShown'), 'touch: a drag on the current chart opens nothing')
    ttap(call('legendPoint', 'small', 'Battery Current'))
    check(call('popShown') and call('hidden', 'small', 'bcur') is not True, 'touch: a tap on the small chart\'s legend opens the larger view, '
          'no line hidden')
    ttap(call('legendPoint', 'big', 'Battery Current'))
    check(call('hidden', 'big', 'bcur') is True and call('hidden', 'small', 'bcur') is True, 'touch: a legend tap in the larger view hides the line')
    ttap(call('legendPoint', 'big', 'Battery Current'))
    check(call('hidden', 'big', 'bcur') is not True, 'touch: and shows it again')
    ttap(call('tickPoint', 'Lock axes'))
    check(js('tickState', 'Lock axes')['checked'] is True, 'touch: Lock axes ticks')
    ttap(call('tickPoint', 'Lock axes'))
    ttap(call('axisZone', False, True))
    d = js('sheetInfo')
    check(d and d['title'] == 'Current maximum', 'touch: the left axis\'s top opens its limit box')
    ttap(call('sheetPoint', '+'))
    check(js('sheetInfo')['value'] > d['value'], 'touch: + raises it')
    ttap(call('sheetPoint', 'Cancel'))
    ttap(QPoint(5, 5))
    check(not call('popShown'), 'touch: closed by a tap around it')

    check(call('startHealth', False), 'Health page created for touch input')
    QTest.qWait(800)
    ttap(call('point', 'sess'))
    check(call('popShown') and call('bigTitle') == 'This Session', 'touch: a tap on the session chart opens its larger view')
    ttap(call('legendPoint', 'big', 'Estimated'))
    check(call('hidden', 'sess', 'est') is True, 'touch: its legend hides the estimated dots')
    ttap(QPoint(5, 5))

    call('fresh', False)
    check(call('startLong'), 'Long Term page created for touch input')
    QTest.qWait(500)
    ttap(call('point', 'longCur'))
    check(call('popShown') and call('bigTitle') == 'Currents', 'touch: a tap on a Long Term chart opens its larger view')
    ttap(QPoint(5, 5))

    check(call('startWind', False, False), 'Wind tab created for touch input')
    QTest.qWait(500)
    ttap(call('point', 'wind'))
    check(call('popShown'), 'touch: a tap on the Wind tab\'s chart opens its larger view')
    ttap(QPoint(5, 5))

    call('fresh', False)
    check(call('startDash'), 'Dashboard created for touch input on its chips and barometer')
    QTest.qWait(400)
    for l1, l2, title in (('Batt', 'SOC', 'Battery'), ('Alt', '', 'Temperatures'), ('Health', '', 'Alternator Health')):
        ttap(call('chipPoint', l1, l2))
        check(call('popShown') and title in js('texts'), 'touch: a tap on the %s chip opens "%s"' % (l1 + ('/' + l2 if l2 else ''), title))
        if title == 'Battery':
            check('Reset' not in js('texts'), 'touch: no Reset from a regulator that sends no start times')
            call('put', 'Live/WmSince/IBV', call('nowS'))
            QTest.qWait(100)
            nw = len(js('writes', '/Live/WmReset'))
            ttap(call('textPoint', 'Reset'))
            ttap(call('textPoint', 'Tap again to reset'))
            check(js('writes', '/Live/WmReset')[nw:] == ['IBV,SOC'] and call('popShown'),
                  'touch: two taps on Reset reset the Battery panel and leave it open')
        ttap(QPoint(5, 5))
    tdrag(call('baroPoint'), -200)
    check(not call('popShown'), 'touch: a drag on the barometer tile opens nothing')
    ttap(call('baroPoint'))
    check(call('popShown') and call('bigTitle') == 'Barometer', 'touch: a tap on the barometer tile opens its larger view')
    ttap(call('legendPoint', 'big', 'Yesterday'))
    check(js('baroInfo')['n'][1] == 0, 'touch: a day tapped off in its legend leaves the tile too')
    ttap(QPoint(5, 5))

    check(call('startDash2', False, perf_json), 'Dashboard created for touch input on its wind and power-flow tiles')
    QTest.qWait(600)
    ttap(call('windPoint'))
    check(call('popShown') and call('bigTitle') == 'Wind', 'touch: a tap on the wind tile opens its larger view')
    ttap(QPoint(5, 5))
    ttap(call('flowPoint'))
    check(call('popShown') and 'Power flow' in js('texts'), 'touch: a tap on the power flow opens its larger view')
    ttap(QPoint(5, 5))
    check(not call('popShown'), 'touch: closed by a tap around it')


test_health_larger_views()
test_wind_tab_larger_view()
test_dash_wind_tile_larger_view()
test_dash_flow_larger_view()
test_dash_perf_larger_view()
test_perf_layout()
test_perf_touch()
test_perf_demo_dot()
test_touch_elsewhere()

check(not warnings, 'no Qt warnings (%d)' % len(warnings))
print('SUMMARY: %d PASS, %d FAIL, %d Qt warnings' % (passes, fails, len(warnings)), flush=True)
sys.exit(1 if fails else 0)
