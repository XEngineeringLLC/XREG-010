#!/usr/bin/env python3
# Offline check of a GX app package; build_package.py runs it on every build. A fake GX root (/data, /service, a USB
# stick) with daemontools, dbus-send and the bridge stood in by shell shims, booted in Venus OS's order: update-data.sh
# at rc5 S30, svscanboot S95, /data/rc.local S99 (meta-victronenergy recipes initscripts, daemontools-run-venus,
# custom-rc.d).
# update-data.sh's unpack is done as its source does it (rc/* to a temp dir; rc/pre-hook.sh, a non-zero exit skipping
# the rest; everything but rc/ into /data; rc/post-hook.sh success|extraction-failed) and as busybox unzip extracts
# (modes from Unix entries, umask applied to files, no file times). /data/rc.local is executed directly when it is
# executable, as custom-rc-late.sh does, so its shebang and line endings count. install.sh and the hooks run under
# dash, the Almquist shell busybox's ash descends from. A fake /proc/uptime restarts at each boot; a `date` shim takes
# an offset, to set the wall clock the way NTP or GPS does after boot.
#   python3 sim_install.py <venus-data.zip>     the standard scenarios; PASS/FAIL lines, exit 1 on a failure
import fnmatch, hashlib, json, os, shutil, stat, subprocess, sys, tempfile, threading, time, zipfile

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'xregulator', 'bridge'))
import xreg_update   # noqa: E402

SHELL = '/bin/dash' if os.path.exists('/bin/dash') else '/bin/sh'
PAYLOAD = xreg_update.PAYLOAD
UMASK = 0o022
SHA256SUM = shutil.which('sha256sum') or shutil.which('gsha256sum')

SHIMS = {
    'sh': '#!/bin/sh\nexec %s "$@"\n' % SHELL,
    'pidof': '#!/bin/sh\n[ "$1" = svscan ] && [ -f "$SIM_ROOT/state/svscan" ]\n',
    'pgrep': '#!/bin/sh\nexit 1\n',
    'svok': '#!/bin/sh\np=$(cat "$1/supervise/pid" 2>/dev/null) && [ -n "$p" ] && kill -0 "$p" 2>/dev/null\n',
    # records each call, one argument a line, then hangs SIM_DBUS_S seconds, as a bus that does not answer would
    'dbus-send': r'''#!/bin/sh
{ for a in "$@"; do printf '%s\n' "$a"; done; echo --; } >> "$SIM_ROOT/state/dbus-send.log"
sleep "${SIM_DBUS_S:-0}"
''',
    'date': r'''#!/bin/sh
off=$(cat "$SIM_ROOT/state/clock_offset" 2>/dev/null || echo 0)
if [ "$1" = +%s ]; then echo $(( $(/bin/date +%s) + off )); else exec /bin/date "$@"; fi
''',
    # daemontools svc as install.sh uses it: -t restart, -d down, -x supervise exits, -u up
    'svc': r'''#!/bin/sh
opts=
while [ $# -gt 0 ]; do case $1 in -*) opts=$opts${1#-}; shift ;; *) break ;; esac; done
until mkdir "$SIM_ROOT/state/svlock" 2>/dev/null; do sleep 0.05; done
for d in "$@"; do
	[ -d "$d/supervise" ] || continue
	p=$(cat "$d/supervise/pid" 2>/dev/null)
	case $opts in *t*|*d*) [ -n "$p" ] && kill "$p" 2>/dev/null; rm -f "$d/supervise/pid" ;; esac
	case $opts in *d*) touch "$d/supervise/down" ;; esac
	case $opts in *t*|*u*) rm -f "$d/supervise/down"; sh "$SIM_ROOT/bin/sim_start" "$d" ;; esac
	case $opts in *x*) rm -rf "$d/supervise" ;; esac
	echo "svc -$opts $d" >> "$SIM_ROOT/state/svc.log"
done
rmdir "$SIM_ROOT/state/svlock"
''',
    'sim_start': r'''#!/bin/sh
mkdir -p "$1/supervise"
case $1 in
*/log) sleep 600 >/dev/null 2>&1 </dev/null & ;;
*) sh "$SIM_ROOT/bin/fake_bridge" "$1" >/dev/null 2>&1 </dev/null & ;;
esac
echo $! > "$1/supervise/pid"
echo $! >> "$SIM_ROOT/state/pids"
''',
    # the bridge run from $1/run: gone at once when its app is missing or its version (or release) is in SIM_BROKEN,
    # else it writes the running marker as xreg_update.mark_running does (0.3 and later) and stays up
    'fake_bridge': r'''#!/bin/sh
py=$(sed -n 's/^exec python3 //p' "$1/run")
py=$XREG_DATA${py#/data}
app=${py%/bridge/*}
[ -f "$py" ] || exit 1
v=$(head -n 1 "$app/VERSION" 2>/dev/null | tr -d ' \r\n')
b=$(head -n 1 "$app/BUILD" 2>/dev/null | tr -d ' \r\n')
case " $SIM_BROKEN " in *" $v "*|*" $v+${b:-0} "*) exit 1 ;; esac
sleep "${SIM_START_S:-0}"
if [ -f "$app/bridge/xreg_update.py" ]; then
	w=$XREG_DATA/xregulator/install
	mkdir -p "$w" && echo "$v+${b:-0} $$ $XREG_BOOT_ID" > "$w/running.tmp" && mv "$w/running.tmp" "$w/running"
fi
exec sleep 600
''',
}
if SHA256SUM:
    SHIMS['sha256sum'] = '#!/bin/sh\nexec %s "$@"\n' % SHA256SUM


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except (OSError, TypeError):
        return False


