#!/usr/bin/env python3
# Toggle lane 2026-10-29, bench round 8: after a GX write the bridge showed the written value for 3 s, then the frame
# from before the write until the regulator's echo came (up to 4.6 s on the bench, where CSV1 goes every 5 s with the
# engine stopped). The fake regulator here runs that schedule (CSV1 every 5 s, announced in GxRate) and sends the
# confirming CSV1 exactly N s after a write. Each case samples the D-Bus value every 50 ms from the write on.
# Uses test_bridge.py's harness. Run: python3.12 -B test_switch_hold.py -v
import os, sys, time, unittest

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import test_bridge as tb   # imported as a module, so its own TestCases do not run here
xc, fr, Harness = tb.xc, tb.fr, tb.Harness

STATE = '/SwitchableOutput/0/State'


class Base(unittest.TestCase):
    def start(self, on_off=1, **params):
        self.reg = fr.FakeReg()
        self.reg.on_off = on_off
        self.reg.clock_fields = True
        self.reg.gx_csv1_s = 5.0
        self.reg.csv3_every_s = 120.0
        for k, v in params.items():
            setattr(self.reg, k, v)
        self.h = Harness(self.reg)
        self.h.until(lambda: self.h.pub['/Connected'] == 1 and self.h.pub[STATE] == on_off, 20, 'connected')

    def tearDown(self):
        self.h.close()
        self.reg.close()

    def watch(self, path, seconds):
        # (seconds since the write, value) every 50 ms
        out, t0 = [], time.time()
        while time.time() - t0 < seconds:
            self.h.tick()
            out.append((time.time() - t0, self.h.pub[path]))
            time.sleep(0.05)
        return out

    @staticmethod
    def spans(samples, value):
        # the stretches (from, to) in which the value shown was `value`
        out, start = [], None
        for t, v in samples:
            if v == value and start is None:
                start = t
            elif v != value and start is not None:
                out.append((round(start, 2), round(t, 2)))
                start = None
        if start is not None:
            out.append((round(start, 2), round(samples[-1][0], 2)))
        return out


class TestOnOffConfirmation(Base):
    def flip(self, delay, to):
        self.start(on_off=1 - to, confirm_delay_s=delay)
        h = self.h
        t_write = time.time()
        self.assertTrue(h.write(STATE, to))
        samples = self.watch(STATE, delay + 2.5)
        live = [(t, v) for t, v in self.watch('/Live/OnOff', 0.2)]
        old = self.spans(samples, 1 - to)
        res = h.result()
        n_echo = next((t for t, v in samples if v == to and t > delay - 0.3), None)
        print('confirmation %.1f s, %s -> %s: result ok %s; old state shown again %s; /Live/OnOff now %s'
              % (delay, 1 - to, to, res and res['ok'], old or 'never', live[-1][1]), flush=True)
        self.assertTrue(res and res['ok'])
        self.assertEqual(old, [], 'the old state showed again after a successful write')
        self.assertEqual(live[-1][1], float(to))
        return t_write, n_echo

    def test_off_after_0_5_s(self):
        self.flip(0.5, 0)

    def test_on_after_0_5_s(self):
        self.flip(0.5, 1)

    def test_off_after_3_s(self):
        self.flip(3.0, 0)

    def test_on_after_3_s(self):
        self.flip(3.0, 1)

    def test_off_after_4_6_s(self):
        self.flip(4.6, 0)

    def test_on_after_4_6_s(self):
        self.flip(4.6, 1)

    def test_off_after_6_s(self):
        self.flip(6.0, 0)

    def test_on_after_6_s(self):
        self.flip(6.0, 1)

    def test_csv3_echo_confirms_before_csv1(self):
        # the firmware's settingsDirty echo: CSV3 carries the new OnOff at once, CSV1 only 4.6 s on
        self.start(on_off=1, confirm_delay_s=4.6, onoff_csv3_echo=True)
        h = self.h
        self.assertTrue(h.write(STATE, 0))
        live = self.watch('/Live/OnOff', 1.5)
        first = next((round(t, 2) for t, v in live if v == 0.0), None)
        samples = self.watch(STATE, 4.0)
        print('CSV3 echo at once, CSV1 at 4.6 s: /Live/OnOff 0 from %s s; old state shown again %s'
              % (first, self.spans(samples, 1) or 'never'), flush=True)
        self.assertIsNotNone(first)
        self.assertLess(first, 1.0)
        self.assertEqual(self.spans(samples, 1), [])


