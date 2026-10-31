#!/usr/bin/env python3
# Second-round checks of the reworked install.sh on the project's own fake GX (tools/sim_install.py).
import os, sys, time, json, shutil, stat, threading, zipfile
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import reviewenv
S = reviewenv.work()
sys.path.insert(0, os.path.join(S, 'gx_app', 'tools'))
import sim_install as sim

PKG = os.path.join(S, 'out', 'venus-data.zip')
T = os.path.join(S, 'nt'); os.makedirs(T, exist_ok=True)
p4 = sim.bump(PKG, '0.4', os.path.join(T, 'p4.zip'))
which = sys.argv[1:] or ['repeat', 'cronly', 'symlink', 'both', 'twoarch', 'ownline', 'bench']


def res(gx):
    r = gx.result_full()
    return r and (r['result'], r['to'], r['toBuild'], r['note'][:110])


if 'repeat' in which:
    # a healthy app with its stick left in: never reinstalled
    gx = sim.GX()
    try:
        gx.reboot(PKG)
        h0, r0 = gx.tree_hash(gx.app), os.stat(os.path.join(gx.work, 'result')).st_mtime
        outs = [gx.reboot(PKG) for _ in range(3)]
        print('repeat: stick left in, 3 reboots ->', outs, 'tree unchanged:', gx.tree_hash(gx.app) == h0,
              'result untouched:', os.stat(os.path.join(gx.work, 'result')).st_mtime == r0, 'running:', gx.running())
        with open(gx.rc + '.disabled', 'w') as f:   # hooks disabled after install: line moves to .disabled
            f.write(gx.read('rc.local'))
        os.remove(gx.rc)
        outs = [gx.reboot(PKG) for _ in range(2)]
        print('repeat: hooks disabled after install, stick left in, 2 reboots ->', outs)
    finally:
        gx.close()

if 'cronly' in which:
    # old Mac line endings (CR only)
    gx = sim.GX()
    try:
        with open(gx.rc, 'wb') as f:
            f.write(b'#!/bin/sh\recho owner >> "$XREG_DATA/owner.log"\r')
        os.chmod(gx.rc, 0o755)
        gx.reboot(PKG)
        r1 = res(gx)
        gx.reboot()
        print('cronly: install result', r1, '\n        rc.local now %r' % gx.read('rc.local')[:120],
              '\n        next boot: running=%s rc exec=%s' % (gx.running(3), gx.last_rc))
        out = gx.reboot(PKG)
        print('        same stick: %s, running=%s' % (out, gx.running(3)))
    finally:
        gx.close()

if 'symlink' in which:
    gx = sim.GX()
    try:
        target = os.path.join(gx.data, 'myscripts', 'rc.local')
        os.makedirs(os.path.dirname(target))
        with open(target, 'w') as f:
            f.write('#!/bin/sh\necho owner >> "$XREG_DATA/owner.log"\n')
        os.chmod(target, 0o755)
        os.symlink(target, gx.rc)
        gx.reboot(PKG)
        print('symlink: rc.local still a link: %s; target has our line: %s; rc.local has our line: %s' % (
            os.path.islink(gx.rc), '# xregulator-app' in open(target).read(), len(gx.rc_lines()) == 1))
        with open(target, 'a') as f:
            f.write('echo owner-edit >> "$XREG_DATA/owner.log"\n')
        gx.reboot()
        print('         owner edits the target afterwards; the edit runs at boot: %s' % (
            'owner-edit' in (gx.read('owner.log') or '')))
    finally:
        gx.close()

if 'both' in which:
    # installed with hooks disabled (line in rc.local.disabled); later something creates /data/rc.local
    gx = sim.GX()
    try:
        with open(gx.rc + '.disabled', 'w') as f:
            f.write('#!/bin/sh\necho owner\n')
        gx.reboot(PKG)
        with open(gx.rc, 'w') as f:
            f.write('#!/bin/sh\n/data/other-addon/start.sh &\n')
        os.chmod(gx.rc, 0o755)
        gx.reboot()
        print('both: rc.local (no line) + rc.local.disabled (line): bridge running=%s' % gx.running(3))
        out = gx.reboot(PKG)
        print('      same-release stick: %s, running=%s' % (out, gx.running(3)))
    finally:
        gx.close()

if 'twoarch' in which:
    # two of our archives in one boot (venus-data-0.3.zip and venus-data.zip with 0.4) on a fresh GX
    gx = sim.GX(boot_verify_s=20)
    try:
        orig = gx.reboot

        def reboot2(a1, a2):
            gx.svscan_off(); gx.kill_all(); shutil.rmtree(gx.service); os.makedirs(gx.service)
            gx.boot += 1; gx.env['XREG_BOOT_ID'] = 'boot%d' % gx.boot; gx.booted = time.monotonic(); gx._tick_uptime()
            t0 = time.monotonic()
            o1 = gx.update_data(a1)
            t1 = time.monotonic()
            o2 = gx.update_data(a2)   # update-data.sh unpacks each archive in turn, at S30, before svscan (S95)
            t2 = time.monotonic()
            gx.svscan_on(); gx.run_rc_local(); gx.settle()
            return o1, o2, t1 - t0, t2 - t1
        o1, o2, d1, d2 = reboot2(PKG, p4)
        time.sleep(2)
        print('twoarch: archive 1 %s (%.1f s), archive 2 %s (%.1f s at "S30", BOOT_VERIFY_S=20)' % (o1, d1, o2, d2))
        print('         installed=%s running=%s result=%s failed_version=%r' % (
            gx.installed_release(), gx.running_release(4), res(gx),
            (gx.read('xregulator', 'install', 'failed_version') or '').strip()))
    finally:
        gx.close()

if 'ownline' in which:
    # rc.local holding only our line (no shebang)
    gx = sim.GX()
    try:
        gx.reboot(PKG)
        lines = gx.read('rc.local').splitlines()
        with open(gx.rc, 'w') as f:
            f.write(lines[1] + '\n')
        gx.in_app(p4)
        print('ownline: rc.local = only our line; in-app install result %s; rc lines now %d' % (res(gx), len(gx.rc_lines())))
    finally:
        gx.close()

if 'bench' in which:
    # push_to_gx.sh tree (no BUILD, no FILES) of the same version, then the published stick left in
    gx = sim.GX()
    try:
        gx.reboot(PKG)
        new = os.path.join(gx.work, 'new')
        sim.xreg_update.stage(PKG, new)
        for f in ('BUILD', 'FILES'):
            os.remove(os.path.join(new, f))
        gx.sh(os.path.join(new, 'install.sh'), 'install', new)
        print('bench: pushed tree release', gx.installed_release(), 'running', gx.running_release())
        print('       reboot with the published stick:', gx.reboot(PKG), '->', gx.installed_release())
    finally:
        gx.close()
