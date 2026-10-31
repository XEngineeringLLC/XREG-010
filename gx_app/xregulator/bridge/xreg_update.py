#!/usr/bin/env python3
# GX app updates from inside the app. The release list (gxapp_versions.json on X Engineering's OTA host, written by
# gx_app/publish_gx.sh) is read only when the owner opens the update page or taps Check, with a plain GET that carries
# nothing about this GX. The package is the same zip an owner copies to a USB stick as venus-data.zip
# (gx_app/build_package.py): downloaded, its RSA signature checked against the firmware's OTA key, its app tree staged,
# then the app's install.sh run in a session of its own, so the bridge restart it does cannot stop it. install.sh keeps
# the previous version and puts it back if the new bridge does not come up; it reports through files in its workspace.
# A release is "<version>+<build>" (VERSION and BUILD in the app tree; a tree without BUILD is build 0), ordered by
# version field by field, then build, so a version number issued again with a newer build reaches every GX.

import base64, hashlib, hmac, json, logging, os, re, secrets, shutil, ssl, stat, subprocess, tempfile, threading, time
import urllib.error, urllib.request, zipfile

log = logging.getLogger('xregulator')

OTA_BASE = 'https://ota.xengineering.net/'
MANIFEST = 'gxapp_versions.json'
PAYLOAD = 'xregulator/install/new/'   # the app tree's place in a package: venus-data.zip unpacks into /data
NAME_RE = re.compile(r'[A-Za-z0-9._-]{1,100}\Z')   # the OTA host serves one path element of these characters
VERSION_RE = re.compile(r'[0-9]{1,4}(\.[0-9]{1,4}){0,3}\Z')
BUILD_RE = re.compile(r'(0|[1-9][0-9]{0,11})\Z')   # install.sh's build_of takes exactly these
SHA_RE = re.compile(r'[0-9a-f]{64}\Z')
RUN_ID_RE = re.compile(r'[0-9a-f]{8}\Z')
MANIFEST_MAX = 65536
SIG_MAX = 4096
PACKAGE_MAX = 16 << 20
UNPACKED_MAX = 64 << 20
TIMEOUT_S = 20.0     # per read
DOWNLOAD_S = 300.0   # a whole download
POLL_S = 2.0
SPAWN_STALE_S = 900.0   # an install.sh this bridge started that has neither run nor left a result by then died

IDLE, CHECKING, CURRENT, AVAILABLE, DOWNLOADING, VERIFYING, INSTALLING, FAILED, REMOVING = range(9)
BUSY = (CHECKING, DOWNLOADING, VERIFYING, INSTALLING, REMOVING)

# The firmware's OTA_PUBLIC_KEY (Xregulator.ino): one key pair signs firmware and GX app packages
OTA_PUBLIC_KEY = '''-----BEGIN PUBLIC KEY-----
MIICIjANBgkqhkiG9w0BAQEFAAOCAg8AMIICCgKCAgEAp2sRgMjD4wazKHo6Rk3g
7APj7xsYGimOUKhWGWvd8wxFOEL+bHatAeKyCZXBjrZI9rdiUGtyHDdXS246YvV6
dr56hJFT05n9iMHfx5T99NOyBl8Nm18dijqLGDSgVD9rC8v4aMrx5lSMrgzfz+bh
HaDny6BFBHvaCQZW2KxXdnZElgnRidsoSR+5twdwdv9biHEwm4Miss1QQjmVZPVg
SPIj4j9pf6wf8+kr57nEtjQORu+lLVj3OkM+A94N4k78+tR3gc3UMr1GvrqXm9D6
VBH4OZDEgnx+QzraU9O60c6Jp/YXJkeLjRGFS7AaTd5+8sEFHoyx4BhvvxyUC7tX
SpIZqziIlRjdJFE+8PjJ76u2BD1JmGeboUU+sxLJFiMpNeBskJG0kVwkFobkUtpN
7O57CaGuzA8Eih+x3jTO85CiqgsK7rRckFyvMin1AgIvvxN9CUY+jTYvfcRdUO5Y
7xQ8bFCvNUnpmqlWRveq3cxNZpNjOf99NinLJ6ewuaBqYXTtYbNCeAO+l0IiEl74
c0HALRefCJO9mHmSoHsYJ7RRTLKhNO7jvoo7B+qCb2JDUKhwaLEetIDF6nw0dH22
hONJuBOYktuwhrrsupJyBWYHHFmnFhqBcXCuTiPDhpcW05/3oAwApS7SpN8bN7dp
SXQcqfsev1ke5IRosYpCv6cCAwEAAQ==
-----END PUBLIC KEY-----
'''
RSA_OID = bytes.fromhex('2a864886f70d010101')
SHA256_INFO = bytes.fromhex('3031300d060960864801650304020105000420')   # DER DigestInfo before the 32-byte hash


