#!/usr/bin/env python3
# The fast lane (/Fast/Short append-only Short Term, /Fast/Live) and the fast=2 hold: ShortRecorder directly, a page
# following its messages (short_client.py), a Core against fake_regulator.py, and the D-Bus glue's arrival hook.
# Run: python3 -B test_fastlane.py
import json, os, random, sys, tempfile, threading, time, types, unittest

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
BRIDGE = os.environ.get('XREG_BRIDGE', os.path.dirname(os.path.dirname(os.path.dirname(HERE))) + '/gx_app/xregulator/bridge')
sys.path.insert(0, BRIDGE)
sys.path.insert(0, HERE)
import xreg_core as xc
import fake_regulator as fr
from short_client import ShortClient, recorder_rows
from test_bridge import Harness, Pub, WORK, install_stubs

NSER = len(xc.SHORT) + 1


def frame(sms, amps=50.0, sess=7, **kw):
    f = {'sendMs': sms, 'sessionId': sess, 'MeasuredAmps': amps * 100, 'Bcur': 2500, 'iiout': 350, 'dutyCycle': 4500,
         'pidAltPV': 4900, 'BatteryV': 1385, 'IBV': 1380, 'RPM': 1800, 'AlternatorTemperatureF': 15000}
    f.update(kw)
    return f


def parse(msg):
    p = msg.split(';')
    k = int(p[3])
    return {'epoch': p[1], 'n': int(p[2]), 'k': k, 'full': p[4] == '1',
            'dt': [int(x) for x in p[5].split(',')] if k else [],
            'cols': [p[6 + j].split(',') if k else [] for j in range(NSER)]}


