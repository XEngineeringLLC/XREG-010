#!/usr/bin/env python3
# Offline tests for the GX bridge: parsers and models directly, then a Core against fake_regulator.py over real HTTP,
# then the D-Bus glue with stub dbus/gi modules. Run: python3.12 -B test_bridge.py
import atexit, collections, json, math, os, re, shutil, subprocess, sys, tempfile, threading, time, types, unittest, urllib.parse

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
BRIDGE = os.environ.get('XREG_BRIDGE', REPO + '/gx_app/xregulator/bridge')
# Caches and node files the tests write, kept out of the repo
WORK = tempfile.mkdtemp(prefix='xreg_bt_')
atexit.register(shutil.rmtree, WORK, True)
sys.path.insert(0, BRIDGE)
sys.path.insert(0, HERE)
import xreg_core as xc
import fake_regulator as fr
from short_client import ShortClient
SHORT_NAMES = [n for n, *_ in xc.SHORT] + ['chargeStage']

SCHEMA_TEXT = open(BRIDGE + '/settings_schema.json').read()
xc.WIND_PLOT_S = 1
xc.LOCATE_RETRY_S = 1
xc.GXLIMITS_EVERY_S = 0.3
xc.DVCC_FRESH_S = 1.0
xc.DEMO_RETRY_S = 1.0
UID_B = '00005EEDC0DE5678'   # a second unit: ID 5EEDC0


class Store(dict):
    def get(self, k):
        return dict.get(self, k, '')

    def set(self, k, v):
        self[k] = v


class Pub(dict):
    # Strict stand-in for the VeDbusService: a path must be declared before it is set
    def __init__(self, core, paths=None):
        super().__init__()
        self.modes = {}
        for path, value, mode in core.static_paths() if paths is None else paths:
            assert path not in self, 'duplicate static path ' + path
            self.add(path, value, mode)

    def add(self, path, value, mode):
        assert re.fullmatch(r'(/[A-Za-z0-9_]+)+', path), 'not a valid D-Bus object path: ' + path
        if path not in self.modes:
            dict.__setitem__(self, path, value)
            self.modes[path] = mode

    def __setitem__(self, path, value):
        if path not in self.modes:
            raise KeyError('undeclared path ' + path)
        dict.__setitem__(self, path, value)