class UpdateError(Exception):
    pass


# ---- versions and releases ----

def _read_first(path):
    try:
        with open(path, encoding='ascii', errors='replace') as f:
            return f.readline().strip()
    except OSError:
        return ''


def app_version(app_dir):
    # The app tree's VERSION file, the one number build_plugin.sh, build_package.py and install.sh read too
    v = _read_first(os.path.join(app_dir, 'VERSION'))
    return v if VERSION_RE.match(v) else '0'


def build_text(raw):
    # a BUILD file's build: its first line without the newline, as install.sh reads it with head -n 1; None if invalid
    try:
        b = raw.split(b'\n', 1)[0].decode('ascii')
    except UnicodeDecodeError:
        return None
    return int(b) if BUILD_RE.match(b) else None


def app_build(app_dir):
    # 0 for a tree without BUILD (push_to_gx.sh copies the source tree); None for a BUILD that is not a build
    try:
        with open(os.path.join(app_dir, 'BUILD'), 'rb') as f:
            return build_text(f.read())
    except FileNotFoundError:
        return 0
    except OSError:
        return None


def release(version, build):
    return '%s+%d' % (version, build)


def parse_release(r):
    # (version, build) from "0.3+1761790000", or a bare "0.3" (build 0); None when neither
    v, plus, b = str(r).partition('+')
    if not VERSION_RE.match(v) or (plus and not BUILD_RE.match(b)):
        return None
    return v, int(b or 0)


def version_key(v):
    return tuple(int(x) for x in str(v).split('.')) if VERSION_RE.match(str(v)) else None


def release_key(r):
    p = parse_release(r)
    if p is None:
        return None
    k = version_key(p[0])
    return k + (0,) * (4 - len(k)), p[1]


def newer(a, b):
    # release (or bare version) a newer than b: version field by field (0.10 above 0.9), then build
    ka, kb = release_key(a), release_key(b)
    if ka is None:
        return False
    return kb is None or ka > kb


# ---- RSA PKCS#1 v1.5 SHA-256 verify, as openssl dgst -sha256 -sign makes it ----

def _der(b, i):
    # (tag, start, end) of the DER element at i
    tag, n = b[i], b[i + 1]
    i += 2
    if n & 0x80:
        k = n & 0x7f
        n = int.from_bytes(b[i:i + k], 'big')
        i += k
    if i + n > len(b):
        raise ValueError('DER element runs past the end')
    return tag, i, i + n


def load_public_key(pem):
    # (n, e) from a PEM SubjectPublicKeyInfo holding an RSA key
    der = base64.b64decode(''.join(l.strip() for l in pem.strip().splitlines() if not l.startswith('-----')))
    tag, s, _ = _der(der, 0)
    tag_alg, a, a_end = _der(der, s)
    tag_oid, o, o_end = _der(der, a)
    tag_bits, b, b_end = _der(der, a_end)
    if (tag, tag_alg, tag_oid, tag_bits) != (0x30, 0x30, 0x06, 0x03) or der[o:o_end] != RSA_OID or der[b] != 0:
        raise ValueError('not an RSA public key')
    tag, r, _ = _der(der, b + 1)
    tag_n, n0, n1 = _der(der, r)
    tag_e, e0, e1 = _der(der, n1)
    if (tag, tag_n, tag_e) != (0x30, 0x02, 0x02):
        raise ValueError('not an RSA public key')
    return int.from_bytes(der[n0:n1], 'big'), int.from_bytes(der[e0:e1], 'big')


def verify_digest(digest, sig, key):
    n, e = key
    k = (n.bit_length() + 7) // 8
    if len(digest) != 32 or len(sig) != k:
        return False
    s = int.from_bytes(sig, 'big')
    if s >= n:
        return False
    want = b'\x00\x01' + b'\xff' * (k - 3 - len(SHA256_INFO) - 32) + b'\x00' + SHA256_INFO + digest
    return hmac.compare_digest(pow(s, e, n).to_bytes(k, 'big'), want)


# ---- release list and package ----