class TestOnOffRefusedOrUnconfirmed(Base):
    def test_refusal_returns_at_once(self):
        self.start(on_off=0, refuse_onoff='Rejected: test refusal')
        h = self.h
        self.assertTrue(h.write(STATE, 1))
        samples = self.watch(STATE, 2.0)
        res = h.result()
        back = next((round(t, 2) for t, v in samples if v == 0), None)
        print('refused (ok %s, %r): real state 0 again at %s s' % (res and res['ok'], res and res['message'], back),
              flush=True)
        self.assertFalse(res['ok'])
        self.assertIsNotNone(back)
        self.assertLess(back, 1.0)
        self.assertTrue(all(v == 0 for t, v in samples if t > back))

    def test_unconfirmed_returns_after_the_window(self):
        # answered 200 with an empty body (an ignored copy) and never applied: the frames keep showing 0
        self.start(on_off=0, ignore_onoff=True)
        h = self.h
        self.assertTrue(h.write(STATE, 1))
        samples = self.watch(STATE, 10.0)
        window = 5.0 + xc.CONFIRM_MARGIN_S if hasattr(xc, 'CONFIRM_MARGIN_S') else None
        held = self.spans(samples, 1)
        back = next((round(t, 2) for t, v in samples if v == 0 and t > 0.2), None)
        print('accepted, never confirmed (CSV1 every 5 s, window %s s): written state shown %s, real state from %s s'
              % (window, held, back), flush=True)
        self.assertIsNotNone(back)
        self.assertGreater(back, 7.0)    # held through the measured worst case (4.6 s) with margin
        self.assertLess(back, 8.8)       # and back promptly once the window has passed
        self.assertTrue(all(v == 0 for t, v in samples if t > back))


class TestSettingConfirmation(Base):
    KEY = '/Settings/Value/BulkVoltage'

    def test_slow_echo_never_shows_the_old_value(self):
        self.start(csv3_echo_delay_s=4.6)
        h = self.h
        h.until(lambda: h.pub[self.KEY] == 14.2, 10, 'setting echoed')
        self.assertTrue(h.write(self.KEY, 14.3))
        samples = self.watch(self.KEY, 7.0)
        old = self.spans(samples, 14.2)
        print('setting, CSV3 echo 4.6 s after the write: old value shown again %s' % (old or 'never'), flush=True)
        self.assertTrue(h.result()['ok'])
        self.assertEqual(old, [])
        self.assertEqual(samples[-1][1], 14.3)

    def test_lost_echo_waits_for_the_next_csv3(self):
        # the write's CSV3 echo never comes; the next periodic CSV3 (10 s on) carries the value
        self.start(csv3_echo_delay_s=100.0, csv3_every_s=10.0)
        h = self.h
        h.until(lambda: h.pub[self.KEY] == 14.2, 15, 'setting echoed')
        self.assertTrue(h.write(self.KEY, 14.3))
        samples = self.watch(self.KEY, 12.0)
        old = self.spans(samples, 14.2)
        print('setting, its CSV3 echo lost: old value shown again %s' % (old or 'never'), flush=True)
        self.assertEqual(old, [])

    def test_refused_setting_returns_at_once(self):
        self.start()
        h = self.h
        h.until(lambda: h.pub[self.KEY] == 14.2, 10, 'setting echoed')
        self.reg.unknown_keys.add('BulkVoltage')
        self.assertTrue(h.write(self.KEY, 14.3))
        samples = self.watch(self.KEY, 2.0)
        back = next((round(t, 2) for t, v in samples if v == 14.2), None)
        print('setting refused (%r): old value again at %s s' % (h.result()['message'], back), flush=True)
        self.assertFalse(h.result()['ok'])
        self.assertIsNotNone(back)
        self.assertLess(back, 1.0)


if __name__ == '__main__':
    unittest.main()
