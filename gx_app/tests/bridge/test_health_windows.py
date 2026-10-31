#!/usr/bin/env python3
# Toggle lane 2026-10-29: the Health tab's window on two GX screens at once (the Ekrano's own screen and the remote
# console). Each screen asks for its window every 15 s, as XregPlotsHealth does, and must only ever see that window's
# trend. The fake regulator's /alttrend.csv rows start at 600 - hours for ?hours, at 1 for the whole history, so a trend
# says which window it is. Uses test_bridge.py's harness. Run: python3.12 -B test_health_windows.py -v
import os, sys, time, unittest

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import test_bridge as tb   # imported as a module, so its own TestCases do not run here
xc, fr, Harness = tb.xc, tb.fr, tb.Harness

_seq = [0]


def first_row(csv):
    lines = [l for l in (csv or '').strip().split('\n')[1:] if l]
    return float(lines[0].split(',')[0]) if lines else None


def window_of(csv):
    # which window a trend is: hours, or None while empty
    f = first_row(csv)
    return None if f is None else 0 if f == 1.0 else int(round(600 - f))


class Screen:
    # One GX screen on the Health tab at a window. The bridge before 2026-10-29 had one /Ui/HealthHours and one
    # /Health/TrendCsv for every screen; the bridge after has /Ui/Want/HealthTrend/<h> and /Health/TrendCsv/<h>.
    def __init__(self, h, hours):
        self.h, self.hours = h, hours
        self.shared = '/Ui/HealthHours' in h.pub.modes

    def ask(self):
        _seq[0] += 1
        stamp = int(time.time()) * 100 + _seq[0]   # a new value each time: velib drops an unchanged SetValue
        self.h.write('/Ui/Want/Health', stamp)
        if self.shared:
            self.h.write('/Ui/HealthHours', self.hours)
        else:
            self.h.write('/Ui/Want/HealthTrend/%d' % self.hours, stamp)

    def sees(self):
        return window_of(self.h.pub['/Health/TrendCsv' if self.shared else '/Health/TrendCsv/%d' % self.hours])


def trend_fetches(reg, hours):
    return [r for r in reg.requests('/alttrend.csv') if int(r['q'].get('hours') or 0) == hours]


class TestHealthWindows(unittest.TestCase):
    def setUp(self):
        self.reg = fr.FakeReg()
        self.h = Harness(self.reg)
        self.h.until(lambda: self.h.pub['/Connected'] == 1, 15, 'connected')

    def tearDown(self):
        self.h.close()
        self.reg.close()

    def test_two_screens_at_different_windows(self):
        # the bench case: one screen at 10 h, the other at All, each re-asking every 15 s (here every 0.3 s, 20 rounds)
        h = self.h
        a, b = Screen(h, 10), Screen(h, 0)
        a.ask()
        b.ask()
        h.until(lambda: a.sees() is not None and b.sees() is not None, 10, 'both trends')
        seen = []
        for _ in range(20):
            for s in (a, b):
                s.ask()
                h.tick_for(0.15)
                seen.append((a.sees(), b.sees()))
        wrong_a = sum(1 for x, _ in seen if x != 10)
        wrong_b = sum(1 for _, y in seen if y != 0)
        n10, nall = len(trend_fetches(self.reg, 10)), len(trend_fetches(self.reg, 0))
        print('two screens: %d samples; 10 h screen saw another window %d times, All screen %d; trend fetches 10 h %d, '
              'All %d' % (len(seen), wrong_a, wrong_b, n10, nall))
        self.assertEqual((wrong_a, wrong_b), (0, 0))
        self.assertEqual((n10, nall), (1, 1))   # each window fetched once, not once per ask of the other screen

    def test_window_change_never_shows_the_old_window(self):
        h = self.h
        a = Screen(h, 10)
        a.ask()
        h.until(lambda: a.sees() == 10, 10, '10 h trend')
        a.hours = 100
        a.ask()
        seen = []
        end = time.time() + 3
        while time.time() < end:
            h.tick()
            seen.append(a.sees())
            time.sleep(0.05)
        print('10 h -> 100 h: %d samples, windows seen %s' % (len(seen), sorted({str(x) for x in seen})))
        self.assertTrue(all(x in (None, 100) for x in seen))
        self.assertEqual(seen[-1], 100)

    def test_paths(self):
        modes = self.h.pub.modes
        self.assertNotIn('/Ui/HealthHours', modes)
        self.assertNotIn('/Health/TrendCsv', modes)
        for w in xc.HEALTH_WINDOWS:
            self.assertEqual(modes['/Ui/Want/HealthTrend/%d' % w], 'core')
            self.assertIn('/Health/TrendCsv/%d' % w, modes)
        self.assertFalse(self.h.core.on_write('/Ui/Want/HealthTrend/7', 1))   # not a window the plugin offers
        self.assertEqual(xc.HEALTH_WINDOWS, (10, 100, 0))

    def test_lapse_cache_and_refetch(self):
        h, core, pub = self.h, self.h.core, self.h.pub
        old = xc.HEALTH_REFETCH_S
        xc.HEALTH_REFETCH_S = 1
        try:
            a, b = Screen(h, 10), Screen(h, 100)
            a.ask()
            b.ask()
            h.until(lambda: a.sees() == 10 and b.sees() == 100, 10, 'both trends')
            # both still shown: each window refetched on its own every HEALTH_REFETCH_S
            n10, n100 = len(trend_fetches(self.reg, 10)), len(trend_fetches(self.reg, 100))
            h.tick_for(2.5)
            d10, d100 = len(trend_fetches(self.reg, 10)) - n10, len(trend_fetches(self.reg, 100)) - n100
            # the 10 h screen leaves: its path empties 45 s on, the 100 h one stays, and 10 h is no longer fetched
            core.trend_want_at[10] -= 60
            h.tick_for(0.3)
            gone = (pub['/Health/TrendCsv/10'], window_of(pub['/Health/TrendCsv/100']))
            n10 = len(trend_fetches(self.reg, 10))
            h.tick_for(2.5)
            stopped = len(trend_fetches(self.reg, 10)) - n10
            # it comes back: the cached trend shows at once, then a fresh fetch
            a.ask()
            h.tick()
            back = window_of(pub['/Health/TrendCsv/10'])
            h.until(lambda: len(trend_fetches(self.reg, 10)) > n10, 5, 'refetch on return')
            print('refetches in 2.5 s: 10 h %d, 100 h %d; 10 h left: path %r, 100 h shows %s, 10 h fetches after %d; '
                  'back: shows %s at once' % (d10, d100, gone[0], gone[1], stopped, back))
            self.assertGreaterEqual(min(d10, d100), 1)
            self.assertEqual(gone, ('', 100))
            self.assertEqual(stopped, 0)
            self.assertEqual(back, 10)
        finally:
            xc.HEALTH_REFETCH_S = old

    def test_reconnect_drops_cached_trends(self):
        h, core, pub = self.h, self.h.core, self.h.pub
        a = Screen(h, 0)
        a.ask()
        h.until(lambda: a.sees() == 0, 10, 'All trend')
        core._drop_history()
        self.assertEqual(core.health_trend, {w: '' for w in xc.HEALTH_WINDOWS})
        h.until(lambda: a.sees() == 0, 10, 'All trend again')


if __name__ == '__main__':
    unittest.main()
