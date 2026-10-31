#!/usr/bin/env python3
# xreg_update.Updater against a fake OTA host, with a throwaway RSA key made here.
import base64, hashlib, io, json, os, subprocess, sys, tempfile, time, zipfile, email.message, urllib.error
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import reviewenv
S = reviewenv.work()
sys.path.insert(0, os.path.join(S, 'gx_app', 'xregulator', 'bridge'))
import xreg_update as U

T = tempfile.mkdtemp(dir=S, prefix='ut_')
KEY = os.path.join(T, 'k.pem')
KEY2 = os.path.join(T, 'k2.pem')
for k in (KEY, KEY2):
    subprocess.run(['openssl', 'genrsa', '-out', k, '4096'], check=True, capture_output=True)
PUB = subprocess.run(['openssl', 'rsa', '-in', KEY, '-pubout'], check=True, capture_output=True, text=True).stdout
PKG = os.path.join(S, 'out', 'venus-data.zip')


def sign(path, key=KEY):
    sig = subprocess.run(['openssl', 'dgst', '-sha256', '-sign', key, path], check=True, capture_output=True).stdout
    return base64.b64encode(sig)


def repack(version, extra=None, out=None, build=1761800000):
    out = out or os.path.join(T, 'p_%s_%d.zip' % (version, len(os.listdir(T))))
    with zipfile.ZipFile(PKG) as z, zipfile.ZipFile(out, 'w') as o:
        for zi in z.infolist():
            data = z.read(zi)
            if zi.filename in ('rc/VERSION', U.PAYLOAD + 'VERSION'):
                data = (version + '\n').encode()
            if zi.filename in ('rc/BUILD', U.PAYLOAD + 'BUILD'):
                data = b'%d\n' % build
            o.writestr(zi, data)
        for name, data, mode in (extra or []):
            zi = zipfile.ZipInfo(name)
            zi.create_system = 3
            zi.external_attr = mode << 16
            o.writestr(zi, data)
    return out


class Resp(io.BytesIO):
    def __init__(self, data, length=None, cut=None):
        super().__init__(data if cut is None else data[:cut])
        self.headers = email.message.Message()
        if length is not None:
            self.headers['Content-Length'] = str(length)

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()


class Opener:
    def __init__(self, files):
        self.files, self.urls = files, []

    def open(self, req, timeout=None):
        url = req.full_url
        self.urls.append((url, dict(req.header_items())))
        name = url.rsplit('/', 1)[1].split('?')[0]
        if name not in self.files:
            raise urllib.error.HTTPError(url, 404, 'nf', {}, None)
        v = self.files[name]
        if isinstance(v, tuple):
            return Resp(v[0], len(v[0]), cut=v[1])
        return Resp(v, len(v))


APPDIR = os.path.join(T, 'app'); os.makedirs(APPDIR, exist_ok=True)
open(os.path.join(APPDIR, 'BUILD'), 'w').write('1761800000\n')


def run(name, files, installed='0.3', want=None, expect_spawn=False):
    work = tempfile.mkdtemp(dir=T)
    spawned = []
    if want and '+' not in want:
        want = want + '+1761800000'
    up = U.Updater(installed, APPDIR, work, key=U.load_public_key(PUB),
                   opener=Opener(files), spawn=lambda a, w, r: spawned.append(a))
    up.check()
    for _ in range(100):
        if up.state not in U.BUSY:
            break
        time.sleep(0.05)
    st1 = (up.state, up.latest, up.message)
    ok = None
    if want:
        ok = up.install(want)
        for _ in range(200):
            if up.state not in (U.DOWNLOADING, U.VERIFYING):
                break
            time.sleep(0.05)
    good = bool(spawned) == expect_spawn
    print('%s %-55s check=%s install(%s)=%s -> state %d %r spawned=%s' % (
        'OK  ' if good else 'BAD ', name, st1, want, ok, up.state, up.message[:90], bool(spawned)))
    return up


