#!/usr/bin/env python3
# Recovery without SSH: an install whose rc.local line is gone (another add-on rewrote rc.local, or a remove cut short
# by power loss after it stripped the line), then a reboot with the same version's stick.
import os, sys, shutil, time
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import reviewenv
S = reviewenv.work()
sys.path.insert(0, os.path.join(S, 'gx_app', 'tools'))
import sim_install as sim

PKG = os.path.join(S, 'out', 'venus-data.zip')

gx = sim.GX()
try:
    gx.reboot(PKG)
    print('installed by stick:', gx.installed(), 'running:', gx.running())
    with open(gx.rc, 'w') as f:            # another add-on's installer: echo '...' > /data/rc.local
        f.write('#!/bin/sh\n/data/other-addon/start.sh &\n')
    gx.reboot()
    print('after rc.local rewritten, reboot: app dir=%s link=%s service copy=%s bridge running=%s'
          % (os.path.isdir(gx.app), os.path.islink(gx.link), os.path.isdir(gx.svc), gx.running(3)))
    out = gx.reboot(PKG)
    print('reboot with the same-version stick: %s; bridge running=%s, rc lines=%d' % (out, gx.running(3), len(gx.rc_lines())))
finally:
    gx.close()