class GX:
    def __init__(self, verify_s=8, boot_verify_s=20, boot_uptime_s=60):
        self.root = tempfile.mkdtemp(prefix='xreg_gx_')
        self.data, self.service = os.path.join(self.root, 'data'), os.path.join(self.root, 'service')
        self.bin, self.state = os.path.join(self.root, 'bin'), os.path.join(self.root, 'state')
        for d in (self.data, self.service, self.bin, self.state):
            os.makedirs(d)
        for name, body in SHIMS.items():
            p = os.path.join(self.bin, name)
            with open(p, 'w') as f:
                f.write(body)
            os.chmod(p, 0o755)
        self.app = os.path.join(self.data, 'apps', 'available', 'xregulator')
        self.link = os.path.join(self.data, 'apps', 'enabled', 'xregulator')
        self.work = os.path.join(self.data, 'xregulator', 'install')
        self.svc = os.path.join(self.service, 'xregulator')
        self.rc = os.path.join(self.data, 'rc.local')
        self.uptime = os.path.join(self.state, 'uptime')
        self.boot = 0
        self.booted = time.monotonic()
        self.uptime_lock = threading.Lock()
        self.env = dict(os.environ, PATH=self.bin + ':' + os.environ.get('PATH', '/usr/bin:/bin'), SIM_ROOT=self.root,
                        XREG_DATA=self.data, XREG_SERVICE=self.service, XREG_UPTIME=self.uptime,
                        XREG_LOGDIR=os.path.join(self.root, 'varlog', 'xregulator'), XREG_VERIFY_S=str(verify_s),
                        XREG_BOOT_VERIFY_S=str(boot_verify_s), XREG_BOOT_UPTIME_S=str(boot_uptime_s),
                        XREG_STABLE_S='1', XREG_BOOT_ID='boot0', SIM_BROKEN='')
        self.boot_uptime_s = boot_uptime_s
        self.env.pop('XREG_RUN_ID', None)
        self.scanning = False
        self.scanner = None
        self.ticking = True
        self._tick_uptime()
        threading.Thread(target=self._uptime_loop, daemon=True).start()
        self.transcript = []
        self.last_rc = None

    # -- processes --

    def sh(self, *args):
        r = subprocess.run([SHELL] + [str(a) for a in args], env=self.env, capture_output=True, text=True, timeout=300)
        self.transcript.append('$ sh %s -> %d\n%s%s' % (' '.join(map(str, args)), r.returncode, r.stdout, r.stderr))
        return r

    def _tick_uptime(self):
        with self.uptime_lock:
            tmp = self.uptime + '.tmp'
            with open(tmp, 'w') as f:
                f.write('%.2f 0.00\n' % (time.monotonic() - self.booted))
            os.replace(tmp, self.uptime)

    def _uptime_loop(self):
        while self.ticking:
            try:
                self._tick_uptime()
            except OSError:
                return
            time.sleep(0.1)

    def age(self):
        # the GX up for longer than install.sh's boot phase, as when an owner taps Update
        if time.monotonic() - self.booted <= self.boot_uptime_s:
            self.booted = time.monotonic() - self.boot_uptime_s - 1
            self._tick_uptime()

    def set_clock(self, offset_s):
        # the wall clock only: NTP or GPS setting the time; /proc/uptime goes on as before
        with open(os.path.join(self.state, 'clock_offset'), 'w') as f:
            f.write('%d\n' % offset_s)

    def _svlock(self):
        lk = os.path.join(self.state, 'svlock')
        while True:
            try:
                os.mkdir(lk)
                return lk
            except FileExistsError:
                time.sleep(0.02)

    def _scan_once(self):
        # svscan starts supervise in every directory not named with a dot; supervise restarts a service that exits
        lk = self._svlock()
        try:
            for name in sorted(os.listdir(self.service)):
                if name.startswith('.'):
                    continue
                for d in (os.path.join(self.service, name), os.path.join(self.service, name, 'log')):
                    if not os.path.isfile(os.path.join(d, 'run')) or os.path.exists(os.path.join(d, 'supervise', 'down')):
                        continue
                    pidf = os.path.join(d, 'supervise', 'pid')
                    try:
                        with open(pidf) as f:
                            pid = int(f.read().strip())
                        age = time.time() - os.path.getmtime(pidf)
                    except (OSError, ValueError):
                        pid, age = None, 99
                    if (pid and alive(pid)) or age < 1.0:
                        continue
                    subprocess.run([SHELL, os.path.join(self.bin, 'sim_start'), d], env=self.env)
        finally:
            os.rmdir(lk)

    def _scan_loop(self):
        while self.scanning:
            self._scan_once()
            time.sleep(0.25)

    def svscan_on(self):
        open(os.path.join(self.state, 'svscan'), 'w').close()
        self.scanning = True
        self.scanner = threading.Thread(target=self._scan_loop, daemon=True)
        self.scanner.start()

    def svscan_off(self):
        self.scanning = False
        if self.scanner:
            self.scanner.join()
        if os.path.exists(os.path.join(self.state, 'svscan')):
            os.remove(os.path.join(self.state, 'svscan'))

    def kill_all(self):
        try:
            with open(os.path.join(self.state, 'pids')) as f:
                pids = [int(x) for x in f.read().split()]
        except OSError:
            pids = []
        for p in pids:
            try:
                os.kill(p, 9)
            except OSError:
                pass
        # install.sh processes of the boot before (a detached confirm waiting for a bridge)
        subprocess.run(['pkill', '-9', '-f', self.root + '/data/'], capture_output=True)

    # -- Venus OS boot --

    def unzip(self, archive, dest, accept=None, reject=None):
        # busybox unzip -o -d dest archive [accept] [-x reject]
        try:
            with zipfile.ZipFile(archive) as z:
                for zi in z.infolist():
                    name = zi.filename.lstrip('/')
                    while name.startswith('../'):
                        name = name[3:]
                    if (reject and fnmatch.fnmatchcase(name, reject)) or (accept and not fnmatch.fnmatchcase(name, accept)):
                        continue
                    unix = zi.create_system == 3
                    mode = (zi.external_attr >> 16) & 0o7777 if unix else None
                    target = os.path.join(dest, name)
                    if name.endswith('/'):
                        if not os.path.isdir(target):
                            os.makedirs(target)
                            os.chmod(target, mode if mode is not None else 0o777 & ~UMASK)
                        continue
                    os.makedirs(os.path.dirname(target), exist_ok=True)
                    existed = os.path.exists(target)
                    with z.open(zi) as s, open(target, 'wb') as o:
                        shutil.copyfileobj(s, o)
                    if not existed:
                        os.chmod(target, (mode if mode is not None else 0o666) & ~UMASK)
            return True
        except (zipfile.BadZipFile, OSError) as e:
            self.transcript.append('unzip %s: %s' % (archive, e))
            return False

    def update_data(self, archive):
        tmp = tempfile.mkdtemp(dir=self.root)
        try:
            self.unzip(archive, tmp, accept='rc/*')
            pre, post = os.path.join(tmp, 'rc', 'pre-hook.sh'), os.path.join(tmp, 'rc', 'post-hook.sh')
            if os.path.isfile(pre) and self.sh(pre).returncode != 0:
                return 'skipped'
            ok = self.unzip(archive, self.data, reject='rc/*')
            if os.path.isfile(post):
                self.sh(post, 'success' if ok else 'extraction-failed')
            return 'unpacked'
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def run_rc_local(self):
        # custom-rc-late.sh: if [ -x /data/rc.local ]; then /data/rc.local; fi, under busybox ash
        r = subprocess.run([SHELL, '-c', 'if [ -x "$1" ]; then "$1"; fi', 'sh', self.rc], env=self.env,
                           capture_output=True, text=True, timeout=120)
        self.last_rc = (r.returncode, r.stderr.strip()[:200])
        self.transcript.append('rc.local -> %d %s' % self.last_rc)

    def settle(self, timeout=120):
        # until no install.sh holds the lock and none waits to confirm a release
        end = time.time() + timeout
        time.sleep(0.4)
        while time.time() < end:
            if not os.path.exists(os.path.join(self.work, 'lock')) and not os.path.exists(os.path.join(self.work, 'pending')):
                time.sleep(0.3)
                if not os.path.exists(os.path.join(self.work, 'lock')):
                    return True
            time.sleep(0.2)
        return False

    def update_all(self, sticks):
        # update-data.sh unpacks each archive it finds in turn; the times each took
        self.unpack_s = []
        out = None
        for a in sticks:
            t0 = time.monotonic()
            out = self.update_data(a)
            self.unpack_s.append(time.monotonic() - t0)
        return out

    def reboot(self, stick=None, delayed=False):
        # stick: an archive, or a list of them unpacked in turn; delayed: a slow stick, which update-data.sh unpacks in
        # the background after its 30 s wait for /media
        self.svscan_off()
        self.kill_all()
        shutil.rmtree(self.service)
        os.makedirs(self.service)   # a ramdisk rebuilt from the rootfs at every boot (overlays.sh)
        self.boot += 1
        self.env['XREG_BOOT_ID'] = 'boot%d' % self.boot
        self.booted = time.monotonic()
        self._tick_uptime()
        sticks = stick if isinstance(stick, (list, tuple)) else [stick] if stick else []
        out = self.update_all(sticks) if sticks and not delayed else None
        self.svscan_on()
        self.run_rc_local()
        if sticks and delayed:
            out = self.update_all(sticks)
        self.settle()
        return out

    # -- what the bridge does --

    def in_app(self, pkg, *extra):
        # xreg_update.Updater._install after the signature check: stage, then install.sh in the staged tree
        self.age()
        new = os.path.join(self.work, 'new')
        xreg_update.stage(pkg, new)
        return self.sh(os.path.join(new, 'install.sh'), 'install', new, *(extra or ('--upgrade-only',))).returncode

    def remove(self):
        # as the bridge does it, from a copy named as its mkstemp names it
        copy = self.remove_copy = os.path.join(self.root, 'xregulator-remove-sim.sh')
        shutil.copyfile(os.path.join(self.app, 'install.sh'), copy)
        return self.sh(copy, 'remove').returncode

    # -- reading the fake GX --

    def installed(self):
        try:
            with open(os.path.join(self.app, 'VERSION')) as f:
                return f.read().strip()
        except OSError:
            return None

    def installed_release(self):
        v = self.installed()
        return v and xreg_update.release(v, xreg_update.app_build(self.app))

    def running_release(self, timeout=6):
        # the release whose bridge runs this boot, by its marker
        end = time.time() + timeout
        while time.time() < end:
            try:
                with open(os.path.join(self.work, 'running')) as f:
                    v, pid, bid = (f.read().split() + ['', '', ''])[:3]
                if bid == self.env['XREG_BOOT_ID'] and alive(int(pid)):
                    return v
            except (OSError, ValueError):
                pass
            time.sleep(0.2)
        return None

    def running(self, timeout=6):
        r = self.running_release(timeout)
        return r.partition('+')[0] if r else None

    def result_full(self):
        return xreg_update.read_result(self.work)

    def result(self):
        r = self.result_full()
        return (r['result'], r['to'], r['from'], r['note']) if r else None

    def read(self, *parts):
        try:
            with open(os.path.join(self.data, *parts), newline='') as f:
                return f.read()
        except OSError:
            return None

    def rc_lines(self, path=None):
        t = self.read(os.path.relpath(path or self.rc, self.data)) or ''
        return [l for l in t.splitlines() if 'xregulator' in l]

    def notifications(self):
        # the arguments of each dbus-send call
        try:
            with open(os.path.join(self.state, 'dbus-send.log')) as f:
                return [c.splitlines() for c in f.read().split('--\n') if c]
        except OSError:
            return []

    def tree_hash(self, path):
        h = hashlib.sha256()
        for d, dirs, files in sorted(os.walk(path)):
            dirs.sort()
            for f in sorted(files):
                p = os.path.join(d, f)
                h.update(os.path.relpath(p, path).encode() + oct(os.stat(p).st_mode).encode())
                with open(p, 'rb') as fh:
                    h.update(fh.read())
        return h.hexdigest()

    def close(self):
        self.svscan_off()
        self.kill_all()
        self.ticking = False
        shutil.rmtree(self.root, ignore_errors=True)