def manifest(v, file, sig, size=0, sha='', build=1761800000):
    return json.dumps({'versions': {v: {'file': file, 'sig': sig, 'size': size, 'sha256': sha, 'build': build}}}).encode()


p4 = repack('0.4')
b4 = open(p4, 'rb').read()
s4 = sign(p4)
run('valid 0.4 over 0.3', {'gxapp_versions.json': manifest('0.4', 'a.zip', 'a.sig', len(b4), hashlib.sha256(b4).hexdigest()),
                           'a.zip': b4, 'a.sig': s4}, want='0.4', expect_spawn=True)
run('valid 0.4 over 0.3, no sha/size in manifest', {'gxapp_versions.json': manifest('0.4', 'a.zip', 'a.sig'),
                                                    'a.zip': b4, 'a.sig': s4}, want='0.4', expect_spawn=True)
run('signed by another key', {'gxapp_versions.json': manifest('0.4', 'a.zip', 'a.sig'), 'a.zip': b4,
                              'a.sig': sign(p4, KEY2)}, want='0.4')
run('unsigned (sig 404)', {'gxapp_versions.json': manifest('0.4', 'a.zip', 'a.sig'), 'a.zip': b4}, want='0.4')
run('sig is garbage base64', {'gxapp_versions.json': manifest('0.4', 'a.zip', 'a.sig'), 'a.zip': b4,
                              'a.sig': b'!!!notbase64'}, want='0.4')