class TestRecorder(unittest.TestCase):
    def test_times_from_send_ms_numbers_and_messages(self):
        r = xc.ShortRecorder()
        self.assertEqual(r.message(None), 's1;%s;0;0;1;' % r.epoch + ';' * NSER)
        at = 100.0
        for i, sms in enumerate((1000, 1107, 1201, 1310)):
            r.frame(at + i * 0.3, 1, frame(sms, amps=50 + i), 1)   # arrival jitter does not matter: sendMs does
        m = parse(r.message(None))
        self.assertEqual((m['n'], m['k'], m['full'], m['dt']), (4, 4, True, [0, 107, 94, 109]))
        self.assertEqual(m['cols'][0], ['50.0', '51.0', '52.0', '53.0'])
        self.assertEqual(m['cols'][8], ['65.6'] * 4)                # F to C, 1 decimal
        self.assertEqual(m['cols'][9], ['1'] * 4)
        m = parse(r.message(2))
        self.assertEqual((m['n'], m['k'], m['full'], m['dt'], m['cols'][0]), (4, 2, False, [94, 109], ['52.0', '53.0']))
        self.assertEqual(parse(r.message(4))['k'], 0)
        self.assertTrue(parse(r.message(9))['full'])               # a number this recorder never had: everything
        r.frame(at + 2, 1, frame(1310), 1)                          # a sendMs that does not move: the bridge's clock
        self.assertEqual(parse(r.message(4))['dt'], [1100])

    def test_empty_values_shutdown_stage_and_no_stage(self):
        r = xc.ShortRecorder()
        r.frame(1.0, 1, frame(10, Bcur=xc.EMPTY - 1), None)
        m = parse(r.message(None))
        self.assertEqual((m['cols'][1], m['cols'][9]), ([''], ['']))

    def test_gap_while_stale_then_resume(self):
        r = xc.ShortRecorder()
        r.gap(0.0)
        self.assertEqual(r.n, 0)                                    # nothing before the first sample
        r.frame(10.0, 1, frame(5000), 1)
        r.gap(10.2)                                                 # stale: one empty sample at once
        r.gap(10.7)                                                 # within SHORT_GAP_EVERY_S: none
        r.gap(11.25)
        m = parse(r.message(None))
        self.assertEqual(m['dt'], [0, 200, 1050])
        self.assertEqual([c[1:] for c in m['cols']], [['', '']] * NSER)
        r.frame(12.0, 1, frame(7000), 1)                            # same boot, same stream: sendMs places it
        self.assertEqual(parse(r.message(3))['dt'], [750])          # 2000 ms after the frame, 750 after the last gap

    def test_reboot_and_reconnect_use_the_bridge_clock(self):
        r = xc.ShortRecorder()
        r.frame(10.0, 1, frame(900000), 1)
        r.frame(10.5, 1, frame(900500), 1)
        r.frame(14.0, 1, frame(1200, sess=8), 1)                    # rebooted: sendMs restarted
        r.frame(14.1, 1, frame(1300, sess=8), 1)
        r.frame(20.0, 2, frame(7300, sess=8), 1)                    # a new stream: the bridge's 5.9 s
        self.assertEqual(parse(r.message(None))['dt'], [0, 500, 3500, 100, 5900])

    def test_millis_wrap_and_rate_changes(self):
        r = xc.ShortRecorder()
        sms, at = 2 ** 32 - 1500, 0.0
        for iv in (1000, 1000, 500, 500, 100, 100, 100, 1000):
            r.frame(at, 1, frame(sms % 2 ** 32), 1)
            sms += iv
            at += iv / 1000.0
        self.assertEqual(parse(r.message(None))['dt'], [0, 1000, 1000, 500, 500, 100, 100, 100])

    def test_older_firmware_without_send_ms(self):
        r = xc.ShortRecorder()
        for i in range(3):
            f = frame(0)
            del f['sendMs'], f['sessionId']
            r.frame(5.0 + i * 0.5, 1, f, 1)
        self.assertEqual(parse(r.message(None))['dt'], [0, 500, 500])

    def test_keeps_the_window_plus_one(self):
        r = xc.ShortRecorder()
        for i in range(3500):
            r.frame(i * 0.1, 1, frame(i * 100), 1)
        self.assertEqual(r.n, 3500)
        self.assertEqual(len(r.t), 3001)                            # 300 s at 100 ms, and the sample the line enters from
        self.assertLessEqual(r.t[0], r.t[-1] - xc.SHORT_S * 1000)
        self.assertGreater(r.t[1], r.t[-1] - xc.SHORT_S * 1000)
        m = parse(r.message(10))                                    # sample 11 is long gone: everything instead
        self.assertEqual((m['full'], m['k'], m['dt'][0]), (True, 3001, 0))
        m = parse(r.message(3500 - 3001))                           # the page holds the one before the oldest
        self.assertEqual((m['full'], m['k']), (False, 3001))

    def test_fill_from_backfill(self):
        r = xc.ShortRecorder()
        e0 = r.epoch
        rows = [{'MeasuredAmps': 40.0 + i / 100, 'AlternatorTemperatureF': 212.0, 'chargeStage': 1} for i in range(300)]
        rows[5]['MeasuredAmps'] = None                              # a reading a row lacks holds the one before
        r.fill(rows, 50.0)
        m = parse(r.message(None))
        self.assertNotEqual(r.epoch, e0)
        self.assertEqual((m['n'], m['dt'][:3], set(m['dt'][1:])), (300, [0, 1000, 1000], {1000}))
        self.assertEqual((m['cols'][0][4], m['cols'][0][5], m['cols'][8][0], m['cols'][1][0]), ('40.0', '40.0', '100.0', ''))
        r.frame(50.4, 1, frame(123), 1)                             # live frames carry on from now
        self.assertEqual(parse(r.message(300))['dt'], [400])

    def test_a_page_following_the_messages_always_holds_the_window(self):
        # Random frames at 1000 / 500 / 100 ms, stale spells, reboots, reconnects, publishes at random points, missed
        # messages, page restarts (remote console reconnect) and full requests: after every message the page holds
        # exactly what the recorder holds.
        rnd = random.Random(5)
        checked = [0]
        for trial in range(40):
            r = xc.ShortRecorder()
            pages = [ShortClient(), ShortClient()]
            since, full = None, False
            at, sms, sess, gen, iv = 0.0, rnd.randrange(2 ** 32), 1, 1, 0.5
            for step in range(1500):
                ev = rnd.random()
                if ev < 0.01:
                    iv = rnd.choice((0.1, 0.5, 1.0))
                elif ev < 0.012:
                    sess += 1
                    sms = rnd.randrange(100000)
                elif ev < 0.014:
                    gen += 1
                if rnd.random() < 0.02:
                    for _ in range(rnd.randrange(1, 12)):            # stale spell
                        at += 0.5
                        r.gap(at)
                at += iv + rnd.uniform(-0.03, 0.03)
                sms = (sms + int(iv * 1000) + rnd.randrange(-5, 30)) % 2 ** 32
                r.frame(at, gen, frame(sms, amps=rnd.uniform(0, 100)), rnd.choice((1, 2, None)))
                if rnd.random() < 0.6:                              # a publish (the fast lane is rate-capped)
                    msg = r.message(None if full else since)
                    since, full = r.n, False
                    for pg in pages:
                        if rnd.random() < 0.03:
                            continue                                # this page missed it (or was hidden)
                        if pg.take(msg) == 'gap':
                            full = True                             # its /Fast/ShortFull write
                    if rnd.random() < 0.005:
                        pages[1] = ShortClient()                    # the remote console reconnected
                    for pg in pages:
                        if pg.epoch == r.epoch and pg.n == r.n:
                            self.assertEqual(len(pg.t), len(r.t), 'trial %d step %d' % (trial, step))
                            self.assertEqual(pg.rows()[1:], recorder_rows(r)[1:], 'trial %d step %d' % (trial, step))
                            checked[0] += 1
                if rnd.random() < 0.002:
                    r = xc.ShortRecorder()                          # a demo switch / bridge restart: a new epoch
                    since = None if rnd.random() < 0.5 else 0
        self.assertGreater(checked[0], 20000)


