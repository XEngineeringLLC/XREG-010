#!/usr/bin/env python3
# A stick upgrade (0.3 -> 0.4) whose new bridge needs 12 s to write its marker (a GX still starting its services), with
# the push/in-app window 8 s and the boot window 20 s: unpacked at S30 (before svscan), or by update-data.sh's delayed
# path (a stick that mounts late, unpacked after svscan has started).
import os, sys, time
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import reviewenv
S = reviewenv.work()
sys.path.insert(0, os.path.join(S, 'gx_app', 'tools'))
import sim_install as sim

sim.SHIMS['fake_bridge'] = sim.SHIMS['fake_bridge'].replace('if [ -f "$app/bridge/xreg_update.py" ]; then',
                                                            'sleep "${SIM_DELAY:-0}"\nif [ -f "$app/bridge/xreg_update.py" ]; then')
PKG = os.path.join(S, 'out', 'venus-data.zip')
T = os.path.join(S, 'dl'); os.makedirs(T, exist_ok=True)
p4 = sim.bump(PKG, '0.4', os.path.join(T, 'p4.zip'))

for delayed in (False, True):
    gx = sim.GX(verify_s=8, boot_verify_s=20)
    try:
        gx.reboot(PKG)
        gx.env['SIM_DELAY'] = '12'
        out = gx.reboot(p4, delayed=delayed)
        time.sleep(1)
        print('%-28s %s: installed=%s running=%s result=%s failed_version=%r' % (
            'delayed stick' if delayed else 'stick at S30', out, gx.installed(), gx.running(15),
            gx.result() and gx.result()[:3] + (gx.result()[3][:70],),
            (gx.read('xregulator', 'install', 'failed_version') or '').strip()))
        gx.env['SIM_DELAY'] = '0'
        if delayed:
            print('   same stick again at the next boot:', gx.reboot(p4), gx.installed())
    finally:
        gx.close()