def parse_manifest(body):
    # (newest release, its entry) from gxapp_versions.json: {"versions": {"0.3": {"build", "file", "sig", "size",
    # "sha256", ...}}}; a version issued again replaces its entry with the new build and new file names
    try:
        m = json.loads(body.decode('utf-8'))
    except (UnicodeDecodeError, ValueError):
        raise UpdateError('The release list from ota.xengineering.net is not readable.')
    vs = m.get('versions') if isinstance(m, dict) else None
    if not isinstance(vs, dict):
        raise UpdateError('The release list from ota.xengineering.net is not readable.')
    good = {}
    for v, e in vs.items():
        if not (isinstance(v, str) and VERSION_RE.match(v) and isinstance(e, dict)):
            continue
        f, s, size, sha, build = e.get('file'), e.get('sig'), e.get('size'), e.get('sha256'), e.get('build', 0)
        if not (isinstance(f, str) and NAME_RE.match(f) and isinstance(s, str) and NAME_RE.match(s)):
            continue
        if not isinstance(build, int) or isinstance(build, bool) or not 0 <= build < 10 ** 12:
            continue
        if isinstance(size, int) and not isinstance(size, bool) and size > PACKAGE_MAX:
            continue
        good[release(v, build)] = {'version': v, 'build': build, 'file': f, 'sig': s,
                                   'size': size if isinstance(size, int) and not isinstance(size, bool) and size > 0 else 0,
                                   'sha256': sha if isinstance(sha, str) and SHA_RE.match(sha) else ''}
    if not good:
        raise UpdateError('ota.xengineering.net lists no GX app release.')
    latest = max(good, key=release_key)
    return latest, good[latest]


def package_release(z):
    # the release a package carries in its app tree, '' when it carries none
    try:
        v = z.read(PAYLOAD + 'VERSION').decode('ascii').strip()
    except (KeyError, UnicodeDecodeError):
        return ''
    try:
        b = build_text(z.read(PAYLOAD + 'BUILD'))
    except KeyError:
        b = 0
    return release(v, b) if VERSION_RE.match(v) and b is not None else ''


def stage(zpath, dest):
    # The package's app tree into dest, modes from the zip (busybox unzip on the stick path does the same); its release
    try:
        z = zipfile.ZipFile(zpath)
    except (zipfile.BadZipFile, OSError):
        raise UpdateError('The download is not a GX app package. Nothing was installed.')
    with z:
        rel = package_release(z)
        if not rel:
            raise UpdateError('The download is not a GX app package. Nothing was installed.')
        if sum(i.file_size for i in z.infolist()) > UNPACKED_MAX:
            raise UpdateError('The package is larger than a GX app package can be. Nothing was installed.')
        shutil.rmtree(dest, ignore_errors=True)
        os.makedirs(dest, 0o755)
        for info in z.infolist():
            name = info.filename
            if name.startswith('rc/') or (name.endswith('/') and PAYLOAD.startswith(name)):
                continue
            parts = name[len(PAYLOAD):].rstrip('/').split('/')
            mode = info.external_attr >> 16
            if not name.startswith(PAYLOAD) or '\\' in name or any(p in ('', '.', '..') for p in parts) \
                    or stat.S_ISLNK(mode):
                raise UpdateError('The package holds an unexpected entry (%s). Nothing was installed.' % name[:60])
            target = os.path.join(dest, *parts)
            if name.endswith('/'):
                os.makedirs(target, 0o755, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(target), 0o755, exist_ok=True)
            with z.open(info) as src, open(target, 'wb') as out:
                shutil.copyfileobj(src, out)
            os.chmod(target, 0o755 if mode & 0o111 else 0o644)
    return rel


def boot_id():
    v = os.environ.get('XREG_BOOT_ID')
    if v is not None:
        return v
    try:
        with open('/proc/sys/kernel/random/boot_id') as f:
            return f.read().strip()
    except OSError:
        return ''


def owner_live(work, name):
    # install.sh's lock (lock/owner) and a confirm waiting without it (confirming) name a pid and a boot id; one from an
    # earlier boot, or whose process is gone, is stale
    try:
        with open(os.path.join(work, name)) as f:
            pid, _, bid = f.readline().strip().partition(' ')
        if bid != boot_id():
            return False
        os.kill(int(pid), 0)
        return True
    except (OSError, ValueError):
        return False


