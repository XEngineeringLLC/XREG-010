#!/usr/bin/env python3
# Boot-time confirm of a stick upgrade (0.3 -> 0.4) when the wall clock jumps forward during verify (NTP/GPS setting
# the time after a GX restored a stale one), and when the bridge needs longer than VERIFY_S to write its marker.
import os, sys, threading, time, json, zipfile
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import reviewenv
S = reviewenv.work()
sys.path.insert(0, os.path.join(S, 'gx_app', 'tools'))
import sim_install as sim

sim.SHIMS['date'] = r'''#!/bin/sh
off=$(cat "$SIM_ROOT/state/clock_offset" 2>/dev/null || echo 0)
if [ "$1" = +%s ]; then echo $(( $(/bin/date +%s) + off )); else exec /bin/date "$@"; fi
'''
sim.SHIMS['fake_bridge'] = sim.SHIMS['fake_bridge'].replace('if [ -f "$app/bridge/xreg_update.py" ]; then',
                                                            'sleep "${SIM_DELAY:-0}"\nif [ -f "$app/bridge/xreg_update.py" ]; then')
PKG = os.path.join(S, 'out', 'venus-data.zip')
T = os.path.join(S, 'clk'); os.makedirs(T, exist_ok=True)
p4 = sim.bump(PKG, '0.4', os.path.join(T, 'p4.zip'))


def scenario(name, delay, jump_after=None, verify_s=8):
    gx = sim.GX(verify_s=verify_s)
    try:
        gx.reboot(PKG)
        assert gx.running() == '0.3'
        gx.env['SIM_DELAY'] = str(delay)
        if jump_after is not None:
            def jump():
                time.sleep(jump_after)
                with open(os.path.join(gx.state, 'clock_offset'), 'w') as f:
                    f.write('86400')
            threading.Thread(target=jump, daemon=True).start()
        gx.reboot(p4)                 # stick with 0.4 in, confirmed at S99 by rc.local
        print('%-62s installed=%s running=%s result=%s failed_version=%r' % (
            name, gx.installed(), gx.running(delay + 3), gx.result(),
            (gx.read('xregulator', 'install', 'failed_version') or '').strip()))
    finally:
        gx.close()


scenario('stick 0.3->0.4, bridge marker after 2 s, no clock jump', 2)
scenario('stick 0.3->0.4, bridge marker after 5 s, clock +1 day at 2.5 s', 5, jump_after=2.5)
scenario('stick 0.3->0.4, bridge marker after VERIFY_S+2 s (slow GX)', 10)