class Harness:
    # A Manager with fake regulators on the network; its /identify sweep reaches exactly these. `pin` maps uid to the
    # cached address (default: the first fake, where it is). A store that already lists units (a restart) is used as is.
    def __init__(self, regs, pin=None, schema=SCHEMA_TEXT, store=None, unit_stores=None):
        self.regs = list(regs) if isinstance(regs, (list, tuple)) else [regs]
        self.cache = tempfile.mkdtemp(dir=WORK)
        self.unit_stores = {} if unit_stores is None else unit_stores
        self.forgotten = []
        if store is None:
            pin = {self.regs[0].uid: self.regs[0].host} if pin is None else pin
            store = Store(units=','.join(pin))
            for uid, host in pin.items():
                self.unit_store(uid, xc.INSTANCE).set('host', host)
        self.store = store
        self.main = (store.get('units') or '').split(',')[0]
        self.pubs = {}
        self.alts = {}   # uid -> (mode, Pub): the alternator service xregulator_bridge.py keeps in step with alt_mode()
        self.discovery = xc.Discovery(browse=False, sweep_hosts=lambda: [r.host for r in self.regs])
        self.mgr = xc.Manager(store, schema, self.discovery, self.unit_store, self.forget, self, cache_dir=self.cache)
        self.mpub = Pub(self.mgr)
        self.page = ShortClient()   # a Short Term page following /Fast/Short of the main regulator
        self.follow_short = True    # off where a test is the page itself (test_fastlane.py)
        self._short_msg = None
        self.mgr.start()

    # -- what xregulator_bridge.py does on the GX --

    def unit_store(self, uid, instance):
        # localsettings' ClassAndVrmInstance rule: a taken instance moves to the next free one
        if uid not in self.unit_stores:
            taken = {st.get('instance') for st in self.unit_stores.values()}
            while 'switch:%d' % instance in taken:
                instance += 1
            self.unit_stores[uid] = Store(instance='switch:%d' % instance)
        return self.unit_stores[uid]

    def forget(self, uid):
        self.forgotten.append(uid)
        self.unit_stores.pop(uid, None)

    def unit_added(self, core):
        self.pubs[core.uid] = Pub(core)

    def unit_removed(self, core):
        del self.pubs[core.uid]
        self.alts.pop(core.uid, None)

    # --

    @property
    def core(self):
        return self.mgr.units[self.main]

    @property
    def pub(self):
        return self.pubs[self.main]

    def tick(self):
        self.mgr.tick(self.mpub)
        for uid, core in list(self.mgr.units.items()):
            core.tick(self.pubs[uid])
            want = core.alt_mode()
            if self.alts.get(uid, (None,))[0] != want:
                self.alts.pop(uid, None)
                if want:
                    self.alts[uid] = (want, Pub(core, core.alt_static_paths(want)))
            if want:
                core.alt_tick(self.alts[uid][1], want)
        pub = self.pubs.get(self.main) if self.follow_short else None
        m = pub.get('/Fast/Short') if pub is not None else None
        if m != self._short_msg:
            self._short_msg = m
            if m and self.page.take(m) == 'gap':
                self.write('/Fast/ShortFull', 1)

    def short(self, name):
        # the page's samples of one Short Term series
        return [None if x == '' else float(x) for x in self.page.cols[SHORT_NAMES.index(name)]]

    def alt(self, uid=None):
        return self.alts.get(uid or self.main, (None, None))[1]

    def systemcalc(self, **paths):
        # systemcalc's SetValue on the alternator service: every call stamped, an unchanged value included
        pub = self.alt()
        for name, value in paths.items():
            path = '/Link/' + name
            assert pub.modes[path] == 'stamp', path
            self.core.alt_write(path, value)
            dict.__setitem__(pub, path, value)

    def tick_for(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            self.tick()
            time.sleep(0.1)

    def until(self, pred, timeout=10.0, what=''):
        end = time.time() + timeout
        while time.time() < end:
            self.tick()
            try:
                if pred():
                    return True
            except (KeyError, TypeError, ValueError, IndexError):
                pass
            time.sleep(0.1)
        raise AssertionError('timed out waiting for ' + what)

    @staticmethod
    def _write(pub, target, path, value):
        # velib's SetValue: an unchanged value is accepted without calling back; otherwise the callback decides
        if pub.get(path) == value:
            return True
        ok = target.on_write(path, value)
        if ok:
            dict.__setitem__(pub, path, value)
        return ok

    def write(self, path, value, uid=None):
        return self._write(self.pubs[uid or self.main], self.mgr.units[uid or self.main], path, value)

    def mwrite(self, path, value):
        return self._write(self.mpub, self.mgr, path, value)

    def want(self, topic):
        return self.write('/Ui/Want/' + topic, int(time.time()) + len(self.core.want_at))

    def units(self):
        return {u['uid']: u for u in json.loads(self.mpub['/Units'])}

    def found(self):
        return {f['uid']: f for f in json.loads(self.mpub['/Found'])}

    def connected(self, uid):
        return uid in self.pubs and self.pubs[uid]['/Connected'] == 1

    def result(self):
        r = self.pub['/Settings/Result']
        return json.loads(r) if r else None

    def wait_result(self, key, timeout=10):
        seq0 = (self.result() or {}).get('seq', 0)
        self.until(lambda: self.result() and self.result()['seq'] > seq0 and self.result()['key'] == key, timeout,
                   'result for ' + key)
        return self.result()

    def close(self):
        self.mgr.stop()
        shutil.rmtree(self.cache, ignore_errors=True)


def series(s):
    return [None if x == '' else float(x) for x in s.split(',')] if s else []


def gets(reg, key):
    return [r for r in reg.requests('/get') if key in r['q'] or key in r['body']]


# ------------------------------------------------------------------ pure logic

class TestParsers(unittest.TestCase):
    def test_field_lists_match_firmware_counts(self):
        js = fr.SCRIPT.decode()
        counts = {ev: len(xc.field_list(js, c)) for ev, c in xc.STREAMS.items()}
        self.assertEqual(counts['CSVData'], 53)
        self.assertEqual(counts["CSVData2"], 653)
        self.assertEqual(counts['CSVData3'], 450)
        self.assertIn(counts['CSVData4'], (31, 32, 33, 36, 37))   # the round 3 power-flow fields; 36-37 the demo*A ones
        self.assertLessEqual(set(fr.FLOW_FIELDS), set(xc.field_list(js, 'CSV4_FIELDS')))
        self.assertEqual(counts['TimestampData'], 44)

    def test_counted_frame(self):
        self.assertEqual(xc.parse_counted('3,1,2.5,-2000000000'), [1.0, 2.5, -2e9])
        self.assertIsNone(xc.parse_counted('4,1,2,3'))
        self.assertIsNone(xc.parse_counted('3,1,x,3'))

    def test_baro(self):
        b, end = xc.parse_baro(fr.make_baro())
        self.assertEqual(len(b), 432)
        self.assertEqual(b[0], 1000.0)
        self.assertEqual(b[-1], (10000 + 431) / 10.0)
        self.assertEqual(end, 1_790_000_000)   # the newest sample's epoch, from the header

    def test_serial_matches(self):
        uids = [fr.UID, UID_B, '0000AAAAC0DE1234']
        self.assertTrue(xc.serial_matches(fr.UID, fr.UID.lower(), uids))
        self.assertFalse(xc.serial_matches(UID_B, fr.UID, uids))
        self.assertTrue(xc.serial_matches(UID_B, 'C0DE5678', uids))       # older firmware: the last 8 hex, unique
        self.assertFalse(xc.serial_matches(fr.UID, 'C0DE1234', uids))     # two pinned regulators end that way
        self.assertFalse(xc.serial_matches(fr.UID, '1234', uids))
        self.assertFalse(xc.serial_matches(fr.UID, None, uids))

    def test_identity_helpers(self):
        self.assertEqual(xc.uid6('000028621AD9B4DC'), '28621A')   # the app's uid.slice(-12, -6), never the OUI tail
        self.assertEqual(xc.unit_service('000028621AD9B4DC'), 'com.victronenergy.switch.xregulator_28621a')
        self.assertEqual(xc.label('000028621AD9B4DC', 'Port engine'), 'Port engine (XREG-28621A)')
        self.assertEqual(xc.label('000028621AD9B4DC', 'XREG-28621A'), 'XREG-28621A')
        self.assertEqual(xc.norm_uid(' 000028621ad9b4dc '), '000028621AD9B4DC')
        self.assertEqual(xc.norm_uid('28621A'), '')
        self.assertEqual([xc.gx_version(i) for i in ({'gx': 2}, {'gx': 1}, {}, {'gx': 'x'})], [2, 1, 0, 0])
        self.assertIsNone(xc.xreg_ident(b'{"device":"printer","uid":"000028621AD9B4DC"}'))
        self.assertIsNone(xc.xreg_ident(b'{"device":"xreg-010","uid":"none"}'))
        self.assertEqual(xc.xreg_ident(b'{"device":"XREG-010","uid":"000028621ad9b4dc"}')['uid'], '000028621ad9b4dc')
        self.assertEqual(xc._instance('switch:101', 100), 101)
        self.assertEqual(xc._instance('', 100), 100)

    def test_circular_mean(self):
        r = [math.radians(a) for a in (350, 10)]
        m = xc.circ_mean(sum(map(math.sin, r)), sum(map(math.cos, r)), False)
        self.assertLess(min(m, 360 - m), 1e-6)
        r = [math.radians(a) for a in (170, -170)]
        self.assertAlmostEqual(abs(xc.circ_mean(sum(map(math.sin, r)), sum(map(math.cos, r)), True)), 180.0, 6)

    def test_short_recorder_hold_and_gap(self):
        # per frame now (test_fastlane.py has the rest): two frames 1 s apart by sendMs, then a stale stream's gap
        s = xc.ShortRecorder()
        f = {'MeasuredAmps': 1000, 'AlternatorTemperatureF': 21200, 'sendMs': 5000, 'sessionId': 1}
        s.frame(0.0, 1, f, 1)
        s.frame(1.0, 1, dict(f, sendMs=6000), 1)
        s.gap(2.0)
        p = s.message(None).split(';')
        self.assertEqual(p[5], '0,1000,1000')
        self.assertEqual(p[6 + SHORT_NAMES.index('MeasuredAmps')], '10.0,10.0,')
        self.assertEqual(p[6 + SHORT_NAMES.index('AlternatorTemperatureC')], '100.0,100.0,')
        self.assertEqual(p[6 + SHORT_NAMES.index('chargeStage')], '1,1,')
        self.assertEqual(p[6 + SHORT_NAMES.index('Bcur')], ',,')

    def test_wind_recorder(self):
        w = xc.WindRecorder()
        for a in (350, 10, 350, 10):
            w.sample({'TrueWindSpeedNMEA': 1000, 'TrueWindAngleNMEA': -170 if a == 350 else 170,
                      'ApparentWindSpeedNMEA': 1500, 'ApparentWindAngleNMEA': a})
        w.close()
        w.close()   # empty bucket = gap
        out = w.strings()
        self.assertEqual(out['Tws'], '10.0,')
        self.assertEqual(out['Aws'], '15.0,')
        awa = float(out['Awa'].split(',')[0])
        self.assertLess(min(awa, 360 - awa), 1)
        self.assertEqual(abs(float(out['Twa'].split(',')[0])), 180)


class TestLongTerm(unittest.TestCase):
    def setUp(self):
        self.buf = fr.make_longterm(n=1000, gap_at=(400, 420), v5=True)
        self.lt = xc.parse_longterm(self.buf)

    def test_against_app_decoder(self):
        # Decode the same bytes with script.js's own parseLT() in node and compare record by record
        js = fr.SCRIPT.decode()
        start = js.index('  const ENV = [')
        end = js.index('function parseLT(buf)', start)
        depth, i = 0, js.index('{', end)
        while True:
            c = js[i]
            depth += c == '{'
            depth -= c == '}'
            i += 1
            if depth == 0:
                break
        code = js[start:i]
        binp = os.path.join(WORK, 'lt_test.bin')
        with open(binp, 'wb') as f:
            f.write(self.buf)
        script = code + '''
const fs = require('fs'); const b = fs.readFileSync(process.argv[2]);
const r = parseLT(b.buffer.slice(b.byteOffset, b.byteOffset + b.length));
const keep = r.t.map((_, i) => !r.isGap[i]);
const pick = a => a.filter((_, i) => keep[i]);
const out = { t: pick(r.t), stage: pick(r.stage), cover: pick(r.cover), f: {} };
for (const k of Object.keys(r.fields)) { out.f[k] = {}; for (const s of Object.keys(r.fields[k])) out.f[k][s] = pick(r.fields[k][s]); }
console.log(JSON.stringify(out));'''
        jsp = os.path.join(WORK, 'lt_test.js')
        with open(jsp, 'w') as f:
            f.write(script)
        app = json.loads(subprocess.check_output(['node', jsp, binp]))
        n = len(self.lt['recs'])
        self.assertEqual(len(app['t']), n)
        self.assertEqual([ts for ts, _ in self.lt['recs']], app['t'])
        span = self.lt['recs'][-1][0] - self.lt['recs'][0][0] + 600
        b = xc.bin_longterm(self.lt, span, max_points=10 ** 6)   # one record per sample
        idx = [round((ts - b['start']) / b['step']) for ts, _ in self.lt['recs']]
        checked = collections.Counter()
        for name in xc.LT_FIELDS:
            for suffix, key in (('', 'avg'), ('Min', 'min'), ('Max', 'max')):
                mine = b['series'].get(name + suffix)
                theirs = app['f'][name].get(key)
                if mine is None or theirs is None:
                    continue
                for j, i in enumerate(idx):
                    a, t = mine[i], theirs[j]
                    if t is None:
                        self.assertIsNone(a, (name, suffix, j))
                        continue
                    if name in xc.LT_TEMPS:
                        t = xc.F_TO_C(t)
                    self.assertAlmostEqual(a, t, 6, (name, suffix, j))
                    checked[name] += 1
        for name, _ in xc.LT_ONAVG:
            mine = b['series'][name + 'OnAvg']
            for j, i in enumerate(idx):
                t = app['f'][name]['onAvg'][j]
                if t is None:
                    self.assertIsNone(mine[i])
                else:
                    self.assertAlmostEqual(mine[i], t, 6)
                    checked[name + 'OnAvg'] += 1
        self.assertEqual([b['series']['chargeStage'][i] for i in idx], app['stage'])
        self.assertEqual([b['series']['cover'][i] for i in idx], app['cover'])
        self.assertGreater(sum(checked.values()), 10000)
        self.assertGreater(checked['battTemp'], 3 * 700)   # the v5 tail, read where the app reads it
        os.remove(binp)
        os.remove(jsp)

    def test_grids(self):
        b = xc.bin_longterm(self.lt, 3600)
        self.assertEqual((b['n'], b['step']), (6, 600))
        self.assertEqual(b['start'], 1_790_000_000 - 5 * 600)
        self.assertAlmostEqual(b['series']['battVolt'][-1], 13.99)
        self.assertAlmostEqual(b['series']['battVoltMin'][-1], 13.89)
        self.assertAlmostEqual(b['series']['altTemp'][-1], xc.F_TO_C(150.0))
        b = xc.bin_longterm(self.lt, 86400)
        self.assertEqual((b['n'], b['step']), (144, 600))
        b = xc.bin_longterm(self.lt, 30 * 86400)
        self.assertEqual((b['n'], b['step']), (300, 8640))
        self.assertIsNone(b['series']['battVolt'][0])          # before the first record
        filled = [i for i, v in enumerate(b['series']['battVolt']) if v is not None]
        self.assertGreater(len(filled), 60)
        awa = [v for v in b['series']['awa'] if v is not None]
        self.assertTrue(all(min(v % 360, 360 - v % 360) < 15 for v in awa), awa[:5])   # 350/10 -> near 0, never 180
        b = xc.bin_longterm(self.lt, 7 * 86400)
        gap_ts = 1_790_000_000 - (999 - 410) * 600
        gi = round((gap_ts - b['start']) / b['step'])
        self.assertIsNone(b['series']['battVolt'][gi])           # the 20-record hole stays a hole
        self.assertIsNotNone(b['series']['battVolt'][-1])

    def test_last_values(self):
        lt = xc.parse_longterm(fr.make_longterm(n=20, v5=True))
        last = xc.longterm_last(lt)
        self.assertAlmostEqual(last['battVolt'], 13.01)
        self.assertEqual(last['altTemp'], round(xc.F_TO_C(150.0), 1))
        self.assertEqual(last['battTemp'], 25.0)            # record 19 has one (records 0, 5, 10, 15 do not)
        self.assertEqual(last['baro'], 1013.2)
        self.assertNotIn('sog', last)                       # never recorded: no value, the legend keeps its dash
        self.assertEqual(xc.longterm_last(None), {})

    def test_v5_battery_temperature(self):
        lt = xc.parse_longterm(fr.make_longterm(n=20, v5=True))
        self.assertTrue(lt['v5'])
        b = xc.bin_longterm(lt, 20 * 600)
        bt = b['series']['battTemp']
        self.assertEqual(len(bt), 20)
        self.assertIsNone(bt[0])                                         # record 0: no battery reading
        self.assertAlmostEqual(bt[1], 25.0)                              # 77 F
        self.assertAlmostEqual(b['series']['battTempMax'][1], xc.F_TO_C(78.0))
        self.assertAlmostEqual(b['series']['battVolt'][-1], 13.01)        # the v4 part is read as before
        self.assertEqual(b['series']['cover'][-1], 50.0)                  # activeFrac 100 = 50 %, not the tail
        self.assertAlmostEqual(b['series']['altCurrOnAvg'][-1], 48.0)
        v4 = xc.bin_longterm(xc.parse_longterm(fr.make_longterm(n=20)), 20 * 600)
        self.assertEqual(set(v4['series']['battTemp']), {None})          # older rings: an empty series
        self.assertEqual(xc.lt_decimals('battTempMin'), 1)

    def test_unsynced_and_empty(self):
        lt = xc.parse_longterm(fr.make_longterm(n=10, unsynced_tail=3), wall=lambda: 0)
        ts = [t for t, _ in lt['recs']]
        self.assertEqual(ts, sorted(ts))
        self.assertEqual(len(ts), 10)
        self.assertEqual(xc.bin_longterm({'recs': [], 'interval': 600}, 3600)['n'], 0)
        self.assertIsNone(xc.parse_longterm(b'\x00' * 8))


class TestSettingsModel(unittest.TestCase):
    def setUp(self):
        self.m = xc.SettingsModel.from_text(SCHEMA_TEXT)
        self.csv3 = {f: 0.0 for f in fr.FIELDS['CSVData3']}
        reg = fr.FakeReg.__new__(fr.FakeReg)
        self.frames = {'CSVData3': self.csv3}

    def test_answer_wording(self):
        core = xc.Core.__new__(xc.Core)
        self.assertEqual(core._answer(409, b'Turn the alternator off before changing RPM scaling')[1],
                         'Turn the alternator off before changing the RPM scaling factor.')
        self.assertEqual(core._answer(409, b'000088621AD9B4DC')[1], 'A different regulator answered at this address')
        self.assertEqual(core._answer(403, b'Not available from a Victron GX'),
                         (False, 'Not available from a Victron GX'))
        self.assertEqual(core._answer(200, b'Rejected: out of range'), (False, 'Rejected: out of range'))
        self.assertEqual(core._answer(200, b'OK'), (True, ''))

    def test_outline_walk(self):
        # every item of the nested outline reaches the model, wherever it sits (pages hold items and deeper pages)
        schema = json.loads(SCHEMA_TEXT)
        self.assertNotIn('sections', schema)
        leaves = []

        def walk(nodes):
            for n in nodes:
                if 'children' in n:
                    walk(n['children'])
                else:
                    leaves.append(n)
        walk(schema['outline'])
        keys = {it['key'] for it in leaves if it['type'] not in ('table', 'info')}
        self.assertEqual(keys - set(self.m.items), set())
        self.assertEqual({it['tableId'] for it in leaves if it['type'] == 'table'}, set(self.m.tables))
        self.assertIn('HiLow', self.m.values)                       # drawn by the table, still a setting of its own
        self.assertIn('PhysicalPanelOverride', self.m.values)       # hidden, on a page with nothing else shown
        self.assertNotIn('@connection', self.m.items)               # GX-only page, nothing to publish

    def test_counts_and_kinds(self):
        self.assertIn('rpmTable', self.m.tables)
        self.assertEqual(len(self.m.tables['rpmTable']['cells']), 10)
        self.assertEqual(len(self.m.tables['rpmTable']['cells'][0]), 4)
        for k in ('BulkVoltage', 'TemperatureLimitF', 'rpmCapKW3', 'kneeMarginPct'):
            self.assertIn(k, self.m.values)
        self.assertNotIn('owAssignAlt', self.m.values)   # probe roles go through /Settings/ProbeRole
        self.assertEqual(self.m.probe_roles, ['owAssignAlt', 'owAssignBatt', 'owAssignExtra'])
        for k in ('owScan', 'SilenceAlarm', 'AltZeroCaptureNow'):
            self.assertIn(k, self.m.actions)
        self.assertIn('kneeLearnState', self.m.endpoints)
        self.assertFalse(self.m.items['SilenceAlarm']['arm'])
        self.assertFalse([k for k in self.m.values + self.m.actions if not re.fullmatch(r'[A-Za-z0-9_]+', k)])

    def test_display_and_wire(self):
        self.csv3['BulkVoltage'] = 1420
        self.csv3['rpmCapPowerTable2'] = 750
        it = self.m.items['BulkVoltage']
        self.assertEqual(self.m.display(it, self.frames), 14.2)
        self.assertEqual(self.m.wire(it, self.m.check(it, 14.25)), '14.25')
        self.assertEqual(self.m.display(self.m.items['rpmCapKW2'], self.frames), 0.75)
        with self.assertRaises(ValueError):
            self.m.check(self.m.items['TemperatureLimitF'], 999)
        with self.assertRaises(ValueError):
            self.m.check(self.m.items['ShuntLead'], 7)
        self.csv3['BulkVoltage'] = -2000000000
        self.assertIsNone(self.m.display(it, self.frames))
        f = dict(self.frames, **{'HTTP:kneeLearnState': {'marginPct': 2.5, 'enable': True}})
        self.assertEqual(self.m.display(self.m.items['kneeMarginPct'], f), 2.5)
        self.assertEqual(self.m.display(self.m.items['kneeLearnEnable'], f), 1)

    def test_table_params(self):
        for i in range(10):
            self.csv3['rpmTableRPMPoints%d' % i] = 500 + 300 * i
            self.csv3['rpmCapCurrentTable%d' % i] = 5000
            self.csv3['rpmCapPowerTable%d' % i] = 1500
            self.csv3['rpmMinDutyTable%d' % i] = 1250
        self.csv3['capLimitMode'] = 1
        p = self.m.table_params('rpmTable', {'rpmCapCurrentTable3': 77}, self.frames)
        self.assertEqual(p[0], ('capLimitMode', '1'))
        # learning state unknown: the unedited Keep-Alive cells stay out (the app disables them while learning runs)
        self.assertEqual(len(p), 31)
        d = dict(p)
        self.assertEqual(d['rpmCapCurrentTable3'], '77')
        self.assertEqual(d['rpmCapCurrentTable2'], '125')   # kW mode: amps derived from 1.5 kW at 12 V (no CSV1 yet)
        self.assertEqual(d['rpmCapKW0'], '1.5')
        self.assertNotIn('rpmMinDutyTable9', d)
        self.assertEqual(d['rpmTableRPMPoints9'], '3200')
        off = dict(self.frames, **{'HTTP:kneeLearnState': {'enable': 0}})
        on = dict(self.frames, **{'HTTP:kneeLearnState': {'enable': 1}})
        self.assertEqual(len(self.m.table_params('rpmTable', {}, off)), 41)
        self.assertEqual(dict(self.m.table_params('rpmTable', {}, off))['rpmMinDutyTable9'], '12.5')
        d = dict(self.m.table_params('rpmTable', {'rpmMinDutyTable2': 30}, on))
        self.assertFalse([k for k in d if k.startswith('rpmMinDuty')])   # learning owns the column: never sent
        d = dict(self.m.table_params('rpmTable', {'rpmMinDutyTable2': 30, 'rpmMinDutyTable3': 12.5}, self.frames))
        self.assertEqual(d['rpmMinDutyTable2'], '30')   # unknown state: a real edit still goes,
        self.assertNotIn('rpmMinDutyTable3', d)          # a cell copied from the echo does not
        # amps mode: kW derived from the amps at the live battery voltage (IBV), as the app's form fills it
        self.csv3['capLimitMode'] = 0
        d = dict(self.m.table_params('rpmTable', {'rpmCapCurrentTable3': 150},
                                     dict(off, CSVData={'IBV': 1380, 'BatteryV': 1385})))
        self.assertEqual(d['rpmCapKW3'], '2.07')
        self.assertEqual(d['rpmCapKW0'], '0.69')
        self.assertEqual(d['rpmCapCurrentTable0'], '50')
        self.csv3['capLimitMode'] = 1
        with self.assertRaises(ValueError) as e:
            self.m.table_params('rpmTable', {'rpmTableRPMPoints2': 100}, self.frames)
        self.assertIn('RPM values must increase', str(e.exception))
        edits = {'rpmTableRPMPoints%d' % i: (0 if i in (3, 9) else 500 + 300 * i) for i in range(10)}
        with self.assertRaises(ValueError) as e:
            self.m.table_params('rpmTable', edits, self.frames)
        self.assertIn('Row 4 (0) must be greater than Row 3 (1100)', str(e.exception))   # the app reports this first
        edits = {'rpmTableRPMPoints%d' % i: (0 if i == 0 else 500 + 300 * i) for i in range(10)}
        with self.assertRaises(ValueError) as e:
            self.m.table_params('rpmTable', edits, self.frames)
        self.assertIn('Zero RPM values are only allowed at the end of the table. Found zero at row 1', str(e.exception))
        edits = {'rpmTableRPMPoints%d' % i: (0 if i >= 6 else 500 + 300 * i) for i in range(10)}
        self.assertEqual(len(self.m.table_params('rpmTable', edits, off)), 41)


# ------------------------------------------------------------------ against the fake regulator

class TestGxFirmware(unittest.TestCase):
    def setUp(self):
        self.reg = fr.FakeReg()
        self.h = Harness(self.reg)

    def tearDown(self):
        self.h.close()
        self.reg.close()

    def test_wind_hour_true_and_apparent(self):
        # the Dashboard's wind tile plots apparent wind (/History/AppWind); /History/Wind keeps the true wind means
        old = xc.WIND_EVERY_S
        xc.WIND_EVERY_S = 1
        try:
            h, pub = self.h, self.h.pub
            h.until(lambda: pub['/Connected'] == 1, 15, 'connected')
            h.until(lambda: len([x for x in series(pub['/History/AppWind'])[1:] if x is not None]) >= 2, 10, 'apparent means')
            self.assertEqual((pub['/Live/AppWind'], pub['/Live/TrueWind']), (15.0, 12.0))
            self.assertEqual({x for x in series(pub['/History/AppWind'])[1:] if x is not None}, {15.0})
            self.assertEqual({x for x in series(pub['/History/Wind'])[1:] if x is not None}, {12.0})
        finally:
            xc.WIND_EVERY_S = old

    def test_live_link_and_cache(self):
        h, pub = self.h, self.h.pub
        h.until(lambda: pub['/Connected'] == 1, 15, 'connected')
        h.tick_for(2.5)
        self.assertEqual(pub['/Link/Channel'], 'gxevents')
        self.assertEqual(pub['/Link/Status'], 'Connected')
        self.assertEqual(pub['/Link/Firmware'], '0.0.9')
        self.assertEqual(pub['/Serial'], fr.UID)
        self.assertEqual(pub['/CustomName'], 'Port engine')
        # the Dashboard's Alt chip reads the tile's alternator (CSV4 flowAltA, 68.0) while the flow group streams (bench
        # round 8, 2026-10-29); CSV1 MeasuredAmps still feeds the switch pane's alternator readout
        self.assertEqual(pub['/Live/AltAmps'], 68.0)
        self.assertAlmostEqual(pub['/GenericInput/0/Value'], 50.0, delta=1.05)
        self.assertEqual(pub['/Live/BattV'], 13.8)
        self.assertEqual(pub['/Live/BattTemp'], 25.0)
        self.assertEqual(pub['/Live/Soc'], 85.0)
        self.assertEqual(pub['/Live/Baro'], 1013.2)
        h.until(lambda: pub['/History/BaroEnd'] is not None, 5, 'barometer history')
        self.assertEqual(series(pub['/History/Baro'])[-1], (10000 + 431) / 10.0)
        self.assertEqual(pub['/History/BaroEnd'], 1_790_000_000)
        self.assertEqual(pub['/Live/Health'], 93.0)
        self.assertEqual(pub['/Live/AltCapMax'], 110.0)
        self.assertEqual(pub['/SwitchableOutput/0/State'], 1)
        self.assertEqual(pub['/Health/Live/latchPct'], 93.0)
        self.assertIsNone(pub['/Perf/Live/pct'])                 # not wanted
        self.assertEqual(pub['/Settings/Value/BulkVoltage'], 14.2)
        self.assertEqual(json.loads(pub['/Settings/Table/rpmTable'])['rpmCapKW1'], 0.5)
        self.assertEqual((pub['/Link/Host'], pub['/Mgmt/Connection']), (self.reg.host, 'WiFi ' + self.reg.host))
        self.assertEqual((pub['/DeviceInstance'], pub['/SwitchableOutput/0/Settings/Group']), (100, 'XREG-FEEDC0'))
        self.assertEqual((pub['/Link/OtherUid'], pub['/Link/OtherName']), ('', ''))
        self.assertEqual(self.reg.requests('/armSettings'), [])
        self.assertEqual(self.reg.requests('/gxPair'), [])
        self.assertEqual(self.reg.script_downloads, 1)
        # a second bridge start revalidates script.js by ETag instead of downloading it
        link = xc.Link(fr.UID, self.reg.host, xc.Discovery(browse=False), h.cache)
        link._field_lists(xc.Http(self.reg.host))
        self.assertEqual(self.reg.script_downloads, 1)
        self.assertTrue(any(r['path'] == '/script.js' for r in self.reg.log[-3:]))

    def test_short_plots_demand_and_fast_schedule(self):
        h, pub = self.h, self.h.pub
        h.until(lambda: pub['/Connected'] == 1, 15, 'connected')
        h.tick_for(1)
        self.assertEqual(pub['/Fast/Short'], '')
        h.want('Short')
        h.until(lambda: len([v for v in h.short('MeasuredAmps') if v is not None]) >= 3, 8, 'short samples')
        temps = [v for v in h.short('AlternatorTemperatureC') if v is not None]
        self.assertAlmostEqual(temps[-1], 65.6, delta=0.05)
        self.assertEqual(h.short('chargeStage')[-1], 1)
        self.assertEqual(h.short('BatteryV')[-1], 13.85)
        h.until(lambda: '2' in self.reg.fast, 5, 'fast=2')
        h.core.want_at['Short'] -= 60
        h.tick_for(0.5)
        self.assertEqual(pub['/Fast/Short'], '')
        h.until(lambda: self.reg.fast and self.reg.fast[-1] == '0', 5, 'fast=0')

    def test_long_term(self):
        h, pub = self.h, self.h.pub
        h.until(lambda: pub['/Connected'] == 1, 15, 'connected')
        h.want('Long')
        h.until(lambda: pub['/Plot/Long/Fields'] != '', 15, 'long-term published')
        self.assertEqual(pub['/Plot/Long/Range'], '24h')
        self.assertEqual(pub['/Plot/Long/StepS'], 600)
        self.assertEqual(pub['/Plot/Long/Fields'].split(',')[:3], ['battVolt', 'battCurr', 'altCurr'])
        bv = series(pub['/Plot/Long/battVolt'])
        self.assertEqual(len(bv), 144)
        self.assertEqual(bv[-1], 13.99)
        self.assertEqual(series(pub['/Plot/Long/altTemp'])[-1], 65.6)
        self.assertEqual(series(pub['/Plot/Long/baro'])[-1], 1013.2)
        self.assertEqual(series(pub['/Plot/Long/socMax'])[-1], 90.0)
        self.assertEqual(pub['/Plot/Long/Fields'].split(',')[-1], 'battTemp')
        self.assertEqual(set(series(pub['/Plot/Long/battTemp'])), {None})   # a v4 ring has no battery temperature
        self.assertEqual(json.loads(pub['/Plot/Long/Last'])['battVolt'], 13.99)
        fetches = len(self.reg.requests('/longTermPlots.bin'))
        self.assertTrue(h.write('/Ui/LongRange', '1h'))
        self.assertFalse(h.write('/Ui/LongRange', '2h'))
        h.tick_for(0.3)
        self.assertEqual(pub['/Plot/Long/Range'], '1h')
        self.assertEqual(len(series(pub['/Plot/Long/battVolt'])), 6)
        self.assertEqual(len(self.reg.requests('/longTermPlots.bin')), fetches)   # re-binned from the cached ring
        h.write('/Ui/LongRange', '30d')
        h.tick_for(0.3)
        self.assertEqual(pub['/Plot/Long/StepS'], 8640)
        self.assertEqual(len(series(pub['/Plot/Long/rpm'])), 300)
        h.core.want_at['Long'] -= 60
        h.tick_for(0.3)
        self.assertEqual(pub['/Plot/Long/battVolt'], '')
        self.assertIsNone(pub['/Plot/Long/StartEpoch'])

    def test_health_perf_wind(self):
        h, pub = self.h, self.h.pub
        h.until(lambda: pub['/Connected'] == 1, 15, 'connected')
        h.want('Health')
        h.want('Perf')
        h.want('Wind')
        # (toggle lane, 2026-10-29) one trend per Health window: /Ui/Want/HealthTrend/<h> and /Health/TrendCsv/<h>
        h.write('/Ui/Want/HealthTrend/0', int(time.time()))
        h.until(lambda: pub['/Health/TrendCsv/0'].startswith('engHours') and pub['/Health/SessionCsv'].startswith('sec')
                and pub['/Perf/CurveCsv'].startswith('BEFRONT1'), 10, 'health and perf files')
        h.until(lambda: pub['/Perf/Live/pct'] == 87.25 and pub['/Perf/Motor/rpm'] == 1800.0, 5, 'perf live')
        # the 10 h window asks the regulator to thin within the last 10 engine-hours
        self.assertTrue(h.write('/Ui/Want/HealthTrend/10', int(time.time())))
        h.until(lambda: any(r['q'].get('hours') == '10' for r in self.reg.requests('/alttrend.csv')), 5, 'hours=10')
        self.assertIn('motorLive', json.loads(pub['/Perf/Schema']))
        h.until(lambda: len([v for v in series(pub['/Plot/Wind/Tws']) if v is not None]) >= 2, 6, 'wind samples')
        self.assertEqual([v for v in series(pub['/Plot/Wind/Tws']) if v is not None][-1], 12.0)
        self.assertEqual([v for v in series(pub['/Plot/Wind/Aws']) if v is not None][-1], 15.0)
        twa = [v for v in series(pub['/Plot/Wind/Twa']) if v is not None][-1]
        self.assertGreater(abs(twa), 160)   # 170 and -170 average to 180, never 0
        for t in ('Health', 'Perf', 'Wind'):
            h.core.want_at[t] -= 60
        for w in (0, 10):
            h.core.trend_want_at[w] -= 60
        h.tick_for(0.3)
        self.assertEqual((pub['/Health/TrendCsv/0'], pub['/Health/TrendCsv/10']), ('', ''))
        self.assertEqual(pub['/Perf/CurveCsv'], '')
        self.assertIsNone(pub['/Perf/Live/pct'])
        self.assertEqual(pub['/Plot/Wind/Tws'], '')
        self.assertEqual(pub['/Health/Live/latchPct'], 93.0)   # live health fields are always published

    def test_keyless_gx_writes(self):
        h, pub, reg = self.h, self.h.pub, self.reg
        h.until(lambda: pub['/Connected'] == 1, 15, 'connected')
        # gx=1 and the pinned uid on the write itself; no /armSettings at all; value held, then echoed
        self.assertTrue(h.write('/Settings/Value/BulkVoltage', 14.3))
        h.core.tick(pub)
        self.assertEqual(pub['/Settings/Value/BulkVoltage'], 14.3)
        r = h.wait_result('BulkVoltage')
        self.assertTrue(r['ok'], r)
        g = gets(reg, 'BulkVoltage')[-1]['q']
        self.assertEqual((g['BulkVoltage'], g['gx'], g['uid']), ('14.3', '1', fr.UID))
        self.assertTrue(g['src'].startswith('gx') and len(g['src']) == 10)
        self.assertNotIn('gxk', g)
        h.tick_for(3.5)
        self.assertEqual(pub['/Settings/Value/BulkVoltage'], 14.3)   # the regulator's echo
        self.assertEqual(reg.csv3['BulkVoltage'], 1430)
        # an out-of-range write is refused before anything is sent
        n_before = len(reg.requests('/get'))
        self.assertFalse(h.write('/Settings/Value/TemperatureLimitF', 999))
        h.core.tick(pub)
        self.assertFalse(h.result()['ok'])
        self.assertIn('Out of range', h.result()['message'])
        self.assertEqual(len(reg.requests('/get')), n_before)
        # the switch pane: OFF then ON, both flagged and tagged, n rising
        self.assertTrue(h.write('/SwitchableOutput/0/State', 0))
        self.assertTrue(h.wait_result('OnOff')['ok'])
        self.assertTrue(h.write('/SwitchableOutput/0/State', 1))
        self.assertTrue(h.wait_result('OnOff')['ok'])
        ons = gets(reg, 'OnOff')
        self.assertEqual([(x['q']['OnOff'], x['q']['gx']) for x in ons[-2:]], [('0', '1'), ('1', '1')])
        self.assertLess(int(ons[-2]['q']['n']), int(ons[-1]['q']['n']))
        self.assertTrue(h.write('/Settings/Action/SilenceAlarm', 1))
        self.assertTrue(h.wait_result('SilenceAlarm')['ok'])
        self.assertEqual(gets(reg, 'SilenceAlarm')[-1]['q']['gx'], '1')
        self.assertEqual(reg.requests('/armSettings'), [])
        self.assertFalse(reg.armed)
        self.assertEqual(reg.requests('/gxPair'), [])

    def test_table_action_source_hedge(self):
        h, pub, reg = self.h, self.h.pub, self.reg
        h.until(lambda: pub['/Connected'] == 1, 15, 'connected')
        h.tick_for(0.5)
        self.assertTrue(h.write('/Settings/Table/rpmTable', json.dumps({'rpmCapCurrentTable3': 77})))
        self.assertTrue(h.wait_result('rpmTable')['ok'])
        g = [r for r in reg.requests('/get') if 'rpmCapCurrentTable3' in r['q']][-1]
        names = [k for k, _ in urllib.parse.parse_qsl(urllib.parse.urlsplit(g['raw']).query)]
        self.assertEqual(names[0], 'capLimitMode')
        self.assertEqual(len([k for k in names if k.startswith('rpm')]), 30)   # Keep-Alive left out: learning unknown
        self.assertEqual(g['q']['rpmCapCurrentTable3'], '77')
        self.assertEqual(g['q']['rpmCapKW0'], '0.28')        # amps mode: 20 A x 13.8 V, as the app derives it
        self.assertEqual(g['q']['rpmCapKW3'], '1.06')        # from the edited 77 A
        self.assertNotIn('rpmMinDutyTable1', g['q'])
        self.assertEqual(g['q']['rpmTableRPMPoints9'], '3200')
        h.tick_for(3.5)
        self.assertEqual(json.loads(pub['/Settings/Table/rpmTable'])['rpmCapCurrentTable3'], 77)
        self.assertFalse(h.write('/Settings/Table/rpmTable', json.dumps({'rpmTableRPMPoints2': 100})))
        h.core.tick(pub)
        self.assertIn('RPM values must increase', h.result()['message'])
        # an action fires on every press, even the same value twice
        self.assertTrue(h.write('/Settings/Action/owScan', 1))
        self.assertTrue(h.wait_result('owScan')['ok'])
        h.core.tick(pub)
        self.assertTrue(h.write('/Settings/Action/owScan', 1))
        self.assertTrue(h.wait_result('owScan')['ok'])
        self.assertEqual(len(gets(reg, 'owScan')), 2)
        # an endpoint-backed page: polled only while wanted, echoes from it
        self.assertEqual(pub['/Settings/Http/kneeLearnState'], '')
        h.want('Settings')
        h.until(lambda: pub['/Settings/Value/kneeMarginPct'] == 2.5, 5, 'knee echo')
        self.assertEqual(json.loads(pub['/Settings/Http/tempSensors'])['probes'][0]['role'], 0)
        self.assertFalse([q for q, *_ in h.core.static_paths() if '/Source/' in q])
        h.core.want_at['Settings'] -= 60
        h.tick_for(0.3)
        self.assertEqual(pub['/Settings/Http/kneeLearnState'], '')
        # control write: a hedge copy with the same n goes out after 500 ms unanswered
        reg.delay_onoff_s = 1.0
        h.write('/SwitchableOutput/0/State', 0)
        h.wait_result('OnOff')
        h.write('/SwitchableOutput/0/State', 1)
        self.assertTrue(h.wait_result('OnOff')['ok'])
        ons = [r for r in gets(reg, 'OnOff') if r['q']['OnOff'] == '1']
        self.assertEqual(len(ons), 2)
        self.assertEqual(ons[0]['q']['n'], ons[1]['q']['n'])
        self.assertAlmostEqual(ons[1]['t'] - ons[0]['t'], 0.5, delta=0.25)

    def test_settings_pages_and_probe_roles(self):
        # the settings pages' own names: /Ui/Want/Settings, /Settings/Http/<endpoint>, /Settings/ProbeRole
        h, pub, reg = self.h, self.h.pub, self.reg
        h.until(lambda: pub['/Connected'] == 1, 15, 'connected')
        self.assertEqual(pub['/Settings/Http/tempSensors'], '')
        h.want('Settings')
        h.until(lambda: pub['/Settings/Http/tempSensors'] and pub['/Settings/Http/kneeLearnState']
                and pub['/Settings/Http/zerofitstate'], 5, 'settings sources')
        h.until(lambda: pub['/Settings/Value/kneeLearnEnable'] == 1, 5, 'knee echo')
        # learning on: a full table from the page, Keep-Alive column included, goes out without that column
        cells = json.loads(pub['/Settings/Table/rpmTable'])
        cells['rpmCapCurrentTable4'] = 88
        cells['capLimitMode'] = 0
        self.assertTrue(h.write('/Settings/Table/rpmTable', json.dumps(cells)))
        self.assertTrue(h.wait_result('rpmTable')['ok'])
        g = [r for r in reg.requests('/get') if 'rpmCapCurrentTable4' in r['q']][-1]
        self.assertEqual(g['q']['rpmCapCurrentTable4'], '88')
        self.assertFalse([k for k in g['q'] if k.startswith('rpmMinDuty')])
        # probe moves from Alternator to Battery: clear, then bind, two requests in that order
        n0 = len(reg.requests('/get'))
        self.assertTrue(h.write('/Settings/ProbeRole', json.dumps({'id': fr.PROBE.upper(), 'from': 0, 'to': 1})))
        self.assertTrue(h.wait_result('ProbeRole')['ok'])
        sent = [(k, v) for r in reg.requests('/get')[n0:] for k, v in r['q'].items() if k.startswith('owAssign')]
        self.assertEqual(sent, [('owAssignAlt', 'none'), ('owAssignBatt', fr.PROBE)])
        self.assertEqual(reg.roles, {'owAssignAlt': '', 'owAssignBatt': fr.PROBE, 'owAssignExtra': ''})
        h.core.tick(pub)
        self.assertIsNone(pub['/Settings/ProbeRole'])   # the same change can be written again
        h.until(lambda: json.loads(pub['/Settings/Http/tempSensors'])['probes'][0]['role'] == 1, 5, 're-polled')
        # a stale "from" makes the bind collide with the role it still holds: refused, reported
        self.assertTrue(h.write('/Settings/ProbeRole', json.dumps({'id': fr.PROBE, 'from': -1, 'to': 2})))
        r = h.wait_result('ProbeRole')
        self.assertFalse(r['ok'])
        self.assertIn('already assigned', r['message'])
        self.assertFalse(h.write('/Settings/ProbeRole', json.dumps({'id': 'xyz', 'from': 0, 'to': 1})))
        # a setting this firmware does not know: HTTP 200 "No message sent" is a refusal, and the hold lets go
        reg.unknown_keys.add('SolarWatts')
        self.assertTrue(h.write('/Settings/Value/SolarWatts', 450))
        r = h.wait_result('SolarWatts')
        self.assertFalse(r['ok'])
        h.tick_for(0.3)
        self.assertEqual(pub['/Settings/Value/SolarWatts'], 0)
        # the topic lapses: the endpoint text is cleared and polling stops
        h.core.want_at['Settings'] -= 60
        h.tick_for(0.3)
        self.assertEqual(pub['/Settings/Http/tempSensors'], '')
        n1 = len(reg.requests('/tempSensors'))
        h.tick_for(6)
        self.assertEqual(len(reg.requests('/tempSensors')), n1)

    def test_gx_firmware_never_uses_phone_stream(self):
        h, pub, reg = self.h, self.h.pub, self.reg
        h.until(lambda: pub['/Connected'] == 1, 15, 'connected')
        reg.gxevents_refuse = True
        h.core.link.sock.shutdown(2)   # drop the stream: the reconnect finds /gxevents refusing
        h.tick_for(8)                  # past the 6.5 s freshness of the last CSV1 on the running schedule
        self.assertEqual(reg.requests('/events'), [])
        self.assertEqual(pub['/Connected'], 0)
        reg.gxevents_refuse = False
        h.until(lambda: pub['/Connected'] == 1, 20, 'back on /gxevents')
        self.assertEqual(pub['/Link/Channel'], 'gxevents')
        self.assertEqual(reg.requests('/events'), [])

    def test_interface_out_of_date(self):
        h, pub = self.h, self.h.pub
        h.until(lambda: pub['/Connected'] == 1, 15, 'connected')
        self.reg.extra_csv1_field = True
        h.until(lambda: pub['/Link/Status'].startswith('Interface Out of Date'), 10, 'mismatch status')
        self.assertIn('CSVData', pub['/Link/Status'])

    def test_uid_check_reconnects(self):
        h, pub = self.h, self.h.pub
        h.until(lambda: pub['/Connected'] == 1, 15, 'connected')
        n0 = len(self.reg.requests('/identify'))
        self.reg.uid_in_stream = '00000000DEADBEEF'
        h.until(lambda: len(self.reg.requests('/identify')) > n0, 10, 're-identify')


class TestGxWithoutSchemaEvents(unittest.TestCase):
    # Firmware whose /gxevents carries only the five CSV/TS frames (the fw_gx_link.md design as written)
    def test_health_and_perf_live_stay_empty(self):
        reg = fr.FakeReg(schema_events_on_gx=False)
        h = Harness(reg)
        try:
            h.until(lambda: h.pub['/Connected'] == 1, 15, 'connected')
            h.want('Perf')
            h.tick_for(3)
            self.assertIsNone(h.pub['/Live/Health'])
            self.assertIsNone(h.pub['/Health/Live/latchPct'])
            self.assertIsNone(h.pub['/Perf/Live/pct'])
            self.assertTrue(h.pub['/Perf/CurveCsv'].startswith('BEFRONT1'))   # the files still arrive
        finally:
            h.close()
            reg.close()


class TestOlderFirmware(unittest.TestCase):
    def test_legacy_arm_dance(self):
        # 0.0.7-0.0.9 (no gx flag) and the unshipped pairing build (gx 1): /events and unlock-write-relock
        for gx in (0, 1):
            with self.subTest(gx=gx):
                reg = fr.FakeReg(gx=gx)
                h = Harness(reg)
                try:
                    pub = h.pub
                    h.until(lambda: pub['/Connected'] == 1, 15, 'connected')
                    h.tick_for(0.5)
                    self.assertEqual(pub['/Link/Channel'], 'events')
                    self.assertTrue(h.write('/Settings/Value/BulkVoltage', 14.1))
                    self.assertTrue(h.wait_result('BulkVoltage')['ok'])
                    seq = [(r['method'], r['path'], r['body'].get('arm', ''), 'BulkVoltage' in r['q'])
                           for r in reg.log if r['path'] in ('/armSettings', '/get')]
                    self.assertEqual(seq, [('GET', '/armSettings', '', False), ('POST', '/armSettings', '1', False),
                                           ('GET', '/get', '', True), ('POST', '/armSettings', '0', False)])
                    self.assertFalse(reg.armed)
                    q = gets(reg, 'BulkVoltage')[0]['q']
                    self.assertEqual(q['uid'], fr.UID)
                    self.assertNotIn('gx', q)
                    self.assertNotIn('gxk', q)
                    h.want('Short')
                    h.tick_for(1)
                    for path in ('/gxevents', '/gxrate', '/gxPair'):
                        self.assertEqual(reg.requests(path), [], path)
                finally:
                    h.close()
                    reg.close()

    def test_legacy_password(self):
        reg = fr.FakeReg(gx=0, password=True)
        h = Harness(reg)
        try:
            h.until(lambda: h.pub['/Connected'] == 1, 15, 'connected')
            h.write('/Settings/Value/BulkVoltage', 14.1)
            r = h.wait_result('BulkVoltage')
            self.assertFalse(r['ok'])
            self.assertIn('settings password', r['message'])
            self.assertEqual(gets(reg, 'BulkVoltage'), [])
        finally:
            h.close()
            reg.close()

    def test_gx_writes_with_password(self):
        reg = fr.FakeReg(password=True)
        h = Harness(reg)
        try:
            h.until(lambda: h.pub['/Connected'] == 1, 15, 'connected')
            self.assertTrue(h.write('/Settings/Value/BulkVoltage', 14.1))
            self.assertTrue(h.wait_result('BulkVoltage')['ok'])
            self.assertEqual(reg.csv3['BulkVoltage'], 1410)
            self.assertEqual(reg.requests('/armSettings'), [])
        finally:
            h.close()
            reg.close()

    def test_unreachable_cached_address(self):
        reg = fr.FakeReg()
        host = reg.host
        reg.close()
        h = Harness(types.SimpleNamespace(uid=fr.UID, host=host))
        try:
            h.until(lambda: h.pub['/Link/Status'].startswith('XREG-FEEDC0 not found: no answer at ' + host), 10,
                    'not-found status')
            self.assertEqual(h.pub['/Connected'], 0)
            self.assertEqual(h.pub['/Link/Host'], host)   # the hint stays until the regulator answers elsewhere
            self.assertTrue(h.write('/SwitchableOutput/0/State', 0))
            r = h.wait_result('OnOff')
            self.assertEqual((r['ok'], r['message']), (False, 'Not connected to XREG-FEEDC0'))
        finally:
            h.close()


# ------------------------------------------------------------------ finding regulators by uid

class FakeInfo:
    def __init__(self, txt, addrs, port=80):
        self.properties = {k.encode(): v.encode() for k, v in txt.items()}
        self.port = port
        self._addrs = addrs

    def parsed_addresses(self):
        return list(self._addrs)


class TestDiscovery(unittest.TestCase):
    def test_one_address_one_regulator(self):
        d = xc.Discovery(browse=False)
        wake = d.watch[fr.UID] = threading.Event()
        d.note(fr.UID, '10.0.0.5', 'Port engine', '0.0.9', ['5EEDC0'], 'mdns')
        self.assertTrue(wake.is_set())
        wake.clear()
        d.note(fr.UID, '10.0.0.5', via='link')
        self.assertFalse(wake.is_set())                 # same address: nothing to wake for
        self.assertEqual(d.get(fr.UID)['pair'], ['5EEDC0'])   # an answer without a pair list keeps the last one
        d.note(UID_B, '10.0.0.5', 'Stbd engine', via='sweep')
        self.assertIsNone(d.get(fr.UID))                # B answers there now, so A is no longer known to be there
        self.assertEqual(d.hosts_for(UID_B), ['10.0.0.5'])
        self.assertEqual(d.hosts_for(fr.UID), [])
        d.note('not a uid', '10.0.0.9')
        self.assertEqual(len(d.snapshot()), 1)

    def test_stale_entries_expire_except_mdns(self):
        now = [1000.0]
        d = xc.Discovery(clock=lambda: now[0], browse=False)
        d.note(fr.UID, '10.0.0.5', via='sweep')
        d.note(UID_B, '10.0.0.6', via='mdns')
        now[0] += xc.FOUND_KEEP_S + 1
        self.assertEqual([e['uid'] for e in d.snapshot()], [UID_B])

    def test_mdns_events(self):
        d = xc.Discovery(browse=False)
        zc = types.SimpleNamespace(get_service_info=lambda t, n, timeout: FakeInfo(
            {'uid': '0000feedc0de1234', 'name': 'Port engine', 'fw': '0.0.10', 'host': 'xreg-feedc0',
             'pair': '5EEDC0,zz,28621a'}, ['fe80::1', '172.20.10.3']))
        added = types.SimpleNamespace(name='Added')
        d._mdns_event(zeroconf=zc, service_type=xc.MDNS_TYPE, name='xreg-feedc0.' + xc.MDNS_TYPE, state_change=added)
        e = d.get(fr.UID)
        self.assertEqual((e['host'], e['name'], e['fw'], e['pair'], e['via']),
                         ('172.20.10.3', 'Port engine', '0.0.10', ['5EEDC0', '28621A'], 'mdns'))
        zc.get_service_info = lambda t, n, timeout: FakeInfo({'uid': UID_B}, ['172.20.10.4'], port=8080)
        d._mdns_event(zeroconf=zc, service_type=xc.MDNS_TYPE, name='xreg-5eedc0.' + xc.MDNS_TYPE, state_change=added)
        self.assertEqual(d.hosts_for(UID_B), ['172.20.10.4:8080'])
        zc.get_service_info = lambda t, n, timeout: FakeInfo({'uid': 'garbage'}, ['172.20.10.9'])
        d._mdns_event(zeroconf=zc, service_type=xc.MDNS_TYPE, name='other.' + xc.MDNS_TYPE, state_change=added)
        d._mdns_event(zeroconf=zc, service_type=xc.MDNS_TYPE, name='xreg-feedc0.' + xc.MDNS_TYPE,
                      state_change=types.SimpleNamespace(name='Removed'))
        self.assertEqual([x['uid'] for x in d.snapshot()], [UID_B])

    def test_own_subnet_hosts(self):
        out = ('1: lo    inet 127.0.0.1/8 scope host lo\\       valid_lft forever preferred_lft forever\n'
               '5: wifi0    inet 172.20.10.2/28 metric 100 brd 172.20.10.15 scope global wifi0\\       valid_lft\n'
               '6: ap0    inet 172.24.24.1/24 scope global ap0\\       valid_lft forever preferred_lft forever\n'
               '7: ll-eth0    inet 169.254.13.70/16 scope link ll-eth0\\       valid_lft forever\n'
               '8: eth0    inet 10.1.2.3/16 brd 10.1.255.255 scope global eth0\n')
        real = xc.subprocess.run
        xc.subprocess.run = lambda *a, **k: types.SimpleNamespace(stdout=out)
        try:
            hosts = xc.own_subnet_hosts()
        finally:
            xc.subprocess.run = real
        self.assertEqual(len(hosts), 13 + 253 + 253)       # a /28, a /24, and a /16 cut to its own /24
        self.assertIn('172.20.10.14', hosts)
        self.assertNotIn('172.20.10.2', hosts)
        self.assertNotIn('172.24.24.1', hosts)
        self.assertIn('10.1.2.254', hosts)
        self.assertFalse([x for x in hosts if x.startswith(('169.254.', '127.', '10.1.3.'))])


def two_regs(**kw):
    a = fr.FakeReg(**kw)
    b = fr.FakeReg(uid=UID_B, name='Stbd engine', **kw)
    b.csv3['BulkVoltage'] = 1380
    return a, b


class TestUnits(unittest.TestCase):
    def setUp(self):
        self.a, self.b = two_regs()
        self.hs = []

    def tearDown(self):
        for h in self.hs:
            h.close()
        self.a.close()
        self.b.close()

    def harness(self, *args, **kw):
        h = Harness(*args, **kw)
        self.hs.append(h)
        return h

    def test_found_list_add_and_restart(self):
        a, b = self.a, self.b
        a.paired = [UID_B]
        h = self.harness([a, b], pin={})
        h.tick_for(0.5)
        self.assertEqual((h.mpub['/Units'], h.pubs), ('[]', {}))   # nothing is pinned until the owner picks one
        self.assertTrue(h.mwrite('/Scan', 1))
        h.until(lambda: len(h.found()) == 2, 10, 'both found')
        f = h.found()
        self.assertEqual({k: f[a.uid][k] for k in ('id', 'name', 'host', 'fw', 'unit')},
                         {'id': 'FEEDC0', 'name': 'Port engine', 'host': a.host, 'fw': '0.0.9', 'unit': 0})
        self.assertEqual((f[UID_B]['id'], f[UID_B]['name'], f[UID_B]['boat']), ('5EEDC0', 'Stbd engine', 0))
        self.assertIsNone(h.mpub['/Scan'])
        for reg in (a, b):
            self.assertEqual(reg.requests('/gxevents') + reg.requests('/events'), [])
        self.assertEqual(h.store['units'], '')
        self.assertFalse(h.mwrite('/Add', '0000000000000001'))     # never found
        # pick one: its own service, VRM instance and settings
        self.assertTrue(h.mwrite('/Add', a.uid))
        h.until(lambda: h.units()[a.uid]['connected'] == 1, 15, 'A connected')   # /Units trails a unit by a tick
        u = h.units()[a.uid]
        self.assertEqual((u['service'], u['instance'], u['name'], u['host'], u['connected']),
                         ('com.victronenergy.switch.xregulator_feedc0', 100, 'Port engine', a.host, 1))
        self.assertEqual((h.store['units'], h.unit_stores[a.uid]['host']), (a.uid, a.host))
        h.until(lambda: h.found()[UID_B]['boat'] == 1, 5, 'B offered as on this boat')   # A lists B as its pair
        self.assertEqual(h.found()[a.uid]['unit'], 1)
        self.assertTrue(h.mwrite('/Add', UID_B))
        h.until(lambda: h.connected(UID_B), 15, 'B connected')
        self.assertEqual(h.pubs[UID_B]['/DeviceInstance'], 101)
        h.until(lambda: h.pubs[a.uid]['/Settings/Value/BulkVoltage'] == 14.2
                and h.pubs[UID_B]['/Settings/Value/BulkVoltage'] == 13.8, 10, 'each its own settings')
        self.assertEqual((h.pubs[a.uid]['/CustomName'], h.pubs[UID_B]['/CustomName']), ('Port engine', 'Stbd engine'))
        self.assertEqual(h.store['units'], a.uid + ',' + UID_B)
        # each write goes to its own regulator, addressed by uid
        self.assertTrue(h.write('/Settings/Value/BulkVoltage', 13.9, uid=UID_B))
        self.assertTrue(h.until(lambda: b.csv3['BulkVoltage'] == 1390, 5, 'B written'))
        self.assertEqual(a.csv3['BulkVoltage'], 1420)
        self.assertEqual(gets(b, 'BulkVoltage')[-1]['q']['uid'], UID_B)
        # a restart: both come back from the store alone, no search and no pick
        h.close()
        self.hs.remove(h)
        h2 = self.harness([a, b], store=h.store, unit_stores=h.unit_stores)
        h2.until(lambda: h2.connected(a.uid) and h2.connected(UID_B), 15, 'both back')
        self.assertEqual([u['uid'] for u in json.loads(h2.mpub['/Units'])], [a.uid, UID_B])
        self.assertEqual((h2.pubs[a.uid]['/DeviceInstance'], h2.pubs[UID_B]['/DeviceInstance']), (100, 101))

    def test_manual_address_then_remove(self):
        a = self.a
        h = self.harness([], pin={})
        self.assertFalse(h.mwrite('/Probe', 'not an address!'))
        self.assertTrue(h.mwrite('/Probe', a.host))
        h.until(lambda: h.mpub['/ProbeResult'], 5, 'probe answer')
        r = json.loads(h.mpub['/ProbeResult'])
        self.assertEqual({k: r[k] for k in ('seq', 'host', 'ok', 'uid', 'id', 'name', 'unit')},
                         {'seq': 1, 'host': a.host, 'ok': True, 'uid': a.uid, 'id': 'FEEDC0', 'name': 'Port engine',
                          'unit': 0})
        self.assertEqual(h.mpub['/Probe'], '')            # ready for the next address
        self.assertEqual(h.pubs, {})                       # the owner confirms first
        self.assertTrue(h.mwrite('/Add', a.uid))
        h.until(lambda: h.connected(a.uid), 15, 'connected')
        dead = fr.FakeReg()
        dead_host = dead.host
        dead.close()
        self.assertTrue(h.mwrite('/Probe', dead_host))
        h.until(lambda: json.loads(h.mpub['/ProbeResult'])['seq'] == 2, 5, 'second probe')
        r = json.loads(h.mpub['/ProbeResult'])
        self.assertFalse(r['ok'])
        self.assertTrue(r['message'].startswith('No answer at ' + dead_host), r)
        # an address probe of a regulator already pinned only tells it where it is
        self.assertTrue(h.mwrite('/Probe', a.host))
        h.until(lambda: json.loads(h.mpub['/ProbeResult'])['seq'] == 3, 5, 'third probe')
        self.assertEqual(json.loads(h.mpub['/ProbeResult'])['unit'], 1)
        self.assertTrue(h.mwrite('/Add', a.uid))
        h.tick_for(0.3)
        self.assertEqual(list(h.mgr.units), [a.uid])
        # remove
        self.assertFalse(h.mwrite('/Remove', UID_B))
        self.assertTrue(h.mwrite('/Remove', a.uid))
        h.tick_for(0.3)
        self.assertEqual((h.mgr.units, h.pubs, h.store['units'], h.forgotten), ({}, {}, '', [a.uid]))
        self.assertEqual(h.mpub['/Units'], '[]')

    def test_swapped_addresses_follow_uid(self):
        a, b = self.a, self.b
        host_a, host_b = a.host, b.host
        h = self.harness([a, b], pin={fr.UID: host_a, UID_B: host_b})
        h.until(lambda: h.connected(fr.UID) and h.connected(UID_B), 15, 'both connected')
        h.until(lambda: h.pubs[fr.UID]['/Settings/Value/BulkVoltage'] == 14.2
                and h.pubs[UID_B]['/Settings/Value/BulkVoltage'] == 13.8, 10, 'settings echoed')
        fr.swap(a, b)   # DHCP hands each unit the other's address
        end, wrong = time.time() + 30, []
        while time.time() < end:
            h.tick()
            pa, pb = h.pubs[fr.UID], h.pubs[UID_B]
            if pa['/Settings/Value/BulkVoltage'] not in (14.2, None) or pa['/CustomName'] != 'Port engine':
                wrong.append(('A', pa['/Settings/Value/BulkVoltage'], pa['/CustomName']))
            if pb['/Settings/Value/BulkVoltage'] not in (13.8, None) or pb['/CustomName'] != 'Stbd engine':
                wrong.append(('B', pb['/Settings/Value/BulkVoltage'], pb['/CustomName']))
            if pa['/Link/Host'] == host_b and pb['/Link/Host'] == host_a and h.connected(fr.UID) \
                    and h.connected(UID_B) and pa['/Settings/Value/BulkVoltage'] == 14.2 \
                    and pb['/Settings/Value/BulkVoltage'] == 13.8:
                break
            time.sleep(0.1)
        else:
            self.fail('units did not follow their uids: %s / %s' % (h.pubs[fr.UID]['/Link/Status'],
                                                                     h.pubs[UID_B]['/Link/Status']))
        self.assertEqual(wrong, [])
        self.assertEqual((h.unit_stores[fr.UID]['host'], h.unit_stores[UID_B]['host']), (host_b, host_a))
        # a write to A now goes to the server at B's old address, which is A
        self.assertTrue(h.write('/Settings/Value/BulkVoltage', 14.4, uid=fr.UID))
        self.assertTrue(h.wait_result('BulkVoltage')['ok'])
        self.assertEqual(gets(b, 'BulkVoltage')[-1]['q']['uid'], fr.UID)
        self.assertEqual((b.csv3['BulkVoltage'], a.csv3['BulkVoltage']), (1440, 1380))

    def test_missing_unit_refuses_writes_and_repins(self):
        b = self.b
        h = self.harness([b], pin={fr.UID: b.host})      # A was last seen where B answers now; A is nowhere
        pub = h.pub
        want = 'XREG-FEEDC0 not found. Stbd engine (XREG-5EEDC0) answers at ' + b.host
        h.until(lambda: pub['/Link/Status'] == want, 10, 'mismatch status')
        self.assertEqual((pub['/Connected'], pub['/Link/OtherUid'], pub['/Link/OtherName']),
                         (0, UID_B, 'Stbd engine'))
        self.assertEqual((pub['/Serial'], pub['/CustomName']), (fr.UID, ''))
        self.assertIsNone(pub['/Live/BattV'])
        self.assertIsNone(pub['/Settings/Value/BulkVoltage'])
        self.assertEqual(b.requests('/gxevents') + b.requests('/events'), [])   # B's numbers never reach A's service
        self.assertEqual(h.units()[fr.UID]['connected'], 0)
        for path, value, key in (('/Settings/Value/BulkVoltage', 14.0, 'BulkVoltage'),
                                 ('/SwitchableOutput/0/State', 0, 'OnOff'),
                                 ('/Settings/Action/SilenceAlarm', 1, 'SilenceAlarm')):
            self.assertTrue(h.write(path, value))
            r = h.wait_result(key)
            self.assertEqual((r['ok'], r['message']), (False, 'Not connected to XREG-FEEDC0'), key)
        self.assertEqual(b.requests('/get'), [])
        # "Use this regulator": B takes A's place, its list position and VRM instance
        self.assertFalse(h.write('/Link/AcceptRegulator', '0000000000000001'))
        self.assertTrue(h.write('/Link/AcceptRegulator', UID_B))
        h.until(lambda: h.connected(UID_B), 15, 'B connected in A\'s place')
        self.assertEqual(list(h.mgr.units), [UID_B])
        self.assertEqual((h.store['units'], h.forgotten, h.unit_stores[UID_B]['host']), (UID_B, [fr.UID], b.host))
        self.assertEqual(h.pubs[UID_B]['/DeviceInstance'], 100)

    def test_repin_refused_when_the_other_is_pinned(self):
        b = self.b
        h = self.harness([b], pin={fr.UID: b.host, UID_B: b.host})
        h.until(lambda: h.pub['/Link/OtherUid'] == UID_B and h.connected(UID_B), 15, 'A missing, B connected')
        self.assertFalse(h.write('/Link/AcceptRegulator', UID_B))
        h.tick_for(0.3)
        self.assertEqual(sorted(h.mgr.units), sorted([fr.UID, UID_B]))

    def test_two_alternators_share_by_running(self):
        a, b = self.a, self.b
        a.gx = b.gx = 3
        h = self.harness([a, b], pin={a.uid: a.host, UID_B: b.host})
        h.until(lambda: h.alt(a.uid) is not None and h.alt(UID_B) is not None, 15, 'both alternator services')
        cap = lambda uid: h.alt(uid)['/Settings/ChargeCurrentLimit']
        h.until(lambda: cap(a.uid) == 110.0 and cap(UID_B) == 110.0, 5, 'both capacities')
        b.field_status, b.off_reason = 0, 14        # B's engine stops: A takes the whole remainder
        h.until(lambda: cap(UID_B) == 0, 5, 'B no share')
        self.assertEqual(cap(a.uid), 110.0)
        a.field_status, a.off_reason = 0, 14        # both stopped: both keep their capacity, so writes go on
        h.until(lambda: cap(a.uid) == 110.0 and cap(UID_B) == 110.0, 5, 'both keep capacity')
        a.field_status, a.off_reason = 1, 0         # A starts: B gives up its share again
        h.until(lambda: cap(UID_B) == 0 and cap(a.uid) == 110.0, 5, 'A running, B no share')

    def test_no_address_found_by_sweep(self):
        a = self.a
        h = self.harness([a], pin={a.uid: ''})
        h.until(lambda: h.connected(a.uid), 15, 'found by sweep, connected')
        self.assertEqual((h.pub['/Link/Host'], h.unit_stores[a.uid]['host']), (a.host, a.host))


# ------------------------------------------------------------------ DVCC over WiFi

N2K_TWIN = 'com.victronenergy.alternator.socketcan_vecan0_vi0_uc1684700'


class TestDvcc(unittest.TestCase):
    def setUp(self):
        self.reg = fr.FakeReg(gx=3)
        self.h = Harness(self.reg)
        self.h.until(lambda: self.h.pub['/Connected'] == 1 and self.h.alt() is not None, 15, 'connected, gx 3')

    def tearDown(self):
        self.h.close()
        self.reg.close()

    def feed(self, seconds, **paths):
        # systemcalc's ADJUST, sped up: the same values written again and again
        end = time.time() + seconds
        while time.time() < end:
            self.h.systemcalc(**paths)
            self.h.tick()
            time.sleep(0.1)

    def test_alternator_service_and_status(self):
        h, alt = self.h, self.h.alt()
        self.assertEqual(h.alts[fr.UID][0], 'shown')
        self.assertEqual({p: alt.modes[p] for p in xc.ALT_LINK_PATHS}, dict.fromkeys(xc.ALT_LINK_PATHS, 'stamp'))
        self.assertEqual((alt['/ProductName'], alt['/ProductId'], alt['/Serial'], alt['/DeviceInstance'],
                          alt['/Mgmt/Connection']), ('Alternator', 0xB091, fr.UID, 100, 'WiFi'))
        h.tick_for(0.5)
        self.assertAlmostEqual(alt['/Dc/0/Current'], 50.0, delta=1.01)
        self.assertEqual(alt['/Dc/0/Voltage'], 13.8)
        self.assertAlmostEqual(alt['/Dc/0/Power'], 690, delta=15)
        self.assertEqual((alt['/State'], alt['/Connected'], alt['/FirmwareVersion']), (3, 1, '0.0.9'))
        self.assertEqual(alt['/Settings/ChargeCurrentLimit'], 110.0)   # Command Limit 150 A, largest table entry 110 A
        self.reg.csv3['MaxTableValue'] = 9000
        self.reg.csv3_dirty.set()
        h.until(lambda: alt['/Settings/ChargeCurrentLimit'] == 90.0, 5, 'Command Limit below the table')
        # the dashboard's follow status: CSV2 state and merged limits, TS carrier ages
        pub = h.pub
        h.until(lambda: pub['/Live/DvccWifiAge'] is not None, 10, 'TS frame')
        self.assertEqual((pub['/Live/DvccState'], pub['/Live/DvccUntrust'], pub['/Live/DvccCvl'], pub['/Live/DvccCcl']),
                         (3, 0, 14.2, 37.5))
        self.assertEqual((pub['/Live/DvccWifiAge'], pub['/Live/DvccCanAge']), (1200, 999999))
        self.assertEqual(pub['/Link/GxLimits/State'], xc.LIMITS_IDLE)       # systemcalc not writing: nothing posted
        self.assertEqual(self.reg.requests('/gxLimits'), [])

    def test_twin_and_native_support(self):
        h = self.h
        # an NMEA 2000 service for this regulator: ours keeps only the DVCC paths, so nothing counts twice
        h.mgr.set_alternators([{'service': N2K_TWIN, 'serial': fr.UID, 'link': False},
                               {'service': 'com.victronenergy.alternator.other', 'serial': 'X1', 'link': True}])
        h.tick()
        self.assertEqual(h.alts[fr.UID][0], 'hidden')
        alt = h.alt()
        for p in ('/ProductName', '/State', '/Dc/0/Voltage', '/Dc/0/Current', '/Dc/0/Power'):
            self.assertNotIn(p, alt)
        for p in xc.ALT_LINK_PATHS + ('/Settings/ChargeCurrentLimit', '/DeviceInstance', '/ProductId'):
            self.assertIn(p, alt)
        h.mgr.set_alternators([{'service': N2K_TWIN, 'serial': fr.UID[-8:], 'link': False}])   # older 8-hex serial
        h.tick()
        self.assertEqual(h.alts[fr.UID][0], 'hidden')
        # forwarding, then Victron's own control of this regulator appears: one empty post, our service goes
        self.feed(0.8, ChargeCurrent=37.5)
        n = len(self.reg.limits)
        self.assertGreater(n, 0)
        h.mgr.set_alternators([{'service': N2K_TWIN, 'serial': fr.UID, 'link': True}])
        h.tick_for(1.0)
        self.assertIsNone(h.alt())
        self.assertEqual(self.reg.limits[n:], [{}])
        self.assertEqual(h.pub['/Link/GxLimits/State'], xc.LIMITS_NATIVE)
        h.mgr.set_alternators([])
        h.tick()
        self.assertEqual(h.alts[fr.UID][0], 'shown')

    def test_forwarding_freshness_and_the_empty_post(self):
        h, reg = self.h, self.reg
        self.feed(1.2, ChargeVoltage=14.2, ChargeCurrent=37.5)
        posts = reg.requests('/gxLimits')
        self.assertGreaterEqual(len(posts), 3)
        self.assertEqual({(r['q'].get('cvl'), r['q'].get('ccl'), r['q']['gx'], r['q']['uid']) for r in posts},
                         {('14.2', '37.5', '1', fr.UID)})
        gaps = [b['t'] - a['t'] for a, b in zip(posts, posts[1:])]
        self.assertGreater(min(gaps), 0.25)                               # one post per period, not per write
        self.assertEqual(h.pub['/Link/GxLimits/State'], xc.LIMITS_SENDING)
        self.assertEqual((h.pub['/Link/GxLimits/Cvl'], h.pub['/Link/GxLimits/Ccl']), (14.2, 37.5))
        # the BMS goes: systemcalc writes only the current; the voltage leaves the posts once its writes are stale
        self.feed(1.6, ChargeCurrent=37.5)
        self.assertEqual(reg.limits[-1], {'ccl': '37.5'})
        self.assertIsNone(h.pub['/Link/GxLimits/Cvl'])
        # systemcalc stops writing: its last value goes on for the freshness window, then one empty post, then nothing
        n, stopped = len(reg.limits), time.time()
        h.tick_for(1.6)
        tail = reg.limits[n:]
        self.assertEqual(tail[-1], {})
        self.assertEqual(tail[:-1], [{'ccl': '37.5'}] * (len(tail) - 1))
        end = [r for r in reg.requests('/gxLimits') if 'ccl' not in r['q'] and 'cvl' not in r['q']][-1]
        self.assertAlmostEqual(end['t'] - stopped, xc.DVCC_FRESH_S, delta=0.35)
        h.tick_for(0.8)
        self.assertEqual(reg.limits[n:], tail)
        self.assertEqual(h.pub['/Link/GxLimits/State'], xc.LIMITS_IDLE)
        # a stop-charging share is forwarded as it is, never replaced
        self.feed(0.5, ChargeCurrent=0)
        self.assertEqual(reg.limits[-1], {'ccl': '0'})

    def test_409_stops_and_rediscovers(self):
        h, reg = self.h, self.reg
        reg.limits_uid = UID_B   # the post reaches a unit that is not the pinned one
        start = len(reg.log)
        self.feed(1.5, ChargeCurrent=37.5)
        seq = [r['path'] for r in reg.log[start:] if r['path'] in ('/gxLimits', '/identify')]
        self.assertEqual(seq[0], '/gxLimits')
        self.assertNotIn(['/gxLimits', '/gxLimits'], [seq[i:i + 2] for i in range(len(seq) - 1)])   # each refusal: find it again
        self.assertIn('/identify', seq)
        self.assertEqual(reg.limits, [])
        reg.limits_uid = None
        h.until(lambda: h.pub['/Connected'] == 1, 10, 'back')
        self.feed(0.8, ChargeCurrent=37.5)
        self.assertEqual(reg.limits[-1], {'ccl': '37.5'})                 # posting resumes on the new connection

    def test_a_page_is_not_the_route(self):
        h, reg = self.h, self.reg
        reg.limits_page = '<html>Success</html>'
        self.feed(1.0, ChargeCurrent=37.5)
        self.assertEqual(len(reg.requests('/gxLimits')), 1)               # refused for this connection
        self.assertEqual(h.pub['/Link/GxLimits/State'], xc.LIMITS_REFUSED)

    def test_new_alternator_service_quiets_measurements(self):
        h, alt = self.h, self.h.alt()
        h.until(lambda: alt['/Dc/0/Current'] is not None, 5, 'measurements')
        h.mgr.alternator_appeared()                  # NameOwnerChanged: maybe the NMEA 2000 twin coming back
        h.tick()
        self.assertEqual(h.alts[fr.UID][0], 'shown')
        self.assertEqual((alt['/Dc/0/Current'], alt['/Dc/0/Power'], alt['/Dc/0/Voltage']), (None, None, None))
        h.mgr.set_alternators([{'service': 'com.victronenergy.alternator.other', 'serial': 'X1', 'link': False}],
                              settled=False)          # its serial is not readable yet: stay quiet
        h.tick_for(0.3)
        self.assertIsNone(alt['/Dc/0/Current'])
        h.mgr.set_alternators([{'service': 'com.victronenergy.alternator.other', 'serial': 'X1', 'link': False}])
        h.until(lambda: alt['/Dc/0/Current'] is not None, 5, 'not a twin: measurements back')
        h.mgr.alternator_appeared()
        h.mgr.set_alternators([{'service': N2K_TWIN, 'serial': fr.UID, 'link': False}])
        h.tick()
        self.assertEqual(h.alts[fr.UID][0], 'hidden')

    def test_capacity_while_held_off(self):
        h, reg = self.h, self.reg
        alt = h.alt()
        lim = lambda: alt['/Settings/ChargeCurrentLimit']
        h.until(lambda: lim() == 110.0, 5, 'capacity')
        reg.field_status, reg.off_reason = 0, 14   # engine stopped
        h.tick_for(1.0)
        self.assertEqual(lim(), 110.0)              # alone on the GX: keeps its share, so its limit holds at the start
        h.mgr.set_alternators([{'service': 'com.victronenergy.alternator.orionxs_hq2222', 'serial': 'HQ2222', 'link': True}])
        h.until(lambda: lim() == 0, 5, 'no share while held off beside another alternator')
        reg.field_status, reg.off_reason = 0, 0     # 0 % field under a followed 0 A limit: still free to charge
        h.until(lambda: lim() == 110.0, 5, 'a 0 A limit keeps the share')
        reg.field_status, reg.off_reason = 1, 3     # a warning with the field still on
        h.tick_for(1.0)
        self.assertEqual(lim(), 110.0)
        reg.field_status, reg.off_reason = 0, 10    # switched off
        h.until(lambda: lim() == 0, 5, 'switched off')
        reg.field_status, reg.off_reason = 1, 0     # engine start
        h.until(lambda: lim() == 110.0, 5, 'share back')

    def test_older_firmware_gets_no_alternator_service(self):
        for gx in (0, 2):
            with self.subTest(gx=gx):
                reg = fr.FakeReg(gx=gx)
                h = Harness(reg)
                try:
                    h.until(lambda: h.pub['/Connected'] == 1, 15, 'connected')
                    h.tick_for(0.5)
                    self.assertIsNone(h.alt())
                    self.assertEqual(h.pub['/Link/GxLimits/State'], xc.LIMITS_NONE)
                    self.assertEqual(reg.requests('/gxLimits'), [])
                finally:
                    h.close()
                    reg.close()


# ------------------------------------------------------------------ demo mode

class TestDemo(unittest.TestCase):
    def setUp(self):
        self.reg = fr.FakeReg(gx=3)
        self.h = None

    def tearDown(self):
        if self.h:
            self.h.close()
        self.reg.close()

    def start(self):
        self.h = Harness(self.reg)
        self.h.until(lambda: self.h.pub['/Connected'] == 1, 15, 'connected')
        self.h.want('Short')
        self.h.want('Wind')
        return self.h

    def test_toggle_enters_backfills_and_exit_clears(self):
        h, reg = self.start(), self.reg
        item = h.core.model.items['hardwarePresent']
        self.assertEqual((item['type'], [o['label'] for o in item['options']]), ('toggle', ['No', 'Yes']))
        h.until(lambda: len(h.short('MeasuredAmps')) >= 2, 10, 'real readings')
        self.assertFalse({'/History/AltAmps', '/History/BattV', '/History/Field', '/History/IntervalS'} & set(h.pub))
        # Hardware Present? = No from the GX: a flagged write, then the regulator's own CSV3 says demo
        self.assertTrue(h.write('/Settings/Value/hardwarePresent', 0))
        self.assertTrue(h.wait_result('hardwarePresent')['ok'])
        self.assertEqual(gets(reg, 'hardwarePresent')[-1]['q']['gx'], '1')
        h.until(lambda: h.core.demo, 10, 'demo seen')
        h.until(lambda: len(h.short('MeasuredAmps')) >= 300, 10, 'short term backfilled')
        amps = h.short('MeasuredAmps')
        self.assertIn(len(amps), (300, 301, 302))                         # the backfill, then a live frame or two
        self.assertAlmostEqual(amps[0], 40.0, delta=0.05)
        self.assertAlmostEqual(amps[299], 42.99, delta=1.5)               # the newest backfill row
        self.assertEqual(set(h.page.t[i] - h.page.t[i - 1] for i in range(1, 300)), {1000})
        self.assertEqual(h.short('AlternatorTemperatureC')[0], 100.0)
        self.assertEqual(h.short('chargeStage')[0], 1)
        self.assertEqual(series(h.pub['/History/Wind']), [9.0] * 120)    # 30 s means of three 10 s rows
        self.assertEqual(series(h.pub['/History/AppWind']), [10.0] * 120)  # apparent, the Dashboard's tile
        self.assertEqual(len(series(h.pub['/Plot/Wind/Tws'])), 360)
        self.assertEqual(series(h.pub['/Plot/Wind/Twa'])[-1], 45.0)
        self.assertEqual(len(reg.requests('/demoBackfill')), 1)
        h.tick_for(1.0)
        self.assertEqual(len(reg.requests('/demoBackfill')), 1)           # once per entry
        gen = h.core.link.snapshot()[2]['generation']
        # back to Yes: nothing simulated stays beside the real readings
        self.assertTrue(h.write('/Settings/Value/hardwarePresent', 1))
        h.until(lambda: not h.core.demo, 10, 'demo left')
        self.assertLess(len(h.short('MeasuredAmps')), 3)
        self.assertLess(len(series(h.pub['/Plot/Wind/Tws'])), 2)
        self.assertEqual(h.pub['/History/Wind'], '')
        self.assertEqual(h.pub['/History/AppWind'], '')
        self.assertEqual(len(reg.requests('/demoBackfill')), 1)
        # No again on the same stream (no reconnect between): this entry gets its own backfill (bench 2026-10-28: the
        # second demo of a session had an empty wind hour)
        self.assertEqual(h.core.link.snapshot()[2]['generation'], gen)
        self.assertTrue(h.write('/Settings/Value/hardwarePresent', 0))
        h.until(lambda: len(reg.requests('/demoBackfill')) == 2, 10, 'second entry backfilled')
        h.until(lambda: series(h.pub['/History/Wind']) == [9.0] * 120, 10, 'wind hour filled again')
        self.assertEqual(len(series(h.pub['/Plot/Wind/Tws'])), 360)
        self.assertEqual(h.core.link.snapshot()[2]['generation'], gen)

    def test_connect_while_in_demo(self):
        reg = self.reg
        reg.csv3['hardwarePresent'] = 0
        h = self.start()
        h.until(lambda: len(h.short('MeasuredAmps')) >= 300, 10, 'backfilled at connect')
        self.assertTrue(h.core.demo)
        self.assertEqual(len(reg.requests('/demoBackfill')), 1)
        # a new connection fills the rings again (its gap included)
        gen = h.core.link.snapshot()[2]['generation']
        h.core.link.sock.shutdown(2)
        h.until(lambda: h.core.link.snapshot()[2]['generation'] > gen and len(reg.requests('/demoBackfill')) == 2, 10,
                'backfilled again')
        self.assertTrue(h.core.demo)
        # the services keep publishing in demo
        self.assertEqual(h.pub['/Connected'], 1)
        self.assertIsNotNone(h.pub['/Live/AltAmps'])
        self.assertIsNotNone(h.alt())

    def test_exit_keeps_demo_wind_out(self):
        # bench 2026-10-28: the last demo CSV4 was still fresh after the exit cleared the plots, so the empty real wind
        # plot and the dashboard hour got one demo point (11.4 kn, 150 deg)
        h, reg = self.start(), self.reg
        reg.wind_demo_only = True
        self.assertTrue(h.write('/Settings/Value/hardwarePresent', 0))
        h.until(lambda: len(series(h.pub['/Plot/Wind/Tws'])) >= 360, 10, 'wind backfilled')
        h.tick_for(3.0)
        self.assertTrue(h.write('/Settings/Value/hardwarePresent', 1))
        h.until(lambda: not h.core.demo, 10, 'demo left')
        h.tick_for(12.0)
        self.assertEqual([x for x in series(h.pub['/Plot/Wind/Tws']) if x is not None], [])
        self.assertEqual([x for x in series(h.pub['/History/Wind']) if x is not None], [])
        self.assertEqual([x for x in series(h.pub['/History/AppWind']) if x is not None], [])
        self.assertIsNone(h.pub['/Live/AppWind'])

    def test_quiet_twin_in_demo_leaves_the_readings_to_wifi(self):
        # demo turns NMEA 2000 off: a VE.Can twin still listed has stopped, so our service carries the readings at once
        h, reg = self.start(), self.reg
        h.mgr.set_alternators([{'service': 'com.victronenergy.alternator.socketcan_vecan0_vi0_uc1', 'serial': fr.UID,
                                'link': False}])
        h.until(lambda: h.core.alt_mode() == 'hidden', 5, 'twin carries the readings')
        self.assertTrue(h.write('/Settings/Value/hardwarePresent', 0))
        h.until(lambda: h.core.demo and h.core.alt_mode() == 'shown', 10, 'ours in demo')
        h.until(lambda: h.alt()['/Dc/0/Current'] is not None, 5, 'WiFi readings')
        self.assertTrue(h.write('/Settings/Value/hardwarePresent', 1))
        h.until(lambda: not h.core.demo and h.core.alt_mode() == 'hidden', 10, 'the twin again after demo')

    def test_backfill_retried_while_entering(self):
        # CSV3 can say demo while the regulator is still entering it, when /demoBackfill answers 404
        h, reg = self.start(), self.reg
        reg.backfill_refuse = 2
        self.assertTrue(h.write('/Settings/Value/hardwarePresent', 0))
        h.until(lambda: len(reg.requests('/demoBackfill')) == 3, 10, 'retried until it answers')
        h.until(lambda: series(h.pub['/History/Wind']) == [9.0] * 120, 10, 'wind hour filled')
        h.tick_for(1.5)
        self.assertEqual(len(reg.requests('/demoBackfill')), 3)

    def test_backfill_gives_up(self):
        h, reg = self.start(), self.reg
        reg.backfill_refuse = 100
        self.assertTrue(h.write('/Settings/Value/hardwarePresent', 0))
        h.until(lambda: len(reg.requests('/demoBackfill')) == xc.DEMO_TRIES, 15, 'every try used')
        h.tick_for(2.5)
        self.assertEqual(len(reg.requests('/demoBackfill')), xc.DEMO_TRIES)

    def test_no_backfill_outside_demo(self):
        h = self.start()
        h.tick_for(1.5)
        self.assertFalse(h.core.demo)
        self.assertEqual(self.reg.requests('/demoBackfill'), [])

    def test_backfill_parser(self):
        b = xc.parse_backfill(json.dumps(fr.make_backfill(1_790_000_000)).encode())
        self.assertEqual((len(b['short']), len(b['wind'])), (300, 360))
        self.assertEqual(b['short'][-1]['MeasuredAmps'], 42.99)
        bad = {'v': 1, 'short': {'cols': ['IBV'], 'rows': [[13.5], [None], ['x'], [1, 2]]}}
        self.assertEqual(xc.parse_backfill(json.dumps(bad).encode()), {'short': [{'IBV': 13.5}, {'IBV': None},
                                                                                  {'IBV': None}], 'wind': []})
        with self.assertRaises(ValueError):
            xc.parse_backfill(b'{"v":2}')


# ------------------------------------------------------------------ round 3: power flow

class TestFlow(unittest.TestCase):
    # The redesigned tile's model (/Flow/Json): rows per source, House calculated, fixed full bar (2026-10-28)
    def setUp(self):
        self.reg = fr.FakeReg(gx=3)
        self.h = Harness(self.reg)
        self.h.until(lambda: self.h.pub['/Connected'] == 1, 15, 'connected')
        self.h.want('Flow')

    def tearDown(self):
        self.h.close()
        self.reg.close()

    def flow(self):
        t = self.h.pub['/Flow/Json']
        return json.loads(t) if t else None

    def wait_flow(self, what, pred, timeout=8):
        self.h.until(lambda: self.flow() is not None and pred(self.flow()), timeout, what)
        return self.flow()

    @staticmethod
    def rows(f):
        return {r['id']: r for r in f['rows']}

    def test_balance(self):
        # solar 22 + alternator 68 = house 40 + battery 50 (charging)
        f = self.wait_flow('first flow', lambda f: f['b']['a'] == 50.0)
        r = self.rows(f)
        self.assertEqual([x['id'] for x in f['rows']], ['alt', 'solar'])
        self.assertEqual((r['alt']['a'], r['solar']['a'], r['solar']['c'], f['house'], f['why']),
                         (68.0, 22.0, 'VE.Direct', 40.0, ''))
        self.assertEqual((f['b']['st'], f['b']['from'], f['b']['lim']), (1, 'Shunt', None))
        # full bar: the Command Limit (150 A) held to the largest Charge Rate Limits entry (110 A), as DVCC is told
        self.assertEqual(f['fs'], 110.0)
        self.assertEqual(self.h.pub['/Flow/Ok'], 1)

    def test_full_bar_follows_the_settings_not_the_flow(self):
        reg = self.reg
        self.wait_flow('first flow', lambda f: f['fs'] == 110.0)
        reg.flow.update(flowAltA=1000, flowBattA=-1800)                      # a quiet moment: still 110
        self.wait_flow('quiet', lambda f: self.rows(f)['alt']['a'] == 10.0)
        self.assertEqual(self.flow()['fs'], 110.0)
        reg.csv3.update(MaxTableValue=9000)
        reg.csv3_dirty.set()
        self.wait_flow('Command Limit 90 A', lambda f: f['fs'] == 90.0)
        for i in range(10):
            reg.csv3['rpmCapCurrentTable%d' % i] = 0
        reg.csv3_dirty.set()
        self.wait_flow('no table: the Command Limit alone', lambda f: f['fs'] == 90.0)

    def test_unmeasured_source(self):
        # a step shorter than FLOW_IMBALANCE_S (one input ahead of another) never shows the note
        self.wait_flow('balanced', lambda f: f['house'] == 40.0)
        self.reg.flow.update(flowBattA=10000)
        seen = []
        end = time.time() + 2.5                  # with the fake's CSV4 every 1 s, at most 3.5 s of the 4 s hold
        while time.time() < end:
            self.h.tick()
            seen.append(self.flow())
            time.sleep(0.1)
        self.reg.flow.update(flowBattA=5000)
        self.h.tick_for(2.0)
        self.assertTrue(any(f['b']['a'] == 100.0 for f in seen))
        self.assertEqual([f['why'] for f in seen if f['why']], [])
        self.reg.flow.update(flowBattA=10000)    # 100 A into the battery from 90 A measured
        f = self.wait_flow('excess', lambda f: f['b']['a'] == 100.0)
        # named once it has held FLOW_IMBALANCE_S (bench round 8, 2026-10-29); House reads the clamped sum until then
        self.assertEqual((f['house'], f['why']), (0.0, ''))
        f = self.wait_flow('named', lambda f: f['why'] == 'unmeasured', timeout=xc.FLOW_IMBALANCE_S + 2)
        self.assertEqual((f['house'], f['why'], f['x']), (None, 'unmeasured', 10.0))
        # inside 1 A it is sensor offset: House 0
        self.reg.flow.update(flowBattA=9080)
        f = self.wait_flow('offset', lambda f: f['b']['a'] == 90.8)
        self.assertEqual((f['house'], f['why']), (0.0, ''))

    def test_missing_readings(self):
        self.reg.flow.update(flowBattA=-2000000000)
        f = self.wait_flow('no battery', lambda f: f['why'] == 'batt')
        self.assertEqual((f['b']['a'], f['b']['st'], f['b']['from'], f['house'], self.rows(f)['alt']['a']),
                         (None, None, '', None, 68.0))
        self.reg.flow.update(flowBattA=5000, flowSolarA=-2000000000, flowSrc=13)   # an array set, nothing reads it
        f = self.wait_flow('solar unseen', lambda f: f['why'] == 'solar')
        self.assertEqual((self.rows(f)['solar']['a'], f['house'], f['b']['a']), (None, None, 50.0))
        self.reg.flow.update(flowSolarA=2200, flowSrc=11, flowAltA=-2000000000)
        f = self.wait_flow('alternator unseen', lambda f: f['why'] == 'alt')
        self.assertIsNone(self.rows(f)['alt']['a'])

    def test_no_array_no_solar_row(self):
        self.reg.flow.update(flowSolarA=0, flowSrc=10, flowBattA=4000)   # SolarWatts 0: the regulator sends 0.0
        f = self.wait_flow('no solar', lambda f: 'solar' not in self.rows(f))
        self.assertEqual(([x['id'] for x in f['rows']], f['house']), (['alt'], 28.0))

    def test_source_deadband(self):
        self.reg.flow.update(flowAltA=30, flowSolarA=-40, flowBattA=-1200)   # Hall noise at zero; 12 A out
        f = self.wait_flow('noise', lambda f: self.rows(f)['alt']['a'] == 0.0 and f['b']['st'] == -1)
        self.assertEqual((self.rows(f)['solar']['a'], f['house'], f['why']), (0.0, 12.0, ''))

    def test_battery_word_holds_two_seconds(self):
        self.wait_flow('charging', lambda f: f['b']['st'] == 1)
        self.reg.flow.update(flowBattA=-20)                                  # inside +-0.3 A: resting, after 2 s
        self.wait_flow('tiny discharge', lambda f: f['b']['a'] == -0.2)
        t0 = time.time()
        self.wait_flow('resting', lambda f: f['b']['st'] == 0)
        self.assertGreater(time.time() - t0, 1.0)
        self.reg.flow.update(flowBattA=-500)
        self.wait_flow('discharging', lambda f: f['b']['a'] == -5.0)
        self.assertEqual(self.flow()['b']['st'], 0)
        self.wait_flow('word changes', lambda f: f['b']['st'] == -1)

    def test_limit_line(self):
        reg = self.reg
        reg.csv3.update(BatteryShuntPresent=1, BattLimitEnable=1, BattCurrentLimitA=500)
        reg.csv3_dirty.set()
        reg.flow['ctrlLimiter'] = 4
        self.wait_flow('G4 limit', lambda f: f['b']['lim'] == 50.0)
        # an external limit: the VE.Can one is on the battery's basis, the WiFi one is the alternator's share
        reg.flow['ctrlLimiter'] = 8
        reg.dvcc.update(dvccRxCanCcl=600, dvccRxWifiCcl=375)
        reg.ts.update(ts_DvccVeCan=3000, ts_DvccWifi=1200)
        self.h.tick_for(4.5)
        self.assertIsNone(self.flow()['b']['lim'])                          # WiFi is the lower: alternator basis
        reg.ts.update(ts_DvccWifi=999999)
        self.wait_flow('VE.Can limit', lambda f: f['b']['lim'] == 60.0)
        reg.csv3.update(BatteryShuntPresent=0)
        reg.csv3_dirty.set()
        self.wait_flow('no shunt, no battery basis', lambda f: f['b']['lim'] is None)

    def test_only_while_shown(self):
        h = self.h
        self.wait_flow('shown', lambda f: True)
        h.core.want_at['Flow'] -= 60
        h.until(lambda: h.pub['/Flow/Json'] == '', 5, 'empty when not shown')
        self.assertEqual(h.pub['/Flow/Ok'], 1)
        self.assertNotIn('/Flow/Scale', h.pub)                               # the autoscale is gone
        h.want('Flow')
        self.wait_flow('shown again', lambda f: f['b']['a'] == 50.0)

    def test_csv4_hold_while_shown(self):
        h, reg = self.h, self.reg
        h.until(lambda: reg.flow_holds == ['1'], 5, 'flow=1')
        self.assertEqual(reg.fast, [])                                        # its own hold, not the fast tier
        h.core.want_at['Flow'] -= 60
        h.until(lambda: reg.flow_holds[-1:] == ['0'], 5, 'flow=0')


# ------------------------------------------------------------------ round 3: Cloud page

class FakeCloud:
    # get-leaderboard-bundle on localhost: answers what the test sets, records each request
    def __init__(self):
        import http.server, gzip as gz
        self.status, self.body, self.gzip, self.requests = 200, None, False, []
        cloudself = self

        class H(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_POST(self):
                n = int(self.headers.get('Content-Length') or 0)
                cloudself.requests.append({'path': self.path, 'headers': {k.lower(): v for k, v in self.headers.items()},
                                           'body': json.loads(self.rfile.read(n) or b'null')})
                b = json.dumps(cloudself.body).encode()
                self.send_response(cloudself.status)
                self.send_header('Content-Type', 'application/json')
                if cloudself.gzip:
                    b = gz.compress(b)
                    self.send_header('Content-Encoding', 'gzip')
                self.send_header('Content-Length', str(len(b)))
                self.end_headers()
                self.wfile.write(b)
        self.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), H)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.base = 'http://127.0.0.1:%d/functions/v1/' % self.server.server_address[1]

    def close(self):
        self.server.shutdown()
        self.server.server_close()