def mark_running(work, rel):
    # "<release> <pid> <boot id>": install.sh keeps a new release only once its bridge has written this
    try:
        os.makedirs(work, exist_ok=True)
        tmp = os.path.join(work, 'running.tmp')
        with open(tmp, 'w') as f:
            f.write('%s %d %s\n' % (rel, os.getpid(), boot_id()))
        os.replace(tmp, os.path.join(work, 'running'))
    except OSError as e:
        log.warning('install marker: %s', e)
    return False   # GLib.idle_add: once


def read_result(work):
    # The last install's outcome, "<ok|rolledback|failed> <to> <from or -> <epoch> <run id> <note>" (install.sh, or this
    # bridge for a download it refused), as the dict /Update/Last carries. 0.3's install.sh wrote no run id.
    try:
        with open(os.path.join(work, 'result'), encoding='utf-8', errors='replace') as f:
            parts = f.readline().strip().split(' ', 5)
    except OSError:
        return None
    if len(parts) < 4 or parts[0] not in ('ok', 'rolledback', 'failed') or not BUILD_RE.match(parts[3]):
        return None
    run_id = parts[4] if len(parts) > 4 and (parts[4] == '-' or RUN_ID_RE.match(parts[4])) else None
    note = ' '.join(parts[5:] if run_id is not None else parts[4:])
    to, frm = parse_release(parts[1]), parse_release(parts[2])
    return {'result': parts[0], 'to': to[0] if to else parts[1], 'toBuild': to[1] if to else 0,
            'from': frm[0] if frm else '', 'fromBuild': frm[1] if frm else 0, 'at': int(parts[3]),
            'id': run_id or '-', 'note': note}


def write_result(work, kind, to, frm, run_id, note):
    try:
        os.makedirs(work, exist_ok=True)
        tmp = os.path.join(work, 'result.new')
        with open(tmp, 'w') as f:
            f.write('%s %s %s %d %s %s\n' % (kind, to or 'unknown', frm or '-', int(time.time()), run_id, note))
        os.replace(tmp, os.path.join(work, 'result'))
    except OSError as e:
        log.warning('install result: %s', e)


def _spawn(args, work, run_id):
    # A session of its own: daemontools signals the bridge's pid when install.sh restarts it, never this child
    os.makedirs(work, exist_ok=True)
    with open(os.path.join(work, 'install.log'), 'ab') as err:
        subprocess.Popen(args, cwd='/', stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=err,
                         start_new_session=True, close_fds=True, env=dict(os.environ, XREG_RUN_ID=run_id))


def _error_text(e):
    if isinstance(e, urllib.error.HTTPError):
        return 'ota.xengineering.net answered HTTP %d.' % e.code
    reason = getattr(e, 'reason', None) or e
    return 'No answer from ota.xengineering.net (%s).' % (str(reason)[:80] or type(e).__name__)


def _opener():
    # no proxy from the environment, as xreg_cloud does; the default certificate check
    return urllib.request.build_opener(urllib.request.ProxyHandler({}),
                                       urllib.request.HTTPSHandler(context=ssl.create_default_context()))


# ---- the updater ----