def files_list(blobs):
    return ''.join('%s  %s\n' % (hashlib.sha256(blobs[rel]).hexdigest(), rel) for rel in sorted(blobs)).encode()


def bump(pkg, version, out, build=None, plugin=True, change=None):
    # the same package as another release: VERSION and BUILD in rc/ and the app tree, the plugin's own version field
    # (plugin=False keeps the plugin file as it is), change {path: bytes} in the app tree, and FILES made again
    with zipfile.ZipFile(pkg) as z:
        infos = z.infolist()
        data = {zi.filename: z.read(zi) for zi in infos}
    if build is None:
        build = int(data.get(PAYLOAD + 'BUILD', b'0').strip() or 0)
    for p in ('rc/VERSION', PAYLOAD + 'VERSION'):
        data[p] = (version + '\n').encode()
    for p in ('rc/BUILD', PAYLOAD + 'BUILD'):
        data[p] = b'%d\n' % build
    if plugin:
        j = json.loads(data[PAYLOAD + 'gui-v2/xregulator.json'])
        j['version'] = version
        data[PAYLOAD + 'gui-v2/xregulator.json'] = json.dumps(j).encode()
    for rel, b in (change or {}).items():
        data[PAYLOAD + rel] = b
    blobs = {n[len(PAYLOAD):]: b for n, b in data.items()
             if n.startswith(PAYLOAD) and not n.endswith('/') and n != PAYLOAD + 'FILES'}
    if PAYLOAD + 'FILES' in data:
        data[PAYLOAD + 'FILES'] = data['rc/FILES'] = files_list(blobs)
    with zipfile.ZipFile(out, 'w') as o:
        names = {zi.filename for zi in infos}
        for zi in infos:
            o.writestr(zi, data[zi.filename])
        for p in ('rc/BUILD', PAYLOAD + 'BUILD'):
            if p not in names:
                zi = zipfile.ZipInfo(p)
                zi.create_system, zi.external_attr = 3, (stat.S_IFREG | 0o644) << 16
                o.writestr(zi, data[p])
    return out