# the contract's synthetic example (cloud_contract.md), DemoBoat as the caller
CONTRACT_BUNDLE = {
    'as_of': '2026-10-28T12:22:51.208Z',
    'you': {'p': '0', 'username': 'DemoBoat', 'boat_type': 'monohull', 'boat_length_ft': 38,
            'boat_make_model': 'Beneteau 38', 'boat_year': 2008},
    'profiles': {
        '0': {'username': 'DemoBoat', 'boat_type': 'monohull', 'boat_length_ft': 38, 'boat_make_model': 'Beneteau 38',
              'boat_year': 2008},
        '1': {'username': 'Meridian', 'boat_type': 'catamaran', 'boat_length_ft': 42, 'boat_make_model': 'Lagoon 42',
              'boat_year': 2019},
        '2': {'username': 'Blue Heron', 'boat_type': 'monohull', 'boat_length_ft': 36,
              'boat_make_model': 'Catalina 36', 'boat_year': 1999},
        '3': {'username': 'Wanderer', 'boat_type': 'monohull', 'boat_length_ft': 44, 'boat_make_model': None,
              'boat_year': None}},
    'boards': {
        'sailing_days_alltime': {'top': [{'p': '1', 'v': 212}, {'p': '2', 'v': 148}, {'p': '3', 'v': 97}],
                                 'rank': 4, 'value': 61},
        'eng_hrs': {'top': [{'p': '1', 'v': 4413600}, {'p': '0', 'v': 3024000, 'you': True},
                            {'p': '2', 'v': 1980000}], 'rank': 2, 'value': 3024000},
        'farthest_lat_south': {'top': [{'p': '0', 'v': -41, 'you': True}, {'p': '1', 'v': -34.2},
                                       {'p': '3', 'v': -12.5}], 'rank': 1, 'value': -41},
        'board_temp_max_alltime': {'top': [{'p': '2', 'v': 131.5}], 'rank': None, 'value': None},
        'solar_power_max_alltime_w': {'error': 'database error'},
        'not_a_column': {'error': 'Unknown leaderboard column'}},
    'speed': [{'p': '1', 'v': 11.2}, {'p': '3', 'v': 8.9}, {'p': '2', 'v': 8.1}],
    'fleet': {'totalDevices': 4, 'activeDevices': 2, 'avgVoltage': 13.3, 'avgSoc': 84, 'totalDistance': 15210.4,
              'avgSpeed': 5.2, 'batteryCharged': 2100000, 'discharged': 1900000, 'altEnergy': 840000,
              'solarEnergy': 1300000, 'altFuel': 61.5, 'altTime': 410, 'engineHours': 2650, 'engineFuel': 1890,
              'calculatedAt': '2026-10-28T08:00:00.063882+00:00'}}


