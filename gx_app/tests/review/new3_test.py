#!/usr/bin/env python3
# Third-round checks: the lock-free confirm wait, the first-install removal, the rc.local link handling.
import os, sys, time, threading, shutil
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import reviewenv
S = reviewenv.work()
sys.path.insert(0, os.path.join(S, 'gx_app', 'tools'))
import sim_install as sim

PKG = os.path.join(S, 'out', 'venus-data.zip')
T = os.path.join(S, 'n3'); os.makedirs(T, exist_ok=True)
p4 = sim.bump(PKG, '0.4', os.path.join(T, 'p4.zip'))
p5 = sim.bump(PKG, '0.5', os.path.join(T, 'p5.zip'))
which = sys.argv[1:] or ['dangling', 'freshfail', 'removewait', 'pushwait', 'busy']


def res(gx):
    r = gx.result_full()
    return r and (r['result'], r['to'], r['note'][:100])


def listing(gx):
    d = os.path.join(gx.data, 'xregulator')
    out = []
    for root, dirs, files in os.walk(d):
        for f in dirs + files:
            out.append(os.path.relpath(os.path.join(root, f), gx.data))
    return sorted(out)


if 'dangling' in which:
    gx = sim.GX()
    try:
        os.symlink(os.path.join(gx.data, 'nowhere', 'rc.local'), gx.rc)
        gx.reboot(PKG)
        print('dangling: install result', res(gx))
        outs = [gx.reboot(PKG) for _ in range(2)]
        print('          same stick left in, 2 reboots ->', outs)
    finally:
        gx.close()

if 'freshfail' in which:
    gx = sim.GX(boot_verify_s=8)
    try:
        gx.env['SIM_BROKEN'] = '0.3'
        gx.reboot(PKG)
        print('freshfail: result', res(gx), '\n           left:', [p for p in (gx.app, gx.link, gx.svc, gx.rc) if os.path.lexists(p)],
              '\n           /data/xregulator:', listing(gx))
        print('           same stick at the next boot:', gx.reboot(PKG), '; newer stick:', gx.reboot(p4), gx.installed())
    finally:
        gx.close()

if 'removewait' in which:
    # Remove (the bridge's /tmp copy) while an in-app install waits, without the lock, for a 6 s slow bridge
    gx = sim.GX(verify_s=15)
    try:
        gx.reboot(PKG)
        gx.env['SIM_START_S'] = '6'
        th = threading.Thread(target=lambda: setattr(gx, 'rc_in_app', gx.in_app(p4)))
        th.start()
        time.sleep(2.5)
        print('removewait: confirming present while waiting:', os.path.exists(os.path.join(gx.work, 'confirming')),
              'lock present:', os.path.exists(os.path.join(gx.work, 'lock')))
        rc = gx.remove()
        th.join(60)
        time.sleep(1)
        print('            remove rc=%s, in-app install rc=%s' % (rc, getattr(gx, 'rc_in_app', None)))
        print('            left:', [p for p in (gx.app, gx.link, gx.svc) if os.path.lexists(p)], '/data/xregulator:', listing(gx))
        gx.env['SIM_START_S'] = '0'
        gx.reboot()
        print('            next boot: app=%s running=%s' % (os.path.isdir(gx.app), gx.running(2)))
    finally:
        gx.close()

if 'pushwait' in which:
    # a push (install without --upgrade-only) of 0.5 while an in-app 0.4 install waits for its slow bridge
    gx = sim.GX(verify_s=15)
    try:
        gx.reboot(PKG)
        gx.env['SIM_START_S'] = '5'
        th = threading.Thread(target=lambda: setattr(gx, 'rc_in_app', gx.in_app(p4)))
        th.start()
        time.sleep(2.0)
        new2 = os.path.join(gx.work, 'new2')
        sim.xreg_update.stage(p5, new2)
        t0 = time.monotonic()
        rc2 = gx.sh(os.path.join(new2, 'install.sh'), 'install', new2).returncode
        th.join(60)
        print('pushwait: first install rc=%s, second rc=%s (%.1f s); installed=%s running=%s result=%s prev=%s'
              % (getattr(gx, 'rc_in_app', None), rc2, time.monotonic() - t0, gx.installed(), gx.running(10), res(gx),
                 (gx.read('xregulator', 'install', 'prev', 'VERSION') or '').strip()))
    finally:
        gx.close()

if 'busy' in which:
    # the bridge's view while a confirm waits without the lock
    import tempfile
    U = sim.xreg_update
    os.environ['XREG_BOOT_ID'] = 'bootZ'
    work = tempfile.mkdtemp(dir=T)
    open(os.path.join(work, 'pending'), 'w').write('0.4+1 0.3+1 bootZ - 1.2\n')
    open(os.path.join(work, 'confirming'), 'w').write('%d bootZ\n' % os.getpid())
    up = U.Updater('0.3', os.path.join(S, 'gx_app', 'xregulator'), work, opener=object(), spawn=lambda *a: None)
    print('busy: state while a confirm waits unlocked =', up.state, '; Check accepted:', up.check(),
          '; Remove accepted:', up.remove())