class TestCoreFastLane(unittest.TestCase):
    def setUp(self):
        self.reg = fr.FakeReg()
        self.reg.clock_fields = True
        self.reg.live_ms = 100
        self.h = Harness(self.reg)
        self.h.follow_short = False
        self.wakes = 0

        def wake():
            self.wakes += 1
        self.h.core.wake = wake

    def tearDown(self):
        self.h.close()
        self.reg.close()

    def fast_ticks(self, seconds):
        # what xregulator_bridge.py does between its 500 ms ticks: a fast tick per wake, at most every FAST_MIN_S
        end = time.time() + seconds
        nxt = 0.0
        while time.time() < end:
            if time.time() >= nxt:
                self.h.tick()
                nxt = time.time() + 0.5
            if self.wakes:
                self.wakes = 0
                self.h.core.fast_tick(self.h.pub)
            time.sleep(xc.FAST_MIN_S)

    def test_silent_with_no_page(self):
        h, pub = self.h, self.h.pub
        h.until(lambda: pub['/Connected'] == 1, 15, 'connected')
        h.tick_for(2.0)
        self.assertEqual(self.wakes, 0)                             # no page: the stream thread wakes nothing
        self.assertEqual((pub['/Fast/Short'], pub['/Fast/Live']), ('', ''))
        self.assertGreaterEqual(h.core.short.n, 4)                  # Short Term keeps recording (CSV1 every 500 ms here)
        self.assertEqual(self.reg.fast, [])

    def test_short_page_first_ask_increments_rate_and_release(self):
        h, pub = self.h, self.h.pub
        h.until(lambda: pub['/Connected'] == 1, 15, 'connected')
        h.tick_for(1.5)
        held = h.core.short.n
        page = ShortClient()
        seen = []
        orig = Pub.__setitem__

        def spy(p, path, value):
            if p is pub and path == '/Fast/Short' and value != p.get(path):
                seen.append(value)
            orig(p, path, value)
        Pub.__setitem__ = spy
        try:
            h.want('Short')
            h.tick()
            self.assertTrue(seen and parse(seen[0])['full'])        # the first ask: everything held, at once
            self.assertGreaterEqual(parse(seen[0])['k'], held)
            h.until(lambda: '2' in self.reg.fast, 5, 'fast=2')
            h.until(lambda: (h.core.link.snapshot()[2]['rate'] or {}).get('csv1') == 100, 5, 'GxRate csv1 100')
            self.fast_ticks(2.0)
            for m in seen:
                self.assertNotEqual(page.take(m), 'gap')
            self.assertEqual(page.n, h.core.short.n)
            self.assertEqual(page.rows()[1:], recorder_rows(h.core.short)[1:])
            incs = [parse(m) for m in seen[1:]]
            self.assertTrue(incs and not any(m['full'] for m in incs))
            self.assertLessEqual(max(m['k'] for m in incs), 3)       # newest samples only
            fast = [d for m in incs for d in m['dt']][-8:]
            self.assertTrue(all(90 <= d <= 160 for d in fast), fast)  # CSV1 at the web app's 100 ms
            # a page that missed messages asks for the whole history
            n0 = len(seen)
            self.assertTrue(h.write('/Fast/ShortFull', 1))
            h.tick()
            self.assertTrue(parse(seen[n0])['full'])
            self.assertIsNone(pub['/Fast/ShortFull'])
            # the page goes: the lane empties, the hold is released
            h.core.want_at['Short'] -= 60
            h.tick()
            self.assertEqual(pub['/Fast/Short'], '')
            h.until(lambda: self.reg.fast[-1] == '0', 5, 'fast=0')
            w = self.wakes = 0
            h.tick_for(1.0)
            self.assertEqual(self.wakes, w)
        finally:
            Pub.__setitem__ = orig

    def test_older_firmware_at_its_slower_rate(self):
        self.reg.live_ms = None                                     # reads fast=2 as 1: CSV1 every 500 ms
        h, pub = self.h, self.h.pub
        h.until(lambda: pub['/Connected'] == 1, 15, 'connected')
        h.want('Short')
        h.until(lambda: '2' in self.reg.fast, 5, 'fast=2 asked')
        n0 = h.core.short.n
        self.fast_ticks(2.5)
        dts = parse(h.core.short.message(n0))['dt']
        self.assertTrue(dts and all(450 <= d <= 600 for d in dts), dts)
        self.assertEqual((h.core.link.snapshot()[2]['rate'] or {}).get('csv1'), 500)

    def test_regulator_reboot_and_bridge_restart(self):
        h, pub = self.h, self.h.pub
        h.until(lambda: pub['/Connected'] == 1, 15, 'connected')
        h.want('Short')
        h.tick()
        page = ShortClient()
        self.assertEqual(page.take(pub['/Fast/Short']), 'full')
        self.fast_ticks(1.0)
        self.reg.session, self.reg.started = 99, time.time()       # rebooted: sessionId and sendMs restart
        self.reg.stream_gen += 1                                    # its stream drops and comes back
        h.until(lambda: h.core.link.snapshot()[2]['generation'] >= 2, 15, 'reconnected')
        self.fast_ticks(1.5)
        r = page.take(pub['/Fast/Short'])
        self.assertIn(r, ('inc', 'tail', 'old', 'gap'))
        if r == 'gap':                                              # it missed the messages between: asks, gets all
            self.assertTrue(h.write('/Fast/ShortFull', 1))
            h.tick()
            page.take(pub['/Fast/Short'])
        self.assertEqual(page.epoch, h.core.short.epoch)            # append-only across a reboot: same history
        rr = recorder_rows(h.core.short)
        self.assertEqual(page.rows()[1:], rr[len(rr) - len(page.t) + 1:])
        # a bridge restart: a new recorder, a new epoch; the page starts over from its first message
        h2 = Harness(self.reg, store=h.store, unit_stores=h.unit_stores)
        h2.follow_short = False
        try:
            h2.until(lambda: h2.pub['/Connected'] == 1, 15, 'second bridge connected')
            h2.want('Short')
            h2.tick()
            self.assertNotEqual(h2.core.short.epoch, page.epoch)
            self.assertEqual(page.take(h2.pub['/Fast/Short']), 'full')
            self.assertEqual(page.epoch, h2.core.short.epoch)
        finally:
            h2.close()

    def test_rate_changes_between_1000_500_and_100(self):
        h, pub = self.h, self.h.pub
        self.reg.gx_csv1_s = 1.0
        h.until(lambda: pub['/Connected'] == 1, 15, 'connected')
        h.tick_for(3.2)
        n1 = h.core.short.n
        self.reg.gx_csv1_s = 0.5
        h.tick_for(2.2)
        n2 = h.core.short.n
        h.want('Short')
        h.until(lambda: (h.core.link.snapshot()[2]['rate'] or {}).get('csv1') == 100, 5, 'csv1 100')
        self.fast_ticks(1.5)
        dts = parse(h.core.short.message(None))['dt']
        self.assertTrue(all(950 <= d <= 1100 for d in dts[2:n1]), dts[:n1])
        self.assertTrue(all(450 <= d <= 600 for d in dts[n1 + 1:n2]), dts[n1:n2])
        self.assertTrue(all(90 <= d <= 160 for d in dts[-5:]), dts[-5:])
        h.core.want_at['Short'] -= 60
        h.until(lambda: self.reg.fast[-1] == '0', 5, 'fast=0')
        h.until(lambda: (h.core.link.snapshot()[2]['rate'] or {}).get('csv1') == 500, 5, 'back to 500')

    def test_live_lane_and_its_hold(self):
        h, pub = self.h, self.h.pub
        h.until(lambda: pub['/Connected'] == 1, 15, 'connected')
        h.want('Live')
        h.tick()
        live = json.loads(pub['/Fast/Live'])
        self.assertEqual(set(live), set(xc.FAST_LIVE) | {'OnOff'})
        for name in xc.FAST_LIVE:
            self.assertEqual(live[name], pub['/Live/' + name], name)
        self.assertEqual(live['OnOff'], pub['/SwitchableOutput/0/State'])
        h.until(lambda: '2' in self.reg.fast, 5, 'fast=2 while the engine turns')
        # a write: the lane shows the written value until the regulator's echo, as the switch does
        self.reg.confirm_delay_s = 1.5
        self.assertTrue(h.write('/SwitchableOutput/0/State', 0))
        h.until(lambda: json.loads(pub['/Fast/Live'])['OnOff'] == 0, 3, 'held at the written value')
        self.fast_ticks(0.5)
        self.assertEqual(json.loads(pub['/Fast/Live'])['OnOff'], 0)
        # engine stopped, field off: the Dashboard no longer holds the fast schedule (it shows at anchor all day)
        self.reg.confirm_delay_s = None
        self.reg.field_status = 0
        fr_frame = self.reg.frame

        def stopped(ev, t):
            s = fr_frame(ev, t)
            if ev != 'CSVData':
                return s
            names = fr.FIELDS['CSVData']
            v = s.split(',')
            v[1 + names.index('RPM')] = '0'
            return ','.join(v)
        self.reg.frame = stopped
        h.until(lambda: self.reg.fast[-1] == '0', 5, 'released at anchor')
        self.assertEqual(json.loads(pub['/Fast/Live'])['Rpm'], 0.0)   # the lane itself stays while the page is open
        h.core.want_at['Live'] -= 60
        h.tick()
        self.assertEqual(pub['/Fast/Live'], '')

    def test_live_readouts_twice_a_second_state_at_once(self):
        h, pub = self.h, self.h.pub
        h.until(lambda: pub['/Connected'] == 1, 15, 'connected')
        sent = []
        orig = Pub.__setitem__

        def spy(p, path, value):
            if p is pub and path == '/Fast/Live' and value:
                sent.append((time.time(), json.loads(value)))
            orig(p, path, value)
        Pub.__setitem__ = spy
        try:
            h.want('Live')
            h.tick()
            self.assertEqual(len(sent), 1)                                    # the first reading at once
            h.until(lambda: (h.core.link.snapshot()[2]['rate'] or {}).get('csv1') == 100, 5, 'GxRate csv1 100')
            del sent[:]
            self.fast_ticks(3.0)
            gaps = [round(b[0] - a[0], 3) for a, b in zip(sent, sent[1:])]
            self.assertTrue(5 <= len(sent) <= 7, (len(sent), gaps))          # frames 100 ms apart: 30 without the rule
            self.assertTrue(all(g >= 0.4 for g in gaps), gaps)
            # a switch write: the held value goes out with the next frame, not at the next half second
            self.reg.confirm_delay_s = 1.5
            t0 = time.time()
            self.assertTrue(h.write('/SwitchableOutput/0/State', 0))
            self.fast_ticks(0.4)
            first = next((t for t, v in sent if t >= t0 and v['OnOff'] == 0), None)
            self.assertIsNotNone(first)
            self.assertLess(first - t0, 0.25)
        finally:
            Pub.__setitem__ = orig