def package_build(pkg):
    with zipfile.ZipFile(pkg) as z:
        return xreg_update.parse_release(xreg_update.package_release(z))[1]


def old_bench_tree(pkg, dest, version='0.2'):
    # an install from before 0.3 the way push_to_gx.sh made it: no VERSION file, no xreg_update.py, VERSION in xreg_core
    tmp = tempfile.mkdtemp()
    xreg_update.stage(pkg, tmp)
    for f in ('VERSION', 'BUILD', 'FILES', 'bridge/xreg_update.py'):
        if os.path.exists(os.path.join(tmp, f)):
            os.remove(os.path.join(tmp, f))
    with open(os.path.join(tmp, 'bridge', 'xreg_core.py'), 'a') as f:
        f.write("\nVERSION = '%s'\n" % version)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    shutil.move(tmp, dest)


class Checks:
    def __init__(self):
        self.fails = 0
        self.passes = 0

    def __call__(self, cond, msg, gx=None):
        if cond:
            self.passes += 1
            print('PASS ' + msg, flush=True)
        else:
            self.fails += 1
            print('FAIL ' + msg, flush=True)
            if gx:
                print('\n'.join(gx.transcript[-6:]))


HOOKS_OFF = 'Custom startup scripts are disabled on this GX, so the app will not start after a restart until they are ' \
            're-enabled in Settings > General > Support status.'