class Updater:
    # /Update/* on com.victronenergy.xregulator. State: 0 idle, 1 checking, 2 up to date, 3 update available,
    # 4 downloading (/Update/Progress %), 5 checking the signature, 6 installing (this bridge restarts), 7 failed
    # (/Update/Message), 8 removing. /Update/Last: the last install's outcome as JSON, '' for none; a refused download is
    # written there too, so it stays until a later install replaces it, while a failed check lasts until the next check.
    def __init__(self, version, app_dir, work, base=OTA_BASE, key=None, opener=None, spawn=None,
                 clock=time.monotonic, wall=time.time):
        self.version, self.app_dir, self.work, self.base = version, app_dir, work, base
        build = app_build(app_dir)
        self.build = build or 0
        self.rid = release(version, build) if build is not None and version != '0' else ''   # '' offers any release
        self.key = key or load_public_key(OTA_PUBLIC_KEY)
        self.opener = opener or _opener()
        self.spawn = spawn or _spawn
        self.clock, self.wall = clock, wall
        self.lock = threading.Lock()
        self.state, self.latest, self.entry, self.progress, self.message = IDLE, '', None, 0, ''
        self.run_id = None      # the id of the install.sh this bridge started, echoed in its result
        self.spawned = None     # clock() when it was started
        self.next_poll = 0.0
        self.result_mtime = None
        self.result = None
        self.last = ''
        self.poll()

    def static_paths(self):
        return [('/Update/Installed', self.version, None), ('/Update/InstalledBuild', self.build, None),
                ('/Update/Latest', '', None), ('/Update/LatestBuild', 0, None), ('/Update/State', IDLE, None),
                ('/Update/Progress', 0, None), ('/Update/Size', 0, None), ('/Update/Message', '', None),
                ('/Update/Last', self.last, None), ('/Update/Check', None, 'core'), ('/Update/Install', None, 'core'),
                ('/Update/Remove', None, 'core')]

    def values(self):
        self.poll()
        with self.lock:
            e = self.entry or {}
            return {'/Update/Installed': self.version, '/Update/InstalledBuild': self.build,
                    '/Update/Latest': e.get('version', ''), '/Update/LatestBuild': e.get('build', 0),
                    '/Update/State': self.state, '/Update/Progress': self.progress, '/Update/Size': e.get('size', 0),
                    '/Update/Message': self.message, '/Update/Last': self.last}

    def on_write(self, path, value):
        if path == '/Update/Check':
            return self.check()
        if path == '/Update/Install':
            return self.install(str(value or ''))
        if path == '/Update/Remove':
            return str(value) == '1' and self.remove()
        return False

    # -- owner actions --

    def check(self):
        with self.lock:
            if self.state in BUSY:
                return False
            self.state, self.message = CHECKING, ''
        self._run(self._check, False)
        return True

    def install(self, rel):
        # only the newest release listed, named as the page showed it ("<version>+<build>"), and only when newer than
        # this one; the package's own VERSION and BUILD are checked again after its signature, so a release list
        # pointing at an older signed package installs nothing
        with self.lock:
            if self.state in BUSY or not self.entry or rel != self.latest or not newer(rel, self.rid):
                return False
            self.state, self.progress, self.message = DOWNLOADING, 0, ''
            self.run_id = secrets.token_hex(4)
            entry, run_id = dict(self.entry), self.run_id
        self._run(lambda: self._install(rel, entry, run_id), True)
        return True

    def remove(self):
        with self.lock:
            if self.state in BUSY:
                return False
            self.state, self.message = REMOVING, ''
            self.spawned = self.clock()
        self._run(self._remove, False)
        return True

    # -- jobs (their own thread) --

    def _run(self, fn, installing):
        threading.Thread(target=self._guard, args=(fn, installing), name='update', daemon=True).start()

    def _guard(self, fn, installing):
        try:
            fn()
        except UpdateError as e:
            self._fail(str(e), installing)
        except (urllib.error.URLError, OSError, ValueError) as e:
            self._fail(_error_text(e) if isinstance(e, (urllib.error.URLError, TimeoutError, ConnectionError))
                       else 'Update stopped: %s' % str(e)[:100], installing)
        except Exception as e:
            log.exception('update')
            self._fail('Update stopped: %s' % str(e)[:100], installing)

    def _fail(self, message, installing):
        log.warning('GX app update: %s', message)
        shutil.rmtree(os.path.join(self.work, 'download'), ignore_errors=True)
        if installing:
            write_result(self.work, 'failed', self.latest, self.rid, self.run_id or '-', message)
            self.next_poll = 0.0
        with self.lock:
            self.state, self.message, self.progress = FAILED, message, 0
            self.run_id = None

    def _fetch(self, name, limit, out=None, hasher=None, total=0, deadline=None):
        with self.opener.open(urllib.request.Request(self.base + name), timeout=TIMEOUT_S) as r:
            length = r.headers.get('Content-Length')
            if length and length.isdigit() and int(length) > limit:
                raise UpdateError('%s is larger than expected. Nothing was installed.' % name.split('?')[0])
            total = total or (int(length) if length and length.isdigit() else 0)
            got, chunks = 0, []
            read = getattr(r, 'read1', None) or r.read   # one socket read at a time, so the deadline is seen
            while True:
                if deadline is not None and self.clock() > deadline:
                    raise UpdateError('The download took longer than %d s. Nothing was installed.' % DOWNLOAD_S)
                b = read(65536)
                if not b:
                    break
                got += len(b)
                if got > limit:
                    raise UpdateError('%s is larger than expected. Nothing was installed.' % name.split('?')[0])
                if hasher:
                    hasher.update(b)
                if out:
                    out.write(b)
                    if total:
                        with self.lock:
                            self.progress = min(99, got * 100 // total)
                else:
                    chunks.append(b)
            return b''.join(chunks)

    def _check(self):
        # a query no cache between here and the OTA host has seen; the host serves the file without it
        latest, entry = parse_manifest(self._fetch(MANIFEST + '?r=' + secrets.token_hex(4), MANIFEST_MAX))
        with self.lock:
            self.latest, self.entry = latest, entry
            self.state = AVAILABLE if newer(latest, self.rid) else CURRENT
        log.info('GX app update check: %s installed, %s listed', self.rid, latest)

    def _install(self, rel, entry, run_id):
        deadline = self.clock() + DOWNLOAD_S
        dl = os.path.join(self.work, 'download')
        shutil.rmtree(dl, ignore_errors=True)
        os.makedirs(dl, 0o755)
        path = os.path.join(dl, entry['file'])
        h = hashlib.sha256()
        with open(path, 'wb') as out:
            self._fetch(entry['file'], PACKAGE_MAX, out=out, hasher=h, total=entry['size'], deadline=deadline)
        if entry['sha256'] and h.hexdigest() != entry['sha256']:
            raise UpdateError('The download is damaged. Nothing was installed.')
        with self.lock:
            self.state, self.progress = VERIFYING, 100
        try:
            sig = base64.b64decode(self._fetch(entry['sig'], SIG_MAX, deadline=deadline).strip(), validate=True)
        except ValueError:
            sig = b''
        if not verify_digest(h.digest(), sig, self.key):
            raise UpdateError('The package signature does not match. Nothing was installed.')
        with zipfile.ZipFile(path) as z:
            got = package_release(z)
        if got != rel:
            raise UpdateError('The package is release %s, not %s. Nothing was installed.' % (got or 'unknown', rel))
        if not newer(got, self.rid):
            raise UpdateError('Release %s is not newer than %s. Nothing was installed.' % (got, self.rid))
        new = os.path.join(self.work, 'new')
        stage(path, new)
        shutil.rmtree(dl, ignore_errors=True)
        with self.lock:
            self.state, self.spawned = INSTALLING, self.clock()
        log.info('GX app update: installing %s over %s', rel, self.rid)
        self.spawn(['sh', os.path.join(new, 'install.sh'), 'install', new, '--upgrade-only'], self.work, run_id)

    def _remove(self):
        # from a copy outside the app folder, which install.sh deletes; a name of its own, as root writes it
        fd, copy = tempfile.mkstemp(prefix='xregulator-remove-', suffix='.sh')
        with os.fdopen(fd, 'wb') as out, open(os.path.join(self.app_dir, 'install.sh'), 'rb') as src:
            shutil.copyfileobj(src, out)
        log.info('GX app: removing')
        self.spawn(['sh', copy, 'remove'], self.work, '-')

    # -- install.sh's files --

    def poll(self):
        now = self.clock()
        if now < self.next_poll:
            return
        self.next_poll = now + POLL_S
        try:
            mtime = os.stat(os.path.join(self.work, 'result')).st_mtime
        except OSError:
            mtime = None
        # an install runs while install.sh holds its lock, or waits for the new bridge, with a release to confirm
        running = os.path.exists(os.path.join(self.work, 'pending')) and (
            owner_live(self.work, os.path.join('lock', 'owner')) or owner_live(self.work, 'confirming'))
        with self.lock:
            if mtime != self.result_mtime:
                self.result_mtime = mtime
                self.result = read_result(self.work)
                self.last = json.dumps({k: v for k, v in self.result.items() if k != 'id'}, separators=(',', ':'),
                                       sort_keys=True) if self.result else ''
            if self.state in (DOWNLOADING, VERIFYING, CHECKING):
                return
            stale = self.spawned is not None and now - self.spawned > SPAWN_STALE_S
            if self.state == REMOVING:
                if stale:
                    self.state, self.message, self.spawned = FAILED, 'The app was not removed.', None
                return
            if running:
                self.state = INSTALLING
            elif self.state == INSTALLING:
                res = self.result
                if self.run_id and res and res['id'] == self.run_id:
                    # install.sh ended without restarting this bridge: it refused the package or rolled it back
                    self.state = IDLE if res['result'] == 'ok' else FAILED
                    self.message = '' if res['result'] == 'ok' else res['note'] or 'The update did not install.'
                elif self.run_id and stale:
                    self.state, self.message = FAILED, 'The installer stopped without a result.'
                elif not self.run_id:
                    self.state = IDLE   # an install this bridge did not start (a stick, a push) has ended
                if self.state != INSTALLING:
                    self.run_id, self.spawned = None, None