def board_tab(v, column):
    for s in v['sections']:
        for b in s['boards']:
            for t, (_l, c, _u) in zip(b['tabs'], next(x for x in [bb for _s, bs in xc.cloud.SECTIONS for bb in bs]
                                                     if x[0] == b['key'])[3]):
                if c == column:
                    return t
    raise KeyError(column)


class TestCloudView(unittest.TestCase):
    def test_registry_is_the_web_pages(self):
        cols = xc.cloud.columns()
        self.assertEqual(len(cols), 34)
        self.assertEqual(len(set(cols)), 34)
        with open(os.path.join(os.environ.get('XREG_CLOUD_WORKSPACE', os.path.expanduser('~/Projects/Cloud Workspace')), 'vercel', 'leaderboards.js')) as f:
            js = f.read()
        self.assertEqual(cols, re.findall(r"column: '(\w+)'", js))         # same columns, same order
        self.assertEqual(len(xc.cloud.SECTIONS), 7)

    def test_contract_example(self):
        v = xc.cloud.view(CONTRACT_BUNDLE)
        self.assertEqual(v['asOf'], 1793190171)                              # 2026-10-28T12:22:51Z
        self.assertEqual(v['you']['n'], 'DemoBoat')
        self.assertEqual(v['pulse'], '4 boats · 15,210 nm logged · 2,140 kWh generated')
        t = board_tab(v, 'sailing_days_alltime')                            # caller outside the top rows
        self.assertEqual([(r['r'], r['n'], r['you']) for r in t['rows']],
                         [(1, 'Meridian', 0), (2, 'Blue Heron', 0), (3, 'Wanderer', 0), (4, 'DemoBoat', 1)])
        self.assertEqual((t['gap'], t['rows'][-1]['t']), (1, '61 days'))
        t = board_tab(v, 'eng_hrs')                                         # inside them, seconds -> hours
        self.assertEqual([r['t'] for r in t['rows']], ['1,226 hrs', '840 hrs', '550 hrs'])
        self.assertEqual(t['gap'], 0)
        t = board_tab(v, 'farthest_lat_south')                              # hemisphere from the sign
        self.assertEqual(t['rows'][0]['t'], '41.0°S')
        t = board_tab(v, 'board_temp_max_alltime')                          # F carries the number for the GX unit
        self.assertEqual((t['rows'][0]['t'], t['rows'][0]['f'], t['rows'][0]['v']), ('131.5°F', 1, 131.5))
        self.assertEqual(board_tab(v, 'solar_power_max_alltime_w')['rows'], [])   # a failed column: no data
        self.assertEqual([(s['rank'], s['label']) for s in v['standings']],
                         [(1, 'Latitude · South'), (2, 'Engine hours'), (4, 'Under sail · Total Days')])
        self.assertEqual([(b['n'], b['len'], b['v'], b['type']) for b in v['speed']],
                         [('Meridian', 42.0, 11.2, 'catamaran'), ('Wanderer', 44.0, 8.9, 'monohull'),
                          ('Blue Heron', 36.0, 8.1, 'monohull')])
        self.assertEqual(v['fleet']['totalDevices'], 4)

    def test_format(self):
        f = xc.cloud.format_value
        self.assertEqual(f(1.23456, 'kW'), '1.23 kW')
        self.assertEqual(f(12345.6, 'nm'), '12,346 nm')
        self.assertEqual(f(7.25, 'kts'), '7.2 kts')   # same rounding as JS toFixed on this value
        self.assertEqual(f(45.0, '°N'), '45.0°N')
        self.assertEqual(f(-45.0, '°N'), '45.0°S')
        self.assertEqual(f(3, ''), '3')
        self.assertEqual(f(None, 'nm'), '---')

    def test_demo_bundle_has_every_board(self):
        v = xc.cloud.view(xc.cloud.demo_bundle(1_790_000_000))
        tabs = [t for s in v['sections'] for b in s['boards'] for t in b['tabs']]
        self.assertEqual(len(tabs), 34)
        self.assertTrue(all(len(t['rows']) >= 5 for t in tabs))
        self.assertTrue(all(any(r['you'] and r['n'] == 'DemoBoat' for r in t['rows']) for t in tabs))
        self.assertTrue(any(t['gap'] for t in tabs) and not all(t['gap'] for t in tabs))
        self.assertEqual(len(v['standings']), 6)
        self.assertEqual(sum(b['you'] for b in v['speed']), 1)
        self.assertGreater(len(json.dumps(v)), 10000)

    def test_demo_fleet_is_consistent(self):
        # one story per boat: the records a fleet like this would really hold, agreeing with each other
        b = xc.cloud.demo_bundle(1_790_000_000)
        prof = b['profiles']
        val = lambda col: {e['p']: e['v'] for e in b['boards'][col]['top']}
        full = {}
        for col in xc.cloud.columns():
            for k, x in val(col).items():
                full.setdefault(k, {})[col] = x
        for k, d in full.items():
            name = prof[k]['username']
            if 'sailing_dist_alltime' in d and 'total_dist_alltime' in d:
                self.assertLess(d['sailing_dist_alltime'], d['total_dist_alltime'], name)
            if 'longest_consecutive_days_under_sail' in d and 'longest_streak_days' in d:
                self.assertLessEqual(d['longest_consecutive_days_under_sail'], d['longest_streak_days'], name)
            if 'farthest_lat_north' in d and 'farthest_lat_south' in d and 'latitude_max_range' in d:
                self.assertAlmostEqual(d['latitude_max_range'], d['farthest_lat_north'] - d['farthest_lat_south'], 1)
            if 'alt_power_max_alltime_w' in d and 'alt_curr_max_alltime' in d:
                volts = d['alt_power_max_alltime_w'] / d['alt_curr_max_alltime']
                self.assertTrue(13.4 < volts < 14.8 or 26.8 < volts < 29.6, (name, volts))
            if 'best_nm_per_gallon' in d and 'nm_per_gallon_engine_on' in d and prof[k]['boat_type'] in ('monohull', 'catamaran'):
                self.assertGreater(d['best_nm_per_gallon'], d['nm_per_gallon_engine_on'], name)
            if prof[k]['boat_type'] in ('powerboat', 'trawler'):
                self.assertNotIn('sailing_days_alltime', d, name)          # never sails: off the sailing boards
        # speeds follow hull type and length: power fastest, then catamarans; displacement hulls near hull speed
        sp = {prof[e['p']]['username']: (prof[e['p']], e['v']) for e in b['speed']}
        hull = lambda p: 1.34 * (p['boat_length_ft'] * 0.9) ** 0.5
        for name, (p, kn) in sp.items():
            lo, hi = {'powerboat': (2.0, 5.0), 'catamaran': (1.3, 1.85), 'monohull': (0.95, 1.35),   # planing: 2-5
                      'trawler': (0.95, 1.12)}[p['boat_type']]
            self.assertTrue(lo <= kn / hull(p) <= hi, (name, kn, kn / hull(p)))
        # no pattern a viewer would spot: values differ, few are round
        for col in xc.cloud.columns():
            xs = [x for x in val(col).values() if not isinstance(x, int)]
            self.assertLessEqual(max([xs.count(x) for x in xs] or [0]), 2, col)
        floats = [x for col in xc.cloud.columns() for x in val(col).values() if isinstance(x, float)]
        self.assertLess(sum(1 for x in floats if x == int(x) and int(x) % 10 == 0) / len(floats), 0.05)
        # the fleet line agrees with the boards: the 16 public boats are a part of the 47
        f = b['fleet']
        self.assertGreater(f['totalDistance'], sum(full[k].get('total_dist_alltime', 0) for k in full))
        self.assertGreater(f['batteryCharged'], 0.85 * (f['altEnergy'] + f['solarEnergy']))
        self.assertTrue(4.0 < f['avgSpeed'] < 7.5)
        self.assertEqual(b['you']['username'], 'DemoBoat')