RC_CASES = {
    # name: (rc.local bytes, mode, note expected in the result)
    'ends in exec': (b'#!/bin/sh\necho owner >> "$XREG_DATA/owner.log"\nexec true\n', 0o755, ''),
    'exit inside if...fi at the end': (b'#!/bin/sh\nif true; then\n  echo owner >> "$XREG_DATA/owner.log"\n  exit 0\nfi\n',
                                       0o755, ''),
    'no shebang': (b'echo owner >> "$XREG_DATA/owner.log"\n', 0o755, ''),
    'Windows line endings': (b'#!/bin/sh\r\necho owner >> "$XREG_DATA/owner.log"\r\nexit 0\r\n', 0o755,
                             'carriage-return line endings'),
    'old Mac line endings (CR only)': (b'#!/bin/sh\recho owner >> "$XREG_DATA/owner.log"\r', 0o755,
                                       'carriage-return line endings'),
    'not executable': (b'#!/bin/sh\necho owner >> "$XREG_DATA/owner.log"\n', 0o644, 'was not executable'),
    'no final newline': (b'#!/bin/sh\necho owner >> "$XREG_DATA/owner.log"', 0o755, ''),
}


def standard(pkg, check):
    with zipfile.ZipFile(pkg) as z:
        rid = xreg_update.package_release(z)
    v, b = xreg_update.parse_release(rid)
    t = tempfile.mkdtemp()
    v1, v2 = v + '.1', v + '.2'
    p1, p2 = bump(pkg, v1, os.path.join(t, 'p1.zip')), bump(pkg, v2, os.path.join(t, 'p2.zip'))
    reissue = bump(p1, v1, os.path.join(t, 'p1b.zip'), build=b + 100)
    older = bump(p1, v1, os.path.join(t, 'p1a.zip'), build=max(b - 100, 0))

    gx = GX()
    try:
        out = gx.reboot(pkg)
        check(out == 'unpacked' and gx.installed_release() == rid and gx.running_release() == rid,
              'fresh GX, stick at boot: %s installed, its bridge running (%s, %s)' % (rid, out, gx.running_release()), gx)
        check(gx.result() == ('ok', v, '', ''), 'fresh GX: result ok, nothing to confirm (%s)' % (gx.result(),), gx)
        lines = (gx.read('rc.local') or '').splitlines()
        check(lines[0] == '#!/bin/sh' and lines[1].endswith('# xregulator-app') and len(lines) == 2
              and os.access(gx.rc, os.X_OK), 'fresh GX: rc.local made with the shebang and our line, executable', gx)
        check(os.path.islink(gx.link) and os.readlink(gx.link) == gx.app and os.path.isfile(os.path.join(gx.svc, 'run'))
              and not os.path.exists(os.path.join(gx.work, 'new')), 'fresh GX: plugin link, service copy, stage gone', gx)
        modes = {rel: stat.S_IMODE(os.stat(os.path.join(gx.app, rel)).st_mode) for rel in
                 ('install.sh', 'service/run', 'service/log/run', 'bridge/xregulator_bridge.py', 'bridge/xreg_core.py',
                  'gui-v2/xregulator.json', 'bridge/ext/velib_python/vedbus.py', 'FILES', 'BUILD')}
        dirs = [stat.S_IMODE(os.stat(d).st_mode) for d, _, _ in os.walk(gx.app)]
        check(all(m == 0o755 for k, m in modes.items() if k.endswith(('.sh', 'run', 'bridge.py')))
              and all(m == 0o644 for k, m in modes.items() if k.endswith(('core.py', '.json', 'vedbus.py', 'FILES', 'BUILD')))
              and all(m == 0o755 for m in dirs), 'fresh GX: modes as packed (%s)' % {k: oct(m) for k, m in modes.items()})

        before = gx.tree_hash(gx.app)
        out = gx.reboot(pkg)
        check(out == 'skipped' and gx.tree_hash(gx.app) == before and gx.running_release() == rid,
              'stick left in, reboot: skipped, app untouched, %s running' % rid, gx)

        rc = gx.in_app(p1)
        r1 = gx.installed_release()
        check(rc == 0 and gx.installed() == v1 and gx.running_release() == r1 and gx.result() == ('ok', v1, v, ''),
              'in-app update %s -> %s: ok, new bridge running (%s)' % (v, v1, gx.result()), gx)
        check(gx.read('xregulator', 'install', 'prev', 'VERSION') == v + '\n' and len(gx.rc_lines()) == 1,
              'in-app update: previous version kept, still one rc.local line', gx)

        out = gx.reboot(pkg)
        check(out == 'skipped' and gx.installed() == v1 and gx.running() == v1,
              'reboot with the %s stick still in after the update: skipped, %s stays' % (v, v1), gx)

        out = gx.reboot(reissue)
        check(out == 'unpacked' and gx.installed_release() == '%s+%d' % (v1, b + 100) and gx.running() == v1,
              'reissued %s (build %d) on a stick: admitted over build %d and running' % (v1, b + 100, b), gx)
        out = gx.reboot(p1)
        check(out == 'skipped' and gx.installed_release() == '%s+%d' % (v1, b + 100),
              'a stick with the older build of %s: skipped' % v1, gx)
        rc = gx.in_app(older)
        check(rc != 0 and gx.installed_release() == '%s+%d' % (v1, b + 100) and gx.result()[0] == 'failed',
              'in-app older build of %s refused' % v1, gx)

        gx.env['SIM_BROKEN'] = v2
        rc = gx.in_app(p2)
        check(rc != 0 and gx.installed() == v1 and gx.running() == v1 and gx.result()[:3] == ('rolledback', v2, v1)
              and gx.result()[3].endswith('The previous version is back and running.'),
              'update to %s whose bridge does not start: %s put back and running (%s)' % (v2, v1, gx.result()), gx)
        check(gx.read('xregulator', 'install', 'failed_version') == '%s+%d\n' % (v2, b) and not os.path.exists(
            os.path.join(gx.work, 'pending')), 'rollback: failed release recorded with its build, nothing pending', gx)
        out = gx.reboot(p2)
        check(out == 'skipped' and gx.installed() == v1 and gx.running() == v1,
              'reboot with a %s stick: the release that failed is not retried' % v2, gx)
        gx.env['SIM_BROKEN'] = ''

        rc = gx.in_app(pkg)
        check(rc != 0 and gx.installed() == v1 and gx.running() == v1 and gx.result()[:3] == ('failed', v, v1)
              and not os.path.exists(os.path.join(gx.work, 'new')), 'in-app %s over %s refused as a downgrade' % (v, v1), gx)

        # another add-on rewrites rc.local with >: the app no longer starts; the same stick repairs it
        with open(gx.rc, 'w') as f:
            f.write('#!/bin/sh\necho other-addon >> /dev/null\n')
        gx.reboot()
        check(gx.running(2) is None, 'rc.local rewritten by another add-on: the app does not start', gx)
        out = gx.reboot(reissue)
        check(out == 'unpacked' and gx.running_release() == '%s+%d' % (v1, b + 100) and len(gx.rc_lines()) == 1
              and 'other-addon' in gx.read('rc.local'), 'the same release on a stick repairs it: running, line back, '
              "the other add-on's line kept", gx)
        os.remove(os.path.join(gx.app, 'bridge', 'xreg_cloud.py'))
        out = gx.reboot(reissue)
        check(out == 'unpacked' and os.path.isfile(os.path.join(gx.app, 'bridge', 'xreg_cloud.py')),
              'an app file missing: the same stick repairs it', gx)

        rc = gx.remove()
        left = [p for p in (gx.app, gx.link, gx.svc, os.path.join(gx.data, 'xregulator'), gx.remove_copy)
                if os.path.lexists(p)]
        check(rc == 0 and not left and not gx.rc_lines() and 'other-addon' in gx.read('rc.local'),
              'remove: nothing left, its /tmp copy gone too, the other lines of rc.local kept (%s)' % left, gx)
        out = gx.reboot()
        check(not os.path.exists(gx.app) and not os.listdir(gx.service), 'remove, reboot: nothing starts', gx)
        check(not gx.notifications(), 'no notification from any of these, which the page shows (%s)'
              % gx.notifications(), gx)
    finally:
        gx.close()

    gx = GX()
    try:
        with open(gx.rc + '.disabled', 'w') as f:
            f.write('#!/bin/sh\necho owner\n')
        os.chmod(gx.rc + '.disabled', 0o755)
        out = gx.reboot(pkg)
        r = gx.result()
        check(out == 'unpacked' and gx.running_release() == rid and r and r[0] == 'ok' and r[3] == HOOKS_OFF
              and not os.path.exists(gx.rc) and len(gx.rc_lines(gx.rc + '.disabled')) == 1,
              'custom startup scripts disabled, stick install: confirmed this boot, the result says why it will not '
              'start after a restart (%s)' % (r,), gx)
        gx.reboot()
        check(gx.running(2) is None and gx.result()[0] == 'ok', 'disabled: the next boot starts nothing, the result stays', gx)
    finally:
        gx.close()

    # a stick that mounts late, unpacked after svscan has started while the services still start, its bridge 12 s slow
    gx = GX(verify_s=8, boot_verify_s=30)
    try:
        gx.reboot(pkg)
        gx.env['SIM_START_S'] = '12'
        out = gx.reboot(p1, delayed=True)
        gx.env['SIM_START_S'] = '0'
        check(out == 'unpacked' and gx.running(15) == v1 and gx.result()[:2] == ('ok', v1),
              'a stick unpacked after svscan, its bridge 12 s to start: the boot window, not the 8 s one (%s)'
              % (gx.result(),), gx)
    finally:
        gx.close()

    # two of our archives on one stick: neither waits at boot step 30 for a bridge that cannot start before step 95
    gx = GX(boot_verify_s=8)
    try:
        out = gx.reboot([pkg, p1])
        r1 = '%s+%d' % (v1, b)
        check(out == 'unpacked' and max(gx.unpack_s) < 5 and gx.running_release() == r1
              and gx.result()[:3] == ('ok', v1, v), 'two archives on one stick: each unpacked in under 5 s at step 30 '
              '(%s s), the second one installed and running' % ', '.join('%.1f' % x for x in gx.unpack_s), gx)
    finally:
        gx.close()

    # a first install that never starts: taken out again, its result and log kept; not retried from the same stick
    gx = GX(boot_verify_s=8)
    try:
        gx.env['SIM_BROKEN'] = v
        gx.env['SIM_DBUS_S'] = '30'
        t0 = time.monotonic()
        gx.reboot(pkg)
        took = time.monotonic() - t0
        r = gx.result()
        left = [p for p in (gx.app, gx.link, gx.svc, gx.rc) if os.path.lexists(p)]
        kept = sorted(os.listdir(gx.work)) if os.path.isdir(gx.work) else None
        check(r and r[0] == 'failed' and r[3].endswith('There was no previous version, so the app was removed again.')
              and not left and kept == ['failed_version', 'install.log', 'result'],
              'a first install that does not start: removed again, result and log kept (%s, left %s, kept %s)'
              % (r, left, kept), gx)
        want = ['--system', '--type=method_call', '--dest=com.victronenergy.platform', '/Notifications/Inject',
                'com.victronenergy.BusItem.SetValue', 'variant:string:2\tXREG-010 GX app\t' + (r[3] if r else '')]
        check(gx.notifications() == [want] and took < 25,
              'a first install that does not start: its reason as one information notification on the GX, a bus that '
              'does not answer holding nothing (%.1f s, %s)' % (took, gx.notifications()), gx)
        out = gx.reboot(pkg)
        check(out == 'skipped' and not os.path.exists(gx.app), 'the same stick at the next boot: not retried', gx)
        gx.env['SIM_BROKEN'] = ''
        out = gx.reboot(p1)
        check(out == 'unpacked' and gx.running() == v1 and len(gx.notifications()) == 1,
              'a newer stick then installs, with no notification', gx)
    finally:
        gx.close()

    # the same with the owner's rc.local not executable and no dbus-send: the install's note reaches the result
    gx = GX(boot_verify_s=8)
    try:
        os.remove(os.path.join(gx.bin, 'dbus-send'))
        owner = '#!/bin/sh\necho owner\n'
        with open(gx.rc, 'w') as f:
            f.write(owner)
        os.chmod(gx.rc, 0o644)
        gx.env['SIM_BROKEN'] = v
        gx.reboot(pkg)
        r = gx.result()
        kept = sorted(os.listdir(gx.work)) if os.path.isdir(gx.work) else None
        check(r and r[0] == 'failed' and 'There was no previous version, so the app was removed again. /data/rc.local '
              'was not executable' in r[3] and gx.read('rc.local') == owner and os.access(gx.rc, os.X_OK)
              and not os.path.exists(gx.app) and kept == ['failed_version', 'install.log', 'result'],
              "a first install that does not start, the owner's rc.local made executable, no dbus-send: the result "
              'says so (%s, kept %s)' % (r, kept), gx)
    finally:
        gx.close()

    # rc.local a link to the owner's file; then rc.local holding only our line
    gx = GX()
    try:
        target = os.path.join(gx.data, 'myscripts', 'rc.local')
        os.makedirs(os.path.dirname(target))
        owner = '#!/bin/sh\necho owner >> "$XREG_DATA/owner.log"\n'
        with open(target, 'w') as f:
            f.write(owner)
        os.chmod(target, 0o755)
        os.symlink(target, gx.rc)
        gx.reboot(pkg)
        with open(target) as f:
            tl = f.read().splitlines()
        check(os.path.islink(gx.rc) and os.readlink(gx.rc) == target and tl[0] == '#!/bin/sh'
              and tl[1].endswith('# xregulator-app') and len(tl) == 3, 'rc.local a link: the link kept, its file rewritten',
              gx)
        gx.remove()
        with open(target) as f:
            check(os.path.islink(gx.rc) and f.read() == owner, "a linked rc.local after Remove: the owner's file back", gx)
    finally:
        gx.close()
    gx = GX()
    try:
        gx.reboot(pkg)
        line = gx.read('rc.local').splitlines()[1]
        with open(gx.rc, 'w') as f:
            f.write(line + '\n')
        rc = gx.in_app(p1)
        check(rc == 0 and gx.result() == ('ok', v1, v, '') and gx.read('rc.local') == '#!/bin/sh\n' + line + '\n',
              'rc.local holding only our line: rewritten with a shebang, no note (%s)' % (gx.result(),), gx)
    finally:
        gx.close()

    # installed with startup scripts disabled, then something makes an rc.local without our line: the same stick repairs
    gx = GX()
    try:
        with open(gx.rc + '.disabled', 'w') as f:
            f.write('#!/bin/sh\necho owner\n')
        gx.reboot(pkg)
        with open(gx.rc, 'w') as f:
            f.write('#!/bin/sh\necho other >> /dev/null\n')
        os.chmod(gx.rc, 0o755)
        gx.reboot()
        down = gx.running(2)
        out = gx.reboot(pkg)
        check(down is None and out == 'unpacked' and gx.running() == v and len(gx.rc_lines()) == 1,
              'our line only in rc.local.disabled while an rc.local exists: the same stick repairs it', gx)
    finally:
        gx.close()

    gx = GX(boot_verify_s=12)
    try:
        gx.reboot(pkg)
        threading.Timer(1.5, gx.set_clock, (86400,)).start()   # NTP sets the clock a day forward during the wait
        gx.env['SIM_BROKEN'] = v1
        t0 = time.monotonic()
        gx.reboot(p1)
        took = time.monotonic() - t0
        gx.env['SIM_BROKEN'] = ''
        check(gx.running() == v and gx.result()[:3] == ('rolledback', v1, v) and took >= 12,
              'the wall clock a day forward while a boot install waits: the wait still runs its 12 s (%.1f s, %s)'
              % (took, gx.result()), gx)
    finally:
        gx.close()

    for name, (body, mode, note) in RC_CASES.items():
        gx = GX()
        try:
            with open(gx.rc, 'wb') as f:
                f.write(body)
            os.chmod(gx.rc, mode)
            gx.reboot(pkg)
            first = gx.result()
            if os.path.exists(os.path.join(gx.data, 'owner.log')):
                os.remove(os.path.join(gx.data, 'owner.log'))
            gx.reboot()
            text = gx.read('rc.local')
            check(gx.running() == v and gx.read('owner.log') == 'owner\n' and '\r' not in text
                  and len(gx.rc_lines()) == 1 and (note in first[3] if note else first[3] == ''),
                  "owner's rc.local, %s: the app and the owner's line run at the next boot, note %r" % (name, first[3]),
                  gx)
            lines = text.splitlines()
            check(lines[1 if lines[0].startswith('#!') else 0].endswith('# xregulator-app'),
                  "owner's rc.local, %s: our line right after the shebang" % name, gx)
            gx.remove()
            want = body.replace(b'\r\n', b'\n').replace(b'\r', b'\n')
            if not want.endswith(b'\n'):
                want += b'\n'
            with open(gx.rc, 'rb') as f:
                check(f.read() == want, "owner's rc.local, %s: Remove leaves the owner's lines" % name, gx)
        finally:
            gx.close()

    gx = GX()
    try:
        old_bench_tree(pkg, gx.app)
        with open(gx.rc, 'w') as f:
            f.write('#!/bin/sh\n[ -d %s ] || cp -a %s/service %s\n' % (gx.svc, gx.app, gx.svc))
        os.chmod(gx.rc, 0o755)
        gx.reboot()
        check(gx.running(2) is None and os.path.isdir(gx.svc), 'bench install from before 0.3 boots (no marker yet)', gx)
        new = os.path.join(gx.work, 'new')
        xreg_update.stage(pkg, new)   # push_to_gx.sh's rsync into the stage
        rc = gx.sh(os.path.join(new, 'install.sh'), 'install', new).returncode
        check(rc == 0 and gx.running() == v and gx.result() == ('ok', v, '0.2', '') and len(gx.rc_lines()) == 1
              and gx.rc_lines()[0].endswith('# xregulator-app'),
              'bench install from before 0.3 upgraded the push_to_gx.sh way: ok, old rc.local line replaced', gx)
    finally:
        gx.close()
    shutil.rmtree(t, ignore_errors=True)


def main():
    check = Checks()
    standard(sys.argv[1], check)
    print('SUMMARY: %d PASS, %d FAIL' % (check.passes, check.fails))
    sys.exit(1 if check.fails else 0)


if __name__ == '__main__':
    main()