class TestGlueArrival(unittest.TestCase):
    def test_arrival_ticks_the_lane_rate_capped_and_only_when_wanted(self):
        timers, sd, names, bus = install_stubs()
        import gi.repository
        idle, later = [], []
        gi.repository.GLib.idle_add = lambda fn, *a: idle.append((fn, a))
        gi.repository.GLib.timeout_add = lambda ms, fn, *a: (later.append((ms, fn, a)) if a else timers.append(fn))
        reg = fr.FakeReg(gx=3)
        reg.live_ms, reg.clock_fields = 100, True
        cache = tempfile.mkdtemp(dir=WORK)
        group = '/Settings/Devices/xregulator_feedc0'
        real_run = xc.subprocess.run
        xc.subprocess.run = lambda *a, **k: types.SimpleNamespace(stdout='')
        try:
            sd.values.update({'/Settings/Xregulator/Units': reg.uid, group + '/Host': reg.host})
            import importlib
            sys.path.insert(1, BRIDGE + '/ext/velib_python')
            br = importlib.import_module('xregulator_bridge')
            b = br.Bridge()
            core, svc, ubus = b.units[reg.uid]
            core.link.cache_dir = cache
            tick = timers[0]
            end = time.time() + 15
            while time.time() < end and svc['/Connected'] != 1:
                tick()
                time.sleep(0.1)
            self.assertEqual(svc['/Connected'], 1)
            idle.clear()
            time.sleep(1.0)
            self.assertEqual(idle, [])                              # no page: frames schedule nothing
            b._on_write(core, '/Ui/Want/Short', 1)
            for fn, a in list(idle):                                # the open-tick
                fn(*a)
            idle.clear()
            self.assertTrue(svc['/Fast/Short'].startswith('s1;'))
            self.assertEqual(svc._dbusobjects['/Fast/Short'].GetText(), '')    # no second copy as Text
            self.assertNotEqual(svc._dbusobjects['/Live/BattV'].GetText(), '')
            n0 = core.short.n
            runs = 0
            end = time.time() + 2.0
            while time.time() < end:
                time.sleep(0.02)
                pend = list(idle) + [(fn, a) for _ms, fn, a in later]
                idle.clear()
                later.clear()
                self.assertLessEqual(len(pend), 1)                  # one fast tick pending at a time
                for fn, a in pend:
                    fn(*a)
                    runs += 1
            self.assertGreater(core.short.n, n0 + 8)
            self.assertEqual(int(svc['/Fast/Short'].split(';')[2]), core.short.n)   # every sample published
            self.assertLess(runs, 2.0 / xc.FAST_MIN_S + 6)
        finally:
            xc.subprocess.run = real_run
            reg.close()


if __name__ == '__main__':
    unittest.main(verbosity=2)