class TestCloud(unittest.TestCase):
    def setUp(self):
        self.cloud = FakeCloud()
        self.cloud.body = CONTRACT_BUNDLE
        self.base0, xc.cloud.CLOUD_BASE = xc.cloud.CLOUD_BASE, self.cloud.base
        self.reg = fr.FakeReg(gx=3)
        self.reg.csv3['CloudFeatures'] = 1
        self.h = None

    def tearDown(self):
        xc.cloud.CLOUD_BASE = self.base0
        if self.h:
            self.h.close()
        self.reg.close()
        self.cloud.close()

    def start(self):
        self.h = Harness(self.reg)
        self.h.until(lambda: self.h.pub['/Connected'] == 1, 15, 'connected')
        return self.h

    def state(self):
        return self.h.pub['/Cloud/State']

    def test_ready_only_while_shown(self):
        h = self.start()
        h.tick_for(1)
        self.assertEqual(self.cloud.requests, [])                           # nobody looking: no cloud call
        h.want('Cloud')
        h.until(lambda: self.state() == xc.CLOUD_READY, 10, 'ready')
        r = self.cloud.requests[0]
        self.assertEqual(r['path'], '/functions/v1/get-leaderboard-bundle')
        self.assertEqual((r['body']['token'], r['body']['limit'], r['body']['speed_limit'], len(r['body']['columns'])),
                         (self.reg.token, 10, 500, 34))
        self.assertEqual(r['headers']['apikey'], xc.cloud.CLOUD_ANON_KEY)
        self.assertEqual(r['headers']['authorization'], 'Bearer ' + xc.cloud.CLOUD_ANON_KEY)
        v = json.loads(h.pub['/Cloud/Json'])
        self.assertEqual((v['you']['n'], h.pub['/Cloud/AsOf']), ('DemoBoat', 1793190171))
        h.tick_for(1)
        self.assertEqual(len(self.cloud.requests), 1)                        # cached for 10 minutes
        self.assertTrue(h.write('/Cloud/Refresh', 1))
        h.until(lambda: len(self.cloud.requests) == 2, 10, 'refresh')
        h.core.want_at['Cloud'] -= 60
        h.tick_for(0.3)
        self.assertEqual(h.pub['/Cloud/Json'], '')                           # 40-60 KB only while the page shows
        self.assertEqual(self.state(), xc.CLOUD_READY)

    def test_gzip_answer(self):
        self.cloud.gzip = True
        h = self.start()
        h.want('Cloud')
        h.until(lambda: self.state() == xc.CLOUD_READY, 10, 'ready')
        self.assertEqual(self.cloud.requests[0]['headers']['accept-encoding'], 'gzip')

    def test_switched_off(self):
        self.reg.csv3['CloudFeatures'] = 0
        h = self.start()
        h.want('Cloud')
        h.until(lambda: self.state() == xc.CLOUD_OFF, 5, 'off')
        h.tick_for(1)
        self.assertIn('Cloud Features is switched off', h.pub['/Cloud/Message'])
        self.assertEqual((self.cloud.requests, self.reg.token_requests, h.pub['/Cloud/Json']), ([], 0, ''))

    def test_not_registered(self):
        self.reg.token = None
        h = self.start()
        h.want('Cloud')
        h.until(lambda: self.state() == xc.CLOUD_UNREGISTERED, 10, 'unregistered')
        self.assertIn('not registered', h.pub['/Cloud/Message'])
        self.assertEqual(self.cloud.requests, [])
        # a token the cloud no longer knows reads the same
        self.reg.token = 'gone'
        self.cloud.status, self.cloud.body = 401, {'error': 'Invalid token', 'code': 'token_not_found'}
        self.assertTrue(h.write('/Cloud/Refresh', 1))
        h.until(lambda: len(self.cloud.requests) == 1, 10, 'asked the cloud')
        h.tick_for(0.5)
        self.assertEqual(self.state(), xc.CLOUD_UNREGISTERED)

    def test_offline_keeps_the_last_bundle(self):
        h = self.start()
        h.want('Cloud')
        h.until(lambda: self.state() == xc.CLOUD_READY, 10, 'ready')
        xc.cloud.CLOUD_BASE = 'http://127.0.0.1:1/functions/v1/'             # nothing listens there
        self.assertTrue(h.write('/Cloud/Refresh', 1))
        h.until(lambda: self.state() == xc.CLOUD_OFFLINE, 20, 'offline')
        self.assertEqual(h.pub['/Cloud/Message'], 'No internet connection on the GX')
        self.assertEqual(json.loads(h.pub['/Cloud/Json'])['you']['n'], 'DemoBoat')
        self.assertEqual(h.pub['/Cloud/AsOf'], 1793190171)

    def test_server_error(self):
        self.cloud.status, self.cloud.body = 503, {'error': 'Device lookup failed, retry'}
        h = self.start()
        h.want('Cloud')
        h.until(lambda: self.state() == xc.CLOUD_ERROR, 10, 'error')
        self.assertIn('Device lookup failed, retry', h.pub['/Cloud/Message'])
        h.tick_for(1)
        self.assertEqual(len(self.cloud.requests), 1)                        # retried after a minute, not each tick

    def test_demo_is_canned(self):
        self.reg.csv3['hardwarePresent'] = 0
        h = self.start()
        h.want('Cloud')
        h.until(lambda: self.state() == xc.CLOUD_READY and h.pub['/Cloud/Json'], 10, 'demo bundle')
        v = json.loads(h.pub['/Cloud/Json'])
        self.assertEqual(v['you']['n'], 'DemoBoat')
        self.assertEqual(v['pulse'], xc.cloud.view(xc.cloud.demo_bundle(0))['pulse'])
        self.assertRegex(v['pulse'], r'^47 boats · [0-9,]+ nm logged · [0-9,]+ kWh generated$')
        h.tick_for(1)
        self.assertEqual((self.cloud.requests, self.reg.token_requests), ([], 0))