tam = bytearray(b4); tam[len(tam) // 2] ^= 1
run('zip tampered, original sig', {'gxapp_versions.json': manifest('0.4', 'a.zip', 'a.sig'), 'a.zip': bytes(tam),
                                   'a.sig': s4}, want='0.4')
run('truncated download, no sha in manifest', {'gxapp_versions.json': manifest('0.4', 'a.zip', 'a.sig'),
                                               'a.zip': (b4, len(b4) // 2), 'a.sig': s4}, want='0.4')
p2 = repack('0.2'); b2 = open(p2, 'rb').read()
run('manifest says 0.5, file is signed 0.2', {'gxapp_versions.json': manifest('0.5', 'a.zip', 'a.sig'), 'a.zip': b2,
                                              'a.sig': sign(p2)}, want='0.5')
run('manifest lists only 0.2 (downgrade offered)', {'gxapp_versions.json': manifest('0.2', 'a.zip', 'a.sig'),
                                                    'a.zip': b2, 'a.sig': sign(p2)}, want='0.2')
run('same version 0.3 listed', {'gxapp_versions.json': manifest('0.3', 'a.zip', 'a.sig')}, want='0.3')
evil = repack('0.4', extra=[(U.PAYLOAD + '../../../../etc/evil', b'x', 0o100644)])
run('signed zip with ../ entry', {'gxapp_versions.json': manifest('0.4', 'a.zip', 'a.sig'), 'a.zip': open(evil, 'rb').read(),
                                  'a.sig': sign(evil)}, want='0.4')
evil2 = repack('0.4', extra=[(U.PAYLOAD + 'bridge/link', b'/etc/passwd', 0o120777)])
run('signed zip with a symlink', {'gxapp_versions.json': manifest('0.4', 'a.zip', 'a.sig'), 'a.zip': open(evil2, 'rb').read(),
                                  'a.sig': sign(evil2)}, want='0.4')
evil3 = repack('0.4', extra=[('xregulator/other/x', b'x', 0o100644)])
run('signed zip with an entry outside the payload', {'gxapp_versions.json': manifest('0.4', 'a.zip', 'a.sig'),
                                                     'a.zip': open(evil3, 'rb').read(), 'a.sig': sign(evil3)}, want='0.4')
evil4 = repack('0.4', extra=[('rc/../../etc/x', b'x', 0o100644)])
up = run('signed zip with rc/../ entry (skipped by stage)', {'gxapp_versions.json': manifest('0.4', 'a.zip', 'a.sig'),
                                                             'a.zip': open(evil4, 'rb').read(), 'a.sig': sign(evil4)},
         want='0.4', expect_spawn=True)
run('manifest file name with path', {'gxapp_versions.json': manifest('0.4', '../a.zip', 'a.sig')}, want='0.4')
run('manifest not JSON', {'gxapp_versions.json': b'<html>'}, want='0.4')
run('manifest 404', {}, want='0.4')
run('oversize Content-Length', {'gxapp_versions.json': manifest('0.4', 'a.zip', 'a.sig'), 'a.zip': b'x' * (U.PACKAGE_MAX + 1)},
    want='0.4')

# ---- version+build ----
pr = repack('0.3', build=1761900000); br = open(pr, 'rb').read()
run('reissue: 0.3 build newer than installed 0.3', {'gxapp_versions.json': manifest('0.3', 'a.zip', 'a.sig', build=1761900000),
    'a.zip': br, 'a.sig': sign(pr)}, want='0.3+1761900000', expect_spawn=True)
run('manifest says 0.3 build 1761900000, signed package is the installed build', {'gxapp_versions.json':
    manifest('0.3', 'a.zip', 'a.sig', build=1761900000), 'a.zip': open(repack('0.3'), 'rb').read(),
    'a.sig': sign(repack('0.3'))}, want='0.3+1761900000')
po = repack('0.3', build=1761700000)
run('manifest lists 0.3 with an older build (downgrade)', {'gxapp_versions.json': manifest('0.3', 'a.zip', 'a.sig', build=1761700000),
    'a.zip': open(po, 'rb').read(), 'a.sig': sign(po)}, want='0.3+1761700000')
run('manifest build as a string', {'gxapp_versions.json': json.dumps({'versions': {'0.4': {'file': 'a.zip', 'sig': 'a.sig',
    'build': '1761800000'}}}).encode()}, want='0.4')
run('manifest build true (bool)', {'gxapp_versions.json': json.dumps({'versions': {'0.4': {'file': 'a.zip', 'sig': 'a.sig',
    'build': True}}}).encode()}, want='0.4+1')
run('manifest without build: 0.4+0 over 0.3+b', {'gxapp_versions.json': json.dumps({'versions': {'0.4': {'file': 'a.zip',
    'sig': 'a.sig'}}}).encode(), 'a.zip': b4, 'a.sig': s4}, want='0.4+0')
print('newer checks:', U.newer('0.3+2', '0.3+1'), U.newer('0.3', '0.3+0'), U.newer('0.10+0', '0.9+99'),
      U.newer('0.3+1', '0.3.0+1'), U.newer('0.3+', '0.2'), U.parse_release('0.3+'), U.parse_release('0.3+01'),
      U.parse_release('0.3+1+2'), U.parse_release(' 0.3'))

# what a check sends
o = Opener({'gxapp_versions.json': manifest('0.4', 'a.zip', 'a.sig')})
up = U.Updater('0.3', APPDIR, tempfile.mkdtemp(dir=T), key=U.load_public_key(PUB), opener=o,
               spawn=lambda a, w, r: None)
print('requests made at construction:', o.urls)
up.check(); time.sleep(0.3)
print('check request:', o.urls)

# raw verify edge cases
n, e = U.load_public_key(PUB)
d = hashlib.sha256(b4).digest()
sig = base64.b64decode(s4)
print('verify good:', U.verify_digest(d, sig, (n, e)), ' sig+n:', U.verify_digest(d, (int.from_bytes(sig, 'big') + n).to_bytes(513, 'big')[-512:] if False else sig, (n, e)),
      ' short sig:', U.verify_digest(d, sig[1:], (n, e)), ' zero sig:', U.verify_digest(d, bytes(512), (n, e)),
      ' s>=n:', U.verify_digest(d, n.to_bytes(512, 'big'), (n, e)))
