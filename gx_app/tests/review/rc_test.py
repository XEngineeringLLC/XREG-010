#!/usr/bin/env python3
# rc.local edge cases: what install.sh writes, and whether the app starts on the next boot when Venus OS execs the file
# directly (custom-rc-late.sh: [ -x /data/rc.local ] && /data/rc.local).
import os, sys, subprocess, stat, time
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import reviewenv
S = reviewenv.work()
sys.path.insert(0, os.path.join(S, 'gx_app', 'tools'))
from sim_install import GX

PKG = os.path.join(S, 'out', 'venus-data.zip')


def exec_rc(self):
    # as custom-rc-late.sh: only when executable, exec'd directly (shebang matters)
    if os.path.isfile(self.rc) and os.access(self.rc, os.X_OK):
        try:
            r = subprocess.run([self.rc], env=self.env, capture_output=True, text=True, timeout=60)
            self.last_rc = (r.returncode, r.stderr.strip()[:120])
        except OSError as e:
            self.last_rc = ('exec failed', str(e)[:60])
    else:
        self.last_rc = ('not run', '')


GX.run_rc_local = exec_rc

CASES = {
    'missing': None,
    'owner, not executable': ('#!/bin/sh\necho owner-ran >> "$XREG_DATA/owner.log"\n', 0o644),
    'CRLF': ('#!/bin/sh\r\necho owner-ran >> "$XREG_DATA/owner.log"\r\nexit 0\r\n', 0o755),
    'no trailing newline': ('#!/bin/sh\necho owner-ran >> "$XREG_DATA/owner.log"', 0o755),
    'no trailing newline, ends in exit': ('#!/bin/sh\necho owner-ran >> "$XREG_DATA/owner.log"\nexit 0', 0o755),
    'empty file': ('', 0o755),
    'no shebang': ('echo owner-ran >> "$XREG_DATA/owner.log"\n', 0o755),
    'ends in exec': ('#!/bin/sh\necho owner-ran >> "$XREG_DATA/owner.log"\nexec true\n', 0o755),
    'exit inside if at end': ('#!/bin/sh\nif true; then\n  echo owner-ran >> "$XREG_DATA/owner.log"\n  exit 0\nfi\n', 0o755),
    'disabled (rc.local.disabled only)': 'disabled',
}

for name, spec in CASES.items():
    gx = GX()
    try:
        if spec == 'disabled':
            with open(gx.rc + '.disabled', 'w') as f:
                f.write('#!/bin/sh\necho owner\n')
            os.chmod(gx.rc + '.disabled', 0o755)
        elif spec is not None:
            body, mode = spec
            with open(gx.rc, 'w', newline='') as f:
                f.write(body)
            os.chmod(gx.rc, mode)
        out = gx.reboot(PKG)   # stick install; then rc.local exec'd at S99 of this same boot
        first = (gx.installed(), gx.running(3), gx.result())
        gx.reboot()            # the next boot, no stick
        run = gx.running(4)
        rc_text = gx.read('rc.local')
        dis_text = gx.read('rc.local.disabled')
        mode = oct(stat.S_IMODE(os.stat(gx.rc).st_mode)) if os.path.exists(gx.rc) else None
        owner_ran = (gx.read('owner.log') or '').count('owner-ran')
        print('== %s\n   install boot: installed/running/result %s\n   next boot: bridge running=%s rc.local exec=%s owner_ran=%d mode=%s'
              % (name, first, run, getattr(gx, 'last_rc', None), owner_ran, mode))
        print('   rc.local: %r' % (rc_text[:300] if rc_text is not None else None))
        if dis_text is not None:
            print('   rc.local.disabled: %r' % dis_text[:300])
    finally:
        gx.close()