# ------------------------------------------------------------------ D-Bus glue with stub modules

def install_stubs():
    dbus = types.ModuleType('dbus')
    service = types.ModuleType('dbus.service')
    exceptions = types.ModuleType('dbus.exceptions')
    mainloop = types.ModuleType('dbus.mainloop')
    glib = types.ModuleType('dbus.mainloop.glib')

    class Obj:
        def __init__(self, conn=None, path=None):
            self._locations = [(conn, path, None)]
            self.__dbus_object_path__ = path

        def remove_from_connection(self):
            self._locations = []

    service.Object = Obj
    service.method = lambda *a, **k: (lambda f: f)
    service.signal = lambda *a, **k: (lambda f: f)
    names = []
    service.BusName = lambda name, bus, do_not_queue=False: names.append(name) or types.SimpleNamespace(
        name=name, __del__=lambda: names.remove(name))

    def typ(base, name):
        def new(cls, v=base(), *a, **k):
            return base.__new__(cls, v)
        return type(name, (base,), {'__new__': new})
    for n in ('Int32', 'Int64', 'UInt32', 'Byte', 'Int16', 'UInt16', 'UInt64', 'Boolean'):
        setattr(dbus, n, typ(int, n))
    dbus.Double = typ(float, 'Double')
    dbus.String = typ(str, 'String')
    dbus.Signature = typ(str, 'Signature')
    dbus.ByteArray = typ(bytes, 'ByteArray')

    class Array(list):
        def __init__(self, v=(), signature=None, variant_level=0):
            super().__init__(v)

    class Dictionary(dict):
        def __init__(self, v=(), signature=None, variant_level=0):
            super().__init__(v)
    dbus.Array, dbus.Dictionary = Array, Dictionary

    class DBusException(Exception):
        pass
    exceptions.DBusException = DBusException
    dbus.exceptions = exceptions
    dbus.service = service

    class SettingsDevice:
        values = {}   # localsettings: path -> value

        def __init__(self, bus, supported, cb, timeout=0):
            self.cb, self.paths = cb, {k: opts[0] for k, opts in supported.items()}
            for k, opts in supported.items():
                SettingsDevice.values.setdefault(opts[0], opts[1])

        def __getitem__(self, k):
            return SettingsDevice.values[self.paths[k]]

        def __setitem__(self, k, v):
            old = SettingsDevice.values[self.paths[k]]
            SettingsDevice.values[self.paths[k]] = v
            self.cb(k, old, v)

    class Bus:
        closed = []
        others = {}   # the GX's other services: name -> {path: value}
        receivers = []
        settings = {'/Settings/Services/Bol': 0, '/Settings/Services/Modbus': 0}   # what the bridge reads: DVCC, Modbus TCP off

        def add_signal_receiver(self, handler, **kw):
            Bus.receivers.append((kw.get('signal_name'), handler))

        def __init__(self, private=False):
            self.private = private

        def list_names(self):
            return list(Bus.others) + list(names)

        def get_object(self, svc, path, introspect=False):
            if svc == 'com.victronenergy.settings':
                def remove(paths, dbus_interface=None):
                    for p in paths:
                        SettingsDevice.values.pop(p, None)

                def setting(dbus_interface=None, timeout=None):
                    if path not in Bus.settings:
                        raise DBusException('no such setting ' + path)
                    return Bus.settings[path]
                return types.SimpleNamespace(RemoveSettings=remove, GetValue=setting)

            def get(dbus_interface=None, timeout=None):
                if path not in Bus.others.get(svc, {}):
                    raise DBusException('no such object path ' + path)
                return Bus.others[svc][path]
            return types.SimpleNamespace(GetValue=get)

        def close(self):
            Bus.closed.append(self)
    dbus.SystemBus = Bus
    glib.DBusGMainLoop = lambda set_as_default=False: None
    mainloop.glib = glib
    dbus.mainloop = mainloop
    gi = types.ModuleType('gi')
    repo = types.ModuleType('gi.repository')
    timers = []
    repo.GLib = types.SimpleNamespace(timeout_add=lambda ms, fn: timers.append(fn), idle_add=lambda fn, *a: fn(*a),
                                      MainLoop=lambda: None)
    gi.repository = repo
    sd = types.ModuleType('settingsdevice')
    sd.SettingsDevice = SettingsDevice
    sys.modules.update({'dbus': dbus, 'dbus.service': service, 'dbus.exceptions': exceptions,
                        'dbus.mainloop': mainloop, 'dbus.mainloop.glib': glib, 'gi': gi, 'gi.repository': repo,
                        'settingsdevice': sd})
    return timers, SettingsDevice, names, Bus


