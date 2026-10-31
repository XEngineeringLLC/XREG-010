#!/usr/bin/env python3
# Power-loss (SIGKILL) at each step of install.sh, then a reboot of the fake GX from tools/sim_install.py.
import os, sys, json, zipfile, shutil, tempfile, time
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import reviewenv
S = reviewenv.work()
sys.path.insert(0, os.path.join(S, 'gx_app', 'tools'))
import sim_install as sim
from sim_install import GX, bump, PAYLOAD

PKG = os.path.join(S, 'out', 'venus-data.zip')
T = tempfile.mkdtemp(dir=S, prefix='ct_')


def instrument(src_text):
    import re
    out = []
    fn = None
    for l in src_text.split('\n'):
        s = l.strip()
        m = re.match(r'^(\w+)\(\) \{', l)
        if m:
            fn = m.group(1)
        if fn == 'do_install' and s == 'mkdir -p "$DATA/apps/available"':
            out.append('\tcrash c2')
        if fn == 'do_install' and s == 'rc_set':
            out.append('\tcrash c3')
        if fn == 'do_install' and s.startswith('service_up || log'):
            out.append('\tcrash c4')
        if fn == 'do_install' and s == 'wait_confirm "$win"':
            out.append('\tcrash c7')
        if fn == 'rc_commit' and s == 'mv "$2" "$1"':
            out.append('\tcrash k1')
        if fn == 'rollback' and s == 'if ! mv "$APP" "$WORK/failed" || ! mv "$WORK/prev" "$APP"; then':
            l = '\tif ! mv "$APP" "$WORK/failed" || ! { crash r1; mv "$WORK/prev" "$APP"; }; then'
        out.append(l)
        if fn == 'do_install' and s.startswith('echo "$to ${from:--} $BOOT_ID $RUN_ID'):
            out.append('\tcrash c1')
        if fn == 'plugin_link' and s == 'rm -f "$LINK"':
            out.append('\t\tcrash c5')
        if fn == 'do_install' and s.startswith('plugin_link'):
            out.append('\tcrash c6')
        if fn == 'rollback' and s == 'plugin_link "$WORK/failed/gui-v2/xregulator.json"':
            out.append('\tcrash r2')
        if fn == 'rollback' and s == 'svok "$SVC" 2>/dev/null && svc -t "$SVC"':
            out.append('\tcrash r3')
    text = '\n'.join(out)
    text = text.replace('log() {', 'crash() { [ "$XREG_CRASH" = "$1" ] && { echo "CRASH $1" >> "$WORK/crash.log"; kill -9 $$; }; return 0; }\n\nlog() {', 1)
    return text


def repack(pkg, version, out, install_text=None):
    with zipfile.ZipFile(pkg) as z, zipfile.ZipFile(out, 'w') as o:
        for zi in z.infolist():
            data = z.read(zi)
            if zi.filename in ('rc/VERSION', PAYLOAD + 'VERSION'):
                data = (version + '\n').encode()
            elif zi.filename == PAYLOAD + 'gui-v2/xregulator.json':
                j = json.loads(data); j['version'] = version; data = json.dumps(j).encode()
            elif install_text is not None and zi.filename in ('rc/install.sh', PAYLOAD + 'install.sh'):
                data = install_text.encode()
            o.writestr(zi, data)
    return out


def state(gx):
    return dict(installed=gx.installed(), running=gx.running(3), result=gx.result(), rc=len(gx.rc_lines()),
                link=os.path.islink(gx.link) and os.path.exists(gx.link), prev=os.path.isdir(os.path.join(gx.work, 'prev')),
                pending=os.path.exists(os.path.join(gx.work, 'pending')), lock=os.path.exists(os.path.join(gx.work, 'lock')))


def main():
    with open(os.path.join(S, 'gx_app', 'xregulator', 'install.sh')) as f:
        inst = instrument(f.read())
    with open(os.path.join(T, 'install_instr.sh'), 'w') as f:
        f.write(inst)
    for c in ('c1', 'c2', 'c3', 'c4', 'c5', 'c6', 'c7', 'k1', 'r1', 'r2', 'r3'):
        assert ('crash %s' % c) in inst, c
    ch = {'install.sh': inst.encode()}
    base = bump(PKG, '0.3', os.path.join(T, 'base.zip'), change=ch)
    old = bump(PKG, '0.2.9', os.path.join(T, 'old.zip'), change=ch)
    p4 = bump(PKG, '0.4', os.path.join(T, 'p4.zip'), change=ch)
    p5 = bump(PKG, '0.5', os.path.join(T, 'p5.zip'), change=ch)
    points = sys.argv[1:] or ['c1', 'c2', 'c3', 'c4', 'c5', 'c6', 'c7', 'k1', 'r1', 'r2', 'r3']
    for cp in points:
        for stick in (None, 'old'):
            gx = GX()
            try:
                gx.reboot(base)                 # 0.3 from a stick
                rc = gx.in_app(p4)              # 0.4 ok
                assert gx.installed() == '0.4' and gx.running() == '0.4', ('setup', state(gx))
                gx.env['XREG_CRASH'] = cp
                if cp.startswith('r'):
                    gx.env['SIM_BROKEN'] = '0.5'
                rc = gx.in_app(p5)
                after = state(gx)
                gx.env.pop('XREG_CRASH')
                gx.env['SIM_BROKEN'] = ''
                st = 'with a 0.2.9 stick' if stick else 'no stick'
                gx.reboot(old if stick else None)
                time.sleep(0.5)
                fin = state(gx)
                ok = fin['running'] in ('0.4', '0.5') and fin['rc'] == 1 and fin['link'] and not fin['pending']
                print('%s %s, %s: after crash %s\n      after reboot %s' % ('OK  ' if ok else 'BAD ', cp, st, after, fin))
            finally:
                gx.close()


if __name__ == '__main__':
    main()