class TestGlue(unittest.TestCase):
    def test_bridge_on_stub_dbus(self):
        timers, sd, names, bus = install_stubs()
        reg = fr.FakeReg(gx=3)
        cache = tempfile.mkdtemp(dir=WORK)
        group = '/Settings/Devices/xregulator_feedc0'
        real_run = xc.subprocess.run
        xc.subprocess.run = lambda *a, **k: types.SimpleNamespace(stdout='')   # never sweep this Mac's real LAN
        try:
            sd.values.update({'/Settings/Xregulator/Units': reg.uid, group + '/Host': reg.host})
            import importlib
            sys.path.insert(1, BRIDGE + '/ext/velib_python')
            br = importlib.import_module('xregulator_bridge')
            b = br.Bridge()
            core, svc, ubus = b.units[reg.uid]
            core.link.cache_dir = cache
            self.assertEqual(names, ['com.victronenergy.xregulator', 'com.victronenergy.switch.xregulator_feedc0'])
            self.assertTrue(ubus.private)                   # one connection per regulator service
            self.assertEqual(sd.values[group + '/ClassAndVrmInstance'], 'switch:100')
            tick = timers[0]

            def spin(pred, timeout=15):
                end = time.time() + timeout
                while time.time() < end and not pred():
                    tick()
                    time.sleep(0.1)
                tick()
                return pred()
            self.assertTrue(spin(lambda: svc['/Connected'] == 1))
            for _ in range(5):
                tick()
                time.sleep(0.1)
            # gx 3: the alternator service systemcalc controls, on its own connection, its own VRM instance
            mode, asvc, abus = b.alts[reg.uid]
            self.assertEqual(mode, 'shown')
            self.assertEqual(names, ['com.victronenergy.xregulator', 'com.victronenergy.switch.xregulator_feedc0',
                                     'com.victronenergy.alternator.xregulator_feedc0'])
            self.assertTrue(abus.private and abus is not ubus)
            self.assertEqual(sd.values[group + '_alt/ClassAndVrmInstance'], 'alternator:100')
            self.assertEqual((asvc['/ProductName'], asvc['/DeviceInstance'], asvc['/Serial']), ('Alternator', 100, reg.uid))
            self.assertEqual(asvc['/Settings/ChargeCurrentLimit'], 110.0)
            item = asvc._dbusobjects['/Link/ChargeCurrent']
            self.assertEqual(item.SetValue(37.5), 0)
            t1 = core.alt_writes['/Link/ChargeCurrent'][1]
            time.sleep(0.02)
            self.assertEqual(item.SetValue(37.5), 0)                       # unchanged: velib skips its callback
            self.assertGreater(core.alt_writes['/Link/ChargeCurrent'][1], t1)   # but the write is still stamped
            self.assertEqual(asvc['/Link/ChargeCurrent'], 37.5)
            self.assertTrue(spin(lambda: reg.limits and reg.limits[-1] == {'ccl': '37.5'}, 5))
            # the regulator's NMEA 2000 twin appears: the measurements go invalid on its NameOwnerChanged, before any scan,
            # then the rescans read its serial and rebuild ours with no measurements and no device-list name
            self.assertTrue(spin(lambda: asvc['/Dc/0/Current'] is not None, 5))
            owner = [f for n, f in bus.receivers if n == 'NameOwnerChanged'][-1]
            bus.others['com.victronenergy.alternator.socketcan_vecan0_vi0_uc1684700'] = {}   # named, /Serial not yet
            n = len(timers)
            owner('com.victronenergy.alternator.socketcan_vecan0_vi0_uc1684700', '', ':1.99')
            self.assertIsNone(asvc['/Dc/0/Current'])
            self.assertIsNone(asvc['/Dc/0/Power'])
            rescans = timers[n:]
            self.assertEqual(len(rescans), 3)
            rescans[0]()                                              # /Serial unreadable: stays quiet
            tick()
            self.assertIsNone(b.alts[reg.uid][1]['/Dc/0/Current'])
            bus.others['com.victronenergy.alternator.socketcan_vecan0_vi0_uc1684700'] = {'/Serial': reg.uid}
            rescans[1]()
            tick()
            mode, asvc, _ = b.alts[reg.uid]
            self.assertEqual(mode, 'hidden')
            self.assertNotIn('/ProductName', asvc)
            self.assertNotIn('/Dc/0/Current', asvc)
            self.assertIn('/Link/ChargeCurrent', asvc)
            # and gains /Link/ChargeCurrent (Victron's own control): ours goes, so DVCC sees one copy
            bus.others['com.victronenergy.alternator.socketcan_vecan0_vi0_uc1684700']['/Link/ChargeCurrent'] = None
            b._scan_alternators()
            tick()
            self.assertNotIn(reg.uid, b.alts)
            self.assertNotIn('com.victronenergy.alternator.xregulator_feedc0', names)
            bus.others.clear()
            b._scan_alternators()
            tick()
            self.assertEqual(b.alts[reg.uid][0], 'shown')
            # demo on a GX with a VE.Can port: no demo battery, ever (systemcalc would put its SOC on the bus)
            batt = 'com.victronenergy.battery.xregulator_feedc0'
            self.assertNotIn(batt, names)
            bus.others['com.victronenergy.vecan.vecan0'] = {}
            b._scan_alternators()
            reg.csv3['hardwarePresent'] = 0
            reg.csv3_dirty.set()
            self.assertTrue(spin(lambda: core.demo, 10))
            for _ in range(10):
                tick()
                time.sleep(0.05)
            self.assertNotIn(reg.uid, b.batts)
            self.assertNotIn(batt, names)
            # Modbus TCP on (it serves the system battery to whatever polls) also keeps it away
            del bus.others['com.victronenergy.vecan.vecan0']
            bus.settings['/Settings/Services/Modbus'] = 1
            b._scan_alternators()
            tick()
            self.assertNotIn(reg.uid, b.batts)
            # a GX with no CAN port, nothing that takes a battery reading, DVCC and Modbus TCP off: the demo battery
            bus.settings['/Settings/Services/Modbus'] = 0
            b._scan_alternators()
            self.assertTrue(spin(lambda: reg.uid in b.batts, 10))
            mode, bsvc, bbus = b.batts[reg.uid]
            self.assertIn(batt, names)
            self.assertTrue(bbus.private and bbus is not ubus)
            self.assertEqual(sd.values[group + '_batt/ClassAndVrmInstance'], 'battery:100')
            self.assertTrue(spin(lambda: bsvc['/Soc'] is not None, 5))
            self.assertEqual((bsvc['/Dc/0/Voltage'], bsvc['/Dc/0/Current'], bsvc['/Soc'], bsvc['/Dc/0/Temperature']),
                             (13.8, 50.0, 85.0, 25.0))   # flowBattA ahead of Bcur
            self.assertNotIn('/Info/MaxChargeVoltage', bsvc)   # never a BMS, so never DVCC's battery
            # a CAN port appearing (a VE.Can interface brought up) takes it away at once
            owner = [f for n, f in bus.receivers if n == 'NameOwnerChanged'][-1]
            bus.others['com.victronenergy.vecan.can1'] = {}
            owner('com.victronenergy.vecan.can1', '', ':1.121')
            self.assertNotIn(reg.uid, b.batts)
            bus.others.clear()
            b._scan_alternators()
            self.assertTrue(spin(lambda: reg.uid in b.batts, 5))
            # a solar charger arrives: the demo battery goes at once, before systemcalc can take it as the system battery
            owner = [f for n, f in bus.receivers if n == 'NameOwnerChanged'][-1]
            bus.others['com.victronenergy.solarcharger.ttyS3'] = {}
            owner('com.victronenergy.solarcharger.ttyS3', '', ':1.120')
            self.assertNotIn(reg.uid, b.batts)
            self.assertNotIn(batt, names)
            self.assertIn(bbus, bus.closed)
            b._scan_alternators()
            tick()
            self.assertNotIn(reg.uid, b.batts)
            # it leaves; DVCC on still keeps the battery away (shared voltage, current and temperature sense)
            bus.others.clear()
            Bus_settings = bus.settings
            Bus_settings['/Settings/Services/Bol'] = 1
            b._scan_alternators()
            tick()
            self.assertNotIn(reg.uid, b.batts)
            Bus_settings['/Settings/Services/Bol'] = 0
            b._scan_alternators()
            self.assertTrue(spin(lambda: reg.uid in b.batts, 5))
            # demo exit removes it
            reg.csv3['hardwarePresent'] = 1
            reg.csv3_dirty.set()
            self.assertTrue(spin(lambda: reg.uid not in b.batts, 10))
            self.assertNotIn(batt, names)
            self.assertEqual(svc['/ProductName'], 'XREG-010 alternator regulator')
            self.assertEqual((svc['/DeviceInstance'], svc['/Serial']), (100, reg.uid))
            self.assertIn('/Health/Live/latchPct', svc)        # added after connect from /altschema
            self.assertEqual(svc['/Health/Live/latchPct'], 93.0)
            self.assertEqual(svc['/Settings/Value/BulkVoltage'], 14.2)
            item = svc._dbusobjects['/Ui/Want/Short']
            ticked = []
            b._tick_one = (lambda orig: lambda t, s: (ticked.append(t), orig(t, s)))(b._tick_one)
            self.assertEqual(item.SetValue(int(time.time())), 0)          # accepted through Core.on_write
            self.assertEqual(ticked, [core])                          # a page opening ticks its unit at once
            self.assertFalse(core.want_rose)
            self.assertEqual(item.SetValue(int(time.time())), 0)          # the 15 s refresh of an open page does not
            self.assertEqual(ticked, [core])
            del b._tick_one
            self.assertEqual(svc._dbusobjects['/Ui/LongRange'].SetValue('2h'), 2)   # refused
            self.assertEqual(svc._dbusobjects['/Connected'].SetValue(0), 1)         # read-only
            self.assertEqual(svc._dbusobjects['/CustomName'].SetValue('Stbd'), 0)   # free
            units = json.loads(b.svc['/Units'])
            self.assertEqual([(u['uid'], u['service'], u['instance'], u['connected']) for u in units],
                             [(reg.uid, 'com.victronenergy.switch.xregulator_feedc0', 100, 1)])
            self.assertNotIn('/ProductName', b.svc)            # the manager is never a device in gui-v2's lists
            # the GX app's own update paths on the manager service: read-only state, writable triggers
            self.assertEqual((b.svc['/Update/Installed'], b.svc['/Update/InstalledBuild'], b.svc['/Update/State'],
                              b.svc['/Update/Last'] is not None), (xc.VERSION, 0, 0, True))   # the source tree: build 0
            self.assertEqual(b.svc._dbusobjects['/Update/State'].SetValue(6), 1)       # read-only
            self.assertEqual(b.svc._dbusobjects['/Update/Install'].SetValue('9.9'), 2)  # refused: nothing checked yet
            self.assertEqual(b.svc._dbusobjects['/Update/Remove'].SetValue(0), 2)       # refused: only 1 removes
            # remove through the manager's D-Bus item: service gone, connection closed, settings removed
            self.assertEqual(b.svc._dbusobjects['/Remove'].SetValue(reg.uid), 0)
            self.assertTrue(spin(lambda: not b.units))
            self.assertEqual(names, ['com.victronenergy.xregulator'])
            self.assertIn(ubus, bus.closed)
            self.assertIn(abus, bus.closed)
            self.assertNotIn(group + '/Host', sd.values)
            self.assertNotIn(group + '/ClassAndVrmInstance', sd.values)
            self.assertNotIn(group + '_alt/ClassAndVrmInstance', sd.values)
            self.assertNotIn(group + '_batt/ClassAndVrmInstance', sd.values)
            self.assertEqual(b.alts, {})
            self.assertEqual(sd.values['/Settings/Xregulator/Units'], '')
            self.assertEqual(json.loads(b.svc['/Units']), [])
            # and back: it is still on the found list
            self.assertEqual(b.svc._dbusobjects['/Add'].SetValue(reg.uid), 0)
            self.assertTrue(spin(lambda: reg.uid in b.units and b.units[reg.uid][1]['/Connected'] == 1))
            self.assertEqual(sd.values[group + '/Host'], reg.host)
            self.assertEqual(sd.values['/Settings/Xregulator/Units'], reg.uid)
            b.manager.stop()
        finally:
            xc.subprocess.run = real_run
            reg.close()
            shutil.rmtree(cache, ignore_errors=True)


if __name__ == '__main__':
    unittest.main(verbosity=2)
